#include "core/smart.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/page_geometry.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/vector.hpp"

namespace genko::core::smart {

namespace {

constexpr std::array<std::string_view, 10> kSettingKeys{"contents", "matrix", "perspective", "warp", "interp", "stack",
                                                        "filters", "filters_on", "filter_mask", "psd"};
constexpr std::array<std::string_view, 13> kFileKeys{"kind", "name", "asset", "suffix", "format", "size_px", "dpi",
                                                     "depth", "mode", "profile", "link", "psd", "spec"};
constexpr std::array<std::string_view, 4> kModes{"gray", "rgb", "cmyk", "lab"};
constexpr std::array<std::string_view, 3> kUses{"keep", "embedded", "assign"};
constexpr std::array<std::string_view, 4> kIntents{"perceptual", "relative", "saturation", "absolute"};

template <std::size_t N>
bool in(const std::array<std::string_view, N>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool is_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool is_ref(std::string_view text) { return text.size() == 71 && text.starts_with("sha256:") && is_hex(text.substr(7)); }

[[noreturn]] void refuse(const std::string& message) { throw Error("value", message); }

double finite(const Json& value, const std::string& what) {
    if (!value.is_number() || value.is_boolean()) refuse(what + " must be a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) refuse(what + " must be a finite number");
    return v;
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool same_frame(const Frame& a, const Frame& b);

bool same_frames(const std::vector<Frame>& a, const std::vector<Frame>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_frame(a[i], b[i])) return false;
    }
    return true;
}

bool same_points(const std::optional<std::vector<Point>>& a, const std::optional<std::vector<Point>>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->size() != b->size()) return false;
    for (std::size_t i = 0; i < a->size(); ++i) {
        if (!((*a)[i].x == (*b)[i].x) || !((*a)[i].y == (*b)[i].y)) return false;
    }
    return true;
}

// The same panel as the drawing takes it (its box, shape, clipping and border, and the panels in it).
bool same_frame(const Frame& a, const Frame& b) {
    return a.id == b.id && a.rect.x == b.rect.x && a.rect.y == b.rect.y && a.rect.width == b.rect.width &&
           a.rect.height == b.rect.height && a.split_axis == b.split_axis && a.clip == b.clip && a.bleed == b.bleed &&
           a.border_mm == b.border_mm && same_points(a.poly, b.poly) && a.curves == b.curves && a.corner_mm == b.corner_mm &&
           a.line == b.line && a.panel == b.panel && same_frames(a.children, b.children);
}

// The pages a book draws: its own (those read) and its contents' pages.
template <class Each>
void each_page(const Document& doc, Each each) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i)) each(*doc.pages[i], std::string());
    }
    for (const auto& [id, contents] : doc.smart_contents) {
        if (contents && contents->page) each(*contents->page, id);
    }
}

// A point [x, y] of finite numbers.
std::pair<double, double> point_at(const Json& point, const std::string& what) {
    if (!point.is_array() || point.size() != 2) refuse(what + " is [x, y]");
    return {finite(point[0], what), finite(point[1], what)};
}

// Only these keys.
void known_keys(const std::string& who, const Json& object, std::initializer_list<std::string_view> keys) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) refuse(who + ": the key " + py_repr_str(key) + " is not known to this build");
    }
}

// The settings of a smart filter that bends the picture (PC3 A5), as far as core reads them: their keys and the shape of
// their lists (render/smart_filters reads them through 節 C's own readers when it draws them).
void check_bend(const std::string& who, std::string_view kind, const Json& params) {
    const std::string what = who + "'s smart filter " + std::string(kind);
    if (kind == "liquify") {
        known_keys(what, params, {"strokes", "mesh_mm"});
        const Json* strokes = get(params, "strokes");
        if (strokes == nullptr || !strokes->is_array() || strokes->size() > 10000) refuse(what + ": strokes is a list of at most 10000");
        std::size_t points = 0;
        for (const Json& s : *strokes) {
            if (!s.is_object()) refuse(what + ": a stroke is {tool, size_mm, density, pressure, rate, points, amount?}");
            known_keys(what, s, {"tool", "size_mm", "density", "pressure", "rate", "points", "amount"});
            if (const Json* tool = get(s, "tool"); tool != nullptr && !tool->is_string()) refuse(what + ": a stroke's tool is its name");
            for (const char* key : {"size_mm", "density", "pressure", "rate", "amount"}) {
                if (const Json* v = get(s, key); v != nullptr && !v->is_null()) (void)finite(*v, what + "'s " + key);
            }
            if (const Json* p = get(s, "points"); p != nullptr && !p->is_null()) {
                if (!p->is_array()) refuse(what + ": a stroke's points are [[x, y, pressure?]…]");
                for (const Json& q : *p) {
                    if (!q.is_array() || q.size() < 2 || q.size() > 3) refuse(what + ": a stroke's points are [[x, y, pressure?]…]");
                    for (const Json& v : q) (void)finite(v, what + "'s point");
                }
                points += p->size();
            }
        }
        if (points > 1'000'000) refuse(what + ": at most a million points");
        if (const Json* cell = get(params, "mesh_mm"); cell != nullptr && !cell->is_null() && !(finite(*cell, what + "'s mesh_mm") > 0))
            refuse(what + ": mesh_mm is above 0");
        return;
    }
    const auto interp = [&] {
        const Json* v = get(params, "interp");
        if (v != nullptr && !v->is_null() && (!v->is_string() || (*v != "nearest" && *v != "bilinear" && *v != "bicubic")))
            refuse(what + ": interp is nearest, bilinear or bicubic");
    };
    if (kind == "perspective_warp") {
        known_keys(what, params, {"planes", "interp"});
        interp();
        const Json* planes = get(params, "planes");
        if (planes == nullptr || !planes->is_array() || planes->empty() || planes->size() > 64) refuse(what + ": planes is a list of 1 to 64");
        for (const Json& plane : *planes) {
            if (!plane.is_object()) refuse(what + ": a plane is {src, dst}");
            known_keys(what, plane, {"src", "dst"});
            for (const char* key : {"src", "dst"}) {
                const Json* quad = get(plane, key);
                if (quad == nullptr || !quad->is_array() || quad->size() != 4) refuse(what + ": a plane's " + key + " is four corners");
                for (const Json& p : *quad) (void)point_at(p, what + "'s corner");
            }
        }
        return;
    }
    known_keys(what, params, {"pins", "mode", "density", "expansion_mm", "interp"});
    interp();
    if (const Json* pins = get(params, "pins"); pins != nullptr && !pins->is_null()) {
        if (!pins->is_array() || pins->size() > 1000) refuse(what + ": pins is a list of at most 1000");
        for (const Json& pin : *pins) {
            if (!pin.is_object() || get(pin, "src") == nullptr || get(pin, "dst") == nullptr) refuse(what + ": a pin is {src, dst, rotate?, depth?}");
            known_keys(what, pin, {"src", "dst", "rotate", "depth"});
            (void)point_at(pin["src"], what + "'s pin");
            (void)point_at(pin["dst"], what + "'s pin");
            if (const Json* r = get(pin, "rotate"); r != nullptr && !r->is_null()) (void)finite(*r, what + "'s pin rotate");
            if (const Json* d = get(pin, "depth"); d != nullptr && !d->is_null() && !d->is_number_integer()) refuse(what + ": a pin's depth is a whole number");
        }
    }
    for (const char* key : {"mode", "density"}) {
        if (const Json* v = get(params, key); v != nullptr && !v->is_null() && !v->is_string()) refuse(what + ": " + key + " is its name");
    }
    if (const Json* e = get(params, "expansion_mm"); e != nullptr && !e->is_null()) (void)finite(*e, what + "'s expansion_mm");
}

