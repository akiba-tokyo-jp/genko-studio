// PC4 節 D (SPEC.md COLOR-08「フィルター」): フィルター's kinds as filter_raster's local filters — each kind of the table
// (core/filter_kinds.hpp) whose kernel is in this build (render/fx.hpp) on all three ways: an 8-bit layer (its RGBA; the
// op keeps the result inside the selection, filters.within), a precise layer (its new GKCR bytes) and an ink layer (its
// new GKIR bytes), the selection mixing the result by its coverage — and the frames of PC4's new ops (render_picture:
// D4, vanishing_point: D6), refused until their groups write them (not_yet_ported naming them).

#include "render/ops_filters.hpp"

#include <algorithm>

#include "core/adjust.hpp"
#include "core/command_bus.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/layer_masks.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace {

using core::Json;

constexpr std::uint64_t kBudget = 2ULL * 1024 * 1024 * 1024;  // (the imaging budget of a local filter, as PC2c's)

thread_local const core::Layer* g_layer = nullptr;  // (FilterLayer)

// The plane a kind reads beside the layer over a grid of w × h (ぼかし（レンズ）'s "depth_map": "layer_mask": the layer's
// mask as it shows over the page — its density and feather laid on —, each 0..1); none when it reads none or the layer
// filtered has no mask (then no depth: as "none").
std::optional<std::vector<float>> plane_of(const core::Page& page, const Json& params, std::uint32_t w, std::uint32_t h) {
    const Json* depth = core::get(params, "depth_map");
    if (depth == nullptr || *depth != Json("layer_mask") || g_layer == nullptr || !g_layer->mask || !g_layer->mask->png || g_layer->mask->png->empty())
        return std::nullopt;
    const image_ops::Held held(std::uint64_t(w) * h * (sizeof(float) + 1));
    const Image decoded = read_png(*g_layer->mask->png).convert("L");
    const Box whole{0, 0, static_cast<int>(w), static_cast<int>(h)};
    const std::string bytes =
        layer_masks::shown(page, *g_layer->mask, decoded, Size{static_cast<int>(w), static_cast<int>(h)}, whole, Resample::Bilinear).tobytes();
    std::vector<float> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

// The coverage of an area (a resolved {poly} or {mask}) on a grid of w × h over the page, a byte a pixel: an 8-bit layer's
// as filter_raster draws its area (the working 200 dpi), any other's by render/ink_edit.
std::string cover_of(const core::Page& page, const Json& area, std::uint32_t w, std::uint32_t h, bool eight) {
    const image_ops::Held held(std::uint64_t(w) * h);
    if (!eight) return ink_edit::area_coverage(page, w, h, area);
    const selection::AreaMask inside = selection::area_mask(area, raster::kWorkingDpi);
    Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
    mask.paste(inside.mask, Point{static_cast<int>(inside.x0), static_cast<int>(inside.y0)});
    return mask.tobytes();
}

// The box a coverage covers at all (the base box of a filter run inside a selection: upstream's selection bounds).
fx::Rect covered(const std::string& cover, std::uint32_t w, std::uint32_t h) {
    int x0 = int(w), y0 = int(h), x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            if (cover[std::size_t(y) * w + x] == 0) continue;
            x0 = std::min(x0, int(x));
            x1 = std::max(x1, int(x) + 1);
            y0 = std::min(y0, int(y));
            y1 = std::max(y1, int(y) + 1);
        }
    return fx::Rect{x0, y0, x1, y1}.intersect(fx::Rect{0, 0, int(w), int(h)});
}

