#include "core/vector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::core::vector {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", why); }

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

// A number written to 4 decimals (0.1 µm)
Json num(double v) { return py_round(v, 4); }

double number(const Json& value, std::string_view what) {
    if (!value.is_number() || value.is_boolean()) bad(std::string(what) + " must be a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) bad(std::string(what) + " must be a finite number");
    return v;
}

double coordinate(const Json& value, std::string_view what) {
    const double v = number(value, what);
    if (std::fabs(v) > kCoordinateMax) bad(std::string(what) + " is too far (" + std::to_string(static_cast<long long>(kCoordinateMax)) + " mm at most)");
    return v;
}

double in_range(const Json& value, double lo, double hi, std::string_view what) {
    const double v = number(value, what);
    if (v < lo || v > hi) bad(std::string(what) + " must be " + py_repr(Json(lo)) + ".." + py_repr(Json(hi)));
    return v;
}

bool boolean(const Json& value, std::string_view what) {
    if (!value.is_boolean()) bad(std::string(what) + " must be true or false");
    return value.get<bool>();
}

const Json& object_of(const Json& value, std::string_view what) {
    if (!value.is_object()) bad(std::string(what) + " must be an object");
    return value;
}

// Every key of `value` one of `keys`.
void only_keys(const Json& value, std::initializer_list<std::string_view> keys, std::string_view what) {
    for (const auto& [key, item] : value.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) bad(std::string(what) + " has no key " + py_repr_str(key));
    }
}

const Json* field(const Json& value, std::string_view key) {
    const auto it = value.find(std::string(key));
    return it == value.end() ? nullptr : &*it;
}

XY xy_of(const Json& value, std::string_view what) {
    if (!value.is_array() || value.size() != 2) bad(std::string(what) + " must be [x, y]");
    return XY{coordinate(value[0], what), coordinate(value[1], what)};
}

Json xy_json(const XY& p) { return Json::array({num(p.x), num(p.y)}); }

XY map(const Affine& m, const XY& p) { return XY{m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]}; }

template <class E, std::size_t N>
std::string_view name_of(E value, const std::array<std::string_view, N>& names) {
    return names[static_cast<std::size_t>(value)];
}

template <class E, std::size_t N>
E enum_of(const Json& value, const std::array<std::string_view, N>& names, std::string_view what) {
    if (value.is_string()) {
        for (std::size_t i = 0; i < N; ++i)
            if (names[i] == value.get_ref<const std::string&>()) return static_cast<E>(i);
    }
    std::string list;
    for (std::size_t i = 0; i < N; ++i) list += (i == 0 ? "" : i + 1 == N ? " or " : ", ") + std::string(names[i]);
    bad(std::string(what) + " must be " + list);
}

constexpr std::array<std::string_view, 4> kOps{"combine", "subtract", "intersect", "exclude"};
constexpr std::array<std::string_view, 2> kRules{"nonzero", "evenodd"};
constexpr std::array<std::string_view, 3> kAligns{"inside", "center", "outside"};
constexpr std::array<std::string_view, 3> kCaps{"butt", "round", "square"};
constexpr std::array<std::string_view, 3> kJoins{"miter", "round", "bevel"};
constexpr std::array<std::string_view, 4> kLiveKinds{"rect", "ellipse", "polygon", "line"};

Knot knot_of(const Json& value, std::string_view what) {
    if (value.is_array()) {
        const XY p = xy_of(value, what);
        return Knot::corner(p.x, p.y);
    }
    object_of(value, what);
    only_keys(value, {"anchor", "in", "out", "smooth"}, what);
    const Json* anchor = field(value, "anchor");
    if (anchor == nullptr) bad(std::string(what) + " needs its anchor");
    Knot k;
    k.anchor = xy_of(*anchor, what);
    k.in = field(value, "in") != nullptr ? xy_of(value["in"], what) : k.anchor;
    k.out = field(value, "out") != nullptr ? xy_of(value["out"], what) : k.anchor;
    k.smooth = field(value, "smooth") != nullptr && boolean(value["smooth"], "a knot's smooth");
    return k;
}

