#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// PC4 E-0 (SPEC.md COLOR-09「プリセット」, design 2.4 and the coordinator's decision: the presets are the machine's library,
// the book keeps a copy of what it uses): the libraries of presets kept in the settings folder — gradients (E4),
// patterns (E3), styles (E6), tool presets (E2), each its own JSON file of named groups of named items (PhotoCraft
// v0.2.0's engine/src/presets/mod.rs: Group, unique_name, edit_groups, MIT: native/third_party/photocraft/NOTICE.md; the
// shape presets' way, render/shape_presets, which stays as it is). The ops never read these (PC1 9 章): the window and
// the CLI take an item out and give its settings to the op. Each kind (its file, its built-in groups, how its items are
// checked) is its group's (E2–E6); this is what they share:
// - the file: {"version": 1, "groups": [{"name", "items": [{"name", …}]}]}, written whole and atomically (16 MB at most);
// - a file that is there but cannot be read (not JSON, another layout or version, an item its kind refuses, too large)
//   is never written over: the built-in groups are shown, and saving is refused (the person's file kept as it is);
// - the names: an item's 1..200 characters, unique over every group (a name taken gets " 2", " 3"…: upstream's
//   unique_name, as agents name presets by their names); a group's 1..200 characters, unique.

namespace genko::render::preset_store {

inline constexpr std::size_t kFileMaxBytes = 16u << 20;
inline constexpr int kVersion = 1;

struct Group {
    std::string name;
    std::vector<core::Json> items;  // (objects, each with its "name")
};

// A kind of preset.
struct Kind {
    std::string file;           // in the settings folder ("gradients.json")
    std::string default_group;  // the group add() puts an item in when it names none
    std::function<std::vector<Group>()> builtin;  // its built-in groups (none: no groups)
    // An item's own settings checked (its "name" is checked here): core::Error("value") says what is wrong.
    std::function<void(const core::Json& item)> check;
    std::size_t max_items = 4096;
    std::size_t max_groups = 256;
};

// The library of a kind as it is read.
struct Library {
    std::vector<Group> groups;
    bool from_file = false;  // (read from the settings folder; else the built-in groups)
    std::string problem;     // (the file is there but cannot be read: why — and it is never written over)
};

// The kind's library in `config_dir` (its file), else its built-in groups (no file, or one that cannot be read:
// `problem` says why).
Library load(const std::filesystem::path& config_dir, const Kind& kind);
// The groups written to `config_dir`'s file (atomically; the folder made). core::Error("io") when the file there cannot
// be read now (the person's file is not written over), core::Error("value") for groups that cannot be (a name, an
// item its kind refuses, too many).
void save(const std::filesystem::path& config_dir, const Kind& kind, const std::vector<Group>& groups);

// {"groups": [{"name", "items": [names]}]}.
core::Json list(const std::vector<Group>& groups);
// The item named (in `group` when given; the first of the name otherwise), or none.
const core::Json* find(const std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group = std::nullopt);
// `base`, or "base 2", "base 3"… — a name no item has.
std::string unique_name(const std::vector<Group>& groups, std::string_view base);
// The item (its "name", or `fallback_name`, made unique) kept in `group` (made when it is not there; none: the kind's
// default group) → {"name", "group"}. core::Error("value") for an item the kind refuses or one too many.
core::Json add(std::vector<Group>& groups, const Kind& kind, core::Json item, std::optional<std::string_view> group,
               std::string_view fallback_name);
// {"action": rename | delete | move | new_group | rename_group | delete_group, "preset": a name or names, "group"?,
// "name"?, "to"?, "index"?} → what changed (rename: {"name"}; delete: {"deleted"}; move: {"moved", "group"}; new_group,
// rename_group: {"group"}; delete_group: {"deleted": its items}). core::Error("value") for what cannot be done (a name
// taken, none named, a group that is not there), the groups then as they were (no part of the edit done).
core::Json edit(std::vector<Group>& groups, const Kind& kind, const core::Json& params);
// The built-in groups (`append`: after the person's, a group's name taken getting " 2"…, an item's name taken getting
// one too).
std::vector<Group> reset(const std::vector<Group>& groups, const Kind& kind, bool append);

}  // namespace genko::render::preset_store
