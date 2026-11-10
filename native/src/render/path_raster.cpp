#include "render/path_raster.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// PhotoCraft v0.2.0 crates/vector/src/flatten.rs (1-132), raster.rs (1-419), stroke.rs (1-400), shapes.rs (1-153) and
// lib.rs (40-110), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::path_raster {

using core::vector::FillRule;
using core::vector::Knot;
using core::vector::LineCap;
using core::vector::LineJoin;
using core::vector::Path;
using core::vector::PathOp;
using core::vector::Subpath;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * std::numbers::pi;

// --- flattening ---------------------------------------------------------------------------------------------------

void push(Polyline& line, XY p, bool knot) {
    if (!line.pts.empty()) {
        const XY last = line.pts.back();
        if (std::fabs(last.first - p.first) < 1e-12 && std::fabs(last.second - p.second) < 1e-12) {
            if (knot && !line.knot.empty()) line.knot.back() = true;
            return;
        }
    }
    line.pts.push_back(p);
    line.knot.push_back(knot);
}

using Segment = std::array<core::vector::XY, 4>;

// A straight segment: both handles on the chord (within 1e-9 px) between its ends.
bool is_line(const Segment& seg) {
    const auto& [p0, c1, c2, p1] = seg;
    const double dx = p1.x - p0.x, dy = p1.y - p0.y;
    const double len2 = dx * dx + dy * dy;
    const auto on_chord = [&](const core::vector::XY& c) {
        const double vx = c.x - p0.x, vy = c.y - p0.y;
        if (len2 < 1e-24) return std::fabs(vx) < 1e-9 && std::fabs(vy) < 1e-9;
        const double t = (vx * dx + vy * dy) / len2;
        const double cross = std::fabs(vx * dy - vy * dx) / std::sqrt(len2);
        return cross < 1e-9 && t >= -1e-9 && t <= 1.0 + 1e-9;
    };
    return on_chord(c1) && on_chord(c2);
}

// Uniform pieces for a chord deviation of at most `tol` (max|B''| / 8n², max|B''| ≤ 6·max(|p0 − 2c1 + c2|, |c1 − 2c2 + p1|)).
std::size_t pieces(const Segment& seg, double tol) {
    const auto& [p0, c1, c2, p1] = seg;
    const double d1 = std::sqrt(std::pow(p0.x - 2.0 * c1.x + c2.x, 2) + std::pow(p0.y - 2.0 * c1.y + c2.y, 2));
    const double d2 = std::sqrt(std::pow(c1.x - 2.0 * c2.x + p1.x, 2) + std::pow(c1.y - 2.0 * c2.y + p1.y, 2));
    const double m = std::max(d1, d2);
    // (small curves: a finer tolerance, so their area stays right)
    const double size = std::hypot(c1.x - p0.x, c1.y - p0.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) + std::hypot(p1.x - c2.x, p1.y - c2.y);
    const double t = std::max(std::min(tol, size * 1e-3), 1e-4);
    const double n = std::ceil(std::sqrt(0.75 * m / t));
    if (!std::isfinite(n)) return 1;
    return static_cast<std::size_t>(std::clamp(n, 1.0, 65536.0));
}

XY eval(const Segment& seg, double t) {
    const auto& [p0, c1, c2, p1] = seg;
    const double mt = 1.0 - t;
    const double a = mt * mt * mt, b = 3.0 * mt * mt * t, c = 3.0 * mt * t * t, d = t * t * t;
    return {a * p0.x + b * c1.x + c * c2.x + d * p1.x, a * p0.y + b * c1.y + c * c2.y + d * p1.y};
}

// --- stroking ----------------------------------------------------------------------------------------------------

XY sub(XY a, XY b) { return {a.first - b.first, a.second - b.second}; }
XY add(XY a, XY b) { return {a.first + b.first, a.second + b.second}; }
XY mul(XY a, double k) { return {a.first * k, a.second * k}; }
double len(XY a) { return std::hypot(a.first, a.second); }
XY norm(XY a) {
    const double l = len(a);
    return l > 0.0 ? XY{a.first / l, a.second / l} : XY{0.0, 0.0};
}
XY perp(XY a) { return {a.second, -a.first}; }  // (the left normal, y down)
double cross(XY a, XY b) { return a.first * b.second - a.second * b.first; }
double dot(XY a, XY b) { return a.first * b.first + a.second * b.second; }

double signed_area(const std::vector<XY>& p) {
    const std::size_t n = p.size();
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += cross(p[i], p[(i + 1) % n]);
    return s * 0.5;
}

