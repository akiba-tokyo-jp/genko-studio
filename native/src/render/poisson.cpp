#include "render/poisson.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <utility>

// PhotoCraft v0.2.0 crates/algo/src/poisson.rs (lines 1-232), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::poisson {

namespace {

// Convergence threshold (the largest update of a sweep).
constexpr float kTol = 2e-5f;
// The sweeps (at most) and the relaxation on every level above the coarsest: a 96×72 disc within ~1e-3 of a fully
// converged Gauss–Seidel solve (below one 8-bit step).
constexpr std::size_t kFineIters = 30;
constexpr float kFineOmega = 1.7f;
// The four pixels a pixel of the half-size grid stands for.
constexpr std::pair<std::size_t, std::size_t> kChildren[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};

// SOR sweeps over the unknown pixels until the largest update drops below kTol.
void sor(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v, std::size_t max_iter, float omega) {
    // (the unknown pixels and which of their neighbours exist)
    std::vector<std::pair<std::uint32_t, std::uint8_t>> cells;
    for (std::size_t i = 0; i < w * h; ++i) {
        if (!unknown[i]) continue;
        const std::size_t x = i % w, y = i / w;
        const auto m = static_cast<std::uint8_t>((x > 0 ? 1 : 0) | (x + 1 < w ? 2 : 0) | (y > 0 ? 4 : 0) | (y + 1 < h ? 8 : 0));
        cells.emplace_back(static_cast<std::uint32_t>(i), m);
    }
    for (std::size_t it = 0; it < max_iter; ++it) {
        float max_d = 0.0f;
        for (const auto& [cell, m] : cells) {
            const std::size_t i = cell;
            float s = 0.0f, n = 0.0f;
            if (m == 0b1111) {
                s = v[i - 1] + v[i + 1] + v[i - w] + v[i + w];
                n = 4.0f;
            } else {
                if ((m & 1) != 0) {
                    s += v[i - 1];
                    n += 1.0f;
                }
                if ((m & 2) != 0) {
                    s += v[i + 1];
                    n += 1.0f;
                }
                if ((m & 4) != 0) {
                    s += v[i - w];
                    n += 1.0f;
                }
                if ((m & 8) != 0) {
                    s += v[i + w];
                    n += 1.0f;
                }
                if (n == 0.0f) continue;
            }
            const float d = omega * (s / n - v[i]);
            v[i] += d;
            max_d = std::max(max_d, std::fabs(d));
        }
        if (max_d < kTol) break;
    }
}

void solve_rec(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v, std::size_t depth) {
    std::size_t n_unknown = 0;
    float known_sum = 0.0f;  // (the known values summed in float, in order)
    std::size_t n_known = 0;
    for (std::size_t i = 0; i < w * h; ++i) {
        if (unknown[i]) {
            ++n_unknown;
        } else {
            known_sum += v[i];
            ++n_known;
        }
    }
    if (n_unknown == 0 || n_known == 0) return;  // (nothing to solve, or nothing to interpolate from)
    if (w >= 8 && h >= 8 && n_unknown > 64 && depth < 16) {
        // Restrict: a coarse pixel is known when any of its children is (the mean of the known ones).
        const std::size_t cw = (w + 1) / 2, chh = (h + 1) / 2;
        std::vector<bool> cu(cw * chh, true);
        std::vector<float> cv(cw * chh, 0.0f);
        for (std::size_t cy = 0; cy < chh; ++cy) {
            for (std::size_t cx = 0; cx < cw; ++cx) {
                float s = 0.0f;
                unsigned n = 0;
                for (const auto& [dx, dy] : kChildren) {
                    const std::size_t x = cx * 2 + dx, y = cy * 2 + dy;
                    if (x < w && y < h && !unknown[y * w + x]) {
                        s += v[y * w + x];
                        ++n;
                    }
                }
                if (n > 0) {
                    cu[cy * cw + cx] = false;
                    cv[cy * cw + cx] = s / static_cast<float>(n);
                }
            }
        }
        solve_rec(cw, chh, cu, cv, depth + 1);
        // Prolong: the coarse solution sampled bilinearly as the first guess.
        for (std::size_t y = 0; y < h; ++y) {
            const float fy = std::clamp((static_cast<float>(y) + 0.5f) / 2.0f - 0.5f, 0.0f, static_cast<float>(chh - 1));
            const auto y0 = static_cast<std::size_t>(std::floor(fy));
            const std::size_t y1 = std::min(y0 + 1, chh - 1);
            const float ty = fy - static_cast<float>(y0);
            for (std::size_t x = 0; x < w; ++x) {
                if (!unknown[y * w + x]) continue;
                const float fx = std::clamp((static_cast<float>(x) + 0.5f) / 2.0f - 0.5f, 0.0f, static_cast<float>(cw - 1));
                const auto x0 = static_cast<std::size_t>(std::floor(fx));
                const std::size_t x1 = std::min(x0 + 1, cw - 1);
                const float tx = fx - static_cast<float>(x0);
                const float a = cv[y0 * cw + x0] + (cv[y0 * cw + x1] - cv[y0 * cw + x0]) * tx;
                const float b = cv[y1 * cw + x0] + (cv[y1 * cw + x1] - cv[y1 * cw + x0]) * tx;
                v[y * w + x] = a + (b - a) * ty;
            }
        }
        sor(w, h, unknown, v, kFineIters, kFineOmega);
    } else {
        const float mean = known_sum / static_cast<float>(n_known);
        for (std::size_t i = 0; i < w * h; ++i) {
            if (unknown[i]) v[i] = mean;
        }
        sor(w, h, unknown, v, 2000, 1.85f);
    }
}

// `f(c)` for each channel of an interleaved buffer, solved as planes and laid back interleaved.
template <typename F>
std::vector<float> per_channel(std::size_t w, std::size_t h, std::size_t ch, F f) {
    std::vector<float> out(w * h * ch, 0.0f);
    for (std::size_t c = 0; c < ch; ++c) {
        const std::vector<float> plane = f(c);
        for (std::size_t i = 0; i < w * h; ++i) out[i * ch + c] = plane[i];
    }
    return out;
}

}  // namespace

