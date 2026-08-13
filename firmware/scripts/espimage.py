"""Shared helpers for reading ESP-IDF build outputs.

Used by package.py and flash.py. The interesting one is
:func:`app_build_stamp`: it reads the build timestamp ESP-IDF embeds in every
application image, which is what lets both scripts tell a fresh
``merged-binary.bin`` from a stale one. That distinction matters -- ``idf.py
build`` does not regenerate the merged image, so it is entirely possible to
flash a clean-looking binary that quietly predates your last several commits.
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1]
REPO_ROOT = FIRMWARE.parent
ESPTOOL_VENV = REPO_ROOT / ".venv-esptool"
ESPTOOL_REQUIREMENTS = ("esptool", "pyserial")
BUILD = FIRMWARE / "build"

PARTITION_MAGIC = b"\xaa\x50"
PARTITION_ENTRY = 32

#: esp_app_desc_t, which sits at offset 0x20 of every application image.
APP_DESC_MAGIC = b"\x32\x54\xcd\xab"
APP_DESC_OFFSET = 0x20

#: Where ESP-IDF tends to live. $IDF_PATH and --idf-path both win over these.
IDF_CANDIDATES = (
    "~/Developer/esp/v6.0/esp-idf",
    "~/esp/v6.0/esp-idf",
    "~/esp/esp-idf",
    "~/.espressif/frameworks/esp-idf",
    "/opt/esp-idf",
)

PARTITION_TYPES = {0: "app", 1: "data"}


@dataclass(frozen=True)
class Partition:
    name: str
    type: str
    subtype: int
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


@dataclass(frozen=True)
class AppStamp:
    project: str
    version: str
    date: str
    time: str
    idf_version: str

    @property
    def built(self) -> str:
        return f"{self.date} {self.time}"


def read_partition_table(path: Path | None = None) -> dict[str, Partition]:
    path = path or (BUILD / "partition_table" / "partition-table.bin")
    if not path.exists():
        raise FileNotFoundError(
            f"no partition table at {path}. Build first: firmware/scripts/package.py"
        )
    data = path.read_bytes()
    out: dict[str, Partition] = {}
    for i in range(0, len(data), PARTITION_ENTRY):
        entry = data[i : i + PARTITION_ENTRY]
        if entry[:2] != PARTITION_MAGIC:
            continue
        ptype, subtype = entry[2], entry[3]
        offset, size = struct.unpack("<II", entry[4:12])
        name = entry[12:28].split(b"\0")[0].decode("utf-8", "replace")
        out[name] = Partition(name, PARTITION_TYPES.get(ptype, str(ptype)), subtype, offset, size)
    if not out:
        raise ValueError(f"no partitions parsed from {path}")
    return out


def _parse_desc(data: bytes, base: int) -> AppStamp | None:
    def field(start: int, length: int) -> str:
        return data[base + start : base + start + length].split(b"\0")[0].decode("utf-8", "replace")

    project = field(48, 32)
    if not project:
        return None
    return AppStamp(project, field(16, 32), field(96, 16), field(80, 16), field(112, 32))


def app_build_stamp(image: Path) -> AppStamp | None:
    """Build stamp of an application image, or of the app inside a merged image."""
    if not image.exists():
        return None
    data = image.read_bytes()

    # A bare app image has the descriptor at a known offset.
    if data[APP_DESC_OFFSET : APP_DESC_OFFSET + 4] == APP_DESC_MAGIC:
        stamp = _parse_desc(data, APP_DESC_OFFSET)
        if stamp:
            return stamp

    # A merged image has it wherever the app partition landed.
    index = data.find(APP_DESC_MAGIC)
    while index != -1:
        stamp = _parse_desc(data, index)
        if stamp:
            return stamp
        index = data.find(APP_DESC_MAGIC, index + 4)
    return None


def find_idf(explicit: str | None = None) -> Path | None:
    """Locate an ESP-IDF checkout: explicit path, then $IDF_PATH, then the usual spots."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("IDF_PATH"):
        candidates.append(os.environ["IDF_PATH"])
    candidates.extend(IDF_CANDIDATES)

    for candidate in candidates:
        path = Path(candidate).expanduser()
        if (path / "export.sh").exists():
            return path
    return None


