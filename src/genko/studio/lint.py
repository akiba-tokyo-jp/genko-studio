"""Deterministic checks on what an agent writes. Paths are JSON pointers into the input."""

from __future__ import annotations

from pathlib import Path

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
    _unique(issues, bible.get("props") or [], "/props", "小物")
    from genko import fonts

    for kind, look in (bible.get("lettering") or {}).items():
        face = (look or {}).get("font") if isinstance(look, dict) else None
        if face and face not in fonts.BUNDLED and not Path(str(face)).expanduser().is_file():
            issues.append(warning("unknown_font", f"/lettering/{kind}/font", f"書体 {face} が見つからない（台詞の書体になる）",
                                  "inspect target=fonts の bundled の key か、パソコンの書体のパス"))
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
        from genko.studio import fxwords

        known_props = {p.get("id") for p in bible.get("props") or [] if isinstance(p, dict)}
        for pi2, pid in enumerate(panel.get("props") or []):
            if pid not in known_props:
                issues.append(warning("unknown_prop", f"{ppath}/props/{pi2}", f"小物 {pid} が企画書（props）にない",
                                      "企画書の props に足すと、登録した参照画像が絵の依頼に付く"))

        for fi, word in enumerate(panel.get("fx") or []):
            if fxwords.resolve(str(word)) is None:
                issues.append(warning("fx_unknown", f"{ppath}/fx/{fi}", f"効果の言葉「{word}」を Genko は知らない（絵の依頼文には入る。"
                                      "仕上げで Genko は描かない）", "知っている言葉: " + "・".join(fxwords.KNOWN)))
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
    issues.extend(_size_hints(plan, page, page_count))
    if plan.get("title"):
        if not str(bible.get("title") or "").strip():
            issues.append(warning("title_missing", "/title", "扉にするページだが、企画書に題名（title）が無い"))
        if not str(bible.get("author") or "").strip():
            issues.append(warning("author_missing", "/title", "扉にするページだが、企画書に作者名（author）が無い", "set_bible で author を足す"))
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


def _shares(plan: dict) -> dict[str, float]:
    """Each slot's share of the page (from the tier, column and row ratios)."""
    from genko.studio.layout import LayoutError, resolve_tiers

    try:
        tiers = resolve_tiers(plan)
    except LayoutError:
        return {}
    out: dict[str, float] = {}
    total_h = sum(float(t.get("h") or 0) for t in tiers) or 1.0
    for tier in tiers:
        cols = tier.get("cols") or []
        total_w = sum(float(c.get("w") or 0) for c in cols) or 1.0
        for col in cols:
            part = float(tier.get("h") or 0) / total_h * float(col.get("w") or 0) / total_w
            rows = col.get("rows") or []
            if rows:
                total_r = sum(float(r.get("h") or 0) for r in rows) or 1.0
                for row in rows:
                    out[row["slot"]] = part * float(row.get("h") or 0) / total_r
            else:
                out[col["slot"]] = part
    return out


def _size_hints(plan: dict, page: int, page_count: int) -> list[Issue]:
    """Big moments in small panels: the reveal after a page turn, a panel of high emphasis, the book's last panel."""
    from genko.studio.layout import slots_in_order, resolve_tiers, LayoutError

    shares = _shares(plan)
    if not shares:
        return []
    try:
        order = slots_in_order(resolve_tiers(plan))
    except LayoutError:
        return []
    panels = {p.get("slot"): (i, p) for i, p in enumerate(plan.get("panels") or [])}
    out: list[Issue] = []
    if plan.get("turn_role") == "reveal" and order and shares.get(order[0], 1) < 0.3:
        out.append(warning("reveal_small", "/tiers", "めくってすぐの見せ場なのに、最初のコマがページの3割より小さい",
                           "template の reveal_top・reveal_bleed・splash を使うか、最初の段を大きくして bleed（断ち切り）にする"))
    for slot, (i, panel) in panels.items():
        try:
            weight = float(panel.get("emphasis") or 0)
        except (TypeError, ValueError):
            continue
        if weight >= 0.8 and shares.get(slot, 1) < 0.3:
            out.append(warning("emphasis_small", f"/panels/{i}/emphasis", f"強調の高いコマ {slot} がページの3割より小さい",
                               "大きい段に置くか、bleed（断ち切り）や slant（斜め）で目立たせる"))
    if page == page_count and order:
        last = order[-1]
        panel = (panels.get(last) or (None, {}))[1]
        template_bleed = last in (_template(plan).get("bleed") or [])
        if shares.get(last, 1) < 0.35 and not (panel.get("bleed") or template_bleed):
            out.append(warning("finale_small", "/tiers", "最後のページの最後のコマが小さく、断ち切りでもない（余韻が弱い）",
                               "template の finale_bleed を使うか、最後の段を大きくして bleed にする"))
    return out


def _template(plan: dict) -> dict:
    from genko.studio.layout import templates

    return {} if plan.get("tiers") else templates().get(plan.get("template") or "", {})
