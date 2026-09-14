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
    FRAMES = "frames"
    TEXT = "text"


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    width: float
    height: float


@dataclass(frozen=True)
class PageSpec:
    width_mm: float
    height_mm: float
    dpi: int
    bleed_mm: float
    inner_margin_mm: float
    expression: str = "mono"

    @staticmethod
    def a4_mono() -> PageSpec:
        return PageSpec(
            width_mm=210,
            height_mm=297,
            dpi=600,
            bleed_mm=3,
            inner_margin_mm=10,
            expression="mono",
        )

    @staticmethod
    def webtoon() -> PageSpec:
        return PageSpec(
            width_mm=80,
            height_mm=400,
            dpi=300,
            bleed_mm=0,
            inner_margin_mm=4,
            expression="color",
        )


@dataclass
class Frame:
    id: str
    rect: Rect
    children: list[Frame] = field(default_factory=list)
    split_axis: str | None = None


@dataclass
class StoryLine:
    id: str
    page_index: int
    text: str
    speaker: str = ""
    frame_id: str | None = None


def _new_id() -> str:
    return uuid4().hex[:12]


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
    name_strokes: list[list[tuple[float, float]]] = field(default_factory=list)
    ink_strokes: list[list[tuple[float, float]]] = field(default_factory=list)

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
            a = Frame(
                id=_new_id(),
                rect=Rect(rect.x, rect.y, rect.width, first_span),
            )
            b = Frame(
                id=_new_id(),
                rect=Rect(rect.x, rect.y + first_span + gutter_mm, rect.width, second_span),
            )
        elif axis == "vertical":
            available = rect.width - gutter_mm
            first_span = available * ratio
            second_span = available - first_span
            a = Frame(
                id=_new_id(),
                rect=Rect(rect.x, rect.y, first_span, rect.height),
            )
            b = Frame(
                id=_new_id(),
                rect=Rect(rect.x + first_span + gutter_mm, rect.y, second_span, rect.height),
            )
        else:
            raise ValueError(axis)
        target.children = [a, b]
        target.split_axis = axis
        return a, b

    def paint(self, role: LayerRole, rgb: tuple[int, int, int]) -> None:
        self.fills[role] = rgb


@dataclass
class Episode:
    title: str
    episode: int
    spec: PageSpec
    binding: Binding
    pages: list[Page]
    story: list[StoryLine] = field(default_factory=list)

    def reorder(self, order: list[int]) -> None:
        by_index = {page.index: page for page in self.pages}
        self.pages = [by_index[i] for i in order]
        for i, page in enumerate(self.pages, start=1):
            page.index = i

    def add_line(
        self,
        page_index: int,
        text: str,
        speaker: str = "",
        frame_id: str | None = None,
    ) -> StoryLine:
        line = StoryLine(
            id=_new_id(),
            page_index=page_index,
            text=text,
            speaker=speaker,
            frame_id=frame_id,
        )
        self.story.append(line)
        return line

    def story_for_page(self, page_index: int) -> list[StoryLine]:
        return [line for line in self.story if line.page_index == page_index]


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
        root = Frame(id=_new_id(), rect=page.inner_rect_mm())
        page.frames = [root]
        pages.append(page)
    return Episode(
        title=title,
        episode=episode,
        spec=spec,
        binding=binding,
        pages=pages,
    )
