// layer-comps@1 (PC5 J-0): a page's layer comps, their JSON and checks (core/layer_comps.hpp). The shapes after
// PhotoCraft v0.2.0's doc/src/comps.rs (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, the
// positions in mm.

#include "core/layer_comps.hpp"

#include <algorithm>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace pj = production;

namespace {

std::size_t characters(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

Json states_json(const std::vector<CompLayerState>& states) {
    Json out = Json::array();
    for (const CompLayerState& s : states) {
        Json item = Json::object();
        item["layer"] = s.layer;
        if (s.visible) item["visible"] = *s.visible;
        if (s.has_position) item["position"] = s.position ? pj::to_json(*s.position) : Json(nullptr);
        if (s.appearance) {
            item["appearance"] = Json{{"blend", s.appearance->blend}, {"opacity", s.appearance->opacity},
                                      {"style", s.appearance->style ? *s.appearance->style : Json(nullptr)}};
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<CompLayerState> states_from(const Json& value, const std::string& where) {
    if (!value.is_array()) pj::wrong(where, "must be a list");
    std::vector<CompLayerState> out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string at = where + "[" + std::to_string(i) + "]";
        const Json& item = value[i];
        pj::only_keys(item, at, {"layer", "visible", "position", "appearance"});
        CompLayerState s;
        s.layer = pj::text(pj::field(item, "layer", at), at + ".layer", 256);
        if (s.layer.empty()) pj::wrong(at + ".layer", "must name a layer");
        if (const Json* v = pj::optional(item, "visible")) s.visible = pj::boolean(*v, at + ".visible");
        if (const Json* p = pj::optional(item, "position")) {
            s.has_position = true;
            if (!p->is_null()) s.position = pj::point_mm(*p, at + ".position");
        }
        if (const Json* a = pj::optional(item, "appearance")) {
            const std::string here = at + ".appearance";
            pj::only_keys(*a, here, {"blend", "opacity", "style"});
            CompAppearance look;
            look.blend = pj::text(pj::field(*a, "blend", here), here + ".blend", 64);
            look.opacity = pj::number(pj::field(*a, "opacity", here), here + ".opacity", 0, 1);
            if (const Json* style = pj::optional(*a, "style"); style != nullptr && !style->is_null()) {
                if (!style->is_object()) pj::wrong(here + ".style", "must be a layer style or null");
                look.style = *style;
            }
            s.appearance = std::move(look);
        }
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

Json to_json(const LayerComps& comps) {
    Json list = Json::array();
    for (const LayerComp& c : comps.comps) {
        Json item = Json::object();
        item["id"] = c.id;
        item["name"] = c.name;
        item["comment"] = c.comment;
        item["apply"] = Json{{"visibility", c.apply_visibility}, {"position", c.apply_position}, {"appearance", c.apply_appearance}};
        item["layers"] = states_json(c.layers);
        list.push_back(std::move(item));
    }
    Json out = Json::object();
    out["comps"] = std::move(list);
    out["last_applied"] = comps.last_applied ? Json(*comps.last_applied) : Json(nullptr);
    out["last_state"] = comps.last_state ? Json{{"layers", states_json(*comps.last_state)}} : Json(nullptr);
    return out;
}

LayerComps layer_comps_from_json(const Json& value) {
    const std::string where = "layer_comps";
    pj::only_keys(value, where, {"comps", "last_applied", "last_state"});
    LayerComps out;
    const Json& list = pj::field(value, "comps", where);
    if (!list.is_array()) pj::wrong(where + ".comps", "must be a list");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = where + ".comps[" + std::to_string(i) + "]";
        const Json& item = list[i];
        pj::only_keys(item, at, {"id", "name", "comment", "apply", "layers"});
        LayerComp c;
        c.id = pj::integer(pj::field(item, "id", at), at + ".id", 1, kLayerCompIdMax);
        c.name = pj::text(pj::field(item, "name", at), at + ".name", kLayerCompNameMax * 4);
        c.comment = pj::text(pj::field(item, "comment", at), at + ".comment", kLayerCompTextMax);
        const Json& apply = pj::field(item, "apply", at);
        pj::only_keys(apply, at + ".apply", {"visibility", "position", "appearance"});
        c.apply_visibility = pj::boolean(pj::field(apply, "visibility", at + ".apply"), at + ".apply.visibility");
        c.apply_position = pj::boolean(pj::field(apply, "position", at + ".apply"), at + ".apply.position");
        c.apply_appearance = pj::boolean(pj::field(apply, "appearance", at + ".apply"), at + ".apply.appearance");
        c.layers = states_from(pj::field(item, "layers", at), at + ".layers");
        out.comps.push_back(std::move(c));
    }
    if (const Json* last = pj::optional(value, "last_applied"); last != nullptr && !last->is_null()) {
        out.last_applied = pj::integer(*last, where + ".last_applied", 1, kLayerCompIdMax);
    }
    if (const Json* state = pj::optional(value, "last_state"); state != nullptr && !state->is_null()) {
        pj::only_keys(*state, where + ".last_state", {"layers"});
        out.last_state = states_from(pj::field(*state, "layers", where + ".last_state"), where + ".last_state.layers");
    }
    check_layer_comps(out);
    return out;
}

void check_layer_comps(const LayerComps& comps) {
    const std::string where = "layer_comps";
    if (comps.comps.size() > kLayerCompsMax) pj::wrong(where, "holds more than " + std::to_string(kLayerCompsMax) + " comps");
    std::set<std::int64_t> ids;
    for (const LayerComp& c : comps.comps) {
        if (c.id < 1 || c.id > kLayerCompIdMax) pj::wrong(where, "has a comp id out of 1..2147483647");
        if (!ids.insert(c.id).second) pj::wrong(where, "has two comps of the id " + std::to_string(c.id));
        const std::size_t n = characters(c.name);
        if (n < 1 || n > kLayerCompNameMax) pj::wrong(where, "has a comp name not of 1.." + std::to_string(kLayerCompNameMax) + " characters");
    }
    if (comps.last_applied && !ids.contains(*comps.last_applied)) {
        pj::wrong(where + ".last_applied", "names no comp of the page (" + std::to_string(*comps.last_applied) + ")");
    }
}

bool uses_layer_comps(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->layer_comps && !written.page->layer_comps->empty()) return true;
    }
    return false;
}

void validate_layer_comps(const Document& doc) {
    const auto check = [](const Page& page) {
        if (page.layer_comps) check_layer_comps(*page.layer_comps);
    };
    for (const PagePtr& page : doc.pages)
        if (page) check(*page);
    for (const smart::WrittenPage& written : smart::written_pages(doc))
        if (!written.contents.empty()) check(*written.page);
}

}  // namespace genko::core
