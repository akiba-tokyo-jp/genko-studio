// The ops of M3-A that draw nothing (Python's layerops.group_layers, move_layers and set_paper, fileops.set_timelapse,
// and ops._apply_one's store_area and forget_area): layers put in a folder or moved together, the paper's colour (用紙色),
// the timelapse switch (タイムラプス: the recording itself belongs to saving) and areas kept on the page (選択範囲をストック).

#include <algorithm>
#include <set>
#include <utility>

#include "core/areas.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// layerops._ids: [str(v) for v in op.get("ids") or []], at least one.
std::vector<std::string> layer_ids(const Json& op) {
    std::vector<std::string> ids;
    const Json* given = get(op, "ids");
    if (given != nullptr && py_truthy(*given)) {
        for (const Json& v : iterate(*given)) ids.push_back(py_str(v));
    }
    if (ids.empty()) throw OpError("ids is the list of layer ids");
    return ids;
}

// layerops._layers: whether each layer of the page is one named (OpError "no layer <id>" for the first missing).
std::vector<bool> layers_named(const Page& page, const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
        const bool found = std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (!found) throw OpError("no layer " + id);
    }
    const std::set<std::string> wanted(ids.begin(), ids.end());
    std::vector<bool> out;
    for (const Layer& layer : page.layers) out.push_back(wanted.contains(layer.id));
    return out;
}

bool same_id(const Json& value, const std::string& id) { return value.is_string() && value.get_ref<const std::string&>() == id; }

// The position of the first layer whose id is `value` (none for none; a value that is not a str names none).
std::optional<std::size_t> first_with_id(const std::vector<Layer>& layers, const Json& value) {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (same_id(value, layers[i].id)) return i;
    }
    return std::nullopt;
}

// layerops._inside: whether a folder the layer `folder` is in (any level up) is one of `ids`. ({item.id: item}: the
// last layer with an id; a parent that is not a str names no layer.)
bool inside(const Page& page, const Layer& folder, const std::set<std::string>& ids) {
    const auto by_id = [&page](const Json& id) -> const Layer* {
        if (!id.is_string()) return nullptr;
        const Layer* found = nullptr;
        for (const Layer& layer : page.layers) {
            if (layer.id == id.get_ref<const std::string&>()) found = &layer;
        }
        return found;
    };
    const Layer* parent = by_id(folder.parent_id);
    std::set<std::string> seen;
    while (parent != nullptr && !seen.contains(parent->id)) {
        if (ids.contains(parent->id)) return true;
        seen.insert(parent->id);
        parent = by_id(parent->parent_id);
    }
    return false;
}

void group_layers(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const std::vector<std::string> ids = layer_ids(op);
    Page& page = doc.edit_page(at);
    const std::vector<bool> chosen = layers_named(page, ids);
    std::size_t last = 0;  // the topmost of them
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        if (chosen[i]) last = i;
    }
    Layer folder;
    folder.id = truthy_at(op, "id") ? py_str(op["id"]) : new_id();
    folder.role = LayerRole::User;
    folder.kind = LayerKind::Folder;
    folder.title = truthy_at(op, "name") ? py_str(op["name"]) : std::string("フォルダー");
    folder.exportable = true;
    folder.parent_id = page.layers[last].parent_id;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == folder.id) throw OpError("layer " + folder.id + " exists");
    }
    // Check the actual new folder's destination ancestry, not every ancestry pair among the selection.
    // A topmost selected child may put the new folder inside a folder being moved (a cycle). A topmost
    // selected root layer keeps it outside and permits the same ancestor+child selection as Python.
    std::set<std::string> moving;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        if (chosen[i]) moving.insert(page.layers[i].id);
    }
    if (inside(page, folder, moving)) throw OpError("a folder cannot be grouped with a layer in it");
    // the folder where the topmost of them was, and they (in its folder now) just under it, in their order
    std::vector<Layer> out;
    std::vector<Layer> moved;
    out.reserve(page.layers.size() + 1);
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (chosen[i]) {
            Layer layer = std::move(page.layers[i]);
            layer.parent_id = folder.id;
            moved.push_back(std::move(layer));
        } else {
            out.push_back(std::move(page.layers[i]));
        }
        if (i == last) {
            for (Layer& layer : moved) out.push_back(std::move(layer));
            out.push_back(std::move(folder));
        }
    }
    page.layers = std::move(out);
}

