// PC4 節 D (SPEC.md COLOR-08): the engine of フィルター's kinds (render/fx.hpp) — upstream's apply_tiled (algo/src/lib.rs
// 833-966) with Genko's three ways, its scale and its imaging budget, and the shared pieces of the kernels (image.rs,
// blur.rs: conv_sep). PhotoCraft v0.2.0, MIT: native/third_party/photocraft/NOTICE.md.

#include "render/fx.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "render/image_ops.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::fx {

using core::Json;

// --- rectangles and windows ------------------------------------------------------------------------------------------

Rect Rect::intersect(const Rect& o) const {
    Rect out{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    if (out.empty()) return Rect{};
    return out;
}

Rect Rect::unite(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return Rect{std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}

namespace {

int rem_euclid(int a, int b) {
    const int r = a % b;
    return r < 0 ? r + b : r;
}

}  // namespace

float Window::get_edge(int x, int y, std::size_t c, Edge edge, const Rect& area) const {
    if (area.contains(x, y)) return get(x, y, c);
    switch (edge) {
        case Edge::Transparent: return 0.0f;
        case Edge::Repeat: return get(std::clamp(x, area.x0, area.x1 - 1), std::clamp(y, area.y0, area.y1 - 1), c);
        case Edge::Wrap: {
            const int w = std::max(1, area.width()), h = std::max(1, area.height());
            return get(area.x0 + rem_euclid(x - area.x0, w), area.y0 + rem_euclid(y - area.y0, h), c);
        }
    }
    return 0.0f;
}

void Window::sample(float x, float y, Edge edge, const Rect& area, bool alpha, float* out) const {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const float bx = std::floor(fx), by = std::floor(fy);
    const float ax = fx - bx, ay = fy - by;
    const int x0 = static_cast<int>(bx), y0 = static_cast<int>(by);
    const std::size_t n = ch;
    for (std::size_t c = 0; c < n; ++c) out[c] = 0.0f;
    float acc_a = 0.0f;
    const struct {
        int dx, dy;
        float w;
    } corners[4] = {{0, 0, (1.0f - ax) * (1.0f - ay)}, {1, 0, ax * (1.0f - ay)}, {0, 1, (1.0f - ax) * ay}, {1, 1, ax * ay}};
    for (const auto& k : corners) {
        if (k.w <= 0.0f) continue;
        const float a = alpha ? get_edge(x0 + k.dx, y0 + k.dy, n - 1, edge, area) : 1.0f;
        const std::size_t colours = alpha ? n - 1 : n;
        for (std::size_t c = 0; c < colours; ++c) out[c] += get_edge(x0 + k.dx, y0 + k.dy, c, edge, area) * a * k.w;
        acc_a += a * k.w;
    }
    if (alpha) {
        if (acc_a > 0.0f)
            for (std::size_t c = 0; c + 1 < n; ++c) out[c] /= acc_a;
        out[n - 1] = acc_a;
    }
}

std::vector<float> Window::crop(const Rect& out) const {
    std::vector<float> v;
    v.reserve(out.area() * ch);
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x)
            for (std::size_t c = 0; c < ch; ++c) v.push_back(get(x, y, c));
    return v;
}

// --- the shared pieces of the kernels --------------------------------------------------------------------------------

void premultiply(std::vector<float>& data, std::size_t ch, bool alpha) {
    if (!alpha || ch == 0) return;
    for (std::size_t i = 0; i + ch <= data.size(); i += ch) {
        const float a = data[i + ch - 1];
        for (std::size_t c = 0; c + 1 < ch; ++c) data[i + c] *= a;
    }
}

void unpremultiply(std::vector<float>& data, std::size_t ch, bool alpha) {
    if (!alpha || ch == 0) return;
    for (std::size_t i = 0; i + ch <= data.size(); i += ch) {
        const float a = data[i + ch - 1];
        for (std::size_t c = 0; c + 1 < ch; ++c) data[i + c] = a > 1e-7f ? data[i + c] / a : 0.0f;
    }
}

std::vector<float> gaussian_kernel(float sigma) {
    if (sigma < 0.05f) return {1.0f};
    const int r = static_cast<int>(std::ceil(sigma * 3.0f));
    std::vector<float> k;
    k.reserve(static_cast<std::size_t>(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) {
        const float v = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
        k.push_back(v);
        sum += v;
    }
    for (float& v : k) v /= sum;
    return k;
}

std::vector<float> conv_sep(const Window& src, const Rect& out, const std::vector<float>& kx, const std::vector<float>& ky, bool alpha) {
    const std::size_t n = src.ch;
    const int rx = static_cast<int>(kx.size() / 2), ry = static_cast<int>(ky.size() / 2);
    const std::size_t ow = static_cast<std::size_t>(out.width()), oh = static_cast<std::size_t>(out.height());
    // (a premultiplied copy of what it reads — the output grown by the kernels, cut to the window: past it, its edge pixel)
    const Rect win = Rect{out.x0 - rx, out.y0 - ry, out.x1 + rx, out.y1 + ry}.intersect(src.rect);
    if (win.empty()) return std::vector<float>(out.area() * n, 0.0f);
    const std::size_t ww = static_cast<std::size_t>(win.width()), wh = static_cast<std::size_t>(win.height());
    std::vector<float> p(ww * wh * n, 0.0f);
    for (int y = win.y0; y < win.y1; ++y) {
        const float* from = src.px(win.x0, y);
        std::copy(from, from + ww * n, p.begin() + static_cast<std::ptrdiff_t>(std::size_t(y - win.y0) * ww * n));
    }
    premultiply(p, n, alpha);
    // (along the rows: the output's columns of each of the window's rows)
    std::vector<float> tmp(ow * wh * n, 0.0f);
    for (std::size_t ty = 0; ty < wh; ++ty) {
        const float* row = p.data() + ty * ww * n;
        float* dst = tmp.data() + ty * ow * n;
        for (std::size_t ox = 0; ox < ow; ++ox) {
            float* d = dst + ox * n;
            const int x = out.x0 + static_cast<int>(ox) - rx;
            for (std::size_t i = 0; i < kx.size(); ++i) {
                const std::size_t sx = static_cast<std::size_t>(std::clamp(x + static_cast<int>(i), win.x0, win.x1 - 1) - win.x0);
                const float* s = row + sx * n;
                for (std::size_t c = 0; c < n; ++c) d[c] += s[c] * kx[i];
            }
        }
    }
    // (along the columns)
    std::vector<float> res(ow * oh * n, 0.0f);
    for (std::size_t oy = 0; oy < oh; ++oy) {
        float* d = res.data() + oy * ow * n;
        const int y = out.y0 + static_cast<int>(oy) - ry;
        for (std::size_t i = 0; i < ky.size(); ++i) {
            const std::size_t sy = static_cast<std::size_t>(std::clamp(y + static_cast<int>(i), win.y0, win.y1 - 1) - win.y0);
            const float* s = tmp.data() + sy * ow * n;
            const float kv = ky[i];
            for (std::size_t j = 0; j < ow * n; ++j) d[j] += s[j] * kv;
        }
    }
    unpremultiply(res, n, alpha);
    return res;
}

// --- the kernels -----------------------------------------------------------------------------------------------------

Kernels render_kernels() { return {}; }        // (D4)
Kernels blur_gallery_kernels() { return {}; }  // (D4)
Kernels camera_raw_kernels() { return {}; }    // (D5)

const Kernels& kernels() {
    static const Kernels all = [] {
        Kernels out;
        for (auto group : {blur_kernels, sharpen_kernels, noise_kernels, other_kernels, video_kernels, distort_kernels, pixelate_kernels,
                           stylize_kernels, gallery_kernels, render_kernels, blur_gallery_kernels, camera_raw_kernels, lens_kernels}) {
            for (Kernel& each : group()) out.push_back(std::move(each));
        }
        return out;
    }();
    return all;
}

const Kernel* kernel(std::string_view kind) {
    for (const Kernel& each : kernels()) {
        if (each.kind == kind) return &each;
    }
    return nullptr;
}

// --- the three ways' surfaces -----------------------------------------------------------------------------------------

namespace {

Space space_of(core::InkSpace space) {
    switch (space) {
        case core::InkSpace::Cmyk: return Space::Cmyk;
        case core::InkSpace::Lab: return Space::Lab;
        case core::InkSpace::Gray: return Space::Gray;
        case core::InkSpace::Multi: return Space::Multi;
        default: return Space::Rgb;
    }
}

[[noreturn]] void not_finite() { throw core::Error("value", "the result exceeds the finite working range"); }

// The box of the pixels `shows(x, y)` says show.
template <typename Shows>
Rect box_of(std::uint32_t w, std::uint32_t h, Shows&& shows) {
    int x0 = int(w), y0 = int(h), x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            if (!shows(x, y)) continue;
            x0 = std::min(x0, int(x));
            x1 = std::max(x1, int(x) + 1);
            y0 = std::min(y0, int(y));
            y1 = std::max(y1, int(y) + 1);
        }
    return Rect{x0, y0, x1, y1}.intersect(Rect{0, 0, int(w), int(h)});
}

}  // namespace

// An 8-bit layer's RGBA (its bytes / 255).
class EightSurface : public Surface {
public:
    explicit EightSurface(const Image& rgba) {
        const Image picture = rgba.mode() == "RGBA" ? rgba : rgba.convert("RGBA");
        width_ = static_cast<std::uint32_t>(picture.width());
        height_ = static_cast<std::uint32_t>(picture.height());
        src_ = picture.tobytes();
        out_ = src_;
    }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }
    unsigned colours() const override { return 3; }
    Space space() const override { return Space::Rgb; }
    unsigned levels() const override { return 255; }
    void read(const Rect& r, float* out) const override {
        for (int y = r.y0; y < r.y1; ++y) {
            const auto* q = reinterpret_cast<const unsigned char*>(src_.data() + (std::size_t(y) * width_ + std::size_t(r.x0)) * 4);
            for (std::size_t k = 0; k < std::size_t(r.width()) * 4; ++k) *out++ = q[k] / 255.0f;
        }
    }
    void write(const Rect& r, const float* in) override {
        for (int y = r.y0; y < r.y1; ++y) {
            char* q = out_.data() + (std::size_t(y) * width_ + std::size_t(r.x0)) * 4;
            for (std::size_t k = 0; k < std::size_t(r.width()) * 4; ++k) {
                const float v = *in++;
                q[k] = static_cast<char>(std::lround((std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f) * 255.0f));
            }
        }
    }
    Rect content() const override {
        return box_of(width_, height_, [this](std::uint32_t x, std::uint32_t y) { return src_[(std::size_t(y) * width_ + x) * 4 + 3] != 0; });
    }
    Image taken() { return Image::frombytes("RGBA", Size{static_cast<int>(width_), static_cast<int>(height_)}, out_); }

private:
    std::uint32_t width_ = 0, height_ = 0;
    std::string src_, out_;
};

