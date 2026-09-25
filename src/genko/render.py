from __future__ import annotations

import io
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

from genko.models import Episode, LayerKind, LayerRole, Page, Rect, StoryLine

EXPORT_ROLES = (
    LayerRole.INK,
    LayerRole.BG,
    LayerRole.FINISH,
    LayerRole.TONE,
    LayerRole.EFFECT,
    LayerRole.FRAMES,
    LayerRole.TEXT,
)

NAME_COLOR = (58, 110, 165)
INK_COLOR = (20, 20, 20)


def export_plan(page: Page) -> list[LayerRole]:
    return list(EXPORT_ROLES)


def mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def rect_px(rect: Rect, dpi: int) -> tuple[int, int, int, int]:
    x = mm_to_px(rect.x, dpi)
    y = mm_to_px(rect.y, dpi)
    w = mm_to_px(rect.width, dpi)
    h = mm_to_px(rect.height, dpi)
    return x, y, x + w, y + h


def _xy(point: tuple, dpi: int) -> tuple[int, int]:
    return mm_to_px(float(point[0]), dpi), mm_to_px(float(point[1]), dpi)


def _stroke(
    draw: ImageDraw.ImageDraw,
    points: list[tuple],
    dpi: int,
    color: tuple[int, int, int],
    width: int,
) -> None:
    if len(points) < 2:
        return
    if len(points[0]) >= 3:
        for a, b in zip(points, points[1:]):
            pressure = float(a[2]) if len(a) > 2 else 1.0
            w = max(1, round(width * max(0.15, min(1.5, pressure))))
            draw.line([_xy(a, dpi), _xy(b, dpi)], fill=color, width=w, joint="curve")
        return
    xy = [_xy(pt, dpi) for pt in points]
    draw.line(xy, fill=color, width=width, joint="curve")


_STROKE_CACHE: dict = {}  # (layer id, dpi, size, guide) → (patches' signature, lines' signatures, image); oldest first
STROKE_CACHE_SIZE = 12


def _stroke_sig(stroke, brushes) -> tuple:
    """What decides how a line looks (a changed line gets a different signature)."""
    points = getattr(stroke, "points", None) or []
    return (getattr(stroke, "id", None), getattr(stroke, "kind", None), getattr(stroke, "width_mm", None), str(getattr(stroke, "rgb", None)),
            getattr(stroke, "opacity", None), len(points), tuple(points[0]) if points else None, tuple(points[-1]) if points else None,
            len(getattr(stroke, "pressure", None) or []), brushes.brush(getattr(stroke, "kind", None)))


