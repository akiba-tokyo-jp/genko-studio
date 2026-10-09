// The book and page ops of M4 (Python's ops._apply_one with genko/pagespec.py, genko/bookops.py and genko/merge.py):
// set_page_spec (the book's paper, everything on each page moved from the old basic frame onto the new one),
// set_spread, add_cover (表紙・裏表紙・カバー・帯), set_assignee (担当) and import_pages (作品の結合: pages of another book,
// their lines too). for_pages is expanded by the CommandBus (expand_for_pages), replace_text is a line op
// (core/ops_lines.cpp) and set_nombre an M1 book op (core/ops_book.cpp).
//
// What Python keeps that this build's book cannot hold is refused last, once every error Python gives has had its turn:
// a paper that is not a finite number (NaN passes PageSpec.custom's checks) and positions moved beyond any number.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

Json op_value(const Json& op, std::string_view key) { return get_or(op, key, Json()); }

// --- set_page_spec: pagespec.spec_from ----------------------------------------------------------------------------

// pagespec.STUDIO_NAMES: the studio's names of the paper presets.
std::string paper_key(const std::string& name) {
    if (name == "commercial-b4") return "b4";
    if (name == "doujin-b5") return "b5";
    if (name == "doujin-a5") return "a5";
    if (name == "a4-mono") return "a4";
    return name;
}

// pagespec.spec_from: a PageSpec from a preset name or numbers (unset numbers keep the current ones), each value
// converted where and in the order Python converts it. Python's ValueError is PyValueError or core::Error("value") (from
// PageSpec::custom) or core::Error("format") (current margins that are not four numbers): set_page_spec words them as
// its refusal, as Python's op does.
PageSpec spec_from(const Json& op, const PageSpec& current) {
    const Json preset = op_value(op, "preset");
    if (py_truthy(preset)) {
        const PaperPreset* found = find_paper_preset(paper_key(py_str(preset)));
        if (found == nullptr) throw PyValueError("preset must be one of b4, b5, a5, a4, webtoon");
        PageSpec spec = found->make();
        if (truthy_at(op, "dpi")) spec.dpi = Num(to_int(op["dpi"]));  // (dataclasses.replace(spec, dpi=int(op["dpi"])))
        return spec;
    }
    const Json paper = py_or(op_value(op, "paper"), Json::array({current.width_mm.json(), current.height_mm.json()}));
    Json trim = op_value(op, "trim");
    if (!py_truthy(trim)) {
        const auto [w, h] = current.trim_size();
        trim = Json::array({w.json(), h.json()});
    }
    // margins: the current ones (a dict), then the given ones over them (a dict), or the given ones alone (a list)
    const PageSpec::Margins now = current.margins();
    std::vector<std::pair<std::string, double>> margins{{"top", now.top}, {"bottom", now.bottom}, {"inner", now.inner},
                                                        {"outer", now.outer}};
    const auto put = [&margins](const std::string& key, double value) {
        for (auto& [k, v] : margins) {
            if (k == key) {
                v = value;
                return;
            }
        }
        margins.emplace_back(key, value);
    };
    const Json given = op_value(op, "margins");
    if (given.is_object()) {
        for (const auto& [key, value] : given.items()) put(key, to_float(value));
    } else if (py_truthy(given)) {
        // dict(zip(("top", "bottom", "inner", "outer"), (float(v) for v in given))): at most four converted
        const std::vector<Json> items = iterate(given);
        margins.clear();
        const char* const keys[] = {"top", "bottom", "inner", "outer"};
        for (std::size_t k = 0; k < 4 && k < items.size(); ++k) put(keys[k], to_float(items[k]));
    }
    const auto margin = [&margins](const char* key) {
        for (const auto& [k, v] : margins) {
            if (k == key) return v;
        }
        throw OpKeyError(py_repr_str(key));
    };
    // PageSpec.custom(float(paper[0]), float(paper[1]), float(trim[0]), float(trim[1]), float(op.get("bleed_mm",
    // current.bleed_mm)), margins["top"], …, int(op.get("dpi") or current.dpi), current.expression)
    const double paper_w = to_float(subscript(paper, 0));
    const double paper_h = to_float(subscript(paper, 1));
    const double trim_w = to_float(subscript(trim, 0));
    const double trim_h = to_float(subscript(trim, 1));
    const double bleed = to_float(get_or(op, "bleed_mm", current.bleed_mm.json()));
    const double top = margin("top");
    const double bottom = margin("bottom");
    const double inner = margin("inner");
    const double outer = margin("outer");
    const Json dpi = py_or(op_value(op, "dpi"), current.dpi.json());
    return PageSpec::custom(paper_w, paper_h, trim_w, trim_h, bleed, top, bottom, inner, outer, to_int(dpi),
                            current.expression);
}

