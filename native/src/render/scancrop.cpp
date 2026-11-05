// 写真の切り抜きと角度補正 (PC5 I-h): PhotoCraft v0.2.0's algo/src/scancrop.rs in C++ (MIT,
// native/third_party/photocraft/NOTICE.md), and the cut of each photo at the scan's own pixels (render/resample).

#include "render/scancrop.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

#include "core/error.hpp"
#include "render/image_ops.hpp"
#include "render/page.hpp"
#include "render/resample.hpp"

namespace genko::render::scancrop {

namespace {

using Pt = std::array<double, 2>;

float otsu(const std::vector<float>& v) {
    constexpr std::size_t kBins = 256;
    std::array<double, kBins> hist{};
    for (const float x : v) hist[static_cast<std::size_t>(std::clamp(x, 0.0f, 1.0f) * static_cast<float>(kBins - 1))] += 1.0;
    double total = 0, sum = 0;
    for (std::size_t i = 0; i < kBins; ++i) {
        total += hist[i];
        sum += static_cast<double>(i) * hist[i];
    }
    double wb = 0, sb = 0, best = 0;
    std::size_t thr = 0;
    for (std::size_t i = 0; i < kBins; ++i) {
        wb += hist[i];
        if (wb == 0.0) continue;
        const double wf = total - wb;
        if (wf == 0.0) break;
        sb += static_cast<double>(i) * hist[i];
        const double mb = sb / wb, mf = (sum - sb) / wf;
        const double between = wb * wf * (mb - mf) * (mb - mf);
        if (between > best) {
            best = between;
            thr = i;
        }
    }
    return (static_cast<float>(thr) + 0.5f) / static_cast<float>(kBins - 1);
}

// A square structuring element, separable: dilated (any) or eroded (all), the outside false.
std::vector<char> morph(std::size_t w, std::size_t h, const std::vector<char>& m, std::int64_t r, bool dilate) {
    const auto pass = [&](const std::vector<char>& src, bool horizontal) {
        std::vector<char> out(w * h, 0);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                bool acc = !dilate;
                for (std::int64_t d = -r; d <= r; ++d) {
                    const std::int64_t xx = horizontal ? static_cast<std::int64_t>(x) + d : static_cast<std::int64_t>(x);
                    const std::int64_t yy = horizontal ? static_cast<std::int64_t>(y) : static_cast<std::int64_t>(y) + d;
                    const bool v = xx >= 0 && yy >= 0 && xx < static_cast<std::int64_t>(w) && yy < static_cast<std::int64_t>(h) && src[std::size_t(yy) * w + std::size_t(xx)] != 0;
                    if (dilate) {
                        acc = acc || v;
                    } else {
                        acc = acc && v;
                    }
                }
                out[y * w + x] = acc ? 1 : 0;
            }
        }
        return out;
    };
    return pass(pass(m, true), false);
}

std::vector<Pt> convex_hull(std::vector<Pt> pts) {
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a[0] != b[0] ? a[0] < b[0] : a[1] < b[1]; });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    const auto cross = [](const Pt& o, const Pt& a, const Pt& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); };
    std::vector<Pt> lower, upper;
    for (const Pt& p : pts) {
        while (lower.size() >= 2 && cross(lower[lower.size() - 2], lower[lower.size() - 1], p) <= 0.0) lower.pop_back();
        lower.push_back(p);
    }
    for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
        while (upper.size() >= 2 && cross(upper[upper.size() - 2], upper[upper.size() - 1], *it) <= 0.0) upper.pop_back();
        upper.push_back(*it);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

