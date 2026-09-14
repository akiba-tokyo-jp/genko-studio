# Genko Studio 設計

Windows と Linux で動く、漫画原稿のオペレーティングシステム。
CLIP STUDIO PAINT EX の全描画エンジンを複製しない。連載工場に必要な**ページOS・ゲート・コマ・セリフ・下描き除外の出荷**を本体にする。

## 何を置き、何を置かないか

置く（v0.1で実装）

- 複数ページの作品管理（追加・並べ替え・右綴じ）
- A4モノクロ / Webtoon 用紙（塗り足し・基本枠）
- ネーム段階とペン入れ段階の分離。ネームOKが出るまで ink に進めない
- コマ枠の分割（横／縦、アキ、日本語の右→左読み順）
- ストーリーエディター（ページ／コマにセリフ）
- 下描き（NAME/DRAFT）を書き出しから除外
- `.genko` フォルダ保存
- PNG連番書き出し
- デスクトップGUI（人）とヘッドレス JSON CLI / HTTP（生成AI）。同じ `.genko`

置かない（意図的。CSP互換を謳わない）

- 3D / LT変換
- トーン網点エンジン
- ベクターペンの完全再現
- チーム制作クラウド
- アニメーション
- 出版社プリセットの全網羅

これらは後からプラグイン層に足す。本体は原稿の状態機械である。

## 状態機械

```
name → (name_ok) → ink → finish → export
```

`advance(page, "ink")` は `name_ok` が False なら `InkBlockedError`。

書き出しレイヤー順: BG → INK → FINISH → FRAMES → TEXT。NAME と DRAFT は乗らない。

## データ

単位はミリメートル。ラスタは書き出し時に dpi で焼く。

```
title.genko/
  project.json
```

ページは木構造のコマを持つ。葉だけが描画領域。縦分割の葉の順は右が先（日本語）。

## モジュール

- `genko.models` — Episode / Page / Frame / StoryLine / PageSpec
- `genko.pipeline` — ゲート
- `genko.export` — 出荷
- `genko.io` — 保存
- `genko.app` — Qt UI（Windows / Linux）
- `python -m genko` — CLI

## UI

左: ページ管理。中央: 用紙キャンバス（ネームは青、ペン入れは黒）。右: ストーリーエディター、ネームOK、コマ割り。

ホイールでズーム。保存はフォルダ選択（`.genko`）。

## 実行

```
uv sync --extra app --extra dev
uv run python -m genko app
uv run python -m genko new ./demo.genko --title 試作 --pages 8 --json
uv run python -m genko inspect ./demo.genko
uv run python -m genko apply ./demo.genko ops.json
uv run python -m genko serve --port 8765
uv run python -m genko export ./demo.genko ./out --json
uv run pytest
```

Python 3.11+。人は GUI（PySide6）。生成AIは Qt なしで `inspect` / `apply` / `serve`。操作仕様は `docs/AGENT.md`。
