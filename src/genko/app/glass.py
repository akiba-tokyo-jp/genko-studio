"""Frosted glass for what floats over the page (menus, tooltips, the round menu, the command search), made by the
system itself: Acrylic on Windows 11, the vibrancy of menus on a Mac. Nothing is imitated: elsewhere, on an older
Windows, or when the person has turned transparency off in the system, the floating parts keep their plain face.

The parts ask for the glass by class; the app's style (a Fusion that also does this, only where the glass exists)
makes them see-through as they are readied, before their window is made, and a watcher on each such window asks
the system for the glass when it is first shown. (Not an application event filter: that would run for every event
of every widget and slow drawing.)
"""

from __future__ import annotations

import sys
from functools import lru_cache

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtWidgets import QApplication, QMenu, QProxyStyle, QWidget


@lru_cache(maxsize=1)
def available() -> str:
    """"windows", "mac" or "" (no system glass here, or turned off by the person)."""
    if sys.platform == "win32":
        try:
            if sys.getwindowsversion().build < 22621:  # (Windows 11 22H2: the system backdrop for any window)
                return ""
            import winreg

            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
            if int(winreg.QueryValueEx(key, "EnableTransparency")[0]) == 0:
                return ""
        except OSError:
            pass
        return "windows"
    if sys.platform == "darwin":
        try:
            from AppKit import NSWorkspace  # (pyobjc)

            if NSWorkspace.sharedWorkspace().accessibilityDisplayShouldReduceTransparency():
                return ""
        except Exception:  # no pyobjc: no glass
            return ""
        return "mac"
    return ""


def wants(widget: QWidget) -> bool:
    """The floating parts that get the glass."""
    if isinstance(widget, QMenu):
        return True
    if widget.windowType() == Qt.WindowType.ToolTip:
        return True
    return bool(widget.property("glass_wanted"))


def _windows(widget: QWidget, dark: bool) -> bool:
    import ctypes
    from ctypes import byref, c_int, sizeof

    class Margins(ctypes.Structure):
        _fields_ = [("left", c_int), ("right", c_int), ("top", c_int), ("bottom", c_int)]

    hwnd = int(widget.winId())
    dwm = ctypes.windll.dwmapi
    value = c_int(1 if dark else 0)
    dwm.DwmSetWindowAttribute(hwnd, 20, byref(value), sizeof(value))  # DWMWA_USE_IMMERSIVE_DARK_MODE
    corner = c_int(2)
    dwm.DwmSetWindowAttribute(hwnd, 33, byref(corner), sizeof(corner))  # DWMWA_WINDOW_CORNER_PREFERENCE: round
    dwm.DwmExtendFrameIntoClientArea(hwnd, byref(Margins(-1, -1, -1, -1)))
    backdrop = c_int(3)  # DWMSBT_TRANSIENTWINDOW: Acrylic
    return dwm.DwmSetWindowAttribute(hwnd, 38, byref(backdrop), sizeof(backdrop)) == 0  # DWMWA_SYSTEMBACKDROP_TYPE


def _mac(widget: QWidget, dark: bool) -> bool:
    import objc
    from AppKit import NSAppearance, NSVisualEffectView, NSWindowBelow

    view = objc.objc_object(c_void_p=int(widget.winId()))
    effect = NSVisualEffectView.alloc().initWithFrame_(view.bounds())
    effect.setAutoresizingMask_(2 | 16)  # width and height follow the window
    effect.setBlendingMode_(0)  # behind the window: what is under the menu shows through
    effect.setMaterial_(5)  # NSVisualEffectMaterialMenu
    effect.setState_(1)  # always active
    effect.setAppearance_(NSAppearance.appearanceNamed_("NSAppearanceNameDarkAqua" if dark else "NSAppearanceNameAqua"))
    view.addSubview_positioned_relativeTo_(effect, NSWindowBelow, None)
    return True


def apply(widget: QWidget) -> bool:
    """Ask the system for the glass behind this (already see-through) window; its face then lets it through."""
    from genko.app import theme

    kind = available()
    if not kind or widget.property("glass") is not None:
        return bool(widget.property("glass"))
    try:
        done = (_windows if kind == "windows" else _mac)(widget, theme.tokens().dark)
    except Exception:  # (the system said no: the plain face stays)
        done = False
    theme.role_prop(widget, "glass", bool(done))
    return done


class _Shown(QObject):
    """On one see-through window: the glass asked for when it is first shown."""

    def eventFilter(self, obj, event) -> bool:  # noqa: N802
        if event.type() == QEvent.Type.Show:
            apply(obj)
            obj.removeEventFilter(self)
        return False


class GlassStyle(QProxyStyle):
    """Fusion, with the floating parts made see-through as each is readied (polished)."""

    def __init__(self) -> None:
        super().__init__("Fusion")
        self.setObjectName("fusion")  # (the theme keeps it: it is the Fusion style)
        self._shown = _Shown(self)

    def polish(self, arg):  # (three overloads: a widget, a palette, the app)
        result = super().polish(arg)
        if isinstance(arg, QWidget) and arg.isWindow() and arg.property("glass") is None and wants(arg):
            arg.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)  # (before its window is made)
            arg.installEventFilter(self._shown)
        return result


_MADE: list = []


def style() -> QProxyStyle | str:
    """The app's style: Fusion, which also readies the glass where the system has it."""
    if not available():
        return "Fusion"
    _MADE[:] = [GlassStyle()]
    return _MADE[0]


def install(app: QApplication | None = None) -> bool:
    """Once, and only where the system has the glass (elsewhere the style is plain Fusion): the app's style readies
    the floating parts."""
    app = app or QApplication.instance()
    if app is None or not available():
        return False
    if not _MADE:  # (the theme sets it; an app styled some other way gets it here)
        app.setStyle(style())
    return True
