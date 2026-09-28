"""Taking a drawing in from paper: the lines of a scan drawn out with settings, and the scanner itself."""

from __future__ import annotations

from pathlib import Path

import pytest
from PIL import Image, ImageDraw

from genko import lineart, scanner


def _paper() -> Image.Image:
    im = Image.new("RGB", (300, 300), (250, 248, 240))  # (paper is never quite white)
    d = ImageDraw.Draw(im)
    d.line([(20, 280), (280, 20)], fill=(120, 170, 240), width=3)  # the blue pencil first
    d.line([(20, 20), (280, 280)], fill=(30, 30, 30), width=3)  # then the ink over it
    d.rectangle([200, 40, 260, 90], fill=(10, 10, 10))  # a solid black
    d.point((150, 50), fill=(0, 0, 0))  # a speck of dust
    return im


def test_the_lines_of_a_scan_with_the_blue_pencil_and_the_dust_taken_away():
    im = _paper()
    plain = lineart.extract(im, lineart.LineParams(min_px=0))
    clean = lineart.extract(im, lineart.LineParams.from_dict({"drop_blue": 1, "min_px": 6}))
    assert plain.getpixel((50, 250))[3] == 255 and clean.getpixel((50, 250))[3] == 0  # (the blue line gone)
    assert clean.getpixel((150, 150))[3] == 255  # (the ink line kept)
    assert plain.getpixel((150, 50))[3] == 255 and clean.getpixel((150, 50))[3] == 0  # (the speck gone)
    assert clean.getpixel((230, 65))[3] == 255
    lines_only = lineart.extract(im, lineart.LineParams.from_dict({"keep_solid": 0}))
    assert lines_only.getpixel((230, 65))[3] == 0  # (the middle of the black is not a line)
    red = lineart.extract(im, lineart.LineParams.from_dict({"rgb": [200, 0, 0]}))
    assert red.getpixel((150, 150))[:3] == (200, 0, 0)
    faint = lineart.extract(im, lineart.LineParams.from_dict({"threshold": 0.3}))
    assert faint.getpixel((50, 250))[3] == 0  # (a low strength leaves the light lines out)


def test_the_scanner_through_the_computers_own_command(monkeypatch):
    monkeypatch.setattr(scanner, "method", lambda: "sane")
    seen = {}

    def run(cmd, **kwargs):
        seen["cmd"] = cmd
        out = Path(next(c for c in cmd if c.startswith("--output-file=")).split("=", 1)[1])
        Image.new("L", (40, 60), 255).save(out, format="PNG")

        class Done:
            returncode, stderr = 0, ""
        return Done()

    blob = scanner.scan(300, "gray", run=run)
    assert blob.startswith(b"\x89PNG") and "--resolution=300" in seen["cmd"] and "--mode=Gray" in seen["cmd"]
    wia = scanner.command("wia", Path("C:/t/scan.bmp"))
    assert wia[0] == "powershell" and "WIA.CommonDialog" in wia[-1]

    def fails(cmd, **kwargs):
        class Done:
            returncode, stderr = 1, "scanimage: no SANE devices found"
        return Done()

    with pytest.raises(scanner.ScanError, match="no SANE devices"):
        scanner.scan(run=fails)
    monkeypatch.setattr(scanner, "method", lambda: None)
    with pytest.raises(scanner.ScanError):
        scanner.scan()


def test_a_scan_file_comes_in_as_a_layer_of_lines(tmp_path, monkeypatch):
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    from genko.app import main as app_main

    file = tmp_path / "scan.png"
    _paper().save(file)
    window = app_main.MainWindow()
    try:
        monkeypatch.setattr(app_main.QFileDialog, "getOpenFileName", lambda *a, **k: (str(file), ""))
        monkeypatch.setattr(app_main, "filter_params", lambda parent, kind, now=None, preview=None, histogram=None:
                            (preview({"threshold": 0.72, "radius": 7, "min_px": 6, "drop_blue": 1, "keep_solid": 1}) or
                             {"threshold": 0.72, "radius": 7, "min_px": 6, "drop_blue": 1, "keep_solid": 1}))
        before = len(window._current().layers)
        window.act_import_scan.trigger()
        page = window._current()
        assert len(page.layers) == before + 1
        layer = page.layers[-1] if page.layers[-1].title.startswith("線画") else next(x for x in page.layers if x.title.startswith("線画"))
        from genko.raster import ensure_raster

        pixels = ensure_raster(page, layer)
        corner = pixels.getpixel((5, 5))
        assert corner[3] == 0  # (the paper is gone: only lines are left)
        assert any(pixels.getpixel((x, y))[3] == 255 for x in range(pixels.width // 2 - 20, pixels.width // 2 + 20)
                   for y in range(pixels.height // 2 - 20, pixels.height // 2 + 20))
    finally:
        window.close()
