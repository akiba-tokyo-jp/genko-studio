# Genko Studio

Manga manuscript OS for **Windows and Linux**. For **humans and generative AI**.

Not a Clip Studio clone. Pages, the name gate, frames, story text, export that never ships draft.

Two faces, one `.genko` file:

- Human: desktop GUI
- AI / automation: headless JSON CLI and HTTP (no Qt)

## Human

```bash
uv sync --extra app --extra dev
uv run python -m genko app ./manga/demo.genko   # review, approve, edit (without a path: recent projects)
```

## Generative AI (headless)

```bash
uv run python -m genko new ./demo.genko --title demo --pages 8
uv run python -m genko inspect ./demo.genko
uv run python -m genko apply ./demo.genko ops.json
uv run python -m genko render ./demo.genko --page 1 --mode name --out p1.png
uv run python -m genko export ./demo.genko ./out --format tiff --json
uv run python -m genko serve --port 8765
```

See `docs/AGENT.md`.

### Agents over MCP (Hermes Agent など)

```bash
uv sync --extra mcp
uv run python -m genko mcp --root ./manga --agent ai:hermes   # stdio MCP server
uv run python -m genko studio review ./manga/demo.genko --out review.html
uv run python -m genko studio approve ./manga/demo.genko name --pages 1-4 --as human:you
uv run python -m genko studio export ./manga/demo.genko --format pdf --out ./out --as human:you
```

The agent writes the bible, script and name plans; Genko checks, lays out and letters them. For the art, Genko writes generation requests (sizes, prompt drafts, guides, references), the agent generates with its own image tool, and Genko imports, places, finishes and checks the result. People approve the sheets, names, art and the export. See `docs/AGENT.md` and `integrations/hermes/`. Trying it with a real agent and measuring the run: `docs/STUDIO_EVAL.md`.

```bash
uv run pytest
```

Python 3.11+.

## Layout

- `src/genko/ops.py` — single command bus
- `src/genko/render.py` — name/proof/print composite
- `src/genko/export.py` — PNG / TIFF / PDF / strip / EPUB
- `src/genko/psd.py` — minimal PSD
- `src/genko/headless.py` — JSON snapshot
- `src/genko/server.py` — HTTP API + OpenAPI
- `src/genko/app/` — PySide6 GUI
- `src/genko/studio/` — agent tools: schemas, lint, layout DSL, lettering, worklist, studio ops (panel briefs, candidates, adoption, approvals)
- `src/genko/placement.py` — where a placed image lands in its panel and what clips it
- `src/genko/guide.py` — panel guides for image tools (composition, pose, keepout, mask, compare)
- `src/genko/studio/genreq.py`, `importer.py`, `preflight.py`, `finish.py` — generation requests, image import, export checks and proofs, finishing
- `src/genko/studio/evaluate.py`, `toollog.py` — run statistics, the approval audit, the blind character test
- `src/genko/mcp/` — MCP server (`genko mcp`)
- `docs/AGENT.md` — how an agent should drive Genko
- `docs/DESIGN.md` — architecture
- `docs/AI_PIPELINE.md` — agent-driven manga production design (external agents such as Hermes Agent operate Genko over MCP)
- `docs/ops.schema.json` — ops catalog

## License

MIT