void emit(std::vector<std::vector<XY>>& out, std::vector<XY> poly) {
    const double a = signed_area(poly);
    if (std::fabs(a) < 1e-12) return;
    if (a < 0.0) std::reverse(poly.begin(), poly.end());
    out.push_back(std::move(poly));
}

// The angle step for chords within `tol` of a circle of radius r.
double arc_step(double r, double tol) {
    if (r <= tol) return kPi / 2;
    return std::clamp(2.0 * std::acos(std::clamp(1.0 - tol / r, -1.0, 1.0)), 0.01, kPi / 2);
}

// The vertex radius for chords of `step` radians that keeps each slice the sector's area (round caps and joins exact).
double area_radius(double r, double step) { return step < 1e-9 ? r : r * std::sqrt(step / std::sin(step)); }

std::vector<XY> circle(XY c, double r, double tol) {
    const std::size_t n = std::max<std::size_t>(static_cast<std::size_t>(std::ceil(kTau / arc_step(r, tol))), 8);
    const double rr = area_radius(r, kTau / static_cast<double>(n));
    std::vector<XY> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = kTau * static_cast<double>(i) / static_cast<double>(n);
        out.emplace_back(c.first + rr * std::cos(a), c.second + rr * std::sin(a));
    }
    return out;
}

void join_piece(XY v, XY d1, XY d2, double hw, LineJoin join, double miter_limit, double tol, std::vector<std::vector<XY>>& out) {
    const double c = cross(d1, d2);
    const double d = dot(d1, d2);
    if (std::fabs(c) < 1e-12 && d > 0.0) return;
    // (the outer side: a right turn — c > 0, y down — opens the left)
    const double s = c > 0.0 ? 1.0 : -1.0;
    const XY n1 = mul(perp(d1), hw * s);
    const XY n2 = mul(perp(d2), hw * s);
    const XY a = add(v, n1), b = add(v, n2);
    switch (join) {
        case LineJoin::Bevel:
            emit(out, {v, a, b});
            break;
        case LineJoin::Miter: {
            const double cos_half = std::sqrt(std::max((1.0 + d) * 0.5, 0.0));
            if (cos_half < 1e-9 || 1.0 / cos_half > miter_limit) {
                emit(out, {v, a, b});
            } else {
                const XY bis = norm(add(n1, n2));
                emit(out, {v, a, add(v, mul(bis, hw / cos_half)), b});
            }
            break;
        }
        case LineJoin::Round: {
            const double a0 = std::atan2(n1.second, n1.first);
            double delta = std::atan2(n2.second, n2.first) - a0;
            while (delta > kPi) delta -= kTau;
            while (delta < -kPi) delta += kTau;
            if (std::fabs(c) < 1e-12) delta = kPi * (cross(n1, d1) > 0.0 ? 1.0 : -1.0);  // (a U-turn: half a circle)
            const double a1 = a0 + delta;
            const std::size_t steps = std::max<std::size_t>(static_cast<std::size_t>(std::ceil(std::fabs(delta) / arc_step(hw, tol))), 1);
            const double rr = area_radius(hw, std::fabs(delta) / static_cast<double>(steps));
            std::vector<XY> poly{v, a};
            for (std::size_t k = 1; k < steps; ++k) {
                const double t = a0 + (a1 - a0) * static_cast<double>(k) / static_cast<double>(steps);
                poly.emplace_back(v.first + rr * std::cos(t), v.second + rr * std::sin(t));
            }
            poly.push_back(b);
            emit(out, std::move(poly));
            break;
        }
    }
}

void cap_piece(XY p, XY dir, double hw, LineCap cap, double tol, std::vector<std::vector<XY>>& out) {
    switch (cap) {
        case LineCap::Butt:
            break;
        case LineCap::Round:
            emit(out, circle(p, hw, tol));
            break;
        case LineCap::Square: {
            const XY n = mul(perp(dir), hw);
            const XY e = mul(dir, hw);
            emit(out, {add(p, n), add(add(p, n), e), add(sub(p, n), e), sub(p, n)});
            break;
        }
    }
}

