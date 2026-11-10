// PC3 B3: Select and Mask's edge refinement, the colours decontaminated and Layer › Matting — PhotoCraft v0.2.0's
// algo/src/matting.rs and engine/src/layer_menu_cmds.rs (defringe, remove_matte), MIT
// (native/third_party/photocraft/NOTICE.md).

#include "render/matting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/image_ops.hpp"
#include "render/selection_ext.hpp"

namespace genko::render::matting {

namespace {

constexpr int kTile = 256;

std::vector<Rect> tiles_of(const Rect& area) {
    std::vector<Rect> tiles;
    for (int y = area.y0; y < area.y1; y += kTile)
        for (int x = area.x0; x < area.x1; x += kTile) tiles.push_back(Rect{x, y, std::min(x + kTile, area.x1), std::min(y + kTile, area.y1)});
    return tiles;
}

Rect inflate(const Rect& r, int by) { return Rect{r.x0 - by, r.y0 - by, r.x1 + by, r.y1 + by}; }
Rect intersect(const Rect& a, const Rect& b) {
    Rect out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (out.empty()) return Rect{};
    return out;
}

// van Herk / Gil–Werman: three comparisons a sample, whatever the radius.
void running_extreme(const float* src, std::size_t n, float* out, std::size_t r, bool max, std::vector<float>& g, std::vector<float>& hb) {
    const std::size_t k = 2 * r + 1;
    const auto f = [max](float a, float b) { return max ? std::max(a, b) : std::min(a, b); };
    const float pad = max ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    const std::size_t m = ((n + 2 * r + k - 1) / k) * k;
    g.assign(m, pad);
    hb.assign(m, pad);
    const auto p = [&](std::size_t j) { return j >= r && j < r + n ? src[j - r] : pad; };
    for (std::size_t j = 0; j < m; ++j) g[j] = j % k == 0 ? p(j) : f(g[j - 1], p(j));
    for (std::size_t j = m; j-- > 0;) hb[j] = (j % k == k - 1 || j == m - 1) ? p(j) : f(hb[j + 1], p(j));
    for (std::size_t i = 0; i < n; ++i) out[i] = f(hb[i], g[i + 2 * r]);
}

}  // namespace

// --- the settings -----------------------------------------------------------------------------------------------------

std::size_t RefineParams::gf_radius() const { return static_cast<std::size_t>(std::clamp(std::ceil(radius), 1.0f, 64.0f)); }
float RefineParams::smooth_sigma() const { return std::clamp(smooth, 0.0f, 100.0f) / 100.0f * 4.0f; }
float RefineParams::feather_sigma() const { return std::max(feather, 0.0f) / 2.0f; }
int RefineParams::shift_px() const {
    return static_cast<int>(std::clamp(std::round(std::clamp(shift_edge, -100.0f, 100.0f) / 100.0f * std::max(radius, 4.0f)), -64.0f, 64.0f));
}
int RefineParams::halo() const {
    int edge = 0;
    if (uses_image()) {
        const int g = static_cast<int>(gf_radius());
        edge = std::max(static_cast<int>(std::ceil(radius)), 2 * g + (smart_radius ? g + 1 : 0));
    }
    return edge + static_cast<int>(std::ceil(3.0f * smooth_sigma())) + static_cast<int>(std::ceil(3.0f * feather_sigma())) + std::abs(shift_px()) + 2;
}
int RefineParams::spread() const {
    return (uses_image() ? static_cast<int>(std::ceil(radius)) : 0) + static_cast<int>(std::ceil(3.0f * smooth_sigma())) +
           static_cast<int>(std::ceil(3.0f * feather_sigma())) + std::max(shift_px(), 0) + 1;
}

RefineParams enhance_params() {
    RefineParams p;
    p.radius = 4;
    p.smart_radius = true;
    p.smooth = 15;
    p.contrast = 25;
    return p;
}

// --- filters ----------------------------------------------------------------------------------------------------------

std::vector<float> box_mean(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r) {
    std::vector<float> tmp(w * h, 0.0f);
    std::vector<double> pre(std::max(w, h) + 1, 0.0);
    for (std::size_t y = 0; y < h; ++y) {
        const float* row = src.data() + y * w;
        for (std::size_t x = 0; x < w; ++x) pre[x + 1] = pre[x] + static_cast<double>(row[x]);
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t lo = x > r ? x - r : 0, hi = std::min(x + r + 1, w);
            tmp[y * w + x] = static_cast<float>((pre[hi] - pre[lo]) / static_cast<double>(hi - lo));
        }
    }
    std::vector<float> out(w * h, 0.0f);
    std::vector<double> col((h + 1) * w, 0.0);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) col[(y + 1) * w + x] = col[y * w + x] + static_cast<double>(tmp[y * w + x]);
    for (std::size_t y = 0; y < h; ++y) {
        const std::size_t lo = y > r ? y - r : 0, hi = std::min(y + r + 1, h);
        const double n = static_cast<double>(hi - lo);
        for (std::size_t x = 0; x < w; ++x) out[y * w + x] = static_cast<float>((col[hi * w + x] - col[lo * w + x]) / n);
    }
    return out;
}

