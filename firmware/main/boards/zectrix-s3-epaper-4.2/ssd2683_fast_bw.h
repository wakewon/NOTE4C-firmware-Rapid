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

// Recovery delays describe how long the completed FAST_BW result remains on
// glass, not how long ago the request was queued. In particular, an
// inverse/restore pair (two waveforms at least 4 s apart) takes a good part of
// the 10 s quality delay by itself.
constexpr uint32_t RecoveryDeadlineFromCompletion(uint32_t completed_tick,
                                                  RecoveryMode mode,
                                                  uint32_t quality_delay_ticks,
                                                  uint32_t deferred_delay_ticks) {
    return completed_tick + RecoveryDelayMs(
        mode, quality_delay_ticks, deferred_delay_ticks);
}

// The fast path drives a black/yellow RAM waveform, so every semantic RawDraw
// colour (00=black, 01=white, 10=yellow, 11=red) is reduced to one of those
// two. Ink stays ink: black and red become black. Paper stays paper: white
// and yellow become yellow. The theme's selection style (white on red) thus
// reads as yellow on black, and its yellow cards keep their black text.
constexpr bool SemanticIsFastBlack(uint8_t color) {
    return color == 0x00 || color == 0x03;
}

// Four 2bpp semantic pixels -> the high nibble of a 1bpp "is black" mask
// (bit set = black), MSB first like the framebuffer.
constexpr uint8_t SemanticByteToBlackNibble(uint8_t packed) {
    uint8_t out = 0;
    for (int pixel = 0; pixel < 4; ++pixel) {
        const uint8_t color = static_cast<uint8_t>((packed >> (6 - pixel * 2)) & 0x03);
        if (SemanticIsFastBlack(color)) {
            out |= static_cast<uint8_t>(0x80U >> pixel);
        }
    }
    return out;
}

// The RAM waveform selects one of four transitions per pixel from the
// previous and next physical state: bit 1 = previous was yellow, bit 0 = next
// is yellow. Codes 0 (black->black) and 3 (yellow->yellow) are hold states;
// only 1 and 2 drive the pixel. The previous state therefore has to be what
// is really on the glass, which is why the driver tracks it explicitly.
constexpr uint8_t TransitionCode(bool old_black, bool new_black) {
    return static_cast<uint8_t>((old_black ? 0U : 2U) | (new_black ? 0U : 1U));
}

// Packs four transitions from the top nibbles of two black masks.
constexpr uint8_t EncodeTransitionByte(uint8_t old_nibble, uint8_t new_nibble) {
    uint8_t out = 0;
    for (int pixel = 0; pixel < 4; ++pixel) {
        const uint8_t bit = static_cast<uint8_t>(0x80U >> pixel);
        out = static_cast<uint8_t>((out << 2) |
                                   TransitionCode((old_nibble & bit) != 0,
                                                  (new_nibble & bit) != 0));
    }
    return out;
}

// One pixel of a single differential update, planned from the exact colour on
// the glass (any of the four, as left by a full-colour waveform) rather than
// from its black/yellow reduction. A pixel whose colour does not change holds,
// so a status-bar change over a photo leaves the photo's white and red exactly
// as the full-colour waveform drew them. A pixel that already shows the fast
// colour of the new value (yellow under a new white) also holds. Every other
// pixel is driven by the transition into its target from the opposite state,
// whatever it really was: from white or red that is the only way to make the
// controller drive it at all.
struct FastByPixelStep {
    bool old_black;
    bool new_black;
    bool driven;
    uint8_t glass_after;
};

constexpr FastByPixelStep PlanFastByPixel(uint8_t glass, uint8_t next) {
    const bool target_black = SemanticIsFastBlack(next);
    if (glass == next) {
        return {target_black, target_black, false, glass};
    }
    const bool glass_black_or_yellow = glass == 0x00 || glass == 0x02;
    if (glass_black_or_yellow && SemanticIsFastBlack(glass) == target_black) {
        return {target_black, target_black, false, glass};
    }
    return {!target_black, target_black, true,
            static_cast<uint8_t>(target_black ? 0x00 : 0x02)};
}

enum class FastByPlan : uint8_t {
    // Nothing changes in black/yellow terms: no waveform at all.
    Skip,
    // One differential waveform; unchanged pixels hold.
    Single,
    // Invert the current image, then draw the target from that state. Every
    // pixel is driven at least once, so the pair takes over glass whose state
    // is unknown (boot, a failed waveform) and clears the ghosting a large
    // page change or a long run of singles leaves.
    InverseRestore,
};

