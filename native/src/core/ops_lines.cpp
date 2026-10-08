// The lines of dialogue and their balloons (Python's ops._apply_one and bookops.replace_text): add_line, edit_line,
// move_line, delete_line, reorder_lines, cut_balloon (フキダシ消しゴム), set_balloon_path (a balloon drawn by hand) and
// replace_text (一括置換), with the helpers they share: the style keys a line takes (_merge_style), its tails
// (_parse_tails, and _tails_outside for a balloon moved over its own tail), its ruby, dots (傍点) and styled words.
// Nothing is drawn.
//
// Python also keeps each page's lines in page.texts (the same objects as the story's, read from the story); this build
// keeps the story alone. Every id page.texts holds is in the story too (add_line and delete_line change both, and the
// ops that renumber pages keep the ids), so _find_line, which looks in the story first, finds the same line here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyre.hpp"

namespace genko::core {

namespace {

// balloons.SHAPES and balloons.TAIL_KINDS
constexpr std::array<std::string_view, 16> kShapes{"speech", "rounded", "box",    "cloud",     "thought",  "shout",
                                                   "electric", "flash", "whisper", "narration", "sfx",      "none",
                                                   "picture", "dotted_box", "tone_box", "fancy_box"};
constexpr std::array<std::string_view, 5> kTailKinds{"wedge", "straight", "zigzag", "fade", "bubbles"};

// ops.FEATURES: the OpenType forms a line's style may ask for
constexpr std::array<std::string_view, 25> kFeatures{"jp78", "jp83", "jp90", "jp04", "trad", "expt", "nlck", "hojo", "hwid",
                                                     "fwid", "pwid", "palt", "twid", "qwid", "ruby", "liga", "kern", "smpl",
                                                     "ital", "salt", "ss01", "ss02", "ss03", "ss04", "ss05"};

// ops.STYLE_KEYS: each key and the kind of value it keeps, in Python's order (the order its error lists them in)
enum class Kind { Str, Float, List, Bool, Int, Dict, Points, Tags, Png, Corners, Cuts };

struct StyleKey {
    std::string_view name;
    Kind kind;
};

constexpr StyleKey kStyleKeys[] = {
    {"font", Kind::Str},          {"size_mm", Kind::Float},      {"tracking", Kind::Float},     {"leading", Kind::Float},
    {"align", Kind::Str},         {"outline_mm", Kind::Float},   {"rgb", Kind::List},           {"tcy", Kind::Bool},
    {"border_mm", Kind::Float},   {"fill", Kind::Str},           {"group", Kind::Str},          {"rotate_deg", Kind::Float},
    {"skew_deg", Kind::Float},    {"arc", Kind::Float},          {"latin", Kind::Str},          {"emphasis_mark", Kind::Str},
    {"bold", Kind::Bool},         {"weight", Kind::Str},         {"italic", Kind::Bool},        {"outline_rgb", Kind::List},
    {"wobble", Kind::Float},      {"double", Kind::Bool},        {"spikes", Kind::Int},         {"spike_depth", Kind::Float},
    {"scale_x", Kind::Float},     {"gradient", Kind::Dict},      {"text_path", Kind::Points},   {"features", Kind::Tags},
    {"yakumono", Kind::Bool},     {"spike_jitter", Kind::Float}, {"bumps", Kind::Int},          {"picture", Kind::Png},
    {"fill_png", Kind::Png},      {"warp", Kind::Corners},       {"speaker_id", Kind::Str},     {"line_rgb", Kind::List},
    {"fill_rgb", Kind::List},     {"fill_opacity", Kind::Float}, {"text_dx_mm", Kind::Float},   {"text_dy_mm", Kind::Float},
    {"path_curve", Kind::Bool},   {"cuts", Kind::Cuts},          {"ruby_scale", Kind::Float},   {"mono_ruby", Kind::Bool},
    {"below_layer", Kind::Str},   {"hand", Kind::Bool},          {"periods", Kind::Bool},
};

template <std::size_t N>
std::string joined(const std::array<std::string_view, N>& names) {
    std::string out;
    for (const std::string_view name : names) out += (out.empty() ? "" : ", ") + std::string(name);
    return out;
}

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

Json op_value(const Json& op, std::string_view key) { return get_or(op, key, Json()); }

// Python's float / float: ZeroDivisionError ("float division by zero"), which apply_ops lets through
double divided(double a, double b) {
    if (b == 0.0) throw PyUncaught("ZeroDivisionError", "float division by zero");
    return a / b;
}

// x ** 2 for a float (libm's pow, as Python calls it): OverflowError past the largest float, which apply_ops lets through
double squared(double x) {
    try {
        return py_pow(x, 2.0);
    } catch (const Error& error) {
        throw PyUncaught("OverflowError", error.what());
    }
}

// Python's min(values) and max(values) of floats: the first unless a later one is smaller (larger), as Python compares
// them (a NaN met first stays)
double min_of(const std::vector<double>& values) {
    double out = values.front();
    for (const double v : values) out = py_min(out, v);
    return out;
}

double max_of(const std::vector<double>& values) {
    double out = values.front();
    for (const double v : values) out = py_max(out, v);
    return out;
}

// [float(p[0]), float(p[1])]
Json float_pair(const Json& p) {
    const double x = to_float(subscript(p, 0));
    const double y = to_float(subscript(p, 1));
    return Json::array({x, y});
}

Json point_json(const Point& p) { return Json::array({p.x.json(), p.y.json()}); }

// tuple(tail["to"]) as the line's single tail: Python keeps whatever the tail's "to" holds; this build's line keeps two
// numbers, so anything else (only a hand-written book has it) is refused.
Point tail_point(const Json& to) {
    const std::vector<Json> items = iterate(to);
    if (items.size() == 2) {
        const auto x = Num::from_json(items[0]);
        const auto y = Num::from_json(items[1]);
        if (x && y) return Point{*x, *y};
    }
    throw OpError("a tail's to must be two numbers [x, y]");
}

// ops._find_line: the first line of the story with this id (see the top of the file for page.texts)
StoryLine& find_line(Document& doc, const std::string& line_id) {
    for (StoryLine& line : doc.story) {
        if (line.id == line_id) return line;
    }
    throw OpError("no line " + line_id);
}

// ops._parse_tail: None, or (float(value[0]), float(value[1]))
std::optional<Point> parse_tail(const Json& value) {
    if (value.is_null()) return std::nullopt;
    const double x = to_float(subscript(value, 0));
    const double y = to_float(subscript(value, 1));
    return Point{Num(x), Num(y)};
}

// ops._parse_tails: [{"to": [x, y], "via"?, "vias"?, "width_mm"?, "kind"?}] (an item may be the [x, y] alone)
std::vector<Json> parse_tails(const Json& raw) {
    std::vector<Json> tails;
    for (Json item : iterate(py_or(raw, Json::array()))) {
        if (item.is_array()) item = Json::object({{"to", item}});
        const Json* to = item.is_object() ? get(item, "to") : nullptr;
        if (to == nullptr || !py_truthy(*to) || length(*to) < 2) throw OpError("a tail needs to: [x, y]");
        Json tail = Json::object();
        tail["to"] = float_pair(*to);
        if (truthy_at(item, "via")) tail["via"] = float_pair(item["via"]);
        if (truthy_at(item, "vias")) {  // 折れ線のしっぽ: it bends at each of these
            Json vias = Json::array();
            for (const Json& v : iterate(item["vias"])) vias.push_back(float_pair(v));
            tail["vias"] = std::move(vias);
            tail.erase("via");
        }
        if (truthy_at(item, "width_mm")) tail["width_mm"] = to_float(item["width_mm"]);
        if (truthy_at(item, "kind") && item["kind"] != Json("wedge")) {
            if (!item["kind"].is_string() || !listed(kTailKinds, item["kind"].get_ref<const std::string&>())) {
                throw OpError("tail kind must be one of " + joined(kTailKinds));
            }
            tail["kind"] = py_str(item["kind"]);
        }
        tails.push_back(std::move(tail));
    }
    return tails;
}

// ops._balloon_kind: str(kind or "speech"), one of balloons.SHAPES
std::string balloon_kind(const Json& kind) {
    const std::string out = py_str(py_or(kind, Json("speech")));
    if (!listed(kShapes, out)) throw OpError("balloon must be one of " + joined(kShapes));
    return out;
}

// [tuple(item) for item in raw]: each ruby run as the list of what it holds
std::vector<Json> ruby_runs_of(const Json& raw) {
    std::vector<Json> out;
    for (const Json& item : iterate(raw)) {
        Json run = Json::array();
        for (const Json& part : iterate(item)) run.push_back(part);
        out.push_back(std::move(run));
    }
    return out;
}

// ops._emphasis: the words that carry dots (傍点)
std::vector<std::string> emphasis(const Json& raw) {
    if (!raw.is_array()) throw OpError("emphasis_runs must be a list of the words that carry dots");
    std::vector<std::string> out;
    for (const Json& item : raw) {
        if (!py_truthy(item)) continue;
        out.push_back(py_str(item.is_array() ? item[0] : item));
    }
    return out;
}

// ops._style_runs: [[words, {scale?, bold?, weight?, rgb?, tcy?}]] — part of a line styled
std::vector<std::pair<std::string, Json>> style_runs(const Json& raw) {
    std::vector<std::pair<std::string, Json>> out;
    for (const Json& item : iterate(py_or(raw, Json::array()))) {
        if (!item.is_array() || item.size() < 2 || !py_truthy(item[0]) || !item[1].is_object()) {
            throw OpError("style_runs is [[words, {scale, bold, rgb}], ...]");
        }
        Json style = Json::object();
        for (const auto& [key, value] : item[1].items()) {
            if (key == "scale") {
                const double scale = to_float(value);
                if (!(0.3 <= scale && scale <= 3)) throw OpError("scale must be between 0.3 and 3");
                style["scale"] = scale;
            } else if (key == "bold") {
                style["bold"] = py_equals(value, Json(2)) ? Json(2) : Json(py_truthy(value));  // (2: 極太)
            } else if (key == "tcy") {
                style["tcy"] = py_truthy(value);
            } else if (key == "weight") {
                if (!is_one_of(value, {"normal", "bold", "heavy"})) throw OpError("weight must be normal, bold or heavy");
                const std::string& weight = value.get_ref<const std::string&>();
                style["bold"] = weight == "normal" ? Json(0) : weight == "bold" ? Json(true) : Json(2);
            } else if (key == "rgb") {
                Json rgb = Json::array();
                for (const Json& v : iterate(value)) rgb.push_back(to_int(v));
                style["rgb"] = py_slice(rgb, 0, 3);
            } else {
                throw OpError("unknown style_runs key " + key + " (scale, bold, weight, rgb, tcy)");
            }
        }
        out.emplace_back(py_str(item[0]), std::move(style));
    }
    return out;
}

// ops._set_path: a balloon drawn by hand — its outline, and the box becomes the outline's bounds
void set_path(StoryLine& line, const Json& raw) {
    if (!py_truthy(raw)) {
        line.path.reset();
        return;
    }
    std::vector<double> xs, ys;
    try {
        for (const Json& p : iterate(raw)) {
            const double x = py_round(to_float(subscript(p, 0)), 3);
            const double y = py_round(to_float(subscript(p, 1)), 3);
            xs.push_back(x);
            ys.push_back(y);
        }
    } catch (const PyTypeError&) {
        throw OpError("path must be [[x_mm, y_mm], ...]");
    } catch (const PyValueError&) {
        throw OpError("path must be [[x_mm, y_mm], ...]");
    } catch (const PyUncaught& error) {
        if (error.type() != "IndexError") throw;
        throw OpError("path must be [[x_mm, y_mm], ...]");
    }
    if (xs.size() < 3) throw OpError("a balloon outline needs at least three points");
    const double x0 = min_of(xs), x1 = max_of(xs), y0 = min_of(ys), y1 = max_of(ys);
    if (x1 - x0 < 2 || y1 - y0 < 2) throw OpError("the balloon outline is too small");
    std::vector<Point> points;
    for (std::size_t i = 0; i < xs.size(); ++i) points.push_back(Point{Num(xs[i]), Num(ys[i])});
    line.path = std::move(points);
    line.x_mm = Num(x0);
    line.y_mm = Num(y0);
    line.w_mm = Num(x1 - x0);
    line.h_mm = Num(y1 - y0);
}

// [[round(float(p[0]), digits), round(float(p[1]), digits)] for p in value]
Json rounded_points(const Json& value, int digits) {
    Json out = Json::array();
    for (const Json& p : iterate(value)) {
        const double x = py_round(to_float(subscript(p, 0)), digits);
        const double y = py_round(to_float(subscript(p, 1)), digits);
        out.push_back(Json::array({x, y}));
    }
    return out;
}

// [int(v) for v in value][:3]
Json int_list3(const Json& value) {
    Json out = Json::array();
    for (const Json& v : iterate(value)) out.push_back(to_int(v));
    return py_slice(out, 0, 3);
}

// A style value converted as STYLE_KEYS says (inside _merge_style's try: Python's TypeError, ValueError, IndexError,
// AttributeError and OSError become "style <key>: <message>" there)
Json style_value(const StyleKey& spec, const Json& value, const PictureCheck& check) {
    switch (spec.kind) {
        case Kind::Corners: {
            Json corners = rounded_points(value, 4);
            const bool inside = std::all_of(corners.begin(), corners.end(), [](const Json& p) {
                return std::all_of(p.begin(), p.end(), [](const Json& c) {
                    const double v = c.get<double>();
                    return -1 <= v && v <= 2;
                });
            });
            if (corners.size() != 4 || !inside) throw PyValueError("warp is four corners [[x, y] ×4] as shares of the box (-1..2)");
            return corners;
        }
        case Kind::Cuts: {
            Json cuts = Json::array();
            for (const Json& c : iterate(value)) {
                Json points = rounded_points(subscript(c, "points"), 3);
                const Json* width = get(c, "width_mm");
                const double w = width != nullptr ? to_float(*width) : 2.0;
                cuts.push_back(Json::object({{"points", std::move(points)}, {"width_mm", py_round(py_max(0.1, py_min(40.0, w)), 3)}}));
            }
            if (std::any_of(cuts.begin(), cuts.end(), [](const Json& c) { return c["points"].empty(); })) {
                throw PyValueError("a cut needs its points");
            }
            return cuts;
        }
        case Kind::Points: {
            Json points = rounded_points(value, 3);
            if (points.size() < 2) throw PyValueError("a path needs two points or more");
            return points;
        }
        case Kind::Tags: {
            Json tags = Json::array();
            for (const Json& v : iterate(value)) tags.push_back(py_str(v));
            for (const Json& tag : tags) {
                if (!listed(kFeatures, tag.get_ref<const std::string&>())) {
                    throw PyValueError("unknown feature " + tag.get<std::string>());
                }
            }
            return tags;
        }
        case Kind::Png: {
            // str(value); Image.open(io.BytesIO(base64.b64decode(value))).verify()
            const std::string text = py_str(value);
            std::string bytes;
            try {
                bytes = a2b_base64(text);
            } catch (const Error& error) {
                throw PyValueError(error.what());  // (binascii.Error, and the ValueError of a text that is not ASCII)
            }
            if (!check) {
                not_yet_ported("a picture in a line's style (style." + std::string(spec.name) +
                               ") is checked by the drawing ops (render::ops_registry)");
            }
            check(bytes);
            return text;
        }
        case Kind::Dict: {
            if (!value.is_object()) throw PyValueError("gradient is {rgb_from, rgb_to, angle?}");
            static const Json kFrom = Json::array({20, 20, 20}), kTo = Json::array({230, 40, 40});
            const Json from = int_list3(get_else(value, "rgb_from", kFrom));
            const Json to = int_list3(get_else(value, "rgb_to", kTo));
            const Json* angle = get(value, "angle");
            return Json::object({{"rgb_from", from}, {"rgb_to", to}, {"angle", angle != nullptr ? to_float(*angle) : 90.0}});
        }
        case Kind::List: return int_list3(value);
        case Kind::Str: return py_str(value);
        case Kind::Float: return to_float(value);
        case Kind::Bool: return py_truthy(value);
        case Kind::Int: return to_int_held(value);  // (an int past 64 bits held at its end: refused by the bounds below)
    }
    return value;
}

// ops._merge_style: style keys merged into a line's style; a key set to null (or "") goes back to the default
Json merge_style(const Json& current, const Json& change, const PictureCheck& check) {
    if (!change.is_object()) throw OpError("style must be an object");
    Json out = current.is_object() ? current : Json::object();
    for (const auto& [key, value] : change.items()) {
        const auto* spec = std::find_if(std::begin(kStyleKeys), std::end(kStyleKeys), [&](const StyleKey& k) { return k.name == key; });
        if (spec == std::end(kStyleKeys)) {
            std::string names;
            for (const StyleKey& k : kStyleKeys) names += (names.empty() ? "" : ", ") + std::string(k.name);
            throw OpError("unknown style key " + key + " (one of " + names + ")");
        }
        if (value.is_null() || value == Json("")) {
            out.erase(key);
            continue;
        }
        Json kept;
        try {
            kept = style_value(*spec, value, check);
        } catch (const PyValueError& error) {
            throw OpError("style " + key + ": " + error.what());
        } catch (const PyTypeError& error) {
            throw OpError("style " + key + ": " + error.what());
        } catch (const PyUncaught& error) {
            const std::string& type = error.type();
            if (type != "IndexError" && type != "AttributeError" && type != "OSError" && type != "PIL.UnidentifiedImageError") throw;
            throw OpError("style " + key + ": " + error.what());
        }
        const auto must = [](bool ok, const char* words) {
            if (!ok) throw OpError(words);
        };
        const auto number = [&kept] { return kept.is_number_integer() ? static_cast<double>(kept.get<std::int64_t>()) : kept.get<double>(); };
        if (key == "align") {
            must(is_one_of(kept, {"top", "center", "bottom", "left", "right", "justify"}),
                 "align must be top, center, bottom, left, right or justify");
        }
        if (key == "fill") must(is_one_of(kept, {"white", "none"}), "fill must be white or none");
        if (key == "latin") must(is_one_of(kept, {"rotate", "upright"}), "latin must be rotate or upright");
        if (key == "emphasis_mark") must(is_one_of(kept, {"sesame", "dot"}), "emphasis_mark must be sesame or dot");
        if (key == "weight") must(is_one_of(kept, {"normal", "bold", "heavy"}), "weight must be normal, bold or heavy");
        if (key == "skew_deg") must(!(std::abs(number()) > 60), "skew_deg must be between -60 and 60");
        if (key == "arc") must(!(std::abs(number()) > 1), "arc must be between -1 and 1");
        if (key == "wobble") must(0 <= number() && number() <= 1, "wobble must be between 0 and 1");
        if (key == "spikes") must(6 <= number() && number() <= 80, "spikes must be between 6 and 80");
        if (key == "spike_depth") must(0.05 <= number() && number() <= 0.6, "spike_depth must be between 0.05 and 0.6");
        if (key == "scale_x") must(0.3 <= number() && number() <= 3, "scale_x must be between 0.3 and 3");
        if (key == "spike_jitter") must(0 <= number() && number() <= 1, "spike_jitter must be between 0 and 1");
        if (key == "bumps") must(5 <= number() && number() <= 60, "bumps must be between 5 and 60");
        out[key] = std::move(kept);
    }
    return out;
}

// ops.tail_hidden: whether a tail's tip lies inside its balloon (an ellipse for the round kinds, the box for the others)
bool tail_hidden(const StoryLine& line, const Json& tip) {
    const double w = line.w_mm.truthy() ? line.w_mm.value() : 40.0;
    const double h = line.h_mm.truthy() ? line.h_mm.value() : 20.0;
    const double cx = line.x_mm.value() + w / 2, cy = line.y_mm.value() + h / 2;
    const double dx = to_float(subscript(tip, 0)) - cx;
    const double dy = to_float(subscript(tip, 1)) - cy;
    const std::string kind = line.balloon.empty() ? std::string("speech") : line.balloon;
    if (kind == "box" || kind == "narration" || kind == "rounded" || kind == "none" || kind == "sfx") {
        return std::abs(dx) < w / 2 && std::abs(dy) < h / 2;
    }
    return squared(divided(dx, w / 2)) + squared(divided(dy, h / 2)) < 1.0;
}

// ops._tails_outside: each tail's tip that the balloon now covers moved out past its outline, the same way from its
// middle (a bigger or moved balloon must not swallow its tail)
void tails_outside(StoryLine& line, double beyond = 3.0) {
    const double w = line.w_mm.truthy() ? line.w_mm.value() : 40.0;
    const double h = line.h_mm.truthy() ? line.h_mm.value() : 20.0;
    const double cx = line.x_mm.value() + w / 2, cy = line.y_mm.value() + h / 2;
    std::vector<Json> tails = line.tails;  // ([dict(t) for t in line.tails], or the single tail as one)
    if (tails.empty() && line.tail) tails.push_back(Json::object({{"to", point_json(*line.tail)}}));
    bool changed = false;
    for (Json& tail : tails) {
        const Json* tip = get(tail, "to");
        if (tip == nullptr || !py_truthy(*tip) || !tail_hidden(line, *tip)) continue;
        double dx = to_float(subscript(*tip, 0)) - cx;
        double dy = to_float(subscript(*tip, 1)) - cy;
        if (std::abs(dx) + std::abs(dy) < 1e-6) {
            dx = 0.0;
            dy = 1.0;
        }
        const double length = py_hypot(dx, dy);
        const double ux = divided(dx, length), uy = divided(dy, length);
        const double edge = divided(1.0, std::sqrt(squared(divided(ux, w / 2)) + squared(divided(uy, h / 2))));  // (the outline that way)
        tail["to"] = Json::array({py_round(cx + ux * (edge + beyond), 2), py_round(cy + uy * (edge + beyond), 2)});
        changed = true;
    }
    if (changed) {
        line.tails = tails;
        line.tail = tail_point(subscript(tails[0], "to"));
    }
}

// What Python keeps that this build's book cannot hold, refused once the op has done everything Python does (so every
// error Python gives comes first): a frame_id that is not a panel's id (Python keeps a number or a list there, which
// this build's reader repairs), and a number that is not finite (Python writes NaN or Infinity into project.json).
void require_kept(const StoryLine& line, const Json* frame_id = nullptr) {
    if (frame_id != nullptr && !frame_id->is_null() && !frame_id->is_string()) throw OpError("frame_id must be a panel's id or null");
    if (line.path) {
        for (const Point& p : *line.path) {
            if (!std::isfinite(p.x.value()) || !std::isfinite(p.y.value())) throw OpError("path must be a finite number");
        }
    }
    const std::pair<const char*, const Num*> box[] = {{"x_mm", &line.x_mm}, {"y_mm", &line.y_mm}, {"w_mm", &line.w_mm}, {"h_mm", &line.h_mm}};
    for (const auto& [name, value] : box) {
        if (!std::isfinite(value->value())) throw OpError(std::string(name) + " must be a finite number");
    }
    if (line.tail && (!std::isfinite(line.tail->x.value()) || !std::isfinite(line.tail->y.value()))) {
        throw OpError("tail must be a finite number");
    }
    for (const Json& tail : line.tails) require_finite(tail, "tails");
    require_finite(line.style, "style");
}

// --- the ops -------------------------------------------------------------------------------------------------------------

void add_line(OpContext& c, const PictureCheck& check) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const Num index = doc.page(at).index;
    const Json text = op_value(op, "text");
    if (!py_truthy(text)) throw OpError("text is required");
    // episode.add_line(page.index, str(text), speaker=…, frame_id=…, …): the arguments in Python's order
    std::string words = py_str(text);
    std::string speaker = py_str(get_or(op, "speaker", Json("")));
    const Json frame_id = op_value(op, "frame_id");
    std::string ruby = py_str(get_or(op, "ruby", Json("")));
    const double x = to_float(get_or(op, "x_mm", Json(0)));
    const double y = to_float(get_or(op, "y_mm", Json(0)));
    const double w = to_float(get_or(op, "w_mm", Json(40)));
    const double h = to_float(get_or(op, "h_mm", Json(20)));
    std::string balloon = py_str(get_or(op, "balloon", Json("speech")));
    const std::optional<Point> tail = parse_tail(op_value(op, "tail"));
    std::optional<std::string> frame;
    if (frame_id.is_string()) frame = frame_id.get<std::string>();
    StoryLine& line = doc.add_line(index, std::move(words), std::move(speaker), std::move(frame), std::move(ruby), Num(x), Num(y),
                                   Num(w), Num(h), std::move(balloon), tail);
    if (truthy_at(op, "id")) {
        const std::string id = py_str(op["id"]);
        for (const StoryLine& item : doc.story) {
            if (item.id == id && &item != &line) throw OpError("line " + id + " already exists");
        }
        line.id = id;
    }
    // manga dialogue is set vertically unless asked otherwise (the app's text tool does the same)
    line.wrap = py_str(py_or(op_value(op, "wrap"), Json("vertical")));
    if (line.wrap != "vertical" && line.wrap != "horizontal") throw OpError("wrap must be vertical or horizontal");
    if (truthy_at(op, "ruby_runs")) line.ruby_runs = ruby_runs_of(op["ruby_runs"]);
    if (truthy_at(op, "emphasis_runs")) line.emphasis_runs = emphasis(op["emphasis_runs"]);
    if (truthy_at(op, "style_runs")) line.style_runs = style_runs(op["style_runs"]);
    if (truthy_at(op, "path")) set_path(line, op["path"]);
    if (truthy_at(op, "style")) line.style = merge_style(Json::object(), op["style"], check);
    if (truthy_at(op, "tails")) {
        line.tails = parse_tails(op["tails"]);
        line.tail = tail_point(line.tails.front()["to"]);
    }
    line.balloon = balloon_kind(Json(line.balloon));
    require_kept(line, &frame_id);
}

void reorder_lines(OpContext& c) {
    Document& doc = c.doc;
    const Num index = doc.page(require_page(doc, c.op)).index;
    std::vector<std::string> order;
    for (const Json& item : iterate(py_or(op_value(c.op, "order"), Json::array()))) order.push_back(py_str(item));
    std::vector<std::string> mine;
    std::map<std::string, StoryLine> by_id;  // (a dict: the last line with an id is the one kept for it)
    for (const StoryLine& line : doc.story) {
        if (!(line.page_index == index)) continue;
        mine.push_back(line.id);
        by_id[line.id] = line;
    }
    std::vector<std::string> wanted = order;
    std::sort(wanted.begin(), wanted.end());  // (UTF-8 sorts as Python sorts str: by code point)
    std::sort(mine.begin(), mine.end());
    if (wanted != mine) throw OpError("order must list every line of the page exactly once");
    // The page's lines take their places in the story in the new order. (Where an id repeats, Python puts that one line
    // object in each of its places; here each place has a copy of it.)
    std::size_t next = 0;
    for (StoryLine& line : doc.story) {
        if (line.page_index == index) line = by_id.at(order[next++]);
    }
}

void edit_line(OpContext& c, const PictureCheck& check) {
    const Json& op = c.op;
    const Json* line_id = get(op, "id");
    if (line_id == nullptr || !py_truthy(*line_id)) throw OpError("id is required");
    StoryLine& line = find_line(c.doc, py_str(*line_id));
    if (has(op, "style")) line.style = merge_style(line.style, op["style"], check);
    if (has(op, "tails")) {
        line.tails = parse_tails(op["tails"]);
        line.tail = line.tails.empty() ? std::nullopt : std::optional<Point>(tail_point(line.tails.front()["to"]));
    }
    if (has(op, "text")) line.text = py_str(op["text"]);
    if (has(op, "speaker")) line.speaker = py_str(op["speaker"]);
    if (has(op, "frame_id")) {
        line.frame_id = op["frame_id"].is_string() ? std::optional<std::string>(op["frame_id"].get<std::string>()) : std::nullopt;
    }
    if (has(op, "ruby")) line.ruby = py_str(op["ruby"]);
    if (has(op, "ruby_runs")) line.ruby_runs = ruby_runs_of(py_or(op["ruby_runs"], Json::array()));
    if (has(op, "emphasis_runs")) line.emphasis_runs = emphasis(op["emphasis_runs"]);
    if (has(op, "style_runs")) line.style_runs = style_runs(op["style_runs"]);
    if (has(op, "balloon")) line.balloon = balloon_kind(op["balloon"]);
    if (has(op, "wrap")) {
        if (!is_one_of(op["wrap"], {"vertical", "horizontal"})) throw OpError("wrap must be vertical or horizontal");
        line.wrap = py_str(op["wrap"]);
    }
    require_kept(line, get(op, "frame_id"));
}

// フキダシ消しゴム: part of the balloon taken away where the eraser went (its points kept from the box's corner)
void cut_balloon(OpContext& c, const PictureCheck& check) {
    const Json& op = c.op;
    StoryLine& line = find_line(c.doc, py_str(py_or(op_value(op, "id"), Json(""))));
    // ops._parse_points: [x, y] or [x, y, pressure] each
    std::vector<std::pair<double, double>> points;
    for (const Json& item : iterate(py_or(op_value(op, "points"), Json::array()))) {
        const std::size_t n = length(item);
        if (n < 2) throw OpError("points needs [x_mm, y_mm]");
        const double x = to_float(subscript(item, 0));
        const double y = to_float(subscript(item, 1));
        if (n >= 3) (void)to_float(subscript(item, 2));
        points.emplace_back(x, y);
    }
    if (points.empty()) throw OpError("points: where the eraser went over the balloon");
    Json cut_points = Json::array();
    for (const auto& [x, y] : points) {
        cut_points.push_back(Json::array({py_round(x - line.x_mm.value(), 3), py_round(y - line.y_mm.value(), 3)}));
    }
    const Json* width = get(op, "width_mm");
    const double w = py_round(py_max(0.1, py_min(40.0, width != nullptr ? to_float(*width) : 2.0)), 3);
    Json cuts = Json::array();
    if (const Json* existing = get(line.style, "cuts"); existing != nullptr && py_truthy(*existing)) {
        for (const Json& item : iterate(*existing)) cuts.push_back(item);
    }
    cuts.push_back(Json::object({{"points", std::move(cut_points)}, {"width_mm", w}}));
    line.style = merge_style(line.style, Json::object({{"cuts", std::move(cuts)}}), check);
    require_kept(line);
}

void move_line(OpContext& c) {
    const Json& op = c.op;
    const Json* line_id = get(op, "id");
    if (line_id == nullptr || !py_truthy(*line_id)) throw OpError("id is required");
    StoryLine& line = find_line(c.doc, py_str(*line_id));
    if (has(op, "x_mm")) line.x_mm = Num(to_float(op["x_mm"]));
    if (has(op, "y_mm")) line.y_mm = Num(to_float(op["y_mm"]));
    if (has(op, "w_mm")) line.w_mm = Num(to_float(op["w_mm"]));
    if (has(op, "h_mm")) line.h_mm = Num(to_float(op["h_mm"]));
    if (has(op, "tail")) {
        line.tail = parse_tail(op["tail"]);
        line.tails.clear();
        if (line.tail) line.tails.push_back(Json::object({{"to", point_json(*line.tail)}}));
    }
    if (has(op, "tails")) {
        line.tails = parse_tails(op["tails"]);
        line.tail = line.tails.empty() ? std::nullopt : std::optional<Point>(tail_point(line.tails.front()["to"]));
    }
    if (has(op, "balloon")) line.balloon = balloon_kind(op["balloon"]);
    const bool moved = has(op, "x_mm") || has(op, "y_mm") || has(op, "w_mm") || has(op, "h_mm");
    if (!(has(op, "tail") || has(op, "tails")) && moved) tails_outside(line);
    require_kept(line);
}

void delete_line(OpContext& c) {
    const Json* line_id = get(c.op, "id");
    if (line_id == nullptr || !py_truthy(*line_id)) throw OpError("id is required");
    const std::size_t before = c.doc.story.size();
    // (line.id != line_id: the id as it is given — a number never matches a line's id)
    if (line_id->is_string()) {
        std::erase_if(c.doc.story, [&](const StoryLine& line) { return line.id == line_id->get_ref<const std::string&>(); });
    }
    if (c.doc.story.size() == before) throw OpError("no line " + py_str(*line_id));
}

void set_balloon_path(OpContext& c) {
    const Json& op = c.op;
    StoryLine& line = find_line(c.doc, py_str(py_or(op_value(op, "id"), Json(""))));
    if (has(op, "path")) set_path(line, op["path"]);
    if (has(op, "wrap")) line.wrap = py_str(op["wrap"]);
    if (has(op, "ruby_runs")) line.ruby_runs = ruby_runs_of(op["ruby_runs"]);
    if (has(op, "emphasis_runs")) line.emphasis_runs = emphasis(op["emphasis_runs"]);
    if (has(op, "style_runs")) line.style_runs = style_runs(op["style_runs"]);
    require_kept(line);
}

// The first `count` characters of a text (Python's text[:count], by code point)
std::string first_characters(const std::string& text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t n = 0; n < count && at < text.size(); ++n) {
        const auto c = static_cast<unsigned char>(text[at]);
        at += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    }
    return text.substr(0, std::min(at, text.size()));
}

