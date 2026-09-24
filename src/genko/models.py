from __future__ import annotations

import copy
import dataclasses
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
    width_mm: float
    height_mm: float
    dpi: int
    bleed_mm: float
    inner_margin_mm: float
    expression: str = "mono"
    preset: str | None = None

    @staticmethod
    def a4_mono() -> PageSpec:
        return PageSpec(210, 297, 600, 3, 10, "mono")

    @staticmethod
    def webtoon() -> PageSpec:
        return PageSpec(80, 400, 300, 0, 4, "color")

    @staticmethod
    def b4_comic() -> PageSpec:
        return PageSpec(257, 364, 600, 3, 10, "mono", preset="commercial-b4")

    @staticmethod
    def publisher(name: str) -> PageSpec:
        key = name.strip().lower()
        known = {"shueisha", "kodansha", "kadokawa", "shogakukan"}
        preset = key if key in known else "none"
        return PageSpec(257, 364, 600, 3, 10, "mono", preset=preset)


@dataclass
class Stroke:
    id: str
    points: list[tuple[float, float]] = field(default_factory=list)
    pressure: list[float] = field(default_factory=list)
    width_mm: float = 0.35
    kind: str = "gpen"
    handles: list | None = None


def coerce_stroke(raw) -> Stroke:
    if isinstance(raw, Stroke):
        return raw
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
        return {
            "id": stroke.id,
            "points": stroke.points,
            "pressure": stroke.pressure,
            "width_mm": stroke.width_mm,
            "kind": stroke.kind,
        }
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
    parent_id: str | None = None
    asset: str | None = None  # placed: "sha256:…" in assets/
    frame_id: str | None = None  # placed: the panel it belongs to
    placement_mm: Rect | None = None  # placed: where the whole image lands on the page
    fit: str = "cover"  # cover | contain | stretch
    clip_to: str = "frame"  # frame | bleed | none
    source: dict | None = None  # placed: {"candidate": …, "request": …}
    finish: dict | None = None  # placed: mono finishing override (M6)


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
    path: list | None = None


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
    ruler: dict | None = None
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

    def inner_rect_mm(self) -> Rect:
        inset = self.spec.bleed_mm + self.spec.inner_margin_mm
        return Rect(
            x=inset,
            y=inset,
            width=self.spec.width_mm - 2 * inset,
            height=self.spec.height_mm - 2 * inset,
        )

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
        children = list(frame.children)
        if frame.split_axis == "vertical":
            children.sort(key=lambda child: -child.rect.x)
        else:
            children.sort(key=lambda child: child.rect.y)
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
        for frame in self.leaf_frames():
            if frame.rect.contains(x_mm, y_mm):
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
