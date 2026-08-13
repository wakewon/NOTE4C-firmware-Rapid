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


#: White bands around the patch grid, in chart coordinates, clear of both the
#: registration marks and the patches. These are known-white by construction,
#: so they are what the illumination field is fitted from.
WHITE_ZONES = [
    (44, 8, 356, 44),    # top
    (44, 252, 356, 292),  # bottom
    (8, 44, 44, 256),    # left
    (356, 44, 392, 256),  # right
]


def _patch_rects() -> list[tuple[str, tuple[int, int, int, int]]]:
    x0, y0 = 52, 52
    x1, y1 = SCREEN_WIDTH - 52, SCREEN_HEIGHT - 52
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

    # Mix patches are coarse horizontal bands on a 4-row cell, not a fine
    # checkerboard. Coverage is exact either way, but coarse clusters have far
    # less edge per unit area, so they suffer far less optical spreading. That
    # matters because these patches are the reference used to measure the
    # *camera's* response (see fit_camera_gamma); the less panel behaviour they
    # carry, the cleaner that measurement is.
    yy = np.mgrid[0:SCREEN_HEIGHT, 0:SCREEN_WIDTH][0]
    for (label, ink_a, ink_b, frac), (_, (px, py, qx, qy)) in zip(PATCHES, _patch_rects()):
        if frac <= 0.0:
            codes[py:qy, px:qx] = ink_a
            continue
        pattern = np.where((yy % 4) < round(frac * 4), ink_b, ink_a).astype(np.uint8)
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
    clipped_high: float = 0.0  # fraction of sampled pixels at or above 253
    clipped_low: float = 0.0  # fraction at or below 2


def _sample(linear_photo: np.ndarray, h: np.ndarray, chart_pts: np.ndarray) -> np.ndarray:
    """Bilinear-sample the photo at a set of chart-space points. (N, 3) linear."""
    photo_pts = _project(h, chart_pts)
    coords = np.stack([photo_pts[:, 1], photo_pts[:, 0]])
    return np.stack(
        [ndimage.map_coordinates(linear_photo[..., c], coords, order=1, mode="nearest") for c in range(3)],
        axis=1,
    )


def _poly_features(pts: np.ndarray) -> np.ndarray:
    """Quadratic basis over chart coordinates normalised to roughly -1..1."""
    x = pts[:, 0] / (SCREEN_WIDTH * 0.5) - 1.0
    y = pts[:, 1] / (SCREEN_HEIGHT * 0.5) - 1.0
    return np.stack([np.ones_like(x), x, y, x * x, x * y, y * y], axis=1)


class IlluminationField:
    """Per-channel quadratic model of how the light fell across the panel.

    Fitted from the chart's white border, which is white by construction, so
    any variation across it is lighting plus lens vignetting rather than
    content. Dividing it out is what makes patches at opposite corners
    comparable -- and without a colorimeter, that is the single biggest source
    of error in the whole procedure.
    """

    def __init__(self, coeffs: np.ndarray, residual: float):
        self.coeffs = coeffs  # (6, 3)
        self._residual = residual

    def evaluate(self, chart_pts: np.ndarray) -> np.ndarray:
        return _poly_features(np.asarray(chart_pts, dtype=np.float64)) @ self.coeffs

    def gain(self, chart_pts: np.ndarray) -> np.ndarray:
        """Correction factor: divide a sample by this to flatten the field."""
        field = self.evaluate(chart_pts)
        return field / np.maximum(self.reference, 1e-9)

    @property
    def reference(self) -> np.ndarray:
        """Field value at the chart centre; the normalisation anchor."""
        return self.evaluate(np.array([[SCREEN_WIDTH / 2.0, SCREEN_HEIGHT / 2.0]]))[0]

    @property
    def non_uniformity(self) -> float:
        """Peak-to-peak luminance variation across the chart, as a fraction."""
        gx, gy = np.meshgrid(np.linspace(20, SCREEN_WIDTH - 20, 24), np.linspace(20, SCREEN_HEIGHT - 20, 18))
        field = self.evaluate(np.stack([gx.ravel(), gy.ravel()], axis=1))
        lum = field @ np.array([0.2126, 0.7152, 0.0722])
        return float((lum.max() - lum.min()) / max(lum.mean(), 1e-9))

    @property
    def residual(self) -> float:
        """RMS of the quadratic fit against the white border, in linear light.

        A large residual means the light is not smooth -- a hard shadow edge, a
        specular highlight, or a reflection of something in the room.
        """
        return float(self._residual)


