#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/slices.hpp"

// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」): where a page's slices lie in pixels and how they are numbered — PhotoCraft
// v0.2.0's crates/doc/src/slices.rs (auto_slices, resolve, base_name: MIT, native/third_party/photocraft/NOTICE.md) over
// Genko's slices in mm. A slice's edges are its mm at the pixels a mm of the picture (the page's own resolution on the
// canvas and in the lists; the size written by Save for Web), each edge rounded alone: two slices sharing an edge share its
// pixel, so no gap or overlap is made between them. The auto slices (never kept) fill the rest of the page around the user
// and layer-based ones, made again each time, as Photoshop makes them.

namespace genko::core {

// A box of whole pixels, half-open: [x0, x1) × [y0, y1).
struct SliceBox {
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    std::int64_t width() const { return x1 - x0; }
    std::int64_t height() const { return y1 - y0; }
    SliceBox intersect(const SliceBox& other) const;
    bool operator==(const SliceBox&) const = default;
};

// The pixels a mm across and down a picture of the page's paper (`width_mm` × `height_mm` drawn `width_px` × `height_px`),
// and its canvas.
struct SliceGrid {
    double px_x = 1.0, px_y = 1.0;
    SliceBox canvas;
};
SliceGrid slice_grid(double width_mm, double height_mm, std::int64_t width_px, std::int64_t height_px);
// A rect [x, y, w, h] (mm) in the grid's pixels: each edge rounded alone (round half away from zero).
SliceBox slice_box(const std::array<double, 4>& rect_mm, const SliceGrid& grid);
// … and back: a box's rect (mm, 4 places).
std::array<double, 4> slice_rect_mm(const SliceBox& box, const SliceGrid& grid);

// The rectangles that tile `canvas` minus `taken` (PhotoCraft's auto_slices: the canvas cut along every slice's edges, the
// free cells joined into runs along each band, then runs of the same columns joined down).
std::vector<SliceBox> auto_slices(const SliceBox& canvas, const std::vector<SliceBox>& taken);

// A slice as shown, listed and written: the user and layer-based ones and the auto ones, numbered as Photoshop numbers them
// (top to bottom, then left to right, from 1).
struct ResolvedSlice {
    int number = 0;
    std::optional<std::int64_t> id;  // (none: an auto slice)
    std::string origin;              // user | layer | auto
    SliceBox box;
    std::string name;                // its own, or the made one: <base>_NN (<base> alone when it is the only one)
    std::string kind = "image";
};
// `stored`: each user and layer-based slice's box (in the list's order, a layer-based one's made again from its layer),
// clipped to the canvas — empty ones left out. `base`: the base of the made names (slice_base).
std::vector<ResolvedSlice> resolve_slices(const Slices& slices, const std::vector<std::pair<std::int64_t, SliceBox>>& stored, const SliceBox& canvas,
                                          const std::string& base);

// A name safe for a file: letters and digits (any script), '-' and '_' kept, the rest '_' (PhotoCraft's base_name and
// slice_file); empty: `otherwise`.
std::string slice_file_name(std::string_view name, std::string_view otherwise);

// The slice of the id, or none.
const Slice* find_slice(const Slices& slices, std::int64_t id);
// A fresh id (past every one and next_id).
std::int64_t fresh_slice_id(const Slices& slices);

// slice.divide: the rect cut into `rows` (down) × `columns` (across) parts of equal size, row by row (mm, 4 places).
std::vector<std::array<double, 4>> divide_rect(const std::array<double, 4>& rect_mm, int rows, int columns);
// slice.fromGuides: the paper (`width` × `height` mm) cut along the guides inside it (`xs` upright guides, `ys` across):
// the cells, row by row; none when no guide lies inside.
std::vector<std::array<double, 4>> guide_cells(double width_mm, double height_mm, std::vector<double> xs, std::vector<double> ys);

}  // namespace genko::core
