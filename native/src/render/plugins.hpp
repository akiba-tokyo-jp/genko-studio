#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"
#include "render/plugin_bands.hpp"

// Filter plugins (プラグイン, Python's genko/plugins.py, with docs/cpp-migration/SPEC.md COMP-04): a Python file a person
// puts in <config>/plugins/ (NAME, PARAMS, run(image, **params) on a PIL RGBA picture) becomes a filter for paint layers
// ("plugin:<file name>"). The contract of the Python files is kept; what is different:
//   - A plugin never runs inside Genko. It runs in its own process, Genko's runner (a small Python script) started with
//     the Python on this computer, over a pipe: the layer's pixels and the settings in, the picture out. That process
//     keeps a crash away from the book; it is NOT a sandbox: a plugin can do whatever its code does.
//   - Plugins are off until the person turns them on, and only the plugins they choose run, each as the file it was when
//     they chose it (a changed file is chosen again). Listing them reads only file names and a manifest beside each
//     (<name>.json: {"name", "params"}), never the code; a plugin's own NAME and PARAMS are read through the runner, once
//     chosen.
//   - The runner gets the pixels and the settings only: not the book, not the environment's secrets (a short list of
//     variables: PATH, the home and application data folders, the language, the temporary folder), and runs in a
//     folder of its own, with a copy of the plugin's code as it was chosen (a file changed since is refused). Asking a
//     plugin is bounded in time and in how much it may write back.
// Python is needed only for this: Genko and its own filters never need it.
//
// Native plugins (this build only, COMP-04's native extension, docs/PLUGIN_NATIVE.md): a folder <config>/plugins/<key>/
// (not a link) holding a plugin.json (protocol 1: {"protocol": 1, "name", "executable", "args"?, "params"?}) and the
// program it names, inside that folder (not a link, readable). It is chosen as plugin.json and the program are then
// (either changed: chosen again); its name and settings are plugin.json's, so nothing runs to list or describe it.
// To run, plugin.json and the program are read once more, checked against the choice, and the very bytes checked are
// copied to a private folder under the settings folder (<config>/plugin-runs/run-…, removed afterwards; not the
// system's temporary folder, which may forbid programs): that copy is started directly (no shell), with plugin.json's
// args, in that folder, with the same short list of variables and GENKO_PLUGIN_PROTOCOL=1 and GENKO_PLUGIN_HOME (the
// plugin's own folder). It reads the request (one JSON line {"protocol": 1, "width", "height", "params"}, then the
// RGBA pixels) and writes the reply (one JSON line {"ok": true, "width", "height"}, then the RGBA pixels; or {"ok":
// false, "error"}), as the runner does. The same limits in time and in what it may write back. A plugin's process (the
// runner's too) starts a process group of its own on POSIX, stopped whole when it is stopped (Windows: the process).
//
// PC5 J-f (plugins:WASM-ABI-v1, docs/PLUGIN_WASM.md): WebAssembly plugins, a third kind — <config>/plugins/<key>.wasm
// (and the *.wasm of the additional plugins folder, when it is used; not recursive): a module of PhotoCraft's plug-in ABI
// v1, run by genko-plugin-host (native/wasm-host: PhotoCraft's wasmi sandbox — no imports, an instruction, time and
// memory budget, a fresh instance for each band —, a process of its own beside genko), over protocol 2: the picture in
// bands of f32 samples in its own colour model (render/plugin_bands). Off until chosen, as the others: chosen as the
// module's bytes are (sha256; changed: chosen again); listing reads the file's name and size only (nothing runs before
// it is chosen); the bytes checked are the bytes the host gets. Native plugins of protocol 2 (plugin.json "protocol":
// 2, docs/PLUGIN_NATIVE.md) take the same bands. Both work on every kind of paint layer — 8 bits, precise colour, a
// page's inks — and on an alpha channel or a Quick Mask (one grey channel); Python's plugins and native ones of
// protocol 1 stay 8-bit RGBA as before.