def estimate_illumination(photo: np.ndarray, corners: np.ndarray) -> IlluminationField:
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    linear_photo = C.srgb_to_linear(photo.astype(np.float64) / 255.0)

    pts = []
    for x0, y0, x1, y1 in WHITE_ZONES:
        gx, gy = np.meshgrid(
            np.linspace(x0 + 3, x1 - 3, max(4, int((x1 - x0) / 8))),
            np.linspace(y0 + 3, y1 - 3, max(4, int((y1 - y0) / 8))),
        )
        pts.append(np.stack([gx.ravel(), gy.ravel()], axis=1))
    chart_pts = np.concatenate(pts)

    vals = _sample(linear_photo, h, chart_pts)
    basis = _poly_features(chart_pts)
    coeffs, *_ = np.linalg.lstsq(basis, vals, rcond=None)
    resid = float(np.sqrt(np.mean((basis @ coeffs - vals) ** 2)))
    return IlluminationField(coeffs, resid)


def sample_patches(
    photo: np.ndarray,
    corners: np.ndarray,
    inset: float = 0.30,
    illumination: IlluminationField | None = None,
) -> dict[str, PatchSample]:
    """Sample every chart patch from ``photo`` given the four mark centres."""
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    photo_f = photo.astype(np.float64)
    linear_photo = C.srgb_to_linear(photo_f / 255.0)

    out: dict[str, PatchSample] = {}
    for label, (px, py, qx, qy) in _patch_rects():
        # Sample the central portion only, so a little perspective error or
        # blur at the patch border cannot contaminate the reading.
        mx, my = (qx - px) * inset, (qy - py) * inset
        gx, gy = np.meshgrid(
            np.linspace(px + mx, qx - mx, 24), np.linspace(py + my, qy - my, 24)
        )
        chart_pts = np.stack([gx.ravel(), gy.ravel()], axis=1)

        vals = _sample(linear_photo, h, chart_pts)
        if illumination is not None:
            vals = vals / np.maximum(illumination.gain(chart_pts), 1e-9)

        # Clipping check on the raw 8-bit values: a blown white patch or a
        # crushed black one makes the whole profile wrong, silently.
        raw = _sample(photo_f, h, chart_pts)
        clipped_high = float(np.mean(raw >= 253.0))
        clipped_low = float(np.mean(raw <= 2.0))

        mean_linear = vals.mean(axis=0)
        out[label] = PatchSample(
            label, mean_linear, C.srgb_to_u8(C.linear_to_srgb(np.clip(mean_linear, 0, 1))),
            clipped_high, clipped_low,
        )
    return out


#: Black/white ramp used to measure the camera: (label, fraction of white ink).
_BW_RAMP = [("black", 0.0), ("mix_bw_25", 0.25), ("mix_bw_50", 0.50), ("mix_bw_75", 0.75), ("white", 1.0)]

_LUMA = np.array([0.2126, 0.7152, 0.0722])


