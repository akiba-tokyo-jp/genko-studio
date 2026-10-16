#include "render/quantize.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>

#include "core/error.hpp"
#include "render/libm_float.hpp"

// PhotoCraft v0.2.0 crates/algo/src/quantize.rs (MIT) as it computes in Rust: f32 arithmetic, its median cut's and
// k-means' sums in f64, Rust's round (half away from zero), its stable sorts by total order. Median cut follows Heckbert
// (1982); k-means refinement is Lloyd's algorithm; error diffusion is Floyd & Steinberg (1976).

namespace genko::render::quantize {

namespace {

std::uint8_t to8(float v) { return static_cast<std::uint8_t>(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

Rgb8 rgb8(const Rgba& p) { return {to8(p[0]), to8(p[1]), to8(p[2])}; }

// Rust's f32::total_cmp as a key: ordered the way it orders.
std::int32_t total_key(float v) {
    auto bits = std::bit_cast<std::int32_t>(v);
    bits ^= static_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 31) >> 1);
    return bits;
}

std::vector<Rgb8> forced_colors(Forced f) {
    switch (f) {
        case Forced::None: return {};
        case Forced::BlackWhite: return {{0, 0, 0}, {255, 255, 255}};
        case Forced::Primaries:
            return {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255}, {255, 0, 255}, {255, 255, 0}};
        case Forced::Web: return web_palette();
    }
    return {};
}

// The 8-bit colours of the pixels that show (alpha at least ½) and their counts, in the colours' order.
std::vector<std::pair<Rgb8, std::uint32_t>> histogram(const std::vector<Rgba>& px) {
    std::map<Rgb8, std::uint32_t> counts;
    for (const Rgba& p : px) {
        if (p[3] >= 0.5f) ++counts[rgb8(p)];
    }
    return {counts.begin(), counts.end()};
}

using Point3 = std::array<float, 3>;
using Weighted = std::pair<Point3, std::uint32_t>;

// Median cut over weighted points into at most n boxes: the boxes' weighted means.
std::vector<Point3> median_cut(const std::vector<Weighted>& pts, std::size_t n) {
    if (pts.empty() || n == 0) return {};
    std::vector<std::vector<Weighted>> boxes{pts};
    while (boxes.size() < n) {
        // (the box of the largest weighted extent: the first of them)
        std::size_t bi = static_cast<std::size_t>(-1);
        std::size_t axis = 0;
        float score = -1.0f;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const std::vector<Weighted>& b = boxes[i];
            if (b.size() <= 1) continue;
            std::size_t best_axis = 0;
            float best = -1.0f;
            for (std::size_t a = 0; a < 3; ++a) {
                float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
                for (const Weighted& p : b) {
                    lo = std::min(lo, p.first[a]);
                    hi = std::max(hi, p.first[a]);
                }
                if (hi - lo > best) {
                    best_axis = a;
                    best = hi - lo;
                }
            }
            float w = 0.0f;
            for (const Weighted& p : b) w += static_cast<float>(p.second);
            const float s = best * std::sqrt(w);
            if (s > score) {
                bi = i;
                axis = best_axis;
                score = s;
            }
        }
        if (bi == static_cast<std::size_t>(-1) || score <= 0.0f) break;
        // (Vec::swap_remove: the last box in its place)
        std::vector<Weighted> b = std::move(boxes[bi]);
        boxes[bi] = std::move(boxes.back());
        boxes.pop_back();
        std::stable_sort(b.begin(), b.end(), [axis](const Weighted& p, const Weighted& q) { return total_key(p.first[axis]) < total_key(q.first[axis]); });
        std::uint64_t total = 0;
        for (const Weighted& p : b) total += p.second;
        std::uint64_t acc = 0;
        std::size_t cut = 1;
        for (std::size_t i = 0; i < b.size(); ++i) {
            acc += b[i].second;
            if (acc * 2 >= total) {
                cut = std::clamp<std::size_t>(i + 1, 1, b.size() - 1);
                break;
            }
        }
        std::vector<Weighted> rest(b.begin() + static_cast<std::ptrdiff_t>(cut), b.end());
        b.resize(cut);
        boxes.push_back(std::move(b));
        boxes.push_back(std::move(rest));
    }
    std::vector<Point3> out;
    for (const std::vector<Weighted>& b : boxes) {
        double w = 0.0;
        for (const Weighted& p : b) w += static_cast<double>(p.second);
        Point3 mean{};
        for (std::size_t a = 0; a < 3; ++a) {
            double sum = 0.0;
            for (const Weighted& p : b) sum += static_cast<double>(p.first[a]) * static_cast<double>(p.second);
            mean[a] = static_cast<float>(sum / w);
        }
        out.push_back(mean);
    }
    return out;
}

