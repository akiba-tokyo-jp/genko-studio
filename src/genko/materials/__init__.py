"""Materials (素材): tones, effect lines, pictures and drawn parts ready to put on a page.

Built-in ones ship with Genko (catalog.json). A person's own library lives in the config dir
(`materials/library.json` and its pictures), never in a project, so it serves every book; putting a
material on a page copies it into the book (`stamp_material`).

Entry: {"id", "name", "folder", "kind": tone | effect | image | lines, …} with
- tone: "tone": {pattern, lpi, density, angle, gradient}
- effect: "effect" (a kind of genko.effects), "params"
- image: "file" (a PNG next to library.json), "width_mm"
- lines: "items" (copied pen lines and fills, as the clipboard keeps them)
- lettering (描き文字): "text", "balloon" (sfx by default), "wrap", "style" (a line's style), "w_mm", "h_mm" — put
  on a page as a line set that way
Old catalog kinds "dot" / "noise" are tones.
"""

from __future__ import annotations

import io
import json
from pathlib import Path

_CATALOG: list[dict] | None = None
KINDS = ("tone", "effect", "image", "lines", "lettering")


def load_catalog() -> list[dict]:
    global _CATALOG
    if _CATALOG is None:
        path = Path(__file__).with_name("catalog.json")
        _CATALOG = [normalize({**item, "builtin": True}) for item in json.loads(path.read_text(encoding="utf-8"))]
    return _CATALOG


def normalize(item: dict) -> dict:
    """Old catalog kinds as tones."""
    if item.get("kind") in ("dot", "noise"):
        tone = {"pattern": "noise" if item["kind"] == "noise" else "dot", "density": item.get("density", 0.3)}
        if item.get("lpi"):
            tone["lpi"] = item["lpi"]
        return {**item, "kind": "tone", "tone": tone}
    if item.get("kind") == "effect":
        return {"params": {}, **item, "effect": item.get("effect", "speed")}
    return item


def library_dir() -> Path:
    from genko.tokens import config_dir

    return config_dir() / "materials"


def _library_file() -> Path:
    return library_dir() / "library.json"


def user_materials() -> list[dict]:
    path = _library_file()
    if not path.exists():
        return []
    try:
        return [normalize(item) for item in json.loads(path.read_text(encoding="utf-8"))]
    except (OSError, ValueError):
        return []


def _save(items: list[dict]) -> None:
    library_dir().mkdir(parents=True, exist_ok=True)
    tmp = _library_file().with_suffix(".tmp")
    tmp.write_text(json.dumps(items, ensure_ascii=False, indent=1), encoding="utf-8")
    tmp.replace(_library_file())


def all_materials() -> list[dict]:
    return load_catalog() + user_materials()


def folders() -> list[str]:
    seen: list[str] = []
    for item in all_materials():
        folder = item.get("folder") or "その他"
        if folder not in seen:
            seen.append(folder)
    for extra in _empty_folders():
        if extra not in seen:
            seen.append(extra)
    return seen


def _empty_folders() -> list[str]:
    path = library_dir() / "folders.json"
    try:
        return list(json.loads(path.read_text(encoding="utf-8"))) if path.exists() else []
    except (OSError, ValueError):
        return []


def add_folder(name: str) -> None:
    name = name.strip()
    if not name:
        raise ValueError("a folder needs a name")
    names = _empty_folders()
    if name not in names and name not in folders():
        library_dir().mkdir(parents=True, exist_ok=True)
        (library_dir() / "folders.json").write_text(json.dumps(names + [name], ensure_ascii=False), encoding="utf-8")


def get_material(material_id: str) -> dict:
    for item in all_materials():
        if item["id"] == material_id:
            return item
    raise KeyError(material_id)


def _new_id(name: str) -> str:
    from genko.models import new_id

    return "u-" + new_id()


def add_material(name: str, kind: str, folder: str = "マイ素材", **data) -> dict:
    """Register a material in the person's library; returns it."""
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {', '.join(KINDS)}")
    if not name.strip():
        raise ValueError("a material needs a name")
    item = {"id": _new_id(name), "name": name.strip(), "folder": folder.strip() or "マイ素材", "kind": kind, **data}
    _save([*[i for i in user_materials()], item])
    return item


