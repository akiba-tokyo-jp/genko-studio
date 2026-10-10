"""Usage: make_ruby_cases.py REPO_ROOT

Writes native/tests/data/ruby_cases.json (TEXT-01's table of expected results) and its MANIFEST entry.
Characters that do not show on their own (combining marks, variation selectors, joiners, emoji) are written as \\u
escapes so the table can be read."""
import hashlib, json, sys, unicodedata
from pathlib import Path

ROOT = Path(sys.argv[1])
FAMILY = "\U0001F468‍\U0001F469‍\U0001F467"
THUMB = "\U0001F44D\U0001F3FD"
JP = "\U0001F1EF\U0001F1F5"
IVS = "\U000E0100"

def span(start, end, ruby, base=None, review=None, **settings):
    """A ruby: [start, end) graphemes, its reading; base and review as the book keeps them; its own settings
    (font, scale, align, gap_mm, tracking)."""
    out = {"start": start, "end": end, "ruby": ruby}
    if base is not None:
        out["base"] = base
    if review:
        out["review"] = review
    out.update(settings)
    return out

# a ruby's own settings, carried with it through every edit
SET = {"font": "gothic", "scale": 0.6, "align": "head", "gap_mm": 1.0, "tracking": 0.2}
SET2 = {"font": "mincho", "align": "tail", "gap_mm": 0.5}

def edit(text):
    return {"op": "edit_line", "text": text}

def replace(find, to):
    return {"op": "replace_text", "find": find, "replace": to}

def want(text, spans, runs, warnings=(), report=None):
    out = {"text": text, "spans": spans, "runs": runs, "warnings": list(warnings)}
    reviews = sum(1 for s in spans or [] if s.get("review"))
    if reviews and report is None:
        report = {"ruby_review": reviews}  # (the op reports the rubies it made need review)
    if report is not None:
        out["report"] = report
    return out

