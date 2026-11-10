#include "render/layer_align.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

#include "core/areas.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/snap.hpp"
#include "render/layer_transform.hpp"
#include "render/selection.hpp"

// PhotoCraft v0.2.0 crates/engine/src/layer_multi_cmds.rs (with_links, top_level, align, distribute, dedup_moves,
// link_layers, reverse) and commands.rs (arrange), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::layer_align {

using core::Json;
using core::Layer;
using core::OpError;
using core::Page;

namespace {

bool vertical(Edge e) { return e == Edge::Top || e == Edge::VMiddle || e == Edge::Bottom; }

// The edge's place doubled (a middle stays whole on whole pixels), in working pixels.
double value2(Edge e, const Box& b) {
    const double a = vertical(e) ? b[1] : b[0], c = vertical(e) ? b[3] : b[2];
    switch (e) {
        case Edge::Top:
        case Edge::Left: return 2 * a;
        case Edge::Bottom:
        case Edge::Right: return 2 * c;
        default: return a + c;
    }
}

// The item's box in working pixels (a layer's on whole pixels: the noise of the mm taken off).
Box in_px(const Item& item, const Grid& g) {
    Box b{};
    for (std::size_t i = 0; i < 4; ++i) b[i] = item.whole ? std::round(item.box[i] * g[i % 2]) : item.box[i] * g[i % 2];
    return b;
}

std::array<double, 2> along(Edge e, double d_px, const Grid& g) {
    return vertical(e) ? std::array<double, 2>{0, d_px / g[1]} : std::array<double, 2>{d_px / g[0], 0};
}

std::string parent_of(const Layer& layer) { return layer.parent_id.is_string() ? layer.parent_id.get<std::string>() : std::string(); }

std::optional<std::size_t> index_of(const std::vector<Layer>& layers, const std::string& id) {
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == id) return i;
    return std::nullopt;
}

// The layer and every layer inside it (a folder), page order.
std::vector<std::size_t> block_of(const std::vector<Layer>& layers, const std::string& id) {
    std::set<std::string> held{id};
    for (bool more = true; more;) {
        more = false;
        for (const Layer& layer : layers) {
            if (held.contains(layer.id) || !layer.parent_id.is_string() || !held.contains(layer.parent_id.get<std::string>())) continue;
            held.insert(layer.id);
            more = true;
        }
    }
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (held.contains(layers[i].id)) out.push_back(i);
    return out;
}

// The layer's block taken out and put back in what is left: over the whole block of the layer `anchor` (after = true) or
// under it.
void move_block(std::vector<Layer>& layers, const std::string& id, const std::string& anchor, bool after) {
    const std::vector<std::size_t> block = block_of(layers, id);
    std::vector<Layer> taken, rest;
    std::size_t k = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (k < block.size() && block[k] == i) {
            taken.push_back(std::move(layers[i]));
            ++k;
        } else {
            rest.push_back(std::move(layers[i]));
        }
    }
    const std::vector<std::size_t> there = block_of(rest, anchor);
    const std::size_t place = after ? there.back() + 1 : there.front();
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(place), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
    layers = std::move(rest);
}

// The other siblings of the layer (its folder's layers, or the page's outside any folder), page order.
std::vector<std::string> siblings_of(const std::vector<Layer>& layers, const std::string& id) {
    const std::string parent = parent_of(layers[*index_of(layers, id)]);
    std::vector<std::string> out;
    for (const Layer& layer : layers)
        if (layer.id != id && parent_of(layer) == parent) out.push_back(layer.id);
    return out;
}

// Where a layer's block reaches up to and down to (a folder's layers may lie over it or under it in the page's order).
std::size_t top_of(const std::vector<Layer>& layers, const std::string& id) { return block_of(layers, id).back(); }
std::size_t bottom_of(const std::vector<Layer>& layers, const std::string& id) { return block_of(layers, id).front(); }

std::vector<std::string> order_of(const std::vector<Layer>& layers) {
    std::vector<std::string> out;
    for (const Layer& layer : layers) out.push_back(layer.id);
    return out;
}

