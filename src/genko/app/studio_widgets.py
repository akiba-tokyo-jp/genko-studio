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
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSizePolicy,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from genko.app import review_model, wording
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


def _brief_words(panel: dict) -> list[str]:
    """shot / angle in words, then the action and the emotion."""
    from genko.studio.genreq import vocab

    v = vocab()
    out = []
    for key, table in (("shot", "shot"), ("angle", "angle")):
        code = panel.get(key)
        if code:
            out.append(str((v.get(table, {}).get(code) or {}).get("ja") or code))
    out += [str(panel[k]) for k in ("action", "emotion") if panel.get(k)]
    return out


# --- process bar -------------------------------------------------------------------------------------


class ProcessBar(QWidget):
    """How far the book is: only the steps that have pages, and the approval box count."""

    def __init__(self) -> None:
        super().__init__()
        self.layout_ = QHBoxLayout(self)
        self.layout_.setContentsMargins(6, 2, 6, 2)
        self.layout_.setSpacing(6)
        self.labels: list[QLabel] = []

    def refresh(self, episode) -> None:
        for label in self.labels:
            self.layout_.removeWidget(label)
            label.setParent(None)  # gone now, not at the next event loop turn
            label.deleteLater()
        self.labels = []
        pending = len(review_model.inbox(episode))
        chips = [(f"承認箱 <b>{pending}</b>", "#ffc9c9" if pending else "#e9ecef",
                  "エージェントからの承認依頼と相談。右の「承認箱」で 1 件ずつ見て決めます")]
        for text, count in review_model.progress(episode):
            if count:
                chips.append((f"{text} <b>{count}</b>", "#ffe8cc" if "待ち" in text or "未承認" in text else "#e9ecef", ""))
        for text, color, tip in chips:
            label = QLabel(text)
            label.setStyleSheet(f"padding:1px 8px;border-radius:8px;background:{color}")
            if tip:
                label.setToolTip(tip)
            self.layout_.addWidget(label)
            self.labels.append(label)


# --- approval box ------------------------------------------------------------------------------------

HOW = {
    "name": "コマ割り・読み順・台詞の位置を見ます。大きく見る で 1 ページずつ確かめられます。",
    "art": "印刷と同じ見た目（網点は平らなグレー）で、絵がネームどおりか、人物が設定画に似ているかを見ます。",
    "sheet": "候補から 1 枚選んで承認します。選んだ絵の顔のアップが、以後の作画の参照になります。",
    "export": "書き出す前の点検結果です。止める理由が無ければ、書き出す… で正式に書き出します。",
    "help": "エージェントからの相談です。返事を書いて「返事を送る」と、エージェントの作業に指示として届きます。",
    "proposal": "アタリから読み取った提案です。確定するまで原稿は変わりません。",
}


class PreviewLabel(QLabel):
    """A preview that keeps its image fitted to the space it has; double-click opens the viewer."""

    activated = Signal()

    def __init__(self) -> None:
        super().__init__()
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumHeight(120)
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Ignored)
        self._source: QPixmap | None = None
        self.setToolTip("ダブルクリックで大きく見る")

    def set_image(self, pix: QPixmap | None) -> None:
        self._source = pix
        self._fit()

    def _fit(self) -> None:
        if self._source is None or self._source.isNull():
            self.setPixmap(QPixmap())
            return
        self.setPixmap(self._source.scaled(max(40, self.width() - 4), max(40, self.height() - 4), Qt.AspectRatioMode.KeepAspectRatio,
                                           Qt.TransformationMode.SmoothTransformation))

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        self._fit()

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        self.activated.emit()


