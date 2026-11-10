#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Tone layers (Python's genko/tones.py and the AM screen of genko/screentone.py): where the tone is (the mask: the
// layer's region, its patches and its pen lines — lines paint tone, scrape lines take it away; every panel when it
// has none of these) and how it looks (dots, lines, cross, noise, flat, and the pattern tones check, brick, wave,
// grid, hatch, star, sand and image), with gradients, the dot shape and the screen moved; and a layer's greys as a
// halftone (レイヤーのトーン化). The same pixels as the Python baseline, for the whole page or any box of it.

namespace genko::render::tones {

// PATTERNS, MOTIFS and screentone.DOT_SHAPES, in Python's order.
const std::vector<std::string>& patterns();
const std::vector<std::string>& motifs();
const std::vector<std::string>& dot_shapes();

// tones.validate(tone): core::PyValueError with Python's message for a tone that cannot be drawn.
void validate(const core::Json& tone);

// tones.settings(layer): how the layer's tone looks.
struct Settings {
    core::Json pattern = "dot";  // (a str normally; another value is drawn as Python draws it)
    core::Json gradient;         // null, or the gradient
    double lpi = 60.0;
    double density = 0.3;
    double angle = 45.0;
    std::optional<core::Json> scale_mm;
    std::optional<core::Json> tile_png;
    std::optional<core::Json> seed;
    std::optional<core::Json> dot_shape;
    std::optional<core::Json> offset_mm;
};
Settings settings(const core::Layer& layer);

// tones.shift_px(offset_mm, dpi): 網の位置のずれ in pixels.
std::pair<int, int> shift_px(const std::optional<core::Json>& offset_mm, int dpi);

// screentone.screen_vector(dpi, lpi, angle)
std::pair<int, int> screen_vector(double dpi, double lpi, double angle);

// screentone.threshold_tile(m, n, shape): each pixel's rank (0..255) by distance to its dot centre, (m²+n²) square
// ("L"; the last 32 are remembered). Refused (core::Error "value") beyond 4096 pixels a side: Python takes minutes and
// gigabytes for such a coarse screen.
std::shared_ptr<const Image> threshold_tile(int m, int n, std::string_view shape);

// tones._tile(data): the ink of an image tone's picture (L), or nothing when it is not a picture.
std::optional<Image> tile(std::string_view data);

// What a tone layer needs from the page drawing (page.cpp): render._paint_patch for one patch onto an RGBA picture
// of a box of the page.
using PatchPainter = std::function<void(Image& out, const Box& box, const core::Patch& patch)>;

struct Page {
    const core::Page* page = nullptr;
    int dpi = 0;
    Size size;
    PatchPainter paint_patch;
};

// tones.mask(layer, page, size, dpi): where the layer has tone over the whole page (L, 255 = tone): its region, its
// patches and its pen lines (scrape lines take tone away), every panel when it has none of these.
Image mask(const core::Layer& layer, const Page& page);

}  // namespace genko::render::tones

namespace genko::render {

// tones.mask(layer, page, size, dpi) as Python calls it, with render._paint_patch for the patches (page.cpp): where a
// tone layer has tone over the whole page at `dpi` (L of `size`, 255 = tone).
Image tone_mask(const core::Page& page, const core::Layer& layer, Size size, int dpi);

}  // namespace genko::render

namespace genko::render::tones {

// tones.draw_layer(image, layer, page, dpi, print_mode) for the box `box` of the page: `image` (RGBA, the box's
// size) with the layer's tone over it. `panels` is render._clip_mask over the box (nothing when no panel cuts).
Image draw_layer(Image image, const core::Layer& layer, const Page& page, const Box& box, const Image* panels,
                 bool print_mode);

// tones.screened(raster, spec, dpi): a layer's picture (`raster`, the box `box` of the page; RGBA) as a halftone in
// black. A noise screen needs the rows above the box too: give it a box from the top of the page.
Image screened(const Image& raster, const core::Json& spec, int dpi, Size page_size, const Box& box);

// The threshold of render.to_bitonal's screen at each pixel of an image (`size`) for the pattern dot, line or cross
// (tones._screen): below `cover` is black.
std::vector<float> screen(std::string_view pattern, Size size, int dpi, double lpi, double angle,
                          std::pair<int, int> shift, std::string_view shape, const Box& box);

// tones.swatch(tone, size, dpi, print_mode): a small sample of a tone (RGB).
Image swatch(const core::Json& tone, Size size = Size{96, 96}, int dpi = 150, bool print_mode = true);

}  // namespace genko::render::tones
