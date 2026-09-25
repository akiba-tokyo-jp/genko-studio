"""Colour for print (CMYK・カラープロファイル): RGB pages as CMYK, through the printer's ICC profile (Japan
Color and the like, given as a file) or, without one, a plain conversion that prints black lines on the black
plate only and keeps the total ink under a limit; a proof of how the CMYK will look on screen; and the sRGB
profile embedded in RGB files so other apps show the colours as Genko does.
"""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from PIL import Image

INK_LIMIT = 320  # % total ink (C+M+Y+K) most coated-paper printers ask for at most
INTENTS = ("perceptual", "relative", "saturation", "absolute")


@lru_cache(maxsize=1)
def srgb_icc() -> bytes:
    from PIL import ImageCms

    return ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()


def _profile(path):
    from PIL import ImageCms

    try:
        return ImageCms.getOpenProfile(str(path))
    except (OSError, ImageCms.PyCMSError) as exc:
        raise ValueError(f"the colour profile cannot be read ({exc})") from exc


def profile_name(path) -> str:
    from PIL import ImageCms

    try:
        return ImageCms.getProfileDescription(_profile(path)).strip() or Path(path).name
    except ImageCms.PyCMSError:
        return Path(path).name


def is_cmyk_profile(path) -> bool:
    return _profile(path).profile.xcolor_space.strip() == "CMYK"


def to_cmyk(image: Image.Image, icc: str | Path | None = None, *, ink_limit: int = INK_LIMIT,
            intent: str = "relative") -> Image.Image:
    """The picture in CMYK. With `icc` (a CMYK output profile) through it, black point compensated; without,
    grey component replacement: neutral greys and black print on K alone, the rest keeps under `ink_limit`%."""
    rgb = image.convert("RGB")
    if icc:
        from PIL import ImageCms

        if not is_cmyk_profile(icc):
            raise ValueError("the profile is not a CMYK printing profile")
        transform = ImageCms.buildTransform(ImageCms.createProfile("sRGB"), _profile(icc), "RGB", "CMYK",
                                            renderingIntent=INTENTS.index(intent),
                                            flags=ImageCms.Flags.BLACKPOINTCOMPENSATION)
        return ImageCms.applyTransform(rgb, transform)
    import numpy as np

    a = np.asarray(rgb, dtype=np.float32) / 255
    k = 1 - a.max(axis=2)
    denom = np.where(k < 1, 1 - k, 1)
    c, m, y = ((1 - a[..., i] - k) / denom for i in range(3))
    # (full grey replacement: what the three colours share goes to black; colour stays colour)
    total = c + m + y + k
    limit = ink_limit / 100
    over = total > limit
    if over.any():
        room = np.clip(limit - k, 0, None)
        cmy = c + m + y
        scale = np.where(over & (cmy > 0), room / np.maximum(cmy, 1e-6), 1)
        c, m, y = c * scale, m * scale, y * scale
    out = np.stack([c, m, y, k], axis=2)
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "CMYK")


def from_cmyk(image: Image.Image, icc: str | Path | None = None, intent: str = "relative") -> Image.Image:
    if icc:
        from PIL import ImageCms

        transform = ImageCms.buildTransform(_profile(icc), ImageCms.createProfile("sRGB"), "CMYK", "RGB",
                                            renderingIntent=INTENTS.index(intent))
        return ImageCms.applyTransform(image, transform)
    return image.convert("RGB")


def proof(image: Image.Image, icc: str | Path | None = None) -> Image.Image:
    """How the picture will print in CMYK, on screen (色校正): colours out of the printer's reach come back
    duller. RGBA keeps its transparency."""
    alpha = image.getchannel("A") if image.mode == "RGBA" else None
    out = from_cmyk(to_cmyk(image, icc), icc)
    if alpha is not None:
        out = out.convert("RGBA")
        out.putalpha(alpha)
    return out


def ink_coverage(image: Image.Image) -> float:
    """The most ink anywhere on a CMYK picture, in % (for checking against the printer's limit)."""
    import numpy as np

    return float(np.asarray(image.convert("CMYK"), dtype=np.uint16).sum(axis=2).max()) / 255 * 100