void move_layers(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const std::vector<std::string> ids = layer_ids(op);
    Page& page = doc.edit_page(at);
    const std::vector<bool> chosen = layers_named(page, ids);
    const Json* parent = get(op, "parent");
    const bool into = parent != nullptr && py_truthy(*parent);
    if (parent != nullptr) {
        if (into) {
            const auto folder = first_with_id(page.layers, *parent);
            if (!folder || page.layers[*folder].kind != LayerKind::Folder) throw OpError("parent must be a folder");
            std::set<std::string> moving;
            for (std::size_t i = 0; i < chosen.size(); ++i) {
                if (chosen[i]) moving.insert(page.layers[i].id);
            }
            if (chosen[*folder] || inside(page, page.layers[*folder], moving)) throw OpError("a folder cannot hold itself");
        }
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            if (chosen[i]) page.layers[i].parent_id = into ? Json(py_str(*parent)) : Json(nullptr);
        }
    }
    Json after = get_or(op, "after", Json(nullptr));
    if (after.is_null() && into) {
        // into a folder: just above its layers (the last of them that is not moved), else just under the folder
        const std::size_t folder = *first_with_id(page.layers, *parent);
        std::optional<std::size_t> last_kid;
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (same_id(page.layers[i].parent_id, page.layers[folder].id) && !chosen[i]) last_kid = i;
        }
        after = last_kid ? Json(page.layers[*last_kid].id) : Json("__before__" + page.layers[folder].id);
    }
    if (after.is_null()) return;
    std::vector<Layer> moved;
    std::vector<Layer> rest;
    for (std::size_t i = 0; i < page.layers.size(); ++i) (chosen[i] ? moved : rest).push_back(std::move(page.layers[i]));
    std::size_t place = 0;
    const std::string after_text = py_str(after);
    if (after == Json("bottom")) {
        place = 0;
    } else if (after_text.starts_with("__before__")) {
        const auto found = first_with_id(rest, Json(after_text.substr(10)));
        if (!found) throw PyUncaught("StopIteration", "");  // (Python's next() without a default)
        place = *found;
    } else {
        const auto anchor = first_with_id(rest, after);
        if (!anchor) throw OpError("no layer " + after_text);
        place = *anchor + 1;
    }
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(place), std::make_move_iterator(moved.begin()),
                std::make_move_iterator(moved.end()));
    page.layers = std::move(rest);
}

void set_paper(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    std::optional<std::vector<std::int64_t>> value;
    if (truthy_at(op, "rgb")) {
        std::vector<std::int64_t> rgb = int_tuple(op["rgb"]);  // (every value converted, the first three kept)
        if (rgb.size() > 3) rgb.resize(3);
        value = std::move(rgb);
    }
    if (value && (value->size() != 3 || std::any_of(value->begin(), value->end(), [](std::int64_t v) { return v < 0 || v > 255; }))) {
        throw OpError("rgb is [r, g, b], each 0..255");
    }
    // op.get("page") in (None, "", 0): every page (each one's lock asked before, in check_page_lock)
    const Json* page = get(op, "page");
    const bool every = paper_on_every_page(op);
    std::vector<std::size_t> pages;
    if (every) {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) pages.push_back(i);
    } else if (!doc.pages.empty()) {
        // (int(op["page"]) for each page; a number past 64 bits is a page no book has)
        if (!py_big_int_text(*page)) {
            const Num index(to_int(*page));
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                if (doc.pages[i]->index == index) pages.push_back(i);
            }
        }
    }
    if (pages.empty()) throw OpError("page not found");
    for (const std::size_t i : pages) {
        Json& extra = doc.edit_page(i).extra;
        if (!value) {
            extra.erase("paper_rgb");
        } else {
            extra["paper_rgb"] = ints_json(*value);
        }
    }
}

void set_timelapse(OpContext& c) {
    if (py_truthy(get_or(c.op, "on", Json(true)))) {
        c.doc.extra["timelapse"] = Json::object({{"on", true}});
    } else {
        c.doc.extra.erase("timelapse");
    }
}

// page.extra["saved_areas"] as dict(…) makes it ({} when it is missing or empty)
Json saved_areas(const Page& page) {
    const Json* saved = get(page.extra, "saved_areas");
    return saved != nullptr && py_truthy(*saved) ? py_dict(*saved) : Json::object();
}

void store_area(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    const std::string label = truthy_at(op, "name") ? py_strip(py_str(op["name"])) : std::string();
    if (label.empty()) throw OpError("name is required");
    Json saved = saved_areas(doc.page(at));
    saved[label] = op_area(op);
    doc.edit_page(at).extra["saved_areas"] = std::move(saved);
}

void forget_area(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = require_page(doc, op);
    Json saved = saved_areas(doc.page(at));
    const std::string label = truthy_at(op, "name") ? py_str(op["name"]) : std::string();
    const auto it = saved.find(label);
    // (saved.pop(name, None) is None: a kept null is "not there" too)
    if (it == saved.end() || it->is_null()) throw OpError("no saved area " + py_str(get_or(op, "name", Json(nullptr))));
    saved.erase(it);
    doc.edit_page(at).extra["saved_areas"] = std::move(saved);
}

}  // namespace

void register_arrange_ops(OpRegistry& registry) {
    registry.add("group_layers", group_layers);
    registry.add("move_layers", move_layers);
    registry.add("set_paper", set_paper);
    registry.add("set_timelapse", set_timelapse);
    registry.add("store_area", store_area);
    registry.add("forget_area", forget_area);
}

}  // namespace genko::core
