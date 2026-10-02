// The layer ops (Python's ops._apply_one and layerops.set_layers): add_layer, delete_layer, duplicate_layer,
// set_layer, set_layers and reorder_layers, with the checks of a fill (ベタ塗り・グラデーション), a correction layer's
// adjustment (tried on its settings as Python tries it on a small picture), the layer effects and the screen.

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

constexpr const char* kAdjustments[] = {"levels",    "curve",        "hue",     "invert",           "posterize",
                                        "threshold", "gradient_map", "bitonal", "brightness_contrast"};

Json value_or(const Json& object, const char* key, const Json& fallback) {
    const Json* value = get(object, key);
    return value != nullptr ? *value : fallback;
}

// float(params.get(key, fallback))
double param_float(const Json& params, const char* key, const Json& fallback) { return to_float(value_or(params, key, fallback)); }
std::int64_t param_int(const Json& params, const char* key, const Json& fallback) { return to_int(value_or(params, key, fallback)); }

// `x, y = item` for two values
std::pair<Json, Json> unpack_two(const Json& item) {
    if (!(item.is_array() || item.is_string() || item.is_object())) {
        throw PyTypeError("cannot unpack non-iterable " + py_type_name(item) + " object");
    }
    const std::vector<Json> items = iterate(item);
    if (items.size() < 2) {
        throw PyValueError("not enough values to unpack (expected 2, got " + std::to_string(items.size()) + ")");
    }
    if (items.size() > 2) throw PyValueError("too many values to unpack (expected 2)");
    return {items[0], items[1]};
}

// The checks filters.apply_filter makes of an adjustment's settings (Python tries the adjustment on a 4×4 picture;
// only its settings can fail there).
void try_adjustment(const std::string& kind, const Json& params) {
    if (kind == "levels") {
        static const char* const kTableKeys[] = {"gamma", "out_black", "out_white", "channel"};
        const bool table = std::any_of(std::begin(kTableKeys), std::end(kTableKeys), [&](const char* key) {
            const Json* v = get(params, key);
            return v != nullptr && !v->is_null();
        });
        if (table) {  // filters.levels_table
            param_float(params, "black", Json(0));
            param_float(params, "black", Json(0));
            param_float(params, "white", Json(255));
            param_float(params, "gamma", Json(1.0));
            param_float(params, "out_black", Json(0));
            param_float(params, "out_white", Json(255));
            return;
        }
        param_int(params, "black", Json(0));
        param_int(params, "white", Json(255));
        return;
    }
    if (kind == "curve") {
        const Json* points = get(params, "points");
        if (points != nullptr && py_truthy(*points)) {  // filters.curve_table
            std::set<double> keys;  // (int(round(x)): the rounded values, as doubles)
            for (const Json& item : iterate(*points)) {
                const auto [x, y] = unpack_two(item);
                keys.insert(py_round(to_float(x), 0));
                to_float(y);
            }
            if (keys.size() < 2) throw PyValueError("a tone curve needs two points or more");
            return;
        }
        param_float(params, "gamma", Json(1.6));
        return;
    }
    if (kind == "hue") {
        param_float(params, "shift", Json(30));
        param_float(params, "saturation", Json(1.0));
        param_float(params, "value", Json(1.0));
        return;
    }
    if (kind == "posterize") {
        param_int(params, "levels", Json(4));
        return;
    }
    if (kind == "threshold") {
        param_int(params, "threshold", Json(128));
        return;
    }
    if (kind == "bitonal") {
        param_int(params, "threshold", Json(180));
        return;
    }
    if (kind == "brightness_contrast") {
        param_float(params, "brightness", Json(0));
        param_float(params, "contrast", Json(0));
        return;
    }
    if (kind == "gradient_map") {
        const Json* stops = get(params, "stops");
        if (stops != nullptr && py_truthy(*stops)) {
            std::vector<std::size_t> sizes;
            for (const Json& st : iterate(*stops)) {
                to_float(subscript(st, 0));
                std::size_t n = 0;
                for (const Json& v : iterate(subscript(st, 1))) {
                    to_int(v);
                    ++n;
                }
                sizes.push_back(std::min<std::size_t>(n, 3));
            }
            if (sizes.size() < 2) throw PyValueError("a gradient map needs two colours or more");
            for (const std::size_t n : sizes) {
                if (n < 3) throw PyUncaught("IndexError", "list index out of range");
            }
            return;
        }
        const Json* colors = get(params, "colors");
        const Json list = colors != nullptr && py_truthy(*colors) ? *colors
                                                                  : Json::array({Json::array({0, 0, 0}), Json::array({255, 255, 255})});
        std::vector<std::size_t> sizes;
        for (const Json& c : iterate(list)) {
            std::size_t n = 0;
            for (const Json& v : iterate(c)) {
                to_int(v);
                ++n;
            }
            sizes.push_back(n);
        }
        if (sizes.size() < 2) throw PyValueError("a gradient map needs two colours or more");
        for (const std::size_t n : sizes) {
            if (n < 3) throw PyUncaught("IndexError", "tuple index out of range");
        }
        return;
    }
    // invert: nothing to check
}

