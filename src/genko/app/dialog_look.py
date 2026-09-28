"""The dialogs made of the same parts as the main window (UI-G): a heading with one quiet line under it, sections of
rows on the left, a picture card on the right (what the choice will look like), and a footer whose one main
button is in the accent — no table of fields floating over an empty half."""

from __future__ import annotations

from PySide6.QtCore import QRectF, QSize, Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLayout,
    QVBoxLayout,
    QWidget,
)

from genko.app import theme


def header(title: str, subtitle: str = "") -> QWidget:
    box = QWidget()
    layout = QVBoxLayout(box)
    layout.setContentsMargins(0, 0, 0, 4)
    layout.setSpacing(2)
    heading = QLabel(title)
    theme.role(heading, "title")
    layout.addWidget(heading)
    if subtitle:
        note = QLabel(subtitle)
        note.setWordWrap(True)
        theme.role(note, "hint")
        layout.addWidget(note)
    return box


def section(title: str) -> QLabel:
    label = QLabel(title)
    theme.role(label, "section")
    return label


def form() -> QFormLayout:
    """Rows of a section: the names quiet and right-aligned, the fields growing."""
    rows = QFormLayout()
    rows.setLabelAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
    rows.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
    rows.setHorizontalSpacing(10)
    rows.setVerticalSpacing(6)
    return rows


def quiet_labels(rows: QFormLayout) -> None:
    """The row names in the quiet colour (the values carry the weight)."""
    for i in range(rows.rowCount()):
        item = rows.itemAt(i, QFormLayout.ItemRole.LabelRole)
        if item is not None and isinstance(item.widget(), QLabel):
            theme.role(item.widget(), "label")


def card(content: QWidget | QLayout, caption: str = "") -> QFrame:
    """A panel-faced card (the picture of what is being chosen)."""
    frame = QFrame()
    frame.setObjectName("dialogCard")
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(12, 12, 12, 12)
    layout.setSpacing(8)
    if caption:
        layout.addWidget(section(caption))
    if isinstance(content, QLayout):
        layout.addLayout(content)
    else:
        layout.addWidget(content, 1)
    return frame


def footer(buttons: QDialogButtonBox, note: QWidget | None = None) -> QWidget:
    """A line, then (a note on the left and) the buttons: the accept one in the accent."""
    box = QWidget()
    box.setObjectName("dialogFooter")
    layout = QHBoxLayout(box)
    layout.setContentsMargins(0, 10, 0, 0)
    if note is not None:
        layout.addWidget(note, 1)
    else:
        layout.addStretch(1)
    for role in (QDialogButtonBox.StandardButton.Ok, QDialogButtonBox.StandardButton.Save, QDialogButtonBox.StandardButton.Apply):
        button = buttons.button(role)
        if button is not None:
            theme.primary(button)
            button.setDefault(True)
            break
    layout.addWidget(buttons)
    return box


def frame(dialog: QDialog, head: QWidget, body: QWidget | QLayout, side: QWidget | None, foot: QWidget) -> None:
    """The dialog's whole layout: heading, the body with its card beside it, the footer."""
    outer = QVBoxLayout(dialog)
    outer.setContentsMargins(20, 18, 20, 16)
    outer.setSpacing(12)
    outer.addWidget(head)
    middle = QHBoxLayout()
    middle.setSpacing(18)
    if isinstance(body, QLayout):
        holder = QWidget()
        holder.setLayout(body)
        body = holder
    middle.addWidget(body, 3)
    if side is not None:
        middle.addWidget(side, 2)
    outer.addLayout(middle, 1)
    outer.addWidget(foot)
    theme.name_buttons(dialog)


class PaperDiagram(QWidget):
    """The sheet as it will be: the paper, the bleed (cut off), the finished size and the basic frame, to scale."""

    def __init__(self) -> None:
        super().__init__()
        self.spec = None
        self.error = False
        self.setMinimumSize(QSize(170, 220))

    def show_spec(self, spec, error: bool = False) -> None:
        self.spec, self.error = spec, error
        self.update()

    def paintEvent(self, _event) -> None:  # noqa: N802
        if self.spec is None:
            return
        t = theme.tokens()
        spec = self.spec
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pad = 10
        scale = min((self.width() - 2 * pad) / spec.width_mm, (self.height() - 2 * pad) / spec.height_mm)
        w, h = spec.width_mm * scale, spec.height_mm * scale
        ox, oy = (self.width() - w) / 2, (self.height() - h) / 2

        def box(x, y, bw, bh) -> QRectF:
            return QRectF(ox + x * scale, oy + y * scale, bw * scale, bh * scale)

        painter.fillRect(box(0, 0, spec.width_mm, spec.height_mm), QColor("#f4f4f2"))
        try:
            tx, ty = spec.trim_origin()
            tw, th = spec.trim_size()
            b = spec.bleed_mm
            m = spec.margins()
        except Exception:
            return
        painter.fillRect(box(tx - b, ty - b, tw + 2 * b, th + 2 * b), QColor("#e3e6ea"))
        painter.fillRect(box(tx, ty, tw, th), QColor("#ffffff"))
        painter.setPen(QPen(QColor(200, 60, 120, 200), 1))
        painter.drawRect(box(tx, ty, tw, th))
        painter.setPen(QPen(QColor(40, 126, 214, 200), 1, Qt.PenStyle.DashLine))
        painter.drawRect(box(tx + m["outer"], ty + m["top"], tw - m["outer"] - m["inner"], th - m["top"] - m["bottom"]))
        painter.setPen(QPen(QColor(t.danger if self.error else t.border), 1))
        painter.drawRect(box(0, 0, spec.width_mm, spec.height_mm))
        painter.end()


def legend() -> QWidget:
    """What the lines in the paper picture mean."""
    box = QWidget()
    rows = QVBoxLayout(box)
    rows.setContentsMargins(0, 0, 0, 0)
    rows.setSpacing(2)
    for colour, style, words in (("#c83c78", "solid", "仕上がり（ここで切られる）"), ("#287ed6", "dash", "基本枠（コマを収める目安）"),
                                 ("#e3e6ea", "fill", "裁ち落とし（切られる帯）")):
        mark = "━" if style == "solid" else ("┅" if style == "dash" else "■")
        label = QLabel(f"<span style='color:{colour}'>{mark}</span>　{words}")
        theme.role(label, "hint")
        rows.addWidget(label)
    return box