def _layer_strokes(layer, size: tuple[int, int], dpi: int, panel_mask: Image.Image | None,
                   raster: Image.Image | None) -> Image.Image | None:
    """A layer's fills (patches) and pen lines, drawn from their data at this resolution (None if none).

    Name and draft lines are drawn in the name colour; the others in their own colour (ink black by
    default) with their brush's look (genko.brushes). Everything stays inside the panels unless the
    layer runs out of them, and a layer with locked transparency only keeps it where it has pixels.
    """
    from genko import brushes
    from genko.models import stroke_points

    strokes = getattr(layer, "strokes", None) or []
    patches = getattr(layer, "patches", None) or []
    if not strokes and not patches:
        return None
    guide = layer.role in (LayerRole.NAME, LayerRole.DRAFT)
    # the lines drawn so far are remembered per layer and resolution: a new line is drawn on top of them
    # instead of drawing the whole layer again (a finished page has thousands of lines)
    key = (getattr(layer, "id", None), dpi, size, guide)
    patch_sig = tuple((p.get("id"), tuple(p.get("box") or ()), len(p.get("png") or b""), str(p.get("rgb")), p.get("opacity"),
                       p.get("mode")) for p in patches)
    sigs = [_stroke_sig(stroke, brushes) for stroke in strokes]
    cached = _STROKE_CACHE.get(key) if key[0] else None
    if cached is not None and cached[0] == patch_sig and cached[1] == sigs[:len(cached[1])]:
        out = cached[2].copy()
        todo = strokes[len(cached[1]):]
    else:
        out = Image.new("RGBA", size, (0, 0, 0, 0))
        for patch in patches:
            _paint_patch(out, patch, dpi, NAME_COLOR if guide else None)
        todo = strokes
    # lines of one colour gather in one coverage mask (screen: a + b − ab, the same as laying them over each
    # other) and go onto the layer once per colour, instead of once per line
    ink: Image.Image | None = None
    ink_rgb = None

    def lay() -> None:
        nonlocal out
        if ink is not None and ink.getbbox():
            box = ink.getbbox()
            patch = Image.new("RGBA", (box[2] - box[0], box[3] - box[1]), (*ink_rgb, 0))
            patch.putalpha(ink.crop(box))
            out.alpha_composite(patch, (box[0], box[1]))

    for stroke in todo:
        b = brushes.brush(getattr(stroke, "kind", None))
        drawn = brushes.draw(size, stroke_points(stroke), dpi, float(getattr(stroke, "width_mm", 0.35) or 0.35),
                             getattr(stroke, "kind", None), seed=str(getattr(stroke, "id", "")))
        if drawn is None:
            continue
        cover, (x0, y0) = drawn
        rgb = NAME_COLOR if guide else tuple(getattr(stroke, "rgb", None) or b.rgb or INK_COLOR)
        opacity = max(0.0, min(1.0, float(getattr(stroke, "opacity", 1.0)))) * b.opacity
        if opacity < 1:
            cover = cover.point(lambda v, o=opacity: int(v * o))
        if rgb != ink_rgb:
            lay()
            ink, ink_rgb = Image.new("L", size, 0), rgb
        box = (x0, y0, x0 + cover.width, y0 + cover.height)
        ink.paste(ImageChops.screen(ink.crop(box), cover), box[:2])
    lay()
    if key[0]:
        _STROKE_CACHE.pop(key, None)
        _STROKE_CACHE[key] = (patch_sig, sigs, out.copy())
        while len(_STROKE_CACHE) > STROKE_CACHE_SIZE:
            _STROKE_CACHE.pop(next(iter(_STROKE_CACHE)))
    if panel_mask is not None and getattr(layer, "panel_clip", True):
        out.putalpha(_and_alpha(out, panel_mask))
    if getattr(layer, "lock_alpha", False) and raster is not None:
        out.putalpha(ImageChops.multiply(out.split()[3], raster.convert("RGBA").split()[3]))
    return out


def _paint_patch(out: Image.Image, patch: dict, dpi: int, colour=None) -> None:
    """A fill or a pasted image, kept at its own resolution over its box (mm), onto the layer image."""
    data = patch.get("png")
    if not data:
        return
    x, y, w, h = (float(v) for v in patch["box"])
    x0, y0 = round(x / 25.4 * dpi), round(y / 25.4 * dpi)
    pw, ph = max(1, round(w / 25.4 * dpi)), max(1, round(h / 25.4 * dpi))
    image = Image.open(io.BytesIO(data))
    opacity = max(0.0, min(1.0, float(patch.get("opacity", 1.0))))
    if patch.get("mode", "mask") == "mask":
        cover = image.convert("L").resize((pw, ph), Image.Resampling.LANCZOS)
        if pw > image.width * 1.5:  # upscaled fills: keep their edge crisp
            cover = cover.point(lambda v: 255 if v >= 128 else 0)
        if opacity < 1:
            cover = cover.point(lambda v, o=opacity: int(v * o))
        rgb = colour or tuple(patch.get("rgb") or INK_COLOR)
        piece = Image.new("RGBA", (pw, ph), (*rgb, 0))
        piece.putalpha(cover)
    else:
        piece = image.convert("RGBA").resize((pw, ph), Image.Resampling.LANCZOS)
        if opacity < 1:
            piece.putalpha(piece.split()[3].point(lambda v, o=opacity: int(v * o)))
    region_box = (x0, y0, x0 + pw, y0 + ph)
    region = out.crop(region_box)
    out.paste(Image.alpha_composite(region, piece), (x0, y0))


