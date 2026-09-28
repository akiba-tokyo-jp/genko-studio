"""An 8-page story for the offline pipeline: the 4-page demo told twice (pages 5-8 repeat 1-4)."""

import copy
import json
from pathlib import Path

DEMO = Path(__file__).resolve().parents[1] / "fixtures" / "studio" / "demo4"


def _load(name: str) -> dict:
    return json.loads((DEMO / name).read_text(encoding="utf-8"))


def bible() -> dict:
    return _load("bible.json")


def script(pages: int = 8) -> dict:
    base = _load("script.json")
    if pages == 4:
        return base
    second = copy.deepcopy(base)
    for scene in second["scenes"]:
        scene["id"] += "_b"
        for beat in scene["beats"]:
            beat["id"] += "_b"
            beat["page"] += 4
    return {"scenes": base["scenes"] + second["scenes"]}


def plan(page: int) -> dict:
    if page <= 4:
        return _load(f"p00{page}.json")
    out = copy.deepcopy(_load(f"p00{page - 4}.json"))
    out["page"] = page
    for panel in out["panels"]:
        panel["beat_ids"] = [b + "_b" for b in panel.get("beat_ids", [])]
        for line in panel.get("lines", []):
            line["beat_id"] += "_b"
    return out


def broken(page: int) -> dict:
    out = plan(page)
    out["tiers"][0]["h"] = 0.9  # tier heights no longer sum to 1
    return out