// --- the ops' keys ------------------------------------------------------------------------------------------------------

std::vector<std::string> ids_of(const Json& op, bool lines_given) {
    const Json* ids = core::get(op, "ids");
    std::vector<std::string> out;
    if (ids == nullptr || ids->is_null()) {
        if (const Json* id = core::get(op, "id"); id != nullptr && id->is_string()) return {id->get<std::string>()};
        if (lines_given) return out;
        throw OpError("ids is [layer id, …]");
    }
    if (!ids->is_array() || (ids->empty() && !lines_given)) throw OpError("ids is [layer id, …]");
    for (const Json& id : *ids) {
        if (!id.is_string()) throw OpError("ids is [layer id, …]");
        if (std::find(out.begin(), out.end(), id.get<std::string>()) == out.end()) out.push_back(id.get<std::string>());
    }
    return out;
}

const Layer& layer_of(const Page& page, const std::string& id) {
    for (const Layer& layer : page.layers)
        if (layer.id == id) return layer;
    throw core::OpKeyError(core::py_repr_str(id));
}

// The lines named (on this page), their places in the story.
std::vector<std::size_t> lines_of(const core::Document& doc, const Page& page, const Json& op) {
    std::vector<std::size_t> out;
    const Json* lines = core::get(op, "lines");
    if (lines == nullptr || lines->is_null()) return out;
    if (!lines->is_array()) throw OpError("lines is [line id, …]");
    for (const Json& id : *lines) {
        if (!id.is_string()) throw OpError("lines is [line id, …]");
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < doc.story.size(); ++i) {
            if (doc.story[i].id == id.get<std::string>() && core::py_equals(doc.story[i].page_index.json(), page.index.json())) found = i;
        }
        if (!found) throw core::OpKeyError(core::py_repr_str(id.get<std::string>()));
        if (std::find(out.begin(), out.end(), *found) == out.end()) out.push_back(*found);
    }
    return out;
}

bool lines_named(const Json& op) {
    const Json* lines = core::get(op, "lines");
    return lines != nullptr && lines->is_array() && !lines->empty();
}

// What align and distribute work on: the layers (top level) and lines with a box.
struct Things {
    std::vector<std::string> layers;  // (each item's layer, or "" for a line)
    std::vector<std::size_t> lines;   // (each item's line, for a line)
    std::vector<Item> items;
};

Things things_of(const core::Document& doc, const Page& page, const Json& op) {
    Things t;
    const std::vector<std::string> ids = top_level(page, ids_of(op, lines_named(op)));
    for (const std::string& id : ids) {
        const auto box = layer_bounds::of_layer(doc, page, id);
        if (!box) continue;  // (a layer that shows nothing, a fill or adjustment layer: not aligned)
        t.layers.push_back(id);
        t.lines.push_back(0);
        t.items.push_back(Item{*box, true});
    }
    for (const std::size_t i : lines_of(doc, page, op)) {
        t.layers.push_back(std::string());
        t.lines.push_back(i);
        t.items.push_back(Item{layer_bounds::of_line(doc.story[i]), false});
    }
    return t;
}

// The items moved: each layer with its folder's layers and its linked ones (the first move of a layer wins), each line.
Json apply_moves(core::Document& doc, std::size_t at, const Things& t, const std::vector<std::array<double, 2>>& moves) {
    Json moved = Json::array(), lines = Json::array();
    std::vector<std::pair<std::string, std::array<double, 2>>> layer_moves;
    std::set<std::string> taken;
    for (std::size_t i = 0; i < t.items.size(); ++i) {
        if (t.layers[i].empty()) continue;
        const auto& d = moves[i];
        if (std::fabs(d[0]) < 1e-9 && std::fabs(d[1]) < 1e-9) continue;
        for (const std::string& id : layer_transform::layers_moved(doc.page(at), {t.layers[i]})) {  // (OpError: a lock)
            if (taken.insert(id).second) layer_moves.emplace_back(id, d);
        }
    }
    for (const auto& [id, d] : layer_moves) {
        const core::Rect paper = doc.page(at).paper_rect_mm();  // (an affine map's box is not used)
        const auto m = layer_transform::affine_map({1, 0, 0, 1, d[0], d[1]}, Box{0, 0, paper.width.value(), paper.height.value()});
        layer_transform::move_layer(doc, at, id, m, std::nullopt, resample::Interp::Bicubic);
        moved.push_back(id);
    }
    for (std::size_t i = 0; i < t.items.size(); ++i) {
        if (!t.layers[i].empty()) continue;
        const auto& d = moves[i];
        if (std::fabs(d[0]) < 1e-9 && std::fabs(d[1]) < 1e-9) continue;
        core::StoryLine& line = doc.story[t.lines[i]];
        layer_transform::move_line(line, layer_transform::affine_map({1, 0, 0, 1, d[0], d[1]}, t.items[i].box));
        lines.push_back(line.id);
    }
    return Json{{"moved", moved}, {"lines", lines}};
}

