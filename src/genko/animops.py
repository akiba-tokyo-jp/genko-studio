"""J12 ops for animation (see genko.anim): the timeline, animation folders and cels, the exposure sheet, the
camera and the light table."""

from __future__ import annotations

from typing import Any

from genko import anim
from genko.models import Layer, LayerKind, LayerRole, new_id
from genko.ops import ApplyError

OPS = ("set_animation", "add_anim_folder", "add_cel", "set_exposure", "set_exposures", "set_camera_key", "set_light_table")


def _anim(page, create: bool = False) -> dict:
    data = anim.spec(page)
    if data is None:
        if not create:
            raise ApplyError(f"page {page.index} is not an animation (set_animation first)")
        data = {"fps": 12, "frames": 24, "loop": True, "tracks": []}
        page.extra["anim"] = data
    return data


def _frame(page, value, what: str = "frame") -> int:
    try:
        frame = int(value)
    except (TypeError, ValueError) as exc:
        raise ApplyError(f"{what} is a frame number") from exc
    if not 1 <= frame <= anim.frames_of(page):
        raise ApplyError(f"{what} is 1 to {anim.frames_of(page)}")
    return frame


def _folder(page, folder_id) -> Layer:
    folder = next((layer for layer in page.layers if layer.id == str(folder_id or "")), None)
    if folder is None or folder.kind != LayerKind.FOLDER or anim.track(page, folder.id) is None:
        raise ApplyError("folder is an animation folder's id (add_anim_folder)")
    return folder


def _cel(page, folder: Layer, cel_id) -> str | None:
    if cel_id is None:
        return None
    if str(cel_id) not in {layer.id for layer in anim.cels_of(page, folder.id)}:
        raise ApplyError("cel must be a layer in the animation folder")
    return str(cel_id)


def set_animation(episode, page, op: dict) -> None:
    """Make the page an animation (or change its speed and length); off: an ordinary page again (the cels stay
    as layers)."""
    if op.get("off"):
        page.extra.pop("anim", None)
        return
    data = _anim(page, create=True)
    if "fps" in op:
        fps = float(op["fps"])
        if not 1 <= fps <= 60:
            raise ApplyError("fps is 1 to 60")
        data["fps"] = fps
    if "frames" in op:
        frames = int(op["frames"])
        if not 1 <= frames <= anim.MAX_FRAMES:
            raise ApplyError(f"frames is 1 to {anim.MAX_FRAMES}")
        data["frames"] = frames
        for entry in data.get("tracks", []):  # (exposures past the end go)
            entry["cels"] = [item for item in entry.get("cels", []) if item[0] <= frames]
        data["camera"] = [key for key in data.get("camera") or [] if key["frame"] <= frames]
    if "loop" in op:
        data["loop"] = bool(op["loop"])


def add_anim_folder(episode, page, op: dict) -> None:
    data = _anim(page, create=True)
    folder = Layer(id=str(op.get("id") or new_id()), role=LayerRole.USER, kind=LayerKind.FOLDER,
                   title=str(op.get("name") or f"アニメーション {len(data['tracks']) + 1}"), exportable=True)
    if any(layer.id == folder.id for layer in page.layers):
        raise ApplyError(f"layer {folder.id} exists")
    page.layers.append(folder)
    data["tracks"].append({"folder": folder.id, "cels": []})


def add_cel(episode, page, op: dict) -> None:
    """A new cel (a pen or paint layer) in an animation folder, shown from frame `at` (the first cel of a folder
    shows from frame 1)."""
    folder = _folder(page, op.get("folder"))
    kind = str(op.get("kind") or "pen")
    if kind not in ("pen", "paint"):
        raise ApplyError("kind must be pen or paint")
    cels = anim.cels_of(page, folder.id)
    cel = Layer(id=str(op.get("id") or new_id()), role=LayerRole.USER,
                kind=LayerKind.STROKES if kind == "pen" else LayerKind.RASTER,
                title=str(op.get("name") or str(len(cels) + 1)), parent_id=folder.id, exportable=True)
    if any(layer.id == cel.id for layer in page.layers):
        raise ApplyError(f"layer {cel.id} exists")
    at = page.layers.index(folder)  # (a folder's layers sit just under it)
    page.layers.insert(at, cel)
    entry = anim.track(page, folder.id)
    if "at" in op:
        _expose(entry, _frame(page, op["at"], "at"), cel.id)
    elif not entry["cels"]:
        entry["cels"] = [[1, cel.id]]


def _expose(entry: dict, frame: int, cel: str | None) -> None:
    cels = [item for item in entry.get("cels", []) if item[0] != frame]
    cels.append([frame, cel])
    entry["cels"] = sorted(cels, key=lambda item: item[0])


def set_exposure(episode, page, op: dict) -> None:
    """From `frame` on, this cel shows (cel null: nothing); clear: the frame goes back to what was before it."""
    folder = _folder(page, op.get("folder"))
    frame = _frame(page, op.get("frame"))
    entry = anim.track(page, folder.id)
    if op.get("clear"):
        entry["cels"] = [item for item in entry["cels"] if item[0] != frame]
        return
    _expose(entry, frame, _cel(page, folder, op.get("cel")))


def set_exposures(episode, page, op: dict) -> None:
    """The whole exposure sheet of a folder: [[frame, cel | null], …]."""
    folder = _folder(page, op.get("folder"))
    items = op.get("cels")
    if not isinstance(items, list):
        raise ApplyError("cels is a list of [frame, cel id or null]")
    sheet: dict[int, str | None] = {}
    for item in items:
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            raise ApplyError("cels is a list of [frame, cel id or null]")
        sheet[_frame(page, item[0])] = _cel(page, folder, item[1])
    anim.track(page, folder.id)["cels"] = [[frame, cel] for frame, cel in sorted(sheet.items())]


def set_camera_key(episode, page, op: dict) -> None:
    """カメラワーク: where the camera looks at a frame (rect mm: x, y, width, height); null removes the key."""
    data = _anim(page)
    frame = _frame(page, op.get("frame"))
    keys = [key for key in data.get("camera") or [] if key["frame"] != frame]
    rect = op.get("rect")
    if rect is not None:
        try:
            x, y, w, h = (float(v) for v in rect)
        except (TypeError, ValueError) as exc:
            raise ApplyError("rect is [x, y, width, height] in mm") from exc
        if w <= 1 or h <= 1:
            raise ApplyError("rect is [x, y, width, height] in mm")
        keys.append({"frame": frame, "rect": [x, y, w, h]})
    data["camera"] = sorted(keys, key=lambda key: key["frame"])


def set_light_table(episode, page, op: dict) -> None:
    """ライトテーブル: cels always shown faint while drawing (never in the picture)."""
    data = _anim(page)
    cels = [str(c) for c in op.get("cels") or []]
    known = {layer.id for layer in page.layers}
    missing = [c for c in cels if c not in known]
    if missing:
        raise ApplyError(f"no layer {missing[0]}")
    data["light_table"] = cels


def apply(episode, op: dict[str, Any], name: str) -> None:
    from genko.ops import _require_page

    page = _require_page(episode, op)
    {"set_animation": set_animation, "add_anim_folder": add_anim_folder, "add_cel": add_cel, "set_exposure": set_exposure,
     "set_exposures": set_exposures, "set_camera_key": set_camera_key, "set_light_table": set_light_table}[name](episode, page, op)
