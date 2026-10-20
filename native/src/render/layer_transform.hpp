#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/page_geometry.hpp"
#include "render/puppet.hpp"
#include "render/resample.hpp"

// PC3 C-c (SPEC.md COLOR-07, docs/cpp-migration/ARCHITECTURE.md「層の変形」): layers moved through one map of the page —
// Edit › Free Transform and Edit › Transform's commands, Warp, Perspective Warp, Puppet Warp, and the moves of the Move
// tool (C-d) — each kind of what a layer holds as its kind holds it:
//
// * its pen lines and shape parts by their points (an affine map exactly: core/page_geometry; any other map the line
//   given points every 1.5 mm first, render/warp's warp_stroke, a shape part's segments cut 1.5 mm long first and its
//   anchors and handles moved);
// * its own pixels (8-bit, precise, inks: on their own grid) and its patches' pictures drawn again through the map
//   (render/resample: premultiplied; a map that takes the grid onto itself moves them exactly, their values the same;
//   a patch only moved or scaled keeps its picture, its box moved);
// * a fill's gradient, a tone's region (its screen turned with an affine map), a placed picture's box (moved and
//   scaled: turned or bent it is turned as a smart object);
// * a smart object's placing, never its contents (PC3 A5; render/smart draws it again from them): an affine map its
//   matrix, a quad's its corners (smart.perspective), a warp its warp (smart.warp, in its contents' mm: PhotoCraft's
//   warp_layer — the warp it holds replaced, as Photoshop edits a smart object's one warp); a perspective warp's planes and
//   a puppet warp's pins are kept as its smart filters (PhotoCraft's distort_cmds.rs:233-272), its masks left as they are
//   then; its filter mask moved with it otherwise;
// * its mask when it is linked (masks@1), its vector mask when it is linked (vector@1).
//
// The maps are the page's, in mm (y down).

