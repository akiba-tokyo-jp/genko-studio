#include "render/liquify_field.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

// PhotoCraft v0.2.0 crates/algo/src/liquify.rs (1-455 and its tests from 678), MIT: native/third_party/photocraft/
// NOTICE.md. The document's pixels are the page's mm here; the brush's least radius and step half a working pixel.

namespace genko::render::liquify {

namespace {

constexpr std::array<std::string_view, 14> kTools{"forward_warp", "reconstruct", "smooth", "twirl_cw", "twirl_ccw", "pucker", "bloat",
                                                  "push_left", "freeze", "thaw", "reconstruct_all", "freeze_all", "thaw_all", "invert_freeze"};

std::array<double, 4> catmull_rom(double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2};
}

// The displacement at grid place (gx, gy) of a w × h grid, bilinear; 0 outside it.
std::array<double, 2> bilinear(const std::vector<std::array<float, 2>>& d, std::size_t w, std::size_t h, double gx, double gy) {
    if (!(gx > -1.0 && gy > -1.0 && gx < static_cast<double>(w) && gy < static_cast<double>(h))) return {0.0, 0.0};
    gx = std::clamp(gx, 0.0, static_cast<double>(w - 1));
    gy = std::clamp(gy, 0.0, static_cast<double>(h - 1));
    const std::size_t i = std::min(static_cast<std::size_t>(gx), w >= 2 ? w - 2 : 0);
    const std::size_t j = std::min(static_cast<std::size_t>(gy), h >= 2 ? h - 2 : 0);
    const double fx = gx - static_cast<double>(i), fy = gy - static_cast<double>(j);
    const std::size_t i1 = std::min(i + 1, w - 1), j1 = std::min(j + 1, h - 1);
    const auto& a = d[j * w + i];
    const auto& b = d[j * w + i1];
    const auto& c = d[j1 * w + i];
    const auto& e = d[j1 * w + i1];
    std::array<double, 2> out{};
    for (std::size_t k = 0; k < 2; ++k) {
        out[k] = (static_cast<double>(a[k]) * (1.0 - fx) + static_cast<double>(b[k]) * fx) * (1.0 - fy) +
                 (static_cast<double>(c[k]) * (1.0 - fx) + static_cast<double>(e[k]) * fx) * fy;
    }
    return out;
}

}  // namespace

std::optional<Tool> tool_from(std::string_view text) {
    for (std::size_t i = 0; i < kTools.size(); ++i)
        if (kTools[i] == text) return static_cast<Tool>(i);
    return std::nullopt;
}

std::string_view to_string(Tool tool) { return kTools[static_cast<std::size_t>(tool)]; }

bool is_global(Tool t) { return t == Tool::ReconstructAll || t == Tool::FreezeAll || t == Tool::ThawAll || t == Tool::InvertFreeze; }

bool is_stationary(Tool t) { return t != Tool::ForwardWarp && t != Tool::PushLeft; }

double auto_cell(double width_mm, double height_mm, double unit) {
    return (width_mm / unit) * (height_mm / unit) <= 4'200'000.0 ? 2.0 * unit : 4.0 * unit;
}

Field::Field(double x0, double y0, double width, double height, double cell, double unit)
    : x0_(x0), y0_(y0), cell_(std::clamp(cell, unit, 64.0 * unit)), unit_(unit) {
    w_ = static_cast<std::size_t>(std::ceil(std::max(width, 0.0) / cell_)) + 2;
    h_ = static_cast<std::size_t>(std::ceil(std::max(height, 0.0) / cell_)) + 2;
    d_.assign(w_ * h_, {0.0f, 0.0f});
    freeze_.assign(w_ * h_, 0.0f);
}

std::array<double, 2> Field::node_pos(std::size_t i, std::size_t j) const {
    // (node 0 on the first working pixel's centre)
    return {x0_ + 0.5 * unit_ + static_cast<double>(i) * cell_, y0_ + 0.5 * unit_ + static_cast<double>(j) * cell_};
}

std::array<double, 2> Field::to_grid(double x, double y) const { return {(x - x0_ - 0.5 * unit_) / cell_, (y - y0_ - 0.5 * unit_) / cell_}; }

bool Field::is_identity() const {
    return std::all_of(d_.begin(), d_.end(), [](const std::array<float, 2>& v) { return v[0] == 0.0f && v[1] == 0.0f; });
}

double Field::max_displacement() const {
    double most = 0.0;
    for (const auto& v : d_) most = std::max(most, std::hypot(static_cast<double>(v[0]), static_cast<double>(v[1])));
    return most;
}

