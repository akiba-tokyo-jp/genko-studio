// Image registration (render/features.hpp, PC5 I-g): PhotoCraft v0.2.0's algo/src/features.rs (MIT,
// native/third_party/photocraft/NOTICE.md), ported line for line — its loops in the same order on one thread, so the corners,
// the descriptors, the matches and RANSAC's draws are the same for the same pictures.

#include "render/features.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/segment/segment.hpp"

namespace genko::render::features {

std::uint64_t Rng::next() {
    s += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double Rng::unit() { return static_cast<double>(next() >> 11) / static_cast<double>(std::uint64_t(1) << 53); }

std::size_t Rng::below(std::size_t n) { return static_cast<std::size_t>(next() % std::max<std::uint64_t>(n, 1)); }

std::vector<float> box_blur(std::size_t w, std::size_t h, const std::vector<float>& v, std::size_t r) {
    const auto pass = [&](const std::vector<float>& src, bool horizontal) {
        std::vector<float> dst(src.size(), 0.0f);
        const std::size_t n = horizontal ? h : w, m = horizontal ? w : h;
        const auto at = [&](std::size_t line, std::int64_t k) {
            const std::size_t kk = static_cast<std::size_t>(std::clamp<std::int64_t>(k, 0, static_cast<std::int64_t>(m) - 1));
            return horizontal ? src[line * w + kk] : src[kk * w + line];
        };
        const std::int64_t ri = static_cast<std::int64_t>(r);
        for (std::size_t line = 0; line < n; ++line) {
            float acc = 0.0f;
            for (std::int64_t k = -ri; k <= ri; ++k) acc += at(line, k);
            for (std::size_t k = 0; k < m; ++k) {
                const std::size_t i = horizontal ? line * w + k : k * w + line;
                dst[i] = acc / static_cast<float>(2 * r + 1);
                acc += at(line, static_cast<std::int64_t>(k) + ri + 1) - at(line, static_cast<std::int64_t>(k) - ri);
            }
        }
        return dst;
    };
    return pass(pass(v, true), false);
}

std::vector<Corner> harris(std::size_t w, std::size_t h, const std::vector<float>& img, std::size_t max, float min_dist, const std::vector<bool>* valid) {
    if (w < 2 * kPatchRadius + 3 || h < 2 * kPatchRadius + 3) return {};
    std::vector<float> xx(w * h, 0.0f), yy(w * h, 0.0f), xy(w * h, 0.0f);
    for (std::size_t y = 1; y + 1 < h; ++y) {
        for (std::size_t x = 1; x + 1 < w; ++x) {
            const auto p = [&](int dx, int dy) { return img[(y + dy) * w + (x + dx)]; };
            // (Sobel)
            const float gx = (p(1, -1) + 2.0f * p(1, 0) + p(1, 1)) - (p(-1, -1) + 2.0f * p(-1, 0) + p(-1, 1));
            const float gy = (p(-1, 1) + 2.0f * p(0, 1) + p(1, 1)) - (p(-1, -1) + 2.0f * p(0, -1) + p(1, -1));
            const std::size_t i = y * w + x;
            xx[i] = gx * gx;
            yy[i] = gy * gy;
            xy[i] = gx * gy;
        }
    }
    segment::check_stop();
    // (two box passes approximate the Gaussian window)
    xx = box_blur(w, h, box_blur(w, h, xx, 1), 1);
    yy = box_blur(w, h, box_blur(w, h, yy, 1), 1);
    xy = box_blur(w, h, box_blur(w, h, xy, 1), 1);
    std::vector<float> r(w * h);
    float peak = 0.0f;
    for (std::size_t i = 0; i < w * h; ++i) {
        const float t = xx[i] + yy[i];
        r[i] = xx[i] * yy[i] - xy[i] * xy[i] - 0.04f * (t * t);
        peak = std::max(peak, r[i]);
    }
    if (!(peak > 0.0f)) return {};
    const std::size_t m = kPatchRadius + 1;
    std::vector<Corner> cand;
    for (std::size_t y = m; y < h - m; ++y) {
        for (std::size_t x = m; x < w - m; ++x) {
            const float v = r[y * w + x];
            if (v < peak * 0.01f) continue;
            bool is_max = true;
            for (int dy = -1; dy <= 1 && is_max; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx != 0 || dy != 0) && r[(y + dy) * w + (x + dx)] > v) {
                        is_max = false;
                        break;
                    }
                }
            }
            if (!is_max) continue;
            if (valid != nullptr) {
                bool all = true;
                for (std::size_t sy = y - kPatchRadius; sy <= y + kPatchRadius && all; sy += 5) {
                    for (std::size_t sx = x - kPatchRadius; sx <= x + kPatchRadius; sx += 5) {
                        if (!(*valid)[sy * w + sx]) {
                            all = false;
                            break;
                        }
                    }
                }
                if (!all) continue;
            }
            cand.push_back(Corner{static_cast<float>(x), static_cast<float>(y), v});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const Corner& a, const Corner& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    // (minimum spacing through a coarse grid)
    const float cell = std::max(min_dist, 1.0f);
    const std::size_t gw = static_cast<std::size_t>(std::ceil(static_cast<float>(w) / cell)) + 1;
    const std::size_t gh = static_cast<std::size_t>(std::ceil(static_cast<float>(h) / cell)) + 1;
    std::vector<std::vector<std::pair<float, float>>> grid(gw * gh);
    std::vector<Corner> out;
    for (const Corner& c : cand) {
        const std::size_t gx = static_cast<std::size_t>(c.x / cell), gy = static_cast<std::size_t>(c.y / cell);
        bool near = false;
        for (std::size_t yy2 = gy == 0 ? 0 : gy - 1; yy2 <= std::min(gy + 1, gh - 1) && !near; ++yy2) {
            for (std::size_t xx2 = gx == 0 ? 0 : gx - 1; xx2 <= std::min(gx + 1, gw - 1) && !near; ++xx2) {
                for (const auto& [px, py] : grid[yy2 * gw + xx2]) {
                    if (std::hypot(px - c.x, py - c.y) < min_dist) {
                        near = true;
                        break;
                    }
                }
            }
        }
        if (near) continue;
        grid[gy * gw + gx].emplace_back(c.x, c.y);
        out.push_back(c);
        if (out.size() >= max) break;
    }
    return out;
}

namespace {

// The 256 BRIEF test pairs (fixed: Gaussian around the centre, σ = 31/5, clipped to the patch).
const std::array<std::array<float, 4>, 256>& pairs() {
    static const std::array<std::array<float, 4>, 256> p = [] {
        Rng rng{0x0B21EF00ULL};
        const auto gauss = [&rng] {
            // (Box–Muller)
            const double u = std::max(rng.unit(), 1e-12), v = rng.unit();
            return static_cast<float>(std::clamp(std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * std::numbers::pi * v) * 31.0 / 5.0, -13.0, 13.0));
        };
        std::array<std::array<float, 4>, 256> out{};
        for (auto& q : out) {
            const float a = gauss(), b = gauss(), c = gauss(), d = gauss();
            q = {a, b, c, d};
        }
        return out;
    }();
    return p;
}

}  // namespace

