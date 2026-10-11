#!/usr/bin/env python3
"""Writes native/tests/contract/service_cases.json: the jobs of the studio service's contract test
(native/tests/contract/test_contract_service.cpp), run by tools/migration/service_harness.py with Python's
StudioService / HumanService / `genko studio` and by the test with this build's. Development only.

    PYTHONPATH=src python3 tools/migration/make_service_cases.py

Each job is {"name", "clock", "env", "steps"} (service_harness.py documents the steps). The inputs are the demo book of
tests/fixtures/studio/demo4 (a bible, a script and four name plans), copied in, and small pictures made here. Texts in
steps may hold "$root" (the job's root folder on each side); a whole text "$ticket:<book>:<n>" stands for the id of
that book's n-th ticket (Python's index: -1 the last), "$frame:<book>:<page>:<n>" for the n-th panel of a page,
"$candidate:<book>:<character>:<n>" for a character's n-th sheet candidate, "$panelcand:<book>:<page>:<panel>:<n>" for
the n-th candidate of a page's panel, "$request:<book>:<n>" and "$proposal:<book>:<n>" for the n-th art request and
proposal, and "$job:<book>:<n>" for the n-th background job, read from the book when the step runs.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import io
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures" / "studio" / "demo4"
OUT = ROOT / "native" / "tests" / "contract" / "service_cases.json"
BOOK = "demo.genko"


def fixture(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def png(size, colour, mode="RGBA", dots=()) -> bytes:
    from PIL import Image

    image = Image.new(mode, size, colour)
    for x, y, value in dots:
        image.putpixel((x, y), value)
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def ref(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def tool(name, *args, actor=None, **kwargs):
    step = {"tool": name, "args": list(args), "kwargs": kwargs}
    if actor:
        step["actor"] = actor
    return step


def human(name, *args, actor="human:leaf", project=BOOK, **kwargs):
    return {"human": name, "project": project, "actor": actor, "args": list(args), "kwargs": kwargs}


def asset(data: bytes, project=BOOK):
    return {"asset": project, "base64": base64.b64encode(data).decode("ascii")}


def write(rel: str, value) -> dict:
    if isinstance(value, bytes):
        return {"write": rel, "base64": base64.b64encode(value).decode("ascii")}
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)
    return {"write": rel, "text": text}


def drawing(size, colour, lines=(), mode="RGB", fmt="PNG") -> bytes:
    """A small picture with a few dark strokes (lines to find, edges to measure)."""
    from PIL import Image, ImageDraw

    image = Image.new(mode, size, colour)
    draw = ImageDraw.Draw(image)
    for box in lines:
        draw.line(box, fill=(20, 20, 20) if mode == "RGB" else 20, width=2)
    buf = io.BytesIO()
    image.save(buf, format=fmt)
    return buf.getvalue()


def scan(size=(296, 420), boxes=((12, 12, 284, 150), (12, 162, 140, 408), (152, 162, 284, 408))) -> bytes:
    """A hand-drawn name's scan: white paper, panel borders drawn in black (an A5 page's shape)."""
    from PIL import Image, ImageDraw

    image = Image.new("L", size, 255)
    draw = ImageDraw.Draw(image)
    for box in boxes:
        draw.rectangle(box, outline=0, width=3)
    draw.line((40, 60, 120, 90), fill=0, width=2)  # (a figure's scribble inside a panel)
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def art_job() -> dict:
    """Art (M5②b): requests and their guides, previews, imports, candidates and their reviews, adoption with
    Genko's and a person's enlargers, a panel's picture in one call (cut first), reported regions, the finishing pass,
    lines drawn from a picture, images brought in by path or base64; then the checks, the exports into the book, proofs,
    a background job, review.html and the blind character test."""
    bible, script = fixture("bible.json"), fixture("script.json")
    for scene in script["scenes"]:  # (a book of two pages)
        scene["beats"] = [b for b in scene["beats"] if b.get("page", 1) <= 2]
    plans = [fixture("p001.json"), fixture("p002.json")]
    origin = {"kind": "agent", "tool_id": "local:sd", "model": "m1", "prompt": "屋上", "secret": "x"}
    pic_a = drawing((64, 48), (200, 180, 160), [(4, 4, 60, 44), (10, 40, 50, 8)])
    pic_b = drawing((48, 64), (150, 170, 200), [(4, 60, 44, 4)])
    pic_c = drawing((80, 60), (230, 230, 230), [(0, 30, 80, 30), (40, 0, 40, 60)])
    pic_d = drawing((90, 70), (210, 200, 190), [(5, 5, 85, 65)])
    pic_e = drawing((40, 40), (120, 120, 120), [(0, 0, 39, 39)])
    photo = drawing((40, 30), (180, 90, 60), [(2, 2, 38, 28)], fmt="JPEG")
    gif = drawing((36, 28), (40, 160, 90), [(1, 1, 34, 26)], fmt="GIF")
    jpeg = drawing((44, 32), (90, 60, 200), [(0, 16, 43, 16)], fmt="JPEG")
    sheet_h = drawing((60, 90), (240, 230, 220), [(30, 5, 30, 85)])
    sheet_s = drawing((60, 90), (220, 230, 240), [(5, 45, 55, 45)])
    f0, f1, f2 = (f"$frame:{BOOK}:1:{n}" for n in range(3))
    req = f"$request:{BOOK}:0"
    steps = [
        tool("create_project", BOOK, "art", 2, spec_preset="doujin-a5"),
        tool("set_bible", BOOK, bible, commit=True),
        tool("set_script", BOOK, script, commit=True),
        tool("submit_name", BOOK, plans[0], commit=True, max_px=200),
        tool("submit_name", BOOK, plans[1], commit=True, max_px=200),
        tool("generation_request", BOOK, page=1, frame_id=f0),
        human("approve_name", [1, 2]),
        tool("generation_request", BOOK, page=1, frame_id=f0),
        tool("generation_request", BOOK, page=1, frame_id=f0, instruction="夕日を強く", tool="local:sd"),
        tool("generation_request", BOOK, page=2, frame_id=f"$frame:{BOOK}:2:0", purpose="draft"),
        tool("generation_request", BOOK, page=1, frame_id="nope"),
        tool("generation_request", BOOK, page=9, frame_id="x"),
        tool("generation_request", BOOK, character_id="hina"),
        tool("generation_request", BOOK, character_id="nobody"),
        tool("generation_request", BOOK, location_id="loc_rooftop"),
        tool("generation_request", BOOK, page=1, frame_id=f1, mode="nope"),
        tool("render", BOOK, 1, max_px=200),
        tool("render", BOOK, 1, mode="proof", max_px=200, frame_id=f1),
        tool("render", BOOK, 1, mode="nope"),
        tool("render", BOOK, 9),
        tool("render", BOOK, 1, kind="guide:composition", frame_id=f0, max_px=160),
        tool("render", BOOK, 1, kind="guide:pose", frame_id=f0, max_px=160),
        tool("render", BOOK, 1, kind="guide:keepout", frame_id=f0, max_px=160),
        tool("render", BOOK, 1, kind="nope", frame_id=f0),
        tool("render", BOOK, 1, kind="guide:pose", frame_id="nope"),
        tool("render", BOOK, 1, kind="compare", frame_id=f0),
        tool("render", BOOK, 1, layer_id="nope"),
        tool("render", BOOK, 1, kind="atari", max_px=160),
        asset(pic_a), asset(pic_b), asset(pic_c), asset(pic_d), asset(sheet_h), asset(sheet_s),
        write("pics/e.png", pic_e),
        write("pics/photo.jpg", photo),
        write("pics/not.png", b"not a picture"),
        tool("import_images", BOOK, "nope", []),
        tool("import_images", BOOK, req, []),
        tool("import_images", BOOK, req, [{"asset": ref(pic_a)}]),
        tool("import_images", BOOK, req, [{"asset": ref(pic_a), "origin": {"kind": "robot"}}]),
        tool("import_images", BOOK, req, [{"asset": "sha256:" + "0" * 64, "origin": origin}]),
        tool("import_images", BOOK, req, [{"file": "$root/pics/e.png", "origin": origin}]),
        tool("import_images", BOOK, req, [{"asset": ref(pic_a), "origin": origin}], preview=False),
        tool("import_images", BOOK, req, [{"asset": ref(pic_b), "origin": {"kind": "fixture"}},
                                          {"asset": ref(pic_c), "origin": {"kind": "agent"}}]),
        asset(gif), asset(jpeg),
        tool("import_images", BOOK, f"$request:{BOOK}:1", [{"asset": ref(gif), "origin": origin}, {"asset": ref(jpeg), "origin": origin,
                                                                                                   "face_box01": [0.2, 0.1, 0.5, 0.5]}],
             preview=False),
        tool("import_images", BOOK, f"$request:{BOOK}:1", [{"asset": ref(jpeg), "origin": origin, "face_box01": [0.9, 0.1, 0.5, 0.5]}]),
        tool("candidates", BOOK, page=1, frame_id=f0),
        tool("candidates", BOOK, page=1, frame_id="nope"),
        tool("candidates", BOOK, page=9),
        tool("candidates", BOOK, character_id="hina"),
        tool("render", BOOK, 1, kind="compare", frame_id=f0, max_px=160),
        tool("render", BOOK, 1, kind="compare", frame_id=f0, candidate_id="nope"),
        tool("review_candidates", BOOK, 1, f0, [{"candidate_id": f"$panelcand:{BOOK}:1:0:0", "score": 0.8}]),
        tool("review_candidates", BOOK, 1, f0, [{"candidate_id": f"$panelcand:{BOOK}:1:0:0", "score": 0.8,
                                                  "checks": {"likeness": 2, "hands": "ok", "text": "none", "cut": "none"}}]),
        tool("review_candidates", BOOK, 1, f0, [{"candidate_id": f"$panelcand:{BOOK}:1:0:0", "score": 0.8, "note": "よい",
                                                  "checks": {"likeness": 0.9, "hands": "ok", "text": "none", "cut": "some"}}]),
        tool("adopt", BOOK, "nope"),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:0:0", preview=False),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:0:1", page=1, frame_id=f0, upscale=False,
             regions=[{"kind": "face", "box01": [0.1, 0.1, 0.3, 0.3], "confidence": 0.6}], preview=True),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:0:2", page=1, frame_id=f0, regions=[{"kind": ""}], preview=False),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:0:2", page=1, frame_id=f0, to="draft", fit="contain", preview=False),
        tool("take_panel_art", BOOK, req, {"asset": ref(pic_d), "origin": origin}, crop01=[0.1, 0.2, 0.5, 0.6], upscale=False),
        tool("take_panel_art", BOOK, req, {"asset": ref(pic_d), "origin": origin}, crop01=[0.9, 0.9, 0.5, 0.5]),
        tool("take_panel_art", BOOK, req, {"asset": ref(pic_d), "origin": origin}, crop01="x"),
        tool("take_panel_art", BOOK, req, {"asset": "sha256:" + "1" * 64, "origin": origin}),
        tool("take_panel_art", BOOK, f"$request:{BOOK}:2", {"asset": ref(pic_d), "origin": origin}),
        tool("take_panel_art", BOOK, f"$request:{BOOK}:3", {"asset": ref(pic_d), "origin": origin}),
        tool("take_panel_art", BOOK, "nope", {}),
        tool("report_regions", BOOK, 1, f0, [{"kind": "face", "rect_mm": [10.123, 10, 20, 20.5]},
                                             {"kind": "hand", "box01": [0.5, 0.5, 0.2, 0.2], "confidence": 0.7, "char": "hina"}]),
        tool("report_regions", BOOK, 1, f0, [{"kind": "face"}]),
        tool("report_regions", BOOK, 1, f2, [{"kind": "face", "box01": [0, 0, 1, 1]}]),
        tool("report_regions", BOOK, 9, f0, []),
        tool("derive", BOOK, 1, f0),
        tool("derive", BOOK, 1, f0, kind="nope"),
        tool("derive", BOOK, 1, f2),
        tool("derive", BOOK, 1, "nope"),
        tool("derive", BOOK, 1, f0, candidate_id=f"$panelcand:{BOOK}:1:0:2", params={"threshold": 100}),
        tool("upscale", BOOK, 1, f0, scale=2),
        tool("upscale", BOOK, 1, f0, scale=2),
        tool("upscale", BOOK, 1, f0, method="nope"),
        tool("upscale", BOOK, 1, f0, scale=9),
        tool("upscale", BOOK, 1, f0, candidate_id=f"$panelcand:{BOOK}:1:0:2"),
        tool("upscale", BOOK, 1, f2),
        {"cli": ["upscaler", "add", "copy", "--command", "cp {in} {out}", "--scales", "2"]},
        {"cli": ["upscaler", "add", "broken", "--command", "sh -c 'echo oops >&2; exit 3' {in} {out}"]},
        {"cli": ["upscaler", "add", "missing", "--command", "genko-no-such-program {in} {out}"]},
        tool("upscalers"),
        tool("upscale", BOOK, 1, f0, scale=2, method="copy"),
        tool("upscale", BOOK, 1, f0, scale=3, method="broken"),
        tool("upscale", BOOK, 1, f0, scale=3, method="missing"),
        tool("upscale", BOOK, 1, f0, scale=1.5, background=True),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:0:0", method="copy", preview=False),
        tool("apply_ops", BOOK, [{"op": "import_candidates", "page": 1, "frame_id": frame,
                                  "candidates": [{"id": f"cd_art{n}", "asset": ref(pic), "px": [80, 60], "origin": origin}]}
                                 for n, (frame, pic) in enumerate(((f1, pic_c), (f2, pic_b)))], commit=True),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:1:0", upscale=False, preview=False),
        tool("adopt", BOOK, f"$panelcand:{BOOK}:1:2:0", preview=False),
        tool("finish_page", BOOK, 1),
        human("approve_art", [1]),
        tool("finish_page", BOOK, 1),
        tool("finish_page", BOOK, 1, commit=True),
        tool("finish_page", BOOK, 9),
        tool("import_image", BOOK, path="pics/e.png"),
        tool("import_image", BOOK, path="$root/pics/photo.jpg"),
        tool("import_image", BOOK, png_base64=b64(pic_e)),
        tool("import_image", BOOK, path="pics/e.png", png_base64=b64(pic_e)),
        tool("import_image", BOOK),
        tool("import_image", BOOK, png_base64="not base64!"),
        tool("import_image", BOOK, png_base64="QUJD="),
        *[tool("import_image", BOOK, png_base64=text) for text in ("=QUJD", "QQ==QQ==", "QU=D", "Q", "QUJD==", "QQ==", "QQ=", "QU JD")],
        tool("import_image", BOOK, png_base64=b64(b"not an image")),
        tool("import_image", BOOK, path="/etc/hostname"),
        tool("import_image", BOOK, path="pics/none.png"),
        tool("import_image", BOOK, path="pics/not.png"),
        tool("import_image", BOOK, path="/etc/hostname", confine=False),
        tool("apply_ops", BOOK, [{"op": "import_candidates", "character_id": "hina",
                                  "candidates": [{"asset": ref(sheet_h), "px": [60, 90], "origin": {"kind": "agent", "tool_id": "x"}}]},
                                 {"op": "import_candidates", "character_id": "sora",
                                  "candidates": [{"asset": ref(sheet_s), "px": [60, 90], "origin": {"kind": "agent", "tool_id": "x"}}]}],
             commit=True),
        human("approve_sheet", "hina", "$candidate:demo.genko:hina:0"),
        human("approve_sheet", "sora", "$candidate:demo.genko:sora:0"),
        tool("preflight", BOOK),
        tool("preflight", BOOK, allow_fixture=True, force=True),
        tool("check", BOOK),
        tool("check", BOOK, pages=[2, "1"]),
        tool("export", BOOK, format="png", pages=[1], dpi=20),
        tool("export", BOOK, format="nope"),
        tool("export", BOOK, format="png", pages=[]),
        tool("export", BOOK, format="png", pages=[3]),
        tool("export", BOOK, format="timelapse"),
        tool("export", BOOK, format="timelapse", pages=[1, 2]),
        tool("export", BOOK, format="animation", pages=[1]),
        tool("export", BOOK, format="animation"),
        tool("export", BOOK, format="sns", long_edge=120, jpeg=True),
        tool("export", BOOK, format="webtoon", width=100, max_height=300, background=True),
        tool("export_status", BOOK, f"$job:{BOOK}:1"),
        tool("export_status", BOOK, "nope"),
        tool("export_status", BOOK, "../x"),
        tool("export_proof", BOOK, format="nope"),
        tool("export_proof", BOOK, format="png"),
        tool("export_proof", BOOK, format="pdf", background=True),
        human("export", "png", "$root/out/official", dpi=20),
        human("export", "pdf", "$root/out/official", allow_fixture=True, force=True, color="rgb"),
        human("export", "gif", "$root/out/official"),
        tool("review_page", BOOK),
        {"fn": "eval_sample", "project": BOOK, "args": ["$root/eval", 2, 3]},
        write("answers.csv", "evaluator,sample,choice\r\nleaf,s01,a\r\nleaf,s02,B\r\n,s01,A\r\nmio,s01, b \r\n\r\nmio,nope,A\r\n"
                             "\"ki,m\",s02,\"A\"\r\n"),
        {"fn": "eval_score", "project": "eval", "args": ["$root/answers.csv"]},
        {"fn": "eval_score", "project": "nowhere", "args": ["$root/answers.csv"]},
    ]
    return {"name": "art", "clock": "2026-10-01T09:00:00", "env": {"GENKO_USER": "leaf"}, "steps": steps}


def ship_job() -> dict:
    """The final export (M5②c): a one-page book taken to its end (name and art approved, every panel adopted, the
    sheet approved, finished), its preflight, and a person's export to print and to screens with the approval
    recorded."""
    bible, script = fixture("bible.json"), fixture("script.json")
    for scene in script["scenes"]:
        scene["beats"] = [b for b in scene["beats"] if b.get("page", 1) <= 1]
    plan = fixture("p001.json")
    origin = {"kind": "agent", "tool_id": "local:sd", "model": "m1"}
    pictures = [drawing((70, 50), (200, 190, 180), [(3, 3, 66, 46)]), drawing((60, 80), (190, 200, 210), [(3, 76, 56, 3)]),
                drawing((50, 70), (210, 190, 200), [(25, 2, 25, 68)])]
    sheet = drawing((60, 90), (240, 230, 220), [(30, 5, 30, 85)])
    steps = [
        tool("create_project", BOOK, "ship", 1, spec_preset="doujin-a5"),
        tool("set_bible", BOOK, bible, commit=True),
        tool("set_script", BOOK, script, commit=True),
        tool("submit_name", BOOK, plan, commit=True, max_px=200),
        human("approve_name", [1]),
        *[asset(p) for p in pictures],
        asset(sheet),
        tool("apply_ops", BOOK, [{"op": "import_candidates", "page": 1, "frame_id": f"$frame:{BOOK}:1:{n}",
                                  "candidates": [{"id": f"cd_ship{n}", "asset": ref(p), "px": [70, 50],
                                                  "origin": {**origin, "kind": "fixture" if n == 2 else "agent"}}]}
                                 for n, p in enumerate(pictures)], commit=True),
        *[tool("adopt", BOOK, f"$panelcand:{BOOK}:1:{n}:0", upscale=False, preview=False) for n in range(3)],
        tool("apply_ops", BOOK, [{"op": "import_candidates", "character_id": "hina",
                                  "candidates": [{"asset": ref(sheet), "px": [60, 90], "origin": origin}]}], commit=True),
        tool("preflight", BOOK),
        human("export", "png", "$root/out/early", dpi=20, force=True, allow_fixture=True),
        human("approve_sheet", "hina", f"$candidate:{BOOK}:hina:0"),
        human("approve_art", [1]),
        tool("finish_page", BOOK, 1, commit=True),
        tool("preflight", BOOK),
        tool("preflight", BOOK, allow_fixture=True, force=True),
        human("export", "png", "$root/out/print", dpi=20),
        human("export", "png", "$root/out/print", dpi=20, force=True, allow_fixture=True),
        human("export", "tiff", "$root/out/tiff", dpi=20, force=True, allow_fixture=True, color="bitonal"),
        human("export", "pdf", "$root/out/pdf", dpi=20, force=True, allow_fixture=True, color="rgb"),
        human("export", "pdf", "$root/out/pdf2", dpi=20, allow_fixture=True, color="rgb"),
        human("export", "webtoon", "$root/out/webtoon", allow_fixture=True, width_px=90, max_height=200),
        human("export", "sns", "$root/out/sns", allow_fixture=True, long_edge=100, jpeg=False),
        human("export", "sns", "$root/out/sns-jpeg", allow_fixture=True, long_edge=100),
        human("export", "png", "$root/out/agent", dpi=20, force=True, allow_fixture=True, actor="ai:agent"),
        tool("status", BOOK),
        tool("check", BOOK, pages=[1]),
        {"fn": "audit", "project": BOOK},
    ]
    return {"name": "ship", "clock": "2026-10-01T09:00:00", "env": {"GENKO_USER": "leaf"}, "steps": steps}


def atari_job() -> dict:
    """Hand-drawn names (M5②b): scans placed on the draft layer and analysed into layout proposals, analysed again with
    other settings, lines read from the handwriting proposed, a person accepting them, the overlay; D8's evaluation."""
    page_scan = scan()
    other = scan((300, 300), ((10, 10, 290, 140), (10, 150, 290, 290)))
    steps = [
        tool("create_project", BOOK, "atari", 3, spec_preset="doujin-a5"),
        write("scans/p1.png", page_scan),
        write("scans/p2.png", other),
        write("scans/bad.png", b"nope"),
        tool("import_name", BOOK, ["scans/p1.png"], align="sideways"),
        tool("import_name", BOOK, []),
        tool("import_name", BOOK, "scans/p1.png"),
        tool("import_name", BOOK, ["/etc/hostname"]),
        tool("import_name", BOOK, ["scans/none.png"]),
        tool("import_name", BOOK, ["scans/bad.png"]),
        tool("import_name", BOOK, ["scans/p1.png"], start_page=3, align="page"),
        tool("import_name", BOOK, ["scans/p1.png", "scans/p2.png"], start_page=3),
        tool("import_name", BOOK, ["scans/p1.png", "scans/p2.png"]),
        tool("proposals", BOOK),
        tool("render", BOOK, 1, kind="atari", max_px=160),
        tool("analyze_name", BOOK, 1, {"min_gutter_mm": 1.0, "speck_mm": 2, "other": 5}),
        tool("analyze_name", BOOK, 3),
        tool("analyze_name", BOOK, 9),
        tool("propose_lines", BOOK, 1, [{"text": "こんにちは\nせかい", "x_mm": 30, "y_mm": 40},
                                        {"text": "box", "box01": [0.5, 0.5, 0.1, 0.2], "speaker": "hina"}]),
        tool("propose_lines", BOOK, 1, []),
        tool("propose_lines", BOOK, 1, [{"x_mm": 1}]),
        tool("propose_lines", BOOK, 1, ["x"]),
        tool("propose_lines", BOOK, 9, [{"text": "a"}]),
        tool("proposals", BOOK, status="all"),
        human("accept_proposal", f"$proposal:{BOOK}:0"),
        human("accept_proposal", f"$proposal:{BOOK}:0"),
        human("accept_proposal", "nope"),
        human("accept_proposal", f"$proposal:{BOOK}:2", force=True),
        human("accept_proposal", f"$proposal:{BOOK}:3"),
        tool("render", BOOK, 1, kind="atari", max_px=160),
        tool("review_page", BOOK),
        write("truth.json", {"page_mm": [148, 210], "align": "page",
                             "pages": [{"scan": "scans/p1.png", "panels": [[3, 3, 140, 66], [3, 77, 64, 125]]},
                                       {"scan": "scans/p2.png", "panels": []}]}),
        {"fn": "eval_atari", "project": "truth.json"},
        {"fn": "eval_atari", "project": "truth.json", "args": [0.5]},
    ]
    return {"name": "atari", "clock": "2026-10-01T09:00:00", "env": {"GENKO_USER": "leaf"}, "steps": steps}


