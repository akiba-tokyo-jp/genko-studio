#include "render/paint_tools/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "core/error.hpp"
#include "render/ink_canvas.hpp"

// PhotoCraft v0.2.0 crates/engine/src/retouch_cmds.rs (lines 434-575), crates/paint/src/mixer.rs (lines 1-140),
// crates/paint/src/bg_erase.rs (lines 1-170) and crates/paint/src/replace.rs (lines 1-200), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::paint_tools {

namespace {

using image_ops::Pixels;
using retouch::Footprint;
using tiled::TiledPixels;

Box meet(const Box& a, const Box& b) {
    const Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return out.x1 > out.x0 && out.y1 > out.y0 ? out : Box{};
}

bool empty(const Box& b) { return b.width() <= 0 || b.height() <= 0; }

// The footprint's box within the grid.
Box within(const Footprint& fp, Size grid) { return meet(fp.rect, Box{0, 0, grid.width, grid.height}); }

// How many of a dab's pixels go through the colours at once (an ink layer's through the page's rules: a chunk's doubles,
// not the dab's whole — a wide brush's dab is millions of pixels).
constexpr std::size_t kChunk = 16384;

// A place of the grid.
struct Spot {
    int x = 0, y = 0;
    float k = 0.0f;
};

// The spots of `box` that `keep` takes (it gives the spot's weight: none for a spot left out), `act` on each chunk of them.
template <class Keep, class Act>
void in_chunks(const Box& box, Keep&& keep, Act&& act) {
    std::vector<Spot> chunk;
    chunk.reserve(std::min<std::size_t>(kChunk, std::size_t(box.width()) * std::size_t(box.height())));
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            const std::optional<float> k = keep(x, y);
            if (!k) continue;
            chunk.push_back(Spot{x, y, *k});
            if (chunk.size() == kChunk) {
                act(chunk);
                chunk.clear();
            }
        }
    }
    if (!chunk.empty()) act(chunk);
}

// The largest difference between two colours' values.
float distance(const float* a, const float* b, unsigned n) {
    float m = 0.0f;
    for (unsigned c = 0; c < n; ++c) m = std::max(m, std::fabs(a[c] - b[c]));
    return m;
}

// How well a difference matches within the tolerance: past it nothing, the last quarter of it falling softly when `soft`.
float match_weight(float d, float tolerance, bool soft) {
    if (d > tolerance) return 0.0f;
    if (soft && tolerance > 0.0f) return std::clamp((tolerance - d) / (tolerance * 0.25f), 0.0f, 1.0f);
    return 1.0f;
}

// The weights not 4-connected to (sx, sy) through weights above 0 made 0.
void keep_connected(std::vector<float>& weight, std::size_t w, std::size_t h, std::size_t sx, std::size_t sy) {
    std::vector<std::uint8_t> keep(w * h, 0);
    const std::size_t si = sy * w + sx;
    if (si < weight.size() && weight[si] > 0.0f) {
        std::vector<std::size_t> stack{si};
        keep[si] = 1;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const std::size_t x = i % w, y = i / w;
            const auto next = [&](std::size_t j) {
                if (keep[j] == 0 && weight[j] > 0.0f) {
                    keep[j] = 1;
                    stack.push_back(j);
                }
            };
            if (x > 0) next(i - 1);
            if (x + 1 < w) next(i + 1);
            if (y > 0) next(i - w);
            if (y + 1 < h) next(i + w);
        }
    }
    for (std::size_t i = 0; i < weight.size(); ++i) {
        if (keep[i] == 0) weight[i] = 0.0f;
    }
}

}  // namespace

// --- the colours -----------------------------------------------------------------------------------------------------------

Colours::Colours(const Pixels& like) : colours_(like.colours()) {
    if (like.kind() == Pixels::Kind::Inks && like.ink) {
        ink_ = *like.ink;
        rules_.emplace(*ink_);
        floating_ = ink_->depth == 32;
    } else {
        floating_ = like.kind() == Pixels::Kind::Precise && like.floating;
    }
}

