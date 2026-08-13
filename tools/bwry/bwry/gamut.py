"""Gamut mapping into the reachable colour set of a four-ink palette.

Key fact this module rests on: a halftone made of four inks is additive in its
optical mixing domain.  For an ideal panel that is linear XYZ; for the measured
Note4C it is the Yule-Nielsen domain fitted by the calibration chart.  In that
domain the reachable set is exactly the convex hull of the four ink colours --
a tetrahedron.

So instead of clipping saturated sRGB and letting error diffusion smear the
leftovers into red/yellow confetti, we first map every pixel into that
tetrahedron and only then dither.  Two intents are available: hue-preserving
compression for fidelity, and a vivid intent which may trade hue accuracy for
visible colourfulness on a palette with only red and yellow chromatic inks.
"""

from __future__ import annotations

import numpy as np

from . import color as C
from .palette import PaletteProfile


class GamutHull:
    """Tetrahedral 4-ink hull in an optical additive mixing domain."""

    def __init__(self, profile: PaletteProfile, mixing_n: float = 1.0):
        self.profile = profile
        self.mixing_n = float(mixing_n)
        v = C.yule_nielsen_encode_xyz(profile.xyz, self.mixing_n)  # (4, 3), additive domain
        if v.shape[0] != 4:
            raise ValueError("GamutHull currently assumes exactly 4 inks")
        self.vertices = v
        self.v0 = v[0]
        # Column matrix of edge vectors from v0; barycentric solve is one 3x3.
        self._m_inv = np.linalg.inv(np.stack([v[1] - v[0], v[2] - v[0], v[3] - v[0]], axis=1))

        # Neutral axis of the *medium*: the black<->white segment.
        self.black_mix = v[profile.index_of("black")]
        self.white_mix = v[profile.index_of("white")]
        self.black_xyz = profile.xyz[profile.index_of("black")]
        self.white_xyz = profile.xyz[profile.index_of("white")]

        # Hue / lightness of the chromatic inks, used for cusp-aware mapping.
        lab = profile.lab
        chromatic = ~profile.achromatic_mask
        self.cusp_hue = C.hue_deg(lab[chromatic])
        self.cusp_l = lab[chromatic, 0]
        self.max_chroma = float(np.max(C.chroma(lab))) if chromatic.any() else 0.0
        self._sample_cache: dict[int, tuple[np.ndarray, np.ndarray]] = {}

    # ------------------------------------------------------------------
    def barycentric(self, xyz: np.ndarray) -> np.ndarray:
        """(..., 3) XYZ -> (..., 4) barycentric weights over the four inks."""
        mixed = C.yule_nielsen_encode_xyz(xyz, self.mixing_n)
        d = np.asarray(mixed, dtype=np.float64) - self.v0
        w = d @ self._m_inv.T
        w0 = 1.0 - w.sum(axis=-1, keepdims=True)
        return np.concatenate([w0, w], axis=-1)

    #: Barycentric slack. A point sitting exactly on the hull surface reads as
    #: very slightly outside once it has been through Lab and back; measured
    #: worst case is -6.2e-6, and it does not shrink with more bisection
    #: iterations, so it is round-trip float noise rather than convergence.
    #: 1e-5 of the gamut's extent is about dE 0.002 -- far below anything that
    #: matters, and still four orders of magnitude tighter than a colour that
    #: is genuinely out of gamut.
    TOL = 1e-5

    def contains(self, xyz: np.ndarray, tol: float | None = None) -> np.ndarray:
        t = self.TOL if tol is None else tol
        return np.all(self.barycentric(xyz) >= -t, axis=-1)

    def neutral_lab_at(self, l: np.ndarray) -> np.ndarray:
        """Point on the black<->white segment whose L* is ``l`` (clamped)."""
        lab_k = C.xyz_to_lab(self.black_xyz)
        lab_w = C.xyz_to_lab(self.white_xyz)
        if self.mixing_n == 1.0:
            t = np.clip((l - lab_k[0]) / max(lab_w[0] - lab_k[0], 1e-9), 0.0, 1.0)[..., None]
        else:
            # L* is a function of Y only. Solve for the requested physical Y,
            # encode it into the additive Yule-Nielsen domain, then interpolate
            # between the measured black/white vertices in that domain.
            fy = (np.asarray(l, dtype=np.float64) + 16.0) / 116.0
            target_y = np.where(
                fy > 6.0 / 29.0,
                fy**3,
                (fy - 4.0 / 29.0) * 3.0 * (6.0 / 29.0) ** 2,
            )
            target_mix_y = np.power(np.maximum(target_y, 0.0), 1.0 / self.mixing_n)
            denom = max(float(self.white_mix[1] - self.black_mix[1]), 1e-9)
            t = np.clip((target_mix_y - self.black_mix[1]) / denom, 0.0, 1.0)[..., None]
        mixed = self.black_mix + t * (self.white_mix - self.black_mix)
        xyz = C.yule_nielsen_decode_xyz(mixed, self.mixing_n)
        return C.xyz_to_lab(xyz)

    def sample(self, steps: int = 18) -> tuple[np.ndarray, np.ndarray]:
        """Uniform barycentric samples of the physical gamut as ``(XYZ, Lab)``."""
        steps = max(2, int(steps))
        if steps not in self._sample_cache:
            weights = []
            for a in range(steps + 1):
                for b in range(steps + 1 - a):
                    for c in range(steps + 1 - a - b):
                        d = steps - a - b - c
                        weights.append((a, b, c, d))
            w = np.asarray(weights, dtype=np.float64) / steps
            mixed = w @ self.vertices
            xyz = C.yule_nielsen_decode_xyz(mixed, self.mixing_n)
            self._sample_cache[steps] = (xyz, C.xyz_to_lab(xyz))
        return self._sample_cache[steps]

    def cusp_lightness(self, hue: np.ndarray, sigma: float = 55.0) -> np.ndarray:
        """L* of the most-saturated reachable colour near ``hue`` (degrees).

        With only two chromatic inks a proper cusp search is overkill; a
        hue-weighted blend of the chromatic inks' own L* is within a couple of
        L* units and is smooth, which matters more here.
        """
        if self.cusp_hue.size == 0:
            return np.zeros_like(hue)
        d = C.hue_distance(hue[..., None], self.cusp_hue)
        w = np.exp(-0.5 * (d / sigma) ** 2)
        w = w / np.maximum(w.sum(axis=-1, keepdims=True), 1e-12)
        return (w * self.cusp_l).sum(axis=-1)


