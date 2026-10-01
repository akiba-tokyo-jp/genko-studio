// Python's genko/brushes.py.

#include "render/brushes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "render/draw.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/stroke.hpp"

namespace genko::render::brushes {

namespace {

using core::Json;

constexpr double kDegToRad = core::kPi / 180.0;
constexpr double kRadToDeg = 180.0 / core::kPi;
constexpr double kTau = 6.283185307179586;  // math.tau

double pmax(double a, double b) { return b > a ? b : a; }
double pmin(double a, double b) { return b < a ? b : a; }

// Python's round(x) as an int (ties to even), with Python's errors for nan and inf.
std::int64_t py_round_int(double x) {
    if (std::isnan(x)) throw core::Error("value", "cannot convert float NaN to integer");
    if (std::isinf(x)) throw core::Error("value", "cannot convert float infinity to integer");
    const double r = std::nearbyint(x);
    if (r >= 9.2e18 || r <= -9.2e18) throw core::Error("value", "integer out of range");
    return static_cast<std::int64_t>(r);
}

// Python's int(x) for a float.
double py_trunc(double x) {
    if (std::isnan(x)) throw core::Error("value", "cannot convert float NaN to integer");
    if (std::isinf(x)) throw core::Error("value", "cannot convert float infinity to integer");
    return std::trunc(x);
}

int clamp_int(double v) {
    if (v > 2147483647.0) return 2147483647;
    if (v < -2147483648.0) return -2147483647 - 1;
    return static_cast<int>(v);
}

Brush make(std::string key, std::string label, double width_mm) {
    Brush b;
    b.key = std::move(key);
    b.label = std::move(label);
    b.width_mm = width_mm;
    return b;
}

std::vector<Brush> make_builtin() {
    std::vector<Brush> out;
    {
        Brush b = make("gpen", "G ペン", 0.5);
        b.min_pressure = 0.1;
        b.gamma = 1.4;
        b.stabilize = 3;
        b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("maru", "丸ペン", 0.25);
        b.min_pressure = 0.2;
        b.gamma = 1.2;
        b.stabilize = 3;
        b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("kabura", "かぶらペン", 0.6);
        b.min_pressure = 0.35;
        b.gamma = 1.0;
        b.stabilize = 2;
        b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("mili", "ミリペン", 0.3);
        b.fixed_width = true;
        b.stabilize = 4;
        b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("pencil", "鉛筆", 0.5);
        b.min_pressure = 0.4;
        b.gamma = 1.0;
        b.opacity = 0.85;
        b.stabilize = 1;
        b.taper = false;
        b.texture = "grain";
        out.push_back(b);
    }
    {
        Brush b = make("fude", "筆", 1.6);
        b.min_pressure = 0.05;
        b.gamma = 1.8;
        b.stabilize = 2;
        b.taper = true;
        b.texture = "dry";
        out.push_back(b);
    }
    {
        Brush b = make("marker", "マーカー", 1.5);
        b.fixed_width = true;
        b.opacity = 0.6;
        b.stabilize = 2;
        b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("airbrush", "エアブラシ", 8.0);
        b.min_pressure = 0.5;
        b.opacity = 0.5;
        b.stabilize = 1;
        b.taper = false;
        b.texture = "soft";
        out.push_back(b);
    }
    {
        Brush b = make("fill_pen", "ベタ塗りペン", 3.0);
        b.fixed_width = true;
        b.stabilize = 1;
        b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("white", "ホワイト（修正）", 1.0);
        b.min_pressure = 0.3;
        b.stabilize = 2;
        b.taper = false;
        b.rgb = std::vector<std::int64_t>{255, 255, 255};
        out.push_back(b);
    }
    {
        Brush b = make("fx", "効果線ペン", 0.5);
        b.min_pressure = 0.0;
        b.gamma = 1.0;
        b.stabilize = 0;
        b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("calligraphy", "カリグラフィ（平たいペン先）", 1.6);
        b.min_pressure = 0.5;
        b.stabilize = 3;
        b.taper = false;
        b.tip = "flat";
        b.tip_angle = 35;
        b.tip_rotation = true;
        b.tip_ratio = 0.22;
        out.push_back(b);
    }
    {
        Brush b = make("water", "水彩", 4.0);
        b.min_pressure = 0.4;
        b.opacity = 0.55;
        b.stabilize = 2;
        b.taper = false;
        b.texture = "water";
        out.push_back(b);
    }
    {
        Brush b = make("spray", "スプレー", 8.0);
        b.min_pressure = 0.5;
        b.stabilize = 1;
        b.taper = false;
        b.pattern = "dots";
        b.spacing = 0.08;
        b.scatter = 0.5;
        b.size_jitter = 0.6;
        b.count = 6;
        b.stamp_size = 0.06;
        out.push_back(b);
    }
    {
        Brush b = make("stipple", "点描", 3.0);
        b.min_pressure = 0.5;
        b.stabilize = 1;
        b.taper = false;
        b.pattern = "dots";
        b.spacing = 0.35;
        b.scatter = 0.45;
        b.size_jitter = 0.5;
        b.count = 2;
        b.stamp_size = 0.2;
        out.push_back(b);
    }
    {
        Brush b = make("dotline", "点線", 0.8);
        b.fixed_width = true;
        b.stabilize = 4;
        b.taper = false;
        b.pattern = "dots";
        b.spacing = 2.2;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("dashline", "破線", 0.5);
        b.fixed_width = true;
        b.stabilize = 4;
        b.taper = false;
        b.pattern = "dash";
        b.spacing = 5.0;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("lace", "レース", 3.0);
        b.fixed_width = true;
        b.stabilize = 4;
        b.taper = false;
        b.pattern = "lace";
        b.spacing = 1.0;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("grass", "草むら", 6.0);
        b.min_pressure = 0.5;
        b.stabilize = 2;
        b.taper = false;
        b.pattern = "grass";
        b.spacing = 0.22;
        b.size_jitter = 0.5;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("leaves", "木の葉", 5.0);
        b.min_pressure = 0.5;
        b.stabilize = 2;
        b.taper = false;
        b.pattern = "leaves";
        b.spacing = 0.45;
        b.scatter = 0.6;
        b.size_jitter = 0.5;
        b.turn_jitter = true;
        b.count = 2;
        b.stamp_size = 0.6;
        out.push_back(b);
    }
    {
        Brush b = make("hearts", "ハート", 3.0);
        b.fixed_width = true;
        b.stabilize = 3;
        b.taper = false;
        b.pattern = "hearts";
        b.spacing = 1.4;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("stars", "星", 3.0);
        b.fixed_width = true;
        b.stabilize = 3;
        b.taper = false;
        b.pattern = "stars";
        b.spacing = 1.5;
        b.turn_jitter = true;
        b.size_jitter = 0.3;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    return out;
}

// CUSTOM: the registered brushes, in the order they were first registered (Python's dict).
std::shared_mutex g_custom_mutex;
std::vector<Brush> g_custom;

const Brush* find_builtin(std::string_view key) {
    for (const Brush& b : builtin()) {
        if (b.key == key) return &b;
    }
    return nullptr;
}

const char* const kTextures[] = {"", "grain", "soft", "dry", "water"};
const char* const kTips[] = {"round", "flat", "image"};
const char* const kPatterns[] = {"", "dots", "dash", "lace", "grass", "hearts", "stars", "leaves"};
const char* const kAas[] = {"none", "weak", "normal", "strong"};

template <std::size_t N>
bool one_of(const Json& value, const char* const (&choices)[N]) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    for (const char* c : choices) {
        if (text == c) return true;
    }
    return false;
}

const char* const kJ3Keys[] = {"tip", "tip_angle", "tip_ratio", "tip_follow", "tip_rotation", "tip_png", "spacing",
                               "scatter", "size_jitter", "turn_jitter", "count", "pattern", "speed", "post_smooth",
                               "aa", "stamp_size", "mix", "stretch"};

Json j3_value(const Brush& b, std::string_view key) {
    if (key == "tip") return b.tip;
    if (key == "tip_angle") {
        // (the built-in calligraphy pen says tip_angle=35: an int in Python)
        if (b.key == "calligraphy" && find_builtin("calligraphy") != nullptr && b == *find_builtin("calligraphy")) {
            return static_cast<std::int64_t>(b.tip_angle);
        }
        return b.tip_angle;
    }
    if (key == "tip_ratio") return b.tip_ratio;
    if (key == "tip_follow") return b.tip_follow;
    if (key == "tip_rotation") return b.tip_rotation;
    if (key == "tip_png") return b.tip_png;
    if (key == "spacing") return b.spacing;
    if (key == "scatter") return b.scatter;
    if (key == "size_jitter") return b.size_jitter;
    if (key == "turn_jitter") return b.turn_jitter;
    if (key == "count") return b.count;
    if (key == "pattern") return b.pattern;
    if (key == "speed") return b.speed;
    if (key == "post_smooth") return b.post_smooth;
    if (key == "aa") return b.aa;
    if (key == "stamp_size") return b.stamp_size;
    if (key == "mix") return b.mix;
    return b.stretch;
}

// Python's str.isspace() for one code point.
bool py_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

// The code points of UTF-8 text with where each starts.
std::vector<std::pair<char32_t, std::size_t>> code_points(const std::string& s) {
    std::vector<std::pair<char32_t, std::size_t>> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > s.size()) n = s.size() - i;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.emplace_back(cp, i);
        i += n;
    }
    return out;
}

