#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/photo_storage.h"
#include "common/display_stall_guard.h"
#include "display.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"
#include "boards/zectrix-s3-epaper-4.2/rtc_pcf8563.h"

#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_rtc_time.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <sys/time.h>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr char kWifiEnabledKey[] = "wifi_enabled";
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
constexpr int kSettingsSlideshowIndex = 3;
constexpr int kSettingsNetworkSyncIndex = 5;
constexpr int kSettingsWifiIndex = 6;
constexpr int kSettingsHttpServerIndex = 7;
constexpr int kSettingsLanIpIndex = 8;
constexpr int kSettingsRestartIndex = 1;
constexpr uint32_t kRetainedMagic = 0x4E344354;  // "N4CT" (layout v5, RTC_NOINIT)
constexpr int64_t kInteractiveAwakeUs = 120LL * 1000 * 1000;
constexpr int64_t kNetworkSyncTimeoutUs = 35LL * 1000 * 1000;
// On battery, a local service (AP transfer, LAN server, Wi-Fi config hotspot)
// with no HTTP request and no key press for this long is stopped so the
// device can sleep again. Each of them blocks idle sleep, and an accidental
// one-second BOOT press on the gallery (which opens AP transfer) used to keep
// the radio and CPU running until the battery was empty.
constexpr int64_t kLocalServiceIdleTimeoutUs = 10LL * 60 * 1000 * 1000;
constexpr int64_t kRestartConfirmWindowUs = 8LL * 1000 * 1000;
// A requested restart waits for the panel to settle, but never longer than
// this: the boot that follows repaints everything anyway.
constexpr int64_t kRestartDisplayWaitUs = 30LL * 1000 * 1000;
// Battery protection. ReadBatteryStatus() maps about 3.47 V to 3 %. Below
// that every further slideshow waveform risks a brownout halfway through, and
// a fullscreen photo has no status bar to say why the frame stopped changing.
// Two consecutive readings, so one noisy sample cannot stop the slideshow;
// 10 % (or a charger) to resume.
constexpr int kCriticalBatteryPercent = 3;
constexpr int kBatteryResumePercent = 10;
constexpr uint8_t kCriticalBatteryStrikes = 2;

constexpr int SchedulePeriodSeconds(int interval_minutes) {
    return interval_minutes > 0 ? interval_minutes * 60 : 0;
}

int64_t NowRtcUs() {
    return static_cast<int64_t>(esp_rtc_get_time_us());
}

// Seconds remaining until an absolute deadline, clamped at 0. Returns -1 for
// "no deadline" so callers can tell "disabled" from "already due".
constexpr int SecondsUntilDeadline(uint64_t due_rtc_us, int64_t now_rtc_us) {
    if (due_rtc_us == 0) return -1;
    const int64_t remaining_us = static_cast<int64_t>(due_rtc_us) - now_rtc_us;
    if (remaining_us <= 0) return 0;
    return static_cast<int>((remaining_us + 999999) / 1000000);
}

// What an absolute deadline buys over the countdown it replaced, stated as
// cases. The countdown was decremented by the *planned* sleep length at commit
// time, so an early GPIO wake credited sleep that never happened, and time
// spent awake was credited to nothing at all.
static_assert(SecondsUntilDeadline(0, 1000) == -1, "0 means not scheduled");
static_assert(SecondsUntilDeadline(300000000, 0) == 300, "freshly armed");
static_assert(SecondsUntilDeadline(300000000, 30000000) == 270,
              "a button wake 30 s in leaves 270 s, not a consumed period");
static_assert(SecondsUntilDeadline(300000000, 150000000) == 150,
              "120 s of interactive awake time is charged like any other");
static_assert(SecondsUntilDeadline(300000000, 300000000) == 0, "due");
static_assert(SecondsUntilDeadline(300000000, 900000000) == 0,
              "overslept still reads due exactly once");

// A deadline cannot legitimately be further out than one whole period; that
// only happens when the RTC timebase restarts under a retained schedule.
constexpr bool ScheduleDeadlineIsImplausible(int remaining_seconds,
                                             int period_seconds) {
    return period_seconds > 0 &&
           (remaining_seconds < 0 || remaining_seconds > period_seconds);
}

static_assert(!ScheduleDeadlineIsImplausible(0, 300));
static_assert(!ScheduleDeadlineIsImplausible(300, 300));
static_assert(ScheduleDeadlineIsImplausible(301, 300), "timebase moved");
static_assert(ScheduleDeadlineIsImplausible(-1, 300), "interval on, never armed");
static_assert(!ScheduleDeadlineIsImplausible(-1, 0), "interval off");

int SecondsUntilRtcDeadline(uint64_t due_rtc_us) {
    return SecondsUntilDeadline(due_rtc_us, NowRtcUs());
}

// remaining_seconds is the SecondsUntilRtcDeadline() result: -1 unscheduled,
// 0 due, >0 pending.
constexpr bool ShouldRunScheduledNetworkSync(bool wifi_enabled,
                                             int interval_minutes,
                                             int remaining_seconds) {
    return wifi_enabled && interval_minutes > 0 && remaining_seconds == 0;
}

// The periodic setting is only a schedule. It must never override the user's
// persisted Wi-Fi switch, including on a timer wake with an already-due sync.
static_assert(!ShouldRunScheduledNetworkSync(false, 10, 0));
static_assert(!ShouldRunScheduledNetworkSync(true, 0, 0));
static_assert(!ShouldRunScheduledNetworkSync(true, 10, 1));
static_assert(!ShouldRunScheduledNetworkSync(true, 10, -1));
static_assert(ShouldRunScheduledNetworkSync(true, 10, 0));

constexpr bool ShouldEnterIdleSleep(bool idle_deadline_reached,
                                    bool external_power_present) {
    return idle_deadline_reached && !external_power_present;
}

static_assert(!ShouldEnterIdleSleep(true, true));
static_assert(ShouldEnterIdleSleep(true, false));
static_assert(!ShouldEnterIdleSleep(false, false));
static_assert(BOOT_BUTTON_GPIO <= GPIO_NUM_21);
static_assert(TODO_DOWN_BUTTON_GPIO <= GPIO_NUM_21);
static_assert(CHARGE_DETECT_GPIO <= GPIO_NUM_21);
static_assert(CHARGE_FULL_GPIO <= GPIO_NUM_21);
static_assert(TODO_UP_BUTTON_GPIO > GPIO_NUM_21,
              "UP is not an RTC IO on ESP32-S3 and cannot wake deep sleep");

// Absolute deadlines in the RTC microsecond counter, which keeps running
// through deep sleep and resets only on power-on. Storing "when", rather than
// "how many seconds are left", is what makes one value correct for every way a
// cycle can end: an early GPIO wake no longer credits the full planned sleep,
// and a long awake session no longer credits nothing at all.
//
// 0 means "not scheduled" (interval disabled).
//
// Both records below are RTC_NOINIT, so they survive every reset that keeps
// the RTC domain powered: deep sleep, but also esp_restart(), panics,
// watchdogs and brownouts. They used to be RTC_DATA_ATTR, which the
// bootloader reloads from flash on every reset except a deep-sleep wake. The
// stall guard's esp_restart() therefore wiped exactly the state its recovery
// path was written to restore, any crash sent the frame back to photo 1 in
// memory-card mode, and the diagnostics that should have explained a field
// failure were zeroed by it.
//
// RTC_NOINIT storage must not have a C++ initializer: GCC emits a startup
// constructor for any type with default member initializers, and it would
// overwrite the retained copy on every boot. Hence the trivially
// constructible layouts, the display metadata carried as bytes, and a
// checksum that tells a valid record from power-on garbage or a torn update.
struct RetainedSchedule {
    uint32_t magic;
    uint32_t size;
    int32_t slideshow_config_minutes;
    int32_t sync_config_minutes;
    uint64_t slideshow_due_rtc_us;
    uint64_t sync_due_rtc_us;
    int32_t selected_photo_index;
    int32_t gallery_fullscreen;
    uint8_t display_metadata[sizeof(ui::PersistentDisplayMetadata)];
    // Consecutive wakes that measured a critical battery on battery power,
    // and whether the "please charge" notice is the frame on glass.
    uint8_t low_battery_strikes;
    uint8_t low_battery_notice_shown;
    uint8_t reserved[2];
    uint32_t checksum;
};
static_assert(std::is_trivially_default_constructible_v<RetainedSchedule>,
              "RTC_NOINIT storage must not get a startup constructor");
static_assert(std::is_trivially_copyable_v<ui::PersistentDisplayMetadata>,
              "display metadata is retained with memcpy");

constexpr int kBootHistoryLength = 12;
enum BootRecordFlag : uint8_t {
    kBootFlagFrameRefreshed = 1u << 0,
    kBootFlagNetworkSession = 1u << 1,
    kBootFlagSleepCommitted = 1u << 2,
    kBootFlagPromoted = 1u << 3,
    kBootFlagQuietRecovery = 1u << 4,
    kBootFlagServicesTimedOut = 1u << 5,
    kBootFlagRestartRequested = 1u << 6,
};

// One entry per boot, completed when that boot commits deep sleep. A wake
// that never reaches the commit (crash, watchdog, brownout, stall restart)
// keeps awake_s == 0, which is what a field failure looks like afterwards.
struct BootRecord {
    uint32_t rtc_s;
    uint16_t awake_s;
    uint16_t wake_causes;
    uint8_t reset_reason;
    uint8_t boot_kind;
    uint8_t battery;
    uint8_t flags;
    // Part of awake_s spent in auto light sleep.
    uint16_t light_sleep_s;
    uint16_t reserved;
};

constexpr uint32_t kPowerDiagnosticsMagic = 0x4E345045;  // "N4PE"
struct PowerLifecycleDiagnostics {
    uint32_t magic;
    uint32_t size;
    uint32_t boot_count;
    uint32_t timer_wake_count;
    uint32_t sleep_commit_count;
    uint32_t unexpected_reset_count;
    uint32_t consecutive_unexpected_resets;
    uint32_t last_wakeup_causes;
    int32_t last_photo_index;
    uint32_t history_next;
    BootRecord history[kBootHistoryLength];
    uint32_t checksum;
};
static_assert(std::is_trivially_default_constructible_v<PowerLifecycleDiagnostics>,
              "RTC_NOINIT storage must not get a startup constructor");

RTC_NOINIT_ATTR RetainedSchedule s_retained_schedule;
RTC_NOINIT_ATTR PowerLifecycleDiagnostics s_power_diagnostics;
bool s_sntp_started = false;

// Auto light sleep this boot. Written only by the exit callback, which runs
// on the IDLE task of the one core that slept (the other is stalled for the
// duration), and read by the app task once the low-power window has closed.
uint64_t s_light_sleep_us = 0;
uint32_t s_light_sleep_periods = 0;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
IRAM_ATTR esp_err_t OnLightSleepExit(int64_t sleep_time_us, void*) {
    if (sleep_time_us > 0) {
        s_light_sleep_us += static_cast<uint64_t>(sleep_time_us);
        ++s_light_sleep_periods;
    }
    return ESP_OK;
}
#endif

uint32_t RetainedChecksum(const void* data, size_t length) {
    // FNV-1a. Only has to reject random power-on contents and torn writes.
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

template <typename Record>
void SealRetained(Record& record) {
    record.checksum = RetainedChecksum(&record, offsetof(Record, checksum));
}

template <typename Record>
bool RetainedRecordValid(const Record& record, uint32_t magic) {
    return record.magic == magic && record.size == sizeof(Record) &&
           record.checksum == RetainedChecksum(&record, offsetof(Record, checksum));
}

void SealRetainedSchedule() { SealRetained(s_retained_schedule); }

ui::PersistentDisplayMetadata RetainedDisplayMetadata() {
    ui::PersistentDisplayMetadata metadata;
    memcpy(&metadata, s_retained_schedule.display_metadata, sizeof(metadata));
    return metadata;
}

void StoreRetainedDisplayMetadata(const ui::PersistentDisplayMetadata& metadata) {
    memcpy(s_retained_schedule.display_metadata, &metadata, sizeof(metadata));
}

BootRecord& CurrentBootRecord() {
    const uint32_t newest = (s_power_diagnostics.history_next + kBootHistoryLength - 1) %
                            kBootHistoryLength;
    return s_power_diagnostics.history[newest];
}

void MarkBootRecord(uint8_t flags) {
    CurrentBootRecord().flags |= flags;
    SealRetained(s_power_diagnostics);
}

// Indexed by Application::BootKind.
constexpr const char* kBootKindNames[] = {
    "cold", "restart", "scheduled", "button", "charger", "recovery",
};

const char* BootKindName(uint8_t kind) {
    return kind < sizeof(kBootKindNames) / sizeof(kBootKindNames[0])
               ? kBootKindNames[kind]
               : "?";
}

const char* ResetReasonName(int reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "ext";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "other";
    }
}