// `words in text` for a str on the left: PyTypeError for anything else, as Python's `in` gives it
bool found_in(const Json& words, const std::string& text) {
    if (!words.is_string()) {
        throw PyTypeError("'in <string>' requires string as left operand, not " + py_type_name(words));
    }
    return text.find(words.get_ref<const std::string&>()) != std::string::npos;
}

// bookops.replace_text (一括置換): every line's words (and speakers, if asked) with `find` swapped for `replace`; `pages`
// limits it; how many and where go back in the op's report. A pattern of re's own syntax ("regex") is not here yet.
void replace_text(OpContext& c) {
    const Json& op = c.op;
    const std::string find = py_str(py_or(op_value(op, "find"), Json("")));
    if (find.empty()) throw OpError("find is the words to look for");
    const std::string replace = py_str(py_or(op_value(op, "replace"), Json("")));
    const bool ignore_case = !py_truthy(get_or(op, "case", Json(true)));
    if (truthy_at(op, "regex")) not_yet_ported("replace_text with regex (a pattern in Python's re syntax) is not in the C++ build yet");
    const PyLiteralPattern pattern(find, ignore_case);  // (re.compile(re.escape(find), flags))
    std::vector<std::int64_t> pages;
    for (const Json& p : iterate(py_or(op_value(op, "pages"), Json::array()))) pages.push_back(to_int_held(p));
    std::int64_t count = 0;
    Json where = Json::array();
    for (StoryLine& line : c.doc.story) {
        if (!pages.empty() && std::none_of(pages.begin(), pages.end(), [&](std::int64_t p) { return line.page_index == Num(p); })) {
            continue;
        }
        auto [text, n] = pattern.subn(replace, line.text);
        if (n > 0) {
            where.push_back(Json::object({{"line", line.id}, {"page", line.page_index.json()}, {"count", n},
                                          {"text", first_characters(text, 40)}}));
            line.text = text;
            count += n;
            // (ruby, dots and styled parts keep pointing at words that are still there)
            std::vector<Json> ruby;
            for (const Json& run : line.ruby_runs) {
                if (py_truthy(run) && found_in(subscript(run, 0), text)) ruby.push_back(run);
            }
            line.ruby_runs = std::move(ruby);
            std::erase_if(line.emphasis_runs, [&](const std::string& words) { return text.find(words) == std::string::npos; });
            std::erase_if(line.style_runs, [&](const auto& run) { return text.find(run.first) == std::string::npos; });
        }
        if (truthy_at(op, "speakers")) {
            auto [speaker, m] = pattern.subn(replace, line.speaker);
            if (m > 0) {
                where.push_back(Json::object({{"line", line.id}, {"page", line.page_index.json()}, {"count", m}, {"speaker", speaker}}));
                line.speaker = speaker;
                count += m;
            }
        }
    }
    if (count == 0 && truthy_at(op, "must_find")) throw OpError("nothing matched");
    c.report = Json::object({{"replaced", count}, {"where", py_slice(where, 0, 200)}});
}

}  // namespace

void register_line_ops(OpRegistry& registry, PictureCheck check) {
    registry.add("add_line", [check](OpContext& c) { add_line(c, check); });
    registry.add("edit_line", [check](OpContext& c) { edit_line(c, check); });
    registry.add("cut_balloon", [check](OpContext& c) { cut_balloon(c, check); });
    registry.add("move_line", move_line);
    registry.add("delete_line", delete_line);
    registry.add("reorder_lines", reorder_lines);
    registry.add("set_balloon_path", set_balloon_path);
    registry.add("replace_text", replace_text);
}

}  // namespace genko::core
