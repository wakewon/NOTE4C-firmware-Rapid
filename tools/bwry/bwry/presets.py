"""Named recipes.

Two groups:

* **Presets** -- what you would actually ship for a given kind of content
  (``photo``, ``illustration``, ``text``).
* **A/B candidates** -- the matrix from the research plan. Each one isolates a
  single change from the one before it, so the on-panel comparison says which
  step earned its keep rather than just "the new one looks better".
"""

from __future__ import annotations

from copy import deepcopy

from .dither import DitherParams
from .pipeline import EdgeParams, Recipe
from .tone import ChromaGate, ToneParams

CAL = "note4c-measured-v1"
IDEAL = "note4c-ideal"


# --------------------------------------------------------------------------
# Shipping presets
# --------------------------------------------------------------------------


def photo(profile: str = CAL) -> Recipe:
    return Recipe(
        name="photo",
        description="Photographs. Moderate S-curve, strong local contrast to buy back the "
        "detail lost to the panel's compressed L* range, tight chroma gate.",
        profile=profile,
        tone=ToneParams(
            autocontrast=True,
            auto_low_pct=0.5,
            auto_high_pct=99.5,
            contrast=0.22,
            contrast_pivot=0.45,
            shadow_lift=0.14,
            highlight_compress=0.28,
            local_contrast=0.32,
            local_radius=6.0,
            local_detail=0.18,
            local_detail_radius=1.6,
            saturation=1.18,
        ),
        gate=ChromaGate(c_lo=3.5, c_hi=13.0, penalty=26.0),
        dither=DitherParams(algorithm="sierra2", serpentine=True, edge_suppress=0.45),
        edge=EdgeParams(low_pct=75.0, high_pct=97.0),
        gamut_knee=0.80,
        gamut_l_adapt=0.35,
    )


def illustration(profile: str = CAL) -> Recipe:
    return Recipe(
        name="illustration",
        description="Flat art, posters, comics, UI mockups. Keeps saturation, opens the "
        "chroma gate, leans on Atkinson for clean flat areas and crisp lines.",
        profile=profile,
        tone=ToneParams(
            autocontrast=True,
            auto_low_pct=1.0,
            auto_high_pct=99.0,
            contrast=0.14,
            shadow_lift=0.06,
            highlight_compress=0.12,
            local_contrast=0.14,
            local_radius=5.0,
            local_detail=0.10,
            saturation=1.45,
        ),
        gate=ChromaGate(c_lo=2.5, c_hi=9.0, penalty=18.0),
        dither=DitherParams(algorithm="atkinson", serpentine=True, edge_suppress=0.55),
        edge=EdgeParams(low_pct=70.0, high_pct=95.0),
        gamut_knee=0.90,
        gamut_l_adapt=0.55,
    )


def text(profile: str = CAL) -> Recipe:
    return Recipe(
        name="text",
        description="Screenshots and documents. Hard tone curve, chroma almost fully "
        "closed, error diffusion damped so glyph edges stay clean.",
        profile=profile,
        tone=ToneParams(
            autocontrast=True,
            auto_low_pct=2.0,
            auto_high_pct=98.0,
            contrast=0.50,
            contrast_pivot=0.50,
            shadow_lift=0.0,
            highlight_compress=0.0,
            local_contrast=0.45,
            local_radius=3.0,
            local_detail=0.25,
            local_detail_radius=1.2,
            saturation=0.55,
        ),
        gate=ChromaGate(c_lo=10.0, c_hi=28.0, penalty=45.0),
        dither=DitherParams(algorithm="floyd-steinberg", serpentine=True, strength=0.55, edge_suppress=0.85),
        edge=EdgeParams(low_pct=60.0, high_pct=92.0, dilate=1),
        gamut_knee=0.95,
        gamut_l_adapt=0.20,
    )


def legacy() -> Recipe:
    return Recipe(
        name="legacy",
        description="Exactly what ships today: ideal #000/#FFF/#F00/#FF0 palette, "
        "luma-weighted RGB distance, raster Floyd-Steinberg, no preprocessing.",
        profile=IDEAL,
        legacy=True,
    )


PRESETS = {
    "photo": photo,
    "illustration": illustration,
    "text": text,
    "legacy": legacy,
}


