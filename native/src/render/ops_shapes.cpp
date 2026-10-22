// pen-shapes@1 (PC3 C-b, the user's decision 7, SPEC.md COLOR-07「シェイプ」): a pen layer's shape parts — add_shape_part
// (shape.create, shape.presets.place, select.convertToShape), edit_shape_part (shape.edit, layer.combineShapes.* inside one
// part, mergeShapeComponents, shape.rasterize: to patch, to lines and fill), combine_shape_parts (Combine Shapes over
// several parts), move_shape_parts (between pen layers) — and the parts in the eraser (erase) and in the line editor
// (vector_edit). PhotoCraft v0.2.0's engine/src/vector_cmds.rs (300-480: live shapes, fills and strokes, shape.create,
// shape.edit; 1011-1050: merge_components, selection_to_shape) in mm (MIT, native/third_party/photocraft/NOTICE.md).

#include "render/ops_shapes.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/areas.hpp"
#include "core/brushes.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/rulers.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/path_raster.hpp"
#include "render/path_trace.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpContext;
using core::OpError;
using core::Page;
namespace vec = core::vector;
namespace sp = shape_parts;

const Json* given(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr && !value->is_null() ? value : nullptr;
}

double number(const Json& value, std::string_view key, double lo, double hi) {
    const double v = core::finite_float(value, key);
    if (v < lo || v > hi) throw OpError(std::string(key) + " must be " + core::py_repr(Json(lo)) + ".." + core::py_repr(Json(hi)));
    return v;
}

bool boolean(const Json& value, std::string_view key) {
    if (!value.is_boolean()) throw OpError(std::string(key) + " must be true or false");
    return value.get<bool>();
}

double coordinate(const Json& value, std::string_view key) { return number(value, key, -vec::kCoordinateMax, vec::kCoordinateMax); }

vec::XY point_of(const Json& value, std::string_view key) {
    if (!value.is_array() || value.size() != 2) throw OpError(std::string(key) + " is [x, y] (mm)");
    return vec::XY{coordinate(value[0], key), coordinate(value[1], key)};
}

// [x, y, w, h] (mm): a box dragged up or left (a negative side) turned the right way round.
std::array<double, 4> rect_of(const Json& value, std::string_view key) {
    if (!value.is_array() || value.size() != 4) throw OpError(std::string(key) + " is [x, y, w, h] (mm)");
    std::array<double, 4> r{};
    for (std::size_t i = 0; i < 4; ++i) r[i] = coordinate(value[i], key);
    if (r[2] < 0) {
        r[0] += r[2];
        r[2] = -r[2];
    }
    if (r[3] < 0) {
        r[1] += r[3];
        r[3] = -r[3];
    }
    return r;
}

vec::Path path_of(const Json& value, std::string_view key) {
    try {
        return vec::path_from_json(value);
    } catch (const core::Error& error) {
        throw OpError(std::string(key) + ": " + error.what());
    }
}

vec::PathOp path_op(const Json& value, std::string_view key) {
    if (value.is_string()) {
        if (const auto op = vec::path_op_from(value.get<std::string>())) return *op;
    }
    throw OpError(std::string(key) + " is combine, subtract, intersect or exclude");
}

// A part as it is kept: through its JSON (its numbers to 0.1 µm, its keys checked) — what is saved is what is held.
vec::ShapePart normal(const vec::ShapePart& part) {
    try {
        return vec::part_from_json(vec::to_json(part));
    } catch (const core::Error& error) {
        throw OpError(std::string("the shape: ") + error.what());
    }
}

// --- the shape's terms ------------------------------------------------------------------------------------------------

// The kinds of a live shape and the title a new layer of one takes (Photoshop's tools: 長方形ツール…).
constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kKinds{{{"rect", "長方形"},
                                                                               {"rounded_rect", "角丸長方形"},
                                                                               {"ellipse", "楕円形"},
                                                                               {"triangle", "三角形"},
                                                                               {"polygon", "多角形"},
                                                                               {"star", "星形"},
                                                                               {"line", "ライン"},
                                                                               {"path", "シェイプ"}}};

std::string kind_title(std::string_view kind) {
    for (const auto& [k, title] : kKinds)
        if (k == kind) return std::string(title);
    return "シェイプ";
}

// The kind a live shape is (none: a path of its own).
std::string kind_of(const std::optional<vec::LiveShape>& live) {
    if (!live) return "path";
    switch (live->kind) {
        case vec::LiveShape::Kind::Rect:
            return std::any_of(live->radii.begin(), live->radii.end(), [](double r) { return r > 0; }) ? "rounded_rect" : "rect";
        case vec::LiveShape::Kind::Ellipse:
            return "ellipse";
        case vec::LiveShape::Kind::Polygon:
            return live->star_ratio < 1.0 ? "star" : live->sides == 3 ? "triangle" : "polygon";
        case vec::LiveShape::Kind::Line:
            return "line";
    }
    return "path";
}

struct Made {
    vec::Path path;
    std::optional<vec::LiveShape> live;
    std::string kind;
};

std::array<double, 4> radii_of(const Json& value) {
    if (value.is_number() && !value.is_boolean()) {
        const double r = number(value, "radii", 0.0, vec::kCoordinateMax);
        return {r, r, r, r};
    }
    if (!value.is_array() || value.size() != 4) throw OpError("radii is a radius or [tl, tr, br, bl] (mm)");
    std::array<double, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = number(value[i], "radii", 0.0, vec::kCoordinateMax);
    return out;
}

