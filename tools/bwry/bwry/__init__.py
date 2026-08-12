"""Note4C B/W/R/Y image conversion toolkit.

Turns an ordinary photograph into the 400x300, 2bpp, 30,000-byte framebuffer the
Note4C firmware expects, without changing the device colour encoding or anything
about how SSD2683 refreshes.
"""

from .dither import ALGORITHMS, DitherParams
from .palette import PaletteProfile
from .pipeline import Recipe, Result, convert, write_outputs
from .presets import PRESETS, ab_matrix, get_preset
from .tone import ChromaGate, ToneParams

__all__ = [
    "ALGORITHMS",
    "ChromaGate",
    "DitherParams",
    "PaletteProfile",
    "PRESETS",
    "Recipe",
    "Result",
    "ToneParams",
    "ab_matrix",
    "convert",
    "get_preset",
    "write_outputs",
]
