from __future__ import annotations

import array
import base64
import binascii
import copy
import dataclasses
import sys
from dataclasses import dataclass, field
from enum import Enum
from uuid import uuid4


class Binding(str, Enum):
    RIGHT = "right"
    LEFT = "left"


class LayerRole(str, Enum):
    NAME = "name"
    DRAFT = "draft"
    INK = "ink"
    BG = "bg"
    FINISH = "finish"
    TONE = "tone"
    EFFECT = "effect"
    FRAMES = "frames"
    TEXT = "text"
    USER = "user"


class LayerKind(str, Enum):
    RASTER = "raster"
    STROKES = "strokes"
    FILL = "fill"
    TONE = "tone"
    FOLDER = "folder"
    PLACED = "placed"  # an imported image placed into a panel; bytes stay in assets/
    ADJUST = "adjust"  # a correction layer (色調補正レイヤー): changes the colours of what is under it, at render time


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    width: float
    height: float

    def contains(self, x: float, y: float) -> bool:
        return self.x <= x <= self.x + self.width and self.y <= y <= self.y + self.height


@dataclass(frozen=True)
class PageSpec:
    """One page's paper (the canvas), its finished size (仕上がり, centred on the paper), the bleed around
    it (裁ち落とし) and the basic frame inside it (基本枠: margins from the trim at the top, bottom, the
    binding side (のど, inner) and the fore-edge (小口, outer)). Coordinates start at the paper's top left.

    Old books had no trim or margins: their trim is the paper less the bleed all round and their margins
    are `inner_margin_mm` on every side (so they draw exactly as before).
    """

    width_mm: float
    height_mm: float
    dpi: int
    bleed_mm: float
    inner_margin_mm: float
    expression: str = "mono"
    preset: str | None = None
    trim_w_mm: float | None = None
    trim_h_mm: float | None = None
    margins_mm: tuple[float, float, float, float] | None = None  # top, bottom, inner (のど), outer (小口)

    # --- sizes ------------------------------------------------------------------------------------------

    def trim_size(self) -> tuple[float, float]:
        if self.trim_w_mm and self.trim_h_mm:
            return float(self.trim_w_mm), float(self.trim_h_mm)
        return self.width_mm - 2 * self.bleed_mm, self.height_mm - 2 * self.bleed_mm

    def trim_origin(self) -> tuple[float, float]:
        w, h = self.trim_size()
        return (self.width_mm - w) / 2, (self.height_mm - h) / 2

    def margins(self) -> dict[str, float]:
        if self.margins_mm:
            top, bottom, inner, outer = (float(v) for v in self.margins_mm)
        else:
            top = bottom = inner = outer = float(self.inner_margin_mm)
        return {"top": top, "bottom": bottom, "inner": inner, "outer": outer}

    def frame_size(self) -> tuple[float, float]:
        w, h = self.trim_size()
        m = self.margins()
        return w - m["inner"] - m["outer"], h - m["top"] - m["bottom"]

    def describe(self) -> str:
        w, h = self.trim_size()
        fw, fh = self.frame_size()
        return (f"用紙 {self.width_mm:g}×{self.height_mm:g} mm ・ 仕上がり {w:g}×{h:g} mm ・ 裁ち落とし {self.bleed_mm:g} mm ・ "
                f"基本枠 {fw:g}×{fh:g} mm ・ {self.dpi} dpi")

    # --- presets (the usual sizes; publishers and printers differ, so every number can be changed) --------

    @staticmethod
    def b4_comic() -> PageSpec:
        """B4 manuscript paper for magazines and contests: finished 220×310 (1.2× the printed B5), bleed 5,
        basic frame 180×270."""
        return PageSpec(257, 364, 600, 5, 20, "mono", preset="commercial-b4", trim_w_mm=220, trim_h_mm=310,
                        margins_mm=(20, 20, 20, 20))

    @staticmethod
    def b5_doujin() -> PageSpec:
        """B5 doujinshi at print size: finished 182×257, bleed 3, basic frame 150×220."""
        return PageSpec(208, 283, 600, 3, 16, "mono", preset="doujin-b5", trim_w_mm=182, trim_h_mm=257,
                        margins_mm=(18.5, 18.5, 16, 16))

    @staticmethod
    def a5_doujin() -> PageSpec:
        """A5 doujinshi at print size: finished 148×210, bleed 3, basic frame 120×180."""
        return PageSpec(174, 236, 600, 3, 14, "mono", preset="doujin-a5", trim_w_mm=148, trim_h_mm=210,
                        margins_mm=(15, 15, 14, 14))

    @staticmethod
    def a4_mono() -> PageSpec:
        """An A4 sheet for practice: the whole sheet is the page (bleed 3, 10 mm margins)."""
        return PageSpec(210, 297, 600, 3, 10, "mono")

    @staticmethod
    def webtoon() -> PageSpec:
        return PageSpec(80, 400, 300, 0, 4, "color")

    @staticmethod
    def custom(paper_w: float, paper_h: float, trim_w: float, trim_h: float, bleed: float, top: float, bottom: float,
               inner: float, outer: float, dpi: int = 600, expression: str = "mono") -> PageSpec:
        if trim_w + 2 * bleed > paper_w + 1e-6 or trim_h + 2 * bleed > paper_h + 1e-6:
            raise ValueError("the paper must hold the finished size and its bleed")
        if inner + outer >= trim_w or top + bottom >= trim_h:
            raise ValueError("the basic frame must fit inside the finished size")
        if min(trim_w, trim_h, paper_w, paper_h) <= 0 or min(bleed, top, bottom, inner, outer) < 0:
            raise ValueError("sizes must be positive")
        return PageSpec(float(paper_w), float(paper_h), int(dpi), float(bleed), float(min(top, bottom, inner, outer)), expression,
                        preset="custom", trim_w_mm=float(trim_w), trim_h_mm=float(trim_h),
                        margins_mm=(float(top), float(bottom), float(inner), float(outer)))

    @staticmethod
    def publisher(name: str) -> PageSpec:
        key = name.strip().lower()
        known = {"shueisha", "kodansha", "kadokawa", "shogakukan"}
        return dataclasses.replace(PageSpec.b4_comic(), preset=key if key in known else "none")


