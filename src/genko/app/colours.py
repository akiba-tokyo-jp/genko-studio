"""カラー: the colour panel (J4) — a square of saturation and brightness with a hue bar, RGB and hex,
the main and sub colours (X swaps them) and the transparent colour, colour sets, the colours used lately,
colours between four chosen ones (中間色) and colours near the current one (近似色).
"""

from __future__ import annotations

import colorsys
import json

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QLinearGradient, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from genko.app.preferences import settings

HISTORY = 24
BUILT_IN_SETS = {
    "マンガのグレー": [(v, v, v) for v in (0, 25, 51, 76, 102, 128, 153, 178, 204, 229, 255)],
    "基本の色": [tuple(round(c * 255) for c in colorsys.hsv_to_rgb(h / 12, s, v)) for s, v in ((1, 1), (0.55, 1), (1, 0.6))
                 for h in range(12)],
    "肌・髪": [(255, 224, 196), (247, 206, 170), (234, 184, 146), (205, 146, 110), (160, 105, 75), (110, 70, 50),
               (40, 30, 25), (90, 60, 40), (150, 100, 50), (220, 180, 100), (240, 220, 160), (200, 60, 50)],
}


def _config_path():
    from genko.tokens import config_dir

    return config_dir() / "colorsets.json"


def load_sets() -> dict[str, list[tuple[int, int, int]]]:
    """The built-in sets and the person's own (the settings folder's colorsets.json)."""
    out = {k: list(v) for k, v in BUILT_IN_SETS.items()}
    try:
        data = json.loads(_config_path().read_text(encoding="utf-8"))
        for name, colours in (data.get("sets") or {}).items():
            out[str(name)] = [tuple(int(v) for v in c)[:3] for c in colours]
    except (OSError, ValueError, TypeError):
        pass
    return out


def save_set(name: str, colours: list) -> None:
    path = _config_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {}
    sets = data.get("sets") or {}
    if colours is None:
        sets.pop(name, None)
    else:
        sets[name] = [list(c) for c in colours]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"sets": sets}, ensure_ascii=False, indent=1), encoding="utf-8")


def mix(a, b, t: float) -> tuple[int, int, int]:
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))  # type: ignore[return-value]


def between(corners, n: int = 5) -> list[list[tuple[int, int, int]]]:
    """中間色: an n×n grid mixed from four corners (top-left, top-right, bottom-left, bottom-right)."""
    tl, tr, bl, br = corners
    grid = []
    for row in range(n):
        v = row / (n - 1)
        left, right = mix(tl, bl, v), mix(tr, br, v)
        grid.append([mix(left, right, col / (n - 1)) for col in range(n)])
    return grid


