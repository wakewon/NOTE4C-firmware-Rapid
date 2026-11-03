/**
 * @file settings_renderer.cc
 * @brief Modernized settings page renderer implementation - v3.8.0 visual fix
 *
 * F1 FIXES:
 * - 文字和横线不重叠：横线 y = item_y + item_h - 1，文字在 item 中央偏上
 * - 选中效果：黑色背景矩形 + 白色文字
 * - 版本号从 PROJECT_VER 宏获取，不硬编码
 * - WiFi 图标使用正确码位 "\xee\xa4\x80"/"\xee\xa4\x81"
 *
 * Design for 400x300 1bpp ePaper:
 * - Title bar with subtle border
 * - Card-based rows with borders and padding
 * - Icon (left) + Label (middle) + Value/Indicator (right)
 * - Checkbox for toggleable items
 * - Selected items have inverted colors
 * - No section headers in visual layout
 */

#include "settings_renderer.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"
#include "rawdraw/font_engine.h"
#include <esp_timer.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

// Version from CMakeLists.txt PROJECT_VER
#ifndef PROJECT_VER
#define PROJECT_VER "3.8.0"
#endif
#ifndef IDF_VER
#define IDF_VER "unknown"
#endif

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t fa_settings_16;

namespace rawdraw {

namespace {

// ---------------------------------------------------------------------------
// Settings page layout (400x300): status bar, a tab strip with one tab per
// section, full-width option rows, and a key-hint footer. One source of truth
// for the geometry; the dialogs below keep their own.
// ---------------------------------------------------------------------------
constexpr int kPageMarginX = 12;
constexpr int kTabTop = Style::kStatusBarHeight + 4;
constexpr int kTabH = 32;
constexpr int kTabRuleY = kTabTop + kTabH;      // hairline under the tabs
constexpr int kListTop = kTabRuleY + 7;
constexpr int kRowH = 44;
constexpr int kRowRadius = 10;
constexpr int kInfoRowH = 30;
constexpr int kFooterH = 24;
constexpr int kToggleW = 40;
constexpr int kToggleH = 22;
constexpr int kVolumeDialogClearPad = 0;    // Clear mask tightly follows dialog border
constexpr int kAboutRowHeight = 24;         // About dialog info row height
constexpr int kAboutRowGap = 8;             // About dialog row spacing
constexpr int kDialogClearPad = 0;          // Local clear pad around modal dialogs
constexpr int kDialogClearRadiusBoost = 0;  // Clear radius equals dialog radius
constexpr int kCategoryHintDurationUs = 2 * 1000 * 1000;

// Fontawesome glyphs present in fa_settings_16. Spelled out rather than taken
// from fa_settings.h, whose FA_SETTINGS_LINK encodes U+F301 instead of U+F0C1.
constexpr const char* kFaGear = "\xef\x80\x93";       // U+F013
constexpr const char* kFaImage = "\xef\x80\xbe";      // U+F03E
constexpr const char* kFaWifi = "\xef\x87\xab";       // U+F1EB
constexpr const char* kFaInfo = "\xef\x81\x9a";       // U+F05A
constexpr const char* kFaPower = "\xef\x80\x91";      // U+F011
constexpr const char* kFaClock = "\xef\x80\x97";      // U+F017
constexpr const char* kFaSync = "\xef\x80\xa1";       // U+F021
constexpr const char* kFaServer = "\xef\x88\xb3";     // U+F233
constexpr const char* kFaMarker = "\xef\x81\x81";     // U+F041
constexpr const char* kFaVolume = "\xef\x80\xa8";     // U+F028
constexpr const char* kFaBattery = "\xef\x89\x80";    // U+F240
constexpr const char* kFaDatabase = "\xef\x87\x80";   // U+F1C0
constexpr const char* kFaCalendar = "\xef\x81\xb3";   // U+F073
constexpr const char* kFaComment = "\xef\x81\xb5";    // U+F075
constexpr const char* kFaWrench = "\xef\x82\xad";     // U+F0AD

const char* SettingsIconFor(const std::string& label) {
    struct Entry { const char* label; const char* icon; };
    static const Entry kIcons[] = {
        {"系统", kFaGear},       {"相册", kFaImage},      {"网络", kFaWifi},
        {"关于", kFaInfo},       {"重启", kFaPower},      {"关机", kFaPower},
        {"轮播间隔", kFaClock},  {"定时联网", kFaSync},   {"Wi-Fi", kFaWifi},
        {"局域网服务", kFaServer}, {"局域网IP", kFaMarker}, {"固件", kFaInfo},
        {"音量", kFaVolume},     {"电池方向", kFaBattery}, {"存储", kFaDatabase},
        {"存储空间", kFaDatabase}, {"日期格式", kFaCalendar}, {"同步间隔", kFaSync},
        {"AI对话长度", kFaComment}, {"服务", kFaServer},   {"服务地址", kFaMarker},
        {"功能", kFaWrench},     {"时钟显示", kFaClock},
    };
    for (const auto& e : kIcons) {
        if (label == e.label) return e.icon;
    }
    return kFaInfo;
}

bool IsDangerAction(const std::string& label) {
    return label == "重启" || label == "关机";
}

// Interactive pages are yellow paper; display pages white. Semantic ink is
// whatever part of the style differs from the paper.
Color PaperColor() {
    return ThemeManager::Get().Style(ThemeToken::BackgroundPrimary).bg;
}

Color TokenInkOnPaper(ThemeToken token) {
    const PaintStyle style = ThemeManager::Get().Style(token);
    const Color paper = PaperColor();
    if (style.bg != paper) return style.bg;
    if (style.border != paper) return style.border;
    return style.fg;
}

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    static const std::string kEllipsis = "...";
    const int ellipsis_w = MeasureTextWidth(kEllipsis.c_str(), font);
    if (ellipsis_w >= max_width) return "";

    std::string fitted;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        std::string next = fitted;
        next.append(start, p - start);
        next += kEllipsis;
        if (MeasureTextWidth(next.c_str(), font) > max_width) break;
        fitted.append(start, p - start);
    }

    if (fitted.empty()) return "";
    fitted += kEllipsis;
    return fitted;
}

void ClearDialogRegionRounded(uint8_t* fb, int width, int height,
                              int x, int y, int w, int h, int radius, int pad) {
    DrawStyledRoundRect(fb, width, height,
                        {x - pad, y - pad, w + pad * 2, h + pad * 2},
                        std::max(0, radius + kDialogClearRadiusBoost),
                        ThemeManager::Get().Style(ThemeToken::BackgroundPrimary));
}

void DrawIconCentered(uint8_t* fb, int width, int x, int center_y, const char* icon,
                      Color color) {
    DrawText(fb, width, x, InkCenteredTextTopY(&fa_settings_16, icon, center_y, 0), icon,
             &fa_settings_16, color);
}

// Every other pixel: a separator that reads as a hairline rather than a rule.
void DrawDottedHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color) {
    for (int x = x1; x <= x2; x += 2) {
        set_pixel(fb, width, x, y, color);
    }
}

// Toggle switch. ON fills the track with the accent; on a selected (ink) row
// the track keeps a paper-coloured outline so it stays readable when the fast
// refresh draws the accent as black on the black row.
void DrawToggle(uint8_t* fb, int width, int height, int x, int center_y, bool on,
                Color ink, Color row_bg, Color accent) {
    const Rect track{x, center_y - kToggleH / 2, kToggleW, kToggleH};
    const int radius = kToggleH / 2;
    const int knob_r = radius - 4;
    if (on) {
        DrawRoundRect(fb, width, height, track, radius, accent,
                      row_bg == PaperColor() ? accent : ink, 2);
        DrawCircle(fb, width, {track.x + track.w - radius, center_y}, knob_r,
                   row_bg == PaperColor() ? row_bg : ink);
    } else {
        DrawRoundRect(fb, width, height, track, radius, row_bg, ink, 2);
        DrawCircle(fb, width, {track.x + radius, center_y}, knob_r, ink);
    }
}