PAPER_PRESETS = {
    "b4": ("B4 商業誌・投稿（仕上がり 220×310・基本枠 180×270・600 dpi）", PageSpec.b4_comic),
    "b5": ("B5 同人誌（原寸・仕上がり 182×257・基本枠 150×220）", PageSpec.b5_doujin),
    "a5": ("A5 同人誌（原寸・仕上がり 148×210・基本枠 120×180）", PageSpec.a5_doujin),
    "a4": ("A4 練習用（紙全体がページ）", PageSpec.a4_mono),
    "webtoon": ("縦読み・カラー（Webtoon、幅 80 mm）", PageSpec.webtoon),
}


@dataclass
class Stroke:
    id: str
    points: list[tuple[float, float]] = field(default_factory=list)
    pressure: list[float] = field(default_factory=list)
    width_mm: float = 0.35
    kind: str = "gpen"
    handles: list | None = None
    rgb: tuple[int, int, int] | None = None  # None: the layer's default ink colour
    opacity: float = 1.0
    rotation: list[float] = field(default_factory=list)  # the pen's barrel turn at each point (degrees; アートペン)

    def __deepcopy__(self, memo: dict) -> Stroke:
        # (a page holds thousands of these; points are tuples of numbers, which never need copying)
        new = copy.copy(self)
        memo[id(self)] = new
        new.points = [p if type(p) is tuple else copy.deepcopy(p, memo) for p in self.points]
        new.pressure = list(self.pressure)
        new.rotation = list(self.rotation)
        if self.handles is not None:
            new.handles = copy.deepcopy(self.handles, memo)
        return new


_BIG = sys.byteorder == "big"  # blobs are little-endian everywhere


def _pack(values, kind: str) -> str:
    arr = array.array(kind, values)
    if _BIG:
        arr.byteswap()
    return base64.b64encode(arr.tobytes()).decode("ascii")


def _unpack(text: str, kind: str):
    arr = array.array(kind)
    arr.frombytes(binascii.a2b_base64(text))
    if _BIG:
        arr.byteswap()
    return arr


