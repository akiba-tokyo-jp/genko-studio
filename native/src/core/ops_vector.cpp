// The vector line edits (Python's ops._vector_edit and ops._trace_edit): ベクター線の編集 — control points moved,
// added or taken out, a point's own width, two lines joined, a line cut in two, lines recoloured, deleted or given fewer
// points — and 線の直しを、なぞって — the lines near a trace widened or narrowed there, redrawn along it, given its
// pressure, joined where their ends meet it, or simplified. No pictures.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"

namespace genko::core {

namespace {

constexpr const char* kVectorActions = "move_point, add_point, delete_point, connect, cut, recolor, delete, set_pressure, simplify";
constexpr const char* kTraceActions = "widen, narrow, redraw, redraw_width, join, simplify";

Json op_get(const Json& op, std::string_view key) {
    const Json* value = get(op, key);
    return value != nullptr ? *value : Json();
}

// ops._stroke_by_id: the position of the first line with this id.
std::size_t stroke_by_id(const Layer& layer, const std::string& stroke_id) {
    const auto& items = layer.strokes->items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i]->id == stroke_id) return i;
    }
    throw OpError("no stroke " + stroke_id);
}

// `list(stroke.pressure) if len(stroke.pressure) == len(stroke.points) else [0.7] * len(stroke.points)`
std::vector<double> pressure_or_default(const Stroke& stroke) {
    if (stroke.pressure.size() == stroke.points.size()) return stroke.pressure;
    return std::vector<double>(stroke.points.size(), 0.7);
}

// round(max(0.05, min(1.5, v)), 3)
double held_pressure(double v) { return py_round(py_max(0.05, py_min(1.5, v)), 3); }

// int(op.get("index", -1)) as a point of the line (OpError otherwise)
std::size_t point_index(const Json& op, std::size_t count) {
    const Json* given = get(op, "index");
    const std::int64_t k = given != nullptr ? to_int_held(*given) : -1;
    if (k < 0 || k >= static_cast<std::int64_t>(count)) throw OpError("index is not a point of the line");
    return static_cast<std::size_t>(k);
}

// `at`: [x, y] as Python reads it (float(at[0]), float(at[1]))
std::pair<double, double> xy_of(const Json& at) { return {to_float(subscript(at, 0)), to_float(subscript(at, 1))}; }