constexpr FastByPlan PlanFastByUpdate(bool cache_valid,
                                      uint32_t changed_pixels,
                                      uint32_t total_pixels,
                                      uint32_t page_change_percent,
                                      uint32_t singles_since_pair,
                                      uint32_t cleanup_interval) {
    if (!cache_valid) {
        return FastByPlan::InverseRestore;
    }
    if (changed_pixels == 0) {
        return FastByPlan::Skip;
    }
    if (static_cast<uint64_t>(changed_pixels) * 100U >=
        static_cast<uint64_t>(total_pixels) * page_change_percent) {
        return FastByPlan::InverseRestore;
    }
    if (cleanup_interval > 0 && singles_since_pair >= cleanup_interval) {
        return FastByPlan::InverseRestore;
    }
    return FastByPlan::Single;
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

static_assert(SemanticByteToBlackNibble(0x00) == 0xF0, "black must stay black");
static_assert(SemanticByteToBlackNibble(0x55) == 0x00, "white must become yellow");
static_assert(SemanticByteToBlackNibble(0xAA) == 0x00, "yellow must stay yellow");
static_assert(SemanticByteToBlackNibble(0xFF) == 0xF0, "red must become black");
static_assert(SemanticByteToBlackNibble(0x1B) == 0x90,
              "black, white, yellow, red must map independently");
static_assert(TransitionCode(true, true) == 0, "black hold");
static_assert(TransitionCode(true, false) == 1, "black to yellow");
static_assert(TransitionCode(false, true) == 2, "yellow to black");
static_assert(TransitionCode(false, false) == 3, "yellow hold");
static_assert(EncodeTransitionByte(0xF0, 0xF0) == 0x00, "all-black hold");
static_assert(EncodeTransitionByte(0x00, 0x00) == 0xFF, "all-yellow hold");
static_assert(EncodeTransitionByte(0x00, 0xF0) == 0xAA, "yellow to black everywhere");
static_assert(EncodeTransitionByte(0xA0, 0x60) == 0x63,
              "transitions must be packed MSB first per pixel");
static_assert(!PlanFastByPixel(0x01, 0x01).driven &&
                  PlanFastByPixel(0x01, 0x01).glass_after == 0x01,
              "unchanged white must hold and stay white");
static_assert(!PlanFastByPixel(0x03, 0x03).driven &&
                  PlanFastByPixel(0x03, 0x03).glass_after == 0x03,
              "unchanged red must hold and stay red");
static_assert(!PlanFastByPixel(0x02, 0x01).driven,
              "fast yellow already shows a new white");
static_assert(!PlanFastByPixel(0x00, 0x03).driven,
              "fast black already shows a new red");
static_assert(PlanFastByPixel(0x01, 0x00).driven &&
                  !PlanFastByPixel(0x01, 0x00).old_black &&
                  PlanFastByPixel(0x01, 0x00).new_black &&
                  PlanFastByPixel(0x01, 0x00).glass_after == 0x00,
              "white to black is a yellow-to-black drive");
static_assert(PlanFastByPixel(0x03, 0x02).driven &&
                  PlanFastByPixel(0x03, 0x02).old_black &&
                  PlanFastByPixel(0x03, 0x02).glass_after == 0x02,
              "red to yellow is a black-to-yellow drive");
static_assert(PlanFastByPixel(0x03, 0x00).driven,
              "red to black must be driven, or the red would stay");
static_assert(PlanFastByPixel(0x01, 0x02).driven,
              "white to yellow must be driven, or the white would stay");
static_assert(PlanFastByUpdate(false, 0, 120000, 30, 0, 10) == FastByPlan::InverseRestore,
              "an unknown glass state must be taken over with a pair");
static_assert(PlanFastByUpdate(true, 0, 120000, 30, 0, 10) == FastByPlan::Skip,
              "no black/yellow change must not run a waveform");
static_assert(PlanFastByUpdate(true, 100, 120000, 30, 0, 10) == FastByPlan::Single,
              "a small change is one differential waveform");
static_assert(PlanFastByUpdate(true, 36000, 120000, 30, 0, 10) == FastByPlan::InverseRestore,
              "a page change at the threshold uses a pair");
static_assert(PlanFastByUpdate(true, 100, 120000, 30, 10, 10) == FastByPlan::InverseRestore,
              "the cleanup interval forces a pair");
static_assert(PlanFastByUpdate(true, 100, 120000, 30, 99, 0) == FastByPlan::Single,
              "a cleanup interval of 0 disables the cleanup pair");
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
static_assert(RecoveryDeadlineFromCompletion(14000, RecoveryMode::Quality,
                                             10000, 30000) == 24000,
              "quality dwell must start after FAST_BW completes");
static_assert(RecoveryDeadlineFromCompletion(UINT32_MAX - 4,
                                             RecoveryMode::Quality, 10, 30) == 5,
              "completion-based deadline must preserve unsigned tick wrap");

}  // namespace ssd2683_fast_bw

#endif  // ZECTRIX_SSD2683_FAST_BW_H
