// The panel ops (Python's ops._apply_one): split_frame, cut_frame, move_gutter, add_frame, delete_frame, merge_frame,
// resize_frame, set_frame and select_frame, with Python's arguments, defaults, checks and messages. Each changes
// the one page it names (copy-on-write) and, when it moves panels, the lines in them (the story).

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// A dict of panel id → box (Python's dict: a repeated id keeps its first place and its last value).
using Rects = std::vector<std::pair<std::string, Rect>>;

void dict_set(Rects& rects, const std::string& key, const Rect& rect) {
    for (auto& [k, v] : rects) {
        if (k == key) {
            v = rect;
            return;
        }
    }
    rects.emplace_back(key, rect);
}

const Rect* dict_get(const Rects& rects, const std::string& key) {
    for (const auto& [k, v] : rects) {
        if (k == key) return &v;
    }
    return nullptr;
}

// ops._leaf_rects
Rects leaf_rects(const Page& page) {
    Rects out;
    for (const Frame* leaf : page.leaf_frames()) dict_set(out, leaf->id, leaf->rect);
    return out;
}

// page._find(frame_id): the first panel with the id. KeyError (repr of the id) when there is none; Python's
// IndexError when the page has no panels at all.
Frame& find_or_key_error(Page& page, const std::string& frame_id) {
    if (page.frames.empty()) throw PyUncaught("IndexError", "list index out of range");
    Frame* frame = page.find_frame(frame_id);
    if (frame == nullptr) throw OpKeyError(py_repr_str(frame_id));
    return *frame;
}

// ops._placed_on: the placed layers of these panels (their positions in page.layers).
std::vector<std::size_t> placed_on(const Page& page, const std::set<std::string>& frame_ids) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const Layer& layer = page.layers[i];
        if (layer.kind == LayerKind::Placed && layer.frame_id && frame_ids.contains(*layer.frame_id)) out.push_back(i);
    }
    return out;
}

// episode.studio.setdefault("orphans", []).append(record)
void add_orphan(Document& doc, Json record) {
    if (!doc.studio.is_object()) doc.studio = Json::object();
    if (!doc.studio.contains("orphans")) doc.studio["orphans"] = Json::array();
    Json& orphans = doc.studio["orphans"];
    if (!orphans.is_array()) {
        throw PyUncaught("AttributeError", "'" + py_type_name(orphans) + "' object has no attribute 'append'");
    }
    orphans.push_back(std::move(record));
}

// ops._orphan_art: placed art whose panel went away is kept in studio.orphans (asset refs stay alive for gc) and
// taken off the page.
void orphan_art(Document& doc, Page& page, const std::vector<std::size_t>& art, const std::string& reason) {
    if (art.empty()) return;
    Json layers = Json::array();
    for (const std::size_t i : art) layers.push_back(layer_to_dict(page.layers[i]));
    Json record = Json::object();
    record["kind"] = "layers";
    record["page_id"] = page.id;
    record["reason"] = reason;
    record["rev"] = doc.revision;
    record["layers"] = std::move(layers);
    add_orphan(doc, std::move(record));
    std::vector<Layer> kept;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (std::find(art.begin(), art.end(), i) == art.end()) kept.push_back(std::move(page.layers[i]));
    }
    page.layers = std::move(kept);
}

Json panels_record(const Page& page, const std::string& frame_id, Json panels, std::int64_t revision) {
    Json record = Json::object();
    record["kind"] = "panels";
    record["page_id"] = page.id;
    record["frame_id"] = frame_id;
    record["panels"] = std::move(panels);
    record["rev"] = revision;
    return record;
}

// --- _carry_lines -------------------------------------------------------------------------------------------------

// A number of a tail (to, via, vias) in arithmetic with `other`: PyTypeError as Python words it.
Num operand(const Json& value, const Num& other, const char* op) {
    if (value.is_boolean()) return Num(value.get<bool>() ? 1 : 0);
    if (const auto n = Num::from_json(value)) return *n;
    throw PyTypeError(std::string("unsupported operand type(s) for ") + op + ": '" + py_type_name(value) + "' and '" +
                      (other.is_int() ? "int" : "float") + "'");
}

