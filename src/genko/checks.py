"""入稿前の点検 (layout checks) for every book, with the place of each problem so the editor can show it.

Each issue: {"level": "error" | "warning", "code", "page", "message" (Japanese), "box": [x, y, w, h] (mm,
the place to show), "target": {"kind": line | layer | frame | page, "id"}}. `book(episode)` also folds in
the studio preflight (approvals, adopted art) when the book is a studio project and a folder is given.
"""

from __future__ import annotations

import io

from PIL import Image

from genko.models import LayerKind, LayerRole

MIN_TEXT_MM = 2.4  # smaller dialogue is hard to read in print
MIN_DPI = 350


def _issue(level, code, page, message, box=None, kind="page", target_id=None) -> dict:
    return {"level": level, "code": code, "page": page.index, "message": message,
            "box": [round(float(v), 2) for v in box] if box else None, "target": {"kind": kind, "id": target_id}}


def _overlap(a, b) -> float:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    w = min(ax + aw, bx + bw) - max(ax, bx)
    h = min(ay + ah, by + bh) - max(ay, by)
    return max(0.0, w) * max(0.0, h)


def _text_size_mm(line) -> float | None:
    """The size the lettering ends up at in its balloon (mm), or None when it cannot be measured."""
    from genko import balloons

    try:
        _, em = balloons.text_image(line, 150)
    except Exception:
        return None
    return em / 150 * 25.4


