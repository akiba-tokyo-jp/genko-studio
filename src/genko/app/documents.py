"""Several books open at once, and one book in several windows (J11).

A window holds its books as tabs (each tab one Session: the book, its page, how it was shown). Another window on
the same book shares that Session, so a change made in one shows at once in the other, while each keeps its own
page, zoom, turn and panels (one zoomed in to draw, the other showing the whole page).
"""

from __future__ import annotations

from dataclasses import dataclass, field

_WINDOWS: list = []  # (held here: a window opened from another has no other owner)


@dataclass
class Document:
    """One open book in a window: its session and how the window last showed it."""

    session: object
    page_index: int = 0
    view: dict = field(default_factory=dict)  # zoom, scroll, turn, mirror (PageCanvas.view_state)
    target_layer_id: str | None = None

    @property
    def title(self) -> str:
        episode = self.session.episode
        return f"{episode.title or '無題'} 第{episode.episode}話"


def register(window) -> None:
    if window not in _WINDOWS:
        _WINDOWS.append(window)


def unregister(window) -> None:
    if window in _WINDOWS:
        _WINDOWS.remove(window)


def windows() -> list:
    return [w for w in list(_WINDOWS) if not getattr(w, "_closed", False)]


def sharing(session, but=None) -> list:
    """The other windows showing this book now."""
    return [w for w in windows() if w is not but and getattr(w, "session", None) is session]


def open_elsewhere(session, but=None) -> bool:
    """Whether another window (in any of its tabs) still has this book open: then closing it here must not
    close the book."""
    return any(any(doc.session is session for doc in getattr(w, "documents", [])) for w in windows() if w is not but)


def find(path) -> tuple[object, object] | tuple[None, None]:
    """(window, document) where this book is already open."""
    from pathlib import Path

    if path is None:
        return None, None
    wanted = Path(path).resolve()
    for window in windows():
        for doc in getattr(window, "documents", []):
            if doc.session.path is not None and Path(doc.session.path).resolve() == wanted:
                return window, doc
    return None, None


def notify(session, source) -> None:
    """A change made in `source`: every other window on the same book shows it."""
    for window in sharing(session, but=source):
        window.on_shared_change()
