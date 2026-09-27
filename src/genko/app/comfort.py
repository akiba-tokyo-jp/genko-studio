"""Comfort over long sessions (UI-B): the page alone with one key (the panels come back at the window's edge),
a layout for each stage of the work, a round menu at the pen, a reminder to rest the eyes (off unless chosen),
fewer moving things on the screen, and the letters' size changed at once.
For manga work (UI-C): a request from the agent brings the approval box forward, and a vertical-scroll book shows
how much of it one phone screen holds.
"""

from __future__ import annotations

import math
import time

from PySide6.QtCore import QEvent, QObject, QPointF, QRectF, QSize, Qt, QTimer
from PySide6.QtGui import QColor, QCursor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QApplication, QDockWidget, QLabel, QToolBar, QWidget

from genko.app import theme

# --- the page alone (Tab) -------------------------------------------------------------------------------------

EDGE_PX = 8  # (the cursor this close to the window's side brings that side's panels back)


class CanvasOnly(QObject):
    """Hide every panel and bar but the menus and the status line; bring a side back while the cursor is at it."""

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.on = False
        self._saved: list[tuple[QWidget, bool]] = []
        self._peek: str | None = None
        self._timer = QTimer(self)
        self._timer.setInterval(120)
        self._timer.timeout.connect(self._watch)

    def _pieces(self) -> list[QWidget]:
        return [w for w in self.window.findChildren(QDockWidget) + self.window.findChildren(QToolBar)
                if w.parent() is self.window]

    def toggle(self) -> bool:
        if self.on:
            self.leave()
        else:
            self.enter()
        return self.on

    def enter(self) -> None:
        self._saved = [(w, w.isVisible()) for w in self._pieces()]
        for widget, _shown in self._saved:
            widget.hide()
        self.on = True
        self._peek = None
        self._timer.start()
        self.window.flash("原稿だけの表示（Tab で戻す）。ウィンドウの左右の端にカーソルを寄せると、その側のパネルが出ます", 3500)

    def leave(self) -> None:
        self._timer.stop()
        for widget, shown in self._saved:
            try:
                widget.setVisible(shown)
            except RuntimeError:
                pass
        self._saved = []
        self.on = False
        self._peek = None

    def _side_of(self, widget) -> str:
        area = self.window.dockWidgetArea(widget) if isinstance(widget, QDockWidget) else self.window.toolBarArea(widget)
        name = getattr(area, "name", str(area)).lower()
        return "left" if "left" in name else "right" if "right" in name else "top"

    def _watch(self) -> None:
        if not self.on or not self.window.isVisible():
            return
        pos = self.window.mapFromGlobal(QCursor.pos())
        width = self.window.width()
        inside = 0 <= pos.y() <= self.window.height()
        if inside and 0 <= pos.x() <= EDGE_PX:
            self._show_side("left")
        elif inside and width - EDGE_PX <= pos.x() <= width:
            self._show_side("right")
        elif self._peek is not None:
            shown = [w for w, was in self._saved if was and self._side_of(w) == self._peek and w.isVisible()]
            if shown:
                edge = max(w.geometry().right() for w in shown) if self._peek == "left" else min(w.geometry().left() for w in shown)
                away = pos.x() > edge + 40 if self._peek == "left" else pos.x() < edge - 40
            else:
                away = True
            if away:
                for widget, _was in self._saved:
                    if self._side_of(widget) == self._peek:
                        widget.hide()
                self._peek = None

    def _show_side(self, side: str) -> None:
        if self._peek == side:
            return
        self._peek = side
        for widget, was in self._saved:
            if was and self._side_of(widget) == side:
                widget.show()


# --- a layout for each stage of the work -----------------------------------------------------------------------

STAGES: dict[str, dict] = {
    "name": {"label": "ネーム", "show": ["ツールの設定", "クイックアクセス", "ページ", "台詞", "点検"], "front": ["台詞", "ページ"]},
    "ink": {"label": "作画", "show": ["ツールの設定", "クイックアクセス", "全体図", "レイヤー", "カラー", "素材", "履歴"],
            "front": ["レイヤー", "素材"]},
    "finish": {"label": "仕上げ", "show": ["ツールの設定", "クイックアクセス", "レイヤー", "素材", "定規・3D", "点検"],
               "front": ["レイヤー", "素材"]},
    "letter": {"label": "写植", "show": ["ツールの設定", "ページ", "台詞", "点検"], "front": ["台詞", "ページ"]},
    "review": {"label": "承認", "show": ["承認箱", "コマの詳細", "資料", "ページ", "点検"], "front": ["承認箱", "ページ"]},
}


