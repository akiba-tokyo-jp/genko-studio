// The page ops of M2 (Python's ops._apply_one): duplicate_page, reorder and advance; and set_brush (the book's pen).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

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
    FrameIdMap frame_map;
    for (Frame& frame : clone.frames) refresh_frame_ids(frame, frame_map);
    if (py_truthy(clone.selected_frame_id)) {
        require_hashable(clone.selected_frame_id);
        const std::string* mapped =
            clone.selected_frame_id.is_string() ? frame_map.get(clone.selected_frame_id.get<std::string>()) : nullptr;
        clone.selected_frame_id = mapped != nullptr ? Json(*mapped) : Json(nullptr);
    }
    FrameIdMap layer_map;
    for (Layer& layer : clone.layers) {
        const std::string old = layer.id;
        layer.id = new_id();
        layer_map.set(old, layer.id);
        if (layer.frame_id && !layer.frame_id->empty()) {
            if (const std::string* mapped = frame_map.get(*layer.frame_id)) layer.frame_id = *mapped;
        }
    }
    std::vector<StoryLine> new_lines;
    for (const StoryLine& line : doc.story) {
        if (!(line.page_index == source_index)) continue;
        StoryLine copied = line;
        copied.id = new_id();
        copied.page_index = clone.index;
        if (copied.frame_id && !copied.frame_id->empty()) {
            if (const std::string* mapped = frame_map.get(*copied.frame_id)) copied.frame_id = *mapped;
        }
        new_lines.push_back(std::move(copied));
    }
    // (the copy's folders, rulers, cels and lines set under a layer name its own layers: the user's decision D2)
    std::vector<StoryLine*> copied_lines;
    for (StoryLine& line : new_lines) copied_lines.push_back(&line);
    remap_layer_refs(clone, copied_lines, layer_map);
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

// Call after validating/converting the string: malformed literals must keep their
// own Python error, but even a long sequence of leading zeroes counts as digits.
void check_onion_digit_limit(const Json& value) {
    if (!value.is_string()) return;
    const auto& text = value.get_ref<const std::string&>();
    const auto digits = std::count_if(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (digits > 4300) {
        throw PyValueError("Exceeds the limit (4300 digits) for integer string conversion: value has " +
                           std::to_string(digits) + " digits; use sys.set_int_max_str_digits() to increase the limit");
    }
}

std::string onion_integer_text(const Json& value);

// ops.set_onion: only None, the empty string, and values equal to 0 clear the reference.
// A nonzero reference is kept even when it names this page or no existing page.
void set_onion(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    const Json* from = get(c.op, "from");
    const bool clear = from == nullptr || from->is_null() || py_equals(*from, Json("")) || py_equals(*from, Json(0));
    std::optional<Num> reference;
    if (!clear) {
        // Validate Python's syntax and digit limit before the model's int64 bound.
        (void)onion_integer_text(*from);
        reference = Num(to_int(*from));
    }
    c.doc.edit_page(i).onion_from = reference;
}

// int(current) + int(delta) is unbounded in Python, but only its clamp to [1, pages]
// is stored. Keep decimal magnitudes exact so wide opposite offsets cannot lose a
// small difference in floating-point arithmetic (no new big-integer dependency).
std::string onion_integer_text(const Json& value) {
    const auto big = py_big_int_text(value);
    const std::string text = big ? *big : std::to_string(to_int(value));
    check_onion_digit_limit(value);
    return text;
}

int magnitude_compare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a == b ? 0 : (a < b ? -1 : 1);
}

Num bounded_onion_sum(std::string a, std::string b, const Num& upper) {
    const bool negative_a = a.front() == '-';
    const bool negative_b = b.front() == '-';
    if (negative_a) a.erase(0, 1);
    if (negative_b) b.erase(0, 1);
    const std::string bound = upper.repr();
    if (negative_a == negative_b) {
        if (negative_a) return Num(1);
        if (magnitude_compare(a, bound) >= 0 || magnitude_compare(b, bound) >= 0) return upper;
        const Num sum = Num(to_int(Json(a))) + Num(to_int(Json(b)));
        return std::max(Num(1), std::min(upper, sum));
    }
    const std::string& positive = negative_a ? b : a;
    const std::string& negative = negative_a ? a : b;
    if (magnitude_compare(positive, negative) <= 0) return Num(1);
    std::string difference = positive;
    std::size_t j = negative.size();
    int borrow = 0;
    for (std::size_t i = difference.size(); i-- > 0;) {
        int digit = positive[i] - '0' - borrow;
        if (j > 0) digit -= negative[--j] - '0';
        borrow = digit < 0 ? 1 : 0;
        difference[i] = static_cast<char>('0' + digit + 10 * borrow);
    }
    difference.erase(0, difference.find_first_not_of('0'));
    if (magnitude_compare(difference, bound) >= 0) return upper;
    return std::max(Num(1), Num(to_int(Json(difference))));
}

void step_onion(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    const Page& page = c.doc.page(i);
    const Num current = page.onion_from && page.onion_from->truthy() ? *page.onion_from : page.index;
    const std::string current_text = onion_integer_text(current.json());
    const std::string delta_text = onion_integer_text(py_or(py_get(c.op, "delta"), Json(-1)));
    Num reference = bounded_onion_sum(current_text, delta_text, Num(c.doc.pages.size()));
    if (reference == page.index) reference = std::max(Num(1), page.index - Num(1));
    c.doc.edit_page(i).onion_from = reference;
}

void set_lt(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    const Json* threshold = get(c.op, "threshold");
    if (threshold == nullptr) throw OpKeyError(py_repr_str("threshold"));
    const double value = finite_float(*threshold, "threshold");
    c.doc.edit_page(i).lt_threshold = Num(value);
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
    registry.add("set_onion", set_onion);
    registry.add("step_onion", step_onion);
    registry.add("set_lt", set_lt);
}

}  // namespace genko::core