void LogBootHistory(int max_entries) {
    int logged = 0;
    for (int age = std::min(max_entries, kBootHistoryLength); age >= 1; --age) {
        const uint32_t slot = (s_power_diagnostics.history_next + kBootHistoryLength - age) %
                              kBootHistoryLength;
        const BootRecord& entry = s_power_diagnostics.history[slot];
        if (entry.rtc_s == 0 && entry.reset_reason == 0) continue;
        ESP_LOGI(kTag,
                 "Boot history -%d: rtc=%lus reset=%s kind=%s battery=%d flags=0x%02x "
                 "awake=%us light_sleep=%us%s",
                 age, static_cast<unsigned long>(entry.rtc_s),
                 ResetReasonName(entry.reset_reason), BootKindName(entry.boot_kind),
                 entry.battery == 0xFF ? -1 : static_cast<int>(entry.battery),
                 static_cast<unsigned>(entry.flags),
                 static_cast<unsigned>(entry.awake_s),
                 static_cast<unsigned>(entry.light_sleep_s),
                 (entry.flags & kBootFlagSleepCommitted)     ? ""
                 : (entry.flags & kBootFlagRestartRequested) ? " (restart requested)"
                                                             : " (no sleep commit)");
        ++logged;
    }
    if (logged == 0) {
        ESP_LOGI(kTag, "Boot history: empty");
    }
}

// Resets that interrupt whatever the device was doing. A recovery boot must
// rebuild the frame the glass was showing (the interrupted waveform may have
// left it half drawn) instead of falling back to the cold-boot page.
constexpr bool IsUnexpectedReset(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT || reason == ESP_RST_CPU_LOCKUP ||
           reason == ESP_RST_PWR_GLITCH;
}

static_assert(IsUnexpectedReset(ESP_RST_PANIC));
static_assert(IsUnexpectedReset(ESP_RST_BROWNOUT));
static_assert(!IsUnexpectedReset(ESP_RST_DEEPSLEEP));
static_assert(!IsUnexpectedReset(ESP_RST_POWERON));
static_assert(!IsUnexpectedReset(ESP_RST_SW), "the stall guard is reported separately");

// After this many unexpected resets without a single clean sleep commit in
// between, stop repeating whatever the crashing boot did (a full waveform, a
// radio start): keep the glass as it is and go straight back to sleep.
constexpr uint32_t kQuietRecoveryThreshold = 3;

extern "C" RtcPcf8563* ZectrixGetRtc();
extern "C" void ZectrixPrepareForDeepSleep();
extern "C" void ZectrixPrepareForScheduledWake();
extern "C" bool ZectrixIsExternalPowerPresent();
extern "C" bool ZectrixEnterLowPowerWait();
extern "C" void ZectrixLeaveLowPowerWait();

int CurrentLocalDateKey() {
    time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    return (local_tm.tm_year + 1900) * 10000 +
           (local_tm.tm_mon + 1) * 100 + local_tm.tm_mday;
}

int SecondsUntilNextLocalMidnight() {
    time_t now = time(nullptr);
    struct tm next_tm = {};
    localtime_r(&now, &next_tm);
    if (next_tm.tm_year + 1900 < 2020) return 0;
    next_tm.tm_hour = 0;
    next_tm.tm_min = 0;
    next_tm.tm_sec = 0;
    next_tm.tm_mday += 1;
    next_tm.tm_isdst = -1;
    const time_t next_midnight = mktime(&next_tm);
    if (next_midnight <= now) return 0;
    return static_cast<int>(next_midnight - now);
}

bool HasDateWakeDependency(const ui::PersistentDisplayContract& contract) {
    const auto date_dependencies =
        rawdraw::PersistentDependencyMask(rawdraw::PersistentDisplayDependency::Date) |
        rawdraw::PersistentDependencyMask(rawdraw::PersistentDisplayDependency::PageDate);
    return contract.restorable &&
           (contract.wake_dependencies & date_dependencies) != 0;
}

void InitializeLocalClockFromRtc() {
    setenv("TZ", "CST-8", 1);
    tzset();
    auto* rtc = ZectrixGetRtc();
    if (!rtc) return;
    struct tm rtc_tm = {};
    if (!rtc->GetTime(rtc_tm) || rtc_tm.tm_year + 1900 < 2020) {
        ESP_LOGW(kTag, "RTC time unavailable or invalid; midnight scheduling deferred");
        return;
    }
    rtc_tm.tm_isdst = -1;
    const time_t rtc_epoch = mktime(&rtc_tm);
    if (rtc_epoch <= 0) return;
    const timeval tv{.tv_sec = rtc_epoch, .tv_usec = 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(kTag, "System clock restored from PCF8563: date_key=%d",
             CurrentLocalDateKey());
}

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "关闭";
    if (minutes == 1440) return "1天";
    if (minutes >= 60 && minutes % 60 == 0) {
        char hour_buf[16];
        snprintf(hour_buf, sizeof(hour_buf), "%dh", minutes / 60);
        return hour_buf;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

std::string FormatNetworkSyncLabel(int minutes, bool wifi_enabled) {
    if (minutes <= 0) return "关闭";
    if (!wifi_enabled) return "暂停/Wi-Fi关闭";
    return FormatMinutesLabel(minutes);
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "关闭" : "开启";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

int NextNetworkSyncInterval(int current) {
    static constexpr int kOptions[] = {0, 10, 30, 60, 360, 720, 1440};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 0;
}

bool IsValidSlideshowInterval(int value) {
    return value == 0 || value == 5 || value == 10 || value == 30;
}

bool IsValidNetworkSyncInterval(int value) {
    return value == 0 || value == 10 || value == 30 || value == 60 ||
           value == 360 || value == 720 || value == 1440;
}

// The three helpers below report whether they actually changed anything, so
// UpdateStatusBarForUi() can tell a real state transition from an echo of one
// a button handler already applied.
bool UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool enabled, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return false;
    const bool checked_changed = renderer->UpdateChecked(kSettingsWifiIndex, enabled);
    const bool value_changed = renderer->UpdateItem(
        kSettingsWifiIndex,
        value ? value : (!enabled ? "已关闭" : (connected ? "已连接" : "未连接")));
    return checked_changed || value_changed;
}

bool UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return false;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "已开启" : "已关闭";
    }
    const bool checked_changed = renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    const bool value_changed = renderer->UpdateItem(kSettingsHttpServerIndex, value);
    return checked_changed || value_changed;
}

bool UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return false;
    return renderer->UpdateItem(kSettingsLanIpIndex,
                                ip_address.empty() ? "未获取" : ip_address);
}

void StartSntpClockSyncOnce() {
    if (s_sntp_started) return;

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        // This callback runs in lwIP's TCP/IP task.  Publish only an atomic
        // event here: stopping Wi-Fi, destroying esp-netif, touching UI state,
        // or performing an I2C RTC transaction from this context can deadlock
        // the network stack that is currently delivering the callback.
        Application::GetInstance().OnSntpSynchronized();
    });
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Asia/Shanghai servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org");
}

void StopSntpIfStarted() {
    if (!s_sntp_started) return;
    esp_sntp_stop();
    s_sntp_started = false;
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

}  // namespace

Application::Application() = default;

Application::~Application() = default;

