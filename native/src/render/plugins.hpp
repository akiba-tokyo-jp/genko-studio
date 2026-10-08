#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

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
//     variables: PATH, the home folder, the language, the temporary folder), and runs in a folder of its own. Asking a
//     plugin is bounded in time and in how much it may write back.
// Python is needed only for this: Genko and its own filters never need it.

namespace genko::render::plugins {

inline constexpr std::string_view kPrefix = "plugin:";
inline constexpr std::chrono::seconds kDescribeTime{10};
inline constexpr std::chrono::seconds kRunTime{120};

// <config>/plugins
std::filesystem::path folder();

// The plugin files there (*.py, not starting with "_", sorted), as the list shows them before anything runs.
struct Listed {
    std::string key;                   // the file name without .py
    std::optional<core::Json> manifest;  // <key>.json beside it when it is a JSON object
    std::string sha256;                // of the file now
    bool chosen = false;               // chosen to run, as it is now
};
std::vector<Listed> listed();

// The person's settings (<config>/plugin_settings.json): plugins on at all (off at first), the Python that runs them
// (empty: python3 or python on PATH), the plugins chosen (key → the file's sha256 when chosen).
struct Settings {
    bool enabled = false;
    std::string python;
    core::Json chosen = core::Json::object();
};
Settings settings();
void save_settings(const Settings& settings);
// Choose a plugin to run (as its file is now), or take the choice back.
void choose(const std::string& key, bool on);
// Plugins are on and this one is chosen, as its file is now.
bool allowed(const std::string& key);

// The Python the runner is started with (none when there is none).
std::optional<std::filesystem::path> python();

// A chosen plugin's own name and settings (NAME, PARAMS), read through the runner. Throws PyValueError with Python's
// words ("no plugin x", "plugin x cannot be loaded (…)", "plugin x has no run(image)") and the runner's ("plugin x
// failed (…)"); core::Error("plugin_not_allowed") when it is not chosen; core::Error("plugin_runner") when there is no
// Python or no Pillow in it.
struct Described {
    std::string key;
    std::string name;
    core::Json params = core::Json::object();
};
Described describe(const std::string& key);
// The chosen plugins that can be described (plugins.available for the filter list; the others left out).
std::vector<Described> available();
// plugins.fields: a plugin's settings as the filter dialog asks them: (key, label, min, max, default).
std::vector<std::tuple<std::string, std::string, double, double, double>> fields(std::string_view kind);

// plugins.run: the picture (RGBA, the same size back; its transparency kept when the plugin returns RGB) through the
// plugin. Its errors as describe's, and "plugin x failed (…)" / "plugin x did not return a picture".
Image run(std::string_view kind, const Image& image, const core::Json& params = core::Json::object());

}  // namespace genko::render::plugins