def flow_job() -> dict:
    """A book written as an agent writes one (demo4), with every reading and writing tool, a person's approvals and
    their refusals to an agent, chat approvals, tickets, proposals, Undo, character sheets, stats and the audit."""
    bible, script = fixture("bible.json"), fixture("script.json")
    plans = [fixture(f"p00{n}.json") for n in range(1, 5)]
    bad_bible = copy.deepcopy(bible)
    bad_bible["characters"][0]["id"] = 7
    bad_plan = copy.deepcopy(plans[0])
    bad_plan["page"] = 2
    bad_plan["tiers"][0]["h"] = 0.9
    bible["props"] = [{"id": "pr_clip", "name": "星の髪留め", "desc": "左側に付ける", "tokens_en": None},
                      {"id": "pr_tank", "name": "給水塔", "desc": "屋上の", "tokens_en": "a water tank"}]
    prop_plan = copy.deepcopy(plans[2])
    prop_plan["panels"][0]["props"] = ["星の髪留め", "pr_tank", "知らない物"]
    sheet_a = png((60, 90), (230, 230, 230, 255), "RGB", [(30, 10, (10, 10, 10))])
    sheet_b = png((80, 100), (240, 220, 200, 255), "RGBA")
    steps = [
        tool("projects"),
        tool("create_project", BOOK, "demo", 4),
        tool("create_project", BOOK, "demo", 4),
        tool("create_project", "../outside.genko", "x", 2),
        tool("create_project", "", "x", 2),
        tool("create_project", "bad.genko", "x", 2, spec_preset="nope"),
        tool("create_project", "bad.genko", "x", 0),
        tool("create_project", "bad.genko", "x", 401),
        tool("create_project", "bad.genko", "x", 2, binding="up"),
        tool("create_project", "bad.genko", "x", "2"),
        tool("create_project", "small.genko", "小さな本", 2, spec_preset="doujin-b5", binding="left"),
        tool("projects"),
        tool("status", BOOK),
        tool("status", "nothing.genko"),
        tool("next", BOOK),
        tool("next", BOOK, limit=1),
        tool("inspect", BOOK, "ops"),
        tool("inspect", BOOK, "ops", op="add_stroke, fill,nope"),
        tool("inspect", BOOK, "ops", op="nope,approve"),
        tool("inspect", BOOK, "schemas"),
        tool("inspect", BOOK, "rules"),
        tool("inspect", BOOK, "drawing"),
        tool("inspect", BOOK, "bible"),
        tool("inspect", BOOK, "script"),
        tool("inspect", BOOK, "studio"),
        tool("inspect", BOOK, "upscalers"),
        tool("inspect", BOOK, "brushes"),
        tool("inspect", BOOK, "page"),
        tool("inspect", BOOK, "page", page=9),
        tool("inspect", BOOK, "nothing"),
        tool("upscalers"),
        tool("set_bible", BOOK, bible),
        tool("set_bible", BOOK, bad_bible),
        tool("set_bible", BOOK, {"title": 5}),
        tool("set_script", BOOK, script),
        tool("set_bible", BOOK, bible, commit=True),
        tool("inspect", BOOK, "bible"),
        tool("next", BOOK),
        tool("set_script", BOOK, {"scenes": "x"}),
        tool("set_script", BOOK, script),
        tool("submit_name", BOOK, plans[0]),
        tool("set_script", BOOK, script, commit=True),
        tool("submit_name", BOOK, plans[0]),
        tool("submit_name", BOOK, plans[0], commit=True),
        tool("submit_name", BOOK, plans[0], commit=True),
        tool("submit_name", BOOK, plans[0], commit=True, replace=True, max_px=400),
        tool("submit_name", BOOK, bad_plan, commit=True),
        tool("submit_name", BOOK, plans[1], commit=True),
        tool("submit_name", BOOK, prop_plan, commit=True),
        tool("submit_name", BOOK, plans[3], commit=True),
        tool("status", BOOK),
        tool("inspect", BOOK, "page", page=1),
        tool("inspect", BOOK, "page", page=2),
        tool("inspect", BOOK, "panel", page=1),
        tool("inspect", BOOK, "panel", page=3, full=True),
        tool("inspect", BOOK, "panel", page=1, frame_id="$frame:demo.genko:1:1"),
        tool("inspect", BOOK, "panel", page=1, frame_id="nope"),
        tool("inspect", BOOK, "snapshot", page=2),
        tool("next", BOOK, limit=10),
        tool("record_review", BOOK, 9, 0.5, "x"),
        *[tool("record_review", BOOK, n, 0.8, "読み順よし") for n in range(1, 5)],
        tool("next", BOOK),
        tool("request_approval", BOOK, "name", [1, 2, 3, 4], "確認お願いします"),
        tool("request_approval", BOOK, "nope", [1]),
        tool("request_approval", BOOK, "sheet", []),
        tool("tickets", BOOK),
        human("approve_name", [1, 2, 3, 4], actor="ai:agent"),
        human("approve_name", [3, 1, 2, 4]),
        tool("status", BOOK),
        tool("next", BOOK, limit=50),
        tool("tickets", BOOK, status="all"),
        tool("apply_ops", BOOK, "x"),
        tool("apply_ops", BOOK, [{"op": "approve", "gate": "name", "page": 1}]),
        tool("apply_ops", BOOK, [{"no": 1}]),
        tool("apply_ops", BOOK, [{"op": "set_note", "page": 2, "note": "夕日を強く"}]),
        tool("apply_ops", BOOK, [{"op": "set_note", "page": 2, "note": "夕日を強く"}], commit=True),
        tool("apply_ops", BOOK, [{"op": "set_note", "page": 99, "note": "x"}], commit=True),
        tool("apply_ops", BOOK, [{"op": "import_psd", "path": "/etc/passwd"}]),
        tool("apply_ops", BOOK, [{"op": "import_pages", "from": "../.."}]),
        tool("request_fix", BOOK, 2, "背景をもっと暗く", scope="page"),
        tool("ask_human", BOOK, "ソラの髪は白でいい？", page=1, item="sheet"),
        tool("tickets", BOOK),
        tool("resolve_ticket", BOOK, "$ticket:demo.genko:-2", "暗くした"),
        tool("resolve_ticket", BOOK, "nope", "x"),
        human("comment", 1, "コマ割りを見直して"),
        human("comment", 1, "この顔を直して", frame_id="$frame:demo.genko:1:0"),
        human("close_ticket", "$ticket:demo.genko:-3", reply="白でいい。そのまま進めて"),
        human("close_ticket", "nope"),
        human("reopen_ticket", "$ticket:demo.genko:-4", note="まだ暗い"),
        human("reopen_ticket", "nope"),
        tool("record_chat_approval", BOOK, "export", "OK"),
        tool("record_chat_approval", BOOK, "art", "  "),
        tool("record_chat_approval", BOOK, "art", "OK"),
        human("chat_approval", False),
        tool("record_chat_approval", BOOK, "name", "OK", pages=[1]),
        human("chat_approval", True, actor="human:作者"),
        human("revoke", "name", [2], reason="台詞を変える"),
        tool("record_chat_approval", BOOK, "name", "2ページ目OKです", pages=[2], person="leaf"),
        tool("record_chat_approval", BOOK, "name", "OK", pages=["x"]),
        tool("apply_ops", BOOK, [{"op": "propose", "proposal": {"id": "pr_lines1", "kind": "lines", "page": 3,
                                                               "lines": [{"text": "あ", "x_mm": 40, "y_mm": 60}]}}], commit=True),
        tool("proposals", BOOK),
        tool("proposals", BOOK, status="all"),
        human("reject_proposal", "pr_lines1", note="手書きを読み直して"),
        human("reject_proposal", "pr_lines1", actor="ai:agent"),
        tool("proposals", BOOK, status="rejected"),
        asset(sheet_a),
        asset(sheet_b),
        tool("apply_ops", BOOK, [{"op": "import_candidates", "character_id": "hina",
                                  "candidates": [{"asset": ref(sheet_a), "px": [60, 90], "origin": {"kind": "agent"},
                                                  "face_box01": [0.25, 0.05, 0.5, 0.3]},
                                                 {"asset": ref(sheet_b), "px": [80, 100], "origin": {"kind": "agent"}}]}], commit=True),
        tool("request_approval", BOOK, "sheet", [], character_id="hina", note="設定画を見て"),
        tool("request_approval", BOOK, "sheet", [], character_ids=["sora", "hina", "sora"]),
        human("approve_sheet", "hina", "$candidate:demo.genko:hina:1"),
        human("approve_sheet", "hina", "$candidate:demo.genko:hina:0", face_box01=[0.1, 0.1, 0.5, 0.4]),
        tool("record_chat_approval", BOOK, "sheet", "この顔でOK", character_id="hina"),
        tool("record_chat_approval", BOOK, "sheet", "この顔でOK", character_id="hina", candidate_id="$candidate:demo.genko:hina:1",
             person="human:leaf"),
        human("revoke", "sheet", [], character_id="hina", reason="髪型を変える"),
        human("revoke", "art", [1]),
        tool("set_bible", BOOK, bible, commit=True),
        tool("status", BOOK),
        tool("next", BOOK, limit=50, claim=True),
        tool("next", BOOK, limit=50, actor="ai:other"),
        {"fn": "stats", "project": BOOK},
        {"fn": "audit", "project": BOOK},
        {"fn": "audit", "project": "small.genko"},
        # (Undo last: v4 keeps the revision counting up through an Undo, where Python's goes back, so what a book records
        # of its revision afterwards differs by design — schema-v4.md)
        tool("undo", BOOK),
        tool("apply_ops", BOOK, [{"op": "set_note", "page": 3, "note": "取り消すメモ"}], commit=True),
        tool("undo", BOOK, actor="ai:other"),
        tool("undo", BOOK),
        tool("undo", BOOK),
        tool("status", BOOK),
    ]
    return {"name": "flow", "clock": "2026-05-01T10:00:00", "env": {"GENKO_USER": "leaf"}, "steps": steps}


