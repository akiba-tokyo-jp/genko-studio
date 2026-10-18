#include "render/content_aware.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <optional>

#include "render/inpaint.hpp"
#include "render/poisson.hpp"

// PhotoCraft v0.2.0 crates/algo/src/content_aware.rs (lines 1-255), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::content_aware {

std::vector<float> rotation_level(std::string_view level) {
    if (level == "low") return {-10.0f, 10.0f};
    if (level == "medium") return {-20.0f, 20.0f};
    if (level == "high") return {-35.0f, 35.0f};
    if (level == "full") return {90.0f, 180.0f, 270.0f};
    return {};
}

float color_level(std::string_view level) {
    if (level == "none") return 0.0f;
    if (level == "high") return 0.65f;
    if (level == "very_high") return 0.9f;
    return 0.35f;
}

namespace {

// The source sampled bilinearly at (x, y); false when any of the four is not known.
bool sample(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& ok, float x, float y, float* out) {
    if (x < 0.0f || y < 0.0f || x > static_cast<float>(w - 1) || y > static_cast<float>(h - 1)) return false;
    const auto x0 = static_cast<std::size_t>(std::floor(x)), y0 = static_cast<std::size_t>(std::floor(y));
    const std::size_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    if (!ok[y0 * w + x0] || !ok[y0 * w + x1] || !ok[y1 * w + x0] || !ok[y1 * w + x1]) return false;
    for (std::size_t c = 0; c < ch; ++c) {
        const auto p = [&](std::size_t xx, std::size_t yy) { return img[(yy * w + xx) * ch + c]; };
        const float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
        const float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
        out[c] = top + (bot - top) * ty;
    }
    return true;
}

// A separable box blur of radius r (the edges repeated).
std::vector<float> box_blur(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& v, std::size_t r) {
    if (r == 0) return v;
    const auto pass = [&](const std::vector<float>& src, bool horizontal) {
        std::vector<float> dst(src.size(), 0.0f);
        const std::size_t n = horizontal ? h : w, m = horizontal ? w : h;
        const auto idx = [&](std::size_t line, std::size_t k) { return horizontal ? line * w + k : k * w + line; };
        const float norm = 1.0f / static_cast<float>(2 * r + 1);
        const long ir = static_cast<long>(r), im = static_cast<long>(m);
        for (std::size_t line = 0; line < n; ++line) {
            for (std::size_t c = 0; c < ch; ++c) {
                const auto at = [&](long k) { return src[idx(line, static_cast<std::size_t>(std::clamp(k, 0L, im - 1))) * ch + c]; };
                float acc = 0.0f;
                for (long k = -ir; k <= ir; ++k) acc += at(k);
                for (long k = 0; k < im; ++k) {
                    dst[idx(line, static_cast<std::size_t>(k)) * ch + c] = acc * norm;
                    acc += at(k + ir + 1) - at(k - ir);
                }
            }
        }
        return dst;
    };
    return pass(pass(v, true), false);
}

// The fill's low frequencies pulled toward the membrane of the hole's edge.
void adapt_colors(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole, std::vector<float>& out,
                  float k, bool unit_range) {
    const std::optional<inpaint::HoleBox> box = inpaint::hole_box(w, h, hole);
    if (!box) return;
    const std::size_t r = std::clamp<std::size_t>(std::max(box->x1 - box->x0, box->y1 - box->y0) / 6, 1, 32);
    const std::vector<float> membrane = poisson::membrane_fill(w, h, ch, img, hole);
    const std::vector<float> low = box_blur(w, h, ch, out, r);
    for (std::size_t i = 0; i < w * h; ++i) {
        if (!hole[i]) continue;
        for (std::size_t c = 0; c < ch; ++c) {
            const std::size_t j = i * ch + c;
            const float v = out[j] + k * (membrane[j] - low[j]);
            out[j] = unit_range ? std::clamp(v, 0.0f, 1.0f) : std::max(v, 0.0f);
        }
    }
}

}  // namespace

