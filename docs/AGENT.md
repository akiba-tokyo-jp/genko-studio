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

## Studio: write the name through MCP

Genko does not call any LLM or image model. You (the agent) write the bible, the script and one name plan per page; Genko checks them, splits the panels, letters vertical balloons and returns a preview image. Only a human can approve.

Connect (same machine, stdio). Hermes Agent `~/.hermes/config.yaml`:

```yaml
mcp_servers:
  genko:
    command: "genko"
    args: ["mcp", "--root", "/home/you/manga", "--agent", "ai:hermes"]
```

Install with the extra: `uv sync --extra mcp` (or `pip install "genko-studio[mcp]"`). The skill for Hermes is `integrations/hermes/genko-manga/SKILL.md`.

Tools: `projects`, `create_project`, `status`, `next`, `inspect` (bible / script / page / panel / studio / schemas / rules / snapshot), `render` (a page, one panel with `frame_id`, or `kind` compare / guides), `import_image`, `set_bible`, `set_script`, `submit_name`, `apply_ops` (allow-listed ops only), `record_review`, `request_approval` (gate name / art / sheet / export), `tickets`, `generation_request`, `import_images`, `candidates`, `review_candidates`, `adopt`, `request_fix`, `report_regions`, `finish_page`, `preflight`, `export_proof`, `review_page`, `ask_human`. Resources: `genko://guide/skill`, `genko://guide/manga-rules`, `genko://ops` (the ops `apply_ops` accepts).