// The shape {kind, rect?, radii?, sides?, star_ratio?, angle?, from?, to?, weight_mm?, path?} made into its path and the
// live shape it comes from (PhotoCraft's live_from_params): `prev` gives what is not said (an edit of a live shape).
Made shape_from(const Json& shape, const std::optional<vec::LiveShape>& prev) {
    if (!shape.is_object()) throw OpError("shape is {kind, …}");
    static const std::set<std::string> kKeys{"kind", "rect", "radii", "sides", "star_ratio", "angle", "from", "to", "weight_mm", "path"};
    for (const auto& [key, value] : shape.items()) {
        if (!kKeys.contains(key)) throw OpError("shape has no key " + core::py_repr_str(key));
    }
    std::string kind = given(shape, "kind") != nullptr ? core::py_str(shape["kind"]) : (prev ? kind_of(prev) : std::string("rect"));
    if (std::none_of(kKinds.begin(), kKinds.end(), [&](const auto& k) { return k.first == kind; })) {
        throw OpError("shape's kind is rect, rounded_rect, ellipse, triangle, polygon, star, line or path");
    }
    Made made;
    made.kind = kind;
    if (kind == "path") {
        const Json* path = given(shape, "path");
        if (path == nullptr) throw OpError("a shape of kind path needs its path");
        made.path = path_of(*path, "shape's path");
        return made;
    }
    std::optional<std::array<double, 4>> prev_rect;
    if (prev && prev->kind != vec::LiveShape::Kind::Line) prev_rect = prev->rect;
    const auto rect = [&] {
        if (const Json* r = given(shape, "rect")) return rect_of(*r, "shape's rect");
        if (prev_rect) return *prev_rect;
        throw OpError("a shape of kind " + kind + " needs its rect [x, y, w, h] (mm)");
    };
    vec::LiveShape live;
    if (kind == "rect" || kind == "rounded_rect") {
        live.kind = vec::LiveShape::Kind::Rect;
        live.rect = rect();
        std::array<double, 4> before{0, 0, 0, 0};
        if (prev && prev->kind == vec::LiveShape::Kind::Rect) before = prev->radii;
        if (kind == "rounded_rect" && std::all_of(before.begin(), before.end(), [](double r) { return r <= 0; })) before = {2, 2, 2, 2};
        live.radii = given(shape, "radii") != nullptr ? radii_of(shape["radii"]) : before;
    } else if (kind == "ellipse") {
        live.kind = vec::LiveShape::Kind::Ellipse;
        live.rect = rect();
    } else if (kind == "polygon" || kind == "star" || kind == "triangle") {
        live.kind = vec::LiveShape::Kind::Polygon;
        live.rect = rect();
        const bool was = prev && prev->kind == vec::LiveShape::Kind::Polygon;
        int sides = was ? prev->sides : 5;
        double ratio = was ? prev->star_ratio : 1.0;
        live.angle = was ? prev->angle : 0.0;
        if (kind == "triangle") sides = 3;
        if (kind == "star" && ratio >= 1.0) ratio = 0.5;
        if (kind == "polygon" && given(shape, "star_ratio") == nullptr) ratio = 1.0;
        if (const Json* s = given(shape, "sides"); s != nullptr && kind != "triangle") {
            if (!s->is_number_integer()) throw OpError("sides must be a whole number (3..100)");
            sides = static_cast<int>(number(*s, "sides", 3, 100));
        }
        if (const Json* r = given(shape, "star_ratio")) ratio = number(*r, "star_ratio", 0.01, 1.0);
        if (const Json* a = given(shape, "angle")) live.angle = number(*a, "angle", -360.0, 360.0);
        live.sides = sides;
        live.star_ratio = ratio;
    } else {  // line
        live.kind = vec::LiveShape::Kind::Line;
        const bool was = prev && prev->kind == vec::LiveShape::Kind::Line;
        const Json* from = given(shape, "from");
        const Json* to = given(shape, "to");
        if ((from == nullptr || to == nullptr) && !was) throw OpError("a shape of kind line needs from and to [x, y] (mm)");
        live.from = from != nullptr ? point_of(*from, "shape's from") : prev->from;
        live.to = to != nullptr ? point_of(*to, "shape's to") : prev->to;
        live.weight_mm = given(shape, "weight_mm") != nullptr ? number(shape["weight_mm"], "weight_mm", 0.0, vec::kStrokeWidthMax)
                         : was                                ? prev->weight_mm
                                                              : 0.5;
    }
    made.path = path_raster::live_path(live);
    made.live = live;
    return made;
}

// A fill as the op gives it: null (none), {"rgb": [r, g, b] | null (the layer's ink)}, [r, g, b], "#rrggbb", or
// {"gradient": {…a fill layer's…}} (from and to, when not given, the part's top and bottom through its middle).
Json fill_from(const Json& value, const vec::Path& path, std::string_view key) {
    if (value.is_null()) return nullptr;
    const auto colour = [&](const Json& rgb) {
        if (rgb.is_null()) return Json{{"rgb", nullptr}};
        return Json{{"rgb", core::ints_json(core::rgb3(rgb, key))}};
    };
    if (value.is_string()) {
        const std::string hex = value.get<std::string>();
        const std::string digits = hex.starts_with("#") ? hex.substr(1) : hex;
        if (digits.size() != 6 || digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            throw OpError(std::string(key) + " is \"#rrggbb\", [r, g, b], {\"rgb\"} or {\"gradient\"}");
        }
        const auto byte = [&](std::size_t i) { return static_cast<std::int64_t>(std::stoi(digits.substr(i, 2), nullptr, 16)); };
        return Json{{"rgb", Json::array({byte(0), byte(2), byte(4)})}};
    }
    if (value.is_array()) return colour(value);
    if (!value.is_object()) throw OpError(std::string(key) + " is null, {\"rgb\": [r, g, b] | null} or {\"gradient\": {…}}");
    if (const Json* pattern = given(value, "pattern")) {  // (patterns@1, PC4 E3: a pattern the book keeps, by its id)
        for (const auto& [k, v] : value.items()) {
            if (k != "pattern") throw OpError(std::string(key) + " has no key " + core::py_repr_str(k));
        }
        try {
            return core::fill_spec(Json{{"pattern", *pattern}});
        } catch (const OpError& error) {
            throw OpError(std::string(key) + "'s " + error.what());
        }
    }
    if (const Json* g = given(value, "gradient")) {
        for (const auto& [k, v] : value.items()) {
            if (k != "gradient") throw OpError(std::string(key) + " has no key " + core::py_repr_str(k));
        }
        if (!g->is_object()) throw OpError(std::string(key) + "'s gradient is an object");
        Json spec = *g;
        if (!spec.contains("from") || !spec.contains("to")) {
            const auto b = path.control_bounds();
            const double cx = b ? ((*b)[0] + (*b)[2]) / 2 : 0.0;
            if (!spec.contains("from")) spec["from"] = Json::array({core::py_round(cx, 4), core::py_round(b ? (*b)[1] : 0.0, 4)});
            if (!spec.contains("to")) spec["to"] = Json::array({core::py_round(cx, 4), core::py_round(b ? (*b)[3] : 100.0, 4)});
        }
        try {
            return core::fill_spec(Json{{"gradient", spec}});
        } catch (const OpError& error) {
            throw OpError(std::string(key) + "'s gradient: " + error.what());
        }
    }
    for (const auto& [k, v] : value.items()) {
        if (k != "rgb") throw OpError(std::string(key) + " has no key " + core::py_repr_str(k));
    }
    if (!value.contains("rgb")) throw OpError(std::string(key) + " is null, {\"rgb\": [r, g, b] | null} or {\"gradient\": {…}}");
    return colour(value["rgb"]);
}

