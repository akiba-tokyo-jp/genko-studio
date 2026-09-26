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
from genko.studio.service import RULES_PATH, StudioService, ToolResult, wording_error

SKILL_PATH = Path(__file__).resolve().parent.parent / "studio" / "guide" / "SKILL.md"

INSTRUCTIONS = """Genko は漫画原稿のシステム。文章も絵も作らない。あなた（エージェント）が企画書・脚本・ネーム計画を書き、
Genko が検査・コマ割り・縦書き写植・プレビュー描画をする。絵はあなたが別の道具で生成し、import_image で取り込む。
進め方: status → next で次の作業を取る → 書く道具は commit=false で試し、issues の path を直してから commit=true。
ネームの規則は resource genko://guide/manga-rules（inspect target=rules でも読める）。
作画: generation_request で依頼パック（サイズ・プロンプトの下書き・描かせないもの・ガイドと参照の画像）を受け取る →
自分の画像ツールで生成し、画像を返された inbox フォルダに保存 → import_images（来歴 origin を必ず付ける）→
candidates と render kind=compare で比べる → review_candidates → adopt。コマの外にはみ出した部分は自動で切り取られる。
採用後は report_regions で顔と人物の位置を報告し、作画の承認後に finish_page。最後に check（人と同じ点検）・preflight と export（各形式）。
描く・直す: apply_ops でページ・コマ・レイヤー（複製・結合・マスク）・線・塗り・グラデーション・台詞（傍点・部分書式・回転）・3D（背景は add_scene）・トーン・効果線まで、人が画面でできることは全部できる（op 一覧は resource genko://ops）。間違えたら undo（自分の変更だけ）。
使える素材・書体・ブラシは inspect target=materials / fonts / brushes、レイヤーと台詞の今の設定は inspect target=snapshot。
render は layer_id でそのレイヤーだけ、mode=print で印刷と同じ見え方。
3 回直しても通らないときは ask_human で人間に相談して、その作業を置いておく。
承認と本番の書き出しは人間だけが行う。承認が要るところでは request_approval を出して待つ。
同じ Genko を複数の会話（Telegram のスレッドなど）で使うときは、どの道具にも session に会話の名前（例 "9204"）を渡す。
変更はその名前で記録され、undo はその会話の変更だけを戻す。別の会話が使っている原稿に書くと、一度だけ book_in_use で止まる。"""


def _out(result: ToolResult) -> list[Any]:
    content: list[Any] = [json.dumps(result.to_dict(), ensure_ascii=False)]
    content.extend(Image(data=png, format="png") for png in result.images)
    return content


READ_ONLY = frozenset({
    "projects", "status", "next", "inspect", "render", "candidates", "preflight", "check", "tickets", "proposals",
    "review_page", "export", "export_proof", "export_status",
})


