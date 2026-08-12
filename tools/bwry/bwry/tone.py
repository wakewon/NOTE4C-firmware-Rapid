"""Tone / chroma preprocessing tuned for a reflective four-ink panel.

Everything operates on Lab, because the quantity we care about preserving is
perceived lightness, and because the panel's usable range is best expressed as
an L* interval.

The pointwise part of the tone curve is baked into a monotone LUT. That has two
practical benefits: parameter combinations can never produce a non-monotonic
(posterising) curve, and the exact curve can be dumped into the A/B sidecar so a
result is reproducible from its metadata alone.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict

import numpy as np
from scipy import ndimage

from . import color as C

LUT_SIZE = 1024


@dataclass
class ToneParams:
    """Pointwise + local tone controls, all neutral at their defaults."""

    # Auto black/white point on L*, as percentiles of the image histogram.
    autocontrast: bool = True
    auto_low_pct: float = 0.5
    auto_high_pct: float = 99.5
    #: Never darken pixels that were already near-white (keeps paper as paper).
    preserve_white_pct: float = 99.9
    preserve_white_l: float = 96.0

    exposure: float = 0.0  # stops
    contrast: float = 0.0  # S-curve strength, -1..1
    contrast_pivot: float = 0.45  # normalised L* the S-curve rotates about
    shadow_lift: float = 0.0  # 0..1
    highlight_compress: float = 0.0  # 0..1

    local_contrast: float = 0.0  # unsharp amount on L*
    local_radius: float = 6.0  # px
    local_detail: float = 0.0  # second, finer unsharp pass
    local_detail_radius: float = 1.6

    saturation: float = 1.0  # multiplier on Lab chroma

    def to_dict(self) -> dict:
        return asdict(self)


# --------------------------------------------------------------------------
# Pointwise curve
# --------------------------------------------------------------------------


def _l_to_y(l: np.ndarray) -> np.ndarray:
    """L* (0..100) -> relative luminance Y (0..1)."""
    f = (l + 16.0) / 116.0
    return np.where(f > 6.0 / 29.0, f**3, (f - 4.0 / 29.0) * 3.0 * (6.0 / 29.0) ** 2)


def _y_to_l(y: np.ndarray) -> np.ndarray:
    y = np.maximum(y, 0.0)
    eps = (6.0 / 29.0) ** 3
    f = np.where(y > eps, np.cbrt(y), y / (3.0 * (6.0 / 29.0) ** 2) + 4.0 / 29.0)
    return 116.0 * f - 16.0


def _s_curve(x: np.ndarray, strength: float, pivot: float) -> np.ndarray:
    """Endpoint-preserving S-curve on 0..1 that leaves ``pivot`` fixed."""
    if abs(strength) < 1e-6:
        return x
    pivot = float(np.clip(pivot, 0.05, 0.95))
    # Warp so the pivot sits at 0.5, apply a symmetric sigmoid, warp back.
    g = np.log(0.5) / np.log(pivot)
    u = np.clip(x, 0.0, 1.0) ** g

    k = 8.0 * strength
    if abs(k) < 1e-6:
        v = u
    else:
        sig = lambda t: 1.0 / (1.0 + np.exp(-k * (t - 0.5)))
        lo, hi = sig(0.0), sig(1.0)
        v = (sig(u) - lo) / (hi - lo)
    return np.clip(v, 0.0, 1.0) ** (1.0 / g)


def _shadow_lift(x: np.ndarray, amount: float) -> np.ndarray:
    """Open up the toe without touching midtones or highlights."""
    if amount <= 1e-6:
        return x
    lifted = x ** (1.0 / (1.0 + 2.2 * amount))
    w = np.clip(1.0 - x / 0.55, 0.0, 1.0) ** 1.5
    return x * (1.0 - w) + lifted * w


def _highlight_compress(x: np.ndarray, amount: float) -> np.ndarray:
    """Roll off the shoulder so specular highlights keep some texture."""
    if amount <= 1e-6:
        return x
    knee = 1.0 - 0.55 * amount
    over = np.clip(x - knee, 0.0, None)
    span = max(1.0 - knee, 1e-6)
    rolled = knee + span * (1.0 - np.exp(-over / span))
    # Renormalise so pure white still reaches 1.0.
    top = knee + span * (1.0 - np.exp(-(1.0 - knee) / span))
    return np.where(x > knee, rolled / max(top, 1e-6), x)


def build_tone_lut(params: ToneParams, black_l: float, white_l: float) -> np.ndarray:
    """Monotone LUT mapping input L* (0..100) to tone-mapped L* (0..100).

    ``black_l``/``white_l`` come from the image histogram (autocontrast); the
    device range fit happens later, after local contrast.
    """
    x = np.linspace(0.0, 100.0, LUT_SIZE)

    if params.autocontrast and white_l - black_l > 1.0:
        y = (x - black_l) / (white_l - black_l)
    else:
        y = x / 100.0
    y = np.clip(y, 0.0, 1.0)

    if abs(params.exposure) > 1e-6:
        y = np.clip(_y_to_l(_l_to_y(y * 100.0) * (2.0**params.exposure)) / 100.0, 0.0, 1.0)

    y = _s_curve(y, params.contrast, params.contrast_pivot)
    y = _shadow_lift(y, params.shadow_lift)
    y = _highlight_compress(y, params.highlight_compress)

    y = np.maximum.accumulate(np.clip(y, 0.0, 1.0))
    return y * 100.0


def apply_lut(l: np.ndarray, lut: np.ndarray) -> np.ndarray:
    grid = np.linspace(0.0, 100.0, lut.size)
    return np.interp(np.clip(l, 0.0, 100.0), grid, lut)


# --------------------------------------------------------------------------
# Local contrast
# --------------------------------------------------------------------------


def unsharp(l: np.ndarray, amount: float, radius: float) -> np.ndarray:
    if abs(amount) < 1e-6 or radius <= 0:
        return l
    blurred = ndimage.gaussian_filter(l, sigma=radius, mode="nearest")
    return l + amount * (l - blurred)


# --------------------------------------------------------------------------
# Full tone stage
# --------------------------------------------------------------------------


def auto_levels(l: np.ndarray, params: ToneParams) -> tuple[float, float]:
    """Black/white point in L*, from percentiles of the image."""
    if not params.autocontrast:
        return 0.0, 100.0
    lo = float(np.percentile(l, params.auto_low_pct))
    hi = float(np.percentile(l, params.auto_high_pct))
    if hi - lo < 5.0:  # nearly flat image: leave it alone
        return 0.0, 100.0
    # If the image already contains genuine paper-white, do not stretch it
    # further -- that only blows out backgrounds and scanned documents.
    if params.preserve_white_pct > 0:
        near_white = float(np.percentile(l, params.preserve_white_pct))
        if near_white >= params.preserve_white_l:
            hi = max(hi, near_white)
    return lo, hi


def apply_tone(lab: np.ndarray, params: ToneParams) -> tuple[np.ndarray, dict]:
    """Tone + chroma stage. Returns the adjusted Lab and a metadata dict."""
    lab = np.asarray(lab, dtype=np.float64).copy()
    l = lab[..., 0]

    black_l, white_l = auto_levels(l, params)
    lut = build_tone_lut(params, black_l, white_l)
    l = apply_lut(l, lut)

    l = unsharp(l, params.local_contrast, params.local_radius)
    l = unsharp(l, params.local_detail, params.local_detail_radius)
    l = np.clip(l, 0.0, 100.0)

    lab[..., 0] = l
    if abs(params.saturation - 1.0) > 1e-6:
        lab[..., 1:] *= params.saturation

    meta = {
        "auto_black_l": round(black_l, 2),
        "auto_white_l": round(white_l, 2),
        "lut_samples": [round(float(v), 3) for v in lut[:: LUT_SIZE // 16]],
    }
    return lab, meta


def fit_to_device_range(lab: np.ndarray, l_black: float, l_white: float, headroom: float = 0.0) -> np.ndarray:
    """Linearly map L* 0..100 onto the panel's reachable lightness interval.

    ``headroom`` (in L*) keeps a sliver away from the pure ink endpoints, which
    stops error diffusion from starving in large flat black or white areas.
    """
    lab = np.asarray(lab, dtype=np.float64).copy()
    lo = l_black + headroom
    hi = l_white - headroom
    lab[..., 0] = lo + np.clip(lab[..., 0], 0.0, 100.0) * (hi - lo) / 100.0
    return lab


# --------------------------------------------------------------------------
# Chroma gate
# --------------------------------------------------------------------------


@dataclass
class ChromaGate:
    """Keeps low-chroma content on the black/white axis.

    Grey walls, overcast sky, shadowed skin and JPEG chroma noise all carry a
    small but non-zero a*/b*. Error diffusion happily accumulates that until it
    tips a pixel into red or yellow, which is where the confetti comes from.
    Attenuating chroma below ``c_lo`` removes the fuel; the residual penalty
    handed to the quantiser removes the rest.
    """

    c_lo: float = 3.5
    c_hi: float = 13.0
    penalty: float = 26.0  # dE units added to chromatic inks when fully closed
    hue_penalty: float = 0.0  # dE units per 180deg of hue error
    enabled: bool = True

    def openness(self, lab: np.ndarray) -> np.ndarray:
        """0 where the pixel should be pure B/W, 1 where all four inks are fair game."""
        if not self.enabled:
            return np.ones(lab.shape[:-1], dtype=np.float64)
        c = C.chroma(lab)
        t = np.clip((c - self.c_lo) / max(self.c_hi - self.c_lo, 1e-6), 0.0, 1.0)
        return t * t * (3.0 - 2.0 * t)  # smoothstep

    def apply(self, lab: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Returns ``(gated_lab, openness)``."""
        open_ = self.openness(lab)
        if not self.enabled:
            return lab, open_
        out = np.asarray(lab, dtype=np.float64).copy()
        out[..., 1:] *= open_[..., None]
        return out, open_

    def to_dict(self) -> dict:
        return asdict(self)