def _clip_mask(page: Page, size: tuple[int, int], dpi: int) -> Image.Image | None:
    leaves = [frame for frame in page.leaf_frames() if getattr(frame, "clip", True)]
    if not leaves:
        return None
    from genko.placement import clip_box

    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    for frame in leaves:
        if getattr(frame, "bleed", False) and not getattr(frame, "poly", None):
            draw.rectangle(rect_px(clip_box(page, frame, "bleed"), dpi), fill=255)  # a bleed panel runs out to the bleed
        else:
            fill_frame(draw, frame, dpi)
    return mask


def fill_frame(draw: ImageDraw.ImageDraw, frame, dpi: int, fill=255) -> None:
    """A panel's area: its rectangle, or its polygon when it is slanted or free-form."""
    if getattr(frame, "poly", None):
        draw.polygon([_xy(p, dpi) for p in frame.poly], fill=fill)
    else:
        draw.rectangle(rect_px(frame.rect, dpi), fill=fill)


def _and_alpha(layer: Image.Image, mask: Image.Image) -> Image.Image:
    alpha = layer.split()[3]
    return ImageChops.multiply(alpha, mask)


_DELA = Path(__file__).resolve().parent / "fonts" / "DelaGothicOne-Regular.ttf"  # package data (ships in the wheel)
_CJK_FONTS = (
    str(_DELA),
    r"C:\Windows\Fonts\YuGothM.ttc",
    r"C:\Windows\Fonts\meiryo.ttc",
    r"C:\Windows\Fonts\NotoSansJP-VF.ttf",
    r"C:\Windows\Fonts\msgothic.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
)


def _font(path: str | None = None, size: int = 14) -> ImageFont.ImageFont:
    size = max(8, int(size))
    candidates = [path] if path else []
    candidates.extend(_CJK_FONTS)
    for candidate in candidates:
        if not candidate:
            continue
        try:
            return ImageFont.truetype(candidate, size)
        except (OSError, ValueError):
            continue
    return ImageFont.load_default()


def _placed_raster(layer, page: Page, episode: Episode | None, size: tuple[int, int], dpi: int,
                   mode: str = "print", finish: bool = True) -> Image.Image | None:
    """Resample a placed image from its asset into its placement, clipped to its panel, and give
    monochrome pages their finish (print: black and white with dots; proof: the flat grey steps)."""
    from genko.assets import AssetStore
    from genko.placement import clip_box

    if episode is None or episode.asset_dir is None or not layer.asset or layer.placement_mm is None:
        return None
    data = AssetStore(episode.asset_dir).get_bytes(layer.asset, ".png")
    if data is None:
        return None
    r = layer.placement_mm  # may start left of / above the paper, so no clamping here
    x0, y0 = round(r.x / 25.4 * dpi), round(r.y / 25.4 * dpi)
    x1, y1 = round((r.x + r.width) / 25.4 * dpi), round((r.y + r.height) / 25.4 * dpi)
    if x1 <= x0 or y1 <= y0:
        return None
    frame = None
    if layer.frame_id:
        try:
            frame = page._find(layer.frame_id)
        except (KeyError, IndexError):
            frame = None
    cx0, cy0, cx1, cy1 = rect_px(clip_box(page, frame, layer.clip_to), dpi)
    # only the visible part is resampled, straight from the source pixels
    vx0, vy0 = max(x0, cx0, 0), max(y0, cy0, 0)
    vx1, vy1 = min(x1, cx1, size[0]), min(y1, cy1, size[1])
    if vx1 <= vx0 or vy1 <= vy0:
        return None
    source = Image.open(io.BytesIO(data)).convert("RGBA")
    sx, sy = source.width / (x1 - x0), source.height / (y1 - y0)
    box = ((vx0 - x0) * sx, (vy0 - y0) * sy, (vx1 - x0) * sx, (vy1 - y0) * sy)
    fitted = source.resize((vx1 - vx0, vy1 - vy0), Image.Resampling.LANCZOS, box=box)
    if finish:
        fitted = _finish_placed(fitted, layer, page, episode, dpi, mode, (vx0, vy0))
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.paste(fitted, (vx0, vy0))
    if frame is not None and getattr(frame, "poly", None) and layer.clip_to == "frame":
        shape_mask = Image.new("L", size, 0)
        fill_frame(ImageDraw.Draw(shape_mask), frame, dpi)
        canvas.putalpha(ImageChops.multiply(canvas.split()[3], shape_mask))
    return canvas


