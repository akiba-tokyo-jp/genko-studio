#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"
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

// The resolution fill_area's pattern is laid at on a page: its grid on a page of grid "finest" (raster::working_dpi),
// else 300 dpi (a colour's fill's).
int pattern_fill_dpi(const core::Page& page);

// fill_area's and fill's "pattern" (PC4 E3): the pattern of c.op["pattern"] (fill_pattern) laid where `coverage` covers
// (an "L" picture at `dpi`, its top left `offset` on the page's grid at that dpi) into the layer `ti` of the page `at`, at
// the op's opacity (0..1), blend and preserve_transparency (and the layer's lock_alpha). Normal and its transparency not
// kept, a picture patch of it (at pattern_fill_dpi) as a colour's fill lays one: on a pen layer whatever its lines and
// the page, and on a paint layer of 8 bits — or of a precise page it cannot be precise on (keep_page_modes' rule) — on a
// page that is not of inks nor of the finest grid. Otherwise into the layer's pixels in their own values on their own
// grid (design 0.4: an RGB pattern's values as the page's, on a page of inks a pattern of its mode as it is, another by
// its rules), cut at the panels when the layer is; a paint layer with no pixels yet of a precise RGB page that can hold
// precise pixels gets them first (render/paint_fill since E4's merge: paint_values and becomes_precise, a colour's fill's and
// a gradient's way — an ink paint layer's lines kept as lines in the 通常 mode). → {"pattern": its id}. OpError for a tone layer, a blend on a pen layer, an area that
// covers nothing.
core::Json fill_with_pattern(core::OpContext& c, std::size_t at, std::size_t ti, const Image& coverage, Point offset, int dpi);

// Shape parts copied (the clipboard's items, a material's): the patterns of `book` their fills name, as items carry them
// ({"id": the id the parts name, "name", "png" | "gkcr" | "gkir": base64, "mode", "depth", "dpi"}).
core::Json patterns_of_parts(const core::Document& book, const std::vector<core::vector::ShapePart>& parts);
// Pasted (paste's items): the patterns they carry put into the book (add_pattern's: the book's own of the same picture,
// else a new one) and the parts' fills pointed at them. A part naming a pattern none carries is left as it is (the bus's
// check refuses one the book does not keep). OpError for a pattern not well formed.
void bring_part_patterns(core::Document& doc, std::vector<core::vector::ShapePart>& parts, const core::Json& patterns);

}  // namespace genko::render
