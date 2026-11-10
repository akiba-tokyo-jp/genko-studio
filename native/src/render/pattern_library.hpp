#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/preset_store.hpp"

// PC4 E3 (SPEC.md COLOR-09「パターン」「プリセット」, design 2.4 and 3.4): the machine's library of patterns — the settings
// folder's patterns.json (render/preset_store: named groups of named items) and each pattern's picture beside it in
// patterns/<its sha256><.png | .colorrgba | .gkir>. An item is {"name", "asset", "mode", "depth", "width", "height", "dpi"}
// (a book's pattern's keys but its id). Its built-in groups — 幾何学 and テクスチャ — hold PhotoCraft v0.2.0's procedural
// patterns (crates/engine/src/pattern_cmds.rs builtin(), MIT: native/third_party/photocraft/NOTICE.md) with Japanese
// names, made by the program (their pictures are never files). The ops never read the library: the window and the CLI
// put a pattern into a book (add_pattern) from it. Which pattern is chosen (Photoshop's 今のパターン) is the window's
// state, not the library's.

namespace genko::render::pattern_library {

// The library's kind for render/preset_store.
const preset_store::Kind& kind();
// The pattern an item names (its id empty), checked: core::Error("value").
core::Pattern pattern_of(const core::Json& item);
// The item of a pattern (its name `name`).
core::Json item_of(const core::Pattern& pattern, const std::string& name);
// The picture of a library pattern: a built-in's made, another's read from `config_dir`/patterns (null when the file is
// not there or not that picture).
core::Bytes picture(const std::filesystem::path& config_dir, const core::Pattern& pattern);
// The built-in patterns (their names, pictures made by the program; 300 dpi).
struct Builtin {
    core::Pattern pattern;
    core::Bytes bytes;
    std::string group;
};
const std::vector<Builtin>& builtins();

// The library as it is read, and written (its pictures first, each once in patterns/; then the groups).
preset_store::Library load(const std::filesystem::path& config_dir);
void save(const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& groups);
// A pattern added to the groups (its picture kept in `config_dir`/patterns): {"name", "group"} (its name made unique).
core::Json add(const std::filesystem::path& config_dir, std::vector<preset_store::Group>& groups, core::Pattern pattern,
               const core::Bytes& bytes, std::optional<std::string_view> group);
// Pictures in patterns/ no item names any more, taken away (after a delete).
void tidy(const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& groups);
// {"groups": [{"name", "patterns": [{"name", "mode", "depth", "width", "height", "dpi"}]}]}.
core::Json list(const std::vector<preset_store::Group>& groups);

// A library pattern as the op that puts it into a book takes it: {"op": "add_pattern", "name", "png" | "gkcr" | "gkir",
// "mode", "depth", "dpi"} (its picture base64).
core::Json add_op(const core::Pattern& pattern, const core::Bytes& bytes);

}  // namespace genko::render::pattern_library
