#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// PC3 (SPEC.md COLOR-07, docs/cpp-migration/schema-v4.md「パス・ベクトル」): the vector data of a page — paths (the Pen
// tool's, saved in the Paths panel, the work path, the clipping path), the shape parts a pen layer holds (決定 7:
// pen-shapes@1), a layer's vector mask, a layer's link group and position lock (transform@1) — their types, their JSON
// and their checks. PhotoCraft v0.2.0's doc/src/vector.rs in mm (y down, page coordinates; native/third_party/photocraft/
// NOTICE.md). The coverage of a path is the drawing build's (render/path_raster).
//
// A subpath is a list of knots; segment i runs from knots[i].anchor via knots[i].out and knots[i + 1].in to
// knots[i + 1].anchor (a closed subpath adds the segment back to the first knot). A knot whose handles are its anchor is a
// corner. Every subpath is filled on its own (with the path's fill rule) and folded, in order, into the area with its
// op; the first subpath always combines (Photoshop's rule); `inverted` inverts the final area.

namespace genko::core {
struct Document;
}  // namespace genko::core

namespace genko::core::vector {

inline constexpr std::string_view kVectorFeature = "vector@1";
inline constexpr std::string_view kPenShapesFeature = "pen-shapes@1";
inline constexpr std::string_view kTransformFeature = "transform@1";
inline constexpr std::string_view kPageSpecFeature = "page-spec@1";
inline constexpr std::string_view kShapesSuffix = ".shapes.json";
inline constexpr std::size_t kKnotsMax = 100000;     // (one path's knots, all subpaths)
inline constexpr std::size_t kPathNameMax = 64;      // (characters)
inline constexpr std::size_t kPathsMax = 998;        // (a page's saved paths: PSD's 2000..2997)
inline constexpr std::size_t kShapePartsMax = 10000;  // (one pen layer's)
inline constexpr double kCoordinateMax = 1.0e5;      // (mm, either way: what a path's points may reach)
inline constexpr double kStrokeWidthMax = 200.0;     // (mm)
inline constexpr double kFeatherMax = 100.0;         // (mm: a vector mask's feather)

struct XY {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const XY&, const XY&) = default;
};

struct Knot {
    XY anchor;
    XY in;   // the control point of the segment arriving here
    XY out;  // the control point of the segment leaving here
    bool smooth = false;

    static Knot corner(double x, double y) { return Knot{XY{x, y}, XY{x, y}, XY{x, y}, false}; }
    bool is_corner() const { return in == anchor && out == anchor; }
    friend bool operator==(const Knot&, const Knot&) = default;
};

enum class PathOp { Combine, Subtract, Intersect, Exclude };
enum class FillRule { NonZero, EvenOdd };

struct Subpath {
    bool closed = true;
    PathOp op = PathOp::Combine;
    std::vector<Knot> knots;

    // Its cubic segments [p0, c1, c2, p1].
    std::vector<std::array<XY, 4>> segments() const;
    friend bool operator==(const Subpath&, const Subpath&) = default;
};

// An affine map of the page (mm): x' = a·x + c·y + e, y' = b·x + d·y + f (the selection's matrices' order).
using Affine = std::array<double, 6>;

struct Path {
    std::vector<Subpath> subpaths;
    FillRule fill_rule = FillRule::NonZero;
    bool inverted = false;

    // No knot in any subpath.
    bool empty() const;
    std::size_t knot_count() const;
    // The op subpath i is folded in with: the first always combines.
    PathOp effective_op(std::size_t i) const { return i == 0 ? PathOp::Combine : subpaths[i].op; }
    // The box of every anchor and handle [x0, y0, x1, y1] (a superset of the curve); none for no knot.
    std::optional<std::array<double, 4>> control_bounds() const;
    // Every anchor and handle through m (exact for an affine map).
    Path transformed(const Affine& m) const;
    friend bool operator==(const Path&, const Path&) = default;
};

enum class StrokeAlign { Inside, Center, Outside };
enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

// A shape part's stroke (Photoshop's shape Stroke options, not a layer style): `fill` is a fill as below; the dashes and
// their offset are multiples of the width.
struct ShapeStroke {
    double width_mm = 0.35;
    Json fill = nullptr;
    double opacity = 1.0;
    StrokeAlign align = StrokeAlign::Center;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    double miter_limit = 100.0;
    std::vector<double> dashes;
    double dash_offset = 0.0;
    friend bool operator==(const ShapeStroke&, const ShapeStroke&) = default;
};

// The parametric ("live") shape a part's path was made from (PhotoCraft's LiveShape; render/path_raster's live_path makes
// the path again): a rectangle with its corner radii (tl, tr, br, bl), an ellipse, a polygon or star in a box (the
// polygon turned by `angle`, degrees clockwise, as Genko's 図形 turns it), a line drawn as a bar `weight_mm` thick.
struct LiveShape {
    enum class Kind { Rect, Ellipse, Polygon, Line };
    Kind kind = Kind::Rect;
    std::array<double, 4> rect{0, 0, 0, 0};  // x, y, w, h
    std::array<double, 4> radii{0, 0, 0, 0};
    int sides = 5;
    double star_ratio = 1.0;
    double angle = 0.0;
    XY from;
    XY to;
    double weight_mm = 0.35;
    friend bool operator==(const LiveShape&, const LiveShape&) = default;
};