// One smart filter as stored (PC3 A2): its keys, a kind a smart object can hold, its settings read as that kind reads them
// where core reads them (the C++ build's own kinds and natives, Python's colour adjustments; the filters that move
// shapes read theirs as they are drawn; PC3 A5's that bend the picture, their shape), its blend, opacity and visibility.
void check_filter(const std::string& who, const Json& f) {
    if (!f.is_object()) refuse(who + ": a smart filter is an object {kind, params, blend, opacity, visible}");
    for (const auto& [key, value] : f.items()) {
        if (!in(kFilterKeys, key)) refuse(who + ": the smart filter key " + py_repr_str(key) + " is not known to this build");
    }
    const Json* kind = get(f, "kind");
    if (kind == nullptr || !kind->is_string()) refuse(who + ": a smart filter names its kind");
    const std::string name = kind->get<std::string>();
    if (!filter_kind(name)) refuse(who + ": the filter " + py_repr_str(name) + " cannot be a smart filter");
    const Json* params = get(f, "params");
    if (params != nullptr && !params->is_object()) refuse(who + ": a smart filter's params are an object");
    const Json settings = params != nullptr ? *params : Json::object();
    if (const Json* blend = get(f, "blend"); blend != nullptr) {
        if (!blend->is_string()) refuse(who + ": a smart filter's blend is a blend mode");
        try {
            (void)blend_mode(*blend);
        } catch (const std::exception&) {
            refuse(who + ": a smart filter's blend " + py_repr_str(blend->get<std::string>()) + " is not a blend mode");
        }
    }
    if (const Json* opacity = get(f, "opacity"); opacity != nullptr) {
        const double v = finite(*opacity, who + "'s smart filter opacity");
        if (v < 0 || v > 1) refuse(who + ": a smart filter's opacity is 0 to 1");
    }
    if (const Json* visible = get(f, "visible"); visible != nullptr && !visible->is_boolean()) refuse(who + ": a smart filter's visible is true or false");
    if (bend_filter(name)) {
        check_bend(who, name, settings);
        return;
    }
    try {
        if (native_filter(name, settings)) {
            (void)normalize_filter(name, settings, nullptr);
        } else if (name != "exposure" && std::find(kAdjustments.begin(), kAdjustments.end(), name) != kAdjustments.end()) {
            (void)adjustment(name, settings);
        }
    } catch (const std::exception& error) {
        refuse(who + ": the smart filter " + name + "'s settings cannot be read (" + error.what() + ")");
    }
    // (a lookup table of a file is a correction layer's: a smart filter's colour lookup is one of the looks)
    if (name == "color_lookup" && (settings.contains("lut") || settings.contains("data")))
        refuse(who + ": a smart filter's colour lookup is one of the looks (look), not a table");
}

// A PSD's placed layer as a smart object keeps (PC3 A3): {"block": its key, "raw": its payload (an asset), "shown"?:
// Photoshop's picture of it ({"asset": a PNG} or {"ink_raster": a page of inks' GKIR})}.
void check_layer_psd(const std::string& who, const Json& psd) {
    if (!psd.is_object()) refuse(who + ": a smart object's psd is {block, raw, shown}");
    for (const auto& [key, value] : psd.items()) {
        if (key != "block" && key != "raw" && key != "shown") refuse(who + ": the psd key " + py_repr_str(key) + " is not known to this build");
    }
    const Json* block = get(psd, "block");
    if (block == nullptr || !block->is_string() || (*block != "SoLd" && *block != "SoLE" && *block != "PlLd"))
        refuse(who + ": a smart object's psd block is SoLd, SoLE or PlLd");
    const Json* raw = get(psd, "raw");
    if (raw == nullptr || !raw->is_string() || !is_ref(raw->get_ref<const std::string&>())) refuse(who + ": a smart object's psd raw is an asset (sha256:…)");
    if (const Json* shown = get(psd, "shown"); shown != nullptr && !shown->is_null()) {
        if (!shown->is_object() || shown->size() != 1) refuse(who + ": a smart object's psd shown is {asset} or {ink_raster}");
        const auto only = shown->begin();
        if ((only.key() != "asset" && only.key() != "ink_raster") || !only->is_string() || !is_ref(only->get_ref<const std::string&>()))
            refuse(who + ": a smart object's psd shown is {asset} or {ink_raster} (sha256:…)");
    }
}

