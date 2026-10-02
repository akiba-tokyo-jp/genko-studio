// Python's genko/tones.py and the AM screen of genko/screentone.py.
//
// numpy works in float32 here (np.mgrid(...).astype(np.float32), and every Python float meets those arrays as a
// float32): each pixel's value is computed with C++ floats in numpy's order of operations, which gives numpy's bits
// (no FMA: GenkoFlags). A box of the page is drawn alone with the same pixels as the whole page cut: what needs the
// whole page (a gradient's box, the star tone's mean coverage, the noise screen's error diffusion, which runs from the
// top of the page) is computed for the whole page, or the rows above the box, first.

#include "render/tones.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <mutex>

#include "core/base64.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyvalue.hpp"
#include "core/stroke_geom.hpp"
#include "render/brushes.hpp"
#include "render/draw.hpp"
#include "render/npcompat.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/render_ops.hpp"

namespace genko::render::tones {

namespace {

using core::Json;

constexpr double kDegToRad = core::kPi / 180.0;  // math.radians

Size size_of(const Box& b) { return Size{b.width(), b.height()}; }
bool intersects(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
Box intersection(const Box& a, const Box& b) {
    return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}
Box shifted(const Box& b, int dx, int dy) { return Box{b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy}; }

bool is_str(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

bool one_of(const Json& value, const std::vector<std::string>& names) {
    return value.is_string() && std::find(names.begin(), names.end(), value.get<std::string>()) != names.end();
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

// Python's min(a, b) / max(a, b) for floats (the first unless the second is smaller / larger).
double pmin(double a, double b) { return b < a ? b : a; }
double pmax(double a, double b) { return b > a ? b : a; }

float f32(double v) { return static_cast<float>(v); }

// np.clip for float32 (NaN stays NaN)
float clipf(float v, float lo, float hi) {
    if (std::isnan(v)) return v;
    return v < lo ? lo : (v > hi ? hi : v);
}

// .astype(np.uint8) of a float32 already clipped to 0..255 (truncation; NaN gives 0 as on x86)
std::uint8_t to_u8(float v) {
    if (!(v >= 0.0f)) return 0;
    return static_cast<std::uint8_t>(v > 255.0f ? 255.0f : v);
}

// Python's round(x) (an int, ties to even).
std::int64_t round_int(double x) { return core::py_int_of(std::nearbyint(x)); }

// --- int(x) as a list of 32-bit words (numpy's SeedSequence entropy), for numbers of any size ---------------------

std::vector<std::uint32_t> words_of(std::uint64_t v) {
    std::vector<std::uint32_t> out;
    if (v == 0) out.push_back(0);
    while (v > 0) {
        out.push_back(static_cast<std::uint32_t>(v & 0xffffffffu));
        v >>= 32;
    }
    return out;
}

// big *= m; big += a
void mul_add(std::vector<std::uint32_t>& big, std::uint32_t m, std::uint32_t a) {
    std::uint64_t carry = a;
    for (auto& w : big) {
        const std::uint64_t v = static_cast<std::uint64_t>(w) * m + carry;
        w = static_cast<std::uint32_t>(v & 0xffffffffu);
        carry = v >> 32;
    }
    if (carry != 0) big.push_back(static_cast<std::uint32_t>(carry));
}

// int(value) for a seed, as numpy's SeedSequence takes it: its words, least significant first.
std::vector<std::uint32_t> seed_words(const Json& value) {
    const auto negative = [] { throw core::PyValueError("expected non-negative integer"); };
    switch (value.type()) {
        case Json::value_t::boolean: return words_of(value.get<bool>() ? 1 : 0);
        case Json::value_t::number_unsigned: return words_of(value.get<std::uint64_t>());
        case Json::value_t::number_integer: {
            const auto v = value.get<std::int64_t>();
            if (v < 0) negative();
            return words_of(static_cast<std::uint64_t>(v));
        }
        case Json::value_t::number_float: {
            const double d = std::trunc(value.get<double>());
            (void)core::py_int_of(d);  // (NaN and infinities raise)
            if (d < 0) negative();
            if (d < 18446744073709551616.0) return words_of(static_cast<std::uint64_t>(d));
            int exp = 0;
            const double mant = std::frexp(d, &exp);  // d = mant * 2**exp, 0.5 <= mant < 1
            auto top = static_cast<std::uint64_t>(std::ldexp(mant, 53));
            std::vector<std::uint32_t> out = words_of(top);
            for (int shift = exp - 53; shift > 0; --shift) mul_add(out, 2, 0);
            return out;
        }
        case Json::value_t::string: {
            const std::int64_t small = core::to_int(value);  // (ValueError for what is not a number)
            if (small < 0) negative();
            if (small != INT64_MAX) return words_of(static_cast<std::uint64_t>(small));
            std::vector<std::uint32_t> out{0};  // (beyond 64 bits: from its decimal digits)
            for (const char c : value.get_ref<const std::string&>()) {
                if (c >= '0' && c <= '9') mul_add(out, 10, static_cast<std::uint32_t>(c - '0'));
            }
            return out;
        }
        default:
            throw core::PyTypeError("int() argument must be a string, a bytes-like object or a real number, not '" +
                                    core::py_type_name(value) + "'");
    }
}

// --- remembered pictures ------------------------------------------------------------------------------------------

struct TileEntry {
    int m = 0;
    int n = 0;
    std::string shape;
    std::shared_ptr<const Image> image;
};

std::mutex g_tiles_mutex;
std::list<TileEntry> g_tiles;  // most recent first (screentone.threshold_tile's lru_cache(maxsize=32))

struct PictureEntry {
    std::string data;
    std::optional<Image> ink;
};

std::mutex g_pictures_mutex;
std::list<PictureEntry> g_pictures;  // (tones._TILES)

// round(dist, 9) as a whole number of 1e-9: Python's correctly rounded decimal rounding (ties to even). Sorting by it
// sorts by Python's rounded floats.
std::int64_t nanos(double dist) {
    const double scaled = dist * 1e9;
    const double frac = scaled - std::floor(scaled);
    if (std::fabs(frac - 0.5) > 1e-6) return static_cast<std::int64_t>(std::nearbyint(scaled));
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, dist, std::chars_format::fixed, 9);
    std::int64_t out = 0;
    for (const char* p = buf; p < r.ptr; ++p) {
        if (*p >= '0' && *p <= '9') out = out * 10 + (*p - '0');
    }
    return out;
}

Image make_threshold_tile(int m, int n, std::string_view shape) {
    const std::int64_t size = static_cast<std::int64_t>(m) * m + static_cast<std::int64_t>(n) * n;
    if (size > 4096) {
        throw core::Error("value", "the screen is too coarse for this resolution (" + std::to_string(size) + " px a side)");
    }
    const double norm = static_cast<double>(size);
    struct Item {
        std::uint64_t key;  // round(dist, 9) in 1e-9, then the tie breaker (x * 7 + y * 13) % size
        std::uint32_t at;   // y * size + x
    };
    std::vector<Item> order;
    order.reserve(static_cast<std::size_t>(size * size));
    for (std::int64_t y = 0; y < size; ++y) {
        for (std::int64_t x = 0; x < size; ++x) {
            // coordinates in the lattice basis v=(m,n), w=(-n,m); dot centres sit at cell centres
            const double u = static_cast<double>(x * m + y * n) / norm;
            const double v = static_cast<double>(-x * n + y * m) / norm;
            const double du = u - std::floor(u) - 0.5;
            const double dv = v - std::floor(v) - 0.5;
            double dist = 0.0;
            if (shape == "square") {
                dist = core::py_pow(pmax(std::fabs(du), std::fabs(dv)), 2.0) + (du * du + dv * dv) * 1e-3;
            } else if (shape == "diamond") {
                dist = core::py_pow(std::fabs(du) + std::fabs(dv), 2.0) + (du * du + dv * dv) * 1e-3;
            } else if (shape == "ellipse") {
                dist = du * du * 0.55 + dv * dv * 1.45;
            } else {
                dist = du * du + dv * dv;
            }
            const auto tie = static_cast<std::uint64_t>((x * 7 + y * 13) % size);
            order.push_back(Item{(static_cast<std::uint64_t>(nanos(dist)) << 13) | tie, static_cast<std::uint32_t>(y * size + x)});
        }
    }
    std::sort(order.begin(), order.end(), [](const Item& a, const Item& b) { return a.key != b.key ? a.key < b.key : a.at < b.at; });
    const auto total = static_cast<std::int64_t>(order.size());
    std::string data(static_cast<std::size_t>(total), '\0');
    for (std::int64_t rank = 0; rank < total; ++rank) {
        data[order[static_cast<std::size_t>(rank)].at] = static_cast<char>(std::min<std::int64_t>(255, rank * 256 / total));
    }
    return Image::frombytes("L", Size{static_cast<int>(size), static_cast<int>(size)}, data);
}

// --- coverage ------------------------------------------------------------------------------------------------------

// tones.coverage_map: the black share at a pixel (float32).
struct Coverage {
    bool constant = true;
    float value = 0.0f;
    bool radial = false;
    float cx = 0, cy = 0, radius = 1;  // radial
    float dx = 0, dy = 0, lo = 0, span = 1;  // linear
    float start = 0, delta = 0;  // start + (end - start) * t

    float at(int x, int y) const {
        if (constant) return value;
        const float xs = static_cast<float>(x);
        const float ys = static_cast<float>(y);
        float t = 0.0f;
        if (radial) {
            t = np::hypotf(xs - cx, ys - cy) / radius;
        } else {
            t = (xs * dx + ys * dy - lo) / span;
        }
        t = clipf(t, 0.0f, 1.0f);
        return start + delta * t;
    }
};

Coverage coverage_of(const Settings& s, const std::optional<Box>& where_box, Size size) {
    Coverage c;
    if (!core::py_truthy(s.gradient)) {
        c.value = f32(s.density);
        return c;
    }
    c.constant = false;
    const Json& gradient = s.gradient;
    const Box box = where_box.value_or(Box{0, 0, size.width, size.height});
    const double x0 = box.x0, y0 = box.y0, x1 = box.x1, y1 = box.y1;
    const double start = core::to_float(core::py_get(gradient, "start", Json(0.0)));
    const double end = core::to_float(core::py_get(gradient, "end", Json(s.density)));
    if (is_str(core::py_get(gradient, "shape"), "radial")) {
        c.radial = true;
        c.cx = f32((x0 + x1) / 2);
        c.cy = f32((y0 + y1) / 2);
        c.radius = f32(pmax(1.0, core::py_hypot(x1 - x0, y1 - y0) / 2));
    } else {
        const double a = core::to_float(core::py_get(gradient, "angle", Json(90))) * kDegToRad;
        const double dx = core::math_cos(a);
        const double dy = core::math_sin(a);
        const double proj[4] = {x0 * dx + y0 * dy, x1 * dx + y0 * dy, x0 * dx + y1 * dy, x1 * dx + y1 * dy};
        double lo = proj[0];
        double hi = proj[0];
        for (const double p : proj) {
            lo = pmin(lo, p);
            hi = pmax(hi, p);
        }
        c.dx = f32(dx);
        c.dy = f32(dy);
        c.lo = f32(lo);
        c.span = f32(pmax(1.0, hi - lo));
    }
    c.start = f32(start);
    c.delta = f32(end - start);
    return c;
}

// The pairwise sum numpy makes of the coverage over the whole page, as its mean (cover.mean()).
float coverage_mean(const Coverage& c, Size size) {
    std::vector<float> all(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height));
    std::size_t i = 0;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) all[i++] = c.at(x, y);
    }
    return np::mean(all.data(), all.size());
}

// --- the screens ---------------------------------------------------------------------------------------------------

// tones._screen's lines(theta): 0 on a line's middle, 1 between lines.
struct Lines {
    float c = 0, s = 0, period = 1;
    float at(float xs, float ys) const {
        const float u = (xs * c + ys * s) / period;
        return std::fabs(u - std::floor(u) - 0.5f) * 2.0f;
    }
};

Lines lines_at(double theta, double period) { return Lines{f32(core::math_cos(theta)), f32(core::math_sin(theta)), f32(period)}; }

// The threshold field of tones._screen over a box (each value: below the coverage is black).
class Screen {
public:
    Screen(const Json& pattern, Size size, int dpi, double lpi, double angle, std::pair<int, int> shift, std::string_view shape)
        : shift_(shift) {
        if (is_str(pattern, "dot")) {
            dot_ = true;
            const auto [m, n] = screen_vector(dpi, lpi, angle);
            const std::string_view known = std::find(dot_shapes().begin(), dot_shapes().end(), shape) != dot_shapes().end()
                                               ? shape
                                               : std::string_view("round");
            tile_ = threshold_tile(m, n, known);
            t_ = tile_->width();
            const auto pmod = [](std::int64_t a, std::int64_t b) { return ((a % b) + b) % b; };  // Python's %
            ox_ = static_cast<int>(pmod(-static_cast<std::int64_t>(shift.first), t_));
            oy_ = static_cast<int>(pmod(-static_cast<std::int64_t>(shift.second), t_));
            raw_ = tile_->tobytes();
            (void)size;
            return;
        }
        const double period = pmax(2.0, static_cast<double>(dpi) / pmax(1.0, lpi));
        const double a = angle * kDegToRad;
        first_ = lines_at(a + core::kPi / 2, period);
        cross_ = !is_str(pattern, "line");
        if (cross_) second_ = lines_at(a, period);
    }