void Colours::to_rgb(const std::vector<const float*>& px, std::vector<float>& rgb) const {
    rgb.resize(px.size() * 3);
    if (!ink_) {
        for (std::size_t i = 0; i < px.size(); ++i) std::copy_n(px[i], 3, rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
        return;
    }
    const unsigned n = colours_;
    std::vector<double> inks(px.size() * n), out(px.size() * 3);
    for (std::size_t i = 0; i < px.size(); ++i) {
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(ink_->space, c, px[i][c]);
    }
    rules_->meaning(inks.data(), out.data(), px.size());
    for (std::size_t i = 0; i < out.size(); ++i) rgb[i] = static_cast<float>(out[i]);
}

void Colours::from_rgb(const std::vector<float*>& px, const std::vector<float>& rgb, const std::vector<float>& before) const {
    if (!ink_) {
        for (std::size_t i = 0; i < px.size(); ++i) std::copy_n(rgb.begin() + static_cast<std::ptrdiff_t>(i * 3), 3, px[i]);
        return;
    }
    const unsigned n = colours_;
    std::vector<double> inks(px.size() * n), out(px.size() * n);
    for (std::size_t i = 0; i < px.size(); ++i) {
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(ink_->space, c, px[i][c]);
    }
    std::size_t k = 0;  // (asked for each pixel once, in their order)
    rules_->correct(inks.data(), out.data(), px.size(), [&](const std::array<double, 3>& was) {
        const std::size_t i = 3 * k++;
        // (a colour the formula left as to_rgb gave it — to a float — is the colour it was: its inks are kept)
        if (before.size() == rgb.size() && rgb[i] == before[i] && rgb[i + 1] == before[i + 1] && rgb[i + 2] == before[i + 2]) return was;
        return std::array<double, 3>{rgb[i], rgb[i + 1], rgb[i + 2]};
    });
    for (std::size_t i = 0; i < px.size(); ++i) {
        for (unsigned c = 0; c < n; ++c) px[i][c] = static_cast<float>(to_canvas(ink_->space, c, out[i * n + c]));
    }
}

std::vector<float> Colours::values_of(const std::array<float, 3>& rgb) const {
    if (!ink_) return {rgb[0], rgb[1], rgb[2]};
    const double in[3] = {rgb[0], rgb[1], rgb[2]};
    std::vector<double> inks(colours_);
    rules_->inks(in, inks.data(), 1);
    std::vector<float> out(colours_);
    for (unsigned c = 0; c < colours_; ++c) out[c] = static_cast<float>(to_canvas(ink_->space, c, inks[c]));
    return out;
}

// --- Dodge, Burn, Sponge ---------------------------------------------------------------------------------------------------

void colour_dab(TiledPixels& work, const Footprint& fp, const Colours& colours, float strength, float spacing, bool keep_bright,
                const ColourFormula& f) {
    const Box r = within(fp, work.grid());
    if (empty(r)) return;
    const unsigned n = colours.colours();
    const bool bright = keep_bright && colours.floating();
    std::vector<const float*> seen;
    std::vector<float*> edit;
    std::vector<float> rgb, before;
    in_chunks(
        r,
        [&](int x, int y) -> std::optional<float> {
            const float k = per_dab(fp.at(x, y) * strength, spacing);
            if (!(k > 0.0f)) return std::nullopt;
            const float* v = work.at(x, y);
            if (!(v[n] > 0.0f)) return std::nullopt;  // (a clear pixel's colour shows nothing: left as it is)
            if (bright && std::any_of(v, v + n, [](float c) { return c > 1.0f; })) return std::nullopt;  // (HDR kept)
            return k;
        },
        [&](const std::vector<Spot>& spots) {
            seen.clear();
            for (const Spot& s : spots) seen.push_back(work.at(s.x, s.y));
            colours.to_rgb(seen, rgb);
            before = rgb;
            for (std::size_t i = 0; i < spots.size(); ++i) {
                const std::array<float, 3> o = f({rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]}, spots[i].k);
                std::copy(o.begin(), o.end(), rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
            }
            edit.clear();
            for (const Spot& s : spots) edit.push_back(work.edit(s.x, s.y));
            colours.from_rgb(edit, rgb, before);
        });
}

// --- Blur, Sharpen -----------------------------------------------------------------------------------------------------------

float focus_sigma(float diameter) { return std::clamp(diameter * 0.03f, 1.0f, 4.0f); }

int focus_halo(float sigma) { return static_cast<int>(std::ceil(sigma * 3.0f)) + 1; }

void focus_dab(TiledPixels& work, const Footprint& fp, bool sharpen, float sigma, float strength, float spacing, bool protect_detail) {
    const Size grid = work.grid();
    const Box r = within(fp, grid);
    if (empty(r)) return;
    const unsigned n = work.all();
    const int reach = sharpen ? 3 : std::max(static_cast<int>(std::ceil(sigma * 3.0f)), 1);
    // (the window's passes in floats: across over its rows grown by the reach, then the result)
    const image_ops::Held held((std::uint64_t(r.width()) * std::uint64_t(r.height() + 2 * reach) + std::uint64_t(r.width()) * std::uint64_t(r.height())) * n *
                               sizeof(float));
    const Reader get = [&work](int x, int y) { return work.at(x, y); };
    const int alpha = static_cast<int>(work.colours());
    const std::vector<float> out = sharpen ? local_sharpen(get, grid, static_cast<int>(n), alpha, r, 1.0f, protect_detail)
                                           : local_blur(get, grid, static_cast<int>(n), alpha, r, sigma);
    const auto rw = std::size_t(r.width());
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float k = per_dab(fp.at(x, y) * strength, spacing);
            if (!(k > 0.0f)) continue;
            const float* o = out.data() + (std::size_t(y - r.y0) * rw + std::size_t(x - r.x0)) * n;
            float* px = work.edit(x, y);
            for (unsigned c = 0; c < n; ++c) px[c] += (o[c] - px[c]) * k;
        }
    }
}

