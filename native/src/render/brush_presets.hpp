#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/abr_map.hpp"

// PC4 E2 (brush.presets.list・.save・.delete・.importAbr, brush.defineFromSelection; SPEC.md COLOR-09「ブラシのプリセット」,
// design section E 2.4 and 3.5): the brushes of this machine as Photoshop's ブラシ panel holds them — named groups of
// brushes. The person's are in the settings folder's brushes.json (render/brushes.hpp: each brush under its key, its
// "group" in its entry, the groups' order in "groups", a dab brush's pictures in brush_tips/), read again and written
// whole on every change (another window's or `genko brushes`' changes are never written over). The built-in groups are
// the program's: Genko (the brushes every book knows, core/brushes.hpp) and PhotoCraft v0.2.0's 36 dab brushes (crates/
// paint/src/presets.rs, MIT: native/third_party/photocraft/NOTICE.md; their own designs, the tips made by the program)
// in 一般, ドライメディア, ウェットメディア and 特殊効果 with Japanese names. A built-in dab brush is drawn by a key of its own
// (my_genko_…): the window gives it to a book with define_brush, as one's own (the book keeps a copy of what it uses).
// The ops never read the library (PC1 9 章).

namespace genko::render::brush_presets {

inline constexpr std::string_view kOwnGroup = "自分のブラシ";
inline constexpr std::string_view kGenkoGroup = "Genko";
inline constexpr std::string_view kBuiltinPrefix = "my_genko_";
inline constexpr std::size_t kMaxOwn = 10000;  // (the person's brushes)

struct Preset {
    std::string key;    // (gpen…: Genko's; my_genko_…: a built-in dab brush; my_…: the person's)
    std::string name;   // (its label)
    std::string group;
    bool builtin = false;
    core::Json settings;  // (as define_brush and brushes.json take them: core::brush_to_dict's)
};

struct Group {
    std::string name;
    bool builtin = false;
    std::vector<Preset> items;
};

struct Library {
    std::vector<Group> groups;  // (the built-in groups, then the person's)
    std::string problem;        // (brushes.json is there but cannot be read, or holds entries that are not brushes: why —
                                // it is never written over then)
};

// The built-in groups (made once: their tips' pictures made known to the dab engine).
const std::vector<Group>& builtin_groups();
// The built-in brush of a key (Genko's or a dab brush's), or none.
const Preset* builtin(std::string_view key);
// The built-in dab brush a key is the book's copy of: my_genko_<name> or my_genko_<name>_2… (the key a book takes when
// it has another brush of the first), or none.
const Preset* builtin_dab_of(std::string_view key);
// Whether a key is the person's own brush (my_…, not a built-in dab brush's).
bool is_own(std::string_view key);
// Whether two brushes' settings draw the same (core::brush_to_dict of each).
bool same_settings(const core::Json& a, const core::Json& b);

// The library: the built-in groups and the person's (brushes.json; one that cannot be read: `problem`, and none of its
// brushes; one with entries that are not brushes: `problem`, and the others — review P2-2). It never throws for what the
// file holds.
Library load(const std::filesystem::path& config_dir);
// The brush of a key or a name (its key first), or none.
const Preset* find(const Library& library, std::string_view key_or_name);
// {"groups": [{"name", "builtin", "brushes": [{"key", "name", "builtin", "engine": "dabs" | null, "size_mm", "hardness"?,
// "tip": "round" | "sampled" | "flat" | "image"}]}]} — `full`: each with its "settings" too.
core::Json list(const Library& library, bool full);

// brush.presets.save: settings as define_brush takes them (Genko's, a dab brush's "engine" and "dabs" with pictures as
// "png" or upstream's with "format": "photocraft", a paper's "png") kept as the person's brush `key` (none: a new
// my_<id>; one of the person's: put in its place, its group kept unless `group` is given), named `name` (else its
// "label"; a name another brush has gets " 2"…), in `group` (else 自分のブラシ) → {"key", "name", "group"}.
// core::Error("value") for settings a brush cannot have or a built-in key, core::Error("io"), PyUncaught OSError when the
// library cannot be read or written (it is left as it is).
core::Json save(const std::filesystem::path& config_dir, std::optional<std::string> key, const core::Json& settings,
                std::optional<std::string> name, std::optional<std::string> group,
                const std::map<std::string, core::Bytes>& pictures = {});
// brush.presets.delete: the person's brushes of these keys or names forgotten → {"deleted": [keys]}. core::Error("key")
// for one that is not there, ("value") for a built-in one (nothing is forgotten then).
core::Json remove(const std::filesystem::path& config_dir, const std::vector<std::string>& keys_or_names);
// The Preset Manager's edits of the person's brushes: {"action": rename | delete | move | new_group | rename_group |
// delete_group, "preset": a key or name, or several, "name"?, "group"?, "to"?, "index"?} → what changed (as
// render/preset_store's edit says it; move: "to" the group moved to — made when it is not there —, "group" the group the
// brushes are in, checked when given, as the other kinds' edits name them). core::Error("value") for what cannot be done
// (a built-in brush or group, a name taken, none named); nothing is written then. A library without groups is written
// without them still (the file as it was).
core::Json edit(const std::filesystem::path& config_dir, const core::Json& params);

// brush.presets.importAbr: the brushes of a Photoshop file (render/abr_map) kept as the person's, in `group` (made when it
// is not there; `replace`: its brushes forgotten first, as a file read again) → {"group", "brushes": [{"key", "name"}],
// "warnings", "version", "replaced"?: [the keys forgotten]}. abr::parse's errors, core::Error("value") past kMaxOwn.
core::Json import_abr(const std::filesystem::path& config_dir, std::string_view bytes, const std::string& group, bool replace);
core::Json import_mapped(const std::filesystem::path& config_dir, const abr_map::Import& mapped, const std::string& group, bool replace);
// The group a file's brushes go to when none is given: its name (review P3-11: "一般 2" for a file named as a built-in
// group, "読み込んだブラシ" for none).
std::string import_group(const std::string& stem);

// brush.defineFromSelection (ブラシを定義…): a sampled tip from the page as it shows (or the layer `layer_id` alone) within
// `area` (op_area's form; none: the whole page) — its paint is darkness × alpha × the area (black is full paint, as
// Photoshop's) at the page's working grid, cut to what holds paint — as a dab brush's settings named `name` (none:
// サンプルブラシ N) and its picture. core::Error("value") when nothing there holds paint, or the tip would be past 2500
// pixels a side.
struct Defined {
    core::Json settings;
    std::map<std::string, core::Bytes> pictures;
    std::uint32_t width = 0, height = 0;
    double dpi = 200.0;
};
Defined define_from(const core::Document& doc, const core::Page& page, const core::Json* area, const std::string* layer_id,
                    const std::string& name);
// The name a new defined brush takes: サンプルブラシ N (N one past the person's of that name).
std::string next_sample_name(const Library& library);

// The window's: a dab picture this process does not hold is read from the library's brush_tips/ when it is asked for.
void use_library_pictures(const std::filesystem::path& config_dir);

}  // namespace genko::render::brush_presets
