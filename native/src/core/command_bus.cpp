#include "core/command_bus.hpp"

#include <set>
#include <utility>

#include "core/ops_schema.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

constexpr std::string_view kSeparator = " ‖ ";  // " ‖ " (Python's _usage)

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

bool is_op(const Json& op, std::string_view name) {
    const Json* value = find(op, "op");
    return value != nullptr && value->is_string() && value->get_ref<const std::string&>() == name;
}

// The op's entry in the public list (`genko schema`), when it has one.
const Json* schema_of(const Json& op) {
    const Json* name = find(op, "op");
    if (name == nullptr || !name->is_string()) return nullptr;
    for (const Json& entry : ops_schema()) {
        const Json* op_name = find(entry, "op");
        if (op_name != nullptr && *op_name == *name) return &entry;
    }
    return nullptr;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += separator;
        out += items[i];
    }
    return out;
}

// Python's _usage: the keys given that the op does not take, and how the op is written.
std::string usage(const Json& op) {
    const Json* schema = schema_of(op);
    if (schema == nullptr) return {};
    std::vector<std::string> strange;
    for (const auto& [key, value] : op.items()) {
        if (!schema->contains(key) && key != "op" && key != "area" && !key.starts_with("_")) strange.push_back(key);
    }
    std::vector<std::string> keys;
    for (const auto& [key, value] : schema->items()) {
        if (key != "op" && key != "note") keys.push_back(key + ": " + py_str(value));
    }
    std::string out;
    if (!strange.empty()) out += std::string(kSeparator) + "unknown keys: " + join(strange, ", ");
    out += std::string(kSeparator) + py_str(*find(op, "op")) + " takes {" + join(keys, ", ") + "}";
    return out;
}

// Python's _unknown_keys: keys an op was given that it does not take (ignored, so said).
std::vector<std::string> unknown_key_warnings(const Json& ops) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Json& op = ops[i];
        if (!op.is_object()) continue;
        const Json* schema = schema_of(op);
        if (schema == nullptr) continue;
        std::vector<std::string> strange;
        for (const auto& [key, value] : op.items()) {
            const bool known = schema->contains(key) || key == "op" || key == "area" || key == "page" || key == "id" ||
                               key == "note";
            if (!known && !key.starts_with("_")) strange.push_back(key);
        }
        if (!strange.empty()) {
            out.push_back("ops[" + std::to_string(i) + "] " + py_str(*find(op, "op")) +
                          ": unknown keys ignored: " + join(strange, ", "));
        }
    }
    return out;
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

const Page* first_page(const Document& doc, const Num& index) {
    for (const auto& page : doc.pages) {
        if (page->index == index) return page.get();
    }
    return nullptr;
}

// Python's facing_problem: why two pages cannot form a spread (nothing when they face each other).
std::optional<std::string> facing_problem(const Document& doc, const Page& a, const Page& b) {
    if (!(py_abs(a.index - b.index) == Num(1))) {
        return "pages " + a.index.repr() + " and " + b.index.repr() + " are not next to each other";
    }
    const bool b_first = b.index < a.index;  // (sorted by index, a first on a tie)
    const Page& first = b_first ? b : a;
    const Page& second = b_first ? a : b;
    const std::string start = doc.binding == Binding::Right ? "right" : "left";
    const std::string start_side = doc.start_side.value_or("");
    if (first.side(start_side) != start || second.side(start_side) == start) {
        return "pages " + first.index.repr() + " and " + second.index.repr() + " are two sides of one leaf, not a spread";
    }
    return std::nullopt;
}

// Python's _op_page_index: the page an op is about (its "page", or the page of the line a line op names).
std::optional<Num> op_page_index(const Document& doc, const Json& op) {
    if (const Json* page = find(op, "page")) {
        try {
            return Num(py_int(*page));
        } catch (const Error&) {
            return std::nullopt;
        }
    }
    static const std::set<std::string, std::less<>> kLineOps{"edit_line", "move_line", "delete_line",
                                                              "set_balloon_path", "cut_balloon"};
    const Json* name = find(op, "op");
    const Json* id = find(op, "id");
    if (name != nullptr && name->is_string() && kLineOps.contains(name->get_ref<const std::string&>()) &&
        id != nullptr && py_truthy(*id)) {
        for (const StoryLine& line : doc.story) {
            if (id->is_string() && line.id == id->get_ref<const std::string&>()) return line.page_index;
        }
    }
    return std::nullopt;
}

