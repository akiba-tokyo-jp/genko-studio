from __future__ import annotations

import base64
import copy
import dataclasses
import io
import math
from pathlib import Path
from typing import Any

from genko.models import (
    TRANSIENT_FIELDS,
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
from PIL import Image, ImageChops, ImageDraw, ImageOps

from genko.pipeline import InkBlockedError, advance


class ApplyError(ValueError):
    """Invalid or blocked operation."""


OPS_SCHEMA: list[dict[str, Any]] = [
    {"op": "split_frame", "page": "int", "axis": "horizontal|vertical", "ratio": "float", "gutter_mm": "float", "tilt_mm": "float? (slant: the cut's ends differ by this)", "frame_id": "optional", "force": "bool? (placed art goes to studio.orphans)"},
    {"op": "merge_frame", "page": "int", "frame_id": "str", "force": "bool? (placed art goes to studio.orphans)"},
    {"op": "resize_frame", "page": "int", "frame_id": "str", "rect": "{x,y,width,height}"},
    {"op": "set_frame", "page": "int", "frame_id": "str", "bleed": "bool?", "clip": "bool?", "border_mm": "float? (0: no border)", "poly": "[[x,y],...] | null? (a free-form panel; null goes back to the cut shape)"},
    {"op": "cut_frame", "page": "int", "frame_id": "str?", "p0": "[x,y]", "p1": "[x,y]", "gutter_mm": "float?", "note": "cut a panel along any line (slanted panels)"},
    {"op": "move_gutter", "page": "int", "frame_id": "str (the split)", "index": "int? (gutter after this child)", "delta_mm": "float", "gutter_mm": "float? (new width)"},
    {"op": "add_line", "page": "int", "text": "str", "speaker": "optional", "frame_id": "optional", "balloon": "optional", "x_mm": "optional", "y_mm": "optional", "w_mm": "optional", "h_mm": "optional", "wrap": "vertical|horizontal?", "tail": "[x,y]?", "tails": "[{to, via?, width_mm?}]?", "style": "object?", "ruby_runs": "[[base, ruby]]?", "emphasis_runs": "[str]?", "style_runs": "[[words, {scale, bold, rgb}]]?", "path": "[[x,y]]? (a hand-drawn balloon)", "id": "str? (choose the id)"},
    {"op": "edit_line", "id": "str", "text": "optional", "speaker": "optional", "balloon": "speech|rounded|box|cloud|thought|shout|electric|flash|whisper|narration|sfx|none?", "wrap": "vertical|horizontal?", "ruby": "str?", "ruby_runs": "[[base, ruby]]?", "emphasis_runs": "[str]? (傍点 on these words)", "style_runs": "[[words, {scale 0.3..3, bold, weight, rgb}]]? (part of the line larger, smaller, bolder, coloured)", "frame_id": "str?", "style": "{font, size_mm, tracking, leading, align, outline_mm, rgb, tcy, border_mm, fill, group, rotate_deg, skew_deg, arc (-1..1), latin: rotate|upright, emphasis_mark: sesame|dot, bold, weight: normal|bold|heavy, italic, outline_rgb, wobble 0..1, double, spikes 6..80, spike_depth 0.05..0.6}? (null resets a key)", "tails": "[{to:[x,y], via?:[x,y], width_mm?}]?"},
    {"op": "reorder_lines", "page": "int", "order": "[line id] (reading order)"},
    {"op": "delete_line", "id": "str"},
    {"op": "move_line", "id": "str", "x_mm": "float?", "y_mm": "float?", "w_mm": "float?", "h_mm": "float?", "tail": "[x,y]?", "tails": "[{to, via?, width_mm?}]?", "balloon": "str?"},
    {"op": "name_ok", "page": "int, optional (all pages if omitted)"},
    {"op": "advance", "page": "int", "to": "name|ink|finish"},
    {"op": "add_stroke", "page": "int", "layer": "name|ink", "layer_id": "str? (a pen or paint layer)", "points": "[[x,y,pressure?],...]", "space": "page|spread?", "width_mm": "float?", "rgb": "[r,g,b]?", "opacity": "float?", "kind": "gpen|maru|kabura|mili|pencil|fude|marker|airbrush|fill_pen|white?", "stabilize": "int?", "taper": "bool?", "pressure_gamma": "float? (>1 needs more force)"},
    {"op": "delete_stroke", "page": "int", "layer": "name|ink", "index": "int"},
    {"op": "put_raster", "page": "int", "layer": "name|draft|ink|bg|finish", "path": "optional", "png_base64": "optional"},
    {"op": "set_layer", "page": "int", "layer": "str", "id": "str?", "visible": "bool?", "exportable": "bool?", "opacity": "float?", "blend": "str?", "clip": "bool?", "lock_alpha": "bool?", "locked": "bool?", "panel_clip": "bool? (false: lines run out of the panels)", "name": "str?", "color": "[r,g,b]|null? (shown in this colour on screen, never printed)", "reference": "bool? (fills with reference: reference look at this layer)"},
    {"op": "add_page", "count": "int", "after": "int? (insert after this page; default at the end)"},
    {"op": "delete_page", "page": "int"},
    {"op": "duplicate_page", "page": "int", "next_to": "bool? (the copy right after the page; default at the end)"},
    {"op": "set_page_spec", "preset": "b4|b5|a5|a4|webtoon?", "paper": "[w,h]? mm", "trim": "[w,h]? (finished size)", "bleed_mm": "float?", "margins": "[top,bottom,inner,outer] | {top,bottom,inner,outer}? (basic frame, from the trim)", "dpi": "int?", "move": "bool? (default true: move everything onto the new basic frame)"},
    {"op": "set_nombre", "page": "int? (with numero: show or hide that page's)", "numero": "bool?", "position": "bottom_center|bottom_outside|top_outside|side_outside?", "font": "str?", "size_mm": "float?", "start": "int? (the number of page 1)", "hidden": "bool? (隠しノンブル)", "hidden_size_mm": "float?", "show": "bool? (visible nombres)"},
    {"op": "set_note", "page": "int", "note": "str"},
    {"op": "set_meta", "title": "str?", "episode": "int?", "preset": "str?", "binding": "right|left?", "start_side": "left|right|null?", "strict_gates": "bool?", "font_path": "str?"},
    {"op": "set_bible", "plot": "str?", "characters": "list?", "constraints": "list?"},
    {"op": "set_spread", "page": "int", "with": "int|null"},
    {"op": "reorder", "order": "[int]"},
    {"op": "flood_fill", "page": "int", "layer": "ink|bg", "x_mm": "float", "y_mm": "float", "rgb": "[r,g,b]", "gap_mm": "float?"},
    {"op": "add_tone", "page": "int", "frame_id": "str?", "area": "{poly}|{mask}?", "at": "{x_mm, y_mm, gap_mm?, reference?}? (the region a fill would take)", "lpi": "float?", "density": "float? (0..1 black)", "angle": "float?", "pattern": "dot|line|cross|noise|flat?", "gradient": "{shape: linear|radial, angle, start, end}?", "name": "str?", "after": "layer id?", "id": "str?", "note": "nothing given: every panel"},
    {"op": "set_tone", "page": "int", "id": "str", "lpi": "float?", "density": "float?", "angle": "float?", "pattern": "str?", "gradient": "object|null?", "name": "str?"},
    {"op": "delete_tone", "page": "int", "id": "str"},
    {"op": "add_effect", "page": "int", "kind": "focus|speed|uni_flash|beta_flash|white", "frame_id": "str?", "params": "object (see docs)", "id": "str?"},
    {"op": "edit_effect", "page": "int", "id": "str", "params": "object? (merged; null removes a key)", "kind": "str?", "frame_id": "str|null?", "visible": "bool?"},
    {"op": "delete_effect", "page": "int", "id": "str"},
    {"op": "effect_to_layer", "page": "int", "id": "str", "layer_id": "str", "keep": "bool? (keep the effect too)"},
    {"op": "set_autosave", "enabled": "bool"},
    {"op": "erase_raster", "page": "int", "layer": "ink|name", "points": "[[x,y],...]", "width_mm": "float"},
    {"op": "fill", "page": "int", "layer_id": "str?", "x_mm": "float", "y_mm": "float", "rgb": "[r,g,b]?", "opacity": "float?", "gap_mm": "float? (close gaps up to this)", "expand_mm": "float? (grow under the lines)", "reference": "page|layer|reference? (reference: the layers set as reference)"},
    {"op": "fill_area", "page": "int", "layer_id": "str?", "area": "{poly} | {mask: {box, png}}", "rgb": "[r,g,b]?", "opacity": "float?"},
    {"op": "transform_area", "page": "int", "layer_id": "str?", "area": "{poly} | {mask}", "matrix": "[a,b,c,d,e,f] (x'=ax+cy+e, y'=bx+dy+f, mm)", "warp": "{perspective: [[x,y]×4] (where the box's top-left, top-right, bottom-right, bottom-left go)} | {mesh: [[x,y]×9] (a 3×3 grid over the box, row by row)} (instead of matrix)"},
    {"op": "delete_area", "page": "int", "layer_id": "str?", "area": "{poly} | {mask}"},
    {"op": "paste", "page": "int", "layer_id": "str?", "items": "{strokes, patches} (copied)", "matrix": "[a,b,c,d,e,f]?"},
    {"op": "set_stroke_width", "page": "int", "layer_id": "str?", "area": "object?", "ids": "[stroke id]?", "width_mm": "float?", "scale": "float?", "kind": "str?", "rgb": "[r,g,b]?"},
    {"op": "reshape_stroke", "page": "int", "layer_id": "str?", "stroke_id": "str", "points": "[[x,y,p?],...]?", "width_mm": "float?"},
    {"op": "erase", "page": "int", "layer_id": "str? (else layer: role)", "layer": "str?", "points": "[[x,y],...]", "width_mm": "float", "mode": "to_crossing? (cut a line only up to where it crosses others)", "note": "cuts pen lines (vector) and clears paint"},
    {"op": "reorder_layers", "page": "int", "order": "[id]"},
    {"op": "stamp_material", "page": "int", "material_id": "str", "frame_id": "str?", "area": "object?", "at": "object?", "layer_id": "str? (pictures and drawn parts)", "x_mm": "float?", "y_mm": "float? (where a picture's / part's middle goes)", "width_mm": "float?"},
    {"op": "set_balloon_path", "id": "str", "path": "[[x,y]]? (a hand-drawn outline; the box becomes its bounds; null goes back to the shape)", "wrap": "vertical|horizontal", "ruby_runs": "[[base,ruby]]", "emphasis_runs": "[str]"},
    {"op": "add_mannequin", "page": "int", "pos": "[x,y,z] (pelvis, mm)", "height_mm": "float?", "rot": "[tip,turn,lean]?", "preset": "stand|walk|run|sit|point|look_back|arms_up?", "id": "str?"},
    {"op": "pose_mannequin", "page": "int", "id": "str", "joints": "{name: {yaw, pitch}}?", "rot": "[tip,turn,lean]?", "pos": "[x,y,z]?", "height_mm": "float?", "preset": "str?", "drag": "{handle: pelvis|chest|head|l_elbow|l_hand|l_knee|…, to: [x,y]}?"},
    {"op": "set_onion", "page": "int", "from": "int?"},
    {"op": "step_onion", "page": "int", "delta": "int"},
    {"op": "set_lt", "page": "int", "threshold": "float"},
    {"op": "lock_page", "page": "int", "agent": "str"},
    {"op": "unlock_page", "page": "int"},
    {"op": "add_layer", "page": "int", "name": "str?", "kind": "pen|paint|folder?", "blend": "str?", "clip": "bool?", "folder": "bool?", "parent": "str?", "after": "layer id?", "id": "str?"},
    {"op": "delete_layer", "page": "int", "id": "str"},
    {"op": "gradient_fill", "page": "int", "layer_id": "str?", "area": "{poly} | {mask}? (default: the whole page)", "from": "[x,y] (mm)", "to": "[x,y] (mm)", "rgb_from": "[r,g,b]?", "rgb_to": "[r,g,b]?", "opacity_from": "0..1? (1)", "opacity_to": "0..1? (0: fades out)", "shape": "linear|radial?"},
    {"op": "define_brush", "key": "str (my_…)", "label": "str", "base": "a brush to start from?", "width_mm": "float?", "min_pressure": "0..1?", "gamma": "0.2..5?", "opacity": "0.05..1?", "stabilize": "0..15?", "taper": "bool?", "texture": "''|grain|soft|dry?", "rgb": "[r,g,b]|null?", "fixed_width": "bool?", "delete": "bool?"},
    {"op": "duplicate_layer", "page": "int", "id": "str", "new_id": "str?"},
    {"op": "merge_down", "page": "int", "id": "str (merged into the layer below it; pen onto pen stays lines, anything else becomes pixels)"},
    {"op": "set_layer_mask", "page": "int", "id": "str", "area": "{poly} | {mask}? (only this area shows)", "fill": "show|hide? (the whole mask)", "invert": "bool?", "enabled": "bool?", "delete": "bool?"},
    {"op": "paint_mask", "page": "int", "id": "str", "points": "[[x,y],...]", "width_mm": "float?", "show": "bool (true: the pen shows the layer, false: the eraser hides it)"},
    {"op": "filter_raster", "page": "int", "layer": "str?", "id": "str?", "kind": "blur|sharpen|hue|levels|curve|mosaic|bitonal"},
    {"op": "set_brush", "rgb": "[r,g,b]?", "width_mm": "float?", "stabilize": "int?", "taper": "bool?", "curve": "gpen|linear"},
    {"op": "select_frame", "page": "int", "frame_id": "str"},
    {"op": "edit_stroke", "page": "int", "layer": "name|ink", "index": "int", "points": "[[x,y],...]"},
    {"op": "simplify_stroke", "page": "int", "layer": "name|ink", "index": "int", "epsilon_mm": "float?"},
    {"op": "set_ruler", "page": "int", "kind": "str", "pos": "[x,y]?", "points": "[[x,y],...]?", "note": "old single ruler; use add_ruler"},
    {"op": "add_ruler", "page": "int", "kind": "line|curve|parallel|concentric|radial|perspective|symmetry", "points": "[[x,y],...]?", "angle": "float? (deg)", "ratio": "float? (concentric height/width)", "copies": "int? (symmetry)", "mirror": "bool?", "frame_id": "str? (only in this panel)", "reach_mm": "float?", "id": "str?"},
    {"op": "edit_ruler", "page": "int", "id": "str", "points": "[[x,y],...]?", "angle": "float?", "ratio": "float?", "copies": "int?", "mirror": "bool?", "frame_id": "str|null?", "active": "bool?", "visible": "bool?"},
    {"op": "delete_ruler", "page": "int", "id": "str? (none: every ruler on the page)"},
    {"op": "add_prim3d", "kind": "box|cylinder|stairs|floor", "steps": "int? (stairs)", "lines": "int? (floor grid)", "page": "int", "pos": "[x,y,z]?", "size": "[w,h,d] | float?", "rot": "[tip,turn,lean]?", "focal_mm": "float?", "id": "str?"},
    {"op": "add_scene", "page": "int", "kind": "room|classroom|corridor|street", "pos": "[x,y,z]? (centre; z = depth)", "size": "[w,h,d]|number? (mm; a number scales the usual size)", "rot": "[tip,turn,lean]? radians", "focal_mm": "float? (smaller = stronger perspective; 220)", "id": "str?"},
    {"op": "edit_prim", "page": "int", "id": "str", "pos": "[x,y,z]?", "size": "[w,h,d]?", "rot": "[tip,turn,lean]?", "focal_mm": "float?"},
    {"op": "delete_prim", "page": "int", "id": "str"},
    {"op": "trace_prims", "page": "int", "layer_id": "str", "ids": "[id]? (none: all)", "kind": "str? (pencil)", "width_mm": "float?", "rgb": "[r,g,b]?"},
    {"op": "lt_convert", "page": "int", "layer": "str?", "to": "ink|name?"},
    {"op": "add_ticket", "page": "int", "id": "str?", "frame_id": "str?", "role": "str?", "assignee": "str?", "rate": "str?"},
    {"op": "set_ticket", "id": "str", "status": "str?", "assignee": "str?", "rate": "str?"},
    {"op": "undo"},
]

def _with_studio_schema() -> None:
    from genko.studio.studio_ops import STUDIO_SCHEMA

    OPS_SCHEMA.extend(STUDIO_SCHEMA)


_with_studio_schema()


def _resolve_layer(page: Page, op: dict[str, Any]) -> Layer:
    if op.get("id"):
        found = next((item for item in page.layers if item.id == op["id"]), None)
        if found is None:
            raise ApplyError(f"no layer {op['id']}")
        return found
    if op.get("layer"):
        return page._layer(LayerRole(str(op["layer"])))
    raise ApplyError("layer id or role required")


def _rasterize_strokes(page, layer) -> None:
    """Turn a layer's pen lines into its pixels (filters and fills work on pixels)."""
    from genko.models import stroke_points
    from genko.raster import bake_stroke

    for stroke in layer.strokes:
        bake_stroke(page, layer, stroke_points(stroke), rgb=tuple(stroke.rgb or (20, 20, 20)), kind=stroke.kind,
                    width_mm=stroke.width_mm)
    layer.strokes = []
    layer.kind = LayerKind.RASTER


def _brush_kind(kind, episode=None) -> str:
    from genko.brushes import BRUSHES, LEGACY

    kind = LEGACY.get(str(kind), str(kind))
    if kind not in BRUSHES and not (episode is not None and kind in episode.brush_custom):
        raise ApplyError(f"kind must be one of {', '.join(BRUSHES)} (or a brush defined in the book with define_brush)")
    return kind


def _paint_target(page, op: dict):
    """The layer an edit works on: layer_id, else the role in `layer` (ink by default)."""
    if op.get("layer_id"):
        target = _layer_by_id(page, str(op["layer_id"]))
    else:
        target = page._layer(LayerRole(str(op.get("layer") or "ink")))
    if getattr(target, "locked", False):
        raise ApplyError("the layer is locked")
    if target.kind not in (LayerKind.STROKES, LayerKind.RASTER, LayerKind.TONE):
        raise ApplyError("this layer cannot be painted on (choose a pen, paint or tone layer)")
    return target


def _rgb(op: dict, episode) -> tuple[int, int, int]:
    rgb = op.get("rgb") or episode.brush_rgb or (20, 20, 20)
    return tuple(int(v) for v in rgb)


def _area(op: dict) -> dict:
    area = op.get("area") or {}
    if area.get("poly"):
        if len(area["poly"]) < 3:
            raise ApplyError("an area needs at least three corners")
        return area
    if area.get("mask") and area["mask"].get("box") and area["mask"].get("png"):
        return area
    raise ApplyError("area is {poly: [[x, y], …]} or {mask: {box, png}}")


def _gated(episode) -> bool:
    """The name → art → finish order is enforced only for books made with agents (strict gates or a
    studio); a person drawing alone can ink, tone and finish whenever they like."""
    return bool(getattr(episode, "strict_gates", False) or getattr(episode, "studio", None))


def _tone_numbers(layer, op: dict) -> None:
    if op.get("lpi") is not None:
        lpi = float(op["lpi"])
        if not 5 <= lpi <= 300:
            raise ApplyError("lpi is 5 to 300")
        layer.lpi = lpi
    if op.get("density") is not None:
        density = float(op["density"])
        if not 0 <= density <= 1:
            raise ApplyError("density is 0 to 1 (the black share)")
        layer.density = density
    if op.get("angle") is not None:
        layer.angle = float(op["angle"])


def _new_tone(episode, page, op: dict) -> Layer:
    """A tone layer from add_tone / a tone material: its look and where it goes."""
    from genko import fill as fills
    from genko import frames as geo
    from genko import selection, tones

    tone = {"pattern": str(op.get("pattern") or "dot")}
    if op.get("gradient"):
        tone["gradient"] = dict(op["gradient"])
    try:
        tones.validate(tone)
    except ValueError as exc:
        raise ApplyError(str(exc)) from exc
    layer = Layer(id=str(op.get("id") or new_id()), role=LayerRole.TONE, kind=LayerKind.TONE, lpi=60.0, density=0.3,
                  exportable=True, angle=45.0, tone=tone, title=str(op.get("name") or ""))
    if any(item.id == layer.id for item in page.layers):
        raise ApplyError(f"layer {layer.id} already exists")
    _tone_numbers(layer, op)
    patch = None
    if op.get("area"):
        area = _area(op)
        if area.get("poly"):
            patch = fills.polygon_patch(area["poly"], (0, 0, 0))
        else:
            mask, origin = selection.area_mask(area)
            patch = fills.mask_patch(mask, fills.FILL_DPI, (0, 0, 0), 1.0, origin)
    elif op.get("frame_id"):
        frame = _frame_or_fail(page, op["frame_id"])
        patch = fills.polygon_patch([list(p) for p in geo.shape(frame)], (0, 0, 0))
    elif op.get("at"):
        at = dict(op["at"])
        dpi = fills.FILL_DPI
        x, y = float(at["x_mm"]), float(at["y_mm"])
        reference = _fill_reference(episode, page, page.layers[0] if page.layers else layer, str(at.get("reference") or "page"), dpi)
        panel = page.frame_at(x, y)
        window = None
        if panel is not None:
            r = panel.rect
            window = (max(0, fills.px(r.x - 2, dpi)), max(0, fills.px(r.y - 2, dpi)),
                      min(reference.width, fills.px(r.x + r.width + 2, dpi)), min(reference.height, fills.px(r.y + r.height + 2, dpi)))
        mask = fills.region_mask(reference, (fills.px(x, dpi), fills.px(y, dpi)), gap_px=fills.px(float(at.get("gap_mm", 0.3) or 0), dpi),
                                 expand_px=1, window=window)
        if mask is None:
            raise ApplyError("nothing to fill there (the click is on a line)")
        patch = fills.mask_patch(mask, dpi, (0, 0, 0))
    if patch is not None:
        layer.patches.append(patch)
    elif any(op.get(k) for k in ("area", "frame_id", "at")):
        raise ApplyError("the area is empty")
    return layer


def _frame_or_fail(page, frame_id):
    try:
        return page._find(str(frame_id))
    except (KeyError, IndexError) as exc:
        raise ApplyError(f"no panel {frame_id}") from exc


def _effect(page, effect_id) -> dict:
    found = next((e for e in page.effects if e.get("id") == effect_id), None)
    if found is None:
        raise ApplyError(f"no effect {effect_id}")
    return found


def _items_box(items: dict):
    xs, ys = [], []
    for stroke in items.get("strokes", []):
        xs += [p[0] for p in stroke.points]
        ys += [p[1] for p in stroke.points]
    for patch in items.get("patches", []):
        x, y, w, h = (float(v) for v in patch["box"])
        xs += [x, x + w]
        ys += [y, y + h]
    if not xs:
        return None
    return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)