void check_settings(const Layer& layer) {
    const std::string who = "layer " + layer.id;
    if (!layer.smart || !layer.smart->is_object()) refuse(who + " is a smart object without its settings");
    const Json& s = *layer.smart;
    for (const auto& [key, value] : s.items()) {
        if (!in(kSettingKeys, key)) refuse(who + ": the smart object setting " + py_repr_str(key) + " is not known to this build");
    }
    const Json* contents = get(s, "contents");
    if (contents == nullptr || !contents->is_string() || !is_contents_id(contents->get_ref<const std::string&>()))
        refuse(who + ": a smart object names its contents (sc_ and 12 hex digits)");
    if (const Json* m = get(s, "matrix"); m != nullptr) {
        const Matrix matrix = matrix_from(*m);
        if (std::abs(matrix.det()) <= kDetMin) refuse(who + ": the smart object's matrix squashes it flat");
    }
    if (const Json* corners = get(s, "perspective"); corners != nullptr && !corners->is_null()) {
        if (!corners->is_array() || corners->size() != 4) refuse(who + ": a smart object's perspective is its four corners [x, y]");
        Corners p;
        for (std::size_t i = 0; i < 4; ++i) {
            const Json& point = (*corners)[i];
            if (!point.is_array() || point.size() != 2) refuse(who + ": a smart object's perspective is its four corners [x, y]");
            p[i] = {finite(point[0], who + "'s corner"), finite(point[1], who + "'s corner")};
        }
        if (!convex(p)) refuse(who + ": a smart object's corners must make a convex shape");
    }
    if (const Json* interp = get(s, "interp"); interp != nullptr) {
        if (!interp->is_string() || !in(kInterps, interp->get_ref<const std::string&>()))
            refuse(who + ": interp is lanczos, bicubic, bilinear or nearest");
    }
    if (const Json* stack = get(s, "stack"); stack != nullptr && !stack->is_null()) {
        if (!stack->is_string() || !in(kStackModes, stack->get_ref<const std::string&>())) refuse(who + ": an unknown stack mode");
    }
    if (const Json* filters = get(s, "filters"); filters != nullptr && !filters->is_null()) {
        if (!filters->is_array()) refuse(who + ": a smart object's filters are a list");
        if (filters->size() > kFiltersMax) refuse(who + ": a smart object holds at most " + std::to_string(kFiltersMax) + " smart filters");
        for (const Json& f : *filters) check_filter(who, f);
    }
    if (const Json* on = get(s, "filters_on"); on != nullptr && !on->is_boolean()) refuse(who + ": filters_on is true or false");
    if (const Json* mask = get(s, "filter_mask"); mask != nullptr && !mask->is_null()) {
        if (!mask->is_object()) refuse(who + ": a smart object's filter_mask is {asset, enabled}");
        for (const auto& [key, value] : mask->items()) {
            if (key != "asset" && key != "enabled") refuse(who + ": the filter mask key " + py_repr_str(key) + " is not known to this build");
        }
        const Json* asset = get(*mask, "asset");
        if (asset == nullptr || !asset->is_string() || !is_ref(asset->get_ref<const std::string&>())) refuse(who + ": a filter mask is an asset (sha256:…)");
        if (const Json* enabled = get(*mask, "enabled"); enabled != nullptr && !enabled->is_boolean()) refuse(who + ": a filter mask's enabled is true or false");
    }
    if (const Json* warp = get(s, "warp"); warp != nullptr && !warp->is_null()) {
        try {
            check_warp(*warp);
        } catch (const Error& error) {
            refuse(who + ": " + error.what());
        }
    }
    if (const Json* psd = get(s, "psd"); psd != nullptr && !psd->is_null()) check_layer_psd(who, *psd);
    const bool drawn = layer.raster_png || layer.color_raster || layer.ink_raster || layer.stroke_count() > 0 || !layer.patches.empty() ||
                       (layer.fill && py_truthy(*layer.fill)) || (layer.adjust && py_truthy(*layer.adjust)) || (layer.tone && py_truthy(*layer.tone)) ||
                       layer.asset || layer.placement_mm;
    if (drawn) refuse("layer " + layer.id + " is a smart object: it holds no pixels, lines or fill of its own (rasterize it first)");
    if (layer.clip_to != "frame" && layer.clip_to != "bleed" && layer.clip_to != "none") refuse(who + ": clip_to is frame, bleed or none");
}

void check_contents(const std::string& id, const SmartContents& c) {
    const std::string who = "the smart object contents " + id;
    if (!is_contents_id(id)) refuse("a smart object contents' id is sc_ and 12 hex digits, not " + py_repr_str(id));
    if (code_points(c.name) > kNameMax) refuse(who + ": its name is longer than 255 characters");
    if (c.kind == "page") {
        if (!c.page) refuse(who + " have no page");
        return;
    }
    if (c.kind != "file") refuse(who + ": kind is page or file");
    if (!is_ref(c.asset)) refuse(who + ": its file is an asset (sha256:…)");
    const auto format = format_of_suffix(c.suffix);
    if (!format || *format != c.format) refuse(who + ": an unknown kind of file (" + c.suffix + ", " + c.format + ")");
    if (c.width_px < 1 || c.height_px < 1 || c.width_px > kSideMax || c.height_px > kSideMax) refuse(who + ": its size in pixels");
    if (!std::isfinite(c.dpi) || c.dpi <= 0 || c.dpi > 1e6) refuse(who + ": its dpi");
    if (c.depth != 1 && c.depth != 8 && c.depth != 16 && c.depth != 32) refuse(who + ": its depth is 1, 8, 16 or 32 bits");
    std::string mode = c.mode;
    if (mode.ends_with("a")) mode.pop_back();  // (with an alpha: rgba, graya, cmyka, laba)
    if (!in(kModes, mode)) refuse(who + ": its mode is gray, rgb, cmyk or lab");
    if (!c.profile.is_object()) refuse(who + ": its colour use is an object");
    const Json* use = get(c.profile, "use");
    if (use == nullptr || !use->is_string() || !in(kUses, use->get_ref<const std::string&>())) refuse(who + ": its colour use is keep, embedded or assign");
    if (*use == "assign") {
        const Json* icc = get(c.profile, "icc");
        if (icc == nullptr || !icc->is_string() || !is_ref(icc->get_ref<const std::string&>())) refuse(who + ": assign names its profile (sha256:…)");
    }
    if (const Json* intent = get(c.profile, "intent"); intent != nullptr && (!intent->is_string() || !in(kIntents, intent->get_ref<const std::string&>())))
        refuse(who + ": an unknown rendering intent");
    if (const Json* bpc = get(c.profile, "bpc"); bpc != nullptr && !bpc->is_boolean()) refuse(who + ": bpc is true or false");
    if (c.link) {
        try {
            check_link(*c.link);
        } catch (const Error& error) {
            refuse(who + ": " + error.what());
        }
    }
    if (c.psd) {  // (PC3 A3: a PSD's linked file, {"uuid", "item": its item without the file's bytes, "at": where they go})
        if (!c.psd->is_object()) refuse(who + ": its psd is {uuid, item, at}");
        for (const auto& [key, value] : c.psd->items()) {
            if (key != "uuid" && key != "item" && key != "at") refuse(who + ": its psd's " + py_repr_str(key) + " is not known to this build");
        }
        const Json* uuid = get(*c.psd, "uuid");
        if (uuid == nullptr || !uuid->is_string() || uuid->get_ref<const std::string&>().empty() || uuid->get_ref<const std::string&>().size() > 255)
            refuse(who + ": its psd's uuid is text of 1 to 255 bytes");
        const Json* item = get(*c.psd, "item");
        if (item == nullptr || !item->is_string() || !is_ref(item->get_ref<const std::string&>())) refuse(who + ": its psd's item is an asset (sha256:…)");
        const Json* at = get(*c.psd, "at");
        if (at == nullptr || !at->is_number_integer() || at->get<std::int64_t>() < 0) refuse(who + ": its psd's at is where the file's bytes go");
    }
}

}  // namespace

