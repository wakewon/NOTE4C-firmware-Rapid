"""Halftoning from gamut-mapped Lab to device colour codes.

Two families live here:

* **Error diffusion** -- Floyd-Steinberg and friends, with optional serpentine
  scanning, blue-noise modulation, chroma gating and edge-aware attenuation.
  Sequential by nature, so the inner loop is plain Python scalars (numpy
  per-pixel would be an order of magnitude slower).
* **Ordered blue noise** -- for every pixel, find the ink *pair* whose optimal
  linear mix best matches the target, then threshold that mixing fraction
  against a void-and-cluster mask. Fully vectorised, no worms, no directional
  bias, at the cost of some accuracy where three inks would have been needed.

Which colour, and which space
-----------------------------
Ink *selection* is dE76 in Lab: that is what stops a slightly-blue shadow from
being handed the red ink.

The *residual*, though, is carried in linear light (XYZ) by default, because
that is what physically happens when the eye averages a halftone. Diffusing the
residual in Lab looks reasonable on paper and is measurably wrong: a target
half-way between the inks in L* comes out as a 50/50 mix, which then integrates
optically to a good deal lighter than the target. Every flat area ends up
under-inked and the whole image washes out. ``error_space="lab"`` is kept so
that failure mode can be put on the panel next to the correct one.

On the measured panel, fine halftones are not perfectly additive even in
reflectance: the fitted Yule-Nielsen exponent is 1.57.  Optional dot-gain
compensation carries the residual in that model's additive domain, buying back
the lightness that optical spreading would otherwise lose.  It is deliberately
an independent recipe switch so the change can be judged on the panel.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict
from math import sqrt

import numpy as np

from . import bluenoise as bn_mod
from . import color as C
from .palette import PaletteProfile

# --------------------------------------------------------------------------
# Diffusion kernels: (dx, dy, weight), dy >= 0, dx > 0 only on dy == 0
# --------------------------------------------------------------------------

KERNELS: dict[str, list[tuple[int, int, float]]] = {
    "floyd-steinberg": [
        (1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16),
    ],
    "sierra2": [
        (1, 0, 4 / 16), (2, 0, 3 / 16),
        (-2, 1, 1 / 16), (-1, 1, 2 / 16), (0, 1, 3 / 16), (1, 1, 2 / 16), (2, 1, 1 / 16),
    ],
    "sierra3": [
        (1, 0, 5 / 32), (2, 0, 3 / 32),
        (-2, 1, 2 / 32), (-1, 1, 4 / 32), (0, 1, 5 / 32), (1, 1, 4 / 32), (2, 1, 2 / 32),
        (-1, 2, 2 / 32), (0, 2, 3 / 32), (1, 2, 2 / 32),
    ],
    "sierra-lite": [
        (1, 0, 2 / 4), (-1, 1, 1 / 4), (0, 1, 1 / 4),
    ],
    "stucki": [
        (1, 0, 8 / 42), (2, 0, 4 / 42),
        (-2, 1, 2 / 42), (-1, 1, 4 / 42), (0, 1, 8 / 42), (1, 1, 4 / 42), (2, 1, 2 / 42),
        (-2, 2, 1 / 42), (-1, 2, 2 / 42), (0, 2, 4 / 42), (1, 2, 2 / 42), (2, 2, 1 / 42),
    ],
    "jarvis": [
        (1, 0, 7 / 48), (2, 0, 5 / 48),
        (-2, 1, 3 / 48), (-1, 1, 5 / 48), (0, 1, 7 / 48), (1, 1, 5 / 48), (2, 1, 3 / 48),
        (-2, 2, 1 / 48), (-1, 2, 3 / 48), (0, 2, 5 / 48), (1, 2, 3 / 48), (2, 2, 1 / 48),
    ],
    "burkes": [
        (1, 0, 8 / 32), (2, 0, 4 / 32),
        (-2, 1, 2 / 32), (-1, 1, 4 / 32), (0, 1, 8 / 32), (1, 1, 4 / 32), (2, 1, 2 / 32),
    ],
    # Atkinson deliberately drops 2/8 of the error: lighter, crisper, higher
    # local contrast, at the cost of clipping the extremes.
    "atkinson": [
        (1, 0, 1 / 8), (2, 0, 1 / 8),
        (-1, 1, 1 / 8), (0, 1, 1 / 8), (1, 1, 1 / 8),
        (0, 2, 1 / 8),
    ],
}

ALGORITHMS = sorted(KERNELS) + ["bluenoise", "nearest"]

_LAB_EPS = (6.0 / 29.0) ** 3
_LAB_KAPPA = 3.0 * (6.0 / 29.0) ** 2
_XN, _YN, _ZN = 0.95047, 1.00000, 1.08883
_THIRD = 1.0 / 3.0


@dataclass
class DitherParams:
    algorithm: str = "sierra2"
    serpentine: bool = True
    strength: float = 1.0
    #: ``"linear"`` (XYZ) or ``"lab"``. See the module docstring.
    error_space: str = "linear"
    #: Compensate measured optical dot gain using the profile's Yule-Nielsen n.
    #: False preserves ordinary linear-reflectance diffusion for a clean A/B.
    dot_gain_compensation: bool = False
    #: dE penalty applied to red/yellow, scaled by how closed the chroma gate is.
    chroma_penalty: float = 26.0
    #: Global damping of the colour residual; 1.0 keeps it fully in play. The
    #: effective per-pixel value is additionally multiplied by the chroma gate's
    #: openness -- see ``_chroma_error_scale``.
    chroma_error_scale: float = 1.0
    #: Floor for that gate modulation. 0 means a fully-closed gate diffuses no
    #: colour residual at all, which is what stops neutral areas from slowly
    #: charging up a colour debt they eventually pay off with a stray red or
    #: yellow pixel.
    gate_error_floor: float = 0.0
    #: 0 = diffuse normally across edges, 1 = fully suppress diffusion on edges.
    edge_suppress: float = 0.0
    #: Blue-noise modulation of the decision threshold (error diffusion only).
    blue_noise_amount: float = 0.0
    blue_noise_scale: float = 10.0  # L* units at amount 1.0
    #: Hard cap on accumulated error, in units of the working space. 0 disables.
    error_clamp: float = 0.0
    mask_size: int = 64
    mask_sigma: float = 1.9

    def to_dict(self) -> dict:
        return asdict(self)


# --------------------------------------------------------------------------
# Shared per-pixel side inputs
# --------------------------------------------------------------------------


def _chroma_error_scale(params: DitherParams, gate_open, n: int):
    """Per-pixel damping of the colour part of the residual.

    Neither ink is perfectly neutral -- a real e-paper black is slightly blue --
    so a long run of black/white halftone accumulates a small colour residual
    pointing the other way. Left alone it charges up until one pixel finally
    discharges it as red or yellow: the confetti on a grey wall. Where the
    chroma gate says "this area is neutral", the colour residual is simply not
    worth carrying, so it is damped out.
    """
    base = float(params.chroma_error_scale)
    if gate_open is None:
        return None if base == 1.0 else [base] * n
    floor = float(params.gate_error_floor)
    o = np.asarray(gate_open).ravel()
    return (base * (floor + (1.0 - floor) * o)).tolist()


def _side_inputs(shape, params: DitherParams, gate_open, edge):
    h, w = shape
    n = h * w

    if gate_open is None or params.chroma_penalty <= 0:
        pen = [0.0] * n
    else:
        pen = (params.chroma_penalty * (1.0 - np.asarray(gate_open).ravel())).tolist()

    if edge is None or params.edge_suppress <= 0:
        atten = None
    else:
        atten = (1.0 - params.edge_suppress * np.clip(np.asarray(edge).ravel(), 0.0, 1.0)).tolist()

    if params.blue_noise_amount > 0:
        mask = bn_mod.tile(bn_mod.void_and_cluster(params.mask_size, params.mask_sigma), h, w)
        offs = ((mask - 0.5) * (2.0 * params.blue_noise_amount * params.blue_noise_scale)).ravel().tolist()
    else:
        offs = None

    return pen, atten, offs


def _scan_order(w: int):
    return list(range(w)), list(range(w - 1, -1, -1))


# --------------------------------------------------------------------------
# Error diffusion, residual carried in linear light (default, correct)
# --------------------------------------------------------------------------


def _error_diffusion_linear(target_lab, profile, params, *, gate_open, edge):
    h, w = target_lab.shape[:2]
    n = h * w

    kernel = KERNELS[params.algorithm]
    pal_lab = profile.lab
    pal_xyz = profile.xyz
    mix_n = profile.yule_nielsen_n if params.dot_gain_compensation else 1.0
    pal_work = C.yule_nielsen_encode_xyz(pal_xyz, mix_n)
    pl = [float(v) for v in pal_lab[:, 0]]
    pa = [float(v) for v in pal_lab[:, 1]]
    pb = [float(v) for v in pal_lab[:, 2]]
    px = [float(v) for v in pal_work[:, 0]]
    py = [float(v) for v in pal_work[:, 1]]
    pz = [float(v) for v in pal_work[:, 2]]
    pcode = [int(v) for v in profile.device_codes]
    chromatic = [bool(v) for v in ~profile.achromatic_mask]
    nk = len(pl)

    tgt_xyz = C.lab_to_xyz(target_lab)
    tgt_work = C.yule_nielsen_encode_xyz(tgt_xyz, mix_n)
    tx = tgt_work[..., 0].ravel().tolist()
    ty = tgt_work[..., 1].ravel().tolist()
    tz = tgt_work[..., 2].ravel().tolist()

    pen, atten, offs = _side_inputs((h, w), params, gate_open, edge)
    cscale = _chroma_error_scale(params, gate_open, n)

    ex = [0.0] * n
    ey = [0.0] * n
    ez = [0.0] * n
    codes = bytearray(n)

    strength = float(params.strength)
    clamp = float(params.error_clamp)
    serpentine = bool(params.serpentine)
    forward, backward = _scan_order(w)

    for row_y in range(h):
        row = row_y * w
        reverse = serpentine and (row_y & 1) == 1
        xs = backward if reverse else forward
        for x in xs:
            i = row + x
            aX = ex[i]
            aY = ey[i]
            aZ = ez[i]
            if clamp > 0.0:
                if aX > clamp: aX = clamp
                elif aX < -clamp: aX = -clamp
                if aY > clamp: aY = clamp
                elif aY < -clamp: aY = -clamp
                if aZ > clamp: aZ = clamp
                elif aZ < -clamp: aZ = -clamp

            X = tx[i] + aX
            Y = ty[i] + aY
            Z = tz[i] + aZ

            # Ink choice still happens in perceptual Lab.  With dot-gain
            # compensation X/Y/Z above live in the Yule-Nielsen additive
            # domain, so decode the accumulated value before measuring dE.
            if mix_n != 1.0:
                gain = max(Y, 0.0) ** (mix_n - 1.0)
                decision_X = X * gain
                decision_Y = Y * gain
                decision_Z = Z * gain
            else:
                decision_X, decision_Y, decision_Z = X, Y, Z

            # Inline XYZ -> Lab; the branch handles the negative values that
            # accumulated error legitimately produces.
            t = decision_X / _XN
            fx = t**_THIRD if t > _LAB_EPS else t / _LAB_KAPPA + 0.13793103448275862
            t = decision_Y / _YN
            fy = t**_THIRD if t > _LAB_EPS else t / _LAB_KAPPA + 0.13793103448275862
            t = decision_Z / _ZN
            fz = t**_THIRD if t > _LAB_EPS else t / _LAB_KAPPA + 0.13793103448275862
            L = 116.0 * fy - 16.0
            A = 500.0 * (fx - fy)
            B = 200.0 * (fy - fz)

            # Blue noise only nudges the decision; the residual is still
            # measured against the true value, so average tone stays correct.
            dL = L + offs[i] if offs is not None else L

            p = pen[i]
            best = 0
            best_d = 1e30
            for k in range(nk):
                d0 = dL - pl[k]
                d1 = A - pa[k]
                d2 = B - pb[k]
                d = sqrt(d0 * d0 + d1 * d1 + d2 * d2)
                if chromatic[k]:
                    d += p
                if d < best_d:
                    best_d = d
                    best = k

            codes[i] = pcode[best]

            s = strength if atten is None else strength * atten[i]
            if s <= 0.0:
                continue
            rX = (X - px[best]) * s
            rY = (Y - py[best]) * s
            rZ = (Z - pz[best]) * s
            if cscale is not None:
                cs = cscale[i]
                if cs != 1.0:
                    # Damp only the part of the residual that is off the
                    # neutral axis; luminance is never touched.
                    mX = rY * _XN
                    mZ = rY * _ZN
                    rX = mX + (rX - mX) * cs
                    rZ = mZ + (rZ - mZ) * cs

            for dx, dy, wgt in kernel:
                nx = x - dx if reverse else x + dx
                if nx < 0 or nx >= w:
                    continue
                ny = row_y + dy
                if ny >= h:
                    continue
                j = ny * w + nx
                ex[j] += rX * wgt
                ey[j] += rY * wgt
                ez[j] += rZ * wgt

    return np.frombuffer(bytes(codes), dtype=np.uint8).reshape(h, w)


# --------------------------------------------------------------------------
# Error diffusion, residual carried in Lab (for comparison)
# --------------------------------------------------------------------------


def _error_diffusion_lab(target_lab, profile, params, *, gate_open, edge):
    h, w = target_lab.shape[:2]
    n = h * w

    kernel = KERNELS[params.algorithm]
    pal = profile.lab
    pl = [float(v) for v in pal[:, 0]]
    pa = [float(v) for v in pal[:, 1]]
    pb = [float(v) for v in pal[:, 2]]
    pcode = [int(v) for v in profile.device_codes]
    chromatic = [bool(v) for v in ~profile.achromatic_mask]
    nk = len(pl)

    tl = target_lab[..., 0].ravel().tolist()
    ta = target_lab[..., 1].ravel().tolist()
    tb = target_lab[..., 2].ravel().tolist()

    pen, atten, offs = _side_inputs((h, w), params, gate_open, edge)
    cscale = _chroma_error_scale(params, gate_open, n)

    el = [0.0] * n
    ea = [0.0] * n
    eb = [0.0] * n
    codes = bytearray(n)

    strength = float(params.strength)
    clamp = float(params.error_clamp)
    serpentine = bool(params.serpentine)
    forward, backward = _scan_order(w)

    for row_y in range(h):
        row = row_y * w
        reverse = serpentine and (row_y & 1) == 1
        xs = backward if reverse else forward
        for x in xs:
            i = row + x
            aL, aA, aB = el[i], ea[i], eb[i]
            if clamp > 0.0:
                if aL > clamp: aL = clamp
                elif aL < -clamp: aL = -clamp
                if aA > clamp: aA = clamp
                elif aA < -clamp: aA = -clamp
                if aB > clamp: aB = clamp
                elif aB < -clamp: aB = -clamp

            L = tl[i] + aL
            A = ta[i] + aA
            B = tb[i] + aB
            dL = L + offs[i] if offs is not None else L

            p = pen[i]
            best = 0
            best_d = 1e30
            for k in range(nk):
                d0 = dL - pl[k]
                d1 = A - pa[k]
                d2 = B - pb[k]
                d = sqrt(d0 * d0 + d1 * d1 + d2 * d2)
                if chromatic[k]:
                    d += p
                if d < best_d:
                    best_d = d
                    best = k

            codes[i] = pcode[best]

            s = strength if atten is None else strength * atten[i]
            if s <= 0.0:
                continue
            cs = 1.0 if cscale is None else cscale[i]
            rL = (L - pl[best]) * s
            rA = (A - pa[best]) * s * cs
            rB = (B - pb[best]) * s * cs

            for dx, dy, wgt in kernel:
                nx = x - dx if reverse else x + dx
                if nx < 0 or nx >= w:
                    continue
                ny = row_y + dy
                if ny >= h:
                    continue
                j = ny * w + nx
                el[j] += rL * wgt
                ea[j] += rA * wgt
                eb[j] += rB * wgt

    return np.frombuffer(bytes(codes), dtype=np.uint8).reshape(h, w)


def error_diffusion(
    target_lab: np.ndarray,
    profile: PaletteProfile,
    params: DitherParams,
    *,
    gate_open: np.ndarray | None = None,
    edge: np.ndarray | None = None,
) -> np.ndarray:
    if params.error_space == "lab" and params.dot_gain_compensation:
        raise ValueError("dot-gain compensation requires error_space='linear'")
    fn = _error_diffusion_lab if params.error_space == "lab" else _error_diffusion_linear
    return fn(target_lab, profile, params, gate_open=gate_open, edge=edge)


# --------------------------------------------------------------------------
# Nearest colour (no halftone)
# --------------------------------------------------------------------------


def nearest_codes(
    target_lab: np.ndarray,
    profile: PaletteProfile,
    *,
    gate_open: np.ndarray | None = None,
    chroma_penalty: float = 0.0,
    delta_e: str = "76",
) -> np.ndarray:
    fn = C.DELTA_E[delta_e]
    d = fn(target_lab[..., None, :], profile.lab)  # (H, W, N)
    if gate_open is not None and chroma_penalty > 0:
        pen = chroma_penalty * (1.0 - np.asarray(gate_open))[..., None] * (~profile.achromatic_mask)
        d = d + pen
    idx = np.argmin(d, axis=-1)
    return profile.device_codes[idx]


# --------------------------------------------------------------------------
# Ordered blue noise over ink pairs
# --------------------------------------------------------------------------


def ordered_bluenoise(
    target_lab: np.ndarray,
    profile: PaletteProfile,
    params: DitherParams,
    *,
    gate_open: np.ndarray | None = None,
    edge: np.ndarray | None = None,
    delta_e: str = "76",
) -> np.ndarray:
    h, w = target_lab.shape[:2]
    pal_xyz = profile.xyz
    mix_n = profile.yule_nielsen_n if params.dot_gain_compensation else 1.0
    pal_work = C.yule_nielsen_encode_xyz(pal_xyz, mix_n)
    codes = profile.device_codes
    chromatic = (~profile.achromatic_mask).astype(np.float64)
    nk = pal_xyz.shape[0]

    tgt_xyz = C.lab_to_xyz(target_lab)
    tgt_work = C.yule_nielsen_encode_xyz(tgt_xyz, mix_n)
    fn = C.DELTA_E[delta_e]

    closed = np.zeros((h, w)) if gate_open is None else (1.0 - np.asarray(gate_open))
    penalty = params.chroma_penalty * closed

    best_score = np.full((h, w), np.inf)
    best_a = np.zeros((h, w), dtype=np.uint8)
    best_b = np.zeros((h, w), dtype=np.uint8)
    best_t = np.zeros((h, w))

    for i in range(nk):
        for j in range(i + 1, nk):
            a = pal_work[i]
            d = pal_work[j] - a
            denom = float(np.dot(d, d))
            if denom < 1e-12:
                continue
            # Mixing is linear in the selected optical model's additive domain,
            # so the best two-ink approximation is the orthogonal projection of
            # the target onto that segment.
            t = np.clip(((tgt_work - a) @ d) / denom, 0.0, 1.0)
            mixed = a + t[..., None] * d
            optical = C.yule_nielsen_decode_xyz(mixed, mix_n)
            score = fn(C.xyz_to_lab(optical), target_lab)
            # Charge the chroma penalty in proportion to how much chromatic
            # ink the mix actually spends.
            score = score + penalty * ((1.0 - t) * chromatic[i] + t * chromatic[j])

            better = score < best_score
            best_score = np.where(better, score, best_score)
            best_a = np.where(better, i, best_a).astype(np.uint8)
            best_b = np.where(better, j, best_b).astype(np.uint8)
            best_t = np.where(better, t, best_t)

    if edge is not None and params.edge_suppress > 0:
        # Pull the mixing fraction toward 0 or 1 on edges: less halftone
        # texture riding on top of a contour.
        e = params.edge_suppress * np.clip(np.asarray(edge), 0.0, 1.0)
        best_t = best_t + e * (np.round(best_t) - best_t)

    mask = bn_mod.tile(bn_mod.void_and_cluster(params.mask_size, params.mask_sigma), h, w)
    out = np.where(mask < best_t, codes[best_b], codes[best_a])
    return out.astype(np.uint8)


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def dither(
    target_lab: np.ndarray,
    profile: PaletteProfile,
    params: DitherParams,
    *,
    gate_open: np.ndarray | None = None,
    edge: np.ndarray | None = None,
) -> np.ndarray:
    if params.algorithm == "nearest":
        return nearest_codes(
            target_lab, profile, gate_open=gate_open, chroma_penalty=params.chroma_penalty
        )
    if params.algorithm == "bluenoise":
        return ordered_bluenoise(target_lab, profile, params, gate_open=gate_open, edge=edge)
    if params.algorithm not in KERNELS:
        raise ValueError(f"unknown algorithm {params.algorithm!r}; try one of {ALGORITHMS}")
    return error_diffusion(target_lab, profile, params, gate_open=gate_open, edge=edge)