class ApprovalBox(QWidget):
    """Requests from the agent. Look at one, then approve it or send it back with a reason."""

    changed = Signal()

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.items: list[review_model.InboxItem] = []
        self.list = QListWidget()
        self.list.setMaximumHeight(120)
        self.list.currentRowChanged.connect(self._show)
        self.detail = QLabel()
        self.detail.setWordWrap(True)
        self.detail.setTextFormat(Qt.TextFormat.RichText)
        self.page_pick = QComboBox()
        self.page_pick.currentIndexChanged.connect(lambda _: self._page_changed())
        self.preview = PreviewLabel()
        self.preview.activated.connect(self.open_viewer)
        self.big = QPushButton("大きく見る")
        self.big.clicked.connect(self.open_viewer)
        self.choices = QListWidget()
        self.choices.setViewMode(QListWidget.ViewMode.IconMode)
        self.choices.setIconSize(QSize(120, 150))
        self.choices.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.choices.setMaximumHeight(200)
        self.choices.itemDoubleClicked.connect(lambda _: self.open_viewer())
        self.reason = QLineEdit()
        self.approve_button = QPushButton("承認")
        self.approve_button.clicked.connect(self.approve)
        self.back_button = QPushButton("差し戻し")
        self.back_button.clicked.connect(self.send_back)
        buttons = QHBoxLayout()
        buttons.addWidget(self.back_button)
        buttons.addStretch(1)
        buttons.addWidget(self.approve_button)
        head = QHBoxLayout()
        head.addWidget(self.page_pick, 1)
        head.addWidget(self.big)
        self.empty = QLabel("承認を待っている依頼はありません。\nエージェントが依頼を出すと、ここに届きます。")
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.empty.setStyleSheet("color:#777")
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("エージェントからの依頼（1 件ずつ見て決める）"))
        layout.addWidget(self.list)
        layout.addWidget(self.empty)
        layout.addWidget(self.detail)
        layout.addLayout(head)
        layout.addWidget(self.preview, 3)
        layout.addWidget(self.choices)
        layout.addWidget(self.reason)
        layout.addLayout(buttons)
        self.approve_button.setStyleSheet("font-weight:bold;padding:4px 16px")

    def refresh(self) -> None:
        episode = self.window.session.episode
        current = self.current()
        self.items = review_model.inbox(episode)
        self.list.blockSignals(True)
        self.list.clear()
        for item in self.items:
            self.list.addItem(item.title)
        self.list.blockSignals(False)
        ids = [i.ticket_id for i in self.items]
        row = ids.index(current.ticket_id) if current and current.ticket_id in ids else (0 if self.items else -1)
        self.list.setCurrentRow(row)
        self._show(row, follow=False)

    def current(self) -> review_model.InboxItem | None:
        row = self.list.currentRow()
        return self.items[row] if 0 <= row < len(self.items) else None

    def _kind(self, item) -> str:
        return item.kind if item.kind in ("help", "proposal") else (item.gate or "")

    def _show(self, _row: int, follow: bool = True) -> None:
        item = self.current()
        self.choices.clear()
        self.page_pick.blockSignals(True)
        self.page_pick.clear()
        for page in (item.pages if item else []):
            self.page_pick.addItem(f"{page} ページ", page)
        self.page_pick.setVisible(bool(item and len(item.pages) > 1))
        self.page_pick.blockSignals(False)
        has = item is not None
        for widget in (self.list, self.detail, self.preview, self.reason, self.approve_button, self.back_button, self.big):
            widget.setVisible(has)
        self.empty.setVisible(not has)
        if item is None:
            self.choices.setVisible(False)
            self.preview.set_image(None)
            return
        kind = self._kind(item)
        self.approve_button.setEnabled(True)
        self.back_button.setEnabled(item.gate != "export")
        self.back_button.setVisible(item.gate != "export")
        self.approve_button.setText({"export": "書き出す…", "help": "返事なしで閉じる", "proposal": "確定"}.get(kind, "承認"))
        self.back_button.setText({"help": "返事を送る", "proposal": "却下"}.get(kind, "差し戻す"))
        self.reason.setPlaceholderText({"help": "返事（エージェントへの指示になる）", "proposal": "却下の理由"}.get(
            kind, "差し戻す理由（エージェントへの指示になる）"))
        self.reason.setVisible(item.gate != "export")
        text = f"<b>{item.title}</b>"
        if item.text:
            text += f"<br>{item.text}"
        text += f"<br><small style='color:#555'>{HOW.get(kind, '')}</small>"
        text += f"<br><small style='color:#888'>依頼: {wording.actor(item.by)}</small>"
        self.detail.setText(text)
        self.choices.setVisible(item.gate == "sheet" and item.kind == "gate")
        self.big.setVisible(item.gate != "export")
        sheet = item.gate == "sheet" and item.kind == "gate"
        self.preview.setVisible(not sheet)  # a sheet is chosen from the candidates, which get the space
        self.choices.setMaximumHeight(16777215 if sheet else 200)
        if sheet:
            self._sheet_choices(item)
        if follow and item.pages:
            self.window.go_to_page(item.pages[0])
        self._preview()

    def _page_changed(self) -> None:
        page = self.page_pick.currentData()
        if page:
            self.window.go_to_page(page)
        self._preview()

    def _sheet_candidates(self, item) -> list[dict]:
        episode = self.window.session.episode
        return [c for c in (episode.studio.get("character_candidates") or {}).get(item.character_id, [])
                if c.get("status") != "rejected"]

    def _sheet_choices(self, item) -> None:
        for n, cand in enumerate(self._sheet_candidates(item), 1):
            image = asset_image(self.window.session.path, cand["asset"])
            entry = QListWidgetItem(f"候補 {n}")
            entry.setData(Qt.ItemDataRole.UserRole, cand["id"])
            entry.setToolTip(cand["id"])
            if image is not None:
                image.thumbnail((150, 190))
                entry.setIcon(QIcon(to_pixmap(image)))
            self.choices.addItem(entry)
        if self.choices.count():
            self.choices.setCurrentRow(0)

    def _page(self, item):
        episode = self.window.session.episode
        page_index = self.page_pick.currentData() or (item.pages[0] if item.pages else None)
        return next((p for p in episode.pages if p.index == page_index), None)

    def _page_image(self, item, page, dpi: int):
        episode = self.window.session.episode
        from genko.render import render_page

        if item.kind == "proposal":
            from genko.studio.atari import overlay_image

            return overlay_image(episode, page, dpi)
        mode = "name" if item.gate == "name" or not page.name_ok else "proof"
        return render_page(page, dpi, mode=mode, episode=episode)

    def _preview(self) -> None:
        item = self.current()
        if item is None:
            return
        episode = self.window.session.episode
        if item.gate == "export":
            from genko.studio import preflight

            report = preflight.check(episode, self.window.session.path)
            text = "点検は通っています。書き出せます。" if report["ok"] else "止めている理由:\n" + "\n".join(e["message"] for e in report["errors"][:12])
            self.preview.set_image(None)
            self.preview.setText(text)
            self.approve_button.setEnabled(report["ok"])
            return
        if item.gate == "sheet" and item.kind == "gate":
            self.preview.set_image(None)
            self.preview.setMinimumHeight(0)
            return
        self.preview.setMinimumHeight(120)
        page = self._page(item)
        if page is None:
            self.preview.set_image(None)
            return
        self.preview.set_image(to_pixmap(self._page_image(item, page, 90)))

    def open_viewer(self) -> None:
        from genko.app.viewer import ViewerDialog

        item = self.current()
        if item is None or item.gate == "export":
            return
        if item.gate == "sheet" and item.kind == "gate":
            items = []
            for n, cand in enumerate(self._sheet_candidates(item), 1):
                image = asset_image(self.window.session.path, cand["asset"])
                if image is not None:
                    items.append((f"候補 {n}", image.convert("RGB")))
            ViewerDialog(self, item.title, items, "候補を並べて比べます。選ぶのは承認箱の一覧で行います。").exec()
            return
        episode = self.window.session.episode

        def load(index: int):
            page = next(p for p in episode.pages if p.index == index)
            return [(f"{index} ページ", self._page_image(item, page, 150))]

        page = self._page(item)
        if page is None:
            return
        ViewerDialog(self, item.title, load(page.index), HOW.get(self._kind(item), ""),
                     pages=item.pages, load_page=load, current=page.index).exec()

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
            self.reason.setFocus()
            return
        if self.window.apply_and_commit(ops):
            self.reason.clear()
            self.changed.emit()

    def _export(self, item) -> None:
        from genko.app.dialogs import ExportDialog

        self.window.commit_now()
        ExportDialog(self, self.window.session.episode, self.window.session.path, self.window.session.actor, official=True).exec()
        self.window.session.reload()
        self.changed.emit()


