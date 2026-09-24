"""Generation requests (genko.genreq@1): everything an agent hands to its image tool.

Genko generates nothing. It writes studio/requests/<id>/request.json with the
suggested sizes, a prompt draft (ja, en, tags), what not to draw, where to keep
calm for balloons, and files: guides (composition, pose, keepout), copies of
the approved reference images, and for fixes the source image and a mask. The
id is a hash of the content, so asking again for the same thing gives the same
request.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from genko import guide
from genko.assets import AssetStore
from genko.models import Episode, Frame, Page
from genko.studio import tools_registry
from genko.studio.jsonutil import canonical_json, sha256_hex
from genko.studio.studio_ops import brief_hash

TYPE = "genko.genreq@1"
PAD_MM = 3.0
TARGET_PIXELS = 1024 * 1024
PURPOSES = ("panel_art", "draft", "character_sheet", "location")
MODES = ("new", "edit", "inpaint", "upscale")
_VOCAB_PATH = Path(__file__).with_name("vocab.json")


class RequestError(ValueError):
    def __init__(self, message: str, path: str = "/") -> None:
        super().__init__(message)
        self.path = path


@dataclass
class Pack:
    request: dict
    files: dict[str, bytes] = field(default_factory=dict)  # relative name → bytes

    @property
    def id(self) -> str:
        return self.request["id"]


def vocab(expression: str = "mono") -> dict:
    """The phrase book; colour pages get the colour style instead of the monochrome one."""
    v = json.loads(_VOCAB_PATH.read_text(encoding="utf-8"))
    if expression == "color":
        v["style"] = v["style_color"]
        v["avoid"] = [a for a in v["avoid"] if a != "色"]
    return v


# --- sizes ---------------------------------------------------------------------


def suggested_px(aspect: float, pixels: int = TARGET_PIXELS) -> list[int]:
    width = math.sqrt(pixels * aspect)
    height = width / aspect
    return [max(256, int(round(width / 64)) * 64), max(256, int(round(height / 64)) * 64)]


def _aspect_text(aspect: float) -> str:
    return f"{aspect:.2f}:1" if aspect >= 1 else f"1:{1 / aspect:.2f}"


def tool_sizes(aspect: float, sizes: list[list[int]]) -> list[dict]:
    ranked = []
    for w, h in sizes:
        tool_aspect = w / h
        if tool_aspect > aspect * 1.02:
            crop = "左右を中央で切る"
        elif tool_aspect < aspect / 1.02:
            crop = "上下を中央で切る"
        else:
            crop = "そのまま"
        ranked.append((abs(math.log(tool_aspect / aspect)), -w * h, {"px": [w, h], "crop": crop}))
    ranked.sort(key=lambda item: (round(item[0], 6), item[1]))
    return [item[2] for item in ranked[:3]]


def size_block(width_mm: float, height_mm: float, pad_mm: float, dpi: int, tool: dict, mode: str) -> dict:
    box_w, box_h = width_mm + 2 * pad_mm, height_mm + 2 * pad_mm
    aspect = box_w / box_h
    print_px = [round(box_w / 25.4 * dpi), round(box_h / 25.4 * dpi)]
    block = {
        "frame_mm": [round(width_mm, 2), round(height_mm, 2)],
        "pad_mm": pad_mm,
        "aspect": _aspect_text(aspect),
        "suggested_px": suggested_px(aspect),
        "tool_sizes": tool_sizes(aspect, tool["sizes_px"]),
        "print_px": {"dpi": dpi, "px": print_px},
    }
    if mode == "upscale":
        block["suggested_px"] = print_px
    return block


# --- prompt ----------------------------------------------------------------------


def _character_ja(char: dict, outfit_id: str | None, v: dict) -> str:
    look = char.get("look") or {}
    parts = [look.get("build", ""), look.get("hair", "")]
    if look.get("hair_value") in v["hair_value"]:
        parts.append(v["hair_value"][look["hair_value"]]["ja"])
    if look.get("eyes"):
        parts.append(f"目は{look['eyes']}")
    outfits = {o.get("id"): o.get("desc", "") for o in look.get("outfits", [])}
    outfit = outfits.get(outfit_id or "default") or next(iter(outfits.values()), "")
    if outfit:
        parts.append(outfit)
    parts += list(look.get("marks", []))
    desc = "、".join(p for p in parts if p)
    return f"{char.get('name', char.get('id'))}（{desc}）" if desc else str(char.get("name", char.get("id")))


def _character_en(char: dict, v: dict) -> str:
    if char.get("tokens_en"):
        return str(char["tokens_en"])
    look = char.get("look") or {}
    value = v["hair_value"].get(look.get("hair_value"), {}).get("en", "")
    return f"the character '{char.get('id')}' as in the reference sheet" + (f", {value}" if value else "")


def _keepout_sentence(boxes01: list[list[float]]) -> tuple[str, str]:
    if not boxes01:
        return "", ""
    x0 = min(b[0] for b in boxes01)
    y0 = min(b[1] for b in boxes01)
    x1 = max(b[0] + b[2] for b in boxes01)
    y1 = max(b[1] + b[3] for b in boxes01)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    horiz_ja, horiz_en = ("左", "left") if cx < 0.4 else (("右", "right") if cx > 0.6 else ("", ""))
    vert_ja, vert_en = ("上", "upper") if cy < 0.4 else (("下", "lower") if cy > 0.6 else ("", ""))
    where_ja = (horiz_ja + vert_ja) or "中央"  # 右上, 左下 …
    where_en = "-".join(p for p in (vert_en, horiz_en) if p) or "central"
    share = min(0.9, max(0.1, (x1 - x0) * (y1 - y0)))
    tenths = max(1, round(share * 10))
    return (f"{where_ja}の約{tenths}割は台詞用に静かに空けておく。",
            f"Keep the {where_en} area (about {tenths * 10}% of the image) calm and empty for speech balloons.")


def panel_prompt(episode: Episode, page: Page, frame: Frame, keep01: list[list[float]], instruction: str | None) -> tuple[dict, list[str]]:
    v = vocab(page.spec.expression)
    panel = frame.panel or {}
    studio = episode.studio
    doc = studio.get("bible_doc") or {}
    chars = {c.get("id"): c for c in episode.bible.characters}
    notes: list[str] = []
    ja: list[str] = [v["style"]["ja"]]
    en: list[str] = [v["style"]["en"]]
    tags: list[str] = [v["style"]["tags"]]
    style_notes = (studio.get("style") or {}).get("notes") or (doc.get("style") or {}).get("notes") or []
    ja += [str(n).rstrip("。") + "。" for n in style_notes]
    # fixed look of each character, as written (never paraphrased)
    cast = [c for c in panel.get("characters", []) if isinstance(c, dict)]
    for c in cast:
        char = chars.get(c.get("id"), {"id": c.get("id")})
        ja.append(_character_ja(char, c.get("outfit"), v) + "。")
        en.append(_character_en(char, v) + ".")
        value = v["hair_value"].get((char.get("look") or {}).get("hair_value"), {}).get("tags")
        if value:
            tags.append(value)
    shot = v["shot"].get(panel.get("shot", ""), {})
    angle = v["angle"].get(panel.get("angle", ""), {})
    if shot or angle:
        ja.append("、".join(p for p in (angle.get("ja"), shot.get("ja")) if p) + "の構図。")
        en.append(", ".join(p for p in (angle.get("en"), shot.get("en")) if p) + ".")
        tags += [t for t in (shot.get("tags"), angle.get("tags")) if t]
    location = _location(episode, panel.get("location_id"))
    time = v["time"].get(panel.get("time") or "", {})
    if location:
        ja.append(f"場所は{location.get('name', location.get('id'))}（{location.get('desc', '')}）" + (f"、{time['ja']}" if time else "") + "。")
        en.append(f"Setting: {location.get('name_en') or location.get('name', location.get('id'))}" + (f", {time['en']}" if time else "") + ".")
        if time:
            tags.append(time["tags"])
    for c in cast:
        char = chars.get(c.get("id"), {})
        name = char.get("name", c.get("id"))
        pos = v["pos"].get(c.get("pos", ""), {})
        facing = v["facing"].get(c.get("facing", ""), {})
        bits = [p for p in (pos.get("ja"), facing.get("ja"), c.get("pose"), c.get("expression")) if p]
        if bits:
            ja.append(f"{name}: " + "、".join(bits) + "。")
        en_bits = [p for p in (pos.get("en"), facing.get("en")) if p]
        if en_bits:
            en.append(f"{c.get('id')}: " + ", ".join(en_bits) + ".")
        if facing.get("tags"):
            tags.append(facing["tags"])
    if panel.get("action"):
        ja.append(str(panel["action"]) + "。")
    if panel.get("emotion"):
        ja.append(f"感情: {panel['emotion']}。")
    keep_ja, keep_en = _keepout_sentence(keep01)
    if keep_ja:
        ja.append(keep_ja)
        en.append(keep_en)
    ja += [str(c).rstrip("。") + "。" for c in episode.bible.constraints]
    plan = page.plan or {}
    if plan.get("turn_role") == "reveal":
        ja.append("めくってすぐの見せ場のコマ。")
    elif plan.get("turn_role") == "hook":
        ja.append("次のページへ引く最後のコマ。")
    if panel.get("memo"):
        ja.append(str(panel["memo"]))
    human = panel.get("instruction")
    if isinstance(human, dict) and human.get("text"):
        ja.append(f"指示: {human['text']}")
    elif isinstance(human, str) and human:
        ja.append(f"指示: {human}")
    if instruction:
        ja.append(f"今回の指示: {instruction}")
    if not all(chars.get(c.get("id"), {}).get("tokens_en") for c in cast):
        notes.append("英語の見た目の記述（tokens_en）が無い人物がいる。英語のプロンプトは参照画像と日本語から補う")
    prompt = {"ja": "".join(ja), "en": " ".join(en), "tags": ", ".join(dict.fromkeys(t for t in tags if t))}
    override = (panel.get("gen") or {}).get("prompt_override")
    if override:
        prompt = override if isinstance(override, dict) else {"ja": str(override), "en": str(override), "tags": ""}
        notes.append("プロンプトは人間の指定なので変えない")
    return prompt, notes


def _location(episode: Episode, location_id: str | None) -> dict | None:
    if not location_id:
        return None
    for item in episode.studio.get("locations", []):
        if item.get("id") == location_id:
            base = next((loc for loc in (episode.studio.get("bible_doc") or {}).get("locations", []) if loc.get("id") == location_id), {})
            return {**base, **item}
    for item in (episode.studio.get("bible_doc") or {}).get("locations", []):
        if item.get("id") == location_id:
            return item
    return None


def _avoid(episode: Episode, char_ids: list[str], panel: dict | None, expression: str = "mono") -> list[str]:
    v = vocab(expression)
    override = ((panel or {}).get("gen") or {}).get("avoid_override")
    if override:
        return [str(x) for x in override]
    out = list(v["avoid"])
    chars = {c.get("id"): c for c in episode.bible.characters}
    for cid in char_ids:
        for item in chars.get(cid, {}).get("never", []) or []:
            out.append(f"{item}（{cid}）")
    return out


# --- building ------------------------------------------------------------------------


def _page_frame(episode: Episode, target: dict) -> tuple[Page, Frame]:
    page = None
    if target.get("page_id"):
        page = next((p for p in episode.pages if p.id == target["page_id"]), None)
    elif target.get("page") is not None:
        page = next((p for p in episode.pages if p.index == int(target["page"])), None)
    if page is None:
        raise RequestError(f"ページ {target.get('page') or target.get('page_id')} はない", "/page")
    try:
        frame = page._find(str(target.get("frame_id") or ""))
    except (KeyError, IndexError):
        raise RequestError(f"{page.index} ページにコマ {target.get('frame_id')} はない", "/frame_id") from None
    if frame.children:
        raise RequestError("コマ（葉）を指定する", "/frame_id")
    return page, frame


def _asset(store: AssetStore, ref: str) -> bytes | None:
    return store.get_bytes(ref, ".png") if isinstance(ref, str) and ref.startswith("sha256:") else None


def _candidate(episode: Episode, frame: Frame | None, candidate_id: str | None) -> dict | None:
    if not candidate_id:
        return None
    pools = [(frame.panel or {}).get("candidates", [])] if frame is not None else []
    for bucket in ("character_candidates", "location_candidates"):
        pools += list(episode.studio.get(bucket, {}).values())
    for pool in pools:
        for cand in pool:
            if cand.get("id") == candidate_id:
                return cand
    return None


def build(episode: Episode, project: Path, *, purpose: str = "panel_art", mode: str = "new", page: int | None = None,
          frame_id: str | None = None, character_id: str | None = None, location_id: str | None = None,
          parent: str | None = None, tool: str | None = None, instruction: str | None = None,
          regions: list | None = None, focus_character: str | None = None) -> Pack:
    if purpose not in PURPOSES:
        raise RequestError(f"purpose は {', '.join(PURPOSES)} のどれか", "/purpose")
    if mode not in MODES:
        raise RequestError(f"mode は {', '.join(MODES)} のどれか", "/mode")
    locked = (episode.studio.get("style") or {}).get("locked") or None
    from_lock = tool is None and bool(locked and locked.get("tool"))
    if from_lock:
        tool = locked["tool"]  # the pilot page fixed the image tool
    tool_id, tool_spec = tools_registry.get(tool)
    if from_lock and tool_id is None:
        tool_id = tool  # fixed by the pilot page even when tools.json does not describe it
    store = AssetStore(project)
    files: dict[str, bytes] = {}
    notes: list[str] = []
    steps: list[str] | None = None
    dpi = episode.spec.dpi or 600
    expression = episode.spec.expression
    if purpose in ("panel_art", "draft"):
        pg, frame = _page_frame(episode, {"page": page, "frame_id": frame_id})
        expression = pg.spec.expression
        panel = frame.panel or {}
        box_size = size_block(frame.rect.width, frame.rect.height, PAD_MM, dpi, tool_spec, mode)
        # guides are drawn at the ~1 MP size, also for upscale requests (whose suggested size is the print size)
        box = guide.GenBox.for_frame(frame, PAD_MM, tuple(suggested_px((frame.rect.width + 2 * PAD_MM) / (frame.rect.height + 2 * PAD_MM))))
        keep = guide.keepout_boxes(episode, pg, frame)
        keep01 = [{"box01": box.box01(rect), "why": why} for rect, why in keep]
        prompt, more = panel_prompt(episode, pg, frame, [k["box01"] for k in keep01], instruction)
        notes += more
        cast = [c.get("id") for c in panel.get("characters", []) if isinstance(c, dict)]
        target = {"page_id": pg.id, "page": pg.index, "frame_id": frame.id, "label": f"{pg.index}-{panel.get('slot') or frame.id}"}
        files["guides/composition.png"] = guide.to_png(guide.composition(pg, frame, box))
        if cast or guide.has_mannequin(pg, frame):
            files["guides/pose.png"] = guide.to_png(guide.pose(pg, frame, box))
            if guide.has_mannequin(pg, frame):
                notes.append("guides/pose.png のマネキンがポーズの指定。体の向きと手足の角度を合わせる")
        if keep:
            files["guides/keepout.png"] = guide.to_png(guide.keepout(episode, pg, frame, box))
        figures = [{"char": f.char_id, "head01": box.box01(f.head), "body01": box.box01(f.body),
                    "head_mm": [round(v, 2) for v in f.head], "body_mm": [round(v, 2) for v in f.body]}
                   for f in guide.figures_for(frame)]
        if focus_character:
            if focus_character not in cast:
                raise RequestError(f"{focus_character} はこのコマにいない", "/focus_character")
            if mode == "new":
                raise RequestError("focus_character は直し（mode edit / inpaint）で使う", "/focus_character")
            name = next((c.get("name") for c in episode.bible.characters if c.get("id") == focus_character), focus_character)
            prompt = {**prompt,
                      "ja": f"直すのは{name}だけ。ほかの人物と背景は元の画像のまま変えない。{name}は参照画像（設定画・顔）に合わせる。" + prompt["ja"],
                      "en": f"Only change {focus_character}; keep everything else exactly as in the source image. "
                            f"Match {focus_character} to the reference sheet and face. " + prompt["en"]}
            refs = _panel_refs(episode, store, panel, [focus_character], files)
            if mode == "inpaint" and not regions:
                regions = [f"person:{focus_character}", f"face:{focus_character}"]
        else:
            refs = _panel_refs(episode, store, panel, cast, files)
        if len(cast) >= 2 and mode == "new":
            steps = [
                "1. このパックで構図全体（全員）を生成して取り込む",
                "2. 採用候補を決めたら report_regions で人物ごとの領域を報告する",
                "3. 似ていない人物ごとに generation_request（mode inpaint、parent、focus_character）で直す",
            ]
            notes.append("複数人物のコマ: まず全体を作り、似ていない人物だけを一人ずつ inpaint で直す（steps を参照）")
        if locked and locked.get("reference") and locked.get("page") != pg.index and purpose == "panel_art":
            data = _asset(store, locked["reference"])
            if data is not None:
                files["refs/style_pilot.png"] = data
                refs.append("refs/style_pilot.png")
                notes.append(f"スタイルは {locked['page']} ページで固定済み。refs/style_pilot.png の絵柄・線の太さ・トーンに合わせる"
                             + (f"。画像ツールは {locked['tool']} を使う" if locked.get("tool") else ""))
        b_hash = brief_hash(panel)
        avoid = _avoid(episode, cast, panel, pg.spec.expression)
        characters = [{"id": cid, "tokens_en": next((c.get("tokens_en") for c in episode.bible.characters if c.get("id") == cid), None),
                       "files": [r for r in refs if r.startswith(f"refs/{cid}_")]} for cid in cast]
    elif purpose == "character_sheet":
        char = next((c for c in episode.bible.characters if c.get("id") == character_id), None)
        if char is None:
            raise RequestError(f"登場人物 {character_id} はない", "/character_id")
        v = vocab(episode.spec.expression)
        box_size = size_block(160.0, 240.0, 0.0, dpi, tool_spec, mode)
        box_size["frame_mm"] = None
        prompt = {
            "ja": v["style"]["ja"] + "キャラクター設定画。" + _character_ja(char, None, v) +
                  "。同じ人物の正面・横・後ろの全身と、顔のアップ。無地の背景、ポーズは自然に立つ。",
            "en": v["style"]["en"] + " Character model sheet: " + _character_en(char, v) +
                  ". Front, side and back full-body views of the same person, plus a face close-up. Plain background.",
            "tags": v["style"]["tags"] + ", character sheet, multiple views, full body, simple background",
        }
        target = {"character_id": char["id"], "label": f"sheet:{char['id']}"}
        cast = [char["id"]]
        keep01, figures, b_hash = [], [], None
        avoid = _avoid(episode, cast, None, episode.spec.expression)
        refs = []
        for ref in char.get("refs", []):
            data = _asset(store, ref.get("asset", ""))
            if data is not None and ref.get("kind") not in ("sheet", "face"):
                name = f"refs/{char['id']}_{ref.get('kind', 'ref')}_{ref['asset'][7:15]}.png"
                files[name] = data
                refs.append(name)
        characters = [{"id": char["id"], "tokens_en": char.get("tokens_en"), "files": refs}]
        notes.append("顔が正面を向いたアップを必ず入れる（承認時に顔の参照として切り出す）")
    else:  # location
        location = _location(episode, location_id)
        if location is None:
            raise RequestError(f"場所 {location_id} はない", "/location_id")
        v = vocab(episode.spec.expression)
        box_size = size_block(240.0, 160.0, 0.0, dpi, tool_spec, mode)
        box_size["frame_mm"] = None
        prompt = {
            "ja": v["style"]["ja"] + f"背景美術: {location.get('name', location['id'])}（{location.get('desc', '')}）。人物は描かない。",
            "en": v["style"]["en"] + f" Background art of {location.get('name_en') or location.get('name', location['id'])}, no people.",
            "tags": v["style"]["tags"] + ", scenery, background, no humans",
        }
        target = {"location_id": location["id"], "label": f"location:{location['id']}"}
        cast, keep01, figures, b_hash, refs, characters = [], [], [], None, [], []
        avoid = _avoid(episode, [], None, episode.spec.expression)
        for ref in location.get("refs", []):
            data = _asset(store, ref.get("asset", ""))
            if data is not None:
                name = f"refs/{location['id']}_{ref['asset'][7:15]}.png"
                files[name] = data
                refs.append(name)
    source = mask = None
    if mode in ("edit", "inpaint", "upscale"):
        frame = _page_frame(episode, {"page": page, "frame_id": frame_id})[1] if purpose in ("panel_art", "draft") else None
        if not parent and frame is not None and mode == "upscale":
            parent = ((frame.panel or {}).get("adopted") or {}).get("art")
        cand = _candidate(episode, frame, parent)
        if cand is None:
            raise RequestError(f"mode {mode} には元の候補（parent）が要る", "/parent")
        data = _asset(store, cand["asset"])
        if data is None:
            raise RequestError(f"候補 {parent} の画像が assets/ にない", "/parent")
        files["source.png"] = data
        source = "source.png"
        if mode == "inpaint":
            if frame is None:
                raise RequestError("inpaint はコマの候補だけ", "/mode")
            rects = _mask_rects(frame, regions)
            if not rects:
                raise RequestError("inpaint には描き直す領域（regions: 領域 id、\"face:<人物>\"、rect_mm）が要る。"
                                   "人物の領域は先に report_regions で報告する", "/regions")
            px = tuple(cand.get("px") or box_size["suggested_px"])
            pad = (cand.get("mapping") or {}).get("pad_mm", PAD_MM)
            files["mask.png"] = guide.to_png(guide.mask(guide.GenBox.for_frame(frame, pad, px), rects))
            mask = "mask.png"
        if mode == "upscale":
            notes.append("画像ツールの高解像度化で source.png を拡大する。無ければ record_review kind=upscale で理由を残す")
    if not tool_spec["supports"].get("references", True):
        notes.append("このツールは参照画像を渡せない。prompt だけで作り、取り込み時に refs_used を空にする")
    if mask and not tool_spec["supports"].get("mask", True):
        notes.append("このツールはマスクを使えない。mode edit で指示だけ渡す")
    notes += [
        "composition と pose は構図の参考。線をなぞらせる必要はない",
        "画像に文字・フキダシ・効果音を描かせない。台詞は Genko が描く",
    ]
    request: dict[str, Any] = {
        "type": TYPE,
        "purpose": purpose,
        "mode": mode,
        "target": target,
        "parent": parent,
        "tool": tool_id,
        "brief_hash": b_hash,
        "size": box_size,
        "prompt": prompt,
        "color": expression == "color",
        "avoid": avoid,
        "keepout": keep01,
        "figures": figures,
        "characters": characters,
        "focus_character": focus_character,
        "steps": steps,
        "files": {
            "composition": "guides/composition.png" if "guides/composition.png" in files else None,
            "pose": "guides/pose.png" if "guides/pose.png" in files else None,
            "keepout": "guides/keepout.png" if "guides/keepout.png" in files else None,
            "references": sorted(r for r in files if r.startswith("refs/")),
            "source": source,
            "mask": mask,
        },
        "order_of_instructions": ["共通制約", "ページ", "コマのメモ", "今回の指示"],
        "notes_for_agent": notes,
        "instruction": instruction,
    }
    digest = sha256_hex(canonical_json({"request": request, "files": {k: sha256_hex(v) for k, v in sorted(files.items())}}))
    request_id = "rq_" + digest[:10]
    request = {"type": TYPE, "id": request_id, **{k: v for k, v in request.items() if k != "type"}}
    request["import"] = {"tool": "import_images", "request_id": request_id, "inbox": f"studio/inbox/{request_id}/"}
    request["notes_for_agent"] = notes + [f"画像は studio/inbox/{request_id}/ に置いてから import_images を呼ぶ（別マシンなら POST /v1/assets）"]
    return Pack(request, files)


def _panel_refs(episode: Episode, store: AssetStore, panel: dict, cast: list[str], files: dict[str, bytes]) -> list[str]:
    names: list[str] = []
    chars = {c.get("id"): c for c in episode.bible.characters}
    for cid in cast:
        for ref in chars.get(cid, {}).get("refs", []):
            if ref.get("kind") not in ("face", "sheet"):
                continue
            data = _asset(store, ref.get("asset", ""))
            if data is not None:
                name = f"refs/{cid}_{ref['kind']}.png"
                files[name] = data
                names.append(name)
    location = _location(episode, panel.get("location_id"))
    for ref in (location or {}).get("refs", []):
        data = _asset(store, ref.get("asset", ""))
        if data is not None:
            name = f"refs/{location['id']}_{ref['asset'][7:15]}.png"
            files[name] = data
            names.append(name)
    for ref in sorted(panel.get("refs", []), key=lambda r: r.get("order", 0)):
        source = ref.get("source") or {}
        data = _asset(store, source.get("asset", "")) if source.get("kind") == "asset" else None
        if data is not None:
            name = f"refs/{ref.get('id')}_{ref.get('role', 'ref')}.png"
            files[name] = data
            names.append(name)
    return names


def _mask_rects(frame: Frame, regions: list | None) -> list[tuple[float, float, float, float]]:
    """Regions to redraw: region ids, "face:<char>" / "person:<char>" (reported regions), or rect_mm lists."""
    all_regions = (frame.panel or {}).get("regions", [])
    by_id = {r.get("id"): r for r in all_regions}
    out = []
    for item in regions or []:
        if isinstance(item, str) and ":" in item and item not in by_id:
            kind, char = item.split(":", 1)
            kinds = ("face", "head") if kind == "face" else ("person", "body")
            out += [tuple(float(v) for v in r["rect_mm"]) for r in all_regions
                    if r.get("kind") in kinds and r.get("char") == char and r.get("rect_mm")]
        elif isinstance(item, str) and item in by_id and by_id[item].get("rect_mm"):
            out.append(tuple(float(v) for v in by_id[item]["rect_mm"]))
        elif isinstance(item, (list, tuple)) and len(item) == 4:
            out.append(tuple(float(v) for v in item))
    return out


def write(pack: Pack, project: Path) -> Path:
    """studio/requests/<id>/ with request.json and its files, plus the inbox folder. Idempotent."""
    from genko.studio.jsonutil import atomic_write_text

    folder = Path(project) / "studio" / "requests" / pack.id
    for name, data in pack.files.items():
        path = folder / name
        if path.is_file() and path.read_bytes() == data:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_bytes(data)
        tmp.replace(path)
    atomic_write_text(folder / "request.json", json.dumps(pack.request, ensure_ascii=False, indent=2) + "\n")
    (Path(project) / "studio" / "inbox" / pack.id).mkdir(parents=True, exist_ok=True)
    return folder


def read(project: Path, request_id: str) -> dict | None:
    if not request_id.startswith("rq_") or "/" in request_id or "\\" in request_id or ".." in request_id:
        return None
    path = Path(project) / "studio" / "requests" / request_id / "request.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def record(pack: Pack) -> dict:
    """The open_request op body for this pack (what project.json keeps about it)."""
    req = pack.request
    target = dict(req["target"])
    return {
        "id": req["id"],
        "purpose": req["purpose"],
        "mode": req["mode"],
        "target": {k: target[k] for k in ("page", "page_id", "frame_id", "character_id", "location_id") if k in target},
        "brief_hash": req["brief_hash"],
        "parent": req["parent"],
        "tool": req["tool"],
        "pad_mm": req["size"]["pad_mm"],
    }
