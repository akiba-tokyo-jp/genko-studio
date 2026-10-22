#include "render/paint_tools/tone.hpp"

#include <algorithm>
#include <cmath>

// PhotoCraft v0.2.0 crates/algo/src/retouch.rs (lines 1-200) and crates/engine/src/retouch_cmds.rs (lines 434-456), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::paint_tools {

float luma(const std::array<float, 3>& c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

float tone_curve(float v, float e, ToneRange range, bool burn) {
    v = std::max(v, 0.0f);
    e = std::clamp(e, 0.0f, 1.0f);
    const float u = std::min(v, 1.0f);
    switch (range) {
        case ToneRange::Midtones: {  // (a gamma bend weighted to peak at mid-grey: 0 and 1 stay)
            const float w = std::clamp(4.0f * v * (1.0f - v), 0.0f, 1.0f);
            return v + w * (std::pow(u, burn ? 1.0f + e : 1.0f / (1.0f + e)) - v);
        }
        case ToneRange::Shadows:  // (v + e·w·(target − v), weight (1 − v)²: black lifts most when dodging)
            return burn ? v - e * (1.0f - u) * (1.0f - u) * v : v + e * (1.0f - u) * (1.0f - u) * (1.0f - u);
        case ToneRange::Highlights:  // (weight v²: white drops most when burning)
            return burn ? v - e * u * u * v : v + e * u * u * (1.0f - u);
    }
    return v;
}

namespace {

// Back into 0..1 keeping the luma (toward its grey), instead of clipping each channel (which would shift the hue).
std::array<float, 3> compress_gamut(const std::array<float, 3>& c, float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    const float lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
    float t = 1.0f;
    if (hi > 1.0f && hi - l > 1e-9f) t = std::min(t, (1.0f - l) / (hi - l));
    if (lo < 0.0f && l - lo > 1e-9f) t = std::min(t, l / (l - lo));
    return {l + (c[0] - l) * t, l + (c[1] - l) * t, l + (c[2] - l) * t};
}

}  // namespace

std::array<float, 3> dodge_burn(const std::array<float, 3>& c, float e, ToneRange range, bool burn, bool protect_tones) {
    if (e <= 0.0f) return c;
    if (protect_tones) {
        const float l = luma(c);
        const float nl = tone_curve(l, e, range, burn);
        const std::array<float, 3> out = l > 1e-6f ? std::array<float, 3>{c[0] * nl / l, c[1] * nl / l, c[2] * nl / l} : std::array<float, 3>{nl, nl, nl};
        return compress_gamut(out, nl);
    }
    return {tone_curve(c[0], e, range, burn), tone_curve(c[1], e, range, burn), tone_curve(c[2], e, range, burn)};
}

float saturation(const std::array<float, 3>& c) {
    const float lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
    return hi <= 1e-6f ? 0.0f : std::clamp((hi - lo) / hi, 0.0f, 1.0f);
}

std::array<float, 3> sponge(const std::array<float, 3>& c, float amount, bool saturate, bool vibrance) {
    float a = std::clamp(amount, 0.0f, 1.0f);
    if (a <= 0.0f) return c;
    const float s = saturation(c);
    if (vibrance) a *= saturate ? 1.0f - s : 0.25f + 0.75f * s;
    const float l = luma(c);
    const float k = saturate ? 1.0f + a : 1.0f - a;
    return compress_gamut({l + (c[0] - l) * k, l + (c[1] - l) * k, l + (c[2] - l) * k}, l);
}

float per_dab(float e, float spacing) { return 1.0f - std::pow(1.0f - std::clamp(e, 0.0f, 1.0f), std::clamp(spacing, 0.01f, 1.0f)); }

std::vector<float> local_blur(const Reader& get, Size grid, int ch, int alpha, const Box& win, float sigma) {
    const int ow = std::max(win.width(), 0), oh = std::max(win.height(), 0);
    const int r = std::max(static_cast<int>(std::ceil(sigma * 3.0f)), 1);
    std::vector<float> k;
    k.reserve(std::size_t(2 * r + 1));
    float ks = 0.0f;
    for (int i = -r; i <= r; ++i) {
        k.push_back(std::exp(-static_cast<float>(i * i) / std::max(2.0f * sigma * sigma, 1e-6f)));
        ks += k.back();
    }
    for (float& v : k) v /= ks;
    const auto n = std::size_t(ch);
    // (a pixel at a place, the grid's edge repeated past it; its colours premultiplied by its alpha)
    const auto fetch = [&](int x, int y, float* out) {
        const float* v = get(std::clamp(x, 0, grid.width - 1), std::clamp(y, 0, grid.height - 1));
        std::copy(v, v + n, out);
        if (alpha >= 0) {
            const float a = out[alpha];
            for (int c = 0; c < ch; ++c) {
                if (c != alpha) out[c] *= a;
            }
        }
    };
    // the rows across, over the window's rows grown by r up and down
    const int th = oh + 2 * r;
    std::vector<float> tmp(std::size_t(ow) * std::size_t(th) * n, 0.0f);
    std::vector<float> px(n);
    for (int ty = 0; ty < th; ++ty) {
        const int y = win.y0 + ty - r;
        for (int ox = 0; ox < ow; ++ox) {
            float* o = tmp.data() + (std::size_t(ty) * std::size_t(ow) + std::size_t(ox)) * n;
            for (int i = 0; i <= 2 * r; ++i) {
                fetch(win.x0 + ox + i - r, y, px.data());
                for (std::size_t c = 0; c < n; ++c) o[c] += px[c] * k[std::size_t(i)];
            }
        }
    }
    // then down
    std::vector<float> out(std::size_t(ow) * std::size_t(oh) * n, 0.0f);
    for (int oy = 0; oy < oh; ++oy) {
        for (int ox = 0; ox < ow; ++ox) {
            float* o = out.data() + (std::size_t(oy) * std::size_t(ow) + std::size_t(ox)) * n;
            for (int i = 0; i <= 2 * r; ++i) {
                const float* s = tmp.data() + (std::size_t(oy + i) * std::size_t(ow) + std::size_t(ox)) * n;
                for (std::size_t c = 0; c < n; ++c) o[c] += s[c] * k[std::size_t(i)];
            }
            if (alpha >= 0) {
                const float a = o[alpha];
                const float* was = get(win.x0 + ox, win.y0 + oy);
                for (int c = 0; c < ch; ++c) {
                    if (c != alpha) o[c] = a > 1e-6f ? o[c] / a : was[c];
                }
            }
        }
    }
    return out;
}

std::vector<float> local_sharpen(const Reader& get, Size grid, int ch, int alpha, const Box& win, float amount, bool protect_detail) {
    const int ow = std::max(win.width(), 0);
    std::vector<float> out = local_blur(get, grid, ch, alpha, win, 1.0f);
    const auto n = std::size_t(ch);
    for (int y = win.y0; y < win.y1; ++y) {
        for (int x = win.x0; x < win.x1; ++x) {
            float* o = out.data() + (std::size_t(y - win.y0) * std::size_t(ow) + std::size_t(x - win.x0)) * n;
            const float* v = get(x, y);
            for (int c = 0; c < ch; ++c) {
                if (c == alpha) {
                    o[c] = v[c];
                    continue;
                }
                float s = v[c] + amount * (v[c] - o[c]);
                if (protect_detail) {
                    float lo = v[c], hi = v[c];
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const float nv = get(std::clamp(x + dx, 0, grid.width - 1), std::clamp(y + dy, 0, grid.height - 1))[c];
                            lo = std::min(lo, nv);
                            hi = std::max(hi, nv);
                        }
                    }
                    s = std::clamp(s, lo, hi);
                }
                o[c] = s;
            }
        }
    }
    return out;
}

}  // namespace genko::render::paint_tools
