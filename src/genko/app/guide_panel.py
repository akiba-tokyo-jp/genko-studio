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
    QGridLayout,
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
# the 3D figure's poses (genko.mesh3d.FIGURE_PRESETS), and the hands'
FIGURE_PRESETS = {"stand": "立つ", "walk": "歩く", "run": "走る", "sit": "座る", "point": "指さす", "arms_up": "両手を上げる",
                  "think": "考える", "kneel": "片ひざ", "peace": "ピース"}
HAND_POSES = {"open": "開く", "relaxed": "力を抜く", "fist": "握る", "point": "指さす", "peace": "ピース", "grip": "つかむ"}
KIND_LABELS = {"box": "箱", "cylinder": "円柱", "stairs": "階段", "floor": "床", "scene": "背景", "figure": "デッサン人形（3D）",
               "head": "頭部", "hand": "手", "mesh": "モデル", "mannequin": "デッサン人形（棒）"}


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
        self.in_panel = QCheckBox("選んだコマの中だけ")
        self.in_panel.setToolTip("この定規を、選んだコマの中だけで効かせます")
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
        more = QGridLayout()
        self.body_button = QPushButton("体型・手…")
        self.body_button.setToolTip("デッサン人形の等身・肩幅・腰幅・体格・脚の長さと、手のポーズ（手のモデルはその形）")
        self.body_button.clicked.connect(self.body_dialog)
        camera = QPushButton("カメラ・光…")
        camera.setToolTip("このページの 3D をまとめて見る向き・画角と、光の向き")
        camera.clicked.connect(self.camera_dialog)
        surfaces = QPushButton("線と面に…")
        surfaces.setToolTip("3D を描く先のレイヤーに、線（見えない所は描かない）と陰の面（トーン化もできる）で写します")
        surfaces.clicked.connect(self.render_dialog)
        more.addWidget(self.body_button, 0, 0)
        more.addWidget(camera, 0, 1)
        more.addWidget(surfaces, 1, 0, 1, 2)
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
        pl.addLayout(more)
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
        counts: dict[str, int] = {}
        for prim in (page.prims if page else []):
            kind = prim.get("kind") or "box"
            counts[kind] = counts.get(kind, 0) + 1
            label = f"{KIND_LABELS.get(kind, kind)} {counts[kind]}"
            if kind == "mesh" and prim.get("title"):
                label = f"モデル「{prim['title']}」"
            if prim.get("preset"):
                label += f"（{({**PRESETS, **FIGURE_PRESETS}).get(prim['preset'], prim['preset'])}）"
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
        figure = prim.get("kind") in ("mannequin", "figure")
        self.preset.clear()
        self.preset.addItem("（ポーズを選ぶ）", "")
        for key, label in (FIGURE_PRESETS if prim.get("kind") == "figure" else HAND_POSES if prim.get("kind") == "hand" else PRESETS).items():
            self.preset.addItem(label, key)
        self.preset.setEnabled(figure or prim.get("kind") == "hand")
        self.body_button.setEnabled(prim.get("kind") in ("figure", "hand"))
        self.focal.setEnabled(prim.get("kind") != "mannequin")
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
        if prim.get("kind") in ("mannequin", "figure"):
            self._prim_set({"size": [value / 2, value, value / 4]})
        else:
            size = [float(v) for v in prim.get("size") or [40, 40, 40]]
            k = value / max(size)
            self._prim_set({"size": [round(v * k, 3) for v in size]})

    def _preset(self) -> None:
        prim, key = self._prim(), self.preset.currentData()
        if prim is None or not key:
            return
        if prim.get("kind") == "mannequin":
            self.window.apply_ops([{"op": "pose_mannequin", "page": self._page().index, "id": prim["id"], "preset": key}])
        elif prim.get("kind") == "figure":
            self.window.apply_ops([{"op": "pose_figure", "page": self._page().index, "id": prim["id"], "preset": key}])
        elif prim.get("kind") == "hand":
            self.window.apply_ops([{"op": "pose_figure", "page": self._page().index, "id": prim["id"], "pose": key}])
        self.preset.setCurrentIndex(0)

    # --- the figure's body and hands, the camera and light, 3D into drawing ---------------------------------

    def body_dialog(self) -> None:
        from PySide6.QtWidgets import QDialog, QDialogButtonBox

        prim = self._prim()
        if prim is None or prim.get("kind") not in ("figure", "hand"):
            self.window.flash("先にデッサン人形（3D）か手を選びます", 3000)
            return
        dialog = QDialog(self)
        dialog.setWindowTitle("体型・手")
        form = QFormLayout(dialog)
        fields = {}
        if prim["kind"] == "figure":
            body = {"heads": 7.5, "shoulders": 1.0, "hips": 1.0, "build": 1.0, "legs": 1.0, **(prim.get("body") or {})}
            for key, label, lo, hi in (("heads", "等身", 4, 10), ("shoulders", "肩幅", 0.6, 1.5), ("hips", "腰幅", 0.6, 1.6),
                                       ("build", "体格（太さ）", 0.5, 1.8), ("legs", "脚の長さ", 0.6, 1.5)):
                box = QDoubleSpinBox()
                box.setRange(lo, hi)
                box.setSingleStep(0.1 if hi <= 2 else 0.5)
                box.setValue(float(body[key]))
                form.addRow(label, box)
                fields[key] = box
        hands = {}
        for side, label in (("l", "左手"), ("r", "右手")) if prim["kind"] == "figure" else (("pose", "手の形"),):
            combo = QComboBox()
            for key, name in HAND_POSES.items():
                combo.addItem(name, key)
            now = (prim.get("hands") or {}).get(side) if prim["kind"] == "figure" else prim.get("pose")
            combo.setCurrentIndex(max(0, combo.findData(now or "relaxed")))
            form.addRow(label, combo)
            hands[side] = combo
        ok = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        ok.accepted.connect(dialog.accept)
        ok.rejected.connect(dialog.reject)
        form.addRow(ok)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        op = {"op": "pose_figure", "page": self._page().index, "id": prim["id"]}
        if prim["kind"] == "figure":
            op["body"] = {k: round(v.value(), 2) for k, v in fields.items()}
            op["hands"] = {k: c.currentData() for k, c in hands.items()}
        else:
            op["pose"] = hands["pose"].currentData()
        self.window.apply_ops([op])

    def camera_dialog(self) -> None:
        from PySide6.QtWidgets import QDialog, QDialogButtonBox

        page = self._page()
        if page is None:
            return
        camera = dict((page.extra or {}).get("camera") or {})
        light = dict((page.extra or {}).get("light") or {})
        dialog = QDialog(self)
        dialog.setWindowTitle("カメラ・光（このページの 3D）")
        form = QFormLayout(dialog)
        use = QCheckBox("カメラを使う（3D をまとめて同じ向きから見る）")
        use.setChecked(bool(camera))
        turn, tip, roll = (QDoubleSpinBox() for _ in range(3))
        for box, key, label in ((turn, "turn", "回り込み（°）"), (tip, "tip", "見下ろし（°）"), (roll, "roll", "傾き（°）")):
            box.setRange(-180, 180)
            box.setValue(math.degrees(float(camera.get(key, 0))))
            form.addRow(label, box)
        focal = QDoubleSpinBox()
        focal.setRange(20, 5000)
        focal.setSuffix(" mm")
        focal.setValue(float(camera.get("focal_mm", 400)))
        focal.setToolTip("小さいほど広角（遠近が強い）")
        form.insertRow(0, use)
        form.addRow("画角（焦点距離）", focal)
        lx, ly, lz = (QDoubleSpinBox() for _ in range(3))
        direction = list(light.get("dir") or [-0.5, -0.7, -0.6])
        for box, value, label in ((lx, direction[0], "光: 右へ"), (ly, direction[1], "光: 下へ"), (lz, direction[2], "光: 奥へ")):
            box.setRange(-1, 1)
            box.setSingleStep(0.1)
            box.setValue(float(value))
            form.addRow(label, box)
        ambient = QDoubleSpinBox()
        ambient.setRange(0, 1)
        ambient.setSingleStep(0.05)
        ambient.setValue(float(light.get("ambient", 0.35)))
        form.addRow("明るさの底上げ", ambient)
        ok = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        ok.accepted.connect(dialog.accept)
        ok.rejected.connect(dialog.reject)
        form.addRow(ok)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        ops = [{"op": "set_light", "page": page.index, "dir": [lx.value(), ly.value(), lz.value()], "ambient": ambient.value()}]
        if use.isChecked():
            ops.append({"op": "set_camera", "page": page.index, "turn": math.radians(turn.value()), "tip": math.radians(tip.value()),
                        "roll": math.radians(roll.value()), "focal_mm": focal.value()})
        else:
            ops.append({"op": "set_camera", "page": page.index, "off": True})
        self.window.apply_ops(ops)

    def render_dialog(self) -> None:
        from PySide6.QtWidgets import QMessageBox

        page = self._page()
        layer = self.window._paint_layer()
        if page is None or layer is None or not page.prims:
            self.window.flash("3D を置き、描く先のレイヤーを選びます", 3000)
            return
        prim = self._prim()
        answer = QMessageBox.question(self, "3D を線と面に", "陰の面も写しますか？（はい: 線と面・面はトーン化して網点で印刷 ／ いいえ: 線だけ）",
                                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No | QMessageBox.StandardButton.Cancel)
        if answer == QMessageBox.StandardButton.Cancel:
            return
        op = {"op": "render_prims", "page": page.index, "layer_id": layer.id, "surfaces": answer == QMessageBox.StandardButton.Yes}
        if prim is not None:
            op["ids"] = [prim["id"]]
        if op["surfaces"]:
            op["tone"] = {"lpi": 60}
        self.window.apply_ops([op])

    def delete_prim(self) -> None:
        prim_id = (self._prim() or {}).get("id") or self.window.canvas.selected_prim_id
        if prim_id and self.window.apply_ops([{"op": "delete_prim", "page": self._page().index, "id": prim_id}]):
            self.window.canvas.selected_prim_id = None

