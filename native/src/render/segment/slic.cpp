// PC3 B3: SLIC superpixels — PhotoCraft v0.2.0's algo/src/segment/slic.rs (MIT, native/third_party/photocraft/NOTICE.md),
// from R. Achanta et al., "SLIC Superpixels Compared to State-of-the-Art Superpixel Methods", IEEE PAMI 34(11), 2012:
// k-means in CIELAB + xy within 2S × 2S of each centre, D = sqrt(d_lab² + (d_xy / S)² m²), the centres seeded on a grid
// and moved to the lowest gradient of their 3×3, then the connectivity enforced (small fragments join a neighbour).

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

// 4-connected components relabelled; one smaller than `min_size` joins the component found before it next to it.
std::pair<std::vector<std::uint32_t>, std::size_t> enforce_connectivity(const std::vector<std::uint32_t>& labels, std::size_t w, std::size_t h,
                                                                         std::size_t min_size) {
    const std::size_t n = w * h;
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> out(n, kUnset);
    std::uint32_t next = 0;
    std::vector<std::size_t> comp;
    for (std::size_t s = 0; s < n; ++s) {
        if (out[s] != kUnset) continue;
        const std::size_t sx = s % w, sy = s / w;
        std::optional<std::uint32_t> adj;
        if (sx > 0 && out[s - 1] != kUnset) adj = out[s - 1];
        if (sy > 0 && out[s - w] != kUnset) adj = out[s - w];
        comp.clear();
        comp.push_back(s);
        out[s] = next;
        for (std::size_t k = 0; k < comp.size(); ++k) {
            const std::size_t i = comp[k];
            const std::size_t x = i % w, y = i / w;
            const auto visit = [&](std::size_t j) {
                if (out[j] == kUnset && labels[j] == labels[s]) {
                    out[j] = next;
                    comp.push_back(j);
                }
            };
            if (x > 0) visit(i - 1);
            if (x + 1 < w) visit(i + 1);
            if (y > 0) visit(i - w);
            if (y + 1 < h) visit(i + w);
        }
        if (adj && comp.size() < min_size) {
            for (const std::size_t i : comp) out[i] = *adj;
        } else {
            ++next;
        }
    }
    return {std::move(out), next};
}

}  // namespace

