// PC5 J-a (layer-comps@1, render/ops_layer_comps.hpp): the layer comps of a page — PhotoCraft v0.2.0's
// crates/doc/src/comps.rs and engine/src/comps_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md), in mm, on Genko's
// layers.

#include "render/ops_layer_comps.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "core/error.hpp"
#include "core/groups.hpp"
#include "core/layer_styles.hpp"
#include "core/ops_util.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/layer_bounds.hpp"
#include "render/layer_transform.hpp"
#include "render/ops_layer_styles.hpp"
#include "render/own_pixels.hpp"
#include "render/page.hpp"

namespace genko::render::layer_comps {

using core::CompAppearance;
using core::CompLayerState;
using core::Json;
using core::LayerComp;
using core::LayerComps;
using core::OpError;

namespace {

const core::Layer* layer_named(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers)
        if (layer.id == id) return &layer;
    return nullptr;
}

double r4(double v) { return core::py_round(v, 4); }

}  // namespace

Placing placing(const core::Document& doc, const core::Page& page, const core::Layer& layer) {
    Placing out;
    // (a folder's place is its layers'; a fill and a correction layer cover the page)
    if (layer.kind == core::LayerKind::Folder || layer.kind == core::LayerKind::Fill || layer.kind == core::LayerKind::Adjust) return out;
    std::optional<std::array<double, 4>> box;  // x0, y0, x1, y1 mm
    const auto unite = [&box](double x0, double y0, double x1, double y1) {
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
        box = box ? std::array<double, 4>{std::min((*box)[0], x0), std::min((*box)[1], y0), std::max((*box)[2], x1), std::max((*box)[3], y1)}
                  : std::array<double, 4>{x0, y0, x1, y1};
    };
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    // its own pixels (8-bit, precise, inks): the box of those that show, on their own grid — not cut by the panels
    if (w > 0 && h > 0) {
        if (const std::optional<own_pixels::OwnPixels> own = own_pixels::OwnPixels::of(page, layer)) {
            const double sx = own->width() / w, sy = own->height() / h;
            out.grid = std::array<double, 2>{sx, sy};
            if (const auto shown = own->shown()) unite(shown->x0 / sx, shown->y0 / sy, shown->x1 / sx, shown->y1 / sy);
        }
    }
    // its pen lines and shape parts where they are (the paper's edge and the panels not cutting them)
    if (layer.strokes) {
        for (const core::StrokePtr& stroke : layer.strokes->items) {
            const double r = stroke->width_mm / 2;
            for (const core::PointF& p : stroke->points) unite(p.x - r, p.y - r, p.x + r, p.y + r);
        }
    }
    if (layer.shapes) {
        for (const core::vector::ShapePart& part : layer.shapes->items) {
            const auto b = part.path.control_bounds();
            if (!b) continue;
            const double r = part.stroke ? part.stroke->width_mm / 2 : 0.0;
            unite((*b)[0] - r, (*b)[1] - r, (*b)[2] + r, (*b)[3] + r);
        }
    }
    // a placed picture where it lies
    if (layer.kind == core::LayerKind::Placed && layer.placement_mm) {
        const core::Rect& r = *layer.placement_mm;
        unite(r.x.value(), r.y.value(), r.x.value() + r.width.value(), r.y.value() + r.height.value());
    }
    if (!box) {
        // (anything else — a smart object, a type layer, a tone, a pen layer's patches —: what it shows on the paper, the
        // panels not cutting it)
        core::Layer shown = layer;
        shown.visible = true;
        shown.opacity = 1.0;
        shown.panel_clip = false;
        shown.mask.reset();
        shown.vector_mask.reset();
        const Image picture = layer_image(page, shown, layer_bounds::kDpi, &doc, true);
        if (const auto b = picture.getbbox()) {
            const auto g = layer_bounds::grid(page);
            unite(b->x0 / g[0], b->y0 / g[1], b->x1 / g[0], b->y1 / g[1]);
        }
    }
    if (box) out.at = std::array<double, 2>{r4((*box)[0]), r4((*box)[1])};
    return out;
}

std::optional<std::array<double, 2>> position_of(const core::Document& doc, const core::Page& page, const core::Layer& layer) {
    return placing(doc, page, layer).at;
}

