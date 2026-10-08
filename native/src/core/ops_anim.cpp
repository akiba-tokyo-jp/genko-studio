// The animation ops (Python's genko/animops.py): the timeline (set_animation), animation folders and cels
// (add_anim_folder, add_cel), the exposure sheet (set_exposure, set_exposures), the camera (set_camera_key) and the
// light table (set_light_table). No pictures. core/anim.hpp reads the timeline they write.
//
// Where Python would keep a number that is not finite (a camera rect of "nan" or "inf", which its json.dumps writes as
// NaN / Infinity), the op is refused here: a book never holds one (docs/cpp-migration/ARCHITECTURE.md §3).

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// op.get(key) (null when missing)
Json op_get(const Json& op, std::string_view key) {
    const Json* value = get(op, key);
    return value != nullptr ? *value : Json();
}

// animops._anim: the timeline, made ({fps 12, frames 24, loop, no tracks}) when `create` and the page has none.
Json& anim_of(Page& page, bool create) {
    if (!page.extra.is_object()) page.extra = Json::object();
    const auto found = page.extra.find("anim");
    if (found != page.extra.end() && found->is_object()) return *found;
    if (!create) throw OpError("page " + page.index.repr() + " is not an animation (set_animation first)");
    Json data = Json::object();
    data["fps"] = 12;
    data["frames"] = 24;
    data["loop"] = true;
    data["tracks"] = Json::array();
    page.extra["anim"] = std::move(data);
    return page.extra["anim"];
}

// animops._frame: int(value) between 1 and the page's frames; "<what> is a frame number" where int() fails.
std::int64_t frame_of(const Page& page, const Json& value, const std::string& what = "frame") {
    std::int64_t frame = 0;
    try {
        frame = to_int_held(value);
    } catch (const PyValueError&) {
        throw OpError(what + " is a frame number");
    } catch (const PyTypeError&) {
        throw OpError(what + " is a frame number");
    }
    const std::int64_t count = anim::frames_of(page);
    if (frame < 1 || frame > count) throw OpError(what + " is 1 to " + std::to_string(count));
    return frame;
}

// The first track of the timeline whose folder == folder_id, to change (anim.track).
Json& track_of(Json& data, const std::string& folder_id) {
    const auto tracks = data.find("tracks");
    if (tracks == data.end() || !tracks->is_array()) throw OpError("folder is an animation folder's id (add_anim_folder)");
    for (Json& t : *tracks) {
        if (py_equals(py_get(t, "folder"), Json(folder_id))) return t;
    }
    throw OpError("folder is an animation folder's id (add_anim_folder)");
}

// animops._folder: the animation folder (a folder layer with a track) op["folder"] names.
const Layer& folder_of(const Page& page, const Json& folder_id) {
    const std::string id = py_str(py_or(folder_id, ""));
    const auto folder = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& layer) { return layer.id == id; });
    if (folder == page.layers.end() || folder->kind != LayerKind::Folder || anim::track(page, Json(folder->id)) == nullptr) {
        throw OpError("folder is an animation folder's id (add_anim_folder)");
    }
    return *folder;
}

// animops._cel: null, or the id of a cel in this folder.
Json cel_of(const Page& page, const std::string& folder_id, const Json& cel_id) {
    if (cel_id.is_null()) return nullptr;
    const std::string id = py_str(cel_id);
    for (const Layer* layer : anim::cels_of(page, Json(folder_id))) {
        if (layer->id == id) return id;
    }
    throw OpError("cel must be a layer in the animation folder");
}

// sorted(items, key=lambda item: item[key]) (stable; the keys taken first)
template <typename Key>
Json sorted_items(const std::vector<Json>& items, Key key) {
    std::vector<std::pair<Json, std::size_t>> keyed;
    for (std::size_t i = 0; i < items.size(); ++i) keyed.emplace_back(key(items[i]), i);
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return py_less(a.first, b.first); });
    Json out = Json::array();
    for (const auto& [unused, i] : keyed) out.push_back(items[i]);
    return out;
}

Json first_of(const Json& item) { return subscript(item, 0); }
Json frame_key(const Json& key) { return subscript(key, "frame"); }

