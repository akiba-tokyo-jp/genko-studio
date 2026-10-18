// The studio ops (Python's genko/studio/studio_ops.py, apply_studio_op and its helpers), and set_bible, add_ticket and
// set_ticket (Python's ops._apply_one): each with Python's arguments, checks in Python's order and messages. The values
// a book keeps for the studio are Python's dicts and lists (core::Json): where Python takes a value for a dict or a list
// and it is not one (a book written elsewhere), Python's AttributeError or TypeError is raised where Python raises it.
//
// Beyond Python (SPEC COMP-01a): a number made from text ("inf", "nan") that the op would keep — a placed picture's box,
// a region's box moved with its picture, a panel's box from a layout tree, a candidate's face box or margin — is refused,
// last, with "<key> must be a finite number" (Python writes Infinity or NaN into project.json); an asset ref that names
// no file under assets/ (one that reaches outside it: "/", "\", "." or "..") is not looked for there; what only a person
// holds — a panel's regions set wholesale, a character's approval (locked, and an approved character's sheet and face:
// set_bible, upsert_character), an approved character deleted, a request for approval, the style fixed by the pilot
// page — is refused to an agent ("only a person can …"); a picture is placed only when it can be drawn
// (StudioHooks::picture_check).

#include "core/studio.hpp"

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "core/clock.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/stylecat.hpp"

namespace genko::core {

namespace {

// --- Python's values --------------------------------------------------------------------------------------------------

// repr of a tuple of names: "('a', 'b')"
std::string tuple_repr(std::span<const std::string_view> items) {
    std::string out = "(";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ", ";
        out += py_repr_str(items[i]);
    }
    return out + ")";
}

// `value in (…)` for a tuple of names (==: only a str equal to one of them).
bool in_names(const Json& value, std::span<const std::string_view> items) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return std::any_of(items.begin(), items.end(), [&text](std::string_view item) { return item == text; });
}

bool is_text(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// op.get(key) (null when missing)
Json op_get(const Json& op, std::string_view key) { return get_or(op, key, Json()); }

// str(op.get(key) or fallback)
std::string text_or(const Json& op, std::string_view key, std::string_view fallback) {
    return py_str(py_or(op_get(op, key), Json(std::string(fallback))));
}

// x.get(key) on a value Python takes for a dict (AttributeError otherwise); null when missing.
Json dget(const Json& x, std::string_view key) {
    const Json* value = dict_get(x, key);
    return value != nullptr ? *value : Json();
}

// (x or {}).get(key)
Json or_get(const Json& x, std::string_view key) { return py_truthy(x) ? dget(x, key) : Json(); }

// x.setdefault(key, fallback): the value at key (fallback put there when it has none). The reference is to the value
// inside x: putting another key into x moves it (ordered_json keeps its items in a vector).
Json& setdefault(Json& x, std::string_view key, Json fallback) {
    if (!x.is_object()) raise_attribute_error(x, "setdefault");
    const auto it = x.find(key);
    if (it != x.end()) return *it;
    return x[std::string(key)] = std::move(fallback);
}

// x[key] = value, with Python's TypeError for what does not take it.
void setitem(Json& x, std::string_view key, Json value) {
    if (x.is_object()) {
        x[std::string(key)] = std::move(value);
        return;
    }
    if (x.is_array()) throw PyTypeError("list indices must be integers or slices, not str");
    throw PyTypeError("'" + py_type_name(x) + "' object does not support item assignment");
}

// list.append
void append(Json& list, Json item) {
    if (!list.is_array()) raise_attribute_error(list, "append");
    list.push_back(std::move(item));
}

// dict.update(other) for a dict literal `other`
void update(Json& x, const Json& other) {
    if (!x.is_object()) raise_attribute_error(x, "update");
    for (const auto& [key, value] : other.items()) x[key] = value;
}

// dict.pop(key, None)
void pop(Json& x, std::string_view key) {
    if (!x.is_object()) raise_attribute_error(x, "pop");
    x.erase(std::string(key));
}

// {**a, **b}: Python's TypeError "'list' object is not a mapping" for what is not a dict.
Json merged(const Json& a, const Json& b) {
    if (!a.is_object()) throw PyTypeError("'" + py_type_name(a) + "' object is not a mapping");
    if (!b.is_object()) throw PyTypeError("'" + py_type_name(b) + "' object is not a mapping");
    Json out = a;
    for (const auto& [key, value] : b.items()) out[key] = value;
    return out;
}

// A dict key as json.dumps writes it (a str as it is; int and float as repr; True/False/None as true/false/null).
std::string json_key(const Json& key) {
    if (key.is_string()) return key.get<std::string>();
    if (key.is_boolean()) return key.get<bool>() ? "true" : "false";
    if (key.is_null()) return "null";
    require_hashable(key);
    return py_str(key);
}

// dict(x): a dict, or [key, value] pairs (each key as json.dumps would write it), with Python's errors.
Json to_dict(const Json& value) {
    if (value.is_object()) return value;
    if (!value.is_array() && !value.is_string()) throw PyTypeError("'" + py_type_name(value) + "' object is not iterable");
    Json out = Json::object();
    std::size_t n = 0;
    for (const Json& item : iterate(value)) {
        if (!item.is_array() && !item.is_string() && !item.is_object()) {
            throw PyTypeError("cannot convert dictionary update sequence element #" + std::to_string(n) + " to a sequence");
        }
        const std::vector<Json> pair = iterate(item);
        if (pair.size() != 2) {
            throw PyValueError("dictionary update sequence element #" + std::to_string(n) + " has length " +
                               std::to_string(pair.size()) + "; 2 is required");
        }
        out[json_key(pair[0])] = pair[1];
        ++n;
    }
    return out;
}

// x += n for a number Python keeps in a book (an int, a float, a bool), with Python's TypeError otherwise. An int stays
// an int as Python's does (exact, kept as JSON keeps ints up to 2**64 - 1); one past what a book's JSON holds here is
// refused ("too large for this build": Python's int has no bound).
Json plus(const Json& x, std::int64_t n) {
    if (x.is_number_float()) return x.get<double>() + static_cast<double>(n);
    if (!x.is_boolean() && !x.is_number_integer()) {
        if (x.is_string()) throw PyTypeError("can only concatenate str (not \"int\") to str");
        if (x.is_array()) throw PyTypeError("can only concatenate list (not \"int\") to list");
        throw PyTypeError("unsupported operand type(s) for +=: '" + py_type_name(x) + "' and 'int'");
    }
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::uint64_t kUMax = std::numeric_limits<std::uint64_t>::max();
    if (x.is_number_unsigned()) {  // (0 … 2**64 - 1)
        const std::uint64_t u = x.get<std::uint64_t>();
        if (n >= 0) {
            const auto add = static_cast<std::uint64_t>(n);
            if (u <= kUMax - add) {
                const std::uint64_t sum = u + add;
                return sum <= static_cast<std::uint64_t>(kMax) ? Json(static_cast<std::int64_t>(sum)) : Json(sum);
            }
        } else {
            const std::uint64_t take = static_cast<std::uint64_t>(-(n + 1)) + 1;  // (|n|, kept in range)
            if (u >= take) {
                const std::uint64_t sum = u - take;
                return sum <= static_cast<std::uint64_t>(kMax) ? Json(static_cast<std::int64_t>(sum)) : Json(sum);
            }
            return Json(-static_cast<std::int64_t>(take - u - 1) - 1);  // (take - u ≤ 2**63)
        }
    } else {
        const std::int64_t i = x.is_boolean() ? (x.get<bool>() ? 1 : 0) : x.get<std::int64_t>();
        if (n > 0 && i > kMax - n) return Json(static_cast<std::uint64_t>(i) + static_cast<std::uint64_t>(n));  // (≤ 2**64 - 2)
        if (!(n < 0 && i < kMin - n)) return Json(i + n);
    }
    throw OpError("a count past 2**64 - 1 is too large for this build (Python's int has no bound)");
}

// float + x (Python's TypeError for what is not a number)
double float_plus(double a, const Json& x) {
    if (x.is_boolean()) return a + (x.get<bool>() ? 1.0 : 0.0);
    if (x.is_number()) return a + x.get<double>();
    throw PyTypeError("unsupported operand type(s) for +: 'float' and '" + py_type_name(x) + "'");
}

// A text that is the same for the values a Python dict or set takes for the same key (1, 1.0 and True; equal texts)
// and different otherwise, after require_hashable (Python's TypeError). Each NaN is its own key, as two NaNs read from
// JSON are two objects that are not equal.
std::string hash_key(const Json& value) {
    require_hashable(value);
    if (value.is_null()) return "n";
    if (value.is_boolean()) return value.get<bool>() ? "#1" : "#0";
    if (value.is_number_unsigned()) return "#" + std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer()) return "#" + std::to_string(value.get<std::int64_t>());
    if (value.is_number_float()) {
        const double v = value.get<double>();
        if (std::isnan(v)) {
            static std::atomic<std::uint64_t> nans{0};
            return "nan:" + std::to_string(++nans);
        }
        if (v == 0.0) return "#0";
        std::array<char, 400> text{};
        if (std::isfinite(v) && v == std::trunc(v)) {  // (an integral float is the int of the same value)
            const auto end = std::to_chars(text.data(), text.data() + text.size(), v, std::chars_format::fixed, 0).ptr;
            return "#" + std::string(text.data(), end);
        }
        const auto end = std::to_chars(text.data(), text.data() + text.size(), v).ptr;
        return "f" + std::string(text.data(), end);
    }
    if (value.is_string()) return "s" + value.get<std::string>();
    return "j" + dump_python(value);
}

// A Python set of JSON values (studio_ops keeps "pinned" as one): kept by hash_key.
class PySet {
public:
    void add(const Json& value) { items_.try_emplace(hash_key(value), value); }
    void discard(const Json& value) { items_.erase(hash_key(value)); }
    bool contains(const Json& value) const { return items_.contains(hash_key(value)); }
    // sorted(set): strs by their code points, numbers by value; Python's TypeError for values that do not compare.
    Json sorted() const {
        std::vector<Json> items;
        items.reserve(items_.size());
        for (const auto& [key, item] : items_) items.push_back(item);
        const bool texts = std::all_of(items.begin(), items.end(), [](const Json& v) { return v.is_string(); });
        const bool numbers = std::all_of(items.begin(), items.end(), [](const Json& v) {
            return v.is_boolean() || (v.is_number() && !(v.is_number_float() && std::isnan(v.get<double>())));
        });
        if (texts) {
            std::sort(items.begin(), items.end(),
                      [](const Json& a, const Json& b) { return a.get_ref<const std::string&>() < b.get_ref<const std::string&>(); });
        } else if (numbers) {
            std::stable_sort(items.begin(), items.end(), [](const Json& a, const Json& b) { return py_less(a, b); });
        } else {
            // (values of kinds that do not compare, or a NaN: compared one by one, as an insertion sort, so the first pair
            // that does not compare raises Python's TypeError)
            const auto less = [](const Json& a, const Json& b) {
                if (a.is_string() && b.is_string()) return a.get_ref<const std::string&>() < b.get_ref<const std::string&>();
                const bool na = a.is_number() || a.is_boolean();
                const bool nb = b.is_number() || b.is_boolean();
                if (na && nb) return py_less(a, b);
                throw PyTypeError("'<' not supported between instances of '" + py_type_name(a) + "' and '" + py_type_name(b) + "'");
            };
            for (std::size_t i = 1; i < items.size(); ++i) {
                for (std::size_t j = i; j > 0 && less(items[j], items[j - 1]); --j) std::swap(items[j], items[j - 1]);
            }
        }
        Json out = Json::array();
        for (Json& item : items) out.push_back(std::move(item));
        return out;
    }

private:
    std::map<std::string, Json> items_;
};

// set(x)
PySet to_set(const Json& value) {
    PySet out;
    for (const Json& item : iterate(value)) out.add(item);
    return out;
}

// --- studio_ops' helpers ------------------------------------------------------------------------------------------------

// item.get("id") or prefix + new_id()[:n]: a new id only when the item has none.
Json id_or_new(const Json& item, const std::string& prefix, std::size_t n) {
    const Json given = dget(item, "id");
    return py_truthy(given) ? given : Json(prefix + new_id().substr(0, n));
}

bool person(const OpContext& c) { return c.actor.can_approve(); }

// _by_id(items, key): the first item whose "id" == key, or none. Iterated as Python iterates it: a dict's keys and a
// str's characters have no .get (AttributeError); TypeError for what cannot be iterated. The pointer is into `items`.
template <typename J>
J* by_id(J& items, const Json& key) {
    if (items.is_array()) {
        for (auto& item : items) {
            if (py_equals(dget(item, "id"), key)) return &item;
        }
        return nullptr;
    }
    for (const Json& item : iterate(items)) (void)dget(item, "id");
    return nullptr;
}