def compress_into_gamut(
    lab: np.ndarray,
    hull: GamutHull,
    *,
    knee: float = 0.80,
    l_adapt: float = 0.35,
    iterations: int = 18,
) -> np.ndarray:
    """Hue-preserving soft compression of ``lab`` into ``hull``.

    Parameters
    ----------
    knee
        Fraction of the boundary distance kept uncompressed. ``1.0`` is a hard
        clip (cheap, but bands); lower values roll saturated colours off
        smoothly and also pull in-gamut colours in slightly, which is what
        keeps gradients from flat-topping.
    l_adapt
        ``0`` compresses at constant L* (colorimetrically strict, but bright
        reds go grey). ``1`` slides the anchor all the way to the hue's cusp
        lightness, trading lightness accuracy for saturation. ``0.3-0.5`` is a
        good photographic compromise.
    iterations
        Bisection steps used to locate the hull boundary. 18 is ~4e-6 of the
        segment, far below a just-noticeable difference.
    """
    lab = np.asarray(lab, dtype=np.float64)
    lch = C.lab_to_lch(lab)
    l, c, h = lch[..., 0], lch[..., 1], lch[..., 2]

    # How chromatic is this pixel, relative to what the inks can reach? Both
    # the cusp anchor shift and the soft knee are tools for *chroma*, and both
    # do damage if they are allowed to act on near-neutral pixels: the knee in
    # particular would roll paper white back off the hull's white vertex, and
    # a background that is a hair below white gets a sparse speckle of black
    # instead of reading as clean paper.
    c_ref = max(0.25 * hull.max_chroma, 1e-6)
    w = np.clip(c / c_ref, 0.0, 1.0)
    w = w * w * (3.0 - 2.0 * w)

    # Anchor: a point guaranteed inside the hull, on the achromatic axis.
    l_anchor = l + (l_adapt * w) * (hull.cusp_lightness(h) - l)
    anchor = hull.neutral_lab_at(l_anchor)
    # Neutral pixels get a hard clip (knee 1.0); saturated ones get the full
    # roll-off, which is what keeps saturated gradients from flat-topping.
    knee_eff = 1.0 - (1.0 - knee) * w

    # Bisect for the boundary crossing along anchor -> target, searching past
    # t = 1 as well so that in-gamut colours still know how much headroom they
    # have. Without that the knee could not roll off gradients smoothly.
    t_max = 4.0
    lo = np.zeros_like(l)
    hi = np.full_like(l, t_max)
    delta = lab - anchor
    for _ in range(iterations):
        mid = 0.5 * (lo + hi)
        probe = anchor + mid[..., None] * delta
        inside = hull.contains(C.lab_to_xyz(probe))
        lo = np.where(inside, mid, lo)
        hi = np.where(inside, hi, mid)
    t_boundary = np.maximum(lo, 1e-6)

    # Soft roll-off; the target always sits at t = 1 by construction, so
    # everything below the knee passes through untouched.
    k = knee_eff * t_boundary
    span = np.maximum(t_boundary - k, 1e-9)
    compressed = k + span * (1.0 - np.exp(-np.maximum(1.0 - k, 0.0) / span))
    t = np.clip(np.where(k >= 1.0, 1.0, compressed), 0.0, 1.0)

    out = anchor + t[..., None] * delta

    # Chroma-free pixels have no direction to compress; keep them on the axis.
    achromatic = c < 1e-6
    return np.where(achromatic[..., None], anchor, out)


