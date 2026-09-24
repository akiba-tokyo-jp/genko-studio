"""StudioService: the tools an agent calls (MCP, CLI and HTTP share this).

Writing tools default to a dry run: they check, compile and render, and only
save when commit=True and nothing is an error. Every change goes through
apply_ops into project.json (bible, script, name plans, panel briefs, reviews,
tickets, candidates). Approvals live in HumanService, which the MCP server
never exposes.
"""

from __future__ import annotations

import copy
import io
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from genko.headless import snapshot
from genko.io import load_episode, save_episode
from genko.lock import ProjectLock
from genko.models import Binding, Episode, PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.studio import layout as layout_mod
from genko.studio import lint, state, worklist
from genko.studio.issues import Issue, error, has_errors
from genko.studio.jsonschema_lite import validate
from genko.studio.letter import place_page, placements_to_ops
from genko.studio.schemas import SCHEMAS

RULES_PATH = Path(__file__).with_name("guide") / "manga_rules.md"

# Ops an agent may send through apply_ops. Gates, locks, meta (font paths),
# rasters from local paths and structural page changes are not on the list.
AGENT_OPS = frozenset({
    "split_frame", "merge_frame", "resize_frame", "set_frame",
    "add_line", "edit_line", "delete_line", "move_line", "set_balloon_path",
    "add_stroke", "delete_stroke", "edit_stroke", "simplify_stroke",
    "set_note", "add_mannequin", "pose_mannequin", "add_prim3d", "set_ruler",
    "add_tone", "delete_tone", "add_effect", "stamp_material",
    # studio state (M3); approve / revoke are for people only
    "set_studio", "upsert_character", "upsert_location", "delete_location", "upsert_prop", "delete_prop",
    "set_page_plan", "set_panel", "record_review", "request_approval", "request_fix",
    "add_region", "edit_region", "delete_region", "replace_regions", "bind_ref", "unbind_ref",
    "register_assets", "attach_reference", "open_request", "close_request", "import_candidates",
    "review_candidates", "set_candidate", "adopt_candidate", "unadopt", "set_placement", "place_asset", "set_finish",
})

MAX_IMPORT_BYTES = 64 * 1024 * 1024
BRIEF_FROM_PLAN = ("shot", "angle", "characters", "location_id", "time", "action", "emotion", "fx", "emphasis", "beat_ids")

SPEC_PRESETS = {
    "commercial-b4": PageSpec.b4_comic,
    "a4-mono": PageSpec.a4_mono,
    "webtoon": PageSpec.webtoon,
}


@dataclass
class ToolResult:
    ok: bool
    data: dict = field(default_factory=dict)
    issues: list[Issue] = field(default_factory=list)
    images: list[bytes] = field(default_factory=list)  # small PNG previews for the model to look at
    files: list[str] = field(default_factory=list)  # the same previews as files

    def to_dict(self) -> dict:
        return {
            "ok": self.ok,
            **self.data,
            "issues": [issue.to_dict() for issue in self.issues],
            "files": self.files,
        }


def fail(message: str, code: str = "error", path: str = "/") -> ToolResult:
    return ToolResult(False, {"error": message}, [error(code, path, message)])


