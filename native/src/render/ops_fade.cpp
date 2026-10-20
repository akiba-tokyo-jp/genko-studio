// PC2c (フェード, SPEC.md COLOR-04): the op fade_layer — a layer's pixels and those it had before the last filter,
// correction or fill mixed by a blend and an opacity (design 3.4, PhotoCraft v0.2.0's engine/edit_menu_cmds.rs:50-79,
// 152-218, MIT: third_party/photocraft/NOTICE.md; render/ops_adjust.hpp). The pixels before are an asset of the book
// (`from`: the window puts them in its folder and names them; a precise layer's as the book saves them, in tiles), so
// the op is the same wherever it is replayed.

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "core/color_model.hpp"
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

// A precise layer's pixels before as the book saves them — in tiles, its color_raster as project.json holds it
// (storage/reader's way) —, in the page's space; `refused()` when they are not the tiles of such a raster (another size,
// space or precision), OpError when one of them is not in the book. Each tile is read alone, so a layer of any size
// fades (independent review B (E)).
std::string joined_tiles(const storage::AssetStore& store, const Json& m, const core::Page& page, const std::function<OpError()>& refused) {
    try {
        if (!m.contains("tiles") || !m["tiles"].is_array()) throw refused();
        for (const Json& tile : m["tiles"])
            if (!tile.is_string() || !storage::AssetStore::is_ref(tile.get<std::string>())) throw refused();
        if (core::color_raster_space_of(m) != core::color_raster_space(page.rgb)) throw refused();
        const auto dimension = [&](const char* key) {
            if (!m.contains(key) || !m[key].is_number_integer() || m[key].get<std::int64_t>() < 1) throw refused();
            return static_cast<std::uint64_t>(m[key].get<std::int64_t>());
        };
        const std::uint64_t w = dimension("width"), h = dimension("height");
        if (w > core::kColorRasterMaxPixels || h > core::kColorRasterMaxPixels / w || !m.contains("precision") || !m["precision"].is_string())
            throw refused();
        const std::uint64_t across = (w + core::kColorTile - 1) / core::kColorTile, down = (h + core::kColorTile - 1) / core::kColorTile;
        if (m["tiles"].size() != across * down) throw refused();
        std::string bytes = core::join_color_tiles(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), m["precision"].get<std::string>(),
                                                   [&](std::size_t i) {
                                                       const std::string ref = m["tiles"][i].get<std::string>();
                                                       auto tile = store.get_bytes(ref, core::kColorRasterSuffix,
                                                                                   16 + std::size_t(core::kColorTile) * core::kColorTile * 16);
                                                       if (!tile) throw OpError("the book has no asset " + ref + " (a tile of the pixels before)");
                                                       if (storage::AssetStore::ref(*tile) != ref) throw refused();
                                                       return std::move(*tile);
                                                   });
        core::check_tiled_metadata(core::ColorRasterView(bytes), m);
        return bytes;
    } catch (const OpError&) {
        throw;
    } catch (const core::Error&) {
        throw refused();
    }
}

}  // namespace

void fade_layer(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* id_value = core::get(op, "id");
    if (id_value == nullptr || !id_value->is_string()) throw OpError("id (the layer to fade) is required");
    const std::string id = id_value->get<std::string>();
    const Json* from = core::get(op, "from");
    const bool tiled = from != nullptr && from->is_object();
    if (from == nullptr || !(tiled || (from->is_string() && storage::AssetStore::is_ref(from->get<std::string>()))))
        throw OpError("from (the layer's pixels before: their asset sha256:…, or a precise layer's tiles as project.json holds them) is required");
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
    if (layer.stroke_count() > 0 || !layer.patches.empty() || (layer.shapes && !layer.shapes->items.empty()))
        throw OpError("the layer " + id + " holds lines or fills besides its pixels");
    if (!doc.asset_dir) throw OpError("fade_layer reads the pixels before from the book's folder: save the book first");
    // (the pixels before and after, of the same kind and size)
    const auto refused = [&]() -> OpError { return OpError("from is not the pixels of this layer before (another kind or size)"); };
    const storage::AssetStore store(*doc.asset_dir);
    std::optional<std::string> bytes;
    if (tiled) {
        if (!layer.color_raster) throw OpError("from as tiles is a precise layer's (its color_raster as project.json holds it): give the layer's asset");
        bytes = joined_tiles(store, *from, page, refused);
    } else {
        const std::string_view suffix = layer.color_raster ? core::kColorRasterSuffix : eight ? std::string_view(".png") : core::kInkRasterSuffix;
        try {
            bytes = store.get_bytes(from->get<std::string>(), suffix, storage::kAssetMaxBytes);
        } catch (const core::Error& error) {  // (past what the store reads of one asset: said so, nothing of it read)
            if (error.code() != "memory") throw;
            throw OpError(std::string("the pixels before are too large to read whole (at most 256 MiB") +
                          (layer.color_raster ? "; give a precise layer's tiles as project.json holds them)" : ")"));
        }
        if (!bytes) throw OpError("the book has no asset " + from->get<std::string>() + " (the pixels before)");
    }
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
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
    } else {  // (the colours: an ink layer's as its inks mean them, back by the page's rules; their RGBA in the budget)
        const image_ops::Held held(std::uint64_t(out.size()) * sizeof(Rgba) * 3);
        const std::vector<Rgba> b = image_ops::rgba_of(*before);
        const std::vector<Rgba> a = image_ops::rgba_of(out);
        std::vector<Rgba> faded(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) faded[i] = image_ops::fade_rgba(b[i], a[i], blend, k);
        if (out.kind() == Pixels::Kind::Inks) {
            image_ops::from_rgba(out, faded);
        } else {
            for (std::size_t i = 0; i < out.size(); ++i) std::copy(faded[i].begin(), faded[i].end(), out.at(i));
        }
    }
    image_ops::put_layer_pixels(doc, at, id, out);
}

}  // namespace genko::render::adjust_ops
