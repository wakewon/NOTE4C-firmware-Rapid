/**
 * @file ap_transfer_renderer.cc
 * @brief AP Transfer mode renderer implementation
 */

#include "ap_transfer_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {
static const char* kTag = "ApTransferRenderer";
constexpr const char* kDefaultApIp = "192.168.4.1";

bool LooksLikeIpv4(const std::string& value) {
    int dots = 0;
    int digits = 0;
    for (char ch : value) {
        if (ch >= '0' && ch <= '9') {
            digits++;
        } else if (ch == '.') {
            dots++;
        } else {
            return false;
        }
    }
    return dots == 3 && digits >= 4;
}
}  // namespace

ApTransferRenderer::ApTransferRenderer()
    : font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim) {
    ESP_LOGI(kTag, "ApTransferRenderer created");
}

ApTransferRenderer::~ApTransferRenderer() {
    ESP_LOGI(kTag, "ApTransferRenderer destroyed");
}

void ApTransferRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = kWaitingForConnection;
    status_message_.clear();
    ESP_LOGI(kTag, "ApTransferRenderer initialized: %dx%d", width, height);
}

namespace {
// Page geometry below the 28 px status bar. The page shows two numbered steps
// (join the hotspot, open the browser), a status line and the exit hint.
constexpr int kMarginX = 12;
constexpr int kCardRadius = 10;
constexpr int kStep1Top = Style::kStatusBarHeight + 8;
constexpr int kStep1H = 88;
constexpr int kStep2Top = kStep1Top + kStep1H + 8;
constexpr int kStep2H = 64;
constexpr int kFooterH = 24;

void DottedHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color) {
    for (int x = x1; x <= x2; x += 2) set_pixel(fb, width, x, y, color);
}

// Numbered step marker: an accent disc with a paper-coloured digit. The fast
// refresh shows it as a black disc, which keeps the digit readable.
void DrawStepBadge(uint8_t* fb, int width, int cx, int cy, const char* digit,
                   const lv_font_t* font, Color accent, Color paper) {
    DrawCircle(fb, width, {cx, cy}, 10, accent);
    const int w = MeasureTextWidth(digit, font);
    DrawText(fb, width, cx - w / 2 + 1, InkCenteredTextTopY(font, digit, cy, 0), digit, font,
             paper);
}

void DrawCheckMark(uint8_t* fb, int width, int cx, int cy, Color color) {
    for (int t = -1; t <= 1; ++t) {
        DrawLine(fb, width, {cx - 10, cy + t}, {cx - 3, cy + 7 + t}, color);
        DrawLine(fb, width, {cx - 3, cy + 7 + t}, {cx + 11, cy - 7 + t}, color);
    }
}

void DrawCrossMark(uint8_t* fb, int width, int cx, int cy, Color color) {
    for (int t = -1; t <= 1; ++t) {
        DrawLine(fb, width, {cx - 8 + t, cy - 8}, {cx + 8 + t, cy + 8}, color);
        DrawLine(fb, width, {cx + 8 + t, cy - 8}, {cx - 8 + t, cy + 8}, color);
    }
}
}  // namespace

void ApTransferRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    // SetState() arrives from the HTTP and AP-start tasks while the UI task
    // renders; status_message_ is a heap-backed string once it passes the
    // small-string limit, so an unguarded read could use freed memory.
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto& theme = ThemeManager::Get();
    DrawStyledRect(fb, width, {0, 0, width, height}, theme.Style(ThemeToken::BackgroundPrimary));

    switch (state_) {
        case kWaitingForConnection:
            RenderInstructions(fb, width, height);
            break;
        default:
            RenderStatus(fb, width, height);
            break;
    }

    // Footer: how to leave, and which mode this is (the status bar title is
    // the same for transfer and Wi-Fi setup).
    const Color ink = theme.Style(ThemeToken::TextPrimary).fg;
    const int top = height - kFooterH - 2;
    DottedHLine(fb, width, top, kMarginX + 4, width - kMarginX - 4, ink);
    const char* exit_hint = exit_hint_text_.empty() ? "长按 BOOT 退出" : exit_hint_text_.c_str();
    const int hint_y = InkCenteredTextTopYInBox(font_, exit_hint, top + 1, kFooterH, 0);
    DrawText(fb, width, kMarginX + 4, hint_y, exit_hint, font_, ink, height);
    const char* mode = title_text_.empty() ? "WiFi 传图" : title_text_.c_str();
    const int mode_w = MeasureTextWidth(mode, font_);
    DrawText(fb, width, width - kMarginX - 4 - mode_w, hint_y, mode, font_, ink, height);
}

