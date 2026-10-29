#include "core/page_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/pyops.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/smart.hpp"
#include "core/type_text.hpp"

// The warps: PhotoCraft v0.2.0 crates/geom/src/warp.rs (1-620), MIT: native/third_party/photocraft/NOTICE.md.
// The turns' maps: crates/engine/src/canvas_geom.rs (200-235).

namespace genko::core::page_geometry {

namespace {

constexpr double kEps = 1e-9;
constexpr double kPi = std::numbers::pi;

double round3(double v) { return py_round(v, 3); }

XY xy_of(const Json& value) {
    if (!value.is_array() || value.size() < 2 || !value[0].is_number() || !value[1].is_number()) {
        throw OpError("a point must be [x, y]");
    }
    return XY{value[0].get<double>(), value[1].get<double>()};
}

Json xy_json(const XY& p) { return Json::array({round3(p.x), round3(p.y)}); }

// The value at `key` moved when it is [x, y]; a list of them, each moved.
void move_point_at(Json& object, std::string_view key, const Mapping& m) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || it->is_null()) return;
    *it = xy_json(m.apply(xy_of(*it)));
}

void move_points_at(Json& object, std::string_view key, const Mapping& m) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->is_array()) return;
    for (Json& p : *it) p = xy_json(m.apply(xy_of(p)));
}

Rect rect_of(const std::array<double, 4>& b) { return Rect{Num(b[0]), Num(b[1]), Num(b[2]), Num(b[3])}; }

std::array<double, 4> box_of(const std::vector<XY>& pts) {
    double x0 = pts[0].x, y0 = pts[0].y, x1 = pts[0].x, y1 = pts[0].y;
    for (const XY& p : pts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return {round3(x0), round3(y0), round3(x1 - x0), round3(y1 - y0)};
}

// A split's cut point, [u, v] in its panel's box, through the map into the new box.
Json map_norm(const Json& q, const Rect& old_box, const Rect& new_box, const Mapping& m) {
    const XY u = xy_of(q);
    const XY page{old_box.x.value() + u.x * old_box.width.value(), old_box.y.value() + u.y * old_box.height.value()};
    const XY to = m.apply(page);
    const double w = new_box.width.truthy() ? new_box.width.value() : 1.0;
    const double h = new_box.height.truthy() ? new_box.height.value() : 1.0;
    return Json::array({py_round((to.x - new_box.x.value()) / w, 6), py_round((to.y - new_box.y.value()) / h, 6)});
}

bool near(const XY& a, const XY& b) { return std::fabs(a.x - b.x) < 1e-6 && std::fabs(a.y - b.y) < 1e-6; }

}  // namespace

std::optional<Turn> turn_from(std::string_view text) {
    if (text == "90cw") return Turn::Cw90;
    if (text == "90ccw") return Turn::Ccw90;
    if (text == "180") return Turn::Rotate180;
    if (text == "flip_h") return Turn::FlipH;
    if (text == "flip_v") return Turn::FlipV;
    return std::nullopt;
}

Affine turn_affine(Turn turn, double w, double h) {
    switch (turn) {
        case Turn::FlipH: return {-1.0, 0.0, 0.0, 1.0, w, 0.0};
        case Turn::FlipV: return {1.0, 0.0, 0.0, -1.0, 0.0, h};
        case Turn::Rotate180: return {-1.0, 0.0, 0.0, -1.0, w, h};
        case Turn::Cw90: return {0.0, 1.0, -1.0, 0.0, h, 0.0};
        case Turn::Ccw90: return {0.0, -1.0, 1.0, 0.0, 0.0, w};
    }
    return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
}

double Mapping::scale() const { return std::sqrt(std::fabs(det())); }
bool Mapping::keeps_axes() const { return std::fabs(m[1]) < kEps && std::fabs(m[2]) < kEps; }
bool Mapping::swaps_axes() const { return std::fabs(m[0]) < kEps && std::fabs(m[3]) < kEps; }

double Mapping::angle_deg() const {
    // (the direction x goes to; a mirror is taken as x flipped first, then turned)
    const double sx = reflects() ? -1.0 : 1.0;
    const double a = std::atan2(m[1] * sx, m[0] * sx) * 180.0 / kPi;
    const double r = std::round(a);
    return std::fabs(a - r) < 1e-9 ? r : a;
}

bool Mapping::is_similarity_upright() const {
    return keeps_axes() && m[0] > 0 && m[3] > 0 && std::fabs(m[0] - m[3]) < 1e-12;
}

Mapping mapping(const Affine& m) { return Mapping{m}; }
Mapping turn_mapping(Turn turn, double w, double h) { return Mapping{turn_affine(turn, w, h)}; }

Mapping then(const Mapping& first, const Mapping& second) {
    const Affine& a = first.m;
    const Affine& b = second.m;
    // (second ∘ first)
    return Mapping{{b[0] * a[0] + b[2] * a[1], b[1] * a[0] + b[3] * a[1], b[0] * a[2] + b[2] * a[3], b[1] * a[2] + b[3] * a[3],
                    b[0] * a[4] + b[2] * a[5] + b[4], b[1] * a[4] + b[3] * a[5] + b[5]}};
}

Stroke map_stroke(const Stroke& stroke, const Mapping& m) {
    Stroke out = stroke;
    for (PointF& p : out.points) {
        const XY q = m.apply(XY{p.x, p.y});
        p = PointF{py_round(q.x, 4), py_round(q.y, 4)};
    }
    out.width_mm = py_round(stroke.width_mm * m.scale(), 4);
    const double turn = m.angle_deg();
    for (double& r : out.rotation) r = m.reflects() ? turn - r : r + turn;
    return out;
}