class StudioService:
    def __init__(self, root: Path, actor: str = "ai:agent") -> None:
        self.root = Path(root).resolve()
        self.actor = actor

    # --- projects -----------------------------------------------------------

    def project_path(self, name: str, *, must_exist: bool = True) -> Path:
        rel = Path(name)
        if rel.is_absolute() or ".." in rel.parts or not name.strip():
            raise ApplyError(f"project は --root からの相対名で指定する: {name}")
        path = (self.root / rel).resolve()
        if self.root not in path.parents:
            raise ApplyError(f"project が --root の外を指している: {name}")
        if must_exist and not (path / "project.json").is_file():
            raise ApplyError(f"project が見つからない: {name}")
        return path

    def projects(self) -> ToolResult:
        found = []
        for project_json in sorted(self.root.rglob("project.json")):
            try:
                episode = load_episode(project_json.parent)
            except Exception:
                continue
            found.append({"name": project_json.parent.relative_to(self.root).as_posix(), "title": episode.title, "pages": len(episode.pages)})
        return ToolResult(True, {"projects": found})

    def create_project(self, name: str, title: str, pages: int, spec_preset: str = "commercial-b4", binding: str = "right") -> ToolResult:
        path = self.project_path(name, must_exist=False)
        if (path / "project.json").exists():
            return fail(f"すでにある: {name}", "project_exists")
        if spec_preset not in SPEC_PRESETS:
            return fail(f"判型 {spec_preset} はない（{', '.join(SPEC_PRESETS)}）", "unknown_preset", "/spec_preset")
        if not 1 <= pages <= 400:
            return fail("pages は 1〜400", "pages_out_of_range", "/pages")
        episode = new_episode(title, 1, pages, SPEC_PRESETS[spec_preset](), Binding(binding))
        episode.strict_gates = True  # agent projects: printed layers change only after the name is approved
        save_episode(episode, path, actor=self.actor)
        return ToolResult(True, {"project": name, "pages": pages})

    # --- reading --------------------------------------------------------------

    def status(self, project: str) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        pages = []
        for page in episode.pages:
            draft = state.name(episode, page.index)
            review = state.name_review(episode, page.index)
            leaves = page.leaf_frames()
            pages.append({
                "page": page.index,
                "named": draft is not None,
                "reviewed": bool(draft and review and review.get("input_hash") == draft.get("input_hash")),
                "name_ok": page.name_ok,
                "art_ok": page.art_ok,
                "panels": len(leaves),
                "adopted": sum(1 for f in leaves if (f.panel or {}).get("status") in ("adopted", "skip")),
                "lines": len(episode.story_for_page(page.index)),
            })
        items = worklist.next_actions(episode)
        return ToolResult(True, {
            "title": episode.title,
            "revision": episode.revision,
            "bible": state.bible(episode) is not None,
            "script": state.script(episode) is not None,
            "pages": pages,
            "waiting_for": _waiting(items),
        })

    def next(self, project: str, limit: int = 5) -> ToolResult:
        path = self.project_path(project)
        items = worklist.next_actions(load_episode(path))
        runnable = [i for i in items if not i["blocked_by"]]
        return ToolResult(True, {"items": runnable[:limit], "blocked": len(items) - len(runnable), "waiting_for": _waiting(items)})

    def inspect(self, project: str, target: str, page: int | None = None, frame_id: str | None = None) -> ToolResult:
        path = self.project_path(project)
        if target == "schemas":
            return ToolResult(True, {"schemas": SCHEMAS})
        if target == "rules":
            return ToolResult(True, {"rules": RULES_PATH.read_text(encoding="utf-8")})
        episode = load_episode(path)
        if target == "bible":
            return ToolResult(True, {"bible": state.bible(episode)})
        if target == "script":
            return ToolResult(True, {"script": state.script(episode)})
        if target == "studio":
            return ToolResult(True, {"studio": episode.studio})
        if target == "snapshot":
            return ToolResult(True, {"snapshot": snapshot(episode)})
        if target in ("page", "panel"):
            if page is None:
                return fail("page が要る", "page_required", "/page")
            if not any(p.index == page for p in episode.pages):
                return fail(f"{page} ページはない", "no_page", "/page")
            if target == "page":
                return ToolResult(True, self._page_brief(episode, page))
            return ToolResult(True, _panel_brief(episode, page, frame_id))
        return fail(f"target {target} はない（bible / script / page / panel / studio / schemas / rules / snapshot）", "unknown_target", "/target")

    def render(self, project: str, page: int, mode: str = "name", max_px: int = 1024, frame_id: str | None = None) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        draft = state.name(episode, page)
        png = _preview(episode, page, mode, max_px, draft.get("name") if draft else None, frame_id)
        suffix = f"_{frame_id}" if frame_id else ""
        out = path / "studio" / "reviews" / f"p{page:03d}_{mode}{suffix}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(png)
        return ToolResult(True, {"page": page, "mode": mode, "frame_id": frame_id}, images=[png], files=[str(out)])

    # --- images ----------------------------------------------------------------------

    def import_image(self, project: str, path: str | None = None, png_base64: str | None = None, *, confine: bool = True) -> ToolResult:
        """Copy an image into assets/ (content-addressed). Returns its ref and pixel size.

        Nothing refers to it until an op (import_candidates, place_asset, attach_reference)
        does; unreferenced assets are removed by `genko gc` after a day.
        """
        import base64

        from PIL import Image

        from genko.assets import AssetStore
        from genko.ops import _verify_image

        project_path = self.project_path(project)
        if (path is None) == (png_base64 is None):
            return fail("path か png_base64 のどちらか一方を渡す", "image_source", "/")
        if path is not None:
            source = Path(path)
            source = (source if source.is_absolute() else self.root / source).resolve()
            if confine and self.root not in source.parents:
                return fail(f"path が --root の外: {path}", "outside_root", "/path")
            if not source.is_file():
                return fail(f"ファイルがない: {path}", "no_file", "/path")
            if source.stat().st_size > MAX_IMPORT_BYTES:
                return fail("画像が大きすぎる（64MB まで）", "too_large", "/path")
            blob = source.read_bytes()
        else:
            try:
                blob = base64.b64decode(png_base64 or "", validate=True)
            except ValueError:
                return fail("png_base64 が base64 ではない", "bad_base64", "/png_base64")
        _verify_image(blob)
        with Image.open(io.BytesIO(blob)) as img:
            img.load()
            if img.format != "PNG":
                buf = io.BytesIO()
                img.save(buf, format="PNG")
                blob = buf.getvalue()
            size = img.size
            mode = img.mode
        ref = AssetStore(project_path).put_bytes(blob, ".png")
        return ToolResult(True, {"asset": ref, "px": list(size), "mode": mode})

    # --- writing: bible, script, name ----------------------------------------------

    def set_bible(self, project: str, bible: dict, commit: bool = False) -> ToolResult:
        path = self.project_path(project)
        issues = validate(bible, SCHEMAS["bible@1"])
        if not has_errors(issues):
            issues += lint.lint_bible(bible)
        if has_errors(issues) or not commit:
            return ToolResult(not has_errors(issues), {"committed": False, "characters": len(bible.get("characters", []))}, issues)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            characters, more = _keep_approved(episode.bible.characters, bible["characters"])
            if has_errors(more):
                return ToolResult(False, {"committed": False}, issues + more)
            doc = {k: v for k, v in bible.items() if k not in ("plot", "characters", "constraints")}
            ops = [
                {"op": "set_bible", "plot": bible["plot"], "characters": characters, "constraints": bible["constraints"]},
                {"op": "set_studio", "bible_doc": doc},
            ]
            if bible.get("title"):
                ops.append({"op": "set_meta", "title": bible["title"]})
            apply_ops(episode, ops, agent=self.actor)
            save_episode(episode, path, actor=self.actor)
        return ToolResult(True, {"committed": True}, issues)

    def set_script(self, project: str, script: dict, commit: bool = False) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        bible = state.bible(episode)
        if bible is None:
            return fail("先に set_bible で企画書を保存する", "bible_missing")
        issues = validate(script, SCHEMAS["script@1"])
        if not has_errors(issues):
            issues += lint.lint_script(script, bible, len(episode.pages))
        per_page: dict[int, int] = {}
        for scene in script.get("scenes", []) if isinstance(script, dict) else []:
            for beat in scene.get("beats", []):
                if isinstance(beat, dict) and isinstance(beat.get("page"), int):
                    per_page[beat["page"]] = per_page.get(beat["page"], 0) + 1
        data = {"committed": False, "beats_per_page": per_page}
        if has_errors(issues) or not commit:
            return ToolResult(not has_errors(issues), data, issues)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            apply_ops(episode, [{"op": "set_script", "script": script}], agent=self.actor)
            save_episode(episode, path, actor=self.actor)
        data["committed"] = True
        return ToolResult(True, data, issues)

    def submit_name(self, project: str, plan: dict, commit: bool = False, replace: bool = False, max_px: int = 1024) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        bible, script = state.bible(episode), state.script(episode)
        if bible is None or script is None:
            return fail("先に企画書（set_bible）と脚本（set_script）を保存する", "script_missing")
        issues = validate(plan, SCHEMAS["name_plan@1"])
        if has_errors(issues):
            return ToolResult(False, {"committed": False}, issues)
        issues += lint.lint_name_plan(plan, script, bible, len(episode.pages))
        if has_errors(issues):
            return ToolResult(False, {"committed": False}, issues)
        page_index = plan["page"]
        work = copy.deepcopy(episode)
        compiled, more = self._compile_name(work, page_index, plan, bible, script, replace)
        issues += more
        if compiled is None or has_errors(issues):
            return ToolResult(False, {"committed": False}, issues)
        data = {
            "committed": False,
            "page": page_index,
            "reading_order": compiled.reading_order,
            "panels": {slot: [round(v, 1) for v in rect] for slot, rect in compiled.leaf_rects_mm.items()},
        }
        png = _preview(work, page_index, "name", max_px, plan)
        out = path / "studio" / "reviews" / f"p{page_index:03d}_name.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(png)
        result = ToolResult(True, data, issues, [png], [str(out)])
        if not commit:
            return result
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            compiled, more = self._compile_name(episode, page_index, plan, bible, script, replace)
            if compiled is None or has_errors(more):
                return ToolResult(False, {"committed": False}, more)
            ops = [{"op": "set_panel", "page": page_index, "frame_id": compiled.slot_to_frame[panel["slot"]],
                    "set": {"slot": panel["slot"], **{k: panel[k] for k in BRIEF_FROM_PLAN if k in panel}}}
                   for panel in plan["panels"] if panel["slot"] in compiled.slot_to_frame]
            ops.append({"op": "set_page_plan", "page": page_index, "plan": {
                "name": plan,
                "input_hash": worklist.plan_hash(plan),
                "rev": episode.revision,
                "turn_role": plan.get("turn_role"),
                "slot_to_frame": compiled.slot_to_frame,
                "reading_order": compiled.reading_order,
                "by": self.actor,
            }})
            ops += [{"op": "set_ticket", "id": t["id"], "status": "done"} for t in state.page_fixes(episode, page_index)]
            try:
                apply_ops(episode, ops, agent=self.actor)
            except ApplyError as exc:
                return ToolResult(False, {"committed": False}, [error("apply_failed", "/", str(exc))])
            save_episode(episode, path, actor=self.actor)
        data["committed"] = True
        return result

    def _compile_name(self, episode: Episode, page_index: int, plan: dict, bible: dict, script: dict, replace: bool):
        page = next(p for p in episode.pages if p.index == page_index)
        if page.name_ok:
            return None, [error("name_approved", "/page", f"{page_index} ページのネームは承認済み。変えるには人間が承認を取り消す")]
        if not layout_mod.is_blank(episode, page):
            if not replace:
                return None, [error("page_not_blank", "/page", f"{page_index} ページにはすでにコマか台詞がある", "作り直すなら replace:true")]
            layout_mod.clear_page(episode, page, agent=self.actor)
        try:
            compiled = layout_mod.apply_layout(episode, page_index, plan, agent=self.actor)
        except layout_mod.LayoutError as exc:
            return None, [error("layout_failed", exc.path, exc.message)]
        speakers = {bid: info["beat"].get("speaker_id") for bid, info in lint.script_index(script).items()}
        placements, issues = place_page(plan, compiled, bible, speakers)
        issues += lint.lint_name_geometry(plan, compiled, placements)
        if has_errors(issues):
            return None, issues
        ops = placements_to_ops(page_index, placements, compiled)
        ops.append({"op": "set_note", "page": page_index, "note": _brief(plan, bible)})
        try:
            episode.undo_stack.clear()
            apply_ops(episode, ops, agent=self.actor)
        except ApplyError as exc:
            return None, [error("apply_failed", "/", str(exc))]
        return compiled, issues

    def apply_ops(self, project: str, ops: list[dict], commit: bool = False) -> ToolResult:
        path = self.project_path(project)
        if not isinstance(ops, list) or len(ops) > 500:
            return fail("ops は 500 個までの配列", "ops_invalid", "/ops")
        for i, op in enumerate(ops):
            name = op.get("op") if isinstance(op, dict) else None
            if name not in AGENT_OPS:
                return fail(f"op {name} はこの道具では使えない", "op_not_allowed", f"/ops/{i}/op")
        episode = load_episode(path)
        try:
            result = apply_ops(episode, ops, dry_run=True, agent=self.actor)
            if commit:
                with ProjectLock(path, agent=self.actor):
                    episode = load_episode(path)
                    result = apply_ops(episode, ops, agent=self.actor)
                    save_episode(episode, path, actor=self.actor)
        except ApplyError as exc:
            return fail(str(exc), "apply_failed", "/ops")
        return ToolResult(True, {"committed": commit, "applied": result["applied"], "warnings": result.get("warnings", [])})

    # --- reviews and requests --------------------------------------------------------

    def record_review(self, project: str, page: int, score: float | None, notes: str) -> ToolResult:
        path = self.project_path(project)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            draft = state.name(episode, page)
            if draft is None:
                return fail(f"{page} ページのネームがまだない", "name_missing", "/page")
            apply_ops(episode, [{"op": "record_review", "page": page, "kind": "name", "score": score, "notes": notes,
                                 "input_hash": draft["input_hash"]}], agent=self.actor)
            save_episode(episode, path, actor=self.actor)
        return ToolResult(True, {"page": page})

    def request_approval(self, project: str, gate: str, pages: list[int], note: str = "", character_id: str | None = None) -> ToolResult:
        path = self.project_path(project)
        if gate not in ("name", "art", "sheet"):
            return fail("依頼できる承認は name / art / sheet", "gate_not_available", "/gate")
        if gate == "sheet" and not character_id:
            return fail("sheet の承認依頼には character_id が要る", "character_required", "/character_id")
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            before = {t["id"] for t in episode.tickets}
            apply_ops(episode, [{"op": "request_approval", "gate": gate, "pages": pages, "note": note,
                                 "character_id": character_id}], agent=self.actor)
            ticket = next(t for t in episode.tickets if t["id"] not in before)
            save_episode(episode, path, actor=self.actor)
        pages_arg = ",".join(map(str, sorted(set(pages))))
        how = (f"genko studio approve {path} sheet --character {character_id} --candidate <候補id> --as human:<name>" if gate == "sheet"
               else f"genko studio approve {path} {gate} --pages {pages_arg} --as human:<name>")
        return ToolResult(True, {"request": ticket["id"], "how_to_approve": how})

    def tickets(self, project: str, status: str = "open") -> ToolResult:
        path = self.project_path(project)
        items = [t for t in load_episode(path).tickets if status == "all" or t.get("status") == status]
        return ToolResult(True, {"tickets": items})

    # --- helpers ----------------------------------------------------------------

    def _page_brief(self, episode: Episode, page: int) -> dict:
        script = state.script(episode) or {"scenes": []}
        beats = []
        neighbours: dict[int, list[str]] = {page - 1: [], page + 1: []}
        for scene in script.get("scenes", []):
            for beat in scene.get("beats", []):
                entry = {"scene_id": scene.get("id"), **beat}
                if beat.get("page") == page:
                    beats.append(entry)
                elif beat.get("page") in neighbours:
                    neighbours[beat["page"]].append(beat.get("text", ""))
        draft = state.name(episode, page)
        target = next(p for p in episode.pages if p.index == page)
        return {
            "page": page,
            **worklist.page_side(page),
            "beats": beats,
            "previous_page": " / ".join(neighbours[page - 1])[:400],
            "next_page": " / ".join(neighbours[page + 1])[:400],
            "current_plan": draft.get("name") if draft else None,
            "panels": [{"frame_id": f.id, "rect_mm": [round(v, 1) for v in (f.rect.x, f.rect.y, f.rect.width, f.rect.height)],
                        "slot": (f.panel or {}).get("slot"), "status": (f.panel or {}).get("status", "empty")}
                       for f in target.leaf_frames()],
            "fixes": [t.get("text", "") for t in state.page_fixes(episode, page)],
            "templates": {k: v["description"] for k, v in layout_mod.templates().items()},
        }


