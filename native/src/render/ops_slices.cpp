// PC5 J-b (slices@1, render/ops_slices.hpp): a page's slices — PhotoCraft v0.2.0's crates/engine/src/slice_cmds.rs (MIT,
// native/third_party/photocraft/NOTICE.md), in mm.

#include "render/ops_slices.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/measure.hpp"
#include "core/production_json.hpp"
#include "core/pyops.hpp"
#include "core/pynum.hpp"
#include "render/layer_bounds.hpp"
#include "render/layer_styles.hpp"

namespace genko::render::slices {

using core::Json;
using core::OpError;
using core::Slice;
using core::Slices;

namespace {

constexpr int kDivideMost = 1000;

double r4(double v) { return core::py_round(v, 4); }

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v != nullptr && !v->is_null() ? v : nullptr;
}

const core::Layer* layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& l : page.layers)
        if (l.id == id) return &l;
    return nullptr;
}

std::string text_given(const Json& value, const char* key, std::size_t most) {
    if (!value.is_string()) throw OpError(std::string("the slice's ") + key + " is text");
    std::string out = value.get<std::string>();
    if (out.size() > most) throw OpError(std::string("the slice's ") + key + " is at most " + std::to_string(most) + " bytes");
    return out;
}

bool flag(const Json& value, const char* key) {
    if (!value.is_boolean()) throw OpError(std::string(key) + " is true or false");
    return value.get<bool>();
}

int whole(const Json& value, const char* key, int lo, int hi) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < lo || value.get<std::int64_t>() > hi)
        throw OpError(std::string(key) + " is a whole number " + std::to_string(lo) + ".." + std::to_string(hi));
    return static_cast<int>(value.get<std::int64_t>());
}

// [x, y, w, h]: rect_mm, or rect_px in the page's pixels (w and h above 0).
std::optional<std::array<double, 4>> rect_given(const core::Page& page, const Json& op) {
    const Json* mm = given(op, "rect_mm");
    const Json* px = given(op, "rect_px");
    if (mm != nullptr && px != nullptr) throw OpError("give rect_mm or rect_px, not both");
    const Json* v = mm != nullptr ? mm : px;
    if (v == nullptr) return std::nullopt;
    const char* key = mm != nullptr ? "rect_mm" : "rect_px";
    const auto bad = [&] { throw OpError(std::string(key) + " is [x, y, width, height] (" + (mm != nullptr ? "mm" : "the page's pixels") + "), width and height above 0"); };
    if (!v->is_array() || v->size() != 4) bad();
    std::array<double, 4> r{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Json& n = (*v)[i];
        if (!n.is_number() || n.is_boolean() || !std::isfinite(n.get<double>())) bad();
        r[i] = n.get<double>();
    }
    if (!(r[2] > 0) || !(r[3] > 0)) bad();
    if (px != nullptr) {
        const core::SliceGrid g = page_grid(page);
        r = {r[0] / g.px_x, r[1] / g.px_y, r[2] / g.px_x, r[3] / g.px_y};
    }
    for (const double n : r)
        if (std::fabs(n) > core::production::kCoordinateMaxMm) throw OpError(std::string(key) + " lies past 1000 m from the page");
    return std::array<double, 4>{r4(r[0]), r4(r[1]), r4(r[2]), r4(r[3])};
}

// The rect cut to the paper (none: nothing of it on it).
std::optional<std::array<double, 4>> on_paper(const core::Page& page, const std::array<double, 4>& r) {
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    const double x0 = std::max(0.0, r[0]), y0 = std::max(0.0, r[1]);
    const double x1 = std::min(w, r[0] + r[2]), y1 = std::min(h, r[1] + r[3]);
    if (!(x1 > x0) || !(y1 > y0)) return std::nullopt;
    const std::array<double, 4> out{r4(x0), r4(y0), r4(x1 - x0), r4(y1 - y0)};
    if (core::slice_box(out, page_grid(page)).empty()) return std::nullopt;  // (less than a pixel of the page)
    return out;
}

