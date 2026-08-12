"""Measure what the panel's four inks actually look like.

Workflow
--------
1. ``bwryctl chart -o chart.bin`` and push it to the device.
2. Photograph the panel: flat, even, diffuse light; no flash, no glare; the
   whole screen in frame and roughly square-on. Lock white balance and exposure
   if the camera lets you, and shoot the panel together with the room it will
   live in.
3. ``bwryctl calibrate chart.jpg -o my_profile.json``, giving the four corner
   marks either automatically or with ``--corners``.

The result is *media-relative*: the paper white is normalised to neutral 255,
per channel. That is deliberate. It divides out the ambient illuminant, which is
the largest error in any phone photo, and it discards only information the panel
could not have used anyway -- there is nothing whiter than the paper to
reproduce a paper tint against.

If you own a colorimeter or a colour-checker app, ``bwryctl calibrate
--swatches`` skips the photo entirely and takes the four measurements directly.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image
from scipy import ndimage

from . import color as C
from .palette import PaletteProfile, build_profile
from .pack import SCREEN_HEIGHT, SCREEN_WIDTH

BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3

MARK_SIZE = 26
MARK_INSET = 8

#: (label, ink_a, ink_b, fraction_of_ink_b) -- fraction 0 means a solid patch.
PATCHES: list[tuple[str, int, int, float]] = [
    ("black", BLACK, BLACK, 0.0),
    ("white", WHITE, WHITE, 0.0),
    ("yellow", YELLOW, YELLOW, 0.0),
    ("red", RED, RED, 0.0),
    ("mix_bw_50", BLACK, WHITE, 0.5),
    ("mix_bw_25", BLACK, WHITE, 0.25),
    ("mix_bw_75", BLACK, WHITE, 0.75),
    ("mix_wy_50", WHITE, YELLOW, 0.5),
    ("mix_wr_50", WHITE, RED, 0.5),
    ("mix_br_50", BLACK, RED, 0.5),
    ("mix_by_50", BLACK, YELLOW, 0.5),
    ("mix_ry_50", RED, YELLOW, 0.5),
]

GRID_COLS = 4
GRID_ROWS = 3


# --------------------------------------------------------------------------
# Chart generation
# --------------------------------------------------------------------------


def _patch_rects() -> list[tuple[str, tuple[int, int, int, int]]]:
    x0, y0 = 46, 46
    x1, y1 = SCREEN_WIDTH - 46, SCREEN_HEIGHT - 46
    gutter = 8
    cw = (x1 - x0 - gutter * (GRID_COLS - 1)) // GRID_COLS
    ch = (y1 - y0 - gutter * (GRID_ROWS - 1)) // GRID_ROWS

    out = []
    for idx, (label, _, _, _) in enumerate(PATCHES):
        r, c = divmod(idx, GRID_COLS)
        px = x0 + c * (cw + gutter)
        py = y0 + r * (ch + gutter)
        out.append((label, (px, py, px + cw, py + ch)))
    return out


def make_chart() -> np.ndarray:
    """400x300 device-code array: registration marks plus 12 measurement patches."""
    codes = np.full((SCREEN_HEIGHT, SCREEN_WIDTH), WHITE, dtype=np.uint8)

    # Corner registration marks, in reading order TL, TR, BL, BR.
    for cy, cx in _mark_origins():
        codes[cy : cy + MARK_SIZE, cx : cx + MARK_SIZE] = BLACK

    # A checkerboard carries the mix fraction exactly and averages cleanly even
    # when the photo is slightly out of focus.
    yy, xx = np.mgrid[0:SCREEN_HEIGHT, 0:SCREEN_WIDTH]
    for (label, ink_a, ink_b, frac), (_, (px, py, qx, qy)) in zip(PATCHES, _patch_rects()):
        if frac <= 0.0:
            codes[py:qy, px:qx] = ink_a
            continue
        # 4x4 ordered cell gives exact 25 / 50 / 75 % coverage.
        cell = ((yy % 4) * 4 + (xx % 4)) / 16.0
        pattern = np.where(cell < frac, ink_b, ink_a).astype(np.uint8)
        codes[py:qy, px:qx] = pattern[py:qy, px:qx]

    return codes


def _mark_origins() -> list[tuple[int, int]]:
    return [
        (MARK_INSET, MARK_INSET),
        (MARK_INSET, SCREEN_WIDTH - MARK_INSET - MARK_SIZE),
        (SCREEN_HEIGHT - MARK_INSET - MARK_SIZE, MARK_INSET),
        (SCREEN_HEIGHT - MARK_INSET - MARK_SIZE, SCREEN_WIDTH - MARK_INSET - MARK_SIZE),
    ]


def mark_centers() -> np.ndarray:
    """Chart-space centres of the four registration marks: TL, TR, BR, BL."""
    o = _mark_origins()
    half = MARK_SIZE / 2.0
    tl = (o[0][1] + half, o[0][0] + half)
    tr = (o[1][1] + half, o[1][0] + half)
    bl = (o[2][1] + half, o[2][0] + half)
    br = (o[3][1] + half, o[3][0] + half)
    return np.array([tl, tr, br, bl], dtype=np.float64)


# --------------------------------------------------------------------------
# Homography
# --------------------------------------------------------------------------


def homography(src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    """3x3 projective transform mapping ``src`` (4x2) onto ``dst`` (4x2)."""
    a = []
    b = []
    for (x, y), (u, v) in zip(src, dst):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        b.append(u)
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        b.append(v)
    h = np.linalg.solve(np.array(a, dtype=np.float64), np.array(b, dtype=np.float64))
    return np.append(h, 1.0).reshape(3, 3)


def _project(h: np.ndarray, pts: np.ndarray) -> np.ndarray:
    p = np.concatenate([pts, np.ones((pts.shape[0], 1))], axis=1) @ h.T
    return p[:, :2] / p[:, 2:3]


def find_marks(photo: np.ndarray) -> np.ndarray:
    """Best-effort detection of the four registration marks. TL, TR, BR, BL.

    Looks for the darkest sizeable blob in each quadrant. Works on a clean,
    square-on shot of the panel; if it picks the wrong thing, pass ``--corners``.
    """
    gray = photo.astype(np.float64).mean(axis=2)
    h, w = gray.shape
    thr = np.percentile(gray, 12.0)
    mask = gray <= thr
    mask = ndimage.binary_opening(mask, structure=np.ones((3, 3)))

    labels, count = ndimage.label(mask)
    if count == 0:
        raise ValueError("no dark regions found; pass --corners")
    sizes = ndimage.sum(mask, labels, range(1, count + 1))
    centers = np.array(ndimage.center_of_mass(mask, labels, range(1, count + 1)))

    min_area = (h * w) * 0.00015
    keep = sizes >= min_area
    if not keep.any():
        raise ValueError("registration marks too small to detect; pass --corners")
    centers = centers[keep]

    corners = np.array([[0, 0], [0, w], [h, w], [h, 0]], dtype=np.float64)  # TL TR BR BL in (y, x)
    picked = []
    for corner in corners:
        d = np.linalg.norm(centers - corner, axis=1)
        picked.append(centers[int(np.argmin(d))])
    picked = np.array(picked)
    if len({tuple(np.round(p, 1)) for p in picked}) < 4:
        raise ValueError("could not separate four registration marks; pass --corners")
    return picked[:, ::-1]  # -> (x, y)


# --------------------------------------------------------------------------
# Sampling
# --------------------------------------------------------------------------


@dataclass
class PatchSample:
    label: str
    linear: np.ndarray  # linear-light RGB, camera-relative
    srgb_u8: np.ndarray


def sample_patches(photo: np.ndarray, corners: np.ndarray, inset: float = 0.30) -> dict[str, PatchSample]:
    """Sample every chart patch from ``photo`` given the four mark centres."""
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    linear_photo = C.srgb_to_linear(photo.astype(np.float64) / 255.0)

    out: dict[str, PatchSample] = {}
    for label, (px, py, qx, qy) in _patch_rects():
        # Sample the central portion only, so a little perspective error or
        # blur at the patch border cannot contaminate the reading.
        mx, my = (qx - px) * inset, (qy - py) * inset
        gx, gy = np.meshgrid(
            np.linspace(px + mx, qx - mx, 24), np.linspace(py + my, qy - my, 24)
        )
        chart_pts = np.stack([gx.ravel(), gy.ravel()], axis=1)
        photo_pts = _project(h, chart_pts)

        coords = np.stack([photo_pts[:, 1], photo_pts[:, 0]])
        vals = np.stack(
            [ndimage.map_coordinates(linear_photo[..., c], coords, order=1, mode="nearest") for c in range(3)],
            axis=1,
        )
        mean_linear = vals.mean(axis=0)
        out[label] = PatchSample(label, mean_linear, C.srgb_to_u8(C.linear_to_srgb(mean_linear)))
    return out


def profile_from_samples(
    samples: dict[str, PatchSample],
    *,
    name: str = "note4c-measured",
    display: str = "ZECTRIX Note4C 400x300 BWRY (SSD2683)",
    notes: str = "",
    neutralise_white: bool = True,
) -> tuple[PaletteProfile, dict]:
    """Turn patch readings into a media-relative palette profile plus a report."""
    white_linear = samples["white"].linear
    if neutralise_white:
        scale = 1.0 / np.maximum(white_linear, 1e-9)
    else:
        scale = np.full(3, 1.0 / max(float(white_linear.mean()), 1e-9))

    normalised = {k: np.clip(v.linear * scale, 0.0, 1.0) for k, v in samples.items()}

    colors = {}
    for ink in ("black", "white", "yellow", "red"):
        colors[ink] = C.srgb_to_u8(C.linear_to_srgb(normalised[ink]))

    profile = build_profile(
        name=name,
        colors={k: [int(c) for c in v] for k, v in colors.items()},
        display=display,
        source="measured",
        notes=notes or "Measured from a photograph of the calibration chart; media-relative, "
        "white normalised per channel.",
        measured_reflectance={
            "camera_relative_linear": {k: [round(float(c), 5) for c in v.linear] for k, v in samples.items()},
            "white_normalisation": [round(float(s), 5) for s in scale],
        },
    )

    # Linearity check: does a 50/50 halftone actually read as the average of
    # its two inks? Large residuals mean optical dot gain, which would justify
    # a non-linear mixing model later.
    report = {"mix_check": {}}
    for label, ink_a, ink_b, frac in PATCHES:
        if frac <= 0.0:
            continue
        names = {BLACK: "black", WHITE: "white", YELLOW: "yellow", RED: "red"}
        predicted = (1.0 - frac) * normalised[names[ink_a]] + frac * normalised[names[ink_b]]
        measured = normalised[label]
        de = float(
            C.delta_e76(
                C.xyz_to_lab(C.linear_to_xyz(predicted)), C.xyz_to_lab(C.linear_to_xyz(measured))
            )
        )
        report["mix_check"][label] = {
            "predicted_hex": C.srgb_to_hex(C.linear_to_srgb(predicted)),
            "measured_hex": C.srgb_to_hex(C.linear_to_srgb(measured)),
            "delta_e76": round(de, 2),
        }
    des = [v["delta_e76"] for v in report["mix_check"].values()]
    report["mix_check_mean_delta_e"] = round(float(np.mean(des)), 2) if des else 0.0
    report["linear_mixing_ok"] = report["mix_check_mean_delta_e"] < 5.0

    return profile, report


def profile_from_swatches(
    black: str, white: str, yellow: str, red: str, *, name: str = "note4c-measured", notes: str = ""
) -> PaletteProfile:
    """Build a profile straight from four measured hex colours."""
    raw = {n: C.hex_to_srgb(v) for n, v in (("black", black), ("white", white), ("yellow", yellow), ("red", red))}
    linear = {n: C.srgb_to_linear(v) for n, v in raw.items()}
    scale = 1.0 / np.maximum(linear["white"], 1e-9)
    colors = {n: [int(c) for c in C.srgb_to_u8(C.linear_to_srgb(np.clip(v * scale, 0, 1)))] for n, v in linear.items()}
    return build_profile(
        name=name,
        colors=colors,
        display="ZECTRIX Note4C 400x300 BWRY (SSD2683)",
        source="measured-swatches",
        notes=notes or "Built from four directly measured swatches; media-relative, white normalised per channel.",
    )


def chart_preview(profile: PaletteProfile) -> Image.Image:
    from .pack import render_preview

    return render_preview(make_chart(), profile)