void Application::Initialize(bool display_stall_recovery_boot,
                             bool user_restart_bounce) {
    static_assert(static_cast<int>(BootKind::Recovery) == 5,
                  "kBootKindNames must follow BootKind");
    RuntimeGuardNoteProgress(RuntimeGuardPhase::Initializing);
    input_ready_.store(false, std::memory_order_release);
    scheduled_wake_accepts_input_.store(false, std::memory_order_release);
    InitializePowerManagement();
    Settings gallery_nvs(kGalleryNamespace, false);
    slideshow_interval_minutes_ = gallery_nvs.GetInt(kSlideshowIntervalKey, 5);
    if (!IsValidSlideshowInterval(slideshow_interval_minutes_)) {
        slideshow_interval_minutes_ = 5;
    }
    Settings sync_nvs(kSyncNamespace, false);
    network_sync_interval_minutes_ = sync_nvs.GetInt(kSyncIntervalKey, 0);
    wifi_enabled_.store(sync_nvs.GetBool(kWifiEnabledKey, false),
                        std::memory_order_release);
    if (!IsValidNetworkSyncInterval(network_sync_interval_minutes_)) {
        network_sync_interval_minutes_ = 0;
    }
    auto& board = Board::GetInstance();
    RuntimeGuardNoteProgress(RuntimeGuardPhase::Initializing);
    InitializeLocalClockFromRtc();

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    const bool deep_sleep_wake = reset_reason == ESP_RST_DEEPSLEEP;
    const bool timer_wakeup =
        deep_sleep_wake && (wakeup_causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0;
    uint64_t ext1_wakeup_mask = 0;
    if ((wakeup_causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) != 0) {
        ext1_wakeup_mask = esp_sleep_get_ext1_wakeup_status();
    }
    const uint64_t button_wakeup_mask =
        ext1_wakeup_mask & ((1ULL << BOOT_BUTTON_GPIO) | (1ULL << TODO_DOWN_BUTTON_GPIO));
    const bool charger_wakeup =
        (wakeup_causes & (1UL << ESP_SLEEP_WAKEUP_EXT0)) != 0 ||
        (ext1_wakeup_mask & (1ULL << CHARGE_DETECT_GPIO)) != 0;
    const bool unexpected_reset =
        display_stall_recovery_boot || IsUnexpectedReset(reset_reason);
    const bool retained_valid = RetainedRecordValid(s_retained_schedule, kRetainedMagic);

    if (user_restart_bounce) {
        boot_kind_ = BootKind::UserRestart;
    } else if (unexpected_reset) {
        boot_kind_ = BootKind::Recovery;
    } else if (timer_wakeup && retained_valid) {
        boot_kind_ = BootKind::ScheduledWake;
    } else if (deep_sleep_wake && !timer_wakeup && button_wakeup_mask != 0) {
        boot_kind_ = BootKind::ButtonWake;
    } else if (deep_sleep_wake && !timer_wakeup && charger_wakeup) {
        boot_kind_ = BootKind::ChargerWake;
    } else {
        boot_kind_ = BootKind::ColdBoot;
    }

    if (!RetainedRecordValid(s_power_diagnostics, kPowerDiagnosticsMagic)) {
        memset(&s_power_diagnostics, 0, sizeof(s_power_diagnostics));
        s_power_diagnostics.magic = kPowerDiagnosticsMagic;
        s_power_diagnostics.size = sizeof(s_power_diagnostics);
        s_power_diagnostics.last_photo_index = -1;
    }
    // The previous boots first: when the frame is found frozen, this is the
    // record of how the last wakes ended, and a boot whose entry has no
    // awake time never reached its sleep commit.
    LogBootHistory(boot_kind_ == BootKind::ScheduledWake ? 3 : kBootHistoryLength);
    ++s_power_diagnostics.boot_count;
    if (timer_wakeup) {
        ++s_power_diagnostics.timer_wake_count;
    }
    if (unexpected_reset) {
        ++s_power_diagnostics.unexpected_reset_count;
        ++s_power_diagnostics.consecutive_unexpected_resets;
    } else if (!deep_sleep_wake) {
        s_power_diagnostics.consecutive_unexpected_resets = 0;
    }
    s_power_diagnostics.last_wakeup_causes = wakeup_causes;
    BootRecord& record =
        s_power_diagnostics.history[s_power_diagnostics.history_next % kBootHistoryLength];
    record = {};
    record.rtc_s = static_cast<uint32_t>(NowRtcUs() / 1000000);
    record.wake_causes = static_cast<uint16_t>(wakeup_causes & 0xFFFF);
    record.reset_reason = static_cast<uint8_t>(reset_reason);
    record.boot_kind = static_cast<uint8_t>(boot_kind_);
    record.battery = 0xFF;
    s_power_diagnostics.history_next =
        (s_power_diagnostics.history_next + 1) % kBootHistoryLength;
    SealRetained(s_power_diagnostics);
    ESP_LOGI(kTag,
             "Power lifecycle: boots=%u timer_wakes=%u sleep_commits=%u "
             "unexpected_resets=%u consecutive=%u reset=%s kind=%s last_photo=%ld",
             static_cast<unsigned>(s_power_diagnostics.boot_count),
             static_cast<unsigned>(s_power_diagnostics.timer_wake_count),
             static_cast<unsigned>(s_power_diagnostics.sleep_commit_count),
             static_cast<unsigned>(s_power_diagnostics.unexpected_reset_count),
             static_cast<unsigned>(s_power_diagnostics.consecutive_unexpected_resets),
             ResetReasonName(reset_reason), BootKindName(record.boot_kind),
             static_cast<long>(s_power_diagnostics.last_photo_index));
    ESP_LOGI(kTag, "Wake sources=0x%08lx ext1_gpio_mask=0x%llx retained=%d",
             static_cast<unsigned long>(wakeup_causes),
             static_cast<unsigned long long>(ext1_wakeup_mask),
             retained_valid ? 1 : 0);

    if (!retained_valid) {
        memset(&s_retained_schedule, 0, sizeof(s_retained_schedule));
        s_retained_schedule.magic = kRetainedMagic;
        s_retained_schedule.size = sizeof(s_retained_schedule);
        StoreRetainedDisplayMetadata(ui::PersistentDisplayMetadata{});
        ResetSlideshowDeadline();
        ResetNetworkSyncDeadline();
    } else if (s_retained_schedule.slideshow_config_minutes != slideshow_interval_minutes_ ||
               s_retained_schedule.sync_config_minutes != network_sync_interval_minutes_) {
        // The stored intervals changed underneath the retained schedule (NVS
        // reset, firmware defaults). Re-arm both, keep the photo position, and
        // start this boot interactively rather than trust stale deadlines.
        ResetSlideshowDeadline();
        ResetNetworkSyncDeadline();
        if (boot_kind_ == BootKind::ScheduledWake) {
            boot_kind_ = BootKind::ColdBoot;
        }
    }
    scheduler_timer_wake_ = boot_kind_ == BootKind::ScheduledWake;
    const bool interactive_button_wake = boot_kind_ == BootKind::ButtonWake;
    NormalizeScheduleDeadlines();

    const ui::PersistentDisplayMetadata retained_metadata = RetainedDisplayMetadata();
    const int retained_photo_index = s_retained_schedule.selected_photo_index;
    const bool retained_fullscreen = s_retained_schedule.gallery_fullscreen != 0;
    const bool retained_gallery_restorable =
        retained_valid && retained_metadata.contract.restorable &&
        retained_metadata.contract.restore_kind == ui::PersistentRestoreKind::Gallery;
    const bool quiet_recovery =
        boot_kind_ == BootKind::Recovery &&
        s_power_diagnostics.consecutive_unexpected_resets >= kQuietRecoveryThreshold;
    quiet_recovery_ = quiet_recovery;
    // A recovery boot rebuilds the frame the glass was showing: the reset may
    // have cut a waveform short, and falling back to the cold-boot memory-card
    // page is what used to leave a menu-like screen up until the next
    // slideshow wake. A charger insertion wakes the device too; it is not a
    // reason to forget which photo was on display.
    // The low-battery notice is not a restorable page, but the selection
    // behind it is still valid: charging resumes the gallery it replaced.
    const bool notice_on_glass =
        retained_valid && s_retained_schedule.low_battery_notice_shown != 0;
    const bool restore_retained_frame =
        (boot_kind_ == BootKind::Recovery || boot_kind_ == BootKind::ChargerWake) &&
        (retained_gallery_restorable || notice_on_glass);
    const bool adopt_retained_display =
        retained_valid && (scheduler_timer_wake_ || interactive_button_wake ||
                           restore_retained_frame || quiet_recovery);
    // One line per boot describing the single schedule, the way
    // "Power lifecycle:" describes the single power record. Both deadlines are
    // absolute, so consecutive boots must show them counting down in real
    // time; a value that jumps back to a full period, or that does not move
    // across a cycle, is the desynchronization this replaced.
    ESP_LOGI(kTag,
             "Schedule: rtc_now=%llds slideshow=%dmin/%ds sync=%dmin/%ds photo=%d fullscreen=%d",
             static_cast<long long>(NowRtcUs() / 1000000),
             slideshow_interval_minutes_, SecondsUntilSlideshow(),
             network_sync_interval_minutes_, SecondsUntilNetworkSync(),
             retained_photo_index, retained_fullscreen ? 1 : 0);
    RuntimeGuardSetScheduledWake(scheduler_timer_wake_);
    if (scheduler_timer_wake_) {
        slideshow_due_ = IsSlideshowDue();
        network_sync_pending_ = ShouldRunScheduledNetworkSync(
            wifi_enabled_.load(std::memory_order_acquire),
            network_sync_interval_minutes_,
            SecondsUntilNetworkSync());
        // Consuming a due job re-arms its own deadline, and only its own. The
        // two schedules previously shared one decrement of the planned sleep
        // length, so waking for either of them advanced both.
        if (slideshow_due_) {
            ResetSlideshowDeadline();
        }
        if (network_sync_pending_) {
            ResetNetworkSyncDeadline();
        }
        display_invalidation_due_ =
            HasDateWakeDependency(retained_metadata.contract) &&
            retained_metadata.snapshot.date_key != CurrentLocalDateKey();
        ESP_LOGI(kTag,
                 "Timer wake reason: slideshow=%d network=%d display_invalidation=%d photo=%d",
                 slideshow_due_ ? 1 : 0, network_sync_pending_ ? 1 : 0,
                 display_invalidation_due_ ? 1 : 0, retained_photo_index);
    }
    ESP_LOGI(kTag, "Network policy: wifi_enabled=%d sync_interval=%d scheduled_sync=%d",
             wifi_enabled_.load(std::memory_order_acquire) ? 1 : 0,
             network_sync_interval_minutes_, network_sync_pending_ ? 1 : 0);

    SetDeviceState(kDeviceStateStarting);

    if (!scheduler_timer_wake_) {
        AudioCodec* codec = board.GetAudioCodec();
        if (codec == nullptr) {
            ESP_LOGE(kTag, "Audio codec is null");
            SetDeviceState(kDeviceStateFatalError);
            return;
        }
        audio_service_.Initialize(codec);
        audio_service_.Start();
        audio_started_ = true;
    } else {
        ZectrixPrepareForScheduledWake();
        ESP_LOGI(kTag, "Timer wake: audio codec and audio tasks skipped");
    }

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    RuntimeGuardRegisterDisplay(lcd);
    RuntimeGuardNoteProgress(RuntimeGuardPhase::Initializing);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    // Seed the very first framebuffer with a real battery sample. Previously
    // this happened accidentally through the automatic Wi-Fi status callback;
    // with radio-idle startup that callback does not run, leaving level=-1 and
    // hiding the icon. Timer wakes sample here too, so the battery indication
    // advances only together with an already-due slideshow refresh.
    int initial_battery_level = -1;
    bool initial_charging = false;
    bool initial_discharging = false;
    board.GetBatteryLevel(initial_battery_level, initial_charging,
                          initial_discharging);
    ui::RawDrawStatusBarData initial_status =
        rawdraw_ui_manager_->GetStatusBarData();
    initial_status.battery_level = initial_battery_level;
    initial_status.battery_charging = initial_charging;
    rawdraw_ui_manager_->UpdateStatusBar(initial_status);
    external_power_present_ = !initial_discharging;
    external_power_probe_deadline_us_ =
        esp_timer_get_time() + 1000LL * 1000;
    display_freshness_probe_deadline_us_ =
        esp_timer_get_time() + 30LL * 1000 * 1000;
    if (initial_battery_level >= 0 && initial_battery_level <= 100) {
        CurrentBootRecord().battery = static_cast<uint8_t>(initial_battery_level);
        SealRetained(s_power_diagnostics);
    }
    battery_level_at_boot_ = initial_battery_level;
    UpdateBatteryProtection(initial_battery_level, external_power_present_);
    if (battery_critical_ && scheduler_timer_wake_) {
        // No slideshow waveform and no radio on a battery this low; the sleep
        // path puts up the charge notice instead.
        slideshow_due_ = false;
        display_invalidation_due_ = false;
        network_sync_pending_ = false;
    }
    ESP_LOGI(kTag,
             "Initial battery sample: level=%d charging=%d external_power=%d",
             initial_battery_level, initial_charging ? 1 : 0,
             external_power_present_ ? 1 : 0);
    const bool defer_initial_refresh = scheduler_timer_wake_ || interactive_button_wake ||
                                       restore_retained_frame || quiet_recovery;
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, ui::RefreshIntent intent) {
        if (intent != ui::RefreshIntent::FullColor) {
            // Buttons, menus and cursor movement keep the low-latency path.
            // Photo/content changes recover after 10 s; menu-only operations
            // deliberately defer the same recovery to 30 s.
            const auto recovery_mode =
                intent == ui::RefreshIntent::FastBwDeferredInteraction
                    ? ssd2683_fast_bw::RecoveryMode::DeferredInteraction
                    : ssd2683_fast_bw::RecoveryMode::Quality;
            lcd->RequestFastBwRefresh(recovery_mode);
        } else {
            // Frames nobody is waiting on (slideshow, sleep, date rollover)
            // and menu entry: render them correctly once.
            lcd->RequestUrgentFullRefresh();
        }
    }, defer_initial_refresh);
    if (adopt_retained_display) {
        rawdraw_ui_manager_->AdoptRetainedPersistentMetadata(retained_metadata);
    }
    // The UI manager performs the advance; the dwell clock lives here so the
    // awake path and the deep-sleep wake share one deadline.
    rawdraw_ui_manager_->SetSlideshowDwellResetCallback(
        [this]() { ResetSlideshowDeadline(); });
    rawdraw_ui_manager_->SetSlideshowIntervalChangedCallback(
        [this](int minutes) { RequestSlideshowIntervalChange(minutes); });
    if (quiet_recovery) {
        // Whatever the last few boots did, it reset the device each time.
        // Leave the glass and the radio alone, keep the retained photo
        // position, and go straight back to the deep-sleep schedule.
        if (restore_retained_frame) {
            rawdraw_ui_manager_->RestoreRetainedGalleryState(
                retained_photo_index, retained_fullscreen, false);
        }
        MarkBootRecord(kBootFlagQuietRecovery);
        ESP_LOGW(kTag,
                 "Quiet recovery after %u consecutive unexpected resets: no refresh, no radio",
                 static_cast<unsigned>(s_power_diagnostics.consecutive_unexpected_resets));
    } else if (restore_retained_frame) {
        // A recovery repaints the retained frame: the reset may have stopped
        // the panel mid-waveform. A charger wake only repaints a page that
        // shows the battery state; a fullscreen photo is already on glass.
        const bool refresh =
            boot_kind_ == BootKind::Recovery || !retained_fullscreen || notice_on_glass;
        const bool restored = rawdraw_ui_manager_->RestoreRetainedGalleryState(
            retained_photo_index, retained_fullscreen, refresh);
        if (!restored) {
            rawdraw_ui_manager_->PresentBootBaseline();
        }
        if (!restored || refresh) {
            MarkBootRecord(kBootFlagFrameRefreshed);
        }
        ESP_LOGW(kTag,
                 "%s restored retained gallery frame=%d photo=%d fullscreen=%d refresh=%d",
                 BootKindName(static_cast<uint8_t>(boot_kind_)), restored ? 1 : 0,
                 retained_photo_index, retained_fullscreen ? 1 : 0, refresh ? 1 : 0);
    } else if (scheduler_timer_wake_ && slideshow_due_) {
        slideshow_refresh_requested_ = rawdraw_ui_manager_->PrepareGallerySlideshowFrame(
            retained_photo_index, true, retained_fullscreen);
        if (slideshow_refresh_requested_) {
            MarkBootRecord(kBootFlagFrameRefreshed);
        }
        ESP_LOGI(kTag, "Scheduled slideshow frame requested=%d",
                 slideshow_refresh_requested_ ? 1 : 0);
    } else if (scheduler_timer_wake_ && display_invalidation_due_) {
        display_invalidation_refresh_requested_ =
            rawdraw_ui_manager_->PrepareRetainedDisplayInvalidation(
                retained_photo_index, retained_fullscreen);
        if (display_invalidation_refresh_requested_) {
            MarkBootRecord(kBootFlagFrameRefreshed);
        }
        ESP_LOGI(kTag,
                 "Scheduled display invalidation caused refresh=%d",
                 display_invalidation_refresh_requested_ ? 1 : 0);
    } else if (interactive_button_wake) {
        const bool down_wake =
            (button_wakeup_mask & (1ULL << TODO_DOWN_BUTTON_GPIO)) != 0;
        const rawdraw::ButtonEvent wake_event{
            down_wake ? rawdraw::ButtonEvent::kDownClick
                      : rawdraw::ButtonEvent::kBootClick};
        slideshow_refresh_requested_ =
            rawdraw_ui_manager_->PrepareGalleryInteractiveWakeFrame(
                retained_photo_index, retained_fullscreen, wake_event);
        MarkBootRecord(kBootFlagFrameRefreshed);
        ESP_LOGI(kTag,
                 "Interactive button wake replayed: gpio=%d requested=%d",
                 down_wake ? static_cast<int>(TODO_DOWN_BUTTON_GPIO)
                           : static_cast<int>(BOOT_BUTTON_GPIO),
                 slideshow_refresh_requested_ ? 1 : 0);
    }

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        int slideshow_interval = slideshow_interval_minutes_;
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         [this]() { OnRestartItemActivated(); }});
        items.push_back({"相册", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"轮播间隔", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ApplySlideshowInterval(
                                 NextSlideshowInterval(slideshow_interval_minutes_));
                         }});
        items.push_back({"网络", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"定时联网",
                         FormatNetworkSyncLabel(
                             network_sync_interval_minutes_,
                             wifi_enabled_.load(std::memory_order_acquire)),
                         nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kSyncNamespace, true);
                             const int next =
                                 NextNetworkSyncInterval(network_sync_interval_minutes_);
                             nvs.SetInt(kSyncIntervalKey, next);
                             network_sync_interval_minutes_ = next;
                             ResetNetworkSyncDeadline();
                             sr->UpdateItem(kSettingsNetworkSyncIndex,
                                            FormatNetworkSyncLabel(
                                                next,
                                                wifi_enabled_.load(
                                                    std::memory_order_acquire)));
                             ESP_LOGI(kTag, "Periodic network session set to %s",
                                      FormatMinutesLabel(next).c_str());
                         }});
        items.push_back({"Wi-Fi",
                         wifi_enabled_.load(std::memory_order_acquire)
                             ? "未连接" : "已关闭",
                         nullptr, rawdraw::SettingsItemType::Checkbox,
                         wifi_enabled_.load(std::memory_order_acquire),
                         [this, sr]() {
                             const bool enabled =
                                 wifi_enabled_.load(std::memory_order_acquire);
                             if (enabled) {
                                 // Same teardown as a remote request, so the
                                 // two entry points cannot drift apart.
                                 DisableWifiAndPersist("settings toggle");
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateWifiSettingsItem(sr, false, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 wifi_enabled_.store(true, std::memory_order_release);
                                 Settings nvs(kSyncNamespace, true);
                                 nvs.SetBool(kWifiEnabledKey, true);
                                 ResetNetworkSyncDeadline();
                                 UpdateWifiSettingsItem(sr, true, false, "连接中");
                                 StartInteractiveWifiAttempt();
                             }
                             sr->UpdateItem(
                                 kSettingsNetworkSyncIndex,
                                 FormatNetworkSyncLabel(
                                     network_sync_interval_minutes_,
                                     wifi_enabled_.load(
                                         std::memory_order_acquire)));
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网服务", "已关闭", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, "需先连接WiFi");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, "等待IP");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网IP", "未获取", nullptr, rawdraw::SettingsItemType::Normal, false});
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    if (interactive_button_wake) {
        ESP_LOGI(kTag, "Interactive wake from deep sleep");
        board.FlashActivityLed();
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        // Wi-Fi events arrive on an ESP event-loop task. Serialize them with UI,
        // buttons, SNTP completion, and the sleep state machine on app_main.
        Schedule([this, event, data]() { HandleNetworkEvent(event, data); });
    });

    if (network_sync_pending_) {
        StartScheduledNetworkSync();
        MarkBootRecord(kBootFlagNetworkSession);
    } else if (!scheduler_timer_wake_ && boot_kind_ != BootKind::Recovery &&
               !battery_critical_ && wifi_enabled_.load(std::memory_order_acquire)) {
        // A recovery boot stays radio-idle: if the radio was involved in the
        // reset, starting it again on every boot is how a single fault turns
        // into a reset loop. The periodic sync still runs on schedule. A
        // critical battery gets no radio either.
        StartInteractiveWifiAttempt();
    }

    if (!scheduler_timer_wake_) {
        const int64_t now_us = esp_timer_get_time();
        interactive_sleep_deadline_us_ =
            quiet_recovery ? now_us : now_us + kInteractiveAwakeUs;
        last_input_us_ = now_us;
        if (interactive_button_wake) {
            const gpio_num_t wake_gpio =
                (button_wakeup_mask & (1ULL << TODO_DOWN_BUTTON_GPIO)) != 0
                    ? static_cast<gpio_num_t>(TODO_DOWN_BUTTON_GPIO)
                    : static_cast<gpio_num_t>(BOOT_BUTTON_GPIO);
            // The wake action above is deterministic. Keep normal callbacks
            // gated until release so the same physical press is not delivered
            // a second time by the button component during startup.
            // PostButtonAction() samples readiness when the event happens,
            // so a release or long-press that the button component reports
            // while this loop is still waiting is dropped, not queued.
            const int64_t release_deadline_us =
                esp_timer_get_time() + 5LL * 1000 * 1000;
            while (gpio_get_level(wake_gpio) == 0 &&
                   esp_timer_get_time() < release_deadline_us) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (gpio_get_level(wake_gpio) == 0) {
                ESP_LOGW(kTag,
                         "Wake key GPIO%d still held after 5 s; enabling input anyway",
                         static_cast<int>(wake_gpio));
            }
            // iot_button reports SINGLE_CLICK only after the 180 ms
            // multi-click window that follows the release. Keep input closed
            // past it so that click is still dropped at the source.
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        input_ready_.store(true, std::memory_order_release);
    } else {
        scheduled_wake_accepts_input_.store(true, std::memory_order_release);
    }

    SetDeviceState(kDeviceStateIdle);
    RuntimeGuardNoteProgress(RuntimeGuardPhase::Running);
}

void Application::PostButtonAction(ButtonAction action) {
    // Runs on the button task at the moment of the event, and that is where
    // readiness has to be sampled. The press that wakes the device from deep
    // sleep is replayed inside Initialize(); its release -- or a hold past the
    // one-second long-press time -- reaches the button component while
    // Initialize() is still waiting for that release. Checked later on the
    // app task, the queued event already saw input_ready_ == true and acted a
    // second time: a DOWN wake held for a second advanced the photo and then
    // opened Settings. Only BOOT used to be gated here.
    if (input_ready_.load(std::memory_order_acquire)) {
        Schedule([this, action]() { DispatchButtonAction(action); });
        return;
    }
    if (scheduled_wake_accepts_input_.load(std::memory_order_acquire)) {
        Schedule([this, action]() { HandleInputDuringScheduledWake(action); });
        return;
    }
    ESP_LOGI(kTag, "Button action %d ignored: input not ready",
             static_cast<int>(action));
}

void Application::DispatchButtonAction(ButtonAction action) {
    switch (action) {
        case ButtonAction::UpClick: OnUpClick(); break;
        case ButtonAction::DownClick: OnDownClick(); break;
        case ButtonAction::BootClick: OnBootClick(); break;
        case ButtonAction::UpLongPress: OnUpLongPress(); break;
        case ButtonAction::DownLongPress: OnDownLongPress(); break;
        case ButtonAction::BootLongPress: OnBootLongPress(); break;
        case ButtonAction::WifiConfigCombo: OnWifiConfigComboLongPress(); break;
    }
}

// A press during a timer wake used to be dropped: input stays closed for the
// whole scheduled session (about 25 s for a slideshow refresh), the device
// then went back to sleep, and the frame simply did not respond. Turn the
// session interactive instead and act on the press.
void Application::HandleInputDuringScheduledWake(ButtonAction action) {
    if (!scheduler_timer_wake_ || sleep_phase_ != SleepPhase::Awake) {
        ESP_LOGI(kTag, "Button action %d dropped: scheduled wake is already preparing sleep",
                 static_cast<int>(action));
        return;
    }
    PromoteScheduledWakeToInteractive("button press");
    DispatchButtonAction(action);
}

void Application::PromoteScheduledWakeToInteractive(const char* reason) {
    if (!scheduler_timer_wake_ || sleep_phase_ != SleepPhase::Awake) return;
    ESP_LOGI(kTag, "Scheduled wake promoted to interactive: %s", reason ? reason : "unspecified");
    EndLowPowerWait("interactive");
    scheduler_timer_wake_ = false;
    scheduled_wake_accepts_input_.store(false, std::memory_order_release);
    RuntimeGuardSetScheduledWake(false);
    MarkBootRecord(kBootFlagPromoted);
    if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->DisplayedStateReconstructed()) {
        // A wake that only ran the network job never rebuilt the frame on
        // glass; the framebuffer still holds Init()'s default page. Rebuild
        // the retained gallery state so the press acts on what is visible --
        // and repaint it when the glass shows something else (a page that
        // cannot be restored, or the low-battery notice).
        const auto retained = RetainedDisplayMetadata();
        const bool glass_shows_gallery =
            retained.contract.restorable &&
            retained.contract.restore_kind == ui::PersistentRestoreKind::Gallery &&
            s_retained_schedule.low_battery_notice_shown == 0;
        const bool restored = rawdraw_ui_manager_->RestoreRetainedGalleryState(
            s_retained_schedule.selected_photo_index,
            s_retained_schedule.gallery_fullscreen != 0, !glass_shows_gallery);
        if (!restored) {
            rawdraw_ui_manager_->PresentBootBaseline();
        }
    }
    const int64_t now_us = esp_timer_get_time();
    interactive_sleep_deadline_us_ = now_us + kInteractiveAwakeUs;
    last_input_us_ = now_us;
    external_power_probe_deadline_us_ = now_us;
    display_freshness_probe_deadline_us_ = now_us + 30LL * 1000 * 1000;
    input_ready_.store(true, std::memory_order_release);
}