class HumanService:
    """Operations only a person may do. Not exposed over MCP."""

    def __init__(self, project: Path, actor: str) -> None:
        if not actor.startswith("human:"):
            raise ApplyError("承認できるのは human:<名前> だけ")
        self.path = Path(project)
        self.actor = actor

    def _apply(self, ops: list[dict]) -> Episode:
        with ProjectLock(self.path, agent=self.actor):
            episode = load_episode(self.path)
            apply_ops(episode, ops, agent=self.actor)
            save_episode(episode, self.path, actor=self.actor)
        return episode

    def approve_name(self, pages: list[int]) -> dict:
        self._apply([{"op": "approve", "gate": "name", "page": p} for p in pages])
        return {"ok": True, "approved": sorted(pages)}

    def approve_art(self, pages: list[int]) -> dict:
        self._apply([{"op": "approve", "gate": "art", "page": p} for p in pages])
        return {"ok": True, "approved": sorted(pages)}

    def approve_sheet(self, character_id: str, candidate_id: str) -> dict:
        self._apply([{"op": "approve", "gate": "sheet", "character_id": character_id, "candidate_id": candidate_id}])
        return {"ok": True, "approved": character_id}

    def revoke(self, gate: str, pages: list[int], character_id: str | None = None, reason: str = "") -> dict:
        if gate == "sheet":
            self._apply([{"op": "revoke", "gate": "sheet", "character_id": character_id, "reason": reason}])
        else:
            self._apply([{"op": "revoke", "gate": gate, "page": p, "reason": reason} for p in pages])
        return {"ok": True, "revoked": gate}

    def comment(self, page: int, text: str, frame_id: str | None = None) -> dict:
        episode = self._apply([{"op": "request_fix", "page": page, "frame_id": frame_id, "instruction": text,
                                "scope": "frame" if frame_id else "page"}])
        return {"ok": True, "comment": episode.tickets[-1]["id"]}


