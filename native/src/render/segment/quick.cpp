// PC3 B3: the Quick Selection brush — PhotoCraft v0.2.0's algo/src/segment/quick.rs (MIT,
// native/third_party/photocraft/NOTICE.md). A stroke seeds a graph cut in a window around it: the stroked pixels are hard
// foreground and their colours its model; a geodesic distance from the stroke over a barrier map (the colour gradient
// above the texture under the stroke; Toivanen 1996, Criminisi, Sharp & Blake, "GeoS", ECCV 2008) tells what lies behind
// an edge — sampled for the background model (with a uniform outlier density) and weighed in the data term —; a
// contrast-sensitive cut snaps to the picture's edges and only the parts touching the stroke stay (in the spirit of
// "Paint Selection", Liu, Sun & Shum, SIGGRAPH 2009).

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

constexpr float kQReg = 6e-4f;    // (the covariance ridge of the often tiny stroke sample: ~6 levels)
constexpr float kEdge = 0.06f;    // (the geodesic distance that counts as behind an edge)
constexpr float kMinStep = 0.03f; // (neighbour steps below this never count as edges)
constexpr float kKappa = 2.0f;    // (the data term's weight of the distance, nats per kEdge)
constexpr float kGamma = 50.0f;

RgbImage blur3(const RgbImage& img) {
    const std::size_t w = img.w, h = img.h;
    RgbImage tmp = img;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const Rgb &l = img.at(x > 0 ? x - 1 : 0, y), &r = img.at(std::min(x + 1, w - 1), y), &c = img.at(x, y);
            for (std::size_t k = 0; k < 3; ++k) tmp.px[y * w + x][k] = (l[k] + c[k] + r[k]) / 3.0f;
        }
    }
    RgbImage out = tmp;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const Rgb &u = tmp.at(x, y > 0 ? y - 1 : 0), &d = tmp.at(x, std::min(y + 1, h - 1)), &c = tmp.at(x, y);
            for (std::size_t k = 0; k < 3; ++k) out.px[y * w + x][k] = (u[k] + c[k] + d[k]) / 3.0f;
        }
    }
    return out;
}

}  // namespace

std::optional<Region> quick_select(const Sampler& sampler, const Rect& canvas, const std::vector<std::pair<float, float>>& points, float size_given,
                                   std::size_t work_px) {
    if (points.empty()) return std::nullopt;
    const float size = std::max(size_given, 1.0f);
    const float r = size / 2.0f;
    float x0 = std::numeric_limits<float>::max(), y0 = x0, x1 = std::numeric_limits<float>::lowest(), y1 = x1;
    for (const auto& [x, y] : points) {
        x0 = std::min(x0, x - r);
        y0 = std::min(y0, y - r);
        x1 = std::max(x1, x + r);
        y1 = std::max(y1, y + r);
    }
    const Rect bbox{std::max(static_cast<int>(std::floor(x0)), canvas.x0), std::max(static_cast<int>(std::floor(y0)), canvas.y0),
                    std::min(static_cast<int>(std::ceil(x1)) + 1, canvas.x1), std::min(static_cast<int>(std::ceil(y1)) + 1, canvas.y1)};
    if (bbox.empty()) return std::nullopt;
    const int margin = static_cast<int>(std::max(size * 6.0f, 512.0f));
    const Rect window{std::max(bbox.x0 - margin, canvas.x0), std::max(bbox.y0 - margin, canvas.y0), std::min(bbox.x1 + margin, canvas.x1),
                      std::min(bbox.y1 + margin, canvas.y1)};
    const std::size_t step = scale_for(window, work_px);
    const RgbImage img = sampler.rgb_scaled(window, step);
    const std::size_t lw = img.w, lh = img.h;
    const auto st = static_cast<float>(step);
    std::vector<std::pair<float, float>> pts;
    pts.reserve(points.size());
    for (const auto& [x, y] : points) pts.emplace_back((x - static_cast<float>(window.x0)) / st, (y - static_cast<float>(window.y0)) / st);
    const Mask seeds = stroke_mask(pts, std::max(r / st, 0.75f), lw, lh);
    if (std::none_of(seeds.begin(), seeds.end(), [](std::uint8_t s) { return s != 0; })) return std::nullopt;
    QuickCut cut = quick_segment(img, seeds);
    if (std::none_of(cut.low.begin(), cut.low.end(), [](std::uint8_t v) { return v != 0; })) return std::nullopt;
    return finish_region(sampler, window, step, cut.low, lw, lh, &cut.fg, &cut.bg);
}

