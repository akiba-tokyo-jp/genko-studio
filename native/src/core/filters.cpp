#include "core/filters.hpp"
#include "core/exposure.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// max(0, min(255, round(x))): round() of NaN and of an infinity raise; of any other value it is an int (unbounded)
int round_clamp255(double x) {
    const double whole = py_round_whole(x);
    return whole > 255 ? 255 : whole < 0 ? 0 : static_cast<int>(whole);
}

// int(x) where Python goes on to compare, clamp or add the int: its value as a whole double (Python's int has no
// bound; past 2**53 the nearest double). Python's errors: int()'s (to_int's), and for a float ValueError (NaN) and
// OverflowError (an infinity).
double whole_int(const Json& value) {
    if (value.is_number_float()) return py_trunc(value.get<double>());
    if (const auto big = py_big_int_text(value)) return std::strtod(big->c_str(), nullptr);
    return static_cast<double>(to_int(value));
}

// [int(v) for v in x][:3] (every value converted, the first three kept)
std::vector<double> colour_of(const Json& value) {
    std::vector<double> out;
    for (const Json& v : iterate(value)) out.push_back(whole_int(v));
    if (out.size() > 3) out.resize(3);
    return out;
}

// Tables of round(v) values that Python gives Image.point as they are (not kept in 0..255 first), read one channel
// after the other as Pillow reads them: each a C long (OverflowError past one) of which an int keeps the low 32 bits;
// point() then keeps that in 0..255.
std::array<std::vector<int>, 3> lut_tables(const std::array<std::vector<double>, 3>& wholes) {
    std::array<std::vector<int>, 3> out;
    for (std::size_t ch = 0; ch < 3; ++ch) {
        for (const double whole : wholes[ch]) {
            if (whole >= 9223372036854775808.0 || whole < -9223372036854775808.0) {
                throw PyUncaught("OverflowError", "Python int too large to convert to C long");
            }
            out[ch].push_back(static_cast<int>(static_cast<std::int64_t>(whole)));  // (modulo 2**32, as C converts it)
        }
    }
    return out;
}

std::vector<int> identity() {
    std::vector<int> out(256);
    for (int i = 0; i < 256; ++i) out[static_cast<std::size_t>(i)] = i;
    return out;
}

Adjustment rgb(std::vector<int> table) {
    Adjustment out;
    out.tables = {table, table, std::move(table)};
    return out;
}

// _by_channel(rgba, table, str(params.get("channel") or "rgb")): the table through all three colours, or one of them
Adjustment by_channel(std::vector<int> table, const Json& params) {
    const Json* given = get(params, "channel");
    const std::string channel = given != nullptr && py_truthy(*given) ? py_str(*given) : std::string("rgb");
    if (channel != "r" && channel != "g" && channel != "b") return rgb(std::move(table));
    Adjustment out;
    for (std::size_t c = 0; c < 3; ++c) out.tables[c] = channel == std::string(1, "rgb"[c]) ? table : identity();
    return out;
}

// `x, y = item`
std::pair<Json, Json> unpack_two(const Json& item) {
    if (!(item.is_array() || item.is_string() || item.is_object())) {
        throw PyTypeError("cannot unpack non-iterable " + py_type_name(item) + " object");
    }
    const std::vector<Json> items = iterate(item);
    if (items.size() < 2) {
        throw PyValueError("not enough values to unpack (expected 2, got " + std::to_string(items.size()) + ")");
    }
    if (items.size() > 2) throw PyValueError("too many values to unpack (expected 2)");
    return {items[0], items[1]};
}

// filters.levels_table(black, white, gamma, out_black, out_white)
std::vector<int> levels_table(const Json& black_in, const Json& white_in, const Json& gamma_in, const Json& out_black_in,
                              const Json& out_white_in) {
    const double black = to_float(black_in);
    const double white = py_max(to_float(black_in) + 1, to_float(white_in));
    const double gamma = py_max(0.1, py_min(9.99, to_float(gamma_in)));
    // (Python reads out_black and out_white at the first value, after its t: a black past 2**53 fails before them)
    if (white - black == 0.0) throw PyUncaught("ZeroDivisionError", "float division by zero");
    const double out_black = to_float(out_black_in);
    const double out_white = to_float(out_white_in);
    std::vector<int> table;
    for (int p = 0; p < 256; ++p) {
        double t = py_min(1.0, py_max(0.0, (p - black) / (white - black)));
        t = py_pow(t, 1 / gamma);
        table.push_back(round_clamp255(out_black + t * (out_white - out_black)));
    }
    return table;
}