def fit_camera_gamma(
    samples: dict[str, PatchSample], search=(0.5, 3.0), steps: int = 501
) -> tuple[float, dict]:
    """Measure the tone curve the camera applied on top of sRGB.

    Phone JPEGs are not photometric. They carry a contrast curve -- often a
    fairly strong one -- and it survives the sRGB decode, so a naive reading of
    the photo reports far more contrast than the panel has. On this panel that
    showed up as 33:1 against a physical ceiling around 12:1.

    The chart's black/white ramp pins it down. Those patches are halftones of
    two inks, and halftones mix *linearly in reflectance*, so their true
    reflectances must lie on a straight line against ink coverage. Whatever
    exponent makes the measured ramp straight again is the curve the camera
    added. Solid patches carry no halftone at all, so this is measuring the
    camera rather than the panel.

    Caveat worth stating plainly: any real optical spreading in the halftone
    pushes the same way and gets absorbed into the exponent, so this slightly
    over-corrects. The coarse band pattern keeps that small, and the recovered
    contrast ratio is an independent check on whether the answer is sane.
    """
    missing = [name for name, _ in _BW_RAMP if name not in samples]
    if missing:
        raise KeyError(f"chart is missing ramp patches: {missing}")

    white = samples["white"].linear
    fracs = np.array([f for _, f in _BW_RAMP])
    # Per-channel white normalisation first, then collapse to luminance: the
    # camera's curve acts per channel, and luminance is the robust summary.
    w = np.array([
        float(np.clip(samples[name].linear / np.maximum(white, 1e-9), 0.0, 1.5) @ _LUMA)
        for name, _ in _BW_RAMP
    ])
    w = np.clip(w, 1e-6, None)

    best = (np.inf, 1.0)
    curve = []
    for gamma in np.linspace(search[0], search[1], steps):
        r = w ** (1.0 / gamma)
        # How straight is reflectance against coverage?
        fit = np.polyfit(fracs, r, 1)
        resid = float(np.sqrt(np.mean((np.polyval(fit, fracs) - r) ** 2)))
        curve.append((float(gamma), resid))
        if resid < best[0]:
            best = (resid, float(gamma))

    resid, gamma = best
    r = w ** (1.0 / gamma)
    slope, intercept = np.polyfit(fracs, r, 1)
    report = {
        "gamma": round(gamma, 3),
        "ramp_residual": round(resid, 5),
        "ramp_measured": [round(float(v), 5) for v in w],
        "ramp_corrected": [round(float(v), 5) for v in r],
        "implied_black_reflectance_ratio": round(float(intercept), 5),
        "contrast_before": round(float(1.0 / max(w[0], 1e-9)), 1),
        "contrast_after": round(float(1.0 / max(r[0], 1e-9)), 1),
    }
    return gamma, report


def apply_camera_gamma(samples: dict[str, PatchSample], gamma: float) -> dict[str, PatchSample]:
    """Undo the camera's tone curve, per channel, relative to the white patch."""
    if abs(gamma - 1.0) < 1e-6:
        return samples
    white = samples["white"].linear
    out: dict[str, PatchSample] = {}
    for label, s in samples.items():
        w = np.clip(s.linear / np.maximum(white, 1e-9), 0.0, 1.0)
        corrected = (w ** (1.0 / gamma)) * white
        out[label] = PatchSample(
            label, corrected, C.srgb_to_u8(C.linear_to_srgb(np.clip(corrected, 0, 1))),
            s.clipped_high, s.clipped_low,
        )
    return out