bool filter_kind(std::string_view kind) {
    if (std::find(kShapeFilters.begin(), kShapeFilters.end(), kind) != kShapeFilters.end()) return true;
    if (bend_filter(kind)) return true;  // (PC3 A5)
    const AdjustKind* found = adjust_kind(kind);
    return found != nullptr && found->filter && found->smart;
}

bool bend_filter(std::string_view kind) { return std::find(kBendFilters.begin(), kBendFilters.end(), kind) != kBendFilters.end(); }

bool bent(const Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return false;
    for (const char* key : {"perspective", "warp"}) {
        const Json* v = get(*layer.smart, key);
        if (v != nullptr && !v->is_null()) return true;
    }
    return false;
}

Corners corners_on_page(const Layer& layer, const SmartContents& contents) {
    const Json* quad = layer.smart && layer.smart->is_object() ? get(*layer.smart, "perspective") : nullptr;
    Corners out;
    if (quad != nullptr && quad->is_array() && quad->size() == 4) {
        bool read = true;
        for (std::size_t i = 0; i < 4 && read; ++i) {
            const Json& p = (*quad)[i];
            read = p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number();
            if (read) out[i] = {p[0].get<double>(), p[1].get<double>()};
        }
        if (read) return out;
    }
    const Matrix m = matrix_of(layer);
    const auto [w, h] = size_mm(contents);
    const std::array<std::pair<double, double>, 4> box{{{0, 0}, {w, 0}, {w, h}, {0, h}}};
    for (std::size_t i = 0; i < 4; ++i) out[i] = m.apply(box[i].first, box[i].second);
    return out;
}

bool convex(const Corners& p) {
    int sign = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& [x0, y0] = p[i];
        const auto& [x1, y1] = p[(i + 1) % 4];
        const auto& [x2, y2] = p[(i + 2) % 4];
        const double cross = (x1 - x0) * (y2 - y1) - (y1 - y0) * (x2 - x1);
        const int here = cross > 1e-9 ? 1 : cross < -1e-9 ? -1 : 0;
        if (here == 0 || (sign != 0 && here != sign)) return false;
        sign = here;
    }
    return true;
}

void set_corners(Layer& layer, const SmartContents& contents, const Corners& p) {
    for (const auto& [x, y] : p) {
        if (!std::isfinite(x) || !std::isfinite(y)) refuse("a smart object's corners must be finite numbers");
    }
    if (!convex(p)) refuse("a smart object's corners must make a convex shape");
    const auto [w, h] = size_mm(contents);
    // (the matrix through the top left, top right and bottom left corners)
    const Matrix m = snapped(Matrix{(p[1].first - p[0].first) / w, (p[1].second - p[0].second) / w, (p[3].first - p[0].first) / h,
                                    (p[3].second - p[0].second) / h, p[0].first, p[0].second});
    if (!(std::abs(m.det()) > kDetMin)) refuse("the transform squashes the smart object flat");
    double size = 1;
    for (const auto& [x, y] : p) size = std::max({size, std::abs(x), std::abs(y)});
    const double gap = std::max(std::abs(p[0].first + p[2].first - p[1].first - p[3].first), std::abs(p[0].second + p[2].second - p[1].second - p[3].second));
    set_matrix(layer, m);
    if (gap <= 1e-9 * size) {
        put_setting(layer, "perspective", Json());
    } else {
        Json corners = Json::array();
        for (const auto& [x, y] : p) corners.push_back(Json::array({x, y}));
        put_setting(layer, "perspective", std::move(corners));
    }
    put_setting(layer, "psd", Json());  // (PC3 A3: a PSD's picture and corners are no longer where it is)
}

