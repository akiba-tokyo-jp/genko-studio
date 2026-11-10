// PC3 B3: the smart selection's common parts — PhotoCraft v0.2.0's algo/src/segment/mod.rs (MIT,
// native/third_party/photocraft/NOTICE.md): RGB images and samplers, the working resolution, the contrast-sensitive
// grid cut (Boykov & Jolly, ICCV 2001), components, the mask's cleaning and anti-aliasing, and the full-resolution re-cut
// of a working-resolution boundary in a narrow band, tile by tile.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/page.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

thread_local const std::stop_token* g_stop = nullptr;

// The 4 forward neighbours of an 8-connected grid and their lengths.
struct Step {
    int dx, dy;
    float len;
};
constexpr Step kNb[4] = {{1, 0, 1.0f}, {0, 1, 1.0f}, {1, 1, std::numbers::sqrt2_v<float>}, {-1, 1, std::numbers::sqrt2_v<float>}};

void add_fixed_link(Graph& g, std::size_t node, std::uint8_t fixed, float wt) {
    if (fixed == kHardFg) {
        g.add_tweights(node, wt, 0.0f);  // (labelling the node background cuts the link)
    } else {
        g.add_tweights(node, 0.0f, wt);
    }
}

std::pair<std::vector<std::uint32_t>, std::vector<std::size_t>> components4(const Mask& mask, std::size_t w, std::size_t h) {
    std::vector<std::uint32_t> lab(w * h, 0);
    std::vector<std::size_t> sizes{0};
    std::vector<std::size_t> stack;
    for (std::size_t s = 0; s < w * h; ++s) {
        if (mask[s] == 0 || lab[s] != 0) continue;
        const auto l = static_cast<std::uint32_t>(sizes.size());
        sizes.push_back(0);
        lab[s] = l;
        stack.push_back(s);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            ++sizes[l];
            const std::size_t x = i % w, y = i / w;
            const auto visit = [&](std::size_t j) {
                if (mask[j] != 0 && lab[j] == 0) {
                    lab[j] = l;
                    stack.push_back(j);
                }
            };
            if (x > 0) visit(i - 1);
            if (x + 1 < w) visit(i + 1);
            if (y > 0) visit(i - w);
            if (y + 1 < h) visit(i + w);
        }
    }
    return {std::move(lab), std::move(sizes)};
}

// The full-resolution graph cut within the band blocks (finish_region).
void refine_band(const Sampler& sampler, const Rect& window, std::size_t step, const Mask& low, const Mask& band, std::size_t lw, std::size_t lh,
                 const Gmm& fg, const Gmm& bg, std::vector<std::uint8_t>& mask) {
    constexpr int kT = 192;
    constexpr int kHalo = 12;
    const auto w = static_cast<std::size_t>(window.width());
    const auto block = [&](int x, int y) {
        return std::min(static_cast<std::size_t>(y - window.y0) / step, lh - 1) * lw + std::min(static_cast<std::size_t>(x - window.x0) / step, lw - 1);
    };
    for (int ty = window.y0; ty < window.y1; ty += kT) {
        for (int tx = window.x0; tx < window.x1; tx += kT) {
            const Rect t{tx, ty, std::min(tx + kT, window.x1), std::min(ty + kT, window.y1)};
            // (any band block over the tile?)
            const std::size_t bx0 = static_cast<std::size_t>(t.x0 - window.x0) / step, by0 = static_cast<std::size_t>(t.y0 - window.y0) / step;
            const std::size_t bx1 = std::min(static_cast<std::size_t>(t.x1 - 1 - window.x0) / step, lw - 1);
            const std::size_t by1 = std::min(static_cast<std::size_t>(t.y1 - 1 - window.y0) / step, lh - 1);
            bool any = false;
            for (std::size_t by = by0; by <= by1 && !any; ++by)
                for (std::size_t bx = bx0; bx <= bx1 && !any; ++bx) any = band[by * lw + bx] != 0;
            if (!any) continue;
            check_stop();
            const Rect r{std::max(t.x0 - kHalo, window.x0), std::max(t.y0 - kHalo, window.y0), std::min(t.x1 + kHalo, window.x1),
                         std::min(t.y1 + kHalo, window.y1)};
            const RgbImage img = sampler.rgb(r);
            const auto rw = static_cast<std::size_t>(r.width()), rh = static_cast<std::size_t>(r.height());
            std::vector<std::uint8_t> fixed(rw * rh, kFree);
            std::vector<float> cfg(rw * rh, 0.0f), cbg(rw * rh, 0.0f);
            for (std::size_t yy = 0; yy < rh; ++yy) {
                for (std::size_t xx = 0; xx < rw; ++xx) {
                    const std::size_t i = yy * rw + xx;
                    const std::size_t b = block(r.x0 + static_cast<int>(xx), r.y0 + static_cast<int>(yy));
                    const bool lab = low[b] != 0;
                    if (band[b] == 0) {
                        fixed[i] = lab ? kHardFg : kHardBg;
                        continue;
                    }
                    const Rgb& p = img.px[i];
                    // (the colour likelihoods and a weak prior toward the working-resolution label)
                    cfg[i] = fg.neg_log(p) + (lab ? 0.0f : 0.4f);
                    cbg[i] = bg.neg_log(p) + (lab ? 0.4f : 0.0f);
                }
            }
            const Mask cut = grid_cut(img, cfg, cbg, fixed, 50.0f, contrast_beta(img));
            const auto cw = static_cast<std::size_t>(t.width()), ch = static_cast<std::size_t>(t.height());
            for (std::size_t yy = 0; yy < ch; ++yy) {
                const std::size_t sy = static_cast<std::size_t>(t.y0 - r.y0) + yy, sx = static_cast<std::size_t>(t.x0 - r.x0);
                const std::size_t o = (static_cast<std::size_t>(t.y0 - window.y0) + yy) * w + static_cast<std::size_t>(t.x0 - window.x0);
                for (std::size_t xx = 0; xx < cw; ++xx) mask[o + xx] = cut[sy * rw + sx + xx] != 0 ? 255 : 0;
            }
        }
    }
}

}  // namespace

