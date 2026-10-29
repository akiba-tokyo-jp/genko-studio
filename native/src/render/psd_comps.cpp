// PC5 J-a (render/psd_comps.hpp): a PSD's layer comps as the page's — PhotoCraft v0.2.0's crates/io/src/comps_map.rs (MIT,
// native/third_party/photocraft/NOTICE.md), its places in mm.

#include "render/psd_comps.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/error.hpp"
#include "core/layer_comps.hpp"
#include "core/pynum.hpp"
#include "render/ops_layer_comps.hpp"
#include "render/psd_descriptor.hpp"

namespace genko::render::psd_comps {

using core::Json;
namespace d = psd::descriptor;

namespace {

const core::Layer* layer_named(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers)
        if (layer.id == id) return &layer;
    return nullptr;
}

std::optional<std::int64_t> integer(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    if (v == nullptr) return std::nullopt;
    const auto n = v->numeric();
    if (!n || !std::isfinite(*n)) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(*n));
}

std::optional<std::string> text(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    if (v == nullptr || v->type != "TEXT") return std::nullopt;
    return psd::utf8_of(v->text);
}

const d::Object* object(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->object && (v->type == "Objc" || v->type == "GlbO") ? v->object.get() : nullptr;
}

// A layer's kept block of the key, as the book being read holds it (none: it keeps none).
std::optional<std::string> kept_block(const core::Document* doc, const core::Layer& layer, std::string_view key) {
    if (doc == nullptr || !layer.native_psd) return std::nullopt;
    for (const auto& [k, ref] : layer.native_psd->blocks) {
        if (k != key) continue;
        const auto held = doc->native_files.find(ref);
        if (held != doc->native_files.end() && held->second) return *held->second;
        return std::nullopt;
    }
    return std::nullopt;
}

// One layerSettings entry of a cmls descriptor.
struct Setting {
    std::vector<std::int64_t> comps;
    std::optional<bool> visible;
    std::optional<std::array<std::int64_t, 2>> offset;
};

std::optional<std::vector<Setting>> settings_of(std::string_view cmls) {
    d::Object top;
    try {
        top = d::read_versioned(cmls);
    } catch (const d::Broken&) {
        return std::nullopt;
    }
    const d::Value* list = top.get("layerSettings");
    if (list == nullptr || list->type != "VlLs") return std::nullopt;
    std::vector<Setting> out;
    for (const d::Value& item : list->list) {
        if (!item.object) continue;
        const d::Object& s = *item.object;
        Setting setting;
        if (const d::Value* ids = s.get("compList"); ids != nullptr && ids->type == "VlLs") {
            for (const d::Value& id : ids->list) {
                if (const auto n = id.numeric()) setting.comps.push_back(static_cast<std::int64_t>(std::llround(*n)));
            }
        }
        if (const d::Value* on = s.get("enab"); on != nullptr && on->type == "bool") setting.visible = on->boolean;
        if (const d::Object* o = object(s, "Ofst")) {
            const auto x = integer(*o, "Hrzn"), y = integer(*o, "Vrtc");
            if (x && y) setting.offset = std::array<std::int64_t, 2>{*x, *y};
        }
        out.push_back(std::move(setting));
    }
    return out;
}