// rect.x <= x <= rect.x + rect.width and rect.y <= y <= rect.y + rect.height (Python's chained comparisons)
bool inside(const Rect& rect, const Json& x, const Json& y) {
    return py_less(rect.x.json(), x, "<=") && py_less(x, (rect.x + rect.width).json(), "<=") &&
           py_less(rect.y.json(), y, "<=") && py_less(y, (rect.y + rect.height).json(), "<=");
}

std::pair<Num, Num> carry(const Num& px, const Num& py, const Rect& old, const Rect& fresh) {
    const Num fx = old.width.truthy() ? (px - old.x) / old.width : Num(0.5);
    const Num fy = old.height.truthy() ? (py - old.y) / old.height : Num(0.5);
    return {fresh.x + fx * fresh.width, fresh.y + fy * fresh.height};
}

// [round(v, 3) for v in carry(point, *spot)] for a point of a tail (any JSON, as Python reads it; a coordinate is
// read only when the panel's old size along it is not zero)
Json carry_json(const Json& point, const Rect& old, const Rect& fresh) {
    const Num fx = old.width.truthy() ? (operand(subscript(point, 0), old.x, "-") - old.x) / old.width : Num(0.5);
    const Num fy = old.height.truthy() ? (operand(subscript(point, 1), old.y, "-") - old.y) / old.height : Num(0.5);
    return Json::array({py_round(fresh.x + fx * fresh.width, 3).json(), py_round(fresh.y + fy * fresh.height, 3).json()});
}

// ops._carry_lines: the lines go where their panels went (their own frame_id, or their middle inside the panel's old
// place), and so do the ends of their tails.
void carry_lines(Document& doc, const Page& page, const Rects& before) {
    const Rects after = leaf_rects(page);
    std::vector<std::pair<std::string, std::pair<Rect, Rect>>> moved;
    for (const auto& [fid, old] : before) {
        const Rect* now = dict_get(after, fid);
        if (now == nullptr) continue;
        const Num change = py_abs(old.x - now->x) + py_abs(old.y - now->y) + py_abs(old.width - now->width) +
                           py_abs(old.height - now->height);
        if (change > Num(1e-6)) moved.emplace_back(fid, std::make_pair(old, *now));
    }
    if (moved.empty()) return;
    const auto find_moved = [&](const std::string& fid) -> const std::pair<Rect, Rect>* {
        for (const auto& [k, v] : moved) {
            if (k == fid) return &v;
        }
        return nullptr;
    };
    const auto spot_of = [&](const Json& x, const Json& y) -> const std::pair<Rect, Rect>* {
        for (const auto& [k, v] : moved) {
            if (inside(v.first, x, y)) return &v;
        }
        return nullptr;
    };
    for (StoryLine& line : doc.story) {
        if (!(line.page_index == page.index)) continue;
        const Num cx = line.x_mm + line.w_mm / Num(2);
        const Num cy = line.y_mm + line.h_mm / Num(2);
        const std::pair<Rect, Rect>* pair = line.frame_id && !line.frame_id->empty() ? find_moved(*line.frame_id) : nullptr;
        if (pair == nullptr) pair = spot_of(cx.json(), cy.json());
        if (pair != nullptr) {
            const Rect& old = pair->first;
            const Rect& fresh = pair->second;
            const auto [nx, ny] = carry(cx, cy, old, fresh);
            // (a smaller panel: the box stays inside)
            const Num w = fresh.width < line.w_mm ? fresh.width : line.w_mm;
            const Num h = fresh.height < line.h_mm ? fresh.height : line.h_mm;
            const auto min_max = [](const Num& v, const Num& lo, const Num& hi) {
                const Num m = lo > v ? lo : v;  // max(v, lo)
                return hi < m ? hi : m;         // min(m, hi)
            };
            line.x_mm = py_round(min_max(nx - w / Num(2), fresh.x, fresh.x + fresh.width - w), 3);
            line.y_mm = py_round(min_max(ny - h / Num(2), fresh.y, fresh.y + fresh.height - h), 3);
            line.w_mm = py_round(w, 3);
            line.h_mm = py_round(h, 3);
        }
        for (Json& tail : line.tails) {
            if (!tail.is_object()) {
                throw PyUncaught("AttributeError", "'" + py_type_name(tail) + "' object has no attribute 'get'");
            }
            const Json* to = get(tail, "to");
            if (to == nullptr || !py_truthy(*to)) continue;
            const Json to_value = *to;
            const std::pair<Rect, Rect>* spot = spot_of(subscript(to_value, 0), subscript(to_value, 1));
            if (spot == nullptr) continue;
            tail["to"] = carry_json(to_value, spot->first, spot->second);
            if (truthy_at(tail, "via")) tail["via"] = carry_json(tail["via"], spot->first, spot->second);
            if (truthy_at(tail, "vias")) {
                Json vias = Json::array();
                for (const Json& bend : iterate(tail["vias"])) vias.push_back(carry_json(bend, spot->first, spot->second));
                tail["vias"] = std::move(vias);
            }
        }
        if (line.tail) {
            const std::pair<Rect, Rect>* spot = spot_of(line.tail->x.json(), line.tail->y.json());
            if (spot != nullptr) {
                const auto [tx, ty] = carry(line.tail->x, line.tail->y, spot->first, spot->second);
                line.tail = Point{py_round(tx, 3), py_round(ty, 3)};
            }
        }
    }
}

