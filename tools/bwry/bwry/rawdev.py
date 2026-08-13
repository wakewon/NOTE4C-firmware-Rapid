"""Develop camera RAW files into sRGB, without the camera deciding anything.

Calibration measures reflectance, so every convenience a camera normally applies
is a contaminant. A JPEG of the chart arrives with an unknown tone curve, an
unknown saturation boost and clipped channels; the first calibration shot read
this panel's yellow at 1.74x the white patch in red and pinned its blue channel
to zero across a third of the patch, which is not a measurement of anything.

RAW removes the guesswork: the sensor data is linear, 14-bit, and unprocessed.
What this module adds on top is deliberately minimal.

* No auto-brightness. The whole point is to keep absolute levels.
* An explicit sRGB transfer curve, not rawpy's BT.709 default.
* Exposure chosen from the image content so that the brightest thing that
  matters lands just under clipping. This is a pure linear scale, so it changes
  no ratio and no colour -- it only stops an underexposed frame from spending
  its shadows on eight quantisation levels. A frame exposed for the panel's
  white leaves a saturated yellow nowhere to go, so the gain is fitted to the
  brightest *channel*, not to the white patch.

Requires ``rawpy`` (LibRaw). It is an optional dependency: everything else in
the toolchain works without it, and the import error says what to install.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

#: Extensions LibRaw handles that anyone here is plausibly going to shoot.
RAW_SUFFIXES = {
    ".arw", ".sr2", ".srf",           # Sony
    ".cr2", ".cr3", ".crw",           # Canon
    ".nef", ".nrw",                   # Nikon
    ".raf",                           # Fujifilm
    ".orf",                           # Olympus / OM
    ".rw2",                           # Panasonic
    ".pef",                           # Pentax
    ".dng",                           # Adobe / Apple ProRAW / Leica
    ".raw", ".rwl", ".iiq", ".3fr",
}

#: Leave a little room below clipping. sample_patches treats >= 253/255 as
#: clipped, and the flat-field correction divides by a gain that can exceed 1,
#: so landing exactly at 255 would create the very problem this avoids.
DEFAULT_HEADROOM = 0.92


def is_raw(path: str | Path) -> bool:
    return Path(path).suffix.lower() in RAW_SUFFIXES


@dataclass
class DevelopReport:
    """What the developer decided, so a calibration run can show its work."""

    exposure_gain: float
    white_level_before: float
    brightest_channel_after: float
    clipped_fraction: float
    camera_white_balance: tuple[float, ...]

    def describe(self) -> str:
        return (
            f"  RAW development        : linear, sRGB primaries, no auto-brightness\n"
            f"  exposure applied       : {self.exposure_gain:.2f}x "
            f"({np.log2(max(self.exposure_gain, 1e-9)):+.2f} stops, linear scale)\n"
            f"  brightest channel      : {self.brightest_channel_after:.3f} of full scale\n"
            f"  clipped after exposure : {self.clipped_fraction * 100:.4f}% of pixels"
        )


def _require_rawpy():
    try:
        import rawpy
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise ImportError(
            "reading camera RAW needs rawpy (LibRaw). Install it into the tool venv:\n"
            "  .venv-imgtool/bin/pip install rawpy"
        ) from exc
    return rawpy


def develop_linear(
    path: str | Path, *, use_camera_wb: bool = True
) -> tuple[np.ndarray, tuple[float, ...]]:
    """Demosaic to linear-light sRGB primaries, 0..1 float. No curve, no gain.

    Everything downstream decides exposure from this, because deciding it from
    an encoded image would mean measuring through the very curve we are trying
    to keep out of the measurement.
    """
    rawpy = _require_rawpy()
    with rawpy.imread(str(path)) as raw:
        camera_wb = tuple(float(v) for v in raw.camera_whitebalance)
        linear = raw.postprocess(
            use_camera_wb=use_camera_wb,
            use_auto_wb=False,
            no_auto_bright=True,
            output_color=rawpy.ColorSpace.sRGB,
            user_flip=0,
            gamma=(1, 1),
            output_bps=16,
        )
    return linear.astype(np.float64) / 65535.0, camera_wb


def auto_exposure(
    linear: np.ndarray, *, headroom: float = DEFAULT_HEADROOM, percentile: float = 99.99
) -> float:
    """Linear gain that puts the brightest content just under clipping.

    Percentile rather than max, so a specular glint off the panel glass or a
    status LED cannot drag the whole frame down with it.
    """
    reference = float(np.percentile(linear, percentile))
    return headroom / max(reference, 1e-6)


def exposure_for_level(level: float, *, headroom: float = DEFAULT_HEADROOM) -> float:
    """Gain that maps ``level`` (linear) to ``headroom``."""
    return headroom / max(float(level), 1e-6)


def encode_srgb(linear: np.ndarray, exposure: float) -> tuple[np.ndarray, float]:
    """Apply a linear gain and encode to 8-bit sRGB. Returns (rgb, clipped_fraction)."""
    scaled = linear * exposure
    clipped = float((scaled >= 1.0).any(axis=2).mean())
    return _linear_to_srgb_u8(np.clip(scaled, 0.0, 1.0)), clipped


def develop(
    path: str | Path,
    *,
    exposure: float | None = None,
    headroom: float = DEFAULT_HEADROOM,
    use_camera_wb: bool = True,
) -> tuple[np.ndarray, DevelopReport]:
    """Develop a RAW file to 8-bit sRGB.

    Returns ``(rgb_uint8, report)``. ``exposure`` forces a linear gain; the
    default fits one from the whole frame. Calibration overrides that with a
    gain fitted to the chart itself -- see :func:`exposure_for_level` -- because
    a frame containing anything brighter than the panel (a white bezel, a wall)
    would otherwise leave the panel badly underexposed.
    """
    linear, camera_wb = develop_linear(path, use_camera_wb=use_camera_wb)
    if exposure is None:
        exposure = auto_exposure(linear, headroom=headroom)
    rgb, clipped = encode_srgb(linear, exposure)
    report = DevelopReport(
        exposure_gain=float(exposure),
        white_level_before=float(linear.max()),
        brightest_channel_after=float(np.clip(linear * exposure, 0, 1).max()),
        clipped_fraction=clipped,
        camera_white_balance=camera_wb,
    )
    return rgb, report


def _linear_to_srgb_u8(linear: np.ndarray) -> np.ndarray:
    a = 0.055
    out = np.where(
        linear <= 0.0031308,
        linear * 12.92,
        (1 + a) * np.power(np.maximum(linear, 0.0), 1 / 2.4) - a,
    )
    return np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8)
