#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC2c (局所の補正と画像の演算, SPEC.md COLOR-04): what the local corrections (shadows_highlights, replace_color,
// match_color, hdr_toning), the image calculations (apply_image, calculations), trapping (trap) and the fade
// (fade_layer) do to pixels — PhotoCraft v0.2.0's algo/tone.rs, algo/trap.rs, algo/selection.rs (color_range),
// algo/fxutil.rs (gauss_blur_n), color/convert.rs (Lab), color/blend.rs (the per-channel blends), engine/channel_cmds.rs
// (Blending) and engine/edit_menu_cmds.rs (fade_surface), MIT (third_party/photocraft/NOTICE.md) — and the way the ops
// read and write a paint layer's pixels of each kind. The ops are render/tone_ops.cpp, render/ops_calc.cpp,
// render/trap.cpp and render/ops_fade.cpp (registered by render/ops_adjust.cpp).

namespace genko::render::image_ops {

// Straight RGBA, 0..1 (a precise layer's HDR values beyond), as PhotoCraft's algorithms take it.
using Rgba = std::array<float, 4>;

// --- the algorithms (each as PhotoCraft's, in float as it computes them) ----------------------------------------------

// gauss_blur_n: a Gaussian of `sigma` as three box passes along the rows, then three along the columns (Kovesi's
// radii; sums in double, the edges repeated) over a buffer of `n` interleaved channels. Nothing below sigma 0.3.
void gauss_blur(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, float sigma);
// The three box radii of a Gaussian of `sigma` (gauss_box_radii).
std::array<std::size_t, 3> gauss_box_radii(float sigma);

// Image › Adjustments › Shadows/Highlights (percentages 0..100; radii in pixels of the buffer).
struct ShadowsHighlights {
    float shadow_amount = 35, shadow_tone = 50, shadow_radius = 30;
    float highlight_amount = 0, highlight_tone = 50, highlight_radius = 30;
    float color = 20, midtone = 0;
    float black_clip = 0.01f, white_clip = 0.01f;
};
void shadows_highlights(std::vector<Rgba>& px, std::size_t w, std::size_t h, const ShadowsHighlights& p);

// Color Range: each pixel's coverage, falling off with its largest channel difference from `color` (in levels) to
// fuzziness + 1; nothing where it is clear.
std::vector<float> color_range(const std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness);
// Replace Color: the colours Color Range takes shifted in hue (degrees), saturation and lightness (−100..100), each by
// its coverage. The coverage returned (what the window shows).
std::vector<float> replace_color(std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness, float hue, float saturation,
                                 float lightness);

// HSL as PhotoCraft's algo/other2.rs (hue 0..1).
std::array<float, 3> rgb_to_hsl(const std::array<float, 3>& c);
std::array<float, 3> hsl_to_rgb(const std::array<float, 3>& c);

// CIE L*a*b* (D50) of encoded sRGB and back (color/convert.rs: Bradford-adapted, the linear light clamped on the way
// back).
std::array<float, 3> srgb_to_lab(const std::array<float, 3>& rgb);
std::array<float, 3> lab_to_srgb(const std::array<float, 3>& lab);

// Lab mean and standard deviation of the pixels, each weighted by its alpha × `mask` (none: 1); none when nothing
// weighs.
struct LabStats {
    std::array<float, 3> mean{};
    std::array<float, 3> std{};
};
std::optional<LabStats> lab_stats(const std::vector<Rgba>& px, const std::vector<float>* mask = nullptr);
// Match Color (luminance and intensity 1..200, fade 0..100): each channel of the pixels moved from the target's
// statistics to the source's (none: the target's own — only neutralize, luminance and intensity act).
struct MatchColor {
    float luminance = 100, intensity = 100, fade = 0;
    bool neutralize = false;
};
void match_color(std::vector<Rgba>& px, const LabStats& target, const LabStats* source, const MatchColor& o);

// HDR Toning (Local Adaptation): radius in pixels, the toning curve on luminance in 0..1 (empty: none).
struct HdrToning {
    float radius = 30, strength = 0.5f, gamma = 1, exposure = 0, detail = 30;
    float shadow = 0, highlight = 0, vibrance = 0, saturation = 20;
    std::vector<std::pair<float, float>> curve;
};
void hdr_toning(std::vector<Rgba>& px, std::size_t w, std::size_t h, const HdrToning& p);

// Trap: each of the four inks (CMYK, ink amounts 0..1, of `ch` interleaved channels) spread by `width` pixels (a max
// filter) under the pixels that hold any ink — the paper is left as it is.
void trap(std::vector<float>& px, std::size_t w, std::size_t h, std::size_t ch, std::size_t width);

// One channel of Genko's blend `mode` (a separable one: not hue, saturation, color, luminosity), PhotoCraft's
// blend_channel: b the backdrop, s the source.
float blend_channel(std::string_view mode, float b, float s);
// A whole colour of any of Genko's 20 blends (the non-separable ones as W3C's, PhotoCraft's blend_rgb).
std::array<float, 3> blend_rgb(std::string_view mode, const std::array<float, 3>& b, const std::array<float, 3>& s);
// A straight colour laid over another by the blend at `opacity` (PhotoCraft's composite: the W3C general formula).
Rgba composite(std::string_view mode, const Rgba& backdrop, const Rgba& source, float opacity);
// Is it one of Genko's 20 blends?
bool is_blend(std::string_view mode);

// Apply Image / Calculations: a separable blend, or add and subtract with their scale (1..2) and offset (levels,
// −255..255). Read from the op's "blending" (default multiply), "scale" and "offset" (clamped; OpError for what is
// not a number, a blend it does not take).
struct Blending {
    enum class Kind { Mode, Add, Subtract };
    Kind kind = Kind::Mode;
    std::string mode = "multiply";
    float scale = 1, offset = 0;  // (offset in 0..1 units)
    // `top` (the source) blended onto `base` (the target), 0..1.
    float apply(float base, float top) const;
};
Blending blending_of(const core::Json& op);

// Fade: `after` faded back toward `before` — before + k × (B(before, after) − before), premultiplied (the alpha fades
// too). Normal (or a single channel) mixes the values themselves, any other blend the colours (composite).
void fade_values(const float* before, const float* after, float* out, std::size_t n, bool alpha, float k);
Rgba fade_rgba(const Rgba& before, const Rgba& after, std::string_view mode, float k);

// --- the work's memory -----------------------------------------------------------------------------------------------

// Working memory held in the imaging budget (render::ImageAllocationBudget) while it lives: `bytes` taken from it, or
// the work refused before they are had (too_large). Outside a budget it holds nothing.
class Held {
public:
    explicit Held(std::uint64_t bytes);
    ~Held();
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

private:
    struct Work;
    std::unique_ptr<Work> work_;
};
// The refusal of a work past the imaging budget (independent review B (C): refused, not run out of memory): OpError
// "the pixels are too large to work on at once (…)".
[[noreturn]] void too_large();

// --- a paint layer's pixels ------------------------------------------------------------------------------------------

// A layer's pixels as straight floats on its own grid (stretched over the page): `colours` values then the alpha for
// each pixel, rows from the top. An 8-bit layer (RGB page): its bytes / 255 (sRGB, or the page's profile's values) at
// its resolution; a precise layer: its samples (the page's values, HDR kept); an ink layer: the ink canvas's values
// (render/ink_canvas: CMYK and a multichannel page's plates their ink 0..1, Lab L/100 and (a, b + 128)/255, grey its
// lightness). Held in the imaging budget (past it: too_large).
class Pixels {
public:
    enum class Kind { Eight, Precise, Inks };
    Pixels(Kind kind, std::uint32_t width, std::uint32_t height, unsigned colours);
    Pixels(const Pixels& other);
    Pixels& operator=(const Pixels&) = delete;
    Pixels(Pixels&&) noexcept;
    Pixels& operator=(Pixels&&) noexcept;
    ~Pixels();