// A precise layer's samples (GKCR: 16 bytes of header, then RGBA samples little-endian, u16 or f32).
class PreciseSurface : public Surface {
public:
    explicit PreciseSurface(const std::string& raster) : src_(raster), out_(raster) {
        const core::ColorRasterView view(raster);  // (checked)
        width_ = view.width();
        height_ = view.height();
        bytes_ = static_cast<unsigned char>(raster[5]);
    }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }
    unsigned colours() const override { return 3; }
    Space space() const override { return Space::Rgb; }
    void read(const Rect& r, float* out) const override {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
                for (unsigned c = 0; c < 4; ++c) *out++ = sample(std::size_t(y) * width_ + std::size_t(x), c);
    }
    void write(const Rect& r, const float* in) override {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const std::size_t i = std::size_t(y) * width_ + std::size_t(x);
                for (unsigned c = 0; c < 4; ++c) put(i, c, *in++);
            }
    }
    Rect content() const override {
        double most = 0, low = 0;
        const Rect box = box_of(width_, height_, [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t i = std::size_t(y) * width_ + x;
            if (!(sample(i, 3) > 0.0f)) return false;
            if (bytes_ == 4)
                for (unsigned c = 0; c < 3; ++c) {
                    const double v = sample(i, c);
                    most = std::max(most, std::fabs(v));
                    low = std::min(low, v);
                }
            return true;
        });
        largest_ = std::max(1.0, most);
        least_ = low;
        return box;
    }
    double largest() const override { return largest_; }
    unsigned levels() const override { return bytes_ == 2 ? 65535 : 0; }
    double least() const override { return least_; }
    std::string taken() { return std::move(out_); }
    // (its f32 samples, little-endian RGBA after the 16 bytes of the header, are the floats themselves on a little-endian
    // machine)
    const float* floats(int y) const override {
        if constexpr (std::endian::native != std::endian::little) return nullptr;
        if (bytes_ != 4 || reinterpret_cast<std::uintptr_t>(src_.data() + 16) % alignof(float) != 0) return nullptr;
        return reinterpret_cast<const float*>(src_.data() + 16) + std::size_t(y) * width_ * 4;
    }