void stroke_one(const Polyline& pl, const StrokeStyle& style, double tol, std::vector<std::vector<XY>>& out) {
    const double hw = style.width * 0.5;
    std::vector<XY> pts;
    std::vector<bool> knot;
    pts.reserve(pl.pts.size());
    for (std::size_t i = 0; i < pl.pts.size(); ++i) {  // (repeated points dropped)
        if (!pts.empty() && len(sub(pl.pts[i], pts.back())) < 1e-9) continue;
        pts.push_back(pl.pts[i]);
        knot.push_back(i < pl.knot.size() ? pl.knot[i] : true);
    }
    bool closed = pl.closed;
    if (closed && pts.size() > 1 && len(sub(pts.front(), pts.back())) < 1e-9) {
        pts.pop_back();
        knot.pop_back();
    }
    if (pts.size() < 3) closed = false;
    if (pts.empty()) return;
    if (pts.size() == 1) {  // (a dot: only round and square caps draw it)
        const XY c = pts[0];
        if (style.cap == LineCap::Round) emit(out, circle(c, hw, tol));
        if (style.cap == LineCap::Square) {
            emit(out, {{c.first - hw, c.second - hw}, {c.first + hw, c.second - hw}, {c.first + hw, c.second + hw}, {c.first - hw, c.second + hw}});
        }
        return;
    }
    const std::size_t n = pts.size();
    const std::size_t segs = closed ? n : n - 1;
    const auto dir = [&](std::size_t i) { return norm(sub(pts[(i + 1) % n], pts[i])); };
    const auto seg_len = [&](std::size_t i) { return len(sub(pts[(i + 1) % n], pts[i])); };
    // (a vertex goes on in one ribbon — no join piece — where a flattening point turns a little without folding back,
    // or a knot where the path is straight)
    const double cos_smooth = std::cos(25.0 * kPi / 180.0);
    const auto smooth = [&](std::size_t j) {
        if (j == 0 || (!closed && j == n - 1)) return false;
        const std::size_t i0 = (j + n - 1) % n, i1 = j;
        const double d = dot(dir(i0), dir(i1));
        if (knot[j]) return d > 1.0 - 1e-12;
        if (d < cos_smooth) return false;
        const double tan_half = std::sqrt(std::max((1.0 - d) / (1.0 + d), 0.0));
        return hw * tan_half < 0.5 * std::min(seg_len(i0), seg_len(i1));
    };
    std::vector<XY> left, right;
    for (std::size_t i = 0; i < segs; ++i) {
        const XY a = pts[i];
        const XY b = pts[(i + 1) % n];
        const XY nrm = mul(perp(dir(i)), hw);
        if (left.empty()) {
            left.push_back(add(a, nrm));
            right.push_back(sub(a, nrm));
        }
        const std::size_t j = (i + 1) % n;
        if (i + 1 < segs + (closed ? 0 : 1) && smooth(j)) {
            // (the two offset lines meet at v ± bisector · hw / cos(θ/2))
            const XY n2 = mul(perp(dir(j)), hw);
            const XY bis = norm(add(nrm, n2));
            const double cos_half = std::sqrt(std::max((1.0 + dot(dir(i), dir(j))) * 0.5, 1e-9));
            const XY off = mul(bis, hw / cos_half);
            left.push_back(add(b, off));
            right.push_back(sub(b, off));
            continue;
        }
        left.push_back(add(b, nrm));
        right.push_back(sub(b, nrm));
        std::vector<XY> poly = std::move(left);
        left.clear();
        poly.insert(poly.end(), right.rbegin(), right.rend());
        right.clear();
        emit(out, std::move(poly));
        if (closed || j != n - 1) {
            const XY prev = pts[i], v = pts[j], next = pts[(j + 1) % n];
            const XY d1 = norm(sub(v, prev)), d2 = norm(sub(next, v));
            // (flattening points mitre — the true offset of a smooth curve — unless round; bevels only at real corners)
            const LineJoin jn = style.join == LineJoin::Round ? LineJoin::Round : knot[j] ? style.join : LineJoin::Miter;
            join_piece(v, d1, d2, hw, jn, knot[j] ? style.miter_limit : std::numeric_limits<double>::infinity(), tol, out);
        }
    }
    if (!closed) {
        cap_piece(pts[0], mul(norm(sub(pts[1], pts[0])), -1.0), hw, style.cap, tol, out);
        cap_piece(pts[n - 1], norm(sub(pts[n - 1], pts[n - 2])), hw, style.cap, tol, out);
    }
}

// --- the rasterizer's accumulation -------------------------------------------------------------------------------

void add_full(std::vector<float>& acc, double h) { acc[0] += static_cast<float>(h); }

