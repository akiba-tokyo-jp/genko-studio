#pragma once

#include <stop_token>
#include <string_view>

#include "core/model.hpp"
#include "render/image.hpp"

// Covers drawn (Python's genko/covers.py: draw_folds and front_of; the rest of covers.py is core's, core/covers.hpp).

namespace genko::render {

// covers.draw_folds(image, page, dpi, binding): where a jacket or a band folds (blue dashed lines down the trim at each
// part's edges) and what each part is (袖・表紙・背・裏表紙, in the default face at the middle of the part), for the
// name and proof views. Drawn onto `part`, the box `area` of the page drawn at `page_size` pixels; nothing for a page
// that has no folds. `binding` is "right" or "left" (the book's).
void draw_folds(Image& part, const Box& area, Size page_size, const core::Page& page, int dpi, std::string_view binding,
                std::stop_token stop = {});

// covers.front_of(page, image, dpi, binding, which): the front cover (`which` "表紙") or the back ("裏表紙") cut out of a
// jacket's or a band's picture at the trim's height; the picture itself for a page without that part.
Image front_of(const core::Page& page, const Image& image, int dpi, std::string_view binding = "right",
               std::string_view which = "表紙");

}  // namespace genko::render
