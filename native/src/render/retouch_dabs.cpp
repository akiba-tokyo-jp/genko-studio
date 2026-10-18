#include "render/retouch_dabs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

// PhotoCraft v0.2.0 crates/paint/src/lib.rs (lines 43-150: StrokePoint, Stroke, Dab, dabs, dab_coverage),
// crates/paint/src/dynamics.rs (lines 125-215: the path walker's spacing), crates/paint/src/retouch.rs (lines 1-224:
// Footprint, stroke_coverage, over_native, apply_coverage) and crates/engine/src/presets/clone_source.rs (lines 103-240:
// Mapping, transform_matrix, resample), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::retouch {

using image_ops::Pixels;

namespace {

Box meet(const Box& a, const Box& b) {
    const Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return out.x1 > out.x0 && out.y1 > out.y0 ? out : Box{};
}

Box join(const Box& a, const Box& b) {
    if (a.width() <= 0 || a.height() <= 0) return b;
    return Box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

StrokePoint lerp(const StrokePoint& a, const StrokePoint& b, double f) {
    return StrokePoint{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.pressure + (b.pressure - a.pressure) * static_cast<float>(f)};
}

}  // namespace

std::vector<Dab> dabs(const Brush& brush, const std::vector<StrokePoint>& points) {
    std::vector<Dab> out;
    if (points.empty()) return out;
    const auto diameter = [&](const StrokePoint& p) {
        return brush.size * (brush.pressure_size ? std::clamp(p.pressure, 0.0f, 1.0f) : 1.0f);
    };
    const auto step_len = [&](const StrokePoint& p) {
        return std::max(static_cast<double>(std::max(diameter(p), 1.0f) * std::max(brush.spacing, 0.01f)), 0.5);
    };
    const auto emit = [&](const StrokePoint& p) {
        Dab d;
        d.x = p.x;
        d.y = p.y;
        d.radius = std::max(diameter(p) / 2.0f, 0.5f);
        d.alpha = std::clamp(brush.flow, 0.0f, 1.0f);
        d.opacity = 1.0f;
        d.index = out.size();
        out.push_back(d);
    };
    // (the path walker: the first point's dab once the stroke goes on, then one every step along it)
    StrokePoint last = points.front();
    bool pending = true;
    double next_at = step_len(last);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const StrokePoint& p = points[i];
        const double len = std::hypot(p.x - last.x, p.y - last.y);
        if (pending) {
            emit(points.front());
            pending = false;
        }
        if (len > 1e-9) {
            while (next_at <= len + 1e-9) {
                const StrokePoint q = lerp(last, p, std::min(next_at / len, 1.0));
                emit(q);
                next_at += std::max(step_len(q), 0.25);
            }
            next_at -= len;
        }
        last = p;
    }
    if (pending) emit(points.front());
    return out;
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

Box dab_rect(const Dab& dab) {
    const int rr = static_cast<int>(std::ceil(dab.radius)) + 1;
    const int cx = static_cast<int>(std::floor(dab.x)), cy = static_cast<int>(std::floor(dab.y));
    return Box{cx - rr, cy - rr, cx + rr + 1, cy + rr + 1};
}

Footprint footprint(const Dab& dab, float hardness) {
    Footprint fp;
    fp.dab = dab;
    fp.rect = dab_rect(dab);
    const auto cx = static_cast<float>(dab.x), cy = static_cast<float>(dab.y);
    fp.cov.reserve(std::size_t(fp.rect.width()) * std::size_t(fp.rect.height()));
    for (int y = fp.rect.y0; y < fp.rect.y1; ++y) {
        for (int x = fp.rect.x0; x < fp.rect.x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            fp.cov.push_back(dab_coverage(std::sqrt(dx * dx + dy * dy), dab.radius, hardness) * dab.alpha);
        }
    }
    return fp;
}

Coverage stroke_coverage(const Brush& brush, const std::vector<StrokePoint>& points) {
    Coverage out;
    const std::vector<Dab> all = dabs(brush, points);
    if (all.empty()) return out;
    for (const Dab& d : all) out.box = join(out.box, dab_rect(d));
    const auto w = std::size_t(out.box.width());
    out.cov.assign(w * std::size_t(out.box.height()), 0.0f);
    for (const Dab& d : all) {
        const Footprint fp = footprint(d, brush.hardness);
        const float ceil = d.opacity;
        for (int y = fp.rect.y0; y < fp.rect.y1; ++y) {
            for (int x = fp.rect.x0; x < fp.rect.x1; ++x) {
                const float v = fp.at(x, y);
                if (v <= 0.0f) continue;
                float& c = out.cov[std::size_t(y - out.box.y0) * w + std::size_t(x - out.box.x0)];
                if (ceil > c) c = c + v * (ceil - c);  // (the flow builds up toward the ceiling)
            }
        }
    }
    return out;
}

Coverage padded(const Coverage& coverage, int margin) {
    if (coverage.empty()) return coverage;
    Coverage out;
    out.box = Box{coverage.box.x0 - margin, coverage.box.y0 - margin, coverage.box.x1 + margin, coverage.box.y1 + margin};
    const auto w = std::size_t(out.box.width()), cw = std::size_t(coverage.box.width());
    out.cov.assign(w * std::size_t(out.box.height()), 0.0f);
    for (std::size_t i = 0; i < coverage.cov.size(); ++i) {
        const std::size_t x = i % cw + std::size_t(margin), y = i / cw + std::size_t(margin);
        out.cov[y * w + x] = coverage.cov[i];
    }
    return out;
}

void over(float* dst, const float* src, unsigned colours, float k, bool lock_alpha) {
    const float da = dst[colours], sa = src[colours] * k;
    const float oa = sa + da * (1.0f - sa);
    if (oa > 0.0f) {
        for (unsigned c = 0; c < colours; ++c) dst[c] = (src[c] * sa + dst[c] * da * (1.0f - sa)) / oa;
    }
    if (!lock_alpha) dst[colours] = oa;
}

Box apply_coverage(Pixels& target, const Box& box, const Coverage& coverage, float opacity, const std::vector<float>* selection, bool lock_alpha,
                   const Pixels& paint, std::string_view blend) {
    if (coverage.empty()) return {};
    const Box r = meet(coverage.box, box);
    if (r.width() <= 0) return {};
    const unsigned n = target.colours();
    const auto bw = std::size_t(box.width());
    const bool normal = blend.empty() || blend == "normal";
    // (another blend composites the colours: an ink layer's as its inks mean them, back by the page's rules)
    std::optional<image_ops::Held> held;
    std::vector<image_ops::Rgba> under, laid;
    if (!normal) {
        held.emplace(std::uint64_t(target.size()) * sizeof(image_ops::Rgba) * 2);
        under = image_ops::rgba_of(target);
        laid = image_ops::rgba_of(paint);
    }
    Box changed;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const std::size_t i = std::size_t(y - box.y0) * bw + std::size_t(x - box.x0);
            const float k = std::min(coverage.at(x, y) * opacity * (selection != nullptr ? (*selection)[i] : 1.0f), 1.0f);
            if (!(k > 0.0f)) continue;
            float* px = target.at(i);
            if (lock_alpha && px[n] <= 0.0f) continue;
            if (normal) {
                over(px, paint.at(i), n, k, lock_alpha);
            } else {
                image_ops::Rgba o = image_ops::composite(blend, under[i], laid[i], k);
                if (lock_alpha) o[3] = under[i][3];
                if (target.kind() == Pixels::Kind::Inks) {
                    under[i] = o;
                } else {
                    std::copy(o.begin(), o.end(), px);
                }
            }
            changed = join(changed, Box{x, y, x + 1, y + 1});
        }
    }
    if (!normal && target.kind() == Pixels::Kind::Inks && changed.width() > 0) image_ops::from_rgba(target, under);
    return changed;
}