// --- the panels in reading order ----------------------------------------------------------------------------------

void leaves_in_reading_order(const Frame& frame, Binding binding, std::vector<const Frame*>& out) {
    if (frame.children.empty()) {
        out.push_back(&frame);
        return;
    }
    std::vector<const Frame*> children;
    for (const Frame& child : frame.children) children.push_back(&child);
    if (frame.split_axis && *frame.split_axis == "free") {
        children = reading_order(std::move(children), binding == Binding::Right);
    } else if (frame.split_axis && *frame.split_axis == "vertical" && binding == Binding::Right) {
        std::reverse(children.begin(), children.end());
    }
    for (const Frame* child : children) leaves_in_reading_order(*child, binding, out);
}

// ops._simplify_outline: Ramer–Douglas–Peucker on a closed freehand outline (the corners stay, the jitter goes).
std::vector<Point> simplify_outline(const std::vector<Point>& points, double tolerance) {
    const auto rdp = [tolerance](const auto& self, const std::vector<Point>& pts) -> std::vector<Point> {
        if (pts.size() < 3) return pts;
        const Point& a = pts.front();
        const Point& b = pts.back();
        double far = 0.0;
        std::size_t index = 0;
        for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
            const double d = distance_to_segment(pts[i].x.value(), pts[i].y.value(), a.x.value(), a.y.value(),
                                                 b.x.value(), b.y.value());
            if (d > far) {
                far = d;
                index = i;
            }
        }
        if (far <= tolerance) return {a, b};
        // (Python recurses for ever on the same points here: a negative tolerance on a straight run)
        if (index == 0) throw PyUncaught("RecursionError", "maximum recursion depth exceeded");
        std::vector<Point> left = self(self, std::vector<Point>(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(index) + 1));
        const std::vector<Point> right = self(self, std::vector<Point>(pts.begin() + static_cast<std::ptrdiff_t>(index), pts.end()));
        left.pop_back();
        left.insert(left.end(), right.begin(), right.end());
        return left;
    };
    std::size_t far_i = 0;
    double far_d = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double d = py_dist(points[0].x.value(), points[0].y.value(), points[i].x.value(), points[i].y.value());
        if (i == 0 || d > far_d) {
            far_i = i;
            far_d = d;
        }
    }
    std::vector<Point> first = rdp(rdp, std::vector<Point>(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(far_i) + 1));
    std::vector<Point> rest(points.begin() + static_cast<std::ptrdiff_t>(far_i), points.end());
    rest.push_back(points[0]);
    std::vector<Point> second = rdp(rdp, rest);
    if (!first.empty()) first.pop_back();
    if (!second.empty()) second.pop_back();
    first.insert(first.end(), second.begin(), second.end());
    std::vector<Point> out;
    for (const Point& p : first) out.push_back(Point{py_round(p.x, 3), py_round(p.y, 3)});
    return out;
}

