// A filter plugin's run in bands (render/plugin_bands.hpp). PhotoCraft v0.2.0's crates/plugins/src/apply.rs (output_area,
// apply, run_band, read_clamped: lines 70-235, MIT, native/third_party/photocraft/NOTICE.md) on a picture read and written
// where it is kept: the same bands (tile-aligned, one tile tall, as many tiles across as fit in the band's bytes), the
// same margins, the same cleaning and the same mix with the selection.

#include "render/plugin_bands.hpp"

#include <algorithm>
#include <cmath>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "render/imaging.hpp"
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

// Bytes borrowed from the imaging budget the caller is in (none open: nothing counted), given back when it goes.
class Lease {
public:
    Lease() = default;
    ~Lease() { genko_imaging_budget_release(&reservation_); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    void borrow(std::uint64_t bytes) {
        if (!genko_imaging_budget_reserve(&reservation_, bytes)) {
            genko_imaging_clear_error();
            throw core::OpError("the picture is too large for a filter plugin to work on in memory");
        }
    }

private:
    ImagingMemoryInstance reservation_{};
};

// The band input's size at most (f32 bytes) and the tiles a band takes across.
struct Cut {
    int across = 1;
    std::size_t in_bytes = 0;
};
Cut cut_of(unsigned n, const Shape& shape) {
    const int ov = static_cast<int>(shape.overlap);
    const std::size_t tile_bytes = std::size_t(kTile) * kTile * n * 4;
    const std::size_t room = shape.memory_bytes / 2;
    Cut c;
    c.across = static_cast<int>(std::clamp<std::size_t>(std::min(shape.band_bytes, room) / tile_bytes, 1, 1024));
    c.in_bytes = std::size_t(c.across * kTile + 2 * ov) * std::size_t(kTile + 2 * ov) * n * 4;
    return c;
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

Picture picture_of(Samples& s, const std::vector<float>* coverage) {
    Picture p;
    p.width = s.width;
    p.height = s.height;
    p.channels = s.channels;
    p.depth = s.depth;
    p.alpha = s.alpha;
    p.mode = s.mode;
    Samples* held = &s;
    p.read = [held](int y, int x0, int x1, float* out) {
        const unsigned n = held->channels;
        std::copy_n(held->data.data() + (std::size_t(y) * std::size_t(held->width) + std::size_t(x0)) * n, std::size_t(x1 - x0) * n, out);
    };
    p.write = [held](int y, int x0, int x1, const float* in) {
        const unsigned n = held->channels;
        std::copy_n(in, std::size_t(x1 - x0) * n, held->data.data() + (std::size_t(y) * std::size_t(held->width) + std::size_t(x0)) * n);
    };
    if (coverage != nullptr) {
        const int w = s.width;
        p.coverage = [coverage, w](int y, int x0, int x1, float* out) {
            std::copy_n(coverage->data() + std::size_t(y) * std::size_t(w) + std::size_t(x0), std::size_t(x1 - x0), out);
        };
    }
    return p;
}

Box output_area(const Picture& p, const Shape& shape) {
    const Box whole{0, 0, p.width, p.height};
    if (whole.empty()) return Box{};
    const unsigned n = p.channels;
    // (the box of the pixels where a row's `keep` holds, row by row)
    const auto bounds = [&](unsigned per_pixel, const std::function<void(int, float*)>& row, const std::function<bool(const float*)>& keep) {
        Box out{p.width, p.height, -1, -1};
        std::vector<float> line(std::size_t(p.width) * per_pixel);
        for (int y = 0; y < p.height; ++y) {
            row(y, line.data());
            for (int x = 0; x < p.width; ++x) {
                if (!keep(line.data() + std::size_t(x) * per_pixel)) continue;
                out.x0 = std::min(out.x0, x);
                out.y0 = std::min(out.y0, y);
                out.x1 = std::max(out.x1, x + 1);
                out.y1 = std::max(out.y1, y + 1);
            }
        }
        return out.x1 < 0 ? Box{} : out;
    };
    Box area = whole;
    if (!shape.whole_canvas && p.alpha) {
        const Box content = bounds(n, [&](int y, float* out) { p.read(y, 0, p.width, out); }, [n](const float* px) { return px[n - 1] > 0.0f; });
        if (content.empty()) return Box{};
        area = intersect(inflate(content, static_cast<int>(shape.overlap)), whole);
    }
    if (p.coverage) {
        area = intersect(area, bounds(1, [&](int y, float* out) { p.coverage(y, 0, p.width, out); }, [](const float* v) { return *v > 0.0f; }));
    }
    return area;
}

std::uint64_t held_bytes(int width, int height, unsigned n, const Shape& shape) {
    (void)height;
    const Cut c = cut_of(n, shape);
    const std::uint64_t row = std::uint64_t(width) * n * 4;
    // the band's input and its original, the result kept for it, and what goes through the pipe (out, back, read in)
    const std::uint64_t band = std::uint64_t(c.in_bytes) * 6;
    const std::uint64_t made = row * kTile;                     // (a tile row of results)
    const std::uint64_t strip = row * shape.overlap;            // (the rows its margins reach above it)
    const std::uint64_t lines = row * 2 + std::uint64_t(width) * 4;  // (a row read, a row of coverage)
    return band + made + strip + lines;
}

void run(Picture& picture, const Shape& shape, const Json& params, const BandWork& work, const std::stop_token& stop) {
    const unsigned n = picture.channels;
    const int ov = static_cast<int>(shape.overlap);
    const int W = picture.width, H = picture.height;
    const Cut cut = cut_of(n, shape);
    if (cut.in_bytes > shape.memory_bytes / 2) throw core::Error("plugin_runner", "a band of the picture does not fit in the plugin's memory");
    Lease lease;
    lease.borrow(held_bytes(W, H, n, shape));
    const Box area = output_area(picture, shape);
    if (area.empty()) return;
    const int tx0 = floor_div(area.x0, kTile), ty0 = floor_div(area.y0, kTile);
    const int tx1 = floor_div(area.x1 - 1, kTile), ty1 = floor_div(area.y1 - 1, kTile);
    const Json given = params.is_object() ? params : Json::object();
    const bool int_depth = picture.depth != 32;
    // (the rows above the tile row being made, as they were before it was written: rows [strip_y0, strip_y0 + strip_rows),
    // the picture's width)
    std::vector<float> strip;
    int strip_y0 = 0, strip_rows = 0;
    std::vector<float> segment(std::size_t(W) * n);
    // `input` of the picture as it was, its edge pixels repeated outside it (as built-in neighbourhood filters do)
    const auto fetch = [&](const Box& input) {
        const int iw = input.x1 - input.x0, ih = input.y1 - input.y0;
        std::vector<float> out(std::size_t(iw) * std::size_t(ih) * n);
        const int lo = std::clamp(input.x0, 0, W - 1), hi = std::clamp(input.x1, 1, W);
        for (int r = 0; r < ih; ++r) {
            const int sy = std::clamp(input.y0 + r, 0, H - 1);
            const float* src = nullptr;
            if (sy >= strip_y0 && sy < strip_y0 + strip_rows) {
                src = strip.data() + (std::size_t(sy - strip_y0) * std::size_t(W) + std::size_t(lo)) * n;
            } else {
                picture.read(sy, lo, hi, segment.data());
                src = segment.data();
            }
            for (int c = 0; c < iw; ++c) {
                const int sx = std::clamp(input.x0 + c, lo, hi - 1);
                std::copy_n(src + std::size_t(sx - lo) * n, n, out.data() + (std::size_t(r) * std::size_t(iw) + std::size_t(c)) * n);
            }
        }
        return out;
    };
    struct Made {
        Box band;
        std::vector<float> data;
    };
    std::vector<float> cover;
    for (int ty = ty0; ty <= ty1; ++ty) {
        std::vector<Made> made;
        for (int tx = tx0; tx <= tx1; tx += cut.across) {
            const Box band = intersect(Box{tx * kTile, ty * kTile, std::min(tx + cut.across, tx1 + 1) * kTile, (ty + 1) * kTile}, area);
            if (band.empty()) continue;
            if (stop.stop_requested()) throw Cancelled();
            const Box input = inflate(band, ov);
            const int iw = input.x1 - input.x0, ih = input.y1 - input.y0;
            std::vector<float> data = fetch(input);
            const std::vector<float> original = data;
            Json p = given;
            p["_image"] = Json{{"x", input.x0},
                               {"y", input.y0},
                               {"width", iw},
                               {"height", ih},
                               {"overlap", ov},
                               {"canvasWidth", W},
                               {"canvasHeight", H},
                               {"mode", mode_name(picture.mode)},
                               {"depth", picture.depth},
                               {"alpha", picture.alpha},
                               {"channels", n}};
            work(data, iw, ih, p);
            if (data.size() != original.size()) throw core::Error("plugin_runner", "the plugin returned a band of another size");
            // the band kept (the margins dropped), cleaned, laid over the picture by the coverage
            const int bw = band.x1 - band.x0, bh = band.y1 - band.y0;
            Made m{band, std::vector<float>(std::size_t(bw) * std::size_t(bh) * n)};
            cover.assign(std::size_t(bw), 1.0f);
            for (int y = band.y0; y < band.y1; ++y) {
                if (picture.coverage) picture.coverage(y, band.x0, band.x1, cover.data());
                for (int x = band.x0; x < band.x1; ++x) {
                    const float k = std::clamp(cover[std::size_t(x - band.x0)], 0.0f, 1.0f);
                    const std::size_t si = (std::size_t(y - input.y0) * std::size_t(iw) + std::size_t(x - input.x0)) * n;
                    float* out = m.data.data() + (std::size_t(y - band.y0) * std::size_t(bw) + std::size_t(x - band.x0)) * n;
                    for (unsigned c = 0; c < n; ++c) {
                        const float o = original[si + c];
                        float v = data[si + c];
                        if (!std::isfinite(v)) v = o;
                        if (int_depth || (picture.alpha && c == n - 1)) v = std::clamp(v, 0.0f, 1.0f);
                        out[c] = o + (v - o) * k;
                    }
                }
            }
            made.push_back(std::move(m));
        }
        // the rows the next tile row's margins reach in this one, kept as they are before this row is written
        if (ov > 0) {
            const int below = (ty + 1) * kTile;
            strip_y0 = std::clamp(below - ov, 0, H);
            strip_rows = std::clamp(below, 0, H) - strip_y0;
            strip.resize(std::size_t(std::max(0, strip_rows)) * std::size_t(W) * n);
            for (int r = 0; r < strip_rows; ++r) picture.read(strip_y0 + r, 0, W, strip.data() + std::size_t(r) * std::size_t(W) * n);
        }
        for (const Made& m : made) {
            const int bw = m.band.x1 - m.band.x0;
            for (int y = m.band.y0; y < m.band.y1; ++y)
                picture.write(y, m.band.x0, m.band.x1, m.data.data() + std::size_t(y - m.band.y0) * std::size_t(bw) * n);
        }
    }
}

}  // namespace genko::render::plugin_bands
