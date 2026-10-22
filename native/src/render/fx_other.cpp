// PC4 節 D1 (SPEC.md COLOR-08): フィルター → その他's kernels — カスタム, ハイパス, HSB/HSL, 明るさの最大値・最小値 (render/fx_rank:
// running extremes) and スクロール. PhotoCraft v0.2.0's algo/src/other.rs (min_max_preserve, offset), other2.rs (custom,
// rgb_to_hsb, rgb_to_hsl, hsb_to_rgb, hsl_to_rgb, hsb_hsl) and sharpen.rs (high_pass), MIT:
// native/third_party/photocraft/NOTICE.md. カスタム's 5 × 5 grid and スクロール's distances are working pixels (on a finer grid
// the kernel's taps a working pixel apart, read between its pixels).

#include <algorithm>
#include <array>
#include <cmath>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

namespace {

using core::Json;

double number(const Json& params, std::string_view key, double fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() && !v->is_boolean() ? v->get<double>() : fallback;
}

std::string text(const Json& params, std::string_view key, std::string_view fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

// The straight sample of channel c at (x, y) between pixels (centres at whole numbers), past the window's edge its edge.
float between(const Window& src, float x, float y, std::size_t c) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const float ax = x - fx, ay = y - fy;
    const float top = src.at(x0, y0, c) + (src.at(x0 + 1, y0, c) - src.at(x0, y0, c)) * ax;
    if (ay == 0.0f) return top;
    const float bottom = src.at(x0, y0 + 1, c) + (src.at(x0 + 1, y0 + 1, c) - src.at(x0, y0 + 1, c)) * ax;
    return top + (bottom - top) * ay;
}

// --- HSB/HSL (other2.rs) ----------------------------------------------------------------------------------------------

float hue(const std::array<float, 3>& c, float mx, float d) {
    if (d <= 0) return 0;
    float h = 0;
    if (mx == c[0]) {
        h = std::fmod((c[1] - c[2]) / d, 6.0f);
        if (h < 0) h += 6.0f;
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return h / 6.0f;
}

std::array<float, 3> rgb_to_hsb(const std::array<float, 3>& c) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn;
    return {hue(c, mx, d), mx > 0 ? d / mx : 0.0f, mx};
}

std::array<float, 3> rgb_to_hsl(const std::array<float, 3>& c) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn;
    const float l = (mx + mn) / 2.0f;
    const float s = d <= 0 ? 0.0f : d / std::max(1.0f - std::fabs(2.0f * l - 1.0f), 1e-7f);
    return {hue(c, mx, d), std::clamp(s, 0.0f, 1.0f), l};
}

std::array<float, 3> from_hcm(float h, float chroma, float m) {
    float hp = std::fmod(h, 1.0f);
    if (hp < 0) hp += 1.0f;
    hp *= 6.0f;
    float rem = std::fmod(hp, 2.0f);
    if (rem < 0) rem += 2.0f;
    const float x = chroma * (1.0f - std::fabs(rem - 1.0f));
    std::array<float, 3> rgb{};
    switch (static_cast<int>(hp)) {
        case 0: rgb = {chroma, x, 0}; break;
        case 1: rgb = {x, chroma, 0}; break;
        case 2: rgb = {0, chroma, x}; break;
        case 3: rgb = {0, x, chroma}; break;
        case 4: rgb = {x, 0, chroma}; break;
        default: rgb = {chroma, 0, x}; break;
    }
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

std::array<float, 3> hsb_to_rgb(const std::array<float, 3>& c) {
    const float chroma = c[2] * c[1];
    return from_hcm(c[0], chroma, c[2] - chroma);
}

std::array<float, 3> hsl_to_rgb(const std::array<float, 3>& c) {
    const float chroma = (1.0f - std::fabs(2.0f * c[2] - 1.0f)) * c[1];
    return from_hcm(c[0], chroma, c[2] - chroma / 2.0f);
}

Edge edge_of(const std::string& undefined) {
    if (undefined == "repeat") return Edge::Repeat;
    if (undefined == "transparent") return Edge::Transparent;
    return Edge::Wrap;
}

}  // namespace