void Application::OnUpClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "DOWN click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        ESP_LOGI(kTag, "UP long press - leaving settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
    }
}

void Application::OnDownLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "BOOT click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootLongPress() {
    if (!IsInputReady()) return;
    InputScope scope(*this);
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (network_initialized_ && WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
        }
        // StopStation() alone used to be the "Wi-Fi off" branch here, but
        // StartConfigAp() had already marked the station inactive, so it
        // returned at once and left the hotspot, its web/DNS servers and a
        // 10 s scan running. IsConfigMode() then stayed true and blocked idle
        // sleep until the battery was empty.
        StopConfigApIfRunning();
        if (wifi_enabled_.load(std::memory_order_acquire)) {
            StartInteractiveWifiAttempt();
        }
        return;
    }
    if (rawdraw_ui_manager_) {
        if (rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Gallery &&
            !rawdraw_ui_manager_->IsApTransferRunning()) {
            // BOOT long press on the gallery opens AP transfer, which drives
            // the Wi-Fi driver directly. With the Wi-Fi switch off since boot
            // that driver, esp-netif and the default event loop did not exist,
            // and esp_netif_create_default_wifi_ap() aborted the device.
            // Initialization alone leaves the radio off.
            EnsureNetworkInitialized();
            // An interactive station attempt still waiting to connect must not
            // time out underneath the hotspot: its StopStation() would stop
            // the driver the AP is running on.
            wifi_connect_deadline_us_ = 0;
        }
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::OnRestartItemActivated() {
    const int64_t now_us = esp_timer_get_time();
    auto* sr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetSettingsRenderer() : nullptr;
    // The settings cursor starts on this item, so a single BOOT press right
    // after opening Settings used to restart the device. Ask for a second one.
    if (restart_confirm_deadline_us_ == 0 || now_us > restart_confirm_deadline_us_) {
        restart_confirm_deadline_us_ = now_us + kRestartConfirmWindowUs;
        if (sr) sr->UpdateItem(kSettingsRestartIndex, "再按一次确认");
        ESP_LOGI(kTag, "Restart armed; press BOOT again within %lld s",
                 static_cast<long long>(kRestartConfirmWindowUs / 1000000));
        return;
    }
    restart_confirm_deadline_us_ = 0;
    if (sr) sr->UpdateItem(kSettingsRestartIndex, "重启中");
    // Restart from the supervisor loop once the panel is idle, so the reset
    // does not cut the waveform that is drawing this very confirmation.
    restart_requested_ = true;
    restart_requested_at_us_ = now_us;
    ESP_LOGI(kTag, "Restart confirmed; waiting for the panel to settle");
}

void Application::NoteButtonActivity() {
    const int64_t now_us = esp_timer_get_time();
    interactive_sleep_deadline_us_ = now_us + kInteractiveAwakeUs;
    last_input_us_ = now_us;
    manual_sleep_requested_.store(false, std::memory_order_release);
    // Fold a fresh battery/network sample into the button handler's one
    // already-planned render. This is opportunistic and never queues another
    // refresh of its own (fullscreen photos still have no such dependency).
    UpdateStatusBarForUi(false);
    // The actual button handler renders the resulting UI state with its
    // explicit FAST_BW intent. Queueing an additional background redraw here
    // used to turn long presses into a same-content automatic refresh and,
    // after refresh intents were separated, would immediately override the
    // 10/30-second interaction policy with FULL_COLOR.
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsApTransferRunning()) {
        // Both modes create the default AP netif. Starting the config portal
        // while AP transfer still owns it made esp_netif_new() refuse the
        // duplicate key, and the assert behind it aborted the device.
        ESP_LOGI(kTag, "Stopping AP transfer before entering Wi-Fi config");
        rawdraw_ui_manager_->StopApTransferMode();
    }
    wifi_connected_.store(false, std::memory_order_release);
    wifi_connect_deadline_us_ = 0;
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    EnsureNetworkInitialized();
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::StopConfigApIfRunning() {
    if (network_initialized_ && WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "Stopping Wi-Fi config hotspot");
        WifiManager::GetInstance().StopConfigAp();
    }
}

