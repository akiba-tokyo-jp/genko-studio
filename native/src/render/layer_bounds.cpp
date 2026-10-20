#include "render/layer_bounds.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/image.hpp"
#include "render/page.hpp"

// PhotoCraft v0.2.0 crates/engine/src/layer_multi_cmds.rs (layer_bounds, 254-262) and pick_cmds.rs (layers_at,
// top_group), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::layer_bounds {

using core::Layer;
using core::LayerKind;
using core::Page;

namespace {

const Layer* find(const Page& page, const std::string& id) {
    for (const Layer& layer : page.layers)
        if (layer.id == id) return &layer;
    return nullptr;
}

bool in_folder(const Layer& layer, const std::string& folder) { return layer.parent_id.is_string() && layer.parent_id.get<std::string>() == folder; }

std::atomic<std::uint64_t> g_drawn{0};

}  // namespace

std::uint64_t pixels_drawn() { return g_drawn.load(); }

std::vector<std::string> inside(const Page& page, const std::string& folder) {
    std::set<std::string> held{folder};
    for (bool more = true; more;) {  // (until no layer joins: folders in folders)
        more = false;
        for (const Layer& layer : page.layers) {
            if (held.contains(layer.id) || !layer.parent_id.is_string() || !held.contains(layer.parent_id.get<std::string>())) continue;
            held.insert(layer.id);
            more = true;
        }
    }
    std::vector<std::string> out;
    for (const Layer& layer : page.layers)
        if (layer.id != folder && held.contains(layer.id)) out.push_back(layer.id);
    return out;
}

std::optional<Box> of_layer(const core::Document& doc, const Page& page, const std::string& id) {
    const Layer* layer = find(page, id);
    if (layer == nullptr) throw core::OpKeyError(core::py_repr_str(id));
    if (layer->kind == LayerKind::Fill || layer->kind == LayerKind::Adjust) return std::nullopt;
    if (layer->kind == LayerKind::Folder) {
        std::optional<Box> all;
        for (const Layer& child : page.layers) {
            if (!in_folder(child, id)) continue;
            const auto b = of_layer(doc, page, child.id);
            if (!b) continue;
            all = all ? Box{std::min((*all)[0], (*b)[0]), std::min((*all)[1], (*b)[1]), std::max((*all)[2], (*b)[2]), std::max((*all)[3], (*b)[3])} : *b;
        }
        return all;
    }
    Layer shown = *layer;  // (what it holds: shown, whole, without its masks)
    shown.visible = true;
    shown.opacity = 1.0;
    shown.mask.reset();
    shown.vector_mask.reset();
    const Image picture = layer_image(page, shown, kDpi, &doc, true);
    const auto b = picture.getbbox();
    if (!b) return std::nullopt;
    const auto g = grid(page);
    return Box{b->x0 / g[0], b->y0 / g[1], b->x1 / g[0], b->y1 / g[1]};
}

std::array<double, 2> grid(const Page& page) {
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    return {w > 0 ? mm_to_px(w, kDpi) / w : kPx, h > 0 ? mm_to_px(h, kDpi) / h : kPx};
}

Box of_line(const core::StoryLine& line) {
    const double x = line.x_mm.value(), y = line.y_mm.value();
    return Box{x, y, x + line.w_mm.value(), y + line.h_mm.value()};
}

bool shows_at(const core::Document& doc, const Page& page, const Layer& layer, double x_mm, double y_mm) {
    if (layer.kind == LayerKind::Folder || layer.kind == LayerKind::Adjust) return false;
    const auto g = grid(page);
    const int x = static_cast<int>(std::floor(x_mm * g[0])), y = static_cast<int>(std::floor(y_mm * g[1]));
    if (x < 0 || y < 0) return false;
    Layer shown = layer;
    shown.opacity = 1.0;
    // (the layer drawn round the point only — the same pixel as the whole layer drawn there: independent review A P2-3)
    const render::Box around{std::max(0, x - 1), std::max(0, y - 1), x + 2, y + 2};
    const Image picture = layer_image_over(page, shown, kDpi, around, &doc, true);
    g_drawn += std::uint64_t(picture.width()) * std::uint64_t(picture.height());
    const int ix = x - around.x0, iy = y - around.y0;
    if (ix >= picture.width() || iy >= picture.height()) return false;
    const std::vector<double> px = picture.getpixel(ix, iy);
    const std::string_view mode = picture.mode();
    if (mode == "RGBA" || mode == "LA" || mode == "La" || mode == "RGBa") return !px.empty() && px.back() > 0;
    return true;  // (a picture without alpha covers the page)
}

std::vector<std::string> layers_at(const core::Document& doc, const Page& page, double x_mm, double y_mm) {
    const auto shown = [&page](const Layer& layer) {  // (the layer and every folder it is in shown)
        std::set<std::string> seen;
        for (const Layer* up = &layer; up != nullptr && seen.insert(up->id).second;) {
            if (!up->visible) return false;
            up = up->parent_id.is_string() ? find(page, up->parent_id.get<std::string>()) : nullptr;
        }
        return true;
    };
    std::vector<std::string> out;
    for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {
        if (!shown(*it)) continue;
        if (shows_at(doc, page, *it, x_mm, y_mm)) out.push_back(it->id);
    }
    return out;
}

std::string outermost(const Page& page, const std::string& id) {
    std::string out = id;
    std::set<std::string> seen;
    for (const Layer* up = find(page, id); up != nullptr && seen.insert(up->id).second;) {
        out = up->id;
        up = up->parent_id.is_string() ? find(page, up->parent_id.get<std::string>()) : nullptr;
    }
    return out;
}

}  // namespace genko::render::layer_bounds
