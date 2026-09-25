"""The book as a reader holds it (本の形でプレビュー: spreads, the cover first, a page turning over), and the
find-and-replace over every line (台詞の検索・置換)."""

from __future__ import annotations

import re

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QImage, QLinearGradient, QPainter, QPixmap, QTransform
from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

PREVIEW_DPI = 45


def _pixmap(image) -> QPixmap:
    rgb = image.convert("RGB")
    data = rgb.tobytes("raw", "RGB")
    return QPixmap.fromImage(QImage(data, rgb.width, rgb.height, rgb.width * 3, QImage.Format.Format_RGB888).copy())


def book_pages(episode) -> list:
    """What the reader turns through: (kind, page) — the front, the pages, the back (a jacket gives both)."""
    from genko import covers

    out = []
    for page in covers.pages_in_order(episode):
        cover = covers.cover_of(page)
        if cover and cover["kind"] == "jacket":
            out.append(("front", page))
        else:
            out.append((cover["kind"] if cover else "page", page))
    jacket = next((p for p in episode.pages if (covers.cover_of(p) or {}).get("kind") == "jacket"), None)
    if jacket is not None:
        out.append(("back", jacket))
    return out


def spreads(count: int) -> list[list[int]]:
    """Which pages lie open together: the first alone (the cover, or page 1), then pairs."""
    if count <= 0:
        return []
    out = [[0]]
    k = 1
    while k < count:
        out.append([k, k + 1] if k + 1 < count else [k])
        k += 2
    return out


class BookView(QWidget):
    def __init__(self, dialog) -> None:
        super().__init__()
        self.dialog = dialog
        self.setMinimumSize(640, 440)
        self.turn = 0.0  # 0..1 while a page turns
        self.direction = 1

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        painter.fillRect(self.rect(), QColor(58, 58, 62))
        d = self.dialog
        left, right = d.sides(d.spread)
        page_h = self.height() - 40
        sample = d.pixmap(left if left is not None else right) if (left is not None or right is not None) else None
        aspect = sample.width() / max(1, sample.height()) if sample is not None else 0.7
        page_w = min((self.width() - 40) / 2, page_h * aspect)
        page_h = page_w / aspect
        spine = QPointF(self.width() / 2, (self.height() - page_h) / 2)
        for index, x in ((left, spine.x() - page_w), (right, spine.x())):
            if index is None:
                continue
            painter.drawPixmap(QRectF(x, spine.y(), page_w, page_h), d.pixmap(index), QRectF(d.pixmap(index).rect()))
        # the fold's shadow
        shade = QLinearGradient(spine.x() - 18, 0, spine.x() + 18, 0)
        shade.setColorAt(0, QColor(0, 0, 0, 0))
        shade.setColorAt(0.5, QColor(0, 0, 0, 70))
        shade.setColorAt(1, QColor(0, 0, 0, 0))
        painter.fillRect(QRectF(spine.x() - 18, spine.y(), 36, page_h), shade)
        if self.turn > 0:  # the page turning over: squeezed toward the spine, then opening on the other side
            t = self.turn
            leaving = d.turning_page()
            if leaving is not None:
                pix = d.pixmap(leaving)
                width = abs(1 - 2 * t) * page_w
                # forward, a left-to-right book turns its right page over to the left; a right-bound one the other way
                starts_right = (self.direction > 0) != d.rtl
                on_right = starts_right if t < 0.5 else not starts_right
                x = spine.x() if on_right else spine.x() - width
                painter.save()
                painter.setTransform(QTransform())
                painter.drawPixmap(QRectF(x, spine.y() - 6 * (1 - abs(1 - 2 * t)), width, page_h), pix, QRectF(pix.rect()))
                painter.fillRect(QRectF(x, spine.y(), width, page_h), QColor(0, 0, 0, int(90 * (1 - abs(1 - 2 * t)))))
                painter.restore()
        painter.setPen(QColor(220, 220, 220))
        painter.drawText(QRectF(0, self.height() - 28, self.width(), 24), Qt.AlignmentFlag.AlignCenter, d.caption())
        painter.end()


