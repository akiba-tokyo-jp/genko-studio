// PC3 B3: Select Subject (classical, heuristic) — PhotoCraft v0.2.0's algo/src/segment/subject.rs (MIT,
// native/third_party/photocraft/NOTICE.md). Salient-object detection seeding a GrabCut: SLIC superpixels on a working
// copy; each superpixel's saliency from "Saliency Optimization from Robust Background Detection" (W. Zhu, S. Liang, Y. Wei
// and J. Sun, CVPR 2014: boundary connectivity over geodesic colour distances, background-weighted contrast, a
// least-squares optimisation with smoothness between neighbours), blended with frequency-tuned saliency (R. Achanta et
// al., CVPR 2009); Otsu's threshold seeds the trimap; the result cleaned and cut again at full resolution. Good for a
// distinct object on a plainer background; not a semantic segmenter.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

constexpr float kSigmaClr = 10.0f;
constexpr float kSigmaSpa = 0.25f;

RgbImage blur_lab(const RgbImage& img) {
    const std::size_t w = img.w, h = img.h;
    RgbImage cur = img;
    for (int pass = 0; pass < 2; ++pass) {
        RgbImage tmp = cur;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const Rgb &l = cur.at(x > 0 ? x - 1 : 0, y), &c = cur.at(x, y), &r = cur.at(std::min(x + 1, w - 1), y);
                for (std::size_t k = 0; k < 3; ++k) tmp.px[y * w + x][k] = (l[k] + 2.0f * c[k] + r[k]) / 4.0f;
            }
        }
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const Rgb &u = tmp.at(x, y > 0 ? y - 1 : 0), &c = tmp.at(x, y), &d = tmp.at(x, std::min(y + 1, h - 1));
                for (std::size_t k = 0; k < 3; ++k) cur.px[y * w + x][k] = (u[k] + 2.0f * c[k] + d[k]) / 4.0f;
            }
        }
    }
    return cur;
}

template <typename Weight>
void dijkstra(std::size_t src, const std::vector<std::vector<std::size_t>>& adj, const Weight& wt, std::vector<float>& dist) {
    std::fill(dist.begin(), dist.end(), std::numeric_limits<float>::infinity());
    dist[src] = 0.0f;
    // (a min-heap by distance; equal distances by the larger index first, as Rust's BinaryHeap of (d, u) reversed on d)
    using Item = std::pair<float, std::size_t>;
    const auto later = [](const Item& a, const Item& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    };
    std::priority_queue<Item, std::vector<Item>, decltype(later)> heap(later);
    heap.emplace(0.0f, src);
    while (!heap.empty()) {
        const auto [d, u] = heap.top();
        heap.pop();
        if (d > dist[u]) continue;
        for (const std::size_t v : adj[u]) {
            const float nd = d + wt(u, v);
            if (nd < dist[v]) {
                dist[v] = nd;
                heap.emplace(nd, v);
            }
        }
    }
}

}  // namespace

std::optional<Region> select_subject(const Sampler& sampler, const Rect& canvas) {
    if (canvas.width() < 4 || canvas.height() < 4) return std::nullopt;
    const std::size_t step = scale_for(canvas, kSubjectWorkPx);
    const RgbImage img = sampler.rgb_scaled(canvas, step);
    const std::size_t w = img.w, h = img.h;
    const std::vector<float> sal = saliency(img);
    const float t = std::clamp(otsu(sal), 0.15f, 0.85f);
    std::vector<std::uint8_t> trimap(w * h, kPrBg);
    bool any_fg = false;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            const bool border = x == 0 || y == 0 || x + 1 == w || y + 1 == h;
            if (sal[i] >= t) {
                any_fg = true;
                trimap[i] = kPrFg;
            } else if (border || sal[i] < 0.4f * t) {
                trimap[i] = kBg;
            } else {
                trimap[i] = kPrBg;
            }
        }
    }
    if (!any_fg) return std::nullopt;
    const auto models = grabcut(img, trimap, 6);
    if (!models) return std::nullopt;
    Mask low(trimap.size());
    for (std::size_t i = 0; i < trimap.size(); ++i) low[i] = trimap[i] == kPrFg || trimap[i] == kFg ? 1 : 0;
    low = clean_mask(low, w, h, 0.2f, 0.05f);
    if (std::none_of(low.begin(), low.end(), [](std::uint8_t v) { return v != 0; })) return std::nullopt;
    return finish_region(sampler, canvas, step, low, w, h, &models->first, &models->second);
}

