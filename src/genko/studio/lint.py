"""Deterministic checks on what an agent writes. Paths are JSON pointers into the input."""

from __future__ import annotations

from genko.studio.issues import Issue, error, warning
from genko.studio.layout import CompiledLayout, check_tiers
from genko.studio.letter import BalloonPlacement
from genko.studio.schemas import TEXT_BEATS

MAX_BALLOON_CHARS = 40
MAX_COLUMN_CHARS = 14
MAX_BALLOONS_PER_BEAT = 3
MAX_SAME_SHOT_RUN = 2
BALLOON_AREA_RATIO = 0.35
MIN_PANEL_SIDE_MM = 25.0
ESTABLISHING = ("ELS", "LS", "FS")


def lint_bible(bible: dict) -> list[Issue]:
    issues: list[Issue] = []
    _unique(issues, bible.get("characters", []), "/characters", "キャラ")
    _unique(issues, bible.get("locations", []), "/locations", "場所")
    chars = bible.get("characters", [])
    for i, a in enumerate(chars):
        for j in range(i + 1, len(chars)):
            b = chars[j]
            la, lb = a.get("look", {}), b.get("look", {})
            if la.get("hair_value") == lb.get("hair_value") and la.get("silhouette") and la.get("silhouette") == lb.get("silhouette"):
                issues.append(
                    warning(
                        "characters_look_alike",
                        f"/characters/{j}/look",
                        f"{a.get('name')} と {b.get('name')} はモノクロで見分けにくい（髪の値とシルエットが同じ）",
                        "hair_value か silhouette を変える",
                    )
                )
    return issues


def lint_script(script: dict, bible: dict, page_count: int) -> list[Issue]:
    issues: list[Issue] = []
    known = {c.get("id") for c in bible.get("characters", [])}
    locations = {loc.get("id") for loc in bible.get("locations", [])}
    seen: set[str] = set()
    pages_used: set[int] = set()
    for si, scene in enumerate(script.get("scenes", [])):
        spath = f"/scenes/{si}"
        if scene.get("location_id") and scene["location_id"] not in locations:
            issues.append(warning("unknown_location", f"{spath}/location_id", f"場所 {scene['location_id']} が企画書にない"))
        for bi, beat in enumerate(scene.get("beats", [])):
            bpath = f"{spath}/beats/{bi}"
            beat_id = beat.get("id", "")
            if beat_id in seen:
                issues.append(error("duplicate_beat_id", f"{bpath}/id", f"beat id {beat_id} が重複している"))
            seen.add(beat_id)
            page = beat.get("page", 0)
            if not 1 <= page <= page_count:
                issues.append(error("page_out_of_range", f"{bpath}/page", f"page {page} は範囲外（1〜{page_count}）"))
            else:
                pages_used.add(page)
            kind = beat.get("kind")
            speaker = beat.get("speaker_id")
            if kind in ("dialogue", "monologue"):
                if not speaker:
                    issues.append(error("speaker_missing", f"{bpath}/speaker_id", "台詞に話者がない"))
                elif speaker not in known:
                    issues.append(error("unknown_speaker", f"{bpath}/speaker_id", f"話者 {speaker} が企画書のキャラにない"))
            if kind in TEXT_BEATS:
                length = len(beat.get("text", ""))
                if length > MAX_BALLOON_CHARS * MAX_BALLOONS_PER_BEAT:
                    issues.append(error("beat_too_long", f"{bpath}/text", f"台詞が {length} 字ある（フキダシ {MAX_BALLOONS_PER_BEAT} つ分の {MAX_BALLOON_CHARS * MAX_BALLOONS_PER_BEAT} 字まで）", "beat を分けるか短くする"))
            if beat.get("reveal") and page % 2 == 1:
                issues.append(warning("reveal_on_odd_page", f"{bpath}/reveal", f"reveal が奇数ページ（{page}）にある", "めくってすぐ見えるのは偶数ページ。偶数ページの先頭に置く"))
    for page in range(1, page_count + 1):
        if page not in pages_used:
            issues.append(warning("page_without_beats", "/scenes", f"{page} ページに beat がない"))
    return issues


