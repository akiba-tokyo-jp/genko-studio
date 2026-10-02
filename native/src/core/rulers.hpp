#pragma once

<<<<<<< HEAD
#include <functional>
#include <vector>

#include "core/geometry.hpp"
#include "core/json.hpp"

// Rulers a pen line snaps to (Python's genko/rulers.py): snap() and symmetry_copies(), the parts the drawing ops
// use. The rulers are the page's JSON as Python keeps it; a ruler that lacks what its kind needs fails as it does in
// Python (OpKeyError "not found: 'points'", …).

namespace genko::core {

// frame_contains(frame_id, x, y): whether the point is in the page's panel with that id (false when there is none).
using FrameContains = std::function<bool(const Json& frame_id, double x, double y)>;

// rulers.snap: the stroke snapped to the ruler that suits it best (the one whose line stays nearest to what was
// drawn); the points as they are when no ruler takes it. `only`: one ruler by id (when truthy); `layer_id`: the layer
// drawn on (nullptr: Python's None).
PenPoints ruler_snap(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains, const Json& only,
                     const Json* layer_id);

// rulers.symmetry_copies: the extra strokes the active symmetry rulers make from one stroke.
std::vector<PenPoints> symmetry_copies(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains,
                                       const Json* layer_id);

}  // namespace genko::core
=======
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/pynum.hpp"
#include "core/stroke_geom.hpp"

// Rulers (Python's genko/rulers.py): guides a pen line snaps to, kept on the page (Page::rulers, each a JSON object
// as Python keeps it; page mm). The snapping, the symmetry copies, the lines a ruler draws as and the perspective
// grid, with Python's numbers to the last bit (the same operations in the same order, Python 3.12's sum(), libm called
// as CPython calls it). No pictures: the screen (GUI) and the ops use these.
//
// Kinds: line, curve, parallel, concentric, radial, perspective, symmetry, guide, parallel_curve, multi_curve,
// radial_curve, rect, ellipse, polygon (see rulers.py for what each uses).
//
// A ruler or point that is not what Python expects raises what Python raises: core::PyValueError, core::PyTypeError,
// core::PyIndexError or core::OpKeyError.

namespace genko::core::rulers {

struct XY {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const XY&, const XY&) = default;
};

using Polyline = std::vector<XY>;

inline constexpr double kGuideReachMm = 3.0;
inline constexpr double kReachMm = 10.0;

// KINDS, in Python's order.
const std::vector<std::string>& kinds();

// validate(ruler): PyValueError with Python's message when the ruler cannot work.
void validate(const Json& ruler);

// _xy(p): (float(p[0]), float(p[1])).
XY xy_of(const Json& point);

// smooth_curve(points, per_mm): a Catmull-Rom curve through the points, about one point per mm (the points
// themselves when there are fewer than three).
Polyline smooth_curve(const Json& points, double per_mm = 1.0);

// shape_outline(ruler): a rect / ellipse ruler's outline in the box of its two points (turned by its angle), or a
// polygon ruler's corners; closed (the first point again at the end).
Polyline shape_outline(const Json& ruler);

// The straight directions a parallel, radial or perspective ruler offers at a point.
std::vector<XY> directions(const Json& ruler, XY at);

// The eye level of a perspective ruler: (a point, a unit direction), or nothing without points.
std::optional<std::pair<XY, XY>> horizon(const Json& ruler);

// Python's frame_contains(frame_id, x, y): whether a point is in a panel (the ruler's frame_id as it is kept).
using FrameContains = std::function<bool(const Json& frame_id, double x, double y)>;

// snap(points, rulers, frame_contains, only, layer_id): the stroke snapped to the ruler that suits it best (whose line
// stays nearest to what was drawn), its points rounded to 4 places with the pressures spread along them; the points
// unchanged when no ruler takes the stroke. `rulers` is a JSON list (Page::rulers).
PenPoints snap(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains = {},
               const std::optional<std::string>& only = std::nullopt,
               const std::optional<std::string>& layer_id = std::nullopt);

// symmetry_copies(points, rulers, frame_contains, layer_id): the extra strokes the active symmetry rulers make.
std::vector<PenPoints> symmetry_copies(const PenPoints& points, const Json& rulers, const FrameContains& frame_contains = {},
                                       const std::optional<std::string>& layer_id = std::nullopt);

// A page size as Python passes it (page.spec's numbers: ints stay ints).
struct PageSize {
    Num width = 400.0;
    Num height = 500.0;
};

// outline(ruler, page_size): the ruler as lines to draw (定規ペン, showing it); nothing for kinds that are only
// directions. A guide's ends are the page's edges as Python has them (an int width stays an int).
struct OutlinePoint {
    Num x;
    Num y;
};
std::vector<std::vector<OutlinePoint>> outline(const Json& ruler, const PageSize& page_size = {});

// perspective_grid(ruler, page_size, lines): パースのグリッド, as segments.
std::vector<std::array<XY, 2>> perspective_grid(const Json& ruler, const PageSize& page_size,
                                                std::optional<std::int64_t> lines = std::nullopt);

// snap_to_grid(point, spacing_mm, origin).
XY snap_to_grid(XY point, double spacing_mm, XY origin = {});

}  // namespace genko::core::rulers
>>>>>>> native/m3-tones
