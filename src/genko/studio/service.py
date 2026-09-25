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
    "split_frame", "merge_frame", "resize_frame", "set_frame", "cut_frame", "move_gutter",
    "add_line", "edit_line", "delete_line", "move_line", "set_balloon_path", "reorder_lines",
    "add_stroke", "delete_stroke", "edit_stroke", "simplify_stroke", "erase",
    "add_layer", "set_layer", "reorder_layers",
    "set_note", "add_mannequin", "pose_mannequin", "add_prim3d", "set_ruler",
    "add_tone", "delete_tone", "add_effect", "stamp_material",
    # studio state (M3); approve / revoke are for people only
    "set_studio", "upsert_character", "upsert_location", "delete_location", "upsert_prop", "delete_prop",
    "set_page_plan", "set_panel", "record_review", "request_approval", "request_fix",
    "add_region", "edit_region", "delete_region", "replace_regions", "bind_ref", "unbind_ref",
    "register_assets", "attach_reference", "open_request", "close_request", "import_candidates",
    "review_candidates", "set_candidate", "adopt_candidate", "unadopt", "set_placement", "place_asset", "set_finish",
    "ask_human", "propose",
})

# Agent tools callable by name from `genko studio call` (the MCP tool set, minus project management).
AGENT_TOOLS = frozenset({
    "status", "next", "inspect", "render", "import_image", "set_bible", "set_script", "submit_name", "apply_ops",
    "record_review", "request_approval", "tickets", "generation_request", "import_images", "candidates",
    "review_candidates", "adopt", "request_fix", "report_regions", "finish_page", "preflight", "export_proof", "ask_human",
    "review_page", "derive", "import_name", "analyze_name", "propose_lines", "proposals",
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
        items = worklist.next_actions(episode, path)
        return ToolResult(True, {
            "title": episode.title,
            "revision": episode.revision,
            "bible": state.bible(episode) is not None,
            "script": state.script(episode) is not None,
            "pages": pages,
            "waiting_for": _waiting(items),
        })

    def next(self, project: str, limit: int = 5, claim: bool = False) -> ToolResult:
        """Runnable work items. claim=True leases the returned items to this actor (§7.4) so a
        parallel agent gets other ones; items another actor holds show as blocked."""
        from genko.studio import claims

        path = self.project_path(project)
        items = worklist.next_actions(load_episode(path), path)
        for entry in items:
            if not entry["blocked_by"]:
                holder = claims.holder(path, entry["id"])
                if holder and holder != self.actor:
                    entry["blocked_by"].append(f"claimed:{holder}")
        runnable = [i for i in items if not i["blocked_by"]][:limit]
        if claim:
            runnable = [i for i in runnable if claims.claim(path, i["id"], self.actor)]
        blocked = sum(1 for i in items if i["blocked_by"])
        return ToolResult(True, {"items": runnable, "blocked": blocked, "waiting_for": _waiting(items)})

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

    def render(self, project: str, page: int, mode: str = "name", max_px: int = 1024, frame_id: str | None = None,
               kind: str | None = None, candidate_id: str | None = None) -> ToolResult:
        """kind: None (page or panel in `mode`), compare (candidate + the name in red), guide:composition,
        guide:pose, guide:keepout (the request guides for a panel)."""
        path = self.project_path(project)
        episode = load_episode(path)
        if kind == "atari":
            from genko.studio import atari

            target = next((p for p in episode.pages if p.index == page), None)
            if target is None:
                return fail(f"{page} ページはない", "no_page", "/page")
            dpi = max(36, min(200, int(max_px / (target.spec.height_mm / 25.4))))
            png, label = _png(atari.overlay_image(episode, target, dpi)), "atari"
        elif kind:
            png, label = _render_kind(episode, path, page, frame_id, kind, candidate_id, max_px)
        else:
            draft = state.name(episode, page)
            png = _preview(episode, page, mode, max_px, draft.get("name") if draft else None, frame_id)
            label = f"{mode}_{frame_id}" if frame_id else mode
        out = path / "studio" / "reviews" / f"p{page:03d}_{label.replace(':', '-')}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(png)
        return ToolResult(True, {"page": page, "mode": mode, "kind": kind, "frame_id": frame_id}, images=[png], files=[str(out)])

    # --- art: requests, imports, candidates -------------------------------------------

    def generation_request(self, project: str, page: int | None = None, frame_id: str | None = None,
                           character_id: str | None = None, location_id: str | None = None, purpose: str | None = None,
                           mode: str = "new", parent: str | None = None, tool: str | None = None,
                           instruction: str | None = None, regions: list | None = None,
                           focus_character: str | None = None) -> ToolResult:
        from genko.studio import genreq

        path = self.project_path(project)
        if purpose is None:
            purpose = "character_sheet" if character_id else ("location" if location_id else "panel_art")
        episode = load_episode(path)
        if purpose in ("panel_art", "draft"):
            target_page = next((p for p in episode.pages if p.index == page), None)
            if target_page is None:
                return fail(f"{page} ページはない", "no_page", "/page")
            if purpose == "panel_art" and not target_page.name_ok:
                return fail(f"{page} ページのネームが承認されていない（絵の依頼は承認後。ラフは purpose=draft）", "name_not_approved", "/page")
        try:
            pack = genreq.build(episode, path, purpose=purpose, mode=mode, page=page, frame_id=frame_id,
                                character_id=character_id, location_id=location_id, parent=parent, tool=tool,
                                instruction=instruction, regions=regions, focus_character=focus_character)
        except genreq.RequestError as exc:
            return fail(str(exc), "bad_request", exc.path)
        folder = genreq.write(pack, path)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            if pack.id not in (episode.studio.get("requests") or {}):
                apply_ops(episode, [{"op": "open_request", "request": genreq.record(pack)}], agent=self.actor)
                save_episode(episode, path, actor=self.actor)
            elif episode.studio["requests"][pack.id].get("status") != "open":
                # asked again after its images came in: another round of the same request
                apply_ops(episode, [{"op": "open_request", "request": genreq.record(pack)}], agent=self.actor)
                save_episode(episode, path, actor=self.actor)
        files = {name: str(folder / name) for name in sorted(pack.files)}
        preview = _guide_sheet(pack)
        return ToolResult(True, {"request": pack.request, "dir": str(folder), "files": files,
                                 "inbox": str(path / "studio" / "inbox" / pack.id)},
                          images=[preview] if preview else [])

    def import_images(self, project: str, request_id: str, images: list[dict]) -> ToolResult:
        from genko.studio import genreq, importer

        path = self.project_path(project)
        request = genreq.read(path, str(request_id))
        if request is None:
            return fail(f"依頼 {request_id} はない（generation_request で作る）", "no_request", "/request_id")
        try:
            items, first = importer.prepare(path, request, images)
        except importer.ImportError_ as exc:
            return fail(str(exc), "import_failed", exc.path)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            if request_id not in (episode.studio.get("requests") or {}):
                apply_ops(episode, [{"op": "open_request", "request": _record_from_request(request)}], agent=self.actor)
            apply_ops(episode, [{"op": "import_candidates", "request_id": request_id, "candidates": items}], agent=self.actor)
            save_episode(episode, path, actor=self.actor)
        ids = [i["id"] for i in items]
        preview: list[bytes] = []
        target = request.get("target") or {}
        if first is not None and target.get("frame_id"):
            try:
                png, _ = _render_kind(episode, path, int(target["page"]), target["frame_id"], "compare", ids[0], 512)
                preview.append(png)
            except ApplyError:
                pass
        return ToolResult(True, {"request_id": request_id, "candidates": ids,
                                 "metrics": {i["id"]: i["metrics"] for i in items}}, images=preview)

    def candidates(self, project: str, page: int | None = None, frame_id: str | None = None,
                   character_id: str | None = None, location_id: str | None = None) -> ToolResult:
        path = self.project_path(project)
        episode = load_episode(path)
        if character_id or location_id:
            bucket = "character_candidates" if character_id else "location_candidates"
            items = (episode.studio.get(bucket) or {}).get(character_id or location_id, [])
            adopted = {}
        else:
            target = next((p for p in episode.pages if p.index == page), None)
            if target is None:
                return fail(f"{page} ページはない", "no_page", "/page")
            try:
                panel = target._find(str(frame_id)).panel or {}
            except (KeyError, IndexError):
                return fail(f"コマ {frame_id} はない", "no_frame", "/frame_id")
            items = panel.get("candidates", [])
            adopted = panel.get("adopted") or {}
        rows = []
        for cand in items:
            origin = cand.get("origin") or {}
            rows.append({
                "id": cand["id"], "status": cand.get("status"), "adopted_as": [k for k, v in adopted.items() if v == cand["id"]],
                "px": cand.get("px"), "mode": cand.get("mode"), "parent": cand.get("parent"), "request": cand.get("request"),
                "stale": cand.get("stale", False), "review": cand.get("review"), "metrics": cand.get("metrics"),
                "origin": {k: origin.get(k) for k in ("kind", "tool_id", "model") if origin.get(k)},
            })
        rows.sort(key=lambda r: ((r["metrics"] or {}).get("rank", 0), r["id"]))
        sheet = _contact_sheet(path, items)
        return ToolResult(True, {"candidates": rows}, images=[sheet] if sheet else [])

    def review_candidates(self, project: str, page: int, frame_id: str, reviews: list[dict]) -> ToolResult:
        return self._ops(project, [{"op": "review_candidates", "page": page, "frame_id": frame_id, "reviews": reviews}])

    def adopt(self, project: str, candidate_id: str, page: int | None = None, frame_id: str | None = None,
              to: str = "art", fit: str | None = None, offset_mm: list | None = None, scale: float | None = None,
              location_id: str | None = None) -> ToolResult:
        path = self.project_path(project)
        if location_id:
            episode = load_episode(path)
            cand = next((c for c in (episode.studio.get("location_candidates") or {}).get(location_id, []) if c["id"] == candidate_id), None)
            if cand is None:
                return fail(f"場所 {location_id} の候補 {candidate_id} はない", "no_candidate", "/candidate_id")
            return self._ops(project, [{"op": "attach_reference", "target": {"location_id": location_id}, "asset": cand["asset"], "kind": "reference"}])
        op = {"op": "adopt_candidate", "page": page, "frame_id": frame_id, "candidate_id": candidate_id, "to": to}
        for key, value in (("fit", fit), ("offset_mm", offset_mm), ("scale", scale)):
            if value is not None:
                op[key] = value
        result = self._ops(project, [op])
        if result.ok and page is not None and frame_id:
            shown = self.render(project, page, "print" if to != "draft" else "proof", 512, frame_id)
            result.images, result.files = shown.images, shown.files
        return result

    def request_fix(self, project: str, page: int, instruction: str, frame_id: str | None = None,
                    candidate_id: str | None = None, scope: str = "frame") -> ToolResult:
        return self._ops(project, [{"op": "request_fix", "page": page, "frame_id": frame_id, "candidate_id": candidate_id,
                                    "instruction": instruction, "scope": scope}])

    def report_regions(self, project: str, page: int, frame_id: str, regions: list[dict]) -> ToolResult:
        """Faces and people in the adopted art. rect_mm on the page, or box01 in the adopted image (0..1)."""
        path = self.project_path(project)
        episode = load_episode(path)
        target = next((p for p in episode.pages if p.index == page), None)
        if target is None:
            return fail(f"{page} ページはない", "no_page", "/page")
        from genko.models import LayerKind

        layer = next((lay for lay in target.layers if lay.kind == LayerKind.PLACED and lay.frame_id == frame_id
                      and (lay.source or {}).get("to", "art") == "art"), None)
        converted = []
        for i, region in enumerate(regions or []):
            if not isinstance(region, dict) or not region.get("kind"):
                return fail("region は {kind, rect_mm | box01, char?}", "bad_region", f"/regions/{i}")
            item = {k: region[k] for k in ("kind", "char", "confidence") if region.get(k) is not None}
            if region.get("rect_mm"):
                item["rect_mm"] = [round(float(v), 2) for v in region["rect_mm"]]
            elif region.get("box01"):
                if layer is None or layer.placement_mm is None:
                    return fail("box01 は採用した絵に対する座標。先に adopt する", "no_art", f"/regions/{i}")
                b, pl = [float(v) for v in region["box01"]], layer.placement_mm
                item["rect_mm"] = [round(pl.x + b[0] * pl.width, 2), round(pl.y + b[1] * pl.height, 2),
                                   round(b[2] * pl.width, 2), round(b[3] * pl.height, 2)]
            else:
                return fail("rect_mm か box01 が要る", "bad_region", f"/regions/{i}")
            converted.append(item)
        result = self._ops(project, [{"op": "replace_regions", "page": page, "frame_id": frame_id, "source": "agent", "regions": converted}])
        if result.ok:
            from genko.studio import finish

            episode = load_episode(path)
            ops, notes = finish.plan(episode, next(p for p in episode.pages if p.index == page))
            result.data["finish_preview"] = [n for n in notes if n.get("frame_id") == frame_id]
        return result

    def finish_page(self, project: str, page: int, commit: bool = False) -> ToolResult:
        from genko.studio import finish

        path = self.project_path(project)
        episode = load_episode(path)
        target = next((p for p in episode.pages if p.index == page), None)
        if target is None:
            return fail(f"{page} ページはない", "no_page", "/page")
        if not target.art_ok:
            return fail(f"{page} ページの作画が承認されていない", "art_not_approved", "/page")
        ops, notes = finish.plan(episode, target)
        work = copy.deepcopy(episode)
        try:
            apply_ops(work, ops, agent=self.actor)
        except ApplyError as exc:
            return fail(str(exc), "apply_failed", "/")
        png = _preview(work, page, "proof", 1024, None)
        data = {"committed": False, "page": page, "changes": notes, "ops": len(ops)}
        if commit:
            with ProjectLock(path, agent=self.actor):
                episode = load_episode(path)
                ops, notes = finish.plan(episode, next(p for p in episode.pages if p.index == page))
                apply_ops(episode, ops, agent=self.actor)
                save_episode(episode, path, actor=self.actor)
            data.update(committed=True, changes=notes)
        return ToolResult(True, data, images=[png])

    def preflight(self, project: str, allow_fixture: bool = False, force: bool = False) -> ToolResult:
        from genko.studio import preflight

        path = self.project_path(project)
        report = preflight.check(load_episode(path), path, allow_fixture=allow_fixture, force=force)
        return ToolResult(True, {"ready": report["ok"], **{k: v for k, v in report.items() if k != "ok"}})

    def export_proof(self, project: str, format: str = "pdf") -> ToolResult:  # noqa: A002
        from genko.studio import preflight

        path = self.project_path(project)
        episode = load_episode(path)
        if format not in ("pdf", "png"):
            return fail("format は pdf か png", "bad_format", "/format")
        written = preflight.export_proof(episode, path, format)
        return ToolResult(True, {"files": [str(p) for p in written], "dpi": preflight.PROOF_DPI, "watermark": True},
                          files=[str(p) for p in written])

    def derive(self, project: str, page: int, frame_id: str, kind: str = "lineart", candidate_id: str | None = None,
               params: dict | None = None) -> ToolResult:
        """Genko's own deterministic image steps, added as candidates (origin kind "genko", mode "derive").
        kind lineart: the lines of a candidate (default the adopted art) as black-on-transparent ink;
        adopt it with to: "ink" to print crisp lines over the toned art."""
        from genko import lineart
        from genko.assets import AssetStore
        from genko.studio.jsonutil import content_hash

        if kind != "lineart":
            return fail("kind は lineart だけ", "bad_kind", "/kind")
        path = self.project_path(project)
        episode = load_episode(path)
        target = next((p for p in episode.pages if p.index == page), None)
        if target is None:
            return fail(f"{page} ページはない", "no_page", "/page")
        try:
            panel = target._find(str(frame_id)).panel or {}
        except (KeyError, IndexError):
            return fail(f"コマ {frame_id} はない", "no_frame", "/frame_id")
        source_id = candidate_id or (panel.get("adopted") or {}).get("art")
        source = next((c for c in panel.get("candidates", []) if c.get("id") == source_id), None)
        if source is None:
            return fail("元の候補が無い（candidate_id を渡すか、先に採用する）", "no_candidate", "/candidate_id")
        store = AssetStore(path)
        data = store.get_bytes(source["asset"], ".png")
        if data is None:
            return fail(f"候補 {source_id} の画像が assets/ に無い", "asset_missing", "/candidate_id")
        from PIL import Image

        settings = lineart.LineParams.from_dict(params)
        layer = lineart.extract(Image.open(io.BytesIO(data)), settings)
        ref = store.put_bytes(lineart.to_png(layer), ".png")
        cand_id = "cd_" + content_hash({"derive": kind, "from": source["asset"], "params": settings.__dict__})[7:17]
        item = {"id": cand_id, "asset": ref, "px": list(layer.size), "mode": "derive", "parent": source_id,
                "origin": {"kind": "genko", "tool_id": f"genko:{kind}", "params": settings.__dict__}}
        result = self._ops(project, [{"op": "import_candidates", "page": page, "frame_id": frame_id, "candidates": [item]}])
        if not result.ok:
            return result
        preview = Image.new("RGB", layer.size, (255, 255, 255))
        preview.paste(layer, (0, 0), layer)
        preview.thumbnail((512, 512))
        ink_share = sum(layer.split()[3].histogram()[128:]) / max(1, layer.width * layer.height)
        return ToolResult(True, {"candidate": cand_id, "kind": kind, "parent": source_id, "ink_share": round(ink_share, 4),
                                 "adopt": {"candidate_id": cand_id, "to": "ink"}}, images=[_png(preview)])

    # --- hand-drawn names (atari) ---------------------------------------------------------------

    def import_name(self, project: str, files: list[str], start_page: int = 1, align: str = "auto",
                    *, confine: bool = True) -> ToolResult:
        """Scans of hand-drawn names, one per page from `start_page`: stored as assets (origin self),
        placed on DRAFT (never printed), aligned, and analysed into layout proposals."""
        from genko.assets import AssetStore
        from genko.studio import atari, importer

        path = self.project_path(project)
        if align not in atari.ALIGNS:
            return fail(f"align は {', '.join(atari.ALIGNS)}", "bad_align", "/align")
        if not isinstance(files, list) or not files:
            return fail("files は 1 つ以上", "no_files", "/files")
        store = AssetStore(path)
        episode = load_episode(path)
        prepared = []
        for i, name in enumerate(files):
            source = Path(name)
            source = (source if source.is_absolute() else self.root / source).resolve()
            if confine and self.root not in source.parents:
                return fail(f"--root の外: {name}", "outside_root", f"/files/{i}")
            if not source.is_file():
                return fail(f"ファイルがない: {name}", "no_file", f"/files/{i}")
            try:
                blob, size, image = importer.normalize(source.read_bytes(), i)
            except importer.ImportError_ as exc:
                return fail(str(exc), "bad_image", f"/files/{i}")
            index = start_page + i
            page = next((p for p in episode.pages if p.index == index), None)
            if page is None:
                return fail(f"{index} ページが無い（ページを先に増やす）", "no_page", f"/files/{i}")
            placement, used = atari.placement_for(image.convert("L"), page, align)
            prepared.append((index, store.put_bytes(blob, ".png"), list(size), placement, used, source.name))
        imported = []
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            ops: list[dict] = []
            for index, ref, size, placement, used, filename in prepared:
                ops += [
                    {"op": "register_assets", "assets": {ref: {"kind": "atari", "origin": "self", "file": filename, "by": self.actor}}},
                    {"op": "place_asset", "page": index, "asset": ref, "to": "draft", "placement_mm": placement, "title": "アタリ"},
                    {"op": "set_page_plan", "page": index, "plan": {"atari": {"asset": ref, "px": size, "placement_mm": placement,
                                                                              "align": used, "file": filename, "by": self.actor}}},
                ]
                imported.append({"page": index, "asset": ref, "align": used, "placement_mm": placement})
            apply_ops(episode, ops, agent=self.actor)
            proposals = []
            for item in imported:
                page = next(p for p in episode.pages if p.index == item["page"])
                proposal = atari.analyze(path, episode, page)
                apply_ops(episode, [{"op": "propose", "proposal": proposal}], agent=self.actor)
                proposals.append({"page": item["page"], "proposal": proposal["id"], "panels": len(proposal["panels"]),
                                  "confidence": proposal["confidence"]})
            save_episode(episode, path, actor=self.actor)
        return ToolResult(True, {"imported": imported, "proposals": proposals})

    def analyze_name(self, project: str, page: int, params: dict | None = None) -> ToolResult:
        """Run the panel detection again (optionally with other parameters): a new layout proposal."""
        from genko.studio import atari

        path = self.project_path(project)
        with ProjectLock(path, agent=self.actor):
            episode = load_episode(path)
            target = next((p for p in episode.pages if p.index == page), None)
            if target is None:
                return fail(f"{page} ページはない", "no_page", "/page")
            try:
                proposal = atari.analyze(path, episode, target, params)
            except ValueError as exc:
                return fail(str(exc), "no_atari", "/page")
            apply_ops(episode, [{"op": "propose", "proposal": proposal}], agent=self.actor)
            save_episode(episode, path, actor=self.actor)
        png = _png(atari.overlay_image(load_episode(path), next(p for p in load_episode(path).pages if p.index == page), 72))
        return ToolResult(True, {"proposal": proposal["id"], "panels": proposal["panels"], "tiers": proposal["tiers"],
                                 "confidence": proposal["confidence"], "analysis": proposal["analysis"]}, images=[png])

    def propose_lines(self, project: str, page: int, lines: list[dict]) -> ToolResult:
        """Lines read from the handwriting (text with \n for columns, and where: x_mm/y_mm[/w_mm/h_mm] or box01
        in the scan). A proposal only: a person accepts it. Returns the overlay to check against the scan."""
        from genko.studio import atari
        from genko.studio.jsonutil import content_hash

        path = self.project_path(project)
        episode = load_episode(path)
        target = next((p for p in episode.pages if p.index == page), None)
        if target is None:
            return fail(f"{page} ページはない", "no_page", "/page")
        if not isinstance(lines, list) or not lines:
            return fail("lines は 1 つ以上", "no_lines", "/lines")
        checked = []
        for i, item in enumerate(lines):
            try:
                checked.append(atari.line_from_input(item if isinstance(item, dict) else {}, target, atari.atari_of(target)))
            except (ValueError, TypeError) as exc:
                return fail(str(exc), "bad_line", f"/lines/{i}")
        proposal = {"id": "pr_" + content_hash({"page": target.id, "lines": checked})[7:17], "kind": "lines", "page": page,
                    "source": self.actor, "lines": checked}
        result = self._ops(project, [{"op": "propose", "proposal": proposal}])
        if not result.ok:
            return result
        episode = load_episode(path)
        page_obj = next(p for p in episode.pages if p.index == page)
        png = _png(atari.overlay_image(episode, page_obj, 72))
        return ToolResult(True, {"proposal": proposal["id"], "lines": len(checked)}, images=[png])

    def proposals(self, project: str, status: str = "open") -> ToolResult:
        path = self.project_path(project)
        items = [p for p in (load_episode(path).studio.get("proposals") or {}).values() if status == "all" or p.get("status") == status]
        brief = [{k: p.get(k) for k in ("id", "kind", "page", "status", "source", "by", "confidence")} |
                 {"count": len(p.get("panels") or p.get("lines") or [])} for p in items]
        return ToolResult(True, {"proposals": brief})

    def review_page(self, project: str) -> ToolResult:
        """Write studio/review.html (previews, candidates, open requests, the commands a person runs to approve).
        Send its path to the person (for example through Hermes messaging); the approving is theirs."""
        from genko.studio.review import review_html

        path = self.project_path(project)
        out = path / "studio" / "review.html"
        out.parent.mkdir(parents=True, exist_ok=True)
        from genko.studio.jsonutil import atomic_write_text

        atomic_write_text(out, review_html(path))
        open_items = [{"kind": t.get("kind"), "gate": t.get("gate"), "pages": t.get("pages"), "character_id": t.get("character_id")}
                      for t in state.open_tickets(load_episode(path)) if t.get("kind") in ("gate", "help")]
        return ToolResult(True, {"path": str(out), "open_requests": open_items,
                                 "message": f"確認ページ: {out}（ブラウザで開く。承認はページ内のコマンドで）"})

    def ask_human(self, project: str, text: str, page: int | None = None, frame_id: str | None = None,
                  item: str | None = None) -> ToolResult:
        op: dict[str, Any] = {"op": "ask_human", "text": text, "item": item}
        if page is not None:
            op["page"] = page
        if frame_id:
            op["frame_id"] = frame_id
        return self._ops(project, [op])

    def _ops(self, project: str, ops: list[dict]) -> ToolResult:
        path = self.project_path(project)
        try:
            with ProjectLock(path, agent=self.actor):
                episode = load_episode(path)
                before = {t["id"] for t in episode.tickets}
                result = apply_ops(episode, ops, agent=self.actor)
                save_episode(episode, path, actor=self.actor)
        except ApplyError as exc:
            return fail(str(exc), "apply_failed", "/")
        new = [t["id"] for t in episode.tickets if t["id"] not in before]
        return ToolResult(True, {"applied": result["applied"], **({"tickets": new} if new else {})})

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
            submits = int(((next(p for p in episode.pages if p.index == page_index).plan) or {}).get("submits", 0)) + 1
            ops.append({"op": "set_page_plan", "page": page_index, "plan": {
                "submits": submits,
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
        if gate not in ("name", "art", "sheet", "export"):
            return fail("依頼できる承認は name / art / sheet / export", "gate_not_available", "/gate")
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
        if gate == "sheet":
            how = f"genko studio approve {path} sheet --character {character_id} --candidate <候補id> --as human:<name>"
        elif gate == "export":
            how = f"genko studio export {path} --format pdf --out <出力先> --as human:<name>"
        else:
            how = f"genko studio approve {path} {gate} --pages {pages_arg} --as human:<name>"
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

    def approve_sheet(self, character_id: str, candidate_id: str, face_box01: list[float] | None = None) -> dict:
        """Approve a sheet and cut the face reference out of it (`face_box01` in the image, 0..1;
        default: a square at the top centre, where sheets usually put the face close-up)."""
        op = sheet_approval_op(self.path, load_episode(self.path), character_id, candidate_id, face_box01)
        self._apply([op])
        return {"ok": True, "approved": character_id, "face_asset": op.get("face_asset")}

    def export(self, fmt: str, out: Path, dpi: int | None = None, allow_fixture: bool = False, force: bool = False,
               *, width_px: int = 800, max_height: int = 1280, long_edge: int = 2048, jpeg: bool | None = None,
               spreads: bool = False) -> dict:
        """The final export (gate ④): preflight must pass, then the pages are written and the approval recorded."""
        from genko.export import export_print
        from genko.studio import preflight

        episode = load_episode(self.path)
        if fmt not in ("pdf", "tiff", "png", "webtoon", "sns"):
            return {"ok": False, "error": "format は pdf / tiff / png / webtoon / sns"}
        screen = fmt in ("webtoon", "sns")
        # screen outputs are sized in pixels, so print resolution does not apply to them
        report = preflight.check(episode, self.path, allow_fixture=allow_fixture, force=force or screen)
        if not report["ok"]:
            return {"ok": False, "error": "preflight で止まった", **report}
        if screen:
            from genko import profiles

            if fmt == "webtoon":
                written = profiles.export_webtoon(episode, Path(out), width_px, max_height, fmt="jpeg" if jpeg else "png")
            else:
                written = profiles.export_sns(episode, Path(out), long_edge, fmt="png" if jpeg is False else "jpeg", spreads=spreads)
        else:
            written = export_print(episode, Path(out), fmt=fmt, dpi=int(dpi or episode.spec.dpi or 600))
        self._apply([{"op": "approve", "gate": "export"}])
        return {"ok": True, "files": [str(p) for p in written], "warnings": report["warnings"], "dpi": report["dpi"]}

    def revoke(self, gate: str, pages: list[int], character_id: str | None = None, reason: str = "") -> dict:
        if gate == "sheet":
            self._apply([{"op": "revoke", "gate": "sheet", "character_id": character_id, "reason": reason}])
        else:
            self._apply([{"op": "revoke", "gate": gate, "page": p, "reason": reason} for p in pages])
        return {"ok": True, "revoked": gate}

    def accept_proposal(self, proposal_id: str, force: bool = False) -> dict:
        from genko.studio import atari

        episode = load_episode(self.path)
        proposal = (episode.studio.get("proposals") or {}).get(proposal_id)
        if proposal is None or proposal.get("status") != "open":
            return {"ok": False, "error": f"開いている提案 {proposal_id} は無い"}
        try:
            ops = atari.accept_ops(episode, proposal, force=force)
        except ValueError as exc:
            return {"ok": False, "error": str(exc)}
        page = next(p for p in episode.pages if p.id == proposal["page_id"])
        ops = atari.assign_frames(ops, page)
        try:
            self._apply(ops)
        except ApplyError as exc:
            return {"ok": False, "error": str(exc)}
        return {"ok": True, "accepted": proposal_id, "kind": proposal["kind"]}

    def reject_proposal(self, proposal_id: str, note: str = "") -> dict:
        self._apply([{"op": "resolve_proposal", "id": proposal_id, "status": "rejected", "note": note}])
        return {"ok": True, "rejected": proposal_id}

    def close_ticket(self, ticket_id: str, reply: str = "") -> dict:
        episode = load_episode(self.path)
        ticket = next((t for t in episode.tickets if t.get("id") == ticket_id), None)
        if ticket is None:
            return {"ok": False, "error": f"チケット {ticket_id} はない"}
        ops = [{"op": "set_ticket", "id": ticket_id, "status": "done"}]
        if reply and ticket.get("page_index"):
            ops.append({"op": "request_fix", "page": ticket["page_index"], "frame_id": ticket.get("frame_id"),
                        "instruction": reply, "scope": "frame" if ticket.get("frame_id") else "page"})
        self._apply(ops)
        return {"ok": True, "closed": ticket_id}

    def comment(self, page: int, text: str, frame_id: str | None = None) -> dict:
        episode = self._apply([{"op": "request_fix", "page": page, "frame_id": frame_id, "instruction": text,
                                "scope": "frame" if frame_id else "page"}])
        return {"ok": True, "comment": episode.tickets[-1]["id"]}


def _waiting(items: list[dict]) -> list[dict]:
    out = []
    for gate in ("proposal", "name", "art"):
        pages = sorted({i["target"]["page"] for i in items if i["blocked_by"] and i.get("gate") == gate})
        if pages:
            out.append({"gate": gate, "pages": pages})
    chars = sorted({i["target"]["character_id"] for i in items if i["blocked_by"] and i.get("gate") == "sheet"})
    if chars:
        out.append({"gate": "sheet", "characters": chars})
    if any(i["blocked_by"] and i.get("gate") == "export" for i in items):
        out.append({"gate": "export"})
    return out


def sheet_approval_op(project: Path, episode: Episode, character_id: str, candidate_id: str,
                      face_box01: list[float] | None = None) -> dict:
    """The approve op for a character sheet, with the face cut out of it and stored as an asset."""
    from PIL import Image

    from genko.assets import AssetStore

    cand = next((c for c in (episode.studio.get("character_candidates") or {}).get(character_id, [])
                 if c.get("id") == candidate_id), None)
    op = {"op": "approve", "gate": "sheet", "character_id": character_id, "candidate_id": candidate_id}
    store = AssetStore(project)
    data = store.get_bytes(cand["asset"], ".png") if cand else None
    if data is not None:
        image = Image.open(io.BytesIO(data))
        w, h = image.size
        if face_box01:
            x, y, bw, bh = (float(v) for v in face_box01)
        else:
            side = 0.4 * w
            x, y, bw, bh = 0.3, 0.03, 0.4, min(0.9, side / h)
        box = (round(x * w), round(y * h), round((x + bw) * w), round((y + bh) * h))
        op["face_asset"] = store.put_bytes(_png(image.crop(box)), ".png")
    return op


def _record_from_request(request: dict) -> dict:
    target = request.get("target") or {}
    return {
        "id": request["id"], "purpose": request.get("purpose"), "mode": request.get("mode"),
        "target": {k: target[k] for k in ("page", "page_id", "frame_id", "character_id", "location_id") if k in target},
        "brief_hash": request.get("brief_hash"), "parent": request.get("parent"), "tool": request.get("tool"),
        "pad_mm": (request.get("size") or {}).get("pad_mm", 0.0),
    }


def _png(image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def _guide_sheet(pack) -> bytes | None:
    """The request's guides side by side, long side 512 px: one small image for the model to look at."""
    from PIL import Image

    names = [n for n in ("guides/composition.png", "guides/pose.png", "guides/keepout.png") if n in pack.files]
    if not names:
        return None
    images = [Image.open(io.BytesIO(pack.files[n])).convert("L") for n in names]
    h = min(256, images[0].height)
    thumbs = [img.resize((max(1, round(img.width * h / img.height)), h)) for img in images]
    sheet = Image.new("L", (sum(t.width for t in thumbs) + 8 * (len(thumbs) - 1), h), 200)
    x = 0
    for thumb in thumbs:
        sheet.paste(thumb, (x, 0))
        x += thumb.width + 8
    sheet.thumbnail((512, 512))
    return _png(sheet)


def _contact_sheet(project: Path, items: list[dict]) -> bytes | None:
    from PIL import Image, ImageDraw

    from genko.assets import AssetStore

    store = AssetStore(project)
    thumbs = []
    for cand in items[:8]:
        data = store.get_bytes(cand["asset"], ".png")
        if data is None:
            continue
        img = Image.open(io.BytesIO(data)).convert("RGB")
        img.thumbnail((192, 192))
        tile = Image.new("RGB", (192, 208), (255, 255, 255))
        tile.paste(img, ((192 - img.width) // 2, 16 + (192 - img.height) // 2))
        ImageDraw.Draw(tile).text((2, 2), cand["id"], fill=(0, 0, 0))
        thumbs.append(tile)
    if not thumbs:
        return None
    cols = min(4, len(thumbs))
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 196, rows * 212), (220, 220, 220))
    for i, tile in enumerate(thumbs):
        sheet.paste(tile, ((i % cols) * 196, (i // cols) * 212))
    return _png(sheet)


def _render_kind(episode: Episode, project: Path, page_index: int, frame_id: str | None, kind: str,
                 candidate_id: str | None, max_px: int) -> tuple[bytes, str]:
    from PIL import Image

    from genko import guide
    from genko.assets import AssetStore
    from genko.studio.genreq import PAD_MM

    def exact_px(aspect: float) -> tuple[int, int]:
        # previews keep the exact aspect (the request sizes are rounded to 64 for image tools)
        width = max(16, round((max_px * max_px / 2 * aspect) ** 0.5))
        return width, max(16, round(width / aspect))

    page = next((p for p in episode.pages if p.index == page_index), None)
    if page is None:
        raise ApplyError(f"{page_index} ページはない")
    try:
        frame = page._find(str(frame_id))
    except (KeyError, IndexError) as exc:
        raise ApplyError(f"{page_index} ページにコマ {frame_id} はない") from exc
    panel = frame.panel or {}
    if kind == "compare":
        cand_id = candidate_id or (panel.get("adopted") or {}).get("art")
        cand = next((c for c in panel.get("candidates", []) if c["id"] == cand_id), None)
        if cand is None:
            raise ApplyError(f"候補 {cand_id} はない")
        data = AssetStore(project).get_bytes(cand["asset"], ".png")
        if data is None:
            raise ApplyError(f"候補 {cand_id} の画像が assets/ に無い")
        pad = (cand.get("mapping") or {}).get("pad_mm", 0.0)
        box_aspect = (frame.rect.width + 2 * pad) / (frame.rect.height + 2 * pad)
        box = guide.GenBox.for_frame(frame, pad, exact_px(box_aspect))
        image = guide.compare(Image.open(io.BytesIO(data)), page, frame, box)
        label = f"compare_{frame.id}_{cand_id}"
    elif kind in ("guide:composition", "guide:pose", "guide:keepout"):
        box_aspect = (frame.rect.width + 2 * PAD_MM) / (frame.rect.height + 2 * PAD_MM)
        box = guide.GenBox.for_frame(frame, PAD_MM, exact_px(box_aspect))
        name = kind.split(":", 1)[1]
        image = guide.keepout(episode, page, frame, box) if name == "keepout" else getattr(guide, name)(page, frame, box)
        label = f"{name}_{frame.id}"
    else:
        raise ApplyError("kind は compare / guide:composition / guide:pose / guide:keepout")
    image.thumbnail((max_px, max_px))
    return _png(image), label


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
        from genko.guide import figures_for

        out.append({
            "frame_id": frame.id,
            "rect_mm": [round(v, 2) for v in (frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height)],
            "bleed": frame.bleed,
            "panel": panel,
            "figures": [{"char": f.char_id, "head_mm": [round(v, 2) for v in f.head], "body_mm": [round(v, 2) for v in f.body]}
                        for f in figures_for(frame)],
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