// --- set_page_spec: pagespec.relayout ------------------------------------------------------------------------------

// f(*value) for _Map's methods: Python's TypeErrors for what does not unpack into their parameters (`names`).
std::vector<Json> star_args(const Json& value, const char* method, std::initializer_list<const char*> names) {
    const std::string function = std::string("_Map.") + method + "()";
    if (!(value.is_array() || value.is_string() || value.is_object())) {
        throw PyTypeError("genko.pagespec." + function + " argument after * must be an iterable, not " + py_type_name(value));
    }
    std::vector<Json> items = iterate(value);
    const std::vector<const char*> all(names);
    if (items.size() < all.size()) {
        std::vector<std::string> missing;
        for (std::size_t k = items.size(); k < all.size(); ++k) missing.push_back(std::string("'") + all[k] + "'");
        std::string list = missing.front();  // 'h' | 'w' and 'h' | 'y', 'w', and 'h'
        for (std::size_t k = 1; k < missing.size(); ++k) {
            list += missing.size() == 2 ? " and " : (k + 1 == missing.size() ? ", and " : ", ");
            list += missing[k];
        }
        throw PyTypeError(function + " missing " + std::to_string(missing.size()) + " required positional argument" +
                          (missing.size() == 1 ? "" : "s") + ": " + list);
    }
    if (items.size() > all.size()) {
        throw PyTypeError(function + " takes " + std::to_string(all.size() + 1) + " positional arguments but " +
                          std::to_string(items.size() + 1) + " were given");
    }
    return items;
}

// pagespec._Map: old basic frame → new basic frame. Positions follow the frame (a stretch in x and y); sizes scale by
// the smaller of the two. Every value is rounded as Python rounds it; one that is not a finite number is remembered
// (`overflow`), to be refused once the whole book has been moved as Python moves it.
class Map {
public:
    Map(const Rect& old_frame, const Rect& new_frame, bool& overflow) : old_(old_frame), new_(new_frame), overflow_(overflow) {
        sx_ = old_.width.truthy() ? (new_.width / old_.width).value() : 1.0;
        sy_ = old_.height.truthy() ? (new_.height / old_.height).value() : 1.0;
        s_ = py_min(sx_, sy_);
    }

    const Rect& old_frame() const { return old_; }
    const Rect& new_frame() const { return new_; }
    double s() const { return s_; }

    // round(v, digits), remembering a value that is not finite
    double rounded(double v, int digits) const {
        const double out = py_round(v, digits);
        if (!std::isfinite(out)) overflow_ = true;
        return out;
    }

