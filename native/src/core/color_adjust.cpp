#include "core/color_adjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// The settings as core::adjustment reads them (it has refused anything it cannot read by then).
double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = get(params, key);
    return v == nullptr ? fallback : to_float(*v);
}
double whole(const Json& params, std::string_view key, double fallback) {
    const Json* v = get(params, key);
    if (v == nullptr) return fallback;
    if (v->is_number_float()) return py_trunc(v->get<double>());
    if (const auto big = py_big_int_text(*v)) return std::strtod(big->c_str(), nullptr);
    return static_cast<double>(to_int(*v));
}
std::array<double, 3> colour(const Json& value) {
    std::array<double, 3> out{};
    std::size_t c = 0;
    for (const Json& v : iterate(value)) {
        if (c == 3) break;
        out[c++] = v.is_number_float() ? py_trunc(v.get<double>()) : static_cast<double>(to_int(v));
    }
    return out;
}
// ImageOps.grayscale's weights (ITU-R 601-2 luma as Pillow keeps it, 16-bit fixed point), on 8-bit units
double luma(const std::array<double, 3>& u) { return (u[0] * 19595 + u[1] * 38470 + u[2] * 7471) / 65536.0; }
// Pillow rounds the luma to a whole grey level: kept for a threshold, so an 8-bit picture falls on the same side
double grey_level(const std::array<double, 3>& u) { return std::floor(luma(u) + 0.5); }

}  // namespace