// filters.brightness_contrast_table(brightness, contrast)
std::vector<int> brightness_contrast_table(const Json& brightness, const Json& contrast) {
    const double b = py_max(-100.0, py_min(100.0, to_float(brightness))) * 1.275;
    const double c = py_max(-100.0, py_min(100.0, to_float(contrast)));
    const double k = c <= 0 ? 1 + c / 100 : 1 / py_max(0.01, 1 - c / 100 * 0.99);
    std::vector<int> table;
    for (int p = 0; p < 256; ++p) table.push_back(round_clamp255((p - 127.5) * k + 127.5 + b));
    return table;
}

// filters.curve_table(points): a monotone cubic (Fritsch–Carlson) through the points
std::vector<int> curve_table(const Json& points) {
    // sorted({int(round(float(x))): float(y) for x, y in points}.items()): later points win (the ints as the whole
    // floats they are)
    std::vector<std::pair<double, double>> pts;
    for (const Json& item : iterate(points)) {
        const auto [x_value, y_value] = unpack_two(item);
        const double x = py_round_whole(to_float(x_value));
        const double y = to_float(y_value);
        const auto at = std::find_if(pts.begin(), pts.end(), [x](const auto& p) { return p.first == x; });
        if (at != pts.end()) {
            at->second = y;
        } else {
            pts.emplace_back(x, y);
        }
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (pts.size() < 2) throw PyValueError("a tone curve needs two points or more");
    std::vector<double> xs;
    std::vector<double> ys;
    for (const auto& [x, y] : pts) {
        xs.push_back(x);
        ys.push_back(py_max(0.0, py_min(255.0, y)));
    }
    const std::size_t n = xs.size();
    std::vector<double> d;
    for (std::size_t k = 0; k + 1 < n; ++k) d.push_back((ys[k + 1] - ys[k]) / py_max(1e-9, xs[k + 1] - xs[k]));
    std::vector<double> m{d[0]};
    for (std::size_t k = 1; k + 1 < n; ++k) m.push_back(d[k - 1] * d[k] <= 0 ? 0.0 : (d[k - 1] + d[k]) / 2);
    m.push_back(d.back());
    for (std::size_t k = 0; k + 1 < n; ++k) {
        if (d[k] == 0) {
            m[k] = m[k + 1] = 0.0;
            continue;
        }
        const double a = m[k] / d[k];
        const double b = m[k + 1] / d[k];
        if (a * a + b * b > 9) {
            const double t = 3 / std::sqrt(a * a + b * b);
            m[k] = t * a * d[k];
            m[k + 1] = t * b * d[k];
        }
    }
    std::vector<int> table;
    for (int i = 0; i < 256; ++i) {
        if (i <= xs[0]) {
            table.push_back(round_clamp255(ys[0]));  // (ys are in 0..255)
            continue;
        }
        if (i >= xs.back()) {
            table.push_back(round_clamp255(ys.back()));
            continue;
        }
        std::size_t k = 0;
        while (!(xs[k] <= i && i <= xs[k + 1])) ++k;
        const double h = xs[k + 1] - xs[k];
        const double t = (i - xs[k]) / h;
        const double h00 = 2 * py_pow(t, 3.0) - 3 * py_pow(t, 2.0) + 1;
        const double h10 = py_pow(t, 3.0) - 2 * py_pow(t, 2.0) + t;
        const double h01 = -2 * py_pow(t, 3.0) + 3 * py_pow(t, 2.0);
        const double h11 = py_pow(t, 3.0) - py_pow(t, 2.0);
        const double value = h00 * ys[k] + h10 * h * m[k] + h01 * ys[k + 1] + h11 * h * m[k + 1];
        table.push_back(round_clamp255(value));
    }
    return table;
}

// numpy.linspace(0, 1, 256)
std::vector<double> linspace01() {
    std::vector<double> out(256);
    const double step = 1.0 / 255;
    for (int i = 0; i < 256; ++i) out[static_cast<std::size_t>(i)] = static_cast<double>(i) * step + 0.0;
    out[255] = 1.0;
    return out;
}

Adjustment gradient_map(const Json& params) {
    // the picture's lightness mapped to a row of colours (dark → first)
    Adjustment out;
    out.way = Adjustment::Way::Grey;
    std::array<std::vector<double>, 3> wholes;  // (round(v) of each value)
    if (const Json* stops = get(params, "stops"); stops != nullptr && py_truthy(*stops)) {
        // sorted((float(st[0]), [int(v) for v in st[1]][:3]) for st in params["stops"])
        std::vector<std::pair<double, std::vector<double>>> placed;
        for (const Json& st : iterate(*stops)) {
            const double at = to_float(subscript(st, 0));
            placed.emplace_back(at, colour_of(subscript(st, 1)));
        }
        std::stable_sort(placed.begin(), placed.end());
        if (placed.size() < 2) throw PyValueError("a gradient map needs two colours or more");
        const std::vector<double> xs = linspace01();
        std::vector<double> positions;
        for (const auto& p : placed) positions.push_back(p.first);
        for (std::size_t ch = 0; ch < 3; ++ch) {
            std::vector<double> fp;
            for (const auto& p : placed) {
                if (p.second.size() <= ch) throw PyUncaught("IndexError", "list index out of range");
                fp.push_back(p.second[ch]);
            }
            const NumpyInterp interp(positions, fp);
            for (const double x : xs) wholes[ch].push_back(py_round_whole(interp(x)));
        }
        out.tables = lut_tables(wholes);
        return out;
    }
    // [tuple(int(v) for v in c)[:3] for c in params.get("colors") or [[0, 0, 0], [255, 255, 255]]]
    const Json* colors = get(params, "colors");
    const Json list = colors != nullptr && py_truthy(*colors) ? *colors
                                                              : Json::array({Json::array({0, 0, 0}), Json::array({255, 255, 255})});
    std::vector<std::vector<double>> stops;
    for (const Json& c : iterate(list)) stops.push_back(colour_of(c));
    if (stops.size() < 2) throw PyValueError("a gradient map needs two colours or more");
    for (int i = 0; i < 256; ++i) {
        const double pos = i / 255.0 * static_cast<double>(stops.size() - 1);
        const std::size_t k = std::min(stops.size() - 2, static_cast<std::size_t>(py_trunc_int(pos)));
        const double t = pos - static_cast<double>(k);
        for (std::size_t c = 0; c < 3; ++c) {
            if (stops[k].size() <= c || stops[k + 1].size() <= c) throw PyUncaught("IndexError", "tuple index out of range");
            const double a = stops[k][c];
            const double b = stops[k + 1][c];
            wholes[c].push_back(py_round_whole(a + (b - a) * t));
        }
    }
    out.tables = lut_tables(wholes);
    return out;
}

}  // namespace

