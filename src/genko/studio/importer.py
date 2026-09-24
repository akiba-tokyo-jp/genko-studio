"""Bring generated images in as candidates of a generation request.

Accepted sources: a file inside <project>/studio/inbox/ (same machine), or an
asset already uploaded to <project>/assets/ (POST /v1/assets). Nothing else is
read. Each image is checked (a real image, size limits), stored as PNG, and
measured (aspect match, how busy the balloon areas are, brightness, how well
the edges follow the pose guide) so weak candidates sort last before anyone
looks at them. Importing the same image twice adds nothing.
"""

from __future__ import annotations

import io
import math
from pathlib import Path

from PIL import Image, ImageFilter

from genko.assets import AssetStore
from genko.studio.jsonutil import sha256_hex
from genko.studio.studio_ops import ORIGIN_KINDS

MAX_BYTES = 30 * 1024 * 1024
MAX_PIXELS = 64_000_000
MAX_IMAGES = 16
THUMB = 256


class ImportError_(ValueError):
    def __init__(self, message: str, path: str = "/") -> None:
        super().__init__(message)
        self.path = path


def inbox(project: Path) -> Path:
    return Path(project) / "studio" / "inbox"


def read_source(project: Path, item: dict, index: int) -> bytes:
    if not isinstance(item, dict):
        raise ImportError_("画像は {file} か {asset} のオブジェクト", f"/images/{index}")
    if bool(item.get("file")) == bool(item.get("asset")):
        raise ImportError_("file か asset のどちらか一方を渡す", f"/images/{index}")
    if item.get("file"):
        root = inbox(project).resolve()
        raw = Path(str(item["file"]))
        path = (raw if raw.is_absolute() else Path(project) / raw).resolve()
        if root not in path.parents:
            raise ImportError_(f"file は studio/inbox/ の中だけ: {item['file']}", f"/images/{index}/file")
        if not path.is_file():
            raise ImportError_(f"ファイルがない: {item['file']}", f"/images/{index}/file")
        if path.stat().st_size > MAX_BYTES:
            raise ImportError_("画像が大きすぎる（30 MB まで）", f"/images/{index}/file")
        return path.read_bytes()
    ref = str(item["asset"])
    data = AssetStore(project).get_bytes(ref, ".png") if ref.startswith("sha256:") else None
    if data is None:
        raise ImportError_(f"asset {ref} が assets/ にない（先にアップロードする）", f"/images/{index}/asset")
    return data


def normalize(blob: bytes, index: int = 0) -> tuple[bytes, tuple[int, int], Image.Image]:
    """Check the bytes are an image within limits; return PNG bytes, size and the decoded image."""
    if len(blob) > MAX_BYTES:
        raise ImportError_("画像が大きすぎる（30 MB まで）", f"/images/{index}")
    try:
        with Image.open(io.BytesIO(blob)) as probe:
            size = probe.size
            fmt = probe.format
            probe.verify()
    except Exception as exc:  # Pillow raises many types for bad data
        raise ImportError_(f"画像として読めない: {exc}", f"/images/{index}") from exc
    if size[0] * size[1] > MAX_PIXELS:
        raise ImportError_(f"画素数が多すぎる: {size[0]}x{size[1]}（64 MP まで）", f"/images/{index}")
    image = Image.open(io.BytesIO(blob))
    image.load()
    if fmt != "PNG":
        buf = io.BytesIO()
        (image if image.mode in ("RGB", "RGBA", "L", "LA") else image.convert("RGB")).save(buf, format="PNG")
        blob = buf.getvalue()
    return blob, size, image


def check_origin(origin, index: int) -> dict:
    if not isinstance(origin, dict):
        raise ImportError_("origin（来歴）が要る: {kind, tool_id, model, prompt, …}", f"/images/{index}/origin")
    out = {k: v for k, v in origin.items() if k in ("kind", "tool_id", "tool", "model", "prompt", "params", "refs_used", "note")}
    out.setdefault("kind", "agent")
    if out["kind"] not in ORIGIN_KINDS:
        raise ImportError_(f"origin.kind は {', '.join(ORIGIN_KINDS)} のどれか", f"/images/{index}/origin/kind")
    out["reported"] = out["kind"] == "agent"
    return out


