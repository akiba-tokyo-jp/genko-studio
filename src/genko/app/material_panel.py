"""The 素材 panel: materials (built-in and the person's own) by folder, the settings of the tone being
worked on, and the effect lines of the page."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt
from PySide6.QtGui import QIcon, QImage, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from genko import effects, materials

PATTERNS = [("網点", "dot"), ("線", "line"), ("カケアミ風（交差）", "cross"), ("砂目", "noise"), ("ベタのグレー", "flat")]
KIND_WORD = {"tone": "トーン", "effect": "効果線", "image": "画像", "lines": "パーツ"}
# the settings people change per effect kind: (key, label, lo, hi, step, default)
EFFECT_FIELDS = {
    "focus": [("count", "本数", 10, 600, 10, 90), ("inner_r", "中心の空き（mm）", 1, 200, 1, None),
              ("jitter", "ばらつき", 0, 1, 0.05, 0.25), ("width_mm", "太さ（mm）", 0.05, 5, 0.05, 0.8)],
    "speed": [("count", "本数", 5, 400, 5, 40), ("angle", "向き（°）", -180, 180, 5, 0), ("length", "長さ", 0.05, 1, 0.05, 0.7),
              ("curve", "曲がり（mm）", -80, 80, 1, 0), ("jitter", "ばらつき", 0, 1, 0.05, 0.25), ("width_mm", "太さ（mm）", 0.05, 5, 0.05, 0.5)],
    "uni_flash": [("count", "本数", 20, 800, 10, 140), ("inner_r", "中心の空き（mm）", 1, 200, 1, None),
                  ("length_mm", "線の長さ（mm）", 2, 150, 1, None), ("jitter", "ばらつき", 0, 1, 0.05, 0.25),
                  ("width_mm", "太さ（mm）", 0.05, 3, 0.05, 0.35)],
    "beta_flash": [("spikes", "トゲの数", 10, 400, 5, 70), ("inner_r", "中心の空き（mm）", 1, 200, 1, None),
                   ("depth", "トゲの長さ", 0.05, 1, 0.05, 0.45), ("jitter", "ばらつき", 0, 1, 0.05, 0.25)],
    "white": [],
}


def _icon(image) -> QIcon:
    rgb = image.convert("RGB")
    data = rgb.tobytes("raw", "RGB")
    qimage = QImage(data, rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888).copy()
    return QIcon(QPixmap.fromImage(qimage))


class MaterialPanel(QWidget):
    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self._loading = False
        self._icons: dict[str, QIcon] = {}
        # --- materials
        self.folder = QComboBox()
        self.folder.currentIndexChanged.connect(lambda _: self._fill_list())
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.ViewMode.IconMode)
        self.list.setIconSize(QSize(56, 56))
        self.list.setGridSize(QSize(84, 92))
        self.list.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.list.setWordWrap(True)
        self.list.setMinimumHeight(240)
        self.list.itemDoubleClicked.connect(lambda _: self.use())
        use = QPushButton("貼る")
        use.setToolTip("トーン: 選択範囲か選んだコマに（なければクリックした所に）。効果線: 選んだコマに。画像・パーツ: クリックした所に")
        use.clicked.connect(self.use)
        more = QGridLayout()
        for i, (title, slot, tip) in enumerate((("画像を追加…", self.import_image, "画像ファイルを素材にします"),
                                 ("範囲を登録…", self.register_selection, "選んだ範囲の線と塗りを素材（パーツ）にします"),
                                 ("名前…", self.rename, "名前とフォルダを変えます"), ("消す", self.delete, "自分で登録した素材を消します"))):
            button = QPushButton(title)
            button.setToolTip(tip)
            button.clicked.connect(slot)
            more.addWidget(button, i // 2, i % 2)
        new_folder = QPushButton("＋フォルダ…")
        new_folder.setToolTip("素材を分けるフォルダを作ります")
        new_folder.clicked.connect(self.new_folder)
        mat_box = QGroupBox("素材（ダブルクリックで貼る）")
        ml = QVBoxLayout(mat_box)
        row = QHBoxLayout()
        row.addWidget(self.folder, 1)
        row.addWidget(new_folder)
        ml.addLayout(row)
        ml.addWidget(self.list)
        ml.addWidget(use)
        ml.addLayout(more)
        # --- the tone being worked on
        self.tone_label = QLabel()
        self.pattern = QComboBox()
        for label, key in PATTERNS:
            self.pattern.addItem(label, key)
        self.pattern.activated.connect(lambda _: self._tone({"pattern": self.pattern.currentData()}))
        self.lpi = QDoubleSpinBox()
        self.lpi.setRange(5, 300)
        self.lpi.setSuffix(" 線")
        self.lpi.editingFinished.connect(lambda: self._tone({"lpi": self.lpi.value()}))
        self.density = QSpinBox()
        self.density.setRange(0, 100)
        self.density.setSuffix(" %")
        self.density.editingFinished.connect(lambda: self._tone({"density": self.density.value() / 100}))
        self.angle = QDoubleSpinBox()
        self.angle.setRange(-180, 180)
        self.angle.setSuffix("°")
        self.angle.editingFinished.connect(lambda: self._tone({"angle": self.angle.value()}))
        self.gradient = QComboBox()
        for label, key in (("なし", ""), ("直線", "linear"), ("円", "radial")):
            self.gradient.addItem(label, key)
        self.gradient.activated.connect(lambda _: self._gradient())
        self.g_angle = QDoubleSpinBox()
        self.g_angle.setRange(-180, 180)
        self.g_angle.setSuffix("°")
        self.g_angle.setToolTip("90 で上から下へ")
        self.g_start = QSpinBox()
        self.g_end = QSpinBox()
        for spin in (self.g_start, self.g_end):
            spin.setRange(0, 100)
            spin.setSuffix(" %")
        for widget in (self.g_angle, self.g_start, self.g_end):
            widget.editingFinished.connect(self._gradient)
        self.soft = QCheckBox("消しゴムでぼかして削る")
        tone_form = QFormLayout()
        tone_form.addRow("", self.tone_label)
        tone_form.addRow("模様", self.pattern)
        tone_form.addRow("線数", self.lpi)
        tone_form.addRow("濃さ", self.density)
        tone_form.addRow("角度", self.angle)
        tone_form.addRow("グラデーション", self.gradient)
        tone_form.addRow("　向き", self.g_angle)
        tone_form.addRow("　始まり", self.g_start)
        tone_form.addRow("　終わり", self.g_end)
        tone_form.addRow("", self.soft)
        self.tone_box = QGroupBox("トーン（ペンで足す・消しゴムで削る）")
        self.tone_box.setLayout(tone_form)
        # --- effect lines on the page
        self.effects = QListWidget()
        self.effects.setMaximumHeight(96)
        self.effects.currentRowChanged.connect(lambda _: self._effect_picked())
        self.effect_form = QFormLayout()
        self.effect_fields: dict[str, QDoubleSpinBox] = {}
        effect_buttons = QHBoxLayout()
        for title, slot in (("線にする", self.effect_to_layer), ("消す", self.delete_effect)):
            button = QPushButton(title)
            button.clicked.connect(slot)
            effect_buttons.addWidget(button)
        self.effect_box = QGroupBox("このページの効果線（K）")
        self.effect_box.setToolTip("効果線ツール（K）でコマの中をクリックすると入ります。中心の＋をドラッグで動かします")
        el = QVBoxLayout(self.effect_box)
        el.addWidget(self.effects)
        el.addLayout(self.effect_form)
        el.addLayout(effect_buttons)
        # three pages in the panel, so none of them needs a long scroll
        from PySide6.QtWidgets import QTabWidget

        self.tabs = QTabWidget()
        for box, title in ((mat_box, "素材"), (self.tone_box, "トーン"), (self.effect_box, "効果線")):
            page = QWidget()
            pl = QVBoxLayout(page)
            pl.setContentsMargins(0, 0, 0, 0)
            box.setFlat(True)  # (the tab already frames it)
            box.layout().setContentsMargins(2, 4, 2, 2)
            pl.addWidget(box)
            pl.addStretch(1)
            self.tabs.addTab(page, title)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.tabs)
        self._fill_folders()

    # --- materials ------------------------------------------------------------------------------------------

    def _fill_folders(self) -> None:
        keep = self.folder.currentData()
        self.folder.blockSignals(True)
        self.folder.clear()
        self.folder.addItem("すべて", "")
        for name in materials.folders():
            self.folder.addItem(name, name)
        index = self.folder.findData(keep)
        self.folder.setCurrentIndex(max(0, index))
        self.folder.blockSignals(False)
        self._fill_list()

    def _fill_list(self) -> None:
        folder = self.folder.currentData()
        keep = self.current_material()
        self.list.clear()
        for item in materials.all_materials():
            if folder and (item.get("folder") or "その他") != folder:
                continue
            entry = QListWidgetItem(item.get("name") or item["id"])
            entry.setData(Qt.ItemDataRole.UserRole, item["id"])
            entry.setToolTip(f"{KIND_WORD.get(item.get('kind'), '')} ・ {item.get('folder') or ''}"
                             + ("" if item.get("builtin") else " ・ マイ素材"))
            if item["id"] not in self._icons:
                try:
                    self._icons[item["id"]] = _icon(materials.thumbnail(item, 56))
                except Exception:  # a missing picture must not take the panel down
                    self._icons[item["id"]] = QIcon()
            entry.setIcon(self._icons[item["id"]])
            self.list.addItem(entry)
            if keep and item["id"] == keep["id"]:
                self.list.setCurrentItem(entry)

    def current_material(self) -> dict | None:
        entry = self.list.currentItem()
        if entry is None:
            return None
        try:
            return materials.get_material(entry.data(Qt.ItemDataRole.UserRole))
        except KeyError:
            return None

    def select_material(self, material_id: str) -> None:
        for row in range(self.list.count()):
            if self.list.item(row).data(Qt.ItemDataRole.UserRole) == material_id:
                self.list.setCurrentRow(row)
                return

    def use(self) -> None:
        item = self.current_material()
        if item is None:
            self.window.flash("先に素材を選びます", 2500)
            return
        self.window.use_material(item)

    def import_image(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "画像を素材に取り込む", "", "画像 (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)")
        if not path:
            return
        folder = self.folder.currentData() or "画像"
        try:
            item = materials.import_image(path, folder=folder)
        except Exception as exc:
            self.window.flash(f"取り込めませんでした（{exc}）", 6000, error=True)
            return
        self._fill_folders()
        self.select_material(item["id"])
        self.window.flash(f"「{item['name']}」を素材にしました", 2500)

    def register_selection(self) -> None:
        items = self.window.copy_selection_items()
        if items is None:
            return
        name, ok = QInputDialog.getText(self, "素材に登録", "名前")
        if not ok or not name.strip():
            return
        folder = self.folder.currentData() or "マイ素材"
        item = materials.add_material(name, "lines", folder, items=items)
        self._fill_folders()
        self.select_material(item["id"])
        self.window.flash(f"「{item['name']}」を素材にしました（素材 → 貼る）", 3000)

    def rename(self) -> None:
        item = self.current_material()
        if item is None or item.get("builtin"):
            self.window.flash("名前を変えられるのは自分で登録した素材です", 2500)
            return
        name, ok = QInputDialog.getText(self, "素材の名前", "名前", text=item.get("name", ""))
        if not ok:
            return
        folders = materials.folders()
        folder, ok = QInputDialog.getItem(self, "素材のフォルダ", "フォルダ", folders,
                                          max(0, folders.index(item.get("folder")) if item.get("folder") in folders else 0), True)
        if ok:
            materials.update_material(item["id"], name=name or item["name"], folder=folder or item.get("folder", "マイ素材"))
            self._icons.pop(item["id"], None)
            self._fill_folders()

    def delete(self) -> None:
        item = self.current_material()
        if item is None:
            return
        if item.get("builtin"):
            self.window.flash("最初から入っている素材は消せません", 2500)
            return
        answer = QMessageBox.question(self, "Genko", f"素材「{item.get('name')}」を消しますか？（原稿に貼ったものは残ります）")
        if answer == QMessageBox.StandardButton.Yes:
            materials.delete_material(item["id"])
            self._fill_folders()

    def new_folder(self) -> None:
        name, ok = QInputDialog.getText(self, "フォルダを作る", "フォルダの名前")
        if ok and name.strip():
            materials.add_folder(name)
            self._fill_folders()
            self.folder.setCurrentIndex(self.folder.findData(name.strip()))

    # --- the tone -----------------------------------------------------------------------------------------

    def _tone_layer(self):
        layer = self.window.target_layer()
        kind = getattr(getattr(layer, "kind", None), "value", "")
        return layer if kind == "tone" else None

    def refresh(self) -> None:
        self._loading = True
        layer = self._tone_layer()
        self.tone_box.setEnabled(layer is not None)
        if layer is None:
            self.tone_label.setText("レイヤー パネルでトーンのレイヤーを選ぶと、ここで変えられます")
        else:
            from genko.tones import settings

            tone = settings(layer)
            self.tone_label.setText(f"「{layer.title or 'トーン'}」")
            self.pattern.setCurrentIndex(max(0, self.pattern.findData(tone["pattern"])))
            self.lpi.setValue(tone["lpi"])
            self.density.setValue(round(tone["density"] * 100))
            self.angle.setValue(tone["angle"])
            gradient = tone.get("gradient") or {}
            self.gradient.setCurrentIndex(max(0, self.gradient.findData(gradient.get("shape", "") if gradient else "")))
            self.g_angle.setValue(float(gradient.get("angle", 90)))
            self.g_start.setValue(round(float(gradient.get("start", tone["density"])) * 100))
            self.g_end.setValue(round(float(gradient.get("end", 0)) * 100))
        for widget in (self.g_angle, self.g_start, self.g_end):
            widget.setEnabled(bool(self.gradient.currentData()))
        self._loading = False
        self._fill_effects()

    def _tone(self, change: dict) -> None:
        layer = self._tone_layer()
        if self._loading or layer is None:
            return
        self.window.apply_ops([{"op": "set_tone", "page": self.window.current_page().index, "id": layer.id, **change}])

    def _gradient(self) -> None:
        shape = self.gradient.currentData()
        for widget in (self.g_angle, self.g_start, self.g_end):
            widget.setEnabled(bool(shape))
        if not shape:
            self._tone({"gradient": None})
            return
        if self.g_start.value() == self.g_end.value():
            self._loading = True
            self.g_start.setValue(self.density.value())
            self.g_end.setValue(0)
            self._loading = False
        self._tone({"gradient": {"shape": shape, "angle": self.g_angle.value(), "start": self.g_start.value() / 100,
                                 "end": self.g_end.value() / 100}})

    # --- effect lines ---------------------------------------------------------------------------------------

    def _fill_effects(self) -> None:
        page = self.window.current_page()
        keep = self.window.canvas.selected_effect_id
        self._loading = True
        self.effects.clear()
        for effect in (page.effects if page else []):
            label = effects.LABELS.get(effect.get("kind"), effect.get("kind"))
            if effect.get("visible") is False:
                label += "（隠す）"
            entry = QListWidgetItem(label)
            entry.setData(Qt.ItemDataRole.UserRole, effect.get("id"))
            self.effects.addItem(entry)
            if effect.get("id") == keep:
                self.effects.setCurrentItem(entry)
        self._loading = False
        self._show_effect()

    def _effect(self) -> dict | None:
        page, entry = self.window.current_page(), self.effects.currentItem()
        if page is None or entry is None:
            return None
        return next((e for e in page.effects if e.get("id") == entry.data(Qt.ItemDataRole.UserRole)), None)

    def select_effect(self, effect_id: str) -> None:
        for row in range(self.effects.count()):
            if self.effects.item(row).data(Qt.ItemDataRole.UserRole) == effect_id:
                self.effects.setCurrentRow(row)
                return

    def _effect_picked(self) -> None:
        if self._loading:
            return
        effect = self._effect()
        self.window.canvas.selected_effect_id = effect.get("id") if effect else None
        self.window.canvas.update()
        self._show_effect()

    def _show_effect(self) -> None:
        while self.effect_form.rowCount():
            self.effect_form.removeRow(0)
        self.effect_fields = {}
        effect = self._effect()
        if effect is None:
            return
        params = effect.get("params") or {}
        _, box = effects.area(effect, self.window.current_page())
        for key, label, lo, hi, step, default in EFFECT_FIELDS.get(effect.get("kind"), []):
            spin = QDoubleSpinBox()
            spin.setRange(lo, hi)
            spin.setSingleStep(step)
            spin.setDecimals(0 if step >= 1 else 2)
            if key == "inner_r":
                rx, _ry = effects._inner(params, box)
                value = rx
            elif key == "length_mm":
                value = float(params.get(key, max(8.0, min(box[2], box[3]) * 0.18)))
            else:
                value = float(params.get(key, default if default is not None else lo))
            spin.setValue(value)
            spin.editingFinished.connect(lambda k=key, sp=spin: self._effect_set(k, sp.value()))
            self.effect_form.addRow(label, spin)
            self.effect_fields[key] = spin

    def _effect_set(self, key: str, value: float) -> None:
        effect = self._effect()
        if effect is None:
            return
        if key == "inner_r":
            _, box = effects.area(effect, self.window.current_page())
            rx, ry = effects._inner(effect.get("params") or {}, box)
            change = {"inner": [round(value, 2), round(value * ry / max(rx, 1e-6), 2)]}
        elif key in ("count", "spikes"):
            change = {key: int(value)}
        else:
            change = {key: round(value, 3)}
        self.window.apply_ops([{"op": "edit_effect", "page": self.window.current_page().index, "id": effect["id"], "params": change}])

    def effect_to_layer(self) -> None:
        effect = self._effect()
        layer = self.window._paint_layer()
        if effect is None or layer is None:
            return
        if self.window.apply_ops([{"op": "effect_to_layer", "page": self.window.current_page().index, "id": effect["id"],
                                   "layer_id": layer.id}]):
            self.window.flash("効果線を線にしました。消しゴムやペンで手を入れられます", 3500)

    def delete_effect(self) -> None:
        effect = self._effect()
        if effect is not None:
            self.window.apply_ops([{"op": "delete_effect", "page": self.window.current_page().index, "id": effect["id"]}])
            self.window.canvas.selected_effect_id = None

