// PC4 節 D1 (SPEC.md COLOR-08): フィルター → シャープ's kernels — アンシャープマスク (and シャープ・シャープ（強）・シャープ（輪郭
// のみ）, the unsharp mask at fixed settings) and スマートシャープ. PhotoCraft v0.2.0's algo/src/sharpen.rs (with_blur,
// unsharp, smart), MIT: native/third_party/photocraft/NOTICE.md; the blur is the engine's Gaussian (render/fx: the exact
// kernel to a radius of 4, three exact box passes beyond), its radius working pixels.

#include <algorithm>
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

}  // namespace

// Each colour of each pixel that shows as `f(its value, the blurred one)` (sharpen.rs with_blur; the alpha kept).
std::vector<float> with_blur(const Window& src, const Rect& out, const Ctx& ctx, float sigma, const std::function<float(float, float)>& f) {
    const std::vector<float> blurred = gaussian(src, out, true, sigma, ctx.largest);
    std::vector<float> res = src.crop(out);
    const std::size_t n = src.ch;
    for (std::size_t i = 0; i < res.size(); i += n) {
        if (!(res[i + n - 1] > 0.0f)) continue;
        for (std::size_t c = 0; c + 1 < n; ++c) res[i + c] = f(res[i + c], blurred[i + c]);
    }
    return res;
}

Kernels sharpen_kernels() {
    Kernels all;
    const auto sigma = [](const Json& params, const Ctx& ctx) { return ctx.px(number(params, "radius", 1)); };
    const auto halo = [sigma](const Json& params, const Ctx& ctx) { return Halo::margin(gaussian_halo(sigma(params, ctx))); };
    const auto cost = [sigma](const Json& params, const Ctx& ctx) {
        const float s = sigma(params, ctx);
        return (s <= 4.0f ? 8.0 * (2.0 * std::ceil(3.0 * s) + 1.0) : 80.0) + 10.0;
    };
    {  // アンシャープマスク (filter.sharpen.unsharpMask): the difference from the blur, `amount` % of it added where it is at
       // least the threshold (levels)
        Kernel k;
        k.kind = "sharpen_unsharp_mask";
        k.halo = halo;
        k.run = [sigma](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const float amount = static_cast<float>(number(params, "amount", 50) / 100.0);
            const float t = static_cast<float>(number(params, "threshold", 0) / 255.0);
            return with_blur(src, out, ctx, sigma(params, ctx), [amount, t](float o, float b) {
                const float d = o - b;
                return std::fabs(d) + 1e-6f >= t ? o + amount * d : o;
            });
        };
        k.cost = cost;
        all.push_back(std::move(k));
    }
    {  // スマートシャープ (filter.sharpen.smartSharpen): the unsharp mask with the small (noise's) differences held back by
       // ノイズを軽減 %
        Kernel k;
        k.kind = "sharpen_smart";
        k.halo = halo;
        k.run = [sigma](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const float amount = static_cast<float>(number(params, "amount", 100) / 100.0);
            const float nr = static_cast<float>(std::clamp(number(params, "reduce_noise", 10) / 100.0, 0.0, 1.0));
            return with_blur(src, out, ctx, sigma(params, ctx), [amount, nr](float o, float b) {
                const float d = o - b;
                const float keep = 1.0f - nr * std::exp(-std::fabs(d) * 40.0f);
                return o + amount * d * keep;
            });
        };
        k.cost = cost;
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
