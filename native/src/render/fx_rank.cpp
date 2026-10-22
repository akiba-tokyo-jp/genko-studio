// PC4 節 D1 (SPEC.md COLOR-08): the rank filters of フィルター's kernels and the pieces they share (render/fx_rank.hpp).
// PhotoCraft v0.2.0's algo/src/noise.rs (hash01, median), other.rs (min_max, min_max_round, running_extreme) and blur.rs
// (surface), MIT: native/third_party/photocraft/NOTICE.md. The fast algorithms (design 4.3) are Genko's: Huang's sliding
// histogram ("A fast two-dimensional median filtering algorithm", 1979) with two levels of bins, and van Herk / Gil-Werman
// running extremes along the rows and columns; their bins hold whole numbers, so a sample's result is the same whichever
// window — whatever tile — holds it.

#include "render/fx_rank.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace genko::render::fx {

namespace {

// A sample's key on a grid of `levels` (0..levels), and back as the surfaces read it (an 8-bit layer's byte / 255 in
// floats, a 16-bit one's in doubles).
int key_of(float v, unsigned levels) {
    const long k = std::lround(static_cast<double>(v) * levels);
    return static_cast<int>(std::clamp<long>(k, 0, static_cast<long>(levels)));
}
float value_of(int k, unsigned levels) {
    if (levels == 255) return static_cast<float>(k) / 255.0f;
    return static_cast<float>(k / static_cast<double>(levels));
}

// The disc's half widths: row dy (−r..r) holds dx with dx² + dy² ≤ r² + r (noise.rs's), at index dy + r.
std::vector<int> disc_rows(int r) {
    std::vector<int> out;
    const long most = long(r) * r + r;
    for (int dy = -r; dy <= r; ++dy) {
        int hw = static_cast<int>(std::floor(std::sqrt(static_cast<double>(most - long(dy) * dy))));
        while (long(hw + 1) * (hw + 1) + long(dy) * dy <= most) ++hw;
        while (hw > 0 && long(hw) * hw + long(dy) * dy > most) --hw;
        out.push_back(hw);
    }
    return out;
}

// --- the median --------------------------------------------------------------------------------------------------------

// Two levels of counts (each key's and each 16 keys'), the coarse bin of the median found last and the count below it.
struct Bins {
    std::vector<std::int32_t> fine, coarse;
    int cb = 0;
    std::int64_t below = 0;
    void reset(std::size_t keys) {
        fine.assign(keys, 0);
        coarse.assign((keys + 15) / 16, 0);
        cb = 0;
        below = 0;
    }
    void add(int k) {
        ++fine[std::size_t(k)];
        ++coarse[std::size_t(k >> 4)];
        if ((k >> 4) < cb) ++below;
    }
    void remove(int k) {
        --fine[std::size_t(k)];
        --coarse[std::size_t(k >> 4)];
        if ((k >> 4) < cb) --below;
    }
    // The key of rank m (0 the least; m below the count held).
    int at(std::int64_t m) {
        while (below > m) {
            --cb;
            below -= coarse[std::size_t(cb)];
        }
        while (below + coarse[std::size_t(cb)] <= m) {
            below += coarse[std::size_t(cb)];
            ++cb;
        }
        std::int64_t acc = below;
        for (int k = cb << 4;; ++k) {
            acc += fine[std::size_t(k)];
            if (acc > m) return k;
        }
    }
};

std::vector<float> median_naive(const Window& src, const Rect& out, int r, std::optional<float> threshold) {
    const std::size_t n = src.ch;
    std::vector<std::pair<int, int>> offs;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r + r) offs.emplace_back(dx, dy);
    std::vector<float> vals(offs.size()), res;
    res.reserve(out.area() * n);
    const std::size_t mid = offs.size() / 2;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x)
            for (std::size_t c = 0; c < n; ++c) {
                for (std::size_t i = 0; i < offs.size(); ++i) vals[i] = src.at(x + offs[i].first, y + offs[i].second, c);
                std::nth_element(vals.begin(), vals.begin() + static_cast<std::ptrdiff_t>(mid), vals.end());
                const float m = vals[mid];
                const float o = src.at(x, y, c);
                res.push_back(threshold && std::fabs(o - m) <= *threshold ? o : m);
            }
    return res;
}

