#include "render/filters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>

#include "core/command_bus.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "render/draw.hpp"
#include "render/op_limits.hpp"

namespace genko::render::filters {

using core::Json;

// --- numpy's float32 sine and cosine -----------------------------------------------------------------------------------

namespace {

// (loops_trigonometric: the Cody–Waite constants and Myklebust's polynomials, every multiply-add fused)
constexpr float kTwoOverPi = 0x1.45f306p-1f;
constexpr float kPio2High = -0x1.921fb0p+00f;
constexpr float kPio2Med = -0x1.5110b4p-22f;
constexpr float kPio2Low = -0x1.846988p-48f;
constexpr float kRintMagic = 0x1.800000p+23f;

float cosine_poly(float x2) {
    float r = std::fmaf(0x1.98e616p-16f, x2, -0x1.6c06dcp-10f);
    r = std::fmaf(r, x2, 0x1.55553cp-05f);
    r = std::fmaf(r, x2, -0x1.000000p-01f);
    r = std::fmaf(r, x2, 0x1.000000p+00f);
    return r;
}

float sine_poly(float x, float x2) {
    float r = std::fmaf(0x1.7d3bbcp-19f, x2, -0x1.a06bbap-13f);
    r = std::fmaf(r, x2, 0x1.11119ap-07f);
    r = std::fmaf(r, x2, -0x1.555556p-03f);
    r = std::fmaf(r, x2, 0.0f);
    r = std::fmaf(r, x, x);
    return r;
}

// libm's sinf and cosf, called as numpy calls them past the vectorised range (never folded by the compiler)
float (*volatile g_sinf)(float) = [](float x) { return ::sinf(x); };
float (*volatile g_cosf)(float) = [](float x) { return ::cosf(x); };

float numpy_trig(float x, bool cosine) {
    if (std::isnan(x)) return std::numeric_limits<float>::quiet_NaN();
    const float limit = cosine ? 71476.0625f : 117435.992f;
    if (!(std::fabs(x) <= limit)) return cosine ? g_cosf(x) : g_sinf(x);
    volatile float magic = kRintMagic;  // (round to nearest by adding and taking away 1.5 × 2**23)
    float quadrant = x * kTwoOverPi;
    quadrant = quadrant + magic;
    quadrant = quadrant - magic;
    float reduced = std::fmaf(quadrant, kPio2High, x);
    reduced = std::fmaf(quadrant, kPio2Med, reduced);
    reduced = std::fmaf(quadrant, kPio2Low, reduced);
    const float reduced2 = reduced * reduced;
    const float c = cosine_poly(reduced2);
    const float s = sine_poly(reduced, reduced2);
    auto iquadrant = static_cast<std::int32_t>(std::nearbyint(quadrant));
    if (cosine) iquadrant += 1;
    float out = (iquadrant & 1) == 0 ? s : c;
    if ((iquadrant & 2) == 2) out = 0.0f - out;
    return out;
}

}  // namespace

float numpy_sinf(float x) { return numpy_trig(x, false); }
float numpy_cosf(float x) { return numpy_trig(x, true); }

// --- connected parts ------------------------------------------------------------------------------------------------

std::vector<std::int64_t> component_sizes(const BoolGrid& grid) {
    const int w = grid.width;
    const int h = grid.height;
    std::vector<std::int32_t> label(grid.cells.size(), -1);
    std::vector<std::int32_t> parent;
    const auto find = [&parent](std::int32_t i) {
        while (parent[static_cast<std::size_t>(i)] != i) {
            parent[static_cast<std::size_t>(i)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(i)])];
            i = parent[static_cast<std::size_t>(i)];
        }
        return i;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (grid.at(x, y) == 0) continue;
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            std::int32_t mine = -1;
            // the neighbours already seen (8-neighbour: left, and the three above)
            const int nx[4] = {x - 1, x - 1, x, x + 1};
            const int ny[4] = {y, y - 1, y - 1, y - 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w) continue;
                const std::int32_t other = label[static_cast<std::size_t>(ny[k]) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx[k])];
                if (other < 0) continue;
                if (mine < 0) {
                    mine = find(other);
                } else {
                    const std::int32_t a = find(mine);
                    const std::int32_t b = find(other);
                    if (a != b) parent[static_cast<std::size_t>(a)] = b;
                    mine = find(b);
                }
            }
            if (mine < 0) {
                mine = static_cast<std::int32_t>(parent.size());
                parent.push_back(mine);
            }
            label[at] = mine;
        }
    }
    std::vector<std::int64_t> count(parent.size(), 0);
    for (const std::int32_t l : label) {
        if (l >= 0) ++count[static_cast<std::size_t>(find(l))];
    }
    std::vector<std::int64_t> out(grid.cells.size(), 0);
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] >= 0) out[i] = count[static_cast<std::size_t>(find(label[i]))];
    }
    return out;
}