std::vector<Descriptor> describe(std::size_t w, std::size_t h, const std::vector<float>& img, const std::vector<Corner>& corners, bool upright) {
    const std::vector<float> smooth = box_blur(w, h, img, 2);
    const auto at = [&](float x, float y) {
        const std::size_t xi = static_cast<std::size_t>(std::clamp<std::int64_t>(std::llround(x), 0, static_cast<std::int64_t>(w) - 1));
        const std::size_t yi = static_cast<std::size_t>(std::clamp<std::int64_t>(std::llround(y), 0, static_cast<std::int64_t>(h) - 1));
        return smooth[yi * w + xi];
    };
    std::vector<Descriptor> out;
    out.reserve(corners.size());
    for (const Corner& c : corners) {
        float s = 0.0f, co = 1.0f;
        if (!upright) {
            float m10 = 0.0f, m01 = 0.0f;
            const int r = static_cast<int>(kPatchRadius);
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy <= r * r) {
                        const float v = at(c.x + static_cast<float>(dx), c.y + static_cast<float>(dy));
                        m10 += static_cast<float>(dx) * v;
                        m01 += static_cast<float>(dy) * v;
                    }
                }
            }
            const float angle = std::atan2(m01, m10);
            s = std::sin(angle);
            co = std::cos(angle);
        }
        Descriptor d{0, 0, 0, 0};
        const auto& p = pairs();
        for (std::size_t i = 0; i < 256; ++i) {
            const auto rot = [&](float x, float y) { return std::pair<float, float>{c.x + x * co - y * s, c.y + x * s + y * co}; };
            const auto [ax, ay] = rot(p[i][0], p[i][1]);
            const auto [bx, by] = rot(p[i][2], p[i][3]);
            if (at(ax, ay) < at(bx, by)) d[i / 64] |= std::uint64_t(1) << (i % 64);
        }
        out.push_back(d);
    }
    return out;
}