std::size_t nearest_f(const std::vector<Point3>& centers, const Point3& p) {
    float best = std::numeric_limits<float>::max();
    std::size_t at = 0;
    for (std::size_t i = 0; i < centers.size(); ++i) {
        const Point3& c = centers[i];
        const float d0 = c[0] - p[0], d1 = c[1] - p[1], d2 = c[2] - p[2];
        const float d = d0 * d0 + d1 * d1 + d2 * d2;
        if (d < best) {
            best = d;
            at = i;
        }
    }
    return at;
}

// Lloyd's refinement of the centres over weighted points.
void kmeans(const std::vector<Weighted>& pts, std::vector<Point3>& centers, int iters) {
    if (centers.empty()) return;  // (the forced colours fill the palette: nothing to refine)
    for (int it = 0; it < iters; ++it) {
        std::vector<std::array<double, 3>> sum(centers.size(), std::array<double, 3>{});
        std::vector<double> count(centers.size(), 0.0);
        for (const Weighted& p : pts) {
            const std::size_t i = nearest_f(centers, p.first);
            for (std::size_t a = 0; a < 3; ++a) sum[i][a] += static_cast<double>(p.first[a]) * static_cast<double>(p.second);
            count[i] += static_cast<double>(p.second);
        }
        for (std::size_t i = 0; i < centers.size(); ++i) {
            if (count[i] > 0.0) {
                for (std::size_t a = 0; a < 3; ++a) centers[i][a] = static_cast<float>(sum[i][a] / count[i]);
            }
        }
    }
}

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : libm::powf((v + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * libm::powf(v, 1.0f / 2.4f) - 0.055f; }

constexpr std::array<float, 3> kD50{0.96422f, 1.0f, 0.82521f};

std::size_t nearest(const std::vector<Rgb8>& palette, const std::array<float, 3>& c) {
    float best = std::numeric_limits<float>::max();
    std::size_t at = 0;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        float d = 0.0f;
        for (std::size_t k = 0; k < 3; ++k) {
            const float e = c[k] * 255.0f - static_cast<float>(palette[i][k]);
            d += e * e;
        }
        if (d < best) {
            best = d;
            at = i;
        }
    }
    return at;
}

float hash01(std::size_t x, std::size_t y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65535.0f;
}

// A halftone cell's spot function at (u, v) in -1..1, 0..1.
float spot(HalftoneShape shape, float u, float v) {
    float s = 0.0f;
    switch (shape) {
        case HalftoneShape::Round: s = 1.0f - (u * u + v * v) / 2.0f; break;
        case HalftoneShape::Ellipse: s = 1.0f - (u * u + 0.6f * v * v) / 1.6f; break;
        case HalftoneShape::Line: s = 1.0f - std::abs(v); break;
        case HalftoneShape::Square: s = 1.0f - std::max(std::abs(u), std::abs(v)); break;
        case HalftoneShape::Diamond: s = 1.0f - (std::abs(u) + std::abs(v)) / 2.0f; break;
        case HalftoneShape::Cross: s = 1.0f - std::min(std::abs(u), std::abs(v)); break;
    }
    return std::clamp(s, 0.0f, 1.0f);
}

// Rust's f32::rem_euclid for a positive divisor.
float rem_euclid(float a, float b) {
    const float r = std::fmod(a, b);
    return r < 0.0f ? r + std::abs(b) : r;
}

}  // namespace

std::optional<PaletteKind> palette_kind(std::string_view s) {
    if (s == "exact") return PaletteKind::Exact;
    if (s == "system" || s == "systemMac" || s == "systemMacOs") return PaletteKind::SystemMac;
    if (s == "systemWindows") return PaletteKind::SystemWindows;
    if (s == "web") return PaletteKind::Web;
    if (s == "uniform") return PaletteKind::Uniform;
    if (s == "perceptual" || s == "localPerceptual") return PaletteKind::Perceptual;
    if (s == "selective" || s == "localSelective") return PaletteKind::Selective;
    if (s == "adaptive" || s == "localAdaptive") return PaletteKind::Adaptive;
    return std::nullopt;
}

