#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/actor.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// Every change to a book goes through the CommandBus (docs/cpp-migration/ARCHITECTURE.md §5): Python's
// ops.apply_ops — the ops are checked and applied one by one to a working copy of the book (pages are shared until
// an op changes them), and only when every op succeeds is the copy the new book. A failure names the op and its
// place, as Python's ApplyError does: "ops[2] set_note: no page 9 ‖ set_note takes {page: int}".

namespace genko::core {

// A batch (or one of its ops) that cannot be applied. Codes: "apply" (Python's ApplyError), "nothing_to_undo",
// "not_implemented" (an op of the public list that this build has not ported yet).
class ApplyError : public Error {
public:
    explicit ApplyError(const std::string& message, std::string code = "apply");
};

// Raised inside an op: becomes "ops[i] <op>: <message>" plus the op's usage (Python's ApplyError in _apply_one).
class OpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Raised inside an op for a key or id that is not there (Python's KeyError): "ops[i] <op>: not found: <repr> (a key
// the op needs, or an id the book does not have)". A core::Error raised inside an op is Python's ValueError or
// TypeError: "ops[i] <op>: a value of the wrong type (<message>)".
class OpKeyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What an op works on: the working copy of the book (change pages only through doc.edit_page), the op and who is
// applying it.
struct OpContext {
    Document& doc;
    const Json& op;
    const Actor& actor;
};

using OpFunction = std::function<void(OpContext&)>;

// The ops by name. builtin() holds the ops this build implements, each with Python's arguments, behaviour and
// messages; the other ops of the public list (`genko schema`) come with later milestones.
class OpRegistry {
public:
    static const OpRegistry& builtin();

    void add(std::string name, OpFunction function);
    const OpFunction* find(std::string_view name) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, OpFunction, std::less<>> ops_;
};

struct ApplyResult {
    Document doc;                     // the book after the ops (the input book for [{"op": "undo"}] with dry_run)
    Json applied = Json::array();     // the op names, in order
    Json warnings = Json::array();    // Python's validate_episode and unknown-key warnings
    bool has_warnings = true;         // false for [{"op": "undo"}] (Python's reply has no "warnings" then)
    Json journal_ops = Json::array(); // the ops as the journal records them (Python's _journal_op)
};

class CommandBus {
public:
    explicit CommandBus(const OpRegistry& registry = OpRegistry::builtin());

    // Apply `ops` (a JSON array of op objects) as `actor`. Throws ApplyError and leaves `doc` as it was when any op
    // fails. With dry_run the result is the same, and the caller does not save it.
    ApplyResult apply(const Document& doc, const Json& ops, const Actor& actor, bool dry_run = false) const;

private:
    const OpRegistry& registry_;
};

// Python's validate_episode: reference checks after a batch, returned as warnings (older files may already break
// them).
std::vector<std::string> validate_document(const Document& doc);

// The op as the journal keeps it: an inline png_base64 picture replaced by "<N base64 chars>" (Python's _journal_op).
Json journal_op(const Json& op);

// The page lock and approval checks made before each op (Python's _check_page_lock): name_ok needs a person;
// lock_page and unlock_page take effect here.
void check_page_lock(Document& doc, const Json& op, const Actor& actor);

// --- helpers shared by the op implementations -------------------------------------------------------------------

// Python's _require_page: the position of the first page whose index is int(op["page"]). Throws OpError("page (int)
// is required") or OpError("no page N").
std::size_t require_page(const Document& doc, const Json& op);

// A page number → its new number (Python's dict in remap_page_refs; nothing for a page that went away).
class PageMapping {
public:
    void set(const Num& from, std::optional<std::int64_t> to);
    // The new number (nothing when there is none, or the page went away).
    std::optional<std::int64_t> get(const Num& from) const;
    std::optional<std::int64_t> get(const Json& from) const;

private:
    std::vector<std::pair<Num, std::optional<std::int64_t>>> items_;
};

// Python's remap_page_refs: renumber pages after a delete or reorder and fix every reference by page number.
void remap_page_refs(Document& doc, const PageMapping& mapping);

// Python's _reorder: the pages in this order of their current numbers, renumbered 1, 2, ….
void reorder_pages(Document& doc, const std::vector<Num>& order);

// Register the book and page ops of M1 (set_note, set_meta, name_ok, lock_page, unlock_page, set_autosave,
// add_page, delete_page).
void register_book_ops(OpRegistry& registry);

}  // namespace genko::core
