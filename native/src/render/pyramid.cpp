// Multiresolution blending (render/pyramid.hpp, PC5 I-g): PhotoCraft v0.2.0's algo/src/pyramid.rs (MIT,
// native/third_party/photocraft/NOTICE.md), its loops in its order on one thread; blend_tiled is Genko's.

#include "render/pyramid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "render/segment/segment.hpp"

namespace genko::render::pyramid {

namespace {

constexpr std::array<float, 5> kK{1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

std::int64_t clampi(std::int64_t v, std::int64_t lo, std::int64_t hi) { return std::min(std::max(v, lo), hi); }

}  // namespace

Level reduce(const Level& l, std::size_t ch) {
    const std::size_t w = (l.w + 1) / 2, h = (l.h + 1) / 2;
    // (across on the even columns, then down on the even rows)
    std::vector<float> tmp(w * l.h * ch, 0.0f);
    for (std::size_t y = 0; y < l.h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            for (std::size_t k = 0; k < 5; ++k) {
                const std::size_t sx = static_cast<std::size_t>(clampi(std::int64_t(2 * x + k) - 2, 0, std::int64_t(l.w) - 1));
                for (std::size_t c = 0; c < ch; ++c) tmp[(y * w + x) * ch + c] += kK[k] * l.px[(y * l.w + sx) * ch + c];
            }
        }
    }
    Level out{w, h, std::vector<float>(w * h * ch, 0.0f)};
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t k = 0; k < 5; ++k) {
            const std::size_t sy = static_cast<std::size_t>(clampi(std::int64_t(2 * y + k) - 2, 0, std::int64_t(l.h) - 1));
            for (std::size_t x = 0; x < w; ++x) {
                for (std::size_t c = 0; c < ch; ++c) out.px[(y * w + x) * ch + c] += kK[k] * tmp[(sy * w + x) * ch + c];
            }
        }
    }
    return out;
}

std::vector<float> expand(const Level& l, std::size_t ch, std::size_t w, std::size_t h) {
    // (EXPAND: zeros put between, the same filter, a gain of 4)
    std::vector<float> tmp(w * l.h * ch, 0.0f);
    for (std::size_t y = 0; y < l.h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            for (std::size_t k = 0; k < 5; ++k) {
                const std::int64_t t = std::int64_t(x + k) - 2;
                if (((t % 2) + 2) % 2 != 0) continue;
                const std::size_t sx = static_cast<std::size_t>(clampi(t / 2, 0, std::int64_t(l.w) - 1));
                for (std::size_t c = 0; c < ch; ++c) tmp[(y * w + x) * ch + c] += 2.0f * kK[k] * l.px[(y * l.w + sx) * ch + c];
            }
        }
    }
    std::vector<float> out(w * h * ch, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t k = 0; k < 5; ++k) {
            const std::int64_t t = std::int64_t(y + k) - 2;
            if (((t % 2) + 2) % 2 != 0) continue;
            const std::size_t sy = static_cast<std::size_t>(clampi(t / 2, 0, std::int64_t(l.h) - 1));
            for (std::size_t x = 0; x < w; ++x) {
                for (std::size_t c = 0; c < ch; ++c) out[(y * w + x) * ch + c] += 2.0f * kK[k] * tmp[(sy * w + x) * ch + c];
            }
        }
    }
    return out;
}

std::vector<Level> gaussian(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& px, std::size_t levels) {
    std::vector<Level> out{Level{w, h, px}};
    while (out.size() < std::max<std::size_t>(levels, 1)) {
        const Level& last = out.back();
        if (last.w <= 1 && last.h <= 1) break;
        Level next = reduce(last, ch);
        out.push_back(std::move(next));
    }
    return out;
}

std::vector<Level> laplacian(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& px, std::size_t levels) {
    std::vector<Level> g = gaussian(w, h, ch, px, levels);
    std::vector<Level> out;
    out.reserve(g.size());
    for (std::size_t i = 0; i < g.size(); ++i) {
        if (i + 1 == g.size()) {
            out.push_back(g[i]);
        } else {
            const std::vector<float> up = expand(g[i + 1], ch, g[i].w, g[i].h);
            Level l{g[i].w, g[i].h, std::vector<float>(g[i].px.size())};
            for (std::size_t k = 0; k < up.size(); ++k) l.px[k] = g[i].px[k] - up[k];
            out.push_back(std::move(l));
        }
    }
    return out;
}

std::vector<float> collapse(const std::vector<Level>& pyr, std::size_t ch) {
    if (pyr.empty()) return {};
    Level cur = pyr.back();
    for (std::size_t i = pyr.size() - 1; i-- > 0;) {
        const Level& l = pyr[i];
        const std::vector<float> up = expand(cur, ch, l.w, l.h);
        Level next{l.w, l.h, std::vector<float>(l.px.size())};
        for (std::size_t k = 0; k < up.size(); ++k) next.px[k] = l.px[k] + up[k];
        cur = std::move(next);
    }
    return std::move(cur.px);
}

