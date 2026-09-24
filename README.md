# Genko Studio

Manga manuscript OS for **Windows and Linux**. For **humans and generative AI**.

Not a Clip Studio clone. Pages, the name gate, frames, story text, export that never ships draft.

Two faces, one `.genko` file:

- Human: desktop GUI
- AI / automation: headless JSON CLI and HTTP (no Qt)

## Human

```bash
uv sync --extra app --extra dev
uv run python -m genko app
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
- `docs/AGENT.md` — how an agent should drive Genko
- `docs/DESIGN.md` — architecture
- `docs/AI_PIPELINE.md` — agent-driven manga production design (external agents such as Hermes Agent operate Genko over MCP)
- `docs/ops.schema.json` — ops catalog

## License

MIT