    float at(int x, int y) const {
        if (dot_) {
            const auto tx = static_cast<std::size_t>((x + ox_) % t_);
            const auto ty = static_cast<std::size_t>((y + oy_) % t_);
            return static_cast<float>(static_cast<unsigned char>(raw_[ty * static_cast<std::size_t>(t_) + tx])) / 256.0f;
        }
        const float xs = static_cast<float>(x) - static_cast<float>(shift_.first);
        const float ys = static_cast<float>(y) - static_cast<float>(shift_.second);
        const float one = first_.at(xs, ys);
        if (!cross_) return one;
        const float two = second_.at(xs, ys);
        return two < one ? two : one;  // (np.minimum)
    }

private:
    std::pair<int, int> shift_;
    bool dot_ = false;
    std::shared_ptr<const Image> tile_;
    std::string raw_;
    int t_ = 1, ox_ = 0, oy_ = 0;
    Lines first_, second_;
    bool cross_ = false;
};

// A float32 grey picture's Floyd–Steinberg black and white (Image.fromarray(grey).convert("1").convert("L") < 128) over
// the rows [0, rows) of the page, its whole width: `grey(x, y)` gives each pixel.
template <class Grey>
std::string dithered(Size size, int rows, const Grey& grey) {
    std::string data(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(rows), '\0');
    std::size_t i = 0;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < size.width; ++x) data[i++] = static_cast<char>(grey(x, y));
    }
    return Image::frombytes("L", Size{size.width, rows}, data).convert("1").convert("L").tobytes();
}