std::size_t auto_levels(std::size_t w, std::size_t h) {
    std::size_t n = 1, s = std::min(w, h);
    while (s > 8 && n < 10) {
        s /= 2;
        ++n;
    }
    return n;
}

std::vector<float> blend(std::size_t w, std::size_t h, std::size_t ch, const std::vector<const std::vector<float>*>& images,
                         const std::vector<const std::vector<float>*>& weights, std::size_t levels, bool clamp) {
    levels = std::max<std::size_t>(levels, 1);
    std::vector<Level> acc, wsum;
    for (std::size_t k = 0; k < images.size(); ++k) {
        segment::check_stop();
        const std::vector<Level> lp = laplacian(w, h, ch, *images[k], levels);
        const std::vector<Level> gp = gaussian(w, h, 1, *weights[k], levels);
        if (acc.empty()) {
            for (const Level& l : lp) acc.push_back(Level{l.w, l.h, std::vector<float>(l.px.size(), 0.0f)});
            for (const Level& l : gp) wsum.push_back(Level{l.w, l.h, std::vector<float>(l.px.size(), 0.0f)});
        }
        for (std::size_t li = 0; li < lp.size() && li < gp.size(); ++li) {
            const Level &l = lp[li], &g = gp[li];
            for (std::size_t i = 0; i < l.w * l.h; ++i) {
                const float kk = g.px[i];
                wsum[li].px[i] += kk;
                for (std::size_t c = 0; c < ch; ++c) acc[li].px[i * ch + c] += kk * l.px[i * ch + c];
            }
        }
    }
    if (acc.empty()) return std::vector<float>(w * h * ch, 0.0f);
    for (std::size_t li = 0; li < acc.size(); ++li) {
        Level& l = acc[li];
        for (std::size_t i = 0; i < l.w * l.h; ++i) {
            const float kk = wsum[li].px[i];
            for (std::size_t c = 0; c < ch; ++c) l.px[i * ch + c] = kk > 1e-6f ? l.px[i * ch + c] / kk : 0.0f;
        }
    }
    std::vector<float> out = collapse(acc, ch);
    if (clamp) {
        for (float& v : out) v = std::clamp(v, 0.0f, 1.0f);
    }
    return out;
}

std::size_t tile_overlap(std::size_t levels) { return 5u << std::min(levels, kMaxTiledLevels); }

void blend_tiled(std::size_t w, std::size_t h, std::size_t ch, std::size_t images, const ReadPart& read, const ReadPart& weight, std::size_t levels,
                 bool clamp, const WritePart& write) {
    levels = std::clamp<std::size_t>(levels, 1, kMaxTiledLevels);
    const std::size_t overlap = tile_overlap(levels), grid = std::size_t(1) << (levels - 1);
    for (std::size_t ty = 0; ty < h; ty += kTile) {
        for (std::size_t tx = 0; tx < w; tx += kTile) {
            segment::check_stop();
            const std::size_t cw = std::min(kTile, w - tx), chh = std::min(kTile, h - ty);
            // (the tile widened by the overlap, its corner on the coarsest level's grid so the tiles' levels line up)
            const std::size_t x0 = (tx > overlap ? tx - overlap : 0) / grid * grid, y0 = (ty > overlap ? ty - overlap : 0) / grid * grid;
            const std::size_t x1 = std::min(w, tx + cw + overlap), y1 = std::min(h, ty + chh + overlap);
            const std::size_t rw = x1 - x0, rh = y1 - y0;
            std::vector<std::vector<float>> px(images), wt(images);
            for (std::size_t i = 0; i < images; ++i) {
                px[i].assign(rw * rh * ch, 0.0f);
                wt[i].assign(rw * rh, 0.0f);
                read(i, x0, y0, rw, rh, px[i]);
                weight(i, x0, y0, rw, rh, wt[i]);
            }
            std::vector<const std::vector<float>*> ip, wp;
            for (std::size_t i = 0; i < images; ++i) {
                ip.push_back(&px[i]);
                wp.push_back(&wt[i]);
            }
            const std::vector<float> out = blend(rw, rh, ch, ip, wp, levels, clamp);
            std::vector<float> core(cw * chh * ch);
            for (std::size_t y = 0; y < chh; ++y) {
                const std::size_t ry = ty + y - y0;
                std::copy_n(out.begin() + static_cast<std::ptrdiff_t>((ry * rw + (tx - x0)) * ch), cw * ch, core.begin() + static_cast<std::ptrdiff_t>(y * cw * ch));
            }
            write(tx, ty, cw, chh, core);
        }
    }
}

