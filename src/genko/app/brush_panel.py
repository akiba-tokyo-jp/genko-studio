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
# a few quiet colours for colour work and blue pencil (the chosen colour is always one click away)
COLOURS = [(196, 72, 60), (214, 140, 72), (206, 178, 92), (98, 142, 96), (72, 118, 164), (86, 96, 150), (130, 96, 140),
           (222, 178, 172), (150, 116, 88), (238, 222, 204)]


def stroke_preview(kind: str, ink, size=(96, 20)):
    """A short line drawn with this brush (pressed lightly, hard, lightly), for the brush list."""
    import math

    from PIL import Image

    from genko.stroke import taper_points

    key = (kind, tuple(ink), size)
    if key in _PREVIEWS:
        return _PREVIEWS[key]
    w, h = size[0] * 2, size[1] * 2
    dpi = 96
    mm = 25.4 / dpi
    brush = brushes.brush(kind)
    pts = [[(6 + i * (w - 12) / 60) * mm, (h / 2 + (h / 4) * math.sin(i / 9.5)) * mm, math.sin(math.pi * i / 60)]
           for i in range(61)]
    if brush.taper:
        pts = taper_points(pts)
    width = max(0.4, min(brush.width_mm * 1.6, h * 0.4 * mm))
    image = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    try:
        drawn = brushes.draw((w, h), pts, dpi, width, kind, seed="preview")
    except Exception:  # (a broken brush of one's own: no picture rather than no list)
        drawn = None
    if drawn is not None:
        cover, origin = drawn
        rgb = tuple(ink) if brush.rgb in (None, (20, 20, 20)) else brush.rgb
        layer = Image.new("RGBA", cover.size, (*rgb, 255))
        layer.putalpha(cover.point(lambda v: int(v * max(0.35, brush.opacity))))
        image.alpha_composite(layer, origin)
    data = image.tobytes()
    pixmap = QPixmap.fromImage(QImage(data, w, h, w * 4, QImage.Format.Format_RGBA8888).copy())
    pixmap.setDevicePixelRatio(2)
    _PREVIEWS[key] = pixmap
    return pixmap