std::string colour_given(const Json& value) {
    if (value.is_null()) return {};
    if (!value.is_string()) throw OpError("background is \"#rrggbb\", \"none\" or null");
    std::string s = value.get<std::string>();
    if (s == "none" || s.empty()) return {};
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool ok = s.size() == 7 && s[0] == '#' && std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    if (!ok) throw OpError("background is \"#rrggbb\", \"none\" or null");
    return s;
}

// スライスオプション: the options given put on the slice.
void put_options(Slice& s, const Json& op) {
    if (const Json* v = given(op, "name")) s.name = text_given(*v, "name", 1024);
    if (const Json* v = given(op, "kind")) {
        const std::string kind = text_given(*v, "kind", 16);
        if (kind != "image" && kind != "noImage" && kind != "table") throw OpError("kind is image, noImage or table");
        s.kind = kind;
    }
    if (const Json* v = given(op, "url")) s.url = text_given(*v, "url", core::kSliceTextMax);
    if (const Json* v = given(op, "target")) s.target = text_given(*v, "target", core::kSliceTextMax);
    if (const Json* v = given(op, "message")) s.message = text_given(*v, "message", core::kSliceTextMax);
    if (const Json* v = given(op, "alt")) s.alt = text_given(*v, "alt", core::kSliceTextMax);
    if (const Json* v = given(op, "cell_text")) s.cell_text = text_given(*v, "cell_text", core::kSliceTextMax);
    if (const Json* v = given(op, "cell_text_is_html")) s.cell_text_is_html = flag(*v, "cell_text_is_html");
    if (const Json* v = given(op, "align")) {
        if (!v->is_array() || v->size() != 2) throw OpError("align is [horizontal, vertical], each 0..4");
        for (std::size_t k = 0; k < 2; ++k) s.align[k] = whole((*v)[k], "align", 0, 4);
    }
    if (const Json* v = core::get(op, "background")) {
        const std::string c = colour_given(*v);
        s.background = c.empty() ? std::nullopt : std::optional<std::string>(c);
    }
    if (const Json* v = given(op, "outsets")) {
        if (!v->is_array() || v->size() != 4) throw OpError("outsets is [top, left, bottom, right] (the page's pixels)");
        for (std::size_t k = 0; k < 4; ++k) s.outsets[k] = whole((*v)[k], "outsets", -core::kSliceOutsetMax, core::kSliceOutsetMax);
    }
}

Slices& slices_of(core::Document& doc, std::size_t at) {
    core::Page& page = doc.edit_page(at);
    if (!page.slices) page.slices = Slices{};
    return *page.slices;
}

// The page's slices checked, none kept when nothing is left of them (nothing written: no slices@1).
void settle(core::Document& doc, std::size_t at) {
    core::Page& page = doc.edit_page(at);
    if (!page.slices) return;
    core::check_slices(*page.slices);
    if (page.slices->empty()) page.slices.reset();
}

const Slices& seen(const core::Page& page) {
    static const Slices none;
    return page.slices ? *page.slices : none;
}

// The slice an op names: its id (`slice`) or its number as shown (`number`); an auto slice's number made a user slice
// (`promote`) — its id then.
std::int64_t named(core::Document& doc, std::size_t at, const Json& op, bool promote) {
    const core::Page& page = doc.page(at);
    if (const Json* id = given(op, "slice")) {
        if (!id->is_number_integer()) throw OpError("slice is a slice's id");
        if (core::find_slice(seen(page), id->get<std::int64_t>()) == nullptr) throw OpError("no slice " + std::to_string(id->get<std::int64_t>()) + " on the page");
        return id->get<std::int64_t>();
    }
    const Json* number = given(op, "number");
    if (number == nullptr) throw OpError("give slice (its id) or number (as shown)");
    if (!number->is_number_integer() || number->get<std::int64_t>() < 1) throw OpError("number is a slice's number as shown (1 the first)");
    const core::SliceGrid grid = page_grid(page);
    const std::vector<core::ResolvedSlice> all = resolve(doc, page, grid);
    const auto it = std::find_if(all.begin(), all.end(), [&](const core::ResolvedSlice& r) { return r.number == number->get<std::int64_t>(); });
    if (it == all.end()) throw OpError("no slice number " + std::to_string(number->get<std::int64_t>()) + " on the page");
    if (it->id) return *it->id;
    if (!promote) throw OpError("slice number " + std::to_string(it->number) + " is an auto slice");
    Slices& list = slices_of(doc, at);
    if (list.list.size() >= core::kSlicesMax) throw OpError("a page holds at most " + std::to_string(core::kSlicesMax) + " slices");
    Slice s;
    s.id = core::fresh_slice_id(list);
    s.rect_mm = core::slice_rect_mm(it->box, grid);
    list.list.push_back(s);
    list.next_id = s.id + 1;
    return s.id;
}