bool json_equals_string(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

}  // namespace

ApplyError::ApplyError(const std::string& message, std::string code) : Error(std::move(code), message) {}

// --- OpRegistry --------------------------------------------------------------------------------------------------

const OpRegistry& OpRegistry::builtin() {
    static const OpRegistry registry = [] {
        OpRegistry r;
        register_book_ops(r);
        return r;
    }();
    return registry;
}

void OpRegistry::add(std::string name, OpFunction function) { ops_[std::move(name)] = std::move(function); }

const OpFunction* OpRegistry::find(std::string_view name) const {
    const auto it = ops_.find(name);
    return it == ops_.end() ? nullptr : &it->second;
}

std::vector<std::string> OpRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [name, function] : ops_) out.push_back(name);
    return out;
}

// --- checks --------------------------------------------------------------------------------------------------------

std::size_t require_page(const Document& doc, const Json& op) {
    std::int64_t index = 0;
    const Json* page = find(op, "page");
    try {
        if (page == nullptr) throw OpError("page (int) is required");
        index = py_int(*page);
    } catch (const Error&) {
        throw OpError("page (int) is required");
    }
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index == Num(index)) return i;
    }
    throw OpError("no page " + std::to_string(index));
}

void check_page_lock(Document& doc, const Json& op, const Actor& actor) {
    const Json* name_value = find(op, "op");
    const std::string name = name_value != nullptr && name_value->is_string() ? name_value->get<std::string>() : "";
    const bool named = name_value != nullptr && name_value->is_string();
    if (named && name == "name_ok" && !actor.can_approve()) {  // (GATE_OPS)
        throw OpError(name + " needs a person (actor " + actor.name() + " cannot approve)");
    }
    if (named && (name == "undo" || name == "set_meta" || name == "set_bible" || name == "set_autosave")) return;
    const auto index = op_page_index(doc, op);
    if (!index) return;
    const Page* page = first_page(doc, *index);
    if (page == nullptr) return;
    const std::string key = page->id;  // (v3: locks follow the page, not its position)
    const Json* owner = find(doc.page_locks, key);
    const bool owned = owner != nullptr && py_truthy(*owner);
    if (named && name == "lock_page") {
        const Json* agent = find(op, "agent");
        const std::string wanted = agent != nullptr && py_truthy(*agent) ? py_str(*agent) : actor.name();
        if (actor.name() != kLegacyActor && wanted != actor.name()) {
            throw OpError("cannot lock page " + index->repr() + " as " + wanted + " (actor is " + actor.name() + ")");
        }
        const bool owner_is_ai = owner != nullptr && owner->is_string() &&
                                 owner->get_ref<const std::string&>().starts_with("ai:");
        if (owned && !json_equals_string(*owner, wanted) && !(actor.can_approve() && owner_is_ai)) {
            throw OpError("page " + index->repr() + " locked by " + py_str(*owner));
        }
        doc.page_locks[key] = wanted;
        return;
    }
    if (named && name == "unlock_page") {
        if (owned && !json_equals_string(*owner, actor.name()) && !actor.can_approve()) {
            throw OpError("page " + index->repr() + " locked by " + py_str(*owner) + "; " + actor.name() +
                          " cannot unlock it");
        }
        doc.page_locks.erase(key);
        return;
    }
    if (owned && !json_equals_string(*owner, actor.name())) {
        throw OpError("page " + index->repr() + " locked by " + py_str(*owner));
    }
}