unsigned hamming(const Descriptor& a, const Descriptor& b) {
    unsigned n = 0;
    for (std::size_t i = 0; i < 4; ++i) n += static_cast<unsigned>(std::popcount(a[i] ^ b[i]));
    return n;
}

std::vector<std::pair<std::size_t, std::size_t>> match_descriptors(const std::vector<Descriptor>& a, const std::vector<Descriptor>& b, float ratio) {
    struct Best {
        std::size_t index;
        unsigned first, second;
    };
    constexpr unsigned kNone = std::numeric_limits<unsigned>::max();
    const auto best = [](const Descriptor& d, const std::vector<Descriptor>& set) -> std::optional<Best> {
        std::size_t index = std::numeric_limits<std::size_t>::max();
        unsigned first = kNone, second = kNone;
        for (std::size_t j = 0; j < set.size(); ++j) {
            const unsigned h = hamming(d, set[j]);
            if (h < first) {
                second = first;
                first = h;
                index = j;
            } else if (h < second) {
                second = h;
            }
        }
        if (index == std::numeric_limits<std::size_t>::max()) return std::nullopt;
        return Best{index, first, second};
    };
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 64 == 0) segment::check_stop();
        const auto m = best(a[i], b);
        if (!m) continue;
        if (m->first > 80 || (m->second != kNone && static_cast<float>(m->first) > ratio * static_cast<float>(m->second))) continue;
        const auto back = best(b[m->index], a);
        if (back && back->index == i) out.emplace_back(i, m->index);
    }
    return out;
}

// --- model fitting -----------------------------------------------------------------------------------------------------