// Adds w × the area right of the segment (xa, ya) → (xb, yb) (ya < yb, within one pixel row) to the cells whose cell 0
// is column x0.
void draw(std::vector<float>& acc, int x0, double xa, double ya, double xb, double yb, double w) {
    const double width = static_cast<double>(acc.size() - 1);
    const double left = static_cast<double>(x0);
    double ua = xa - left, ub = xb - left;
    const double dy = yb - ya;
    if (dy <= 0.0) return;
    if (ua > ub) std::swap(ua, ub);
    const double dx = ub - ua;
    const auto h_of = [&](double u0, double u1) { return dx > 1e-12 ? dy * (u1 - u0) / dx : dy; };
    if (ua < 0.0) {  // (the part left of the box: full coverage of every column)
        const double cut = std::min(ub, 0.0);
        acc[0] += static_cast<float>(w * h_of(ua, cut));
        if (ub <= 0.0) return;
        ua = 0.0;
    }
    if (ua >= width) return;
    const double end = std::min(ub, width);
    if (dx <= 1e-12) {
        const double c = std::floor(ua);
        const double f = ua - c;
        const std::size_t ci = static_cast<std::size_t>(c);
        acc[ci] += static_cast<float>(w * dy * (1.0 - f));
        acc[ci + 1] += static_cast<float>(w * dy * f);
        return;
    }
    double u = ua;
    while (u < end) {
        const double c = std::floor(u);
        const double v = std::min(c + 1.0, end);
        const double h = h_of(u, v);
        const double m = 0.5 * (u + v) - c;
        const std::size_t ci = static_cast<std::size_t>(c);
        acc[ci] += static_cast<float>(w * h * (1.0 - m));
        acc[ci + 1] += static_cast<float>(w * h * m);
        if (v <= u) break;
        u = v;
    }
}

}  // namespace

// --- flattening ------------------------------------------------------------------------------------------------------

Polyline flatten_subpath(const Subpath& s, double tol) {
    Polyline out;
    out.closed = s.closed;
    if (!s.knots.empty()) push(out, {s.knots.front().anchor.x, s.knots.front().anchor.y}, true);
    for (const Segment& seg : s.segments()) {
        if (is_line(seg)) {
            push(out, {seg[3].x, seg[3].y}, true);
            continue;
        }
        const std::size_t n = pieces(seg, tol);
        for (std::size_t j = 1; j <= n; ++j) push(out, eval(seg, static_cast<double>(j) / static_cast<double>(n)), j == n);
    }
    if (s.closed && out.pts.size() > 1) {
        const XY a = out.pts.front(), b = out.pts.back();
        if (std::fabs(a.first - b.first) < 1e-12 && std::fabs(a.second - b.second) < 1e-12) {
            out.pts.pop_back();
            out.knot.pop_back();
            out.knot[0] = true;
        }
    }
    return out;
}

std::vector<Polyline> flatten_path(const Path& p, double tol) {
    std::vector<Polyline> out;
    out.reserve(p.subpaths.size());
    for (const Subpath& s : p.subpaths) out.push_back(flatten_subpath(s, tol));
    return out;
}

// --- the rasterizer --------------------------------------------------------------------------------------------------

struct Rasterizer::RowState {
    std::vector<std::size_t> active;
    std::vector<double> cuts;
    std::vector<BandEdge> band;
    std::vector<int> wind;
    std::vector<float> acc;  // (width + 1 cells: the last takes what spills over)
    RowState(std::size_t comps, std::size_t width) : wind(std::max<std::size_t>(comps, 1), 0), acc(width + 1, 0.0f) {}
};

void Rasterizer::add_component(const std::vector<std::vector<XY>>& polys, PathOp op, FillRule rule) {
    const auto comp = static_cast<std::uint32_t>(comps_.size());
    comps_.push_back(Component{op, rule});
    if (op != PathOp::Combine && comp != 0) union_only_ = false;
    for (const auto& poly : polys) {
        const std::size_t n = poly.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const XY a = poly[i], b = poly[(i + 1) % n];
            if (!(std::isfinite(a.first) && std::isfinite(a.second) && std::isfinite(b.first) && std::isfinite(b.second))) continue;
            if (!bounds_) {
                bounds_ = std::array<double, 4>{std::min(a.first, b.first), std::min(a.second, b.second), std::max(a.first, b.first),
                                                std::max(a.second, b.second)};
            } else {
                auto& bb = *bounds_;
                bb[0] = std::min(bb[0], std::min(a.first, b.first));
                bb[1] = std::min(bb[1], std::min(a.second, b.second));
                bb[2] = std::max(bb[2], std::max(a.first, b.first));
                bb[3] = std::max(bb[3], std::max(a.second, b.second));
            }
            if (a.second == b.second) continue;
            const bool down = a.second < b.second;
            const XY p = down ? a : b, q = down ? b : a;
            edges_.push_back(Edge{p.first, p.second, q.second, down ? 1 : -1, comp, (q.first - p.first) / (q.second - p.second)});
        }
    }
    std::stable_sort(edges_.begin(), edges_.end(), [](const Edge& x, const Edge& y) { return x.y0 < y.y0; });
}

std::optional<PixelBox> Rasterizer::pixel_bounds() const {
    if (inverted_ || !bounds_) return std::nullopt;
    const auto& b = *bounds_;
    PixelBox r{static_cast<int>(std::floor(b[0])), static_cast<int>(std::floor(b[1])), static_cast<int>(std::ceil(b[2])) + 1,
               static_cast<int>(std::ceil(b[3])) + 1};
    if (r.empty()) return std::nullopt;
    return r;
}