std::vector<std::string> validate_document(const Document& doc) {
    std::vector<std::string> warnings;
    std::set<std::string> ids;
    for (const auto& page : doc.pages) ids.insert(page->id);
    for (const StoryLine& line : doc.story) {
        const Page* page = first_page(doc, line.page_index);
        if (page == nullptr) {
            warnings.push_back("line " + line.id + ": page " + line.page_index.repr() + " does not exist");
            continue;
        }
        if (line.frame_id && !line.frame_id->empty() && page->find_frame(*line.frame_id) == nullptr) {
            warnings.push_back("line " + line.id + ": frame " + *line.frame_id + " is not on page " +
                               line.page_index.repr());
        }
    }
    for (const auto& page : doc.pages) {
        std::set<std::string> leaves;
        for (const Frame* frame : page->leaf_frames()) leaves.insert(frame->id);
        for (const Layer& layer : page->layers) {
            if (layer.kind == LayerKind::Placed && layer.frame_id && !layer.frame_id->empty() &&
                !leaves.contains(*layer.frame_id)) {
                warnings.push_back("page " + page->index.repr() + " layer " + layer.id + ": panel " + *layer.frame_id +
                                   " is gone (placed art will not clip)");
            }
        }
    }
    if (doc.page_locks.is_object()) {
        for (const auto& [key, owner] : doc.page_locks.items()) {
            if (!ids.contains(key)) warnings.push_back("page lock on unknown page " + key);
        }
    }
    for (const auto& page : doc.pages) {
        if (!page->spread_with || !(page->index < *page->spread_with)) continue;
        const Page* partner = first_page(doc, *page->spread_with);
        const std::optional<std::string> problem =
            partner != nullptr ? facing_problem(doc, *page, *partner)
                               : std::optional<std::string>("spread partner " + page->spread_with->repr() + " missing");
        if (problem) {
            warnings.push_back("spread " + page->index.repr() + "-" + page->spread_with->repr() + ": " + *problem);
        }
    }
    return warnings;
}

Json journal_op(const Json& op) {
    Json out = op;
    if (out.is_object()) {
        const auto it = out.find("png_base64");
        if (it != out.end() && it->is_string()) {
            *it = "<" + std::to_string(code_points(it->get_ref<const std::string&>())) + " base64 chars>";
        }
    }
    return out;
}

// --- page numbers ------------------------------------------------------------------------------------------------

void PageMapping::set(const Num& from, std::optional<std::int64_t> to) {
    for (auto& [key, value] : items_) {
        if (key == from) {  // (a dict: the key keeps its place and takes the new value)
            value = to;
            return;
        }
    }
    items_.emplace_back(from, to);
}

