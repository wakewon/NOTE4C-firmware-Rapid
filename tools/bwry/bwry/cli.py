"""Command line front end. See ``tools/bwry/README.md`` for the workflow."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

from . import abtest, calibrate, pack
from .dither import ALGORITHMS
from .palette import PaletteProfile
from .pipeline import Recipe, convert
from .presets import PRESETS, ab_matrix, get_preset


# --------------------------------------------------------------------------
# convert
# --------------------------------------------------------------------------


def cmd_convert(args: argparse.Namespace) -> int:
    if args.recipe:
        recipe = Recipe.from_dict(json.loads(Path(args.recipe).read_text()))
    else:
        recipe = get_preset(args.preset, args.profile)

    if args.profile and not args.recipe and args.preset != "legacy":
        recipe.profile = args.profile
    if args.algorithm:
        recipe.dither.algorithm = args.algorithm
    if args.no_serpentine:
        recipe.dither.serpentine = False
    if args.edge_suppress is not None:
        recipe.dither.edge_suppress = args.edge_suppress
    if args.saturation is not None:
        recipe.tone.saturation = args.saturation
    if args.contrast is not None:
        recipe.tone.contrast = args.contrast
    if args.fit:
        recipe.fit = args.fit

    render = PaletteProfile.load(args.render_profile) if args.render_profile else None
    result = convert(args.input, recipe, render_profile=render)

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result.payload)

    if args.preview:
        result.preview(args.preview_scale).save(args.preview)
    if args.simulated:
        result.simulated(scale=args.preview_scale).save(args.simulated)
    if args.sidecar:
        sidecar = dict(result.meta)
        sidecar["metrics"] = result.metrics()
        Path(args.sidecar).write_text(json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n")

    m = result.metrics()
    print(f"wrote {out} ({len(result.payload)} bytes, expected {pack.SIZE_2BPP})")
    print(f"  recipe        : {recipe.name}  [{recipe.profile}]")
    print(f"  algorithm     : {'legacy FS' if recipe.legacy else recipe.dither.algorithm}"
          f"{' serpentine' if not recipe.legacy and recipe.dither.serpentine else ''}")
    print(f"  content looks : {result.meta['suggested_preset']} "
          f"(flat {result.meta['content']['flat_ratio']:.2f}, edges {result.meta['content']['strong_edge_ratio']:.3f})")
    print(f"  HVS dE76      : mean {m['hvs_delta_e']['mean']:.2f}  p95 {m['hvs_delta_e']['p95']:.2f}")
    print(f"  colour on grey: {m['spurious_chroma']['rate_in_neutral'] * 100:.2f}% of neutral area")
    print("  ink usage     : " + "  ".join(f"{k}={v * 100:.1f}%" for k, v in m["ink_usage"].items()))

    if args.push:
        body = abtest.push_to_device(args.push, result.payload, title=recipe.name)
        print(f"  pushed to {args.push}: {body}")
    return 0


# --------------------------------------------------------------------------
# ab
# --------------------------------------------------------------------------


def cmd_ab(args: argparse.Namespace) -> int:
    if args.recipes:
        data = json.loads(Path(args.recipes).read_text())
        recipes = [Recipe.from_dict(d) for d in data]
    else:
        recipes = ab_matrix(args.profile)

    if args.only:
        wanted = {n.strip() for n in args.only.split(",")}
        recipes = [r for r in recipes if r.name in wanted]
        if not recipes:
            print(f"no recipes matched {args.only!r}", file=sys.stderr)
            return 2

    out_dir = Path(args.out)
    print(f"running {len(recipes)} recipes x {len(args.images)} images -> {out_dir}")
    manifest = abtest.run_matrix(
        args.images, recipes, out_dir,
        render_profile=args.render_profile or args.profile,
        preview_scale=args.preview_scale,
    )

    (out_dir / "recipes.json").write_text(
        json.dumps([r.to_dict() for r in recipes], indent=2, ensure_ascii=False) + "\n"
    )
    print(f"\ncontact sheet: {out_dir / 'index.html'}")
    print(f"manifest     : {out_dir / 'manifest.json'}")
    print(f"recipes      : {out_dir / 'recipes.json'}")

    if args.push:
        print(f"\npushing {len(manifest['images']) * len(recipes)} images to {args.push}")
        for image in manifest["images"]:
            for r in image["results"]:
                payload = (out_dir / r["bin"]).read_bytes()
                body = abtest.push_to_device(args.push, payload, title=f"{image['name']} · {r['recipe']}")
                print(f"  {image['name']}/{r['recipe']}: {body}")
    return 0


# --------------------------------------------------------------------------
# chart / calibrate / profiles
# --------------------------------------------------------------------------


def cmd_chart(args: argparse.Namespace) -> int:
    codes = calibrate.make_chart()
    payload = pack.pack_2bpp(codes)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(payload)
    print(f"wrote {out} ({len(payload)} bytes)")

    if args.preview:
        profile = PaletteProfile.load(args.profile)
        pack.render_preview(codes, profile, args.preview_scale).save(args.preview)
        print(f"wrote {args.preview}")
    if args.push:
        body = abtest.push_to_device(args.push, payload, title="BWRY calibration chart")
        print(f"pushed to {args.push}: {body}")

    print("\npatches (reading order):")
    for i, (label, _, _, _) in enumerate(calibrate.PATCHES):
        end = "\n" if i % 4 == 3 else "  "
        print(f"  {label:<12}", end=end)
    return 0


def cmd_calibrate(args: argparse.Namespace) -> int:
    if args.swatches:
        parts = [s.strip() for s in args.swatches.split(",")]
        if len(parts) != 4:
            print("--swatches needs 4 hex colours: black,white,yellow,red", file=sys.stderr)
            return 2
        profile = calibrate.profile_from_swatches(*parts, name=args.name)
        report = {"source": "swatches"}
    else:
        if not args.photo:
            print("give a photograph of the chart, or use --swatches", file=sys.stderr)
            return 2
        # open_image applies EXIF orientation and converts via the embedded ICC
        # profile: phone photos are usually Display P3, not sRGB.
        photo = np.asarray(pack.open_image(args.photo), dtype=np.uint8)
        if args.corners:
            nums = [float(v) for v in args.corners.replace(";", ",").split(",")]
            if len(nums) != 8:
                print("--corners needs 8 numbers: x_tl,y_tl,x_tr,y_tr,x_br,y_br,x_bl,y_bl", file=sys.stderr)
                return 2
            corners = np.array(nums, dtype=np.float64).reshape(4, 2)
        else:
            corners = calibrate.find_marks(photo)
            print("auto-detected registration marks (TL, TR, BR, BL):")
            for (x, y) in corners:
                print(f"  {x:8.1f}, {y:8.1f}")
            print("  if these look wrong, re-run with --corners")

        illumination = None if args.no_flat_field else calibrate.estimate_illumination(photo, corners)
        samples = calibrate.sample_patches(photo, corners, illumination=illumination)

        gamma_report = None
        if args.camera_gamma is None:
            gamma, gamma_report = calibrate.fit_camera_gamma(samples)
        else:
            gamma = args.camera_gamma
        raw_samples = samples
        samples = calibrate.apply_camera_gamma(samples, gamma)

        diagnostics = calibrate.photo_diagnostics(samples, illumination, raw=raw_samples)
        diagnostics["camera_gamma"] = round(gamma, 3)

        print("\nphotograph check:")
        if gamma_report:
            print(f"  camera tone curve      : gamma {gamma_report['gamma']:.2f} "
                  f"(fitted from the black/white ramp)")
            print(f"  contrast               : {gamma_report['contrast_before']:.1f}:1 as shot "
                  f"-> {gamma_report['contrast_after']:.1f}:1 corrected")
        else:
            print(f"  camera tone curve      : gamma {gamma:.2f} (given)")
        print(f"  contrast measured      : {diagnostics['contrast_ratio']:.1f}:1")
        if diagnostics["illumination_non_uniformity"] is not None:
            print(f"  light evenness         : {diagnostics['illumination_non_uniformity'] * 100:.0f}% "
                  f"variation across the panel (corrected)")
        for w in diagnostics["warnings"]:
            print(f"  warning: {w}")
        for p in diagnostics["problems"]:
            print(f"  PROBLEM: {p}")

        if diagnostics["problems"] and not args.force:
            print("\nNot writing a profile from this photograph. A profile built on a bad "
                  "measurement is worse than the estimate it would replace -- re-shoot, or "
                  "pass --force if you know better.", file=sys.stderr)
            return 1

        profile, report = calibrate.profile_from_samples(
            samples, name=args.name, notes=f"Measured from {Path(args.photo).name}."
        )
        report["corners"] = [[round(float(v), 1) for v in c] for c in corners]
        report["diagnostics"] = diagnostics
        if gamma_report:
            report["camera_response"] = gamma_report
        if illumination is not None:
            report["illumination_coeffs"] = illumination.coeffs.round(6).tolist()

    out = Path(args.out)
    profile.save(out)
    print(f"\nwrote {out}\n")
    print(profile.describe())

    if "mix_check" in report:
        print("\nhalftone linearity check (predicted vs measured mixes):")
        for label, row in report["mix_check"].items():
            print(f"  {label:<12} {row['predicted_hex']} -> {row['measured_hex']}   dE76 {row['delta_e76']:5.2f}")
        print(f"  mean dE76 {report['mix_check_mean_delta_e']:.2f} -> "
              f"{'linear mixing holds' if report['linear_mixing_ok'] else 'significant dot gain; treat the profile as approximate'}")
        Path(out).with_suffix(".report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


def cmd_profiles(args: argparse.Namespace) -> int:
    names = PaletteProfile.available()
    if args.name:
        print(PaletteProfile.load(args.name).describe())
        return 0
    for name in names:
        print(PaletteProfile.load(name).describe())
        print()
    return 0


def cmd_preview(args: argparse.Namespace) -> int:
    """Render an existing .bin back to PNG, e.g. one pulled off the device."""
    codes = pack.unpack_2bpp(Path(args.input).read_bytes())
    profile = PaletteProfile.load(args.profile)
    img = pack.render_simulated(codes, profile, scale=args.scale) if args.simulated \
        else pack.render_preview(codes, profile, args.scale)
    img.save(args.output)
    print(f"wrote {args.output}")
    return 0


# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bwryctl",
        description="Note4C B/W/R/Y image conversion toolkit (400x300, 2bpp, 30000 bytes).",
    )
    sub = p.add_subparsers(dest="command", required=True)

    c = sub.add_parser("convert", help="convert one image to a device payload")
    c.add_argument("input")
    c.add_argument("output")
    c.add_argument("--preset", default="photo", choices=sorted(PRESETS))
    c.add_argument("--profile", default="note4c-estimate-v1", help="palette profile name or path")
    c.add_argument("--render-profile", help="panel appearance for previews and metrics (defaults to --profile)")
    c.add_argument("--recipe", help="JSON recipe file; overrides --preset")
    c.add_argument("--algorithm", choices=ALGORITHMS)
    c.add_argument("--no-serpentine", action="store_true")
    c.add_argument("--edge-suppress", type=float)
    c.add_argument("--saturation", type=float)
    c.add_argument("--contrast", type=float)
    c.add_argument("--fit", choices=["contain", "cover", "stretch"])
    c.add_argument("--preview", help="write a per-pixel PNG preview here")
    c.add_argument("--simulated", help="write an eye-simulated PNG preview here")
    c.add_argument("--sidecar", help="write the parameter/metric JSON here")
    c.add_argument("--preview-scale", type=int, default=1)
    c.add_argument("--push", metavar="URL", help="also POST to a device, e.g. http://192.168.1.20")
    c.set_defaults(func=cmd_convert)

    a = sub.add_parser("ab", help="run the A/B matrix and build a contact sheet")
    a.add_argument("images", nargs="+")
    a.add_argument("--out", required=True)
    a.add_argument("--profile", default="note4c-estimate-v1", help="palette the recipes convert against")
    a.add_argument("--render-profile", help="panel's measured appearance; used for every preview and "
                   "metric so all candidates are judged against the same physical reality "
                   "(defaults to --profile)")
    a.add_argument("--recipes", help="JSON array of recipes; overrides the built-in matrix")
    a.add_argument("--only", help="comma separated recipe names to run")
    a.add_argument("--preview-scale", type=int, default=1)
    a.add_argument("--push", metavar="URL", help="upload every result to a device")
    a.set_defaults(func=cmd_ab)

    ch = sub.add_parser("chart", help="generate the calibration chart payload")
    ch.add_argument("--out", default="chart_400x300_2bpp.bin")
    ch.add_argument("--preview")
    ch.add_argument("--preview-scale", type=int, default=1)
    ch.add_argument("--profile", default="note4c-estimate-v1")
    ch.add_argument("--push", metavar="URL")
    ch.set_defaults(func=cmd_chart)

    cal = sub.add_parser("calibrate", help="build a palette profile from a photo of the chart")
    cal.add_argument("photo", nargs="?")
    cal.add_argument("--out", required=True)
    cal.add_argument("--name", default="note4c-measured")
    cal.add_argument("--corners", help="x_tl,y_tl,x_tr,y_tr,x_br,y_br,x_bl,y_bl in photo pixels")
    cal.add_argument("--swatches", help="black,white,yellow,red as hex, skipping the photo")
    cal.add_argument("--camera-gamma", type=float,
                     help="tone curve the camera applied, to divide out. Default is to fit it "
                          "from the chart's black/white ramp; pass 1.0 to trust the photo as-is")
    cal.add_argument("--no-flat-field", action="store_true",
                     help="skip the illumination correction fitted from the chart's white border")
    cal.add_argument("--force", action="store_true",
                     help="write a profile even if the photograph failed its checks")
    cal.set_defaults(func=cmd_calibrate)

    pr = sub.add_parser("profiles", help="list or describe palette profiles")
    pr.add_argument("name", nargs="?")
    pr.set_defaults(func=cmd_profiles)

    pv = sub.add_parser("preview", help="render an existing .bin back to PNG")
    pv.add_argument("input")
    pv.add_argument("output")
    pv.add_argument("--profile", default="note4c-estimate-v1")
    pv.add_argument("--scale", type=int, default=1)
    pv.add_argument("--simulated", action="store_true")
    pv.set_defaults(func=cmd_preview)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)