Kernels other_kernels() {
    Kernels all;
    {  // カスタム (filter.other.custom): Σ kernel · pixel / scale + offset / 255 of each colour (the alpha kept); the 5 × 5
       // grid row by row, its centre the 13th, its taps a working pixel apart
        Kernel k;
        k.kind = "other_custom";
        k.halo = [](const Json&, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(2.0 * ctx.s)) + 2); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            struct Tap {
                float dx, dy, w;
            };
            std::vector<Tap> taps;
            const Json* kernel = core::get(params, "kernel");
            for (std::size_t i = 0; kernel != nullptr && kernel->is_array() && i < kernel->size() && i < 25; ++i) {
                const Json& v = (*kernel)[i];
                const double w = v.is_number() && !v.is_boolean() ? v.get<double>() : 0.0;
                if (w != 0.0)
                    taps.push_back({static_cast<float>((int(i) % 5 - 2) * ctx.s), static_cast<float>((int(i) / 5 - 2) * ctx.s), static_cast<float>(w)});
            }
            std::vector<float> res = src.crop(out);
            if (taps.empty()) return res;
            const double scale = number(params, "scale", 1);
            const float div = static_cast<float>(scale == 0.0 ? 1.0 : scale);
            const float offset = static_cast<float>(number(params, "offset", 0) / 255.0);
            const std::size_t n = src.ch;
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n)
                    for (std::size_t c = 0; c + 1 < n; ++c) {
                        float sum = 0;
                        for (const Tap& t : taps) sum += between(src, static_cast<float>(x) + t.dx, static_cast<float>(y) + t.dy, c) * t.w;
                        res[i + c] = sum / div + offset;
                    }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0 * 25.0 * 2.0; };
        all.push_back(std::move(k));
    }
    {  // ハイパス (filter.other.highPass): the difference from the Gaussian of the radius about a middle grey
        Kernel k;
        k.kind = "other_high_pass";
        k.halo = [](const Json& params, const Ctx& ctx) { return Halo::margin(gaussian_halo(ctx.px(number(params, "radius", 10)))); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return with_blur(src, out, ctx, ctx.px(number(params, "radius", 10)), [](float o, float b) { return 0.5f + (o - b); });
        };
        k.cost = [](const Json& params, const Ctx& ctx) {
            const float s = ctx.px(number(params, "radius", 10));
            return (s <= 4.0f ? 8.0 * (2.0 * std::ceil(3.0 * s) + 1.0) : 80.0) + 10.0;
        };
        all.push_back(std::move(k));
    }
    {  // HSB/HSL (filter.other.hsbHsl): the three colours read as RGB, HSB or HSL (入力モード) and written as one of them (行の
       // 順序), each 0..1
        Kernel k;
        k.kind = "other_hsb_hsl";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx&) {
            std::vector<float> res = src.crop(out);
            const std::string input = text(params, "input_mode", "rgb"), output = text(params, "row_order", "hsb");
            if (input == output || src.ch < 4) return res;
            for (std::size_t i = 0; i < res.size(); i += src.ch) {
                const std::array<float, 3> c{std::clamp(res[i], 0.0f, 1.0f), std::clamp(res[i + 1], 0.0f, 1.0f), std::clamp(res[i + 2], 0.0f, 1.0f)};
                const std::array<float, 3> rgb = input == "hsb" ? hsb_to_rgb(c) : input == "hsl" ? hsl_to_rgb(c) : c;
                const std::array<float, 3> o = output == "hsb" ? rgb_to_hsb(rgb) : output == "hsl" ? rgb_to_hsl(rgb) : rgb;
                std::copy(o.begin(), o.end(), res.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 12.0; };
        all.push_back(std::move(k));
    }
    for (const bool max : {true, false}) {  // 明るさの最大値・最小値 (filter.other.maximum, minimum): 保持 直角度 a square, 真円率 a disc
        Kernel k;
        k.kind = max ? "other_maximum" : "other_minimum";
        k.halo = [](const Json& params, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(ctx.px(number(params, "radius", 1)))) + 1); };
        k.run = [max](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            return extreme(src, out, ctx.px(number(params, "radius", 1)), text(params, "preserve", "squareness") != "roundness", max);
        };
        k.cost = [](const Json& params, const Ctx& ctx) {
            const double r = std::ceil(ctx.px(number(params, "radius", 1)));
            return text(params, "preserve", "squareness") == "roundness" ? 4.0 * 3.0 * (2 * r + 1) + 8 : 4.0 * 6.0 + 8;
        };
        all.push_back(std::move(k));
    }
    {  // スクロール (filter.other.offset): the base box's pixels moved right and down by whole working pixels, what they leave
       // filled as 未定義領域 says (折り返す, 端のピクセルを繰り返して埋める, 透明にする)
        Kernel k;
        k.kind = "other_offset";
        k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const int dx = static_cast<int>(std::lround(ctx.px(number(params, "horizontal", 0))));
            const int dy = static_cast<int>(std::lround(ctx.px(number(params, "vertical", 0))));
            const Edge edge = edge_of(text(params, "undefined_areas", "wrap"));
            std::vector<float> res;
            res.reserve(out.area() * src.ch);
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x)
                    for (std::size_t c = 0; c < src.ch; ++c) res.push_back(src.get_edge(x - dx, y - dy, c, edge, ctx.bounds));
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0; };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