bool Rasterizer::comp_inside(std::size_t comp, int w) const {
    return comps_[comp].rule == FillRule::NonZero ? w != 0 : (w & 1) != 0;
}

bool Rasterizer::inside(const std::vector<int>& wind, int union_count) const {
    if (union_only_) return (union_count > 0) != inverted_;
    bool acc = false;
    for (std::size_t i = 0; i < comps_.size(); ++i) {
        const bool v = comp_inside(i, wind[i]);
        const PathOp op = i == 0 ? PathOp::Combine : comps_[i].op;  // (the first always combines)
        switch (op) {
            case PathOp::Combine: acc = acc || v; break;
            case PathOp::Subtract: acc = acc && !v; break;
            case PathOp::Intersect: acc = acc && v; break;
            case PathOp::Exclude: acc = acc != v; break;
        }
    }
    return acc != inverted_;
}

std::vector<float> Rasterizer::render(const PixelBox& box) const {
    const std::size_t w = static_cast<std::size_t>(std::max(0, box.width())), h = static_cast<std::size_t>(std::max(0, box.height()));
    std::vector<float> out(w * h, 0.0f);
    if (w == 0 || h == 0) return out;
    render_into(box, out.data());
    return out;
}

void Rasterizer::render_into(const PixelBox& box, float* out) const {
    const std::size_t w = static_cast<std::size_t>(box.width());
    RowState st(comps_.size(), w);
    const double top = static_cast<double>(box.y0);
    std::size_t next = static_cast<std::size_t>(
        std::partition_point(edges_.begin(), edges_.end(), [top](const Edge& e) { return e.y0 < top; }) - edges_.begin());
    for (std::size_t i = 0; i < next; ++i)
        if (edges_[i].y1 > top) st.active.push_back(i);
    for (int y = box.y0, row = 0; y < box.y1; ++y, ++row) {
        const double ya = static_cast<double>(y), yb = ya + 1.0;
        std::erase_if(st.active, [&](std::size_t i) { return !(edges_[i].y1 > ya); });
        while (next < edges_.size() && edges_[next].y0 < yb) {
            if (edges_[next].y1 > ya) st.active.push_back(next);
            ++next;
        }
        std::fill(st.acc.begin(), st.acc.end(), 0.0f);
        float* row_out = out + static_cast<std::size_t>(row) * w;
        if (st.active.empty()) {
            std::fill(row_out, row_out + w, inverted_ ? 1.0f : 0.0f);
            continue;
        }
        // (sub-bands: the row's edges and every edge's end inside the row)
        st.cuts.clear();
        st.cuts.push_back(ya);
        st.cuts.push_back(yb);
        for (const std::size_t i : st.active) {
            const Edge& e = edges_[i];
            if (e.y0 > ya && e.y0 < yb) st.cuts.push_back(e.y0);
            if (e.y1 > ya && e.y1 < yb) st.cuts.push_back(e.y1);
        }
        std::sort(st.cuts.begin(), st.cuts.end());
        st.cuts.erase(std::unique(st.cuts.begin(), st.cuts.end()), st.cuts.end());
        const std::vector<double> cuts = st.cuts;
        for (std::size_t k = 0; k + 1 < cuts.size(); ++k) band(cuts[k], cuts[k + 1], box.x0, st, 0);
        float sum = 0.0f;
        for (std::size_t x = 0; x < w; ++x) {
            sum += st.acc[x];
            row_out[x] = std::clamp(sum, 0.0f, 1.0f);
        }
    }
}

