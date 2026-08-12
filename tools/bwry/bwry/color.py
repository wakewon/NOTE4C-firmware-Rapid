"""Colour-space primitives for the Note4C B/W/R/Y pipeline.

Everything here is vectorised over numpy arrays whose last axis is the channel
axis, so the same helpers work for a single colour ``(3,)``, a palette
``(N, 3)`` and a full image ``(H, W, 3)``.

Conventions
-----------
* ``srgb``   float in ``[0, 1]``, sRGB transfer function (IEC 61966-2-1).
* ``linear`` float in ``[0, 1]``, linear-light sRGB primaries.
* ``xyz``    CIE 1931 XYZ, D65, ``Y`` normalised so that sRGB white is 1.0.
* ``lab``    CIE 1976 L*a*b*, D65 2-degree observer.
* ``lch``    cylindrical Lab, ``h`` in degrees ``[0, 360)``.
"""

from __future__ import annotations

import numpy as np

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

# D65, 2-degree standard observer.
WHITE_D65 = np.array([0.95047, 1.00000, 1.08883], dtype=np.float64)

RGB_TO_XYZ = np.array(
    [
        [0.4124564, 0.3575761, 0.1804375],
        [0.2126729, 0.7151522, 0.0721750],
        [0.0193339, 0.1191920, 0.9503041],
    ],
    dtype=np.float64,
)

XYZ_TO_RGB = np.array(
    [
        [3.2404542, -1.5371385, -0.4985314],
        [-0.9692660, 1.8760108, 0.0415560],
        [0.0556434, -0.2040259, 1.0572252],
    ],
    dtype=np.float64,
)

_LAB_EPS = (6.0 / 29.0) ** 3
_LAB_KAPPA = 3.0 * (6.0 / 29.0) ** 2


# --------------------------------------------------------------------------
# Transfer function
# --------------------------------------------------------------------------


def srgb_to_linear(srgb: np.ndarray) -> np.ndarray:
    """sRGB (0..1) -> linear light (0..1)."""
    s = np.asarray(srgb, dtype=np.float64)
    return np.where(s <= 0.04045, s / 12.92, np.power((s + 0.055) / 1.055, 2.4))


def linear_to_srgb(linear: np.ndarray) -> np.ndarray:
    """Linear light (0..1) -> sRGB (0..1). Values outside 0..1 are clipped."""
    v = np.clip(np.asarray(linear, dtype=np.float64), 0.0, 1.0)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1.0 / 2.4) - 0.055)


# --------------------------------------------------------------------------
# Linear RGB <-> XYZ
# --------------------------------------------------------------------------


def linear_to_xyz(linear: np.ndarray) -> np.ndarray:
    return np.asarray(linear, dtype=np.float64) @ RGB_TO_XYZ.T


def xyz_to_linear(xyz: np.ndarray) -> np.ndarray:
    return np.asarray(xyz, dtype=np.float64) @ XYZ_TO_RGB.T


# --------------------------------------------------------------------------
# XYZ <-> Lab
# --------------------------------------------------------------------------


def xyz_to_lab(xyz: np.ndarray) -> np.ndarray:
    t = np.asarray(xyz, dtype=np.float64) / WHITE_D65
    f = np.where(t > _LAB_EPS, np.cbrt(np.maximum(t, 0.0)), t / _LAB_KAPPA + 4.0 / 29.0)
    fx, fy, fz = f[..., 0], f[..., 1], f[..., 2]
    return np.stack([116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)], axis=-1)


def lab_to_xyz(lab: np.ndarray) -> np.ndarray:
    lab = np.asarray(lab, dtype=np.float64)
    fy = (lab[..., 0] + 16.0) / 116.0
    fx = fy + lab[..., 1] / 500.0
    fz = fy - lab[..., 2] / 200.0
    f = np.stack([fx, fy, fz], axis=-1)
    t = np.where(f > 6.0 / 29.0, np.power(f, 3.0), (f - 4.0 / 29.0) * _LAB_KAPPA)
    return t * WHITE_D65


# --------------------------------------------------------------------------
# Convenience chains
# --------------------------------------------------------------------------


def srgb_to_lab(srgb: np.ndarray) -> np.ndarray:
    return xyz_to_lab(linear_to_xyz(srgb_to_linear(srgb)))


def lab_to_srgb(lab: np.ndarray) -> np.ndarray:
    return linear_to_srgb(xyz_to_linear(lab_to_xyz(lab)))


