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
// "not_yet_ported" (an op of the public list, or a part of one, that this build has not ported yet: "ops[3] fill: fill
// is not in the C++ build yet"), "python_error" (an exception Python's apply_ops lets through, so Python stops with a
// traceback there: "ops[0] add_frame: IndexError: list index out of range").
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
// applying it. An op may leave a report for the reply's "results" (Python's op["_report"]).
struct OpContext {
    Document& doc;
    const Json& op;
    const Actor& actor;
    std::optional<Json> report = std::nullopt;
};

using OpFunction = std::function<void(OpContext&)>;

// Python's selops.resolve: an op's "area" of a kind beyond {poly} and {mask} (rect, ellipse, layer, color, all, saved,
// union, intersect, subtract, invert, grow_mm, feather_mm) as the {poly} or {mask} the drawing ops take, on the page
// at `page` of the book. Some kinds need the page or a layer drawn, so the resolver is render's (render::ops_registry).
// Throws OpError with selops.AreaError's words ("the area is empty", "no saved area x", …); Python's other exceptions
// as the ops raise them.
using AreaResolver = std::function<Json(const Document& doc, std::size_t page, const Json& area)>;

// The ops by name. builtin() holds the ops of core, each with Python's arguments, behaviour and messages; the ops that
// draw are render's (render::ops_registry() holds both); the other ops of the public list (`genko schema`) come with
// later milestones.
class OpRegistry {
public:
    static const OpRegistry& builtin();

    void add(std::string name, OpFunction function);
    const OpFunction* find(std::string_view name) const;
    std::vector<std::string> names() const;

    // Without a resolver the bus refuses an area that needs one with not_yet_ported.
    void set_area_resolver(AreaResolver resolver);
    const AreaResolver& area_resolver() const { return area_resolver_; }

private:
    std::map<std::string, OpFunction, std::less<>> ops_;
    AreaResolver area_resolver_;
};

struct ApplyResult {
    Document doc;                     // the book after the ops (the input book for [{"op": "undo"}] with dry_run)
    Json applied = Json::array();     // the op names, in order (for_pages: the ops it stands for)
    Json warnings = Json::array();    // Python's validate_episode and unknown-key warnings
    bool has_warnings = true;         // false for [{"op": "undo"}] (Python's reply has no "warnings" then)
    Json results = Json::array();     // what some ops report: {"index", "op", …} (the reply's "results" when any)
    Json journal_ops = Json::array(); // the ops as the journal records them (Python's _journal_op)
};

class CommandBus {
public:
    explicit CommandBus(const OpRegistry& registry = OpRegistry::builtin());

    // Apply `ops` (a JSON array of op objects) as `actor`. Throws ApplyError and leaves `doc` as it was when any op
    // fails. With dry_run the result is the same, and the caller does not save it.
    //
    // As Python's apply_ops: for_pages is expanded first (against the book as it was given); then each op is checked
    // (its area resolved by the registry's AreaResolver, the page locks and approvals, strict_gates) and applied. An
    // op of the public list that this build does not have is refused with not_yet_ported, never skipped; so is an
    // area that needs resolving when the registry has no resolver.
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
// lock_page and unlock_page take effect here, on a page the book has (OpError "no page N" for a number no page has,
// where Python does nothing and says nothing); set_paper on every page asks each page's lock (Python asks none).
void check_page_lock(Document& doc, const Json& op, const Actor& actor);

// The strict_gates checks made before each op of a studio book (Python's _check_strict, every rule, by the op's
// name: the layout of an approved name, finishing without the art approved, printed layers before the name is
// approved, …), the layer an op draws on found as the op finds it (Python reads "layer_id" for every op). Throws
// OpError.
void check_strict(const Document& doc, const Json& op, const Actor& actor);

// for_pages as the ops it stands for (Python's bookops.expand): each op once per page, its "page" set to the page.
// Throws OpError (Python's ApplyError) and, for values Python fails on outside its checks, PyValueError/PyTypeError.
// "pages" is "all", "body" or a list of ints: what else Python turns into page numbers (a text of digits, floats, a
// dict's keys) is refused with OpError "pages must be all, body or a list of page numbers".
Json expand_for_pages(const Document& doc, const Json& op);

// selops.needs_resolving: an area of a kind beyond {poly} and {mask} (rect, ellipse, layer, color, all, saved,
// union, intersect, subtract, invert, grow_mm, feather_mm).
bool area_needs_resolving(const Json& area);

// --- helpers shared by the op implementations -------------------------------------------------------------------

// Python's _require_page: the position of the first page whose index is int(op["page"]). Throws OpError("page (int)
// is required") or OpError("no page N").
std::size_t require_page(const Document& doc, const Json& op);

// Whether set_paper changes every page: its "page" is none, null, "" or 0 (Python's op.get("page") in (None, "", 0):
// False and 0.0 are 0 too). check_page_lock then asks every page's lock.
bool paper_on_every_page(const Json& op);

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
// add_page, delete_page). The ops of M2 are registered by core/ops_util.hpp's register_*_ops.
void register_book_ops(OpRegistry& registry);

}  // namespace genko::core
