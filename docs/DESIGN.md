# Genko Studio 設計

Windows と Linux で動く、漫画原稿のオペレーティングシステム。
CLIP STUDIO PAINT EX の全描画エンジンを複製しない。連載工場に必要な**ページOS・ゲート・コマ・セリフ・下描き除外の出荷**を本体にする。

## 何を置き、何を置かないか

置く（P0–P4）

- レイヤー（name は exportable=false）。v1 ファイルはマイグレーション
- コマクリップ合成。`render mode=name|proof|print`
- すべての変更は `apply_ops`（GUI も AI も）。dry_run と undo
- ページ追加／削除／複製、セリフ編集／移動／フキダシ／ルビ
- コマ merge/resize/bleed、put_raster、ベタ、トーン網点、集中線／流線
- 2階調 TIFF、PDF、Webtoon 縦結合、B4／出版社プリセット数値、ノンブル、トンボ
- 筆圧付き折れ線、ベクター簡略化、パース定規、箱の3Dガイド、LT（エッジ→線）
- 助手チケット、最小 PSD（8BPS＋文字レイヤー名）、EPUB
- ページPNG、OpenAPI、job_id、project.lock（15分で失効）
- 選択コマの分割、保存でフォルダ作成、GUI レイヤー／セリフドラッグ／自動保存

置かない（意図的。CSP互換を謳わない）

- 油彩混色、フルアニメ、CSP プラグイン、クラウドチームの OT
- 商業印刷機と同等の網点／イワタフォント同梱
- 本格ボーン付き3D／リアルタイム LT 変換エンジン

本体は原稿の状態機械である。画像生成は `put_raster` の外側。

## 状態機械

```
name → (name_ok) → ink → finish → export
```

`advance(page, "ink")` は `name_ok` が False なら `InkBlockedError`。

書き出しレイヤー順: BG → INK → FINISH → TONE → EFFECT → FRAMES → TEXT。NAME と DRAFT は乗らない。

## データ

単位はミリメートル。作業 dpi は 150–300、入稿時だけ 600。

```
title.genko/
  project.json
  project.lock
  pages/001/*.png
```

ページは木構造のコマを持つ。葉だけが描画領域。縦分割の葉の順は右が先（日本語）。

## モジュール

- `genko.models` — Episode / Page / Frame / StoryLine / PageSpec
- `genko.ops` — コマンドバス
- `genko.pipeline` — ゲート
- `genko.render` / `genko.export` — 合成と出荷
- `genko.io` — 保存
- `genko.app` — Qt UI（Windows / Linux）
- `python -m genko` — CLI

## UI

左: ページ、レイヤー、チケット。中央: 用紙キャンバス（ネームは青、ペン入れは黒、フキダシ）。右: ストーリー、ゲート、コマ割り。中ドラッグでパン。Ctrl+Z。

## 実行

```
uv sync --extra app --extra dev
uv run python -m genko app
uv run python -m genko new ./demo.genko --title 試作 --pages 8
uv run python -m genko inspect ./demo.genko
uv run python -m genko apply ./demo.genko ops.json
uv run python -m genko serve --port 8765
uv run python -m genko export ./demo.genko ./out --format tiff --json
uv run pytest
```

Python 3.11+。人は GUI（PySide6）。生成AIは Qt なし。操作仕様は `docs/AGENT.md`。JSON Schema は `docs/ops.schema.json`。
