# AGENT.md — Genko for generative AI

Drive Genko **headless**. Do not open the GUI. Stdout is JSON. No Qt required.

## Loop

1. `inspect` for compact state (no stroke coordinates)
2. `render` or `GET /v1/pages/{n}.png?mode=name` to **see** the page
3. `apply` ops (`--dry-run` to preview)
4. Repeat
5. `export` with print mode (name/draft never included)

## Contract

- Project = folder with `project.json`
- `name_ok` before ink strokes
- mm coordinates, page index 1-based
- Failed apply is transactional (nothing from that request is kept)
- `project.lock` while `apply` runs

## CLI

```bash
python -m genko new ./demo.genko --title 試作 --pages 8 --json
python -m genko inspect ./demo.genko
python -m genko inspect ./demo.genko --full
python -m genko apply ./demo.genko ops.json
python -m genko apply ./demo.genko ops.json --dry-run
python -m genko render ./demo.genko --page 1 --mode name --out p1.png
python -m genko export ./demo.genko ./out --json
python -m genko schema
python -m genko serve --port 8765
```

Ops include: `split_frame`, `add_line`, `edit_line`, `delete_line`, `name_ok`, `advance`, `add_stroke`, `delete_stroke`, `add_page`, `delete_page`, `duplicate_page`, `set_note`, `reorder`, `undo`.

## HTTP

`GET /health`  
`GET /schema`  
`GET /v1/inspect?path=&full=0`  
`GET /v1/pages/{n}.png?path=&mode=name|proof|print`  
`POST /v1/new`  
`POST /v1/apply` `{"path","ops","dry_run?"}`  
`POST /v1/export`

## Human

`python -m genko app` — click a panel to select before splitting. Ctrl+Z undoes. Same `.genko`.
