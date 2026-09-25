"""Studio ops: agent state, panel briefs, candidates and adoption, all through apply_ops.

Every op only changes the in-memory Episode (transactional like the core ops).
The only I/O is reading an asset's image header for place_asset; no op writes
files. Gate ops need a person (see genko.ops.can_approve).
"""

from __future__ import annotations

import copy
import io
from typing import Any

from genko.models import Episode, Frame, Layer, LayerKind, LayerRole, Page, Rect, new_id
from genko.placement import default_target, fit_rect

BRIEF_KEYS = ("shot", "angle", "characters", "props", "refs", "regions", "instruction", "location_id", "time",
              "action", "emotion", "gen")
PANEL_STATUSES = ("empty", "briefed", "requested", "candidates", "fix_requested", "adopted", "skip")
CANDIDATE_STATUSES = ("candidate", "shortlisted", "rejected")
ORIGIN_KINDS = ("agent", "human", "self", "fixture", "genko")
GATES = ("bible", "script", "sheet", "name", "art", "export")
ADOPT_TARGETS = ("art", "bg", "draft", "ink")

STUDIO_OPS = frozenset({
    "set_studio", "upsert_character", "delete_character", "upsert_location", "delete_location",
    "upsert_prop", "delete_prop", "set_script", "set_page_plan", "set_panel", "record_review",
    "approve", "revoke", "request_approval", "request_fix", "add_region", "edit_region", "delete_region",
    "replace_regions", "bind_ref", "unbind_ref", "register_assets", "attach_reference",
    "open_request", "close_request", "import_candidates", "review_candidates", "set_candidate",
    "adopt_candidate", "unadopt", "set_placement", "place_asset", "set_finish", "ask_human", "reject_sheet",
    "set_layout", "propose", "resolve_proposal",
})

STUDIO_SCHEMA = [
    {"op": "set_studio", "bible_doc": "object?", "style": "object?", "policy": "object? (human)", "premise": "str?"},
    {"op": "upsert_character", "character": "{id, …}", "unlock": "bool? (human)"},
    {"op": "delete_character", "id": "str", "force": "bool?"},
    {"op": "upsert_location", "location": "{id, …}"},
    {"op": "delete_location", "id": "str"},
    {"op": "upsert_prop", "prop": "{id, …}"},
    {"op": "delete_prop", "id": "str"},
    {"op": "set_script", "script": "script@1"},
    {"op": "set_page_plan", "page": "int", "plan": "object"},
    {"op": "set_panel", "page": "int", "frame_id": "str", "set": "object", "unset": "[str]?", "pin": "[str]?", "unpin": "[str]?"},
    {"op": "record_review", "page": "int", "frame_id": "str?", "kind": "name|art", "score": "float?", "notes": "str", "input_hash": "str"},
    {"op": "approve", "gate": "bible|script|sheet|name|art|export", "page": "int?", "character_id": "str?", "candidate_id": "str?", "face_asset": "str?"},
    {"op": "revoke", "gate": "name|art|sheet", "page": "int?", "character_id": "str?", "reason": "str"},
    {"op": "request_approval", "gate": "str", "pages": "[int]?", "character_id": "str?", "note": "str?"},
    {"op": "request_fix", "page": "int", "frame_id": "str?", "candidate_id": "str?", "instruction": "str", "scope": "frame|person|background|text|regions?"},
    {"op": "add_region", "page": "int", "frame_id": "str", "region": "{id?, kind, rect_mm, char?, confidence?}"},
    {"op": "edit_region", "page": "int", "frame_id": "str", "id": "str", "set": "object"},
    {"op": "delete_region", "page": "int", "frame_id": "str", "id": "str"},
    {"op": "replace_regions", "page": "int", "frame_id": "str", "source": "agent|detected|derived", "regions": "[region]"},
    {"op": "bind_ref", "page": "int", "frame_id": "str", "ref": "{id?, source, role, order?}"},
    {"op": "unbind_ref", "page": "int", "frame_id": "str", "id": "str"},
    {"op": "register_assets", "assets": "{sha256:…: {kind, origin?, note?}}"},
    {"op": "attach_reference", "target": "{character_id|location_id|prop_id}", "asset": "str", "kind": "str"},
    {"op": "open_request", "request": "{id, purpose, mode, target, brief_hash?, parent?}"},
    {"op": "close_request", "id": "str", "reason": "done|cancelled|superseded"},
    {"op": "import_candidates", "request_id": "str?", "page": "int?", "frame_id": "str?", "character_id": "str?", "location_id": "str?", "candidates": "[{id?, asset, px:[w,h], mode?, parent?, origin}]"},
    {"op": "review_candidates", "page": "int", "frame_id": "str", "reviews": "[{candidate_id, score, note?, fix?}]"},
    {"op": "set_candidate", "page": "int", "frame_id": "str", "candidate_id": "str", "status": "candidate|shortlisted|rejected"},
    {"op": "adopt_candidate", "page": "int", "frame_id": "str", "candidate_id": "str", "to": "art|bg|draft|ink?", "fit": "cover|contain|stretch?", "offset_mm": "[dx,dy]?", "scale": "float?", "clip_to": "frame|bleed|none?"},
    {"op": "unadopt", "page": "int", "frame_id": "str", "to": "art|bg|draft|ink?"},
    {"op": "set_placement", "page": "int", "frame_id": "str", "to": "art|bg|draft|ink?", "fit": "str?", "offset_mm": "[dx,dy]?", "scale": "float?", "clip_to": "str?"},
    {"op": "place_asset", "page": "int", "asset": "sha256:…", "to": "art|bg|draft|name?", "frame_id": "str?", "fit": "str?", "clip_to": "str?", "placement_mm": "[x,y,w,h]?", "title": "str?"},
    {"op": "set_finish", "page": "int", "frame_id": "str", "finish": "object|null"},
    {"op": "ask_human", "text": "str", "page": "int?", "frame_id": "str?", "item": "str? (work item kind it blocks)"},
    {"op": "reject_sheet", "character_id": "str", "candidate_ids": "[str]? (all if omitted)", "note": "str (person only)"},
    {"op": "set_layout", "page": "int", "tree": "{rect_mm, axis?, children?}", "force": "bool?"},
    {"op": "propose", "proposal": "{id?, kind: layout|lines, page, …}"},
    {"op": "resolve_proposal", "id": "str", "status": "accepted|rejected", "note": "str? (person only)"},
]