std::optional<std::int64_t> PageMapping::get(const Num& from) const {
    for (const auto& [key, value] : items_) {
        if (key == from) return value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> PageMapping::get(const Json& from) const {
    if (from.is_boolean()) return get(Num(from.get<bool>() ? 1 : 0));  // (Python: True == 1)
    if (const auto number = Num::from_json(from)) return get(*number);
    return std::nullopt;
}

void remap_page_refs(Document& doc, const PageMapping& mapping) {
    for (StoryLine& line : doc.story) {
        if (const auto to = mapping.get(line.page_index); to && *to != 0) line.page_index = Num(*to);
    }
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        const auto to = mapping.get(doc.pages[i]->index);
        if (to && *to != 0 && !doc.pages[i]->index.same(Num(*to))) doc.edit_page(i).index = Num(*to);
    }
    const auto remapped = [&](const std::optional<Num>& value) -> std::optional<Num> {
        if (!value) return std::nullopt;
        const auto to = mapping.get(*value);
        return to ? std::optional<Num>(Num(*to)) : std::nullopt;
    };
    const auto same = [](const std::optional<Num>& a, const std::optional<Num>& b) {
        return a.has_value() == b.has_value() && (!a || a->same(*b));
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        const Page& page = *doc.pages[i];
        const auto spread = remapped(page.spread_with);
        const auto onion = remapped(page.onion_from);
        if (!same(spread, page.spread_with)) doc.edit_page(i).spread_with = spread;
        if (!same(onion, doc.pages[i]->onion_from)) doc.edit_page(i).onion_from = onion;
    }
    if (doc.tickets.is_array()) {
        for (Json& ticket : doc.tickets) {
            if (!ticket.is_object()) continue;
            const auto at = ticket.find("page_index");
            if (at == ticket.end() || at->is_null()) continue;
            if (const auto to = mapping.get(*at)) {
                *at = *to;
            } else {
                ticket["status"] = "orphaned";
            }
        }
    }
}

void reorder_pages(Document& doc, const std::vector<Num>& order) {
    std::vector<PagePtr> pages;
    pages.reserve(order.size());
    for (const Num& index : order) {
        PagePtr found;
        for (const auto& page : doc.pages) {
            if (page->index == index) found = page;  // (by_index: the last page with the number)
        }
        if (!found) throw OpKeyError(index.repr());
        pages.push_back(found);
    }
    doc.pages = std::move(pages);
    PageMapping mapping;
    for (std::size_t n = 0; n < order.size(); ++n) mapping.set(order[n], static_cast<std::int64_t>(n + 1));
    remap_page_refs(doc, mapping);
}

// --- CommandBus ----------------------------------------------------------------------------------------------------

CommandBus::CommandBus(const OpRegistry& registry) : registry_(registry) {}

ApplyResult CommandBus::apply(const Document& doc, const Json& ops, const Actor& actor, bool dry_run) const {
    if (!ops.is_array()) throw ApplyError("ops must be a JSON array");
    ApplyResult result;
    if (ops.size() == 1 && ops[0].is_object() && is_op(ops[0], "undo")) {
        // Python undoes the session's own changes here; a book just read has none. (Saved changes are undone with
        // `genko undo`, from the journal.)
        if (!dry_run) throw ApplyError("nothing to undo", "nothing_to_undo");
        result.doc = doc;
        result.applied.push_back("undo");
        result.has_warnings = false;
        result.journal_ops = Json::array({Json::object({{"op", "undo"}})});
        return result;
    }

    Document work = doc;  // (the pages stay shared until an op changes one)
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Json& op = ops[i];
        if (!op.is_object()) throw ApplyError("ops[" + std::to_string(i) + "] must be an object");
        const Json* name = find(op, "op");
        const std::string name_text = name != nullptr ? py_str(*name) : std::string("None");
        const std::string prefix = "ops[" + std::to_string(i) + "] " + name_text + ": ";
        try {
            check_page_lock(work, op, actor);
            // (strict_gates checks the drawing and layout ops; they come with those ops in M2)
            const bool lock_op = is_op(op, "lock_page") || is_op(op, "unlock_page");
            if (!lock_op) {  // (lock_page and unlock_page took effect in check_page_lock)
                if (name == nullptr || !py_truthy(*name)) throw OpError("op is required");
                const OpFunction* function = name->is_string() ? registry_.find(name->get_ref<const std::string&>()) : nullptr;
                if (function == nullptr) {
                    if (schema_of(op) != nullptr) {
                        throw ApplyError(prefix + name_text + " is not implemented in this build yet" + usage(op),
                                         "not_implemented");
                    }
                    throw OpError("unknown op: " + name_text);
                }
                OpContext context{work, op, actor};
                (*function)(context);
            }
        } catch (const ApplyError&) {
            throw;
        } catch (const OpError& error) {
            throw ApplyError(prefix + error.what() + usage(op));
        } catch (const OpKeyError& error) {
            throw ApplyError(prefix + "not found: " + error.what() +
                             " (a key the op needs, or an id the book does not have)" + usage(op));
        } catch (const Error& error) {
            throw ApplyError(prefix + "a value of the wrong type (" + error.what() + ")" + usage(op));
        }
        result.applied.push_back(name_text);
    }
    for (auto& warning : validate_document(work)) result.warnings.push_back(std::move(warning));
    for (auto& warning : unknown_key_warnings(ops)) result.warnings.push_back(std::move(warning));
    for (const Json& op : ops) result.journal_ops.push_back(journal_op(op));
    result.doc = std::move(work);
    return result;
}

}  // namespace genko::core
