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
    return {
        "id": "wi_" + sha256_hex(f"{kind}:{page}:{extra.get('input_hash', '')}")[:10],
        "kind": kind,
        "target": target,
        "why": why,
        "tools": tools,
        "blocked_by": blocked_by or [],
        **{k: v for k, v in extra.items() if k != "input_hash"},
    }


def next_actions(episode: Episode, bible: dict | None, script: dict | None, names: dict[int, dict],
                 reviews: dict, requests: list[dict]) -> list[dict]:
    if bible is None:
        return [item("write_bible", "企画書（bible）がまだない", ["inspect", "set_bible"])]
    if script is None:
        return [item("write_script", "脚本がまだない", ["inspect", "set_script"])]
    out: list[dict] = []
    open_comments = [r for r in requests if r.get("kind") == "comment" and r.get("status") == "open"]
    requested = {(r.get("gate"), p) for r in requests if r.get("kind") == "approval" and r.get("status") == "open" for p in r.get("pages", [])}
    for page in episode.pages:
        n = page.index
        draft = names.get(n)
        comments = [c for c in open_comments if c.get("page") == n and (draft is None or draft.get("seq", 0) < c.get("seq", 0))]
        if comments:
            out.append(item("revise_page", "人間から修正の指示がある", ["inspect", "render", "submit_name"], n,
                            comments=[c.get("text", "") for c in comments]))
            continue
        if page.name_ok:
            continue
        if draft is None:
            blank = len(page.leaf_frames()) == 1 and not episode.story_for_page(n)
            if blank:
                out.append(item("plan_page", "ネームがまだない", ["inspect", "submit_name", "render"], n, **page_side(n)))
            continue
        review = reviews.get(str(n))
        if not review or review.get("input_hash") != draft.get("input_hash"):
            out.append(item("review_name", "今のネームをまだ自己点検していない", ["render", "record_review", "submit_name"], n,
                            input_hash=draft.get("input_hash", "")))
            continue
        blocker = "requested" if ("name", n) in requested else "await_human:name"
        out.append(item("await_human", "人間のネーム承認待ち", ["request_approval"], n, blocked_by=[blocker], gate="name"))
    return out


def plan_hash(plan: dict) -> str:
    return content_hash(plan)