// [(float(p[0]), float(p[1])) for p in raw]
std::vector<Point> float_points(const Json& raw) {
    std::vector<Point> out;
    for (const Json& p : iterate(raw)) {
        const double x = to_float(subscript(p, 0));
        const double y = to_float(subscript(p, 1));
        out.push_back(Point{Num(x), Num(y)});
    }
    return out;
}

// ops._border_style: a panel border's look.
Json border_style(const Json& raw) {
    static const char* const kKinds[] = {"solid", "double", "dashed", "dotted", "rough"};
    const std::string message = "line kind must be one of solid, double, dashed, dotted, rough";
    if (!raw.is_object()) throw OpError(message);
    const Json* kind_value = get(raw, "kind");
    const std::string kind = kind_value != nullptr && py_truthy(*kind_value) ? py_str(*kind_value) : "solid";
    if (std::find(std::begin(kKinds), std::end(kKinds), kind) == std::end(kKinds)) throw OpError(message);
    Json out = Json::object();
    out["kind"] = kind;
    if (truthy_at(raw, "rgb")) out["rgb"] = ints_json(rgb3(raw["rgb"], "rgb"));
    static const std::tuple<const char*, double, double> kRanges[] = {
        {"gap_mm", 0.1, 10.0}, {"dash_mm", 0.01, 30.0}, {"wobble_mm", 0.0, 3.0}};
    for (const auto& [key, lo, hi] : kRanges) {
        const Json* value = get(raw, key);
        if (value != nullptr && !value->is_null()) out[key] = py_clamp(to_float(*value), lo, hi);
    }
    return out;
}

// --- the ops ----------------------------------------------------------------------------------------------------------

void split_frame(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json* axis_value = get(c.op, "axis");
    const std::string axis = axis_value != nullptr && axis_value->is_string() ? axis_value->get<std::string>() : "";
    if (axis != "horizontal" && axis != "vertical") throw OpError("axis must be horizontal or vertical");
    const auto leaves = doc.page(i).leaf_frames();
    if (leaves.empty()) throw OpError("page has no frames");
    // frame_id = op.get("frame_id") or page.selected_frame_id or leaves[0].id
    Json frame_id;
    if (truthy_at(c.op, "frame_id")) {
        frame_id = c.op["frame_id"];
    } else if (py_truthy(doc.page(i).selected_frame_id)) {
        frame_id = doc.page(i).selected_frame_id;
    } else {
        frame_id = leaves[0]->id;
    }
    Page& page = doc.edit_page(i);
    Frame& target = find_or_key_error(page, py_str(frame_id));
    const std::string target_id = target.id;
    const std::vector<std::size_t> art = placed_on(page, {target_id});
    if (!art.empty() && !truthy_at(c.op, "force")) {
        throw OpError("frame " + target_id + " has placed art; pass force to move it to studio.orphans");
    }
    const Json* ratio_value = get(c.op, "ratio");
    const double ratio = ratio_value != nullptr ? to_float(*ratio_value) : 0.5;
    const Json* gutter_value = get(c.op, "gutter_mm");
    const double gutter = gutter_value != nullptr ? to_float(*gutter_value) : 4.0;
    const Json* tilt_value = get(c.op, "tilt_mm");
    const double tilt = tilt_value != nullptr && py_truthy(*tilt_value) ? to_float(*tilt_value) : 0.0;
    if (!target.children.empty()) throw OpError("can only split a leaf frame");
    Frame* a = nullptr;
    Frame* b = nullptr;
    if (tilt != 0.0 || (target.poly && !target.poly->empty())) {
        const auto [p0, p1] = axis_line(target, axis, ratio, gutter, tilt);
        try {
            std::tie(a, b) = cut_frame(target, p0, p1, gutter);
        } catch (const PyValueError& error) {
            throw OpError(error.what());
        }
    } else {
        // (Python's page.split_frame looks the panel up by the id as it was given, not as a str)
        if (!frame_id.is_string()) throw OpKeyError(py_repr(frame_id));
        std::tie(a, b) = page.split_frame(target_id, axis, Num(ratio), Num(gutter));
        remember_split(target);
    }
    if (target.panel) {
        // the brief stays with the panel read first: top, or the binding-side column
        Frame* first = axis == "horizontal" || doc.binding != Binding::Right ? a : b;
        first->panel = std::move(target.panel);
        target.panel.reset();
    }
    orphan_art(doc, page, art, "split " + target_id);
}