// str(label).strip()[:40]
std::string clean_label(const std::string& text) {
    const auto cps = code_points(text);
    std::size_t first = 0;
    std::size_t last = cps.size();
    while (first < last && py_space(cps[first].first)) ++first;
    while (last > first && py_space(cps[last - 1].first)) --last;
    if (last - first > 40) last = first + 40;
    if (first == last) return {};
    const std::size_t begin = cps[first].second;
    const std::size_t end = last < cps.size() ? cps[last].second : text.size();
    return text.substr(begin, end - begin);
}

// --- drawing --------------------------------------------------------------------------------------------------------

struct PPoint {
    double x, y, p;
};

// _pressured: each point's pressure through the brush's curve (and thinned where the hand was quick).
std::vector<PPoint> pressured(const core::PenPoints& points, const Brush& b) {
    std::vector<PPoint> out;
    out.reserve(points.size());
    for (const core::PenPoint& pt : points) {
        if (b.fixed_width || !pt.p) {
            out.push_back(PPoint{pt.x, pt.y, 1.0});
            continue;
        }
        const double p = core::py_pow(pmax(0.0, pmin(1.0, *pt.p)), b.gamma);
        out.push_back(PPoint{pt.x, pt.y, b.min_pressure + (1 - b.min_pressure) * p});
    }
    if (b.speed > 0 && out.size() > 2) {
        std::vector<double> steps;
        for (std::size_t i = 0; i + 1 < out.size(); ++i) {
            steps.push_back(core::py_dist(out[i].x, out[i].y, out[i + 1].x, out[i + 1].y));
        }
        double usual = core::py_median(steps);
        if (usual == 0.0) usual = 1e-6;
        std::vector<PPoint> thin;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double step = steps[std::min(i, steps.size() - 1)];
            const double quick = pmax(0.0, pmin(1.0, (step / usual - 1.0) / 3.0));
            thin.push_back(PPoint{out[i].x, out[i].y, out[i].p * (1 - 0.75 * b.speed * quick)});
        }
        out = std::move(thin);
    }
    return out;
}

