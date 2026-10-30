#include "core/layer_basics.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", why); }

bool same_points(const std::optional<std::vector<Point>>& a, const std::optional<std::vector<Point>>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->size() != b->size()) return false;
    for (std::size_t i = 0; i < a->size(); ++i)
        if (!((*a)[i].x == (*b)[i].x) || !((*a)[i].y == (*b)[i].y)) return false;
    return true;
}

bool same_rect(const std::optional<Rect>& a, const std::optional<Rect>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height);
}

bool same_patches(const std::vector<Patch>& a, const std::vector<Patch>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].png != b[i].png || a[i].attrs != b[i].attrs) return false;
    return true;
}

bool same_mask(const std::optional<Mask>& a, const std::optional<Mask>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->png == b->png && a->enabled == b->enabled && a->linked == b->linked && a->density == b->density && a->feather_mm == b->feather_mm);
}

// The ops that work on whole pages — their size, their mode, the image flattened, the layer comps applied (layer-comps@1
// shows and moves every layer it records, locked or not), the book's text styles changed — and so change a layer locked
// with すべてをロック as Photoshop's do (its flatten, image size, mode… do not look at the locks). An op that goes over every
// layer of a page (rasterize_layer all, show_layer_effects without ids, set_finish) skips those locked instead, as it
// skips those locked with locked (each op: locked_all).
bool page_op(const Json& op) {
    static constexpr std::string_view kOps[] = {"resize_pages", "set_canvas_size", "crop_page", "trim_page", "rotate_pages", "reveal_all",
                                                "set_color_mode", "assign_profile", "convert_profile", "to_bitmap", "to_indexed", "set_duotone",
                                                "set_inks", "set_color_table", "split_channels", "merge_channels", "set_page_spec",
                                                "apply_layer_comp", "set_global_light", "replace_fonts", "set_text_gamma",
                                                // (the page flattened: HDR トーン, a spot channel merged, a plate deleted — the page
                                                // multichannel then)
                                                "hdr_toning", "merge_spot_channel", "delete_channel"};
    const Json* name = op.is_object() ? get(op, "op") : nullptr;
    if (name == nullptr || !name->is_string()) return false;
    const std::string& text = name->get_ref<const std::string&>();
    if (std::find(std::begin(kOps), std::end(kOps), text) != std::end(kOps)) return true;
    // (PC5 J-d1: 角度補正 with crop: true turns and cuts the page, crop_page's way)
    if (text == "straighten_layer") return truthy_at(op, "crop");
    // (PC5 G-c: a text style of the book set, redefined or deleted — every text taking it set again, as Photoshop's)
    if (text == "type_style") {
        const Json* action = get(op, "action");
        return action != nullptr && action->is_string() &&
               (*action == Json("set") || *action == Json("redefine") || *action == Json("delete"));
    }
    return false;
}

}  // namespace

unsigned blend_channel_count(const Page& page) {
    if (!page.color) return page.rgb && page.rgb->table ? 0u : 3u;  // (an indexed page: none)
    const PageColor& color = *page.color;
    if (color.bitmap || !color.duotone.empty()) return 0;
    switch (color.space) {
    case InkSpace::Cmyk: return 4;
    case InkSpace::Lab: return 3;
    case InkSpace::Gray: return 1;
    default: return 0;  // (multichannel, a mask)
    }
}

bool leaves_channels_out(const Layer& layer) {
    return std::any_of(layer.blend_channels.begin(), layer.blend_channels.end(), [](bool on) { return !on; });
}

bool uses_layer_locks(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers)
            if (layer.lock_all || layer.lock_nest) return true;
    }
    return false;
}

bool uses_blend_channels(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers)
            if (!layer.blend_channels.empty()) return true;
    }
    return false;
}

void validate_layer_basics(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        const Page& page = *written.page;
        const std::string where =
            written.contents.empty() ? "page " + py_str(page.index.json()) + ": " : "the smart object contents " + written.contents + ": ";
        const unsigned count = blend_channel_count(page);
        for (const Layer& layer : page.layers) {
            if (layer.blend_channels.empty()) continue;
            if (layer.kind == LayerKind::Folder) bad(where + "the folder " + layer.id + " has blend_channels (a layer's only)");
            if (count == 0) bad(where + "the layer " + layer.id + " leaves channels out on a page whose mode has none to leave out");
            if (layer.blend_channels.size() != count) {
                bad(where + "the layer " + layer.id + "'s blend_channels has " + std::to_string(layer.blend_channels.size()) + " flags; the page has " +
                    std::to_string(count) + " colour channels");
            }
            if (!leaves_channels_out(layer)) bad(where + "the layer " + layer.id + "'s blend_channels leaves nothing out (none is written then)");
        }
    }
}

