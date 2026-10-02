#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/geometry.hpp"
#include "core/model.hpp"

// Where a panel reaches when it runs off the page (Python's genko/placement.py): the parts that the drawing ops need
// to know whether a line lies in a panel.

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

// Whether (x, y) is inside the polygon (even-odd).
bool in_poly(std::span<const PointF> points, double x, double y);

// The box a placed picture is clipped to: the page ("none" or no panel), the panel's box ("frame") or the panel's
// box run out to the bleed on its outer sides ("bleed").
Rect clip_box(const Page& page, const Frame* frame, std::string_view clip_to);

}  // namespace genko::core