// The file's pixels a mm, across and down (the page's canvas as the file's).
std::array<double, 2> px_a_mm(const core::Page& page, double width_px, double height_px) {
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    return {w > 0 && width_px > 0 ? width_px / w : 1.0, h > 0 && height_px > 0 ? height_px / h : 1.0};
}

}  // namespace

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context) {
    // (its states name the layers read: none to name — the merged picture alone —, or the page's own comps or an earlier
    // PSD's blocks there: kept as it was)
    if (context.merged_only || context.page_had || (page.layer_comps && !page.layer_comps->empty())) return false;
    d::Object top;
    try {
        top = d::read_versioned(data);
    } catch (const d::Broken&) {
        return false;
    }
    core::LayerComps comps;
    if (const d::Value* list = top.get("list"); list != nullptr && list->type == "VlLs") {
        for (const d::Value& item : list->list) {
            if (!item.object) continue;
            const d::Object& c = *item.object;
            const auto id = integer(c, "compID");
            if (!id) continue;
            core::LayerComp comp;
            comp.id = *id;
            comp.name = text(c, "Nm  ").value_or(std::string());
            comp.comment = text(c, "comment").value_or(std::string());
            const std::int64_t captured = integer(c, "capturedInfo").value_or(0);
            comp.apply_visibility = (captured & 1) != 0;
            comp.apply_position = (captured & 2) != 0;
            comp.apply_appearance = (captured & 4) != 0;
            comps.comps.push_back(std::move(comp));
        }
    }
    if (const auto last = integer(top, "lastAppliedComp")) {
        if (std::any_of(comps.comps.begin(), comps.comps.end(), [&](const core::LayerComp& c) { return c.id == *last; })) comps.last_applied = *last;
    }
    // each layer's states (its cmls): its place now and the offset
    const auto scale = px_a_mm(page, context.width_px, context.height_px);
    std::vector<core::CompLayerState> last_state;
    for (const auto& [lyid, id] : context.layers) {
        const core::Layer* layer = layer_named(page, id);
        if (layer == nullptr) continue;
        const auto shmd = kept_block(context.doc, *layer, "shmd");
        if (!shmd) continue;
        const auto items = psd_production::shmd_items(*shmd);
        if (!items) continue;
        const auto cmls = std::find_if(items->begin(), items->end(), [](const psd_production::MetaItem& m) { return m.key == "cmls"; });
        if (cmls == items->end()) continue;
        const auto settings = settings_of(cmls->data);
        if (!settings) continue;
        std::optional<std::array<double, 2>> here;
        bool asked = false;
        for (const Setting& s : *settings) {
            if (!s.visible && !s.offset) continue;
            core::CompLayerState state;
            state.layer = id;
            state.visible = s.visible;
            if (s.offset) {
                if (!asked && context.doc != nullptr) here = layer_comps::position_of(*context.doc, page, *layer);
                asked = true;
                state.has_position = true;
                if (here) {
                    state.position = std::array<double, 2>{core::py_round((*here)[0] + static_cast<double>((*s.offset)[0]) / scale[0], 4),
                                                           core::py_round((*here)[1] + static_cast<double>((*s.offset)[1]) / scale[1], 4)};
                }
            }
            for (const std::int64_t comp : s.comps) {
                if (comp == 0) {
                    last_state.push_back(state);
                    continue;
                }
                for (core::LayerComp& c : comps.comps)
                    if (c.id == comp) c.layers.push_back(state);
            }
        }
    }
    if (!last_state.empty()) comps.last_state = std::move(last_state);
    // (the page's states in its layers' order, as a comp made here holds them)
    const auto order = [&](std::vector<core::CompLayerState>& states) {
        std::stable_sort(states.begin(), states.end(), [&](const core::CompLayerState& a, const core::CompLayerState& b) {
            const auto at = [&](const std::string& id) {
                for (std::size_t i = 0; i < page.layers.size(); ++i)
                    if (page.layers[i].id == id) return i;
                return page.layers.size();
            };
            return at(a.layer) < at(b.layer);
        });
    };
    for (core::LayerComp& c : comps.comps) order(c.layers);
    if (comps.last_state) order(*comps.last_state);
    if (comps.empty()) return false;
    try {
        core::check_layer_comps(comps);  // (ids out of range or twice, a name empty or too long: kept as it was)
    } catch (const core::Error&) {
        return false;
    }
    if (context.report != nullptr) (*context.report)["layer_comps"] = comps.comps.size();
    page.layer_comps = std::move(comps);
    return true;
}

