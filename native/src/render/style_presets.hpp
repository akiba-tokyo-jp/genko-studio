#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/preset_store.hpp"

// PC4 E6 (style.presets.*, window.panel.styles, SPEC.md COLOR-09「スタイル」): the layer styles of this machine — the
// settings folder's styles.json, in render/preset_store's form — as Photoshop's スタイル panel holds them, and the built-in
// groups 基本・テキスト効果・ボタン (PhotoCraft v0.2.0's engine/src/presets/styles.rs builtin(): its own designs, MIT:
// native/third_party/photocraft/NOTICE.md; its pixel sizes taken as pixels of 150 dpi, the names in Japanese).
//
// A preset: {"name", "effects": [effect, …], "enabled"?: false, "fill_opacity"?, "blend_if"?, "blend"?: mode} — a layer
// style in core::layer_style_spec's form (its effects may be none: デフォルトスタイル（なし）) and, when it was saved with
// the layer's blending options (レイヤー効果とレイヤーの混合オプションを含める), the layer's blend mode. A pattern it
// names is the library's (render/pattern_library), not a book's: {"library": its name, "asset"?: its picture's ref,
// "scale", "angle", "phase_mm", "link"} — found by its picture first (a pattern renamed in the library is still found),
// then by its name. The ops never read the library (PC1 9 章): the window and the CLI make set_layer_style of a preset
// (apply_op: its patterns given as their pictures, put into the book by the op).

namespace genko::render::style_presets {

// styles.json: its built-in groups, how an item is checked.
const preset_store::Kind& kind();

// A preset in its normal form, of a style given as above (its patterns the library's), named `name`. core::Error
// ("value") says what is wrong.
core::Json item_of(const core::Json& given, std::string_view name);

// 新規スタイル…: a preset of a layer of `book` — its effects (`effects`) and its blending options (`blending`: its fill,
// Blend If and blend mode) —, each pattern its style names put into the library (`pattern_groups`, the library of
// `config_dir`: the library's own of the same picture when it has one; `pattern_added` set when one was added, the caller
// then saving the library). core::Error("value") when neither is asked for.
core::Json item_of_layer(const core::Document& book, const core::Layer& layer, bool effects, bool blending, std::string_view name,
                         const std::filesystem::path& config_dir, std::vector<preset_store::Group>& pattern_groups, bool& pattern_added);

// The library patterns a preset names (each once): {"library", "asset"?}.
std::vector<core::Json> patterns_of(const core::Json& item);

// The style a preset gives (set_layer_style's "style"; null for none), each of its patterns given as its picture with its
// placement (add_pattern's form): core::Error("value") for a pattern the library of `config_dir` does not have.
core::Json style_for(const core::Json& item, const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& pattern_groups);
// The op that applies a preset to layers of a page: {"op": "set_layer_style", "page", "ids", "style", "add"?, "blend"?} —
// "add" (Shift を押しながら: the preset's effects after the layers' own), the blend mode only when the preset has one.
core::Json apply_op(const core::Json& item, const core::Json& page, const std::vector<std::string>& ids, bool add,
                    const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& pattern_groups);

// {"groups": [{"name", "styles": [{"name", "effects": [kinds], "blend"?, "fill_opacity"?}]}]} (style.presets.list).
core::Json list(const std::vector<preset_store::Group>& groups);

// .asl (スタイルを読み込み…): the file's styles as presets (render/asl), its patterns put into the pattern library
// (`pattern_groups`; the library's own of the same picture when it has one) in the group `pattern_group`. Lengths are
// the file's pixels at `dpi`. → {"items": [presets], "patterns": [names added], "notes": [what was not read as it is]}.
// core::Error("format") for a file that is not an .asl or cannot be read, core::Error("too_large") for one past the bounds.
core::Json import_asl(std::string_view data, double dpi, const std::filesystem::path& config_dir, std::vector<preset_store::Group>& pattern_groups,
                      const std::string& pattern_group, bool& pattern_added);

// The panel's swatch: the preset on a rounded grey square, `size`² (16..256) RGBA (transparent around it). Its patterns
// from the library of `config_dir` (one it does not have is left out). Made off the window's thread.
Image thumbnail(const core::Json& item, int size, const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& pattern_groups);

}  // namespace genko::render::style_presets