// --- stopping ---------------------------------------------------------------------------------------------------------

StopScope::StopScope(std::stop_token stop) : before_(g_stop), stop_(std::move(stop)) { g_stop = &stop_; }
StopScope::~StopScope() { g_stop = before_; }

void check_stop() {
    if (g_stop != nullptr && g_stop->stop_requested()) throw Cancelled();
}

const std::stop_token* current_stop() { return g_stop; }

// --- images and samplers ----------------------------------------------------------------------------------------------

RgbImage RgbImage::from_rgba(const std::vector<Rgba>& px, std::size_t w, std::size_t h) {
    RgbImage out(w, h);
    for (std::size_t i = 0; i < px.size() && i < out.px.size(); ++i) {
        const float a = std::clamp(px[i][3], 0.0f, 1.0f);
        for (std::size_t c = 0; c < 3; ++c) out.px[i][c] = std::clamp(px[i][c], 0.0f, 1.0f) * a + 0.5f * (1.0f - a);
    }
    return out;
}

RgbImage RgbImage::downsample(std::size_t step) const {
    if (step <= 1) return *this;
    const std::size_t ow = (w + step - 1) / step, oh = (h + step - 1) / step;
    RgbImage out(ow, oh);
    for (std::size_t oy = 0; oy < oh; ++oy) {
        for (std::size_t ox = 0; ox < ow; ++ox) {
            std::array<float, 3> acc{0, 0, 0};
            float n = 0;
            for (std::size_t y = oy * step; y < std::min((oy + 1) * step, h); ++y) {
                for (std::size_t x = ox * step; x < std::min((ox + 1) * step, w); ++x) {
                    const Rgb& p = at(x, y);
                    for (std::size_t c = 0; c < 3; ++c) acc[c] += p[c];
                    n += 1.0f;
                }
            }
            out.px[oy * ow + ox] = Rgb{acc[0] / n, acc[1] / n, acc[2] / n};
        }
    }
    return out;
}

RgbImage Sampler::rgb_scaled(const Rect& r, std::size_t step) const {
    if (step <= 1) return rgb(r);
    const auto w = static_cast<std::size_t>(r.width()), h = static_cast<std::size_t>(r.height());
    const std::size_t ow = (w + step - 1) / step, oh = (h + step - 1) / step;
    const std::size_t rows_per = std::max<std::size_t>((64 + step - 1) / step, 1);
    RgbImage out;
    out.w = ow;
    out.h = oh;
    out.px.reserve(ow * oh);
    for (std::size_t oy0 = 0; oy0 < oh; oy0 += rows_per) {
        check_stop();
        const std::size_t oy1 = std::min(oy0 + rows_per, oh);
        const int y0 = r.y0 + static_cast<int>(oy0 * step);
        const int y1 = std::min(r.y0 + static_cast<int>(oy1 * step), r.y1);
        const RgbImage strip = rgb(Rect{r.x0, y0, r.x1, y1}).downsample(step);
        out.px.insert(out.px.end(), strip.px.begin(), strip.px.end());
    }
    return out;
}

