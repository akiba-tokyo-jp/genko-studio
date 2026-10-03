// The tone ops (Python's ops._apply_one: add_tone with _new_tone, set_tone, delete_tone). A tone layer's patch (its
// area, panel or the region a fill would take) is made here as Python makes it (genko/fill.py), so these ops are in
// genko_render_ops. (An area of a rect or an ellipse comes resolved to its polygon by the CommandBus, as Python's
// apply_ops resolves it.)
//
// Where Python would keep a number that is not finite (an offset or angle of "inf" or "nan", written by its json.dumps
// as Infinity / NaN), the op is refused: a book never holds one (docs/cpp-migration/ARCHITECTURE.md §3). Python's
// delete_tone takes away any layer of the id it is given, a pen or paint layer with its drawing too: refused here
// (docs/cpp-migration/SPEC.md COMP-01a).

#include <cmath>
#include <optional>

#include "core/command_bus.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/fill_patches.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/tones.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpContext;
using core::OpError;

const Json* given(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr && !value->is_null() ? value : nullptr;
}

const Json* truthy(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr && core::py_truthy(*value) ? value : nullptr;
}

// A tone layer, as set_tone takes one (its kind or its role is tone).
bool is_tone(const core::Layer& layer) { return layer.kind == core::LayerKind::Tone || layer.role == core::LayerRole::Tone; }

// The op is refused when the layer would keep a number that is not finite (Python keeps it and writes Infinity or NaN
// into project.json). Checked once the op has done everything else, so every other error comes first, as in Python.
void require_finite_tone(const core::Layer& layer) {
    if (!std::isfinite(layer.angle)) throw OpError("angle must be a finite number");
    if (layer.tone) {
        core::require_finite(*layer.tone);  // (offset_mm, gradient.angle, …)
        // Gradient angles are stored as supplied, including numeric strings; inspect the value the renderer reads.
        const Json* gradient = core::get(*layer.tone, "gradient");
        if (gradient != nullptr && core::py_truthy(*gradient)) {
            const Json* angle = core::get(*gradient, "angle");
            if (angle != nullptr && !std::isfinite(core::to_float(*angle))) {
                throw OpError("gradient.angle must be a finite number");
            }
        }
    }
}

// _tone_screen_keys(tone, op): 網の種類 (dot_shape) and 網のずれ (offset_mm; move_by adds to it); null takes them away.
void tone_screen_keys(Json& tone, const Json& op) {
    if (const Json* shape = core::get(op, "dot_shape")) {
        if (shape->is_null() || (shape->is_string() && (shape->get<std::string>().empty() || shape->get<std::string>() == "round"))) {
            tone.erase("dot_shape");
        } else {
            tone["dot_shape"] = core::py_str(*shape);
        }
    }
    const Json* offset = core::get(op, "offset_mm");
    const Json* move = truthy(op, "move_by_mm");
    if (offset == nullptr && move == nullptr) return;
    const Json base = offset != nullptr ? *offset : core::py_get(tone, "offset_mm");
    double x = 0.0;
    double y = 0.0;
    try {
        const auto pair = [](const Json& value) {
            const std::vector<Json> items = core::iterate(value);
            if (items.size() != 2) throw core::PyValueError("unpack");
            return std::pair<double, double>{core::to_float(items[0]), core::to_float(items[1])};
        };
        std::tie(x, y) = pair(core::py_truthy(base) ? base : Json::array({0, 0}));
        if (move != nullptr) {
            const auto [ax, ay] = pair(*move);
            x = x + ax;
            y = y + ay;
        }
    } catch (const core::PyValueError&) {
        throw OpError("offset_mm is [x, y] in mm");
    } catch (const core::PyTypeError&) {
        throw OpError("offset_mm is [x, y] in mm");
    }
    if (x == 0 && y == 0) {
        tone.erase("offset_mm");
    } else {
        tone["offset_mm"] = Json::array({core::py_round(x, 3), core::py_round(y, 3)});
    }
}

// _tone_numbers(layer, op)
void tone_numbers(core::Layer& layer, const Json& op) {
    if (const Json* value = given(op, "lpi")) {
        const double lpi = core::to_float(*value);
        if (!(5 <= lpi && lpi <= 300)) throw OpError("lpi is 5 to 300");
        layer.lpi = core::Num(lpi);
    }
    if (const Json* value = given(op, "density")) {
        const double density = core::to_float(*value);
        if (!(0 <= density && density <= 1)) throw OpError("density is 0 to 1 (the black share)");
        layer.density = core::Num(density);
    }
    if (const Json* value = given(op, "angle")) layer.angle = core::to_float(*value);
}

void validated(const Json& tone) {
    try {
        tones::validate(tone);
    } catch (const core::PyValueError& error) {
        throw OpError(error.what());
    }
}