bool stamped(const Brush& b) {
    return !b.pattern.empty() || b.tip == "flat" || b.tip == "image" || b.scatter > 0 || b.spacing > 0;
}

// _decode_tip: the ink of an image tip (its alpha, or its darkness), or nothing for a picture that cannot be opened.
std::optional<Image> decode_tip(const std::string& data) {
    std::string bytes;
    try {
        bytes = core::a2b_base64(data);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    Image image;
    try {
        image = open_image(bytes, kPillowOpenLimits);
    } catch (const core::Error& e) {
        // (Python catches what Image.open raises: a file it cannot identify, or one too large; a picture that breaks
        // while it is decoded fails the draw)
        if (e.code() == "unidentified_image" || e.code() == "image_too_large") return std::nullopt;
        throw;
    }
    const std::string_view mode = image.mode();
    if ((mode == "RGBA" || mode == "LA") && image.getextrema().back().first < 255) return image.split().back();
    Image grey = image.convert("L");
    long long total = 0;
    const std::vector<long> hist = grey.histogram();
    for (int v = 0; v < 256; ++v) total += static_cast<long long>(hist[static_cast<std::size_t>(v)]) * v;
    const long long n = std::max<long long>(1, static_cast<long long>(grey.width()) * grey.height());
    if (static_cast<double>(total) / static_cast<double>(n) > 127) return chops::invert(grey);
    return grey;
}

// _shape: a stamp's shape ("L", ink 255), pointing along +x.
Image shape(const std::string& pattern, std::int64_t size_in, const Brush& b) {
    const int n = static_cast<int>(std::max<std::int64_t>(3, size_in));
    Image tile = Image::create("L", Size{pattern == "dash" ? n * 3 : n, n}, Ink(0));
    Draw d(tile);
    const double w = tile.width();
    const double h = tile.height();
    if (pattern == "dash") {
        d.rectangle(BoxF{0, h * 0.3, w - 1, h * 0.7}, Ink(255));
    } else if (pattern == "lace") {
        d.arc(BoxF{0, 0, w - 1, h * 2 - 1}, 180, 360, Ink(255), std::max(1, n / 8));
        const double r = std::max(1, n / 10);
        d.ellipse(BoxF{w / 2 - r, h * 0.55 - r, w / 2 + r, h * 0.55 + r}, Ink(255));
    } else if (pattern == "grass") {
        const std::vector<PointD> blade{{w * 0.35, h - 1}, {w * 0.65, h - 1}, {w * 0.55, 0}};
        d.polygon(blade, Ink(255));
    } else if (pattern == "leaves") {
        d.ellipse(BoxF{0, h * 0.3, w - 1, h * 0.7}, Ink(255));
        const std::vector<PointD> vein{{w * 0.1, h / 2}, {w * 0.95, h / 2}};
        d.line(vein, Ink(90), std::max(1, n / 16));
    } else if (pattern == "hearts") {
        const double r = w / 4;
        d.ellipse(BoxF{w * 0.05, h * 0.1, w * 0.05 + 2 * r, h * 0.1 + 2 * r}, Ink(255));
        d.ellipse(BoxF{w * 0.95 - 2 * r, h * 0.1, w * 0.95, h * 0.1 + 2 * r}, Ink(255));
        const std::vector<PointD> tip{{w * 0.07, h * 0.38}, {w * 0.93, h * 0.38}, {w / 2, h * 0.95}};
        d.polygon(tip, Ink(255));
    } else if (pattern == "stars") {
        std::vector<PointD> pts;
        for (int k = 0; k < 10; ++k) {
            const double r = (w / 2) * (k % 2 == 0 ? 1.0 : 0.42);
            const double a = -core::kPi / 2 + k * core::kPi / 5;
            pts.push_back({w / 2 + r * core::py_cos(a), h / 2 + r * core::py_sin(a)});
        }
        d.polygon(pts, Ink(255));
    } else if (b.tip == "flat") {
        const double thin = pmax(1.0, h * b.tip_ratio);
        d.ellipse(BoxF{0, h / 2 - thin / 2, w - 1, h / 2 + thin / 2}, Ink(255));
    } else {
        d.ellipse(BoxF{0, 0, w - 1, h - 1}, Ink(255));
    }
    return tile;
}

// _draw_stamps: the brush's stamp laid along the line (points in px with their radius).
void draw_stamps(Image& mask, const std::vector<PPoint>& pts, const Brush& b, int dpi, double width_mm,
                 std::string_view seed, std::span<const double> rotation) {
    core::PyRandom rng = core::PyRandom::from_str(seed.empty() ? std::string_view("genko") : seed);
    const double width_px = width_mm * dpi / 25.4;
    const double step = pmax(1.0, (b.spacing != 0.0 ? b.spacing : 0.1) * width_px);
    std::optional<Image> tip;
    if (b.tip == "image" && !b.tip_png.empty()) tip = decode_tip(b.tip_png);
    std::map<std::pair<std::int64_t, std::int64_t>, Image> cache;

    struct Position {
        double x, y, r, direction, turn;
    };
    std::vector<Position> positions;
    double carry = 0.0;
    const bool has_turns = !rotation.empty() && rotation.size() == pts.size();
    const std::size_t pairs = pts.size() > 1 ? pts.size() - 1 : pts.size();
    for (std::size_t i = 0; i < pairs; ++i) {
        const PPoint& a = pts[i];
        const PPoint& e = pts.size() > 1 ? pts[i + 1] : pts[i];
        const double length = core::py_hypot(e.x - a.x, e.y - a.y);
        const double direction = length > 1e-6 ? core::py_atan2(e.y - a.y, e.x - a.x) : 0.0;
        const double ta = has_turns ? rotation[i] : 0.0;
        const double tb = has_turns ? rotation[std::min(i + 1, rotation.size() - 1)] : 0.0;
        double t = carry;
        while (t <= length) {
            const double f = length > 1e-6 ? t / length : 0.0;
            positions.push_back(Position{a.x + (e.x - a.x) * f, a.y + (e.y - a.y) * f, a.p + (e.p - a.p) * f, direction,
                                         ta + (tb - ta) * f});
            t += step;
        }
        carry = t - length;
        if (length <= 1e-6) break;
    }
    for (const Position& pos : positions) {
        for (std::int64_t k = 0; k < b.count; ++k) {
            const double size = pmax(1.5, 2 * pos.r * b.stamp_size * (1 - b.size_jitter * rng.random()));
            double ox = 0.0;
            double oy = 0.0;
            if (b.scatter != 0.0) {
                const double spread = b.scatter * width_px;
                ox = rng.gauss(0, spread / 2);
                oy = rng.gauss(0, spread / 2);
            }
            double angle = 0.0;
            if (b.pattern == "grass") {
                angle = rng.uniform(-0.35, 0.35);  // blades stand up, a little astray
            } else if (b.turn_jitter) {
                angle = rng.uniform(0, kTau);
            } else if (b.tip == "flat" && b.tip_rotation && has_turns) {
                angle = (b.tip_angle + pos.turn) * kDegToRad;  // (the nib turns as the pen is turned in the hand)
            } else if (b.tip == "flat" && !b.tip_follow) {
                angle = b.tip_angle * kDegToRad;
            } else if (b.pattern == "dash" || b.pattern == "lace" || b.pattern == "leaves" || b.tip_follow) {
                angle = pos.direction;
            }
            const std::pair<std::int64_t, std::int64_t> key{py_round_int(size), py_round_int(angle * kRadToDeg / 3)};
            const Image* stamp = nullptr;
            Image made;
            const auto found = cache.find(key);
            if (found != cache.end()) {
                stamp = &found->second;
            } else {
                Image base;
                if (tip) {
                    const auto bw = std::max<std::int64_t>(1, py_round_int(size));
                    const auto bh = std::max<std::int64_t>(1, py_round_int(size * tip->height() / std::max(1, tip->width())));
                    base = tip->resize(Size{static_cast<int>(bw), static_cast<int>(bh)});
                } else {
                    base = shape(b.pattern, std::max<std::int64_t>(3, py_round_int(size)), b);
                }
                made = angle != 0.0 ? base.rotate(-(angle * kRadToDeg), Resample::Bicubic, true) : std::move(base);
                if (cache.size() < 400) {
                    stamp = &cache.emplace(key, std::move(made)).first->second;
                } else {
                    stamp = &made;
                }
            }
            const auto px = py_round_int(pos.x + ox - stamp->width() / 2.0);
            const auto py = py_round_int(pos.y + oy - stamp->height() / 2.0);
            const Box box{clamp_int(static_cast<double>(px)), clamp_int(static_cast<double>(py)),
                          clamp_int(static_cast<double>(px + stamp->width())), clamp_int(static_cast<double>(py + stamp->height()))};
            if (box.x1 <= 0 || box.y1 <= 0 || box.x0 >= mask.width() || box.y0 >= mask.height()) continue;
            mask.paste(chops::lighter(mask.crop(box), *stamp), Point{box.x0, box.y0});
        }
    }
}

// _finish_edges: 水彩 edges and the anti-aliasing choice.
Image finish_edges(Image mask, const Brush& b, int dpi) {
    if (b.texture == "water") {
        const std::int64_t size = std::max<std::int64_t>(3, (py_round_int(dpi / 40.0) / 2) * 2 + 1);
        const Image inner = mask.filter(Filter::min_filter(static_cast<int>(size)));
        const Image rim = chops::subtract(mask, inner);
        mask = chops::add(mask.point([](int v) { return v * 45 / 100; }), rim.point([](int v) { return v * 90 / 100; }));
    }
    if (b.aa == "none") {
        mask = mask.point([](int v) { return v >= 128 ? 255 : 0; });
    } else if (b.aa == "weak") {
        mask = mask.point([](int v) { return v < 64 ? 0 : v > 192 ? 255 : (v - 64) * 2; });
    } else if (b.aa == "strong") {
        mask = mask.filter(Filter::gaussian_blur(pmax(0.6, dpi / 300.0)));
    }
    return mask;
}

struct Extent {
    Box box;
    std::vector<PPoint> pts;
    double scale = 0.0;
};

std::optional<Extent> extent_of(const Brush& b, Size size, const core::PenPoints& points, int dpi, double width_mm) {
    Extent e;
    e.pts = pressured(points, b);
    if (e.pts.empty()) return std::nullopt;
    e.scale = dpi / 25.4;
    double reach = 0.75;
    if (stamped(b)) {  // stamps reach further: long dashes, stars, spray thrown off the line
        reach = pmax(0.75, 0.75 * b.stamp_size * (b.pattern == "dash" ? 3 : 1.5) + 1.5 * b.scatter);
    }
    const double pad = width_mm * e.scale * (b.texture == "soft" ? 1.5 : reach) + 3;
    // min()/max() of the coordinates (Python keeps the first of equal values)
    double min_x = e.pts[0].x * e.scale;
    double max_x = min_x;
    double min_y = e.pts[0].y * e.scale;
    double max_y = min_y;
    for (const PPoint& p : e.pts) {
        const double x = p.x * e.scale;
        const double y = p.y * e.scale;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    const double x0 = pmax(0.0, py_trunc(min_x - pad));
    const double y0 = pmax(0.0, py_trunc(min_y - pad));
    const double x1 = pmin(static_cast<double>(size.width), py_trunc(max_x + pad) + 1);
    const double y1 = pmin(static_cast<double>(size.height), py_trunc(max_y + pad) + 1);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    e.box = Box{clamp_int(x0), clamp_int(y0), clamp_int(x1), clamp_int(y1)};
    return e;
}

// _shade: how dark each part of the line is (full where pressed hard, lighter where the touch was light).
Image shade(const core::PenPoints& points, Size size, Point origin, int dpi, double width_mm, double amount) {
    const double scale = dpi / 25.4;
    Image out = Image::create("L", size, Ink(255));
    Draw d(out);
    const double reach = pmax(2.0, width_mm * scale * 1.6);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const core::PenPoint& a = points[i];
        const core::PenPoint& b = points[i + 1];
        const double ax = a.x * scale - origin.x;
        const double ay = a.y * scale - origin.y;
        const double bx = b.x * scale - origin.x;
        const double by = b.y * scale - origin.y;
        const double pressure = pmax(0.0, pmin(1.0, (a.p.value_or(1.0) + b.p.value_or(1.0)) / 2));
        const auto level = static_cast<int>(py_trunc(255 * (1 - amount * (1 - pressure))));
        const std::vector<PointD> seg{{ax, ay}, {bx, by}};
        d.line(seg, Ink(level), std::max(1, clamp_int(py_trunc(reach))));
        d.ellipse(BoxF{bx - reach / 2, by - reach / 2, bx + reach / 2, by + reach / 2}, Ink(level));
    }
    return out;
}

std::optional<Coverage> draw_plain(Size size, const core::PenPoints& points, int dpi, double width_mm,
                                   std::string_view kind, std::string_view seed, std::span<const double> rotation) {
    const Brush b = brush(kind);
    const auto e = extent_of(b, size, points, dpi, width_mm);
    if (!e) return std::nullopt;
    const double scale = e->scale;
    const Box box = e->box;
    core::PenPoints shift;
    shift.reserve(e->pts.size());
    for (const PPoint& p : e->pts) shift.push_back(core::PenPoint{p.x - box.x0 / scale, p.y - box.y0 / scale, p.p});
    Image mask = Image::create("L", Size{box.width(), box.height()}, Ink(0));
    if (stamped(b)) {
        std::vector<PPoint> radius;
        radius.reserve(shift.size());
        for (const core::PenPoint& p : shift) {
            radius.push_back(PPoint{p.x * scale, p.y * scale, pmax(0.5, width_mm * pmax(0.03, pmin(1.5, *p.p)) * scale / 2)});
        }
        draw_stamps(mask, radius, b, dpi, width_mm, seed, rotation);
        return Coverage{finish_edges(std::move(mask), b, dpi), Point{box.x0, box.y0}};
    }
    if (b.texture == "soft") {
        {
            Draw d(mask);
            draw_stroke_mm(d, shift, dpi, width_mm * 0.5, Ink(255));
        }
        return Coverage{mask.filter(Filter::gaussian_blur(pmax(1.0, width_mm * scale / 3))), Point{box.x0, box.y0}};
    }
    {
        Draw d(mask);
        draw_stroke_mm(d, shift, dpi, width_mm, Ink(255), true, b.min_pressure < 0.05 ? 0.03 : 0.15);
    }
    if (b.texture == "grain" || b.texture == "dry") {
        core::PyRandom rng = core::PyRandom::from_str(seed.empty() ? std::string_view("genko") : seed);
        const std::int64_t grain_px = b.texture == "grain" ? std::max<std::int64_t>(1, py_round_int(dpi / 150.0))
                                                           : std::max<std::int64_t>(1, py_round_int(dpi / 60.0));
        const int sw = static_cast<int>(std::max<std::int64_t>(1, mask.width() / grain_px));
        const int sh = static_cast<int>(std::max<std::int64_t>(1, mask.height() / grain_px));
        const double keep = b.texture == "grain" ? 0.72 : 0.9;
        std::string data(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh), '\0');
        for (char& v : data) v = static_cast<char>(rng.random() < keep ? 255 : 70);
        const Image small = Image::frombytes("L", Size{sw, sh}, data);
        mask = chops::multiply(mask, small.resize(mask.size(), Resample::Nearest));
    }
    return Coverage{finish_edges(std::move(mask), b, dpi), Point{box.x0, box.y0}};
}

}  // namespace

