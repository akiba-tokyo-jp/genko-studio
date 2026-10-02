#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/geometry.hpp"
#include "core/model.hpp"

// Where a panel reaches when it runs off the page (Python's genko/placement.py): what the drawing ops need to know
// whether a line lies in a panel, and what the renderer needs to cut the layers and draw the borders.

namespace genko::core {

inline constexpr double kEdgeEpsMm = 0.5;

struct OuterEdges {
    bool left = false, top = false, right = false, bottom = false;
};

// Which sides of the panel touch the basic frame's edge (so they may bleed). Like Python, the page's own side
// decides the basic frame (the book's start_side is not given).
OuterEdges outer_edges(const Page& page, const Frame& frame);

// A slanted or free-form bleed panel run out to the bleed: its corners on the basic frame's edge moved out to the
// bleed's edge (floats). Nothing for other panels.
std::optional<std::vector<PointF>> bleed_poly(const Page& page, const Frame* frame);

// A bleed panel with rounded corners or a styled border, as drawn: its corners on the basic frame's edge taken out
// past the paper by beyond_mm (nothing: to the bleed's edge), its other corners rounded as asked. Nothing for a
// panel that does not bleed.
std::optional<std::vector<Point>> bleed_outline(const Page& page, const Frame* frame,
                                                std::optional<double> beyond_mm = 8.0);

// Whether a side a→b of a bleed polygon lies along the bleed's edge (it is cut off: no border there).
bool on_bleed_edge(const Page& page, const PointF& a, const PointF& b);

// Whether (x, y) is inside the polygon (even-odd).
bool in_poly(std::span<const PointF> points, double x, double y);

// The box a placed picture is clipped to: the page ("none" or no panel), the panel's box ("frame") or the panel's
// box run out to the bleed on its outer sides ("bleed").
Rect clip_box(const Page& page, const Frame* frame, std::string_view clip_to);

}  // namespace genko::core
