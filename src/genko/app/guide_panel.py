"""The 定規・3D panel: the page's rulers (on / off, angle, ellipse, copies, the panel they work in) and its
3D figures and boxes (pose, turn, size, perspective, trace them as lines)."""

from __future__ import annotations

import math

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
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


RULER = {"line": "直線定規", "curve": "曲線定規", "parallel": "平行線定規", "concentric": "同心円定規", "radial": "放射線定規",
         "perspective": "パース定規", "symmetry": "対称定規"}
PRESETS = {"stand": "立つ", "walk": "歩く", "run": "走る", "sit": "座る", "point": "指さす", "look_back": "振り返る", "arms_up": "両手を上げる"}


def ruler_label(ruler: dict) -> str:
    label = RULER.get(ruler.get("kind"), ruler.get("kind", ""))
    if ruler.get("kind") == "perspective":
        label += f"（{len(ruler.get('points') or [])} 点）"
    if ruler.get("kind") == "symmetry" and int(ruler.get("copies", 2) or 2) > 2:
        label += f"（{ruler['copies']} 方向）"
    if ruler.get("frame_id"):
        label += " ・ コマの中だけ"
    return label


class GuidePanel(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self._loading = False
        # rulers
        self.rulers = QListWidget()
        self.rulers.setMaximumHeight(130)
        self.rulers.currentRowChanged.connect(lambda _: self._ruler_picked())
        self.rulers.itemChanged.connect(self._ruler_toggled)
        self.angle = QDoubleSpinBox()
        self.angle.setRange(-360, 360)
        self.angle.setSuffix("°")
        self.angle.editingFinished.connect(lambda: self._ruler_set({"angle": self.angle.value()}))
        self.ratio = QDoubleSpinBox()
        self.ratio.setRange(0.05, 20)
        self.ratio.setSingleStep(0.05)
        self.ratio.setToolTip("1 で円。小さいほど横長の楕円")
        self.ratio.editingFinished.connect(lambda: self._ruler_set({"ratio": self.ratio.value()}))
        self.copies = QSpinBox()
        self.copies.setRange(2, 32)
        self.copies.setToolTip("2 で左右対称。3 以上は中心の周りに回した写し")
        self.copies.editingFinished.connect(lambda: self._ruler_set({"copies": self.copies.value()}))
        self.mirror = QCheckBox("鏡写しも")
        self.mirror.toggled.connect(lambda on: self._loading or self._ruler_set({"mirror": on}))
        self.in_panel = QCheckBox("選んだコマの中だけで効かせる")
        self.in_panel.toggled.connect(self._panel_only)
        delete = QPushButton("この定規を消す")
        delete.clicked.connect(self.delete_ruler)
        ruler_form = QFormLayout()
        ruler_form.addRow("角度", self.angle)
        ruler_form.addRow("縦横の比", self.ratio)
        ruler_form.addRow("写しの数", self.copies)
        ruler_form.addRow("", self.mirror)
        ruler_box = QGroupBox("定規（R で置く・点をドラッグで動かす）")
        rl = QVBoxLayout(ruler_box)
        rl.addWidget(self.rulers)
        rl.addLayout(ruler_form)
        rl.addWidget(self.in_panel)
        rl.addWidget(delete)
        self.ruler_hint = QLabel("このページに定規はありません。「定規」メニューから置きます。")
        self.ruler_hint.setWordWrap(True)
        rl.addWidget(self.ruler_hint)
        # 3D
        self.prims = QListWidget()
        self.prims.setMaximumHeight(110)
        self.prims.currentRowChanged.connect(lambda _: self._prim_picked())
        self.preset = QComboBox()
        self.preset.addItem("（ポーズを選ぶ）", "")
        for key, label in PRESETS.items():
            self.preset.addItem(label, key)
        self.preset.activated.connect(self._preset)
        self.turn = self._slider(-180, 180, lambda v: self._rot(1, v))
        self.tip = self._slider(-90, 90, lambda v: self._rot(0, v))
        self.lean = self._slider(-180, 180, lambda v: self._rot(2, v))
        self.size = QDoubleSpinBox()
        self.size.setRange(5, 400)
        self.size.setSuffix(" mm")
        self.size.editingFinished.connect(self._size)
        self.focal = QSlider(Qt.Orientation.Horizontal)
        self.focal.setRange(60, 1500)
        self.focal.setToolTip("左ほど遠近が強い（広角）")
        self.focal.sliderReleased.connect(lambda: self._prim_set({"focal_mm": self.focal.value()}))
        buttons = QHBoxLayout()
        trace = QPushButton("線にする")
        trace.setToolTip("選んだ 3D を、描く先のレイヤーに鉛筆の線で写します（下描きに）")
        trace.clicked.connect(lambda: self.window.trace_prims(selected_only=True))
        remove = QPushButton("消す")
        remove.clicked.connect(self.delete_prim)
        buttons.addWidget(trace)
        buttons.addWidget(remove)
        prim_form = QFormLayout()
        prim_form.addRow("ポーズ", self.preset)
        prim_form.addRow("向き", self.turn)
        prim_form.addRow("傾き（前後）", self.tip)
        prim_form.addRow("傾き（左右）", self.lean)
        prim_form.addRow("大きさ", self.size)
        prim_form.addRow("パース", self.focal)
        prim_box = QGroupBox("3D（J で関節や箱をドラッグ）")
        pl = QVBoxLayout(prim_box)
        pl.addWidget(self.prims)
        pl.addLayout(prim_form)
        pl.addLayout(buttons)
        layout = QVBoxLayout(self)
        layout.addWidget(ruler_box)
        layout.addWidget(prim_box)
        layout.addStretch(1)

    def _slider(self, lo, hi, on_change) -> QSlider:
        slider = QSlider(Qt.Orientation.Horizontal)
        slider.setRange(lo, hi)
        slider.sliderReleased.connect(lambda: on_change(slider.value()))
        slider.valueChanged.connect(lambda v: slider.setToolTip(f"{v}°"))
        return slider

    # --- state ----------------------------------------------------------------------------------------------

    def _page(self):
        return self.window.current_page()

    def refresh(self) -> None:
        page = self._page()
        self._loading = True
        keep_ruler = self.window.canvas.selected_ruler_id
        self.rulers.clear()
        for ruler in (page.rulers if page else []):
            item = QListWidgetItem(ruler_label(ruler))
            item.setData(Qt.ItemDataRole.UserRole, ruler["id"])
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked if ruler.get("active", True) else Qt.CheckState.Unchecked)
            item.setToolTip("チェックを外すと効かなくなります（線は吸い付かない）")
            self.rulers.addItem(item)
            if ruler["id"] == keep_ruler:
                self.rulers.setCurrentItem(item)
        self.ruler_hint.setVisible(self.rulers.count() == 0)
        keep_prim = self.window.canvas.selected_prim_id
        self.prims.clear()
        figures = boxes = 0
        for prim in (page.prims if page else []):
            if prim.get("kind") == "mannequin":
                figures += 1
                label = f"デッサン人形 {figures}"
                if prim.get("preset"):
                    label += f"（{PRESETS.get(prim['preset'], prim['preset'])}）"
            else:
                boxes += 1
                label = f"箱 {boxes}"
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, prim.get("id"))
            self.prims.addItem(item)
            if prim.get("id") == keep_prim:
                self.prims.setCurrentItem(item)
        self._loading = False
        self._show_ruler()
        self._show_prim()

    def _ruler(self) -> dict | None:
        page, item = self._page(), self.rulers.currentItem()
        if page is None or item is None:
            return None
        return next((r for r in page.rulers if r["id"] == item.data(Qt.ItemDataRole.UserRole)), None)

    def _prim(self) -> dict | None:
        page, item = self._page(), self.prims.currentItem()
        if page is None or item is None:
            return None
        return next((p for p in page.prims if p.get("id") == item.data(Qt.ItemDataRole.UserRole)), None)

    def _show_ruler(self) -> None:
        ruler = self._ruler()
        kind = ruler.get("kind") if ruler else None
        self._loading = True
        for widget, kinds in ((self.angle, ("parallel", "concentric")), (self.ratio, ("concentric",)),
                              (self.copies, ("symmetry",)), (self.mirror, ("symmetry",))):
            widget.setEnabled(kind in kinds)
        self.in_panel.setEnabled(ruler is not None)
        if ruler:
            self.angle.setValue(float(ruler.get("angle", 0) or 0))
            self.ratio.setValue(float(ruler.get("ratio", 1) or 1))
            self.copies.setValue(int(ruler.get("copies", 2) or 2))
            self.mirror.setChecked(bool(ruler.get("mirror")))
            self.in_panel.setChecked(bool(ruler.get("frame_id")))
        self._loading = False

    def _show_prim(self) -> None:
        prim = self._prim()
        for widget in (self.preset, self.turn, self.tip, self.lean, self.size, self.focal):
            widget.setEnabled(prim is not None)
        if prim is None:
            return
        self._loading = True
        figure = prim.get("kind") == "mannequin"
        self.preset.setEnabled(figure)
        self.focal.setEnabled(not figure)
        rot = (list(prim.get("rot") or [0, 0, 0]) + [0, 0, 0])[:3]
        self.tip.setValue(round(math.degrees(float(rot[0]))))
        self.turn.setValue(round(math.degrees(float(rot[1]))))
        self.lean.setValue(round(math.degrees(float(rot[2]))))
        size = prim.get("size") or [40, 80, 20]
        self.size.setValue(float(size[1]) if figure else max(float(v) for v in size))
        self.size.setToolTip("身長" if figure else "いちばん長い辺（形はそのまま）")
        self.focal.setValue(int(float(prim.get("focal_mm", 400) or 400)))
        self._loading = False

    # --- edits ------------------------------------------------------------------------------------------

    def _ruler_picked(self) -> None:
        if self._loading:
            return
        ruler = self._ruler()
        self.window.canvas.selected_ruler_id = ruler["id"] if ruler else None
        self.window.canvas.update()
        self._show_ruler()

    def _ruler_toggled(self, item: QListWidgetItem) -> None:
        if self._loading:
            return
        self.window.apply_ops([{"op": "edit_ruler", "page": self._page().index, "id": item.data(Qt.ItemDataRole.UserRole),
                                "active": item.checkState() == Qt.CheckState.Checked}])

    def _ruler_set(self, change: dict) -> None:
        ruler = self._ruler()
        if self._loading or ruler is None:
            return
        self.window.apply_ops([{"op": "edit_ruler", "page": self._page().index, "id": ruler["id"], **change}])

    def _panel_only(self, on: bool) -> None:
        if self._loading or self._ruler() is None:
            return
        frame = self.window.selected_frame()
        if on and frame is None:
            self.window.flash("先にコマを選びます（選択ツールでコマをクリック）", 3000)
            self._loading = True
            self.in_panel.setChecked(False)
            self._loading = False
            return
        self._ruler_set({"frame_id": frame.id if on else None})

    def delete_ruler(self) -> None:
        ruler = self._ruler()
        if ruler is None:
            ruler_id = self.window.canvas.selected_ruler_id
        else:
            ruler_id = ruler["id"]
        if ruler_id and self.window.apply_ops([{"op": "delete_ruler", "page": self._page().index, "id": ruler_id}]):
            self.window.canvas.selected_ruler_id = None

    def _prim_picked(self) -> None:
        if self._loading:
            return
        prim = self._prim()
        self.window.canvas.selected_prim_id = prim.get("id") if prim else None
        self.window.canvas.update()
        self._show_prim()

    def select_prim(self, prim_id: str) -> None:
        for row in range(self.prims.count()):
            if self.prims.item(row).data(Qt.ItemDataRole.UserRole) == prim_id:
                self.prims.setCurrentRow(row)
                return
        self.prims.setCurrentRow(-1)

    def _prim_set(self, change: dict) -> None:
        prim = self._prim()
        if self._loading or prim is None:
            return
        self.window.apply_ops([{"op": "edit_prim", "page": self._page().index, "id": prim["id"], **change}])

    def _rot(self, index: int, degrees: int) -> None:
        prim = self._prim()
        if self._loading or prim is None:
            return
        rot = (list(prim.get("rot") or [0, 0, 0]) + [0, 0, 0])[:3]
        rot[index] = round(math.radians(degrees), 4)
        self._prim_set({"rot": rot})

    def _size(self) -> None:
        prim = self._prim()
        if self._loading or prim is None:
            return
        value = self.size.value()
        if prim.get("kind") == "mannequin":
            self._prim_set({"size": [value / 2, value, value / 4]})
        else:
            size = [float(v) for v in prim.get("size") or [40, 40, 40]]
            k = value / max(size)
            self._prim_set({"size": [round(v * k, 3) for v in size]})

    def _preset(self) -> None:
        prim, key = self._prim(), self.preset.currentData()
        if prim is None or not key or prim.get("kind") != "mannequin":
            return
        self.window.apply_ops([{"op": "pose_mannequin", "page": self._page().index, "id": prim["id"], "preset": key}])
        self.preset.setCurrentIndex(0)

    def delete_prim(self) -> None:
        prim_id = (self._prim() or {}).get("id") or self.window.canvas.selected_prim_id
        if prim_id and self.window.apply_ops([{"op": "delete_prim", "page": self._page().index, "id": prim_id}]):
            self.window.canvas.selected_prim_id = None

