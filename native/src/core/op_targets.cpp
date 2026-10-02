#include "core/op_targets.hpp"

#include <algorithm>
#include <cmath>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyvalue.hpp"

namespace genko::core {

namespace {

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

}  // namespace

std::size_t layer_index_by_id(const Page& page, std::string_view layer_id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == layer_id) return i;
    }
    throw OpError("no layer " + std::string(layer_id));
}

Layer& paint_target(Page& page, const Json& op) {
    Layer* target = nullptr;
    const Json* layer_id = find(op, "layer_id");
    if (layer_id != nullptr && py_truthy(*layer_id)) {
        target = &page.layers[layer_index_by_id(page, py_str(*layer_id))];
    } else {
        const Json* role_value = find(op, "layer");
        const std::string text = role_value != nullptr && py_truthy(*role_value) ? py_str(*role_value) : std::string("ink");
        const auto role = layer_role_from(text);
        if (!role) throw PyValueError(py_repr_str(text) + " is not a valid LayerRole");
        target = &page.layer_for(*role);
    }
    if (target->locked) throw OpError("the layer is locked");
    if (target->kind != LayerKind::Strokes && target->kind != LayerKind::Raster && target->kind != LayerKind::Tone) {
        throw OpError("this layer cannot be painted on (choose a pen, paint or tone layer)");
    }
    return *target;
}

const Frame& frame_or_fail(const Page& page, const Json& frame_id) {
    const Frame* frame = page.find_frame(py_str(frame_id));
    if (frame == nullptr) throw OpError("no panel " + py_str(frame_id));
    return *frame;
}

std::size_t ruler_index(const Page& page, const Json& ruler_id) {
    for (std::size_t i = 0; i < page.rulers.size(); ++i) {
        if (py_equal(py_get(page.rulers[i], "id"), ruler_id)) return i;
    }
    throw OpError("no ruler " + py_str(ruler_id));
}

const std::vector<std::string>& builtin_brush_kinds() {
    static const std::vector<std::string> kinds{"gpen",   "maru",     "kabura", "mili",    "pencil",   "fude",   "marker",
                                                "airbrush", "fill_pen", "white",  "fx",      "calligraphy", "water",
                                                "spray",  "stipple",  "dotline", "dashline", "lace",   "grass",
                                                "leaves", "hearts",   "stars"};
    return kinds;
}

std::string brush_kind(const Json& kind_value, const Document& doc) {
    std::string kind = py_str(kind_value);
    if (kind == "oil") kind = "marker";  // (brushes.LEGACY)
    const auto& kinds = builtin_brush_kinds();
    const bool known = std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    const bool custom = doc.brush_custom.is_object() && doc.brush_custom.contains(kind);
    if (!known && !custom) {
        std::string names;
        for (const auto& k : kinds) names += (names.empty() ? "" : ", ") + k;
        throw OpError("kind must be one of " + names + " (or a brush defined in the book with define_brush)");
    }
    return kind;
}

void check_strict_raster_edit(const Document& doc, const Json& op, std::string_view name) {
    if (!doc.strict_gates) return;
    const Json* layer_id = find(op, "layer_id");
    if (layer_id != nullptr && py_truthy(*layer_id)) {
        const Page& page = doc.page(require_page(doc, op));
        const Layer* target = nullptr;
        for (const Layer& layer : page.layers) {
            if (py_equal(Json(layer.id), *layer_id)) {
                target = &layer;
                break;
            }
        }
        // (a layer that is not printed — the name, a draft, or one set exportable:false — may be drawn on before the
        // name is approved: trying a line out does not change the page)
        if (target != nullptr && target->role != LayerRole::Name && target->role != LayerRole::Draft && target->exportable &&
            !page.name_ok) {
            throw OpError(std::string(name) + " on a printed layer needs name_ok on page " + page.index.repr() + " (strict_gates)");
        }
        return;
    }
    const Json* layer = find(op, "layer");
    const std::string role = layer != nullptr && py_truthy(*layer) ? py_str(*layer) : std::string(name != "filter_raster" ? "ink" : "");
    if (role != "name" && role != "draft" && !role.empty()) {
        const Page& page = doc.page(require_page(doc, op));
        if (!page.name_ok) {
            throw OpError(std::string(name) + " on " + role + " needs name_ok on page " + page.index.repr() + " (strict_gates)");
        }
    }
}

void require_finite(const Json& value, const std::string& what) {
    if (value.is_number_float() && !std::isfinite(value.get<double>())) throw OpError(what + " must be a finite number");
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) require_finite(item, what.empty() ? key : what + "." + key);
    } else if (value.is_array()) {
        for (const Json& item : value) require_finite(item, what);
    }
}

}  // namespace genko::core