std::vector<float> saliency(const RgbImage& img) {
    const std::size_t w = img.w, h = img.h;
    std::vector<Rgb> lab(img.px.size());
    for (std::size_t i = 0; i < lab.size(); ++i) lab[i] = rgb_to_lab(img.px[i]);
    const std::size_t target = std::clamp<std::size_t>((w * h) / 150, 16, 400);
    const Superpixels sp = slic_lab(lab, w, h, target, 10.0f, 8);
    const std::size_t n = sp.count;
    // the regions' statistics
    std::vector<std::array<double, 3>> mean_sum(n, std::array<double, 3>{0, 0, 0});
    std::vector<std::array<double, 2>> pos_sum(n, std::array<double, 2>{0, 0});
    std::vector<double> cnt(n, 0.0);
    std::vector<std::uint8_t> on_border(n, 0);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t l = sp.labels[y * w + x];
            const Rgb& c = lab[y * w + x];
            for (std::size_t k = 0; k < 3; ++k) mean_sum[l][k] += c[k];
            pos_sum[l][0] += static_cast<double>(x) / static_cast<double>(w);
            pos_sum[l][1] += static_cast<double>(y) / static_cast<double>(h);
            cnt[l] += 1.0;
            if (x == 0 || y == 0 || x + 1 == w || y + 1 == h) on_border[l] = 1;
        }
    }
    std::vector<Rgb> mean(n);
    std::vector<std::array<float, 2>> pos(n);
    for (std::size_t l = 0; l < n; ++l) {
        const double c = std::max(cnt[l], 1.0);
        for (std::size_t k = 0; k < 3; ++k) mean[l][k] = static_cast<float>(mean_sum[l][k] / c);
        for (std::size_t k = 0; k < 2; ++k) pos[l][k] = static_cast<float>(pos_sum[l][k] / c);
    }
    const auto dapp = [&](std::size_t a, std::size_t b) { return std::sqrt(d2(mean[a], mean[b])); };
    // the adjacency
    std::vector<std::vector<std::size_t>> adj(n);
    const auto link = [&](std::size_t a, std::size_t b) {
        if (a != b && std::find(adj[a].begin(), adj[a].end(), b) == adj[a].end()) {
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    };
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t l = sp.labels[y * w + x];
            if (x + 1 < w) link(l, sp.labels[y * w + x + 1]);
            if (y + 1 < h) link(l, sp.labels[(y + 1) * w + x]);
        }
    }
    // boundary connectivity from all-pairs geodesic distances (Dijkstra from each region)
    std::vector<float> wbg(n, 0.0f), geo(n, std::numeric_limits<float>::infinity());
    for (std::size_t p = 0; p < n; ++p) {
        if ((p & 31) == 0) check_stop();
        dijkstra(p, adj, dapp, geo);
        float area = 0, len = 0;
        for (std::size_t q = 0; q < n; ++q) {
            const float s = std::exp(-geo[q] * geo[q] / (2.0f * kSigmaClr * kSigmaClr));
            area += s;
            if (on_border[q] != 0) len += s;
        }
        const float bnd = len / std::sqrt(std::max(area, 1e-6f));
        wbg[p] = 1.0f - std::exp(-bnd * bnd / 2.0f);
    }
    // background-weighted contrast
    std::vector<float> wctr(n, 0.0f);
    for (std::size_t p = 0; p < n; ++p) {
        float s = 0;
        for (std::size_t q = 0; q < n; ++q) {
            const float dx = pos[p][0] - pos[q][0], dy = pos[p][1] - pos[q][1];
            const float wspa = std::exp(-(dx * dx + dy * dy) / (2.0f * kSigmaSpa * kSigmaSpa));
            s += dapp(p, q) * wspa * wbg[q];
        }
        wctr[p] = s;
    }
    float mx = 0.0f;
    for (const float v : wctr) mx = std::max(mx, v);
    mx = std::max(mx, 1e-6f);
    std::vector<float> wfg(n);
    for (std::size_t p = 0; p < n; ++p) wfg[p] = wctr[p] / mx;
    // the optimisation: min Σ wbg s² + Σ wfg (s − 1)² + Σ_{i~j} w_ij (s_i − s_j)² (Gauss–Seidel)
    std::vector<std::vector<float>> wij(n);
    for (std::size_t p = 0; p < n; ++p) {
        for (const std::size_t q : adj[p]) {
            const float da = dapp(p, q);
            wij[p].push_back(std::exp(-(da * da) / (2.0f * kSigmaClr * kSigmaClr)) + 0.1f);
        }
    }
    std::vector<float> s = wfg;
    for (int it = 0; it < 200; ++it) {
        for (std::size_t p = 0; p < n; ++p) {
            float num = wfg[p], den = wbg[p] + wfg[p];
            for (std::size_t k = 0; k < adj[p].size(); ++k) {
                num += wij[p][k] * s[adj[p][k]];
                den += wij[p][k];
            }
            s[p] = num / std::max(den, 1e-6f);
        }
    }
    // frequency-tuned saliency
    std::array<double, 3> mean_all_sum{0, 0, 0};
    for (const Rgb& c : lab)
        for (std::size_t k = 0; k < 3; ++k) mean_all_sum[k] += c[k];
    const Rgb mean_all{static_cast<float>(mean_all_sum[0] / static_cast<double>(lab.size())), static_cast<float>(mean_all_sum[1] / static_cast<double>(lab.size())),
                       static_cast<float>(mean_all_sum[2] / static_cast<double>(lab.size()))};
    RgbImage as_image;
    as_image.w = w;
    as_image.h = h;
    as_image.px = lab;
    const RgbImage blurred = blur_lab(as_image);
    std::vector<float> ft(blurred.px.size());
    float ftmax = 0.0f;
    for (std::size_t i = 0; i < ft.size(); ++i) {
        ft[i] = std::sqrt(d2(blurred.px[i], mean_all));
        ftmax = std::max(ftmax, ft[i]);
    }
    ftmax = std::max(ftmax, 1e-6f);
    float smin = std::numeric_limits<float>::max(), smax = std::numeric_limits<float>::lowest();
    for (const float v : s) {
        smin = std::min(smin, v);
        smax = std::max(smax, v);
    }
    const float srange = std::max(smax - smin, 1e-6f);
    std::vector<float> out(sp.labels.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = 0.75f * (s[sp.labels[i]] - smin) / srange + 0.25f * ft[i] / ftmax;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (const float v : out) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float range = std::max(hi - lo, 1e-6f);
    for (float& v : out) v = (v - lo) / range;
    return out;
}

float otsu(const std::vector<float>& v) {
    std::array<std::uint64_t, 256> hist{};
    for (const float x : v) ++hist[static_cast<std::size_t>(std::round(std::clamp(x, 0.0f, 1.0f) * 255.0f))];
    const auto total = static_cast<double>(v.size());
    double sum = 0;
    for (std::size_t i = 0; i < 256; ++i) sum += static_cast<double>(i) * static_cast<double>(hist[i]);
    double wb = 0, sb = 0, best = -1.0;
    std::size_t bt = 0;
    for (std::size_t t = 0; t < 256; ++t) {
        wb += static_cast<double>(hist[t]);
        if (wb == 0.0) continue;
        const double wf = total - wb;
        if (wf == 0.0) break;
        sb += static_cast<double>(t) * static_cast<double>(hist[t]);
        const double mb = sb / wb, mf = (sum - sb) / wf;
        const double between = wb * wf * (mb - mf) * (mb - mf);
        if (between > best) {
            best = between;
            bt = t;
        }
    }
    return (static_cast<float>(bt) + 0.5f) / 255.0f;
}

}  // namespace genko::render::segment
