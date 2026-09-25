"""The brush panel: pen kind, size, opacity, steadiness, taper, pressure, colour; the fill and eraser settings.

The settings live in the app (QSettings) and travel with every line as op fields, so a line keeps
the look it was drawn with.
"""

from __future__ import annotations

from PySide6.QtCore import QSettings, Qt, Signal
from PySide6.QtGui import QColor, QImage, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSlider,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from genko import brushes
from genko.brushes import DEFAULT

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
        brushes.register(brushes.load_library())  # the person's own brushes
        self._fill_kinds()
        self.kinds.setMaximumHeight(170)
        self.make = QPushButton("ブラシを複製して調整…")
        self.make.setToolTip("選んでいるペンをもとに、入り抜き・筆圧・質感などを変えた自分のブラシを作ります")
        self.forget = QPushButton("自作のブラシを消す")
        self.forget.setToolTip("自分のブラシ一覧から消します（そのブラシで描いた原稿の線はそのまま）")
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
        make_row = QGridLayout()
        make_row.addWidget(self.make, 0, 0)
        make_row.addWidget(self.forget, 1, 0)
        layout.addLayout(make_row)
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

    def set_personal_pressure(self, gamma: float | None) -> None:
        """The pressure curve measured in the preferences, as 「自分に合わせた」."""
        for at in reversed(range(self.pressure.count())):
            if self.pressure.itemText(at).startswith("自分に合わせた"):
                self.pressure.removeItem(at)
        self.personal_gamma = gamma
        if gamma is not None:
            self.pressure.addItem(f"自分に合わせた（γ {gamma:g}）", gamma)

    def _fill_kinds(self) -> None:
        mine = brushes.load_library()
        for key, brush in brushes.everything().items():
            item = QListWidgetItem(("★ " if key.startswith("my_") else "") + brush.label)
            item.setData(Qt.ItemDataRole.UserRole, key)
            if key.startswith("my_") and key not in mine:
                item.setToolTip("この原稿に入っていたブラシ")
            self.kinds.addItem(item)

    def reload_kinds(self, select: str | None = None) -> None:
        """The list again (a new brush, or a book that brought its own)."""
        current = select or self.kind()
        self._loading = True
        self.kinds.clear()
        self._fill_kinds()
        keys = [self.kinds.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.kinds.count())]
        self.kinds.setCurrentRow(keys.index(current) if current in keys else 0)
        self._loading = False
        self.forget.setEnabled(self.kind().startswith("my_"))
        if select:
            self._kind_changed()

    def kind(self) -> str:
        item = self.kinds.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else DEFAULT

    def _load(self) -> None:
        kind = str(self.settings.value("brush/kind", DEFAULT))
        keys = list(brushes.everything())
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
        brush = brushes.brush(kind)
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
        self.forget.setEnabled(self.kind().startswith("my_"))
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
        brush = brushes.brush(self.kind())
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


TEXTURE_LABELS = [("なし（なめらか）", ""), ("鉛筆のざらつき", "grain"), ("筆のかすれ", "dry"), ("エアブラシ（ぼかし）", "soft")]