// --- the clone source --------------------------------------------------------------------------------------------------

bool CloneMapping::is_translation() const {
    return std::fabs(m[0] - 1.0) < 1e-12 && std::fabs(m[1]) < 1e-12 && std::fabs(m[2]) < 1e-12 && std::fabs(m[3] - 1.0) < 1e-12;
}

std::pair<int, int> CloneMapping::offset() const {
    return {static_cast<int>(std::lround(source.first - anchor.first)), static_cast<int>(std::lround(source.second - anchor.second))};
}

std::pair<double, double> CloneMapping::map(double x, double y) const {
    const double dx = x - anchor.first, dy = y - anchor.second;
    return {source.first + m[0] * dx + m[1] * dy, source.second + m[2] * dx + m[3] * dy};
}

Box CloneMapping::source_box(const Box& dst) const {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [x, y] : {std::pair{dst.x0, dst.y0}, std::pair{dst.x1, dst.y0}, std::pair{dst.x0, dst.y1}, std::pair{dst.x1, dst.y1}}) {
        const auto [sx, sy] = map(x, y);
        x0 = std::min(x0, sx);
        y0 = std::min(y0, sy);
        x1 = std::max(x1, sx);
        y1 = std::max(y1, sy);
    }
    const auto held = [](double v) { return static_cast<int>(std::clamp(v, -1e9, 1e9)); };
    return Box{held(std::floor(x0)) - 2, held(std::floor(y0)) - 2, held(std::ceil(x1)) + 2, held(std::ceil(y1)) + 2};
}

