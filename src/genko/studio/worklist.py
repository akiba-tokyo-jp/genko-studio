"""What to do next, computed from the project state alone (no stored "done" marks)."""

from __future__ import annotations

from pathlib import Path

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


DEFAULT_LIMITS = {"images_per_panel": 8, "fix_rounds": 2}


def next_actions(episode: Episode, project: Path | None = None) -> list[dict]:
    """Pure function of the project state (plus the inbox and asset headers when `project` is given)."""
    from genko.studio import state

    if state.bible(episode) is None:
        return [item("write_bible", "企画書（bible）がまだない", ["inspect", "set_bible"])]
    atari_pages = {p.index for p in episode.pages if (p.plan or {}).get("atari")}
    no_script = state.script(episode) is None
    if no_script and not atari_pages:
        return [item("write_script", "脚本がまだない", ["inspect", "set_script"])]
    out: list[dict] = []
    if no_script:
        # a hand-drawn name needs no script for its own pages; the other pages still do
        if any(p.index not in atari_pages and not p.name_ok for p in episode.pages):
            out.append(item("write_script", "アタリの無いページのために脚本が要る", ["inspect", "set_script"]))
    gates = state.open_tickets(episode, "gate")
    requested = {(t.get("gate"), p) for t in gates for p in t.get("pages") or []}
    requested_sheets = {t.get("character_id") for t in gates if t.get("gate") == "sheet"}
    for page in episode.pages:
        n = page.index
        draft = state.name(episode, n)
        fixes = state.page_fixes(episode, n)
        if fixes and not page.name_ok:
            out.append(item("revise_page", "人間から修正の指示がある", ["inspect", "render", "submit_name"], n,
                            comments=[t.get("text", "") for t in fixes], tickets=[t["id"] for t in fixes]))
            continue
        if page.name_ok:
            out.extend(_art_items(episode, page, requested, requested_sheets, project))
            continue
        if (page.plan or {}).get("atari"):
            out.extend(_atari_items(episode, page, requested))
            continue
        if draft is None:
            blank = len(page.leaf_frames()) == 1 and not episode.story_for_page(n)
            if blank and not no_script:
                out.append(item("plan_page", "ネームがまだない", ["inspect", "submit_name", "render"], n, **page_side(n)))
            continue
        review = (draft.get("reviews") or {}).get("name")
        if not review or review.get("input_hash") != draft.get("input_hash"):
            out.append(item("review_name", "今のネームをまだ自己点検していない", ["render", "record_review", "submit_name"], n,
                            input_hash=draft.get("input_hash", "")))
            continue
        blocker = "requested" if ("name", n) in requested else "await_human:name"
        out.append(item("await_human", "人間のネーム承認待ち", ["request_approval"], n, blocked_by=[blocker], gate="name"))
    out.extend(_export_items(episode, requested))
    return _block_by_help(episode, _block_by_pilot(episode, _dedupe(out)))


def _atari_items(episode: Episode, page, requested: set) -> list[dict]:
    """A page drawn by hand: proposals wait for a person; the agent reads the lines and writes briefs."""
    n = page.index
    proposals = [p for p in (episode.studio.get("proposals") or {}).values() if p.get("page_id") == page.id]
    open_kinds = {p["kind"] for p in proposals if p.get("status") == "open"}
    accepted_lines = any(p["kind"] == "lines" and p.get("status") == "accepted" for p in proposals)
    blank = len(page.leaf_frames()) == 1
    lines = episode.story_for_page(n)
    no_lines_noted = bool(((page.plan or {}).get("reviews") or {}).get("atari_lines"))
    out: list[dict] = []
    if open_kinds:
        out.append(item("await_human", "アタリからの提案（コマ割り・台詞）を人間が確かめて確定する", ["render", "review_page"], n,
                        blocked_by=["await_human:proposal"], gate="proposal", input_hash=",".join(sorted(open_kinds))))
    if blank and "layout" not in open_kinds:
        out.append(item("atari_layout", "アタリのコマ割りの提案が無い（見つからなかったか却下された）",
                        ["render", "analyze_name", "ask_human"], n))
    if not lines and not accepted_lines and "lines" not in open_kinds and not no_lines_noted:
        out.append(item("read_atari", "アタリの手書きの台詞を読み、位置と一緒に提案する（台詞が無ければ record_review kind=atari_lines）",
                        ["render", "propose_lines", "record_review"], n))
    if not blank and "layout" not in open_kinds:
        missing = [f.id for f in page.leaf_frames() if not (f.panel or {}).get("shot")]
        if missing:
            out.append(item("brief_panels", "コマ割りは確定した。アタリを見て各コマの指示（shot、人物、動き）を書く",
                            ["inspect", "render", "apply_ops"], n, frames=missing, input_hash=",".join(missing)))
            return out
        if not open_kinds and (lines or accepted_lines or no_lines_noted):
            blocker = "requested" if ("name", n) in requested else "await_human:name"
            out.append(item("await_human", "人間のネーム承認待ち", ["request_approval"], n, blocked_by=[blocker], gate="name"))
    return out