void Rasterizer::band(double a, double b, int x0, RowState& st, unsigned depth) const {
    if (b - a <= 1e-12) return;
    st.band.clear();
    const double mid = 0.5 * (a + b);
    for (const std::size_t i : st.active) {
        const Edge& e = edges_[i];
        if (e.y0 <= mid && e.y1 >= mid) {
            const double xa = e.x_at(a), xb = e.x_at(b);
            st.band.push_back(BandEdge{xa, xb, 0.5 * (xa + xb), static_cast<std::uint32_t>(i)});
        }
    }
    if (st.band.empty()) {
        if (inverted_) add_full(st.acc, b - a);
        return;
    }
    std::stable_sort(st.band.begin(), st.band.end(), [](const BandEdge& p, const BandEdge& q) { return p.xm < q.xm; });
    // (two edges crossing inside the band: split there, so the order is the same in every piece)
    if (depth < 24) {
        for (std::size_t k = 1; k < st.band.size(); ++k) {
            const BandEdge p = st.band[k - 1], q = st.band[k];
            const double top = p.xa - q.xa, bot = p.xb - q.xb;
            if (top > 1e-9 || bot > 1e-9) {
                const double t = std::fabs(top - bot) > 1e-15 ? top / (top - bot) : 0.5;
                double y = a + (b - a) * std::clamp(t, 0.0, 1.0);
                if (y <= a + 1e-9 || y >= b - 1e-9) y = mid;
                band(a, y, x0, st, depth + 1);
                band(y, b, x0, st, depth + 1);
                return;
            }
        }
    }
    std::fill(st.wind.begin(), st.wind.end(), 0);
    int union_count = 0;
    bool in = inverted_;
    if (in) add_full(st.acc, b - a);
    const std::vector<BandEdge> list = st.band;
    for (const BandEdge& be : list) {
        const Edge& e = edges_[be.idx];
        const std::size_t c = e.comp;
        if (union_only_) {
            const bool was = comp_inside(c, st.wind[c]);
            st.wind[c] += e.dir;
            const bool now = comp_inside(c, st.wind[c]);
            union_count += static_cast<int>(now) - static_cast<int>(was);
        } else {
            st.wind[c] += e.dir;
        }
        const bool now = inside(st.wind, union_count);
        if (now != in) {
            draw(st.acc, x0, be.xa, a, be.xb, b, now ? 1.0 : -1.0);
            in = now;
        }
    }
}

// --- stroking ---------------------------------------------------------------------------------------------------------

std::vector<std::vector<XY>> stroke_polygons(const std::vector<Polyline>& lines, const StrokeStyle& style, double tol) {
    std::vector<std::vector<XY>> out;
    if (!(style.width > 0.0) || !std::isfinite(style.width)) return out;
    const bool dashed = std::any_of(style.dashes.begin(), style.dashes.end(), [](double d) { return d > 0.0; });
    for (const Polyline& pl : lines) {
        if (dashed) {
            for (const Polyline& dash : dash_polyline(pl, style.dashes, style.dash_offset)) stroke_one(dash, style, tol, out);
        } else {
            stroke_one(pl, style, tol, out);
        }
    }
    return out;
}

std::vector<Polyline> dash_polyline(const Polyline& pl, const std::vector<double>& pattern, double offset) {
    std::vector<double> pat;
    for (const double d : pattern) pat.push_back(std::max(d, 0.0));
    if (pat.size() % 2 == 1) {
        const std::vector<double> copy = pat;
        pat.insert(pat.end(), copy.begin(), copy.end());
    }
    double total = 0.0;
    for (const double d : pat) total += d;
    if (total <= 1e-9 || pl.pts.size() < 2) return {pl};
    std::vector<XY> pts = pl.pts;
    std::vector<bool> knots = pl.knot;
    knots.resize(pts.size(), true);
    if (pl.closed) {
        pts.push_back(pl.pts[0]);
        knots.push_back(true);
    }
    std::size_t idx = 0;
    double left = pat[0];
    double skip = std::fmod(offset, total);
    if (skip < 0.0) skip += total;
    while (skip > 0.0) {
        if (skip >= left) {
            skip -= left;
            idx = (idx + 1) % pat.size();
            left = pat[idx];
        } else {
            left -= skip;
            skip = 0.0;
        }
    }
    std::vector<Polyline> out;
    std::optional<Polyline> cur;
    if (idx % 2 == 0) cur = Polyline{{pts[0]}, {true}, false};
    std::size_t guard = 0;
    for (std::size_t wi = 0; wi + 1 < pts.size(); ++wi) {
        const XY a = pts[wi], b = pts[wi + 1];
        const bool b_knot = knots[wi + 1];
        const double seg = len(sub(b, a));
        double pos = 0.0;
        while (seg - pos > 1e-12) {
            if (++guard > 10000000) break;
            const double step = std::min(left, seg - pos);
            pos += step;
            left -= step;
            const bool at_end = seg - pos <= 1e-12;
            const XY p = at_end ? b : add(a, mul(sub(b, a), pos / seg));
            if (cur) {
                cur->pts.push_back(p);
                cur->knot.push_back(!at_end || b_knot);
            }
            if (left <= 1e-12) {
                idx = (idx + 1) % pat.size();
                left = pat[idx];
                if (idx % 2 == 1) {
                    if (cur) {
                        out.push_back(std::move(*cur));
                        cur.reset();
                    }
                } else {
                    cur = Polyline{{p}, {true}, false};
                }
            }
        }
    }
    if (cur && cur->pts.size() > 1) out.push_back(std::move(*cur));
    return out;
}

// --- paths -------------------------------------------------------------------------------------------------------------