def page_issues(episode, page) -> list[dict]:
    out: list[dict] = []
    t = page.trim_rect_mm()
    trim = (t.x, t.y, t.width, t.height)
    inner = page.inner_rect_mm(getattr(episode, "start_side", None))
    lines = episode.story_for_page(page.index)
    placed = []
    for line in lines:
        text = (line.text or "").replace("\n", "")
        short = text[:14] + ("…" if len(text) > 14 else "")
        if not line.x_mm and not line.y_mm and not line.balloon:
            out.append(_issue("error", "line_unplaced", page, f"台詞「{short}」がどこにも置かれていない", None, "line", line.id))
            continue
        box = (line.x_mm, line.y_mm, line.w_mm or 0, line.h_mm or 0)
        placed.append((line, box))
        x, y, w, h = box
        if x < trim[0] - 0.01 or y < trim[1] - 0.01 or x + w > trim[0] + trim[2] + 0.01 or y + h > trim[1] + trim[3] + 0.01:
            out.append(_issue("error", "text_outside_trim", page, f"台詞「{short}」が仕上がり線の外にはみ出している（裁ち落とされる）",
                              box, "line", line.id))
        elif (line.balloon or "speech") != "sfx" and (x < inner.x - 0.5 or y < inner.y - 0.5 or x + w > inner.x + inner.width + 0.5
                                                     or y + h > inner.y + inner.height + 0.5):
            out.append(_issue("warning", "text_outside_frame", page, f"台詞「{short}」が基本枠の外にある（裁ちずれで切れるおそれ）",
                              box, "line", line.id))
        if (line.balloon or "speech") not in ("sfx", "none") and text:
            size = _text_size_mm(line)
            if size is not None and size < MIN_TEXT_MM:
                out.append(_issue("warning", "text_too_small", page,
                                  f"台詞「{short}」の文字が {size:.1f} mm まで小さくなっている（フキダシを大きくするか、文を短く）",
                                  box, "line", line.id))
    for i, (a, box_a) in enumerate(placed):
        for b, box_b in placed[i + 1:]:
            group_a, group_b = (a.style or {}).get("group"), (b.style or {}).get("group")
            if group_a and group_a == group_b:
                continue
            shared = _overlap(box_a, box_b)
            smaller = min(box_a[2] * box_a[3], box_b[2] * box_b[3]) or 1
            if shared / smaller > 0.15:
                x0, y0 = max(box_a[0], box_b[0]), max(box_a[1], box_b[1])
                x1 = min(box_a[0] + box_a[2], box_b[0] + box_b[2])
                y1 = min(box_a[1] + box_a[3], box_b[1] + box_b[3])
                out.append(_issue("warning", "text_overlap", page, f"台詞「{(a.text or '')[:8]}」と「{(b.text or '')[:8]}」が重なっている",
                                  (x0, y0, x1 - x0, y1 - y0), "line", a.id))
    for layer in page.layers:
        if not layer.visible or not layer.exportable or layer.role in (LayerRole.NAME, LayerRole.DRAFT):
            continue
        label = layer.title or {"ink": "ペン入れ", "bg": "背景", "tone": "トーン"}.get(layer.role.value, "レイヤー")
        for patch in getattr(layer, "patches", None) or []:
            if patch.get("mode") != "image" or not patch.get("png"):
                continue
            with Image.open(io.BytesIO(patch["png"])) as image:
                px_w = image.width
            w_mm = float(patch["box"][2])
            dpi = px_w / (w_mm / 25.4) if w_mm else 0
            if dpi < MIN_DPI:
                out.append(_issue("warning", "low_dpi", page, f"「{label}」に貼った画像の解像度が {dpi:.0f} dpi（{MIN_DPI} 未満。印刷で粗くなる）",
                                  patch["box"], "layer", layer.id))
        if layer.kind == LayerKind.RASTER and layer.raster_png:
            from genko.raster import WORKING_DPI

            if WORKING_DPI < MIN_DPI:
                out.append(_issue("warning", "paint_dpi", page,
                                  f"ペイントのレイヤー「{label}」は {WORKING_DPI} dpi で持っている（細い線はペンのレイヤーで描くと 600 dpi でもきれい）",
                                  None, "layer", layer.id))
        # lines and fills beyond the page's edge are cut off
        xs, ys = [], []
        for stroke in layer.strokes:
            xs += [p[0] for p in stroke.points]
            ys += [p[1] for p in stroke.points]
        for patch in getattr(layer, "patches", None) or []:
            x, y, w, h = (float(v) for v in patch["box"])
            xs += [x, x + w]
            ys += [y, y + h]
        b = page.bleed_rect_mm()
        if xs and (min(xs) < b.x - 1 or min(ys) < b.y - 1 or max(xs) > b.x + b.width + 1 or max(ys) > b.y + b.height + 1):
            out.append(_issue("warning", "art_outside_page", page, f"「{label}」の絵が裁ち落としの外まで出ている（はみ出た所は印刷されない）",
                              (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)), "layer", layer.id))
    if page.spread_with:
        from genko.ops import facing_problem

        partner = next((p for p in episode.pages if p.index == page.spread_with), None)
        problem = facing_problem(episode, page, partner) if partner is not None else "the partner page is missing"
        if problem:
            out.append(_issue("error", "spread_not_facing", page, f"{page.index} ページの見開きの相手（{page.spread_with} ページ）が向かい合わない"))
    art = any(layer.strokes or getattr(layer, "patches", None) or layer.raster_png or layer.kind == LayerKind.PLACED
              for layer in page.layers if layer.exportable and layer.role not in (LayerRole.NAME, LayerRole.DRAFT))
    if not art and not lines and not page.effects:
        out.append(_issue("warning", "empty_page", page, f"{page.index} ページに何も描かれていない"))
    return out


def book(episode, project=None) -> dict:
    """Every page's issues, plus the studio preflight when the book is a studio project with a folder."""
    issues = [issue for page in episode.pages for issue in page_issues(episode, page)]
    if project is not None and (episode.strict_gates or episode.studio):
        from genko.studio import preflight

        try:
            report = preflight.check(episode, project)
        except Exception:
            report = {"errors": [], "warnings": []}
        for level, key in (("error", "errors"), ("warning", "warnings")):
            for item in report.get(key, []):
                where = str(item.get("where") or item.get("path") or "")
                page_no = None
                parts = where.split("/")
                if len(parts) > 2 and parts[1] == "pages" and parts[2].isdigit():
                    page_no = int(parts[2])
                issues.append({"level": level, "code": item.get("code"), "page": page_no, "message": item.get("message"),
                               "box": None, "target": {"kind": "page", "id": None}})
    return {"ok": not any(i["level"] == "error" for i in issues), "issues": issues,
            "errors": sum(1 for i in issues if i["level"] == "error"),
            "warnings": sum(1 for i in issues if i["level"] == "warning")}
