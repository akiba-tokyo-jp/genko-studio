#include "render/dabs/coverage.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "render/dabs/maths.hpp"
#include "render/dabs/rng.hpp"

// PhotoCraft v0.2.0 crates/paint/src/render.rs (mask_combine, BrushContext, CoverageMap, StrokeRenderer's laying) and
// lib.rs (dab_coverage), MIT: native/third_party/photocraft/NOTICE.md. Upstream's f32 arithmetic is kept for the
// coverage; the dabs' places are mm (render/dabs/dynamics.hpp), turned into pixels of the resolution here.

namespace genko::render::dabs {

namespace {

using core::DabMaskMode;

constexpr std::uint64_t kNoiseSalt = 0x006E6F697365ULL;
constexpr double kSqrt2 = 1.4142135623730951;

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Box meet(const Box& a, const Box& b) {
    Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (out.x1 <= out.x0 || out.y1 <= out.y0) return Box{};
    return out;
}

bool blank(const Box& b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

Box join(const Box& a, const Box& b) {
    if (blank(a)) return b;
    if (blank(b)) return a;
    return Box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

}  // namespace

float mask_combine(DabMaskMode mode, float v, float t, float d) {
    if (v <= 0.0f) return 0.0f;
    const auto lerp = [d](float a, float b) { return a + (b - a) * d; };
    float r = 0.0f;
    switch (mode) {
        case DabMaskMode::Multiply: r = v * (1.0f - d + d * t); break;
        case DabMaskMode::Subtract: r = std::max(v - d * (1.0f - t), 0.0f); break;
        case DabMaskMode::Darken: r = std::min(v, 1.0f - d + d * t); break;
        case DabMaskMode::Overlay: r = lerp(v, v < 0.5f ? 2.0f * v * t : 1.0f - 2.0f * (1.0f - v) * (1.0f - t)); break;
        case DabMaskMode::ColorDodge: r = lerp(v, std::min(v / std::max(1.0f - t, 1e-3f), 1.0f)); break;
        case DabMaskMode::ColorBurn: r = lerp(v, 1.0f - std::min((1.0f - v) / std::max(t, 1e-3f), 1.0f)); break;
        case DabMaskMode::LinearBurn: r = lerp(v, std::max(v + t - 1.0f, 0.0f)); break;
        case DabMaskMode::HardMix: r = lerp(v, v + t > 1.0f ? 1.0f : 0.0f); break;
        case DabMaskMode::LinearHeight: r = v * std::clamp((t - (1.0f - d * v)) * 4.0f + 0.5f, 0.0f, 1.0f); break;
        case DabMaskMode::Height: r = t >= 1.0f - d * v ? v : 0.0f; break;
    }
    return std::clamp(r, 0.0f, 1.0f);
}

float dab_coverage(float d, float radius, float hardness) {
    if (d >= radius + 0.5f) return 0.0f;
    const float edge = std::min(std::clamp(radius + 0.5f - d, 0.0f, 1.0f), 1.0f);
    const float h = std::clamp(hardness, 0.0f, 1.0f);
    const float inner = radius * h;
    float falloff = 1.0f;
    if (!(d <= inner || radius - inner < 1e-3f)) {
        const float t = std::clamp((d - inner) / (radius - inner), 0.0f, 1.0f);
        falloff = 1.0f - t * t * (3.0f - 2.0f * t);  // (smoothstep)
    }
    return edge * falloff;
}

std::shared_ptr<const PatternImage> prepare_pattern(const core::DabTexture& t, const PatternImage& picture) {
    if (!t.enabled) return nullptr;
    auto img = std::make_shared<PatternImage>(picture);
    const auto br = static_cast<float>(std::clamp(t.brightness, -1.0, 1.0)), ct = static_cast<float>(std::clamp(t.contrast, -1.0, 1.0));
    for (float& v : img->data) {
        const float x = std::clamp((v - 0.5f) * (1.0f + ct) + 0.5f + br, 0.0f, 1.0f);
        v = t.invert ? 1.0f - x : x;
    }
    return img;
}

BrushContext::BrushContext(const core::DabSettings& settings, std::shared_ptr<const Mips> tip, std::shared_ptr<const Mips> dual_tip,
                           std::shared_ptr<const PatternImage> texture, int dpi)
    : settings_(settings),
      tip_(std::move(tip)),
      dual_tip_(settings.dual.enabled ? std::move(dual_tip) : nullptr),
      texture_(settings.texture.enabled ? std::move(texture) : nullptr),
      dpi_(dpi),
      px_per_mm_(dpi / 25.4),
      texel_(300.0 / (dpi * std::max(settings.texture.scale, 0.01))) {}

float BrushContext::texture_at(int x, int y) const {
    if (!texture_) return 1.0f;
    // (the page's pixel centre in texture pixels: one is scale / 300 inch; at a whole match the texels themselves)
    const double fx = (x + 0.5) * texel_ - 0.5, fy = (y + 0.5) * texel_ - 0.5;
    const double x0 = std::floor(fx), y0 = std::floor(fy);
    const auto tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
    const PatternImage& p = *texture_;
    const float a = p.sample_wrap(x0, y0) + (p.sample_wrap(x0 + 1, y0) - p.sample_wrap(x0, y0)) * tx;
    const float b = p.sample_wrap(x0, y0 + 1) + (p.sample_wrap(x0 + 1, y0 + 1) - p.sample_wrap(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

Box BrushContext::dab_rect(const Dab& d, bool dual) const {
    const bool sampled = dual ? dual_tip_ != nullptr : tip_ != nullptr;
    const double r = d.radius * px_per_mm_;
    const double reach = sampled ? r * kSqrt2 : r;
    const int rr = static_cast<int>(std::ceil(reach)) + 1;
    const int cx = static_cast<int>(std::floor(d.x * px_per_mm_)), cy = static_cast<int>(std::floor(d.y * px_per_mm_));
    return Box{cx - rr, cy - rr, cx + rr + 1, cy + rr + 1};
}

void BrushContext::rasterize(const Dab& d, bool dual, const Box& rect, std::vector<float>& out) const {
    const auto w = static_cast<std::size_t>(rect.width()), h = static_cast<std::size_t>(rect.height());
    out.assign(w * h, 0.0f);
    const core::DabSettings& b = settings_;
    const float hardness = static_cast<float>(dual ? b.dual.hardness : b.hardness);
    const Mips* mips = dual ? dual_tip_.get() : tip_.get();
    const bool aliased = b.aliased && !dual;
    const bool wet = b.wet_edges && !dual;
    const bool noise = b.noise && !dual;
    const bool tex_tip = !dual && b.texture.enabled && b.texture.each_tip && texture_ != nullptr;
    const auto cx = static_cast<float>(d.x * px_per_mm_), cy = static_cast<float>(d.y * px_per_mm_);
    const auto [sn_d, cs_d] = sin_cos(d.angle);
    const auto sn = static_cast<float>(sn_d), cs = static_cast<float>(cs_d);
    const float fx = d.flip_x ? -1.0f : 1.0f, fy = d.flip_y ? -1.0f : 1.0f;
    const auto r = static_cast<float>(d.radius * px_per_mm_);
    const float ro = std::min(std::max(static_cast<float>(d.roundness), 0.5f / r), 1.0f);
    const float rm = std::max(r * ro, 0.5f);
    const float reach2 = (rm + 0.5f) * (rm + 0.5f);
    std::size_t level = 0;
    float inv_scale = 1.0f, tw = 1.0f, th = 1.0f;
    if (mips != nullptr) {
        const auto& l0 = mips->levels.front();
        const auto big = static_cast<float>(std::max(l0.width, l0.height));
        level = mips->level_for(2.0f * r * std::max(ro, 0.25f));
        inv_scale = big / (2.0f * r);
        tw = static_cast<float>(l0.width);
        th = static_cast<float>(l0.height);
    }
    for (std::size_t yy = 0; yy < h; ++yy) {
        const int y = rect.y0 + static_cast<int>(yy);
        const float dy = static_cast<float>(y) + 0.5f - cy;
        for (std::size_t xx = 0; xx < w; ++xx) {
            const int x = rect.x0 + static_cast<int>(xx);
            const float dx = static_cast<float>(x) + 0.5f - cx;
            // (to y up, turned by −angle, flipped)
            const float ux = dx, uy = -dy;
            const float u = (ux * cs + uy * sn) * fx;
            const float v = (-ux * sn + uy * cs) * fy;
            float val = 0.0f, rn = 0.0f;
            if (mips == nullptr) {
                const float d2 = (u * ro) * (u * ro) + v * v;
                if (d2 >= reach2) continue;
                const float dd = std::sqrt(d2);
                val = aliased ? (dd <= rm ? 1.0f : 0.0f) : dab_coverage(dd, rm, hardness);
                rn = dd / rm;
            } else {
                const float px = u * inv_scale + tw / 2.0f, py = -(v / ro) * inv_scale + th / 2.0f;
                if (px < -1.0f || py < -1.0f || px > tw + 1.0f || py > th + 1.0f) continue;
                const float s = mips->sample_clamped(level, px / tw, py / th);
                val = aliased ? (s >= 0.5f ? 1.0f : 0.0f) : s;
                rn = 1.0f - s;
            }
            if (val <= 0.0f) continue;
            if (noise && val < 1.0f) {
                // (its grain 1/300 inch, as a texture pixel at 100 %)
                const auto gx = static_cast<std::int32_t>(std::floor((x + 0.5) * 300.0 / dpi_));
                const auto gy = static_cast<std::int32_t>(std::floor((y + 0.5) * 300.0 / dpi_));
                const float n = hash2(gx, gy, kNoiseSalt);
                const float hard = n < val ? 1.0f : 0.0f;
                val += (hard - val) * 0.7f;
            }
            if (tex_tip) val = mask_combine(b.texture.mode, val, texture_at(x, y), d.depth);
            if (wet) val *= 0.5f + 0.5f * smoothstep(0.5f, 1.0f, rn);
            out[yy * w + xx] = val * d.alpha;
        }
    }
}

float BrushContext::stroke_mask(float c, std::optional<float> dual, int x, int y) const {
    float m = c;
    if (dual) m = mask_combine(settings_.dual.mode, m, *dual, 1.0f);
    if (texture_ != nullptr && !settings_.texture.each_tip) {
        m = mask_combine(settings_.texture.mode, m, texture_at(x, y), static_cast<float>(std::clamp(settings_.texture.depth, 0.0, 1.0)));
    }
    return m;
}

// --- the sparse buffer ---------------------------------------------------------------------------------------------------

const CoverageMap::Tile* CoverageMap::find(int x, int y, int& index) const {
    const int tx = floor_div(x, kTile), ty = floor_div(y, kTile);
    const auto it = tiles_.find(key(tx, ty));
    if (it == tiles_.end()) return nullptr;
    index = (y - ty * kTile) * kTile + (x - tx * kTile);
    return &it->second;
}

float CoverageMap::get(int x, int y) const {
    int i = 0;
    const Tile* t = find(x, y, i);
    return t == nullptr ? 0.0f : t->cov[static_cast<std::size_t>(i)];
}

std::array<float, 3> CoverageMap::colour_at(int x, int y) const {
    int i = 0;
    const Tile* t = find(x, y, i);
    if (t == nullptr || t->col.empty()) return {0.0f, 0.0f, 0.0f};
    const float c = t->cov[static_cast<std::size_t>(i)];
    if (c <= 0.0f) return {0.0f, 0.0f, 0.0f};
    const float* p = &t->col[static_cast<std::size_t>(i) * 3];
    return {p[0] / c, p[1] / c, p[2] / c};
}

std::vector<Box> CoverageMap::tile_boxes() const {
    std::vector<Box> out;
    for (const auto& entry : tiles_) {
        const Tile& tile = entry.second;
        out.push_back(Box{tile.tx * kTile, tile.ty * kTile, (tile.tx + 1) * kTile, (tile.ty + 1) * kTile});
    }
    std::sort(out.begin(), out.end(), [](const Box& a, const Box& b) { return a.y0 != b.y0 ? a.y0 < b.y0 : a.x0 < b.x0; });
    return out;
}

void CoverageMap::accumulate(const Box& rect, const std::vector<float>& vals, float ceiling, bool use_max, const std::array<float, 4>* colour) {
    if (blank(rect)) return;
    const auto w = static_cast<std::size_t>(rect.width());
    const bool want_col = colour_ && colour != nullptr;
    const int t0x = floor_div(rect.x0, kTile), t0y = floor_div(rect.y0, kTile);
    const int t1x = floor_div(rect.x1 - 1, kTile), t1y = floor_div(rect.y1 - 1, kTile);
    for (int ty = t0y; ty <= t1y; ++ty) {
        for (int tx = t0x; tx <= t1x; ++tx) {
            const Box tr = meet(Box{tx * kTile, ty * kTile, (tx + 1) * kTile, (ty + 1) * kTile}, rect);
            if (blank(tr)) continue;
            bool any = false;  // (a tile the dab does not touch is not made)
            for (int y = tr.y0; y < tr.y1 && !any; ++y) {
                const std::size_t row = std::size_t(y - rect.y0) * w;
                for (int x = tr.x0; x < tr.x1; ++x) {
                    if (vals[row + std::size_t(x - rect.x0)] > 0.0f) {
                        any = true;
                        break;
                    }
                }
            }
            if (!any) continue;
            Tile& tile = tiles_[key(tx, ty)];
            if (tile.cov.empty()) {
                tile.tx = tx;
                tile.ty = ty;
                tile.cov.assign(std::size_t(kTile) * kTile, 0.0f);
            }
            if (want_col && tile.col.empty()) tile.col.assign(std::size_t(kTile) * kTile * 3, 0.0f);
            for (int y = tr.y0; y < tr.y1; ++y) {
                const std::size_t row = std::size_t(y - rect.y0) * w;
                const std::size_t trow = std::size_t((y - ty * kTile) * kTile);
                for (int x = tr.x0; x < tr.x1; ++x) {
                    const float v = vals[row + std::size_t(x - rect.x0)];
                    if (v <= 0.0f) continue;
                    const std::size_t ti = trow + std::size_t(x - tx * kTile);
                    const float c = tile.cov[ti];
                    const float nv = use_max ? std::max(c, v * ceiling) : (ceiling > c ? c + v * (ceiling - c) : c);
                    if (nv <= c) continue;
                    if (want_col) {
                        const float k = c < 1.0f ? (nv - c) / (1.0f - c) : 0.0f;
                        float* p = &tile.col[ti * 3];
                        for (int i = 0; i < 3; ++i) p[i] = (*colour)[static_cast<std::size_t>(i)] * k + p[i] * (1.0f - k);
                    }
                    tile.cov[ti] = nv;
                }
            }
        }
    }
}

float LineCover::at(int x, int y) const {
    const float c = cov.get(x, y);
    if (c <= 0.0f) return 0.0f;
    const float m = ctx->stroke_mask(c, dual ? std::optional<float>(dual->get(x, y)) : std::nullopt, x, y);
    return std::min(m * opacity, 1.0f);
}

void LineCover::fill(const Box& area, std::vector<float>& out) const {
    const auto w = static_cast<std::size_t>(std::max(area.width(), 0));
    out.assign(w * static_cast<std::size_t>(std::max(area.height(), 0)), 0.0f);
    constexpr int kT = CoverageMap::kTile;
    for (const auto& entry : cov.tiles()) {
        const CoverageMap::Tile& tile = entry.second;
        const Box tr = meet(Box{tile.tx * kT, tile.ty * kT, (tile.tx + 1) * kT, (tile.ty + 1) * kT}, area);
        if (blank(tr)) continue;
        for (int y = tr.y0; y < tr.y1; ++y) {
            for (int x = tr.x0; x < tr.x1; ++x) {
                const float c = tile.cov[std::size_t((y - tile.ty * kT) * kT + (x - tile.tx * kT))];
                if (c <= 0.0f) continue;
                const float m = ctx->stroke_mask(c, dual ? std::optional<float>(dual->get(x, y)) : std::nullopt, x, y);
                out[std::size_t(y - area.y0) * w + std::size_t(x - area.x0)] = std::min(m * opacity, 1.0f);
            }
        }
    }
}

void LineCover::fill_colours(const Box& area, std::vector<std::array<float, 3>>& out) const {
    const auto w = static_cast<std::size_t>(std::max(area.width(), 0));
    out.assign(w * static_cast<std::size_t>(std::max(area.height(), 0)), {0.0f, 0.0f, 0.0f});
    constexpr int kT = CoverageMap::kTile;
    for (const auto& entry : cov.tiles()) {
        const CoverageMap::Tile& tile = entry.second;
        if (tile.col.empty()) continue;
        const Box tr = meet(Box{tile.tx * kT, tile.ty * kT, (tile.tx + 1) * kT, (tile.ty + 1) * kT}, area);
        if (blank(tr)) continue;
        for (int y = tr.y0; y < tr.y1; ++y) {
            for (int x = tr.x0; x < tr.x1; ++x) {
                const std::size_t ti = std::size_t((y - tile.ty * kT) * kT + (x - tile.tx * kT));
                const float c = tile.cov[ti];
                if (c <= 0.0f) continue;
                out[std::size_t(y - area.y0) * w + std::size_t(x - area.x0)] = {tile.col[ti * 3] / c, tile.col[ti * 3 + 1] / c, tile.col[ti * 3 + 2] / c};
            }
        }
    }
}

std::shared_ptr<const LineCover> lay(std::shared_ptr<const BrushContext> ctx, const std::vector<Dab>& dabs, const std::vector<Dab>& dual,
                                     const Box& within, bool coloured) {
    if (dabs.size() > kMaxDabs || dual.size() > kMaxDabs) {
        throw core::Error("too_large", "the line has more than " + std::to_string(kMaxDabs) + " dabs");
    }
    auto out = std::make_shared<LineCover>();
    out->cov = CoverageMap(coloured);
    out->opacity = static_cast<float>(std::clamp(ctx->settings().opacity, 0.0, 1.0));
    const core::DabSettings& b = ctx->settings();
    std::vector<float> scratch;
    Box reached;
    for (const Dab& d : dabs) {
        const Box full = ctx->dab_rect(d, false);
        const Box rect = meet(full, within);
        if (blank(rect)) continue;
        ctx->rasterize(d, false, rect, scratch);
        const std::array<float, 4>* col = coloured ? &d.color : nullptr;
        out->cov.accumulate(rect, scratch, d.opacity, b.wet_edges, col);
        reached = join(reached, rect);
    }
    if (b.dual.enabled) {
        out->dual.emplace(false);
        for (const Dab& d : dual) {
            const Box rect = meet(ctx->dab_rect(d, true), within);
            if (blank(rect)) continue;
            ctx->rasterize(d, true, rect, scratch);
            out->dual->accumulate(rect, scratch, 1.0f, false, nullptr);
        }
    }
    out->box = reached;
    out->ctx = std::move(ctx);
    return out;
}

}  // namespace genko::render::dabs