_PREVIEWS: dict = {}
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
        self.make = QPushButton("複製して調整…")
        self.make.setToolTip("選んでいるペンをもとに、入り抜き・筆圧・質感などを変えた自分のブラシを作ります")
        self.forget = QPushButton("自作のブラシを消す")
        for button in (self.make, self.forget):
            button.setProperty("row", True)
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
            button.setProperty("chip", True)  # (small flat choices: theme.py)
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
        self.taper_in, self.taper_out = QDoubleSpinBox(), QDoubleSpinBox()
        for box, what in ((self.taper_in, "入り（描き始め）"), (self.taper_out, "抜き（描き終わり）")):
            box.setMaximumWidth(110)
            box.setRange(-1, 30)  # (-1: 自動, the line's length decides as before)
            box.setSingleStep(0.5)
            box.setDecimals(1)
            box.setSuffix(" mm")
            box.setSpecialValueText("自動")
            box.setToolTip(f"{what}が細くなる長さ。0 でその端は細くしない。自動: 線の長さの 1/4（両方とも自動のとき）")
            box.valueChanged.connect(lambda _: self._save())
        self.ink_pressure = QSpinBox()
        self.ink_pressure.setMaximumWidth(110)
        self.ink_pressure.setRange(0, 100)
        self.ink_pressure.setSuffix(" %")
        self.ink_pressure.setToolTip("弱い筆圧で線が薄くなる割合（鉛筆の下描き・影に）。0 % で筆圧は太さにだけ効く")
        self.ink_pressure.valueChanged.connect(lambda _: self._save())
        self.speed_steady = QCheckBox("速い線ほど補正を強く")
        self.speed_steady.setToolTip("速度による手ブレ補正: すばやく引いた所ほど手ぶれを強く抑え、ゆっくり描いた所は細かい形を残す")
        self.speed_steady.toggled.connect(lambda _: self._save())
        self.post_fit = QDoubleSpinBox()
        self.post_fit.setMaximumWidth(110)
        self.post_fit.setRange(0, 2)
        self.post_fit.setSingleStep(0.1)
        self.post_fit.setDecimals(1)
        self.post_fit.setSuffix(" mm")
        self.post_fit.setSpecialValueText("しない")
        self.post_fit.setToolTip("後補正: 描き終えた線のゆれ（この幅まで）を除き、なめらかな曲線に置き換える")
        self.post_fit.valueChanged.connect(lambda _: self._save())
        self.pressure = QComboBox()
        self.pressure.setToolTip("やわらかい: 弱い力でも太く。かたい: 強く押したときだけ太く")
        for label, gamma in PRESSURE:
            self.pressure.addItem(label, gamma)
        self.pressure.currentIndexChanged.connect(lambda _: self._save())
        # colour
        self.swatch = QPushButton()
        self.swatch.setFixedSize(40, 40)
        self.swatch.clicked.connect(self._pick)
        palette = QGridLayout()
        for i, rgb in enumerate(MONO + COLOURS):
            button = QPushButton()
            button.setFixedSize(22, 22)  # (22 px, 2 px apart or more: a 24 px target pitch)
            button.setStyleSheet(f"QPushButton {{ background: rgb{rgb}; border: 1px solid rgba(128,128,128,0.45); border-radius: 11px; }}"
                                 "QPushButton:hover { border: 2px solid palette(highlight); }")
            button.setToolTip("白" if rgb == (255, 255, 255) else ("黒" if rgb == (20, 20, 20) else f"色（RGB {rgb[0]}, {rgb[1]}, {rgb[2]}）"))
            button.setAccessibleName(button.toolTip())
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
        self.reference.addItem("参照レイヤー", "reference")
        self.reference.setToolTip("参照レイヤー: レイヤー パネルで「参照にする」にしたレイヤーの線を見て塗ります")
        self.reference.currentIndexChanged.connect(lambda _: self._save())
        self.crossing = QCheckBox("消しゴムで交点まで消す")
        self.crossing.setToolTip("線の交わる所までを一度に消します（はみ出しの掃除）")
        self.crossing.toggled.connect(lambda _: self._save())
        from genko.app.fields import LineSample, slider_for, with_value

        # the pen as set now, drawn (a change is seen before the page is touched)
        self.sample = LineSample(lambda: {**self.stroke_fields(), "opacity": self.opacity.value() / 100, "rgb": self.rgb})
        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.addRow(self.sample)
        form.addRow("太さ", slider_for(self.size, log=True))
        form.addRow(self._wrap(sizes))
        form.addRow("不透明度", with_value(self.opacity))
        form.addRow("手ぶれ補正", slider_for(self.steady))
        form.addRow(self.speed_steady)
        form.addRow("後補正", slider_for(self.post_fit))
        form.addRow(self.taper)
        form.addRow("入り", slider_for(self.taper_in))
        form.addRow("抜き", slider_for(self.taper_out))
        form.addRow("筆圧で濃さ", slider_for(self.ink_pressure))
        form.addRow("筆圧", self.pressure)
        palette.setSpacing(3)
        colour_row = QHBoxLayout()
        colour_row.addWidget(self.swatch, 0, Qt.AlignmentFlag.AlignTop)
        colour_row.addLayout(palette)
        colour_row.addStretch(1)
        fill_form = QFormLayout()
        fill_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapAllRows)
        fill_form.addRow("隙間を閉じる", slider_for(self.gap))
        fill_form.addRow("見る範囲", self.reference)
        from genko.app import theme

        def section(title: str, tip: str = "") -> QLabel:
            label = QLabel(title)
            theme.role(label, "section")
            label.setToolTip(tip)
            return label

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(section("ペンの種類"))
        layout.addWidget(self.kinds)
        self.files = QPushButton("読み込み・書き出し ▾")
        self.files.setProperty("row", True)
        self.files.setToolTip("ブラシをファイルに書き出す・読み込む（.genkobrush、Photoshop の .abr）")
        make_row = QVBoxLayout()
        make_row.setSpacing(1)
        from genko.app.tool_settings import _or_blank

        for button in (self.make, self.forget, self.files):
            button.setIcon(_or_blank(button.icon()))  # (the rows' words start together)
            make_row.addWidget(button)
        layout.addLayout(make_row)
        layout.addWidget(section("描き味"))
        layout.addLayout(form)
        layout.addWidget(section("色", "スポイト（I）で原稿から拾えます"))
        layout.addLayout(colour_row)
        layout.addWidget(section("塗りつぶし（G）"))
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
        from PySide6.QtCore import QSize

        from genko.app import theme

        mine = brushes.load_library()
        ink = theme.QColor(theme.tokens().text)
        self.kinds.setIconSize(QSize(72, 20))
        for key, brush in brushes.everything().items():
            item = QListWidgetItem(("★ " if key.startswith("my_") else "") + brush.label)
            item.setIcon(theme.still_icon(stroke_preview(key, (ink.red(), ink.green(), ink.blue()), (72, 20))))
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

        def kept(key: str, default):
            return self.settings.value(prefix + key, default) if stored else default

        self.taper_in.setValue(float(kept("taper_in", -1)))
        self.taper_out.setValue(float(kept("taper_out", -1)))
        self.ink_pressure.setValue(int(float(kept("ink_pressure", 0))))
        self.speed_steady.setChecked(str(kept("speed_steady", False)).lower() == "true")
        self.post_fit.setValue(float(kept("post_fit", 0)))
        self._follow_taper()

    def _kind_changed(self) -> None:
        if self._loading:
            return
        self._loading = True
        self._apply_kind_defaults(self.kind(), stored=True)
        self._loading = False
        self.forget.setEnabled(self.kind().startswith("my_"))
        self._save()

    def _save(self) -> None:
        if hasattr(self, "sample"):
            self.sample.refresh()
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
        self.settings.setValue(prefix + "taper_in", self.taper_in.value())
        self.settings.setValue(prefix + "taper_out", self.taper_out.value())
        self.settings.setValue(prefix + "ink_pressure", self.ink_pressure.value())
        self.settings.setValue(prefix + "speed_steady", self.speed_steady.isChecked())
        self.settings.setValue(prefix + "post_fit", self.post_fit.value())
        self._follow_taper()
        self.settings.setValue("fill/gap", self.gap.value())
        self.settings.setValue("fill/reference", self.reference.currentData())
        self.settings.setValue("eraser/crossing", self.crossing.isChecked())
        self.changed.emit()

    def _follow_taper(self) -> None:
        """The lengths only mean something while the line tapers."""
        for box in (self.taper_in, self.taper_out):
            box.setEnabled(self.taper.isChecked())

    def set_colour(self, rgb) -> None:
        self.rgb = tuple(int(v) for v in rgb)[:3]
        self.swatch.setStyleSheet(f"QPushButton {{ background: rgb{self.rgb}; border: 2px solid rgba(128,128,128,0.6); border-radius: 20px; }}")
        self.swatch.setToolTip(f"今の色 {self.rgb}")
        self.settings.setValue("brush/rgb", ",".join(str(v) for v in self.rgb))
        if hasattr(self, "sample"):
            self.sample.refresh()
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
        if out["taper"] and (self.taper_in.value() >= 0 or self.taper_out.value() >= 0):
            for key, box in (("taper_in_mm", self.taper_in), ("taper_out_mm", self.taper_out)):
                if box.value() >= 0:
                    out[key] = round(box.value(), 2)
        if self.ink_pressure.value():
            out["pressure_opacity"] = round(self.ink_pressure.value() / 100, 2)
        if self.speed_steady.isChecked() and out["stabilize"]:
            out["stabilize_speed"] = True
        if self.post_fit.value() > 0:
            out["post_fit"] = round(self.post_fit.value(), 2)
        return out


