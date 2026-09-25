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

BLEND = [("normal", "通常"), ("multiply", "乗算"), ("screen", "スクリーン"), ("add", "加算（発光）"), ("overlay", "オーバーレイ"),
         ("darken", "比較（暗）"), ("lighten", "比較（明）"), ("color_burn", "焼き込みカラー"), ("linear_burn", "焼き込み（リニア）"),
         ("color_dodge", "覆い焼きカラー"), ("soft_light", "ソフトライト"), ("hard_light", "ハードライト"),
         ("difference", "差の絶対値"), ("exclusion", "除外"), ("subtract", "減算"), ("divide", "除算"), ("hue", "色相"),
         ("saturation", "彩度"), ("color", "カラー"), ("luminosity", "輝度")]

FILTERS = [("levels", "レベル補正"), ("curve", "明るさの曲線（トーンカーブ）"), ("hue", "色相・彩度・明度"), ("blur", "ぼかし"),
           ("sharpen", "シャープ"), ("mosaic", "モザイク"), ("motion_blur", "移動ぼかし"), ("radial_blur", "放射ぼかし"),
           ("zoom_blur", "ズームぼかし"), ("noise", "ノイズ"), ("wave", "波形"), ("twirl", "渦巻き"), ("lineart", "線画抽出"),
           ("invert", "色調反転"), ("posterize", "階調化（ポスタリゼーション）"), ("threshold", "2 値化（しきい値）"),
           ("bitonal", "白黒にする"), ("gradient_map", "グラデーションマップ")]
