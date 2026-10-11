"""Usage: make_studio_cases.py OUT

Writes the case file of test_contract_studio's studioOpsLikePython (native/tests/contract/studio_cases.json): each case
its ops and agent on the book `studio_harness.py make-studiobook` makes, whether Python's apply_ops takes it, and for
what this build refuses on purpose ("cpp": its code and words). Development only."""
import json
import sys

L1A, L1B, L1C = "000000000028", "000000000029", "000000000026"
P1_ROOT, P1_NODE = "000000000006", "000000000025"
P3A, P3B = "000000000033", "000000000034"
P4 = "000000000018"
P5 = "00000000001e"
P6A, P6B, P6C = "000000000045", "000000000046", "000000000044"
P7 = "00000000004e"
HUMAN, AI, OTHER = "human:作者", "ai:hermes", "ai:other"
T_FIX_HUMAN, T_GATE_NAME, T_GATE_SHEET, T_HELP, T_FIX_AGENT, T_DONE = (
    "000000000a0b", "000000000a0c", "000000000a0d", "000000000a0e", "000000000a10", "000000000a11")
NOASSET = "sha256:" + "0" * 64
# (a picture whose pixels cannot be decoded, or no picture: placed or adopted by Python, at most its size read from the
# header, and the drawing fails on it; refused here — SPEC COMP-01a — the whole picture decoded before it is placed)
CANNOT_READ = "the asset's picture cannot be read ("

cases = []


def case(n, op, ok, ops=None, agent=None, steps=None, dry=None, **extra):
    c = {"n": n, "op": op, "ok": ok}
    if steps is not None:
        c["steps"] = steps
    else:
        c["ops"] = ops
        if agent is not None:
            c["agent"] = agent
        if dry is not None:
            c["dry"] = dry
    c.update(extra)
    cases.append(c)


def step(ops, agent=None, dry=None):
    s = {"ops": ops}
    if agent is not None:
        s["agent"] = agent
    if dry is not None:
        s["dry_run"] = dry
    return s


# --- set_studio ---------------------------------------------------------------------------------------------------------
case("set_studio bible_doc by an agent", "set_studio", True, [{"op": "set_studio", "bible_doc": {"title": "新しい題"}}], AI)
case("set_studio style merged", "set_studio", True, [{"op": "set_studio", "style": {"notes": ["新しい"], "extra": 1}}], AI)
case("set_studio style replaced by a text", "set_studio", True, [{"op": "set_studio", "style": "plain"}], AI,
     cpp="apply", cpp_says="only a person can change the style fixed by the pilot page")
case("set_studio style replaced by a text by a person", "set_studio", True, [{"op": "set_studio", "style": "plain"}], HUMAN)
case("set_studio policy by an agent", "set_studio", False, [{"op": "set_studio", "policy": {"auto_adopt": True}}], AI)
case("set_studio policy null by an agent", "set_studio", False, [{"op": "set_studio", "policy": None, "premise": "x"}], AI)
case("set_studio policy merged by a person", "set_studio", True, [{"op": "set_studio", "policy": {"max_images": 4}}], HUMAN)
case("set_studio policy replaced by a list", "set_studio", True, [{"op": "set_studio", "policy": [1, 2]}], "genko")
case("set_studio premise null", "set_studio", True, [{"op": "set_studio", "premise": None}], AI)
case("set_studio nothing", "set_studio", True, [{"op": "set_studio"}], AI)
case("set_studio unknown keys", "set_studio", True, [{"op": "set_studio", "title": "x", "premise": "p"}], AI)
case("set_studio on a style that is not an object", "set_studio", True,
     steps=[step([{"op": "set_studio", "style": "plain"}], HUMAN), step([{"op": "set_studio", "style": {"notes": []}}], AI)])

# --- set_style_catalog ---------------------------------------------------------------------------------------------------
CAT = {"id": "gag-yuru", "title": "ゆるい落書き風", "path": ["ギャグ漫画", "シュール"], "prompt": {"ja": "ゆるい線", "en": "loose lines", "tags": "a,b"},
       "avoid": ["写実"], "sample": "$A1", "version": 3, "updated_at": "2026-09-01", "extra_key": "dropped"}
