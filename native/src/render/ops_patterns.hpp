#pragma once

#include <string>
#include <utility>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC4 E3 (patterns@1, SPEC.md COLOR-09「パターン」): the book's patterns as the ops take them (render/ops_patterns.cpp).

namespace genko::render {

// add_pattern's work: {"name"?, "png" | "gkcr" | "gkir" (base64) | "asset", "mode"?, "depth"?, "dpi"?, "id"?} — the picture
// (a PNG: 8-bit RGB as RGBA, grey as LA; GKCR: 16- or 32-bit RGB; GKIR: grey of more bits, CMYK, Lab; an asset: the
// picture of a pattern the book keeps) as a pattern of the book. One the book keeps already (the same picture, mode, depth
// and dpi) is the one given back ({id, false}); else a new one ({id, true}). OpError for what cannot be.
std::pair<std::string, bool> add_pattern_to(core::Document& doc, const core::Json& given);
// A pattern and its picture (checked) kept by the book: one it keeps of the same picture, mode, depth and dpi given back
// ({id, false}), else this one — its id, or the next free one when it has none or one taken — ({id, true}).
std::pair<std::string, bool> keep_pattern(core::Document& doc, core::Pattern pattern, const core::Bytes& bytes);

// The pattern a fill op names (fill_area's, fill's "pattern"): {"id", "scale"?, "angle"?, "phase_mm"?, "link"?} of one
// the book keeps, or its picture given whole as add_pattern takes it (put into the book: the op's undo takes it out
// again) with its placement → the fill's pattern in its normal form (core::pattern_fill_spec).
core::Json fill_pattern(core::Document& doc, const core::Json& given);

// fill_area's and fill's "pattern" (PC4 E3): the pattern of c.op["pattern"] (fill_pattern) laid where `coverage` covers
// (an "L" picture at `dpi`, its top left `offset` on the page's grid at that dpi) into the layer `ti` of the page `at`, at
// the op's opacity (0..1), blend and preserve_transparency (and the layer's lock_alpha) — on a pen layer and an 8-bit
// paint layer (normal, its transparency not kept) a picture patch of it at 300 dpi; on a paint layer of more bits or of
// inks, and in a blend or keeping the transparency, into the layer's pixels in their own values (design 0.4: an
// RGB pattern's values as the page's, on a page of inks a pattern of its mode as it is, another by its rules).
// → {"pattern": its id}. OpError for a tone layer, a blend on a pen layer, an area that covers nothing.
core::Json fill_with_pattern(core::OpContext& c, std::size_t at, std::size_t ti, const Image& coverage, Point offset, int dpi);

}  // namespace genko::render