Slice& stored(core::Document& doc, std::size_t at, std::int64_t id) {
    for (Slice& s : slices_of(doc, at).list)
        if (s.id == id) return s;
    throw OpError("no slice " + std::to_string(id) + " on the page");
}

std::optional<core::ResolvedSlice> resolved_of(const core::Document& doc, const core::Page& page, std::int64_t id, const core::SliceGrid& grid) {
    for (const core::ResolvedSlice& r : resolve(doc, page, grid))
        if (r.id == id) return r;
    return std::nullopt;
}

void add_slice(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    const Slices& before = seen(page);
    if (before.list.size() >= core::kSlicesMax) throw OpError("a page holds at most " + std::to_string(core::kSlicesMax) + " slices");
    Slice s;
    s.id = core::fresh_slice_id(before);
    const std::optional<std::array<double, 4>> rect = rect_given(page, c.op);
    if (const Json* layer = given(c.op, "layer")) {  // (新規レイヤーベースのスライス)
        if (rect) throw OpError("a layer-based slice follows its layer: give layer or a rect, not both");
        if (!layer->is_string()) throw OpError("layer is a layer's id");
        const std::string id = layer->get<std::string>();
        const core::Layer* l = layer_of(page, id);
        if (l == nullptr) throw OpError("no layer " + id + " on the page");
        if (l->kind == core::LayerKind::Fill || l->kind == core::LayerKind::Adjust) throw OpError("a fill or adjustment layer has no bounds to slice");
        if (std::any_of(before.list.begin(), before.list.end(), [&](const Slice& x) { return x.layer == id; }))
            throw OpError("the layer has a layer-based slice already");
        s.origin = "layer";
        s.layer = id;
        put_options(s, c.op);
        const auto now = layer_rect_mm(c.doc, page, s);
        if (!now) throw OpError("the layer is empty: no layer-based slice can be made of it");
        s.rect_mm = *now;
    } else {
        if (!rect) throw OpError("give rect_mm or rect_px (or layer, for a layer-based slice)");
        const auto cut = on_paper(page, *rect);
        if (!cut) throw OpError("the slice lies outside the page");
        put_options(s, c.op);
        if (s.outsets != std::array<int, 4>{}) throw OpError("outsets are a layer-based slice's");
        s.rect_mm = *cut;
    }
    Slices& list = slices_of(c.doc, at);
    list.list.push_back(s);
    list.next_id = s.id + 1;
    settle(c.doc, at);
    const core::SliceGrid grid = page_grid(c.doc.page(at));
    const auto r = resolved_of(c.doc, c.doc.page(at), s.id, grid);
    c.report = Json{{"slice", s.id}, {"number", r ? Json(r->number) : Json(nullptr)}, {"rect_mm", core::production::to_json(s.rect_mm)}};
}

