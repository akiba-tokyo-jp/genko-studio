"""Scanning a page straight in (スキャナーから取り込む), through what the computer already has: WIA on Windows
(its own scan window), SANE's scanimage on Linux. Macs have no scanning command to call; there the scan is
saved from the Image Capture app and read as a file."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class ScanError(RuntimeError):
    pass


def method() -> str | None:
    """"wia" (Windows), "sane" (Linux with scanimage), or None (nothing to scan with here)."""
    if sys.platform.startswith("win"):
        return "wia" if shutil.which("powershell") or shutil.which("powershell.exe") else None
    if sys.platform.startswith("linux") and shutil.which("scanimage"):
        return "sane"
    return None


def command(how: str, out: Path, dpi: int = 600, mode: str = "gray") -> list[str]:
    """The command that scans one page into `out` (PNG or BMP)."""
    if how == "sane":
        colour = {"gray": "Gray", "color": "Color", "lineart": "Lineart"}.get(mode, "Gray")
        return ["scanimage", "--format=png", f"--resolution={int(dpi)}", f"--mode={colour}", f"--output-file={out}"]
    if how == "wia":
        # the scanner's own window (WIA): the person picks the scanner, the area and the colour there
        script = ("$d = New-Object -ComObject WIA.CommonDialog; $i = $d.ShowAcquireImage(); "
                  f"if ($i -eq $null) {{ exit 2 }}; $i.SaveFile('{out}')")
        return ["powershell", "-NoProfile", "-NonInteractive", "-Command", script]
    raise ScanError("このパソコンでは、スキャナーから直接取り込めません。スキャンした画像をファイルに保存して読み込んでください")


def scan(dpi: int = 600, mode: str = "gray", run=subprocess.run, timeout: float = 300) -> bytes:
    """One page from the scanner, as the file's bytes (None found: ScanError). `run` is for tests."""
    how = method()
    if how is None:
        raise ScanError("このパソコンでは、スキャナーから直接取り込めません。スキャンした画像をファイルに保存して読み込んでください")
    with tempfile.TemporaryDirectory() as folder:
        out = Path(folder) / ("scan.bmp" if how == "wia" else "scan.png")
        try:
            done = run(command(how, out, dpi, mode), capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise ScanError(f"スキャナーが応えませんでした（{exc}）") from exc
        if getattr(done, "returncode", 1) == 2 and how == "wia":
            raise ScanError("スキャンをやめました")
        if getattr(done, "returncode", 1) != 0 or not out.exists():
            detail = (getattr(done, "stderr", "") or "").strip().splitlines()[-1:] or ["スキャナーが見つからないか、使えません"]
            raise ScanError(f"スキャンできませんでした: {detail[0]}")
        return out.read_bytes()