def import_image(path: Path, name: str | None = None, folder: str = "画像", width_mm: float | None = None) -> dict:
    """Copy a picture into the library (as PNG) and register it."""
    from PIL import Image

    source = Path(path)
    with Image.open(source) as image:
        image.load()
        rgba = image.convert("RGBA")
    library_dir().mkdir(parents=True, exist_ok=True)
    item = add_material(name or source.stem, "image", folder, width_mm=float(width_mm or 60.0))
    target = library_dir() / f"{item['id']}.png"
    rgba.save(target, format="PNG")
    items = user_materials()
    for entry in items:
        if entry["id"] == item["id"]:
            entry["file"] = target.name
            entry["aspect"] = round(rgba.height / max(1, rgba.width), 5)
    _save(items)
    return get_material(item["id"])


def update_material(material_id: str, **change) -> dict:
    items = user_materials()
    for entry in items:
        if entry["id"] == material_id:
            entry.update({k: v for k, v in change.items() if k in ("name", "folder")})
            _save(items)
            return entry
    raise KeyError(material_id)


def delete_material(material_id: str) -> None:
    items = user_materials()
    kept = [i for i in items if i["id"] != material_id]
    if len(kept) == len(items):
        if any(i["id"] == material_id for i in load_catalog()):
            raise ValueError("built-in materials cannot be deleted")
        raise KeyError(material_id)
    gone = next(i for i in items if i["id"] == material_id)
    if gone.get("file"):
        (library_dir() / gone["file"]).unlink(missing_ok=True)
    _save(kept)


def image_bytes(item: dict) -> bytes | None:
    if item.get("kind") != "image" or not item.get("file"):
        return None
    path = library_dir() / item["file"]
    return path.read_bytes() if path.exists() else None


def thumbnail(item: dict, size: int = 72):
    """A small picture of the material (PIL RGB)."""
    from PIL import Image, ImageDraw

    kind = item.get("kind")
    if kind == "tone":
        from genko.tones import swatch

        return swatch(item.get("tone") or {}, (size, size), dpi=110)
    if kind == "effect":
        from genko import effects
        from genko.models import PageSpec, new_episode

        page = new_episode("t", 1, 1, PageSpec(width_mm=60, height_mm=60, dpi=150, bleed_mm=0, inner_margin_mm=0)).pages[0]
        effect = {"id": item["id"], "kind": item.get("effect"), "params": item.get("params") or {}}
        big = size * 4  # drawn large and made small, so thin lines stay lines
        base = Image.new("RGB", (big, big), "white")
        return effects.draw(base, effect, page, round(big / 60 * 25.4)).resize((size, size), Image.Resampling.LANCZOS)
    if kind == "image":
        data = image_bytes(item)
        if data:
            image = Image.open(io.BytesIO(data)).convert("RGBA")
            image.thumbnail((size, size))
            base = Image.new("RGB", (size, size), "white")
            base.paste(image, ((size - image.width) // 2, (size - image.height) // 2), image)
            return base
    base = Image.new("RGB", (size, size), "white")
    if kind == "lettering":
        from genko.balloons import text_image
        from genko.models import StoryLine

        line = StoryLine(id="t", page_index=1, text=item.get("text") or "ド", balloon=item.get("balloon") or "sfx",
                         w_mm=float(item.get("w_mm") or 40), h_mm=float(item.get("h_mm") or 30), wrap=item.get("wrap") or "horizontal")
        line.style = dict(item.get("style") or {})
        picture, _em = text_image(line, 120)
        picture.thumbnail((size - 4, size - 4))
        base.paste(picture, ((size - picture.width) // 2, (size - picture.height) // 2), picture)
        return base
    if kind == "lines":
        from genko.selection import items_from_json

        items = items_from_json(item.get("items") or {})
        pts = [p for s in items["strokes"] for p in s.points]
        if pts:
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            x0, y0 = min(xs), min(ys)
            k = (size - 8) / max(1e-6, max(max(xs) - x0, max(ys) - y0))
            draw = ImageDraw.Draw(base)
            for s in items["strokes"]:
                draw.line([(4 + (p[0] - x0) * k, 4 + (p[1] - y0) * k) for p in s.points], fill=(20, 20, 20), width=1)
    return base