def _ruler(page, ruler_id) -> dict:
    found = next((r for r in page.rulers if r.get("id") == ruler_id), None)
    if found is None:
        raise ApplyError(f"no ruler {ruler_id}")
    return found


def _prim(page, prim_id) -> dict:
    found = next((p for p in page.prims if p.get("id") == prim_id), None)
    if found is None:
        raise ApplyError(f"no 3D figure or box {prim_id}")
    return found


def _vec3(value) -> list[float]:
    values = [float(v) for v in (list(value) + [0.0, 0.0, 0.0])[:3]]
    return [round(v, 4) for v in values]


def _frame_contains(page):
    from genko import frames as geo

    def inside(frame_id: str, x: float, y: float) -> bool:
        try:
            frame = page._find(frame_id)
        except (KeyError, IndexError):
            return False
        return geo.contains(frame, x, y)

    return inside


def _fill_reference(episode, page, target, reference: str, dpi: int):
    """What a fill looks at: the page as seen (every visible layer), only the target layer, or the layers
    marked as reference (参照レイヤー), each with the panel borders."""
    from PIL import Image, ImageDraw

    from genko import render

    if reference == "page":
        return render.render_page(page, dpi, mode="name" if not page.name_ok else "proof", episode=episode).convert("L")
    if reference not in ("layer", "reference"):
        raise ApplyError("reference must be page, layer or reference")
    looked = [target] if reference == "layer" else [layer for layer in page.layers if getattr(layer, "reference", False)]
    if not looked:
        raise ApplyError("no layer is set as the reference (set_layer reference: true)")
    size = (render.mm_to_px(page.spec.width_mm, dpi), render.mm_to_px(page.spec.height_mm, dpi))
    base = Image.new("RGBA", size, (255, 255, 255, 255))
    for layer in looked:
        raster = render._open_raster(layer)
        if raster is not None:
            base = Image.alpha_composite(base, raster.resize(size))
        lines = render._layer_strokes(layer, size, dpi, None, None)
        if lines is not None:
            base = Image.alpha_composite(base, lines)
    image = base.convert("RGB")
    render._draw_frames(ImageDraw.Draw(image), page, dpi)
    return image.convert("L")


