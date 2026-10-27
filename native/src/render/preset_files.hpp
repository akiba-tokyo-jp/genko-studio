#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// PC4 E2 (edit.presets.presetManager・.exportImportPresets・.migratePresets; SPEC.md COLOR-09「プリセットマネージャー」, design
// section E 0.3 and 2.4): the presets of this machine of every kind together — ブラシ (render/brush_presets), グラデーション
// (render/gradient_presets), パターン (render/pattern_library), シェイプ (render/shape_presets), ツールプリセット
// (render/tool_presets) — listed and edited as Photoshop's プリセットマネージャー does, written to one file and read back
// (PhotoCraft v0.2.0's engine/src/edit_menu_cmds.rs preset_manager and export_import, migrate_cmds.rs; MIT:
// native/third_party/photocraft/NOTICE.md), and taken from another settings folder or such a file (プリセットを移行: the
// groups the library does not have yet). Each kind's library is read again before each change and written whole, so
// what another window or `genko …` wrote is never written over. (PC4 E6: スタイル, render/style_presets, among them.)
//
// The file (.genkopresets, JSON, 256 MB at most): {"genko_presets": 1, "brushes": [{"name", "brushes": [{"key",
// "settings"}]}], "brush_pictures": {ref: base64 PNG}, "gradients": [{"name", "items"}], "patterns": [{"name", "items"}],
// "pattern_pictures": {ref: base64}, "shapes": [{"name", "shapes": [{"name", "path"}]}], "tool_presets", "styles" (E6): [{"name",
// "items"}]} — each kind only when it was asked for.

namespace genko::render::preset_files {

inline constexpr std::size_t kMaxFileBytes = std::size_t(256) << 20;
inline constexpr int kVersion = 1;

// The kinds in the Preset Manager's order: brushes, gradients, patterns, shapes, tool_presets, styles (review P3-13: this table
// alone names them — a kind added here, E6's styles, is listed by the Preset Manager and the 書き出し／読み込み window,
// and refused by every function below until each knows it).
const std::vector<std::string>& kinds();
// A kind's name on the screen (UTF-8): ブラシ, グラデーション, パターン, シェイプ, ツールプリセット; the kind itself for one not
// known.
std::string kind_title(std::string_view kind);

// {kind: {"groups": [{"name", "builtin"?, "presets": [names]}], "problem"?}} for the kinds asked (none: all).
core::Json manager_list(const std::filesystem::path& config_dir, const std::vector<std::string>& wanted = {});
// One kind's edit as its library takes it ({"action": rename | delete | move | new_group | rename_group | delete_group,
// …}) → what changed. core::Error("value") for a kind not known and for what cannot be done; core::Error("io") when the
// kind's file cannot be read (it is left as it is).
core::Json manager_edit(const std::filesystem::path& config_dir, const std::string& kind, const core::Json& params);

// The file's text of the kinds asked (none: all): the person's presets (`builtins`: the built-in ones too), their
// pictures with them; `counts` gets {kind: presets}. core::Error("too_large") past kMaxFileBytes.
std::string export_text(const std::filesystem::path& config_dir, const std::vector<std::string>& wanted, bool builtins, core::Json* counts = nullptr);
// A file's presets of the kinds asked (none: all) added: each into its group of the same name (made when it is not
// there), a name taken getting " 2"…, one the library has as it is not added again → {"added": {kind: n}}.
// core::Error("format") for text that is not such a file.
core::Json import_text(const std::filesystem::path& config_dir, std::string_view text, const std::vector<std::string>& wanted = {});
// プリセットを移行: from another settings folder (its brushes.json, gradients.json, patterns.json, shapes.json,
// tool_presets.json and their pictures) or a file export_text wrote — the groups whose names the library does not have
// yet added (as a whole), again none: nothing → {"migrated": n, "added": {kind: n}}.
core::Json migrate(const std::filesystem::path& config_dir, const std::filesystem::path& from);

}  // namespace genko::render::preset_files
