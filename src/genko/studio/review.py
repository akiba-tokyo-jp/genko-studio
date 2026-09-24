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


CSS = (
    "body{font-family:sans-serif;margin:16px;max-width:1200px;color:#222}"
    ".page{display:flex;gap:16px;border-top:1px solid #ccc;padding:12px 0;flex-wrap:wrap}"
    ".page img.pg{max-width:min(100%,520px);border:1px solid #999}table{border-collapse:collapse;font-size:13px}"
    "td,th{border:1px solid #ddd;padding:3px 6px;vertical-align:top}code{background:#f3f3f3;padding:2px 4px;font-size:12px}"
    ".cands{display:flex;gap:8px;flex-wrap:wrap;margin:4px 0 12px}.cand{border:2px solid #ddd;padding:4px;font-size:11px;width:168px}"
    ".cand.adopted{border-color:#2a7}.cand.rejected{opacity:.5}.cand img{display:block;max-width:160px;max-height:160px}"
    ".warn{color:#a40}.ok{color:#2a7}"
)


def _candidate_tiles(store, cands: list[dict], adopted: set[str], command=None) -> list[str]:
    parts = ["<div class='cands'>"]
    for cand in cands:
        b64 = _thumb_b64(store, cand.get("asset", ""))
        review = cand.get("review") or {}
        cls = "cand" + (" adopted" if cand["id"] in adopted else "") + (" rejected" if cand.get("status") == "rejected" else "")
        origin = cand.get("origin") or {}
        lines = [f"<b>{html.escape(cand['id'])}</b>" + (" ✔採用" if cand["id"] in adopted else "")]
        if review:
            lines.append(f"評価 {review.get('score')}: {html.escape(str(review.get('note') or ''))}")
        if cand.get("stale"):
            lines.append("<span class='warn'>指示が変わった後の候補</span>")
        lines.append(html.escape(" / ".join(str(origin.get(k)) for k in ("tool_id", "model") if origin.get(k))))
        if origin.get("kind") == "fixture":
            lines.append("<span class='warn'>試験用の画像</span>")
        if command:
            lines.append(_code(command(cand)))
        img = f"<img alt='{html.escape(cand['id'])}' src='data:image/png;base64,{b64}'>" if b64 else "(画像なし)"
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
    parts = [
        "<!doctype html><html lang='ja'><meta charset='utf-8'>",
        f"<title>{html.escape(episode.title)} 確認</title><style>{CSS}</style>",
        f"<h1>{html.escape(episode.title)}</h1><p>版 r{episode.revision}。承認はここに書いたコマンドで人間が行う（エージェントは承認できない）。</p>",
    ]
    if requests:
        parts.append("<h2>承認依頼・指示・相談</h2><table><tr><th>種類</th><th>内容</th><th>依頼者</th><th>操作</th></tr>")
        for req in requests:
            kind = req.get("kind")
            if kind == "gate":
                gate = req.get("gate")
                what = f"{gate} の承認依頼（{req.get('pages') or req.get('character_id') or ''}）"
                if gate in ("name", "art"):
                    action = f"genko studio approve {proj} {gate} --pages {','.join(map(str, req.get('pages') or []))} --as human:名前"
                elif gate == "sheet":
                    action = f"genko studio approve {proj} sheet --character {req.get('character_id')} --candidate 候補id --as human:名前"
                elif gate == "export":
                    action = f"genko studio export {proj} --format pdf --out 出力先 --as human:名前"
                else:
                    action = ""
            elif kind == "help":
                what = f"相談（{req.get('page_index') or ''} {req.get('frame_id') or ''}）"
                action = f"genko studio close-ticket {proj} {req['id']} --as human:名前"
            else:
                what = f"{req.get('page_index')} ページ{' ' + req['frame_id'] if req.get('frame_id') else ''}への指示"
                action = ""
            parts.append(f"<tr><td>{html.escape(what)}</td><td>{html.escape(req.get('text') or '')}</td>"
                         f"<td>{html.escape(req.get('created_by') or '')}</td><td>{_code(action) if action else ''}</td></tr>")
        parts.append("</table>")
    proposals = [p for p in (episode.studio.get("proposals") or {}).values() if p.get("status") == "open"]
    if proposals:
        from genko.studio.atari import overlay_image

        parts.append("<h2>アタリからの提案（確定するまでページは変わらない）</h2>")
        for proposal in proposals:
            page = next((p for p in episode.pages if p.id == proposal.get("page_id")), None)
            if page is None:
                continue
            count = len(proposal.get("panels") or proposal.get("lines") or [])
            label = f"{count} コマ（信頼度 {proposal.get('confidence', 0):.2f}）" if proposal["kind"] == "layout" else f"{count} 本の台詞"
            image = overlay_image(episode, page, max(36, int(max_px / (page.spec.height_mm / 25.4))))
            parts.append(f"<div class='page'><div><h3>{page.index} ページ: {html.escape(label)}（{html.escape(str(proposal.get('source') or ''))}）</h3>"
                         f"<img class='pg' alt='proposal' src='data:image/png;base64,{_png_b64(image)}'></div><div>")
            if proposal["kind"] == "lines":
                parts.append("<ul>" + "".join(f"<li>{html.escape(line['text'])}</li>" for line in proposal.get("lines", [])) + "</ul>")
            pid = proposal["id"]
            parts.append(f"<p>確定: {_code(f'genko studio accept {proj} {pid} --as human:名前')}</p>"
                         f"<p>却下: {_code(f'genko studio reject {proj} {pid} --note 理由 --as human:名前')}</p></div></div>")
    # character sheets
    sheets = episode.studio.get("character_candidates") or {}
    if episode.bible.characters:
        parts.append("<h2>キャラクター設定画</h2>")
        for char in episode.bible.characters:
            cid = char.get("id")
            refs = {r.get("kind"): r.get("asset") for r in char.get("refs", [])}
            status = "<span class='ok'>承認済み</span>" if char.get("locked") else "未承認"
            parts.append(f"<h3>{html.escape(str(char.get('name', cid)))}（{html.escape(str(cid))}）{status}</h3>")
            if refs.get("sheet"):
                parts += _candidate_tiles(store, [{"id": "承認した設定画", "asset": refs["sheet"]}] +
                                          ([{"id": "顔", "asset": refs["face"]}] if refs.get("face") else []), set())
            elif sheets.get(cid):
                parts += _candidate_tiles(store, sheets[cid], set(), lambda c, cid=cid: (
                    f"genko studio approve {proj} sheet --character {cid} --candidate {c['id']} --as human:名前"))
    locked = (episode.studio.get("style") or {}).get("locked")
    if locked:
        parts.append(f"<p>スタイルは {locked.get('page')} ページで固定（画像ツール: {html.escape(str(locked.get('tool') or '記録なし'))}）。</p>")
    for page in episode.pages:
        record = state.name(episode, page.index)
        draft = {"plan": record["name"]} if record else None
        dpi = max(36, int(max_px / (page.spec.height_mm / 25.4)))
        mode = "name" if not page.name_ok else "proof"
        image = render_page(page, dpi, mode=mode, episode=episode)
        if mode == "name":
            image = annotate(image, page, dpi, draft.get("plan") if draft else None)
        status = "承認済み" if page.name_ok else ("未承認" if draft else "ネームなし")
        if page.name_ok:
            status += "・作画承認済み" if page.art_ok else "・作画中"
            if page.stage == "finish":
                status += "・仕上げ済み"
        parts.append(f"<div class='page'><div><h3>{page.index} ページ（ネーム{status}・{html.escape(page_side(page.index)['turn'])}）</h3>")
        parts.append(f"<img class='pg' alt='page {page.index}' src='data:image/png;base64,{_png_b64(image)}'></div><div>")
        if draft and not page.name_ok:
            parts.append("<table><tr><th>コマ</th><th>shot</th><th>人物</th><th>内容</th><th>台詞</th></tr>")
            for panel in draft["plan"].get("panels", []):
                who = ", ".join(f"{c.get('id')}({c.get('pos')})" for c in panel.get("characters", []))
                lines = " / ".join("".join(line.get("breaks", [])) for line in panel.get("lines", []))
                parts.append(
                    f"<tr><td>{html.escape(panel.get('slot', ''))}</td><td>{panel.get('shot')}/{panel.get('angle')}</td>"
                    f"<td>{html.escape(who)}</td><td>{html.escape(panel.get('action', ''))}（{html.escape(panel.get('emotion', ''))}）</td>"
                    f"<td>{html.escape(lines)}</td></tr>"
                )
            parts.append("</table>")
            review = state.name_review(episode, page.index)
            if review:
                parts.append(f"<p>自己点検（{html.escape(str(review.get('by')))}）: {review.get('score')} {html.escape(review.get('notes') or '')}</p>")
            parts.append(f"<p>承認: {_code(f'genko studio approve {proj} name --pages {page.index} --as human:名前')}</p>")
        if page.name_ok:
            for frame in page.leaf_frames():
                panel = frame.panel or {}
                cands = panel.get("candidates", [])
                if not cands and panel.get("status") in (None, "empty", "skip"):
                    continue
                adopted = {v for v in (panel.get("adopted") or {}).values() if v}
                label = panel.get("slot") or frame.id
                parts.append(f"<b>コマ {html.escape(str(label))}</b>（{html.escape(frame.id)}、{html.escape(str(panel.get('status')))}、"
                             f"取り込み {((panel.get('attempts') or {}).get('images', 0))} 枚）")
                parts += _candidate_tiles(store, cands, adopted)
            if not page.art_ok:
                parts.append(f"<p>作画の承認: {_code(f'genko studio approve {proj} art --pages {page.index} --as human:名前')}</p>")
        if not page.art_ok:
            parts.append(f"<p>直してほしいとき: {_code(f'genko studio comment {proj} --page {page.index} 指示 --as human:名前')}"
                         f"（コマだけなら --frame コマid）</p>")
        parts.append("</div></div>")
    parts.append("<h2>書き出し</h2>")
    if report["ok"]:
        parts.append(f"<p class='ok'>preflight は通っている。{_code(f'genko studio export {proj} --format pdf --out 出力先 --as human:名前')}</p>")
    else:
        codes: dict[str, int] = {}
        for item in report["errors"]:
            codes[item["code"]] = codes.get(item["code"], 0) + 1
        parts.append("<p class='warn'>preflight で止まる理由: " + html.escape(", ".join(f"{k} ×{v}" for k, v in sorted(codes.items()))) + "</p>")
    parts.append("</html>")
    return "".join(parts)