case("set_style_catalog by an agent while the style is fixed", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": CAT}], AI)
case("set_style_catalog by a person: the fixed style goes stale", "set_style_catalog", True, [{"op": "set_style_catalog", "catalog": CAT}], HUMAN)
case("set_style_catalog null by a person", "set_style_catalog", True, [{"op": "set_style_catalog", "catalog": None}], HUMAN)
case("set_style_catalog bad id", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {**CAT, "id": "Bad_ID"}}], HUMAN)
case("set_style_catalog id with spaces", "set_style_catalog", True, [{"op": "set_style_catalog", "catalog": {**CAT, "id": "  gag-yuru  "}}], HUMAN)
case("set_style_catalog id a number", "set_style_catalog", True, [{"op": "set_style_catalog", "catalog": {**CAT, "id": 5, "sample": None}}], HUMAN)
case("set_style_catalog id starting with -", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {**CAT, "id": "-x"}}], HUMAN)
case("set_style_catalog id too long", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {**CAT, "id": "a" * 122}}], HUMAN)
case("set_style_catalog id of 121", "set_style_catalog", True, [{"op": "set_style_catalog", "catalog": {**CAT, "id": "a" * 121}}], HUMAN)
case("set_style_catalog no prompt", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {"id": "x"}}], HUMAN)
case("set_style_catalog blank prompt", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {"id": "x", "prompt": {"ja": "  "}}}], HUMAN)
case("set_style_catalog prompt not an object", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {"id": "x", "prompt": "words"}}], HUMAN)
case("set_style_catalog sample not an asset", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {**CAT, "sample": "pic.png"}}], HUMAN)
case("set_style_catalog sample missing", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": {**CAT, "sample": NOASSET}}], HUMAN)
case("set_style_catalog not an object", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": "gag-yuru"}], HUMAN)
case("set_style_catalog a list", "set_style_catalog", False, [{"op": "set_style_catalog", "catalog": [CAT]}], HUMAN)
case("set_style_catalog by an agent after the style is opened", "set_style_catalog", True,
     steps=[step([{"op": "revoke", "gate": "art", "page": 6, "reason": "やり直し"}], HUMAN),
            step([{"op": "set_style_catalog", "catalog": CAT}], AI),
            step([{"op": "set_style_catalog", "catalog": {**CAT, "title": "同じ id"}}], AI)])
case("set_style_catalog the same catalog again", "set_style_catalog", True,
     steps=[step([{"op": "set_style_catalog", "catalog": CAT}], HUMAN), step([{"op": "set_style_catalog", "catalog": CAT}], HUMAN)])

# --- characters, locations, props, script, plans ----------------------------------------------------------------------
case("upsert_character new", "upsert_character", True, [{"op": "upsert_character", "character": {"id": "c-new", "name": "新"}}], AI)
case("upsert_character replaces", "upsert_character", True, [{"op": "upsert_character", "character": {"id": "c-yuki", "name": "雪"}}], AI)
case("upsert_character locked by an agent", "upsert_character", False, [{"op": "upsert_character", "character": {"id": "c-ken", "name": "x"}}], AI)
case("upsert_character locked unlock by an agent", "upsert_character", False, [{"op": "upsert_character", "character": {"id": "c-ken"}, "unlock": True}], AI)
case("upsert_character locked by a person without unlock", "upsert_character", False, [{"op": "upsert_character", "character": {"id": "c-ken"}}], HUMAN)
case("upsert_character unlocked by a person", "upsert_character", True, [{"op": "upsert_character", "character": {"id": "c-ken", "name": "健"}, "unlock": True}], HUMAN)
case("upsert_character kept locked by a person", "upsert_character", True, [{"op": "upsert_character", "character": {"id": "c-ken", "locked": True}, "unlock": 1}], "genko")
case("upsert_character without id", "upsert_character", False, [{"op": "upsert_character", "character": {"name": "x"}}], AI)
case("upsert_character null", "upsert_character", False, [{"op": "upsert_character", "character": None}], AI)
case("upsert_character empty list", "upsert_character", False, [{"op": "upsert_character", "character": []}], AI)
case("upsert_character a list", "upsert_character", False, [{"op": "upsert_character", "character": [["id", "x"]]}], AI)
case("upsert_character number id", "upsert_character", True, [{"op": "upsert_character", "character": {"id": 7}}], AI)
case("delete_character unused", "delete_character", True, [{"op": "delete_character", "id": "c-mio"}], AI)
case("delete_character used in a panel", "delete_character", False, [{"op": "delete_character", "id": "c-yuki"}], AI)
case("delete_character used, force", "delete_character", True, [{"op": "delete_character", "id": "c-yuki", "force": True}], AI)
case("delete_character unknown", "delete_character", True, [{"op": "delete_character", "id": "nobody"}], AI)
case("delete_character no id", "delete_character", True, [{"op": "delete_character"}], AI)
case("delete_character used by a locked character", "delete_character", False, [{"op": "delete_character", "id": "c-ken"}], AI)
case("upsert_location new", "upsert_location", True, [{"op": "upsert_location", "location": {"id": "loc-park", "name": "公園"}}], AI)
case("upsert_location replaces", "upsert_location", True, [{"op": "upsert_location", "location": {"id": "loc-roof"}}], AI)
case("upsert_location no id", "upsert_location", False, [{"op": "upsert_location", "location": {"name": "x"}}], AI)
case("upsert_location a text", "upsert_location", False, [{"op": "upsert_location", "location": "loc-x"}], AI)
case("upsert_location none", "upsert_location", False, [{"op": "upsert_location"}], AI)
case("delete_location", "delete_location", True, [{"op": "delete_location", "id": "loc-room"}], AI)
case("delete_location unknown", "delete_location", True, [{"op": "delete_location", "id": "x"}], AI)
case("delete_location named with a locked page", "delete_location", False, [{"op": "delete_location", "id": "loc-room", "page": 5}], AI)
case("delete_prop named with a locked page", "delete_prop", False, [{"op": "delete_prop", "id": "prop-bag", "page": 5}], AI)
case("set_script named with a locked page", "set_script", False, [{"op": "set_script", "script": {}, "page": 5}], AI)
case("upsert_prop new", "upsert_prop", True, [{"op": "upsert_prop", "prop": {"id": "prop-pen", "name": "ペン"}}], AI)
case("upsert_prop replaces", "upsert_prop", True, [{"op": "upsert_prop", "prop": {"id": "prop-bag", "name": "かばん"}}], AI)
case("upsert_prop no id", "upsert_prop", False, [{"op": "upsert_prop", "prop": {"id": ""}}], AI)
case("delete_prop", "delete_prop", True, [{"op": "delete_prop", "id": "prop-bag"}], AI)
case("delete_prop on a fresh studio key", "delete_prop", True,
     steps=[step([{"op": "delete_prop", "id": "prop-bag"}], AI), step([{"op": "delete_prop", "id": "prop-bag"}], AI)])
case("delete_location number id", "delete_location", True, [{"op": "delete_location", "id": 5}], AI)
case("set_script", "set_script", True, [{"op": "set_script", "script": {"scenes": []}}], AI)
case("set_script a list", "set_script", False, [{"op": "set_script", "script": []}], AI)
case("set_script missing", "set_script", False, [{"op": "set_script"}], AI)
case("set_page_plan keeps the reviews", "set_page_plan", True, [{"op": "set_page_plan", "page": 2, "plan": {"input_hash": "h2", "rev": 3}}], AI)
case("set_page_plan new", "set_page_plan", True, [{"op": "set_page_plan", "page": 3, "plan": {"name": {"page": 3}}}], AI)
case("set_page_plan null", "set_page_plan", True, [{"op": "set_page_plan", "page": 4, "plan": None}], AI)
case("set_page_plan a list", "set_page_plan", False, [{"op": "set_page_plan", "page": 2, "plan": [1]}], AI)
case("set_page_plan by page_id", "set_page_plan", True, [{"op": "set_page_plan", "page_id": "pg_00000000000d", "plan": {"a": 1}}], AI)
case("set_page_plan unknown page_id", "set_page_plan", False, [{"op": "set_page_plan", "page_id": "pg_x", "plan": {}}], AI)
case("set_page_plan page_id a number", "set_page_plan", False, [{"op": "set_page_plan", "page_id": 3, "plan": {}}], AI)
case("set_page_plan no page", "set_page_plan", False, [{"op": "set_page_plan", "plan": {}}], AI)
case("set_page_plan page text", "set_page_plan", True, [{"op": "set_page_plan", "page": "3", "plan": {"b": 2}}], AI)
case("set_page_plan page float", "set_page_plan", True, [{"op": "set_page_plan", "page": 3.9, "plan": {"b": 2}}], AI)
case("set_page_plan page word", "set_page_plan", False, [{"op": "set_page_plan", "page": "three", "plan": {}}], AI)
case("set_page_plan page 99", "set_page_plan", False, [{"op": "set_page_plan", "page": 99, "plan": {}}], AI)
case("set_page_plan page past 64 bits", "set_page_plan", False, [{"op": "set_page_plan", "page": 2 ** 70, "plan": {}}], AI)
case("set_page_plan page locked", "set_page_plan", False, [{"op": "set_page_plan", "page": 5, "plan": {}}], AI)
case("set_page_plan page locked by its owner", "set_page_plan", True, [{"op": "set_page_plan", "page": 5, "plan": {"x": 1}}], OTHER)

# --- set_panel ----------------------------------------------------------------------------------------------------------
case("set_panel pinned field by an agent", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1A, "set": {"shot": "CU"}}], AI)
case("set_panel field by an agent", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "set": {"action": "走る", "emotion": "焦り"}}], AI)
case("set_panel pinned field by a person", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "set": {"shot": "CU"}}], HUMAN)
case("set_panel prompt override by an agent", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"gen": {"size_override": [1024, 1024]}}}], AI)
case("set_panel prompt override by a person", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"gen": {"size_override": [1024, 1024]}}}], HUMAN)
case("set_panel gen without override", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"gen": {"prompt_override": None, "seed": 3}}}], AI)
case("set_panel status bad", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"status": "done"}}], AI)
case("set_panel status skip by an agent", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"status": "skip"}}], AI)
case("set_panel status skip by a person", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"status": "skip"}}], HUMAN)
case("set_panel candidates", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"candidates": []}}], HUMAN)
case("set_panel adopted", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"adopted": {}}}], HUMAN)
case("set_panel unset pinned by an agent", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unset": ["shot"]}], AI)
case("set_panel unset by an agent", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unset": ["action", "nothing"]}], AI)
case("set_panel unset pinned by a person", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unset": ["shot", "gen"]}], HUMAN)
case("set_panel unset a text", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unset": "action"}], AI)
case("set_panel pin by an agent", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "pin": ["time", "angle"]}], AI)
case("set_panel unpin by an agent", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unpin": ["shot"]}], AI)
case("set_panel unpin by a person", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1A, "unpin": ["shot", "zz"]}], HUMAN)
case("set_panel pin a list", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "pin": [["x"]]}], AI)
case("set_panel pin a number", "set_panel", True, [{"op": "set_panel", "page": 4, "frame_id": P4, "pin": [5, 2.5, True]}], AI)
case("set_panel pin words", "set_panel", True, [{"op": "set_panel", "page": 4, "frame_id": P4, "pin": ["b", "a", "ゆ", "B", "a"]}], AI)
case("set_panel new panel briefed", "set_panel", True, [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"shot": "ELS"}}], AI)
case("set_panel new panel stays empty", "set_panel", True, [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"angle": "high"}}], AI)
case("set_panel characters with a tag", "set_panel", True,
     [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"characters": [{"id": "c-yuki", "tag": "名札"}, "c-ken"]}}], AI)
