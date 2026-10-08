// Python's genko/brushes.py: a line drawn with its brush (the brushes themselves are core's: core/brushes.hpp).

#include "render/brushes.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "core/base64.hpp"
#include "core/paths.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "render/draw.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/stroke.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::brushes {

namespace {

using core::Json;
using core::py_max;
using core::py_min;
using core::py_round_int;
using core::py_trunc;

constexpr double kDegToRad = core::kPi / 180.0;
constexpr double kRadToDeg = 180.0 / core::kPi;
constexpr double kTau = 6.283185307179586;  // math.tau

int clamp_int(double v) {
    if (v > 2147483647.0) return 2147483647;
    if (v < -2147483648.0) return -2147483647 - 1;
    return static_cast<int>(v);
}

// CUSTOM: the registered brushes, in the order they were first registered (Python's dict).
std::shared_mutex g_custom_mutex;
std::vector<Brush> g_custom;
// The book's brushes as last made known (register_book, follow_book).
Json g_book_seen = Json::object();

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
        const double p = core::py_pow(py_max(0.0, py_min(1.0, *pt.p)), b.gamma);
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
            const double quick = py_max(0.0, py_min(1.0, (step / usual - 1.0) / 3.0));
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
        const double thin = py_max(1.0, h * b.tip_ratio);
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
    const double step = py_max(1.0, (b.spacing != 0.0 ? b.spacing : 0.1) * width_px);
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
            const double size = py_max(1.5, 2 * pos.r * b.stamp_size * (1 - b.size_jitter * rng.random()));
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
        mask = mask.filter(Filter::gaussian_blur(py_max(0.6, dpi / 300.0)));
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
        reach = py_max(0.75, 0.75 * b.stamp_size * (b.pattern == "dash" ? 3 : 1.5) + 1.5 * b.scatter);
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
    const double x0 = py_max(0.0, py_trunc(min_x - pad));
    const double y0 = py_max(0.0, py_trunc(min_y - pad));
    const double x1 = py_min(static_cast<double>(size.width), py_trunc(max_x + pad) + 1);
    const double y1 = py_min(static_cast<double>(size.height), py_trunc(max_y + pad) + 1);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    e.box = Box{clamp_int(x0), clamp_int(y0), clamp_int(x1), clamp_int(y1)};
    return e;
}