// A stroke as the op gives it (null or false: none), its keys laid over `base` (an edit's present stroke; a new one: 0.35
// mm in the layer's ink, centred). Its opacity 0..1.
std::optional<vec::ShapeStroke> stroke_from(const Json& value, const std::optional<vec::ShapeStroke>& base, const vec::Path& path) {
    if (value.is_null() || (value.is_boolean() && !value.get<bool>())) return std::nullopt;
    if (value.is_boolean()) {
        if (base) return base;
        vec::ShapeStroke made;  // (0.35 mm in the layer's ink)
        made.fill = Json{{"rgb", nullptr}};
        return made;
    }
    if (!value.is_object()) throw OpError("stroke is null or {width_mm, fill, opacity, align, cap, join, miter_limit, dashes, dash_offset}");
    vec::ShapeStroke start = base.value_or(vec::ShapeStroke{});
    if (!base) start.fill = Json{{"rgb", nullptr}};
    Json merged = vec::to_json(start);
    for (const auto& [key, item] : value.items()) {
        if (key == "fill" || key == "rgb") {
            merged["fill"] = key == "rgb" ? fill_from(Json{{"rgb", item}}, path, "stroke's rgb") : fill_from(item, path, "stroke's fill");
            if (merged["fill"].is_null()) merged["fill"] = Json{{"rgb", nullptr}};
        } else {
            merged[key] = item;
        }
    }
    if (merged.contains("dashes") && merged["dashes"].is_null()) merged.erase("dashes");
    try {
        return vec::stroke_from_json(merged);
    } catch (const core::Error& error) {
        throw OpError(std::string("stroke: ") + error.what());
    }
}

// --- layers and parts ---------------------------------------------------------------------------------------------------

std::size_t layer_at(const Page& page, const Json& id, std::string_view key) {
    if (!id.is_string()) throw OpError(std::string(key) + " is a layer's id");
    return core::layer_by_id(page, id.get<std::string>());
}

// The pen layer (unlocked) at `i`, or OpError.
Layer& pen(Page& page, std::size_t i) {
    Layer& layer = page.layers[i];
    if (layer.locked) throw OpError("the layer is locked");
    if (layer.kind != LayerKind::Strokes) {
        throw OpError("a shape part goes on a pen layer (the layer " + layer.id + " is not one: give new_layer to make one)");
    }
    return layer;
}

std::size_t part_index(const Layer& layer, const Json& id) {
    if (id.is_string()) {
        for (std::size_t i = 0; i < layer.shapes->items.size(); ++i)
            if (layer.shapes->items[i].id == id.get_ref<const std::string&>()) return i;
    }
    throw OpError("the layer " + layer.id + " has no shape part " + core::py_str(id));
}

std::vector<std::string> ids_of(const Json& op) {
    const Json* list = given(op, "ids");
    if (list == nullptr || !list->is_array()) throw OpError("ids is the list of shape parts' ids");
    std::vector<std::string> ids;
    for (const Json& v : *list) {
        if (!v.is_string()) throw OpError("ids is the list of shape parts' ids");
        if (std::find(ids.begin(), ids.end(), v.get<std::string>()) != ids.end()) throw OpError("ids names " + v.get<std::string>() + " twice");
        ids.push_back(v.get<std::string>());
    }
    return ids;
}

std::string fresh_id(const Layer& layer) {
    for (;;) {
        std::string id = core::new_id();
        if (std::none_of(layer.shapes->items.begin(), layer.shapes->items.end(), [&](const vec::ShapePart& p) { return p.id == id; })) return id;
    }
}

void put_parts(Layer& layer, std::vector<vec::ShapePart> parts) {
    if (parts.size() > vec::kShapePartsMax) throw OpError("a pen layer holds at most " + std::to_string(vec::kShapePartsMax) + " shape parts");
    layer.shapes = vec::make_shapes(std::move(parts));
}

Json bounds_json(const vec::ShapePart& part) {
    const auto b = sp::drawn_bounds(part);
    if (!b) return nullptr;
    return Json::array({core::py_round((*b)[0], 4), core::py_round((*b)[1], 4), core::py_round((*b)[2] - (*b)[0], 4), core::py_round((*b)[3] - (*b)[1], 4)});
}

Json report_of(const Layer& layer, const vec::ShapePart& part) {
    return Json{{"layer", layer.id}, {"part", part.id}, {"kind", kind_of(part.live)}, {"shape", vec::to_json(part)}, {"bounds", bounds_json(part)}};
}

