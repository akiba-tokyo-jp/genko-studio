# AGENT.md — Genko for generative AI

Drive Genko **headless**. Do not open the GUI. Stdout is JSON. No Qt required for `inspect` / `apply` / `export` / `serve`.

## Contract

- Project = a folder with `project.json` (`.genko`).
- Name gate: `name_ok` must be true before `advance` to `ink` or `add_stroke` with `"layer":"ink"`.
- Export never includes name/draft strokes.
- Coordinates are millimetres. Page 1 is 1-based.
- `inspect` is compact (no stroke points). Use it as working memory.

## CLI

```bash
python -m genko new ./demo.genko --title 試作 --pages 8 --json
python -m genko inspect ./demo.genko
python -m genko apply ./demo.genko ops.json
python -m genko apply ./demo.genko -          # ops JSON on stdin
python -m genko export ./demo.genko ./out --json
python -m genko schema
python -m genko serve --port 8765
```

`ops.json` is a JSON **array**:

```json
[
  {"op": "split_frame", "page": 1, "axis": "vertical", "ratio": 0.5, "gutter_mm": 4},
  {"op": "add_line", "page": 1, "text": "始めよう。", "speaker": "主人公"},
  {"op": "name_ok", "page": 1},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 30], [80, 30]]}
]
```

On failure stdout is `{"ok": false, "error": "..."}` and exit code is 1.

## HTTP (headless)

`GET /health`  
`GET /schema`  
`GET /v1/inspect?path=`  
`POST /v1/new` `{"dest","title","pages","webtoon?"}`  
`POST /v1/apply` `{"path","ops":[...]}`  
`POST /v1/export` `{"path","out","dpi?"}`  

## Human

`python -m genko app` — same files, mouse and menus. AI and humans share `.genko`.