void map_points(std::vector<Point>& points, const Mapping& m) {
    for (Point& p : points) {
        const XY q = m.apply(XY{p.x.value(), p.y.value()});
        p = Point{Num(round3(q.x)), Num(round3(q.y))};
    }
}

std::array<double, 4> map_box(const std::array<double, 4>& box, const Mapping& m) {
    const auto [x, y, w, h] = box;
    return box_of({m.apply({x, y}), m.apply({x + w, y}), m.apply({x + w, y + h}), m.apply({x, y + h})});
}

void map_frame(Frame& frame, const Mapping& m) {
    const bool right = m.keeps_axes() || m.swaps_axes();
    const Rect old_box = frame.rect;
    const std::vector<Point> before = shape(frame);
    std::vector<XY> mapped;
    for (const Point& p : before) mapped.push_back(m.apply(XY{p.x.value(), p.y.value()}));
    const double k = m.scale();
    if (frame.poly && !frame.poly->empty()) {
        map_points(*frame.poly, m);
        std::vector<XY> pts;
        for (const Point& p : *frame.poly) pts.push_back(XY{p.x.value(), p.y.value()});
        frame.rect = rect_of(box_of(pts));
    } else if (right) {
        frame.rect = rect_of(box_of(mapped));
        // (each bow kept on its edge: the new box's corners, top left on, are the old ones in another order)
        if (frame.curves && frame.curves->size() == 4) {
            const std::vector<Point> now = corners(frame.rect);
            std::vector<double> curves(4, 0.0);
            for (std::size_t e = 0; e < 4; ++e) {
                const XY a{now[e].x.value(), now[e].y.value()}, b{now[(e + 1) % 4].x.value(), now[(e + 1) % 4].y.value()};
                for (std::size_t i = 0; i < 4; ++i) {
                    const XY p = mapped[i], q = mapped[(i + 1) % 4];
                    if ((near(p, a) && near(q, b)) || (near(p, b) && near(q, a))) curves[e] = (*frame.curves)[i];
                }
            }
            *frame.curves = std::move(curves);
        }
    } else {
        // (a map that is no right-angle turn: free corners — the box cannot hold it)
        std::vector<Point> poly;
        for (const XY& p : mapped) poly.push_back(Point{Num(round3(p.x)), Num(round3(p.y))});
        frame.poly = std::move(poly);
        frame.custom = true;
        frame.rect = rect_of(box_of(mapped));
    }
    if (frame.curves) {
        for (double& c : *frame.curves) c = round3(c * k);
    }
    frame.corner_mm = round3(frame.corner_mm * k);
    frame.border_mm = round3(frame.border_mm * k);
    if (frame.panel && frame.panel->is_object()) {
        if (const auto it = frame.panel->find("regions"); it != frame.panel->end() && it->is_array()) {
            for (Json& region : *it) {
                if (!region.is_object()) continue;
                const auto r = region.find("rect_mm");
                if (r == region.end() || !r->is_array() || r->size() != 4) continue;
                const auto b = map_box({(*r)[0].get<double>(), (*r)[1].get<double>(), (*r)[2].get<double>(), (*r)[3].get<double>()}, m);
                *r = Json::array({b[0], b[1], b[2], b[3]});
            }
        }
    }
    const bool split = frame.split_axis && (*frame.split_axis == "horizontal" || *frame.split_axis == "vertical");
    if (split && right) {
        // (the axis the children lie along, through the map: which it is now and whether they now run backwards)
        const bool across = *frame.split_axis == "vertical";  // (side by side, along x)
        const XY d = m.linear(across ? XY{1, 0} : XY{0, 1});
        const bool along_x = std::fabs(d.x) > std::fabs(d.y);
        frame.split_axis = along_x ? "vertical" : "horizontal";
        if ((along_x ? d.x : d.y) < 0) std::reverse(frame.children.begin(), frame.children.end());
        if (frame.split && frame.split->is_object() && frame.split->contains("a") && frame.split->contains("b")) {
            Json& cut = *frame.split;
            cut["a"] = map_norm(cut["a"], old_box, frame.rect, m);
            cut["b"] = map_norm(cut["b"], old_box, frame.rect, m);
            if (const auto g = cut.find("gutter_mm"); g != cut.end() && g->is_number()) *g = round3(g->get<double>() * k);
        }
    } else if (split) {
        frame.split_axis = std::string(kFreeSplit);
        frame.split.reset();
    }
    for (Frame& child : frame.children) map_frame(child, m);
}

