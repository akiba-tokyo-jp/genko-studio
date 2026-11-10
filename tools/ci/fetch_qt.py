#!/usr/bin/env python3
"""Fetch the official Qt binaries for the C++ build from download.qt.io (no Qt account, no installer).

    python3 tools/ci/fetch_qt.py --os windows --out <dir>      # -> <dir>/6.11.2/msvc2022_64
    python3 tools/ci/fetch_qt.py --os linux --out <dir>        # -> <dir>/6.11.2/gcc_64

Reads the repository's Updates.xml, downloads the 7z archives of the modules Genko uses (qtbase, qtsvg,
qttools, qttranslations, qtimageformats; Windows also d3dcompiler and the software OpenGL), checks each one's
SHA-1 from the repository, and unpacks them with 7z. Only LGPL modules are fetched. Writes a qt.conf so the
tree works where it lies. Used by CI where aqtinstall does not yet know the repository's layout.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

BASE = "https://download.qt.io/online/qtsdkrepository"
VERSION = "6.11.2"
TAG = VERSION.replace(".", "")
LAYOUT = {
    "windows": {"repo": f"{BASE}/windows_x86/desktop/qt6_{TAG}/qt6_{TAG}_msvc2022_64", "arch": "win64_msvc2022_64",
                "dir": "msvc2022_64", "extra": ("d3dcompiler_47", "opengl32sw")},
    "linux": {"repo": f"{BASE}/linux_x64/desktop/qt6_{TAG}/qt6_{TAG}", "arch": "linux_gcc_64", "dir": "gcc_64", "extra": ()},
}
WANTED = ("qtbase", "qtsvg", "qttools", "qttranslations", "qtimageformats")


def fetch(url: str) -> bytes:
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=300) as response:
                return response.read()
        except OSError:
            if attempt == 3:
                raise
    raise RuntimeError("unreachable")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--os", choices=sorted(LAYOUT), required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    layout = LAYOUT[args.os]
    updates = ET.fromstring(fetch(layout["repo"] + "/Updates.xml"))
    packages = {p.findtext("Name"): p for p in updates.findall("PackageUpdate")}
    names = [f"qt.qt6.{TAG}.{layout['arch']}", f"qt.qt6.{TAG}.addons.qtimageformats.{layout['arch']}"]
    seven = shutil.which("7z") or shutil.which("7za")
    if seven is None:
        print("7z is needed", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    prefix = args.out / VERSION / layout["dir"]
    got = []
    for name in names:
        package = packages.get(name)
        if package is None:
            print(f"missing package {name}", file=sys.stderr)
            return 1
        version = package.findtext("Version")
        archives = [a.strip() for a in (package.findtext("DownloadableArchives") or "").split(",") if a.strip()]
        for archive in archives:
            if not (archive.startswith(WANTED) or archive.startswith(layout["extra"])):
                continue
            url = f"{layout['repo']}/{name}/{version}{archive}"
            data = fetch(url)
            expected = fetch(url + ".sha1").decode().split()[0].strip()
            actual = hashlib.sha1(data).hexdigest()
            if actual != expected:
                print(f"checksum mismatch for {archive}: {actual} != {expected}", file=sys.stderr)
                return 1
            target = args.out / archive
            target.write_bytes(data)
            # Newer repositories put the files at the archive's root (bin/, lib/…); older ones under
            # <version>/<arch>/. Unpack into a scratch folder and move whichever it is into place.
            scratch = args.out / "_unpack"
            shutil.rmtree(scratch, ignore_errors=True)
            subprocess.run([seven, "x", "-y", f"-o{scratch}", str(target)], check=True, stdout=subprocess.DEVNULL)
            nested = scratch / VERSION / layout["dir"]
            source = nested if nested.is_dir() else scratch
            prefix.mkdir(parents=True, exist_ok=True)
            for item in source.rglob("*"):
                if item.is_file():
                    dest = prefix / item.relative_to(source)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(item), dest)
            shutil.rmtree(scratch, ignore_errors=True)
            target.unlink()
            got.append(archive)
    if not (prefix / "lib" / "cmake" / "Qt6").is_dir():
        print(f"Qt not found under {prefix}", file=sys.stderr)
        return 1
    (prefix / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
    print(f"Qt {VERSION} ({args.os}) at {prefix}: {', '.join(got)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
