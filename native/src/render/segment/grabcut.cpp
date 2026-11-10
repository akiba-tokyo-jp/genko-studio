// PC3 B3: GrabCut and Object Selection (a rectangle) — PhotoCraft v0.2.0's algo/src/segment/grabcut.rs (MIT,
// native/third_party/photocraft/NOTICE.md), from C. Rother, V. Kolmogorov and A. Blake, "GrabCut: Interactive
// Foreground Extraction using Iterated Graph Cuts", SIGGRAPH 2004: a trimap, 5-component full-covariance GMMs a side,
// and "assign components → learn the GMMs → min cut" steps with the contrast-sensitive smoothness γ e^{−β‖z_m − z_n‖²} /
// dist(m, n) (γ = 50).

#include <algorithm>
#include <cmath>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

constexpr float kGamma = 50.0f;
constexpr std::size_t kK = 5;
constexpr std::size_t kMaxSamples = 40'000;

bool is_fg(std::uint8_t t) { return t == kFg || t == kPrFg; }

}  // namespace

std::optional<std::pair<Gmm, Gmm>> grabcut(const RgbImage& img, std::vector<std::uint8_t>& trimap, std::size_t iters) {
    const float beta = contrast_beta(img);
    std::vector<std::uint8_t> fixed(trimap.size());
    for (std::size_t i = 0; i < trimap.size(); ++i) fixed[i] = trimap[i] == kFg ? kHardFg : trimap[i] == kBg ? kHardBg : kFree;
    std::optional<std::pair<Gmm, Gmm>> models;
    for (std::size_t it = 0; it < std::max<std::size_t>(iters, 1); ++it) {
        check_stop();
        std::vector<Rgb> fs, bs;
        for (std::size_t i = 0; i < img.px.size(); ++i) (is_fg(trimap[i]) ? fs : bs).push_back(img.px[i]);
        fs = subsample(fs, kMaxSamples);
        bs = subsample(bs, kMaxSamples);
        std::optional<Gmm> fg, bg;
        if (!models) {
            fg = Gmm::fit(fs, kK, kDefaultReg);
            if (!fg) return std::nullopt;
            bg = Gmm::fit(bs, kK, kDefaultReg);
            if (!bg) return std::nullopt;
        } else {
            fg = models->first.refit(fs);
            if (!fg) fg = models->first;
            bg = models->second.refit(bs);
            if (!bg) bg = models->second;
        }
        const auto [cf, cb] = data_costs(img, *fg, *bg);
        const Mask cut = grid_cut(img, cf, cb, fixed, kGamma, beta);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < trimap.size(); ++i) {
            std::uint8_t& t = trimap[i];
            if (t == kPrBg || t == kPrFg) {
                const std::uint8_t nt = cut[i] != 0 ? kPrFg : kPrBg;
                changed += nt != t ? 1 : 0;
                t = nt;
            }
        }
        models = std::make_pair(std::move(*fg), std::move(*bg));
        if (changed * 2000 < trimap.size()) break;
    }
    return models;
}

std::optional<Region> object_select(const Sampler& sampler, const Rect& canvas, const Rect& given, std::size_t work_px) {
    const Rect rect{std::max(given.x0, canvas.x0), std::max(given.y0, canvas.y0), std::min(given.x1, canvas.x1), std::min(given.y1, canvas.y1)};
    if (rect.width() < 2 || rect.height() < 2) return std::nullopt;
    const int margin = std::max(static_cast<int>(std::ceil(static_cast<float>(std::max(rect.width(), rect.height())) * 0.15f)), 8);
    const Rect window{std::max(rect.x0 - margin, canvas.x0), std::max(rect.y0 - margin, canvas.y0), std::min(rect.x1 + margin, canvas.x1),
                      std::min(rect.y1 + margin, canvas.y1)};
    const std::size_t step = scale_for(window, work_px);
    const RgbImage img = sampler.rgb_scaled(window, step);
    const std::size_t lw = img.w, lh = img.h;
    std::vector<std::uint8_t> trimap(lw * lh, kBg);
    // (a working pixel covers [x0 + x·step, …): its centre decides inside or out)
    bool any_bg = false;
    for (std::size_t y = 0; y < lh; ++y) {
        for (std::size_t x = 0; x < lw; ++x) {
            const int cx = window.x0 + static_cast<int>(x * step + step / 2), cy = window.y0 + static_cast<int>(y * step + step / 2);
            if (rect.contains(cx, cy)) {
                trimap[y * lw + x] = kPrFg;
            } else {
                any_bg = true;
            }
        }
    }
    if (!any_bg) {
        // (the rectangle covers the whole canvas: a thin ring outside as the background)
        const std::size_t ring = std::max<std::size_t>(std::min(lw, lh) / 50, 1);
        for (std::size_t y = 0; y < lh; ++y)
            for (std::size_t x = 0; x < lw; ++x)
                if (x < ring || y < ring || x + ring >= lw || y + ring >= lh) trimap[y * lw + x] = kBg;
    }
    const auto models = grabcut(img, trimap, 8);
    if (!models) return std::nullopt;
    Mask low(trimap.size());
    for (std::size_t i = 0; i < trimap.size(); ++i) low[i] = is_fg(trimap[i]) ? 1 : 0;
    low = clean_mask(low, lw, lh, 0.1f, 0.002f);
    return finish_region(sampler, window, step, low, lw, lh, &models->first, &models->second);
}

}  // namespace genko::render::segment