    std::pair<double, double> pt(double x, double y) const {
        const double nx = rounded(new_.x.value() + (x - old_.x.value()) * sx_, 3);
        const double ny = rounded(new_.y.value() + (y - old_.y.value()) * sy_, 3);
        return {nx, ny};
    }
    // m.pt(x, y) of values float() converts (x first)
    std::pair<double, double> pt(const Json& x, const Json& y) const {
        const double fx = to_float(x);
        const double nx = rounded(new_.x.value() + (fx - old_.x.value()) * sx_, 3);
        const double fy = to_float(y);
        const double ny = rounded(new_.y.value() + (fy - old_.y.value()) * sy_, 3);
        return {nx, ny};
    }
    // list(m.pt(*value))
    Json pt_star(const Json& value) const {
        const std::vector<Json> args = star_args(value, "pt", {"x", "y"});
        const auto [x, y] = pt(args[0], args[1]);
        return Json::array({x, y});
    }

    std::array<double, 4> box(double x, double y, double w, double h) const {
        const auto [nx, ny] = pt(x, y);
        return {nx, ny, rounded(w * sx_, 3), rounded(h * sy_, 3)};
    }
    // list(m.box(*value))
    Json box_star(const Json& value) const {
        const std::vector<Json> args = star_args(value, "box", {"x", "y", "w", "h"});
        const auto [nx, ny] = pt(args[0], args[1]);
        const double nw = rounded(to_float(args[2]) * sx_, 3);
        const double nh = rounded(to_float(args[3]) * sy_, 3);
        return Json::array({nx, ny, nw, nh});
    }

    // A box that keeps its shape: the middle follows the frame, the size scales by the smaller factor.
    std::array<double, 4> centred(double x, double y, double w, double h) const {
        const auto [cx, cy] = pt(x + w / 2, y + h / 2);
        const double nw = w * s_;
        const double nh = h * s_;
        return {rounded(cx - nw / 2, 3), rounded(cy - nh / 2, 3), rounded(nw, 3), rounded(nh, 3)};
    }

    // round(float(v) * m.s, 3) for each v (prim sizes, an effect's inner ellipse)
    Json scaled(const Json& values) const {
        Json out = Json::array();
        for (const Json& v : iterate(values)) out.push_back(rounded(to_float(v) * s_, 3));
        return out;
    }

private:
    Rect old_;
    Rect new_;
    double sx_ = 1.0;
    double sy_ = 1.0;
    double s_ = 1.0;
    bool& overflow_;
};

Rect rect_of(const std::array<double, 4>& box) { return Rect{Num(box[0]), Num(box[1]), Num(box[2]), Num(box[3])}; }

// [m.pt(x, y) for x, y in points] for points the model keeps (each a pair of numbers)
void move_points(std::vector<Point>& points, const Map& m) {
    for (Point& p : points) {
        const auto [x, y] = m.pt(p.x.value(), p.y.value());
        p = Point{Num(x), Num(y)};
    }
}

// [list(m.pt(x, y)) for x, y in points] for points kept as JSON (a ruler's, the old single ruler's)
Json moved_pairs(const Json& points, const Map& m) {
    Json out = Json::array();
    for (const Json& item : iterate(points)) {
        const std::vector<Json> xy = unpack_values(item, 2);
        const auto [x, y] = m.pt(xy[0], xy[1]);
        out.push_back(Json::array({x, y}));
    }
    return out;
}

// for item in items: …, with each item that Python takes for a dict changed in place (`each` gets a dict; an item of
// another kind is refused as Python's item.get refuses it)
template <class Each>
void each_dict(Json& items, Each each) {
    if (items.is_array()) {
        for (Json& item : items) {
            if (!item.is_object()) raise_attribute_error(item, "get");
            each(item);
        }
        return;
    }
    for (const Json& item : iterate(items)) raise_attribute_error(item, "get");  // (a str's characters, a dict's keys)
}