def script_index(script: dict) -> dict[str, dict]:
    """beat id → {beat, scene_id, scene_first}."""
    out: dict[str, dict] = {}
    for scene in script.get("scenes", []):
        for bi, beat in enumerate(scene.get("beats", [])):
            out[beat.get("id", "")] = {"beat": beat, "scene_id": scene.get("id"), "scene_first": bi == 0}
    return out


def lint_name_plan(plan: dict, script: dict, bible: dict, page_count: int) -> list[Issue]:
    issues: list[Issue] = []
    page = plan.get("page", 0)
    if not 1 <= page <= page_count:
        return [error("page_out_of_range", "/page", f"page {page} は範囲外（1〜{page_count}）")]
    issues.extend(check_tiers(plan))
    index = script_index(script)
    known = {c.get("id") for c in bible.get("characters", [])}
    page_text_beats = [bid for bid, info in index.items() if info["beat"].get("page") == page and info["beat"].get("kind") in TEXT_BEATS]
    placed: dict[str, list[str]] = {}
    shots: list[str] = []
    sides: dict[str, dict[str, float]] = {}
    for pi, panel in enumerate(plan.get("panels", [])):
        ppath = f"/panels/{pi}"
        shots.append(panel.get("shot", ""))
        for bi, bid in enumerate(panel.get("beat_ids", [])):
            info = index.get(bid)
            if info is None:
                issues.append(error("unknown_beat", f"{ppath}/beat_ids/{bi}", f"beat {bid} が脚本にない"))
            elif info["beat"].get("page") != page:
                issues.append(warning("beat_on_other_page", f"{ppath}/beat_ids/{bi}", f"beat {bid} は脚本では {info['beat'].get('page')} ページ"))
        for ci, char in enumerate(panel.get("characters", [])):
            if char.get("id") not in known:
                issues.append(error("unknown_character", f"{ppath}/characters/{ci}/id", f"キャラ {char.get('id')} が企画書にない"))
        for li, line in enumerate(panel.get("lines", [])):
            lpath = f"{ppath}/lines/{li}"
            bid = line.get("beat_id", "")
            info = index.get(bid)
            if info is None:
                issues.append(error("unknown_beat", f"{lpath}/beat_id", f"beat {bid} が脚本にない"))
                continue
            placed.setdefault(bid, []).append(lpath)
            breaks = line.get("breaks", [])
            text = "".join(breaks)
            if not text.strip():
                issues.append(error("empty_line", f"{lpath}/breaks", "台詞が空"))
            if len(text) > MAX_BALLOON_CHARS:
                issues.append(error("balloon_too_long", f"{lpath}/breaks", f"フキダシ1つに {len(text)} 字（{MAX_BALLOON_CHARS} 字まで）", "フキダシを分ける"))
            for ki, part in enumerate(breaks):
                if len(part) > MAX_COLUMN_CHARS:
                    issues.append(error("column_too_long", f"{lpath}/breaks/{ki}", f"1行が {len(part)} 字（{MAX_COLUMN_CHARS} 字まで）", "breaks で行を分ける"))
            speaker = info["beat"].get("speaker_id")
            if info["beat"].get("kind") == "dialogue" and speaker and panel.get("characters") and speaker not in {c.get("id") for c in panel["characters"]}:
                issues.append(warning("speaker_off_panel", lpath, f"話者 {speaker} がこのコマにいない（フキダシの尾が付かない）"))
        _collect_sides(sides, panel, index)
    for bid in page_text_beats:
        if bid not in placed:
            issues.append(error("beat_not_placed", "/panels", f"台詞の beat {bid} がどのコマにも置かれていない"))
    for bid, paths in placed.items():
        if len(paths) > 1:
            issues.append(error("beat_placed_twice", paths[1], f"beat {bid} が2回置かれている（最初は {paths[0]}）"))
    run = 1
    for i in range(1, len(shots)):
        run = run + 1 if shots[i] == shots[i - 1] else 1
        if run > MAX_SAME_SHOT_RUN:
            issues.append(warning("same_shot_run", f"/panels/{i}/shot", f"同じ shot（{shots[i]}）が {run} コマ続く", "寄りと引きを混ぜる"))
    for pi, panel in enumerate(plan.get("panels", [])):
        first = [bid for bid in panel.get("beat_ids", []) if index.get(bid, {}).get("scene_first")]
        if first and panel.get("shot") not in ESTABLISHING:
            issues.append(warning("scene_without_establishing", f"/panels/{pi}/shot", "場面の最初のコマが状況説明のショット（ELS / LS / FS）でない"))
    for scene_id, positions in sides.items():
        if positions.get("_crossed"):
            issues.append(warning("axis_crossed", "/panels", f"場面 {scene_id} で人物の左右が入れ替わる（180度ルール）", "意図的なら cross:true を付ける"))
    role = plan.get("turn_role")
    if role == "reveal" and page % 2 == 1:
        issues.append(warning("reveal_on_odd_page", "/turn_role", "reveal は偶数ページ（めくってすぐ見えるページ）に置く"))
    if role == "hook" and page % 2 == 0:
        issues.append(warning("hook_on_even_page", "/turn_role", "hook（引き）は奇数ページの最後に置く"))
    return issues