private:
    float sample(std::size_t i, unsigned c) const {
        const auto* at = reinterpret_cast<const unsigned char*>(src_.data() + 16 + (i * 4 + c) * bytes_);
        if (bytes_ == 2) return static_cast<float>((at[0] | (at[1] << 8)) / 65535.0);
        std::uint32_t bits = 0;
        for (unsigned k = 0; k < 4; ++k) bits |= std::uint32_t(at[k]) << (8 * k);
        return std::bit_cast<float>(bits);
    }
    void put(std::size_t i, unsigned c, float v) {
        if (!std::isfinite(v)) not_finite();
        std::uint32_t bits = 0;
        if (bytes_ == 2) {
            bits = static_cast<std::uint32_t>(std::lround(std::clamp(static_cast<double>(v), 0.0, 1.0) * 65535.0));
        } else {
            bits = std::bit_cast<std::uint32_t>(c == 3 ? std::clamp(v, 0.0f, 1.0f) : v);
        }
        char* at = out_.data() + 16 + (i * 4 + c) * bytes_;
        for (unsigned k = 0; k < bytes_; ++k) at[k] = static_cast<char>((bits >> (8 * k)) & 255);
    }
    std::string_view src_;
    std::string out_;
    std::uint32_t width_ = 0, height_ = 0;
    unsigned bytes_ = 2;
    mutable double largest_ = 1.0;
    mutable double least_ = 0.0;
};

// An ink layer's pixels as the ink canvas's values (CMYK and a multichannel page's plates their ink 0..1, Lab L/100 and
// (a, b + 128)/255, grey its lightness; 32 bits: the values themselves), written back as image_ops::to_inks writes them.
class InkSurface : public Surface {
public:
    InkSurface(const core::InkPixels& inks, const core::PageColor& color)
        : src_(inks), out_(inks), color_(color),
          blank_(core::blank_ink_pixels(inks.space, inks.depth, 1, 1, inks.space == core::InkSpace::Multi ? inks.planes : 0)) {
        if (color.space != inks.space || color.depth != inks.depth) throw core::OpError("the ink pixels are not in the page's colour mode");
    }
    std::uint32_t width() const override { return src_.width; }
    std::uint32_t height() const override { return src_.height; }
    unsigned colours() const override { return src_.colours(); }
    Space space() const override { return space_of(src_.space); }
    const core::PageColor* ink() const override { return &color_; }
    void read(const Rect& r, float* out) const override {
        const unsigned n = src_.colours();
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const std::size_t i = std::size_t(y) * src_.width + std::size_t(x);
                for (unsigned c = 0; c < n; ++c) *out++ = canvas(i, c);
                *out++ = src_.has_alpha() ? static_cast<float>(src_.value(i, n)) : 1.0f;
            }
    }
    void write(const Rect& r, const float* in) override {
        const unsigned n = src_.colours();
        const double most = out_.max();
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x, in += n + 1) {
                const std::size_t i = std::size_t(y) * src_.width + std::size_t(x);
                for (unsigned c = 0; c <= n; ++c) {
                    if (!std::isfinite(in[c])) not_finite();
                }
                const float a = src_.has_alpha() ? in[n] : 1.0f;
                if (out_.is_float()) {
                    if (src_.has_alpha() && !(a > 0)) {
                        out_.copy_pixel(i, blank_, 0);
                        continue;
                    }
                    for (unsigned c = 0; c < n; ++c) out_.set(i, c, in[c]);
                    if (src_.has_alpha()) out_.set(i, n, a);
                    continue;
                }
                const auto alpha = static_cast<std::uint16_t>(std::lround(std::clamp(static_cast<double>(a), 0.0, 1.0) * most));
                if (src_.has_alpha() && alpha == 0) {
                    out_.copy_pixel(i, blank_, 0);
                    continue;
                }
                std::uint16_t* s = out_.samples.data() + i * out_.channels();
                for (unsigned c = 0; c < n; ++c)
                    s[c] = ink::encode_sample(src_.space, c, from_canvas(src_.space, c, std::clamp(static_cast<double>(in[c]), 0.0, 1.0)), src_.depth);
                if (src_.has_alpha()) s[n] = alpha;
            }
    }
    Rect content() const override {
        const unsigned n = src_.colours();
        double most = 0, low = 0;
        const Rect box = box_of(src_.width, src_.height, [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t i = std::size_t(y) * src_.width + x;
            if (src_.has_alpha() && !(src_.value(i, n) > 0)) return false;
            if (src_.is_float())
                for (unsigned c = 0; c < n; ++c) {
                    const double v = canvas(i, c);
                    most = std::max(most, std::fabs(v));
                    low = std::min(low, v);
                }
            return true;
        });
        largest_ = std::max(1.0, most);
        least_ = low;
        return box;
    }
    double largest() const override { return largest_; }
    // (an ink canvas's values of whole samples: n / 255 or n / 65535 — a grey's 1 − n / most, Lab's L and a, b within a
    // rounding of it)
    unsigned levels() const override { return src_.is_float() ? 0 : src_.depth == 8 ? 255 : 65535; }
    double least() const override { return least_; }
    std::string taken() { return core::encode_ink_raster(out_); }

