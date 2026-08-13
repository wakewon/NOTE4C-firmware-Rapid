"""Faithful re-implementation of the historical pre-09k web algorithm.

This exists purely as the A/B baseline. It mirrors, line for line, the
``bwry2bpp`` branch of the uploader embedded in
explicit ``rgbaToBwry2bppLegacy`` export in
``docs/inkscreen_image_converter.js``:

* palette assumed to be pure ``#000000 / #FFFFFF / #FF0000 / #FFFF00``
* nearest colour by luma-weighted squared RGB distance
* Floyd-Steinberg, raster order, no serpentine, no clamping of the work buffer

Do not "improve" anything in here -- the whole point is that the research
baseline remains frozen after the shipping default moved to 09k.
"""

from __future__ import annotations

import numpy as np

# Palette order as written in the firmware page: black, white, red, yellow.
_PALETTE = ((0.0, 0.0, 0.0), (255.0, 255.0, 255.0), (255.0, 0.0, 0.0), (255.0, 255.0, 0.0))
# ...mapped onto the device codes black=0, white=1, yellow=2, red=3.
_DEVICE = (0, 1, 3, 2)

_WR, _WG, _WB = 0.299, 0.587, 0.114


def legacy_bwry_codes(rgb: np.ndarray) -> np.ndarray:
    """(H, W, 3) uint8 sRGB -> (H, W) uint8 device codes."""
    h, w = rgb.shape[:2]
    src = rgb.astype(np.float64)
    wr = src[..., 0].ravel().tolist()
    wg = src[..., 1].ravel().tolist()
    wb = src[..., 2].ravel().tolist()

    codes = bytearray(h * w)
    pal = _PALETTE

    for y in range(h):
        row = y * w
        for x in range(w):
            i = row + x
            r, g, b = wr[i], wg[i], wb[i]

            best = 0
            best_d = 1e30
            for k in range(4):
                pr, pg, pb = pal[k]
                dr, dg, db = r - pr, g - pg, b - pb
                d = _WR * dr * dr + _WG * dg * dg + _WB * db * db
                if d < best_d:
                    best_d = d
                    best = k

            codes[i] = _DEVICE[best]
            pr, pg, pb = pal[best]
            er, eg, eb = r - pr, g - pg, b - pb

            if x + 1 < w:
                j = i + 1
                wr[j] += er * 7 / 16; wg[j] += eg * 7 / 16; wb[j] += eb * 7 / 16
            if y + 1 < h:
                if x > 0:
                    j = i + w - 1
                    wr[j] += er * 3 / 16; wg[j] += eg * 3 / 16; wb[j] += eb * 3 / 16
                j = i + w
                wr[j] += er * 5 / 16; wg[j] += eg * 5 / 16; wb[j] += eb * 5 / 16
                if x + 1 < w:
                    j = i + w + 1
                    wr[j] += er * 1 / 16; wg[j] += eg * 1 / 16; wb[j] += eb * 1 / 16

    return np.frombuffer(bytes(codes), dtype=np.uint8).reshape(h, w)