// ops._adjust_spec
Json adjust_spec(const Json& raw) {
    std::string names;
    for (const char* name : kAdjustments) names += (names.empty() ? "" : ", ") + std::string(name);
    const Json* kind = get(raw, "kind");
    const bool known = raw.is_object() && kind != nullptr && kind->is_string() &&
                       std::find(std::begin(kAdjustments), std::end(kAdjustments), kind->get<std::string>()) !=
                           std::end(kAdjustments);
    if (!known) throw OpError("adjust kind must be one of " + names);
    Json params = Json::object();
    for (const auto& [key, value] : raw.items()) {
        if (key != "kind") params[key] = value;
    }
    try {
        try_adjustment(kind->get<std::string>(), params);
    } catch (const PyValueError& error) {
        throw OpError(std::string("the adjustment cannot be used: ") + error.what());
    } catch (const PyTypeError& error) {
        throw OpError(std::string("the adjustment cannot be used: ") + error.what());
    } catch (const OpKeyError& error) {
        throw OpError(std::string("the adjustment cannot be used: ") + error.what());
    }
    return raw;
}

// ops._gradient_extras: 多色 (stops), 楕円 (ratio), 繰り返し (repeat).
Json gradient_extras(const Json& g) {
    Json out = Json::object();
    if (truthy_at(g, "stops")) {
        const std::string message = "stops are [[position 0..1, [r,g,b], opacity?], …]";
        std::vector<std::pair<double, Json>> stops;
        try {
            for (const Json& s : iterate(g["stops"])) {
                const double position = py_round(to_float(subscript(s, 0)), 4);
                const std::vector<std::int64_t> rgb = rgb3(subscript(s, 1), "stops");
                double opacity = 1.0;
                if (length(s) > 2) {
                    const Json third = subscript(s, 2);
                    if (!third.is_null()) opacity = py_round(to_float(third), 3);
                }
                stops.emplace_back(position, Json::array({position, ints_json(rgb), opacity}));
            }
        } catch (const PyTypeError&) {
            throw OpError(message);
        } catch (const PyValueError&) {
            throw OpError(message);
        } catch (const OpError&) {  // (Python's ApplyError is a ValueError: _rgb3's is caught here too)
            throw OpError(message);
        } catch (const PyUncaught& error) {
            if (error.type() != "IndexError") throw;
            throw OpError(message);
        }
        if (stops.size() < 2 || stops.size() > 16) throw OpError(message);
        for (const auto& [position, stop] : stops) {
            const double opacity = stop[2].get<double>();
            if (!(0 <= position && position <= 1) || !(0 <= opacity && opacity <= 1)) throw OpError(message);
        }
        std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        Json sorted = Json::array();
        for (auto& [position, stop] : stops) sorted.push_back(std::move(stop));
        out["stops"] = std::move(sorted);
    }
    if (const Json* ratio = get(g, "ratio"); ratio != nullptr && !ratio->is_null()) out["ratio"] = clamp(to_float(*ratio), 0.05, 20.0);
    const Json* repeat = get(g, "repeat");
    if (repeat != nullptr && !repeat->is_null() && *repeat != Json("") && *repeat != Json("none")) {
        if (*repeat != Json("repeat") && *repeat != Json("mirror")) throw OpError("repeat is none, repeat or mirror");
        out["repeat"] = py_str(*repeat);
    }
    return out;
}