def _err(message: str) -> Exception:
    from genko.ops import ApplyError

    return ApplyError(message)


def _person(agent: str) -> bool:
    from genko.ops import can_approve

    return can_approve(agent)


def _page(episode: Episode, op: dict) -> Page:
    if op.get("page_id"):
        for page in episode.pages:
            if page.id == op["page_id"]:
                return page
        raise _err(f"no page {op['page_id']}")
    try:
        index = int(op["page"])
    except (KeyError, TypeError, ValueError) as exc:
        raise _err("page is required") from exc
    for page in episode.pages:
        if page.index == index:
            return page
    raise _err(f"no page {index}")


def _leaf(page: Page, frame_id: Any) -> Frame:
    if not frame_id:
        raise _err("frame_id is required")
    try:
        frame = page._find(str(frame_id))
    except (KeyError, IndexError) as exc:
        raise _err(f"no frame {frame_id} on page {page.index}") from exc
    if frame.children:
        raise _err(f"frame {frame_id} is not a leaf panel")
    return frame


def _panel(frame: Frame) -> dict:
    if frame.panel is None:
        frame.panel = {"status": "empty"}
    return frame.panel


def brief_hash(panel: dict) -> str:
    """What the image is asked to be. Regions count only when a person drew them (an instruction);
    faces reported from the adopted art describe the result and must not make it stale."""
    from genko.studio.jsonutil import content_hash

    brief = {key: panel.get(key) for key in BRIEF_KEYS}
    brief["regions"] = [r for r in panel.get("regions") or [] if r.get("source") == "user"] or None
    return content_hash(brief)


def _refresh_brief(panel: dict) -> None:
    panel["brief_hash"] = brief_hash(panel)
    for cand in panel.get("candidates", []):
        cand["stale"] = cand.get("brief_hash") != panel["brief_hash"]


def _by_id(items: list[dict], key: str) -> dict | None:
    return next((item for item in items if item.get("id") == key), None)


def _upsert(items: list[dict], item: dict, label: str) -> None:
    if not isinstance(item, dict) or not item.get("id"):
        raise _err(f"{label} needs an id")
    for i, existing in enumerate(items):
        if existing.get("id") == item["id"]:
            items[i] = item
            return
    items.append(item)


def _ticket(episode: Episode, page: Page | None, agent: str, **fields) -> dict:
    ticket = {
        "id": fields.pop("id", None) or new_id(),
        "page_index": page.index if page else None,
        "page_id": page.id if page else None,
        "assignee": fields.pop("assignee", "human"),
        "status": "open",
        "created_by": agent,
        "at_rev": episode.revision,
        **fields,
    }
    episode.tickets.append(ticket)
    return ticket


def _close_tickets(episode: Episode, agent: str, kind: str, gate: str, page: Page | None = None, character_id: str | None = None) -> None:
    for ticket in episode.tickets:
        if ticket.get("kind") != kind or ticket.get("status") != "open" or ticket.get("gate") != gate:
            continue
        if page is not None:
            pages = ticket.get("pages") or ([ticket.get("page_index")] if ticket.get("page_index") else [])
            if page.index not in pages:
                continue
            remaining = [p for p in pages if p != page.index]
            if remaining:
                ticket["pages"] = remaining
                continue
        if character_id is not None and ticket.get("character_id") != character_id:
            continue
        ticket["status"] = "done"
        ticket["closed_by"] = agent


def _art_layer(page: Page, frame_id: str, to: str) -> Layer | None:
    for layer in page.layers:
        if layer.kind == LayerKind.PLACED and layer.frame_id == frame_id and (layer.source or {}).get("to", "art") == to:
            return layer
    return None