class BrushDialog(QDialog):
    """Duplicate a brush and adjust it: size, how thin a light touch gets, the pressure curve, opacity,
    steadiness, tapered ends, texture, a fixed width, drawing in white. A sample line shows the result."""

    def __init__(self, parent, base: str) -> None:
        super().__init__(parent)
        self.setWindowTitle("ブラシを複製して調整")
        b = brushes.brush(base)
        self.base = base
        self.name = QLineEdit(f"{b.label} のコピー")
        self.width = QDoubleSpinBox()
        self.width.setRange(0.05, 50)
        self.width.setSingleStep(0.1)
        self.width.setSuffix(" mm")
        self.width.setValue(b.width_mm)
        self.thin = QSpinBox()
        self.thin.setRange(0, 100)
        self.thin.setSuffix(" %")
        self.thin.setValue(round(b.min_pressure * 100))
        self.thin.setToolTip("いちばん弱く描いたときの太さ（太さに対する割合）。0 で針のように細くなる")
        self.curve = QDoubleSpinBox()
        self.curve.setRange(0.2, 5)
        self.curve.setSingleStep(0.1)
        self.curve.setValue(b.gamma)
        self.curve.setToolTip("1 より大きいと、強く押したときだけ太くなる（かたい）。小さいと弱い力でも太い（やわらかい）")
        self.opacity = QSpinBox()
        self.opacity.setRange(5, 100)
        self.opacity.setSuffix(" %")
        self.opacity.setValue(round(b.opacity * 100))
        self.steady = QSpinBox()
        self.steady.setRange(0, 15)
        self.steady.setValue(b.stabilize)
        self.taper = QCheckBox("入り抜き（線の両端を細く）")
        self.taper.setChecked(b.taper)
        self.texture = QComboBox()
        for label, key in TEXTURE_LABELS:
            self.texture.addItem(label, key)
        self.texture.setCurrentIndex(max(0, self.texture.findData(b.texture)))
        self.fixed = QCheckBox("筆圧で太さを変えない")
        self.fixed.setChecked(b.fixed_width)
        self.white = QCheckBox("白で描く（修正用）")
        self.white.setChecked(b.rgb == (255, 255, 255))
        self.sample = QLabel()
        self.sample.setMinimumHeight(70)
        self.sample.setStyleSheet("background: white; border: 1px solid #bbb")
        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.addRow("名前", self.name)
        form.addRow("太さ（はじめの値）", self.width)
        form.addRow("弱い筆圧での太さ", self.thin)
        form.addRow("筆圧の効き方", self.curve)
        form.addRow("不透明度", self.opacity)
        form.addRow("手ぶれ補正", self.steady)
        form.addRow("", self.taper)
        form.addRow("質感", self.texture)
        form.addRow("", self.fixed)
        form.addRow("", self.white)
        form.addRow("試し描き", self.sample)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("作る")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
        for widget in (self.width, self.thin, self.curve, self.opacity, self.steady):
            widget.valueChanged.connect(lambda _: self._draw_sample())
        for widget in (self.taper, self.fixed, self.white):
            widget.toggled.connect(lambda _: self._draw_sample())
        self.texture.currentIndexChanged.connect(lambda _: self._draw_sample())
        self._draw_sample()

    def data(self) -> dict:
        return {"label": self.name.text().strip() or "自分のブラシ", "base": self.base, "width_mm": self.width.value(),
                "min_pressure": self.thin.value() / 100, "gamma": round(self.curve.value(), 2), "opacity": self.opacity.value() / 100,
                "stabilize": self.steady.value(), "taper": self.taper.isChecked(), "texture": self.texture.currentData(),
                "fixed_width": self.fixed.isChecked(), "rgb": [255, 255, 255] if self.white.isChecked() else None}

    def _draw_sample(self) -> None:
        """An S-curve pressed lightly, then hard, then lightly, as this brush draws it."""
        import math

        from PIL import Image

        from genko.stroke import taper_points

        try:
            made = brushes.from_dict("my_sample", self.data())
        except ValueError:
            return
        brushes.CUSTOM["my_sample"] = made
        w, h, dpi = 300, 70, 96
        mm = 25.4 / dpi
        pts = [[(20 + i * 2.6) * mm, (35 + 18 * math.sin(i / 16)) * mm, math.sin(math.pi * i / 100)] for i in range(101)]
        if made.taper:
            pts = taper_points(pts)
        drawn = brushes.draw((w, h), pts, dpi, max(0.3, min(made.width_mm, 4.0)), "my_sample", seed="sample")
        paper = Image.new("RGB", (w, h), (235, 235, 235) if made.rgb == (255, 255, 255) else (255, 255, 255))
        if drawn is not None:
            cover, origin = drawn
            ink = Image.new("RGB", cover.size, made.rgb or (20, 20, 20))
            paper.paste(ink, origin, cover.point(lambda v: int(v * made.opacity)))
        data = paper.tobytes()
        self.sample.setPixmap(QPixmap.fromImage(QImage(data, w, h, w * 3, QImage.Format.Format_RGB888).copy()))
        brushes.CUSTOM.pop("my_sample", None)