void settle_blend_channels(Document& doc) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (!doc.pages[p]) continue;
        const Page& page = *doc.pages[p];
        const unsigned count = blend_channel_count(page);
        const auto off = [count](const Layer& l) { return !l.blend_channels.empty() && l.blend_channels.size() != count; };
        // (the review's P3-5: the deferred pages looked up only for a page with such a layer; one not read yet: as written)
        if (std::none_of(page.layers.begin(), page.layers.end(), off) || doc.is_deferred(p)) continue;
        for (Layer& layer : doc.edit_page(p).layers)
            if (off(layer)) layer.blend_channels.clear();
    }
}

bool same_locked_look(const Layer& a, const Layer& b) {
    return a.kind == b.kind && a.strokes == b.strokes && a.raster_png == b.raster_png && a.raster_relpath == b.raster_relpath &&
           a.fill_rgb == b.fill_rgb && a.lpi == b.lpi && a.density == b.density && same_points(a.region, b.region) && a.opacity == b.opacity &&
           a.angle == b.angle && a.blend == b.blend && a.clip == b.clip && same_patches(a.patches, b.patches) && a.tone == b.tone &&
           a.asset == b.asset && same_rect(a.placement_mm, b.placement_mm) && a.fit == b.fit && a.clip_to == b.clip_to && a.finish == b.finish &&
           same_mask(a.mask, b.mask) && a.fill == b.fill && a.adjust == b.adjust && a.effect == b.effect && a.color_raster == b.color_raster &&
           a.ink_raster == b.ink_raster && a.screen == b.screen && a.smart == b.smart && a.shapes == b.shapes && a.vector_mask == b.vector_mask &&
           a.style == b.style && a.type == b.type && a.group == b.group && a.blend_channels == b.blend_channels && a.panel_clip == b.panel_clip &&
           a.panel_each == b.panel_each && a.frame_id == b.frame_id && a.exportable == b.exportable && a.color == b.color &&
           a.color_prints == b.color_prints && a.source == b.source && a.role == b.role;
}

bool locked_all(const std::vector<Layer>& layers, std::size_t index) {
    if (index >= layers.size()) return false;
    std::set<std::size_t> seen{index};
    for (const Layer* at = &layers[index];;) {
        if (at->lock_all) return true;
        if (!at->parent_id.is_string()) return false;
        const std::string& parent = at->parent_id.get_ref<const std::string&>();
        const Layer* up = nullptr;
        for (std::size_t i = layers.size(); i-- > 0;) {  // (the last layer of an id, as the ops find it)
            if (layers[i].id == parent) {
                if (layers[i].kind == LayerKind::Folder && seen.insert(i).second) up = &layers[i];
                break;
            }
        }
        if (up == nullptr) return false;
        at = up;
    }
}

std::vector<LockedLayer> locked_layers(const Document& doc) {
    std::vector<LockedLayer> out;
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (!doc.pages[p]) continue;
        const std::vector<Layer>& layers = doc.pages[p]->layers;
        if (std::none_of(layers.begin(), layers.end(), [](const Layer& l) { return l.lock_all; }) || doc.is_deferred(p)) continue;
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (locked_all(layers, i)) out.push_back(LockedLayer{doc.pages[p]->id, layers[i]});
    }
    return out;
}

void check_locked_layers(const std::vector<LockedLayer>& before, const Document& after, const Json& op) {
    if (before.empty() || page_op(op)) return;
    // (the review's P2-1: only set_layer and set_layers take a lock off — any other op that leaves a locked layer unlocked,
    // a layer made anew under its id, is looked at as it was locked)
    const Json* name = get(op, "op");
    const bool unlocking = name != nullptr && (*name == Json("set_layer") || *name == Json("set_layers"));
    std::map<std::string, const Page*, std::less<>> pages;
    for (std::size_t p = 0; p < after.pages.size(); ++p)
        if (after.pages[p] && !after.is_deferred(p)) pages.emplace(after.pages[p]->id, after.pages[p].get());
    for (const LockedLayer& was : before) {
        const auto page = pages.find(was.page);
        if (page == pages.end()) continue;  // (the page deleted, as a page may be)
        const std::vector<Layer>& layers = page->second->layers;
        std::size_t now = layers.size();
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (layers[i].id == was.layer.id) now = i;
        if (now == layers.size()) throw OpError("the layer " + was.layer.id + " is locked (lock_all): it cannot be deleted");
        // (unlocked by set_layer — its own lock_all, or its folder's, off. Taken out of a folder locked so by another op:
        // as it was locked)
        if (unlocking && !locked_all(layers, now)) continue;
        if (!same_locked_look(was.layer, layers[now]) || !locked_all(layers, now)) throw OpError("the layer " + was.layer.id + " is locked (lock_all)");
    }
}

bool core_layer(const Layer& layer) {
    return layer.role == LayerRole::Name || layer.role == LayerRole::Ink || layer.role == LayerRole::Bg || layer.role == LayerRole::Finish;
}

