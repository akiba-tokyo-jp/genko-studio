from __future__ import annotations

import json
from pathlib import Path

_CATALOG: list[dict] | None = None


def load_catalog() -> list[dict]:
    global _CATALOG
    if _CATALOG is None:
        path = Path(__file__).with_name("catalog.json")
        _CATALOG = json.loads(path.read_text(encoding="utf-8"))
    return _CATALOG


def get_material(material_id: str) -> dict:
    for item in load_catalog():
        if item["id"] == material_id:
            return item
    raise KeyError(material_id)