void map_line(StoryLine& line, const Mapping& m) {
    const double k = m.scale();
    double w = line.w_mm.value() * k, h = line.h_mm.value() * k;
    if (m.swaps_axes()) std::swap(w, h);
    const XY c = m.apply(XY{line.x_mm.value() + line.w_mm.value() / 2, line.y_mm.value() + line.h_mm.value() / 2});
    line.x_mm = Num(round3(c.x - w / 2));
    line.y_mm = Num(round3(c.y - h / 2));
    line.w_mm = Num(round3(w));
    line.h_mm = Num(round3(h));
    if (line.tail) {
        const XY t = m.apply(XY{line.tail->x.value(), line.tail->y.value()});
        line.tail = Point{Num(round3(t.x)), Num(round3(t.y))};
    }
    for (Json& tail : line.tails) {
        if (!tail.is_object()) continue;
        move_point_at(tail, "to", m);
        move_point_at(tail, "via", m);
        move_points_at(tail, "vias", m);
    }
    if (line.path && !line.path->empty()) map_points(*line.path, m);
    // (a scale: its letters and its balloon's lines with it — Image Size)
    if (std::fabs(k - 1.0) > 1e-12 && line.style.is_object()) {
        for (const char* key : {"size_mm", "outline_mm", "border_mm", "text_dx_mm", "text_dy_mm"}) {
            if (const auto it = line.style.find(key); it != line.style.end() && it->is_number()) *it = round3(it->get<double>() * k);
        }
    }
    // (type@1, PC5 G-0: its point text's anchor and matrix with the page; a scale, its lengths too — in their normal form)
    if (line.native_type && line.native_type->is_object()) {
        Json nt = *line.native_type;
        if (const auto a = nt.find("anchor_mm"); a != nt.end() && a->is_array() && a->size() == 2) {
            const XY p = m.apply(XY{(*a)[0].get<double>(), (*a)[1].get<double>()});
            *a = Json::array({p.x, p.y});
        }
        if (const auto t = nt.find("transform"); t != nt.end() && t->is_array() && t->size() == 6) {
            std::array<double, 6> v{};
            for (std::size_t i = 0; i < 6; ++i) v[i] = (*t)[i].get<double>();
            const auto& M = m.m;
            *t = Json::array({M[0] * v[0] + M[2] * v[1], M[1] * v[0] + M[3] * v[1], M[0] * v[2] + M[2] * v[3], M[1] * v[2] + M[3] * v[3],
                              M[0] * v[4] + M[2] * v[5] + M[4], M[1] * v[4] + M[3] * v[5] + M[5]});
        }
        if (std::fabs(k - 1.0) > 1e-12) {
            const auto scale = [&](Json& attrs, std::initializer_list<const char*> keys) {
                if (!attrs.is_object()) return;
                for (const char* key : keys)
                    if (const auto it = attrs.find(key); it != attrs.end() && it->is_number()) *it = it->get<double>() * k;
            };
            const auto chars = [&](Json& holder) {
                if (holder.is_object() && holder.contains("char")) scale(holder["char"], {"size_mm", "leading_mm", "baseline_shift_mm"});
            };
            const auto paras = [&](Json& holder) {
                if (holder.is_object() && holder.contains("para"))
                    scale(holder["para"], {"first_indent_mm", "start_indent_mm", "end_indent_mm", "space_before_mm", "space_after_mm"});
            };
            chars(nt);
            paras(nt);
            if (nt.contains("runs") && nt["runs"].is_array())
                for (Json& run : nt["runs"]) chars(run);
            if (nt.contains("paragraphs") && nt["paragraphs"].is_array())
                for (Json& p : nt["paragraphs"]) paras(p);
        }
        try {
            const Json normal = type_text::check_native_type(nt, line.text);
            line.native_type = normal.is_null() ? std::nullopt : std::optional<Json>(normal);
        } catch (const Error&) {
            // (a scale that takes a length past its bounds: the settings kept as they were)
        }
    }
}

void map_ruler(Json& ruler, const Mapping& m) {
    if (!ruler.is_object()) return;
    const Json* kind = get(ruler, "kind");
    if (kind != nullptr && *kind == Json("guide")) {
        const auto at = ruler.find("at");
        if (at == ruler.end() || !at->is_number()) return;
        const double v = at->get<double>();
        const bool across = py_equals(get_or(ruler, "axis", Json()), Json("h"));  // (a horizontal guide at y = v)
        if (m.keeps_axes()) {
            *at = round3(across ? m.m[3] * v + m.m[5] : m.m[0] * v + m.m[4]);
            return;
        }
        if (m.swaps_axes()) {
            // (a horizontal guide at y becomes a vertical one at x' = c·y + e, and the other way)
            *at = round3(across ? m.m[2] * v + m.m[4] : m.m[1] * v + m.m[5]);
            ruler["axis"] = across ? "v" : "h";
            return;
        }
        // (no right-angle map: a line ruler through the same place)
        const XY p0 = across ? XY{0, v} : XY{v, 0};
        const XY p1 = across ? XY{100, v} : XY{v, 100};
        ruler["kind"] = "line";
        ruler.erase("at");
        ruler.erase("axis");
        ruler["points"] = Json::array({xy_json(m.apply(p0)), xy_json(m.apply(p1))});
        return;
    }
    move_points_at(ruler, "points", m);
    move_points_at(ruler, "points2", m);
    move_point_at(ruler, "center", m);
}

void map_effect(Json& effect, const Mapping& m) {
    if (!effect.is_object()) return;
    const auto params = effect.find("params");
    if (params == effect.end() || !params->is_object()) return;
    move_point_at(*params, "center", m);
    move_points_at(*params, "points", m);
    if (const auto inner = params->find("inner"); inner != params->end() && inner->is_array()) {
        for (Json& v : *inner)
            if (v.is_number()) v = round3(v.get<double>() * m.scale());
    }
    if (const auto angle = params->find("angle"); angle != params->end() && angle->is_number()) {
        const double a = angle->get<double>();
        *angle = round3(m.reflects() ? m.angle_deg() - a : a + m.angle_deg());
    }
}

void map_prim(Json& prim, const Mapping& m) {
    if (!prim.is_object()) return;
    if (!m.is_similarity_upright()) throw OpError("a 3D prop cannot be turned or mirrored with the page: render it to pixels first");
    if (const auto pos = prim.find("pos"); pos != prim.end() && pos->is_array() && pos->size() >= 2) {
        const XY p = m.apply(xy_of(*pos));
        (*pos)[0] = round3(p.x);
        (*pos)[1] = round3(p.y);
    }
    if (const auto size = prim.find("size"); size != prim.end() && size->is_array()) {
        for (Json& v : *size)
            if (v.is_number()) v = round3(v.get<double>() * m.scale());
    }
}