Adjustment adjustment(std::string_view kind, const Json& params) {
    if (kind == "exposure") {
        const auto exposure = Exposure::parse(params);
        Adjustment out;
        for (int i = 0; i < 256; ++i) {
            const int value = static_cast<int>(std::lround(linear_to_srgb(exposure.apply(srgb_to_linear(i/255.0)))*255));
            for (auto& table : out.tables) table.push_back(value);
        }
        return out;
    }
    if (kind == "hue") {
        // int(float(params.get("shift", 30))) % 360 (a whole float is an int's value: its float % 360 is exact)
        const double shift_float = to_float(get_or(params, "shift", Json(30)));
        const double whole = py_trunc(shift_float);
        const auto shift = static_cast<std::int64_t>(py_fmod(whole, 360.0));
        const double sat = py_max(0.0, to_float(get_or(params, "saturation", Json(1.0))));
        const double val = py_max(0.0, to_float(get_or(params, "value", Json(1.0))));
        const std::int64_t turn = py_round_int(static_cast<double>(shift) * 255 / 360);  // round(shift * 255 / 360)
        Adjustment out;
        out.way = Adjustment::Way::Hsv;
        // (Python makes the hue's table, then the saturation's, then the value's: min(255, round(p * …)), never below 0)
        for (int p = 0; p < 256; ++p) out.tables[0].push_back(static_cast<int>((p + turn) % 256));
        for (int p = 0; p < 256; ++p) out.tables[1].push_back(round_clamp255(p * sat));
        for (int p = 0; p < 256; ++p) out.tables[2].push_back(round_clamp255(p * val));
        return out;
    }
    if (kind == "brightness_contrast") {
        return by_channel(brightness_contrast_table(get_or(params, "brightness", Json(0)), get_or(params, "contrast", Json(0))),
                          params);
    }
    if (kind == "levels") {
        // any(params.get(k) is not None for k in ("gamma", "out_black", "out_white", "channel")): the table form
        static constexpr const char* kTableKeys[] = {"gamma", "out_black", "out_white", "channel"};
        const bool table_form = std::any_of(std::begin(kTableKeys), std::end(kTableKeys), [&](const char* key) {
            const Json* v = get(params, key);
            return v != nullptr && !v->is_null();
        });
        if (table_form) {
            return by_channel(levels_table(get_or(params, "black", Json(0)), get_or(params, "white", Json(255)),
                                           get_or(params, "gamma", Json(1.0)), get_or(params, "out_black", Json(0)),
                                           get_or(params, "out_white", Json(255))),
                              params);
        }
        const double black = whole_int(get_or(params, "black", Json(0)));
        const double white = whole_int(get_or(params, "white", Json(255)));
        // round((p - black) * 255 / span): Python's ints are exact; these doubles are the same while below 2**53
        const double span = py_max(1.0, white - black);
        std::vector<int> table;
        for (int p = 0; p < 256; ++p) {
            if (p <= black) {
                table.push_back(0);
            } else if (p >= white) {
                table.push_back(255);
            } else {
                table.push_back(static_cast<int>(py_round_int((p - black) * 255 / span)));
            }
        }
        return rgb(std::move(table));
    }
    if (kind == "curve") {
        if (const Json* points = get(params, "points"); points != nullptr && py_truthy(*points)) {
            return by_channel(curve_table(*points), params);
        }
        // a gamma curve: > 1 darkens the middle tones, < 1 lightens them; black and white stay
        const double gamma = py_max(0.2, py_min(5.0, to_float(get_or(params, "gamma", Json(1.6)))));
        std::vector<int> table;
        for (int i = 0; i < 256; ++i) table.push_back(static_cast<int>(py_round_int(255 * py_pow(i / 255.0, gamma))));
        return rgb(std::move(table));
    }
    if (kind == "bitonal" || kind == "threshold") {
        // bitonal: 255 above the threshold (180); threshold: 255 from the threshold (128)
        const bool bitonal = kind == "bitonal";
        const double cut = whole_int(get_or(params, "threshold", Json(bitonal ? 180 : 128)));
        std::vector<int> table;
        for (int p = 0; p < 256; ++p) table.push_back((bitonal ? p > cut : p >= cut) ? 255 : 0);
        Adjustment out = rgb(std::move(table));
        out.way = Adjustment::Way::Grey;
        return out;
    }
    if (kind == "invert") {
        std::vector<int> table;
        for (int p = 0; p < 256; ++p) table.push_back(255 - p);
        return rgb(std::move(table));
    }
    if (kind == "posterize") {
        const double levels = py_max(2.0, py_min(64.0, whole_int(get_or(params, "levels", Json(4)))));
        const double step = 255.0 / (levels - 1);
        std::vector<int> table;
        for (int i = 0; i < 256; ++i) {
            table.push_back(static_cast<int>(py_round_int(static_cast<double>(py_round_int(i / step)) * step)));
        }
        return rgb(std::move(table));
    }
    if (kind == "gradient_map") return gradient_map(params);
    throw PyValueError("unknown filter " + std::string(kind));
}

