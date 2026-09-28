"""絵柄を選ぶ: walk the manga style catalog (genre → sub-genre → finished style) and choose the book's style.

The left side lists the branches of the level you are in (with their sample thumbnails); a double click goes down.
The card on the right shows the chosen branch: its sample, where it is in the tree, and its words. Any level may be
chosen (the levels under it are left to the artist). The catalog is read in the background; nothing here writes to
the book — the window calls `MainWindow.use_style` with the chosen id.
"""

from __future__ import annotations

from PySide6.QtCore import QObject, QRunnable, QSize, Qt, QThreadPool, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QIcon, QPixmap
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QLineEdit,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from genko import stylecat
from genko.app import dialog_look as look
from genko.app import theme

THUMB = QSize(72, 96)
PICTURE = QSize(240, 320)


def _icon(picture: QPixmap) -> QIcon:
    """A thumbnail that keeps its own colours when its row is chosen (not tinted with the selection)."""
    icon = QIcon(picture)
    icon.addPixmap(picture, QIcon.Mode.Selected)
    return icon


class _Done(QObject):
    done = Signal(object, object)  # (result, error)


class _Job(QRunnable):
    def __init__(self, fn, signals: _Done) -> None:
        super().__init__()
        self.fn, self.signals = fn, signals

    def run(self) -> None:
        try:
            result, problem = self.fn(), None
        except Exception as exc:  # noqa: BLE001 (shown to the person)
            result, problem = None, exc
        self.signals.done.emit(result, problem)