// ops._fill_spec: a fill layer's colour, or its gradient.
Json fill_spec(const Json& raw) {
    if (!raw.is_object()) throw OpError("fill is {rgb} or {gradient}");
    const Json* gradient = get(raw, "gradient");
    if (gradient != nullptr && !gradient->is_null()) {
        const Json g = gradient->is_object() ? *gradient : Json::object();
        const auto floats_of = [&](const char* key, const Json& fallback) {
            const Json* v = get(g, key);
            Json list = Json::array();
            for (const Json& item : iterate(v != nullptr && py_truthy(*v) ? *v : fallback)) list.push_back(to_float(item));
            Json out = Json::array();
            for (std::size_t i = 0; i < list.size() && i < 2; ++i) out.push_back(list[i]);
            return out;
        };
        Json out = Json::object();
        out["from"] = floats_of("from", Json::array({0, 0}));
        out["to"] = floats_of("to", Json::array({0, 100}));
        const Json* rgb_from = get(g, "rgb_from");
        out["rgb_from"] = ints_json(rgb3(rgb_from != nullptr && py_truthy(*rgb_from) ? *rgb_from : Json::array({20, 20, 20}), "rgb_from"));
        const Json* rgb_to = get(g, "rgb_to");
        out["rgb_to"] = ints_json(rgb3(rgb_to != nullptr && py_truthy(*rgb_to) ? *rgb_to : Json::array({255, 255, 255}), "rgb_to"));
        out["opacity_from"] = clamp(to_float(value_or(g, "opacity_from", Json(1.0))), 0.0, 1.0);
        out["opacity_to"] = clamp(to_float(value_or(g, "opacity_to", Json(1.0))), 0.0, 1.0);
        const Json* shape = get(g, "shape");
        out["shape"] = shape != nullptr && py_truthy(*shape) ? py_str(*shape) : std::string("linear");
        const Json extras = gradient_extras(g);
        for (const auto& [key, value] : extras.items()) out[key] = value;
        const std::string& s = out["shape"].get_ref<const std::string&>();
        if (s != "linear" && s != "radial" && s != "ellipse") throw OpError("shape must be linear, radial or ellipse");
        if (out["from"].size() != 2 || out["to"].size() != 2) throw OpError("from and to are [x_mm, y_mm]");
        return Json::object({{"gradient", std::move(out)}});
    }
    const Json* rgb = get(raw, "rgb");
    return Json::object({{"rgb", ints_json(rgb3(rgb != nullptr && py_truthy(*rgb) ? *rgb : Json::array({255, 255, 255}), "rgb"))}});
}

// ops._effect_spec: 境界効果 (a border round the layer's picture, a watercolour edge).
Json effect_spec(const Json& raw) {
    if (!raw.is_object()) throw OpError("effect is {border} and/or {water_edge}");
    Json out = Json::object();
    for (const auto& [key, item] : raw.items()) {
        if (key == "border" && py_truthy(item)) {
            const Json value = item.is_object() ? item : Json::object();
            Json border = Json::object();
            border["width_mm"] = clamp(to_float(value_or(value, "width_mm", Json(0.5))), 0.05, 10.0);
            const Json* rgb = get(value, "rgb");
            border["rgb"] = ints_json(rgb3(rgb != nullptr && py_truthy(*rgb) ? *rgb : Json::array({255, 255, 255}), "rgb"));
            out["border"] = std::move(border);
        } else if (key == "water_edge" && py_truthy(item)) {
            const Json value = item.is_object() ? item : Json::object();
            Json edge = Json::object();
            edge["width_mm"] = clamp(to_float(value_or(value, "width_mm", Json(0.6))), 0.05, 10.0);
            edge["strength"] = clamp(to_float(value_or(value, "strength", Json(0.6))), 0.0, 1.0);
            out["water_edge"] = std::move(edge);
        } else if (key != "border" && key != "water_edge") {
            throw OpError("unknown effect " + key + " (border, water_edge)");
        }
    }
    return out;
}

