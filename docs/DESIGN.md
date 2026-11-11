# Genko Studio 設計

Windows と Linux で動く、漫画原稿のオペレーティングシステム。
CLIP STUDIO PAINT EX の全描画エンジンを複製しない。連載工場に必要な**ページOS・ゲート・コマ・セリフ・下描き除外の出荷**を本体にする。

## 何を置き、何を置かないか

置く（P0–P9）

- レイヤー（name は exportable=false）。v1 ファイルはマイグレーション
- コマクリップ合成。`render mode=name|proof|print`
- すべての変更は `apply_ops`（GUI も AI も）。dry_run と undo
- Stroke（筆圧Gペン）と作業ラスタ。消しゴムは画素
- 見開き1枚、縦組みルビ、フキダシ path
- トーンカタログ（幾何）。パース拘束。棒人間（print に出ない）
- ページ追加／削除／複製、セリフ編集／移動／フキダシ／ルビ
- コマ merge/resize/bleed、put_raster、ベタ、トーン網点、集中線／流線
- 2階調 TIFF、PDF、Webtoon 縦結合、B4／出版社プリセット数値、ノンブル、トンボ、入稿パック
- 油彩風の半透明混色（物理油彩ではない）、オニオンスキン、ページ単位ロック（OTではない）
- ユーザーレイヤー追加／削除、合成（normal/multiply/screen/add）、下のレイヤーでクリップ、フォルダ、透明ロック
- フィルター（ぼかし／シャープ／色相／レベル／カーブ／モザイク／二値化）
- LT しきい、オニオン頁送り、手首足首、見開き用紙、色相スライダ、クリップ、ペン先プレビュー

置かない（意図的。CSP互換を謳わない）

- CSP プラグイン、クラウドの OT/CRDT、イワタ同梱
- 印刷機プレート相当の網点、油彩物理、ボーンマーケットの3D

本体は原稿の状態機械である。画像生成は `put_raster` の外側。

## 状態機械

```
name → (name_ok) → ink → finish → export
```

`advance(page, "ink")` は `name_ok` が False なら拒む。

書き出しレイヤー順: BG → INK → FINISH → TONE → EFFECT → FRAMES → TEXT。NAME と DRAFT は乗らない。

## データ

単位はミリメートル。作業 dpi は 150–300、入稿時だけ 600。

```
title.genko/
  project.json          # version 4（docs/cpp-migration/schema-v4.md）
  project.lock
  assets/ab/<sha256>.*  # 画素・線・状態。書き換えない
  studio/journal.jsonl  # 保存ごとの取引（Undo・Redo）
```

ページは木構造のコマを持つ。葉だけが描画領域。縦分割の葉の順は右が先（日本語）。

## モジュール

- `native/src/core` — 原稿のモデル（Document / Page / Frame / Line / PageSpec）、コマンドバス、ゲート
- `native/src/storage` — 保存（v4 の形式、journal、復旧）
- `native/src/render` — 合成、op、フィルター
- `native/src/formats` — 読み込みと書き出し（出荷）
- `native/src/service`・`native/src/studio` — AI と作る工程（台本、ネーム、承認）
- `native/src/api` — CLI（`genko`）、HTTP、MCP
- `native/src/app` — Qt UI（Windows / Linux）

## UI

左: ページ、レイヤー（合成）、サブビュー、チケット。中央: 用紙キャンバス。右: ストーリー、ゲート、コマ割り。ツールバー: ペン(B) 消しゴム(E) 色(C) 太さ([ ])。中ドラッグでパン。Ctrl+Z。

## 実行

```
genko app
genko new ./demo.genko --title 試作 --pages 8
genko inspect ./demo.genko
genko apply ./demo.genko ops.json
genko serve --root . --port 8765
genko export ./demo.genko ./out --format tiff --json
```

組み立てと試験は README.md の「Build from source」（`cmake --preset linux-release`、`ctest`）。人は GUI（Qt）。生成AIは画面なしの CLI・HTTP・MCP。操作仕様は `docs/AGENT.md`。JSON Schema は `docs/ops.schema.json`。