def style_job() -> dict:
    """The manga style catalog read from a folder (GENKO_STYLE_CATALOG=file://…): genres, a branch, a search, the book's
    style and whether the catalog has a newer one; keeping a branch (its sample as an asset) and going back."""
    sample = png((24, 16), (90, 120, 200, 255), "RGB")
    nodes = [
        {"id": "gag", "parent": None, "level": 1, "title": "ギャグ漫画", "summary": "笑い"},
        {"id": "gag-surreal", "parent": "gag", "level": 2, "title": "シュール", "summary": "不条理"},
        {"id": "gag-surreal-doodle", "parent": "gag-surreal", "level": 3, "title": "ゆるい落書き風", "summary": "線が震える"},
        {"id": "shojo", "parent": None, "level": 1, "title": "少女漫画", "summary": "恋"},
        {"id": "shojo-doodle", "parent": "shojo", "level": 2, "title": "ゆるい落書き風", "summary": "同じ名前"},
        {"title": "id の無いもの"},
    ]
    style = {"id": "gag-surreal-doodle", "title": "ゆるい落書き風", "summary": "線が震える", "level": 3, "expression": "mono",
             "path": [{"id": "gag", "title": "ギャグ漫画"}, {"id": "gag-surreal", "title": "シュール"}],
             "prompt": {"ja": "震える細い線の落書き風", "en": "shaky doodle lines", "tags": "doodle"},
             "avoid": ["写実的な陰影", "色"], "samples": [{"url": "file://$root/catalog/sample.png"}], "reference": 0,
             "version": 3, "updated_at": "2026-04-01", "license": {"terms": "見本は参考用"},
             "children": [{"id": "c1", "title": "子", "summary": "s", "x": 1}, "not a dict"]}
    colour = {**style, "id": "shojo-doodle", "title": "ゆるい落書き風", "expression": "color", "path": ["少女漫画"], "samples": [],
              "version": 1}
    newer = {**style, "version": 4}
    steps = [
        tool("create_project", BOOK, "style", 2),
        tool("style_catalog"),
        write("catalog/v1/tree", {"nodes": nodes}),
        write("catalog/v1/styles/gag-surreal-doodle", style),
        write("catalog/v1/styles/shojo-doodle", colour),
        write("catalog/v1/styles/no-words", {"id": "no-words", "prompt": {"en": "x"}}),
        write("catalog/v1/styles/broken", "{not json"),
        write("catalog/sample.png", sample),
        tool("style_catalog"),
        tool("style_catalog", title="落書き"),
        tool("style_catalog", title="シュール"),
        tool("style_catalog", style_id="gag-surreal-doodle"),
        tool("style_catalog", style_id="genko://use-style?id=shojo-doodle"),
        tool("style_catalog", style_id="NOT AN ID"),
        tool("style_catalog", style_id="missing-style"),
        tool("style_catalog", style_id="no-words"),
        tool("style_catalog", style_id="broken"),
        tool("style_catalog", style_id="https://example.com/n/x"),
        tool("style_catalog", project=BOOK),
        tool("use_style", BOOK, "gag-surreal-doodle"),
        tool("use_style", BOOK, "gag-surreal-doodle", commit=True),
        tool("status", BOOK),
        tool("style_catalog", project=BOOK),
        write("catalog/v1/styles/gag-surreal-doodle", newer),
        tool("style_catalog", project=BOOK),
        tool("use_style", BOOK, "shojo-doodle", commit=True),
        tool("use_style", BOOK, None, commit=True),
        tool("style_catalog", project=BOOK),
        tool("use_style", BOOK, "missing-style", commit=True),
        tool("apply_ops", BOOK, [{"op": "set_studio", "style": {"locked": True}}], commit=True, actor="human:leaf"),
        tool("use_style", BOOK, "gag-surreal-doodle", commit=True),
    ]
    return {"name": "style", "clock": "2026-05-01T10:00:00", "env": {"GENKO_STYLE_CATALOG": "file://$root/catalog"}, "steps": steps}


