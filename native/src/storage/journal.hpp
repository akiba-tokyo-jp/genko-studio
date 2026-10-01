#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// studio/journal.jsonl and studio/audit.jsonl (schema-v4 §4, §5): reading and writing their lines, the
// transactions of the journal, the Undo and Redo stacks replayed from it (with the converted v3 history of
// legacy/map.json first), and the repair that brings a book back to its last consistent point after a crash or a
// failed write.

namespace genko::storage::journal {

std::filesystem::path journal_file(const std::filesystem::path& dir);        // studio/journal.jsonl
std::filesystem::path audit_file(const std::filesystem::path& dir);          // studio/audit.jsonl
std::filesystem::path partial_file(const std::filesystem::path& dir);        // studio/journal.partial
std::filesystem::path audit_partial_file(const std::filesystem::path& dir);  // studio/audit.partial
std::filesystem::path map_file(const std::filesystem::path& dir);            // legacy/map.json

// A JSON-lines file as it is on disk.
struct Lines {
    bool exists = false;
    std::vector<core::Json> values;         // the complete lines that are JSON objects, in order
    std::vector<std::size_t> unreadable;    // the numbers (from 1) of complete lines that are not
    std::string tail;                       // the bytes after the last "\n": a line cut short (or "")
    std::uintmax_t complete_size = 0;       // the bytes up to and including the last "\n"
};
Lines read_lines(const std::filesystem::path& file);

// A line: compact JSON ("," and ":", no spaces, keys in their order, UTF-8) and "\n".
std::string line_text(const core::Json& value);
// Append a line and fsync (the file and its folder are made when missing).
void append(const std::filesystem::path& file, const core::Json& value);

bool is_v4(const core::Json& line);

// One transaction: its prepare line and what became of it.
struct Transaction {
    core::Json prepare;
    std::string txn;
    std::string action;
    std::int64_t rev = 0;
    enum class Status { pending, committed, aborted } status = Status::pending;
    bool recovered = false;  // committed by a repair
};

// The transactions in the order of their prepare lines. A commit or abort line settles the latest pending prepare
// of its txn.
std::vector<Transaction> transactions(const Lines& journal);
// The latest committed transaction with this txn.
const Transaction* find_committed(const std::vector<Transaction>& transactions, std::string_view txn);
// The highest "rev" of the journal's v4 lines (0 when there is none).
std::int64_t max_revision(const Lines& journal);

// The audit line of a prepared transaction that changes approvals ({"v":4,"txn","rev","actor","at","changes",
// "via"?}); null when it changes none.
core::Json audit_line(const core::Json& prepare);

// {"v":4,"kind":"commit","txn","rev"} (and "recovered": true when a repair commits it); {"v":4,"kind":"abort","txn"}.
core::Json commit_line(const std::string& txn, std::int64_t rev, bool recovered = false);
core::Json abort_line(const std::string& txn);

// --- Undo and Redo (§4.4) --------------------------------------------------------------------------------------

struct HistoryItem {
    std::string id;     // the txn; "legacy:<n>" for entry n of legacy/map.json
    bool legacy = false;
    core::Json rev;     // the revision the change made (a v3 rev for the legacy entries)
    core::Json actor;   // who made it
    std::optional<std::string> before;  // the state refs; nothing: no book (or, with *_old, no state could be made)
    std::optional<std::string> after;
    std::string before_old;  // legacy: the v3 snapshot refs
    std::string after_old;
};

struct Stacks {
    std::vector<HistoryItem> undo;
    std::vector<HistoryItem> redo;
    std::vector<std::string> problems;  // what did not add up while replaying
};

// Replay the committed transactions (edit: onto the Undo stack, Redo emptied; undo: Undo's last onto Redo; redo:
// back; recover and migrate: no change), after the entries of legacy/map.json (replayed as a v3 journal is).
Stacks replay(const Lines& journal, const core::Json* legacy_map);
Stacks stacks(const std::filesystem::path& dir);

// --- Repair (§4.3) -------------------------------------------------------------------------------------------

struct Repair {
    std::string journal_tail;            // a cut line kept in studio/journal.partial, the journal cut before it
    std::string audit_tail;              // the same for studio/audit.jsonl (kept in studio/audit.partial)
    std::vector<std::string> committed;  // pending transactions whose project.json was in place: committed
    std::vector<std::string> aborted;    // pending transactions whose project.json was not: aborted
    std::vector<std::string> audited;    // committed transactions whose audit line was added

    bool needed() const;
    core::Json to_json() const;
};

// What repair() would do; writes nothing.
Repair plan_repair(const std::filesystem::path& dir);
// Bring the journal and the audit to the last consistent point. Call it holding the project lock: at the start of
// every write and right after a write fails. Throws core::Error("io") when it cannot write (then nothing may be
// written on top of the book until a repair succeeds).
Repair repair(const std::filesystem::path& dir);

// --- Audit (§5) ----------------------------------------------------------------------------------------------

// The approval records: v3 lines as they are and v4 lines, each txn once, in the order of the file.
std::vector<core::Json> audit_entries(const std::filesystem::path& dir);

// Seconds since the epoch, as Python's time.time().
double now_seconds();

}  // namespace genko::storage::journal