def _untouched(stroke, eraser: list, radius: float) -> bool:
    """True when no point of the line comes within the eraser's radius (keep the line as it is)."""
    import math

    for p in stroke.points:
        for (ax, ay, *_), (bx, by, *_) in zip(eraser, eraser[1:] or eraser):
            dx, dy = bx - ax, by - ay
            seg = dx * dx + dy * dy
            t = 0.0 if seg == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / seg))
            if math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy)) <= radius:
                return False
    return True


STYLE_KEYS = {"font": str, "size_mm": float, "tracking": float, "leading": float, "align": str, "outline_mm": float,
              "rgb": list, "tcy": bool, "border_mm": float, "fill": str, "group": str, "rotate_deg": float, "skew_deg": float,
              "arc": float, "latin": str, "emphasis_mark": str, "bold": bool, "weight": str, "italic": bool, "outline_rgb": list,
              "wobble": float, "double": bool, "spikes": int, "spike_depth": float}


def _merge_style(current: dict, change) -> dict:
    """Merge style keys into a line's style; a key set to null goes back to the default."""
    if not isinstance(change, dict):
        raise ApplyError("style must be an object")
    out = dict(current or {})
    for key, value in change.items():
        if key not in STYLE_KEYS:
            raise ApplyError(f"unknown style key {key} (one of {', '.join(STYLE_KEYS)})")
        if value is None or value == "":
            out.pop(key, None)
            continue
        kind = STYLE_KEYS[key]
        try:
            value = [int(v) for v in value][:3] if kind is list else kind(value)
        except (TypeError, ValueError) as exc:
            raise ApplyError(f"style {key}: {exc}") from exc
        if key == "align" and value not in ("top", "center", "bottom", "left", "right"):
            raise ApplyError("align must be top, center, bottom, left or right")
        if key == "fill" and value not in ("white", "none"):
            raise ApplyError("fill must be white or none")
        if key == "latin" and value not in ("rotate", "upright"):
            raise ApplyError("latin must be rotate or upright")
        if key == "emphasis_mark" and value not in ("sesame", "dot"):
            raise ApplyError("emphasis_mark must be sesame or dot")
        if key == "weight" and value not in ("normal", "bold", "heavy"):
            raise ApplyError("weight must be normal, bold or heavy")
        if key in ("skew_deg",) and abs(value) > 60:
            raise ApplyError("skew_deg must be between -60 and 60")
        if key == "arc" and abs(value) > 1:
            raise ApplyError("arc must be between -1 and 1")
        if key == "wobble" and not 0 <= value <= 1:
            raise ApplyError("wobble must be between 0 and 1")
        if key == "spikes" and not 6 <= value <= 80:
            raise ApplyError("spikes must be between 6 and 80")
        if key == "spike_depth" and not 0.05 <= value <= 0.6:
            raise ApplyError("spike_depth must be between 0.05 and 0.6")
        out[key] = value
    return out


MASK_DPI = 150
GRADIENT_DPI = 150  # gradients are smooth: this is plenty, and keeps a whole-page one light
ROLE_TITLES = {"name": "ネーム", "draft": "下描き", "ink": "ペン入れ", "bg": "背景", "finish": "仕上げ"}


