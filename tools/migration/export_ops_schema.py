#!/usr/bin/env python3
"""Write the op catalogue the C++ `genko schema` prints, from the Python baseline's own output.

    python3 tools/migration/export_ops_schema.py      # needs Pillow and numpy (the baseline imports them)

Runs `python -m genko schema` against src/genko and stores its "ops" list as native/src/core/data/ops_schema.json
(UTF-8, indent 1). The C++ build embeds this file; tests check that `genko schema` prints exactly this list. When an
op's arguments change in C++, change this file in the same commit (it is the public contract of the ops).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "native" / "src" / "core" / "data" / "ops_schema.json"


def main() -> int:
    python = os.environ.get("GENKO_PYREF", sys.executable)
    env = {**os.environ, "PYTHONPATH": str(ROOT / "src"), "PYTHONDONTWRITEBYTECODE": "1"}
    done = subprocess.run([python, "-m", "genko", "schema"], capture_output=True, env=env, check=True)
    payload = json.loads(done.stdout)
    ops = payload["ops"]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(ops, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    names = [op["op"] for op in ops]
    print(json.dumps({"ops": len(ops), "unique": len(set(names)), "path": str(OUT.relative_to(ROOT))}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