// ops._screen_spec: レイヤーのトーン化 (the layer's greys printed as a halftone).
Json screen_spec(const Json& raw) {
    if (!raw.is_object()) throw OpError("screen is {pattern, lpi, angle}");
    const Json* pattern_value = get(raw, "pattern");
    const std::string pattern = pattern_value != nullptr && py_truthy(*pattern_value) ? py_str(*pattern_value) : "dot";
    if (pattern != "dot" && pattern != "line" && pattern != "cross" && pattern != "noise") {
        throw OpError("screen pattern must be dot, line, cross or noise");
    }
    const double lpi = to_float(value_or(raw, "lpi", Json(60)));
    if (!(10 <= lpi && lpi <= 150)) throw OpError("lpi must be between 10 and 150");
    const Json* shape_value = get(raw, "shape");
    const std::string shape = shape_value != nullptr && py_truthy(*shape_value) ? py_str(*shape_value) : "round";
    if (shape != "round" && shape != "square" && shape != "diamond" && shape != "ellipse") {
        throw OpError("screen shape must be one of round, square, diamond, ellipse");
    }
    Json spec = Json::object();
    spec["pattern"] = pattern;
    spec["lpi"] = lpi;
    spec["angle"] = py_fmod(to_float(value_or(raw, "angle", Json(45))), 180);
    spec["black"] = clamp(to_float(value_or(raw, "black", Json(0.1))), 0.0, 0.9);
    spec["white"] = clamp(to_float(value_or(raw, "white", Json(0.95))), 0.1, 1.0);
    if (shape != "round") spec["shape"] = shape;
    if (truthy_at(raw, "offset_mm")) {
        Json offset = Json::array();
        try {
            for (const Json& v : iterate(raw["offset_mm"])) offset.push_back(to_float(v));
        } catch (const PyTypeError&) {
            throw OpError("offset_mm is [x, y] in mm");
        } catch (const PyValueError&) {
            throw OpError("offset_mm is [x, y] in mm");
        }
        Json two = Json::array();
        for (std::size_t i = 0; i < offset.size() && i < 2; ++i) two.push_back(offset[i]);
        spec["offset_mm"] = std::move(two);
    }
    return spec;
}

// set_layer on the book (also each layer of set_layers).
void set_layer_with(Document& doc, const Json& op) {
    const std::size_t at = require_page(doc, op);
    Page& page = doc.edit_page(at);
    std::optional<std::size_t> li;
    if (truthy_at(op, "id")) {
        const Json& id = op["id"];
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (id.is_string() && page.layers[i].id == id.get_ref<const std::string&>()) {
                li = i;
                break;
            }
        }
    }
    if (!li && truthy_at(op, "layer")) li = layer_for_role(page, role_from(Json(py_str(op["layer"]))));
    if (!li) throw OpError("layer id or role required");
    Layer& layer = page.layers[*li];
    if (has(op, "visible")) layer.visible = py_truthy(op["visible"]);
    if (has(op, "opacity")) layer.opacity = to_float(op["opacity"]);
    if (has(op, "exportable") && layer.role != LayerRole::Name && layer.role != LayerRole::Draft) {
        layer.exportable = py_truthy(op["exportable"]);
    }
    if (has(op, "blend")) layer.blend = blend_mode(op["blend"]);
    if (has(op, "clip")) layer.clip = py_truthy(op["clip"]);
    if (has(op, "lock_alpha")) layer.lock_alpha = py_truthy(op["lock_alpha"]);
    if (has(op, "locked")) layer.locked = py_truthy(op["locked"]);
    if (has(op, "panel_clip")) layer.panel_clip = py_truthy(op["panel_clip"]);
    if (has(op, "panel_each")) layer.panel_each = py_truthy(op["panel_each"]);
    if (has(op, "title")) layer.title = py_truthy(op["title"]) ? py_str(op["title"]) : "";
    if (has(op, "parent")) layer.parent_id = op["parent"];
    if (has(op, "name")) layer.title = py_str(op["name"]);
    if (has(op, "color")) {
        if (py_truthy(op["color"])) {
            std::vector<std::int64_t> color = int_tuple(op["color"]);
            if (color.size() > 3) color.resize(3);
            layer.color = std::move(color);
        } else {
            layer.color.reset();
        }
    }
    if (has(op, "reference")) layer.reference = py_truthy(op["reference"]);
    if (has(op, "color_prints")) layer.color_prints = py_truthy(op["color_prints"]);
    if (has(op, "fill")) {
        if (layer.kind != LayerKind::Fill) throw OpError("fill is set on a fill layer");
        if (py_truthy(op["fill"])) {
            layer.fill = fill_spec(op["fill"]);
        } else {
            layer.fill.reset();
        }
    }
    if (has(op, "adjust")) {
        if (layer.kind != LayerKind::Adjust) throw OpError("adjust is set on a correction layer");
        layer.adjust = adjust_spec(op["adjust"]);
    }
    if (has(op, "effect")) {
        if (py_truthy(op["effect"])) {
            layer.effect = effect_spec(op["effect"]);
        } else {
            layer.effect.reset();
        }
    }
    if (has(op, "screen")) {
        if (py_truthy(op["screen"])) {
            layer.screen = screen_spec(op["screen"]);
        } else {
            layer.screen.reset();
        }
    }
}

void set_layer(OpContext& c) { set_layer_with(c.doc, c.op); }