// pagespec._frames: the panel, its corners, its regions' boxes and the panels under it
void move_frame(Frame& frame, const Map& m) {
    frame.rect = rect_of(m.box(frame.rect.x.value(), frame.rect.y.value(), frame.rect.width.value(), frame.rect.height.value()));
    if (frame.poly && !frame.poly->empty()) move_points(*frame.poly, m);
    // for region in (frame.panel or {}).get("regions", []) or []
    if (frame.panel && py_truthy(*frame.panel)) {
        const Json* regions = dict_get(*frame.panel, "regions");
        if (regions != nullptr && py_truthy(*regions)) {
            each_dict((*frame.panel)["regions"], [&m](Json& region) {
                const Json* box = get(region, "rect_mm");
                if (box != nullptr && py_truthy(*box)) region["rect_mm"] = m.box_star(*box);
            });
        }
    }
    for (Frame& child : frame.children) move_frame(child, m);
}

// pagespec.relayout's work on one layer: its lines, patches, region, placement and pixels
void move_layer(Layer& layer, const Map& m, const PageSpec& spec, const BookPageHooks& hooks) {
    if (layer.stroke_count() > 0) {
        std::vector<StrokePtr> items;
        items.reserve(layer.strokes->items.size());
        for (const StrokePtr& stroke : layer.strokes->items) {
            Stroke moved = *stroke;
            for (PointF& p : moved.points) {
                const auto [x, y] = m.pt(p.x, p.y);
                p = PointF{x, y};
            }
            moved.width_mm = m.rounded(moved.width_mm * m.s(), 4);
            items.push_back(std::make_shared<const Stroke>(std::move(moved)));
        }
        layer.strokes = make_strokes(std::move(items));
    }
    for (Patch& patch : layer.patches) patch.attrs["box"] = m.box_star(subscript(patch.attrs, "box"));
    if (layer.region && !layer.region->empty()) move_points(*layer.region, m);
    if (layer.placement_mm) {
        const Rect& r = *layer.placement_mm;
        layer.placement_mm = rect_of(m.box(r.x.value(), r.y.value(), r.width.value(), r.height.value()));
    }
    if (layer.raster_png && !layer.raster_png->empty()) {
        if (!hooks.relayout_raster) {
            not_yet_ported("set_page_spec moving a paint layer's pixels: they are moved by the drawing build (render::ops_registry)");
        }
        layer.raster_png = std::make_shared<const std::string>(hooks.relayout_raster(*layer.raster_png, spec, m.old_frame(), m.new_frame()));
    }
    // (Python has no precise colour pixels: this build cannot move them onto the new frame yet)
    if (layer.color_raster) not_yet_ported("set_page_spec moving high-precision colour pixels is not in the C++ build yet");
}

