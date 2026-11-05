#pragma once

#include <cstdint>
#include <optional>

#include "core/model.hpp"
#include "render/image.hpp"

// PC5 G-0 (type@1, SPEC.md TEXT-02, core/type_text.hpp): a type layer drawn on its page. Its record is set by the lines'
// typesetting (render/text: the balloons' draw_lines — its balloon, none by default, and its letters, rubies, emphasis and
// effects) at the page's resolution as a picture of its own ("RGBA", sRGB: drawn on black and on white, its cover the
// alpha), which the page draws as a layer's — its mask, opacity, blend, clipping, layer style and colour — among its
// layers (under the panels' borders, where a line is drawn over them). An 8-bit page lays it as it is, a page in precise
// colour brings it in as an 8-bit picture, a page of inks takes it by its rules: the three ways show it alike. Its letters
// are drawn without the book's text gamma (the picture of a layer has no page under it); what the typesetting cannot
// draw yet is left out and said (render::NotYetPorted with skip_unported), as a line's.

namespace genko::render {
namespace detail {
struct Ctx;
}

namespace type_layer {

// The type layer's picture over `area` of the page as the 8-bit page draws it ("RGBA"); nothing when it holds no record
// or none of it is there.
std::optional<Image> picture_8bit(const detail::Ctx& ctx, const core::Layer& layer, const Box& area);
// PC5 G-a: forget where the type layers' letters were found (書式 → すべてのテキストレイヤーを更新: a font file replaced
// under the same name may set them wider).
void forget_extents();
// PC5 G-a (tests): the most pixels one upright piece of a turned type layer took since the last reset.
std::int64_t largest_upright_pixels(bool reset = false);

}  // namespace type_layer
}  // namespace genko::render