void add_layer(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const std::string kind_name = truthy_at(op, "folder") ? std::string("folder")
                                  : truthy_at(op, "kind") ? py_str(op["kind"])
                                                          : std::string("paint");
    static const std::pair<const char*, LayerKind> kKinds[] = {
        {"folder", LayerKind::Folder}, {"pen", LayerKind::Strokes}, {"paint", LayerKind::Raster},
        {"fill", LayerKind::Fill},     {"gradient", LayerKind::Fill}, {"adjust", LayerKind::Adjust}};
    const auto kind = std::find_if(std::begin(kKinds), std::end(kKinds), [&](const auto& k) { return kind_name == k.first; });
    if (kind == std::end(kKinds)) throw OpError("kind must be pen, paint, folder, fill, gradient or adjust");
    Layer layer;
    layer.id = truthy_at(op, "id") ? py_str(op["id"]) : new_id();
    layer.role = LayerRole::User;
    layer.kind = kind->second;
    layer.title = truthy_at(op, "name") ? py_str(op["name"]) : std::string("layer");
    layer.blend = blend_mode(truthy_at(op, "blend") ? op["blend"] : Json("normal"));
    layer.clip = truthy_at(op, "clip");
    layer.lock_alpha = truthy_at(op, "lock_alpha");
    layer.parent_id = truthy_at(op, "parent") ? Json(py_str(op["parent"])) : Json(nullptr);
    layer.exportable = true;
    // (a new drawing layer keeps each line in the panel it began in, as CLIP STUDIO's panel folders do)
    layer.panel_each = op.contains("panel_each") ? py_truthy(op["panel_each"]) : (kind_name == "pen" || kind_name == "paint");
    if (kind_name == "fill") {
        layer.fill = fill_spec(Json::object({{"rgb", truthy_at(op, "rgb") ? op["rgb"] : Json::array({255, 255, 255})}}));
        layer.title = truthy_at(op, "name") ? py_str(op["name"]) : std::string("ベタ塗り");
    } else if (kind_name == "gradient") {
        layer.fill = fill_spec(Json::object({{"gradient", truthy_at(op, "gradient") ? op["gradient"] : Json::object()}}));
        layer.title = truthy_at(op, "name") ? py_str(op["name"]) : std::string("グラデーション");
    } else if (kind_name == "adjust") {
        layer.adjust = adjust_spec(truthy_at(op, "adjust") ? op["adjust"] : Json::object({{"kind", "levels"}}));
        layer.title = truthy_at(op, "name") ? py_str(op["name"]) : std::string("色調補正");
    }
    Page& page = doc.edit_page(at);
    for (const Layer& item : page.layers) {
        if (item.id == layer.id) throw OpError("layer " + layer.id + " exists");
    }
    std::size_t index = page.layers.size();
    if (truthy_at(op, "after")) {
        const Json& after = op["after"];
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (after.is_string() && page.layers[i].id == after.get_ref<const std::string&>()) {
                index = i + 1;
                break;
            }
        }
    }
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
}

void delete_layer(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    const std::string layer_id = truthy_at(c.op, "id") ? py_str(c.op["id"]) : std::string();
    const Page& view = doc.page(at);
    const auto found = std::find_if(view.layers.begin(), view.layers.end(), [&](const Layer& l) { return l.id == layer_id; });
    if (found == view.layers.end()) throw OpError("layer not found");
    const LayerRole role = found->role;
    if (role == LayerRole::Name || role == LayerRole::Ink || role == LayerRole::Bg || role == LayerRole::Finish) {
        throw OpError("cannot delete core layer");
    }
    std::erase_if(doc.edit_page(at).layers, [&](const Layer& l) { return l.id == layer_id; });
}

