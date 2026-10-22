#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC4 E3 (patterns@1, SPEC.md COLOR-09「パターン」, schema-v4.md「パターン」): a pattern's picture made ready to draw and
// laid over the page — PhotoCraft v0.2.0 crates/compose/src/pattern.rs (Tile, Placement, render), MIT:
// native/third_party/photocraft/NOTICE.md. A pattern tiles the page from its phase (mm), scaled and turned about that
// point; one of its pixels is 1/dpi inch at 100 % (the pattern's own dpi: Genko's pages are in mm, their grids differ by
// the way they are drawn). Its samples are bilinear with wrap-around on premultiplied values, so a whole-pixel placement
// at 100 % on a grid of its dpi gives the tile exactly and a scaled one stays seamless.
//
// Its values come to the page as the page holds colours (design 1.3): on an RGB page an RGB pattern's values are the
// page's values (as a paint layer's pixels: sRGB, or the page's profile's), a grey one's its grey, a CMYK or Lab one's
// the colour its inks mean (render/ink rules); on a page of inks a pattern of the page's mode its values as they are, any
// other the colour it means by the page's rules (once, when it is made ready).

namespace genko::render {
struct ColorPicture;
struct InkPicture;
class RgbSpace;
}  // namespace genko::render

namespace genko::render::patterns {

// The keys of a pattern fill (core::pattern_fill_spec's normal form) as numbers.
struct Spot {
    double scale = 1.0;  // (the % / 100)
    double angle = 0.0;  // (degrees, counter-clockwise)
    double phase_x_mm = 0.0;
    double phase_y_mm = 0.0;
    bool link = true;
};
Spot spot_of(const core::Json& spec);

// A pattern's picture as straight floats: its mode's colours then the alpha, each pixel, rows from the top. RGB: the
// values (8 bits /255, 16 bits /65535, 32 bits as they are: HDR kept); grey: its lightness; CMYK and Lab: GKIR's
// normalized values (the ink canvas's: CMYK ink 0..1, Lab L/100 and (a, b + 128)/255).
struct Tile {
    std::string mode = "rgb";
    int depth = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    unsigned colours = 3;
    std::vector<float> values;
};
// The picture of the pattern decoded (core::Error("value") when it cannot be: core::check_pattern_file's checks).
Tile decode(const core::Pattern& pattern, std::string_view bytes);
// A tile as a pattern's picture again: an 8-bit RGB or grey one a PNG (RGBA, LA), 16 and 32 bits RGB GKCR, the rest GKIR
// (core::pattern_suffix's kinds).
std::string encode(const Tile& tile);

// What a tile is made into to be drawn.
struct Target {
    enum class Kind { Rgb, Inks };
    Kind kind = Kind::Rgb;
    // Rgb: the page's values in its space (null: sRGB — a page without a profile); Inks: the page's colour mode.
    std::shared_ptr<const RgbSpace> space;
    std::optional<core::PageColor> color;
    std::string space_key;  // (the space's or colour mode's identity)
    static Target rgb(const core::Page& page);
    static Target srgb();
    static Target inks(const core::PageColor& color);
    unsigned colours() const;
    std::string key() const;  // (what the made tiles are kept by)
};

// A pattern made ready to lay over the page at a resolution: its tile in the target's values and where it lies.
class Laid {
public:
    // `dpi`: the grid the page is drawn on (pixel (x, y) covers [x, x + 1) × [y, y + 1) of the page at dpi).
    Laid(std::shared_ptr<const std::vector<float>> premultiplied, std::uint32_t width, std::uint32_t height, unsigned colours, const Spot& spot,
         double pattern_dpi, double dpi);
    unsigned colours() const { return colours_; }
    // The straight values of the pixel (x, y) of the grid: colours() values then the alpha.
    void at(int x, int y, float* out) const;
    // (tile coordinates of a grid point)
    std::pair<double, double> map(double x, double y) const;

private:
    std::shared_ptr<const std::vector<float>> px_;
    std::uint32_t w_, h_;
    unsigned colours_;
    double ox_, oy_, c_, s_, k_;
};

// The pattern of a fill ({"id", …}: core::pattern_fill_spec's form) of the book made ready for `target` at `dpi`;
// none when the book does not hold it or its picture (a book read so is read-only: nothing is drawn). Made tiles are
// kept (by the picture's ref and the target) for the next drawing, within a memory budget.
std::optional<Laid> laid(const core::Document* book, const core::Json& fill_pattern, const Target& target, double dpi);
// The same for a pattern and its picture given (the library's, a pattern not in a book yet).
Laid laid(const core::Pattern& pattern, const core::Bytes& bytes, const core::Json& fill_pattern, const Target& target, double dpi);

// Its pixels over `area` of the grid: "RGBA" (an sRGB target's values, rounded), a precise picture (linear light of the
// target's space), the page's inks (ink canvas values).
Image rgba8(const Laid& laid, const Box& area);
ColorPicture linear(const Laid& laid, const Box& area, const RgbSpace& space);
InkPicture inks(const Laid& laid, const Box& area);

// The made tiles kept (tests).
std::size_t kept();
void forget();

// A pattern made from the page as it shows (image_ops::page_pixels: its own values — 8-bit RGB, 16- and 32-bit RGB, its
// inks; a multichannel page's colours as 8-bit RGB — on its working grid, whose dpi is the pattern's), cut to the box of
// `area` ({poly} or {mask}, core::op_area's; none: the whole page) with what lies outside the area clear: Edit › Define
// Pattern…, the library's new pattern (its id empty). core::Error("value") when the area covers none of the page or the
// pattern would be past 8192 pixels a side.
struct FromPage {
    core::Pattern pattern;
    core::Bytes bytes;
};
FromPage from_page(const core::Document& doc, const core::Page& page, const core::Json* area, const std::string& name);

// The pattern's look as a grey (its lightness over white: 0.299 R + 0.587 G + 0.114 B of the colour it means, laid over
// white by its alpha), 8 bits, one byte a pixel — a brush's paper or a dab brush's texture made from it (brush.texturePattern).
std::string grey(const core::Pattern& pattern, std::string_view bytes);
// A small picture of the pattern for a list (sRGB "RGBA", `side` pixels square at most, the tile repeated to fill it).
Image swatch(const core::Pattern& pattern, std::string_view bytes, int side);

}  // namespace genko::render::patterns
