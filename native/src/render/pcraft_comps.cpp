// PC5 J-a (render/pcraft_comps.hpp): a .pcraft's layer comps as the page's, and back — PhotoCraft v0.2.0's
// format/src/manifest.rs and convert.rs (comp_m, Loader::comp), MIT: native/third_party/photocraft/NOTICE.md.

#include "render/pcraft_comps.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/error.hpp"
#include "core/layer_comps.hpp"
#include "core/layer_styles.hpp"
#include "core/pynum.hpp"
#include "render/pcraft_manifest.hpp"

namespace genko::render::pcraft_comps {

using core::Json;

namespace {

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool flag(const Json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_boolean()) throw Bad(key);
    return it->get<bool>();
}

std::uint64_t whole(const Json& v) {
    if (v.is_number_unsigned()) return v.get<std::uint64_t>();
    if (v.is_number_integer() && v.get<std::int64_t>() >= 0) return static_cast<std::uint64_t>(v.get<std::int64_t>());
    throw Bad("a whole number");
}

double number(const Json& v) {
    if (!v.is_number() || !std::isfinite(v.get<double>())) throw Bad("a number");
    return v.get<double>();
}

std::string text(const Json& o, const char* key, bool must) {
    const auto it = o.find(key);
    if (it == o.end() || it->is_null()) {
        if (must) throw Bad(key);
        return {};
    }
    if (!it->is_string()) throw Bad(key);
    return it->get<std::string>();
}

// A comp's states (`effects`: one held layer effects, left out).
std::vector<core::CompLayerState> states_of(const Json& comp, const pcraft_production::ReadContext& ctx, bool& effects) {
    std::vector<core::CompLayerState> out;
    const Json* states = pcraft::optional_field(comp, "states");
    if (states == nullptr) return out;
    if (!states->is_array()) throw Bad("states");
    for (const Json& s : *states) {
        if (!s.is_object()) throw Bad("a state");
        core::CompLayerState state;
        const std::string raw = std::to_string(whole(s.value("layer", Json())));
        const auto made = ctx.layers.find(raw);
        state.layer = made != ctx.layers.end() ? made->second : "pcraft-" + raw;  // (a layer gone before it was saved)
        if (const Json* v = pcraft::optional_field(s, "visible")) {
            if (!v->is_boolean()) throw Bad("visible");
            state.visible = v->get<bool>();
        }
        state.has_position = true;  // (PhotoCraft records every layer's place: none for one without)
        if (const Json* p = pcraft::optional_field(s, "position")) {
            if (!p->is_array() || p->size() != 2) throw Bad("position");
            state.position = std::array<double, 2>{core::py_round(number((*p)[0]) * ctx.mm, 4), core::py_round(number((*p)[1]) * ctx.mm, 4)};
        }
        if (const Json* a = pcraft::optional_field(s, "appearance")) {
            if (!a->is_object()) throw Bad("appearance");
            core::CompAppearance look;
            const std::string blend = pcraft::blend_from(a->value("blend", Json()));
            look.blend = pcraft::genko_blend(blend).value_or("normal");
            const double opacity = std::clamp(number(a->value("opacity", Json())), 0.0, 1.0);
            const double fill = a->contains("fill_opacity") ? std::clamp(number((*a)["fill_opacity"]), 0.0, 1.0) : 1.0;
            look.opacity = std::round(opacity * fill * 1000.0) / 1000.0;  // (as a layer's: its effects left out)
            if (const Json* fx = pcraft::optional_field(*a, "effects")) {
                const Json* items = pcraft::optional_field(*fx, "items");
                if ((items != nullptr && items->is_array() && !items->empty()) || pcraft::optional_field(*fx, "psd_raw") != nullptr) effects = true;
            }
            state.appearance = std::move(look);
        }
        out.push_back(std::move(state));
    }
    return out;
}

}  // namespace

