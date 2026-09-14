from __future__ import annotations

import base64
import copy
from pathlib import Path
from typing import Any

from genko.models import (
    Binding,
    Episode,
    Frame,
    Layer,
    LayerKind,
    LayerRole,
    Page,
    PageSpec,
    Rect,
    StoryLine,
    new_id,
)
from genko.pipeline import InkBlockedError, advance


class ApplyError(ValueError):
    """Invalid or blocked operation."""


OPS_SCHEMA: list[dict[str, Any]] = [
    {"op": "split_frame", "page": "int", "axis": "horizontal|vertical", "ratio": "float", "gutter_mm": "float", "frame_id": "optional"},
    {"op": "merge_frame", "page": "int", "frame_id": "str"},
    {"op": "resize_frame", "page": "int", "frame_id": "str", "rect": "{x,y,width,height}"},
    {"op": "set_frame", "page": "int", "frame_id": "str", "bleed": "bool?", "clip": "bool?", "border_mm": "float?"},
    {"op": "add_line", "page": "int", "text": "str", "speaker": "optional", "frame_id": "optional", "balloon": "optional", "x_mm": "optional"},
    {"op": "edit_line", "id": "str", "text": "optional", "speaker": "optional"},
    {"op": "delete_line", "id": "str"},
    {"op": "move_line", "id": "str", "x_mm": "float?", "y_mm": "float?", "w_mm": "float?", "h_mm": "float?", "tail": "[x,y]?"},
    {"op": "name_ok", "page": "int, optional (all pages if omitted)"},
    {"op": "advance", "page": "int", "to": "name|ink|finish"},
    {"op": "add_stroke", "page": "int", "layer": "name|ink", "points": "[[x,y],...]"},
    {"op": "delete_stroke", "page": "int", "layer": "name|ink", "index": "int"},
    {"op": "put_raster", "page": "int", "layer": "name|draft|ink|bg|finish", "path": "optional", "png_base64": "optional"},
    {"op": "set_layer", "page": "int", "layer": "str", "visible": "bool?", "exportable": "bool?"},
    {"op": "add_page", "count": "int"},
    {"op": "delete_page", "page": "int"},
    {"op": "duplicate_page", "page": "int"},
    {"op": "set_note", "page": "int", "note": "str"},
    {"op": "set_meta", "title": "str?", "episode": "int?", "preset": "str?"},
    {"op": "set_bible", "plot": "str?", "characters": "list?", "constraints": "list?"},
    {"op": "set_spread", "page": "int", "with": "int|null"},
    {"op": "reorder", "order": "[int]"},
    {"op": "flood_fill", "page": "int", "layer": "ink|bg", "x_mm": "float", "y_mm": "float", "rgb": "[r,g,b]", "gap_mm": "float?"},
    {"op": "add_tone", "page": "int", "frame_id": "str?", "lpi": "float", "density": "float"},
    {"op": "delete_tone", "page": "int", "id": "str"},
    {"op": "add_effect", "page": "int", "kind": "focus|speed|white", "frame_id": "str?", "params": "object"},
    {"op": "set_autosave", "enabled": "bool"},
    {"op": "erase_raster", "page": "int", "layer": "ink|name", "points": "[[x,y],...]", "width_mm": "float"},
    {"op": "reorder_layers", "page": "int", "order": "[id]"},
    {"op": "stamp_material", "page": "int", "material_id": "str", "frame_id": "str?"},
    {"op": "set_balloon_path", "id": "str", "path": "[[x,y]]?", "wrap": "vertical|horizontal", "ruby_runs": "[[base,ruby]]"},
    {"op": "add_mannequin", "page": "int", "pos": "[x,y,z]"},
    {"op": "pose_mannequin", "page": "int", "id": "str"},
    {"op": "set_onion", "page": "int", "from": "int?"},
    {"op": "lock_page", "page": "int", "agent": "str"},
    {"op": "unlock_page", "page": "int"},
    {"op": "add_layer", "page": "int", "name": "str?", "blend": "str?", "clip": "bool?", "folder": "bool?", "parent": "str?"},
    {"op": "delete_layer", "page": "int", "id": "str"},
    {"op": "filter_raster", "page": "int", "layer": "str?", "id": "str?", "kind": "blur|sharpen|hue|levels|curve|mosaic|bitonal"},
    {"op": "set_brush", "rgb": "[r,g,b]?", "width_mm": "float?", "stabilize": "int?", "taper": "bool?", "curve": "gpen|linear"},
    {"op": "undo"},
]