std::vector<float> median_fast(const Window& src, const Rect& out, unsigned levels, int r, std::optional<float> threshold) {
    const std::size_t n = src.ch;
    const std::vector<int> rows = disc_rows(r);
    std::int64_t count = 0;
    for (const int hw : rows) count += 2 * hw + 1;
    const std::int64_t mid = count / 2;
    // (each sample's key once: the window's)
    const Rect& win = src.rect;
    std::vector<std::int32_t> keys(src.data.size());
    for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = key_of(src.data[i], levels);
    const auto key = [&](int x, int y, std::size_t c) {
        const int cx = std::clamp(x, win.x0, win.x1 - 1), cy = std::clamp(y, win.y0, win.y1 - 1);
        return keys[src.index(cx, cy) + c];
    };
    std::vector<Bins> bins(n);
    for (Bins& b : bins) b.reset(std::size_t(levels) + 1);
    std::vector<float> res(out.area() * n);
    const std::size_t ow = std::size_t(out.width());
    for (int y = out.y0; y < out.y1; ++y) {
        // (the disc at the row's first pixel, slid along it; taken out again after its last)
        for (std::size_t c = 0; c < n; ++c) {
            Bins& b = bins[c];
            b.cb = 0;
            b.below = 0;
            for (int dy = -r; dy <= r; ++dy) {
                const int hw = rows[std::size_t(dy + r)];
                for (int dx = -hw; dx <= hw; ++dx) b.add(key(out.x0 + dx, y + dy, c));
            }
        }
        for (int x = out.x0; x < out.x1; ++x) {
            float* dst = res.data() + (std::size_t(y - out.y0) * ow + std::size_t(x - out.x0)) * n;
            for (std::size_t c = 0; c < n; ++c) {
                const float m = value_of(bins[c].at(mid), levels);
                const float o = src.at(x, y, c);
                dst[c] = threshold && std::fabs(o - m) <= *threshold ? o : m;
            }
            if (x + 1 >= out.x1) break;
            for (std::size_t c = 0; c < n; ++c) {
                Bins& b = bins[c];
                for (int dy = -r; dy <= r; ++dy) {
                    const int hw = rows[std::size_t(dy + r)];
                    b.remove(key(x - hw, y + dy, c));
                    b.add(key(x + 1 + hw, y + dy, c));
                }
            }
        }
        for (std::size_t c = 0; c < n; ++c) {
            Bins& b = bins[c];
            for (int dy = -r; dy <= r; ++dy) {
                const int hw = rows[std::size_t(dy + r)];
                for (int dx = -hw; dx <= hw; ++dx) b.remove(key(out.x1 - 1 + dx, y + dy, c));
            }
        }
    }
    return res;
}

// --- the extremes ------------------------------------------------------------------------------------------------------

float pick(bool max, float a, float b) { return max ? std::max(a, b) : std::min(a, b); }

// out[i] = the extreme of v[i .. i + k) for each whole window (other.rs running_extreme: prefix extremes within blocks
// of k and suffix extremes within them, one of each).
void running_extreme(const std::vector<float>& v, std::size_t k, bool max, std::vector<float>& out, std::vector<float>& pre, std::vector<float>& suf) {
    out.clear();
    const std::size_t n = v.size();
    if (k <= 1) {
        out = v;
        return;
    }
    if (n < k) return;
    pre = v;
    suf = v;
    for (std::size_t i = 1; i < n; ++i)
        if (i % k != 0) pre[i] = pick(max, pre[i - 1], v[i]);
    for (std::size_t i = n - 1; i-- > 0;)
        if ((i + 1) % k != 0) suf[i] = pick(max, suf[i + 1], v[i]);
    for (std::size_t i = 0; i + k <= n; ++i) out.push_back(pick(max, suf[i], pre[i + k - 1]));
}

// The disc's half widths (other.rs min_max_round: the epsilon keeps exact squares — radius 5, dy 3 → 4).
int round_half_width(double radius, int dy) { return static_cast<int>(std::floor(std::sqrt(std::max(0.0, radius * radius - double(dy) * dy)) + 1e-4)); }