private:
    float canvas(std::size_t i, unsigned c) const {
        if (src_.is_float()) return static_cast<float>(src_.value(i, c));
        const std::uint16_t s = src_.samples[i * src_.channels() + c];
        return static_cast<float>(to_canvas(src_.space, c, ink::decode_sample(src_.space, c, s, src_.depth)));
    }
    const core::InkPixels& src_;
    core::InkPixels out_;
    core::PageColor color_;
    core::InkPixels blank_;
    mutable double largest_ = 1.0;
    mutable double least_ = 0.0;
};

std::unique_ptr<Surface> eight_surface(const Image& rgba) { return std::make_unique<EightSurface>(rgba); }
std::unique_ptr<Surface> precise_surface(const std::string& raster) { return std::make_unique<PreciseSurface>(raster); }
std::unique_ptr<Surface> ink_surface(const core::InkPixels& inks, const core::PageColor& color) { return std::make_unique<InkSurface>(inks, color); }
Image take_image(Surface& surface) {
    auto* eight = dynamic_cast<EightSurface*>(&surface);
    if (eight == nullptr) throw std::logic_error("fx::take_image: not an 8-bit surface");
    return eight->taken();
}
std::string take_bytes(Surface& surface) {
    if (auto* precise = dynamic_cast<PreciseSurface*>(&surface)) return precise->taken();
    if (auto* inks = dynamic_cast<InkSurface*>(&surface)) return inks->taken();
    throw std::logic_error("fx::take_bytes: an 8-bit surface");
}

// --- the run ---------------------------------------------------------------------------------------------------------

namespace {

thread_local std::atomic<double>* g_progress = nullptr;
thread_local const PlaceScope* g_place = nullptr;
thread_local Json* g_notes = nullptr;  // (NoteScope, D6)

// Upstream's auto_tile: results do not depend on the tiling, so wide halos take bigger tiles (the margin read again
// kept below about twice the tile).
int auto_tile(const Halo& halo) {
    constexpr int kTile = 256;
    if (halo.bounds) return kTile;
    return std::min(2048, std::max(kTile, (2 * halo.radius + 63) / 64 * 64));
}

// The box of a selection's pixels that it covers at all.
Rect cover_box(const std::string& cover, std::uint32_t w, std::uint32_t h) {
    int x0 = static_cast<int>(w), y0 = static_cast<int>(h), x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        const char* row = cover.data() + std::size_t(y) * w;
        for (std::uint32_t x = 0; x < w; ++x) {
            if (row[x] == 0) continue;
            x0 = std::min(x0, int(x));
            x1 = std::max(x1, int(x) + 1);
            y0 = std::min(y0, int(y));
            y1 = std::max(y1, int(y) + 1);
        }
    }
    return Rect{x0, y0, x1, y1}.intersect(Rect{0, 0, int(w), int(h)});
}

// How a run turns the surface's values into the kernel's samples and back.
struct Way {
    bool meaning = false;    // the colours the inks mean (rgb), back by the page's rules
    bool lightness = false;  // CMYK and multichannel plates as 1 − ink
    bool lab_lightness = false;  // a Lab page's lightness alone (its L, the alpha): a and b as they were
    unsigned colours = 3;    // the surface's
    unsigned kernel_colours = 3;
    core::InkSpace space = core::InkSpace::Cmyk;
};

// `count` pixels of surface values (`n` colours and the alpha) as the kernel's samples, into `out`.
void to_kernel(const Way& way, const float* in, float* out, std::size_t count, const ink::Rules* rules) {
    const unsigned n = way.colours;
    if (way.meaning) {
        std::vector<double> inks(count * n), rgb(count * 3);
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(way.space, c, in[i * (n + 1) + c]);
        rules->meaning(inks.data(), rgb.data(), count);
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = static_cast<float>(rgb[i * 3 + c]);
            out[i * 4 + 3] = in[i * (n + 1) + n];
        }
        return;
    }
    if (way.lab_lightness) {
        for (std::size_t i = 0; i < count; ++i) {
            out[i * 2] = in[i * (n + 1)];
            out[i * 2 + 1] = in[i * (n + 1) + n];
        }
        return;
    }
    std::memcpy(out, in, count * (n + 1) * sizeof(float));
    if (way.lightness) {
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < n; ++c) out[i * (n + 1) + c] = 1.0f - out[i * (n + 1) + c];
    }
}

// The kernel's samples of `count` pixels back as surface values (`original`: the surface's values there, for the rules).
void from_kernel(const Way& way, const float* in, const float* original, float* out, std::size_t count, const ink::Rules* rules) {
    const unsigned n = way.colours;
    if (way.meaning) {
        std::vector<double> inks(count * n), changed(count * n);
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(way.space, c, original[i * (n + 1) + c]);
        std::size_t k = 0;  // (asked for each pixel once, in their order)
        rules->correct(inks.data(), changed.data(), count, [&](const std::array<double, 3>&) {
            const float* q = in + (k++) * 4;
            return std::array<double, 3>{q[0], q[1], q[2]};
        });
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < n; ++c) out[i * (n + 1) + c] = static_cast<float>(to_canvas(way.space, c, changed[i * n + c]));
            out[i * (n + 1) + n] = in[i * 4 + 3];
        }
        return;
    }
    if (way.lab_lightness) {
        for (std::size_t i = 0; i < count; ++i) {
            std::copy(original + i * (n + 1), original + (i + 1) * (n + 1), out + i * (n + 1));
            out[i * (n + 1)] = in[i * 2];
            out[i * (n + 1) + n] = in[i * 2 + 1];
        }
        return;
    }
    std::memcpy(out, in, count * (n + 1) * sizeof(float));
    if (way.lightness) {
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < n; ++c) out[i * (n + 1) + c] = 1.0f - out[i * (n + 1) + c];
    }
}