// Two-pixel chevron, 5 px wide and 9 px tall, with its tip at (x + 4, cy).
void DrawChevronMark(uint8_t* fb, int width, int x, int center_y, Color color) {
    for (int t = 0; t < 2; ++t) {
        DrawLine(fb, width, {x + t, center_y - 4}, {x + 4 + t, center_y}, color);
        DrawLine(fb, width, {x + 4 + t, center_y}, {x + t, center_y + 4}, color);
    }
}

void DrawKeyHints(uint8_t* fb, int width, int height, const lv_font_t* font,
                  const char* left, const char* right, Color ink) {
    const int top = height - kFooterH - 2;
    DrawDottedHLine(fb, width, top, kPageMarginX + 4, width - kPageMarginX - 4, ink);
    const int y = InkCenteredTextTopYInBox(font, left, top + 1, kFooterH, 0);
    DrawText(fb, width, kPageMarginX + 4, y, left, font, ink, height);
    if (right && right[0]) {
        const int rw = MeasureTextWidth(right, font);
        DrawText(fb, width, width - kPageMarginX - 4 - rw, y, right, font, ink, height);
    }
}

}  // namespace

SettingsRenderer::SettingsRenderer()
    : selected_index_(0)
    , scroll_offset_(0)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , icon_font_(&fa_settings_16)
    , value_font_(&SourceHanSansSC_Regular_slim) {
}

SettingsRenderer::~SettingsRenderer() {}

void SettingsRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    scroll_offset_ = 0;
    first_visible_index_ = 0;
    showing_debug_info_ = false;
    debug_hint_until_us_ = 0;
    ShowCategoryHint();
    // F1: Use PROJECT_VER macro, not hardcoded version
    if (firmware_version_.empty()) {
        firmware_version_ = "v" PROJECT_VER;
    }
}

void SettingsRenderer::ShowCategoryHint(int duration_ms) {
    const int64_t duration_us = duration_ms > 0
        ? static_cast<int64_t>(duration_ms) * 1000
        : kCategoryHintDurationUs;
    category_hint_until_us_ = esp_timer_get_time() + duration_us;
    needs_full_refresh_ = true;
}

bool SettingsRenderer::IsCategoryHintVisible() const {
    return esp_timer_get_time() < category_hint_until_us_;
}

int SettingsRenderer::CalcItemHeight(const SettingsItemDef& item) const {
    if (item.type == SettingsItemType::Section) {
        return title_font_->line_height + Style::kSpacingSM * 2;
    }
    // Minimum height for comfortable touch targets
    int h = font_->line_height + kItemPadding * 2;
    if (h < kItemMinHeight) h = kItemMinHeight;
    return h;
}

int SettingsRenderer::CalcTotalContentHeight() const {
    int total = 0;
    for (const auto& item : items_) {
        total += CalcItemHeight(item);
    }
    return total;
}

int SettingsRenderer::GetFirstSelectableIndex() const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].type != SettingsItemType::Section) return i;
    }
    return 0;
}

int SettingsRenderer::GetLastSelectableIndex() const {
    for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
        if (items_[i].type != SettingsItemType::Section) return i;
    }
    return 0;
}

int SettingsRenderer::FindPrevSelectable(int index) const {
    for (int i = index - 1; i >= 0; --i) {
        if (items_[i].type != SettingsItemType::Section) return i;
    }
    return index;
}

int SettingsRenderer::FindNextSelectable(int index) const {
    for (int i = index + 1; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].type != SettingsItemType::Section) return i;
    }
    return index;
}

void SettingsRenderer::EnsureSelectionVisible() {
    if (items_.empty()) {
        first_visible_index_ = 0;
        scroll_offset_ = 0;
        return;
    }

    if (items_[selected_index_].type == SettingsItemType::Section) {
        selected_index_ = GetFirstSelectableIndex();
    }

    if (first_visible_index_ < 0 || first_visible_index_ >= static_cast<int>(items_.size()) ||
        items_[first_visible_index_].type == SettingsItemType::Section) {
        first_visible_index_ = selected_index_;
    }

    int visible_count = 0;
    bool selection_visible = false;
    for (int i = first_visible_index_; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].type == SettingsItemType::Section) continue;
        if (i == selected_index_) selection_visible = true;
        visible_count++;
        if (visible_count >= kVisibleOptionCount) break;
    }

    while (!selection_visible) {
        if (selected_index_ < first_visible_index_) {
            first_visible_index_ = selected_index_;
        } else {
            int next = FindNextSelectable(first_visible_index_);
            if (next == first_visible_index_) break;
            first_visible_index_ = next;
        }

        visible_count = 0;
        selection_visible = false;
        for (int i = first_visible_index_; i < static_cast<int>(items_.size()); ++i) {
            if (items_[i].type == SettingsItemType::Section) continue;
            if (i == selected_index_) selection_visible = true;
            visible_count++;
            if (visible_count >= kVisibleOptionCount) break;
        }
    }

    scroll_offset_ = 0;
}

void SettingsRenderer::DrawSelectedBackground(uint8_t* fb, int width, int x, int y, int w, int h) const {
    if (!fb || w <= 0 || h <= 0) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const PaintStyle soft_style = theme.Style(ThemeToken::BackgroundSecondary);
    const bool sidebar_item = w <= Style::kSettingsSidebarWidth + 2;
    if (sidebar_item) {
        DrawStyledRect(fb, width, {x, y, w, h}, soft_style);
        for (int py = y + 2; py < y + h - 2; py += 4) {
            for (int px = x + 2; px < x + w - 2; px += 4) {
                set_pixel(fb, width, px, py, selected_style.border);
            }
        }
        DrawRectBorder(fb, width, {x, y, w, h}, 1, selected_style.border);
        return;
    }

    DrawStyledRoundRect(fb, width, 300, {x, y, w, h}, Style::kSettingsCardRadius, soft_style);
    const int marker_x = x + 6;
    const int marker_y = y + 4;
    const int marker_w = w - 12;
    const int marker_h = std::max(10, h - 8);
    for (int py = marker_y + 2; py < marker_y + marker_h - 1; py += 4) {
        for (int px = marker_x + 2; px < marker_x + marker_w - 1; px += 6) {
            set_pixel(fb, width, px, py, selected_style.border);
        }
    }
}

void SettingsRenderer::ShowVolumeDialog(int volume) {
    volume_dialog_value_ = std::clamp(volume, 0, 100);
    showing_volume_dialog_ = true;
    needs_full_refresh_ = true;
}

void SettingsRenderer::UpdateVolumeValue(int delta, bool commit) {
    if (commit) {
        showing_volume_dialog_ = false;
    } else {
        volume_dialog_value_ = std::clamp(volume_dialog_value_ + delta, 0, 100);
    }
    if (volume_dialog_handler_) {
        volume_dialog_handler_(volume_dialog_value_, commit);
    }
    needs_full_refresh_ = true;
}

void SettingsRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    EnsureSelectionVisible();
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color ink = theme.Style(ThemeToken::TextPrimary).fg;
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    const int body_top = Style::kStatusBarHeight;

    DrawStyledRect(fb, width, {0, body_top, width, height - body_top}, bg_style);

    std::vector<int> section_indices;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].type == SettingsItemType::Section) section_indices.push_back(i);
    }
    if (section_indices.empty()) section_indices.push_back(-1);

    int current_section_pos = 0;
    for (int i = 0; i < static_cast<int>(section_indices.size()); ++i) {
        if (section_indices[i] <= selected_index_) current_section_pos = i;
    }
    auto section_label = [&](int pos) -> const char* {
        return section_indices[pos] >= 0 ? items_[section_indices[pos]].label.c_str() : "系统";
    };
    const char* current_section = section_label(current_section_pos);

    // --- Tab strip: icon + label per section, the current one underlined in
    // the accent. The underline turns black in the fast preview, which still
    // marks the tab; there is no filled tab, so the strip stays light.
    const int tab_count = static_cast<int>(section_indices.size());
    const int strip_w = width - kPageMarginX * 2;
    const int tab_w = strip_w / std::max(1, tab_count);
    const int tab_center_y = kTabTop + kTabH / 2 - 1;
    for (int i = 0; i < tab_count; ++i) {
        const char* label = section_label(i);
        const char* icon = SettingsIconFor(label);
        const int icon_w = MeasureTextWidth(icon, &fa_settings_16);
        const int label_w = MeasureTextWidth(label, font_);
        const int content_w = icon_w + 6 + label_w;
        const int tab_x = kPageMarginX + i * tab_w;
        const int cx = tab_x + (tab_w - content_w) / 2;
        DrawIconCentered(fb, width, cx, tab_center_y, icon, ink);
        DrawText(fb, width, cx + icon_w + 6,
                 InkCenteredTextTopY(font_, label, tab_center_y, 0), label, font_, ink);
        if (i == current_section_pos) {
            DrawRect(fb, width, {cx - 4, kTabRuleY - 3, content_w + 8, 4}, accent);
        }
    }
    DrawHLine(fb, width, kTabRuleY, kPageMarginX, width - kPageMarginX - 1, ink);

    const int section_start = section_indices[current_section_pos] + 1;
    const int section_end = (current_section_pos + 1 < tab_count)
        ? section_indices[current_section_pos + 1]
        : static_cast<int>(items_.size());
    std::vector<int> option_indices;
    for (int i = section_start; i < section_end; ++i) {
        if (items_[i].type != SettingsItemType::Section) option_indices.push_back(i);
    }

    const int list_bottom = height - kFooterH - 6;
    const bool about_section = std::string(current_section) == "关于";
    if (about_section) {
        const std::string version = firmware_version_.empty() ? "未知" : firmware_version_;
        const std::string serial = mac_address_.empty() ? "未读取" : mac_address_;
        int y = kListTop;
        struct InfoRow {
            const char* label;
            std::string value;
        };
        const InfoRow rows[] = {
            {"设备名称", "notellm"},
            {"型号", "Youn-Beta1.0"},
            {"固件版本", version},
            {"硬件版本", chip_model_.empty() ? "ESP32-S3" : chip_model_},
            {"序列号", serial},
            {"官方网站", "blog.lazyyoun.xyz"},
        };
        bool first = true;
        for (const auto& row : rows) {
            if (y + kInfoRowH > list_bottom) break;
            if (!first) {
                DrawDottedHLine(fb, width, y, kPageMarginX + 4, width - kPageMarginX - 4, ink);
            }
            first = false;
            const int center_y = y + kInfoRowH / 2;
            DrawText(fb, width, kPageMarginX + 4,
                     InkCenteredTextTopY(font_, row.label, center_y, 0), row.label, font_, ink);
            const std::string value = FitTextToWidth(row.value, value_font_, width / 2);
            const int value_w = MeasureTextWidth(value.c_str(), value_font_);
            DrawText(fb, width, width - kPageMarginX - 4 - value_w,
                     InkCenteredTextTopY(value_font_, value.c_str(), center_y, 0),
                     value.c_str(), value_font_, ink);
            y += kInfoRowH;
        }
    } else {
        int selected_pos = 0;
        for (int i = 0; i < static_cast<int>(option_indices.size()); ++i) {
            if (option_indices[i] == selected_index_) selected_pos = i;
        }
        const int total = static_cast<int>(option_indices.size());
        const int visible_count = std::min(kVisibleOptionCount, total);
        int window_start = std::max(0, selected_pos - visible_count / 2);
        if (window_start + visible_count > total) {
            window_start = std::max(0, total - visible_count);
        }
        int y = kListTop;
        for (int i = 0; i < visible_count; ++i) {
            const int item_index = option_indices[window_start + i];
            const bool selected = item_index == selected_index_;
            const bool next_selected = (i + 1 < visible_count) &&
                                       option_indices[window_start + i + 1] == selected_index_;
            RenderItem(fb, width, y, kPageMarginX, item_index, selected, kRowH);
            // Separators only between two unselected rows: the cursor pill
            // carries its own edge.
            if (i + 1 < visible_count && !selected && !next_selected) {
                DrawDottedHLine(fb, width, y + kRowH, kPageMarginX + 42,
                                width - kPageMarginX - 4, ink);
            }
            y += kRowH;
        }

        if (total > visible_count) {
            const int track_x = width - 5;
            const int track_h = kRowH * visible_count - 8;
            const int thumb_h = std::max(12, track_h * visible_count / total);
            const int max_start = std::max(1, total - visible_count);
            const int thumb_y = kListTop + 4 + (track_h - thumb_h) * window_start / max_start;
            DrawRect(fb, width, {track_x - 1, thumb_y, 3, thumb_h}, selected_style.bg);
        }
    }

    char page_counter[24];
    snprintf(page_counter, sizeof(page_counter), "%d/%d", current_section_pos + 1, tab_count);
    DrawKeyHints(fb, width, height, font_, "UP/DN 选择   BOOT 确认   长按UP 返回",
                 page_counter, ink);

    const int64_t now = esp_timer_get_time();

    // === About dialog overlay (if active) ===
    if (showing_about_dialog_) {
        RenderAboutDialog(fb, width, height);
    }
    if (showing_volume_dialog_) {
        RenderVolumeDialog(fb, width, height);
    }
    if (showing_storage_dialog_) {
        RenderStorageDialog(fb, width, height);
    }
    if (showing_server_dialog_) {
        RenderServerDialog(fb, width, height);
    }
    if (showing_server_list_dialog_) {
        RenderServerListDialog(fb, width, height);
    }
    if (showing_theme_dialog_) {
        RenderThemeDialog(fb, width, height);
    }
    if (showing_ota_dialog_) {
        RenderOtaDialog(fb, width, height);
        if (showing_ota_confirm_dialog_) {
            RenderOtaConfirmDialog(fb, width, height);
        }
    }
    // === Debug info hint (transient overlay, 3s auto-dismiss) ===
    if (showing_debug_info_ && now < debug_hint_until_us_) {
        const int hint_y = Style::kStatusBarHeight + 2;
        const int hint_h = font_->line_height + Style::kSpacingXS * 2;
        const char* hint_text = "调试信息已显示";
        int hint_w = MeasureTextWidth(hint_text, font_);
        int hint_x = (width - hint_w) / 2;

        const PaintStyle hint_style = ThemeManager::Get().Style(ThemeToken::Badge);
        DrawStyledRoundRect(fb, width, height,
                            {hint_x - Style::kSpacingSM, hint_y, hint_w + Style::kSpacingSM * 2, hint_h},
                            Style::kBorderRadiusSM, hint_style);
        DrawText(fb, width, hint_x, hint_y + Style::kSpacingXS, hint_text, font_, hint_style.fg, height);
    } else if (showing_debug_info_) {
        showing_debug_info_ = false;
        needs_full_refresh_ = true;
    }

    needs_full_refresh_ = false;
}

