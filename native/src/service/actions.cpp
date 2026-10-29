// オートアクション, kept apart from the windows (PC5 I-0; Python's genko/app/actions.py).

#include "service/actions.hpp"

#include <QDateTime>
#include <QString>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>

#include "core/config_dir.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
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
const std::vector<std::string> kNotRecorded{"undo", "set_meta", "set_bible", "add_page", "delete_page", "duplicate_page", "reorder", "set_page_spec"};

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

std::optional<std::string> save(const Json& all) {
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    try {
        storage::write_atomic(path(), core::dump(all, options));
    } catch (const std::exception& error) {
        return QStringLiteral("オートアクションを書けませんでした（%1）").arg(wording::error(std::string(error.what()))).toStdString();
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
        if (!in(kNotRecorded, name) && !gate_op(name)) out.push_back(op);
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