RgbImage ImageSampler::rgb(const Rect& r) const {
    RgbImage out(static_cast<std::size_t>(std::max(0, r.width())), static_cast<std::size_t>(std::max(0, r.height())));
    std::size_t i = 0;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x, ++i) {
            const int ix = x - origin_.first, iy = y - origin_.second;
            if (ix < 0 || iy < 0 || static_cast<std::size_t>(ix) >= img_.w || static_cast<std::size_t>(iy) >= img_.h) {
                out.px[i] = Rgb{0.5f, 0.5f, 0.5f};  // (clear: over mid grey)
            } else {
                out.px[i] = img_.at(static_cast<std::size_t>(ix), static_cast<std::size_t>(iy));
            }
        }
    }
    return out;
}

std::size_t scale_for(const Rect& area, std::size_t max_px) {
    const double n = static_cast<double>(area.width()) * static_cast<double>(area.height());
    return std::max<std::size_t>(static_cast<std::size_t>(std::ceil(std::sqrt(n / static_cast<double>(std::max<std::size_t>(max_px, 1))))), 1);
}

float d2(const Rgb& a, const Rgb& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

float contrast_beta(const RgbImage& img) {
    const auto w = static_cast<int>(img.w), h = static_cast<int>(img.h);
    double sum = 0;
    std::uint64_t n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgb& a = img.px[static_cast<std::size_t>(y * w + x)];
            for (const Step& s : kNb) {
                const int nx = x + s.dx, ny = y + s.dy;
                if (nx >= 0 && nx < w && ny < h) {
                    sum += static_cast<double>(d2(a, img.px[static_cast<std::size_t>(ny * w + nx)]));
                    ++n;
                }
            }
        }
    }
    if (n == 0 || sum <= 1e-12) return 0.0f;
    return static_cast<float>(static_cast<double>(n) / (2.0 * sum));
}

// --- the cut ----------------------------------------------------------------------------------------------------------

Mask grid_cut(const RgbImage& img, const std::vector<float>& cost_fg, const std::vector<float>& cost_bg, const std::vector<std::uint8_t>& fixed,
              float gamma, float beta) {
    const auto w = static_cast<int>(img.w), h = static_cast<int>(img.h);
    const std::size_t n = img.w * img.h;
    constexpr std::uint32_t kNot = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> idx(n, kNot);
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (fixed[i] == kFree) idx[i] = count++;
    Mask out(n, 0);
    for (std::size_t i = 0; i < n; ++i) out[i] = fixed[i] == kHardFg ? 1 : 0;
    if (count == 0) return out;
    Graph g(count, static_cast<std::size_t>(count) * 4);
    for (std::size_t i = 0; i < n; ++i)
        if (idx[i] != kNot) g.add_tweights(idx[i], cost_bg[i], cost_fg[i]);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto i = static_cast<std::size_t>(y * w + x);
            const Rgb& a = img.px[i];
            for (const Step& s : kNb) {
                const int nx = x + s.dx, ny = y + s.dy;
                if (nx < 0 || nx >= w || ny >= h) continue;
                const auto j = static_cast<std::size_t>(ny * w + nx);
                const float wt = gamma * std::exp(-beta * d2(a, img.px[j])) / s.len;
                const bool fi = idx[i] != kNot, fj = idx[j] != kNot;
                if (fi && fj) {
                    g.add_edge(idx[i], idx[j], wt, wt);
                } else if (fi) {
                    add_fixed_link(g, idx[i], fixed[j], wt);  // (the neighbour fixed: disagreeing with it costs wt)
                } else if (fj) {
                    add_fixed_link(g, idx[j], fixed[i], wt);
                }
            }
        }
    }
    g.maxflow();
    for (std::size_t i = 0; i < n; ++i)
        if (idx[i] != kNot) out[i] = g.in_source(idx[i]) ? 1 : 0;
    return out;
}

std::pair<std::vector<float>, std::vector<float>> data_costs(const RgbImage& img, const Gmm& fg, const Gmm& bg) {
    std::vector<float> cf(img.px.size()), cb(img.px.size());
    for (std::size_t i = 0; i < img.px.size(); ++i) {
        cf[i] = fg.neg_log(img.px[i]);
        cb[i] = bg.neg_log(img.px[i]);
    }
    return {std::move(cf), std::move(cb)};
}