void SettingsRenderer::RenderItem(uint8_t* fb, int width, int y,
                                   int content_left, int index, bool selected,
                                   int row_h) {
    const SettingsItemDef& item = items_[index];
    const auto& theme = ThemeManager::Get();
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color paper = PaperColor();
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    // The cursor is the page's one solid element: an ink pill with paper-
    // coloured content and a short accent stripe. The fast refresh shows the
    // stripe black on black, which loses nothing.
    const Color row_bg = selected ? selected_style.bg : paper;
    const Color fg = selected ? selected_style.fg : theme.Style(ThemeToken::TextPrimary).fg;
    const int left = content_left;
    const int right = width - content_left;
    const int center_y = y + row_h / 2;

    if (selected) {
        DrawRoundRect(fb, width, height_, {left, y + 2, right - left, row_h - 4}, kRowRadius,
                      row_bg, row_bg, 1);
        DrawRect(fb, width, {left + 6, center_y - 8, 3, 16}, selected_style.border);
    }

    const int icon_x = left + 16;
    DrawIconCentered(fb, width, icon_x, center_y, SettingsIconFor(item.label), fg);
    const int label_x = icon_x + 26;
    int value_right = right - 14;

    if (item.type == SettingsItemType::Checkbox) {
        const int toggle_x = value_right - kToggleW;
        DrawToggle(fb, width, height_, toggle_x, center_y, item.checked, fg, row_bg, accent);
        value_right = toggle_x - 10;
    } else if (item.type == SettingsItemType::Action && !IsDangerAction(item.label)) {
        // Something BOOT changes: a chevron after the current value.
        DrawChevronMark(fb, width, value_right - 5, center_y, selected ? fg : accent);
        value_right -= 14;
    }

    const int label_w = MeasureTextWidth(item.label.c_str(), font_);
    const int value_max_w = std::max(0, value_right - (label_x + label_w + 12));
    std::string value = item.value;
    if (item.type == SettingsItemType::Action && value.empty()) value = "执行";
    value = FitTextToWidth(value, value_font_, value_max_w);
    if (!value.empty()) {
        const int value_w = MeasureTextWidth(value.c_str(), value_font_);
        if (item.type == SettingsItemType::Action && IsDangerAction(item.label)) {
            // Destructive actions read as a button: an accent chip.
            const int chip_h = 24;
            const Rect chip{value_right - value_w - 20, center_y - chip_h / 2, value_w + 20, chip_h};
            DrawRoundRect(fb, width, height_, chip, chip_h / 2, accent,
                          selected ? fg : accent, 1);
            DrawText(fb, width, chip.x + 10,
                     InkCenteredTextTopY(value_font_, value.c_str(), center_y, 0),
                     value.c_str(), value_font_, selected ? fg : paper);
        } else {
            DrawText(fb, width, value_right - value_w,
                     InkCenteredTextTopY(value_font_, value.c_str(), center_y, 0),
                     value.c_str(), value_font_, fg);
        }
    }

    const std::string label = FitTextToWidth(item.label, font_, std::max(0, value_right - label_x));
    if (!label.empty()) {
        DrawText(fb, width, label_x, InkCenteredTextTopY(font_, label.c_str(), center_y, 0),
                 label.c_str(), font_, fg);
    }
}

void SettingsRenderer::DrawChevron(uint8_t* fb, int width, int x, int y,
                                    int size, Color color) {
    // Simple ">" shape drawn as lines
    const int mid_y = y + size / 2;
    const int tip_x = x + size;
    const int base_x = x;

    // Top line: base-left to tip-center
    Point p1 = {base_x, y};
    Point p2 = {tip_x, mid_y};
    DrawLine(fb, width, p1, p2, color);

    // Bottom line: tip-center to base-left+offset
    Point p3 = {tip_x, mid_y};
    Point p4 = {base_x, y + size - 1};
    DrawLine(fb, width, p3, p4, color);
}

const char* SettingsRenderer::GetCheckboxIcon(bool checked) const {
    if (checked) {
        return "\xee\xa4\x8a";  // icon-checkboxok (e90a)
    } else {
        return "\xee\xa4\x91";  // icon-checkbox (e911)
    }
}

bool SettingsRenderer::HandleInput(const ButtonEvent& event) {
    if (items_.empty()) return false;

    if (showing_volume_dialog_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
                UpdateVolumeValue(10, false);
                return true;
            case ButtonEvent::kDownClick:
                UpdateVolumeValue(-10, false);
                return true;
            case ButtonEvent::kUpLongPress:
                volume_dialog_value_ = 100;
                UpdateVolumeValue(0, false);
                return true;
            case ButtonEvent::kDownLongPress:
                volume_dialog_value_ = 0;
                UpdateVolumeValue(0, false);
                return true;
            case ButtonEvent::kBootClick:
                UpdateVolumeValue(0, true);
                return true;
            default:
                return true;
        }
    }

    // About dialog: intercept all input when dialog is showing
    if (showing_about_dialog_) {
        switch (event.type) {
            case ButtonEvent::kBootClick:
            case ButtonEvent::kUpClick:
            case ButtonEvent::kUpLongPress:
            case ButtonEvent::kDownClick:
            case ButtonEvent::kDownLongPress:
                // Close about dialog
                showing_about_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;  // Consume all other input while dialog is open
        }
    }

    // Storage dialog: intercept all input
    if (showing_storage_dialog_) {
        switch (event.type) {
            case ButtonEvent::kBootClick:
            case ButtonEvent::kUpClick:
            case ButtonEvent::kUpLongPress:
            case ButtonEvent::kDownClick:
            case ButtonEvent::kDownLongPress:
                showing_storage_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    if (showing_theme_dialog_) {
        const int total = ThemeManager::ThemeCount();
        switch (event.type) {
            case ButtonEvent::kUpClick:
                if (theme_selected_ > 0) {
                    theme_selected_--;
                    needs_full_refresh_ = true;
                }
                return true;
            case ButtonEvent::kDownClick:
                if (theme_selected_ < total - 1) {
                    theme_selected_++;
                    needs_full_refresh_ = true;
                }
                return true;
            case ButtonEvent::kBootClick:
                if (theme_dialog_handler_) {
                    theme_dialog_handler_(ThemeManager::ThemeAt(theme_selected_));
                }
                showing_theme_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootLongPress:
                showing_theme_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    // Server list dialog: UP/DN scroll, BOOT select
    if (showing_server_list_dialog_) {
        const int total = static_cast<int>(server_list_addresses_.size());
        if (total == 0) {
            showing_server_list_dialog_ = false;
            needs_full_refresh_ = true;
            return true;
        }
        switch (event.type) {
            case ButtonEvent::kUpClick:
                if (server_list_selected_ > 0) {
                    server_list_selected_--;
                    // Scroll up if needed
                    if (server_list_selected_ < server_list_scroll_offset_) {
                        server_list_scroll_offset_ = server_list_selected_;
                    }
                    needs_full_refresh_ = true;
                }
                return true;
            case ButtonEvent::kDownClick:
                if (server_list_selected_ < total - 1) {
                    server_list_selected_++;
                    // Scroll down if needed
                    if (server_list_selected_ >= server_list_scroll_offset_ + kServerListVisibleRows) {
                        server_list_scroll_offset_ = server_list_selected_ - kServerListVisibleRows + 1;
                    }
                    needs_full_refresh_ = true;
                }
                return true;
            case ButtonEvent::kBootClick:
                if (server_list_dialog_handler_ && !server_list_addresses_.empty()) {
                    server_list_dialog_handler_(server_list_addresses_[server_list_selected_]);
                }
                showing_server_list_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootLongPress:
                // Cancel
                showing_server_list_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    // Server dialog: UP/DN switch between local/remote, BOOT confirm
    if (showing_server_dialog_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
            case ButtonEvent::kDownClick:
                server_selected_ = (server_selected_ == 0) ? 1 : 0;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                if (server_dialog_handler_) {
                    server_dialog_handler_(server_selected_);
                }
                showing_server_dialog_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    // OTA dialog: UP/DN select version, BOOT confirm (弹出确认框), long BOOT cancel/close.
    if (showing_ota_dialog_) {
        // 先处理确认弹窗的输入
        if (showing_ota_confirm_dialog_) {
            switch (event.type) {
                case ButtonEvent::kUpClick:
                case ButtonEvent::kDownClick:
                    ota_confirm_selected_ = (ota_confirm_selected_ == 0) ? 1 : 0;
                    needs_full_refresh_ = true;
                    return true;
                case ButtonEvent::kBootClick:
                    showing_ota_confirm_dialog_ = false;
                    if (ota_confirm_selected_ == 0) {
                        // 确认更新
                        if (ota_dialog_handler_) ota_dialog_handler_(0, true, false);
                    }
                    needs_full_refresh_ = true;
                    return true;
                case ButtonEvent::kBootLongPress:
                case ButtonEvent::kUpLongPress:
                case ButtonEvent::kDownLongPress:
                    // 长按取消确认弹窗，回到版本选择
                    showing_ota_confirm_dialog_ = false;
                    needs_full_refresh_ = true;
                    return true;
                default:
                    return true;
            }
        }

        switch (event.type) {
            case ButtonEvent::kUpClick:
                if (ota_dialog_handler_) ota_dialog_handler_(-1, false, false);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kDownClick:
                if (ota_dialog_handler_) ota_dialog_handler_(1, false, false);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                // 选择固件后点击，弹出确认弹窗而不是直接更新
                if (ota_state_ == 2 && !ota_versions_.empty() && ota_selected_index_ >= 0) {
                    ota_confirm_firmware_name_ = ota_versions_[ota_selected_index_];
                    ota_confirm_selected_ = 0;  // 默认确认
                    showing_ota_confirm_dialog_ = true;
                } else if (ota_dialog_handler_) {
                    ota_dialog_handler_(0, true, false);
                }
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootLongPress:
            case ButtonEvent::kUpLongPress:
            case ButtonEvent::kDownLongPress:
                if (ota_dialog_handler_) ota_dialog_handler_(0, false, true);
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }


    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_ = FindPrevSelectable(selected_index_);
            } else {
                selected_index_ = GetLastSelectableIndex();
            }
            EnsureSelectionVisible();
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kDownClick: {
            const int n = static_cast<int>(items_.size());
            if (selected_index_ < n - 1) {
                selected_index_ = FindNextSelectable(selected_index_);
            } else {
                selected_index_ = GetFirstSelectableIndex();  // Wrap to first
            }
            EnsureSelectionVisible();
            needs_full_refresh_ = true;
            return true;
        }

        case ButtonEvent::kUpLongPress:
            // Scroll to top
            scroll_offset_ = 0;
            selected_index_ = GetFirstSelectableIndex();
            first_visible_index_ = selected_index_;
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootClick:
            // Toggle checkbox, trigger action, or navigate
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(items_.size())) {
                SettingsItemDef& item = items_[selected_index_];
                if (item.type == SettingsItemType::Checkbox) {
                    item.checked = !item.checked;
                    if (item.on_click) {
                        item.on_click();
                    }
                    needs_full_refresh_ = true;
                    return true;
                } else if (item.type == SettingsItemType::Action || item.on_click) {
                    if (item.on_click) {
                        item.on_click();
                    }
                    return true;
                }
            }
            break;

        case ButtonEvent::kDownLongPress: {
            // Scroll to bottom
            selected_index_ = GetLastSelectableIndex();
            first_visible_index_ = selected_index_;
            for (int shown = 1; shown < kVisibleOptionCount; ++shown) {
                int prev = FindPrevSelectable(first_visible_index_);
                if (prev == first_visible_index_) break;
                first_visible_index_ = prev;
            }
            EnsureSelectionVisible();
            needs_full_refresh_ = true;
            return true;
        }

        default:
            break;
    }

    return false;
}

void SettingsRenderer::SetItems(const std::vector<SettingsItemDef>& items) {
    // Remember current selection label to preserve it across rebuilds
    std::string prev_label;
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(items_.size())) {
        prev_label = items_[selected_index_].label;
    }

    items_.clear();
    items_ = items;

    // Try to find the previously selected item in the new list
    if (!prev_label.empty()) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[i].label == prev_label && items_[i].type != SettingsItemType::Section) {
                selected_index_ = i;
                break;
            }
        }
        if (items_[selected_index_].type == SettingsItemType::Section) {
            selected_index_ = GetFirstSelectableIndex();
        }
    } else {
        selected_index_ = GetFirstSelectableIndex();
    }

    scroll_offset_ = 0;
    first_visible_index_ = selected_index_;
    EnsureSelectionVisible();
    ShowCategoryHint();
    needs_full_refresh_ = true;
}