Rasterizer fill_rasterizer(const Path& path, double tol) {
    Rasterizer r(path.inverted);
    for (const Subpath& s : path.subpaths) r.add_component({flatten_subpath(s, tol).pts}, s.op, path.fill_rule);
    return r;
}

Rasterizer stroke_rasterizer(const Path& path, const StrokeStyle& style, double tol) {
    // (outlines take a coarser flattening than fills: the offset hides the chords)
    const std::vector<Polyline> lines = flatten_path(path, std::max(std::min(tol * 4.0, std::max(style.width, 0.01) * 0.1), 1e-3));
    Rasterizer r(false);
    r.add_component(stroke_polygons(lines, style, tol), PathOp::Combine, FillRule::NonZero);
    return r;
}

StrokeStyle stroke_style(const core::vector::ShapeStroke& s, double px_per_mm) {
    const double w = std::max(s.width_mm, 0.0) * px_per_mm;
    StrokeStyle out;
    out.width = w;
    out.cap = s.cap;
    out.join = s.join;
    out.miter_limit = std::max(s.miter_limit, 1.0);
    for (const double d : s.dashes) out.dashes.push_back(d * w);
    out.dash_offset = s.dash_offset * w;
    return out;
}

std::vector<float> path_coverage(const Path& path, const PixelBox& box) { return fill_rasterizer(path, kTolerance).render(box); }

std::vector<float> vector_mask_values(const core::vector::VectorMask& m, const PixelBox& box) {
    const std::size_t n = static_cast<std::size_t>(std::max(0, box.width())) * static_cast<std::size_t>(std::max(0, box.height()));
    if (!m.enabled) return std::vector<float>(n, 1.0f);
    std::vector<float> v = m.path.empty() ? std::vector<float>(n, m.path.inverted ? 0.0f : 1.0f) : path_coverage(m.path, box);
    if (m.density < 1.0) {
        const float d = static_cast<float>(std::clamp(m.density, 0.0, 1.0));
        for (float& x : v) x = 1.0f - d * (1.0f - x);
    }
    return v;
}

Path to_pixels(const Path& path, double dpi, int x0, int y0) {
    const double k = dpi / 25.4;
    return path.transformed({k, 0.0, 0.0, k, -static_cast<double>(x0), -static_cast<double>(y0)});
}

Image path_mask(const Path& path, double dpi, Size size) {
    const std::vector<float> cover = path_coverage(to_pixels(path, dpi), PixelBox{0, 0, size.width, size.height});
    std::string bytes(cover.size(), '\0');
    for (std::size_t i = 0; i < cover.size(); ++i) bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(cover[i], 0.0f, 1.0f) * 255.0f)));
    return Image::frombytes("L", size, bytes);
}

// --- live shapes ---------------------------------------------------------------------------------------------------------

namespace {

Knot smooth_knot(core::vector::XY anchor, core::vector::XY in, core::vector::XY out) { return Knot{anchor, in, out, true}; }

Path one(Subpath s) {
    Path p;
    p.subpaths.push_back(std::move(s));
    return p;
}

Subpath polygon_of(const std::vector<core::vector::XY>& pts) {
    Subpath s;
    s.closed = true;
    for (const auto& p : pts) s.knots.push_back(Knot::corner(p.x, p.y));
    return s;
}

}  // namespace

Path rect_path(double x, double y, double w, double h) { return one(polygon_of({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}})); }

std::array<double, 4> clamp_radii(double w, double h, const std::array<double, 4>& radii) {
    std::array<double, 4> r{};
    for (std::size_t i = 0; i < 4; ++i) r[i] = std::max(radii[i], 0.0);
    double f = 1.0;
    const std::array<std::array<double, 3>, 4> sides{{{r[0], r[1], w}, {r[1], r[2], h}, {r[2], r[3], w}, {r[3], r[0], h}}};
    for (const auto& [a, b, side] : sides) {
        if (a + b > side && a + b > 0.0) f = std::min(f, side / (a + b));
    }
    for (double& v : r) v *= f;
    return r;
}

