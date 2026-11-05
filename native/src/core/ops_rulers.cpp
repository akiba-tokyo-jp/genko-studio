// The ruler ops (Python's ops._apply_one): set_ruler (the old single perspective ruler), add_ruler, edit_ruler,
// delete_ruler and ruler_to_layer (定規ペン: the ruler itself drawn as pen lines on a layer). No pictures.
//
// Where Python would keep a number that is not finite (an angle or a point of "inf" or "nan", which its json.dumps
// writes as Infinity / NaN), the op is refused here: a book never holds one (docs/cpp-migration/ARCHITECTURE.md §3).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/guide_layout.hpp"
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

// (PC5 H-b) delete_ruler's `kind` (this build's own): with no id, the rulers of that kind alone ("guide": ガイドを消去 and
// カンバスガイドを消去, the drawing rulers left) — without it every ruler, as before.
std::optional<std::string> kind_given(const Json& op) {
    const Json* kind = given(op, "kind");
    if (kind == nullptr) return std::nullopt;
    const auto& all = rulers::kinds();
    if (!kind->is_string() || std::find(all.begin(), all.end(), kind->get<std::string>()) == all.end()) {
        std::string names;
        for (const auto& k : all) names += (names.empty() ? "" : ", ") + k;
        throw OpError("kind must be one of " + names);
    }
    return kind->get<std::string>();
}

void delete_ruler(OpContext& c) {
    const std::size_t page_at = require_page(c.doc, c.op);
    const Json* id = get(c.op, "id");
    const std::optional<std::string> kind = kind_given(c.op);
    if (kind && !(id != nullptr && py_truthy(*id))) {
        Page& page = c.doc.edit_page(page_at);
        Json kept = Json::array();
        for (const Json& r : page.rulers) {
            if (!py_equals(py_get(r, "kind"), Json(*kind))) kept.push_back(r);
        }
        page.rulers = std::move(kept);
        if (page.ruler && py_equals(py_get(*page.ruler, "kind"), Json(*kind))) page.ruler.reset();
        return;
    }
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

// --- 新規ガイドレイアウトを作成 (PC5 H-b, view.newGuideLayout; PhotoCraft v0.2.0's file_cmds.rs new_guide_layout and
// layout_lines, MIT — native/third_party/photocraft/NOTICE.md) ------------------------------------------------------------

// A finite number of the op (none: not given), refused when it is not one.
std::optional<double> finite_of(const Json& holder, std::string_view key, std::string_view what) {
    const Json* value = given(holder, key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_number() || value->is_boolean()) throw OpError(std::string(what) + " must be a number");
    const double v = value->get<double>();
    if (!std::isfinite(v)) throw OpError(std::string(what) + " must be finite");
    return v;
}

// Columns (or rows) across [start, end]: both edges of each, a fixed width packed from the start (or centred), else the
// width that fills it (upstream's layout_lines, in mm).
void layout_lines(double start, double end, std::int64_t count, std::optional<double> width, double gutter, bool center,
                  const std::function<void(double)>& push) {
    if (count <= 0) return;
    const double avail = std::max(0.0, end - start);
    const double n = static_cast<double>(count);
    const double w = width ? *width : std::max(0.0, (avail - (n - 1.0) * gutter) / n);
    const double total = n * w + (n - 1.0) * gutter;
    const double x0 = center && width ? start + (avail - total) / 2.0 : start;
    for (std::int64_t i = 0; i < count; ++i) {
        const double a = x0 + static_cast<double>(i) * (w + gutter);
        push(a);
        push(a + w);
    }
}

struct LayoutAxis {
    std::int64_t count = 0;
    std::optional<double> size;
    double gutter = 0;
};

LayoutAxis axis_of(const Json& op, std::string_view key, std::string_view size_key) {
    LayoutAxis out;
    const Json* part = given(op, key);
    if (part == nullptr) return out;
    if (!part->is_object()) throw OpError(std::string(key) + " is {\"count\", \"" + std::string(size_key) + "\"?, \"gutter_mm\"?}");
    const Json* count = given(*part, "count");
    if (count == nullptr || !count->is_number_integer() || count->get<std::int64_t>() < 0 || count->get<std::int64_t>() > 100) {
        throw OpError(std::string(key) + ".count must be a whole number from 0 to 100");
    }
    out.count = count->get<std::int64_t>();
    out.size = finite_of(*part, size_key, std::string(key) + "." + std::string(size_key));
    if (out.size && *out.size <= 0) throw OpError(std::string(key) + "." + std::string(size_key) + " must be above 0");
    out.gutter = finite_of(*part, "gutter_mm", std::string(key) + ".gutter_mm").value_or(0.0);
    if (out.gutter < 0) throw OpError(std::string(key) + ".gutter_mm must be 0 or more");
    return out;
}

}  // namespace

