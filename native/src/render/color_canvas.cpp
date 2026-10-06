#include "render/color_canvas.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "core/error.hpp"
#include "render/imaging.hpp"

namespace genko::render {
namespace {
struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};
using Pixel = std::array<double, 5>; // straight linear RGB, alpha, preceding layer alpha
std::array<double, 4> linear(std::array<double, 4> p) {
    for (unsigned c = 0; c < 3; ++c) p[c] = core::srgb_to_linear(p[c]);
    return p;
}
int byte(double x) { return static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0)*255)); }
std::array<double, 4> sampled(const core::ColorRasterView& src, double x, double y) {
    x = std::clamp(x, 0.0, double(src.width()-1));
    y = std::clamp(y, 0.0, double(src.height()-1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0+1, src.width()-1), y1 = std::min(y0+1, src.height()-1);
    const double fx = x-x0, fy = y-y0;
    const std::array<double, 4> weights{(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    const std::array<std::size_t, 4> indices{std::size_t(y0)*src.width()+x0, std::size_t(y0)*src.width()+x1,
        std::size_t(y1)*src.width()+x0, std::size_t(y1)*src.width()+x1};
    std::array<double, 4> out{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto p = linear(src.pixel(indices[i]));
        out[3] += p[3]*weights[i];
        for (unsigned c = 0; c < 3; ++c) out[c] += p[c]*p[3]*weights[i];
    }
    if (out[3] > 0) for (unsigned c = 0; c < 3; ++c) out[c] /= out[3];
    return out;
}
}
struct ColorCanvas::Impl {
    // The budget must outlive both the reservation and its payload.
    ImageAllocationBudget budget{512ULL*1024*1024};
    Work work;
    std::vector<Pixel> pixels;
    Size size;
    bool previous = false;
    explicit Impl(Size s) : size(s) {
        if (s.width <= 0 || s.height <= 0) throw core::Error("value", "invalid color canvas dimensions");
        const auto n = std::uint64_t(s.width)*std::uint64_t(s.height);
        if (n > (512ULL*1024*1024)/sizeof(Pixel)) throw core::Error("image_too_large", "color canvas budget exceeded");
        if (!genko_imaging_budget_reserve(&work.reservation, n*sizeof(Pixel))) detail::throw_imaging_error();
        pixels.resize(static_cast<std::size_t>(n));
    }
    void over(std::size_t i, const std::array<double, 4>& src, double opacity, bool clip) {
        auto& dst = pixels[i];
        const double a = src[3]*std::clamp(opacity, 0.0, 1.0)*(clip && previous ? dst[4] : 1);
        const double alpha = a+dst[3]*(1-a);
        if (alpha > 0) for (unsigned c = 0; c < 3; ++c)
            dst[c] = (src[c]*a+dst[c]*dst[3]*(1-a))/alpha;
        dst[3] = alpha;
        dst[4] = src[3];
    }
};
ColorCanvas::ColorCanvas(const Image& initial) : impl_(std::make_unique<Impl>(initial.size())) {
    const auto rgba = initial.convert("RGBA");
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        for (int x = 0; x < rgba.width(); ++x) {
            const auto p = linear({row[x*4]/255.0, row[x*4+1]/255.0, row[x*4+2]/255.0, row[x*4+3]/255.0});
            impl_->pixels[std::size_t(y)*rgba.width()+x] = {p[0], p[1], p[2], p[3], 0};
        }
    }
}
ColorCanvas::~ColorCanvas() = default;
void ColorCanvas::blend(const Image& input, double opacity, bool clip) {
    if (input.size() != impl_->size) throw core::Error("value", "color canvas images do not match");
    const auto rgba = input.convert("RGBA");
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        for (int x = 0; x < rgba.width(); ++x)
            impl_->over(std::size_t(y)*rgba.width()+x,
                linear({row[x*4]/255.0, row[x*4+1]/255.0, row[x*4+2]/255.0, row[x*4+3]/255.0}), opacity, clip);
    }
    impl_->previous = true;
}
void ColorCanvas::blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip) {
    if (full.width <= 0 || full.height <= 0 || area.width() != impl_->size.width || area.height() != impl_->size.height)
        throw core::Error("value", "color canvas region does not match");
    for (int y = 0; y < area.height(); ++y) for (int x = 0; x < area.width(); ++x) {
        const auto p = sampled(source, (area.x0+x+.5)*source.width()/full.width-.5,
            (area.y0+y+.5)*source.height()/full.height-.5);
        impl_->over(std::size_t(y)*area.width()+x, p, opacity, clip);
    }
    impl_->previous = true;
}
void ColorCanvas::expose(const core::Exposure& e, double opacity, bool clip, const Image* mask) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "exposure mask does not match");
    for (int y = 0; y < impl_->size.height; ++y) for (int x = 0; x < impl_->size.width; ++x) {
        auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
        double amount = std::clamp(opacity, 0.0, 1.0)*(clip && impl_->previous ? p[4] : 1);
        if (mask) amount *= reinterpret_cast<const unsigned char*>(mask->raw()->image[y])[x]/255.0;
        for (unsigned c = 0; c < 3; ++c) p[c] = std::lerp(p[c], e.apply(p[c]), amount);
    }
}
Image ColorCanvas::image() const {
    auto out = Image::create("RGBA", impl_->size, Ink{0, 0, 0, 0});
    for (int y = 0; y < impl_->size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) {
            const auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
            for (unsigned c = 0; c < 3; ++c) row[x*4+c] = static_cast<unsigned char>(byte(core::linear_to_srgb(p[c])));
            row[x*4+3] = static_cast<unsigned char>(byte(p[3]));
        }
    }
    return out;
}
Image color_raster_preview(const core::ColorRasterView& src, Size full, Box area) {
    const auto transparent = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    ColorCanvas out(transparent);
    out.blend(src, full, area, 1, false);
    return out.image();
}
} // namespace genko::render
