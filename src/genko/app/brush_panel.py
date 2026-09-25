"""The brush panel: pen kind, size, opacity, steadiness, taper, pressure, colour; the fill and eraser settings.

The settings live in the app (QSettings) and travel with every line as op fields, so a line keeps
the look it was drawn with.
"""

from __future__ import annotations

from PySide6.QtCore import QSettings, Qt, Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSlider,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from genko.brushes import BRUSHES, DEFAULT

SIZES = [0.2, 0.3, 0.5, 0.8, 1.2, 2.0, 3.0, 5.0, 10.0]
MONO = [(20, 20, 20), (64, 64, 64), (128, 128, 128), (192, 192, 192), (255, 255, 255)]
COLOURS = [(220, 50, 50), (240, 140, 40), (250, 210, 60), (90, 170, 80), (50, 140, 200), (70, 80, 190), (150, 80, 180),
           (240, 170, 180), (160, 110, 70), (250, 225, 200)]
PRESSURE = [("やわらかい", 0.7), ("ふつう", 1.0), ("かたい", 1.6)]


class BrushPanel(QWidget):
    changed = Signal()

    def __init__(self) -> None:
        super().__init__()
        self.settings = QSettings("Genko", "Genko Studio")
        self.kinds = QListWidget()
        for key, brush in BRUSHES.items():
            item = QListWidgetItem(brush.label)
            item.setData(Qt.ItemDataRole.UserRole, key)
            self.kinds.addItem(item)
        self.kinds.setMaximumHeight(170)
        self.kinds.currentRowChanged.connect(lambda _: self._kind_changed())
        self.size = QDoubleSpinBox()
        self.size.setMaximumWidth(110)
        self.size.setRange(0.05, 50)
        self.size.setSingleStep(0.1)
        self.size.setDecimals(2)
        self.size.setSuffix(" mm")
        self.size.valueChanged.connect(lambda _: self._save())
        sizes = QGridLayout()
        sizes.setSpacing(2)
        for i, value in enumerate(SIZES):
            button = QPushButton(f"{value:g}")
            button.setFixedWidth(34)
            button.setToolTip(f"{value:g} mm")
            button.clicked.connect(lambda _=False, v=value: self.size.setValue(v))
            sizes.addWidget(button, i // 5, i % 5)
        self.opacity = QSlider(Qt.Orientation.Horizontal)
        self.opacity.setRange(5, 100)
        self.opacity.valueChanged.connect(lambda _: self._save())
        self.steady = QSpinBox()
        self.steady.setMaximumWidth(110)
        self.steady.setRange(0, 15)
        self.steady.setToolTip("大きいほど手ぶれを抑える（線が少し遅れて付いてくる）")
        self.steady.valueChanged.connect(lambda _: self._save())
        self.taper = QCheckBox("入り抜き")
        self.taper.setToolTip("線の両端を細くします")
        self.taper.toggled.connect(lambda _: self._save())
        self.pressure = QComboBox()
        self.pressure.setToolTip("やわらかい: 弱い力でも太く。かたい: 強く押したときだけ太く")
        for label, gamma in PRESSURE:
            self.pressure.addItem(label, gamma)
        self.pressure.currentIndexChanged.connect(lambda _: self._save())
        # colour
        self.swatch = QPushButton()
        self.swatch.setFixedSize(46, 30)
        self.swatch.clicked.connect(self._pick)
        palette = QGridLayout()
        for i, rgb in enumerate(MONO + COLOURS):
            button = QPushButton()
            button.setFixedSize(22, 22)
            button.setStyleSheet(f"background: rgb{rgb}; border: 1px solid #888")
            button.setToolTip("白" if rgb == (255, 255, 255) else ("黒" if rgb == (20, 20, 20) else ""))
            button.clicked.connect(lambda _=False, c=rgb: self.set_colour(c))
            palette.addWidget(button, i // 5, i % 5)
        self.rgb = (20, 20, 20)
        # fill
        self.gap = QDoubleSpinBox()
        self.gap.setMaximumWidth(110)
        self.gap.setRange(0, 5)
        self.gap.setSingleStep(0.1)
        self.gap.setSuffix(" mm")
        self.gap.setToolTip("線の切れ目がこの幅までなら、閉じているとみなして塗る")
        self.gap.valueChanged.connect(lambda _: self._save())
        self.reference = QComboBox()
        self.reference.addItem("見えている全部", "page")
        self.reference.addItem("このレイヤーだけ", "layer")
        self.reference.currentIndexChanged.connect(lambda _: self._save())
        self.crossing = QCheckBox("消しゴムで交点まで消す")
        self.crossing.setToolTip("線の交わる所までを一度に消します（はみ出しの掃除）")
        self.crossing.toggled.connect(lambda _: self._save())
        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.addRow("太さ", self.size)
        form.addRow(self._wrap(sizes))
        form.addRow("不透明度", self.opacity)
        form.addRow("手ぶれ補正", self.steady)
        form.addRow(self.taper)
        form.addRow("筆圧", self.pressure)
        palette.setSpacing(3)
        colour_row = QHBoxLayout()
        colour_row.addWidget(self.swatch, 0, Qt.AlignmentFlag.AlignTop)
        colour_row.addLayout(palette)
        colour_row.addStretch(1)
        fill_form = QFormLayout()
        fill_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapAllRows)
        fill_form.addRow("隙間を閉じる", self.gap)
        fill_form.addRow("見る範囲", self.reference)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(QLabel("ペンの種類"))
        layout.addWidget(self.kinds)
        layout.addLayout(form)
        layout.addWidget(QLabel("色（スポイト I で拾う）"))
        layout.addLayout(colour_row)
        layout.addWidget(QLabel("塗りつぶし（G）"))
        layout.addLayout(fill_form)
        layout.addWidget(self.crossing)
        layout.addStretch(1)
        self._loading = True
        self._load()
        self._loading = False

    @staticmethod
    def _wrap(inner) -> QWidget:
        box = QWidget()
        box.setLayout(inner)
        inner.setContentsMargins(0, 0, 0, 0)
        return box

    # --- state ------------------------------------------------------------------------------------------

    def kind(self) -> str:
        item = self.kinds.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else DEFAULT

    def _load(self) -> None:
        kind = str(self.settings.value("brush/kind", DEFAULT))
        keys = list(BRUSHES)
        self.kinds.setCurrentRow(keys.index(kind) if kind in keys else 0)
        self._apply_kind_defaults(kind, stored=True)
        rgb = self.settings.value("brush/rgb", "20,20,20")
        try:
            self.set_colour(tuple(int(v) for v in str(rgb).split(",")))
        except ValueError:
            self.set_colour((20, 20, 20))
        self.gap.setValue(float(self.settings.value("fill/gap", 0.3)))
        self.reference.setCurrentIndex(max(0, self.reference.findData(self.settings.value("fill/reference", "page"))))
        self.crossing.setChecked(str(self.settings.value("eraser/crossing", "false")) == "true")

    def _apply_kind_defaults(self, kind: str, stored: bool = False) -> None:
        brush = BRUSHES[kind]
        prefix = f"brush/{kind}/"
        self.size.setValue(float(self.settings.value(prefix + "size", brush.width_mm)) if stored else brush.width_mm)
        self.opacity.setValue(int(float(self.settings.value(prefix + "opacity", brush.opacity)) * 100) if stored else int(brush.opacity * 100))
        self.steady.setValue(int(self.settings.value(prefix + "steady", brush.stabilize)) if stored else brush.stabilize)
        self.taper.setChecked(str(self.settings.value(prefix + "taper", brush.taper)).lower() == "true" if stored else brush.taper)
        gamma = float(self.settings.value(prefix + "pressure", 1.0)) if stored else 1.0
        self.pressure.setCurrentIndex(max(0, self.pressure.findData(gamma)))

    def _kind_changed(self) -> None:
        if self._loading:
            return
        self._loading = True
        self._apply_kind_defaults(self.kind(), stored=True)
        self._loading = False
        self._save()

    def _save(self) -> None:
        if self._loading:
            return
        kind = self.kind()
        prefix = f"brush/{kind}/"
        self.settings.setValue("brush/kind", kind)
        self.settings.setValue(prefix + "size", self.size.value())
        self.settings.setValue(prefix + "opacity", self.opacity.value() / 100)
        self.settings.setValue(prefix + "steady", self.steady.value())
        self.settings.setValue(prefix + "taper", self.taper.isChecked())
        self.settings.setValue(prefix + "pressure", self.pressure.currentData())
        self.settings.setValue("fill/gap", self.gap.value())
        self.settings.setValue("fill/reference", self.reference.currentData())
        self.settings.setValue("eraser/crossing", self.crossing.isChecked())
        self.changed.emit()

    def set_colour(self, rgb) -> None:
        self.rgb = tuple(int(v) for v in rgb)[:3]
        self.swatch.setStyleSheet(f"background: rgb{self.rgb}; border: 2px solid #333")
        self.swatch.setToolTip(f"今の色 {self.rgb}")
        self.settings.setValue("brush/rgb", ",".join(str(v) for v in self.rgb))
        self.changed.emit()

    def _pick(self) -> None:
        colour = QColorDialog.getColor(QColor(*self.rgb), self, "色")
        if colour.isValid():
            self.set_colour((colour.red(), colour.green(), colour.blue()))

    def nudge_size(self, step: int) -> float:
        value = self.size.value()
        i = min(range(len(SIZES)), key=lambda k: abs(SIZES[k] - value))
        self.size.setValue(SIZES[max(0, min(len(SIZES) - 1, i + step))])
        return self.size.value()

    def stroke_fields(self) -> dict:
        """What a new line carries (add_stroke fields)."""
        brush = BRUSHES[self.kind()]
        out = {"kind": self.kind(), "width_mm": round(self.size.value(), 3), "stabilize": self.steady.value(),
               "taper": self.taper.isChecked()}
        if brush.rgb is None:
            out["rgb"] = list(self.rgb)
        opacity = self.opacity.value() / 100
        if abs(opacity - brush.opacity) > 1e-6:
            out["opacity"] = round(opacity / max(brush.opacity, 0.01), 3)
        if self.pressure.currentData() != 1.0:
            out["pressure_gamma"] = self.pressure.currentData()
        return out
