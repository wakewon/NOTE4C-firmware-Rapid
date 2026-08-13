#!/usr/bin/env python3
"""Invariant checks for the B/W/R/Y pipeline.

    python3 tools/bwry/selftest.py

The important one is ``legacy vs docs/inkscreen_image_converter.js``: the whole
A/B ladder is meaningless if the baseline is not bit-for-bit what the device
does today. That check runs only when node is available.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np

from bwry import color as C
from bwry import pack
from bwry.bluenoise import void_and_cluster
from bwry.calibrate import make_chart, homography, mark_centers, sample_patches
from bwry.gamut import (
    GamutHull,
    compress_into_gamut,
    map_selective_vivid_into_gamut,
    map_vivid_into_gamut,
)
from bwry.grade import adaptive_grade_amount, apply_palette_grade, selective_grade_amount
from bwry.legacy import legacy_bwry_codes
from bwry.palette import PaletteProfile
from bwry.dither import DitherParams, dither
from bwry.pipeline import convert
from bwry.presets import ab_matrix, get_preset
from bwry.tone import ToneParams, build_tone_lut

REPO = Path(__file__).resolve().parents[2]

_failures: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        _failures.append(name)


# --------------------------------------------------------------------------


def test_colour_roundtrip() -> None:
    print("colour space")
    rng = np.random.default_rng(7)
    srgb = rng.random((512, 3))
    back = C.lab_to_srgb(C.srgb_to_lab(srgb))
    check("sRGB -> Lab -> sRGB", float(np.abs(back - srgb).max()) < 1e-5,
          f"max err {np.abs(back - srgb).max():.2e}")

    # Known values: D65 white and mid grey. The tolerance is loose because the
    # sRGB matrix and the D65 white point disagree in the 8th decimal.
    lab_white = C.srgb_to_lab(np.array([1.0, 1.0, 1.0]))
    check("white is L*=100, a=b=0", bool(np.allclose(lab_white, [100, 0, 0], atol=1e-4)),
          f"{np.round(lab_white, 6).tolist()}")
    lab_mid = C.srgb_to_lab(np.array([0.5, 0.5, 0.5]))
    check("sRGB 0.5 is L*~53.39", abs(float(lab_mid[0]) - 53.389) < 0.01, f"L*={lab_mid[0]:.3f}")


def test_packing() -> None:
    print("2bpp packing")
    rng = np.random.default_rng(11)
    codes = rng.integers(0, 4, size=(pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH)).astype(np.uint8)
    data = pack.pack_2bpp(codes)
    check("payload is 30000 bytes", len(data) == pack.SIZE_2BPP, f"{len(data)}")
    check("unpack round-trips", bool(np.array_equal(pack.unpack_2bpp(data), codes)))

    # Bit layout, spelled out against the firmware's shift arithmetic.
    probe = np.zeros((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH), dtype=np.uint8)
    probe[0, 0] = 3  # bits 7..6 of byte 0
    probe[0, 3] = 1  # bits 1..0 of byte 0
    b0 = pack.pack_2bpp(probe)[0]
    check("pixel 0 -> bits 7..6, pixel 3 -> bits 1..0", b0 == 0b11000001, f"byte0=0b{b0:08b}")


def test_palette_and_gamut() -> None:
    print("palette and gamut hull")
    for name in PaletteProfile.available():
        p = PaletteProfile.load(name)
        check(f"{name}: device codes are 0..3 and unique",
              sorted(int(c) for c in p.device_codes) == [0, 1, 2, 3])

    p = PaletteProfile.load("note4c-estimate-v1")
    hull = GamutHull(p)
    check("every ink is inside its own hull", bool(hull.contains(p.xyz).all()))

    mid = hull.neutral_lab_at(np.array(70.0))
    check("neutral axis point is in gamut", bool(hull.contains(C.lab_to_xyz(mid))))
    check("neutral axis point is neutral", float(C.chroma(mid)) < 2.0, f"C*={float(C.chroma(mid)):.2f}")

    # Saturated sRGB must land inside after compression, whatever we throw at it.
    rng = np.random.default_rng(3)
    wild = C.srgb_to_lab(rng.random((64, 64, 3)))
    wild[..., 0] = np.clip(wild[..., 0], p.l_black, p.l_white)
    mapped = compress_into_gamut(wild, hull, knee=0.8, l_adapt=0.35)
    inside = hull.contains(C.lab_to_xyz(mapped))
    check("gamut compression lands everything inside", bool(inside.all()),
          f"{int((~inside).sum())} stragglers of {inside.size}")

    # Hue must survive compression for colours the panel can actually reach.
    warm = np.array([[[60.0, 40.0, 30.0]]])
    out = compress_into_gamut(warm, hull, knee=0.8, l_adapt=0.35)
    dh = float(C.hue_distance(C.hue_deg(warm), C.hue_deg(out)).item())
    check("compression preserves hue", dh < 3.0, f"{dh:.2f} deg")
    check("compression actually reduced chroma",
          float(C.chroma(out).item()) < float(C.chroma(warm).item()),
          f"C* {float(C.chroma(warm).item()):.1f} -> {float(C.chroma(out).item()):.1f}")

    measured = PaletteProfile.load("note4c-measured-v1")
    physical_hull = GamutHull(measured, mixing_n=measured.yule_nielsen_n)
    samples_xyz, _ = physical_hull.sample(12)
    check("Yule-Nielsen gamut samples remain inside the physical hull",
          bool(physical_hull.contains(samples_xyz).all()))

    # A B/W/R/Y gamut has no blue cusp. Hue-preserving compression correctly
    # turns blue nearly neutral; the opt-in vivid intent must be able to retain
    # colourfulness by rotating it toward a printable warm hue, while leaving
    # genuinely neutral content alone.
    blue = C.srgb_to_lab(np.array([[[0.10, 0.20, 0.90]]]))
    blue[..., 0] = 60.0
    blue_base = compress_into_gamut(blue, physical_hull)
    blue_vivid = map_vivid_into_gamut(blue, physical_hull, strength=0.7)
    check("vivid intent keeps unsupported blue visibly chromatic",
          float(C.chroma(blue_vivid).item()) > float(C.chroma(blue_base).item()) + 20.0,
          f"C* {float(C.chroma(blue_base).item()):.1f} -> {float(C.chroma(blue_vivid).item()):.1f}")
    check("vivid result remains physically reachable",
          bool(physical_hull.contains(C.lab_to_xyz(blue_vivid)).all()))

    grey = np.array([[[60.0, 0.0, 0.0]]])
    grey_base = compress_into_gamut(grey, physical_hull)
    grey_vivid = map_vivid_into_gamut(grey, physical_hull, strength=0.7)
    check("vivid intent does not tint neutral content",
          bool(np.allclose(grey_vivid, grey_base, atol=1e-10)))

    blue_selective = map_selective_vivid_into_gamut(
        blue, physical_hull, strength=0.72
    )
    check("selective vivid recovers colour that strict mapping loses",
          float(C.chroma(blue_selective).item()) > float(C.chroma(blue_base).item()) + 12.0,
          f"C* {float(C.chroma(blue_base).item()):.1f} -> "
          f"{float(C.chroma(blue_selective).item()):.1f}")

    native_red = measured.lab[measured.index_of("red")][None, None, :]
    native_base = compress_into_gamut(native_red, physical_hull)
    native_selective = map_selective_vivid_into_gamut(
        native_red, physical_hull, strength=0.72
    )
    check("selective vivid leaves already printable colour alone",
          float(C.delta_e76(native_base, native_selective).item()) < 0.01,
          f"dE76={float(C.delta_e76(native_base, native_selective).item()):.4f}")


def test_halftone_model() -> None:
    print("halftone optical model")
    profile = PaletteProfile.load("note4c-measured-v1")
    n = profile.yule_nielsen_n
    check("measured profile exposes Yule-Nielsen n", abs(n - 1.57) < 1e-9, f"n={n:.2f}")

    # The transform must leave every solid ink unchanged. Only spatial mixtures
    # are supposed to move; otherwise previews would alter the calibrated
    # palette itself.
    roundtrip = C.yule_nielsen_decode_xyz(C.yule_nielsen_encode_xyz(profile.xyz, n), n)
    check("Yule-Nielsen transform preserves solid inks",
          bool(np.allclose(roundtrip, profile.xyz, atol=1e-12)))

    black = profile.xyz[profile.index_of("black")]
    white = profile.xyz[profile.index_of("white")]
    half_work = 0.5 * C.yule_nielsen_encode_xyz(black, n) \
        + 0.5 * C.yule_nielsen_encode_xyz(white, n)
    physical_half = C.yule_nielsen_decode_xyz(half_work, n)
    linear_half = 0.5 * black + 0.5 * white
    check("measured dot gain makes a 50% halftone darker than linear mixing",
          float(physical_half[1]) < float(linear_half[1]) * 0.85,
          f"Y {linear_half[1]:.3f} -> {physical_half[1]:.3f}")

    # Ask for exactly that physical 50/50 tone. Ordinary linear diffusion lays
    # down too much black; compensation should recover 50% coverage and the
    # requested optical colour once the same panel model integrates the result.
    target_one = C.xyz_to_lab(physical_half)
    target = np.broadcast_to(target_one, (120, 160, 3)).copy()
    closed_gate = np.zeros(target.shape[:2])

    def run(compensate: bool, algorithm: str = "sierra2") -> tuple[float, float]:
        params = DitherParams(
            algorithm=algorithm, serpentine=True, chroma_penalty=100.0,
            dot_gain_compensation=compensate,
        )
        codes = dither(target, profile, params, gate_open=closed_gate)
        mixed = C.yule_nielsen_encode_xyz(profile.xyz_of_codes(codes), n).mean(axis=(0, 1))
        got = C.xyz_to_lab(C.yule_nielsen_decode_xyz(mixed, n))
        de = float(C.delta_e76(got, target_one))
        black_fraction = float(np.mean(codes == profile.inks[profile.index_of("black")].device_code))
        return de, black_fraction

    uncorrected_de, uncorrected_black = run(False)
    corrected_de, corrected_black = run(True)
    check("uncorrected diffusion reproduces the measured dark-midtones failure",
          uncorrected_de > 5.0, f"dE76={uncorrected_de:.2f}, black={uncorrected_black:.3f}")
    check("dot-gain compensation recovers the requested physical midtone",
          corrected_de < 0.3 and abs(corrected_black - 0.5) < 0.01,
          f"dE76={corrected_de:.2f}, black={corrected_black:.3f}")
    blue_de, blue_black = run(True, "bluenoise")
    check("ordered blue noise uses the same dot-gain model",
          blue_de < 0.3 and abs(blue_black - 0.5) < 0.01,
          f"dE76={blue_de:.2f}, black={blue_black:.3f}")
    tetra_de, tetra_black = run(True, "tetra-bluenoise")
    check("tetrahedral blue noise preserves optical area coverage",
          tetra_de < 0.3 and abs(tetra_black - 0.5) < 0.01,
          f"dE76={tetra_de:.2f}, black={tetra_black:.3f}")

    # A half-open chroma gate describes a target that has already been faded
    # toward neutral by ChromaGate.apply().  The tetra sampler must not cube
    # that openness and erase the remaining colour a second time.
    mixed_weights = np.full(4, 0.25)
    mixed_work = mixed_weights @ C.yule_nielsen_encode_xyz(profile.xyz, n)
    mixed_lab = C.xyz_to_lab(C.yule_nielsen_decode_xyz(mixed_work, n))
    mixed_target = np.broadcast_to(mixed_lab, (120, 160, 3)).copy()
    half_gate = np.full(mixed_target.shape[:2], 0.5)
    mixed_codes = dither(
        mixed_target,
        profile,
        DitherParams(
            algorithm="tetra-bluenoise", dot_gain_compensation=True,
            chroma_penalty=0.0,
        ),
        gate_open=half_gate,
    )
    chromatic_codes = [
        ink.device_code for ink in profile.inks if C.chroma(ink.lab) >= 12.0
    ]
    chromatic_fraction = float(np.mean(np.isin(mixed_codes, chromatic_codes)))
    check("tetra gate does not attenuate confidently chromatic targets twice",
          abs(chromatic_fraction - 0.5) < 0.02,
          f"chromatic coverage={chromatic_fraction:.3f}")


def test_tone_lut() -> None:
    print("tone curve")
    for params in (
        ToneParams(),
        ToneParams(contrast=0.6, shadow_lift=0.5, highlight_compress=0.6, exposure=0.8),
        ToneParams(contrast=-0.5, shadow_lift=0.9, highlight_compress=0.9, exposure=-1.2),
    ):
        lut = build_tone_lut(params, 8.0, 96.0)
        check(f"LUT monotone (contrast={params.contrast})", bool(np.all(np.diff(lut) >= -1e-12)))
        check(f"LUT stays in 0..100 (contrast={params.contrast})",
              bool(lut.min() >= -1e-9 and lut.max() <= 100 + 1e-9))

    lut = build_tone_lut(ToneParams(autocontrast=False), 0.0, 100.0)
    check("neutral params are the identity", float(np.abs(lut - np.linspace(0, 100, lut.size)).max()) < 1e-9)


def test_bluenoise() -> None:
    print("blue noise mask")
    mask = void_and_cluster(32, 1.9)
    check("mask is 32x32", mask.shape == (32, 32))
    check("thresholds are a uniform permutation",
          bool(np.allclose(np.sort(mask.ravel()), (np.arange(mask.size) + 0.5) / mask.size)))

    # Blue noise means little energy at low spatial frequencies.
    spectrum = np.abs(np.fft.fftshift(np.fft.fft2(mask - mask.mean())))
    c = mask.shape[0] // 2
    low = spectrum[c - 3 : c + 4, c - 3 : c + 4].sum()
    total = spectrum.sum()
    check("low-frequency energy is suppressed", low / total < 0.05, f"{low / total:.4f} of total")


def test_palette_grade() -> None:
    print("palette-aware colour grade")
    profile = PaletteProfile.load("note4c-measured-v1")
    swatches = C.srgb_to_lab(np.array([[
        [0.10, 0.20, 0.90],  # blue sky
        [0.10, 0.70, 0.20],  # green foliage
        [0.50, 0.50, 0.50],  # neutral stone
        [0.00, 0.00, 0.00],
        [1.00, 1.00, 1.00],
    ]]))
    graded, meta = apply_palette_grade(swatches, profile, style="vintage", strength=1.0)
    hues = C.hue_deg(graded)[0]
    chroma = C.chroma(graded)[0]
    red_h = float(C.hue_deg(profile.lab[profile.index_of("red")]))
    yellow_h = float(C.hue_deg(profile.lab[profile.index_of("yellow")]))

    check("vintage grade maps blue toward brick red",
          float(C.hue_distance(hues[0], red_h)) < float(C.hue_distance(hues[0], yellow_h)))
    check("vintage grade maps green toward ochre",
          float(C.hue_distance(hues[1], yellow_h)) < float(C.hue_distance(hues[1], red_h)))
    check("vintage grade gives neutral midtones a restrained warm cast",
          3.0 < float(chroma[2]) < 12.0, f"C*={chroma[2]:.1f}")
    check("vintage grade keeps solid black and paper white neutral",
          float(chroma[3]) < 1e-6 and float(chroma[4]) < 1e-3,
          f"C* black={chroma[3]:.3g} white={chroma[4]:.3g}")
    check("vintage grade reports its transform", meta["style"] == "vintage")

    # The adaptive controller must distinguish a palette-native photograph
    # from one dominated by hues the panel cannot reproduce.
    hull = GamutHull(profile, mixing_n=profile.yule_nielsen_n)
    native = np.broadcast_to(profile.lab[[2, 3]][None, :, :], (12, 2, 3)).copy()
    native_faithful = compress_into_gamut(native, hull, knee=1.0, l_adapt=0.0)
    native_amount, native_meta = adaptive_grade_amount(native, native_faithful)
    cool = np.broadcast_to(swatches[:, :2, :], (12, 2, 3)).copy()
    cool_faithful = compress_into_gamut(cool, hull, knee=1.0, l_adapt=0.0)
    cool_amount, cool_meta = adaptive_grade_amount(cool, cool_faithful)
    check("adaptive grade stays subtle for palette-native colours",
          float(np.mean(native_amount)) < 0.18,
          f"mean strength={float(np.mean(native_amount)):.3f}")
    check("adaptive grade grows when unsupported hues dominate",
          float(np.mean(cool_amount)) > 3.0 * float(np.mean(native_amount)),
          f"native={native_meta['mean_effective_strength']}, "
          f"cool={cool_meta['mean_effective_strength']}")

    selective_native, _ = selective_grade_amount(native, native_faithful)
    selective_cool, _ = selective_grade_amount(cool, cool_faithful)
    check("selective controller protects already printable colour",
          float(np.mean(selective_native)) < 0.08,
          f"mean strength={float(np.mean(selective_native)):.3f}")
    check("selective controller targets unsupported visible colour",
          float(np.mean(selective_cool)) > 5.0 * float(np.mean(selective_native)),
          f"native={float(np.mean(selective_native)):.3f}, "
          f"cool={float(np.mean(selective_cool)):.3f}")

    half, _ = apply_palette_grade(swatches[:, :2], profile,
                                  style="selective-vintage", strength=0.5)
    check("polar selective grade does not cross the grey axis",
          bool(np.all(C.chroma(half) > 20.0)),
          f"minimum C*={float(np.min(C.chroma(half))):.1f}")


def test_pipeline_outputs() -> None:
    print("pipeline")
    rng = np.random.default_rng(5)
    rgb = (rng.random((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH, 3)) * 255).astype(np.uint8)
    # A flat mid-grey block, to check ink density lands where it should.
    rgb[:100, :100] = 128

    # Pin the profile explicitly rather than inheriting whatever the current
    # default is: the integration check below only means anything if the
    # conversion and the rendering are talking about the same panel.
    profile_name = "note4c-estimate-v1"
    profile = PaletteProfile.load(profile_name)
    matrix = ab_matrix(profile_name)
    for recipe in matrix:
        result = convert(rgb, recipe, render_profile=profile)
        ok_size = len(result.payload) == pack.SIZE_2BPP
        ok_codes = bool(result.codes.min() >= 0 and result.codes.max() <= 3)
        ok_round = bool(np.array_equal(pack.unpack_2bpp(result.payload), result.codes))
        check(f"{recipe.name}: 30000 bytes, codes 0..3, round-trips",
              ok_size and ok_codes and ok_round)

    shipping = get_preset("photo", profile_name).to_dict()
    candidate = next(r for r in matrix if r.name == "09k-selective-vintage-hybrid").to_dict()
    for recipe_dict in (shipping, candidate):
        recipe_dict.pop("name")
        recipe_dict.pop("description")
    check("photo preset is exactly the selected 09k algorithm",
          shipping == candidate)

    # Average tone: a flat patch must integrate to what was asked for.
    flat = np.full((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH, 3), 128, dtype=np.uint8)
    recipe = get_preset("photo", profile_name)
    recipe.tone = ToneParams(autocontrast=False, saturation=1.0)
    recipe.dither.edge_suppress = 0.0
    result = convert(flat, recipe, render_profile=profile)
    got = C.xyz_to_lab(profile.xyz_of_codes(result.codes).reshape(-1, 3).mean(axis=0))
    want = result.target_lab.reshape(-1, 3).mean(axis=0)
    dl = abs(float(got[0] - want[0]))
    check("flat patch integrates to the target lightness", dl < 1.0, f"dL* = {dl:.3f}")


def test_calibration_chart() -> None:
    print("calibration chart")
    codes = make_chart()
    check("chart packs to 30000 bytes", len(pack.pack_2bpp(codes)) == pack.SIZE_2BPP)

    # Round-trip the chart through a synthetic "photograph": render it with a
    # known palette, then check calibration recovers that palette.
    truth = PaletteProfile.load("note4c-estimate-v1")
    from bwry.calibrate import profile_from_samples

    rendered = truth.render_codes(codes)
    corners = mark_centers()  # identity framing: the photo IS the chart
    samples = sample_patches(rendered, corners)
    recovered, report = profile_from_samples(samples, name="roundtrip")

    de = float(C.delta_e76(recovered.lab, truth.lab).max())
    check("calibration recovers the palette it was rendered with", de < 3.0, f"max dE76 {de:.2f}")
    check("halftone linearity check passes on synthetic data", report["linear_mixing_ok"],
          f"mean dE76 {report['mix_check_mean_delta_e']:.2f}")

    # Same thing under uneven light: this is the realistic case for a phone
    # photo, and the whole point of fitting the illumination field.
    from bwry.calibrate import estimate_illumination, photo_diagnostics

    h, w = rendered.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    # ~35% brighter top-left than bottom-right, plus a little vignetting.
    shade = 1.15 - 0.30 * (xx / w) - 0.18 * (yy / h)
    shade *= 1.0 - 0.10 * (((xx / w - 0.5) ** 2 + (yy / h - 0.5) ** 2) * 2.0)
    lit = C.srgb_to_u8(C.linear_to_srgb(C.srgb_to_linear(rendered / 255.0) * shade[..., None]))

    field = estimate_illumination(lit, corners)
    nu = field.non_uniformity
    check("illumination field detects the gradient", 0.25 < nu < 0.60, f"{nu * 100:.0f}% variation")

    corrected, _ = profile_from_samples(sample_patches(lit, corners, illumination=field), name="lit")
    naive, _ = profile_from_samples(sample_patches(lit, corners), name="lit-naive")
    de_corr = float(C.delta_e76(corrected.lab, truth.lab).max())
    de_naive = float(C.delta_e76(naive.lab, truth.lab).max())
    check("flat-field correction recovers the palette under uneven light", de_corr < 3.0,
          f"max dE76 {de_corr:.2f} (uncorrected {de_naive:.2f})")
    check("flat-field correction is a clear improvement", de_corr < de_naive * 0.6,
          f"{de_naive:.2f} -> {de_corr:.2f}")

    # Diagnostics have to actually catch a bad photograph. The "good" fixture
    # is exposed the way a real shot should be -- paper white a little under
    # clipping, not pinned at 255.
    exposed = C.srgb_to_u8(C.linear_to_srgb(C.srgb_to_linear(rendered / 255.0) * 0.78))
    ok = photo_diagnostics(sample_patches(exposed, corners), None)
    check("a well-exposed render passes the photo checks", ok["usable"], str(ok["problems"]))
    check("...and still recovers the palette",
          float(C.delta_e76(profile_from_samples(sample_patches(exposed, corners), name="e")[0].lab,
                            truth.lab).max()) < 3.0)

    blown = np.clip(rendered.astype(np.int32) + 90, 0, 255).astype(np.uint8)
    bad = photo_diagnostics(sample_patches(blown, corners), None)
    check("an over-exposed photograph is rejected", not bad["usable"],
          bad["problems"][0][:60] if bad["problems"] else "not caught")

    glared = C.srgb_to_u8(C.linear_to_srgb(C.srgb_to_linear(rendered / 255.0) * 0.5 + 0.25))
    glare = photo_diagnostics(sample_patches(glared, corners), None)
    check("a glare-washed photograph is rejected", not glare["usable"],
          glare["problems"][0][:60] if glare["problems"] else "not caught")

    # The white zones are what the illumination field is fitted from, and the
    # homography is only valid inside the quad through the four mark centres.
    # A zone reaching outside it samples the bezel and poisons the fit.
    from bwry.calibrate import WHITE_ZONES

    mc = mark_centers()
    inside = all(
        z[0] >= mc[:, 0].min() and z[2] <= mc[:, 0].max()
        and z[1] >= mc[:, 1].min() and z[3] <= mc[:, 1].max()
        for z in WHITE_ZONES
    )
    check("white zones stay inside the registration-mark quad", inside)

    # A saturated ink may out-reflect the white state in one channel while
    # being less luminous overall. Media-relative normalisation must not clip
    # that away, or the profile describes an ink the panel cannot make.
    from bwry.calibrate import PatchSample

    def sample(label, lin):
        return PatchSample(label, np.asarray(lin, float),
                           C.srgb_to_u8(C.linear_to_srgb(np.clip(lin, 0, 1))), 0.0, 0.0)

    hot = {
        "black": sample("black", [0.03, 0.03, 0.035]),
        "white": sample("white", [0.60, 0.62, 0.63]),
        # 1.55x white in red, but only 0.91x its luminance -- physical.
        "yellow": sample("yellow", [0.93, 0.505, 0.008]),
        "red": sample("red", [0.35, 0.045, 0.045]),
    }
    hot_profile, _ = profile_from_samples(dict(hot), name="hot")
    y_lin = hot_profile.inks[hot_profile.index_of("yellow")].linear
    check("an ink brighter than white in one channel is kept unclipped",
          y_lin is not None and float(y_lin[0]) > 1.2, f"yellow R = {float(y_lin[0]):.2f}")
    reloaded = PaletteProfile.from_dict(hot_profile.to_dict())
    de_rt = float(C.delta_e76(reloaded.lab, hot_profile.lab).max())
    check("...and survives a save/load round trip", de_rt < 0.5, f"max dE76 {de_rt:.3f}")
    check("...and the luminance invariant does not reject it",
          photo_diagnostics(dict(hot), None)["usable"],
          str(photo_diagnostics(dict(hot), None)["problems"]))


def test_mark_detection() -> None:
    print("registration mark detection")
    from bwry.calibrate import find_marks

    truth = PaletteProfile.load("note4c-estimate-v1")
    photo = truth.render_codes(make_chart())
    photo = np.repeat(np.repeat(photo, 3, axis=0), 3, axis=1)  # a "photo" 3x the chart
    expected = mark_centers() * 3.0 + 1.0

    found = find_marks(photo)
    err = float(np.abs(found - expected).max())
    check("finds the four marks on a clean render", err < 4.0, f"max error {err:.1f}px")

    # A thin dark line touching a mark -- a panel bezel seam, a cable, a table
    # edge. Before this was handled, the mark and the seam merged into one
    # component whose centre of mass sat out on the seam, moving that corner
    # far enough to sample a patch as "white border".
    seamed = photo.copy()
    y = int(expected[0][1])
    seamed[y - 2 : y + 2, :] = 0
    found_seam = find_marks(seamed)
    err_seam = float(np.abs(found_seam - expected).max())
    check("a dark seam through a mark does not drag the corner off it",
          err_seam < 8.0, f"max error {err_seam:.1f}px")

    h = homography(mark_centers(), mark_centers() * 2.0 + 17.0)
    check("homography solves an affine case",
          bool(np.allclose(h @ np.array([10.0, 20.0, 1.0]), [10 * 2 + 17, 20 * 2 + 17, 1.0])))


def test_legacy_matches_shipping_js() -> None:
    print("legacy baseline vs docs/inkscreen_image_converter.js")
    js = REPO / "docs" / "inkscreen_image_converter.js"
    if not js.exists():
        check("reference converter present", False, str(js))
        return
    if not _have_node():
        print("  SKIP  node not available")
        return

    rng = np.random.default_rng(1234)
    rgb = (rng.random((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH, 3)) * 255).astype(np.uint8)
    rgba = np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255, dtype=np.uint8)], axis=2)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "in.raw").write_bytes(rgba.tobytes())
        script = tmp / "run.mjs"
        script.write_text(
            "import { createRequire } from 'node:module';\n"
            "import fs from 'node:fs';\n"
            f"const require = createRequire({str(js)!r});\n"
            f"const mod = require({str(js)!r});\n"
            f"const rgba = new Uint8Array(fs.readFileSync({str(tmp / 'in.raw')!r}));\n"
            "const out = mod.rgbaToBwry2bpp(rgba);\n"
            f"fs.writeFileSync({str(tmp / 'out.bin')!r}, Buffer.from(out));\n"
        )
        proc = subprocess.run(["node", str(script)], capture_output=True, text=True)
        if proc.returncode != 0:
            check("reference converter runs", False, proc.stderr.strip()[:200])
            return
        reference = (tmp / "out.bin").read_bytes()

    ours = pack.pack_2bpp(legacy_bwry_codes(rgb))
    check("byte-for-byte identical to the shipping converter", ours == reference,
          f"{sum(a != b for a, b in zip(ours, reference))} of {len(reference)} bytes differ")


def _have_node() -> bool:
    try:
        return subprocess.run(["node", "--version"], capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def test_recipe_serialisation() -> None:
    print("recipe serialisation")
    from bwry.pipeline import Recipe

    for recipe in ab_matrix():
        again = Recipe.from_dict(json.loads(json.dumps(recipe.to_dict())))
        check(f"{recipe.name}: survives a JSON round trip", again.to_dict() == recipe.to_dict())

    by_name = {r.name: r for r in ab_matrix()}
    base = by_name["09-sierra2-edge"].to_dict()
    corrected = by_name["09b-sierra2-edge-yn"].to_dict()
    for data in (base, corrected):
        data.pop("name")
        data.pop("description")
    corrected["dither"]["dot_gain_compensation"] = False
    check("09/09b A/B pair differs only by dot-gain compensation", corrected == base)


def main() -> int:
    for fn in (
        test_colour_roundtrip,
        test_packing,
        test_palette_and_gamut,
        test_halftone_model,
        test_tone_lut,
        test_bluenoise,
        test_palette_grade,
        test_calibration_chart,
        test_mark_detection,
        test_recipe_serialisation,
        test_pipeline_outputs,
        test_legacy_matches_shipping_js,
    ):
        fn()
        print()

    if _failures:
        print(f"{len(_failures)} check(s) failed:")
        for name in _failures:
            print(f"  - {name}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