double r4(double v) { return core::py_round(v, 4); }

Json box_json(const Box& b) { return Json::array({r4(b[0]), r4(b[1]), r4(b[2] - b[0]), r4(b[3] - b[1])}); }

}  // namespace

// --- the arithmetic -----------------------------------------------------------------------------------------------------

std::optional<Edge> edge_from(std::string_view name) {
    if (name == "top") return Edge::Top;
    if (name == "vcenter") return Edge::VMiddle;
    if (name == "bottom") return Edge::Bottom;
    if (name == "left") return Edge::Left;
    if (name == "hcenter") return Edge::HMiddle;
    if (name == "right") return Edge::Right;
    return std::nullopt;
}

std::vector<std::array<double, 2>> align_moves(const std::vector<Item>& items, Edge edge, const Box& target, const Grid& grid) {
    Box t{};
    for (std::size_t i = 0; i < 4; ++i) t[i] = target[i] * grid[i % 2];
    const double t2 = value2(edge, t);
    std::vector<std::array<double, 2>> out;
    for (const Item& item : items) {
        const double diff = (t2 - value2(edge, in_px(item, grid))) / 2;
        // (a layer by whole pixels, half a pixel up or left: PhotoCraft's div_euclid; a line exactly)
        out.push_back(along(edge, item.whole ? std::ceil(diff - 0.5 - 1e-6) : diff, grid));
    }
    return out;
}

std::vector<std::array<double, 2>> distribute_moves(const std::vector<Item>& items, std::string_view by, const Grid& grid) {
    const std::size_t n = items.size();
    std::vector<std::array<double, 2>> out(n, std::array<double, 2>{0, 0});
    if (n < 3) return out;
    std::vector<Box> px;
    for (const Item& item : items) px.push_back(in_px(item, grid));
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    if (by == "horizontal" || by == "vertical") {  // (equal gaps between them; the outermost two stay)
        const bool v = by == "vertical";
        const auto lo = [&](std::size_t i) { return v ? px[i][1] : px[i][0]; };
        const auto hi = [&](std::size_t i) { return v ? px[i][3] : px[i][2]; };
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return lo(a) + hi(a) < lo(b) + hi(b) || (lo(a) + hi(a) == lo(b) + hi(b) && lo(a) < lo(b));
        });
        const double span = hi(order.back()) - lo(order.front());
        double total = 0;
        for (std::size_t i = 0; i < n; ++i) total += hi(i) - lo(i);
        const double gap = (span - total) / static_cast<double>(n - 1);
        double cursor = lo(order.front());
        for (const std::size_t i : order) {
            const double want = items[i].whole ? std::round(cursor) : cursor;
            const double d = want - lo(i);
            out[i] = v ? std::array<double, 2>{0, d / grid[1]} : std::array<double, 2>{d / grid[0], 0};
            cursor += hi(i) - lo(i) + gap;
        }
        return out;
    }
    const auto edge = edge_from(by);
    if (!edge) return out;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return value2(*edge, px[a]) < value2(*edge, px[b]); });
    const double first = value2(*edge, px[order.front()]), last = value2(*edge, px[order.back()]);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = order[k];
        const double want2 = first + (last - first) * static_cast<double>(k) / static_cast<double>(n - 1);
        const double diff = (want2 - value2(*edge, px[i])) / 2;
        out[i] = along(*edge, items[i].whole ? std::round(diff) : diff, grid);
    }
    return out;
}