// The window `r` of the surface cut to its grid, as the kernel's samples: a kernel reads past the grid's edge as its edge
// pixel (Window::at — upstream's read_clamped: a filter does not fade a layer that fills the page at its edge), so a
// window holds no more than the layer, whatever the halo.
Window read_window(const Surface& surface, const Rect& r, const Way& way, const ink::Rules* rules) {
    const Rect grid{0, 0, int(surface.width()), int(surface.height())};
    const Rect inner = r.intersect(grid);
    const std::size_t sch = way.colours + 1, kch = way.kernel_colours + 1;
    Window out(inner, kch);
    if (inner.empty()) return out;
    std::vector<float> values(inner.area() * sch);
    surface.read(inner, values.data());
    to_kernel(way, values.data(), out.data.data(), inner.area(), rules);
    return out;
}

// The rows of a band of read_window (a whole kind's one window): about kReadBandPixels pixels, one row at least.
int band_rows(const Rect& inner) { return std::max(1, static_cast<int>(kReadBandPixels / static_cast<std::size_t>(std::max(1, inner.width())))); }

// What a band's reading holds besides the window (the surface's values; the inks and colours the rules work in, on the
// way of the colours the inks mean).
std::uint64_t band_bytes(const Rect& inner, const Way& way) {
    if (inner.empty()) return 0;
    const std::uint64_t px = std::uint64_t(std::min(band_rows(inner), inner.height())) * std::uint64_t(inner.width());
    return px * ((way.colours + 1) * sizeof(float) + (way.meaning ? (way.colours + 3) * sizeof(double) : 0));
}

// read_window for a whole kind's base box (D2's review P2-1): read in bands of rows straight into the window, so what the
// reading holds besides the window is a band's (band_bytes), not another window of the whole.
Window read_window_in_bands(const Surface& surface, const Rect& r, const Way& way, const ink::Rules* rules) {
    const Rect grid{0, 0, int(surface.width()), int(surface.height())};
    const Rect inner = r.intersect(grid);
    const std::size_t sch = way.colours + 1, kch = way.kernel_colours + 1;
    Window out(inner, kch);
    if (inner.empty()) return out;
    const int rows = band_rows(inner);
    std::vector<float> values(std::size_t(std::min(rows, inner.height())) * std::size_t(inner.width()) * sch);
    for (int y = inner.y0; y < inner.y1; y += rows) {
        const Rect band{inner.x0, y, inner.x1, std::min(y + rows, inner.y1)};
        surface.read(band, values.data());
        to_kernel(way, values.data(), out.data.data() + std::size_t(y - inner.y0) * std::size_t(inner.width()) * kch, band.area(), rules);
    }
    return out;
}

}  // namespace

ProgressScope::ProgressScope(std::atomic<double>& done) : before_(g_progress) {
    done.store(0.0);
    g_progress = &done;
}
ProgressScope::~ProgressScope() { g_progress = before_; }
void ProgressScope::put(double done) {
    if (g_progress != nullptr) g_progress->store(std::clamp(done, 0.0, 1.0));
}

// (D6) What the kinds resolve as they run, for the op's report (NoteScope)
NoteScope::NoteScope(Json& into) : before_(g_notes) { g_notes = &into; }
NoteScope::~NoteScope() { g_notes = before_; }
void NoteScope::put(std::string_view key, Json value) {
    if (g_notes == nullptr) return;
    if (!g_notes->is_object()) *g_notes = Json::object();
    (*g_notes)[std::string(key)] = std::move(value);
}

PlaceScope::PlaceScope(int x, int y, const Rect& bounds) : before_(g_place), x_(x), y_(y), bounds_(bounds) { g_place = this; }
PlaceScope::~PlaceScope() { g_place = before_; }
const PlaceScope* PlaceScope::current() { return g_place; }