bool SettingsRenderer::UpdateItem(int index, const std::string& value) {
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        const bool changed = items_[index].value != value;
        items_[index].value = value;
        needs_full_refresh_ = true;
        // RenderItem() draws the ON/OFF switch for a checkbox row, never its
        // value text. Reporting such a change as dirty ordered a FAST_BW
        // refresh plus its 30 s colour recovery for pixels that did not move.
        return changed && items_[index].type != SettingsItemType::Checkbox;
    }
    return false;
}

bool SettingsRenderer::UpdateChecked(int index, bool checked) {
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        const bool changed = items_[index].checked != checked;
        items_[index].checked = checked;
        needs_full_refresh_ = true;
        return changed;
    }
    return false;
}

void SettingsRenderer::ShowOtaDialog(const std::vector<std::string>& versions,
                                      const std::string& current_version,
                                      int selected_index,
                                      int progress_percent,
                                      const std::string& status_text,
                                      int state) {
    ota_versions_ = versions;
    ota_current_version_ = current_version;
    ota_selected_index_ = std::clamp(selected_index, 0,
        std::max(0, static_cast<int>(versions.size()) - 1));
    ota_progress_percent_ = std::clamp(progress_percent, 0, 100);
    ota_status_text_ = status_text;
    ota_state_ = state;
    showing_ota_dialog_ = true;
    needs_full_refresh_ = true;
}

void SettingsRenderer::ShowDebugInfo() {
    showing_debug_info_ = true;
    debug_hint_until_us_ = esp_timer_get_time() + 3000000;  // 3 seconds
    needs_full_refresh_ = true;
}

void SettingsRenderer::RenderDebugInfo(uint8_t* fb, int width, int height, int bottom_y) {
    if (!showing_debug_info_) return;
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // Draw debug info as expanded rows below the settings list
    const int line_h = font_->line_height + Style::kSpacingXS;
    int y = bottom_y;

    // Section header
    DrawHLine(fb, width, y, Style::kSpacingMD, width - Style::kSpacingMD, border);
    y += Style::kSpacingXS;

    DrawText(fb, width, Style::kSpacingMD, y, "调试信息", title_font_, text);
    y += title_font_->line_height + Style::kSpacingXS;

    // MAC address
    if (!mac_address_.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "MAC: %s", mac_address_.c_str());
        DrawText(fb, width, Style::kSpacingMD, y, buf, font_, secondary, height);
        y += line_h;
    }

    // Chip model
    if (!chip_model_.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Chip: %s", chip_model_.c_str());
        DrawText(fb, width, Style::kSpacingMD, y, buf, font_, secondary, height);
        y += line_h;
    }

    // Firmware version
    if (!firmware_version_.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Firmware: %s", firmware_version_.c_str());
        DrawText(fb, width, Style::kSpacingMD, y, buf, font_, secondary, height);
    }
}

void SettingsRenderer::RenderVersionBar(uint8_t* fb, int width, int height, int y) {
    // Draw a thin version info bar at the bottom of the settings page
    // Shows firmware version + MAC address in small text
    if (firmware_version_.empty()) return;

    // Ensure bar stays within screen bounds
    const int bar_h = font_->line_height + Style::kSpacingXS;
    if (y + bar_h > height) y = height - bar_h;
    if (y < Style::kStatusBarHeight + kTitleBarH) return;

    // Center the version text
    char buf[64];
    if (!mac_address_.empty()) {
        snprintf(buf, sizeof(buf), "%s  MAC: %s", firmware_version_.c_str(), mac_address_.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s", firmware_version_.c_str());
    }
    int text_w = MeasureTextWidth(buf, font_);
    int text_x = (width - text_w) / 2;
    DrawText(fb, width, text_x, y, buf, font_, ThemeManager::Get().ColorFor(ThemeToken::TextSecondary));
}

void SettingsRenderer::RenderAboutDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    const int dialog_w = 316;
    const int dialog_h = 210;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 30;
    const int content_right = dialog_x + dialog_w - 20;
    const int titlebar_h = 28;
    const int shadow_offset = 2;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    // Solid offset shadow: draw the black base first, then cover it with the
    // white dialog. Only the right and bottom 2px remain visible, avoiding the
    // previous white gap between shadow and modal.
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);
    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, accent);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, accent);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, accent);
    const char* title = "About notellm";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, text, height);

    // Macintosh-inspired title-bar stripes.
    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    const int icon_x = dialog_x + 16;
    const int icon_y = dialog_y + titlebar_h + 20;
    DrawRectBorder(fb, width, {icon_x, icon_y, 46, 38}, 2, accent);
    DrawRectBorder(fb, width, {icon_x + 7, icon_y + 7, 32, 20}, 1, accent);
    DrawRect(fb, width, {icon_x + 18, icon_y + 30, 10, 2}, accent);

    struct InfoRow {
        const char* label;
        std::string value;
    };
    const std::vector<InfoRow> rows = {
        {"设备名称", "notellm"},
        {"型号", "Youn-Beta1.0"},
        {"固件版本", firmware_version_.empty() ? "未知" : firmware_version_},
        {"硬件版本", chip_model_.empty() ? "ESP32-S3" : chip_model_},
        {"序列号", mac_address_.empty() ? "未读取" : mac_address_},
        {"官方网站", "blog.lazyyoun.xyz"},
    };
    const int row_h = kAboutRowHeight;
    int y = dialog_y + titlebar_h + 12;
    const int rows_x = dialog_x + 70;
    for (const auto& row : rows) {
        const int center_y = y + row_h / 2;
        DrawText(fb, width, rows_x, InkCenteredTextTopY(font_, row.label, center_y, 0),
                 row.label, font_, text, height);
        const int value_left_min = rows_x + 84;
        const std::string value = FitTextToWidth(row.value, value_font_,
            std::max(0, content_right - value_left_min));
        const int value_w = MeasureTextWidth(value.c_str(), value_font_);
        DrawText(fb, width, content_right - value_w,
                 InkCenteredTextTopY(value_font_, value.c_str(), center_y, 0),
                 value.c_str(), value_font_, secondary, height);
        DrawHLine(fb, width, y + row_h - 1, rows_x, content_right, border);
        y += row_h;
    }
}

