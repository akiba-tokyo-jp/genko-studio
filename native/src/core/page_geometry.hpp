#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"

// PC3 節 C (C-0, SPEC.md COLOR-07, docs/cpp-migration/ARCHITECTURE.md「要素を写す」): everything on a page moved by one map
// of the page (mm, y down) — what set_page_spec's map (one basic frame onto another) does, made general for the transform
// and page size ops (transform_layers, resize_pages, set_canvas_size, crop_page, trim_page, reveal_all, rotate_pages): the
// lines and shape parts of a pen layer, patches, a tone layer's region, a fill's gradient, a placed picture, the saved and
// work paths, a vector mask, panels (their split tree turned or mirrored with them), lines of dialogue (their text kept
// upright, never mirrored), rulers and guides, effect lines, the 3D props and the stock of saved areas. The pixels (paint
// layers, masks, channels, a patch's picture under a turn, a mask area) are the drawing build's: render/resample, through
// PixelHooks.
//
// And the warps PhotoCraft's geom/src/warp.rs (MIT, native/third_party/photocraft/NOTICE.md) describes: Photoshop's
// fifteen warp styles (StyleWarp) and the custom mesh of bicubic Bézier patches (BezierMesh), forward maps of a box.

namespace genko::core::page_geometry {

using vector::Affine;
using vector::XY;

// Image › Image Rotation by a right angle, or a flip, of a paper w × h.
enum class Turn { FlipH, FlipV, Rotate180, Cw90, Ccw90 };
std::optional<Turn> turn_from(std::string_view text);  // (90cw, 90ccw, 180, flip_h, flip_v)
// The turn's map of the page (continuous: the paper's edges onto the turned paper's), x' = a·x + c·y + e, y' = b·x + d·y + f.
Affine turn_affine(Turn turn, double w, double h);

// A map of the page: the affine map and what it is (a turn's, when it is one).
struct Mapping {
    Affine m{1, 0, 0, 1, 0, 0};
    XY apply(const XY& p) const { return XY{m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]}; }
    XY linear(const XY& v) const { return XY{m[0] * v.x + m[2] * v.y, m[1] * v.x + m[3] * v.y}; }
    double det() const { return m[0] * m[3] - m[1] * m[2]; }
    // How a length scales (√|det|).
    double scale() const;
    // Mirrors (a negative determinant).
    bool reflects() const { return det() < 0; }
    // Keeps the axes (x stays x: no turn but by 180°) / swaps them (a quarter turn).
    bool keeps_axes() const;
    bool swaps_axes() const;
    // The angle the map turns things by (degrees, clockwise on the page; a mirror's after its flip of x).
    double angle_deg() const;
    // A move and a uniform scale (no turn, no mirror, no shear).
    bool is_similarity_upright() const;
};
Mapping mapping(const Affine& m);
Mapping turn_mapping(Turn turn, double w, double h);
// Maps applied one after the other (first, then second).
Mapping then(const Mapping& first, const Mapping& second);

// --- the elements --------------------------------------------------------------------------------------------------

// A pen line through the map: its points, its width × scale, its pen's turn (アートペン) plus the map's (a mirror's
// turned the other way). Its id is kept.
Stroke map_stroke(const Stroke& stroke, const Mapping& m);
// Points through the map.
void map_points(std::vector<Point>& points, const Mapping& m);
// A box [x, y, w, h] as the box of its corners through the map.
std::array<double, 4> map_box(const std::array<double, 4>& box, const Mapping& m);
// A panel and the panels under it: its box (the box of its corners), its corners when it has them, its split (the cut,
// the axis — swapped by a quarter turn — and its children in order along it, mirrored by a flip), its bows (each kept on
// its edge), its corners' rounding, its border width and its regions × scale. A map that is no turn by a right angle
// makes it a panel of free corners (its split tree cannot be kept: its children become free).
void map_frame(Frame& frame, const Mapping& m);
// A line of dialogue: its balloon's middle moved and its size × scale (w and h swapped by a quarter turn), its tails,
// its drawn outline; its text kept upright (never turned, never mirrored), its letters and its balloon's lines × scale
// (style's size_mm, outline_mm, border_mm, text_dx_mm, text_dy_mm).
void map_line(StoryLine& line, const Mapping& m);
// A ruler (Page::rulers' JSON): its points, points2 and centre; a guide moved along (a quarter turn makes a horizontal
// guide vertical); a map that is no turn by a right angle makes a guide a line ruler through the same place.
void map_ruler(Json& ruler, const Mapping& m);
// An effect line (集中線…): its centre, its inner ellipse × scale, its points.
void map_effect(Json& effect, const Mapping& m);
// A 3D prop: its place moved and its size × scale. OpError for a map that turns or mirrors or scales unevenly (the
// camera cannot show it: render it to pixels first).
void map_prim(Json& prim, const Mapping& m);
// An area as the ops take one: a poly's corners, a mask's box (only when the map keeps each axis the way it points: a
// move and a scale — otherwise, a half turn too, the mask's pixels move: PixelHooks::area); the richer kinds' parts in turn
// (union, saved …).
void map_area(Json& area, const Mapping& m);
// A path, a vector mask, a shape part (its stroke's width × scale; a live shape kept when the map only moves and scales
// evenly, otherwise dropped: the path is the shape then).
vector::Path map_path(const vector::Path& path, const Mapping& m);
void map_shape_part(vector::ShapePart& part, const Mapping& m);
// A fill's gradient ({"gradient": {"from", "to", …}}): its ends moved.
void map_fill(Json& fill, const Mapping& m);