// --- the pattern tones (柄トーン) ---------------------------------------------------------------------------------------

class Motif {
public:
    Motif(const Settings& s, Size size, int dpi, const Coverage& cover) : size_(size) {
        pattern_ = s.pattern.get<std::string>();
        const Json scale = s.scale_mm ? *s.scale_mm : Json();
        period_ = pmax(3.0, core::to_float(core::py_or(scale, Json(3.0))) / 25.4 * dpi);
        const auto [dx, dy] = shift_px(s.offset_mm, dpi);
        dx_ = dx;
        dy_ = dy;
        const double a = s.angle * kDegToRad;
        cos_ = f32(core::math_cos(a));
        sin_ = f32(core::math_sin(a));
        fperiod_ = f32(period_);
        if (pattern_ == "star") {
            const double mean = static_cast<double>(coverage_mean(cover, size));
            star_k_ = f32(0.18 + 0.5 * std::sqrt(pmax(0.01, mean)));
        } else if (pattern_ == "sand") {
            const auto rng_seed = seed_words(s.seed ? *s.seed : Json(7));
            grain_ = std::max<std::int64_t>(1, round_int(period_ / 6));
            rows_ = static_cast<std::int64_t>(size.height) / grain_ + 1;
            cols_ = static_cast<std::int64_t>(size.width) / grain_ + 1;
            np::Pcg64 rng(rng_seed);
            field_.resize(static_cast<std::size_t>(rows_ * cols_));
            for (auto& v : field_) v = f32(rng.random());
        } else if (pattern_ == "image") {
            const Json data = s.tile_png ? core::py_or(*s.tile_png, Json("")) : Json("");
            const auto ink = data.is_string() ? tile(data.get_ref<const std::string&>()) : std::nullopt;
            if (ink) {
                const std::int64_t side = std::max<std::int64_t>(2, round_int(period_));
                const std::int64_t tall = std::max<std::int64_t>(
                    2, round_int(static_cast<double>(side) * ink->height() / static_cast<double>(std::max(1, ink->width()))));
                const Image scaled = ink->resize(Size{static_cast<int>(side), static_cast<int>(tall)});
                tw_ = scaled.width();
                th_ = scaled.height();
                const std::string raw = scaled.tobytes();
                picture_.resize(raw.size());
                for (std::size_t i = 0; i < raw.size(); ++i) picture_[i] = static_cast<float>(static_cast<unsigned char>(raw[i])) / 255.0f;
            }
        }
    }