def _collect_sides(sides: dict[str, dict], panel: dict, index: dict[str, dict]) -> None:
    scene_ids = {index[bid]["scene_id"] for bid in panel.get("beat_ids", []) if bid in index}
    chars = panel.get("characters", [])
    if len(scene_ids) != 1 or len(chars) < 2:
        return
    scene = sides.setdefault(scene_ids.pop(), {})
    order = {"left": 0, "left_third": 1, "center": 2, "right_third": 3, "right": 4}
    for c in chars:
        pos = order.get(c.get("pos"), 2)
        prev = scene.get(c.get("id"))
        if prev is not None and not panel.get("cross"):
            for other in chars:
                if other is c or other.get("id") not in scene:
                    continue
                before = prev - scene[other.get("id")]
                now = pos - order.get(other.get("pos"), 2)
                if before * now < 0:
                    scene["_crossed"] = True
    for c in chars:
        scene[c.get("id")] = order.get(c.get("pos"), 2)


def lint_name_geometry(plan: dict, layout: CompiledLayout, placements: list[BalloonPlacement]) -> list[Issue]:
    issues: list[Issue] = []
    panel_path = {p.get("slot"): f"/panels/{i}" for i, p in enumerate(plan.get("panels", []))}
    for slot, (x, y, w, h) in layout.leaf_rects_mm.items():
        path = panel_path.get(slot, "/panels")
        if min(w, h) < MIN_PANEL_SIDE_MM:
            issues.append(warning("panel_too_small", path, f"コマ {slot} が {w:.0f}×{h:.0f} mm（短辺 {MIN_PANEL_SIDE_MM:.0f} mm 未満）", "段の h か列の w を大きくする"))
        area = sum(p.w_mm * p.h_mm for p in placements if p.slot == slot)
        if w * h > 0 and area / (w * h) > BALLOON_AREA_RATIO:
            issues.append(warning("panel_too_wordy", path, f"コマ {slot} の {area / (w * h):.0%} が台詞で埋まる（{BALLOON_AREA_RATIO:.0%} まで）", "台詞を減らすかコマを大きくする"))
    return issues


def _unique(issues: list[Issue], items: list[dict], path: str, label: str) -> None:
    seen: set[str] = set()
    for i, item in enumerate(items):
        key = item.get("id", "")
        if key in seen:
            issues.append(error("duplicate_id", f"{path}/{i}/id", f"{label}の id {key} が重複している"))
        seen.add(key)