void set_slice(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const std::int64_t id = named(c.doc, at, c.op, true);
    const core::Page& page = c.doc.page(at);
    const std::optional<std::array<double, 4>> rect = rect_given(page, c.op);
    bool promote = false;
    if (const Json* p = given(c.op, "promote")) promote = flag(*p, "promote");
    // (a layer-based slice made a user one where its layer puts it now — its rect after, if one is given)
    std::optional<std::array<double, 4>> was_layers;
    if (const Slice* now = core::find_slice(seen(page), id); now != nullptr && now->origin == "layer" && (rect || promote)) {
        was_layers = layer_rect_mm(c.doc, page, *now);
    }
    std::optional<std::array<double, 4>> cut;
    if (rect) {
        cut = on_paper(page, *rect);
        if (!cut) throw OpError("the slice lies outside the page");
    }
    Slice& s = stored(c.doc, at, id);
    put_options(s, c.op);
    if (s.origin == "user" && s.outsets != std::array<int, 4>{} && (given(c.op, "outsets") != nullptr)) throw OpError("outsets are a layer-based slice's");
    if (s.origin == "layer" && (rect || promote)) {
        s.origin = "user";
        s.layer.reset();
        s.outsets = {};
        if (was_layers) s.rect_mm = *was_layers;
    }
    if (cut) s.rect_mm = *cut;
    settle(c.doc, at);
    const core::Page& after = c.doc.page(at);
    const core::SliceGrid grid = page_grid(after);
    const auto r = resolved_of(c.doc, after, id, grid);
    if (!r) {
        c.report = Json{{"slice", id}, {"number", nullptr}};
        return;
    }
    c.report = slice_json(after, *r, grid);
}

void delete_slices(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* which = given(c.op, "slices");
    if (which == nullptr) throw OpError("slices is a list of the slices' ids, or \"all\"");
    const Slices& now = seen(c.doc.page(at));
    if (now.list.empty()) throw OpError("the page has no user or layer-based slices");
    std::set<std::int64_t> gone;
    if (which->is_string() && which->get<std::string>() == "all") {
        for (const Slice& s : now.list) gone.insert(s.id);
    } else if (which->is_array() && !which->empty()) {
        for (const Json& id : *which) {
            if (!id.is_number_integer()) throw OpError("slices is a list of the slices' ids, or \"all\"");
            if (core::find_slice(now, id.get<std::int64_t>()) == nullptr) throw OpError("no slice " + std::to_string(id.get<std::int64_t>()) + " on the page");
            gone.insert(id.get<std::int64_t>());
        }
    } else {
        throw OpError("slices is a list of the slices' ids, or \"all\"");
    }
    Slices& list = slices_of(c.doc, at);
    std::erase_if(list.list, [&](const Slice& s) { return gone.contains(s.id); });
    settle(c.doc, at);
    c.report = Json{{"deleted", gone.size()}};
}

void divide_slice(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    int rows = 1, columns = 1;
    if (const Json* v = given(c.op, "rows")) rows = whole(*v, "rows", 1, kDivideMost);
    if (const Json* v = given(c.op, "columns")) columns = whole(*v, "columns", 1, kDivideMost);
    if (rows == 1 && columns == 1) throw OpError("give rows (slices down) or columns (slices across) above 1");
    const std::int64_t id = named(c.doc, at, c.op, true);
    const core::Page& page = c.doc.page(at);
    const Slice source = *core::find_slice(seen(page), id);
    std::array<double, 4> rect = source.rect_mm;
    if (source.origin == "layer") {
        const auto now = layer_rect_mm(c.doc, page, source);
        if (!now) throw OpError("the slice's layer shows nothing to divide");
        rect = *now;
    }
    const core::SliceBox box = core::slice_box(rect, page_grid(page));
    if (box.width() < columns || box.height() < rows) throw OpError("the slice is too small to divide that many times");
    if (seen(page).list.size() - 1 + static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns) > core::kSlicesMax)
        throw OpError("a page holds at most " + std::to_string(core::kSlicesMax) + " slices");
    Slices& list = slices_of(c.doc, at);
    const auto pos = std::find_if(list.list.begin(), list.list.end(), [&](const Slice& s) { return s.id == id; }) - list.list.begin();
    list.list.erase(list.list.begin() + pos);
    std::vector<Slice> parts;
    Json ids = Json::array();
    std::int64_t next = core::fresh_slice_id(list);
    if (next <= id) next = id + 1;
    for (const std::array<double, 4>& part : core::divide_rect(rect, rows, columns)) {
        Slice s;
        if (parts.empty()) {  // (the first keeps the slice's options, a user slice now)
            s = source;
            s.origin = "user";
            s.layer.reset();
            s.outsets = {};
        } else {
            s.id = next++;
        }
        s.rect_mm = part;
        ids.push_back(s.id);
        parts.push_back(std::move(s));
    }
    list.list.insert(list.list.begin() + pos, parts.begin(), parts.end());
    list.next_id = std::max(list.next_id, next);
    settle(c.doc, at);
    c.report = Json{{"slices", ids}, {"count", ids.size()}};
}

