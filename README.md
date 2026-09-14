# Genko Studio

Manga manuscript OS for **Windows and Linux**.

Not a Clip Studio clone. It is the factory: pages, the name gate, frames, story text, and export that never ships draft layers.

## Run

```bash
uv sync --extra app --extra dev
uv run python -m genko app
```

CLI without a GUI:

```bash
uv run python -m genko new ./demo.genko --title demo --pages 8
uv run python -m genko export ./demo.genko ./out
```

```bash
uv run pytest
```

Requires Python 3.11+.

## Layout

- `src/genko/models.py` — episode / page / frame / story
- `src/genko/pipeline.py` — name-OK gate
- `src/genko/export.py` — PNG sequence
- `src/genko/io.py` — `.genko` folder
- `src/genko/app/` — PySide6 desktop
- `docs/DESIGN.md` — architecture

## License

MIT
