// A filter plugin's run in bands (render/plugin_bands.hpp). PhotoCraft v0.2.0's crates/plugins/src/apply.rs (output_area,
// apply, run_band, read_clamped: lines 70-235, MIT, native/third_party/photocraft/NOTICE.md) on a picture held whole
// instead of PhotoCraft's tiles: the same bands (tile-aligned, one tile tall, as many tiles across as fit in the band's
// bytes), the same margins, the same cleaning and the same mix with the selection.

#include "render/plugin_bands.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "render/page.hpp"

namespace genko::render::plugin_bands {

namespace {

using core::Json;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

Box intersect(const Box& a, const Box& b) {
    Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (out.empty()) return Box{};
    return out;
}

Box inflate(const Box& b, int by) { return Box{b.x0 - by, b.y0 - by, b.x1 + by, b.y1 + by}; }

// The box of the pixels where `keep(i)` holds (none: empty).
template <class Keep>
Box bounds_of(int width, int height, const Keep& keep) {
    Box out{width, height, -1, -1};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!keep(std::size_t(y) * std::size_t(width) + std::size_t(x))) continue;
            out.x0 = std::min(out.x0, x);
            out.y0 = std::min(out.y0, y);
            out.x1 = std::max(out.x1, x + 1);
            out.y1 = std::max(out.y1, y + 1);
        }
    }
    if (out.x1 < 0) return Box{};
    return out;
}

// `rect` of the picture, its edge pixels repeated outside it (as built-in neighbourhood filters at the canvas edge).
std::vector<float> read_clamped(const Samples& p, const Box& rect) {
    const unsigned n = p.channels;
    const int w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    std::vector<float> out(std::size_t(w) * std::size_t(h) * n);
    for (int row = 0; row < h; ++row) {
        const int sy = std::clamp(rect.y0 + row, 0, p.height - 1);
        for (int col = 0; col < w; ++col) {
            const int sx = std::clamp(rect.x0 + col, 0, p.width - 1);
            const float* from = p.data.data() + (std::size_t(sy) * std::size_t(p.width) + std::size_t(sx)) * n;
            std::copy_n(from, n, out.data() + (std::size_t(row) * std::size_t(w) + std::size_t(col)) * n);
        }
    }
    return out;
}

}  // namespace

std::string mode_name(Mode mode) {
    switch (mode) {
        case Mode::Bitmap: return "bitmap";
        case Mode::Gray: return "grayscale";
        case Mode::Rgb: return "rgb";
        case Mode::Cmyk: return "cmyk";
        case Mode::Multichannel: return "multichannel";
        case Mode::Duotone: return "duotone";
        case Mode::Lab: return "lab";
    }
    return "rgb";
}

Mode mode_of_inks(const core::PageColor& color, core::InkSpace space) {
    switch (space) {
        case core::InkSpace::Cmyk: return Mode::Cmyk;
        case core::InkSpace::Lab: return Mode::Lab;
        case core::InkSpace::Multi: return Mode::Multichannel;
        case core::InkSpace::Mask: return Mode::Gray;
        case core::InkSpace::Gray: break;
    }
    if (!color.duotone.empty()) return Mode::Duotone;
    if (color.bitmap) return Mode::Bitmap;
    return Mode::Gray;
}

Box output_area(const Samples& picture, const Shape& shape, const std::vector<float>* coverage) {
    const Box whole{0, 0, picture.width, picture.height};
    if (whole.empty()) return Box{};
    Box area = whole;
    if (!shape.whole_canvas && picture.alpha) {
        const unsigned n = picture.channels;
        const Box content = bounds_of(picture.width, picture.height, [&](std::size_t i) { return picture.data[i * n + n - 1] > 0.0f; });
        if (content.empty()) return Box{};
        area = intersect(inflate(content, static_cast<int>(shape.overlap)), whole);
    }
    if (coverage != nullptr) area = intersect(area, bounds_of(picture.width, picture.height, [&](std::size_t i) { return (*coverage)[i] > 0.0f; }));
    return area;
}

void run(Samples& picture, const Shape& shape, const std::vector<float>* coverage, const Json& params, const BandWork& work, const std::stop_token& stop) {
    const Box area = output_area(picture, shape, coverage);
    if (area.empty()) return;
    const unsigned n = picture.channels;
    const int ov = static_cast<int>(shape.overlap);
    // (bands of tiles: one tile tall, as many across as fit in band_bytes; half the plugin's memory is for the band)
    const std::size_t tile_bytes = std::size_t(kTile) * kTile * n * 4;
    const std::size_t room = shape.memory_bytes / 2;
    const int across = static_cast<int>(std::clamp<std::size_t>(std::min(shape.band_bytes, room) / tile_bytes, 1, 1024));
    const std::size_t in_bytes = std::size_t(across * kTile + 2 * ov) * std::size_t(kTile + 2 * ov) * n * 4;
    if (in_bytes > room) throw core::Error("plugin_runner", "a band of the picture does not fit in the plugin's memory");
    std::vector<Box> bands;
    const int tx0 = floor_div(area.x0, kTile), ty0 = floor_div(area.y0, kTile);
    const int tx1 = floor_div(area.x1 - 1, kTile), ty1 = floor_div(area.y1 - 1, kTile);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; tx += across) {
            const Box r{tx * kTile, ty * kTile, std::min(tx + across, tx1 + 1) * kTile, (ty + 1) * kTile};
            const Box b = intersect(r, area);
            if (!b.empty()) bands.push_back(b);
        }
    }
    Json given = params.is_object() ? params : Json::object();
    const bool int_depth = picture.depth != 32;
    for (const Box& band : bands) {
        if (stop.stop_requested()) throw Cancelled();
        const Box input = inflate(band, ov);
        const int iw = input.x1 - input.x0, ih = input.y1 - input.y0;
        std::vector<float> data = read_clamped(picture, input);
        const std::vector<float> original = data;
        Json p = given;
        p["_image"] = Json{{"x", input.x0},
                           {"y", input.y0},
                           {"width", iw},
                           {"height", ih},
                           {"overlap", ov},
                           {"canvasWidth", picture.width},
                           {"canvasHeight", picture.height},
                           {"mode", mode_name(picture.mode)},
                           {"depth", picture.depth},
                           {"alpha", picture.alpha},
                           {"channels", n}};
        work(data, iw, ih, p);
        if (data.size() != original.size()) throw core::Error("plugin_runner", "the plugin returned a band of another size");
        // keep the band (the margins dropped), cleaned, laid over the picture by the coverage
        for (int y = band.y0; y < band.y1; ++y) {
            for (int x = band.x0; x < band.x1; ++x) {
                const std::size_t at = std::size_t(y) * std::size_t(picture.width) + std::size_t(x);
                const float k = coverage != nullptr ? std::clamp((*coverage)[at], 0.0f, 1.0f) : 1.0f;
                const std::size_t si = (std::size_t(y - input.y0) * std::size_t(iw) + std::size_t(x - input.x0)) * n;
                float* out = picture.data.data() + at * n;
                for (unsigned c = 0; c < n; ++c) {
                    const float o = original[si + c];
                    float v = data[si + c];
                    if (!std::isfinite(v)) v = o;
                    if (int_depth || (picture.alpha && c == n - 1)) v = std::clamp(v, 0.0f, 1.0f);
                    out[c] = o + (v - o) * k;
                }
            }
        }
    }
}

}  // namespace genko::render::plugin_bands