def apply_stage(window, key: str) -> None:
    """Show the panels a stage needs (in front, the ones used most), hide the rest. The agent's panels only
    show for books made with agents."""
    stage = STAGES[key]
    agent_docks = set(getattr(window, "agent_docks", []))
    agent_book = getattr(window, "_agent_mode", False)
    docks = [d for d in window.findChildren(QDockWidget) if d.parent() is window]
    for dock in docks:
        wanted = dock.windowTitle() in stage["show"] and (dock not in agent_docks or agent_book)
        dock.setVisible(wanted)
    for title in reversed(stage["front"]):
        dock = next((d for d in docks if d.windowTitle() == title and d.isVisible()), None)
        if dock is not None:
            dock.raise_()
    from genko.app.preferences import settings

    settings().setValue("ui/stage", key)
    window.flash(f"「{stage['label']}」の並びにしました（ウィンドウ → 作業の段階）", 2500)


# --- the round menu at the pen ------------------------------------------------------------------------------

RADIAL_TOOLS = ("pen", "eraser", "fill", "lassofill", "blend", "gradient", "shape", "picker", "effect", "stamp")


class RadialMenu(QWidget):
    """Up to eight commands around the cursor: point and click (or let go of the right button) to choose."""

    RADIUS = 78
    BUTTON = 26

    def __init__(self, window, actions: list, centre) -> None:
        super().__init__(window, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.setMouseTracking(True)
        self.actions_ = [a for a in actions if a is not None][:8]
        size = 2 * (self.RADIUS + self.BUTTON + 6)
        self.resize(size, size)
        self.move(centre.x() - size // 2, centre.y() - size // 2)
        self.hover = -1
        self.opened = time.monotonic()

    def _centre(self) -> QPointF:
        return QPointF(self.width() / 2, self.height() / 2)

    def _spot(self, i: int) -> QPointF:
        angle = -math.pi / 2 + 2 * math.pi * i / max(1, len(self.actions_))
        c = self._centre()
        return QPointF(c.x() + self.RADIUS * math.cos(angle), c.y() + self.RADIUS * math.sin(angle))

    def _at(self, pos) -> int:
        c = self._centre()
        dx, dy = pos.x() - c.x(), pos.y() - c.y()
        if math.hypot(dx, dy) < 22:
            return -1
        angle = (math.atan2(dy, dx) + math.pi / 2) % (2 * math.pi)
        step = 2 * math.pi / max(1, len(self.actions_))
        return int((angle + step / 2) // step) % len(self.actions_)

    def paintEvent(self, _event) -> None:  # noqa: N802
        t = theme.tokens()
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        c = self._centre()
        ring = QPainterPath()
        ring.addEllipse(c, self.RADIUS + self.BUTTON + 2, self.RADIUS + self.BUTTON + 2)
        ring.addEllipse(c, 20, 20)
        back = QColor(t.panel)
        back.setAlpha(238)
        p.setPen(QPen(QColor(t.border), 1))
        p.setBrush(back)
        p.drawPath(ring)
        font = QFont(self.font())
        font.setPointSizeF(max(7.0, font.pointSizeF() * 0.85))
        p.setFont(font)
        for i, action in enumerate(self.actions_):
            spot = self._spot(i)
            chosen = i == self.hover
            p.setPen(QPen(QColor(t.accent if chosen else t.border), 1.5 if chosen else 1))
            p.setBrush(QColor(t.accent_soft if chosen else t.raised))
            p.drawEllipse(spot, self.BUTTON, self.BUTTON)
            icon = action.icon()
            if not icon.isNull():
                icon.paint(p, int(spot.x() - 11), int(spot.y() - 17), 22, 22)
            p.setPen(QColor(t.text))
            label = (action.iconText() or action.text()).replace("…", "").split("（")[0]
            p.drawText(QRectF(spot.x() - 40, spot.y() + 5, 80, 16), Qt.AlignmentFlag.AlignCenter, label[:6])

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        at = self._at(event.position())
        if at != self.hover:
            self.hover = at
            self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        # (a quick right-click opens it and leaves it open; letting go over a command after holding chooses it)
        if event.button() == Qt.MouseButton.RightButton and time.monotonic() - self.opened < 0.3:
            return
        self._choose(self._at(event.position()))

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if not self.rect().contains(event.position().toPoint()):
            self.close()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self._choose(self._at(event.position()))

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Escape:
            self.close()

    def _choose(self, index: int) -> None:
        self.close()
        if 0 <= index < len(self.actions_):
            self.actions_[index].trigger()


def radial_actions(window) -> list:
    """The round menu's commands: the drawing tools and the few moves made most often."""
    names = ("act_pen", "act_eraser", "act_fill", "act_picker", "act_select", "act_undo", "act_redo", "act_fit")
    return [getattr(window, n) for n in names if hasattr(window, n)]


def radial_on() -> bool:
    from genko.app.preferences import settings

    return str(settings().value("ui/radial", "true")).lower() in ("1", "true", "yes")


# --- resting the eyes ---------------------------------------------------------------------------------------


class _Input(QObject):
    """One watcher for the whole application: when a person last pressed, drew or typed."""

    KINDS = (QEvent.Type.MouseButtonPress, QEvent.Type.KeyPress, QEvent.Type.TabletPress, QEvent.Type.Wheel,
             QEvent.Type.TabletMove)

    def __init__(self) -> None:
        super().__init__()
        self.last = time.monotonic()

    def eventFilter(self, _obj, event) -> bool:  # noqa: N802
        if event.type() in self.KINDS:
            self.last = time.monotonic()
        return False


_input: _Input | None = None


def last_input() -> float:
    global _input
    app = QApplication.instance()
    if _input is None and app is not None:
        _input = _Input()
        _input.setParent(app)
        app.installEventFilter(_input)
    return _input.last if _input is not None else time.monotonic()


class RestReminder(QObject):
    """Counts the time actually spent working (input within the last two minutes) and, after the chosen
    minutes, says so quietly. Off (0) unless the person chooses a length in the preferences."""

    IDLE_S = 120
    RESET_IDLE_S = 300

    def __init__(self, window) -> None:
        super().__init__(window)
        self.window = window
        self.worked = 0.0
        last_input()
        self._tick = QTimer(self)
        self._tick.setInterval(15_000)
        self._tick.timeout.connect(self._count)
        self._tick.start()

    def minutes(self) -> int:
        from genko.app.preferences import settings

        try:
            return max(0, int(settings().value("ui/rest_minutes", 0)))
        except (TypeError, ValueError):
            return 0

    def _count(self) -> None:
        if getattr(self.window, "_closed", False):
            self._tick.stop()
            return
        if not self.minutes():
            return
        idle = time.monotonic() - last_input()
        if idle > self.RESET_IDLE_S:
            self.worked = 0.0  # (a real pause already happened)
        elif idle < self.IDLE_S:
            self.worked += self._tick.interval() / 1000
        if self.worked >= self.minutes() * 60:
            self.worked = 0.0
            self.remind()

    def remind(self) -> None:
        toast(self.window, f"{self.minutes()} 分続けて作業しました。20 秒ほど遠くを見て、目を休めましょう", 12_000)


# --- a quiet notice over the canvas --------------------------------------------------------------------------


def toast(window, text: str, ms: int = 5000) -> QLabel:
    """A small note at the bottom of the canvas that goes away by itself (click to close)."""
    t = theme.tokens()
    note = QLabel(text, window.canvas)
    note.setWordWrap(True)
    note.setStyleSheet(f"background:{t.panel}; color:{t.text}; border:1px solid {t.accent}; border-radius:8px; padding:8px 12px;")
    note.setMaximumWidth(460)
    note.adjustSize()
    note.setFixedWidth(min(460, note.fontMetrics().horizontalAdvance(text.split("\n")[0]) + 34))
    note.adjustSize()
    note.move(max(8, (window.canvas.width() - note.width()) // 2), max(8, window.canvas.height() - note.height() - 24))
    note.mousePressEvent = lambda _e: note.deleteLater()
    note.show()
    QTimer.singleShot(ms, note.deleteLater)
    return note


# --- fewer moving things; the letters' size at once -------------------------------------------------------------


def reduce_motion() -> bool:
    from genko.app.preferences import settings

    return str(settings().value("ui/reduce_motion", "false")).lower() in ("1", "true", "yes")


def apply_motion(app: QApplication | None = None) -> None:
    """Menus, lists and tooltips open without sliding or fading when the person asks for fewer moving things."""
    app = app or QApplication.instance()
    if app is None:
        return
    still = reduce_motion()
    for effect in (Qt.UIEffect.UI_AnimateMenu, Qt.UIEffect.UI_FadeMenu, Qt.UIEffect.UI_AnimateCombo,
                   Qt.UIEffect.UI_AnimateTooltip, Qt.UIEffect.UI_FadeTooltip, Qt.UIEffect.UI_AnimateToolBox):
        app.setEffectEnabled(effect, not still)


def apply_font(app: QApplication | None = None) -> None:
    """The screen's letters at the chosen size, now (every panel follows)."""
    from genko.app import preferences

    app = app or QApplication.instance()
    size = preferences.ui_font_pt()
    if app is None or not size:
        return
    font = app.font()
    if font.pointSize() == size:
        return
    font.setPointSize(size)
    app.setFont(font)  # (every widget without a font of its own follows)
    app._genko_tokens = None  # (the style sheet is set again, so the sizes are measured again)
    theme.apply(app)


def icon_size() -> QSize:
    return QSize(24, 24)


# --- a request from the agent (UI-C) ----------------------------------------------------------------------------


def raise_requests() -> bool:
    from genko.app.preferences import settings

    return str(settings().value("ui/raise_requests", "true")).lower() in ("1", "true", "yes")


def request_ids(window) -> set[str]:
    from genko.app import review_model

    return {item.ticket_id for item in review_model.inbox(window.episode)}


def notice_requests(window, before: set[str]) -> list:
    """New requests since `before`: a quiet note, the taskbar's mark, and (unless turned off) the approval box in
    front with the newest one chosen. Nothing moves under a pen that is still down; it waits for the pen to lift."""
    from genko.app import review_model

    new = [item for item in review_model.inbox(window.episode) if item.ticket_id not in before]
    if not new:
        return []

    def show(tries: int = 0) -> None:
        if QApplication.mouseButtons() != Qt.MouseButton.NoButton and tries < 40:
            QTimer.singleShot(250, lambda: show(tries + 1))
            return
        more = f" ほか {len(new) - 1} 件" if len(new) > 1 else ""
        toast(window, f"承認の依頼が届きました: {new[-1].title}{more}", 6000)
        QApplication.alert(window)
        if not raise_requests():
            return
        if getattr(window, "canvas_only", None) is not None and window.canvas_only.on:
            window.canvas_only.leave()
        window.show_dock("承認箱")
        dock = next((d for d in window.findChildren(QDockWidget) if d.windowTitle() == "承認箱"), None)
        if dock is not None and not dock.isFloating() and dock.height() < 360:  # (room enough to see the request)
            area = window.dockWidgetArea(dock)
            column = [d for d in window.findChildren(QDockWidget) if d is not dock and not d.isFloating()
                      and window.dockWidgetArea(d) == area and window._dock_visible(d)
                      and abs(d.x() - dock.x()) < 20]  # (the panels above and below it, not the ones in its tabs)
            window.resizeDocks([dock, *column], [int(window.height() * 0.62)] + [80] * len(column), Qt.Orientation.Vertical)
        box = window.approvals
        box.refresh()
        ids = [i.ticket_id for i in box.items]
        if new[-1].ticket_id in ids:
            box.list.setCurrentRow(ids.index(new[-1].ticket_id))

    show()
    return new


# --- a phone screen over a vertical-scroll book (UI-C) ------------------------------------------------------------


def phone_default(episode) -> bool:
    """On by choice; with no choice made, on for tall strips (a webtoon's pages)."""
    from genko.app.preferences import settings

    chosen = settings().value("ui/phone_view", None)
    if chosen is not None:
        return str(chosen).lower() in ("1", "true", "yes")
    if not episode.pages:
        return False
    w, h = episode.pages[0].spec.trim_size()
    return h >= 2.5 * w