void cut_frame_op(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    Point p0, p1;
    try {
        const auto read = [&](const char* key) {
            const Json value = subscript(c.op, key);
            const double x = to_float(subscript(value, 0));
            const Json again = subscript(c.op, key);
            const double y = to_float(subscript(again, 1));
            return Point{Num(x), Num(y)};
        };
        p0 = read("p0");
        p1 = read("p1");
    } catch (const OpKeyError&) {
        throw OpError("p0 and p1 are [x, y] in mm");
    } catch (const PyTypeError&) {
        throw OpError("p0 and p1 are [x, y] in mm");
    } catch (const PyValueError&) {
        throw OpError("p0 and p1 are [x, y] in mm");
    } catch (const PyUncaught& error) {
        if (error.type() != "IndexError") throw;
        throw OpError("p0 and p1 are [x, y] in mm");
    }
    Json frame_id = c.op.contains("frame_id") ? c.op["frame_id"] : Json(nullptr);
    if (!py_truthy(frame_id)) {  // the panel under the middle of the cut
        const Frame* under = doc.page(i).frame_at((p0.x + p1.x) / Num(2), (p0.y + p1.y) / Num(2));
        if (under == nullptr) throw OpError("frame_id is required");
        frame_id = under->id;
    }
    Page& page = doc.edit_page(i);
    Frame& target = find_or_key_error(page, py_str(frame_id));
    if (!target.children.empty()) throw OpError("can only split a leaf frame");
    if (py_dist(p0.x.value(), p0.y.value(), p1.x.value(), p1.y.value()) < 1) throw OpError("the cut is too short");
    const std::string target_id = target.id;
    const std::vector<std::size_t> art = placed_on(page, {target_id});
    if (!art.empty() && !truthy_at(c.op, "force")) {
        throw OpError("frame " + target_id + " has placed art; pass force to move it to studio.orphans");
    }
    std::optional<Json> panel = target.panel;
    Frame* a = nullptr;
    Frame* b = nullptr;
    try {
        const Json* gutter_value = get(c.op, "gutter_mm");
        const double gutter = gutter_value != nullptr ? to_float(*gutter_value) : 4.0;
        std::tie(a, b) = cut_frame(target, p0, p1, gutter);
    } catch (const PyValueError& error) {
        throw OpError(error.what());
    }
    if (panel) {
        Frame* first = (target.split_axis && *target.split_axis == "horizontal") || doc.binding != Binding::Right ? a : b;
        first->panel = std::move(panel);
        target.panel.reset();
    }
    orphan_art(doc, page, art, "cut " + target_id);
}

void move_gutter_op(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json* id_value = get(c.op, "frame_id");
    const std::string frame_id = id_value != nullptr && py_truthy(*id_value) ? py_str(*id_value) : "";
    Page& page = doc.edit_page(i);
    Frame& node = find_or_key_error(page, frame_id);
    if (node.children.empty()) throw OpError("frame_id must be a split (the parent of the panels on both sides)");
    const Rects before = leaf_rects(page);
    try {
        const Json* index_value = get(c.op, "index");
        const std::int64_t index = index_value != nullptr ? to_int(*index_value) : 0;
        const Json* delta_value = get(c.op, "delta_mm");
        const double delta = delta_value != nullptr ? to_float(*delta_value) : 0.0;
        const Json* gutter_value = get(c.op, "gutter_mm");
        const std::optional<double> gutter =
            gutter_value == nullptr || gutter_value->is_null() ? std::nullopt : std::optional<double>(to_float(*gutter_value));
        move_gutter(node, index, delta, gutter);
    } catch (const PyValueError& error) {
        throw OpError(error.what());
    }
    carry_lines(doc, page, before);
}

