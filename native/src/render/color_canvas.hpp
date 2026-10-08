#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include "core/color_adjust.hpp"
#include "core/color_raster.hpp"
#include "core/exposure.hpp"
#include "render/image.hpp"
namespace genko::render {
// Linear-light high precision compositor; RGBA8 is produced only at the output edge.
// Its working memory participates in the same budget as libImaging allocations.
class ColorCanvas {
public:
    explicit ColorCanvas(const Image& initial);
    ~ColorCanvas();
    ColorCanvas(const ColorCanvas&) = delete;
    ColorCanvas& operator=(const ColorCanvas&) = delete;
    static bool supports_blend(std::string_view mode);
    void blend(const Image& rgba, double opacity, bool clip, std::string_view mode = "normal");
    // `mask` ("L", the area's size): the layer mask, which multiplies the source's alpha.
    void blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip, std::string_view mode = "normal",
               const Image* mask = nullptr);
    void blend(const ColorCanvas& source, double opacity, bool clip, std::string_view mode = "normal");
    void blend_stroke(const Image& mask, const core::Json& color, double opacity);
    void expose(const core::Exposure& exposure, double opacity, bool clip, const Image* mask = nullptr);
    // A correction layer of filters.ADJUSTMENTS: its change laid over the pixels below in their sRGB samples (as the
    // 8-bit page composites a correction: by opacity × mask × the clipped-to layer's alpha).
    void adjust(const core::PreciseAdjustment& adjustment, double opacity, bool clip, const Image* mask = nullptr);
    // Drawing in 8 bits on the canvas's own picture (image(), RGBA), as the 8-bit page draws on its picture so far
    // (the balloons set under a layer): each pixel `draw` paints takes the value it is given there, brought in as an
    // 8-bit picture is (its sRGB samples in linear light, as the paper comes in); the others keep their precise values.
    // A pixel is painted where the drawing changes it on that picture or on one whose colour samples all lie half the
    // range away (so a balloon over a precise colour that rounds to its own colour still covers it). What is drawn so
    // is not a layer: the alpha a clipped layer clips to stays that of the layer before.
    void draw_8bit(const std::function<void(Image&)>& draw);
    Image image() const;
    Size size() const;
    // One pixel as composited: straight linear-light RGB and its alpha.
    std::array<double, 4> linear_pixel(std::size_t i) const;
    bool is_opaque() const;
    bool keeps_preceding_alpha() const;
    std::string color_raster(std::string_view precision) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Image color_raster_preview(const core::ColorRasterView& source, Size full, Box area);
// A correction layer's adjustment (its "adjust": a kind of filters.ADJUSTMENTS but exposure, and its settings), or
// none when it does nothing: no kind, or settings the 8-bit page refuses with a ValueError (render._adjusted).
// NotYetPorted for a kind of filter that is not a colour adjustment.
std::optional<core::PreciseAdjustment> correction_of(const core::Layer& layer);
} // namespace genko::render
