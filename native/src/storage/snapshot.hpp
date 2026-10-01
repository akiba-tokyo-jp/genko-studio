#pragma once

#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

namespace genko::storage {

// Python's genko.headless.snapshot(episode, full): what `genko inspect` prints (pages with their leaves in reading
// order, lines, layer briefs, stroke counts; with full, every name and ink stroke's points).
core::Json snapshot(const core::Document& doc, bool full = false);

// Python's genko.headless.inspect_stroke: {"id", "page", "layer", "points", "kind"}. Throws core::Error("key",
// "no stroke <id>") when there is no such stroke.
core::Json inspect_stroke(const core::Document& doc, std::string_view stroke_id);

}  // namespace genko::storage