// The minimum-area rectangle around a convex hull: its centre, sides and angle (degrees, normalised to −45..45).
struct Rect {
    Pt center{0, 0};
    double w = 0, h = 0, angle = 0;
};
Rect min_rect(const std::vector<Pt>& hull) {
    double best = std::numeric_limits<double>::max();
    Rect out;
    const std::size_t n = hull.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Pt a = hull[i], b = hull[(i + 1) % n];
        const double th = std::atan2(b[1] - a[1], b[0] - a[0]);
        const double s = std::sin(th), c = std::cos(th);
        double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = std::numeric_limits<double>::lowest(), y1 = x1;
        for (const Pt& p : hull) {
            const double u = c * p[0] + s * p[1], v = -s * p[0] + c * p[1];
            x0 = std::min(x0, u);
            x1 = std::max(x1, u);
            y0 = std::min(y0, v);
            y1 = std::max(y1, v);
        }
        const double area = (x1 - x0) * (y1 - y0);
        if (area < best) {
            best = area;
            const double cu = (x0 + x1) / 2.0, cv = (y0 + y1) / 2.0;
            out = Rect{{c * cu - s * cv, s * cu + c * cv}, x1 - x0, y1 - y0, th * 180.0 / std::numbers::pi};
        }
    }
    while (out.angle > 45.0) {
        out.angle -= 90.0;
        std::swap(out.w, out.h);
    }
    while (out.angle < -45.0) {
        out.angle += 90.0;
        std::swap(out.w, out.h);
    }
    return out;
}

// A pixel's colour as RGB in 0..1 (grey its value thrice, CMYK by the plain formula), a clear one over white.
std::array<float, 3> rgb_at(const codecs::CodecImage& scan, std::size_t pixel) {
    const unsigned n = codecs::channels(scan.layout());
    const std::size_t at = pixel * n;
    const auto v = [&](unsigned c) { return std::clamp(scan.value(at + c), 0.0f, 1.0f); };
    std::array<float, 3> rgb{};
    if (codecs::is_gray(scan.layout())) {
        rgb = {v(0), v(0), v(0)};
    } else if (codecs::is_cmyk(scan.layout())) {
        const float k = 1.0f - v(3);
        rgb = {(1.0f - v(0)) * k, (1.0f - v(1)) * k, (1.0f - v(2)) * k};
    } else {
        rgb = {v(0), v(1), v(2)};
    }
    if (codecs::has_alpha(scan.layout())) {
        const float a = v(n - 1);
        for (float& c : rgb) c = c * a + (1.0f - a);
    }
    return rgb;
}

}  // namespace

std::array<std::array<double, 2>, 4> FoundPhoto::corners() const {
    const double rad = angle * std::numbers::pi / 180.0;
    const double s = std::sin(rad), c = std::cos(rad);
    const double hw = width / 2.0, hh = height / 2.0;
    std::array<std::array<double, 2>, 4> out{};
    const std::array<Pt, 4> local{{{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}}};
    for (std::size_t i = 0; i < 4; ++i) out[i] = {center[0] + c * local[i][0] - s * local[i][1], center[1] + s * local[i][0] + c * local[i][1]};
    return out;
}