def _waiting(items: list[dict]) -> list[dict]:
    out = []
    for gate in ("name", "art"):
        pages = sorted(i["target"]["page"] for i in items if i["blocked_by"] and i.get("gate") == gate)
        if pages:
            out.append({"gate": gate, "pages": pages})
    chars = sorted({i["target"]["character_id"] for i in items if i["blocked_by"] and i.get("gate") == "sheet"})
    if chars:
        out.append({"gate": "sheet", "characters": chars})
    return out


def _keep_approved(current: list[dict], incoming: list[dict]) -> tuple[list[dict], list[Issue]]:
    """A new bible keeps approved sheets (refs, locked) and may not drop a locked character."""
    by_id = {c.get("id"): c for c in current}
    issues: list[Issue] = []
    out = []
    for char in incoming:
        old = by_id.get(char.get("id"))
        merged = dict(char)
        if old:
            for key in ("refs", "locked"):
                if key in old:
                    merged[key] = old[key]
        out.append(merged)
    kept = {c.get("id") for c in incoming}
    for cid, old in by_id.items():
        if old.get("locked") and cid not in kept:
            issues.append(error("locked_character", "/characters", f"{cid} の設定画は承認済み。消すには人間が承認を取り消す"))
    return out, issues


def _panel_brief(episode: Episode, page_index: int, frame_id: str | None) -> dict:
    page = next(p for p in episode.pages if p.index == page_index)
    frames = [page._find(frame_id)] if frame_id else page.leaf_frames()
    chars = {c.get("id"): c for c in episode.bible.characters}
    out = []
    for frame in frames:
        panel = frame.panel or {}
        cast = [c.get("id") for c in panel.get("characters", []) if isinstance(c, dict)]
        out.append({
            "frame_id": frame.id,
            "rect_mm": [round(v, 2) for v in (frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height)],
            "bleed": frame.bleed,
            "panel": panel,
            "character_refs": {cid: chars[cid].get("refs", []) for cid in cast if cid in chars},
            "lines": [line.text for line in episode.story_for_page(page_index) if line.frame_id == frame.id],
        })
    return {"page": page_index, "name_ok": page.name_ok, "art_ok": page.art_ok, "panels": out}


