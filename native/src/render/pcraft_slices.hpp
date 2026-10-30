#pragma once

#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」, FILE-01「.pcraft」): a .pcraft's slices (the document's "slices": PhotoCraft
// v0.2.0's doc/src/slices.rs Slices — groupName, list [{id, groupId, origin user | layer | auto, layer, name, rect {x0, y0,
// x1, y1}, kind image | noImage | table, url, target, message, alt, cellTextIsHtml, cellText, horizontalAlign, verticalAlign,
// background [a, r, g, b] | null, outsets [t, l, b, r]}]; the rects in the document's pixels, a field left out its default
// as PhotoCraft's serde reads it; MIT, native/third_party/photocraft/NOTICE.md) as the page's, and back. Read: the user and
// layer-based slices in mm (an auto one, which PhotoCraft never keeps, left out), a layer-based one whose layer the
// document has not a user slice where it was; a value not of that shape leaves them out, said ("スライスは読めない形のため
// 外しました"). Written: the page's slices (a layer-based one where its layer puts it, naming the document's layer).

namespace genko::render::pcraft_slices {

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_slices
