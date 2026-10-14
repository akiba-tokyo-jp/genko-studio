#pragma once

#include <array>
#include <memory>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/geometry.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/selection.hpp"
#include "render/warp.hpp"

// color-mode@1, the second step (SPEC.md COLOR-02): the pixel ops of paint layers on CMYK and Lab pages.
//
// What only takes, moves or clears pixels works the inks themselves, as render/color_edit works precise pixels: the
// eraser, an area deleted or moved (warped), a copied piece of inks laid down, the paper changed. What a tool does not
// reach keeps its samples (16 bits as they were); what it moves keeps its inks (a pixel carried whole, its samples; one
// resampled, the inks mixed, premultiplied by the alpha). A merge (merge_down, merge_layers, merge_visible, a layer
// converted to paint) composites the layers it takes in inks, as the page draws them (render::composite_ink_layers).
//
// What draws new colours or mixes them (smudge and liquify, the fills, shapes, 3D, rulers, the light table, precise RGB
// pixels) works the picture the screen shows of the layers it works on: those ink layers lend the op their picture (an
// RGB paint layer's PNG, 8-bit RGBA, as ink::to_rgba8 shows it); after it, what the op left goes back into inks:
//   - a pixel whose colour the op did not change keeps its inks, and its alpha when the op did not change that either;
//   - a colour the page's table holds takes its inks;
//   - a colour the layers lent showed takes the inks they showed it for (what moved keeps its inks);
//   - Genko's blacks and white take the rules' inks; a colour the rules show (their black, the greys on K alone —
//     Lab: on L —, no ink) their inks; another grey the rules';
//   - a colour within 2 a channel of one shown (what straight alpha's rounding leaves where only the alpha changed)
//     that colour's inks;
//   - any other colour (mixed, smudged, a new line's edge) the page's profile.
// A precise RGB raster an op left there (put_color_raster) goes into inks through the profile.

namespace genko::render::ink_edit {

// スポイト on a CMYK or Lab page: the inks of the page at a point (mm) as it is drawn there (in proof), or none off the
// page; the inks of a paint layer's own pixel there, when its ink pixels show (alpha over 20 of 255).
std::optional<std::vector<double>> inks_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, int dpi = 100);
std::optional<std::vector<double>> layer_inks_at(const core::Page& page, const core::Layer& layer, double x_mm, double y_mm);

// --- the inks themselves -------------------------------------------------------------------------------------------
// Each returns the layer's new GKIR bytes (its size, space and depth as they were). The raster lies stretched over the
// page, as a precise one does (render/color_edit).

// erase / erase_raster: the alpha loses what the 8-bit eraser's mask takes (a hard eraser clears the pixel).
std::string erase(const core::Page& page, const core::Layer& layer, const core::PenPoints& points, double width_mm,
                  std::string_view texture, const std::string& seed);
// delete_area: the area's pixels cleared (its soft edge partly).
std::string delete_area(const core::Page& page, const core::Layer& layer, const core::Json& area);
// transform_area: the area's pixels lifted, resampled through m (mm) and laid over what stays; with a warp, the area's
// box through `go` in pieces of two triangles.
std::string move_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const selection::Matrix& m, Resample resample);
std::string warp_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const warp::Go& go, Resample resample);

// A piece of inks copied from a CMYK or Lab page (its pixels and the box it covers, mm), and what lifts it: the pixels
// of a paint layer an area covers (alpha × the area's coverage), cut to what shows; none when nothing shows there. The
// area as an op gets it ({poly} or {mask: {box, png}}: the GUI's selection, an op's area after the command bus).
struct Piece {
    std::array<double, 4> box_mm{};  // x, y, w, h
    core::InkPixels pixels;
};
std::optional<Piece> lift_piece(const core::Page& page, const core::Layer& layer, const core::Json& area);
// The piece as a picture to copy (selection::Items' patch): its picture as the screen shows it, its box, and its inks
// ("gkir": its GKIR bytes in base64 — a C++ build's own key, never saved: paste draws them or drops the key).
core::Patch piece_patch(const Piece& piece, const core::PageColor& color);
// A patch's own inks, when it carries them for a page of this space and depth (anything else: none).
std::optional<core::InkPixels> patch_inks(const core::Patch& patch, const core::PageColor& color);
// The piece laid over the layer's inks at the box (mm), resampled to the raster (a whole pixel each when the box spans
// exactly the piece's pixels). A layer with no inks yet gets a clear raster at the working 200 dpi.
std::string lay_piece(const core::Page& page, const core::Layer& layer, const core::InkPixels& piece, const std::array<double, 4>& box_mm);

// set_page_spec: the inks of a paint layer on the new paper (render/bookpages' relayout_raster for inks): the old
// frame onto the new, at the working 200 dpi, bilinear.
std::string relayout_inks(const std::string& gkir, const core::PageSpec& spec, const core::Rect& old_frame, const core::Rect& new_frame);

// The pictures an op left on a paint layer of a CMYK or Lab page (a pasted picture, a material, an area moved) drawn
// into its inks there and then, as a precise layer takes them (render/color_edit::bake_marks): their colours by the
// page's rules, laid over the inks; the layer keeps none.
void bake_marks(core::Document& doc);

// --- through the picture the screen shows ---------------------------------------------------------------------------

// The ops that go this way.
bool lends(std::string_view op);

class Lending {
public:
    // Lends the pictures of the ink layers op (by its name) works on, on the CMYK or Lab page at `page` (an index into
    // doc.pages): its target layer, or every ink layer of the page for an op that composites several. A merge's layers
    // are kept as they were, to be composited again in inks.
    Lending(core::Document& doc, std::size_t page, std::string_view op, const core::Json& args);
    // Takes them back: every paint layer of that page (found again by their ids) in inks; the layer a merge left, its
    // layers composited in inks.
    void take_back(core::Document& doc) const;

    struct Loan {
        std::string layer;       // the layer's id
        core::Bytes ink_raster;  // what it held
        core::Bytes lent;        // the PNG it was lent
        core::InkPixels pixels;  // its pixels
        std::string shown;       // the picture lent (RGBA bytes)
    };

private:
    // A merge (merge_down, merge_layers, merge_visible, convert_layer to paint): the layers it composites, as they were
    // before the op, made again in inks (render::composite_ink_layers) for the layer it leaves — that layer's id, or
    // none for a layer the op makes (the one that was not there before).
    struct Merge {
        std::optional<std::string> target;
        std::vector<core::Layer> layers;
        bool paper = false;
    };
    std::string page_id_;
    core::PageColor color_;
    std::vector<Loan> loans_;
    bool had_color_raster_feature_ = false;
    std::optional<Merge> merge_;
    std::shared_ptr<const core::Page> before_;  // (the page as it was, for a merge)
    std::vector<std::string> ids_before_;
};

}  // namespace genko::render::ink_edit