Json knot_json(const Knot& k) {
    if (k.is_corner() && !k.smooth) return xy_json(k.anchor);
    Json out = Json::object();
    out["anchor"] = xy_json(k.anchor);
    if (!(k.in == k.anchor)) out["in"] = xy_json(k.in);
    if (!(k.out == k.anchor)) out["out"] = xy_json(k.out);
    if (k.smooth) out["smooth"] = true;
    return out;
}

}  // namespace

std::vector<std::array<XY, 4>> Subpath::segments() const {
    std::vector<std::array<XY, 4>> out;
    const std::size_t n = knots.size();
    if (n < 2) return out;
    const std::size_t count = closed ? n : n - 1;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Knot& a = knots[i];
        const Knot& b = knots[(i + 1) % n];
        out.push_back({a.anchor, a.out, b.in, b.anchor});
    }
    return out;
}

bool Path::empty() const {
    return std::all_of(subpaths.begin(), subpaths.end(), [](const Subpath& s) { return s.knots.empty(); });
}

std::size_t Path::knot_count() const {
    std::size_t n = 0;
    for (const Subpath& s : subpaths) n += s.knots.size();
    return n;
}

std::optional<std::array<double, 4>> Path::control_bounds() const {
    std::optional<std::array<double, 4>> out;
    for (const Subpath& s : subpaths) {
        for (const Knot& k : s.knots) {
            for (const XY& p : {k.anchor, k.in, k.out}) {
                if (!out) {
                    out = std::array<double, 4>{p.x, p.y, p.x, p.y};
                    continue;
                }
                auto& b = *out;
                b[0] = std::min(b[0], p.x);
                b[1] = std::min(b[1], p.y);
                b[2] = std::max(b[2], p.x);
                b[3] = std::max(b[3], p.y);
            }
        }
    }
    return out;
}

Path Path::transformed(const Affine& m) const {
    Path out = *this;
    for (Subpath& s : out.subpaths) {
        for (Knot& k : s.knots) {
            k.anchor = map(m, k.anchor);
            k.in = map(m, k.in);
            k.out = map(m, k.out);
        }
    }
    return out;
}

const ShapeListPtr& empty_shapes() {
    static const ShapeListPtr empty = std::make_shared<const ShapeList>();
    return empty;
}

ShapeListPtr make_shapes(std::vector<ShapePart> items, std::string blob_ref) {
    if (items.empty() && blob_ref.empty()) return empty_shapes();
    return std::make_shared<const ShapeList>(ShapeList{std::move(items), std::move(blob_ref)});
}

std::string_view to_string(PathOp op) { return name_of(op, kOps); }
std::string_view to_string(FillRule rule) { return name_of(rule, kRules); }
std::optional<PathOp> path_op_from(std::string_view text) {
    for (std::size_t i = 0; i < kOps.size(); ++i)
        if (kOps[i] == text) return static_cast<PathOp>(i);
    return std::nullopt;
}

Json to_json(const Path& path) {
    Json subpaths = Json::array();
    for (const Subpath& s : path.subpaths) {
        Json item = Json::object();
        item["closed"] = s.closed;
        item["op"] = to_string(s.op);
        Json knots = Json::array();
        for (const Knot& k : s.knots) knots.push_back(knot_json(k));
        item["knots"] = std::move(knots);
        subpaths.push_back(std::move(item));
    }
    Json out = Json::object();
    out["subpaths"] = std::move(subpaths);
    out["fill_rule"] = to_string(path.fill_rule);
    out["inverted"] = path.inverted;
    return out;
}

