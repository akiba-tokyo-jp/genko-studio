"""Studio panels for the person: process bar, approval box, panel view, library.

The widgets only read the session's episode and send ops through `apply`
(the main window's session). Nothing here approves on its own: every approval
is a button a person presses on one request after looking at it.
"""

from __future__ import annotations

import io
from pathlib import Path
from typing import Callable

from PIL import Image
from PySide6.QtCore import QPointF, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from genko.app import review_model
from genko.ops import ApplyError

Apply = Callable[[list[dict]], bool]


def to_pixmap(image: Image.Image) -> QPixmap:
    buf = io.BytesIO()
    image.convert("RGB").save(buf, format="PNG")
    return QPixmap.fromImage(QImage.fromData(buf.getvalue()))


def asset_image(project: Path | None, ref: str | None) -> Image.Image | None:
    if not project or not ref:
        return None
    from genko.assets import AssetStore

    data = AssetStore(project).get_bytes(ref, ".png")
    return Image.open(io.BytesIO(data)) if data else None


# --- process bar -------------------------------------------------------------------------------------


class ProcessBar(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.layout_ = QHBoxLayout(self)
        self.layout_.setContentsMargins(6, 2, 6, 2)
        self.labels: list[QLabel] = []

    def refresh(self, episode) -> None:
        for label in self.labels:
            self.layout_.removeWidget(label)
            label.setParent(None)  # gone now, not at the next event loop turn
            label.deleteLater()
        self.labels = []
        for text, count in review_model.progress(episode):
            label = QLabel(f"{text} <b>{count}</b>")
            label.setStyleSheet("padding:2px 8px;border-radius:8px;background:%s" % ("#ffe2b8" if count and "待ち" in text else "#e8e8e8"))
            self.layout_.addWidget(label)
            self.labels.append(label)
        pending = len(review_model.inbox(episode))
        label = QLabel(f"承認箱 <b>{pending}</b>")
        label.setStyleSheet("padding:2px 8px;border-radius:8px;background:%s" % ("#ffc9c9" if pending else "#e8e8e8"))
        self.layout_.addWidget(label)
        self.labels.append(label)


# --- approval box ------------------------------------------------------------------------------------


class ApprovalBox(QWidget):
    """Requests from the agent. Look at one, then approve it or send it back with a reason."""

    changed = Signal()

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.items: list[review_model.InboxItem] = []
        self.list = QListWidget()
        self.list.currentRowChanged.connect(self._show)
        self.detail = QLabel()
        self.detail.setWordWrap(True)
        self.page_pick = QComboBox()
        self.page_pick.currentIndexChanged.connect(lambda _: self._preview())
        self.preview = QLabel()
        self.preview.setMinimumHeight(260)
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.choices = QListWidget()
        self.choices.setViewMode(QListWidget.ViewMode.IconMode)
        self.choices.setIconSize(QSize(120, 160))
        self.choices.setMaximumHeight(200)
        self.reason = QLineEdit()
        self.reason.setPlaceholderText("差し戻しの理由（エージェントへの指示になる）")
        self.approve_button = QPushButton("承認")
        self.approve_button.clicked.connect(self.approve)
        self.back_button = QPushButton("差し戻し")
        self.back_button.clicked.connect(self.send_back)
        buttons = QHBoxLayout()
        buttons.addWidget(self.approve_button)
        buttons.addWidget(self.back_button)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("承認箱（1 件ずつ見て決める）"))
        layout.addWidget(self.list, 1)
        layout.addWidget(self.detail)
        layout.addWidget(self.page_pick)
        layout.addWidget(self.preview, 2)
        layout.addWidget(self.choices)
        layout.addWidget(self.reason)
        layout.addLayout(buttons)

    def refresh(self) -> None:
        episode = self.window.session.episode
        current = self.current()
        self.items = review_model.inbox(episode)
        self.list.blockSignals(True)
        self.list.clear()
        for item in self.items:
            self.list.addItem(f"{item.title}  —  {item.by}")
        self.list.blockSignals(False)
        ids = [i.ticket_id for i in self.items]
        row = ids.index(current.ticket_id) if current and current.ticket_id in ids else (0 if self.items else -1)
        self.list.setCurrentRow(row)
        self._show(row)

    def current(self) -> review_model.InboxItem | None:
        row = self.list.currentRow()
        return self.items[row] if 0 <= row < len(self.items) else None

    def _show(self, _row: int) -> None:
        item = self.current()
        self.choices.clear()
        self.page_pick.blockSignals(True)
        self.page_pick.clear()
        for page in (item.pages if item else []):
            self.page_pick.addItem(f"{page} ページ", page)
        self.page_pick.setVisible(bool(item and len(item.pages) > 1))
        self.page_pick.blockSignals(False)
        if item is None:
            self.choices.setVisible(False)
            self.detail.setText("承認を待っている依頼はない")
            self.preview.clear()
            self.approve_button.setEnabled(False)
            self.back_button.setEnabled(False)
            return
        self.approve_button.setEnabled(True)
        self.back_button.setEnabled(item.gate != "export")
        self.approve_button.setText("書き出す…" if item.gate == "export" else ("閉じる" if item.kind == "help" else "承認"))
        self.back_button.setText("返事をして閉じる" if item.kind == "help" else "差し戻し")
        self.detail.setText(f"<b>{item.title}</b><br>{item.text or ''}<br><small>依頼: {item.by}</small>")
        self.choices.setVisible(item.gate == "sheet")
        if item.gate == "sheet":
            self._sheet_choices(item)
        self._preview()

    def _sheet_choices(self, item) -> None:
        episode = self.window.session.episode
        for cand in (episode.studio.get("character_candidates") or {}).get(item.character_id, []):
            if cand.get("status") == "rejected":
                continue
            image = asset_image(self.window.session.path, cand["asset"])
            entry = QListWidgetItem(cand["id"])
            entry.setData(Qt.ItemDataRole.UserRole, cand["id"])
            if image is not None:
                image.thumbnail((120, 160))
                entry.setIcon(QIcon(to_pixmap(image)))
            self.choices.addItem(entry)
        if self.choices.count():
            self.choices.setCurrentRow(0)

    def _preview(self) -> None:
        item = self.current()
        if item is None:
            return
        episode = self.window.session.episode
        if item.gate == "export":
            from genko.studio import preflight

            report = preflight.check(episode, self.window.session.path)
            text = "書き出せる" if report["ok"] else "止める理由:\n" + "\n".join(e["message"] for e in report["errors"][:12])
            self.preview.setText(text)
            return
        page_index = self.page_pick.currentData() or (item.pages[0] if item.pages else None)
        page = next((p for p in episode.pages if p.index == page_index), None)
        if page is None:
            self.preview.clear()
            return
        from genko.render import render_page

        mode = "name" if item.gate == "name" or not page.name_ok else "proof"
        image = render_page(page, 60, mode=mode, episode=episode)
        pix = to_pixmap(image)
        self.preview.setPixmap(pix.scaled(self.preview.width() or 360, 420, Qt.AspectRatioMode.KeepAspectRatio,
                                          Qt.TransformationMode.SmoothTransformation))

    def approve(self) -> None:
        item = self.current()
        if item is None:
            return
        session = self.window.session
        if item.gate == "export":
            self._export(item)
            return
        choice = self.choices.currentItem().data(Qt.ItemDataRole.UserRole) if self.choices.currentItem() else None
        try:
            ops = review_model.approve_ops(session.episode, item, project=session.path, candidate_id=choice)
        except ValueError as exc:
            QMessageBox.information(self, "Genko", str(exc))
            return
        if self.window.apply_and_commit(ops):
            self.changed.emit()

    def send_back(self) -> None:
        item = self.current()
        if item is None:
            return
        try:
            ops = review_model.send_back_ops(self.window.session.episode, item, self.reason.text())
        except ValueError as exc:
            QMessageBox.information(self, "Genko", str(exc))
            return
        if self.window.apply_and_commit(ops):
            self.reason.clear()
            self.changed.emit()

    def _export(self, item) -> None:
        from genko.studio.service import HumanService

        self.window.commit_now()
        folder = QFileDialog.getExistingDirectory(self, "書き出し先")
        if not folder:
            return
        result = HumanService(self.window.session.path, self.window.session.actor).export("pdf", Path(folder))
        if not result.get("ok"):
            QMessageBox.warning(self, "Genko", "\n".join(e["message"] for e in result.get("errors", [])[:12]) or result.get("error", ""))
        self.window.session.reload()
        self.changed.emit()


