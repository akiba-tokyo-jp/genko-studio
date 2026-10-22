#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"

// The shape presets (PC3 C-b, SPEC.md COLOR-07「シェイプのプリセット」: ウィンドウ → シェイプ, カスタムシェイプツール, 編集 →
// カスタムシェイプを定義…, `genko shapes`): groups of named shapes, each a path in a 100 × 100 box — PhotoCraft v0.2.0's
// engine/src/presets/shapes.rs (its path language and its own drawings, MIT: native/third_party/photocraft/NOTICE.md),
// their names in Japanese. A machine's setting, not a book's (PC1 9 章: the ops do not read machine settings): the
// person's groups are kept in the settings folder's shapes.json (none: the built-in ones); placing one, the window takes
// its path out and sends it in add_shape_part's "shape": {"kind": "path"} (fitted into the box drawn).

namespace genko::render::shape_presets {

struct Preset {
    std::string name;
    core::vector::Path path;
};
struct Group {
    std::string name;
    std::vector<Preset> shapes;
};

// The group Edit › Define Custom Shape puts its shapes in when none is named.
inline constexpr std::string_view kCustomGroup = "カスタムシェイプ";
// The settings file (in core::config_dir()).
inline constexpr std::string_view kFile = "shapes.json";

// PhotoCraft's path language: M x y (a new subpath), L x y… (lines), C x1 y1 x2 y2 x y… (cubics), Q x1 y1 x y…
// (quadratics), Z (closed), O cx cy r (a circle), and ! before M or O (that subpath subtracts: a hole). core::Error
// ("value") says what is wrong.
core::vector::Path parse(std::string_view source);

// The built-in groups (記号, 矢印, フキダシ, 自然, 動物).
std::vector<Group> builtin();

// The groups kept in `config_dir` (its shapes.json), else the built-in ones; a file that cannot be read: the built-in
// ones too (`problem`: why, when it is given).
std::vector<Group> load(const std::filesystem::path& config_dir, std::string* problem = nullptr);
// The groups written to `config_dir`'s shapes.json (atomically).
void save(const std::filesystem::path& config_dir, const std::vector<Group>& groups);

// The path's anchors and handles mapped onto rect [x, y, w, h] (its control box's); `keep_aspect`: scaled alike both
// ways, centred in the rect.
core::vector::Path fit(const core::vector::Path& path, const std::array<double, 4>& rect, bool keep_aspect = true);

// The preset named (in `group` when given; the first of the name otherwise), or none.
const Preset* find(const std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group = std::nullopt);

// shape.presets.list: {"groups": [{"name", "shapes": [{"name", "subpaths"}]}]}.
core::Json list(const std::vector<Group>& groups);
// shape.presets.new / edit.defineCustomShape: the path (fitted into the 100 box) kept under a name not taken yet (the
// name given, then "name 2"…) in `group` (made when it is not there; none: カスタムシェイプ) → {"name", "group"}.
core::Json add(std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group, const core::vector::Path& path);
// shape.presets.edit: {"action": rename | delete | move | new_group | rename_group | delete_group, "preset": a name or
// names, "group"?, "name"?, "to"?, "index"?} → what changed. core::Error("value") for what cannot be done.
core::Json edit(std::vector<Group>& groups, const core::Json& params);
// shape.presets.reset: the built-in groups (`append`: after the person's, a name taken getting a number).
std::vector<Group> reset(const std::vector<Group>& groups, bool append);

// A preset's swatch: its coverage fitted into a `size`² picture with a small margin ("L", white the shape).
Image thumbnail(const core::vector::Path& path, int size);

}  // namespace genko::render::shape_presets