Path path_from_json(const Json& value) {
    object_of(value, "a path");
    only_keys(value, {"subpaths", "fill_rule", "inverted"}, "a path");
    Path out;
    const Json* subpaths = field(value, "subpaths");
    if (subpaths == nullptr || !subpaths->is_array()) bad("a path's subpaths must be a list");
    std::size_t knots = 0;
    for (const Json& item : *subpaths) {
        object_of(item, "a subpath");
        only_keys(item, {"closed", "op", "knots"}, "a subpath");
        Subpath s;
        if (const Json* closed = field(item, "closed")) s.closed = boolean(*closed, "a subpath's closed");
        if (const Json* op = field(item, "op")) s.op = enum_of<PathOp>(*op, kOps, "a subpath's op");
        const Json* list = field(item, "knots");
        if (list == nullptr || !list->is_array()) bad("a subpath's knots must be a list");
        knots += list->size();
        if (knots > kKnotsMax) bad("a path has at most " + std::to_string(kKnotsMax) + " knots");
        for (const Json& k : *list) s.knots.push_back(knot_of(k, "a knot"));
        out.subpaths.push_back(std::move(s));
    }
    if (const Json* rule = field(value, "fill_rule")) out.fill_rule = enum_of<FillRule>(*rule, kRules, "a path's fill_rule");
    if (const Json* inverted = field(value, "inverted")) out.inverted = boolean(*inverted, "a path's inverted");
    return out;
}

void check_fill(const Json& fill, std::string_view what) {
    if (fill.is_null()) return;
    object_of(fill, what);
    if (field(fill, "pattern") != nullptr) bad(std::string(what) + ": a pattern fill is not in this build yet");
    if (const Json* gradient = field(fill, "gradient")) {
        only_keys(fill, {"gradient"}, what);
        object_of(*gradient, std::string(what) + "'s gradient");
        return;
    }
    only_keys(fill, {"rgb", "color_rgb"}, what);
    const Json* rgb = field(fill, "rgb");
    if (rgb == nullptr) bad(std::string(what) + " is null, {rgb} or {gradient}");
    if (rgb->is_null()) return;
    if (!rgb->is_array() || rgb->size() != 3) bad(std::string(what) + "'s rgb must be [r, g, b]");
    for (const Json& c : *rgb) {
        if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) bad(std::string(what) + "'s rgb must be 0..255");
    }
}

Json to_json(const ShapeStroke& s) {
    Json out = Json::object();
    out["width_mm"] = num(s.width_mm);
    out["fill"] = s.fill;
    if (s.opacity != 1.0) out["opacity"] = num(s.opacity);
    if (s.align != StrokeAlign::Center) out["align"] = name_of(s.align, kAligns);
    if (s.cap != LineCap::Butt) out["cap"] = name_of(s.cap, kCaps);
    if (s.join != LineJoin::Miter) out["join"] = name_of(s.join, kJoins);
    if (s.miter_limit != 100.0) out["miter_limit"] = num(s.miter_limit);
    if (!s.dashes.empty()) {
        Json dashes = Json::array();
        for (const double d : s.dashes) dashes.push_back(num(d));
        out["dashes"] = std::move(dashes);
    }
    if (s.dash_offset != 0.0) out["dash_offset"] = num(s.dash_offset);
    return out;
}

ShapeStroke stroke_from_json(const Json& value) {
    object_of(value, "a shape's stroke");
    only_keys(value, {"width_mm", "fill", "opacity", "align", "cap", "join", "miter_limit", "dashes", "dash_offset"}, "a shape's stroke");
    ShapeStroke s;
    if (const Json* w = field(value, "width_mm")) s.width_mm = in_range(*w, 0.0, kStrokeWidthMax, "a shape's stroke width_mm");
    if (const Json* fill = field(value, "fill")) {
        check_fill(*fill, "a shape's stroke fill");
        s.fill = *fill;
    }
    if (const Json* o = field(value, "opacity")) s.opacity = in_range(*o, 0.0, 1.0, "a shape's stroke opacity");
    if (const Json* a = field(value, "align")) s.align = enum_of<StrokeAlign>(*a, kAligns, "a shape's stroke align");
    if (const Json* c = field(value, "cap")) s.cap = enum_of<LineCap>(*c, kCaps, "a shape's stroke cap");
    if (const Json* j = field(value, "join")) s.join = enum_of<LineJoin>(*j, kJoins, "a shape's stroke join");
    if (const Json* m = field(value, "miter_limit")) s.miter_limit = in_range(*m, 1.0, 1000.0, "a shape's stroke miter_limit");
    if (const Json* dashes = field(value, "dashes")) {
        if (!dashes->is_array() || dashes->size() > 64) bad("a shape's stroke dashes must be a list of at most 64 lengths");
        for (const Json& d : *dashes) s.dashes.push_back(in_range(d, 0.0, 1000.0, "a shape's stroke dash"));
    }
    if (const Json* o = field(value, "dash_offset")) s.dash_offset = in_range(*o, -1000.0, 1000.0, "a shape's stroke dash_offset");
    return s;
}

