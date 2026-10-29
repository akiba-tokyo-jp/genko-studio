// PC4 E6 (layer-styles@1, render/ops_layer_styles.hpp): the ops of a layer's style — PhotoCraft v0.2.0's
// crates/engine/src/layer_style.rs (set_effect: a kind's effect replaced, or added with "add"), presets/styles.rs (apply:
// a style put on layers, "add" adding to their effects), layer_menu_cmds.rs (global_light, scale_effects, create_layer)
// and extra_cmds.rs (hide and show all effects), MIT: native/third_party/photocraft/NOTICE.md — in mm, on Genko's layers.

#include "render/ops_layer_styles.hpp"

#include <algorithm>
#include <set>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/layer_styles.hpp"
#include "core/ops_util.hpp"
#include "core/psd_keep.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/layer_styles.hpp"
#include "render/ops_patterns.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"

namespace genko::render {

using core::Json;
using core::OpError;

namespace {

// The layers an op names: "ids" (a list) or "id", in the page's order.
std::vector<std::size_t> named_layers(const core::Page& page, const Json& op) {
    std::vector<std::string> wanted;
    if (const Json* ids = core::get(op, "ids"); ids != nullptr && !ids->is_null()) {
        if (!ids->is_array() || ids->empty()) throw OpError("ids is a list of layer ids");
        for (const Json& id : *ids) {
            if (!id.is_string()) throw OpError("ids is a list of layer ids");
            wanted.push_back(id.get<std::string>());
        }
    } else {
        if (!core::truthy_at(op, "id") || !op["id"].is_string()) throw OpError("id is the layer's id");
        wanted.push_back(op["id"].get<std::string>());
    }
    std::vector<std::size_t> out;
    for (const std::string& id : wanted) {
        const std::size_t i = core::layer_by_id(page, id);
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void check_takes(const core::Layer& layer) {
    if (!core::takes_layer_style(layer)) {
        throw OpError("the layer " + layer.id + " takes no layer style (a folder, a correction layer or a tone layer)");
    }
}

// core::layer_style_spec with the op's words.
Json spec_of(const Json& style) {
    try {
        return core::layer_style_spec(style);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

// A layer's style set (none: none), and what it kept of a PSD's effects let go when it changed.
void put_style(core::Layer& layer, Json style) {
    const std::optional<Json> next = style.is_null() ? std::nullopt : std::optional<Json>(std::move(style));
    if (layer.style == next) return;
    layer.style = next;
    forget_psd_effects(layer);
}

// One pattern given (an id the book keeps, or a picture) as a pattern fill in its normal form, the picture put into the book.
Json pattern_in_book(core::Document& doc, const Json& given) {
    if (!given.is_object()) throw OpError("a style's pattern is {\"id\", …} or a picture as add_pattern takes it");
    return fill_pattern(doc, given);
}

const char* kind_label(const std::string& kind) {
    if (kind == "drop_shadow") return "ドロップシャドウ";
    if (kind == "inner_shadow") return "シャドウ（内側）";
    if (kind == "outer_glow") return "光彩（外側）";
    if (kind == "inner_glow") return "光彩（内側）";
    if (kind == "bevel") return "ベベルとエンボス";
    if (kind == "satin") return "サテン";
    if (kind == "color_overlay") return "カラーオーバーレイ";
    if (kind == "gradient_overlay") return "グラデーションオーバーレイ";
    if (kind == "pattern_overlay") return "パターンオーバーレイ";
    return "境界線";
}

void set_layer_style(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    if (!c.op.contains("style")) throw OpError("style is the layer style ({\"effects\", …}) or null");
    const std::vector<std::size_t> layers = named_layers(doc.page(at), c.op);
    for (const std::size_t i : layers) check_takes(doc.page(at).layers[i]);
    const bool add = core::truthy_at(c.op, "add");
    const bool keep_blending = core::truthy_at(c.op, "keep_blending");  // (the layer's fill opacity and Blend If kept)
    std::optional<std::string> blend;
    if (const Json* b = core::get(c.op, "blend"); b != nullptr && !b->is_null()) blend = core::blend_mode(*b);
    Json given = c.op["style"].is_null() ? Json(nullptr) : style_with_patterns(doc, c.op["style"]);
    if (!given.is_null() && !given.is_object()) throw OpError("style is the layer style ({\"effects\", …}) or null");
    core::Page& page = doc.edit_page(at);
    Json ids = Json::array();
    for (const std::size_t i : layers) {
        core::Layer& layer = page.layers[i];
        Json style = given;
        if (add && layer.style && !given.is_null()) {  // (its effects after the layer's own; what else is given replaces the layer's)
            style = *layer.style;
            Json effects = style["effects"];
            if (const Json* more = core::get(given, "effects"); more != nullptr && more->is_array())
                for (const Json& e : *more) effects.push_back(e);
            style["effects"] = std::move(effects);
            for (const char* key : {"enabled", "fill_opacity", "blend_if"})
                if (given.contains(key)) style[key] = given[key];
        }
        if (keep_blending && style.is_object()) {
            for (const char* key : {"fill_opacity", "blend_if"}) {
                style.erase(key);
                if (layer.style && layer.style->is_object() && layer.style->contains(key)) style[key] = (*layer.style)[key];
            }
        }
        put_style(layer, spec_of(style));
        if (blend) layer.blend = *blend;
        ids.push_back(layer.id);
    }
    c.report = Json{{"ids", std::move(ids)}};
}

void add_layer_effect(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* effect = core::get(c.op, "effect");
    if (effect == nullptr || !effect->is_object()) throw OpError("effect is {\"kind\", …} (bevel, stroke, inner_shadow, inner_glow, satin, color_overlay, gradient_overlay, pattern_overlay, outer_glow, drop_shadow)");
    const std::vector<std::size_t> layers = named_layers(doc.page(at), c.op);
    for (const std::size_t i : layers) check_takes(doc.page(at).layers[i]);
    const Json given = style_with_patterns(doc, Json{{"effects", Json::array({*effect})}})["effects"][0];
    const bool add = core::truthy_at(c.op, "add");
    core::Page& page = doc.edit_page(at);
    Json ids = Json::array();
    for (const std::size_t i : layers) {
        core::Layer& layer = page.layers[i];
        Json style = layer.style ? *layer.style : Json{{"effects", Json::array()}};
        Json& effects = style["effects"];
        const std::string kind = given.value("kind", std::string());
        auto same = std::find_if(effects.begin(), effects.end(), [&](const Json& e) { return e.value("kind", std::string()) == kind; });
        try {
            if (add || same == effects.end()) {
                effects.push_back(core::layer_effect_spec(given));
            } else {
                *same = core::layer_effect_spec(given, &*same);
            }
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
        style.erase("enabled");  // (a new effect shows the effects again, as Photoshop's)
        put_style(layer, spec_of(style));
        ids.push_back(layer.id);
    }
    c.report = Json{{"ids", std::move(ids)}};
}

void set_global_light(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::array<double, 2> now = core::global_light(doc);
    Json raw = Json::object();
    raw["angle"] = c.op.contains("angle") && !c.op["angle"].is_null() ? c.op["angle"] : Json(now[0]);
    raw["altitude"] = c.op.contains("altitude") && !c.op["altitude"].is_null() ? c.op["altitude"] : Json(now[1]);
    std::array<double, 2> light{};
    try {
        light = core::global_light_of(raw);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    if (light[0] == core::kDefaultLightAngle && light[1] == core::kDefaultLightAltitude) {
        doc.global_light.reset();  // (Photoshop's own light: not written)
    } else {
        doc.global_light = light;
    }
    c.report = Json{{"angle", light[0]}, {"altitude", light[1]}};
}

void scale_layer_style(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* scale = core::get(c.op, "scale");
    if (scale == nullptr || !scale->is_number() || !(scale->get<double>() >= 1 && scale->get<double>() <= 1000)) throw OpError("scale is 1..1000 (%)");
    const std::vector<std::size_t> layers = named_layers(doc.page(at), c.op);
    core::Page& page = doc.edit_page(at);
    Json ids = Json::array();
    for (const std::size_t i : layers) {
        core::Layer& layer = page.layers[i];
        if (!layer.style) continue;
        try {
            put_style(layer, core::scaled_layer_style(*layer.style, scale->get<double>() / 100.0));
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
        ids.push_back(layer.id);
    }
    if (ids.empty()) throw OpError("no layer named has a layer style");
    c.report = Json{{"ids", std::move(ids)}};
}

void show_layer_effects(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* show = core::get(c.op, "show");
    if (show == nullptr || !show->is_boolean()) throw OpError("show is true (すべての効果を表示) or false (すべての効果を隠す)");
    std::vector<std::size_t> layers;
    if (c.op.contains("ids") || c.op.contains("id")) {
        layers = named_layers(doc.page(at), c.op);
    } else {
        for (std::size_t i = 0; i < doc.page(at).layers.size(); ++i)
            if (doc.page(at).layers[i].style) layers.push_back(i);
    }
    core::Page& page = doc.edit_page(at);
    Json ids = Json::array();
    for (const std::size_t i : layers) {
        core::Layer& layer = page.layers[i];
        if (!layer.style) continue;
        Json style = *layer.style;
        if (show->get<bool>()) style.erase("enabled");
        else style["enabled"] = false;
        put_style(layer, spec_of(style));
        ids.push_back(layer.id);
    }
    c.report = Json{{"ids", std::move(ids)}};
}

// レイヤーを作成: each effect shown drawn alone (in the normal mode at full opacity, over nothing, the layer's own pixels
// not) into a paint layer of its own that carries the effect's mode and opacity — below the layer (not clipped) for what
// is drawn beneath it, above it and clipped to it for the rest —; the layer keeps its pixels, fill and Blend If and loses
// its effects.
void layer_style_to_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = core::layer_by_id(doc.page(at), core::truthy_at(c.op, "id") ? core::py_str(c.op["id"]) : std::string());
    const core::Layer source = doc.page(at).layers[i];
    if (source.locked) throw OpError("the layer is locked");
    if (!source.style || !core::style_shows_effects(*source.style)) throw OpError("the layer " + source.id + " has no layer effects shown");
    const core::Page& page = doc.page(at);
    const int dpi = raster::working_dpi(page);
    std::vector<core::Layer> below, above;
    for (const Json& e : (*source.style)["effects"]) {
        if (!e.value("enabled", true)) continue;
        core::Layer alone = source;
        alone.visible = true;
        alone.opacity = 1.0;
        alone.blend = "normal";
        alone.clip = false;
        alone.locked = false;
        alone.mask.reset();
        alone.vector_mask.reset();
        alone.native_psd.reset();
        Json one = e;
        if (one.contains("blend")) one["blend"] = "normal";
        if (one.contains("opacity")) one["opacity"] = 1;
        alone.style = spec_of(Json{{"effects", Json::array({one})}, {"fill_opacity", 0}});
        const Alone drawn = composite_alone(page, {alone}, dpi, &doc);
        core::Layer made;
        made.id = core::new_id();
        made.role = core::LayerRole::User;
        made.kind = core::LayerKind::Raster;
        made.title = "「" + (source.title.empty() ? source.id : source.title) + "」の" + kind_label(e.value("kind", std::string()));
        made.blend = e.contains("blend") ? e["blend"].get<std::string>() : std::string("normal");
        made.opacity = e.contains("opacity") ? e["opacity"].get<double>() : 1.0;
        made.parent_id = source.parent_id;
        made.visible = source.visible;
        made.exportable = source.exportable;
        made.panel_clip = false;  // (cut at the panels as the layer's shape is: drawn so)
        made.panel_each = false;
        made.clip = !layer_styles::drawn_below(e);
        if (drawn.inks) {
            made.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*drawn.inks));
        } else if (drawn.precise) {
            made.color_raster = std::make_shared<const std::string>(*drawn.precise);
            if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end())
                doc.features.emplace_back(core::kColorRasterFeature);
        } else {
            raster::save_raster(page, made, drawn.image);
        }
        (made.clip ? above : below).push_back(std::move(made));
    }
    core::Page& out = doc.edit_page(at);
    {
        core::Layer& layer = out.layers[i];
        Json rest = *layer.style;
        rest["effects"] = Json::array();
        rest.erase("enabled");
        put_style(layer, spec_of(rest));
    }
    Json ids = Json::array();
    for (const core::Layer& l : below) ids.push_back(l.id);
    for (const core::Layer& l : above) ids.push_back(l.id);
    out.layers.insert(out.layers.begin() + static_cast<std::ptrdiff_t>(i + 1), above.begin(), above.end());
    out.layers.insert(out.layers.begin() + static_cast<std::ptrdiff_t>(i), below.begin(), below.end());
    c.report = Json{{"ids", std::move(ids)}};
}

// PC5 I-a: does the layer have what すべてのレイヤー効果を統合 draws in — its style's effects shown, its fill opacity, or a
// Genko 境界効果 (フチ・水彩境界)?
bool has_effects_to_bake(const core::Layer& layer) {
    const bool styled = layer.style && layer.style->is_object() &&
                        (core::style_shows_effects(*layer.style) || core::style_fill_opacity(*layer.style) < 1.0);
    return styled || (layer.effect && core::py_truthy(*layer.effect));
}

// bake_layer_styles {"page": n | "all", "ids"?} (ファイル → スクリプト → すべてのレイヤー効果を統合, after PhotoCraft v0.2.0's
// file_cmds.rs flatten_all_effects, MIT): each layer with effects drawn alone as it shows — its style's effects and fill
// opacity, its 境界効果, its mask and vector mask — into its pixels (an 8-bit page's at the paint layers' grid, a page in
// precise colour's values, a page of inks' inks), at full opacity, in the normal blend and not clipped: its opacity, blend,
// clipping and Blend If stay its own. A folder, a correction layer, a tone layer and a locked one are left (report
// "skipped").
void bake_layer_styles(core::OpContext& c) {
    core::Document& doc = c.doc;
    std::vector<std::size_t> pages;
    if (const Json* page = core::get(c.op, "page"); page != nullptr && page->is_string() && page->get_ref<const std::string&>() == "all") {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) pages.push_back(i);
    } else {
        pages.push_back(core::require_page(doc, c.op));
    }
    const Json* given = core::get(c.op, "ids");
    if (given != nullptr && !given->is_null() && pages.size() != 1) throw OpError("ids name the layers of one page (every page: no ids)");
    Json baked = Json::array();
    Json skipped = Json::array();
    for (const std::size_t at : pages) {
        std::vector<std::size_t> layers;
        if (given != nullptr && !given->is_null()) {
            layers = named_layers(doc.page(at), c.op);
        } else {
            for (std::size_t i = 0; i < doc.page(at).layers.size(); ++i)
                if (has_effects_to_bake(doc.page(at).layers[i])) layers.push_back(i);
        }
        for (const std::size_t i : layers) {
            const core::Page& page = doc.page(at);
            const core::Layer source = page.layers[i];
            const Json where{{"page", page.index.json()}, {"id", source.id}};
            const auto skip = [&](const char* why) {
                Json item = where;
                item["why"] = why;
                skipped.push_back(std::move(item));
            };
            if (!has_effects_to_bake(source)) {
                skip("no_effects");
                continue;
            }
            if (!core::takes_layer_style(source)) {
                skip("kind");  // (a folder, a correction or a tone layer: no pixels to draw them into)
                continue;
            }
            if (source.locked || source.lock_all) {
                skip("locked");
                continue;
            }
            Json rest = source.style ? *source.style : Json::object();  // (what stays the layer's: its Blend If)
            core::Layer alone = source;
            alone.visible = true;
            alone.opacity = 1.0;
            alone.blend = "normal";
            alone.clip = false;
            alone.blend_channels.clear();
            if (alone.style) {
                Json drawn = *alone.style;
                drawn.erase("blend_if");
                alone.style = drawn;
            }
            const Alone picture = composite_alone(page, {alone}, raster::working_dpi(page), &doc);
            core::Layer& layer = doc.edit_page(at).layers[i];
            layer.kind = core::LayerKind::Raster;
            layer.strokes = core::empty_strokes();
            layer.patches.clear();
            layer.shapes = core::vector::empty_shapes();
            layer.mask.reset();
            layer.vector_mask.reset();
            layer.fill.reset();
            layer.fill_rgb.reset();
            layer.effect.reset();
            layer.tone.reset();
            layer.smart.reset();
            layer.type.reset();
            layer.asset.reset();
            layer.placement_mm.reset();
            layer.frame_id.reset();
            layer.fit = "cover";
            layer.clip_to = "frame";
            layer.finish.reset();
            layer.source.reset();
            layer.native_psd.reset();  // (psd-keep@1: drawn in, a PSD's effects and blocks are not its own any more)
            layer.raster_png = nullptr;
            layer.raster_relpath.reset();
            layer.raster_memo = {};
            layer.color_raster = nullptr;
            layer.ink_raster = nullptr;
            layer.ink_memo = {};
            Json kept = Json::object();
            if (rest.is_object() && rest.contains("blend_if")) kept["blend_if"] = rest["blend_if"];
            layer.style = kept.empty() ? std::nullopt : std::optional<Json>(spec_of(kept));
            if (picture.inks) {
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*picture.inks));
            } else if (picture.precise) {
                layer.color_raster = std::make_shared<const std::string>(*picture.precise);
                if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end())
                    doc.features.emplace_back(core::kColorRasterFeature);
            } else {
                raster::save_raster(doc.page(at), layer, picture.image);
            }
            baked.push_back(where);
        }
    }
    c.report = Json{{"baked", std::move(baked)}, {"skipped", std::move(skipped)}};
}

}  // namespace