def near(rgb, n: int = 5) -> list[list[tuple[int, int, int]]]:
    """近似色: rows of the colour with its hue turned a little, and lighter / darker, more / less vivid."""
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    rows = []
    for dy in range(n):
        row = []
        for dx in range(n):
            hh = (h + (dx - n // 2) * 0.03) % 1
            ss = max(0.0, min(1.0, s + (dy - n // 2) * 0.12))
            vv = max(0.0, min(1.0, v - (dy - n // 2) * 0.08 + (dx - n // 2) * 0.02))
            row.append(tuple(round(c * 255) for c in colorsys.hsv_to_rgb(hh, ss, vv)))
        rows.append(row)
    return rows


class Swatch(QPushButton):
    picked = Signal(object)

    def __init__(self, rgb=(0, 0, 0), size: int = 18) -> None:
        super().__init__()
        self.setFixedSize(size, size)
        self.set_rgb(rgb)
        self.clicked.connect(lambda: self.picked.emit(self.rgb))

    def set_rgb(self, rgb) -> None:
        self.rgb = tuple(int(v) for v in rgb)[:3]
        self.setStyleSheet(f"background: rgb{self.rgb}; border: 1px solid #888; border-radius: 2px")
        self.setToolTip(f"RGB {self.rgb}")


class SVSquare(QWidget):
    """Saturation across, brightness up and down, for the hue chosen on the bar."""

    changed = Signal(float, float)

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(120, 110)
        self.hue, self.sat, self.val = 0.0, 1.0, 1.0

    def paintEvent(self, event) -> None:  # noqa: N802
        p = QPainter(self)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        across = QLinearGradient(r.topLeft(), r.topRight())
        across.setColorAt(0, QColor("white"))
        across.setColorAt(1, QColor.fromHsvF(self.hue, 1, 1))
        p.fillRect(r, across)
        down = QLinearGradient(r.topLeft(), r.bottomLeft())
        down.setColorAt(0, QColor(0, 0, 0, 0))
        down.setColorAt(1, QColor(0, 0, 0, 255))
        p.fillRect(r, down)
        x, y = r.left() + self.sat * r.width(), r.top() + (1 - self.val) * r.height()
        p.setPen(QPen(QColor("white" if self.val < 0.6 else "black"), 1.5))
        p.drawEllipse(QPointF(x, y), 5, 5)

    def _pick(self, pos) -> None:
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        self.sat = max(0.0, min(1.0, (pos.x() - r.left()) / r.width()))
        self.val = max(0.0, min(1.0, 1 - (pos.y() - r.top()) / r.height()))
        self.update()
        self.changed.emit(self.sat, self.val)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        self._pick(event.position())

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if event.buttons() & Qt.MouseButton.LeftButton:
            self._pick(event.position())


class HueBar(QWidget):
    changed = Signal(float)

    def __init__(self) -> None:
        super().__init__()
        self.setFixedWidth(18)
        self.setMinimumHeight(110)
        self.hue = 0.0

    def paintEvent(self, event) -> None:  # noqa: N802
        p = QPainter(self)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        g = QLinearGradient(r.topLeft(), r.bottomLeft())
        for k in range(7):
            g.setColorAt(k / 6, QColor.fromHsvF((k / 6) % 1, 1, 1))
        p.fillRect(r, g)
        y = r.top() + self.hue * r.height()
        p.setPen(QPen(QColor("black"), 2))
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))

    def _pick(self, pos) -> None:
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        self.hue = max(0.0, min(0.999, (pos.y() - r.top()) / r.height()))
        self.update()
        self.changed.emit(self.hue)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        self._pick(event.position())

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if event.buttons() & Qt.MouseButton.LeftButton:
            self._pick(event.position())


def _grid(rows, on_pick) -> QWidget:
    box = QWidget()
    grid = QGridLayout(box)
    grid.setSpacing(2)
    grid.setContentsMargins(0, 0, 0, 0)
    for r, row in enumerate(rows):
        for c, rgb in enumerate(row):
            swatch = Swatch(rgb)
            swatch.picked.connect(on_pick)
            grid.addWidget(swatch, r, c)
    return box


class ColourPanel(QWidget):
    """The pen's colour, chosen every way a painter chooses one."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self._loading = False
        store = settings()
        sub = str(store.value("colour/sub", "255,255,255"))
        self.sub_rgb = tuple(int(v) for v in sub.split(","))[:3] if sub.count(",") == 2 else (255, 255, 255)
        history = str(store.value("colour/history", "") or "")
        self.history = [tuple(int(v) for v in item.split(","))[:3] for item in history.split(";") if item.count(",") == 2]
        corners = str(store.value("colour/corners", "") or "")
        self.corners = [tuple(int(v) for v in item.split(","))[:3] for item in corners.split(";") if item.count(",") == 2]
        if len(self.corners) != 4:
            self.corners = [(255, 255, 255), (230, 60, 60), (60, 90, 220), (20, 20, 20)]
        # main / sub / transparent
        self.main = Swatch(window.brush.rgb, 30)
        self.main.setToolTip("メインの色（今の色）")
        self.sub = Swatch(self.sub_rgb, 22)
        self.sub.setToolTip("サブの色（X で入れ替え）")
        self.sub.picked.connect(lambda _: self.swap())
        swap = QPushButton("⇄")
        swap.setFixedWidth(28)
        swap.setToolTip("メインとサブを入れ替える（X）")
        swap.clicked.connect(self.swap)
        self.transparent = QCheckBox("透明色")
        self.transparent.toggled.connect(lambda on: hasattr(window, "act_transparent") and window.act_transparent.setChecked(on))
        self.transparent.setToolTip("透明色で描く: 描いた所が消える（ペンのまま消しゴムになる）")
        top = QHBoxLayout()
        top.addWidget(self.main)
        top.addWidget(self.sub)
        top.addWidget(swap)
        top.addWidget(self.transparent)
        top.addStretch(1)
        # the square and the numbers
        self.square = SVSquare()
        self.bar = HueBar()
        self.square.changed.connect(lambda *_: self._from_square())
        self.bar.changed.connect(lambda h: self._from_hue(h))
        picker = QHBoxLayout()
        picker.addWidget(self.square, 1)
        picker.addWidget(self.bar)
        self.spins = []
        numbers = QHBoxLayout()
        numbers.setSpacing(2)
        for label in ("R", "G", "B"):
            spin = QSpinBox()
            spin.setRange(0, 255)
            spin.setMaximumWidth(52)
            spin.setButtonSymbols(QSpinBox.ButtonSymbols.NoButtons)
            spin.valueChanged.connect(lambda _: self._from_numbers())
            numbers.addWidget(QLabel(label))
            numbers.addWidget(spin)
            self.spins.append(spin)
        self.hex = QLineEdit()
        self.hex.setMaximumWidth(70)
        self.hex.setToolTip("#RRGGBB")
        self.hex.editingFinished.connect(self._from_hex)
        numbers.addWidget(self.hex)
        # sets, history, between, near
        self.tabs = QTabWidget()
        self.set_choice = QComboBox()
        self.set_choice.activated.connect(lambda _: self._fill_set())
        self.set_box = QWidget()
        self.set_grid = QGridLayout(self.set_box)
        self.set_grid.setSpacing(2)
        add = QPushButton("今の色を足す")
        add.clicked.connect(self.add_to_set)
        new = QPushButton("新しいセット…")
        new.clicked.connect(self.new_set)
        sets_page = QWidget()
        sl = QVBoxLayout(sets_page)
        sl.setContentsMargins(2, 2, 2, 2)
        sl.addWidget(self.set_choice)
        sl.addWidget(self.set_box)
        row = QHBoxLayout()
        row.addWidget(add)
        row.addWidget(new)
        sl.addLayout(row)
        sl.addStretch(1)
        self.history_page = QWidget()
        self.history_grid = QGridLayout(self.history_page)
        self.history_grid.setSpacing(2)
        self.between_page = QWidget()
        self.between_layout = QVBoxLayout(self.between_page)
        self.between_layout.setContentsMargins(2, 2, 2, 2)
        self.near_page = QWidget()
        self.near_layout = QVBoxLayout(self.near_page)
        self.near_layout.setContentsMargins(2, 2, 2, 2)
        self.tabs.addTab(sets_page, "セット")
        self.tabs.addTab(self.history_page, "履歴")
        self.tabs.addTab(self.between_page, "中間色")
        self.tabs.addTab(self.near_page, "近似色")
        self.pick_source = QComboBox()
        self.pick_source.addItem("スポイト: 見えている色", "view")
        self.pick_source.addItem("スポイト: 描く先のレイヤーの色", "layer")
        self.pick_source.activated.connect(lambda _: setattr(window.canvas, "pick_source", self.pick_source.currentData()))
        layout = QVBoxLayout(self)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.addLayout(top)
        layout.addLayout(picker)
        layout.addLayout(numbers)
        layout.addWidget(self.pick_source)
        layout.addWidget(self.tabs, 1)
        self.sets = load_sets()
        self._fill_sets()
        self._fill_history()
        self._fill_between()
        self.show_colour(window.brush.rgb)
        window.brush.changed.connect(lambda: self.show_colour(window.brush.rgb))

    # --- the colour --------------------------------------------------------------------------------------

    @property
    def rgb(self) -> tuple[int, int, int]:
        return tuple(self.window.brush.rgb)  # type: ignore[return-value]

    def choose(self, rgb) -> None:
        self.window.brush.set_colour(rgb)
        self.transparent.setChecked(False)

    def show_colour(self, rgb) -> None:
        rgb = tuple(int(v) for v in rgb)[:3]
        self._loading = True
        self.main.set_rgb(rgb)
        h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
        if s > 0 and v > 0:
            self.square.hue = self.bar.hue = h
        self.square.sat, self.square.val = s, v
        self.square.update()
        self.bar.update()
        for spin, value in zip(self.spins, rgb):
            spin.setValue(value)
        self.hex.setText("#{:02X}{:02X}{:02X}".format(*rgb))
        self._fill_near(rgb)
        self._loading = False

    def _from_square(self) -> None:
        if not self._loading:
            self.choose(tuple(round(c * 255) for c in colorsys.hsv_to_rgb(self.bar.hue, self.square.sat, self.square.val)))

    def _from_hue(self, h: float) -> None:
        self.square.hue = h
        self.square.update()
        self._from_square()

    def _from_numbers(self) -> None:
        if not self._loading:
            self.choose(tuple(spin.value() for spin in self.spins))

    def _from_hex(self) -> None:
        text = self.hex.text().strip().lstrip("#")
        if len(text) == 6:
            try:
                self.choose(tuple(int(text[i:i + 2], 16) for i in (0, 2, 4)))
            except ValueError:
                pass

    def swap(self) -> None:
        new_main = self.sub_rgb
        self.sub_rgb = self.rgb
        self.sub.set_rgb(self.sub_rgb)
        settings().setValue("colour/sub", ",".join(str(v) for v in self.sub_rgb))
        self.choose(new_main)

    def remember(self, rgb) -> None:
        """A colour just used goes to the front of 履歴."""
        rgb = tuple(int(v) for v in rgb)[:3]
        self.history = [rgb] + [c for c in self.history if c != rgb]
        del self.history[HISTORY:]
        settings().setValue("colour/history", ";".join(",".join(str(v) for v in c) for c in self.history))
        self._fill_history()

    # --- the pages ---------------------------------------------------------------------------------------

    def _fill_sets(self) -> None:
        current = self.set_choice.currentText()
        self.set_choice.clear()
        for name in self.sets:
            self.set_choice.addItem(name)
        if current:
            self.set_choice.setCurrentText(current)
        self._fill_set()

    def _fill_set(self) -> None:
        while self.set_grid.count():
            item = self.set_grid.takeAt(0)
            if item.widget():
                item.widget().setParent(None)
        for n, rgb in enumerate(self.sets.get(self.set_choice.currentText(), [])):
            swatch = Swatch(rgb)
            swatch.picked.connect(self.choose)
            self.set_grid.addWidget(swatch, n // 8, n % 8)

    def add_to_set(self) -> None:
        name = self.set_choice.currentText()
        if name in BUILT_IN_SETS:  # (the built-in sets stay as they are: a copy becomes one's own)
            name = f"{name}（自分）"
            self.sets[name] = list(self.sets[self.set_choice.currentText()])
        self.sets[name] = [*self.sets.get(name, []), self.rgb]
        save_set(name, self.sets[name])
        self._fill_sets()
        self.set_choice.setCurrentText(name)
        self._fill_set()

    def new_set(self, name: str | None = None) -> None:
        if name is None:
            name, ok = QInputDialog.getText(self, "カラーセット", "新しいセットの名前")
            if not ok or not name.strip():
                return
        name = name.strip()
        self.sets[name] = [self.rgb]
        save_set(name, self.sets[name])
        self._fill_sets()
        self.set_choice.setCurrentText(name)
        self._fill_set()

    def _fill_history(self) -> None:
        while self.history_grid.count():
            item = self.history_grid.takeAt(0)
            if item.widget():
                item.widget().setParent(None)
        for n, rgb in enumerate(self.history):
            swatch = Swatch(rgb)
            swatch.picked.connect(self.choose)
            self.history_grid.addWidget(swatch, n // 8, n % 8)

    def _fill_between(self) -> None:
        while self.between_layout.count():
            item = self.between_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)
        self.between_layout.addWidget(_grid(between(self.corners), self.choose))
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(2)
        holder = QWidget()
        holder.setLayout(row)
        for k, label in enumerate(("左上", "右上", "左下", "右下")):
            button = QPushButton(label)
            button.setMinimumWidth(30)
            button.setToolTip("今の色をこの角にする")
            button.clicked.connect(lambda _=False, i=k: self.set_corner(i))
            row.addWidget(button)
        self.between_layout.addWidget(holder)

    def set_corner(self, i: int) -> None:
        self.corners[i] = self.rgb
        settings().setValue("colour/corners", ";".join(",".join(str(v) for v in c) for c in self.corners))
        self._fill_between()

    def _fill_near(self, rgb) -> None:
        while self.near_layout.count():
            item = self.near_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)
        self.near_layout.addWidget(_grid(near(rgb), self.choose))