Json to_json(const LiveShape& live) {
    Json out = Json::object();
    out["kind"] = name_of(live.kind, kLiveKinds);
    const auto four = [](const std::array<double, 4>& v) { return Json::array({num(v[0]), num(v[1]), num(v[2]), num(v[3])}); };
    switch (live.kind) {
        case LiveShape::Kind::Rect:
            out["rect"] = four(live.rect);
            out["radii"] = four(live.radii);
            break;
        case LiveShape::Kind::Ellipse:
            out["rect"] = four(live.rect);
            break;
        case LiveShape::Kind::Polygon:
            out["rect"] = four(live.rect);
            out["sides"] = live.sides;
            out["star_ratio"] = num(live.star_ratio);
            if (live.angle != 0.0) out["angle"] = num(live.angle);
            break;
        case LiveShape::Kind::Line:
            out["from"] = xy_json(live.from);
            out["to"] = xy_json(live.to);
            out["weight_mm"] = num(live.weight_mm);
            break;
    }
    return out;
}

LiveShape live_from_json(const Json& value) {
    object_of(value, "a live shape");
    const Json* kind = field(value, "kind");
    if (kind == nullptr) bad("a live shape needs its kind");
    LiveShape live;
    live.kind = enum_of<LiveShape::Kind>(*kind, kLiveKinds, "a live shape's kind");
    const auto four = [](const Json& v, std::string_view what, bool size) {
        if (!v.is_array() || v.size() != 4) bad(std::string(what) + " must be four numbers");
        std::array<double, 4> out{};
        for (std::size_t i = 0; i < 4; ++i) out[i] = coordinate(v[i], what);
        if (size && (out[2] < 0 || out[3] < 0)) bad(std::string(what) + "'s width and height must not be negative");
        return out;
    };
    const auto need = [&](std::string_view key) -> const Json& {
        const Json* v = field(value, key);
        if (v == nullptr) bad("a live " + std::string(name_of(live.kind, kLiveKinds)) + " needs its " + std::string(key));
        return *v;
    };
    switch (live.kind) {
        case LiveShape::Kind::Rect:
            only_keys(value, {"kind", "rect", "radii"}, "a live rect");
            live.rect = four(need("rect"), "a live shape's rect", true);
            if (field(value, "radii") != nullptr) {
                live.radii = four(value["radii"], "a live rect's radii", false);
                for (const double r : live.radii)
                    if (r < 0) bad("a live rect's radii must not be negative");
            }
            break;
        case LiveShape::Kind::Ellipse:
            only_keys(value, {"kind", "rect"}, "a live ellipse");
            live.rect = four(need("rect"), "a live shape's rect", true);
            break;
        case LiveShape::Kind::Polygon:
            only_keys(value, {"kind", "rect", "sides", "star_ratio", "angle"}, "a live polygon");
            live.rect = four(need("rect"), "a live shape's rect", true);
            live.sides = static_cast<int>(in_range(need("sides"), 3, 100, "a live polygon's sides"));
            if (need("sides").is_number_float()) bad("a live polygon's sides must be a whole number");
            live.star_ratio = field(value, "star_ratio") != nullptr ? in_range(value["star_ratio"], 0.0, 1.0, "a live polygon's star_ratio") : 1.0;
            live.angle = field(value, "angle") != nullptr ? in_range(value["angle"], -360.0, 360.0, "a live polygon's angle") : 0.0;
            break;
        case LiveShape::Kind::Line:
            only_keys(value, {"kind", "from", "to", "weight_mm"}, "a live line");
            live.from = xy_of(need("from"), "a live line's from");
            live.to = xy_of(need("to"), "a live line's to");
            live.weight_mm = in_range(need("weight_mm"), 0.0, kStrokeWidthMax, "a live line's weight_mm");
            break;
    }
    return live;
}