std::vector<FoundPhoto> find_photos(const codecs::CodecImage& scan, const std::function<bool()>& stopped) {
    const std::size_t w = scan.width(), h = scan.height();
    if (w < 8 || h < 8) return {};
    // (worked on at most 800 px a side)
    const std::size_t k = std::max<std::size_t>(1, (std::max(w, h) + 799) / 800);
    const std::size_t sw = (w + k - 1) / k, sh = (h + k - 1) / k;
    std::vector<std::array<float, 3>> small(sw * sh, {0, 0, 0});
    std::vector<std::uint32_t> cnt(sw * sh, 0);
    for (std::size_t y = 0; y < h; ++y) {
        if (y % 64 == 0 && stopped && stopped()) throw Cancelled();
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = (y / k) * sw + x / k;
            const auto p = rgb_at(scan, y * w + x);
            for (std::size_t c = 0; c < 3; ++c) small[i][c] += p[c];
            ++cnt[i];
        }
    }
    for (std::size_t i = 0; i < small.size(); ++i)
        for (float& v : small[i]) v /= static_cast<float>(std::max<std::uint32_t>(cnt[i], 1));
    // (the background: each channel's median of the border)
    std::vector<std::array<float, 3>> border;
    for (std::size_t x = 0; x < sw; ++x) {
        border.push_back(small[x]);
        border.push_back(small[(sh - 1) * sw + x]);
    }
    for (std::size_t y = 0; y < sh; ++y) {
        border.push_back(small[y * sw]);
        border.push_back(small[y * sw + sw - 1]);
    }
    std::array<float, 3> bg{};
    for (std::size_t c = 0; c < 3; ++c) {
        std::vector<float> v;
        v.reserve(border.size());
        for (const auto& p : border) v.push_back(p[c]);
        std::sort(v.begin(), v.end());
        bg[c] = v[v.size() / 2];
    }
    std::vector<float> dist(small.size());
    for (std::size_t i = 0; i < small.size(); ++i) {
        const auto& p = small[i];
        dist[i] = std::sqrt((p[0] - bg[0]) * (p[0] - bg[0]) + (p[1] - bg[1]) * (p[1] - bg[1]) + (p[2] - bg[2]) * (p[2] - bg[2])) / std::sqrt(3.0f);
    }
    const float thr = std::max(otsu(dist), 0.04f);
    std::vector<char> mask(dist.size());
    for (std::size_t i = 0; i < dist.size(); ++i) mask[i] = dist[i] > thr ? 1 : 0;
    const std::int64_t r = std::max<std::int64_t>(static_cast<std::int64_t>(std::max(sw, sh) / 200), 1);
    mask = morph(sw, sh, morph(sw, sh, mask, r, true), r, false);
    mask = morph(sw, sh, morph(sw, sh, mask, 1, false), 1, true);
    // (8-connected parts)
    std::vector<std::uint32_t> label(sw * sh, std::numeric_limits<std::uint32_t>::max());
    std::vector<std::vector<std::size_t>> comps;
    for (std::size_t s = 0; s < sw * sh; ++s) {
        if (mask[s] == 0 || label[s] != std::numeric_limits<std::uint32_t>::max()) continue;
        const auto id = static_cast<std::uint32_t>(comps.size());
        std::vector<std::size_t> stack{s};
        label[s] = id;
        std::vector<std::size_t> px;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            px.push_back(i);
            const auto x = static_cast<std::int64_t>(i % sw), y = static_cast<std::int64_t>(i / sw);
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const std::int64_t xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= static_cast<std::int64_t>(sw) || yy >= static_cast<std::int64_t>(sh)) continue;
                    const std::size_t j = std::size_t(yy) * sw + std::size_t(xx);
                    if (mask[j] != 0 && label[j] == std::numeric_limits<std::uint32_t>::max()) {
                        label[j] = id;
                        stack.push_back(j);
                    }
                }
            }
        }
        comps.push_back(std::move(px));
    }
    const double total = static_cast<double>(sw * sh);
    std::vector<FoundPhoto> out;
    for (const auto& c : comps) {
        if (!(static_cast<double>(c.size()) > total * 0.01)) continue;
        // (the hull of the boundary pixels' corners, in the scan's pixels)
        std::vector<Pt> pts;
        for (const std::size_t i : c) {
            const std::size_t x = i % sw, y = i / sw;
            const bool edge = x == 0 || y == 0 || x + 1 == sw || y + 1 == sh || mask[i - 1] == 0 || mask[i + 1] == 0 || mask[i - sw] == 0 || mask[i + sw] == 0;
            if (!edge) continue;
            const auto fx = static_cast<double>(x), fy = static_cast<double>(y), kk = static_cast<double>(k);
            for (const Pt& p : {Pt{fx, fy}, Pt{fx + 1, fy}, Pt{fx, fy + 1}, Pt{fx + 1, fy + 1}}) pts.push_back({p[0] * kk, p[1] * kk});
        }
        const Rect rect = min_rect(convex_hull(std::move(pts)));
        const auto inset = static_cast<double>(k);
        FoundPhoto f;
        f.center = rect.center;
        f.width = std::max(rect.w - 2.0 * inset, 1.0);
        f.height = std::max(rect.h - 2.0 * inset, 1.0);
        f.angle = rect.angle;
        f.area = static_cast<double>(c.size()) / total;
        out.push_back(f);
    }
    // (reading order: rows by the centre's y, in bands of a third of the smallest photo, then x)
    double band = std::numeric_limits<double>::max();
    for (const FoundPhoto& p : out) band = std::min(band, std::min(p.height, p.width));
    band = std::max(band, 1.0) / 3.0;
    std::stable_sort(out.begin(), out.end(), [band](const FoundPhoto& a, const FoundPhoto& b) {
        const double ra = std::floor(a.center[1] / band), rb = std::floor(b.center[1] / band);
        return ra != rb ? ra < rb : a.center[0] < b.center[0];
    });
    return out;
}

