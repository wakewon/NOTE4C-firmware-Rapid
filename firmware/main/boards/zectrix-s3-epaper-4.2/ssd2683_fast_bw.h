#ifndef ZECTRIX_SSD2683_FAST_BW_H
#define ZECTRIX_SSD2683_FAST_BW_H

#include <stdint.h>

namespace ssd2683_fast_bw {

enum class RecoveryMode : uint8_t {
    Quality,
    DeferredInteraction,
};

constexpr uint32_t RecoveryDelayMs(RecoveryMode mode,
                                   uint32_t quality_ms,
                                   uint32_t deferred_ms) {
    return mode == RecoveryMode::DeferredInteraction ? deferred_ms : quality_ms;
}

// RawDraw and SSD2683 both pack four 2-bit pixels MSB-first. RawDraw uses
// 00=black, 01=white, 10=yellow, 11=red. FAST_BW keeps white and maps every
// chromatic/non-white value to black.
constexpr uint8_t EncodeSemanticByte(uint8_t packed) {
    uint8_t out = 0;
    for (int pixel = 0; pixel < 4; ++pixel) {
        const uint8_t shift = static_cast<uint8_t>(6 - pixel * 2);
        const uint8_t color = static_cast<uint8_t>((packed >> shift) & 0x03);
        if (color == 0x01) {
            out |= static_cast<uint8_t>(0x01U << shift);
        }
    }
    return out;
}

// Wrap-safe for deadlines less than half the uint32_t tick range away.
constexpr bool DeadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

// A same-content FULL_COLOR is useful only when it is the first known screen
// baseline or when a non-standard FAST_BW waveform has run since the previous
// full refresh. This makes repeated explicit requests idempotent without
// suppressing the one recovery pass that removes FAST_BW colour/ghosting debt.
constexpr bool FullColorHasWork(bool four_color_panel,
                                bool previous_frame_synced,
                                bool framebuffer_changed,
                                bool fast_bw_since_full) {
    return !four_color_panel || !previous_frame_synced ||
           framebuffer_changed || fast_bw_since_full;
}

static_assert(EncodeSemanticByte(0x00) == 0x00, "black must remain black");
static_assert(EncodeSemanticByte(0x55) == 0x55, "white must remain white");
static_assert(EncodeSemanticByte(0xAA) == 0x00, "yellow must map to black");
static_assert(EncodeSemanticByte(0xFF) == 0x00, "red must map to black");
static_assert(EncodeSemanticByte(0x6D) == 0x41, "mixed semantic pixels must map independently");
static_assert(!DeadlineReached(99, 100), "deadline must not fire early");
static_assert(DeadlineReached(100, 100), "deadline must fire on equality");
static_assert(DeadlineReached(1, UINT32_MAX), "deadline comparison must handle tick wrap");
static_assert(FullColorHasWork(true, false, false, false),
              "boot must establish a full-color baseline");
static_assert(FullColorHasWork(true, true, false, true),
              "FAST_BW debt requires one same-content recovery");
static_assert(!FullColorHasWork(true, true, false, false),
              "same content after FULL_COLOR must not refresh again");
static_assert(FullColorHasWork(true, true, true, false),
              "changed content must honor an explicit full refresh");
static_assert(RecoveryDelayMs(RecoveryMode::Quality, 10000, 30000) == 10000,
              "quality mode must use its shorter recovery delay");
static_assert(RecoveryDelayMs(RecoveryMode::DeferredInteraction, 10000, 30000) == 30000,
              "menu interaction must use its deferred recovery delay");

}  // namespace ssd2683_fast_bw

#endif  // ZECTRIX_SSD2683_FAST_BW_H