std::pair<std::vector<std::uint32_t>, std::vector<std::size_t>> components(const Mask& mask, std::size_t w, std::size_t h) {
    std::vector<std::uint32_t> lab(w * h, 0);
    std::vector<std::size_t> sizes{0};
    std::vector<std::size_t> stack;
    for (std::size_t s = 0; s < w * h; ++s) {
        if (mask[s] == 0 || lab[s] != 0) continue;
        const auto l = static_cast<std::uint32_t>(sizes.size());
        sizes.push_back(0);
        lab[s] = l;
        stack.push_back(s);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            ++sizes[l];
            const auto x = static_cast<std::int64_t>(i % w), y = static_cast<std::int64_t>(i / w);
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const std::int64_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<std::int64_t>(w) || ny >= static_cast<std::int64_t>(h)) continue;
                    const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                    if (mask[j] != 0 && lab[j] == 0) {
                        lab[j] = l;
                        stack.push_back(j);
                    }
                }
            }
        }
    }
    return {std::move(lab), std::move(sizes)};
}

Mask keep_seeded(const Mask& mask, const Mask& seeds, std::size_t w, std::size_t h) {
    const auto [lab, sizes] = components(mask, w, h);
    std::vector<std::uint8_t> keep(sizes.size(), 0);
    for (std::size_t i = 0; i < lab.size(); ++i)
        if (seeds[i] != 0 && lab[i] != 0) keep[lab[i]] = 1;
    Mask out(lab.size(), 0);
    for (std::size_t i = 0; i < lab.size(); ++i) out[i] = lab[i] != 0 && keep[lab[i]] != 0 ? 1 : 0;
    return out;
}

Mask clean_mask(const Mask& mask, std::size_t w, std::size_t h, float frac, float hole_frac) {
    const auto [lab, sizes] = components(mask, w, h);
    std::size_t largest = 0;
    for (std::size_t l = 1; l < sizes.size(); ++l) largest = std::max(largest, sizes[l]);
    if (largest == 0) return Mask(w * h, 0);
    const auto min = static_cast<std::size_t>(std::ceil(static_cast<float>(largest) * frac));
    Mask out(w * h, 0);
    std::size_t area = 0;
    for (std::size_t i = 0; i < lab.size(); ++i) {
        out[i] = lab[i] != 0 && sizes[lab[i]] >= min ? 1 : 0;
        area += out[i];
    }
    const auto max_hole = static_cast<std::size_t>(static_cast<float>(area) * hole_frac);
    Mask inv(w * h, 0);
    for (std::size_t i = 0; i < inv.size(); ++i) inv[i] = out[i] == 0 ? 1 : 0;
    const auto [hl, hs] = components4(inv, w, h);
    std::vector<std::uint8_t> touches(hs.size(), 0);
    for (std::size_t x = 0; x < w; ++x) {
        touches[hl[x]] = 1;
        touches[hl[(h - 1) * w + x]] = 1;
    }
    for (std::size_t y = 0; y < h; ++y) {
        touches[hl[y * w]] = 1;
        touches[hl[y * w + w - 1]] = 1;
    }
    for (std::size_t i = 0; i < out.size(); ++i)
        if (hl[i] != 0 && touches[hl[i]] == 0 && hs[hl[i]] <= max_hole) out[i] = 1;
    return out;
}

void antialias_u8(std::vector<std::uint8_t>& mask, std::size_t w, std::size_t h) {
    const std::vector<std::uint8_t> src = mask;
    const auto at = [&](std::int64_t x, std::int64_t y, std::uint8_t v) -> std::uint8_t {
        if (x < 0 || y < 0 || x >= static_cast<std::int64_t>(w) || y >= static_cast<std::int64_t>(h)) return v;
        return src[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
    };
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::uint8_t v = src[y * w + x];
            std::uint32_t sum = 0;
            bool edge = false;
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const std::uint8_t nb = at(static_cast<std::int64_t>(x) + dx, static_cast<std::int64_t>(y) + dy, v);
                    edge = edge || nb != v;
                    sum += nb;
                }
            }
            if (edge) {
                const float avg = static_cast<float>(sum) / (9.0f * 255.0f);
                const float f = v > 127 ? 0.5f + 0.5f * avg : 0.5f * avg;
                mask[y * w + x] = static_cast<std::uint8_t>(std::round(f * 255.0f));
            }
        }
    }
}

