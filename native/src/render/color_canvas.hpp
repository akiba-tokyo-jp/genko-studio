#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "core/color_adjust.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/exposure.hpp"
#include "render/image.hpp"
namespace genko::render {
// The RGB space a precise canvas composites in (color-model@1, ARCHITECTURE.md §4a): sRGB (Python's colours), or an
// RGB page's profile — its tone curves to and from linear light (the canvas mixes the page's linear values, in its
// primaries), and Little CMS between it and sRGB: sRGB colours (8-bit elements, precise lines) come in by the page's
// intent and black point compensation, and the screen gets sRGB by relative colorimetric with black point compensation.
class RgbSpace {
public:
    RgbSpace() = default;  // (sRGB)
    explicit RgbSpace(const core::RgbMode& mode);
    // The page's: its RGB mode's profile, or sRGB.
    static RgbSpace of(const core::Page& page);
    bool is_srgb() const { return !impl_; }
    // An encoded value (the page's, 0..1 or HDR) in linear light, and back.
    double decode(unsigned channel, double encoded) const;
    double encode(unsigned channel, double linear) const;
    // sRGB colours (encoded, 0..1; `count` RGB triples) as the page's encoded values, in place.
    void from_srgb(double* rgb, std::size_t count) const;
    // The page's encoded values as sRGB for the screen (relative colorimetric, black point compensation), in place.
    void to_srgb(double* rgb, std::size_t count) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
// PC2d (document:color-depth-model): a layer's own picture over a part of the page in a canvas's space, before it is
// composited — a precise layer's pixels with what is drawn on it (its lines, the pictures laid on it, its effects): per
// pixel straight linear-light RGB (the space's primaries) and its alpha.
struct ColorPicture {
    Size size;
    std::vector<std::array<double, 4>> pixels;
};
// Linear-light high precision compositor; RGBA8 is produced only at the output edge.
// Its working memory participates in the same budget as libImaging allocations.
class ColorCanvas {
public:
    // `space`: the page's RGB space (the canvas holds its linear values; image() shows them as sRGB).
    explicit ColorCanvas(const Image& initial, RgbSpace space = {});
    ~ColorCanvas();
    ColorCanvas(const ColorCanvas&) = delete;
    ColorCanvas& operator=(const ColorCanvas&) = delete;
    static bool supports_blend(std::string_view mode);
    // `page_values`: the 8-bit picture holds the page's values (a paint layer's pixels on a page of a profile), not sRGB
    // colours.
    void blend(const Image& rgba, double opacity, bool clip, std::string_view mode = "normal", bool page_values = false);
    // `mask` ("L", the area's size): the layer mask, which multiplies the source's alpha.
    void blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip, std::string_view mode = "normal",
               const Image* mask = nullptr);
    void blend(const ColorCanvas& source, double opacity, bool clip, std::string_view mode = "normal");
    // PC2d: a layer's picture (the canvas's size; `mask` ("L"): its mask, which multiplies its alpha).
    void blend(const ColorPicture& picture, double opacity, bool clip, std::string_view mode = "normal", const Image* mask = nullptr);
    void blend_stroke(const Image& mask, const core::Json& color, double opacity);
    // brushes@1 (PC4 E1): a dab brush's line by its coverage in float (the canvas's size, row by row: no steps in a soft
    // brush), in `color`, or each pixel in its own straight sRGB colour (`colours`: the colour dynamics).
    void blend_stroke(const std::vector<float>& cover, const core::Json& color, double opacity,
                      const std::vector<std::array<float, 3>>* colours = nullptr);
    // PC2d: what sets nothing for the next layer to clip to (a tone layer: the 8-bit page forgets the alpha before).
    void forget_clip();
    // PC2d: a correction that moves shapes, worked on the canvas's own values (color_raster's): `changed` (the space's
    // encoded samples and the alpha, the canvas's size) laid over the pixels below as the 8-bit page composites its
    // result (render._adjusted): every sample, the alpha too, mixed by opacity × mask × the clipped-to layer's alpha;
    // `mode` (its native.blend) laid on the colour first as adjust does.
    void correct(const core::ColorRasterView& changed, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal");
    // An exposure in linear light. `mode` (PC2: its native.blend; normal: mixed in linear light as before): the exposed
    // colour laid over the one below by the blend's formula in their encoded samples, as adjust does.
    void expose(const core::Exposure& exposure, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal");
    // A correction layer (core/adjust.hpp's AdjustFunction: Python's form is PreciseAdjustment): its change laid over the
    // pixels below in their encoded samples (sRGB, or the page's values on a page of a profile), as the 8-bit page
    // composites a correction: by opacity × mask × the clipped-to layer's alpha. `mode` (PC2: its native.blend): the
    // changed colour b' laid over the colour b below by the blend's formula (this canvas's, on the encoded samples),
    // mix(b, blend(b, b'), amount); the alpha stays. `at`: the canvas's top left on the page (where a pixel is, for a
    // dither). core::Error("not_yet_ported") for a mode it does not composite.
    void adjust(const core::AdjustFunction& adjustment, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal",
                Point at = Point{0, 0});
    // Drawing in 8 bits on the canvas's own picture (image(), RGBA), as the 8-bit page draws on its picture so far
    // (the balloons set under a layer): each pixel `draw` paints takes the value it is given there, brought in as an
    // 8-bit picture is (its sRGB samples in linear light, as the paper comes in); the others keep their precise values.
    // A pixel is painted where the drawing changes it on that picture or on one whose colour samples all lie half the
    // range away (so a balloon over a precise colour that rounds to its own colour still covers it). What is drawn so
    // is not a layer: the alpha a clipped layer clips to stays that of the layer before.
    void draw_8bit(const std::function<void(Image&)>& draw);
    // The picture as the screen shows it (sRGB).
    Image image() const;
    Size size() const;
    const RgbSpace& space() const;
    // One pixel as composited: straight linear-light RGB (the space's) and its alpha.
    std::array<double, 4> linear_pixel(std::size_t i) const;
    bool is_opaque() const;
    bool keeps_preceding_alpha() const;
    // The pixels as a precise raster of the space's encoded values (GKCR).
    std::string color_raster(std::string_view precision) const;
    // PC2d: the pixels as a layer's picture (what is drawn on a canvas of a layer's own).
    ColorPicture picture() const;
    // PC4 E6 (layer-styles@1, render/layer_styles): a layer drawn with its style over `part` of the canvas — `run` gets its
    // pixels there (straight linear RGB and the alpha, 4 a pixel) and what the layer's opacity is multiplied by at each
    // (the alpha of the layer before when `clip`, else 1), and leaves the result in the pixels; `own` (the part's size):
    // the layer's own alpha, what a layer clipped to it clips to.
    void styled(const Box& part, bool clip, const std::vector<float>& own,
                const std::function<void(std::vector<float>& pixels, const std::vector<float>& weight)>& run);
    // (after the last part) the next layer clips to the styled one
    void styled_done();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Image color_raster_preview(const core::ColorRasterView& source, Size full, Box area, const RgbSpace& space = {});
// PC2d: a precise raster stretched over the page (`full`, pixels), the part `area`, as ColorCanvas::blend samples it.
ColorPicture color_picture(const core::ColorRasterView& source, Size full, Box area, const RgbSpace& space);
// PC2d: an 8-bit RGBA picture in the space: its sRGB colours brought in as an 8-bit layer's are, or (`page_values`) its
// samples the space's own values.
ColorPicture color_picture(const Image& rgba, const RgbSpace& space, bool page_values = false);
// PC2d: `above` laid over `below` (the same size), normal, straight alpha, as one layer's parts are.
void picture_over(ColorPicture& below, const ColorPicture& above);
// PC2d: the picture as the screen shows it ("RGBA", sRGB, its alpha kept), and its alpha alone as an 8-bit picture
// holds it ("L").
Image picture_rgba(const ColorPicture& picture, const RgbSpace& space);
Image picture_alpha(const ColorPicture& picture);
// PC2d: an sRGB colour (0..255) as the space's linear light.
std::array<double, 3> linear_colour(const RgbSpace& space, const std::array<int, 3>& rgb);
// PC4 E6: PhotoCraft's formula of a blend mode (color/blend.rs, the precise canvas's: B(below, above)) on three values.
std::array<double, 3> blend_linear(std::string_view mode, const std::array<double, 3>& below, const std::array<double, 3>& above);
// An 8-bit RGBA picture's sRGB colours as the space's values (its alpha kept; sRGB: the picture itself).
Image srgb_into(const Image& rgba, const RgbSpace& space);
// And back: an 8-bit RGBA picture of the space's values as the sRGB the screen shows (relative colorimetric, black point
// compensation; its alpha kept; sRGB: the picture itself).
Image srgb_from(const Image& rgba, const RgbSpace& space);
// A correction layer's function (its "adjust", core::adjust_function: Python's form — a kind of filters.ADJUSTMENTS or
// exposure, and its settings — as PreciseAdjustment, the C++ build's own by its kind; `page` and `doc` the layer's), or
// none when it does nothing: no kind, a kind apply_filter does not know (a plugin, one that moves shapes: the callers'),
// or settings the 8-bit page refuses with a ValueError (render._adjusted). NotYetPorted("adjust:<kind>") for one whose
// formula is not in this build yet.
core::AdjustPtr correction_of(const core::Layer& layer, const core::Page& page, const core::Document* doc = nullptr);
} // namespace genko::render