// _shade: how dark each part of the line is (full where pressed hard, lighter where the touch was light).
Image shade(const core::PenPoints& points, Size size, Point origin, int dpi, double width_mm, double amount) {
    const double scale = dpi / 25.4;
    Image out = Image::create("L", size, Ink(255));
    Draw d(out);
    const double reach = py_max(2.0, width_mm * scale * 1.6);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const core::PenPoint& a = points[i];
        const core::PenPoint& b = points[i + 1];
        const double ax = a.x * scale - origin.x;
        const double ay = a.y * scale - origin.y;
        const double bx = b.x * scale - origin.x;
        const double by = b.y * scale - origin.y;
        const double pressure = py_max(0.0, py_min(1.0, (a.p.value_or(1.0) + b.p.value_or(1.0)) / 2));
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
            radius.push_back(PPoint{p.x * scale, p.y * scale, py_max(0.5, width_mm * py_max(0.03, py_min(1.5, *p.p)) * scale / 2)});
        }
        draw_stamps(mask, radius, b, dpi, width_mm, seed, rotation);
        return Coverage{finish_edges(std::move(mask), b, dpi), Point{box.x0, box.y0}};
    }
    if (b.texture == "soft") {
        {
            Draw d(mask);
            draw_stroke_mm(d, shift, dpi, width_mm * 0.5, Ink(255));
        }
        return Coverage{mask.filter(Filter::gaussian_blur(py_max(1.0, width_mm * scale / 3))), Point{box.x0, box.y0}};
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

// --- the brushes this process knows ---------------------------------------------------------------------------------

Brush brush(std::string_view key) {
    std::shared_lock lock(g_custom_mutex);
    return core::find_brush(Json(std::string(key)), g_custom);
}

std::vector<Brush> everything() {
    const std::span<const Brush> builtin = core::builtin_brushes();
    std::vector<Brush> out(builtin.begin(), builtin.end());
    std::shared_lock lock(g_custom_mutex);
    out.insert(out.end(), g_custom.begin(), g_custom.end());
    return out;
}

void register_brushes(const Json& definitions) {
    std::unique_lock lock(g_custom_mutex);
    core::register_brushes(definitions, g_custom);
}

void register_book(const Json& definitions) {
    std::unique_lock lock(g_custom_mutex);
    core::register_brushes(definitions, g_custom);
    g_book_seen = definitions.is_object() ? definitions : Json::object();
}

void follow_book(const Json& definitions) {
    std::unique_lock lock(g_custom_mutex);
    const Json now = definitions.is_object() ? definitions : Json::object();
    Json changed = Json::object();
    for (const auto& [key, value] : now.items()) {
        const auto seen = g_book_seen.find(key);
        if (seen == g_book_seen.end() || *seen != value) changed[key] = value;
    }
    if (!changed.empty()) core::register_brushes(changed, g_custom);
    g_book_seen = now;
}

void clear_custom() {
    std::unique_lock lock(g_custom_mutex);
    g_custom.clear();
    g_book_seen = Json::object();
}

void define_brush(std::string_view key, const Json& data) {
    std::unique_lock lock(g_custom_mutex);
    if (core::find_builtin(key) != nullptr) throw core::PyValueError("a built-in brush cannot be replaced: " + std::string(key));
    Brush made = core::brush_from_dict(key, data, std::nullopt, g_custom);
    for (Brush& existing : g_custom) {
        if (existing.key == key) {
            existing = std::move(made);
            return;
        }
    }
    g_custom.push_back(std::move(made));
}

void forget_brush(std::string_view key) {
    std::unique_lock lock(g_custom_mutex);
    std::erase_if(g_custom, [key](const Brush& b) { return b.key == key; });
}

std::optional<Image> tip_ink(const std::string& base64_png) { return decode_tip(base64_png); }

std::filesystem::path library_path(const std::filesystem::path& config_dir) { return config_dir / "brushes.json"; }

namespace {

// The library as Python reads it: nothing (an empty library) for a file that is not there, cannot be read or is not
// JSON; `strict`: only a file that is not there is an empty library, and any other that cannot be read throws.
core::Json read_library(const std::filesystem::path& config_dir, bool strict) {
    const std::filesystem::path path = library_path(config_dir);
    const auto unreadable = [&]() -> core::Json {
        if (strict) throw core::PyUncaught("OSError", "the brush library cannot be read, so it is left as it is: " + core::path_to_utf8(path));
        return core::Json::object();
    };
    core::Json data;
    try {
        std::error_code missing;
        if (!std::filesystem::exists(path, missing) && !missing) return core::Json::object();
        std::ifstream file(path, std::ios::binary);
        if (!file) return unreadable();
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file.bad()) return unreadable();
        if (core::utf8_error(text)) return unreadable();  // (UnicodeDecodeError is a ValueError)
        data = core::parse_python_json(text);
    } catch (const core::PyUncaught&) {
        throw;
    } catch (const core::Error&) {
        return unreadable();
    }
    core::Json out = core::Json::object();
    if (!data.is_object()) return strict ? unreadable() : out;
    const auto found = data.find("brushes");
    if (found == data.end() || !core::py_truthy(*found)) return out;
    if (!found->is_object()) throw core::PyUncaught("AttributeError", "'" + core::py_type_name(*found) + "' object has no attribute 'items'");
    for (const auto& [key, value] : found->items()) {
        if (!value.is_object()) throw core::PyValueError("dictionary update sequence element #0 has length 1; 2 is required");
        out[key] = value;
    }
    return out;
}

}  // namespace

core::Json load_library(const std::filesystem::path& config_dir) { return read_library(config_dir, false); }

void save_to_library(const std::filesystem::path& config_dir, std::string_view key, const std::optional<core::Json>& data) {
    core::Json brushes = read_library(config_dir, true);
    if (data) brushes[std::string(key)] = *data;
    else brushes.erase(std::string(key));
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    const std::string text = core::dump(core::Json{{"brushes", brushes}}, options);
    try {
        storage::write_atomic(library_path(config_dir), text);
    } catch (const core::Error& error) {
        throw core::PyUncaught("OSError", "the brush library cannot be written: " + core::path_to_utf8(library_path(config_dir)) + " (" + error.what() + ")");
    }
}

std::optional<Coverage> draw(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind,
                             std::string_view seed, std::span<const double> rotation, double pressure_opacity) {
    auto drawn = draw_plain(size, points, dpi, width_mm, kind, seed, rotation);
    if (!drawn || pressure_opacity <= 0) return drawn;
    const Image light = shade(points, drawn->mask.size(), drawn->origin, dpi, width_mm, py_min(1.0, pressure_opacity));
    drawn->mask = chops::multiply(drawn->mask, light);
    return drawn;
}

std::optional<Box> extent(Size size, const core::PenPoints& points, int dpi, double width_mm, std::string_view kind) {
    const auto e = extent_of(brush(kind), size, points, dpi, width_mm);
    if (!e) return std::nullopt;
    return e->box;
}

}  // namespace genko::render::brushes