def stroke_to_packed(stroke) -> dict:
    """A stroke for the saved blob: points as little-endian float64 pairs (mm) and pressure as float64,
    both base64. Reading these is many times quicker than lists of numbers,
    which is what a book of fifty thousand lines needs to open quickly."""
    stroke = coerce_stroke(stroke)
    out = {"id": stroke.id, "kind": stroke.kind, "width_mm": stroke.width_mm,
           "xy": _pack([float(v) for p in stroke.points for v in (p[0], p[1])], "d")}
    if stroke.pressure:
        out["p"] = _pack([float(v) for v in stroke.pressure], "d")
    if stroke.rgb is not None:
        out["rgb"] = list(stroke.rgb)
    if stroke.opacity != 1.0:
        out["opacity"] = stroke.opacity
    if stroke.rotation:
        out["r"] = _pack([float(v) for v in stroke.rotation], "d")
    return out


def coerce_stroke(raw) -> Stroke:
    if isinstance(raw, Stroke):
        return raw
    if isinstance(raw, dict) and "xy" in raw:
        xy = _unpack(raw["xy"], "d")
        return Stroke(
            id=raw.get("id") or new_id(),
            points=list(zip(xy[0::2], xy[1::2])),
            pressure=_unpack(raw["p"], "d").tolist() if raw.get("p") else [],
            width_mm=float(raw.get("width_mm", 0.35)),
            kind=str(raw.get("kind") or "gpen"),
            rgb=tuple(int(v) for v in raw["rgb"]) if raw.get("rgb") else None,
            opacity=float(raw.get("opacity", 1.0)),
            rotation=_unpack(raw["r"], "d").tolist() if raw.get("r") else [],
        )
    if isinstance(raw, dict):
        points = [tuple(pt[:2]) for pt in raw.get("points") or []]
        pressure = [float(p) for p in raw.get("pressure") or []]
        if not pressure:
            pressure = [float(pt[2]) for pt in raw.get("points") or [] if len(pt) > 2]
        return Stroke(
            id=raw.get("id") or new_id(),
            points=[(float(x), float(y)) for x, y in points],
            pressure=pressure,
            width_mm=float(raw.get("width_mm", 0.35)),
            kind=str(raw.get("kind") or "gpen"),
            rgb=tuple(int(v) for v in raw["rgb"]) if raw.get("rgb") else None,
            opacity=float(raw.get("opacity", 1.0)),
            rotation=[float(v) for v in raw.get("rotation") or []],
        )
    points: list[tuple[float, float]] = []
    pressure: list[float] = []
    for pt in raw:
        points.append((float(pt[0]), float(pt[1])))
        if len(pt) > 2:
            pressure.append(float(pt[2]))
    if len(pressure) != len(points):
        pressure = []
    return Stroke(id=new_id(), points=points, pressure=pressure)


def stroke_points(stroke) -> list[tuple]:
    if isinstance(stroke, Stroke):
        if stroke.pressure and len(stroke.pressure) == len(stroke.points):
            return [(p[0], p[1], pr) for p, pr in zip(stroke.points, stroke.pressure)]
        return list(stroke.points)
    return list(stroke)


def stroke_to_dict(stroke) -> dict | list:
    if isinstance(stroke, Stroke):
        out = {
            "id": stroke.id,
            "points": stroke.points,
            "pressure": stroke.pressure,
            "width_mm": stroke.width_mm,
            "kind": stroke.kind,
        }
        if stroke.rgb is not None:
            out["rgb"] = list(stroke.rgb)
        if stroke.opacity != 1.0:
            out["opacity"] = stroke.opacity
        return out
    return stroke