void add_frame(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    std::vector<Point> points;
    const Json* rect_value = get(c.op, "rect");
    if (rect_value != nullptr && !rect_value->is_null()) {
        const std::vector<double> v = unpack_floats(*rect_value, 4);
        double x = v[0], y = v[1], w = v[2], h = v[3];
        if (w < 0) {
            x = x + w;
            w = -w;
        }
        if (h < 0) {
            y = y + h;
            h = -h;
        }
        points = corners(Rect{Num(x), Num(y), Num(w), Num(h)});
    } else {
        const Json* raw = get(c.op, "points");
        points = dedupe(float_points(raw != nullptr && py_truthy(*raw) ? *raw : Json::array()));
        if (points.size() > 3) {  // (a freehand outline: only the points that change its shape)
            const Json* tolerance = get(c.op, "tolerance_mm");
            points = simplify_outline(points, tolerance != nullptr ? to_float(*tolerance) : 0.4);
        }
    }
    if (points.size() < 3 || area(points) < 25) {
        throw OpError("a panel needs rect [x, y, w, h] or points around at least 25 mm²");
    }
    const Rect box = bbox(points);
    if ((box.height < box.width ? box.height : box.width) < Num(4)) throw OpError("a panel is at least 4 mm across");
    Page& page = doc.edit_page(i);
    if (page.frames.empty()) throw PyUncaught("IndexError", "list index out of range");
    const auto leaves = page.leaf_frames();
    double border = leaves.empty() ? 0.8 : leaves.front()->border_mm;
    if (!is_free(page.frames[0])) {
        Frame& root = page.frames[0];
        const bool blank = root.children.empty() && !root.panel && placed_on(page, {root.id}).empty() && !root.custom &&
                           !(root.poly && !root.poly->empty());
        Frame free;
        free.id = new_id();
        free.rect = Rect{Num(0.0), Num(0.0), page.spec.width_mm, page.spec.height_mm};
        free.split_axis = std::string(kFreeSplit);
        if (blank) {  // (the page's first drawn panel takes the place of the basic frame, as a new page's is)
            border = root.border_mm;
            for (StoryLine& line : doc.story) {
                if (line.page_index == page.index && line.frame_id && *line.frame_id == root.id) line.frame_id.reset();
            }
        } else {
            free.children.push_back(std::move(root));
        }
        page.frames[0] = std::move(free);
    }
    Frame frame;
    const Json* id_value = get(c.op, "id");
    frame.id = id_value != nullptr && py_truthy(*id_value) ? py_str(*id_value) : new_id();
    frame.rect = box;
    const Json* border_value = get(c.op, "border_mm");
    frame.border_mm = border_value != nullptr ? to_float(*border_value) : border;
    for (const Frame* leaf : page.leaf_frames()) {
        if (leaf->id == frame.id) throw OpError("frame " + frame.id + " exists");
    }
    if (!as_rect(points)) {
        set_shape(frame, points);
        frame.custom = true;
    }
    const std::string new_frame_id = frame.id;
    page.frames[0].children.push_back(std::move(frame));
    page.selected_frame_id = new_frame_id;
}

void delete_frame(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    Json chosen = Json("");
    if (truthy_at(c.op, "frame_id")) {
        chosen = c.op["frame_id"];
    } else if (py_truthy(doc.page(i).selected_frame_id)) {
        chosen = doc.page(i).selected_frame_id;
    }
    const std::string frame_id = py_str(chosen);
    Page& page = doc.edit_page(i);
    if (page.frames.empty()) throw PyUncaught("IndexError", "list index out of range");
    Frame* found = page.find_frame(frame_id);
    if (found == nullptr) throw OpError("no frame " + frame_id);
    if (!found->children.empty()) throw OpError("delete_frame takes one panel (not a split)");
    if (page.leaf_frames().size() <= 1) {
        throw OpError("the page's last panel cannot be deleted (set_frame border_mm 0 hides its border)");
    }
    const std::string id = found->id;
    const std::optional<Json> panel = found->panel;
    const std::vector<std::size_t> art = placed_on(page, {id});
    if (!art.empty() && !truthy_at(c.op, "force")) {
        throw OpError("frame " + id + " has placed art; pass force to move it to studio.orphans");
    }
    Frame* parent = page.parent_of(id);
    if (parent == nullptr) throw OpError("the page's last panel cannot be deleted");
    // (the others keep their places: the gap is left empty, as when a panel is deleted in CLIP STUDIO)
    std::erase_if(parent->children, [&id](const Frame& child) { return child.id == id; });
    parent->split_axis = std::string(kFreeSplit);
    parent->split.reset();
    if (panel && py_truthy(*panel)) add_orphan(doc, panels_record(page, id, Json::array({*panel}), doc.revision));
    for (StoryLine& line : doc.story) {
        if (line.page_index == page.index && line.frame_id && *line.frame_id == id) line.frame_id.reset();
    }
    orphan_art(doc, page, art, "delete " + id);
    if (page.selected_frame_id.is_string() && page.selected_frame_id.get_ref<const std::string&>() == id) {
        page.selected_frame_id = nullptr;
    }
}

