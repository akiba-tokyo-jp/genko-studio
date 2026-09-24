"""Tier DSL → frames, through the existing split_frame op.

Tiers run top to bottom; the cols inside a tier are listed right to left
(Japanese reading order), and a col may stack rows. split_frame cannot name
its children, so each split is applied on its own and the new leaves are
read back from the frame tree.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from genko.models import Episode, Page
from genko.ops import ApplyError, apply_ops
from genko.studio.issues import Issue, error

TIER_GUTTER_MM = 7.0
COL_GUTTER_MM = 3.0
SUM_TOLERANCE = 0.02
MAX_PANELS = 8

_TEMPLATES_PATH = Path(__file__).with_name("layouts.json")


class LayoutError(ApplyError):
    def __init__(self, path: str, message: str) -> None:
        super().__init__(f"{path}: {message}")
        self.path = path
        self.message = message


@dataclass
class CompiledLayout:
    slot_to_frame: dict[str, str] = field(default_factory=dict)
    leaf_rects_mm: dict[str, tuple[float, float, float, float]] = field(default_factory=dict)
    reading_order: list[str] = field(default_factory=list)
    source_map: list[tuple[int, str]] = field(default_factory=list)
    ops: list[dict] = field(default_factory=list)


def templates() -> dict:
    return json.loads(_TEMPLATES_PATH.read_text(encoding="utf-8"))


def resolve_tiers(plan: dict) -> list[dict]:
    """The tiers of a plan, expanding `template` when tiers are not given."""
    if plan.get("tiers"):
        return plan["tiers"]
    name = plan.get("template")
    if not name:
        raise LayoutError("/tiers", "tiers か template のどちらかが要る")
    known = templates()
    if name not in known:
        raise LayoutError("/template", f"定型 {name} はない（{', '.join(sorted(known))}）")
    return known[name]["tiers"]


def slots_in_order(tiers: list[dict]) -> list[str]:
    out: list[str] = []
    for tier in tiers:
        for col in tier["cols"]:
            if col.get("rows"):
                out.extend(row["slot"] for row in col["rows"])
            else:
                out.append(col["slot"])
    return out


def check_tiers(plan: dict) -> list[Issue]:
    issues: list[Issue] = []
    try:
        tiers = resolve_tiers(plan)
    except LayoutError as exc:
        return [error("layout_missing", exc.path, exc.message)]
    base = "/tiers" if plan.get("tiers") else "/template"
    if not tiers:
        return [error("layout_empty", base, "段が1つもない")]
    _check_ratios(issues, [t["h"] for t in tiers], base, "h", "段")
    seen: dict[str, str] = {}
    for ti, tier in enumerate(tiers):
        tpath = f"{base}/{ti}"
        if not tier["cols"]:
            issues.append(error("layout_empty", f"{tpath}/cols", "列が1つもない"))
            continue
        _check_ratios(issues, [c["w"] for c in tier["cols"]], f"{tpath}/cols", "w", "列")
        for ci, col in enumerate(tier["cols"]):
            cpath = f"{tpath}/cols/{ci}"
            rows = col.get("rows")
            if rows:
                _check_ratios(issues, [r["h"] for r in rows], f"{cpath}/rows", "h", "行")
                for ri, row in enumerate(rows):
                    _check_slot(issues, seen, row["slot"], f"{cpath}/rows/{ri}/slot")
            else:
                _check_slot(issues, seen, col["slot"], f"{cpath}/slot")
    if len(seen) > MAX_PANELS:
        issues.append(error("layout_too_many", base, f"コマが {len(seen)} 個ある（1ページ {MAX_PANELS} 個まで）"))
    panel_slots = [p["slot"] for p in plan.get("panels", [])]
    for pi, slot in enumerate(panel_slots):
        if slot not in seen:
            issues.append(error("panel_unknown_slot", f"/panels/{pi}/slot", f"段組に {slot} というコマがない"))
        elif panel_slots.count(slot) > 1:
            issues.append(error("panel_duplicate_slot", f"/panels/{pi}/slot", f"{slot} のコマ指示が2つある"))
    for slot, where in seen.items():
        if slot not in panel_slots:
            issues.append(error("panel_missing", where, f"コマ {slot} の指示（panels）がない"))
    return issues


def _check_ratios(issues: list[Issue], values: list[float], path: str, key: str, label: str) -> None:
    for index, value in enumerate(values):
        if not 0 < value <= 1:
            issues.append(error("layout_ratio_range", f"{path}/{index}/{key}", f"{label}の {key} は 0 より大きく 1 以下にする（今 {value}）"))
    total = sum(values)
    if abs(total - 1.0) > SUM_TOLERANCE:
        issues.append(error("layout_ratio_sum", path, f"{label}の {key} の合計が {total:.2f}", f"合計が 1.0 になるように直す"))


def _check_slot(issues: list[Issue], seen: dict[str, str], slot: str, path: str) -> None:
    if not slot:
        issues.append(error("layout_slot_empty", path, "slot が空"))
    elif slot in seen:
        issues.append(error("layout_slot_duplicate", path, f"slot {slot} が重複している"))
    else:
        seen[slot] = path


def is_blank(episode: Episode, page: Page) -> bool:
    return len(page.leaf_frames()) == 1 and not episode.story_for_page(page.index)


def clear_page(episode: Episode, page: Page, *, agent: str) -> list[dict]:
    """Ops that remove all lines and merge the frames back to the root."""
    ops: list[dict] = [{"op": "delete_line", "id": line.id} for line in list(episode.story_for_page(page.index))]
    root = page.frames[0]
    if root.children:
        ops.append({"op": "merge_frame", "page": page.index, "frame_id": root.children[0].id})
    # merge_frame only collapses one level: the root keeps no children afterwards.
    if ops:
        _apply(episode, ops, agent)
    return ops


def apply_layout(episode: Episode, page_index: int, plan: dict, *, agent: str = "ai:agent") -> CompiledLayout:
    """Split a blank page into the plan's panels. Mutates `episode` (pass a copy for dry runs)."""
    page = _page(episode, page_index)
    if not is_blank(episode, page):
        raise LayoutError("/", f"{page_index} ページはすでにコマか台詞がある（replace:true で作り直す）")
    problems = [issue for issue in check_tiers(plan) if issue.severity == "error"]
    if problems:
        raise LayoutError(problems[0].path, problems[0].message)
    tiers = resolve_tiers(plan)
    base = "/tiers" if plan.get("tiers") else "/template"
    out = CompiledLayout()
    tier_ids = _split_sequence(
        episode, page_index, page.frames[0].id, [t["h"] for t in tiers], "horizontal", TIER_GUTTER_MM, agent, out, base
    )
    for ti, (tier, tier_id) in enumerate(zip(tiers, tier_ids)):
        cols = tier["cols"]
        col_ids = _split_sequence(
            episode, page_index, tier_id, [c["w"] for c in cols], "vertical", COL_GUTTER_MM, agent, out, f"{base}/{ti}/cols"
        )
        for ci, (col, col_id) in enumerate(zip(cols, col_ids)):
            rows = col.get("rows")
            if rows:
                row_ids = _split_sequence(
                    episode, page_index, col_id, [r["h"] for r in rows], "horizontal", TIER_GUTTER_MM, agent, out,
                    f"{base}/{ti}/cols/{ci}/rows",
                )
                for row, frame_id in zip(rows, row_ids):
                    out.slot_to_frame[row["slot"]] = frame_id
            else:
                out.slot_to_frame[col["slot"]] = col_id
    by_frame = {frame_id: slot for slot, frame_id in out.slot_to_frame.items()}
    for frame in _page(episode, page_index).leaf_frames():
        slot = by_frame[frame.id]
        out.reading_order.append(slot)
        r = frame.rect
        out.leaf_rects_mm[slot] = (round(r.x, 3), round(r.y, 3), round(r.width, 3), round(r.height, 3))
    return out