namespace genko::render::layer_transform {

using XY = core::vector::XY;

// A map of the page: affine, projective (a quad's homography) or any other (a warp's, a perspective warp's, a puppet
// warp's), and the box (x0, y0, x1, y1 mm) of what it moves — where a mesh map is drawn from.
struct Map {
    enum class Kind { Affine, Projective, Mesh };
    Kind kind = Kind::Affine;
    core::vector::Affine affine{1, 0, 0, 1, 0, 0};
    resample::Homography projective;
    std::function<XY(double, double)> forward;      // (Mesh)
    std::shared_ptr<const puppet::Deformed> puppet;  // (Mesh: a puppet warp's triangles, in the page's 200 dpi pixels)
    // (PC3 A5: what made it, for a smart object it moves — a warp, kept as its warp even when it bends nothing; a perspective
    // warp's or a puppet warp's settings, kept as its smart filter: {"kind", "params"})
    std::shared_ptr<const core::page_geometry::Warp> warp;
    std::optional<core::Json> filter;
    std::array<double, 4> box{0, 0, 0, 0};
    XY apply(double x, double y) const;
    bool is_affine() const { return kind == Kind::Affine; }
};

Map affine_map(const core::vector::Affine& m, const std::array<double, 4>& box);

// The box (x0, y0, x1, y1 mm, on the working 200 dpi grid) of what the layers show, each drawn alone at 200 dpi (its
// panel clip and mask as drawn); none when they show nothing.
std::optional<std::array<double, 4>> content_box(const core::Document& doc, const core::Page& page, const std::vector<std::string>& ids);

// The layers an op names (`ids`, page order), with the folders' layers inside them and the layers linked to any of them
// (transform@1); OpError for a layer the page does not have, a locked one or one whose position is locked.
std::vector<std::string> layers_moved(const core::Page& page, const std::vector<std::string>& ids);

// One layer moved through the map: within `area` when given (a resolved area: what lies in it lifted and moved, the
// rest left); `parts`: only these shape parts of it (none: all it holds). OpError for what it cannot move that way.
void move_layer(core::Document& doc, std::size_t at, const std::string& id, const Map& m, const std::optional<core::Json>& area,
                resample::Interp interp, const std::vector<std::string>* parts = nullptr);
// The layer's mask and vector mask through the map when they are linked (masks@1, vector@1), as move_layer moves them
// with a layer moved whole (smart_transform's too).
void move_linked_masks(core::Page& page, core::Layer& layer, const Map& m);
// A pen line, a path and a shape part through the map (an affine map exactly; any other: a line given points every
// 1.5 mm first, a path's segments cut 1.5 mm long first and their anchors and handles moved — a shape part's live shape
// dropped then, its stroke's width scaled as the map scales near it).
core::Stroke stroke_through(const core::Stroke& stroke, const Map& m);
core::vector::Path bend_path(const core::vector::Path& path, const Map& m);
core::vector::ShapePart part_through(const core::vector::ShapePart& part, const Map& m);

// A line of dialogue moved with them: its balloon's middle, its tails and its drawn outline (an affine map as
// core/page_geometry's map_line: its text kept upright).
void move_line(core::StoryLine& line, const Map& m);

// --- the warps' forms ------------------------------------------------------------------------------------------------

// A warp as the op and a smart object hold it: {"style", "bend", "h_distort", "v_distort", "vertical", "bounds": [x0,
// y0, x1, y1]?, "mesh"?: {"us", "vs", "points"}, "grid"?: [cols, rows]} over `box` (x0, y0, x1, y1: its bounds when
// it has none). OpError for one that is not well formed.
core::page_geometry::Warp warp_from_json(const core::Json& value, const std::array<double, 4>& box);
// Its normal form ({"style", "bend", "h_distort", "v_distort", "vertical", "bounds", "mesh"?}; numbers to 4 decimals).
core::Json warp_to_json(const core::page_geometry::Warp& warp);
// Edit › Transform › Split Warp Crosswise, Horizontally, Vertically and Remove Warp Split on a warp being edited (a
// preset becomes custom): at `at` (mm; none: the middle of the patch); none when there is nothing to split or remove
// there.
enum class Split { Crosswise, Horizontal, Vertical, Remove };
std::optional<core::page_geometry::Warp> split_warp(const core::page_geometry::Warp& warp, const std::optional<XY>& at, Split how);

// A perspective warp's planes ({"src": [[x, y] × 4], "dst": …} in mm) and a puppet warp's pins ({"src", "dst",
// "rotate"?, "depth"?}), read and written (OpError for what is not well formed).
struct PerspectiveSpec {
    std::vector<std::array<XY, 4>> src, dst;
    std::optional<std::string> straighten;
};
PerspectiveSpec perspective_from_json(const core::Json& value);
core::Json perspective_to_json(const PerspectiveSpec& spec);
puppet::Warp puppet_from_json(const core::Json& value);
core::Json puppet_to_json(const puppet::Warp& warp);

// --- smart objects (PC3 A5) --------------------------------------------------------------------------------------------

// Where a smart object lays its contents' mm on the page's mm: its corners' homography, else its matrix (its warp before
// it is not in it).
resample::Homography smart_placing(const core::Layer& layer, const core::SmartContents& contents);
// A warp of the page as a smart object placed by `place` keeps it, in its contents' mm (PhotoCraft's warp_layer): a style
// over the box its bounds' corners come back to, a mesh's points brought back; none for one that bends nothing.
std::optional<core::page_geometry::Warp> warp_in_contents(const core::page_geometry::Warp& on_page, const resample::Homography& place);
// A smart object's warp as it shows on the page (what Warp starts from on it: a style kept where its placing only moves
// and scales it evenly, else its mesh through the placing); none when it has none or it cannot be read.
std::optional<core::page_geometry::Warp> smart_warp_on_page(const core::Document& doc, const core::Layer& layer);
// A smart object's filter mask (a page's 150 dpi grey PNG) moved through the map: its new PNG kept with the book's files
// (Document::smart_files); one not in the book is left as it is.
void move_filter_mask(core::Document& doc, const core::Page& page, core::Layer& layer, const Map& m);

// The op transform_layers ({page, ids, parts?, area?, box?, move | matrix | quad | turn | warp | perspective | puppet,
// interp?, with?}).
void transform_layers(core::OpContext& c);

}  // namespace genko::render::layer_transform