// ops._area(op): a {poly} or {mask} area.
Json area_of(const Json& op) {
    const Json area = core::py_or(core::py_get(op, "area"), Json::object());
    const Json poly = core::py_get(area, "poly");
    if (core::py_truthy(poly)) {
        if (core::length(poly) < 3) throw OpError("an area needs at least three corners");
        return area;
    }
    const Json mask = core::py_get(area, "mask");
    if (core::py_truthy(mask) && core::py_truthy(core::py_get(mask, "box")) && core::py_truthy(core::py_get(mask, "png"))) return area;
    throw OpError("area is {poly: [[x, y], …]} or {mask: {box, png}}");
}

std::vector<std::array<double, 2>> points_of(const Json& points) {
    std::vector<std::array<double, 2>> out;
    for (const Json& p : core::iterate(points)) {
        const double x = core::to_float(core::subscript(p, 0));
        const double y = core::to_float(core::subscript(p, 1));
        out.push_back({x, y});
    }
    return out;
}

// ops._fill_reference(episode, page, target, reference, dpi): what a fill looks at.
Image fill_reference(const core::Document& doc, const core::Page& page, const core::Layer& target, const std::string& reference,
                     int dpi) {
    if (reference == "page") {
        RenderOptions options;
        options.mode = !page.name_ok ? "name" : "proof";
        try {
            return render_page(page, dpi, options, &doc).image.convert("L");
        } catch (const NotYetPorted& missing) {
            throw OpError("the page has what this build does not draw yet (" + missing.element() +
                          "): a fill cannot see it; fill by an area or a panel instead");
        }
    }
    if (reference != "layer" && reference != "reference") throw OpError("reference must be page, layer or reference");
    std::vector<const core::Layer*> looked;
    if (reference == "layer") {
        looked.push_back(&target);
    } else {
        for (const core::Layer& layer : page.layers) {
            if (layer.reference) looked.push_back(&layer);
        }
    }
    if (looked.empty()) throw OpError("no layer is set as the reference (set_layer reference: true)");
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    Image base = Image::create("RGBA", size, Ink{255, 255, 255, 255});
    for (const core::Layer* layer : looked) {
        if (layer->raster_png && !layer->raster_png->empty()) {
            base = alpha_composite(base, open_image(*layer->raster_png, kPillowOpenLimits).convert("RGBA").resize(size));
        }
        // render._layer_strokes(layer, size, dpi, None, None): the layer's fills and lines alone, no panel cut — as
        // layer_image draws a plain pen layer on a page without panels
        if (layer->stroke_count() > 0 || !layer->patches.empty()) {
            core::Page bare = page;
            bare.frames.clear();
            core::Layer lines = *layer;
            lines.raster_png.reset();
            lines.mask.reset();
            lines.effect.reset();
            lines.lock_alpha = false;
            lines.kind = core::LayerKind::Strokes;
            if (lines.role == core::LayerRole::Tone) lines.role = core::LayerRole::User;
            bare.layers = {lines};
            base = alpha_composite(base, layer_image(bare, bare.layers.front(), dpi, nullptr, false));
        }
    }
    Image image = base.convert("RGB");
    detail::draw_frames(image, Box{0, 0, size.width, size.height}, page, size, dpi);
    return image.convert("L");
}