// A path of an area: its polygon, or its mask's outline traced (at its 200 dpi within `tolerance_mm`).
vec::Path area_path(const Json& area, const Page& page, const Document& doc, double tolerance_mm) {
    const int dpi = selection::kSelDpi;
    const Image mask = selection::to_mask(area, page, &doc, dpi);
    const std::string bytes = mask.tobytes();
    std::vector<float> values(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    const double k = 25.4 / dpi;
    return path_trace::trace_mask(values, path_raster::PixelBox{0, 0, mask.width(), mask.height()}, 0.0f, tolerance_mm / k)
        .transformed({k, 0.0, 0.0, k, 0.0, 0.0});
}

}  // namespace

// --- placing a part ------------------------------------------------------------------------------------------------------

Json place_shape(OpContext& c, std::size_t at, vec::Path path, std::optional<vec::LiveShape> live, const std::string& kind, const Json& default_fill) {
    Document& doc = c.doc;
    const Json& op = c.op;
    if (path.empty()) throw OpError("the shape is empty");
    vec::ShapePart part;
    part.path = std::move(path);
    part.live = std::move(live);
    part.fill = op.contains("fill") ? fill_from(op["fill"], part.path, "fill") : fill_from(default_fill, part.path, "fill");
    if (const Json* stroke = given(op, "stroke")) part.stroke = stroke_from(*stroke, std::nullopt, part.path);
    core::check_pattern_kept(doc, part.fill);  // (patterns@1, PC4 E3)
    if (part.stroke) core::check_pattern_kept(doc, part.stroke->fill);
    if (const Json* o = given(op, "opacity")) part.opacity = number(*o, "opacity", 0.0, 1.0);
    if (const Json* o = given(op, "over")) part.over = boolean(*o, "over");
    if (const Json* n = given(op, "name")) {
        if (!n->is_string()) throw OpError("name must be text");
        try {
            part.name = vec::path_name(n->get<std::string>());
        } catch (const core::Error& error) {
            throw OpError(std::string("name: ") + error.what());
        }
    }
    Page& page = doc.edit_page(at);
    std::size_t li = 0;
    if (const Json* made = given(op, "new_layer")) {
        // (Photoshop's shape on a layer of its own: a new pen layer above the one named — or above `after`, or on top)
        if (!made->is_object() && !(made->is_boolean() && made->get<bool>())) throw OpError("new_layer is {name?, id?, after?} or true");
        const Json terms = made->is_object() ? *made : Json::object();
        Layer layer;
        layer.id = given(terms, "id") != nullptr ? core::py_str(terms["id"]) : core::new_id();
        for (const Layer& item : page.layers)
            if (item.id == layer.id) throw OpError("layer " + layer.id + " exists");
        layer.role = core::LayerRole::User;
        layer.kind = LayerKind::Strokes;
        layer.title = given(terms, "name") != nullptr ? core::py_str(terms["name"]) : kind_title(kind);
        layer.exportable = true;
        layer.panel_each = true;  // (as add_layer's new pen layer)
        std::size_t index = page.layers.size();
        const Json* after = given(terms, "after");
        if (after == nullptr) after = given(op, "layer_id");
        if (after != nullptr) {
            const std::size_t ref = layer_at(page, *after, "new_layer's after");
            index = ref + 1;
            layer.parent_id = page.layers[ref].kind == LayerKind::Folder && given(terms, "after") != nullptr ? Json(page.layers[ref].id)
                                                                                                            : page.layers[ref].parent_id;
        }
        page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
        li = index;
    } else if (const Json* id = given(op, "layer_id")) {
        li = layer_at(page, *id, "layer_id");
    } else {
        Layer& target = core::paint_target(page, op);
        li = static_cast<std::size_t>(&target - page.layers.data());
    }
    Layer& layer = pen(page, li);
    if (const Json* id = given(op, "id")) {
        if (!id->is_string() || id->get_ref<const std::string&>().empty() || id->get_ref<const std::string&>().size() > 64)
            throw OpError("id must be text (1..64 bytes)");
        part.id = id->get<std::string>();
        if (std::any_of(layer.shapes->items.begin(), layer.shapes->items.end(), [&](const vec::ShapePart& p) { return p.id == part.id; }))
            throw OpError("the layer " + layer.id + " has a shape part " + part.id + " already");
    } else {
        part.id = fresh_id(layer);
    }
    part = normal(part);
    std::vector<vec::ShapePart> parts = layer.shapes->items;
    parts.push_back(part);
    put_parts(layer, std::move(parts));
    return report_of(layer, part);
}