def get_preset(name: str, profile: str = CAL) -> Recipe:
    if name not in PRESETS:
        raise KeyError(f"unknown preset {name!r}; try one of {sorted(PRESETS)}")
    fn = PRESETS[name]
    return fn() if name == "legacy" else fn(profile)


# --------------------------------------------------------------------------
# A/B matrix
# --------------------------------------------------------------------------


def _tweak(base: Recipe, name: str, description: str, **kw) -> Recipe:
    r = deepcopy(base)
    r.name = name
    r.description = description
    for key, value in kw.items():
        if "." in key:
            head, tail = key.split(".", 1)
            setattr(getattr(r, head), tail, value)
        else:
            setattr(r, key, value)
    return r


def ab_matrix(profile: str = CAL) -> list[Recipe]:
    """The comparison ladder, in the order the research plan prioritises them."""
    flat_tone = ToneParams(
        autocontrast=False, contrast=0.0, shadow_lift=0.0, highlight_compress=0.0,
        local_contrast=0.0, local_detail=0.0, saturation=1.0,
    )
    off_gate = ChromaGate(enabled=False)
    p = photo(profile)
    yn = _tweak(
        p, "09b-sierra2-edge-yn", "Exact 09 pair with measured Yule-Nielsen dot-gain "
        "compensation and physical gamut (n from the profile).",
        **{"dither.algorithm": "sierra2", "dither.edge_suppress": 0.45,
           "dither.dot_gain_compensation": True},
    )
    cover = _tweak(
        yn, "09c-sierra2-yn-cover", "Exact 09b conversion with a 4:3 cover crop so a wide "
        "photo uses the full panel instead of spending 25% of it on white letterboxing.",
        fit="cover",
    )
    vivid = _tweak(
        cover, "09d-sierra2-vivid", "Adds saturation-intent gamut mapping. Neutral areas stay "
        "neutral; unsupported blue/green hues rotate gently toward the available red/yellow "
        "inks instead of collapsing to greyscale.",
        gamut_intent="vivid", gamut_vivid_strength=0.45,
    )
    vivid_strong = _tweak(
        vivid, "09e-vivid-strong", "Raises only saturation-intent strength from 45% to 70%, "
        "showing how much false-hue colourfulness this four-ink palette can usefully carry.",
        gamut_vivid_strength=0.70,
    )
    vivid_hybrid = _tweak(
        vivid_strong, "09f-vivid-hybrid", "Adds gentle tone-dependent blue-noise threshold modulation "
        "to break up diffusion worms without disturbing solid highlights or shadows.",
        **{"dither.blue_noise_amount": 0.25},
    )
    vintage = _tweak(
        cover, "09g-adaptive-vintage", "Adaptive aged-photo grade before hue-preserving gamut "
        "mapping: the measured loss of unsupported colours controls the overall strength, "
        "while already printable red/yellow regions receive less local restyling.",
        color_style="vintage", color_style_strength=0.88,
    )
    vintage_hybrid = _tweak(
        vintage, "09h-adaptive-vintage-hybrid", "Exact adaptive-vintage pair with gentle "
        "tone-dependent blue-noise threshold modulation to compare texture only.",
        **{"dither.blue_noise_amount": 0.25},
    )
    vintage_tetra = _tweak(
        vintage, "09i-adaptive-vintage-tetra", "Same adaptive look, but replaces sequential "
        "error diffusion with four-primary tetrahedral blue-noise screening: slightly less "
        "locally exact, with no scan-order worms or directional grain.",
        **{"dither.algorithm": "tetra-bluenoise"},
    )
    vivid_tetra = _tweak(
        vivid_strong, "09j-vivid-tetra", "Same vivid mapping with four-primary tetrahedral "
        "blue-noise screening, isolating texture from the colour-rendering intent.",
        **{"dither.algorithm": "tetra-bluenoise"},
    )

    recipes = [
        legacy(),
        # -- step 1: measured palette + Lab metric, nothing else changed -----
        Recipe(
            name="01-cal-lab-fs",
            description="Calibrated palette + Lab dE76 nearest colour, raster Floyd-Steinberg, "
            "residual still carried non-linearly. Isolates one thing: dropping the "
            "pure-RGB palette assumption.",
            profile=profile,
            tone=deepcopy(flat_tone),
            gate=deepcopy(off_gate),
            dither=DitherParams(algorithm="floyd-steinberg", serpentine=False,
                                chroma_penalty=0.0, error_space="lab"),
            gamut_knee=1.0,
            gamut_l_adapt=0.0,
        ),
        # -- step 2: error space ---------------------------------------------
        Recipe(
            name="02-linear-error",
            description="Same, but the residual is carried in linear light instead of Lab. "
            "This is the ink-density fix: a halftone averages linearly in reflectance, so "
            "diffusing perceptually leaves every flat area under-inked.",
            profile=profile,
            tone=deepcopy(flat_tone),
            gate=deepcopy(off_gate),
            dither=DitherParams(algorithm="floyd-steinberg", serpentine=False, chroma_penalty=0.0),
            gamut_knee=1.0,
            gamut_l_adapt=0.0,
        ),
        # -- step 3: tone + gamut mapping ------------------------------------
        Recipe(
            name="03-tone-gamut",
            description="Adds the EPD tone curve, local contrast and gamut compression. "
            "Still raster Floyd-Steinberg, still no chroma gate.",
            profile=profile,
            tone=deepcopy(p.tone),
            gate=deepcopy(off_gate),
            dither=DitherParams(algorithm="floyd-steinberg", serpentine=False, chroma_penalty=0.0),
            gamut_knee=p.gamut_knee,
            gamut_l_adapt=p.gamut_l_adapt,
        ),
        # -- step 4: chroma gate ---------------------------------------------
        _tweak(
            p, "04-chroma-gate",
            "Adds the chroma gate. Still raster Floyd-Steinberg, so the worm pattern is "
            "left in place for the next comparison.",
            **{"dither.algorithm": "floyd-steinberg", "dither.serpentine": False, "dither.edge_suppress": 0.0},
        ),
        # -- step 5: serpentine ----------------------------------------------
        _tweak(
            p, "05-fs-serpentine",
            "Same, but serpentine scanning. A direct read on how much of the directional "
            "texture is pure scan-order artefact.",
            **{"dither.algorithm": "floyd-steinberg", "dither.edge_suppress": 0.0},
        ),
        # -- step 6: kernel bake-off ------------------------------------------
        _tweak(p, "06-sierra2", "Sierra-2 serpentine. Primary candidate from the plan.",
               **{"dither.algorithm": "sierra2", "dither.edge_suppress": 0.0}),
        _tweak(p, "07-stucki", "Stucki serpentine: widest kernel, smoothest gradients, softest edges.",
               **{"dither.algorithm": "stucki", "dither.edge_suppress": 0.0}),
        _tweak(p, "08-atkinson", "Atkinson serpentine: drops 25% of the error, crisper and lighter, "
               "clips the extremes.",
               **{"dither.algorithm": "atkinson", "dither.edge_suppress": 0.0}),
        # -- step 7: edge aware ------------------------------------------------
        _tweak(p, "09-sierra2-edge", "Sierra-2 serpentine + edge-aware error attenuation. "
               "Expected best-of-class for photographs.",
               **{"dither.algorithm": "sierra2", "dither.edge_suppress": 0.45}),
        yn,
        cover,
        vivid,
        vivid_strong,
        vivid_hybrid,
        vintage,
        vintage_hybrid,
        vintage_tetra,
        vivid_tetra,
        # -- step 8: blue noise ------------------------------------------------
        _tweak(p, "10-bluenoise", "Ordered void-and-cluster blue noise over optimal ink pairs. "
               "No scan-order artefacts at all; second route from the plan.",
               **{"dither.algorithm": "bluenoise", "dither.edge_suppress": 0.35}),
        _tweak(p, "11-sierra2-bluenoise-hybrid",
               "Sierra-2 serpentine with blue-noise modulated decisions: error diffusion "
               "accuracy, blue-noise texture.",
               **{"dither.algorithm": "sierra2", "dither.edge_suppress": 0.45,
                  "dither.blue_noise_amount": 0.45}),
        # -- presets -------------------------------------------------------
        _tweak(illustration(profile), "12-illustration", illustration().description),
        _tweak(text(profile), "13-text", text().description),
    ]
    return recipes