void solve_membrane(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v) {
    assert(unknown.size() == w * h && v.size() == w * h);
    solve_rec(w, h, unknown, v, 0);
}

void solve_membrane_reference(std::size_t w, std::size_t h, const std::vector<bool>& unknown, std::vector<float>& v, float tol,
                              std::size_t max_iter) {
    for (std::size_t i = 0; i < w * h; ++i) {
        if (unknown[i]) v[i] = 0.0f;
    }
    for (std::size_t it = 0; it < max_iter; ++it) {
        float max_d = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = y * w + x;
                if (!unknown[i]) continue;
                float s = 0.0f, n = 0.0f;
                if (x > 0) s += v[i - 1], n += 1.0f;
                if (x + 1 < w) s += v[i + 1], n += 1.0f;
                if (y > 0) s += v[i - w], n += 1.0f;
                if (y + 1 < h) s += v[i + w], n += 1.0f;
                if (n > 0.0f) {
                    const float nv = s / n;
                    max_d = std::max(max_d, std::fabs(nv - v[i]));
                    v[i] = nv;
                }
            }
        }
        if (max_d < tol) break;
    }
}

std::vector<float> seamless_clone(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& src, const std::vector<float>& dst,
                                  const std::vector<bool>& mask) {
    assert(src.size() == w * h * ch && dst.size() == w * h * ch && mask.size() == w * h);
    return per_channel(w, h, ch, [&](std::size_t c) {
        std::vector<float> hv(w * h);
        for (std::size_t i = 0; i < w * h; ++i) hv[i] = mask[i] ? 0.0f : dst[i * ch + c] - src[i * ch + c];
        solve_membrane(w, h, mask, hv);
        std::vector<float> out(w * h);
        for (std::size_t i = 0; i < w * h; ++i) out[i] = mask[i] ? src[i * ch + c] + hv[i] : dst[i * ch + c];
        return out;
    });
}

std::vector<float> membrane_fill(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole) {
    return per_channel(w, h, ch, [&](std::size_t c) {
        std::vector<float> v(w * h);
        for (std::size_t i = 0; i < w * h; ++i) v[i] = hole[i] ? 0.0f : img[i * ch + c];
        solve_membrane(w, h, hole, v);
        return v;
    });
}

}  // namespace genko::render::poisson