std::optional<Forced> forced_of(std::string_view s) {
    if (s == "none") return Forced::None;
    if (s == "blackWhite" || s == "blackAndWhite") return Forced::BlackWhite;
    if (s == "primaries") return Forced::Primaries;
    if (s == "web") return Forced::Web;
    return std::nullopt;
}

std::optional<Dither> dither_of(std::string_view s) {
    if (s == "none") return Dither::None;
    if (s == "diffusion") return Dither::Diffusion;
    if (s == "pattern") return Dither::Pattern;
    if (s == "noise") return Dither::Noise;
    return std::nullopt;
}

std::vector<Rgb8> web_palette() {
    std::vector<Rgb8> v;
    v.reserve(216);
    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) v.push_back({static_cast<std::uint8_t>(r * 51), static_cast<std::uint8_t>(g * 51), static_cast<std::uint8_t>(b * 51)});
        }
    }
    return v;
}

std::vector<Rgb8> mac_palette() {
    std::vector<Rgb8> v = web_palette();
    // (the ramps take the values between the cube's steps: 17 apart, the multiples of 51 left out)
    std::vector<std::uint8_t> steps;
    for (int k = 1; k <= 15; ++k) {
        if ((k * 17) % 51 != 0) steps.push_back(static_cast<std::uint8_t>(k * 17));
    }
    for (int ramp = 0; ramp < 4; ++ramp) {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            const std::uint8_t s = *it;
            v.push_back(ramp == 0 ? Rgb8{s, 0, 0} : ramp == 1 ? Rgb8{0, s, 0} : ramp == 2 ? Rgb8{0, 0, s} : Rgb8{s, s, s});
        }
    }
    if (v.size() > 256) v.resize(256);
    return v;
}

std::vector<Rgb8> windows_palette() {
    std::vector<Rgb8> v{{0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},   {0, 0, 128},     {128, 0, 128},   {0, 128, 128},
                        {192, 192, 192}, {192, 220, 192}, {166, 202, 240}, {255, 251, 240}, {160, 160, 164}, {128, 128, 128}, {255, 0, 0},
                        {0, 255, 0},     {255, 255, 0},   {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
    for (const Rgb8& c : web_palette()) {
        if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);
    }
    std::uint8_t g = 8;
    while (v.size() < 256) {
        const Rgb8 grey{g, g, g};
        if (std::find(v.begin(), v.end(), grey) == v.end()) v.push_back(grey);
        g = static_cast<std::uint8_t>(g + 12);
    }
    return v;
}

std::vector<Rgb8> uniform_palette(std::size_t n) {
    const auto k = std::max<std::size_t>(static_cast<std::size_t>(std::floor(libm::cbrtf(static_cast<float>(n)))), 2);
    const auto level = [k](std::size_t i) {
        return static_cast<std::uint8_t>(std::round(static_cast<float>(i) / static_cast<float>(k - 1) * 255.0f));
    };
    std::vector<Rgb8> v;
    v.reserve(k * k * k);
    for (std::size_t r = 0; r < k; ++r) {
        for (std::size_t g = 0; g < k; ++g) {
            for (std::size_t b = 0; b < k; ++b) v.push_back({level(r), level(g), level(b)});
        }
    }
    return v;
}

std::optional<std::vector<Rgb8>> preset_table(std::string_view name) {
    const auto ramp = [](auto f) {
        std::vector<Rgb8> out;
        for (int i = 0; i < 256; ++i) {
            const std::array<float, 3> c = f(static_cast<float>(i) / 255.0f);
            out.push_back({to8(c[0]), to8(c[1]), to8(c[2])});
        }
        return out;
    };
    if (name == "grayscale") return ramp([](float t) { return std::array<float, 3>{t, t, t}; });
    if (name == "blackBody") {  // (black → red → yellow → white: incandescence)
        return ramp([](float t) {
            return std::array<float, 3>{std::min(t * 3.0f, 1.0f), std::clamp(t * 3.0f - 1.0f, 0.0f, 1.0f), std::clamp(t * 3.0f - 2.0f, 0.0f, 1.0f)};
        });
    }
    if (name == "spectrum") {
        return ramp([](float t) {
            const float h = t * 300.0f / 60.0f;
            const float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
            switch (static_cast<int>(h)) {
                case 0: return std::array<float, 3>{1.0f, x, 0.0f};
                case 1: return std::array<float, 3>{x, 1.0f, 0.0f};
                case 2: return std::array<float, 3>{0.0f, 1.0f, x};
                case 3: return std::array<float, 3>{0.0f, x, 1.0f};
                default: return std::array<float, 3>{x, 0.0f, 1.0f};
            }
        });
    }
    if (name == "systemMac" || name == "system") return mac_palette();
    if (name == "systemWindows") return windows_palette();
    if (name == "web") return web_palette();
    return std::nullopt;
}

std::array<float, 3> srgb_to_lab(const std::array<float, 3>& rgb) {
    const std::array<float, 3> lin{srgb_to_linear(rgb[0]), srgb_to_linear(rgb[1]), srgb_to_linear(rgb[2])};
    constexpr float m[3][3] = {{0.436074f, 0.385064f, 0.143080f}, {0.222504f, 0.716878f, 0.060618f}, {0.013932f, 0.097104f, 0.714173f}};
    std::array<float, 3> xyz{};
    for (int i = 0; i < 3; ++i) xyz[i] = (m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2]) / kD50[i];
    // ((6/29)^3 as the Rust build has it: LLVM folds powi of a constant through double; ^2 is the same either way)
    const float six = 6.0f / 29.0f, six2 = six * six, six3 = static_cast<float>(static_cast<double>(six) * six * six);
    const auto f = [six2, six3](float t) { return t > six3 ? libm::cbrtf(t) : t / (3.0f * six2) + 4.0f / 29.0f; };
    const float fx = f(xyz[0]), fy = f(xyz[1]), fz = f(xyz[2]);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

std::array<float, 3> lab_to_srgb(const std::array<float, 3>& lab) {
    const float fy = (lab[0] + 16.0f) / 116.0f;
    const float fx = fy + lab[1] / 500.0f;
    const float fz = fy - lab[2] / 200.0f;
    const float six = 6.0f / 29.0f;
    const auto finv = [six](float t) { return t > six ? t * t * t : 3.0f * (six * six) * (t - 4.0f / 29.0f); };
    const std::array<float, 3> xyz{finv(fx) * kD50[0], finv(fy) * kD50[1], finv(fz) * kD50[2]};
    // (D50 XYZ → linear sRGB, the Bradford-adapted matrix)
    constexpr float m[3][3] = {{3.133856f, -1.616867f, -0.490615f}, {-0.978768f, 1.916142f, 0.033454f}, {0.071945f, -0.228991f, 1.405243f}};
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i) {
        const float lin = m[i][0] * xyz[0] + m[i][1] * xyz[1] + m[i][2] * xyz[2];
        out[i] = linear_to_srgb(std::clamp(lin, 0.0f, 1.0f));
    }
    return out;
}

