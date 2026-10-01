#include "storage/snapshot.hpp"

#include <string>

#include "core/error.hpp"
#include "core/pyconv.hpp"
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

Json layer_brief(const core::Layer& layer) {
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
    if (layer.parent_id && !layer.parent_id->empty()) out["parent_id"] = *layer.parent_id;
    if (layer.reference) out["reference"] = true;
    if (!layer.panel_clip) out["panel_clip"] = false;
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
        item["selected_frame_id"] = page.selected_frame_id ? Json(*page.selected_frame_id) : Json(nullptr);
        Json layers = Json::array();
        for (const auto& layer : page.layers) layers.push_back(layer_brief(layer));
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