def _insert_below_ink(page: Page, layer: Layer) -> None:
    """Placed art goes under the ink; extracted line art (to: ink) goes over the other placed art."""
    is_ink = (layer.source or {}).get("to") == "ink"
    for i, existing in enumerate(page.layers):
        placed_ink = existing.kind == LayerKind.PLACED and (existing.source or {}).get("to") == "ink"
        if existing.role == LayerRole.INK or (placed_ink and not is_ink):
            page.layers.insert(i, layer)
            return
    page.layers.append(layer)


def _place(page: Page, frame: Frame | None, layer: Layer, px: tuple[int, int], fit: str, clip_to: str,
           offset: tuple[float, float], scale: float, pad_mm: float = 0.0) -> None:
    """Fit the image over its panel. A generated image covers the panel plus the request's pad,
    so that margin is kept outside the panel (and cut off by the clip)."""
    if frame is None:
        target = Rect(0, 0, page.spec.width_mm, page.spec.height_mm)
    elif clip_to == "bleed":
        target = default_target(page, frame, clip_to)
    else:
        r = frame.rect
        target = Rect(r.x - pad_mm, r.y - pad_mm, r.width + 2 * pad_mm, r.height + 2 * pad_mm)
    layer.fit, layer.clip_to = fit, clip_to
    layer.placement_mm = fit_rect(target, int(px[0]), int(px[1]), fit, offset, scale)


def _style_lock(episode: Episode, page: Page, agent: str) -> dict:
    """What the pilot page fixes for the rest of the book: the image tool most used for its art,
    a reference image (the adopted art of its largest panel), the style notes and finish."""
    tools: dict[str, int] = {}
    best: tuple[float, str] | None = None
    for frame in page.leaf_frames():
        panel = frame.panel or {}
        cand = _by_id(panel.get("candidates", []), str((panel.get("adopted") or {}).get("art") or ""))
        if cand is None:
            continue
        tool = (cand.get("origin") or {}).get("tool_id")
        if tool:
            tools[tool] = tools.get(tool, 0) + 1
        area = frame.rect.width * frame.rect.height
        if best is None or area > best[0]:
            best = (area, cand["asset"])
    style = episode.studio.get("style") or {}
    notes = style.get("notes") or ((episode.studio.get("bible_doc") or {}).get("style") or {}).get("notes") or []
    return {
        "from_page": page.id,
        "page": page.index,
        "tool": max(sorted(tools), key=lambda t: tools[t]) if tools else None,
        "reference": best[1] if best else None,
        "notes": list(notes),
        "finish": style.get("finish"),
        "by": agent,
        "rev": episode.revision,
    }


def _frame_from_tree(node: dict) -> Frame:
    x, y, w, h = (float(v) for v in node["rect_mm"])
    frame = Frame(id=new_id(), rect=Rect(round(x, 2), round(y, 2), round(w, 2), round(h, 2)))
    children = node.get("children") or []
    if children:
        if node.get("axis") not in ("horizontal", "vertical"):
            raise _err("a node with children needs axis horizontal or vertical")
        frame.split_axis = node["axis"]
        frame.children = [_frame_from_tree(child) for child in children]
    return frame


def _pad(cand: dict) -> float:
    return float((cand.get("mapping") or {}).get("pad_mm") or 0.0)


