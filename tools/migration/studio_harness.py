#!/usr/bin/env python3
"""The Python reference for the studio's C++ contract tests (native/tests/contract/test_contract_studio.cpp): the studio
book the op cases run on, the studio's ops saved one batch at a time (the approval audit, Undo and Redo by actor), and
the studio's own files (studio/presence.json, studio/logs/tools.jsonl, studio/claims/, studio/requests/) and checks
(the input schemas, jsonschema_lite, the state read from a book). Development only; nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/studio_harness.py <command> ...

Commands (JSON is UTF-8 without escapes):
  make-studiobook DEST         the v3 book of the studio op cases (native/tests/contract/studio_cases.json names its
                               ids): the op contract book (pyref_harness.py make-opsbook) with a studio — a bible
                               with characters (one approved and locked), locations, props, a script, page plans,
                               briefs with regions, references and candidates, requests, adopted art, approvals,
                               tickets of every kind, a proposal, the style and its finish — and its assets (pictures
                               of several sizes, a JPEG and pictures Pillow cannot open, under assets/ as .png). Ids are
                               counted (from 0xa00 for the studio's additions), so it is the same book every time.
  make-placedbook DEST         the book of the placed pictures' drawing: the studio book with pictures placed on every
                               page in every way the drawing tells apart (make_placedbook), ids counted from 0xc00
  batch JOBS                   jobs (a JSON list), in one process:
                               {"op": "steps", …} as pyref_harness.py's steps job, with "clock" (a text): what
                               time.strftime gives meanwhile (allow_chat_approval's "at") |
                               {"op": "saved", "book", "dest", "clock"?, "steps": [{"ops", "agent"} |
                               {"restore": actor, "redo"?, "force"?}], "out"} (the book copied to dest; each batch
                               applied and saved by its agent as genko apply does, each restore as genko undo / redo
                               does: the reply, and the audit lines and journal kinds each step added: saved_job) |
                               {"op": "files", "project", "config", "calls": [...], "out"} (the studio's files and
                               checks called one after another: files_job)
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import copy
import io
import json
import shutil
import struct
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for path in (str(ROOT / "src"), str(HERE)):
    if path not in sys.path:
        sys.path.insert(0, path)

import pyref_harness as base  # noqa: E402  (the shared reference: ids, the steps job, the op book)


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


@contextlib.contextmanager
def fixed_clock(text: str | None):
    """time.strftime gives `text` meanwhile (the C++ tests fix core::local_time_text the same way)."""
    if text is None:
        yield
        return
    original = time.strftime
    time.strftime = lambda fmt, *rest: text  # noqa: E731
    try:
        yield
    finally:
        time.strftime = original


# --- the studio book ----------------------------------------------------------------------------------------------------


def _png(size, rgba=(200, 30, 30, 255), mode="RGBA") -> bytes:
    from PIL import Image

    buf = io.BytesIO()
    Image.new(mode, size, rgba if mode == "RGBA" else rgba[: len(mode)]).save(buf, format="PNG")
    return buf.getvalue()


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def _bomb_png(width: int, height: int) -> bytes:
    """A PNG whose header says width × height (RGB, 8 bits) with no pixels: Pillow refuses it as a decompression bomb
    when it opens it (more than 2 × MAX_IMAGE_PIXELS) before reading any pixels."""
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + _chunk(b"IDAT", zlib.compress(b"")) + _chunk(b"IEND", b""))


STUDIO_ASSETS = {
    # name: (bytes maker): the studio book's pictures, by the names the cases use ($A1 … in studio_cases.json)
    "A1": lambda: _png((40, 30), (200, 30, 30, 255)),
    "A2": lambda: _png((30, 40), (30, 200, 30, 180)),
    "A3": lambda: _png((64, 48), (30, 30, 200, 255), "RGB"),
    "S1": lambda: _png((20, 30), (90, 90, 90, 255)),
    "S2": lambda: _png((22, 30), (120, 60, 60, 255)),
    "F1": lambda: _png((8, 8), (240, 200, 180, 255)),
    "L1": lambda: _png((48, 32), (180, 180, 140, 255)),
    "G1": lambda: _png((36, 36), (128, 128, 128, 255), "L"),
}


def _jpeg() -> bytes:
    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGB", (24, 16), (10, 120, 200)).save(buf, format="JPEG", quality=90)
    return buf.getvalue()


# 9×6 pictures as Pillow writes them, cut short in their pixels (native/tests/unit/test_placed.cpp's): Image.open reads
# their size from the header, load() fails ("image file is truncated").
_CUT_PICTURES = {
    "JX": "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4z"
          "NDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAGAAkDASIAAhEB"
          "AxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS"
          "0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKz"
          "tLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgEC"
          "BAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpj"
          "ZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6"
          "/9oADAMBAAIRAxEAPwDz+w==",
    "BX": "Qk3eAAAAAAAAADYAAAAoAAAACQAAAAYAAAABABgAAAAAAKgAAADEDgAAxA4AAAAAAAAAAAAAHh7IHh7IHh7IHh7IHh7IHh7I/wAA/wAA/wAAAB4e"
          "yB4eyB4eyB4eyB4eyP8AAP8AAP8AAB4eyAAeHsgeHsgeHsj/AAD/AAD/AAD/AAAeHsgeHsgAHh7IHh7I/wAA/wAA/wAAHh7IHh7IHh7IHh7IAP8AAP8A"
          "AP8AAP8AAB4eyB4=",
    "GX": "R0lGODdhCQAGAIIAAMwAAMwzAMwAM8wzMwAA/wAAAAAAAAAAACwAAAAACQAGAAAIIgAHEAgggKBBAggJDCC4cACAhAULMkyoEA==",
}


def _odd_assets() -> dict[str, bytes]:
    """Files under assets/ as .png that are not plain PNGs: a JPEG (Pillow opens it by its bytes), bytes that are no
    picture, a PNG cut short after its header (Pillow reads its size; its pixels are not there), a PNG whose header's
    checksum is wrong (Pillow cannot identify it), a header too large to open (a decompression bomb), and a JPEG, a BMP
    and a GIF cut short in their pixels (JX, BX, GX: Pillow reads their size)."""
    good = _png((16, 12), (60, 160, 60, 255))
    crc = bytearray(good)
    crc[29] ^= 0xFF  # (IHDR's CRC: after the 8-byte signature, 4 length, 4 type, 13 data)
    return {"J1": _jpeg(), "BAD": b"this is not a picture", "CUT": good[:60], "CRC": bytes(crc),
            "BOMB": _bomb_png(20000, 20000), **{name: base64.b64decode(text) for name, text in _CUT_PICTURES.items()}}


def make_studiobook(dest: str) -> dict:
    """The studio book (see the module's doc); returns {name: ref} of its assets (also written to DEST/assets.json, which
    is not part of the book)."""
    from genko.assets import AssetStore
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode

    base.make_opsbook(dest)
    base.fresh_process_state(True)
    episode = load_episode(Path(dest))
    base.counting_ids(0xA00)
    store = AssetStore(Path(dest))
    refs = {name: store.put_bytes(make(), ".png") for name, make in STUDIO_ASSETS.items()}
    refs.update({name: store.put_bytes(data, ".png") for name, data in _odd_assets().items()})

    def run(ops, agent="human:作者"):
        with fixed_clock("2026-10-01T09:00:00"):
            reply = apply_ops(episode, copy.deepcopy(ops), agent=agent)
        if not reply.get("ok"):
            raise SystemExit(f"make-studiobook: {ops}: {reply}")

    l1a, l1b, l1c = "000000000028", "000000000029", "000000000026"
    run([{"op": "set_studio", "premise": "放課後の屋上で、ユキが落とし物を拾う",
          "bible_doc": {"title": "屋上の落とし物", "logline": "拾った鞄の持ち主を探す", "themes": ["友情"],
                        "style": {"notes": ["線は細め", "背景は省略気味"]}, "author": "作者"},
          "style": {"notes": ["スクリーントーンは控えめ"], "finish": {"black": 50, "steps": [0.15, 0.3]}},
          "policy": {"auto_adopt": False}},
         {"op": "set_bible", "plot": "ユキが屋上で鞄を拾い、持ち主のケンを探す。", "constraints": ["暴力なし", "8ページ"],
          "characters": [{"id": "c-yuki", "name": "ユキ", "role": "主人公",
                          "look": {"hair": "黒髪のボブ", "eyes": "大きい", "build": "小柄", "height_cm": 152,
                                   "outfits": [{"id": "o1", "desc": "制服"}], "marks": ["ヘアピン"],
                                   "clothes_value": "beta"}},
                         {"id": "c-ken", "name": "ケン", "role": "相手", "look": {"hair": "短髪", "clothes_value": "white"}},
                         {"id": "c-mio", "name": "ミオ", "role": "友人", "look": {}}]},
         {"op": "upsert_location", "location": {"id": "loc-roof", "name": "屋上", "desc": "フェンスのある屋上", "times": ["夕方"]}},
         {"op": "upsert_location", "location": {"id": "loc-room", "name": "教室", "desc": "", "times": []}},
         {"op": "upsert_prop", "prop": {"id": "prop-bag", "name": "鞄", "desc": "紺色の通学鞄"}},
         {"op": "set_script", "script": {"scenes": [{"id": "s1", "summary": "屋上で鞄を拾う", "location_id": "loc-roof",
                                                     "time": "夕方", "beats": [{"id": "b1", "kind": "dialogue",
                                                                                 "speaker_id": "c-yuki", "text": "これ、誰の？",
                                                                                 "emotion": "驚き", "page": 1, "reveal": False}]}]}},
         {"op": "set_page_plan", "page": 2, "plan": {"name": {"page": 2, "turn_role": "normal", "panels": []},
                                                     "input_hash": "sha256:" + "11" * 32, "by": "ai:hermes"}},
         {"op": "record_review", "page": 2, "kind": "name", "score": 0.7, "notes": "流れは良い", "input_hash": "h1"},
         {"op": "register_assets", "assets": {refs["A1"]: {"kind": "art", "origin": "agent"}, refs["L1"]: {"kind": "ref"}}},
         {"op": "attach_reference", "target": {"location_id": "loc-roof"}, "asset": refs["L1"], "kind": "location"},
         {"op": "allow_chat_approval", "on": True}])
    # sheets: c-ken's approved (locked), c-yuki's candidates from two agents and a person
    run([{"op": "import_candidates", "character_id": "c-ken",
          "candidates": [{"id": "cd_k1", "asset": refs["S1"], "px": [20, 30], "origin": {"kind": "agent", "tool_id": "openai:gpt-image-1"}},
                         {"id": "cd_k2", "asset": refs["S2"], "px": [22, 30], "origin": {"kind": "agent"}}]}], agent="ai:hermes")
    run([{"op": "approve", "gate": "sheet", "character_id": "c-ken", "candidate_id": "cd_k1", "face_asset": refs["F1"]}])
    run([{"op": "import_candidates", "character_id": "c-yuki",
          "candidates": [{"id": "cd_y1", "asset": refs["S2"], "px": [22, 30], "origin": {"kind": "agent"},
                          "face_box01": [0.2, 0.1, 0.5, 0.4], "metrics": {"sharp": 0.8}}]}], agent="ai:hermes")
    run([{"op": "import_candidates", "character_id": "c-yuki",
          "candidates": [{"id": "cd_y2", "asset": refs["A2"], "px": [30, 40], "origin": {"kind": "agent"}}]}], agent="ai:other")
    run([{"op": "import_candidates", "character_id": "c-yuki",
          "candidates": [{"id": "cd_y3", "asset": refs["G1"], "px": [36, 36], "origin": {"kind": "human"}}]}])
    run([{"op": "import_candidates", "location_id": "loc-roof",
          "candidates": [{"id": "cd_l1", "asset": refs["L1"], "px": [48, 32], "origin": {"kind": "agent"}}]}], agent="ai:hermes")
    # page 1: briefs (a person's pinned shot and prompt override on the first panel; regions and references on the last),
    # a request, its candidates and the art adopted; a fix a person asked for on the middle panel
    run([{"op": "set_panel", "page": 1, "frame_id": l1a, "set": {"shot": "MS", "angle": "eye", "action": "鞄を拾う",
                                                                 "characters": [{"id": "c-yuki", "pose": "しゃがむ", "tag": "ユキ"}],
                                                                 "gen": {"prompt_override": "kneeling girl"}},
          "pin": ["shot"]}])
    run([{"op": "set_panel", "page": 1, "frame_id": l1c, "set": {"shot": "LS", "location_id": "loc-roof", "time": "夕方",
                                                                 "characters": [{"id": "c-yuki"}, {"id": "c-ken"}]}},
         {"op": "add_region", "page": 1, "frame_id": l1c, "region": {"id": "rg_face", "kind": "face", "rect_mm": [60, 150, 20, 22], "char": "c-yuki"}},
         {"op": "bind_ref", "page": 1, "frame_id": l1c, "ref": {"id": "rf_roof", "source": refs["L1"], "role": "location"}}])
    run([{"op": "add_region", "page": 1, "frame_id": l1c, "region": {"id": "rg_body", "kind": "person", "rect_mm": [55, 165, 40, 80], "char": "c-yuki", "confidence": 0.9}},
         {"op": "open_request", "request": {"id": "rq_open", "purpose": "art", "mode": "new", "target": {"page": 1, "frame_id": l1c},
                                            "brief_hash": "sha256:" + "22" * 32, "pad_mm": 3.0}},
         {"op": "import_candidates", "request_id": "rq_open",
          "candidates": [{"id": "cd_a1", "asset": refs["A1"], "px": [40, 30], "origin": {"kind": "agent", "tool_id": "openai:gpt-image-1"}},
                         {"id": "cd_a2", "asset": refs["A3"], "px": [64, 48], "origin": {"kind": "agent", "tool_id": "openai:gpt-image-1"},
                          "mapping": {"pad_mm": 1.5}},
                         {"id": "cd_a3", "asset": refs["J1"], "px": [24, 16], "origin": {"kind": "genko", "tool_id": "genko:upscale"},
                          "parent": "cd_a1"}]},
         {"op": "open_request", "request": {"id": "rq_edit", "purpose": "art", "mode": "edit", "target": {"page": 1, "frame_id": l1b}}},
         {"op": "close_request", "id": "rq_edit", "reason": "cancelled"},
         {"op": "review_candidates", "page": 1, "frame_id": l1c, "reviews": [{"candidate_id": "cd_a1", "score": 0.8, "note": "良い"}]},
         {"op": "adopt_candidate", "page": 1, "frame_id": l1c, "candidate_id": "cd_a1"}], agent="ai:hermes")
    run([{"op": "request_fix", "page": 1, "frame_id": l1b, "instruction": "表情をもっと明るく", "scope": "person"},
         {"op": "request_approval", "gate": "name", "pages": [2, 3], "note": "ネームを見てください"},
         {"op": "request_approval", "gate": "sheet", "character_id": "c-yuki"},
         {"op": "ask_human", "text": "鞄の色は紺でいいですか", "page": 1, "frame_id": l1c, "item": "art"},
         {"op": "propose", "proposal": {"id": "pr_layout", "kind": "layout", "page": 2, "tree": {"rect_mm": [20, 25, 150, 230]}}},
         {"op": "propose", "proposal": {"id": "pr_lines", "kind": "lines", "page": 3, "lines": []}}], agent="ai:hermes")
    run([{"op": "approve", "gate": "script"},
         {"op": "approve", "gate": "bible", "via": "telegram", "message": "OK です"},
         # page 6 approved name and art (nothing adopted there): the style is fixed from it
         {"op": "approve", "gate": "name", "page": 6}, {"op": "approve", "gate": "art", "page": 6},
         # a person's fixes for the agent: one on the adopted panel, one on page 3 (answered below)
         {"op": "request_fix", "page": 1, "frame_id": l1c, "candidate_id": "cd_a2", "instruction": "空を夕焼けに",
          "scope": "background"},
         {"op": "request_fix", "page": 3, "instruction": "見開きの流れを確認"},
         {"op": "set_finish", "page": 1, "frame_id": l1c, "finish": {"face_light": 0.3}}])
    answered = next(t["id"] for t in episode.tickets if t.get("text") == "見開きの流れを確認")
    run([{"op": "resolve_ticket", "id": answered, "note": "直しました"}], agent="ai:hermes")
    save_episode(episode, Path(dest), actor="human:作者")
    (Path(dest) / "assets.json").write_text(dumps(refs), encoding="utf-8")
    return refs


# --- the placed pictures book -------------------------------------------------------------------------------------------


def _placed_pictures() -> dict[str, bytes]:
    """The pictures of the placed pictures book: art with greys, lines, a face, dark clothes, a clear corner and a
    half-clear band (RGBA); fine grain (L); a palette picture with a clear index; grey with alpha (LA); 16-bit grey; a
    colourful picture (RGB) and its JPEG; black line art on white."""
    import random

    from PIL import Image, ImageDraw

    rng = random.Random(5101)

    def save(im, fmt="PNG", **kw) -> bytes:
        buf = io.BytesIO()
        im.save(buf, format=fmt, **kw)
        return buf.getvalue()

    art = Image.new("RGBA", (240, 180), (255, 255, 255, 255))
    d = ImageDraw.Draw(art)
    for x in range(240):
        d.line([(x, 0), (x, 179)], fill=(255 - x, 255 - x // 2, 230 - x // 3, 255))
    d.ellipse((30, 20, 110, 100), fill=(250, 220, 200, 255), outline=(0, 0, 0, 255), width=3)
    d.rectangle((120, 60, 220, 170), fill=(40, 40, 60, 255))
    d.rectangle((150, 90, 180, 120), fill=(200, 200, 210, 255))
    for _ in range(14):
        d.line([(rng.randrange(240), rng.randrange(180)), (rng.randrange(240), rng.randrange(180))], fill=(10, 10, 10, 255),
               width=rng.choice([1, 2, 4]))
    d.rectangle((200, 0, 239, 40), fill=(0, 0, 0, 0))
    d.rectangle((0, 150, 60, 179), fill=(90, 160, 90, 110))
    grain = Image.frombytes("L", (160, 160), bytes(min(255, rng.randrange(128) + (100 if (x // 20 + y // 20) % 2 else 20))
                                                   for y in range(160) for x in range(160)))
    pal = Image.new("P", (90, 120), 0)
    pal.putpalette([0, 0, 0, 230, 230, 230, 120, 60, 60, 60, 60, 200] + [0] * (256 * 3 - 12))
    dp = ImageDraw.Draw(pal)
    dp.rectangle((10, 10, 80, 60), fill=1)
    dp.ellipse((20, 50, 70, 110), fill=2)
    dp.line([(0, 119), (89, 0)], fill=3, width=3)
    la = Image.new("LA", (64, 48), (200, 255))
    ImageDraw.Draw(la).ellipse((8, 8, 56, 40), fill=(30, 140))
    deep = Image.new("I;16", (50, 40))
    deep.putdata([(x * 1300 + y * 300) % 65536 for y in range(40) for x in range(50)])
    colour = Image.new("RGB", (200, 150), (240, 240, 250))
    dc = ImageDraw.Draw(colour)
    dc.rectangle((0, 100, 199, 149), fill=(30, 140, 60))
    dc.ellipse((120, 10, 180, 70), fill=(250, 140, 20))
    dc.polygon([(20, 100), (70, 30), (120, 100)], fill=(120, 60, 160))
    ink = Image.new("RGB", (180, 120), (255, 255, 255))
    di = ImageDraw.Draw(ink)
    for _ in range(10):
        di.line([(rng.randrange(180), rng.randrange(120)), (rng.randrange(180), rng.randrange(120))], fill=(0, 0, 0),
                width=rng.choice([1, 3]))
    di.rectangle((60, 40, 120, 80), fill=(150, 150, 150))
    return {"P_ART": save(art), "P_GRAIN": save(grain), "P_PAL": save(pal, transparency=0), "P_LA": save(la),
            "P_16": save(deep), "P_COL": save(colour), "P_JPG": save(colour.resize((120, 90)), "JPEG", quality=88),
            "P_INK": save(ink)}


def make_placedbook(dest: str) -> dict:
    """The book of the placed pictures' drawing (test_contract_studio's placedPicturesLikePython): the studio book with
    pictures placed on every page — in panels cut square, rounded, bowed and slanted, out to the bleed, past the paper,
    on the page alone; adopted art with faces and clothes the bible prints black and white; finishes of the style and
    of the panel (steps, FM noise, the screen, smoothing off, face light past 1); extracted line art, a background, a
    draft and a name picture; pictures of every mode and a JPEG; a painting app's colour picture on a mono page; a colour
    page; pictures hidden, missing and of a panel that is not there. Returns {name: ref} (DEST/assets.json too)."""
    import dataclasses

    from genko.assets import AssetStore
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode
    from genko.models import Layer, LayerKind, LayerRole, Rect

    refs = make_studiobook(dest)
    base.fresh_process_state(True)
    base.counting_ids(0xC00)
    episode = load_episode(Path(dest))
    store = AssetStore(Path(dest))
    refs.update({name: store.put_bytes(data, ".png") for name, data in _placed_pictures().items()})

    def run(ops, agent="human:作者"):
        with fixed_clock("2026-10-02T09:00:00"):
            reply = apply_ops(episode, copy.deepcopy(ops), agent=agent)
        if not reply.get("ok"):
            raise SystemExit(f"make-placedbook: {ops}: {reply}")

    def place(page, **how):
        run([dict({"op": "place_asset", "page": page}, **how)])

    run([{"op": "register_assets", "assets": {refs[n]: {"kind": "art"} for n in _placed_pictures()}}])
    # page 1: a rounded panel (the old placed art), a bowed one with art fitted inside, a person and a head in the
    # rounded one (c-ken's clothes print white), the adopted art's own finish (face light) and the style's
    run([{"op": "set_frame", "page": 1, "frame_id": "000000000028", "corner_mm": 4.0},
         {"op": "set_frame", "page": 1, "frame_id": "000000000029", "curves": [2.0, -1.5, 0.0, 1.5]},
         {"op": "add_region", "page": 1, "frame_id": "000000000028", "region": {"id": "rg_ken", "kind": "person", "rect_mm": [30, 40, 40, 70], "char": "c-ken"}},
         {"op": "add_region", "page": 1, "frame_id": "000000000028", "region": {"id": "rg_kenhead", "kind": "head", "rect_mm": [40, 35, 15, 15]}}])
    place(1, asset=refs["P_ART"], frame_id="000000000029", fit="contain", clip_to="frame")
    # page 2: a slanted panel's background out to the bleed, a picture past its panel on the page alone; the panels'
    # own finishes (FM noise and odd steps; a coarse screen at an angle, faces lightened past white, other line limits)
    place(2, asset=refs["P_PAL"], frame_id="drawn-a", clip_to="none", placement_mm=[10, 15, 90, 120])
    place(2, asset=refs["P_LA"], frame_id="drawn-b", clip_to="bleed", to="bg")
    place(2, asset=refs["P_GRAIN"], frame_id="drawn-b", clip_to="frame", placement_mm=[90, 40, 70, 60])
    run([{"op": "set_finish", "page": 2, "frame_id": "drawn-b",
          "finish": {"screen": "fm", "steps": [0.25, "0.5", 2], "smooth": 0, "black": 20, "white": 240}},
         {"op": "set_finish", "page": 2, "frame_id": "drawn-a",
          "finish": {"lpi": 30, "angle": 15, "face_light": 1.5, "line_threshold": 140.5, "line_contrast": 30, "smooth": 2.5}},
         {"op": "add_region", "page": 2, "frame_id": "drawn-a", "region": {"id": "rg_face2", "kind": "face", "rect_mm": [30, 40, 25, 30]}}])
    # page 3: a slanted bleed panel with colour art out to the bleed; line art in the other
    run([{"op": "set_frame", "page": 3, "frame_id": "000000000033", "bleed": True}])
    place(3, asset=refs["P_COL"], frame_id="000000000033", clip_to="bleed", fit="cover")
    place(3, asset=refs["P_INK"], frame_id="000000000034", clip_to="frame")
    # page 4: a JPEG on the page, 16-bit grey stretched over the panel, a draft and a name picture
    place(4, asset=refs["P_JPG"], placement_mm=[20, 20, 60, 45], clip_to="none")
    place(4, asset=refs["P_16"], frame_id="000000000018", fit="stretch")
    place(4, asset=refs["P_ART"], frame_id="000000000018", to="draft", placement_mm=[100, 150, 50, 40])
    place(4, asset=refs["P_ART"], frame_id="000000000018", to="name", placement_mm=[30, 200, 50, 40])

    by_index = {p.index: p for p in episode.pages}
    p3, p4, p5, p6, p7 = by_index[3], by_index[4], by_index[5], by_index[6], by_index[7]
    for layer in p3.layers:  # (extracted line art)
        if layer.kind == LayerKind.PLACED and layer.asset == refs["P_INK"]:
            layer.source = dict(layer.source or {}, to="ink")
    # a painting app's pictures on a mono page: one in colour (finished like placed art), one grey (left as it is)
    from PIL import Image, ImageDraw

    def picture(colour: bool) -> bytes:
        im = Image.new("RGBA", (208, 283), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.ellipse((20, 150, 120, 260), fill=(220, 60, 40, 255) if colour else (90, 90, 90, 255))
        d.rectangle((110, 30, 190, 120), fill=(40, 90, 200, 200) if colour else (160, 160, 160, 200))
        d.line([(0, 0), (207, 282)], fill=(0, 0, 0, 255), width=3)
        buf = io.BytesIO()
        im.save(buf, format="PNG")
        return buf.getvalue()

    p4.layers.append(Layer(id="psdcolour001", role=LayerRole.USER, kind=LayerKind.RASTER, raster_png=picture(True),
                           source={"kind": "psd", "file": "x.psd"}))
    p4.layers.append(Layer(id="psdgrey00001", role=LayerRole.USER, kind=LayerKind.RASTER, raster_png=picture(False),
                           source={"kind": "psd"}))
    # page 5: grain past the paper's right edge on the page alone, smoothed hard, no steps (the default ones)
    p5.layers.append(Layer(id="placed5grain", role=LayerRole.USER, kind=LayerKind.PLACED, asset=refs["P_GRAIN"],
                           frame_id="00000000001e", placement_mm=Rect(150, 200, 100, 120), clip_to="none",
                           finish={"smooth": 3, "steps": []}))
    # page 6: a colour page (no finish)
    p6.spec = dataclasses.replace(p6.spec, expression="color")
    p6.layers.append(Layer(id="placed6colou", role=LayerRole.USER, kind=LayerKind.PLACED, asset=refs["P_COL"],
                           frame_id="000000000046", placement_mm=Rect(85, 25, 100, 80), clip_to="frame"))
    # page 7: a rounded bleed panel with art past the paper; art of a panel that is not there; hidden and missing art
    next(f for f in p7.leaf_frames() if f.id == "00000000004e").corner_mm = 6.0
    p7.layers.append(Layer(id="placed7bleed", role=LayerRole.USER, kind=LayerKind.PLACED, asset=refs["P_ART"],
                           frame_id="00000000004e", placement_mm=Rect(-10, -10, 230, 300), clip_to="bleed"))
    p7.layers.append(Layer(id="placed7nofra", role=LayerRole.USER, kind=LayerKind.PLACED, asset=refs["P_LA"],
                           frame_id="no-such-frame", placement_mm=Rect(60, 100, 40, 30), clip_to="frame"))
    p7.layers.append(Layer(id="placed7hidde", role=LayerRole.USER, kind=LayerKind.PLACED, asset=refs["P_COL"],
                           frame_id="00000000004e", placement_mm=Rect(20, 20, 100, 100), visible=False))
    p7.layers.append(Layer(id="placed7missi", role=LayerRole.USER, kind=LayerKind.PLACED, asset="sha256:" + "ab" * 32,
                           frame_id="00000000004e", placement_mm=Rect(20, 20, 100, 100)))
    # a page of placements far off the page (any finite size: nothing drawn, as Python draws nothing) and huge ones over
    # it, made by the ops: art moved and scaled by set_placement, pictures placed past the paper and around it, faces and
    # people reported far away (both corners past what a C int holds at every resolution the tests draw, 40 dpi and up:
    # Pillow's (int) cast makes them INT_MIN; not an ellipse hundreds of millions of pixels wide, nor one with a corner
    # inside that range and one past it, which Pillow takes minutes to draw)
    run([{"op": "add_page", "count": 1}])
    known = {p.id for p in by_index.values()}
    huge = next(p for p in episode.pages if p.id not in known)  # (the new page, before the cover)
    run([{"op": "split_frame", "page": huge.index, "frame_id": huge.leaf_frames()[0].id, "axis": "horizontal", "ratio": 0.5,
          "gutter_mm": 4}])
    huge = next(p for p in episode.pages if p.id == huge.id)  # (apply_ops works on a copy of the page)
    left, right = [f.id for f in huge.leaf_frames()][:2]
    run([{"op": "import_candidates", "page": huge.index, "frame_id": left,
          "candidates": [{"id": "cd_h1", "asset": refs["P_ART"], "px": [240, 180], "origin": {"kind": "agent"}}]},
         {"op": "import_candidates", "page": huge.index, "frame_id": right,
          "candidates": [{"id": "cd_h2", "asset": refs["P_COL"], "px": [200, 150], "origin": {"kind": "agent"}}]}],
        agent="ai:hermes")
    run([{"op": "approve", "gate": "name", "page": huge.index},
         {"op": "adopt_candidate", "page": huge.index, "frame_id": left, "candidate_id": "cd_h1"},
         {"op": "adopt_candidate", "page": huge.index, "frame_id": right, "candidate_id": "cd_h2"}])
    run([{"op": "set_placement", "page": huge.index, "frame_id": left, "offset_mm": [1e7, 0]},
         {"op": "set_placement", "page": huge.index, "frame_id": right, "scale": 1e6}])
    place(huge.index, asset=refs["P_GRAIN"], placement_mm=[1e8, 0, 10, 10], clip_to="none")
    place(huge.index, asset=refs["P_LA"], frame_id=right, placement_mm=[-1e9, -1e9, 2e9, 2e9])
    run([{"op": "add_region", "page": huge.index, "frame_id": right,
          "region": {"id": "rg_far", "kind": "face", "rect_mm": [1e10, 0, 1e10, 10]}},
         {"op": "add_region", "page": huge.index, "frame_id": right,
          "region": {"id": "rg_farbody", "kind": "person", "rect_mm": [1e10, 0, 1e10, 10], "char": "c-yuki"}},
         {"op": "add_region", "page": huge.index, "frame_id": right,
          "region": {"id": "rg_farhead", "kind": "head", "rect_mm": [4e8, 0, 10, 10]}}])
    save_episode(episode, Path(dest), actor="human:作者")
    (Path(dest) / "assets.json").write_text(dumps(refs), encoding="utf-8")
    return refs


# --- saved batches ------------------------------------------------------------------------------------------------------


def _lines(path: Path) -> list:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def saved_job(job: dict) -> None:
    """The book copied to job["dest"]; each step applied and saved there as `genko apply --agent` does (load, apply_ops,
    save_episode with the agent) or restored as `genko undo` / `genko redo` does (journal.restore under the lock, as
    pyref_harness's restore): the reply, and the audit lines ({actor, changes, via?}) and the journal entries' kinds the
    step added."""
    from genko.headless import apply_ops
    from genko.io import load_episode, save_episode
    from genko.ops import ApplyError

    dest = Path(job["dest"])
    shutil.copytree(job["book"], dest)
    records = []
    for step in job["steps"]:
        audit_before = len(_lines(dest / "studio" / "audit.jsonl"))
        journal_before = len(_lines(dest / "studio" / "journal.jsonl"))
        if "restore" in step:
            reply = base.restore_book(str(dest), step["restore"], bool(step.get("redo")), bool(step.get("force")))
        else:
            base.fresh_process_state(False)
            episode = load_episode(dest)
            agent = step.get("agent", "genko")
            try:
                with fixed_clock(job.get("clock")):
                    reply = apply_ops(episode, copy.deepcopy(step["ops"]), agent=agent)
            except ApplyError as exc:
                reply = {"ok": False, "error": str(exc)}
            if reply.get("ok"):
                save_episode(episode, dest, actor=agent)
                reply = {"ok": True, "applied": reply["applied"]}
        audit = [{k: line[k] for k in ("actor", "changes", "via") if k in line}
                 for line in _lines(dest / "studio" / "audit.jsonl")[audit_before:]]
        kinds = [line.get("kind") for line in _lines(dest / "studio" / "journal.jsonl")[journal_before:]]
        records.append({"reply": reply, "audit": audit, "journal": kinds})
    Path(job["out"]).write_text(dumps(records), encoding="utf-8")


def run_batch(jobs_path: str) -> None:
    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    for job in jobs:
        if job["op"] == "steps":
            with fixed_clock(job.get("clock")):
                base.steps_job(job)
        elif job["op"] == "saved":
            saved_job(job)
        elif job["op"] == "files":
            files_job(job)
        else:
            raise SystemExit(f"unknown job {job['op']!r}")


# --- the studio's files and checks ------------------------------------------------------------------------------------


def files_job(job: dict) -> None:
    """Calls of the studio's file and check modules, one after another, each with its result or its error
    ({"error": type, "message"}): job["calls"] = [{"fn": name, "args": {...}}], against the book folder job["project"]
    and the config folder job["config"] (GENKO_CONFIG_DIR for tools_registry). After each call, the studio files it may
    have written are read back as they are on disk ("files": {relative path: text}).

    fn: presence.actor_for {base, session} | presence.others {actor, now} | presence.check {actor, now} |
        presence.touch {actor, now} | toollog.record {actor, tool, kwargs, result, error, ms, at} (time.time() is `at`) |
        toollog.read {} | claims.holder {item, now} | claims.claim {item, actor, now} | claims.release {item, actor} |
        genreq.read {request_id} | genreq.write {request, files: {name: base64}} | jsonutil.content_hash {value} |
        jsonutil.write_json {path, value} | jsonutil.read_json {path, default} | schemas.validate {schema, data} |
        schemas.fill_nulls {schema, data} | jsonschema_lite.validate {schema, data} |
        jsonschema_lite.conservative_problems {schema} | state.* {book?, …} | tools_registry.* {…} |
        write {path, text} (a file put there as it is, for the next calls) | remove {path}
    """
    import base64
    import os

    os.environ["GENKO_CONFIG_DIR"] = job["config"]
    from genko.studio import claims, genreq, jsonschema_lite, jsonutil, presence, schemas, state, toollog, tools_registry

    project = Path(job["project"])
    out = []

    def snapshot_files() -> dict:
        """The studio's files as they are on disk: "studio/…" under the book, "config/…" under the config folder (each
        file's text, or "<bytes base64>" when it is not UTF-8)."""
        files = {}
        for prefix, folder in (("studio", project / "studio"), ("config", Path(job["config"]))):
            if not folder.is_dir():
                continue
            for path in sorted(folder.rglob("*")):
                if not path.is_file():
                    continue
                rel = prefix + "/" + str(path.relative_to(folder)).replace("\\", "/")
                data = path.read_bytes()
                try:
                    files[rel] = data.decode("utf-8")
                except UnicodeDecodeError:
                    files[rel] = "<bytes " + base64.b64encode(data).decode() + ">"
        return files

    for call in job["calls"]:
        fn, args = call["fn"], call.get("args", {})
        try:
            if fn == "write":
                target = project / args["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(args["text"].encode("utf-8"))
                result = None
            elif fn == "remove":
                target = project / args["path"]
                if target.is_dir():
                    shutil.rmtree(target)
                elif target.exists():
                    target.unlink()
                result = None
            elif fn == "presence.actor_for":
                result = presence.actor_for(args["base"], args.get("session"))
            elif fn == "presence.others":
                result = presence.others(project, args["actor"], args.get("now"))
            elif fn == "presence.check":
                result = presence.check(project, args["actor"], args.get("now"))
            elif fn == "presence.touch":
                result = presence.touch(project, args["actor"], args.get("now"))
            elif fn == "toollog.record":
                original = time.time
                time.time = lambda: args["at"]  # noqa: E731
                try:
                    result = toollog.record(project, args["actor"], args["tool"], args.get("kwargs", {}), args.get("result"),
                                            args.get("error"), args.get("ms", 0.0))
                finally:
                    time.time = original
            elif fn == "toollog.read":
                result = toollog.read(project)
            elif fn == "claims.holder":
                result = claims.holder(project, args["item"], args.get("now"))
            elif fn == "claims.claim":
                result = claims.claim(project, args["item"], args["actor"], args.get("now"))
            elif fn == "claims.release":
                original = time.time
                time.time = lambda: args["now"]  # noqa: E731  (release asks holder() for now)
                try:
                    result = claims.release(project, args["item"], args["actor"])
                finally:
                    time.time = original
            elif fn == "genreq.read":
                result = genreq.read(project, args["request_id"])
            elif fn == "genreq.write":
                pack = genreq.Pack(request=args["request"],
                                   files={name: base64.b64decode(data) for name, data in args.get("files", {}).items()})
                result = str(genreq.write(pack, project).relative_to(project)).replace("\\", "/")
            elif fn == "jsonutil.content_hash":
                result = jsonutil.content_hash(args["value"])
            elif fn == "jsonutil.canonical_json":
                result = jsonutil.canonical_json(args["value"])
            elif fn == "jsonutil.write_json":
                result = jsonutil.write_json(project / args["path"], args["value"])
            elif fn == "jsonutil.read_json":
                result = jsonutil.read_json(project / args["path"], args.get("default"))
            elif fn == "schemas.validate":
                schema = schemas.SCHEMAS[args["schema"]]
                result = [i.to_dict() for i in jsonschema_lite.validate(schemas.fill_nulls(args["data"], schema), schema)]
            elif fn == "schemas.fill_nulls":
                result = schemas.fill_nulls(args["data"], schemas.SCHEMAS[args["schema"]])
            elif fn == "schemas.schema":
                result = schemas.SCHEMAS[args["schema"]]
            elif fn == "jsonschema_lite.validate":
                result = [i.to_dict() for i in jsonschema_lite.validate(args["data"], args["schema"])]
            elif fn == "jsonschema_lite.conservative_problems":
                result = jsonschema_lite.conservative_problems(args["schema"])
            elif fn.startswith("state."):
                from genko.io import load_episode

                episode = load_episode(Path(args["book"]))
                name = fn.split(".", 1)[1]
                if name in ("bible", "script", "names"):
                    result = getattr(state, name)(episode)
                elif name in ("name", "name_review", "page_fixes"):
                    result = getattr(state, name)(episode, args["page"])
                elif name == "open_tickets":
                    result = state.open_tickets(episode, args.get("kind"))
                elif name == "written_look":
                    result = state.written_look(args["character"])
                else:
                    raise SystemExit(f"unknown state call {fn}")
                if isinstance(result, dict) and name == "names":
                    result = {str(k): v for k, v in result.items()}
            elif fn == "tools_registry.load":
                result = tools_registry.load()
            elif fn == "tools_registry.get":
                tool_id, spec = tools_registry.get(args.get("tool_id"))
                result = [tool_id, spec]
            elif fn == "tools_registry.set_tool":
                result = tools_registry.set_tool(args["tool_id"], args["spec"])
            elif fn == "tools_registry.remove":
                result = tools_registry.remove(args["tool_id"])
            else:
                raise SystemExit(f"unknown call {fn}")
            out.append({"result": result, "files": snapshot_files()})
        except SystemExit:
            raise
        except Exception as exc:  # (each module's own errors, as the C++ build words them)
            out.append({"error": type(exc).__name__, "message": str(exc), "files": snapshot_files()})
    Path(job["out"]).write_text(dumps(out), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make-studiobook")
    p.add_argument("dest")
    p = sub.add_parser("make-placedbook")
    p.add_argument("dest")
    p = sub.add_parser("batch")
    p.add_argument("jobs")
    args = parser.parse_args(argv)
    if args.cmd == "make-studiobook":
        make_studiobook(args.dest)
    elif args.cmd == "make-placedbook":
        make_placedbook(args.dest)
    elif args.cmd == "batch":
        run_batch(args.jobs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