// ---------------------------------------------------------------------------
// Slideshow / network-sync schedule (single owner)
// ---------------------------------------------------------------------------

void Application::ResetSlideshowDeadline() {
    const int period = SchedulePeriodSeconds(slideshow_interval_minutes_);
    s_retained_schedule.slideshow_config_minutes = slideshow_interval_minutes_;
    s_retained_schedule.slideshow_due_rtc_us =
        period > 0 ? static_cast<uint64_t>(NowRtcUs()) +
                         static_cast<uint64_t>(period) * 1000000ULL
                   : 0;
    SealRetainedSchedule();
}

// Self-heals a retained schedule whose deadline cannot be true any more.
// The RTC timebase and the retained schedule normally live and die together --
// a power cycle clears both -- but they can come apart: an OTA that relocates
// esp_clk's retain structure restarts the clock while RTC_NOINIT survives. A
// deadline further out than one whole period is the observable symptom, and
// silently parking the slideshow for hours is the cost of ignoring it.
void Application::NormalizeScheduleDeadlines() {
    const int slideshow_period = SchedulePeriodSeconds(slideshow_interval_minutes_);
    if (slideshow_period <= 0) {
        s_retained_schedule.slideshow_due_rtc_us = 0;
    } else {
        const int remaining =
            SecondsUntilRtcDeadline(s_retained_schedule.slideshow_due_rtc_us);
        if (ScheduleDeadlineIsImplausible(remaining, slideshow_period)) {
            ESP_LOGW(kTag, "Re-arming implausible slideshow deadline (remaining=%d period=%d)",
                     remaining, slideshow_period);
            ResetSlideshowDeadline();
        }
    }

    const int sync_period = SchedulePeriodSeconds(network_sync_interval_minutes_);
    if (sync_period <= 0) {
        s_retained_schedule.sync_due_rtc_us = 0;
    } else {
        const int remaining =
            SecondsUntilRtcDeadline(s_retained_schedule.sync_due_rtc_us);
        if (ScheduleDeadlineIsImplausible(remaining, sync_period)) {
            ESP_LOGW(kTag, "Re-arming implausible network sync deadline (remaining=%d period=%d)",
                     remaining, sync_period);
            ResetNetworkSyncDeadline();
        }
    }
    SealRetainedSchedule();
}

void Application::ResetNetworkSyncDeadline() {
    const int period = SchedulePeriodSeconds(network_sync_interval_minutes_);
    s_retained_schedule.sync_config_minutes = network_sync_interval_minutes_;
    s_retained_schedule.sync_due_rtc_us =
        period > 0 ? static_cast<uint64_t>(NowRtcUs()) +
                         static_cast<uint64_t>(period) * 1000000ULL
                   : 0;
    SealRetainedSchedule();
}

int Application::SecondsUntilSlideshow() const {
    if (slideshow_interval_minutes_ <= 0) return -1;
    return SecondsUntilRtcDeadline(s_retained_schedule.slideshow_due_rtc_us);
}

int Application::SecondsUntilNetworkSync() const {
    if (network_sync_interval_minutes_ <= 0) return -1;
    return SecondsUntilRtcDeadline(s_retained_schedule.sync_due_rtc_us);
}

bool Application::IsSlideshowDue() const {
    return SecondsUntilSlideshow() == 0;
}

void Application::ApplySlideshowInterval(int minutes) {
    if (!IsValidSlideshowInterval(minutes)) {
        ESP_LOGW(kTag, "Ignoring invalid slideshow interval %d", minutes);
        return;
    }
    slideshow_interval_minutes_ = minutes;
    Settings nvs(kGalleryNamespace, true);
    nvs.SetInt(kSlideshowIntervalKey, minutes);
    ResetSlideshowDeadline();
    if (rawdraw_ui_manager_) {
        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
            sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(minutes));
        }
    }
    ESP_LOGI(kTag, "Slideshow interval set to %s", FormatMinutesLabel(minutes).c_str());
}

// Awake advance. Deep sleep is still the normal scheduler; this covers the
// sessions that stay awake past a deadline (USB power, LAN service, config AP)
// and keeps them on exactly the same clock rather than a second timer that
// drifted from the retained one.
void Application::PumpSlideshowSchedule() {
    if (!IsSlideshowDue() || !rawdraw_ui_manager_) return;
    // Re-arm first: a gallery state that cannot advance right now (delete
    // dialog up, a service page) must not re-evaluate as due every tick.
    ResetSlideshowDeadline();
    // A deep-sleep wake always returns to the fullscreen slideshow; an awake
    // session only did so if fullscreen was already showing, so on USB power
    // the frame stayed on the memory-card page after every cold boot, or on
    // Settings after a visit, for as long as it stayed plugged in. Take the
    // page back too, but only once nobody has touched a key for a full
    // interactive window.
    const bool user_idle =
        esp_timer_get_time() - last_input_us_ >= kInteractiveAwakeUs;
    if (rawdraw_ui_manager_->AdvanceGallerySlideshow(user_idle)) {
        ESP_LOGI(kTag, "Awake slideshow advance");
    }
}

void Application::RequestSlideshowIntervalChange(int minutes) {
    requested_slideshow_interval_.store(minutes, std::memory_order_release);
}

void Application::RequestWifiDisabled(const char* reason) {
    ESP_LOGI(kTag, "Wi-Fi disable requested: %s", reason ? reason : "unspecified");
    wifi_disable_requested_.store(true, std::memory_order_release);
}

void Application::DisableWifiAndPersist(const char* reason) {
    wifi_enabled_.store(false, std::memory_order_release);
    Settings sync_nvs(kSyncNamespace, true);
    sync_nvs.SetBool(kWifiEnabledKey, false);
    wifi_connect_deadline_us_ = 0;
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    // "Wi-Fi off" has to include the config hotspot: StopStation() does not
    // touch it, and a running hotspot keeps the device from sleeping.
    StopConfigApIfRunning();
    if (network_initialized_) {
        WifiManager::GetInstance().StopStation();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Wi-Fi switch persisted OFF: %s", reason ? reason : "unspecified");
}

void Application::ApplyPendingWifiDisableRequest() {
    if (!wifi_disable_requested_.exchange(false, std::memory_order_acq_rel)) return;
    // Runs even when the switch already reads off: the point of the request is
    // that some station activity is still up, and only the app task may take
    // it down.
    DisableWifiAndPersist("remote request");
    // Monochrome status-bar/settings text only; never escalate a remote
    // request into a full-colour waveform.
    UpdateStatusBarForUi();
}

void Application::EnsureNetworkInitialized() {
    if (network_initialized_) return;
    Board::GetInstance().RequestNetwork();
    network_initialized_ = true;
}

void Application::StartInteractiveWifiAttempt() {
    if (!wifi_enabled_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Wi-Fi attempt suppressed because switch is OFF");
        return;
    }
    EnsureNetworkInitialized();
    wifi_connect_deadline_us_ = esp_timer_get_time() + kNetworkSyncTimeoutUs;
    ESP_LOGI(kTag, "Interactive Wi-Fi attempt started with %lld s timeout",
             static_cast<long long>(kNetworkSyncTimeoutUs / 1000000));
    WifiManager::GetInstance().StartStation();
}

void Application::StartScheduledNetworkSync() {
    if (!wifi_enabled_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Scheduled network session suppressed because Wi-Fi switch is OFF");
        network_sync_pending_ = false;
        scheduled_network_jobs_pending_ = 0;
        return;
    }

    // The timer owns a short-lived online session, not SNTP itself. Today time
    // synchronization is the only registered job; future weather/news/etc.
    // jobs can join the same session and keep Wi-Fi alive until all are done.
    scheduled_network_jobs_pending_ = kScheduledNetworkJobTimeSync;
    ESP_LOGI(kTag,
             "Scheduled network session: starting Wi-Fi with %lld s timeout jobs=0x%lx",
             static_cast<long long>(kNetworkSyncTimeoutUs / 1000000),
             static_cast<unsigned long>(scheduled_network_jobs_pending_));
    EnsureNetworkInitialized();
    network_sync_deadline_us_ = esp_timer_get_time() + kNetworkSyncTimeoutUs;
    WifiManager::GetInstance().StartStation();
}

void Application::HandleNetworkEvent(NetworkEvent event, const std::string& data) {
    const bool update_interactive_ui =
        !scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake;
    switch (event) {
        case NetworkEvent::Connected:
            ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
            // Events are drained a tick after they were raised. A Connected
            // queued in the same tick as a session timeout or teardown would
            // otherwise mark the radio up and restart SNTP on an interface
            // that no longer exists.
            if (!network_initialized_ || !WifiManager::GetInstance().IsConnected()) {
                ESP_LOGI(kTag, "Ignoring stale Wi-Fi connection event");
                break;
            }
            // The Wi-Fi/IP handlers run on this task; an overflow there is a
            // panic in the middle of a network session.
            if (TaskHandle_t event_task = xTaskGetHandle("sys_evt")) {
                ESP_LOGI(kTag, "sys_evt stack headroom after connect: %u bytes",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(event_task)));
            }
            if (!wifi_enabled_.load(std::memory_order_acquire)) {
                ESP_LOGW(kTag, "Ignoring late Wi-Fi connection because switch is OFF");
                WifiManager::GetInstance().StopStation();
                break;
            }
            wifi_connect_deadline_us_ = 0;
            wifi_connected_.store(true, std::memory_order_release);
            StartSntpClockSyncOnce();
            if (!update_interactive_ui) {
                ESP_LOGI(kTag, "Scheduled/background connection kept off the persistent display");
                break;
            }
            // LAN transfer is intentionally opt-in. A scheduled sync only
            // keeps the radio up long enough for SNTP, then turns it off.
            if (rawdraw_ui_manager_ &&
                rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
            }
            UpdateStatusBarForUi();
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGI(kTag, "WiFi disconnected");
            wifi_connected_.store(false, std::memory_order_release);
            if (!update_interactive_ui) break;
            if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                rawdraw_ui_manager_->StopLanHttpServer();
            }
            UpdateStatusBarForUi();
            break;
        case NetworkEvent::Connecting:
        case NetworkEvent::Scanning:
            wifi_connected_.store(false, std::memory_order_release);
            if (!update_interactive_ui) break;
            UpdateStatusBarForUi(false);
            if (rawdraw_ui_manager_) {
                UpdateWifiSettingsItem(
                    rawdraw_ui_manager_->GetSettingsRenderer(),
                    wifi_enabled_.load(std::memory_order_acquire), false,
                    "连接中");
            }
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
            wifi_connected_.store(false, std::memory_order_release);
            if (!update_interactive_ui) break;
            if (rawdraw_ui_manager_) {
                auto& wifi = WifiManager::GetInstance();
                rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                        wifi.GetApPassword(),
                                                        wifi.GetApWebUrl());
            }
            UpdateStatusBarForUi();
            break;
        case NetworkEvent::WifiConfigModeExit:
            wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                  std::memory_order_release);
            if (!update_interactive_ui) break;
            if (rawdraw_ui_manager_ &&
                rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery");
                rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
            }
            UpdateStatusBarForUi();
            break;
        case NetworkEvent::ModemDetecting:
        case NetworkEvent::ModemErrorNoSim:
        case NetworkEvent::ModemErrorRegDenied:
        case NetworkEvent::ModemErrorInitFailed:
        case NetworkEvent::ModemErrorTimeout:
            wifi_connected_.store(false, std::memory_order_release);
            if (update_interactive_ui) UpdateStatusBarForUi();
            break;
    }
}