// _upsert(items, item, label)
void upsert(Json& items, const Json& item, const std::string& label) {
    if (!item.is_object() || !truthy_at(item, "id")) throw OpError(label + " needs an id");
    if (items.is_array()) {
        for (Json& existing : items) {
            if (py_equals(dget(existing, "id"), item["id"])) {
                existing = item;
                return;
            }
        }
    } else {
        for (const Json& existing : iterate(items)) (void)dget(existing, "id");
    }
    append(items, item);
}

// _page(episode, op): the page op["page_id"] names, else the page int(op["page"]) numbers.
std::size_t studio_page(const Document& doc, const Json& op) {
    const Json page_id = op_get(op, "page_id");
    if (py_truthy(page_id)) {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (is_text(page_id, doc.pages[i]->id)) return i;
        }
        throw OpError("no page " + py_str(page_id));
    }
    const Json* page = get(op, "page");
    if (page == nullptr) throw OpError("page is required");
    // (Python's int has no bound: a number past 64 bits is only a page the book does not have)
    if (const auto big = py_big_int_text(*page)) throw OpError("no page " + *big);
    std::int64_t index = 0;
    try {
        index = to_int(*page);
    } catch (const PyUncaught&) {
        throw;
    } catch (const Error&) {
        throw OpError("page is required");
    }
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index == Num(index)) return i;
    }
    throw OpError("no page " + std::to_string(index));
}

// _leaf(page, frame_id): the leaf panel the id names (looked at only).
const Frame& leaf_of(const Page& page, const Json& frame_id) {
    if (!py_truthy(frame_id)) throw OpError("frame_id is required");
    const std::string id = py_str(frame_id);
    const Frame* frame = page.find_frame(id);  // (Page._find: from the first panel; none at all is Python's IndexError)
    if (frame == nullptr) throw OpError("no frame " + id + " on page " + page.index.repr());
    if (!frame->children.empty()) throw OpError("frame " + id + " is not a leaf panel");
    return *frame;
}

// The same, to change (the page is one being changed: Document::edit_page).
Frame& leaf(Page& page, const Json& frame_id) { return *page.find_frame(leaf_of(page, frame_id).id); }

// _panel(frame): the panel's brief, {"status": "empty"} when it has none.
Json& panel_of(Frame& frame) {
    if (!frame.panel) frame.panel = Json::object({{"status", "empty"}});
    return *frame.panel;
}

// _refresh_brief(panel)
void refresh_brief(Json& panel) {
    const std::string hash = brief_hash(panel);
    setitem(panel, "brief_hash", hash);
    const Json* cands = dict_get(panel, "candidates");  // (panel.get("candidates", []))
    if (cands == nullptr) return;
    if (!cands->is_array()) {  // (a dict's keys, a str's characters: no .get; others are not iterable)
        for (const Json& cand : iterate(*cands)) (void)dget(cand, "brief_hash");
        return;
    }
    for (Json& cand : panel["candidates"]) {
        const bool stale = !py_equals(dget(cand, "brief_hash"), panel["brief_hash"]);
        setitem(cand, "stale", stale);
    }
}

// _ticket(episode, page, agent, **fields): a new open ticket on the book's list.
void new_ticket(Document& doc, const Page* page, const Actor& actor, const std::string& assignee, const Json& fields) {
    Json ticket = Json::object();
    ticket["id"] = new_id();
    ticket["page_index"] = page != nullptr ? page->index.json() : Json();
    ticket["page_id"] = page != nullptr ? Json(page->id) : Json();
    ticket["assignee"] = assignee;
    ticket["status"] = "open";
    ticket["created_by"] = actor.name();
    ticket["at_rev"] = doc.revision;
    for (const auto& [key, value] : fields.items()) ticket[key] = value;
    append(doc.tickets, std::move(ticket));
}

// `index in pages` for a ticket's pages (a list as Python compares it; Python's TypeError for a str or a number)
bool page_listed(const Json& pages, const Num& index) {
    if (pages.is_array()) {
        return std::any_of(pages.begin(), pages.end(), [&index](const Json& p) { return py_equals(p, index.json()); });
    }
    if (pages.is_object()) return false;  // (its keys are strs: never a page number)
    const std::string type = index.is_int() ? "int" : "float";
    if (pages.is_string()) throw PyTypeError("'in <string>' requires string as left operand, not " + type);
    throw PyTypeError("argument of type '" + py_type_name(pages) + "' is not iterable");
}

// _close_tickets(episode, agent, kind, gate, page, character_id)
void close_tickets(Document& doc, const Actor& actor, std::string_view kind, std::string_view gate, const Page* page,
                   const std::optional<std::string>& character_id) {
    if (!doc.tickets.is_array()) {
        for (const Json& ticket : iterate(doc.tickets)) (void)dget(ticket, "kind");
        return;
    }
    for (Json& ticket : doc.tickets) {
        if (!is_text(dget(ticket, "kind"), kind) || !is_text(dget(ticket, "status"), "open") || !is_text(dget(ticket, "gate"), gate)) {
            continue;
        }
        if (page != nullptr) {
            Json pages = dget(ticket, "pages");
            if (!py_truthy(pages)) {
                const Json at = dget(ticket, "page_index");
                pages = py_truthy(at) ? Json::array({at}) : Json::array();
            }
            if (!page_listed(pages, page->index)) continue;
            Json remaining = Json::array();
            for (const Json& p : pages) {
                if (!py_equals(p, page->index.json())) remaining.push_back(p);
            }
            if (!remaining.empty()) {
                ticket["pages"] = std::move(remaining);
                continue;
            }
        }
        if (character_id && !is_text(dget(ticket, "character_id"), *character_id)) continue;
        ticket["status"] = "done";
        ticket["closed_by"] = actor.name();
    }
}

// _art_layer(page, frame_id, to): the position of the panel's placed picture adopted as `to`.
std::optional<std::size_t> art_layer(const Page& page, const std::string& frame_id, const std::string& to) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const Layer& layer = page.layers[i];
        if (layer.kind != LayerKind::Placed || layer.frame_id != frame_id) continue;
        const Json source = layer.source ? *layer.source : Json();
        const Json given = py_truthy(source) ? (dict_get(source, "to") != nullptr ? *dict_get(source, "to") : Json("art")) : Json("art");
        if (is_text(given, to)) return i;
    }
    return std::nullopt;
}

// (layer.source or {}).get("to") == "ink"
bool placed_ink(const Layer& layer) { return layer.source && is_text(or_get(*layer.source, "to"), "ink"); }

// _insert_below_ink(page, layer): placed art goes under the ink; extracted line art (to: ink) over the other placed art.
std::size_t insert_below_ink(Page& page, Layer layer) {
    const bool is_ink = placed_ink(layer);
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const Layer& existing = page.layers[i];
        const bool existing_ink = existing.kind == LayerKind::Placed && placed_ink(existing);
        if (existing.role == LayerRole::Ink || (existing_ink && !is_ink)) {
            page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(i), std::move(layer));
            return i;
        }
    }
    page.layers.push_back(std::move(layer));
    return page.layers.size() - 1;
}

// tuple(x)
std::vector<Json> to_tuple(const Json& value) { return iterate(value); }

// _place(page, frame, layer, px, fit, clip_to, offset, scale, pad_mm): the picture fitted over its panel (a generated
// picture covers the panel and the request's pad, kept outside the panel and cut off by the clip) or the page.
void place(const Page& page, const Frame* frame, Layer& layer, const Json& px, const std::string& fit, const std::string& clip_to,
           const std::vector<Json>& offset, double scale, double pad_mm) {
    Rect target;
    if (frame == nullptr) {
        target = Rect{Num(0), Num(0), page.spec.width_mm, page.spec.height_mm};
    } else if (clip_to == "bleed") {
        target = default_target(page, *frame, clip_to);
    } else {
        const Rect& r = frame->rect;
        target = Rect{Num(r.x.value() - pad_mm), Num(r.y.value() - pad_mm), Num(r.width.value() + 2 * pad_mm),
                      Num(r.height.value() + 2 * pad_mm)};
    }
    layer.fit = fit;
    layer.clip_to = clip_to;
    const std::int64_t w = to_int(subscript(px, 0));
    const std::int64_t h = to_int(subscript(px, 1));
    // (fit_rect adds offset_mm[0], then offset_mm[1]: Python's errors in that order)
    if (offset.empty()) raise_index_error("tuple index out of range");
    const double ox = float_plus(0.0, offset[0]);
    if (offset.size() < 2) raise_index_error("tuple index out of range");
    const double oy = float_plus(0.0, offset[1]);
    layer.placement_mm = fit_rect(target, w, h, fit, ox, oy, scale);
}

// _pad(cand): float((cand.get("mapping") or {}).get("pad_mm") or 0.0)
double pad_of(const Json& cand) { return to_float(py_or(or_get(dget(cand, "mapping"), "pad_mm"), Json(0.0))); }

// The placed picture's box must be finite (C++ only: Python writes Infinity or NaN into project.json).
void require_finite_placement(const Layer& layer) {
    if (layer.placement_mm) require_finite(rect_to_json(*layer.placement_mm), "placement_mm");
}

// _style_lock(episode, page, agent): what the pilot page fixes for the rest of the book.
Json style_lock(const Document& doc, const Page& page, const Actor& actor) {
    Json tools = Json::array();  // [[tool, count]] in the order first seen (a dict)
    std::optional<std::pair<Num, Json>> best;
    for (const Frame* frame : page.leaf_frames()) {
        const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
        const Json candidates = py_get(panel, "candidates", Json::array());
        const Json* cand = by_id(candidates, Json(py_str(py_or(or_get(dget(panel, "adopted"), "art"), Json("")))));
        if (cand == nullptr) continue;
        const Json* maker = cand;
        for (int round = 0; round < 8; ++round) {  // (Genko's own enlargement is not the tool that drew it)
            const std::string tool_id = py_str(py_or(or_get(dget(*maker, "origin"), "tool_id"), Json("")));
            if (!tool_id.starts_with("genko:") || !py_truthy(dget(*maker, "parent"))) break;
            const Json* parent = by_id(candidates, Json(py_str(subscript(*maker, "parent"))));
            if (parent != nullptr && py_truthy(*parent)) maker = parent;
        }
        Json tool = or_get(dget(*maker, "origin"), "tool_id");
        if (py_str(py_or(tool, Json(""))).starts_with("genko:")) tool = Json();
        if (py_truthy(tool)) {
            require_hashable(tool);
            bool counted = false;
            for (Json& entry : tools) {
                if (py_equals(entry[0], tool)) {
                    entry[1] = entry[1].get<std::int64_t>() + 1;
                    counted = true;
                    break;
                }
            }
            if (!counted) tools.push_back(Json::array({tool, 1}));
        }
        const Num area = frame->rect.width * frame->rect.height;
        if (!best || area > best->first) best = std::make_pair(area, subscript(*cand, "asset"));
    }
    Json chosen;  // max(sorted(tools), key=lambda t: tools[t])
    if (!tools.empty()) {
        PySet names;
        for (const Json& entry : tools) names.add(entry[0]);
        std::int64_t most = -1;
        for (const Json& name : names.sorted()) {
            for (const Json& entry : tools) {
                if (py_equals(entry[0], name) && entry[1].get<std::int64_t>() > most) {
                    most = entry[1].get<std::int64_t>();
                    chosen = name;
                }
            }
        }
    }
    const Json style = py_or(op_get(doc.studio, "style"), Json::object());
    Json notes = dget(style, "notes");
    if (!py_truthy(notes)) notes = or_get(or_get(py_or(op_get(doc.studio, "bible_doc"), Json::object()), "style"), "notes");
    if (!py_truthy(notes)) notes = Json::array();
    Json out = Json::object();
    out["from_page"] = page.id;
    out["page"] = page.index.json();
    out["tool"] = chosen;
    out["reference"] = best ? best->second : Json();
    out["notes"] = py_list(notes);
    out["finish"] = dget(style, "finish");
    out["by"] = actor.name();
    out["rev"] = doc.revision;
    return out;
}

// --- assets ---------------------------------------------------------------------------------------------------------

// The first `n` code points of a UTF-8 text (Python's text[:n]).
std::string first_code_points(std::string_view text, std::size_t n) {
    std::size_t at = 0;
    std::size_t count = 0;
    while (at < text.size() && count < n) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
        ++count;
    }
    return std::string(text.substr(0, at));
}