namespace {

// --- the settings, as Python reads them ------------------------------------------------------------------------------

// float(params.get(key, fallback))
double fparam(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : core::finite_float(*v, key);
}

// int(params.get(key, fallback)), the whole number it is
double iparam(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v == nullptr ? fallback : core::int_whole(*v, key);
}

Image keep_alpha(const Image& rgb, const Image& source) {
    Image out = rgb.convert("RGBA");
    out.putalpha(source.getchannel(3));
    return out;
}

// ImageEnhance.Contrast(image).enhance(factor) for an "RGB" picture
Image contrast(const Image& image, double factor) {
    const std::vector<long> h = image.convert("L").histogram();
    double sum = 0.0;
    long count = 0;
    for (int j = 0; j < 256; ++j) {
        sum += static_cast<double>(static_cast<long long>(j) * h[static_cast<std::size_t>(j)]);
        count += h[static_cast<std::size_t>(j)];
    }
    const double mean = count != 0 ? sum / static_cast<double>(count) : 0.0;
    const int grey = static_cast<int>(core::py_trunc(mean + 0.5));
    const Image degenerate = Image::create("L", image.size(), Ink(grey)).convert(image.mode());
    return blend(degenerate, image, factor);
}

// ImageEnhance.Brightness(image).enhance(factor) for an "RGB" picture
Image brightness(const Image& image, double factor) {
    return blend(Image::create(image.mode(), image.size(), Ink(0)), image, factor);
}

// a core::Adjustment laid on the picture (the colour adjustments of filters.apply_filter)
Image adjusted(const Image& rgba, const core::Adjustment& adjustment) {
    const auto& [first, second, third] = adjustment.tables;
    switch (adjustment.way) {
        case core::Adjustment::Way::Hsv: {
            const std::vector<Image> hsv = rgba.convert("RGB").convert("HSV").split();
            return keep_alpha(Image::merge("HSV", {hsv[0].point(first), hsv[1].point(second), hsv[2].point(third)}).convert("RGB"),
                              rgba);
        }
        case core::Adjustment::Way::Grey: {
            const Image grey = rgba.convert("L");
            if (first == second && second == third) return keep_alpha(grey.point(first).convert("RGB"), rgba);
            return keep_alpha(Image::merge("RGB", {grey.point(first), grey.point(second), grey.point(third)}), rgba);
        }
        case core::Adjustment::Way::Rgb: break;
    }
    std::vector<int> lut(first);
    lut.insert(lut.end(), second.begin(), second.end());
    lut.insert(lut.end(), third.begin(), third.end());
    return keep_alpha(rgba.convert("RGB").point(lut), rgba);
}

// arr[..., :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32): numpy's matmul loop adds the
// three products in order, each product and each sum rounded to float32 (no fused multiply-add: checked against
// numpy for every colour whose luminance is next to 128, test_contract_raster_ops filterEdges)
float luminance(unsigned char r, unsigned char g, unsigned char b) {
    const float red = static_cast<float>(r) * 0.299f;
    const float green = static_cast<float>(g) * 0.587f;
    const float blue = static_cast<float>(b) * 0.114f;
    const float sum = red + green;
    return sum + blue;
}

// filters.despeckle
Image despeckle(const Image& rgba, double size_px, std::string_view what) {
    std::string arr = rgba.tobytes();
    const int w = rgba.width();
    const int h = rgba.height();
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const auto ink_of = [&]() {
        BoolGrid ink(w, h);
        for (std::size_t i = 0; i < n; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(arr.data() + i * 4);
            ink.cells[i] = (p[3] >= 128 && luminance(p[0], p[1], p[2]) < 128.0f) ? 1 : 0;
        }
        return ink;
    };
    BoolGrid ink = ink_of();
    const double limit = core::py_pow(core::py_max(1.0, size_px), 2.0);
    std::size_t opaque_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(arr[i * 4 + 3]) > 250) ++opaque_count;
    }
    const bool opaque = n > 0 && static_cast<double>(opaque_count) / static_cast<double>(n) > 0.9;
    std::vector<std::string_view> parts;
    if (what == "both") {
        parts = {"ink", "holes"};
    } else {
        parts = {what};
    }
    for (const std::string_view part : parts) {
        BoolGrid target = ink;
        if (part != "ink") {
            for (unsigned char& c : target.cells) c = c != 0 ? 0 : 1;
        }
        const std::vector<std::int64_t> sizes = component_sizes(target);
        for (std::size_t i = 0; i < n; ++i) {
            if (target.cells[i] == 0 || static_cast<double>(sizes[i]) >= limit) continue;
            unsigned char* p = reinterpret_cast<unsigned char*>(arr.data() + i * 4);
            if (part == "ink") {
                const unsigned char v = opaque ? 255 : 0;
                p[0] = p[1] = p[2] = p[3] = v;
            } else {
                p[0] = p[1] = p[2] = 0;
                p[3] = 255;
            }
        }
        ink = ink_of();
    }
    return Image::frombytes("RGBA", rgba.size(), arr);
}