void Application::CompleteScheduledNetworkJob(uint32_t job) {
    if (!network_sync_pending_) return;
    scheduled_network_jobs_pending_ &= ~job;
    ESP_LOGI(kTag, "Scheduled network job complete: job=0x%lx remaining=0x%lx",
             static_cast<unsigned long>(job),
             static_cast<unsigned long>(scheduled_network_jobs_pending_));
    if (scheduled_network_jobs_pending_ == 0) {
        FinishScheduledNetworkSession("all jobs complete");
    }
}

void Application::FinishScheduledNetworkSession(const char* reason) {
    StopSntpIfStarted();
    if (network_initialized_) {
        WifiManager::GetInstance().StopStation();
    }
    wifi_connected_.store(false, std::memory_order_release);
    network_sync_pending_ = false;
    scheduled_network_jobs_pending_ = 0;
    network_sync_deadline_us_ = 0;
    ESP_LOGI(kTag, "Scheduled network session finished: %s; Wi-Fi stopped",
             reason ? reason : "unspecified");
}

void Application::EnterScheduledSleep() {
    ESP_LOGI(kTag, "Scheduled wake caused display refresh=%d",
             (slideshow_refresh_requested_ ||
              display_invalidation_refresh_requested_) ? 1 : 0);
    EnterLowPowerSleep("scheduled work complete");
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Manual sleep queued; waiting for current EPD refresh to finish");
    manual_sleep_requested_.store(true, std::memory_order_release);
}

// Called from the AP/LAN web-control task. Publish intent only: the NVS write,
// the radio teardown and the sleep transition all belong to the app task.
void Application::RequestManualSleep(bool disable_wifi) {
    if (disable_wifi) {
        RequestWifiDisabled("web sleep request");
    }
    EnterManualSleep();
}

// A photo frame should never go to sleep showing a menu. Leaving Settings
// (or a stopped service page) open and walking away used to commit deep sleep
// on that page; it then stayed on glass until the next slideshow wake, or for
// good with the slideshow off -- the "menu frozen mid-refresh" frame.
void Application::ReturnToSlideshowBeforeSleep() {
    if (!rawdraw_ui_manager_) return;
    const auto page = rawdraw_ui_manager_->GetCurrentPage();
    if (page == ui::RawDrawPageId::Gallery) return;
    if (rawdraw_ui_manager_->ReturnToGalleryForSleep()) {
        ESP_LOGI(kTag, "[FULL_COLOR] returning from %s to the gallery before sleep",
                 ui::RawDrawUiManager::GetPageTitle(page));
    }
}

void Application::UpdateBatteryProtection(int level, bool external_power) {
    uint8_t& strikes = s_retained_schedule.low_battery_strikes;
    if (external_power || level > kBatteryResumePercent) {
        strikes = 0;
    } else if (level >= 0 && level <= kCriticalBatteryPercent) {
        if (strikes < UINT8_MAX) ++strikes;
    }
    // Between the two thresholds the previous verdict stands (hysteresis).
    battery_critical_ = strikes >= kCriticalBatteryStrikes;
    SealRetainedSchedule();
    if (battery_critical_ || strikes > 0) {
        ESP_LOGW(kTag, "Battery protection: level=%d%% external_power=%d strikes=%u critical=%d",
                 level, external_power ? 1 : 0, static_cast<unsigned>(strikes),
                 battery_critical_ ? 1 : 0);
    }
}

void Application::ShowLowBatteryNoticeBeforeSleep() {
    if (!rawdraw_ui_manager_) return;
    // Already the frame on glass, and nothing has been drawn over it in this
    // wake: repainting it would only spend charge.
    if (s_retained_schedule.low_battery_notice_shown != 0 &&
        !rawdraw_ui_manager_->DisplayedStateReconstructed()) {
        return;
    }
    if (rawdraw_ui_manager_->ShowLowBatteryNotice(battery_level_at_boot_)) {
        s_retained_schedule.low_battery_notice_shown = 1;
        SealRetainedSchedule();
    }
}

void Application::EnterLowPowerSleep(const char* reason) {
    if (sleep_phase_ != SleepPhase::Awake) return;
    pending_sleep_reason_ = reason ? reason : "unspecified";
    input_ready_.store(false, std::memory_order_release);
    scheduled_wake_accepts_input_.store(false, std::memory_order_release);
    RuntimeGuardNoteProgress(RuntimeGuardPhase::PreparingSleep);
    ESP_LOGI(kTag, "Sleep preparation beginning: %s", pending_sleep_reason_);

    if (rawdraw_ui_manager_) {
        char dependencies[96];
        const auto metadata = rawdraw_ui_manager_->GetDisplayedPersistentMetadata();
        ESP_LOGI(kTag,
                 "Current display dependencies=%s wake=0x%lx restorable=%d kind=%d",
                 ui::RawDrawUiManager::DescribePersistentDependencies(
                     metadata.contract.visible_dependencies,
                     dependencies, sizeof(dependencies)),
                 static_cast<unsigned long>(metadata.contract.wake_dependencies),
                 metadata.contract.restorable ? 1 : 0,
                 static_cast<int>(metadata.contract.restore_kind));
        rawdraw_ui_manager_->BeginPersistentSleepPreparation();
        rawdraw_ui_manager_->StopHttpServicesForSleep();
    }
    StopSntpIfStarted();
    if (network_initialized_) WifiManager::GetInstance().StopStation();
    wifi_connected_.store(false, std::memory_order_release);
    network_sync_pending_ = false;
    scheduled_network_jobs_pending_ = 0;
    network_sync_deadline_us_ = 0;
    wifi_connect_deadline_us_ = 0;
    UpdateStatusBarForUi(false);
    // After the radio and the services are down, so the one frame drawn here
    // already carries the final status bar.
    if (quiet_recovery_) {
        // Paint nothing: a waveform may be what keeps resetting the device.
    } else if (battery_critical_) {
        ShowLowBatteryNoticeBeforeSleep();
    } else {
        ReturnToSlideshowBeforeSleep();
    }

    auto* sleep_lcd = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
    // Lock-free snapshot. This runs while the render task may still be
    // finishing a waveform, and the sleep path must never wait on that task's
    // framebuffer mutex -- the runtime guard would restart the device before
    // the wait resolved.
    const bool full_color_recovery_needed =
        sleep_lcd != nullptr && sleep_lcd->NeedsFullColorRecovery();

    ui::PersistentFramePreparation frame;
    if (rawdraw_ui_manager_) {
        frame = rawdraw_ui_manager_->PreparePersistentFrame();
    }
    char changed[96];
    char visible[96];
    ESP_LOGI(kTag,
             "Final visible synchronization: changed=%s visible=%s framebuffer_changed=%d",
             ui::RawDrawUiManager::DescribePersistentDependencies(
                 frame.changed_dependencies, changed, sizeof(changed)),
             ui::RawDrawUiManager::DescribePersistentDependencies(
                 frame.visible_changes, visible, sizeof(visible)),
             frame.framebuffer_changed ? 1 : 0);
    if (frame.refresh_requested) {
        ESP_LOGI(kTag,
                 "[FULL_COLOR] pre-sleep content refresh requested; waiting for EPD idle");
        sleep_phase_ = SleepPhase::WaitingForDisplay;
    } else if (full_color_recovery_needed && sleep_lcd != nullptr) {
        // Pixel semantics may already match while the physical panel is still
        // showing a FAST_BW interaction waveform. Deep sleep has no later idle
        // recovery opportunity, so settle that quality debt immediately.
        ESP_LOGI(kTag,
                 "[FULL_COLOR] pre-sleep FAST_BW recovery debt detected; forcing final waveform");
        sleep_lcd->RequestUrgentFullRefresh();
        sleep_phase_ = SleepPhase::WaitingForDisplay;
    } else if (sleep_lcd != nullptr && sleep_lcd->IsRefreshPending()) {
        // Something above (returning to the gallery) already queued the final
        // waveform. Committing now would cut the panel supply in the middle of
        // it.
        ESP_LOGI(kTag, "Pre-sleep refresh already queued; waiting for EPD idle");
        sleep_phase_ = SleepPhase::WaitingForDisplay;
    } else {
        ESP_LOGI(kTag,
                 "Pre-sleep full-color refresh skipped: pixels unchanged and panel already stable");
        sleep_phase_ = SleepPhase::ReadyToCommit;
    }
}

// Dynamic frequency scaling runs everywhere: whenever both cores are idle the
// CPU drops from 240 MHz to the 40 MHz crystal. Auto light sleep is enabled
// too, but awake_lock_ forbids it outside the low-power wait window, so the
// buttons, Wi-Fi, the HTTP servers and the USB console never see it.
void Application::InitializePowerManagement() {
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t lock = nullptr;
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "app_awake", &lock);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Power management lock unavailable: %s", esp_err_to_name(err));
        return;
    }
    // Before esp_pm_configure(): light sleep must be forbidden the moment it
    // becomes possible.
    esp_pm_lock_acquire(lock);
    awake_lock_ = lock;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t callbacks = {};
    callbacks.exit_cb = OnLightSleepExit;
    esp_pm_light_sleep_register_cbs(&callbacks);
#endif
    esp_pm_config_t config = {};
    config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    config.min_freq_mhz = CONFIG_XTAL_FREQ;
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    config.light_sleep_enable = true;
#endif
    err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Power management not configured: %s", esp_err_to_name(err));
    }