def _brief(plan: dict, bible: dict) -> str:
    names = {c.get("id"): c.get("name", "") for c in bible.get("characters", [])}
    parts = []
    for panel in plan.get("panels", []):
        who = "・".join(names.get(c.get("id"), c.get("id", "")) for c in panel.get("characters", []))
        parts.append(f"{panel.get('slot')}: {panel.get('shot')}/{panel.get('angle')} {who} {panel.get('action', '')}".strip())
    return " | ".join(parts)


def _preview(episode: Episode, page_index: int, mode: str, max_px: int, plan: dict | None, frame_id: str | None = None) -> bytes:
    from genko.render import render_frame, render_page
    from genko.studio.review import annotate

    page = next((p for p in episode.pages if p.index == page_index), None)
    if page is None:
        raise ApplyError(f"{page_index} ページはない")
    if frame_id:
        try:
            frame = page._find(frame_id)
        except (KeyError, IndexError) as exc:
            raise ApplyError(f"{page_index} ページにコマ {frame_id} はない") from exc
        longest = max(frame.rect.width, frame.rect.height)
        dpi = max(36, min(600, int(max(64, max_px) / (longest / 25.4))))
        image = render_frame(page, frame_id, dpi, mode="proof" if mode == "name" else mode, episode=episode)
    else:
        dpi = max(36, min(300, int(max(64, max_px) / (page.spec.height_mm / 25.4))))
        image = render_page(page, dpi, mode=mode, episode=episode)
    if mode == "name" and not frame_id:
        image = annotate(image, page, dpi, plan)
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def result_dict(result: ToolResult) -> dict[str, Any]:
    return result.to_dict()