// animops._expose: from `frame` on, this cel (the frame's old entry replaced).
void expose(Json& entry, std::int64_t frame, const Json& cel) {
    std::vector<Json> cels;
    for (const Json& item : iterate(py_get(entry, "cels", Json::array()))) {
        if (!py_equals(first_of(item), Json(frame))) cels.push_back(item);
    }
    cels.push_back(Json::array({frame, cel}));
    entry["cels"] = sorted_items(cels, first_of);
}

void set_animation(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    if (truthy_at(c.op, "off")) {
        if (page.extra.is_object()) page.extra.erase("anim");
        return;
    }
    Json& data = anim_of(page, true);
    if (has(c.op, "fps")) {
        const double fps = to_float(c.op["fps"]);
        if (!(1 <= fps && fps <= 60)) throw OpError("fps is 1 to 60");
        data["fps"] = fps;
    }
    if (has(c.op, "frames")) {
        const std::int64_t frames = to_int_held(c.op["frames"]);
        if (frames < 1 || frames > anim::kMaxFrames) throw OpError("frames is 1 to " + std::to_string(anim::kMaxFrames));
        data["frames"] = frames;
        if (const auto tracks = data.find("tracks"); tracks != data.end()) {  // (exposures past the end go)
            if (!tracks->is_array()) {
                const std::vector<Json> items = iterate(*tracks);
                if (!items.empty()) raise_attribute_error(items.front(), "get");
            } else {
                for (Json& entry : *tracks) {
                    Json kept = Json::array();
                    for (const Json& item : iterate(py_get(entry, "cels", Json::array()))) {
                        if (py_less(first_of(item), Json(frames), "<=")) kept.push_back(item);
                    }
                    entry["cels"] = std::move(kept);
                }
            }
        }
        Json camera = Json::array();
        for (const Json& key : iterate(py_or(py_get(data, "camera"), Json::array()))) {
            if (py_less(frame_key(key), Json(frames), "<=")) camera.push_back(key);
        }
        data["camera"] = std::move(camera);
    }
    if (has(c.op, "loop")) data["loop"] = py_truthy(c.op["loop"]);
}

void add_anim_folder(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    Json& data = anim_of(page, true);
    Layer folder;
    folder.id = truthy_at(c.op, "id") ? py_str(c.op["id"]) : new_id();
    folder.role = LayerRole::User;
    folder.kind = LayerKind::Folder;
    folder.title = truthy_at(c.op, "name") ? py_str(c.op["name"])
                                           : "アニメーション " + std::to_string(length(subscript(data, "tracks")) + 1);
    folder.exportable = true;
    for (const Layer& layer : page.layers) {
        if (layer.id == folder.id) throw OpError("layer " + folder.id + " exists");
    }
    const std::string id = folder.id;
    page.layers.push_back(std::move(folder));
    if (!data.contains("tracks")) (void)subscript(data, "tracks");  // (Python's KeyError('tracks'), never a new key)
    Json& tracks = data["tracks"];
    if (!tracks.is_array()) raise_attribute_error(tracks, "append");
    tracks.push_back(Json{{"folder", id}, {"cels", Json::array()}});
}

void add_cel(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    const std::string folder_id = folder_of(page, op_get(c.op, "folder")).id;
    const std::string kind = truthy_at(c.op, "kind") ? py_str(c.op["kind"]) : std::string("pen");
    if (kind != "pen" && kind != "paint") throw OpError("kind must be pen or paint");
    const std::size_t cels = anim::cels_of(page, Json(folder_id)).size();
    Layer cel;
    cel.id = truthy_at(c.op, "id") ? py_str(c.op["id"]) : new_id();
    cel.role = LayerRole::User;
    cel.kind = kind == "pen" ? LayerKind::Strokes : LayerKind::Raster;
    cel.title = truthy_at(c.op, "name") ? py_str(c.op["name"]) : std::to_string(cels + 1);
    cel.parent_id = folder_id;
    cel.exportable = true;
    for (const Layer& layer : page.layers) {
        if (layer.id == cel.id) throw OpError("layer " + cel.id + " exists");
    }
    const std::string cel_id = cel.id;
    const auto folder = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& layer) { return layer.id == folder_id; });
    page.layers.insert(folder, std::move(cel));  // (a folder's layers sit just under it)
    Json& entry = track_of(anim_of(page, false), folder_id);
    if (has(c.op, "at")) {
        expose(entry, frame_of(page, c.op["at"], "at"), cel_id);
    } else if (!py_truthy(subscript(entry, "cels"))) {
        entry["cels"] = Json::array({Json::array({1, cel_id})});
    }
}