#endif
}

// A slideshow wake spends ~23 of its ~27 s waiting for the panel to finish
// its waveform, and at 240 MHz that idle wait was most of the energy the
// wake cost. Inside this window the chip light-sleeps between BUSY polls
// instead. Only where nothing can need it awake: a scheduled wake that no key
// has turned interactive, on battery, with no network session -- the radio
// and the USB console do not survive light sleep, and on external power there
// is nothing to save.
void Application::UpdateLowPowerWait(bool display_busy) {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (awake_lock_ == nullptr) {
        return;
    }
    const bool wanted = scheduler_timer_wake_ && display_busy && !network_sync_pending_ &&
                        !quiet_recovery_ && !ZectrixIsExternalPowerPresent();
    if (!wanted) {
        EndLowPowerWait(display_busy ? "conditions changed" : "panel idle");
        return;
    }
    // Also re-arms button standby after a key bounce that woke it without
    // producing a press (the window would otherwise stay open with the
    // 5 ms button timer running, and never sleep).
    if (!ZectrixEnterLowPowerWait()) {
        return;
    }
    if (low_power_wait_active_) {
        return;
    }
    if (auto* lcd = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay())) {
        lcd->SetLowPowerBusyWait(true);
    }
    low_power_wait_active_ = true;
    low_power_wait_started_us_ = esp_timer_get_time();
    ESP_LOGI(kTag, "Low-power wait: light sleep allowed until the waveform completes");
    // Last: from here on the chip may sleep whenever every task is blocked.
    esp_pm_lock_release(awake_lock_);
#else
    (void)display_busy;
#endif
}

void Application::EndLowPowerWait(const char* reason) {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (!low_power_wait_active_) {
        return;
    }
    // First: everything below assumes the chip stays awake.
    esp_pm_lock_acquire(awake_lock_);
    low_power_wait_active_ = false;
    ZectrixLeaveLowPowerWait();
    if (auto* lcd = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay())) {
        lcd->SetLowPowerBusyWait(false);
    }
    ESP_LOGI(kTag,
             "Low-power wait ended (%s) after %lld ms; light sleep so far %llu ms in %lu periods",
             reason ? reason : "unspecified",
             static_cast<long long>((esp_timer_get_time() - low_power_wait_started_us_) / 1000),
             static_cast<unsigned long long>(s_light_sleep_us / 1000),
             static_cast<unsigned long>(s_light_sleep_periods));
#else
    (void)reason;
#endif
}

void Application::CommitLowPowerSleep() {
    RuntimeGuardNoteProgress(RuntimeGuardPhase::CommittingSleep);
    // Auto light sleep re-arms the timer wakeup on every entry; it must not be
    // possible between configuring the deep-sleep sources and the commit.
    EndLowPowerWait("sleep commit");
    // A late station callback may race the asynchronous EPD waveform. Reassert
    // the final radio state immediately before power is committed; the glass
    // already contains the disconnected state selected by preflight.
    if (network_initialized_) WifiManager::GetInstance().StopStation();
    wifi_connected_.store(false, std::memory_order_release);
    StopSntpIfStarted();
    if (audio_started_) {
        audio_service_.Stop();
        audio_started_ = false;
    }

    s_retained_schedule.magic = kRetainedMagic;
    s_retained_schedule.size = sizeof(s_retained_schedule);
    s_retained_schedule.slideshow_config_minutes = slideshow_interval_minutes_;
    s_retained_schedule.sync_config_minutes = network_sync_interval_minutes_;
    if (!scheduler_timer_wake_ || slideshow_refresh_requested_ ||
        display_invalidation_refresh_requested_) {
        s_retained_schedule.selected_photo_index =
            rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetGallerySelectedIndex() : 0;
        s_retained_schedule.gallery_fullscreen =
            rawdraw_ui_manager_ && rawdraw_ui_manager_->IsGalleryFullscreen() ? 1 : 0;
    }
    if (rawdraw_ui_manager_) {
        StoreRetainedDisplayMetadata(
            rawdraw_ui_manager_->GetDisplayedPersistentMetadata());
    }
    if (!battery_critical_) {
        s_retained_schedule.low_battery_notice_shown = 0;
    }
    // A deadline that has already elapsed while awake is consumed here rather
    // than turned into a zero-second timer wake.
    if (slideshow_interval_minutes_ <= 0) {
        s_retained_schedule.slideshow_due_rtc_us = 0;
    } else if (SecondsUntilSlideshow() == 0) {
        ResetSlideshowDeadline();
    }
    if (network_sync_interval_minutes_ <= 0) {
        s_retained_schedule.sync_due_rtc_us = 0;
    } else if (SecondsUntilNetworkSync() == 0) {
        ResetNetworkSyncDeadline();
    }
    // Last write before the commit. Every field above must be covered: a
    // stale checksum makes the next wake discard the whole schedule.
    SealRetainedSchedule();

    int display_invalidation_seconds = 0;
    const ui::PersistentDisplayMetadata committed_metadata = RetainedDisplayMetadata();
    if (HasDateWakeDependency(committed_metadata.contract)) {
        display_invalidation_seconds =
            committed_metadata.snapshot.date_key != CurrentLocalDateKey()
                ? 1
                : SecondsUntilNextLocalMidnight();
    }
    ESP_LOGI(kTag, "Calculated display invalidation deadline: %d seconds",
             display_invalidation_seconds);

    const int slideshow_remaining = SecondsUntilSlideshow();
    const int sync_remaining = wifi_enabled_.load(std::memory_order_acquire)
                                   ? SecondsUntilNetworkSync()
                                   : -1;

    int next_wake_seconds = 0;
    auto consider_deadline = [&next_wake_seconds](int remaining) {
        if (remaining > 0 &&
            (next_wake_seconds == 0 || remaining < next_wake_seconds)) {
            next_wake_seconds = remaining;
        }
    };
    if (battery_critical_) {
        // Only a charger or a key press wakes the device now; each timer wake
        // would draw current for nothing and bring the brownout closer.
        ESP_LOGW(kTag, "Battery critical (%d%%): timer wakes suspended until charged",
                 battery_level_at_boot_);
    } else {
        consider_deadline(slideshow_remaining);
        consider_deadline(sync_remaining);
        consider_deadline(display_invalidation_seconds);
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (next_wake_seconds > 0) {
        // Nothing to decrement: the deadlines are absolute, so an early GPIO
        // wake, an overlong waveform or a 120 s interactive window all leave
        // them describing the same instants they described here.
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(next_wake_seconds) * 1000000ULL));
        ESP_LOGI(kTag,
                 "Next wake in %d seconds (slide_remaining=%d sync_remaining=%d display=%d)",
                 next_wake_seconds, slideshow_remaining, sync_remaining,
                 display_invalidation_seconds);
    } else {
        ESP_LOGI(kTag, "All schedules disabled; sleeping until GPIO wake");
    }
    // BOOT, DOWN and the charger's active-low CHARGE output can share EXT1.
    // The active-high FULL output uses EXT0 so USB insertion also wakes a
    // device whose battery is already full. Together these cover the charger
    // states without running a ULP program during deep sleep.
    constexpr uint64_t kLowLevelWakeMask =
        (1ULL << BOOT_BUTTON_GPIO) |
        (1ULL << TODO_DOWN_BUTTON_GPIO) |
        (1ULL << CHARGE_DETECT_GPIO);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        kLowLevelWakeMask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(
        static_cast<gpio_num_t>(CHARGE_FULL_GPIO), 1));

    ESP_LOGI(kTag, "Final deep-sleep commit: %s",
             pending_sleep_reason_ ? pending_sleep_reason_ : "unspecified");
    ++s_power_diagnostics.sleep_commit_count;
    // Only a boot that did its normal work proves the fault has cleared. A
    // quiet recovery's commit must not restart the count, or a wake that
    // crashes every time would be retried with a full waveform forever.
    if (!quiet_recovery_) {
        s_power_diagnostics.consecutive_unexpected_resets = 0;
    }
    s_power_diagnostics.last_photo_index =
        s_retained_schedule.selected_photo_index;
    BootRecord& record = CurrentBootRecord();
    record.awake_s = static_cast<uint16_t>(
        std::min<int64_t>(esp_timer_get_time() / 1000000, UINT16_MAX));
    record.light_sleep_s = static_cast<uint16_t>(
        std::min<uint64_t>(s_light_sleep_us / 1000000, UINT16_MAX));
    record.flags |= kBootFlagSleepCommitted;
    ESP_LOGI(kTag, "Awake %lld ms, of which %llu ms in light sleep (%lu periods)",
             static_cast<long long>(esp_timer_get_time() / 1000),
             static_cast<unsigned long long>(s_light_sleep_us / 1000),
             static_cast<unsigned long>(s_light_sleep_periods));
    SealRetained(s_power_diagnostics);
    ESP_LOGI(kTag,
             "Power lifecycle commit: boots=%u timer_wakes=%u sleep_commits=%u photo=%ld",
             static_cast<unsigned>(s_power_diagnostics.boot_count),
             static_cast<unsigned>(s_power_diagnostics.timer_wake_count),
             static_cast<unsigned>(s_power_diagnostics.sleep_commit_count),
             static_cast<long>(s_power_diagnostics.last_photo_index));
    // Shut down the rails only after all users (display, Wi-Fi, audio and NFC)
    // have become idle. Per-pin holds already configured by the board BSP are
    // then made effective throughout deep sleep on ESP32-S3.
    ZectrixPrepareForDeepSleep();
    gpio_deep_sleep_hold_en();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

void Application::OnSntpSynchronized() {
    sntp_sync_pending_.store(true, std::memory_order_release);
}

void Application::HandleSntpSynchronized() {
    time_t now = 0;
    time(&now);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    char time_buf[32] = {};
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
    ESP_LOGI(kTag, "SNTP time synchronized on main task: %s", time_buf);
    if (auto* rtc = ZectrixGetRtc()) {
        if (!rtc->SetTime(local_tm)) {
            ESP_LOGW(kTag, "SNTP succeeded but writing PCF8563 failed");
        } else {
            ESP_LOGI(kTag, "PCF8563 updated from SNTP");
        }
    }
    if (scheduler_timer_wake_) {
        // RTC access temporarily powers the shared I2C/audio rail. No scheduled
        // job uses audio or NFC, so return those rails to their low-power state
        // while the display waveform and Wi-Fi teardown finish.
        ZectrixPrepareForScheduledWake();
    }
    if (network_sync_pending_) {
        // Also after a press turned the scheduled wake interactive: the
        // session it started still has to end with the radio off.
        CompleteScheduledNetworkJob(kScheduledNetworkJobTimeSync);
    } else if (!scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake) {
        // Clock and status-bar text are monochrome. A successful sync must not
        // turn a background network event into an expensive color refresh.
        UpdateStatusBarForUi();
    }
}