def photo_diagnostics(
    samples: dict[str, PatchSample],
    illumination: IlluminationField | None,
    raw: dict[str, PatchSample] | None = None,
) -> dict:
    """Decide whether this photograph is good enough to calibrate from.

    Without a colorimeter the photo *is* the instrument, so it has to be
    checked like one. Each problem below silently produces a plausible-looking
    but wrong profile, which is worse than no profile at all.
    """
    problems: list[str] = []
    warnings: list[str] = []

    white = samples["white"]
    black = samples["black"]

    if white.clipped_high > 0.02:
        problems.append(
            f"white patch is blown out ({white.clipped_high * 100:.0f}% of it is at 255) -- "
            "the exposure is too high, so the panel's white level is unknown. "
            "Drop exposure until nothing clips."
        )
    if black.clipped_low > 0.02:
        problems.append(
            f"black patch is crushed ({black.clipped_low * 100:.0f}% of it is at 0) -- "
            "exposure too low, or the camera applied a contrast curve."
        )
    for s in samples.values():
        if s.label in ("white", "black"):
            continue
        if s.clipped_high > 0.05:
            warnings.append(f"{s.label} patch is partly clipped high ({s.clipped_high * 100:.0f}%)")
        if s.clipped_low > 0.05:
            problems.append(
                f"{s.label} patch has a channel crushed to zero ({s.clipped_low * 100:.0f}% of it). "
                "The camera's saturation processing has thrown away that ink's real reflectance, "
                "and no tone curve can put it back."
            )

    # Hard physical invariant, and the cleanest validity test available: the
    # white state is the most reflective thing the panel has, so no ink can
    # out-reflect it in any channel. If one does, the camera is not reporting
    # reflectance -- it is reporting its own idea of a nice-looking picture.
    # This has to run on the *uncorrected* samples: the gamma correction
    # normalises against white and clips, which erases the evidence.
    invariant_src = raw if raw is not None else samples
    invariant_white = invariant_src["white"]
    for s in invariant_src.values():
        if s.label == "white" or s.label.startswith("mix"):
            continue
        ratio = s.linear / np.maximum(invariant_white.linear, 1e-9)
        if ratio.max() > 1.06:
            ch = "RGB"[int(np.argmax(ratio))]
            problems.append(
                f"{s.label} reflects {ratio.max():.2f}x the white patch in {ch}, which is "
                "physically impossible. The camera applied saturation/colour processing. "
                "Re-shoot in RAW or with a manual camera app, everything 'enhancing' off."
            )

    yw = float(white.linear @ np.array([0.2126, 0.7152, 0.0722]))
    yb = float(black.linear @ np.array([0.2126, 0.7152, 0.0722]))
    contrast = yw / max(yb, 1e-9)
    # A BWRY panel lives somewhere around 6:1 to 13:1. Outside that band the
    # photograph is measuring something other than the panel.
    if contrast < 3.5:
        problems.append(
            f"contrast is only {contrast:.1f}:1, too low for this panel. Almost always "
            "glare: a light source or a bright window is reflecting off the front surface "
            "and lifting the black patch. Move the light off-axis."
        )
    elif contrast > 18.0:
        problems.append(
            f"contrast is {contrast:.1f}:1, well above what this panel can physically do. "
            "The camera's tone curve is still in the photo. Shoot RAW or in a manual "
            "camera app with HDR off, or pass --camera-gamma to correct it."
        )
    elif contrast > 14.0:
        warnings.append(
            f"contrast is {contrast:.1f}:1, on the high side for this panel; some of the "
            "camera's contrast curve may still be present"
        )

    if illumination is not None:
        nu = illumination.non_uniformity
        if nu > 0.35:
            problems.append(
                f"light across the panel varies by {nu * 100:.0f}% -- too uneven to correct "
                "reliably. Move further from the light source, or use a larger/more diffuse one."
            )
        elif nu > 0.15:
            warnings.append(f"light varies by {nu * 100:.0f}% across the panel; corrected, but even is better")
        if illumination.residual > 0.02:
            warnings.append(
                "the white border does not vary smoothly -- check for a hard shadow edge, "
                "a specular highlight, or a reflection in the panel"
            )

    return {
        "contrast_ratio": round(contrast, 2),
        "white_clipped": round(white.clipped_high, 4),
        "black_clipped": round(black.clipped_low, 4),
        "illumination_non_uniformity": round(illumination.non_uniformity, 4) if illumination else None,
        "illumination_residual": round(illumination.residual, 5) if illumination else None,
        "problems": problems,
        "warnings": warnings,
        "usable": not problems,
    }


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