def _aspect(request: dict) -> float | None:
    size = request.get("size") or {}
    px = size.get("suggested_px")
    if size.get("frame_mm"):
        w, h = size["frame_mm"]
        pad = float(size.get("pad_mm") or 0)
        return (w + 2 * pad) / (h + 2 * pad)
    if px:
        return px[0] / px[1]
    return None


def _cover(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    from genko.guide import fit_cover

    return fit_cover(image, size)


def _density(edges: Image.Image, boxes01: list[list[float]]) -> float:
    w, h = edges.size
    total = count = 0
    for x, y, bw, bh in boxes01:
        box = (int(x * w), int(y * h), max(int(x * w) + 1, int((x + bw) * w)), max(int(y * h) + 1, int((y + bh) * h)))
        region = edges.crop(box)
        hist = region.histogram()
        count += sum(hist[64:])
        total += region.width * region.height
    return count / total if total else 0.0


def metrics(image: Image.Image, request: dict) -> dict:
    """Deterministic hints only; the agent and the person decide."""
    aspect = _aspect(request)
    w, h = image.size
    out: dict = {"px": [w, h]}
    if aspect:
        out["aspect_error"] = round(abs(math.log((w / h) / aspect)), 3)
    ratio = aspect or (w / h)
    size = (THUMB, max(1, round(THUMB / ratio))) if ratio >= 1 else (max(1, round(THUMB * ratio)), THUMB)
    small = _cover(image.convert("L"), size)
    hist = small.histogram()
    out["luminance"] = round(sum(i * n for i, n in enumerate(hist)) / (255 * size[0] * size[1]), 3)
    edges = small.filter(ImageFilter.FIND_EDGES)
    overall = _density(edges, [[0, 0, 1, 1]])
    out["edge_density"] = round(overall, 4)
    keep = [k["box01"] for k in request.get("keepout") or []]
    if keep:
        out["keepout_busy"] = round(_density(edges, keep) / overall, 3) if overall else 0.0
    bodies = [f["body01"] for f in request.get("figures") or []]
    if bodies and overall:
        out["guide_follow"] = round(min(3.0, _density(edges, bodies) / overall) / 3.0, 3)
    return out


def rank_key(metrics_: dict) -> float:
    """Lower is better: wrong shape, busy balloon areas and ignoring the pose guide cost."""
    return round(
        metrics_.get("aspect_error", 0) * 2
        + max(0.0, metrics_.get("keepout_busy", 1.0) - 1.0)
        - metrics_.get("guide_follow", 0.33),
        4,
    )


def candidate_id(request_id: str, asset: str) -> str:
    return "cd_" + sha256_hex(f"{request_id}:{asset}")[:10]


def prepare(project: Path, request: dict, images: list) -> tuple[list[dict], Image.Image | None]:
    """Validate, store and measure; returns import_candidates items (not yet in project.json)."""
    if not isinstance(images, list) or not images:
        raise ImportError_("images は 1 枚以上の配列", "/images")
    if len(images) > MAX_IMAGES:
        raise ImportError_(f"1 回に取り込めるのは {MAX_IMAGES} 枚まで", "/images")
    store = AssetStore(project)
    items: list[dict] = []
    first: Image.Image | None = None
    for i, item in enumerate(images):
        origin = check_origin(item.get("origin") if isinstance(item, dict) else None, i)
        blob, size, image = normalize(read_source(project, item, i), i)
        ref = store.put_bytes(blob, ".png")
        measured = metrics(image, request)
        items.append({
            "id": candidate_id(request["id"], ref),
            "asset": ref,
            "px": list(size),
            "mode": request.get("mode", "new"),
            "parent": request.get("parent"),
            "origin": origin,
            "metrics": {**measured, "rank": rank_key(measured)},
        })
        if first is None:
            first = image
    return items, first
