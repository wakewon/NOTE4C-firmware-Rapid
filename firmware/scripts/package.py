#!/usr/bin/env python3
"""Build the firmware and both flashable images, together, every time.

``idf.py build`` produces ``xiaozhi.bin`` (the OTA/application image) but does
*not* regenerate ``merged-binary.bin`` (the whole-flash image). Build without
also running ``merge-bin`` and the merged image on disk silently keeps whatever
it held before -- it still flashes cleanly, it just isn't your code. That has
already cost one debugging session here: the merged image was nine commits
behind and the fast-refresh ladder appeared to be "missing".

So this script always does both, and then refuses to declare success unless the
application inside the merged image carries the same build stamp as the
standalone one.

    python3 firmware/scripts/package.py
    python3 firmware/scripts/package.py --out firmware/releases   # keep a copy
    python3 firmware/scripts/package.py --clean                   # full rebuild
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from espimage import (  # noqa: E402
    BUILD,
    FIRMWARE,
    app_build_stamp,
    find_idf,
    find_idf_tools,
    human_size,
    idf_env_prefix,
)

MERGED = BUILD / "merged-binary.bin"
APP = BUILD / "xiaozhi.bin"


def check_idf_usable(idf: Path, tools: Path | None) -> None:
    """Fail early and specifically if the toolchain was never installed.

    A checked-out ESP-IDF is not a usable one: export.sh needs the Python
    environment and toolchains that install.sh downloads. Without them export.sh
    still exits 0 and simply does not put idf.py on PATH, so the first symptom
    is a bare 'command not found' several steps later.
    """
    probe = subprocess.run(
        ["bash", "-c", f"{idf_env_prefix(idf, tools)} 2>&1; command -v idf.py"],
        capture_output=True, text=True,
    )
    if "idf.py" in probe.stdout and "/" in probe.stdout:
        return
    detail = probe.stdout.strip().split("\n")
    hint = next((line for line in detail if "ERROR" in line), "")
    raise SystemExit(
        f"ESP-IDF at {idf} is not usable yet.\n"
        + (f"  {hint}\n" if hint else "")
        + f"Install the toolchain first (this downloads several GB):\n"
        f"  {idf}/install.sh esp32s3"
    )


def run_in_idf(idf: Path, tools: Path | None, commands: list[str], quiet: bool) -> int:
    """Run idf.py commands inside an ESP-IDF environment."""
    script = f"{idf_env_prefix(idf, tools)} >/dev/null 2>&1 && " + " && ".join(commands)
    print(f"  $ {' && '.join(commands)}")
    proc = subprocess.run(
        ["bash", "-c", script],
        cwd=FIRMWARE,
        stdout=subprocess.PIPE if quiet else None,
        stderr=subprocess.STDOUT if quiet else None,
        text=True,
    )
    if proc.returncode != 0 and quiet and proc.stdout:
        # Only surface the tail; a failed IDF build is thousands of lines.
        print("\n".join(proc.stdout.strip().split("\n")[-40:]), file=sys.stderr)
    return proc.returncode


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--idf-path", help="ESP-IDF checkout (default: $IDF_PATH, then the usual locations)")
    p.add_argument("--idf-tools-path",
                   help="toolchain tree (default: $IDF_TOOLS_PATH, then .espressif beside the checkout)")
    p.add_argument("--out", type=Path, help="also copy the images here, with a build stamp in the name")
    p.add_argument("--clean", action="store_true", help="fullclean first")
    p.add_argument("--quiet", action="store_true", help="only show build output if it fails")
    args = p.parse_args()

    idf = find_idf(args.idf_path)
    if not idf:
        print(
            "could not find ESP-IDF.\n"
            "Pass --idf-path, or set IDF_PATH. Known checkout on this machine:\n"
            "  ~/Developer/esp/v6.0/esp-idf",
            file=sys.stderr,
        )
        return 2
    tools = find_idf_tools(idf, args.idf_tools_path)
    print(f"ESP-IDF: {idf}")
    print(f"  tools: {tools if tools else '(default ~/.espressif)'}")
    check_idf_usable(idf, tools)

    commands = []
    if args.clean:
        commands.append("idf.py fullclean")
    commands += ["idf.py build", "idf.py merge-bin"]

    started = time.time()
    rc = run_in_idf(idf, tools, commands, args.quiet)
    if rc != 0:
        print(f"\nbuild failed (exit {rc})", file=sys.stderr)
        return rc

    missing = [str(f) for f in (APP, MERGED) if not f.exists()]
    if missing:
        print(f"\nbuild reported success but these are missing: {missing}", file=sys.stderr)
        return 1

    app_stamp = app_build_stamp(APP)
    merged_stamp = app_build_stamp(MERGED)
    if not app_stamp or not merged_stamp:
        print("\ncould not read a build stamp out of the images", file=sys.stderr)
        return 1

    print(f"\nbuilt in {time.time() - started:.0f}s")
    print(f"  {APP.name:<20} {human_size(APP.stat().st_size):>9}   app built {app_stamp.built}")
    print(f"  {MERGED.name:<20} {human_size(MERGED.stat().st_size):>9}   app built {merged_stamp.built}")

    # The whole point of this script.
    if app_stamp.built != merged_stamp.built:
        print(
            f"\nthe merged image carries a different application than the standalone one\n"
            f"  {APP.name}:    {app_stamp.built}\n"
            f"  {MERGED.name}: {merged_stamp.built}\n"
            "merge-bin did not pick up this build. Do not flash either image.",
            file=sys.stderr,
        )
        return 1
    print("  both images carry the same application ✓")

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        tag = f"{app_stamp.version}-{app_stamp.date.replace(' ', '')}-{app_stamp.time.replace(':', '')}"
        for src, kind in ((MERGED, "merged"), (APP, "app")):
            dest = args.out / f"{app_stamp.project}-{tag}-{kind}.bin"
            shutil.copy2(src, dest)
            print(f"  -> {dest}")

    print("\nflash with: python3 firmware/scripts/flash.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
