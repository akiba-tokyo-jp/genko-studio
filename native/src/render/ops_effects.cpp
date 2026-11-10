// The effect line ops (Python's ops._apply_one: add_effect, edit_effect, delete_effect, effect_to_layer). An effect is
// kept as its settings on the page; effect_to_layer turns it into pen lines (効果線ペン) and fills on a layer
// (effects.to_layer), with Python's points, widths and fill pictures.
//
// The settings are kept as given, as in Python (a centre of "inf" stays a str). A book never holds a number that is not
// finite (docs/cpp-migration/ARCHITECTURE.md §3), where Python would write one into project.json as Infinity or NaN: a
// setting that is such a number is refused when it is set, and effect_to_layer refuses an effect whose lines would
// hold one (Python adds them to the layer).

#include <cmath>
#include <optional>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/effects.hpp"
#include "render/fill_patches.hpp"
#include "render/ops_registry.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpContext;
using core::OpError;

const Json* truthy(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr && core::py_truthy(*value) ? value : nullptr;
}

Json op_id(const Json& op) {
    const Json* id = core::get(op, "id");
    return id != nullptr ? *id : Json();
}

// ops._effect(page, effect_id): the position of the first effect whose id == effect_id; OpError "no effect <id>".
std::size_t effect_index(const core::Page& page, const Json& effect_id) {
    for (std::size_t i = 0; i < page.effects.size(); ++i) {
        if (core::py_equals(core::py_get(page.effects[i], "id"), effect_id)) return i;
    }
    throw OpError("no effect " + core::py_str(effect_id));
}

void validated(const Json& kind, const Json& params) {
    try {
        effects::validate(kind, params);
    } catch (const core::PyValueError& error) {
        throw OpError(error.what());
    }
}

void add_effect(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* kind_value = truthy(c.op, "kind");
    const Json kind = kind_value != nullptr ? core::py_str(*kind_value) : std::string();
    const Json* params_value = truthy(c.op, "params");
    const Json params = core::py_dict(params_value != nullptr ? *params_value : Json::object());
    validated(kind, params);
    const Json* frame_id = truthy(c.op, "frame_id");
    if (frame_id != nullptr) (void)core::frame_or_fail(c.doc.page(at), *frame_id);
    core::require_finite(params);  // (Python keeps the settings as given: Infinity or NaN would go into project.json)
    Json effect = Json::object();
    const Json* id = truthy(c.op, "id");
    effect["id"] = id != nullptr ? core::py_str(*id) : core::new_id();
    effect["kind"] = kind;
    const Json* given_frame = core::get(c.op, "frame_id");
    effect["frame_id"] = given_frame != nullptr ? *given_frame : Json();
    effect["params"] = params;
    c.doc.edit_page(at).effects.push_back(std::move(effect));
}

void edit_effect(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const std::size_t index = effect_index(c.doc.page(at), op_id(c.op));
    const Json& seen = c.doc.page(at).effects[index];
    Json params = core::py_dict(core::py_or(core::py_get(seen, "params"), Json::object()));
    const Json* changes = truthy(c.op, "params");
    const Json given = core::py_dict(changes != nullptr ? *changes : Json::object());
    for (const auto& [key, value] : given.items()) {
        if (value.is_null()) {
            params.erase(key);
        } else {
            params[key] = value;
        }
    }
    const Json* kind_value = truthy(c.op, "kind");
    Json kind;
    if (kind_value != nullptr) {
        kind = core::py_str(*kind_value);
    } else {
        if (!seen.contains("kind")) throw core::OpKeyError("'kind'");
        kind = core::py_str(seen["kind"]);
    }
    validated(kind, params);
    core::Page& page = c.doc.edit_page(at);
    Json& effect = page.effects[index];
    if (const Json* frame_id = core::get(c.op, "frame_id")) {
        if (core::py_truthy(*frame_id)) (void)core::frame_or_fail(page, *frame_id);
        effect["frame_id"] = core::py_truthy(*frame_id) ? *frame_id : Json();
    }
    if (const Json* visible = core::get(c.op, "visible")) effect["visible"] = core::py_truthy(*visible);
    core::require_finite(params);
    effect["kind"] = kind;
    effect["params"] = std::move(params);
}

void delete_effect(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json id = op_id(c.op);
    (void)effect_index(c.doc.page(at), id);
    core::Page& page = c.doc.edit_page(at);
    Json kept = Json::array();
    for (const Json& e : page.effects) {
        if (!core::py_equals(core::py_get(e, "id"), id)) kept.push_back(e);
    }
    page.effects = std::move(kept);
}

// Whether every number of the lines and fills is finite. Settings Python stores as given (a centre or a width of
// "inf") can make lines of Infinity and NaN, which Python adds to the layer and writes into project.json: refused here.
bool finite(const effects::Geometry& geo) {
    for (const effects::Line& line : geo.lines) {
        if (!std::isfinite(line.width_mm)) return false;
        for (const auto& p : line.points) {
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
        }
    }
    for (const effects::Fill& fill : geo.fills) {
        for (const effects::XY& p : fill.points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
        }
    }
    return true;
}

// effects.to_layer(effect, page, layer): the effect as pen lines (効果線ペン) and fills on a layer.
void to_layer(const Json& effect, const core::Page& page, core::Layer& layer) {
    const effects::Geometry geo = effects::geometry(effect, page);
    for (const effects::Fill& fill : geo.fills) {  // (first, as in Python: their errors come before the lines')
        std::vector<std::array<double, 2>> points;
        for (const effects::XY& p : fill.points) points.push_back({p.x, p.y});
        if (auto patch = tone_fills::polygon_patch(points, fill.rgb)) layer.patches.push_back(std::move(*patch));
    }
    if (!finite(geo)) throw OpError("this effect's settings give lines that are not finite numbers");
    std::vector<core::StrokePtr> items = layer.strokes->items;
    for (const effects::Line& line : geo.lines) {
        auto stroke = std::make_shared<core::Stroke>();
        stroke->id = core::new_id();  // (coerce_stroke(points))
        for (const auto& p : line.points) {
            stroke->points.push_back(core::PointF{p[0], p[1]});
            stroke->pressure.push_back(p[2]);
        }
        stroke->kind = "fx";
        stroke->width_mm = core::py_round(line.width_mm, 3);
        if (geo.rgb != effects::kInk) stroke->rgb = geo.rgb;
        items.push_back(std::move(stroke));
    }
    layer.strokes = core::make_strokes(std::move(items));
}

void effect_to_layer(OpContext& c) {  // (strict_gates: the CommandBus's check of the raster edits)
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json id = op_id(c.op);
    const Json effect = c.doc.page(at).effects[effect_index(c.doc.page(at), id)];
    core::Page& page = c.doc.edit_page(at);
    core::Layer& target = core::paint_target(page, c.op);
    to_layer(effect, page, target);
    if (truthy(c.op, "keep") == nullptr) {
        Json kept = Json::array();
        for (const Json& e : page.effects) {
            if (!core::py_equals(core::py_get(e, "id"), id)) kept.push_back(e);
        }
        page.effects = std::move(kept);
    }
}

}  // namespace

void register_effect_ops(core::OpRegistry& registry) {
    registry.add("add_effect", add_effect);
    registry.add("edit_effect", edit_effect);
    registry.add("delete_effect", delete_effect);
    registry.add("effect_to_layer", effect_to_layer);
}

}  // namespace genko::render