Path rounded_rect(double x, double y, double w, double h, const std::array<double, 4>& radii) {
    w = std::fabs(w);
    h = std::fabs(h);
    const auto [tl, tr, br, bl] = clamp_radii(w, h, radii);
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) return rect_path(x, y, w, h);
    const double x1 = x + w, y1 = y + h;
    std::vector<Knot> k;
    using P = core::vector::XY;
    const auto push_arc = [&](P start, P end, P c_start, P c_end, double r) {
        if (r <= 0.0) {
            k.push_back(Knot::corner(start.x, start.y));
            return;
        }
        k.push_back(Knot{start, start, c_start, false});
        k.push_back(Knot{end, c_end, end, false});
    };
    // (each corner's arc from the end of one edge to the start of the next, clockwise)
    push_arc({x1 - tr, y}, {x1, y + tr}, {x1 - tr + kKappa * tr, y}, {x1, y + tr - kKappa * tr}, tr);
    push_arc({x1, y1 - br}, {x1 - br, y1}, {x1, y1 - br + kKappa * br}, {x1 - br + kKappa * br, y1}, br);
    push_arc({x + bl, y1}, {x, y1 - bl}, {x + bl - kKappa * bl, y1}, {x, y1 - bl + kKappa * bl}, bl);
    push_arc({x, y + tl}, {x + tl, y}, {x, y + tl - kKappa * tl}, {x + tl - kKappa * tl, y}, tl);
    // (start at the top-left arc's end: the first knot is on the top edge)
    std::rotate(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(k.size() - 1), k.end());
    std::vector<Knot> out;
    for (const Knot& kn : k) {  // (consecutive anchors in one place: one knot)
        if (!out.empty() && std::fabs(out.back().anchor.x - kn.anchor.x) < 1e-12 && std::fabs(out.back().anchor.y - kn.anchor.y) < 1e-12) {
            out.back().out = kn.out;
            continue;
        }
        out.push_back(kn);
    }
    if (out.size() > 1) {
        const Knot f = out.front(), l = out.back();
        if (std::fabs(f.anchor.x - l.anchor.x) < 1e-12 && std::fabs(f.anchor.y - l.anchor.y) < 1e-12) {
            out.front().in = l.in;
            out.pop_back();
        }
    }
    Subpath s;
    s.closed = true;
    s.knots = std::move(out);
    return one(std::move(s));
}

Path ellipse_path(double x, double y, double w, double h) {
    const double rx = w / 2.0, ry = h / 2.0;
    const double cx = x + rx, cy = y + ry;
    const double kx = rx * kKappa, ky = ry * kKappa;
    Subpath s;
    s.closed = true;
    s.knots = {smooth_knot({cx, cy - ry}, {cx - kx, cy - ry}, {cx + kx, cy - ry}), smooth_knot({cx + rx, cy}, {cx + rx, cy - ky}, {cx + rx, cy + ky}),
               smooth_knot({cx, cy + ry}, {cx + kx, cy + ry}, {cx - kx, cy + ry}), smooth_knot({cx - rx, cy}, {cx - rx, cy + ky}, {cx - rx, cy - ky})};
    return one(std::move(s));
}

Path polygon_path(double x, double y, double w, double h, int sides, double star_ratio) {
    const int n_sides = std::clamp(sides, 3, 100);
    const double rx = w / 2.0, ry = h / 2.0;
    const double cx = x + rx, cy = y + ry;
    const bool star = star_ratio > 0.0 && star_ratio < 1.0;
    const int count = star ? n_sides * 2 : n_sides;
    std::vector<core::vector::XY> pts;
    for (int i = 0; i < count; ++i) {
        const double a = -kPi / 2 + kTau * static_cast<double>(i) / static_cast<double>(count);
        const double k = star && i % 2 == 1 ? star_ratio : 1.0;
        pts.push_back({cx + rx * k * std::cos(a), cy + ry * k * std::sin(a)});
    }
    return one(polygon_of(pts));
}

Path line_path(core::vector::XY a, core::vector::XY b, double weight) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l = std::hypot(dx, dy);
    const double hw = std::max(weight, 0.0) / 2.0;
    const double nx = l > 0.0 ? -dy / l * hw : 0.0, ny = l > 0.0 ? dx / l * hw : hw;
    return one(polygon_of({{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}}));
}

Path live_path(const core::vector::LiveShape& s) {
    using Kind = core::vector::LiveShape::Kind;
    const auto& [x, y, w, h] = s.rect;
    switch (s.kind) {
        case Kind::Rect:
            return rounded_rect(x, y, w, h, s.radii);
        case Kind::Ellipse:
            return ellipse_path(x, y, w, h);
        case Kind::Polygon: {
            Path p = polygon_path(x, y, w, h, s.sides, s.star_ratio);
            if (s.angle != 0.0) {  // (turned about the box's middle, clockwise on the page)
                const double t = s.angle * kPi / 180.0, c = std::cos(t), sn = std::sin(t);
                const double cx = x + w / 2, cy = y + h / 2;
                p = p.transformed({c, sn, -sn, c, cx - c * cx + sn * cy, cy - sn * cx - c * cy});
            }
            return p;
        }
        case Kind::Line:
            return line_path(s.from, s.to, s.weight_mm);
    }
    return {};
}

}  // namespace genko::render::path_raster