std::vector<float> extreme_naive(const Window& src, const Rect& out, double radius, bool square, bool max) {
    const std::size_t n = src.ch;
    std::vector<float> res;
    res.reserve(out.area() * n);
    const int ri = square ? static_cast<int>(std::lround(std::max(0.0, radius))) : static_cast<int>(std::floor(std::max(0.0, radius)));
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x)
            for (std::size_t c = 0; c < n; ++c) {
                float m = src.at(x, y, c);
                for (int dy = -ri; dy <= ri; ++dy) {
                    const int hw = square ? ri : round_half_width(radius, dy);
                    for (int dx = -hw; dx <= hw; ++dx) m = pick(max, m, src.at(x + dx, y + dy, c));
                }
                res.push_back(m);
            }
    return res;
}

std::vector<float> extreme_fast(const Window& src, const Rect& out, double radius, bool square, bool max) {
    const std::size_t n = src.ch;
    const std::size_t ow = std::size_t(out.width()), oh = std::size_t(out.height());
    std::vector<float> res(out.area() * n, max ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity());
    std::vector<float> line, ext, pre, suf;
    if (square) {
        const int r = static_cast<int>(std::lround(std::max(0.0, radius)));
        if (r == 0) return src.crop(out);
        // (along the rows over the rows the columns need, then along the columns)
        const std::size_t th = oh + 2 * std::size_t(r);
        std::vector<float> rows(ow * th * n);
        for (std::size_t c = 0; c < n; ++c) {
            for (std::size_t ty = 0; ty < th; ++ty) {
                const int y = out.y0 - r + int(ty);
                line.clear();
                for (int x = out.x0 - r; x < out.x1 + r; ++x) line.push_back(src.at(x, y, c));
                running_extreme(line, std::size_t(2 * r + 1), max, ext, pre, suf);
                for (std::size_t x = 0; x < ow; ++x) rows[(ty * ow + x) * n + c] = ext[x];
            }
            for (std::size_t x = 0; x < ow; ++x) {
                line.clear();
                for (std::size_t ty = 0; ty < th; ++ty) line.push_back(rows[(ty * ow + x) * n + c]);
                running_extreme(line, std::size_t(2 * r + 1), max, ext, pre, suf);
                for (std::size_t y = 0; y < oh; ++y) res[(y * ow + x) * n + c] = ext[y];
            }
        }
        return res;
    }
    const int ri = static_cast<int>(std::floor(std::max(0.0, radius)));
    if (ri == 0) return src.crop(out);
    for (int dy = -ri; dy <= ri; ++dy) {
        const int hw = round_half_width(radius, dy);
        for (int y = out.y0; y < out.y1; ++y)
            for (std::size_t c = 0; c < n; ++c) {
                line.clear();
                for (int x = out.x0 - hw; x < out.x1 + hw; ++x) line.push_back(src.at(x, y + dy, c));
                running_extreme(line, std::size_t(2 * hw + 1), max, ext, pre, suf);
                float* row = res.data() + std::size_t(y - out.y0) * ow * n;
                for (std::size_t x = 0; x < ow; ++x) row[x * n + c] = pick(max, row[x * n + c], ext[x]);
            }
    }
    return res;
}

// --- ぼかし（表面） ------------------------------------------------------------------------------------------------------

std::vector<float> surface_naive(const Window& src, const Rect& out, int r, float t) {
    const std::size_t n = src.ch, cc = n - 1;
    std::vector<float> res;
    res.reserve(out.area() * n);
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x) {
            for (std::size_t c = 0; c < cc; ++c) {
                const double v0 = src.at(x, y, c);
                double acc = 0, wsum = 0;
                for (int yy = y - r; yy <= y + r; ++yy)
                    for (int xx = x - r; xx <= x + r; ++xx) {
                        if (!(src.at(xx, yy, cc) > 0.0f)) continue;
                        const double v = src.at(xx, yy, c);
                        const double w = std::max(0.0, 1.0 - std::fabs(v - v0) / t);
                        acc += v * w;
                        wsum += w;
                    }
                res.push_back(wsum > 0 ? static_cast<float>(acc / wsum) : static_cast<float>(v0));
            }
            res.push_back(src.at(x, y, cc));
        }
    return res;
}