# --- panel view -----------------------------------------------------------------------------------------


class RegionImage(QLabel):
    """The panel image; in region mode a drag draws a rectangle (in image fractions)."""

    drawn = Signal(float, float, float, float)

    def __init__(self) -> None:
        super().__init__()
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(200, 140)
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
            color = QColor("#e03131") if label.startswith("顔") else QColor("#1971c2")
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
        self.candidates.setIconSize(QSize(90, 90))
        self.candidates.setMaximumHeight(130)
        self.candidates.currentRowChanged.connect(lambda _: self._candidate_selected())
        self.provenance = QTextEdit()
        self.provenance.setReadOnly(True)
        self.provenance.setMaximumHeight(70)
        adopt = QPushButton("この候補を採用")
        adopt.clicked.connect(self.adopt)
        big = QPushButton("大きく見る")
        big.clicked.connect(self.open_viewer)
        compare = QPushButton("候補を並べて比べる")
        compare.clicked.connect(self.compare_candidates)
        self.instruction = QLineEdit()
        self.instruction.setPlaceholderText("このコマへの指示（エージェントの作業に出る）")
        send = QPushButton("指示を送る")
        send.clicked.connect(self.send_instruction)
        self.region_kind = QComboBox()
        for key, label in wording.REGION:
            self.region_kind.addItem(label, key)
        self.region_button = QPushButton("領域を描く")
        self.region_button.setCheckable(True)
        self.region_button.toggled.connect(self._region_mode)
        self.regions = QListWidget()
        self.regions.setMaximumHeight(60)
        drop = QPushButton("選んだ領域を消す")
        drop.clicked.connect(self.delete_region)
        top = QHBoxLayout()
        top.addWidget(QLabel("表示"))
        top.addWidget(self.mode, 1)
        top.addWidget(big)
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
        cands_head = QHBoxLayout()
        cands_head.addWidget(QLabel("候補（選ぶと来歴が出る）"), 1)
        cands_head.addWidget(compare)
        layout.addLayout(cands_head)
        layout.addWidget(self.candidates)
        layout.addWidget(self.provenance)
        layout.addWidget(adopt)
        layout.addLayout(row)
        layout.addWidget(QLabel("領域（顔・人物の位置、空けておく場所）"))
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
            self.info.setText("編集画面でコマをクリックすると、そのコマの絵と候補がここに出ます。")
            self.image.clear()
            self.image.boxes = []
            return
        panel = frame.panel or {}
        adopted = panel.get("adopted") or {}
        attempts = panel.get("attempts") or {}
        brief = " / ".join(_brief_words(panel))
        human = panel.get("instruction") or {}
        page = self._page()
        order = next((i + 1 for i, f in enumerate(page.leaf_frames()) if f.id == frame.id), "?") if page else "?"
        self.info.setText(f"<b>{order} コマ目</b>　{wording.PANEL.get(panel.get('status'), panel.get('status'))}　"
                          f"取り込んだ絵 {attempts.get('images', 0)} 枚"
                          f"<br>{brief}" + (f"<br>指示: {human.get('text')}" if isinstance(human, dict) and human.get("text") else ""))
        for n, cand in enumerate(panel.get("candidates", []), 1):
            score = cand.get("review", {}).get("score") if cand.get("review") else None
            entry = QListWidgetItem(f"候補 {n}" + (" ✔採用" if cand["id"] in adopted.values() else "")
                                    + (f"\n評価 {score}" if score is not None else ""))
            entry.setToolTip(cand["id"])
            entry.setData(Qt.ItemDataRole.UserRole, cand["id"])
            image = asset_image(self.window.session.path, cand["asset"])
            if image is not None:
                image.thumbnail((110, 110))
                entry.setIcon(QIcon(to_pixmap(image)))
            self.candidates.addItem(entry)
        for region in panel.get("regions", []):
            who = "人" if region.get("source") == "user" else "エージェント"
            entry = QListWidgetItem(f"{wording.REGION_LABEL.get(region.get('kind'), region.get('kind'))} {region.get('char') or ''}（{who}）")
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
        lines = [f"作った道具: {origin.get('tool_id') or origin.get('kind') or '不明'} {origin.get('model') or ''}",
                 f"id: {cand['id']}　元にした候補: {cand.get('parent') or 'なし'}　{'（指示が変わる前の候補）' if cand.get('stale') else ''}"]
        if origin.get("prompt"):
            lines.append(f"プロンプト: {origin['prompt']}")
        if review:
            lines.append(f"評価（{wording.actor(review.get('by'))}）: {review.get('score')} {review.get('note') or ''}")
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
                out.append((f"{wording.REGION_LABEL.get(region.get('kind'), region.get('kind'))} {region.get('char') or ''}".strip(),
                            ((rect[0] - r.x) / r.width, (rect[1] - r.y) / r.height, rect[2] / r.width, rect[3] / r.height)))
        return out

    # --- actions --------------------------------------------------------------------------

    def open_viewer(self) -> None:
        from genko.app.viewer import ViewerDialog

        frame, page = self._frame(), self._page()
        if frame is None or page is None:
            return
        session = self.window.session
        cand = self._selected_candidate()
        mode = self.mode.currentData()
        image = None
        if mode in ("print", "proof"):
            from genko.render import render_frame

            image = render_frame(page, frame.id, 300, mode=mode, episode=session.episode)
        elif cand is not None and mode == "compare":
            from genko.studio.service import _render_kind

            png, _ = _render_kind(session.episode, session.path, page.index, frame.id, "compare", cand["id"], 1200)
            image = Image.open(io.BytesIO(png))
        elif cand is not None:
            image = asset_image(session.path, cand["asset"])
        if image is not None:
            ViewerDialog(self, "コマ", [(self.mode.currentText(), image.convert("RGB"))]).exec()

    def compare_candidates(self) -> None:
        from genko.app.viewer import ViewerDialog

        frame = self._frame()
        if frame is None:
            return
        adopted = set(((frame.panel or {}).get("adopted") or {}).values())
        items = []
        for n, cand in enumerate((frame.panel or {}).get("candidates", []), 1):
            image = asset_image(self.window.session.path, cand["asset"])
            if image is not None:
                items.append((f"候補 {n}" + (" ✔採用" if cand["id"] in adopted else ""), image.convert("RGB")))
        if items:
            ViewerDialog(self, "候補を比べる", items, "採用は コマ パネルで候補を選んで「この候補を採用」を押します。").exec()

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
                                   "region": {"kind": self.region_kind.currentData(), "rect_mm": rect}}]):
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
            label = f"{char.get('name', char.get('id'))}\n{'設定画 承認済み' if char.get('locked') else '設定画 未承認'}"
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
        lines = [f"{'人物' if data.get('kind') == 'character' else '場所'}: {data.get('name') or data.get('id')}（{data.get('id')}）"]
        look = data.get("look") or {}
        if look:
            lines.append("見た目: " + "、".join(str(v) for k, v in look.items() if isinstance(v, str) and v))
        if data.get("tokens_en"):
            lines.append(f"画像ツール向けの見た目（tokens_en）: {data['tokens_en']}")
        for ref in data.get("refs", []):
            what = {"face": "顔", "sheet": "設定画"}.get(ref.get("kind"), ref.get("kind"))
            lines.append(f"参照（{what}）: 承認 {wording.actor(ref.get('approved_by') or ref.get('by'))}")
        self.detail.setPlainText("\n".join(lines))