void read(const Json& value, pcraft_production::ReadContext& context) {
    if (context.page == nullptr) return;
    core::LayerComps comps;
    bool effects = false;
    try {
        if (!value.is_array()) throw Bad("layer_comps");
        for (const Json& c : value) {
            if (!c.is_object()) throw Bad("a comp");
            core::LayerComp comp;
            comp.id = static_cast<std::int64_t>(whole(c.value("id", Json())));
            comp.name = text(c, "name", true);
            comp.comment = text(c, "comment", false);
            comp.apply_visibility = flag(c, "apply_visibility");
            comp.apply_position = flag(c, "apply_position");
            comp.apply_appearance = flag(c, "apply_appearance");
            comp.layers = states_of(c, context, effects);
            comps.comps.push_back(std::move(comp));
        }
        if (context.document != nullptr) {
            if (const Json* last = pcraft::optional_field(*context.document, "last_applied_comp")) {
                const auto id = static_cast<std::int64_t>(whole(*last));
                if (std::any_of(comps.comps.begin(), comps.comps.end(), [&](const core::LayerComp& c) { return c.id == id; })) comps.last_applied = id;
            }
            if (const Json* state = pcraft::optional_field(*context.document, "last_document_state")) {
                if (!state->is_object()) throw Bad("last_document_state");
                comps.last_state = states_of(*state, context, effects);
            }
        }
        core::check_layer_comps(comps);
    } catch (const Bad&) {
        if (context.say) context.say("レイヤーカンプは読めない形のため外しました");
        return;
    } catch (const core::Error&) {
        if (context.say) context.say("レイヤーカンプは読めない形のため外しました");
        return;
    } catch (const pcraft::Unreadable&) {
        if (context.say) context.say("レイヤーカンプは読めない形のため外しました");
        return;
    }
    if (comps.empty()) return;
    if (effects && context.say) context.say("レイヤーカンプのレイヤー効果は Genko で描けないため外しました");
    context.page->layer_comps = std::move(comps);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (!page.layer_comps || page.layer_comps->comps.empty()) return std::nullopt;
    bool styled = false;
    const auto states = [&](const std::vector<core::CompLayerState>& list) {
        Json out = Json::array();
        for (const core::CompLayerState& s : list) {
            const auto id = context.layers.find(s.layer);
            if (id == context.layers.end()) continue;  // (a layer the document has not)
            Json item{{"layer", std::stoull(id->second)}, {"visible", s.visible ? Json(*s.visible) : Json(nullptr)}, {"position", nullptr}, {"appearance", nullptr}};
            if (s.position) {
                item["position"] = Json::array({static_cast<std::int64_t>(std::llround((*s.position)[0] * context.px)),
                                                static_cast<std::int64_t>(std::llround((*s.position)[1] * context.px))});
            }
            if (s.appearance) {
                const double fill = s.appearance->style ? core::style_fill_opacity(*s.appearance->style) : 1.0;
                if (s.appearance->style && core::style_shows_effects(*s.appearance->style)) styled = true;
                item["appearance"] = Json{{"blend", pcraft::upstream_blend(s.appearance->blend)},
                                          {"opacity", s.appearance->opacity},
                                          {"fill_opacity", fill},
                                          {"effects", Json{{"enabled", true}, {"items", Json::array()}, {"psd_raw", nullptr}, {"reference", nullptr}}}};
            }
            out.push_back(std::move(item));
        }
        return out;
    };
    const auto comp_json = [&](std::int64_t id, const std::string& name, const std::string& comment, bool v, bool p, bool a,
                               const std::vector<core::CompLayerState>& list) {
        return Json{{"id", id}, {"name", name}, {"comment", comment}, {"apply_visibility", v}, {"apply_position", p}, {"apply_appearance", a},
                    {"states", states(list)}};
    };
    Json list = Json::array();
    for (const core::LayerComp& c : page.layer_comps->comps) {
        list.push_back(comp_json(c.id, c.name, c.comment, c.apply_visibility, c.apply_position, c.apply_appearance, c.layers));
    }
    if (context.document != nullptr) {
        (*context.document)["last_applied_comp"] = page.layer_comps->last_applied ? Json(*page.layer_comps->last_applied) : Json(nullptr);
        (*context.document)["last_document_state"] =
            page.layer_comps->last_state ? comp_json(0, "Last Document State", "", true, true, true, *page.layer_comps->last_state) : Json(nullptr);
    }
    if (styled && context.say) context.say("レイヤーカンプのレイヤー効果は .pcraft に書かないため外しました");
    return list;
}

}  // namespace genko::render::pcraft_comps