void vector_edit(OpContext& c) {
    const std::size_t page_at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(page_at);
    Layer& layer = paint_target(page, c.op);
    const Json& op = c.op;
    const std::string action = truthy_at(op, "action") ? py_str(op["action"]) : std::string();
    static const std::set<std::string> kKnown{"move_point", "add_point", "delete_point", "connect", "cut", "recolor", "delete",
                                              "set_pressure", "simplify"};
    if (!kKnown.contains(action)) throw OpError(std::string("action must be one of ") + kVectorActions);
    std::vector<StrokePtr> items = layer.strokes->items;
    if (action == "recolor" || action == "delete") {
        std::vector<std::string> ids;
        if (truthy_at(op, "ids")) {
            for (const Json& v : iterate(op["ids"])) ids.push_back(py_str(v));
        } else if (truthy_at(op, "stroke_id")) {
            ids.push_back(py_str(subscript(op, "stroke_id")));
        }
        if (ids.empty()) throw OpError("ids (or stroke_id) is required");
        for (const std::string& id : ids) (void)stroke_by_id(layer, id);
        const auto named = [&](const StrokePtr& s) { return std::find(ids.begin(), ids.end(), s->id) != ids.end(); };
        if (action == "delete") {
            std::erase_if(items, named);
        } else {
            const Json rgb = op_get(op, "rgb");
            std::optional<std::vector<std::int64_t>> colour;
            if (py_truthy(rgb)) {
                std::vector<std::int64_t> all;
                for (const Json& v : iterate(rgb)) all.push_back(to_int(v));
                if (all.size() > 3) all.resize(3);
                colour = std::move(all);
            }
            for (StrokePtr& s : items) {
                if (!named(s)) continue;
                auto changed = std::make_shared<Stroke>(*s);
                changed->rgb = colour;
                s = std::move(changed);
            }
        }
        layer.strokes = make_strokes(std::move(items));
        return;
    }
    if (action == "connect") {
        std::vector<std::string> ids;
        for (const Json& v : iterate(py_or(op_get(op, "ids"), Json::array()))) ids.push_back(py_str(v));
        if (ids.size() != 2 || ids[0] == ids[1]) throw OpError("connect takes two line ids");
        const std::size_t ia = stroke_by_id(layer, ids[0]);
        const std::size_t ib = stroke_by_id(layer, ids[1]);
        const Stroke& a = *items[ia];
        const Stroke& b = *items[ib];
        std::vector<PointF> pa = a.points, pb = b.points;
        std::vector<double> pra = !a.pressure.empty() ? a.pressure : std::vector<double>(pa.size(), 0.7);
        std::vector<double> prb = !b.pressure.empty() ? b.pressure : std::vector<double>(pb.size(), 0.7);
        if (pa.empty() || pb.empty()) raise_index_error();
        // join at the nearest ends (turning either line round as needed): min over (distance, flip a, flip b)
        const std::tuple<double, bool, bool> ends[] = {
            {py_dist(pa.back().x, pa.back().y, pb.front().x, pb.front().y), false, false},
            {py_dist(pa.back().x, pa.back().y, pb.back().x, pb.back().y), false, true},
            {py_dist(pa.front().x, pa.front().y, pb.front().x, pb.front().y), true, false},
            {py_dist(pa.front().x, pa.front().y, pb.back().x, pb.back().y), true, true}};
        const auto& [unused, flip_a, flip_b] = *std::min_element(std::begin(ends), std::end(ends));
        if (flip_a) {
            std::reverse(pa.begin(), pa.end());
            std::reverse(pra.begin(), pra.end());
        }
        if (flip_b) {
            std::reverse(pb.begin(), pb.end());
            std::reverse(prb.begin(), prb.end());
        }
        auto joined = std::make_shared<Stroke>(a);
        joined->points = pa;
        joined->points.insert(joined->points.end(), pb.begin(), pb.end());
        if (!a.pressure.empty() || !b.pressure.empty()) {
            joined->pressure = pra;
            joined->pressure.insert(joined->pressure.end(), prb.begin(), prb.end());
        } else {
            joined->pressure.clear();
        }
        const StrokePtr a_ptr = items[ia], b_ptr = items[ib];
        std::vector<StrokePtr> out;
        for (const StrokePtr& s : items) {
            if (s == b_ptr) continue;
            out.push_back(s == a_ptr ? StrokePtr(joined) : s);
        }
        layer.strokes = make_strokes(std::move(out));
        return;
    }
    const std::string stroke_id = truthy_at(op, "stroke_id") ? py_str(op["stroke_id"]) : std::string();
    const std::size_t index = stroke_by_id(layer, stroke_id);
    const Stroke& stroke = *items[index];
    std::vector<PointF> points = stroke.points;
    std::vector<double> pressure = stroke.pressure;
    auto changed = std::make_shared<Stroke>(stroke);
    if (action == "set_pressure") {  // 制御点ごとの線幅: one point wider or thinner than the line's width
        const std::size_t k = point_index(op, points.size());
        if (pressure.size() != points.size()) pressure.assign(points.size(), 0.7);
        const Json* given = get(op, "pressure");
        pressure[k] = held_pressure(given != nullptr ? to_float(*given) : 0.7);
        changed->pressure = pressure;
        items[index] = changed;
        layer.strokes = make_strokes(std::move(items));
        return;
    }
    if (action == "simplify") {
        const Json* given = get(op, "epsilon_mm");
        const PenPoints simpler = rdp(stroke_points(stroke), py_max(0.01, given != nullptr ? to_float(*given) : 0.2));
        changed->points.clear();
        std::vector<double> kept;
        for (const PenPoint& p : simpler) {
            changed->points.push_back(PointF{p.x, p.y});
            if (!stroke.pressure.empty() && !p.p) raise_index_error("tuple index out of range");  // (pressures not one a point)
            kept.push_back(p.p.value_or(0.0));
        }
        changed->pressure = stroke.pressure.empty() ? std::vector<double>() : kept;
        items[index] = changed;
        layer.strokes = make_strokes(std::move(items));
        return;
    }
    if (action == "move_point") {
        const std::size_t k = point_index(op, points.size());
        const Json to = op_get(op, "to");
        const auto [x, y] = xy_of(to);
        points[k] = PointF{py_round(x, 3), py_round(y, 3)};
    } else if (action == "add_point") {
        const auto [x, y] = xy_of(op_get(op, "at"));
        const NearestSegment closest = nearest_segment(points, x, y);
        // (list.insert: past the end is the end — a line with no points takes the point as its first)
        const std::size_t at = std::min(closest.index + 1, points.size());
        points.insert(points.begin() + static_cast<std::ptrdiff_t>(at), PointF{py_round(closest.x, 3), py_round(closest.y, 3)});
        if (!pressure.empty()) {
            const std::size_t seg = closest.index;
            const std::size_t next = std::min(seg + 1, pressure.size() - 1);
            if (seg >= pressure.size()) raise_index_error();
            pressure.insert(pressure.begin() + static_cast<std::ptrdiff_t>(seg) + 1, (pressure[seg] + pressure[next]) / 2);
        }
    } else if (action == "delete_point") {
        const std::size_t k = point_index(op, points.size());
        if (points.size() <= 2) throw OpError("a line keeps at least two points (delete the line instead)");
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(k));
        if (!pressure.empty()) {
            if (k >= pressure.size()) raise_index_error("list assignment index out of range");
            pressure.erase(pressure.begin() + static_cast<std::ptrdiff_t>(k));
        }
    } else if (action == "cut") {
        const auto [x, y] = xy_of(op_get(op, "at"));
        const NearestSegment closest = nearest_segment(points, x, y);
        const std::size_t seg = closest.index;
        std::vector<PointF> first(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(std::min(seg + 1, points.size())));
        first.push_back(PointF{closest.x, closest.y});
        std::vector<PointF> second{PointF{closest.x, closest.y}};
        if (seg + 1 < points.size()) second.insert(second.end(), points.begin() + static_cast<std::ptrdiff_t>(seg) + 1, points.end());
        if (first.size() < 2 || second.size() < 2) throw OpError("that is the end of the line");
        auto tail = std::make_shared<Stroke>(stroke);
        tail->id = new_id();
        tail->points = second;
        changed->points = first;
        if (!pressure.empty()) {
            if (seg >= pressure.size()) raise_index_error();
            const double mid = pressure[seg];
            changed->pressure.assign(pressure.begin(), pressure.begin() + static_cast<std::ptrdiff_t>(seg) + 1);
            changed->pressure.push_back(mid);
            tail->pressure = {mid};
            tail->pressure.insert(tail->pressure.end(), pressure.begin() + static_cast<std::ptrdiff_t>(seg) + 1, pressure.end());
        }
        items[index] = changed;
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(index) + 1, tail);
        layer.strokes = make_strokes(std::move(items));
        return;
    }
    changed->points = points;
    changed->pressure = pressure;
    items[index] = changed;
    layer.strokes = make_strokes(std::move(items));
}

