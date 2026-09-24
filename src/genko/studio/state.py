"""Read the agent's state from project.json (M3: no sidecar files).

bible  → episode.bible (plot, characters, constraints) + studio.bible_doc (the rest)
script → studio.script
name   → page.plan: {"name": plan, "input_hash", "rev", "slot_to_frame", "reading_order", "by", "reviews"}
tickets→ episode.tickets (kind "gate" = approval request, kind "fix" = instruction)
"""

from __future__ import annotations

from genko.models import Episode


def bible(episode: Episode) -> dict | None:
    doc = episode.studio.get("bible_doc")
    if doc is None:
        return None
    return {
        **doc,
        "plot": episode.bible.plot,
        "characters": episode.bible.characters,
        "constraints": episode.bible.constraints,
    }


def script(episode: Episode) -> dict | None:
    return episode.studio.get("script")


def name(episode: Episode, index: int) -> dict | None:
    page = next((p for p in episode.pages if p.index == index), None)
    if page is None or not page.plan or "name" not in page.plan:
        return None
    return page.plan


def names(episode: Episode) -> dict[int, dict]:
    return {p.index: p.plan for p in episode.pages if p.plan and "name" in p.plan}


def name_review(episode: Episode, index: int) -> dict | None:
    record = name(episode, index)
    return (record or {}).get("reviews", {}).get("name")


def open_tickets(episode: Episode, kind: str | None = None) -> list[dict]:
    return [t for t in episode.tickets if t.get("status") == "open" and (kind is None or t.get("kind") == kind)]


def page_fixes(episode: Episode, index: int) -> list[dict]:
    """Open instructions from a person for a page (page-level, not a single panel's art)."""
    page = next((p for p in episode.pages if p.index == index), None)
    return [
        t for t in open_tickets(episode, "fix")
        if (t.get("page_id") == (page.id if page else None) or t.get("page_index") == index)
        and not t.get("frame_id") and t.get("assignee") == "agent"
    ]