std::array<double, 4> clone_matrix(double width_pct, double height_pct, double rotation_deg, bool flip_h, bool flip_v) {
    const double sx = std::max(width_pct / 100.0, 1e-3) * (flip_h ? -1.0 : 1.0);
    const double sy = std::max(height_pct / 100.0, 1e-3) * (flip_v ? -1.0 : 1.0);
    // forward (source → destination): scaled, then turned — counter-clockwise on the page is clockwise in y-down maths
    const double t = -rotation_deg * std::numbers::pi / 180.0;
    const double s = std::sin(t), c = std::cos(t);
    const std::array<double, 4> f{c * sx, -s * sy, s * sx, c * sy};
    const double det = f[0] * f[3] - f[1] * f[2];
    return {f[3] / det, -f[1] / det, -f[2] / det, f[0] / det};
}

Pixels resample(const Pixels& src, const Box& from, const Box& dst, const CloneMapping& map) {
    const unsigned n = src.all(), a = src.colours();
    Pixels out(src.kind(), static_cast<std::uint32_t>(std::max(dst.width(), 0)), static_cast<std::uint32_t>(std::max(dst.height(), 0)),
               src.colours());
    out.floating = src.floating;
    out.ink = src.ink;
    const auto fw = std::size_t(from.width());
    // (a source pixel premultiplied by its alpha; past the source, clear)
    const auto fetch = [&](int x, int y, float* buf) {
        if (x < from.x0 || y < from.y0 || x >= from.x1 || y >= from.y1) {
            std::fill(buf, buf + n, 0.0f);
            return;
        }
        const float* v = src.at(std::size_t(y - from.y0) * fw + std::size_t(x - from.x0));
        for (unsigned c = 0; c < n; ++c) buf[c] = c == a ? v[c] : v[c] * v[a];
    };
    std::vector<float> q(4 * std::size_t(n));
    const auto w = std::size_t(dst.width());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int x = dst.x0 + static_cast<int>(i % w), y = dst.y0 + static_cast<int>(i / w);
        const auto [sx, sy] = map.map(x + 0.5, y + 0.5);
        const double fx = sx - 0.5, fy = sy - 0.5;
        const double flx = std::floor(fx), fly = std::floor(fy);
        if (!(std::fabs(flx) < 1e9 && std::fabs(fly) < 1e9)) continue;  // (far past anything: clear)
        const int x0 = static_cast<int>(flx), y0 = static_cast<int>(fly);
        const auto tx = static_cast<float>(fx - flx), ty = static_cast<float>(fy - fly);
        fetch(x0, y0, q.data());
        fetch(x0 + 1, y0, q.data() + n);
        fetch(x0, y0 + 1, q.data() + 2 * n);
        fetch(x0 + 1, y0 + 1, q.data() + 3 * n);
        float* px = out.at(i);
        for (unsigned c = 0; c < n; ++c) {
            const float top = q[c] + (q[n + c] - q[c]) * tx;
            const float bot = q[2 * n + c] + (q[3 * n + c] - q[2 * n + c]) * tx;
            px[c] = top + (bot - top) * ty;
        }
        const float av = px[a];
        for (unsigned c = 0; c < a; ++c) px[c] = av > 1e-6f ? px[c] / av : 0.0f;
    }
    return out;
}

}  // namespace genko::render::retouch
