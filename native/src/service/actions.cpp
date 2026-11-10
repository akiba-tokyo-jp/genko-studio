// オートアクション, kept apart from the windows (PC5 I-0; Python's genko/app/actions.py).

#include "service/actions.hpp"

#include <QDateTime>
#include <QString>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

#include "core/config_dir.hpp"
#include "core/ids.hpp"
#include "core/json_depth.hpp"
#include "core/pyconv.hpp"
#include "render/ops_registry.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::actions {

using core::Json;

namespace {

// ops that make something with an "id" of its own
const std::vector<std::string> kMakers{"add_layer", "add_tone", "add_line", "add_effect", "add_prim3d", "add_scene", "add_mannequin", "add_ruler",
                                       "stamp_material", "duplicate_layer"};
// ops whose "id" names the layer they change: played back, the layer being drawn on
const std::vector<std::string> kLayerOps{"set_layer", "set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer", "filter_raster"};
// ops that pick an existing line by its place in a layer: only right on the page they were recorded on
const std::vector<std::string> kBoundOps{"delete_stroke", "reshape_stroke", "edit_stroke", "set_stroke_width", "simplify_stroke", "edit_line", "move_line",
                                         "delete_line", "set_balloon_path", "reorder_lines"};
// the gates and the locks (GATE_OPS and the page locks): a person's own, never done again by a recording or a script
const std::vector<std::string> kGateOps{"approve", "revoke", "allow_chat_approval", "lock_page", "unlock_page", "name_ok", "advance"};
// what a recording leaves out besides the gates: undo, the book's settings and its pages
// (PC5 J-g: the pages an artboard makes — I-c's add_artboard, artboard_from_layers — as add_page; the book's variables and
// data sets — I-d's set_variables, set_data_sets — as set_bible; the questions, which change nothing — slices_info,
// measure_info, measure_values, type_info)
const std::vector<std::string> kNotRecorded{"undo", "set_meta", "set_bible", "add_page", "delete_page", "duplicate_page", "reorder", "set_page_spec",
                                            "add_artboard", "artboard_from_layers", "set_variables", "set_data_sets", "slices_info", "measure_info",
                                            "measure_values", "type_info", "timeline_info"};
// (PC5 J-g) the count's marks and groups named by their number on the page (J-d1's remove_count, move_count, set_count_group,
// delete_count_group with "group" or "index"): the recorded page's; a mark added, every mark cleared, a group made, the
// scale, the ruler's line and the scale marker (place_scale_marker) are any page's
const std::vector<std::string> kCountOps{"remove_count", "move_count", "set_count_group", "delete_count_group"};
// (PC5 J-g) a video layer named by its folder (J-e2's video_insert_blank … rasterize_video with "folder"): the recorded page's;
// a video layer made (add_video_layer) and a footage's frames made layers (footage_to_layers) are any page's
const std::vector<std::string> kVideoOps{"video_insert_blank", "video_duplicate", "video_delete", "video_restore", "video_restore_all",
                                         "video_alter", "set_video_layer", "replace_footage", "rasterize_video"};

bool in(const std::vector<std::string>& list, std::string_view name) { return std::find(list.begin(), list.end(), name) != list.end(); }

std::string op_name(const Json& op) {
    const auto it = op.find("op");
    return it != op.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// op.get(key) as a truthy string: its text (Python's str(op["id"]) when op.get("id") is truthy)
std::optional<std::string> text_of(const Json& op, const char* key) {
    const auto it = op.find(key);
    if (it == op.end() || !core::py_truthy(*it)) return std::nullopt;
    return it->is_string() ? it->get<std::string>() : core::py_str(*it);
}

std::vector<std::string> made_ids(const Json& ops) {
    std::vector<std::string> out;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        if (in(kMakers, name) && name != "duplicate_layer") {
            if (const auto id = text_of(op, "id")) out.push_back(*id);
        }
        if (name == "duplicate_layer") {
            if (const auto id = text_of(op, "new_id")) out.push_back(*id);
        }
    }
    return out;
}

}  // namespace

std::filesystem::path path() { return core::config_dir() / "actions.json"; }

Json load() {
    Json data;
    try {
        std::ifstream in(path(), std::ios::binary);
        if (!in) return Json::object();
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (core::too_deep(text)) return Json::object();  // (PC6, the review's P3-2: deeper than any reader of it goes, as not JSON)
        data = Json::parse(text);
    } catch (const std::exception&) {
        return Json::object();
    }
    Json out = Json::object();
    if (!data.is_object()) return out;
    for (const auto& [name, value] : data.items()) {
        if (value.is_object() && value.contains("ops") && value["ops"].is_array()) out[name] = value;
    }
    return out;
}

