"""Animation (アニメーション): a page can be a short animation. Its timeline lives in page.extra["anim"]:

    {"fps": 12, "frames": 24, "loop": true,
     "tracks": [{"folder": <animation folder layer id>, "cels": [[frame, cel layer id | null], …]}],
     "camera": [{"frame": 1, "rect": [x, y, w, h] mm}, …],        (カメラワーク: the view moves between keys)
     "light_table": [cel ids]}                                     (ライトテーブル: always shown faint)

An animation folder holds cels (セル: ordinary pen or paint layers). A track's cels list is its exposure sheet
(タイムシート): from each frame on, that cel shows (null: nothing) until the next entry. Layers outside the
animation folders (backgrounds, the panel) show in every frame. Frames count from 1.
"""

from __future__ import annotations

import copy

from PIL import Image

MAX_FRAMES = 3000
FORMATS = ("gif", "webp", "png", "mp4", "frames")


def spec(page) -> dict | None:
    data = (getattr(page, "extra", None) or {}).get("anim")
    return data if isinstance(data, dict) else None


def is_animation(page) -> bool:
    return spec(page) is not None


def frames_of(page) -> int:
    data = spec(page) or {}
    return max(1, int(data.get("frames") or 1))


def fps_of(page) -> float:
    return float((spec(page) or {}).get("fps") or 12)


def track(page, folder_id: str) -> dict | None:
    return next((t for t in (spec(page) or {}).get("tracks", []) if t.get("folder") == folder_id), None)


def cels_of(page, folder_id: str) -> list:
    return [layer for layer in page.layers if layer.parent_id == folder_id]


def cel_at(page, folder_id: str, frame: int) -> str | None:
    """The cel shown in this folder at this frame (None: nothing)."""
    entry = track(page, folder_id)
    shown = None
    for at, cel in sorted(entry.get("cels", []) if entry else [], key=lambda item: item[0]):
        if at > frame:
            break
        shown = cel
    return shown


def at_frame(page, frame: int):
    """The page as it looks at this frame: in each animation folder only the exposed cel shows. (A copy: the
    book is never changed.)"""
    data = spec(page)
    if data is None:
        return page
    hide: set[str] = set()
    for entry in data.get("tracks", []):
        folder = entry.get("folder")
        shown = cel_at(page, folder, frame)
        hide |= {layer.id for layer in cels_of(page, folder) if layer.id != shown}
    out = copy.copy(page)
    out._at_frame = True  # (render_page shows it as it is)
    layers = []
    for layer in page.layers:
        if layer.id in hide and layer.visible:
            layer = copy.copy(layer)
            layer.visible = False
        layers.append(layer)
    out.layers = layers
    return out


def camera_at(page, frame: int) -> list[float] | None:
    """The camera's rect (mm) at this frame, moving evenly between its keys (None: no camera work)."""
    keys = sorted((spec(page) or {}).get("camera") or [], key=lambda k: k["frame"])
    if not keys:
        return None
    if frame <= keys[0]["frame"]:
        return list(keys[0]["rect"])
    for a, b in zip(keys, keys[1:]):
        if a["frame"] <= frame <= b["frame"]:
            t = (frame - a["frame"]) / max(1, b["frame"] - a["frame"])
            return [pa + (pb - pa) * t for pa, pb in zip(a["rect"], b["rect"])]
    return list(keys[-1]["rect"])


def cel_image(page, layer, dpi: int) -> Image.Image:
    """One cel alone on a clear page (for the onion skin and the light table)."""
    from genko import render

    size = (render.mm_to_px(page.spec.width_mm, dpi), render.mm_to_px(page.spec.height_mm, dpi))
    raster = render._open_raster(layer)
    if raster is not None:
        raster = raster.resize(size)
    lines = render._layer_strokes(layer, size, dpi, None, raster)
    if lines is not None:
        raster = lines if raster is None else Image.alpha_composite(raster.convert("RGBA"), lines)
    return raster.convert("RGBA") if raster is not None else Image.new("RGBA", size, (0, 0, 0, 0))


def _tinted(image: Image.Image, rgb: tuple[int, int, int], strength: float) -> Image.Image:
    alpha = image.getchannel("A").point(lambda v: int(v * strength))
    out = Image.new("RGBA", image.size, (*rgb, 0))
    out.putalpha(alpha)
    return out


BEFORE_RGB = (220, 60, 60)  # (the frames before in red, after in blue: as CLIP STUDIO PAINT shows them)
AFTER_RGB = (50, 110, 230)
LIGHT_RGB = (120, 170, 120)


