"""ヘルプ: the shortcut list (made from the menus as they are now, so changed keys show), a one-page
guide from the name to the export, and answers for when something seems stuck."""

from __future__ import annotations

from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import QDialog, QDialogButtonBox, QTextBrowser, QVBoxLayout

GUIDE = """
<h2>はじめての 1 冊</h2>
<p>Genko は、ネームから入稿まで 1 つの窓で描けるマンガの原稿道具です。左端が道具、その右が持っている道具の設定
（選んだ台詞の文字やフキダシの設定もここ）、右側がページ・レイヤー・台詞などのパネルです。
描いた線は「ペン入れ」レイヤーに入り、そのまま印刷されます。</p>
<h3>1. 原稿を作る</h3>
<p>「ファイル → 新しい原稿…」で作品名・ページ数・原稿用紙（B4 投稿・同人誌 B5／A5 など）を選びます。
用紙は後から「ページ → 原稿用紙の設定…」で変えられます（コマや台詞も合わせて動きます）。</p>
<h3>2. コマを割る</h3>
<p>コマ割りの道具（F）で、コマの中を横切るようにドラッグすると割れます。間の白をドラッグで幅、角をドラッグで形を変えます。
「コマ → テンプレートでコマを割る…」で定番の割り方も選べます。</p>
<h3>3. ネーム・下描き</h3>
<p>レイヤー パネルで「ネーム」を選び、ペン（B）で描きます。レイヤーを足して「下描き（書き出さない）」にすると、
印刷には出ない下描きになります。表示色を青にすると見分けやすくなります。</p>
<h3>4. ペン入れ</h3>
<p>「ペン入れ」レイヤーを選び、ブラシ（G ペン・丸ペン・筆…）と太さを選んで描きます。[ と ] で太さ、Shift で直線、
消しゴム（E）は線を触れた所で切ります。ペンの後ろ側でなぞっても消えます。回して描きたいときは「表示 → 右に回す」、
形の狂いは H（左右反転）で確かめます。</p>
<h3>5. 写植（台詞）</h3>
<p>テキストの道具（T）で置きたい所をクリックして打ちます。ルビは ｜約束《やくそく》、傍点は 《《強調》》。
フキダシの形・しっぽ・回転は、選択ツール（V）でフキダシを選んで、ハンドルか台詞パネルで変えます。</p>
<h3>6. トーン・効果線・仕上げ</h3>
<p>範囲を選んで「トーン・効果線 → トーンを貼る」、効果線の道具（K）でコマをクリックすると集中線などが入ります。
素材パネルからトーン・効果線・自分の素材を貼れます。</p>
<h3>7. 点検して書き出す</h3>
<p>「ページ → 入稿前の点検」（F9）で、はみ出し・文字の小ささなどを探します。
「ファイル → 書き出し…」で PDF・TIFF・PSD・縦読み・SNS 用などに書き出します。書き出す前にも点検が行われます。</p>
"""

FAQ = """
<h2>困ったとき</h2>
<h3>線が描けない</h3>
<p>下の欄に「描けません」と出ていないか見ます。レイヤー パネルで選んでいるレイヤーがロックされているか、
ペンかペイントのレイヤーでないと描けません。マスクの編集中だと、ペンはマスクを描きます（レイヤー パネルの「マスク ▾」）。</p>
<h3>描いた線が印刷に出ない</h3>
<p>そのレイヤーが「下描き（書き出さない）」になっていないか、ネームのレイヤーでないかを確かめます。
コマの外に描いた線は、レイヤーの「コマの外にもはみ出す」を入れないとコマで切れます。</p>
<h3>間違えた</h3>
<p>Ctrl+Z で戻り、Ctrl+Shift+Z（Ctrl+Y）でやり直します。「編集 → 履歴…」（Ctrl+H）なら、前の時点をクリックして一度に戻れます。</p>
<h3>画面が回った・反転したまま</h3>
<p>「表示 → 回転・反転を戻す」（Ctrl+Alt+0）。</p>
<h3>パネルを閉じてしまった</h3>
<p>「ウィンドウ」メニューから出し直せます。</p>
<h3>文字が枠からはみ出す・小さすぎる</h3>
<p>入稿前の点検（F9）で場所が分かります。台詞パネルで文字の大きさを「自動」にすると、フキダシに合わせて縮みます。</p>
<h3>ほかのパソコンで開いたら線の見た目が違う</h3>
<p>自作のブラシは原稿の中にも写るので、同じに描かれます。書体は、パソコンの書体を使ったときだけ、そのパソコンにも同じ書体が要ります。</p>
<h3>キーを変えたい・筆圧が合わない</h3>
<p>「ファイル → 環境設定…」でショートカットを変え、ペンタブレットの試し書きで筆圧を合わせます。</p>
"""


def shortcut_rows(window) -> list[tuple[str, str, str]]:
    """(menu, command, keys) for every command that has keys, in menu order."""
    out = []
    seen = set()
    for top in window.menuBar().actions():
        menu = top.menu()
        if menu is None:
            continue
        for action in menu.actions():
            if action.isSeparator() or action.menu() or not action.text():
                continue
            keys = ", ".join(k.toString(QKeySequence.SequenceFormat.NativeText) for k in action.shortcuts())
            if keys and action.text() not in seen:
                seen.add(action.text())
                out.append((top.text(), action.text(), keys))
    return out


def shortcut_html(window) -> str:
    rows = []
    last = None
    for menu, command, keys in shortcut_rows(window):
        if menu != last:
            rows.append(f"<tr><th colspan=2 align=left style='padding-top:8px'>{menu}</th></tr>")
            last = menu
        rows.append(f"<tr><td>{command}</td><td><b>{keys}</b></td></tr>")
    return ("<h2>ショートカット一覧</h2><p>ほかに: スペース＋ドラッグで表示を動かす、Shift＋スペース＋ドラッグで回す、"
            "Ctrl＋ホイールで拡大縮小。キーは「環境設定」で変えられます。</p><table cellspacing=4>" + "".join(rows) + "</table>")


def show(window, title: str, html: str) -> QDialog:
    dialog = QDialog(window)
    dialog.setWindowTitle(title)
    dialog.resize(620, 560)
    text = QTextBrowser()
    text.setOpenExternalLinks(True)
    text.setHtml(html)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
    buttons.button(QDialogButtonBox.StandardButton.Close).setText("閉じる")
    buttons.rejected.connect(dialog.reject)
    layout = QVBoxLayout(dialog)
    layout.addWidget(text, 1)
    layout.addWidget(buttons)
    dialog.text = text  # type: ignore[attr-defined]
    dialog.show()
    return dialog