// The bins of one colour slid along a row: each fine bin's count and its values' sum (whole units), each coarse bin's —
// and the sums of the fine bins' centres (`mul`·`centre` units) times their counts and their values.
struct Weighted {
    int fine_bits = 4;
    double width = 1;   // (a fine bin's width, units)
    double centre = 1;  // (a centre's unit: mul[b]·centre units)
    std::vector<std::int64_t> mul, h, s, hc, sc, pc, qc;

    void reset(std::size_t bins, int bits) {
        fine_bits = bits;
        h.assign(bins, 0);
        s.assign(bins, 0);
        const std::size_t coarse = (bins + (std::size_t(1) << bits) - 1) >> bits;
        hc.assign(coarse, 0);
        sc.assign(coarse, 0);
        pc.assign(coarse, 0);
        qc.assign(coarse, 0);
    }
    void add(std::size_t b, std::int64_t v, int sign) {
        const std::size_t c = b >> fine_bits;
        h[b] += sign;
        s[b] += sign * v;
        hc[c] += sign;
        sc[c] += sign * v;
        pc[c] += sign * mul[b];
        qc[c] += sign * mul[b] * v;
    }
    // The weighted mean about v0 (units) with the threshold `tu` (units), and whether anything weighed.
    std::optional<double> mean(double v0, double tu) const {
        const std::size_t bins = h.size(), per = std::size_t(1) << fine_bits;
        const double lo = v0 - tu, hi = v0 + tu;
        double num = 0, den = 0;
        const auto bin_of = [&](double u) { return std::size_t(std::clamp(std::floor(u / width), 0.0, double(bins - 1))); };
        const std::size_t first = bin_of(lo) >> fine_bits, last = bin_of(hi) >> fine_bits;
        for (std::size_t c = first; c <= last && c < hc.size(); ++c) {
            if (hc[c] == 0) continue;
            const std::size_t b0 = c << fine_bits, b1 = std::min(bins, b0 + per) - 1;
            const double c0 = double(mul[b0]) * centre, c1 = double(mul[b1]) * centre;
            if (c0 > lo && c1 <= v0) {  // (wholly below v0, within the threshold: its sums)
                const double a = 1.0 - v0 / tu, k = centre / tu;
                num += a * double(sc[c]) + k * double(qc[c]);
                den += a * double(hc[c]) + k * double(pc[c]);
            } else if (c0 >= v0 && c1 < hi) {  // (wholly above)
                const double a = 1.0 + v0 / tu, k = centre / tu;
                num += a * double(sc[c]) - k * double(qc[c]);
                den += a * double(hc[c]) - k * double(pc[c]);
            } else {
                for (std::size_t b = b0; b <= b1; ++b) {
                    if (h[b] == 0) continue;
                    const double w = 1.0 - std::fabs(double(mul[b]) * centre - v0) / tu;
                    if (!(w > 0)) continue;
                    num += w * double(s[b]);
                    den += w * double(h[b]);
                }
            }
        }
        if (!(den > 0)) return std::nullopt;
        return num / den;
    }
};

