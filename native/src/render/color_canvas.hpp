#pragma once
#include <memory>
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
    void blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip, std::string_view mode = "normal");
    void blend(const ColorCanvas& source, double opacity, bool clip, std::string_view mode = "normal");
    void blend_stroke(const Image& mask, const core::Json& color, double opacity);
    void expose(const core::Exposure& exposure, double opacity, bool clip, const Image* mask = nullptr);
    Image image() const;
    bool is_opaque() const;
    bool keeps_preceding_alpha() const;
    std::string color_raster(std::string_view precision) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Image color_raster_preview(const core::ColorRasterView& source, Size full, Box area);
} // namespace genko::render
