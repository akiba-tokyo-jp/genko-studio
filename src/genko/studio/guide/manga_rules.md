# Genko ネームの規則

Genko は、あなた（エージェント）が書いた計画を検査し、コマ割りと縦書きの写植を計算する。
ここに書いた規則を破ると、`issues` に指摘が返る。`severity: "error"` は保存できない。`"warning"` は保存できるが直すのが望ましい。
指摘の `path` は、あなたが送った JSON の中の位置（JSON ポインタ）である。その場所だけを直して送り直す。

## 手順

1. `inspect target=schemas` で入力の形を確かめる。
2. 企画書（bible@1）を `set_bible` に送る。`commit:false` で指摘を見て、なくなったら `commit:true`。
3. 脚本（script@1）を `set_script` に送る。beat ごとに `page` を決める。
4. ページごとに `inspect target=page page=N` でそのページの beat と前後を読み、ネーム計画（name_plan@1）を `submit_name` に送る。
5. 返ってきたプレビュー画像を見て直す。よければ `commit:true` で保存し、`record_review` で点検結果を残す。
6. `request_approval` で人間にネームの承認を頼む。承認は人間だけができる。待つ間は別のページを進める。
7. 人間の修正指示は `next`（kind `revise_page`）と `tickets` に出る。`submit_name` を `replace:true` で送り直す。

## 段組（tiers）

- `tiers` は上から下へ並べる。`h` は段の高さの割合で、合計 1.0。
- 段の中の `cols` は **右から左** へ並べる（日本語の読み順）。`w` は幅の割合で、合計 1.0。
- 1つの列を上下に割るときは `rows`（上から下、`h` の合計 1.0）。これより深い入れ子はできない。
- `slot` はコマの名前（例 p1）。`panels` の `slot` と1対1で対応させる。
- 読み順は「上の段から、段の中は右から左、列の中は上から下」。
- 定型を使うときは `tiers: null` にして `template` に名前を書く（`inspect target=page` に一覧がある）。
- 1ページのコマは 1〜8。コマの短辺が 25 mm を切ると警告。

## コマの指示（panels）

- `shot`: ELS（超遠景）/ LS（遠景）/ FS（全身）/ MS（腰から上）/ MCU（胸から上）/ CU（顔）/ ECU（顔の一部）/ INSERT（物のアップ）
- `angle`: eye / high（俯瞰）/ low（あおり）/ bird / worm / dutch
- `characters[].pos`: left / left_third / center / right_third / right。`scale` は人物の大きさ（0〜1）。`facing` は向き。
- 場面（scene）の最初のコマは状況が分かるショット（ELS / LS / FS）にする。
- 同じ `shot` を3コマ以上続けない。
- 同じ場面の2人の左右を入れ替えない（180度ルール）。わざと入れ替えるコマは `cross: true`。

## 台詞（lines）

- 脚本で、そのページの dialogue / monologue / narration の beat は、どこかのコマに **ちょうど1回** 置く。
- `breaks` は縦書きの1行ずつ。1行 14 字まで、フキダシ1つで 40 字まで。自然な切れ目で改行する。
- `balloon`: speech（話す）/ thought（心の声）/ shout / whisper / narration（四角い枠）。
- フキダシはコマの右上から読み順に置かれ、人物の顔の位置を避ける。入らなければ `balloon_overflow` が返るので、台詞を短くするか、コマを大きくするか、台詞を別のコマに移す。
- 話者がそのコマにいないと、フキダシの尾が付かない（画面外の声として扱う）。

## めくり

- 右綴じでも左綴じでも、ページをめくって最初に見えるのは **偶数ページ** である。
- 驚きや見せ場（`reveal`）は偶数ページの先頭に置く。脚本の beat と、ネーム計画の `turn_role: "reveal"` の両方で示す。
- 引き（`hook`）は奇数ページの最後のコマに置く。
- 1ページ目は表紙の次に単独で見えるページ。

## 絵について

- Genko のネームは構図と台詞の設計図である。絵はまだ描かない（画像の工程は後の段階）。
- 台詞・効果音は画像に描かせない。文字は Genko が縦書きで描く。
