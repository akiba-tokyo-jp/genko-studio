// vector@1 (PC3 C-a, SPEC.md COLOR-07): a page's paths (set_path, delete_path, rename_path; make_work_path from an area,
// text_to_path from a line's letters; stroke_path along one) and a layer's vector mask (set_vector_mask) — PhotoCraft
// v0.2.0's engine/src/vector_cmds.rs (482-1064) in mm (MIT, native/third_party/photocraft/NOTICE.md). A path's
// coverage is render/path_raster's, an area of a path render/path_areas', a vector mask drawn render/vector_masks'.

#include "render/ops_paths.hpp"

#include <algorithm>
#include <cmath>

#include "core/areas.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/vector.hpp"
#include "render/color_canvas.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_shapes.hpp"
#include "render/path_areas.hpp"
#include "render/path_raster.hpp"
#include "render/path_trace.hpp"
#include "render/selection.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::OpContext;
using core::OpError;
using core::Page;
namespace vec = core::vector;

// A path, a name, a number as the op gives it: its core::Error("value") an OpError naming the key.
vec::Path path_of(const Json& value, std::string_view key) {
    try {
        return vec::path_from_json(value);
    } catch (const core::Error& error) {
        throw OpError(std::string(key) + ": " + error.what());
    }
}

const Json* given(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr && !value->is_null() ? value : nullptr;
}

// A saved path's name as the op gives it (trimmed; 1..64 characters).
std::string saved_name(const Json& name, std::string_view key) {
    if (!name.is_string()) throw OpError(std::string(key) + " is a path's name (a string) or \"work\"");
    try {
        return vec::path_name(name.get<std::string>());
    } catch (const core::Error& error) {
        throw OpError(std::string(key) + ": " + error.what());
    }
}

vec::PathOp path_op(const Json& value) {
    if (value.is_string()) {
        if (const auto op = vec::path_op_from(value.get<std::string>())) return *op;
    }
    throw OpError("append is combine, subtract, intersect or exclude");
}

bool boolean(const Json& value, std::string_view key) {
    if (!value.is_boolean()) throw OpError(std::string(key) + " must be true or false");
    return value.get<bool>();
}

double number(const Json& value, std::string_view key, double lo, double hi) {
    const double v = core::finite_float(value, key);
    if (v < lo || v > hi) throw OpError(std::string(key) + " must be " + core::py_repr(Json(lo)) + ".." + core::py_repr(Json(hi)));
    return v;
}

std::size_t knots_of(const vec::Path& path) { return path.knot_count(); }

// The path in pixels of a grid of `dpi` brought back to mm on the page.
vec::Path to_mm(const vec::Path& pixels, double dpi) {
    const double k = 25.4 / dpi;
    return pixels.transformed({k, 0.0, 0.0, k, 0.0, 0.0});
}

