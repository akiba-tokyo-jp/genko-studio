#include "storage/snapshot.hpp"

#include <string>

#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pynum.hpp"
#include "core/smart.hpp"
#include "core/strokes.hpp"
#include "storage/writer.hpp"

namespace genko::storage {

using core::Json;

namespace {

Json get_or_null(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? Json(nullptr) : *it;
}

Json frame_brief(const core::Frame& frame) {
    Json out = Json::object();
    out["id"] = frame.id;
    out["x"] = frame.rect.x.json();
    out["y"] = frame.rect.y.json();
    out["width"] = frame.rect.width.json();
    out["height"] = frame.rect.height.json();
    return out;
}

Json line_brief(const core::StoryLine& line) {
    Json out = Json::object();
    out["id"] = line.id;
    out["text"] = line.text;
    out["speaker"] = line.speaker;
    out["frame_id"] = line.frame_id ? Json(*line.frame_id) : Json(nullptr);
    out["x_mm"] = line.x_mm.json();
    out["y_mm"] = line.y_mm.json();
    out["w_mm"] = line.w_mm.json();
    out["h_mm"] = line.h_mm.json();
    out["balloon"] = line.balloon;
    out["wrap"] = line.wrap;
    if (core::py_truthy(line.style)) out["style"] = line.style;
    return out;
}

Json layer_brief(const core::Layer& layer, bool colour_model) {
    Json out = Json::object();
    out["id"] = layer.id;
    out["role"] = core::to_string(layer.role);
    out["kind"] = core::to_string(layer.kind);
    out["title"] = layer.title;
    out["exportable"] = layer.exportable;
    out["visible"] = layer.visible;
    out["opacity"] = layer.opacity;
    out["blend"] = layer.blend.empty() ? std::string("normal") : layer.blend;
    out["clip"] = layer.clip;
    out["locked"] = layer.locked;
    out["stroke_count"] = static_cast<std::int64_t>(layer.stroke_count());
    out["patch_count"] = static_cast<std::int64_t>(layer.patches.size());
    if (layer.mask) out["mask"] = Json::object({{"enabled", layer.mask->enabled}});
    if (layer.color && !layer.color->empty()) {
        Json color = Json::array();
        for (const auto v : *layer.color) color.push_back(v);
        out["color"] = std::move(color);
    }
    if (core::py_truthy(layer.parent_id)) out["parent_id"] = layer.parent_id;
    if (layer.reference) out["reference"] = true;
    if (!layer.panel_clip) out["panel_clip"] = false;
    // (color-model@1, on a page of a colour model alone: the precision its pixels are kept in)
    if (colour_model) {
        try {  // (pixels this build cannot read — a read-only book's — have none said)
            if (layer.ink_raster) out["precision"] = core::ink_raster_header(*layer.ink_raster).depth;
            else if (layer.color_raster) out["precision"] = static_cast<unsigned char>(layer.color_raster->at(5)) == 2 ? "u16" : "f32";
            else if (layer.raster_png && !layer.raster_png->empty()) out["precision"] = 8;
        } catch (const std::exception&) {
            out.erase("precision");
        }
    }
    return out;
}

// smart-objects@1: what genko inspect says of a smart object — its contents (id, kind, name, size; a file's format,
// pixels, depth, colours, colour use and whether it is linked; a page's layers), its matrix and interpolation, how many
// smart filters it has and its stack mode.
Json smart_brief(const core::Document& doc, const core::Layer& layer) {
    Json out = Json::object();
    const auto id = core::smart::contents_id(layer);
    out["contents"] = id ? Json(*id) : Json(nullptr);
    const core::SmartContents* contents = core::smart::contents_of(doc, layer);
    if (contents != nullptr) {
        out["kind"] = contents->kind;
        out["name"] = contents->name;
        const auto [w, h] = core::smart::size_mm(*contents);
        out["size_mm"] = Json::array({core::py_round(w, 3), core::py_round(h, 3)});
        if (contents->kind == "file") {
            out["format"] = contents->format;
            out["size_px"] = Json::array({contents->width_px, contents->height_px});
            out["depth"] = contents->depth;
            out["mode"] = contents->mode;
            out["use"] = core::py_get(contents->profile, "use", Json("keep"));
            out["linked"] = contents->link.has_value();
        } else {
            out["layers"] = static_cast<std::int64_t>(contents->page ? contents->page->layers.size() : 0);
        }
    }
    const Json settings = layer.smart && layer.smart->is_object() ? *layer.smart : Json::object();
    out["matrix"] = get_or_null(settings, "matrix");
    out["interp"] = get_or_null(settings, "interp");
    const Json filters = get_or_null(settings, "filters");
    out["filters"] = static_cast<std::int64_t>(filters.is_array() ? filters.size() : 0);
    const Json stack = get_or_null(settings, "stack");
    out["stack"] = stack.is_null() ? Json("none") : stack;
    return out;
}

// color-model@1: a page's colour model as the book keeps it, and its profile's description.
Json colour_brief(const core::Page& page) {
    Json out = page.color ? core::to_json(*page.color) : core::to_json(*page.rgb);
    if (page.color) {
        out["profile_description"] = core::profile_description(page.color->profile, page.color->icc, page.color->space == core::InkSpace::Lab ||
                                                                        (page.color->space == core::InkSpace::Multi && page.color->from == "lab"));
    } else {
        out["profile_description"] = core::profile_description(page.rgb->profile, page.rgb->icc);
    }
    return out;
}

// channels@1: a page's channels as the channel panel lists them (their settings, depth and size; not their pixels). A
// raster that cannot be read (its asset broken: the book is read-only) is listed with its problem, the rest as usual.
Json channels_brief(const core::Page& page) {
    Json out = Json::array();
    for (const core::Channel& channel : page.channels) {
        Json item = core::to_json(channel, std::string());
        item.erase("raster");
        if (channel.raster) {
            try {
                const core::InkRasterHeader header = core::ink_raster_header(*channel.raster);
                if (header.space != core::InkSpace::Mask) throw core::Error("format", "the channel's raster is not a mask");
                item["depth"] = header.depth;
                item["size"] = Json::array({header.width, header.height});
            } catch (const core::Error& error) {
                item["depth"] = core::page_depth(page);
                item["size"] = nullptr;
                item["problem"] = error.what();
            }
        } else {
            item["depth"] = core::page_depth(page);
            item["size"] = nullptr;  // (none yet: all 0)
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::int64_t count(const core::Page& page, core::LayerRole role) {
    std::int64_t n = 0;
    for (const auto& layer : page.layers) {
        if (layer.role == role) n += static_cast<std::int64_t>(layer.stroke_count());
    }
    return n;
}

// page.name_strokes / page.ink_strokes: the points of the first layer with the role.
Json role_strokes(const core::Page& page, core::LayerRole role) {
    Json out = Json::array();
    if (const core::Layer* layer = page.first_layer(role)) {
        for (const auto& stroke : layer->strokes->items) out.push_back(core::stroke_points_json(*stroke));
    }
    return out;
}

const Json& object_or_fail(const Json& value, const char* what) {
    if (!value.is_object()) {
        throw core::Error("format", std::string(what) + " must be an object, not " + core::py_repr(value));
    }
    return value;
}

}  // namespace

Json snapshot(const core::Document& doc, bool full) {
    Json pages = Json::array();
    for (const auto& page_ptr : doc.pages) {
        const core::Page& page = *page_ptr;
        const auto leaves = page.leaf_frames();
        Json item = Json::object();
        item["index"] = page.index.json();
        item["name_ok"] = page.name_ok;
        item["stage"] = page.stage;
        item["note"] = page.note;
        item["leaf_count"] = static_cast<std::int64_t>(leaves.size());
        Json leaf_list = Json::array();
        for (const core::Frame* frame : leaves) leaf_list.push_back(frame_brief(*frame));
        item["leaves"] = std::move(leaf_list);
        Json story = Json::array();
        for (const core::StoryLine* line : doc.story_for_page(page.index)) story.push_back(line_brief(*line));
        item["story"] = std::move(story);
        item["name_stroke_count"] = count(page, core::LayerRole::Name);
        item["ink_stroke_count"] = count(page, core::LayerRole::Ink);
        item["selected_frame_id"] = page.selected_frame_id;
        const bool colour_model = page.color.has_value() || page.rgb.has_value();
        if (colour_model) item["color"] = colour_brief(page);
        if (!page.channels.empty()) item["channels"] = channels_brief(page);  // (channels@1: on a page with one only)
        Json layers = Json::array();
        for (const auto& layer : page.layers) layers.push_back(layer_brief(layer, colour_model));
        // (smart-objects@1: a smart object's contents and settings, as genko inspect shows them)
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (page.layers[i].kind == core::LayerKind::Smart) layers[i]["smart"] = smart_brief(doc, page.layers[i]);
        }
        item["layers"] = std::move(layers);
        Json prims = Json::array();
        for (const Json& prim : page.prims) {
            const Json& p = object_or_fail(prim, "a prim");
            Json brief = Json::object();
            brief["id"] = get_or_null(p, "id");
            brief["kind"] = get_or_null(p, "kind");
            const Json scene = get_or_null(p, "scene");
            if (core::py_truthy(scene)) brief["scene"] = scene;
            prims.push_back(std::move(brief));
        }
        item["prims"] = std::move(prims);
        if (!page.effects.empty()) {
            Json effects = Json::array();
            for (const Json& effect : page.effects) {
                const Json& e = object_or_fail(effect, "an effect");
                Json brief = Json::object();
                brief["id"] = get_or_null(e, "id");
                brief["kind"] = get_or_null(e, "kind");
                brief["frame_id"] = get_or_null(e, "frame_id");
                const Json params = get_or_null(e, "params");
                brief["params"] = core::py_truthy(params) ? params : Json::object();
                effects.push_back(std::move(brief));
            }
            item["effects"] = std::move(effects);
        }
        const Json& extra = page.extra;
        if (const Json assignee = get_or_null(extra, "assignee"); core::py_truthy(assignee)) item["assignee"] = assignee;
        if (const Json cover = get_or_null(extra, "cover"); core::py_truthy(cover)) item["cover"] = cover;
        if (const Json anim_value = get_or_null(extra, "anim"); core::py_truthy(anim_value)) {
            const Json& anim = object_or_fail(anim_value, "the animation");
            Json animation = Json::object();
            animation["fps"] = get_or_null(anim, "fps");
            animation["frames"] = get_or_null(anim, "frames");
            animation["loop"] = anim.contains("loop") ? anim["loop"] : Json(true);
            animation["tracks"] = anim.contains("tracks") ? anim["tracks"] : Json::array();
            animation["camera"] = anim.contains("camera") ? anim["camera"] : Json::array();
            animation["light_table"] = anim.contains("light_table") ? anim["light_table"] : Json::array();
            item["animation"] = std::move(animation);
        }
        if (full) {
            item["name_strokes"] = role_strokes(page, core::LayerRole::Name);
            item["ink_strokes"] = role_strokes(page, core::LayerRole::Ink);
        }
        pages.push_back(std::move(item));
    }
    Json out = Json::object();
    out["title"] = doc.title;
    out["episode"] = doc.episode.json();
    out["binding"] = core::to_string(doc.binding);
    out["spec"] = spec_to_json(doc.spec);
    out["pages"] = std::move(pages);
    out["tickets"] = doc.tickets;
    out["autosave"] = doc.autosave;
    if (doc.text_gamma) out["text_gamma"] = *doc.text_gamma;  // (color-model@1, PC1b: only a book that has one)
    if (doc.file_info && !doc.file_info->empty()) out["file_info"] = core::to_json(*doc.file_info);  // (file-info@1, PC4 F-b: likewise)
    return out;
}

Json inspect_stroke(const core::Document& doc, std::string_view stroke_id) {
    for (const auto& page : doc.pages) {
        for (const auto& layer : page->layers) {
            for (const auto& stroke : layer.strokes->items) {
                if (stroke->id == stroke_id) {
                    Json out = Json::object();
                    out["id"] = stroke->id;
                    out["page"] = page->index.json();
                    out["layer"] = core::to_string(layer.role);
                    out["points"] = core::stroke_points_json(*stroke);
                    out["kind"] = stroke->kind;
                    return out;
                }
            }
        }
    }
    throw core::Error("key", "no stroke " + std::string(stroke_id));
}

}  // namespace genko::storage