std::array<double, 2> Field::sample(double x, double y) const {
    const auto [gx, gy] = to_grid(x, y);
    return bilinear(d_, w_, h_, gx, gy);
}

std::array<double, 2> Field::sample_cubic(double x, double y) const {
    const auto [gx, gy] = to_grid(x, y);
    if (!std::isfinite(gx) || !std::isfinite(gy)) return {0.0, 0.0};
    const double fx = std::floor(gx), fy = std::floor(gy);
    const auto wx = catmull_rom(gx - fx), wy = catmull_rom(gy - fy);
    const long ix = static_cast<long>(std::clamp(fx, -4.0, static_cast<double>(w_) + 4.0));
    const long iy = static_cast<long>(std::clamp(fy, -4.0, static_cast<double>(h_) + 4.0));
    std::array<double, 2> acc{0.0, 0.0};
    for (std::size_t jj = 0; jj < 4; ++jj) {
        const std::size_t row = static_cast<std::size_t>(std::clamp<long>(iy - 1 + static_cast<long>(jj), 0, static_cast<long>(h_) - 1));
        for (std::size_t ii = 0; ii < 4; ++ii) {
            const std::size_t xx = static_cast<std::size_t>(std::clamp<long>(ix - 1 + static_cast<long>(ii), 0, static_cast<long>(w_) - 1));
            const double k = wx[ii] * wy[jj];
            acc[0] += static_cast<double>(d_[row * w_ + xx][0]) * k;
            acc[1] += static_cast<double>(d_[row * w_ + xx][1]) * k;
        }
    }
    return acc;
}

double Field::freeze_at(double x, double y) const {
    auto [gx, gy] = to_grid(x, y);
    gx = std::clamp(gx, 0.0, static_cast<double>(w_ - 1));
    gy = std::clamp(gy, 0.0, static_cast<double>(h_ - 1));
    const std::size_t i = std::min(static_cast<std::size_t>(gx), w_ - 2), j = std::min(static_cast<std::size_t>(gy), h_ - 2);
    const double fx = gx - static_cast<double>(i), fy = gy - static_cast<double>(j);
    const auto at = [&](std::size_t a, std::size_t b) { return static_cast<double>(freeze_[b * w_ + a]); };
    return (at(i, j) * (1.0 - fx) + at(i + 1, j) * fx) * (1.0 - fy) + (at(i, j + 1) * (1.0 - fx) + at(i + 1, j + 1) * fx) * fy;
}

std::optional<std::array<double, 4>> Field::moved_box() const {
    std::size_t i0 = w_, j0 = h_, i1 = 0, j1 = 0;
    for (std::size_t j = 0; j < h_; ++j) {
        for (std::size_t i = 0; i < w_; ++i) {
            const auto& v = d_[j * w_ + i];
            if (v[0] == 0.0f && v[1] == 0.0f) continue;
            i0 = std::min(i0, i);
            j0 = std::min(j0, j);
            i1 = std::max(i1, i);
            j1 = std::max(j1, j);
        }
    }
    if (i0 > i1 || j0 > j1) return std::nullopt;
    // (the cubic reaches two nodes past a displaced one)
    const auto a = node_pos(i0, j0), b = node_pos(i1, j1);
    return std::array<double, 4>{a[0] - 2.0 * cell_, a[1] - 2.0 * cell_, b[0] + 2.0 * cell_, b[1] + 2.0 * cell_};
}

std::array<double, 2> Field::forward(double x, double y) const {
    double qx = x, qy = y;
    for (int k = 0; k < 24; ++k) {
        const auto d = sample_cubic(qx, qy);
        const double nx = x - d[0], ny = y - d[1];
        const bool done = std::fabs(nx - qx) < 1e-7 && std::fabs(ny - qy) < 1e-7;
        qx = nx;
        qy = ny;
        if (done) break;
    }
    return {qx, qy};
}

void Field::apply(const Stroke& s) {
    switch (s.tool) {
        case Tool::ReconstructAll: {
            const float k = static_cast<float>(std::clamp(s.amount.value_or(100.0) / 100.0, 0.0, 1.0));
            for (std::size_t i = 0; i < d_.size(); ++i) {
                const float m = 1.0f - k * (1.0f - freeze_[i]);
                d_[i][0] *= m;
                d_[i][1] *= m;
            }
            return;
        }
        case Tool::FreezeAll: std::fill(freeze_.begin(), freeze_.end(), 1.0f); return;
        case Tool::ThawAll: std::fill(freeze_.begin(), freeze_.end(), 0.0f); return;
        case Tool::InvertFreeze:
            for (float& f : freeze_) f = 1.0f - f;
            return;
        default: break;
    }
    if (s.points.empty()) return;
    stroke_begin(s, s.points.front());
    for (std::size_t i = 0; i + 1 < s.points.size(); ++i) stroke_segment(s, s.points[i], s.points[i + 1]);
}