    // 1 where the motif is black
    bool black(int x, int y, float cover) const {
        const float xs = static_cast<float>(x) - static_cast<float>(dx_);
        const float ys = static_cast<float>(y) - static_cast<float>(dy_);
        const float u = (xs * cos_ + ys * sin_) / fperiod_;
        const float v = (-xs * sin_ + ys * cos_) / fperiod_;
        const float fu = u - std::floor(u);
        const float fv = v - std::floor(v);
        if (pattern_ == "check") return np::remainder(std::floor(u) + std::floor(v), 2.0f) == 0.0f;
        if (pattern_ == "grid") return fu < cover / 2.0f || fv < cover / 2.0f;
        if (pattern_ == "hatch") return fu < cover;
        if (pattern_ == "brick") {
            const float row = std::floor(v * 2.0f);
            const float moved = np::remainder(u + np::remainder(row, 2.0f) * 0.5f, 1.0f);
            return np::remainder(v * 2.0f, 1.0f) < cover / 2.0f || moved < cover / 4.0f;
        }
        if (pattern_ == "wave") {
            const float wave = np::remainder(v + f32(0.18) * np::sinf(u * 2.0f * f32(core::kPi)), 1.0f);
            return wave < cover;
        }
        if (pattern_ == "star") {
            const float ddx = fu - 0.5f;
            const float ddy = fv - 0.5f;
            const float r = np::hypotf(ddx, ddy);
            const float theta = np::atan2f(ddy, ddx);
            const float edge = star_k_ * (f32(0.55) + f32(0.45) * np::cosf(5.0f * theta));
            return r < edge * 0.5f;
        }
        if (pattern_ == "sand") {
            const auto r = static_cast<std::size_t>(y / grain_);
            const auto c = static_cast<std::size_t>(x / grain_);
            return field_[r * static_cast<std::size_t>(cols_) + c] < cover;
        }
        // image
        if (picture_.empty()) return false;
        const auto pymod = [](std::int64_t a, std::int64_t b) { return ((a % b) + b) % b; };
        const float fx = std::floor(fu * static_cast<float>(tw_));
        const float fy = std::floor(fv * static_cast<float>(th_));
        if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
        const auto iu = pymod(static_cast<std::int64_t>(fx), tw_);
        const auto iv = pymod(static_cast<std::int64_t>(fy), th_);
        return picture_[static_cast<std::size_t>(iv * tw_ + iu)] > 0.5f;
    }

private:
    Size size_;
    std::string pattern_;
    double period_ = 3.0;
    float fperiod_ = 3.0f;
    int dx_ = 0, dy_ = 0;
    float cos_ = 1, sin_ = 0;
    float star_k_ = 0;
    std::int64_t grain_ = 1, rows_ = 0, cols_ = 0;
    std::vector<float> field_;
    std::int64_t tw_ = 0, th_ = 0;
    std::vector<float> picture_;
};

