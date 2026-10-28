// PC4 節 D1 (SPEC.md COLOR-08): フィルター → ビデオ's kernels — インターレース解除 and NTSC カラー. PhotoCraft v0.2.0's
// algo/src/video.rs (deinterlace, ntsc), MIT: native/third_party/photocraft/NOTICE.md. インターレース解除's lines are working
// pixels (on a finer grid, each as many of its rows: the same look), counted from the base box's top.

#include <algorithm>
#include <cmath>

#include "core/pyops.hpp"
#include "render/fx.hpp"

namespace genko::render::fx {

namespace {

using core::Json;

std::string text(const Json& params, std::string_view key, std::string_view fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

// Composite-signal limits (fractions of the 7.5–100 IRE luma range): broadcast-safe is −20…120 IRE (video.rs).
constexpr float kNtscMax = (120.0f - 7.5f) / 92.5f;
constexpr float kNtscMin = (-20.0f - 7.5f) / 92.5f;

}  // namespace

Kernels video_kernels() {
    Kernels all;
    {  // インターレース解除 (filter.video.deInterlace): the odd lines (the 1st, 3rd… from the base box's top) or the even ones
       // taken out and made again from the lines kept about them — between them (補間) or the one above (複製)
        Kernel k;
        k.kind = "video_de_interlace";
        k.halo = [](const Json&, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(std::max(1.0, ctx.s))) + 1); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const Rect& b = ctx.bounds;
            const double line = std::max(1.0, ctx.s);
            const bool even = text(params, "eliminate", "odd_fields") == "even_fields";
            const bool interpolate = text(params, "create_by", "interpolation") != "duplication";
            const auto line_of = [&](int y) { return static_cast<long>(std::floor((y - b.y0) / line)); };
            const auto first_row = [&](long l) { return b.y0 + static_cast<int>(std::ceil(static_cast<double>(l) * line - 1e-9)); };
            std::vector<float> res = src.crop(out);
            std::vector<float> acc(n);
            for (int y = out.y0; y < out.y1; ++y) {
                if (y < b.y0 || y >= b.y1) continue;
                const long l = line_of(y);
                if ((l % 2 == 0) == even) continue;  // (line 1, 3… — offsets 0, 2… — are the odd ones: kept when the even go)
                const int above = first_row(l) - 1, below = first_row(l + 1);
                const bool has_above = above >= b.y0, has_below = below < b.y1;
                if (!has_above && !has_below) continue;
                for (int x = out.x0; x < out.x1; ++x) {
                    if (!b.contains(x, y)) continue;
                    float* px = res.data() + (std::size_t(y - out.y0) * std::size_t(out.width()) + std::size_t(x - out.x0)) * n;
                    std::fill(acc.begin(), acc.end(), 0.0f);
                    const auto take = [&](int row, float w) {
                        const float a = src.at(x, row, n - 1);
                        for (std::size_t c = 0; c + 1 < n; ++c) acc[c] += src.at(x, row, c) * a * w;
                        acc[n - 1] += a * w;
                    };
                    if (interpolate && has_above && has_below) {
                        const float t = static_cast<float>(y - above) / static_cast<float>(below - above);
                        take(above, 1.0f - t);
                        take(below, t);
                    } else {
                        take(has_above ? above : below, 1.0f);
                    }
                    const float a = acc[n - 1];
                    for (std::size_t c = 0; c + 1 < n; ++c) px[c] = a > 1e-7f ? acc[c] / a : 0.0f;
                    px[n - 1] = a;
                }
            }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 6.0; };
        all.push_back(std::move(k));
    }
    {  // NTSC カラー (filter.video.ntscColors): the chroma (YIQ) scaled so the composite signal Y ± C stays within broadcast
       // limits; the hue and the luma kept
        Kernel k;
        k.kind = "video_ntsc_colors";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json&, const Window& src, const Rect& out, const Ctx&) {
            std::vector<float> res = src.crop(out);
            if (src.ch < 4) return res;
            for (std::size_t i = 0; i < res.size(); i += src.ch) {
                const float r = res[i], g = res[i + 1], b = res[i + 2];
                const float yy = 0.299f * r + 0.587f * g + 0.114f * b;
                const float ii = 0.595716f * r - 0.274453f * g - 0.321263f * b;
                const float qq = 0.211456f * r - 0.522591f * g + 0.311135f * b;
                const float chroma = std::sqrt(ii * ii + qq * qq);
                if (chroma <= 1e-7f) continue;
                float scale = 1.0f;
                if (yy + chroma > kNtscMax) scale = std::min(scale, std::max(0.0f, (kNtscMax - yy) / chroma));
                if (yy - chroma < kNtscMin) scale = std::min(scale, std::max(0.0f, (yy - kNtscMin) / chroma));
                if (scale >= 1.0f) continue;
                const float i2 = ii * scale, q2 = qq * scale;
                res[i] = yy + 0.9563f * i2 + 0.6210f * q2;
                res[i + 1] = yy - 0.2721f * i2 - 0.6474f * q2;
                res[i + 2] = yy - 1.1070f * i2 + 1.7046f * q2;
            }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 12.0; };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
