#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "core/model.hpp"

// color-mode@1, the second step (SPEC.md COLOR-02): the pixel ops of paint layers on CMYK and Lab pages, through the
// pictures the screen shows of them. Before such an op each ink layer of the pages it may touch lends it its picture
// (an RGB paint layer's PNG: 8-bit RGBA, as ink::to_rgba8 shows it); after it, what the op left on those pages goes back
// into inks:
//   - a pixel whose colour the op did not change keeps its inks, and its alpha when the op did not change that either
//     (16 bits as they were);
//   - an element's colour the rules keep (Genko's blacks, white, the table's) takes the rules' inks;
//   - a colour the page showed takes the inks it was shown for (what moved or was copied keeps its inks): its layers'
//     colours, its table's, Genko's black and the greys on K alone (Lab: on L), no ink; then a grey by the rules; then
//     a colour within 2 a channel of one shown (what straight alpha's rounding leaves where only the alpha changed);
//   - pictures an op left on a paint layer as fills (what transform_area moves, what paste puts down) are drawn into
//     its pixels first, as on a precise layer (render/color_edit);
//   - any other colour (mixed, smudged, a new line's edge) takes the page's inks by its rules (the table, Genko's blacks
//     and white, greys on K alone, the profile).
// A precise RGB raster an op left there (put_color_raster) goes into inks through the profile.

namespace genko::render::ink_edit {

// スポイト on a CMYK or Lab page: the inks of the page at a point (mm) as it is drawn there (in proof), or none off the
// page; the inks of a paint layer's own pixel there, when its ink pixels show (alpha over 20 of 255).
std::optional<std::vector<double>> inks_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, int dpi = 100);
std::optional<std::vector<double>> layer_inks_at(const core::Page& page, const core::Layer& layer, double x_mm, double y_mm);

// The ops that go this way (the rest of the pixel ops: filters wait for their own step).
bool lends(std::string_view op);

class Lending {
public:
    // Lends the pictures of the ink layers of these pages (indices into doc.pages; RGB pages and pages not read yet are
    // passed over).
    Lending(core::Document& doc, const std::vector<std::size_t>& pages);
    // Takes them back: every paint layer of those pages (found again by their ids) in inks.
    void take_back(core::Document& doc) const;

    struct Loan {
        std::string layer;             // the layer's id
        core::Bytes ink_raster;        // what it held
        core::Bytes lent;              // the PNG it was lent
        core::InkPixels pixels;        // its pixels
        std::string shown;             // the picture lent (RGBA bytes)
    };
    struct Page {
        std::string id;
        core::PageColor color;
        std::vector<Loan> loans;
        std::vector<std::pair<std::string, std::size_t>> patches;  // each layer's fills and pictures before the op
    };

private:
    std::vector<Page> pages_;
};

}  // namespace genko::render::ink_edit
