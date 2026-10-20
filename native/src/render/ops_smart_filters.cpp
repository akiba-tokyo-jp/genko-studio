// smart-objects@1, PC3 A2 (docs/cpp-migration/SPEC.md COLOR-05「スマートフィルター」): the ops of a smart object's smart
// filters (this build's own: `genko schema` and the MCP tools' list stay as they are) — smart_filter_add (a filter_raster
// kind kept on the smart object; the selection its filter mask), smart_filter_set (its settings again, its blend and
// opacity, shown or hidden), smart_filter_move, smart_filter_delete (one, or every one and the mask) and
// smart_filter_state (all of them off and on, the filter mask off and on, deleted, made again of a selection).
// PhotoCraft v0.2.0's engine/src/smart_cmds.rs:367-401 (the selection becomes the filter mask when there is none) and
// 692-779, 880-953 (what each command changes and refuses), MIT (third_party/photocraft/NOTICE.md). Each changes the
// smart object's settings only: the filters are laid on its picture when it is drawn (render/smart_filters.cpp).

#include <algorithm>
#include <cmath>

#include "core/areas.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/ink_canvas.hpp"
#include "render/smart_filters.hpp"

namespace genko::render::smart_filters {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpContext;
using core::OpError;
using core::Page;

// The smart object an op names by "id" on its page, not locked.
std::size_t smart_at(const Page& page, const Json& op) {
    const std::size_t at = core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string());
    const Layer& layer = page.layers[at];
    if (layer.kind != LayerKind::Smart || !layer.smart || !layer.smart->is_object()) throw OpError("layer " + layer.id + " is not a smart object");
    if (layer.locked) throw OpError("the layer is locked");
    return at;
}

Json filters_of(const Layer& layer) {
    const Json* filters = core::get(*layer.smart, "filters");
    return filters != nullptr && filters->is_array() ? *filters : Json::array();
}

bool has_mask(const Layer& layer) {
    const Json* mask = core::get(*layer.smart, "filter_mask");
    return mask != nullptr && mask->is_object();
}

// The filters back on the smart object, and its settings tidy: no list when there is none; nothing said of them being on
// when there are no filters and no mask. A PSD's picture shown for it no longer is what it draws (PC3 A3's).
void put_filters(Layer& layer, Json filters) {
    Json& s = *layer.smart;
    if (filters.empty()) s.erase("filters");
    else s["filters"] = std::move(filters);
    if (!s.contains("filters") && !s.contains("filter_mask")) s.erase("filters_on");
    if (Json* psd = s.contains("psd") ? &s["psd"] : nullptr; psd != nullptr && psd->is_object()) psd->erase("shown");
}

// An index into the filters ("index" etc.): a whole number 0..n−1 (0 the bottom one), else `fallback`.
std::size_t index_at(const Json& op, const char* key, std::size_t n, std::optional<std::size_t> fallback) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) {
        if (!fallback) throw OpError(std::string(key) + " is the filter's place (0 to " + std::to_string(n - 1) + ", 0 the bottom one)");
        return *fallback;
    }
    if (!v->is_number_integer() || v->get<std::int64_t>() < 0 || v->get<std::int64_t>() >= static_cast<std::int64_t>(n))
        throw OpError(std::string(key) + " is the filter's place (0 to " + std::to_string(n - 1) + ", 0 the bottom one)");
    return static_cast<std::size_t>(v->get<std::int64_t>());
}

// A blend mode a smart filter is laid in on this page (the 20 of Genko's; a Lab or multichannel page: those it composites).
std::string blend_on(const Page& page, const Json& value) {
    if (!value.is_string()) throw OpError("blend is a blend mode");
    const std::string mode = core::blend_mode(value);
    if (page.color && !InkCanvas(*page.color, Size{1, 1}).supports_blend(mode)) throw OpError("this page does not composite the blend mode " + mode);
    return mode;
}

