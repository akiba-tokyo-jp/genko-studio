// PC3 節 C (SPEC.md COLOR-06): the page size ops (C-e; 決定 8: the whole book or each page, page-spec@1) — registered
// here; C-e writes their work in this file (every element moved: core/page_geometry; the pixels: render/resample).

#include "render/ops_pc3.hpp"

namespace genko::render {

void register_pagesize_ops(core::OpRegistry& registry) {
    // each takes "pages"?: [numbers] | "all" (Image Size and Canvas Size: all by default; the others: the page named)
    registry.add("resize_pages", not_yet("resize_pages", "C-e"));      // (width_mm, height_mm | width_px, height_px, dpi?, resample)
    registry.add("set_canvas_size", not_yet("set_canvas_size", "C-e"));  // (width_mm, height_mm, relative?, anchor, extension?)
    registry.add("crop_page", not_yet("crop_page", "C-e"));            // (rect | area, angle?, delete_pixels?)
    registry.add("trim_page", not_yet("trim_page", "C-e"));            // (based_on, top, bottom, left, right)
    registry.add("reveal_all", not_yet("reveal_all", "C-e"));
    registry.add("rotate_pages", not_yet("rotate_pages", "C-e"));      // (turn | angle, interp, text: upright)
}

}  // namespace genko::render