std::vector<float> guided_filter_gray(const std::vector<float>& guide, const std::vector<float>& p, std::size_t w, std::size_t h, std::size_t r,
                                      float eps) {
    const std::size_t n = w * h;
    const std::vector<float> mi = box_mean(guide, w, h, r);
    const std::vector<float> mp = box_mean(p, w, h, r);
    std::vector<float> ip(n), ii(n);
    for (std::size_t i = 0; i < n; ++i) {
        ip[i] = guide[i] * p[i];
        ii[i] = guide[i] * guide[i];
    }
    const std::vector<float> mip = box_mean(ip, w, h, r);
    const std::vector<float> mii = box_mean(ii, w, h, r);
    std::vector<float> a(n), b(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float var = mii[i] - mi[i] * mi[i];
        const float cov = mip[i] - mi[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mi[i];
    }
    const std::vector<float> ma = box_mean(a, w, h, r);
    const std::vector<float> mb = box_mean(b, w, h, r);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = ma[i] * guide[i] + mb[i];
    return out;
}

std::vector<float> guided_filter_color(const RgbImage& guide, const std::vector<float>& p, std::size_t r, float eps) {
    const std::size_t w = guide.w, h = guide.h, n = w * h;
    std::array<std::vector<float>, 3> chans;
    for (std::size_t c = 0; c < 3; ++c) {
        chans[c].resize(n);
        for (std::size_t i = 0; i < n; ++i) chans[c][i] = guide.px[i][c];
    }
    std::array<std::vector<float>, 3> m, mip;
    std::vector<float> tmp(n);
    for (std::size_t c = 0; c < 3; ++c) {
        m[c] = box_mean(chans[c], w, h, r);
        for (std::size_t i = 0; i < n; ++i) tmp[i] = chans[c][i] * p[i];
        mip[c] = box_mean(tmp, w, h, r);
    }
    const std::vector<float> mp = box_mean(p, w, h, r);
    constexpr std::pair<std::size_t, std::size_t> kPairs[6] = {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 2}, {2, 2}};
    std::array<std::vector<float>, 6> mii;
    for (std::size_t k = 0; k < 6; ++k) {
        for (std::size_t i = 0; i < n; ++i) tmp[i] = chans[kPairs[k].first][i] * chans[kPairs[k].second][i];
        mii[k] = box_mean(tmp, w, h, r);
    }
    std::array<std::vector<float>, 3> av{std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
    std::vector<float> bv(n);
    const double e = static_cast<double>(eps);
    for (std::size_t i = 0; i < n; ++i) {
        const double mu[3] = {m[0][i], m[1][i], m[2][i]};
        const auto s = [&](std::size_t k, std::size_t a, std::size_t b) { return static_cast<double>(mii[k][i]) - mu[a] * mu[b]; };
        const double rr = s(0, 0, 0) + e, rg = s(1, 0, 1), rb = s(2, 0, 2), gg = s(3, 1, 1) + e, gb = s(4, 1, 2), bb = s(5, 2, 2) + e;
        const double cov[3] = {static_cast<double>(mip[0][i]) - mu[0] * static_cast<double>(mp[i]), static_cast<double>(mip[1][i]) - mu[1] * static_cast<double>(mp[i]),
                               static_cast<double>(mip[2][i]) - mu[2] * static_cast<double>(mp[i])};
        // (the symmetric 3×3 inverted)
        const double i00 = gg * bb - gb * gb, i01 = gb * rb - rg * bb, i02 = rg * gb - gg * rb;
        const double i11 = rr * bb - rb * rb, i12 = rb * rg - rr * gb, i22 = rr * gg - rg * rg;
        const double det = rr * i00 + rg * i01 + rb * i02;
        double a0 = 0, a1 = 0, a2 = 0;
        if (std::abs(det) >= 1e-30) {
            a0 = (i00 * cov[0] + i01 * cov[1] + i02 * cov[2]) / det;
            a1 = (i01 * cov[0] + i11 * cov[1] + i12 * cov[2]) / det;
            a2 = (i02 * cov[0] + i12 * cov[1] + i22 * cov[2]) / det;
        }
        av[0][i] = static_cast<float>(a0);
        av[1][i] = static_cast<float>(a1);
        av[2][i] = static_cast<float>(a2);
        bv[i] = static_cast<float>(static_cast<double>(mp[i]) - a0 * mu[0] - a1 * mu[1] - a2 * mu[2]);
    }
    std::array<std::vector<float>, 3> ma;
    for (std::size_t c = 0; c < 3; ++c) ma[c] = box_mean(av[c], w, h, r);
    const std::vector<float> mb = box_mean(bv, w, h, r);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = ma[0][i] * chans[0][i] + ma[1][i] * chans[1][i] + ma[2][i] * chans[2][i] + mb[i];
    return out;
}

std::vector<float> gaussian_blur(const std::vector<float>& src, std::size_t w, std::size_t h, float sigma) {
    if (sigma < 0.1f) return src;
    const auto r = static_cast<std::int64_t>(std::ceil(sigma * 3.0f));
    std::vector<float> k;
    for (std::int64_t i = -r; i <= r; ++i) k.push_back(std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma)));
    float s = 0;
    for (const float v : k) s += v;
    for (float& v : k) v /= s;
    const auto wi = static_cast<std::int64_t>(w), hi = static_cast<std::int64_t>(h);
    std::vector<float> tmp(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float a = 0;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const auto xx = static_cast<std::size_t>(std::clamp(static_cast<std::int64_t>(x) + static_cast<std::int64_t>(i) - r, std::int64_t{0}, wi - 1));
                a += src[y * w + xx] * k[i];
            }
            tmp[y * w + x] = a;
        }
    }
    std::vector<float> out(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float a = 0;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const auto yy = static_cast<std::size_t>(std::clamp(static_cast<std::int64_t>(y) + static_cast<std::int64_t>(i) - r, std::int64_t{0}, hi - 1));
                a += tmp[yy * w + x] * k[i];
            }
            out[y * w + x] = a;
        }
    }
    return out;
}

