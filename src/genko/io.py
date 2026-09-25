from __future__ import annotations

import json
import os
import time
from pathlib import Path

from genko import blobcache, journal
from genko.assets import AssetStore, canonical_json
from genko.migrate import KNOWN_PAGE_KEYS as _PAGE_KEYS
from genko.migrate import migrate_payload
from genko.models import Episode, Frame, Layer, LayerKind, Rect, StoryLine, stroke_to_dict, stroke_to_packed

V2_BACKUP = "project.v2.bak.json"


def _rect_to_dict(rect: Rect) -> dict:
    return {"x": rect.x, "y": rect.y, "width": rect.width, "height": rect.height}


def _frame_to_dict(frame: Frame) -> dict:
    return {
        "id": frame.id,
        "rect": _rect_to_dict(frame.rect),
        "split_axis": frame.split_axis,
        "clip": frame.clip,
        "bleed": frame.bleed,
        "border_mm": frame.border_mm,
        "children": [_frame_to_dict(child) for child in frame.children],
        "panel": frame.panel,
    } | ({"poly": [list(p) for p in frame.poly]} if frame.poly else {}) | ({"split": frame.split} if frame.split else {}) \
        | ({"custom": True} if frame.custom else {}) | ({"curves": list(frame.curves)} if frame.curves else {}) \
        | ({"line": dict(frame.line)} if frame.line else {})


def _layer_to_dict(layer: Layer, strokes: bool = True) -> dict:
    return {
        "id": layer.id,
        "role": layer.role.value,
        "kind": layer.kind.value,
        "visible": layer.visible,
        "exportable": layer.exportable,
        "strokes": [stroke_to_dict(stroke) for stroke in layer.strokes] if strokes else [],
        "raster_relpath": layer.raster_relpath,
        "fill_rgb": list(layer.fill_rgb) if layer.fill_rgb else None,
        "lpi": layer.lpi,
        "density": layer.density,
        "region": layer.region,
        "opacity": layer.opacity,
        "material_id": layer.material_id,
        "angle": layer.angle,
        "title": layer.title,
        "blend": layer.blend,
        "clip": layer.clip,
        "lock_alpha": layer.lock_alpha,
        "parent_id": layer.parent_id,
    } | ({"patches": [{k: v for k, v in p.items() if k != "png"} for p in layer.patches]} if layer.patches else {}) \
        | ({"locked": True} if layer.locked else {}) | ({"panel_clip": False} if not layer.panel_clip else {}) \
        | ({"tone": dict(layer.tone)} if layer.tone else {}) \
        | ({"mask": {"enabled": bool(layer.mask.get("enabled", True))}} if layer.mask else {}) \
        | ({"color": list(layer.color)} if layer.color else {}) | ({"reference": True} if layer.reference else {}) \
        | ({"fill": layer.fill} if layer.fill else {}) | ({"adjust": layer.adjust} if layer.adjust else {}) \
        | ({"effect": layer.effect} if layer.effect else {}) | ({"color_prints": True} if layer.color_prints else {}) \
        | ({"screen": layer.screen} if layer.screen else {}) | ({
        "asset": layer.asset,
        "frame_id": layer.frame_id,
        "placement_mm": _rect_to_dict(layer.placement_mm) if layer.placement_mm else None,
        "fit": layer.fit,
        "clip_to": layer.clip_to,
        "source": layer.source,
        "finish": layer.finish,
    } if layer.kind == LayerKind.PLACED else {})


def _line_to_dict(line: StoryLine) -> dict:
    return {
        "id": line.id,
        "page_index": line.page_index,
        "text": line.text,
        "speaker": line.speaker,
        "frame_id": line.frame_id,
        "ruby": line.ruby,
        "x_mm": line.x_mm,
        "y_mm": line.y_mm,
        "w_mm": line.w_mm,
        "h_mm": line.h_mm,
        "balloon": line.balloon,
        "tail": list(line.tail) if line.tail else None,
        "wrap": line.wrap,
        "ruby_runs": [list(item) for item in line.ruby_runs],
        "path": line.path,
    } | ({"emphasis_runs": list(line.emphasis_runs)} if line.emphasis_runs else {}) \
        | ({"style_runs": [[r[0], dict(r[1])] for r in line.style_runs]} if line.style_runs else {}) \
        | ({"style": dict(line.style)} if line.style else {}) | ({"tails": [dict(t) for t in line.tails]} if line.tails else {})