namespace {

// A map that keeps each axis the way it points (a move and a scale, each axis its own): what a mask area's box alone
// follows, its picture stretched over it. Any other — a half turn too, which keeps the axes but turns them round — moves
// its pixels (PixelHooks::area).
bool upright(const Mapping& m) { return m.keeps_axes() && m.m[0] > 0 && m.m[3] > 0; }

// Whether the map turns or mirrors the page (not a move and a scale alone).
bool turns_or_mirrors(const Mapping& m) { return m.reflects() || std::fabs(std::remainder(m.angle_deg(), 360.0)) > 1e-9; }

// An angle on the page (degrees, x towards y: as the map's own, Mapping::angle_deg) turned and mirrored as the map turns
// and mirrors the page — a mirror flips about the x axis first, then turns —: a screen's lines or dots, which have no way
// along them, mirrored to −a; a gradient's way to 180 − a.
double turned_angle(double a, const Mapping& m, bool has_way) {
    if (!m.reflects()) return a + m.angle_deg();
    return (has_way ? 180.0 - a : -a) + m.angle_deg();
}

double in_turn(double a, double turn) { return round3(std::fmod(std::fmod(a, turn) + turn, turn)); }

// (the coordinator's decision after the PC3 review) A tone's screen, its linear gradient's way and a layer's トーン化
// screen turned and mirrored with the page, its look kept: a tone layer's angle (0..360, as transform_layers turns it),
// a screen's (0..180, as screen_spec keeps it), the gradient's (0..360; 90 when it had none). A move or a scale leaves
// them as they are.
void turn_screens(Layer& layer, const Mapping& m) {
    if (!turns_or_mirrors(m)) return;
    if (layer.kind == LayerKind::Tone) layer.angle = in_turn(turned_angle(layer.angle, m, false), 360.0);
    if (layer.tone && layer.tone->is_object()) {
        if (const auto g = layer.tone->find("gradient"); g != layer.tone->end() && g->is_object()) {
            const auto shape = g->find("shape");
            const bool radial = shape != g->end() && shape->is_string() && shape->get<std::string>() == "radial";
            const auto angle = g->find("angle");
            std::optional<double> a;  // (as the drawing reads it: a number, or a number's text, as the op kept it)
            if (angle == g->end()) {
                a = 90.0;
            } else if (angle->is_number() || angle->is_string()) {
                try {
                    a = to_float(*angle);
                } catch (const Error&) {
                }
            }
            if (!radial && a && std::isfinite(*a)) (*g)["angle"] = in_turn(turned_angle(*a, m, true), 360.0);
        }
    }
    if (layer.screen && layer.screen->is_object()) {
        const auto angle = layer.screen->find("angle");
        if (angle == layer.screen->end() || angle->is_number()) {
            const double a = angle == layer.screen->end() ? 45.0 : angle->get<double>();
            (*layer.screen)["angle"] = in_turn(turned_angle(a, m, false), 180.0);
        }
    }
}

}  // namespace