@dataclass
class Layer:
    id: str
    role: LayerRole
    kind: LayerKind = LayerKind.STROKES
    visible: bool = True
    exportable: bool = True
    strokes: list = field(default_factory=list)
    raster_relpath: str | None = None
    fill_rgb: tuple[int, int, int] | None = None
    raster_png: bytes | None = field(default=None, repr=False, compare=False)
    lpi: float | None = None
    density: float | None = None
    region: list[tuple[float, float]] | None = None
    opacity: float = 1.0
    material_id: str | None = None
    angle: float = 45.0
    title: str = ""
    blend: str = "normal"
    clip: bool = False
    lock_alpha: bool = False
    locked: bool = False  # nothing can be drawn on or erased from a locked layer
    panel_clip: bool = True  # lines stay inside the panels; False lets them run out (はみ出し)
    # fills and pasted pixels kept at their own resolution over a box:
    # [{"id", "box": [x, y, w, h] mm, "mode": "mask" | "image", "png": bytes, "rgb", "opacity"}]
    patches: list = field(default_factory=list)
    tone: dict | None = None  # tone layers: {pattern, gradient} (genko.tones); lpi / density / angle are fields
    parent_id: str | None = None
    asset: str | None = None  # placed: "sha256:…" in assets/
    frame_id: str | None = None  # placed: the panel it belongs to
    placement_mm: Rect | None = None  # placed: where the whole image lands on the page
    fit: str = "cover"  # cover | contain | stretch
    clip_to: str = "frame"  # frame | bleed | none
    source: dict | None = None  # placed: {"candidate": …, "request": …}
    finish: dict | None = None  # placed: mono finishing override (M6)
    # a layer mask: {"png": L image over the whole page (white shows, black hides), "enabled": bool}
    mask: dict | None = None
    color: tuple[int, int, int] | None = None  # shown in this colour on screen (a blue draft); never printed
    reference: bool = False  # fills set to "reference" look at the lines of these layers (参照レイヤー)
    # J5: a fill layer's colour or gradient ({"rgb"} | {"gradient": {from, to, rgb_from, rgb_to, opacity_from,
    # opacity_to, shape}}); a correction layer's adjustment ({"kind", …params}); effects on the layer's picture
    # ({"border": {width_mm, rgb}, "water_edge": {width_mm, strength}}); the layer colour printed too
    fill: dict | None = None
    adjust: dict | None = None
    effect: dict | None = None
    color_prints: bool = False
    screen: dict | None = None  # J6 トーン化: the layer's greys printed as a halftone {pattern, lpi, angle, black, white}


@dataclass
class Frame:
    id: str
    rect: Rect
    children: list[Frame] = field(default_factory=list)
    split_axis: str | None = None
    clip: bool = True
    bleed: bool = False
    border_mm: float = 0.8
    panel: dict | None = None  # leaf only: the panel brief (PanelSpec), candidates and adoption
    poly: list | None = None  # a slanted or free-form panel: its corners (page mm); rect is their box
    split: dict | None = None  # a split node's cut: {"a", "b"} in its box's 0..1 coordinates, "gutter_mm"
    custom: bool = False  # a person shaped this panel by hand (it keeps its form when the page is re-laid)
    curves: list | None = None  # J6: how far each edge bows out (mm, outward +; edge i runs from corner i)
    line: dict | None = None  # J6: the border's look {kind: solid|double|dashed|dotted|rough, rgb, gap_mm, dash_mm}


@dataclass
class StoryLine:
    id: str
    page_index: int
    text: str
    speaker: str = ""
    frame_id: str | None = None
    ruby: str = ""
    x_mm: float = 0
    y_mm: float = 0
    w_mm: float = 40
    h_mm: float = 20
    balloon: str = "speech"
    tail: tuple[float, float] | None = None
    wrap: str = "horizontal"
    ruby_runs: list = field(default_factory=list)
    path: list | None = None  # a balloon drawn by hand: its outline (mm); the box is its bounds
    emphasis_runs: list = field(default_factory=list)  # 傍点: the stretches of text that carry dots
    style_runs: list = field(default_factory=list)  # part of the line styled: [[words, {scale, bold, rgb}]]
    # lettering and balloon style: font, size_mm, tracking, leading, align, outline_mm, rgb, tcy,
    # border_mm, fill ("white" | "none"), group (balloons with the same group are drawn as one)
    style: dict = field(default_factory=dict)
    # tails: [{"to": [x, y], "via": [x, y] | None, "width_mm": float | None}]; `tail` is the old single tail
    tails: list = field(default_factory=list)


def new_id() -> str:
    return uuid4().hex[:12]


_new_id = new_id


