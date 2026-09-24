"""What the approval box and the process bar show, and the ops their buttons send (Qt-free).

People approve one request at a time after looking at it: there is no bulk
button. Sending back turns into instructions the agent sees in `next`:
a page fix for a name, panel fixes for art, a rejected sheet with a note.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from genko.models import Episode
from genko.studio import state


@dataclass
class InboxItem:
    ticket_id: str
    kind: str                # gate | help | proposal
    gate: str | None
    pages: list[int] = field(default_factory=list)
    character_id: str | None = None
    frame_id: str | None = None
    text: str = ""
    by: str = ""
    name: str = ""  # the character's name for sheet requests

    @property
    def title(self) -> str:
        if self.kind == "help":
            where = f"{self.pages[0]} ページ" if self.pages else "全体"
            return f"相談（{where}）"
        if self.kind == "proposal":
            label = {"layout": "コマ割り", "lines": "台詞"}.get(self.gate or "", self.gate or "")
            return f"アタリからの提案: {self.pages[0]} ページの{label}"
        label = {"name": "ネーム", "art": "作画", "sheet": "設定画", "export": "書き出し"}.get(self.gate or "", self.gate or "")
        if self.gate == "sheet":
            return f"{label}の承認: {self.name or self.character_id}"
        if self.pages:
            return f"{label}の承認: {', '.join(map(str, self.pages))} ページ"
        return f"{label}の承認"


def inbox(episode: Episode) -> list[InboxItem]:
    items = []
    names = {c.get("id"): c.get("name") for c in episode.bible.characters}
    for ticket in state.open_tickets(episode):
        kind = ticket.get("kind")
        if kind not in ("gate", "help"):
            continue
        pages = list(ticket.get("pages") or ([ticket["page_index"]] if ticket.get("page_index") else []))
        items.append(InboxItem(ticket["id"], kind, ticket.get("gate"), pages, ticket.get("character_id"),
                               ticket.get("frame_id"), str(ticket.get("text") or ""), str(ticket.get("created_by") or ""),
                               str(names.get(ticket.get("character_id")) or "")))
    for proposal in (episode.studio.get("proposals") or {}).values():
        if proposal.get("status") != "open":
            continue
        page = next((p for p in episode.pages if p.id == proposal.get("page_id")), None)
        count = len(proposal.get("panels") or proposal.get("lines") or [])
        what = f"{count} コマ（信頼度 {proposal.get('confidence', 0):.2f}）" if proposal["kind"] == "layout" else f"{count} 本の台詞"
        items.append(InboxItem(proposal["id"], "proposal", proposal["kind"], [page.index] if page else [], text=what,
                               by=str(proposal.get("source") or proposal.get("by") or "")))
    return items


def progress(episode: Episode) -> list[tuple[str, int]]:
    """Counts for the process bar."""
    counts = {"ネーム未提出": 0, "ネーム承認待ち": 0, "作画中": 0, "作画承認待ち": 0, "仕上げ待ち": 0, "仕上げ済み": 0}
    for page in episode.pages:
        if not page.name_ok:
            counts["ネーム承認待ち" if state.name(episode, page.index) else "ネーム未提出"] += 1
        elif not page.art_ok:
            leaves = page.leaf_frames()
            done = all((f.panel or {}).get("status") in ("adopted", "skip", None) for f in leaves if f.panel)
            counts["作画承認待ち" if done and any(f.panel for f in leaves) else "作画中"] += 1
        elif page.stage != "finish":
            counts["仕上げ待ち"] += 1
        else:
            counts["仕上げ済み"] += 1
    sheets = [c for c in episode.bible.characters if not c.get("locked")]
    out = [("設定画未承認", len(sheets))] if episode.bible.characters else []
    return out + list(counts.items())


def approve_ops(episode: Episode, item: InboxItem, *, project=None, candidate_id: str | None = None,
                face_box01: list[float] | None = None) -> list[dict]:
    """The ops for 承認. Export is not an op (the GUI runs the checked export instead)."""
    if item.kind == "help":
        return [{"op": "set_ticket", "id": item.ticket_id, "status": "done"}]
    if item.kind == "proposal":
        from genko.studio import atari

        proposal = episode.studio["proposals"][item.ticket_id]
        page = next(p for p in episode.pages if p.id == proposal["page_id"])
        return atari.assign_frames(atari.accept_ops(episode, proposal), page)
    if item.gate in ("name", "art"):
        return [{"op": "approve", "gate": item.gate, "page": p} for p in item.pages]
    if item.gate == "sheet":
        if not candidate_id:
            raise ValueError("設定画の候補を 1 つ選んでから承認します")
        from genko.studio.service import sheet_approval_op

        return [sheet_approval_op(project, episode, item.character_id or "", candidate_id, face_box01)]
    if item.gate == "export":
        return []
    return [{"op": "set_ticket", "id": item.ticket_id, "status": "done"}]


def send_back_ops(episode: Episode, item: InboxItem, text: str, frame_ids: list[str] | None = None) -> list[dict]:
    """The ops for 差し戻し: the request is closed as returned and the reason reaches the agent."""
    text = text.strip()
    if not text:
        raise ValueError("理由（返事）を書いてから送ります")
    if item.kind == "proposal":
        return [{"op": "resolve_proposal", "id": item.ticket_id, "status": "rejected", "note": text}]
    ops: list[dict] = [{"op": "set_ticket", "id": item.ticket_id, "status": "returned"}]
    if item.kind == "help":
        ops = [{"op": "set_ticket", "id": item.ticket_id, "status": "done"}]
        if item.pages:
            ops.append({"op": "request_fix", "page": item.pages[0], "frame_id": item.frame_id, "instruction": text,
                        "scope": "frame" if item.frame_id else "page"})
        return ops
    if item.gate == "name":
        ops += [{"op": "request_fix", "page": p, "instruction": text, "scope": "page"} for p in item.pages]
    elif item.gate == "art":
        for p in item.pages:
            page = next(pg for pg in episode.pages if pg.index == p)
            targets = frame_ids or [f.id for f in page.leaf_frames() if (f.panel or {}).get("status") == "adopted"]
            ops += [{"op": "request_fix", "page": p, "frame_id": fid, "instruction": text, "scope": "frame"} for fid in targets]
    elif item.gate == "sheet":
        ops = [{"op": "reject_sheet", "character_id": item.character_id, "note": text}]
    return ops
