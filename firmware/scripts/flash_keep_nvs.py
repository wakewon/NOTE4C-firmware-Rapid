#!/usr/bin/env python3
"""Flash the device without losing its Wi-Fi credentials.

``merged-binary.bin`` is written from ``0x0`` and is ~3 MB, so it runs straight
over ``nvs`` at ``0x9000`` -- which is where Wi-Fi credentials, the device key
and the rest of the provisioning state live. That is why a full flash always
means setting the network up again.

This reads ``nvs`` off the device first, flashes, then writes it back and
verifies the read-back matches. Partitions past the image (``ota_1`` at
``0x410000``, ``assets`` at ``0x800000``) are never touched by a merged flash,
so the photo album and asset pack survive on their own.

    python3 firmware/scripts/flash_keep_nvs.py --port /dev/cu.usbmodem1101

Add ``--backup-only`` to just take a snapshot, or ``--restore FILE`` to push a
previous snapshot back without flashing.

The partition offsets are read from the built partition table rather than
hardcoded, so this stays correct if the layout changes.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1]
BUILD = FIRMWARE / "build"
PARTITION_TABLE = BUILD / "partition_table" / "partition-table.bin"
BACKUP_DIR = BUILD / "nvs_backup"

PARTITION_MAGIC = b"\xaa\x50"

#: Partitions worth carrying across a flash, in priority order.
#: nvs  -- Wi-Fi credentials and provisioning state; the whole point.
#: phy_init -- RF calibration. Regenerated if lost, but keeping it avoids a
#:             recalibration pass on first boot.
PRESERVE = ("nvs", "phy_init")


class EsptoolError(SystemExit):
    pass


def run(cmd: list[str], what: str = "esptool", **kw) -> subprocess.CompletedProcess:
    print("  $ " + " ".join(cmd))
    try:
        return subprocess.run(cmd, check=True, **kw)
    except subprocess.CalledProcessError as exc:
        raise EsptoolError(
            f"\n{what} failed (exit {exc.returncode}).\n"
            "Check the cable, that the port is right, and that nothing else "
            "(a serial monitor) is holding it open."
        ) from None


def esptool_cmd(port: str | None, chip: str) -> list[str]:
    cmd = [sys.executable, "-m", "esptool", "--chip", chip]
    if port:
        cmd += ["--port", port]
    return cmd


def read_partitions(table: Path) -> dict[str, tuple[int, int]]:
    """{name: (offset, size)} from a built partition-table.bin."""
    if not table.exists():
        raise SystemExit(
            f"no partition table at {table}\n"
            "Build first (idf.py build), or pass --nvs-offset/--nvs-size explicitly."
        )
    data = table.read_bytes()
    out: dict[str, tuple[int, int]] = {}
    for i in range(0, len(data), 32):
        entry = data[i : i + 32]
        if entry[:2] != PARTITION_MAGIC:
            continue
        offset, size = struct.unpack("<II", entry[4:12])
        name = entry[12:28].split(b"\0")[0].decode("utf-8", "replace")
        out[name] = (offset, size)
    if not out:
        raise SystemExit(f"could not parse any partitions from {table}")
    return out


def is_blank(path: Path) -> bool:
    data = path.read_bytes()
    return not data or all(b == 0xFF for b in data)


def backup(port: str | None, chip: str, parts: dict, names, stamp: str) -> dict[str, Path]:
    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    saved: dict[str, Path] = {}
    for name in names:
        if name not in parts:
            print(f"  skip {name}: not in the partition table")
            continue
        offset, size = parts[name]
        dest = BACKUP_DIR / f"{name}_{stamp}.bin"
        print(f"\nreading {name} (0x{offset:x}, {size // 1024} KB) -> {dest.name}")
        # A failure here must abort before anything is written: flashing after
        # a failed backup is exactly the situation this script exists to avoid.
        run(esptool_cmd(port, chip) + ["read-flash", hex(offset), hex(size), str(dest)],
            what=f"backup of {name}")
        if is_blank(dest):
            print(f"  note: {name} is entirely 0xFF -- nothing was provisioned yet, "
                  "so there is nothing to preserve")
        else:
            saved[name] = dest
    return saved


def restore(port: str | None, chip: str, parts: dict, saved: dict[str, Path], verify: bool) -> bool:
    ok = True
    for name, src in saved.items():
        offset, size = parts[name]
        print(f"\nrestoring {name} (0x{offset:x}) from {src.name}")
        run(esptool_cmd(port, chip) + ["write-flash", hex(offset), str(src)])

        if not verify:
            continue
        check = src.with_name(src.stem + "_verify.bin")
        run(esptool_cmd(port, chip) + ["read-flash", hex(offset), hex(size), str(check)])
        same = check.read_bytes() == src.read_bytes()
        print(f"  verify {name}: {'match' if same else 'MISMATCH'}")
        check.unlink(missing_ok=True)
        ok = ok and same
    return ok


def flash(port: str | None, chip: str, merged: Path, args_file: Path | None) -> None:
    if merged.exists():
        print(f"\nflashing {merged.name} at 0x0")
        run(esptool_cmd(port, chip) + ["write-flash", "0x0", str(merged)])
        return

    # Fall back to the per-image offsets ESP-IDF recorded at build time.
    if not args_file or not args_file.exists():
        raise SystemExit(f"neither {merged} nor {args_file} exists; run idf.py build (and merge-bin)")
    print(f"\n{merged.name} not found; flashing the images listed in {args_file.name}")
    cmd = esptool_cmd(port, chip) + ["write-flash"]
    for line in args_file.read_text().split("\n"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("--"):
            cmd += line.split()
            continue
        offset, image = line.split()
        cmd += [offset, str(BUILD / image)]
    run(cmd, cwd=BUILD)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port, e.g. /dev/cu.usbmodem1101. Omitted = esptool autodetect")
    p.add_argument("--chip", default="esp32s3")
    p.add_argument("--merged", type=Path, default=BUILD / "merged-binary.bin")
    p.add_argument("--preserve", default=",".join(PRESERVE),
                   help=f"comma-separated partitions to carry across (default: {','.join(PRESERVE)})")
    p.add_argument("--backup-only", action="store_true", help="snapshot and stop, do not flash")
    p.add_argument("--restore", type=Path, help="write this nvs snapshot back and stop")
    p.add_argument("--no-verify", action="store_true")
    p.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    args = p.parse_args()

    parts = read_partitions(PARTITION_TABLE)
    names = [n.strip() for n in args.preserve.split(",") if n.strip()]
    stamp = time.strftime("%Y%m%d-%H%M%S")

    print("partition table:")
    for name, (offset, size) in parts.items():
        mark = "  <- preserved" if name in names else ""
        print(f"  {name:<10} 0x{offset:07x}  {size // 1024:>5} KB{mark}")

    if args.restore:
        if "nvs" not in parts:
            raise SystemExit("no nvs partition in the table")
        return 0 if restore(args.port, args.chip, parts, {"nvs": args.restore}, not args.no_verify) else 1

    saved = backup(args.port, args.chip, parts, names, stamp)

    if args.backup_only:
        print(f"\nsnapshots in {BACKUP_DIR}")
        return 0

    if not saved:
        print("\nnothing to preserve; this will be an ordinary flash")

    if not args.yes:
        print(f"\nAbout to erase and rewrite the device from {args.merged}.")
        print(f"Snapshots are in {BACKUP_DIR} and will be written back afterwards.")
        if input("continue? [y/N] ").strip().lower() not in ("y", "yes"):
            print("aborted; snapshots kept")
            return 1

    flash(args.port, args.chip, args.merged, BUILD / "flash_args")

    if saved and not restore(args.port, args.chip, parts, saved, not args.no_verify):
        print("\nrestore did not verify. The snapshots are still in "
              f"{BACKUP_DIR}; retry with --restore <file>.", file=sys.stderr)
        return 1

    print("\ndone. Wi-Fi configuration preserved." if saved else "\ndone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