def default_layers() -> list[Layer]:
    return [
        Layer(id=new_id(), role=LayerRole.BG, kind=LayerKind.FILL, exportable=True),
        Layer(id=new_id(), role=LayerRole.NAME, kind=LayerKind.STROKES, exportable=False),
        Layer(id=new_id(), role=LayerRole.INK, kind=LayerKind.STROKES, exportable=True),
        Layer(id=new_id(), role=LayerRole.FINISH, kind=LayerKind.STROKES, exportable=True),
    ]


@dataclass
class Bible:
    plot: str = ""
    characters: list[dict] = field(default_factory=list)
    constraints: list[str] = field(default_factory=list)


@dataclass
class Page:
    index: int
    spec: PageSpec
    frames: list[Frame]
    binding: Binding = Binding.RIGHT
    note: str = ""
    name_ok: bool = False
    stage: str = "name"
    fills: dict[LayerRole, tuple[int, int, int]] = field(default_factory=dict)
    layers: list[Layer] = field(default_factory=list)
    texts: list[StoryLine] = field(default_factory=list)
    spread_with: int | None = None
    selected_frame_id: str | None = None
    effects: list[dict] = field(default_factory=list)
    ruler: dict | None = None  # the old single perspective ruler (kept for old books)
    rulers: list[dict] = field(default_factory=list)  # genko.rulers
    prims: list[dict] = field(default_factory=list)
    numero: bool = True
    onion_from: int | None = None
    lt_threshold: float | None = None
    extra: dict = field(default_factory=dict)  # keys this build does not know; written back unchanged
    id: str = field(default_factory=lambda: "pg_" + new_id())  # stable across reorder/delete (v3)
    art_ok: bool = False  # art gate: a person approved this page's pictures
    plan: dict | None = None  # name plan metadata: turn_role, layout, slots, reviews

    def __post_init__(self) -> None:
        if not self.layers:
            self.layers = default_layers()

    def _layer(self, role: LayerRole) -> Layer:
        for layer in self.layers:
            if layer.role == role:
                return layer
        layer = Layer(
            id=new_id(),
            role=role,
            kind=LayerKind.STROKES,
            exportable=role not in (LayerRole.NAME, LayerRole.DRAFT),
        )
        self.layers.append(layer)
        return layer

    @property
    def name_strokes(self) -> list:
        return [stroke_points(s) for s in self._layer(LayerRole.NAME).strokes]

    @name_strokes.setter
    def name_strokes(self, value: list) -> None:
        self._layer(LayerRole.NAME).strokes = [coerce_stroke(item) for item in value]

    @property
    def ink_strokes(self) -> list:
        return [stroke_points(s) for s in self._layer(LayerRole.INK).strokes]

    @ink_strokes.setter
    def ink_strokes(self, value: list) -> None:
        self._layer(LayerRole.INK).strokes = [coerce_stroke(item) for item in value]

    def paper_rect_mm(self) -> Rect:
        return Rect(0.0, 0.0, float(self.spec.width_mm), float(self.spec.height_mm))

    def trim_rect_mm(self) -> Rect:
        """The finished size (仕上がり): where the book is cut."""
        x, y = self.spec.trim_origin()
        w, h = self.spec.trim_size()
        return Rect(x, y, w, h)

    def bleed_rect_mm(self) -> Rect:
        """The finished size plus the bleed (裁ち落とし): how far art that runs off the page must reach."""
        t = self.trim_rect_mm()
        b = float(self.spec.bleed_mm)
        return Rect(t.x - b, t.y - b, t.width + 2 * b, t.height + 2 * b)

    def spread_step_mm(self) -> float:
        """How far right the right-hand page of a spread starts, in this page's coordinates, so that the two
        finished sizes meet at the gutter (the paper margins overlap, as the sheets are laid on each other)."""
        return self.trim_rect_mm().width

    def binding_edge(self, start_side: str | None = None) -> str:
        """Which edge of this page is at the binding (のど): "left" or "right"."""
        return "right" if self.side(start_side) == "left" else "left"

    def inner_rect_mm(self, start_side: str | None = None) -> Rect:
        """The basic frame (基本枠); the binding (のど) and fore-edge (小口) margins swap with the page's side."""
        t = self.trim_rect_mm()
        m = self.spec.margins()
        left, right = (m["inner"], m["outer"]) if self.binding_edge(start_side) == "left" else (m["outer"], m["inner"])
        return Rect(t.x + left, t.y + m["top"], t.width - left - right, t.height - m["top"] - m["bottom"])

    def is_recto(self) -> bool:
        return self.index % 2 == 1

    def side(self, start_side: str | None = None) -> str:
        """Physical side of the open book: "left" or "right".

        The first page sits on the side opposite the binding's reading start: in a
        right-bound (Japanese) book page 1 is on the left and spreads are (2, 3),
        (4, 5)… with the even page on the right; a left-bound book mirrors this.
        start_side overrides where page 1 sits.
        """
        first = start_side or ("left" if self.binding == Binding.RIGHT else "right")
        same = self.index % 2 == 1
        if same:
            return first
        return "right" if first == "left" else "left"

    def leaf_frames(self) -> list[Frame]:
        if not self.frames:
            return []
        return self._walk_leaves(self.frames[0])

    def _walk_leaves(self, frame: Frame) -> list[Frame]:
        if not frame.children:
            return [frame]
        from genko.frames import centroid, shape

        children = list(frame.children)
        # by the middle of each child (slanted panels' boxes overlap): right to left, top to bottom
        if frame.split_axis == "vertical":
            children.sort(key=lambda child: -centroid(shape(child))[0])
        else:
            children.sort(key=lambda child: centroid(shape(child))[1])
        out: list[Frame] = []
        for child in children:
            out.extend(self._walk_leaves(child))
        return out

    def _find(self, frame_id: str, node: Frame | None = None) -> Frame:
        node = node or self.frames[0]
        if node.id == frame_id:
            return node
        for child in node.children:
            try:
                return self._find(frame_id, child)
            except KeyError:
                continue
        raise KeyError(frame_id)

    def frame_at(self, x_mm: float, y_mm: float) -> Frame | None:
        from genko.frames import contains

        for frame in self.leaf_frames():
            if contains(frame, x_mm, y_mm):
                return frame
        return None

    def split_frame(
        self,
        frame_id: str,
        axis: str,
        ratio: float,
        gutter_mm: float,
    ) -> tuple[Frame, Frame]:
        target = self._find(frame_id)
        if target.children:
            raise ValueError("can only split a leaf frame")
        rect = target.rect
        if axis == "horizontal":
            available = rect.height - gutter_mm
            first_span = available * ratio
            second_span = available - first_span
            a = Frame(id=new_id(), rect=Rect(rect.x, rect.y, rect.width, first_span))
            b = Frame(
                id=new_id(),
                rect=Rect(rect.x, rect.y + first_span + gutter_mm, rect.width, second_span),
            )
        elif axis == "vertical":
            available = rect.width - gutter_mm
            first_span = available * ratio
            second_span = available - first_span
            a = Frame(id=new_id(), rect=Rect(rect.x, rect.y, first_span, rect.height))
            b = Frame(
                id=new_id(),
                rect=Rect(rect.x + first_span + gutter_mm, rect.y, second_span, rect.height),
            )
        else:
            raise ValueError(axis)
        target.children = [a, b]
        target.split_axis = axis
        return a, b

    def parent_of(self, frame_id: str, node: Frame | None = None) -> Frame | None:
        node = node or (self.frames[0] if self.frames else None)
        if node is None:
            return None
        for child in node.children:
            if child.id == frame_id:
                return node
            found = self.parent_of(frame_id, child)
            if found is not None:
                return found
        return None

    def merge_frame(self, frame_id: str) -> Frame:
        parent = self.parent_of(frame_id)
        if parent is None:
            raise ValueError("cannot merge the root frame")
        parent.children = []
        parent.split_axis = None
        return parent

    def resize_frame(self, frame_id: str, rect: Rect) -> Frame:
        target = self._find(frame_id)
        if target.children:
            raise ValueError("can only resize a leaf frame")
        target.rect = rect
        return target

    def paint(self, role: LayerRole, rgb: tuple[int, int, int]) -> None:
        self.fills[role] = rgb
        if role in (LayerRole.NAME, LayerRole.DRAFT):
            layer = self._layer(role)
            layer.kind = LayerKind.FILL
            layer.fill_rgb = rgb
            layer.exportable = False
        elif role in (LayerRole.INK, LayerRole.BG, LayerRole.FINISH):
            layer = self._layer(role)
            layer.kind = LayerKind.FILL
            layer.fill_rgb = rgb


