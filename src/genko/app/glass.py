"""Frosted glass for what floats over the page (menus, tooltips, the round menu, the command search), made by the
system itself: Acrylic on Windows 11, the vibrancy of menus on a Mac. Nothing is imitated: elsewhere, on an older
Windows, or when the person has turned transparency off in the system, the floating parts keep their plain face.

The parts ask for the glass by class; one application event filter (installed only where the glass exists) makes
them see-through before their window is made, and asks the system for the glass when they are first shown.
"""

from __future__ import annotations

import sys
from functools import lru_cache

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtWidgets import QApplication, QMenu, QWidget


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


class _Glass(QObject):
    def eventFilter(self, obj, event) -> bool:  # noqa: N802
        kind = event.type()
        if kind == QEvent.Type.Polish and isinstance(obj, QWidget) and obj.isWindow() and wants(obj):
            obj.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)  # (before its window is made)
        elif kind == QEvent.Type.Show and isinstance(obj, QWidget) and obj.isWindow() and wants(obj):
            apply(obj)
        return False


_FILTER: list = []


def install(app: QApplication | None = None) -> bool:
    """Once, and only where the system has the glass (elsewhere nothing watches the events)."""
    app = app or QApplication.instance()
    if app is None or _FILTER or not available():
        return bool(_FILTER)
    _FILTER.append(_Glass(app))
    app.installEventFilter(_FILTER[0])
    return True