void Field::stroke_begin(const Stroke& s, const std::array<double, 3>& p) {
    if (is_stationary(s.tool) && !is_global(s.tool)) dab(s, {p[0], p[1]}, {0.0, 0.0}, std::clamp(p[2], 0.0, 1.0));
}

void Field::stroke_segment(const Stroke& s, const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double pa = std::clamp(a[2], 0.0, 1.0), pb = std::clamp(b[2], 0.0, 1.0);
    const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
    if (len == 0.0) {
        if (is_stationary(s.tool)) dab(s, {b[0], b[1]}, {0.0, 0.0}, pb);
        return;
    }
    const double r = std::max(s.size / 2.0, 0.5 * unit_);
    const double spacing = std::max(r * 0.2, 0.5 * unit_);
    const std::size_t steps = static_cast<std::size_t>(std::max(std::ceil(len / spacing), 1.0));
    for (std::size_t k = 1; k <= steps; ++k) {
        const double t0 = static_cast<double>(k - 1) / static_cast<double>(steps);
        const double t1 = static_cast<double>(k) / static_cast<double>(steps);
        const std::array<double, 2> c{a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1};
        const std::array<double, 2> delta{(b[0] - a[0]) * (t1 - t0), (b[1] - a[1]) * (t1 - t0)};
        const double pr = pa + (pb - pa) * t1;
        // (Forward Warp centres the dab where the brush was: it drags what is under it)
        const std::array<double, 2> centre = is_stationary(s.tool) ? c : std::array<double, 2>{c[0] - delta[0], c[1] - delta[1]};
        dab(s, centre, delta, pr);
    }
}