std::vector<Rgb8> build_palette(const std::vector<Rgba>& px, PaletteKind kind, std::size_t colors, Forced forced) {
    colors = std::clamp<std::size_t>(colors, 2, 256);
    std::vector<Rgb8> pal = forced_colors(forced);
    const std::size_t room = colors > pal.size() ? colors - pal.size() : 0;
    const auto hist = histogram(px);
    std::vector<Rgb8> computed;
    const auto all_of_them = [&] {
        std::vector<Rgb8> out;
        for (const auto& [c, n] : hist) out.push_back(c);
        return out;
    };
    switch (kind) {
        case PaletteKind::Exact:
            if (hist.size() > 256) {
                throw core::Error("value", "the image has " + std::to_string(hist.size()) + " colours; Exact needs 256 or fewer");
            }
            computed = all_of_them();
            break;
        case PaletteKind::SystemMac: computed = mac_palette(); break;
        case PaletteKind::SystemWindows: computed = windows_palette(); break;
        case PaletteKind::Web: computed = web_palette(); break;
        case PaletteKind::Uniform: computed = uniform_palette(std::max<std::size_t>(room, 8)); break;
        case PaletteKind::Adaptive:
            if (hist.size() <= room) {
                computed = all_of_them();
            } else {
                std::vector<Weighted> pts;
                for (const auto& [c, n] : hist) pts.push_back({{static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])}, n});
                std::vector<Point3> centers = median_cut(pts, room);
                kmeans(pts, centers, 2);
                for (const Point3& c : centers) {
                    computed.push_back({static_cast<std::uint8_t>(std::clamp(std::round(c[0]), 0.0f, 255.0f)),
                                        static_cast<std::uint8_t>(std::clamp(std::round(c[1]), 0.0f, 255.0f)),
                                        static_cast<std::uint8_t>(std::clamp(std::round(c[2]), 0.0f, 255.0f))});
                }
            }
            break;
        case PaletteKind::Perceptual:
        case PaletteKind::Selective:
            if (hist.size() <= room) {
                computed = all_of_them();
            } else {
                std::vector<Weighted> pts;
                for (const auto& [c, n] : hist) {
                    pts.push_back({srgb_to_lab({static_cast<float>(c[0]) / 255.0f, static_cast<float>(c[1]) / 255.0f, static_cast<float>(c[2]) / 255.0f}), n});
                }
                std::vector<Point3> centers = median_cut(pts, room);
                kmeans(pts, centers, 4);
                for (const Point3& c : centers) {
                    const std::array<float, 3> s = lab_to_srgb(c);
                    computed.push_back({to8(s[0]), to8(s[1]), to8(s[2])});
                }
                if (kind == PaletteKind::Selective) {  // (favour web-safe colours: an entry within 10 levels of one snaps to it)
                    for (Rgb8& c : computed) {
                        Rgb8 snap{};
                        for (std::size_t k = 0; k < 3; ++k) snap[k] = static_cast<std::uint8_t>(std::round(static_cast<float>(c[k]) / 51.0f) * 51.0f);
                        bool near = true;
                        for (std::size_t k = 0; k < 3; ++k) near = near && std::abs(int(c[k]) - int(snap[k])) <= 10;
                        if (near) c = snap;
                    }
                }
            }
            break;
    }
    for (const Rgb8& c : computed) {
        if (pal.size() >= 256) break;
        if (std::find(pal.begin(), pal.end(), c) == pal.end()) pal.push_back(c);
    }
    if (kind == PaletteKind::Adaptive || kind == PaletteKind::Perceptual || kind == PaletteKind::Selective || kind == PaletteKind::Uniform) {
        const std::size_t keep = std::max(colors, forced_colors(forced).size());
        if (pal.size() > keep) pal.resize(keep);
    }
    if (pal.empty()) pal.push_back({0, 0, 0});
    return pal;
}