Json to_json(const ShapePart& part) {
    Json out = Json::object();
    out["id"] = part.id;
    if (part.name) out["name"] = *part.name;
    out["path"] = to_json(part.path);
    out["fill"] = part.fill;
    out["stroke"] = part.stroke ? to_json(*part.stroke) : Json(nullptr);
    if (part.live) out["live"] = to_json(*part.live);
    if (part.opacity != 1.0) out["opacity"] = num(part.opacity);
    if (part.over) out["over"] = true;
    return out;
}

ShapePart part_from_json(const Json& value) {
    object_of(value, "a shape part");
    only_keys(value, {"id", "name", "path", "fill", "stroke", "live", "opacity", "over"}, "a shape part");
    ShapePart part;
    const Json* id = field(value, "id");
    if (id == nullptr || !id->is_string() || id->get_ref<const std::string&>().empty() || id->get_ref<const std::string&>().size() > 64) {
        bad("a shape part's id must be text (1..64 bytes)");
    }
    part.id = id->get<std::string>();
    if (const Json* name = field(value, "name"); name != nullptr && !name->is_null()) {
        if (!name->is_string()) bad("a shape part's name must be text");
        part.name = path_name(name->get<std::string>());
    }
    const Json* path = field(value, "path");
    if (path == nullptr) bad("a shape part needs its path");
    part.path = path_from_json(*path);
    if (const Json* fill = field(value, "fill")) {
        check_fill(*fill, "a shape part's fill");
        part.fill = *fill;
    }
    if (const Json* stroke = field(value, "stroke"); stroke != nullptr && !stroke->is_null()) part.stroke = stroke_from_json(*stroke);
    if (const Json* live = field(value, "live"); live != nullptr && !live->is_null()) part.live = live_from_json(*live);
    if (const Json* opacity = field(value, "opacity")) part.opacity = in_range(*opacity, 0.0, 1.0, "a shape part's opacity");
    if (const Json* over = field(value, "over")) part.over = boolean(*over, "a shape part's over");
    return part;
}

Json to_json(const VectorMask& mask) {
    Json out = Json::object();
    out["path"] = to_json(mask.path);
    if (!mask.enabled) out["enabled"] = false;
    if (!mask.linked) out["linked"] = false;
    if (mask.density != 1.0) out["density"] = num(mask.density);
    if (mask.feather_mm != 0.0) out["feather_mm"] = num(mask.feather_mm);
    return out;
}

VectorMask vector_mask_from_json(const Json& value) {
    object_of(value, "a vector mask");
    only_keys(value, {"path", "enabled", "linked", "density", "feather_mm"}, "a vector mask");
    VectorMask out;
    const Json* path = field(value, "path");
    if (path == nullptr) bad("a vector mask needs its path");
    out.path = path_from_json(*path);
    if (const Json* v = field(value, "enabled")) out.enabled = boolean(*v, "a vector mask's enabled");
    if (const Json* v = field(value, "linked")) out.linked = boolean(*v, "a vector mask's linked");
    if (const Json* v = field(value, "density")) out.density = in_range(*v, 0.0, 1.0, "a vector mask's density");
    if (const Json* v = field(value, "feather_mm")) out.feather_mm = in_range(*v, 0.0, kFeatherMax, "a vector mask's feather_mm");
    return out;
}

Json to_json(const std::vector<NamedPath>& paths) {
    Json out = Json::array();
    for (const NamedPath& p : paths) out.push_back(Json::object({{"name", p.name}, {"path", to_json(p.path)}}));
    return out;
}

