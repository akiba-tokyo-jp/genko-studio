"""Read PSD files with psd-tools and print what their layers are (PSD-01's structure check, ACCEPTANCE.md §6).

    python psd_text_check.py out.json a.psd [b.psd …]
    python psd_text_check.py python-book OPS BOOK      (the PSD book made by Python's genko)
    python psd_text_check.py python-psd BOOK OUT DPI   (Python's PSD of it: the image way)

For each file, {"width", "height", "layers": {"<folder>/<name>": {...}}, "order": {"<folder>": [names bottom to top]}}:
each layer's kind (pixel, type, group) and, for a type layer, its text as psd-tools reads it (the descriptor's "Txt "
and the EngineData's text), orientation, transform, kind of text (point or area, its box), warp, the style runs (length,
font name, size, bold, italic, fill colour, tracking, leading, baseline direction, horizontal and vertical scale,
alignment of the run) and the paragraph runs (length, justification); and a stroke among its effects. The C++ test
(native/tests/contract/test_contract_psd_text.cpp) makes the same view with Genko's own reader and compares them.
Development only: the product never runs Python or psd-tools.
"""

from __future__ import annotations

import json
import sys

from psd_tools import PSDImage


def _value(element):
    return element.value if hasattr(element, "value") else element


def _text_of(value) -> str:
    # (psd-tools leaves a character past the BMP as its two UTF-16 halves in layer names: joined again)
    return str(value).encode("utf-16-be", "surrogatepass").decode("utf-16-be")


def _number(element) -> float:
    return float(_value(element))


def _enum(value) -> str:
    raw = getattr(value, "enum", value)
    raw = getattr(raw, "value", raw)
    if isinstance(raw, bytes):
        return raw.decode("latin-1")
    return str(raw)


def _colour(values) -> list[int]:
    # (FillColor: Type 1, Values [alpha r g b] as 0..1)
    return [round(_number(v) * 255) for v in list(values)[1:4]]


def _style(data, fonts) -> dict:
    def get(key, default):
        return data[key] if key in data else default

    return {
        "font": fonts[int(_value(get("Font", 0)))],
        "size": _number(get("FontSize", 0)),
        "faux_bold": bool(_value(get("FauxBold", False))),
        "faux_italic": bool(_value(get("FauxItalic", False))),
        "fill": _colour(data["FillColor"]["Values"]) if "FillColor" in data else [0, 0, 0],
        "tracking": _number(get("Tracking", 0)),
        "leading": _number(get("Leading", 0)),
        "baseline_direction": int(_value(get("BaselineDirection", 2))),
        "horizontal_scale": _number(get("HorizontalScale", 1)),
        "vertical_scale": _number(get("VerticalScale", 1)),
        "run_alignment": int(_value(get("StyleRunAlignment", 2))),
    }


def _text(layer) -> dict:
    engine = layer.engine_dict
    resources = layer.resource_dict
    fonts = [_text_of(_value(font["Name"])) for font in resources["FontSet"]]
    style_run = engine["StyleRun"]
    runs = [
        {"length": int(_value(length)), "style": _style(run["StyleSheet"]["StyleSheetData"], fonts)}
        for length, run in zip(style_run["RunLengthArray"], style_run["RunArray"])
    ]
    paragraph_run = engine["ParagraphRun"]
    paragraphs = [
        {"length": int(_value(length)), "justification": int(_value(run["ParagraphSheet"]["Properties"].get("Justification", 0)))}
        for length, run in zip(paragraph_run["RunLengthArray"], paragraph_run["RunArray"])
    ]
    shapes = engine["Rendered"]["Shapes"]
    photoshop = shapes["Children"][0]["Cookie"]["Photoshop"]
    data = layer._data.text_data
    out = {
        "text": _text_of(layer.text),
        "engine_text": _text_of(_value(engine["Editor"]["Text"])),
        "orientation": _enum(data[b"Ornt"]),
        "writing_direction": int(_value(shapes["WritingDirection"])),
        "transform": [float(v) for v in layer.transform],
        "shape": "point" if int(_value(photoshop["ShapeType"])) == 0 else "box",
        "warp": _enum(layer.warp[b"warpStyle"]),
        "runs": runs,
        "paragraphs": paragraphs,
    }
    if out["shape"] == "box":
        out["box"] = [_number(v) for v in photoshop["BoxBounds"]]
    return out


def _stroke(layer):
    for effect in layer.effects:
        if type(effect).__name__ == "Stroke":
            colour = effect.color
            return {
                "size": float(effect.size),
                "rgb": [round(float(colour[key])) for key in (b"Rd  ", b"Grn ", b"Bl  ")],
                "position": _enum(effect.position),
                "opacity": float(effect.opacity),
                "enabled": bool(effect.enabled),
            }
    return None


def view(path: str) -> dict:
    psd = PSDImage.open(path)
    layers: dict = {}
    order: dict = {}

    def walk(group, folder: str) -> None:
        names = []
        for layer in group:  # (bottom to top)
            name = _text_of(layer.name)
            names.append(name)
            entry = {"kind": layer.kind}
            if layer.kind == "type":
                entry["text"] = _text(layer)
                stroke = _stroke(layer)
                if stroke is not None:
                    entry["stroke"] = stroke
            layers[folder + "/" + name] = entry
            if layer.is_group():
                walk(layer, name)
        order[folder] = names

    walk(psd, "")
    return {"width": psd.width, "height": psd.height, "layers": layers, "order": order}


def _graphemes(text: str) -> list[str]:
    # (a letter with its variation selectors, as the book's ruby ranges count them)
    out: list[str] = []
    for ch in text:
        if out and (0xFE00 <= ord(ch) <= 0xFE0F or 0xE0100 <= ord(ch) <= 0xE01EF):
            out[-1] += ch
        else:
            out.append(ch)
    return out


def python_book(ops_path: str, dest: str) -> None:
    """The PSD book as Python's genko makes it (four A4 pages, the ops applied; a ruby by place given as its ruby_runs:
    Python's book has no rubies by place), saved."""
    from pathlib import Path

    from genko.headless import apply_ops
    from genko.io import save_episode
    from genko.models import PageSpec, new_episode

    with open(ops_path, encoding="utf-8") as f:
        ops = json.load(f)
    for op in ops:
        spans = op.pop("ruby_spans", None)
        if spans:
            letters = _graphemes(op["text"])
            op["ruby_runs"] = [["".join(letters[s["start"]:s["end"]]), s["ruby"]] for s in spans]
    episode = new_episode("PSD文字", 1, 4, PageSpec.a4_mono())
    apply_ops(episode, ops, agent="human:作者")
    save_episode(episode, Path(dest))


def python_psd(book: str, out: str, dpi: int) -> None:
    """Python's PSD of every page of a book (psd.export_psd_pages), its text laid out as the measured reference's:
    Pillow's BASIC layout, without raqm, as render_harness and pyref_harness hold it (the C++ drawing is BASIC's)."""
    from pathlib import Path

    from PIL import ImageFont

    ImageFont.core.HAVE_RAQM = False

    from genko import psd
    from genko.io import load_episode

    psd.export_psd_pages(load_episode(Path(book)), Path(out), dpi)


def main(argv: list[str]) -> int:
    if len(argv) > 1 and argv[1] == "python-book":
        python_book(argv[2], argv[3])
        return 0
    if len(argv) > 1 and argv[1] == "python-psd":
        python_psd(argv[2], argv[3], int(argv[4]))
        return 0
    out, paths = argv[1], argv[2:]
    with open(out, "w", encoding="utf-8") as f:
        json.dump({path: view(path) for path in paths}, f, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
