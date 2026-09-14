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
uv run python -m genko new ./demo.genko --title demo --pages 8 --json
uv run python -m genko inspect ./demo.genko
uv run python -m genko apply ./demo.genko ops.json
uv run python -m genko export ./demo.genko ./out --json
uv run python -m genko serve --port 8765
```

See `docs/AGENT.md`.

```bash
uv run pytest
```

Python 3.11+.

## Layout

- `src/genko/headless.py` — JSON ops + compact snapshot
- `src/genko/server.py` — HTTP API
- `src/genko/app/` — PySide6 GUI
- `docs/AGENT.md` — how an agent should drive Genko
- `docs/DESIGN.md` — architecture

## License

MIT