- Writing tools default to `commit: false`: they return `issues` (`{code, severity, path, message, hint}`, `path` is a JSON pointer into your input) and a preview. Fix what `path` points at, then send `commit: true`.
- Name plan tiers run top to bottom; the cols inside a tier are listed right to left.
- Everything the agent writes lives in project.json (M3): the bible (`episode.bible` + `studio.bible_doc`), the script (`studio.script`), each page's name plan and self-checks (`page.plan`), each panel's brief, candidates and adoption (`frame.panel`), and tickets. Projects from M0 with `studio/drafts/` are moved in with `genko studio adopt-drafts PROJECT`.

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
genko studio approve demo.genko sheet --character hina --candidate c1 --as human:leaf
genko studio approve demo.genko art --pages 1 --as human:leaf
genko studio revoke demo.genko name --pages 2 --reason "コマ割りを変える" --as human:leaf
genko studio comment demo.genko --page 2 "2コマ目の台詞を減らして" --as human:leaf
genko studio comment demo.genko --page 2 --frame FRAME_ID "空をもっと暗く" --as human:leaf
```

## Art: requests, imports, finishing and export (M3–M4)

Genko makes no images. It writes a generation request, the agent generates with its own image tool, and Genko takes the result back as candidates.

1. `next` gives the work in order: `make_sheet` → (person approves the sheet) → `gen_panel` → `import_pending` → `review_candidates` → `fix_panel` → `report_regions` → (person approves the art) → `upscale_panel` → `finish_page` → (person exports). Items another agent claimed (`next claim=true`, 10-minute lease) and items parked by `ask_human` show as blocked.
2. `generation_request {page, frame_id}` (or `character_id` for a sheet, `location_id` for a background reference; `mode: edit|inpaint|upscale` with `parent`) → `studio/requests/<id>/request.json` with sizes (`suggested_px` ≈ 1 MP in multiples of 64, `tool_sizes` from `tools.json`, `print_px`), a prompt draft (`ja`, `en`, `tags`; character `tokens_en` as written), `avoid`, `keepout` (balloon areas, 0..1), `figures`, and files: `guides/composition.png`, `guides/pose.png`, `guides/keepout.png`, `refs/*` (approved sheets and faces, location images), `source.png`/`mask.png` for fixes. Same content → same id.
3. Save the generated images in `studio/inbox/<id>/` (or upload with `POST /v1/assets?path=PROJECT`, body = the image) and call `import_images {request_id, images: [{file | asset, origin: {kind, tool_id, model, prompt, params, refs_used}}]}`. Only the inbox and uploaded assets are read; 30 MB / 64 MP per image; the same image twice adds nothing. Each candidate gets `metrics` (aspect error, how busy the balloon areas are, brightness, edge overlap with the pose guide; lower `rank` is better).
4. `candidates`, `render kind=compare` (the candidate with the name in red), `review_candidates`, then `adopt`. `report_regions` (faces and people, `box01` in the adopted image or `rect_mm`) feeds the balloon pass.
5. After the person approves the art: `finish_page` (moves balloons off reported faces, turns `panel.fx` into effects, advances to finish). `preflight` lists what blocks the export; `export_proof` writes a 150 dpi proof with a 校正 watermark into `studio/proofs/`.

Rules:

- The image is kept as its asset and resampled from the source at print time into its placement, then clipped to its panel (`bleed` panels extend to the paper edge on their outer sides). A generated image covers the panel plus the request's 3 mm pad. Gutters stay white.
- Art (`to: art|bg`) needs the page's name approved; `to: draft` works any time and never prints. Candidates never print.
- With `strict_gates` (studio projects) an approved name freezes the layout for agents; `advance to=finish` needs `art_ok`.
- `split_frame` gives the brief to the panel read first; `merge_frame` keeps the first panel's brief. Placed art needs `force: true` and moves to `studio.orphans`. `duplicate_page` keeps art on the new panel ids.
- A person's pins, prompt/size overrides (`gen.prompt_override` replaces the prompt draft), regions and `skip` cannot be changed by an agent. Approvals are person-only; an approved sheet locks the character and its face is cut out as a `face` reference.
- Limits (`studio.policy.limits`): 8 images and 2 fix rounds per panel, then the panel waits for a person.
- The final export is a person's: `genko studio export PROJECT --format pdf|tiff|png --out DIR --as human:NAME`. It refuses with reasons: pages not finished or not approved, panels without art, unplaced lines, missing assets, effective resolution under 350 dpi (`--force`), test images (`--allow-fixture`). Missing provenance only warns.

Pilot page, style and multi-character panels (M5):

- Art starts on the pilot page (`studio.policy.pilot_page`, default the first page; `policy.pilot: false` turns it off). Other pages' panel work shows as blocked (`pilot:1`) until its art is approved.
- Approving the pilot art fixes the style (`studio.style.locked`): the image tool used most for it and a reference image (its largest panel). Later requests default to that tool and carry `refs/style_pilot.png`. Revoking the pilot art unfixes it.
- A request for a panel with two or more characters lists `steps`: generate the whole composition, report regions, then fix people one at a time with `mode: inpaint`, `parent` and `focus_character` (mask from that person's regions, only their references). `regions: ["face:hina"]` redraws only a face.

People and records (M5):

- `review_page` (agent) writes `studio/review.html` with previews, candidates, open requests and the commands a person runs; the agent sends its path to the person. `genko studio close-ticket PROJ ID --reply "…" --as human:NAME` answers an `ask_human` question (the reply reaches the agent as a fix ticket).
- Every MCP tool call is logged to `studio/logs/tools.jsonl` (tool, ok, error codes, time; no documents or images). `genko studio stats PROJ` summarises a run.
- Every approval change is appended to `studio/audit.jsonl` with its actor (never trimmed). `genko studio audit PROJ` fails if anyone but a person changed one. `undo`/`redo` that would change an approval are refused for agents.
- D5: `genko studio eval-sample PROJ --out d5` and `genko studio eval-score d5 answers.csv`. See `docs/STUDIO_EVAL.md`.

Printing in black and white (M6):

- Monochrome pages (`spec.expression` "mono") finish placed art at render time, without touching the asset: levels, a line mask (dark and locally contrasted pixels print solid), solid black and paper white, and the mid greys in flat tone steps (default 10 / 20 / 30%). `print` draws the steps as AM dots (60 lpi, 45°) with the exact black share at any dpi (±1%), so the page is pure black and white; `proof` and `name` show the flat greys instead of dots. Settings: `studio.style.finish` for the book, `set_finish {page, frame_id, finish}` per panel: `{black, white, line_threshold, line_contrast, steps, lpi, angle, screen: am|fm}`. Colour pages are left as they are.
- Tone layers (`add_tone`, `stamp_material`) use the same dots; noise materials use FM (error diffusion).
- `derive {page, frame_id, kind: "lineart"}` extracts the lines of the adopted art (or `candidate_id`) as a black-on-transparent candidate (origin `genko`, mode `derive`, not counted in the image budget). `adopt … to: "ink"` puts it over the toned art for crisp lines.
- Balloons: ellipses are sized to go around the text block (half-size × √2 + pad; the name lettering measures them the same way). Tails leave from the side that faces the speaker with a base of a third of the short side. `shout` is spiky, `whisper` dashed, `thought` trails small bubbles, `narration` is a box, `sfx` is large outlined lettering without a balloon. Ruby sits beside every base it belongs to. Focus and speed lines are clipped to their panel and focus lines leave the centre clear.
- Export: print, pack and PSD default to the page spec's dpi (B4 comic: 600); strip and EPUB to 150. File names are made safe for Windows. EPUB is EPUB 3, fixed layout, right to left for right-bound books. PSD: `genko export PROJ OUTDIR --format psd` writes one layered PSD per page (paper, each placed image as greyscale art, raster layers, ink, tone, effects, panel borders, one layer per balloon, page number; Unicode layer names, cropped layers, resolution set). The manual check for CLIP STUDIO PAINT and Photoshop is `docs/PSD_CHECKLIST.md`.

Image tools (`tools.json` in the config dir, never in a project):

```bash
genko studio tools example                       # an example entry
genko studio tools set openai:gpt-image-1        # or --file spec.json with {label, sizes_px, supports, notes}
genko studio tools list
```

Every agent tool is also on the shell: `genko studio call PROJECT generation_request args.json`.

HTTP: `GET /v1/pages/{n}/frames/{frame_id}.png` renders one panel; `POST /v1/assets?path=PROJECT` (image body) returns `{asset, px}`; `GET /v1/requests/{id}/files/{name}?path=PROJECT` serves a request's guides and references.
