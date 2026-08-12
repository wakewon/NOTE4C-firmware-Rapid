"""Edge / content maps used to steer how hard the ditherer is allowed to work.

Error diffusion is at its worst exactly where the image is most structured:
around type, hair, branches and building outlines it smears a pixel's residual
across the boundary and the edge grows a halo of the wrong ink. Suppressing
diffusion there costs a little tonal accuracy in a place nobody can measure it,
and buys a visibly crisper edge.
"""

from __future__ import annotations

import numpy as np
from scipy import ndimage


def _smoothstep(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    t = np.clip((x - lo) / max(hi - lo, 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gradient_magnitude(l: np.ndarray, presmooth: float = 0.6) -> np.ndarray:
    """Sobel magnitude of L*, lightly pre-smoothed to reject sensor noise."""
    src = ndimage.gaussian_filter(l, sigma=presmooth, mode="nearest") if presmooth > 0 else l
    gx = ndimage.sobel(src, axis=1, mode="nearest")
    gy = ndimage.sobel(src, axis=0, mode="nearest")
    return np.hypot(gx, gy)


def edge_strength(
    l: np.ndarray,
    *,
    low_pct: float = 75.0,
    high_pct: float = 97.0,
    presmooth: float = 0.6,
    dilate: int = 0,
) -> np.ndarray:
    """Per-pixel edge weight in ``0..1``, self-normalising per image.

    Percentile thresholds rather than absolute ones, so a soft portrait and a
    hard-edged screenshot both end up with a sensible fraction of the frame
    marked as "edge" instead of one of them saturating.
    """
    mag = gradient_magnitude(l, presmooth)
    lo = float(np.percentile(mag, low_pct))
    hi = float(np.percentile(mag, high_pct))
    if hi - lo < 1e-6:
        return np.zeros_like(mag)
    strength = _smoothstep(mag, lo, hi)
    if dilate > 0:
        strength = ndimage.maximum_filter(strength, size=2 * dilate + 1, mode="nearest")
    return strength


def flatness(l: np.ndarray, radius: float = 3.0) -> np.ndarray:
    """Local standard deviation of L*, normalised to ``0..1`` (1 = flat)."""
    mean = ndimage.uniform_filter(l, size=int(2 * radius + 1), mode="nearest")
    sq = ndimage.uniform_filter(l * l, size=int(2 * radius + 1), mode="nearest")
    std = np.sqrt(np.maximum(sq - mean * mean, 0.0))
    return 1.0 - _smoothstep(std, 0.5, 6.0)


def classify(l: np.ndarray, chroma: np.ndarray) -> dict:
    """Cheap content statistics, used to suggest a preset."""
    mag = gradient_magnitude(l)
    strong = float(np.mean(mag > 25.0))
    flat = float(np.mean(mag < 2.0))
    hist, _ = np.histogram(l, bins=32, range=(0.0, 100.0))
    p = hist / max(hist.sum(), 1)
    entropy = float(-(p[p > 0] * np.log2(p[p > 0])).sum())
    return {
        "strong_edge_ratio": round(strong, 4),
        "flat_ratio": round(flat, 4),
        "lightness_entropy": round(entropy, 3),
        "mean_chroma": round(float(np.mean(chroma)), 2),
        "high_chroma_ratio": round(float(np.mean(chroma > 25.0)), 4),
    }


def suggest_preset(stats: dict) -> str:
    """Rough Photo / Illustration / Text-Screenshot triage."""
    if stats["flat_ratio"] > 0.55 and stats["strong_edge_ratio"] > 0.06 and stats["lightness_entropy"] < 3.2:
        return "text"
    if stats["flat_ratio"] > 0.42 and stats["lightness_entropy"] < 4.2:
        return "illustration"
    return "photo"
