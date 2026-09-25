"""The words people see (Qt-free): stages, layers, panel states, actors and error messages.

Internal ids and op names stay in the data; the screens show these instead.
"""

from __future__ import annotations

import re

STAGE = {"name": "ネーム", "ink": "作画", "finish": "仕上げ"}

LAYER = {
    "name": "ネーム", "draft": "下描き・アタリ", "ink": "ペン入れ", "bg": "背景", "finish": "仕上げ",
    "tone": "トーン", "effect": "効果", "frames": "コマ枠", "text": "文字", "user": "レイヤー",
}

PANEL = {
    None: "未着手", "empty": "未着手", "briefed": "指示あり", "requested": "依頼済み", "candidates": "候補あり",
    "adopted": "採用済み", "fix_requested": "直し待ち", "skip": "絵なし",
}

GATE = {"name": "ネーム", "art": "作画", "sheet": "設定画", "export": "書き出し"}

REGION = [("face", "顔"), ("person", "人物"), ("keep", "空けておく")]
REGION_LABEL = dict(REGION)

BLEND = [("normal", "通常"), ("multiply", "乗算"), ("screen", "スクリーン"), ("add", "加算")]

FILTERS = [("blur", "ぼかし"), ("sharpen", "シャープ"), ("levels", "レベル補正"), ("mosaic", "モザイク")]


def actor(name: str | None) -> str:
    """ai:hermes → エージェント（hermes）, human:leaf → leaf."""
    name = str(name or "")
    if name.startswith("ai:"):
        return f"エージェント（{name[3:]}）"
    if name.startswith("human:"):
        return name[6:]
    return name or "不明"


def layer_label(layer) -> str:
    title = (layer.title or "").strip()
    role = getattr(layer.role, "value", str(layer.role))
    base = LAYER.get(role, role)
    kind = getattr(layer.kind, "value", "")
    if kind == "placed":
        base = "絵（配置）"
    # titles made of ids (placed art) are not names people gave
    if title and not re.fullmatch(r"(art|ink|placed)?\s*[0-9a-f]{8,}|placed (art|bg|draft|name)|layer", title) and title != role:
        return title
    return base


# --- errors -------------------------------------------------------------------------------------

# names of things in messages (op fields) as people call them
_FIELDS = {
    "kind": "種類", "balloon": "フキダシの種類", "align": "揃え", "position": "ノンブルの位置", "font": "書体", "pattern": "トーンの模様",
    "preset": "見本", "handle": "動かす所", "count": "数", "lpi": "線数", "density": "濃さ", "size_mm": "大きさ",
    "hidden_size_mm": "隠しノンブルの大きさ", "start": "始まりの番号", "copies": "写しの数", "ratio": "縦横の比", "jitter": "ばらつき",
    "depth": "トゲの長さ", "length": "長さ", "fill": "フキダシの塗り", "wrap": "縦書き・横書き", "axis": "割る向き", "to": "送り先",
    "space": "座標の取り方", "start_side": "1 ページ目の側", "style": "文字の設定", "id": "対象", "index": "番号", "op": "操作",
    "text": "文字", "frame_id": "コマ", "page": "ページ", "order": "並び順", "matrix": "変形", "area": "範囲", "points": "点",
    "gradient start": "グラデーションの始まり", "gradient end": "グラデーションの終わり", "gradient shape": "グラデーションの形",
}
_THINGS = {
    "page": "ページ", "panel": "コマ", "layer": "レイヤー", "line": "台詞", "ruler": "定規", "effect": "効果線", "tone": "トーン",
    "ticket": "依頼", "material": "素材", "mannequin": "デッサン人形", "3D figure or box": "3D", "stroke": "線", "frame": "コマ",
}


def _field(name: str) -> str:
    return _FIELDS.get(name.strip(), name.strip())


