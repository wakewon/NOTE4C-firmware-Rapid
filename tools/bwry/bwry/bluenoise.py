"""Tileable blue-noise threshold mask (Ulichney's void-and-cluster, 1993).

The energy field is maintained incrementally -- adding or removing a minority
pixel just adds or subtracts one toroidally-shifted Gaussian -- so building a
64x64 mask is a few milliseconds instead of a few thousand full convolutions.
"""

from __future__ import annotations

from functools import lru_cache

import numpy as np


def _gaussian_kernel(size: int, sigma: float) -> np.ndarray:
    """Toroidal Gaussian centred on (0, 0), normalised to a peak of 1."""
    d = np.arange(size)
    d = np.minimum(d, size - d)  # wrap-around distance
    dy, dx = np.meshgrid(d, d, indexing="ij")
    return np.exp(-(dx.astype(np.float64) ** 2 + dy.astype(np.float64) ** 2) / (2.0 * sigma**2))


class _Energy:
    """Sum of toroidal Gaussians centred on every set pixel."""

    def __init__(self, size: int, sigma: float):
        self.size = size
        self.kernel = _gaussian_kernel(size, sigma)
        self.field = np.zeros((size, size), dtype=np.float64)

    def _shifted(self, y: int, x: int) -> np.ndarray:
        return np.roll(np.roll(self.kernel, y, axis=0), x, axis=1)

    def add(self, y: int, x: int) -> None:
        self.field += self._shifted(y, x)

    def remove(self, y: int, x: int) -> None:
        self.field -= self._shifted(y, x)

    def rebuild(self, pattern: np.ndarray) -> None:
        self.field[:] = 0.0
        for y, x in zip(*np.nonzero(pattern)):
            self.add(int(y), int(x))


def _tightest_cluster(field: np.ndarray, pattern: np.ndarray) -> tuple[int, int]:
    masked = np.where(pattern, field, -np.inf)
    return np.unravel_index(int(np.argmax(masked)), field.shape)


def _largest_void(field: np.ndarray, pattern: np.ndarray) -> tuple[int, int]:
    masked = np.where(pattern, np.inf, field)
    return np.unravel_index(int(np.argmin(masked)), field.shape)


@lru_cache(maxsize=8)
def void_and_cluster(size: int = 64, sigma: float = 1.9, seed: int = 20260813) -> np.ndarray:
    """Return a ``(size, size)`` float mask of thresholds in ``(0, 1)``.

    The mask tiles seamlessly and its rank ordering is blue-noise distributed,
    so thresholding any grey level against it produces a pattern with no
    low-frequency clumping and no visible grid.
    """
    n = size * size
    rng = np.random.default_rng(seed)

    # --- initial binary pattern -------------------------------------
    pattern = np.zeros((size, size), dtype=bool)
    minority = max(1, int(round(n * 0.1)))
    pattern.flat[rng.choice(n, minority, replace=False)] = True

    energy = _Energy(size, sigma)
    energy.rebuild(pattern)

    for _ in range(n * 4):  # converges in far fewer; the cap is just a guard
        cy, cx = _tightest_cluster(energy.field, pattern)
        pattern[cy, cx] = False
        energy.remove(cy, cx)
        vy, vx = _largest_void(energy.field, pattern)
        pattern[vy, vx] = True
        energy.add(vy, vx)
        if (vy, vx) == (cy, cx):
            break

    initial = pattern.copy()
    rank = np.zeros((size, size), dtype=np.int64)

    # --- phase 1: remove the minority pixels, tightest cluster first --
    work = initial.copy()
    energy.rebuild(work)
    for r in range(minority - 1, -1, -1):
        y, x = _tightest_cluster(energy.field, work)
        work[y, x] = False
        energy.remove(y, x)
        rank[y, x] = r

    # --- phase 2: fill the largest voids, up to half coverage ---------
    work = initial.copy()
    energy.rebuild(work)
    half = (n + 1) // 2
    for r in range(minority, half):
        y, x = _largest_void(energy.field, work)
        work[y, x] = True
        energy.add(y, x)
        rank[y, x] = r

    # --- phase 3: roles reverse, zeros are now the minority -----------
    energy.rebuild(~work)
    for r in range(half, n):
        y, x = _tightest_cluster(energy.field, ~work)
        work[y, x] = True
        energy.remove(y, x)
        rank[y, x] = r

    return (rank.astype(np.float64) + 0.5) / n


def tile(mask: np.ndarray, height: int, width: int, offset: tuple[int, int] = (0, 0)) -> np.ndarray:
    """Tile ``mask`` to cover ``height x width``."""
    my, mx = mask.shape
    ys = (np.arange(height) + offset[0]) % my
    xs = (np.arange(width) + offset[1]) % mx
    return mask[np.ix_(ys, xs)]
