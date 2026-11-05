#pragma once

#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」, FILE-01「.pcraft」): a PhotoCraft document's layer comps as the
// page's — its "layer_comps" (each {id, name, comment, apply_visibility, apply_position, apply_appearance, states: [{layer,
// visible?, position?: [x, y] pixels, appearance?: {blend, opacity, fill_opacity, effects}}]}), "last_applied_comp" and
// "last_document_state" (PhotoCraft v0.2.0's format/src/manifest.rs LayerCompM, MIT) — and back. A state's layer is the
// document's layer id (render/pcraft_read and formats/pcraft_write give the contexts' `layers`); one the document has not
// is kept as a layer gone (`pcraft-<id>`: Photoshop's warning). A place is the top left of the layer's content in the
// document's pixels (mm here). Genko draws no layer effects of a .pcraft: an appearance's effects are left out (said); its
// fill opacity goes into its opacity, as a layer's does. Written: a state of a layer the document does not have is left
// out; an appearance's layer style is not written (its fill opacity is).

namespace genko::render::pcraft_comps {

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_comps
