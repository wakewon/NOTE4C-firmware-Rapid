#!/usr/bin/env python3
"""Entry point for the Note4C B/W/R/Y conversion toolkit.

    python3 tools/bwry/bwryctl.py convert photo.jpg out.bin --preset photo
    python3 tools/bwry/bwryctl.py ab tmp/Cubes.jpg --out tmp/ab

Needs numpy, pillow and scipy; see tools/bwry/requirements.txt.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bwry.cli import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