// float32(x), as numpy makes it: from the largest float and half a step on, an infinity (said, not left to the cast)
float to_float32(double x) {
    constexpr double kEdge = 3.4028235677973366e38;
    if (x >= kEdge) return HUGE_VALF;
    if (x <= -kEdge) return -HUGE_VALF;
    return static_cast<float>(x);
}

// filters._remap: the pixels fetched from (map_x, map_y) for each place (bilinear; outside: transparent), the maps
// float32 (as wave and twirl make them): the weights float64, as numpy mixes float32 and int64
Image remap(const Image& rgba, const std::vector<float>& map_x, const std::vector<float>& map_y) {
    const std::string src = rgba.tobytes();
    const std::int64_t w = rgba.width();
    const std::int64_t h = rgba.height();
    std::string out(src.size(), '\0');
    const auto sample = [&](std::int64_t yy, std::int64_t xx, int c) -> double {
        const bool inside = xx >= 0 && xx < w && yy >= 0 && yy < h;
        if (!inside) return 0.0;
        return static_cast<unsigned char>(src[static_cast<std::size_t>((yy * w + xx) * 4 + c)]);
    };
    for (std::int64_t i = 0; i < w * h; ++i) {
        const auto mx = static_cast<double>(map_x[static_cast<std::size_t>(i)]);
        const auto my = static_cast<double>(map_y[static_cast<std::size_t>(i)]);
        const double fx0 = std::floor(mx);
        const double fy0 = std::floor(my);
        // (a place off the picture, NaN or an infinity among them, has all four neighbours outside: transparent, as
        // numpy gives it, and not made a whole number)
        if (!(fx0 >= -1 && fx0 < static_cast<double>(w) && fy0 >= -1 && fy0 < static_cast<double>(h))) continue;
        const auto x0 = static_cast<std::int64_t>(fx0);
        const auto y0 = static_cast<std::int64_t>(fy0);
        const double fx = mx - fx0;
        const double fy = my - fy0;
        for (int c = 0; c < 4; ++c) {
            const double top = sample(y0, x0, c) * (1 - fx) + sample(y0, x0 + 1, c) * fx;
            const double bottom = sample(y0 + 1, x0, c) * (1 - fx) + sample(y0 + 1, x0 + 1, c) * fx;
            const double mixed = top * (1 - fy) + bottom * fy;
            const double v = std::min(std::max(mixed + 0.5, 0.0), 255.0);
            out[static_cast<std::size_t>(i * 4 + c)] = static_cast<char>(static_cast<unsigned char>(static_cast<int>(v)));
        }
    }
    return Image::frombytes("RGBA", rgba.size(), out);
}

