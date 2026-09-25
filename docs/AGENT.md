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

`python -m genko app [PROJECT]` (needs the `app` extra). Without a project a start screen lists recent manuscripts and makes new ones (title, pages, paper: B4 / A4 / webtoon, binding, folder).

- Layout: pages on the left, the page in the middle, panels on the right as tabs (承認箱, コマ, 台詞, レイヤー, ライブラリ; the パネル menu shows hidden ones). It fits a 1280×720 screen (the tall panel view scrolls). The status bar shows the page, its stage, the selected panel, whether changes are saved, who you are, the process counts and the zoom.
- The page is shown as Genko renders it (name render before the name approval, proof after: placed art, tones, vertical lettering), re-rendered at the zoom's resolution. It opens fitted to the view. Ctrl+0 fits, Ctrl+1 is paper size, Ctrl+wheel / pinch / Ctrl+± zoom, the wheel or two fingers scroll, Space+drag or the middle button pan.
- Tools: 選択 (V: click a panel to select it, drag a balloon to move it, drag the paper to move the view), ペン (B) and 消しゴム (E) draw on the name layer before the name approval and on ink after. Right-click a panel for split / merge.
- Every change is an op applied in memory as `human:<name>` (`$GENKO_USER`, else the login name) and shown at once. Changes are written after a second without edits, on a page switch, right after an approval and on close. When an agent commits in between (the window watches project.json), the window reloads and replays its own pending ops on top; ops that no longer apply are listed instead of overwriting the agent's change. Ctrl+Z undoes (pending edits in memory, or your own last saved change through the journal); Ctrl+Shift+Z / Ctrl+Y redoes. Deleting a page asks first. Error messages are in plain Japanese.
- Approval box: the agent's requests and questions. Selecting one moves the page to it. Look at the preview, or 大きく見る (a zoomable viewer; ← → between the request's pages; sheet candidates side by side), then approve it or send it back with a reason. There is no bulk approval. Sending back becomes an instruction the agent sees in `next` (a page fix for a name, panel fixes for art, a rejected sheet with a note, a reply to a question). The export request opens the export dialog in official mode.
- Panel view: click a panel on the page. Show it as printed, as a proof, compared with the name, or the source image of a candidate; 大きく見る and 候補を並べて比べる open the viewer. Candidates show their score and provenance (tool, model, the prompt actually used); adopt one; send an instruction (pinned on the panel and sent to the agent as a fix); draw regions (顔, 人物, 空けておく) that the agent may not change.
- 書き出し… (Ctrl+E): PDF, TIFF, PNG, PSD, 入稿セット, 縦読み (webtoon), SNS, EPUB, つなげた 1 枚, with the options of each format. Any format can be written freely; "正式な書き出し" (pdf / tiff / png / webtoon / sns) runs the preflight and records the export approval.
- Library: characters (approved sheet and face, the look and `tokens_en`) and locations with their references.
- The manual checklist is `docs/GUI_CHECKLIST.md`.
- review.html (written by the agent's `review_page`) is for looking, also on a phone; it points to the app for decisions and keeps the equivalent commands folded. `--as` is optional on the human commands (default `human:$GENKO_USER` or the login name).

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
genko studio approve demo.genko name --pages 1-4
genko studio approve demo.genko sheet --character hina --candidate c1
genko studio approve demo.genko art --pages 1
genko studio revoke demo.genko name --pages 2 --reason "コマ割りを変える"
genko studio comment demo.genko --page 2 "2コマ目の台詞を減らして"
genko studio comment demo.genko --page 2 --frame FRAME_ID "空をもっと暗く"
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
- The final export is a person's: `genko studio export PROJECT --format pdf|tiff|png --out DIR`. It refuses with reasons: pages not finished or not approved, panels without art, unplaced lines, missing assets, effective resolution under 350 dpi (`--force`), test images (`--allow-fixture`). Missing provenance only warns.

Pilot page, style and multi-character panels (M5):

- Art starts on the pilot page (`studio.policy.pilot_page`, default the first page; `policy.pilot: false` turns it off). Other pages' panel work shows as blocked (`pilot:1`) until its art is approved.
- Approving the pilot art fixes the style (`studio.style.locked`): the image tool used most for it and a reference image (its largest panel). Later requests default to that tool and carry `refs/style_pilot.png`. Revoking the pilot art unfixes it.
- A request for a panel with two or more characters lists `steps`: generate the whole composition, report regions, then fix people one at a time with `mode: inpaint`, `parent` and `focus_character` (mask from that person's regions, only their references). `regions: ["face:hina"]` redraws only a face.

People and records (M5):

- `review_page` (agent) writes `studio/review.html` with previews, candidates, open requests and the commands a person runs; the agent sends its path to the person. `genko studio close-ticket PROJ ID --reply "…"` answers an `ask_human` question (the reply reaches the agent as a fix ticket).
- Every MCP tool call is logged to `studio/logs/tools.jsonl` (tool, ok, error codes, time; no documents or images). `genko studio stats PROJ` summarises a run.
- Every approval change is appended to `studio/audit.jsonl` with its actor (never trimmed). `genko studio audit PROJ` fails if anyone but a person changed one. `undo`/`redo` that would change an approval are refused for agents.
- D5: `genko studio eval-sample PROJ --out d5` and `genko studio eval-score d5 answers.csv`. See `docs/STUDIO_EVAL.md`.

Printing in black and white (M6):

- Monochrome pages (`spec.expression` "mono") finish placed art at render time, without touching the asset: levels, a line mask (dark and locally contrasted pixels print solid), solid black and paper white, and the mid greys in flat tone steps (default 10 / 20 / 30%). `print` draws the steps as AM dots (60 lpi, 45°) with the exact black share at any dpi (±1%), so the page is pure black and white; `proof` and `name` show the flat greys instead of dots. Settings: `studio.style.finish` for the book, `set_finish {page, frame_id, finish}` per panel: `{black, white, line_threshold, line_contrast, steps, lpi, angle, screen: am|fm}`. Colour pages are left as they are.
- Tone layers (`add_tone`, `stamp_material`) use the same dots; noise materials use FM (error diffusion).
- `derive {page, frame_id, kind: "lineart"}` extracts the lines of the adopted art (or `candidate_id`) as a black-on-transparent candidate (origin `genko`, mode `derive`, not counted in the image budget). `adopt … to: "ink"` puts it over the toned art for crisp lines.
- Balloons: ellipses are sized to go around the text block (half-size × √2 + pad; the name lettering measures them the same way). Tails leave from the side that faces the speaker with a base of a third of the short side. `shout` is spiky, `whisper` dashed, `thought` trails small bubbles, `narration` is a box, `sfx` is large outlined lettering without a balloon. Ruby sits beside every base it belongs to. Focus and speed lines are clipped to their panel and focus lines leave the centre clear.
- Export: print, pack and PSD default to the page spec's dpi (B4 comic: 600); strip and EPUB to 150. File names are made safe for Windows. EPUB is EPUB 3, fixed layout, right to left for right-bound books. PSD: `genko export PROJ OUTDIR --format psd` writes one layered PSD per page (paper, each placed image as greyscale art, raster layers, ink, tone, effects, panel borders, one layer per balloon, page number; Unicode layer names, cropped layers, resolution set). The manual check for CLIP STUDIO PAINT and Photoshop is `docs/PSD_CHECKLIST.md`.

Hand-drawn names (M8):

- `genko studio import-name PROJ scan1.png scan2.png … [--start-page N] [--align auto|page|live]` (or the MCP tool `import_name` with files under `--root`). Each scan becomes an asset (origin `self`), is placed on DRAFT (never printed) and aligned: `page` = the scan is the whole sheet, `live` = the drawing fills the live area, `auto` = `live` when the drawing has the live area's shape, else `page`.
- Genko finds the panels (XY-cut on the scan: gutters between panel borders, specks and dialogue scribbles ignored) and proposes a layout with a confidence per panel; everything the analysis produced is in `studio/analysis/<page id>/`. `analyze_name {page, params}` runs it again.
- The agent reads the handwritten lines and proposes them: `propose_lines {page, lines: [{text ("\n" between columns), balloon?, speaker?, box01 in the scan | x_mm, y_mm[, w_mm, h_mm]}]}`. If a page has no lines, `record_review {page, kind: "atari_lines"}` says so.
- Nothing changes on the page until a person accepts: `genko studio accept PROJ PROPOSAL` (a layout over existing panels needs `--force`), `genko studio reject PROJ PROPOSAL --note "…"`, or the approval box in the app. Proposals show as an overlay in `render kind=atari`, review.html and the app.
- `next` on such pages: `read_atari`, then (after the layout is accepted) `brief_panels` (write `set_panel` briefs from the scan), then the name approval. No script is needed for pages drawn by hand.
- D8: `genko studio eval-atari truth.json` measures how many panels are recovered within 5 mm (`{"align", "pages": [{"scan", "panels": [[x, y, w, h], …]}]}`).

Colour, screens, mannequins and other agents (M9):

- Screen outputs: `--format webtoon` (all pages at `--width` px, default 800, stacked and cut into slices of at most `--max-height` px, default 1280: `001.png`, `002.png` …) and `--format sns` (one image per page, long edge `--long-edge` px, default 2048, JPEG; `--spreads` adds one image per spread). Both cut the bleed, skip the dot screen (tones are flat grey) and embed sRGB. `genko export` takes all options; `genko studio export --format webtoon|sns` (human, after preflight; no dpi check) uses the defaults.
- Colour pages (`spec.expression` "color") are never screened, also in print. Their generation requests have `color: true`, the colour style in the prompt draft, and no "colour" in `avoid`.
- Mannequins: `add_mannequin {page, pos (pelvis mm), height_mm?, rot?: [tip, turn, lean], preset?, id?}` and `pose_mannequin {page, id, preset?, joints?, rot?, pos?, height_mm?}`. Joints: neck, head, shoulders (`l_arm`/`r_arm`), elbows, wrists, hips (`l_leg`/`r_leg`), knees, ankles; each `{yaw, pitch}` in radians. Presets: stand, walk, run, sit, point, look_back, arms_up. The figure is eight heads tall and is drawn on name and proof only. A mannequin inside a panel is drawn into that panel's `guides/pose.png` and noted in `notes_for_agent`.
- Other MCP clients (Claude Code, Claude Desktop, the Python SDK): `docs/OTHER_AGENTS.md`, `integrations/claude-code/`, `integrations/generic/mcp_client_example.py`. Genko has no content filter; what is drawn depends on the agent and its image tool.

Drawing (M10, see `docs/TOOLS_PLAN.md`):

- Pen lines are vectors: `add_stroke` keeps the points (with pressure) and every render draws them at its own resolution with the line's width, colour (`rgb`) and `opacity`. Nothing is baked into pixels; a filter (`filter_raster`) turns a layer's lines into pixels first.
- `add_stroke {layer_id}` draws on any pen or paint layer; `erase {page, layer_id, points, width_mm}` cuts pen lines where the eraser passes (vector erase) and clears paint. A `locked` layer refuses both.
- Layers are composited in their order (lines included). `add_layer {kind: pen|paint|folder, name, after, id}`, `set_layer {name, opacity, blend, clip, lock_alpha, locked, visible}`, `reorder_layers`, `delete_layer`. In studio books, drawing on a printed layer still needs the name approval.
- `edit_line {wrap: vertical|horizontal, balloon}`; vertical text without breaks wraps inside its balloon (the box is the balloon's outside) in even columns.
- Panel borders print at their `border_mm` (default 0.8 mm).

Lettering (M11):

- Faces ship with Genko (SIL OFL, `src/genko/fonts/licenses/`): `antique` (the default for dialogue: kana in Zen Old Mincho, kanji in Zen Kaku Gothic New), `gothic`, `mincho`, `maru`, `hand`, `sfx` (Dela Gothic One, the default for sound effects), `sfx_pop`. A font file path works too. Characters a face lacks fall back to a look-alike or the Gothic.
- `edit_line {style}` (and `add_line {style}`): `font`, `size_mm` (null = fit the balloon), `tracking`, `leading` (in em; columns are 0.15 em apart by default), `align` (top / center / bottom; left / center / right for horizontal text), `outline_mm` (white halo), `rgb`, `tcy` (縦中横 for 2-3 digits or letters and !? runs, on by default; full-width digits too), `border_mm`, `fill` (white / none), `group` (balloons with the same group are drawn as one, with one outline). A key set to null goes back to the default.
- Balloon shapes (`balloon`): speech, rounded, box, cloud, thought, shout, flash, whisper, narration, sfx, none. The box is the balloon's outside; the text is centred in the space inside it and shrinks to fit unless it has a size.
- Tails: `tails: [{to: [x, y], via?: [x, y], width_mm?}]` — several per balloon, curved through `via`. `tail` (one straight tail) still works.
- Vertical text: kinsoku at both ends of a column (closing marks hang at the end of the previous column), balanced columns, ruby runs. `reorder_lines {page, order}` sets the reading order.

Panels (M12):

- A panel is a rectangle or a polygon (`poly`, page mm; `rect` is then its box). `split_frame {tilt_mm}` slants a split; `cut_frame {p0, p1, gutter_mm, frame_id?}` cuts a panel along any line (the panel under the line's middle when no frame_id). Split nodes keep their cut (`split`: the line in the node's own 0..1 box and the gutter), so cuts follow their panel when the page is re-laid.
- `move_gutter {frame_id: the split, index?, delta_mm, gutter_mm?}` moves the gutter (and can set its width); the panels on both sides and everything inside them are laid out again. Stacks from `set_layout` move the same way.
- `set_frame {poly}` makes a free-form panel (it keeps its form when the page is re-laid; `poly: null` goes back to the cut), `border_mm` (0: no border), `bleed`.
- Reading order follows the middle of each panel (slanted boxes overlap). Ink, fills and placed art are clipped to the polygon; `set_layer {panel_clip: false}` lets a layer run out of the panels.
- Layout changes (`cut_frame`, `move_gutter`, `set_frame poly`) are refused for agents once the name is approved, like `split_frame`.

Pens, fills and selections (M13):

- `add_stroke {kind}`: `gpen` (G pen), `maru`, `kabura`, `mili` (even width), `pencil`, `fude` (brush), `marker` (see-through), `airbrush`, `fill_pen`, `white` (correction white). Also `width_mm`, `rgb`, `opacity`, `stabilize` (0 = off), `taper`, `pressure_gamma` (<1 soft, >1 hard). The look is drawn at render time, at the output's resolution.
- `fill {page, layer_id, x_mm, y_mm, rgb?, opacity?, gap_mm? (0.3), reference: page|layer}` fills the region under the point, closing line gaps up to `gap_mm`, inside the clicked panel. `fill_area {area}` fills an area. Fills are kept as patches on the layer (300 dpi masks, saved as assets).
- Areas: `{poly: [[x, y], …]}` (mm) or `{mask: {box: [x, y, w, h], png: base64}}`. `transform_area {area, matrix: [a, b, c, d, e, f]}` (x' = a·x + c·y + e, y' = b·x + d·y + f) moves, scales, turns or flips what lies in the area on that layer: lines (most of their points inside), fills and paint pixels. `delete_area {area}`. `paste {items: {strokes, patches}, matrix?}` puts copied items on a layer.
- Line fixes: `set_stroke_width {area | ids, width_mm | scale, kind?, rgb?}`, `reshape_stroke {stroke_id, points}`, `erase {mode: "to_crossing"}` (cut a line only up to where it crosses the others).
- The same rules as drawing apply: printed layers need the name approval (`strict_gates`), locked layers refuse edits.

Rulers and 3D (M14):

- `add_ruler {page, kind, points, …, frame_id?}` puts a ruler on the page: `line` [a, b], `curve` (points of a smooth curve), `parallel {angle}`, `concentric` [centre] `{ratio, angle}`, `radial` [centre] (focus lines), `perspective` (1 to 3 vanishing points), `symmetry` [a, b] `{copies, mirror}`. `edit_ruler {id, active, visible, …}`, `delete_ruler {id?}` (none: all).
- `add_stroke {snap_ruler: true}` (or `ruler_id`) makes the line follow the ruler that suits it (line and curve rulers only take lines that start within 10 mm of them); active symmetry rulers draw the line again. Rulers are guides: never printed.
- `add_mannequin`, `pose_mannequin {drag: {handle, to: [x, y]}}` points a part (chest, head, elbows, hands, fingers, knees, ankles, toes; `pelvis` moves the figure) at a place on the page. `add_prim3d {kind: box, pos, size: [w, h, d], rot: [tip, turn, lean], focal_mm}`, `edit_prim`, `delete_prim`. Figures and boxes show in name and proof renders, never in print. `trace_prims {layer_id, ids?}` draws them as pencil lines on a layer (for the draft).

Tones, effect lines and materials (M15):

- `add_tone {page, area | frame_id | at: {x_mm, y_mm, gap_mm?}, pattern: dot|line|cross|noise|flat, lpi, density (0..1 black), angle, gradient: {shape: linear|radial, angle, start, end}, name, id}` makes a tone layer; with none of area / frame_id / at it covers every panel. `set_tone {id, …}` changes it. On a tone layer `add_stroke` paints more tone, `erase` scrapes it (`soft: true` fades), `fill` / `fill_area` add areas. Tones sit in the layer order (a layer above covers them) and print as pure black-and-white patterns; proofs show their grey.
- `add_effect {page, kind: focus|speed|uni_flash|beta_flash|white, frame_id, params}`: focus `{center, inner: [rx, ry], count, jitter, width_mm}`, speed `{angle, count, length, curve, jitter, width_mm}`, uni_flash `{center, inner, count, length_mm, width_mm}`, beta_flash `{center, inner, spikes, depth}`; `rgb` for white lines. `edit_effect {id, params (merged, null removes), visible}`, `delete_effect`, `effect_to_layer {id, layer_id}` turns it into pen lines (効果線ペン, kind `fx`) and fills to finish by hand.
- Materials: built-in tones, gradients and effects, plus the person's own library in the config dir (pictures, drawn parts, folders). `stamp_material {page, material_id, frame_id | area | at (tones), x_mm, y_mm (effect centre, or where a picture / part goes), layer_id (pictures and parts), width_mm}` copies it into the book.

Pages and the book (M16):

- `add_page {count, after}` inserts pages; `duplicate_page {page, next_to: true}` puts the copy right after; `reorder {order}` moves pages (lines, spreads and onion skins follow).
- `set_nombre {position: bottom_center|bottom_outside|top_outside|side_outside, font, size_mm, start, hidden, hidden_size_mm, show}` sets the book's page numbers (outside = the fore-edge, which changes side each page; the hidden nombre sits in the gutter). `set_nombre {page, numero: false}` hides one page's. They print in exports and show in proofs.
- `add_line {id}` can choose the new line's id.
- `genko.checks.book(episode, project?)` lists what to fix before printing, each with a page and a place: text outside the trim or the basic frame, text too small for its balloon, overlapping balloons, low-resolution pictures, paint layers, art beyond the paper, spreads that do not face, empty pages — plus the studio preflight for studio books.

Books without agents (F2): the name → art → finish order (ink needs `name_ok`) applies only to books with `strict_gates` or a studio; a person's own book can be inked at any time.

Paper (F1):

- A page has its paper (the canvas, `width_mm` × `height_mm`, coordinates from its top left), the finished size centred on it (`trim_w_mm` × `trim_h_mm`, 仕上がり), the bleed around that (`bleed_mm`, 裁ち落とし) and the basic frame (基本枠) inside the trim (`margins_mm`: top, bottom, binding side (のど), fore-edge (小口); binding and fore-edge swap with the page's side). New panels fill the basic frame.
- Presets (`create_project spec_preset`, CLI `genko new --paper`): `commercial-b4` / `b4` (B4 sheet, finished 220×310, bleed 5, frame 180×270), `doujin-b5` / `b5` (finished 182×257, bleed 3, frame 150×220), `doujin-a5` / `a5` (finished 148×210, bleed 3, frame 120×180), `a4-mono` / `a4` (practice: the sheet is the page), `webtoon`. Old books keep their old spec (the sheet less the bleed is the trim).
- `set_page_spec {preset | paper: [w, h], trim: [w, h], bleed_mm, margins: [top, bottom, inner, outer], dpi, move: true}` puts the book on other paper and moves panels, lines, art, rulers, figures and effects onto the new basic frame.
- Bleed panels run out to the bleed; art beyond it is not printed. Spreads join where the finished sizes meet (the gutter). Print exports take `area`: `paper` (with crop marks), `bleed` (the usual for printers) or `trim`; screen exports are cut to the trim.

Image tools (`tools.json` in the config dir, never in a project):

```bash
genko studio tools example                       # an example entry
genko studio tools set openai:gpt-image-1        # or --file spec.json with {label, sizes_px, supports, notes}
genko studio tools list
```

Every agent tool is also on the shell: `genko studio call PROJECT generation_request args.json`.

HTTP: `GET /v1/pages/{n}/frames/{frame_id}.png` renders one panel; `POST /v1/assets?path=PROJECT` (image body) returns `{asset, px}`; `GET /v1/requests/{id}/files/{name}?path=PROJECT` serves a request's guides and references.

## Parity with the app (G6)

Everything a person can do in the app can be done through `apply_ops`; the ops an agent may use are listed in
`AGENT_OPS` (studio/service.py). Only the human steps stay out of reach: approvals and their undoing
(`approve`, `revoke`, `name_ok`, `advance`, `reject_sheet`, `resolve_proposal`), page locks and tickets, and
`set_bible` / `set_script` (they have their own tools).

- Pages: `add_page`, `delete_page`, `duplicate_page`, `reorder`, `set_spread`, `set_page_spec`.
- Layers: `duplicate_layer`, `merge_down` (pen onto pen stays lines; anything else becomes pixels as it showed),
  `delete_layer`, `set_layer {exportable, color, …}`, `set_layer_mask {area | fill | invert | enabled | delete}`,
  `paint_mask {points, width_mm, show}`.
- Drawing: `define_brush {key: my_…, label, base, width_mm, min_pressure, gamma, opacity, stabilize, taper, texture,
  rgb, fixed_width}` (the book keeps the definition), `gradient_fill {area?, from, to, rgb_from, rgb_to, opacity_from,
  opacity_to, shape}`, `filter_raster {kind: levels | curve | hue | blur | sharpen | mosaic, …}`, `flood_fill`,
  `erase_raster` (pictures come in through `import_image`; `put_raster` can read local files and stays out), `transform_area {warp: {perspective: [4 points]} | {mesh: [9 points]}}`.
- Lines: `emphasis_runs` (傍点), `style_runs` (`[[words, {scale, bold, rgb}]]`), `path` (a drawn balloon) and the
  style keys `rotate_deg`, `skew_deg`, `arc`, `bold`, `italic`, `outline_rgb`, `latin`, `emphasis_mark`, `wobble`,
  `double`, `spikes`, `spike_depth`.
- 3D: `add_prim3d {kind: box | cylinder | stairs | floor}`.

### J9 additions

- `add_cover {kind: front | back | jacket, spine_mm, flap_mm}`: covers are pages at the end without nombre; a
  jacket is one sheet [flap][front][spine][back][flap] (right-bound). Exports name them cover_front / cover_back /
  cover_jacket; EPUB puts the front first and the back last.
- `replace_text {find, replace, regex?, case?, pages?, speakers?, must_find?}` over every line.
- `for_pages {pages: [n] | all | body, ops}` runs the ops on each page (checked like any other op).
- `set_assignee {pages, who}` (担当).

### J8 additions

- `add_figure {pos (pelvis), height_mm, body {heads, shoulders, hips, build, legs}, preset, joints {name: {x, y, z}},
  hands {l, r}, rot}`; `pose_figure {id, joints (merged) | set_joints, body, hands, preset, rot, pos, drag {handle, to}}`
  (joints: hip, spine, chest, neck, head, l/r_arm, l/r_elbow, l/r_wrist, l/r_leg, l/r_knee, l/r_ankle; x swings toward
  the viewer, y twists, z turns in the picture; presets stand, walk, run, sit, point, arms_up, think, kneel, peace;
  hands open, relaxed, fist, point, peace, grip).
- `add_head {pos, size_mm, rot}` (the face's centre and eye lines), `add_hand {pos, size_mm, side, pose, rot}`,
  `import_model {obj (OBJ text) | glb (a .glb / .vrm, base64) | gltf (text), size_mm, pos, rot, name}`.
- `set_camera {turn, tip, roll, focal_mm, target | off}` turns every 3D on the page together; `set_light {dir,
  ambient}`.
- `render_prims {layer_id, ids?, lines, surfaces, tone {lpi}, light?, width_mm, kind}`: pen lines with hidden parts
  left out, and the lit surfaces as greys (with `tone`, the layer prints them as dots). `trace_prims` also leaves
  hidden lines out for figures, heads, hands and models.

### J7 additions

- `inspect materials` lists every kind: tone, effect, image, lines (marks 漫符, props 小物, traced backgrounds),
  lettering (描き文字, with its `text`), brush, prim (3D figures, boxes, scenes); each with `folder` and `tags`.
- `stamp_material`: lines on `layer_id` at `x_mm/y_mm`; lettering becomes a line there; a brush is added to the
  book (`define_brush`, key `my_…`, then usable as `add_stroke {kind}`); a prim is a 3D guide at `x_mm/y_mm`.
- Packs (a folder or .zip with pack.json and pictures, or just pictures) are the person's library: imported and
  exported in the app (materials panel → 素材パック).

### J6 additions

- Panels: `set_frame {curves: [mm per edge] | null, bow: {edge, mm}, line: {kind: solid | double | dashed | dotted |
  rough, rgb?, gap_mm?, dash_mm?, wobble_mm?} | null}` (edges run from corner i: top, right, bottom, left for a
  rectangle; + bows out).
- Lettering style keys: `scale_x` (長体 < 1 < 平体), `gradient {rgb_from, rgb_to, angle}`, `fill_png` (letters
  painted with a picture), `warp` (the letters' four corners as shares of their box), `text_path` ([[x, y]…] mm
  from the box's top left), `features` (OpenType forms: jp78, jp90, trad, expt, hwid…; across text), `yakumono`
  (default true: paired punctuation set half wide), and variation selectors in the text (異体字).
- Balloons: `balloon: picture` with `style.picture` (base64 PNG), or `stamp_material {material_id, line_id}` with an
  image material; `spike_jitter`, `bumps` (cloud); tails take `kind: wedge | zigzag | fade | bubbles`.
- Effects: speed lines `path` + `spread_mm` (along a curve); focus lines `inner_path` (any clear shape), `twist`.
- Tones: patterns `check | brick | wave | grid | hatch | star | sand | image` (`scale_mm`, `tile_png`);
  `set_layer {screen: {pattern, lpi, angle, black, white} | null}` (a layer's greys print as a halftone); the
  checks report `tone_moire` for overlapping tones at different angles or line counts.
- Rulers: kinds `parallel_curve`, `multi_curve` (`points2`), `radial_curve` (`center`); any ruler may have
  `layer_id` (only for that layer); perspective `lock_horizon`, `horizon_y`, `fixed`; `ruler_to_layer {id,
  layer_id, width_mm}` draws a ruler's own line (定規ペン).
- Materials: kind `lettering` (描き文字: text, balloon, style, size); `stamp_material` puts it as a line.

### J5 additions

- Layer kinds in `add_layer`: `fill {rgb}` (ベタ塗り), `gradient {gradient: {from, to, rgb_from, rgb_to, opacity_from,
  opacity_to, shape}}` and `adjust {adjust: {kind: levels | curve | hue | invert | posterize | threshold |
  gradient_map | bitonal, …}}` (a correction layer: it changes what is under it at render time and can be changed
  again with `set_layer {fill | adjust}`; its opacity, mask and clip apply).
- `set_layer {effect: {border: {width_mm, rgb}, water_edge: {width_mm, strength}} | null, color_prints}` and the blend
  modes `darken`, `lighten`, `color_burn`, `color_dodge`, `linear_burn`, `soft_light`, `hard_light`, `difference`,
  `exclusion`, `subtract`, `divide`, `hue`, `saturation`, `color`, `luminosity` (besides normal, multiply, screen,
  add, overlay).
- Several layers: `merge_layers {ids}` (into the lowest, as they showed), `merge_visible {copy?}` (copy: a new
  layer on top), `group_layers {ids, name?}`, `move_layers {ids, parent?, after?}`, `set_layers {ids | all, …}`,
  `convert_layer {id, to: paint | pen}` (pen: the pixels traced into lines).
- `set_paper {page?, rgb | null}` (用紙色), `liquify {layer_id, points, width_mm, strength, mode: push | pinch |
  bloat | twirl_cw | twirl_ccw}` (pixels and pen lines), `transform_area {interp: nearest | bilinear | bicubic}`.
- `filter_raster {kind}` also takes `motion_blur`, `radial_blur`, `zoom_blur`, `noise`, `wave`, `twirl`, `lineart`,
  `invert`, `posterize`, `threshold`, `gradient_map`.

### J4 additions

- `vector_edit {page, layer_id, action: move_point | add_point | delete_point | connect | cut | recolor | delete,
  stroke_id?, ids?, index?, to?, at?, rgb?}`.
- `fill_gaps {page, layer_id, max_mm?, rgb?, area?}` (塗り残し).
- Colours are plain `rgb` in every op; the colour sets, history and the main / sub / transparent colours are
  the app's (drawing with the transparent colour is an `erase`).

### J3 additions

- Brushes: new built-ins (calligraphy, water, spray, stipple, dotline, dashline, lace, grass, leaves, hearts,
  stars); `define_brush` also takes `tip` (round | flat | image with `tip_png`), `tip_angle`, `tip_ratio`,
  `tip_follow`, `pattern`, `spacing`, `scatter`, `stamp_size`, `size_jitter`, `turn_jitter`, `count`, `speed`,
  `post_smooth`, `aa`. `add_stroke {post_smooth}` overrides the brush's.
- `smudge {page, layer_id, points, width_mm, strength, mode: blur | smudge | blend}` on paint layers.
- `erase {mode: cut | to_crossing | whole}`.
- `genko/abr.py` reads Photoshop .abr sampled tips (people import them in the app).

### J2 additions

- `add_shape {shape: line | polyline | curve | rect | ellipse | polygon, points | box, sides?, radius_mm?, closed?,
  line?, fill?, rgb?, fill_rgb?, width_mm?, kind?}`.
- Areas everywhere (`genko/selops.py`): besides `poly` and `mask`, `rect`, `ellipse`, `layer`, `color {x_mm, y_mm,
  tolerance?, contiguous?}`, `all`, `saved` (kept with `store_area {name, area}` / `forget_area`), and
  `union` / `intersect` / `subtract` of areas, with `invert`, `grow_mm` (negative shrinks) and `feather_mm`.
  `apply_ops` turns them into a mask before the op runs.
- Guide lines: `add_ruler {kind: guide, axis: h | v, at}`.

### H5 additions

- `inspect` targets: `materials` (id, name, kind, folder — what `stamp_material` takes), `fonts` (the bundled
  `style.font` keys and the computer's fonts by path), `brushes` (built-in `add_stroke` kinds, the book's own,
  the person's library).
- `snapshot` layers carry `title`, `kind`, `opacity`, `blend`, `clip`, `locked`, `mask`, `color`, `parent_id`,
  `reference`, `panel_clip`; lines carry `w_mm`, `h_mm`, `balloon`, `wrap`, `style`; pages list their 3D prims.
- `render {layer_id}` shows one layer alone on white; `mode: print` is the print look.
- New in H3/H4: balloon `electric`, `style.weight: normal | bold | heavy` (also in `style_runs`), `add_scene
  {kind: room | classroom | corridor | street, pos?, size?, rot?, focal_mm?}`, `set_layer {reference}` and
  `fill {reference: "reference"}`.

Tools: `check` (the same pre-press check as the app; `preflight` also returns it under `checks`), `undo` (the
agent's own latest saved change only), `export {format: pdf | tiff | png | psd | pack | epub | strip | webtoon | sns,
pages?, dpi?, area?, width?, max_height?, long_edge?, jpeg?, spreads?}` → files under `<project>/exports/`. The
official export, recorded as the export approval, stays with people.