void trace_edit(OpContext& c) {
    const std::size_t page_at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(page_at);
    Layer& layer = paint_target(page, c.op);
    const Json& op = c.op;
    const std::string action = truthy_at(op, "action") ? py_str(op["action"]) : std::string();
    static const std::set<std::string> kKnown{"widen", "narrow", "redraw", "redraw_width", "join", "simplify"};
    if (!kKnown.contains(action)) throw OpError(std::string("action must be one of ") + kTraceActions);
    struct TracePoint {
        double x, y, p;
    };
    std::vector<TracePoint> trace;
    for (const Json& p : iterate(py_or(op_get(op, "points"), Json::array()))) {
        const double x = to_float(subscript(p, 0)), y = to_float(subscript(p, 1));
        trace.push_back(TracePoint{x, y, length(p) > 2 ? to_float(subscript(p, 2)) : 0.7});
    }
    if (trace.size() < 2) throw OpError("points needs the trace: two points or more");
    const Json* radius_given = get(op, "radius_mm");
    const double radius = py_max(0.1, radius_given != nullptr ? to_float(*radius_given) : 2.0);
    std::vector<PointF> flat;
    for (const TracePoint& t : trace) flat.push_back(PointF{t.x, t.y});
    const auto closest_to = [&](double x, double y) { return nearest_segment(flat, x, y); };

    std::vector<StrokePtr> items = layer.strokes->items;
    std::vector<std::size_t> touched;  // (positions in items)
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (std::any_of(items[i]->points.begin(), items[i]->points.end(), [&](const PointF& p) { return closest_to(p.x, p.y).distance <= radius; }))
            touched.push_back(i);
    }
    if (touched.empty()) throw OpError("no line of this layer is near the trace");
    const auto done = [&] { layer.strokes = make_strokes(std::move(items)); };
    if (action == "widen" || action == "narrow") {
        const Json* given = get(op, "amount");
        const double amount = py_max(0.0, py_min(1.0, given != nullptr ? to_float(*given) : 0.3));
        for (const std::size_t i : touched) {
            auto stroke = std::make_shared<Stroke>(*items[i]);
            std::vector<double> pressure = pressure_or_default(*stroke);
            for (std::size_t k = 0; k < stroke->points.size(); ++k) {
                const double d = closest_to(stroke->points[k].x, stroke->points[k].y).distance;
                if (d <= radius) {
                    const double w = 1 - py_pow(d / radius, 2);  // (full at the trace, nothing at the edge of its reach)
                    const double factor = action == "widen" ? 1 + amount * w : 1 - 0.7 * amount * w;
                    pressure[k] = held_pressure(pressure[k] * factor);
                }
            }
            stroke->pressure = pressure;
            items[i] = stroke;
        }
        done();
        return;
    }
    if (action == "simplify") {
        const Json* given = get(op, "epsilon_mm");
        const double epsilon = py_max(0.01, given != nullptr ? to_float(*given) : 0.2);
        for (const std::size_t i : touched) {
            auto stroke = std::make_shared<Stroke>(*items[i]);
            const PenPoints simpler = rdp(stroke_points(*items[i]), epsilon);
            const bool had = !stroke->pressure.empty();
            stroke->points.clear();
            stroke->pressure.clear();
            for (const PenPoint& p : simpler) {
                stroke->points.push_back(PointF{p.x, p.y});
                if (had && !p.p) raise_index_error("tuple index out of range");  // (pressures not one a point)
                if (had) stroke->pressure.push_back(*p.p);
            }
            items[i] = stroke;
        }
        done();
        return;
    }
    if (action == "redraw_width") {
        for (const std::size_t i : touched) {
            auto stroke = std::make_shared<Stroke>(*items[i]);
            std::vector<double> pressure = pressure_or_default(*stroke);
            for (std::size_t k = 0; k < stroke->points.size(); ++k) {
                const NearestSegment hit = closest_to(stroke->points[k].x, stroke->points[k].y);
                if (hit.distance <= radius) {
                    const std::size_t seg = hit.index;
                    pressure[k] = held_pressure((trace[seg].p + trace[std::min(seg + 1, trace.size() - 1)].p) / 2);
                }
            }
            stroke->pressure = pressure;
            items[i] = stroke;
        }
        done();
        return;
    }
    if (action == "redraw") {
        // the one line the trace starts and ends on: the part between is replaced by the trace
        std::optional<std::tuple<double, std::size_t, std::size_t, std::size_t>> best;  // (d0 + d1, item, s0, s1)
        for (const std::size_t i : touched) {
            const Stroke& stroke = *items[i];
            if (stroke.points.size() < 2) continue;
            const NearestSegment h0 = nearest_segment(stroke.points, flat.front().x, flat.front().y);
            const NearestSegment h1 = nearest_segment(stroke.points, flat.back().x, flat.back().y);
            if (h0.distance <= radius && h1.distance <= radius && (!best || h0.distance + h1.distance < std::get<0>(*best))) {
                best = std::make_tuple(h0.distance + h1.distance, i, h0.index, h1.index);
            }
        }
        if (!best) throw OpError("the trace must start and end on the same line (within radius_mm)");
        auto [unused, i, s0, s1] = *best;
        const Stroke& old = *items[i];
        std::vector<TracePoint> full;
        for (const PenPoint& p : stroke_points(old)) full.push_back(TracePoint{p.x, p.y, p.p.value_or(0.7)});
        std::vector<TracePoint> fresh = trace;
        if (s0 > s1) {
            std::swap(s0, s1);
            std::reverse(fresh.begin(), fresh.end());
        }
        std::vector<TracePoint> joined(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(std::min(s0 + 1, full.size())));
        // the trace keeps the line's own pressure where it joins it (its shape changes, not its weight)
        const double weight = full[std::min(s0, full.size() - 1)].p;
        for (const TracePoint& t : fresh) joined.push_back(TracePoint{t.x, t.y, weight});
        if (s1 + 1 < full.size()) joined.insert(joined.end(), full.begin() + static_cast<std::ptrdiff_t>(s1) + 1, full.end());
        auto stroke = std::make_shared<Stroke>(old);
        stroke->points.clear();
        std::vector<double> pressure;
        for (const TracePoint& t : joined) {
            stroke->points.push_back(PointF{py_round(t.x, 3), py_round(t.y, 3)});
            pressure.push_back(py_round(t.p, 3));
        }
        stroke->pressure = old.pressure.empty() ? std::vector<double>() : pressure;
        items[i] = stroke;
        done();
        return;
    }
    // join: the ends near the trace, paired with the nearest other end near it
    const Json* join_given = get(op, "join_mm");
    const double reach = py_max(0.1, join_given != nullptr ? to_float(*join_given) : 5.0);
    struct End {
        StrokePtr stroke;
        bool first;  // which: 0 (true) or -1 (false)
    };
    std::vector<End> ends;
    for (const std::size_t i : touched) {
        const StrokePtr& s = items[i];
        for (const bool first : {true, false}) {
            const PointF& p = first ? s->points.front() : s->points.back();
            if (closest_to(p.x, p.y).distance <= radius) ends.push_back(End{s, first});
        }
    }
    bool joined_any = false;
    std::set<std::string> used;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        const End& a = ends[i];
        if (used.contains(a.stroke->id)) continue;
        const PointF& at = a.first ? a.stroke->points.front() : a.stroke->points.back();
        std::optional<std::pair<double, std::size_t>> pick;  // (distance, the end)
        for (std::size_t j = i + 1; j < ends.size(); ++j) {
            const End& b = ends[j];
            if (b.stroke == a.stroke || used.contains(b.stroke->id)) continue;
            const PointF& bt = b.first ? b.stroke->points.front() : b.stroke->points.back();
            const double d = py_dist(at.x, at.y, bt.x, bt.y);
            if (d <= reach && (!pick || d < pick->first)) pick = std::make_pair(d, j);
        }
        if (!pick) continue;
        const End& b = ends[pick->second];
        std::vector<PointF> pa = a.stroke->points, pb = b.stroke->points;
        std::vector<double> ra = pressure_or_default(*a.stroke), rb = pressure_or_default(*b.stroke);
        if (a.first) {
            std::reverse(pa.begin(), pa.end());
            std::reverse(ra.begin(), ra.end());
        }
        if (!b.first) {
            std::reverse(pb.begin(), pb.end());
            std::reverse(rb.begin(), rb.end());
        }
        auto merged = std::make_shared<Stroke>(*a.stroke);
        merged->points = pa;
        merged->points.insert(merged->points.end(), pb.begin(), pb.end());
        merged->pressure = ra;
        merged->pressure.insert(merged->pressure.end(), rb.begin(), rb.end());
        std::vector<StrokePtr> out;
        for (const StrokePtr& s : items) {
            if (s == b.stroke) continue;
            out.push_back(s == a.stroke ? StrokePtr(merged) : s);
        }
        items = std::move(out);
        used.insert(a.stroke->id);
        used.insert(b.stroke->id);
        joined_any = true;
    }
    if (!joined_any) throw OpError("no two line ends near the trace are close enough to join (join_mm)");
    done();
}

}  // namespace

void register_vector_ops(OpRegistry& registry) {
    registry.add("vector_edit", vector_edit);
    registry.add("trace_edit", trace_edit);
}

}  // namespace genko::core