class StylePicker(QDialog):
    """Choose a branch of the style catalog. `chosen` is its id after Accepted; `back_to_genko` when the person
    asked for Genko's own words instead."""

    inline = False  # (tests: read the catalog at once, not in the background)

    def __init__(self, parent: QWidget | None, kept: dict | None = None, expression: str = "mono") -> None:
        super().__init__(parent)
        self.setWindowTitle("絵柄を選ぶ")
        self.kept = kept
        self.expression = expression  # (the book's: a black-and-white style on a colour book is marked)
        self.chosen: str | None = None
        self.back_to_genko = False
        self.nodes: dict[str, dict] = {}
        self.here: str | None = None  # (the branch whose children are listed; None: the genres)
        self.details: dict[str, dict] = {}
        self.pictures: dict[str, QPixmap] = {}
        self._jobs: list[_Done] = []

        self.where = QLabel()
        theme.role(self.where, "hint")
        self.up = QPushButton("上の段へ")
        self.up.clicked.connect(self._go_up)
        top = QHBoxLayout()
        top.addWidget(self.up)
        top.addWidget(self.where, 1)
        self.find = QLineEdit()
        self.find.setPlaceholderText("この段から探す（名前・説明）。サイトのページの URL を貼ると、その絵柄へ")
        self.find.setClearButtonEnabled(True)
        self.find.textChanged.connect(self._filter)
        self.list = QListWidget()
        self.list.setIconSize(THUMB)
        self.list.setSpacing(2)
        self.list.currentItemChanged.connect(lambda item, _old: self._show(item.data(Qt.ItemDataRole.UserRole) if item else None))
        self.list.itemActivated.connect(lambda item: self._go_down(item.data(Qt.ItemDataRole.UserRole)))
        self.note = QLabel("ダブルクリックで下の段へ。どの段でも「この絵柄にする」で決められます（下の段は描く人におまかせ）。")
        self.note.setWordWrap(True)
        theme.role(self.note, "hint")
        body = QVBoxLayout()
        body.addLayout(top)
        body.addWidget(self.find)
        body.addWidget(self.list, 1)
        body.addWidget(self.note)

        self.picture = QLabel()
        self.picture.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.picture.setFixedHeight(PICTURE.height())
        self.title = QLabel()
        theme.role(self.title, "heading")
        self.title.setWordWrap(True)
        self.summary = QLabel()
        self.summary.setWordWrap(True)
        self.fit = QLabel()
        self.fit.setWordWrap(True)
        self.words = QLabel()
        self.words.setWordWrap(True)
        self.words.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        theme.role(self.words, "hint")
        self.show_words = QPushButton("依頼の言葉を見る")
        self.show_words.setCheckable(True)
        theme.role_prop(self.show_words, "quiet", True)
        self.show_words.setToolTip("この絵柄が絵の依頼に書く言葉（長い）を見ます")
        self.site = QPushButton("サイトで見る")
        theme.role_prop(self.site, "quiet", True)
        self.site.clicked.connect(lambda: self.chosen_id() and QDesktopServices.openUrl(QUrl(stylecat.page_url(self.chosen_id()))))
        scroll = QScrollArea()  # (the words are long: they scroll, the picture stays)
        scroll.setWidget(self.words)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        side = QVBoxLayout()
        side.addWidget(self.picture)
        side.addWidget(self.title)
        side.addWidget(self.summary)
        side.addWidget(self.fit)
        side.addWidget(scroll, 1)
        scroll.hide()  # (the words for the image tool are long: shown when asked)
        self.show_words.toggled.connect(scroll.setVisible)
        links_row = QHBoxLayout()
        links_row.addWidget(self.show_words)
        links_row.addWidget(self.site)
        links_row.addStretch(1)
        side.addLayout(links_row)
        side.addStretch(1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        self.ok = buttons.button(QDialogButtonBox.StandardButton.Ok)
        self.ok.setText("この絵柄にする")
        self.ok.setEnabled(False)
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("やめる")
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        extra = QWidget()
        row = QHBoxLayout(extra)
        row.setContentsMargins(0, 0, 0, 0)
        self.plain = QPushButton("Genko の言葉に戻す")
        theme.role_prop(self.plain, "quiet", True)
        self.plain.setVisible(kept is not None)
        self.plain.clicked.connect(self._plain)
        self.links = QPushButton("サイトのボタンで開けるようにする")
        theme.role_prop(self.links, "quiet", True)
        self.links.setToolTip("サイトの「この絵柄を使う」を押すと、この Genko に届くようにします（このパソコンに 1 回だけ）")
        self.links.clicked.connect(self._register)
        row.addWidget(self.plain)
        row.addWidget(self.links)
        row.addStretch(1)

        now = f"今の絵柄: {'・'.join(kept.get('path') or [kept.get('title', '')])}（版 {kept.get('version')}）" if kept else \
            "今の絵柄: Genko の言葉（カタログから選んでいない）"
        look.frame(self, look.header("絵柄を選ぶ", "マンガの絵柄カタログから選びます。選んだ絵柄の言葉と見本は、この原稿に写して"
                                     "残します（サイトが変わっても、この原稿の絵柄は変わりません）。" + now),
                   body, look.card(side, "選んでいる絵柄"), look.footer(buttons, extra))
        self.resize(820, 600)
        self._message("絵柄カタログを読んでいます…")
        self._run(stylecat.tree, self._got_tree)

    # --- reading in the background --------------------------------------------------------------------

    def _run(self, fn, then) -> None:
        if self.inline:
            try:
                then(fn(), None)
            except Exception as exc:  # noqa: BLE001
                then(None, exc)
            return
        signals = _Done()
        signals.done.connect(then)
        self._jobs.append(signals)  # (kept alive until the dialog goes)
        QThreadPool.globalInstance().start(_Job(fn, signals))

    def _message(self, words: str, error: bool = False) -> None:
        self.where.setText(words)
        theme.role(self.where, "error" if error else "hint")

    def _got_tree(self, nodes, problem) -> None:
        if problem is not None:
            self._message(str(problem), error=True)
            return
        self.nodes = {n["id"]: n for n in nodes}
        start = None
        if self.kept and self.kept.get("id") in self.nodes:  # (open where the book's style is)
            start = self.nodes[self.kept["id"]].get("parent")
        self._list(start, select=self.kept.get("id") if self.kept else None)

    # --- the list -------------------------------------------------------------------------------------

    def _children(self, parent: str | None) -> list[dict]:
        return [n for n in self.nodes.values() if n.get("parent") == parent]

    def _list(self, parent: str | None, select: str | None = None) -> None:
        self.here = parent
        self.list.clear()
        trail = []
        node = self.nodes.get(parent) if parent else None
        while node is not None:
            trail.insert(0, node.get("title", node["id"]))
            node = self.nodes.get(node.get("parent")) if node.get("parent") else None
        levels = {None: "1段目（ジャンル）", 1: "2段目", 2: "3段目（仕上げの絵柄）"}
        depth = self.nodes[parent]["level"] if parent else None
        self._message(" › ".join(["絵柄カタログ", *trail]) + f" — {levels.get(depth, '')}")
        self.up.setEnabled(parent is not None)
        for child in self._children(parent):
            count = len(self._children(child["id"]))
            item = QListWidgetItem(f"{child.get('title')}\n{child.get('summary') or ''}" + (f"（下に {count} 種）" if count else ""))
            item.setData(Qt.ItemDataRole.UserRole, child["id"])
            item.setSizeHint(QSize(0, THUMB.height() + 8))
            self.list.addItem(item)
            self._thumb(child, item)
            if child["id"] == select:
                self.list.setCurrentItem(item)
        self._filter(self.find.text())

    def _filter(self, words: str) -> None:
        """Only the rows whose name or summary has all the words (the third level has twenty-odd styles).
        A pasted page URL or genko:// link goes to that style."""
        if "://" in str(words or ""):
            from genko import stylecat

            try:
                found = stylecat.style_from_link(words)
            except stylecat.CatalogError:
                found = None
            if found and found in self.nodes:
                self.find.blockSignals(True)
                self.find.clear()
                self.find.blockSignals(False)
                self._list(self.nodes[found].get("parent"), select=found)
                return
        wanted = [w for w in str(words or "").split() if w]
        for row in range(self.list.count()):
            item = self.list.item(row)
            item.setHidden(bool(wanted) and not all(w in item.text() for w in wanted))

    def _thumb(self, node: dict, item: QListWidgetItem) -> None:
        url = node.get("thumbnail_url")
        if not url:
            return
        key = "t:" + node["id"]
        if key in self.pictures:
            item.setIcon(_icon(self.pictures[key]))
            return

        def got(data, problem, item=item, key=key) -> None:
            if problem is None and data:
                picture = QPixmap()
                if picture.loadFromData(data):
                    self.pictures[key] = picture
                    try:
                        item.setIcon(_icon(picture))
                    except RuntimeError:  # (the list moved on)
                        pass

        self._run(lambda: stylecat._get(url), got)

    def _go_down(self, node_id: str | None) -> None:
        if node_id and self._children(node_id):
            self._list(node_id)

    def _go_up(self) -> None:
        if self.here is None:
            return
        parent = self.nodes[self.here].get("parent")
        self._list(parent, select=self.here)

    # --- the card -------------------------------------------------------------------------------------

    def chosen_id(self) -> str | None:
        item = self.list.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else None

    def _show(self, node_id: str | None) -> None:
        self.ok.setEnabled(node_id is not None)
        if node_id is None:
            return
        node = self.nodes.get(node_id, {})
        self.title.setText(node.get("title", node_id))
        self.summary.setText(node.get("summary") or "")
        self.fit.setText("")
        self.words.setText("")
        self.picture.setPixmap(QPixmap())
        if node_id in self.details:
            self._detail(node_id, self.details[node_id])
            return
        self.words.setText("読んでいます…")

        def got(data, problem, node_id=node_id) -> None:
            if problem is not None:
                if self.chosen_id() == node_id:
                    self.words.setText(str(problem))
                return
            self.details[node_id] = data
            if self.chosen_id() == node_id:
                self._detail(node_id, data)

        self._run(lambda: stylecat.style(node_id), got)

    def _detail(self, node_id: str, data: dict) -> None:
        words = (data.get("prompt") or {}).get("ja") or ""
        colour = "白黒" if data.get("expression") == "mono" else "カラー"
        clash = data.get("expression", "mono") == "mono" and self.expression == "color"
        self.fit.setText(f"{colour}・版 {data.get('version')}" + ("　この原稿はカラーなので、絵の依頼には使われません" if clash else ""))
        theme.role(self.fit, "error" if clash else "hint")
        self.words.setText(words)
        key = "s:" + node_id
        if key in self.pictures:
            self._picture(self.pictures[key])
            return
        samples = data.get("samples") or []
        index = data.get("reference") if isinstance(data.get("reference"), int) else 0
        url = samples[index].get("url") if 0 <= index < len(samples) else None
        if not url:
            return

        def got(raw, problem, key=key, node_id=node_id) -> None:
            if problem is None and raw:
                picture = QPixmap()
                if picture.loadFromData(raw):
                    self.pictures[key] = picture
                    if self.chosen_id() == node_id:
                        self._picture(picture)

        self._run(lambda: stylecat._get(url), got)

    def _picture(self, picture: QPixmap) -> None:
        self.picture.setPixmap(picture.scaled(PICTURE, Qt.AspectRatioMode.KeepAspectRatio,
                                              Qt.TransformationMode.SmoothTransformation))

    # --- choosing -------------------------------------------------------------------------------------

    def _accept(self) -> None:
        self.chosen = self.chosen_id()
        if self.chosen:
            self.accept()

    def _plain(self) -> None:
        self.back_to_genko = True
        self.accept()

    def _register(self) -> None:
        from genko.app import links

        done, words = links.register()
        self._message(words, error=not done)