std::vector<NamedPath> paths_from_json(const Json& value) {
    if (!value.is_array()) bad("a page's paths must be a list");
    if (value.size() > kPathsMax) bad("a page has at most " + std::to_string(kPathsMax) + " saved paths");
    std::vector<NamedPath> out;
    std::set<std::string> names;
    for (const Json& item : value) {
        object_of(item, "a saved path");
        only_keys(item, {"name", "path"}, "a saved path");
        const Json* name = field(item, "name");
        if (name == nullptr || !name->is_string()) bad("a saved path's name must be text");
        NamedPath p;
        p.name = path_name(name->get<std::string>());
        if (p.name != name->get<std::string>()) bad("a saved path's name has spaces around it");
        if (!names.insert(p.name).second) bad("two saved paths of a page are named " + py_repr_str(p.name));
        const Json* path = field(item, "path");
        if (path == nullptr) bad("a saved path needs its path");
        p.path = path_from_json(*path);
        out.push_back(std::move(p));
    }
    return out;
}

Json to_json(const ClippingPath& clip) {
    Json out = Json::object();
    out["name"] = clip.name;
    out["flatness"] = num(clip.flatness);
    return out;
}

ClippingPath clipping_from_json(const Json& value) {
    object_of(value, "a clipping path");
    only_keys(value, {"name", "flatness"}, "a clipping path");
    ClippingPath out;
    const Json* name = field(value, "name");
    if (name == nullptr || !name->is_string()) bad("a clipping path's name must be text");
    out.name = name->get<std::string>();
    if (const Json* f = field(value, "flatness")) out.flatness = in_range(*f, 0.0, 100.0, "a clipping path's flatness");
    return out;
}

std::string shapes_blob(const ShapeList& shapes) {
    Json list = Json::array();
    for (const ShapePart& part : shapes.items) list.push_back(to_json(part));
    return dump_canonical(list);
}

std::vector<ShapePart> shapes_from_blob(std::string_view bytes) {
    ParseOptions options;
    options.bytes = true;
    const Json value = parse_python_json(bytes, nullptr, options);
    if (!value.is_array()) bad("a shapes blob must be a list of shape parts");
    if (value.size() > kShapePartsMax) bad("a pen layer has at most " + std::to_string(kShapePartsMax) + " shape parts");
    std::vector<ShapePart> out;
    std::set<std::string> ids;
    for (const Json& item : value) {
        ShapePart part = part_from_json(item);
        if (!ids.insert(part.id).second) bad("two shape parts of a layer have the id " + py_repr_str(part.id));
        out.push_back(std::move(part));
    }
    return out;
}

std::string path_name(std::string_view name) {
    std::string out = py_strip(name);
    const std::size_t n = code_points(out);
    if (n == 0) bad("a path's name cannot be empty");
    if (n > kPathNameMax) bad("a path's name is " + std::to_string(kPathNameMax) + " characters at most");
    return out;
}

bool uses_vector(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const Page& page = doc.page(i);
        if (!page.paths.empty() || page.work_path || page.clipping_path) return true;
        for (const Layer& layer : page.layers)
            if (layer.vector_mask) return true;
    }
    return false;
}

bool uses_pen_shapes(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        for (const Layer& layer : doc.page(i).layers)
            if (layer.shapes && !layer.shapes->items.empty()) return true;
    }
    return false;
}

bool uses_transform(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        for (const Layer& layer : doc.page(i).layers)
            if (layer.link || layer.lock_position) return true;
    }
    // (PC3 C-c: a line of dialogue's warp of PhotoCraft's styles, Warp Text)
    for (const StoryLine& line : doc.story)
        if (line.style.is_object() && line.style.contains("native_warp")) return true;
    return false;
}

bool uses_page_spec(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i) && doc.page(i).own_spec) return true;
    }
    return false;
}