std::optional<std::vector<double>> solve_dense(std::size_t n, std::vector<double> a, std::vector<double> b, double tiny) {
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t p = c;
        for (std::size_t i = c + 1; i < n; ++i) {
            if (std::fabs(a[i * n + c]) > std::fabs(a[p * n + c])) p = i;
        }
        if (!(std::fabs(a[p * n + c]) >= tiny)) return std::nullopt;
        if (p != c) {
            for (std::size_t k = 0; k < n; ++k) std::swap(a[c * n + k], a[p * n + k]);
            std::swap(b[c], b[p]);
        }
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            if (f == 0.0) continue;
            for (std::size_t k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    std::vector<double> x(n, 0.0);
    for (std::size_t r = n; r-- > 0;) {
        double s = 0.0;
        for (std::size_t k = r + 1; k < n; ++k) s += a[r * n + k] * x[k];
        x[r] = (b[r] - s) / a[r * n + r];
        if (!std::isfinite(x[r])) return std::nullopt;
    }
    return x;
}

namespace {

// Least squares min |A x − b| through the normal equations.
std::optional<std::vector<double>> least_squares(const std::vector<std::pair<std::vector<double>, double>>& rows, std::size_t n) {
    std::vector<double> ata(n * n, 0.0), atb(n, 0.0);
    for (const auto& [r, b] : rows) {
        for (std::size_t i = 0; i < n; ++i) {
            atb[i] += r[i] * b;
            for (std::size_t j = 0; j < n; ++j) ata[i * n + j] += r[i] * r[j];
        }
    }
    return solve_dense(n, std::move(ata), std::move(atb));
}

// Hartley's normalisation: the centroid to the origin, the mean distance √2.
Homography normaliser(const std::vector<XY>& pts) {
    const double n = static_cast<double>(std::max<std::size_t>(pts.size(), 1));
    double cx = 0.0, cy = 0.0;
    for (const XY& p : pts) cx += p[0];
    for (const XY& p : pts) cy += p[1];
    cx /= n;
    cy /= n;
    double d = 0.0;
    for (const XY& p : pts) d += std::hypot(p[0] - cx, p[1] - cy);
    d /= n;
    const double s = d > 1e-12 ? std::numbers::sqrt2 / d : 1.0;
    return Homography{{s, 0.0, -s * cx, 0.0, s, -s * cy, 0.0, 0.0, 1.0}};
}

}  // namespace

std::optional<Homography> fit(Model model, const std::vector<XY>& src, const std::vector<XY>& dst) {
    const std::size_t n = src.size();
    switch (model) {
        case Model::Translation: {
            if (n == 0) return std::nullopt;
            double tx = 0.0, ty = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                tx += dst[i][0] - src[i][0];
                ty += dst[i][1] - src[i][1];
            }
            return Homography{{1.0, 0.0, tx / static_cast<double>(n), 0.0, 1.0, ty / static_cast<double>(n), 0.0, 0.0, 1.0}};
        }
        case Model::Similarity: {
            if (n < 2) return std::nullopt;
            // (x' = a·x − b·y + tx, y' = b·x + a·y + ty)
            std::vector<std::pair<std::vector<double>, double>> rows;
            rows.reserve(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                rows.emplace_back(std::vector<double>{src[i][0], -src[i][1], 1.0, 0.0}, dst[i][0]);
                rows.emplace_back(std::vector<double>{src[i][1], src[i][0], 0.0, 1.0}, dst[i][1]);
            }
            const auto v = least_squares(rows, 4);
            if (!v) return std::nullopt;
            return Homography{{(*v)[0], -(*v)[1], (*v)[2], (*v)[1], (*v)[0], (*v)[3], 0.0, 0.0, 1.0}};
        }
        case Model::Homography: {
            if (n < 4) return std::nullopt;
            const Homography ts = normaliser(src), td = normaliser(dst);
            std::vector<std::pair<std::vector<double>, double>> rows;
            rows.reserve(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = ts.apply(src[i][0], src[i][1]);
                const auto [u, v] = td.apply(dst[i][0], dst[i][1]);
                rows.emplace_back(std::vector<double>{x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y}, u);
                rows.emplace_back(std::vector<double>{0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y}, v);
            }
            const auto h = least_squares(rows, 8);
            if (!h) return std::nullopt;
            const Homography hn{{(*h)[0], (*h)[1], (*h)[2], (*h)[3], (*h)[4], (*h)[5], (*h)[6], (*h)[7], 1.0}};
            const auto tdi = td.inverse();
            if (!tdi) return std::nullopt;
            Homography m = tdi->mul(hn).mul(ts);
            const double k = m.m[8];
            if (!(std::fabs(k) > 1e-12)) return std::nullopt;
            for (double& v : m.m) v /= k;
            return m;
        }
    }
    return std::nullopt;
}

