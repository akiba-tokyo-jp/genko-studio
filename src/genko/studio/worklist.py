"""What to do next, computed from the project state alone (no stored "done" marks)."""

from __future__ import annotations

from genko.models import Episode
from genko.studio.jsonutil import content_hash, sha256_hex


def page_side(page: int) -> dict:
    """In both right- and left-bound books the even page is seen first after a turn."""
    if page == 1:
        return {"side": "first", "turn": "1ページ目（表紙の次）"}
    if page % 2 == 0:
        return {"side": "after_turn", "turn": "めくってすぐ見えるページ（reveal を置く）"}
    return {"side": "before_turn", "turn": "めくる前のページ（最後のコマに hook を置く）"}


def item(kind: str, why: str, tools: list[str], page: int | None = None, blocked_by: list[str] | None = None, **extra) -> dict:
    target = {"page": page} if page is not None else {}
    for key in ("frame_id", "character_id"):
        if extra.get(key) is not None:
            target[key] = extra.pop(key)
    return {
        "id": "wi_" + sha256_hex(f"{kind}:{page}:{target.get('frame_id') or target.get('character_id') or ''}:{extra.get('input_hash', '')}")[:10],
        "kind": kind,
        "target": target,
        "why": why,
        "tools": tools,
        "blocked_by": blocked_by or [],
        **{k: v for k, v in extra.items() if k != "input_hash"},
    }


def next_actions(episode: Episode) -> list[dict]:
    from genko.studio import state

    if state.bible(episode) is None:
        return [item("write_bible", "企画書（bible）がまだない", ["inspect", "set_bible"])]
    if state.script(episode) is None:
        return [item("write_script", "脚本がまだない", ["inspect", "set_script"])]
    out: list[dict] = []
    requested = {(t.get("gate"), p) for t in state.open_tickets(episode, "gate") for p in t.get("pages") or []}
    requested_sheets = {t.get("character_id") for t in state.open_tickets(episode, "gate") if t.get("gate") == "sheet"}
    for page in episode.pages:
        n = page.index
        draft = state.name(episode, n)
        fixes = state.page_fixes(episode, n)
        if fixes and not page.name_ok:
            out.append(item("revise_page", "人間から修正の指示がある", ["inspect", "render", "submit_name"], n,
                            comments=[t.get("text", "") for t in fixes], tickets=[t["id"] for t in fixes]))
            continue
        if page.name_ok:
            out.extend(_art_items(episode, page, requested, requested_sheets))
            continue
        if draft is None:
            blank = len(page.leaf_frames()) == 1 and not episode.story_for_page(n)
            if blank:
                out.append(item("plan_page", "ネームがまだない", ["inspect", "submit_name", "render"], n, **page_side(n)))
            continue
        review = (draft.get("reviews") or {}).get("name")
        if not review or review.get("input_hash") != draft.get("input_hash"):
            out.append(item("review_name", "今のネームをまだ自己点検していない", ["render", "record_review", "submit_name"], n,
                            input_hash=draft.get("input_hash", "")))
            continue
        blocker = "requested" if ("name", n) in requested else "await_human:name"
        out.append(item("await_human", "人間のネーム承認待ち", ["request_approval"], n, blocked_by=[blocker], gate="name"))
    return _dedupe(out)


def _art_items(episode: Episode, page, requested: set, requested_sheets: set) -> list[dict]:
    """Per-panel art work on a page whose name is approved."""
    if page.art_ok:
        return []
    chars = {c.get("id"): c for c in episode.bible.characters}
    out: list[dict] = []
    waiting = 0
    for frame in page.leaf_frames():
        panel = frame.panel or {}
        status = panel.get("status", "empty")
        if status in ("adopted", "skip"):
            continue
        waiting += 1
        target = {"frame_id": frame.id}
        cast = [c.get("id") for c in panel.get("characters", []) if isinstance(c, dict)]
        unlocked = [cid for cid in cast if cid in chars and not chars[cid].get("locked")]
        if unlocked:
            for cid in unlocked:
                if cid in requested_sheets:
                    out.append(item("await_human", "キャラクター設定画の承認待ち", ["request_approval"], None,
                                    blocked_by=["requested"], gate="sheet", character_id=cid))
                else:
                    out.append(item("make_sheet", "作画の前にキャラクター設定画を承認してもらう",
                                    ["import_image", "import_candidates", "request_approval"], None, character_id=cid))
            continue
        if status == "candidates":
            out.append(item("choose_art", "候補画像から採用するものを選ぶ", ["render", "review_candidates", "adopt_candidate"],
                            page.index, **target))
        elif status == "fix_requested":
            fixes = [t.get("text", "") for t in episode.tickets
                     if t.get("status") == "open" and t.get("kind") == "fix" and t.get("frame_id") == frame.id]
            out.append(item("fix_art", "人間からコマの修正指示がある", ["inspect", "import_image", "import_candidates", "adopt_candidate"],
                            page.index, comments=fixes, **target))
        elif status == "requested":
            out.append(item("import_art", "依頼した画像を取り込む", ["import_image", "import_candidates"], page.index,
                            **target))
        else:
            out.append(item("make_art", "このコマの絵がまだない（外部で生成して取り込む）",
                            ["inspect", "import_image", "import_candidates", "adopt_candidate"], page.index,
                            **target))
    if not waiting:
        blocker = "requested" if ("art", page.index) in requested else "await_human:art"
        out.append(item("await_human", "人間の作画承認待ち", ["render", "request_approval"], page.index, blocked_by=[blocker], gate="art"))
    return out


def _dedupe(items: list[dict]) -> list[dict]:
    seen: set[str] = set()
    out = []
    for entry in items:
        if entry["id"] not in seen:
            seen.add(entry["id"])
            out.append(entry)
    return out


def plan_hash(plan: dict) -> str:
    return content_hash(plan)
