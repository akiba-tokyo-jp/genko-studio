from __future__ import annotations

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


class LayerKind(str, Enum):
    RASTER = "raster"
    STROKES = "strokes"
    FILL = "fill"
    TONE = "tone"


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


@dataclass
class Layer:
    id: str
    role: LayerRole
    kind: LayerKind = LayerKind.STROKES
    visible: bool = True
    exportable: bool = True
    strokes: list[list[tuple[float, float]]] = field(default_factory=list)
    raster_relpath: str | None = None
    fill_rgb: tuple[int, int, int] | None = None


@dataclass
class Frame:
    id: str
    rect: Rect
    children: list[Frame] = field(default_factory=list)
    split_axis: str | None = None
    clip: bool = True
    bleed: bool = False
    border_mm: float = 0.8


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
    def name_strokes(self) -> list[list[tuple[float, float]]]:
        return self._layer(LayerRole.NAME).strokes

    @name_strokes.setter
    def name_strokes(self, value: list[list[tuple[float, float]]]) -> None:
        self._layer(LayerRole.NAME).strokes = value

    @property
    def ink_strokes(self) -> list[list[tuple[float, float]]]:
        return self._layer(LayerRole.INK).strokes

    @ink_strokes.setter
    def ink_strokes(self, value: list[list[tuple[float, float]]]) -> None:
        self._layer(LayerRole.INK).strokes = value

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

    def reorder(self, order: list[int]) -> None:
        by_index = {page.index: page for page in self.pages}
        self.pages = [by_index[i] for i in order]
        mapping = {page.index: new for new, page in enumerate(self.pages, start=1)}
        for line in self.story:
            line.page_index = mapping[line.page_index]
        for new, page in enumerate(self.pages, start=1):
            page.index = new
            for line in page.texts:
                line.page_index = new

    def add_line(
        self,
        page_index: int,
        text: str,
        speaker: str = "",
        frame_id: str | None = None,
    ) -> StoryLine:
        line = StoryLine(
            id=new_id(),
            page_index=page_index,
            text=text,
            speaker=speaker,
            frame_id=frame_id,
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
