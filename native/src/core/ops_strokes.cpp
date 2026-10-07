// The pen line ops (Python's ops._apply_one): add_stroke, delete_stroke, edit_stroke, simplify_stroke, erase and
// erase_raster. Lines stay vectors; the parts that work on pixels (mixing the colour under a line, erasing a paint
// layer's picture) come with the canvas and are refused here with not_yet_ported.

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/brushes.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/rulers.hpp"
#include "core/stroke_tools.hpp"

namespace genko::core {

namespace {

const char* const kOutsidePanels =
    "この線はどのコマにも入っていないので、コマの形で切られて見えません"
    "（コマの外に描くなら、そのレイヤーを set_layer panel_clip:false にする）";

// The layer's strokes with one more (the others shared).
StrokeListPtr with_stroke(const StrokeListPtr& strokes, Stroke stroke) {
    std::vector<StrokePtr> items = strokes->items;
    items.push_back(std::make_shared<const Stroke>(std::move(stroke)));
    return make_strokes(std::move(items));
}

// ops._shift
PenPoints shift(const PenPoints& points, const Num& dx) {
    PenPoints out;
    for (const PenPoint& p : points) out.push_back(PenPoint{(Num(p.x) + dx).value(), p.y, p.p});
    return out;
}

// ops._stroke_target: which page of a spread a stroke belongs to (and its points in that page's coordinates).
std::pair<std::size_t, PenPoints> stroke_target(const Document& doc, std::size_t at, const PenPoints& points,
                                                const std::string& space) {
    const Page& page = doc.page(at);
    const Num step = page.spread_step_mm();  // the finished sizes meet at the gutter
    const Rect trim = page.trim_rect_mm();
    const Num gutter = trim.x + trim.width;  // the gutter, in the left page's coordinates
    double max_x = points.front().x, min_x = points.front().x;
    for (const PenPoint& p : points) {
        if (p.x > max_x) max_x = p.x;
        if (p.x < min_x) min_x = p.x;
    }
    std::optional<std::size_t> other;
    if (page.spread_with && page.spread_with->truthy()) {
        for (std::size_t k = 0; k < doc.pages.size(); ++k) {
            if (doc.pages[k]->index == *page.spread_with) {
                other = k;
                break;
            }
        }
    }
    const std::string start_side = doc.start_side.value_or("");
    if (space == "spread") {
        if (!other) throw OpError("space spread needs a page with spread_with");
        const bool left_page = page.side(start_side) == "left";
        const std::size_t left = left_page ? at : *other;
        const std::size_t right = left_page ? *other : at;
        if (Num(max_x) < gutter) return {left, points};
        if (Num(min_x) >= gutter) return {right, shift(points, -step)};
        throw OpError("a stroke cannot cross the gutter between spread pages");
    }
    if (space != "page") throw OpError("space must be page or spread");
    if (other && page.side(start_side) == "left" && Num(min_x) >= gutter) return {*other, shift(points, -step)};
    if (other && page.side(start_side) == "right" && Num(max_x) < trim.x) return {*other, shift(points, step)};
    return {at, points};
}

// ops._frame_contains(page)
rulers::FrameContains frame_contains_for(const Page& page) {
    return [&page](const Json& frame_id, double x, double y) {
        if (page.frames.empty() || !frame_id.is_string()) return false;
        const Frame* frame = page.find_frame(frame_id.get_ref<const std::string&>());
        return frame != nullptr && contains(*frame, Num(x), Num(y));
    };
}

// ops._snap_points: the old single perspective ruler (page.ruler): the line's end on the line from its start toward
// the vanishing point.
PenPoints snap_points_old(const Page& page, const PenPoints& points) {
    const Json ruler = page.ruler && py_truthy(*page.ruler) ? *page.ruler : Json::object();
    if (!ruler.is_object()) {
        throw PyUncaught("AttributeError", "'" + py_type_name(ruler) + "' object has no attribute 'get'");
    }
    const Json* vps_value = get(ruler, "points");
    if (vps_value == nullptr || !py_truthy(*vps_value)) return points;
    const Json& vps = *vps_value;
    const double vx = to_float(subscript(subscript(vps, 0), 0));
    const double vy = to_float(subscript(subscript(vps, 0), 1));
    const double x0 = points.front().x, y0 = points.front().y;
    const double x1 = points.back().x, y1 = points.back().y;
    const double dx = vx - x0, dy = vy - y0;
    const double denom = dx * dx + dy * dy;
    if (denom < 1e-6) return points;
    const double t = ((x1 - x0) * dx + (y1 - y0) * dy) / denom;
    PenPoints out = points;
    out.back() = PenPoint{x0 + t * dx, y0 + t * dy, points.back().p};
    return out;
}

// ops._snap_ends (ベクター吸着): each end within `reach` of a line already on the layer moves onto it (an end of it
// first, when one is near enough).
PenPoints snap_ends(Page& page, const Json& op, const PenPoints& points, double reach) {
    const Layer* layer = nullptr;
    if (truthy_at(op, "layer_id")) {
        const std::string id = py_str(op["layer_id"]);
        for (const Layer& item : page.layers) {
            if (item.id == id) {
                layer = &item;
                break;
            }
        }
        if (layer == nullptr) return points;
    } else {
        const std::string name = op.contains("layer") ? py_str(op["layer"]) : std::string("name");
        layer = &page.layers[layer_for_role(page, name == "ink" ? LayerRole::Ink : LayerRole::Name)];
    }
    std::vector<const std::vector<PointF>*> others;
    for (const StrokePtr& s : layer->strokes->items) {
        if (s->points.size() >= 2) others.push_back(&s->points);
    }
    if (others.empty() || points.size() < 2) return points;
    PenPoints out = points;
    const auto target = [&](double x, double y) -> std::optional<PointF> {
        // an end within reach first (shapes close end to end), else the nearest point along a line
        std::optional<PointF> best_end;
        double best_end_d = 0.0;
        for (const auto* line : others) {
            for (const PointF& end : {line->front(), line->back()}) {
                const double d = py_dist(end.x, end.y, x, y);
                if (d <= reach && (!best_end || d < best_end_d)) {
                    best_end = end;
                    best_end_d = d;
                }
            }
        }
        if (best_end) return best_end;
        std::optional<PointF> best;
        double best_d = 0.0;
        for (const auto* line : others) {
            const NearestSegment near = nearest_segment(*line, x, y);
            if (near.distance <= reach && (!best || near.distance < best_d)) {
                best = PointF{near.x, near.y};
                best_d = near.distance;
            }
        }
        return best;
    };
    for (const std::size_t k : {std::size_t{0}, out.size() - 1}) {
        if (const auto found = target(out[k].x, out[k].y)) {
            out[k].x = found->x;
            out[k].y = found->y;
        }
    }
    return out;
}

// ops._in_a_panel: whether any point of the line (within `pad`) lies in a panel that cuts the layers (a page with no
// such panel cuts nothing).
bool in_a_panel(const Page& page, const std::vector<PointF>& points_in, double pad) {
    std::vector<const Frame*> leaves;
    for (const Frame* frame : page.leaf_frames()) {
        if (frame->clip) leaves.push_back(frame);
    }
    if (leaves.empty()) return true;
    std::vector<PointF> points;
    if (!points_in.empty()) points.push_back(points_in.front());
    for (std::size_t i = 0; i + 1 < points_in.size(); ++i) {  // (along the line every millimetre)
        const double x0 = points_in[i].x, y0 = points_in[i].y, x1 = points_in[i + 1].x, y1 = points_in[i + 1].y;
        const std::int64_t steps = std::max<std::int64_t>(1, loop_count(py_hypot(x1 - x0, y1 - y0)));
        for (std::int64_t k = 1; k <= steps; ++k) {
            const double kd = static_cast<double>(k), sd = static_cast<double>(steps);
            points.push_back(PointF{x0 + (x1 - x0) * kd / sd, y0 + (y1 - y0) * kd / sd});
        }
    }
    for (const Frame* frame : leaves) {
        if (const auto shape = bleed_poly(page, frame)) {  // (a slanted bleed panel reaches out to the bleed)
            for (const PointF& p : points) {
                if (in_poly(*shape, p.x, p.y)) return true;
            }
            continue;
        }
        const bool own_box = frame->bleed && !(frame->poly && !frame->poly->empty());
        const Rect box = own_box ? clip_box(page, frame, "bleed") : frame->rect;
        for (const PointF& p : points) {
            const Num x(p.x), y(p.y), margin(pad);
            if (!(box.x - margin <= x && x <= box.x + box.width + margin && box.y - margin <= y &&
                  y <= box.y + box.height + margin)) {
                continue;
            }
            if (own_box || contains(*frame, x, y)) return true;
            if (pad != 0.0) {
                const double offsets[4][2] = {{pad, 0}, {-pad, 0}, {0, pad}, {0, -pad}};
                for (const auto& d : offsets) {
                    if (contains(*frame, Num(p.x + d[0]), Num(p.y + d[1]))) return true;
                }
            }
        }
    }
    return false;
}

void add_stroke(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    std::size_t at = require_page(doc, op);
    const std::string layer_name = op.contains("layer") ? py_str(op["layer"]) : std::string("name");
    const Json* raw = get(op, "points");
    PenPoints points = parse_points(raw != nullptr && py_truthy(*raw) ? *raw : Json::array());
    if (points.size() < 2) throw OpError("points needs at least two [x_mm, y_mm] pairs");
    const Json* space_value = get(op, "space");
    std::tie(at, points) =
        stroke_target(doc, at, points, space_value != nullptr && py_truthy(*space_value) ? py_str(*space_value) : "page");
    Page& page = doc.edit_page(at);
    const Json stabilize = op.contains("stabilize") ? op["stabilize"] : Json(doc.brush_stabilize);
    if (py_truthy(stabilize)) points = stabilize_points(points, to_int(stabilize), truthy_at(op, "stabilize_speed"));
    // (the brush's own after-smoothing: its kind is checked here, even when post_smooth is given)
    const Json kind_value = truthy_at(op, "kind") ? op["kind"] : Json("gpen");
    const std::string kind = brush_kind(kind_value, doc);
    const Json post_smooth = op.contains("post_smooth") ? op["post_smooth"] : Json(brush_for(Json(kind), doc).post_smooth);
    if (py_truthy(post_smooth)) points = smoothed(points, to_int(post_smooth));
    if (truthy_at(op, "post_fit")) points = fit_curve(points, py_clamp(to_float(op["post_fit"]), 0.05, 3.0));
    std::vector<PenPoints> copies;
    if (truthy_at(op, "snap_ruler") || truthy_at(op, "ruler_id")) {
        if (py_truthy(page.rulers)) {
            const rulers::FrameContains inside = frame_contains_for(page);
            const Json* layer_id = get(op, "layer_id");
            const Json* lid = layer_id != nullptr && !layer_id->is_null() ? layer_id : nullptr;
            const Json only = op.contains("ruler_id") ? op["ruler_id"] : Json(nullptr);
            points = rulers::snap(points, page.rulers, inside, only, lid);
            copies = rulers::symmetry_copies(points, page.rulers, inside, lid);
        } else {
            points = snap_points_old(page, points);
        }
    }
    if (truthy_at(op, "snap_lines_mm")) points = snap_ends(page, op, points, py_clamp(to_float(op["snap_lines_mm"]), 0.1, 10.0));
    const bool taper = op.contains("taper") ? py_truthy(op["taper"]) : doc.brush_taper;
    if (taper) {
        std::optional<double> in_mm, out_mm;
        bool any = false;
        for (const char* key : {"taper_in_mm", "taper_out_mm"}) {
            const Json* value = get(op, key);
            if (value == nullptr || value->is_null()) continue;
            const double v = py_clamp(to_float(*value), 0.0, 80.0);
            (std::string_view(key) == "taper_in_mm" ? in_mm : out_mm) = v;
            any = true;
        }
        if (any) {
            if (!in_mm) in_mm = 0.0;
            if (!out_mm) out_mm = 0.0;
        }
        points = taper_points(points, in_mm, out_mm);
    }
    if (truthy_at(op, "pressure_gamma")) {
        const double gamma = py_clamp(to_float(op["pressure_gamma"]), 0.2, 5.0);
        for (PenPoint& p : points) {
            if (p.p) p.p = py_pow(py_clamp(*p.p, 0.0, 1.0), gamma);
        }
    }
    const Json* curve_value = get(op, "curve");
    std::string curve = curve_value != nullptr && py_truthy(*curve_value) ? py_str(*curve_value) : doc.brush_curve;
    if (curve.empty()) curve = "linear";
    if (curve != "linear") points = apply_pressure_curve(points, curve);
    Stroke stroke = coerce_stroke(points);
    // a micrometre is finer than any pen; fewer digits keep a thick book quick to save and open
    for (PointF& p : stroke.points) p = PointF{py_round(p.x, 3), py_round(p.y, 3)};
    for (double& v : stroke.pressure) v = py_round(v, 3);
    stroke.kind = brush_kind(kind_value, doc);
    const Json* width = get(op, "width_mm");
    stroke.width_mm = width != nullptr && !width->is_null() ? to_float(*width) : doc.brush_width_mm;
    if (truthy_at(op, "pressure_opacity")) stroke.pressure_opacity = py_round(py_clamp(to_float(op["pressure_opacity"]), 0.0, 1.0), 3);
    if (truthy_at(op, "rotation")) {  // the pen's barrel turn, along the line as drawn
        std::vector<double> values;
        for (const Json& v : iterate(op["rotation"])) values.push_back(to_float(v));
        stroke.rotation = resampled(values, static_cast<std::int64_t>(stroke.points.size()));
    }
    std::size_t li = 0;
    if (truthy_at(op, "layer_id")) {
        li = layer_by_id(page, py_str(op["layer_id"]));
        const LayerKind k = page.layers[li].kind;
        if (k != LayerKind::Strokes && k != LayerKind::Raster && k != LayerKind::Tone) {
            throw OpError("this layer cannot take pen lines (choose a pen, paint or tone layer)");
        }
    } else {
        li = layer_for_role(page, stroke_role(layer_name));
    }
    Layer& target = page.layers[li];
    if (target.locked) throw OpError("the layer is locked");
    if (target.role == LayerRole::Ink && target.exportable && !page.name_ok && gated(doc)) {
        throw OpError("ink strokes require name_ok");
    }
    const Json* rgb_value = get(op, "rgb");
    if (rgb_value != nullptr && py_truthy(*rgb_value)) {
        stroke.rgb = int_tuple(*rgb_value);
    } else if (doc.brush_rgb != std::vector<std::int64_t>{20, 20, 20} && !doc.brush_rgb.empty()) {
        stroke.rgb = doc.brush_rgb;
    } else {
        stroke.rgb.reset();
    }
    const Json* opacity = get(op, "opacity");
    if (opacity != nullptr && !opacity->is_null()) stroke.opacity = py_clamp(to_float(*opacity), 0.0, 1.0);
    const Brush brush = brush_for(Json(stroke.kind), doc);
    const Json mix = op.contains("mix") ? op["mix"] : Json(brush.mix);
    if (py_truthy(mix) && to_float(mix) > 0 &&
        (target.stroke_count() > 0 || !target.patches.empty() || (target.raster_png && !target.raster_png->empty()))) {
        not_yet_ported("add_stroke with mix (下地混色) on a layer that has something on it: the C++ colour mixing comes "
                       "with the canvas (M2)");
    }
    const std::vector<PointF> drawn = stroke.points;
    const double pad = stroke.width_mm / 2;
    std::vector<StrokePtr> items = target.strokes->items;
    items.push_back(std::make_shared<const Stroke>(stroke));
    for (const PenPoints& copy_points : copies) {  // symmetry rulers draw the line again
        Stroke twin = stroke;
        twin.id = new_id();
        twin.points.clear();
        for (const PenPoint& p : copy_points) twin.points.push_back(PointF{p.x, p.y});
        items.push_back(std::make_shared<const Stroke>(std::move(twin)));
    }
    target.strokes = make_strokes(std::move(items));
    if (target.panel_clip && !in_a_panel(page, drawn, pad)) {
        Json report = Json::object();
        report["warning"] = "outside_panels";
        report["message"] = kOutsidePanels;
        c.report = std::move(report);
    }
}

// ops._strokes_of: the strokes of the page's layer with the role (made when there is none).
std::size_t strokes_layer(Page& page, const std::string& layer_name) { return layer_for_role(page, stroke_role(layer_name)); }

std::string layer_name_of(const Json& op) { return op.contains("layer") ? py_str(op["layer"]) : std::string("name"); }

void delete_stroke(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    const std::string layer_name = layer_name_of(c.op);
    std::int64_t index = 0;
    try {
        index = to_int(subscript(c.op, "index"));
    } catch (const OpKeyError&) {
        throw OpError("index is required");
    } catch (const PyTypeError&) {
        throw OpError("index is required");
    } catch (const PyValueError&) {
        throw OpError("index is required");
    }
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[strokes_layer(page, layer_name)];
    const auto count = static_cast<std::int64_t>(layer.stroke_count());
    if (index < 0 || index >= count) throw OpError("stroke index out of range");
    std::vector<StrokePtr> items = layer.strokes->items;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    layer.strokes = make_strokes(std::move(items));
}

void edit_stroke(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[strokes_layer(page, layer_name_of(c.op))];
    const std::int64_t index = to_int(subscript(c.op, "index"));
    if (index < 0 || index >= static_cast<std::int64_t>(layer.stroke_count())) throw OpError("stroke index out of range");
    const Json* raw = get(c.op, "points");
    const PenPoints points = parse_points(raw != nullptr && py_truthy(*raw) ? *raw : Json::array());
    if (points.size() < 2) throw OpError("points needs at least two [x_mm, y_mm] pairs");
    std::vector<StrokePtr> items = layer.strokes->items;
    Stroke edited=coerce_stroke(points);
    const auto& old=*items[static_cast<std::size_t>(index)];
    if(old.color_rgb) {edited.id=old.id;edited.color_rgb=old.color_rgb;edited.rgb=old.rgb;edited.width_mm=old.width_mm;edited.kind=old.kind;edited.opacity=old.opacity;edited.pressure_opacity=old.pressure_opacity;}
    items[static_cast<std::size_t>(index)] = std::make_shared<const Stroke>(std::move(edited));
    layer.strokes = make_strokes(std::move(items));
}

void simplify_stroke(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[strokes_layer(page, layer_name_of(c.op))];
    const std::int64_t index = to_int(subscript(c.op, "index"));
    if (index < 0 || index >= static_cast<std::int64_t>(layer.stroke_count())) throw OpError("stroke index out of range");
    const PenPoints raw = stroke_points(*layer.strokes->items[static_cast<std::size_t>(index)]);
    const Json* epsilon = get(c.op, "epsilon_mm");
    const PenPoints simplified = rdp(raw, epsilon != nullptr ? to_float(*epsilon) : 0.8);
    std::vector<StrokePtr> items = layer.strokes->items;
    Stroke edited=coerce_stroke(simplified);
    const auto& old=*items[static_cast<std::size_t>(index)];
    if(old.color_rgb) {edited.id=old.id;edited.color_rgb=old.color_rgb;edited.rgb=old.rgb;edited.width_mm=old.width_mm;edited.kind=old.kind;edited.opacity=old.opacity;edited.pressure_opacity=old.pressure_opacity;}
    items[static_cast<std::size_t>(index)] = std::make_shared<const Stroke>(std::move(edited));
    layer.strokes = make_strokes(std::move(items));
}

// erase and erase_raster (one op in Python): pen lines are cut where the eraser went (or up to their crossings, or
// whole); a tone is scraped; a paint layer's pixels are the canvas's (not_yet_ported).
void erase(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string name = py_str(op["op"]);
    const std::size_t at = require_page(doc, op);
    Page& page = doc.edit_page(at);
    std::size_t li = 0;
    if (truthy_at(op, "layer_id")) {
        li = layer_by_id(page, py_str(op["layer_id"]));
    } else {
        const Json* layer = get(op, "layer");
        li = layer_for_role(page, role_from(Json(layer != nullptr && py_truthy(*layer) ? py_str(*layer) : "ink")));
    }
    if (page.layers[li].locked) throw OpError("the layer is locked");
    if (page.layers[li].color_raster)
        not_yet_ported(name + " on high-precision raster pixels is not supported yet");
    const Json* raw = get(op, "points");
    PenPoints points = parse_points(raw != nullptr && py_truthy(*raw) ? *raw : Json::array());
    const Json* width_value = get(op, "width_mm");
    const double width = width_value != nullptr ? to_float(*width_value) : 2.0;
    if (truthy_at(op, "snap_ruler") && py_truthy(page.rulers)) {  // スナップ消しゴム: the eraser runs along the ruler
        const Json* layer_id = get(op, "layer_id");
        const std::string lid = layer_id != nullptr && py_truthy(*layer_id) ? py_str(*layer_id) : "";
        const Json lid_json(lid);
        const Json only = op.contains("ruler_id") ? op["ruler_id"] : Json(nullptr);
        points = rulers::snap(points, page.rulers, frame_contains_for(page), only, lid.empty() ? nullptr : &lid_json);
    }
    const Json* texture_value = get(op, "texture");
    const std::string texture = texture_value != nullptr && py_truthy(*texture_value) ? py_str(*texture_value) : "";
    if (texture != "" && texture != "hard" && texture != "soft" && texture != "rough") {
        throw OpError("texture must be hard, soft or rough");
    }
    Layer& target = page.layers[li];
    if (target.kind == LayerKind::Tone) {  // on a tone the eraser scrapes (削り); soft fades it out
        PenPoints scrape_points = points;
        if (points.size() <= 1) {
            if (points.empty()) throw PyUncaught("IndexError", "list index out of range");
            scrape_points.push_back(PenPoint{points[0].x + 0.01, points[0].y + 0.01, std::nullopt});
        }
        Stroke scrape = coerce_stroke(scrape_points);
        scrape.kind = truthy_at(op, "soft") ? "scrape_soft" : "scrape";
        scrape.width_mm = width;
        target.strokes = with_stroke(target.strokes, std::move(scrape));
        return;
    }
    const Json mode = op.contains("mode") ? op["mode"] : Json(nullptr);
    if (mode == Json("to_crossing")) {
        target.strokes = make_strokes(erase_to_crossing(target.strokes->items, points, width / 2));
        return;
    }
    if (mode == Json("whole")) {  // 線全体: every line the eraser touches goes, whole
        std::vector<StrokePtr> kept;
        for (const StrokePtr& stroke : target.strokes->items) {
            const auto pieces = split_by_eraser(stroke_points(*stroke), points, width / 2);
            const bool touched = !(pieces.size() == 1 && pieces[0].size() >= stroke->points.size() &&
                                   untouched(*stroke, points, width / 2));
            if (!touched) kept.push_back(stroke);
        }
        target.strokes = make_strokes(std::move(kept));
        return;
    }
    if (!(mode.is_null() || mode == Json("") || mode == Json("cut"))) throw OpError("mode must be cut, to_crossing or whole");
    if (target.stroke_count() > 0) {
        std::vector<StrokePtr> kept;
        for (const StrokePtr& stroke : target.strokes->items) {
            const auto pieces = split_by_eraser(stroke_points(*stroke), points, width / 2);
            if (pieces.size() == 1 && pieces[0].size() >= stroke->points.size() && untouched(*stroke, points, width / 2)) {
                kept.push_back(stroke);
                continue;
            }
            for (const PenPoints& piece : pieces) {
                Stroke part = coerce_stroke(piece);
                part.width_mm = stroke->width_mm;
                part.kind = stroke->kind;
                part.rgb = stroke->rgb;
                part.color_rgb = stroke->color_rgb;
                part.opacity = stroke->opacity;
                kept.push_back(std::make_shared<const Stroke>(std::move(part)));
            }
        }
        target.strokes = make_strokes(std::move(kept));
    }
    const bool raster = target.raster_png && !target.raster_png->empty();
    if ((target.kind == LayerKind::Raster && !target.patches.empty() && !raster) || raster) {
        not_yet_ported(name + " on a raster layer: the C++ raster tools come with the canvas (M2)");
    }
}

}  // namespace

void register_stroke_ops(OpRegistry& registry) {
    registry.add("add_stroke", add_stroke);
    registry.add("delete_stroke", delete_stroke);
    registry.add("edit_stroke", edit_stroke);
    registry.add("simplify_stroke", simplify_stroke);
    registry.add("erase", erase);
    registry.add("erase_raster", erase);
}

}  // namespace genko::core
