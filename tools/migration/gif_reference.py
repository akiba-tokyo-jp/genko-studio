#!/usr/bin/env python3
"""Check the curated GIF inputs and fixed first-frame oracle using Pillow 12.3.0.

The original GIF bytes are fixtures, not generated product output. This command
never rewrites them or the expected tables. Run in the pinned pyref environment.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path

import PIL
from PIL import Image, UnidentifiedImageError


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", required=True)
    parser.parse_args()
    assert PIL.__version__ == "12.3.0", PIL.__version__
    root = Path(__file__).resolve().parents[2]
    folder = root / "native/tests/data/pyref/gif"
    tables = json.loads((folder / "tables.json").read_text())
    hashes = {}
    for row in tables:
        path = folder.parent / row["file"]
        data = path.read_bytes()
        with Image.open(io.BytesIO(data)) as image:
            image.load()
            assert image.mode == row["mode"] and list(image.size) == row["size"], row["name"]
            for key, pixels in [("data", image.tobytes()), ("rgba", image.convert("RGBA").tobytes()),
                                ("l", image.convert("L").tobytes())]:
                assert pixels == base64.b64decode(row[key]), (row["name"], key)
            assert image.info.get("transparency") == row.get("transparency_index"), row["name"]
        hashes[row["file"]] = hashlib.sha256(data).hexdigest()
    errors = json.loads((folder / "errors.json").read_text())
    for row in errors:
        data = base64.b64decode(row["b64"])
        digest = hashlib.sha256(data).hexdigest()
        if "sha256" in row:
            assert digest == row["sha256"], row["name"]
        hashes["error:" + row["name"]] = digest
        stage = "open"
        try:
            with Image.open(io.BytesIO(data)) as image:
                stage = "load"
                image.load()
        except Exception as error:
            expected = row["python"]
            code = "unidentified_image" if isinstance(error, UnidentifiedImageError) else "format"
            assert not expected["ok"] and type(error).__name__ == expected["type"]
            assert stage == expected["stage"] and code == expected["code"], row["name"]
            # Open failures embed an ephemeral BytesIO address; the error class/stage is the oracle there.
            if stage == "load":
                assert str(error) == expected["message"], row["name"]
        else:
            raise AssertionError("expected refusal: " + row["name"])
    assert len({row["name"] for row in tables}) == len(tables)
    assert len({row["name"] for row in errors}) == len(errors)
    print(json.dumps({"passed": True, "pillow": PIL.__version__, "normal": len(tables),
                      "refused": len(errors), "fixture_sha256": hashes}, ensure_ascii=False))


if __name__ == "__main__":
    main()
