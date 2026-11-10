# Genko Studio

Manga manuscript OS for **Windows and Linux**. For **humans and generative AI**.

Not a Clip Studio clone. Pages, the name gate, frames, story text, export that never ships draft.

Genko Studio was migrated from its earlier Python version.

Two faces, one `.genko` file:

- Human: desktop GUI
- AI / automation: headless JSON CLI, HTTP and MCP

## Install

Ubuntu 24.04 LTS (x86_64):

```bash
tar xf genko-studio-<version>-linux-x86_64.tar.gz
cd genko-studio-<version>-linux-x86_64
./install.sh            # into ~/.local/opt/genko-studio-<version>, no root needed
genko-studio            # or the "Genko Studio" menu entry
```

Windows 10 / 11 (x64): unzip and run `bin\genko.exe`. Acceptance on real Windows machines is still to be done before distribution.

Requirements, settings, uninstalling and signatures: `native/package/README-ja.md`.

## Human

```bash
genko app ./manga/demo.genko      # without a path: recent projects
```

## Generative AI (headless)

```bash
genko new ./demo.genko
genko inspect ./demo.genko
genko apply ./demo.genko ops.json
genko render ./demo.genko --page 1 --out p1.png
genko export ./demo.genko ./out --format tiff
genko serve --port 8765
```

Export formats: png, tiff, pdf, strip, psd, psb, epub, pack, webtoon, sns, cmyk, layers, kindle, timelapse, animation, pcraft.

### Agents over MCP

```bash
genko mcp --root ./manga --agent ai:hermes   # stdio MCP server
genko studio review ./manga/demo.genko --out review.html
genko studio approve ./manga/demo.genko name --pages 1-4
```

The agent writes the bible, script and name plans; Genko checks, lays out and letters them. People approve the sheets, names, art and the export. See `integrations/hermes/`.

### Plugins

WebAssembly filter plugins (ABI v1): `genko plugins install FILE`, then choose them in the app. See `docs/PLUGIN_WASM.md`.

## Build from source

C++20, Qt 6.11, CMake 3.24+, Rust (cargo) for the plugin host, LibRaw 0.22 (`tools/ci/build_libraw.sh <prefix>`).

```bash
cd native
GENKO_LIBRAW_ROOT=<prefix> cmake --preset linux-release -DCMAKE_PREFIX_PATH=<Qt>/gcc_64
cmake --build ../build/linux-release
ctest --test-dir ../build/linux-release
(cd ../build/linux-release && cpack -B ../package)   # genko-studio-<version>-linux-x86_64.tar.gz
```

Presets: `linux-release`, `linux-debug`, `linux-asan`, `linux-tsan`, `windows-release`, `windows-debug`.

Some tests read the files written with Pillow and psd-tools, or drive `genko mcp` through the MCP SDK's client: point `GENKO_PYREF` at a Python with `pillow==12.3.0` and `psd-tools==1.10.9`, and `GENKO_MCP_PYTHON` at one made from `tools/migration/mcp_reference_requirements.txt` (the full `ctest` run expects both).

## Layout

- `native/src/core` — model, command bus, features
- `native/src/storage` — `.genko` v4 format, journal, recovery
- `native/src/render` — compositing, ops, filters, PSD / .pcraft
- `native/src/formats` — import and export
- `native/src/service` — batches, actions, automation
- `native/src/studio` — the AI production pipeline (bible, script, name, approvals)
- `native/src/api` — CLI, HTTP, MCP
- `native/src/app` — Qt GUI
- `native/wasm-host` — WebAssembly plugin host
- `native/tests` — unit, contract, GUI, performance tests
- `docs/cpp-migration` — specification (`SPEC.md`), architecture, acceptance, hand-off