// A shape part of a pen layer (決定 7, pen-shapes@1): a path filled and stroked, drawn after the layer's patches and
// before its lines (`over`: after its lines). A fill is null (none), {"rgb": [r, g, b] | null} (null: the layer's ink),
// {"gradient": {…a fill layer's gradient…}}; a pattern ({"pattern"}) is PC4's.
struct ShapePart {
    std::string id;
    std::optional<std::string> name;
    Path path;
    Json fill = nullptr;
    std::optional<ShapeStroke> stroke;
    std::optional<LiveShape> live;
    double opacity = 1.0;
    bool over = false;
    friend bool operator==(const ShapePart&, const ShapePart&) = default;
};

// A pen layer's shape parts. Never changed once made (an edit makes a new list), shared between copies of the layer as
// its strokes are. `blob_ref`: the .shapes.json asset it was read from ("" for a list made in memory).
struct ShapeList {
    std::vector<ShapePart> items;
    std::string blob_ref;
};
using ShapeListPtr = std::shared_ptr<const ShapeList>;
// The shared empty list (a layer's shapes are never null).
const ShapeListPtr& empty_shapes();
ShapeListPtr make_shapes(std::vector<ShapePart> items, std::string blob_ref = {});

// A layer's vector mask: its path's coverage (density applied: 1 − density·(1 − coverage), feathered by a Gaussian of σ =
// feather_mm / 2) multiplies what the layer shows. A path of no subpath reveals all (hides all when inverted).
struct VectorMask {
    Path path;
    bool enabled = true;
    bool linked = true;
    double density = 1.0;
    double feather_mm = 0.0;
    friend bool operator==(const VectorMask&, const VectorMask&) = default;
};

// A page's saved path (the Paths panel's order) and its clipping path (PSD resource 2999).
struct NamedPath {
    std::string name;
    Path path;
    friend bool operator==(const NamedPath&, const NamedPath&) = default;
};
struct ClippingPath {
    std::string name;
    double flatness = 0.0;
    friend bool operator==(const ClippingPath&, const ClippingPath&) = default;
};

// --- JSON ----------------------------------------------------------------------------------------------------------
// Each written in its normal form (a corner knot as [x, y]; keys in a fixed order, a default left out; numbers to 4
// decimals: 0.1 µm) and read back checked: core::Error("value") says what is wrong (a number not finite or past
// kCoordinateMax, an unknown key, more knots than kKnotsMax…).

std::string_view to_string(PathOp op);
std::string_view to_string(FillRule rule);
std::optional<PathOp> path_op_from(std::string_view text);

// {"subpaths": [{"closed", "op", "knots": [[x, y] | {"anchor", "in", "out", "smooth"}]}], "fill_rule", "inverted"}
Json to_json(const Path& path);
Path path_from_json(const Json& value);
Json to_json(const ShapeStroke& stroke);
ShapeStroke stroke_from_json(const Json& value);
Json to_json(const LiveShape& live);
LiveShape live_from_json(const Json& value);
Json to_json(const ShapePart& part);
ShapePart part_from_json(const Json& value);
Json to_json(const VectorMask& mask);
VectorMask vector_mask_from_json(const Json& value);
Json to_json(const std::vector<NamedPath>& paths);
std::vector<NamedPath> paths_from_json(const Json& value);
Json to_json(const ClippingPath& clip);
ClippingPath clipping_from_json(const Json& value);
// A fill as a shape part keeps it, checked (core::Error("value")).
void check_fill(const Json& fill, std::string_view what);

// The bytes of a .shapes.json asset: the canonical JSON of the parts (core::dump_canonical).
std::string shapes_blob(const ShapeList& shapes);
// The parts of a .shapes.json asset, checked (core::Error("value"); "json" for bytes that are not JSON).
std::vector<ShapePart> shapes_from_blob(std::string_view bytes);

// A saved path's name trimmed of the spaces around it: core::Error("value") for one empty or longer than kPathNameMax.
std::string path_name(std::string_view name);

// --- the book --------------------------------------------------------------------------------------------------------

// Does the book need vector@1 (a page's paths, work path or clipping path; a layer's vector mask), pen-shapes@1 (a pen
// layer with shape parts), transform@1 (a layer's link or position lock, a line's native_warp), page-spec@1 (a page of
// its own paper)?
bool uses_vector(const Document& doc);
bool uses_pen_shapes(const Document& doc);
bool uses_transform(const Document& doc);
bool uses_page_spec(const Document& doc);
// The book's vector data as it must be (core::Error("value") says what is not): saved paths' names one each on a page and
// at most kPathsMax, a clipping path naming a saved path, shape parts only on pen layers with ids one each, a page's own
// paper well formed. Pages not read yet are not looked at.
void validate_vector(const Document& doc);
// PC3 C-c (transform@1): a line of dialogue's warp of PhotoCraft's styles (Warp Text, the style's native_warp: {"style":
// one of the fifteen, "bend", "h_distort", "v_distort": −100..100 %, "vertical": bool}) as it must be (core::Error("value")).
void check_native_warp(const Json& value);

}  // namespace genko::core::vector