// pagespec.relayout(episode, new_spec, move): the book on new paper; with `move`, everything on each page follows the
// basic frame. Whether a value moved is not a finite number (left as Python leaves it, for the op to refuse).
bool relayout(Document& doc, const PageSpec& spec, bool move, const BookPageHooks& hooks) {
    const std::string start_side = doc.start_side.value_or("");
    // olds = {page.index: page.inner_rect_mm(episode.start_side) for page in episode.pages}
    std::vector<std::pair<Num, Rect>> olds;
    for (const auto& page : doc.pages) {
        const Rect rect = page->inner_rect_mm(start_side);
        bool kept = false;
        for (auto& [index, value] : olds) {
            if (index == page->index) {
                value = rect;
                kept = true;
            }
        }
        if (!kept) olds.emplace_back(page->index, rect);
    }
    doc.spec = spec;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        Page& page = doc.edit_page(i);
        const Json* cover = cover_of(page);
        page.spec = cover != nullptr ? spec_for(spec, *cover) : spec;
    }
    if (!move) return false;
    bool overflow = false;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        Page& page = doc.edit_page(i);
        const Rect* old_frame = nullptr;
        for (const auto& [index, value] : olds) {
            if (index == page.index) old_frame = &value;
        }
        const Map m(*old_frame, page.inner_rect_mm(start_side), overflow);
        for (Frame& frame : page.frames) move_frame(frame, m);
        for (Layer& layer : page.layers) move_layer(layer, m, spec, hooks);
        for (StoryLine& line : doc.story) {
            if (!(line.page_index == page.index)) continue;
            if (line.x_mm.truthy() || line.y_mm.truthy()) {
                const auto box = m.centred(line.x_mm.value(), line.y_mm.value(), line.w_mm.value(), line.h_mm.value());
                line.x_mm = Num(box[0]);
                line.y_mm = Num(box[1]);
                line.w_mm = Num(box[2]);
                line.h_mm = Num(box[3]);
            }
            if (line.tail) {
                const auto [x, y] = m.pt(line.tail->x.value(), line.tail->y.value());
                line.tail = Point{Num(x), Num(y)};
            }
            for (Json& tail : line.tails) {
                if (!tail.is_object()) raise_attribute_error(tail, "get");
                // (only the tip and the one bend: Python leaves "vias" where they were)
                if (const Json* to = get(tail, "to"); to != nullptr && py_truthy(*to)) tail["to"] = m.pt_star(*to);
                if (const Json* via = get(tail, "via"); via != nullptr && py_truthy(*via)) tail["via"] = m.pt_star(*via);
            }
            if (line.path && !line.path->empty()) move_points(*line.path, m);
        }
        // (a ruler's "points" only: Python leaves points2, center and a guide's "at" where they were, and gives a ruler
        // without points an empty list of them)
        each_dict(page.rulers, [&m](Json& ruler) {
            const Json* points = get(ruler, "points");
            ruler["points"] = moved_pairs(points != nullptr && py_truthy(*points) ? *points : Json::array(), m);
        });
        if (page.ruler && py_truthy(*page.ruler)) {
            const Json* points = dict_get(*page.ruler, "points");
            if (points != nullptr && py_truthy(*points)) (*page.ruler)["points"] = moved_pairs(*points, m);
        }
        each_dict(page.prims, [&m](Json& prim) {
            // pos = list(prim.get("pos") or [0, 0, 0]) + [0, 0, 0]; prim["pos"] = [*m.pt(pos[0], pos[1]), pos[2]]
            const Json* given = get(prim, "pos");
            std::vector<Json> pos = given != nullptr && py_truthy(*given) ? iterate(*given)
                                                                          : std::vector<Json>{Json(0), Json(0), Json(0)};
            for (int k = 0; k < 3; ++k) pos.emplace_back(0);
            const auto [x, y] = m.pt(pos[0], pos[1]);
            prim["pos"] = Json::array({x, y, pos[2]});
            if (const Json* size = get(prim, "size"); size != nullptr && py_truthy(*size)) prim["size"] = m.scaled(*size);
        });
        each_dict(page.effects, [&m](Json& effect) {
            // params = effect.get("params") or {}: an empty one is a new dict, and changes nothing
            const Json* given = get(effect, "params");
            if (given == nullptr || !py_truthy(*given)) return;
            Json& params = effect["params"];
            const Json* center = dict_get(params, "center");
            if (center != nullptr && py_truthy(*center)) params["center"] = m.pt_star(*center);
            const Json* inner = dict_get(params, "inner");
            if (inner != nullptr && py_truthy(*inner)) params["inner"] = m.scaled(*inner);
        });
    }
    return overflow;
}

// The new paper's sizes as numbers this build's book can hold (Python keeps a NaN paper, which passes
// PageSpec.custom's checks, and writes NaN into project.json).
void require_finite_spec(const PageSpec& spec) {
    const auto finite = [](const std::optional<Num>& n) { return !n || std::isfinite(n->value()); };
    if (!std::isfinite(spec.width_mm.value()) || !std::isfinite(spec.height_mm.value())) {
        throw OpError("paper must be a finite number");
    }
    if (!finite(spec.trim_w_mm) || !finite(spec.trim_h_mm)) throw OpError("trim must be a finite number");
    if (!std::isfinite(spec.bleed_mm.value())) throw OpError("bleed_mm must be a finite number");
    bool margins = std::isfinite(spec.inner_margin_mm.value());
    if (spec.margins_mm) {
        for (const Num& v : *spec.margins_mm) margins = margins && std::isfinite(v.value());
    }
    if (!margins) throw OpError("margins must be a finite number");
}