std::vector<float> surface_fast(const Window& src, const Rect& out, bool eight, int r, float t) {
    const std::size_t n = src.ch, cc = n - 1;
    // (8 bits: a bin a level, its centre its value; within 0..1: 4096 bins of 2^12 units of 2^-24, their centres (2b + 1)·2^11)
    const std::size_t bins = eight ? 256 : 4096;
    const int bits = eight ? 4 : 6;
    const double unit = eight ? 1.0 / 255.0 : std::ldexp(1.0, -24);
    const Rect& win = src.rect;
    std::vector<std::int64_t> values(src.data.size());
    std::vector<std::uint16_t> bin_of(src.data.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double v = std::clamp(static_cast<double>(src.data[i]), 0.0, 1.0);
        const std::int64_t q = eight ? std::lround(v * 255.0) : std::llround(std::ldexp(v, 24));
        values[i] = q;
        bin_of[i] = static_cast<std::uint16_t>(eight ? q : std::min<std::int64_t>(4095, q >> 12));
    }
    std::vector<Weighted> weighted(cc);
    for (Weighted& w : weighted) {
        w.reset(bins, bits);
        w.width = eight ? 1.0 : 4096.0;
        w.centre = eight ? 1.0 : std::ldexp(1.0, 11);
        w.mul.resize(bins);
        for (std::size_t b = 0; b < bins; ++b) w.mul[b] = eight ? std::int64_t(b) : std::int64_t(2 * b + 1);
    }
    const auto take = [&](int x, int y, int sign) {
        const int cx = std::clamp(x, win.x0, win.x1 - 1), cy = std::clamp(y, win.y0, win.y1 - 1);
        const std::size_t i = src.index(cx, cy);
        if (!(src.data[i + cc] > 0.0f)) return;  // (what does not show weighs nothing)
        for (std::size_t c = 0; c < cc; ++c) weighted[c].add(bin_of[i + c], values[i + c], sign);
    };
    const double tu = t / unit;
    std::vector<float> res(out.area() * n);
    const std::size_t ow = std::size_t(out.width());
    for (int y = out.y0; y < out.y1; ++y) {
        for (int yy = y - r; yy <= y + r; ++yy)
            for (int xx = out.x0 - r; xx <= out.x0 + r; ++xx) take(xx, yy, 1);
        for (int x = out.x0; x < out.x1; ++x) {
            float* dst = res.data() + (std::size_t(y - out.y0) * ow + std::size_t(x - out.x0)) * n;
            for (std::size_t c = 0; c < cc; ++c) {
                const float v0 = src.at(x, y, c);
                const std::optional<double> m = weighted[c].mean(static_cast<double>(v0) / unit, tu);
                dst[c] = m ? static_cast<float>(*m * unit) : v0;
            }
            dst[cc] = src.at(x, y, cc);
            if (x + 1 >= out.x1) break;
            for (int yy = y - r; yy <= y + r; ++yy) {
                take(x - r, yy, -1);
                take(x + 1 + r, yy, 1);
            }
        }
        for (int yy = y - r; yy <= y + r; ++yy)
            for (int xx = out.x1 - 1 - r; xx <= out.x1 - 1 + r; ++xx) take(xx, yy, -1);
    }
    return res;
}

}  // namespace

float hash01(int x, int y, std::uint32_t c, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ c * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

std::vector<float> median(const Window& src, const Rect& out, const Ctx& ctx, int r, std::optional<float> threshold, Method method) {
    if (r <= 0) return src.crop(out);
    const bool exact = ctx.levels == 255 || ctx.levels == 65535;
    // (a small disc is as quick as written; the same values either way)
    if (method == Method::Naive || !exact || (method == Method::Auto && r < 3)) return median_naive(src, out, r, threshold);
    return median_fast(src, out, ctx.levels, r, threshold);
}

std::vector<float> extreme(const Window& src, const Rect& out, double radius, bool square, bool max, Method method) {
    if (method == Method::Naive) return extreme_naive(src, out, radius, square, max);
    return extreme_fast(src, out, radius, square, max);
}

std::vector<float> surface(const Window& src, const Rect& out, const Ctx& ctx, int r, float t, Method method) {
    if (r <= 0) return src.crop(out);
    const bool eight = ctx.levels == 255;
    const bool unit = ctx.least >= 0 && ctx.largest <= 1;
    if (method == Method::Naive || (method == Method::Auto && r <= 8) || (!eight && !unit)) return surface_naive(src, out, r, t);
    return surface_fast(src, out, eight, r, t);
}

float lightness(const Ctx& ctx, const float* px) {
    switch (ctx.space) {
        case Space::Rgb: return 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
        case Space::Gray:
        case Space::Lab: return px[0];
        case Space::Cmyk: {
            const float k = 1.0f - px[3];
            return 0.299f * (1.0f - px[0]) * k + 0.587f * (1.0f - px[1]) * k + 0.114f * (1.0f - px[2]) * k;
        }
        case Space::Multi: {
            float l = 1.0f;
            for (unsigned c = 0; c < ctx.colours; ++c) l *= 1.0f - std::clamp(px[c], 0.0f, 1.0f);
            return l;
        }
    }
    return px[0];
}

}  // namespace genko::render::fx