// Settings laid over a filter's (smart_filter_set's params; PhotoCraft's setParams merges them): an object key by key —
// one inside it too (a native's) —, null taking the key out, anything else in its place.
void lay_over(Json& now, const Json& changes) {
    for (const auto& [key, value] : changes.items()) {
        if (value.is_null()) now.erase(key);
        else if (value.is_object() && now.contains(key) && now[key].is_object()) lay_over(now[key], value);
        else now[key] = value;
    }
}

// The filter's settings in an op: what is not the op's own.
Json params_in(const Json& op) {
    Json out = Json::object();
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "page" && key != "id" && key != "layer" && key != "kind" && key != "area" && key != "contents") out[key] = value;
    }
    return out;
}

// The op's area as a filter mask: its PNG kept with the book's files (the writer puts it in assets/).
Json mask_from(Document& doc, const Page& page, const Json& op) {
    const std::string png = mask_of_area(page, core::op_area(op));
    const std::string ref = core::bytes_ref(png);
    doc.smart_files[ref] = std::make_shared<const std::string>(png);
    return Json{{"asset", ref}, {"enabled", true}};
}

void add(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_at(doc.page(at), c.op);
    const Json* kind = core::get(c.op, "kind");
    if (kind == nullptr || !kind->is_string() || kind->get<std::string>().empty()) throw OpError("kind names the filter");
    std::optional<Json> area;
    if (core::truthy_at(c.op, "area") && !has_mask(doc.page(at).layers[i])) area = core::op_area(c.op);
    Page& page = doc.edit_page(at);
    // (on top, the selection its mask when it has none: push_filter, which the ops putting their own on a smart object share)
    const std::size_t index = push_filter(doc, page, page.layers[i], kind->get<std::string>(), params_in(c.op), area);
    c.report = Json{{"index", index}};
}

void set(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_at(doc.page(at), c.op);
    Json filters = filters_of(doc.page(at).layers[i]);
    if (filters.empty()) throw OpError("the smart object has no smart filters");
    const std::size_t index = index_at(c.op, "index", filters.size(), filters.size() - 1);
    Json f = filters[index];
    bool changed = false;
    if (const Json* params = core::get(c.op, "params"); params != nullptr && !params->is_null()) {
        if (!params->is_object()) throw OpError("params is an object of the filter's settings (laid over its settings now)");
        Json merged = f.contains("params") && f["params"].is_object() ? f["params"] : Json::object();
        lay_over(merged, *params);
        f["params"] = settled(doc, doc.page(at), core::py_str(f["kind"]), merged);
        changed = true;
    }
    if (const Json* blend = core::get(c.op, "blend"); blend != nullptr && !blend->is_null()) {
        f["blend"] = blend_on(doc.page(at), *blend);
        changed = true;
    }
    if (const Json* opacity = core::get(c.op, "opacity"); opacity != nullptr && !opacity->is_null()) {
        if (!opacity->is_number() || opacity->is_boolean() || !std::isfinite(opacity->get<double>())) throw OpError("opacity is 0 to 1 (or 0 to 100 %)");
        const double v = opacity->get<double>();
        f["opacity"] = std::clamp(v > 1 ? v / 100 : v, 0.0, 1.0);  // (PhotoCraft's: past 1 a percentage)
        changed = true;
    }
    const Json* visible = core::get(c.op, "visible");
    if (visible != nullptr && !visible->is_null()) {
        if (!visible->is_boolean()) throw OpError("visible is true or false");
        f["visible"] = *visible;
    } else if (!changed) {  // (nothing else asked: shown or hidden the other way, as PhotoCraft's setVisible)
        f["visible"] = f.contains("visible") && f["visible"].is_boolean() && !f["visible"].get<bool>();  // (hidden: shown)
    }
    filters[index] = std::move(f);
    put_filters(doc.edit_page(at).layers[i], std::move(filters));
    c.report = Json{{"index", index}};
}