namespace {

std::vector<float> box_blur1(std::size_t w, std::size_t h, const std::vector<float>& v, std::size_t r) {
    const auto pass = [&](const std::vector<float>& src, bool horizontal) {
        std::vector<float> dst(src.size(), 0.0f);
        const std::size_t n = horizontal ? h : w, m = horizontal ? w : h;
        const std::int64_t ri = static_cast<std::int64_t>(r);
        for (std::size_t line = 0; line < n; ++line) {
            const auto at = [&](std::int64_t k) {
                const std::size_t kk = static_cast<std::size_t>(clampi(k, 0, std::int64_t(m) - 1));
                return horizontal ? src[line * w + kk] : src[kk * w + line];
            };
            float s = 0.0f;
            for (std::int64_t k = -ri; k <= ri; ++k) s += at(k);
            for (std::size_t k = 0; k < m; ++k) {
                const std::size_t i = horizontal ? line * w + k : k * w + line;
                dst[i] = s / static_cast<float>(2 * r + 1);
                s += at(std::int64_t(k) + ri + 1) - at(std::int64_t(k) - ri);
            }
        }
        return dst;
    };
    return pass(pass(v, true), false);
}

std::vector<std::vector<float>> winner_take_all(const std::vector<std::vector<float>>& scores, std::size_t n) {
    std::vector<std::vector<float>> out(scores.size(), std::vector<float>(n, 0.0f));
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t best = 0;
        float score = std::numeric_limits<float>::lowest();
        for (std::size_t k = 0; k < scores.size(); ++k) {
            if (scores[k][i] > score) {
                best = k;
                score = scores[k][i];
            }
        }
        if (score > std::numeric_limits<float>::lowest()) out[best][i] = 1.0f;
    }
    return out;
}

}  // namespace

std::vector<std::vector<float>> stack_weights(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& luma,
                                              const std::vector<std::vector<float>>& alpha) {
    const std::size_t radius = std::clamp<std::size_t>(std::min(w, h) / 64, 2, 12);
    std::vector<std::vector<float>> scores;
    for (std::size_t k = 0; k < luma.size(); ++k) {
        segment::check_stop();
        const std::vector<float>& l = luma[k];
        std::vector<float> lap(w * h, 0.0f);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const auto p = [&](std::size_t xx, std::size_t yy) { return l[yy * w + xx]; };
                const std::size_t xl = x == 0 ? 0 : x - 1, xr = std::min(x + 1, w - 1), yu = y == 0 ? 0 : y - 1, yd = std::min(y + 1, h - 1);
                lap[y * w + x] = std::fabs(p(xl, y) + p(xr, y) + p(x, yu) + p(x, yd) - 4.0f * p(x, y));
            }
        }
        std::vector<float> s = box_blur1(w, h, lap, radius);
        for (std::size_t i = 0; i < w * h; ++i) s[i] = alpha[k][i] > 0.5f ? s[i] : -1.0f;
        scores.push_back(std::move(s));
    }
    return winner_take_all(scores, w * h);
}

std::vector<std::vector<float>> panorama_weights(std::size_t w, std::size_t h, const std::vector<std::vector<float>>& alpha) {
    std::vector<std::vector<float>> scores;
    for (const std::vector<float>& a : alpha) {
        // (two passes of the 3-4 chamfer distance from the clear pixels and the border)
        constexpr float kInf = 1.0e9f;
        std::vector<float> d(w * h);
        for (std::size_t i = 0; i < w * h; ++i) d[i] = a[i] > 0.5f ? kInf : 0.0f;
        const auto at = [&](std::int64_t x, std::int64_t y) {
            return x < 0 || y < 0 || x >= std::int64_t(w) || y >= std::int64_t(h) ? 0.0f : d[std::size_t(y) * w + std::size_t(x)];
        };
        for (std::int64_t y = 0; y < std::int64_t(h); ++y) {
            for (std::int64_t x = 0; x < std::int64_t(w); ++x) {
                const std::size_t i = std::size_t(y) * w + std::size_t(x);
                d[i] = std::min({d[i], at(x - 1, y) + 3.0f, at(x, y - 1) + 3.0f, at(x - 1, y - 1) + 4.0f, at(x + 1, y - 1) + 4.0f});
            }
        }
        for (std::int64_t y = std::int64_t(h) - 1; y >= 0; --y) {
            for (std::int64_t x = std::int64_t(w) - 1; x >= 0; --x) {
                const std::size_t i = std::size_t(y) * w + std::size_t(x);
                d[i] = std::min({d[i], at(x + 1, y) + 3.0f, at(x, y + 1) + 3.0f, at(x + 1, y + 1) + 4.0f, at(x - 1, y + 1) + 4.0f});
            }
        }
        for (float& v : d) v = v <= 0.0f ? -1.0f : v;
        scores.push_back(std::move(d));
    }
    return winner_take_all(scores, w * h);
}

}  // namespace genko::render::pyramid