def find_idf_tools(idf: Path, explicit: str | None = None) -> Path | None:
    """Locate the toolchain tree that goes with ``idf``.

    This project keeps tools beside the checkout rather than in ``~/.espressif``
    so the whole environment is one self-contained, pinned tree. export.sh does
    not infer that -- without ``IDF_TOOLS_PATH`` it silently falls back to the
    home directory, finds nothing, and leaves ``idf.py`` off PATH. So look for
    the sibling first, and only then let the default stand.
    """
    for candidate in (explicit, os.environ.get("IDF_TOOLS_PATH"), idf.parent / ".espressif"):
        if not candidate:
            continue
        path = Path(candidate).expanduser()
        if (path / "python_env").is_dir():
            return path
    return None


def idf_python_version(tools: Path | None) -> str | None:
    """Python version the IDF environment was installed with, e.g. "3.12".

    export.sh derives the virtualenv directory name from whatever ``python3``
    it finds -- ``idf6.0_py3.12_env`` -- so if the shell's default python has
    moved on since install time, it looks for an environment that was never
    created and reports the toolchain as missing. Reading the version back off
    the installed directory is what lets the activation pin itself.
    """
    if not tools:
        return None
    env_dir = tools / "python_env"
    if not env_dir.is_dir():
        return None
    for child in sorted(env_dir.iterdir()):
        match = re.search(r"_py(\d+\.\d+)_env$", child.name)
        if match:
            return match.group(1)
    return None


def find_python(version: str) -> Path | None:
    """Directory whose ``python3`` is the requested version."""
    candidates = [
        Path(f"/opt/homebrew/opt/python@{version}/libexec/bin"),  # unversioned names
        Path(f"/usr/local/opt/python@{version}/libexec/bin"),
    ]
    for path in candidates:
        if (path / "python3").exists():
            return path

    # Fall back to a versioned binary anywhere on PATH, via a shim directory.
    exe = shutil.which(f"python{version}")
    if exe:
        shim = BUILD / ".idf-python-shim"
        shim.mkdir(parents=True, exist_ok=True)
        link = shim / "python3"
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(exe)
        return shim
    return None


def idf_env_prefix(idf: Path, tools: Path | None) -> str:
    """Shell prelude that puts idf.py on PATH, with the environment pinned."""
    parts = []
    if tools:
        parts.append(f'export IDF_TOOLS_PATH="{tools}"')
    version = idf_python_version(tools)
    if version:
        python_dir = find_python(version)
        if python_dir:
            parts.append(f'export PATH="{python_dir}:$PATH"')
    parts.append(f'. "{idf}/export.sh"')
    return "; ".join(parts)


def human_size(n: int) -> str:
    return f"{n / 1024 / 1024:.2f} MB" if n >= 1 << 20 else f"{n / 1024:.1f} KB"


def _cli() -> int:
    """Print the activation prelude, for ``eval "$(python3 firmware/scripts/espimage.py)"``.

    package.py and flash.py never need this -- they activate their own
    environment internally. This is only for typing bare ``idf.py`` commands
    (menuconfig, monitor, size, ...) by hand, without copy-pasting hardcoded
    paths that go stale the moment the toolchain moves or gets reinstalled
    under a different Python version.
    """
    import argparse

    p = argparse.ArgumentParser(description=_cli.__doc__)
    p.add_argument("--idf-path")
    p.add_argument("--idf-tools-path")
    args = p.parse_args()

    idf = find_idf(args.idf_path)
    if not idf:
        print("could not find ESP-IDF; pass --idf-path or set IDF_PATH", file=sys.stderr)
        return 2
    tools = find_idf_tools(idf, args.idf_tools_path)
    print(idf_env_prefix(idf, tools))
    print(f"echo 'idf.py active: {idf}'", file=sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())


def _venv_python(venv: Path) -> Path:
    return venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python3")


def ensure_esptool_venv(venv: Path = ESPTOOL_VENV) -> Path:
    """Path to a python that can ``-m esptool``, creating a dedicated venv if needed.

    esptool talks to hardware over pyserial; it has no business living in
    whatever ``python3`` happens to be first on PATH (often a Homebrew
    interpreter that refuses ``pip install`` outright as externally-managed).
    This keeps it in its own throwaway venv, the same way ``tools/bwry`` keeps
    its dependencies in ``.venv-imgtool`` rather than the system Python.
    """
    python = _venv_python(venv)
    if python.exists():
        probe = subprocess.run([str(python), "-c", "import esptool, serial"], capture_output=True)
        if probe.returncode == 0:
            return python
    print(f"setting up {venv.relative_to(REPO_ROOT)} (one-time, needed to talk to the device)...")
    subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    subprocess.run(
        [str(python), "-m", "pip", "install", "--quiet", "--upgrade", "pip", *ESPTOOL_REQUIREMENTS],
        check=True,
    )
    return python