    Kind kind() const { return kind_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    unsigned colours() const { return colours_; }
    unsigned all() const { return colours_ + 1; }
    std::size_t size() const { return std::size_t(width_) * height_; }
    float* at(std::size_t i) { return v_.data() + i * all(); }
    const float* at(std::size_t i) const { return v_.data() + i * all(); }

    // How they are written back: a precise layer's precision, an ink layer's colour mode.
    bool floating = false;
    std::optional<core::PageColor> ink;

private:
    struct Work;
    Kind kind_;
    std::uint32_t width_, height_;
    unsigned colours_;
    std::unique_ptr<Work> work_;
    std::vector<float> v_;
};

// Pixels from each kind of bytes, and back (the same kind, size and depth; an 8-bit picture's bytes rounded).
Pixels from_image(const Image& rgba);
Image to_image(const Pixels& p);
Pixels from_precise(const std::string& raster);
std::string to_precise(const Pixels& p);
Pixels from_inks(const core::InkPixels& raster, const core::PageColor& color);
std::string to_inks(const Pixels& p);

// The paint layer `id` of the page at `at` as pixels on its own grid, its lines and fills drawn in first as
// filter_raster draws them (an 8-bit layer's into its PNG, precise pen lines into precise paint — convert_layer —, an
// ink layer's into its inks: the layer holds them as pixels after). OpError for no such layer, a locked one, one that is
// not a paint layer (pen or paint).
Pixels layer_pixels(core::OpContext& c, std::size_t at, const std::string& id);
// (PC4 D4) What layer_pixels does first, without reading the pixels: OpError for no such layer, a locked one, one that is
// not a paint layer; precise pen lines made precise paint (convert_layer). A caller that draws reads them itself.
void ready_paint_layer(core::OpContext& c, std::size_t at, const std::string& id);
// The pixels written back into that layer as its kind holds them (it is a paint layer after).
void put_layer_pixels(core::Document& doc, std::size_t at, const std::string& id, const Pixels& p);
// PC3 (B-0): a part of them — `box` on the layer's own grid (pixels; clamped to it: grid() says how big it is) — as
// layer_pixels gives the whole (its lines and fills drawn in first), without the rest of the layer held in floats (a
// precise B4 page at 600 dpi is some 820 MB of floats). Pixels of the box past the grid: none (the box is clamped).
struct Window {
    Box box;      // (the part read, clamped to the grid)
    Size grid;    // (the layer's whole grid)
};
Pixels layer_pixels(core::OpContext& c, std::size_t at, const std::string& id, const Box& box, Window* window = nullptr);
// Those pixels written back at `box` (as layer_pixels clamped it): the layer's pixels outside it keep their bytes (an
// 8-bit layer's PNG is written again, its other pixels the same; a precise layer's samples outside the box as they were;
// an ink layer's too).
void put_layer_pixels(core::Document& doc, std::size_t at, const std::string& id, const Pixels& p, const Box& box);
// PC4 E-0 (render/tiled_pixels): several parts at once — each of `wanted` as layer_pixels(…, box) gives it, the layer
// read (an 8-bit one's picture decoded, an ink one's inks drawn) once; `boxes` gets each part's box (clamped to the grid),
// `grid` the layer's whole grid — and written back at once, each part at its box as put_layer_pixels(…, box) writes one
// (the layer written once; the pixels outside the parts keep their bytes), or only the pixels its `only` marks (one
// byte a pixel of the box, row by row: not 0 written; the others keep their bytes — an ink layer's clear pixel its inks
// too). The parts are of one kind.
std::vector<Pixels> layer_pixels(core::OpContext& c, std::size_t at, const std::string& id, const std::vector<Box>& wanted,
                                 std::vector<Box>* boxes = nullptr, Size* grid = nullptr);
struct PixelsPart {
    Box box;
    const Pixels* pixels = nullptr;
    const std::vector<std::uint8_t>* only = nullptr;  // (none: every pixel of the box)
};
void put_layer_pixels(core::Document& doc, std::size_t at, const std::string& id, const std::vector<PixelsPart>& parts);
// PC4 E4 (review P2-1, P3-6): a part (`box`, clamped) of an ink raster as layer_pixels gives an ink layer's, and those
// pixels written back into it (or only those `only` marks) — for a worker that holds the layer's inks itself (its lines
// left as lines, the inks encoded once).
Pixels inks_part(const core::InkPixels& raster, const core::PageColor& color, const Box& box);
void put_inks_part(core::InkPixels& raster, const Pixels& p, const Box& box, const std::vector<std::uint8_t>* only = nullptr);

// An ink layer's pixels set from changed colours and alphas (`px`, one each: rgba_of's form) by the page's rules
// (render/ink correct: a colour left as it was keeps its inks). Worked in bands, their arrays in the imaging budget.
void from_rgba(Pixels& p, const std::vector<Rgba>& px);

// The page as it shows (print, without its finish, onion skin, nombre and spots) at the working 200 dpi: an RGB page's
// picture (its values on a page of a profile or of more than 8 bits: precise), an ink page's inks (opaque).
Pixels page_pixels(const core::Document& doc, const core::Page& page);
// The same at `dpi` (PC4 E3: Define Pattern at the page's working grid).
Pixels page_pixels(const core::Document& doc, const core::Page& page, int dpi);
// (PC4 E4 review P2-3) Only `region` of it (pixels of the working 200 dpi, within the page): the same pixels, the rest
// never drawn (RenderOptions::region).
Pixels page_pixels(const core::Document& doc, const core::Page& page, const Box& region);
// (PC4 E2, review P3-7) The same at `dpi` (the region in pixels of that dpi): ブラシを定義… draws the selection's box only.
Pixels page_pixels(const core::Document& doc, const core::Page& page, const Box& region, int dpi);
// A layer's pixels, nothing changed: a paint layer's (its lines and fills drawn in on a copy), any other layer as it
// shows alone (layer_image, 8 bits).
Pixels shown_layer_pixels(const core::Document& doc, const core::Page& page, const core::Layer& layer);
// The RGB of pixels (an ink layer's: the colours its inks mean, render/ink meaning_of, worked in bands) with their
// alpha. The caller holds what it returns in the budget (Held) when it keeps it.
std::vector<Rgba> rgba_of(const Pixels& p);

// An area of the page (a resolved {poly} or {mask}) as each pixel's coverage (0..1) on a grid of width × height
// stretched over the page; an 8-bit layer's grid as filter_raster draws its area (selection.area_mask at the working
// 200 dpi), any other through render/ink_edit's area_coverage.
std::vector<float> coverage(const core::Page& page, const core::Json& area, std::uint32_t width, std::uint32_t height, bool eight);

// How many of a grid's pixels (width × height over the page) span one pixel of the working 200 dpi (1 exactly for the
// page at that dpi, as an 8-bit paint layer's).
double grid_scale(const core::Page& page, std::uint32_t width, std::uint32_t height);

}  // namespace genko::render::image_ops