void merge_frame_op(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    Json frame_id = c.op.contains("frame_id") ? c.op["frame_id"] : Json(nullptr);
    if (!py_truthy(frame_id)) frame_id = doc.page(i).selected_frame_id;
    if (!py_truthy(frame_id)) throw OpError("frame_id is required");
    const std::string id = py_str(frame_id);
    Page& page = doc.edit_page(i);
    Frame* parent = page.parent_of(id);
    if (parent == nullptr) throw OpError("cannot merge the root frame");
    if (parent->split_axis && *parent->split_axis == "free") {
        throw OpError("drawn panels are not merged: delete_frame one, or reshape it with set_frame poly");
    }
    std::vector<const Frame*> leaves;
    leaves_in_reading_order(*parent, doc.binding, leaves);
    std::set<std::string> ids;
    for (const Frame* leaf : leaves) ids.insert(leaf->id);
    const std::vector<std::size_t> art = placed_on(page, ids);
    if (!art.empty() && !truthy_at(c.op, "force")) {
        throw OpError("panels under " + parent->id + " have placed art; pass force to move it to studio.orphans");
    }
    Json panels = Json::array();
    for (const Frame* leaf : leaves) {
        if (leaf->panel && py_truthy(*leaf->panel)) panels.push_back(*leaf->panel);
    }
    page.merge_frame(id);
    if (!panels.empty()) {
        parent->panel = panels[0];
        if (panels.size() > 1) {
            Json rest(panels.begin() + 1, panels.end());
            add_orphan(doc, panels_record(page, parent->id, std::move(rest), doc.revision));
        }
    }
    orphan_art(doc, page, art, "merge into " + parent->id);
}

void resize_frame_op(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json frame_id = c.op.contains("frame_id") ? c.op["frame_id"] : Json(nullptr);
    const Json* rect_value = get(c.op, "rect");
    const Json rect_raw = rect_value != nullptr && py_truthy(*rect_value) ? *rect_value : Json::object();
    if (!py_truthy(frame_id)) throw OpError("frame_id is required");
    const Rects before = leaf_rects(doc.page(i));
    const std::string id = py_str(frame_id);
    const double x = to_float(subscript(rect_raw, "x"));
    const double y = to_float(subscript(rect_raw, "y"));
    const double w = to_float(subscript(rect_raw, "width"));
    const double h = to_float(subscript(rect_raw, "height"));
    Page& page = doc.edit_page(i);
    Frame& target = find_or_key_error(page, id);
    if (!target.children.empty()) throw PyValueError("can only resize a leaf frame");
    target.rect = Rect{Num(x), Num(y), Num(w), Num(h)};
    carry_lines(doc, page, before);
}

