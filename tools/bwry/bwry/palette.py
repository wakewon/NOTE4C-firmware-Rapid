"""Calibrated B/W/R/Y palette profiles for the Note4C panel.

A *profile* describes what the four inks actually look like on the physical
panel, in **media-relative** sRGB: the paper white of the panel is normalised to
(near) 255 so that the profile survives changes in ambient light level and only
encodes what matters, i.e. the relative lightness / hue / chroma of the four
inks against their own paper.

Nothing here touches the device encoding: ``device_code`` is fixed by the
firmware (``rawdraw::Color``) and is carried through unchanged.

    BLACK = 0, WHITE = 1, YELLOW = 2, RED = 3
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import numpy as np

from . import color as C

PROFILE_DIR = Path(__file__).resolve().parent / "profiles"

# Firmware device codes; see firmware/main/rawdraw/rawdraw.h
DEVICE_CODES = {"black": 0, "white": 1, "yellow": 2, "red": 3}


@dataclass(frozen=True)
class Ink:
    name: str
    device_code: int
    srgb: np.ndarray  # media-relative sRGB, float 0..1

    @property
    def lab(self) -> np.ndarray:
        return C.srgb_to_lab(self.srgb)

    @property
    def xyz(self) -> np.ndarray:
        return C.srgb_to_xyz(self.srgb)


@dataclass
class PaletteProfile:
    """Four inks plus provenance metadata."""

    name: str
    inks: list[Ink]
    display: str = ""
    source: str = "unknown"
    notes: str = ""
    measured_reflectance: dict = field(default_factory=dict)

    # -- derived, cached -----------------------------------------------
    def __post_init__(self) -> None:
        order = {"black": 0, "white": 1, "yellow": 2, "red": 3}
        self.inks = sorted(self.inks, key=lambda i: order.get(i.name, 99))
        self._srgb = np.stack([i.srgb for i in self.inks])
        self._lab = C.srgb_to_lab(self._srgb)
        self._xyz = C.srgb_to_xyz(self._srgb)
        self._codes = np.array([i.device_code for i in self.inks], dtype=np.uint8)

    @property
    def srgb(self) -> np.ndarray:
        """(N, 3) media-relative sRGB, 0..1, in profile order."""
        return self._srgb

    @property
    def lab(self) -> np.ndarray:
        """(N, 3) Lab of each ink."""
        return self._lab

    @property
    def xyz(self) -> np.ndarray:
        """(N, 3) XYZ of each ink. Spatial mixing is linear in this space."""
        return self._xyz

    @property
    def device_codes(self) -> np.ndarray:
        """(N,) uint8 device colour codes, aligned with :attr:`srgb`."""
        return self._codes

    @property
    def names(self) -> list[str]:
        return [i.name for i in self.inks]

    def index_of(self, name: str) -> int:
        return self.names.index(name)

    @property
    def achromatic_mask(self) -> np.ndarray:
        """True for inks that carry (essentially) no chroma: black and white."""
        return C.chroma(self._lab) < 12.0

    @property
    def l_black(self) -> float:
        return float(self._lab[self.index_of("black"), 0])

    @property
    def l_white(self) -> float:
        return float(self._lab[self.index_of("white"), 0])

    @property
    def contrast_ratio(self) -> float:
        """Y_white : Y_black, the classic e-paper contrast figure."""
        yw = float(self._xyz[self.index_of("white"), 1])
        yb = float(self._xyz[self.index_of("black"), 1])
        return yw / max(yb, 1e-9)

    # -- serialisation --------------------------------------------------
    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "display": self.display,
            "source": self.source,
            "notes": self.notes,
            "measured_reflectance": self.measured_reflectance,
            "inks": [
                {
                    "name": i.name,
                    "device_code": i.device_code,
                    "srgb": [int(v) for v in C.srgb_to_u8(i.srgb)],
                    "hex": C.srgb_to_hex(i.srgb),
                    "lab": [round(float(v), 2) for v in i.lab],
                }
                for i in self.inks
            ],
        }

    def save(self, path: str | Path) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, ensure_ascii=False) + "\n")
        return path

    @staticmethod
    def from_dict(data: dict) -> "PaletteProfile":
        inks: list[Ink] = []
        for entry in data["inks"]:
            name = entry["name"]
            if "srgb" in entry:
                srgb = np.asarray(entry["srgb"], dtype=np.float64) / 255.0
            elif "hex" in entry:
                srgb = C.hex_to_srgb(entry["hex"])
            else:
                raise ValueError(f"ink {name!r} needs 'srgb' or 'hex'")
            code = int(entry.get("device_code", DEVICE_CODES[name]))
            inks.append(Ink(name=name, device_code=code, srgb=srgb))
        return PaletteProfile(
            name=data.get("name", "unnamed"),
            inks=inks,
            display=data.get("display", ""),
            source=data.get("source", "unknown"),
            notes=data.get("notes", ""),
            measured_reflectance=data.get("measured_reflectance", {}),
        )

    @staticmethod
    def load(ref: str | Path) -> "PaletteProfile":
        """Load by profile name (``note4c-estimate-v1``) or by file path."""
        path = Path(ref)
        if not path.exists():
            candidate = PROFILE_DIR / f"{str(ref).replace('-', '_')}.json"
            if candidate.exists():
                path = candidate
            else:
                available = ", ".join(sorted(p.stem.replace("_", "-") for p in PROFILE_DIR.glob("*.json")))
                raise FileNotFoundError(f"unknown palette profile {ref!r}; available: {available}")
        return PaletteProfile.from_dict(json.loads(path.read_text()))

    @staticmethod
    def available() -> list[str]:
        return sorted(p.stem.replace("_", "-") for p in PROFILE_DIR.glob("*.json"))

    # -- rendering ------------------------------------------------------
    def render_codes(self, codes: np.ndarray) -> np.ndarray:
        """(H, W) device codes -> (H, W, 3) uint8 sRGB preview using this profile."""
        lut = np.zeros((4, 3), dtype=np.uint8)
        for ink in self.inks:
            lut[ink.device_code] = C.srgb_to_u8(ink.srgb)
        return lut[np.asarray(codes, dtype=np.uint8)]

    def xyz_of_codes(self, codes: np.ndarray) -> np.ndarray:
        """(H, W) device codes -> (H, W, 3) XYZ, for linear-light simulation."""
        lut = np.zeros((4, 3), dtype=np.float64)
        for ink in self.inks:
            lut[ink.device_code] = ink.xyz
        return lut[np.asarray(codes, dtype=np.uint8)]

    def describe(self) -> str:
        rows = [f"profile: {self.name}  ({self.source})"]
        if self.display:
            rows.append(f"display: {self.display}")
        for ink, lab in zip(self.inks, self._lab):
            lch = C.lab_to_lch(lab)
            rows.append(
                f"  {ink.name:<6} code={ink.device_code}  {C.srgb_to_hex(ink.srgb)}  "
                f"L*={lab[0]:6.2f}  C*={lch[1]:5.2f}  h={lch[2]:6.1f}deg"
            )
        rows.append(f"  contrast (Yw/Yb): {self.contrast_ratio:.1f}:1   L* range: {self.l_black:.1f}..{self.l_white:.1f}")
        return "\n".join(rows)


def build_profile(
    name: str,
    colors: dict[str, Iterable[float]],
    *,
    display: str = "",
    source: str = "unknown",
    notes: str = "",
    measured_reflectance: dict | None = None,
) -> PaletteProfile:
    """Build a profile from ``{"black": (r, g, b), ...}`` with 0..255 values."""
    inks = [
        Ink(name=n, device_code=DEVICE_CODES[n], srgb=np.asarray(v, dtype=np.float64) / 255.0)
        for n, v in colors.items()
    ]
    return PaletteProfile(
        name=name,
        inks=inks,
        display=display,
        source=source,
        notes=notes,
        measured_reflectance=measured_reflectance or {},
    )
