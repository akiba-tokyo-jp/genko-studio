#include "render/hdr_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/color_raster.hpp"
#include "core/exposure.hpp"
#include "core/model.hpp"
#include "render/color_canvas.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"

namespace genko::render {

namespace {

unsigned char byte(double v) { return static_cast<unsigned char>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0) * 255.0)); }

double cut(double v) { return std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0); }

// (within one step: the values' own picture as render_page made it, its rounding aside)
bool near(const unsigned char* a, const unsigned char* b) {
    return std::abs(a[0] - b[0]) <= 1 && std::abs(a[1] - b[1]) <= 1 && std::abs(a[2] - b[2]) <= 1;
}

// The tile with each pixel whose plain look (`plain`, 3 bytes a pixel) is the drawn one's shown as `shown`.
Image laid(const Image& drawn, const std::vector<unsigned char>& plain, const std::vector<unsigned char>& shown) {
    std::string bytes = drawn.convert("RGB").tobytes();
    for (std::size_t i = 0; i + 2 < bytes.size() && i + 2 < plain.size(); i += 3) {
        auto* now = reinterpret_cast<unsigned char*>(bytes.data() + i);
        if (!near(now, plain.data() + i)) continue;  // (drawn over the page: as it is)
        now[0] = shown[i];
        now[1] = shown[i + 1];
        now[2] = shown[i + 2];
    }
    return Image::frombytes("RGB", drawn.size(), bytes);
}

}  // namespace

HdrPreview HdrPreview::clamped() const {
    HdrPreview out = *this;
    out.exposure = std::isfinite(exposure) ? std::clamp(exposure, kMinExposure, kMaxExposure) : 0.0;
    out.gamma = std::isfinite(gamma) ? std::clamp(gamma, kMinGamma, kMaxGamma) : 1.0;
    return out;
}

void hdr_linear(const HdrPreview& preview, double* rgb) {
    if (preview.identity()) return;
    if (preview.highlight_compression) {
        // (Reinhard on the light: L / (1 + L), the colour kept in proportion)
        for (int c = 0; c < 3; ++c) rgb[c] = std::max(0.0, std::isfinite(rgb[c]) ? rgb[c] : 0.0);
        const double light = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
        if (light > 0) {
            const double k = std::isfinite(light) ? 1.0 / (1.0 + light) : 0.0;
            for (int c = 0; c < 3; ++c) rgb[c] *= k;
        }
        return;
    }
    const double factor = std::exp2(preview.exposure);
    for (int c = 0; c < 3; ++c) {
        const double v = std::max(0.0, std::isfinite(rgb[c]) ? rgb[c] * factor : 0.0);
        rgb[c] = preview.gamma == 1.0 ? v : std::pow(v, 1.0 / preview.gamma);
    }
}

double hdr_grey(const HdrPreview& preview, double linear) {
    double rgb[3] = {linear, linear, linear};
    hdr_linear(preview, rgb);
    return rgb[0];
}

bool hdr_page(const core::Page& page) {
    if (page.color) return page.color->depth == 32;
    return page.rgb && page.rgb->depth == 32;
}

Image hdr_shown(const Image& drawn, const std::string& precise, const RgbSpace& space, const HdrPreview& preview) {
    if (preview.identity()) return drawn;
    const core::ColorRasterView view(precise);
    const auto w = static_cast<std::size_t>(view.width());
    const auto h = static_cast<std::size_t>(view.height());
    if (static_cast<std::size_t>(drawn.width()) != w || static_cast<std::size_t>(drawn.height()) != h) return drawn;
    std::vector<unsigned char> plain(w * h * 3), shown(w * h * 3);
    std::vector<double> a(w * 3), b(w * 3);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::array<double, 4> p = view.pixel(y * w + x);
            double lin[3];
            for (unsigned c = 0; c < 3; ++c) {
                a[x * 3 + c] = p[c];  // (the page's value, encoded: as ColorCanvas::image shows it)
                lin[c] = space.decode(c, p[c]);
            }
            hdr_linear(preview, lin);
            for (unsigned c = 0; c < 3; ++c) b[x * 3 + c] = space.encode(c, cut(lin[c]));
        }
        if (!space.is_srgb()) {
            space.to_srgb(a.data(), w);
            space.to_srgb(b.data(), w);
        }
        for (std::size_t k = 0; k < w * 3; ++k) {
            plain[y * w * 3 + k] = byte(a[k]);
            shown[y * w * 3 + k] = byte(b[k]);
        }
    }
    return laid(drawn, plain, shown);
}

Image hdr_shown(const Image& drawn, const core::InkPixels& inks, const core::PageColor& color, const HdrPreview& preview) {
    if (preview.identity()) return drawn;
    const auto w = static_cast<std::size_t>(inks.width);
    const auto h = static_cast<std::size_t>(inks.height);
    if (static_cast<std::size_t>(drawn.width()) != w || static_cast<std::size_t>(drawn.height()) != h || inks.colours() == 0) return drawn;
    const unsigned n = inks.colours();
    const bool grey = color.space == core::InkSpace::Gray && !color.bitmap;
    const GrayCurve curve = grey ? GrayCurve(color) : GrayCurve();
    std::vector<unsigned char> plain(w * h * 3), shown(w * h * 3);
    std::vector<double> values(w * n), changed(w * n);
    std::vector<float> a(w * 3), b(w * 3);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            for (unsigned c = 0; c < n; ++c) values[x * n + c] = from_canvas(color.space, c, inks.value(i, c));
            if (grey) {  // (its lightness, any value: through its tone curve; cut to 0..1 as the screen shows it)
                const double v = values[x * n];
                values[x * n] = cut(v);
                changed[x * n] = cut(curve.encode(hdr_grey(preview, curve.decode(v))));
            }
        }
        if (grey && !color.duotone.empty()) {  // (a duotone page: its grey printed in its inks)
            for (std::size_t x = 0; x < w; ++x) {
                const std::array<double, 3> was = core::duotone_print(color.duotone, values[x * n]);
                const std::array<double, 3> now = core::duotone_print(color.duotone, changed[x * n]);
                for (unsigned c = 0; c < 3; ++c) {
                    a[x * 3 + c] = static_cast<float>(was[c]);
                    b[x * 3 + c] = static_cast<float>(now[c]);
                }
            }
        } else {
            ink::to_srgb(color, values.data(), a.data(), w);
            if (grey) {
                ink::to_srgb(color, changed.data(), b.data(), w);
            } else {  // (the colours as the screen shows them, through the preview in linear light)
                for (std::size_t x = 0; x < w; ++x) {
                    double lin[3];
                    for (unsigned c = 0; c < 3; ++c) lin[c] = core::srgb_to_linear(cut(a[x * 3 + c]));
                    hdr_linear(preview, lin);
                    for (unsigned c = 0; c < 3; ++c) b[x * 3 + c] = static_cast<float>(core::linear_to_srgb(cut(lin[c])));
                }
            }
        }
        for (std::size_t k = 0; k < w * 3; ++k) {
            plain[y * w * 3 + k] = byte(a[k]);
            shown[y * w * 3 + k] = byte(b[k]);
        }
    }
    return laid(drawn, plain, shown);
}

}  // namespace genko::render