void set_frame(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json frame_id = c.op.contains("frame_id") ? c.op["frame_id"] : Json(nullptr);
    if (!py_truthy(frame_id)) throw OpError("frame_id is required");
    Page& page = doc.edit_page(i);
    Frame& frame = find_or_key_error(page, py_str(frame_id));
    const Json& op = c.op;
    if (has(op, "bleed")) frame.bleed = py_truthy(op["bleed"]);
    if (has(op, "clip")) frame.clip = py_truthy(op["clip"]);
    if (has(op, "border_mm")) frame.border_mm = to_float(op["border_mm"]);
    if (has(op, "line")) {
        if (py_truthy(op["line"])) {
            frame.line = border_style(op["line"]);
        } else {
            frame.line.reset();
        }
    }
    if (has(op, "corner_mm")) {
        if (!frame.children.empty()) throw OpError("only a panel (not a split) takes round corners");
        frame.corner_mm = py_clamp(py_truthy(op["corner_mm"]) ? to_float(op["corner_mm"]) : 0.0, 0.0, 50.0);
    }
    if (has(op, "poly")) {
        if (!frame.children.empty()) throw OpError("only a panel (not a split) takes a shape");
        if (py_truthy(op["poly"])) {
            const std::vector<Point> points = float_points(op["poly"]);
            if (points.size() < 3 || area(points) < 4) throw OpError("a shape needs at least three corners around some area");
            set_shape(frame, points);
            frame.custom = true;
        } else {
            frame.custom = false;
            if (Frame* parent = page.parent_of(frame.id)) relayout(*parent);
        }
    }
    if (has(op, "curves") || has(op, "bow")) {
        if (!frame.children.empty()) throw OpError("only a panel (not a split) takes a shape");
        const std::vector<Point> corner_points = shape(frame);
        const std::size_t n = corner_points.size();
        std::optional<std::vector<double>> curves;
        if (has(op, "curves")) {
            if (py_truthy(op["curves"])) {
                std::vector<double> values;
                for (const Json& v : iterate(op["curves"])) values.push_back(to_float(v));
                curves = std::move(values);
            }
        } else {
            const Json bow = op["bow"].is_object() ? op["bow"] : Json::object();
            std::vector<double> values = frame.curves && !frame.curves->empty() && frame.curves->size() == n
                                             ? *frame.curves
                                             : std::vector<double>(n, 0.0);
            const Json* edge_value = get(bow, "edge");
            const std::int64_t edge = edge_value != nullptr ? to_int(*edge_value) : -1;
            if (!(0 <= edge && edge < static_cast<std::int64_t>(n))) {
                throw OpError("edge must be 0.." + std::to_string(static_cast<std::int64_t>(n) - 1));
            }
            const Json* mm = get(bow, "mm");
            values[static_cast<std::size_t>(edge)] = mm != nullptr ? to_float(*mm) : 0.0;
            curves = std::move(values);
        }
        if (curves) {
            if (curves->size() != n) throw OpError("curves needs one number per edge (" + std::to_string(n) + ")");
            double longest = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                const Point& a = corner_points[k];
                const Point& b = corner_points[(k + 1) % n];
                const double d = py_dist(a.x.value(), a.y.value(), b.x.value(), b.y.value());
                if (k == 0 || d > longest) longest = d;
            }
            for (const double v : *curves) {
                if (std::fabs(v) > longest / 2) throw OpError("an edge cannot bow more than half its length");
            }
            const bool any = std::any_of(curves->begin(), curves->end(), [](double v) { return std::fabs(v) > 1e-6; });
            if (any) {
                for (double& v : *curves) v = py_round(v, 3);
            } else {
                curves.reset();
            }
        }
        frame.curves = curves;
        if (curves && !curves->empty()) frame.custom = true;
    }
}

void select_frame(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    const Json frame_id = c.op.contains("frame_id") ? c.op["frame_id"] : Json(nullptr);
    // (Python keeps whatever it is given, a panel of the page or not; refused here)
    const Page& page = c.doc.page(i);
    if (!frame_id.is_string() || page.find_frame(frame_id.get_ref<const std::string&>()) == nullptr) {
        throw OpError("no frame " + py_str(frame_id) + " on page " + page.index.repr());
    }
    c.doc.edit_page(i).selected_frame_id = frame_id;
}

}  // namespace

void register_frame_ops(OpRegistry& registry) {
    registry.add("split_frame", split_frame);
    registry.add("cut_frame", cut_frame_op);
    registry.add("move_gutter", move_gutter_op);
    registry.add("add_frame", add_frame);
    registry.add("delete_frame", delete_frame);
    registry.add("merge_frame", merge_frame_op);
    registry.add("resize_frame", resize_frame_op);
    registry.add("set_frame", set_frame);
    registry.add("select_frame", select_frame);
}

}  // namespace genko::core