_ERRORS: list[tuple[str, object]] = [
    (r"brush (\S+) must be between (\S+) and (\S+)", lambda m: f"ブラシの{ {'width_mm': '太さ', 'min_pressure': '弱い筆圧での太さ', 'gamma': '筆圧の効き方', 'opacity': '不透明度', 'stabilize': '手ぶれ補正'}.get(m.group(1), m.group(1)) }は {m.group(2)}〜{m.group(3)} の間で決めます"),
    (r"brush texture must be none, grain, soft or dry", "ブラシの質感は「なし・鉛筆・筆・エアブラシ」から選びます"),
    (r"a brush needs a name", "ブラシに名前を付けます"),
    (r"a brush of one's own has a key starting with my_", "自作のブラシの名前（key）は my_ で始めます"),
    (r"the area has no size", "選んだ範囲に大きさがありません"),
    (r"perspective takes four corners.*", "遠近の変形は 4 隅（左上・右上・右下・左下）で指定します"),
    (r"the four corners must enclose an area", "4 隅が一直線に並んでいて、形になりません"),
    (r"mesh takes nine points.*", "メッシュの変形は 3×3 の 9 点で指定します"),
    (r"a warp is perspective.*", "自由変形は遠近（4 隅）かメッシュ（9 点）で指定します"),
    (r"the transform stretches the area too far", "引き伸ばしすぎです。点を近づけます"),
    (r"a folder cannot be duplicated", "フォルダは複製できません（中のレイヤーを選んで複製します）"),
    (r"there is no layer below to merge into", "下にレイヤーがないので結合できません"),
    (r"placed images and tones cannot be merged", "配置した画像とトーンのレイヤーは結合できません"),
    (r"this layer cannot take a mask \(a folder or a placed image\)", "フォルダと配置した画像にはマスクを付けられません"),
    (r"fill must be show or hide", "マスクは「全部見せる」か「全部隠す」で作ります"),
    (r"points needs at least one \[x_mm, y_mm\] pair", "点が 1 つもありません"),
    # undo, pages, layers
    (r"nothing to undo", "これ以上戻せません"),
    (r"nothing to redo", "やり直せる操作はありません"),
    (r"cannot delete the last page", "最後の 1 ページは削除できません"),
    (r"cannot merge the root frame", "ページ全体のコマは結合できません（隣のコマと結合するには、割ったコマを選びます）"),
    (r"cannot delete core layer", "基本のレイヤーは削除できません"),
    (r"page has no frames", "このページにはコマがありません"),
    (r"can only split a leaf frame", "割れるのは、まだ割っていないコマだけです"),
    (r"can only resize a leaf frame", "大きさを変えられるのは、割っていないコマだけです"),
    (r"only a panel \(not a split\) takes a shape", "形を変えられるのはコマだけです（割った線の親は変えられません）"),
    (r"frame_id must be a split.*", "コマの間（割った所）を選びます"),
    (r"the cut is too short", "割る線が短すぎます。コマを横切るように引きます"),
    (r"the cut does not cross the panel", "割る線がコマを横切っていません"),
    (r"the gutter would leave a panel too small", "それ以上動かすとコマが小さくなりすぎます"),
    (r"a balloon outline needs at least three points", "フキダシの形は 3 点以上で囲みます"),
    (r"the balloon outline is too small", "描いたフキダシが小さすぎます。もう少し大きく囲みます"),
    (r"path must be \[\[x_mm, y_mm\], \.\.\.\]", "フキダシの形は [[x, y], …]（mm）で指定します"),
    (r"emphasis_runs must be a list of the words that carry dots", "傍点は、付ける言葉のリストで指定します"),
    (r"latin must be rotate or upright", "欧文の組み方は「寝かせる」か「立てる」です"),
    (r"emphasis_mark must be sesame or dot", "傍点の形は「ゴマ」か「黒丸」です"),
    (r"skew_deg must be between -60 and 60", "傾きは -60°〜60° の間で指定します"),
    (r"arc must be between -1 and 1", "弓なりは -1〜1 の間で指定します"),
    (r"no gutter there", "そこにはコマの間がありません"),
    (r"p0 and p1 are \[x, y\] in mm", "割る線の両端を指定します"),
    (r"a shape needs at least three corners.*|an area needs at least three corners", "範囲には 3 つ以上の角が要ります"),
    (r"area is .*", "範囲の指定が正しくありません"),
    (r"the area is empty", "範囲が空です"),
    (r"the layer is locked", "このレイヤーはロックされています（レイヤー パネルでロックを外します）"),
    (r"this layer cannot (be painted on|take pen lines).*", "このレイヤーには描けません。ペン・ペイント・トーンのレイヤーを選びます"),
    (r"layer not found|layer id or role required", "レイヤーが見つかりません"),
    (r"that layer is not a tone", "トーンのレイヤーを選びます"),
    (r"layer (.+) (already )?exists", "同じ名前のレイヤーがすでにあります"),
    (r"unknown layer (.+)", "そのレイヤーはありません"),
    (r"kind must be pen, paint or folder", "レイヤーの種類は ペン・ペイント・フォルダ のどれかです"),
    # drawing, filling, selections
    (r"nothing to fill there.*", "線の上なので塗れません。線で囲まれた内側をクリックします"),
    (r"flood_fill missed the page", "ページの外は塗れません"),
    (r"no line there", "その範囲に線がありません"),
    (r"nothing to paste", "貼り付けるものがありません（先にコピーします）"),
    (r"the transform squashes the selection flat", "つぶれてしまう変形はできません"),
    (r"matrix is .*", "変形の指定が正しくありません"),
    (r"points needs.*", "線には 2 つ以上の点が要ります"),
    (r"stroke index out of range", "その線はありません"),
    (r"a stroke cannot cross the gutter.*", "見開きの綴じ目をまたぐ線は引けません"),
    (r"space spread needs a page with spread_with", "見開きにしたページでだけ使えます"),
    (r"put_raster needs path or png_base64|lt_convert needs a raster.*", "画像がありません"),
    (r"image too large.*", "画像が大きすぎます"),
    (r"not a readable image.*", "読み込めない画像です"),
    (r"the picture file of this material is missing", "この素材の画像ファイルが見つかりません"),
    (r"unknown material kind .*", "この素材は使えません"),
    (r"built-in materials cannot be deleted", "最初から入っている素材は消せません"),
    (r"a folder needs a name|a material needs a name", "名前を入れます"),
    # approvals and people (studio books)
    (r"page \d+: the name is approved.*", "このページのネームは承認済みなので、コマ割りを変えられません。変えるには承認を取り消します"),
    (r".*needs name_ok.*|ink strokes require name_ok|.*requires name_ok",
     "エージェントと進める原稿では、ネームの承認の後でペン入れできます（一人の原稿には、この制限はありません）"),
    (r".*finish needs the art approved.*", "作画が承認されてから仕上げに進めます"),
    (r".*needs a person.*cannot approve.*", "承認は人だけが行えます"),
    (r".* is handled with the actor in apply_ops", "この操作はここではできません"),
    (r"cannot lock page .*", "このページは作業中にできません"),
    (r"page (\d+) locked by (.+?)(;.*)?$", r"\1 ページは \2 が作業中です"),
    (r"project locked: .*", "この原稿は、ほかの Genko かエージェントが作業中です。少し待ってからやり直します"),
    (r"frame .* has placed art.*|panels under .* have placed art.*", "このコマには絵が置かれています。絵をどかしてから操作します"),
    (r"the latest change is by (.+?);.*", r"直前の変更は \1 のものなので戻せません"),
    (r"this (undo|redo) changes approvals.*", "承認が変わる操作は、承認・取り消しの画面から行います"),
    (r"project.json changed outside the journal.*", "原稿が外で書き換えられたため、戻せません"),
    (r"the project did not exist before this change", "これより前には戻せません"),
    (r"snapshot .* is missing .*", "戻すための記録が見つかりません"),
    (r"revision conflict.*", "原稿がほかで変わっています。開き直してからやり直します"),
    (r"add_line with frame_id needs explicit x_mm/y_mm.*", "台詞の位置を指定します"),
    # paper, rulers, 3D, tones, effects, nombre
    (r"the paper must hold the finished size and its bleed", "用紙が、仕上がりと裁ち落としより小さくなっています"),
    (r"the basic frame must fit inside the finished size", "基本枠が仕上がりに収まりません"),
    (r"sizes must be positive", "寸法は 0 より大きくします"),
    (r"a (\w+) ruler needs (\d+) point\(s\)", r"この定規には点が \2 つ要ります"),
    (r"a perspective ruler has 1 to 3 vanishing points", "パース定規の消失点は 1〜3 つです"),
    (r"ratio must be above 0", "縦横の比は 0 より大きくします"),
    (r"no 3D figure or box to trace", "このページに 3D がありません"),
    (r"kind must be box.*", "置ける 3D は箱とデッサン人形です"),
    (r"too many lines.*", "線が多すぎます（2000 本まで）"),
    (r"lpi is 5 to 300", "線数は 5〜300 です"),
    (r"density is 0 to 1.*", "濃さは 0〜100% です"),
    (r"count is 1 to 200", "一度に足せるのは 200 ページまでです"),
    (r"style must be an object", "文字の設定の指定が正しくありません"),
    (r"unknown style key .*", "文字の設定に知らない項目があります"),
    (r"style (\w+): .*", lambda m: f"文字の設定（{_field(m.group(1))}）が正しくありません"),
    (r"unknown op: .*", "この版の Genko にない操作です"),
    (r"ops must be a JSON array|ops\[\d+\] must be an object", "操作の指定が正しくありません"),
    (r"order must .*", "並び順の指定が正しくありません"),
    (r"no page (\d+)", r"\1 ページはありません"),
    (r"a tail needs to: .*", "しっぽの先の位置を指定します"),
    (r"gradient \S+ is a density from 0 to 1", "グラデーションの濃さは 0〜100% です"),
    (r"ops file must be a JSON array", "操作のファイルの中身が正しくありません"),
    (r"this .* changes approvals.*", "承認が変わる操作は、承認・取り消しの画面から行います"),
    # families
    (r"no (page|panel|layer|line|ruler|effect|tone|ticket|material|mannequin|3D figure or box|stroke|frame) .*",
     lambda m: f"{_THINGS[m.group(1)]}が見つかりません（消されたか、別のページのものです）"),
    (r"(?:line|ruler) .* already exists", "同じ ID がすでにあります"),
    (r"(gradient \w+|[a-z_]+) must be one of .*|(gradient \w+|[a-z_]+) must be .*",
     lambda m: f"{_field(m.group(1) or m.group(2))}の指定が正しくありません"),
    (r"(gradient \w+|[a-z_]+) is (?:a density from )?(\d+) to (\d+).*", lambda m: f"{_field(m.group(1))}は {m.group(2)}〜{m.group(3)} で指定します"),
    (r"([a-z_]+) is (\d+) or more", lambda m: f"{_field(m.group(1))}は {m.group(2)} 以上にします"),
    (r"([a-z_]+(?: \(int\))?) is required", lambda m: f"{_field(m.group(1).replace(' (int)', ''))}の指定が要ります"),
]


def error(message: str) -> str:
    """An ApplyError message in plain Japanese (unknown ones are kept, after a short lead)."""
    text = str(message or "").strip()
    text = re.sub(r"^ops\[\d+\] [a-z_]+: ", "", text)
    for pattern, replacement in _ERRORS:
        match = re.fullmatch(pattern, text)
        if match:
            if callable(replacement):
                return replacement(match)
            return match.expand(replacement) if "\\" in replacement else replacement
    if re.search(r"[ぁ-んァ-ヶ一-龥]", text):
        return text
    return f"この操作はできませんでした（{text}）"