void map_area(Json& area, const Mapping& m) {
    if (!area.is_object()) return;
    move_points_at(area, "poly", m);
    for (const char* key : {"union", "intersect", "subtract"}) {
        if (const auto it = area.find(key); it != area.end() && it->is_array())
            for (Json& part : *it) map_area(part, m);
    }
    for (const char* key : {"rect", "ellipse"}) {
        const auto it = area.find(key);
        if (it == area.end() || !it->is_array() || it->size() != 4) continue;
        if (!m.keeps_axes()) {  // (a turned box: its corners)
            const double x = (*it)[0].get<double>(), y = (*it)[1].get<double>(), w = (*it)[2].get<double>(), h = (*it)[3].get<double>();
            if (std::string_view(key) == "rect") {
                Json poly = Json::array();
                for (const XY& p : {XY{x, y}, XY{x + w, y}, XY{x + w, y + h}, XY{x, y + h}}) poly.push_back(xy_json(m.apply(p)));
                area.erase(key);
                area["poly"] = std::move(poly);
                continue;
            }
        }
        const auto b = map_box({(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>(), (*it)[3].get<double>()}, m);
        *it = Json::array({b[0], b[1], b[2], b[3]});
    }
    if (const auto mask = area.find("mask"); mask != area.end() && mask->is_object() && upright(m)) {
        if (const auto box = mask->find("box"); box != mask->end() && box->is_array() && box->size() == 4) {
            const auto b = map_box({(*box)[0].get<double>(), (*box)[1].get<double>(), (*box)[2].get<double>(), (*box)[3].get<double>()}, m);
            *box = Json::array({b[0], b[1], b[2], b[3]});
        }
    }
}

vector::Path map_path(const vector::Path& path, const Mapping& m) { return path.transformed(m.m); }

void map_shape_part(vector::ShapePart& part, const Mapping& m) {
    part.path = map_path(part.path, m);
    if (part.stroke) part.stroke->width_mm = py_round(part.stroke->width_mm * m.scale(), 4);
    map_fill(part.fill, m);
    if (part.stroke) map_fill(part.stroke->fill, m);
    if (!part.live) return;
    if (!m.is_similarity_upright()) {
        part.live.reset();  // (the path is the shape now)
        return;
    }
    vector::LiveShape& live = *part.live;
    const double k = m.scale();
    const XY at = m.apply(XY{live.rect[0], live.rect[1]});
    live.rect = {at.x, at.y, live.rect[2] * k, live.rect[3] * k};
    for (double& r : live.radii) r *= k;
    live.from = m.apply(live.from);
    live.to = m.apply(live.to);
    live.weight_mm *= k;
}

void map_fill(Json& fill, const Mapping& m) {
    if (!fill.is_object()) return;
    const auto gradient = fill.find("gradient");
    if (gradient == fill.end() || !gradient->is_object()) return;
    move_point_at(*gradient, "from", m);
    move_point_at(*gradient, "to", m);
}

void map_page(Document& doc, std::size_t at, const Mapping& m, const PixelHooks& hooks) {
    const Page before = doc.page(at);  // (a copy: its paper and pixels as they were)
    Page& page = doc.edit_page(at);
    const Page& after = page;
    for (Frame& frame : page.frames) map_frame(frame, m);
    for (Layer& layer : page.layers) {
        if (layer.stroke_count() > 0) {
            std::vector<StrokePtr> items;
            for (const StrokePtr& s : layer.strokes->items) items.push_back(std::make_shared<const Stroke>(map_stroke(*s, m)));
            layer.strokes = make_strokes(std::move(items));
        }
        if (layer.shapes && !layer.shapes->items.empty()) {
            std::vector<vector::ShapePart> parts = layer.shapes->items;
            for (vector::ShapePart& part : parts) map_shape_part(part, m);
            layer.shapes = vector::make_shapes(std::move(parts));
        }
        for (Patch& patch : layer.patches) {
            const Json* box = get(patch.attrs, "box");
            if (box == nullptr || !box->is_array() || box->size() != 4) continue;
            const auto b = map_box({(*box)[0].get<double>(), (*box)[1].get<double>(), (*box)[2].get<double>(), (*box)[3].get<double>()}, m);
            patch.attrs["box"] = Json::array({b[0], b[1], b[2], b[3]});
        }
        if (layer.region && !layer.region->empty()) map_points(*layer.region, m);
        if (layer.fill) map_fill(*layer.fill, m);
        if (layer.tone && layer.tone->is_object()) {
            if (const auto g = layer.tone->find("gradient"); g != layer.tone->end() && g->is_object()) {
                move_point_at(*g, "from", m);
                move_point_at(*g, "to", m);
            }
        }
        turn_screens(layer, m);
        if (layer.placement_mm) {
            // (a placed picture is laid upright in its box: a turn or a mirror is a smart object's — the page size ops make
            // it one first)
            if (!upright(m)) {
                not_yet_ported("a placed picture turned with the page: it is turned as a smart object (PC3 節 A)");
            }
            const Rect& r = *layer.placement_mm;
            layer.placement_mm = rect_of(map_box({r.x.value(), r.y.value(), r.width.value(), r.height.value()}, m));
        }
        // (type@1, PC5 G-0: a type layer's text mapped as a line's — upright, its box's centre where the map takes it)
        if (layer.kind == LayerKind::Type && layer.type) {
            StoryLine record = *layer.type;
            map_line(record, m);
            layer.type = type_text::shared_record(std::move(record));
        }
        // (smart-objects@1: its matrix after the map; its contents as they are)
        if (layer.kind == LayerKind::Smart) smart::transform_layer(layer, smart::Matrix{m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5]});
        if (layer.vector_mask) layer.vector_mask->path = map_path(layer.vector_mask->path, m);
        const bool filter_mask = layer.kind == LayerKind::Smart && layer.smart && layer.smart->is_object() && layer.smart->contains("filter_mask");
        const bool pixels = (layer.raster_png && !layer.raster_png->empty()) || layer.color_raster || layer.ink_raster ||
                            (layer.mask && layer.mask->png) || (!layer.patches.empty() && !m.is_similarity_upright()) || filter_mask;
        if (pixels) {
            if (!hooks.layer) not_yet_ported("moving a layer's pixels with the page: they are moved by the drawing build");
            hooks.layer(layer, m, before, after);
        }
    }
    for (Channel& channel : page.channels) {
        if (!channel.raster) continue;
        if (!hooks.channel) not_yet_ported("moving a page's channels: they are moved by the drawing build");
        hooks.channel(channel, m, before, after);
    }
    for (vector::NamedPath& p : page.paths) p.path = map_path(p.path, m);
    if (page.work_path) page.work_path = map_path(*page.work_path, m);
    if (page.rulers.is_array())
        for (Json& ruler : page.rulers) map_ruler(ruler, m);
    if (page.ruler && page.ruler->is_object()) move_points_at(*page.ruler, "points", m);
    if (page.effects.is_array())
        for (Json& effect : page.effects) map_effect(effect, m);
    if (page.prims.is_array())
        for (Json& prim : page.prims) map_prim(prim, m);
    if (const auto saved = page.extra.find("saved_areas"); saved != page.extra.end() && saved->is_object()) {
        for (auto& [name, area] : saved->items()) {
            const bool mask = area.is_object() && area.contains("mask");
            if (mask && !upright(m)) {
                if (!hooks.area) not_yet_ported("moving a saved area's pixels with the page: they are moved by the drawing build");
                hooks.area(area, m, before, after);
                continue;
            }
            map_area(area, m);
        }
    }
    for (StoryLine& line : doc.story) {
        if (line.page_index == page.index) map_line(line, m);
    }
}

// --- warps -------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 17> kStyles{"none",       "custom",     "arc",  "arcLower", "arcUpper", "arch",    "bulge",   "shellLower", "shellUpper",
                                                   "flag",       "wave",       "fish", "rise",     "fisheye",  "inflate", "squeeze", "twist"};