float bayer8(std::size_t x, std::size_t y) {
    static constexpr std::uint8_t kM[64] = {0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46,
                                            6,  38, 60, 28, 52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27,
                                            49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};
    return (static_cast<float>(kM[(y % 8) * 8 + x % 8]) + 0.5f) / 64.0f - 0.5f;
}

std::vector<std::uint8_t> quantize(std::vector<Rgba>& px, std::size_t w, const std::vector<Rgb8>& pal, Dither dither, float amount,
                                   std::optional<std::size_t> transparent) {
    const std::size_t h = w == 0 ? 0 : px.size() / w;
    std::vector<std::uint8_t> idx(px.size(), 0);
    // (the palette's typical spacing sets the ordered and noise dithers' amplitude)
    const float spread = std::min(1.0f / std::max(libm::cbrtf(static_cast<float>(pal.size())), 1.0f), 0.5f) * std::clamp(amount, 0.0f, 1.0f);
    std::vector<std::array<float, 3>> err(2 * (w + 2), std::array<float, 3>{});
    for (std::size_t y = 0; y < h; ++y) {
        if (dither == Dither::Diffusion) {  // (rows ping-pong: cur is row y, next row y + 1, offset by one for x = -1)
            std::copy(err.begin() + static_cast<std::ptrdiff_t>(w + 2), err.end(), err.begin());
            std::fill(err.begin() + static_cast<std::ptrdiff_t>(w + 2), err.end(), std::array<float, 3>{});
        }
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            const Rgba p = px[i];
            if (p[3] < 0.5f) {
                if (transparent) {
                    idx[i] = static_cast<std::uint8_t>(*transparent);
                    px[i] = {0.0f, 0.0f, 0.0f, 0.0f};
                    continue;
                }
                if (p[3] <= 0.0f) continue;
            }
            std::array<float, 3> c{p[0], p[1], p[2]};
            switch (dither) {
                case Dither::None: break;
                case Dither::Diffusion:
                    for (std::size_t k = 0; k < 3; ++k) c[k] += err[x + 1][k];
                    break;
                case Dither::Pattern: {
                    const float t = bayer8(x, y) * spread;
                    for (float& v : c) v += t;
                    break;
                }
                case Dither::Noise: {
                    const float t = (hash01(x, y, 7) - 0.5f) * spread;
                    for (float& v : c) v += t;
                    break;
                }
            }
            for (float& v : c) v = std::clamp(v, 0.0f, 1.0f);
            const std::size_t k = nearest(pal, c);
            idx[i] = static_cast<std::uint8_t>(k);
            const std::array<float, 3> e{static_cast<float>(pal[k][0]) / 255.0f, static_cast<float>(pal[k][1]) / 255.0f,
                                         static_cast<float>(pal[k][2]) / 255.0f};
            if (dither == Dither::Diffusion) {
                const float a = std::clamp(amount, 0.0f, 1.0f);
                for (std::size_t q = 0; q < 3; ++q) {
                    const float d = (c[q] - e[q]) * a;
                    err[x + 2][q] += d * 7.0f / 16.0f;  // (cur)
                    err[w + 2 + x][q] += d * 3.0f / 16.0f;  // (next)
                    err[w + 2 + x + 1][q] += d * 5.0f / 16.0f;
                    err[w + 2 + x + 2][q] += d / 16.0f;
                }
            }
            px[i] = {e[0], e[1], e[2], transparent ? 1.0f : p[3]};
        }
    }
    return idx;
}