// _require_asset(episode, ref): an asset ref ("sha256:…") with a file of its digest under assets/<first two>/ (when the
// book is on disk). A digest that would reach outside assets/<ab>/ ("/", "\", "." or ".." as its folder, a NUL) names
// no file there (Python joins it as a path and may look elsewhere).
void require_asset(const Document& doc, const Json& ref) {
    if (!ref.is_string()) raise_attribute_error(ref, "startswith");
    const std::string& text = ref.get_ref<const std::string&>();
    if (!text.starts_with("sha256:")) throw OpError("asset must be sha256:…, got " + py_repr_str(text));
    if (!doc.asset_dir) return;
    const std::string digest = text.substr(7);
    const std::string head = first_code_points(digest, 2);
    // (a digest that would name a path elsewhere — "/", "\\", NUL, ":" (a drive on Windows), or "." / ".." as its
    // folder — is no asset: C++ only, Python would look there)
    const bool outside = digest.find_first_of(std::string("/\\\0:", 4)) != std::string::npos || head == "." || head == "..";
    bool found = false;
    if (!outside) {
        std::filesystem::path folder = *doc.asset_dir / "assets";
        if (!head.empty()) folder /= path_from_utf8(head);
        std::error_code error;
        if (std::filesystem::is_directory(folder, error)) {
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                if (path_to_utf8(it->path().filename()).starts_with(digest)) {
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) throw OpError("asset " + text + " is not in assets/ (import it first)");
}

// _asset_size(episode, ref): the size of the asset's picture.
std::pair<std::int64_t, std::int64_t> asset_size(const Document& doc, const Json& ref, const StudioHooks& hooks) {
    require_asset(doc, ref);
    if (!doc.asset_dir) throw OpError("placing an asset needs a saved project");
    if (!hooks.picture_size) not_yet_ported("reading an asset's picture is render's (render::ops_registry)");
    const std::string text = ref.get<std::string>();
    const auto size = hooks.picture_size(*doc.asset_dir, text);
    if (!size) throw OpError("asset " + text + " is not a PNG");
    return *size;
}

// (C++ only, SPEC COMP-01a: a picture is placed — place_asset, adopt_candidate, set_placement of a placed picture that is
// no candidate — only when it can be drawn: the whole picture decoded, StudioHooks::picture_check. Python reads its size
// at most, places it, and the drawing and the export fail on it. Looked at after Python's own checks; nothing to say
// when there is no such file, as Python, which does not look.)
void require_drawable(const Document& doc, const std::optional<std::string>& ref, const StudioHooks& hooks) {
    if (!hooks.picture_check || !doc.asset_dir || !ref) return;
    hooks.picture_check(*doc.asset_dir, *ref);
}

// --- the ops ----------------------------------------------------------------------------------------------------------

// The style fixed by the pilot page (studio.style.locked), or nothing.
std::optional<Json> style_locked(const Json& studio) {
    const Json* style = get(studio, "style");
    if (style == nullptr || !style->is_object() || !style->contains("locked")) return std::nullopt;
    return (*style)["locked"];
}

void set_studio(OpContext& c) {
    if (has(c.op, "policy") && !person(c)) throw OpError("only a person can change the policy");
    Json& studio = c.doc.studio;
    const std::optional<Json> fixed = style_locked(studio);
    for (const char* key : {"bible_doc", "style", "policy", "premise"}) {
        const Json* value = get(c.op, key);
        if (value == nullptr) continue;
        const std::string_view name(key);
        const Json* kept = get(studio, key);
        if ((name == "style" || name == "policy") && value->is_object() && kept != nullptr && kept->is_object()) {
            studio[key] = merged(*kept, *value);
        } else {
            studio[key] = *value;
        }
    }
    // (C++ only: Python lets an agent change the style a person fixed by approving the pilot page — SPEC 295)
    if (!person(c) && style_locked(studio) != fixed) {
        throw OpError("only a person can change the style fixed by the pilot page (style.locked)");
    }
}

void set_style_catalog(OpContext& c) {
    Json& studio = c.doc.studio;
    Json style = py_or(op_get(studio, "style"), Json::object());
    if (py_truthy(dget(style, "locked")) && !person(c)) {
        throw OpError("the style is fixed by the pilot page: a person changes the style catalog");
    }
    const Json catalog = op_get(c.op, "catalog");
    // (style.get("catalog") or {}).get("id") != (catalog or {}).get("id"), the left side first
    const auto other_style = [&style, &catalog] {
        const Json kept = or_get(dget(style, "catalog"), "id");
        return !py_equals(kept, or_get(catalog, "id"));
    };
    if (py_truthy(dget(style, "locked")) && other_style()) {
        // (a person changed the style after the pilot page: its picture no longer shows the style)
        Json locked = merged(style["locked"], Json::object());
        locked["reference"] = nullptr;
        locked["stale"] = true;
        locked["rev"] = c.doc.revision;
        style["locked"] = std::move(locked);
    }
    if (other_style()) style["changed_rev"] = c.doc.revision;  // (sheets drawn before this are in the old style)
    if (catalog.is_null()) {
        pop(style, "catalog");
    } else {
        Json kept;
        try {
            kept = stylecat::validate(catalog);
        } catch (const stylecat::CatalogError& error) {
            throw OpError(error.what());
        }
        if (py_truthy(dget(kept, "sample"))) require_asset(c.doc, kept["sample"]);
        kept["by"] = c.actor.name();
        style["catalog"] = std::move(kept);
    }
    studio["style"] = std::move(style);
}

// The approved characters (locked: a person approved the sheet) by id (the first of an id), each with the sheet and
// face refs the approval holds. (A character that is not locked holds no approval: its sheet and face refs are
// references until a person approves one — attach_reference gives them, as Python.)
std::map<std::string, Json> approvals_of(const Json& characters) {
    std::map<std::string, Json> out;
    for (const Json& character : iterate(characters)) {
        if (!character.is_object()) continue;
        const std::string id = dump_python(dget(character, "id"));
        if (out.contains(id)) continue;
        Json refs = Json::array();
        if (const Json* given = get(character, "refs"); given != nullptr && given->is_array()) {
            for (const Json& ref : *given) {
                if (ref.is_object() && in_names(dget(ref, "kind"), std::initializer_list<std::string_view>{"sheet", "face"})) refs.push_back(ref);
            }
        }
        out[id] = truthy_at(character, "locked") ? std::move(refs) : Json();  // (null: not approved)
    }
    std::erase_if(out, [](const auto& item) { return item.second.is_null(); });
    return out;
}

// (C++ only, SPEC COMP-01a: Python lets an agent's bible or character be locked — an approval no person gave —, an
// approved character be unlocked or dropped, or the sheet and face of an approved character be changed — SPEC 295)
const char* const kApprovalIsPersons =
    "only a person can change a character's approval (locked, and an approved character's sheet and face)";

void upsert_character(OpContext& c) {
    Json character = py_or(op_get(c.op, "character"), Json::object());
    Json& characters = c.doc.bible.characters;
    const Json* existing = by_id(characters, py_get(character, "id", Json("")));
    const bool locked = existing != nullptr && py_truthy(*existing) && py_truthy(dget(*existing, "locked"));
    if (locked && !(truthy_at(c.op, "unlock") && person(c))) {
        throw OpError("character " + py_str(dget(character, "id")) + " is locked (a person can unlock it)");
    }
    // (an approved character an agent cannot change at all, as in Python: what is left is one locked by the agent)
    const std::optional<std::map<std::string, Json>> approvals = person(c) ? std::nullopt : std::optional(approvals_of(characters));
    if (locked && !character.contains("locked")) character["locked"] = false;
    upsert(characters, character, "character");
    if (approvals && *approvals != approvals_of(characters)) throw OpError(kApprovalIsPersons);
}

void delete_character(OpContext& c) {
    const std::string cid = text_or(c.op, "id", "");
    bool used = false;
    for (const auto& page : c.doc.pages) {
        for (const Frame* frame : page->leaf_frames()) {
            const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
            for (const Json& character : iterate(py_get(panel, "characters", Json::array()))) {
                if (is_text(dget(character, "id"), cid)) {
                    used = true;
                    break;
                }
            }
            if (used) break;
        }
        if (used) break;
    }
    if (used && !truthy_at(c.op, "force")) throw OpError("character " + cid + " appears in panels; use force");
    if (!person(c)) {  // (C++ only: Python lets an agent delete a character whose sheet a person approved — SPEC 295)
        for (const Json& character : iterate(c.doc.bible.characters)) {
            if (is_text(dget(character, "id"), cid) && truthy_at(character, "locked")) {
                throw OpError("only a person can delete an approved character (" + cid + ")");
            }
        }
    }
    Json kept = Json::array();
    for (const Json& character : iterate(c.doc.bible.characters)) {
        if (!is_text(dget(character, "id"), cid)) kept.push_back(character);
    }
    c.doc.bible.characters = std::move(kept);
}

void upsert_place(OpContext& c, const char* key, const char* item_key, const std::string& label) {
    Json& items = setdefault(c.doc.studio, key, Json::array());
    upsert(items, op_get(c.op, item_key), label);
}

void delete_place(OpContext& c, const char* key) {
    Json kept = Json::array();
    const Json id = op_get(c.op, "id");
    for (const Json& item : iterate(py_get(c.doc.studio, key, Json::array()))) {
        if (!py_equals(dget(item, "id"), id)) kept.push_back(item);
    }
    c.doc.studio[key] = std::move(kept);
}

void set_script(OpContext& c) {
    const Json* script = get(c.op, "script");
    if (script == nullptr || !script->is_object()) throw OpError("script must be an object");
    c.doc.studio["script"] = *script;
}

void set_page_plan(OpContext& c) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    const Json plan = py_or(op_get(c.op, "plan"), Json::object());
    const Json old = page.plan ? *page.plan : Json();
    Json result = merged(py_or(old, Json::object()), plan);
    if (py_truthy(old) && old.contains("reviews") && !plan.contains("reviews")) result["reviews"] = old["reviews"];
    page.plan = std::move(result);
}

void set_panel(OpContext& c) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    Json& panel = panel_of(leaf(page, op_get(c.op, "frame_id")));
    PySet pinned = to_set(py_get(panel, "pinned", Json::array()));
    const Json changes = py_or(op_get(c.op, "set"), Json::object());
    if (!changes.is_object()) raise_attribute_error(changes, "items");
    const bool human = person(c);
    // (C++ only: Python lets an agent overwrite a panel's regions with set_panel, a person's own among them — SPEC 295)
    const auto regions_by_agent = [human](const Json& key) {
        if (key == Json("regions") && !human) {
            throw OpError("only a person can set a panel's regions with set_panel (an agent uses add_region, edit_region or "
                          "replace_regions)");
        }
    };
    for (const auto& [key, value] : changes.items()) {
        if (pinned.contains(key) && !human) throw OpError(key + " is pinned by a person");
        regions_by_agent(Json(key));
        if (key == "gen" && value.is_object()) {
            bool overrides = false;
            for (const auto& [k, v] : value.items()) {
                if (k.ends_with("_override") && !v.is_null()) overrides = true;
            }
            if (overrides) {
                if (!human) throw OpError("only a person can set prompt/size overrides");
                pinned.add(Json("gen"));
            }
        }
        if (key == "status" && !in_names(value, kPanelStatuses)) throw OpError("status must be one of " + tuple_repr(kPanelStatuses));
        if (key == "status" && is_text(value, "skip") && !human) throw OpError("only a person can mark a panel skip");
        if (key == "candidates" || key == "adopted") throw OpError(key + " changes through the candidate ops");
        setitem(panel, key, value);
    }
    for (const Json& key : iterate(py_or(op_get(c.op, "unset"), Json::array()))) {
        if (pinned.contains(key) && !human) throw OpError(py_str(key) + " is pinned by a person");
        regions_by_agent(key);
        if (key.is_string()) panel.erase(key.get<std::string>());  // (panel.pop(key, None): its keys are strs)
    }
    for (const Json& key : iterate(py_or(op_get(c.op, "pin"), Json::array()))) pinned.add(key);
    for (const Json& key : iterate(py_or(op_get(c.op, "unpin"), Json::array()))) {
        if (!human) throw OpError("only a person can unpin");
        pinned.discard(key);
    }
    setitem(panel, "pinned", pinned.sorted());
    if (is_text(dget(panel, "status"), "empty") &&
        (py_truthy(dget(panel, "shot")) || py_truthy(dget(panel, "action")) || py_truthy(dget(panel, "characters")))) {
        panel["status"] = "briefed";
    }
    refresh_brief(panel);
}

void record_review(OpContext& c) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    Json review = Json::object();
    review["by"] = c.actor.name();
    review["score"] = op_get(c.op, "score");
    review["notes"] = text_or(c.op, "notes", "");
    review["input_hash"] = op_get(c.op, "input_hash");
    review["rev"] = c.doc.revision;
    const std::string kind = text_or(c.op, "kind", "name");
    if (truthy_at(c.op, "frame_id")) {
        Json& panel = panel_of(leaf(page, c.op["frame_id"]));
        setitem(setdefault(panel, "reviews", Json::object()), kind, std::move(review));
    } else {
        if (!page.plan || !py_truthy(*page.plan)) page.plan = Json::object();
        setitem(setdefault(*page.plan, "reviews", Json::object()), kind, std::move(review));
    }
}