namespace {

// --- the ops ---------------------------------------------------------------------------------------------------------------

// {page, layer_id? | new_layer?: {name?, id?, after?}, shape: {kind, …} | area (+ tolerance_mm?), fill?, stroke?, opacity?,
// over?, name?, id?} → {layer, part, kind, shape, bounds}
void add_shape_part(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* shape = given(c.op, "shape");
    const Json area = given(c.op, "area") != nullptr ? core::op_area(c.op) : Json();
    if ((shape != nullptr) == !area.is_null()) throw OpError("give shape ({kind, …}) or area (Convert to Shape)");
    if (shape != nullptr) {
        Made made = shape_from(*shape, std::nullopt);
        c.report = place_shape(c, at, std::move(made.path), std::move(made.live), made.kind, Json{{"rgb", nullptr}});
        return;
    }
    // (選択範囲 → シェイプに変換: the area's outline fitted with curves, as Make Work Path)
    double tolerance = 0.254;
    if (const Json* t = given(c.op, "tolerance_mm")) tolerance = number(*t, "tolerance_mm", 0.0635, 1.27);
    vec::Path path = area_path(area, doc.page(at), doc, tolerance);
    if (path.subpaths.empty()) throw OpError("the area is empty");
    c.report = place_shape(c, at, std::move(path), std::nullopt, "path", Json{{"rgb", nullptr}});
}

// A part's lines and fill as add_shape makes them (to: "lines"): its outline as pen lines of its stroke (none without
// one), its fill a patch — one colour a "mask" patch in that colour, a gradient an "image" patch.
void into_lines(Document& doc, const Page& page, Layer& layer, const vec::ShapePart& part) {
    const int dpi = fills::kFillDpi;
    if (sp::fills(part.fill)) {
        vec::ShapePart fill_only = part;
        fill_only.stroke.reset();
        const Json& rgb = part.fill.contains("rgb") ? part.fill["rgb"] : Json();
        if (part.fill.contains("rgb")) {
            const sp::Compiled one(fill_only, dpi);
            if (std::optional<Box> b = one.box()) {
                limits::check_picture(b->width(), b->height(), "the shape");
                const std::vector<std::array<float, 4>> rgba = one.rgba(*b);
                std::string bytes(rgba.size(), '\0');
                for (std::size_t i = 0; i < rgba.size(); ++i)
                    bytes[i] = static_cast<char>(std::lround(std::clamp(rgba[i][3] / std::max(part.opacity, 1e-9), 0.0, 1.0) * 255.0));
                const Image mask = Image::frombytes("L", Size{b->width(), b->height()}, bytes);
                const std::vector<std::int64_t> colour = rgb.is_null() ? sp::kInkColour : core::rgb3(rgb, "fill");
                if (auto patch = fills::mask_patch(mask, dpi, colour, part.opacity, {static_cast<double>(b->x0), static_cast<double>(b->y0)}))
                    layer.patches.push_back(std::move(*patch));
            }
        } else if (auto patch = sp::as_patch(&page, fill_only, &doc)) {
            layer.patches.push_back(std::move(*patch));
        }
    }
    if (part.stroke && part.stroke->width_mm > 0) {
        std::vector<core::StrokePtr> items = layer.strokes->items;
        const Json& fill = part.stroke->fill;
        std::optional<std::vector<std::int64_t>> colour;
        if (fill.is_object() && fill.contains("rgb") && !fill["rgb"].is_null()) colour = core::rgb3(fill["rgb"], "stroke's rgb");
        for (const path_raster::Polyline& line : path_raster::flatten_path(part.path, 0.02)) {
            if (line.pts.size() < 2) continue;
            core::PenPoints points;
            for (const auto& [x, y] : line.pts) points.push_back(core::PenPoint{core::py_round(x, 3), core::py_round(y, 3), 1.0});
            if (line.closed) points.push_back(points.front());
            limits::check_count(static_cast<double>(points.size()), static_cast<double>(limits::kPoints), "the shape's outline");
            auto stroke = std::make_shared<core::Stroke>(core::coerce_stroke(points));
            stroke->kind = core::brush_kind(Json("mili"), doc);
            stroke->width_mm = part.stroke->width_mm;
            stroke->rgb = colour;
            stroke->opacity = std::clamp(part.stroke->opacity * part.opacity, 0.0, 1.0);
            items.push_back(std::move(stroke));
        }
        layer.strokes = core::make_strokes(std::move(items));
    }
}

// {page, layer_id, id, path? | shape?: {kind?, rect?, …}, move_mm?: [dx, dy], matrix?: [a, b, c, d, e, f], fill_rule?,
// operation? + subpath? (Photoshop's path operation: the key "op" names the op itself), append?: {path, op},
// merge_components?: true | {tolerance_mm}, fill?, stroke?, opacity?, over?, name?, order?: front | forward | backward |
// back, to?: patch | lines} → {layer, part, kind, shape, bounds} ({layer, part: null, to} when it became a patch or lines)
void edit_shape_part(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* layer_id = given(op, "layer_id");
    if (layer_id == nullptr) throw OpError("layer_id is required");
    Page& page = doc.edit_page(at);
    Layer& layer = pen(page, layer_at(page, *layer_id, "layer_id"));
    const Json* id = given(op, "id");
    if (id == nullptr) throw OpError("id is required (the shape part's id)");
    std::vector<vec::ShapePart> parts = layer.shapes->items;
    std::size_t k = part_index(layer, *id);
    vec::ShapePart part = parts[k];
    const auto unlive = [&] { part.live.reset(); };
    if (const Json* p = given(op, "path")) {
        part.path = path_of(*p, "path");
        unlive();
    }
    if (const Json* s = given(op, "shape")) {
        Made made = shape_from(*s, part.live);
        part.path = std::move(made.path);
        part.live = std::move(made.live);
    }
    if (const Json* m = given(op, "move_mm")) {
        const vec::XY d = point_of(*m, "move_mm");
        part = sp::transformed(part, {1.0, 0.0, 0.0, 1.0, d.x, d.y}, false);
    }
    if (const Json* m = given(op, "matrix")) {
        if (!m->is_array() || m->size() != 6) throw OpError("matrix is [a, b, c, d, e, f]");
        vec::Affine a{};
        for (std::size_t i = 0; i < 6; ++i) a[i] = core::finite_float((*m)[i], "matrix");
        if (std::fabs(a[0] * a[3] - a[1] * a[2]) < 1e-12) throw OpError("the matrix squashes the shape flat");
        part = sp::transformed(part, a, false);
    }
    if (const Json* r = given(op, "fill_rule")) {
        const std::string rule = core::py_str(*r);
        if (rule != "nonzero" && rule != "evenodd") throw OpError("fill_rule is nonzero or evenodd");
        part.path.fill_rule = rule == "evenodd" ? vec::FillRule::EvenOdd : vec::FillRule::NonZero;
    }
    if (const Json* o = given(op, "operation")) {
        // (Combine Shapes inside the part: the subpath's operation — by default every subpath but the first)
        const vec::PathOp how = path_op(*o, "operation");
        if (const Json* s = given(op, "subpath")) {
            if (!s->is_number_integer() || s->get<std::int64_t>() < 0 || s->get<std::int64_t>() >= static_cast<std::int64_t>(part.path.subpaths.size()))
                throw OpError("subpath is not one of the shape's subpaths");
            part.path.subpaths[static_cast<std::size_t>(s->get<std::int64_t>())].op = how;
        } else {
            for (std::size_t i = 1; i < part.path.subpaths.size(); ++i) part.path.subpaths[i].op = how;
        }
        unlive();
    }
    if (const Json* a = given(op, "append")) {
        if (!a->is_object() || given(*a, "path") == nullptr) throw OpError("append is {path, op?}");
        const vec::PathOp how = given(*a, "op") != nullptr ? path_op((*a)["op"], "append's op") : vec::PathOp::Combine;
        vec::Path more = path_of((*a)["path"], "append's path");
        for (vec::Subpath& s : more.subpaths) {
            s.op = how;
            part.path.subpaths.push_back(std::move(s));
        }
        unlive();
    }
    if (const Json* merge = given(op, "merge_components")) {
        double tolerance = sp::kBakeTolerance;
        if (merge->is_object()) {
            if (const Json* t = given(*merge, "tolerance_mm")) tolerance = number(*t, "tolerance_mm", 0.001, 1.27);
        } else if (!boolean(*merge, "merge_components")) {
            tolerance = -1;
        }
        if (tolerance > 0) {
            part.path = sp::baked(part.path, tolerance);
            unlive();
        }
    }
    if (op.contains("fill")) part.fill = fill_from(op["fill"], part.path, "fill");
    if (op.contains("stroke")) part.stroke = stroke_from(op["stroke"], part.stroke, part.path);
    core::check_pattern_kept(doc, part.fill);  // (patterns@1, PC4 E3)
    if (part.stroke) core::check_pattern_kept(doc, part.stroke->fill);
    if (const Json* o = given(op, "opacity")) part.opacity = number(*o, "opacity", 0.0, 1.0);
    if (const Json* o = given(op, "over")) part.over = boolean(*o, "over");
    if (op.contains("name")) {
        if (op["name"].is_null()) {
            part.name.reset();
        } else {
            if (!op["name"].is_string()) throw OpError("name must be text");
            try {
                part.name = vec::path_name(op["name"].get<std::string>());
            } catch (const core::Error& error) {
                throw OpError(std::string("name: ") + error.what());
            }
        }
    }
    if (part.path.empty()) throw OpError("the shape would be empty");
    part = normal(part);
    parts[k] = part;
    if (const Json* order = given(op, "order")) {
        const std::string where = core::py_str(*order);
        std::size_t to = k;
        if (where == "front") to = parts.size() - 1;
        else if (where == "back") to = 0;
        else if (where == "forward") to = std::min(k + 1, parts.size() - 1);
        else if (where == "backward") to = k == 0 ? 0 : k - 1;
        else throw OpError("order is front, forward, backward or back");
        vec::ShapePart moving = parts[k];
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(k));
        parts.insert(parts.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
        k = to;
    }
    if (const Json* to = given(op, "to")) {
        const std::string into = core::py_str(*to);
        if (into != "patch" && into != "lines") throw OpError("to is patch or lines");
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(k));
        if (into == "patch") {
            if (auto patch = sp::as_patch(&page, part, &doc)) layer.patches.push_back(std::move(*patch));
        } else {
            into_lines(doc, page, layer, part);
        }
        put_parts(layer, std::move(parts));
        c.report = Json{{"layer", layer.id}, {"part", nullptr}, {"to", into}};
        return;
    }
    put_parts(layer, std::move(parts));
    c.report = report_of(layer, part);
}