void apply(Surface& surface, const Kernel& kernel, const Json& params, core::FilterInks inks, const Rect& bounds_in, const std::string* cover,
           double s, const Rect* limit, const Tiling& tiling, const Reads& reads) {
    const std::uint32_t w = surface.width(), h = surface.height();
    const Rect grid{0, 0, int(w), int(h)};
    if (grid.empty()) return;
    if (cover != nullptr && cover->size() != std::size_t(w) * h) throw std::logic_error("fx::apply: the coverage is not the grid's");
    if (reads.within != nullptr && reads.within->size() != std::size_t(w) * h) throw std::logic_error("fx::apply: the coverage is not the grid's");
    if (reads.plane != nullptr && reads.plane->size() != std::size_t(w) * h) throw std::logic_error("fx::apply: the plane is not the grid's");
    Way way;
    way.colours = surface.colours();
    // (D2: a screen — カラーハーフトーン, メゾティント — on each plate; a Lab page's L, a and b are no plates: the colours they mean)
    way.meaning = (inks == core::FilterInks::Meaning || (inks == core::FilterInks::Halftone && surface.space() == Space::Lab)) && surface.ink() != nullptr;
    way.lightness = (inks == core::FilterInks::Lightness || inks == core::FilterInks::LightnessAndColour) &&
                    (surface.space() == Space::Cmyk || surface.space() == Space::Multi);
    way.lab_lightness = inks == core::FilterInks::Lightness && surface.space() == Space::Lab;
    way.kernel_colours = way.meaning ? 3 : way.lab_lightness ? 1 : way.colours;
    if (surface.ink() != nullptr) way.space = surface.ink()->space;
    Ctx ctx;
    ctx.bounds = bounds_in.intersect(grid);
    if (ctx.bounds.empty()) ctx.bounds = grid;
    const Rect content = surface.content();
    if (kernel.shown_frame) {  // (D6: the frame of the picture — what shows —, a selection only keeping the result to it)
        if (content.empty() && PlaceScope::current() == nullptr) return;
        ctx.bounds = content;
    }
    if (const PlaceScope* place = PlaceScope::current()) {  // (D3: a part of the grid — its origin, the grid's base box there)
        ctx.x0 = place->x();
        ctx.y0 = place->y();
        const Rect& b = place->bounds();
        ctx.bounds = Rect{b.x0 - ctx.x0, b.y0 - ctx.y0, b.x1 - ctx.x0, b.y1 - ctx.y0};
    }
    ctx.space = way.meaning ? Space::Rgb : way.lab_lightness ? Space::Gray : surface.space();
    ctx.colours = way.kernel_colours;
    ctx.alpha = true;
    ctx.s = s > 0 ? s : 1.0;
    ctx.cover = cover != nullptr ? cover : reads.within;
    ctx.grid_width = w;
    ctx.plane = reads.plane;
    ctx.map = reads.map;
    ctx.ink = surface.ink();  // (D6)
    const Halo halo = kernel.halo ? kernel.halo(params, ctx) : Halo::margin(0);

    // the part worked: as far as the halo reaches from what shows (upstream's output_area), the base box for a whole kind
    // or a render; within the selection's box
    Rect area;
    ctx.largest = surface.largest();
    ctx.least = surface.least();
    ctx.levels = way.meaning ? 0 : surface.levels();  // (the colours the inks mean: floats)
    if (limit != nullptr) {
        area = limit->intersect(grid);
    } else if (halo.bounds || kernel.fills) {
        area = ctx.bounds.intersect(grid);
    } else {
        area = content.empty() ? Rect{} : content.inflate(halo.radius).intersect(grid);
    }
    if (cover != nullptr) area = area.intersect(cover_box(*cover, w, h));
    if (kernel.shown_frame) area = area.intersect(ctx.bounds);  // (D6: within the frame)
    if (area.empty()) return;

    // the memory, held in the imaging budget before it is had: a whole kind's one window, then each thread's window (the
    // tile grown by the halo, cut to the grid) — its floats, their reading and a kernel's own work on it (whole numbers, as
    // the blurs' exact sums, and a float copy: three words a sample; and what the kernel says it takes a pixel) —, the
    // tile's result and the selection's mix. When not even one thread's fits, smaller tiles (their windows smaller; the
    // result the same whatever the tiling) before it is refused as too large.
    const std::size_t sch = way.colours + 1, kch = way.kernel_colours + 1;
    const Rect whole = halo.bounds ? ctx.bounds.intersect(grid).unite(area) : Rect{};
    // (D2: a 32-bit precise layer's base box of whole rows read in place — no float copy, nothing held for it)
    const float* in_place = halo.bounds && !way.meaning && !way.lightness && !way.lab_lightness && kch == sch && whole.x0 == 0 && whole.x1 == int(w)
                                ? surface.floats(whole.y0)
                                : nullptr;
    // (its reading: a band of rows at a time, held too — read_window_in_bands)
    const image_ops::Held shared_held(in_place != nullptr ? 0 : std::uint64_t(whole.area()) * kch * sizeof(float) + band_bytes(whole.intersect(grid), way));
    const auto per_thread_of = [&](int tile_side) {
        const std::uint64_t tile_px = std::uint64_t(tile_side) * tile_side;
        const std::uint64_t reach = std::uint64_t(tile_side) + 2 * std::uint64_t(halo.radius);
        const std::uint64_t window_px = halo.bounds ? 0 : std::min<std::uint64_t>(reach, w) * std::min<std::uint64_t>(reach, h);
        return window_px * (kch * (2 * sizeof(float) + sizeof(std::int64_t)) + sch * sizeof(float) + kernel.work_bytes) +
               tile_px * (kch + 3 * sch) * sizeof(float) + (way.meaning ? (window_px + tile_px) * (sch + 4) * sizeof(double) : 0);
    };
    std::vector<int> sides{tiling.tile > 0 ? tiling.tile : auto_tile(halo)};
    if (tiling.tile <= 0)
        for (const int smaller : {256, 128, 64})
            if (smaller < sides.back()) sides.push_back(smaller);
    int side = sides.front();
    std::optional<image_ops::Held> held;
    int threads = 1;
    for (std::size_t k = 0; k < sides.size() && !held; ++k) {
        side = sides[k];
        const std::uint64_t per_thread = per_thread_of(side);
        const std::uint64_t count = (std::uint64_t(area.width()) + side - 1) / side * ((std::uint64_t(area.height()) + side - 1) / side);
        int wanted = tiling.threads > 0 ? tiling.threads : band_threads();
        wanted = static_cast<int>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(std::uint64_t(wanted), count)));
        for (threads = wanted;; threads = threads / 2) {
            try {
                held.emplace(per_thread * std::uint64_t(threads));
                break;
            } catch (const core::OpError&) {
                if (threads <= 1) {  // (not even one window of these tiles: smaller ones, or too large)
                    if (k + 1 == sides.size()) throw;
                    break;
                }
            }
        }
    }
    std::vector<Rect> tiles;
    for (int y = area.y0; y < area.y1; y += side)
        for (int x = area.x0; x < area.x1; x += side) tiles.push_back(Rect{x, y, std::min(x + side, area.x1), std::min(y + side, area.y1)});

    std::optional<Window> shared;
    const auto rules_of = [&way, &surface]() -> std::unique_ptr<ink::Rules> {
        return way.meaning ? std::make_unique<ink::Rules>(*surface.ink()) : nullptr;
    };
    if (halo.bounds) {
        const auto rules = rules_of();
        if (in_place != nullptr) {
            shared.emplace();
            shared->rect = whole;
            shared->ch = kch;
            shared->view = in_place;
        } else {
            shared = read_window_in_bands(surface, whole, way, rules.get());
        }
    }
    // (a whole kind's work done once — 平均's mean —, its tiles given what it found)
    const Json found = kernel.once && shared ? kernel.once(params, *shared, ctx) : Json();
    const Json& given = kernel.once && shared ? found : params;

    const std::stop_token* stop = segment::current_stop();
    std::atomic<double>* progress = g_progress;
    std::atomic<std::size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_lock;
    const auto run_tile = [&](const Rect& t, const ink::Rules* rules) {
        std::optional<Window> own;
        if (!shared) own = read_window(surface, t.inflate(halo.radius), way, rules);
        const Window& src = shared ? *shared : *own;
        const std::vector<float> data = kernel.run(given, src, t, ctx);
        if (data.size() != t.area() * kch) throw std::logic_error("fx: the kernel of " + std::string(kernel.kind) + " gave a result of another size");
        std::vector<float> values(t.area() * sch), original;
        if (way.meaning || way.lab_lightness || cover != nullptr) {
            original.resize(t.area() * sch);
            surface.read(t, original.data());
        }
        from_kernel(way, data.data(), original.data(), values.data(), t.area(), rules);
        if (cover != nullptr) {  // (mixed with the layer as it was by the selection's coverage: upstream's apply_tiled)
            std::size_t i = 0;
            for (int y = t.y0; y < t.y1; ++y)
                for (int x = t.x0; x < t.x1; ++x, ++i) {
                    const float k = static_cast<unsigned char>((*cover)[std::size_t(y) * w + std::size_t(x)]) / 255.0f;
                    if (k >= 1.0f) continue;
                    for (std::size_t c = 0; c < sch; ++c) {
                        const float o = original[i * sch + c];
                        values[i * sch + c] = o + (values[i * sch + c] - o) * k;
                    }
                }
        }
        surface.write(t, values.data());
    };
    const auto work = [&]() noexcept {
        std::unique_ptr<ink::Rules> rules;
        try {
            rules = rules_of();
        } catch (...) {
            const std::lock_guard<std::mutex> guard(error_lock);
            if (!error) error = std::current_exception();
            failed = true;
            return;
        }
        for (std::size_t i = next.fetch_add(1); i < tiles.size(); i = next.fetch_add(1)) {
            if (failed.load() || (stop != nullptr && stop->stop_requested())) return;
            try {
                run_tile(tiles[i], rules.get());
            } catch (...) {
                const std::lock_guard<std::mutex> guard(error_lock);
                if (!error) error = std::current_exception();
                failed = true;
                return;
            }
            const std::size_t now = done.fetch_add(1) + 1;
            if (progress != nullptr) progress->store(static_cast<double>(now) / static_cast<double>(tiles.size()));
        }
    };
    const int granted = threads > 1 ? take_band_threads(threads - 1) : 0;
    std::vector<std::thread> workers;
    int started = 0;
    try {
        workers.reserve(static_cast<std::size_t>(granted));
        for (; started < granted; ++started) workers.emplace_back(work);
    } catch (...) {  // (no thread to be had: the tiles go to the threads there are, this one at least)
    }
    give_band_threads(granted - started);
    work();
    for (std::thread& t : workers) t.join();
    give_band_threads(started);
    if (error) std::rethrow_exception(error);
    if (stop != nullptr && stop->stop_requested()) throw Cancelled();
}

