"""オートアクション: record what was done (the ops) under a name, and do it again on another page.

The recording keeps the ops as they were applied. Played back, they go to the page on screen and the
layer being drawn on: "page" becomes the current page, "layer_id" the current layer, a panel the chosen
panel (or none), and things the recording made (a layer, a tone, a line…) get new ids each time.
Actions are kept in the settings folder (actions.json), so every book can use them.
"""

from __future__ import annotations

import copy
import json
import time
from pathlib import Path

# ops that make something with an "id" of its own
MAKERS = ("add_layer", "add_tone", "add_line", "add_effect", "add_prim3d", "add_scene", "add_mannequin", "add_ruler",
          "stamp_material", "duplicate_layer")
# ops whose "id" names the layer they change: played back, the layer being drawn on
LAYER_OPS = ("set_layer", "set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer", "filter_raster")
NOT_RECORDED = ("undo", "lock_page", "unlock_page", "name_ok", "advance", "set_meta", "set_bible", "add_page",
                "delete_page", "duplicate_page", "reorder", "set_page_spec")


def path() -> Path:
    from genko.tokens import config_dir

    return config_dir() / "actions.json"


def load() -> dict:
    try:
        data = json.loads(path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {str(k): v for k, v in data.items() if isinstance(v, dict) and isinstance(v.get("ops"), list)}


def save(actions: dict) -> None:
    target = path()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(actions, ensure_ascii=False, indent=1), encoding="utf-8")


def store(name: str, ops: list[dict]) -> None:
    actions = load()
    actions[name] = {"ops": ops, "made": time.strftime("%Y-%m-%d %H:%M")}
    save(actions)


def remove(name: str) -> None:
    actions = load()
    actions.pop(name, None)
    save(actions)


def recordable(ops: list[dict]) -> list[dict]:
    return [copy.deepcopy(op) for op in ops if isinstance(op, dict) and op.get("op") not in NOT_RECORDED]


def replay(ops: list[dict], page: int, layer_id: str | None, frame_id: str | None) -> list[dict]:
    """The recorded ops, aimed at this page, layer and panel, with fresh ids for what they make."""
    from genko.models import new_id

    made: dict[str, str] = {}
    for op in ops:
        if op.get("op") in MAKERS and op.get("id") and op.get("op") != "duplicate_layer":
            made.setdefault(str(op["id"]), new_id())
        if op.get("op") == "duplicate_layer" and op.get("new_id"):
            made.setdefault(str(op["new_id"]), new_id())
    out = []
    for op in ops:
        op = copy.deepcopy(op)
        if "page" in op:
            op["page"] = page
        for key in ("id", "new_id", "after", "parent", "parent_id", "clip_to"):
            if isinstance(op.get(key), str) and op[key] in made:
                op[key] = made[op[key]]
            elif key == "id" and op.get("op") in LAYER_OPS and op.get("id") and layer_id:
                op["id"] = layer_id  # (the layer it changed then: the one drawn on now)
        if "layer_id" in op:
            recorded = str(op["layer_id"])
            op["layer_id"] = made.get(recorded) or layer_id or recorded
        if "frame_id" in op:
            if frame_id:
                op["frame_id"] = frame_id
            else:
                op.pop("frame_id")
        out.append(op)
    return out


def describe(ops: list[dict]) -> str:
    from genko.app.history import describe as said

    return said(ops)