std::string fingerprint(const core::Page& page, const core::Document* doc) {
    if (!page.layer_comps || page.layer_comps->empty()) return {};
    Json at = Json::object();  // (the places of the layers its states place: their offsets in the file are from them)
    if (doc != nullptr) {
        std::set<std::string> placed;
        const auto note = [&](const std::vector<core::CompLayerState>& states) {
            for (const core::CompLayerState& s : states)
                if (s.has_position) placed.insert(s.layer);
        };
        for (const core::LayerComp& c : page.layer_comps->comps) note(c.layers);
        if (page.layer_comps->last_state) note(*page.layer_comps->last_state);
        for (const std::string& id : placed) {
            const core::Layer* layer = layer_named(page, id);
            if (layer == nullptr) continue;
            const auto p = layer_comps::position_of(*doc, page, *layer);
            at[id] = p ? Json::array({(*p)[0], (*p)[1]}) : Json(nullptr);
        }
    }
    return psd_production::fingerprint_of(Json{{"comps", core::to_json(*page.layer_comps)}, {"at", std::move(at)}});
}

std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context) {
    (void)context;
    if (!page.layer_comps || page.layer_comps->comps.empty()) return std::nullopt;
    std::vector<std::pair<std::string, d::Writer>> list;
    for (const core::LayerComp& c : page.layer_comps->comps) {
        d::Writer comp;
        comp.text("Nm  ", psd::utf16(c.name));
        if (!c.comment.empty()) comp.text("comment", psd::utf16(c.comment));
        comp.integer("compID", static_cast<std::int32_t>(c.id));
        comp.integer("capturedInfo", (c.apply_visibility ? 1 : 0) | (c.apply_position ? 2 : 0) | (c.apply_appearance ? 4 : 0));
        list.emplace_back("Comp", std::move(comp));
    }
    d::Writer top;
    top.objects("list", list);
    if (page.layer_comps->last_applied) top.integer("lastAppliedComp", static_cast<std::int32_t>(*page.layer_comps->last_applied));
    return top.versioned("CompList");
}

void write_layers(const core::Page& page, const psd_production::WriteContext& context, psd_production::LayerItems& out) {
    if (!page.layer_comps || page.layer_comps->comps.empty()) return;
    const core::LayerComps& comps = *page.layer_comps;
    const auto scale = px_a_mm(page, context.width_px, context.height_px);
    for (const core::Layer& layer : page.layers) {
        const auto lyid = context.lyids.find(layer.id);
        if (lyid == context.lyids.end()) continue;
        std::optional<std::array<double, 2>> here;
        if (context.doc != nullptr) here = layer_comps::position_of(*context.doc, page, layer);
        const auto setting = [&](std::int32_t id, const core::CompLayerState* state) {
            d::Writer s;
            if (state != nullptr && state->visible) s.boolean("enab", *state->visible);
            if (state != nullptr && state->position && here) {
                d::Writer offset;
                offset.integer("Hrzn", static_cast<std::int32_t>(std::llround(((*state->position)[0] - (*here)[0]) * scale[0])));
                offset.integer("Vrtc", static_cast<std::int32_t>(std::llround(((*state->position)[1] - (*here)[1]) * scale[1])));
                s.object("Ofst", offset, "null");
            }
            s.integers("compList", {id});
            return s;
        };
        const auto state_in = [&](const std::vector<core::CompLayerState>& states) -> const core::CompLayerState* {
            for (const core::CompLayerState& s : states)
                if (s.layer == layer.id) return &s;
            return nullptr;
        };
        std::vector<std::pair<std::string, d::Writer>> settings;
        for (const core::LayerComp& c : comps.comps) settings.emplace_back("null", setting(static_cast<std::int32_t>(c.id), state_in(c.layers)));
        settings.emplace_back("null", setting(0, comps.last_state ? state_in(*comps.last_state) : nullptr));
        d::Writer origin;
        origin.number("Hrzn", 0.0);
        origin.number("Vrtc", 0.0);
        d::Writer cmls;
        cmls.object("origFXRefPoint", origin, "null");
        cmls.integer("LyrI", static_cast<std::int32_t>(lyid->second));
        cmls.objects("layerSettings", settings);
        out.items[layer.id].emplace_back("cmls", cmls.versioned("null"));
        out.lyids[layer.id] = lyid->second;
    }
}

}  // namespace genko::render::psd_comps