// The kernel on a surface: the base box the selection's box (else the grid), the coverage mixing it (`mix`: the caller
// keeps it to the area itself — an 8-bit layer — and only the area's box is worked).
void run(fx::Surface& surface, const fx::Kernel& kernel, core::FilterInks inks, const Json& params, const core::Page& page, const Json* area,
         bool eight, bool mix) {
    const std::uint32_t w = surface.width(), h = surface.height();
    const fx::Rect grid{0, 0, int(w), int(h)};
    const double s = image_ops::grid_scale(page, w, h);
    const std::optional<std::vector<float>> plane = plane_of(page, params, w, h);
    fx::Reads reads;
    reads.plane = plane ? &*plane : nullptr;
    if (area == nullptr) {
        fx::apply(surface, kernel, params, inks, grid, nullptr, s, nullptr, {}, reads);
        return;
    }
    const std::string cover = cover_of(page, *area, w, h, eight);
    const fx::Rect box = covered(cover, w, h);
    if (box.empty()) return;  // (nothing inside: as it was)
    if (mix) {
        fx::apply(surface, kernel, params, inks, box, &cover, s, nullptr, {}, reads);
    } else {  // (the caller keeps the result to the area; a kind that reads the coverage itself has it: 平均)
        reads.within = &cover;
        fx::apply(surface, kernel, params, inks, box, nullptr, s, &box, {}, reads);
    }
}

adjust_ops::LocalFilter local_of(const fx::Kernel& kernel, core::FilterInks inks) {
    adjust_ops::LocalFilter out;
    out.eight = [&kernel, inks](const Image& rgba, const Json& params, const core::Page& page, const core::Document&) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::eight_surface(rgba);
        run(*surface, kernel, inks, params, page, nullptr, true, false);
        return fx::take_image(*surface);
    };
    out.eight_in = [&kernel, inks](const Image& rgba, const Json& params, const core::Page& page, const core::Document&, const Json& area) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::eight_surface(rgba);
        run(*surface, kernel, inks, params, page, &area, true, false);  // (the caller keeps it to the area)
        return fx::take_image(*surface);
    };
    out.precise = [&kernel, inks](const core::Page& page, const core::Layer& layer, const Json& params, const Json* area, const core::Document&) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::precise_surface(*layer.color_raster);
        run(*surface, kernel, inks, params, page, area, false, true);
        return fx::take_bytes(*surface);
    };
    out.inks = [&kernel, inks](const core::Page& page, const core::InkPixels& raster, const Json& params, const Json* area, const core::Document&) {
        if (!page.color) throw core::OpError("an RGB page holds no ink pixels");
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::ink_surface(raster, *page.color);
        run(*surface, kernel, inks, params, page, area, false, true);
        return fx::take_bytes(*surface);
    };
    return out;
}

// PC4's new ops, refused until their groups write them.
[[noreturn]] void op_not_yet(std::string_view op, std::string_view group) {
    core::not_yet_ported(std::string(op) + " is not in the C++ build yet (PC4 " + std::string(group) + ")");
}

}  // namespace

FilterLayer::FilterLayer(const core::Layer& layer) : before_(g_layer) { g_layer = &layer; }
FilterLayer::~FilterLayer() { g_layer = before_; }

namespace adjust_ops {

LocalFilters filter_local_filters() {
    LocalFilters out;
    for (const core::FilterInfo& info : core::filter_infos()) {
        const fx::Kernel* kernel = fx::kernel(info.kind);
        if (kernel == nullptr) continue;  // (a kind its group has not drawn yet: not_yet_ported naming it)
        out.emplace_back(info.kind, local_of(*kernel, info.inks));
    }
    return out;
}

}  // namespace adjust_ops

std::optional<Json> filter_report(const core::Document& doc, const Json& op) {
    const Json* kind = core::get(op, "kind");
    if (kind == nullptr || !kind->is_string() || !core::pc4_filter(kind->get_ref<const std::string&>())) return std::nullopt;
    Json params = Json::object();
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "page" && key != "layer" && key != "id" && key != "kind" && key != "area" && key != "channel") params[key] = value;
    }
    const std::size_t at = core::require_page(doc, op);
    Json drawn{{"kind", *kind}};
    drawn.update(core::normalize_filter(kind->get_ref<const std::string&>(), params, &doc.page(at)));
    return Json{{"filter", std::move(drawn)}};
}

void register_filter_ops(core::OpRegistry& registry) {
    // フィルター → 描画 → 炎…・額縁…・木… (D4: render/render_picture.cpp) and 消点… (D6: render/vanishing.cpp)
    registry.add("render_picture", [](core::OpContext&) { op_not_yet("render_picture", "D4"); });
    registry.add("vanishing_point", [](core::OpContext&) { op_not_yet("vanishing_point", "D6"); });
}

}  // namespace genko::render