// lineart.LineParams.from_dict
struct LineParams {
    int radius = 7;
    double threshold = 0.72;
    double min_px = 12;
    bool drop_blue = false;
    bool keep_solid = true;
    std::vector<std::int64_t> rgb{0, 0, 0};
};

LineParams line_params(const Json& data) {
    LineParams p;
    const double radius = iparam(data, "radius", 7);
    const Json* rgb_value = core::get(data, "rgb");
    const Json rgb = rgb_value != nullptr && core::py_truthy(*rgb_value) ? *rgb_value : Json::array({0, 0, 0});
    // radius if radius % 2 else radius + 1 (Python's % takes the sign of 2)
    const double odd = core::py_fmod(radius, 2.0) != 0.0 ? radius : radius + 1;
    const double size = core::py_max(3.0, odd);
    limits::check_count(size, static_cast<double>(limits::kRankSize), "radius");
    p.radius = static_cast<int>(size);
    p.threshold = core::py_max(0.05, core::py_min(0.99, fparam(data, "threshold", 0.72)));
    p.min_px = core::py_max(0.0, iparam(data, "min_px", 12));
    p.drop_blue = core::py_truthy(core::get_or(data, "drop_blue", Json(false)));
    p.keep_solid = core::py_truthy(core::get_or(data, "keep_solid", Json(true)));
    p.rgb.clear();
    const std::vector<Json> values = core::iterate(core::py_list(rgb));
    for (std::size_t i = 0; i < values.size() && i < 3; ++i) p.rgb.push_back(core::to_int(values[i]));
    return p;
}

// lineart.extract: the lines of a picture as ink on a transparent layer
Image lineart(const Image& image, const LineParams& params) {
    const Image rgb = image.convert("RGB");
    Image grey = rgb.convert("L");
    if (params.drop_blue) {  // (水色の下描き: where blue stands clearly above red, the mark is taken as paper)
        const std::string a = rgb.tobytes();
        std::string g = grey.tobytes();
        for (std::size_t i = 0; i < g.size(); ++i) {
            const int r = static_cast<unsigned char>(a[i * 3]);
            const int b = static_cast<unsigned char>(a[i * 3 + 2]);
            if (b - r > 30 && b > 90) g[i] = static_cast<char>(255);
        }
        grey = Image::frombytes("L", grey.size(), g);
    }
    const Image local_max = grey.filter(Filter::max_filter(params.radius));
    const std::string g = grey.tobytes();
    const std::string m = local_max.tobytes();
    const auto threshold = static_cast<float>(params.threshold);
    std::string ink_bytes(g.size(), '\0');
    for (std::size_t i = 0; i < g.size(); ++i) {
        const auto gv = static_cast<float>(static_cast<unsigned char>(g[i]));
        const auto mv = static_cast<float>(static_cast<unsigned char>(m[i]));
        const float ratio = mv == 0.0f ? 1.0f : gv / std::max(mv, 1.0f);
        ink_bytes[i] = ratio < threshold ? static_cast<char>(255) : '\0';
    }
    Image ink = Image::frombytes("L", grey.size(), ink_bytes);
    if (params.keep_solid) {  // (solid blacks are not lines, but keep them: a dark region whose local max is dark too)
        const Image solid = chops::multiply(grey.point([](int v) { return v < 40 ? 255 : 0; }),
                                            local_max.point([](int v) { return v < 80 ? 255 : 0; }));
        ink = chops::lighter(ink, solid);
    }
    if (params.min_px > 1) {  // _drop_specks
        const BoolGrid on = compare(ink, 0, true);
        const std::vector<std::int64_t> sizes = component_sizes(on);
        std::string arr = ink.tobytes();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (on.cells[i] != 0 && static_cast<double>(sizes[i]) < params.min_px) arr[i] = '\0';
        }
        ink = Image::frombytes("L", ink.size(), arr);
    }
    Image out = Image::create("RGBA", grey.size(), Ink::with_alpha(params.rgb, 0));
    out.putalpha(ink);
    return out;
}