Json style_with_patterns(core::Document& doc, const Json& style) {
    if (!style.is_object()) return style;
    Json out = style;
    const auto effects = out.find("effects");
    if (effects == out.end() || !effects->is_array()) return out;
    for (Json& e : *effects) {
        if (!e.is_object()) continue;
        if (const auto p = e.find("pattern"); p != e.end() && !p->is_null()) *p = pattern_in_book(doc, *p);
        if (const auto t = e.find("texture"); t != e.end() && t->is_object()) {
            if (const auto p = t->find("pattern"); p != t->end() && !p->is_null()) *p = pattern_in_book(doc, *p);
        }
    }
    return out;
}

void forget_psd_effects(core::Layer& layer) {
    if (!layer.native_psd) return;
    std::erase_if(layer.native_psd->blocks, [](const auto& block) { return core::psd_effect_key(block.first); });
    layer.native_psd->ranges.clear();  // (its Blend If is the style's now: a style taken off does not bring the file's back)
    if (layer.native_psd->empty()) layer.native_psd.reset();
}

void register_layer_style_ops(core::OpRegistry& registry) {
    registry.add("set_layer_style", set_layer_style);
    registry.add("add_layer_effect", add_layer_effect);
    registry.add("set_global_light", set_global_light);
    registry.add("scale_layer_style", scale_layer_style);
    registry.add("show_layer_effects", show_layer_effects);
    registry.add("layer_style_to_layers", layer_style_to_layers);
    registry.add("bake_layer_styles", bake_layer_styles);  // (PC5 I-a: すべてのレイヤー効果を統合)
}

}  // namespace genko::render