// --- the brushes ----------------------------------------------------------------------------------------------------

const std::vector<Brush>& builtin() {
    static const std::vector<Brush> all = make_builtin();
    return all;
}

Brush brush(std::string_view key) {
    // LEGACY.get(key or "", key or DEFAULT)
    std::string_view name = key.empty() ? kDefault : key;
    if (key == "oil") name = "marker";
    if (const Brush* b = find_builtin(name)) return *b;
    {
        std::shared_lock lock(g_custom_mutex);
        for (const Brush& b : g_custom) {
            if (b.key == name) return b;
        }
    }
    return *find_builtin(kDefault);
}

std::vector<Brush> everything() {
    std::vector<Brush> out = builtin();
    std::shared_lock lock(g_custom_mutex);
    out.insert(out.end(), g_custom.begin(), g_custom.end());
    return out;
}

Json to_dict(const Brush& b) {
    Json out = Json::object();
    out["label"] = b.label;
    out["width_mm"] = b.width_mm;
    out["min_pressure"] = b.min_pressure;
    out["gamma"] = b.gamma;
    out["opacity"] = b.opacity;
    out["stabilize"] = b.stabilize;
    out["taper"] = b.taper;
    out["texture"] = b.texture;
    if (b.rgb && !b.rgb->empty()) {
        out["rgb"] = *b.rgb;
    } else {
        out["rgb"] = nullptr;
    }
    out["fixed_width"] = b.fixed_width;
    const Brush plain = make("", "", 1.0);
    for (const char* key : kJ3Keys) {
        const Json mine = j3_value(b, key);
        const Json theirs = j3_value(plain, key);
        if (mine != theirs) out[key] = mine;
    }
    return out;
}