void move(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_at(doc.page(at), c.op);
    Json filters = filters_of(doc.page(at).layers[i]);
    if (filters.empty()) throw OpError("the smart object has no smart filters");
    const std::size_t from = index_at(c.op, "index", filters.size(), filters.size() - 1);
    const std::size_t to = index_at(c.op, "to", filters.size(), std::nullopt);
    Json f = filters[from];
    filters.erase(filters.begin() + static_cast<std::ptrdiff_t>(from));
    filters.insert(filters.begin() + static_cast<std::ptrdiff_t>(to), std::move(f));
    put_filters(doc.edit_page(at).layers[i], std::move(filters));
    c.report = Json{{"index", to}};
}

void remove(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_at(doc.page(at), c.op);
    Json filters = filters_of(doc.page(at).layers[i]);
    if (filters.empty() && !(core::truthy_at(c.op, "all") && has_mask(doc.page(at).layers[i]))) throw OpError("the smart object has no smart filters");
    if (core::truthy_at(c.op, "all")) {  // (スマートフィルターをクリア: every one, and the mask)
        Layer& layer = doc.edit_page(at).layers[i];
        layer.smart->erase("filter_mask");
        put_filters(layer, Json::array());
        return;
    }
    const std::size_t index = index_at(c.op, "index", filters.size(), filters.size() - 1);
    filters.erase(filters.begin() + static_cast<std::ptrdiff_t>(index));
    put_filters(doc.edit_page(at).layers[i], std::move(filters));
}

void state(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_at(doc.page(at), c.op);
    const Layer& seen = doc.page(at).layers[i];
    const Json* on = core::get(c.op, "filters_on");
    const Json* mask_on = core::get(c.op, "mask_on");
    const Json* mask = core::get(c.op, "mask");
    if ((on == nullptr || on->is_null()) && (mask_on == nullptr || mask_on->is_null()) && (mask == nullptr || mask->is_null()))
        throw OpError("smart_filter_state changes filters_on, mask_on or mask");
    if (on != nullptr && !on->is_null()) {
        if (!on->is_boolean()) throw OpError("filters_on is true or false");
        if (filters_of(seen).empty()) throw OpError("the smart object has no smart filters");
    }
    if (mask_on != nullptr && !mask_on->is_null()) {
        if (!mask_on->is_boolean()) throw OpError("mask_on is true or false");
        if (!has_mask(seen)) throw OpError("the smart object has no filter mask");
    }
    std::optional<Json> made;
    bool drop = false;
    if (mask != nullptr && !mask->is_null()) {
        if (*mask == Json("delete")) {
            if (!has_mask(seen)) throw OpError("the smart object has no filter mask");
            drop = true;
        } else if (*mask == Json("from_area")) {
            if (!core::truthy_at(c.op, "area")) throw OpError("mask from_area takes the area");
            made = mask_from(doc, doc.page(at), c.op);
        } else {
            throw OpError("mask is delete or from_area");
        }
    }
    Layer& layer = doc.edit_page(at).layers[i];
    Json& s = *layer.smart;
    if (on != nullptr && on->is_boolean()) {
        if (on->get<bool>()) s.erase("filters_on");  // (none: on)
        else s["filters_on"] = false;
    }
    if (drop) s.erase("filter_mask");
    if (made) {
        if (has_mask(layer) && s["filter_mask"].contains("enabled")) (*made)["enabled"] = s["filter_mask"]["enabled"];
        s["filter_mask"] = std::move(*made);
    }
    if (mask_on != nullptr && mask_on->is_boolean() && has_mask(layer)) s["filter_mask"]["enabled"] = *mask_on;
    put_filters(layer, filters_of(layer));
}

}  // namespace

void register_ops(core::OpRegistry& registry) {
    registry.add("smart_filter_add", add);
    registry.add("smart_filter_set", set);
    registry.add("smart_filter_move", move);
    registry.add("smart_filter_delete", remove);
    registry.add("smart_filter_state", state);
}

}  // namespace genko::render::smart_filters