def drafts_job() -> dict:
    """M0's sidecar drafts moved into a book (adopt-drafts)."""
    bible, script, plan = fixture("bible.json"), fixture("script.json"), fixture("p001.json")
    steps = [
        tool("create_project", BOOK, "drafts", 3),
        {"fn": "adopt_drafts", "project": BOOK},
        write(f"{BOOK}/studio/drafts/bible.json", bible),
        write(f"{BOOK}/studio/drafts/script.json", script),
        write(f"{BOOK}/studio/drafts/name/p001.json", {"plan": plan, "slot_to_frame": {"p1": "$frame:demo.genko:1:0"},
                                                       "input_hash": "sha256:x", "reading_order": ["p1"], "by": "ai:m0", "seq": 3}),
        write(f"{BOOK}/studio/drafts/reviews.json", {"1": {"score": 0.7, "notes": "M0", "input_hash": "sha256:x"}}),
        write(f"{BOOK}/studio/drafts/requests.json", [
            {"kind": "approval", "status": "open", "gate": "name", "pages": [1], "note": "見て", "by": "ai:m0"},
            {"kind": "comment", "status": "open", "page": 1, "text": "古いコメント", "seq": 1, "by": "human:leaf"},
            {"kind": "comment", "status": "open", "page": 2, "text": "2ページ目", "seq": 5},
            {"kind": "comment", "status": "done", "page": 2, "text": "済み"},
            {"kind": "other", "status": "open"},
        ]),
        {"fn": "adopt_drafts", "project": BOOK},
        tool("status", BOOK),
        tool("tickets", BOOK),
        {"fn": "adopt_drafts", "project": BOOK},
        tool("next", BOOK, limit=3),
        tool("inspect", BOOK, "page", page=1),
        tool("proposals", BOOK, status="all"),
        {"fn": "stats", "project": BOOK},
        {"fn": "audit", "project": BOOK},
    ]
    return {"name": "drafts", "clock": "2026-05-01T10:00:00", "env": {}, "steps": steps}