// {page, layer_id, ids: [two or more], operation?: combine (default) | subtract | intersect | exclude} → {layer, part, …}:
// the first part takes the others' subpaths with that operation (its fill and stroke kept), the others go.
void combine_shape_parts(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* layer_id = given(op, "layer_id");
    if (layer_id == nullptr) throw OpError("layer_id is required");
    Page& page = doc.edit_page(at);
    Layer& layer = pen(page, layer_at(page, *layer_id, "layer_id"));
    const std::vector<std::string> ids = ids_of(op);
    if (ids.size() < 2) throw OpError("choose two or more shape parts to combine");
    const vec::PathOp how = given(op, "operation") != nullptr ? path_op(op["operation"], "operation") : vec::PathOp::Combine;
    std::vector<vec::ShapePart> parts = layer.shapes->items;
    const std::size_t first = part_index(layer, Json(ids[0]));
    vec::ShapePart& into = parts[first];
    std::set<std::string> gone;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        const vec::ShapePart& other = parts[part_index(layer, Json(ids[i]))];
        for (vec::Subpath s : other.path.subpaths) {
            s.op = how;
            into.path.subpaths.push_back(std::move(s));
        }
        gone.insert(ids[i]);
    }
    into.live.reset();
    if (into.path.knot_count() > vec::kKnotsMax) throw OpError("the shape would have more than " + std::to_string(vec::kKnotsMax) + " knots");
    into = normal(into);
    const vec::ShapePart kept = into;
    std::erase_if(parts, [&](const vec::ShapePart& p) { return gone.contains(p.id); });
    put_parts(layer, std::move(parts));
    c.report = report_of(layer, kept);
}

// {page, layer_id, ids, to_layer, order?: front (default) | back} → {layer, parts}: the parts moved to another pen layer
// (over its parts, or under them), an id it has already given a new one.
void move_shape_parts(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* layer_id = given(op, "layer_id");
    const Json* to_layer = given(op, "to_layer");
    if (layer_id == nullptr || to_layer == nullptr) throw OpError("layer_id and to_layer are required");
    const std::vector<std::string> ids = ids_of(op);
    if (ids.empty()) throw OpError("ids names no shape part");
    const std::string order = given(op, "order") != nullptr ? core::py_str(op["order"]) : std::string("front");
    if (order != "front" && order != "back") throw OpError("order is front or back");
    Page& page = doc.edit_page(at);
    const std::size_t from_i = layer_at(page, *layer_id, "layer_id");
    const std::size_t to_i = layer_at(page, *to_layer, "to_layer");
    Layer& from = pen(page, from_i);
    (void)pen(page, to_i);
    std::vector<vec::ShapePart> moving;
    for (const std::string& id : ids) moving.push_back(from.shapes->items[part_index(from, Json(id))]);
    std::vector<vec::ShapePart> left = from.shapes->items;
    std::erase_if(left, [&](const vec::ShapePart& p) { return std::find(ids.begin(), ids.end(), p.id) != ids.end(); });
    put_parts(from, std::move(left));
    Layer& to = page.layers[to_i];
    std::vector<vec::ShapePart> there = to.shapes->items;
    Json moved = Json::array();
    for (vec::ShapePart& part : moving) {
        if (std::any_of(there.begin(), there.end(), [&](const vec::ShapePart& p) { return p.id == part.id; })) part.id = fresh_id(to);
        moved.push_back(part.id);
    }
    if (order == "front") {
        there.insert(there.end(), moving.begin(), moving.end());
    } else {
        there.insert(there.begin(), moving.begin(), moving.end());
    }
    put_parts(to, std::move(there));
    c.report = Json{{"layer", to.id}, {"parts", std::move(moved)}};
}