QuickCut quick_segment(const RgbImage& img, const Mask& seeds) {
    const std::size_t lw = img.w, lh = img.h;
    const std::vector<float> geo = geodesic(img, seeds);
    std::vector<Rgb> fs, bs;
    for (std::size_t i = 0; i < img.px.size(); ++i) {
        if (seeds[i] != 0) fs.push_back(img.px[i]);
        if (geo[i] > kEdge) bs.push_back(img.px[i]);
    }
    fs = subsample(fs, 20'000);
    bs = subsample(bs, 20'000);
    std::optional<Gmm> fitted = Gmm::fit(fs, std::clamp<std::size_t>(fs.size() / 40, 1, 4), kQReg);
    Gmm fg = fitted ? std::move(*fitted) : broad_model();
    std::optional<Gmm> bg;
    if (bs.size() >= 30) bg = Gmm::fit(bs, 6, kQReg);
    std::vector<float> cf(img.px.size()), cb(img.px.size());
    for (std::size_t i = 0; i < img.px.size(); ++i) {
        cf[i] = fg.neg_log(img.px[i]) + kKappa * std::min(geo[i] / kEdge, 4.0f);
        cb[i] = bg ? -static_cast<float>(std::log(0.85 * std::exp(bg->log_prob(img.px[i])) + 0.15)) : 0.0f;
    }
    std::vector<std::uint8_t> fixed(seeds.size());
    for (std::size_t i = 0; i < seeds.size(); ++i) fixed[i] = seeds[i] != 0 ? kHardFg : kFree;
    check_stop();
    const Mask cut = grid_cut(img, cf, cb, fixed, kGamma, contrast_beta(img));
    Mask low = keep_seeded(cut, seeds, lw, lh);
    return QuickCut{std::move(fg), bg ? std::move(*bg) : broad_model(), std::move(low)};
}

Gmm broad_model() {
    std::vector<Rgb> corners;
    for (int i = 0; i < 8; ++i) corners.push_back(Rgb{static_cast<float>(i & 1), static_cast<float>((i >> 1) & 1), static_cast<float>((i >> 2) & 1)});
    // (eight fixed, distinct samples: the fit cannot fail)
    return *Gmm::fit(corners, 1, 0.0f);
}

Mask stroke_mask(const std::vector<std::pair<float, float>>& pts, float radius, std::size_t w, std::size_t h) {
    Mask m(w * h, 0);
    std::vector<std::pair<std::pair<float, float>, std::pair<float, float>>> segs;
    if (pts.size() == 1) {
        segs.emplace_back(pts[0], pts[0]);
    } else {
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) segs.emplace_back(pts[i], pts[i + 1]);
    }
    for (const auto& [a, b] : segs) {
        const auto x0 = static_cast<std::size_t>(std::max(std::floor(std::min(a.first, b.first) - radius), 0.0f));
        const auto y0 = static_cast<std::size_t>(std::max(std::floor(std::min(a.second, b.second) - radius), 0.0f));
        const auto x1 = std::min(static_cast<std::size_t>(std::max(std::ceil(std::max(a.first, b.first) + radius), 0.0f)), w);
        const auto y1 = std::min(static_cast<std::size_t>(std::max(std::ceil(std::max(a.second, b.second) + radius), 0.0f)), h);
        const float dx = b.first - a.first, dy = b.second - a.second;
        const float len2 = dx * dx + dy * dy;
        for (std::size_t y = y0; y < y1; ++y) {
            for (std::size_t x = x0; x < x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float t = len2 > 0.0f ? std::clamp(((px - a.first) * dx + (py - a.second) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                const float qx = a.first + t * dx - px, qy = a.second + t * dy - py;
                if (qx * qx + qy * qy <= radius * radius) m[y * w + x] = 1;
            }
        }
    }
    for (const auto& [x, y] : pts) {
        if (x >= 0.0f && y >= 0.0f && static_cast<std::size_t>(x) < w && static_cast<std::size_t>(y) < h)
            m[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = 1;
    }
    return m;
}

std::vector<float> geodesic(const RgbImage& img, const Mask& seeds) {
    const std::size_t w = img.w, h = img.h;
    const RgbImage sm = blur3(blur3(img));
    // the texture of what was painted: the median gradient over the stroke grown by two pixels (gradual changes and
    // texture at that level cost nothing)
    Mask near(w * h, 0);
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        if (seeds[i] == 0) continue;
        const std::size_t x = i % w, y = i / w;
        for (std::size_t yy = y > 2 ? y - 2 : 0; yy < std::min(y + 3, h); ++yy)
            std::fill(near.begin() + static_cast<std::ptrdiff_t>(yy * w + (x > 2 ? x - 2 : 0)), near.begin() + static_cast<std::ptrdiff_t>(yy * w + std::min(x + 3, w)),
                      std::uint8_t{1});
    }
    // the colour gradient (central differences) of the smoothed picture
    std::vector<float> g(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const Rgb &l = sm.at(x > 0 ? x - 1 : 0, y), &r = sm.at(std::min(x + 1, w - 1), y);
            const Rgb &u = sm.at(x, y > 0 ? y - 1 : 0), &dn = sm.at(x, std::min(y + 1, h - 1));
            g[y * w + x] = std::sqrt((d2(l, r) + d2(u, dn)) * 0.25f);
        }
    }
    std::vector<float> diffs;
    for (std::size_t i = 0; i < g.size(); ++i)
        if (near[i] != 0) diffs.push_back(g[i]);
    float tau = 0.0f;
    if (!diffs.empty()) {
        const std::size_t mid = diffs.size() / 2;
        std::nth_element(diffs.begin(), diffs.begin() + static_cast<std::ptrdiff_t>(mid), diffs.end());
        tau = diffs[mid];
    }
    const float thr = std::max(2.0f * tau, kMinStep);
    // (crossing a pixel whose gradient passes the texture costs the excess: an edge costs about its contrast whichever
    // way a path crosses it)
    std::vector<float> e(w * h);
    for (std::size_t i = 0; i < e.size(); ++i) e[i] = std::max(g[i] - thr, 0.0f);
    const auto cost = [&](std::size_t a, std::size_t b, float len) { return len * (0.5f * (e[a] + e[b]) + 1e-6f); };
    std::vector<float> d(w * h);
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = seeds[i] != 0 ? 0.0f : std::numeric_limits<float>::infinity();
    const float sq = std::numbers::sqrt2_v<float>;
    for (int pass = 0; pass < 2; ++pass) {
        check_stop();
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = y * w + x;
                float v = d[i];
                if (x > 0) v = std::min(v, d[i - 1] + cost(i, i - 1, 1.0f));
                if (y > 0) {
                    v = std::min(v, d[i - w] + cost(i, i - w, 1.0f));
                    if (x > 0) v = std::min(v, d[i - w - 1] + cost(i, i - w - 1, sq));
                    if (x + 1 < w) v = std::min(v, d[i - w + 1] + cost(i, i - w + 1, sq));
                }
                d[i] = v;
            }
        }
        for (std::size_t y = h; y-- > 0;) {
            for (std::size_t x = w; x-- > 0;) {
                const std::size_t i = y * w + x;
                float v = d[i];
                if (x + 1 < w) v = std::min(v, d[i + 1] + cost(i, i + 1, 1.0f));
                if (y + 1 < h) {
                    v = std::min(v, d[i + w] + cost(i, i + w, 1.0f));
                    if (x + 1 < w) v = std::min(v, d[i + w + 1] + cost(i, i + w + 1, sq));
                    if (x > 0) v = std::min(v, d[i + w - 1] + cost(i, i + w - 1, sq));
                }
                d[i] = v;
            }
        }
    }
    return d;
}

}  // namespace genko::render::segment
