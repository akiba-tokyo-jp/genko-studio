#pragma once

#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 I-d (variables@1, SPEC.md FILE-01「.pcraft」・AUTO-01「変数とデータセット」): a .pcraft document's "variables" (PhotoCraft
// v0.2.0's doc/src/variables.rs, MIT) — the book's variables, its defs naming the layers made of the document's (the page's),
// a picture value's path read beside the .pcraft (a relative path within that folder: a picture this build reads, kept as the
// book's; any other left out, said); written from the book's defs on the page (a def on a line, or on a layer the document
// has not, left out and said; a picture value left out and said: a .pcraft holds a path, not the picture).

namespace genko::render::pcraft_variables {

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_variables
