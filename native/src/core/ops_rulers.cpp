// The ruler ops (Python's ops._apply_one): set_ruler (the old single perspective ruler), add_ruler, edit_ruler,
// delete_ruler and ruler_to_layer (定規ペン: the ruler itself drawn as pen lines on a layer). No pictures.
//
// Where Python would keep a number that is not finite (an angle or a point of "inf" or "nan", which its json.dumps
// writes as Infinity / NaN), the op is refused here: a book never holds one (docs/cpp-migration/ARCHITECTURE.md §3).

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/rulers.hpp"

namespace genko::core {

namespace {

// op.get(key) is not None
const Json* given(const Json& op, std::string_view key) {
    const Json* value = get(op, key);
    return value != nullptr && !value->is_null() ? value : nullptr;
}

// round(float(v), 3)
double rounded3(const Json& value) { return py_round(to_float(value), 3); }

// [[round(float(p[0]), 3), round(float(p[1]), 3)] for p in value or []]
Json rounded_points(const Json& value) {
    Json out = Json::array();
    for (const Json& p : iterate(py_or(value, Json::array()))) {
        const double x = rounded3(subscript(p, 0));
        const double y = rounded3(subscript(p, 1));
        out.push_back(Json::array({x, y}));
    }
    return out;
}

// v + shift for a value Python adds a float to (a number; TypeError for the rest, with Python's message).
double plus(const Json& value, double shift) {
    if (value.is_number() || value.is_boolean()) return to_float(value) + shift;
    if (value.is_string() || value.is_array()) {
        throw PyTypeError("can only concatenate " + py_type_name(value) + " (not \"float\") to " + py_type_name(value));
    }
    throw PyTypeError("unsupported operand type(s) for +: '" + py_type_name(value) + "' and 'float'");
}

void set_ruler(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    Json ruler = Json::object();
    const Json* kind = get(c.op, "kind");
    ruler["kind"] = kind != nullptr && py_truthy(*kind) ? py_str(*kind) : std::string("perspective");
    Json points = Json::array();
    const Json* given_points = get(c.op, "points");
    for (const Json& pt : iterate(given_points != nullptr ? py_or(*given_points, Json::array()) : Json::array())) {
        Json tuple = Json::array();  // (tuple(pt))
        for (const Json& v : iterate(pt)) tuple.push_back(v);
        points.push_back(std::move(tuple));
    }
    require_finite(points, "points");
    ruler["points"] = std::move(points);
    c.doc.edit_page(i).ruler = std::move(ruler);
}

void add_or_edit_ruler(OpContext& c, bool add) {
    const std::size_t page_at = require_page(c.doc, c.op);
    const Json& op = c.op;
    Json ruler;
    if (add) {
        const Json* id = get(op, "id");
        const std::string ruler_id = id != nullptr && py_truthy(*id) ? py_str(*id) : new_id();
        const Json* kind = get(op, "kind");
        ruler = Json::object();
        ruler["id"] = ruler_id;
        ruler["kind"] = kind != nullptr && py_truthy(*kind) ? py_str(*kind) : std::string();
        ruler["points"] = Json::array();
        ruler["active"] = true;
        ruler["visible"] = true;
        for (const Json& r : c.doc.page(page_at).rulers) {
            if (py_equals(py_get(r, "id"), ruler["id"])) throw OpError("ruler " + ruler_id + " already exists");
        }
    } else {
        const Json* id = get(op, "id");
        ruler = c.doc.page(page_at).rulers[ruler_index(c.doc.page(page_at), id != nullptr ? *id : Json())];  // (a copy)
        if (!ruler.is_object()) throw PyTypeError("'" + py_type_name(ruler) + "' object does not support item assignment");
    }
    for (const char* key : {"angle", "ratio", "reach_mm", "at"}) {
        if (const Json* value = given(op, key)) ruler[key] = rounded3(*value);
    }
    if (const Json* axis = given(op, "axis")) ruler["axis"] = py_str(*axis);
    bool huge_copies = false;  // (an int beyond 64 bits, which Python would keep: refused at the end)
    if (const Json* copies = given(op, "copies")) {
        const std::int64_t n = to_int_held(*copies);
        huge_copies = n == INT64_MAX || n == INT64_MIN;
        ruler["copies"] = n;
    }
    if (const Json* grid = get(op, "grid")) {  // (perspective: パースのグリッド, how many lines; 0 or null: none)
        if (py_truthy(*grid)) {
            ruler["grid"] = to_int_held(*grid);
        } else {
            ruler.erase("grid");
        }
    }
    for (const char* key : {"mirror", "active", "visible", "lock_horizon", "fixed"}) {
        if (const Json* value = get(op, key)) ruler[key] = py_truthy(*value);
    }
    const Json* fixed_op = get(op, "fixed");
    if (!add && py_truthy(py_get(ruler, "fixed")) && (has(op, "points") || has(op, "horizon_y")) &&
        !(fixed_op != nullptr && fixed_op->is_boolean() && !fixed_op->get<bool>())) {
        throw OpError("the ruler is fixed (unfix it first)");
    }
    const bool perspective = py_equals(py_get(ruler, "kind"), Json("perspective"));
    if (const Json* points = get(op, "points")) {
        Json fresh = rounded_points(*points);
        if (py_truthy(py_get(ruler, "lock_horizon")) && perspective && !add && py_truthy(py_get(ruler, "points"))) {
            if (const auto eye = rulers::horizon(ruler)) {  // (the eye level stays: each point slides onto it)
                const auto [h, d] = *eye;
                for (std::size_t i = 0; i < fresh.size() && i < 2; ++i) {
                    const double px = fresh[i][0].get<double>();
                    const double py = fresh[i][1].get<double>();
                    const double along = (px - h.x) * d.x + (py - h.y) * d.y;
                    fresh[i] = Json::array({py_round(h.x + d.x * along, 3), py_round(h.y + d.y * along, 3)});
                }
            }
        }
        ruler["points"] = std::move(fresh);
    }
    if (const Json* points2 = get(op, "points2")) ruler["points2"] = rounded_points(*points2);
    if (const Json* center = get(op, "center")) {
        const double x = rounded3(subscript(*center, 0));
        const double y = rounded3(subscript(*center, 1));
        ruler["center"] = Json::array({x, y});
    }
    if (const Json* horizon_y = given(op, "horizon_y"); horizon_y != nullptr && perspective) {
        // 目の高さ: the eye level moved up or down, the vanishing points with it
        if (const auto eye = rulers::horizon(ruler)) {
            const double shift = to_float(*horizon_y) - eye->first.y;
            Json moved = Json::array();
            std::size_t i = 0;
            for (const Json& p : iterate(ruler.at("points"))) {
                if (i++ < 2) {
                    const Json x = subscript(p, 0);
                    moved.push_back(Json::array({x, py_round(plus(subscript(p, 1), shift), 3)}));
                } else {
                    moved.push_back(p);
                }
            }
            ruler["points"] = std::move(moved);
        }
    }
    Page& page = c.doc.edit_page(page_at);
    if (const Json* layer_id = get(op, "layer_id")) {
        if (py_truthy(*layer_id)) (void)layer_by_id(page, py_str(*layer_id));
        ruler["layer_id"] = py_truthy(*layer_id) ? *layer_id : Json(nullptr);
    }
    if (const Json* frame_id = get(op, "frame_id")) {
        if (py_truthy(*frame_id)) (void)frame_or_fail(page, *frame_id);
        ruler["frame_id"] = py_truthy(*frame_id) ? *frame_id : Json(nullptr);
    }
    try {
        rulers::validate(ruler);
    } catch (const PyValueError& error) {
        throw OpError(error.what());
    }
    require_finite(ruler);
    if (huge_copies) throw OpError("copies is too large");
    if (add) {
        page.rulers.push_back(std::move(ruler));
    } else {
        const Json id = ruler.contains("id") ? ruler["id"] : throw OpKeyError("'id'");
        for (Json& r : page.rulers) {
            if (py_equals(py_get(r, "id"), id)) r = ruler;
        }
    }
}

void add_ruler(OpContext& c) { add_or_edit_ruler(c, true); }
void edit_ruler(OpContext& c) { add_or_edit_ruler(c, false); }

void ruler_to_layer(OpContext& c) {  // 定規ペン: the ruler itself drawn as pen lines on a layer
    const std::size_t page_at = require_page(c.doc, c.op);
    const Json* id = get(c.op, "id");
    const Json ruler = c.doc.page(page_at).rulers[ruler_index(c.doc.page(page_at), id != nullptr ? *id : Json())];
    Page& page = c.doc.edit_page(page_at);
    Layer& target = paint_target(page, c.op);
    const auto paths = rulers::outline(ruler, rulers::PageSize{page.spec.width_mm, page.spec.height_mm});
    if (paths.empty()) throw OpError("this ruler has no line to draw (only directions)");
    std::optional<std::vector<std::int64_t>> rgb;
    bool huge_rgb = false;  // (an int beyond 64 bits, which Python would keep: refused at the end)
    if (const Json* value = get(c.op, "rgb"); value != nullptr && py_truthy(*value)) {
        std::vector<std::int64_t> all;
        for (const Json& v : iterate(*value)) {
            const std::int64_t n = to_int_held(v);
            huge_rgb = huge_rgb || n == INT64_MAX || n == INT64_MIN;
            all.push_back(n);
        }
        if (all.size() > 3) all.resize(3);
        rgb = std::move(all);
    }
    std::vector<StrokePtr> items = target.strokes->items;
    bool finite = true;
    bool finite_points = true;
    for (const auto& path : paths) {
        auto stroke = std::make_shared<Stroke>();
        stroke->id = new_id();
        for (const auto& p : path) {
            const PointF point{py_round(p.x, 3).value(), py_round(p.y, 3).value()};
            finite_points = finite_points && std::isfinite(point.x) && std::isfinite(point.y);
            stroke->points.push_back(point);
        }
        stroke->pressure.assign(stroke->points.size(), 1.0);
        const Json* width = get(c.op, "width_mm");
        stroke->width_mm = width != nullptr && py_truthy(*width) ? to_float(*width) : 0.5;
        const Json* kind = get(c.op, "kind");
        stroke->kind = brush_kind(kind != nullptr && py_truthy(*kind) ? *kind : Json("mili"), c.doc);
        stroke->rgb = rgb;
        finite = finite && std::isfinite(stroke->width_mm);
        items.push_back(std::move(stroke));
    }
    if (!finite) throw OpError("width_mm must be a finite number");
    if (!finite_points) throw OpError("points must be finite");
    if (huge_rgb) throw OpError("rgb values are too large");
    target.strokes = make_strokes(std::move(items));
}

void delete_ruler(OpContext& c) {
    const std::size_t page_at = require_page(c.doc, c.op);
    const Json* id = get(c.op, "id");
    if (id != nullptr && py_truthy(*id)) {
        (void)ruler_index(c.doc.page(page_at), *id);
        Page& page = c.doc.edit_page(page_at);
        Json kept = Json::array();
        for (const Json& r : page.rulers) {
            if (!py_equals(py_get(r, "id"), *id)) kept.push_back(r);
        }
        page.rulers = std::move(kept);
    } else {
        Page& page = c.doc.edit_page(page_at);
        page.rulers = Json::array();
        page.ruler.reset();
    }
}

}  // namespace

void register_ruler_ops(OpRegistry& registry) {
    registry.add("set_ruler", set_ruler);
    registry.add("add_ruler", add_ruler);
    registry.add("edit_ruler", edit_ruler);
    registry.add("delete_ruler", delete_ruler);
    registry.add("ruler_to_layer", ruler_to_layer);
}

}  // namespace genko::core