def _png(image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _mask_image(page, layer):
    """The layer's mask as an L image over the page (all white — everything shows — when it has none)."""
    size = (max(1, round(page.spec.width_mm / 25.4 * MASK_DPI)), max(1, round(page.spec.height_mm / 25.4 * MASK_DPI)))
    if layer.mask and layer.mask.get("png"):
        return Image.open(io.BytesIO(layer.mask["png"])).convert("L").resize(size)
    return Image.new("L", size, 255)


def _merge_down(episode, page, upper) -> None:
    """The layer into the one below it (same folder). Pen onto pen with nothing in between stays lines;
    anything else is drawn into pixels on the lower layer, as it showed."""
    from genko import raster as rasters
    from genko import render

    siblings = [item for item in page.layers if item.parent_id == upper.parent_id and item.kind != LayerKind.FOLDER]
    at = siblings.index(upper)
    if at == 0:
        raise ApplyError("there is no layer below to merge into")
    lower = siblings[at - 1]
    for item in (upper, lower):
        if item.kind in (LayerKind.PLACED, LayerKind.TONE) or getattr(item, "tone", None):
            raise ApplyError("placed images and tones cannot be merged")
        if getattr(item, "locked", False):
            raise ApplyError("the layer is locked")
    plain = (upper.kind == lower.kind == LayerKind.STROKES and not upper.raster_png and not lower.raster_png
             and not upper.mask and not lower.mask and (upper.blend or "normal") == "normal" and not upper.clip
             and float(upper.opacity if upper.opacity is not None else 1) >= 1 and upper.panel_clip == lower.panel_clip
             and upper.color == lower.color)
    if plain:
        lower.strokes.extend(upper.strokes)
        lower.patches.extend(upper.patches)
    else:
        dpi = rasters.WORKING_DPI
        below = render.layer_image(page, lower, dpi, episode)
        above = render.layer_image(page, upper, dpi, episode)
        clip = below.split()[3] if upper.clip else None
        opacity = float(upper.opacity if upper.opacity is not None else 1)
        merged = render._blend_over(below, above, upper.blend or "normal", opacity, clip)
        if (upper.blend or "normal") != "normal":
            # (a blend mode keeps the lower alpha; where only the upper layer is, it still shows)
            shown = ImageChops.multiply(above.split()[3], clip) if clip else above.split()[3]
            shown = shown.point(lambda v: int(v * opacity))
            merged.putalpha(ImageChops.lighter(below.split()[3], shown))
        lower.strokes, lower.patches, lower.mask = [], [], None
        lower.kind = LayerKind.RASTER
        rasters.save_raster(page, lower, merged)
    page.layers.remove(upper)


def _style_runs(raw) -> list:
    """[[words, {scale?, bold?, rgb?}]] — part of a line styled."""
    out = []
    for item in raw or []:
        if not isinstance(item, (list, tuple)) or len(item) < 2 or not item[0] or not isinstance(item[1], dict):
            raise ApplyError("style_runs is [[words, {scale, bold, rgb}], ...]")
        style = {}
        for key, value in item[1].items():
            if key == "scale":
                value = float(value)
                if not 0.3 <= value <= 3:
                    raise ApplyError("scale must be between 0.3 and 3")
                style["scale"] = value
            elif key == "bold":
                style["bold"] = bool(value)
            elif key == "weight":
                if value not in ("normal", "bold", "heavy"):
                    raise ApplyError("weight must be normal, bold or heavy")
                style["bold"] = {"normal": 0, "bold": True, "heavy": 2}[value]
            elif key == "rgb":
                style["rgb"] = [int(v) for v in value][:3]
            else:
                raise ApplyError(f"unknown style_runs key {key} (scale, bold, weight, rgb)")
        out.append([str(item[0]), style])
    return out


def _emphasis(raw) -> list[str]:
    if not isinstance(raw, (list, tuple)):
        raise ApplyError("emphasis_runs must be a list of the words that carry dots")
    return [str(item[0] if isinstance(item, (list, tuple)) else item) for item in raw if item]


def _set_path(line, raw) -> None:
    """A hand-drawn balloon: its outline, and the box becomes the outline's bounds."""
    if not raw:
        line.path = None
        return
    try:
        points = [(round(float(p[0]), 3), round(float(p[1]), 3)) for p in raw]
    except (TypeError, ValueError, IndexError) as exc:
        raise ApplyError("path must be [[x_mm, y_mm], ...]") from exc
    if len(points) < 3:
        raise ApplyError("a balloon outline needs at least three points")
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    if max(xs) - min(xs) < 2 or max(ys) - min(ys) < 2:
        raise ApplyError("the balloon outline is too small")
    line.path = points
    line.x_mm, line.y_mm = min(xs), min(ys)
    line.w_mm, line.h_mm = max(xs) - min(xs), max(ys) - min(ys)


def _parse_tails(raw) -> list[dict]:
    tails = []
    for item in raw or []:
        if isinstance(item, (list, tuple)):
            item = {"to": item}
        to = item.get("to") if isinstance(item, dict) else None
        if not to or len(to) < 2:
            raise ApplyError("a tail needs to: [x, y]")
        tail = {"to": [float(to[0]), float(to[1])]}
        if item.get("via"):
            tail["via"] = [float(item["via"][0]), float(item["via"][1])]
        if item.get("width_mm"):
            tail["width_mm"] = float(item["width_mm"])
        tails.append(tail)
    return tails


def _balloon_kind(kind) -> str:
    from genko.balloons import SHAPES

    kind = str(kind or "speech")
    if kind not in SHAPES:
        raise ApplyError(f"balloon must be one of {', '.join(SHAPES)}")
    return kind


def _layer_by_id(page, layer_id: str):
    layer = next((item for item in page.layers if item.id == layer_id), None)
    if layer is None:
        raise ApplyError(f"no layer {layer_id}")
    return layer


def _require_page(episode: Episode, op: dict[str, Any]) -> Page:
    try:
        index = int(op["page"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ApplyError("page (int) is required") from exc
    for page in episode.pages:
        if page.index == index:
            return page
    raise ApplyError(f"no page {index}")


UNDO_LIMIT = 50


def _copy_state(dst: Episode, src: Episode) -> None:
    """Move every field except the undo history from src into dst."""
    for f in dataclasses.fields(Episode):
        if f.name not in TRANSIENT_FIELDS:
            setattr(dst, f.name, getattr(src, f.name))


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
        target = page._find(str(frame_id))
        art = _placed_on(page, {target.id})
        if art and not op.get("force"):
            raise ApplyError(f"frame {target.id} has placed art; pass force to move it to studio.orphans")
        from genko import frames as geo

        ratio, gutter, tilt = float(op.get("ratio", 0.5)), float(op.get("gutter_mm", 4)), float(op.get("tilt_mm", 0) or 0)
        if target.children:
            raise ApplyError("can only split a leaf frame")
        if tilt or target.poly:
            p0, p1 = geo.axis_line(target, axis, ratio, gutter, tilt)
            try:
                a, b = geo.cut_frame(target, p0, p1, gutter, new_id)
            except ValueError as exc:
                raise ApplyError(str(exc)) from exc
        else:
            a, b = page.split_frame(frame_id, axis=axis, ratio=ratio, gutter_mm=gutter)
            geo.remember_split(target)
        if target.panel is not None:
            # the brief stays with the panel read first: top, or the binding-side column
            first = a if axis == "horizontal" or episode.binding != Binding.RIGHT else b
            first.panel, target.panel = target.panel, None
        _orphan_art(episode, page, art, reason=f"split {target.id}")
        return

    if name == "cut_frame":
        from genko import frames as geo

        page = _require_page(episode, op)
        try:
            p0 = (float(op["p0"][0]), float(op["p0"][1]))
            p1 = (float(op["p1"][0]), float(op["p1"][1]))
        except (KeyError, TypeError, ValueError, IndexError) as exc:
            raise ApplyError("p0 and p1 are [x, y] in mm") from exc
        frame_id = op.get("frame_id")
        if not frame_id:  # the panel under the middle of the cut
            under = page.frame_at((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2)
            if under is None:
                raise ApplyError("frame_id is required")
            frame_id = under.id
        target = page._find(str(frame_id))
        if target.children:
            raise ApplyError("can only split a leaf frame")
        if math.dist(p0, p1) < 1:
            raise ApplyError("the cut is too short")
        art = _placed_on(page, {target.id})
        if art and not op.get("force"):
            raise ApplyError(f"frame {target.id} has placed art; pass force to move it to studio.orphans")
        panel = target.panel
        try:
            a, b = geo.cut_frame(target, p0, p1, float(op.get("gutter_mm", 4)), new_id)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        if panel is not None:
            first = a if target.split_axis == "horizontal" or episode.binding != Binding.RIGHT else b
            first.panel, target.panel = panel, None
        _orphan_art(episode, page, art, reason=f"cut {target.id}")
        return

    if name == "move_gutter":
        from genko import frames as geo

        page = _require_page(episode, op)
        node = page._find(str(op.get("frame_id") or ""))
        if not node.children:
            raise ApplyError("frame_id must be a split (the parent of the panels on both sides)")
        try:
            geo.move_gutter(node, int(op.get("index", 0)), float(op.get("delta_mm", 0)),
                            None if op.get("gutter_mm") is None else float(op["gutter_mm"]))
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        return

    if name == "merge_frame":
        page = _require_page(episode, op)
        frame_id = op.get("frame_id") or page.selected_frame_id
        if not frame_id:
            raise ApplyError("frame_id is required")
        parent = page.parent_of(str(frame_id))
        if parent is None:
            raise ApplyError("cannot merge the root frame")
        leaves = _leaves_in_reading_order(parent, episode.binding)
        art = _placed_on(page, {leaf.id for leaf in leaves})
        if art and not op.get("force"):
            raise ApplyError(f"panels under {parent.id} have placed art; pass force to move it to studio.orphans")
        panels = [leaf.panel for leaf in leaves if leaf.panel]
        page.merge_frame(str(frame_id))
        if panels:
            parent.panel = panels[0]
            if len(panels) > 1:
                episode.studio.setdefault("orphans", []).append(
                    {"kind": "panels", "page_id": page.id, "frame_id": parent.id, "panels": panels[1:], "rev": episode.revision}
                )
        _orphan_art(episode, page, art, reason=f"merge into {parent.id}")
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
        if "poly" in op:
            from genko import frames as geo

            if frame.children:
                raise ApplyError("only a panel (not a split) takes a shape")
            if op["poly"]:
                points = [(float(p[0]), float(p[1])) for p in op["poly"]]
                if len(points) < 3 or geo.area(points) < 4:
                    raise ApplyError("a shape needs at least three corners around some area")
                geo.set_shape(frame, points)
                frame.custom = True
            else:
                frame.custom = False
                parent = page.parent_of(frame.id)
                if parent is not None:
                    geo.relayout(parent)
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
        if op.get("id"):
            if any(item.id == str(op["id"]) and item is not line for item in episode.story):
                raise ApplyError(f"line {op['id']} already exists")
            line.id = str(op["id"])
        if "wrap" in op:
            line.wrap = str(op["wrap"])
        if op.get("ruby_runs"):
            line.ruby_runs = [tuple(item) for item in op["ruby_runs"]]
        if op.get("emphasis_runs"):
            line.emphasis_runs = _emphasis(op["emphasis_runs"])
        if op.get("style_runs"):
            line.style_runs = _style_runs(op["style_runs"])
        if op.get("path"):
            _set_path(line, op["path"])
        if op.get("style"):
            line.style = _merge_style({}, op["style"])
        if op.get("tails"):
            line.tails = _parse_tails(op["tails"])
            line.tail = tuple(line.tails[0]["to"])
        line.balloon = _balloon_kind(line.balloon)
        return

    if name == "reorder_lines":
        page = _require_page(episode, op)
        order = [str(item) for item in op.get("order") or []]
        mine = [line for line in episode.story if line.page_index == page.index]
        if sorted(order) != sorted(line.id for line in mine):
            raise ApplyError("order must list every line of the page exactly once")
        by_id = {line.id: line for line in mine}
        others = iter([by_id[i] for i in order])
        episode.story = [next(others) if line.page_index == page.index else line for line in episode.story]
        return

    if name == "edit_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        line = _find_line(episode, str(line_id))
        if "style" in op:
            line.style = _merge_style(line.style, op["style"])
        if "tails" in op:
            line.tails = _parse_tails(op["tails"])
            line.tail = tuple(line.tails[0]["to"]) if line.tails else None
        if "text" in op:
            line.text = str(op["text"])
        if "speaker" in op:
            line.speaker = str(op["speaker"])
        if "frame_id" in op:
            line.frame_id = op["frame_id"]
        if "ruby" in op:
            line.ruby = str(op["ruby"])
        if "ruby_runs" in op:
            line.ruby_runs = [tuple(item) for item in op["ruby_runs"] or []]
        if "emphasis_runs" in op:
            line.emphasis_runs = _emphasis(op["emphasis_runs"])
        if "style_runs" in op:
            line.style_runs = _style_runs(op["style_runs"])
        if "balloon" in op:
            line.balloon = _balloon_kind(op["balloon"])
        if "wrap" in op:
            if op["wrap"] not in ("vertical", "horizontal"):
                raise ApplyError("wrap must be vertical or horizontal")
            line.wrap = str(op["wrap"])
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
            line.tails = [{"to": list(line.tail)}] if line.tail else []
        if "tails" in op:
            line.tails = _parse_tails(op["tails"])
            line.tail = tuple(line.tails[0]["to"]) if line.tails else None
        if "balloon" in op:
            line.balloon = _balloon_kind(op["balloon"])
        return

    if name == "delete_line":
        line_id = op.get("id")
        if not line_id:
            raise ApplyError("id is required")
        before = len(episode.story)
        episode.story = [line for line in episode.story if line.id != line_id]
        for page in episode.pages:
            if any(line.id == line_id for line in page.texts):
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
        page, points = _stroke_target(episode, page, points, str(op.get("space") or "page"))
        stabilize = op.get("stabilize", episode.brush_stabilize)
        if stabilize:
            from genko.stroke import stabilize_points

            points = stabilize_points(points, int(stabilize))
        copies: list = []
        if op.get("snap_ruler") or op.get("ruler_id"):
            if page.rulers:
                from genko import rulers as guides

                inside = _frame_contains(page)
                points = [tuple(p) for p in guides.snap(points, page.rulers, inside, only=op.get("ruler_id"))]
                copies = [[tuple(p) for p in c] for c in guides.symmetry_copies(points, page.rulers, inside)]
            else:
                points = _snap_points(page, points)
        taper = op["taper"] if "taper" in op else episode.brush_taper
        if taper:
            from genko.stroke import taper_points

            points = taper_points(points)
        if op.get("pressure_gamma"):
            gamma = max(0.2, min(5.0, float(op["pressure_gamma"])))
            points = [[p[0], p[1], max(0.0, min(1.0, float(p[2]))) ** gamma] if len(p) > 2 else p for p in points]
        curve = str(op.get("curve") or episode.brush_curve or "linear")
        if curve and curve != "linear":
            from genko.stroke import apply_pressure_curve

            points = apply_pressure_curve(points, curve)
        from genko.models import coerce_stroke, stroke_points

        stroke = coerce_stroke(points)
        # a micrometre is finer than any pen; fewer digits keep a thick book quick to save and open
        stroke.points = [(round(x, 3), round(y, 3)) for x, y in stroke.points]
        stroke.pressure = [round(v, 3) for v in stroke.pressure]
        stroke.kind = _brush_kind(op.get("kind") or "gpen", episode)
        stroke.width_mm = float(op["width_mm"]) if op.get("width_mm") is not None else float(episode.brush_width_mm)
        if op.get("layer_id"):
            target = _layer_by_id(page, str(op["layer_id"]))
            if target.kind not in (LayerKind.STROKES, LayerKind.RASTER, LayerKind.TONE):
                raise ApplyError("this layer cannot take pen lines (choose a pen, paint or tone layer)")
        else:
            role = LayerRole.INK if layer_name == "ink" else LayerRole.NAME if layer_name == "name" else LayerRole(layer_name)
            target = page._layer(role)
        if getattr(target, "locked", False):
            raise ApplyError("the layer is locked")
        if target.role == LayerRole.INK and not page.name_ok and _gated(episode):
            raise ApplyError("ink strokes require name_ok")
        rgb = op.get("rgb") or (episode.brush_rgb if tuple(episode.brush_rgb) != (20, 20, 20) else None)
        stroke.rgb = tuple(int(v) for v in rgb) if rgb else None
        if op.get("opacity") is not None:
            stroke.opacity = max(0.0, min(1.0, float(op["opacity"])))
        # lines stay vectors: they are drawn at the resolution of each render (no baking)
        target.strokes.append(stroke)
        for copy_points in copies:  # symmetry rulers draw the line again
            twin = copy.deepcopy(stroke)
            twin.id = new_id()
            twin.points = [(float(p[0]), float(p[1])) for p in copy_points]
            target.strokes.append(twin)
        return

    if name == "fill":
        from genko import fill as fills

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        dpi = fills.FILL_DPI
        at = (fills.px(float(op["x_mm"]), dpi), fills.px(float(op["y_mm"]), dpi))
        reference = _fill_reference(episode, page, target, str(op.get("reference") or "page"), dpi)
        panel = page.frame_at(float(op["x_mm"]), float(op["y_mm"]))
        window = None
        if panel is not None:  # search only the clicked panel's box (and a little around it)
            r = panel.rect
            window = (max(0, fills.px(r.x - 2, dpi)), max(0, fills.px(r.y - 2, dpi)),
                      min(reference.width, fills.px(r.x + r.width + 2, dpi)), min(reference.height, fills.px(r.y + r.height + 2, dpi)))
        mask = fills.region_mask(reference, at, gap_px=fills.px(float(op.get("gap_mm", 0.3) or 0), dpi),
                                 expand_px=max(1, fills.px(float(op.get("expand_mm", 0.15) or 0), dpi)), window=window)
        if mask is None:
            raise ApplyError("nothing to fill there (the click is on a line)")
        patch = fills.mask_patch(mask, dpi, _rgb(op, episode), float(op.get("opacity", 1.0)))
        target.patches.append(patch)
        return

    if name == "fill_area":
        from genko import fill as fills
        from genko import selection

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        area = _area(op)
        if area.get("poly"):
            patch = fills.polygon_patch(area["poly"], _rgb(op, episode), float(op.get("opacity", 1.0)))
        else:
            mask, origin = selection.area_mask(area)
            patch = fills.mask_patch(mask, fills.FILL_DPI, _rgb(op, episode), float(op.get("opacity", 1.0)), origin)
        if patch is None:
            raise ApplyError("the area is empty")
        target.patches.append(patch)
        return

    if name in ("transform_area", "delete_area"):
        from genko import selection

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        area = _area(op)
        items = selection.lift(target, area, page)
        if name == "delete_area":
            return
        if op.get("warp"):
            from genko import warp

            try:
                go = warp.mapping(selection.area_bbox(area), op["warp"])
                selection.drop_warped(target, items, go)
            except warp.WarpError as exc:
                raise ApplyError(str(exc)) from exc
            return
        matrix = tuple(float(v) for v in op.get("matrix") or selection.IDENTITY)
        if len(matrix) != 6:
            raise ApplyError("matrix is [a, b, c, d, e, f]")
        try:
            selection.drop(target, items, matrix)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        return

    if name == "paste":
        from genko import selection

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        items = selection.items_from_json(op.get("items") or {})
        if not items["strokes"] and not items["patches"]:
            raise ApplyError("nothing to paste")
        matrix = tuple(float(v) for v in op.get("matrix") or selection.IDENTITY)
        selection.drop(target, items, matrix, fresh_ids=True)
        return

    if name == "set_stroke_width":
        from genko import selection

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        area = op.get("area")
        ids = set(op.get("ids") or [])
        changed = 0
        for stroke in target.strokes:
            if (ids and stroke.id in ids) or (area and selection.stroke_inside(stroke, area)):
                if op.get("width_mm") is not None:
                    stroke.width_mm = max(0.05, float(op["width_mm"]))
                if op.get("scale") is not None:
                    stroke.width_mm = max(0.05, stroke.width_mm * float(op["scale"]))
                if op.get("kind"):
                    stroke.kind = _brush_kind(op["kind"], episode)
                if op.get("rgb"):
                    stroke.rgb = tuple(int(v) for v in op["rgb"])
                changed += 1
        if not changed:
            raise ApplyError("no line there")
        return

    if name == "reshape_stroke":
        page = _require_page(episode, op)
        target = _paint_target(page, op)
        stroke = next((item for item in target.strokes if item.id == op.get("stroke_id")), None)
        if stroke is None:
            raise ApplyError(f"no line {op.get('stroke_id')}")
        if op.get("points"):
            points = _parse_points(op["points"])
            if len(points) < 2:
                raise ApplyError("points needs at least two [x_mm, y_mm] pairs")
            from genko.models import coerce_stroke

            new = coerce_stroke(points)
            stroke.points, stroke.pressure = new.points, new.pressure or stroke.pressure[:len(new.points)]
            if stroke.pressure and len(stroke.pressure) != len(stroke.points):
                stroke.pressure = []
        if op.get("width_mm") is not None:
            stroke.width_mm = max(0.05, float(op["width_mm"]))
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
        _verify_image(blob)
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
        if "locked" in op:
            layer.locked = bool(op["locked"])
        if "panel_clip" in op:
            layer.panel_clip = bool(op["panel_clip"])
        if "title" in op:
            layer.title = str(op["title"] or "")
        if "parent" in op:
            layer.parent_id = op.get("parent")
        if "name" in op:
            layer.title = str(op["name"])
        if "color" in op:
            layer.color = tuple(int(v) for v in op["color"])[:3] if op["color"] else None
        if "reference" in op:
            layer.reference = bool(op["reference"])
        return

    if name == "gradient_fill":
        from genko import selection

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        try:
            (fx, fy), (tx, ty) = [float(v) for v in op["from"][:2]], [float(v) for v in op["to"][:2]]
        except (KeyError, TypeError, ValueError) as exc:
            raise ApplyError("gradient_fill needs from and to: [x_mm, y_mm]") from exc
        if math.hypot(tx - fx, ty - fy) < 0.5:
            raise ApplyError("the gradient needs a longer drag")
        dpi = GRADIENT_DPI
        if op.get("area"):
            shown, (x0, y0) = selection.area_mask(_area(op), dpi)
        else:
            x0 = y0 = 0
            shown = Image.new("L", (round(page.spec.width_mm / 25.4 * dpi), round(page.spec.height_mm / 25.4 * dpi)), 255)
        import numpy as np

        w, h = shown.size
        scale = dpi / 25.4
        xs = (np.arange(w) + x0 + 0.5) / scale
        ys = (np.arange(h) + y0 + 0.5) / scale
        gx, gy = np.meshgrid(xs, ys)
        if op.get("shape") == "radial":
            t = np.hypot(gx - fx, gy - fy) / math.hypot(tx - fx, ty - fy)
        else:
            dx, dy = tx - fx, ty - fy
            t = ((gx - fx) * dx + (gy - fy) * dy) / (dx * dx + dy * dy)
        t = np.clip(t, 0.0, 1.0)
        c0 = np.array([int(v) for v in (op.get("rgb_from") or [20, 20, 20])][:3], dtype=float)
        c1 = np.array([int(v) for v in (op.get("rgb_to") or op.get("rgb_from") or [20, 20, 20])][:3], dtype=float)
        a0 = max(0.0, min(1.0, float(op.get("opacity_from", 1.0))))
        a1 = max(0.0, min(1.0, float(op.get("opacity_to", 0.0 if not op.get("rgb_to") else 1.0))))
        rgb = (c0[None, None, :] * (1 - t[..., None]) + c1[None, None, :] * t[..., None]).round().astype("uint8")
        alpha = ((a0 * (1 - t) + a1 * t) * np.asarray(shown, dtype=float)).round().astype("uint8")
        image = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")
        patch = selection._to_patch(image, (x0, y0), {"mode": "image", "opacity": 1.0}, dpi)
        if patch is None:
            raise ApplyError("the gradient has nothing to show there")
        target.patches.append(patch)
        return

    if name == "define_brush":
        from genko import brushes

        key = str(op.get("key") or "")
        if not key.startswith("my_") or len(key) > 40:
            raise ApplyError("a brush of one's own has a key starting with my_")
        if op.get("delete"):
            episode.brush_custom.pop(key, None)
            return
        data = {k: v for k, v in op.items() if k not in ("op", "key", "delete")}
        try:
            made = brushes.from_dict(key, data)
        except (ValueError, TypeError) as exc:
            raise ApplyError(str(exc)) from exc
        episode.brush_custom[key] = brushes.to_dict(made)
        brushes.CUSTOM[key] = made
        return

    if name == "duplicate_layer":
        page = _require_page(episode, op)
        source = _layer_by_id(page, str(op.get("id") or ""))
        if source.kind == LayerKind.FOLDER:
            raise ApplyError("a folder cannot be duplicated")
        twin = copy.deepcopy(source)
        twin.id = str(op.get("new_id") or new_id())
        if any(item.id == twin.id for item in page.layers):
            raise ApplyError(f"layer {twin.id} exists")
        if source.kind != LayerKind.PLACED:
            twin.role = LayerRole.USER  # the copy is an ordinary layer (a book has one ink layer, one name layer…)
        twin.title = f"{source.title or ROLE_TITLES.get(source.role.value, 'レイヤー')} のコピー"
        for stroke in twin.strokes:
            stroke.id = new_id()
        for patch in twin.patches:
            patch["id"] = new_id()
        page.layers.insert(page.layers.index(source) + 1, twin)
        return

    if name == "merge_down":
        page = _require_page(episode, op)
        upper = _layer_by_id(page, str(op.get("id") or ""))
        _merge_down(episode, page, upper)
        return

    if name == "set_layer_mask":
        page = _require_page(episode, op)
        layer = _layer_by_id(page, str(op.get("id") or ""))
        if layer.kind in (LayerKind.FOLDER, LayerKind.PLACED):
            raise ApplyError("this layer cannot take a mask (a folder or a placed image)")
        if op.get("delete"):
            layer.mask = None
            return
        image = _mask_image(page, layer)
        if op.get("area"):
            from genko import selection

            shown, (x0, y0) = selection.area_mask(op["area"], MASK_DPI)
            image = Image.new("L", image.size, 0)
            image.paste(shown, (x0, y0))
        elif op.get("fill"):
            if op["fill"] not in ("show", "hide"):
                raise ApplyError("fill must be show or hide")
            image = Image.new("L", image.size, 255 if op["fill"] == "show" else 0)
        if op.get("invert"):
            image = ImageOps.invert(image)
        enabled = bool(op["enabled"]) if "enabled" in op else bool((layer.mask or {}).get("enabled", True))
        layer.mask = {"png": _png(image), "enabled": enabled}
        return

    if name == "paint_mask":
        page = _require_page(episode, op)
        layer = _layer_by_id(page, str(op.get("id") or ""))
        if layer.kind in (LayerKind.FOLDER, LayerKind.PLACED):
            raise ApplyError("this layer cannot take a mask (a folder or a placed image)")
        points = _parse_points(op.get("points") or [])
        if not points:
            raise ApplyError("points needs at least one [x_mm, y_mm] pair")
        from genko.stroke import draw_stroke_mm

        image = _mask_image(page, layer)
        if len(points) == 1:
            points = [points[0], [points[0][0] + 0.01, points[0][1] + 0.01]]
        flat = [[p[0], p[1]] for p in points]  # the mask takes the whole width, whatever the pressure
        draw_stroke_mm(ImageDraw.Draw(image), flat, MASK_DPI, float(op.get("width_mm") or 3.0), 255 if op.get("show", True) else 0,
                       pressure_scale=False)
        layer.mask = {"png": _png(image), "enabled": bool((layer.mask or {}).get("enabled", True))}
        return

    if name == "add_page":
        count = int(op.get("count", 1))
        if not 1 <= count <= 200:
            raise ApplyError("count is 1 to 200")
        after = op.get("after")
        if after is not None and not any(p.index == int(after) for p in episode.pages) and int(after) != 0:
            raise ApplyError(f"no page {after}")
        first_new = len(episode.pages) + 1
        for _ in range(count):
            index = len(episode.pages) + 1
            page = Page(index=index, spec=episode.spec, frames=[], binding=episode.binding)
            page.frames = [Frame(id=new_id(), rect=page.inner_rect_mm())]
            episode.pages.append(page)
        if after is not None and int(after) < first_new - 1:
            old = [p.index for p in episode.pages]
            fresh = old[first_new - 1:]
            kept = old[:first_new - 1]
            order = kept[:int(after)] + fresh + kept[int(after):]
            _reorder(episode, order)
        return

    if name == "set_page_spec":
        from genko import pagespec

        try:
            spec = pagespec.spec_from(op, episode.spec)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        pagespec.relayout(episode, spec, move=op.get("move", True) is not False)
        return

    if name == "set_nombre":
        from genko import nombre

        if op.get("page") is not None and "numero" in op:
            _require_page(episode, op).numero = bool(op["numero"])
        change = {k: op[k] for k in ("position", "font", "size_mm", "start", "hidden", "hidden_size_mm", "show") if k in op}
        try:
            nombre.validate(change)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        episode.nombre = {**episode.nombre, **change}
        return

    if name == "delete_page":
        page = _require_page(episode, op)
        if len(episode.pages) == 1:
            raise ApplyError("cannot delete the last page")
        removed = page.index
        episode.pages = [item for item in episode.pages if item.index != removed]
        episode.story = [line for line in episode.story if line.page_index != removed]
        episode.page_locks.pop(page.id, None)
        mapping: dict[int, int | None] = {item.index: new for new, item in enumerate(episode.pages, start=1)}
        mapping[removed] = None
        remap_page_refs(episode, mapping)
        return

    if name == "duplicate_page":
        page = _require_page(episode, op)
        clone: Page = copy.deepcopy(page)
        clone.index = len(episode.pages) + 1
        clone.id = "pg_" + new_id()
        clone.spread_with = None
        frame_map: dict[str, str] = {}
        for frame in clone.frames:
            _refresh_frame_ids(frame, frame_map)
        if clone.selected_frame_id:
            clone.selected_frame_id = frame_map.get(clone.selected_frame_id)
        for layer in clone.layers:
            layer.id = new_id()
            if layer.frame_id:
                layer.frame_id = frame_map.get(layer.frame_id, layer.frame_id)
        new_lines: list[StoryLine] = []
        for line in list(episode.story_for_page(page.index)):
            copied = copy.deepcopy(line)
            copied.id = new_id()
            copied.page_index = clone.index
            if copied.frame_id:
                copied.frame_id = frame_map.get(copied.frame_id, copied.frame_id)
            new_lines.append(copied)
        clone.texts = new_lines
        episode.story.extend(new_lines)
        episode.pages.append(clone)
        if op.get("next_to") and page.index < len(episode.pages) - 1:
            order = [p.index for p in episode.pages[:-1]]
            order.insert(page.index, clone.index)
            _reorder(episode, order)
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
            for page in episode.pages:
                page.binding = episode.binding
        if "start_side" in op:
            if op["start_side"] not in (None, "left", "right"):
                raise ApplyError("start_side must be left, right or null")
            episode.start_side = op["start_side"]
        if "strict_gates" in op:
            episode.strict_gates = bool(op["strict_gates"])
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
        if other in (None, "", 0):
            page.spread_with = None
            return
        partner = next((item for item in episode.pages if item.index == int(other)), None)
        if partner is None:
            raise ApplyError(f"no page {other}")
        problem = facing_problem(episode, page, partner)
        if problem and episode.strict_gates:
            raise ApplyError(problem)
        page.spread_with = partner.index
        return

    if name == "reorder":
        order = op.get("order")
        if not isinstance(order, list) or not order:
            raise ApplyError("order must be a non-empty list of page indexes")
        order = [int(i) for i in order]
        if sorted(order) != sorted(p.index for p in episode.pages):
            raise ApplyError("order must list every page exactly once")
        _reorder(episode, order)
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
        layer = _new_tone(episode, page, op)
        if op.get("after"):
            index = next((i for i, item in enumerate(page.layers) if item.id == op["after"]), None)
            if index is None:
                raise ApplyError(f"no layer {op['after']}")
            page.layers.insert(index + 1, layer)
        else:
            page.layers.append(layer)
        return

    if name == "set_tone":
        from genko import tones

        page = _require_page(episode, op)
        layer = _layer_by_id(page, str(op.get("id")))
        if layer.kind != LayerKind.TONE and layer.role != LayerRole.TONE:
            raise ApplyError("that layer is not a tone")
        tone = dict(layer.tone or {})
        if op.get("pattern"):
            tone["pattern"] = str(op["pattern"])
        if "gradient" in op:
            tone["gradient"] = dict(op["gradient"]) if op["gradient"] else None
        try:
            tones.validate(tone)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        layer.tone = tone
        _tone_numbers(layer, op)
        if op.get("name"):
            layer.title = str(op["name"])
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
        from genko import effects

        page = _require_page(episode, op)
        kind = str(op.get("kind") or "")
        params = dict(op.get("params") or {})
        try:
            effects.validate(kind, params)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        if op.get("frame_id"):
            _frame_or_fail(page, op["frame_id"])
        page.effects.append({"id": str(op.get("id") or new_id()), "kind": kind, "frame_id": op.get("frame_id"), "params": params})
        return

    if name == "edit_effect":
        from genko import effects

        page = _require_page(episode, op)
        effect = _effect(page, op.get("id"))
        params = dict(effect.get("params") or {})
        for key, value in dict(op.get("params") or {}).items():
            if value is None:
                params.pop(key, None)
            else:
                params[key] = value
        kind = str(op.get("kind") or effect["kind"])
        try:
            effects.validate(kind, params)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        if "frame_id" in op:
            if op["frame_id"]:
                _frame_or_fail(page, op["frame_id"])
            effect["frame_id"] = op["frame_id"] or None
        if "visible" in op:
            effect["visible"] = bool(op["visible"])
        effect["kind"], effect["params"] = kind, params
        return

    if name == "delete_effect":
        page = _require_page(episode, op)
        _effect(page, op.get("id"))
        page.effects = [e for e in page.effects if e.get("id") != op.get("id")]
        return

    if name == "effect_to_layer":
        from genko import effects

        page = _require_page(episode, op)
        effect = _effect(page, op.get("id"))
        target = _paint_target(page, op)
        effects.to_layer(effect, page, target)
        if not op.get("keep"):
            page.effects = [e for e in page.effects if e.get("id") != op.get("id")]
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

    if name in ("add_ruler", "edit_ruler"):
        from genko import rulers as guides

        page = _require_page(episode, op)
        if name == "add_ruler":
            ruler = {"id": str(op.get("id") or new_id()), "kind": str(op.get("kind") or ""), "points": [], "active": True, "visible": True}
            if any(r.get("id") == ruler["id"] for r in page.rulers):
                raise ApplyError(f"ruler {ruler['id']} already exists")
        else:
            ruler = copy.deepcopy(_ruler(page, op.get("id")))
        for key in ("angle", "ratio", "reach_mm"):
            if op.get(key) is not None:
                ruler[key] = float(op[key])
        if op.get("copies") is not None:
            ruler["copies"] = int(op["copies"])
        for key in ("mirror", "active", "visible"):
            if key in op:
                ruler[key] = bool(op[key])
        if "points" in op:
            ruler["points"] = [[round(float(p[0]), 3), round(float(p[1]), 3)] for p in op.get("points") or []]
        if "frame_id" in op:
            if op["frame_id"]:
                try:
                    page._find(str(op["frame_id"]))
                except (KeyError, IndexError) as exc:
                    raise ApplyError(f"no panel {op['frame_id']}") from exc
            ruler["frame_id"] = op["frame_id"] or None
        try:
            guides.validate(ruler)
        except ValueError as exc:
            raise ApplyError(str(exc)) from exc
        if name == "add_ruler":
            page.rulers.append(ruler)
        else:
            page.rulers = [ruler if r.get("id") == ruler["id"] else r for r in page.rulers]
        return

    if name == "delete_ruler":
        page = _require_page(episode, op)
        if op.get("id"):
            _ruler(page, op["id"])
            page.rulers = [r for r in page.rulers if r.get("id") != op["id"]]
        else:
            page.rulers = []
            page.ruler = None
        return

    if name == "add_prim3d":
        page = _require_page(episode, op)
        kind = str(op.get("kind") or "box")
        if kind not in ("box", "cylinder", "stairs", "floor"):
            raise ApplyError("kind must be box, cylinder, stairs or floor (figures: add_mannequin)")
        size = op.get("size") or ([160, 1, 160] if kind == "floor" else [40, 40, 40])
        if not isinstance(size, (list, tuple)):
            size = [float(size)] * 3
        prim = {"id": str(op.get("id") or new_id()), "kind": kind, "pos": _vec3(op.get("pos") or [100, 150, 0]),
                "size": _vec3(size), "rot": _vec3(op.get("rot") or [0.35, 0.6, 0])}
        if op.get("focal_mm"):
            prim["focal_mm"] = max(20.0, float(op["focal_mm"]))
        if kind == "stairs":
            prim["steps"] = max(2, min(30, int(op.get("steps") or 6)))
        if kind == "floor":
            prim["lines"] = max(2, min(40, int(op.get("lines") or 8)))
            if not op.get("rot"):
                prim["rot"] = [-1.2, 0.5, 0]  # seen from above at a slant
        page.prims.append(prim)
        return

    if name == "add_scene":
        from genko import prim3d

        page = _require_page(episode, op)
        kind = str(op.get("kind") or "room")
        if kind not in prim3d.SCENES:
            raise ApplyError(f"scene kind must be one of {', '.join(prim3d.SCENES)}")
        spec = page.spec
        size = op.get("size") or prim3d.SCENE_SIZES[kind]
        if not isinstance(size, (list, tuple)):
            size = [float(size) * v / prim3d.SCENE_SIZES[kind][0] for v in prim3d.SCENE_SIZES[kind]]
        size = _vec3(size)
        focal = max(20.0, float(op.get("focal_mm") or 220))
        rot, near = prim3d.SCENE_VIEWS[kind]
        # (depth: the scene's near end sits `near` × focal from the camera plane; inside for corridors)
        depth = size[2] / 2 + near * focal
        prim = {"id": str(op.get("id") or new_id()), "kind": "scene", "scene": kind,
                "pos": _vec3(op.get("pos") or [spec.width_mm / 2, spec.height_mm / 2, depth]), "size": size,
                "rot": _vec3(op.get("rot") or rot), "focal_mm": focal}
        page.prims.append(prim)
        return

    if name == "edit_prim":
        page = _require_page(episode, op)
        prim = _prim(page, op.get("id"))
        for key in ("pos", "size", "rot"):
            if op.get(key) is not None:
                prim[key] = _vec3(op[key])
        if op.get("focal_mm"):
            prim["focal_mm"] = max(20.0, float(op["focal_mm"]))
        if prim.get("kind") == "mannequin" and op.get("size") is not None:
            height = prim["size"][1]
            prim["size"] = [height / 2, height, height / 4]
        return

    if name == "delete_prim":
        page = _require_page(episode, op)
        _prim(page, op.get("id"))
        page.prims = [p for p in page.prims if p.get("id") != op.get("id")]
        return

    if name == "trace_prims":
        from genko import prim3d
        from genko.models import coerce_stroke

        page = _require_page(episode, op)
        target = _paint_target(page, op)
        ids = set(op.get("ids") or [])
        chosen = [p for p in page.prims if not ids or p.get("id") in ids]
        if not chosen:
            raise ApplyError("no 3D figure or box to trace")
        kind = _brush_kind(op.get("kind") or "pencil", episode)
        for prim in chosen:
            for line in prim3d.trace(prim):
                stroke = coerce_stroke([(round(x, 3), round(y, 3)) for x, y in line])
                stroke.kind = kind
                stroke.width_mm = float(op.get("width_mm") or 0.3)
                if op.get("rgb"):
                    stroke.rgb = tuple(int(v) for v in op["rgb"])
                target.strokes.append(stroke)
        return

    if name == "lt_convert":
        _lt_convert(episode, op)
        return

    if name == "add_ticket":
        page = _require_page(episode, op)
        episode.tickets.append(
            {
                "id": str(op.get("id") or new_id()),
                "page_index": page.index,
                "page_id": page.id,
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

    if name in ("erase_raster", "erase"):
        page = _require_page(episode, op)
        if op.get("layer_id"):
            target = _layer_by_id(page, str(op["layer_id"]))
        else:
            target = page._layer(LayerRole(str(op.get("layer") or "ink")))
        if getattr(target, "locked", False):
            raise ApplyError("the layer is locked")
        points = _parse_points(op.get("points") or [])
        width = float(op.get("width_mm", 2))
        if target.kind == LayerKind.TONE:  # on a tone the eraser scrapes (削り); soft fades it out
            from genko.models import coerce_stroke

            scrape = coerce_stroke(points if len(points) > 1 else [*points, (points[0][0] + 0.01, points[0][1] + 0.01)])
            scrape.kind = "scrape_soft" if op.get("soft") else "scrape"
            scrape.width_mm = width
            target.strokes.append(scrape)
            return
        if op.get("mode") == "to_crossing":
            from genko.stroke import erase_to_crossing

            target.strokes = erase_to_crossing(target.strokes, points, width / 2)
            return
        if target.strokes:
            from genko.models import coerce_stroke, stroke_points
            from genko.stroke import split_by_eraser

            kept = []
            for stroke in target.strokes:
                pieces = split_by_eraser(stroke_points(stroke), points, width / 2)
                if len(pieces) == 1 and len(pieces[0]) >= len(stroke.points) and _untouched(stroke, points, width / 2):
                    kept.append(stroke)
                    continue
                for piece in pieces:
                    part = coerce_stroke(piece)
                    part.width_mm, part.kind, part.rgb, part.opacity = stroke.width_mm, stroke.kind, stroke.rgb, stroke.opacity
                    kept.append(part)
            target.strokes = kept
        if target.raster_png:
            from genko.raster import erase_raster

            erase_raster(page, target, points, width_mm=width)
        return

    if name == "reorder_layers":
        page = _require_page(episode, op)
        order = op.get("order") or []
        by_id = {layer.id: layer for layer in page.layers}
        page.layers = [by_id[item] for item in order if item in by_id]
        return

    if name == "stamp_material":
        page = _require_page(episode, op)
        from genko.materials import get_material, image_bytes

        try:
            material = get_material(str(op.get("material_id")))
        except KeyError as exc:
            raise ApplyError(f"no material {op.get('material_id')}") from exc
        kind = material.get("kind")
        if kind == "tone":
            tone_op = {**(material.get("tone") or {}), **{k: op[k] for k in ("frame_id", "area", "at", "after", "id") if op.get(k)}}
            layer = _new_tone(episode, page, {**tone_op, "name": material.get("name")})
            layer.material_id = material["id"]
            page.layers.append(layer)
            return
        if kind == "effect":
            params = dict(material.get("params") or {})
            if op.get("x_mm") is not None and op.get("y_mm") is not None:
                params["center"] = [float(op["x_mm"]), float(op["y_mm"])]
            frame_id = op.get("frame_id")
            if frame_id:
                _frame_or_fail(page, frame_id)
            page.effects.append({"id": str(op.get("id") or new_id()), "kind": material.get("effect", "speed"),
                                 "frame_id": frame_id, "params": params})
            return
        target = _paint_target(page, op)
        if kind == "image":
            data = image_bytes(material)
            if not data:
                raise ApplyError("the picture file of this material is missing")
            width = float(op.get("width_mm") or material.get("width_mm") or 60)
            height = width * float(material.get("aspect") or 1)
            cx = float(op.get("x_mm", page.spec.width_mm / 2))
            cy = float(op.get("y_mm", page.spec.height_mm / 2))
            target.patches.append({"id": new_id(), "box": [round(cx - width / 2, 3), round(cy - height / 2, 3), round(width, 3), round(height, 3)],
                                   "mode": "image", "png": data, "opacity": 1.0})
            return
        if kind == "lines":
            from genko import selection

            items = selection.items_from_json(material.get("items") or {})
            matrix = selection.IDENTITY
            if op.get("x_mm") is not None and op.get("y_mm") is not None:
                box = _items_box(items)
                if box:
                    matrix = (1.0, 0.0, 0.0, 1.0, float(op["x_mm"]) - (box[0] + box[2] / 2), float(op["y_mm"]) - (box[1] + box[3] / 2))
            selection.drop(target, items, matrix, fresh_ids=True)
            return
        raise ApplyError(f"unknown material kind {kind}")

    if name == "set_balloon_path":
        line = _find_line(episode, str(op.get("id") or ""))
        if "path" in op:
            _set_path(line, op["path"])
        if "wrap" in op:
            line.wrap = str(op["wrap"])
        if "ruby_runs" in op:
            line.ruby_runs = [tuple(item) for item in op["ruby_runs"]]
        if "emphasis_runs" in op:
            line.emphasis_runs = _emphasis(op["emphasis_runs"])
        if "style_runs" in op:
            line.style_runs = _style_runs(op["style_runs"])
        return

    if name == "add_mannequin":
        from genko import mannequin

        page = _require_page(episode, op)
        height = float(op.get("height_mm") or 80)
        prim = {
            "id": str(op.get("id") or new_id()),
            "kind": "mannequin",
            "pos": list(op.get("pos") or [100, 160, 0]),
            "size": [height / 2, height, height / 4],
            "rot": list(op.get("rot") or [0, 0, 0]),
            "joints": mannequin.default_joints(),
        }
        if op.get("preset"):
            try:
                mannequin.apply_preset(prim, str(op["preset"]))
            except ValueError as exc:
                raise ApplyError(str(exc)) from exc
        page.prims.append(prim)
        return

    if name == "pose_mannequin":
        page = _require_page(episode, op)
        mannequin_id = op.get("id")
        for prim in page.prims:
            if prim.get("id") == mannequin_id:
                if op.get("preset"):
                    from genko import mannequin

                    try:
                        mannequin.apply_preset(prim, str(op["preset"]))
                    except ValueError as exc:
                        raise ApplyError(str(exc)) from exc
                if "rot" in op:
                    prim["rot"] = list(op["rot"])
                if "pos" in op:
                    prim["pos"] = list(op["pos"])
                if op.get("height_mm"):
                    height = float(op["height_mm"])
                    prim["size"] = [height / 2, height, height / 4]
                if "joints" in op:
                    joints = prim.setdefault("joints", {})
                    for name, values in dict(op["joints"]).items():
                        slot = joints.setdefault(name, {})
                        slot.update(values)
                if op.get("drag"):
                    from genko import mannequin

                    drag = dict(op["drag"])
                    try:
                        change = mannequin.pose_to(prim, str(drag.get("handle")), drag.get("to") or [0, 0])
                    except ValueError as exc:
                        raise ApplyError(str(exc)) from exc
                    if "pos" in change:
                        prim["pos"] = change["pos"]
                    for joint, values in change.get("joints", {}).items():
                        prim.setdefault("joints", {}).setdefault(joint, {}).update(values)
                if op.get("drag") and not op.get("preset"):
                    prim.pop("preset", None)  # a hand-made pose is no longer the preset
                return
        raise ApplyError(f"no mannequin {mannequin_id}")

    if name == "set_onion":
        page = _require_page(episode, op)
        page.onion_from = None if op.get("from") in (None, "", 0) else int(op["from"])
        return

    if name == "step_onion":
        page = _require_page(episode, op)
        current = page.onion_from if page.onion_from else page.index
        nxt = int(current) + int(op.get("delta") or -1)
        page.onion_from = max(1, min(len(episode.pages), nxt))
        if page.onion_from == page.index:
            page.onion_from = max(1, page.index - 1)
        return

    if name == "set_lt":
        page = _require_page(episode, op)
        page.lt_threshold = float(op["threshold"])
        return

    if name in ("lock_page", "unlock_page"):
        raise ApplyError(f"{name} is handled with the actor in apply_ops")

    if name == "add_layer":
        page = _require_page(episode, op)
        kind_name = "folder" if op.get("folder") else str(op.get("kind") or "paint")
        kinds = {"folder": LayerKind.FOLDER, "pen": LayerKind.STROKES, "paint": LayerKind.RASTER}
        if kind_name not in kinds:
            raise ApplyError("kind must be pen, paint or folder")
        layer = Layer(
            id=str(op.get("id") or new_id()),
            role=LayerRole.USER,
            kind=kinds[kind_name],
            title=str(op.get("name") or "layer"),
            blend=str(op.get("blend") or "normal"),
            clip=bool(op.get("clip")),
            lock_alpha=bool(op.get("lock_alpha")),
            parent_id=str(op["parent"]) if op.get("parent") else None,
            exportable=True,
        )
        if any(item.id == layer.id for item in page.layers):
            raise ApplyError(f"layer {layer.id} exists")
        after = op.get("after")
        index = next((i + 1 for i, item in enumerate(page.layers) if item.id == after), len(page.layers)) if after else len(page.layers)
        page.layers.insert(index, layer)
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
        if layer.strokes:
            _rasterize_strokes(page, layer)
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
    if role == LayerRole.INK and not page.name_ok and _gated(episode):
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
    if dest_role == LayerRole.INK and not page.name_ok and _gated(episode):
        raise ApplyError("lt_convert to ink requires name_ok")
    src = page._layer(src_role)
    if not src.raster_png:
        raise ApplyError("lt_convert needs a raster on the source layer")
    from PIL import Image

    image = Image.open(__import__("io").BytesIO(src.raster_png))
    cut = op.get("threshold")
    if cut is None:
        cut = page.lt_threshold
    binary = to_line_art(image, method=str(op.get("method") or "adaptive"), threshold=cut)
    dest = page._layer(dest_role)
    for run in runs_to_strokes(binary, page.spec.width_mm, page.spec.height_mm):
        dest.strokes.append(coerce_stroke(run))
    if not dest.strokes:
        dest.strokes.append(coerce_stroke([(10.0, 10.0), (page.spec.width_mm - 10, 10.0)]))


def _refresh_frame_ids(frame: Frame, mapping: dict[str, str] | None = None) -> None:
    old = frame.id
    frame.id = new_id()
    if mapping is not None:
        mapping[old] = frame.id
    for child in frame.children:
        _refresh_frame_ids(child, mapping)


def _reorder(episode: Episode, order: list[int]) -> None:
    by_index = {page.index: page for page in episode.pages}
    episode.pages = [by_index[i] for i in order]
    remap_page_refs(episode, {old: new for new, old in enumerate(order, start=1)})


def remap_page_refs(episode: Episode, mapping: dict[int, int | None]) -> None:
    """Renumber pages after delete/reorder and fix every reference that uses page numbers."""
    for line in episode.story:
        line.page_index = mapping.get(line.page_index) or line.page_index
    for page in episode.pages:
        page.index = mapping[page.index] or page.index
        for line in page.texts:
            line.page_index = page.index
    for page in episode.pages:
        if page.spread_with is not None:
            page.spread_with = mapping.get(page.spread_with)
        if page.onion_from is not None:
            page.onion_from = mapping.get(page.onion_from)
    for ticket in episode.tickets:
        index = ticket.get("page_index")
        if index is None:
            continue
        new = mapping.get(index)
        if new is None:
            ticket["status"] = "orphaned"
        else:
            ticket["page_index"] = new


LEGACY_ACTOR = "genko"  # what callers get when they do not name themselves; treated as a person
GATE_OPS = frozenset({"name_ok"})
LINE_OPS = frozenset({"edit_line", "move_line", "delete_line", "set_balloon_path"})


def _studio_ops() -> frozenset[str]:
    from genko.studio.studio_ops import STUDIO_OPS

    return STUDIO_OPS


def can_approve(agent: str) -> bool:
    """Gates and unlocking other people's pages need a person: human:<name>, or the legacy unnamed caller."""
    return agent in (LEGACY_ACTOR, "human") or agent.startswith("human:")


def _shift(points: list[tuple], dx: float) -> list[tuple]:
    return [(float(pt[0]) + dx, float(pt[1]), *pt[2:]) for pt in points]


def _stroke_target(episode: Episode, page: Page, points: list[tuple], space: str) -> tuple[Page, list[tuple]]:
    """Resolve which page of a spread a stroke belongs to.

    space "page" (default): x is from this page's paper; past the gutter (the trim's edge at the
    binding) it goes to the partner. space "spread": x is from the left page's paper; the right page
    starts one trim width further (the two finished sizes meet at the gutter).
    """
    step = page.spread_step_mm()  # the finished sizes meet at the gutter
    trim = page.trim_rect_mm()
    gutter = trim.x + trim.width  # the gutter, in the left page's coordinates
    xs = [float(pt[0]) for pt in points]
    other = next((item for item in episode.pages if item.index == page.spread_with), None) if page.spread_with else None
    if space == "spread":
        if other is None:
            raise ApplyError("space spread needs a page with spread_with")
        left, right = (page, other) if page.side(episode.start_side) == "left" else (other, page)
        if max(xs) < gutter:
            return left, points
        if min(xs) >= gutter:
            return right, _shift(points, -step)
        raise ApplyError("a stroke cannot cross the gutter between spread pages")
    if space != "page":
        raise ApplyError("space must be page or spread")
    if other is not None and page.side(episode.start_side) == "left" and min(xs) >= gutter:
        return other, _shift(points, -step)
    if other is not None and page.side(episode.start_side) == "right" and max(xs) < trim.x:
        return other, _shift(points, step)
    return page, points


def facing_problem(episode: Episode, a: Page, b: Page) -> str | None:
    """Why two pages cannot form a spread, or None when they face each other."""
    if abs(a.index - b.index) != 1:
        return f"pages {a.index} and {b.index} are not next to each other"
    first, second = sorted((a, b), key=lambda p: p.index)
    start = "right" if episode.binding == Binding.RIGHT else "left"
    if first.side(episode.start_side) != start or second.side(episode.start_side) == start:
        return f"pages {first.index} and {second.index} are two sides of one leaf, not a spread"
    return None


MAX_IMAGE_PIXELS = 120_000_000


def _verify_image(blob: bytes) -> None:
    """Refuse bytes Pillow cannot open, and absurd sizes, before they reach a layer."""
    import io

    from PIL import Image

    try:
        with Image.open(io.BytesIO(blob)) as probe:
            width, height = probe.size
            probe.verify()
    except Exception as exc:  # Pillow raises many types for bad data
        raise ApplyError(f"not a readable image: {exc}") from exc
    if width * height > MAX_IMAGE_PIXELS:
        raise ApplyError(f"image too large: {width}x{height}")


def validate_episode(episode: Episode) -> list[str]:
    """Reference checks after a batch: returned as warnings (older files may already break them)."""
    warnings: list[str] = []
    indexes = {page.index for page in episode.pages}
    ids = {page.id for page in episode.pages}
    for line in episode.story:
        if line.page_index not in indexes:
            warnings.append(f"line {line.id}: page {line.page_index} does not exist")
            continue
        if line.frame_id:
            page = next(p for p in episode.pages if p.index == line.page_index)
            try:
                page._find(line.frame_id)
            except (KeyError, IndexError):
                warnings.append(f"line {line.id}: frame {line.frame_id} is not on page {line.page_index}")
    for page in episode.pages:
        leaves = {frame.id for frame in page.leaf_frames()}
        for layer in page.layers:
            if layer.kind == LayerKind.PLACED and layer.frame_id and layer.frame_id not in leaves:
                warnings.append(f"page {page.index} layer {layer.id}: panel {layer.frame_id} is gone (placed art will not clip)")
    for key in episode.page_locks:
        if key not in ids:
            warnings.append(f"page lock on unknown page {key}")
    for page in episode.pages:
        if page.spread_with is not None and page.index < page.spread_with:
            partner = next((p for p in episode.pages if p.index == page.spread_with), None)
            problem = facing_problem(episode, page, partner) if partner else f"spread partner {page.spread_with} missing"
            if problem:
                warnings.append(f"spread {page.index}-{page.spread_with}: {problem}")
    return warnings


def _journal_op(op: dict[str, Any]) -> dict[str, Any]:
    """A copy of the op for the journal, without embedded image bytes."""
    out = dict(op)
    if isinstance(out.get("png_base64"), str):
        out["png_base64"] = f"<{len(out['png_base64'])} base64 chars>"
    return out


def _op_page_index(episode: Episode, op: dict[str, Any]) -> int | None:
    if "page" in op:
        try:
            return int(op["page"])
        except (TypeError, ValueError):
            return None
    if op.get("op") in LINE_OPS and op.get("id"):
        for line in episode.story:
            if line.id == op["id"]:
                return line.page_index
    return None


def _placed_on(page: Page, frame_ids: set[str]) -> list[Layer]:
    return [layer for layer in page.layers if layer.kind == LayerKind.PLACED and layer.frame_id in frame_ids]


def _leaves_in_reading_order(frame: Frame, binding: Binding) -> list[Frame]:
    if not frame.children:
        return [frame]
    children = list(frame.children)
    if frame.split_axis == "vertical" and binding == Binding.RIGHT:
        children.reverse()
    return [leaf for child in children for leaf in _leaves_in_reading_order(child, binding)]


def _orphan_art(episode: Episode, page: Page, layers: list[Layer], reason: str) -> None:
    """Placed art whose panel went away: kept (asset refs stay alive for gc) but no longer printed."""
    if not layers:
        return
    from genko.io import _layer_to_dict

    episode.studio.setdefault("orphans", []).append(
        {"kind": "layers", "page_id": page.id, "reason": reason, "rev": episode.revision,
         "layers": [_layer_to_dict(layer) for layer in layers]}
    )
    page.layers = [layer for layer in page.layers if layer not in layers]


LAYOUT_OPS = frozenset({"split_frame", "merge_frame", "resize_frame", "set_layout", "cut_frame", "move_gutter"})
RASTER_EDIT_OPS = frozenset({"put_raster", "erase_raster", "erase", "filter_raster", "flood_fill", "fill", "fill_area", "gradient_fill",
                             "transform_area", "delete_area", "paste", "set_stroke_width", "reshape_stroke",
                             "trace_prims", "effect_to_layer"})


def _check_strict(episode: Episode, op: dict[str, Any], agent: str = LEGACY_ACTOR) -> None:
    """strict_gates (studio projects): printed layers change only after the name is approved,
    the approved layout stays put, and a page is finished only after its art is approved."""
    name = op.get("op")
    if (name in LAYOUT_OPS or (name == "set_frame" and "poly" in op)) and not can_approve(agent):
        page = _require_page(episode, op)
        if page.name_ok:
            raise ApplyError(f"page {page.index}: the name is approved; a person must revoke it before the layout changes (strict_gates)")
    if name == "advance" and op.get("to") == "finish":
        page = _require_page(episode, op)
        if not page.art_ok:
            raise ApplyError(f"page {page.index}: finish needs the art approved (strict_gates)")
    if name in ("set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer") and op.get("id"):
        op = {**op, "layer_id": op["id"]}  # (these name the layer by id: the same rule as drawing on it)
        name = "fill"
    if name in ("add_stroke", *RASTER_EDIT_OPS) and op.get("layer_id"):
        page = _require_page(episode, op)
        target = next((item for item in page.layers if item.id == op["layer_id"]), None)
        if target is not None and target.role not in (LayerRole.NAME, LayerRole.DRAFT) and not page.name_ok:
            raise ApplyError(f"{name} on a printed layer needs name_ok on page {page.index} (strict_gates)")
        return
    if name in RASTER_EDIT_OPS:
        layer = str(op.get("layer") or ("ink" if name != "filter_raster" else ""))
        if layer not in ("name", "draft", ""):
            page = _require_page(episode, op)
            if not page.name_ok:
                raise ApplyError(f"{name} on {layer} needs name_ok on page {page.index} (strict_gates)")
    if name == "add_line" and op.get("frame_id") and not ("x_mm" in op and "y_mm" in op):
        raise ApplyError("add_line with frame_id needs explicit x_mm/y_mm (strict_gates)")


def _check_page_lock(episode: Episode, op: dict[str, Any], agent: str) -> None:
    name = op.get("op")
    if name in GATE_OPS and not can_approve(agent):
        raise ApplyError(f"{name} needs a person (actor {agent} cannot approve)")
    if name in ("undo", "set_meta", "set_bible", "set_autosave"):
        return
    index = _op_page_index(episode, op)
    if index is None:
        return
    page = next((p for p in episode.pages if p.index == index), None)
    if page is None:
        return
    key = page.id  # v3: locks follow the page, not its position
    owner = episode.page_locks.get(key)
    if name == "lock_page":
        wanted = str(op.get("agent") or agent)
        if agent != LEGACY_ACTOR and wanted != agent:
            raise ApplyError(f"cannot lock page {index} as {wanted} (actor is {agent})")
        if owner and owner != wanted and not (can_approve(agent) and owner.startswith("ai:")):
            raise ApplyError(f"page {index} locked by {owner}")
        episode.page_locks[key] = wanted
        return
    if name == "unlock_page":
        if owner and owner != agent and not can_approve(agent):
            raise ApplyError(f"page {index} locked by {owner}; {agent} cannot unlock it")
        episode.page_locks.pop(key, None)
        return
    if owner and owner != agent:
        raise ApplyError(f"page {index} locked by {owner}")


# Ops that change only the page named by their "page" (and its spread partner, for strokes that
# cross the gutter), plus book-level fields that are always copied. Everything else copies the
# whole book, as before.
PAGE_LOCAL_OPS = frozenset({
    "split_frame", "cut_frame", "move_gutter", "merge_frame", "resize_frame", "set_frame",
    "add_line", "name_ok", "advance",
    "add_stroke", "fill", "fill_area", "transform_area", "delete_area", "paste", "set_stroke_width",
    "reshape_stroke", "delete_stroke", "put_raster", "set_layer", "gradient_fill", "duplicate_layer",
    "merge_down", "set_layer_mask", "paint_mask", "set_note", "select_frame", "flood_fill",
    "add_tone", "set_tone", "delete_tone", "add_effect", "edit_effect", "delete_effect", "effect_to_layer",
    "edit_stroke", "simplify_stroke", "set_ruler", "add_ruler", "edit_ruler", "delete_ruler",
    "add_prim3d", "add_scene", "edit_prim", "delete_prim", "trace_prims", "lt_convert", "erase_raster", "erase",
    "reorder_layers", "stamp_material", "add_mannequin", "pose_mannequin", "set_onion", "step_onion",
    "set_lt", "add_layer", "delete_layer", "filter_raster",
})
# Ops that find a line by id; the line lives in the story (always copied) or in one page's texts.
LINE_OPS = frozenset({"edit_line", "move_line", "delete_line", "set_balloon_path"})
BOOK_OPS = frozenset({"set_brush", "define_brush", "set_autosave", "add_ticket", "set_ticket", "reorder_lines"})


def _touched_pages(episode: Episode, ops: list) -> set[int] | None:
    """The page indexes a batch can change, or None when it may change any (then everything is copied)."""
    by_index = {page.index: page for page in episode.pages}
    touched: set[int] = set()
    for op in ops:
        if not isinstance(op, dict):
            return None
        name = op.get("op")
        if name in BOOK_OPS:
            continue
        if name in LINE_OPS:
            line_id = str(op.get("id") or "")
            touched.update(page.index for page in episode.pages if any(line.id == line_id for line in page.texts))
            continue
        if name not in PAGE_LOCAL_OPS:
            return None
        if name in ("name_ok", "advance") and "page" not in op:
            return None
        try:
            index = int(op["page"])
        except (KeyError, TypeError, ValueError):
            return None
        page = by_index.get(index)
        if page is None:
            continue  # the op itself reports the missing page
        touched.add(index)
        if page.spread_with:
            touched.add(page.spread_with)
    return touched


def _working_copy(episode: Episode, ops: list) -> Episode:
    """A copy of the book to apply a batch to. Only the pages the batch can change are copied; the
    others are shared with the book, which is safe because nothing changes a page in place outside an
    op, and ops only reach the pages _touched_pages names. This keeps one stroke on a thick book cheap."""
    touched = _touched_pages(episode, ops)
    if touched is None or len(touched) * 2 > len(episode.pages):
        work = copy.deepcopy(episode)
        work.undo_stack = []
        return work
    memo: dict = {}
    for page in episode.pages:
        if page.index not in touched:
            memo[id(page)] = page  # deepcopy returns the page itself: shared, not copied
    work = copy.deepcopy(episode, memo)
    work.undo_stack = []
    return work


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
        episode.journal_pending.append({"actor": agent, "ops": [{"op": "undo"}]})
        return {"ok": True, "applied": ["undo"], "snapshot": snapshot(episode), "job_id": new_id()}

    work = _working_copy(episode, ops)
    applied: list[str] = []
    for i, op in enumerate(ops):
        if not isinstance(op, dict):
            raise ApplyError(f"ops[{i}] must be an object")
        try:
            _check_page_lock(work, op, agent)
            if work.strict_gates:
                _check_strict(work, op, agent)
            if op.get("op") in _studio_ops():
                from genko.studio.studio_ops import apply_studio_op

                apply_studio_op(work, op, agent)
            elif op.get("op") not in ("lock_page", "unlock_page"):
                _apply_one(work, op)
        except ApplyError as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        except (KeyError, ValueError, TypeError) as exc:
            raise ApplyError(f"ops[{i}] {op.get('op')}: {exc}") from exc
        applied.append(str(op.get("op")))

    warnings = validate_episode(work)
    if dry_run:
        return {"ok": True, "applied": applied, "snapshot": snapshot(work), "job_id": new_id(), "warnings": warnings}

    # `work` is a deep copy, so the objects episode holds now are never touched again:
    # a shallow copy of them is the undo entry, no second deep copy needed.
    frozen = copy.copy(episode)
    frozen.undo_stack = []
    episode.undo_stack.append(frozen)
    del episode.undo_stack[:-UNDO_LIMIT]
    _copy_state(episode, work)
    episode.journal_pending.append({"actor": agent, "ops": [_journal_op(op) for op in ops]})
    return {"ok": True, "applied": applied, "snapshot": snapshot(episode), "job_id": new_id(), "warnings": warnings}