def apply_studio_op(episode: Episode, op: dict[str, Any], agent: str) -> None:
    name = op["op"]
    studio = episode.studio
    person = _person(agent)

    if name == "set_studio":
        if "policy" in op and not person:
            raise _err("only a person can change the policy")
        for key in ("bible_doc", "style", "policy", "premise"):
            if key in op:
                if key in ("style", "policy") and isinstance(op[key], dict) and isinstance(studio.get(key), dict):
                    studio[key] = {**studio[key], **op[key]}
                else:
                    studio[key] = op[key]
        return

    if name == "upsert_character":
        char = op.get("character") or {}
        existing = _by_id(episode.bible.characters, char.get("id", ""))
        if existing and existing.get("locked") and not (op.get("unlock") and person):
            raise _err(f"character {char.get('id')} is locked (a person can unlock it)")
        if existing and existing.get("locked") and "locked" not in char:
            char = {**char, "locked": False}
        _upsert(episode.bible.characters, char, "character")
        return

    if name == "delete_character":
        cid = str(op.get("id") or "")
        used = any(c.get("id") == cid for page in episode.pages for f in page.leaf_frames() for c in (f.panel or {}).get("characters", []))
        if used and not op.get("force"):
            raise _err(f"character {cid} appears in panels; use force")
        episode.bible.characters = [c for c in episode.bible.characters if c.get("id") != cid]
        return

    if name in ("upsert_location", "upsert_prop"):
        key = "locations" if name == "upsert_location" else "props"
        _upsert(studio.setdefault(key, []), op.get("location" if key == "locations" else "prop"), key[:-1])
        return

    if name in ("delete_location", "delete_prop"):
        key = "locations" if name == "delete_location" else "props"
        studio[key] = [item for item in studio.get(key, []) if item.get("id") != op.get("id")]
        return

    if name == "set_script":
        if not isinstance(op.get("script"), dict):
            raise _err("script must be an object")
        studio["script"] = op["script"]
        return

    if name == "set_page_plan":
        page = _page(episode, op)
        plan = op.get("plan") or {}
        merged = {**(page.plan or {}), **plan}
        if page.plan and "reviews" in page.plan and "reviews" not in plan:
            merged["reviews"] = page.plan["reviews"]
        page.plan = merged
        return

    if name == "set_panel":
        page = _page(episode, op)
        panel = _panel(_leaf(page, op.get("frame_id")))
        pinned = set(panel.get("pinned", []))
        changes = op.get("set") or {}
        for key, value in changes.items():
            if key in pinned and not person:
                raise _err(f"{key} is pinned by a person")
            if key == "gen" and isinstance(value, dict) and any(k.endswith("_override") and v is not None for k, v in value.items()):
                if not person:
                    raise _err("only a person can set prompt/size overrides")
                pinned.add("gen")
            if key == "status" and value not in PANEL_STATUSES:
                raise _err(f"status must be one of {PANEL_STATUSES}")
            if key == "status" and value == "skip" and not person:
                raise _err("only a person can mark a panel skip")
            if key in ("candidates", "adopted"):
                raise _err(f"{key} changes through the candidate ops")
            panel[key] = copy.deepcopy(value)
        for key in op.get("unset") or []:
            if key in pinned and not person:
                raise _err(f"{key} is pinned by a person")
            panel.pop(key, None)
        for key in op.get("pin") or []:
            pinned.add(key)
        for key in op.get("unpin") or []:
            if not person:
                raise _err("only a person can unpin")
            pinned.discard(key)
        panel["pinned"] = sorted(pinned)
        if panel.get("status") == "empty" and any(panel.get(k) for k in ("shot", "action", "characters")):
            panel["status"] = "briefed"
        _refresh_brief(panel)
        return

    if name == "record_review":
        page = _page(episode, op)
        review = {"by": agent, "score": op.get("score"), "notes": str(op.get("notes") or ""),
                  "input_hash": op.get("input_hash"), "rev": episode.revision}
        kind = str(op.get("kind") or "name")
        if op.get("frame_id"):
            panel = _panel(_leaf(page, op["frame_id"]))
            panel.setdefault("reviews", {})[kind] = review
        else:
            page.plan = page.plan or {}
            page.plan.setdefault("reviews", {})[kind] = review
        return

    if name == "approve":
        if not person:
            raise _err(f"approve needs a person (actor {agent} cannot approve)")
        gate = op.get("gate")
        if gate not in GATES:
            raise _err(f"gate must be one of {GATES}")
        record: dict[str, Any] = {"gate": gate, "by": agent, "rev": episode.revision}
        if gate in ("name", "art"):
            page = _page(episode, op)
            record["page_id"] = page.id
            if gate == "name":
                from genko.pipeline import advance

                page.name_ok = True
                if page.stage == "name":
                    advance(page, to="ink")
            else:
                if not page.name_ok:
                    raise _err(f"page {page.index}: approve the name before the art")
                missing = [f.id for f in page.leaf_frames() if f.panel and f.panel.get("status") not in ("adopted", "skip", "empty")]
                if missing:
                    raise _err(f"page {page.index}: panels without adopted art: {', '.join(missing)}")
                page.art_ok = True
                style = studio.setdefault("style", {})
                style.setdefault("locked_from_page", page.id)
                if "locked" not in style:
                    style["locked"] = _style_lock(episode, page, agent)
            _close_tickets(episode, agent, "gate", gate, page=page)
        elif gate == "sheet":
            cid = str(op.get("character_id") or "")
            char = _by_id(episode.bible.characters, cid)
            if char is None:
                raise _err(f"no character {cid}")
            cand = _by_id(studio.get("character_candidates", {}).get(cid, []), str(op.get("candidate_id") or ""))
            if cand is None:
                raise _err(f"no candidate {op.get('candidate_id')} for {cid}")
            refs = [r for r in char.get("refs", []) if r.get("kind") not in ("sheet", "face")]
            refs.append({"asset": cand["asset"], "kind": "sheet", "approved_by": agent})
            if op.get("face_asset"):
                refs.append({"asset": op["face_asset"], "kind": "face", "approved_by": agent})
            char["refs"] = refs
            char["locked"] = True
            record["character_id"] = cid
            _close_tickets(episode, agent, "gate", "sheet", character_id=cid)
        else:
            _close_tickets(episode, agent, "gate", gate)
        studio.setdefault("approvals", []).append(record)
        return

    if name == "revoke":
        if not person:
            raise _err("revoke needs a person")
        gate = op.get("gate")
        if gate in ("name", "art"):
            page = _page(episode, op)
            page.art_ok = False
            style = studio.get("style") or {}
            if (style.get("locked") or {}).get("from_page") == page.id:
                style.pop("locked", None)  # the pilot page is open again: so is the style
                style.pop("locked_from_page", None)
            if gate == "name":
                page.name_ok = False
                page.stage = "name"
        elif gate == "sheet":
            char = _by_id(episode.bible.characters, str(op.get("character_id") or ""))
            if char is None:
                raise _err(f"no character {op.get('character_id')}")
            char["locked"] = False
        else:
            raise _err("gate must be name, art or sheet")
        studio.setdefault("approvals", []).append({"gate": gate, "revoked": True, "by": agent, "reason": str(op.get("reason") or ""),
                                                   "rev": episode.revision, "page_id": op.get("page_id")})
        return

    if name == "request_approval":
        gate = str(op.get("gate") or "")
        if gate not in GATES:
            raise _err(f"gate must be one of {GATES}")
        pages = sorted({int(p) for p in op.get("pages") or []})
        for index in pages:
            if not any(p.index == index for p in episode.pages):
                raise _err(f"no page {index}")
        _ticket(episode, None, agent, kind="gate", gate=gate, pages=pages, character_id=op.get("character_id"),
                text=str(op.get("note") or ""))
        return

    if name == "request_fix":
        page = _page(episode, op)
        instruction = str(op.get("instruction") or "").strip()
        if not instruction:
            raise _err("instruction is required")
        frame_id = op.get("frame_id")
        if frame_id:
            panel = _panel(_leaf(page, frame_id))
            if panel.get("status") != "skip":
                panel["status"] = "fix_requested"
        _ticket(episode, page, agent, kind="fix", frame_id=frame_id, candidate_id=op.get("candidate_id"),
                scope=str(op.get("scope") or "frame"), text=instruction, assignee="agent" if person else "human")
        return

    if name in ("add_region", "edit_region", "delete_region", "replace_regions"):
        page = _page(episode, op)
        panel = _panel(_leaf(page, op.get("frame_id")))
        regions = panel.setdefault("regions", [])
        if name == "add_region":
            region = dict(op.get("region") or {})
            if not region.get("kind"):
                raise _err("region.kind is required")
            region["id"] = region.get("id") or "rg_" + new_id()[:8]
            region["source"] = "user" if person else "agent"
            if _by_id(regions, region["id"]):
                raise _err(f"region {region['id']} exists")
            regions.append(region)
        elif name in ("edit_region", "delete_region"):
            region = _by_id(regions, str(op.get("id") or ""))
            if region is None:
                raise _err(f"no region {op.get('id')}")
            if region.get("source") == "user" and not person:
                raise _err("a person drew this region; only a person can change it")
            if name == "edit_region":
                region.update({k: v for k, v in (op.get("set") or {}).items() if k not in ("id", "source")})
            else:
                regions.remove(region)
        else:
            source = str(op.get("source") or "agent")
            if source == "user":
                raise _err("replace_regions cannot replace a person's regions")
            kept = [r for r in regions if r.get("source") != source]
            new = []
            for region in op.get("regions") or []:
                region = dict(region)
                region["id"] = region.get("id") or "rg_" + new_id()[:8]
                region["source"] = source
                new.append(region)
            panel["regions"] = kept + new
        _refresh_brief(panel)
        return

    if name in ("bind_ref", "unbind_ref"):
        page = _page(episode, op)
        panel = _panel(_leaf(page, op.get("frame_id")))
        refs = panel.setdefault("refs", [])
        if name == "bind_ref":
            ref = dict(op.get("ref") or {})
            if not ref.get("role") or not ref.get("source"):
                raise _err("ref needs role and source")
            ref["id"] = ref.get("id") or "rf_" + new_id()[:8]
            ref.setdefault("order", len(refs))
            refs[:] = [r for r in refs if r.get("id") != ref["id"]] + [ref]
        else:
            panel["refs"] = [r for r in refs if r.get("id") != op.get("id")]
        _refresh_brief(panel)
        return

    if name == "register_assets":
        index = studio.setdefault("assets", {})
        for ref, meta in (op.get("assets") or {}).items():
            _require_asset(episode, ref)
            index[ref] = {**index.get(ref, {}), **(meta or {})}
        return

    if name == "attach_reference":
        target = op.get("target") or {}
        _require_asset(episode, str(op.get("asset") or ""))
        ref = {"asset": op["asset"], "kind": str(op.get("kind") or "reference"), "by": agent}
        if target.get("character_id"):
            item = _by_id(episode.bible.characters, target["character_id"])
        elif target.get("location_id"):
            item = _by_id(studio.get("locations", []), target["location_id"])
        else:
            item = _by_id(studio.get("props", []), str(target.get("prop_id") or ""))
        if item is None:
            raise _err(f"no target {target}")
        if item.get("locked") and ref["kind"] in ("sheet", "face") and not person:
            raise _err("the character sheet is approved; only a person can change it")
        item.setdefault("refs", []).append(ref)
        return

    if name == "open_request":
        request = dict(op.get("request") or {})
        if not request.get("id"):
            raise _err("request.id is required")
        requests = studio.setdefault("requests", {})
        if request["id"] in requests:
            if requests[request["id"]].get("status") == "open":
                return  # same content, same id: nothing to do
            requests[request["id"]]["status"] = "open"  # asked again: another round
            request = requests[request["id"]]
        else:
            request.update({"status": "open", "by": agent, "rev": episode.revision})
            requests[request["id"]] = request
        target = request.get("target") or {}
        if target.get("frame_id"):
            page = _page(episode, target)
            panel = _panel(_leaf(page, target["frame_id"]))
            attempts = panel.setdefault("attempts", {"requests": 0, "images": 0, "fix_rounds": 0})
            attempts["requests"] += 1
            if request.get("mode") in ("edit", "inpaint"):
                attempts["fix_rounds"] += 1
            if panel.get("status") in ("empty", "briefed", "fix_requested"):
                panel["status"] = "requested"
        return

    if name == "close_request":
        request = studio.get("requests", {}).get(str(op.get("id") or ""))
        if request is None:
            raise _err(f"no request {op.get('id')}")
        request["status"] = str(op.get("reason") or "done")
        return

    if name == "import_candidates":
        _import_candidates(episode, op, agent)
        return

    if name in ("review_candidates", "set_candidate"):
        page = _page(episode, op)
        panel = _panel(_leaf(page, op.get("frame_id")))
        cands = panel.get("candidates", [])
        items = (op.get("reviews") or []) if name == "review_candidates" else [op]
        for item in items:
            cand = _by_id(cands, str(item.get("candidate_id") or ""))
            if cand is None:
                raise _err(f"no candidate {item.get('candidate_id')}")
            if name == "review_candidates":
                cand["review"] = {"by": agent, "score": item.get("score"), "note": item.get("note", ""), "fix": item.get("fix")}
            else:
                if item.get("status") not in CANDIDATE_STATUSES:
                    raise _err(f"status must be one of {CANDIDATE_STATUSES}")
                cand["status"] = item["status"]
        return

    if name == "adopt_candidate":
        page = _page(episode, op)
        frame = _leaf(page, op.get("frame_id"))
        panel = _panel(frame)
        to = str(op.get("to") or "art")
        if to not in ADOPT_TARGETS:
            raise _err(f"to must be one of {ADOPT_TARGETS}")
        if to != "draft" and not page.name_ok:
            raise _err(f"page {page.index}: art can be adopted only after the name is approved")
        cand = _by_id(panel.get("candidates", []), str(op.get("candidate_id") or ""))
        if cand is None:
            raise _err(f"no candidate {op.get('candidate_id')}")
        layer = _art_layer(page, frame.id, to)
        if layer is None:
            layer = Layer(
                id=new_id(), role=LayerRole.DRAFT if to == "draft" else (LayerRole.BG if to == "bg" else LayerRole.USER),
                kind=LayerKind.PLACED, exportable=to != "draft", title=f"{to} {frame.id}", frame_id=frame.id,
                source={"to": to},
            )
            _insert_below_ink(page, layer)
        layer.asset = cand["asset"]
        layer.source = {"candidate": cand["id"], "request": cand.get("request"), "to": to}
        clip_to = str(op.get("clip_to") or ("bleed" if frame.bleed else "frame"))
        _place(page, frame, layer, cand.get("px") or (1, 1), str(op.get("fit") or "cover"), clip_to,
               tuple(op.get("offset_mm") or (0.0, 0.0)), float(op.get("scale") or 1.0), _pad(cand))
        adopted = panel.setdefault("adopted", {})
        history = panel.setdefault("adopt_history", [])
        if adopted.get(to):
            history.append({"to": to, "candidate": adopted[to]})
        adopted[to] = cand["id"]
        if to == "art":
            panel["status"] = "adopted"
            if page.art_ok and not person:
                page.art_ok = False  # a person approved different art
            for ticket in episode.tickets:
                if ticket.get("kind") == "fix" and ticket.get("status") == "open" and ticket.get("frame_id") == frame.id:
                    ticket["status"] = "done"
                    ticket["closed_by"] = agent
        return

    if name == "unadopt":
        page = _page(episode, op)
        frame = _leaf(page, op.get("frame_id"))
        panel = _panel(frame)
        to = str(op.get("to") or "art")
        layer = _art_layer(page, frame.id, to)
        if layer is None:
            raise _err(f"nothing adopted as {to} in {frame.id}")
        history = panel.setdefault("adopt_history", [])
        previous = next((h for h in reversed(history) if h["to"] == to), None)
        prev_cand = _by_id(panel.get("candidates", []), previous["candidate"]) if previous else None
        if prev_cand is not None:
            history.remove(previous)
            layer.asset = prev_cand["asset"]
            layer.source = {"candidate": prev_cand["id"], "request": prev_cand.get("request"), "to": to}
            _place(page, frame, layer, prev_cand.get("px") or (1, 1), layer.fit, layer.clip_to, (0.0, 0.0), 1.0, _pad(prev_cand))
            panel.setdefault("adopted", {})[to] = prev_cand["id"]
        else:
            page.layers.remove(layer)
            panel.setdefault("adopted", {}).pop(to, None)
            if to == "art":
                panel["status"] = "candidates" if panel.get("candidates") else "briefed"
        if to == "art" and not person:
            page.art_ok = False
        return

    if name == "set_placement":
        page = _page(episode, op)
        frame = _leaf(page, op.get("frame_id"))
        to = str(op.get("to") or "art")
        layer = _art_layer(page, frame.id, to)
        if layer is None:
            raise _err(f"nothing adopted as {to} in {frame.id}")
        cand = _by_id(_panel(frame).get("candidates", []), (layer.source or {}).get("candidate", ""))
        px = cand.get("px") if cand else _asset_size(episode, layer.asset)
        _place(page, frame, layer, px, str(op.get("fit") or layer.fit), str(op.get("clip_to") or layer.clip_to),
               tuple(op.get("offset_mm") or (0.0, 0.0)), float(op.get("scale") or 1.0), _pad(cand or {}))
        return

    if name == "place_asset":
        page = _page(episode, op)
        asset = str(op.get("asset") or "")
        if not asset.startswith("sha256:"):
            raise _err("place_asset takes an asset ref (sha256:…), not a path")
        px = _asset_size(episode, asset)
        to = str(op.get("to") or "art")
        roles = {"art": LayerRole.USER, "bg": LayerRole.BG, "draft": LayerRole.DRAFT, "name": LayerRole.NAME}
        if to not in roles:
            raise _err("to must be art, bg, draft or name")
        if to in ("art", "bg") and episode.strict_gates and not page.name_ok:
            raise _err(f"page {page.index}: approve the name first (strict_gates)")
        frame = _leaf(page, op["frame_id"]) if op.get("frame_id") else None
        layer = Layer(id=new_id(), role=roles[to], kind=LayerKind.PLACED, exportable=to in ("art", "bg"),
                      title=f"placed {to}", frame_id=frame.id if frame else None, asset=asset,
                      source={"asset": asset, "to": to, "by": agent})
        if op.get("placement_mm"):
            # an explicit position (e.g. a scanned name aligned to the page): the whole image goes there
            x, y, w, h = (float(v) for v in op["placement_mm"])
            layer.placement_mm, layer.fit, layer.clip_to = Rect(x, y, w, h), "stretch", str(op.get("clip_to") or "none")
            layer.title = str(op.get("title") or layer.title)
        else:
            _place(page, frame, layer, px, str(op.get("fit") or "cover"), str(op.get("clip_to") or ("frame" if frame else "none")), (0.0, 0.0), 1.0)
            layer.title = str(op.get("title") or layer.title)
        _insert_below_ink(page, layer)
        return

    if name == "set_layout":
        page = _page(episode, op)
        tree = op.get("tree") or {}
        if not tree.get("rect_mm"):
            raise _err("tree needs rect_mm")
        blank = len(page.leaf_frames()) == 1 and not episode.story_for_page(page.index)
        if not blank and not op.get("force"):
            raise _err(f"page {page.index} already has panels or lines; pass force to replace the layout")
        if not blank:
            from genko.ops import _orphan_art, _placed_on

            _orphan_art(episode, page, _placed_on(page, {f.id for f in page.leaf_frames()}), reason="set_layout")
        page.frames = [_frame_from_tree(tree)]
        page.selected_frame_id = None
        return

    if name == "propose":
        proposal = dict(op.get("proposal") or {})
        if proposal.get("kind") not in ("layout", "lines"):
            raise _err("proposal.kind must be layout or lines")
        page = _page(episode, proposal)
        pid = str(proposal.get("id") or "pr_" + new_id()[:10])
        proposals = studio.setdefault("proposals", {})
        if pid in proposals:
            if proposals[pid].get("status") in ("rejected", "superseded"):  # asked again: open it again
                proposals[pid].update({"status": "open", "by": agent, "rev": episode.revision})
                proposals[pid].pop("resolved_by", None)
            return  # the same proposal again
        for other in proposals.values():  # a newer proposal of the same kind for the page replaces an open one
            if other.get("status") == "open" and other.get("kind") == proposal["kind"] and other.get("page_id") == page.id:
                other["status"] = "superseded"
        proposal.update({"id": pid, "page_id": page.id, "page": page.index, "status": "open", "by": agent, "rev": episode.revision})
        proposals[pid] = proposal
        return

    if name == "resolve_proposal":
        if not person:
            raise _err("only a person accepts or rejects a proposal")
        proposal = studio.get("proposals", {}).get(str(op.get("id") or ""))
        if proposal is None:
            raise _err(f"no proposal {op.get('id')}")
        status = str(op.get("status") or "")
        if status not in ("accepted", "rejected"):
            raise _err("status must be accepted or rejected")
        proposal.update({"status": status, "resolved_by": agent, "note": str(op.get("note") or "")})
        return

    if name == "reject_sheet":
        if not person:
            raise _err("reject_sheet needs a person")
        cid = str(op.get("character_id") or "")
        cands = studio.get("character_candidates", {}).get(cid, [])
        wanted = set(op.get("candidate_ids") or [c["id"] for c in cands])
        for cand in cands:
            if cand["id"] in wanted:
                cand["status"] = "rejected"
        _close_tickets(episode, agent, "gate", "sheet", character_id=cid)
        for ticket in episode.tickets:
            if ticket.get("kind") == "gate" and ticket.get("gate") == "sheet" and ticket.get("character_id") == cid and ticket.get("status") == "done":
                ticket["status"] = "returned"
        note = str(op.get("note") or "").strip()
        if note:
            _ticket(episode, None, agent, kind="fix", character_id=cid, text=note, scope="sheet", assignee="agent")
        return

    if name == "ask_human":
        text = str(op.get("text") or "").strip()
        if not text:
            raise _err("text is required")
        page = _page(episode, op) if op.get("page") is not None or op.get("page_id") else None
        if page is not None and op.get("frame_id"):
            _leaf(page, op["frame_id"])
        _ticket(episode, page, agent, kind="help", frame_id=op.get("frame_id"), item=op.get("item"), text=text,
                assignee="human")
        return

    if name == "set_finish":
        page = _page(episode, op)
        frame = _leaf(page, op.get("frame_id"))
        for layer in page.layers:
            if layer.kind == LayerKind.PLACED and layer.frame_id == frame.id:
                layer.finish = op.get("finish")
        return

    raise _err(f"unknown studio op {name}")