void slices_from_guides(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    std::vector<double> xs, ys;
    if (page.rulers.is_array()) {
        for (const Json& r : page.rulers) {  // (the guides: rulers of kind "guide", across ("h") or upright ("v"))
            if (!r.is_object()) continue;
            const auto kind = r.find("kind"), where = r.find("at"), axis = r.find("axis");
            if (kind == r.end() || *kind != Json("guide") || where == r.end() || !where->is_number()) continue;
            (axis != r.end() && *axis == Json("h") ? ys : xs).push_back(where->get<double>());
        }
    }
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    std::vector<std::array<double, 4>> cells = core::guide_cells(w, h, xs, ys);
    const core::SliceGrid grid = page_grid(page);
    std::erase_if(cells, [&](const std::array<double, 4>& r) { return core::slice_box(r, grid).empty(); });
    if (cells.empty()) throw OpError("the page has no guides inside its paper");
    if (cells.size() > core::kSlicesMax) throw OpError("a page holds at most " + std::to_string(core::kSlicesMax) + " slices");
    Slices& list = slices_of(c.doc, at);
    std::int64_t next = core::fresh_slice_id(list);
    list.list.clear();  // (Photoshop's: every slice replaced)
    for (const std::array<double, 4>& r : cells) {
        Slice s;
        s.id = next++;
        s.rect_mm = r;
        list.list.push_back(std::move(s));
    }
    list.next_id = next;
    settle(c.doc, at);
    c.report = Json{{"slices", cells.size()}};
}

void slices_info(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    c.report = listing(c.doc, c.doc.page(at));
}

}  // namespace

core::SliceGrid page_grid(const core::Page& page) {
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    const double k = core::page_px_per_mm(page);
    return core::slice_grid(w, h, static_cast<std::int64_t>(std::llround(std::max(0.0, w) * k)), static_cast<std::int64_t>(std::llround(std::max(0.0, h) * k)));
}

std::string base_name(const core::Page& page) {
    std::string base;
    if (page.slices && !page.slices->group.empty()) {
        base = page.slices->group;
        if (const auto dot = base.rfind('.'); dot != std::string::npos && dot > 0) base.resize(dot);
    } else {
        try {
            base = core::file_stem(page);
        } catch (const std::exception&) {
            base.clear();
        }
    }
    return core::slice_file_name(base, "Untitled");
}

std::optional<std::array<double, 4>> layer_rect_mm(const core::Document& doc, const core::Page& page, const core::Slice& s) {
    if (!s.layer) return std::nullopt;
    const core::Layer* layer = layer_of(page, *s.layer);
    if (layer == nullptr) return std::nullopt;
    std::optional<layer_bounds::Box> b;
    try {
        b = layer_bounds::of_layer(doc, page, *s.layer);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!b) return std::nullopt;
    // (its style's effects reach past what it shows: PhotoCraft's layer_bounds inflated by the effects' margin)
    if (layer->style && layer->style->is_object()) {
        const double grow = static_cast<double>(layer_styles::margin_px(*layer->style, layer_bounds::kDpi)) / layer_bounds::kPx;
        *b = {(*b)[0] - grow, (*b)[1] - grow, (*b)[2] + grow, (*b)[3] + grow};
    }
    const double k = core::page_px_per_mm(page);
    const double top = s.outsets[0] / k, left = s.outsets[1] / k, bottom = s.outsets[2] / k, right = s.outsets[3] / k;
    const double x0 = (*b)[0] - left, y0 = (*b)[1] - top, x1 = (*b)[2] + right, y1 = (*b)[3] + bottom;
    if (!(x1 > x0) || !(y1 > y0)) return std::nullopt;
    return std::array<double, 4>{r4(x0), r4(y0), r4(x1 - x0), r4(y1 - y0)};
}

