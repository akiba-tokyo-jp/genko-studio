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
#include "core/pyops.hpp"
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

// What reading the comps left out, to be said once: layer effects not read (no style reading given), a style not read,
// the effects of a folder's or a correction layer's state.
struct LeftOut {
    bool effects = false, unread = false, folded = false;
};

// A comp's states.
std::vector<core::CompLayerState> states_of(const Json& comp, const pcraft_production::ReadContext& ctx, LeftOut& left) {
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
            // (PC6, layer-styles@1: its effects and fill opacity its style, as a layer's — the layer's kind's: a folder's or a
            // correction layer's, which take none, their fill in their opacity —, its Blend If the layer's own, upstream's
            // comps holding none; a style not read: its fill in its opacity, said, the comp kept — the review's P2-2, P3-6)
            const core::Layer* layer = nullptr;
            if (ctx.page != nullptr) {
                for (const core::Layer& l : ctx.page->layers)
                    if (l.id == state.layer) layer = &l;
            }
            if (ctx.style && (layer == nullptr || core::takes_layer_style(*layer))) {
                look.opacity = std::round(opacity * 1000.0) / 1000.0;
                Json style;
                bool failed = false;
                try {
                    style = ctx.style(*a);
                } catch (const pcraft::Unreadable&) {
                    failed = true;
                } catch (const core::Error& error) {
                    if (error.code() != "value") throw;
                    failed = true;
                }
                if (failed) {
                    style = Json();
                    left.unread = true;
                    look.opacity = std::round(opacity * fill * 1000.0) / 1000.0;
                }
                const Json* own = layer != nullptr && layer->style ? core::get(*layer->style, "blend_if") : nullptr;
                if (own != nullptr && own->is_array() && !own->empty()) {
                    Json with = style.is_object() ? style : Json{{"effects", Json::array()}};
                    with["blend_if"] = *own;
                    style = core::layer_style_spec(with);
                }
                if (!style.is_null()) look.style = std::move(style);
            } else if (ctx.style) {
                look.opacity = std::round(opacity * fill * 1000.0) / 1000.0;
                if (const Json* fx = pcraft::optional_field(*a, "effects")) {
                    const Json* items = pcraft::optional_field(*fx, "items");
                    if (items != nullptr && items->is_array() && !items->empty()) left.folded = true;
                }
            } else {
                look.opacity = std::round(opacity * fill * 1000.0) / 1000.0;
                if (const Json* fx = pcraft::optional_field(*a, "effects")) {
                    const Json* items = pcraft::optional_field(*fx, "items");
                    if ((items != nullptr && items->is_array() && !items->empty()) || pcraft::optional_field(*fx, "psd_raw") != nullptr) left.effects = true;
                }
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
    LeftOut left;
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
            comp.layers = states_of(c, context, left);
            comps.comps.push_back(std::move(comp));
        }
        if (context.document != nullptr) {
            if (const Json* last = pcraft::optional_field(*context.document, "last_applied_comp")) {
                const auto id = static_cast<std::int64_t>(whole(*last));
                if (std::any_of(comps.comps.begin(), comps.comps.end(), [&](const core::LayerComp& c) { return c.id == id; })) comps.last_applied = id;
            }
            if (const Json* state = pcraft::optional_field(*context.document, "last_document_state")) {
                if (!state->is_object()) throw Bad("last_document_state");
                comps.last_state = states_of(*state, context, left);
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
    if (left.effects && context.say) context.say("レイヤーカンプのレイヤー効果は読んでいないため外しました");
    if (left.unread && context.say) context.say("レイヤーカンプのレイヤースタイルが読めないため効果を外し、塗りの不透明度は不透明度に掛けました");
    if (left.folded && context.say) context.say("Genko のグループ・補正レイヤーはレイヤー効果を持てないため、レイヤーカンプのその効果を外しました");
    context.page->layer_comps = std::move(comps);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (!page.layer_comps || page.layer_comps->comps.empty()) return std::nullopt;
    bool styled = false, blend_if = false;
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
                Json fx{{"enabled", true}, {"items", Json::array()}, {"psd_raw", nullptr}, {"reference", nullptr}};
                if (s.appearance->style) {  // (PC6, layer-styles@1: its effects upstream's; its Blend If upstream's comps have not)
                    if (context.effects) fx = context.effects(*s.appearance->style);
                    else if (core::style_shows_effects(*s.appearance->style)) styled = true;
                    // (one the layer has too — upstream keeps the layer's when it applies a comp — not said)
                    if (const Json* b = core::get(*s.appearance->style, "blend_if"); b != nullptr && b->is_array() && !b->empty()) {
                        const Json* own = nullptr;
                        for (const core::Layer& l : page.layers)
                            if (l.id == s.layer && l.style) own = core::get(*l.style, "blend_if");
                        if (own == nullptr || !core::same_json_unordered(*own, *b)) blend_if = true;
                    }
                }
                item["appearance"] = Json{{"blend", pcraft::upstream_blend(s.appearance->blend)},
                                          {"opacity", s.appearance->opacity},
                                          {"fill_opacity", fill},
                                          {"effects", std::move(fx)}};
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
    if (blend_if && context.say) context.say("レイヤーカンプのブレンド条件は PhotoCraft のレイヤーカンプに無いため書きませんでした");
    return list;
}

}  // namespace genko::render::pcraft_comps
