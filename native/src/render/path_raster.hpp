#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/vector.hpp"
#include "render/image.hpp"

// PC3 節 C (C-0, SPEC.md COLOR-07, docs/cpp-migration/ARCHITECTURE.md「パスの覆い」): the coverage of a path — PhotoCraft
// v0.2.0's vector/src/{flatten, raster, stroke, shapes}.rs and lib.rs (MIT, native/third_party/photocraft/NOTICE.md):
// cubic Bézier flattening within a distance, the exact-area scanline rasterizer (no sub-sampling: every pixel's covered
// area, a winding count per component, the path operations and the inversion folded in), polygon stroking (miter, round
// and bevel joins; butt, round and square caps; dashes) and the live shapes. Coordinates are pixels of the grid drawn on
// (the caller maps the path's mm there: to_pixels), y down; a pixel (x, y) covers [x, x + 1) × [y, y + 1).

namespace genko::render::path_raster {

// The flattening tolerance a fill is drawn with (pixels: the curve within 0.01 px of its chords).
inline constexpr double kTolerance = 0.01;

// A box of pixels [x0, x1) × [y0, y1).
struct PixelBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

using XY = std::pair<double, double>;

// A flattened subpath: its points, which of them are the path's knots (joins go there), and whether it closes.
struct Polyline {
    std::vector<XY> pts;
    std::vector<bool> knot;
    bool closed = false;
};
Polyline flatten_subpath(const core::vector::Subpath& subpath, double tolerance);
std::vector<Polyline> flatten_path(const core::vector::Path& path, double tolerance);

// The exact-area rasterizer: components (sets of closed polygons, each filled by its rule) folded into the area by
// their ops (the first always combines), the whole inverted when asked.
class Rasterizer {
public:
    explicit Rasterizer(bool inverted = false) : inverted_(inverted) {}
    void add_component(const std::vector<std::vector<XY>>& polys, core::vector::PathOp op, core::vector::FillRule rule);
    // The pixels anything may cover (none: nothing — or, inverted, the whole plane: the caller clips).
    std::optional<PixelBox> pixel_bounds() const;
    bool inverted() const { return inverted_; }
    // The coverage (0..1) of every pixel of `box`, row-major.
    std::vector<float> render(const PixelBox& box) const;

private:
    struct Edge {
        double x0, y0, y1;
        int dir;
        std::uint32_t comp;
        double dxdy;
        double x_at(double y) const { return x0 + (y - y0) * dxdy; }
    };
    struct Component {
        core::vector::PathOp op;
        core::vector::FillRule rule;
    };
    struct BandEdge {
        double xa, xb, xm;
        std::uint32_t idx;
    };
    struct RowState;
    bool inside(const std::vector<int>& wind, int union_count) const;
    bool comp_inside(std::size_t comp, int w) const;
    void render_into(const PixelBox& box, float* out) const;
    void band(double a, double b, int x0, RowState& st, unsigned depth) const;

    std::vector<Edge> edges_;  // (sorted by y0)
    std::vector<Component> comps_;
    bool inverted_ = false;
    std::optional<std::array<double, 4>> bounds_;
    bool union_only_ = true;
};

// A stroke's geometry in pixels (dashes and their offset in pixels: on, off, on…).
struct StrokeStyle {
    double width = 1.0;
    core::vector::LineCap cap = core::vector::LineCap::Butt;
    core::vector::LineJoin join = core::vector::LineJoin::Miter;
    double miter_limit = 4.0;
    std::vector<double> dashes;
    double dash_offset = 0.0;
};
// The outline pieces of the stroked lines (each positively oriented: filled together with the non-zero rule, their union).
std::vector<std::vector<XY>> stroke_polygons(const std::vector<Polyline>& lines, const StrokeStyle& style, double tolerance);
// The polyline cut into dashes by length (`pattern` on, off, … in pixels).
std::vector<Polyline> dash_polyline(const Polyline& line, const std::vector<double>& pattern, double offset);

// The fill of a path: one component per subpath (open ones closed), folded by their ops, inverted when the path is.
Rasterizer fill_rasterizer(const core::vector::Path& path, double tolerance = kTolerance);
// The stroke of a path: every subpath's outline, one non-zero component.
Rasterizer stroke_rasterizer(const core::vector::Path& path, const StrokeStyle& style, double tolerance = kTolerance);
// A shape part's stroke as pixels: its width × `px_per_mm`, its dashes multiples of that width.
StrokeStyle stroke_style(const core::vector::ShapeStroke& stroke, double px_per_mm);
// The fill coverage of a path over `box`.
std::vector<float> path_coverage(const core::vector::Path& path, const PixelBox& box);
// A vector mask's values over `box` (all 1 when disabled; a path of no subpath all 1, all 0 inverted; density applied;
// not feathered — the drawing of a layer feathers).
std::vector<float> vector_mask_values(const core::vector::VectorMask& mask, const PixelBox& box);

// The path from mm on the page to pixels of a grid of `dpi` whose top left is the page's (`x0`, `y0`) pixel.
core::vector::Path to_pixels(const core::vector::Path& path, double dpi, int x0 = 0, int y0 = 0);
// The path's coverage as an "L" picture of `size` over the page at `dpi` (render/area_kinds' {"path"}).
Image path_mask(const core::vector::Path& path, double dpi, Size size);

// --- live shapes (vector/src/shapes.rs: knots clockwise on the page, from the top) ------------------------------------

inline constexpr double kKappa = 0.5522847498307934;  // (a quarter circle's handle, 4/3·(√2 − 1))
core::vector::Path rect_path(double x, double y, double w, double h);
std::array<double, 4> clamp_radii(double w, double h, const std::array<double, 4>& radii);
core::vector::Path rounded_rect(double x, double y, double w, double h, const std::array<double, 4>& radii);
core::vector::Path ellipse_path(double x, double y, double w, double h);
core::vector::Path polygon_path(double x, double y, double w, double h, int sides, double star_ratio);
core::vector::Path line_path(core::vector::XY a, core::vector::XY b, double weight);
// The live shape's path (a polygon turned by its angle about its box's middle).
core::vector::Path live_path(const core::vector::LiveShape& live);

}  // namespace genko::render::path_raster