// --- the Mixer Brush -------------------------------------------------------------------------------------------------------

Mixer::Mixer(const Colours& colours, const MixerSettings& settings, Reservoir reservoir, bool lock_alpha, std::vector<image_ops::Rgba> sample,
             Box sample_box)
    : colours_(colours), m_(settings), state_(std::move(reservoir)), lock_(lock_alpha), sample_(std::move(sample)), sample_box_(sample_box) {
    m_.wet = std::clamp(m_.wet, 0.0f, 1.0f);
    m_.load = std::clamp(m_.load, 0.0f, 1.0f);
    m_.mix = std::clamp(m_.mix, 0.0f, 1.0f);
    m_.flow = std::clamp(m_.flow, 0.0f, 1.0f);
    // (the paint as loaded: its alpha full, as upstream's native colour; none loaded, nothing left of it)
    if (state_.paint) (*state_.paint)[3] = 1.0f;
    state_.amount = state_.paint ? std::clamp(state_.amount, 0.0f, 1.0f) : 0.0f;
    if (state_.pickup) (*state_.pickup)[3] = 1.0f;
}

void Mixer::dab(TiledPixels& work, const Footprint& fp) {
    const Box r = within(fp, work.grid());
    const unsigned n = colours_.colours();
    std::vector<const float*> seen;
    std::vector<float*> edit;
    std::vector<float> rgb, before;
    const auto covered = [&](int x, int y) -> std::optional<float> {
        const float w = fp.at(x, y);
        return w > 0.0f ? std::optional<float>(w) : std::nullopt;
    };
    // the canvas under the dab (the layer's colours, or every layer's when it samples them all), weighted by its coverage
    std::array<double, 3> acc{};
    double wsum = 0.0, asum = 0.0;
    if (!empty(r)) {
        in_chunks(r, covered, [&](const std::vector<Spot>& spots) {
            if (!sample_.empty()) {
                for (const Spot& s : spots) {
                    const bool in = s.x >= sample_box_.x0 && s.y >= sample_box_.y0 && s.x < sample_box_.x1 && s.y < sample_box_.y1;
                    if (!in) {
                        wsum += s.k;
                        continue;
                    }
                    const image_ops::Rgba& q =
                        sample_[std::size_t(s.y - sample_box_.y0) * std::size_t(sample_box_.width()) + std::size_t(s.x - sample_box_.x0)];
                    for (std::size_t c = 0; c < 3; ++c) acc[c] += double(q[c]) * s.k * q[3];
                    wsum += s.k;
                    asum += double(s.k) * q[3];
                }
                return;
            }
            seen.clear();
            for (const Spot& s : spots) seen.push_back(work.at(s.x, s.y));
            colours_.to_rgb(seen, rgb);
            for (std::size_t i = 0; i < spots.size(); ++i) {
                const float al = seen[i][n];
                for (std::size_t c = 0; c < 3; ++c) acc[c] += double(rgb[i * 3 + c]) * spots[i].k * al;
                wsum += spots[i].k;
                asum += double(spots[i].k) * al;
            }
        });
    }
    if (asum > 1e-6 && m_.wet > 0.0f) {
        const auto ca = static_cast<float>(asum / std::max(wsum, 1e-6));
        const std::array<float, 4> canvas{static_cast<float>(acc[0] / asum), static_cast<float>(acc[1] / asum), static_cast<float>(acc[2] / asum), 1.0f};
        if (!state_.pickup) {
            state_.pickup = canvas;
        } else {
            for (std::size_t c = 0; c < 4; ++c) (*state_.pickup)[c] += (canvas[c] - (*state_.pickup)[c]) * kPickupRate * m_.wet * ca;
        }
    }
    const float wr = state_.paint ? state_.amount * (1.0f - m_.mix * m_.wet) : 0.0f;
    const float wp = state_.pickup ? m_.mix * m_.wet : 0.0f;
    const float total = wr + wp;
    if (total > 1e-6f && !empty(r)) {
        std::array<float, 4> paint{};
        for (std::size_t c = 0; c < 4; ++c) {
            const float rv = state_.paint ? (*state_.paint)[c] : 0.0f;
            const float pv = state_.pickup ? (*state_.pickup)[c] : 0.0f;
            paint[c] = (rv * wr + pv * wp) / total;
        }
        const float deposit = std::min(total, 1.0f) * m_.flow;
        in_chunks(r, covered, [&](const std::vector<Spot>& spots) {
            seen.clear();
            for (const Spot& s : spots) seen.push_back(work.at(s.x, s.y));
            colours_.to_rgb(seen, rgb);
            before = rgb;
            std::vector<float> alpha(spots.size());
            for (std::size_t i = 0; i < spots.size(); ++i) {
                const float k = spots[i].k * deposit;
                float dst[4] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], seen[i][n]};
                if (k > 0.0f) retouch::over(dst, paint.data(), 3, k, lock_);
                std::copy_n(dst, 3, rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
                alpha[i] = dst[3];
            }
            edit.clear();
            for (const Spot& s : spots) edit.push_back(work.edit(s.x, s.y));
            colours_.from_rgb(edit, rgb, before);
            for (std::size_t i = 0; i < spots.size(); ++i) edit[i][n] = alpha[i];
        });
    }
    state_.amount *= 1.0f - 0.08f * (1.0f - m_.load);  // (a low load runs dry)
}