class BookPreview(QDialog):
    """The book in spreads, read the way it is bound (right-bound books from right to left)."""

    def __init__(self, window) -> None:
        from genko.models import Binding

        super().__init__(window)
        self.window = window
        self.setWindowTitle("本の形でプレビュー")
        self.resize(1000, 700)
        self.pages = book_pages(window.episode)
        self.rtl = window.episode.binding == Binding.RIGHT
        self.groups = spreads(len(self.pages))
        self.spread = 0
        self._cache: dict[int, QPixmap] = {}
        self._turning: int | None = None
        self._target = 0
        self.view = BookView(self)
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, max(0, len(self.groups) - 1))
        self.slider.setInvertedAppearance(self.rtl)
        self.slider.valueChanged.connect(self.go)
        prev_b, next_b = QPushButton("◀"), QPushButton("▶")
        prev_b.setToolTip("左へめくる")
        next_b.setToolTip("右へめくる")
        prev_b.clicked.connect(lambda: self.flip(1 if self.rtl else -1))
        next_b.clicked.connect(lambda: self.flip(-1 if self.rtl else 1))
        row = QHBoxLayout()
        row.addWidget(prev_b)
        row.addWidget(self.slider, 1)
        row.addWidget(next_b)
        layout = QVBoxLayout(self)
        layout.addWidget(self.view, 1)
        layout.addLayout(row)
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._step)

    # --- what is shown ---------------------------------------------------------------------------------

    def pixmap(self, index: int) -> QPixmap:
        if index not in self._cache:
            from genko import covers
            from genko.render import render_page

            kind, page = self.pages[index]
            image = render_page(page, PREVIEW_DPI, "print", self.window.episode, finish=False)
            if (covers.cover_of(page) or {}).get("kind") == "jacket":
                part = "表紙" if kind == "front" else "裏表紙"
                binding = self.window.episode.binding.value
                box = next(((x0, x1) for x0, x1, name in covers.folds(page, binding) if name == part), None)
                if box is not None:
                    from genko.render import mm_to_px

                    t = page.trim_rect_mm()
                    image = image.crop((mm_to_px(box[0], PREVIEW_DPI), mm_to_px(t.y, PREVIEW_DPI), mm_to_px(box[1], PREVIEW_DPI),
                                        mm_to_px(t.y + t.height, PREVIEW_DPI)))
            else:
                from genko.export import crop_to

                image = crop_to(image, page, "trim", PREVIEW_DPI)
            self._cache[index] = _pixmap(image)
        return self._cache[index]

    def sides(self, spread: int) -> tuple[int | None, int | None]:
        """(left, right) page numbers in self.pages of an open spread."""
        if not self.groups:
            return None, None
        group = self.groups[max(0, min(spread, len(self.groups) - 1))]
        if len(group) == 1:
            first = group[0] == 0
            # the cover (or the last page) lies on the side the book opens from
            alone_on_left = (first and self.rtl) or (not first and not self.rtl)
            return (group[0], None) if alone_on_left else (None, group[0])
        a, b = group
        return (b, a) if self.rtl else (a, b)

    def caption(self) -> str:
        if not self.groups:
            return "ページがありません"
        names = []
        for i in self.groups[self.spread]:
            kind, page = self.pages[i]
            names.append({"front": "表紙", "back": "裏表紙"}.get(kind, f"{page.index} ページ"))
        return "・".join(names) + f"　（{self.spread + 1} / {len(self.groups)}）"

    def turning_page(self) -> int | None:
        return self._turning

    # --- turning ---------------------------------------------------------------------------------------

    def go(self, spread: int) -> None:
        self.spread = max(0, min(spread, len(self.groups) - 1))
        if self.slider.value() != self.spread:
            self.slider.blockSignals(True)
            self.slider.setValue(self.spread)
            self.slider.blockSignals(False)
        self.view.update()

    def flip(self, step: int) -> None:
        target = self.spread + step
        if not 0 <= target < len(self.groups):
            return
        group = self.groups[self.spread]
        self._turning = group[-1] if step > 0 else group[0]
        self.view.direction = step
        self.view.turn = 0.01
        self._target = target
        self._timer.start()

    def _step(self) -> None:
        self.view.turn += 0.08
        if self.view.turn >= 0.5 and self.spread != self._target:
            self.go(self._target)
        if self.view.turn >= 1:
            self.view.turn = 0.0
            self._turning = None
            self._timer.stop()
        self.view.update()

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Left:
            self.flip(1 if self.rtl else -1)
        elif event.key() == Qt.Key.Key_Right:
            self.flip(-1 if self.rtl else 1)
        else:
            super().keyPressEvent(event)