// (PC6, the review of pc6-A's P3-6) Is the file there one that load() could not read — too deep, not JSON, not an object?
// Read as no actions, it is kept aside before a save writes over it: what it held is not lost.
static bool unreadable_there() {
    std::ifstream in(path(), std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (core::too_deep(text)) return true;
    const Json data = Json::parse(text, nullptr, false);
    return data.is_discarded() || !data.is_object();
}

std::optional<std::string> save(const Json& all) {
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    if (unreadable_there()) {  // (actions.json.unreadable-<when>: the file as it was, beside the new one)
        const std::string when = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")).toStdString();
        std::filesystem::path aside = path();
        aside += ".unreadable-" + when;
        std::error_code ec;
        std::filesystem::rename(path(), aside, ec);
        if (ec) {
            return QStringLiteral("読めない actions.json を別の名前に退けられないので、アクションを書きませんでした（%1）")
                .arg(QString::fromStdString(ec.message()))
                .toStdString();
        }
    }
    try {
        storage::write_atomic(path(), core::dump(all, options));
    } catch (const std::exception& error) {
        return QStringLiteral("アクションを書けませんでした（%1）").arg(wording::error(std::string(error.what()))).toStdString();
    }
    return std::nullopt;
}

std::optional<std::string> store(const std::string& name, const Json& ops, const std::string& made) {
    Json all = load();
    all[name] = Json{{"ops", ops},
                     {"made", made.empty() ? QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")).toStdString() : made}};
    return save(all);
}

std::optional<std::string> remove(const std::string& name) {
    Json all = load();
    all.erase(name);
    return save(all);
}

bool gate_op(std::string_view name) { return in(kGateOps, name); }

Json recordable(const Json& ops) {
    Json out = Json::array();
    for (const Json& op : ops) {
        if (!op.is_object()) continue;
        const std::string name = op_name(op);
        // (PC5 J-g: nor a question — an op added as one, or type_style and replace_fonts when they only ask)
        if (!in(kNotRecorded, name) && !gate_op(name) && !render::ops_registry().is_query(op)) out.push_back(op);
    }
    return out;
}

std::vector<bool> page_bound(const Json& ops) {
    const std::vector<std::string> made = made_ids(ops);
    std::vector<bool> out;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        bool bound = false;
        if (const auto id = text_of(op, "id"); id && !in(kMakers, name) && !in(kLayerOps, name) && !in(made, *id)) bound = true;
        for (const char* key : {"ids", "stroke_id", "stroke_ids", "line_id"}) {
            if (text_of(op, key)) bound = true;
        }
        if (in(kBoundOps, name)) bound = true;
        // (PC5 J-a, layer-comps@1: a layer comp named by its id or name is one of the recorded page's — "next", "previous",
        // "last_state", every comp ("*") and the comp applied last are any page's)
        if (name.find("layer_comp") != std::string::npos) {
            if (const auto comp = op.find("comp"); comp != op.end() && !comp->is_null()) {
                const bool any = comp->is_string() && in({"next", "previous", "last_state", "*"}, comp->get<std::string>());
                if (!any) bound = true;
            }
        }
        // (PC5 J-b, slices@1: a slice named by its id or number, the slices listed, a layer-based slice's layer — the recorded
        // page's; a slice drawn, every slice ("all") and the slices from the guides are any page's)
        if (in({"add_slice", "set_slice", "delete_slices", "divide_slice"}, name)) {
            for (const char* key : {"slice", "number", "layer"})
                if (const auto it = op.find(key); it != op.end() && !it->is_null()) bound = true;
            if (const auto it = op.find("slices"); it != op.end() && it->is_array()) bound = true;
        }
        if (in(kVideoOps, name)) {
            if (const auto it = op.find("folder"); it != op.end() && !it->is_null()) bound = true;
        }
        if (in(kCountOps, name)) {
            for (const char* key : {"group", "index"})
                if (const auto it = op.find(key); it != op.end() && !it->is_null()) bound = true;
        }
        out.push_back(bound);
    }
    return out;
}

Json playable(const Json& ops) {
    const std::vector<bool> bound = page_bound(ops);
    Json out = Json::array();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (!bound[i]) out.push_back(ops[i]);
    }
    return out;
}

Json replay(const Json& recorded, std::optional<Json> page, const std::optional<std::string>& layer_id, const std::optional<std::string>& frame_id,
            const std::function<std::string()>& new_id) {
    const Json ops = playable(recorded);
    std::map<std::string, std::string> made;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        std::optional<std::string> key;
        if (in(kMakers, name) && name != "duplicate_layer") key = text_of(op, "id");
        if (name == "duplicate_layer") key = text_of(op, "new_id");
        if (key && !made.contains(*key)) made[*key] = new_id ? new_id() : core::new_id();
    }
    Json out = Json::array();
    for (Json op : ops) {
        if (op.contains("page") && page) op["page"] = *page;
        for (const char* key : {"id", "new_id", "after", "parent", "parent_id", "clip_to"}) {
            const auto it = op.find(key);
            if (it != op.end() && it->is_string() && made.contains(it->get<std::string>())) {
                *it = made[it->get<std::string>()];
            } else if (std::string_view(key) == "id" && in(kLayerOps, op_name(op)) && text_of(op, "id") && layer_id) {
                op["id"] = *layer_id;  // (the layer it changed then: the one drawn on now)
            }
        }
        if (op.contains("layer_id")) {
            const std::string recorded_layer = op["layer_id"].is_string() ? op["layer_id"].get<std::string>() : core::py_str(op["layer_id"]);
            op["layer_id"] = made.contains(recorded_layer) ? made[recorded_layer] : layer_id ? *layer_id : recorded_layer;
        }
        if (op.contains("frame_id")) {
            if (frame_id) {
                op["frame_id"] = *frame_id;
            } else {
                op.erase("frame_id");
            }
        }
        out.push_back(std::move(op));
    }
    return out;
}

}  // namespace genko::service::actions
