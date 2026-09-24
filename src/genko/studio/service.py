"""StudioService: the tools an agent calls (MCP, CLI and HTTP share this).

Writing tools default to a dry run: they check, compile and render, and only
save when commit=True and nothing is an error. Approvals live in HumanService,
which the MCP server never exposes.
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
from genko.studio import lint, worklist
from genko.studio.drafts import Drafts
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
})

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
        drafts = Drafts(path)
        names = self._names(drafts, episode)
        reviews = drafts.reviews()
        pages = []
        for page in episode.pages:
            draft = names.get(page.index)
            review = reviews.get(str(page.index))
            pages.append({
                "page": page.index,
                "named": draft is not None,
                "reviewed": bool(draft and review and review.get("input_hash") == draft.get("input_hash")),
                "name_ok": page.name_ok,
                "panels": len(page.leaf_frames()),
                "lines": len(episode.story_for_page(page.index)),
            })
        items = self._next(path, episode)
        return ToolResult(True, {
            "title": episode.title,
            "bible": drafts.bible() is not None,
            "script": drafts.script() is not None,
            "pages": pages,
            "waiting_for": _waiting(items),
        })

    def next(self, project: str, limit: int = 5) -> ToolResult:
        path = self.project_path(project)
        items = self._next(path, load_episode(path))
        runnable = [i for i in items if not i["blocked_by"]]
        return ToolResult(True, {"items": runnable[:limit], "blocked": len(items) - len(runnable), "waiting_for": _waiting(items)})

    def inspect(self, project: str, target: str, page: int | None = None) -> ToolResult:
        path = self.project_path(project)
        drafts = Drafts(path)
        if target == "bible":
            return ToolResult(True, {"bible": drafts.bible()})
        if target == "script":
            return ToolResult(True, {"script": drafts.script()})
        if target == "schemas":
            return ToolResult(True, {"schemas": SCHEMAS})
        if target == "rules":
            return ToolResult(True, {"rules": RULES_PATH.read_text(encoding="utf-8")})
        episode = load_episode(path)
        if target == "snapshot":
            return ToolResult(True, {"snapshot": snapshot(episode)})
        if target == "page":
            if page is None:
                return fail("page が要る", "page_required", "/page")
            return ToolResult(True, self._page_brief(episode, drafts, page))
        return fail(f"target {target} はない（bible / script / page / schemas / rules / snapshot）", "unknown_target", "/target")

    def render(self, project: str, page: int, mode: str = "name", max_px: int = 1024) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        draft = Drafts(path).name(page)
        png = _preview(episode, page, mode, max_px, draft.get("plan") if draft else None)
        out = path / "studio" / "reviews" / f"p{page:03d}_{mode}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(png)
        return ToolResult(True, {"page": page, "mode": mode}, images=[png], files=[str(out)])

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
            ops = [{"op": "set_bible", "plot": bible["plot"], "characters": bible["characters"], "constraints": bible["constraints"]}]
            if bible.get("title"):
                ops.append({"op": "set_meta", "title": bible["title"]})
            apply_ops(episode, ops, agent=self.actor)
            Drafts(path).set_bible(bible)
            save_episode(episode, path)
        return ToolResult(True, {"committed": True}, issues)

    def set_script(self, project: str, script: dict, commit: bool = False) -> ToolResult:
        path = self.project_path(project)
        drafts = Drafts(path)
        bible = drafts.bible()
        if bible is None:
            return fail("先に set_bible で企画書を保存する", "bible_missing")
        issues = validate(script, SCHEMAS["script@1"])
        episode = load_episode(path)
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
            drafts.set_script(script)
        data["committed"] = True
        return ToolResult(True, data, issues)

    def submit_name(self, project: str, plan: dict, commit: bool = False, replace: bool = False, max_px: int = 1024) -> ToolResult:
        path = self.project_path(project)
        drafts = Drafts(path)
        bible, script = drafts.bible(), drafts.script()
        if bible is None or script is None:
            return fail("先に企画書（set_bible）と脚本（set_script）を保存する", "script_missing")
        issues = validate(plan, SCHEMAS["name_plan@1"])
        if has_errors(issues):
            return ToolResult(False, {"committed": False}, issues)
        episode = load_episode(path)
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
            seq = drafts.next_seq()
            drafts.set_name(page_index, {
                "plan": plan,
                "input_hash": worklist.plan_hash(plan),
                "seq": seq,
                "slot_to_frame": compiled.slot_to_frame,
                "reading_order": compiled.reading_order,
                "by": self.actor,
            })
            items = drafts.requests()
            for item in items:
                if item.get("kind") == "comment" and item.get("page") == page_index and item.get("status") == "open":
                    item["status"] = "resolved"
                    item["resolved_seq"] = seq
            drafts.set_requests(items)
            save_episode(episode, path)
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
                    save_episode(episode, path)
        except ApplyError as exc:
            return fail(str(exc), "apply_failed", "/ops")
        return ToolResult(True, {"committed": commit, "applied": result["applied"]})

    # --- reviews and requests --------------------------------------------------------

    def record_review(self, project: str, page: int, score: float | None, notes: str) -> ToolResult:
        path = self.project_path(project)
        drafts = Drafts(path)
        draft = drafts.name(page)
        if draft is None:
            return fail(f"{page} ページのネームがまだない", "name_missing", "/page")
        with ProjectLock(path, agent=self.actor):
            drafts.set_review(page, {"by": self.actor, "score": score, "notes": notes, "input_hash": draft["input_hash"]})
        return ToolResult(True, {"page": page})

    def request_approval(self, project: str, gate: str, pages: list[int], note: str = "") -> ToolResult:
        path = self.project_path(project)
        if gate != "name":
            return fail("M0 で依頼できる承認は name だけ", "gate_not_available", "/gate")
        drafts = Drafts(path)
        with ProjectLock(path, agent=self.actor):
            items = drafts.requests()
            seq = drafts.next_seq()
            items.append({"id": f"rq{seq}", "kind": "approval", "gate": gate, "pages": sorted(set(pages)), "note": note,
                          "by": self.actor, "status": "open", "seq": seq})
            drafts.set_requests(items)
        return ToolResult(True, {"request": f"rq{seq}", "how_to_approve": f"genko studio approve {path} name --pages {','.join(map(str, sorted(set(pages))))} --as human:<name>"})

    def tickets(self, project: str, status: str = "open") -> ToolResult:
        path = self.project_path(project)
        items = [r for r in Drafts(path).requests() if status == "all" or r.get("status") == status]
        return ToolResult(True, {"tickets": items})

    # --- helpers ----------------------------------------------------------------

    def _names(self, drafts: Drafts, episode: Episode) -> dict[int, dict]:
        return {p.index: d for p in episode.pages if (d := drafts.name(p.index)) is not None}

    def _next(self, path: Path, episode: Episode) -> list[dict]:
        drafts = Drafts(path)
        return worklist.next_actions(episode, drafts.bible(), drafts.script(), self._names(drafts, episode), drafts.reviews(), drafts.requests())

    def _page_brief(self, episode: Episode, drafts: Drafts, page: int) -> dict:
        script = drafts.script() or {"scenes": []}
        beats = []
        neighbours: dict[int, list[str]] = {page - 1: [], page + 1: []}
        for scene in script.get("scenes", []):
            for beat in scene.get("beats", []):
                entry = {"scene_id": scene.get("id"), **beat}
                if beat.get("page") == page:
                    beats.append(entry)
                elif beat.get("page") in neighbours:
                    neighbours[beat["page"]].append(beat.get("text", ""))
        draft = drafts.name(page)
        return {
            "page": page,
            **worklist.page_side(page),
            "beats": beats,
            "previous_page": " / ".join(neighbours[page - 1])[:400],
            "next_page": " / ".join(neighbours[page + 1])[:400],
            "current_plan": draft.get("plan") if draft else None,
            "templates": {k: v["description"] for k, v in layout_mod.templates().items()},
        }


class HumanService:
    """Operations only a person may do. Not exposed over MCP."""

    def __init__(self, project: Path, actor: str) -> None:
        if not actor.startswith("human:"):
            raise ApplyError("承認できるのは human:<名前> だけ")
        self.path = Path(project)
        self.actor = actor

    def approve_name(self, pages: list[int]) -> dict:
        with ProjectLock(self.path, agent=self.actor):
            episode = load_episode(self.path)
            apply_ops(episode, [{"op": "name_ok", "page": p} for p in pages], agent=self.actor)
            save_episode(episode, self.path)
            drafts = Drafts(self.path)
            items = drafts.requests()
            for item in items:
                if item.get("kind") == "approval" and item.get("status") == "open" and set(item.get("pages", [])) <= set(pages):
                    item["status"] = "done"
                    item["closed_by"] = self.actor
            drafts.set_requests(items)
        return {"ok": True, "approved": sorted(pages)}

    def comment(self, page: int, text: str) -> dict:
        drafts = Drafts(self.path)
        with ProjectLock(self.path, agent=self.actor):
            items = drafts.requests()
            seq = drafts.next_seq()
            items.append({"id": f"c{seq}", "kind": "comment", "page": page, "text": text, "by": self.actor, "status": "open", "seq": seq})
            drafts.set_requests(items)
        return {"ok": True, "comment": f"c{seq}"}


def _waiting(items: list[dict]) -> list[dict]:
    pages = sorted(i["target"]["page"] for i in items if i["blocked_by"] and i.get("gate") == "name")
    return [{"gate": "name", "pages": pages}] if pages else []


def _brief(plan: dict, bible: dict) -> str:
    names = {c.get("id"): c.get("name", "") for c in bible.get("characters", [])}
    parts = []
    for panel in plan.get("panels", []):
        who = "・".join(names.get(c.get("id"), c.get("id", "")) for c in panel.get("characters", []))
        parts.append(f"{panel.get('slot')}: {panel.get('shot')}/{panel.get('angle')} {who} {panel.get('action', '')}".strip())
    return " | ".join(parts)


def _preview(episode: Episode, page_index: int, mode: str, max_px: int, plan: dict | None) -> bytes:
    from genko.render import render_page
    from genko.studio.review import annotate

    page = next((p for p in episode.pages if p.index == page_index), None)
    if page is None:
        raise ApplyError(f"{page_index} ページはない")
    dpi = max(36, min(300, int(max(64, max_px) / (page.spec.height_mm / 25.4))))
    image = render_page(page, dpi, mode=mode, episode=episode)
    if mode == "name":
        image = annotate(image, page, dpi, plan)
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def result_dict(result: ToolResult) -> dict[str, Any]:
    return result.to_dict()