Rgb rgb_to_lab(const Rgb& c) {
    const auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float r = lin(c[0]), g = lin(c[1]), b = lin(c[2]);
    const float x = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / 0.95047f;
    const float y = 0.2126729f * r + 0.7151522f * g + 0.072175f * b;
    const float z = (0.0193339f * r + 0.119192f * g + 0.9503041f * b) / 1.08883f;
    const auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; };
    const float fx = f(x), fy = f(y), fz = f(z);
    return Rgb{116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

Superpixels slic(const RgbImage& img, std::size_t target, float compactness, std::size_t iters) {
    std::vector<Rgb> lab(img.px.size());
    for (std::size_t i = 0; i < lab.size(); ++i) lab[i] = rgb_to_lab(img.px[i]);
    return slic_lab(lab, img.w, img.h, target, compactness, iters);
}

Superpixels slic_lab(const std::vector<Rgb>& lab, std::size_t w, std::size_t h, std::size_t target, float compactness, std::size_t iters) {
    const std::size_t n = w * h;
    if (n == 0) return Superpixels{w, h, {}, 0, 1.0f};
    const float s = std::max(1.0f, std::sqrt(static_cast<float>(n) / static_cast<float>(std::max<std::size_t>(target, 1))));
    const auto at = [&](std::size_t x, std::size_t y) -> const Rgb& { return lab[std::min(y, h - 1) * w + std::min(x, w - 1)]; };
    const auto grad = [&](std::size_t x, std::size_t y) {
        return d2(at(x > 0 ? x - 1 : 0, y), at(x + 1, y)) + d2(at(x, y > 0 ? y - 1 : 0), at(x, y + 1));
    };
    // the centres: [L, a, b, x, y]
    std::vector<std::array<float, 5>> centers;
    for (float y = s / 2.0f; y < static_cast<float>(h); y += s) {
        for (float x = s / 2.0f; x < static_cast<float>(w); x += s) {
            const std::size_t cx = static_cast<std::size_t>(x), cy = static_cast<std::size_t>(y);
            std::size_t bx = cx, by = cy;
            float bg = std::numeric_limits<float>::max();
            for (std::size_t yy = cy > 0 ? cy - 1 : 0; yy <= std::min(cy + 1, h - 1); ++yy) {
                for (std::size_t xx = cx > 0 ? cx - 1 : 0; xx <= std::min(cx + 1, w - 1); ++xx) {
                    const float g = grad(xx, yy);
                    if (g < bg) {
                        bx = xx;
                        by = yy;
                        bg = g;
                    }
                }
            }
            const Rgb& c = lab[by * w + bx];
            centers.push_back({c[0], c[1], c[2], static_cast<float>(bx), static_cast<float>(by)});
        }
    }
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> labels(n, kUnset);
    std::vector<float> dist(n, std::numeric_limits<float>::max());
    const float wxy = (compactness / s) * (compactness / s);
    const auto r = static_cast<std::int64_t>(std::ceil(s));
    const auto wi = static_cast<std::int64_t>(w), hi = static_cast<std::int64_t>(h);
    for (std::size_t it = 0; it < std::max<std::size_t>(iters, 1); ++it) {
        check_stop();
        std::fill(dist.begin(), dist.end(), std::numeric_limits<float>::max());
        for (std::size_t k = 0; k < centers.size(); ++k) {
            const auto& c = centers[k];
            const auto cx = static_cast<std::int64_t>(std::round(c[3])), cy = static_cast<std::int64_t>(std::round(c[4]));
            for (std::int64_t yy = std::max<std::int64_t>(cy - r, 0); yy < std::min(cy + r + 1, hi); ++yy) {
                for (std::int64_t xx = std::max<std::int64_t>(cx - r, 0); xx < std::min(cx + r + 1, wi); ++xx) {
                    const std::size_t i = static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx);
                    const float dc = d2(lab[i], Rgb{c[0], c[1], c[2]});
                    const float dx = static_cast<float>(xx) - c[3], dy = static_cast<float>(yy) - c[4];
                    const float d = dc + (dx * dx + dy * dy) * wxy;
                    if (d < dist[i]) {
                        dist[i] = d;
                        labels[i] = static_cast<std::uint32_t>(k);
                    }
                }
            }
        }
        std::vector<std::array<double, 6>> sums(centers.size(), std::array<double, 6>{0, 0, 0, 0, 0, 0});
        for (std::size_t i = 0; i < n; ++i) {
            if (labels[i] == kUnset) continue;
            const Rgb& p = lab[i];
            auto& sm = sums[labels[i]];
            sm[0] += p[0];
            sm[1] += p[1];
            sm[2] += p[2];
            sm[3] += static_cast<double>(i % w);
            sm[4] += static_cast<double>(i / w);
            sm[5] += 1.0;
        }
        for (std::size_t k = 0; k < centers.size(); ++k) {
            const auto& sm = sums[k];
            if (sm[5] > 0.0)
                for (std::size_t j = 0; j < 5; ++j) centers[k][j] = static_cast<float>(sm[j] / sm[5]);
        }
    }
    // (pixels no centre reached, at the far edges: the label of the one before)
    for (std::size_t i = 0; i < n; ++i)
        if (labels[i] == kUnset) labels[i] = i > 0 ? labels[i - 1] : 0;
    auto [relabelled, count] = enforce_connectivity(labels, w, h, static_cast<std::size_t>((s * s) / 4.0f));
    return Superpixels{w, h, std::move(relabelled), count, s};
}

}  // namespace genko::render::segment