std::vector<float> morph(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r, bool grow) {
    std::vector<float> cur = src, nxt = src;
    const auto pick = [grow](float a, float b) { return grow ? std::max(a, b) : std::min(a, b); };
    for (std::size_t k = 0; k < r; ++k) {
        const bool square = k % 2 == 0;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float v = cur[y * w + x];
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx != 0 && dy != 0 && !square) || (dx == 0 && dy == 0)) continue;
                        const std::int64_t nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                        if (nx >= 0 && ny >= 0 && nx < static_cast<std::int64_t>(w) && ny < static_cast<std::int64_t>(h))
                            v = pick(v, cur[static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx)]);
                    }
                }
                nxt[y * w + x] = v;
            }
        }
        std::swap(cur, nxt);
    }
    return cur;
}

std::vector<float> max_square(const std::vector<float>& src, std::size_t w, std::size_t h, std::size_t r, bool max) {
    std::vector<float> g, hb;
    std::vector<float> tmp(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) running_extreme(src.data() + y * w, w, tmp.data() + y * w, r, max, g, hb);
    std::vector<float> out(w * h, 0.0f), col(h), res(h);
    for (std::size_t x = 0; x < w; ++x) {
        for (std::size_t y = 0; y < h; ++y) col[y] = tmp[y * w + x];
        running_extreme(col.data(), h, res.data(), r, max, g, hb);
        for (std::size_t y = 0; y < h; ++y) out[y * w + x] = res[y];
    }
    return out;
}

