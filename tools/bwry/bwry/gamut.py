"""Gamut mapping into the reachable colour set of a four-ink palette.

Key fact this module rests on: a halftone made of four inks integrates
**linearly in reflectance**, so the set of colours the panel can actually
produce (once the eye averages over a small neighbourhood) is exactly the
convex hull of the four ink colours *in XYZ* -- a tetrahedron.

So instead of clipping saturated sRGB and letting error diffusion smear the
leftovers into red/yellow confetti, we first project every pixel onto that
tetrahedron, preserving hue, and only then dither.
"""

from __future__ import annotations

import numpy as np

from . import color as C
from .palette import PaletteProfile


class GamutHull:
    """Tetrahedral hull of a 4-ink palette in XYZ, with barycentric tests."""

    def __init__(self, profile: PaletteProfile):
        self.profile = profile
        v = profile.xyz  # (4, 3)
        if v.shape[0] != 4:
            raise ValueError("GamutHull currently assumes exactly 4 inks")
        self.v0 = v[0]
        # Column matrix of edge vectors from v0; barycentric solve is one 3x3.
        self._m_inv = np.linalg.inv(np.stack([v[1] - v[0], v[2] - v[0], v[3] - v[0]], axis=1))

        # Neutral axis of the *medium*: the black<->white segment.
        self.black_xyz = v[profile.index_of("black")]
        self.white_xyz = v[profile.index_of("white")]

        # Hue / lightness of the chromatic inks, used for cusp-aware mapping.
        lab = profile.lab
        chromatic = ~profile.achromatic_mask
        self.cusp_hue = C.hue_deg(lab[chromatic])
        self.cusp_l = lab[chromatic, 0]
        self.max_chroma = float(np.max(C.chroma(lab))) if chromatic.any() else 0.0

    # ------------------------------------------------------------------
    def barycentric(self, xyz: np.ndarray) -> np.ndarray:
        """(..., 3) XYZ -> (..., 4) barycentric weights over the four inks."""
        d = np.asarray(xyz, dtype=np.float64) - self.v0
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
        t = np.clip((l - lab_k[0]) / max(lab_w[0] - lab_k[0], 1e-9), 0.0, 1.0)[..., None]
        # Interpolate in XYZ (physically what a black/white halftone does),
        # then report in Lab.
        xyz = self.black_xyz + t * (self.white_xyz - self.black_xyz)
        return C.xyz_to_lab(xyz)

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