std::pair<double, double> arc(double u, double v, double b, double aspect) {
    if (std::fabs(b) < 1e-6) return {u, v};
    // (in half-heights, so the circle is round; the centre line's arc as long as the box's half-length)
    const double half = aspect;
    const double theta_max = b * kPi / 2;
    const double r = half / theta_max;
    const double theta = u * theta_max;
    const double rr = r - v;
    return {rr * std::sin(theta) / half, r - rr * std::cos(theta)};
}

std::pair<double, double> lerp2(std::pair<double, double> a, std::pair<double, double> b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return {a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t};
}

std::array<double, 4> bernstein(double t) {
    const double s = 1.0 - t;
    return {s * s * s, 3.0 * s * s * t, 3.0 * s * t * t, t * t * t};
}

XY lerp(const XY& a, const XY& b, double t) { return XY{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

std::array<XY, 7> split_cubic(const std::array<XY, 4>& p, double t) {
    const XY a = lerp(p[0], p[1], t), b = lerp(p[1], p[2], t), c = lerp(p[2], p[3], t);
    const XY d = lerp(a, b, t), e = lerp(b, c, t), f = lerp(d, e, t);
    return {p[0], a, d, f, e, c, p[3]};
}

// The Bernstein interpolation at t = 0, 1/3, 2/3, 1, inverted (rows: control point, columns: sample).
constexpr double kBernInv[4][4] = {{1.0, 0.0, 0.0, 0.0}, {-5.0 / 6.0, 3.0, -1.5, 1.0 / 3.0}, {1.0 / 3.0, -1.5, 3.0, -5.0 / 6.0}, {0.0, 0.0, 0.0, 1.0}};

std::pair<std::size_t, double> locate(const std::vector<double>& knots, double s) {
    const std::size_t n = knots.size() - 1;
    s = std::clamp(s, 0.0, 1.0);
    std::size_t i = 0;
    while (i + 1 < n && s > knots[i + 1]) ++i;
    const double w = knots[i + 1] - knots[i];
    return {i, w > 0.0 ? (s - knots[i]) / w : 0.0};
}

}  // namespace

std::optional<WarpStyle> warp_style_from(std::string_view text) {
    for (std::size_t i = 0; i < kStyles.size(); ++i)
        if (kStyles[i] == text) return static_cast<WarpStyle>(i);
    return std::nullopt;
}

std::string_view to_string(WarpStyle style) { return kStyles[static_cast<std::size_t>(style)]; }

bool is_preset(WarpStyle style) { return style != WarpStyle::None && style != WarpStyle::Custom; }

std::optional<StyleWarp> StyleWarp::make(WarpStyle style, double bend, double h_distort, double v_distort, bool vertical,
                                         const std::array<double, 4>& box) {
    if (!is_preset(style)) return std::nullopt;
    const auto [x0, y0, x1, y1] = box;
    const double hw = (x1 - x0) / 2.0, hh = (y1 - y0) / 2.0;
    if (!(hw > 0.0 && hh > 0.0)) return std::nullopt;
    const auto pct = [](double v) { return std::isfinite(v) ? std::clamp(v / 100.0, -1.0, 1.0) : 0.0; };
    StyleWarp w;
    w.style_ = style;
    w.b_ = pct(bend);
    w.hd_ = pct(h_distort);
    w.vd_ = pct(v_distort);
    if (w.b_ == 0.0 && w.hd_ == 0.0 && w.vd_ == 0.0) return std::nullopt;
    w.vertical_ = vertical;
    w.c_ = XY{(x0 + x1) / 2.0, (y0 + y1) / 2.0};
    w.h_ = XY{hw, hh};
    return w;
}

double StyleWarp::max_segment() const { return std::max(std::min(h_.x, h_.y) / 16.0, 0.5); }

XY StyleWarp::apply(double x, double y) const {
    // (u along the warp's axis, v across it, each −1..1 over the box)
    const double dx = x - c_.x, dy = y - c_.y;
    const double u = vertical_ ? dy / h_.y : dx / h_.x;
    const double v = vertical_ ? dx / h_.x : dy / h_.y;
    const double hu = vertical_ ? h_.y : h_.x, hv = vertical_ ? h_.x : h_.y;
    auto [u2, v2] = bend(u, v, hu / hv);
    if (hd_ != 0.0) v2 *= std::max(1.0 + hd_ * u2, 0.05);
    if (vd_ != 0.0) u2 *= std::max(1.0 + vd_ * v2, 0.05);
    const double ox = vertical_ ? v2 * h_.x : u2 * h_.x;
    const double oy = vertical_ ? u2 * h_.y : v2 * h_.y;
    return XY{c_.x + ox, c_.y + oy};
}

std::pair<double, double> StyleWarp::bend(double u, double v, double aspect) const {
    const double b = b_;
    const double bell = std::max(1.0 - u * u, 0.0);
    switch (style_) {
        case WarpStyle::Arc: return arc(u, v, b, aspect);
        case WarpStyle::ArcLower: return lerp2({u, v}, arc(u, v, b, aspect), (v + 1.0) / 2.0);
        case WarpStyle::ArcUpper: return lerp2({u, v}, arc(u, v, b, aspect), (1.0 - v) / 2.0);
        case WarpStyle::Arch: return {u, v - b * bell};
        case WarpStyle::Bulge: return {u, v * (1.0 + b * bell)};
        case WarpStyle::ShellLower: return {u, v + b * bell * (v + 1.0) / 2.0};
        case WarpStyle::ShellUpper: return {u, v - b * bell * (1.0 - v) / 2.0};
        case WarpStyle::Flag: return {u, v - b * 0.5 * std::sin(kPi * u)};
        case WarpStyle::Wave: return {u, v - b * 0.5 * std::sin(kPi * u + v * kPi / 2)};
        case WarpStyle::Fish: return {u, v * (1.0 + b * 0.6 * (1.0 - u) * std::sqrt(bell)) - b * 0.35 * bell};
        case WarpStyle::Rise: return {u, v - b * 0.5 * std::sin(kPi / 2 * u)};
        case WarpStyle::Fisheye: {
            const double r2 = std::min(u * u + v * v, 1.0);
            const double k = 1.0 + b * (1.0 - r2);
            return {u * k, v * k};
        }
        case WarpStyle::Inflate: {
            const double bu = std::max(1.0 - v * v, 0.0), bv = bell;
            return {u * (1.0 + b * 0.5 * bu), v * (1.0 + b * 0.5 * bv)};
        }
        case WarpStyle::Squeeze: return {u * (1.0 + b * 0.5 * std::max(1.0 - v * v, 0.0)), v * (1.0 - b * 0.5 * bell)};
        case WarpStyle::Twist: {
            const double r = std::sqrt(u * u + v * v);
            if (r >= 1.0) return {u, v};
            // (turned in an even frame, so the twist stays round on a wide box)
            const double a = b * kPi / 2 * (1.0 - r) * (1.0 - r);
            const double s = std::sin(a), c = std::cos(a);
            const double x = u * aspect, y = v;
            return {(x * c - y * s) / aspect, x * s + y * c};
        }
        case WarpStyle::None:
        case WarpStyle::Custom: return {u, v};
    }
    return {u, v};
}

BezierMesh BezierMesh::identity(const std::array<double, 4>& box, int cols, int rows) {
    cols = std::clamp(cols, 1, 64);
    rows = std::clamp(rows, 1, 64);
    std::vector<double> us, vs;
    for (int i = 0; i <= cols; ++i) us.push_back(static_cast<double>(i) / cols);
    for (int i = 0; i <= rows; ++i) vs.push_back(static_cast<double>(i) / rows);
    return fit([&](double s, double t) { return XY{box[0] + s * (box[2] - box[0]), box[1] + t * (box[3] - box[1])}; }, std::move(us), std::move(vs));
}

BezierMesh BezierMesh::fit(const std::function<XY(double, double)>& f, std::vector<double> us, std::vector<double> vs) {
    const std::size_t pc = us.size() - 1, pr = vs.size() - 1;
    const std::size_t nx = 3 * pc + 1, ny = 3 * pr + 1;
    std::vector<XY> points(nx * ny);
    for (std::size_t pj = 0; pj < pr; ++pj) {
        for (std::size_t pi = 0; pi < pc; ++pi) {
            XY s[4][4];
            for (std::size_t j = 0; j < 4; ++j) {
                for (std::size_t i = 0; i < 4; ++i) {
                    const double u = us[pi] + (us[pi + 1] - us[pi]) * static_cast<double>(i) / 3.0;
                    const double t = vs[pj] + (vs[pj + 1] - vs[pj]) * static_cast<double>(j) / 3.0;
                    s[j][i] = f(u, t);
                }
            }
            for (std::size_t j = 0; j < 4; ++j) {
                for (std::size_t i = 0; i < 4; ++i) {
                    XY acc;
                    for (std::size_t a = 0; a < 4; ++a) {
                        for (std::size_t b = 0; b < 4; ++b) {
                            const double k = kBernInv[j][a] * kBernInv[i][b];
                            acc.x += k * s[a][b].x;
                            acc.y += k * s[a][b].y;
                        }
                    }
                    points[(3 * pj + j) * nx + 3 * pi + i] = acc;
                }
            }
        }
    }
    return BezierMesh{std::move(us), std::move(vs), std::move(points)};
}

bool BezierMesh::valid() const {
    const auto knots_ok = [](const std::vector<double>& k) {
        if (k.size() < 2 || k.front() != 0.0 || k.back() != 1.0) return false;
        for (std::size_t i = 1; i < k.size(); ++i)
            if (!(k[i] > k[i - 1])) return false;
        return true;
    };
    if (!knots_ok(us) || !knots_ok(vs) || points.size() != nx() * ny()) return false;
    return std::all_of(points.begin(), points.end(), [](const XY& p) { return std::isfinite(p.x) && std::isfinite(p.y); });
}

XY BezierMesh::eval(double s, double t) const {
    const auto [pi, a] = locate(us, s);
    const auto [pj, b] = locate(vs, t);
    const auto ba = bernstein(a), bb = bernstein(b);
    const std::size_t n = nx();
    XY out;
    for (std::size_t j = 0; j < 4; ++j) {
        for (std::size_t i = 0; i < 4; ++i) {
            const XY& p = points[(3 * pj + j) * n + 3 * pi + i];
            const double k = ba[i] * bb[j];
            out.x += p.x * k;
            out.y += p.y * k;
        }
    }
    return out;
}

bool BezierMesh::split_u(double s) {
    const auto [pi, a] = locate(us, s);
    if (!(a >= 0.01 && a <= 0.99)) return false;
    const std::size_t n = nx(), rows = ny();
    std::vector<XY> pts;
    pts.reserve((n + 3) * rows);
    for (std::size_t j = 0; j < rows; ++j) {
        const XY* row = points.data() + j * n;
        pts.insert(pts.end(), row, row + 3 * pi);
        const auto parts = split_cubic({row[3 * pi], row[3 * pi + 1], row[3 * pi + 2], row[3 * pi + 3]}, a);
        pts.insert(pts.end(), parts.begin(), parts.end());
        pts.insert(pts.end(), row + 3 * pi + 4, row + n);
    }
    points = std::move(pts);
    us.insert(us.begin() + static_cast<std::ptrdiff_t>(pi + 1), std::clamp(s, 0.0, 1.0));
    return true;
}

bool BezierMesh::split_v(double t) {
    transpose();
    const bool ok = split_u(t);
    transpose();
    return ok;
}

bool BezierMesh::remove_split_u(std::size_t k) {
    if (k == 0 || k + 1 >= us.size()) return false;
    const double a = (us[k] - us[k - 1]) / (us[k + 1] - us[k - 1]);
    const std::size_t n = nx(), rows = ny();
    std::vector<XY> pts;
    pts.reserve((n - 3) * rows);
    const std::size_t c0 = 3 * (k - 1);
    for (std::size_t j = 0; j < rows; ++j) {
        const XY* row = points.data() + j * n;
        const XY p0 = row[c0], q1 = row[c0 + 1], r2 = row[c0 + 5], p3 = row[c0 + 6];
        // (de Casteljau undone: q1 = lerp(p0, p1, a), r2 = lerp(p2, p3, a))
        const XY p1{(q1.x - (1.0 - a) * p0.x) / a, (q1.y - (1.0 - a) * p0.y) / a};
        const XY p2{(r2.x - a * p3.x) / (1.0 - a), (r2.y - a * p3.y) / (1.0 - a)};
        pts.insert(pts.end(), row, row + c0);
        pts.insert(pts.end(), {p0, p1, p2, p3});
        pts.insert(pts.end(), row + c0 + 7, row + n);
    }
    points = std::move(pts);
    us.erase(us.begin() + static_cast<std::ptrdiff_t>(k));
    return true;
}

bool BezierMesh::remove_split_v(std::size_t k) {
    transpose();
    const bool ok = remove_split_u(k);
    transpose();
    return ok;
}

void BezierMesh::transpose() {
    const std::size_t n = nx(), rows = ny();
    std::vector<XY> pts(n * rows);
    for (std::size_t j = 0; j < rows; ++j)
        for (std::size_t i = 0; i < n; ++i) pts[i * rows + j] = points[j * n + i];
    points = std::move(pts);
    std::swap(us, vs);
}

std::pair<double, double> BezierMesh::param_at(const XY& p) const {
    const int n = 48;
    double bs = 0.5, bt = 0.5, best = std::numeric_limits<double>::max();
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const double s = static_cast<double>(i) / n, t = static_cast<double>(j) / n;
            const XY q = eval(s, t);
            const double d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
            if (d < best) {
                best = d;
                bs = s;
                bt = t;
            }
        }
    }
    double s = bs, t = bt;
    for (int k = 0; k < 12; ++k) {
        const double h = 1e-5;
        const XY q = eval(s, t), qs = eval(s + h, t), qt = eval(s, t + h);
        const double a = (qs.x - q.x) / h, b = (qt.x - q.x) / h, c = (qs.y - q.y) / h, d = (qt.y - q.y) / h;
        const double det = a * d - b * c;
        if (std::fabs(det) < 1e-12) break;
        const double ex = p.x - q.x, ey = p.y - q.y;
        s = std::clamp(s + (d * ex - b * ey) / det, 0.0, 1.0);
        t = std::clamp(t + (a * ey - c * ex) / det, 0.0, 1.0);
    }
    return {s, t};
}