std::vector<float> edge_width(const RgbImage& img, std::size_t r) {
    const std::size_t w = img.w, h = img.h;
    std::vector<float> y(w * h);
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = 0.299f * img.px[i][0] + 0.587f * img.px[i][1] + 0.114f * img.px[i][2];
    std::vector<float> g(w * h, 0.0f);
    for (std::size_t yy = 0; yy < h; ++yy) {
        for (std::size_t xx = 0; xx < w; ++xx) {
            const auto at = [&](std::size_t x, std::size_t y2) { return y[y2 * w + x]; };
            const float gx = (at(std::min(xx + 1, w - 1), yy) - at(xx > 0 ? xx - 1 : 0, yy)) / 2.0f;
            const float gy = (at(xx, std::min(yy + 1, h - 1)) - at(xx, yy > 0 ? yy - 1 : 0)) / 2.0f;
            g[yy * w + xx] = std::sqrt(gx * gx + gy * gy);
        }
    }
    const std::vector<float> gm = max_square(g, w, h, r, true);
    const std::vector<float> hi = max_square(y, w, h, r, true);
    const std::vector<float> lo = max_square(y, w, h, r, false);
    std::vector<float> out(w * h);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = (hi[i] - lo[i]) / std::max(gm[i], 1e-4f);
    return out;
}

// --- the refinement ---------------------------------------------------------------------------------------------------

std::vector<float> refine_buffer(const RgbImage* img, const std::vector<float>& m, std::size_t w, std::size_t h, const RefineParams& p) {
    std::vector<float> a = m;
    if (p.uses_image() && img != nullptr) {
        std::vector<bool> boundary(w * h, false);
        bool any = false;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = y * w + x;
                const bool v = m[i] >= 0.5f;
                const bool diff = (x > 0 && (m[i - 1] >= 0.5f) != v) || (x + 1 < w && (m[i + 1] >= 0.5f) != v) || (y > 0 && (m[i - w] >= 0.5f) != v) ||
                                  (y + 1 < h && (m[i + w] >= 0.5f) != v);
                boundary[i] = diff;
                any = any || diff;
            }
        }
        if (any) {
            const std::vector<float> dist = selection_ext::edt(boundary, w, h);
            const std::size_t r = p.gf_radius();
            const std::vector<float> q = guided_filter_color(*img, m, r, kEps);
            std::optional<std::vector<float>> widths;
            if (p.smart_radius) widths = edge_width(*img, r);
            for (std::size_t i = 0; i < w * h; ++i) {
                const float reff = widths ? std::clamp((*widths)[i], 1.5f, std::max(p.radius, 1.5f)) : p.radius;
                const float wgt = std::clamp(reff + 1.0f - dist[i], 0.0f, 1.0f);
                if (wgt > 0.0f) a[i] = m[i] + (std::clamp(q[i], 0.0f, 1.0f) - m[i]) * wgt;
            }
        }
    }
    const float ss = p.smooth_sigma();
    if (ss >= 0.3f) {
        a = gaussian_blur(a, w, h, ss);
        const float gain = 1.0f + ss;
        for (float& v : a) v = std::clamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
    }
    a = gaussian_blur(a, w, h, p.feather_sigma());
    if (p.contrast > 0.0f) {
        const float gain = 1.0f / (1.0f - 0.99f * std::clamp(p.contrast, 0.0f, 100.0f) / 100.0f);
        for (float& v : a) v = std::clamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
    }
    const int d = p.shift_px();
    if (d != 0) a = morph(a, w, h, static_cast<std::size_t>(std::abs(d)), d > 0);
    return a;
}