void set_exposure(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    const std::string folder_id = folder_of(page, op_get(c.op, "folder")).id;
    const std::int64_t frame = frame_of(page, op_get(c.op, "frame"));
    Json& entry = track_of(anim_of(page, false), folder_id);
    if (truthy_at(c.op, "clear")) {
        Json kept = Json::array();
        for (const Json& item : iterate(subscript(entry, "cels"))) {
            if (!py_equals(first_of(item), Json(frame))) kept.push_back(item);
        }
        entry["cels"] = std::move(kept);
        return;
    }
    expose(entry, frame, cel_of(page, folder_id, op_get(c.op, "cel")));
}

void set_exposures(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    const std::string folder_id = folder_of(page, op_get(c.op, "folder")).id;
    const Json items = op_get(c.op, "cels");
    if (!items.is_array()) throw OpError("cels is a list of [frame, cel id or null]");
    std::map<std::int64_t, Json> sheet;
    for (const Json& item : items) {
        if (!item.is_array() || item.size() != 2) throw OpError("cels is a list of [frame, cel id or null]");
        Json cel = cel_of(page, folder_id, item[1]);  // (Python reads the value before the key)
        sheet[frame_of(page, item[0])] = std::move(cel);
    }
    Json sheet_list = Json::array();
    for (const auto& [frame, cel] : sheet) sheet_list.push_back(Json::array({frame, cel}));
    track_of(anim_of(page, false), folder_id)["cels"] = std::move(sheet_list);
}

void set_camera_key(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    Json& data = anim_of(page, false);
    const std::int64_t frame = frame_of(page, op_get(c.op, "frame"));
    std::vector<Json> keys;
    for (const Json& key : iterate(py_or(py_get(data, "camera"), Json::array()))) {
        if (!py_equals(frame_key(key), Json(frame))) keys.push_back(key);
    }
    const Json rect = op_get(c.op, "rect");
    if (!rect.is_null()) {
        std::vector<double> xywh;
        try {
            xywh = unpack_floats(rect, 4);
        } catch (const PyValueError&) {
            throw OpError("rect is [x, y, width, height] in mm");
        } catch (const PyTypeError&) {
            throw OpError("rect is [x, y, width, height] in mm");
        }
        if (xywh[2] <= 1 || xywh[3] <= 1) throw OpError("rect is [x, y, width, height] in mm");
        Json values = Json::array({xywh[0], xywh[1], xywh[2], xywh[3]});
        require_finite(values, "rect");
        keys.push_back(Json{{"frame", frame}, {"rect", std::move(values)}});
    }
    data["camera"] = sorted_items(keys, frame_key);
}

void set_light_table(OpContext& c) {
    const std::size_t at = require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    Json& data = anim_of(page, false);
    Json cels = Json::array();
    for (const Json& value : iterate(py_or(op_get(c.op, "cels"), Json::array()))) cels.push_back(py_str(value));
    for (const Json& cel : cels) {
        const std::string& id = cel.get_ref<const std::string&>();
        if (std::none_of(page.layers.begin(), page.layers.end(), [&](const Layer& layer) { return layer.id == id; })) {
            throw OpError("no layer " + id);
        }
    }
    data["light_table"] = std::move(cels);
}

}  // namespace

void register_anim_ops(OpRegistry& registry) {
    registry.add("set_animation", set_animation);
    registry.add("add_anim_folder", add_anim_folder);
    registry.add("add_cel", add_cel);
    registry.add("set_exposure", set_exposure);
    registry.add("set_exposures", set_exposures);
    registry.add("set_camera_key", set_camera_key);
    registry.add("set_light_table", set_light_table);
}

}  // namespace genko::core