bool Warp::is_identity() const {
    switch (style) {
        case WarpStyle::None: return true;
        case WarpStyle::Custom: {
            if (!mesh) return true;
            const BezierMesh id = BezierMesh::fit([&](double s, double t) { return XY{box[0] + s * (box[2] - box[0]), box[1] + t * (box[3] - box[1])}; },
                                                  mesh->us, mesh->vs);
            for (std::size_t i = 0; i < mesh->points.size() && i < id.points.size(); ++i) {
                if (std::fabs(mesh->points[i].x - id.points[i].x) >= 1e-9 || std::fabs(mesh->points[i].y - id.points[i].y) >= 1e-9) return false;
            }
            return true;
        }
        default: return !StyleWarp::make(style, bend, h_distort, v_distort, vertical, box).has_value();
    }
}

XY Warp::map(double x, double y) const {
    switch (style) {
        case WarpStyle::None: return XY{x, y};
        case WarpStyle::Custom: {
            const double w = box[2] - box[0], h = box[3] - box[1];
            if (!mesh || w <= 0.0 || h <= 0.0) return XY{x, y};
            return mesh->eval((x - box[0]) / w, (y - box[1]) / h);
        }
        default: {
            const auto sw = StyleWarp::make(style, bend, h_distort, v_distort, vertical, box);
            return sw ? sw->apply(x, y) : XY{x, y};
        }
    }
}

BezierMesh Warp::to_mesh(int cols, int rows) const {
    if (style == WarpStyle::Custom && mesh) return *mesh;
    cols = std::clamp(cols, 1, 64);
    rows = std::clamp(rows, 1, 64);
    std::vector<double> us, vs;
    for (int i = 0; i <= cols; ++i) us.push_back(static_cast<double>(i) / cols);
    for (int i = 0; i <= rows; ++i) vs.push_back(static_cast<double>(i) / rows);
    return BezierMesh::fit([&](double s, double t) { return map(box[0] + s * (box[2] - box[0]), box[1] + t * (box[3] - box[1])); },
                           std::move(us), std::move(vs));
}

}  // namespace genko::core::page_geometry
