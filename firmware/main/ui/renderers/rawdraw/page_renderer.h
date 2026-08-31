/**
 * @file page_renderer.h
 * @brief Rawdraw page renderer base class (no LVGL dependency)
 */

#ifndef RAWDRAW_PAGE_RENDERER_H
#define RAWDRAW_PAGE_RENDERER_H

#include "rawdraw/framebuffer.h"
#include "rawdraw/font_engine.h"
#include <cstdint>
#include <functional>
#include <string>

namespace rawdraw {

enum class PersistentDisplayDependency : uint32_t {
    None = 0,
    Wifi = 1U << 0,
    Server = 1U << 1,
    Battery = 1U << 2,
    Date = 1U << 3,
    Time = 1U << 4,
    PageDate = 1U << 5,
};

using PersistentDisplayDependencies = uint32_t;

constexpr PersistentDisplayDependencies PersistentDependencyMask(
    PersistentDisplayDependency dependency) {
    return static_cast<PersistentDisplayDependencies>(dependency);
}

constexpr PersistentDisplayDependencies operator|(
    PersistentDisplayDependency lhs, PersistentDisplayDependency rhs) {
    return PersistentDependencyMask(lhs) | PersistentDependencyMask(rhs);
}

constexpr PersistentDisplayDependencies operator|(
    PersistentDisplayDependencies lhs, PersistentDisplayDependency rhs) {
    return lhs | PersistentDependencyMask(rhs);
}

/**
 * @brief Button event types
 */
struct ButtonEvent {
    enum Type {
        kUpClick,
        kDownClick,
        kUpDoubleClick,
        kDownDoubleClick,
        kUpLongPress,
        kDownLongPress,
        kBootClick,
        kBootDoubleClick,
        kBootLongPress,
    };
    Type type;
};

/**
 * @brief Page renderer base class for rawdraw mode
 *
 * All page renderers must implement this interface.
 * Renders directly to 1bpp framebuffer, no LVGL dependency.
 */
class PageRenderer {
public:
    virtual ~PageRenderer() = default;

    /**
     * @brief Initialize page resources
     *
     * Called once when page becomes active. Set up fonts, initial state.
     */
    virtual void Init(int width, int height) = 0;

    /**
     * @brief Render page to framebuffer
     *
     * Called on each display update. Draw all page content.
     *
     * @param fb Framebuffer to render to
     * @param width Framebuffer width
     * @param height Framebuffer height
     */
    virtual void Render(uint8_t* fb, int width, int height) = 0;

    /**
     * @brief Handle button input
     *
     * @param event Button event
     * @return true if event was consumed
     */
    virtual bool HandleInput(const ButtonEvent& event) = 0;

    /**
     * @brief Get dirty rect for partial refresh
     *
     * @return Rect that needs refresh, or {0,0,0,0} for full refresh
     */
    virtual Rect GetDirtyRect() const { return {0, 0, 0, 0}; }

    /**
     * Dynamic inputs used by this page's content (excluding the global shell).
     * Power management consumes the aggregate contract from RawDrawUiManager;
     * it never needs to know individual page IDs.
     */
    virtual PersistentDisplayDependencies GetPersistentDisplayDependencies() const {
        return PersistentDependencyMask(PersistentDisplayDependency::None);
    }

    /** Refresh cached data before rendering after a relevant dependency changed. */
    virtual void RefreshPersistentDisplayData(
        PersistentDisplayDependencies dependencies) {
        (void)dependencies;
    }

    /**
     * @brief Check if page needs full refresh
     *
     * @return true if full refresh needed (e.g., page switch)
     */
    virtual bool NeedsFullRefresh() const { return needs_full_refresh_; }

    /**
     * @brief Mark page as needing full refresh
     */
    void MarkFullRefresh() { needs_full_refresh_ = true; }

    /**
     * @brief Clear full refresh flag
     */
    void ClearFullRefreshFlag() { needs_full_refresh_ = false; }

    // Streaming support (for chat pages)
    virtual bool AppendText(const char* chunk) { (void)chunk; return false; }
    virtual void BeginStream() {}
    virtual void EndStream() {}

protected:
    int width_ = 0;
    int height_ = 0;
    bool needs_full_refresh_ = true;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PAGE_RENDERER_H