std::size_t min_points(Model model) {
    switch (model) {
        case Model::Translation: return 1;
        case Model::Similarity: return 2;
        case Model::Homography: return 4;
    }
    return 4;
}

std::optional<std::pair<Homography, std::vector<std::size_t>>> ransac(Model model, const std::vector<XY>& src, const std::vector<XY>& dst,
                                                                      std::size_t iters, double threshold, std::uint64_t seed) {
    const std::size_t k = min_points(model), n = src.size();
    if (n < k) return std::nullopt;
    Rng rng{seed ^ 0xA11C0DE5ULL};
    const auto inliers_of = [&](const Homography& h) {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < n; ++i) {
            const auto [x, y] = h.apply(src[i][0], src[i][1]);
            if (std::isfinite(x) && std::hypot(x - dst[i][0], y - dst[i][1]) <= threshold) out.push_back(i);
        }
        return out;
    };
    std::optional<std::pair<Homography, std::vector<std::size_t>>> best;
    for (std::size_t it = 0; it < std::max<std::size_t>(iters, 1); ++it) {
        std::vector<std::size_t> idx;
        idx.reserve(k);
        while (idx.size() < k) {
            const std::size_t i = rng.below(n);
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i);
            if (n == k) {
                idx.clear();
                for (std::size_t j = 0; j < k; ++j) idx.push_back(j);
            }
        }
        std::vector<XY> s, d;
        for (const std::size_t i : idx) {
            s.push_back(src[i]);
            d.push_back(dst[i]);
        }
        const auto h = fit(model, s, d);
        if (!h) continue;
        std::vector<std::size_t> inl = inliers_of(*h);
        if (!best || inl.size() > best->second.size()) best.emplace(*h, std::move(inl));
    }
    if (!best || best->second.size() < k) return std::nullopt;
    // (fitted again to all the inliers, then gathered again)
    std::vector<XY> s, d;
    for (const std::size_t i : best->second) {
        s.push_back(src[i]);
        d.push_back(dst[i]);
    }
    const Homography h2 = fit(model, s, d).value_or(best->first);
    std::vector<std::size_t> inl2 = inliers_of(h2);
    if (inl2.size() >= best->second.size()) return std::pair<Homography, std::vector<std::size_t>>{h2, std::move(inl2)};
    return best;
}

std::optional<std::pair<Homography, std::size_t>> register_pair(std::size_t w, std::size_t h, const std::vector<float>& reference,
                                                                const std::vector<float>& moving, const std::vector<bool>* valid_ref,
                                                                const std::vector<bool>* valid_mov, Model model) {
    const bool upright = model == Model::Translation;
    const float min_dist = std::clamp(static_cast<float>(std::max(w, h)) / 60.0f, 4.0f, 24.0f);
    const std::vector<Corner> ca = harris(w, h, reference, 600, min_dist, valid_ref);
    const std::vector<Corner> cb = harris(w, h, moving, 600, min_dist, valid_mov);
    if (ca.size() < 4 || cb.size() < 4) return std::nullopt;
    const std::vector<Descriptor> da = describe(w, h, reference, ca, upright), db = describe(w, h, moving, cb, upright);
    const auto matches = match_descriptors(db, da, 0.85f);
    std::vector<XY> src, dst;
    for (const auto& [i, j] : matches) {
        src.push_back(XY{cb[i].x, cb[i].y});
        dst.push_back(XY{ca[j].x, ca[j].y});
    }
    const double threshold = std::clamp(static_cast<double>(std::max(w, h)) / 400.0, 1.5, 4.0);
    const auto r = ransac(model, src, dst, 1000, threshold, 7);
    if (!r || r->second.size() < std::max<std::size_t>(min_points(model), 6)) return std::nullopt;
    return std::pair<Homography, std::size_t>{r->first, r->second.size()};
}

}  // namespace genko::render::features