def _finish_placed(fitted: Image.Image, layer, page: Page, episode: Episode | None, dpi: int, mode: str,
                   origin: tuple[int, int]) -> Image.Image:
    from genko import screentone

    to = (layer.source or {}).get("to", "art")
    if mode not in ("print", "proof") or page.spec.expression == "color" or layer.role in (LayerRole.DRAFT, LayerRole.NAME):
        return fitted
    alpha = fitted.split()[3]
    grey = Image.alpha_composite(Image.new("RGBA", fitted.size, (255, 255, 255, 255)), fitted).convert("L")
    if to == "ink":
        # extracted line art: pure black lines, transparent elsewhere
        ink = grey.point(lambda v: 255 if v < 128 else 0)
        out = Image.new("RGBA", fitted.size, (0, 0, 0, 0))
        out.putalpha(ImageChops.multiply(ink, alpha))
        return out
    style = ((episode.studio.get("style") or {}).get("finish") if episode is not None else None) or {}
    finish = screentone.Finish.from_dict({**style, **(layer.finish or {})})
    done = screentone.finish_gray(grey, finish, dpi, screen=mode == "print", origin=origin)
    out = done.convert("RGBA")
    out.putalpha(alpha if mode == "proof" else alpha.point(lambda v: 255 if v >= 128 else 0))
    return out


def render_frame(page: Page, frame_id: str, working_dpi: int, mode: str = "proof", episode: Episode | None = None) -> Image.Image:
    """One panel, cropped from the page render (bleed panels include their bleed)."""
    from genko.placement import clip_box

    frame = page._find(frame_id)
    image = render_page(page, working_dpi, mode=mode, episode=episode)
    x0, y0, x1, y1 = rect_px(clip_box(page, frame, "bleed" if frame.bleed else "frame"), working_dpi)
    return image.crop((max(0, x0), max(0, y0), min(image.width, x1), min(image.height, y1)))


def _masked(layer, image: Image.Image) -> Image.Image:
    """A layer's pixels through its mask (white shows, black hides)."""
    mask = getattr(layer, "mask", None)
    if not mask or not mask.get("png") or not mask.get("enabled", True):
        return image
    shown = Image.open(io.BytesIO(mask["png"])).convert("L").resize(image.size, Image.Resampling.BILINEAR)
    image = image.convert("RGBA")
    image.putalpha(ImageChops.multiply(image.split()[3], shown))
    return image


def _tinted(image: Image.Image, rgb) -> Image.Image:
    """A layer shown in one colour (its shapes kept by their alpha): a blue draft, a red check."""
    image = image.convert("RGBA")
    out = Image.new("RGBA", image.size, tuple(int(v) for v in rgb)[:3] + (0,))
    out.putalpha(image.split()[3])
    return out


def layer_image(page: Page, layer, dpi: int, episode: Episode | None = None) -> Image.Image:
    """One layer alone over a transparent page (its pixels, fills and lines, panel clip and mask): for
    merging layers and for the layer panel's small pictures."""
    size = (mm_to_px(page.spec.width_mm, dpi), mm_to_px(page.spec.height_mm, dpi))
    empty = Image.new("RGBA", size, (0, 0, 0, 0))
    if getattr(layer, "kind", None) == LayerKind.FOLDER:
        return empty
    if _is_tone(layer):
        from genko import tones

        white = Image.new("RGBA", size, (255, 255, 255, 255))
        drawn = tones.draw_layer(white.copy(), layer, page, dpi, print_mode=False)
        grey = ImageChops.difference(white.convert("L"), drawn.convert("L"))  # the tone's ink as alpha
        out = Image.new("RGBA", size, (20, 20, 20, 0))
        out.putalpha(grey)
        return _masked(layer, out)
    if layer.kind == LayerKind.PLACED:
        raster = _placed_raster(layer, page, episode, size, dpi, "proof", False)
    else:
        raster = _open_raster(layer)
        if raster is not None:
            raster = raster.resize(size)
    lines = _layer_strokes(layer, size, dpi, _clip_mask(page, size, dpi), raster)
    if lines is not None:
        raster = lines if raster is None else Image.alpha_composite(raster.convert("RGBA"), lines)
    return _masked(layer, raster.convert("RGBA")) if raster is not None else empty