// --- where the tone is ---------------------------------------------------------------------------------------------

// tones.mask(layer, page, size, dpi) over `box` (L, 255 = tone).
Image tone_mask(const core::Layer& layer, const Page& p, const Box& box) {
    Image out = Image::create("L", size_of(box), Ink(0));
    const bool region = layer.region && !layer.region->empty();
    if (region) {
        std::vector<PointD> pts;
        for (const core::Point& pt : *layer.region) pts.push_back(detail::xy_point(pt.x.value(), pt.y.value(), p.dpi));
        PageCanvas canvas(out, box, p.size);
        canvas.draw().polygon(pts, Ink(255));
        canvas.commit();
    }
    std::vector<const core::Patch*> patches;
    for (const core::Patch& patch : layer.patches) {
        if (patch.png && !patch.png->empty()) patches.push_back(&patch);
    }
    if (!patches.empty()) {
        Image rgba = Image::create("RGBA", size_of(box), Ink{0, 0, 0, 0});
        for (const core::Patch* patch : patches) {
            core::Patch black = *patch;
            black.attrs["rgb"] = Json::array({0, 0, 0});
            black.attrs["opacity"] = 1.0;
            p.paint_patch(rgba, box, black);
        }
        out = chops::lighter(out, rgba.getchannel(3));
    }
    const auto& strokes = layer.strokes ? layer.strokes->items : core::empty_strokes()->items;
    const bool painted = std::any_of(strokes.begin(), strokes.end(), [](const core::StrokePtr& s) { return !s->kind.starts_with("scrape"); });
    if (!region && patches.empty() && !painted) {
        PageCanvas canvas(out, box, p.size);
        for (const core::Frame* frame : p.page->leaf_frames()) {
            const Box r = rect_px(frame->rect, p.dpi);
            canvas.draw().rectangle(BoxF{static_cast<double>(r.x0), static_cast<double>(r.y0), static_cast<double>(r.x1),
                                         static_cast<double>(r.y1)},
                                    Ink(255));
        }
        canvas.commit();
    }
    for (const core::StrokePtr& stroke : strokes) {
        const std::string& kind = stroke->kind;
        const bool scrape = kind.starts_with("scrape");
        const std::string brush = kind == "scrape_soft" ? "airbrush" : (scrape ? "mili" : kind);
        const core::PenPoints points = core::stroke_pen_points(*stroke);
        const double width = stroke->width_mm != 0.0 ? stroke->width_mm : 1.0;
        const auto reach = brushes::extent(p.size, points, p.dpi, width, brush);
        if (!reach || !intersects(*reach, box)) continue;  // (it changes nothing of this box)
        const auto drawn = brushes::draw(p.size, points, p.dpi, width, brush, stroke->id);
        if (!drawn) continue;
        const Box where{drawn->origin.x, drawn->origin.y, drawn->origin.x + drawn->mask.width(), drawn->origin.y + drawn->mask.height()};
        const Box part = intersection(where, box);
        if (part.x1 <= part.x0 || part.y1 <= part.y0) continue;
        const Box local = shifted(part, -box.x0, -box.y0);
        const Image piece = drawn->mask.crop(shifted(part, -where.x0, -where.y0));
        const Image now = out.crop(local);
        out.paste(scrape ? chops::subtract(now, piece) : chops::lighter(now, piece), Point{local.x0, local.y0});
    }
    return out;
}

