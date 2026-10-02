// The page ops of M2 (Python's ops._apply_one): duplicate_page, reorder and advance; and set_brush (the book's pen).

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// A dict of old panel id → new id (a repeated id keeps its first place and its last value).
using IdMap = std::vector<std::pair<std::string, std::string>>;

void map_set(IdMap& map, const std::string& from, const std::string& to) {
    for (auto& [k, v] : map) {
        if (k == from) {
            v = to;
            return;
        }
    }
    map.emplace_back(from, to);
}

const std::string* map_get(const IdMap& map, const std::string& from) {
    for (const auto& [k, v] : map) {
        if (k == from) return &v;
    }
    return nullptr;
}

// ops._refresh_frame_ids: new ids for the panel and every panel under it (the panel first).
void refresh_frame_ids(Frame& frame, IdMap& map) {
    const std::string old = frame.id;
    frame.id = new_id();
    map_set(map, old, frame.id);
    for (Frame& child : frame.children) refresh_frame_ids(child, map);
}

// Python's list.insert(i, x)
void list_insert(std::vector<Num>& list, std::int64_t at, const Num& value) {
    const auto size = static_cast<std::int64_t>(list.size());
    if (at < 0) at = std::max<std::int64_t>(0, at + size);
    if (at > size) at = size;
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), value);
}

void duplicate_page(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Num source_index = doc.page(i).index;
    Page clone = doc.page(i);
    clone.index = Num(static_cast<std::int64_t>(doc.pages.size()) + 1);
    clone.id = "pg_" + new_id();
    clone.spread_with.reset();
    IdMap frame_map;
    for (Frame& frame : clone.frames) refresh_frame_ids(frame, frame_map);
    if (py_truthy(clone.selected_frame_id)) {
        require_hashable(clone.selected_frame_id);
        const std::string* mapped =
            clone.selected_frame_id.is_string() ? map_get(frame_map, clone.selected_frame_id.get<std::string>()) : nullptr;
        clone.selected_frame_id = mapped != nullptr ? Json(*mapped) : Json(nullptr);
    }
    for (Layer& layer : clone.layers) {
        layer.id = new_id();
        if (layer.frame_id && !layer.frame_id->empty()) {
            if (const std::string* mapped = map_get(frame_map, *layer.frame_id)) layer.frame_id = *mapped;
        }
    }
    std::vector<StoryLine> new_lines;
    for (const StoryLine& line : doc.story) {
        if (!(line.page_index == source_index)) continue;
        StoryLine copied = line;
        copied.id = new_id();
        copied.page_index = clone.index;
        if (copied.frame_id && !copied.frame_id->empty()) {
            if (const std::string* mapped = map_get(frame_map, *copied.frame_id)) copied.frame_id = *mapped;
        }
        new_lines.push_back(std::move(copied));
    }
    const Num clone_index = clone.index;
    doc.story.insert(doc.story.end(), new_lines.begin(), new_lines.end());
    doc.pages.push_back(std::make_shared<Page>(std::move(clone)));
    const Json* next_to = get(c.op, "next_to");
    if (next_to != nullptr && py_truthy(*next_to) &&
        source_index < Num(static_cast<std::int64_t>(doc.pages.size()) - 1)) {
        std::vector<Num> order;
        for (std::size_t k = 0; k + 1 < doc.pages.size(); ++k) order.push_back(doc.pages[k]->index);
        if (!source_index.is_int()) {
            throw PyTypeError("'float' object cannot be interpreted as an integer");
        }
        list_insert(order, source_index.int_value(), clone_index);
        reorder_pages(doc, order);
    }
}

void reorder(OpContext& c) {
    Document& doc = c.doc;
    const Json* order_value = get(c.op, "order");
    if (order_value == nullptr || !order_value->is_array() || order_value->empty()) {
        throw OpError("order must be a non-empty list of page indexes");
    }
    std::vector<Num> order;
    for (const Json& v : *order_value) order.push_back(Num(to_int(v)));
    std::vector<Num> wanted = order;
    std::vector<Num> have;
    for (const auto& page : doc.pages) have.push_back(page->index);
    const auto less = [](const Num& a, const Num& b) { return a < b; };
    std::stable_sort(wanted.begin(), wanted.end(), less);
    std::stable_sort(have.begin(), have.end(), less);
    bool same = wanted.size() == have.size();
    for (std::size_t k = 0; same && k < wanted.size(); ++k) same = wanted[k] == have[k];
    if (!same) throw OpError("order must list every page exactly once");
    reorder_pages(doc, order);
}

void advance(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    const Json* to_value = get(c.op, "to");
    const std::string to = to_value != nullptr && to_value->is_string() ? to_value->get<std::string>() : "";
    if (to != "name" && to != "ink" && to != "finish") throw OpError("to must be name, ink, or finish");
    // pipeline.advance: ink starts only after the name gate
    if (to == "ink" && !doc.page(i).name_ok) throw OpError("name is not OK");
    if (doc.page(i).stage != to) doc.edit_page(i).stage = to;
}

void set_brush(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    if (truthy_at(op, "rgb")) doc.brush_rgb = int_tuple(op["rgb"]);
    if (const Json* width = get(op, "width_mm"); width != nullptr && !width->is_null()) doc.brush_width_mm = to_float(*width);
    if (has(op, "stabilize")) doc.brush_stabilize = py_truthy(op["stabilize"]) ? to_int(op["stabilize"]) : 0;
    if (has(op, "taper")) doc.brush_taper = py_truthy(op["taper"]);
    if (truthy_at(op, "curve")) doc.brush_curve = py_str(op["curve"]);
}

}  // namespace

void register_page_ops(OpRegistry& registry) {
    registry.add("duplicate_page", duplicate_page);
    registry.add("reorder", reorder);
    registry.add("advance", advance);
    registry.add("set_brush", set_brush);
}

}  // namespace genko::core