void approve(OpContext& c) {
    if (!person(c)) throw OpError("approve needs a person (actor " + c.actor.name() + " cannot approve)");
    const Json gate = op_get(c.op, "gate");
    if (!in_names(gate, kGates)) throw OpError("gate must be one of " + tuple_repr(kGates));
    const std::string name = gate.get<std::string>();
    Document& doc = c.doc;
    Json record = Json::object();
    record["gate"] = gate;
    record["by"] = c.actor.name();
    record["rev"] = doc.revision;
    if (truthy_at(c.op, "via")) {  // (sent in a chat and recorded by the AI: who passed it on, and the person's words)
        record["via"] = first_code_points(py_str(c.op["via"]), 80);
        record["message"] = first_code_points(text_or(c.op, "message", ""), 500);
    }
    if (name == "name" || name == "art") {
        const std::size_t at = studio_page(doc, c.op);
        Page& page = doc.edit_page(at);
        record["page_id"] = page.id;
        if (name == "name") {
            page.name_ok = true;
            if (page.stage == "name") page.stage = "ink";  // (pipeline.advance: the name is approved)
        } else {
            if (!page.name_ok) throw OpError("page " + page.index.repr() + ": approve the name before the art");
            std::vector<std::string> missing;
            for (const Frame* frame : page.leaf_frames()) {
                if (frame->panel && py_truthy(*frame->panel) &&
                    !in_names(dget(*frame->panel, "status"), std::initializer_list<std::string_view>{"adopted", "skip", "empty"})) {
                    missing.push_back(frame->id);
                }
            }
            if (!missing.empty()) {
                std::string list;
                for (std::size_t i = 0; i < missing.size(); ++i) list += (i > 0 ? ", " : "") + missing[i];
                throw OpError("page " + page.index.repr() + ": panels without adopted art: " + list);
            }
            page.art_ok = true;
            setdefault(doc.studio, "style", Json::object());
            const Json lock = py_or(dget(doc.studio["style"], "locked"), Json::object());
            if (!py_truthy(lock) || py_truthy(dget(lock, "stale")) || is_text(dget(lock, "from_page"), page.id)) {
                // (the first approval fixes the style; approving the pilot page again, or the first page after a person
                // changed the style, fixes it anew)
                setitem(doc.studio["style"], "locked_from_page", page.id);
                Json fixed = style_lock(doc, page, c.actor);
                setitem(doc.studio["style"], "locked", std::move(fixed));
            }
        }
        close_tickets(doc, c.actor, "gate", name, &doc.page(at), std::nullopt);
    } else if (name == "sheet") {
        const std::string cid = text_or(c.op, "character_id", "");
        Json* character = by_id(doc.bible.characters, Json(cid));
        if (character == nullptr) throw OpError("no character " + cid);
        const Json bucket = py_get(py_get(doc.studio, "character_candidates", Json::object()), cid, Json::array());
        const Json* cand = by_id(bucket, Json(text_or(c.op, "candidate_id", "")));
        if (cand == nullptr) throw OpError("no candidate " + py_str(op_get(c.op, "candidate_id")) + " for " + cid);
        Json refs = Json::array();
        for (const Json& ref : iterate(py_get(*character, "refs", Json::array()))) {
            if (!in_names(dget(ref, "kind"), std::initializer_list<std::string_view>{"sheet", "face"})) refs.push_back(ref);
        }
        refs.push_back(Json::object({{"asset", subscript(*cand, "asset")}, {"kind", "sheet"}, {"approved_by", c.actor.name()}}));
        if (truthy_at(c.op, "face_asset")) {
            refs.push_back(Json::object({{"asset", c.op["face_asset"]}, {"kind", "face"}, {"approved_by", c.actor.name()}}));
        }
        setitem(*character, "refs", std::move(refs));
        (*character)["locked"] = true;
        record["character_id"] = cid;
        close_tickets(doc, c.actor, "gate", "sheet", nullptr, cid);
    } else {
        close_tickets(doc, c.actor, "gate", name, nullptr, std::nullopt);
    }
    append(setdefault(doc.studio, "approvals", Json::array()), std::move(record));
}

void allow_chat_approval(OpContext& c) {
    if (!person(c)) throw OpError("only a person can let approvals come through a chat");
    c.doc.studio["chat_approval"] = Json::object(
        {{"on", truthy_at(c.op, "on")}, {"by", c.actor.name()}, {"at", local_time_text()}});
}

void revoke(OpContext& c) {
    if (!person(c)) throw OpError("revoke needs a person");
    const Json gate = op_get(c.op, "gate");
    Document& doc = c.doc;
    Json& studio = doc.studio;
    if (is_text(gate, "name") || is_text(gate, "art")) {
        Page& page = doc.edit_page(studio_page(doc, c.op));
        page.art_ok = false;
        const Json* kept = get(studio, "style");  // (style = studio.get("style") or {})
        if (kept != nullptr && py_truthy(*kept) && is_text(or_get(dget(*kept, "locked"), "from_page"), page.id)) {
            pop(studio["style"], "locked");  // the pilot page is open again: so is the style
            pop(studio["style"], "locked_from_page");
        }
        if (is_text(gate, "name")) {
            page.name_ok = false;
            page.stage = "name";
        }
    } else if (is_text(gate, "sheet")) {
        Json* character = by_id(doc.bible.characters, Json(text_or(c.op, "character_id", "")));
        if (character == nullptr) throw OpError("no character " + py_str(op_get(c.op, "character_id")));
        setitem(*character, "locked", false);
    } else {
        throw OpError("gate must be name, art or sheet");
    }
    Json record = Json::object();
    record["gate"] = gate;
    record["revoked"] = true;
    record["by"] = c.actor.name();
    record["reason"] = text_or(c.op, "reason", "");
    record["rev"] = doc.revision;
    record["page_id"] = op_get(c.op, "page_id");
    append(setdefault(studio, "approvals", Json::array()), std::move(record));
}

// A page number of request_approval's "pages" (int(p): past 64 bits, Python's unbounded int, kept as its text).
struct PageNumber {
    std::optional<std::string> big;
    std::int64_t value = 0;
};

// a < b for page numbers (a number past 64 bits is past every int64 on its side)
bool page_number_less(const PageNumber& a, const PageNumber& b) {
    const auto rank = [](const PageNumber& n) { return !n.big ? 0 : (n.big->front() == '-' ? -1 : 1); };
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (!a.big) return a.value < b.value;
    const bool negative = a.big->front() == '-';
    if (a.big->size() != b.big->size()) return negative ? a.big->size() > b.big->size() : a.big->size() < b.big->size();
    return negative ? *a.big > *b.big : *a.big < *b.big;
}

void request_approval(OpContext& c) {
    const std::string gate = text_or(c.op, "gate", "");
    if (!in_names(Json(gate), kGates)) throw OpError("gate must be one of " + tuple_repr(kGates));
    std::vector<PageNumber> pages;  // sorted({int(p) for p in op.get("pages") or []})
    for (const Json& p : iterate(py_or(op_get(c.op, "pages"), Json::array()))) {
        if (const auto big = py_big_int_text(p)) {
            pages.push_back(PageNumber{*big, 0});
        } else {
            pages.push_back(PageNumber{std::nullopt, to_int(p)});
        }
    }
    std::sort(pages.begin(), pages.end(), page_number_less);
    pages.erase(std::unique(pages.begin(), pages.end(),
                            [](const PageNumber& a, const PageNumber& b) { return !page_number_less(a, b) && !page_number_less(b, a); }),
                pages.end());
    Json numbers = Json::array();
    for (const PageNumber& index : pages) {
        if (index.big) throw OpError("no page " + *index.big);
        const bool known = std::any_of(c.doc.pages.begin(), c.doc.pages.end(), [&index](const PagePtr& p) { return p->index == Num(index.value); });
        if (!known) throw OpError("no page " + std::to_string(index.value));
        numbers.push_back(index.value);
    }
    Json fields = Json::object();
    fields["kind"] = "gate";
    fields["gate"] = gate;
    fields["pages"] = std::move(numbers);
    fields["character_id"] = op_get(c.op, "character_id");
    fields["text"] = text_or(c.op, "note", "");
    new_ticket(c.doc, nullptr, c.actor, "human", fields);
}

void request_fix(OpContext& c) {
    const std::size_t at = studio_page(c.doc, c.op);
    const std::string instruction = py_strip(text_or(c.op, "instruction", ""));
    if (instruction.empty()) throw OpError("instruction is required");
    const Json frame_id = op_get(c.op, "frame_id");
    if (py_truthy(frame_id)) {
        Json& panel = panel_of(leaf(c.doc.edit_page(at), frame_id));
        if (!is_text(dget(panel, "status"), "skip")) panel["status"] = "fix_requested";
    }
    Json fields = Json::object();
    fields["kind"] = "fix";
    fields["frame_id"] = frame_id;
    fields["candidate_id"] = op_get(c.op, "candidate_id");
    fields["scope"] = text_or(c.op, "scope", "frame");
    fields["text"] = instruction;
    new_ticket(c.doc, &c.doc.page(at), c.actor, person(c) ? "agent" : "human", fields);
}

void regions_op(OpContext& c, std::string_view name) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    Json& panel = panel_of(leaf(page, op_get(c.op, "frame_id")));
    setdefault(panel, "regions", Json::array());
    const bool human = person(c);
    if (name == "add_region") {
        Json region = to_dict(py_or(op_get(c.op, "region"), Json::object()));
        if (!py_truthy(dget(region, "kind"))) throw OpError("region.kind is required");
        region["id"] = id_or_new(region, "rg_", 8);
        region["source"] = human ? "user" : "agent";
        const Json* same = by_id(panel["regions"], region["id"]);
        if (same != nullptr && py_truthy(*same)) throw OpError("region " + py_str(region["id"]) + " exists");
        append(panel["regions"], std::move(region));
    } else if (name == "edit_region" || name == "delete_region") {
        Json& regions = panel["regions"];
        Json* region = by_id(regions, Json(text_or(c.op, "id", "")));
        if (region == nullptr) throw OpError("no region " + py_str(op_get(c.op, "id")));
        if (is_text(dget(*region, "source"), "user") && !human) {
            throw OpError("a person drew this region; only a person can change it");
        }
        if (name == "edit_region") {
            const Json changes = py_or(op_get(c.op, "set"), Json::object());
            if (!changes.is_object()) raise_attribute_error(changes, "items");
            for (const auto& [key, value] : changes.items()) {
                if (key != "id" && key != "source") (*region)[key] = value;
            }
        } else {
            // (list.remove: the first region equal to it, which is the one found)
            const Json found = *region;
            for (auto it = regions.begin(); it != regions.end(); ++it) {
                if (py_equals(*it, found)) {
                    regions.erase(it);
                    break;
                }
            }
        }
    } else {
        const std::string source = text_or(c.op, "source", "agent");
        if (source == "user") throw OpError("replace_regions cannot replace a person's regions");
        Json kept = Json::array();
        for (const Json& region : iterate(panel["regions"])) {
            if (!is_text(dget(region, "source"), source)) kept.push_back(region);
        }
        for (const Json& given : iterate(py_or(op_get(c.op, "regions"), Json::array()))) {
            Json region = to_dict(given);
            region["id"] = id_or_new(region, "rg_", 8);
            region["source"] = source;
            kept.push_back(std::move(region));
        }
        panel["regions"] = std::move(kept);  // (kept + new)
    }
    const Json art = or_get(dget(panel, "adopted"), "art");
    if (py_truthy(art)) panel["regions_for"] = art;  // (the art these faces were reported on)
    refresh_brief(panel);
}

void refs_op(OpContext& c, bool bind) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    Json& panel = panel_of(leaf(page, op_get(c.op, "frame_id")));
    setdefault(panel, "refs", Json::array());
    if (bind) {
        Json ref = to_dict(py_or(op_get(c.op, "ref"), Json::object()));
        if (!py_truthy(dget(ref, "role")) || !py_truthy(dget(ref, "source"))) throw OpError("ref needs role and source");
        ref["id"] = id_or_new(ref, "rf_", 8);
        if (!ref.contains("order")) ref["order"] = static_cast<std::int64_t>(length(panel["refs"]));
        Json kept = Json::array();
        for (const Json& r : iterate(panel["refs"])) {
            if (!py_equals(dget(r, "id"), ref["id"])) kept.push_back(r);
        }
        kept.push_back(std::move(ref));
        // (refs[:] = …: a list in place; a dict cannot take a slice, a str nothing)
        if (panel["refs"].is_object()) throw PyTypeError("unhashable type: 'slice'");
        if (panel["refs"].is_string()) throw PyTypeError("'str' object does not support item assignment");
        panel["refs"] = std::move(kept);
    } else {
        Json kept = Json::array();
        const Json id = op_get(c.op, "id");
        for (const Json& r : iterate(panel["refs"])) {
            if (!py_equals(dget(r, "id"), id)) kept.push_back(r);
        }
        panel["refs"] = std::move(kept);
    }
    refresh_brief(panel);
}

