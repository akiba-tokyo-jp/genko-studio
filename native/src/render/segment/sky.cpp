// PC3 B3: Select › Sky (classical, no learned model) — PhotoCraft v0.2.0's algo/src/segment/sky.rs (MIT,
// native/third_party/photocraft/NOTICE.md). At a working resolution each pixel gets a sky likelihood (a colour prior —
// blue and bright, bright and unsaturated, a warm glow — × smoothness); seeds in the top rows grow through neighbours that
// stay likely and change gradually; sky seen through gaps joins by a colour model of the grown sky; the mask is
// upsampled and refined at full resolution by a guided filter on the luminance (He, Sun & Tang, ECCV 2010), tile by
// tile and only where it is mixed.

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/matting.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luma(const Rgb& p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

}  // namespace

float colour_prior(const Rgb& p) {
    const float r = p[0], g = p[1], b = p[2];
    const float y = luma(p);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.0f;
    // blue sky: blue clearly over red and not far under green (not foliage)
    const float blue = smoothstep(0.02f, 0.12f, b - r) * smoothstep(-0.12f, 0.0f, b - g) * smoothstep(0.18f, 0.4f, y);
    // overcast, clouds, haze: bright and nearly neutral
    const float white = smoothstep(0.5f, 0.75f, y) * (1.0f - smoothstep(0.12f, 0.3f, sat));
    // a sunset's glow: bright and warm, weaker (warm walls are common too)
    const float warm = 0.6f * smoothstep(0.55f, 0.8f, y) * smoothstep(0.0f, 0.1f, r - b) * (1.0f - smoothstep(0.05f, 0.25f, g - b - 0.2f));
    return std::max({blue, white, warm});
}

std::vector<float> sky_likelihood(const RgbImage& img) {
    const std::size_t w = img.w, h = img.h;
    std::vector<float> y(w * h);
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = luma(img.px[i]);
    const auto at = [&](std::int64_t x, std::int64_t yy) {
        return y[static_cast<std::size_t>(std::clamp<std::int64_t>(yy, 0, static_cast<std::int64_t>(h) - 1)) * w +
                 static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, static_cast<std::int64_t>(w) - 1))];
    };
    std::vector<float> grad(w * h, 0.0f);
    for (std::int64_t yy = 0; yy < static_cast<std::int64_t>(h); ++yy) {
        for (std::int64_t x = 0; x < static_cast<std::int64_t>(w); ++x) {
            const float gx = (at(x + 1, yy - 1) + 2.0f * at(x + 1, yy) + at(x + 1, yy + 1)) - (at(x - 1, yy - 1) + 2.0f * at(x - 1, yy) + at(x - 1, yy + 1));
            const float gy = (at(x - 1, yy + 1) + 2.0f * at(x, yy + 1) + at(x + 1, yy + 1)) - (at(x - 1, yy - 1) + 2.0f * at(x, yy - 1) + at(x + 1, yy - 1));
            grad[static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(x)] = std::sqrt(gx * gx + gy * gy) / 8.0f;
        }
    }
    const std::vector<float> tex = matting::box_mean(grad, w, h, 2);
    std::vector<float> out(w * h);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = colour_prior(img.px[i]) * (1.0f - smoothstep(0.015f, 0.07f, tex[i]));
    return out;
}

std::optional<Mask> sky_mask_low(const RgbImage& img, float threshold) {
    const std::size_t w = img.w, h = img.h;
    if (w < 2 || h < 2) return std::nullopt;
    const std::vector<float> like = sky_likelihood(img);
    const float t = 0.12f + 0.6f * (std::clamp(threshold, 0.0f, 100.0f) / 100.0f);
    // the seeds: likely sky in the top rows
    const std::size_t top = std::max<std::size_t>(h / 40, 1);
    Mask mask(w * h, 0);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < top * w; ++i) {
        if (like[i] > t) {
            mask[i] = 1;
            stack.push_back(i);
        }
    }
    if (stack.size() * 20 < top * w) return std::nullopt;  // (under 5 % of the top edge looks like sky)
    // grown where it changes gradually
    constexpr float kMaxStep = 0.12f;
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const std::size_t x = i % w, y = i / w;
        const auto visit = [&](std::size_t j) {
            if (mask[j] != 0 || like[j] <= t) return;
            const Rgb &a = img.px[i], &b = img.px[j];
            const float d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            if (d < kMaxStep) {
                mask[j] = 1;
                stack.push_back(j);
            }
        };
        if (x > 0) visit(i - 1);
        if (x + 1 < w) visit(i + 1);
        if (y > 0) visit(i - w);
        if (y + 1 < h) visit(i + w);
    }
    // sky seen through gaps: likely pixels that fit the grown sky's colours
    std::vector<Rgb> samples;
    for (std::size_t i = 0; i < mask.size(); ++i)
        if (mask[i] != 0) samples.push_back(img.px[i]);
    if (samples.size() >= 16) {
        if (const auto g = Gmm::fit(subsample(samples, 4000), 3, 1e-4f)) {
            const std::vector<Rgb> few = subsample(samples, 2000);
            std::vector<float> nl(few.size());
            for (std::size_t i = 0; i < few.size(); ++i) nl[i] = g->neg_log(few[i]);
            std::sort(nl.begin(), nl.end());
            const float cut = nl[std::min(nl.size() * 95 / 100, nl.size() - 1)];
            for (std::size_t i = 0; i < mask.size(); ++i)
                if (mask[i] == 0 && like[i] > t + 0.1f && g->neg_log(img.px[i]) <= cut) mask[i] = 1;
        }
    }
    Mask cleaned = clean_mask(mask, w, h, 0.002f, 0.01f);
    if (std::none_of(cleaned.begin(), cleaned.end(), [](std::uint8_t v) { return v != 0; })) return std::nullopt;
    return cleaned;
}

