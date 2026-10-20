#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "render/resample.hpp"

// PC3 C-c (SPEC.md COLOR-07, edit.perspectiveWarp): Edit › Perspective Warp — planes (quads) laid over the picture, whose
// corners are then moved; the picture follows a piecewise homography. PhotoCraft v0.2.0's algo/src/perspective.rs (MIT,
// native/third_party/photocraft/NOTICE.md).
//
// Each plane maps its source quad onto its destination quad through the unit square (H_dst ∘ H_src⁻¹). Where two planes
// share an edge, the average of both planes' images of each point of the edge is the target and the difference is faded
// linearly across the plane, so the map is continuous across shared edges, exactly the homography for a lone plane and
// exactly the identity when nothing moved. A point outside every plane uses the nearest one. Corners that coincide in the
// source (within `link`) are linked: they share one destination (the mean of theirs).
//
// Coordinates are any one unit (the transform op: mm, its link half a pixel of the working 200 dpi).

namespace genko::render::perspective {

using XY = std::array<double, 2>;
using Quad = std::array<XY, 4>;  // (corners clockwise from the top left)

struct Plane {
    Quad src{};
    Quad dst{};
};

enum class Straighten { Horizontal, Vertical, Auto };
std::optional<Straighten> straighten_from(std::string_view text);  // (horizontal, vertical, auto)

// The groups of linked corners: (plane, corner) pairs that coincide in the source.
std::vector<std::vector<std::pair<std::size_t, std::size_t>>> linked_corners(const std::vector<Plane>& planes, double link);
// Linked corners moved to one source and one destination place (their means).
void unify(std::vector<Plane>& planes, double link);
// The destination edges that are near level made level, the near upright ones upright (within 45°), linked corners
// moved together.
void straighten(std::vector<Plane>& planes, Straighten mode, double link);
// Every plane onto itself.
bool is_identity(const std::vector<Plane>& planes);

// A prepared perspective warp: the forward map (source → destination).
class Map {
public:
    // None for no plane or a degenerate quad.
    static std::optional<Map> make(const std::vector<Plane>& planes, double link);
    XY map(double x, double y) const;

private:
    struct PlaneMap {
        resample::Homography src, src_inv, dst;
        Quad quad{};
        std::array<std::optional<std::size_t>, 4> shared;
    };
    std::vector<PlaneMap> planes_;
    std::tuple<std::size_t, double, double> locate(double x, double y) const;
};

}  // namespace genko::render::perspective
