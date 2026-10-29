#include "core/layer_basics.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
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

// The ops that work on whole pages — their size, their mode, the layer comps applied (layer-comps@1 shows and moves every
// layer it records, locked or not) — and so change a layer locked with すべてをロック as Photoshop's do.
bool page_op(std::string_view op) {
    static constexpr std::string_view kOps[] = {"resize_pages", "set_canvas_size", "crop_page", "trim_page", "rotate_pages", "reveal_all",
                                                "set_color_mode", "assign_profile", "convert_profile", "to_bitmap", "to_indexed", "set_duotone",
                                                "set_inks", "set_color_table", "split_channels", "merge_channels", "set_page_spec",
                                                "apply_layer_comp", "set_global_light", "replace_fonts", "set_text_gamma"};
    return std::find(std::begin(kOps), std::end(kOps), op) != std::end(kOps);
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
        if (!doc.pages[p] || doc.is_deferred(p)) continue;  // (a page not read yet: as it was written)
        const Page& page = *doc.pages[p];
        const unsigned count = blend_channel_count(page);
        const auto off = [count](const Layer& l) { return !l.blend_channels.empty() && l.blend_channels.size() != count; };
        if (std::none_of(page.layers.begin(), page.layers.end(), off)) continue;
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
           a.panel_each == b.panel_each;
}

std::vector<LockedLayer> locked_layers(const Document& doc) {
    std::vector<LockedLayer> out;
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (!doc.pages[p] || doc.is_deferred(p)) continue;
        for (const Layer& layer : doc.pages[p]->layers)
            if (layer.lock_all) out.push_back(LockedLayer{doc.pages[p]->id, layer});
    }
    return out;
}

void check_locked_layers(const std::vector<LockedLayer>& before, const Document& after, std::string_view op) {
    if (before.empty() || page_op(op)) return;
    std::map<std::string, const Page*, std::less<>> pages;
    for (std::size_t p = 0; p < after.pages.size(); ++p)
        if (after.pages[p] && !after.is_deferred(p)) pages.emplace(after.pages[p]->id, after.pages[p].get());
    for (const LockedLayer& was : before) {
        const auto page = pages.find(was.page);
        if (page == pages.end()) continue;  // (the page deleted, as a page may be)
        const Layer* now = nullptr;
        for (const Layer& layer : page->second->layers)
            if (layer.id == was.layer.id) now = &layer;
        if (now == nullptr) throw OpError("the layer " + was.layer.id + " is locked (lock_all): it cannot be deleted");
        if (!now->lock_all) continue;  // (unlocked by the op: set_layer lock_all false)
        if (!same_locked_look(was.layer, *now)) throw OpError("the layer " + was.layer.id + " is locked (lock_all)");
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
            for (Json& track : anim["tracks"]) {
                if (!track.is_object() || !track.contains("cels") || !track["cels"].is_array()) continue;
                for (Json& cel : track["cels"])  // [frame, cel layer id | null]
                    if (cel.is_array() && cel.size() >= 2 && named(cel[1])) cel[1] = nullptr;
            }
        }
        if (anim.contains("light_table") && anim["light_table"].is_array()) {
            Json kept = Json::array();
            for (const Json& cel : anim["light_table"])
                if (!named(cel)) kept.push_back(cel);
            anim["light_table"] = std::move(kept);
        }
    }
    const Num index = page.index;
    for (StoryLine& line : doc.story) {
        if (!(line.page_index == index) || !line.style.is_object()) continue;
        if (const auto under = line.style.find("below_layer"); under != line.style.end() && named(*under)) line.style.erase(under);
    }
}

}  // namespace genko::core