case("set_panel no frame", "set_panel", False, [{"op": "set_panel", "page": 1, "set": {}}], AI)
case("set_panel unknown frame", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": "nope", "set": {}}], AI)
case("set_panel frame a number", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": 28, "set": {}}], AI)
case("set_panel not a leaf", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": P1_NODE, "set": {}}], AI)
case("set_panel root not a leaf", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": P1_ROOT, "set": {}}], AI)
case("set_panel set a list", "set_panel", False, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": [["shot", "CU"]]}], AI)
case("set_panel set null", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": None}], AI)
case("set_panel locked page", "set_panel", False, [{"op": "set_panel", "page": 5, "frame_id": P5, "set": {"shot": "CU"}}], AI)
case("set_panel locked page by its owner", "set_panel", True, [{"op": "set_panel", "page": 5, "frame_id": P5, "set": {"shot": "CU"}}], OTHER)
case("set_panel regions by a person count in the brief", "set_panel", True,
     [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"regions": [{"id": "r1", "kind": "face", "source": "user"}, {"id": "r2", "source": "agent"}]}}], HUMAN)
case("set_panel drawn panel", "set_panel", True, [{"op": "set_panel", "page": 2, "frame_id": "drawn-b", "set": {"shot": "FS"}}], AI)
case("set_panel for_pages", "set_panel", False,
     [{"op": "for_pages", "pages": [1, 4], "ops": [{"op": "set_panel", "frame_id": P4, "set": {"shot": "CU"}}]}], AI)
case("set_panel dry run", "set_panel", True, [{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"shot": "CU"}}], AI, dry=True)

# --- record_review --------------------------------------------------------------------------------------------------------
case("record_review panel", "record_review", True, [{"op": "record_review", "page": 1, "frame_id": L1C, "kind": "art", "score": 0.9, "notes": "OK", "input_hash": "h"}], AI)
case("record_review page", "record_review", True, [{"op": "record_review", "page": 2, "kind": "regions", "notes": None}], AI)
case("record_review page without a plan", "record_review", True, [{"op": "record_review", "page": 4, "score": 3}], AI)
case("record_review unknown frame", "record_review", False, [{"op": "record_review", "page": 1, "frame_id": "x"}], AI)
case("record_review kind a number", "record_review", True, [{"op": "record_review", "page": 3, "kind": 5}], AI)
case("record_review on a plan with reviews not an object", "record_review", False,
     steps=[step([{"op": "set_page_plan", "page": 3, "plan": {"reviews": [1]}}], AI), step([{"op": "record_review", "page": 3}], AI)])

# --- approve, allow_chat_approval, revoke ---------------------------------------------------------------------------
case("approve by an agent", "approve", False, [{"op": "approve", "gate": "bible"}], AI)
case("approve by a conversation of an agent", "approve", False, [{"op": "approve", "gate": "bible"}], "ai:hermes/9204")
case("approve bad gate", "approve", False, [{"op": "approve", "gate": "cover"}], HUMAN)
case("approve no gate", "approve", False, [{"op": "approve"}], HUMAN)
case("approve name page 2", "approve", True, [{"op": "approve", "gate": "name", "page": 2}], HUMAN)
case("approve name pages 2 and 3", "approve", True, [{"op": "approve", "gate": "name", "page": 2}, {"op": "approve", "gate": "name", "page": 3}], HUMAN)
case("approve name without page", "approve", False, [{"op": "approve", "gate": "name"}], HUMAN)
case("approve name on a locked page", "approve", False, [{"op": "approve", "gate": "name", "page": 5}], HUMAN)
case("approve name by the legacy caller", "approve", True, [{"op": "approve", "gate": "name", "page": 4}], "genko")
case("approve name by human", "approve", True, [{"op": "approve", "gate": "name", "page": 4}], "human")
case("approve art without the name", "approve", False, [{"op": "approve", "gate": "art", "page": 2}], HUMAN)
case("approve art with panels not adopted", "approve", False, [{"op": "approve", "gate": "art", "page": 1}], HUMAN)
case("approve art page 4", "approve", True, [{"op": "approve", "gate": "name", "page": 4}, {"op": "approve", "gate": "art", "page": 4}], HUMAN)
case("approve art page 6 again fixes the style anew", "approve", True, [{"op": "approve", "gate": "art", "page": 6}], HUMAN)
case("approve art after the style went stale", "approve", True,
     steps=[step([{"op": "set_style_catalog", "catalog": CAT}], HUMAN),
            step([{"op": "approve", "gate": "name", "page": 4}, {"op": "approve", "gate": "art", "page": 4}], HUMAN)])
case("approve art with adopted art: the style's tool and reference", "approve", True,
     steps=[step([{"op": "set_panel", "page": 1, "frame_id": L1B, "set": {"status": "skip"}},
                  {"op": "set_panel", "page": 1, "frame_id": L1A, "set": {"status": "skip"}},
                  {"op": "resolve_ticket", "id": T_FIX_AGENT, "note": "直した"},
                  {"op": "revoke", "gate": "art", "page": 6}], HUMAN),
            step([{"op": "approve", "gate": "art", "page": 1}], HUMAN)])
case("approve art with a derived picture adopted", "approve", True,
     steps=[step([{"op": "set_panel", "page": 1, "frame_id": L1B, "set": {"status": "skip"}},
                  {"op": "set_panel", "page": 1, "frame_id": L1A, "set": {"status": "skip"}},
                  {"op": "revoke", "gate": "art", "page": 6}], HUMAN),
            step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a3"}], AI),
            step([{"op": "approve", "gate": "art", "page": 1}], HUMAN)])
case("approve sheet", "approve", True, [{"op": "approve", "gate": "sheet", "character_id": "c-yuki", "candidate_id": "cd_y1"}], HUMAN)
case("approve sheet with a face", "approve", True, [{"op": "approve", "gate": "sheet", "character_id": "c-yuki", "candidate_id": "cd_y2", "face_asset": "$F1"}], HUMAN)
case("approve sheet again replaces the sheet", "approve", True, [{"op": "approve", "gate": "sheet", "character_id": "c-ken", "candidate_id": "cd_k2"}], HUMAN)
case("approve sheet unknown character", "approve", False, [{"op": "approve", "gate": "sheet", "character_id": "c-x", "candidate_id": "cd_y1"}], HUMAN)
case("approve sheet unknown candidate", "approve", False, [{"op": "approve", "gate": "sheet", "character_id": "c-yuki", "candidate_id": "cd_k1"}], HUMAN)
case("approve sheet no candidates", "approve", False, [{"op": "approve", "gate": "sheet", "character_id": "c-mio"}], HUMAN)
case("approve export", "approve", True, [{"op": "approve", "gate": "export"}], HUMAN)
case("approve via a chat", "approve", True, [{"op": "approve", "gate": "export", "via": "ai:hermes" + "x" * 90, "message": "はい" * 300}], HUMAN)
case("approve via a chat without words", "approve", True, [{"op": "approve", "gate": "script", "via": 5}], HUMAN)
case("approve dry run", "approve", True, [{"op": "approve", "gate": "name", "page": 2}], HUMAN, dry=True)
case("approve not per page", "approve", False, [{"op": "for_pages", "pages": "all", "ops": [{"op": "approve", "gate": "name"}]}], HUMAN)
case("allow_chat_approval by an agent", "allow_chat_approval", False, [{"op": "allow_chat_approval", "on": False}], AI)
case("allow_chat_approval off", "allow_chat_approval", True, [{"op": "allow_chat_approval", "on": False}], HUMAN)
case("allow_chat_approval on by a word", "allow_chat_approval", True, [{"op": "allow_chat_approval", "on": "yes"}], "genko")
case("allow_chat_approval without on", "allow_chat_approval", True, [{"op": "allow_chat_approval"}], HUMAN)
case("revoke by an agent", "revoke", False, [{"op": "revoke", "gate": "name", "page": 1}], AI)
case("revoke name", "revoke", True, [{"op": "revoke", "gate": "name", "page": 1, "reason": "直す"}], HUMAN)
case("revoke art of the pilot page", "revoke", True, [{"op": "revoke", "gate": "art", "page": 6, "page_id": "pg_00000000001f"}], HUMAN)
case("revoke art of another page", "revoke", True, [{"op": "revoke", "gate": "art", "page": 1}], HUMAN)
case("revoke name by page_id", "revoke", True, [{"op": "revoke", "gate": "name", "page_id": "pg_00000000001f"}], HUMAN)
case("revoke sheet", "revoke", True, [{"op": "revoke", "gate": "sheet", "character_id": "c-ken"}], HUMAN)
case("revoke sheet unknown", "revoke", False, [{"op": "revoke", "gate": "sheet", "character_id": "c-x"}], HUMAN)
case("revoke export", "revoke", False, [{"op": "revoke", "gate": "export"}], HUMAN)
case("revoke no gate", "revoke", False, [{"op": "revoke"}], HUMAN)
case("revoke name no page", "revoke", False, [{"op": "revoke", "gate": "name"}], HUMAN)

# --- tickets ----------------------------------------------------------------------------------------------------------------
case("request_approval", "request_approval", True, [{"op": "request_approval", "gate": "name", "pages": [4, 2, 4], "note": "見て"}], AI)
case("request_approval bad gate", "request_approval", False, [{"op": "request_approval", "gate": "page"}], AI)
case("request_approval unknown page", "request_approval", False, [{"op": "request_approval", "gate": "name", "pages": [2, 99]}], AI)
case("request_approval pages as text", "request_approval", True, [{"op": "request_approval", "gate": "art", "pages": "21"}], AI)
case("request_approval page a word", "request_approval", False, [{"op": "request_approval", "gate": "art", "pages": ["x"]}], AI)
case("request_approval page past 64 bits", "request_approval", False, [{"op": "request_approval", "gate": "art", "pages": [99, 2 ** 70]}], AI)
case("request_approval sheet", "request_approval", True, [{"op": "request_approval", "gate": "sheet", "character_id": "c-mio", "pages": None}], AI)
case("request_approval pages a number", "request_approval", False, [{"op": "request_approval", "gate": "art", "pages": 3}], AI)
case("request_fix by an agent", "request_fix", True, [{"op": "request_fix", "page": 1, "frame_id": L1A, "instruction": " 手を直す ", "candidate_id": "c"}], AI)
case("request_fix by a person, no panel", "request_fix", True, [{"op": "request_fix", "page": 4, "instruction": "背景"}], HUMAN)
case("request_fix empty", "request_fix", False, [{"op": "request_fix", "page": 4, "instruction": "  "}], HUMAN)
case("request_fix unknown frame", "request_fix", False, [{"op": "request_fix", "page": 4, "frame_id": "x", "instruction": "a"}], HUMAN)
case("request_fix on a skipped panel", "request_fix", True,
     steps=[step([{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"status": "skip"}}], HUMAN),
            step([{"op": "request_fix", "page": 4, "frame_id": P4, "instruction": "直す"}], HUMAN)])
case("request_fix instruction a number", "request_fix", True, [{"op": "request_fix", "page": 4, "instruction": 12}], AI)
case("ask_human", "ask_human", True, [{"op": "ask_human", "text": "質問"}], AI)
case("ask_human with a panel", "ask_human", True, [{"op": "ask_human", "text": "質問", "page": 6, "frame_id": P6A, "item": "art"}], AI)
case("ask_human empty", "ask_human", False, [{"op": "ask_human", "text": ""}], AI)
case("ask_human page 0", "ask_human", False, [{"op": "ask_human", "text": "q", "page": 0}], AI)
case("ask_human by page_id", "ask_human", True, [{"op": "ask_human", "text": "q", "page_id": "pg_000000000013"}], AI)
case("ask_human unknown frame", "ask_human", False, [{"op": "ask_human", "text": "q", "page": 1, "frame_id": "x"}], AI)
case("ask_human frame without page", "ask_human", True, [{"op": "ask_human", "text": "q", "frame_id": "x"}], AI)
case("resolve_ticket by an agent", "resolve_ticket", True, [{"op": "resolve_ticket", "id": T_FIX_AGENT, "note": "夕焼けにした"}], AI)
case("resolve_ticket by an agent without a note", "resolve_ticket", False, [{"op": "resolve_ticket", "id": T_FIX_AGENT}], AI)
case("resolve_ticket a person's question by an agent", "resolve_ticket", False, [{"op": "resolve_ticket", "id": T_HELP, "note": "n"}], AI)
case("resolve_ticket a gate by an agent", "resolve_ticket", False, [{"op": "resolve_ticket", "id": T_GATE_NAME, "note": "n"}], AI)
case("resolve_ticket a gate by a person", "resolve_ticket", True, [{"op": "resolve_ticket", "id": T_GATE_NAME}], HUMAN)
case("resolve_ticket a fix for a person", "resolve_ticket", True, [{"op": "resolve_ticket", "id": T_FIX_HUMAN, "note": "見ました"}], HUMAN)
case("resolve_ticket done", "resolve_ticket", False, [{"op": "resolve_ticket", "id": T_DONE, "note": "n"}], AI)
case("resolve_ticket unknown", "resolve_ticket", False, [{"op": "resolve_ticket", "id": "t9"}], HUMAN)
case("resolve_ticket an old ticket", "resolve_ticket", True, [{"op": "resolve_ticket", "id": "t1"}], HUMAN)
case("resolve_ticket while another fix is open", "resolve_ticket", True,
     steps=[step([{"op": "request_fix", "page": 1, "frame_id": L1C, "instruction": "もう一つ"}], HUMAN),
            step([{"op": "resolve_ticket", "id": T_FIX_AGENT, "note": "一つ目"}], AI)])
case("reopen_ticket by an agent", "reopen_ticket", False, [{"op": "reopen_ticket", "id": T_DONE}], AI)
case("reopen_ticket", "reopen_ticket", True, [{"op": "reopen_ticket", "id": T_DONE, "note": " まだ "}], HUMAN)
case("reopen_ticket a fix on a panel", "reopen_ticket", True,
     steps=[step([{"op": "resolve_ticket", "id": T_FIX_AGENT, "note": "直した"}], AI),
            step([{"op": "reopen_ticket", "id": T_FIX_AGENT, "note": "まだ"}], HUMAN)])
case("reopen_ticket on a skipped panel", "reopen_ticket", True,
     steps=[step([{"op": "set_panel", "page": 1, "frame_id": L1C, "set": {"status": "skip"}},
                  {"op": "reopen_ticket", "id": T_FIX_AGENT}], HUMAN)])
case("reopen_ticket unknown", "reopen_ticket", False, [{"op": "reopen_ticket", "id": "x"}], HUMAN)
case("add_ticket", "add_ticket", True, [{"op": "add_ticket", "page": 2, "frame_id": "drawn-a", "role": "ink", "assignee": "アシ", "rate": "1000"}], AI)
case("add_ticket with id", "add_ticket", True, [{"op": "add_ticket", "page": 3, "id": "t-new"}], AI)
case("add_ticket no page", "add_ticket", False, [{"op": "add_ticket"}], AI)
case("add_ticket unknown page", "add_ticket", False, [{"op": "add_ticket", "page": 9}], AI)
case("set_ticket", "set_ticket", True, [{"op": "set_ticket", "id": "t1", "status": "done", "assignee": "誰か", "rate": 500}], AI)
case("set_ticket studio ticket", "set_ticket", True, [{"op": "set_ticket", "id": T_HELP, "status": "closed"}], AI)
case("set_ticket unknown", "set_ticket", False, [{"op": "set_ticket", "id": "t9", "status": "done"}], AI)
case("set_ticket no id", "set_ticket", False, [{"op": "set_ticket"}], AI)

# --- regions and references --------------------------------------------------------------------------------------------
case("add_region by an agent", "add_region", True, [{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"kind": "prop", "rect_mm": [1, 2, 3, 4]}}], AI)
case("add_region by a person", "add_region", True, [{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"kind": "face", "id": "rg_new", "source": "agent"}}], HUMAN)
case("add_region twice without id", "add_region", False,
     [{"op": "add_region", "page": 4, "frame_id": P4, "region": {"kind": "a"}}, {"op": "add_region", "page": 4, "frame_id": P4, "region": {"kind": "b"}}], AI)