// The tone's alpha over `box` (tones.pattern_image's alpha) for a mask `where` of the box.
Image tone_alpha(const Settings& s, const Image& where, int dpi, bool print_mode, Size size, const Box& box,
                 const std::optional<Box>& where_box) {
    const Coverage cover = coverage_of(s, where_box, size);
    const bool motif = one_of(s.pattern, motifs());
    const std::string mask = where.tobytes();
    std::string alpha(mask.size(), '\0');
    const int w = box.width();
    const auto put = [&](std::size_t i, float black) {
        const float alpha_in = static_cast<float>(static_cast<unsigned char>(mask[i])) / 255.0f;
        alpha[i] = static_cast<char>(to_u8(clipf(black * alpha_in * 255.0f, 0.0f, 255.0f)));
    };
    if (motif) {
        const Motif m(s, size, dpi, cover);
        for (int y = 0; y < box.height(); ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                const float c = cover.at(box.x0 + x, box.y0 + y);
                put(i, m.black(box.x0 + x, box.y0 + y, c) ? 1.0f : 0.0f);
            }
        }
    } else if (!print_mode || is_str(s.pattern, "flat")) {
        for (int y = 0; y < box.height(); ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                put(i, cover.at(box.x0 + x, box.y0 + y));
            }
        }
    } else if (is_str(s.pattern, "noise")) {
        const std::string bw = dithered(size, box.y1, [&](int x, int y) {
            return to_u8(clipf(255.0f * (1.0f - cover.at(x, y)), 0.0f, 255.0f));
        });
        for (int y = 0; y < box.height(); ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                const auto v = static_cast<unsigned char>(
                    bw[static_cast<std::size_t>(box.y0 + y) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(box.x0 + x)]);
                put(i, v < 128 ? 1.0f : 0.0f);
            }
        }
    } else {
        const bool cross = is_str(s.pattern, "cross");  // (two families together reach the asked share)
        const Json shape = s.dot_shape ? core::py_or(*s.dot_shape, Json("round")) : Json("round");
        const Screen screen(s.pattern, size, dpi, s.lpi, s.angle, shift_px(s.offset_mm, dpi), core::py_str(shape));
        for (int y = 0; y < box.height(); ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                float share = cover.at(box.x0 + x, box.y0 + y);
                if (cross) share = 1.0f - std::sqrt(clipf(1.0f - share, 0.0f, 1.0f));
                put(i, screen.at(box.x0 + x, box.y0 + y) < share ? 1.0f : 0.0f);
            }
        }
    }
    return Image::frombytes("L", size_of(box), alpha);
}

Image transparent(Size size) { return Image::create("RGBA", size, Ink{0, 0, 0, 0}); }

// The drawing ops (render_ops.hpp) join core::OpRegistry::builtin() in every program that draws pages: this file is
// linked into each of them through the page drawing.
[[maybe_unused]] const bool kOpsRegistered = (core::add_builtin_registrar(&register_render_ops), true);

}  // namespace

const std::vector<std::string>& patterns() {
    static const std::vector<std::string> p{"dot", "line", "cross", "noise", "flat", "check", "brick",
                                            "wave", "grid", "hatch", "star", "sand", "image"};
    return p;
}

const std::vector<std::string>& motifs() {
    static const std::vector<std::string> m{"check", "brick", "wave", "grid", "hatch", "star", "sand", "image"};
    return m;
}

const std::vector<std::string>& dot_shapes() {
    static const std::vector<std::string> d{"round", "square", "diamond", "ellipse"};
    return d;
}

void validate(const Json& tone) {
    const Json pattern = core::py_get(tone, "pattern");
    if (core::py_truthy(pattern) && !one_of(pattern, patterns())) throw core::PyValueError("pattern must be one of " + joined(patterns()));
    if (is_str(pattern, "image")) {
        const Json data = core::py_get(tone, "tile_png");
        if (!core::py_truthy(data) || !data.is_string() || !tile(data.get_ref<const std::string&>())) {
            throw core::PyValueError("an image tone needs its picture (tile_png, a base64 PNG)");
        }
    }
    const Json scale = core::py_get(tone, "scale_mm");
    if (!scale.is_null()) {
        const double v = core::to_float(scale);
        if (!(0.3 <= v && v <= 50)) throw core::PyValueError("scale_mm is 0.3 to 50");
    }
    const Json shape = core::py_get(tone, "dot_shape");
    if (!shape.is_null() && !one_of(shape, dot_shapes())) throw core::PyValueError("dot_shape must be one of " + joined(dot_shapes()));
    const Json offset = core::py_get(tone, "offset_mm");
    if (!offset.is_null() && (!offset.is_array() || offset.size() != 2)) throw core::PyValueError("offset_mm is [x, y] in mm");
    const Json gradient = core::py_get(tone, "gradient");
    if (core::py_truthy(gradient)) {
        const Json shape_value = core::py_get(gradient, "shape", Json("linear"));
        if (!is_str(shape_value, "linear") && !is_str(shape_value, "radial")) throw core::PyValueError("gradient shape must be linear or radial");
        for (const char* key : {"start", "end"}) {
            const double v = core::to_float(core::py_get(gradient, key, Json(0)));
            if (!(0 <= v && v <= 1)) throw core::PyValueError(std::string("gradient ") + key + " is a density from 0 to 1");
        }
    }
}