void SettingsRenderer::RenderStorageDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const PaintStyle track_style = theme.Style(ThemeToken::BackgroundPrimary);
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    const Color progress_fill = TokenInkOnPaper(ThemeToken::ProgressFill);
    const int dialog_w = 316;
    const int dialog_h = 180;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 30;
    const int content_right = dialog_x + dialog_w - 20;
    const int titlebar_h = 28;
    const int shadow_offset = 2;
    const int row_h = kAboutRowHeight;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    // Solid offset shadow (same style as About dialog)
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);
    // Close box
    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, accent);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, accent);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, accent);

    const char* title = "存储空间";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, text, height);

    // Title-bar stripes (Macintosh style)
    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    // Storage icon (simple box + bars)
    const int icon_x = dialog_x + 22;
    const int icon_y = dialog_y + titlebar_h + 16;
    DrawRectBorder(fb, width, {icon_x, icon_y, 46, 38}, 2, accent);
    // Usage bar fill
    int fill_pct = 0;
    if (!storage_total_.empty() && !storage_used_.empty()) {
        // Parse approximate percentage from strings like "1.2MB/2.0MB"
        // Simple visual: fill proportional
        fill_pct = 60;  // Default moderate fill for visual
    }
    const int bar_x = icon_x + 7;
    const int bar_w = 32;
    const int bar_h = 20;
    const int bar_y = icon_y + 7;
    DrawStyledRect(fb, width, {bar_x, bar_y, bar_w, bar_h}, track_style);
    DrawRectBorder(fb, width, {bar_x, bar_y, bar_w, bar_h}, 1, progress_style.border);
    int fill_w = (bar_w - 2) * fill_pct / 100;
    if (fill_w > 0) {
        DrawRect(fb, width, {bar_x + 1, bar_y + 1, fill_w, bar_h - 2}, progress_fill);
    }

    // Info rows
    struct InfoRow {
        const char* label;
        std::string value;
    };
    const std::vector<InfoRow> rows = {
        {"已用空间", storage_used_},
        {"总空间", storage_total_},
        {"图片数量", std::to_string(storage_photos_)},
        {"TXT数量", std::to_string(storage_txts_)},
    };
    const int rows_x = dialog_x + 88;
    int y = dialog_y + titlebar_h + 12;
    for (const auto& row : rows) {
        const int center_y = y + row_h / 2;
        DrawText(fb, width, rows_x, InkCenteredTextTopY(font_, row.label, center_y, 0),
                 row.label, font_, text, height);
        const int value_left_min = rows_x + 84;
        const std::string value = FitTextToWidth(row.value, value_font_,
            std::max(0, content_right - value_left_min));
        const int value_w = MeasureTextWidth(value.c_str(), value_font_);
        DrawText(fb, width, content_right - value_w,
                 InkCenteredTextTopY(value_font_, value.c_str(), center_y, 0),
                 value.c_str(), value_font_, secondary, height);
        DrawHLine(fb, width, y + row_h - 1, rows_x, content_right, border);
        y += row_h;
    }
}

void SettingsRenderer::RenderVolumeDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const PaintStyle track_style = theme.Style(ThemeToken::BackgroundPrimary);
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    const Color progress_fill = TokenInkOnPaper(ThemeToken::ProgressFill);
    const int dialog_w = 292;
    const int dialog_h = 166;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = (height - dialog_h) / 2;
    const int inner_x = dialog_x + 18;
    const int inner_w = dialog_w - 36;

    ClearDialogRegionRounded(fb, width, height, dialog_x, dialog_y, dialog_w, dialog_h,
                             Style::kBorderRadiusLG, kVolumeDialogClearPad);
    // Same solid 2px offset shadow as About dialog: no separating white gap.
    DrawStyledRoundRect(fb, width, height, {dialog_x + 2, dialog_y + 2, dialog_w, dialog_h},
                        Style::kBorderRadiusLG, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusLG, modal_style);
    const char* title = "音量调整";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopY(font_, title, dialog_y + 24, 0),
             title, font_, text, height);
    DrawHLine(fb, width, dialog_y + 42, dialog_x + 14, dialog_x + dialog_w - 14, border);

    char volume_buf[16];
    snprintf(volume_buf, sizeof(volume_buf), "%d%%", volume_dialog_value_);
    const int value_w = MeasureTextWidth(volume_buf, title_font_);
    DrawText(fb, width, dialog_x + (dialog_w - value_w) / 2,
             InkCenteredTextTopY(title_font_, volume_buf, dialog_y + 70, 0),
             volume_buf, title_font_, accent, height);

    const int speaker_x = inner_x + 12;
    const int speaker_y = dialog_y + 60;
    DrawIconCentered(fb, width, speaker_x, speaker_y, SettingsIconFor("音量"), accent);

    const int track_x = inner_x;
    const int track_y = dialog_y + 102;
    const int track_w = inner_w;
    const int track_h = 16;
    DrawStyledRoundRect(fb, width, height, {track_x, track_y, track_w, track_h},
                        Style::kBorderRadiusPill, track_style);
    DrawRectBorder(fb, width, {track_x, track_y, track_w, track_h}, 1, progress_style.border);
    int fill_w = (track_w - 4) * volume_dialog_value_ / 100;
    if (fill_w > 0) {
        DrawRect(fb, width, {track_x + 2, track_y + 2, fill_w, track_h - 4}, progress_fill);
    }

    for (int i = 0; i <= 4; ++i) {
        const int tick_x = track_x + 2 + (track_w - 4) * i / 4;
        DrawVLine(fb, width, tick_x, track_y + track_h + 4, track_y + track_h + 8, border);
    }

    const int hint_center_y = dialog_y + dialog_h - 20;
    DrawText(fb, width, inner_x + 6, InkCenteredTextTopY(font_, "UP/DN 调整  BOOT 保存", hint_center_y, 0),
             "UP/DN 调整  BOOT 保存", font_, secondary, height);
}