// A coverage ("L") traced into a path in mm (`dpi` the picture's; `x0`, `y0` where its top left lies on that grid).
vec::Path traced(const Image& mask, double dpi, int x0, int y0, double tolerance_px) {
    const std::string bytes = mask.tobytes();
    std::vector<float> values(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    const path_raster::PixelBox box{x0, y0, x0 + mask.width(), y0 + mask.height()};
    return to_mm(path_trace::trace_mask(values, box, 0.0f, tolerance_px), dpi);
}

// --- the page's paths -----------------------------------------------------------------------------------------------

// {page, name?: "work" | a name (default "work"), path, append?: combine | subtract | intersect | exclude} → {name, path}:
// the path set (with append: its subpaths added to the path there, each with that operation — upstream's "op", which is
// every op's own name here).
void set_path(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* raw = given(c.op, "path");
    if (raw == nullptr) throw OpError("path is required ({\"subpaths\": [...]})");
    vec::Path path = path_of(*raw, "path");
    const std::optional<vec::PathOp> append = given(c.op, "append") != nullptr ? std::optional(path_op(*given(c.op, "append"))) : std::nullopt;
    const Json* name = given(c.op, "name");
    const auto merged = [&](const vec::Path* old) {
        if (old == nullptr || !append) return path;
        vec::Path out = *old;
        for (vec::Subpath s : path.subpaths) {
            s.op = *append;
            out.subpaths.push_back(std::move(s));
        }
        return out;
    };
    Page& page = doc.edit_page(at);
    std::string said = "work";
    const vec::Path* now = nullptr;
    if (path_areas::is_work(name)) {
        page.work_path = merged(page.work_path ? &*page.work_path : nullptr);
        now = &*page.work_path;
    } else {
        said = saved_name(*name, "name");
        auto it = std::find_if(page.paths.begin(), page.paths.end(), [&](const vec::NamedPath& p) { return p.name == said; });
        if (it != page.paths.end()) {
            it->path = merged(&it->path);
            now = &it->path;
        } else {
            page.paths.push_back(vec::NamedPath{said, merged(nullptr)});
            now = &page.paths.back().path;
        }
    }
    c.report = Json{{"name", said}, {"path", vec::to_json(*now)}};
}

// {page, name?: "work" | a name (default "work")}: the path deleted (a clipping path naming it too).
void delete_path(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* name = given(c.op, "name");
    Page& page = doc.edit_page(at);
    if (path_areas::is_work(name)) {
        if (!page.work_path) throw OpError("the page has no work path");
        page.work_path.reset();
        return;
    }
    const std::string wanted = saved_name(*name, "name");
    const auto it = std::find_if(page.paths.begin(), page.paths.end(), [&](const vec::NamedPath& p) { return p.name == wanted; });
    if (it == page.paths.end()) throw OpError("no path named " + core::py_repr_str(wanted));
    page.paths.erase(it);
    if (page.clipping_path && page.clipping_path->name == wanted) page.clipping_path.reset();
}

// {page, name?: "work" | a name (default "work"), to} → {name}: a saved path renamed (its clipping path follows), or the
// work path saved under that name (Photoshop's 保存).
void rename_path(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* to_value = given(c.op, "to");
    if (to_value == nullptr) throw OpError("to is required (the new name)");
    if (path_areas::is_work(to_value)) throw OpError("to cannot be the work path (\"work\")");
    const std::string to = saved_name(*to_value, "to");
    const Json* name = given(c.op, "name");
    Page& page = doc.edit_page(at);
    const auto named = [&](const std::string& n) {
        return std::find_if(page.paths.begin(), page.paths.end(), [&](const vec::NamedPath& p) { return p.name == n; });
    };
    if (path_areas::is_work(name)) {
        if (!page.work_path) throw OpError("the page has no work path");
        if (named(to) != page.paths.end()) throw OpError("a path named " + core::py_repr_str(to) + " is already on the page");
        page.paths.push_back(vec::NamedPath{to, *page.work_path});
        page.work_path.reset();
    } else {
        const std::string from = saved_name(*name, "name");
        const auto it = named(from);
        if (it == page.paths.end()) throw OpError("no path named " + core::py_repr_str(from));
        if (from != to) {
            if (named(to) != page.paths.end()) throw OpError("a path named " + core::py_repr_str(to) + " is already on the page");
            it->name = to;
            if (page.clipping_path && page.clipping_path->name == from) page.clipping_path->name = to;
        }
    }
    c.report = Json{{"name", to}};
}

// {page, area, tolerance_mm?: 0.06..1.27 (default 0.254: 2 px of the selection's 200 dpi)} → {subpaths, knots}: the
// area's outline (at 200 dpi, its 0.5 contour) fitted with curves as the work path (選択範囲から作業用パスを作成).
void make_work_path(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json area = core::op_area(c.op);
    if (area.is_null()) throw OpError("area is required");
    double tolerance = 0.254;
    if (const Json* t = given(c.op, "tolerance_mm")) tolerance = std::clamp(core::finite_float(*t, "tolerance_mm"), 0.0635, 1.27);
    const int dpi = selection::kSelDpi;
    const Image mask = selection::to_mask(area, doc.page(at), &doc, dpi);
    const vec::Path path = traced(mask, dpi, 0, 0, tolerance / 25.4 * dpi);
    if (path.subpaths.empty()) throw OpError("the area is empty");
    Page& page = doc.edit_page(at);
    page.work_path = path;
    c.report = Json{{"subpaths", static_cast<std::int64_t>(path.subpaths.size())}, {"knots", static_cast<std::int64_t>(knots_of(path))}};
}

// {id, to?: "work" (default) | "shape", page?, layer_id?, fill?, stroke?, name?} → {page, subpaths, knots, notes}: a placed
// line's letters (its text and rubies, drawn as the page draws them: turned, leaning, bent, warped, along its path) as the
// work path of its page — their outline traced at 600 dpi within 1/1200 inch (作業用パスを作成). What is not a letter is
// left out and named in the notes: the balloon, the halo (白フチ), a picture or gradient on the letters (only their shapes
// are traced). to "shape" (シェイプに変換, PC3 C-b): a shape part of that outline instead (filled in the letters' colour
// unless fill says otherwise) on the pen layer layer_id names, or on a new pen layer on top of the page; the line stays
// as it is (a line of dialogue is not a layer) — its report add_shape_part's with the notes.
void text_to_path(OpContext& c) {
    Document& doc = c.doc;
    const Json* id = given(c.op, "id");
    if (id == nullptr || !id->is_string()) throw OpError("id is required (the line's id)");
    const std::string to = given(c.op, "to") != nullptr ? core::py_str(*given(c.op, "to")) : std::string("work");
    if (to != "work" && to != "shape") throw OpError("to is work or shape");
    const std::string line_id = id->get<std::string>();
    const core::StoryLine& line = core::find_line(doc, line_id);
    std::optional<std::size_t> at;
    for (std::size_t k = 0; k < doc.pages.size(); ++k) {
        if (doc.pages[k]->index == line.page_index) {
            at = k;
            break;
        }
    }
    if (!line.page_index.truthy() || !at) throw OpError("the line " + line.id + " is not placed on a page");
    if (const Json* page_given = given(c.op, "page"); page_given != nullptr && !(core::Num::from_json(*page_given) == std::optional(line.page_index))) {
        throw OpError("the line " + line.id + " is on page " + line.page_index.repr());
    }
    const Page& page = doc.page(*at);
    const std::optional<std::string> font_path = doc.font_path.empty() ? std::nullopt : std::optional<std::string>(doc.font_path);
    text::LetterParts parts;
    parts.balloon = false;
    parts.halo = false;
    const auto draw = [&](Image& image, Point origin, Size page_size, int dpi) {
        text::draw_group(text::PagePart{&image, origin, page_size}, {&line}, dpi, false, font_path, nullptr, {}, nullptr, nullptr, &parts);
    };
    // (where the letters are, from the page drawn at 100 dpi; then that part again, finely)
    constexpr int kFind = 100, kTrace = 600;
    const Size find_size{static_cast<int>(text::px(page.spec.width_mm.value(), kFind)), static_cast<int>(text::px(page.spec.height_mm.value(), kFind))};
    limits::check_picture(find_size.width, find_size.height, "the page");
    Image found = Image::create("RGBA", find_size, Ink{0, 0, 0, 0});
    draw(found, Point{0, 0}, find_size, kFind);
    const std::optional<Box> box = found.getbbox();
    if (!box) throw OpError("the line " + line.id + " has no letters to trace");
    const Size page_size{static_cast<int>(text::px(page.spec.width_mm.value(), kTrace)), static_cast<int>(text::px(page.spec.height_mm.value(), kTrace))};
    const double k = static_cast<double>(kTrace) / kFind;
    const int margin = kTrace / 10;  // (2.5 mm round what the coarse drawing found)
    const Box part{std::max(0, static_cast<int>(std::floor(box->x0 * k)) - margin), std::max(0, static_cast<int>(std::floor(box->y0 * k)) - margin),
                   std::min(page_size.width, static_cast<int>(std::ceil(box->x1 * k)) + margin),
                   std::min(page_size.height, static_cast<int>(std::ceil(box->y1 * k)) + margin)};
    limits::check_picture(part.width(), part.height(), "the letters");
    Image fine = Image::create("RGBA", Size{part.width(), part.height()}, Ink{0, 0, 0, 0});
    draw(fine, Point{part.x0, part.y0}, page_size, kTrace);
    const vec::Path path = traced(fine.getchannel(3), kTrace, part.x0, part.y0, 0.5);
    if (path.subpaths.empty()) throw OpError("the line " + line.id + " has no letters to trace");
    Json notes = Json::array();
    const Json st = text::style_of(line);
    if (line.balloon != "none" && line.balloon != "sfx") notes.push_back("the balloon is not a letter: left out");
    for (const char* key : {"outline_mm"}) {
        if (const Json* v = core::get(st, key); v != nullptr && core::py_truthy(*v)) {
            notes.push_back("the halo (白フチ) is left out");
            break;
        }
    }
    for (const char* key : {"gradient", "fill_png"}) {
        if (const Json* v = core::get(st, key); v != nullptr && core::py_truthy(*v)) {
            notes.push_back("the letters' fill is left out (only their shapes)");
            break;
        }
    }
    if (to == "shape") {  // (pen-shapes@1, PC3 C-b: the outline as a shape part, render/ops_shapes)
        const Json* rgb = core::get(st, "rgb");
        const Json colour = rgb != nullptr && core::py_truthy(*rgb) ? Json{{"rgb", *rgb}} : Json{{"rgb", Json::array({10, 10, 10})}};
        const Json index = page.index.json();
        Json terms = c.op;
        terms["page"] = index;
        if (given(terms, "layer_id") == nullptr) {
            terms["new_layer"] = Json{{"name", "シェイプ"}};
            if (!page.layers.empty()) terms["new_layer"]["after"] = page.layers.back().id;
        }
        terms.erase("id");
        OpContext sub{doc, terms, c.actor};
        Json report = place_shape(sub, *at, path, std::nullopt, "path", colour);
        report["page"] = index;
        report["subpaths"] = static_cast<std::int64_t>(path.subpaths.size());
        report["knots"] = static_cast<std::int64_t>(knots_of(path));
        report["notes"] = notes;
        c.report = std::move(report);
        return;
    }
    Page& edited = doc.edit_page(*at);
    edited.work_path = path;
    c.report = Json{{"page", page.index.json()},
                    {"subpaths", static_cast<std::int64_t>(path.subpaths.size())},
                    {"knots", static_cast<std::int64_t>(knots_of(path))},
                    {"notes", notes}};
}

// {page, layer_id, name? | path?, tool?: brush | pencil | eraser (default brush), kind?, width_mm?, rgb?, opacity?,
// simulate_pressure?} → {strokes}: the path stroked — each subpath (flattened within 0.01 mm, a closed one back to its
// start) a pen line add_stroke draws (a pen, paint or tone layer: its lines; the pencil: the 鉛筆 pen), or the eraser
// along it (erase); simulate_pressure: thin at its ends (the taper), else the whole width.
void stroke_path(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* layer_id = given(c.op, "layer_id");
    if (layer_id == nullptr) throw OpError("layer_id is required");
    vec::Path path;
    if (const Json* raw = given(c.op, "path")) {
        path = path_of(*raw, "path");
    } else {
        path = path_areas::named_path(doc.page(at), c.op.contains("name") ? c.op["name"] : Json());
    }
    const std::string tool = given(c.op, "tool") != nullptr ? core::py_str(*given(c.op, "tool")) : std::string("brush");
    if (tool != "brush" && tool != "pencil" && tool != "eraser") throw OpError("tool is brush, pencil or eraser");
    const bool pressure = given(c.op, "simulate_pressure") != nullptr && boolean(*given(c.op, "simulate_pressure"), "simulate_pressure");
    const core::OpFunction* draw = ops_registry().find(tool == "eraser" ? "erase" : "add_stroke");
    if (draw == nullptr) core::not_yet_ported("stroke_path needs the drawing ops (render::ops_registry)");
    std::int64_t strokes = 0;
    for (const path_raster::Polyline& line : path_raster::flatten_path(path, 0.01)) {
        if (line.pts.size() < 2) continue;
        Json points = Json::array();
        for (const auto& [x, y] : line.pts) points.push_back(Json::array({x, y}));
        if (line.closed) points.push_back(Json::array({line.pts.front().first, line.pts.front().second}));
        limits::check_count(static_cast<double>(points.size()), static_cast<double>(limits::kPoints), "path");
        Json one{{"op", tool == "eraser" ? "erase" : "add_stroke"}, {"page", c.op.at("page")}, {"layer_id", *layer_id}, {"points", points}};
        if (tool == "eraser") {
            if (const Json* w = given(c.op, "width_mm")) one["width_mm"] = *w;
        } else {
            one["stabilize"] = 0;
            one["post_smooth"] = 0;
            one["taper"] = pressure;
            if (const Json* kind = given(c.op, "kind")) one["kind"] = *kind;
            else if (tool == "pencil") one["kind"] = "pencil";
            for (const char* key : {"width_mm", "rgb", "opacity"}) {
                if (const Json* v = given(c.op, key)) one[key] = *v;
            }
        }
        OpContext sub{doc, one, c.actor};
        (*draw)(sub);
        ++strokes;
    }
    if (strokes == 0) throw OpError("the path has nothing to stroke");
    c.report = Json{{"strokes", strokes}};
}

// --- a layer's vector mask ------------------------------------------------------------------------------------------

Json mask_report(const Layer& layer) {
    if (!layer.vector_mask) return Json{{"vector_mask", nullptr}};
    return Json{{"vector_mask", vec::to_json(*layer.vector_mask)}};
}

// {page, id, path? | path_name? (the current path, a path) | reveal_all? | hide_all?, enabled?, linked?, density?
// 0..100, feather_mm?, invert?, delete?} → {vector_mask}: the layer's vector mask made or changed (a new path keeps the
// mask's settings), or deleted.
void set_vector_mask(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* id = given(c.op, "id");
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[core::layer_by_id(page, id != nullptr ? core::py_str(*id) : std::string())];
    if (layer.locked) throw OpError("the layer is locked");
    if (layer.kind == core::LayerKind::Folder) throw OpError("a folder cannot take a vector mask (mask the layers in it)");
    if (const Json* del = given(c.op, "delete"); del != nullptr && boolean(*del, "delete")) {
        if (!layer.vector_mask) throw OpError("the layer has no vector mask");
        layer.vector_mask.reset();
        c.report = mask_report(layer);
        return;
    }
    int sources = 0;
    std::optional<vec::Path> path;
    if (const Json* raw = given(c.op, "path")) {
        path = path_of(*raw, "path");
        ++sources;
    }
    if (c.op.contains("path_name")) {
        path = path_areas::named_path(page, c.op["path_name"]);
        ++sources;
    }
    for (const char* key : {"reveal_all", "hide_all"}) {
        if (const Json* v = given(c.op, key); v != nullptr && boolean(*v, key)) {
            path = vec::Path{};
            path->inverted = std::string_view(key) == "hide_all";
            ++sources;
        }
    }
    if (sources > 1) throw OpError("give one of path, path_name, reveal_all and hide_all");
    if (path) {
        vec::VectorMask mask = layer.vector_mask.value_or(vec::VectorMask{});
        mask.path = std::move(*path);
        layer.vector_mask = std::move(mask);
    } else if (!layer.vector_mask) {
        throw OpError("the layer has no vector mask (give path, path_name, reveal_all or hide_all to make one)");
    }
    vec::VectorMask& mask = *layer.vector_mask;
    if (const Json* v = given(c.op, "enabled")) mask.enabled = boolean(*v, "enabled");
    if (const Json* v = given(c.op, "linked")) mask.linked = boolean(*v, "linked");
    if (const Json* v = given(c.op, "density")) mask.density = number(*v, "density", 0.0, 100.0) / 100.0;
    if (const Json* v = given(c.op, "feather_mm")) mask.feather_mm = number(*v, "feather_mm", 0.0, vec::kFeatherMax);
    if (const Json* v = given(c.op, "invert"); v != nullptr && boolean(*v, "invert")) mask.path.inverted = !mask.path.inverted;
    c.report = mask_report(layer);
}

}  // namespace

