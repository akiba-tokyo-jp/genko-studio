"""Filter plugins (プラグイン): Python files a person puts in <config>/plugins/ show up as filters on paint layers,
in the app and for agents (filter_raster with filter "plugin:<file name>"). A plugin is a file such as

    NAME = "セピア"                                  # the name in the filter list
    PARAMS = {"amount": {"label": "強さ", "min": 0, "max": 1, "default": 0.8}}     # (optional)

    def run(image, amount=0.8):                     # a PIL RGBA image of the layer -> the new picture
        ...
        return image

The picture comes back the same size; its transparency is kept when the plugin returns RGB. Plugins are code on
this computer that the person chose to install, run as it is (like the plug-ins of other painting apps).
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

PREFIX = "plugin:"


def folder() -> Path:
    from genko.tokens import config_dir

    return config_dir() / "plugins"


def _load(path: Path):
    spec = importlib.util.spec_from_file_location(f"genko_plugin_{path.stem}", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"plugin {path.stem} cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # (a broken plugin must not take the app down)
        raise ValueError(f"plugin {path.stem} cannot be loaded ({exc})") from exc
    if not callable(getattr(module, "run", None)):
        raise ValueError(f"plugin {path.stem} has no run(image)")
    return module


def available() -> list[dict]:
    """The plugins installed: [{key, name, params}] (broken ones are left out)."""
    out = []
    base = folder()
    if not base.is_dir():
        return out
    for path in sorted(base.glob("*.py")):
        if path.name.startswith("_"):
            continue
        try:
            module = _load(path)
        except ValueError:
            continue
        params = getattr(module, "PARAMS", {}) or {}
        out.append({"key": path.stem, "name": str(getattr(module, "NAME", path.stem)), "params": params if isinstance(params, dict) else {}})
    return out


def fields(kind: str) -> list[tuple]:
    """A plugin's settings as the filter dialog asks them: [(key, label, min, max, default)]."""
    key = kind[len(PREFIX):] if kind.startswith(PREFIX) else kind
    found = next((p for p in available() if p["key"] == key), None)
    if found is None:
        return []
    out = []
    for name, spec in found["params"].items():
        spec = spec if isinstance(spec, dict) else {}
        out.append((name, str(spec.get("label", name)), float(spec.get("min", 0)), float(spec.get("max", 100)),
                    float(spec.get("default", spec.get("min", 0)))))
    return out


def run(kind: str, image, params: dict | None = None):
    """The layer's picture through the plugin."""
    from PIL import Image

    key = kind[len(PREFIX):] if kind.startswith(PREFIX) else kind
    path = folder() / f"{key}.py"
    if not key or "/" in key or "\\" in key or not path.is_file():
        raise ValueError(f"no plugin {key}")
    module = _load(path)
    rgba = image.convert("RGBA")
    try:
        out = module.run(rgba.copy(), **(params or {}))
    except Exception as exc:
        raise ValueError(f"plugin {key} failed ({exc})") from exc
    if not isinstance(out, Image.Image):
        raise ValueError(f"plugin {key} did not return a picture")
    if out.size != rgba.size:
        out = out.resize(rgba.size)
    if out.mode != "RGBA":
        alpha = rgba.getchannel("A")
        out = out.convert("RGBA")
        out.putalpha(alpha)
    return out
