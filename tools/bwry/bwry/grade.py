"""Global palette-aware colour grading before gamut mapping.

Strict colorimetry is not always the most convincing rendering on a B/W/R/Y
panel.  A photograph whose visual identity is blue sky and green foliage loses
that identity when unsupported hues are merely compressed to the neutral axis.
Palette grading treats the four inks as an artistic palette first: the whole
image is translated into a coherent warm photographic look, then the ordinary
physical gamut mapper and halftoner can do their jobs without isolated patches
of colour floating in an otherwise monochrome frame.
"""

from __future__ import annotations

import numpy as np

from . import color as C
from .palette import PaletteProfile


STYLES = ("natural", "vintage", "selective-vintage")


def _smoothstep(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    t = np.clip((x - lo) / max(hi - lo, 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def vintage_warm(
    lab: np.ndarray,
    profile: PaletteProfile,
    *,
    strength: float | np.ndarray = 0.88,
    base_chroma: float = 7.0,
    max_chroma: float = 42.0,
) -> np.ndarray:
    """Translate a photograph into a cohesive red/yellow aged-print palette.

    Source hue still carries semantic structure, but only inside the printable
    warm arc: blue/purple leans brick red, green leans yellow/ochre, and already
    warm colours land near their corresponding ink. Low-chroma regions receive
    a small amber cast so colour is a property of the whole rendering rather
    than isolated source-red/source-yellow objects. The tint fades to zero at
    pure black and paper white, keeping both endpoints clean.
    """
    src = np.asarray(lab, dtype=np.float64)
    lch = C.lab_to_lch(src)
    lightness, chroma, hue = lch[..., 0], lch[..., 1], lch[..., 2]

    red_h = float(C.hue_deg(profile.lab[profile.index_of("red")]))
    yellow_h = float(C.hue_deg(profile.lab[profile.index_of("yellow")]))
    warm_span = (yellow_h - red_h) % 360.0
    if warm_span > 180.0:
        red_h, yellow_h = yellow_h, red_h
        warm_span = (yellow_h - red_h) % 360.0

    # Relative affinity keeps existing red/yellow near their inks, sends green
    # toward ochre and sends the blue/purple side of the wheel toward brick red.
    sigma = 68.0
    red_affinity = np.exp(-0.5 * (C.hue_distance(hue, red_h) / sigma) ** 2)
    yellow_affinity = np.exp(-0.5 * (C.hue_distance(hue, yellow_h) / sigma) ** 2)
    yellow_mix = yellow_affinity / np.maximum(red_affinity + yellow_affinity, 1e-12)

    colourful = _smoothstep(chroma, 3.0, 32.0)
    # Undefined/noisy hue in neutrals should converge on one stable sepia hue.
    sepia_mix = 0.67
    yellow_mix = sepia_mix + colourful * (yellow_mix - sepia_mix)
    target_hue = (red_h + yellow_mix * warm_span) % 360.0

    x = np.clip(lightness / 100.0, 0.0, 1.0)
    endpoint_fade = np.power(np.maximum(np.sin(np.pi * x), 0.0), 0.72)
    target_chroma = endpoint_fade * (
        base_chroma + colourful * (max_chroma - base_chroma)
    )
    target = C.lch_to_lab(np.stack([lightness, target_chroma, target_hue], axis=-1))

    amount = np.clip(np.asarray(strength, dtype=np.float64), 0.0, 1.0)
    if amount.ndim:
        amount = amount[..., None]
    out = src.copy()
    out[..., 1:] += amount * (target[..., 1:] - out[..., 1:])
    return out


def selective_vintage_warm(
    lab: np.ndarray,
    profile: PaletteProfile,
    *,
    strength: float | np.ndarray = 0.84,
    base_chroma: float = 2.5,
    max_chroma: float = 34.0,
) -> np.ndarray:
    """Locally translate unsupported hues without crossing the neutral axis.

    ``vintage_warm`` deliberately interpolates in Cartesian Lab.  That is a
    useful conventional grade at high strength, but a half-strength move from
    cyan/green/purple to the warm ink arc can pass straight through a*=b*=0.
    An adaptive controller is especially likely to choose that middle amount,
    making a moderately out-of-gamut image *less* colourful than either end.

    This variant rotates hue in polar LCh and interpolates chroma separately.
    A source region that is visibly coloured therefore stays coloured while
    its hue is translated toward the red/yellow palette.  The lower chroma
    ceiling and almost-zero neutral tint also keep already printable colours
    and paper-like backgrounds from becoming over-saturated.
    """
    src = np.asarray(lab, dtype=np.float64)
    lch = C.lab_to_lch(src)
    lightness, chroma, hue = lch[..., 0], lch[..., 1], lch[..., 2]

    red_h = float(C.hue_deg(profile.lab[profile.index_of("red")]))
    yellow_h = float(C.hue_deg(profile.lab[profile.index_of("yellow")]))
    warm_span = (yellow_h - red_h) % 360.0
    if warm_span > 180.0:
        red_h, yellow_h = yellow_h, red_h
        warm_span = (yellow_h - red_h) % 360.0

    sigma = 68.0
    red_affinity = np.exp(-0.5 * (C.hue_distance(hue, red_h) / sigma) ** 2)
    yellow_affinity = np.exp(-0.5 * (C.hue_distance(hue, yellow_h) / sigma) ** 2)
    yellow_mix = yellow_affinity / np.maximum(red_affinity + yellow_affinity, 1e-12)

    colourful = _smoothstep(chroma, 4.0, 28.0)
    # The hue of near-neutrals is noise.  Give the very small optional tint one
    # stable direction, but let genuinely coloured pixels retain their semantic
    # ordering along the available red-to-yellow arc.
    yellow_mix = 0.67 + colourful * (yellow_mix - 0.67)
    target_hue = (red_h + yellow_mix * warm_span) % 360.0

    x = np.clip(lightness / 100.0, 0.0, 1.0)
    endpoint_fade = np.power(np.maximum(np.sin(np.pi * x), 0.0), 0.72)
    target_chroma = endpoint_fade * (
        base_chroma + colourful * (max_chroma - base_chroma)
    )

    amount = np.clip(np.asarray(strength, dtype=np.float64), 0.0, 1.0)
    # Shortest circular interpolation changes hue without taking the Cartesian
    # shortcut through zero chroma.
    hue_delta = (target_hue - hue + 180.0) % 360.0 - 180.0
    out_hue = (hue + amount * hue_delta) % 360.0
    out_chroma = chroma + amount * (target_chroma - chroma)
    out_lch = np.stack([lightness, np.maximum(out_chroma, 0.0), out_hue], axis=-1)
    return C.lch_to_lab(out_lch)


def adaptive_grade_amount(
    source_lab: np.ndarray,
    faithful_lab: np.ndarray,
    *,
    max_strength: float = 0.88,
) -> tuple[np.ndarray, dict]:
    """Choose local and global style strength from destination-gamut loss.

    ``faithful_lab`` is the same image hard-clipped into the measured gamut
    with no aesthetic remapping.  Its dE from the source is therefore a direct
    map of what the panel cannot say faithfully.  The image-wide term answers
    "how much of this photograph needs translation?"; the local term prevents
    already printable reds and yellows from being needlessly restyled.

    The non-zero 12% floor keeps a set of images visually related, while an
    image made mostly of printable colours receives only a very light grade.
    """
    source = np.asarray(source_lab, dtype=np.float64)
    faithful = np.asarray(faithful_lab, dtype=np.float64)
    if source.shape != faithful.shape:
        raise ValueError("source and faithful gamut image must have the same shape")

    chroma = C.chroma(source)
    colour_weight = _smoothstep(chroma, 3.0, 24.0)
    loss = _smoothstep(C.delta_e76(source, faithful), 2.5, 30.0)

    # Spatial mean is deliberate: one unsupported blue logo should not make an
    # otherwise printable red/yellow poster look like an aged photograph.
    severity = float(np.mean(loss * colour_weight))
    global_factor = 0.12 + 0.88 * float(_smoothstep(np.asarray(severity), 0.08, 0.48))
    local_factor = 0.35 + 0.65 * loss
    amount = np.clip(float(max_strength), 0.0, 1.0) * global_factor * local_factor
    return amount, {
        "adaptive": True,
        "unprintable_severity": round(severity, 4),
        "global_factor": round(global_factor, 4),
        "mean_effective_strength": round(float(np.mean(amount)), 4),
        "p95_effective_strength": round(float(np.percentile(amount, 95)), 4),
    }


def selective_grade_amount(
    source_lab: np.ndarray,
    faithful_lab: np.ndarray,
    *,
    max_strength: float = 0.84,
) -> tuple[np.ndarray, dict]:
    """Style only the colour information the destination would otherwise lose.

    The controller distinguishes *visible colour loss* from ordinary dE.  A
    printable red can have a moderate lightness/chroma error yet still read as
    the same red, so it is protected.  A cyan that hue-preserving compression
    turns nearly neutral has high translation need even if it occupies only a
    small part of the image.
    """
    source = np.asarray(source_lab, dtype=np.float64)
    faithful = np.asarray(faithful_lab, dtype=np.float64)
    if source.shape != faithful.shape:
        raise ValueError("source and faithful gamut image must have the same shape")

    source_lch = C.lab_to_lch(source)
    faithful_lch = C.lab_to_lch(faithful)
    source_c = source_lch[..., 1]
    faithful_c = faithful_lch[..., 1]
    colour_weight = _smoothstep(source_c, 5.0, 22.0)
    visible = _smoothstep(faithful_c, 4.0, 11.0)
    hue_error = C.hue_distance(source_lch[..., 2], faithful_lch[..., 2])
    hue_loss = _smoothstep(hue_error, 18.0, 75.0)

    # "Native" means that a strict mapping retains both visible chroma and the
    # source hue.  Those pixels receive only a trace of the style.  Unsupported
    # but still visibly coloured hues get some translation; hues collapsing to
    # grey get the full amount.
    native = colour_weight * visible * (1.0 - _smoothstep(hue_error, 10.0, 38.0))
    translation_need = colour_weight * ((1.0 - visible) + 0.58 * visible * hue_loss)
    translation_need = np.clip(translation_need, 0.0, 1.0)

    severity = float(np.mean(translation_need))
    image_factor = 0.55 + 0.45 * float(
        _smoothstep(np.asarray(severity), 0.06, 0.42)
    )
    local_factor = 0.04 + 0.96 * translation_need
    protection = 1.0 - 0.78 * native
    amount = (
        np.clip(float(max_strength), 0.0, 1.0)
        * image_factor
        * local_factor
        * protection
    )
    return amount, {
        "adaptive": True,
        "controller": "visible-colour-retention-v1",
        "unprintable_severity": round(severity, 4),
        "image_factor": round(image_factor, 4),
        "native_colour_area": round(float(np.mean(native)), 4),
        "mean_translation_need": round(float(np.mean(translation_need)), 4),
        "mean_effective_strength": round(float(np.mean(amount)), 4),
        "p95_effective_strength": round(float(np.percentile(amount, 95)), 4),
    }


def apply_palette_grade(
    lab: np.ndarray,
    profile: PaletteProfile,
    *,
    style: str = "natural",
    strength: float | np.ndarray = 0.88,
    adaptive_meta: dict | None = None,
) -> tuple[np.ndarray, dict]:
    if style == "natural":
        return np.asarray(lab, dtype=np.float64), {"style": style, "strength": 0.0}
    if style == "vintage":
        out = vintage_warm(lab, profile, strength=strength)
        strength_array = np.asarray(strength, dtype=np.float64)
        meta = {
            "style": style,
            "strength": round(float(np.mean(strength_array)), 4),
            "mean_chroma_before": round(float(np.mean(C.chroma(lab))), 2),
            "mean_chroma_after": round(float(np.mean(C.chroma(out))), 2),
        }
        if adaptive_meta:
            meta.update(adaptive_meta)
        return out, meta
    if style == "selective-vintage":
        out = selective_vintage_warm(lab, profile, strength=strength)
        strength_array = np.asarray(strength, dtype=np.float64)
        meta = {
            "style": style,
            "strength": round(float(np.mean(strength_array)), 4),
            "interpolation": "polar-lch",
            "mean_chroma_before": round(float(np.mean(C.chroma(lab))), 2),
            "mean_chroma_after": round(float(np.mean(C.chroma(out))), 2),
        }
        if adaptive_meta:
            meta.update(adaptive_meta)
        return out, meta
    raise ValueError(f"unknown colour style {style!r}; try one of {STYLES}")