void register_assets(OpContext& c) {
    setdefault(c.doc.studio, "assets", Json::object());
    const Json assets = py_or(op_get(c.op, "assets"), Json::object());
    if (!assets.is_object()) raise_attribute_error(assets, "items");
    for (const auto& [ref, meta] : assets.items()) {
        require_asset(c.doc, Json(ref));
        Json& index = c.doc.studio["assets"];
        const Json before = py_get(index, ref, Json::object());
        setitem(index, ref, merged(before, py_or(meta, Json::object())));
    }
}

void attach_reference(OpContext& c) {
    const Json target = py_or(op_get(c.op, "target"), Json::object());
    require_asset(c.doc, Json(text_or(c.op, "asset", "")));
    Json ref = Json::object();
    ref["asset"] = c.op["asset"];
    ref["kind"] = text_or(c.op, "kind", "reference");
    ref["by"] = c.actor.name();
    Json* item = nullptr;
    Json& studio = c.doc.studio;
    if (py_truthy(dget(target, "character_id"))) {
        item = by_id(c.doc.bible.characters, target["character_id"]);
    } else if (py_truthy(dget(target, "location_id"))) {
        if (studio.contains("locations")) item = by_id(studio["locations"], target["location_id"]);  // (else [])
    } else {
        const Json key(py_str(py_or(dget(target, "prop_id"), Json(""))));
        if (studio.contains("props")) item = by_id(studio["props"], key);
    }
    if (item == nullptr) throw OpError("no target " + py_str(target));
    if (py_truthy(dget(*item, "locked")) && in_names(ref["kind"], std::initializer_list<std::string_view>{"sheet", "face"}) && !person(c)) {
        throw OpError("the character sheet is approved; only a person can change it");
    }
    append(setdefault(*item, "refs", Json::array()), std::move(ref));
}

void open_request(OpContext& c) {
    Json request = to_dict(py_or(op_get(c.op, "request"), Json::object()));
    if (!py_truthy(dget(request, "id"))) throw OpError("request.id is required");
    Json& studio = c.doc.studio;
    setdefault(studio, "requests", Json::object());
    Json& requests = studio["requests"];
    require_hashable(request["id"]);
    if (!requests.is_object()) {
        // (`id in requests` and requests[id] for what is not a dict: Python's own errors)
        if (requests.is_array()) {
            for (const Json& r : requests) {
                if (py_equals(r, request["id"])) throw PyTypeError("list indices must be integers or slices, not " + py_type_name(request["id"]));
            }
            throw PyTypeError("list indices must be integers or slices, not " + py_type_name(request["id"]));
        }
        if (requests.is_string()) {
            if (!request["id"].is_string()) throw PyTypeError("'in <string>' requires string as left operand, not " + py_type_name(request["id"]));
            throw PyTypeError("'str' object does not support item assignment");
        }
        throw PyTypeError("argument of type '" + py_type_name(requests) + "' is not iterable");
    }
    const std::string key = json_key(request["id"]);
    std::string id_key = key;
    if (requests.contains(key)) {
        Json& existing = requests[key];
        if (is_text(dget(existing, "status"), "open")) return;  // same content, same id: nothing to do
        setitem(existing, "status", "open");  // asked again: another round
        request = existing;
    } else {
        request["status"] = "open";
        request["by"] = c.actor.name();
        request["rev"] = c.doc.revision;
        requests[key] = request;
    }
    const Json target = py_or(dget(request, "target"), Json::object());
    if (py_truthy(dget(target, "frame_id"))) {
        Page& page = c.doc.edit_page(studio_page(c.doc, target));
        Json& panel = panel_of(leaf(page, target["frame_id"]));
        setdefault(panel, "attempts", Json::object({{"requests", 0}, {"images", 0}, {"fix_rounds", 0}}));
        Json& attempts = panel["attempts"];
        setitem(attempts, "requests", plus(subscript(attempts, "requests"), 1));
        if (in_names(dget(request, "mode"), std::initializer_list<std::string_view>{"edit", "inpaint"})) {
            setitem(attempts, "fix_rounds", plus(subscript(attempts, "fix_rounds"), 1));
        }
        if (in_names(dget(panel, "status"), std::initializer_list<std::string_view>{"empty", "briefed", "fix_requested"})) {
            panel["status"] = "requested";
        }
    }
}

void close_request(OpContext& c) {
    const Json requests = py_get(c.doc.studio, "requests", Json::object());
    const std::string id = text_or(c.op, "id", "");
    const Json* request = dict_get(requests, id);
    if (request == nullptr || request->is_null()) throw OpError("no request " + py_str(op_get(c.op, "id")));
    setitem(c.doc.studio["requests"][id], "status", text_or(c.op, "reason", "done"));
}

// The candidates of an import_candidates op, checked and made (Python's `prepared`).
Json prepare_candidates(const Document& doc, const Json& op, const Actor& actor, const Json* request) {
    const Json items = py_or(op_get(op, "candidates"), Json::array());
    if (!py_truthy(items)) throw OpError("candidates is empty");
    const Json none_request = Json::object();
    const Json& req = request != nullptr && py_truthy(*request) ? *request : none_request;
    Json prepared = Json::array();
    std::size_t i = 0;
    for (const Json& item : iterate(items)) {
        Json origin = to_dict(py_or(dget(item, "origin"), Json::object()));
        if (!in_names(dget(origin, "kind"), kOriginKinds)) {
            throw OpError("candidates[" + std::to_string(i) + "].origin.kind must be one of " + tuple_repr(kOriginKinds));
        }
        origin["actor"] = actor.name();
        require_asset(doc, Json(py_str(py_or(dget(item, "asset"), Json("")))));
        const Json px = dget(item, "px");
        bool good = px.is_array() && px.size() == 2;
        if (good) {
            for (const Json& v : px) {  // (all(int(v) > 0 for v in px))
                if (const auto big = py_big_int_text(v); big && big->front() == '-') {
                    good = false;
                    break;
                }
                if (to_int(v) <= 0) {
                    good = false;
                    break;
                }
            }
        }
        if (!good) throw OpError("candidates[" + std::to_string(i) + "].px must be [width, height]");
        Json cand = Json::object();
        const Json given_id = dget(item, "id");
        cand["id"] = py_truthy(given_id) ? py_str(given_id) : "cd_" + new_id().substr(0, 10);
        cand["asset"] = subscript(item, "asset");
        cand["request"] = request != nullptr && py_truthy(*request) ? dget(*request, "id") : Json();
        Json mode = dget(item, "mode");
        if (!py_truthy(mode)) mode = dget(req, "mode");
        cand["mode"] = py_str(py_or(mode, Json("new")));
        Json parent = dget(item, "parent");
        cand["parent"] = py_truthy(parent) ? parent : dget(req, "parent");
        cand["origin"] = std::move(origin);
        cand["px"] = Json::array({to_int(px[0]), to_int(px[1])});
        cand["status"] = "candidate";
        cand["rev"] = doc.revision;
        if (const Json metrics = dget(item, "metrics"); metrics.is_object()) cand["metrics"] = metrics;
        if (const Json box = dget(item, "face_box01"); py_truthy(box)) {
            Json floats = Json::array();
            for (const Json& v : iterate(box)) floats.push_back(to_float(v));
            cand["face_box01"] = std::move(floats);
        }
        if (const Json mapping = dget(item, "mapping"); mapping.is_object()) cand["mapping"] = mapping;
        if (const Json upscaled = dget(item, "upscaled"); upscaled.is_object()) cand["upscaled"] = upscaled;
        prepared.push_back(std::move(cand));
        ++i;
    }
    return prepared;
}

void import_candidates(OpContext& c) {
    Document& doc = c.doc;
    Json& studio = doc.studio;
    std::optional<std::string> request_key;  // where the request is in studio["requests"]
    Json request;
    if (truthy_at(c.op, "request_id")) {
        const std::string key = py_str(c.op["request_id"]);
        const Json requests = py_get(studio, "requests", Json::object());
        const Json* found = dict_get(requests, key);
        if (found == nullptr || found->is_null()) throw OpError("no request " + py_str(c.op["request_id"]));
        request = *found;
        request_key = key;
    }
    const Json* req = request_key ? &request : nullptr;
    Json target = to_dict(py_or(req != nullptr ? or_get(request, "target") : Json(), Json::object()));
    for (const char* key : {"page", "frame_id", "character_id", "location_id"}) {
        const Json value = op_get(c.op, key);
        if (!value.is_null()) target[key] = value;
    }
    Json prepared = prepare_candidates(doc, c.op, c.actor, req);
    // (the request is "done" once the candidates are in: on the book's own record of it)
    const auto request_done = [&] {
        if (request_key) setitem(studio["requests"][*request_key], "status", "done");
    };
    const Json character_id = dget(target, "character_id");
    const Json location_id = dget(target, "location_id");
    if (py_truthy(character_id) || py_truthy(location_id)) {
        const char* key = py_truthy(character_id) ? "character_candidates" : "location_candidates";
        const Json owner = py_truthy(character_id) ? character_id : location_id;
        Json& owners = setdefault(studio, key, Json::object());
        if (!owners.is_object()) raise_attribute_error(owners, "setdefault");
        const std::string owner_key = json_key(owner);  // (unhashable: TypeError)
        setdefault(owners, owner_key, Json::array());
        Json& bucket = studio[key][owner_key];
        std::map<std::string, std::size_t> known;  // {c["asset"]: c for c in bucket}: the last of an asset
        std::size_t n = 0;
        for (const Json& cand : iterate(bucket)) known[hash_key(subscript(cand, "asset"))] = n++;
        for (Json& cand : prepared) {
            const auto it = known.find(hash_key(cand["asset"]));
            if (it == known.end()) {
                append(bucket, cand);
                continue;
            }
            // (the same picture again: the old entry takes what is new about it, and is a candidate again)
            Json& same = bucket[it->second];
            for (const char* field : {"face_box01", "metrics", "mapping"}) {
                if (cand.contains(field)) setitem(same, field, cand[field]);
            }
            if (is_text(dget(same, "status"), "withdrawn")) same["status"] = "candidate";
        }
        request_done();
        if (py_truthy(character_id) && doc.tickets.is_array()) {
            for (Json& ticket : doc.tickets) {  // new sheets answer a person's send-back
                if (is_text(dget(ticket, "kind"), "fix") && is_text(dget(ticket, "status"), "open") &&
                    py_equals(dget(ticket, "character_id"), owner)) {
                    ticket["status"] = "done";
                    ticket["closed_by"] = c.actor.name();
                }
            }
        }
        for (const Json& cand : prepared) {
            if (cand.contains("face_box01")) require_finite(cand["face_box01"], "face_box01");
        }
        return;
    }
    const std::size_t at = studio_page(doc, target);
    try {
        (void)leaf_of(doc.page(at), dget(target, "frame_id"));  // (the page is not copied for a change it does not get)
    } catch (const OpError&) {
        Json orphan = Json::object();
        orphan["kind"] = "candidates";
        orphan["page_id"] = doc.page(at).id;
        orphan["frame_id"] = dget(target, "frame_id");
        orphan["candidates"] = prepared;
        orphan["rev"] = doc.revision;
        append(setdefault(studio, "orphans", Json::array()), std::move(orphan));
        for (const Json& cand : prepared) {
            if (cand.contains("face_box01")) require_finite(cand["face_box01"], "face_box01");
        }
        return;
    }
    Page& page = doc.edit_page(at);
    Frame* frame = &leaf(page, dget(target, "frame_id"));
    Json& panel = panel_of(*frame);
    const std::string current = brief_hash(panel);
    std::set<std::string> known;  // {c["asset"] for c in panel.get("candidates", [])}
    for (const Json& cand : iterate(py_get(panel, "candidates", Json::array()))) known.insert(hash_key(subscript(cand, "asset")));
    Json fresh = Json::array();
    for (const Json& cand : prepared) {
        if (!known.contains(hash_key(cand["asset"]))) fresh.push_back(cand);
    }
    const Json none = Json::object();
    const Json& req_or = req != nullptr && py_truthy(request) ? request : none;
    for (Json& cand : fresh) {
        cand["brief_hash"] = py_or(dget(req_or, "brief_hash"), Json(current));
        cand["stale"] = !is_text(cand["brief_hash"], current);
        const Json mapping = py_or(dget(cand, "mapping"), Json::object());
        const Json* given_pad = dict_get(mapping, "pad_mm");
        const Json pad = given_pad != nullptr ? *given_pad : py_or(dget(req_or, "pad_mm"), Json(0.0));
        Json map = Json::object();
        map["frame_rect_mm"] = Json::array({frame->rect.x.json(), frame->rect.y.json(), frame->rect.width.json(), frame->rect.height.json()});
        map["pad_mm"] = to_float(py_or(pad, Json(0.0)));
        map["px"] = cand["px"];
        cand["mapping"] = std::move(map);  // (a derived or enlarged picture keeps its parent's margin: it lies on it)
    }
    std::int64_t images = 0;
    for (const Json& cand : fresh) {
        if (!is_text(dget(cand["origin"], "kind"), "genko")) ++images;  // Genko's own derivations are free
    }
    Json& candidates = setdefault(panel, "candidates", Json::array());
    if (!candidates.is_array()) raise_attribute_error(candidates, "extend");
    for (Json& cand : fresh) candidates.push_back(cand);
    setdefault(panel, "attempts", Json::object({{"requests", 0}, {"images", 0}, {"fix_rounds", 0}}));
    Json& attempts = panel["attempts"];
    setitem(attempts, "images", plus(subscript(attempts, "images"), images));
    if (!in_names(dget(panel, "status"), std::initializer_list<std::string_view>{"adopted", "skip"})) panel["status"] = "candidates";
    request_done();
    for (const Json& cand : fresh) {
        if (cand.contains("face_box01")) require_finite(cand["face_box01"], "face_box01");
        require_finite(cand["mapping"]["pad_mm"], "pad_mm");
    }
}

