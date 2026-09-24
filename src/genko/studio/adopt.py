"""`genko studio adopt-drafts`: move M0 sidecar files (studio/drafts/) into project.json.

The bible, script, name plans (as panel briefs on the leaf frames), self-checks,
approval requests and open comments become ops in one save. The folder is then
renamed to studio/drafts.adopted so it is not read again.
"""

from __future__ import annotations

from pathlib import Path

from genko.io import load_episode, save_episode
from genko.lock import ProjectLock
from genko.ops import apply_ops
from genko.studio import state
from genko.studio.drafts import Drafts

ACTOR = "system:adopt-drafts"
BRIEF_FROM_PLAN = ("shot", "angle", "characters", "location_id", "time", "action", "emotion", "fx", "emphasis", "beat_ids")


def adopt_drafts(project: Path) -> dict:
    project = Path(project)
    drafts = Drafts(project)
    if not drafts.root.is_dir():
        return {"ok": False, "error": f"{drafts.root} がない（M0 の下書きはない）"}
    counts = {"bible": 0, "script": 0, "names": 0, "reviews": 0, "tickets": 0}
    with ProjectLock(project, agent=ACTOR):
        episode = load_episode(project)
        ops: list[dict] = []
        bible = drafts.bible()
        if bible is not None and state.bible(episode) is None:
            doc = {k: v for k, v in bible.items() if k not in ("plot", "characters", "constraints")}
            ops += [{"op": "set_bible", "plot": bible.get("plot", ""), "characters": bible.get("characters", []),
                     "constraints": bible.get("constraints", [])},
                    {"op": "set_studio", "bible_doc": doc}]
            counts["bible"] = 1
        script = drafts.script()
        if script is not None and state.script(episode) is None:
            ops.append({"op": "set_script", "script": script})
            counts["script"] = 1
        reviews = drafts.reviews()
        for page in episode.pages:
            record = drafts.name(page.index)
            if record is None or state.name(episode, page.index) is not None:
                continue
            plan = record.get("plan") or {}
            slots = record.get("slot_to_frame") or {}
            leaves = {f.id for f in page.leaf_frames()}
            for panel in plan.get("panels", []):
                frame_id = slots.get(panel.get("slot"))
                if frame_id in leaves:
                    ops.append({"op": "set_panel", "page": page.index, "frame_id": frame_id,
                                "set": {"slot": panel["slot"], **{k: panel[k] for k in BRIEF_FROM_PLAN if k in panel}}})
            ops.append({"op": "set_page_plan", "page": page.index, "plan": {
                "name": plan, "input_hash": record.get("input_hash"), "rev": episode.revision,
                "turn_role": plan.get("turn_role"), "slot_to_frame": slots,
                "reading_order": record.get("reading_order", []), "by": record.get("by", ACTOR),
            }})
            counts["names"] += 1
            review = reviews.get(str(page.index))
            if review:
                ops.append({"op": "record_review", "page": page.index, "kind": "name", "score": review.get("score"),
                            "notes": review.get("notes", ""), "input_hash": review.get("input_hash")})
                counts["reviews"] += 1
        if ops:
            apply_ops(episode, ops, agent=ACTOR)
        # tickets keep who asked: approval requests by the agent, comments by the person
        for item in drafts.requests():
            if item.get("status") != "open":
                continue
            if item.get("kind") == "approval":
                apply_ops(episode, [{"op": "request_approval", "gate": item.get("gate", "name"), "pages": item.get("pages", []),
                                     "note": item.get("note", "")}], agent=str(item.get("by") or ACTOR))
            elif item.get("kind") == "comment" and any(p.index == item.get("page") for p in episode.pages):
                record = state.name(episode, item["page"])
                if record is not None and (drafts.name(item["page"]) or {}).get("seq", 0) > item.get("seq", 0):
                    continue  # the name was resubmitted after the comment
                apply_ops(episode, [{"op": "request_fix", "page": item["page"], "instruction": item.get("text", ""),
                                     "scope": "page"}], agent=str(item.get("by") or "human"))
            else:
                continue
            counts["tickets"] += 1
        save_episode(episode, project, actor=ACTOR)
        drafts.root.rename(drafts.root.with_name("drafts.adopted"))
    return {"ok": True, "adopted": counts}