void check_warp(const Json& warp) {
    if (!warp.is_object()) refuse("a smart object's warp is {style, bend, h_distort, v_distort, vertical, bounds, mesh?}");
    known_keys("its warp", warp, {"style", "bend", "h_distort", "v_distort", "vertical", "bounds", "mesh"});
    const Json* style = get(warp, "style");
    const auto found = style != nullptr && style->is_string() ? page_geometry::warp_style_from(style->get<std::string>()) : std::nullopt;
    if (!found) refuse("a smart object's warp has a style (none, custom, arc, arcLower, …)");
    for (const char* key : {"bend", "h_distort", "v_distort"}) {
        const Json* v = get(warp, key);
        if (v == nullptr || v->is_null()) continue;
        const double x = finite(*v, std::string("its warp's ") + key);
        if (x < -100 || x > 100) refuse(std::string("its warp's ") + key + " is −100 to 100");
    }
    if (const Json* v = get(warp, "vertical"); v != nullptr && !v->is_null() && !v->is_boolean()) refuse("its warp's vertical is true or false");
    const Json* b = get(warp, "bounds");
    if (b == nullptr || !b->is_array() || b->size() != 4) refuse("its warp's bounds are [x0, y0, x1, y1]");
    std::array<double, 4> box{};
    for (std::size_t i = 0; i < 4; ++i) box[i] = finite((*b)[i], "its warp's bounds");
    if (!(box[2] > box[0] && box[3] > box[1])) refuse("its warp's bounds have no size");
    const Json* mesh = get(warp, "mesh");
    if (mesh == nullptr || mesh->is_null()) return;
    if (*found != page_geometry::WarpStyle::Custom) refuse("only a custom warp has a mesh");
    if (!mesh->is_object()) refuse("its warp's mesh is {us, vs, points}");
    known_keys("its warp's mesh", *mesh, {"us", "vs", "points"});
    page_geometry::BezierMesh bm;
    for (const auto& [key, list] : {std::pair{"us", &bm.us}, std::pair{"vs", &bm.vs}}) {
        const Json* k = get(*mesh, key);
        if (k == nullptr || !k->is_array() || k->size() > 65) refuse("its warp's mesh's us and vs are 2 to 65 numbers from 0 to 1");
        for (const Json& x : *k) list->push_back(finite(x, "its warp's mesh"));
    }
    const Json* points = get(*mesh, "points");
    if (points == nullptr || !points->is_array()) refuse("its warp's mesh has its points");
    for (const Json& p : *points) {
        const auto [x, y] = point_at(p, "its warp's mesh's point");
        if (std::abs(x) > vector::kCoordinateMax || std::abs(y) > vector::kCoordinateMax) refuse("its warp's mesh's points are too far");
        bm.points.push_back(vector::XY{x, y});
    }
    if (!bm.valid()) refuse("its warp's mesh needs (3·columns + 1) × (3·rows + 1) points and its us and vs from 0 to 1, rising");
}

bool filters_drawn(const Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return false;
    const Json* on = get(*layer.smart, "filters_on");
    if (on != nullptr && on->is_boolean() && !on->get<bool>()) return false;
    const Json* filters = get(*layer.smart, "filters");
    if (filters == nullptr || !filters->is_array()) return false;
    return std::any_of(filters->begin(), filters->end(), [](const Json& f) {
        const Json* visible = f.is_object() ? get(f, "visible") : nullptr;
        return f.is_object() && !(visible != nullptr && visible->is_boolean() && !visible->get<bool>());
    });
}

std::optional<std::string> suffix_of_name(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return std::nullopt;
    std::string suffix(name.substr(dot));
    for (char& c : suffix) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (suffix == ".jpeg" || suffix == ".jpe") suffix = ".jpg";
    if (suffix == ".tiff") suffix = ".tif";
    if (!format_of_suffix(suffix)) return std::nullopt;
    return suffix;
}

std::optional<std::string> format_of_suffix(std::string_view suffix) {
    for (const FileFormat& f : kFileFormats) {
        if (f.suffix == suffix) return std::string(f.format);
    }
    return std::nullopt;
}

Matrix multiply(const Matrix& l, const Matrix& r) {
    return Matrix{l.a * r.a + l.c * r.b, l.b * r.a + l.d * r.b, l.a * r.c + l.c * r.d,
                  l.b * r.c + l.d * r.d, l.a * r.e + l.c * r.f + l.e, l.b * r.e + l.d * r.f + l.f};
}

std::optional<Matrix> inverse(const Matrix& m) {
    const double det = m.det();
    if (!std::isfinite(det) || std::abs(det) <= 1e-300) return std::nullopt;
    const double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
    return Matrix{a, b, c, d, -(a * m.e + c * m.f), -(b * m.e + d * m.f)};
}

Matrix snapped(const Matrix& m) {
    const auto snap = [](double v) {
        const double r = std::round(v * 1e9) / 1e9;
        return std::abs(v - r) < 1e-9 ? (r == 0 ? 0.0 : r) : v;
    };
    return Matrix{snap(m.a), snap(m.b), snap(m.c), snap(m.d), snap(m.e), snap(m.f)};
}

bool is_identity(const Matrix& m) { return m == Matrix{}; }

bool axis_aligned(const Matrix& m) { return m.b == 0 && m.c == 0 && m.a > 0 && m.d > 0; }

double largest_scale(const Matrix& m) {
    // (the singular values of [[a, c], [b, d]]: the root of the larger eigenvalue of its square)
    const double p = m.a * m.a + m.b * m.b, q = m.c * m.c + m.d * m.d, r = m.a * m.c + m.b * m.d;
    const double mid = (p + q) / 2, spread = std::sqrt(std::max(0.0, (p - q) * (p - q) / 4 + r * r));
    return std::sqrt(mid + spread);
}

Json to_json(const Matrix& m) { return Json::array({m.a, m.b, m.c, m.d, m.e, m.f}); }

Matrix matrix_from(const Json& value) {
    if (!value.is_array() || value.size() != 6) refuse("a smart object's matrix is six numbers [a, b, c, d, e, f]");
    std::array<double, 6> v{};
    for (std::size_t i = 0; i < 6; ++i) v[i] = finite(value[i], "a smart object's matrix");
    return Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
}

std::string new_contents_id() { return "sc_" + new_id(); }

bool is_contents_id(std::string_view text) { return text.size() == 15 && text.starts_with("sc_") && is_hex(text.substr(3)); }

bool is_smart(const Layer& layer) { return layer.kind == LayerKind::Smart; }

std::optional<std::string> contents_id(const Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return std::nullopt;
    const Json* id = get(*layer.smart, "contents");
    if (id == nullptr || !id->is_string()) return std::nullopt;
    return id->get<std::string>();
}

Matrix matrix_of(const Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return Matrix{};
    const Json* m = get(*layer.smart, "matrix");
    if (m == nullptr) return Matrix{};
    try {
        return matrix_from(*m);
    } catch (const Error&) {
        return Matrix{};  // (validate_smart refuses it)
    }
}