NumpyInterp::NumpyInterp(std::vector<double> xp, std::vector<double> fp) : xp_(std::move(xp)), fp_(std::move(fp)) {
    if (xp_.empty()) throw Error("value", "array of sample points is empty");
    if (xp_.size() != fp_.size()) throw Error("value", "fp and xp are not of the same length.");
    for (std::size_t i = 0; i + 1 < xp_.size(); ++i) slopes_.push_back((fp_[i + 1] - fp_[i]) / (xp_[i + 1] - xp_[i]));
}

double NumpyInterp::operator()(double x) const {
    const std::size_t n = xp_.size();
    if (n == 1) return fp_[0];
    if (std::isnan(x)) return x;
    // the last sample point at or before x (sorted points)
    if (x > xp_[n - 1]) return fp_[n - 1];
    if (x < xp_[0]) return fp_[0];
    const std::size_t j = static_cast<std::size_t>(std::upper_bound(xp_.begin(), xp_.end(), x) - xp_.begin()) - 1;
    if (j == n - 1) return fp_[j];
    if (xp_[j] == x) return fp_[j];
    const double slope = slopes_[j];
    double res = slope * (x - xp_[j]) + fp_[j];
    if (std::isnan(res)) {
        res = slope * (x - xp_[j + 1]) + fp_[j + 1];
        if (std::isnan(res) && fp_[j] == fp_[j + 1]) res = fp_[j];
    }
    return res;
}

}  // namespace genko::core
