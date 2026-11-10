#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

// PC3 C-c (SPEC.md COLOR-07, filter.liquify): Filter › Liquify's displacement field, edited by brush strokes and then used
// to draw the layer once. PhotoCraft v0.2.0's algo/src/liquify.rs (MIT, native/third_party/photocraft/NOTICE.md), in the
// page's mm.
//
// The field holds, for each node of a regular grid (one every `cell` mm), the offset d such that the place p shows what
// was at p + d(p) (an inverse map: drawing through it leaves no hole). Strokes are data (a tool, its brush and its
// points), applied in order; the same strokes over the same field give the same field bit for bit (the op replays them).
// Each tool says where the new picture at p comes from in the picture as it is: Forward Warp p − kΔ, Twirl a turn about
// the brush, Pucker and Bloat a scale about it, Push Left sideways; the new field is d'(p) = q + d(q) − p, d read
// bilinearly as it was before the dab. Reconstruct scales d toward 0, Smooth relaxes it toward its neighbours' mean.
// Freeze and Thaw paint a mask that weakens every tool where it is.

namespace genko::render::liquify {

enum class Tool {
    ForwardWarp,
    Reconstruct,
    Smooth,
    TwirlCw,
    TwirlCcw,
    Pucker,
    Bloat,
    PushLeft,
    Freeze,
    Thaw,
    ReconstructAll,  // (the Reconstruct… button: the whole unfrozen field toward 0 by `amount` %)
    FreezeAll,       // (Mask All)
    ThawAll,         // (Mask None)
    InvertFreeze,    // (Invert All)
};
// (forward_warp, reconstruct, smooth, twirl_cw, twirl_ccw, pucker, bloat, push_left, freeze, thaw, reconstruct_all,
// freeze_all, thaw_all, invert_freeze)
std::optional<Tool> tool_from(std::string_view text);
std::string_view to_string(Tool tool);
// A tool of the whole field (no brush, no points).
bool is_global(Tool tool);
// A tool that acts held still (each point a dab).
bool is_stationary(Tool tool);

// One stroke: the tool, its brush (size: diameter in mm; density 0 hard ..100 soft; pressure 0..100; rate 0..100 the
// still tools' speed), its points ([x, y, pressure 0..1] mm), Reconstruct All's amount (%).
struct Stroke {
    Tool tool = Tool::ForwardWarp;
    double size = 12.7;
    double density = 50;
    double pressure = 100;
    double rate = 80;
    std::vector<std::array<double, 3>> points;
    std::optional<double> amount;
};

class Field {
public:
    // A field over the box from (x0, y0), width × height mm, a node every `cell` mm; `unit` is the size of a working
    // pixel (mm: the brush's smallest radius and step are half of one).
    Field(double x0, double y0, double width, double height, double cell, double unit);

    double cell() const { return cell_; }
    std::size_t width() const { return w_; }
    std::size_t height() const { return h_; }

    // One stroke applied.
    void apply(const Stroke& stroke);
    bool is_identity() const;
    // The largest displacement (mm).
    double max_displacement() const;
    // The displacement at a point, bilinear (0 outside the field) and Catmull-Rom (as the picture is drawn).
    std::array<double, 2> sample(double x, double y) const;
    std::array<double, 2> sample_cubic(double x, double y) const;
    // The freeze mask at a point (bilinear).
    double freeze_at(double x, double y) const;
    // The box [x0, y0, x1, y1] (mm) of the places the displaced nodes reach with their reach of the cubic (none: none
    // displaced).
    std::optional<std::array<double, 4>> moved_box() const;
    // Where a point of the picture goes: the place q whose picture comes from it (q + d(q) = p), by fixed-point steps.
    std::array<double, 2> forward(double x, double y) const;

private:
    void dab(const Stroke& s, std::array<double, 2> c, std::array<double, 2> delta, double point_pressure);
    void stroke_begin(const Stroke& s, const std::array<double, 3>& p);
    void stroke_segment(const Stroke& s, const std::array<double, 3>& a, const std::array<double, 3>& b);
    std::array<double, 2> node_pos(std::size_t i, std::size_t j) const;
    std::array<double, 2> to_grid(double x, double y) const;

    double x0_, y0_, cell_, unit_;
    std::size_t w_, h_;
    std::vector<std::array<float, 2>> d_;
    std::vector<float> freeze_;
};

// The default node spacing for a field over a page: 2 working pixels up to 4.2 million of them, 4 past.
double auto_cell(double width_mm, double height_mm, double unit);

}  // namespace genko::render::liquify