// motion, radial and zoom blur
Image motion_blur(const Image& rgba, const Json& params) {
    const double distance = core::py_max(1.0, iparam(params, "distance", 12));
    const double angle = fparam(params, "angle", 0) * (core::kPi / 180.0);  // math.radians
    const int steps = static_cast<int>(core::py_max(2.0, core::py_min(64.0, distance)));
    Image acc;
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * distance;
        const double ox = core::py_round_whole(t * core::py_cos(angle));
        const double oy = core::py_round_whole(t * core::py_sin(angle));
        limits::check_coordinate(ox, "distance");
        limits::check_coordinate(oy, "distance");
        const Image shifted = chops::offset(rgba, static_cast<int>(ox), static_cast<int>(oy));
        acc = acc.empty() ? shifted : blend(acc, shifted, 1.0 / (k + 1));
    }
    return acc;
}

Image radial_zoom_blur(const Image& rgba, std::string_view kind, const Json& params) {
    const double cx = fparam(params, "cx", 0.5) * rgba.width();
    const double cy = fparam(params, "cy", 0.5) * rgba.height();
    const double amount = core::py_max(0.0, fparam(params, "amount", 0.08));
    const int steps = 10;
    Image acc;
    for (int k = 0; k < steps; ++k) {
        const double t = (static_cast<double>(k) / (steps - 1) - 0.5) * amount;
        Image moved;
        if (kind == "radial_blur") {
            moved = rgba.rotate(t * (180.0 / core::kPi), Resample::Bilinear, false, std::pair<double, double>{cx, cy});
        } else {
            const double scale = 1 + t;
            if (scale == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
            const double data[6] = {1 / scale, 0, cx - cx / scale, 0, 1 / scale, cy - cy / scale};
            moved = rgba.transform(rgba.size(), TransformMethod::Affine, data, Resample::Bilinear);
        }
        acc = acc.empty() ? moved : blend(acc, moved, 1.0 / (k + 1));
    }
    return acc;
}

Image noise(const Image& rgba, const Json& params) {
    const double amount = core::py_max(0.0, core::py_min(1.0, fparam(params, "amount", 0.15)));
    core::PyRandom rng = core::PyRandom::from_str(core::py_str(core::get_or(params, "seed", Json("genko"))));
    const auto grain = [&]() {
        std::string data(static_cast<std::size_t>(rgba.width()) * static_cast<std::size_t>(rgba.height()), '\0');
        for (char& c : data) c = static_cast<char>(static_cast<unsigned char>(rng.randrange(256)));
        return Image::frombytes("L", rgba.size(), data);
    };
    const Image rgb = rgba.convert("RGB");
    Image noisy;
    if (core::py_truthy(core::get_or(params, "mono", Json(true)))) {
        const Image g = grain();
        noisy = blend(rgb, Image::merge("RGB", {g, g, g}), amount);
    } else {  // colour noise: each channel its own grain
        const std::vector<Image> bands = rgb.split();
        std::vector<Image> mixed;
        for (const Image& band : bands) mixed.push_back(blend(band, grain(), amount));
        noisy = Image::merge("RGB", mixed);
    }
    return keep_alpha(noisy, rgba);
}

Image wave_twirl(const Image& rgba, std::string_view kind, const Json& params) {
    const int h = rgba.height();
    const int w = rgba.width();
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> map_x(n);
    std::vector<float> map_y(n);
    if (kind == "wave") {
        const auto amplitude = to_float32(fparam(params, "amplitude", 6));
        const auto wavelength = to_float32(core::py_max(2.0, fparam(params, "wavelength", 60)));
        const auto pi = static_cast<float>(core::kPi);
        // xs + amplitude * np.sin(ys * 2 * math.pi / wavelength), and the same for y: each step in float32
        const auto along = [&](float v) { return numpy_sinf(((v * 2.0f) * pi) / wavelength); };
        std::vector<float> by_y(static_cast<std::size_t>(h));
        std::vector<float> by_x(static_cast<std::size_t>(w));
        for (int y = 0; y < h; ++y) by_y[static_cast<std::size_t>(y)] = amplitude * along(static_cast<float>(y));
        for (int x = 0; x < w; ++x) by_x[static_cast<std::size_t>(x)] = amplitude * along(static_cast<float>(x));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                map_x[i] = static_cast<float>(x) + by_y[static_cast<std::size_t>(y)];
                map_y[i] = static_cast<float>(y) + by_x[static_cast<std::size_t>(x)];
            }
        }
    } else {
        const double cx_d = w / 2.0;
        const double cy_d = h / 2.0;
        const double radius_d = core::py_max(1.0, fparam(params, "radius", 0.45) * std::min(w, h));
        const double twist_d = fparam(params, "angle", 90) * (core::kPi / 180.0);
        const auto cx = to_float32(cx_d);
        const auto cy = to_float32(cy_d);
        const auto radius = to_float32(radius_d);
        const auto twist = to_float32(twist_d);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                float share = 1.0f - dist / radius;
                share = std::min(std::max(share, 0.0f), 1.0f);  // np.clip
                const float turn = twist * (share * share);
                const float cos_t = numpy_cosf(turn);
                const float sin_t = numpy_sinf(turn);
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                map_x[i] = (cx + dx * cos_t) - dy * sin_t;
                map_y[i] = (cy + dx * sin_t) + dy * cos_t;
            }
        }
    }
    return remap(rgba, map_x, map_y);
}