def _open_raster(layer) -> Image.Image | None:
    if layer.raster_png:
        return Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
    return None


def _blend_over(base: Image.Image, over: Image.Image, mode: str, opacity: float, clip_mask: Image.Image | None = None) -> Image.Image:
    over = over.convert("RGBA")
    if clip_mask is not None:
        r, g, b, a = over.split()
        a = ImageChops.multiply(a, clip_mask.convert("L"))
        over = Image.merge("RGBA", (r, g, b, a))
    if opacity < 1:
        r, g, b, a = over.split()
        a = a.point(lambda p, o=opacity: int(p * o))
        over = Image.merge("RGBA", (r, g, b, a))
    base_rgba = base.convert("RGBA")
    if mode in ("", "normal", None):
        return Image.alpha_composite(base_rgba, over)
    br, bg, bb, ba = base_rgba.split()
    rr, rg, rb, ra = over.split()
    base_rgb = Image.merge("RGB", (br, bg, bb))
    over_rgb = Image.merge("RGB", (rr, rg, rb))
    if mode == "multiply":
        mixed = ImageChops.multiply(base_rgb, over_rgb)
    elif mode == "screen":
        mixed = ImageChops.screen(base_rgb, over_rgb)
    elif mode == "add":
        mixed = ImageChops.add(base_rgb, over_rgb)
    elif mode == "overlay":
        mixed = ImageChops.overlay(base_rgb, over_rgb) if hasattr(ImageChops, "overlay") else ImageChops.multiply(base_rgb, over_rgb)
    else:
        mixed = over_rgb
    mixed_rgba = mixed.convert("RGBA")
    mixed_rgba.putalpha(ra)
    return Image.alpha_composite(base_rgba, mixed_rgba)


def _is_tone(layer) -> bool:
    return layer.role == LayerRole.TONE or getattr(layer, "kind", None) == LayerKind.TONE


def _draw_tone(image: Image.Image, page: Page, dpi: int, mode: str = "print", finish: bool = True) -> Image.Image:
    """Every visible tone layer (genko.tones) over the image: the pattern in print, its grey otherwise."""
    from genko import tones

    for layer in page.layers:
        if _is_tone(layer) and layer.visible:
            image = tones.draw_layer(image, layer, page, dpi, print_mode=mode == "print" and finish)
    return image


def _draw_effects(image: Image.Image, page: Page, dpi: int) -> Image.Image:
    """Effect lines (genko.effects), each inside its panel; the same effect always gives the same lines."""
    from genko import effects

    for effect in page.effects:
        if effect.get("kind") in effects.KINDS and effect.get("visible", True):
            image = effects.draw(image, effect, page, dpi)
    return image


def _draw_prims(image: Image.Image, page: Page, dpi: int, mode: str) -> None:
    """3D figures and boxes: drawing guides in the name and proof renders (never printed)."""
    if mode == "print" or not page.prims:
        return
    from genko import prim3d

    draw = ImageDraw.Draw(image)
    width = max(1, mm_to_px(0.3, dpi))
    for prim in page.prims:
        if prim.get("kind") == "mannequin":
            _draw_mannequin(draw, prim, dpi)
            continue
        for a, b, seen in prim3d.edges(prim):
            draw.line([_xy(a, dpi), _xy(b, dpi)], fill=(90, 90, 140) if seen else (190, 190, 215), width=width)


