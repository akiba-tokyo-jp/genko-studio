// PC2c (フェード, SPEC.md COLOR-04): the op fade_layer — a layer's pixels and those it had before the last filter,
// correction or fill mixed by a blend and an opacity (design 3.4, PhotoCraft v0.2.0's engine/edit_menu_cmds.rs:50-79,
// 152-218, MIT: third_party/photocraft/NOTICE.md; render/ops_adjust.hpp). The pixels before are an asset of the book
// (`from`: the window puts them in its folder and names them), so the op is the same wherever it is replayed.

#include <algorithm>
#include <cmath>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/ops_adjust.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

namespace genko::render::adjust_ops {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using image_ops::Rgba;

}  // namespace

void fade_layer(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* id_value = core::get(op, "id");
    if (id_value == nullptr || !id_value->is_string()) throw OpError("id (the layer to fade) is required");
    const std::string id = id_value->get<std::string>();
    const Json* from = core::get(op, "from");
    if (from == nullptr || !from->is_string() || !storage::AssetStore::is_ref(from->get<std::string>()))
        throw OpError("from (the asset of the layer's pixels before: sha256:…) is required");
    double opacity = 100;
    if (const Json* given = core::get(op, "opacity"); given != nullptr && !given->is_null()) {
        if (!given->is_number() || given->is_boolean() || !std::isfinite(given->get<double>())) throw OpError("opacity must be a number (0..100)");
        opacity = std::clamp(given->get<double>(), 0.0, 100.0);
    }
    std::string blend = "normal";
    if (const Json* given = core::get(op, "blend"); given != nullptr && !given->is_null()) {
        if (!given->is_string() || !image_ops::is_blend(given->get<std::string>()))
            throw OpError("blend must be one of normal, multiply, screen, add, overlay, darken, lighten, color_burn, color_dodge, linear_burn, "
                          "soft_light, hard_light, difference, exclusion, subtract, divide, hue, saturation, color, luminosity");
        blend = given->get<std::string>();
    }
    const core::Page& page = doc.page(at);
    const core::Layer* found = nullptr;
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) found = &layer;
    }
    if (found == nullptr) throw OpError("no layer " + id);
    const core::Layer& layer = *found;
    if (layer.locked) throw OpError("the layer is locked");
    const bool eight = layer.raster_png && !layer.raster_png->empty();
    if (!eight && !layer.color_raster && !layer.ink_raster) throw OpError("the layer " + id + " holds no pixels to fade");
    if (layer.stroke_count() > 0 || !layer.patches.empty()) throw OpError("the layer " + id + " holds lines or fills besides its pixels");
    if (!doc.asset_dir) throw OpError("fade_layer reads the pixels before from the book's folder: save the book first");
    const std::string_view suffix = layer.color_raster ? core::kColorRasterSuffix : eight ? std::string_view(".png") : core::kInkRasterSuffix;
    const auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(from->get<std::string>(), suffix, storage::kAssetMaxBytes);
    if (!bytes) throw OpError("the book has no asset " + from->get<std::string>() + " (the pixels before)");
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    // (the pixels before and after, of the same kind and size)
    const auto refused = [&]() -> OpError { return OpError("from is not the pixels of this layer before (another kind or size)"); };
    std::optional<Pixels> before;
    std::optional<Pixels> after;
    try {
        if (layer.color_raster) {
            before.emplace(image_ops::from_precise(*bytes));
            after.emplace(image_ops::from_precise(*layer.color_raster));
        } else if (eight) {
            before.emplace(image_ops::from_image(open_image(*bytes, kPillowOpenLimits)));
            after.emplace(image_ops::from_image(open_image(*layer.raster_png, kPillowOpenLimits)));
        } else {
            if (!page.color) throw refused();
            const core::InkPixels was = core::decode_ink_raster(*bytes);
            const core::InkPixels now = core::decode_ink_raster(*layer.ink_raster);
            if (was.space != now.space || was.depth != now.depth || was.colours() != now.colours()) throw refused();
            before.emplace(image_ops::from_inks(was, *page.color));
            after.emplace(image_ops::from_inks(now, *page.color));
        }
    } catch (const OpError&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "memory") throw;
        throw refused();
    }
    if (before->width() != after->width() || before->height() != after->height() || before->colours() != after->colours()) throw refused();
    const auto k = static_cast<float>(opacity / 100.0);
    c.report = Json{{"faded", id}, {"opacity", opacity}, {"blend", blend}};
    if (blend == "normal" && k >= 1.0f) return;  // (all of the result: the layer as it is)
    if (blend == "normal" && k <= 0.0f) {  // (none of it: the pixels before, their very bytes)
        const auto index = static_cast<std::size_t>(found - page.layers.data());
        const bool precise = layer.color_raster != nullptr;
        core::Layer& into = doc.edit_page(at).layers[index];
        const auto kept = std::make_shared<const std::string>(*bytes);
        if (precise) {
            into.color_raster = kept;
        } else if (eight) {
            into.raster_png = kept;
            into.raster_memo = {};
        } else {
            into.ink_raster = kept;
            into.ink_memo = {};
        }
        return;
    }
    Pixels& out = *after;
    const std::size_t n = out.all();
    std::vector<float> mixed(n);
    if (blend == "normal") {  // (the values themselves, premultiplied: the alpha fades too)
        for (std::size_t i = 0; i < out.size(); ++i) {
            image_ops::fade_values(before->at(i), out.at(i), mixed.data(), n, true, k);
            std::copy(mixed.begin(), mixed.end(), out.at(i));
        }
    } else {  // (the colours: an ink layer's as its inks mean them, back by the page's rules)
        const std::vector<Rgba> b = image_ops::rgba_of(*before);
        const std::vector<Rgba> a = image_ops::rgba_of(out);
        std::vector<Rgba> faded(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) faded[i] = image_ops::fade_rgba(b[i], a[i], blend, k);
        if (out.kind() == Pixels::Kind::Inks) {
            std::vector<double> rgb(out.size() * 3);
            for (std::size_t i = 0; i < out.size(); ++i)
                for (unsigned ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] = faded[i][ch];
            image_ops::from_meant(out, rgb);
            for (std::size_t i = 0; i < out.size(); ++i) out.at(i)[out.colours()] = faded[i][3];
        } else {
            for (std::size_t i = 0; i < out.size(); ++i) std::copy(faded[i].begin(), faded[i].end(), out.at(i));
        }
    }
    image_ops::put_layer_pixels(doc, at, id, out);
}

}  // namespace genko::render::adjust_ops