void set_matrix(Layer& layer, const Matrix& m) {
    if (!layer.smart || !layer.smart->is_object()) layer.smart = Json::object();
    (*layer.smart)["matrix"] = to_json(snapped(m));
}

void put_setting(Layer& layer, std::string_view key, Json value) {
    if (!layer.smart || !layer.smart->is_object()) layer.smart = Json::object();
    Json& s = *layer.smart;
    const std::string name(key);
    if (value.is_null() || s.contains(name)) {
        if (value.is_null()) s.erase(name);
        else s[name] = std::move(value);
        return;
    }
    const auto rank = [](std::string_view k) { return std::find(kSettingKeys.begin(), kSettingKeys.end(), k) - kSettingKeys.begin(); };
    Json out = Json::object();
    for (auto& [k, v] : s.items()) {
        if (!value.is_null() && rank(k) > rank(key)) out[name] = std::exchange(value, Json());
        out[k] = std::move(v);
    }
    if (!value.is_null()) out[name] = std::move(value);
    s = std::move(out);
}

Json settings(const std::string& contents, const Matrix& m, std::string_view interp) {
    Json out = Json::object();
    out["contents"] = contents;
    out["matrix"] = to_json(snapped(m));
    out["interp"] = std::string(interp);
    return out;
}

const SmartContents* contents_of(const Document& doc, const Layer& layer) {
    const auto id = contents_id(layer);
    if (!id) return nullptr;
    const auto it = doc.smart_contents.find(*id);
    return it == doc.smart_contents.end() ? nullptr : it->second.get();
}

std::pair<double, double> size_mm(const SmartContents& contents) {
    if (contents.kind == "page" && contents.page) return {contents.page->spec.width_mm.value(), contents.page->spec.height_mm.value()};
    const double dpi = contents.dpi > 0 ? contents.dpi : 72.0;
    return {static_cast<double>(contents.width_px) * 25.4 / dpi, static_cast<double>(contents.height_px) * 25.4 / dpi};
}

Json file_json(const SmartContents& c) {
    Json out = Json::object();
    out["kind"] = "file";
    out["name"] = c.name;
    out["asset"] = c.asset;
    out["suffix"] = c.suffix;
    out["format"] = c.format;
    out["size_px"] = Json::array({c.width_px, c.height_px});
    out["dpi"] = c.dpi;
    out["depth"] = c.depth;
    out["mode"] = c.mode;
    out["profile"] = c.profile;
    if (c.link) out["link"] = *c.link;
    if (c.psd) out["psd"] = *c.psd;
    return out;
}

SmartContents contents_from_json(const Json& value) {
    if (!value.is_object()) refuse("smart object contents are an object");
    SmartContents c;
    for (const auto& [key, item] : value.items()) {
        if (!in(kFileKeys, key) && key != "page") refuse("the smart object contents key " + py_repr_str(key) + " is not known to this build");
    }
    const Json* kind = get(value, "kind");
    if (kind == nullptr || !kind->is_string()) refuse("smart object contents have a kind (page or file)");
    c.kind = kind->get<std::string>();
    if (const Json* name = get(value, "name"); name != nullptr) {
        if (!name->is_string()) refuse("a smart object contents' name is text");
        c.name = name->get<std::string>();
    }
    if (c.kind == "page") return c;
    if (c.kind != "file") refuse("smart object contents are of kind page or file");
    const auto text = [&](const char* key) {
        const Json* v = get(value, key);
        if (v == nullptr || !v->is_string()) refuse(std::string("a smart object file's ") + key + " is text");
        return v->get<std::string>();
    };
    c.asset = text("asset");
    c.suffix = text("suffix");
    c.format = text("format");
    c.mode = text("mode");
    const Json* size = get(value, "size_px");
    if (size == nullptr || !size->is_array() || size->size() != 2 || !(*size)[0].is_number_integer() || !(*size)[1].is_number_integer())
        refuse("a smart object file's size_px is [width, height] in pixels");
    c.width_px = (*size)[0].get<std::int64_t>();
    c.height_px = (*size)[1].get<std::int64_t>();
    const Json* dpi = get(value, "dpi");
    c.dpi = dpi != nullptr ? finite(*dpi, "a smart object file's dpi") : 0.0;
    const Json* depth = get(value, "depth");
    if (depth == nullptr || !depth->is_number_integer()) refuse("a smart object file's depth is a whole number");
    c.depth = static_cast<int>(depth->get<std::int64_t>());
    if (const Json* profile = get(value, "profile"); profile != nullptr) c.profile = *profile;
    if (const Json* link = get(value, "link"); link != nullptr && !link->is_null()) c.link = *link;
    if (const Json* psd = get(value, "psd"); psd != nullptr && !psd->is_null()) c.psd = *psd;
    return c;
}

void check_link(const Json& link) {
    if (!link.is_object()) refuse("its link is {path, relative, sha256, bytes}");
    for (const auto& [key, value] : link.items()) {
        if (key != "path" && key != "relative" && key != "sha256" && key != "bytes") refuse("its link's " + py_repr_str(key) + " is not known to this build");
    }
    const Json* path = get(link, "path");
    if (path == nullptr || !path->is_string() || path->get_ref<const std::string&>().empty()) refuse("its link names the file's path");
    if (path->get_ref<const std::string&>().size() > kPathMax) refuse("its link's path is longer than 4096 bytes");
    if (const Json* relative = get(link, "relative"); relative != nullptr && !relative->is_null()) {
        if (!relative->is_string() || relative->get_ref<const std::string&>().size() > kPathMax) refuse("its link's relative path is text of at most 4096 bytes");
    }
    const Json* sha = get(link, "sha256");
    if (sha == nullptr || !sha->is_string() || sha->get_ref<const std::string&>().size() != 64 || !is_hex(sha->get_ref<const std::string&>()))
        refuse("its link's sha256 is 64 hex digits");
    const Json* bytes = get(link, "bytes");
    if (bytes == nullptr || !bytes->is_number_integer() || bytes->get<std::int64_t>() < 0) refuse("its link's bytes is the file's size");
}