std::vector<CompLayerState> capture(const core::Document& doc, const core::Page& page) {
    std::vector<CompLayerState> out;
    out.reserve(page.layers.size());
    for (const core::Layer& layer : page.layers) {
        CompLayerState s;
        s.layer = layer.id;
        s.visible = layer.visible;
        s.has_position = true;
        s.position = position_of(doc, page, layer);
        CompAppearance look;
        look.blend = layer.blend.empty() ? std::string("normal") : layer.blend;
        look.opacity = std::clamp(layer.opacity, 0.0, 1.0);
        look.style = layer.style;
        s.appearance = std::move(look);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> missing(const core::Page& page, const std::vector<CompLayerState>& states) {
    std::vector<std::string> out;
    for (const CompLayerState& s : states) {
        if (layer_named(page, s.layer) == nullptr && std::find(out.begin(), out.end(), s.layer) == out.end()) out.push_back(s.layer);
    }
    return out;
}

void apply(core::Document& doc, std::size_t at, const std::vector<CompLayerState>& states, bool visibility, bool position, bool appearance) {
    for (const CompLayerState& s : states) {
        if (layer_named(doc.page(at), s.layer) == nullptr) continue;  // (a layer gone since: left out)
        if (position && s.has_position && s.position) {
            const core::Page& page = doc.page(at);
            const core::Layer& layer = *layer_named(page, s.layer);
            const Placing now = placing(doc, page, layer);
            if (now.at) {
                // (a layer of its own pixels by whole pixels of its own grid — copied, never sampled again; lines, parts, a
                // placed picture, a smart object exactly)
                double dx = (*s.position)[0] - (*now.at)[0], dy = (*s.position)[1] - (*now.at)[1];
                if (now.grid) {
                    dx = std::round(dx * (*now.grid)[0]) / (*now.grid)[0];
                    dy = std::round(dy * (*now.grid)[1]) / (*now.grid)[1];
                }
                if (std::fabs(dx) > 1e-9 || std::fabs(dy) > 1e-9) {
                    const core::Rect paper = page.paper_rect_mm();
                    const auto m = layer_transform::affine_map({1, 0, 0, 1, dx, dy}, {0, 0, paper.width.value(), paper.height.value()});
                    layer_transform::move_layer(doc, at, s.layer, m, std::nullopt, resample::Interp::Bicubic);
                }
            }
        }
        core::Page& page = doc.edit_page(at);
        core::Layer& layer = page.layers[core::layer_by_id(page, s.layer)];
        if (visibility && s.visible) layer.visible = *s.visible;
        if (appearance && s.appearance) {
            // (groups@1, PC5 I-b: a group's 通過 kept as it was recorded)
            layer.blend = s.appearance->blend == core::kPassThrough && core::is_group(layer) ? std::string(core::kPassThrough) : core::blend_mode(Json(s.appearance->blend));
            layer.opacity = std::clamp(s.appearance->opacity, 0.0, 1.0);
            std::optional<Json> style;
            if (s.appearance->style && core::takes_layer_style(layer)) {
                try {
                    const Json normal = core::layer_style_spec(*s.appearance->style);
                    if (!normal.is_null()) style = normal;
                } catch (const core::Error& error) {
                    throw OpError(std::string("the comp's layer style of ") + layer.id + " is not one: " + error.what());
                }
            }
            if (style != layer.style) {
                layer.style = std::move(style);
                forget_psd_effects(layer);  // (its PSD's effects are not its own any more)
            }
        }
    }
}

const LayerComp& named(const LayerComps& comps, const Json* comp) {
    if (comps.comps.empty()) throw OpError("the page has no layer comps");
    if (comp == nullptr || comp->is_null()) {
        if (comps.last_applied) {
            for (const LayerComp& c : comps.comps)
                if (c.id == *comps.last_applied) return c;
        }
        return comps.comps.front();
    }
    if (comp->is_number_integer() || comp->is_number_unsigned()) {
        const std::int64_t id = comp->get<std::int64_t>();
        for (const LayerComp& c : comps.comps)
            if (c.id == id) return c;
        throw OpError("no layer comp with id " + std::to_string(id));
    }
    if (comp->is_string()) {
        const std::string& name = comp->get_ref<const std::string&>();
        for (const LayerComp& c : comps.comps)
            if (c.name == name) return c;
        throw OpError("no layer comp named " + name);
    }
    throw OpError("comp is a layer comp's id or name");
}

std::string file_part(const std::string& name) {
    std::string out;
    for (const char ch : name) {
        const bool bad = std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos || static_cast<unsigned char>(ch) < 0x20;
        out += bad ? '_' : ch;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("_") : out;
}

Json listing(const core::Page& page) {
    Json list = Json::array();
    const LayerComps none;
    const LayerComps& comps = page.layer_comps ? *page.layer_comps : none;
    for (const LayerComp& c : comps.comps) {
        Json gone = Json::array();
        for (const std::string& id : missing(page, c.layers)) gone.push_back(id);
        list.push_back(Json{{"id", c.id},
                            {"name", c.name},
                            {"comment", c.comment},
                            {"visibility", c.apply_visibility},
                            {"position", c.apply_position},
                            {"appearance", c.apply_appearance},
                            {"layers", c.layers.size()},
                            {"missing", std::move(gone)}});
    }
    return Json{{"comps", std::move(list)},
                {"last_applied", comps.last_applied ? Json(*comps.last_applied) : Json(nullptr)},
                {"last_state", comps.last_state.has_value()}};
}

}  // namespace genko::render::layer_comps

namespace genko::render {

namespace {

using core::Json;
using core::LayerComp;
using core::LayerComps;
using core::OpError;
namespace lc = layer_comps;

std::size_t characters(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

LayerComps comps_of(const core::Page& page) { return page.layer_comps ? *page.layer_comps : LayerComps{}; }

void put(core::Document& doc, std::size_t at, LayerComps comps) {
    core::check_layer_comps(comps);
    core::Page& page = doc.edit_page(at);
    if (comps.empty()) {
        page.layer_comps.reset();
    } else {
        page.layer_comps = std::move(comps);
    }
}

std::size_t index_of(const LayerComps& comps, std::int64_t id) {
    for (std::size_t i = 0; i < comps.comps.size(); ++i)
        if (comps.comps[i].id == id) return i;
    return 0;
}

std::int64_t next_id(const LayerComps& comps) {
    std::int64_t id = 0;
    for (const LayerComp& c : comps.comps) id = std::max(id, c.id);
    if (id < core::kLayerCompIdMax) return id + 1;
    std::set<std::int64_t> taken;
    for (const LayerComp& c : comps.comps) taken.insert(c.id);
    for (std::int64_t i = 1;; ++i)
        if (!taken.contains(i)) return i;
}

// A flag the op may give (true or false; none: `fallback`).
std::optional<bool> flag(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return std::nullopt;
    if (!v->is_boolean()) throw OpError(std::string(key) + " is true or false");
    return v->get<bool>();
}

std::string name_given(const Json& value) {
    if (!value.is_string()) throw OpError("name is the layer comp's name");
    std::string name = value.get<std::string>();
    const std::size_t n = characters(name);
    if (n < 1 || n > core::kLayerCompNameMax) throw OpError("name is 1 to 255 characters");
    return name;
}

std::string comment_given(const Json& value) {
    if (!value.is_string()) throw OpError("comment is text");
    std::string text = value.get<std::string>();
    if (text.size() > core::kLayerCompTextMax) throw OpError("comment is at most 65536 bytes");
    return text;
}

void room_for_one(const LayerComps& comps) {
    if (comps.comps.size() >= core::kLayerCompsMax) throw OpError("a page holds at most 256 layer comps");
}

void new_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    room_for_one(comps);
    LayerComp comp;
    comp.id = next_id(comps);
    if (const Json* name = core::get(c.op, "name"); name != nullptr && !name->is_null()) {
        comp.name = name_given(*name);
    } else {
        for (std::size_t n = 1;; ++n) {  // (レイヤーカンプ 1, 2 …: the first free)
            const std::string candidate = "レイヤーカンプ " + std::to_string(n);
            if (std::none_of(comps.comps.begin(), comps.comps.end(), [&](const LayerComp& k) { return k.name == candidate; })) {
                comp.name = candidate;
                break;
            }
        }
    }
    if (const Json* text = core::get(c.op, "comment"); text != nullptr && !text->is_null()) comp.comment = comment_given(*text);
    comp.apply_visibility = flag(c.op, "visibility").value_or(true);
    comp.apply_position = flag(c.op, "position").value_or(true);
    comp.apply_appearance = flag(c.op, "appearance").value_or(true);
    comp.layers = lc::capture(c.doc, c.doc.page(at));
    const std::int64_t id = comp.id;
    comps.comps.push_back(std::move(comp));
    comps.last_applied = id;  // (the new comp is the page's state: PhotoCraft's and Photoshop's)
    put(c.doc, at, std::move(comps));
    c.report = Json{{"comp", id}};
}

void update_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    std::string what = "all";
    if (const Json* w = core::get(c.op, "what"); w != nullptr && !w->is_null()) {
        if (!w->is_string()) throw OpError("what is all, visibility, position or appearance");
        what = w->get<std::string>();
    }
    if (what != "all" && what != "visibility" && what != "position" && what != "appearance") {
        throw OpError("what is all, visibility, position or appearance");
    }
    const Json* given = core::get(c.op, "comp");
    std::vector<std::int64_t> ids;
    if (given != nullptr && given->is_string() && given->get<std::string>() == "*") {
        if (comps.comps.empty()) throw OpError("the page has no layer comps");
        for (const LayerComp& k : comps.comps) ids.push_back(k.id);
    } else {
        ids.push_back(lc::named(comps, given).id);
    }
    const std::vector<core::CompLayerState> now = lc::capture(c.doc, c.doc.page(at));
    Json updated = Json::array();
    for (const std::int64_t id : ids) {
        LayerComp& comp = comps.comps[index_of(comps, id)];
        if (what == "all") {
            comp.layers = now;
        } else {
            // (one property: taken again on every layer there is now, the others as they were recorded)
            std::vector<core::CompLayerState> states;
            for (const core::CompLayerState& n : now) {
                core::CompLayerState s;
                s.layer = n.layer;
                for (const core::CompLayerState& old : comp.layers)
                    if (old.layer == n.layer) s = old;
                if (what == "visibility") {
                    s.visible = n.visible;
                } else if (what == "position") {
                    s.has_position = true;
                    s.position = n.position;
                } else {
                    s.appearance = n.appearance;
                }
                states.push_back(std::move(s));
            }
            comp.layers = std::move(states);
        }
        updated.push_back(id);
    }
    put(c.doc, at, std::move(comps));
    c.report = Json{{"updated", std::move(updated)}};
}

void apply_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    const Json* given = core::get(c.op, "comp");
    const std::string word = given != nullptr && given->is_string() ? given->get<std::string>() : std::string();
    if (word == "last_state") {  // (最後のドキュメントの状態: all of it put back)
        if (!comps.last_state) throw OpError("no layer comp has been applied over the page's own state");
        const std::vector<core::CompLayerState> last = *comps.last_state;
        lc::apply(c.doc, at, last, true, true, true);
        comps.last_applied.reset();
        put(c.doc, at, std::move(comps));
        Json gone = Json::array();
        for (const std::string& id : lc::missing(c.doc.page(at), last)) gone.push_back(id);
        c.report = Json{{"comp", nullptr}, {"missing", std::move(gone)}};
        return;
    }
    if (comps.comps.empty()) throw OpError("the page has no layer comps");
    std::int64_t id = 0;
    if (word == "next" || word == "previous") {  // (in the list's order, round)
        const auto n = static_cast<std::int64_t>(comps.comps.size());
        std::optional<std::int64_t> now;
        if (comps.last_applied) {
            for (std::int64_t i = 0; i < n; ++i)
                if (comps.comps[static_cast<std::size_t>(i)].id == *comps.last_applied) now = i;
        }
        const std::int64_t step = word == "next" ? 1 : -1;
        const std::int64_t i = now ? ((*now + step) % n + n) % n : (step > 0 ? 0 : n - 1);
        id = comps.comps[static_cast<std::size_t>(i)].id;
    } else {
        id = lc::named(comps, given).id;
    }
    // (over the page's own state — no comp applied since —, that state first kept as 最後のドキュメントの状態)
    const bool own_state = !comps.last_applied ||
                           std::none_of(comps.comps.begin(), comps.comps.end(), [&](const LayerComp& k) { return k.id == *comps.last_applied; });
    if (own_state) comps.last_state = lc::capture(c.doc, c.doc.page(at));
    const LayerComp comp = comps.comps[index_of(comps, id)];
    lc::apply(c.doc, at, comp.layers, comp.apply_visibility, comp.apply_position, comp.apply_appearance);
    comps.last_applied = id;
    put(c.doc, at, std::move(comps));
    Json gone = Json::array();
    for (const std::string& layer : lc::missing(c.doc.page(at), comp.layers)) gone.push_back(layer);
    c.report = Json{{"comp", id}, {"missing", std::move(gone)}};
}

void set_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    LayerComp& comp = comps.comps[index_of(comps, lc::named(comps, core::get(c.op, "comp")).id)];
    const Json* name = core::get(c.op, "name");
    const Json* comment = core::get(c.op, "comment");
    const auto visibility = flag(c.op, "visibility"), position = flag(c.op, "position"), appearance = flag(c.op, "appearance");
    const bool named = name != nullptr && !name->is_null(), commented = comment != nullptr && !comment->is_null();
    if (!named && !commented && !visibility && !position && !appearance) {
        throw OpError("give name, comment, visibility, position or appearance");
    }
    if (named) comp.name = name_given(*name);
    if (commented) comp.comment = comment_given(*comment);
    if (visibility) comp.apply_visibility = *visibility;
    if (position) comp.apply_position = *position;
    if (appearance) comp.apply_appearance = *appearance;
    const std::int64_t id = comp.id;
    put(c.doc, at, std::move(comps));
    c.report = Json{{"comp", id}};
}

void duplicate_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    const std::size_t i = index_of(comps, lc::named(comps, core::get(c.op, "comp")).id);
    room_for_one(comps);
    LayerComp copy = comps.comps[i];
    copy.id = next_id(comps);
    // (Photoshop's 「… のコピー」, the name cut by whole characters to keep within 255)
    const std::string tail = " のコピー";
    std::string base = copy.name;
    while (characters(base) + characters(tail) > core::kLayerCompNameMax && !base.empty()) {
        while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80) base.pop_back();
        if (!base.empty()) base.pop_back();
    }
    copy.name = base + tail;
    const std::int64_t id = copy.id;
    comps.comps.insert(comps.comps.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(copy));
    put(c.doc, at, std::move(comps));
    c.report = Json{{"comp", id}};
}

