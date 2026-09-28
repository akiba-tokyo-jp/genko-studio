"""Every test run keeps the app's settings (QSettings) in a temporary folder, so one test's brush,
gutter or fill settings never leak into another, nor into the person's own settings."""

import sys
import tempfile

import pytest

if sys.platform == "win32":
    # On Windows QSettings("Genko", "Genko Studio") is the registry, which setPath cannot move: tests would write
    # the person's own settings (and read the last run's). Before the app's modules import QSettings, make that
    # form an INI file, which setPath below sends to the temporary folder.
    try:
        from PySide6 import QtCore
    except ImportError:
        pass
    else:

        class _FileSettings(QtCore.QSettings):
            def __init__(self, *args, **kwargs):
                if len(args) == 2 and all(isinstance(a, str) for a in args) and not kwargs:
                    super().__init__(QtCore.QSettings.Format.IniFormat, QtCore.QSettings.Scope.UserScope, *args)
                else:
                    super().__init__(*args, **kwargs)

        QtCore.QSettings = _FileSettings


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