void SettingsRenderer::RenderServerDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color danger = TokenInkOnPaper(ThemeToken::Danger);
    const int dialog_w = 316;
    const int dialog_h = 210;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 30;
    const int content_right = dialog_x + dialog_w - 20;
    const int titlebar_h = 28;
    const int shadow_offset = 2;
    const int row_h = kAboutRowHeight;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);
    // Close box
    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, danger);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, danger);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, danger);

    const char* title = "服务地址";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, text, height);

    // Title-bar stripes
    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    // Current address
    const int rows_x = dialog_x + 18;
    int y = dialog_y + titlebar_h + 10;

    // Current connection label
    std::string current_label = "当前: " + (server_current_addr_.empty() ? "未连接" : server_current_addr_);
    DrawText(fb, width, rows_x,
             InkCenteredTextTopYInBox(font_, current_label.c_str(), y, row_h, 0),
             current_label.c_str(), font_, secondary, height);
    y += row_h;
    DrawHLine(fb, width, y - 4, rows_x, content_right, border);

    // Option rows: local and remote
    struct ServerOption {
        const char* label;
        std::string addr;
        int index;
    };
    const ServerOption options[] = {
        {"本地自发现", server_local_addr_, 0},
        {"远程服务器", server_remote_addr_, 1},
    };

    for (const auto& opt : options) {
        const int center_y = y + row_h / 2;
        const bool is_selected = (server_selected_ == opt.index);

        // Selection indicator
        if (is_selected) {
            DrawStyledRoundRect(fb, width, height, {rows_x - 4, y, content_right - rows_x + 8, row_h},
                                Style::kBorderRadiusSM, selected_style);
        }

        // Label
        DrawText(fb, width, rows_x,
                 InkCenteredTextTopY(font_, opt.label, center_y, 0),
                 opt.label, font_, is_selected ? selected_style.fg : text, height);

        // Address value
        std::string addr_display = FitTextToWidth(opt.addr.empty() ? "--" : opt.addr, value_font_,
                                                   std::max(0, content_right - rows_x - 80));
        const int addr_w = MeasureTextWidth(addr_display.c_str(), value_font_);
        DrawText(fb, width, content_right - addr_w,
                 InkCenteredTextTopY(value_font_, addr_display.c_str(), center_y, 0),
                 addr_display.c_str(), value_font_, is_selected ? selected_style.fg : secondary, height);

        y += row_h;
    }

    // Hint
    const int hint_center_y = dialog_y + dialog_h - 20;
    DrawText(fb, width, dialog_x + 30,
             InkCenteredTextTopY(font_, "UP/DN 切换  BOOT 确认", hint_center_y, 0),
             "UP/DN 切换  BOOT 确认", font_, secondary, height);
}

void SettingsRenderer::RenderServerListDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color danger = TokenInkOnPaper(ThemeToken::Danger);
    const Color accent = TokenInkOnPaper(ThemeToken::Accent);
    const int dialog_w = 316;
    const int dialog_h = 220;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 30;
    const int content_right = dialog_x + dialog_w - 20;
    const int titlebar_h = 28;
    const int shadow_offset = 2;
    const int row_h = kAboutRowHeight;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);
    // Close box
    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, danger);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, danger);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, danger);

    const char* title = "服务地址历史";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, text, height);

    // Title-bar stripes
    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    const int rows_x = dialog_x + 18;
    int y = dialog_y + titlebar_h + 10;
    const int total = static_cast<int>(server_list_addresses_.size());

    if (total == 0) {
        DrawText(fb, width, rows_x,
                 InkCenteredTextTopY(font_, "无历史地址", y + row_h / 2, 0),
                 "无历史地址", font_, secondary, height);
    } else {
        // Render visible rows with scrolling
        const int visible_rows = std::min(kServerListVisibleRows, total);
        const int scroll_start = server_list_scroll_offset_;

        for (int i = 0; i < visible_rows; ++i) {
            const int item_idx = scroll_start + i;
            if (item_idx >= total) break;

            const std::string& addr = server_list_addresses_[item_idx];
            const bool is_selected = (item_idx == server_list_selected_);
            const bool is_current = (addr == server_list_current_);
            const int center_y = y + row_h / 2;

            // Selection indicator
            if (is_selected) {
                DrawStyledRoundRect(fb, width, height, {rows_x - 4, y, content_right - rows_x + 8, row_h},
                                    Style::kBorderRadiusSM, selected_style);
            }

            // Index number
            char idx_buf[16];  // Large enough for any int32 + "." + null terminator
            snprintf(idx_buf, sizeof(idx_buf), "%d.", item_idx + 1);
            DrawText(fb, width, rows_x,
                     InkCenteredTextTopY(font_, idx_buf, center_y, 0),
                     idx_buf, font_, is_selected ? selected_style.fg : accent, height);

            // Address value (fit to available width)
            std::string addr_display = FitTextToWidth(addr, value_font_,
                                                       std::max(0, content_right - rows_x - 40));
            DrawText(fb, width, rows_x + 28,
                     InkCenteredTextTopY(value_font_, addr_display.c_str(), center_y, 0),
                     addr_display.c_str(), value_font_, is_selected ? selected_style.fg : text, height);

            // Current indicator
            if (is_current) {
                const char* cur_mark = "(当前)";
                DrawText(fb, width, content_right - MeasureTextWidth(cur_mark, font_) - 4,
                         InkCenteredTextTopY(font_, cur_mark, center_y, 0),
                         cur_mark, font_, is_selected ? selected_style.fg : secondary, height);
            }

            y += row_h;
        }

        // Scroll indicator (if more items than visible)
        if (total > kServerListVisibleRows) {
            const int scroll_bar_x = content_right + 4;
            const int scroll_bar_h = kServerListVisibleRows * row_h;
            const int scroll_bar_y = dialog_y + titlebar_h + 10;
            DrawVLine(fb, width, scroll_bar_x, scroll_bar_y, scroll_bar_y + scroll_bar_h, border);

            // Scroll thumb
            const int thumb_h = scroll_bar_h * kServerListVisibleRows / total;
            const int thumb_y = scroll_bar_y + (scroll_start * scroll_bar_h / total);
            DrawRect(fb, width, {scroll_bar_x - 2, thumb_y, 4, thumb_h}, accent);
        }
    }

    // Hint
    const int hint_center_y = dialog_y + dialog_h - 20;
    DrawText(fb, width, dialog_x + 30,
             InkCenteredTextTopY(font_, "UP/DN 滚动  BOOT 选择", hint_center_y, 0),
             "UP/DN 滚动  BOOT 选择", font_, secondary, height);
}

void SettingsRenderer::RenderThemeDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle text_style = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle border_style = theme.Style(ThemeToken::Border);

    const int dialog_w = 292;
    const int dialog_h = 218;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 32;
    const int titlebar_h = 28;
    const int row_h = 25;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    DrawStyledRoundRect(fb, width, height,
                        {dialog_x + 2, dialog_y + 2, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border_style.border);

    const char* title = "选择主题";
    const int title_w = MeasureTextWidth(title, font_);
    DrawStyledText(fb, width, dialog_x + (dialog_w - title_w) / 2,
                   InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
                   title, font_, text_style, height);

    int y = dialog_y + titlebar_h + 8;
    for (int i = 0; i < ThemeManager::ThemeCount(); ++i) {
        const ThemeId id = ThemeManager::ThemeAt(i);
        const bool selected = (i == theme_selected_);
        const bool current = (id == ThemeManager::Get().CurrentId());
        const int row_x = dialog_x + 18;
        const int row_w = dialog_w - 36;
        const int center_y = y + row_h / 2;
        if (selected) {
            DrawStyledRoundRect(fb, width, height, {row_x - 4, y, row_w + 8, row_h},
                                Style::kBorderRadiusSM, selected_style);
        }

        PaintStyle row_text = selected ? selected_style : text_style;
        const char* name = ThemeManager::DisplayName(id);
        DrawStyledText(fb, width, row_x,
                       InkCenteredTextTopY(font_, name, center_y, 0),
                       name, font_, row_text, height);

        const int swatch_x = dialog_x + dialog_w - 62;
        const PaintStyle accent = ThemeManager::Get().GetTheme(id).tokens[static_cast<int>(ThemeToken::Accent)];
        DrawStyledRect(fb, width, {swatch_x, y + 6, 16, 13}, accent);
        DrawRectBorder(fb, width, {swatch_x, y + 6, 16, 13}, 1, border_style.border);
        const PaintStyle danger = ThemeManager::Get().GetTheme(id).tokens[static_cast<int>(ThemeToken::Danger)];
        DrawStyledRect(fb, width, {swatch_x + 20, y + 6, 16, 13}, danger);
        DrawRectBorder(fb, width, {swatch_x + 20, y + 6, 16, 13}, 1, border_style.border);

        if (current) {
            const char* mark = "当前";
            const int mark_w = MeasureTextWidth(mark, value_font_);
            DrawStyledText(fb, width, swatch_x - mark_w - 8,
                           InkCenteredTextTopY(value_font_, mark, center_y, 0),
                           mark, value_font_, row_text, height);
        }
        y += row_h;
    }

    const char* hint = "UP/DN 选择  BOOT 应用";
    DrawStyledText(fb, width, dialog_x + 24,
                   InkCenteredTextTopY(font_, hint, dialog_y + dialog_h - 18, 0),
                   hint, font_, text_style, height);
}