// --- the Background Eraser -------------------------------------------------------------------------------------------------

BackgroundEraser::BackgroundEraser(Sampling sampling, Limits limits, float tolerance, bool protect_foreground, std::vector<float> foreground,
                                   std::vector<float> swatch)
    : sampling_(sampling), limits_(limits), tolerance_(std::isfinite(tolerance) ? std::clamp(tolerance, 0.0f, 1.0f) : 0.0f), protect_(protect_foreground),
      foreground_(std::move(foreground)), swatch_(std::move(swatch)) {}

void BackgroundEraser::dab(TiledPixels& work, const Footprint& fp) {
    const Size grid = work.grid();
    const Box r = within(fp, grid);
    if (empty(r)) return;
    const unsigned n = work.colours();  // (the colour values; the alpha after them)
    const int cx = static_cast<int>(std::floor(fp.dab.x)), cy = static_cast<int>(std::floor(fp.dab.y));
    // the colour under the brush's centre (none where it is clear or off the layer)
    const auto hotspot = [&]() -> std::optional<std::vector<float>> {
        if (cx < 0 || cy < 0 || cx >= grid.width || cy >= grid.height || !work.holds(cx, cy)) return std::nullopt;
        const float* p = work.at(cx, cy);
        if (!(p[n] > 0.0f)) return std::nullopt;
        return std::vector<float>(p, p + n);
    };
    std::optional<std::vector<float>> sampled;
    switch (sampling_) {
        case Sampling::Continuous: sampled = hotspot(); break;
        case Sampling::Once:
            if (!once_) once_ = hotspot();
            sampled = *once_;
            break;
        case Sampling::Swatch: sampled = swatch_; break;
    }
    if (!sampled || sampled->size() < n) return;
    const bool soft = limits_ != Limits::FindEdges;
    const auto w = std::size_t(r.width()), h = std::size_t(r.height());
    const image_ops::Held held(std::uint64_t(w) * h * (sizeof(float) + 1 + sizeof(std::size_t)));  // (the weights, the fill's marks and stack)
    // each pixel's match (erasing only lowers the alpha: the working copy still holds the colours before the stroke)
    std::vector<float> weight(w * h, 0.0f);
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            if (fp.at(x, y) <= 0.0f) continue;
            const float* p = work.at(x, y);
            if (!(p[n] > 0.0f)) continue;
            if (protect_ && foreground_.size() >= n && distance(p, foreground_.data(), n) <= tolerance_) continue;
            weight[std::size_t(y - r.y0) * w + std::size_t(x - r.x0)] = match_weight(distance(p, sampled->data(), n), tolerance_, soft);
        }
    }
    if (limits_ != Limits::Discontiguous) {
        keep_connected(weight, w, h, std::size_t(std::clamp(cx, r.x0, r.x1 - 1) - r.x0), std::size_t(std::clamp(cy, r.y0, r.y1 - 1) - r.y0));
    }
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float k = fp.at(x, y) * weight[std::size_t(y - r.y0) * w + std::size_t(x - r.x0)];
            if (!(k > 0.0f)) continue;
            float* p = work.edit(x, y);
            p[n] *= 1.0f - std::min(k, 1.0f);
            if (p[n] <= 0.0f) std::fill(p, p + n + 1, 0.0f);
        }
    }
}