Settings settings(const core::Layer& layer) {
    Settings s;
    const Json tone = layer.tone && layer.tone->is_object() ? *layer.tone : Json::object();
    const Json pattern = core::py_get(tone, "pattern");
    // (tones._legacy_noise: an old tone layer whose material is of the kind "noise" — never so: genko.materials makes
    // every catalog and library entry of that kind a "tone" when it reads it)
    s.pattern = core::py_truthy(pattern) ? pattern : Json("dot");
    const auto keep = [&](const char* key, std::optional<Json>& slot) {
        const Json v = core::py_get(tone, key);
        if (!v.is_null()) slot = v;
    };
    keep("scale_mm", s.scale_mm);
    keep("tile_png", s.tile_png);
    keep("seed", s.seed);
    keep("dot_shape", s.dot_shape);
    keep("offset_mm", s.offset_mm);
    s.gradient = core::py_get(tone, "gradient");
    s.lpi = layer.lpi && layer.lpi->truthy() ? layer.lpi->value() : 60.0;
    s.density = pmax(0.0, pmin(1.0, layer.density ? layer.density->value() : 0.3));
    s.angle = layer.angle;
    return s;
}

std::pair<int, int> shift_px(const std::optional<Json>& offset_mm, int dpi) {
    if (!offset_mm || !core::py_truthy(*offset_mm)) return {0, 0};
    const double x = core::to_float(core::py_item(*offset_mm, 0));
    const double y = core::to_float(core::py_item(*offset_mm, 1));
    return {static_cast<int>(round_int(x / 25.4 * dpi)), static_cast<int>(round_int(y / 25.4 * dpi))};
}

std::pair<int, int> screen_vector(double dpi, double lpi, double angle) {
    const double cell = pmax(2.0, dpi / pmax(1.0, lpi));
    const double theta = angle * kDegToRad;
    std::int64_t m = round_int(cell * core::math_cos(theta));
    const std::int64_t n = round_int(cell * core::math_sin(theta));
    if (m == 0 && n == 0) m = 2;
    if (std::llabs(m) > 100000 || std::llabs(n) > 100000) throw core::Error("value", "the screen is too coarse for this resolution");
    return {static_cast<int>(m), static_cast<int>(n)};
}

std::shared_ptr<const Image> threshold_tile(int m, int n, std::string_view shape) {
    {
        std::lock_guard lock(g_tiles_mutex);
        for (auto it = g_tiles.begin(); it != g_tiles.end(); ++it) {
            if (it->m == m && it->n == n && it->shape == shape) {
                g_tiles.splice(g_tiles.begin(), g_tiles, it);
                return g_tiles.front().image;
            }
        }
    }
    auto image = std::make_shared<const Image>(make_threshold_tile(m, n, shape));
    std::lock_guard lock(g_tiles_mutex);
    g_tiles.push_front(TileEntry{m, n, std::string(shape), image});
    while (g_tiles.size() > 32) g_tiles.pop_back();
    return image;
}

std::optional<Image> tile(std::string_view data) {
    {
        std::lock_guard lock(g_pictures_mutex);
        for (auto it = g_pictures.begin(); it != g_pictures.end(); ++it) {
            if (it->data == data) {
                g_pictures.splice(g_pictures.begin(), g_pictures, it);
                return g_pictures.front().ink;
            }
        }
    }
    std::optional<Image> ink;
    try {
        const Image grey = open_image(core::a2b_base64(data), kPillowOpenLimits).convert("LA");
        ink = chops::multiply(chops::invert(grey.getchannel(0)), grey.getchannel(1));
    } catch (const NotYetPorted&) {
        throw;  // (a picture in a format this build does not read yet: said, not taken for no picture)
    } catch (const core::Error&) {
        ink.reset();
    }
    std::lock_guard lock(g_pictures_mutex);
    g_pictures.push_front(PictureEntry{std::string(data), ink});
    while (g_pictures.size() > 16) g_pictures.pop_back();
    return ink;
}

Image draw_layer(Image image, const core::Layer& layer, const Page& page, const Box& box, const Image* panels,
                 bool print_mode) {
    Image where = tone_mask(layer, page, box);
    if (layer.panel_clip && panels != nullptr) where = chops::multiply(where, *panels);
    if (!where.getbbox()) return image;  // (nothing of it in this box: Python's empty mask leaves the picture too)
    const Settings s = settings(layer);
    std::optional<Box> where_box;
    const Box whole{0, 0, page.size.width, page.size.height};
    if (core::py_truthy(s.gradient)) {
        // the gradient runs across the mask's box on the whole page
        if (box == whole) {
            where_box = where.getbbox();
        } else {
            Image full = tone_mask(layer, page, whole);
            if (layer.panel_clip) {
                if (const auto all = detail::clip_mask(*page.page, page.size, page.dpi, whole)) full = chops::multiply(full, *all);
            }
            where_box = full.getbbox();
        }
    }
    Image alpha = tone_alpha(s, where, page.dpi, print_mode, page.size, box, where_box);
    const double opacity = layer.opacity;
    if (opacity < 1) alpha = alpha.point([&](int v) { return static_cast<int>(round_int(v * opacity)); });
    Image tone = transparent(size_of(box));
    tone.putalpha(alpha);
    return alpha_composite(image, tone);
}