std::optional<Region> refine_mask(const segment::Sampler& sampler, const MaskReader& mask, const Rect& content_given, const Rect& canvas,
                                  const RefineParams& p) {
    const Rect content = intersect(content_given, canvas);
    if (content.empty()) return std::nullopt;
    const Rect bbox = intersect(inflate(content, p.spread()), canvas);
    const int halo = p.halo();
    const auto bw = static_cast<std::size_t>(bbox.width());
    std::vector<std::uint8_t> out(bw * static_cast<std::size_t>(bbox.height()), 0);
    const auto q = [](float v) { return static_cast<std::uint8_t>(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (const Rect& t : tiles_of(bbox)) {
        segment::check_stop();
        const Rect r = intersect(inflate(t, halo), canvas);
        const std::vector<float> m = mask(r);
        const auto rw = static_cast<std::size_t>(r.width()), rh = static_cast<std::size_t>(r.height());
        const auto cx = static_cast<std::size_t>(t.x0 - r.x0), cy = static_cast<std::size_t>(t.y0 - r.y0);
        const auto cw = static_cast<std::size_t>(t.width()), ch = static_cast<std::size_t>(t.height());
        const float first = m.empty() ? 0.0f : m.front();
        const bool flat = (first == 0.0f || first == 1.0f) && std::all_of(m.begin(), m.end(), [first](float v) { return v == first; });
        std::vector<float> refined;
        if (!flat) {
            std::optional<RgbImage> img;
            if (p.uses_image()) img = sampler.rgb(r);
            refined = refine_buffer(img ? &*img : nullptr, m, rw, rh, p);
        }
        for (std::size_t yy = 0; yy < ch; ++yy) {
            const std::size_t o = (static_cast<std::size_t>(t.y0 - bbox.y0) + yy) * bw + static_cast<std::size_t>(t.x0 - bbox.x0);
            for (std::size_t xx = 0; xx < cw; ++xx) out[o + xx] = flat ? q(first) : q(refined[(cy + yy) * rw + cx + xx]);
        }
    }
    return segment::trim_region(Region{bbox, std::move(out)});
}

MaskReader buffer_reader(const std::vector<float>& m, std::size_t w, std::size_t h) {
    return [&m, w, h](const Rect& r) {
        std::vector<float> v;
        v.reserve(static_cast<std::size_t>(std::max(0, r.width())) * static_cast<std::size_t>(std::max(0, r.height())));
        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) {
                const bool inside = x >= 0 && y >= 0 && static_cast<std::size_t>(x) < w && static_cast<std::size_t>(y) < h;
                v.push_back(inside ? m[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] : 0.0f);
            }
        }
        return v;
    };
}

// --- Layer › Matting --------------------------------------------------------------------------------------------------