std::vector<std::string> top_level(const Page& page, const std::vector<std::string>& ids) {
    std::set<std::string> named(ids.begin(), ids.end());
    std::map<std::string, const Layer*> by_id;
    for (const Layer& layer : page.layers) by_id[layer.id] = &layer;
    for (const std::string& id : ids)
        if (!by_id.contains(id)) throw core::OpKeyError(core::py_repr_str(id));
    std::vector<std::string> out;
    for (const Layer& layer : page.layers) {
        if (!named.contains(layer.id)) continue;
        bool carried = false;  // (a folder it is in, at any depth, named too)
        std::set<std::string> seen{layer.id};
        for (const Layer* up = &layer; up->parent_id.is_string();) {
            const auto found = by_id.find(up->parent_id.get<std::string>());
            if (found == by_id.end() || !seen.insert(found->first).second) break;
            up = found->second;
            if (named.contains(up->id)) carried = true;
        }
        if (!carried) out.push_back(layer.id);
    }
    return out;
}

std::vector<std::string> with_links(const Page& page, const std::vector<std::string>& ids) {
    std::set<std::string> groups;
    for (const Layer& layer : page.layers)
        if (layer.link && std::find(ids.begin(), ids.end(), layer.id) != ids.end()) groups.insert(*layer.link);
    std::vector<std::string> out = ids;
    for (const Layer& layer : page.layers)
        if (layer.link && groups.contains(*layer.link) && std::find(out.begin(), out.end(), layer.id) == out.end()) out.push_back(layer.id);
    return out;
}

bool unlinks(const Page& page, const std::vector<std::string>& ids) {
    if (ids.empty()) return false;
    std::optional<std::string> first;
    bool one = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const Layer& layer = layer_of(page, ids[i]);
        if (i == 0) first = layer.link;
        one = one && layer.link && layer.link == first;
    }
    return first.has_value() && one;
}

