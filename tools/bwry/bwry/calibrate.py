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
#:
#: They must also stay *inside* the quadrilateral through the four mark centres
#: (x 21..379, y 21..279). The homography is only an interpolation within that
#: quad; a zone reaching past it extrapolates off the active area and onto the
#: panel bezel, which reads near-black. That contaminated the illumination fit
#: with values 30x below the true white level and reported a 51% light
#: variation on a frame whose real variation is a third of that.
WHITE_ZONES = [
    (44, 24, 356, 48),    # top
    (44, 252, 356, 276),  # bottom
    (24, 44, 48, 256),    # left
    (352, 44, 376, 256),  # right
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

    # Sever thin dark structures before labelling. The panel's bezel seam, a
    # cable, or the edge of the table is only a few pixels thick, but if one
    # touches a mark it becomes a single connected component whose centre of
    # mass sits out on the line rather than on the mark. That is not a small
    # error: a top-bezel seam merged with the top-left mark here produced a
    # blob 1799 px wide with a fill ratio of 0.20 and moved that corner 280 px,
    # skewing the homography enough to sample the black patch as "white border".
    radius = max(1, int(min(h, w) * 0.0025))
    mask = ndimage.binary_opening(mask, structure=np.ones((2 * radius + 1, 2 * radius + 1)))

    labels, count = ndimage.label(mask)
    if count == 0:
        raise ValueError("no dark regions found; pass --corners")
    sizes = ndimage.sum(mask, labels, range(1, count + 1))
    centers = np.array(ndimage.center_of_mass(mask, labels, range(1, count + 1)))
    boxes = ndimage.find_objects(labels)

    # A mark is a solid square, so demand that shape: it separates the marks
    # from halftone bands, glare edges and anything else dark in frame.
    min_area = (h * w) * 0.00015
    keep = np.zeros(count, dtype=bool)
    for i in range(count):
        if sizes[i] < min_area:
            continue
        ys, xs = boxes[i]
        bh, bw = ys.stop - ys.start, xs.stop - xs.start
        if bh == 0 or bw == 0:
            continue
        fill = sizes[i] / float(bh * bw)
        aspect = bw / float(bh)
        keep[i] = fill >= 0.55 and 0.5 <= aspect <= 2.0
    if keep.sum() < 4:
        raise ValueError(
            f"found {int(keep.sum())} square registration marks, need 4; pass --corners"
        )
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

    def __init__(self, coeffs: np.ndarray, residual: float, zone_spread: float = 0.0):
        self.coeffs = coeffs  # (6, 3)
        self._residual = residual
        self._zone_spread = zone_spread

    @property
    def zone_spread(self) -> float:
        """Worst disagreement between the corrected white zones, as a fraction.

        This is the number that actually decides whether the flat field did its
        job. ``non_uniformity`` says how steep the light was; this says whether
        dividing it out left the four borders of the chart agreeing with each
        other, which is the precondition for comparing a patch in one corner
        against a patch in another. A steep but smooth gradient corrects
        cleanly; a shallow but lumpy one does not.
        """
        return float(self._zone_spread)

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
        """RMS of the quadratic fit against the white border, relative to level.

        A large residual means the light is not smooth -- a hard shadow edge, a
        specular highlight, or a reflection of something in the room.

        Relative, not absolute: the same scene developed a stop brighter has a
        proportionally larger absolute residual while being no less smooth, so
        an absolute threshold would fail frames for being well exposed.
        """
        return float(self._residual / max(float(np.mean(self.reference)), 1e-9))


def estimate_illumination(
    photo: np.ndarray, corners: np.ndarray, linear_photo: np.ndarray | None = None
) -> IlluminationField:
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    if linear_photo is None:
        linear_photo = C.srgb_to_linear(photo.astype(np.float64) / 255.0)
    else:
        linear_photo = np.asarray(linear_photo, dtype=np.float64)

    zones = []
    for x0, y0, x1, y1 in WHITE_ZONES:
        gx, gy = np.meshgrid(
            np.linspace(x0 + 3, x1 - 3, max(4, int((x1 - x0) / 8))),
            np.linspace(y0 + 3, y1 - 3, max(4, int((y1 - y0) / 8))),
        )
        zones.append(np.stack([gx.ravel(), gy.ravel()], axis=1))
    chart_pts = np.concatenate(zones)

    vals = _sample(linear_photo, h, chart_pts)
    basis = _poly_features(chart_pts)
    coeffs, *_ = np.linalg.lstsq(basis, vals, rcond=None)
    resid = float(np.sqrt(np.mean((basis @ coeffs - vals) ** 2)))
    field = IlluminationField(coeffs, resid)

    # How well did it actually work? Correct each zone and compare their means.
    # Patch sampling averages hundreds of points, so per-pixel noise is not the
    # question; systematic disagreement between one edge of the chart and
    # another is, because that is what biases one patch against another.
    luma = np.array([0.2126, 0.7152, 0.0722])
    zone_means = []
    for pts in zones:
        corrected = _sample(linear_photo, h, pts) / np.maximum(field.gain(pts), 1e-9)
        zone_means.append(float(np.mean(corrected @ luma)))
    zone_means = np.asarray(zone_means)
    field._zone_spread = float(
        (zone_means.max() - zone_means.min()) / max(zone_means.mean(), 1e-9)
    )
    return field


def brightest_chart_level(linear: np.ndarray, corners: np.ndarray) -> float:
    """Brightest channel anywhere on the chart itself, in linear light.

    Used to expose a RAW for the chart instead of for the frame around it. It
    reads the patches and the white border, not the whole image, so a white
    bezel or a lit wall cannot set the exposure. The reference is a high
    percentile rather than the maximum, so a dust speck or a hot pixel on one
    patch does not cost the other eleven a stop of range.
    """
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    regions = [rect for _, rect in _patch_rects()] + list(WHITE_ZONES)

    vals = []
    for (px, py, qx, qy) in regions:
        mx, my = (qx - px) * 0.30, (qy - py) * 0.30
        gx, gy = np.meshgrid(
            np.linspace(px + mx, qx - mx, 16), np.linspace(py + my, qy - my, 16)
        )
        pts = np.stack([gx.ravel(), gy.ravel()], axis=1)
        vals.append(_sample(linear, h, pts))
    return float(np.percentile(np.concatenate(vals), 99.5))


def sample_patches(
    photo: np.ndarray,
    corners: np.ndarray,
    inset: float = 0.30,
    illumination: IlluminationField | None = None,
    linear_photo: np.ndarray | None = None,
) -> dict[str, PatchSample]:
    """Sample every chart patch from ``photo`` given the four mark centres.

    ``linear_photo`` optionally supplies the same scene already in linear light
    (from a developed RAW), which is then measured instead of the 8-bit encode.
    That matters for the darkest channel of a saturated ink: this panel's yellow
    reflects 1.3% of white in blue, which survives 14-bit RAW comfortably but
    lands on code value 13 +/- noise once encoded to 8-bit sRGB, where a tenth
    of the patch quantises to zero and looks like a clipped measurement.
    """
    h = homography(mark_centers(), np.asarray(corners, dtype=np.float64))
    if linear_photo is None:
        if photo is None:
            raise ValueError("give either photo or linear_photo")
        photo_f = photo.astype(np.float64)
        linear_photo = C.srgb_to_linear(photo_f / 255.0)
        clip_hi, clip_lo, hi, lo = photo_f, photo_f, 253.0, 2.0
    else:
        linear_photo = np.asarray(linear_photo, dtype=np.float64)
        # In linear light "clipped" means the sensor actually ran out of range,
        # not that the encode ran out of code values.
        clip_hi, clip_lo, hi, lo = linear_photo, linear_photo, 1.0, 1e-5

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

        # Clipping check on the uncorrected values: a blown white patch or a
        # crushed black one makes the whole profile wrong, silently.
        clipped_high = float(np.mean(_sample(clip_hi, h, chart_pts) >= hi))
        clipped_low = float(np.mean(_sample(clip_lo, h, chart_pts) <= lo))

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
            # How much does it actually cost? Compare the ink as measured with
            # the same ink if the floored channel were truly zero. For a
            # saturated ink's weakest channel that bound is small; for a channel
            # carrying real luminance it is not. Reporting the bound beats
            # asserting a cause -- the cause differs between an 8-bit file
            # (processing) and a RAW (the channel is simply at the noise floor).
            floor_test = s.linear.copy()
            floor_test[np.argmin(s.linear)] = 0.0
            impact = float(C.delta_e76(
                C.xyz_to_lab(C.linear_to_xyz(s.linear / max(white.linear.max(), 1e-9))),
                C.xyz_to_lab(C.linear_to_xyz(floor_test / max(white.linear.max(), 1e-9))),
            ))
            where = f"{s.label} patch reads zero in its weakest channel over {s.clipped_low * 100:.0f}% of it"
            if impact > 5.0:
                problems.append(
                    f"{where}, which leaves that ink uncertain by up to dE {impact:.0f}. "
                    "Expose brighter, or shoot RAW if this was a JPEG."
                )
            else:
                warnings.append(
                    f"{where}; the channel is near the noise floor, so this ink's "
                    f"colour is uncertain by up to dE {impact:.1f}. A brighter exposure "
                    "would pin it down"
                )

    # Hard physical invariant: the white state is the most *luminous* thing the
    # panel has, so no ink may out-shine it in Y. If one does, the camera is not
    # reporting reflectance -- it is reporting its own idea of a nice picture.
    #
    # The invariant is deliberately on luminance and not per channel. A
    # saturated ink may legitimately exceed white in a single channel: this
    # panel's yellow reflects 1.55x the white state in R (measured from RAW,
    # with the illumination controlled for by the white border directly above
    # each patch), because its white is a dull particle layer while its yellow
    # pigment reflects strongly above 500 nm. Its luminance is still only 0.91
    # of white, so nothing is physically wrong. An earlier per-channel test
    # rejected exactly this, which is why the first RAW frame was misdiagnosed
    # as camera processing.
    #
    # This has to run on the *uncorrected* samples: the gamma correction
    # normalises against white and clips, which erases the evidence.
    luma = np.array([0.2126, 0.7152, 0.0722])
    invariant_src = raw if raw is not None else samples
    invariant_white = invariant_src["white"]
    yw_inv = float(invariant_white.linear @ luma)
    for s in invariant_src.values():
        if s.label == "white" or s.label.startswith("mix"):
            continue
        y_ratio = float(s.linear @ luma) / max(yw_inv, 1e-9)
        if y_ratio > 1.03:
            problems.append(
                f"{s.label} is {y_ratio:.2f}x as luminous as the white patch, which is "
                "physically impossible. The camera applied saturation/colour processing. "
                "Re-shoot in RAW or with a manual camera app, everything 'enhancing' off."
            )
        ratio = s.linear / np.maximum(invariant_white.linear, 1e-9)
        if ratio.max() > 2.5:
            ch = "RGB"[int(np.argmax(ratio))]
            warnings.append(
                f"{s.label} reflects {ratio.max():.2f}x the white patch in {ch}. That is "
                "possible for a saturated ink but extreme; check for colour processing"
            )

    yw = float(white.linear @ np.array([0.2126, 0.7152, 0.0722]))
    yb = float(black.linear @ np.array([0.2126, 0.7152, 0.0722]))
    contrast = yw / max(yb, 1e-9)
    # The upper bound exists to catch a camera tone curve, which inflates
    # contrast dramatically -- the phone JPEG that motivated this check read
    # 33:1. It was originally set from the *estimated* profile's 7.8:1, which
    # was a guess and turned out to be far too pessimistic: a linear RAW of this
    # panel, with no curve anywhere in the path and neither patch clipped,
    # measures 18:1. Widened to match the evidence rather than the guess, while
    # still rejecting the JPEG-tone-curve case that the check is for.
    if contrast < 3.5:
        problems.append(
            f"contrast is only {contrast:.1f}:1, too low for this panel. Almost always "
            "glare: a light source or a bright window is reflecting off the front surface "
            "and lifting the black patch. Move the light off-axis."
        )
    elif contrast > 28.0:
        problems.append(
            f"contrast is {contrast:.1f}:1, well above what this panel can physically do. "
            "The camera's tone curve is still in the photo. Shoot RAW or in a manual "
            "camera app with HDR off, or pass --camera-gamma to correct it."
        )
    elif contrast > 22.0:
        warnings.append(
            f"contrast is {contrast:.1f}:1, on the high side for this panel; some of the "
            "camera's contrast curve may still be present"
        )

    if illumination is not None:
        nu = illumination.non_uniformity
        spread = illumination.zone_spread
        # Judge the correction, not the lighting. A steep gradient that divides
        # out cleanly is harmless; what ruins a profile is the four edges of the
        # chart still disagreeing afterwards, because then a patch in one corner
        # is not comparable with a patch in another.
        if spread > 0.05:
            problems.append(
                f"after flat-field correction the chart's white borders still disagree by "
                f"{spread * 100:.0f}% -- the light is too uneven or too lumpy to correct "
                "reliably. Move further from the light source, or use a larger/more diffuse one."
            )
        elif spread > 0.02:
            warnings.append(
                f"corrected white borders still differ by {spread * 100:.1f}%; usable, "
                "but more even light would be better"
            )
        if nu > 0.15:
            warnings.append(
                f"light varies by {nu * 100:.0f}% across the panel "
                f"(corrected to {spread * 100:.1f}% between borders)"
            )
        if illumination.residual > 0.15:
            warnings.append(
                f"the white border is lumpy (RMS {illumination.residual * 100:.0f}% of level) -- "
                "check for a hard shadow edge, a specular highlight, or a reflection in the panel"
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


def fit_yule_nielsen(samples: dict[str, PatchSample]) -> tuple[float, dict]:
    """Fit the panel's halftone non-linearity from the black/white ramp.

    A halftone does not average in reflectance the way the dithering model
    assumes. Light entering the paper between two dark pixels can scatter
    sideways and be absorbed on its way out, so a fine mix of black and white
    reads darker than the coverage-weighted average of the two solids. The
    standard description is Yule-Nielsen::

        Y_mix ** (1/n) = sum_i coverage_i * Y_i ** (1/n)

    with ``n = 1`` meaning the ideal linear model. Fitted here on the three
    black/white ramp patches only; the colour mixes are left out so they can
    serve as an independent check of whether the fitted n generalises.
    """
    luma = np.array([0.2126, 0.7152, 0.0722])
    Y = {k: float(v.linear @ luma) for k, v in samples.items()}
    ramp = [(f, Y[f"mix_bw_{int(f * 100)}"]) for f in (0.25, 0.50, 0.75)
            if f"mix_bw_{int(f * 100)}" in Y]
    if not ramp or "black" not in Y or "white" not in Y:
        return 1.0, {}

    def predict(n: float, a: float) -> float:
        return ((1 - a) * Y["black"] ** (1 / n) + a * Y["white"] ** (1 / n)) ** n

    grid = np.arange(1.0, 6.001, 0.01)
    errs = [sum((predict(n, a) - meas) ** 2 for a, meas in ramp) for n in grid]
    n = float(grid[int(np.argmin(errs))])

    checks = {}
    for label, a, b in (("mix_by_50", "black", "yellow"), ("mix_ry_50", "red", "yellow"),
                        ("mix_wr_50", "white", "red"), ("mix_br_50", "black", "red"),
                        ("mix_wy_50", "white", "yellow")):
        if label not in Y:
            continue
        linear_pred = 0.5 * Y[a] + 0.5 * Y[b]
        yn_pred = (0.5 * Y[a] ** (1 / n) + 0.5 * Y[b] ** (1 / n)) ** n
        checks[label] = {
            "measured_Y": round(Y[label], 4),
            "linear_error_pct": round(100 * (linear_pred / max(Y[label], 1e-9) - 1), 1),
            "yule_nielsen_error_pct": round(100 * (yn_pred / max(Y[label], 1e-9) - 1), 1),
        }
    return n, {
        "n": round(n, 2),
        "fitted_from": "black/white ramp (25/50/75%)",
        "independent_checks": checks,
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

    # Keep the measurement unclipped: an ink is not confined to the sRGB cube,
    # and this panel's yellow genuinely reflects more red than its own white.
    # Clipping here would bake a dE76 24 hue error into the profile. The clipped
    # copy is only for the preview colours.
    normalised = {k: v.linear * scale for k, v in samples.items()}
    display_rgb = {k: np.clip(v, 0.0, 1.0) for k, v in normalised.items()}

    colors = {}
    for ink in ("black", "white", "yellow", "red"):
        colors[ink] = C.srgb_to_u8(C.linear_to_srgb(display_rgb[ink]))

    profile = build_profile(
        name=name,
        colors={k: [int(c) for c in v] for k, v in colors.items()},
        linear={k: normalised[k] for k in ("black", "white", "yellow", "red")},
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
        # Skip mixes the caller did not supply: a colorimeter reading, or a
        # partial set, is still enough to build the four inks from.
        if frac <= 0.0 or label not in normalised:
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

    n, yn_report = fit_yule_nielsen(samples)
    if yn_report:
        report["halftone"] = yn_report
        profile.measured_reflectance["yule_nielsen_n"] = round(n, 2)

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