void decontaminate(std::vector<Rgba>& px, std::size_t w, std::size_t h, const std::vector<float>& matte, float radius, float amount_given) {
    const auto rad = static_cast<std::size_t>(std::ceil(std::max(radius, 3.0f)));
    const int halo = static_cast<int>(rad * 4);
    const float amount = std::clamp(amount_given, 0.0f, 100.0f) / 100.0f;
    const auto soft = [](float a) { return a > 0.02f && a < 0.98f; };
    const Rect whole{0, 0, static_cast<int>(w), static_cast<int>(h)};
    struct Done {
        Rect t;
        std::vector<Rgba> px;
    };
    std::vector<Done> results;
    for (const Rect& t : tiles_of(whole)) {
        bool any = false;
        for (int y = t.y0; y < t.y1 && !any; ++y)
            for (int x = t.x0; x < t.x1 && !any; ++x) any = soft(matte[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]);
        if (!any) continue;
        segment::check_stop();
        const Rect r = intersect(inflate(t, halo), whole);
        const auto rw = static_cast<std::size_t>(r.width()), rh = static_cast<std::size_t>(r.height());
        const image_ops::Held held(std::uint64_t(rw) * rh * sizeof(float) * 12);
        const auto at = [&](std::size_t i) { return (static_cast<std::size_t>(r.y0) + i / rw) * w + static_cast<std::size_t>(r.x0) + i % rw; };
        std::vector<float> wts(rw * rh);
        for (std::size_t i = 0; i < wts.size(); ++i) wts[i] = matte[at(i)] >= 0.98f ? px[at(i)][3] : 0.0f;
        const std::vector<float> den = box_mean(wts, rw, rh, rad);
        const std::vector<float> den2 = box_mean(wts, rw, rh, rad * 4);
        std::array<std::vector<float>, 3> num, num2;
        std::vector<float> v(rw * rh);
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t i = 0; i < v.size(); ++i) v[i] = wts[i] * px[at(i)][c];
            num[c] = box_mean(v, rw, rh, rad);
            num2[c] = box_mean(v, rw, rh, rad * 4);
        }
        const auto cx = static_cast<std::size_t>(t.x0 - r.x0), cy = static_cast<std::size_t>(t.y0 - r.y0);
        const auto cw = static_cast<std::size_t>(t.width()), ch = static_cast<std::size_t>(t.height());
        Done done{t, {}};
        done.px.reserve(cw * ch);
        for (std::size_t yy = 0; yy < ch; ++yy) {
            for (std::size_t xx = 0; xx < cw; ++xx) {
                const std::size_t i = (cy + yy) * rw + cx + xx;
                const float a = matte[at(i)];
                const float k = soft(a) ? amount * std::clamp((1.0f - a) * 4.0f, 0.0f, 1.0f) : 0.0f;
                Rgba out = px[at(i)];
                if (k != 0.0f) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        const float val = out[c];
                        const float f = den[i] > 1e-4f ? num[c][i] / den[i] : den2[i] > 1e-4f ? num2[c][i] / den2[i] : val;
                        out[c] = val + (f - val) * k;
                    }
                }
                done.px.push_back(out);
            }
        }
        results.push_back(std::move(done));
    }
    for (const Done& d : results) {
        const auto cw = static_cast<std::size_t>(d.t.width());
        for (std::size_t i = 0; i < d.px.size(); ++i) px[(static_cast<std::size_t>(d.t.y0) + i / cw) * w + static_cast<std::size_t>(d.t.x0) + i % cw] = d.px[i];
    }
}

void defringe(std::vector<Rgba>& px, std::size_t w, std::size_t h, std::size_t width) {
    std::vector<bool> outside(px.size());
    bool any = false;
    for (std::size_t i = 0; i < px.size(); ++i) {
        outside[i] = px[i][3] <= 0.01f;
        any = any || outside[i];
    }
    if (!any) return;
    const std::vector<float> dist = selection_ext::edt(outside, w, h);
    std::vector<std::uint8_t> known(px.size(), 0);
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < px.size(); ++i) {
        known[i] = !outside[i] && dist[i] > static_cast<float>(width) ? 1 : 0;
        if (!outside[i] && known[i] == 0) todo.push_back(i);
    }
    constexpr int kAround[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    while (!todo.empty()) {
        segment::check_stop();
        std::vector<std::size_t> next;
        std::vector<std::pair<std::size_t, std::array<float, 3>>> updates;
        for (const std::size_t i : todo) {
            const auto x = static_cast<std::int64_t>(i % w), y = static_cast<std::int64_t>(i / w);
            std::array<float, 3> acc{0, 0, 0};
            float n = 0;
            for (const auto& d : kAround) {
                const std::int64_t nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= static_cast<std::int64_t>(w) || ny >= static_cast<std::int64_t>(h)) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                if (known[j] == 0) continue;
                for (std::size_t c = 0; c < 3; ++c) acc[c] += px[j][c];
                n += 1.0f;
            }
            if (n > 0.0f) {
                updates.emplace_back(i, std::array<float, 3>{acc[0] / n, acc[1] / n, acc[2] / n});
            } else {
                next.push_back(i);
            }
        }
        if (updates.empty()) break;  // (a fringe with no core colour near it)
        for (const auto& [i, c] : updates) {
            for (std::size_t k = 0; k < 3; ++k) px[i][k] = c[k];
            known[i] = 1;
        }
        todo = std::move(next);
    }
}

void remove_matte(std::vector<Rgba>& px, bool white) {
    for (Rgba& q : px) {
        const float a = q[3];
        if (!(a > 0.0f && a < 1.0f)) continue;
        for (std::size_t c = 0; c < 3; ++c) q[c] = std::clamp(white ? (q[c] - (1.0f - a)) / a : q[c] / a, 0.0f, 1.0f);
    }
}

}  // namespace genko::render::matting