void validate_vector(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const Page& page = doc.page(i);
        const std::string where = "page " + py_str(page.index.json()) + ": ";
        if (page.paths.size() > kPathsMax) bad(where + "a page has at most " + std::to_string(kPathsMax) + " saved paths");
        std::set<std::string> names;
        for (const NamedPath& p : page.paths) {
            if (path_name(p.name) != p.name) bad(where + "a saved path's name has spaces around it");
            if (!names.insert(p.name).second) bad(where + "two saved paths are named " + py_repr_str(p.name));
            if (p.path.knot_count() > kKnotsMax) bad(where + "a path has at most " + std::to_string(kKnotsMax) + " knots");
        }
        if (page.work_path && page.work_path->knot_count() > kKnotsMax) bad(where + "the work path has too many knots");
        if (page.clipping_path && !names.contains(page.clipping_path->name)) {
            bad(where + "the clipping path " + py_repr_str(page.clipping_path->name) + " is not one of the page's saved paths");
        }
        if (page.own_spec) {
            const PageSpec& s = page.spec;
            const auto positive = [&](const Num& v, std::string_view what) {
                if (!std::isfinite(v.value()) || v.value() <= 0) bad(where + "its own paper's " + std::string(what) + " must be a positive number");
            };
            positive(s.width_mm, "width_mm");
            positive(s.height_mm, "height_mm");
            positive(s.dpi, "dpi");
            if (s.trim_origin_mm) {
                const auto [x, y] = *s.trim_origin_mm;
                if (!std::isfinite(x) || !std::isfinite(y)) bad(where + "its own paper's trim_origin_mm must be finite");
            }
        }
        for (const Layer& layer : page.layers) {
            if (layer.shapes && !layer.shapes->items.empty()) {
                if (layer.kind != LayerKind::Strokes) bad(where + "the layer " + layer.id + " holds shape parts but is not a pen layer");
                if (layer.shapes->items.size() > kShapePartsMax) bad(where + "the layer " + layer.id + " has too many shape parts");
                std::set<std::string> ids;
                for (const ShapePart& part : layer.shapes->items) {
                    if (!ids.insert(part.id).second) bad(where + "two shape parts of the layer " + layer.id + " have the id " + py_repr_str(part.id));
                    if (part.path.knot_count() > kKnotsMax) bad(where + "a shape part has too many knots");
                    check_fill(part.fill, "a shape part's fill");
                    if (part.stroke) check_fill(part.stroke->fill, "a shape part's stroke fill");
                    if (!(part.opacity >= 0 && part.opacity <= 1)) bad(where + "a shape part's opacity must be 0..1");
                }
            }
            if (layer.vector_mask) {
                const VectorMask& m = *layer.vector_mask;
                if (!(m.density >= 0 && m.density <= 1)) bad(where + "a vector mask's density must be 0..1");
                if (!(m.feather_mm >= 0 && m.feather_mm <= kFeatherMax)) bad(where + "a vector mask's feather_mm is out of range");
                if (m.path.knot_count() > kKnotsMax) bad(where + "a vector mask has too many knots");
            }
            if (layer.link && (layer.link->empty() || layer.link->size() > 64)) bad(where + "a layer's link must be text (1..64 bytes)");
        }
    }
    for (const StoryLine& line : doc.story) {
        if (!line.style.is_object()) continue;
        if (const auto warp = line.style.find("native_warp"); warp != line.style.end()) check_native_warp(*warp);
    }
}

void check_native_warp(const Json& value) {
    static constexpr std::array<std::string_view, 15> kStyles{"arc",  "arcLower", "arcUpper", "arch",    "bulge",   "shellLower", "shellUpper", "flag",
                                                              "wave", "fish",     "rise",     "fisheye", "inflate", "squeeze",    "twist"};
    const std::string what = "a line's native_warp";
    if (!value.is_object()) bad(what + " is {style, bend, h_distort, v_distort, vertical}");
    for (const auto& [key, v] : value.items()) {
        if (key == "style") {
            if (!v.is_string() || std::find(kStyles.begin(), kStyles.end(), v.get<std::string>()) == kStyles.end())
                bad(what + "'s style is one of arc, arcLower, arcUpper, arch, bulge, shellLower, shellUpper, flag, wave, fish, rise, fisheye, inflate, "
                           "squeeze, twist");
        } else if (key == "bend" || key == "h_distort" || key == "v_distort") {
            const double x = number(v, what + "'s " + key);
            if (x < -100 || x > 100) bad(what + "'s " + key + " must be between -100 and 100");
        } else if (key == "vertical") {
            if (!v.is_boolean()) bad(what + "'s vertical is true or false");
        } else {
            bad(what + " has no key " + key);
        }
    }
    if (!value.contains("style")) bad(what + " needs its style");
}

}  // namespace genko::core::vector
