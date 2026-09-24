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
Genko が検査・コマ割り・縦書き写植・プレビュー描画をする。絵はあなたが別の道具で生成し、import_image で取り込む。
進め方: status → next で次の作業を取る → 書く道具は commit=false で試し、issues の path を直してから commit=true。
ネームの規則は resource genko://guide/manga-rules（inspect target=rules でも読める）。
作画: generation_request で依頼パック（サイズ・プロンプトの下書き・描かせないもの・ガイドと参照の画像）を受け取る →
自分の画像ツールで生成し、画像を返された inbox フォルダに保存 → import_images（来歴 origin を必ず付ける）→
candidates と render kind=compare で比べる → review_candidates → adopt。コマの外にはみ出した部分は自動で切り取られる。
採用後は report_regions で顔と人物の位置を報告し、作画の承認後に finish_page。最後に preflight と export_proof。
3 回直しても通らないときは ask_human で人間に相談して、その作業を置いておく。
承認と本番の書き出しは人間だけが行う。承認が要るところでは request_approval を出して待つ。"""


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
    def next(project: str, limit: int = 5, claim: bool = False) -> list:
        """次にやる作業（kind、対象ページ・コマ、使う道具の目安）。waiting_for は人間の承認待ち。
        並行して動くときは claim=true で返した作業を 10 分予約する（他のエージェントには出ない）。"""
        return call(service.next, project, limit, claim)

    @server.tool(structured_output=False)
    def inspect(project: str, target: str, page: int | None = None, frame_id: str | None = None) -> list:
        """読む。target: bible / script / page（そのページの beat、前後ページ、めくりの位置、定型、コマ一覧） /
        panel（コマのブリーフ・寸法 mm・候補・登場人物の設定画、frame_id 省略でページ全部） / studio / schemas / rules / snapshot。"""
        return call(service.inspect, project, target, page, frame_id)

    @server.tool(structured_output=False)
    def render(project: str, page: int, mode: str = "name", max_px: int = 1024, frame_id: str | None = None,
               kind: str | None = None, candidate_id: str | None = None) -> list:
        """ページのプレビュー画像（mode: name / proof / print）。コマ番号は読み順。frame_id を渡すとそのコマだけ。
        kind: compare（候補にネームを赤で重ねる。candidate_id 省略で採用中の絵）/ guide:composition / guide:pose / guide:keepout。
        画像はファイルにも保存する。"""
        return call(service.render, project, page, mode, max_px, frame_id, kind, candidate_id)

    @server.tool(structured_output=False)
    def import_image(project: str, path: str | None = None, png_base64: str | None = None) -> list:
        """生成した画像を取り込み、asset（sha256:…）と画素数 px を返す。path は --root の中のファイル。
        取り込んだだけではどこにも使われない。apply_ops の import_candidates でコマの候補にする。"""
        return call(service.import_image, project, path, png_base64)

    @server.tool(structured_output=False)
    def generation_request(project: str, page: int | None = None, frame_id: str | None = None,
                           character_id: str | None = None, location_id: str | None = None, purpose: str | None = None,
                           mode: str = "new", parent: str | None = None, tool: str | None = None,
                           instruction: str | None = None, regions: list | None = None) -> list:
        """絵の依頼パックを作る（Genko は生成しない）。コマは page+frame_id、設定画は character_id、背景の参照は location_id。
        purpose: panel_art / draft / character_sheet / location。mode: new / edit / inpaint（regions に領域 id か rect_mm）/ upscale。
        修正は parent に元の候補 id。tool は tools.json のツール id（例 openai:gpt-image-1）。
        返す: request（サイズの候補、prompt の ja/en/tags、avoid、keepout、figures）、files（ガイドと参照画像の場所）、inbox。
        同じ内容なら同じ id。"""
        return call(service.generation_request, project, page, frame_id, character_id, location_id, purpose, mode,
                    parent, tool, instruction, regions)

    @server.tool(structured_output=False)
    def import_images(project: str, request_id: str, images: list[dict]) -> list:
        """生成した画像を依頼の候補として取り込む。images: [{file: "studio/inbox/<request_id>/a.png" か asset: "sha256:…",
        origin: {kind: "agent", tool_id, model, prompt（実際に使ったもの）, params, refs_used, note}}]。
        file は studio/inbox/ の中だけ。同じ画像は 2 回取り込まれない。"""
        return call(service.import_images, project, request_id, images)

    @server.tool(structured_output=False)
    def candidates(project: str, page: int | None = None, frame_id: str | None = None,
                   character_id: str | None = None, location_id: str | None = None) -> list:
        """候補の一覧（状態、点数、stale、来歴の要約、取り込み時の目安 metrics。rank が小さいほど良い）と並べた縮小画像。"""
        return call(service.candidates, project, page, frame_id, character_id, location_id)

    @server.tool(structured_output=False)
    def review_candidates(project: str, page: int, frame_id: str, reviews: list[dict]) -> list:
        """候補の評価を残す。reviews: [{candidate_id, score (0..1), note, fix?}]。"""
        return call(service.review_candidates, project, page, frame_id, reviews)

    @server.tool(structured_output=False)
    def adopt(project: str, candidate_id: str, page: int | None = None, frame_id: str | None = None, to: str = "art",
              fit: str | None = None, offset_mm: list[float] | None = None, scale: float | None = None,
              location_id: str | None = None) -> list:
        """候補を採用してコマに置く（to: art / bg / draft。fit: cover / contain / stretch）。場所の参照画像は location_id。
        作画の確定は人間の art 承認で行う。"""
        return call(service.adopt, project, candidate_id, page, frame_id, to, fit, offset_mm, scale, location_id)

    @server.tool(structured_output=False)
    def request_fix(project: str, page: int, instruction: str, frame_id: str | None = None,
                    candidate_id: str | None = None, scope: str = "frame") -> list:
        """修正のチケットを残す（自分で直す予定のメモ、または人間への相談）。"""
        return call(service.request_fix, project, page, instruction, frame_id, candidate_id, scope)

    @server.tool(structured_output=False)
    def report_regions(project: str, page: int, frame_id: str, regions: list[dict]) -> list:
        """採用した絵の顔と人物の位置を報告する（写植の顔よけに使う）。regions: [{kind: face|person, char?,
        box01: [x, y, w, h]（採用した画像の中の 0..1）か rect_mm}]。人物のいない絵なら record_review kind=regions。"""
        return call(service.report_regions, project, page, frame_id, regions)

    @server.tool(structured_output=False)
    def finish_page(project: str, page: int, commit: bool = False) -> list:
        """作画承認済みのページを仕上げる（顔にかかる台詞の移動、効果、finish へ）。commit=false で提案とプレビューだけ。"""
        return call(service.finish_page, project, page, commit)

    @server.tool(structured_output=False)
    def preflight(project: str) -> list:
        """書き出しを止めている理由の一覧（承認、未採用のコマ、実効解像度、試験用の画像、来歴）。"""
        return call(service.preflight, project)

    @server.tool(structured_output=False)
    def export_proof(project: str, format: str = "pdf") -> list:  # noqa: A002
        """校正用の書き出し（150 dpi、全ページに「校正」の透かし）。本番の書き出しは人間が行う。"""
        return call(service.export_proof, project, format)

    @server.tool(structured_output=False)
    def ask_human(project: str, text: str, page: int | None = None, frame_id: str | None = None, item: str | None = None) -> list:
        """人間に相談する（3 回直しても通らないときなど）。そのページ・コマの作業は人間が閉じるまで next に出ない。"""
        return call(service.ask_human, project, text, page, frame_id, item)

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
        """細かい修正と作画の状態（台詞の移動・編集、コマの分割・結合、set_panel、import_candidates、adopt_candidate、
        set_placement など。一覧は inspect target=schemas ではなく genko://ops）。承認・ロック・ファイル読み込みの op は使えない。"""
        return call(service.apply_ops, project, ops, commit)

    @server.tool(structured_output=False)
    def record_review(project: str, page: int, notes: str, score: float | None = None) -> list:
        """プレビューを見た自己点検の結果を残す（同じネームで点検を繰り返さないため）。"""
        return call(service.record_review, project, page, score, notes)

    @server.tool(structured_output=False)
    def request_approval(project: str, pages: list[int] | None = None, note: str = "", gate: str = "name",
                         character_id: str | None = None) -> list:
        """人間に承認を依頼する。gate: name（ネーム）/ art（そのページの絵）/ sheet（キャラクター設定画、character_id が要る）/
        export（全ページの仕上げ後、本番の書き出し）。"""
        return call(service.request_approval, project, gate, pages or [], note, character_id)

    @server.tool(structured_output=False)
    def tickets(project: str, status: str = "open") -> list:
        """承認依頼と、人間からの修正指示の一覧（status: open / all）。"""
        return call(service.tickets, project, status)

    @server.resource("genko://ops", mime_type="application/json")
    def ops_catalog() -> str:
        """apply_ops で使える op と引数。"""
        from genko.ops import OPS_SCHEMA
        from genko.studio.service import AGENT_OPS

        return json.dumps([op for op in OPS_SCHEMA if op["op"] in AGENT_OPS], ensure_ascii=False)

    @server.resource("genko://guide/manga-rules", mime_type="text/markdown")
    def manga_rules() -> str:
        """ネームの規則（段組 DSL、読み順、めくり、台詞の長さ）。"""
        return RULES_PATH.read_text(encoding="utf-8")

    return server


def run_stdio(root: Path, actor: str) -> None:
    build_server(root, actor).run("stdio")
