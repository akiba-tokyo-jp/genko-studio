"""`genko mcp`: expose StudioService to agents such as Hermes Agent.

Tool results are JSON text plus, for previews, a small PNG the model can look
at (the same image is also written to a file whose path is in the JSON). There
are no approval tools: approving is a human action (`genko studio approve`).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from mcp.server.mcpserver import Image, MCPServer

from genko.ops import ApplyError
from genko.studio.service import RULES_PATH, StudioService, ToolResult

INSTRUCTIONS = """Genko は漫画原稿のシステム。文章も絵も作らない。あなた（エージェント）が企画書・脚本・ネーム計画を書き、
Genko が検査・コマ割り・縦書き写植・プレビュー描画をする。
進め方: status → next で次の作業を取る → 書く道具は commit=false で試し、issues の path を直してから commit=true。
ネームの規則は resource genko://guide/manga-rules（inspect target=rules でも読める）。
承認は人間だけが行う。承認が要るところでは request_approval を出して待つ。"""


def _out(result: ToolResult) -> list[Any]:
    content: list[Any] = [json.dumps(result.to_dict(), ensure_ascii=False)]
    content.extend(Image(data=png, format="png") for png in result.images)
    return content


def build_server(root: Path, actor: str) -> MCPServer:
    if not actor.startswith("ai:"):
        raise ApplyError("MCP サーバーの actor は ai:<名前> にする（人間の権限はエージェントに渡さない）")
    service = StudioService(root, actor)
    server = MCPServer("genko", instructions=INSTRUCTIONS)

    def call(fn, *args, **kwargs) -> list[Any]:
        try:
            return _out(fn(*args, **kwargs))
        except ApplyError as exc:
            return [json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False)]

    @server.tool(structured_output=False)
    def projects() -> list:
        """--root 配下のプロジェクト一覧。"""
        return call(service.projects)

    @server.tool(structured_output=False)
    def create_project(name: str, title: str, pages: int, spec_preset: str = "commercial-b4") -> list:
        """新しいプロジェクトを作る。name は --root からの相対名（例 summer.genko）。spec_preset: commercial-b4 / a4-mono / webtoon。"""
        return call(service.create_project, name, title, pages, spec_preset)

    @server.tool(structured_output=False)
    def status(project: str) -> list:
        """工程の様子（企画書・脚本の有無、ページごとのネームと承認、承認待ち）。"""
        return call(service.status, project)

    @server.tool(structured_output=False)
    def next(project: str, limit: int = 5) -> list:
        """次にやる作業（kind、対象ページ、使う道具の目安）。waiting_for は人間の承認待ち。"""
        return call(service.next, project, limit)

    @server.tool(structured_output=False)
    def inspect(project: str, target: str, page: int | None = None) -> list:
        """読む。target: bible / script / page（そのページの beat、前後ページ、めくりの位置、定型） / schemas / rules / snapshot。"""
        return call(service.inspect, project, target, page)

    @server.tool(structured_output=False)
    def render(project: str, page: int, mode: str = "name", max_px: int = 1024) -> list:
        """ページのプレビュー画像（mode: name / proof）。コマ番号は読み順。画像はファイルにも保存する。"""
        return call(service.render, project, page, mode, max_px)

    @server.tool(structured_output=False)
    def set_bible(project: str, bible: dict, commit: bool = False) -> list:
        """企画書（bible@1）を検査し、commit=true で保存する。形式は inspect target=schemas。"""
        return call(service.set_bible, project, bible, commit)

    @server.tool(structured_output=False)
    def set_script(project: str, script: dict, commit: bool = False) -> list:
        """脚本（script@1: scene → beat、beat ごとに page）を検査し、commit=true で保存する。"""
        return call(service.set_script, project, script, commit)

    @server.tool(structured_output=False)
    def submit_name(project: str, plan: dict, commit: bool = False, replace: bool = False) -> list:
        """1ページのネーム計画（name_plan@1）を検査・コマ割り・写植し、プレビュー画像を返す。
        commit=true で保存。すでにネームがあるページを作り直すときは replace=true。"""
        return call(service.submit_name, project, plan, commit, replace)

    @server.tool(structured_output=False)
    def apply_ops(project: str, ops: list[dict], commit: bool = False) -> list:
        """細かい修正（台詞の移動・編集、コマの分割・結合など）。承認・ロック・画像読み込みの op は使えない。"""
        return call(service.apply_ops, project, ops, commit)

    @server.tool(structured_output=False)
    def record_review(project: str, page: int, notes: str, score: float | None = None) -> list:
        """プレビューを見た自己点検の結果を残す（同じネームで点検を繰り返さないため）。"""
        return call(service.record_review, project, page, score, notes)

    @server.tool(structured_output=False)
    def request_approval(project: str, pages: list[int], note: str = "", gate: str = "name") -> list:
        """人間にネームの承認を依頼する。承認は人間が行う。"""
        return call(service.request_approval, project, gate, pages, note)

    @server.tool(structured_output=False)
    def tickets(project: str, status: str = "open") -> list:
        """承認依頼と、人間からの修正指示の一覧（status: open / all）。"""
        return call(service.tickets, project, status)

    @server.resource("genko://guide/manga-rules", mime_type="text/markdown")
    def manga_rules() -> str:
        """ネームの規則（段組 DSL、読み順、めくり、台詞の長さ）。"""
        return RULES_PATH.read_text(encoding="utf-8")

    return server


def run_stdio(root: Path, actor: str) -> None:
    build_server(root, actor).run("stdio")
