#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

// View › Snap and Snap To (PC3 C-d, SPEC.md COLOR-07「移動・整列」): what a drag snaps to, as plain geometry on the page's mm
// — PhotoCraft v0.2.0 crates/engine/src/snap.rs, MIT (native/third_party/photocraft/NOTICE.md). A set of targets is made
// as a drag starts (the guides, the grid, the paper's edges and middle, the other layers' edges and middles); each move of
// the pointer asks it to snap a point (a corner of the crop box or of a shape, a pen's anchor, a handle of the free
// transform box) or a moving box (the layers the move tool drags, the free transform box moved). The reach is the
// screen's 8 px over the zoom. What snapped comes back as lines for the canvas to draw (the layer-to-layer ones and the
// paper's middle as Photoshop's smart guides).

namespace genko::core::snap {

enum class Kind { Guide, Grid, PaperEdge, PaperMiddle, LayerEdge, LayerMiddle };

// Layer-to-layer and the paper's middle: what Photoshop shows as smart guides.
bool is_smart(Kind kind);

using Box = std::array<double, 4>;  // x0, y0, x1, y1 (mm)

// A line to snap to: at `pos` on one axis (x for an upright line), `span` its reach on the other (for drawing).
struct Target {
    double pos = 0.0;
    Kind kind = Kind::Guide;
    std::pair<double, double> span{0.0, 0.0};
};

// What a drag snapped to, for drawing: an upright line at x = pos from y = from to y = to (or across at y = pos).
struct Line {
    bool vertical = false;
    double pos = 0.0;
    double from = 0.0;
    double to = 0.0;
    Kind kind = Kind::Guide;
};

struct Targets {
    std::vector<Target> x;  // upright lines (their x)
    std::vector<Target> y;  // lines across (their y)
    std::optional<double> grid;  // the grid's step (mm) when it snaps to the grid (its lines from the page's (0, 0))
    Box extent{0.0, 0.0, 0.0, 0.0};  // (where the grid's lines reach)

    // A box's edges and its middle lines.
    void add_box(const Box& box, Kind edge, Kind middle);
    bool empty() const { return x.empty() && y.empty() && !grid; }
    // The target nearest v within reach on one axis (vertical: the x's); a guide wins a tie, as in Photoshop.
    std::optional<Target> nearest(bool vertical, double v, double reach) const;
    // A point snapped on each axis on its own to its nearest target within reach, and the lines it snapped to.
    std::pair<std::array<double, 2>, std::vector<Line>> point(const std::array<double, 2>& p, double reach) const;
    // A moving box: on each axis its edge or middle nearest a target decides the move (dx, dy) that snaps it; the lines
    // are every one it lines up with after (so neighbours of one size show both edges).
    std::pair<std::array<double, 2>, std::vector<Line>> box(const Box& b, double reach) const;
};

// The boxes together (none: none).
std::optional<Box> united(const std::vector<Box>& boxes);

}  // namespace genko::core::snap