TRANSIENT_FIELDS = frozenset({"undo_stack", "journal_pending", "asset_dir"})


@dataclass
class Episode:
    title: str
    episode: int
    spec: PageSpec
    binding: Binding
    pages: list[Page]
    story: list[StoryLine] = field(default_factory=list)
    bible: Bible = field(default_factory=Bible)
    undo_stack: list[Episode] = field(default_factory=list, repr=False, compare=False)
    tickets: list[dict] = field(default_factory=list)
    autosave: bool = False
    font_path: str = ""
    page_locks: dict = field(default_factory=dict)
    brush_rgb: tuple[int, int, int] = (20, 20, 20)
    brush_width_mm: float = 0.35
    brush_stabilize: int = 0
    brush_taper: bool = False
    brush_curve: str = "linear"
    brush_custom: dict = field(default_factory=dict)  # brushes people made, carried with the book (genko.brushes)
    nombre: dict = field(default_factory=dict)  # page numbers (genko.nombre)
    extra: dict = field(default_factory=dict)  # top-level keys this build does not know; written back unchanged
    revision: int = 0  # +1 on every save (v3)
    start_side: str | None = None  # "left" / "right" override for page 1; None = binding default
    strict_gates: bool = False  # studio projects: ink/raster edits need name_ok, spreads must face
    studio: dict = field(default_factory=dict)  # agent state: bible doc, script, locations, approvals, orphans…
    journal_pending: list = field(default_factory=list, repr=False, compare=False)  # ops since last save
    asset_dir: object = field(default=None, repr=False, compare=False)  # project folder for assets/ (not saved)

    def __deepcopy__(self, memo: dict) -> Episode:
        # The undo history is never copied: copying it made every op cost O(history x project).
        new = object.__new__(type(self))
        memo[id(self)] = new
        for f in dataclasses.fields(self):
            if f.name in TRANSIENT_FIELDS:
                continue
            setattr(new, f.name, copy.deepcopy(getattr(self, f.name), memo))
        new.undo_stack = []
        new.journal_pending = []
        new.asset_dir = self.asset_dir
        return new

    def reorder(self, order: list[int]) -> None:
        from genko.ops import remap_page_refs

        by_index = {page.index: page for page in self.pages}
        self.pages = [by_index[i] for i in order]
        remap_page_refs(self, {old: new for new, old in enumerate(order, start=1)})

    def add_line(
        self,
        page_index: int,
        text: str,
        speaker: str = "",
        frame_id: str | None = None,
        ruby: str = "",
        x_mm: float = 0,
        y_mm: float = 0,
        w_mm: float = 40,
        h_mm: float = 20,
        balloon: str = "speech",
        tail: tuple[float, float] | None = None,
    ) -> StoryLine:
        line = StoryLine(
            id=new_id(),
            page_index=page_index,
            text=text,
            speaker=speaker,
            frame_id=frame_id,
            ruby=ruby,
            x_mm=x_mm,
            y_mm=y_mm,
            w_mm=w_mm,
            h_mm=h_mm,
            balloon=balloon,
            tail=tail,
        )
        self.story.append(line)
        for page in self.pages:
            if page.index == page_index:
                page.texts.append(line)
                break
        return line

    def story_for_page(self, page_index: int) -> list[StoryLine]:
        from_story = [line for line in self.story if line.page_index == page_index]
        if from_story:
            return from_story
        for page in self.pages:
            if page.index == page_index:
                return page.texts
        return []


def new_episode(
    title: str,
    episode: int,
    page_count: int,
    spec: PageSpec,
    binding: Binding = Binding.RIGHT,
) -> Episode:
    pages: list[Page] = []
    for index in range(1, page_count + 1):
        page = Page(index=index, spec=spec, frames=[], binding=binding)
        root = Frame(id=new_id(), rect=page.inner_rect_mm())
        page.frames = [root]
        pages.append(page)
    return Episode(
        title=title,
        episode=episode,
        spec=spec,
        binding=binding,
        pages=pages,
    )