class ReplaceDialog(QDialog):
    """台詞の検索・置換: every line holding the words, a click to go there, and one step to replace them all."""

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.setWindowTitle("台詞の検索・置換")
        self.resize(520, 480)
        self.find = QLineEdit()
        self.find.setPlaceholderText("探す言葉")
        self.replace = QLineEdit()
        self.replace.setPlaceholderText("置き換える言葉（空にすると消す）")
        self.regex = QCheckBox("正規表現で探す")
        self.speakers = QCheckBox("話者の名前も")
        self.results = QListWidget()
        self.results.itemDoubleClicked.connect(self._go)
        self.count = QLabel()
        search = QPushButton("探す")
        search.clicked.connect(self.search)
        run = QPushButton("全部置き換える")
        run.clicked.connect(self.replace_all)
        form = QFormLayout()
        form.addRow("探す", self.find)
        form.addRow("置き換え", self.replace)
        options = QHBoxLayout()
        options.addWidget(self.regex)
        options.addWidget(self.speakers)
        buttons = QHBoxLayout()
        buttons.addWidget(search)
        buttons.addWidget(run)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addLayout(options)
        layout.addLayout(buttons)
        layout.addWidget(self.count)
        layout.addWidget(self.results, 1)
        self.find.returnPressed.connect(self.search)

    def _pattern(self):
        text = self.find.text()
        if not text:
            return None
        try:
            return re.compile(text if self.regex.isChecked() else re.escape(text))
        except re.error:
            self.count.setText("正規表現が読めません")
            return None

    def search(self) -> int:
        self.results.clear()
        pattern = self._pattern()
        if pattern is None:
            return 0
        found = 0
        for line in self.window.episode.story:
            hits = len(pattern.findall(line.text or "")) + (len(pattern.findall(line.speaker or "")) if self.speakers.isChecked() else 0)
            if hits:
                found += hits
                item = QListWidgetItem(f"{line.page_index} ページ　{(line.speaker + '：') if line.speaker else ''}{(line.text or '')[:40]}")
                item.setData(Qt.ItemDataRole.UserRole, (line.page_index, line.id))
                self.results.addItem(item)
        self.count.setText(f"{found} か所（{self.results.count()} 行）" if found else "見つかりません")
        return found

    def _go(self, item: QListWidgetItem) -> None:
        page_index, line_id = item.data(Qt.ItemDataRole.UserRole)
        row = next((i for i, p in enumerate(self.window.episode.pages) if p.index == page_index), None)
        if row is not None:
            self.window._select_page(row)
            self.window._on_line_selected(line_id, True)

    def replace_all(self) -> None:
        found = self.search()
        if not found:
            return
        op = {"op": "replace_text", "find": self.find.text(), "replace": self.replace.text(), "regex": self.regex.isChecked(),
              "speakers": self.speakers.isChecked()}
        if self.window.apply_ops([op]):
            self.window.flash(f"{found} か所を置き換えました（元に戻すは 1 回）", 4000)
            self.search()
