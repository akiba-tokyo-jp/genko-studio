from __future__ import annotations

import json
import os
import time
from pathlib import Path

from genko import journal
from genko.assets import AssetStore, canonical_json
from genko.migrate import KNOWN_PAGE_KEYS as _PAGE_KEYS
from genko.migrate import migrate_payload
from genko.models import Episode, Frame, Layer, LayerKind, Rect, StoryLine, stroke_to_dict

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
    }


def _layer_to_dict(layer: Layer) -> dict:
    return {
        "id": layer.id,
        "role": layer.role.value,
        "kind": layer.kind.value,
        "visible": layer.visible,
        "exportable": layer.exportable,
        "strokes": [stroke_to_dict(stroke) for stroke in layer.strokes],
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
    } | ({
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
    }




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
        "brush": {
            "rgb": list(episode.brush_rgb),
            "width_mm": episode.brush_width_mm,
            "stabilize": episode.brush_stabilize,
            "taper": episode.brush_taper,
            "curve": episode.brush_curve,
        },
        "spec": {
            "width_mm": episode.spec.width_mm,
            "height_mm": episode.spec.height_mm,
            "dpi": episode.spec.dpi,
            "bleed_mm": episode.spec.bleed_mm,
            "inner_margin_mm": episode.spec.inner_margin_mm,
            "expression": episode.spec.expression,
            "preset": episode.spec.preset,
        },
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
    data = _layer_to_dict(layer)
    del data["strokes"], data["raster_relpath"]
    if layer.kind == LayerKind.PLACED:
        return data
    if layer.raster_png:
        data["asset"] = store.put_bytes(layer.raster_png, ".png")
        layer.raster_relpath = store.relpath(data["asset"], ".png")
    if layer.strokes:
        blob = canonical_json([stroke_to_dict(stroke) for stroke in layer.strokes])
        data["strokes_blob"] = store.put_bytes(blob.encode("utf-8"), ".strokes.json")
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
        "ops": [op for batch in pending for op in batch["ops"]],
        "before": before,
        "after": after,
    })


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
