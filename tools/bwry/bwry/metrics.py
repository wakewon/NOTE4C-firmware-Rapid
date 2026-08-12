"""Objective numbers to rank candidates *before* they go on the panel.

None of these replace looking at the real screen -- the panel is the arbiter.
They exist so that a 12-way A/B matrix can be narrowed down quickly, and so a
regression in one image does not hide behind an improvement in another.

The two that matter most:

``hvs_delta_e``
    dE76 between the tone-mapped target and the halftone *after* both have been
    integrated by a Gaussian that stands in for the eye's spatial response at
    normal viewing distance. Halftones are meaningless pixel-for-pixel; this is
    the standard way to score them.

``spurious_chroma``
    Fraction of pixels that got red or yellow ink even though the source there
    was essentially neutral. This is the "grey wall full of confetti" number,
    and it is the single metric the current algorithm does worst on.
"""

from __future__ import annotations

import numpy as np
from scipy import ndimage

from . import color as C
from .palette import PaletteProfile

#: Roughly a 400x300 panel viewed from ~40 cm: one pixel subtends about 1.1
#: arcmin, and the eye's contrast sensitivity rolls off over a couple of pixels.
DEFAULT_HVS_SIGMA = 0.9


def hvs_filtered_lab(codes: np.ndarray, profile: PaletteProfile, sigma: float = DEFAULT_HVS_SIGMA) -> np.ndarray:
    """Integrate the halftone in linear light, then report Lab."""
    xyz = profile.xyz_of_codes(codes)
    if sigma > 0:
        xyz = ndimage.gaussian_filter(xyz, sigma=(sigma, sigma, 0), mode="nearest")
    return C.xyz_to_lab(xyz)


def hvs_delta_e(
    target_lab: np.ndarray,
    codes: np.ndarray,
    profile: PaletteProfile,
    sigma: float = DEFAULT_HVS_SIGMA,
) -> dict:
    got = hvs_filtered_lab(codes, profile, sigma)
    ref = ndimage.gaussian_filter(target_lab, sigma=(sigma, sigma, 0), mode="nearest") if sigma > 0 else target_lab
    de = C.delta_e76(got, ref)
    return {
        "mean": round(float(de.mean()), 3),
        "p95": round(float(np.percentile(de, 95)), 3),
        "max": round(float(de.max()), 3),
    }


def ink_usage(codes: np.ndarray, profile: PaletteProfile) -> dict:
    total = codes.size
    out = {}
    for ink in profile.inks:
        out[ink.name] = round(float(np.count_nonzero(codes == ink.device_code)) / total, 4)
    return out


def spurious_chroma(
    codes: np.ndarray,
    source_lab: np.ndarray,
    profile: PaletteProfile,
    chroma_threshold: float = 8.0,
) -> dict:
    """Colour ink landing on neutral source content.

    ``rate`` is over the whole frame; ``rate_in_neutral`` is over the neutral
    region only, which is the number to watch when an image is mostly grey.
    """
    chromatic_codes = [i.device_code for i in profile.inks if C.chroma(i.lab) >= 12.0]
    colored = np.isin(codes, chromatic_codes)
    neutral = C.chroma(source_lab) < chroma_threshold
    n_neutral = int(neutral.sum())
    bad = int(np.count_nonzero(colored & neutral))
    return {
        "rate": round(bad / codes.size, 5),
        "rate_in_neutral": round(bad / n_neutral, 5) if n_neutral else 0.0,
        "neutral_area": round(n_neutral / codes.size, 4),
    }


def texture_anisotropy(codes: np.ndarray, profile: PaletteProfile) -> float:
    """How directional the halftone texture is. Lower is better.

    Worm patterns from raster-order error diffusion show up as an imbalance
    between the horizontal and diagonal energy of the residual; serpentine and
    blue noise both push this toward zero.
    """
    lab = C.xyz_to_lab(profile.xyz_of_codes(codes))
    l = lab[..., 0]
    detail = l - ndimage.gaussian_filter(l, sigma=1.5, mode="nearest")
    fx = float(np.mean(np.abs(np.diff(detail, axis=1))))
    fy = float(np.mean(np.abs(np.diff(detail, axis=0))))
    fd = float(np.mean(np.abs(detail[1:, 1:] - detail[:-1, :-1])))
    ref = max((fx + fy + fd) / 3.0, 1e-9)
    return round(float(np.std([fx, fy, fd]) / ref), 4)


def evaluate(
    codes: np.ndarray,
    target_lab: np.ndarray,
    source_lab: np.ndarray,
    profile: PaletteProfile,
    sigma: float = DEFAULT_HVS_SIGMA,
) -> dict:
    return {
        "hvs_delta_e": hvs_delta_e(target_lab, codes, profile, sigma),
        "ink_usage": ink_usage(codes, profile),
        "spurious_chroma": spurious_chroma(codes, source_lab, profile),
        "texture_anisotropy": texture_anisotropy(codes, profile),
    }