void duplicate_layer(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    Page& page = doc.edit_page(at);
    const std::size_t source_at = layer_by_id(page, truthy_at(c.op, "id") ? py_str(c.op["id"]) : std::string());
    const Layer& source = page.layers[source_at];
    if (source.kind == LayerKind::Folder) throw OpError("a folder cannot be duplicated");
    Layer twin = source;
    twin.identity = new_layer_identity();  // (copy.deepcopy: another layer)
    twin.id = truthy_at(c.op, "new_id") ? py_str(c.op["new_id"]) : new_id();
    for (const Layer& item : page.layers) {
        if (item.id == twin.id) throw OpError("layer " + twin.id + " exists");
    }
    // the copy is an ordinary layer (a book has one ink layer, one name layer…)
    if (source.kind != LayerKind::Placed) twin.role = LayerRole::User;
    std::string base = source.title;
    if (base.empty()) {
        static const std::pair<LayerRole, const char*> kTitles[] = {{LayerRole::Name, "ネーム"},
                                                                    {LayerRole::Draft, "下描き"},
                                                                    {LayerRole::Ink, "ペン入れ"},
                                                                    {LayerRole::Bg, "背景"},
                                                                    {LayerRole::Finish, "仕上げ"}};
        base = "レイヤー";
        for (const auto& [role, title] : kTitles) {
            if (role == source.role) base = title;
        }
    }
    twin.title = base + " のコピー";
    std::vector<StrokePtr> strokes;
    for (const StrokePtr& stroke : source.strokes->items) {
        Stroke fresh = *stroke;
        fresh.id = new_id();
        strokes.push_back(std::make_shared<const Stroke>(std::move(fresh)));
    }
    twin.strokes = make_strokes(std::move(strokes));
    for (Patch& patch : twin.patches) {
        const std::string id = new_id();
        if (patch.attrs.contains("id")) {
            patch.attrs["id"] = id;
        } else if (patch.asset) {
            patch.after_asset["id"] = id;  // (Python's dict has "asset" by now: the new key comes after it)
        } else {
            patch.attrs["id"] = id;
        }
    }
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(source_at) + 1, std::move(twin));
}

void set_layers(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const Page& page = doc.page(at);
    std::vector<std::string> ids;
    if (truthy_at(op, "all")) {
        for (const Layer& layer : page.layers) {
            if (layer.kind != LayerKind::Folder || has(op, "visible")) ids.push_back(layer.id);
        }
    } else {
        std::vector<std::string> wanted;  // layerops._ids
        const Json* given = get(op, "ids");
        if (given != nullptr && py_truthy(*given)) {
            for (const Json& v : iterate(*given)) wanted.push_back(py_str(v));
        }
        if (wanted.empty()) throw OpError("ids is the list of layer ids");
        for (const std::string& id : wanted) {  // layerops._layers
            const bool found = std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
            if (!found) throw OpError("no layer " + id);
        }
        const std::set<std::string> chosen(wanted.begin(), wanted.end());
        for (const Layer& layer : page.layers) {
            if (chosen.contains(layer.id)) ids.push_back(layer.id);
        }
    }
    static const char* const kSettable[] = {"visible", "opacity", "blend", "clip", "lock_alpha", "locked", "panel_clip",
                                            "panel_each", "color", "reference", "exportable", "color_prints", "effect"};
    Json fields = Json::object();
    for (const char* key : kSettable) {
        if (has(op, key)) fields[key] = op[key];
    }
    if (fields.empty()) {
        std::string names;
        for (const char* key : kSettable) names += (names.empty() ? "" : ", ") + std::string(key);
        throw OpError("set_layers needs something to set (" + names + ")");
    }
    const Json page_index = page.index.json();
    for (const std::string& id : ids) {
        Json one = Json::object();
        one["op"] = "set_layer";
        one["page"] = page_index;
        one["id"] = id;
        for (const auto& [key, value] : fields.items()) one[key] = value;
        set_layer_with(doc, one);
    }
}

void reorder_layers(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = require_page(doc, c.op);
    const Json* order_value = get(c.op, "order");
    const Json order = order_value != nullptr && py_truthy(*order_value) ? *order_value : Json::array();
    const std::vector<Json> items = iterate(order);
    Page& page = doc.edit_page(at);
    const auto by_id = [&page](const std::string& id) -> const Layer* {
        const Layer* found = nullptr;
        for (const Layer& layer : page.layers) {
            if (layer.id == id) found = &layer;  // (a dict: the last layer with the id)
        }
        return found;
    };
    std::vector<Layer> layers;
    for (const Json& item : items) {
        require_hashable(item);
        if (!item.is_string()) continue;
        if (const Layer* layer = by_id(item.get_ref<const std::string&>())) layers.push_back(*layer);
    }
    page.layers = std::move(layers);
}

}  // namespace

void register_layer_ops(OpRegistry& registry) {
    registry.add("add_layer", add_layer);
    registry.add("delete_layer", delete_layer);
    registry.add("duplicate_layer", duplicate_layer);
    registry.add("set_layer", set_layer);
    registry.add("set_layers", set_layers);
    registry.add("reorder_layers", reorder_layers);
}

}  // namespace genko::core