std::vector<std::size_t> folder_contents(const std::vector<Layer>& layers, std::size_t folder) {
    std::vector<std::size_t> out;
    if (folder >= layers.size() || layers[folder].kind != LayerKind::Folder) return out;
    std::map<std::string, std::size_t, std::less<>> by_id;
    for (std::size_t i = 0; i < layers.size(); ++i) by_id[layers[i].id] = i;  // (the last layer of an id, as the ops find it)
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i == folder) continue;
        std::set<std::size_t> seen{i};
        for (const Layer* up = &layers[i]; up->parent_id.is_string();) {
            const auto parent = by_id.find(up->parent_id.get_ref<const std::string&>());
            if (parent == by_id.end() || !seen.insert(parent->second).second) break;
            if (parent->second == folder) {
                out.push_back(i);
                break;
            }
            up = &layers[parent->second];
        }
    }
    return out;
}

bool empty_layer(const std::vector<Layer>& layers, std::size_t index) {
    const Layer& layer = layers[index];
    if (layer.kind == LayerKind::Folder) {
        if (layer.mask || layer.vector_mask) return false;
        const std::vector<std::size_t> inside = folder_contents(layers, index);
        return std::all_of(inside.begin(), inside.end(), [&](std::size_t i) {
            const Layer& in = layers[i];
            return in.kind == LayerKind::Folder ? !in.mask && !in.vector_mask : empty_layer(layers, i);
        });
    }
    if (layer.kind != LayerKind::Strokes && layer.kind != LayerKind::Raster) return false;  // (a fill, a correction, a picture, a tone…)
    const bool holds = layer.stroke_count() > 0 || !layer.patches.empty() || (layer.shapes && !layer.shapes->items.empty()) ||
                       (layer.raster_png && !layer.raster_png->empty()) || layer.raster_relpath || layer.color_raster || layer.ink_raster ||
                       (layer.fill && py_truthy(*layer.fill)) || (layer.fill_rgb && !layer.fill_rgb->empty()) || (layer.tone && py_truthy(*layer.tone));
    const bool styled = layer.style && layer.style->contains("effects") && !(*layer.style)["effects"].empty();
    return !holds && !layer.mask && !layer.vector_mask && !styled && !(layer.effect && py_truthy(*layer.effect)) && !layer.native_psd;
}

void forget_layers(Document& doc, std::size_t at, const std::set<std::string>& gone) {
    if (gone.empty()) return;
    const auto named = [&gone](const Json& value) { return value.is_string() && gone.contains(value.get_ref<const std::string&>()); };
    Page& page = doc.edit_page(at);
    const auto let_go = [&](Json& holder, const char* key) {
        if (holder.is_object() && holder.contains(key) && named(holder[key])) holder[key] = nullptr;
    };
    if (page.rulers.is_array())
        for (Json& ruler : page.rulers) let_go(ruler, "layer_id");
    if (page.ruler) let_go(*page.ruler, "layer_id");
    if (page.extra.is_object() && page.extra.contains("anim") && page.extra["anim"].is_object()) {
        Json& anim = page.extra["anim"];
        if (anim.contains("tracks") && anim["tracks"].is_array()) {
            Json tracks = Json::array();
            for (Json& track : anim["tracks"]) {
                // (the track of an animation folder deleted or ungrouped goes with it: its cels are layers as any now)
                if (track.is_object() && track.contains("folder") && named(track["folder"])) continue;
                if (track.is_object() && track.contains("cels") && track["cels"].is_array()) {
                    for (Json& cel : track["cels"])  // [frame, cel layer id | null]
                        if (cel.is_array() && cel.size() >= 2 && named(cel[1])) cel[1] = nullptr;
                }
                tracks.push_back(std::move(track));
            }
            anim["tracks"] = std::move(tracks);
        }
        if (anim.contains("light_table") && anim["light_table"].is_array()) {
            Json kept = Json::array();
            for (const Json& cel : anim["light_table"])
                if (!named(cel)) kept.push_back(cel);
            anim["light_table"] = std::move(kept);
        }
    }
    // (a selection kept from a layer deleted — selops' {"layer": id}, and in a union, intersect or subtract — goes: it
    // would only say "no layer" when loaded)
    if (page.extra.is_object() && page.extra.contains("saved_areas") && page.extra["saved_areas"].is_object()) {
        const std::function<bool(const Json&)> names_gone = [&](const Json& area) {
            if (!area.is_object()) return false;
            if (area.contains("layer") && named(area["layer"])) return true;
            for (const char* key : {"union", "intersect", "subtract"}) {
                if (!area.contains(key) || !area[key].is_array()) continue;
                for (const Json& part : area[key])
                    if (names_gone(part)) return true;
            }
            return false;
        };
        Json& areas = page.extra["saved_areas"];
        for (auto it = areas.begin(); it != areas.end();) {
            if (names_gone(*it)) it = areas.erase(it);
            else ++it;
        }
    }
    const Num index = page.index;
    for (StoryLine& line : doc.story) {
        if (!(line.page_index == index) || !line.style.is_object()) continue;
        if (const auto under = line.style.find("below_layer"); under != line.style.end() && named(*under)) line.style.erase(under);
    }
}

}  // namespace genko::core