void ApTransferRenderer::RenderInstructions(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color paper = theme.Style(ThemeToken::BackgroundPrimary).bg;
    const Color ink = theme.Style(ThemeToken::TextPrimary).fg;
    const Color accent = theme.Style(ThemeToken::Accent).bg;
    const int card_w = width - kMarginX * 2;
    const int text_x = kMarginX + 40;

    // Always show the browser URL. The AP/HTTP startup callback can arrive
    // while the e-paper is busy, so relying on status_message_ made the
    // address occasionally disappear and only show "启动中...".
    const std::string ip = LooksLikeIpv4(status_message_) ? status_message_ : kDefaultApIp;
    const std::string url = url_text_.empty() ? ("http://" + ip) : url_text_;
    ESP_LOGI(kTag, "RenderInstructions ip=%s state=%d message='%s'",
             ip.c_str(), static_cast<int>(state_), status_message_.c_str());
    // The server reports its address once the hotspot and HTTP server are
    // up; until then the page says it is still starting. Every upload ends
    // back here at once, so the count is the on-device receipt for it.
    std::string state_hint;
    if (!hint_text_.empty()) {
        state_hint = hint_text_;
    } else if (!LooksLikeIpv4(status_message_)) {
        state_hint = status_message_.empty() ? "启动中，可先连接热点" : status_message_;
    } else if (received_count_ > 0) {
        state_hint = "已接收 " + std::to_string(received_count_) + " 张，可继续上传";
    } else {
        state_hint = "热点已开启，等待连接";
    }
    const std::string ssid = ssid_text_.empty() ? std::string("InkScreen-AP") : ssid_text_;
    const std::string password = password_text_.empty() ? std::string("无") : password_text_;

    // Step 1: join the hotspot. Headings use the medium face, which only
    // carries CJK and digits; Latin text (SSID, URL) stays in the regular one.
    DrawRoundRectBorder(fb, width, height, {kMarginX, kStep1Top, card_w, kStep1H}, kCardRadius, 1,
                        ink);
    const int head1_y = kStep1Top + 20;
    DrawStepBadge(fb, width, kMarginX + 20, head1_y, "1", font_, accent, paper);
    DrawText(fb, width, text_x, InkCenteredTextTopY(title_font_, "连接热点", head1_y, 0),
             "连接热点", title_font_, ink, height);
    const int row1_y = kStep1Top + 48;
    const int row2_y = kStep1Top + 70;
    const int value_x = text_x + 44;
    DrawText(fb, width, text_x, InkCenteredTextTopY(font_, "名称", row1_y, 0), "名称", font_, ink,
             height);
    DrawText(fb, width, value_x, InkCenteredTextTopY(font_, ssid.c_str(), row1_y, 0),
             ssid.c_str(), font_, ink, height);
    DrawText(fb, width, text_x, InkCenteredTextTopY(font_, "密码", row2_y, 0), "密码", font_, ink,
             height);
    DrawText(fb, width, value_x, InkCenteredTextTopY(font_, password.c_str(), row2_y, 0),
             password.c_str(), font_, ink, height);

    // Step 2: open the page. The address is the one thing to read off the
    // screen, so it gets an accent underline.
    DrawRoundRectBorder(fb, width, height, {kMarginX, kStep2Top, card_w, kStep2H}, kCardRadius, 1,
                        ink);
    const int head2_y = kStep2Top + 20;
    DrawStepBadge(fb, width, kMarginX + 20, head2_y, "2", font_, accent, paper);
    DrawText(fb, width, text_x, InkCenteredTextTopY(title_font_, "浏览器打开", head2_y, 0),
             "浏览器打开", title_font_, ink, height);
    const int url_y = kStep2Top + 46;
    const int url_w = MeasureTextWidth(url.c_str(), font_);
    DrawText(fb, width, text_x, InkCenteredTextTopY(font_, url.c_str(), url_y, 0), url.c_str(),
             font_, ink, height);
    DrawRect(fb, width, {text_x, url_y + 10, url_w, 2}, accent);

    // Status line.
    const int status_y = kStep2Top + kStep2H + 18;
    DrawCircle(fb, width, {kMarginX + 10, status_y}, 4, accent);
    DrawText(fb, width, kMarginX + 22, InkCenteredTextTopY(font_, state_hint.c_str(), status_y, 0),
             state_hint.c_str(), font_, ink, height);
}

