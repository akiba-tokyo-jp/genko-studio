"""Hand-drawn names (atari): import, analysis, proposals and their overlay.

- import: the scan becomes an immutable asset (origin "self"), is placed on the
  page's DRAFT layer (never printed) and aligned to the page in mm.
- analysis: XY-cut finds the panels; every result is written to
  studio/analysis/<page id>/<scan>.json with an overlay image, and becomes a
  layout proposal. Nothing on the page changes until a person accepts it.
- lines: the agent reads the handwriting and proposes lines (text + position);
  also only a proposal until accepted.
"""

from __future__ import annotations

import io
import json
from pathlib import Path

from PIL import Image, ImageDraw

from genko.assets import AssetStore
from genko.models import Episode, Page
from genko.studio import xycut
from genko.studio.jsonutil import atomic_write_text, content_hash

ALIGNS = ("auto", "page", "live")


def placement_for(image: Image.Image, page: Page, align: str = "auto") -> tuple[list[float], str]:
    """Where the scan sits on the page (x, y, w, h in mm) and the alignment actually used.

    page: the scan is the whole sheet. live: the scan's drawing fills the live area.
    auto: the drawing's box goes onto the live area when it is clearly a page of panels,
    else the scan is taken as the whole sheet.
    """
    W, H = page.spec.width_mm, page.spec.height_mm
    if align == "page":
        return [0.0, 0.0, W, H], "page"
    box = xycut.content_box(image, dpi_guess=image.width / (W / 25.4))
    if box is None or ((box[2] - box[0]) * (box[3] - box[1]) < 0.4 * image.width * image.height and align == "auto"):
        return [0.0, 0.0, W, H], "page"
    live = page.inner_rect_mm()
    bw, bh = box[2] - box[0], box[3] - box[1]
    sx, sy = live.width / bw, live.height / bh
    if align == "auto" and abs(sx / sy - 1) > 0.08:
        return [0.0, 0.0, W, H], "page"  # the drawing does not have the live area's shape
    s = (sx + sy) / 2
    x = live.x + live.width / 2 - (box[0] + bw / 2) * s
    y = live.y + live.height / 2 - (box[1] + bh / 2) * s
    return [round(x, 2), round(y, 2), round(image.width * s, 2), round(image.height * s, 2)], "live"


def atari_of(page: Page) -> dict | None:
    return (page.plan or {}).get("atari")


def analysis_dir(project: Path, page: Page) -> Path:
    return Path(project) / "studio" / "analysis" / page.id


def analyze(project: Path, episode: Episode, page: Page, params: dict | None = None) -> dict:
    """Detect the panels of the page's scan. Writes the analysis files; returns the layout proposal."""
    atari = atari_of(page)
    if not atari:
        raise ValueError(f"{page.index} ページにアタリが無い（先に import_name）")
    data = AssetStore(project).get_bytes(atari["asset"], ".png")
    if data is None:
        raise ValueError(f"アタリの画像 {atari['asset'][:19]}… が assets/ に無い")
    image = Image.open(io.BytesIO(data))
    settings = xycut.Params(**{k: v for k, v in (params or {}).items() if k in xycut.Params.__dataclass_fields__})
    from genko.models import Binding

    found = xycut.detect(image, tuple(atari["placement_mm"]), settings, right_to_left=episode.binding == Binding.RIGHT)
    record = {
        "asset": atari["asset"],
        "placement_mm": atari["placement_mm"],
        "align": atari.get("align"),
        "params": settings.__dict__,
        "threshold": found.threshold,
        "confidence": found.confidence,
        "panels": found.leaves_mm,
        "tree": found.tree,
        "tiers": found.tiers,
    }
    stem = atari["asset"][7:19] + "_" + content_hash(settings.__dict__)[7:13]
    folder = analysis_dir(project, page)
    folder.mkdir(parents=True, exist_ok=True)
    atomic_write_text(folder / f"{stem}.json", json.dumps(record, ensure_ascii=False, indent=2) + "\n")
    overlay_image(episode, page, 72, {"kind": "layout", "panels": found.leaves_mm}).save(folder / f"{stem}.png")
    return {
        "id": "pr_" + content_hash({"page": page.id, "layout": found.tree})[7:17],
        "kind": "layout",
        "page": page.index,
        "source": "genko:xycut",
        "analysis": str((folder / f"{stem}.json").relative_to(project)),
        "confidence": found.confidence,
        "panels": found.leaves_mm,
        "tree": found.tree,
        "tiers": found.tiers,
    }