void set_page_spec(OpContext& c, const BookPageHooks& hooks) {
    PageSpec spec;
    try {
        spec = spec_from(c.op, c.doc.spec);
    } catch (const PyUncaught&) {
        throw;
    } catch (const Error& error) {
        // (except ValueError as exc: raise ApplyError(str(exc)))
        if (error.code() == "value" || error.code() == "format") throw OpError(error.what());
        throw;
    }
    // relayout(episode, spec, move=op.get("move", True) is not False)
    const Json* move = get(c.op, "move");
    const bool overflow = relayout(c.doc, spec, !(move != nullptr && move->is_boolean() && !move->get<bool>()), hooks);
    require_finite_spec(spec);
    if (overflow) throw OpError("paper is too large: what is on the pages would be moved beyond any number");
}

// --- set_spread, add_cover, set_assignee -------------------------------------------------------------------------

void set_spread(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json other = op_value(c.op, "with");
    // other in (None, "", 0): no spread
    if (other.is_null() || py_equals(other, Json("")) || py_equals(other, Json(0))) {
        if (doc.page(i).spread_with) doc.edit_page(i).spread_with.reset();
        return;
    }
    // (a page number past 64 bits names no page, as Python's unbounded int does)
    const std::optional<std::int64_t> wanted = py_big_int_text(other) ? std::nullopt : std::optional<std::int64_t>(to_int(other));
    std::size_t partner = doc.pages.size();
    for (std::size_t k = 0; wanted && k < doc.pages.size(); ++k) {
        if (doc.pages[k]->index == Num(*wanted)) {
            partner = k;
            break;
        }
    }
    if (partner == doc.pages.size()) throw OpError("no page " + py_str(other));
    const auto problem = facing_problem(doc, doc.page(i), doc.page(partner));
    if (problem && doc.strict_gates) throw OpError(*problem);
    const Num index = doc.page(partner).index;
    doc.edit_page(i).spread_with = index;
}

// bookops.add_cover: a cover page at the end of the book, without nombre, with one borderless panel to the bleed (or
// the trim)
void add_cover(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string kind = py_str(py_or(op_value(op, "kind"), Json("front")));
    if (!is_cover_kind(kind)) throw OpError("kind must be front, back, jacket or obi");
    for (const auto& page : doc.pages) {
        const Json* cover = cover_of(*page);
        if (cover != nullptr && cover->at("kind") == Json(kind)) throw OpError("the book already has a " + kind + " cover");
    }
    Json cover = Json::object({{"kind", kind}});
    if (is_wrap_kind(kind)) {
        const double spine = to_float(py_or(op_value(op, "spine_mm"), Json(0)));
        const double flap = to_float(py_or(op_value(op, "flap_mm"), Json(0)));
        if (!(0 < spine && spine <= 100)) throw OpError("spine_mm is the spine's width (0..100 mm)");
        if (!(0 <= flap && flap <= 200)) throw OpError("flap_mm is 0..200 mm");
        cover["spine_mm"] = spine;
        cover["flap_mm"] = flap;
        if (kind == "obi") {
            const double height = to_float(py_or(op_value(op, "height_mm"), Json(50)));
            if (!(15 <= height && height <= 200)) throw OpError("height_mm is the band's height (15..200 mm)");
            cover["height_mm"] = height;
        }
    }
    const PageSpec spec = spec_for(doc.spec, cover);
    Page page = make_page(Num(static_cast<std::int64_t>(doc.pages.size()) + 1), spec, doc.binding);
    page.numero = false;
    page.extra["cover"] = cover;
    const bool bleed = py_truthy(get_or(op, "bleed", Json(true)));
    Frame frame;
    frame.id = new_id();
    frame.rect = bleed ? page.bleed_rect_mm() : page.trim_rect_mm();
    frame.bleed = bleed;
    frame.border_mm = 0.0;
    page.frames.push_back(std::move(frame));
    doc.pages.push_back(std::make_shared<Page>(std::move(page)));
}