void Field::dab(const Stroke& s, std::array<double, 2> c, std::array<double, 2> delta, double point_pressure) {
    const double r = std::max(s.size / 2.0, 0.5 * unit_);
    const double strength = std::clamp(s.pressure / 100.0, 0.0, 1.0) * point_pressure;
    const double rate = std::clamp(s.rate / 100.0, 0.0, 1.0);
    const double density = std::clamp(s.density / 100.0, 0.0, 1.0);
    if (strength <= 0.0) return;
    const auto [gx0, gy0] = to_grid(c[0] - r, c[1] - r);
    const auto [gx1, gy1] = to_grid(c[0] + r, c[1] + r);
    const std::size_t i0 = static_cast<std::size_t>(std::max(std::floor(gx0), 0.0));
    const std::size_t j0 = static_cast<std::size_t>(std::max(std::floor(gy0), 0.0));
    const long i1 = std::min(static_cast<long>(std::max(std::ceil(gx1), -1.0)), static_cast<long>(w_) - 1);
    const long j1 = std::min(static_cast<long>(std::max(std::ceil(gy1), -1.0)), static_cast<long>(h_) - 1);
    if (i1 < static_cast<long>(i0) || j1 < static_cast<long>(j0)) return;
    const std::size_t ie = static_cast<std::size_t>(i1), je = static_cast<std::size_t>(j1);
    // (a snapshot of the nodes the dab changes and a margin: its sampling reads around them)
    const std::size_t m = static_cast<std::size_t>(std::ceil((std::max(std::fabs(delta[0]), std::fabs(delta[1])) + r * 0.2) / cell_)) + 2;
    const std::size_t si0 = i0 > m ? i0 - m : 0, sj0 = j0 > m ? j0 - m : 0;
    const std::size_t si1 = std::min(ie + m, w_ - 1), sj1 = std::min(je + m, h_ - 1);
    const std::size_t sw = si1 - si0 + 1, sh = sj1 - sj0 + 1;
    std::vector<std::array<float, 2>> snap;
    snap.reserve(sw * sh);
    for (std::size_t j = sj0; j <= sj1; ++j) snap.insert(snap.end(), d_.begin() + static_cast<std::ptrdiff_t>(j * w_ + si0), d_.begin() + static_cast<std::ptrdiff_t>(j * w_ + si1 + 1));
    const auto old = [&](double gx, double gy) {
        const double lx = gx - static_cast<double>(si0), ly = gy - static_cast<double>(sj0);
        if (lx >= 0.0 && ly >= 0.0 && lx <= static_cast<double>(sw - 1) && ly <= static_cast<double>(sh - 1)) return bilinear(snap, sw, sh, lx, ly);
        return bilinear(d_, w_, h_, gx, gy);  // (outside the snapshot nothing changed)
    };
    struct Write {
        std::size_t at;
        std::array<float, 2> d;
        float freeze;
    };
    std::vector<Write> writes;
    writes.reserve((ie - i0 + 1) * (je - j0 + 1));
    for (std::size_t j = j0; j <= je; ++j) {
        for (std::size_t i = i0; i <= ie; ++i) {
            const auto p = node_pos(i, j);
            const double dx = p[0] - c[0], dy = p[1] - c[1];
            const double t2 = (dx * dx + dy * dy) / (r * r);
            if (t2 >= 1.0) continue;
            const double soft = (1.0 - t2) * (1.0 - t2);
            const double fall = (1.0 - density) + density * soft;
            const std::size_t idx = j * w_ + i;
            const double k_raw = strength * fall;
            if (s.tool == Tool::Freeze || s.tool == Tool::Thaw) {
                const double f = freeze_[idx];
                const double v = s.tool == Tool::Freeze ? std::min(f + k_raw, 1.0) : std::max(f - k_raw, 0.0);
                writes.push_back({idx, d_[idx], static_cast<float>(v)});
                continue;
            }
            const double k = k_raw * (1.0 - static_cast<double>(freeze_[idx]));
            if (k <= 0.0) continue;
            const std::array<float, 2> cur = d_[idx];
            std::array<float, 2> now{};
            if (s.tool == Tool::Reconstruct) {
                const float a = static_cast<float>(1.0 - k * (0.1 + 0.4 * rate));
                now = {cur[0] * a, cur[1] * a};
            } else if (s.tool == Tool::Smooth) {
                const long gx = static_cast<long>(i - si0), gy = static_cast<long>(j - sj0);
                const auto at = [&](long x, long y) {
                    return snap[static_cast<std::size_t>(std::clamp<long>(y, 0, static_cast<long>(sh) - 1)) * sw +
                                static_cast<std::size_t>(std::clamp<long>(x, 0, static_cast<long>(sw) - 1))];
                };
                std::array<float, 2> mean{0.0f, 0.0f};
                constexpr std::array<std::array<long, 2>, 8> kAround{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
                for (const auto& [ox, oy] : kAround) {
                    const auto v = at(gx + ox, gy + oy);
                    mean[0] += v[0] / 8.0f;
                    mean[1] += v[1] / 8.0f;
                }
                const float a = static_cast<float>(k * (0.1 + 0.4 * rate));
                now = {cur[0] + (mean[0] - cur[0]) * a, cur[1] + (mean[1] - cur[1]) * a};
            } else {
                std::array<double, 2> q{};
                switch (s.tool) {
                    case Tool::ForwardWarp: q = {p[0] - k * delta[0], p[1] - k * delta[1]}; break;
                    // (left of the stroke's way, y down: (dx, dy) → (dy, −dx))
                    case Tool::PushLeft: q = {p[0] - k * delta[1], p[1] + k * delta[0]}; break;
                    case Tool::TwirlCw:
                    case Tool::TwirlCcw: {
                        const double th = k * rate * 0.12 * (s.tool == Tool::TwirlCw ? 1.0 : -1.0);
                        // (the picture turns by +th: read at the point turned by −th)
                        const double sn = std::sin(-th), cs = std::cos(-th);
                        q = {c[0] + dx * cs - dy * sn, c[1] + dx * sn + dy * cs};
                        break;
                    }
                    case Tool::Pucker: {
                        const double a = 1.0 + k * rate * 0.06;
                        q = {c[0] + dx * a, c[1] + dy * a};
                        break;
                    }
                    default: {  // (Bloat)
                        const double a = 1.0 - k * rate * 0.06;
                        q = {c[0] + dx * a, c[1] + dy * a};
                        break;
                    }
                }
                const double qx = (q[0] - p[0]) / cell_ + static_cast<double>(i), qy = (q[1] - p[1]) / cell_ + static_cast<double>(j);
                const auto dq = old(qx, qy);
                now = {static_cast<float>(q[0] + dq[0] - p[0]), static_cast<float>(q[1] + dq[1] - p[1])};
            }
            writes.push_back({idx, now, freeze_[idx]});
        }
    }
    for (const Write& w : writes) {
        d_[w.at] = w.d;
        freeze_[w.at] = w.freeze;
    }
}

}  // namespace genko::render::liquify