case("add_region existing id", "add_region", False, [{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"kind": "face", "id": "rg_face"}}], AI)
case("add_region no kind", "add_region", False, [{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"id": "r"}}], AI)
case("add_region pairs", "add_region", True, [{"op": "add_region", "page": 4, "frame_id": P4, "region": [["kind", "face"], ["id", "rp"]]}], AI)
case("add_region a text", "add_region", False, [{"op": "add_region", "page": 4, "frame_id": P4, "region": "face"}], AI)
case("add_region a number", "add_region", False, [{"op": "add_region", "page": 4, "frame_id": P4, "region": 5}], AI)
case("add_region new panel", "add_region", True, [{"op": "add_region", "page": 6, "frame_id": P6C, "region": {"kind": "face"}}], HUMAN)
case("edit_region a person's by an agent", "edit_region", False, [{"op": "edit_region", "page": 1, "frame_id": L1C, "id": "rg_face", "set": {"rect_mm": [0, 0, 1, 1]}}], AI)
case("edit_region by an agent", "edit_region", True, [{"op": "edit_region", "page": 1, "frame_id": L1C, "id": "rg_body", "set": {"rect_mm": [0, 0, 1, 1], "id": "x", "source": "user"}}], AI)
case("edit_region a person's by a person", "edit_region", True, [{"op": "edit_region", "page": 1, "frame_id": L1C, "id": "rg_face", "set": {"char": "c-ken"}}], HUMAN)
case("edit_region unknown", "edit_region", False, [{"op": "edit_region", "page": 1, "frame_id": L1C, "id": "rg_x", "set": {}}], AI)
case("edit_region set a list", "edit_region", False, [{"op": "edit_region", "page": 1, "frame_id": L1C, "id": "rg_body", "set": [1]}], AI)
case("delete_region by an agent", "delete_region", True, [{"op": "delete_region", "page": 1, "frame_id": L1C, "id": "rg_body"}], AI)
case("delete_region a person's by an agent", "delete_region", False, [{"op": "delete_region", "page": 1, "frame_id": L1C, "id": "rg_face"}], AI)
case("delete_region a person's by a person", "delete_region", True, [{"op": "delete_region", "page": 1, "frame_id": L1C, "id": "rg_face"}], HUMAN)
case("delete_region no panel regions", "delete_region", False, [{"op": "delete_region", "page": 4, "frame_id": P4, "id": "x"}], AI)
case("replace_regions agent", "replace_regions", True,
     [{"op": "replace_regions", "page": 1, "frame_id": L1C, "source": "agent", "regions": [{"kind": "face", "id": "rg_a"}, {"kind": "person"}]}], AI)
case("replace_regions detected", "replace_regions", True, [{"op": "replace_regions", "page": 1, "frame_id": L1C, "source": "detected", "regions": []}], AI)
case("replace_regions default source", "replace_regions", True, [{"op": "replace_regions", "page": 1, "frame_id": L1C}], AI)
case("replace_regions user", "replace_regions", False, [{"op": "replace_regions", "page": 1, "frame_id": L1C, "source": "user", "regions": []}], HUMAN)
case("replace_regions a region not an object", "replace_regions", False, [{"op": "replace_regions", "page": 1, "frame_id": L1C, "regions": [5]}], AI)
case("bind_ref", "bind_ref", True, [{"op": "bind_ref", "page": 1, "frame_id": L1C, "ref": {"source": "$A2", "role": "pose"}}], AI)
case("bind_ref replaces", "bind_ref", True, [{"op": "bind_ref", "page": 1, "frame_id": L1C, "ref": {"id": "rf_roof", "source": "$L1", "role": "location", "order": 5}}], AI)
case("bind_ref no role", "bind_ref", False, [{"op": "bind_ref", "page": 1, "frame_id": L1C, "ref": {"source": "x"}}], AI)
case("bind_ref new panel", "bind_ref", True, [{"op": "bind_ref", "page": 4, "frame_id": P4, "ref": {"source": "x", "role": "y", "id": "rf_1"}}], AI)
case("unbind_ref", "unbind_ref", True, [{"op": "unbind_ref", "page": 1, "frame_id": L1C, "id": "rf_roof"}], AI)
case("unbind_ref unknown", "unbind_ref", True, [{"op": "unbind_ref", "page": 1, "frame_id": L1C, "id": "x"}], AI)
case("unbind_ref no frame", "unbind_ref", False, [{"op": "unbind_ref", "page": 1, "id": "rf_roof"}], AI)

# --- assets and references --------------------------------------------------------------------------------------------------
case("register_assets", "register_assets", True, [{"op": "register_assets", "assets": {"$A2": {"kind": "art"}, "$A1": {"note": "n"}}}], AI)
case("register_assets meta null", "register_assets", True, [{"op": "register_assets", "assets": {"$A3": None}}], AI)
case("register_assets unknown asset", "register_assets", False, [{"op": "register_assets", "assets": {NOASSET: {}}}], AI)
case("register_assets not an asset", "register_assets", False, [{"op": "register_assets", "assets": {"pic.png": {}}}], AI)
case("register_assets meta a list", "register_assets", False, [{"op": "register_assets", "assets": {"$A2": [1]}}], AI)
case("register_assets a list", "register_assets", False, [{"op": "register_assets", "assets": ["$A2"]}], AI)
case("register_assets a prefix of a digest", "register_assets", True, [{"op": "register_assets", "assets": {"sha256:22": {"kind": "x"}}}], AI)
case("register_assets the empty digest", "register_assets", True, [{"op": "register_assets", "assets": {"sha256:": {"kind": "x"}}}], AI)
case("register_assets a digest that leaves assets/", "register_assets", False, [{"op": "register_assets", "assets": {"sha256:../project.json": {}}}], AI)
case("register_assets the folder of assets/", "register_assets", False, [{"op": "register_assets", "assets": {"sha256:.": {}}}], AI)
case("register_assets an absolute digest", "register_assets", False, [{"op": "register_assets", "assets": {"sha256:/etc": {}}}], AI)
case("register_assets a digest of other letters", "register_assets", False, [{"op": "register_assets", "assets": {"sha256:写真": {}}}], AI)
case("attach_reference to a character", "attach_reference", True, [{"op": "attach_reference", "target": {"character_id": "c-yuki"}, "asset": "$S2", "kind": "sheet"}], AI)
case("attach_reference sheet of a locked character by an agent", "attach_reference", False,
     [{"op": "attach_reference", "target": {"character_id": "c-ken"}, "asset": "$S2", "kind": "face"}], AI)
