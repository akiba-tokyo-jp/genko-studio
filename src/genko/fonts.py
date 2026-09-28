"""Typefaces for lettering: the bundled ones (SIL OFL, see fonts/licenses/) and the computer's own.

A face gives the font for each character, so a composite face can mix two fonts: アンチック
(the usual manga dialogue face) sets kana and punctuation in a Mincho and kanji, digits and Latin in
a Gothic. A line's `style.font` is a bundled key ("antique", "gothic", …) or a font file path.
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from PIL import ImageFont

DIR = Path(__file__).resolve().parent / "fonts"

GOTHIC = DIR / "ZenKakuGothicNew-Bold.ttf"
MINCHO = DIR / "ZenOldMincho-SemiBold.ttf"
MARU = DIR / "ZenMaruGothic-Bold.ttf"
HAND = DIR / "Yomogi-Regular.ttf"
SFX = DIR / "DelaGothicOne-Regular.ttf"
SFX_POP = DIR / "ReggaeOne-Regular.ttf"

# key: (label, kana/punctuation font, everything else)
BUNDLED: dict[str, tuple[str, Path, Path]] = {
    "antique": ("アンチック（かなは明朝・漢字はゴシック）", MINCHO, GOTHIC),
    "gothic": ("ゴシック", GOTHIC, GOTHIC),
    "mincho": ("明朝", MINCHO, MINCHO),
    "maru": ("丸ゴシック", MARU, MARU),
    "hand": ("手書き風", HAND, HAND),
    "sfx": ("極太（効果音）", SFX, SFX),
    "sfx_pop": ("勢い（効果音）", SFX_POP, SFX_POP),
}
DEFAULT_DIALOGUE = "antique"
LOOKALIKE = {"―": "—", "～": "〜", "〜": "～", "−": "－", "‐": "-", "∼": "〜"}
DEFAULT_SFX = "sfx"


def is_kana_like(char: str) -> bool:
    """Characters an アンチック sets in Mincho: kana, the long vowel mark and Japanese punctuation."""
    code = ord(char[:1] or " ")
    return (0x3040 <= code <= 0x30FF or 0x31F0 <= code <= 0x31FF or 0x3000 <= code <= 0x303F
            or 0xFF61 <= code <= 0xFF9F or char in "ー〜～…‥")


@lru_cache(maxsize=256)
def truetype(path: str, size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    try:
        return ImageFont.truetype(path, max(6, int(size)))
    except (OSError, ValueError):
        return ImageFont.load_default()


@lru_cache(maxsize=8192)
def has_glyph(path: str, char: str) -> bool:
    font = truetype(path, 32)
    try:
        box = font.getmask(char).getbbox()
        if box is None:
            return char.isspace()
        missing = font.getmask("\uffff").getbbox()  # the font's "no glyph" box
        return box != missing or font.getmask(char).tobytes() != font.getmask("\uffff").tobytes()
    except Exception:
        return False


@dataclass(frozen=True)
class Face:
    key: str
    label: str
    kana: str
    other: str

    def font(self, size: int, char: str = "漢") -> ImageFont.ImageFont:
        first = self.kana if is_kana_like(char) else self.other
        if len(char) != 1 or has_glyph(first, char):
            return truetype(first, size)
        # a character the face lacks (e.g. ― in some Minchos): the other half, then the bundled Gothic
        for path in (self.other if first == self.kana else self.kana, str(GOTHIC), str(SFX)):
            if has_glyph(path, char):
                return truetype(path, size)
        return truetype(first, size)

    def normalize(self, text: str) -> str:
        """Swap characters the face lacks for their look-alikes (― → —, ～ → 〜 …)."""
        out = []
        for char in text or "":
            alt = LOOKALIKE.get(char)
            if alt and not any(has_glyph(p, char) for p in (self.kana, self.other)) and has_glyph(self.other, alt):
                char = alt
            out.append(char)
        return "".join(out)

    @property
    def composite(self) -> bool:
        return self.kana != self.other


def face(spec: str | None, default: str = DEFAULT_DIALOGUE) -> Face:
    """A bundled key, a font file path, or None (the default)."""
    spec = spec or default
    if spec in BUNDLED:
        label, kana, other = BUNDLED[spec]
        return Face(spec, label, str(kana), str(other))
    path = Path(spec)
    if path.is_file():
        return Face(str(path), path.stem, str(path), str(path))
    label, kana, other = BUNDLED[default if default in BUNDLED else DEFAULT_DIALOGUE]
    return Face(default, label, str(kana), str(other))


# --- the computer's fonts --------------------------------------------------------------------------


def _font_dirs() -> list[Path]:
    if sys.platform.startswith("win"):
        windir = Path(os.environ.get("WINDIR", r"C:\Windows"))
        dirs = [windir / "Fonts", Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "Windows" / "Fonts"]
    elif sys.platform == "darwin":
        dirs = [Path("/System/Library/Fonts"), Path("/Library/Fonts"), Path.home() / "Library" / "Fonts"]
    else:
        dirs = [Path("/usr/share/fonts"), Path("/usr/local/share/fonts"), Path.home() / ".fonts",
                Path.home() / ".local" / "share" / "fonts"]
    return [d for d in dirs if d.is_dir()]


def system_fonts(refresh: bool = False) -> list[dict]:
    """[{name, style, path}] of the fonts that can draw Japanese, cached in the config dir."""
    from genko.tokens import config_dir

    cache = config_dir() / "fonts.json"
    if not refresh:
        try:
            return json.loads(cache.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
    found: list[dict] = []
    for folder in _font_dirs():
        for path in sorted(folder.rglob("*")):
            if path.suffix.lower() not in (".ttf", ".otf", ".ttc"):
                continue
            try:
                font = ImageFont.truetype(str(path), 24)
                if font.getmask("あ漢").getbbox() is None:
                    continue
                name, style = font.getname()
            except Exception:
                continue
            found.append({"name": name, "style": style, "path": str(path)})
    try:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(found, ensure_ascii=False, indent=1), encoding="utf-8")
    except OSError:
        pass
    return found