def _resolve_layer(page: Page, op: dict[str, Any]) -> Layer:
    if op.get("id"):
        found = next((item for item in page.layers if item.id == op["id"]), None)
        if found is None:
            raise ApplyError(f"no layer {op['id']}")
        return found
    if op.get("layer"):
        return page._layer(LayerRole(str(op["layer"])))
    raise ApplyError("layer id or role required")


def _require_page(episode: Episode, op: dict[str, Any]) -> Page:
    try:
        index = int(op["page"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ApplyError("page (int) is required") from exc
    for page in episode.pages:
        if page.index == index:
            return page
    raise ApplyError(f"no page {index}")


def _copy_state(dst: Episode, src: Episode) -> None:
    dst.title = src.title
    dst.episode = src.episode
    dst.spec = src.spec
    dst.binding = src.binding
    dst.pages = src.pages
    dst.story = src.story
    dst.bible = src.bible
    dst.tickets = src.tickets
    dst.autosave = src.autosave
    dst.font_path = getattr(src, "font_path", "")
    dst.page_locks = src.page_locks
    dst.brush_rgb = getattr(src, "brush_rgb", (20, 20, 20))
    dst.brush_width_mm = getattr(src, "brush_width_mm", 0.35)
    dst.brush_stabilize = getattr(src, "brush_stabilize", 0)
    dst.brush_taper = getattr(src, "brush_taper", False)
    dst.brush_curve = getattr(src, "brush_curve", "linear")


def _find_line(episode: Episode, line_id: str) -> StoryLine:
    for line in episode.story:
        if line.id == line_id:
            return line
    for page in episode.pages:
        for line in page.texts:
            if line.id == line_id:
                return line
    raise ApplyError(f"no line {line_id}")


def _parse_points(raw: list) -> list[tuple]:
    points = []
    for item in raw:
        if len(item) < 2:
            raise ApplyError("points needs [x_mm, y_mm]")
        if len(item) >= 3:
            points.append((float(item[0]), float(item[1]), float(item[2])))
        else:
            points.append((float(item[0]), float(item[1])))
    return points


def _parse_tail(value: Any) -> tuple[float, float] | None:
    if value is None:
        return None
    return (float(value[0]), float(value[1]))


def _apply_one(episode: Episode, op: dict[str, Any]) -> None:
    name = op.get("op")
    if not name:
        raise ApplyError("op is required")

    if name == "split_frame":
        page = _require_page(episode, op)
        axis = op.get("axis")
        if axis not in ("horizontal", "vertical"):
            raise ApplyError("axis must be horizontal or vertical")
        leaves = page.leaf_frames()
        if not leaves:
            raise ApplyError("page has no frames")
        frame_id = op.get("frame_id") or page.selected_frame_id or leaves[0].id
        page.split_frame(
            frame_id,
            axis=axis,
            ratio=float(op.get("ratio", 0.5)),
            gutter_mm=float(op.get("gutter_mm", 4)),
        )
        return

    if name == "merge_frame":
        page = _require_page(episode, op)
        frame_id = op.get("frame_id") or page.selected_frame_id
        if not frame_id:
            raise ApplyError("frame_id is required")
        page.merge_frame(str(frame_id))
        return

    if name == "resize_frame":
        page = _require_page(episode, op)
        frame_id = op.get("frame_id")
        rect_raw = op.get("rect") or {}
        if not frame_id:
            raise ApplyError("frame_id is required")
        page.resize_frame(
            str(frame_id),
            Rect(
                float(rect_raw["x"]),
                float(rect_raw["y"]),
                float(rect_raw["width"]),
                float(rect_raw["height"]),
            ),
        )
        return

    if name == "set_frame":
        page = _require_page(episode, op)
        frame_id = op.get("frame_id")
        if not frame_id:
            raise ApplyError("frame_id is required")
        frame = page._find(str(frame_id))
        if "bleed" in op:
            frame.bleed = bool(op["bleed"])
        if "clip" in op:
            frame.clip = bool(op["clip"])
        if "border_mm" in op:
            frame.border_mm = float(op["border_mm"])
        return

    if name == "add_line":
        page = _require_page(episode, op)
        text = op.get("text")
        if not text:
            raise ApplyError("text is required")
        line = episode.add_line(
            page.index,
            str(text),
            speaker=str(op.get("speaker", "")),
            frame_id=op.get("frame_id"),
            ruby=str(op.get("ruby", "")),
            x_mm=float(op.get("x_mm", 0)),
            y_mm=float(op.get("y_mm", 0)),
            w_mm=float(op.get("w_mm", 40)),
            h_mm=float(op.get("h_mm", 20)),
            balloon=str(op.get("balloon", "speech")),
            tail=_parse_tail(op.get("tail")),
        )
        if "wrap" in op:
            line.wrap = str(op["wrap"])
        if op.get("ruby_runs"):
            line.ruby_runs = [tuple(item) for item in op["ruby_runs"]]
        return

    if name == "edit_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        line = _find_line(episode, str(line_id))
        if "text" in op:
            line.text = str(op["text"])
        if "speaker" in op:
            line.speaker = str(op["speaker"])
        if "frame_id" in op:
            line.frame_id = op["frame_id"]
        if "ruby" in op:
            line.ruby = str(op["ruby"])
        if "balloon" in op:
            line.balloon = str(op["balloon"])
        return

    if name == "move_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        line = _find_line(episode, str(line_id))
        if "x_mm" in op:
            line.x_mm = float(op["x_mm"])
        if "y_mm" in op:
            line.y_mm = float(op["y_mm"])
        if "w_mm" in op:
            line.w_mm = float(op["w_mm"])
        if "h_mm" in op:
            line.h_mm = float(op["h_mm"])
        if "tail" in op:
            line.tail = _parse_tail(op.get("tail"))
        if "balloon" in op:
            line.balloon = str(op["balloon"])
        return

    if name == "delete_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        before = len(episode.story)
        episode.story = [line for line in episode.story if line.id != line_id]
        for page in episode.pages:
            page.texts = [line for line in page.texts if line.id != line_id]
        if len(episode.story) == before:
            raise ApplyError(f"no line {line_id}")
        return

    if name == "name_ok":
        pages = episode.pages if "page" not in op else [_require_page(episode, op)]
        for page in pages:
            page.name_ok = True
            advance(page, to="ink")
        return

    if name == "advance":
        page = _require_page(episode, op)
        to = op.get("to")
        if to not in ("name", "ink", "finish"):
            raise ApplyError("to must be name, ink, or finish")
        try:
            advance(page, to=to)
        except InkBlockedError as exc:
            raise ApplyError(str(exc)) from exc
        return

    if name == "add_stroke":
        page = _require_page(episode, op)
        layer_name = str(op.get("layer", "name"))
        points = _parse_points(op.get("points") or [])
        if len(points) < 2:
            raise ApplyError("points needs at least two [x_mm, y_mm] pairs")
        width = page.spec.width_mm
        if page.spread_with and min(float(pt[0]) for pt in points) >= width:
            other = next((item for item in episode.pages if item.index == page.spread_with), None)
            if other is not None:
                shifted = []
                for pt in points:
                    extra = list(pt[2:]) if len(pt) > 2 else []
                    shifted.append((float(pt[0]) - width, float(pt[1]), *extra) if extra else (float(pt[0]) - width, float(pt[1])))
                points = shifted
                page = other
        if op.get("snap_ruler"):
            points = _snap_points(page, points)
        stabilize = op.get("stabilize", episode.brush_stabilize)
        if stabilize:
            from genko.stroke import stabilize_points

            points = stabilize_points(points, int(stabilize))
        taper = op["taper"] if "taper" in op else episode.brush_taper
        if taper:
            from genko.stroke import taper_points

            points = taper_points(points)
        curve = str(op.get("curve") or episode.brush_curve or "linear")
        if curve and curve != "linear":
            from genko.stroke import apply_pressure_curve

            points = apply_pressure_curve(points, curve)
        from genko.models import coerce_stroke, stroke_points

        stroke = coerce_stroke(points)
        stroke.kind = str(op.get("kind") or "gpen")
        stroke.width_mm = float(op["width_mm"]) if op.get("width_mm") is not None else float(episode.brush_width_mm)
        role = LayerRole.INK if layer_name == "ink" else LayerRole.NAME if layer_name == "name" else LayerRole(layer_name)
        target = page._layer(role)
        if target.role == LayerRole.INK and not page.name_ok:
            raise ApplyError("ink strokes require name_ok")
        target.strokes.append(stroke)
        if target.role in (LayerRole.INK, LayerRole.FINISH, LayerRole.BG):
            from genko.raster import bake_stroke

            rgb = tuple(int(v) for v in op["rgb"]) if op.get("rgb") else tuple(int(v) for v in episode.brush_rgb)
            bake_stroke(
                page,
                target,
                stroke_points(stroke),
                rgb=rgb,
                kind=stroke.kind,
                width_mm=stroke.width_mm,
            )
        return

    if name == "delete_stroke":
        page = _require_page(episode, op)
        layer_name = str(op.get("layer", "name"))
        try:
            index = int(op["index"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ApplyError("index is required") from exc
        strokes = _strokes_of(page, layer_name)
        if index < 0 or index >= len(strokes):
            raise ApplyError("stroke index out of range")
        strokes.pop(index)
        return

    if name == "put_raster":
        page = _require_page(episode, op)
        blob: bytes | None = None
        if op.get("png_base64"):
            blob = base64.b64decode(op["png_base64"])
        elif op.get("path"):
            blob = Path(str(op["path"])).read_bytes()
        if not blob:
            raise ApplyError("put_raster needs path or png_base64")
        if op.get("id"):
            layer = _resolve_layer(page, op)
        else:
            role_name = str(op.get("layer") or "ink")
            try:
                role = LayerRole(role_name)
            except ValueError as exc:
                raise ApplyError(f"unknown layer {role_name}") from exc
            layer = page._layer(role)
        layer.kind = LayerKind.RASTER
        layer.raster_png = blob
        layer.raster_relpath = f"pages/{page.index:03d}/{'user-' + layer.id if layer.role == LayerRole.USER else layer.role.value}.png"
        if layer.role in (LayerRole.NAME, LayerRole.DRAFT):
            layer.exportable = False
        return

    if name == "set_layer":
        page = _require_page(episode, op)
        layer = None
        if op.get("id"):
            layer = next((item for item in page.layers if item.id == op["id"]), None)
        if layer is None and op.get("layer"):
            layer = page._layer(LayerRole(str(op["layer"])))
        if layer is None:
            raise ApplyError("layer id or role required")
        if "visible" in op:
            layer.visible = bool(op["visible"])
        if "opacity" in op:
            layer.opacity = float(op["opacity"])
        if "exportable" in op and layer.role not in (LayerRole.NAME, LayerRole.DRAFT):
            layer.exportable = bool(op["exportable"])
        if "blend" in op:
            layer.blend = str(op["blend"])
        if "clip" in op:
            layer.clip = bool(op["clip"])
        if "lock_alpha" in op:
            layer.lock_alpha = bool(op["lock_alpha"])
        if "parent" in op:
            layer.parent_id = op.get("parent")
        if "name" in op:
            layer.title = str(op["name"])
        return

    if name == "add_page":
        count = int(op.get("count", 1))
        for _ in range(count):
            index = len(episode.pages) + 1
            page = Page(index=index, spec=episode.spec, frames=[], binding=episode.binding)
            page.frames = [Frame(id=new_id(), rect=page.inner_rect_mm())]
            episode.pages.append(page)
        return

    if name == "delete_page":
        page = _require_page(episode, op)
        if len(episode.pages) == 1:
            raise ApplyError("cannot delete the last page")
        removed = page.index
        episode.pages = [item for item in episode.pages if item.index != removed]
        episode.story = [line for line in episode.story if line.page_index != removed]
        mapping = {item.index: new for new, item in enumerate(episode.pages, start=1)}
        for line in episode.story:
            line.page_index = mapping[line.page_index]
        for new, item in enumerate(episode.pages, start=1):
            item.index = new
            for line in item.texts:
                line.page_index = new
        return

    if name == "duplicate_page":
        page = _require_page(episode, op)
        clone: Page = copy.deepcopy(page)
        clone.index = len(episode.pages) + 1
        for frame in clone.frames:
            _refresh_frame_ids(frame)
        for layer in clone.layers:
            layer.id = new_id()
        new_lines: list[StoryLine] = []
        for line in list(episode.story_for_page(page.index)):
            copied = copy.deepcopy(line)
            copied.id = new_id()
            copied.page_index = clone.index
            new_lines.append(copied)
        clone.texts = new_lines
        episode.story.extend(new_lines)
        episode.pages.append(clone)
        return

    if name == "set_note":
        page = _require_page(episode, op)
        page.note = str(op.get("note", ""))
        return

    if name == "set_meta":
        if "title" in op:
            episode.title = str(op["title"])
        if "episode" in op:
            episode.episode = int(op["episode"])
        if "binding" in op:
            episode.binding = Binding(op["binding"])
        if "preset" in op:
            episode.spec = PageSpec.publisher(str(op["preset"]))
            for page in episode.pages:
                page.spec = episode.spec
        if "webtoon" in op and op["webtoon"]:
            episode.spec = PageSpec.webtoon()
            for page in episode.pages:
                page.spec = episode.spec
        if "font_path" in op:
            episode.font_path = str(op["font_path"])
        return

    if name == "set_bible":
        if "plot" in op:
            episode.bible.plot = str(op["plot"])
        if "characters" in op:
            episode.bible.characters = list(op["characters"])
        if "constraints" in op:
            episode.bible.constraints = [str(item) for item in op["constraints"]]
        return

    if name == "set_spread":
        page = _require_page(episode, op)
        other = op.get("with")
        page.spread_with = None if other in (None, "", 0) else int(other)
        return

    if name == "reorder":
        order = op.get("order")
        if not isinstance(order, list) or not order:
            raise ApplyError("order must be a non-empty list of page indexes")
        episode.reorder([int(i) for i in order])
        return

    if name == "select_frame":
        page = _require_page(episode, op)
        page.selected_frame_id = op.get("frame_id")
        return

    if name == "flood_fill":
        _flood_fill(episode, op)
        return

    if name == "add_tone":
        page = _require_page(episode, op)
        layer = Layer(
            id=new_id(),
            role=LayerRole.TONE,
            kind=LayerKind.TONE,
            lpi=float(op.get("lpi", 60)),
            density=float(op.get("density", 0.3)),
            exportable=True,
            angle=float(op.get("angle", 45)),
        )
        frame_id = op.get("frame_id")
        if frame_id:
            frame = page._find(str(frame_id))
            r = frame.rect
            layer.region = [
                (r.x, r.y),
                (r.x + r.width, r.y),
                (r.x + r.width, r.y + r.height),
                (r.x, r.y + r.height),
            ]
        page.layers.append(layer)
        return

    if name == "delete_tone":
        page = _require_page(episode, op)
        tone_id = op.get("id")
        before = len(page.layers)
        page.layers = [layer for layer in page.layers if layer.id != tone_id]
        if len(page.layers) == before:
            raise ApplyError(f"no tone {tone_id}")
        return

    if name == "add_effect":
        page = _require_page(episode, op)
        kind = op.get("kind")
        if kind not in ("focus", "speed", "white"):
            raise ApplyError("kind must be focus, speed, or white")
        page.effects.append(
            {
                "id": new_id(),
                "kind": kind,
                "frame_id": op.get("frame_id"),
                "params": dict(op.get("params") or {}),
            }
        )
        return

    if name == "set_autosave":
        episode.autosave = bool(op.get("enabled", True))
        return

    if name == "edit_stroke":
        page = _require_page(episode, op)
        strokes = _strokes_of(page, str(op.get("layer", "name")))
        index = int(op["index"])
        if index < 0 or index >= len(strokes):
            raise ApplyError("stroke index out of range")
        points = _parse_points(op.get("points") or [])
        if len(points) < 2:
            raise ApplyError("points needs at least two [x_mm, y_mm] pairs")
        from genko.models import coerce_stroke

        strokes[index] = coerce_stroke(points)
        return

    if name == "simplify_stroke":
        page = _require_page(episode, op)
        strokes = _strokes_of(page, str(op.get("layer", "name")))
        index = int(op["index"])
        if index < 0 or index >= len(strokes):
            raise ApplyError("stroke index out of range")
        from genko.models import Stroke, coerce_stroke, stroke_points

        raw = stroke_points(strokes[index])
        simplified = _rdp(raw, float(op.get("epsilon_mm", 0.8)))
        strokes[index] = coerce_stroke(simplified)
        return

    if name == "set_ruler":
        page = _require_page(episode, op)
        page.ruler = {
            "kind": str(op.get("kind") or "perspective"),
            "points": [tuple(pt) for pt in (op.get("points") or [])],
        }
        return

    if name == "add_prim3d":
        page = _require_page(episode, op)
        page.prims.append(
            {
                "id": new_id(),
                "kind": str(op.get("kind") or "box"),
                "pos": list(op.get("pos") or [100, 150, 0]),
                "size": list(op.get("size") or [40, 40, 40]),
                "rot": list(op.get("rot") or [0, 0.5, 0.3]),
            }
        )
        return

    if name == "lt_convert":
        _lt_convert(episode, op)
        return

    if name == "add_ticket":
        page = _require_page(episode, op)
        episode.tickets.append(
            {
                "id": new_id(),
                "page_index": page.index,
                "frame_id": op.get("frame_id"),
                "role": str(op.get("role") or "bg"),
                "assignee": str(op.get("assignee") or "human"),
                "rate": str(op.get("rate") or ""),
                "status": "open",
            }
        )
        return

    if name == "set_ticket":
        ticket_id = op.get("id")
        for ticket in episode.tickets:
            if ticket["id"] == ticket_id:
                if "status" in op:
                    ticket["status"] = str(op["status"])
                if "assignee" in op:
                    ticket["assignee"] = str(op["assignee"])
                if "rate" in op:
                    ticket["rate"] = str(op["rate"])
                return
        raise ApplyError(f"no ticket {ticket_id}")

    if name == "erase_raster":
        page = _require_page(episode, op)
        role = LayerRole(str(op.get("layer") or "ink"))
        target = page._layer(role)
        from genko.raster import erase_raster

        erase_raster(page, target, _parse_points(op.get("points") or []), width_mm=float(op.get("width_mm", 2)))
        return

    if name == "reorder_layers":
        page = _require_page(episode, op)
        order = op.get("order") or []
        by_id = {layer.id: layer for layer in page.layers}
        page.layers = [by_id[item] for item in order if item in by_id]
        return

    if name == "stamp_material":
        page = _require_page(episode, op)
        from genko.materials import get_material

        material = get_material(str(op["material_id"]))
        layer = Layer(
            id=new_id(),
            role=LayerRole.TONE if material.get("kind") != "effect" else LayerRole.EFFECT,
            kind=LayerKind.TONE,
            lpi=material.get("lpi"),
            density=material.get("density"),
            exportable=True,
            material_id=material["id"],
        )
        frame_id = op.get("frame_id")
        if frame_id:
            frame = page._find(str(frame_id))
            r = frame.rect
            layer.region = [(r.x, r.y), (r.x + r.width, r.y), (r.x + r.width, r.y + r.height), (r.x, r.y + r.height)]
        if material.get("kind") == "effect":
            page.effects.append({"id": new_id(), "kind": material.get("effect", "speed"), "frame_id": frame_id, "params": {}})
        else:
            page.layers.append(layer)
        return

    if name == "set_balloon_path":
        line = _find_line(episode, str(op.get("id") or ""))
        if "path" in op:
            line.path = [tuple(pt) for pt in op["path"]] if op["path"] else None
        if "wrap" in op:
            line.wrap = str(op["wrap"])
        if "ruby_runs" in op:
            line.ruby_runs = [tuple(item) for item in op["ruby_runs"]]
        return

    if name == "add_mannequin":
        page = _require_page(episode, op)
        page.prims.append(
            {
                "id": new_id(),
                "kind": "mannequin",
                "pos": list(op.get("pos") or [100, 160, 0]),
                "size": [40, 80, 20],
                "rot": [0, 0.2, 0],
                "joints": {
                    "hip": {"yaw": 0.0, "pitch": 0.0},
                    "spine": {"yaw": 0.0, "pitch": 0.0},
                    "head": {"yaw": 0.0, "pitch": 0.0},
                    "l_arm": {"yaw": 0.4, "pitch": 0.0},
                    "r_arm": {"yaw": -0.4, "pitch": 0.0},
                    "l_leg": {"yaw": 0.15, "pitch": 0.0},
                    "r_leg": {"yaw": -0.15, "pitch": 0.0},
                },
            }
        )
        return

    if name == "pose_mannequin":
        page = _require_page(episode, op)
        mannequin_id = op.get("id")
        for prim in page.prims:
            if prim.get("id") == mannequin_id:
                if "rot" in op:
                    prim["rot"] = list(op["rot"])
                if "pos" in op:
                    prim["pos"] = list(op["pos"])
                if "joints" in op:
                    joints = prim.setdefault("joints", {})
                    for name, values in dict(op["joints"]).items():
                        slot = joints.setdefault(name, {})
                        slot.update(values)
                return
        raise ApplyError(f"no mannequin {mannequin_id}")

    if name == "set_onion":
        page = _require_page(episode, op)
        page.onion_from = None if op.get("from") in (None, "", 0) else int(op["from"])
        return

    if name == "lock_page":
        page = _require_page(episode, op)
        episode.page_locks[str(page.index)] = str(op.get("agent") or "genko")
        return

    if name == "unlock_page":
        page = _require_page(episode, op)
        episode.page_locks.pop(str(page.index), None)
        return

    if name == "add_layer":
        page = _require_page(episode, op)
        folder = bool(op.get("folder"))
        layer = Layer(
            id=new_id(),
            role=LayerRole.USER,
            kind=LayerKind.FOLDER if folder else LayerKind.RASTER,
            title=str(op.get("name") or "layer"),
            blend=str(op.get("blend") or "normal"),
            clip=bool(op.get("clip")),
            lock_alpha=bool(op.get("lock_alpha")),
            parent_id=str(op["parent"]) if op.get("parent") else None,
            exportable=True,
        )
        page.layers.append(layer)
        return

    if name == "delete_layer":
        page = _require_page(episode, op)
        layer_id = str(op.get("id") or "")
        target = next((item for item in page.layers if item.id == layer_id), None)
        if target is None:
            raise ApplyError("layer not found")
        if target.role in (LayerRole.NAME, LayerRole.INK, LayerRole.BG, LayerRole.FINISH):
            raise ApplyError("cannot delete core layer")
        page.layers = [item for item in page.layers if item.id != layer_id]
        return

    if name == "filter_raster":
        from genko.filters import apply_filter
        from genko.raster import ensure_raster, save_raster

        page = _require_page(episode, op)
        layer = _resolve_layer(page, op)
        kind = str(op.get("kind") or "")
        image = ensure_raster(page, layer)
        params = {key: value for key, value in op.items() if key not in {"op", "page", "layer", "id", "kind"}}
        try:
            filtered = apply_filter(image, kind, params)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        save_raster(page, layer, filtered)
        layer.kind = LayerKind.RASTER
        return

    if name == "set_brush":
        if op.get("rgb"):
            episode.brush_rgb = tuple(int(v) for v in op["rgb"])  # type: ignore[assignment]
        if op.get("width_mm") is not None:
            episode.brush_width_mm = float(op["width_mm"])
        if "stabilize" in op:
            episode.brush_stabilize = int(op["stabilize"] or 0)
        if "taper" in op:
            episode.brush_taper = bool(op["taper"])
        if op.get("curve"):
            episode.brush_curve = str(op["curve"])
        return

    raise ApplyError(f"unknown op: {name}")


def _flood_fill(episode: Episode, op: dict[str, Any]) -> None:
    from PIL import Image, ImageDraw, ImageFilter

    from genko.render import mm_to_px, rect_px

    page = _require_page(episode, op)
    role = LayerRole(str(op.get("layer") or "ink"))
    if role == LayerRole.INK and not page.name_ok:
        raise ApplyError("ink flood_fill requires name_ok")
    rgb = tuple(int(v) for v in (op.get("rgb") or [0, 0, 0]))
    x_mm = float(op.get("x_mm", 0))
    y_mm = float(op.get("y_mm", 0))
    gap_mm = float(op.get("gap_mm", 0))
    layer = page._layer(role)
    dpi = 72
    if layer.raster_png:
        image = Image.open(__import__("io").BytesIO(layer.raster_png)).convert("RGB")
    else:
        image = Image.new(
            "RGB",
            (mm_to_px(page.spec.width_mm, dpi), mm_to_px(page.spec.height_mm, dpi)),
            (255, 255, 255),
        )
    if gap_mm > 0:
        radius = max(1, mm_to_px(gap_mm, dpi))
        image = image.filter(ImageFilter.MaxFilter(size=radius * 2 + 1 if radius * 2 + 1 % 2 else radius * 2 + 3))
    seed = (mm_to_px(x_mm, dpi), mm_to_px(y_mm, dpi))
    seed = (min(max(0, seed[0]), image.width - 1), min(max(0, seed[1]), image.height - 1))
    try:
        ImageDraw.floodfill(image, seed, rgb, thresh=8)
    except Exception:
        frame = page.frame_at(x_mm, y_mm) or (page.leaf_frames()[0] if page.leaf_frames() else None)
        if frame is None:
            raise ApplyError("flood_fill missed the page")
        ImageDraw.Draw(image).rectangle(rect_px(frame.rect, dpi), fill=rgb)
    buf = __import__("io").BytesIO()
    image.save(buf, format="PNG")
    layer.kind = LayerKind.RASTER
    layer.raster_png = buf.getvalue()
    layer.raster_relpath = f"pages/{page.index:03d}/{role.value}.png"


def _strokes_of(page: Page, layer_name: str):
    role = LayerRole.INK if layer_name == "ink" else LayerRole.NAME if layer_name == "name" else LayerRole(layer_name)
    return page._layer(role).strokes


def _perp(point, start, end) -> float:
    x, y = float(point[0]), float(point[1])
    x1, y1 = float(start[0]), float(start[1])
    x2, y2 = float(end[0]), float(end[1])
    dx, dy = x2 - x1, y2 - y1
    length = (dx * dx + dy * dy) ** 0.5 or 1.0
    return abs((y2 - y1) * x - (x2 - x1) * y + x2 * y1 - y2 * x1) / length


def _rdp(points: list, epsilon: float) -> list:
    if len(points) < 3:
        return list(points)
    dmax = 0.0
    index = 0
    for i in range(1, len(points) - 1):
        distance = _perp(points[i], points[0], points[-1])
        if distance > dmax:
            index = i
            dmax = distance
    if dmax > epsilon:
        left = _rdp(points[: index + 1], epsilon)
        right = _rdp(points[index:], epsilon)
        return left[:-1] + right
    return [points[0], points[-1]]


def _snap_points(page: Page, points: list) -> list:
    ruler = page.ruler or {}
    vps = ruler.get("points") or []
    if not vps:
        return points
    vx, vy = float(vps[0][0]), float(vps[0][1])
    x0, y0 = float(points[0][0]), float(points[0][1])
    x1, y1 = float(points[-1][0]), float(points[-1][1])
    dx, dy = vx - x0, vy - y0
    denom = dx * dx + dy * dy
    if denom < 1e-6:
        return points
    t = ((x1 - x0) * dx + (y1 - y0) * dy) / denom
    snapped = (x0 + t * dx, y0 + t * dy)
    extra = points[-1][2:] if len(points[-1]) > 2 else ()
    new_last = (snapped[0], snapped[1], *extra) if extra else snapped
    return [*points[:-1], new_last]


def _lt_convert(episode: Episode, op: dict[str, Any]) -> None:
    from genko.lt import runs_to_strokes, to_line_art
    from genko.models import coerce_stroke

    page = _require_page(episode, op)
    src_role = LayerRole(str(op.get("layer") or "bg"))
    dest_role = LayerRole(str(op.get("to") or "ink"))
    if dest_role == LayerRole.INK and not page.name_ok:
        raise ApplyError("lt_convert to ink requires name_ok")
    src = page._layer(src_role)
    if not src.raster_png:
        raise ApplyError("lt_convert needs a raster on the source layer")
    from PIL import Image

    image = Image.open(__import__("io").BytesIO(src.raster_png))
    binary = to_line_art(image, method=str(op.get("method") or "adaptive"))
    dest = page._layer(dest_role)
    for run in runs_to_strokes(binary, page.spec.width_mm, page.spec.height_mm):
        dest.strokes.append(coerce_stroke(run))
    if not dest.strokes:
        dest.strokes.append(coerce_stroke([(10.0, 10.0), (page.spec.width_mm - 10, 10.0)]))


def _refresh_frame_ids(frame: Frame) -> None:
    frame.id = new_id()
    for child in frame.children:
        _refresh_frame_ids(child)


def _check_page_lock(episode: Episode, op: dict[str, Any], agent: str) -> None:
    name = op.get("op")
    if name in ("lock_page", "unlock_page", "undo", "set_meta", "set_bible", "set_autosave"):
        return
    if "page" not in op:
        return
    try:
        index = int(op["page"])
    except (TypeError, ValueError):
        return
    owner = episode.page_locks.get(str(index))
    if owner and owner != agent:
        raise ApplyError(f"page {index} locked by {owner}")


def apply_ops(
    episode: Episode,
    ops: list[dict[str, Any]],
    dry_run: bool = False,
    agent: str = "genko",
) -> dict[str, Any]:
    from genko.headless import snapshot

    if not isinstance(ops, list):
        raise ApplyError("ops must be a JSON array")

    if len(ops) == 1 and ops[0].get("op") == "undo":
        if dry_run:
            return {"ok": True, "applied": ["undo"], "snapshot": snapshot(episode), "job_id": new_id()}
        if not episode.undo_stack:
            raise ApplyError("nothing to undo")
        previous = episode.undo_stack.pop()
        stack = episode.undo_stack
        _copy_state(episode, previous)
        episode.undo_stack = stack
        return {"ok": True, "applied": ["undo"], "snapshot": snapshot(episode), "job_id": new_id()}

    work = copy.deepcopy(episode)
    work.undo_stack = []
    applied: list[str] = []
    for i, op in enumerate(ops):
        if not isinstance(op, dict):
            raise ApplyError(f"ops[{i}] must be an object")
        try:
            _check_page_lock(work, op, agent)
            _apply_one(work, op)
        except ApplyError as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        except (KeyError, ValueError, TypeError) as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        applied.append(str(op.get("op")))

    if dry_run:
        return {"ok": True, "applied": applied, "snapshot": snapshot(work), "job_id": new_id()}

    frozen = copy.deepcopy(episode)
    frozen.undo_stack = []
    episode.undo_stack.append(frozen)
    _copy_state(episode, work)
    return {"ok": True, "applied": applied, "snapshot": snapshot(episode), "job_id": new_id()}