def cli_job() -> dict:
    """`genko studio …` itself: its arguments, its replies and exit codes (the ids it makes are not compared: the command
    line makes random ones)."""
    book = f"$root/{BOOK}"
    steps = [
        write("in/bible.json", fixture("bible.json")),
        write("in/script.json", fixture("script.json")),
        write("in/p001.json", fixture("p001.json")),
        write("in/ops.json", {"ops": [{"op": "set_note", "page": 1, "note": "メモ"}]}),
        write("in/bad.json", "{oops"),
        write("in/list.json", [1, 2]),
        write("in/call_status.json", {}),
        write("in/call_inspect.json", {"target": "page", "page": 1}),
        write("in/call_bad.json", {"nope": 1}),
        write("in/tool.json", {"label": "手元の道具", "sizes_px": [[512, 768]], "supports": {"edit": False}}),
        {"cli": []},
        {"cli": ["nothing"]},
        {"cli": ["init", book, "--title", "CLI の本", "--pages", "3"]},
        {"cli": ["init", book]},
        {"cli": ["init", f"$root/x.genko", "--pages", "many"]},
        {"cli": ["init", f"$root/x.genko", "--spec", "doujin-b5"]},
        {"cli": ["status", book]},
        {"cli": ["status", book, "--session", "bad session!"]},
        {"cli": ["status", f"$root/none.genko"]},
        {"cli": ["next", book, "--limit", "2"]},
        {"cli": ["next", book, "--lim", "1"]},
        {"cli": ["inspect", book, "page", "--page", "1"]},
        {"cli": ["inspect", book, "rules"]},
        {"cli": ["set-bible", book, "$root/in/bible.json"]},
        {"cli": ["set-bible", book, "$root/in/bible.json", "--commit", "--agent", "ai:hermes", "--session", "9204"]},
        {"cli": ["set-script", book, "$root/in/missing.json", "--commit"]},
        {"cli": ["set-script", book, "$root/in/bad.json", "--commit"]},
        {"cli": ["set-script", book, "-", "--commit"], "stdin": json.dumps(fixture("script.json"), ensure_ascii=False)},
        {"cli": ["submit-name", book, "$root/in/p001.json", "--commit"]},
        {"cli": ["submit-name", book, "$root/in/p001.json", "--commit", "--replace"]},
        {"cli": ["record-review", book, "--page", "1", "--score", "0.9", "--notes", "よし"]},
        {"cli": ["record-review", book, "--page", "x"]},
        {"cli": ["record-review", book]},
        {"cli": ["apply", book, "$root/in/ops.json"]},
        {"cli": ["apply", book, "$root/in/ops.json", "--commit"]},
        {"cli": ["apply", book, "$root/in/list.json"]},
        {"cli": ["request-approval", book, "--pages", "1-2,3", "--note", "見て"]},
        {"cli": ["request-approval", book, "--pages", "x"]},
        {"cli": ["request-approval", book, "--gate", "export"]},
        {"cli": ["tickets", book]},
        {"cli": ["tickets", book, "--all"]},
        {"cli": ["approve", book, "name", "--pages", "1"]},
        {"cli": ["approve", book, "name", "--pages", "1", "--as", "ai:agent"]},
        {"cli": ["approve", book, "name", "--pages", "1", "--as", "human:leaf"]},
        {"cli": ["approve", book, "art"]},
        {"cli": ["approve", book, "sheet", "--character", "hina", "--as", "human:leaf"]},
        {"cli": ["approve", book, "sheet", "--character", "hina", "--candidate", "c1", "--face", "a,b", "--as", "human:leaf"]},
        {"cli": ["revoke", book, "name", "--pages", "1", "--reason", "やり直し", "--as", "human:leaf"]},
        {"cli": ["comment", book, "直して", "--page", "1", "--as", "human:leaf"]},
        {"cli": ["comment", book, "直して", "--page", "9", "--as", "human:leaf"]},
        {"cli": ["close-ticket", book, "nope", "--as", "human:leaf"]},
        {"cli": ["reopen-ticket", book, "nope", "--as", "human:leaf"]},
        {"cli": ["chat-approval", book, "off", "--as", "human:leaf"]},
        {"cli": ["chat-approval", book, "maybe"]},
        {"cli": ["reject", book, "nope", "--as", "human:leaf"]},
        {"cli": ["proposals", book]},
        {"cli": ["proposals", book, "--all"]},
        {"cli": ["call", book, "status"]},
        {"cli": ["call", book, "status", "$root/in/call_status.json"]},
        {"cli": ["call", book, "inspect", "$root/in/call_inspect.json"]},
        {"cli": ["call", book, "inspect", "$root/in/call_bad.json"]},
        {"cli": ["call", book, "inspect", "$root/in/list.json"]},
        {"cli": ["call", book, "projects"]},
        {"cli": ["call", book, "undo"]},
        {"cli": ["stats", book]},
        {"cli": ["audit", book]},
        {"cli": ["adopt-drafts", book]},
        {"cli": ["tools", "list"]},
        {"cli": ["tools", "example"]},
        {"cli": ["tools", "set"]},
        {"cli": ["tools", "set", "openai:gpt-image-1"]},
        {"cli": ["tools", "set", "local:sd", "--file", "$root/in/tool.json"]},
        {"cli": ["tools", "set", "other", "--file", "$root/in/list.json"]},
        {"cli": ["tools", "list"]},
        {"cli": ["tools", "remove", "local:sd"]},
        {"cli": ["tools", "remove", "local:sd"]},
        {"cli": ["upscaler", "list"]},
        {"cli": ["upscaler", "add"]},
        {"cli": ["upscaler", "add", "esr"]},
        {"cli": ["upscaler", "add", "genko", "--command", "x {in} {out}"]},
        {"cli": ["upscaler", "add", "bad name", "--command", "x {in} {out}"]},
        {"cli": ["upscaler", "add", "esr", "--command", "esr -i {in}"]},
        {"cli": ["upscaler", "add", "esr", "--command", "esr 'unclosed {in} {out}"]},
        {"cli": ["upscaler", "add", "esr", "--command", "realesrgan -i {in} -o \"{out}\" -s {scale}", "--scales", "2,4",
                 "--label", "Real-ESRGAN"]},
        {"cli": ["upscaler", "add", "w2x", "--command", "waifu2x {in} {out}", "--scales", "x"]},
        {"cli": ["upscaler", "list"]},
        {"cli": ["upscaler", "remove", "esr"]},
        {"cli": ["upscaler", "remove", "esr"]},
        {"cli": ["call", book, "inspect", "$root/in/call_status.json"]},
        write("in/scan.png", scan()),
        write("in/pic.png", drawing((40, 30), (200, 100, 50), [(0, 0, 39, 29)])),
        write("in/truth.json", {"align": "auto", "pages": [{"scan": "scan.png", "panels": [[10, 10, 100, 50]]}]}),
        {"cli": ["render", book, "--page", "1", "--max-px", "128"]},
        {"cli": ["render", book, "--page", "1", "--mode", "proof", "--max-px", "128", "--frame", "nope"]},
        {"cli": ["render", book, "--page", "9"]},
        {"cli": ["render", book]},
        {"cli": ["import-image", book, "$root/in/pic.png"]},
        {"cli": ["import-image", book, "$root/in/none.png"]},
        {"cli": ["import-name", book, "$root/in/scan.png", "--start-page", "2", "--as", "human:leaf"]},
        {"cli": ["import-name", book, "$root/in/scan.png", "--align", "x"]},
        {"cli": ["import-name", book, "$root/in/scan.png", "--start-page", "7"]},
        {"cli": ["analyze", book, "--page", "2", "--params", "{\"min_gutter_mm\": 1}"]},
        {"cli": ["analyze", book, "--page", "1"]},
        {"cli": ["analyze", book, "--page", "2", "--params", "{bad"]},
        {"cli": ["proposals", book]},
        {"cli": ["accept", book, "nope", "--as", "human:leaf"]},
        {"cli": ["export", book, "--out", "$root/out", "--format", "png", "--as", "human:leaf"]},
        {"cli": ["export", book, "--out", "$root/out", "--format", "gif"]},
        {"cli": ["review", book, "--out", "$root/review/r.html"]},
        {"cli": ["eval-atari", "$root/in/truth.json"]},
        {"cli": ["eval-atari", "$root/in/none.json"]},
        {"cli": ["eval-sample", book, "--out", "$root/eval"]},
        {"cli": ["eval-score", "$root/eval", "$root/in/none.csv"]},
    ]
    return {"name": "cli", "clock": "2026-05-01T10:00:00", "env": {"GENKO_USER": "leaf"}, "steps": steps, "cli": True}


def main() -> int:
    jobs = [flow_job(), style_job(), drafts_job(), art_job(), ship_job(), atari_job(), cli_job()]
    OUT.write_text(json.dumps({"jobs": jobs}, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{OUT}: {sum(len(j['steps']) for j in jobs)} steps in {len(jobs)} jobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