# --- panel view -----------------------------------------------------------------------------------------


class RegionImage(QLabel):
    """The panel image; in region mode a drag draws a rectangle (in image fractions)."""

    drawn = Signal(float, float, float, float)

    def __init__(self) -> None:
        super().__init__()
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(320, 240)
        self.drawing = False
        self._start: QPointF | None = None
        self._end: QPointF | None = None
        self.boxes: list[tuple[str, tuple[float, float, float, float]]] = []  # (label, box in 0..1 of the pixmap)

    def _pix_rect(self) -> QRectF:
        pix = self.pixmap()
        if pix is None or pix.isNull():
            return QRectF()
        x = (self.width() - pix.width()) / 2
        y = (self.height() - pix.height()) / 2
        return QRectF(x, y, pix.width(), pix.height())

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if self.drawing:
            self._start = self._end = event.position()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        if self.drawing and self._start is not None:
            self._end = event.position()
            self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if not (self.drawing and self._start is not None):
            return
        area = self._pix_rect()
        a, b = self._start, event.position()
        self._start = self._end = None
        if area.isEmpty():
            return
        x0, x1 = sorted(((a.x() - area.x()) / area.width(), (b.x() - area.x()) / area.width()))
        y0, y1 = sorted(((a.y() - area.y()) / area.height(), (b.y() - area.y()) / area.height()))
        x0, y0, x1, y1 = (min(1.0, max(0.0, v)) for v in (x0, y0, x1, y1))
        if x1 - x0 > 0.01 and y1 - y0 > 0.01:
            self.drawn.emit(x0, y0, x1 - x0, y1 - y0)
        self.update()

    def paintEvent(self, event) -> None:  # noqa: N802
        super().paintEvent(event)
        area = self._pix_rect()
        if area.isEmpty():
            return
        painter = QPainter(self)
        for label, (x, y, w, h) in self.boxes:
            color = QColor("#e03131") if label.startswith("face") else QColor("#1971c2")
            painter.setPen(QPen(color, 2))
            rect = QRectF(area.x() + x * area.width(), area.y() + y * area.height(), w * area.width(), h * area.height())
            painter.drawRect(rect)
            painter.drawText(rect.topLeft() + QPointF(3, 12), label)
        if self._start is not None and self._end is not None:
            painter.setPen(QPen(QColor("#f08c00"), 2, Qt.PenStyle.DashLine))
            painter.drawRect(QRectF(self._start, self._end).normalized())