// _new_tone(episode, page, op): a tone layer and where it goes.
core::Layer new_tone(const core::Document& doc, const core::Page& page, const Json& op) {
    Json tone = Json::object();
    const Json* pattern = truthy(op, "pattern");
    tone["pattern"] = pattern != nullptr ? core::py_str(*pattern) : std::string("dot");
    if (const Json* scale = given(op, "scale_mm")) tone["scale_mm"] = core::to_float(*scale);
    if (const Json* picture = given(op, "tile_png")) tone["tile_png"] = core::py_str(*picture);
    if (const Json* gradient = truthy(op, "gradient")) tone["gradient"] = core::py_dict(*gradient);
    tone_screen_keys(tone, op);
    validated(tone);
    core::Layer layer;
    const Json* id = truthy(op, "id");
    layer.id = id != nullptr ? core::py_str(*id) : core::new_id();
    layer.role = core::LayerRole::Tone;
    layer.kind = core::LayerKind::Tone;
    layer.lpi = core::Num(60.0);
    layer.density = core::Num(0.3);
    layer.exportable = true;
    layer.angle = 45.0;
    layer.tone = tone;
    const Json* name = truthy(op, "name");
    layer.title = name != nullptr ? core::py_str(*name) : std::string();
    for (const core::Layer& item : page.layers) {
        if (item.id == layer.id) throw OpError("layer " + layer.id + " already exists");
    }
    tone_numbers(layer, op);
    std::optional<core::Patch> patch;
    const std::vector<std::int64_t> black{0, 0, 0};
    if (truthy(op, "area") != nullptr) {
        const Json area = area_of(op);
        if (core::py_truthy(core::py_get(area, "poly"))) {
            patch = tone_fills::polygon_patch(points_of(area.at("poly")), black);
        } else {
            const auto [mask, origin] = tone_fills::area_mask(area);
            patch = tone_fills::mask_patch(mask, tone_fills::kFillDpi, black, 1.0, origin);
        }
    } else if (const Json* frame_id = truthy(op, "frame_id")) {
        const core::Frame& frame = core::frame_or_fail(page, *frame_id);
        std::vector<std::array<double, 2>> outline;
        for (const core::Point& p : core::outline(frame)) outline.push_back({p.x.value(), p.y.value()});
        patch = tone_fills::polygon_patch(outline, black);
    } else if (const Json* at_value = truthy(op, "at")) {
        const Json at = core::py_dict(*at_value);
        const int dpi = tone_fills::kFillDpi;
        if (!at.contains("x_mm")) throw core::OpKeyError("'x_mm'");
        const double x = core::to_float(at["x_mm"]);
        if (!at.contains("y_mm")) throw core::OpKeyError("'y_mm'");
        const double y = core::to_float(at["y_mm"]);
        const Json reference_value = core::py_or(core::py_get(at, "reference"), Json("page"));
        const core::Layer& target = page.layers.empty() ? layer : page.layers.front();
        const Image reference = fill_reference(doc, page, target, core::py_str(reference_value), dpi);
        std::optional<Box> window;
        if (const core::Frame* panel = page.frame_at(core::Num(x), core::Num(y))) {
            const core::Rect& r = panel->rect;
            window = Box{std::max(0, tone_fills::px((r.x - core::Num(2)).value(), dpi)), std::max(0, tone_fills::px((r.y - core::Num(2)).value(), dpi)),
                         std::min(reference.width(), tone_fills::px((r.x + r.width + core::Num(2)).value(), dpi)),
                         std::min(reference.height(), tone_fills::px((r.y + r.height + core::Num(2)).value(), dpi))};
        }
        const double gap = core::to_float(core::py_or(core::py_get(at, "gap_mm", Json(0.3)), Json(0)));
        const auto mask = tone_fills::region_mask(reference, Point{tone_fills::px(x, dpi), tone_fills::px(y, dpi)}, tone_fills::px(gap, dpi), 160, 1, window);
        if (!mask) throw OpError("nothing to fill there (the click is on a line)");
        patch = tone_fills::mask_patch(*mask, dpi, black);
    }
    if (patch) {
        layer.patches.push_back(std::move(*patch));
    } else if (truthy(op, "area") != nullptr || truthy(op, "frame_id") != nullptr || truthy(op, "at") != nullptr) {
        throw OpError("the area is empty");
    }
    return layer;
}

void add_tone(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    core::Layer layer = new_tone(c.doc, c.doc.page(at), c.op);
    core::Page& page = c.doc.edit_page(at);
    std::size_t place = page.layers.size();
    if (const Json* after = truthy(c.op, "after")) {
        place = page.layers.size() + 1;
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (core::py_equals(Json(page.layers[i].id), *after)) {
                place = i + 1;
                break;
            }
        }
        if (place > page.layers.size()) throw OpError("no layer " + core::py_str(*after));
    }
    require_finite_tone(layer);
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(place), std::move(layer));
}

void set_tone(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* id = core::get(c.op, "id");
    const std::size_t index = core::layer_by_id(c.doc.page(at), core::py_str(id != nullptr ? *id : Json()));
    core::Page& page = c.doc.edit_page(at);
    core::Layer& layer = page.layers[index];
    if (!is_tone(layer)) throw OpError("that layer is not a tone");
    Json tone = layer.tone && layer.tone->is_object() ? *layer.tone : Json::object();
    if (const Json* pattern = truthy(c.op, "pattern")) tone["pattern"] = core::py_str(*pattern);
    for (const char* key : {"scale_mm", "tile_png"}) {
        if (const Json* value = core::get(c.op, key)) {
            if (value->is_null()) {
                tone.erase(key);
            } else {
                tone[key] = std::string(key) == "scale_mm" ? Json(core::to_float(*value)) : Json(core::py_str(*value));
            }
        }
    }
    if (const Json* gradient = core::get(c.op, "gradient")) tone["gradient"] = core::py_truthy(*gradient) ? core::py_dict(*gradient) : Json();
    tone_screen_keys(tone, c.op);
    validated(tone);
    layer.tone = tone;
    tone_numbers(layer, c.op);
    if (const Json* name = truthy(c.op, "name")) layer.title = core::py_str(*name);
    require_finite_tone(layer);
}

void delete_tone(OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* id = core::get(c.op, "id");
    const Json tone_id = id != nullptr ? *id : Json();
    const core::Page& seen = c.doc.page(at);
    std::vector<core::Layer> kept;
    for (const core::Layer& layer : seen.layers) {
        if (!core::py_equals(Json(layer.id), tone_id)) {
            kept.push_back(layer);
        } else if (!is_tone(layer)) {
            // (Python takes this layer away too, its drawing with it: a tone op does not delete another kind of layer)
            throw OpError("layer " + layer.id + " is not a tone layer");
        }
    }
    if (kept.size() == seen.layers.size()) throw OpError("no tone " + core::py_str(tone_id));
    c.doc.edit_page(at).layers = std::move(kept);
}

}  // namespace

void register_tone_ops(core::OpRegistry& registry) {
    registry.add("add_tone", add_tone);
    registry.add("set_tone", set_tone);
    registry.add("delete_tone", delete_tone);
}

}  // namespace genko::render
