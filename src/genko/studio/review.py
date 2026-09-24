"""Name previews with reading-order badges, and review.html for the human approver."""

from __future__ import annotations

import base64
import html
import io
from pathlib import Path

from PIL import Image, ImageDraw

from genko.io import load_episode
from genko.models import Page
from genko.render import _font, mm_to_px, render_page
from genko.studio import state
from genko.studio.worklist import page_side


def annotate(image: Image.Image, page: Page, dpi: int, plan: dict | None) -> Image.Image:
    """Draw non-printing badges: reading order, slot and a short brief per panel."""
    out = image.convert("RGB")
    draw = ImageDraw.Draw(out)
    panels = {}
    if plan:
        panels = {p.get("slot"): p for p in plan.get("panels", [])}
    slots_by_order = []
    if plan and plan.get("tiers"):
        from genko.studio.layout import slots_in_order

        slots_by_order = slots_in_order(plan["tiers"])
    elif plan and plan.get("template"):
        from genko.studio.layout import resolve_tiers, slots_in_order

        slots_by_order = slots_in_order(resolve_tiers(plan))
    badge = max(10, mm_to_px(6, dpi))
    font = _font(None, max(10, badge * 2 // 3))
    small = _font(None, max(9, badge // 2))
    for order, frame in enumerate(page.leaf_frames(), start=1):
        r = frame.rect
        x2 = mm_to_px(r.x + r.width, dpi)
        y = mm_to_px(r.y, dpi)
        x = mm_to_px(r.x, dpi)
        draw.rectangle([x, y, x + badge, y + badge], fill=(40, 90, 200))
        draw.text((x + badge // 5, y + badge // 8), str(order), fill=(255, 255, 255), font=font)
        slot = slots_by_order[order - 1] if order - 1 < len(slots_by_order) else ""
        panel = panels.get(slot)
        if panel:
            label = f"{slot} {panel.get('shot')}/{panel.get('angle')} {panel.get('action', '')}"[:40]
            ly = mm_to_px(r.y + r.height, dpi) - badge
            draw.rectangle([x, ly, x2, ly + badge], fill=(235, 240, 255))
            draw.text((x + 3, ly + 2), label, fill=(40, 60, 140), font=small)
    return out


def _png_b64(image: Image.Image) -> str:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _thumb_b64(store, ref: str, size: int = 160) -> str | None:
    data = store.get_bytes(ref, ".png") if ref else None
    if data is None:
        return None
    image = Image.open(io.BytesIO(data)).convert("RGB")
    image.thumbnail((size, size))
    return _png_b64(image)


def _code(text: str) -> str:
    return f"<code>{html.escape(text)}</code>"


def _cmd(text: str, label: str = "コマンドで行う") -> str:
    """A command folded away: people use the app, the command is there for those who want it."""
    return f"<details class='cmd'><summary>{html.escape(label)}</summary>{_code(text)}</details>"


GATE = {"name": "ネーム", "art": "作画", "sheet": "設定画", "export": "書き出し"}
PANEL = {None: "未着手", "empty": "未着手", "briefed": "指示あり", "requested": "依頼済み", "candidates": "候補あり",
         "adopted": "採用済み", "fix_requested": "直し待ち", "skip": "絵なし"}


def _who(actor: str | None) -> str:
    actor = str(actor or "")
    return f"エージェント（{actor[3:]}）" if actor.startswith("ai:") else (actor[6:] if actor.startswith("human:") else actor)


def _words(panel: dict) -> str:
    from genko.studio.genreq import vocab

    v = vocab()
    out = [str((v["shot"].get(panel.get("shot")) or {}).get("ja") or panel.get("shot") or ""),
           str((v["angle"].get(panel.get("angle")) or {}).get("ja") or panel.get("angle") or "")]
    return " / ".join(x for x in out if x)


CSS = (
    ":root{--line:#dee2e6;--muted:#6c757d;--accent:#1c7ed6;--ok:#2b8a3e;--warn:#c92a2a}"
    "*{box-sizing:border-box}body{font-family:system-ui,-apple-system,'Hiragino Sans','Noto Sans JP',sans-serif;"
    "margin:0 auto;padding:16px;max-width:1100px;color:#212529;line-height:1.6;background:#fff}"
    "h1{font-size:1.5rem;margin:.2em 0}h2{font-size:1.2rem;margin:1.6em 0 .6em;border-bottom:2px solid var(--line)}"
    "h3{font-size:1.05rem;margin:.4em 0}.lead{background:#e7f5ff;border-radius:8px;padding:10px 14px}"
    ".card{border:1px solid var(--line);border-radius:8px;padding:10px 14px;margin:8px 0}"
    ".card .what{font-weight:bold}.muted{color:var(--muted);font-size:.9em}"
    ".page{display:flex;gap:16px;border-top:1px solid var(--line);padding:14px 0;flex-wrap:wrap}"
    ".page>div{flex:1 1 320px;min-width:0}.page img.pg{width:100%;max-width:520px;border:1px solid #adb5bd}"
    ".cands{display:flex;gap:8px;flex-wrap:wrap;margin:4px 0 12px}"
    ".cand{border:2px solid var(--line);border-radius:6px;padding:4px;font-size:.8em;width:176px;max-width:47%}"
    ".cand.adopted{border-color:var(--ok)}.cand.rejected{opacity:.5}.cand img{display:block;width:100%;height:auto}"
    "table{border-collapse:collapse;font-size:.9em;width:100%}td,th{border:1px solid var(--line);padding:3px 6px;vertical-align:top;text-align:left}"
    ".scroll{overflow-x:auto}details.cmd{margin:.3em 0;font-size:.85em}details.cmd summary{color:var(--muted);cursor:pointer}"
    "code{display:block;background:#f1f3f5;padding:6px 8px;border-radius:4px;font-size:.85em;white-space:pre-wrap;word-break:break-all}"
    ".warn{color:var(--warn)}.ok{color:var(--ok)}"
    "@media (max-width:600px){body{padding:10px}.cand{width:47%}}"
)


def _candidate_tiles(store, cands: list[dict], adopted: set[str], command=None) -> list[str]:
    parts = ["<div class='cands'>"]
    for n, cand in enumerate(cands, 1):
        b64 = _thumb_b64(store, cand.get("asset", ""), 320)
        review = cand.get("review") or {}
        cls = "cand" + (" adopted" if cand["id"] in adopted else "") + (" rejected" if cand.get("status") == "rejected" else "")
        origin = cand.get("origin") or {}
        label = cand.get("label") or f"候補 {n}"
        lines = [f"<b>{html.escape(label)}</b>" + (" ✔採用" if cand["id"] in adopted else "")]
        if review:
            lines.append(f"評価 {review.get('score')}: {html.escape(str(review.get('note') or ''))}")
        if cand.get("stale"):
            lines.append("<span class='warn'>指示が変わる前の候補</span>")
        tool = " / ".join(str(origin.get(k)) for k in ("tool_id", "model") if origin.get(k))
        if tool:
            lines.append(f"<span class='muted'>{html.escape(tool)}</span>")
        if origin.get("kind") == "fixture":
            lines.append("<span class='warn'>試験用の画像</span>")
        if not cand.get("label"):
            lines.append(f"<span class='muted'>{html.escape(cand['id'])}</span>")
        if command:
            lines.append(_cmd(command(cand), "この候補で承認（コマンド）"))
        img = f"<img alt='{html.escape(label)}' src='data:image/png;base64,{b64}'>" if b64 else "(画像なし)"
        parts.append(f"<div class='{cls}'>{img}{'<br>'.join(lines)}</div>")
    parts.append("</div>")
    return parts


def review_html(project: Path, *, max_px: int = 900) -> str:
    from genko.assets import AssetStore
    from genko.studio import preflight

    episode = load_episode(project)
    store = AssetStore(project)
    proj = str(project)
    requests = state.open_tickets(episode)
    report = preflight.check(episode, project)
    names = {c.get("id"): c.get("name") or c.get("id") for c in episode.bible.characters}
    parts = [
        "<!doctype html><html lang='ja'><head><meta charset='utf-8'>",
        "<meta name='viewport' content='width=device-width,initial-scale=1'>",
        f"<title>{html.escape(episode.title)} の確認</title><style>{CSS}</style></head><body>",
        f"<h1>{html.escape(episode.title)}</h1>",
        "<p class='lead'>このページは見るためのものです。承認・差し戻し・相談への返事は Genko アプリの「承認箱」で行います"
        f"（開き方: {html.escape('genko app')} のあと、この原稿を選ぶ）。エージェントは承認できません。"
        f"<br><span class='muted'>版 r{episode.revision}</span></p>",
    ]
    if requests:
        parts.append(f"<h2>あなたを待っているもの（{len(requests)} 件）</h2>")
        for req in requests:
            kind = req.get("kind")
            pages = req.get("pages") or ([req["page_index"]] if req.get("page_index") else [])
            where = f"{'・'.join(map(str, pages))} ページ" if pages else ""
            action = ""
            if kind == "gate":
                gate = req.get("gate")
                target = names.get(req.get("character_id"), "") if gate == "sheet" else where
                what = f"{GATE.get(gate, gate)}の承認（{target}）" if target else f"{GATE.get(gate, gate)}の承認"
                if gate in ("name", "art"):
                    action = f"genko studio approve {proj} {gate} --pages {','.join(map(str, req.get('pages') or []))}"
                elif gate == "sheet":
                    action = f"genko studio approve {proj} sheet --character {req.get('character_id')} --candidate 候補id"
                elif gate == "export":
                    action = f"genko studio export {proj} --format pdf --out 出力先"
            elif kind == "help":
                what = f"相談（{where or '全体'}）"
                action = f"genko studio close-ticket {proj} {req['id']} --reply 返事"
            else:
                what = f"{where}への指示"
            parts.append(f"<div class='card'><div class='what'>{html.escape(what)}</div>"
                         + (f"<div>{html.escape(req.get('text') or '')}</div>" if req.get("text") else "")
                         + f"<div class='muted'>依頼: {html.escape(_who(req.get('created_by')))}</div>"
                         + (_cmd(action) if action else "") + "</div>")
    proposals = [p for p in (episode.studio.get("proposals") or {}).values() if p.get("status") == "open"]
    if proposals:
        from genko.studio.atari import overlay_image

        parts.append("<h2>アタリからの提案（確定するまで原稿は変わりません）</h2>")
        for proposal in proposals:
            page = next((p for p in episode.pages if p.id == proposal.get("page_id")), None)
            if page is None:
                continue
            count = len(proposal.get("panels") or proposal.get("lines") or [])
            label = f"{count} コマ（確からしさ {proposal.get('confidence', 0):.0%}）" if proposal["kind"] == "layout" else f"{count} 本の台詞"
            image = overlay_image(episode, page, max(36, int(max_px / (page.spec.height_mm / 25.4))))
            parts.append(f"<div class='page'><div><h3>{page.index} ページ: {html.escape(label)}</h3>"
                         f"<img class='pg' alt='提案' src='data:image/png;base64,{_png_b64(image)}'></div><div>")
            if proposal["kind"] == "lines":
                parts.append("<ul>" + "".join(f"<li>{html.escape(line['text'])}</li>" for line in proposal.get("lines", [])) + "</ul>")
            pid = proposal["id"]
            parts.append(_cmd(f"genko studio accept {proj} {pid}", "確定（コマンド）")
                         + _cmd(f"genko studio reject {proj} {pid} --note 理由", "却下（コマンド）") + "</div></div>")
    # character sheets
    sheets = episode.studio.get("character_candidates") or {}
    if episode.bible.characters:
        parts.append("<h2>キャラクターの設定画</h2>")
        for char in episode.bible.characters:
            cid = char.get("id")
            refs = {r.get("kind"): r.get("asset") for r in char.get("refs", [])}
            status = "<span class='ok'>承認済み</span>" if char.get("locked") else "<span class='warn'>未承認</span>"
            parts.append(f"<h3>{html.escape(str(char.get('name', cid)))} {status}</h3>")
            if refs.get("sheet"):
                parts += _candidate_tiles(store, [{"id": "sheet", "label": "承認した設定画", "asset": refs["sheet"]}] +
                                          ([{"id": "face", "label": "顔", "asset": refs["face"]}] if refs.get("face") else []), set())
            elif sheets.get(cid):
                parts += _candidate_tiles(store, sheets[cid], set(), lambda c, cid=cid: (
                    f"genko studio approve {proj} sheet --character {cid} --candidate {c['id']}"))
            else:
                parts.append("<p class='muted'>候補はまだありません。</p>")
    locked = (episode.studio.get("style") or {}).get("locked")
    if locked:
        parts.append(f"<p>絵柄は {locked.get('page')} ページで決まりました（画像ツール: {html.escape(str(locked.get('tool') or '記録なし'))}）。</p>")
    parts.append("<h2>ページ</h2>")
    for page in episode.pages:
        record = state.name(episode, page.index)
        draft = {"plan": record["name"]} if record else None
        dpi = max(36, int(max_px / (page.spec.height_mm / 25.4)))
        mode = "name" if not page.name_ok else "proof"
        image = render_page(page, dpi, mode=mode, episode=episode)
        if mode == "name":
            image = annotate(image, page, dpi, draft.get("plan") if draft else None)
        status = "ネーム承認済み" if page.name_ok else ("ネーム確認待ち" if draft else "ネームなし")
        if page.name_ok:
            status += "・作画承認済み" if page.art_ok else "・作画中"
            if page.stage == "finish":
                status += "・仕上げ済み"
        parts.append(f"<div class='page'><div><h3>{page.index} ページ <span class='muted'>{status}・{html.escape(page_side(page.index)['turn'])}</span></h3>")
        parts.append(f"<img class='pg' alt='{page.index} ページ' src='data:image/png;base64,{_png_b64(image)}'></div><div>")
        if draft and not page.name_ok:
            parts.append("<div class='scroll'><table><tr><th>コマ</th><th>構図</th><th>人物</th><th>内容</th><th>台詞</th></tr>")
            for order, panel in enumerate(draft["plan"].get("panels", []), 1):
                who = "、".join(str(names.get(c.get("id"), c.get("id"))) for c in panel.get("characters", []))
                lines = " / ".join("".join(line.get("breaks", [])) for line in panel.get("lines", []))
                parts.append(
                    f"<tr><td>{order}</td><td>{html.escape(_words(panel))}</td>"
                    f"<td>{html.escape(who)}</td><td>{html.escape(panel.get('action', ''))}（{html.escape(panel.get('emotion', ''))}）</td>"
                    f"<td>{html.escape(lines)}</td></tr>"
                )
            parts.append("</table></div>")
            review = state.name_review(episode, page.index)
            if review:
                parts.append(f"<p class='muted'>エージェントの自己点検: {review.get('score')} {html.escape(review.get('notes') or '')}</p>")
            parts.append(_cmd(f"genko studio approve {proj} name --pages {page.index}", "ネームを承認（コマンド）"))
        if page.name_ok:
            for order, frame in enumerate(page.leaf_frames(), 1):
                panel = frame.panel or {}
                cands = panel.get("candidates", [])
                if not cands and panel.get("status") in (None, "empty", "skip"):
                    continue
                adopted = {v for v in (panel.get("adopted") or {}).values() if v}
                parts.append(f"<b>{order} コマ目</b> <span class='muted'>{PANEL.get(panel.get('status'), panel.get('status'))}・"
                             f"取り込んだ絵 {((panel.get('attempts') or {}).get('images', 0))} 枚</span>")
                parts += _candidate_tiles(store, cands, adopted)
            if not page.art_ok:
                parts.append(_cmd(f"genko studio approve {proj} art --pages {page.index}", "作画を承認（コマンド）"))
        if not page.art_ok:
            parts.append(_cmd(f"genko studio comment {proj} --page {page.index} 指示", "直してほしいことを送る（コマンド。コマだけなら --frame コマid）"))
        parts.append("</div></div>")
    parts.append("<h2>書き出し</h2>")
    if report["ok"]:
        parts.append("<p class='ok'>点検は通っています。Genko アプリの「書き出し…」から書き出せます。</p>"
                     + _cmd(f"genko studio export {proj} --format pdf --out 出力先", "正式に書き出す（コマンド）"))
    else:
        parts.append("<p class='warn'>書き出しを止めている理由:</p><ul>"
                     + "".join(f"<li>{html.escape(item['message'])}</li>" for item in report["errors"][:12])
                     + (f"<li>ほか {len(report['errors']) - 12} 件</li>" if len(report["errors"]) > 12 else "") + "</ul>")
    parts.append("</body></html>")
    return "".join(parts)