// A page number as Python's int() makes it from an op's value: the int, or (past 64 bits, where Python's int has no
// bound) its digits, which name no page.
struct PageNumber {
    std::optional<std::int64_t> value;
    std::string text;
};

PageNumber page_number(const Json& v) {
    if (const auto big = py_big_int_text(v)) return PageNumber{std::nullopt, *big};
    const std::int64_t n = to_int(v);
    return PageNumber{n, std::to_string(n)};
}

// bookops.set_assignee: who draws a page (a name, or none), for sharing the work out
void set_assignee(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    // [int(v) for v in op.get("pages") or ([op["page"]] if op.get("page") else [])]
    std::vector<PageNumber> pages;
    const Json given = op_value(op, "pages");
    if (py_truthy(given)) {
        for (const Json& v : iterate(given)) pages.push_back(page_number(v));
    } else if (truthy_at(op, "page")) {
        pages.push_back(page_number(op["page"]));
    }
    if (pages.empty()) throw OpError("pages is the list of pages");
    const std::string who = py_strip(py_str(py_or(op_value(op, "who"), Json(""))));
    for (const PageNumber& index : pages) {
        std::optional<std::size_t> at;
        for (std::size_t k = 0; index.value && k < doc.pages.size(); ++k) {
            if (doc.pages[k]->index == Num(*index.value)) {
                at = k;
                break;
            }
        }
        if (!at) throw OpError("no page " + index.text);
        if (!who.empty()) {
            // who[:40]: its first 40 characters
            std::size_t end = 0;
            for (int n = 0; n < 40 && end < who.size(); ++n) {
                const auto ch = static_cast<unsigned char>(who[end]);
                end += ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : 4;
            }
            const Json name(who.substr(0, std::min(end, who.size())));
            if (get_or(doc.page(*at).extra, "assignee", Json()) != name) doc.edit_page(*at).extra["assignee"] = name;
        } else if (doc.page(*at).extra.contains("assignee")) {
            doc.edit_page(*at).extra.erase("assignee");
        }
    }
}

// --- import_pages (作品の結合) -------------------------------------------------------------------------------------

