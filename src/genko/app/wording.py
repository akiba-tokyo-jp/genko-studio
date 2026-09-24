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
    if title and not re.fullmatch(r"(art|ink|placed)?\s*[0-9a-f]{8,}", title) and title != role:
        return title
    return base


# --- errors -------------------------------------------------------------------------------------

_ERRORS: list[tuple[str, str]] = [
    (r"nothing to undo", "これ以上戻せません"),
    (r"nothing to redo", "やり直せる操作はありません"),
    (r"cannot delete the last page", "最後の 1 ページは削除できません"),
    (r"cannot merge the root frame", "ページ全体のコマは結合できません（隣のコマと結合するには、割ったコマを選びます）"),
    (r"cannot delete core layer", "基本のレイヤーは削除できません"),
    (r"page \d+: the name is approved.*", "このページのネームは承認済みなので、コマ割りを変えられません。変えるには承認を取り消します"),
    (r".*needs name_ok.*|ink strokes require name_ok|.*requires name_ok", "ネームの承認（ネームOK）の後でできる操作です"),
    (r".*finish needs the art approved.*", "作画が承認されてから仕上げに進めます"),
    (r".*needs a person.*cannot approve.*", "承認は人だけが行えます"),
    (r"page (\d+) locked by (.+?)(;.*)?$", r"\1 ページは \2 が作業中です"),
    (r"frame .* has placed art.*|panels under .* have placed art.*", "このコマには絵が置かれています。絵をどかしてから操作します"),
    (r"the latest change is by (.+?);.*", r"直前の変更は \1 のものなので戻せません"),
    (r"this (undo|redo) changes approvals.*", "承認が変わる操作は、承認・取り消しの画面から行います"),
    (r"a stroke cannot cross the gutter.*", "見開きの綴じ目をまたぐ線は引けません"),
    (r"no page (\d+)", r"\1 ページはありません"),
    (r"frame_id is required", "先にコマを選びます"),
    (r"text is required", "文字を入力します"),
    (r"project.json changed outside the journal.*", "原稿が外で書き換えられたため、戻せません"),
    (r"image too large.*", "画像が大きすぎます"),
    (r"not a readable image.*", "読み込めない画像です"),
]


def error(message: str) -> str:
    """An ApplyError message in plain Japanese (unknown ones are kept, after a short lead)."""
    text = str(message or "").strip()
    text = re.sub(r"^ops\[\d+\] [a-z_]+: ", "", text)
    for pattern, replacement in _ERRORS:
        if re.fullmatch(pattern, text):
            return re.sub(pattern, replacement, text) if "\\" in replacement else replacement
    if re.search(r"[ぁ-んァ-ヶ一-龥]", text):
        return text
    return f"この操作はできませんでした（{text}）"
