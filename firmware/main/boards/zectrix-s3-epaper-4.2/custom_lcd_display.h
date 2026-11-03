#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <stdint.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#ifdef HAVE_LVGL
#include <lvgl.h>
#endif

#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "lcd_display.h"
#include "ssd2683_fast_bw.h"

/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    uint8_t power;
    int spi_host;
    int buffer_len;
    int panel_type;
}custom_lcd_spi_t;

typedef enum {
    EPD_PANEL_1BPP = 0,
    EPD_PANEL_4COLOR_SSD2683 = 1,
} epd_panel_type_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,custom_lcd_spi_t _lcd_spi_data);
    ~CustomLcdDisplay();

    void WriteRaw1bpp(int x, int y, int w, int h, const uint8_t* data, size_t len) override;
    void InvertRegion(int x, int y, int w, int h) override;
    void DrawTexts(const std::vector<TextItem>& texts, bool clear) override;

    void EPD_Init();
    void EPD_Clear();
    void EPD_Display();

    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color);

    // Immediate refresh without forcing a full e-paper update.
    void RequestUrgentRefresh() override;
    // SSD2683 interaction path: draw a black/yellow reduction of the frame
    // with the vendor RAM waveform, then schedule a standard full-color
    // recovery after the idle timeout. (The name predates the black/yellow
    // waveform; callers and intents keep it.)
    void RequestFastBwRefresh(
        ssd2683_fast_bw::RecoveryMode recovery_mode =
            ssd2683_fast_bw::RecoveryMode::Quality);
    // The async SSD2683 path snapshots the framebuffer, so navigation can keep
    // updating/queuing the latest frame while a fast waveform is in progress.
    bool AllowsInputDuringRefresh() const;
    // Force a full e-paper refresh on the next immediate update.
    void RequestUrgentFullRefresh() override;

    // Refresh state for sleep gating.
    //
    // Everything a sleep-critical caller needs to make a decision is published
    // as a lock-free snapshot.  Neither the application sleep gate nor the
    // independent runtime guard may wait on the framebuffer mutex to decide
    // whether the panel is idle: the guard exists precisely to recover from a
    // wedged render task, and the sleep path blocking there turns a stuck
    // waveform into a 150 s stall plus a guard restart instead of a clean
    // deep sleep.
    bool IsRefreshPending() const;
    bool NeedsFullColorRecovery() const;

    // Bounded: the comparison needs the framebuffer mutex, so it can fail.
    // Reports "differs" on timeout -- the conservative answer, since the only
    // consequence is one extra full-color waveform.
    bool FramebufferDiffersFromLastRefresh(uint32_t timeout_ms = 250);

    // Bounded framebuffer access for renderers. Returns false without taking
    // the mutex if it could not be acquired in time; the caller must then skip
    // drawing rather than block.
    bool LockFramebuffer(uint32_t timeout_ms);
    void UnlockFramebuffer();

    // Notify when refresh transitions from busy to idle.
    void SetOnRefreshIdle(std::function<void()> cb);
    void SetNextKickMs(uint32_t kick_ms);

    // While set, BUSY waits poll every kLowPowerBusyPollMs instead of every
    // tick, so the system can light-sleep between polls. Only the
    // application's low-power wait window sets it.
    void SetLowPowerBusyWait(bool enabled) {
        low_power_busy_wait_.store(enabled, std::memory_order_release);
    }

private:
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    const epd_panel_type_t panel_type_;
    spi_device_handle_t spi = nullptr;
    bool spi_bus_inited = false;
    uint8_t *buffer      = nullptr;   // semantic 2bpp framebuffer
    uint8_t *prev_buffer = nullptr;   // last requested semantic frame
    uint8_t *tx_buf      = nullptr;   // snapshot buffer for async send (size = buffer_len)
    // Black/yellow fast path, 1bpp, bit set = black. fast_by_state_ is what
    // the glass holds when fast_by_cache_valid_ is set; the differential
    // waveform is only correct against the real previous state.
    uint8_t *fast_by_state_  = nullptr;
    uint8_t *fast_by_target_ = nullptr;
    std::atomic<bool> fast_by_cache_valid_{false};
    uint32_t fast_by_singles_since_pair_ = 0;
    int64_t fast_by_last_start_us_ = 0;
    uint32_t fast_by_last_waveform_ms_ = 0;

    // LVGL (only compiled when HAVE_LVGL is defined)
#ifdef HAVE_LVGL
    static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p);