PreciseAdjustment::PreciseAdjustment(std::string_view kind, const Json& params) {
    if (kind == "exposure") {
        exposure_ = Exposure::parse(params);
        way_ = Way::Exposure;
        return;
    }
    (void)adjustment(kind, params);  // (its refusals, as the 8-bit filter has them)
    const auto channel_of = [&] {
        const Json* given = get(params, "channel");
        const std::string name = given != nullptr && py_truthy(*given) ? py_str(*given) : std::string("rgb");
        return name == "r" ? 0 : name == "g" ? 1 : name == "b" ? 2 : -1;
    };
    if (kind == "levels") {
        static constexpr std::array<const char*, 4> kTable{"gamma", "out_black", "out_white", "channel"};
        if (std::any_of(kTable.begin(), kTable.end(), [&](const char* key) { const Json* v = get(params, key); return v != nullptr && !v->is_null(); })) {
            way_ = Way::Levels;
            a_ = number(params, "black", 0);
            b_ = py_max(a_ + 1, number(params, "white", 255));
            c_ = py_max(0.1, py_min(9.99, number(params, "gamma", 1.0)));
            d_ = number(params, "out_black", 0);
            e_ = number(params, "out_white", 255);
            channel_ = channel_of();
            return;
        }
        way_ = Way::Simple;
        a_ = whole(params, "black", 0);
        b_ = whole(params, "white", 255);
        c_ = py_max(1.0, b_ - a_);
        return;
    }
    if (kind == "curve") {
        if (const Json* points = get(params, "points"); points != nullptr && py_truthy(*points)) {
            way_ = Way::Tone;
            channel_ = channel_of();
            std::vector<std::pair<double, double>> pts;
            for (const Json& item : iterate(*points)) {
                const auto parts = iterate(item);
                const double x = py_round_whole(to_float(parts[0]));
                const double y = to_float(parts[1]);
                const auto at = std::find_if(pts.begin(), pts.end(), [x](const auto& p) { return p.first == x; });
                if (at != pts.end()) at->second = y;
                else pts.emplace_back(x, y);
            }
            std::stable_sort(pts.begin(), pts.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
            for (const auto& [x, y] : pts) { xs_.push_back(x); ys_.push_back(py_max(0.0, py_min(255.0, y))); }
            const std::size_t n = xs_.size();
            std::vector<double> d;
            for (std::size_t k = 0; k + 1 < n; ++k) d.push_back((ys_[k + 1] - ys_[k]) / py_max(1e-9, xs_[k + 1] - xs_[k]));
            ms_ = {d[0]};
            for (std::size_t k = 1; k + 1 < n; ++k) ms_.push_back(d[k - 1] * d[k] <= 0 ? 0.0 : (d[k - 1] + d[k]) / 2);
            ms_.push_back(d.back());
            for (std::size_t k = 0; k + 1 < n; ++k) {
                if (d[k] == 0) { ms_[k] = ms_[k + 1] = 0.0; continue; }
                const double l = ms_[k] / d[k], r = ms_[k + 1] / d[k];
                if (l * l + r * r > 9) {
                    const double t = 3 / std::sqrt(l * l + r * r);
                    ms_[k] = t * l * d[k];
                    ms_[k + 1] = t * r * d[k];
                }
            }
            return;
        }
        way_ = Way::Gamma;
        a_ = py_max(0.2, py_min(5.0, number(params, "gamma", 1.6)));
        return;
    }
    if (kind == "brightness_contrast") {
        way_ = Way::Brightness;
        channel_ = channel_of();
        a_ = py_max(-100.0, py_min(100.0, number(params, "brightness", 0))) * 1.275;
        const double c = py_max(-100.0, py_min(100.0, number(params, "contrast", 0)));
        b_ = c <= 0 ? 1 + c / 100 : 1 / py_max(0.01, 1 - c / 100 * 0.99);
        return;
    }
    if (kind == "hue") {
        way_ = Way::Hue;
        const double shift = static_cast<double>(static_cast<std::int64_t>(py_fmod(py_trunc(number(params, "shift", 30)), 360.0)));
        a_ = static_cast<double>(py_round_int(shift * 255 / 360)) / 255;  // (Pillow's hue turns by 255 steps a turn)
        b_ = py_max(0.0, number(params, "saturation", 1.0));
        c_ = py_max(0.0, number(params, "value", 1.0));
        return;
    }
    if (kind == "invert") { way_ = Way::Invert; return; }
    if (kind == "posterize") {
        way_ = Way::Posterize;
        a_ = 255.0 / (py_max(2.0, py_min(64.0, whole(params, "levels", 4))) - 1);
        return;
    }
    if (kind == "bitonal" || kind == "threshold") {
        way_ = kind == "bitonal" ? Way::Bitonal : Way::Threshold;
        a_ = whole(params, "threshold", kind == "bitonal" ? 180 : 128);
        return;
    }
    if (kind == "gradient_map") {
        if (const Json* stops = get(params, "stops"); stops != nullptr && py_truthy(*stops)) {
            way_ = Way::Stops;
            std::vector<std::pair<double, std::array<double, 3>>> placed;
            for (const Json& st : iterate(*stops)) placed.emplace_back(to_float(subscript(st, 0)), colour(subscript(st, 1)));
            std::stable_sort(placed.begin(), placed.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
            std::vector<double> places;
            for (const auto& [at, rgb] : placed) { places.push_back(at); colours_.push_back(rgb); }
            for (unsigned c = 0; c < 3; ++c) {
                std::vector<double> fp;
                for (const auto& rgb : colours_) fp.push_back(rgb[c]);
                stops_.emplace_back(places, std::move(fp));
            }
            return;
        }
        way_ = Way::Gradient;
        const Json* given = get(params, "colors");
        const Json list = given != nullptr && py_truthy(*given) ? *given : Json::array({Json::array({0, 0, 0}), Json::array({255, 255, 255})});
        for (const Json& c : iterate(list)) colours_.push_back(colour(c));
        return;
    }
    throw PyValueError("unknown filter " + std::string(kind));
}

double PreciseAdjustment::through(double u) const {
    switch (way_) {
    case Way::Levels: {
        const double t = std::pow(std::clamp((u - a_) / (b_ - a_), 0.0, 1.0), 1 / c_);
        return std::clamp(d_ + t * (e_ - d_), 0.0, 255.0);
    }
    case Way::Simple:
        if (u <= a_) return 0;
        if (u >= b_) return 255;
        return (u - a_) * 255 / c_;
    case Way::Tone: {
        if (u <= xs_.front()) return ys_.front();
        if (u >= xs_.back()) return ys_.back();
        std::size_t k = 0;
        while (!(xs_[k] <= u && u <= xs_[k + 1])) ++k;
        const double h = xs_[k + 1] - xs_[k], t = (u - xs_[k]) / h;
        const double h00 = 2 * t * t * t - 3 * t * t + 1, h10 = t * t * t - 2 * t * t + t;
        const double h01 = -2 * t * t * t + 3 * t * t, h11 = t * t * t - t * t;
        return std::clamp(h00 * ys_[k] + h10 * h * ms_[k] + h01 * ys_[k + 1] + h11 * h * ms_[k + 1], 0.0, 255.0);
    }
    case Way::Gamma: {  // (black and white stay; HDR goes on along the curve, a negative sample mirrored)
        const double v = u / 255;
        return 255 * (v < 0 ? -std::pow(-v, a_) : std::pow(v, a_));
    }
    case Way::Brightness: {
        const double out = (u - 127.5) * b_ + 127.5 + a_;
        return u >= 0 && u <= 255 ? std::clamp(out, 0.0, 255.0) : out;
    }
    case Way::Invert:
        return 255 - u;
    case Way::Posterize:
        return std::round(std::clamp(u, 0.0, 255.0) / a_) * a_;
    default:
        return u;
    }
}

std::array<double, 3> PreciseAdjustment::operator()(const std::array<double, 3>& rgb) const {
    const std::array<double, 3> u{rgb[0] * 255, rgb[1] * 255, rgb[2] * 255};
    std::array<double, 3> out = rgb;
    switch (way_) {
    case Way::Exposure: {
        for (unsigned c = 0; c < 3; ++c) out[c] = linear_to_srgb(exposure_.apply(srgb_to_linear(rgb[c])));
        return out;
    }
    case Way::Bitonal:
    case Way::Threshold: {
        const double g = grey_level(u);
        const double v = (way_ == Way::Bitonal ? g > a_ : g >= a_) ? 1.0 : 0.0;
        return {v, v, v};
    }
    case Way::Gradient:
    case Way::Stops: {
        const double g = std::clamp(luma(u), 0.0, 255.0) / 255;
        for (unsigned c = 0; c < 3; ++c) {
            double v = 0;
            if (way_ == Way::Stops) {
                v = stops_[c](g);
            } else {
                const double pos = g * static_cast<double>(colours_.size() - 1);
                const std::size_t k = std::min(colours_.size() - 2, static_cast<std::size_t>(pos));
                const double t = pos - static_cast<double>(k);
                v = colours_[k][c] + (colours_[k + 1][c] - colours_[k][c]) * t;
            }
            out[c] = std::clamp(v, 0.0, 255.0) / 255;
        }
        return out;
    }
    case Way::Hue: {
        // colorsys's HSV (as Pillow's RGB ↔ HSV): the hue turned, the saturation and value scaled; an in-range
        // saturation or value stays within 1, an HDR one goes on
        const double hi = std::max({rgb[0], rgb[1], rgb[2]}), lo = std::min({rgb[0], rgb[1], rgb[2]});
        if (hi <= 0) return {rgb[0] * c_, rgb[1] * c_, rgb[2] * c_};
        double h = 0, s = 0;
        if (hi != lo) {
            const double span = hi - lo;
            s = span / hi;
            const double rc = (hi - rgb[0]) / span, gc = (hi - rgb[1]) / span, bc = (hi - rgb[2]) / span;
            h = rgb[0] == hi ? bc - gc : rgb[1] == hi ? 2.0 + rc - bc : 4.0 + gc - rc;
            h = std::fmod(h / 6.0 + 1.0, 1.0);
        }
        h = std::fmod(h + a_, 1.0);
        s = s <= 1 ? std::min(1.0, s * b_) : s * b_;
        const double v = hi <= 1 ? std::min(1.0, hi * c_) : hi * c_;
        if (s == 0) return {v, v, v};
        const double six = h * 6, i = std::floor(six), f = six - i;
        const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
        }
    }
    default:
        for (unsigned c = 0; c < 3; ++c) {
            if (channel_ >= 0 && static_cast<int>(c) != channel_) continue;
            out[c] = through(u[c]) / 255;
        }
        return out;
    }
}

}  // namespace genko::core