void candidates_op(OpContext& c, bool review) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    Json& panel = panel_of(leaf(page, op_get(c.op, "frame_id")));
    Json empty = Json::array();
    if (!panel.is_object()) raise_attribute_error(panel, "get");
    Json& cands = panel.contains("candidates") ? panel["candidates"] : empty;
    const std::vector<Json> items = review ? iterate(py_or(op_get(c.op, "reviews"), Json::array())) : std::vector<Json>{c.op};
    for (const Json& item : items) {
        Json* cand = by_id(cands, Json(py_str(py_or(dget(item, "candidate_id"), Json("")))));
        if (cand == nullptr) throw OpError("no candidate " + py_str(dget(item, "candidate_id")));
        if (review) {
            Json record = Json::object();
            record["by"] = c.actor.name();
            record["score"] = dget(item, "score");
            record["note"] = py_get(item, "note", Json(""));
            record["fix"] = dget(item, "fix");
            if (const Json checks = dget(item, "checks"); checks.is_object()) record["checks"] = checks;
            setitem(*cand, "review", std::move(record));
        } else {
            if (!in_names(dget(item, "status"), kCandidateStatuses)) {
                throw OpError("status must be one of " + tuple_repr(kCandidateStatuses));
            }
            setitem(*cand, "status", subscript(item, "status"));
        }
    }
}

// What adopt_candidate and set_placement give _place: the offset (tuple(op.get("offset_mm") or (0.0, 0.0))) and the
// scale (float(op.get("scale") or 1.0)).
std::vector<Json> offset_of(const Json& op) {
    return to_tuple(py_or(op_get(op, "offset_mm"), Json::array({0.0, 0.0})));
}

double scale_of(const Json& op) { return to_float(py_or(op_get(op, "scale"), Json(1.0))); }

void adopt_candidate(OpContext& c, const StudioHooks& hooks) {
    Document& doc = c.doc;
    const std::size_t at = studio_page(doc, c.op);
    Page& page = doc.edit_page(at);
    Frame& frame = leaf(page, op_get(c.op, "frame_id"));
    Json& panel = panel_of(frame);
    const std::string to = text_or(c.op, "to", "art");
    if (!in_names(Json(to), kAdoptTargets)) throw OpError("to must be one of " + tuple_repr(kAdoptTargets));
    if (to != "draft" && !page.name_ok) {
        throw OpError("page " + page.index.repr() + ": art can be adopted only after the name is approved");
    }
    const Json candidates = py_get(panel, "candidates", Json::array());
    const Json* found = by_id(candidates, Json(text_or(c.op, "candidate_id", "")));
    if (found == nullptr) throw OpError("no candidate " + py_str(op_get(c.op, "candidate_id")));
    const Json cand = *found;
    const auto existing = art_layer(page, frame.id, to);
    Layer layer;
    if (existing) {
        layer = page.layers[*existing];
    } else {
        layer.id = new_id();
        layer.role = to == "draft" ? LayerRole::Draft : (to == "bg" ? LayerRole::Bg : LayerRole::User);
        layer.kind = LayerKind::Placed;
        layer.exportable = to != "draft";
        layer.title = to + " " + frame.id;
        layer.frame_id = frame.id;
        layer.source = Json::object({{"to", to}});
    }
    layer.asset = subscript(cand, "asset").is_string() ? std::optional<std::string>(cand["asset"].get<std::string>())
                                                       : std::optional<std::string>(py_str(cand["asset"]));
    layer.source = Json::object({{"candidate", subscript(cand, "id")}, {"request", dget(cand, "request")}, {"to", to}});
    const Json clip_given = op_get(c.op, "clip_to");
    const std::string clip_to = py_truthy(clip_given) ? py_str(clip_given) : (frame.bleed ? "bleed" : "frame");
    const Json px = py_or(dget(cand, "px"), Json::array({1, 1}));
    const std::string fit = text_or(c.op, "fit", "cover");
    const std::vector<Json> offset = offset_of(c.op);
    const double scale = scale_of(c.op);
    const double pad = pad_of(cand);
    place(page, &frame, layer, px, fit, clip_to, offset, scale, pad);
    std::size_t layer_at = 0;
    if (existing) {
        page.layers[*existing] = std::move(layer);
        layer_at = *existing;
    } else {
        layer_at = insert_below_ink(page, std::move(layer));
    }
    // (frame and panel are still where they were: only the layers changed)
    setdefault(panel, "adopted", Json::object());
    setdefault(panel, "adopt_history", Json::array());
    const Json cand_id = cand["id"];
    const Json adopted_art = dget(panel["adopted"], "art");
    if (to == "art" && py_truthy(adopted_art) && !py_equals(adopted_art, cand_id)) {
        if (py_truthy(dget(panel, "regions")) && !py_truthy(dget(panel, "regions_for"))) {
            panel["regions_for"] = adopted_art;  // (the faces reported so far are where they are in the old art)
        }
        if (page.stage == "finish") page.stage = "ink";  // (the finish was made for the old art)
    }
    const Json before = dget(panel["adopted"], to);
    if (py_truthy(before)) append(panel["adopt_history"], Json::object({{"to", to}, {"candidate", before}}));
    setitem(panel["adopted"], to, cand_id);
    if (to == "art") {
        panel["status"] = "adopted";
        if (page.art_ok && !person(c)) page.art_ok = false;  // a person approved different art
        for (Json& ticket : doc.tickets) {
            if (is_text(dget(ticket, "kind"), "fix") && is_text(dget(ticket, "status"), "open") && is_text(dget(ticket, "frame_id"), frame.id)) {
                ticket["status"] = "done";
                ticket["closed_by"] = c.actor.name();
            }
        }
    }
    require_finite_placement(page.layers[layer_at]);
    require_drawable(doc, page.layers[layer_at].asset, hooks);
}

void unadopt(OpContext& c) {
    const std::size_t at = studio_page(c.doc, c.op);
    Page& page = c.doc.edit_page(at);
    Frame& frame = leaf(page, op_get(c.op, "frame_id"));
    Json& panel = panel_of(frame);
    const std::string to = text_or(c.op, "to", "art");
    const auto found = art_layer(page, frame.id, to);
    if (!found) throw OpError("nothing adopted as " + to + " in " + frame.id);
    setdefault(panel, "adopt_history", Json::array());
    std::optional<Json> previous;  // next((h for h in reversed(history) if h["to"] == to), None)
    const Json& history_seen = panel["adopt_history"];
    if (!history_seen.is_array() && !history_seen.is_object() && !history_seen.is_string()) {
        throw PyTypeError("'" + py_type_name(history_seen) + "' object is not reversible");
    }
    const std::vector<Json> items = iterate(history_seen);  // (a dict's keys, a str's characters: h["to"] fails)
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (is_text(subscript(*it, "to"), to)) {
            previous = *it;
            break;
        }
    }
    const Json candidates = py_get(panel, "candidates", Json::array());
    const Json* prev_cand = previous ? by_id(candidates, subscript(*previous, "candidate")) : nullptr;
    if (prev_cand != nullptr) {
        const Json prev = *prev_cand;
        Json& history = panel["adopt_history"];
        for (auto it = history.begin(); it != history.end(); ++it) {  // (list.remove: the first one equal to it)
            if (py_equals(*it, *previous)) {
                history.erase(it);
                break;
            }
        }
        Layer& layer = page.layers[*found];
        layer.asset = py_str(subscript(prev, "asset"));
        layer.source = Json::object({{"candidate", subscript(prev, "id")}, {"request", dget(prev, "request")}, {"to", to}});
        const Json px = py_or(dget(prev, "px"), Json::array({1, 1}));
        const double pad = pad_of(prev);
        place(page, &frame, layer, px, layer.fit, layer.clip_to, {Json(0.0), Json(0.0)}, 1.0, pad);
        setitem(setdefault(panel, "adopted", Json::object()), to, prev["id"]);
        require_finite_placement(page.layers[*found]);
    } else {
        page.layers.erase(page.layers.begin() + static_cast<std::ptrdiff_t>(*found));
        pop(setdefault(panel, "adopted", Json::object()), to);
        if (to == "art") panel["status"] = py_truthy(dget(panel, "candidates")) ? "candidates" : "briefed";
    }
    if (to == "art" && !person(c)) page.art_ok = false;
}

void set_placement(OpContext& c, const StudioHooks& hooks) {
    Document& doc = c.doc;
    const std::size_t at = studio_page(doc, c.op);
    Page& page = doc.edit_page(at);
    Frame& frame = leaf(page, op_get(c.op, "frame_id"));
    const std::string to = text_or(c.op, "to", "art");
    const auto found = art_layer(page, frame.id, to);
    if (!found) throw OpError("nothing adopted as " + to + " in " + frame.id);
    Json& panel = panel_of(frame);
    const Layer& current = page.layers[*found];
    const Json source = current.source ? *current.source : Json();
    const Json candidates = py_get(panel, "candidates", Json::array());
    const Json* cand = by_id(candidates, py_truthy(source) ? py_get(source, "candidate", Json("")) : Json(""));
    Json px;
    const bool sized_here = cand == nullptr || !py_truthy(*cand);  // (no candidate: the picture's own size)
    if (!sized_here) {
        px = dget(*cand, "px");
    } else {
        const auto size = asset_size(doc, current.asset ? Json(*current.asset) : Json(), hooks);
        px = Json::array({size.first, size.second});
    }
    const std::optional<Rect> before = current.placement_mm;
    const Json fit_given = op_get(c.op, "fit");
    const std::string fit = py_truthy(fit_given) ? py_str(fit_given) : current.fit;
    const Json clip_given = op_get(c.op, "clip_to");
    const std::string clip_to = py_truthy(clip_given) ? py_str(clip_given) : current.clip_to;
    const std::vector<Json> offset = offset_of(c.op);
    const double scale = scale_of(c.op);
    const double pad = pad_of(cand != nullptr && py_truthy(*cand) ? *cand : Json::object());
    Layer& layer = page.layers[*found];
    place(page, &frame, layer, px, fit, clip_to, offset, scale, pad);
    const std::optional<Rect> after = layer.placement_mm;
    if (to == "art" && before && after && before->width > Num(0) && before->height > Num(0)) {
        // the faces and people reported on the picture move and scale with it
        const double sx = after->width.value() / before->width.value();
        const double sy = after->height.value() / before->height.value();
        const Json regions = py_or(py_get(panel, "regions", Json::array()), Json::array());
        const std::vector<Json> items = iterate(regions);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Json rect = dget(items[i], "rect_mm");
            if (!rect.is_array() || rect.size() != 4) continue;
            const std::vector<double> v = unpack_floats(rect, 4);
            Json moved = Json::array({py_round(after->x.value() + (v[0] - before->x.value()) * sx, 2),
                                      py_round(after->y.value() + (v[1] - before->y.value()) * sy, 2),
                                      py_round(v[2] * sx, 2), py_round(v[3] * sy, 2)});
            panel["regions"][i]["rect_mm"] = std::move(moved);
        }
    }
    require_finite_placement(page.layers[*found]);
    if (panel.contains("regions") && panel["regions"].is_array()) {
        for (const Json& region : panel["regions"]) {
            if (region.is_object() && region.contains("rect_mm")) require_finite(region["rect_mm"], "rect_mm");
        }
    }
    if (sized_here) require_drawable(doc, page.layers[*found].asset, hooks);
}