def _write_atomic(path: Path, text: str | bytes) -> None:
    """Write via a temp file and os.replace, retrying while Windows holds the file open."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    if isinstance(text, bytes):
        tmp.write_bytes(text)
    else:
        tmp.write_text(text, encoding="utf-8")
    for attempt in range(8):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 7:
                raise
            time.sleep(0.05 * (2**attempt))


def _payload(episode: Episode, store: AssetStore) -> dict:
    """The v3 project.json: decisions only. Raster bytes and strokes live in assets/ by hash."""
    return {
        **episode.extra,
        "version": 3,
        "revision": episode.revision,
        "title": episode.title,
        "episode": episode.episode,
        "binding": episode.binding.value,
        "start_side": episode.start_side,
        "strict_gates": episode.strict_gates,
        "autosave": episode.autosave,
        "font_path": episode.font_path,
        "page_locks": episode.page_locks,
        **({"nombre": dict(episode.nombre)} if episode.nombre else {}),
        "brush": {
            "rgb": list(episode.brush_rgb),
            "width_mm": episode.brush_width_mm,
            "stabilize": episode.brush_stabilize,
            "taper": episode.brush_taper,
            "curve": episode.brush_curve,
        } | ({"custom": {k: dict(v) for k, v in episode.brush_custom.items()}} if episode.brush_custom else {}),
        "spec": {
            "width_mm": episode.spec.width_mm,
            "height_mm": episode.spec.height_mm,
            "dpi": episode.spec.dpi,
            "bleed_mm": episode.spec.bleed_mm,
            "inner_margin_mm": episode.spec.inner_margin_mm,
            "expression": episode.spec.expression,
            "preset": episode.spec.preset,
        } | ({"trim_w_mm": episode.spec.trim_w_mm, "trim_h_mm": episode.spec.trim_h_mm} if episode.spec.trim_w_mm else {})
          | ({"margins_mm": list(episode.spec.margins_mm)} if episode.spec.margins_mm else {}),
        "bible": {
            "plot": episode.bible.plot,
            "characters": episode.bible.characters,
            "constraints": episode.bible.constraints,
        },
        "tickets": episode.tickets,
        "studio": episode.studio,
        "pages": [
            {
                "id": page.id,
                "index": page.index,
                "art_ok": page.art_ok,
                "plan": page.plan,
                "note": page.note,
                "name_ok": page.name_ok,
                "stage": page.stage,
                "spread_with": page.spread_with,
                "numero": page.numero,
                "onion_from": page.onion_from,
                "lt_threshold": page.lt_threshold,
                "effects": page.effects,
                "ruler": page.ruler,
                "rulers": page.rulers,
                "prims": page.prims,
                "frames": [_frame_to_dict(frame) for frame in page.frames],
                "layers": [_layer_to_v3(layer, store) for layer in page.layers],
                "fills": {role.value: list(rgb) for role, rgb in page.fills.items()},
            }
            | {k: v for k, v in page.extra.items() if k not in _PAGE_KEYS}
            for page in episode.pages
        ],
        "story": [_line_to_dict(line) for line in episode.story],
    }


def _layer_to_v3(layer: Layer, store: AssetStore) -> dict:
    data = _layer_to_dict(layer, strokes=False)
    del data["strokes"], data["raster_relpath"]
    if layer.kind == LayerKind.PLACED:
        return data
    if layer.raster_png:
        data["asset"] = store.put_bytes(layer.raster_png, ".png")
        layer.raster_relpath = store.relpath(data["asset"], ".png")
    if layer.mask and layer.mask.get("png"):
        data["mask"] = {"enabled": bool(layer.mask.get("enabled", True)), "asset": store.put_bytes(layer.mask["png"], ".png")}
    if layer.patches:
        data["patches"] = [{**{k: v for k, v in p.items() if k != "png"}, "asset": store.put_bytes(p["png"], ".png")}
                           for p in layer.patches if p.get("png")]
    if layer.strokes:
        ref = blobcache.ref_for(layer.strokes)
        if ref is None or not store.path(ref, ".strokes.json").is_file():
            blob = canonical_json([stroke_to_packed(stroke) for stroke in layer.strokes])
            ref = store.put_bytes(blob.encode("utf-8"), ".strokes.json")
            blobcache.remember(ref, layer.strokes)
        data["strokes_blob"] = ref
        data["stroke_count"] = len(layer.strokes)
    return data


def _gates(payload: dict | None) -> dict[str, object]:
    out: dict[str, object] = {}
    if not payload:
        return out
    for page in payload.get("pages", []):
        key = page.get("id") or f"index:{page.get('index')}"
        out[f"page:{key}:name_ok"] = bool(page.get("name_ok"))
        out[f"page:{key}:art_ok"] = bool(page.get("art_ok"))
    for char in (payload.get("bible") or {}).get("characters", []):
        out[f"character:{char.get('id')}:locked"] = bool(char.get("locked"))
    approvals = (payload.get("studio") or {}).get("approvals", [])
    out["export_approvals"] = sum(1 for a in approvals if a.get("gate") == "export" and not a.get("revoked"))
    return out


def gate_changes(before: dict | None, after: dict) -> list[dict]:
    """Approval state that changed in this save (who made the change is recorded with it)."""
    old, new = _gates(before), _gates(after)
    return [{"what": key, "from": old.get(key), "to": value} for key, value in sorted(new.items())
            if old.get(key, False if not key.startswith("export") else 0) != value]


def save_episode(episode: Episode, dest: Path, *, actor: str | None = None) -> None:
    """Save as v3: bump the revision, write only new assets, and journal the change."""
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    store = AssetStore(dest)
    if episode.asset_dir is None:
        episode.asset_dir = dest
    project_json = dest / "project.json"
    before = None
    old_payload = None
    if project_json.is_file():
        old = project_json.read_bytes()
        try:
            old_payload = json.loads(old)
        except ValueError:
            old_payload = None
        before = store.put_bytes(old, ".project.json")
        if not (dest / V2_BACKUP).exists() and json.loads(old).get("version", 1) < 3:
            (dest / V2_BACKUP).write_bytes(old)
    base = episode.revision
    episode.revision = base + 1
    payload = _payload(episode, store)
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    _write_atomic(project_json, text)
    after = store.put_bytes(text.encode("utf-8"), ".project.json")
    pending, episode.journal_pending = episode.journal_pending, []
    who = actor or (pending[-1]["actor"] if pending else "genko")
    changes = gate_changes(old_payload, payload)
    if changes:
        journal.append_audit(dest, {"rev": episode.revision, "actor": who, "at": time.time(), "changes": changes})
    journal.append(dest, {
        "kind": "commit",
        "rev": episode.revision,
        "base_rev": base,
        "actor": who,
        "at": time.time(),
        **_journal_ops([op for batch in pending for op in batch["ops"]], store),
        "before": before,
        "after": after,
    })
    from genko import timelapse

    if timelapse.is_on(episode):  # (a small picture of each changed page; every page when it was just turned on)
        started = not ((old_payload or {}).get("timelapse") or {}).get("on")
        timelapse.record(dest, episode, [p.index for p in episode.pages] if started else timelapse.changed_pages(old_payload, payload))


JOURNAL_OPS_INLINE = 16_000  # bytes; larger batches keep only a brief of each op in the journal line


def _journal_ops(ops: list, store: AssetStore) -> dict:
    """The ops of a commit for the journal. A big batch (thousands of strokes) goes to an asset, and the
    journal line keeps each op's name and small fields, so reading the journal for undo stays quick."""
    text = json.dumps(ops, ensure_ascii=False)
    if len(text) <= JOURNAL_OPS_INLINE:
        return {"ops": ops}
    brief = [{k: v for k, v in op.items() if isinstance(v, (str, int, float, bool)) and len(str(v)) < 80}
             if isinstance(op, dict) else {} for op in ops]
    return {"ops": brief, "ops_asset": store.put_bytes(text.encode("utf-8"), ".ops.json")}


def load_episode(src: Path) -> Episode:
    src = Path(src)
    payload = json.loads((src / "project.json").read_text(encoding="utf-8"))
    episode = migrate_payload(payload, AssetStore(src))
    episode.asset_dir = src
    _attach_legacy_rasters(episode, src)
    return episode


def _attach_legacy_rasters(episode: Episode, src: Path) -> None:
    """v2 files keep rasters under pages/NNN/; v3 layers already carry their bytes."""
    for page in episode.pages:
        for layer in page.layers:
            if layer.raster_png is None and layer.raster_relpath:
                path = src / layer.raster_relpath
                if path.is_file():
                    layer.raster_png = path.read_bytes()