def _limits(episode: Episode) -> dict:
    return {**DEFAULT_LIMITS, **((episode.studio.get("policy") or {}).get("limits") or {})}


def _open_request(episode: Episode, frame_id: str) -> dict | None:
    for req in (episode.studio.get("requests") or {}).values():
        if req.get("status") == "open" and (req.get("target") or {}).get("frame_id") == frame_id:
            return req
    return None


def _inbox_files(project: Path | None, request_id: str) -> int:
    if project is None:
        return 0
    folder = Path(project) / "studio" / "inbox" / request_id
    return sum(1 for p in folder.iterdir() if p.is_file() and not p.name.startswith(".")) if folder.is_dir() else 0


def _regions_done(panel: dict) -> bool:
    cast = [c for c in panel.get("characters", []) if isinstance(c, dict)]
    if not cast:
        return True
    if any(r.get("kind") in ("face", "head", "person", "body") and r.get("source") in ("agent", "user")
           for r in panel.get("regions", [])):
        return True
    review = (panel.get("reviews") or {}).get("regions")
    return bool(review and review.get("input_hash") == (panel.get("adopted") or {}).get("art"))


def _art_items(episode: Episode, page, requested: set, requested_sheets: set, project: Path | None) -> list[dict]:
    """Per-panel art work on a page whose name is approved, then finishing."""
    if page.art_ok:
        return _finish_items(episode, page, project)
    chars = {c.get("id"): c for c in episode.bible.characters}
    limits = _limits(episode)
    out: list[dict] = []
    waiting = 0
    for frame in page.leaf_frames():
        panel = frame.panel or {}
        status = panel.get("status", "empty")
        target = {"frame_id": frame.id}
        if status == "skip":
            continue
        if status == "adopted":
            if not _regions_done(panel):
                waiting += 1
                out.append(item("report_regions", "採用した絵の顔と人物の位置をまだ報告していない（写植の顔よけに使う）",
                                ["render", "report_regions"], page.index, **target))
            continue
        waiting += 1
        cast = [c.get("id") for c in panel.get("characters", []) if isinstance(c, dict)]
        unlocked = [cid for cid in cast if cid in chars and not chars[cid].get("locked")]
        if unlocked:
            for cid in unlocked:
                if cid in requested_sheets:
                    out.append(item("await_human", "キャラクター設定画の承認待ち", ["request_approval"], None,
                                    blocked_by=["requested"], gate="sheet", character_id=cid))
                elif any(c.get("status") != "rejected" for c in (episode.studio.get("character_candidates") or {}).get(cid, [])):
                    out.append(item("await_human", "設定画の候補がある。人間に選んで承認してもらう", ["candidates", "request_approval"], None,
                                    blocked_by=["await_human:sheet"], gate="sheet", character_id=cid))
                else:
                    notes = [t.get("text", "") for t in episode.tickets if t.get("kind") == "fix" and t.get("status") == "open"
                             and t.get("character_id") == cid]
                    out.append(item("make_sheet", "作画の前にキャラクター設定画を作り、承認してもらう",
                                    ["generation_request", "import_images", "candidates", "request_approval"], None,
                                    character_id=cid, **({"comments": notes} if notes else {})))
            continue
        attempts = panel.get("attempts") or {}
        over = attempts.get("images", 0) >= limits["images_per_panel"] or attempts.get("fix_rounds", 0) > limits["fix_rounds"]
        request = _open_request(episode, frame.id)
        if request is not None:
            files = _inbox_files(project, request["id"])
            why = (f"inbox に取り込んでいない画像が {files} 枚ある" if files else "依頼パックの画像を生成して inbox に置き、取り込む")
            out.append(item("import_pending", why, ["import_images"], page.index, request_id=request["id"],
                            input_hash=request["id"], **target))
        elif status == "candidates":
            out.append(item("review_candidates", "候補を比べて評価し、採用するか直す",
                            ["candidates", "render", "review_candidates", "adopt", "generation_request"], page.index, **target))
        elif over:
            out.append(item("gen_panel", "このコマの生成回数が上限に達した。人間の判断を待つ", ["ask_human"], page.index,
                            blocked_by=["limit"], attempts=attempts, **target))
        elif status == "fix_requested":
            fixes = [t.get("text", "") for t in episode.tickets
                     if t.get("status") == "open" and t.get("kind") == "fix" and t.get("frame_id") == frame.id]
            out.append(item("fix_panel", "人間からコマの修正指示がある",
                            ["inspect", "generation_request", "import_images", "adopt"], page.index, comments=fixes, **target))
        else:
            out.append(item("gen_panel", "このコマの絵がまだない（依頼パックを作り、外部で生成して取り込む）",
                            ["inspect", "generation_request", "import_images", "candidates", "adopt"], page.index, **target))
    if not waiting:
        blocker = "requested" if ("art", page.index) in requested else "await_human:art"
        out.append(item("await_human", "人間の作画承認待ち", ["render", "request_approval"], page.index, blocked_by=[blocker], gate="art"))
    return out