Variant variant(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& ok, float deg, float scale,
                bool mirror) {
    const float rad = -deg * std::numbers::pi_v<float> / 180.0f;
    const float s = std::sin(rad), c = std::cos(rad);
    // (the turned, scaled box's size)
    const float fw = static_cast<float>(w) * scale, fh = static_cast<float>(h) * scale;
    Variant out;
    out.w = std::max<std::size_t>(static_cast<std::size_t>(std::round(fw * std::fabs(c) + fh * std::fabs(s))), 1);
    out.h = std::max<std::size_t>(static_cast<std::size_t>(std::round(fw * std::fabs(s) + fh * std::fabs(c))), 1);
    const float icx = (static_cast<float>(w) - 1.0f) / 2.0f, icy = (static_cast<float>(h) - 1.0f) / 2.0f;
    const float ocx = (static_cast<float>(out.w) - 1.0f) / 2.0f, ocy = (static_cast<float>(out.h) - 1.0f) / 2.0f;
    out.img.assign(out.w * out.h * ch, 0.0f);
    out.ok.assign(out.w * out.h, false);
    std::vector<float> px(ch, 0.0f);
    for (std::size_t y = 0; y < out.h; ++y) {
        for (std::size_t x = 0; x < out.w; ++x) {
            const float dx = (static_cast<float>(x) - ocx) / scale, dy = (static_cast<float>(y) - ocy) / scale;
            float sx = icx + dx * c - dy * s;
            const float sy = icy + dx * s + dy * c;
            if (mirror) sx = (static_cast<float>(w) - 1.0f) - sx;
            if (sample(w, h, ch, img, ok, sx, sy, px.data())) {
                const std::size_t i = y * out.w + x;
                out.ok[i] = true;
                std::copy(px.begin(), px.end(), out.img.begin() + static_cast<std::ptrdiff_t>(i * ch));
            }
        }
    }
    return out;
}

std::vector<float> fill(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole,
                        const std::vector<bool>& source, const FillOptions& opts) {
    assert(img.size() == w * h * ch && hole.size() == w * h && source.size() == w * h);
    if (std::none_of(hole.begin(), hole.end(), [](bool b) { return b; })) return img;
    inpaint::CompleteParams params;
    params.seed = opts.seed;
    const std::size_t gap = 2 * params.patch_radius + 2;
    std::vector<bool> ok(w * h);
    for (std::size_t i = 0; i < w * h; ++i) ok[i] = !hole[i] && source[i];
    // the copies: the identity (which holds the hole) first, then the adaptations
    std::vector<Variant> extra;
    if (opts.mirror) extra.push_back(variant(w, h, ch, img, ok, 0.0f, 1.0f, true));
    for (const float deg : opts.rotations) extra.push_back(variant(w, h, ch, img, ok, deg, 1.0f, false));
    if (opts.scale) {
        extra.push_back(variant(w, h, ch, img, ok, 0.0f, 0.8f, false));
        extra.push_back(variant(w, h, ch, img, ok, 0.0f, 1.25f, false));
    }
    // the mosaic: the identity on the left, the copies in a column to its right
    std::size_t col_w = 0, col_h = 0;
    for (const Variant& v : extra) {
        col_w = std::max(col_w, v.w);
        col_h += v.h + gap;
    }
    const std::size_t mw = extra.empty() ? w : w + gap + col_w;
    const std::size_t mh = std::max(h, col_h);
    std::vector<float> mimg(mw * mh * ch, 0.0f);
    std::vector<bool> unknown(mw * mh, true);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t s = y * w + x, d = y * mw + x;
            std::copy_n(img.begin() + static_cast<std::ptrdiff_t>(s * ch), ch, mimg.begin() + static_cast<std::ptrdiff_t>(d * ch));
            unknown[d] = !ok[s];
        }
    }
    std::size_t oy = 0;
    for (const Variant& v : extra) {
        for (std::size_t y = 0; y < v.h; ++y) {
            for (std::size_t x = 0; x < v.w; ++x) {
                const std::size_t s = y * v.w + x, d = (oy + y) * mw + w + gap + x;
                std::copy_n(v.img.begin() + static_cast<std::ptrdiff_t>(s * ch), ch, mimg.begin() + static_cast<std::ptrdiff_t>(d * ch));
                unknown[d] = !v.ok[s];
            }
        }
        oy += v.h + gap;
    }
    const std::optional<std::vector<float>> filled = inpaint::complete(mw, mh, ch, mimg, unknown, params);
    std::vector<float> out = img;
    if (filled) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                if (!hole[y * w + x]) continue;
                std::copy_n(filled->begin() + static_cast<std::ptrdiff_t>((y * mw + x) * ch), ch, out.begin() + static_cast<std::ptrdiff_t>((y * w + x) * ch));
            }
        }
    } else {
        out = poisson::membrane_fill(w, h, ch, img, hole);
    }
    if (opts.color_adaptation > 0.0f) adapt_colors(w, h, ch, img, hole, out, opts.color_adaptation, opts.unit_range);
    return out;
}

}  // namespace genko::render::content_aware