HalftoneShape halftone_shape(std::string_view s) {
    if (s == "ellipse") return HalftoneShape::Ellipse;
    if (s == "line") return HalftoneShape::Line;
    if (s == "square") return HalftoneShape::Square;
    if (s == "diamond") return HalftoneShape::Diamond;
    if (s == "cross") return HalftoneShape::Cross;
    return HalftoneShape::Round;
}

void to_bitmap(std::vector<float>& grey, std::size_t w, const BitmapMethod& method) {
    const std::size_t h = w == 0 ? 0 : grey.size() / w;
    switch (method.kind) {
        case BitmapMethod::Kind::Threshold:
            for (float& v : grey) v = v >= 0.5f ? 1.0f : 0.0f;
            return;
        case BitmapMethod::Kind::Pattern:
            for (std::size_t i = 0; i < grey.size(); ++i) grey[i] = grey[i] > bayer8(i % w, i / w) + 0.5f ? 1.0f : 0.0f;
            return;
        case BitmapMethod::Kind::Diffusion: {
            std::vector<float> buf = grey;
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    const std::size_t i = y * w + x;
                    const float old = buf[i];
                    const float now = old >= 0.5f ? 1.0f : 0.0f;
                    const float e = old - now;
                    grey[i] = now;
                    if (x + 1 < w) buf[i + 1] += e * 7.0f / 16.0f;
                    if (y + 1 < h) {
                        if (x > 0) buf[i + w - 1] += e * 3.0f / 16.0f;
                        buf[i + w] += e * 5.0f / 16.0f;
                        if (x + 1 < w) buf[i + w + 1] += e / 16.0f;
                    }
                }
            }
            return;
        }
        case BitmapMethod::Kind::Halftone: {
            const float cell = std::max(method.cell, 2.0f);
            constexpr float kRadiansPerDegree = 3.14159265358979323846f / 180.0f;  // (Rust's f32::to_radians)
            const float radians = method.angle * kRadiansPerDegree;
            const float s = libm::sinf(radians), c = libm::cosf(radians);
            // (the spot function ranked over the cell, so the cover follows the tone)
            std::vector<float> table(32 * 32);
            for (std::size_t k = 0; k < table.size(); ++k) {
                table[k] = spot(method.shape, (static_cast<float>(k % 32) + 0.5f) / 16.0f - 1.0f, (static_cast<float>(k / 32) + 0.5f) / 16.0f - 1.0f);
            }
            std::stable_sort(table.begin(), table.end(), [](float a, float b) { return total_key(a) < total_key(b); });
            const auto cdf = [&table](float x) {
                const auto at = std::partition_point(table.begin(), table.end(), [x](float t) { return t < x; });
                return static_cast<float>(at - table.begin()) / static_cast<float>(table.size());
            };
            for (std::size_t i = 0; i < grey.size(); ++i) {
                const float x = static_cast<float>(i % w) + 0.5f, y = static_cast<float>(i / w) + 0.5f;
                const float rx = x * c + y * s, ry = -x * s + y * c;
                const float u = rem_euclid(rx / cell, 1.0f) * 2.0f - 1.0f;
                const float v = rem_euclid(ry / cell, 1.0f) * 2.0f - 1.0f;
                // (ink — black — covers where the spot function passes the grey)
                grey[i] = cdf(spot(method.shape, u, v)) >= grey[i] ? 0.0f : 1.0f;
            }
            return;
        }
    }
}

}  // namespace genko::render::quantize
