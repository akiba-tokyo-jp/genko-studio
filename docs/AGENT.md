# AGENT.md — Genko for generative AI

Drive Genko **headless**. Do not open the GUI. Stdout is JSON. No Qt required.

## Loop

1. `inspect` for compact state (no stroke coordinates)
2. `render` or `GET /v1/pages/{n}.png?mode=name` to **see** the page
3. `apply` ops (`--dry-run` to preview)
4. Repeat
5. `export --format png|tiff|pdf|strip|psd|epub` with print mode (name/draft never included)

## Contract

- Project = folder with `project.json`
- `name_ok` before ink strokes / flood_fill / lt_convert to ink
- mm coordinates, page index 1-based
- Failed apply is transactional (nothing from that request is kept)
- `project.lock` while `apply` runs (stale after 15 minutes)
- `put_raster` is the image-generation boundary. Genko does not invent pixels.
- `inspect --stroke ID` returns one polyline. Compact inspect still omits all points.
- Spread: `GET /v1/spreads/{left}-{right}.png`

## CLI

```bash
python -m genko new ./demo.genko --title 試作 --pages 8
python -m genko new ./demo.genko --b4 --preset shueisha
python -m genko inspect ./demo.genko
python -m genko inspect ./demo.genko --full
python -m genko apply ./demo.genko ops.json
python -m genko apply ./demo.genko ops.json --dry-run
python -m genko render ./demo.genko --page 1 --mode name --out p1.png
python -m genko export ./demo.genko ./out --format tiff --json
python -m genko schema
python -m genko serve --port 8765
```

Ops include: split/merge/resize/set_frame, add/edit/delete/move_line, name_ok, advance, add/delete/edit/simplify_stroke, put_raster, flood_fill, add/delete_tone, add_effect, add_prim3d, set_ruler, lt_convert, add/set_ticket, page CRUD, set_meta/bible/spread/autosave, undo.

## HTTP

`GET /health`  
`GET /schema`  
`GET /openapi.json`  
`GET /v1/inspect?path=&full=0`  
`GET /v1/pages/{n}.png?path=&mode=name|proof|print`  
`GET /v1/jobs/{id}`  
`POST /v1/new`  
`POST /v1/apply` `{"path","ops","dry_run?"}` → `{ok, applied, snapshot, job_id}`  
`POST /v1/export`

Locked writer → HTTP 409.

## Human

`python -m genko app` — click a panel to select before splitting. Drag a balloon to `move_line`. Ctrl+Z undoes. Same `.genko`.