std::vector<float> screen(std::string_view pattern, Size size, int dpi, double lpi, double angle, std::pair<int, int> shift,
                          std::string_view shape, const Box& box) {
    const Screen s(Json(std::string(pattern)), size, dpi, lpi, angle, shift, shape);
    std::vector<float> out(static_cast<std::size_t>(box.width()) * static_cast<std::size_t>(box.height()));
    std::size_t i = 0;
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) out[i++] = s.at(x, y);
    }
    return out;
}

Image screened(const Image& raster, const Json& spec, int dpi, Size page_size, const Box& box) {
    const Image rgba = raster.mode() == "RGBA" ? raster : raster.convert("RGBA");
    const std::string grey = rgba.convert("L").tobytes();
    const std::string alpha = rgba.getchannel(3).tobytes();
    const double black_at = core::to_float(core::py_get(spec, "black", Json(0.1)));
    const double white_at = core::to_float(core::py_get(spec, "white", Json(0.95)));
    const float white = f32(white_at);
    const float span = f32(pmax(0.01, white_at - black_at));
    const auto cover_at = [&](std::size_t i) {
        const float g = static_cast<float>(static_cast<unsigned char>(grey[i])) / 255.0f;
        const float a = static_cast<float>(static_cast<unsigned char>(alpha[i])) / 255.0f;
        return clipf((white - g) / span, 0.0f, 1.0f) * a;
    };
    const Json pattern_value = core::py_get(spec, "pattern");
    const std::string pattern = core::py_truthy(pattern_value) ? core::py_str(pattern_value) : std::string("dot");
    const double lpi = core::to_float(core::py_get(spec, "lpi", Json(60)));
    const double angle = core::to_float(core::py_get(spec, "angle", Json(45)));
    const int w = rgba.width();
    const int h = rgba.height();
    std::string out(grey.size(), '\0');
    if (pattern == "noise") {
        std::string lifted(grey.size(), '\0');
        for (std::size_t i = 0; i < grey.size(); ++i) lifted[i] = static_cast<char>(to_u8(clipf(255.0f * (1.0f - cover_at(i)), 0.0f, 255.0f)));
        const std::string bw = Image::frombytes("L", rgba.size(), lifted).convert("1").convert("L").tobytes();
        for (std::size_t i = 0; i < bw.size(); ++i) out[i] = static_cast<unsigned char>(bw[i]) < 128 ? static_cast<char>(255) : '\0';
    } else {
        const std::string kind = pattern == "dot" || pattern == "line" || pattern == "cross" ? pattern : std::string("dot");
        const Json shape_value = core::py_get(spec, "shape");
        const std::string shape = core::py_truthy(shape_value) ? core::py_str(shape_value) : std::string("round");
        const std::optional<Json> offset = spec.contains("offset_mm") ? std::optional<Json>(spec["offset_mm"]) : std::nullopt;
        const Screen s(Json(kind), page_size, dpi, lpi, angle, shift_px(offset, dpi), shape);
        std::size_t i = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x, ++i) out[i] = s.at(box.x0 + x, box.y0 + y) < cover_at(i) ? static_cast<char>(255) : '\0';
        }
    }
    Image result = transparent(rgba.size());
    result.putalpha(Image::frombytes("L", rgba.size(), out));
    return result;
}

Image swatch(const Json& tone, Size size, int dpi, bool print_mode) {
    Json full = Json::object({{"pattern", "dot"}, {"gradient", nullptr}, {"lpi", 60.0}, {"density", 0.3}, {"angle", 45.0}});
    if (tone.is_object()) {
        for (const auto& [key, value] : tone.items()) full[key] = value;
    }
    Settings s;
    s.pattern = full["pattern"];
    s.gradient = full["gradient"];
    s.lpi = core::to_float(full["lpi"]);
    s.density = core::to_float(full["density"]);
    s.angle = core::to_float(full["angle"]);
    const auto keep = [&](const char* key, std::optional<Json>& slot) {
        if (full.contains(key)) slot = full[key];
    };
    keep("scale_mm", s.scale_mm);
    keep("tile_png", s.tile_png);
    keep("seed", s.seed);
    keep("dot_shape", s.dot_shape);
    keep("offset_mm", s.offset_mm);
    const Image where = Image::create("L", size, Ink(255));
    const Box box{0, 0, size.width, size.height};
    std::optional<Box> where_box;
    if (core::py_truthy(s.gradient)) where_box = where.getbbox();
    Image layer = transparent(size);
    layer.putalpha(tone_alpha(s, where, dpi, print_mode, size, box, where_box));
    return alpha_composite(Image::create("RGBA", size, Ink{255, 255, 255, 255}), layer).convert("RGB");
}

}  // namespace genko::render::tones