def srgb_to_xyz(srgb: np.ndarray) -> np.ndarray:
    return linear_to_xyz(srgb_to_linear(srgb))


def xyz_to_srgb(xyz: np.ndarray) -> np.ndarray:
    return linear_to_srgb(xyz_to_linear(xyz))


# --------------------------------------------------------------------------
# Lab <-> LCh
# --------------------------------------------------------------------------


def lab_to_lch(lab: np.ndarray) -> np.ndarray:
    lab = np.asarray(lab, dtype=np.float64)
    c = np.hypot(lab[..., 1], lab[..., 2])
    h = np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360.0
    return np.stack([lab[..., 0], c, h], axis=-1)


def lch_to_lab(lch: np.ndarray) -> np.ndarray:
    lch = np.asarray(lch, dtype=np.float64)
    rad = np.radians(lch[..., 2])
    return np.stack(
        [lch[..., 0], lch[..., 1] * np.cos(rad), lch[..., 1] * np.sin(rad)], axis=-1
    )


def chroma(lab: np.ndarray) -> np.ndarray:
    lab = np.asarray(lab, dtype=np.float64)
    return np.hypot(lab[..., 1], lab[..., 2])


def hue_deg(lab: np.ndarray) -> np.ndarray:
    lab = np.asarray(lab, dtype=np.float64)
    return np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360.0


def hue_distance(h1: np.ndarray, h2: np.ndarray) -> np.ndarray:
    """Shortest angular distance between two hue angles, in degrees (0..180)."""
    d = np.abs(np.asarray(h1, dtype=np.float64) - np.asarray(h2, dtype=np.float64)) % 360.0
    return np.minimum(d, 360.0 - d)


# --------------------------------------------------------------------------
# Colour difference
# --------------------------------------------------------------------------


def delta_e76(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    """CIE76 dE*ab. Plain Euclidean distance in Lab."""
    d = np.asarray(lab1, dtype=np.float64) - np.asarray(lab2, dtype=np.float64)
    return np.sqrt(np.sum(d * d, axis=-1))


def delta_e94(lab1: np.ndarray, lab2: np.ndarray, graphic_arts: bool = True) -> np.ndarray:
    """CIE94 dE. Cheap improvement over dE76 for high-chroma pairs."""
    lab1 = np.asarray(lab1, dtype=np.float64)
    lab2 = np.asarray(lab2, dtype=np.float64)
    kl, k1, k2 = (1.0, 0.045, 0.015) if graphic_arts else (2.0, 0.048, 0.014)

    dl = lab1[..., 0] - lab2[..., 0]
    c1 = np.hypot(lab1[..., 1], lab1[..., 2])
    c2 = np.hypot(lab2[..., 1], lab2[..., 2])
    dc = c1 - c2
    da = lab1[..., 1] - lab2[..., 1]
    db = lab1[..., 2] - lab2[..., 2]
    dh2 = np.maximum(da * da + db * db - dc * dc, 0.0)

    sl = 1.0
    sc = 1.0 + k1 * c1
    sh = 1.0 + k2 * c1
    return np.sqrt((dl / (kl * sl)) ** 2 + (dc / sc) ** 2 + dh2 / (sh * sh))


DELTA_E = {"76": delta_e76, "94": delta_e94}


# --------------------------------------------------------------------------
# 8-bit helpers
# --------------------------------------------------------------------------


def u8_to_srgb(arr: np.ndarray) -> np.ndarray:
    return np.asarray(arr, dtype=np.float64) / 255.0


def srgb_to_u8(arr: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(np.asarray(arr, dtype=np.float64) * 255.0), 0, 255).astype(np.uint8)


def hex_to_srgb(value: str) -> np.ndarray:
    """``"#RRGGBB"`` or ``"#RGB"`` -> float sRGB triple in 0..1."""
    s = value.strip().lstrip("#")
    if len(s) == 3:
        s = "".join(ch * 2 for ch in s)
    if len(s) != 6:
        raise ValueError(f"not a hex colour: {value!r}")
    return np.array([int(s[i : i + 2], 16) for i in (0, 2, 4)], dtype=np.float64) / 255.0


def srgb_to_hex(srgb: np.ndarray) -> str:
    r, g, b = srgb_to_u8(np.asarray(srgb, dtype=np.float64).reshape(3))
    return f"#{r:02X}{g:02X}{b:02X}"