// --- box passes in whole numbers ----------------------------------------------------------------------------------------

std::array<int, 3> gauss_box_radii(float sigma) {
    const float n = 3.0f;
    const float w_ideal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = static_cast<int>(std::floor(w_ideal));
    if (wl % 2 == 0) wl -= 1;
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const float m_ideal = (12.0f * sigma * sigma - n * static_cast<float>(wl * wl) - 4.0f * n * static_cast<float>(wl) - 3.0f * n) / (-4.0f * static_cast<float>(wl) - 4.0f);
    const int m = static_cast<int>(std::clamp(std::round(m_ideal), 0.0f, n));
    std::array<int, 3> out{};
    for (int i = 0; i < 3; ++i) out[static_cast<std::size_t>(i)] = i < m ? (wl - 1) / 2 : (wu - 1) / 2;
    return out;
}

void whole_box_line(std::int64_t* first, std::size_t len, std::size_t stride, std::size_t n, int r, std::vector<std::int64_t>& line) {
    if (r <= 0 || len == 0) return;
    line.resize(len * n);
    for (std::size_t i = 0; i < len; ++i) std::copy(first + i * stride, first + i * stride + n, line.begin() + static_cast<std::ptrdiff_t>(i * n));
    const auto at = [&line, len, n](std::ptrdiff_t k, std::size_t c) { return line[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(k, 0, std::ptrdiff_t(len) - 1)) * n + c]; };
    const std::int64_t width = 2 * std::int64_t(r) + 1;
    const auto div_round = [width](std::int64_t a) { return a >= 0 ? (a + width / 2) / width : -((-a + width / 2) / width); };
    for (std::size_t c = 0; c < n; ++c) {
        std::int64_t acc = 0;
        for (std::ptrdiff_t k = -r; k <= r; ++k) acc += at(k, c);
        for (std::size_t x = 0; x < len; ++x) {
            first[x * stride + c] = div_round(acc);
            acc += at(std::ptrdiff_t(x) + r + 1, c) - at(std::ptrdiff_t(x) - r, c);
        }
    }
}

void whole_box_passes(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, const std::vector<int>& radii, double unit, double most) {
    if (w == 0 || h == 0 || std::all_of(radii.begin(), radii.end(), [](int r) { return r <= 0; })) return;
    std::vector<std::int64_t> q(buf.size()), line;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const double v = std::isfinite(buf[i]) ? static_cast<double>(buf[i]) : 0.0;
        q[i] = std::llround(std::clamp(v, -most, most) / unit);
    }
    for (const int r : radii)
        for (std::size_t y = 0; y < h; ++y) whole_box_line(q.data() + y * w * n, w, n, n, r, line);
    for (const int r : radii)
        for (std::size_t x = 0; x < w; ++x) whole_box_line(q.data() + x * n, h, w * n, n, r, line);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<float>(static_cast<double>(q[i]) * unit);
}

// --- how heavy ---------------------------------------------------------------------------------------------------------

