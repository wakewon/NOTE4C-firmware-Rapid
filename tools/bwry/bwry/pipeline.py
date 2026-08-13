"""End-to-end conversion: source image -> 400x300 2bpp B/W/R/Y framebuffer.

Stage order, and why it is this order:

1. **decode / fit**            -- 400x300, EXIF-corrected, contain or cover.
2. **sRGB -> Lab**             -- everything downstream is perceptual.
3. **tone curve**              -- autocontrast, exposure, S-curve, shadow lift,
                                  highlight roll-off; monotone LUT so no
                                  parameter combination can posterise.
4. **local contrast**          -- the panel's usable L* span is roughly a third
                                  of sRGB's, so global contrast has to be given
                                  up; local contrast is how the image keeps its
                                  apparent detail anyway.
5. **palette grade (opt-in)**  -- image-adaptive warm translation for hues a
                                  B/W/R/Y panel cannot faithfully reproduce.
6. **fit to panel L\\* range**  -- map 0..100 onto the profile's black..white.
7. **gamut compression**       -- project into the tetrahedron the four inks can
                                  actually reach. Doing this *before* dithering
                                  is what stops a saturated blue sky from
                                  dumping its unrepresentable residual into the
                                  error buffer and coming back as red confetti.
8. **chroma gate**             -- near-neutral content is pushed fully onto the
                                  black/white axis so it can never buy colour ink.
9. **halftone**                -- error diffusion or ordered blue noise, with
                                  edge-aware attenuation.
10. **pack**                   -- 2bpp, 30,000 bytes, unchanged device encoding.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict
from pathlib import Path

import numpy as np

from . import color as C
from . import edges as edge_mod
from . import legacy as legacy_mod
from . import metrics as metrics_mod
from . import pack as pack_mod
from .dither import DitherParams, dither
from .grade import adaptive_grade_amount, apply_palette_grade, selective_grade_amount
from .gamut import GamutHull, map_into_gamut
from .palette import PaletteProfile
from .tone import ChromaGate, ToneParams, apply_tone, fit_to_device_range


@dataclass
class EdgeParams:
    low_pct: float = 75.0
    high_pct: float = 97.0
    presmooth: float = 0.6
    dilate: int = 0

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class Recipe:
    """A complete, serialisable description of one conversion."""

    name: str
    description: str = ""
    profile: str = "note4c-measured-v1"
    fit: str = "contain"
    color_style: str = "natural"
    color_style_strength: float = 0.88
    color_style_adaptive: bool = True

    tone: ToneParams = field(default_factory=ToneParams)
    gate: ChromaGate = field(default_factory=ChromaGate)
    dither: DitherParams = field(default_factory=DitherParams)
    edge: EdgeParams = field(default_factory=EdgeParams)

    gamut_knee: float = 0.80
    gamut_l_adapt: float = 0.35
    gamut_intent: str = "hue-preserving"
    gamut_vivid_strength: float = 0.80
    gamut_vivid_hue_weight: float = 0.10
    l_headroom: float = 0.0

    #: Bypass everything and run the algorithm that ships today. A/B baseline.
    legacy: bool = False

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "profile": self.profile,
            "fit": self.fit,
            "color_style": self.color_style,
            "color_style_strength": self.color_style_strength,
            "color_style_adaptive": self.color_style_adaptive,
            "legacy": self.legacy,
            "gamut_knee": self.gamut_knee,
            "gamut_l_adapt": self.gamut_l_adapt,
            "gamut_intent": self.gamut_intent,
            "gamut_vivid_strength": self.gamut_vivid_strength,
            "gamut_vivid_hue_weight": self.gamut_vivid_hue_weight,
            "l_headroom": self.l_headroom,
            "tone": self.tone.to_dict(),
            "gate": self.gate.to_dict(),
            "dither": self.dither.to_dict(),
            "edge": self.edge.to_dict(),
        }

    @staticmethod
    def from_dict(data: dict) -> "Recipe":
        return Recipe(
            name=data["name"],
            description=data.get("description", ""),
            profile=data.get("profile", "note4c-measured-v1"),
            fit=data.get("fit", "contain"),
            color_style=data.get("color_style", "natural"),
            color_style_strength=data.get("color_style_strength", 0.88),
            color_style_adaptive=data.get("color_style_adaptive", True),
            legacy=data.get("legacy", False),
            gamut_knee=data.get("gamut_knee", 0.80),
            gamut_l_adapt=data.get("gamut_l_adapt", 0.35),
            gamut_intent=data.get("gamut_intent", "hue-preserving"),
            gamut_vivid_strength=data.get("gamut_vivid_strength", 0.80),
            gamut_vivid_hue_weight=data.get("gamut_vivid_hue_weight", 0.10),
            l_headroom=data.get("l_headroom", 0.0),
            tone=ToneParams(**data.get("tone", {})),
            gate=ChromaGate(**data.get("gate", {})),
            dither=DitherParams(**data.get("dither", {})),
            edge=EdgeParams(**data.get("edge", {})),
        )


@dataclass
class Result:
    codes: np.ndarray
    payload: bytes
    profile: PaletteProfile
    recipe: Recipe
    source_lab: np.ndarray
    target_lab: np.ndarray
    reference_lab: np.ndarray
    meta: dict
    #: What the panel actually looks like. Previews and every metric are
    #: computed against this, never against the profile the recipe used to make
    #: its decisions -- otherwise the legacy recipe gets graded on a palette of
    #: pure #FF0000 that no e-paper has ever produced, and the comparison is
    #: meaningless.
    render_profile: PaletteProfile | None = None

    @property
    def display_profile(self) -> PaletteProfile:
        return self.render_profile or self.profile

    def preview(self, scale: int = 1):
        return pack_mod.render_preview(self.codes, self.display_profile, scale)

    def simulated(self, sigma: float = 0.75, scale: int = 1):
        return pack_mod.render_simulated(self.codes, self.display_profile, sigma, scale)

    def metrics(self) -> dict:
        p = self.display_profile
        m = metrics_mod.evaluate(
            self.codes, self.target_lab, self.source_lab, p,
            reference_lab=self.reference_lab,
        )
        # Second fidelity figure against a recipe-independent reference, so the
        # numbers stay comparable across candidates with different tone curves.
        m["hvs_delta_e_vs_reference"] = metrics_mod.hvs_delta_e(self.reference_lab, self.codes, p)
        return m


def _reference_lab(source_lab: np.ndarray, profile: PaletteProfile, hull: GamutHull) -> np.ndarray:
    """Neutral yardstick: source, range-fitted and gamut-clipped, nothing else."""
    ref = fit_to_device_range(source_lab, profile.l_black, profile.l_white)
    return map_into_gamut(ref, hull, intent="hue-preserving", knee=1.0, l_adapt=0.0)


def convert(
    source,
    recipe: Recipe,
    profile: PaletteProfile | None = None,
    render_profile: PaletteProfile | None = None,
) -> Result:
    """``source`` is a path, or a ``(H, W, 3)`` uint8 sRGB array already fitted.

    ``profile`` drives the conversion. ``render_profile`` is the panel's real
    appearance, used only for previews and metrics; pass it when comparing
    recipes that were built against different palettes.
    """
    profile = profile or PaletteProfile.load(recipe.profile)
    render = render_profile or profile

    if isinstance(source, np.ndarray):
        rgb = np.asarray(source, dtype=np.uint8)
    else:
        rgb = pack_mod.load_and_fit(source, fit=recipe.fit)

    source_lab = C.srgb_to_lab(rgb / 255.0)
    mix_n = profile.yule_nielsen_n if recipe.dither.dot_gain_compensation else 1.0
    hull = GamutHull(profile, mixing_n=mix_n)
    # The yardstick lives in the *panel's* gamut, not the recipe's model of it.
    reference_lab = _reference_lab(
        source_lab, render, GamutHull(render, mixing_n=render.yule_nielsen_n)
    )

    meta: dict = {"profile": profile.to_dict(), "recipe": recipe.to_dict()}
    if render is not profile:
        meta["render_profile"] = render.name
    meta["content"] = edge_mod.classify(source_lab[..., 0], C.chroma(source_lab))
    meta["suggested_preset"] = edge_mod.suggest_preset(meta["content"])

    if recipe.legacy:
        codes = legacy_mod.legacy_bwry_codes(rgb)
        meta["stages"] = ["legacy: weighted-RGB nearest + raster Floyd-Steinberg on the ideal palette"]
        return Result(
            codes, pack_mod.pack_2bpp(codes), profile, recipe,
            source_lab, reference_lab, reference_lab, meta, render_profile=render,
        )

    toned, tone_meta = apply_tone(source_lab, recipe.tone)
    meta["tone"] = tone_meta

    grade_strength: float | np.ndarray = recipe.color_style_strength
    grade_adaptive_meta = None
    if recipe.color_style != "natural" and recipe.color_style_adaptive:
        diagnostic_source = fit_to_device_range(
            toned, profile.l_black, profile.l_white, recipe.l_headroom
        )
        faithful = map_into_gamut(
            diagnostic_source, hull, intent="hue-preserving", knee=1.0, l_adapt=0.0
        )
        controller = (
            selective_grade_amount
            if recipe.color_style == "selective-vintage"
            else adaptive_grade_amount
        )
        grade_strength, grade_adaptive_meta = controller(
            diagnostic_source, faithful, max_strength=recipe.color_style_strength
        )
    graded, grade_meta = apply_palette_grade(
        toned,
        profile,
        style=recipe.color_style,
        strength=grade_strength,
        adaptive_meta=grade_adaptive_meta,
    )
    meta["color_grade"] = grade_meta

    ranged = fit_to_device_range(graded, profile.l_black, profile.l_white, recipe.l_headroom)

    mapped = map_into_gamut(
        ranged,
        hull,
        intent=recipe.gamut_intent,
        knee=recipe.gamut_knee,
        l_adapt=recipe.gamut_l_adapt,
        vivid_strength=recipe.gamut_vivid_strength,
        vivid_hue_weight=recipe.gamut_vivid_hue_weight,
    )
    meta["gamut"] = {
        "intent": recipe.gamut_intent,
        "mixing_n": round(mix_n, 3),
        "in_gamut_before": round(float(np.mean(hull.contains(C.lab_to_xyz(ranged)))), 4),
        "mean_chroma_before": round(float(np.mean(C.chroma(ranged))), 2),
        "mean_chroma_after": round(float(np.mean(C.chroma(mapped))), 2),
    }

    gated, gate_open = recipe.gate.apply(mapped)
    meta["gate"] = {"mean_openness": round(float(np.mean(gate_open)), 4)}

    if recipe.dither.edge_suppress > 0:
        edge = edge_mod.edge_strength(
            gated[..., 0],
            low_pct=recipe.edge.low_pct,
            high_pct=recipe.edge.high_pct,
            presmooth=recipe.edge.presmooth,
            dilate=recipe.edge.dilate,
        )
        meta["edge"] = {"mean_strength": round(float(np.mean(edge)), 4)}
    else:
        edge = None

    codes = dither(gated, profile, recipe.dither, gate_open=gate_open, edge=edge)
    payload = pack_mod.pack_2bpp(codes)

    return Result(
        codes, payload, profile, recipe, source_lab, gated, reference_lab, meta,
        render_profile=render,
    )


def write_outputs(
    result: Result,
    out_dir: str | Path,
    stem: str,
    *,
    preview_scale: int = 1,
    simulate: bool = True,
    metrics: bool = True,
) -> dict:
    """Write ``<stem>.bin`` plus previews and a JSON sidecar. Returns the sidecar."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    bin_path = out_dir / f"{stem}.bin"
    bin_path.write_bytes(result.payload)

    result.preview(preview_scale).save(out_dir / f"{stem}_preview.png")
    if simulate:
        result.simulated(scale=preview_scale).save(out_dir / f"{stem}_sim.png")

    sidecar = dict(result.meta)
    sidecar["output"] = {
        "bytes": len(result.payload),
        "expected_bytes": pack_mod.SIZE_2BPP,
        "width": pack_mod.SCREEN_WIDTH,
        "height": pack_mod.SCREEN_HEIGHT,
        "format": "bwry2bpp",
        "encoding": "black=0 white=1 yellow=2 red=3, 4px/byte, MSB first",
    }
    if metrics:
        sidecar["metrics"] = result.metrics()

    (out_dir / f"{stem}.json").write_text(json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n")
    return sidecar