Image rain(const Image& rgba, const Json& params) {
    const Image layer = rain_layer(rgba.size(), params);
    Image out = rgba.copy();
    out.alpha_composite(layer);
    out.putalpha(chops::lighter(rgba.getchannel(3), layer.getchannel(3)));
    return out;
}

}  // namespace

Image rain_layer(Size picture, const Json& params) {
    core::PyRandom rng = core::PyRandom::from_str(core::py_str(core::get_or(params, "seed", Json("rain"))));
    const int w = picture.width;
    const int h = picture.height;
    const double count = core::py_max(1.0, core::py_min(20000.0, core::py_trunc(fparam(params, "count", 400))));
    const double length = core::py_max(2.0, fparam(params, "length", 40));
    limits::check_count(length, limits::kStreak, "length");
    const double width = core::py_max(1.0, core::py_round_whole(fparam(params, "width", 1)));
    limits::check_count(width, limits::kReach, "width");
    const double angle = (90 + fparam(params, "angle", 15)) * (core::kPi / 180.0);
    std::vector<std::int64_t> colour;
    const Json* rgb = core::get(params, "rgb");
    for (const Json& v : core::iterate(rgb != nullptr && core::py_truthy(*rgb) ? *rgb : Json::array({255, 255, 255}))) {
        colour.push_back(core::to_int(v));
    }
    if (colour.size() > 3) colour.resize(3);
    const double opacity = core::py_max(0.0, core::py_min(1.0, fparam(params, "opacity", 0.7)));
    Image layer = Image::create("RGBA", picture, Ink{0, 0, 0, 0});
    {
        Draw draw(layer);
        const double dx = core::py_cos(angle);
        const double dy = core::py_sin(angle);
        for (int k = 0; k < static_cast<int>(count); ++k) {
            const double x = rng.uniform(-length, w + length);
            const double y = rng.uniform(-length, h);
            const double size = length * (0.5 + rng.random());
            const double alpha = core::py_round_whole(255 * opacity * (0.5 + 0.5 * rng.random()));
            const std::vector<PointD> line{{x, y}, {x - dx * size, y + dy * size}};
            draw.line(line, Ink::with_alpha(colour, static_cast<std::int64_t>(alpha)), static_cast<int>(width));
        }
    }
    return layer;
}