// --- the parts in the other ops --------------------------------------------------------------------------------------------

// The layer an op on lines names, without making one (op["layer_id"], or the first layer of the role op["layer"], "ink"
// by default): none when there is none.
Layer* lines_layer(Page& page, const Json& op) {
    if (core::truthy_at(op, "layer_id")) {
        const std::string id = core::py_str(op["layer_id"]);
        for (Layer& layer : page.layers)
            if (layer.id == id) return &layer;
        return nullptr;
    }
    const Json* role = core::get(op, "layer");
    std::string name = role != nullptr && core::py_truthy(*role) ? core::py_str(*role) : std::string("ink");
    for (Layer& layer : page.layers)
        if (std::string(core::to_string(layer.role)) == name) return &layer;
    return nullptr;
}

const Layer* layer_with_parts(const Document& doc, const Json& op) {
    std::size_t at = 0;
    try {
        at = core::require_page(doc, op);
    } catch (const std::exception&) {
        return nullptr;
    }
    Layer* layer = lines_layer(const_cast<Page&>(doc.page(at)), op);
    return layer != nullptr && sp::any(*layer) ? layer : nullptr;
}

// Does the part draw anything inside the band (outline pieces in mm, non-zero)? (At 200 dpi.)
bool touches(const vec::ShapePart& part, const path_raster::Rasterizer& band, double k) {
    const auto pb = band.pixel_bounds();
    const auto b = sp::drawn_bounds(part);
    if (!pb || !b) return false;
    const path_raster::PixelBox box{std::max(pb->x0, static_cast<int>(std::floor((*b)[0] * k)) - 1),
                                    std::max(pb->y0, static_cast<int>(std::floor((*b)[1] * k)) - 1),
                                    std::min(pb->x1, static_cast<int>(std::ceil((*b)[2] * k)) + 2),
                                    std::min(pb->y1, static_cast<int>(std::ceil((*b)[3] * k)) + 2)};
    if (box.empty()) return false;
    limits::check_picture(box.width(), box.height(), "the eraser");
    const vec::Path px = part.path.transformed({k, 0.0, 0.0, k, 0.0, 0.0});
    std::vector<float> cover = sp::fills(part.fill) ? path_raster::fill_rasterizer(px).render(box) : std::vector<float>(std::size_t(box.width()) * box.height(), 0.0f);
    if (part.stroke && part.stroke->width_mm > 0) {
        path_raster::StrokeStyle style = path_raster::stroke_style(*part.stroke, k);
        if (sp::closed(part.path) && part.stroke->align != vec::StrokeAlign::Center) style.width *= 2.0;
        const std::vector<float> outline = path_raster::stroke_rasterizer(px, style).render(box);
        for (std::size_t i = 0; i < cover.size(); ++i) cover[i] = std::max(cover[i], outline[i]);
    }
    const std::vector<float> erased = band.render(box);
    for (std::size_t i = 0; i < cover.size(); ++i)
        if (cover[i] * erased[i] > 0.05f) return true;
    return false;
}

// erase and erase_raster with a pen layer's shape parts: the lines as before (the op itself), then the parts the eraser
// went over — every one it touches gone for "whole" and "to_crossing", else the band it went over taken out of each
// closed part (its outline traced again, a vector part still; an open one, which has no inside to cut, gone).
core::OpFunction erase_with_parts(core::OpFunction plain) {
    return [plain](OpContext& c) {
        const Layer* seen = layer_with_parts(c.doc, c.op);
        if (seen == nullptr || seen->locked || seen->kind != LayerKind::Strokes) {
            plain(c);
            return;
        }
        const std::string layer_id = seen->id;
        const std::size_t at = core::require_page(c.doc, c.op);
        const Json* raw = core::get(c.op, "points");
        core::PenPoints points = core::parse_points(raw != nullptr && core::py_truthy(*raw) ? *raw : Json::array());
        const Json* width_value = core::get(c.op, "width_mm");
        const double width = width_value != nullptr ? core::to_float(*width_value) : 2.0;
        const Page& page_seen = c.doc.page(at);
        if (core::truthy_at(c.op, "snap_ruler") && core::py_truthy(page_seen.rulers)) {  // (the line the eraser took: as erase snaps it)
            const Json lid_json(layer_id);
            const Json only = c.op.contains("ruler_id") ? c.op["ruler_id"] : Json(nullptr);
            const core::rulers::FrameContains inside = [&page_seen](const Json& frame_id, double x, double y) {
                if (page_seen.frames.empty() || !frame_id.is_string()) return false;
                const core::Frame* frame = page_seen.find_frame(frame_id.get_ref<const std::string&>());
                return frame != nullptr && core::contains(*frame, core::Num(x), core::Num(y));
            };
            points = core::rulers::snap(points, page_seen.rulers, inside, only, &lid_json);
        }
        const Json mode = c.op.contains("mode") ? c.op["mode"] : Json(nullptr);
        plain(c);  // (the lines; its errors first)
        if (points.empty() || !(width > 0) || !std::isfinite(width)) return;
        if (points.size() == 1) points.push_back(core::PenPoint{points[0].x + 0.01, points[0].y + 0.01, std::nullopt});
        path_raster::Polyline line;
        for (const core::PenPoint& p : points) {
            line.pts.emplace_back(p.x, p.y);
            line.knot.push_back(true);
        }
        path_raster::StrokeStyle style;
        style.width = width;
        style.cap = vec::LineCap::Round;
        style.join = vec::LineJoin::Round;
        const std::vector<std::vector<path_raster::XY>> polys = path_raster::stroke_polygons({line}, style, 0.005);
        constexpr double k = 200.0 / 25.4;
        std::vector<std::vector<path_raster::XY>> polys_px;
        for (const auto& poly : polys) {
            std::vector<path_raster::XY> one;
            for (const auto& [x, y] : poly) one.emplace_back(x * k, y * k);
            polys_px.push_back(std::move(one));
        }
        path_raster::Rasterizer band;
        band.add_component(polys_px, vec::PathOp::Combine, vec::FillRule::NonZero);
        const bool whole = mode == Json("whole") || mode == Json("to_crossing");
        Page& page = c.doc.edit_page(at);
        Layer& layer = page.layers[core::layer_by_id(page, layer_id)];
        std::vector<vec::ShapePart> parts;
        bool changed = false;
        for (const vec::ShapePart& part : layer.shapes->items) {
            if (part.path.inverted || !touches(part, band, k)) {
                parts.push_back(part);
                continue;
            }
            changed = true;
            if (whole || !sp::closed(part.path)) continue;
            vec::ShapePart cut = part;
            cut.path = sp::without(part.path, polys);
            cut.live.reset();
            if (cut.path.empty()) continue;
            parts.push_back(normal(cut));
        }
        if (changed) put_parts(layer, std::move(parts));
    };
}