bool arrange(std::vector<Layer>& layers, const std::vector<std::string>& ids, std::string_view to) {
    const std::vector<std::string> before = order_of(layers);
    const std::set<std::string> items(ids.begin(), ids.end());
    // (the items by their places now, the bottom first)
    const auto by_place = [&]() {
        std::vector<std::string> out;
        for (const Layer& layer : layers)
            if (items.contains(layer.id)) out.push_back(layer.id);
        return out;
    };
    if (to == "reverse") {
        const std::vector<std::string> list = by_place();
        if (list.size() < 2) return false;
        // each item's block out, the slots kept; then the blocks in again end for end, each taking its slot's folder
        std::vector<std::vector<Layer>> blocks;
        std::vector<Json> parents;
        std::vector<std::size_t> slots;
        std::vector<bool> in_block(layers.size(), false);
        for (const std::string& id : list) {
            const std::vector<std::size_t> b = block_of(layers, id);
            parents.push_back(layers[*index_of(layers, id)].parent_id);
            for (const std::size_t i : b) in_block[i] = true;
        }
        std::vector<Layer> rest;
        for (const std::string& id : list) {  // (each block's slot: how many layers outside the blocks come before it)
            const std::vector<std::size_t> b = block_of(layers, id);
            std::size_t before_it = 0;
            for (std::size_t i = 0; i < b.front(); ++i)
                if (!in_block[i]) ++before_it;
            slots.push_back(before_it);
            std::vector<Layer> taken;
            for (const std::size_t i : b) taken.push_back(layers[i]);
            blocks.push_back(std::move(taken));
        }
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (!in_block[i]) rest.push_back(std::move(layers[i]));
        std::vector<Layer> out;
        std::size_t r = 0;
        for (std::size_t k = 0; k < blocks.size(); ++k) {
            while (r < slots[k] && r < rest.size()) out.push_back(std::move(rest[r++]));
            std::vector<Layer>& block = blocks[blocks.size() - 1 - k];
            const std::string head = list[blocks.size() - 1 - k];
            for (Layer& layer : block) {
                if (layer.id == head) layer.parent_id = parents[k];
                out.push_back(std::move(layer));
            }
        }
        while (r < rest.size()) out.push_back(std::move(rest[r++]));
        layers = std::move(out);
        return order_of(layers) != before;
    }
    if (to == "front" || to == "back") {
        const bool front = to == "front";
        std::vector<std::string> list = by_place();
        if (!front) std::reverse(list.begin(), list.end());  // (front: the bottom first goes up first; back: the top first)
        for (const std::string& id : list) {
            std::optional<std::string> anchor;  // (the sibling reaching highest, or lowest)
            for (const std::string& s : siblings_of(layers, id)) {
                if (!anchor || (front ? top_of(layers, s) > top_of(layers, *anchor) : bottom_of(layers, s) < bottom_of(layers, *anchor))) anchor = s;
            }
            if (!anchor) continue;
            // (already past every other sibling: it stays)
            const bool past = front ? top_of(layers, id) > top_of(layers, *anchor) : bottom_of(layers, id) < bottom_of(layers, *anchor);
            if (!past) move_block(layers, id, *anchor, front);
        }
        return order_of(layers) != before;
    }
    if (to == "forward" || to == "backward") {
        const bool up = to == "forward";
        std::vector<std::string> list = by_place();
        if (up) std::reverse(list.begin(), list.end());  // (the one nearest the way it goes first)
        std::set<std::string> stuck;
        for (const std::string& id : list) {
            const std::size_t at = up ? top_of(layers, id) : bottom_of(layers, id);
            std::optional<std::pair<std::size_t, std::string>> next;  // (the nearest sibling that way)
            for (const std::string& s : siblings_of(layers, id)) {
                const std::size_t i = up ? top_of(layers, s) : bottom_of(layers, s);
                if (up ? (i > at && (!next || i < next->first)) : (i < at && (!next || i > next->first))) next = std::make_pair(i, s);
            }
            if (!next || stuck.contains(next->second)) {
                stuck.insert(id);
                continue;
            }
            move_block(layers, id, next->second, up);
        }
        return order_of(layers) != before;
    }
    throw OpError("to is front, forward, backward, back or reverse");
}

// --- the ops -----------------------------------------------------------------------------------------------------------

void align_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Page& page = doc.page(at);
    const Json* edge_value = core::get(op, "edge");
    const auto edge = edge_value != nullptr && edge_value->is_string() ? edge_from(edge_value->get<std::string>()) : std::nullopt;
    if (!edge) throw OpError("edge is top, vcenter, bottom, left, hcenter or right");
    std::string to = "auto";
    if (const Json* t = core::get(op, "to"); t != nullptr && !t->is_null()) {
        if (!t->is_string()) throw OpError("to is auto, layers, selection, page, trim, frame or panel");
        to = t->get<std::string>();
    }
    static const std::set<std::string> kTo{"auto", "layers", "selection", "page", "trim", "frame", "panel"};
    if (!kTo.contains(to)) throw OpError("to is auto, layers, selection, page, trim, frame or panel");
    const Things t = things_of(doc, page, op);
    if (t.items.empty()) throw OpError("the layers show nothing to align");
    std::optional<Box> area_box;
    if (const Json* a = core::get(op, "area"); a != nullptr && !a->is_null()) {
        const std::vector<double> b = selection::area_bbox(core::op_area(op));
        if (b.size() == 4 && b[2] > 0 && b[3] > 0) area_box = Box{b[0], b[1], b[0] + b[2], b[1] + b[3]};
    }
    const auto rect_box = [](const core::Rect& r) { return Box{r.x.value(), r.y.value(), r.x.value() + r.width.value(), r.y.value() + r.height.value()}; };
    std::vector<Box> boxes;
    for (const Item& item : t.items) boxes.push_back(item.box);
    Box target{};
    if (to == "selection" || (to == "auto" && t.items.size() == 1 && area_box)) {
        if (!area_box) throw OpError("there is no selection to align to");
        target = *area_box;
    } else if (to == "page" || (to == "auto" && t.items.size() == 1)) {
        target = rect_box(page.paper_rect_mm());
    } else if (to == "trim") {
        target = rect_box(page.trim_rect_mm());
    } else if (to == "frame") {
        target = rect_box(page.inner_rect_mm(doc.start_side ? std::string_view(*doc.start_side) : std::string_view()));
    } else if (to == "panel") {
        const Json* f = core::get(op, "frame_id");
        if (f == nullptr || !f->is_string()) throw OpError("frame_id names the panel to align to");
        const core::Frame* frame = page.find_frame(f->get<std::string>());
        if (frame == nullptr) throw core::OpKeyError(core::py_repr_str(f->get<std::string>()));
        target = rect_box(frame->rect);
    } else {
        target = *core::snap::united(boxes);
    }
    Json report = apply_moves(doc, at, t, align_moves(t.items, *edge, target, layer_bounds::grid(doc.page(at))));
    report["box"] = box_json(target);
    c.report = std::move(report);
}

