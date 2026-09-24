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
python -m genko apply ./demo.genko ops.json --agent ai:myagent
python -m genko serve --root ./manga --port 8765   # HTTP needs a token: genko token add --actor ai:myagent
```

Ops include: split/merge/resize/set_frame, add/edit/delete/move_line, name_ok, advance, add/delete/edit/simplify_stroke, put_raster, flood_fill, add/delete_tone, add_effect, add_prim3d, set_ruler, lt_convert, add/set_ticket, page CRUD, set_meta/bible/spread/autosave, undo.

## Project format (v3)

`project.json` holds decisions only (version 3, `revision` +1 per save). Raster bytes and strokes live in `assets/ab/<sha256>` and are never rewritten. Every save appends a line to `studio/journal.jsonl` with the project.json before and after, so:

```bash
python -m genko undo ./demo.genko --as ai:myagent     # only your own latest change (--force for others)
python -m genko redo ./demo.genko --as ai:myagent
python -m genko apply ./demo.genko ops.json --agent ai:myagent --expect-revision 12   # fail if someone saved since
python -m genko doctor ./demo.genko                   # missing assets, font, long paths
python -m genko gc ./demo.genko --dry-run             # unreferenced assets (--legacy removes the v2 pages/ folder)
```

Files written by a newer Genko are refused; v2 projects are read and upgraded on the first save (`project.v2.bak.json` keeps the original). Pages have stable ids; page locks follow the page. Spreads follow the binding: in a right-bound book the even page is on the right. `add_stroke{space:"spread"}` takes x from the spread's left edge. Studio projects are `strict_gates`: printed layers change only after `name_ok`, `add_line` with a frame needs coordinates, and spreads must face.

## Actors

Always name yourself: `--agent ai:<name>` on the CLI, a token per actor over HTTP. `ai:*` actors cannot approve (`name_ok`), cannot unlock or take over a person's page lock, and cannot lock a page in someone else's name. In a studio project (has `studio/`), an unnamed CLI call is `legacy:unknown` and cannot approve either.

## HTTP

Every route except `/health` needs `Authorization: Bearer <token>` (create one with `genko token add --actor ai:<name>`; tokens live in the user config dir, `GENKO_CONFIG_DIR` overrides it). Bodies must be `Content-Type: application/json`. Paths are relative to `--root` (absolute paths must be inside it). Browser origins are refused unless allowed with `--allow-origin`.


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

## Studio (M0): write the name through MCP

Genko does not call any LLM or image model. You (the agent) write the bible, the script and one name plan per page; Genko checks them, splits the panels, letters vertical balloons and returns a preview image. Only a human can approve.

Connect (same machine, stdio). Hermes Agent `~/.hermes/config.yaml`:

```yaml
mcp_servers:
  genko:
    command: "genko"
    args: ["mcp", "--root", "/home/you/manga", "--agent", "ai:hermes"]
```

Install with the extra: `uv sync --extra mcp` (or `pip install "genko-studio[mcp]"`). The skill for Hermes is `integrations/hermes/genko-manga/SKILL.md`.

Tools: `projects`, `create_project`, `status`, `next`, `inspect` (bible / script / page / schemas / rules / snapshot), `render`, `set_bible`, `set_script`, `submit_name`, `apply_ops` (allow-listed ops only), `record_review`, `request_approval`, `tickets`. Resource: `genko://guide/manga-rules`.

- Writing tools default to `commit: false`: they return `issues` (`{code, severity, path, message, hint}`, `path` is a JSON pointer into your input) and a preview. Fix what `path` points at, then send `commit: true`.
- Name plan tiers run top to bottom; the cols inside a tier are listed right to left.
- AI drafts live in `<project>/studio/drafts/`; project.json keeps its current format.

The same tools from the shell (project is a path):

```bash
genko studio init demo.genko --pages 8 --title 試作
genko studio set-bible demo.genko bible.json --commit
genko studio set-script demo.genko script.json --commit
genko studio submit-name demo.genko p001.json --commit
genko studio next demo.genko
```

Human only:

```bash
genko studio review demo.genko --out review.html            # previews, briefs, approve commands
genko studio approve demo.genko name --pages 1-4 --as human:leaf
genko studio comment demo.genko --page 2 "2コマ目の台詞を減らして" --as human:leaf
```
