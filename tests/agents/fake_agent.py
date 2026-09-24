"""A scripted agent that drives Genko only through MCP tools, the way Hermes Agent would.

It follows `next`, writes the story from tests/agents/story.py, "generates" images
with tests/agents/images.py into the request inbox, and imports them with
origin.kind = "fixture". A scripted human (a callback) answers the approval
requests with the CLI. Everything runs offline.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Awaitable, Callable

from agents import images, story


class Stop(Exception):
    pass


class FakeAgent:
    def __init__(self, client, root: Path, project: str, human: Callable[[list[dict]], None] | None = None, *,
                 pages: int = 8, broken_page: int | None = None, give_up_after: int = 3) -> None:
        self.client = client
        self.root = Path(root)
        self.project = project
        self.human = human
        self.pages = pages
        self.broken_page = broken_page
        self.give_up_after = give_up_after
        self.failures: dict[str, int] = {}
        self.asked: set[str] = set()
        self.log: list[str] = []
        self.seen_ids: list[str] = []

    async def tool(self, tool_name: str, **args) -> dict:
        result = await self.client.call_tool(tool_name, {"project": self.project, **args} if tool_name != "create_project" else args)
        data = json.loads(result.content[0].text)
        data["_images"] = [c for c in result.content[1:] if getattr(c, "type", "") == "image"]
        return data

    async def run(self, max_steps: int = 2000, stop_after: int | None = None) -> int:
        steps = 0
        idle = 0
        while steps < max_steps:
            if stop_after is not None and steps >= stop_after:
                return steps
            nxt = await self.tool("next", limit=1, claim=True)
            if nxt["items"]:
                idle = 0
                item = nxt["items"][0]
                self.seen_ids.append(item["id"])
                self.log.append(f"{item['kind']} {item['target']}")
                await self.handle(item)
                steps += 1
                continue
            waiting = [w for w in nxt["waiting_for"] if self._ask_key(w) not in self.asked]
            for gate in waiting:
                await self.ask(gate)
            if nxt["waiting_for"] and self.human is not None:
                self.human(nxt["waiting_for"])
                idle += 1
                if idle > 5:
                    return steps  # the human stopped answering
                continue
            return steps
        raise Stop(f"no end after {max_steps} steps: {self.log[-5:]}")

    # --- approvals -------------------------------------------------------------

    @staticmethod
    def _ask_key(gate: dict) -> str:
        return json.dumps(gate, sort_keys=True)

    async def ask(self, gate: dict) -> None:
        self.asked.add(self._ask_key(gate))
        if gate["gate"] in ("name", "art"):
            await self.tool("request_approval", gate=gate["gate"], pages=gate["pages"], note="確認お願いします")
        elif gate["gate"] == "sheet":
            for cid in gate["characters"]:
                await self.tool("request_approval", gate="sheet", character_id=cid, note="設定画です")
        elif gate["gate"] == "export":
            report = await self.tool("preflight")
            proof = await self.tool("export_proof", format="pdf")
            assert proof["ok"] and Path(proof["files"][0]).is_file()
            note = "preflight: " + ", ".join(sorted({e["code"] for e in report["errors"]})) if report["errors"] else "preflight OK"
            await self.tool("request_approval", gate="export", note=note)

    # --- work items ------------------------------------------------------------

    async def handle(self, item: dict) -> None:
        kind, target = item["kind"], item["target"]
        page, frame_id = target.get("page"), target.get("frame_id")
        if kind == "write_bible":
            assert (await self.tool("set_bible", bible=story.bible(), commit=True))["committed"]
        elif kind == "write_script":
            assert (await self.tool("set_script", script=story.script(self.pages), commit=True))["committed"]
        elif kind in ("plan_page", "revise_page"):
            await self.plan_page(page, replace=kind == "revise_page")
        elif kind == "review_name":
            shown = await self.tool("render", page=page, mode="name", max_px=400)
            assert shown["_images"]
            await self.tool("record_review", page=page, notes="読み順よし", score=0.8)
        elif kind == "make_sheet":
            await self.make_sheet(target["character_id"])
        elif kind in ("gen_panel", "fix_panel"):
            await self.generate(page, frame_id, fix=item.get("comments"))
        elif kind == "import_pending":
            await self.import_pending(page, frame_id, item["request_id"])
        elif kind == "review_candidates":
            await self.choose(page, frame_id)
        elif kind == "report_regions":
            await self.report(page, frame_id)
        elif kind == "upscale_panel":
            panel = (await self.tool("inspect", target="panel", page=page, frame_id=frame_id))["panels"][0]["panel"]
            adopted = panel["adopted"]["art"]
            await self.tool("apply_ops", commit=True, ops=[{"op": "record_review", "page": page, "frame_id": frame_id,
                                                             "kind": "upscale", "input_hash": adopted,
                                                             "notes": "高解像度化の道具が無い。Genko の再標本化で出す"}])
        elif kind == "finish_page":
            done = await self.tool("finish_page", page=page, commit=True)
            assert done["committed"], done
        else:
            raise Stop(f"unknown work item {kind}")

    async def plan_page(self, page: int, replace: bool) -> None:
        key = f"plan:{page}"
        plan = story.broken(page) if page == self.broken_page else story.plan(page)
        result = await self.tool("submit_name", plan=plan, commit=True, replace=replace)
        if result["ok"]:
            return
        self.failures[key] = self.failures.get(key, 0) + 1
        if self.failures[key] >= self.give_up_after:
            codes = sorted({i["code"] for i in result["issues"]})
            await self.tool("ask_human", page=page, item="plan_page", text=f"{page} ページのネームが通らない（{', '.join(codes)}）")

    async def make_sheet(self, character_id: str) -> None:
        req = await self.tool("generation_request", character_id=character_id, purpose="character_sheet")
        assert req["ok"], req
        request = req["request"]
        path = Path(req["inbox"]) / "sheet.png"
        path.write_bytes(images.sheet(request["size"]["suggested_px"]))
        imported = await self.tool("import_images", request_id=request["id"], images=[
            {"file": str(path.relative_to(self.root / self.project)), "origin": {"kind": "fixture", "tool_id": "fixture", "model": "fixture-sheet"}}])
        assert imported["ok"], imported

    async def generate(self, page: int, frame_id: str, fix=None) -> None:
        req = await self.tool("generation_request", page=page, frame_id=frame_id,
                              instruction="; ".join(fix) if fix else None)
        assert req["ok"], req
        request = req["request"]
        assert request["prompt"]["ja"] and request["files"]["composition"]
        await self.import_pending(page, frame_id, request["id"], request)

    async def import_pending(self, page: int, frame_id: str, request_id: str, request: dict | None = None) -> None:
        if request is None:
            request = json.loads((self.root / self.project / "studio" / "requests" / request_id / "request.json").read_text(encoding="utf-8"))
        inbox = self.root / self.project / "studio" / "inbox" / request_id
        inbox.mkdir(parents=True, exist_ok=True)
        px = request["size"]["suggested_px"]
        good = inbox / "good.png"
        bad = inbox / "decoy.png"
        good.write_bytes(images.panel(px, request["figures"]))
        bad.write_bytes(images.decoy(px))
        rel = lambda p: str(p.relative_to(self.root / self.project))  # noqa: E731
        imported = await self.tool("import_images", request_id=request_id, images=[
            {"file": rel(good), "origin": {"kind": "fixture", "tool_id": "fixture", "model": "fixture-good", "prompt": request["prompt"]["en"]}},
            {"file": rel(bad), "origin": {"kind": "fixture", "tool_id": "fixture", "model": "fixture-decoy"}},
        ])
        assert imported["ok"], imported

    async def choose(self, page: int, frame_id: str) -> None:
        listed = await self.tool("candidates", page=page, frame_id=frame_id)
        rows = listed["candidates"]
        good = [r for r in rows if r["origin"].get("model") == "fixture-good" and not r["stale"]]
        reviews = [{"candidate_id": r["id"], "score": 0.9 if r in good else 0.1,
                    "note": "構図がネームに合う" if r in good else "関係ない画像"} for r in rows]
        await self.tool("review_candidates", page=page, frame_id=frame_id, reviews=reviews)
        compare = await self.tool("render", page=page, frame_id=frame_id, kind="compare", candidate_id=good[-1]["id"], max_px=256)
        assert compare["_images"]
        adopted = await self.tool("adopt", page=page, frame_id=frame_id, candidate_id=good[-1]["id"])
        assert adopted["ok"], adopted

    async def report(self, page: int, frame_id: str) -> None:
        info = (await self.tool("inspect", target="panel", page=page, frame_id=frame_id))["panels"][0]
        regions = [{"kind": "face", "char": f["char"], "rect_mm": f["head_mm"]} for f in info["figures"]]
        regions += [{"kind": "person", "char": f["char"], "rect_mm": f["body_mm"]} for f in info["figures"]]
        if regions:
            assert (await self.tool("report_regions", page=page, frame_id=frame_id, regions=regions))["ok"]
        else:
            await self.tool("apply_ops", commit=True, ops=[{"op": "record_review", "page": page, "frame_id": frame_id, "kind": "regions",
                                                             "input_hash": info["panel"]["adopted"]["art"], "notes": "人物なし"}])


Human = Callable[[list[dict]], None]
Runner = Callable[[], Awaitable[None]]
