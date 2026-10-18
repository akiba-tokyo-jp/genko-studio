#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC3 (B-0, SPEC.md COLOR-05「区域の種類」, docs/cpp-migration/ARCHITECTURE.md): the kinds and modifiers of an area this
// build adds to the selection language of render/selection (rect, ellipse, poly, mask, layer, color, all, saved,
// channel, union, intersect, subtract; grow_mm, invert, feather_mm — whose meaning is the ops' contract and stays as it
// is). The bus resolves an area that holds any of them before the op (core::add_area_key), as it resolves the others:
// selection::to_mask makes the kind's mask, then lays the modifiers on it in this order —
//   the kind → matrix → grow_mm → expand_mm, contract_mm → border_mm → smooth_mm → invert → feather_mm
// (without the new ones: as before). "antialias": true on a poly, rect or ellipse draws its edge grey (4×4 samples).
//
// The table is the one place a kind is named: a group that writes a kind's solving puts its function in its row (its
// own file; the row's `solve` is empty until then, and the kind is refused with not_yet_ported naming it and its group).

namespace genko::render::area_kinds {

// What a kind is solved in: the page and the book, the grid (`dpi`, the page's `size` there) and a way to solve an area
// inside it (a nested one, such as grow's or refine's "area": as selection::to_mask, its budget and depth counted).
struct Context {
    const core::Page& page;
    const core::Document* episode;
    int dpi;
    Size size;
    std::function<Image(const core::Json& area)> nested;
};

// A kind's mask: an "L" picture of `size` over the page (255 inside), from the kind's value and the whole area.
using Solve = Image (*)(const core::Json& value, const core::Json& area, const Context& context);

struct Kind {
    std::string_view key;
    std::string_view group;  // (who writes its solving: "B-0", "B1", "B3", "C-a")
    Solve solve;             // (null: not in this build yet)
};

// The kinds this build adds, in the order an area's keys are looked at.
const std::vector<Kind>& kinds();
// The first of them the area holds (null: none).
const Kind* kind_of(const core::Json& area);
// The kind's mask; not_yet_ported for a kind not in this build yet.
Image solve(const Kind& kind, const core::Json& area, const Context& context);

// The modifiers this build adds, each with where it goes in the order above: before grow_mm (matrix) or after it
// (expand_mm, contract_mm, border_mm, smooth_mm, in that order).
Image before_grow(Image mask, const core::Json& area, const Context& context);
Image after_grow(Image mask, const core::Json& area, const Context& context);

// Does the area's poly, rect or ellipse draw anti-aliased ("antialias": true)?
bool antialiased(const core::Json& area);
// The polygon (pixels on the grid) drawn anti-aliased over a picture of `size`: selection_ext's polygon.
Image antialiased_polygon(const std::vector<std::pair<double, double>>& corners, Size size);

// Every key of the table and of the modifiers made known to the bus (core::add_area_key). Called when the ops are
// registered; again: nothing more.
void use_keys();

}  // namespace genko::render::area_kinds