codecs::CodecImage cut_photo(const codecs::CodecImage& scan, const FoundPhoto& photo) {
    const int pw = std::max(1, static_cast<int>(std::lround(photo.width)));
    const int ph = std::max(1, static_cast<int>(std::lround(photo.height)));
    const auto corners = photo.corners();
    // (the part of the scan the photo lies on, a few pixels around it: only that is made floats)
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = std::numeric_limits<double>::lowest(), y1 = x1;
    for (const auto& c : corners) {
        x0 = std::min(x0, c[0]);
        y0 = std::min(y0, c[1]);
        x1 = std::max(x1, c[0]);
        y1 = std::max(y1, c[1]);
    }
    const auto clampi = [](double v, std::int64_t hi) { return std::clamp<std::int64_t>(static_cast<std::int64_t>(v), 0, hi); };
    const std::int64_t bx = clampi(std::floor(x0) - 3, scan.width() - 1), by = clampi(std::floor(y0) - 3, scan.height() - 1);
    const std::int64_t ex = clampi(std::ceil(x1) + 3, scan.width()), ey = clampi(std::ceil(y1) + 3, scan.height());
    const auto bw = static_cast<std::uint32_t>(std::max<std::int64_t>(ex - bx, 1)), bh = static_cast<std::uint32_t>(std::max<std::int64_t>(ey - by, 1));
    const codecs::CodecImage part = scan.crop(static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by), bw, bh);
    const auto n = static_cast<int>(codecs::channels(scan.layout()));
    const image_ops::Held held((std::uint64_t(bw) * bh + std::uint64_t(pw) * std::uint64_t(ph)) * std::uint64_t(n) * sizeof(float));
    resample::Plane plane(static_cast<int>(bw), static_cast<int>(bh), n, false);
    plane.hdr = codecs::is_float(scan.sample());
    plane.v = part.normalized();
    // (the photo's rectangle onto its corners in the part: the map from the part to the photo is its inverse)
    std::array<std::array<double, 2>, 4> quad{};
    for (std::size_t i = 0; i < 4; ++i) quad[i] = {corners[i][0] - static_cast<double>(bx), corners[i][1] - static_cast<double>(by)};
    const auto to_part = resample::Homography::rect_to_quad({0.0, 0.0, static_cast<double>(pw), static_cast<double>(ph)}, quad);
    const auto to_photo = to_part ? to_part->inverse() : std::nullopt;
    if (!to_photo) throw Unmappable();
    resample::Plane cut = resample::project(plane, *to_photo, pw, ph, resample::Interp::Bicubic);
    // (opaque, as a scanned print)
    if (codecs::has_alpha(scan.layout())) {
        for (std::size_t i = static_cast<std::size_t>(n - 1); i < cut.v.size(); i += static_cast<std::size_t>(n)) cut.v[i] = 1.0f;
    }
    codecs::CodecImage out = codecs::CodecImage::from_normalized(static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph), scan.layout(), scan.sample(), cut.v);
    out.icc = scan.icc;
    out.meta.dpi = scan.meta.dpi;
    return out;
}

}  // namespace genko::render::scancrop