def _split_sequence(
    episode: Episode,
    page_index: int,
    frame_id: str,
    ratios: list[float],
    axis: str,
    gutter: float,
    agent: str,
    out: CompiledLayout,
    path: str,
) -> list[str]:
    """Split a frame into len(ratios) parts and return their ids in reading order.

    Horizontal: parts run top to bottom and the top part is child a.
    Vertical: parts are listed right to left; the right part is child b.
    apply_ops swaps in new objects, so frames are looked up by id after each step.
    """
    if len(ratios) == 1:
        return [frame_id]
    frame = _page(episode, page_index)._find(frame_id)
    total = sum(ratios)
    extent = frame.rect.height if axis == "horizontal" else frame.rect.width
    usable = extent - gutter * (len(ratios) - 1)
    spans = [usable * r / total for r in ratios]
    parts: list[str] = []
    remaining = frame_id
    for index, span in enumerate(spans[:-1]):
        rect = _page(episode, page_index)._find(remaining).rect
        rest_extent = (rect.height if axis == "horizontal" else rect.width) - gutter
        ratio = span / rest_extent if axis == "horizontal" else (rest_extent - span) / rest_extent
        op = {"op": "split_frame", "page": page_index, "frame_id": remaining, "axis": axis, "ratio": ratio, "gutter_mm": gutter}
        out.source_map.append((len(out.ops), f"{path}/{index}"))
        out.ops.append(op)
        _apply(episode, [op], agent)
        a, b = _page(episode, page_index)._find(remaining).children
        if axis == "horizontal":
            parts.append(a.id)
            remaining = b.id
        else:
            parts.append(b.id)
            remaining = a.id
    parts.append(remaining)
    return parts


def _apply(episode: Episode, ops: list[dict], agent: str) -> None:
    episode.undo_stack.clear()  # the compile runs many small batches; keep deepcopy cheap
    apply_ops(episode, ops, agent=agent)


def _page(episode: Episode, index: int) -> Page:
    for page in episode.pages:
        if page.index == index:
            return page
    raise LayoutError("/page", f"{index} ページはない（1〜{len(episode.pages)}）")

