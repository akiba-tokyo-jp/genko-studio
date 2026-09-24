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


def review_html(project: Path, *, max_px: int = 900) -> str:
    episode = load_episode(project)
    requests = state.open_tickets(episode)
    parts = [
        "<!doctype html><html lang='ja'><meta charset='utf-8'>",
        f"<title>{html.escape(episode.title)} ネーム確認</title>",
        "<style>body{font-family:sans-serif;margin:16px;max-width:1100px}"
        ".page{display:flex;gap:16px;border-top:1px solid #ccc;padding:12px 0;flex-wrap:wrap}"
        ".page img{max-width:min(100%,520px);border:1px solid #999}table{border-collapse:collapse;font-size:13px}"
        "td,th{border:1px solid #ddd;padding:3px 6px;vertical-align:top}code{background:#f3f3f3;padding:2px 4px}</style>",
        f"<h1>{html.escape(episode.title)}</h1>",
    ]
    if requests:
        parts.append("<h2>承認・指示</h2><ul>")
        for req in requests:
            if req.get("kind") == "gate":
                what = f"{req.get('gate')} の承認依頼（{req.get('pages') or req.get('character_id') or ''}）"
            else:
                what = f"{req.get('page_index')} ページ{' ' + req['frame_id'] if req.get('frame_id') else ''}への指示"
            parts.append(f"<li>{html.escape(what)}: {html.escape(req.get('text') or '')}（{html.escape(req.get('created_by') or '')}）</li>")
        parts.append("</ul>")
    for page in episode.pages:
        record = state.name(episode, page.index)
        draft = {"plan": record["name"]} if record else None
        dpi = max(36, int(max_px / (page.spec.height_mm / 25.4)))
        image = annotate(render_page(page, dpi, mode="name", episode=episode), page, dpi, draft.get("plan") if draft else None)
        status = "承認済み" if page.name_ok else ("未承認" if draft else "ネームなし")
        if page.name_ok:
            status += "・作画承認済み" if page.art_ok else "・作画中"
        parts.append(f"<div class='page'><div><h3>{page.index} ページ（{status}・{html.escape(page_side(page.index)['turn'])}）</h3>")
        parts.append(f"<img alt='page {page.index}' src='data:image/png;base64,{_png_b64(image)}'></div><div>")
        if draft:
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
            if not page.name_ok:
                parts.append(f"<p>承認: <code>genko studio approve {html.escape(str(project))} name --pages {page.index} --as human:名前</code></p>")
            elif not page.art_ok:
                parts.append(f"<p>作画の承認: <code>genko studio approve {html.escape(str(project))} art --pages {page.index} --as human:名前</code></p>")
            if not page.art_ok:
                parts.append(f"<p>直してほしいとき: <code>genko studio comment {html.escape(str(project))} --page {page.index} \"指示\" --as human:名前</code></p>")
        parts.append("</div></div>")
    parts.append("</html>")
    return "".join(parts)
