// PC3 B3: Focus Area (heuristic) — PhotoCraft v0.2.0's algo/src/segment/focus.rs (MIT,
// native/third_party/photocraft/NOTICE.md). Sharpness is the local Laplacian energy (the Laplacian focus measure, Nayar &
// Nakagawa, "Shape from Focus", PAMI 1994) on a working copy, normalised by its 99th percentile; `noise` takes a noise
// floor off; `range` (Photoshop's In-Focus Range, 0..1) lowers the threshold as it grows. The thresholded map is smoothed
// by a guided filter (the picture its guide) so that it follows the edges, cleaned, and upsampled with a soft edge.

#include <algorithm>
#include <cmath>

#include "render/matting.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::segment {

std::vector<float> sharpness(const RgbImage& img, float noise) {
    const std::size_t w = img.w, h = img.h;
    std::vector<float> y(w * h);
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = 0.299f * img.px[i][0] + 0.587f * img.px[i][1] + 0.114f * img.px[i][2];
    const auto at = [&](std::size_t x, std::size_t yy) { return y[yy * w + x]; };
    std::vector<float> lap(w * h, 0.0f);
    for (std::size_t yy = 0; yy < h; ++yy) {
        for (std::size_t x = 0; x < w; ++x) {
            const float c = at(x, yy);
            lap[yy * w + x] = std::abs(2.0f * c - at(x > 0 ? x - 1 : 0, yy) - at(std::min(x + 1, w - 1), yy)) +
                              std::abs(2.0f * c - at(x, yy > 0 ? yy - 1 : 0) - at(x, std::min(yy + 1, h - 1)));
        }
    }
    const std::size_t r = std::max<std::size_t>(std::min(w, h) / 60, 3);
    const std::vector<float> e = matting::box_mean(lap, w, h, r);
    std::vector<float> sorted = e;
    const std::size_t k = std::min(static_cast<std::size_t>(static_cast<float>(sorted.size()) * 0.99f), sorted.size() > 0 ? sorted.size() - 1 : 0);
    float p99 = 0.0f;
    if (!sorted.empty()) {
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
        p99 = sorted[k];
    }
    const float floor = std::clamp(noise, 0.0f, 1.0f) * 0.5f;
    std::vector<float> out(e.size());
    for (std::size_t i = 0; i < e.size(); ++i) out[i] = std::max(std::min(e[i] / std::max(p99, 1e-6f), 1.0f) - floor, 0.0f) / std::max(1.0f - floor, 1e-3f);
    return out;
}

std::optional<Region> focus_area(const Sampler& sampler, const Rect& canvas, float range, float noise) {
    if (canvas.width() < 4 || canvas.height() < 4) return std::nullopt;
    const std::size_t step = scale_for(canvas, kFocusWorkPx);
    const RgbImage img = sampler.rgb_scaled(canvas, step);
    const std::size_t w = img.w, h = img.h;
    const std::vector<float> s = sharpness(img, noise);
    const float one_less = 1.0f - std::clamp(range, 0.0f, 1.0f);
    const float t = 0.05f + 0.9f * one_less * one_less;
    std::vector<float> bin(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) bin[i] = s[i] >= t ? 1.0f : 0.0f;
    const std::size_t r = std::max<std::size_t>(std::min(w, h) / 50, 2);
    check_stop();
    const std::vector<float> smooth = matting::guided_filter_color(img, bin, r, 1e-2f);
    Mask mask(smooth.size());
    for (std::size_t i = 0; i < smooth.size(); ++i) mask[i] = smooth[i] >= 0.5f ? 1 : 0;
    mask = clean_mask(mask, w, h, 0.05f, 0.02f);
    if (std::none_of(mask.begin(), mask.end(), [](std::uint8_t v) { return v != 0; })) return std::nullopt;
    // the soft upsampling: a bilinear field, steepened to about a working pixel of change
    std::vector<float> field(mask.size());
    for (std::size_t i = 0; i < mask.size(); ++i) field[i] = mask[i] != 0 ? std::clamp(smooth[i], 0.5f, 1.0f) : std::clamp(smooth[i], 0.0f, 0.5f);
    const auto fw = static_cast<std::size_t>(canvas.width()), fh = static_cast<std::size_t>(canvas.height());
    const auto st = static_cast<float>(step);
    std::vector<std::uint8_t> out(fw * fh, 0);
    for (std::size_t yy = 0; yy < fh; ++yy) {
        if ((yy & 255) == 0) check_stop();
        const float fy = std::clamp((static_cast<float>(yy) + 0.5f) / st - 0.5f, 0.0f, static_cast<float>(h - 1));
        const auto y0 = static_cast<std::size_t>(std::floor(fy));
        const float ty = fy - std::floor(fy);
        const std::size_t y1 = std::min(y0 + 1, h - 1);
        for (std::size_t xx = 0; xx < fw; ++xx) {
            const float fx = std::clamp((static_cast<float>(xx) + 0.5f) / st - 0.5f, 0.0f, static_cast<float>(w - 1));
            const auto x0 = static_cast<std::size_t>(std::floor(fx));
            const float tx = fx - std::floor(fx);
            const std::size_t x1 = std::min(x0 + 1, w - 1);
            const float top = field[y0 * w + x0] * (1.0f - tx) + field[y0 * w + x1] * tx;
            const float bot = field[y1 * w + x0] * (1.0f - tx) + field[y1 * w + x1] * tx;
            const float v = top * (1.0f - ty) + bot * ty;
            out[yy * fw + xx] = static_cast<std::uint8_t>(std::round(std::clamp((v - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f) * 255.0f));
        }
    }
    return trim_region(Region{canvas, std::move(out)});
}

}  // namespace genko::render::segment