def _finish_items(episode: Episode, page, project: Path | None) -> list[dict]:
    out: list[dict] = []
    if project is not None:
        from genko.assets import AssetStore
        from genko.studio.preflight import MIN_DPI, art_layers, layer_dpi

        store = AssetStore(project)
        for layer in art_layers(page):
            dpi = layer_dpi(episode, page, layer, store)
            if dpi is None or dpi >= MIN_DPI or not layer.frame_id:
                continue
            try:
                panel = page._find(layer.frame_id).panel or {}
            except (KeyError, IndexError):
                continue
            adopted = (panel.get("adopted") or {}).get("art")
            review = (panel.get("reviews") or {}).get("upscale")
            if review and review.get("input_hash") == adopted:
                continue
            out.append(item("upscale_panel", f"採用した絵の実効解像度が {dpi:.0f} dpi（{MIN_DPI} 未満）",
                            ["generation_request", "import_images", "adopt", "derive", "record_review"], page.index,
                            frame_id=layer.frame_id, dpi=dpi, input_hash=str(adopted)))
    if page.stage != "finish":
        out.append(item("finish_page", "作画は承認済み。仕上げ（台詞の顔よけ、効果、finish へ）", ["finish_page", "render"], page.index))
    return out


def _export_items(episode: Episode, requested: set) -> list[dict]:
    if not episode.pages or not all(p.art_ok and p.stage == "finish" for p in episode.pages):
        return []
    approvals = episode.studio.get("approvals", [])
    done = False
    for record in approvals:
        if record.get("gate") == "export":
            done = not record.get("revoked")
    if done:
        return []
    asked = any(g == "export" for g, _ in requested) or any(
        t.get("gate") == "export" and t.get("status") == "open" and t.get("kind") == "gate" for t in episode.tickets)
    return [item("await_human", "全ページの仕上げが済んだ。preflight を確かめ、人間に書き出しを頼む",
                 ["preflight", "export_proof", "request_approval"], None,
                 blocked_by=["requested" if asked else "await_human:export"], gate="export")]


PANEL_ART_KINDS = frozenset({"gen_panel", "import_pending", "review_candidates", "fix_panel", "report_regions"})


def pilot_page(episode: Episode):
    """The page drawn first (policy.pilot_page, default the first page); None when turned off."""
    policy = episode.studio.get("policy") or {}
    if policy.get("pilot") is False or not episode.pages:
        return None
    wanted = policy.get("pilot_page")
    return next((p for p in episode.pages if p.index == wanted), episode.pages[0])


def _block_by_pilot(episode: Episode, items: list[dict]) -> list[dict]:
    """Until the pilot page's art is approved (and the style fixed), other pages wait for their art."""
    pilot = pilot_page(episode)
    if pilot is None or pilot.art_ok:
        return items
    for entry in items:
        if entry["kind"] in PANEL_ART_KINDS and entry["target"].get("page") != pilot.index:
            entry["blocked_by"].append(f"pilot:{pilot.index}")
    return items


def _block_by_help(episode: Episode, items: list[dict]) -> list[dict]:
    """Open help tickets (ask_human) park the matching items until a person closes them."""
    tickets = [t for t in episode.tickets if t.get("kind") == "help" and t.get("status") == "open"]
    if not tickets:
        return items
    for entry in items:
        target = entry["target"]
        for ticket in tickets:
            same_page = ticket.get("page_index") is not None and ticket.get("page_index") == target.get("page")
            same_frame = not ticket.get("frame_id") or ticket.get("frame_id") == target.get("frame_id")
            same_kind = ticket.get("item") and ticket.get("item") == entry["kind"] and ticket.get("page_index") is None
            if (same_page and same_frame) or same_kind:
                blocker = f"ticket:{ticket['id']}"
                if blocker not in entry["blocked_by"]:
                    entry["blocked_by"].append(blocker)
    return items


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
