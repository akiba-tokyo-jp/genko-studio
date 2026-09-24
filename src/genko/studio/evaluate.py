"""Measuring real agent runs (§10.8): stats, the approval audit, and the blind character test (D5).

- stats: name resubmissions per page, images and fix rounds per panel (median), tool calls
  that failed and why (from studio/logs/tools.jsonl), tickets.
- audit: every approval change (studio/audit.jsonl) must come from a person.
- eval_sample / eval_score: panels with one character, cropped from the adopted art, shown
  next to the approved sheets under letters; evaluators name the character; the score says
  how often they were right (target ≥ 90%).
"""

from __future__ import annotations

import csv
import html
import io
import json
import random
import statistics
from pathlib import Path

from PIL import Image

from genko import journal
from genko.assets import AssetStore
from genko.io import load_episode
from genko.ops import can_approve
from genko.studio import toollog

TARGET_ACCURACY = 0.9


def stats(project: Path) -> dict:
    project = Path(project)
    episode = load_episode(project)
    pages = []
    panels = []
    for page in episode.pages:
        plan = page.plan or {}
        pages.append({"page": page.index, "name_submits": int(plan.get("submits", 1 if plan.get("name") else 0)),
                      "name_ok": page.name_ok, "art_ok": page.art_ok, "stage": page.stage})
        for frame in page.leaf_frames():
            panel = frame.panel or {}
            if not panel:
                continue
            attempts = panel.get("attempts") or {}
            panels.append({"page": page.index, "frame_id": frame.id, "slot": panel.get("slot"), "status": panel.get("status"),
                           "characters": len([c for c in panel.get("characters", []) if isinstance(c, dict)]),
                           "requests": attempts.get("requests", 0), "images": attempts.get("images", 0),
                           "fix_rounds": attempts.get("fix_rounds", 0)})
    adopted = [p["images"] for p in panels if p["status"] == "adopted"]
    calls = toollog.read(project)
    failed = [c for c in calls if not c.get("ok")]
    by_tool: dict[str, dict] = {}
    for call in calls:
        row = by_tool.setdefault(call["tool"], {"calls": 0, "failed": 0})
        row["calls"] += 1
        row["failed"] += 0 if call.get("ok") else 1
    codes: dict[str, int] = {}
    for call in failed:
        for code in call.get("codes") or [call.get("error", "error")[:60]]:
            codes[code] = codes.get(code, 0) + 1
    ms = sorted(c.get("ms", 0) for c in calls)
    tickets: dict[str, int] = {}
    for ticket in episode.tickets:
        tickets[ticket.get("kind", "?")] = tickets.get(ticket.get("kind", "?"), 0) + 1
    return {
        "ok": True,
        "pages": pages,
        "name_submits_total": sum(p["name_submits"] for p in pages),
        "panels": panels,
        "images_per_adopted_panel": {"median": statistics.median(adopted) if adopted else None,
                                     "max": max(adopted) if adopted else None, "panels": len(adopted)},
        "fix_rounds_total": sum(p["fix_rounds"] for p in panels),
        "tool_calls": {"total": len(calls), "failed": len(failed), "by_tool": by_tool, "failure_reasons": codes,
                       "ms_p50": ms[len(ms) // 2] if ms else None, "ms_p95": ms[int(len(ms) * 0.95)] if ms else None},
        "tickets": tickets,
    }


def audit(project: Path) -> dict:
    """Did anyone but a person change an approval? Reads studio/audit.jsonl and the approval records."""
    project = Path(project)
    entries = journal.audit_entries(project)
    violations = [e for e in entries if not can_approve(str(e.get("actor", "")))]
    episode = load_episode(project)
    records = [a for a in episode.studio.get("approvals", []) if not can_approve(str(a.get("by", "")))]
    by_actor: dict[str, int] = {}
    for entry in entries:
        by_actor[entry.get("actor", "?")] = by_actor.get(entry.get("actor", "?"), 0) + len(entry.get("changes", []))
    return {
        "ok": not violations and not records,
        "changes": sum(len(e.get("changes", [])) for e in entries),
        "changes_by_actor": by_actor,
        "violations": violations + [{"approval": r} for r in records],
        "note": None if (project / journal.AUDIT).is_file() else "studio/audit.jsonl が無い（M5 より前の保存だけのプロジェクト）",
    }


# --- D5: blind character identification ------------------------------------------------


def _crop_art(store: AssetStore, cand: dict, frame, long_side: int = 512) -> Image.Image | None:
    from genko.guide import fit_cover

    data = store.get_bytes(cand["asset"], ".png")
    if data is None:
        return None
    image = Image.open(io.BytesIO(data)).convert("RGB")
    aspect = frame.rect.width / frame.rect.height
    size = (long_side, max(1, round(long_side / aspect))) if aspect >= 1 else (max(1, round(long_side * aspect)), long_side)
    return fit_cover(image, size)  # the panel as printed, without balloons (names in text would give it away)


def eval_sample(project: Path, out: Path, per_character: int = 10, seed: int = 0) -> dict:
    """Up to `per_character` single-character panels per approved character (D5: 10 panels × main characters)."""
    project, out = Path(project), Path(out)
    episode = load_episode(project)
    store = AssetStore(project)
    chars = [c for c in episode.bible.characters if c.get("locked") and any(r.get("kind") == "sheet" for r in c.get("refs", []))]
    if len(chars) < 2:
        return {"ok": False, "error": "設定画が承認された登場人物が 2 人以上要る"}
    rng = random.Random(seed)
    letters = [chr(ord("A") + i) for i in range(len(chars))]
    shuffled = chars[:]
    rng.shuffle(shuffled)
    letter_of = {c["id"]: letters[i] for i, c in enumerate(shuffled)}
    pools: dict[str, list] = {c["id"]: [] for c in chars}
    for page in episode.pages:
        for frame in page.leaf_frames():
            panel = frame.panel or {}
            cast = [c.get("id") for c in panel.get("characters", []) if isinstance(c, dict)]
            art = (panel.get("adopted") or {}).get("art")
            if len(cast) != 1 or cast[0] not in pools or not art:
                continue
            cand = next((c for c in panel.get("candidates", []) if c.get("id") == art), None)
            if cand is not None:
                pools[cast[0]].append((page, frame, cand))
    for pool in pools.values():
        rng.shuffle(pool)
    chosen = [(cid, *item) for cid in sorted(pools, key=lambda c: letter_of[c]) for item in pools[cid][:per_character]]
    rng.shuffle(chosen)
    (out / "sheets").mkdir(parents=True, exist_ok=True)
    (out / "samples").mkdir(parents=True, exist_ok=True)
    for char in chars:
        ref = next(r for r in char["refs"] if r.get("kind") == "sheet")
        data = store.get_bytes(ref["asset"], ".png")
        if data is not None:
            image = Image.open(io.BytesIO(data)).convert("RGB")
            image.thumbnail((640, 640))
            image.save(out / "sheets" / f"{letter_of[char['id']]}.png")
    key = {"seed": seed, "letters": {letter_of[c["id"]]: c["id"] for c in chars}, "samples": {}}
    for i, (cid, page, frame, cand) in enumerate(chosen, start=1):
        name = f"s{i:02d}"
        image = _crop_art(store, cand, frame)
        if image is None:
            continue
        image.save(out / "samples" / f"{name}.png")
        key["samples"][name] = {"answer": letter_of[cid], "character": cid, "page": page.index, "frame_id": frame.id}
    (out / "key.json").write_text(json.dumps(key, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with (out / "answers.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["evaluator", "sample", "choice"])
        for name in key["samples"]:
            writer.writerow(["", name, ""])
    (out / "form.html").write_text(_form(sorted(key["letters"]), list(key["samples"])), encoding="utf-8")
    return {"ok": True, "out": str(out), "samples": len(key["samples"]), "characters": len(chars),
            "per_character": {letter_of[c["id"]]: sum(1 for s in key["samples"].values() if s["character"] == c["id"]) for c in chars},
            "short": any(len(pools[c["id"]]) < per_character for c in chars)}


def _form(letters: list[str], samples: list[str]) -> str:
    parts = ["<!doctype html><html lang='ja'><meta charset='utf-8'><title>キャラクター同定</title>",
             "<style>body{font-family:sans-serif;margin:16px}.row{display:flex;gap:12px;flex-wrap:wrap}"
             "img{max-width:240px;border:1px solid #999}.s{margin:12px 0;border-top:1px solid #ccc;padding-top:8px}</style>",
             "<h1>キャラクター同定（盲検）</h1><p>上の設定画だけを見て、各コマの人物がどれかを選び、answers.csv に書く"
             "（evaluator に自分の名前、choice に文字）。</p><div class='row'>"]
    for letter in letters:
        parts.append(f"<div><h2>{letter}</h2><img src='sheets/{letter}.png' alt='{letter}'></div>")
    parts.append("</div>")
    for name in samples:
        parts.append(f"<div class='s'><h3>{html.escape(name)}</h3><img src='samples/{name}.png' alt='{name}'></div>")
    parts.append("</html>")
    return "".join(parts)


def eval_score(folder: Path, answers: Path) -> dict:
    key = json.loads((Path(folder) / "key.json").read_text(encoding="utf-8"))
    truth = {name: sample["answer"] for name, sample in key["samples"].items()}
    per_evaluator: dict[str, list[int]] = {}
    per_character: dict[str, list[int]] = {}
    with Path(answers).open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            who, name, choice = (row.get("evaluator") or "").strip(), (row.get("sample") or "").strip(), (row.get("choice") or "").strip().upper()
            if not who or name not in truth or not choice:
                continue
            hit = int(choice == truth[name])
            per_evaluator.setdefault(who, []).append(hit)
            per_character.setdefault(key["samples"][name]["character"], []).append(hit)
    hits = [h for rows in per_evaluator.values() for h in rows]
    accuracy = sum(hits) / len(hits) if hits else None
    return {
        "ok": True,
        "answers": len(hits),
        "accuracy": round(accuracy, 3) if accuracy is not None else None,
        "passed": accuracy is not None and accuracy >= TARGET_ACCURACY,
        "target": TARGET_ACCURACY,
        "by_evaluator": {k: round(sum(v) / len(v), 3) for k, v in per_evaluator.items()},
        "by_character": {k: round(sum(v) / len(v), 3) for k, v in per_character.items()},
    }


# --- D8: panels recovered from scanned names ----------------------------------------------------------------


def _edge_error(a: list[float], b: list[float]) -> float:
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[0] + a[2] - b[0] - b[2]), abs(a[1] + a[3] - b[1] - b[3]))


def eval_atari(truth_path: Path, tolerance_mm: float = 5.0) -> dict:
    """truth JSON: {"page_mm": [w, h]?, "align": "page"|"live"|"auto"?, "pages": [{"scan": path, "panels": [[x,y,w,h], …]}]}.
    A panel counts when a detected panel matches it within `tolerance_mm` on every edge (target: 90%)."""
    from genko.models import PageSpec, new_episode
    from genko.studio import atari, xycut

    truth_path = Path(truth_path)
    truth = json.loads(truth_path.read_text(encoding="utf-8"))
    spec = PageSpec.b4_comic()
    if truth.get("page_mm"):
        from dataclasses import replace

        spec = replace(spec, width_mm=float(truth["page_mm"][0]), height_mm=float(truth["page_mm"][1]))
    page = new_episode("eval", 1, 1, spec).pages[0]
    rows, hits, total = [], 0, 0
    for item in truth.get("pages", []):
        scan = Path(item["scan"])
        scan = scan if scan.is_absolute() else truth_path.parent / scan
        image = Image.open(scan)
        placement, used = atari.placement_for(image.convert("L"), page, truth.get("align", "auto"))
        found = xycut.detect(image, tuple(placement)).leaves_mm
        matched = sum(1 for t in item["panels"] if any(_edge_error(d["rect_mm"], t) <= tolerance_mm for d in found))
        hits += matched
        total += len(item["panels"])
        rows.append({"scan": str(item["scan"]), "panels": len(item["panels"]), "detected": len(found), "matched": matched, "align": used})
    rate = hits / total if total else None
    return {"ok": True, "pages": rows, "matched": hits, "panels": total, "rate": round(rate, 3) if rate is not None else None,
            "passed": rate is not None and rate >= 0.9, "tolerance_mm": tolerance_mm}