void place_asset(OpContext& c, const StudioHooks& hooks) {
    Document& doc = c.doc;
    const std::size_t at = studio_page(doc, c.op);
    const std::string asset = text_or(c.op, "asset", "");
    if (!asset.starts_with("sha256:")) throw OpError("place_asset takes an asset ref (sha256:…), not a path");
    const auto px = asset_size(doc, Json(asset), hooks);
    const std::string to = text_or(c.op, "to", "art");
    LayerRole role = LayerRole::User;
    if (to == "art") role = LayerRole::User;
    else if (to == "bg") role = LayerRole::Bg;
    else if (to == "draft") role = LayerRole::Draft;
    else if (to == "name") role = LayerRole::Name;
    else throw OpError("to must be art, bg, draft or name");
    Page& page = doc.edit_page(at);
    if ((to == "art" || to == "bg") && doc.strict_gates && !page.name_ok) {
        throw OpError("page " + page.index.repr() + ": approve the name first (strict_gates)");
    }
    const Frame* frame = truthy_at(c.op, "frame_id") ? &leaf(page, c.op["frame_id"]) : nullptr;
    Layer layer;
    layer.id = new_id();
    layer.role = role;
    layer.kind = LayerKind::Placed;
    layer.exportable = to == "art" || to == "bg";
    layer.title = "placed " + to;
    if (frame != nullptr) layer.frame_id = frame->id;
    layer.asset = asset;
    layer.source = Json::object({{"asset", asset}, {"to", to}, {"by", c.actor.name()}});
    if (truthy_at(c.op, "placement_mm")) {
        // an explicit position (e.g. a scanned name aligned to the page): the whole image goes there
        const std::vector<double> box = unpack_floats(c.op["placement_mm"], 4);
        layer.placement_mm = Rect{Num(box[0]), Num(box[1]), Num(box[2]), Num(box[3])};
        layer.fit = "stretch";
        layer.clip_to = text_or(c.op, "clip_to", "none");
        layer.title = text_or(c.op, "title", layer.title);
    } else {
        const std::string fit = text_or(c.op, "fit", "cover");
        const std::string clip_to = text_or(c.op, "clip_to", frame != nullptr ? "frame" : "none");
        place(page, frame, layer, Json::array({px.first, px.second}), fit, clip_to, {Json(0.0), Json(0.0)}, 1.0, 0.0);
        layer.title = text_or(c.op, "title", layer.title);
    }
    const std::size_t placed = insert_below_ink(page, std::move(layer));
    require_finite_placement(page.layers[placed]);
    require_drawable(doc, asset, hooks);
}

void set_finish(OpContext& c) {
    Page& page = c.doc.edit_page(studio_page(c.doc, c.op));
    const std::string frame_id = leaf(page, op_get(c.op, "frame_id")).id;
    const Json finish = op_get(c.op, "finish");
    for (Layer& layer : page.layers) {
        // (smart-objects@1: a picture's smart object in the panel is finished as its placed art)
        if ((layer.kind == LayerKind::Placed || layer.kind == LayerKind::Smart) && layer.frame_id == frame_id) {
            layer.finish = finish.is_null() ? std::nullopt : std::optional<Json>(finish);
        }
    }
}

// _frame_from_tree(node)
Frame frame_from_tree(const Json& node) {
    const std::vector<double> box = unpack_floats(subscript(node, "rect_mm"), 4);
    Frame frame;
    frame.id = new_id();
    frame.rect = Rect{Num(py_round(box[0], 2)), Num(py_round(box[1], 2)), Num(py_round(box[2], 2)), Num(py_round(box[3], 2))};
    const Json children = py_or(dget(node, "children"), Json::array());
    if (py_truthy(children) || is_text(dget(node, "axis"), "free")) {
        if (!in_names(dget(node, "axis"), std::initializer_list<std::string_view>{"horizontal", "vertical", "free"})) {
            throw OpError("a node with children needs axis horizontal, vertical or free");
        }
        frame.split_axis = node["axis"].get<std::string>();
        for (const Json& child : iterate(children)) frame.children.push_back(frame_from_tree(child));
        if (const Json split = dget(node, "split"); split.is_object()) frame.split = split;
    }
    if (py_truthy(dget(node, "poly"))) {
        std::vector<Point> poly;
        for (const Json& p : iterate(node["poly"])) {
            const double x = to_float(subscript(p, 0));
            const double y = to_float(subscript(p, 1));
            poly.push_back(Point{Num(x), Num(y)});
        }
        frame.poly = std::move(poly);
    }
    if (node.contains("bleed")) frame.bleed = py_truthy(node["bleed"]);
    if (node.contains("clip")) frame.clip = py_truthy(node["clip"]);
    if (node.contains("custom")) frame.custom = py_truthy(node["custom"]);
    if (const Json border = dget(node, "border_mm"); !border.is_null()) frame.border_mm = to_float(border);
    if (const Json curves = dget(node, "curves"); curves.is_array()) {
        std::vector<double> values;
        for (const Json& v : curves) values.push_back(to_float(v));
        frame.curves = std::move(values);
    }
    if (const Json line = dget(node, "line"); line.is_object()) frame.line = line;
    if (py_truthy(dget(node, "corner_mm"))) frame.corner_mm = to_float(node["corner_mm"]);
    return frame;
}

// Every number of a panel tree kept finite (C++ only).
void require_finite_tree(const Frame& frame) {
    require_finite(rect_to_json(frame.rect), "rect_mm");
    if (frame.poly) {
        for (const Point& p : *frame.poly) require_finite(Json::array({p.x.json(), p.y.json()}), "poly");
    }
    require_finite(Json(frame.border_mm), "border_mm");
    if (frame.curves) require_finite(Json(*frame.curves), "curves");
    require_finite(Json(frame.corner_mm), "corner_mm");
    for (const Frame& child : frame.children) require_finite_tree(child);
}

void set_layout(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = studio_page(doc, c.op);
    const Json tree = py_or(op_get(c.op, "tree"), Json::object());
    if (!py_truthy(dget(tree, "rect_mm"))) throw OpError("tree needs rect_mm");
    const Page& seen = doc.page(at);
    const bool blank = seen.leaf_frames().size() == 1 && doc.story_for_page(seen.index).empty();
    if (!blank && !truthy_at(c.op, "force")) {
        throw OpError("page " + seen.index.repr() + " already has panels or lines; pass force to replace the layout");
    }
    Page& page = doc.edit_page(at);
    if (!blank) {
        std::set<std::string> leaves;
        for (const Frame* frame : page.leaf_frames()) leaves.insert(frame->id);
        orphan_art(doc, page, placed_on(page, leaves), "set_layout");
    }
    Frame root = frame_from_tree(tree);
    page.frames.clear();
    page.frames.push_back(std::move(root));
    page.selected_frame_id = nullptr;
    require_finite_tree(page.frames.front());
}

void propose(OpContext& c) {
    Json proposal = to_dict(py_or(op_get(c.op, "proposal"), Json::object()));
    if (!in_names(dget(proposal, "kind"), std::initializer_list<std::string_view>{"layout", "lines"})) {
        throw OpError("proposal.kind must be layout or lines");
    }
    Document& doc = c.doc;
    const Page& page = doc.page(studio_page(doc, proposal));
    const Json given = dget(proposal, "id");
    const std::string pid = py_truthy(given) ? py_str(given) : "pr_" + new_id().substr(0, 10);
    setdefault(doc.studio, "proposals", Json::object());
    Json& proposals = doc.studio["proposals"];
    if (!proposals.is_object()) {  // (`pid in proposals`, then proposals[pid] or proposals.values())
        if (proposals.is_array()) {
            const bool listed = std::any_of(proposals.begin(), proposals.end(), [&pid](const Json& p) { return is_text(p, pid); });
            if (listed) throw PyTypeError("list indices must be integers or slices, not str");
            raise_attribute_error(proposals, "values");
        }
        if (proposals.is_string()) {
            if (proposals.get_ref<const std::string&>().find(pid) != std::string::npos) {
                throw PyTypeError("string indices must be integers, not 'str'");
            }
            raise_attribute_error(proposals, "values");
        }
        throw PyTypeError("argument of type '" + py_type_name(proposals) + "' is not iterable");
    }
    if (proposals.contains(pid)) {
        Json& existing = proposals[pid];
        if (in_names(dget(existing, "status"), std::initializer_list<std::string_view>{"rejected", "superseded"})) {  // asked again
            update(existing, Json::object({{"status", "open"}, {"by", c.actor.name()}, {"rev", doc.revision}}));
            pop(existing, "resolved_by");
        }
        return;  // the same proposal again
    }
    for (auto& [key, other] : proposals.items()) {  // a newer proposal of the same kind for the page replaces an open one
        if (is_text(dget(other, "status"), "open") && py_equals(dget(other, "kind"), proposal["kind"]) &&
            is_text(dget(other, "page_id"), page.id)) {
            other["status"] = "superseded";
        }
    }
    update(proposal, Json::object({{"id", pid}, {"page_id", page.id}, {"page", page.index.json()}, {"status", "open"},
                                   {"by", c.actor.name()}, {"rev", doc.revision}}));
    proposals[pid] = std::move(proposal);
}

void resolve_proposal(OpContext& c) {
    if (!person(c)) throw OpError("only a person accepts or rejects a proposal");
    const std::string id = text_or(c.op, "id", "");
    const Json proposals = py_get(c.doc.studio, "proposals", Json::object());
    const Json* proposal = dict_get(proposals, id);
    if (proposal == nullptr || proposal->is_null()) throw OpError("no proposal " + py_str(op_get(c.op, "id")));
    const std::string status = text_or(c.op, "status", "");
    if (status != "accepted" && status != "rejected") throw OpError("status must be accepted or rejected");
    update(c.doc.studio["proposals"][id],
           Json::object({{"status", status}, {"resolved_by", c.actor.name()}, {"note", text_or(c.op, "note", "")}}));
}

// The page and leaf panel a ticket names (next(...) over the pages and their panels), or none: looked at only (the
// page is copied for a change by panel_to_change when one is made).
std::pair<std::size_t, const Frame*> ticket_panel(const Document& doc, const Json& page_index, const Json& frame_id) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!py_equals(doc.pages[i]->index.json(), page_index)) continue;
        for (const Frame* frame : doc.pages[i]->leaf_frames()) {
            if (is_text(frame_id, frame->id)) return {i, frame};
        }
        return {i, nullptr};
    }
    return {0, nullptr};
}

// That panel's brief, to change.
Json& panel_to_change(Document& doc, const std::pair<std::size_t, const Frame*>& found) {
    const std::string id = found.second->id;  // (read before the page is copied)
    return panel_of(*doc.edit_page(found.first).find_frame(id));
}

void tickets_op(OpContext& c, bool reopen) {
    Document& doc = c.doc;
    const std::string id = text_or(c.op, "id", "");
    Json* ticket = nullptr;
    if (doc.tickets.is_array()) {
        for (Json& t : doc.tickets) {
            if (is_text(dget(t, "id"), id)) {
                ticket = &t;
                break;
            }
        }
    }
    if (ticket == nullptr) throw OpError("no ticket " + py_str(op_get(c.op, "id")));
    const std::string note = py_strip(text_or(c.op, "note", ""));
    const bool human = person(c);
    if (reopen) {
        if (!human) throw OpError("only a person can reopen a ticket");
        update(*ticket, Json::object({{"status", "open"}, {"reopened_by", c.actor.name()}, {"reopen_note", note}}));
        const Json frame_id = dget(*ticket, "frame_id");
        const Json page_index = dget(*ticket, "page_index");
        if (py_truthy(frame_id) && py_truthy(page_index)) {
            const auto found = ticket_panel(doc, page_index, frame_id);
            const Frame* frame = found.second;
            if (frame != nullptr && !is_text(or_get(frame->panel ? *frame->panel : Json(), "status"), "skip")) {
                setitem(panel_to_change(doc, found), "status", "fix_requested");
            }
        }
        return;
    }
    if (!is_text(dget(*ticket, "status"), "open")) throw OpError("ticket " + py_str((*ticket)["id"]) + " is not open");
    if (!human && (!is_text(dget(*ticket, "kind"), "fix") || !is_text(dget(*ticket, "assignee"), "agent"))) {
        throw OpError("an agent resolves only a person's fix instruction (approvals and questions are closed by a person)");
    }
    if (note.empty() && !human) throw OpError("note is required");
    update(*ticket, Json::object({{"status", "done"}, {"resolved_by", c.actor.name()}, {"resolved_note", note},
                                  {"resolved_rev", doc.revision}}));
    const Json frame_id = dget(*ticket, "frame_id");
    const auto found = py_truthy(frame_id) ? ticket_panel(doc, dget(*ticket, "page_index"), frame_id)
                                           : std::pair<std::size_t, const Frame*>{0, nullptr};
    const Frame* frame = found.second;
    bool still = false;
    for (const Json& t : doc.tickets) {
        if (is_text(dget(t, "kind"), "fix") && is_text(dget(t, "status"), "open") && py_equals(dget(t, "frame_id"), frame_id)) {
            still = true;
            break;
        }
    }
    if (frame != nullptr && is_text(or_get(frame->panel ? *frame->panel : Json(), "status"), "fix_requested") && !still) {
        const bool adopted = py_truthy(or_get(dget(*frame->panel, "adopted"), "art"));
        panel_to_change(doc, found)["status"] = adopted ? "adopted" : "briefed";
    }
}

