#ifndef ZECTRIX_SSD2683_FAST_BW_H
#define ZECTRIX_SSD2683_FAST_BW_H

#include <stdint.h>

namespace ssd2683_fast_bw {

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

static_assert(EncodeSemanticByte(0x00) == 0x00, "black must remain black");
static_assert(EncodeSemanticByte(0x55) == 0x55, "white must remain white");
static_assert(EncodeSemanticByte(0xAA) == 0x00, "yellow must map to black");
static_assert(EncodeSemanticByte(0xFF) == 0x00, "red must map to black");
static_assert(EncodeSemanticByte(0x6D) == 0x41, "mixed semantic pixels must map independently");
static_assert(!DeadlineReached(99, 100), "deadline must not fire early");
static_assert(DeadlineReached(100, 100), "deadline must fire on equality");
static_assert(DeadlineReached(1, UINT32_MAX), "deadline comparison must handle tick wrap");

}  // namespace ssd2683_fast_bw

#endif  // ZECTRIX_SSD2683_FAST_BW_H