Json link_json(const std::string& path, const std::optional<std::string>& relative, const std::string& sha256, std::int64_t bytes) {
    Json out = Json::object();
    out["path"] = path;
    if (relative && !relative->empty()) out["relative"] = *relative;
    out["sha256"] = sha256;
    out["bytes"] = bytes;
    return out;
}

std::optional<Showing> showing(const Document& doc, const std::string& id) {
    const auto on = [&id](const Page& page) -> const Layer* {
        for (const Layer& layer : page.layers) {
            if (is_smart(layer) && contents_id(layer) == id) return &layer;
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        if (const Layer* layer = on(*doc.pages[i])) return Showing{std::nullopt, doc.pages[i]->index, layer->id};
    }
    for (const auto& [inside, contents] : doc.smart_contents) {
        if (inside == id || !contents || !contents->page) continue;
        if (const Layer* layer = on(*contents->page)) return Showing{inside, contents->page->index, layer->id};
    }
    return std::nullopt;
}

std::set<std::string> shown_on(const Page& page) {
    std::set<std::string> out;
    for (const Layer& layer : page.layers) {
        if (!is_smart(layer)) continue;
        if (const auto id = contents_id(layer)) out.insert(*id);
    }
    return out;
}

void renew_containers(Document& doc, const std::string& id) {
    std::vector<std::string> containers;
    for (const auto& [other, contents] : doc.smart_contents) {
        if (other == id || !contents || !contents->page) continue;
        std::set<std::string> seen;
        const std::set<std::string> direct = shown_on(*contents->page);
        std::vector<std::string> todo(direct.begin(), direct.end());
        bool found = false;
        while (!todo.empty() && !found) {
            const std::string next = std::move(todo.back());
            todo.pop_back();
            found = next == id;
            if (found || !seen.insert(next).second) continue;
            const auto it = doc.smart_contents.find(next);
            if (it == doc.smart_contents.end() || !it->second || !it->second->page) continue;
            for (const std::string& inner : shown_on(*it->second->page)) todo.push_back(inner);
        }
        if (found) containers.push_back(other);
    }
    for (const std::string& other : containers) (void)doc.edit_contents(other);
}

std::vector<std::string> used_contents(const Document& doc) {
    std::set<std::string> seen;
    std::vector<std::string> todo;
    for (const auto& page : doc.pages) {
        for (const std::string& id : shown_on(*page)) todo.push_back(id);
    }
    while (!todo.empty()) {
        const std::string id = std::move(todo.back());
        todo.pop_back();
        if (!seen.insert(id).second) continue;
        const auto it = doc.smart_contents.find(id);
        if (it == doc.smart_contents.end() || !it->second || !it->second->page) continue;
        for (const std::string& inner : shown_on(*it->second->page)) todo.push_back(inner);
    }
    return {seen.begin(), seen.end()};
}

bool uses_smart(const Document& doc) {
    if (!doc.smart_contents.empty()) return true;
    for (const auto& page : doc.pages) {
        for (const Layer& layer : page->layers) {
            if (is_smart(layer)) return true;
        }
    }
    return false;
}

std::vector<std::size_t> pages_using(const Document& doc, const std::string& id) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        std::set<std::string> seen;
        std::vector<std::string> todo;
        for (const std::string& shown : shown_on(*doc.pages[i])) todo.push_back(shown);
        bool found = false;
        while (!todo.empty() && !found) {
            const std::string next = std::move(todo.back());
            todo.pop_back();
            if (next == id) found = true;
            if (!seen.insert(next).second) continue;
            const auto it = doc.smart_contents.find(next);
            if (it == doc.smart_contents.end() || !it->second || !it->second->page) continue;
            for (const std::string& inner : shown_on(*it->second->page)) todo.push_back(inner);
        }
        if (found) out.push_back(i);
    }
    return out;
}

void validate_smart(const Document& doc) {
    if (doc.smart_contents.empty()) {
        bool any = false;
        for (std::size_t i = 0; i < doc.pages.size() && !any; ++i) {
            for (const Layer& layer : doc.pages[i]->layers) any = any || is_smart(layer) || layer.smart.has_value();
        }
        if (!any) return;
    }
    for (const auto& [id, contents] : doc.smart_contents) {
        if (!contents) refuse("the smart object contents " + id + " are empty");
        check_contents(id, *contents);
    }
    each_page(doc, [&](const Page& page, const std::string& in_contents) {
        for (const Layer& layer : page.layers) {
            if (!is_smart(layer)) {
                // (a smart object an op drew on as on pixels: refused as such)
                if (layer.smart) refuse("layer " + layer.id + " is a smart object: it holds no pixels, lines or fill of its own (rasterize it first)");
                continue;
            }
            check_settings(layer);
            const std::string id = *contents_id(layer);
            if (!doc.smart_contents.contains(id)) refuse("layer " + layer.id + " shows smart object contents the book does not hold (" + id + ")");
            if (id == in_contents) refuse("the smart object contents " + id + " show themselves");
        }
    });
    // (contents inside contents: a tree no deeper than kDepthMax, seen from each page)
    std::map<std::string, int> depth;  // (how deep below it its contents go; -1 while it is being looked at)
    std::function<int(const std::string&)> below = [&](const std::string& id) -> int {
        if (const auto it = depth.find(id); it != depth.end()) {
            if (it->second < 0) refuse("the smart object contents " + id + " show themselves (inside other contents)");
            return it->second;
        }
        depth[id] = -1;
        int most = 0;
        const auto found = doc.smart_contents.find(id);
        if (found != doc.smart_contents.end() && found->second && found->second->page) {
            for (const std::string& inner : shown_on(*found->second->page)) most = std::max(most, below(inner) + 1);
        }
        depth[id] = most;
        return most;
    };
    for (const auto& [id, contents] : doc.smart_contents) {
        if (below(id) + 1 > kDepthMax) refuse("smart objects inside smart objects go deeper than " + std::to_string(kDepthMax));
    }
    // (the contents' pages are pages of the book: checked as its pages are — colour mode, inks, channels —, and the
    // book's budget for precise pixels counts their pictures with its pages')
    std::vector<PagePtr> inner;
    for (const auto& [id, contents] : doc.smart_contents) {
        if (contents && contents->page) inner.push_back(contents->page);
    }
    if (!inner.empty()) {
        Document probe;
        probe.rgb = doc.rgb;
        probe.color = doc.color;
        probe.text_gamma = doc.text_gamma;
        probe.pages = inner;
        validate_ink_document(probe);
        validate_color_model(probe);
        validate_channels(probe);
        probe.deferred = doc.deferred;
        probe.pages.insert(probe.pages.begin(), doc.pages.begin(), doc.pages.end());
        validate_color_document(probe);
    }
}