void delete_layer_comp(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    const std::int64_t id = lc::named(comps, core::get(c.op, "comp")).id;
    comps.comps.erase(comps.comps.begin() + static_cast<std::ptrdiff_t>(index_of(comps, id)));
    if (comps.last_applied == id) comps.last_applied.reset();
    // (the last comp gone: its last document state with it — Photoshop shows none without comps —, the page's comps none:
    // layer-comps@1 not named any more)
    if (comps.comps.empty()) comps.last_state.reset();
    put(c.doc, at, std::move(comps));
    c.report = Json{{"deleted", id}};
}

void clear_layer_comp_warnings(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    LayerComps comps = comps_of(c.doc.page(at));
    const core::Page& page = c.doc.page(at);
    Json cleared = Json::array();
    for (LayerComp& comp : comps.comps) {
        const std::vector<std::string> gone = lc::missing(page, comp.layers);
        if (gone.empty()) continue;
        std::erase_if(comp.layers, [&](const core::CompLayerState& s) { return std::find(gone.begin(), gone.end(), s.layer) != gone.end(); });
        cleared.push_back(Json{{"comp", comp.id}, {"missing", gone}});
    }
    if (cleared.empty()) throw OpError("no layer comp of the page names a layer it does not have");
    put(c.doc, at, std::move(comps));
    c.report = Json{{"cleared", std::move(cleared)}};
}

}  // namespace

void register_layer_comp_ops(core::OpRegistry& registry) {
    registry.add("new_layer_comp", new_layer_comp);
    registry.add("update_layer_comp", update_layer_comp);
    registry.add("apply_layer_comp", apply_layer_comp);
    registry.add("set_layer_comp", set_layer_comp);
    registry.add("duplicate_layer_comp", duplicate_layer_comp);
    registry.add("delete_layer_comp", delete_layer_comp);
    registry.add("clear_layer_comp_warnings", clear_layer_comp_warnings);
}

}  // namespace genko::render