def _draw_mannequin(draw: ImageDraw.ImageDraw, prim: dict, dpi: int, color=(90, 90, 140)) -> None:
    """The posable figure (genko.mannequin): body, elbows, knees and neck, turned and leaned by rot."""
    from genko import mannequin

    bone = mannequin.skeleton(prim)
    width = max(2, mm_to_px(0.5, dpi))
    shade = {"body": color, "left": (60, 120, 170), "right": (150, 90, 120)}
    for a, b, part in bone["segments"]:
        draw.line([_xy(a, dpi), _xy(b, dpi)], fill=shade.get(part, color), width=width)
    for a, _, part in bone["segments"]:
        if part != "body":
            px, py = _xy(a, dpi)
            r = max(1, width)
            draw.ellipse((px - r, py - r, px + r, py + r), fill=shade[part])
    (hx, hy), hr = bone["head"]
    x0, y0 = _xy((hx - hr, hy - hr), dpi)
    x1, y1 = _xy((hx + hr, hy + hr), dpi)
    draw.ellipse((x0, y0, x1, y1), outline=color, width=width)
    nose = (hx + hr * 0.8 * bone["facing"], hy)
    draw.line([_xy((hx, hy), dpi), _xy(nose, dpi)], fill=color, width=max(1, width // 2))


def _draw_balloon(
    draw: ImageDraw.ImageDraw, line: StoryLine, dpi: int, font_path: str | None = None, show_speaker: bool = True
) -> None:
    """One line's balloon and lettering onto the image behind `draw` (see genko.balloons)."""
    from genko import balloons

    balloons.draw_lines(draw._image, [line], dpi, font_path, show_speaker)


def _draw_crop_marks(draw: ImageDraw.ImageDraw, page: Page, dpi: int) -> None:
    """トンボ: at each corner the trim line and the bleed line (内トンボ・外トンボ), and a centre mark on each
    side, all outside the bleed. Pages without room around the bleed get short marks at the trim."""
    trim, bleed = page.trim_rect_mm(), page.bleed_rect_mm()
    room = min(bleed.x, bleed.y, page.spec.width_mm - bleed.x - bleed.width, page.spec.height_mm - bleed.y - bleed.height)
    ink = (0, 0, 0)
    px = lambda v: mm_to_px(v, dpi)  # noqa: E731
    if room < 4:
        mark = px(5)
        for x_mm, y_mm, dx, dy in ((trim.x, trim.y, -1, -1), (trim.x + trim.width, trim.y, 1, -1),
                                   (trim.x, trim.y + trim.height, -1, 1), (trim.x + trim.width, trim.y + trim.height, 1, 1)):
            x, y = px(x_mm), px(y_mm)
            draw.line((x, y, x + dx * mark, y), fill=ink, width=1)
            draw.line((x, y, x, y + dy * mark), fill=ink, width=1)
        return
    gap, length = 1.0, min(10.0, room - 1.5)
    xs = {"l": (trim.x, bleed.x), "r": (trim.x + trim.width, bleed.x + bleed.width)}
    ys = {"t": (trim.y, bleed.y), "b": (trim.y + trim.height, bleed.y + bleed.height)}
    for hx, (tx, bx) in xs.items():
        for vy, (ty, by) in ys.items():
            out_x = -1 if hx == "l" else 1
            out_y = -1 if vy == "t" else 1
            # horizontal marks (at the trim and bleed heights) out beyond the bleed on the left / right
            x0 = bx + out_x * gap
            for y in (ty, by):
                draw.line((px(x0), px(y), px(x0 + out_x * length), px(y)), fill=ink, width=1)
            y0 = by + out_y * gap
            for x in (tx, bx):
                draw.line((px(x), px(y0), px(x), px(y0 + out_y * length)), fill=ink, width=1)
    cx, cy = trim.x + trim.width / 2, trim.y + trim.height / 2
    for x in (bleed.x - gap - length, bleed.x + bleed.width + gap):
        draw.line((px(x), px(cy), px(x + length), px(cy)), fill=ink, width=1)
    for y in (bleed.y - gap - length, bleed.y + bleed.height + gap):
        draw.line((px(cx), px(y), px(cx), px(y + length)), fill=ink, width=1)


def _draw_frames(draw: ImageDraw.ImageDraw, page: Page, working_dpi: int) -> None:
    for frame in page.leaf_frames():
        width_px = max(1, mm_to_px(frame.border_mm if frame.border_mm is not None else 0.8, working_dpi))
        if frame.border_mm is not None and frame.border_mm <= 0:
            continue  # a panel without a border
        if getattr(frame, "poly", None):
            draw.polygon([_xy(p, working_dpi) for p in frame.poly], outline=(20, 20, 20), width=width_px)
            continue
        if not frame.bleed:
            draw.rectangle(rect_px(frame.rect, working_dpi), outline=(20, 20, 20), width=max(1, width_px))
            continue
        # A bleed panel has no border on the sides that run off the paper.
        from genko.placement import outer_edges

        x0, y0, x1, y1 = rect_px(frame.rect, working_dpi)
        open_sides = outer_edges(page, frame)
        for side, line in (("top", (x0, y0, x1, y0)), ("bottom", (x0, y1, x1, y1)), ("left", (x0, y0, x0, y1)), ("right", (x1, y0, x1, y1))):
            if not open_sides[side]:
                draw.line(line, fill=(20, 20, 20), width=max(1, width_px))


def render_page(
    page: Page,
    working_dpi: int,
    mode: str = "print",
    episode: Episode | None = None,
    crop_marks: bool = False,
    onion: bool = True,
    finish: bool | None = None,
) -> Image.Image:
    """`finish`: the monochrome print finish (dots and pure black and white). None = by the page
    (mono pages yes, colour pages no); False for screen and colour outputs (webtoon, SNS)."""
    if finish is None:
        finish = page.spec.expression != "color"
    width = mm_to_px(page.spec.width_mm, working_dpi)
    height = mm_to_px(page.spec.height_mm, working_dpi)
    size = (width, height)
    image = Image.new("RGB", size, (255, 255, 255))
    include_name = mode in ("name", "proof")

    fill_roles = (LayerRole.BG, LayerRole.INK, LayerRole.FINISH)
    if include_name:
        fill_roles = (LayerRole.BG, LayerRole.NAME, LayerRole.INK, LayerRole.FINISH)
    for role in fill_roles:
        fill = page.fills.get(role)
        if fill is None:
            continue
        if role in (LayerRole.NAME, LayerRole.DRAFT) and mode == "print":
            continue
        image.paste(Image.new("RGB", size, fill), (0, 0))

    rgba = image.convert("RGBA")
    prev_alpha = None
    panel_mask = _clip_mask(page, size, working_dpi)
    for layer in page.layers:
        if getattr(layer, "kind", None) == LayerKind.FOLDER:
            continue
        if not layer.visible:
            continue
        if mode == "print" and not layer.exportable:
            continue
        if layer.role in (LayerRole.NAME, LayerRole.DRAFT) and mode == "print":
            continue
        if _is_tone(layer):  # tones sit in the layer order: a layer above can cover them
            from genko import tones

            rgba = tones.draw_layer(rgba, layer, page, working_dpi, print_mode=mode == "print" and finish)
            prev_alpha = None
            continue
        if layer.kind == LayerKind.PLACED:
            raster = _placed_raster(layer, page, episode, size, working_dpi, mode, finish)
        else:
            raster = _open_raster(layer)
            if raster is not None:
                raster = raster.resize(size)
        lines = _layer_strokes(layer, size, working_dpi, panel_mask, raster)
        if lines is not None:
            raster = lines if raster is None else Image.alpha_composite(raster.convert("RGBA"), lines)
        if raster is None:
            continue
        raster = _masked(layer, raster)
        if mode != "print" and getattr(layer, "color", None):
            raster = _tinted(raster, layer.color)
        clip_mask = prev_alpha if getattr(layer, "clip", False) else None
        opacity = getattr(layer, "opacity", None)
        opacity = 1.0 if opacity is None else float(opacity)  # 0 means invisible, not "default"
        rgba = _blend_over(rgba, raster, getattr(layer, "blend", "normal") or "normal", opacity, clip_mask)
        prev_alpha = raster.split()[3]
    image = rgba.convert("RGB")

    if page.effects:
        image = _draw_effects(image, page, working_dpi)
    _draw_prims(image, page, working_dpi, mode)

    if page.ruler and mode in ("name", "proof"):
        draw = ImageDraw.Draw(image)
        for pt in page.ruler.get("points") or []:
            px, py = _xy(pt, working_dpi)
            draw.ellipse((px - 3, py - 3, px + 3, py + 3), outline=(180, 80, 80), width=2)
            draw.line((px, 0, px, image.height), fill=(220, 180, 180), width=1)
            draw.line((0, py, image.width, py), fill=(220, 180, 180), width=1)

    draw = ImageDraw.Draw(image)
    _draw_frames(draw, page, working_dpi)

    font_path = getattr(episode, "font_path", None) if episode is not None else None
    font = _font(font_path)
    lines = episode.story_for_page(page.index) if episode is not None else page.texts
    from genko import balloons

    placed = [line for line in lines if line.x_mm or line.y_mm or line.balloon]
    # Speaker names are a working aid: shown in name/proof, never printed.
    balloons.draw_lines(image, placed, working_dpi, font_path, show_speaker=mode != "print")
    for line in lines:
        if line in placed:
            continue
        else:
            x = mm_to_px(page.inner_rect_mm().x + 4, working_dpi)
            y = mm_to_px(page.inner_rect_mm().y + 4, working_dpi)
            label = f"{line.speaker}: {line.text}" if line.speaker else line.text
            draw.text((x, y), label, fill=(10, 10, 10), font=font)

    if mode in ("print", "proof"):
        from genko import nombre

        nombre.draw(image, episode, page, working_dpi)

    if crop_marks and mode == "print":
        _draw_crop_marks(draw, page, working_dpi)
    if (
        onion
        and episode is not None
        and mode in ("name", "proof")
        and getattr(page, "onion_from", None)
    ):
        prev = next((item for item in episode.pages if item.index == page.onion_from), None)
        if prev is not None:
            ghost = render_page(prev, working_dpi, mode="print", episode=episode, onion=False)
            ghost = ghost.convert("RGBA")
            r, g, b, a = ghost.split()
            tint = Image.merge("RGBA", (r.point(lambda p: int(p * 0.4)), g.point(lambda p: int(p * 0.4)), b, a.point(lambda p: 70)))
            image = Image.alpha_composite(image.convert("RGBA"), tint).convert("RGB")
    return image


def to_bitonal(image: Image.Image, threshold: int = 180) -> Image.Image:
    return image.convert("L").point(lambda p: 255 if p > threshold else 0, mode="1")


def render_spread(episode: Episode, first: int, second: int, dpi: int = 150, mode: str = "print",
                  finish: bool | None = None, to_trim: bool = False) -> Image.Image:
    pages = {page.index: page for page in episode.pages}
    a, b = pages[first], pages[second]
    # Place by the physical side of the book (binding-aware), not by page parity.
    if a.side(episode.start_side) == "right" and b.side(episode.start_side) == "left":
        right, left = a, b
    elif b.side(episode.start_side) == "right" and a.side(episode.start_side) == "left":
        right, left = b, a
    else:
        left, right = a, b
    left_img = render_page(left, dpi, mode=mode, episode=episode, finish=finish)
    right_img = render_page(right, dpi, mode=mode, episode=episode, finish=finish)
    # the finished sizes meet at the gutter: the left page up to its trim's right edge, the right from its trim's left edge
    lt, rt = left.trim_rect_mm(), right.trim_rect_mm()
    top = lambda t, img: mm_to_px(t.y, dpi) if to_trim else 0  # noqa: E731
    bottom = lambda t, img: mm_to_px(t.y + t.height, dpi) if to_trim else img.height  # noqa: E731
    left_img = left_img.crop((mm_to_px(lt.x, dpi) if to_trim else 0, top(lt, left_img), mm_to_px(lt.x + lt.width, dpi),
                              bottom(lt, left_img)))
    right_img = right_img.crop((mm_to_px(rt.x, dpi), top(rt, right_img), mm_to_px(rt.x + rt.width, dpi) if to_trim else right_img.width,
                                bottom(rt, right_img)))
    image = Image.new("RGB", (left_img.width + right_img.width, max(left_img.height, right_img.height)), (255, 255, 255))
    image.paste(left_img, (0, 0))
    image.paste(right_img, (left_img.width, 0))
    return image