def accept_ops(episode: Episode, proposal: dict, *, force: bool = False) -> list[dict]:
    """The ops a person's acceptance applies (the proposal is marked accepted in the same batch)."""
    page = next((p for p in episode.pages if p.id == proposal.get("page_id")), None)
    if page is None:
        raise ValueError("提案のページがもう無い")
    ops: list[dict] = []
    if proposal["kind"] == "layout":
        if not proposal.get("tree"):
            raise ValueError("コマが検出されていない提案は確定できない")
        ops.append({"op": "set_layout", "page": page.index, "tree": proposal["tree"], "force": force})
    else:
        for line in proposal.get("lines", []):
            op = {"op": "add_line", "page": page.index, "text": line["text"], "speaker": line.get("speaker") or "",
                  "balloon": line.get("balloon") or "speech", "wrap": line.get("wrap") or "vertical",
                  "x_mm": line["x_mm"], "y_mm": line["y_mm"], "w_mm": line["w_mm"], "h_mm": line["h_mm"]}
            if line.get("tail"):
                op["tail"] = line["tail"]
            ops.append(op)
    ops.append({"op": "resolve_proposal", "id": proposal["id"], "status": "accepted"})
    return ops


def assign_frames(ops: list[dict], page: Page) -> list[dict]:
    """Give each proposed line the panel its centre falls in (after the layout exists)."""
    out = []
    for op in ops:
        if op.get("op") == "add_line" and not op.get("frame_id"):
            frame = page.frame_at(op["x_mm"] + op["w_mm"] / 2, op["y_mm"] + op["h_mm"] / 2)
            if frame is not None and not frame.children:
                op = {**op, "frame_id": frame.id}
        out.append(op)
    return out


def line_from_input(item: dict, page: Page, atari: dict | None) -> dict:
    """Validate one proposed line: text, and a position in page mm (rect) or in the scan (box01)."""
    text = str(item.get("text") or "").strip()
    if not text:
        raise ValueError("text が空")
    balloon = str(item.get("balloon") or "speech")
    if item.get("box01"):
        if not atari:
            raise ValueError("box01 はアタリの画像の中の位置。アタリが無いページでは x_mm, y_mm を使う")
        bx, by, bw, bh = (float(v) for v in item["box01"])
        px, py, pw, ph = atari["placement_mm"]
        x, y, w, h = px + bx * pw, py + by * ph, bw * pw, bh * ph
    elif item.get("x_mm") is not None and item.get("y_mm") is not None:
        x, y = float(item["x_mm"]), float(item["y_mm"])
        w, h = item.get("w_mm"), item.get("h_mm")
        if not w or not h:
            from genko.studio.letter import measure

            w, h = measure(text.split("\n"), balloon)
        w, h = float(w), float(h)
    else:
        raise ValueError("位置（x_mm, y_mm か box01）が要る")
    if not (0 <= x < page.spec.width_mm and 0 <= y < page.spec.height_mm):
        raise ValueError("位置がページの外")
    line = {"text": text, "balloon": balloon, "x_mm": round(x, 2), "y_mm": round(y, 2), "w_mm": round(w, 2), "h_mm": round(h, 2),
            "speaker": item.get("speaker"), "wrap": item.get("wrap") or "vertical"}
    if item.get("tail"):
        line["tail"] = [float(v) for v in item["tail"]]
    return line


def overlay_image(episode: Episode, page: Page, dpi: int, extra: dict | None = None) -> Image.Image:
    """The page with its scan (DRAFT) and the open proposals drawn over it: panels in red with their
    confidence, proposed lines in blue. Nothing here is on the page yet."""
    from genko.render import _font, mm_to_px, render_page

    image = render_page(page, dpi, mode="name", episode=episode).convert("RGB")
    draw = ImageDraw.Draw(image)
    font = _font(None, max(10, mm_to_px(3.5, dpi)))
    proposals = [p for p in (episode.studio.get("proposals") or {}).values() if p.get("page_id") == page.id and p.get("status") == "open"]
    if extra:
        proposals = proposals + [extra]
    width = max(2, mm_to_px(0.6, dpi))
    for proposal in proposals:
        if proposal.get("kind") == "layout":
            for i, panel in enumerate(proposal.get("panels", []), start=1):
                x, y, w, h = panel["rect_mm"]
                box = [mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi)]
                draw.rectangle(box, outline=(220, 30, 30), width=width)
                draw.text((box[0] + 4, box[1] + 4), f"{i} ({panel.get('confidence', 0):.2f})", fill=(220, 30, 30), font=font)
        else:
            for line in proposal.get("lines", []):
                box = [mm_to_px(line["x_mm"], dpi), mm_to_px(line["y_mm"], dpi),
                       mm_to_px(line["x_mm"] + line["w_mm"], dpi), mm_to_px(line["y_mm"] + line["h_mm"], dpi)]
                draw.rectangle(box, outline=(30, 90, 220), width=width)
                draw.text((box[0] + 2, box[3] + 2), line["text"].replace("\n", " ")[:16], fill=(30, 90, 220), font=font)
    return image