// vector_edit with a pen layer's shape parts: delete and recolor take a part's id too (gone; its fill and stroke in the
// colour — rgb null: the layer's ink); the actions on a line's points refuse one (its anchors and handles are its path:
// edit_shape_part).
core::OpFunction vector_edit_with_parts(core::OpFunction plain) {
    return [plain](OpContext& c) {
        const Layer* seen = layer_with_parts(c.doc, c.op);
        if (seen == nullptr) {
            plain(c);
            return;
        }
        const std::string action = core::truthy_at(c.op, "action") ? core::py_str(c.op["action"]) : std::string();
        const auto is_part = [&](const std::string& id) {
            return std::any_of(seen->shapes->items.begin(), seen->shapes->items.end(), [&](const vec::ShapePart& p) { return p.id == id; });
        };
        if (action != "recolor" && action != "delete") {
            if (core::truthy_at(c.op, "stroke_id") && is_part(core::py_str(c.op["stroke_id"]))) {
                throw OpError(core::py_str(c.op["stroke_id"]) + " is a shape part: its anchors and handles are its path (edit_shape_part)");
            }
            if (const Json* ids = given(c.op, "ids"); ids != nullptr && ids->is_array()) {
                for (const Json& v : *ids)
                    if (v.is_string() && is_part(v.get<std::string>())) throw OpError(v.get<std::string>() + " is a shape part, not a line");
            }
            plain(c);
            return;
        }
        std::vector<std::string> ids;
        if (core::truthy_at(c.op, "ids")) {
            for (const Json& v : core::iterate(c.op["ids"])) ids.push_back(core::py_str(v));
        } else if (core::truthy_at(c.op, "stroke_id")) {
            ids.push_back(core::py_str(c.op["stroke_id"]));
        }
        std::vector<std::string> part_ids, line_ids;
        for (const std::string& id : ids) (is_part(id) ? part_ids : line_ids).push_back(id);
        if (part_ids.empty()) {
            plain(c);
            return;
        }
        if (seen->locked) throw OpError("the layer is locked");
        const std::string layer_id = seen->id;
        if (!line_ids.empty()) {  // (the lines first, as the op does them)
            Json lines = c.op;
            lines.erase("stroke_id");
            Json list = Json::array();
            for (const std::string& id : line_ids) list.push_back(id);
            lines["ids"] = std::move(list);
            OpContext sub{c.doc, lines, c.actor};
            plain(sub);
        }
        const std::size_t at = core::require_page(c.doc, c.op);
        Page& page = c.doc.edit_page(at);
        Layer& layer = page.layers[core::layer_by_id(page, layer_id)];
        std::vector<vec::ShapePart> parts;
        const Json rgb = c.op.contains("rgb") ? c.op["rgb"] : Json();
        const Json colour = core::py_truthy(rgb) ? Json{{"rgb", core::ints_json(core::rgb3(rgb, "rgb"))}} : Json{{"rgb", nullptr}};
        for (const vec::ShapePart& part : layer.shapes->items) {
            if (std::find(part_ids.begin(), part_ids.end(), part.id) == part_ids.end()) {
                parts.push_back(part);
                continue;
            }
            if (action == "delete") continue;
            vec::ShapePart recoloured = part;
            if (!recoloured.fill.is_null()) recoloured.fill = colour;
            if (recoloured.stroke) recoloured.stroke->fill = colour;
            parts.push_back(normal(recoloured));
        }
        put_parts(layer, std::move(parts));
    };
}

}  // namespace

void register_shape_ops(core::OpRegistry& registry) {
    registry.add("add_shape_part", add_shape_part);
    registry.add("edit_shape_part", edit_shape_part);
    registry.add("combine_shape_parts", combine_shape_parts);
    registry.add("move_shape_parts", move_shape_parts);
    // (the eraser and the line editor again, now with the parts of a pen layer)
    for (const char* name : {"erase", "erase_raster"}) {
        if (const core::OpFunction* plain = registry.find(name)) registry.add(name, erase_with_parts(*plain));
    }
    if (const core::OpFunction* plain = registry.find("vector_edit")) registry.add("vector_edit", vector_edit_with_parts(*plain));
}

}  // namespace genko::render