void fill_area_blended(OpContext& c, std::size_t at, const std::string& layer_id, const Json& area, const std::string& mode,
                       const std::vector<std::int64_t>& rgb, double opacity) {
    Document& doc = c.doc;
    const Layer& layer = doc.page(at).layers[core::layer_by_id(doc.page(at), layer_id)];
    if (layer.kind != core::LayerKind::Raster) {
        throw OpError("blend paints a paint layer's pixels (a pen layer's fills are pictures laid on it: leave blend out)");
    }
    if (!image_ops::is_blend(mode)) throw OpError("blend is not one of the blends (normal, multiply, screen, …)");
    const bool keep_alpha = layer.lock_alpha;
    image_ops::Pixels p = image_ops::layer_pixels(c, at, layer_id);
    const std::vector<float> cover = image_ops::coverage(doc.page(at), area, p.width(), p.height(), p.kind() == image_ops::Pixels::Kind::Eight);
    const auto at_k = [&](std::size_t k) { return static_cast<double>(std::clamp<std::int64_t>(rgb.size() > k ? rgb[k] : 0, 0, 255)) / 255.0; };
    double colour[3] = {at_k(0), at_k(1), at_k(2)};
    if (p.kind() != image_ops::Pixels::Kind::Inks) RgbSpace::of(doc.page(at)).from_srgb(colour, 1);  // (the page's values)
    const image_ops::Rgba source{static_cast<float>(colour[0]), static_cast<float>(colour[1]), static_cast<float>(colour[2]), 1.0f};
    const auto paint = [&](image_ops::Rgba backdrop, float w) {
        image_ops::Rgba out = image_ops::composite(mode, backdrop, image_ops::Rgba{source[0], source[1], source[2], w}, static_cast<float>(opacity));
        if (keep_alpha) out[3] = backdrop[3];
        return out;
    };
    if (p.kind() == image_ops::Pixels::Kind::Inks) {
        std::vector<image_ops::Rgba> px = image_ops::rgba_of(p);
        for (std::size_t i = 0; i < px.size(); ++i)
            if (cover[i] > 0.0f) px[i] = paint(px[i], cover[i]);
        image_ops::from_rgba(p, px);
    } else {
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (cover[i] <= 0.0f) continue;
            float* v = p.at(i);
            const image_ops::Rgba out = paint(image_ops::Rgba{v[0], v[1], v[2], v[3]}, cover[i]);
            for (std::size_t k = 0; k < 4; ++k) v[k] = out[k];
        }
    }
    image_ops::put_layer_pixels(doc, at, layer_id, p);
}

void register_path_ops(core::OpRegistry& registry) {
    registry.add("set_path", set_path);
    registry.add("delete_path", delete_path);
    registry.add("rename_path", rename_path);
    registry.add("make_work_path", make_work_path);
    registry.add("text_to_path", text_to_path);
    registry.add("stroke_path", stroke_path);
    registry.add("set_vector_mask", set_vector_mask);
}

}  // namespace genko::render