void SettingsRenderer::RenderOtaDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const PaintStyle track_style = theme.Style(ThemeToken::BackgroundPrimary);
    const Color danger = TokenInkOnPaper(ThemeToken::Danger);
    const Color progress_fill = TokenInkOnPaper(ThemeToken::ProgressFill);
    const int dialog_w = 316;
    const int dialog_h = 220;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = Style::kStatusBarHeight + 30;
    const int content_right = dialog_x + dialog_w - 20;
    const int titlebar_h = 28;
    const int shadow_offset = 2;
    const int row_h = 28;

    ClearDialogRegionRounded(fb, width, height, dialog_x + 3, dialog_y + 3, dialog_w, dialog_h,
                             Style::kBorderRadiusMD, 2);
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);

    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, danger);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, danger);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, danger);

    const char* title = "固件更新";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, text, height);

    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    const int rows_x = dialog_x + 18;
    int y = dialog_y + titlebar_h + 10;

    std::string current = "当前: " + (ota_current_version_.empty() ? std::string("--") : ota_current_version_);
    DrawText(fb, width, rows_x,
             InkCenteredTextTopY(font_, current.c_str(), y + row_h / 2, 0),
             current.c_str(), font_, secondary, height);
    y += row_h;
    DrawHLine(fb, width, y - 4, rows_x, content_right, border);

    const bool selecting = ota_state_ == 2;
    const bool downloading = ota_state_ == 4 || ota_state_ == 5 || ota_state_ == 6;
    const bool failed = ota_state_ == 7;

    if (selecting && !ota_versions_.empty()) {
        const int total = static_cast<int>(ota_versions_.size());
        const int visible_rows = std::min(kOtaVisibleRows, total);
        int start = std::max(0, ota_selected_index_ - visible_rows / 2);
        if (start + visible_rows > total) start = std::max(0, total - visible_rows);
        for (int i = 0; i < visible_rows; ++i) {
            const int idx = start + i;
            const bool is_selected = idx == ota_selected_index_;
            const int center_y = y + row_h / 2;
            if (is_selected) {
                DrawStyledRoundRect(fb, width, height, {rows_x - 4, y, content_right - rows_x + 8, row_h},
                                    Style::kBorderRadiusSM, selected_style);
            }
            std::string label = FitTextToWidth(ota_versions_[idx], font_,
                                               std::max(0, content_right - rows_x - 8));
            DrawText(fb, width, rows_x,
                     InkCenteredTextTopY(font_, label.c_str(), center_y, 0),
                     label.c_str(), font_, is_selected ? selected_style.fg : text, height);
            y += row_h;
        }
    } else if (downloading) {
        const int bar_x = rows_x;
        const int bar_y = y + 34;
        const int bar_w = content_right - rows_x;
        const int bar_h = 16;
        std::string status = ota_status_text_.empty() ? "正在更新..." : ota_status_text_;
        status = FitTextToWidth(status, font_, std::max(0, content_right - rows_x));
        DrawText(fb, width, rows_x,
                 InkCenteredTextTopY(font_, status.c_str(), y + row_h / 2, 0),
                 status.c_str(), font_, text, height);
        DrawStyledRoundRect(fb, width, height, {bar_x, bar_y, bar_w, bar_h},
                            Style::kBorderRadiusPill, track_style);
        DrawRectBorder(fb, width, {bar_x, bar_y, bar_w, bar_h}, 1, progress_style.border);
        const int fill_w = (bar_w - 4) * ota_progress_percent_ / 100;
        if (fill_w > 0) {
            DrawRect(fb, width, {bar_x + 2, bar_y + 2, fill_w, bar_h - 4}, progress_fill);
        }
        char pct_buf[16];
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", ota_progress_percent_);
        const int pct_w = MeasureTextWidth(pct_buf, font_);
        DrawText(fb, width, dialog_x + (dialog_w - pct_w) / 2,
                 bar_y + bar_h + 8, pct_buf, font_, secondary, height);
    } else {
        std::string status = ota_status_text_;
        if (status.empty()) status = failed ? "更新失败" : "正在获取版本列表...";
        status = FitTextToWidth(status, font_, std::max(0, content_right - rows_x));
        DrawText(fb, width, rows_x,
                 InkCenteredTextTopY(font_, status.c_str(), y + row_h / 2, 0),
                 status.c_str(), font_, failed ? danger : text, height);
    }

    const char* hint = selecting ? "UP/DN 选择  BOOT 更新  长按取消"
                                 : "长按取消  BOOT 关闭";
    const std::string hint_text = FitTextToWidth(hint, font_, dialog_w - 40);
    const int hint_center_y = dialog_y + dialog_h - 20;
    DrawText(fb, width, dialog_x + 20,
             InkCenteredTextTopY(font_, hint_text.c_str(), hint_center_y, 0),
             hint_text.c_str(), font_, secondary, height);
}

void SettingsRenderer::RenderOtaConfirmDialog(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle title_style = theme.Style(ThemeToken::Badge);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    // OTA 确认弹窗：显示固件名称，UP/DN 选择确认/取消，BOOT 执行
    const int dialog_w = 280;
    const int dialog_h = 160;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = (height - dialog_h) / 2;
    const int titlebar_h = 36;
    const int row_h = 28;

    DrawStyledRect(fb, width, {dialog_x - kDialogClearPad, dialog_y - kDialogClearPad,
                   dialog_w + kDialogClearPad * 2, dialog_h + kDialogClearPad * 2}, bg_style);

    // 弹窗边框
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);

    // 标题栏背景
    DrawStyledRoundRect(fb, width, height, {dialog_x + 1, dialog_y + 1, dialog_w - 2, titlebar_h - 2},
                        Style::kBorderRadiusMD - 1, title_style);

    // 标题
    const char* title = "确认更新?";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopY(font_, title, dialog_y + titlebar_h / 2, 0),
             title, font_, title_style.fg, height);

    const int content_x = dialog_x + 20;
    int y = dialog_y + titlebar_h + 15;

    // 显示固件名称
    std::string firmware_label = "固件: " + ota_confirm_firmware_name_;
    firmware_label = FitTextToWidth(firmware_label, font_, dialog_w - 60);
    DrawText(fb, width, content_x,
             InkCenteredTextTopY(font_, firmware_label.c_str(), y + row_h / 2, 0),
             firmware_label.c_str(), font_, secondary, height);
    y += row_h + 8;

    // 确认/取消选项
    const char* options[2] = {"确认更新", "取消"};
    for (int i = 0; i < 2; ++i) {
        const bool is_selected = (i == ota_confirm_selected_);
        const int opt_y = y + i * row_h;

        if (is_selected) {
            DrawStyledRoundRect(fb, width, height, {content_x - 4, opt_y + 2, dialog_w - content_x - dialog_x + 4, row_h - 4},
                                Style::kBorderRadiusSM, selected_style);
        }

        DrawText(fb, width, content_x,
                 InkCenteredTextTopY(font_, options[i], opt_y + row_h / 2, 0),
                 options[i], font_, is_selected ? selected_style.fg : text, height);
    }

    // 提示
    const char* hint = "UP/DN 选择  BOOT 确认  长按返回";
    const int hint_w = MeasureTextWidth(hint, font_);
    const int hint_y = dialog_y + dialog_h - 24;
    DrawText(fb, width, dialog_x + (dialog_w - hint_w) / 2,
             InkCenteredTextTopY(font_, hint, hint_y, 0),
             hint, font_, secondary, height);
}

}  // namespace rawdraw