case("attach_reference sheet of a locked character by a person", "attach_reference", True,
     [{"op": "attach_reference", "target": {"character_id": "c-ken"}, "asset": "$S2", "kind": "face"}], HUMAN)
case("attach_reference reference to a locked character", "attach_reference", True,
     [{"op": "attach_reference", "target": {"character_id": "c-ken"}, "asset": "$S2"}], AI)
case("attach_reference to a location", "attach_reference", True, [{"op": "attach_reference", "target": {"location_id": "loc-room"}, "asset": "$L1", "kind": "location"}], AI)
case("attach_reference to a prop", "attach_reference", True, [{"op": "attach_reference", "target": {"prop_id": "prop-bag"}, "asset": "$A1"}], AI)
case("attach_reference unknown prop", "attach_reference", False, [{"op": "attach_reference", "target": {"prop_id": "p-x"}, "asset": "$A1"}], AI)
case("attach_reference no target", "attach_reference", False, [{"op": "attach_reference", "asset": "$A1"}], AI)
case("attach_reference no asset", "attach_reference", False, [{"op": "attach_reference", "target": {"prop_id": "prop-bag"}}], AI)
case("attach_reference unknown asset", "attach_reference", False, [{"op": "attach_reference", "target": {"prop_id": "prop-bag"}, "asset": NOASSET}], AI)
case("attach_reference target a list", "attach_reference", False, [{"op": "attach_reference", "target": ["c-yuki"], "asset": "$A1"}], AI)
case("attach_reference unknown character", "attach_reference", False, [{"op": "attach_reference", "target": {"character_id": "c-x"}, "asset": "$A1"}], AI)

# --- requests and candidates ---------------------------------------------------------------------------------------------
case("open_request new", "open_request", True, [{"op": "open_request", "request": {"id": "rq_new", "purpose": "art", "mode": "new", "target": {"page": 4, "frame_id": P4}}}], AI)
case("open_request again", "open_request", True, [{"op": "open_request", "request": {"id": "rq_open", "purpose": "art"}}], AI)
case("open_request an edit again", "open_request", True, [{"op": "open_request", "request": {"id": "rq_edit"}}], AI)
case("open_request twice in a batch", "open_request", True,
     [{"op": "open_request", "request": {"id": "rq_x", "target": {"page": 1, "frame_id": L1A}, "mode": "inpaint"}},
      {"op": "open_request", "request": {"id": "rq_x", "target": {"page": 1, "frame_id": L1A}}}], AI)