void distribute_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* by_value = core::get(op, "by");
    static const std::set<std::string> kBy{"top", "vcenter", "bottom", "left", "hcenter", "right", "horizontal", "vertical"};
    if (by_value == nullptr || !by_value->is_string() || !kBy.contains(by_value->get<std::string>()))
        throw OpError("by is top, vcenter, bottom, left, hcenter, right, horizontal or vertical");
    const Things t = things_of(doc, doc.page(at), op);
    if (t.items.size() < 3) throw OpError("distributing needs three or more layers or lines that show something");
    c.report = apply_moves(doc, at, t, distribute_moves(t.items, by_value->get<std::string>(), layer_bounds::grid(doc.page(at))));
}

void arrange_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* to = core::get(op, "to");
    static const std::set<std::string> kTo{"front", "forward", "backward", "back", "reverse"};
    if (to == nullptr || !to->is_string() || !kTo.contains(to->get<std::string>())) throw OpError("to is front, forward, backward, back or reverse");
    const std::vector<std::string> ids = top_level(doc.page(at), ids_of(op, false));
    if (to->get<std::string>() == "reverse" && ids.size() < 2) throw OpError("choose two or more layers to reverse");
    std::vector<Layer> layers = doc.page(at).layers;
    if (!arrange(layers, ids, to->get<std::string>())) throw OpError("the layers cannot move further");
    doc.edit_page(at).layers = std::move(layers);
    Json order = Json::array();
    for (const Layer& layer : doc.page(at).layers) order.push_back(layer.id);
    c.report = Json{{"order", order}};
}

void link_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::vector<std::string> ids = ids_of(op, false);
    for (const std::string& id : ids) (void)layer_of(doc.page(at), id);
    bool link = !unlinks(doc.page(at), ids);
    if (const Json* given = core::get(op, "link"); given != nullptr && !given->is_null()) {
        if (!given->is_boolean()) throw OpError("link is true or false");
        link = given->get<bool>();
    }
    if (link && ids.size() < 2) throw OpError("choose two or more layers to link");
    Page& page = doc.edit_page(at);
    std::optional<std::string> name;
    if (link) {  // (a new group: link-1, link-2, … the first not in use on the page)
        std::set<std::string> used;
        for (const Layer& layer : page.layers)
            if (layer.link) used.insert(*layer.link);
        for (int k = 1; !name; ++k)
            if (!used.contains("link-" + std::to_string(k))) name = "link-" + std::to_string(k);
    }
    for (Layer& layer : page.layers)
        if (std::find(ids.begin(), ids.end(), layer.id) != ids.end()) layer.link = name;
    // (a group left with one layer is no link)
    std::map<std::string, int> counts;
    for (const Layer& layer : page.layers)
        if (layer.link) ++counts[*layer.link];
    for (Layer& layer : page.layers)
        if (layer.link && counts[*layer.link] < 2) layer.link.reset();
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    c.report = Json{{"linked", link}, {"link", name ? Json(*name) : Json(nullptr)}, {"ids", list}};
}

}  // namespace genko::render::layer_align
