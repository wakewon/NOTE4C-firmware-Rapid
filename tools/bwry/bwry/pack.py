"""Device framebuffer packing and PC preview rendering.

The output format is fixed by the firmware and is not up for negotiation here:

    400 x 300, 2 bits per pixel, 4 pixels per byte, MSB first
    pixel 0 -> bits 7..6, pixel 3 -> bits 1..0
    black = 0, white = 1, yellow = 2, red = 3
    total 30,000 bytes

See ``firmware/main/rawdraw/rawdraw.h`` and the ``/upload?format=bwry2bpp``
handler in ``firmware/main/ui/renderers/rawdraw/ap_transfer_server.cc``.
"""

from __future__ import annotations

import numpy as np
from PIL import Image
from scipy import ndimage

from . import color as C
from .palette import PaletteProfile

SCREEN_WIDTH = 400
SCREEN_HEIGHT = 300
SIZE_2BPP = SCREEN_WIDTH * SCREEN_HEIGHT * 2 // 8  # 30000
SIZE_1BPP = SCREEN_WIDTH * SCREEN_HEIGHT // 8  # 15000


def pack_2bpp(codes: np.ndarray) -> bytes:
    codes = np.asarray(codes, dtype=np.uint8).ravel()
    if codes.size % 4:
        raise ValueError("pixel count must be a multiple of 4")
    if codes.max(initial=0) > 3:
        raise ValueError("device colour codes must be 0..3")
    q = codes.reshape(-1, 4).astype(np.uint8)
    packed = (q[:, 0] << 6) | (q[:, 1] << 4) | (q[:, 2] << 2) | q[:, 3]
    return packed.astype(np.uint8).tobytes()


def unpack_2bpp(data: bytes, width: int = SCREEN_WIDTH, height: int = SCREEN_HEIGHT) -> np.ndarray:
    arr = np.frombuffer(data, dtype=np.uint8)
    if arr.size != width * height // 4:
        raise ValueError(f"expected {width * height // 4} bytes, got {arr.size}")
    codes = np.stack([(arr >> 6) & 3, (arr >> 4) & 3, (arr >> 2) & 3, arr & 3], axis=1)
    return codes.reshape(height, width).astype(np.uint8)


# --------------------------------------------------------------------------
# Preview rendering
# --------------------------------------------------------------------------


def render_preview(codes: np.ndarray, profile: PaletteProfile, scale: int = 1) -> Image.Image:
    """Exact per-pixel render using the profile's measured ink colours."""
    rgb = profile.render_codes(codes)
    img = Image.fromarray(rgb, mode="RGB")
    if scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.Resampling.NEAREST)
    return img


def render_simulated(
    codes: np.ndarray, profile: PaletteProfile, sigma: float = 0.75, scale: int = 1
) -> Image.Image:
    """Approximate how the panel looks to the eye at normal viewing distance.

    The halftone is integrated in linear light -- which is what actually happens
    optically -- rather than in sRGB, so mid-tones do not drift the way a naive
    blur of the preview PNG would.
    """
    xyz = profile.xyz_of_codes(codes)
    if sigma > 0:
        xyz = ndimage.gaussian_filter(xyz, sigma=(sigma, sigma, 0), mode="nearest")
    rgb = C.srgb_to_u8(C.xyz_to_srgb(xyz))
    img = Image.fromarray(rgb, mode="RGB")
    if scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.Resampling.LANCZOS)
    return img


# --------------------------------------------------------------------------
# Source loading
# --------------------------------------------------------------------------


def to_srgb(img: Image.Image) -> Image.Image:
    """Convert an image to sRGB using its embedded ICC profile, if it has one.

    Phones do not shoot sRGB by default -- iPhones tag Display P3, and several
    Android makers ship wider spaces too. Reading a P3 file as though it were
    sRGB makes every saturated colour read hotter than it is, which for
    calibration means baking a systematic error straight into the profile.
    """
    icc = img.info.get("icc_profile")
    if not icc:
        return img.convert("RGB")
    try:
        from io import BytesIO

        from PIL import ImageCms

        src = ImageCms.ImageCmsProfile(BytesIO(icc))
        dst = ImageCms.createProfile("sRGB")
        return ImageCms.profileToProfile(img.convert("RGB"), src, dst, outputMode="RGB")
    except Exception:
        # A broken or unsupported profile should not stop a conversion; sRGB is
        # the right guess when we have nothing better.
        return img.convert("RGB")


def open_image(path) -> Image.Image:
    """Open, apply EXIF orientation, and normalise to sRGB. Returns RGB.

    Camera RAW files are developed rather than decoded -- see :mod:`.rawdev`.
    """
    from PIL import ImageOps

    from .rawdev import develop, is_raw

    if is_raw(path):
        rgb, _ = develop(path)
        return Image.fromarray(rgb, mode="RGB")

    img = ImageOps.exif_transpose(Image.open(path))
    if img.mode in ("RGBA", "LA", "P"):
        rgba = img.convert("RGBA")
        flat = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        flat.alpha_composite(rgba)
        img = flat
    return to_srgb(img)


def load_and_fit(
    path,
    width: int = SCREEN_WIDTH,
    height: int = SCREEN_HEIGHT,
    fit: str = "contain",
    background=(255, 255, 255),
) -> np.ndarray:
    """Decode, honour EXIF orientation and fit to the panel. Returns uint8 RGB."""
    from PIL import ImageOps

    img = Image.open(path)
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        flat = Image.new("RGBA", img.size, tuple(background) + (255,))
        flat.alpha_composite(img)
        img = flat
    img = to_srgb(img)

    if fit == "cover":
        out = ImageOps.fit(img, (width, height), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    elif fit == "stretch":
        out = img.resize((width, height), Image.Resampling.LANCZOS)
    else:  # contain: letterbox on the background colour, matching the firmware page
        out = Image.new("RGB", (width, height), tuple(background))
        scaled = ImageOps.contain(img, (width, height), method=Image.Resampling.LANCZOS)
        out.paste(scaled, ((width - scaled.width) // 2, (height - scaled.height) // 2))

    return np.asarray(out, dtype=np.uint8)