# what a correction layer can hold (it changes colours, not shapes)
ADJUSTMENTS = [("levels", "レベル補正"), ("curve", "トーンカーブ"), ("hue", "色相・彩度・明度"), ("invert", "色調反転"),
               ("posterize", "階調化"), ("threshold", "2 値化"), ("gradient_map", "グラデーションマップ")]


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
    (r"a brush needs a name", "ブラシに名前を付けます"),
    (r"a brush of one's own has a key starting with my_", "自作のブラシの名前（key）は my_ で始めます"),
    (r"wobble must be between 0 and 1", "線の揺れは 0〜1 の間で決めます"),
    (r"spikes must be between 6 and 80", "トゲの数は 6〜80 の間で決めます"),
    (r"spike_depth must be between 0.05 and 0.6", "トゲの長さは 0.05〜0.6 の間で決めます"),
    (r"style_runs is .*", "文字の一部の書式は、言葉と「大きさ・太字・色」の組で指定します"),
    (r"scale must be between 0.3 and 3", "文字の大きさの倍率は 0.3〜3 の間で決めます"),
    (r"unknown style_runs key (\S+).*", "文字の一部の書式に知らない項目があります（大きさ・太字・色だけ）"),
    (r"gradient_fill needs from and to.*", "グラデーションは、始めと終わりの点で指定します"),
    (r"the gradient needs a longer drag", "もう少し長くドラッグします"),
    (r"the gradient has nothing to show there", "そこにはグラデーションを塗れる所がありません"),
    (r"kind must be box, cylinder, stairs or floor.*", "3D の形は、箱・円柱・階段・床から選びます"),
    (r"scene kind must be one of .*", "背景の 3D は、部屋・教室・廊下・街並みから選びます"),
    (r"shape must be one of .*", "図形は、直線・折れ線・曲線・長方形・楕円・多角形から選びます"),
    (r"box \[x, y, w, h\] is required", "図形の大きさ（box）が要ります"),
    (r"points needs at least two \[x, y\]", "線を引くには 2 点以上が要ります"),
    (r"no saved area .*", "その名前の選択範囲は残っていません"),
    (r"name is required", "名前が要ります"),
    (r"brush texture must be .*", "ブラシの質感は、なし・鉛筆・エアブラシ・かすれ・水彩から選びます"),
    (r"an image tip needs its picture.*", "画像の先端には画像が要ります（画像から先端を作る）"),
    (r"brush aa must be .*", "アンチエイリアスは、なし・弱・中・強から選びます"),
    (r"brush pattern must be .*", "模様は、点・破線・レース・草・ハート・星・葉から選びます"),
    (r"brush tip must be .*", "先端の形は、丸・平たい・画像から選びます"),
    (r"mode must be blur, smudge or blend", "色混ぜは、ぼかし・指先・なじませから選びます"),
    (r"mode must be cut, to_crossing or whole", "消し方は、触れた所・交点まで・線全体から選びます"),
    (r"the brush is off the page", "ページの外です"),
    (r"action must be one of .*", "線の編集は、点の移動・追加・削除、つなぐ・切る・色を変える・消すから選びます"),
    (r"ids \(or stroke_id\) is required", "線を選んでください"),
    (r"connect takes two line ids", "つなぐ線を 2 本選びます"),
    (r"no stroke .*", "その線はありません"),
    (r"index is not a point of the line", "その点は線にありません"),
    (r"a line keeps at least two points.*", "線には点が 2 つ要ります（線ごと消すときは「線を消す」）"),
    (r"that is the end of the line", "線の端では切れません"),
    (r"the layer has no colour yet.*", "このレイヤーにはまだ色がありません（先に塗ってから塗り残しを塗ります）"),
    (r"there is nothing on this layer to blend there", "そこにはこのレイヤーの色がないので、混ぜられません"),
    (r"a guide's axis is h or v", "ガイド線の向きは横（h）か縦（v）です"),
    (r"a folder cannot hold itself", "フォルダを自分の中には入れられません"),
    (r"adjust is set on a correction layer", "補正の設定は、色調補正のレイヤーにだけできます"),
    (r"adjust kind must be one of .*", "色調補正は、レベル補正・トーンカーブ・色相・反転・階調化・2 値化・グラデーションマップ・白黒から選びます"),
    (r"choose two or more layers to merge", "結合するレイヤーを 2 枚以上選びます"),
    (r"effect is .* and/or .*", "境界効果は、フチか水彩境界です"),
    (r"fill is .* or .*", "塗りの設定は、色かグラデーションです"),
    (r"fill is set on a fill layer", "塗りの設定は、塗りつぶしのレイヤーにだけできます"),
    (r"from and to are \[x_mm, y_mm\]", "グラデーションのはじめと終わりの位置が要ります"),
    (r"ids is the list of layer ids", "レイヤーを選んでください"),
    (r"interp must be .*", "補間は、なめらか・よりなめらか・ハードから選びます"),
    (r"mode must be one of push.*", "ゆがみは、押し流す・縮める・ふくらませる・渦から選びます"),
    (r"mode must be one of .*", "その動かし方は選べません"),
    (r"only a paint layer can become a pen layer", "ペンのレイヤーに変換できるのは、ペイントのレイヤーだけです"),
    (r"page not found", "そのページはありません"),
    (r"parent must be a folder", "入れる先はフォルダを選びます"),
    (r"points is \[\[x, y\], \.\.\.\] in mm", "なぞった点が要ります"),
    (r"rgb is \[r, g, b\], each 0\.\.255", "色は 0〜255 の 3 つの数です"),
    (r"set_layers needs something to set.*", "まとめて変える設定がありません"),
    (r"shape must be linear or radial", "グラデーションの形は、直線か円です"),
    (r"the adjustment cannot be used: .*", "その補正の数値は使えません"),
    (r"the layer has no marks to trace", "このレイヤーには線にできる絵がありません"),
    (r"this layer cannot become a paint layer", "このレイヤーはペイントのレイヤーに変換できません"),
    (r"unknown blend mode .*", "その合成モードはありません"),
    (r"unknown effect .*", "その境界効果はありません（フチ・水彩境界）"),
    (r"an area needs at least three points", "範囲には 3 点以上が要ります"),
    (r"no layer .*", "そのレイヤーはありません"),
    (r"the colour point is off the page", "色を取る点がページの外です"),
    (r"(union|intersect|subtract) needs areas", "範囲の組み合わせには範囲が要ります"),
    (r"an area is poly, mask, rect.*", "範囲の指定が読めません"),
    (r"reference must be page, layer or reference", "塗りの見る範囲は「見えている全部」「このレイヤーだけ」「参照レイヤー」です"),
    (r"no layer is set as the reference.*", "参照レイヤーがありません。レイヤー パネルで線のレイヤーを「参照にする」にします"),
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
    (r"weight must be normal, bold or heavy", "文字の太さは「標準」「太」「極太」のどれかです"),
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