void Application::Run() {
    main_task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    while (true) {
        RuntimeGuardNoteProgress(
            sleep_phase_ == SleepPhase::Awake
                ? RuntimeGuardPhase::Running
                : RuntimeGuardPhase::PreparingSleep);
        DrainScheduledCallbacks();
        if (sntp_sync_pending_.exchange(false, std::memory_order_acq_rel)) {
            HandleSntpSynchronized();
        }
        // Drained on every tick, including during sleep preparation: both of
        // these persist to NVS, and dropping one because a sleep started in
        // the same 250 ms window would lose a setting the user just changed.
        // Neither can queue a refresh while preparing -- RequestActivePageRefresh()
        // is suppressed by the UI manager, and UpdateStatusBarForUi() checks
        // sleep_phase_ itself.
        const int requested_interval =
            requested_slideshow_interval_.exchange(-1, std::memory_order_acq_rel);
        if (requested_interval >= 0) {
            ApplySlideshowInterval(requested_interval);
            if (rawdraw_ui_manager_) {
                rawdraw_ui_manager_->RequestActivePageRefresh(
                    ui::RefreshIntent::FastBwDeferredInteraction);
            }
        }
        ApplyPendingWifiDisableRequest();
        if (rawdraw_ui_manager_ && sleep_phase_ == SleepPhase::Awake) {
            if (!scheduler_timer_wake_) {
                PumpSlideshowSchedule();
            }
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        auto* lcd = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        bool display_busy = lcd != nullptr && lcd->IsRefreshPending();
        const int64_t now_us = esp_timer_get_time();

        if (restart_requested_ &&
            (!display_busy || now_us - restart_requested_at_us_ >= kRestartDisplayWaitUs)) {
            ESP_LOGI(kTag, "Restarting as requested from Settings");
            MarkBootRecord(kBootFlagRestartRequested);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
        if (restart_confirm_deadline_us_ != 0 && now_us > restart_confirm_deadline_us_) {
            restart_confirm_deadline_us_ = 0;
            auto* sr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetSettingsRenderer() : nullptr;
            if (sr && sr->UpdateItem(kSettingsRestartIndex, "执行") &&
                sleep_phase_ == SleepPhase::Awake &&
                rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
                rawdraw_ui_manager_->RequestActivePageRefresh(
                    ui::RefreshIntent::FastBwDeferredInteraction);
            }
        }

        if (!scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake &&
            now_us >= display_freshness_probe_deadline_us_) {
            display_freshness_probe_deadline_us_ = now_us + 30LL * 1000 * 1000;
            const auto date_dependencies =
                rawdraw::PersistentDependencyMask(
                    rawdraw::PersistentDisplayDependency::Date) |
                rawdraw::PersistentDependencyMask(
                    rawdraw::PersistentDisplayDependency::PageDate);
            if (!display_busy && rawdraw_ui_manager_ &&
                (rawdraw_ui_manager_->GetVisiblePersistentChanges() &
                 date_dependencies) != 0) {
                const auto refresh = rawdraw_ui_manager_->PreparePersistentFrame();
                ESP_LOGI(kTag,
                         "Awake display invalidation: refresh=%d visible=0x%lx",
                         refresh.refresh_requested ? 1 : 0,
                         static_cast<unsigned long>(refresh.visible_changes));
                display_busy = lcd != nullptr && lcd->IsRefreshPending();
            }
        }

        if (sleep_phase_ == SleepPhase::WaitingForDisplay) {
            if (!display_busy) {
                ESP_LOGI(kTag, "EPD idle after final full-color refresh");
                sleep_phase_ = SleepPhase::ReadyToCommit;
            }
        }
        if (sleep_phase_ == SleepPhase::ReadyToCommit) {
            // Committing cuts the panel supply. Re-check here rather than
            // trust the decision taken when preparation started: anything that
            // queued a waveform since then has to finish first.
            if (lcd != nullptr && lcd->IsRefreshPending()) {
                ESP_LOGW(kTag, "Sleep commit deferred: a refresh started after preparation");
                sleep_phase_ = SleepPhase::WaitingForDisplay;
            } else {
                CommitLowPowerSleep();
            }
        }

        if (!scheduler_timer_wake_ &&
            now_us >= external_power_probe_deadline_us_) {
            external_power_probe_deadline_us_ = now_us + 1000LL * 1000;
            const bool power_present = ZectrixIsExternalPowerPresent();
            if (power_present != external_power_present_) {
                ESP_LOGI(kTag, "External power %s",
                         power_present ? "connected; automatic sleep paused"
                                       : "removed; automatic sleep countdown resumed");
            }
            external_power_present_ = power_present;
            if (external_power_present_ &&
                !ShouldEnterIdleSleep(true, external_power_present_)) {
                // Continuously move the idle deadline forward. After USB is
                // removed the device gets one normal interaction window, then
                // returns to the configured deep-sleep schedule.
                interactive_sleep_deadline_us_ = now_us + kInteractiveAwakeUs;
            }
        }

        // Not gated on scheduler_timer_wake_: a press can turn a scheduled
        // wake interactive while its network session is still running, and
        // that session must still end on time.
        if (network_sync_pending_ &&
            network_sync_deadline_us_ > 0 && now_us >= network_sync_deadline_us_) {
            ESP_LOGW(kTag, "Scheduled network session timed out; stopping until next interval");
            FinishScheduledNetworkSession("timeout");
        }

        if (!scheduler_timer_wake_ && wifi_connect_deadline_us_ > 0 &&
            now_us >= wifi_connect_deadline_us_ &&
            !wifi_connected_.load(std::memory_order_acquire)) {
            ESP_LOGW(kTag, "Interactive Wi-Fi attempt timed out; radio stopped");
            wifi_connect_deadline_us_ = 0;
            if (network_initialized_) {
                WifiManager::GetInstance().StopStation();
            }
            if (rawdraw_ui_manager_) {
                const bool changed = UpdateWifiSettingsItem(
                    rawdraw_ui_manager_->GetSettingsRenderer(),
                    wifi_enabled_.load(std::memory_order_acquire), false,
                    "连接超时");
                if (changed && !InInputScope()) {
                    rawdraw_ui_manager_->RequestActivePageRefresh(
                        ui::RefreshIntent::FastBwDeferredInteraction);
                }
            }
        }

        UpdateLowPowerWait(display_busy);

        if (scheduler_timer_wake_) {
            if (!display_busy && !network_sync_pending_) {
                // A slideshow deadline that came due while this wake was busy
                // with something else (a network session started a few
                // seconds before it) is served now. The commit would otherwise
                // consume the overdue deadline and the frame skipped a whole
                // interval.
                if (!slideshow_refresh_requested_ && !battery_critical_ &&
                    rawdraw_ui_manager_ && IsSlideshowDue()) {
                    ResetSlideshowDeadline();
                    slideshow_refresh_requested_ =
                        rawdraw_ui_manager_->PrepareGallerySlideshowFrame(
                            s_retained_schedule.selected_photo_index, true,
                            s_retained_schedule.gallery_fullscreen != 0);
                    ESP_LOGI(kTag, "Slideshow came due during the wake; frame requested=%d",
                             slideshow_refresh_requested_ ? 1 : 0);
                    if (slideshow_refresh_requested_) {
                        MarkBootRecord(kBootFlagFrameRefreshed);
                        vTaskDelay(pdMS_TO_TICKS(250));
                        continue;
                    }
                }
                EnterScheduledSleep();
            }
        } else {
            const bool manual_sleep_requested =
                manual_sleep_requested_.load(std::memory_order_acquire);
            const bool config_ap_running =
                network_initialized_ && WifiManager::GetInstance().IsConfigMode();
            bool local_service_running =
                IsLocalHttpServiceRunning(rawdraw_ui_manager_.get()) || config_ap_running;
            if (local_service_running && !external_power_present_ &&
                sleep_phase_ == SleepPhase::Awake && rawdraw_ui_manager_) {
                const int64_t last_activity_us =
                    std::max(last_input_us_, rawdraw_ui_manager_->GetHttpLastActivityUs());
                const int64_t idle_us = now_us - last_activity_us;
                if (idle_us >= kLocalServiceIdleTimeoutUs) {
                    StopIdleLocalServices(idle_us);
                    local_service_running =
                        IsLocalHttpServiceRunning(rawdraw_ui_manager_.get()) ||
                        (network_initialized_ && WifiManager::GetInstance().IsConfigMode());
                }
            }
            const bool idle_deadline_reached = interactive_sleep_deadline_us_ > 0 &&
                                               now_us >= interactive_sleep_deadline_us_;
            const bool idle_sleep_allowed =
                ShouldEnterIdleSleep(idle_deadline_reached,
                                     external_power_present_) &&
                !local_service_running;
            display_busy = lcd != nullptr && lcd->IsRefreshPending();
            if ((manual_sleep_requested || idle_sleep_allowed) && !display_busy) {
                EnterLowPowerSleep(manual_sleep_requested ? "manual request"
                                                          : "interactive idle timeout");
            }
        }
        // In the low-power wait this loop only has to notice the end of the
        // waveform, and each of its wake-ups cuts a light sleep short; a key
        // press still gets through at once, because Schedule() notifies.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(low_power_wait_active_ ? 1000 : 250));
    }
}

// Every local service blocks idle sleep, and none of them had a timeout: an
// AP-transfer page opened by a one-second BOOT press, a LAN server switched on
// in Settings, or a config hotspot from the UP+DOWN combo kept the radio and
// CPU running until the battery was empty, with the slideshow stopped. On
// battery, stop whatever has seen neither a key press nor an HTTP request for
// kLocalServiceIdleTimeoutUs; the next tick's ordinary idle check then sleeps.
void Application::StopIdleLocalServices(int64_t idle_us) {
    ESP_LOGW(kTag, "Local services idle for %lld s on battery; stopping them",
             static_cast<long long>(idle_us / 1000000));
    MarkBootRecord(kBootFlagServicesTimedOut);
    if (rawdraw_ui_manager_) {
        if (rawdraw_ui_manager_->IsApTransferRunning() &&
            rawdraw_ui_manager_->IsApTransferModeRunning()) {
            rawdraw_ui_manager_->StopApTransferMode();
        } else if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
            rawdraw_ui_manager_->StopLanHttpServer();
            if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                UpdateHttpServerSettingsItem(sr, false);
            }
        } else if (rawdraw_ui_manager_->IsHttpServerRunning()) {
            // A server in neither mode (half-started) still blocks sleep.
            rawdraw_ui_manager_->StopHttpServicesForSleep();
        }
    }
    StopConfigApIfRunning();
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (!callback) return;
    {
        std::lock_guard<std::mutex> lock(scheduled_callbacks_mutex_);
        scheduled_callbacks_.push_back(std::move(callback));
    }
    if (TaskHandle_t task = main_task_.load(std::memory_order_acquire)) {
        xTaskNotifyGive(task);
    }
}

void Application::DrainScheduledCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(scheduled_callbacks_mutex_);
        callbacks.swap(scheduled_callbacks_);
    }
    for (auto& callback : callbacks) {
        if (callback) callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    auto* display = Board::GetInstance().GetDisplay();
    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    return !network_sync_pending_ &&
           !IsLocalHttpServiceRunning(rawdraw_ui_manager_.get()) &&
           (lcd == nullptr || !lcd->IsRefreshPending());
}

void Application::UpdateStatusBarForUi(bool request_refresh) {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        bool dirty = rawdraw_ui_manager_->UpdateStatusBar(data);
        auto* sr = rawdraw_ui_manager_->GetSettingsRenderer();
        // Evaluate every helper before OR-ing: || would short-circuit and skip
        // the remaining state pushes as soon as one reported a change.
        dirty |= UpdateWifiSettingsItem(
            sr, wifi_enabled_.load(std::memory_order_acquire), wifi_connected);
        dirty |= sr && sr->UpdateItem(
            kSettingsNetworkSyncIndex,
            FormatNetworkSyncLabel(
                network_sync_interval_minutes_,
                wifi_enabled_.load(std::memory_order_acquire)));
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        dirty |= UpdateLanIpSettingsItem(sr, lan_ip);
        dirty |= UpdateHttpServerSettingsItem(sr,
                                              rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                              rawdraw_ui_manager_->IsLanHttpServerRunning()
                                                  ? lan_ip
                                                  : "");
        // Two independent reasons to stay quiet, both needed:
        //   - inside a button handler, that handler repaints on its way out;
        //   - nothing rendered actually changed, so a repaint would be a
        //     pixel-identical frame (repeat Scanning/Connecting events, or a
        //     battery poll that landed on the same percentage).
        const auto visible_changes =
            rawdraw_ui_manager_->GetVisiblePersistentChanges();
        // Status changes are information someone is waiting for (Wi-Fi
        // connected, the config portal up), so they take the fast path like a
        // key press, and the idle recovery draws them in full colour once
        // things settle. Over a photo that is one waveform on the changed
        // icon: the fast refresh holds every pixel that does not change.
        if (dirty && visible_changes != 0 && request_refresh &&
            !scheduler_timer_wake_ && sleep_phase_ == SleepPhase::Awake &&
            !InInputScope()) {
            rawdraw_ui_manager_->RequestActivePageRefresh(
                ui::RefreshIntent::FastBwDeferredInteraction);
        }
    }
    return;
}