// --- Color Replacement -----------------------------------------------------------------------------------------------------

ColorReplacement::ColorReplacement(const Colours& colours, const TiledPixels& before, std::string mode, Sampling sampling, Limits limits, float tolerance,
                                   bool antialias, std::array<float, 3> colour, std::array<float, 3> swatch)
    : colours_(colours), before_(before), mode_(std::move(mode)), sampling_(sampling), limits_(limits),
      tolerance_(std::isfinite(tolerance) ? std::clamp(tolerance, 0.0f, 1.0f) : 0.0f), antialias_(antialias), colour_(colour), swatch_(swatch) {}

void ColorReplacement::dab(TiledPixels& work, const Footprint& fp) {
    const Size grid = work.grid();
    const int cx = static_cast<int>(std::floor(fp.dab.x)), cy = static_cast<int>(std::floor(fp.dab.y));
    // the colour sampled: the layer's before the stroke under the brush's centre (none off the layer), or the swatch
    const auto centre = [&]() -> std::optional<std::array<float, 3>> {
        if (cx < 0 || cy < 0 || cx >= grid.width || cy >= grid.height || !before_.holds(cx, cy)) return std::nullopt;
        std::vector<float> rgb;
        colours_.to_rgb({before_.at(cx, cy)}, rgb);
        return std::array<float, 3>{rgb[0], rgb[1], rgb[2]};
    };
    std::optional<std::array<float, 3>> sampled;
    switch (sampling_) {
        case Sampling::Continuous: sampled = centre(); break;
        case Sampling::Once:
            if (!once_) once_ = centre();
            sampled = once_;
            break;
        case Sampling::Swatch: sampled = swatch_; break;
    }
    const Box r = within(fp, grid);
    if (!sampled || empty(r)) return;
    const unsigned n = colours_.colours();
    const bool soft = antialias_ && limits_ != Limits::FindEdges;
    const auto w = std::size_t(r.width()), h = std::size_t(r.height());
    const image_ops::Held held(std::uint64_t(w) * h * (sizeof(float) + 1 + sizeof(std::size_t)));  // (the weights, the fill's marks and stack)
    // each pixel's match against its colour before the stroke (what earlier dabs recoloured still counts as the sampled)
    std::vector<float> weight(w * h, 0.0f);
    std::vector<const float*> seen;
    std::vector<float*> edit;
    std::vector<float> rgb, before;
    const float s3[3] = {(*sampled)[0], (*sampled)[1], (*sampled)[2]};
    in_chunks(
        r,
        [&](int x, int y) -> std::optional<float> {
            if (fp.at(x, y) <= 0.0f || !(before_.at(x, y)[n] > 0.0f)) return std::nullopt;
            return 1.0f;
        },
        [&](const std::vector<Spot>& spots) {
            seen.clear();
            for (const Spot& s : spots) seen.push_back(before_.at(s.x, s.y));
            colours_.to_rgb(seen, rgb);
            for (std::size_t i = 0; i < spots.size(); ++i) {
                weight[std::size_t(spots[i].y - r.y0) * w + std::size_t(spots[i].x - r.x0)] = match_weight(distance(rgb.data() + i * 3, s3, 3), tolerance_, soft);
            }
        });
    if (limits_ != Limits::Discontiguous) {
        keep_connected(weight, w, h, std::size_t(std::clamp(cx, r.x0, r.x1 - 1) - r.x0), std::size_t(std::clamp(cy, r.y0, r.y1 - 1) - r.y0));
    }
    in_chunks(
        r,
        [&](int x, int y) -> std::optional<float> {
            const float k = fp.at(x, y) * weight[std::size_t(y - r.y0) * w + std::size_t(x - r.x0)];
            return k > 0.0f ? std::optional<float>(k) : std::nullopt;
        },
        [&](const std::vector<Spot>& spots) {
            seen.clear();
            for (const Spot& s : spots) seen.push_back(work.at(s.x, s.y));
            colours_.to_rgb(seen, rgb);
            before = rgb;
            for (std::size_t i = 0; i < spots.size(); ++i) {
                const std::array<float, 3> c{rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
                const std::array<float, 3> b = image_ops::blend_rgb(mode_, c, colour_);
                for (std::size_t q = 0; q < 3; ++q) rgb[i * 3 + q] = c[q] + (b[q] - c[q]) * spots[i].k;
            }
            edit.clear();
            for (const Spot& s : spots) edit.push_back(work.edit(s.x, s.y));
            colours_.from_rgb(edit, rgb, before);
        });
}

}  // namespace genko::render::paint_tools