Brush from_dict(std::string_view key, const Json& data, const std::optional<std::string>& base) {
    if (!data.is_object()) throw core::Error("value", "brush settings must be a dict");
    // brush(base or data.get("base") or DEFAULT)
    Brush start_brush;
    if (base && !base->empty()) {
        start_brush = brush(*base);
    } else {
        const auto it = data.find("base");
        if (it != data.end() && core::py_truthy(*it)) {
            if (it->is_array() || it->is_object()) throw core::Error("value", "unhashable type: '" + core::py_type_name(*it) + "'");
            start_brush = it->is_string() ? brush(it->get<std::string>()) : brush(kDefault);
        } else {
            start_brush = brush(kDefault);
        }
    }
    const Brush plain = make("", "", 1.0);
    Json merged = Json::object();
    for (const char* k : kJ3Keys) merged[k] = j3_value(plain, k);
    const Json start = to_dict(start_brush);
    for (const auto& [k, v] : start.items()) merged[k] = v;
    for (const auto& [k, v] : data.items()) {
        if (merged.contains(k)) merged[k] = v;
    }

    struct Limit {
        const char* name;
        double lo;
        double hi;
    };
    static constexpr Limit kLimits[] = {
        {"width_mm", 0.05, 50.0}, {"min_pressure", 0.0, 1.0}, {"gamma", 0.2, 5.0},      {"opacity", 0.05, 1.0},
        {"stabilize", 0, 15},     {"tip_angle", -360.0, 360.0}, {"tip_ratio", 0.02, 1.0}, {"spacing", 0.0, 5.0},
        {"scatter", 0.0, 5.0},    {"size_jitter", 0.0, 1.0},  {"count", 1, 12},         {"speed", 0.0, 1.0},
        {"post_smooth", 0, 10},   {"stamp_size", 0.02, 3.0},  {"mix", 0.0, 1.0},        {"stretch", 0.0, 1.0},
    };
    for (const Limit& limit : kLimits) {
        double value = 0.0;
        try {
            value = core::py_float(merged[limit.name]);
        } catch (const core::Error& e) {
            throw core::Error("value", e.what());
        }
        if (!(limit.lo <= value && value <= limit.hi)) {
            throw core::Error("value", std::string("brush ") + limit.name + " must be between " + core::py_format_g(limit.lo) +
                                           " and " + core::py_format_g(limit.hi));
        }
    }
    if (!one_of(merged["texture"], kTextures)) throw core::Error("value", "brush texture must be none, grain, soft, dry or water");
    if (!one_of(merged["tip"], kTips)) throw core::Error("value", "brush tip must be round, flat or image");
    if (!one_of(merged["pattern"], kPatterns)) {
        throw core::Error("value", "brush pattern must be dots, dash, lace, grass, hearts, stars or leaves");
    }
    if (!one_of(merged["aa"], kAas)) throw core::Error("value", "brush aa must be none, weak, normal or strong");
    if (merged["tip"] == "image" && !core::py_truthy(merged["tip_png"])) {
        throw core::Error("value", "an image tip needs its picture (tip_png)");
    }
    const Json& raw_label = merged["label"];
    const std::string label = clean_label(core::py_truthy(raw_label) ? core::py_str(raw_label) : std::string());
    if (label.empty()) throw core::Error("value", "a brush needs a name");

    const auto f = [&](const char* name) {
        try {
            return core::py_float(merged[name]);
        } catch (const core::Error& e) {
            throw core::Error("value", e.what());
        }
    };
    const auto i = [&](const char* name) {
        try {
            return core::py_int(merged[name]);
        } catch (const core::Error& e) {
            throw core::Error("value", e.what());
        }
    };
    const auto s = [&](const char* name) { return core::py_truthy(merged[name]) ? core::py_str(merged[name]) : std::string(); };
    Brush out;
    out.key = std::string(key);
    out.label = label;
    out.width_mm = f("width_mm");
    out.min_pressure = f("min_pressure");
    out.gamma = f("gamma");
    out.opacity = f("opacity");
    out.stabilize = i("stabilize");
    out.taper = core::py_truthy(merged["taper"]);
    out.texture = s("texture");
    if (merged.contains("rgb") && core::py_truthy(merged["rgb"])) {
        std::vector<std::int64_t> rgb;
        try {
            for (const Json& v : core::py_list(merged["rgb"])) rgb.push_back(core::py_int(v));
        } catch (const core::Error& e) {
            throw core::Error("value", e.what());
        }
        if (rgb.size() > 3) rgb.resize(3);
        out.rgb = std::move(rgb);
    }
    out.fixed_width = core::py_truthy(merged["fixed_width"]);
    out.tip = core::py_str(merged["tip"]);
    out.tip_angle = f("tip_angle");
    out.tip_ratio = f("tip_ratio");
    out.tip_follow = core::py_truthy(merged["tip_follow"]);
    out.tip_rotation = merged.contains("tip_rotation") && core::py_truthy(merged["tip_rotation"]);
    out.tip_png = s("tip_png");
    out.spacing = f("spacing");
    out.scatter = f("scatter");
    out.size_jitter = f("size_jitter");
    out.turn_jitter = core::py_truthy(merged["turn_jitter"]);
    out.count = i("count");
    out.pattern = s("pattern");
    out.speed = f("speed");
    out.post_smooth = i("post_smooth");
    out.aa = core::py_str(merged["aa"]);
    out.stamp_size = f("stamp_size");
    out.mix = f("mix");
    out.stretch = f("stretch");
    return out;
}