#endif

    // SPI/GPIO
    void spi_gpio_init();
    void spi_port_init();
    void spi_port_rx_init();
    void read_busy();

    void set_cs_1(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,1);}
    void set_cs_0(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,0);}
    void set_dc_1(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,1);}
    void set_dc_0(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,0);}
    void set_rst_1(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,1);}
    void set_rst_0(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,0);}
    void set_scl_1(){gpio_set_level((gpio_num_t)lcd_spi_data.scl,1);}
    void set_scl_0(){gpio_set_level((gpio_num_t)lcd_spi_data.scl,0);}

    void SPI_SendByte(uint8_t data);
    uint8_t SPI_RecvByte();
    uint8_t EPD_RecvData();
    bool read_busy_until(uint32_t timeout_ms);
    void EPD_ReadBytes(uint8_t *buf, size_t len);
    void EPD_PowerOn();
    void EPD_PowerOff();
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t *buf,int len);
    void writeBytes(const uint8_t *buf, int len);

    // SSD1683 helpers
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_TurnOnDisplay();
    // Black/yellow RAM-waveform fast path (vendor epaper_refresh_demo).
    bool EPD_InitFastBy();
    bool EPD_RunFastByWaveform(const uint8_t *new_mask, bool invert_new,
                               const char *step);
    void EPD_AbortFastBy();
    bool RunFastByRefresh(const char **plan_name, uint32_t *changed_pixels);
    void BuildFastByMask(const uint8_t *semantic, uint8_t *mask) const;
    void EPD_TurnOnDisplayPart();
    void EPD_SetFullWindowAndCounter(); // ***关键：恢复全屏窗口+计数器***
    bool IsFourColorPanel() const { return panel_type_ == EPD_PANEL_4COLOR_SSD2683; }
    void EPD_DisplayFourColorTestPattern();

    // Helper functions for partial display
    void bitInterleave(unsigned char bytes1, unsigned char bytes2);
    void WRITE_WHITE_TO_HLINE();
    void WRITE_HLINE_TO_VLINE();
    void WRITE_VLINE_TO_HLINE();

    uint8_t bw_threshold    = 200;

    // -------------------------
    // Async refresh mechanism
    // -------------------------
    void start_refresh_task();
    void stop_refresh_task();
    static void refresh_task_entry(void *arg);
    void refresh_task_loop();

    SemaphoreHandle_t dirty_mutex = nullptr;
    TaskHandle_t      refresh_task = nullptr;

    Rect dirty = {0,0,0,0};
    bool pending = false;

    bool urgent_refresh = false;
    bool force_full_refresh_ = false;
    bool fast_bw_refresh_requested_ = false;
    // The recovery only has something to restore if a FAST_BW refresh actually
    // ran. Without this, any UI change small enough to be skipped still armed
    // the timer, so an idle device ran a full-color waveform every time the
    // status bar clock ticked.
    bool fast_bw_since_full_ = false;
    bool idle_full_refresh_pending_ = false;
    bool idle_full_refresh_armed_ = false;
    TickType_t idle_full_refresh_deadline_ = 0;
    ssd2683_fast_bw::RecoveryMode fast_bw_recovery_mode_ =
        ssd2683_fast_bw::RecoveryMode::Quality;
    TickType_t last_sample_tick = 0;
    int sample_interval_ms = 300; // 节流：采样间隔（可调 200~800）

    bool prev_buffer_synced = false;  // 标志：prev_buffer 是否已与屏幕同步
    bool refresh_in_progress = false;
    std::atomic<bool> refresh_pending_snapshot_{false};
    // Mirrors fast_bw_since_full_ for lock-free readers on the sleep path.
    std::atomic<bool> full_color_recovery_snapshot_{false};
    bool refresh_busy_seen_ = false;
    std::atomic<bool> low_power_busy_wait_{false};
    uint32_t next_kick_ms_ = 0;
    std::function<void()> on_refresh_idle_;

    void UpdateDisplayBusyLocked();
    void PublishRecoveryDebtLocked();
    bool CheckRefreshIdleLocked();
    void ArmIdleFullRefreshLocked(TickType_t now,
                                  ssd2683_fast_bw::RecoveryMode recovery_mode);
    void CompleteFastBwRefreshLocked(TickType_t completed_at);
    void CompleteFullColorRefreshLocked();

    // 文本渲染辅助
    void render_text_to_buffer(const char* text, int x, int y, const lv_font_t* font);

    // Rawdraw-backed drawing helpers (P0: direct framebuffer operations)
    // Acquire mutex, draw on buffer, mark dirty — NO refresh trigger (caller decides)
public:
    void RawDrawRoundRect(int x, int y, int w, int h, int radius,
                          bool filled, bool has_border);
    void RawDrawHLine(int y, int thickness);
    void RawInvertRegion(int x, int y, int w, int h);
    uint8_t* GetFramebuffer() { return buffer; }
    int GetFBWidth() const { return Width; }
    int GetFBHeight() const { return Height; }
    SemaphoreHandle_t GetMutex() { return dirty_mutex; }
    bool DisplayRaw4ColorImage(const uint8_t* data, size_t len, int width, int height) override;
};

#endif // __CUSTOM_LCD_DISPLAY_H__