std::optional<Layer> plain_layer(const Document* doc, const Page& page, const Layer& layer) {
    if (doc == nullptr || !is_smart(layer) || !layer.smart || !layer.smart->is_object()) return std::nullopt;
    const SmartContents* contents = contents_of(*doc, layer);
    if (contents == nullptr || contents->kind != "page" || !contents->page) return std::nullopt;
    const Json& s = *layer.smart;
    const bool filtered = filters_drawn(layer);  // (smart filters hidden or off: as none)
    for (const auto& [key, value] : s.items()) {
        if (key == "contents" || key == "matrix" || key == "interp") continue;
        if ((key == "filters" || key == "filter_mask") && !filtered) continue;
        if (key == "filters_on" || ((key == "perspective" || key == "warp" || key == "stack" || key == "psd") && value.is_null())) continue;
        return std::nullopt;
    }
    if (!is_identity(matrix_of(layer))) return std::nullopt;
    const Page& inner_page = *contents->page;
    if (inner_page.layers.size() != 1) return std::nullopt;
    const Layer& inner = inner_page.layers.front();
    if (inner.kind == LayerKind::Folder || inner.kind == LayerKind::Adjust) return std::nullopt;
    if (!(inner.blend.empty() || inner.blend == "normal") || inner.clip) return std::nullopt;
    if (layer.mask || layer.vector_mask || (layer.effect && py_truthy(*layer.effect)) || (layer.screen && py_truthy(*layer.screen)) ||
        (layer.color && !layer.color->empty()))
        return std::nullopt;
    if (!(inner_page.spec.width_mm == page.spec.width_mm) || !(inner_page.spec.height_mm == page.spec.height_mm) ||
        !(inner_page.spec.dpi == page.spec.dpi) || inner_page.spec.expression != page.spec.expression || !(inner_page.spec.bleed_mm == page.spec.bleed_mm))
        return std::nullopt;
    if (inner_page.color != page.color || inner_page.rgb != page.rgb) return std::nullopt;
    if (!same_frames(inner_page.frames, page.frames)) return std::nullopt;
    Layer out = inner;
    out.id = layer.id;
    out.title = layer.title;
    out.parent_id = layer.parent_id;
    out.visible = layer.visible && inner.visible;
    out.exportable = layer.exportable && inner.exportable;
    out.opacity = layer.opacity * inner.opacity;
    out.blend = layer.blend;
    out.clip = layer.clip;
    out.locked = layer.locked || inner.locked;
    if (layer.role == LayerRole::Name || layer.role == LayerRole::Draft) out.role = layer.role;  // (a guide: never printed)
    return out;
}

std::optional<Page> with_plain_layers(const Page& page, const Document* doc) {
    if (doc == nullptr || std::none_of(page.layers.begin(), page.layers.end(), [](const Layer& l) { return is_smart(l); })) return std::nullopt;
    std::optional<Page> out;
    for (int round = 0; round < kDepthMax; ++round) {  // (contents of one smart object inside another: one level a round)
        const Page& now = out ? *out : page;
        bool changed = false;
        std::vector<Layer> layers;
        layers.reserve(now.layers.size());
        for (const Layer& layer : now.layers) {
            if (auto plain = plain_layer(doc, now, layer)) {
                layers.push_back(std::move(*plain));
                changed = true;
            } else {
                layers.push_back(layer);
            }
        }
        if (!changed) break;
        if (!out) out = page;
        out->layers = std::move(layers);
    }
    return out;
}

bool same_panels(const std::vector<Frame>& a, const std::vector<Frame>& b) { return same_frames(a, b); }

void bring_contents(Document& doc, const Document& from, Page& page) {
    std::map<std::string, std::string> renamed;  // (their id → its id here)
    std::function<void(Page&)> bring = [&](Page& on) {
        for (Layer& layer : on.layers) {
            if (!is_smart(layer)) continue;
            const auto id = contents_id(layer);
            if (!id) continue;
            auto known = renamed.find(*id);
            if (known == renamed.end()) {
                const auto found = from.smart_contents.find(*id);
                if (found == from.smart_contents.end() || !found->second) continue;  // (validate_smart says so)
                std::string mine = *id;
                while (doc.smart_contents.contains(mine)) mine = new_contents_id();
                known = renamed.emplace(*id, mine).first;
                auto contents = std::make_shared<SmartContents>(*found->second);
                if (contents->page) {
                    contents->page = std::make_shared<Page>(*contents->page);
                    bring(*contents->page);
                }
                for (const std::string& ref : {contents->asset, py_str(py_get(contents->profile, "icc", Json("")))}) {
                    if (const auto bytes = from.smart_files.find(ref); bytes != from.smart_files.end()) doc.smart_files.emplace(ref, bytes->second);
                }
                doc.smart_contents[mine] = std::move(contents);
            }
            (*layer.smart)["contents"] = known->second;
        }
    };
    bring(page);
}

void transform_layer(Layer& layer, const Matrix& m) {
    if (!is_smart(layer) || !layer.smart || !layer.smart->is_object()) return;
    set_matrix(layer, multiply(m, matrix_of(layer)));
    layer.smart->erase("psd");  // (PC3 A3: a PSD's picture and corners are no longer where it is)
    if (Json* corners = layer.smart->contains("perspective") ? &(*layer.smart)["perspective"] : nullptr; corners != nullptr && corners->is_array()) {
        for (Json& point : *corners) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) continue;
            const auto [x, y] = m.apply(point[0].get<double>(), point[1].get<double>());
            point = Json::array({x, y});
        }
    }
}

}  // namespace genko::core::smart