namespace {

Image glow(const Image& rgba, const Json& params) {
    const double radius = core::py_max(0.5, fparam(params, "radius", 12));
    limits::check_count(radius, limits::kReach, "radius");
    const double amount = core::py_max(0.0, core::py_min(3.0, fparam(params, "amount", 0.8)));
    const double cut = iparam(params, "threshold", 170);
    const Image rgb = rgba.convert("RGB");
    const Image bright = rgb.convert("L").point([cut](int v) { return v >= cut ? 255 : 0; });
    const Image light = composite(rgb, Image::create("RGB", rgb.size(), Ink{0, 0, 0}), bright).filter(Filter::gaussian_blur(radius));
    const Image glowing = brightness(light, amount);
    return keep_alpha(chops::screen(rgb, glowing), rgba);
}

Image sharpen(const Image& rgba, const Json& params) {
    const Json* amount_value = core::get(params, "amount");
    if (amount_value != nullptr && !amount_value->is_null()) {  // (シャープの強さ: 1 is the usual, 2 the strong one)
        const double amount = core::py_max(0.1, core::py_min(5.0, core::finite_float(*amount_value, "amount")));
        const Image rgb = rgba.convert("RGB");
        const double radius = core::py_max(0.5, fparam(params, "radius", 2));
        limits::check_count(radius, limits::kReach, "radius");
        const int percent = static_cast<int>(core::py_round_int(120 * amount));
        return keep_alpha(rgb.filter(Filter::unsharp_mask(radius, percent, 2)), rgba);
    }
    Image rgb = rgba.convert("RGB").point([](int p) { return std::min(255, p + 12); });
    rgb = contrast(rgb, 1.3);
    rgb = rgb.filter(Filter::unsharp_mask(2, 150, 3));
    return keep_alpha(rgb, rgba);
}

Image mosaic(const Image& rgba, const Json& params) {
    const double block = core::py_max(2.0, iparam(params, "block", 8));
    const int w = rgba.width();
    const int h = rgba.height();
    const auto small_w = static_cast<int>(std::max(1.0, std::floor(w / block)));
    const auto small_h = static_cast<int>(std::max(1.0, std::floor(h / block)));
    const Image small = rgba.resize(Size{small_w, small_h}, Resample::Nearest);
    const Image out = small.resize(Size{w, h}, Resample::Nearest);
    return keep_alpha(brightness(out.convert("RGB"), 0.88), rgba);
}

}  // namespace

Image apply_filter(const Image& image, std::string_view kind, const Json& params_in) {
    const Json params = params_in.is_null() ? Json::object() : params_in;
    const Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    if (kind == "blur") {
        const double radius = fparam(params, "radius", 2);
        limits::check_count(std::fabs(radius), limits::kReach, "radius");
        if (radius == 0.0) return rgba.copy();  // (GaussianBlur((0, 0)): a copy)
        return rgba.filter(Filter::gaussian_blur(radius));
    }
    if (kind == "sharpen") return sharpen(rgba, params);
    if (kind == "glow") return glow(rgba, params);
    if (kind == "rain") return rain(rgba, params);
    if (kind == "despeckle") {
        const double dpi = fparam(params, "dpi", 200);
        const Json* size_px = core::get(params, "size_px");
        const double size = size_px != nullptr && !size_px->is_null() ? core::finite_float(*size_px, "size_px")
                                                                      : fparam(params, "size_mm", 0.3) / 25.4 * dpi;
        const Json* what_value = core::get(params, "what");
        const std::string what = what_value != nullptr && core::py_truthy(*what_value) ? core::py_str(*what_value) : "ink";
        if (what != "ink" && what != "holes" && what != "both") throw core::PyValueError("despeckle what must be ink, holes or both");
        return despeckle(rgba, size, what);
    }
    if (kind == "mosaic") return mosaic(rgba, params);
    if (kind == "motion_blur") return motion_blur(rgba, params);
    if (kind == "radial_blur" || kind == "zoom_blur") return radial_zoom_blur(rgba, kind, params);
    if (kind == "noise") return noise(rgba, params);
    if (kind == "wave" || kind == "twirl") return wave_twirl(rgba, kind, params);
    if (kind == "lineart") {
        Image white = Image::create("RGBA", rgba.size(), Ink{255, 255, 255, 255});
        white.alpha_composite(rgba);
        return lineart(white, line_params(params));
    }
    // the colour adjustments (levels, curve, hue, invert, posterize, threshold, gradient_map, bitonal,
    // brightness_contrast): their tables are core's, as a correction layer has them
    if (std::find(core::kAdjustments.begin(), core::kAdjustments.end(), kind) != core::kAdjustments.end()) {
        return adjusted(rgba, core::adjustment(kind, params));
    }
    throw core::PyValueError("unknown filter " + std::string(kind));
}

Image within(const Image& original, const Image& filtered, const Image& mask) {
    const Image m = mask.mode() == "L" ? mask : mask.convert("L");
    return composite(filtered.mode() == "RGBA" ? filtered : filtered.convert("RGBA"),
                     original.mode() == "RGBA" ? original : original.convert("RGBA"), m.resize(original.size()));
}

}  // namespace genko::render::filters
