#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Panel guides for image tools (Python's genko/guide.py): composition, pose, keepout, mask and compare, each drawn at
// the generation size over the generation box (the panel grown by pad_mm on each side): white ground, simple black or
// grey shapes, so any image tool can take them as reference images. Pixel for pixel as Pillow draws them.

namespace genko::service::guide {

using core::Json;
using Box = std::array<double, 4>;  // x, y, w, h (mm)

// guide.GenBox: the page area (mm) an image of `px` covers.
struct GenBox {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::array<int, 2> px{};

    static GenBox for_frame(const core::Frame& frame, double pad_mm, std::array<int, 2> px);
    std::array<double, 2> to_px(double x_mm, double y_mm) const;
    std::array<double, 4> box_px(const Box& box) const;  // (x0, y0, x1, y1)
    Json box01(const Box& box) const;                    // [round(…, 3) of the box in 0..1]
};

int line_width(const GenBox& box, double mm);

// keepout_boxes(episode, page, frame, grow_mm): the balloons of this panel, grown a little, and why.
std::vector<std::pair<Box, std::string>> keepout_boxes(const core::Document& doc, const core::Page& page, const core::Frame& frame,
                                                       double grow_mm = 2.0);
// has_mannequin(page, frame)
bool has_mannequin(const core::Page& page, const core::Frame& frame);

render::Image composition(const core::Page& page, const core::Frame& frame, const GenBox& box);
render::Image pose(const core::Page& page, const core::Frame& frame, const GenBox& box);
render::Image keepout(const core::Document& doc, const core::Page& page, const core::Frame& frame, const GenBox& box);
render::Image mask(const GenBox& box, const std::vector<Box>& regions_mm);
// fit_cover(image, size): scaled (Lanczos) to cover the size, the middle cut out.
render::Image fit_cover(const render::Image& image, std::array<int, 2> size);
// compare(candidate, page, frame, box): the candidate cover-fitted to the box with the composition in red at 50%.
render::Image compare(const render::Image& candidate, const core::Page& page, const core::Frame& frame, const GenBox& box);
// to_png(image): Image.save(format="PNG", optimize=False).
std::string to_png(const render::Image& image);
// ink_ratio(image): the share of dark pixels (round(…, 4)).
double ink_ratio(const render::Image& image);

}  // namespace genko::service::guide