// The pixels the drawing build moves (null: the op is refused with not_yet_ported naming them).
struct PixelHooks {
    // A layer's own pixels (raster_png, color_raster, ink_raster), its mask (a linked one, or every one when `all`), its
    // patches' pictures when the map turns or mirrors them, a smart object's filter mask (PC3 C-e).
    std::function<void(Layer& layer, const Mapping& m, const Page& before, const Page& after)> layer;
    std::function<void(Channel& channel, const Mapping& m, const Page& before, const Page& after)> channel;
    // A saved mask area's pixels when the map is more than a move and an even scale.
    std::function<void(Json& area, const Mapping& m, const Page& before, const Page& after)> area;
};
// Everything on the page through the map (the lines of dialogue of the book on it too), `after` its paper when the map is
// done (the same page with its new spec). The pixels through the hooks. A smart object's matrix after the map
// (smart::transform_layer); a placed picture only moved and scaled (a turn or a mirror: made a smart object first); a
// tone's screen and its linear gradient's way, and a layer's トーン化 screen, turned and mirrored with the page.
void map_page(Document& doc, std::size_t page, const Mapping& m, const PixelHooks& hooks);

// --- warps (geom/src/warp.rs) ----------------------------------------------------------------------------------------

enum class WarpStyle { None, Custom, Arc, ArcLower, ArcUpper, Arch, Bulge, ShellLower, ShellUpper, Flag, Wave, Fish, Rise, Fisheye, Inflate,
                       Squeeze, Twist };
std::optional<WarpStyle> warp_style_from(std::string_view text);  // (none, custom, arc, arcLower, …)
std::string_view to_string(WarpStyle style);
bool is_preset(WarpStyle style);

// A preset style made for one box (bend and distortions −100..100 %).
class StyleWarp {
public:
    static std::optional<StyleWarp> make(WarpStyle style, double bend, double h_distort, double v_distort, bool vertical,
                                         const std::array<double, 4>& box);
    XY apply(double x, double y) const;
    // The longest segment (box units) worth sending through apply unsplit.
    double max_segment() const;

private:
    std::pair<double, double> bend(double u, double v, double aspect) const;
    WarpStyle style_ = WarpStyle::None;
    double b_ = 0, hd_ = 0, vd_ = 0;
    bool vertical_ = false;
    XY c_, h_;
};

// A grid of bicubic Bézier patches over a box's unit square: `us`/`vs` the patch edges (0..1, from 0 to 1, increasing),
// `points` the (3·(|us|−1)+1) × (3·(|vs|−1)+1) control points, row-major, in page coordinates.
struct BezierMesh {
    std::vector<double> us;
    std::vector<double> vs;
    std::vector<XY> points;

    static BezierMesh identity(const std::array<double, 4>& box, int cols, int rows);
    // The mesh fitted to f (unit square → page) at its patches' thirds: exact for a bicubic map (an affine one too).
    static BezierMesh fit(const std::function<XY(double, double)>& f, std::vector<double> us, std::vector<double> vs);
    std::size_t nx() const { return 3 * (us.size() - 1) + 1; }
    std::size_t ny() const { return 3 * (vs.size() - 1) + 1; }
    bool valid() const;
    XY eval(double s, double t) const;
    // Split the patch column (row) holding s (t) there: the surface the same. False on or too near a patch edge.
    bool split_u(double s);
    bool split_v(double t);
    // Remove an inner patch edge k, merging its two patches (exact after an unedited split).
    bool remove_split_u(std::size_t k);
    bool remove_split_v(std::size_t k);
    // The (s, t) whose point is nearest p (a coarse search, then Newton steps).
    std::pair<double, double> param_at(const XY& p) const;

private:
    void transpose();
};

// A whole warp: a style with its bend and distortions, or a custom mesh, over its box.
struct Warp {
    WarpStyle style = WarpStyle::None;
    double bend = 0, h_distort = 0, v_distort = 0;
    bool vertical = false;
    std::array<double, 4> box{0, 0, 0, 0};  // (x0, y0, x1, y1)
    std::optional<BezierMesh> mesh;

    bool is_identity() const;
    XY map(double x, double y) const;
    BezierMesh to_mesh(int cols, int rows) const;
};

}  // namespace genko::core::page_geometry
