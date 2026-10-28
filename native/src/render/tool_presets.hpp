#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "render/preset_store.hpp"

// PC4 E2 (tool.presets.new・.list・.edit・.reset・.select, window.panel.toolPresets; SPEC.md COLOR-09「ツールプリセット」,
// design section E 2.4 and 3.5): a tool and its settings under a name, as Photoshop's ツールプリセット panel holds them —
// the settings folder's tool_presets.json in render/preset_store's form (PhotoCraft v0.2.0's engine/src/presets/tools.rs,
// MIT: native/third_party/photocraft/NOTICE.md). An item is {"name", "tool", "options": {"brush"?: a brush's key,
// "fields"?: {the tool's settings by their names in the window (objectName): a number, text or yes/no}, "foreground"?:
// [r, g, b] (描画色を含める)}} — "tool" a tool's name in the window (pen, pencil, eraser, gradient, fill, …). The window
// reads its tool page's fields into "fields" and sets them back; the CLI keeps what it is given. Nothing of it is a book's
// (the ops never read it).

namespace genko::render::tool_presets {

// tool_presets.json: its built-in group (Genko's own presets), how an item is checked.
const preset_store::Kind& kind();

// A preset of `tool` named `name` (none: the tool's name) with `options` (none: {}), the colour given or not.
// core::Error("value") says what is wrong.
core::Json item_of(std::string_view tool, std::optional<std::string> name, const core::Json& options);

// tool.presets.list: {"presets": [{"name", "tool", "group"}]} — of `tool` only when it is given (現在のツールのみ).
core::Json list(const std::vector<preset_store::Group>& groups, std::optional<std::string_view> tool = std::nullopt);

// tool.presets.select: the preset named (its whole item) — core::Error("key") for one not there.
const core::Json& select(const std::vector<preset_store::Group>& groups, std::string_view name);

}  // namespace genko::render::tool_presets