case("open_request no id", "open_request", False, [{"op": "open_request", "request": {"purpose": "art"}}], AI)
case("open_request no target panel", "open_request", True, [{"op": "open_request", "request": {"id": "rq_sheet", "target": {"character_id": "c-yuki"}}}], AI)
case("open_request unknown page", "open_request", False, [{"op": "open_request", "request": {"id": "rq_y", "target": {"page": 99, "frame_id": "x"}}}], AI)
case("open_request target by page_id", "open_request", True, [{"op": "open_request", "request": {"id": "rq_z", "target": {"page_id": "pg_00000000001f", "frame_id": P6B}}}], AI)
case("open_request pairs", "open_request", True, [{"op": "open_request", "request": [["id", "rq_p"]]}], AI)
case("close_request", "close_request", True, [{"op": "close_request", "id": "rq_open", "reason": "superseded"}], AI)
case("close_request default reason", "close_request", True, [{"op": "close_request", "id": "rq_edit"}], AI)
case("close_request unknown", "close_request", False, [{"op": "close_request", "id": "rq_x"}], AI)
case("import_candidates for a request", "import_candidates", True,
     [{"op": "import_candidates", "request_id": "rq_open", "candidates": [{"asset": "$A2", "px": [30, 40], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates the same picture", "import_candidates", True,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [40, 30], "origin": {"kind": "self"}}]}], AI)
case("import_candidates with metrics, face and mapping", "import_candidates", True,
     [{"op": "import_candidates", "page": 6, "frame_id": P6A, "candidates": [
         {"id": "cd_m", "asset": "$A3", "px": ["64", 48.9], "origin": {"kind": "fixture"}, "metrics": {"a": 1}, "face_box01": ["0.1", 0.2, 0.3, 0.4],
          "mapping": {"pad_mm": 2}, "upscaled": {"from": "x"}, "mode": "edit", "parent": "cd_a1"},
         {"asset": "$G1", "px": [36, 36], "origin": {"kind": "genko"}, "mapping": "no"}]}], AI)
case("import_candidates two of one picture at once", "import_candidates", True,
     [{"op": "import_candidates", "page": 6, "frame_id": P6A, "candidates": [
         {"id": "c1", "asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}},
         {"id": "c2", "asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates for a character", "import_candidates", True,
     [{"op": "import_candidates", "character_id": "c-yuki", "candidates": [{"id": "cd_y4", "asset": "$F1", "px": [8, 8], "origin": {"kind": "agent"}},
                                                                          {"asset": "$S2", "px": [22, 30], "origin": {"kind": "agent"}, "face_box01": [0, 0, 1, 1]}]}], AI)
case("import_candidates for a location", "import_candidates", True,
     [{"op": "import_candidates", "location_id": "loc-room", "candidates": [{"asset": "$L1", "px": [48, 32], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates a withdrawn picture again", "import_candidates", True,
     steps=[step([{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y1"]}], AI),
            step([{"op": "import_candidates", "character_id": "c-yuki", "candidates": [{"asset": "$S2", "px": [22, 30], "origin": {"kind": "agent"}}]}], AI)])
case("import_candidates answers a sent-back sheet", "import_candidates", True,
     steps=[step([{"op": "reject_sheet", "character_id": "c-yuki", "candidate_ids": ["cd_y1"], "note": "目を大きく"}], HUMAN),
            step([{"op": "import_candidates", "character_id": "c-yuki", "candidates": [{"asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}}]}], AI)])
case("import_candidates empty", "import_candidates", False, [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": []}], AI)
case("import_candidates bad origin", "import_candidates", False,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [1, 1], "origin": {"kind": "robot"}}]}], AI)
case("import_candidates no origin", "import_candidates", False, [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [1, 1]}]}], AI)
case("import_candidates bad px", "import_candidates", False,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [0, 5], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates px a word", "import_candidates", False,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [5, "x"], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates px three", "import_candidates", False,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"asset": "$A1", "px": [5, 5, 5], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates no asset", "import_candidates", False,
     [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": [{"px": [5, 5], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates unknown request", "import_candidates", False, [{"op": "import_candidates", "request_id": "rq_none", "candidates": [{}]}], AI)
case("import_candidates unknown panel goes to the orphans", "import_candidates", True,
     [{"op": "import_candidates", "page": 1, "frame_id": "gone", "candidates": [{"asset": "$A1", "px": [40, 30], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates on a node goes to the orphans", "import_candidates", True,
     [{"op": "import_candidates", "page": 1, "frame_id": P1_NODE, "candidates": [{"asset": "$A1", "px": [40, 30], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates unknown page", "import_candidates", False,
     [{"op": "import_candidates", "page": 9, "frame_id": "x", "candidates": [{"asset": "$A1", "px": [40, 30], "origin": {"kind": "agent"}}]}], AI)
case("import_candidates candidate not an object", "import_candidates", False, [{"op": "import_candidates", "page": 1, "frame_id": L1C, "candidates": ["x"]}], AI)
case("import_candidates face not finite", "import_candidates", True,
     [{"op": "import_candidates", "page": 6, "frame_id": P6A, "candidates": [{"asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}, "face_box01": ["inf", 0, 1, 1]}]}], AI,
     cpp="apply", cpp_says="face_box01 must be a finite number")
case("review_candidates", "review_candidates", True,
     [{"op": "review_candidates", "page": 1, "frame_id": L1C, "reviews": [{"candidate_id": "cd_a2", "score": 0.5, "fix": "空", "checks": {"hands": False}},
                                                                         {"candidate_id": "cd_a3", "note": "小さい", "checks": "no"}]}], AI)
case("review_candidates unknown", "review_candidates", False, [{"op": "review_candidates", "page": 1, "frame_id": L1C, "reviews": [{"candidate_id": "x"}]}], AI)
case("review_candidates none", "review_candidates", True, [{"op": "review_candidates", "page": 1, "frame_id": L1C}], AI)
case("review_candidates a panel without candidates", "review_candidates", False, [{"op": "review_candidates", "page": 4, "frame_id": P4, "reviews": [{"candidate_id": "x"}]}], AI)
case("review_candidates an object", "review_candidates", False, [{"op": "review_candidates", "page": 1, "frame_id": L1C, "reviews": {"candidate_id": "cd_a2"}}], AI)
case("set_candidate", "set_candidate", True, [{"op": "set_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "status": "shortlisted"}], AI)
case("set_candidate bad status", "set_candidate", False, [{"op": "set_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "status": "best"}], AI)
case("set_candidate unknown", "set_candidate", False, [{"op": "set_candidate", "page": 1, "frame_id": L1C, "candidate_id": "x", "status": "rejected"}], AI)
case("withdraw_candidates own", "withdraw_candidates", True, [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y1"]}], AI)
case("withdraw_candidates another agent's", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y2"]}], AI)
case("withdraw_candidates a person's", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y3"]}], AI)
case("withdraw_candidates by a person", "withdraw_candidates", True, [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y2", "cd_y3"]}], HUMAN)
case("withdraw_candidates the approved sheet", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-ken", "candidate_ids": ["cd_k1"]}], AI)
case("withdraw_candidates the approved sheet by a person", "withdraw_candidates", True, [{"op": "withdraw_candidates", "character_id": "c-ken", "candidate_ids": ["cd_k1"]}], HUMAN)
case("withdraw_candidates unknown", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_x"]}], AI)
case("withdraw_candidates none", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-yuki"}], AI)
case("withdraw_candidates no owner", "withdraw_candidates", False, [{"op": "withdraw_candidates", "candidate_ids": ["cd_y1"]}], AI)
case("withdraw_candidates a character without candidates", "withdraw_candidates", False, [{"op": "withdraw_candidates", "character_id": "c-mio", "candidate_ids": ["x"]}], AI)
case("withdraw_candidates location", "withdraw_candidates", True, [{"op": "withdraw_candidates", "location_id": "loc-roof", "candidate_ids": ["cd_l1"]}], AI)
case("withdraw_candidates by another conversation", "withdraw_candidates", False,
     [{"op": "withdraw_candidates", "character_id": "c-yuki", "candidate_ids": ["cd_y1"]}], "ai:hermes/9204")
case("reject_sheet by an agent", "reject_sheet", False, [{"op": "reject_sheet", "character_id": "c-yuki"}], AI)
case("reject_sheet all", "reject_sheet", True, [{"op": "reject_sheet", "character_id": "c-yuki"}], HUMAN)
case("reject_sheet some with a note", "reject_sheet", True, [{"op": "reject_sheet", "character_id": "c-yuki", "candidate_ids": ["cd_y2"], "note": "  髪を短く "}], HUMAN)
case("reject_sheet unknown character", "reject_sheet", True, [{"op": "reject_sheet", "character_id": "c-x", "note": "x"}], HUMAN)
case("reject_sheet after the sheet was approved", "reject_sheet", True,
     steps=[step([{"op": "approve", "gate": "sheet", "character_id": "c-yuki", "candidate_id": "cd_y1"}], HUMAN),
            step([{"op": "reject_sheet", "character_id": "c-yuki", "candidate_ids": ["cd_y1"]}], HUMAN)])

# --- adoption and placement ----------------------------------------------------------------------------------------------
case("adopt_candidate another", "adopt_candidate", True, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2"}], AI)
case("adopt_candidate fit and offset", "adopt_candidate", True,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "fit": "contain", "offset_mm": [1, 2.5], "scale": "0.5", "clip_to": "bleed"}], AI)
case("adopt_candidate stretch", "adopt_candidate", True,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a3", "fit": "stretch", "offset_mm": [1.5, -2], "scale": 2}], AI)
case("adopt_candidate unknown fit", "adopt_candidate", True,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "fit": "zoom", "clip_to": "none", "offset_mm": [0, 0, 9]}], AI)
case("adopt_candidate to bg", "adopt_candidate", True, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "to": "bg"}], AI)
case("adopt_candidate to ink", "adopt_candidate", True, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a3", "to": "ink"}], AI)
case("adopt_candidate to draft before the name", "adopt_candidate", True,
     steps=[step([{"op": "import_candidates", "page": 3, "frame_id": P3A, "candidates": [{"id": "cd_p3", "asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}}]}], AI),
            step([{"op": "adopt_candidate", "page": 3, "frame_id": P3A, "candidate_id": "cd_p3", "to": "draft"}], AI)])
case("adopt_candidate art before the name", "adopt_candidate", False, [{"op": "adopt_candidate", "page": 3, "frame_id": P3A, "candidate_id": "x"}], AI)
case("adopt_candidate bad to", "adopt_candidate", False, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "to": "fg"}], AI)
case("adopt_candidate unknown", "adopt_candidate", False, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "x"}], AI)
case("adopt_candidate offset a word", "adopt_candidate", False,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "offset_mm": ["a", 1]}], AI)
case("adopt_candidate offset one number", "adopt_candidate", False,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "offset_mm": [1]}], AI)
case("adopt_candidate offset a number", "adopt_candidate", False,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "offset_mm": 1}], AI)
case("adopt_candidate scale a word", "adopt_candidate", False,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "scale": "big"}], AI)
case("adopt_candidate scale not finite", "adopt_candidate", True,
     [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2", "scale": "inf"}], AI,
     cpp="apply", cpp_says="placement_mm.x must be a finite number")
case("adopt_candidate by an agent on approved art", "adopt_candidate", True,
     steps=[step([{"op": "import_candidates", "page": 6, "frame_id": P6C, "candidates": [{"id": "cd_p6", "asset": "$A1", "px": [40, 30], "origin": {"kind": "agent"}}]}], AI),
            step([{"op": "adopt_candidate", "page": 6, "frame_id": P6C, "candidate_id": "cd_p6"}], AI)])
case("adopt_candidate other art takes a finished page back to ink", "adopt_candidate", True,
     steps=[step([{"op": "advance", "page": 1, "to": "finish"}], HUMAN),
            step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2"}], HUMAN)])
case("adopt_candidate the same again", "adopt_candidate", True, [{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a1"}], HUMAN)
case("adopt_candidate on a slanted panel", "adopt_candidate", True,
     steps=[step([{"op": "approve", "gate": "name", "page": 3}], HUMAN),
            step([{"op": "import_candidates", "page": 3, "frame_id": P3B, "candidates": [{"id": "cd_s", "asset": "$A2", "px": [30, 40], "origin": {"kind": "agent"}}]}], AI),
            step([{"op": "adopt_candidate", "page": 3, "frame_id": P3B, "candidate_id": "cd_s", "clip_to": "bleed"}], AI)])
case("unadopt back to the one before", "unadopt", True,
     steps=[step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2"}], AI),
            step([{"op": "unadopt", "page": 1, "frame_id": L1C}], AI)])
case("unadopt the only one", "unadopt", True, [{"op": "unadopt", "page": 1, "frame_id": L1C}], HUMAN)
case("unadopt nothing", "unadopt", False, [{"op": "unadopt", "page": 1, "frame_id": L1C, "to": "bg"}], AI)
case("unadopt the old placed art", "unadopt", True, [{"op": "unadopt", "page": 1, "frame_id": L1A}], AI)
case("unadopt history with a missing candidate", "unadopt", True,
     steps=[step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a2"},
                  {"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_a1"}], AI),
            step([{"op": "unadopt", "page": 1, "frame_id": L1C}, {"op": "unadopt", "page": 1, "frame_id": L1C}], AI)])
for name, what in (("JX", "a JPEG cut short"), ("BX", "a BMP cut short"), ("GX", "a GIF cut short"), ("CUT", "a PNG cut short"),
                   ("BAD", "no picture"), ("CRC", "a PNG with a broken header"), ("BOMB", "a decompression bomb"),
                   ("PSDX", "a PSD cut short")):
    case(f"adopt_candidate {what}", "adopt_candidate", True,
         steps=[step([{"op": "import_candidates", "page": 1, "frame_id": L1C,
                       "candidates": [{"id": "cd_x", "asset": "$" + name, "px": [9, 6], "origin": {"kind": "agent"}}]}], AI),
                step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_x"}], AI)],
         cpp="apply", cpp_says=CANNOT_READ)
# (a Photoshop file, opened as one picture; one of Lab colours, which this build does not open yet, adopted unread)
for name, what in (("PSD", "a PSD"), ("PSDLAB", "a PSD of Lab colours")):
    case(f"adopt_candidate {what}", "adopt_candidate", True,
         steps=[step([{"op": "import_candidates", "page": 1, "frame_id": L1C,
                       "candidates": [{"id": "cd_x", "asset": "$" + name, "px": [30, 20], "origin": {"kind": "agent"}}]}], AI),
                step([{"op": "adopt_candidate", "page": 1, "frame_id": L1C, "candidate_id": "cd_x"}], AI)])
case("set_placement contain", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1C, "fit": "contain"}], AI)
case("set_placement offset and scale", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1C, "offset_mm": [3, -4], "scale": 1.25}], AI)
case("set_placement clip bleed", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1C, "clip_to": "bleed"}], AI)
case("set_placement nothing adopted", "set_placement", False, [{"op": "set_placement", "page": 1, "frame_id": L1C, "to": "bg"}], AI)
case("set_placement of a picture with no candidate", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1A, "fit": "stretch"}], AI)
case("set_placement of a picture with no candidate, scaled", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1A, "scale": 0.5, "offset_mm": [2, 2]}], AI)
case("set_placement a region not a box", "set_placement", True,
     steps=[step([{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"kind": "x", "id": "rg_odd", "rect_mm": [1, 2, 3]}}], AI),
            step([{"op": "set_placement", "page": 1, "frame_id": L1C, "scale": 0.5}], AI)])
case("set_placement region box of words", "set_placement", False,
     steps=[step([{"op": "add_region", "page": 1, "frame_id": L1C, "region": {"kind": "x", "id": "rg_odd", "rect_mm": [1, 2, 3, "x"]}}], AI),
            step([{"op": "set_placement", "page": 1, "frame_id": L1C, "scale": 0.5}], AI)])
case("set_placement scale not finite", "set_placement", True, [{"op": "set_placement", "page": 1, "frame_id": L1C, "scale": "nan"}], AI,
     cpp="apply", cpp_says="placement_mm.x must be a finite number")

# --- place_asset ---------------------------------------------------------------------------------------------------------------
case("place_asset on a page", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$A1"}], AI)
case("place_asset in a panel", "place_asset", True, [{"op": "place_asset", "page": 1, "asset": "$A2", "frame_id": L1A, "fit": "contain", "title": "絵"}], AI)
case("place_asset at a place", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$A3", "placement_mm": [10, "20", 30.5, 40], "to": "name"}], AI)
case("place_asset at a place clipped", "place_asset", True, [{"op": "place_asset", "page": 1, "asset": "$A3", "placement_mm": [10, 20, 30, 40], "frame_id": L1A, "clip_to": "frame"}], AI)
case("place_asset bg", "place_asset", True, [{"op": "place_asset", "page": 1, "asset": "$A1", "to": "bg", "clip_to": "bleed", "frame_id": L1C}], AI)
case("place_asset draft", "place_asset", True, [{"op": "place_asset", "page": 2, "asset": "$G1", "to": "draft"}], AI)
case("place_asset bad to", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "$A1", "to": "ink"}], AI)
case("place_asset a path", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "/tmp/a.png"}], AI)
case("place_asset none", "place_asset", False, [{"op": "place_asset", "page": 4}], AI)
case("place_asset unknown", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": NOASSET}], AI)
case("place_asset a prefix of a digest", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "sha256:22"}], AI)
case("place_asset a JPEG", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$J1"}], AI)
case("place_asset a PNG cut short", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$CUT"}], AI,
     cpp="apply", cpp_says=CANNOT_READ)
for name, what in (("JX", "JPEG"), ("BX", "BMP"), ("GX", "GIF")):
    case(f"place_asset a {what} cut short", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$" + name, "frame_id": P4}], AI,
         cpp="apply", cpp_says=CANNOT_READ)
# (a Photoshop file's size read from its header, as Image.open reads it — one of Lab colours too —; one cut short in its
# merged picture refused here)
case("place_asset a PSD", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$PSD", "frame_id": P4, "fit": "contain"}], AI)
case("place_asset a PSD of Lab colours", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$PSDLAB"}], AI)
case("place_asset a PSD cut short", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$PSDX"}], AI,
     cpp="apply", cpp_says=CANNOT_READ)
case("place_asset not a picture", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "$BAD"}], AI)
case("place_asset a broken header", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "$CRC"}], AI)
case("place_asset a decompression bomb", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "$BOMB"}], AI)
case("place_asset box of three", "place_asset", False, [{"op": "place_asset", "page": 4, "asset": "$A1", "placement_mm": [1, 2, 3]}], AI)
case("place_asset box not finite", "place_asset", True, [{"op": "place_asset", "page": 4, "asset": "$A1", "placement_mm": [1, 2, "inf", 4]}], AI,
     cpp="apply", cpp_says="placement_mm.width must be a finite number")
case("place_asset not a leaf", "place_asset", False, [{"op": "place_asset", "page": 1, "asset": "$A1", "frame_id": P1_NODE}], AI)
case("place_asset strict before the name", "place_asset", False,
     steps=[step([{"op": "set_meta", "strict_gates": True}], HUMAN), step([{"op": "place_asset", "page": 2, "asset": "$A1"}], AI)])
case("place_asset strict draft before the name", "place_asset", True,
     steps=[step([{"op": "set_meta", "strict_gates": True}], HUMAN), step([{"op": "place_asset", "page": 2, "asset": "$A1", "to": "draft"}], AI)])
case("place_asset on the cover's bleed panel", "place_asset", True, [{"op": "place_asset", "page": 7, "asset": "$A2", "frame_id": P7, "clip_to": "bleed"}], AI)
case("place_asset on a locked page", "place_asset", False, [{"op": "place_asset", "page": 5, "asset": "$A1"}], AI)
case("set_finish", "set_finish", True, [{"op": "set_finish", "page": 1, "frame_id": L1C, "finish": {"black": 30, "screen": "fm"}}], AI)
case("set_finish null", "set_finish", True, [{"op": "set_finish", "page": 1, "frame_id": L1C}], AI)
case("set_finish no layer", "set_finish", True, [{"op": "set_finish", "page": 1, "frame_id": L1A, "finish": {"black": 1}}], AI)
case("set_finish unknown frame", "set_finish", False, [{"op": "set_finish", "page": 1, "frame_id": "x", "finish": {}}], AI)

# --- layout and proposals ---------------------------------------------------------------------------------------------
TREE = {"rect_mm": [20, 25, 160, 230], "axis": "horizontal", "children": [
    {"rect_mm": [20, 25, 160, 100], "bleed": True, "border_mm": 1.2, "corner_mm": 2},
    {"rect_mm": ["20", 130, 160, 125.555], "axis": "vertical", "split": {"a": 0.5}, "children": [
        {"rect_mm": [20, 130, 80, 125], "poly": [[20, 130], ["100", 130], [100, 255]], "custom": 1, "clip": 0},
        {"rect_mm": [102, 130, 78, 125], "curves": [1, "2", 0, 0], "line": {"kind": "double"}}]}]}
case("set_layout on a blank page", "set_layout", True, [{"op": "set_layout", "page": 4, "tree": TREE}], AI)
case("set_layout over panels without force", "set_layout", False, [{"op": "set_layout", "page": 1, "tree": TREE}], AI)
case("set_layout over panels with force", "set_layout", True, [{"op": "set_layout", "page": 1, "tree": TREE, "force": True}], AI)
case("set_layout no rect", "set_layout", False, [{"op": "set_layout", "page": 4, "tree": {"children": []}}], AI)
case("set_layout no tree", "set_layout", False, [{"op": "set_layout", "page": 4}], AI)
case("set_layout children without axis", "set_layout", False, [{"op": "set_layout", "page": 4, "tree": {"rect_mm": [0, 0, 1, 1], "children": [{"rect_mm": [0, 0, 1, 1]}]}}], AI)
case("set_layout free without children", "set_layout", True, [{"op": "set_layout", "page": 4, "tree": {"rect_mm": [0, 0, 100, 100], "axis": "free"}}], AI)
case("set_layout a child without rect", "set_layout", False, [{"op": "set_layout", "page": 4, "tree": {"rect_mm": [0, 0, 1, 1], "axis": "free", "children": [{}]}}], AI)
case("set_layout rect of three", "set_layout", False, [{"op": "set_layout", "page": 4, "tree": {"rect_mm": [0, 0, 1]}}], AI)
case("set_layout rect not finite", "set_layout", True, [{"op": "set_layout", "page": 4, "tree": {"rect_mm": [0, 0, "inf", 1]}}], AI,
     cpp="apply", cpp_says="rect_mm.width must be a finite number")
case("set_layout strict on an approved name by an agent", "set_layout", False,
     steps=[step([{"op": "set_meta", "strict_gates": True}], HUMAN), step([{"op": "set_layout", "page": 6, "tree": TREE, "force": True}], AI)])
case("set_layout strict by a person", "set_layout", True,
     steps=[step([{"op": "set_meta", "strict_gates": True}], HUMAN), step([{"op": "set_layout", "page": 6, "tree": TREE, "force": True}], HUMAN)])
case("propose layout replaces an open one", "propose", True, [{"op": "propose", "proposal": {"kind": "layout", "page": 2, "tree": {}}}], AI)
case("propose the same again", "propose", True, [{"op": "propose", "proposal": {"id": "pr_layout", "kind": "layout", "page": 2}}], AI)
case("propose bad kind", "propose", False, [{"op": "propose", "proposal": {"kind": "art", "page": 2}}], AI)
case("propose no page", "propose", False, [{"op": "propose", "proposal": {"kind": "lines"}}], AI)
case("propose by page_id", "propose", True, [{"op": "propose", "proposal": {"id": "pr_x", "kind": "lines", "page_id": "pg_000000000013"}}], AI)
case("propose again after a rejection", "propose", True,
     steps=[step([{"op": "resolve_proposal", "id": "pr_layout", "status": "rejected", "note": "違う"}], HUMAN),
            step([{"op": "propose", "proposal": {"id": "pr_layout", "kind": "layout", "page": 2}}], OTHER)])
case("resolve_proposal by an agent", "resolve_proposal", False, [{"op": "resolve_proposal", "id": "pr_layout", "status": "accepted"}], AI)
case("resolve_proposal accepted", "resolve_proposal", True, [{"op": "resolve_proposal", "id": "pr_lines", "status": "accepted"}], HUMAN)
case("resolve_proposal bad status", "resolve_proposal", False, [{"op": "resolve_proposal", "id": "pr_lines", "status": "maybe"}], HUMAN)
case("resolve_proposal unknown", "resolve_proposal", False, [{"op": "resolve_proposal", "id": "pr_x", "status": "accepted"}], HUMAN)

# --- set_bible -----------------------------------------------------------------------------------------------------------
case("set_bible plot", "set_bible", True, [{"op": "set_bible", "plot": 12}], AI)
case("set_bible characters", "set_bible", True, [{"op": "set_bible", "characters": [{"id": "c-a"}, {"name": "b"}], "constraints": ["x", 1, None]}], AI,
     cpp="apply", cpp_says="only a person can change a character's approval")
case("set_bible characters by a person", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-a"}, {"name": "b"}], "constraints": ["x", 1, None]}], HUMAN)
case("set_bible characters with a text", "set_bible", True, [{"op": "set_bible", "characters": [{"id": "c-a"}, "b"]}], AI,
     cpp="apply", cpp_says="each of the bible's characters must be an object, not 'b'")
case("set_bible characters a dict", "set_bible", True, [{"op": "set_bible", "characters": {"c-a": {}}}], AI,
     cpp="apply", cpp_says="each of the bible's characters must be an object, not 'c-a'")
case("set_bible characters as pairs", "set_bible", True, [{"op": "set_bible", "characters": [[["id", "c-a"]]]}], AI,
     cpp="apply", cpp_says="each of the bible's characters must be an object, not [['id', 'c-a']]")
case("set_bible characters null", "set_bible", False, [{"op": "set_bible", "characters": None}], AI)
case("set_bible constraints a text", "set_bible", True, [{"op": "set_bible", "constraints": "ab"}], AI)
case("set_bible then a character op", "set_bible", False, [{"op": "set_bible", "characters": ["c-a"]}, {"op": "upsert_character", "character": {"id": "c-b"}}], AI,
     cpp="apply", cpp_says="ops[0] set_bible: each of the bible's characters must be an object, not 'c-a'")
case("set_bible on a locked page's book", "set_bible", True, [{"op": "set_bible", "plot": "p", "page": 5}], AI)

# What only a person holds, refused to an agent in this build only (SPEC COMP-01a: Python lets the agent do it): a
# panel's regions set wholesale, a character's approval in the bible or upserted (locked, and an approved character's
# sheet and face), an approved character deleted, a request for approval changed, the style a person fixed; a person
# does each, and an agent what is not a person's (the sheet and face of a character that is not locked among them).
PERSON_ONLY = "only a person can "
case("set_panel regions by an agent", "set_panel", True,
     [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"regions": [{"id": "rg_x", "kind": "face", "rect_mm": [1, 2, 3, 4]}]}}],
     AI, cpp="apply", cpp_says=PERSON_ONLY + "set a panel's regions with set_panel")
case("set_panel unset regions by an agent", "set_panel", True,
     [{"op": "set_panel", "page": 1, "frame_id": L1C, "unset": ["regions"]}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "set a panel's regions with set_panel")
case("set_panel regions by a person", "set_panel", True,
     [{"op": "set_panel", "page": 4, "frame_id": P4, "set": {"regions": [{"id": "rg_x", "kind": "face", "rect_mm": [1, 2, 3, 4]}]}}],
     HUMAN)
case("set_panel unset regions by a person", "set_panel", True,
     [{"op": "set_panel", "page": 1, "frame_id": L1C, "unset": ["regions"]}], HUMAN)
case("set_bible dropping an approved character by an agent", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-yuki", "name": "ユキ"}]}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("set_bible locking a character by an agent", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-new", "name": "新", "locked": True}]}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("set_bible another sheet for an approved character by an agent", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-yuki", "name": "ユキ"},
                                         {"id": "c-ken", "name": "健", "locked": True,
                                          "refs": [{"asset": "$S2", "kind": "sheet", "approved_by": HUMAN},
                                                   {"asset": "$F1", "kind": "face", "approved_by": HUMAN}]}]}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("set_bible a sheet ref of a character not locked by an agent", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-new", "name": "新", "refs": [{"asset": "$S2", "kind": "sheet"}]},
                                         {"id": "c-ken", "name": "健", "locked": True,
                                          "refs": [{"asset": "$S1", "kind": "sheet", "approved_by": HUMAN},
                                                   {"asset": "$F1", "kind": "face", "approved_by": HUMAN}]}]}], AI)
case("set_bible dropping an approved character by a person", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-yuki", "name": "ユキ"}]}], HUMAN)
case("set_bible keeping the approvals by an agent", "set_bible", True,
     [{"op": "set_bible", "plot": "新しい筋", "constraints": [],
       "characters": [{"id": "c-yuki", "name": "ユキ", "refs": [{"asset": "$S2", "kind": "pose"}]},
                      {"id": "c-ken", "name": "健", "locked": True,
                       "refs": [{"asset": "$S1", "kind": "sheet", "approved_by": HUMAN}, {"asset": "$F1", "kind": "face", "approved_by": HUMAN}]}]}],
     AI)
case("set_bible unlocking by an agent", "set_bible", True,
     [{"op": "set_bible", "characters": [{"id": "c-ken", "name": "健", "locked": False,
                                          "refs": [{"asset": "$S1", "kind": "sheet", "approved_by": HUMAN},
                                                   {"asset": "$F1", "kind": "face", "approved_by": HUMAN}]}]}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("upsert_character locking a character by an agent", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-yuki", "name": "ユキ", "locked": True}}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("upsert_character a new one locked by an agent", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-new", "name": "新", "locked": True}}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a character's approval")
case("upsert_character locking a character by a person", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-yuki", "name": "ユキ", "locked": True}}], HUMAN)
# (a sheet or a face on a character that is not locked: a reference until a person approves one, as attach_reference
# gives it — an agent's, as in Python)
case("upsert_character a sheet ref by an agent", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-mio", "name": "ミオ", "refs": [{"asset": "$S2", "kind": "sheet"}]}}], AI)
case("upsert_character a face ref by an agent", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-yuki", "refs": [{"asset": "$F1", "kind": "face"}]}}], AI)
case("upsert_character not locked by an agent", "upsert_character", True,
     [{"op": "upsert_character", "character": {"id": "c-mio", "name": "ミオ", "locked": False, "refs": [{"asset": "$S2", "kind": "pose"}]}}], AI)
case("delete_character approved by an agent", "delete_character", True,
     [{"op": "delete_character", "id": "c-ken", "force": True}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "delete an approved character (c-ken)")
case("delete_character approved by a person", "delete_character", True,
     [{"op": "delete_character", "id": "c-ken", "force": True}], HUMAN)
case("set_ticket on a request for approval by an agent", "set_ticket", True,
     [{"op": "set_ticket", "id": T_GATE_NAME, "status": "done"}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change a request for approval (ticket " + T_GATE_NAME + ")")
case("set_ticket on a request for approval by a person", "set_ticket", True,
     [{"op": "set_ticket", "id": T_GATE_NAME, "status": "done"}], HUMAN)
case("set_ticket on a fix by an agent", "set_ticket", True,
     [{"op": "set_ticket", "id": T_FIX_AGENT, "assignee": "human"}], AI)
case("set_studio unfixing the style by an agent", "set_studio", True,
     [{"op": "set_studio", "style": {"locked": None}}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change the style fixed by the pilot page")
case("set_studio replacing the style by an agent", "set_studio", True,
     [{"op": "set_studio", "style": "plain"}], AI,
     cpp="apply", cpp_says=PERSON_ONLY + "change the style fixed by the pilot page")
case("set_studio style notes by an agent", "set_studio", True,
     [{"op": "set_studio", "style": {"notes": ["線は太め"]}}], AI)
case("set_studio unfixing the style by a person", "set_studio", True,
     [{"op": "set_studio", "style": {"locked": None}}], HUMAN)

json.dump({"note": (
    "Cases for test_contract_studio (studioOpsLikePython): the studio ops of M5 (Python's genko/studio/studio_ops.py, which "
    "ops.apply_ops dispatches its STUDIO_OPS to) and set_bible, add_ticket and set_ticket (ops._apply_one) on the book "
    "`studio_harness.py make-studiobook` makes: the op contract book (pyref_harness.py make-opsbook: page 5 held by ai:other, "
    "a front cover as page 7) with a studio — characters (c-ken's sheet approved and locked), locations, a prop, a script, a "
    "page plan, briefs (page 1: a pinned shot and a prompt override on 000000000028, a person's fix on 000000000029, "
    "regions, a reference, three candidates and the art adopted on 000000000026), requests, sheet candidates from two agents "
    "and a person, tickets of every kind, two proposals, the style fixed from page 6 — and its pictures ($A1 … $G1 PNGs, $J1 a "
    "JPEG, $CUT a PNG cut short after its header, $BAD bytes that are no picture, $CRC a PNG whose header's checksum is wrong, "
    "$BOMB a header too large to open, $JX $BX $GX a JPEG, a BMP and a GIF cut short in their pixels, $PSD $PSDLAB $PSDX "
    "Photoshop files (RGB, Lab, RGB cut short in its merged picture); refs from the book's assets.json). Each case: its steps ({ops, agent?, dry_run?}; or one "
    "step of ops, agent, dry), whether its last step succeeds in Python, with time.strftime fixed (allow_chat_approval). A "
    "case marked \"cpp\" is one this build refuses on purpose (a number Python makes from text, 'inf' or 'nan', that it would "
    "keep; what only a person holds, changed by an agent; a picture placed whose pixels cannot be decoded): C++ must refuse it "
    "with that code, its error holding \"cpp_says\"."),
    "first_id": 0xB00, "clock": "2026-10-11T12:34:56", "cases": cases}, open(sys.argv[1], "w"), ensure_ascii=False, indent=1)
print(len(cases))