GuideLayout guide_layout(const Page& before, const Json& op) {
    const LayoutAxis columns = axis_of(op, "columns", "width_mm");
    const LayoutAxis rows = axis_of(op, "rows", "height_mm");
    std::array<double, 4> margin{0, 0, 0, 0};  // top, left, bottom, right
    if (const Json* m = given(op, "margin_mm")) {
        if (m->is_array()) {
            if (m->size() != 4) throw OpError("margin_mm is a number or [top, left, bottom, right]");
            for (std::size_t i = 0; i < 4; ++i) {
                const Json& v = (*m)[i];
                if (!v.is_number() || v.is_boolean() || !std::isfinite(v.get<double>())) throw OpError("margin_mm is a number or [top, left, bottom, right]");
                margin[i] = v.get<double>();
            }
        } else {
            const double all = finite_of(op, "margin_mm", "margin_mm").value_or(0.0);
            margin = {all, all, all, all};
        }
    }
    std::string from = "paper";
    if (const Json* f = given(op, "from")) {
        if (!f->is_string() || (*f != "paper" && *f != "trim" && *f != "frame")) throw OpError("from is paper, trim or frame");
        from = f->get<std::string>();
    }
    const auto flag = [&op](const char* key) {
        const Json* v = given(op, key);
        if (v != nullptr && !v->is_boolean()) throw OpError(std::string(key) + " is true or false");
        return v != nullptr && v->get<bool>();
    };
    const bool center = flag("center_columns");
    const bool clear = flag("clear");
    const Rect base = from == "trim" ? before.trim_rect_mm() : from == "frame" ? before.inner_rect_mm() : before.paper_rect_mm();
    const double x0 = base.x.value(), y0 = base.y.value(), x1 = x0 + base.width.value(), y1 = y0 + base.height.value();
    const auto [top, left, bottom, right] = margin;
    // (each axis's positions in order, none twice — nor where a guide of the page already is, unless they are cleared)
    std::vector<double> across, down;  // (across: y of "h" guides; down: x of "v" guides)
    std::vector<double> had_across, had_down;
    if (!clear) {
        for (const Json& r : before.rulers) {
            if (!py_equals(py_get(r, "kind"), Json("guide"))) continue;
            const Json at = py_get(r, "at");
            if (!at.is_number()) continue;
            (py_equals(py_get(r, "axis"), Json("h")) ? had_across : had_down).push_back(at.get<double>());
        }
    }
    const auto pusher = [](std::vector<double>& list, const std::vector<double>& had) {
        return [&list, &had](double v) {
            const double at = py_round(v, 3);
            const auto near = [at](double g) { return std::abs(g - at) < 0.01; };
            if (std::any_of(list.begin(), list.end(), near) || std::any_of(had.begin(), had.end(), near)) return;
            list.push_back(at);
        };
    };
    const auto push_down = pusher(down, had_down);
    const auto push_across = pusher(across, had_across);
    if (top != 0 || left != 0 || bottom != 0 || right != 0) {
        push_down(x0 + left);
        push_down(x1 - right);
        push_across(y0 + top);
        push_across(y1 - bottom);
    }
    layout_lines(x0 + left, x1 - right, columns.count, columns.size, columns.gutter, center, push_down);
    layout_lines(y0 + top, y1 - bottom, rows.count, rows.size, rows.gutter, center, push_across);
    if (columns.count == 0 && rows.count == 0 && top == 0 && left == 0 && bottom == 0 && right == 0) {
        throw OpError("give columns, rows or a margin_mm");
    }
    for (double v : down) {
        if (!std::isfinite(v)) throw OpError("the layout's guides must be finite");
    }
    for (double v : across) {
        if (!std::isfinite(v)) throw OpError("the layout's guides must be finite");
    }
    return GuideLayout{std::move(down), std::move(across), clear};
}

namespace {

void add_guide_layout(OpContext& c) {
    const std::size_t page_at = require_page(c.doc, c.op);
    const auto [down, across, clear] = guide_layout(c.doc.page(page_at), c.op);
    Page& page = c.doc.edit_page(page_at);
    if (clear) {
        Json kept = Json::array();
        for (const Json& r : page.rulers) {
            if (!py_equals(py_get(r, "kind"), Json("guide"))) kept.push_back(r);
        }
        page.rulers = std::move(kept);
    }
    Json ids = Json::array();
    const auto add = [&](const char* axis, double at) {
        const std::string id = new_id();
        page.rulers.push_back(Json{{"id", id}, {"kind", "guide"}, {"points", Json::array()}, {"active", true}, {"visible", true}, {"axis", axis}, {"at", at}});
        ids.push_back(id);
    };
    for (double x : down) add("v", x);
    for (double y : across) add("h", y);
    c.report = Json{{"rulers", ids}, {"vertical", down.size()}, {"horizontal", across.size()}};
}

}  // namespace

void register_ruler_ops(OpRegistry& registry) {
    registry.add("set_ruler", set_ruler);
    registry.add("add_ruler", add_ruler);
    registry.add("edit_ruler", edit_ruler);
    registry.add("delete_ruler", delete_ruler);
    registry.add("add_guide_layout", add_guide_layout);  // (PC5 H-b: this build's own, SPEC.md VIEW-01)
    registry.add("ruler_to_layer", ruler_to_layer);
}

}  // namespace genko::core