class PanelView(QWidget):
    """One panel: the adopted art, the name over a candidate, the source image; candidates with their
    provenance; the person's instruction; regions."""

    MODES = [("採用中（印刷）", "print"), ("採用中（校正）", "proof"), ("比較", "compare"), ("元画像", "source")]
    changed = Signal()

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.frame_id: str | None = None
        self.mode = QComboBox()
        for label, key in self.MODES:
            self.mode.addItem(label, key)
        self.mode.currentIndexChanged.connect(lambda _: self._show_image())
        self.image = RegionImage()
        self.image.drawn.connect(self._region_drawn)
        self.info = QLabel()
        self.info.setWordWrap(True)
        self.candidates = QListWidget()
        self.candidates.setViewMode(QListWidget.ViewMode.IconMode)
        self.candidates.setIconSize(QSize(110, 110))
        self.candidates.setMaximumHeight(170)
        self.candidates.currentRowChanged.connect(lambda _: self._candidate_selected())
        self.provenance = QTextEdit()
        self.provenance.setReadOnly(True)
        self.provenance.setMaximumHeight(90)
        adopt = QPushButton("この候補を採用")
        adopt.clicked.connect(self.adopt)
        self.instruction = QLineEdit()
        self.instruction.setPlaceholderText("このコマへの指示（エージェントの作業に出る）")
        send = QPushButton("指示を送る")
        send.clicked.connect(self.send_instruction)
        self.region_kind = QComboBox()
        self.region_kind.addItems(["face", "person", "keep"])
        self.region_button = QPushButton("領域を描く")
        self.region_button.setCheckable(True)
        self.region_button.toggled.connect(self._region_mode)
        self.regions = QListWidget()
        self.regions.setMaximumHeight(80)
        drop = QPushButton("選んだ領域を消す")
        drop.clicked.connect(self.delete_region)
        top = QHBoxLayout()
        top.addWidget(QLabel("コマ表示"))
        top.addWidget(self.mode, 1)
        row = QHBoxLayout()
        row.addWidget(self.instruction, 1)
        row.addWidget(send)
        regions = QHBoxLayout()
        regions.addWidget(self.region_kind)
        regions.addWidget(self.region_button)
        regions.addWidget(drop)
        layout = QVBoxLayout(self)
        layout.addLayout(top)
        layout.addWidget(self.image, 3)
        layout.addWidget(self.info)
        layout.addWidget(QLabel("候補（点数・来歴）"))
        layout.addWidget(self.candidates)
        layout.addWidget(self.provenance)
        layout.addWidget(adopt)
        layout.addLayout(row)
        layout.addLayout(regions)
        layout.addWidget(self.regions)

    # --- data --------------------------------------------------------------------

    def _page(self):
        return self.window.current_page()

    def _frame(self):
        page = self._page()
        if page is None or not self.frame_id:
            return None
        try:
            return page._find(self.frame_id)
        except (KeyError, IndexError):
            return None

    def set_frame(self, frame_id: str | None) -> None:
        self.frame_id = frame_id
        self.refresh()

    def refresh(self) -> None:
        frame = self._frame()
        self.candidates.clear()
        self.regions.clear()
        if frame is None:
            self.info.setText("コマを選ぶ（ページ上でクリック）")
            self.image.clear()
            self.image.boxes = []
            return
        panel = frame.panel or {}
        adopted = panel.get("adopted") or {}
        attempts = panel.get("attempts") or {}
        brief = " / ".join(str(panel.get(k)) for k in ("shot", "angle", "action", "emotion") if panel.get(k))
        human = panel.get("instruction") or {}
        self.info.setText(f"<b>{panel.get('slot') or frame.id}</b>  {panel.get('status', 'empty')}  取り込み {attempts.get('images', 0)} 枚"
                          f"<br>{brief}" + (f"<br>指示: {human.get('text')}" if isinstance(human, dict) and human.get("text") else ""))
        for cand in panel.get("candidates", []):
            entry = QListWidgetItem(("✔ " if cand["id"] in adopted.values() else "") + cand["id"]
                                    + (f"\n{cand['review']['score']}" if cand.get("review") else ""))
            entry.setData(Qt.ItemDataRole.UserRole, cand["id"])
            image = asset_image(self.window.session.path, cand["asset"])
            if image is not None:
                image.thumbnail((110, 110))
                entry.setIcon(QIcon(to_pixmap(image)))
            self.candidates.addItem(entry)
        for region in panel.get("regions", []):
            entry = QListWidgetItem(f"{region.get('kind')} {region.get('char') or ''} ({region.get('source')})")
            entry.setData(Qt.ItemDataRole.UserRole, region.get("id"))
            self.regions.addItem(entry)
        self._show_image()

    def _selected_candidate(self) -> dict | None:
        frame = self._frame()
        entry = self.candidates.currentItem()
        if frame is None or entry is None:
            return None
        cid = entry.data(Qt.ItemDataRole.UserRole)
        return next((c for c in (frame.panel or {}).get("candidates", []) if c["id"] == cid), None)

    def _candidate_selected(self) -> None:
        cand = self._selected_candidate()
        if cand is None:
            self.provenance.clear()
            return
        origin = cand.get("origin") or {}
        review = cand.get("review") or {}
        lines = [f"来歴: {origin.get('kind')} {origin.get('tool_id') or ''} {origin.get('model') or ''}",
                 f"依頼: {cand.get('request') or '-'}  親: {cand.get('parent') or '-'}  {'（指示が変わった後の候補）' if cand.get('stale') else ''}"]
        if origin.get("prompt"):
            lines.append(f"プロンプト: {origin['prompt']}")
        if review:
            lines.append(f"評価（{review.get('by')}）: {review.get('score')} {review.get('note') or ''}")
        self.provenance.setPlainText("\n".join(lines))
        if self.mode.currentData() in ("compare", "source"):
            self._show_image()

    # --- images ---------------------------------------------------------------------

    def _show_image(self) -> None:
        frame, page = self._frame(), self._page()
        if frame is None or page is None:
            return
        session = self.window.session
        mode = self.mode.currentData()
        image = None
        box_source = None
        cand = self._selected_candidate()
        panel = frame.panel or {}
        if cand is None:
            cand = next((c for c in panel.get("candidates", []) if c["id"] == (panel.get("adopted") or {}).get("art")), None)
        try:
            if mode in ("print", "proof"):
                from genko.render import render_frame

                image = render_frame(page, frame.id, 150, mode=mode, episode=session.episode)
                box_source = "frame"
            elif mode == "compare" and cand is not None:
                from genko.studio.service import _render_kind

                png, _ = _render_kind(session.episode, session.path, page.index, frame.id, "compare", cand["id"], 480)
                image = Image.open(io.BytesIO(png))
            elif mode == "source" and cand is not None:
                image = asset_image(session.path, cand["asset"])
        except ApplyError:
            image = None
        if image is None:
            self.image.setText("（画像なし）")
            self.image.boxes = []
            return
        pix = to_pixmap(image).scaled(max(320, self.image.width()), max(240, self.image.height()),
                                      Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
        self.image.setPixmap(pix)
        self.image.boxes = self._region_boxes(frame) if box_source == "frame" else []
        self.image.update()

    def _region_boxes(self, frame) -> list:
        r = frame.rect
        out = []
        for region in (frame.panel or {}).get("regions", []):
            rect = region.get("rect_mm")
            if rect and len(rect) == 4:
                out.append((f"{region.get('kind')} {region.get('char') or ''}".strip(),
                            ((rect[0] - r.x) / r.width, (rect[1] - r.y) / r.height, rect[2] / r.width, rect[3] / r.height)))
        return out

    # --- actions --------------------------------------------------------------------------

    def adopt(self) -> None:
        page, cand = self._page(), self._selected_candidate()
        if page is None or cand is None:
            return
        to = "ink" if (cand.get("origin") or {}).get("tool_id") == "genko:lineart" else "art"
        if self.window.apply_ops([{"op": "adopt_candidate", "page": page.index, "frame_id": self.frame_id, "candidate_id": cand["id"], "to": to}]):
            self.changed.emit()

    def send_instruction(self) -> None:
        page, text = self._page(), self.instruction.text().strip()
        if page is None or not self.frame_id or not text:
            return
        ops = [{"op": "set_panel", "page": page.index, "frame_id": self.frame_id,
                "set": {"instruction": {"text": text, "by": self.window.session.actor}}, "pin": ["instruction"]},
               {"op": "request_fix", "page": page.index, "frame_id": self.frame_id, "instruction": text, "scope": "frame"}]
        if self.window.apply_ops(ops):
            self.instruction.clear()
            self.changed.emit()

    def _region_mode(self, on: bool) -> None:
        self.image.drawing = on
        if on:
            self.mode.setCurrentIndex(1)  # regions are drawn on the panel as it prints (proof)

    def _region_drawn(self, x: float, y: float, w: float, h: float) -> None:
        frame, page = self._frame(), self._page()
        if frame is None or page is None:
            return
        r = frame.rect
        rect = [round(r.x + x * r.width, 2), round(r.y + y * r.height, 2), round(w * r.width, 2), round(h * r.height, 2)]
        self.region_button.setChecked(False)
        if self.window.apply_ops([{"op": "add_region", "page": page.index, "frame_id": frame.id,
                                   "region": {"kind": self.region_kind.currentText(), "rect_mm": rect}}]):
            self.changed.emit()

    def delete_region(self) -> None:
        page, entry = self._page(), self.regions.currentItem()
        if page is None or entry is None:
            return
        if self.window.apply_ops([{"op": "delete_region", "page": page.index, "frame_id": self.frame_id,
                                   "id": entry.data(Qt.ItemDataRole.UserRole)}]):
            self.changed.emit()


# --- library ------------------------------------------------------------------------------------------------


class Library(QWidget):
    """Characters (sheets, faces, lock) and locations (reference images), with provenance."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.ViewMode.IconMode)
        self.list.setIconSize(QSize(96, 128))
        self.list.currentRowChanged.connect(lambda _: self._show())
        self.detail = QTextEdit()
        self.detail.setReadOnly(True)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("キャラクターと背景"))
        layout.addWidget(self.list, 2)
        layout.addWidget(self.detail, 1)
        self.entries: list[dict] = []

    def refresh(self) -> None:
        session = self.window.session
        episode = session.episode
        self.list.clear()
        self.entries = []
        for char in episode.bible.characters:
            refs = {r.get("kind"): r.get("asset") for r in char.get("refs", [])}
            label = f"{char.get('name', char.get('id'))}\n{'承認済み' if char.get('locked') else '未承認'}"
            self._add(label, refs.get("face") or refs.get("sheet"), {"kind": "character", **char})
        for loc in episode.studio.get("locations", []):
            asset = next((r.get("asset") for r in loc.get("refs", [])), None)
            self._add(f"{loc.get('name') or loc.get('id')}\n場所", asset, {"kind": "location", **loc})

    def _add(self, label: str, asset: str | None, data: dict) -> None:
        entry = QListWidgetItem(label)
        image = asset_image(self.window.session.path, asset)
        if image is not None:
            image.thumbnail((96, 128))
            entry.setIcon(QIcon(to_pixmap(image)))
        self.list.addItem(entry)
        self.entries.append(data)

    def _show(self) -> None:
        row = self.list.currentRow()
        if not 0 <= row < len(self.entries):
            self.detail.clear()
            return
        data = self.entries[row]
        lines = [f"{data.get('kind')}: {data.get('id')}"]
        look = data.get("look") or {}
        if look:
            lines.append("見た目: " + "、".join(str(v) for k, v in look.items() if isinstance(v, str) and v))
        if data.get("tokens_en"):
            lines.append(f"tokens_en: {data['tokens_en']}")
        for ref in data.get("refs", []):
            lines.append(f"参照 {ref.get('kind')}: {ref.get('asset', '')[:19]}…  {ref.get('approved_by') or ref.get('by') or ''}")
        self.detail.setPlainText("\n".join(lines))
