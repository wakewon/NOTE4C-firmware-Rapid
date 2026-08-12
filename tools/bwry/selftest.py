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
from bwry.gamut import GamutHull, compress_into_gamut
from bwry.legacy import legacy_bwry_codes
from bwry.palette import PaletteProfile
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


def test_pipeline_outputs() -> None:
    print("pipeline")
    rng = np.random.default_rng(5)
    rgb = (rng.random((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH, 3)) * 255).astype(np.uint8)
    # A flat mid-grey block, to check ink density lands where it should.
    rgb[:100, :100] = 128

    profile = PaletteProfile.load("note4c-estimate-v1")
    for recipe in ab_matrix():
        result = convert(rgb, recipe, render_profile=profile)
        ok_size = len(result.payload) == pack.SIZE_2BPP
        ok_codes = bool(result.codes.min() >= 0 and result.codes.max() <= 3)
        ok_round = bool(np.array_equal(pack.unpack_2bpp(result.payload), result.codes))
        check(f"{recipe.name}: 30000 bytes, codes 0..3, round-trips",
              ok_size and ok_codes and ok_round)

    # Average tone: a flat patch must integrate to what was asked for.
    flat = np.full((pack.SCREEN_HEIGHT, pack.SCREEN_WIDTH, 3), 128, dtype=np.uint8)
    recipe = get_preset("photo")
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


def main() -> int:
    for fn in (
        test_colour_roundtrip,
        test_packing,
        test_palette_and_gamut,
        test_tone_lut,
        test_bluenoise,
        test_calibration_chart,
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