// Pages of another book added after this one's, before its covers at the back (or after the page "after" names), with
// their lines, their panels, layers and lines under new ids. The pages refer to the other book's asset files (placed
// pictures) without copying them: the import dialog and the MCP tool copy them first (storage::copy_assets, as
// Python's app and MCP tools do with merge.copy_assets); `genko apply`, as Python's, copies none.
void import_pages(OpContext& c, const BookPageHooks& hooks) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string from = py_str(py_or(op_value(op, "from"), Json("")));
    if (!hooks.load_book) not_yet_ported("import_pages reads the other book through storage (render::ops_registry)");
    const Document other = hooks.load_book(from);
    // wanted = [int(p) for p in op.get("pages") or [p.index for p in other.pages]]
    std::vector<PageNumber> wanted;
    const Json given = op_value(op, "pages");
    if (py_truthy(given)) {
        for (const Json& v : iterate(given)) wanted.push_back(page_number(v));
    } else {
        for (const auto& page : other.pages) wanted.push_back(page_number(page->index.json()));
    }
    // by_index = {p.index: p for p in other.pages}: the last page with each number
    const auto source_of = [&other](const PageNumber& index) -> const Page* {
        const Page* found = nullptr;
        for (const auto& page : other.pages) {
            if (index.value && page->index == Num(*index.value)) found = page.get();
        }
        return found;
    };
    for (const PageNumber& index : wanted) {
        if (source_of(index) == nullptr) throw OpError("the other book has no page " + index.text);
    }
    const std::size_t start = doc.pages.size();
    for (std::size_t n = 0; n < wanted.size(); ++n) {
        const Page& source = *source_of(wanted[n]);
        Page clone = source;
        clone.index = Num(static_cast<std::int64_t>(start + n + 1));
        clone.id = "pg_" + new_id();
        clone.spread_with.reset();
        FrameIdMap frame_map;
        for (Frame& frame : clone.frames) refresh_frame_ids(frame, frame_map);
        if (py_truthy(clone.selected_frame_id)) {  // (never set on a book just read)
            const std::string* mapped =
                clone.selected_frame_id.is_string() ? frame_map.get(clone.selected_frame_id.get<std::string>()) : nullptr;
            clone.selected_frame_id = mapped != nullptr ? Json(*mapped) : Json(nullptr);
        }
        for (Layer& layer : clone.layers) {
            layer.id = new_id();
            if (layer.frame_id && !layer.frame_id->empty()) {
                if (const std::string* mapped = frame_map.get(*layer.frame_id)) layer.frame_id = *mapped;
            }
        }
        // the lines of other.story_for_page(index), copied under new ids onto the new page
        for (const StoryLine& line : other.story) {
            if (!(line.page_index == Num(*wanted[n].value))) continue;
            StoryLine copied = line;
            copied.id = new_id();
            copied.page_index = clone.index;
            if (copied.frame_id && !copied.frame_id->empty()) {
                if (const std::string* mapped = frame_map.get(*copied.frame_id)) copied.frame_id = *mapped;
            }
            doc.story.push_back(std::move(copied));
        }
        doc.pages.push_back(std::make_shared<Page>(std::move(clone)));
    }
    // what the pages' pictures are: the other book's studio assets this book does not have
    if (py_truthy(other.studio)) {
        const Json* assets = dict_get(other.studio, "assets");
        const Json items = assets != nullptr && py_truthy(*assets) ? *assets : Json::object();
        if (!items.is_object()) raise_attribute_error(items, "items");
        for (const auto& [key, info] : items.items()) {
            if (!doc.studio.is_object()) raise_attribute_error(doc.studio, "setdefault");
            if (!doc.studio.contains("assets")) doc.studio["assets"] = Json::object();
            Json& mine = doc.studio["assets"];
            if (!mine.is_object()) raise_attribute_error(mine, "setdefault");
            if (!mine.contains(key)) mine[key] = info;
        }
    }
    // covers, jackets and bands at the back stay at the back: the pages go in before them
    std::size_t tail = 0;
    while (tail < start && cover_of(*doc.pages[start - 1 - tail]) != nullptr) ++tail;
    std::optional<std::int64_t> after;
    if (const Json given_after = op_value(op, "after"); !given_after.is_null()) {
        after = to_int_held(given_after);
    } else if (tail > 0) {
        after = static_cast<std::int64_t>(start - tail);
    }
    if (!after) return;
    const auto at = static_cast<std::size_t>(std::max<std::int64_t>(0, std::min<std::int64_t>(static_cast<std::int64_t>(start), *after)));
    std::vector<Num> order;
    for (std::size_t k = 0; k < start; ++k) order.push_back(doc.pages[k]->index);
    std::vector<Num> fresh;
    for (std::size_t k = 0; k < wanted.size(); ++k) fresh.emplace_back(static_cast<std::int64_t>(start + k + 1));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), fresh.begin(), fresh.end());
    reorder_pages(doc, order);
}

}  // namespace

void register_bookpage_ops(OpRegistry& registry, BookPageHooks hooks) {
    registry.add("set_page_spec", [hooks](OpContext& c) { set_page_spec(c, hooks); });
    registry.add("set_spread", set_spread);
    registry.add("add_cover", add_cover);
    registry.add("set_assignee", set_assignee);
    registry.add("import_pages", [hooks](OpContext& c) { import_pages(c, hooks); });
}

}  // namespace genko::core