std::vector<std::pair<std::int64_t, std::array<double, 4>>> rects_now(const core::Document& doc, const core::Page& page) {
    std::vector<std::pair<std::int64_t, std::array<double, 4>>> out;
    if (!page.slices) return out;
    for (const Slice& s : page.slices->list) {
        if (s.origin == "layer") {
            if (const auto r = layer_rect_mm(doc, page, s)) out.emplace_back(s.id, *r);
            continue;
        }
        out.emplace_back(s.id, s.rect_mm);
    }
    return out;
}

std::vector<core::ResolvedSlice> resolve(const core::Document& doc, const core::Page& page, const core::SliceGrid& grid,
                                         const std::vector<std::pair<std::int64_t, std::array<double, 4>>>* rects) {
    std::vector<std::pair<std::int64_t, std::array<double, 4>>> made;
    if (rects == nullptr) {
        made = rects_now(doc, page);
        rects = &made;
    }
    std::vector<std::pair<std::int64_t, core::SliceBox>> boxes;
    boxes.reserve(rects->size());
    for (const auto& [id, r] : *rects) boxes.emplace_back(id, core::slice_box(r, grid));
    return core::resolve_slices(seen(page), boxes, grid.canvas, base_name(page));
}

Json slice_json(const core::Page& page, const core::ResolvedSlice& r, const core::SliceGrid& grid) {
    const Slice* s = r.id ? core::find_slice(seen(page), *r.id) : nullptr;
    static const Slice none;
    const Slice& o = s != nullptr ? *s : none;
    return Json{{"number", r.number},
                {"id", r.id ? Json(*r.id) : Json(nullptr)},
                {"origin", r.origin},
                {"name", r.name},
                {"kind", r.kind},
                {"rect_px", Json::array({r.box.x0, r.box.y0, r.box.width(), r.box.height()})},
                {"rect_mm", core::production::to_json(core::slice_rect_mm(r.box, grid))},
                {"layer", o.layer ? Json(*o.layer) : Json(nullptr)},
                {"url", o.url},
                {"target", o.target},
                {"message", o.message},
                {"alt", o.alt},
                {"cell_text", o.cell_text},
                {"cell_text_is_html", o.cell_text_is_html},
                {"align", Json::array({o.align[0], o.align[1]})},
                {"background", o.background ? Json(*o.background) : Json(nullptr)},
                {"outsets", Json::array({o.outsets[0], o.outsets[1], o.outsets[2], o.outsets[3]})}};
}

Json listing(const core::Document& doc, const core::Page& page) {
    const core::SliceGrid grid = page_grid(page);
    Json list = Json::array();
    for (const core::ResolvedSlice& r : resolve(doc, page, grid)) list.push_back(slice_json(page, r, grid));
    return Json{{"page", page.index.json()},
                {"dpi", r4(core::page_px_per_mm(page) * 25.4)},
                {"width_px", grid.canvas.width()},
                {"height_px", grid.canvas.height()},
                {"group", page.slices ? page.slices->group : std::string()},
                {"base", base_name(page)},
                {"slices", std::move(list)}};
}

void follow_layers(core::Document& doc, const std::vector<core::PagePtr>& before) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p)) continue;
        const core::Page& page = doc.page(p);
        if (!page.slices || page.slices->list.empty()) continue;
        if (std::any_of(before.begin(), before.end(), [&](const core::PagePtr& b) { return b.get() == &page; })) continue;  // (not changed)
        const bool gone = std::any_of(page.slices->list.begin(), page.slices->list.end(),
                                      [&](const Slice& s) { return s.layer && layer_of(page, *s.layer) == nullptr; });
        if (!gone) continue;
        core::Page& edit = doc.edit_page(p);
        std::erase_if(edit.slices->list, [&](const Slice& s) { return s.layer && layer_of(edit, *s.layer) == nullptr; });
        if (edit.slices->empty()) edit.slices.reset();
    }
}

}  // namespace genko::render::slices

namespace genko::render {

void register_slice_ops(core::OpRegistry& registry) {
    registry.add("add_slice", slices::add_slice);
    registry.add("set_slice", slices::set_slice);
    registry.add("delete_slices", slices::delete_slices);
    registry.add("divide_slice", slices::divide_slice);
    registry.add("slices_from_guides", slices::slices_from_guides);
    registry.add_query("slices_info", slices::slices_info);
}

}  // namespace genko::render