std::optional<Region> trim_region(Region r) {
    const auto w = static_cast<std::size_t>(r.box.width());
    if (w == 0 || r.mask.empty()) return std::nullopt;
    const std::size_t h = r.mask.size() / w;
    std::size_t x0 = std::numeric_limits<std::size_t>::max(), y0 = x0, x1 = 0, y1 = 0;
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* row = r.mask.data() + y * w;
        std::size_t a = 0;
        while (a < w && row[a] == 0) ++a;
        if (a == w) continue;
        std::size_t b = w;
        while (row[b - 1] == 0) --b;
        x0 = std::min(x0, a);
        x1 = std::max(x1, b);
        y0 = std::min(y0, y);
        y1 = y + 1;
    }
    if (x0 == std::numeric_limits<std::size_t>::max()) return std::nullopt;
    if (x0 == 0 && y0 == 0 && x1 == w && y1 == h) return r;
    const std::size_t nw = x1 - x0;
    std::vector<std::uint8_t> mask;
    mask.reserve(nw * (y1 - y0));
    for (std::size_t y = y0; y < y1; ++y) mask.insert(mask.end(), r.mask.begin() + static_cast<std::ptrdiff_t>(y * w + x0), r.mask.begin() + static_cast<std::ptrdiff_t>(y * w + x1));
    const Rect& b = r.box;
    return Region{Rect{b.x0 + static_cast<int>(x0), b.y0 + static_cast<int>(y0), b.x0 + static_cast<int>(x1), b.y0 + static_cast<int>(y1)}, std::move(mask)};
}

std::optional<Region> finish_region(const Sampler& sampler, const Rect& window, std::size_t step, const Mask& low, std::size_t lw,
                                    std::size_t lh, const Gmm* fg, const Gmm* bg) {
    const auto w = static_cast<std::size_t>(window.width()), h = static_cast<std::size_t>(window.height());
    std::vector<std::uint8_t> mask(w * h, 0);
    if (step <= 1) {
        for (std::size_t i = 0; i < mask.size(); ++i) mask[i] = low[i] != 0 ? 255 : 0;
    } else {
        // the band blocks: working pixels with a pixel of the other label within one
        Mask band(lw * lh, 0);
        for (std::size_t y = 0; y < lh; ++y) {
            for (std::size_t x = 0; x < lw; ++x) {
                const std::uint8_t v = low[y * lw + x] != 0 ? 1 : 0;
                bool differs = false;
                for (int dy = -1; dy <= 1 && !differs; ++dy) {
                    for (int dx = -1; dx <= 1 && !differs; ++dx) {
                        const std::int64_t nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                        if (nx >= 0 && ny >= 0 && nx < static_cast<std::int64_t>(lw) && ny < static_cast<std::int64_t>(lh) &&
                            (low[static_cast<std::size_t>(ny) * lw + static_cast<std::size_t>(nx)] != 0 ? 1 : 0) != v) {
                            differs = true;
                        }
                    }
                }
                band[y * lw + x] = differs ? 1 : 0;
            }
        }
        for (std::size_t y = 0; y < h; ++y) {
            const std::size_t ly = std::min(y / step, lh - 1);
            for (std::size_t x = 0; x < w; ++x) mask[y * w + x] = low[ly * lw + std::min(x / step, lw - 1)] != 0 ? 255 : 0;
        }
        if (fg != nullptr && bg != nullptr) refine_band(sampler, window, step, low, band, lw, lh, *fg, *bg, mask);
    }
    antialias_u8(mask, w, h);
    return trim_region(Region{window, std::move(mask)});
}

// --- numbers ----------------------------------------------------------------------------------------------------------

std::uint64_t Rng::next_u64() {
    std::uint64_t x = s_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s_ = x;
    return x;
}

float Rng::f32() { return static_cast<float>(next_u64() >> 40) / static_cast<float>(std::uint64_t(1) << 24); }

float Rng::normal() {
    const float u = std::max(f32(), 1e-7f);
    const float v = f32();
    return std::sqrt(-2.0f * std::log(u)) * std::cos(2.0f * std::numbers::pi_v<float> * v);
}

std::vector<Rgb> subsample(const std::vector<Rgb>& v, std::size_t max) {
    if (v.size() <= max) return v;
    const std::size_t k = (v.size() + max - 1) / max;
    std::vector<Rgb> out;
    out.reserve(v.size() / k + 1);
    for (std::size_t i = 0; i < v.size(); i += k) out.push_back(v[i]);
    return out;
}

}  // namespace genko::render::segment
