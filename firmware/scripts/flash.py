#!/usr/bin/env python3
"""Flash the device, optionally keeping its Wi-Fi provisioning.

Interactive by default -- run it with no arguments and it lists the serial ports
it can see, marks the ones that look like an ESP32, shows what is in each image
and asks before writing anything. Every choice is also a flag, so it scripts.

    python3 firmware/scripts/flash.py                      # interactive
    python3 firmware/scripts/flash.py --list               # just show the ports
    python3 firmware/scripts/flash.py --port /dev/cu.usbmodem14101 --image merged --yes
    python3 firmware/scripts/flash.py --image app          # application only
    python3 firmware/scripts/flash.py --no-keep-wifi       # let provisioning go
    python3 firmware/scripts/flash.py --backup-only
    python3 firmware/scripts/flash.py --restore firmware/build/nvs_backup/nvs_<stamp>.bin

Why Wi-Fi normally disappears: ``merged-binary.bin`` is written from ``0x0`` and
is around 3 MB, so it runs straight over ``nvs`` at ``0x9000`` -- credentials,
device key, provisioning state. This reads those partitions off first, flashes,
writes them back and reads them again to check. ``--image app`` writes only the
application partition, which never overlaps ``nvs`` at all.

Partitions past the image (``ota_1``, ``assets``) are outside a merged flash, so
the photo album and asset pack are never at risk either way.

If esptool cannot connect: hold BOOT, tap RESET (or re-plug USB while holding
BOOT), release BOOT, and run again.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from espimage import (  # noqa: E402
    BUILD,
    Partition,
    app_build_stamp,
    human_size,
    read_partition_table,
)

BACKUP_DIR = BUILD / "nvs_backup"
MERGED = BUILD / "merged-binary.bin"
APP = BUILD / "xiaozhi.bin"

#: Partitions carried across a flash when --keep-wifi is on.
#: nvs      -- credentials and provisioning state; the reason this exists.
#: phy_init -- RF calibration; regenerated if lost, but keeping it skips a pass.
PRESERVE = ("nvs", "phy_init")

#: USB vendor IDs that show up on ESP32 boards: Espressif native USB, CP210x,
#: CH34x, FTDI. Used only to sort the likely candidates to the top.
LIKELY_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}

DEFAULT_BAUD = 460800


class FlashError(SystemExit):
    pass


# --------------------------------------------------------------------------
# Ports
# --------------------------------------------------------------------------


def list_ports() -> list:
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        raise FlashError(
            "pyserial is not installed. Install esptool, which brings it:\n"
            "  python3 -m pip install --upgrade esptool"
        )
    ports = [p for p in lp.comports() if "Bluetooth" not in (p.device or "")]
    ports.sort(key=lambda p: (p.vid not in LIKELY_VIDS, p.device))
    return ports


def show_ports(ports: list) -> None:
    if not ports:
        print("no serial ports found. Check the cable -- some USB cables are charge-only.")
        return
    print("serial ports:")
    for i, p in enumerate(ports, 1):
        likely = " <- looks like an ESP32" if p.vid in LIKELY_VIDS else ""
        ident = f"{p.vid:04x}:{p.pid:04x}" if p.vid else "-"
        print(f"  {i}. {p.device:<30} {(p.description or '')[:34]:<34} {ident}{likely}")


def choose_port(explicit: str | None, interactive: bool) -> str:
    if explicit:
        return explicit
    ports = list_ports()
    likely = [p for p in ports if p.vid in LIKELY_VIDS]

    if len(likely) == 1 and not interactive:
        print(f"using {likely[0].device} ({likely[0].description})")
        return likely[0].device
    if not ports:
        raise FlashError("no serial ports found; pass --port")

    show_ports(ports)
    if len(likely) == 1:
        default = likely[0].device
        answer = input(f"\nport [{default}]: ").strip()
        return answer or default
    answer = input("\nport (number or path): ").strip()
    if answer.isdigit() and 1 <= int(answer) <= len(ports):
        return ports[int(answer) - 1].device
    if not answer:
        raise FlashError("no port chosen")
    return answer


# --------------------------------------------------------------------------
# esptool
# --------------------------------------------------------------------------


def esptool(port: str, chip: str, baud: int, *args: str, what: str = "esptool") -> None:
    cmd = [sys.executable, "-m", "esptool", "--chip", chip, "--port", port, "--baud", str(baud), *args]
    print("  $ " + " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        raise FlashError("esptool not installed: python3 -m pip install --upgrade esptool")
    except subprocess.CalledProcessError as exc:
        raise FlashError(
            f"\n{what} failed (exit {exc.returncode}).\n"
            "If it could not connect: hold BOOT, tap RESET (or re-plug USB while\n"
            "holding BOOT), release BOOT, and run again. Also check that no serial\n"
            "monitor is holding the port open."
        ) from None


# --------------------------------------------------------------------------
# Backup / restore
# --------------------------------------------------------------------------


def is_blank(path: Path) -> bool:
    data = path.read_bytes()
    return not data or all(b == 0xFF for b in data)


def backup(port: str, chip: str, baud: int, parts: dict[str, Partition], names) -> dict[str, Path]:
    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    saved: dict[str, Path] = {}
    for name in names:
        part = parts.get(name)
        if not part:
            print(f"  skip {name}: not in the partition table")
            continue
        dest = BACKUP_DIR / f"{name}_{stamp}.bin"
        print(f"\nreading {name} (0x{part.offset:x}, {human_size(part.size)}) -> {dest.name}")
        # Must abort before anything is written: flashing after a failed backup
        # is precisely the outcome this script exists to prevent.
        esptool(port, chip, baud, "read-flash", hex(part.offset), hex(part.size), str(dest),
                what=f"backup of {name}")
        if is_blank(dest):
            print(f"  note: {name} is entirely 0xFF -- nothing provisioned yet, nothing to keep")
        else:
            saved[name] = dest
    return saved


def restore(port: str, chip: str, baud: int, parts: dict[str, Partition],
            saved: dict[str, Path], verify: bool) -> bool:
    ok = True
    for name, src in saved.items():
        part = parts[name]
        print(f"\nrestoring {name} (0x{part.offset:x}) from {src.name}")
        esptool(port, chip, baud, "write-flash", hex(part.offset), str(src), what=f"restore of {name}")
        if not verify:
            continue
        check = src.with_name(src.stem + "_verify.bin")
        esptool(port, chip, baud, "read-flash", hex(part.offset), hex(part.size), str(check),
                what=f"verify of {name}")
        same = check.read_bytes() == src.read_bytes()
        print(f"  verify {name}: {'match' if same else 'MISMATCH'}")
        check.unlink(missing_ok=True)
        ok = ok and same
    return ok


# --------------------------------------------------------------------------
# Images
# --------------------------------------------------------------------------


def describe_images(parts: dict[str, Partition]) -> dict[str, dict]:
    app_part = parts.get("ota_0")
    out = {}
    for key, path, offset in (("merged", MERGED, 0), ("app", APP, app_part.offset if app_part else 0x20000)):
        if not path.exists():
            continue
        stamp = app_build_stamp(path)
        out[key] = {
            "path": path,
            "offset": offset,
            "size": path.stat().st_size,
            "stamp": stamp,
        }
    return out


def print_images(images: dict) -> None:
    print("\nimages available:")
    for key, info in images.items():
        built = info["stamp"].built if info["stamp"] else "unknown"
        scope = "whole flash" if key == "merged" else "application only, never touches nvs"
        print(f"  {key:<7} {info['path'].name:<22} 0x{info['offset']:<7x} "
              f"{human_size(info['size']):>9}  app built {built}   ({scope})")

    merged, app = images.get("merged"), images.get("app")
    if merged and app and merged["stamp"] and app["stamp"]:
        if merged["stamp"].built != app["stamp"].built:
            print(
                "\n  WARNING: the two images carry different applications.\n"
                "  idf.py build does not regenerate merged-binary.bin -- run\n"
                "  firmware/scripts/package.py to rebuild both together."
            )


def choose_image(explicit: str | None, images: dict, interactive: bool) -> str:
    if explicit:
        if explicit not in images:
            raise FlashError(f"no {explicit} image on disk; run firmware/scripts/package.py")
        return explicit
    if not images:
        raise FlashError("no images on disk; run firmware/scripts/package.py")
    if not interactive:
        return "merged" if "merged" in images else next(iter(images))
    default = "merged" if "merged" in images else next(iter(images))
    answer = input(f"\nimage to flash {list(images)} [{default}]: ").strip()
    return answer or default


# --------------------------------------------------------------------------


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--list", action="store_true", help="list serial ports and images, then exit")
    p.add_argument("--port", help="serial port; omit to be asked, or autodetected with --yes")
    p.add_argument("--chip", default="esp32s3")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--image", choices=["merged", "app"],
                   help="merged = whole flash from 0x0; app = application partition only")
    p.add_argument("--keep-wifi", dest="keep_wifi", action="store_true", default=True,
                   help="preserve nvs/phy_init across the flash (default)")
    p.add_argument("--no-keep-wifi", dest="keep_wifi", action="store_false",
                   help="let provisioning be erased")
    p.add_argument("--preserve", default=",".join(PRESERVE), help="partitions to carry across")
    p.add_argument("--backup-only", action="store_true", help="snapshot nvs and stop")
    p.add_argument("--restore", type=Path, help="write an nvs snapshot back and stop")
    p.add_argument("--no-verify", action="store_true")
    p.add_argument("--yes", action="store_true", help="non-interactive; take the defaults")
    args = p.parse_args()

    interactive = not args.yes

    if args.list:
        show_ports(list_ports())
        try:
            print_images(describe_images(read_partition_table()))
        except FileNotFoundError as exc:
            print(f"\n{exc}")
        return 0

    parts = read_partition_table()
    images = describe_images(parts)

    print("partition table:")
    for part in parts.values():
        mark = ""
        if args.keep_wifi and part.name in args.preserve.split(","):
            mark = "  <- preserved"
        print(f"  {part.name:<10} 0x{part.offset:07x}  {human_size(part.size):>9}{mark}")

    port = choose_port(args.port, interactive and not args.port)

    if args.restore:
        return 0 if restore(port, args.chip, args.baud, parts,
                            {"nvs": args.restore}, not args.no_verify) else 1

    names = [n.strip() for n in args.preserve.split(",") if n.strip()]

    if args.backup_only:
        backup(port, args.chip, args.baud, parts, names)
        print(f"\nsnapshots in {BACKUP_DIR}")
        return 0

    print_images(images)
    kind = choose_image(args.image, images, interactive)
    image = images[kind]

    # An application-only flash starts at ota_0, well past nvs at 0x9000, so
    # provisioning is safe without doing anything.
    nvs = parts.get("nvs")
    overlaps_nvs = bool(nvs and image["offset"] <= nvs.offset < image["offset"] + image["size"])
    keep = args.keep_wifi and overlaps_nvs
    if args.keep_wifi and not overlaps_nvs:
        print("\nthis image does not reach nvs, so provisioning survives on its own")

    if image["stamp"]:
        print(f"\nabout to write {image['path'].name} to 0x{image['offset']:x} on {port}")
        print(f"  application built {image['stamp'].built}  (v{image['stamp'].version}, "
              f"IDF {image['stamp'].idf_version})")
    if keep:
        print(f"  {', '.join(names)} will be read out first and written back afterwards")
    elif args.keep_wifi is False and overlaps_nvs:
        print("  provisioning WILL be erased (--no-keep-wifi)")

    if interactive and input("\ncontinue? [y/N] ").strip().lower() not in ("y", "yes"):
        print("aborted; nothing written")
        return 1

    saved = backup(port, args.chip, args.baud, parts, names) if keep else {}

    print(f"\nflashing {image['path'].name} at 0x{image['offset']:x}")
    esptool(port, args.chip, args.baud, "write-flash", hex(image["offset"]), str(image["path"]),
            what="flash")

    if saved and not restore(port, args.chip, args.baud, parts, saved, not args.no_verify):
        print(f"\nrestore did not verify. Snapshots are still in {BACKUP_DIR};\n"
              f"retry with --restore <file>.", file=sys.stderr)
        return 1

    print("\ndone." + ("  Wi-Fi configuration preserved." if saved else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