def _require_asset(episode: Episode, ref: str) -> None:
    if not ref.startswith("sha256:"):
        raise _err(f"asset must be sha256:…, got {ref!r}")
    if episode.asset_dir is None:
        return
    digest = ref.split(":", 1)[1]
    folder = episode.asset_dir / "assets" / digest[:2]
    if not folder.is_dir() or not any(p.name.startswith(digest) for p in folder.iterdir()):
        raise _err(f"asset {ref} is not in assets/ (import it first)")


def _asset_size(episode: Episode, ref: str) -> tuple[int, int]:
    from PIL import Image

    from genko.assets import AssetStore

    _require_asset(episode, ref)
    if episode.asset_dir is None:
        raise _err("placing an asset needs a saved project")
    data = AssetStore(episode.asset_dir).get_bytes(ref, ".png")
    if data is None:
        raise _err(f"asset {ref} is not a PNG")
    with Image.open(io.BytesIO(data)) as img:
        return img.size


def _import_candidates(episode: Episode, op: dict, agent: str) -> None:
    studio = episode.studio
    request = None
    if op.get("request_id"):
        request = studio.get("requests", {}).get(str(op["request_id"]))
        if request is None:
            raise _err(f"no request {op['request_id']}")
    target = dict((request or {}).get("target") or {})
    for key in ("page", "frame_id", "character_id", "location_id"):
        if op.get(key) is not None:
            target[key] = op[key]
    items = op.get("candidates") or []
    if not items:
        raise _err("candidates is empty")
    prepared = []
    for i, item in enumerate(items):
        origin = dict(item.get("origin") or {})
        if origin.get("kind") not in ORIGIN_KINDS:
            raise _err(f"candidates[{i}].origin.kind must be one of {ORIGIN_KINDS}")
        origin["actor"] = agent
        _require_asset(episode, str(item.get("asset") or ""))
        px = item.get("px")
        if not (isinstance(px, (list, tuple)) and len(px) == 2 and all(int(v) > 0 for v in px)):
            raise _err(f"candidates[{i}].px must be [width, height]")
        prepared.append({
            "id": str(item.get("id") or "cd_" + new_id()[:10]),
            "asset": item["asset"],
            "request": request.get("id") if request else None,
            "mode": str(item.get("mode") or (request or {}).get("mode") or "new"),
            "parent": item.get("parent") or (request or {}).get("parent"),
            "origin": origin,
            "px": [int(px[0]), int(px[1])],
            "status": "candidate",
            **({"metrics": item["metrics"]} if isinstance(item.get("metrics"), dict) else {}),
        })
    if target.get("character_id") or target.get("location_id"):
        key = "character_candidates" if target.get("character_id") else "location_candidates"
        owner = target.get("character_id") or target.get("location_id")
        bucket = studio.setdefault(key, {}).setdefault(owner, [])
        known = {c["asset"] for c in bucket}
        bucket.extend(c for c in prepared if c["asset"] not in known)
        if request is not None:
            request["status"] = "done"
        if target.get("character_id"):
            for ticket in episode.tickets:  # new sheets answer a person's send-back
                if ticket.get("kind") == "fix" and ticket.get("status") == "open" and ticket.get("character_id") == owner:
                    ticket["status"] = "done"
                    ticket["closed_by"] = agent
        return
    page = _page(episode, target)
    try:
        frame = _leaf(page, target.get("frame_id"))
    except Exception:
        studio.setdefault("orphans", []).append({"kind": "candidates", "page_id": page.id, "frame_id": target.get("frame_id"),
                                                 "candidates": prepared, "rev": episode.revision})
        return
    panel = _panel(frame)
    current = brief_hash(panel)
    known = {c["asset"] for c in panel.get("candidates", [])}
    new = [c for c in prepared if c["asset"] not in known]
    for cand in new:
        cand["brief_hash"] = (request or {}).get("brief_hash") or current
        cand["stale"] = cand["brief_hash"] != current
        cand["mapping"] = {"frame_rect_mm": [frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height],
                           "pad_mm": float((request or {}).get("pad_mm") or 0.0), "px": cand["px"]}
    panel.setdefault("candidates", []).extend(new)
    attempts = panel.setdefault("attempts", {"requests": 0, "images": 0, "fix_rounds": 0})
    attempts["images"] += sum(1 for c in new if c["origin"].get("kind") != "genko")  # Genko's own derivations are free
    if panel.get("status") not in ("adopted", "skip"):
        panel["status"] = "candidates"
    if request is not None:
        request["status"] = "done"