T = "東京へ"
R = "とうきょう"
cases = [
    # AC-TEXT's own example and the edit rules
    {"n": "AC-TEXT: 先頭に「新」を挿入すると [1,3)", "text": T, "spans": [span(0, 2, R)], "edit": edit("新東京へ"),
     "want": want("新東京へ", [span(1, 3, R, "東京")], [["東京", R]])},
    {"n": "AC-TEXT: 末尾の境界に「駅」を挿入しても [0,2)", "text": T, "spans": [span(0, 2, R)], "edit": edit("東京駅へ"),
     "want": want("東京駅へ", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "親文字の中への挿入は要確認（元の親文字と読みを残す）", "text": T, "spans": [span(0, 2, R)], "edit": edit("東新京へ"),
     "want": want("東新京へ", [span(0, 3, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "親文字の一部の削除は要確認", "text": T, "spans": [span(0, 2, R)], "edit": edit("東へ"),
     "want": want("東へ", [span(0, 1, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "親文字の中の改行は要確認", "text": T, "spans": [span(0, 2, R)], "edit": edit("東\n京へ"),
     "want": want("東\n京へ", [span(0, 3, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "範囲をまたぐ全文置換は要確認", "text": T, "spans": [span(0, 2, R)], "edit": replace("京へ", "都に"),
     "want": want("東都に", [span(0, 3, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "親文字をすべて消すとルビは描かない（Undo で本文・範囲・読みが戻る）", "text": T, "spans": [span(0, 2, R)], "edit": edit("へ"),
     "want": want("へ", None, [], [], {"ruby_removed": 1}), "undo": True},
    {"n": "後ろの編集は対応を変えない", "text": T, "spans": [span(0, 2, R)], "edit": edit("東京へ行く"),
     "want": want("東京へ行く", [span(0, 2, R, "東京")], [["東京", R]])},
    # the same word more than once
    {"n": "同じ語の繰り返し: 2 つ目の「東京」のルビは 2 つ目に付いたまま", "text": "東京と東京", "spans": [span(3, 5, R)],
     "edit": edit("大東京と東京"), "want": want("大東京と東京", [span(4, 6, R, "東京")], [["東京", R]])},
    {"n": "同じ語の繰り返し: どちらの語が消えたか決められないときは要確認", "text": "東京と東京", "spans": [span(0, 2, R)],
     "edit": edit("東京"), "want": want("東京", [span(0, 2, R, "東京", "ambiguous")], [], ["ruby_review"])},
    {"n": "同じ語の繰り返し: 全文置換で前の語が消えても、残った語に前の語の読みを移さない", "text": "東京と東京",
     "spans": [span(0, 2, R), span(3, 5, "とうけい")], "edit": replace("東京と", ""),
     "want": want("東京", [span(0, 2, "とうけい", "東京")], [["東京", "とうけい"]], [], {"ruby_removed": 1})},
    {"n": "同じ語の繰り返し（Python の ruby_runs）: 全文置換で前の語が消えたら位置で持つ", "text": "東京と東京",
     "runs": [["東京", R], ["東京", "とうけい"]], "edit": replace("東京と", ""),
     "want": want("東京", [span(0, 2, "とうけい", "東京")], [["東京", "とうけい"]], [], {"ruby_removed": 1})},
    {"n": "同じ語の繰り返し: 足した語がどれか決められないときは両方とも要確認", "text": "東京と東京",
     "spans": [span(0, 2, R), span(3, 5, "トウキョウ")], "edit": edit("東京と東京と東京"),
     "want": want("東京と東京と東京", [span(0, 2, R, "東京", "ambiguous"), span(3, 5, "トウキョウ", "東京", "ambiguous")], [],
                  ["ruby_review", "ruby_review"])},
    {"n": "同じ語の繰り返し: 最長共通部分列のどれかが親文字を崩すなら動かさず要確認", "text": "東京東京", "spans": [span(2, 4, R)],
     "edit": edit("東東京京"), "want": want("東東京京", [span(1, 4, R, "東京", "ambiguous")], [], ["ruby_review"])},
    {"n": "親文字の外の 2 か所の編集: 親文字はそのまま動く", "text": "あ東京い", "spans": [span(1, 3, R)], "edit": edit("う東京え"),
     "want": want("う東京え", [span(1, 3, R, "東京")], [["東京", R]])},
    # combining characters, emoji, variation selectors: graphemes, not UTF-16 units or code points
    {"n": "結合文字: 範囲は書記素で数える（e + U+0301 は 1 つ）", "text": "Caféで", "wrap": "horizontal",
     "spans": [span(0, 4, "カフェ")], "edit": edit("Café!で"),
     "want": want("Café!で", [span(0, 4, "カフェ", "Café")], [["Café", "カフェ"]])},
    {"n": "結合文字: 親文字の末尾の字に付いた結合文字は親文字の中の編集（要確認）", "text": T, "spans": [span(0, 2, R)],
     "edit": edit("東京́へ"), "want": want("東京́へ", [span(0, 2, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "結合文字: 1 字の親文字に付いた結合文字は親文字の中の編集（要確認。外さない）", "text": "海へ", "spans": [span(0, 1, "うみ")],
     "edit": edit("海\u0301へ"), "want": want("海\u0301へ", [span(0, 1, "うみ", "海", "edited")], [], ["ruby_review"])},
    {"n": "異体字セレクタ（IVS）: 1 字の親文字に付けたセレクタは親文字の中の編集（要確認）", "text": "葛飾区", "spans": [span(0, 1, "かつ")],
     "edit": edit("葛" + IVS + "飾区"), "want": want("葛" + IVS + "飾区", [span(0, 1, "かつ", "葛", "edited")], [], ["ruby_review"])},
    {"n": "絵文字の並び（ZWJ）: 1 つの絵文字の親文字に ZWJ で続けた並びは親文字の中の編集（要確認）", "text": "\U0001F468へ",
     "spans": [span(0, 1, "ひと")], "edit": edit("\U0001F468\u200d\U0001F469へ"),
     "want": want("\U0001F468\u200d\U0001F469へ", [span(0, 1, "ひと", "\U0001F468", "edited")], [], ["ruby_review"])},
    {"n": "絵文字の並び（ZWJ）: 前の絵文字を別の絵文字の並びに置き換えても範囲は書記素のまま", "text": FAMILY + "と東京",
     "spans": [span(2, 4, R)], "edit": replace(FAMILY, THUMB),
     "want": want(THUMB + "と東京", [span(2, 4, R, "東京")], [["東京", R]])},
    {"n": "絵文字（国旗）の親文字: 後ろへの挿入は含めない", "text": JP + "へ", "spans": [span(0, 1, "にほん")], "edit": edit(JP + "!へ"),
     "want": want(JP + "!へ", [span(0, 1, "にほん", JP)], [[JP, "にほん"]])},
    {"n": "絵文字（国旗）の親文字: 同じ国旗を足すとどちらか決められず要確認", "text": JP + "へ", "spans": [span(0, 1, "にほん")],
     "edit": edit(JP + JP + "へ"), "want": want(JP + JP + "へ", [span(0, 1, "にほん", JP, "ambiguous")], [], ["ruby_review"])},
    {"n": "異体字セレクタ（IVS）: 字と一緒に 1 書記素、前への挿入で両端が動く", "text": "葛" + IVS + "飾区", "spans": [span(0, 1, "かつ")],
     "edit": edit("新葛" + IVS + "飾区"), "want": want("新葛" + IVS + "飾区", [span(1, 2, "かつ", "葛" + IVS)], [["葛" + IVS, "かつ"]])},
    {"n": "異体字セレクタ（IVS）: セレクタだけを消す全文置換は親文字の中の編集（要確認）", "text": "葛" + IVS + "飾区",
     "spans": [span(0, 1, "かつ")], "edit": replace(IVS, ""),
     "want": want("葛飾区", [span(0, 1, "かつ", "葛" + IVS, "edited")], [], ["ruby_review"])},
    {"n": "標準異体字（SVS U+FE00）: 後ろへの挿入は含めない", "text": "辻︀堂", "spans": [span(0, 2, "つじどう")],
     "edit": edit("辻︀堂駅"), "want": want("辻︀堂駅", [span(0, 2, "つじどう", "辻︀堂")], [["辻︀堂", "つじどう"]])},
    # full-text replace (replace_text)
    {"n": "全文置換: 親文字ごと置き換えた語のルビは描かない（ほかは残る）", "text": "東京と大阪",
     "spans": [span(0, 2, R), span(3, 5, "おおさか")], "edit": replace("大阪", "京都"),
     "want": want("東京と京都", [span(0, 2, R, "東京")], [["東京", R]], [], {"ruby_removed": 1})},
    {"n": "全文置換: 置き換えた結果が親文字の後ろに足すだけなら対応はそのまま", "text": "東京", "spans": [span(0, 2, R)],
     "edit": replace("京", "京都"), "want": want("東京都", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "全文置換: 親文字の一部の置換は要確認", "text": T, "spans": [span(0, 2, R)], "edit": replace("京", "都"),
     "want": want("東都へ", [span(0, 2, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "全文置換: 親文字全体を置き換えるとルビを外す（置換文字列と共通の字があっても）", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "東西"), "want": want("東西へ", None, [], [], {"ruby_removed": 1})},
    {"n": "全文置換: 親文字全体を新しい語に置き換えるとルビを外す", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "東新京"), "want": want("東新京へ", None, [], [], {"ruby_removed": 1})},
    {"n": "全文置換: いくつもの一致で親文字全体が置き換わるとルビを外す", "text": "ああへ", "spans": [span(0, 2, "x")],
     "edit": replace("あ", "い"), "want": want("いいへ", None, [], [], {"ruby_removed": 1})},
    {"n": "全文置換: 親文字全体の一致を、親文字を 1 度含む語に置き換えるとルビはその中の親文字へ動く", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "「東京」"), "want": want("「東京」へ", [span(1, 3, R, "東京")], [["東京", R]])},
    {"n": "全文置換: 親文字を含む一致の前に字を足すとルビは親文字へ動く", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京へ", "大東京へ"), "want": want("大東京へ", [span(1, 3, R, "東京")], [["東京", R]])},
    {"n": "全文置換: 親文字全体の一致の後ろに同じ語を足すだけなら対応はそのまま", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "東京東京"), "want": want("東京東京へ", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "全文置換: 親文字全体の一致を、親文字を 2 度含む語に置き換えるとどちらか決められず要確認", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "「東京東京」"), "want": want("「東京東京」へ", [span(0, 6, R, "東京", "ambiguous")], [], ["ruby_review"])},
    {"n": "全文置換: 一致が親文字を含んでも、親文字の外だけが変わるなら対応はそのまま", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京へ", "東京まで"), "want": want("東京まで", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "全文置換: 親文字全体の一致の後ろに足すだけなら対応はそのまま", "text": T, "spans": [span(0, 2, R)],
     "edit": replace("東京", "東京都"), "want": want("東京都へ", [span(0, 2, R, "東京")], [["東京", R]])},
    # Python's ruby_runs (the explicit rule)
    {"n": "ruby_runs の変換: 位置で持つ台詞に ruby_runs を渡すと明示ルール（前から順に、前のルビの後ろで最初に見つかる所）で範囲にする",
     "text": "東京と東京", "spans": [span(3, 5, "x")], "edit": {"op": "edit_line", "ruby_runs": [["東京", R]]},
     "want": want("東京と東京", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "ruby_runs の変換: 前と同じ所に来たルビは、そこにあったルビの組み方を保つ", "text": "東京と東京",
     "spans": [span(3, 5, "x", **SET)], "edit": {"op": "edit_line", "ruby_runs": [["東京", R], ["東京", "きょう"]]},
     "want": want("東京と東京", [span(0, 2, R, "東京"), span(3, 5, "きょう", "東京", **SET)], [["東京", R], ["東京", "きょう"]])},
    {"n": "ruby_runs の受け取り: 位置で持つ台詞の写しの ruby_runs を送り返しても、ルビの位置と組み方はそのまま", "text": "東京と東京",
     "spans": [span(3, 5, "うしろ", gap_mm=1.0)], "edit": {"op": "edit_line", "speaker": "A", "ruby_runs": [["東京", "うしろ"]]},
     "want": want("東京と東京", [span(3, 5, "うしろ", "東京", gap_mm=1.0)], [["東京", "うしろ"]])},
    {"n": "ruby_runs の受け取り: 本文と一緒に写しを送っても、本文の編集に追従した位置と組み方のまま", "text": "東京と東京",
     "spans": [span(3, 5, "うしろ", gap_mm=1.0)], "edit": {"op": "edit_line", "text": "新東京と東京", "ruby_runs": [["東京", "うしろ"]]},
     "want": want("新東京と東京", [span(4, 6, "うしろ", "東京", gap_mm=1.0)], [["東京", "うしろ"]])},
    {"n": "ruby_runs の受け取り: 本文の編集で要確認になったルビも、op の前の写しを送り返しただけならそのまま（前の語へ移さない）",
     "text": "東京と東京", "spans": [span(3, 5, "B", gap_mm=1.0)],
     "edit": {"op": "edit_line", "text": "東京と東京と東京", "ruby_runs": [["東京", "B"]]},
     "want": want("東京と東京と東京", [span(3, 5, "B", "東京", "ambiguous", gap_mm=1.0)], [], ["ruby_review"])},
    {"n": "ruby_runs の受け取り: 写しと違う ruby_runs は明示ルール、組み方は親文字と読みの同じルビから（順に）", "text": "東京と東京",
     "spans": [span(0, 2, "まえ", **SET), span(3, 5, "うしろ", **SET2)], "edit": {"op": "edit_line", "ruby_runs": [["東京", "うしろ"]]},
     "want": want("東京と東京", [span(0, 2, "うしろ", "東京", **SET2)], [["東京", "うしろ"]])},
    {"n": "ruby_runs の変換: 見つからなかった親文字は、後で本文に現れても読みを推測せず要確認で残す", "text": T,
     "runs": [["大阪", "おおさか"], ["東京", R]], "edit": edit("東京へ大阪"),
     "want": want("東京へ大阪", [span(0, 0, "おおさか", "大阪", "unplaced"), span(0, 2, R, "東京")], [["東京", R]], ["ruby_review"])},
    {"n": "ruby_runs のまま: 対応の変わらない編集は Python と同じに保存する", "text": T, "runs": [["東京", R]], "edit": edit("新東京へ"),
     "want": want("新東京へ", None, [["東京", R]])},
    {"n": "ruby_runs のまま: Python なら別の語へ移る編集は位置で持って要確認にする", "text": "東京と東京", "runs": [["東京", R]],
     "edit": edit("東京"), "want": want("東京", [span(0, 2, R, "東京", "ambiguous")], [], ["ruby_review"])},
    {"n": "ruby_runs のまま: 親文字の一部の全文置換は要確認（Python は描かなくなる）", "text": T, "runs": [["東京", R]],
     "edit": replace("京", "都"), "want": want("東都へ", [span(0, 2, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "ruby_runs のまま: 親文字全体の全文置換は Python と同じく読みを外す（位置で持たない）", "text": T, "runs": [["東京", R]],
     "edit": replace("東京", "東西"), "want": want("東西へ", None, [])},
    {"n": "ruby_runs のまま: 親文字の頭を消す全文置換も Python と同じく読みを外す", "text": T, "runs": [["東京", R]],
     "edit": replace("東京", "京"), "want": want("京へ", None, [])},
    {"n": "ruby_runs のまま: いくつもの一致で親文字全体が置き換わると Python と同じく読みを外す", "text": "ああへ", "runs": [["ああ", "x"]],
     "edit": replace("あ", "い"), "want": want("いいへ", None, [])},
    {"n": "ruby_runs のまま: 親文字を 1 度含む語への全文置換は Python と同じに保存する（その中の親文字に描く）", "text": T,
     "runs": [["東京", R]], "edit": replace("東京", "「東京」"), "want": want("「東京」へ", None, [["東京", R]])},
    {"n": "ruby_runs のまま: 親文字を含む一致の前に字を足す全文置換は Python と同じに保存する", "text": T, "runs": [["東京", R]],
     "edit": replace("東京へ", "大東京へ"), "want": want("大東京へ", None, [["東京", R]])},
    {"n": "ruby_runs のまま: 一致を入れ替えても親文字が同じ所なら Python と同じに保存する", "text": "と東京へ", "runs": [["東京", R]],
     "edit": replace("と東京", "東京と"), "want": want("東京とへ", None, [["東京", R]])},
    {"n": "ruby_runs のまま: 親文字を消した語のルビを外したことは報告する（Python なら残った同じ語に移る）", "text": "東京と東京",
     "runs": [["東京", R]], "edit": replace("東京と", ""), "want": want("東京", None, [], [], {"ruby_removed": 1})},
    {"n": "ruby_runs のまま: 後ろに足すだけの全文置換は Python と同じに保存する", "text": "東京", "runs": [[], ["東京", R], "東"],
     "edit": replace("京", "京都"), "want": want("東京都", None, [["東京", R], ["東"]])},
    # the layout: what does not fit or has no font is said, never shrunk or left out
    {"n": "本文より長い読みと読みの重なりを警告する（縮めず、切らない）", "text": "海と空", "box": [20, 20, 60, 60],
     "spans": [span(0, 1, "わたつみのかみさま"), span(2, 3, "そら")], "edit": None,
     "want": want("海と空", [span(0, 1, "わたつみのかみさま", "海"), span(2, 3, "そら", "空")], [["海", "わたつみのかみさま"], ["空", "そら"]],
                  ["ruby_overflow", "ruby_overlap"])},
    {"n": "親文字より長い読みが行頭で内側へ寄るのは警告しない", "text": "東京へ", "box": [20, 20, 60, 60], "spans": [span(0, 2, R)],
     "edit": None, "want": want("東京へ", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "ルビの書体にない字は代わりの書体で描き、警告する", "text": "井戸", "box": [20, 20, 60, 60],
     "spans": [span(0, 1, "ゐ", font="sfx")], "edit": None,
     "want": want("井戸", [span(0, 1, "ゐ", "井", font="sfx")], [["井", "ゐ"]], ["ruby_font_fallback"])},
    # each ruby's own settings go with it wherever the edit takes it (moved, needing review, taken away and back)
    {"n": "ルビの組み方は読みと一緒に動く", "text": T, "spans": [span(0, 2, R, **SET)], "edit": edit("新東京へ"),
     "want": want("新東京へ", [span(1, 3, R, "東京", **SET)], [["東京", R]])},
    {"n": "親文字の全文置換で外したルビは Undo で組み方ごと戻る", "text": T, "spans": [span(0, 2, R, **SET)], "edit": replace("東京", "大阪"),
     "want": want("大阪へ", None, [], [], {"ruby_removed": 1}), "undo": True},
    {"n": "要確認になったルビも組み方を残す", "text": T, "spans": [span(0, 2, R, **SET)], "edit": edit("東新京へ"),
     "want": want("東新京へ", [span(0, 3, R, "東京", "edited", **SET)], [], ["ruby_review"])},
    {"n": "同じ語の繰り返し: それぞれのルビが自分の組み方を保つ", "text": "東京と東京",
     "spans": [span(0, 2, "とう", **SET), span(3, 5, "きょう", **SET2)], "edit": edit("大東京と東京"),
     "want": want("大東京と東京", [span(1, 3, "とう", "東京", **SET), span(4, 6, "きょう", "東京", **SET2)], [["東京", "とう"], ["東京", "きょう"]])},
    {"n": "全文置換でも組み方は読みと一緒に動く（前の語が消えても後ろの語の組み方のまま）", "text": "東京と東京",
     "spans": [span(0, 2, "とう", **SET), span(3, 5, "きょう", **SET2)], "edit": replace("東京と", ""),
     "want": want("東京", [span(0, 2, "きょう", "東京", **SET2)], [["東京", "きょう"]], [], {"ruby_removed": 1})},

    {"n": "どの書体にもない字は警告する", "text": "井戸", "box": [20, 20, 60, 60], "spans": [span(0, 1, "ㇰ")], "edit": None,
     "want": want("井戸", [span(0, 1, "ㇰ", "井")], [["井", "ㇰ"]], ["ruby_glyph_missing"])},
    # typed with the input method (the canvas's editor): the words being converted never change the book
    {"n": "IME: 先頭で変換して「新」と確定", "text": T, "spans": [span(0, 2, R)], "gui": True,
     "ime": {"shown": "｜東京《とうきょう》へ", "steps": [{"cursor": 0, "preedit": "しん", "commit": "新"}]},
     "want": want("新東京へ", [span(1, 3, R, "東京")], [["東京", R]])},
    {"n": "IME: 組み方のあるルビの前で確定しても組み方を保つ", "text": T, "spans": [span(0, 2, R, **SET)], "gui": True,
     "ime": {"shown": "｜東京《とうきょう》へ", "steps": [{"cursor": 0, "preedit": "しん", "commit": "新"}]},
     "want": want("新東京へ", [span(1, 3, R, "東京", **SET)], [["東京", R]])},
    {"n": "IME: ルビの後ろで変換して「駅」と確定", "text": T, "spans": [span(0, 2, R)], "gui": True,
     "ime": {"shown": "｜東京《とうきょう》へ", "steps": [{"cursor": 10, "preedit": "えき", "commit": "駅"}]},
     "want": want("東京駅へ", [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "IME: 親文字の中で確定すると要確認", "text": T, "spans": [span(0, 2, R)], "gui": True,
     "ime": {"shown": "｜東京《とうきょう》へ", "steps": [{"cursor": 2, "preedit": "しん", "commit": "新"}]},
     "want": want("東新京へ", [span(0, 3, R, "東京", "edited")], [], ["ruby_review"])},
    {"n": "IME: 変換中にやめると何も変わらない", "text": T, "spans": [span(0, 2, R)], "gui": True,
     "ime": {"shown": "｜東京《とうきょう》へ", "steps": [{"cursor": 0, "preedit": "しん"}], "cancel": True},
     "want": want(T, [span(0, 2, R, "東京")], [["東京", R]])},
    {"n": "記法: 2 つ目の語のルビを記法ごと消すと、残した記法のルビだけが描かれる（要確認を残さない）", "text": "東京と東京",
     "spans": [span(0, 2, "A"), span(3, 5, "B")], "gui": True,
     "ime": {"shown": "｜東京《A》と｜東京《B》", "steps": [{"delete": [6, 13]}]},
     "want": want("東京", [span(0, 2, "A", "東京")], [["東京", "A"]])},
    {"n": "記法: 1 つ目の語のルビを記法ごと消すと、残したルビが組み方ごと描かれる", "text": "東京と東京",
     "spans": [span(0, 2, "A"), span(3, 5, "B", **SET2)], "gui": True,
     "ime": {"shown": "｜東京《A》と｜東京《B》", "steps": [{"delete": [0, 7]}]},
     "want": want("東京", [span(0, 2, "B", "東京", **SET2)], [["東京", "B"]])},
    {"n": "記法: 同じ親文字と読みのルビが 2 つ、前に同じ語を足すと、どちらも自分の組み方のまま（出てくる順に）", "text": "東京と東京",
     "spans": [span(0, 2, "A", **SET), span(3, 5, "A", **SET2)], "gui": True,
     "ime": {"shown": "｜東京《A》と｜東京《A》", "steps": [{"cursor": 0, "commit": "東京と"}]},
     "want": want("東京と東京と東京", [span(3, 5, "A", "東京", **SET), span(6, 8, "A", "東京", **SET2)], [["東京", "A"], ["東京", "A"]])},
    {"n": "記法（Python の ruby_runs）: 2 つ目の語のルビを記法ごと消すと、Python と同じ ruby_runs で送る", "text": "東京と東京",
     "runs": [["東京", "A"], ["東京", "B"]], "gui": True,
     "ime": {"shown": "｜東京《A》と｜東京《B》", "steps": [{"delete": [6, 13]}]},
     "want": want("東京", None, [["東京", "A"]])},
    {"n": "記法（Python の ruby_runs）: 読みを同じ語の 2 つ目へ付け替えると位置で持つ", "text": "東京と東京", "runs": [["東京", "A"]], "gui": True,
     "ime": {"shown": "｜東京《A》と東京", "steps": [{"delete": [3, 6]}, {"delete": [0, 1]}, {"cursor": 3, "commit": "｜"},
                                                    {"cursor": 6, "commit": "《A》"}]},
     "want": want("東京と東京", [span(3, 5, "A", "東京")], [["東京", "A"]])},
    {"n": "IME: 同じ語の 2 つ目に記法でルビを振る", "text": "東京と東京", "gui": True,
     "ime": {"shown": "東京と東京", "steps": [{"cursor": 3, "commit": "｜"}, {"cursor": 6, "preedit": "とうきょう", "commit": "《とうきょう》"}]},
     "want": want("東京と東京", [span(3, 5, R, "東京")], [["東京", R]])},
]

doc = {
    "note": ("TEXT-01 (改善 I04, AC-TEXT): the expected results of a line's ruby followed through edits. Each case puts a line "
             "(add_line with its text and ruby_spans, or Python's ruby_runs; wrap vertical unless given; box [x, y, w, h] mm "
             "when given; style when given), applies `edit` (edit_line or replace_text; null: none), and wants the line's "
             "text, its saved ruby_spans (null: none, the line keeps Python's ruby_runs), its ruby_runs, the ruby_* codes of "
             "入稿前の点検 for the line (checks.page_issues, in order) and the ruby_* keys of the op's report (ruby_review: the "
             "rubies it made need review, ruby_removed: the ones whose whole base it took away; for the cases typed in the "
             "editor, not checked). A ruby's own settings (font, scale, align, gap_mm, tracking) go with it through every "
             "edit. Ranges are "
             "[start, end) in extended grapheme clusters of the text (Unicode below, Qt's QTextBoundaryFinder). A case "
             "with \"gui\" is typed in the canvas's editor with the input method (test_gui_ruby: each step puts the cursor, types the "
             "words being converted and settles them, or deletes the editor's characters [from, to)); \"undo\": Undo gives the "
             "line back as it was. Written by tools/migration/make_ruby_cases.py; its sha256 is in "
             "MANIFEST.json."),
    "unicode": "17.0",
    "cases": cases,
}

def escape(text):
    out = []
    for ch in text:
        cat = unicodedata.category(ch)
        if ord(ch) > 0xFFFF or cat in ("Mn", "Me", "Cf", "Cc") or 0xFE00 <= ord(ch) <= 0xFE0F:
            if ch == "\n":
                out.append("\\n")
                continue
            data = ch.encode("utf-16-be")
            for i in range(0, len(data), 2):
                out.append("\\u%04x" % int.from_bytes(data[i:i + 2], "big"))
        else:
            out.append(ch)
    return "".join(out)

lines = ["{", '  "note": ' + json.dumps(doc["note"], ensure_ascii=False) + ",", '  "unicode": "17.0",', '  "cases": [']
for i, case in enumerate(cases):
    body = json.dumps(case, ensure_ascii=False)
    # (json.dumps escapes "\n" already; the others are put back as escapes)
    lines.append("    " + escape(body) + ("," if i + 1 < len(cases) else ""))
lines += ["  ]", "}", ""]
text = "\n".join(lines)
assert json.loads(text) == doc
path = ROOT / "native" / "tests" / "data" / "ruby_cases.json"
path.write_text(text, encoding="utf-8")
manifest = {"ruby_cases.json": hashlib.sha256(text.encode("utf-8")).hexdigest()}
(ROOT / "native" / "tests" / "data" / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
print(len(cases), "cases", manifest)