void ApTransferRenderer::RenderStatus(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color paper = theme.Style(ThemeToken::BackgroundPrimary).bg;
    const Color ink = theme.Style(ThemeToken::TextPrimary).fg;
    const Color accent = theme.Style(ThemeToken::Accent).bg;

    const char* status_text = "";
    const char* detail_text = status_message_.empty() ? "" : status_message_.c_str();
    switch (state_) {
        case kClientConnected: status_text = "设备已连接"; break;
        case kUploading: status_text = "正在上传"; break;
        case kProcessing: status_text = "正在处理图片"; break;
        case kComplete: status_text = "传输完成"; break;
        case kError: status_text = "传输失败"; break;
        default: break;
    }

    // One large mark, the state in the medium face, the detail below it.
    const int mark_cy = Style::kStatusBarHeight + 62;
    const int cx = width / 2;
    if (state_ == kComplete) {
        DrawCircle(fb, width, {cx, mark_cy}, 24, accent);
        DrawCheckMark(fb, width, cx, mark_cy, paper);
    } else if (state_ == kError) {
        DrawCircle(fb, width, {cx, mark_cy}, 24, ink);
        DrawCrossMark(fb, width, cx, mark_cy, paper);
    } else {
        DrawCircleBorder(fb, width, {cx, mark_cy}, 24, 3, ink);
        DrawCircle(fb, width, {cx, mark_cy}, 8, accent);
    }

    const int status_y = mark_cy + 48;
    if (status_text[0] != '\0') {
        const int status_w = MeasureTextWidth(status_text, title_font_);
        DrawText(fb, width, cx - status_w / 2,
                 InkCenteredTextTopY(title_font_, status_text, status_y, 0), status_text,
                 title_font_, ink, height);
    }
    if (detail_text[0] != '\0') {
        const int detail_w = MeasureTextWidth(detail_text, font_);
        DrawText(fb, width, cx - detail_w / 2,
                 InkCenteredTextTopY(font_, detail_text, status_y + 28, 0), detail_text, font_,
                 ink, height);
    }

    if (state_ == kUploading || state_ == kProcessing) {
        // Indeterminate: the transfer reports no byte count, so the bar only
        // shows that work is in progress.
        const Rect bar{kMarginX + 40, status_y + 46, width - (kMarginX + 40) * 2, 10};
        DrawRoundRectBorder(fb, width, height, bar, 5, 1, ink);
        DrawRoundRect(fb, width, height, {bar.x + 2, bar.y + 2, bar.w / 3, bar.h - 4}, 3, accent,
                      accent, 1);
    }

    const std::string ip = LooksLikeIpv4(status_message_) ? status_message_ : kDefaultApIp;
    const std::string url = url_text_.empty() ? ("http://" + ip) : url_text_;
    const int url_w = MeasureTextWidth(url.c_str(), font_);
    const int url_y = height - kFooterH - 18;
    DrawText(fb, width, cx - url_w / 2, InkCenteredTextTopY(font_, url.c_str(), url_y, 0),
             url.c_str(), font_, ink, height);
}

bool ApTransferRenderer::HandleInput(const ButtonEvent& event) {
    ESP_LOGI(kTag, "HandleInput: type=%d", event.type);

    // AP mode is intentionally stable during slow e-paper refreshes. Only the
    // global BOOT long-press handler exits AP transfer; short clicks do nothing.
    if (event.type == ButtonEvent::kBootClick) {
        ESP_LOGI(kTag, "BOOT click ignored in AP transfer mode");
        return false;
    }

    return false;
}

void ApTransferRenderer::SetState(TransferState state, const std::string& message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state == kComplete) {
        ++received_count_;
    }
    state_ = state;
    status_message_ = message;
    ESP_LOGI(kTag, "SetState: %d, message='%s'", state, message.c_str());
}

void ApTransferRenderer::UseDefaultTransferInstructions() {
    {
        // A new session: "starting" until the server reports its address.
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = kWaitingForConnection;
        status_message_.clear();
        received_count_ = 0;
    }
    SetInstructionContent("WiFi 传图",
                          "InkScreen-AP",
                          "12345678",
                          "http://192.168.4.1",
                          "",
                          "长按 BOOT 退出");
}

void ApTransferRenderer::SetInstructionContent(const std::string& title,
                                               const std::string& ssid,
                                               const std::string& password,
                                               const std::string& url,
                                               const std::string& hint,
                                               const std::string& exit_hint) {
    title_text_ = title;
    ssid_text_ = ssid;
    password_text_ = password;
    url_text_ = url;
    hint_text_ = hint;
    exit_hint_text_ = exit_hint;
    ESP_LOGI(kTag, "SetInstructionContent title='%s' ssid='%s' url='%s'",
             title_text_.c_str(), ssid_text_.c_str(), url_text_.c_str());
}

void ApTransferRenderer::SetExitCallback(std::function<void()> callback) {
    exit_callback_ = callback;
}

}  // namespace rawdraw