def map_vivid_into_gamut(
    lab: np.ndarray,
    hull: GamutHull,
    *,
    strength: float = 0.80,
    lightness_weight: float = 4.0,
    hue_weight: float = 0.10,
    chroma_weight: float = 0.65,
    neutral_lo: float = 4.0,
    neutral_hi: float = 22.0,
    sample_steps: int = 18,
    knee: float = 0.80,
    l_adapt: float = 0.35,
) -> np.ndarray:
    """Saturation-style mapping for extremely narrow destination gamuts.

    A hue-preserving map is the right default for faithful reproduction, but a
    B/W/R/Y panel has no blue or green cusp at all: strict hue preservation
    collapses most cool colours onto the neutral axis.  This intent searches
    the *measured physical gamut* for a point with similar lightness and chroma,
    while deliberately discounting hue error.  It then blends that point with
    the ordinary hue-preserving result in the additive optical domain.

    Neutrals never move.  ``strength`` controls the preference trade-off, while
    ``hue_weight`` controls how readily blue/green content is represented by
    the available red/yellow inks.  This is analogous to an ICC saturation
    intent and is therefore an opt-in aesthetic rendering, not colorimetry.
    """
    source = np.asarray(lab, dtype=np.float64)
    base = compress_into_gamut(source, hull, knee=knee, l_adapt=l_adapt)
    if strength <= 0.0:
        return base

    source_lch = C.lab_to_lch(source)
    source_c = source_lch[..., 1]
    blend = np.clip((source_c - neutral_lo) / max(neutral_hi - neutral_lo, 1e-9), 0.0, 1.0)
    blend = blend * blend * (3.0 - 2.0 * blend) * np.clip(strength, 0.0, 1.0)
    active = blend > 1e-6
    if not np.any(active):
        return base

    candidate_xyz, candidate_lab = hull.sample(sample_steps)
    candidate_lch = C.lab_to_lch(candidate_lab)
    c_l = candidate_lch[:, 0]
    c_c = candidate_lch[:, 1]
    c_h = candidate_lch[:, 2]

    target = source_lch[active]
    chosen = np.empty(target.shape[0], dtype=np.int32)
    # Keep peak working memory bounded: 1024 x 1330 candidates is ~11 MB per
    # score plane with the default sample grid.
    for start in range(0, target.shape[0], 1024):
        stop = min(start + 1024, target.shape[0])
        t = target[start:stop]
        t_l = t[:, 0, None]
        t_c = np.minimum(t[:, 1, None], hull.max_chroma)
        t_h = t[:, 2, None]

        dl2 = (t_l - c_l[None, :]) ** 2
        dc2 = (t_c - c_c[None, :]) ** 2
        dh = np.radians(C.hue_distance(t_h, c_h[None, :]))
        # Chroma-weighted angular distance is the hue component of a Lab
        # distance. Discounting it is the deliberate saturation-intent trade.
        dh2 = 4.0 * t_c * c_c[None, :] * np.sin(0.5 * dh) ** 2
        score = lightness_weight * dl2 + chroma_weight * dc2 + hue_weight * dh2
        chosen[start:stop] = np.argmin(score, axis=1)

    vivid_xyz = C.lab_to_xyz(base)
    vivid_xyz[active] = candidate_xyz[chosen]
    base_mix = C.yule_nielsen_encode_xyz(C.lab_to_xyz(base), hull.mixing_n)
    vivid_mix = C.yule_nielsen_encode_xyz(vivid_xyz, hull.mixing_n)
    out_mix = base_mix + blend[..., None] * (vivid_mix - base_mix)
    return C.xyz_to_lab(C.yule_nielsen_decode_xyz(out_mix, hull.mixing_n))


def map_into_gamut(
    lab: np.ndarray,
    hull: GamutHull,
    *,
    intent: str = "hue-preserving",
    knee: float = 0.80,
    l_adapt: float = 0.35,
    vivid_strength: float = 0.80,
    vivid_hue_weight: float = 0.10,
) -> np.ndarray:
    if intent == "hue-preserving":
        return compress_into_gamut(lab, hull, knee=knee, l_adapt=l_adapt)
    if intent == "vivid":
        return map_vivid_into_gamut(
            lab, hull,
            strength=vivid_strength,
            hue_weight=vivid_hue_weight,
            knee=knee,
            l_adapt=l_adapt,
        )
    raise ValueError(f"unknown gamut intent {intent!r}; try 'hue-preserving' or 'vivid'")


def gamut_report(hull: GamutHull) -> str:
    """Human-readable summary of what this palette can actually reproduce."""
    p = hull.profile
    lab = p.lab
    lines = [
        f"L* range      : {p.l_black:.1f} .. {p.l_white:.1f}  (span {p.l_white - p.l_black:.1f})",
        f"contrast      : {p.contrast_ratio:.1f}:1",
        f"max ink chroma: {hull.max_chroma:.1f}",
    ]
    for name, row in zip(p.names, lab):
        lch = C.lab_to_lch(row)
        lines.append(f"  {name:<6} L*={lch[0]:6.2f} C*={lch[1]:5.1f} h={lch[2]:6.1f}")
    return "\n".join(lines)