double work(std::string_view kind, const Json& params, std::uint64_t pixels, double s, unsigned levels) {
    const Kernel* found = kernel(kind);
    if (found == nullptr) return 0;
    Ctx ctx;
    ctx.s = s > 0 ? s : 1.0;
    ctx.levels = levels;
    const double each = found->cost ? found->cost(params, ctx) : kDefaultCost;
    return (each + kPixelWork) * static_cast<double>(pixels) + (found->setup ? found->setup(params) : 0.0);
}

Grid layer_grid(const core::Page& page, const core::Layer& layer) {
    std::uint32_t w = 0, h = 0;
    // (the samples' grid: a 32-bit page's are floats — its precise and ink layers', a smart object drawn on it —, the rest
    // whole numbers of 255ths or 65535ths)
    const bool floats = (page.rgb && page.rgb->depth == 32) || (page.color && page.color->depth == 32);
    unsigned levels = floats ? 0 : 65535;
    try {
        if (layer.color_raster) {
            const core::ColorRasterView view(*layer.color_raster);
            w = view.width();
            h = view.height();
            levels = view.metadata("")["precision"] == "f32" ? 0 : 65535;
        } else if (layer.ink_raster) {
            const core::InkRasterHeader header = core::ink_raster_header(*layer.ink_raster);
            w = header.width;
            h = header.height;
            levels = header.depth == 32 ? 0 : header.depth == 8 ? 255 : 65535;
        }
    } catch (const std::exception&) {
        w = h = 0;
    }
    const bool deep = page.color.has_value() || (page.rgb && page.rgb->depth != 8);
    if ((w == 0 || h == 0) && !deep && layer.kind != core::LayerKind::Smart && page.spec.grid && *page.spec.grid == "finest") {
        // (PC4 F-b, image-grid@1: on an opened picture's page an 8-bit layer's pixels at their own grid, the op's surface;
        // a layer of none yet at the page's grid, raster::working_dpi — where filter_raster bakes it)
        try {
            if (layer.raster_png && !layer.raster_png->empty()) {
                const Size size = picture_size(*layer.raster_png);
                w = static_cast<std::uint32_t>(std::max(0, size.width));
                h = static_cast<std::uint32_t>(std::max(0, size.height));
            }
        } catch (const std::exception&) {
            w = h = 0;
        }
        if (w == 0 || h == 0) {
            const int dpi = raster::working_dpi(page);
            w = static_cast<std::uint32_t>(std::max(1, mm_to_px(page.spec.width_mm.value(), dpi)));
            h = static_cast<std::uint32_t>(std::max(1, mm_to_px(page.spec.height_mm.value(), dpi)));
        }
        levels = 255;
    }
    if (w == 0 || h == 0) {  // (an 8-bit layer: the page at the working 200 dpi; lines alone on a deep page: at its dpi)
        const double dpi = deep ? page.spec.dpi.value() : 200.0;
        w = static_cast<std::uint32_t>(std::max(1L, std::lround(page.spec.width_mm.value() / 25.4 * dpi)));
        h = static_cast<std::uint32_t>(std::max(1L, std::lround(page.spec.height_mm.value() / 25.4 * dpi)));
        if (!deep) levels = 255;
    }
    return Grid{w, h, image_ops::grid_scale(page, w, h), levels};
}

double layer_work(const core::Page& page, const core::Layer& layer, std::string_view kind, const Json& params) {
    const Grid grid = layer_grid(page, layer);
    return work(kind, params, std::uint64_t(grid.width) * grid.height, grid.s, grid.levels);
}

core::Document preview_book(const core::Document& doc, std::size_t at, const std::string& id, int most) {
    core::Document out = doc;
    if (at >= out.pages.size() || out.is_deferred(at) || most < 1) return out;
    const core::Page& page = out.page(at);
    std::size_t index = page.layers.size();
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == id) index = i;
    }
    if (index == page.layers.size()) return out;
    const core::Layer& layer = page.layers[index];
    const auto factor = [most](std::uint32_t w, std::uint32_t h) {
        return static_cast<std::uint32_t>((std::max(w, h) + std::uint32_t(most) - 1) / std::uint32_t(most));
    };
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        const std::uint32_t w = view.width(), h = view.height(), k = factor(w, h);
        if (k <= 1) return out;
        const std::uint32_t nw = (w + k - 1) / k, nh = (h + k - 1) / k;
        const bool floating = view.metadata("")["precision"] == "f32";
        std::string bytes = core::encode_color_pixels(nw, nh, floating ? "f32" : "u16", [&](std::size_t i) {
            const std::uint32_t x0 = std::uint32_t(i % nw) * k, y0 = std::uint32_t(i / nw) * k;
            std::array<double, 4> sum{};
            double n = 0;
            for (std::uint32_t y = y0; y < std::min(h, y0 + k); ++y)
                for (std::uint32_t x = x0; x < std::min(w, x0 + k); ++x) {
                    const auto p = view.pixel(std::size_t(y) * w + x);
                    for (unsigned c = 0; c < 3; ++c) sum[c] += p[c] * p[3];
                    sum[3] += p[3];
                    n += 1;
                }
            std::array<double, 4> v{0, 0, 0, n > 0 ? sum[3] / n : 0};
            for (unsigned c = 0; c < 3; ++c) {
                v[c] = sum[3] > 0 ? sum[c] / sum[3] : 0;
                if (!floating) v[c] = std::clamp(v[c], 0.0, 1.0);
            }
            v[3] = std::clamp(v[3], 0.0, 1.0);
            return v;
        });
        out.edit_page(at).layers[index].color_raster = std::make_shared<const std::string>(std::move(bytes));
        return out;
    }
    if (layer.ink_raster) {
        const core::InkRasterHeader header = core::ink_raster_header(*layer.ink_raster);
        const std::uint32_t k = factor(header.width, header.height);
        if (k <= 1) return out;
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        const Size size{static_cast<int>((header.width + k - 1) / k), static_cast<int>((header.height + k - 1) / k)};
        core::Layer& edited = out.edit_page(at).layers[index];
        edited.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(resize_inks(inks, size, Resample::Bicubic)));
        edited.ink_memo = {};
    }
    return out;
}

}  // namespace genko::render::fx