TEXTURE_LABELS = [("なし（なめらか）", ""), ("鉛筆のざらつき", "grain"), ("筆のかすれ", "dry"), ("エアブラシ（ぼかし）", "soft"),
                  ("水彩（縁に色がたまる）", "water")]
TIP_LABELS = [("丸", "round"), ("平たい（カリグラフィ）", "flat"), ("画像", "image")]
PATTERN_LABELS = [("なし", ""), ("点", "dots"), ("破線", "dash"), ("レース", "lace"), ("草", "grass"), ("ハート", "hearts"),
                  ("星", "stars"), ("葉", "leaves")]
AA_LABELS = [("なし", "none"), ("弱", "weak"), ("中", "normal"), ("強", "strong")]


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
        from genko.app import theme

        self.sample.setStyleSheet(f"background: white; border: 1px solid {theme.tokens().border}")  # (the paper)
        # the tip and how it is laid down (J3)
        self.tip = QComboBox()
        for label, key in TIP_LABELS:
            self.tip.addItem(label, key)
        self.tip.setCurrentIndex(max(0, self.tip.findData(b.tip)))
        self.tip_png = b.tip_png
        self.tip_angle = QSpinBox()
        self.tip_angle.setRange(-180, 180)
        self.tip_angle.setSuffix(" °")
        self.tip_angle.setValue(round(b.tip_angle))
        self.tip_ratio = QSpinBox()
        self.tip_ratio.setRange(2, 100)
        self.tip_ratio.setSuffix(" %")
        self.tip_ratio.setValue(round(b.tip_ratio * 100))
        self.tip_follow = QCheckBox("先端を線の向きに合わせて回す")
        self.tip_follow.setChecked(b.tip_follow)
        self.tip_rotation = QCheckBox("ペンの軸を回すと先端も回る（アートペン）")
        self.tip_rotation.setChecked(getattr(b, "tip_rotation", False))
        self.tip_picture = QPushButton("画像から先端を作る…")
        self.tip_picture.clicked.connect(self._pick_tip)
        self.pattern = QComboBox()
        for label, key in PATTERN_LABELS:
            self.pattern.addItem(label, key)
        self.pattern.setCurrentIndex(max(0, self.pattern.findData(b.pattern)))
        self.spacing = QSpinBox()
        self.spacing.setRange(0, 500)
        self.spacing.setSuffix(" %")
        self.spacing.setValue(round(b.spacing * 100))
        self.spacing.setToolTip("先端を置く間隔（太さに対する割合）。0 で続いた線")
        self.scatter = QSpinBox()
        self.scatter.setRange(0, 500)
        self.scatter.setSuffix(" %")
        self.scatter.setValue(round(b.scatter * 100))
        self.scatter.setToolTip("先端を線から散らす広さ（スプレー・点描）")
        self.stamp = QSpinBox()
        self.stamp.setRange(2, 300)
        self.stamp.setSuffix(" %")
        self.stamp.setValue(round(b.stamp_size * 100))
        self.jitter = QSpinBox()
        self.jitter.setRange(0, 100)
        self.jitter.setSuffix(" %")
        self.jitter.setValue(round(b.size_jitter * 100))
        self.turn = QCheckBox("ランダムに回す")
        self.turn.setChecked(b.turn_jitter)
        self.count = QSpinBox()
        self.count.setRange(1, 12)
        self.count.setValue(b.count)
        self.speed = QSpinBox()
        self.speed.setRange(0, 100)
        self.speed.setSuffix(" %")
        self.speed.setValue(round(b.speed * 100))
        self.speed.setToolTip("速く描くほど細くなる強さ")
        self.post = QSpinBox()
        self.post.setRange(0, 10)
        self.post.setValue(b.post_smooth)
        self.post.setToolTip("描き終えた後に線をなめらかに整える強さ（後補正）")
        self.aa = QComboBox()
        for label, key in AA_LABELS:
            self.aa.addItem(label, key)
        self.aa.setCurrentIndex(max(0, self.aa.findData(b.aa)))
        from genko.app import dialog_look as look
        from genko.app import theme

        form = look.form()
        form.addRow("名前", self.name)
        form.addRow(look.section("線の太さと筆圧"))
        form.addRow("太さ（はじめの値）", self.width)
        form.addRow("弱い筆圧での太さ", self.thin)
        form.addRow("筆圧の効き方", self.curve)
        form.addRow("", self.taper)
        form.addRow("速さで細く", self.speed)
        form.addRow(look.section("なめらかさ"))
        form.addRow("手ぶれ補正", self.steady)
        form.addRow("後補正", self.post)
        form.addRow("アンチエイリアス", self.aa)
        form.addRow(look.section("色と質感"))
        form.addRow("不透明度", self.opacity)
        form.addRow("質感", self.texture)
        form.addRow("", self.fixed)
        form.addRow("", self.white)
        tips = look.form()
        tips.addRow(look.section("先端"))
        tips.addRow("先端の形", self.tip)
        tips.addRow("", self.tip_picture)
        tips.addRow("先端の角度", self.tip_angle)
        tips.addRow("平たさ", self.tip_ratio)
        tips.addRow("", self.tip_follow)
        tips.addRow("", self.tip_rotation)
        tips.addRow(look.section("模様（点・レース・草などを並べる）"))
        tips.addRow("模様", self.pattern)
        tips.addRow("間隔", self.spacing)
        tips.addRow("散らばり", self.scatter)
        tips.addRow("1 つの大きさ", self.stamp)
        tips.addRow("大きさの乱れ", self.jitter)
        tips.addRow("", self.turn)
        tips.addRow("一度に置く数", self.count)
        from PySide6.QtWidgets import QTabWidget, QWidget

        tabs = QTabWidget()
        basic, shape = QWidget(), QWidget()
        basic.setLayout(form)
        shape.setLayout(tips)
        tabs.addTab(basic, "描き味")
        tabs.addTab(shape, "先端・模様")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("作る")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        for rows in (form, tips):
            look.quiet_labels(rows)
        self.sample.setMinimumSize(260, 160)
        self.sample.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.sample.setStyleSheet("background: white; border-radius: 6px")
        side = QVBoxLayout()
        side.addWidget(self.sample, 1)
        change = QLabel("値を変えると、すぐにこの線が描き直されます（弱く・強く・弱くと押した線）。")
        change.setWordWrap(True)
        theme.role(change, "hint")
        side.addWidget(change)
        look.frame(self, look.header("ブラシを複製して調整", f"「{brushes.brush(base).label}」をもとに、自分のブラシを作ります。元のブラシは変わりません。"),
                   tabs, look.card(side, "試し描き"), look.footer(buttons))
        self.resize(900, 600)
        for widget in (self.width, self.thin, self.curve, self.opacity, self.steady, self.tip_angle, self.tip_ratio, self.spacing,
                       self.scatter, self.stamp, self.jitter, self.count, self.speed, self.post):
            widget.valueChanged.connect(lambda _: self._draw_sample())
        for widget in (self.taper, self.fixed, self.white, self.tip_follow, self.tip_rotation, self.turn):
            widget.toggled.connect(lambda _: self._draw_sample())
        for widget in (self.texture, self.tip, self.pattern, self.aa):
            widget.currentIndexChanged.connect(lambda _: self._draw_sample())
        self._draw_sample()

    def data(self) -> dict:
        return {"label": self.name.text().strip() or "自分のブラシ", "base": self.base, "width_mm": self.width.value(),
                "min_pressure": self.thin.value() / 100, "gamma": round(self.curve.value(), 2), "opacity": self.opacity.value() / 100,
                "stabilize": self.steady.value(), "taper": self.taper.isChecked(), "texture": self.texture.currentData(),
                "fixed_width": self.fixed.isChecked(), "rgb": [255, 255, 255] if self.white.isChecked() else None,
                "tip": self.tip.currentData() if (self.tip.currentData() != "image" or self.tip_png) else "round",
                "tip_angle": float(self.tip_angle.value()), "tip_ratio": self.tip_ratio.value() / 100,
                "tip_follow": self.tip_follow.isChecked(), "tip_rotation": self.tip_rotation.isChecked(), "tip_png": self.tip_png or "", "pattern": self.pattern.currentData(),
                "spacing": self.spacing.value() / 100, "scatter": self.scatter.value() / 100, "stamp_size": self.stamp.value() / 100,
                "size_jitter": self.jitter.value() / 100, "turn_jitter": self.turn.isChecked(), "count": self.count.value(),
                "speed": self.speed.value() / 100, "post_smooth": self.post.value(), "aa": self.aa.currentData()}

    def _pick_tip(self) -> None:
        from PySide6.QtWidgets import QFileDialog

        from genko import abr

        path, _ = QFileDialog.getOpenFileName(self, "先端にする画像", "", "画像 (*.png *.jpg *.jpeg *.webp *.bmp)")
        if not path:
            return
        try:
            self.tip_png = abr.tip_from_picture(path)
        except (abr.AbrError, OSError):
            return
        self.tip.setCurrentIndex(self.tip.findData("image"))
        if not self.spacing.value():
            self.spacing.setValue(25)
        self._draw_sample()

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
        w, h, dpi = 300, 140, 96
        mm = 25.4 / dpi
        pts = [[(22 + i * 2.56) * mm, (70 + 34 * math.sin(i / 16)) * mm, math.sin(math.pi * i / 100)] for i in range(101)]
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