void withdraw_candidates(OpContext& c) {
    Document& doc = c.doc;
    const std::string cid = text_or(c.op, "character_id", "");
    const std::string lid = text_or(c.op, "location_id", "");
    const std::string owner = !cid.empty() ? cid : lid;
    const char* bucket_key = !cid.empty() ? "character_candidates" : "location_candidates";
    const Json cands = py_get(py_get(doc.studio, bucket_key, Json::object()), owner, Json::array());
    if (owner.empty() || !py_truthy(cands)) {
        throw OpError("withdraw_candidates needs character_id (or location_id) with candidates");
    }
    std::vector<std::string> wanted;
    for (const Json& v : iterate(py_or(op_get(c.op, "candidate_ids"), Json::array()))) wanted.push_back(py_str(v));
    if (wanted.empty()) throw OpError("candidate_ids: the candidates to take back, e.g. [\"cd_…\"]");
    const Json* item = nullptr;
    const Json locations = py_get(doc.studio, "locations", Json::array());
    if (!cid.empty()) {
        item = by_id(doc.bible.characters, Json(cid));
    } else {
        item = by_id(locations, Json(lid));
    }
    PySet approved;
    for (const Json& ref : iterate(py_get(item != nullptr && py_truthy(*item) ? *item : Json::object(), "refs", Json::array()))) {
        if (in_names(dget(ref, "kind"), std::initializer_list<std::string_view>{"sheet", "face"})) approved.add(dget(ref, "asset"));
    }
    const bool human = person(c);
    Json& bucket = doc.studio[bucket_key][owner];
    for (const std::string& key : wanted) {
        Json* cand = by_id(bucket, Json(key));
        if (cand == nullptr) {
            std::string ids;  // ', '.join(c['id'] for c in cands)
            std::size_t n = 0;
            for (const Json& each : iterate(bucket)) {
                const Json id = subscript(each, "id");
                if (!id.is_string()) {
                    throw PyTypeError("sequence item " + std::to_string(n) + ": expected str instance, " + py_type_name(id) + " found");
                }
                ids += (n > 0 ? ", " : "") + id.get<std::string>();
                ++n;
            }
            throw OpError("no candidate " + key + " for " + owner + " (its candidates: " + ids + ")");
        }
        if (approved.contains(subscript(*cand, "asset")) && !human) throw OpError(key + " is the approved sheet: only a person changes it");
        if (!human) {
            const Json from = or_get(dget(*cand, "origin"), "actor");
            if (!(from.is_null() || is_text(from, c.actor.name()))) {
                throw OpError(key + " came from " + py_str(from) + ": an agent takes back only its own");
            }
        }
        (*cand)["status"] = "withdrawn";
        (*cand)["withdrawn_by"] = c.actor.name();
    }
}

void reject_sheet(OpContext& c) {
    if (!person(c)) throw OpError("reject_sheet needs a person");
    Document& doc = c.doc;
    const std::string cid = text_or(c.op, "character_id", "");
    const Json cands = py_get(py_get(doc.studio, "character_candidates", Json::object()), cid, Json::array());
    Json given = op_get(c.op, "candidate_ids");
    if (!py_truthy(given)) {  // ([c["id"] for c in cands])
        given = Json::array();
        for (const Json& cand : iterate(cands)) given.push_back(subscript(cand, "id"));
    }
    const PySet wanted = to_set(given);
    if (cands.is_array() && !cands.empty()) {  // (the live list: studio["character_candidates"][cid])
        for (Json& cand : doc.studio["character_candidates"][cid]) {
            if (wanted.contains(subscript(cand, "id"))) setitem(cand, "status", "rejected");
        }
    } else {
        for (const Json& cand : iterate(cands)) (void)subscript(cand, "id");
    }
    close_tickets(doc, c.actor, "gate", "sheet", nullptr, cid);
    for (Json& ticket : doc.tickets) {  // (a list: close_tickets went through it)
        if (is_text(dget(ticket, "kind"), "gate") && is_text(dget(ticket, "gate"), "sheet") &&
            is_text(dget(ticket, "character_id"), cid) && is_text(dget(ticket, "status"), "done")) {
            ticket["status"] = "returned";
        }
    }
    const std::string note = py_strip(text_or(c.op, "note", ""));
    if (!note.empty()) {
        Json fields = Json::object();
        fields["kind"] = "fix";
        fields["character_id"] = cid;
        fields["text"] = note;
        fields["scope"] = "sheet";
        new_ticket(doc, nullptr, c.actor, "agent", fields);
    }
}

void ask_human(OpContext& c) {
    const std::string text = py_strip(text_or(c.op, "text", ""));
    if (text.empty()) throw OpError("text is required");
    const Page* page = nullptr;
    if (!op_get(c.op, "page").is_null() || truthy_at(c.op, "page_id")) {
        const std::size_t at = studio_page(c.doc, c.op);
        if (truthy_at(c.op, "frame_id")) (void)leaf_of(c.doc.page(at), c.op["frame_id"]);  // (looked at: the page is not changed)
        page = &c.doc.page(at);
    }
    Json fields = Json::object();
    fields["kind"] = "help";
    fields["frame_id"] = op_get(c.op, "frame_id");
    fields["item"] = op_get(c.op, "item");
    fields["text"] = text;
    new_ticket(c.doc, page, c.actor, "human", fields);
}

// --- ops._apply_one's: set_bible, add_ticket, set_ticket -------------------------------------------------------------

void set_bible(OpContext& c) {
    Bible& bible = c.doc.bible;
    const Json before = bible.characters;
    if (const Json* plot = get(c.op, "plot")) bible.plot = py_str(*plot);
    if (const Json* characters = get(c.op, "characters")) bible.characters = py_list(*characters);
    if (const Json* constraints = get(c.op, "constraints")) {
        Json out = Json::array();
        for (const Json& item : iterate(*constraints)) out.push_back(py_str(item));
        bible.constraints = std::move(out);
    }
    // (C++ only: Python keeps a character that is not an object, and then cannot save the book — io._gates — nor
    // run a studio op that looks the characters up)
    if (has(c.op, "characters")) {
        for (const Json& character : bible.characters) {
            if (!character.is_object()) throw OpError("each of the bible's characters must be an object, not " + py_repr(character));
        }
        // (C++ only, as upsert_character's)
        if (!person(c) && approvals_of(before) != approvals_of(bible.characters)) throw OpError(kApprovalIsPersons);
    }
}

void add_ticket(OpContext& c) {
    const Page& page = c.doc.page(require_page(c.doc, c.op));
    Json ticket = Json::object();
    const Json id = op_get(c.op, "id");
    ticket["id"] = py_truthy(id) ? py_str(id) : new_id();
    ticket["page_index"] = page.index.json();
    ticket["page_id"] = page.id;
    ticket["frame_id"] = op_get(c.op, "frame_id");
    ticket["role"] = text_or(c.op, "role", "bg");
    ticket["assignee"] = text_or(c.op, "assignee", "human");
    ticket["rate"] = text_or(c.op, "rate", "");
    ticket["status"] = "open";
    append(c.doc.tickets, std::move(ticket));
}

void set_ticket(OpContext& c) {
    const Json ticket_id = op_get(c.op, "id");
    for (Json& ticket : c.doc.tickets) {  // (a list, as the book keeps it)
        if (py_equals(subscript(ticket, "id"), ticket_id)) {
            // (C++ only: Python lets an agent close or hand over a request for a person's approval — SPEC 295)
            if (!person(c) && is_text(dget(ticket, "kind"), "gate")) {
                throw OpError("only a person can change a request for approval (ticket " + py_str(ticket_id) + ")");
            }
            if (const Json* status = get(c.op, "status")) ticket["status"] = py_str(*status);
            if (const Json* assignee = get(c.op, "assignee")) ticket["assignee"] = py_str(*assignee);
            if (const Json* rate = get(c.op, "rate")) ticket["rate"] = py_str(*rate);
            return;
        }
    }
    throw OpError("no ticket " + py_str(ticket_id));
}

}  // namespace

// --- shared with the studio ------------------------------------------------------------------------------------------

std::string content_hash(const Json& value) {
    const std::string text = dump_canonical(value);
    const QByteArray digest =
        QCryptographicHash::hash(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())), QCryptographicHash::Sha256).toHex();
    return "sha256:" + std::string(digest.constData(), static_cast<std::size_t>(digest.size()));
}

std::string brief_hash(const Json& panel) {
    Json brief = Json::object();
    for (const std::string_view key : kBriefKeys) brief[std::string(key)] = dget(panel, key);
    if (brief["characters"].is_array()) {
        Json characters = Json::array();
        for (const Json& character : brief["characters"]) {  // (a character's 名札 is lettering, not part of the picture)
            if (!character.is_object()) {
                characters.push_back(character);
                continue;
            }
            Json kept = Json::object();
            for (const auto& [k, v] : character.items()) {
                if (k != "tag") kept[k] = v;
            }
            characters.push_back(std::move(kept));
        }
        brief["characters"] = std::move(characters);
    }
    Json regions = Json::array();
    for (const Json& region : iterate(py_or(dget(panel, "regions"), Json::array()))) {
        if (is_text(dget(region, "source"), "user")) regions.push_back(region);
    }
    brief["regions"] = regions.empty() ? Json() : std::move(regions);
    return content_hash(brief);
}

void register_studio_ops(OpRegistry& registry, StudioHooks hooks) {
    registry.add("set_studio", set_studio);
    registry.add("set_style_catalog", set_style_catalog);
    registry.add("upsert_character", upsert_character);
    registry.add("delete_character", delete_character);
    registry.add("upsert_location", [](OpContext& c) { upsert_place(c, "locations", "location", "location"); });
    registry.add("delete_location", [](OpContext& c) { delete_place(c, "locations"); });
    registry.add("upsert_prop", [](OpContext& c) { upsert_place(c, "props", "prop", "prop"); });
    registry.add("delete_prop", [](OpContext& c) { delete_place(c, "props"); });
    registry.add("set_script", set_script);
    registry.add("set_page_plan", set_page_plan);
    registry.add("set_panel", set_panel);
    registry.add("record_review", record_review);
    registry.add("approve", approve);
    registry.add("allow_chat_approval", allow_chat_approval);
    registry.add("revoke", revoke);
    registry.add("request_approval", request_approval);
    registry.add("request_fix", request_fix);
    for (const char* name : {"add_region", "edit_region", "delete_region", "replace_regions"}) {
        registry.add(name, [name](OpContext& c) { regions_op(c, name); });
    }
    registry.add("bind_ref", [](OpContext& c) { refs_op(c, true); });
    registry.add("unbind_ref", [](OpContext& c) { refs_op(c, false); });
    registry.add("register_assets", register_assets);
    registry.add("attach_reference", attach_reference);
    registry.add("open_request", open_request);
    registry.add("close_request", close_request);
    registry.add("import_candidates", import_candidates);
    registry.add("review_candidates", [](OpContext& c) { candidates_op(c, true); });
    registry.add("set_candidate", [](OpContext& c) { candidates_op(c, false); });
    registry.add("adopt_candidate", [hooks](OpContext& c) { adopt_candidate(c, hooks); });
    registry.add("unadopt", unadopt);
    registry.add("set_placement", [hooks](OpContext& c) { set_placement(c, hooks); });
    registry.add("place_asset", [hooks](OpContext& c) { place_asset(c, hooks); });
    registry.add("set_finish", set_finish);
    registry.add("set_layout", set_layout);
    registry.add("propose", propose);
    registry.add("resolve_proposal", resolve_proposal);
    registry.add("resolve_ticket", [](OpContext& c) { tickets_op(c, false); });
    registry.add("reopen_ticket", [](OpContext& c) { tickets_op(c, true); });
    registry.add("withdraw_candidates", withdraw_candidates);
    registry.add("reject_sheet", reject_sheet);
    registry.add("ask_human", ask_human);
    registry.add("set_bible", set_bible);
    registry.add("add_ticket", add_ticket);
    registry.add("set_ticket", set_ticket);
}

}  // namespace genko::core