def _neighbours(page, folder: str, frame: int, direction: int, how_many: int, loop: bool) -> list[str]:
    """The cels shown before (or after) this frame on a track, nearest first, skipping the one shown now."""
    count = frames_of(page)
    now = cel_at(page, folder, frame)
    out: list[str] = []
    f = frame
    for _ in range(count - 1):
        f += direction
        if not 1 <= f <= count:
            if not loop:
                break
            f = (f - 1) % count + 1
        cel = cel_at(page, folder, f)
        if cel and cel != now and cel not in out:
            out.append(cel)
            if len(out) >= how_many:
                break
    return out


def onion(page, frame: int, dpi: int, before: int = 1, after: int = 1, strength: float = 0.35) -> Image.Image | None:
    """The cels before and after the one shown, faint and tinted (オニオンスキン: before in red, after in blue),
    and the light table's cels (ライトテーブル)."""
    data = spec(page)
    if data is None:
        return None
    from genko import render

    size = (render.mm_to_px(page.spec.width_mm, dpi), render.mm_to_px(page.spec.height_mm, dpi))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    loop = bool(data.get("loop", True))
    shown_now = {cel_at(page, t.get("folder"), frame) for t in data.get("tracks", [])}
    layers = {layer.id: layer for layer in page.layers}
    used = False
    for entry in data.get("tracks", []):
        folder = entry.get("folder")
        for direction, rgb, how_many in ((-1, BEFORE_RGB, before), (1, AFTER_RGB, after)):
            if how_many <= 0:
                continue
            for step, cel in enumerate(_neighbours(page, folder, frame, direction, how_many, loop), start=1):
                if cel in layers:
                    out = Image.alpha_composite(out, _tinted(cel_image(page, layers[cel], dpi), rgb, strength / step))
                    used = True
    for cel in data.get("light_table") or []:
        if cel in layers and cel not in shown_now:
            out = Image.alpha_composite(out, _tinted(cel_image(page, layers[cel], dpi), LIGHT_RGB, strength))
            used = True
    return out if used else None


def with_onion(image: Image.Image, page, frame: int, dpi: int, **kwargs) -> Image.Image:
    ghost = onion(page, frame, dpi, **kwargs)
    if ghost is None:
        return image
    base = image.convert("RGBA")
    if ghost.size != base.size:
        ghost = ghost.resize(base.size)
    return Image.alpha_composite(base, ghost).convert(image.mode if image.mode in ("RGB", "RGBA") else "RGB")


def render_frame(page, frame: int, dpi: int, episode=None, *, camera: bool = True, area: str = "trim",
                 size: tuple[int, int] | None = None) -> Image.Image:
    """One frame as it plays: the page at that frame, cut to the camera (or the trim), at `size` if given."""
    from genko.export import crop_to
    from genko.render import mm_to_px, render_page

    image = render_page(at_frame(page, frame), dpi, mode="print", episode=episode, finish=False).convert("RGB")
    rect = camera_at(page, frame) if camera else None
    if rect is not None:
        x, y, w, h = rect
        image = image.crop((mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi)))
    else:
        image = crop_to(image, page, area, dpi)
    if size is not None and image.size != size:
        image = image.resize(size, Image.LANCZOS)
    return image


def export(page, dest, *, episode=None, dpi: int = 100, fmt: str | None = None, camera: bool = True,
           area: str = "trim", width: int | None = None):
    """The animation as a moving picture (gif | webp | png | mp4) or a folder of numbered PNGs (frames).
    `width` scales it to that many pixels across. Returns the file (or the list of files)."""
    from pathlib import Path

    from genko import timelapse

    if not is_animation(page):
        raise ValueError("the page is not an animation (set_animation first)")
    dest = Path(dest)
    fmt = (fmt or dest.suffix.lstrip(".") or "gif").lower()
    if fmt not in FORMATS:
        raise ValueError(f"format must be one of {', '.join(FORMATS)}")
    count = frames_of(page)
    first = render_frame(page, 1, dpi, episode, camera=camera, area=area)
    size = first.size
    if width:
        size = (int(width), max(1, round(first.height * int(width) / first.width)))
    pictures = [first.resize(size, Image.LANCZOS) if first.size != size else first]
    pictures += [render_frame(page, f, dpi, episode, camera=camera, area=area, size=size) for f in range(2, count + 1)]
    if fmt == "frames":
        dest.mkdir(parents=True, exist_ok=True)
        files = []
        for n, picture in enumerate(pictures, start=1):
            path = dest / f"frame_{n:04d}.png"
            picture.save(path)
            files.append(path)
        return files
    return timelapse.write_movie(pictures, dest.with_suffix("." + fmt), fps_of(page), fmt, hold=0,
                                 loop=bool((spec(page) or {}).get("loop", True)))