def build_server(root: Path, actor: str) -> MCPServer:
    import contextvars
    import functools
    import inspect as _inspect

    if not actor.startswith("ai:"):
        raise ApplyError("MCP サーバーの actor は ai:<名前> にする（人間の権限はエージェントに渡さない）")
    service = StudioService(root, actor)
    server = MCPServer("genko", instructions=INSTRUCTIONS)
    current_session: contextvars.ContextVar[str | None] = contextvars.ContextVar("genko_session", default=None)

    def tool(fn):
        """Register a tool with one more argument, session: the conversation's own name. Changes are recorded
        as <agent>/<session>, undo takes back only that conversation's changes, and claims are its own."""
        signature = _inspect.signature(fn, eval_str=True)
        extra = _inspect.Parameter("session", _inspect.Parameter.KEYWORD_ONLY, default=None, annotation=str | None)

        @functools.wraps(fn)
        def wrapped(*args, session: str | None = None, **kwargs):
            token = current_session.set(session)
            try:
                return fn(*args, **kwargs)
            finally:
                current_session.reset(token)

        wrapped.__signature__ = signature.replace(parameters=[*signature.parameters.values(), extra])
        return server.tool(structured_output=False)(wrapped)

    def call(fn, *args, **kwargs) -> list[Any]:
        import time

        from genko.studio import presence, toollog

        started = time.perf_counter()
        try:
            who = presence.actor_for(actor, current_session.get())
        except ValueError as exc:
            return [json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False)]
        method = getattr(StudioService(root, who), fn.__name__)
        bound: dict = {}
        try:
            bound = dict(_inspect.signature(method).bind(*args, **kwargs).arguments)
        except TypeError:
            pass
        project = bound.get("project") or (bound.get("name") if fn.__name__ == "create_project" else None)
        path = None
        if project:
            try:
                path = service.project_path(str(project))
            except ApplyError:
                path = None
        writes = path is not None and fn.__name__ not in READ_ONLY and bound.get("commit", True) is not False
        if writes and path.is_dir():
            busy = presence.check(path, who)
            if busy:  # (held back once: the same call again goes through)
                names = "、".join(f"{o['actor']}（{o['minutes_ago']} 分前）" for o in busy)
                return [json.dumps({"ok": False, "error": f"この原稿は別の会話も使っている: {names}。書き込まなかった。"
                                    "同じ原稿で続けるなら、もう一度同じ呼び出しをする（人に確かめてから）。"
                                    "別々に進めるなら、原稿を複製して使う",
                                    "code": "book_in_use", "others": busy, "you": who}, ensure_ascii=False)]
        try:
            result = method(*args, **kwargs)
            error = None
        except ApplyError as exc:
            result, error = None, str(exc)
        if path is not None:
            toollog.record(path, who, fn.__name__, bound, result.to_dict() if result else None, error,
                           (time.perf_counter() - started) * 1000)
            if writes and result is not None and result.ok:
                presence.touch(path, who)
        if result is None:
            shown = wording_error(error or "")
            return [json.dumps({"ok": False, "error": shown, **({"detail": error} if shown != error else {})}, ensure_ascii=False)]
        return _out(result)

    @tool
    def projects() -> list:
        """--root 配下のプロジェクト一覧。"""
        return call(service.projects)

    @tool
    def create_project(name: str, title: str, pages: int, spec_preset: str = "commercial-b4") -> list:
        """新しいプロジェクトを作る。name は --root からの相対名（例 summer.genko）。spec_preset: commercial-b4 / a4-mono / webtoon。"""
        return call(service.create_project, name, title, pages, spec_preset)

    @tool
    def status(project: str) -> list:
        """工程の様子（企画書・脚本の有無、ページごとのネームと承認、承認待ち）。"""
        return call(service.status, project)

    @tool
    def next(project: str, limit: int = 5, claim: bool = False) -> list:
        """次にやる作業（kind、対象ページ・コマ、使う道具の目安）。waiting_for は人間の承認待ち。
        並行して動くときは claim=true で返した作業を 10 分予約する（他のエージェントには出ない）。"""
        return call(service.next, project, limit, claim)

    @tool
    def inspect(project: str, target: str, page: int | None = None, frame_id: str | None = None) -> list:
        """読む。target: bible / script / page（そのページの beat、前後ページ、めくりの位置、定型、コマ一覧） /
        panel（コマのブリーフ・寸法 mm・候補・登場人物の設定画、frame_id 省略でページ全部） / studio / schemas / rules /
        snapshot（ページ・レイヤー〔名前・種類・不透明度・合成・マスク・表示色・フォルダ・参照〕・台詞〔書式・フキダシ〕・3D） /
        materials（stamp_material で貼れる素材: id・名前・種類〔トーン・効果線・画像・パーツ・描き文字・ブラシ・3D〕・フォルダ・タグ） / fonts（style.font に使える書体） /
        brushes（add_stroke の kind に使えるブラシ: 入っているもの・この原稿の自作・自分の自作） / upscalers（upscale の method） / plugins（人が入れた
        フィルターのプラグイン: filter_raster の kind に "plugin:<key>"、params は PARAMS のとおり。置き場所は folder:
        Linux は ~/.config/genko/plugins、Windows は %APPDATA%\\genko\\plugins）。"""
        return call(service.inspect, project, target, page, frame_id)

    @tool
    def render(project: str, page: int, mode: str = "name", max_px: int = 1024, frame_id: str | None = None,
               kind: str | None = None, candidate_id: str | None = None, layer_id: str | None = None) -> list:
        """ページのプレビュー画像（mode: name / proof / print。print は印刷と同じ見え方）。コマ番号は読み順。
        frame_id を渡すとそのコマだけ。layer_id を渡すとそのレイヤーだけを白の上に。
        kind: compare（候補にネームを赤で重ねる。candidate_id 省略で採用中の絵）/ guide:composition / guide:pose / guide:keepout /
        atari（アタリと提案の重ね表示）。
        画像はファイルにも保存する。"""
        return call(service.render, project, page, mode, max_px, frame_id, kind, candidate_id, layer_id)

    @tool
    def import_image(project: str, path: str | None = None, png_base64: str | None = None) -> list:
        """生成した画像を取り込み、asset（sha256:…）と画素数 px を返す。path は --root の中のファイル。
        取り込んだだけではどこにも使われない。apply_ops の import_candidates でコマの候補にする。"""
        return call(service.import_image, project, path, png_base64)

    @tool
    def generation_request(project: str, page: int | None = None, frame_id: str | None = None,
                           character_id: str | None = None, location_id: str | None = None, purpose: str | None = None,
                           mode: str = "new", parent: str | None = None, tool: str | None = None,
                           instruction: str | None = None, regions: list | None = None,
                           focus_character: str | None = None) -> list:
        """絵の依頼パックを作る（Genko は生成しない）。コマは page+frame_id、設定画は character_id、背景の参照は location_id。
        purpose: panel_art / draft / character_sheet / location。mode: new / edit / inpaint（regions に領域 id、
        "face:<人物 id>"、"person:<人物 id>" か rect_mm）/ upscale。複数人物のコマで一人だけ直すときは focus_character。
        修正は parent に元の候補 id。tool は tools.json のツール id（例 openai:gpt-image-1）。
        返す: request（サイズの候補、prompt の ja/en/tags、avoid、keepout、figures）、files（ガイドと参照画像の場所）、inbox。
        同じ内容なら同じ id。"""
        return call(service.generation_request, project, page, frame_id, character_id, location_id, purpose, mode,
                    parent, tool, instruction, regions, focus_character)

    @tool
    def import_images(project: str, request_id: str, images: list[dict]) -> list:
        """生成した画像を依頼の候補として取り込む。images: [{file: "studio/inbox/<request_id>/a.png" か asset: "sha256:…",
        origin: {kind: "agent", tool_id, model, prompt（実際に使ったもの）, params, refs_used, note}}]。
        file は studio/inbox/ の中だけ。同じ画像は 2 回取り込まれない。設定画では face_box01: [x, y, 幅, 高さ]（画像の中の
        0〜1）で顔のアップの範囲を付ける（承認のとき、ここを顔の参照として切り出す）。"""
        return call(service.import_images, project, request_id, images)

    @tool
    def candidates(project: str, page: int | None = None, frame_id: str | None = None,
                   character_id: str | None = None, location_id: str | None = None) -> list:
        """候補の一覧（状態、点数、stale、来歴の要約、取り込み時の目安 metrics。rank が小さいほど良い）と並べた縮小画像。"""
        return call(service.candidates, project, page, frame_id, character_id, location_id)

    @tool
    def review_candidates(project: str, page: int, frame_id: str, reviews: list[dict]) -> list:
        """候補の評価を残す。page と frame_id（どのコマの候補か）は必須。reviews: [{candidate_id, score (0..1), note, fix?}]。"""
        return call(service.review_candidates, project, page, frame_id, reviews)

    @tool
    def adopt(project: str, candidate_id: str, page: int | None = None, frame_id: str | None = None, to: str = "art",
              fit: str | None = None, offset_mm: list[float] | None = None, scale: float | None = None,
              location_id: str | None = None) -> list:
        """候補を採用してコマに置く（to: art / bg / draft。fit: cover / contain / stretch）。場所の参照画像は location_id。
        作画の確定は人間の art 承認で行う。"""
        return call(service.adopt, project, candidate_id, page, frame_id, to, fit, offset_mm, scale, location_id)

    @tool
    def request_fix(project: str, page: int, instruction: str, frame_id: str | None = None,
                    candidate_id: str | None = None, scope: str = "frame") -> list:
        """修正のチケットを残す（自分で直す予定のメモ、または人間への相談）。"""
        return call(service.request_fix, project, page, instruction, frame_id, candidate_id, scope)

    @tool
    def report_regions(project: str, page: int, frame_id: str, regions: list[dict]) -> list:
        """採用した絵の顔と人物の位置を報告する（写植の顔よけに使う）。regions: [{kind: face|person, char?,
        box01: [x, y, w, h]（採用した画像の中の 0..1）か rect_mm}]。人物のいない絵なら record_review kind=regions。"""
        return call(service.report_regions, project, page, frame_id, regions)

    @tool
    def finish_page(project: str, page: int, commit: bool = False) -> list:
        """作画承認済みのページを仕上げる（顔にかかる台詞の移動、効果、finish へ）。commit=false で提案とプレビューだけ。"""
        return call(service.finish_page, project, page, commit)

    @tool
    def preflight(project: str) -> list:
        """書き出しを止めている理由の一覧（承認、未採用のコマ、実効解像度、試験用の画像、来歴）。"""
        return call(service.preflight, project)

    @tool
    def export_proof(project: str, format: str = "pdf") -> list:  # noqa: A002
        """校正用の書き出し（150 dpi、全ページに「校正」の透かし）。本番の書き出しは人間が行う。40 秒で終わらないときは job を返す。"""
        return call(service.export_proof, project, format, background=True)

    @tool
    def check(project: str) -> list:
        """人が使う「入稿前の点検」と同じ点検（はみ出し・文字の小ささや重なり・印刷に出ない絵など）。見つかった一つずつは checks に入る。"""
        return call(service.check, project)

    @tool
    def undo(project: str) -> list:
        """自分（このエージェント）の最後の保存済みの変更を取り消す。次のときは断る: 最後の変更が人（や別のエージェント）のもの、
        承認が変わる変更、project.json が Genko の外で書き換えられた（記録と中身が合わない）、取り消すものが無い。"""
        return call(service.undo, project)

    @tool
    def export(project: str, format: str = "pdf", pages: list[int] | None = None, dpi: int | None = None,  # noqa: A002
               area: str = "bleed", width: int = 800, max_height: int = 1280, long_edge: int | None = None, jpeg: bool = False,
               spreads: bool = False, color: str = "rgb", icc: str | None = None, fps: float = 12,
               seconds: float | None = None, movie: str = "webp") -> list:
        """書き出し（承認は要らない。正式な書き出しは人だけ）: format は pdf / tiff / png / cmyk / layers / psd / pack / epub /
        kindle / strip / webtoon / sns / timelapse / animation。pages でページを選ぶ（例 [3, 4, 5]）。area は paper / bleed / trim。
        pdf の color は rgb / cmyk / gray、cmyk と pdf の icc は印刷所の CMYK プロファイル（.icc のパス）。long_edge の既定は kindle 2560・sns 2048。timelapse は記録した制作過程（set_timelapse で記録）を movie（webp / gif / png / mp4）で、fps と
        seconds（全体の長さ）、pages は省略で全ページ（描いた順）か、1 ページだけを [n] で。animation はアニメーションのページ（pages に 1 つ）を movie（gif / webp /
        png / mp4 / frames〔連番 PNG〕）で、width で幅を。書いた先は <原稿>/exports/。
        40 秒で終わらないときは job を返す（書き出しは続いている）。export_status で結果を取る。"""
        return call(service.export, project, format, pages, dpi, area, width, max_height, long_edge, jpeg, spreads, color, icc,
                    fps, seconds, movie, background=True)

    @tool
    def export_status(project: str, job: str) -> list:
        """export / export_proof / upscale が job を返したとき（40 秒で終わらなかった処理）の様子。status: running / done / failed /
        lost（Genko が途中で止まった）。done なら result に書き出しの返事（files など）が入る。"""
        return call(service.export_status, project, job)

    @tool
    def upscale(project: str, page: int, frame_id: str, candidate_id: str | None = None, scale: float | None = None,
                method: str = "genko") -> list:
        """採用した絵（か candidate_id の候補）を拡大して、新しい候補にする（印刷の解像度に届かないとき）。method: "genko"
        （Genko の拡大: Lanczos と線の輪郭の整え）か、人が登録した高解像度化の道具（inspect target=upscalers）。scale の既定は
        原稿の dpi に届く倍率（4 倍まで）。描き込みは増えない。候補には拡大した印が付き、preflight に出る。できた候補は adopt で置く。
        40 秒で終わらないときは job を返す（export_status で結果を取る）。"""
        return call(service.upscale, project, page, frame_id, candidate_id, scale, method, background=True)

    @tool
    def derive(project: str, page: int, frame_id: str, kind: str = "lineart", candidate_id: str | None = None,
               params: dict | None = None) -> list:
        """Genko の決定的な処理で候補を作る。kind lineart: 候補（省略で採用中の絵）の線を抜き出した黒線の層。
        adopt の to: "ink" で置くと、トーンにした絵の上にくっきりした線が乗る。params: {radius, threshold, min_px}。"""
        return call(service.derive, project, page, frame_id, kind, candidate_id, params)

    @tool
    def import_name(project: str, files: list[str], start_page: int = 1, align: str = "auto") -> list:
        """人間が描いたアタリ（スキャン画像）を取り込む。files は --root の中の画像（1 ページ 1 枚、start_page から）。
        原本は資産になり下描き層に置かれる（印刷されない）。コマ割りを検出して提案にする。確定は人間。"""
        return call(service.import_name, project, files, start_page, align)

    @tool
    def analyze_name(project: str, page: int, params: dict | None = None) -> list:
        """アタリのコマ割りを検出し直す（params: min_gutter_mm, min_panel_mm, speck_mm など）。新しい提案と重ね表示を返す。"""
        return call(service.analyze_name, project, page, params)

    @tool
    def propose_lines(project: str, page: int, lines: list[dict]) -> list:
        """アタリの手書き台詞を読んだ結果を提案する。lines: [{text（列は \n で区切る）, balloon?, speaker?,
        box01: [x, y, w, h]（アタリ画像の中の 0..1）か x_mm, y_mm（w_mm, h_mm は省略可）}]。確定は人間。重ね表示を返す。"""
        return call(service.propose_lines, project, page, lines)

    @tool
    def proposals(project: str, status: str = "open") -> list:
        """コマ割りと台詞の提案の一覧（status: open / all）。"""
        return call(service.proposals, project, status)

    @tool
    def review_page(project: str) -> list:
        """人間の確認用ページ（studio/review.html）を作り、場所を返す。承認を頼んだら、この場所をメッセージで人間に知らせる。"""
        return call(service.review_page, project)

    @tool
    def ask_human(project: str, text: str, page: int | None = None, frame_id: str | None = None, item: str | None = None) -> list:
        """人間に相談する（3 回直しても通らないときなど）。そのページ・コマの作業は人間が閉じるまで next に出ない。"""
        return call(service.ask_human, project, text, page, frame_id, item)

    @tool
    def set_bible(project: str, bible: dict, commit: bool = False) -> list:
        """企画書（bible@1）を検査し、commit=true で保存する。形式は inspect target=schemas。"""
        return call(service.set_bible, project, bible, commit)

    @tool
    def set_script(project: str, script: dict, commit: bool = False) -> list:
        """脚本（script@1: scene → beat、beat ごとに page）を検査し、commit=true で保存する。"""
        return call(service.set_script, project, script, commit)

    @tool
    def submit_name(project: str, plan: dict, commit: bool = False, replace: bool = False) -> list:
        """1ページのネーム計画（name_plan@1）を検査・コマ割り・写植し、プレビュー画像を返す。
        commit=true で保存。すでにネームがあるページを作り直すときは replace=true。"""
        return call(service.submit_name, project, plan, commit, replace)

    @tool
    def apply_ops(project: str, ops: list[dict], commit: bool = False) -> list:
        """細かい修正と作画の状態（台詞の移動・編集、コマの分割・結合、set_panel、import_candidates、adopt_candidate、
        set_placement など。一覧は inspect target=schemas ではなく genko://ops）。承認・ロック・ファイル読み込みの op は使えない
        （import_psd だけは読める: path は原稿のフォルダからの相対パスか、--root の中の絶対パス）。返事の results に、
        一部の op が見つけたもの・作ったもの（replace_text の件数と場所、import_psd のレイヤー）が入る。"""
        return call(service.apply_ops, project, ops, commit)

    @tool
    def record_review(project: str, page: int, notes: str, score: float | None = None) -> list:
        """プレビューを見た自己点検の結果を残す（同じネームで点検を繰り返さないため）。"""
        return call(service.record_review, project, page, score, notes)

    @tool
    def request_approval(project: str, pages: list[int] | None = None, note: str = "", gate: str = "name",
                         character_id: str | None = None) -> list:
        """人間に承認を依頼する。gate: name（ネーム）/ art（そのページの絵）/ sheet（キャラクター設定画、character_id が要る）/
        export（全ページの仕上げ後、本番の書き出し）。"""
        return call(service.request_approval, project, gate, pages or [], note, character_id)

    @tool
    def tickets(project: str, status: str = "open") -> list:
        """承認依頼と、人間からの修正指示の一覧（status: open / all）。"""
        return call(service.tickets, project, status)

    @tool
    def resolve_ticket(project: str, ticket_id: str, note: str) -> list:
        """人間からの直しの指示（kind fix、担当 agent のチケット）を直し終えたと返して閉じる。note に何をしたかを書く。
        承認の依頼や ask_human の質問は閉じられない（人が閉じる）。人は genko studio reopen-ticket で開き直せる。"""
        return call(service.resolve_ticket, project, ticket_id, note)

    @server.resource("genko://ops", mime_type="application/json")
    def ops_catalog() -> str:
        """apply_ops で使える op と引数。"""
        from genko.ops import OPS_SCHEMA
        from genko.studio.service import AGENT_OPS

        return json.dumps([op for op in OPS_SCHEMA if op["op"] in AGENT_OPS], ensure_ascii=False)

    @server.resource("genko://guide/skill", mime_type="text/markdown")
    def skill() -> str:
        """Hermes 用のスキル（作業の手順、止まるところ、してはいけないこと）の正本。"""
        return SKILL_PATH.read_text(encoding="utf-8") if SKILL_PATH.is_file() else ""

    @server.resource("genko://guide/manga-rules", mime_type="text/markdown")
    def manga_rules() -> str:
        """ネームの規則（段組 DSL、読み順、めくり、台詞の長さ）。"""
        return RULES_PATH.read_text(encoding="utf-8")

    return server


def run_stdio(root: Path, actor: str) -> None:
    build_server(root, actor).run("stdio")