void register_brushes(const Json& definitions) {
    if (!definitions.is_object()) return;
    for (const auto& [key, data] : definitions.items()) {
        if (find_builtin(key) != nullptr) continue;
        Brush made;
        try {
            made = from_dict(key, core::py_dict(data));
        } catch (const core::Error&) {
            continue;  // (a brush that does not make sense is skipped)
        }
        std::unique_lock lock(g_custom_mutex);
        bool replaced = false;
        for (Brush& existing : g_custom) {
            if (existing.key == key) {
                existing = made;
                replaced = true;
                break;
            }
        }
        if (!replaced) g_custom.push_back(std::move(made));
    }
}

void clear_custom() {
    std::unique_lock lock(g_custom_mutex);
    g_custom.clear();
}

std::filesystem::path library_path() { throw NotYetPorted("brush_library"); }
core::Json load_library() { throw NotYetPorted("brush_library"); }
void save_to_library(std::string_view, const std::optional<core::Json>&) { throw NotYetPorted("brush_library"); }

core::PenPoints smoothed(const core::PenPoints& points, std::int64_t strength) { return core::smoothed(points, strength); }

std::optional<Coverage> draw(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind,
                             std::string_view seed, std::span<const double> rotation, double pressure_opacity) {
    auto drawn = draw_plain(size, points, dpi, width_mm, kind, seed, rotation);
    if (!drawn || pressure_opacity <= 0) return drawn;
    const Image light = shade(points, drawn->mask.size(), drawn->origin, dpi, width_mm, pmin(1.0, pressure_opacity));
    drawn->mask = chops::multiply(drawn->mask, light);
    return drawn;
}

std::optional<Box> extent(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind) {
    const auto e = extent_of(brush(kind), size, points, dpi, width_mm);
    if (!e) return std::nullopt;
    return e->box;
}

}  // namespace genko::render::brushes
