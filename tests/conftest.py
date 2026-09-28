"""Every test run keeps the app's settings (QSettings) in a temporary folder, so one test's brush,
gutter or fill settings never leak into another, nor into the person's own settings."""

import tempfile

import pytest


@pytest.fixture(autouse=True, scope="session")
def _isolated_qt_settings():
    try:
        from PySide6.QtCore import QSettings
    except ImportError:
        yield
        return
    with tempfile.TemporaryDirectory() as folder:
        for fmt in (QSettings.Format.NativeFormat, QSettings.Format.IniFormat):
            QSettings.setPath(fmt, QSettings.Scope.UserScope, folder)
        yield