std::optional<Region> select_sky(const Sampler& sampler, const Rect& canvas, const SkyParams& p) {
    if (canvas.width() < 4 || canvas.height() < 4) return std::nullopt;
    const std::size_t step = scale_for(canvas, kSkyWorkPx);
    const RgbImage img = sampler.rgb_scaled(canvas, step);
    const std::size_t lw = img.w, lh = img.h;
    const auto low = sky_mask_low(img, p.threshold);
    if (!low) return std::nullopt;
    std::vector<float> lowf(low->size());
    for (std::size_t i = 0; i < lowf.size(); ++i) lowf[i] = (*low)[i] != 0 ? 1.0f : 0.0f;
    const float soft = std::clamp(p.softness, 0.0f, 100.0f) / 100.0f;
    const auto r = std::max(static_cast<std::size_t>(std::ceil(static_cast<float>(step) * (1.0f + 2.0f * soft))), std::size_t{3});
    const float eps = 1e-4f + 4e-3f * soft;
    // (the contrast after the filter: softness 0 a narrow ramp, 1 the filter's output as it is)
    const float k = 0.08f + 0.42f * soft;
    const auto w = static_cast<std::size_t>(canvas.width());
    const auto st = static_cast<float>(step);
    const auto coarse = [&](int x, int y) {
        // (the working mask upsampled bilinear, at the pixels' centres)
        const float fx = (static_cast<float>(x - canvas.x0) + 0.5f) / st - 0.5f;
        const float fy = (static_cast<float>(y - canvas.y0) + 0.5f) / st - 0.5f;
        const float x0 = std::floor(fx), y0 = std::floor(fy);
        const float ax = fx - x0, ay = fy - y0;
        const auto g = [&](float xx, float yy) {
            return lowf[static_cast<std::size_t>(std::clamp(yy, 0.0f, static_cast<float>(lh) - 1.0f)) * lw +
                        static_cast<std::size_t>(std::clamp(xx, 0.0f, static_cast<float>(lw) - 1.0f))];
        };
        const float top = g(x0, y0) * (1.0f - ax) + g(x0 + 1.0f, y0) * ax;
        const float bot = g(x0, y0 + 1.0f) * (1.0f - ax) + g(x0 + 1.0f, y0 + 1.0f) * ax;
        return top * (1.0f - ay) + bot * ay;
    };
    constexpr int kT = 256;
    const int ri = static_cast<int>(r);
    std::vector<std::uint8_t> mask(w * static_cast<std::size_t>(canvas.height()), 0);
    for (int ty = canvas.y0; ty < canvas.y1; ty += kT) {
        for (int tx = canvas.x0; tx < canvas.x1; tx += kT) {
            check_stop();
            const Rect t{tx, ty, std::min(tx + kT, canvas.x1), std::min(ty + kT, canvas.y1)};
            const Rect win{std::max(t.x0 - ri, canvas.x0), std::max(t.y0 - ri, canvas.y0), std::min(t.x1 + ri, canvas.x1), std::min(t.y1 + ri, canvas.y1)};
            const auto ww = static_cast<std::size_t>(win.width()), wh = static_cast<std::size_t>(win.height());
            std::vector<float> pm(ww * wh);
            float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
            for (std::size_t i = 0; i < pm.size(); ++i) {
                pm[i] = coarse(win.x0 + static_cast<int>(i % ww), win.y0 + static_cast<int>(i / ww));
                lo = std::min(lo, pm[i]);
                hi = std::max(hi, pm[i]);
            }
            const auto put = [&](int x, int y, std::uint8_t v) { mask[static_cast<std::size_t>(y - canvas.y0) * w + static_cast<std::size_t>(x - canvas.x0)] = v; };
            if (hi <= 0.0f || lo >= 1.0f) {
                const std::uint8_t v = lo >= 1.0f ? 255 : 0;
                for (int y = t.y0; y < t.y1; ++y)
                    for (int x = t.x0; x < t.x1; ++x) put(x, y, v);
                continue;
            }
            const RgbImage seen = sampler.rgb(win);
            std::vector<float> guide(seen.px.size());
            for (std::size_t i = 0; i < guide.size(); ++i) guide[i] = luma(seen.px[i]);
            const std::vector<float> q = matting::guided_filter_gray(guide, pm, ww, wh, r, eps);
            for (int y = t.y0; y < t.y1; ++y) {
                for (int x = t.x0; x < t.x1; ++x) {
                    const float v = q[static_cast<std::size_t>(y - win.y0) * ww + static_cast<std::size_t>(x - win.x0)];
                    put(x, y, static_cast<std::uint8_t>(std::round(smoothstep(0.5f - k, 0.5f + k, v) * 255.0f)));
                }
            }
        }
    }
    return trim_region(Region{canvas, std::move(mask)});
}

}  // namespace genko::render::segment