namespace genko::render::plugins {

inline constexpr std::string_view kPrefix = "plugin:";
inline constexpr std::chrono::seconds kDescribeTime{10};
inline constexpr std::chrono::seconds kRunTime{120};
inline constexpr std::size_t kRunPixels = 8'000'000;  // (kRunTime for each this many pixels or part: a larger picture is given longer)

// <config>/plugins
std::filesystem::path folder();

// The plugins there (Python files *.py and native plugins' folders, not starting with "_" or ".", sorted by key), as the
// list shows them before anything runs.
struct Listed {
    std::string key;                   // the file name without .py, or the native plugin's folder name
    std::optional<core::Json> manifest;  // <key>.json beside a Python file when it is a JSON object; a native plugin's plugin.json
    std::string sha256;                // of the file now (a native plugin: of its plugin.json and its program)
    bool chosen = false;               // chosen to run, as it is now
    bool native = false;               // a native plugin (a program), not a Python file
    std::optional<std::string> problem;  // why a native plugin cannot be chosen (its plugin.json or its program)
    // (PC5 J-f) a WebAssembly module (<key>.wasm): its file, its size; its manifest only once described (chosen)
    bool wasm = false;
    std::string path;          // where the file is (a .wasm: the plugins' folder or the additional one)
    std::uintmax_t bytes = 0;  // its size (a .wasm)
    int protocol = 0;          // a native plugin's plugin.json protocol (1, 2), when it can be used
};
std::vector<Listed> listed();
// A plugin of this key is installed: <key>.py or <key>/plugin.json in the folder (nothing read or run).
bool installed(const std::string& key);

// The person's settings (<config>/plugin_settings.json): plugins on at all (off at first), the Python that runs them
// (empty: python3 or python on PATH), the plugins chosen (key → the file's sha256 when chosen).
struct Settings {
    bool enabled = false;
    std::string python;
    core::Json chosen = core::Json::object();
    // (PC5 J-f, edit.preferences.plugIns: Photoshop's 追加のプラグインフォルダー — its *.wasm listed after the plugins
    // folder's, while it is used)
    bool use_extra_folder = false;
    std::string extra_folder;
};
Settings settings();
void save_settings(const Settings& settings);
// Choose a plugin to run (as its file is now), or take the choice back. `listed_sha256`: the file as the person was shown
// it; core::Error("plugin_changed") when it has changed since (nothing is chosen then); core::Error("plugin_manifest")
// for a native plugin whose plugin.json or program cannot be used ("the plugin.json of plugin x cannot be used (…)").
void choose(const std::string& key, bool on, const std::optional<std::string>& listed_sha256 = std::nullopt);
// Plugins are on and this one is chosen, as its file is now.
bool allowed(const std::string& key);

// The Python the runner is started with (none when there is none).
std::optional<std::filesystem::path> python();

// A chosen plugin's own name and settings (NAME, PARAMS), read through the runner (once for each file as chosen and each
// Python; its failures too, until the settings are saved again); a native plugin's from its plugin.json. Throws PyValueError with Python's words ("no plugin
// x", "plugin x cannot be loaded (…)", "plugin x has no run(image)"); core::Error("plugin_not_allowed") when it is not
// chosen (or its file changed since); core::Error("plugin_runner") for the runner's own trouble: no Python, no Pillow in
// it, not started, stopped before it answered, too slow, too much written back.
struct Described {
    std::string key;
    std::string name;
    core::Json params = core::Json::object();
    // (PC5 J-f) "python", "native" or "wasm"; a WebAssembly module's manifest as it says it (id, name, version, author,
    // description, params in its own form, overlap, area)
    std::string kind = "python";
    std::optional<core::Json> manifest;
};
Described describe(const std::string& key);
// The chosen plugins that can be described (plugins.available for the filter list; the others left out).
std::vector<Described> available();
// plugins.fields: a plugin's settings as the filter dialog asks them: (key, label, min, max, default).
std::vector<std::tuple<std::string, std::string, double, double, double>> fields(std::string_view kind);

// plugins.run: the picture (RGBA, the same size back; its transparency kept when the plugin returns RGB) through the
// plugin. Its errors as describe's, and "plugin x failed (…)" / "plugin x did not return a picture" (a native plugin's
// refusal, or a picture of another size); a native plugin's program that cannot be started: core::Error("plugin_runner",
// "the program of plugin x could not be started (…)"). The same request
// asked again while it runs waits for that run; the last few pictures are kept (at most 256 MB). `stop`: no longer
// wanted — the runner is stopped and render::Cancelled thrown.
Image run(std::string_view kind, const Image& image, const core::Json& params = core::Json::object(), const std::stop_token& stop = {});
// The pictures kept forgotten (tests).
void forget_made();

// --- PC5 J-f: WebAssembly modules, protocol 2, installing ------------------------------------------------------------

inline constexpr std::uintmax_t kMostModuleBytes = std::uintmax_t{32} << 20;  // (PhotoCraft's Limits::max_module_bytes)

// The plugin takes the picture in bands of f32 samples in its own colour model (a WebAssembly module, a native plugin
// of protocol 2), so it works on precise and ink layers and channels too; else it takes 8-bit RGBA only.
bool works_in_bands(const std::string& key);
// The program that runs WebAssembly modules: GENKO_PLUGIN_HOST, else genko-plugin-host beside this program (none: the
// WebAssembly plugins cannot run, said as the runner's trouble).
std::optional<std::filesystem::path> host_program();

// plugins.run on samples (render/plugin_bands): a chosen plugin that works in bands over `picture`, in place;
// `coverage` the selection (one value a pixel). Its errors as run's; a plugin that takes 8-bit RGBA only:
// core::Error("plugin_8bit").
void run_samples(std::string_view kind, plugin_bands::Samples& picture, const std::vector<float>* coverage, const core::Json& params,
                 const std::stop_token& stop = {});
// The 8-bit way of a plugin that works in bands, inside a coverage ("L", the picture's size; none: all of it): the
// picture as RGB with its alpha (render/plugin_bands, 8 bits). run() is this without a coverage for such a plugin.
Image run_8bit(std::string_view kind, const Image& image, const Image* coverage, const core::Json& params, const std::stop_token& stop = {});

// The WebAssembly host's limits lowered for tests (the keys of PhotoCraft's Limits: fuel_base, wall_time_ms, …; null:
// as they are). Never raised.
void set_wasm_limits_for_testing(const core::Json& limits);

// plugin.install: a file put into the plugins' folder (a .wasm module: its size and its header looked at, nothing run;
// a .py file as it is), replacing one of the same name; it is not chosen to run. Its key. core::Error("plugin_install",
// …) for what cannot be installed (not a file, another kind, too large, not a module, a name a plugin of another kind
// has, a name not usable).
std::string install(const std::filesystem::path& file);
// plugin.remove: the plugin's file taken out of the plugins' folder (a .wasm or .py), and its choice. core::Error(
// "plugin_install", …) for a native plugin's folder or a module of the additional folder (removed there by hand) and
// PyValueError("no plugin x") for none.
void remove(const std::string& key);
// plugin.reload: everything remembered of the plugins forgotten (their descriptions, digests, pictures made) and the
// list read again; each chosen WebAssembly module described again, a broken one with its reason (`problem`).
std::vector<Listed> reload();
// plugin.list (read only — `genko plugins list`, MCP's inspect plugins "installed"): a plugin as listed, as JSON —
// {"key", "kind": "python" | "native" | "wasm", "filter": "plugin:<key>", "chosen", "sha256", "problem"?, and what is
// known without running it: a native plugin's "name", "params", "protocol"; a module's "size", "path" and, once described,
// its manifest's "id", "name", "version", "author", "description", "params", "overlap", "area"}.
core::Json listed_json(const Listed& item);

}  // namespace genko::render::plugins
