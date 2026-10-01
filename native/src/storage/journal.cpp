#include "storage/journal.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <system_error>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "storage/fault.hpp"
#include "storage/fsutil.hpp"

namespace genko::storage::journal {

namespace fs = std::filesystem;
using core::Json;

namespace {

const Json* get(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

std::string string_at(const Json& object, const char* key) {
    const Json* v = get(object, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string();
}

std::optional<std::string> ref_at(const Json& object, const char* key) {
    const Json* v = get(object, key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return v->get<std::string>();
}

bool blank(std::string_view line) {
    for (const char c : line) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f') return false;
    }
    return true;
}

// Keep a cut line: appended (with a "\n") to the partial file, unless it is already its last line (a repair
// stopped between keeping it and cutting it off).
void keep_fragment(const fs::path& partial, const std::string& fragment) {
    std::error_code ec;
    if (fs::exists(partial, ec)) {
        const std::string kept = read_file(partial);
        const std::string line = fragment + "\n";
        if (kept.size() >= line.size() && kept.compare(kept.size() - line.size(), line.size(), line) == 0) return;
    }
    append_durable(partial, fragment + "\n");
}

HistoryItem item_of(const Transaction& t) {
    HistoryItem item;
    item.id = t.txn;
    item.rev = t.rev;
    item.actor = get(t.prepare, "actor") != nullptr ? *get(t.prepare, "actor") : Json(nullptr);
    item.before = ref_at(t.prepare, "before");
    item.after = ref_at(t.prepare, "after");
    return item;
}

HistoryItem legacy_item(const Json& entry, std::size_t n) {
    HistoryItem item;
    item.id = "legacy:" + std::to_string(n);
    item.legacy = true;
    item.rev = get(entry, "old_rev") != nullptr ? *get(entry, "old_rev") : Json(nullptr);
    item.actor = get(entry, "actor") != nullptr ? *get(entry, "actor") : Json(nullptr);
    item.before = ref_at(entry, "before");
    item.after = ref_at(entry, "after");
    item.before_old = string_at(entry, "before_old");
    item.after_old = string_at(entry, "after_old");
    return item;
}

Repair run_repair(const fs::path& dir, bool write) {
    Repair out;
    Lines journal = read_lines(journal_file(dir));
    if (!journal.tail.empty()) {
        out.journal_tail = journal.tail;
        if (write) {
            fault::StageScope stage(fault::Stage::commit);
            keep_fragment(partial_file(dir), journal.tail);
            truncate_durable(journal_file(dir), journal.complete_size);
        }
        journal.tail.clear();
    }
    const Lines audit = read_lines(audit_file(dir));
    if (!audit.tail.empty()) {
        out.audit_tail = audit.tail;
        if (write) {
            fault::StageScope stage(fault::Stage::audit);
            keep_fragment(audit_partial_file(dir), audit.tail);
            truncate_durable(audit_file(dir), audit.complete_size);
        }
    }
    std::set<std::string> audited;
    for (const Json& line : audit.values) {
        if (is_v4(line) && get(line, "txn") != nullptr && get(line, "txn")->is_string()) {
            audited.insert(get(line, "txn")->get<std::string>());
        }
    }
    const auto add_audit = [&](const Transaction& t) {
        const Json line = audit_line(t.prepare);
        if (line.is_null() || audited.contains(t.txn)) return;
        out.audited.push_back(t.txn);
        audited.insert(t.txn);
        if (write) {
            fault::StageScope stage(fault::Stage::audit);
            append(audit_file(dir), line);
        }
    };

    const std::vector<Transaction> txns = transactions(journal);
    const Transaction* last_pending = nullptr;
    for (const Transaction& t : txns) {
        if (t.status == Transaction::Status::pending) last_pending = &t;
    }
    std::string project_sha;
    if (last_pending != nullptr) {
        std::error_code ec;
        const fs::path project = dir / "project.json";
        if (fs::is_regular_file(project, ec)) project_sha = sha256_file(project);
    }
    for (const Transaction& t : txns) {
        if (t.status == Transaction::Status::committed) {
            add_audit(t);  // (a committed change's approvals are in the audit, once)
            continue;
        }
        if (t.status != Transaction::Status::pending) continue;
        const bool replaced = &t == last_pending && !project_sha.empty() &&
                              project_sha == string_at(t.prepare, "project_sha256");
        if (replaced) {
            add_audit(t);
            out.committed.push_back(t.txn);
            if (write) {
                fault::StageScope stage(fault::Stage::commit);
                append(journal_file(dir), commit_line(t.txn, t.rev, true));
            }
        } else {
            out.aborted.push_back(t.txn);
            if (write) {
                fault::StageScope stage(fault::Stage::commit);
                append(journal_file(dir), abort_line(t.txn));
            }
        }
    }
    return out;
}

}  // namespace

fs::path journal_file(const fs::path& dir) { return dir / "studio" / "journal.jsonl"; }
fs::path audit_file(const fs::path& dir) { return dir / "studio" / "audit.jsonl"; }
fs::path partial_file(const fs::path& dir) { return dir / "studio" / "journal.partial"; }
fs::path audit_partial_file(const fs::path& dir) { return dir / "studio" / "audit.partial"; }
fs::path map_file(const fs::path& dir) { return dir / "legacy" / "map.json"; }

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

Lines read_lines(const fs::path& file) {
    Lines out;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return out;
    out.exists = true;
    const std::string data = read_file(file);
    const auto last = data.rfind('\n');
    const std::size_t complete = last == std::string::npos ? 0 : last + 1;
    out.complete_size = complete;
    out.tail = data.substr(complete);
    std::size_t start = 0;
    std::size_t number = 0;
    while (start < complete) {
        const auto end = data.find('\n', start);
        const std::string_view line(data.data() + start, end - start);
        start = end + 1;
        ++number;
        if (blank(line)) continue;  // (Python skips blank lines)
        try {
            Json value = core::parse_python_json(line);
            if (value.is_object()) {
                out.values.push_back(std::move(value));
                continue;
            }
        } catch (const core::Error&) {
        }
        out.unreadable.push_back(number);
    }
    return out;
}

std::string line_text(const Json& value) {
    core::DumpOptions options;
    options.item_separator = ",";
    options.key_separator = ":";
    return core::dump(value, options) + "\n";
}

void append(const fs::path& file, const Json& value) { append_durable(file, line_text(value)); }

bool is_v4(const Json& line) {
    const Json* v = get(line, "v");
    return v != nullptr && v->is_number_integer() && v->get<std::int64_t>() == 4;
}

std::vector<Transaction> transactions(const Lines& journal) {
    std::vector<Transaction> out;
    for (const Json& line : journal.values) {
        if (!is_v4(line)) continue;
        const std::string kind = string_at(line, "kind");
        const std::string txn = string_at(line, "txn");
        if (txn.empty()) continue;
        if (kind == "prepare") {
            Transaction t;
            t.prepare = line;
            t.txn = txn;
            t.action = string_at(line, "action");
            const Json* rev = get(line, "rev");
            t.rev = rev != nullptr && rev->is_number_integer() ? rev->get<std::int64_t>() : 0;
            out.push_back(std::move(t));
        } else if (kind == "commit" || kind == "abort") {
            for (auto it = out.rbegin(); it != out.rend(); ++it) {
                if (it->txn != txn || it->status != Transaction::Status::pending) continue;
                if (kind == "commit") {
                    it->status = Transaction::Status::committed;
                    const Json* recovered = get(line, "recovered");
                    it->recovered = recovered != nullptr && core::py_truthy(*recovered);
                } else {
                    it->status = Transaction::Status::aborted;
                }
                break;
            }
        }
    }
    return out;
}

const Transaction* find_committed(const std::vector<Transaction>& txns, std::string_view txn) {
    for (auto it = txns.rbegin(); it != txns.rend(); ++it) {
        if (it->txn == txn && it->status == Transaction::Status::committed) return &*it;
    }
    return nullptr;
}

std::int64_t max_revision(const Lines& journal) {
    std::int64_t out = 0;
    for (const Json& line : journal.values) {
        if (!is_v4(line)) continue;
        const Json* rev = get(line, "rev");
        if (rev != nullptr && rev->is_number_integer()) out = std::max(out, rev->get<std::int64_t>());
    }
    return out;
}

Json commit_line(const std::string& txn, std::int64_t rev, bool recovered) {
    Json line = Json::object();
    line["v"] = 4;
    line["kind"] = "commit";
    line["txn"] = txn;
    line["rev"] = rev;
    if (recovered) line["recovered"] = true;
    return line;
}

Json abort_line(const std::string& txn) {
    Json line = Json::object();
    line["v"] = 4;
    line["kind"] = "abort";
    line["txn"] = txn;
    return line;
}

Json audit_line(const Json& prepare) {
    const Json* changes = get(prepare, "audit");
    if (changes == nullptr || !changes->is_array() || changes->empty()) return Json(nullptr);
    Json line = Json::object();
    line["v"] = 4;
    line["txn"] = string_at(prepare, "txn");
    line["rev"] = get(prepare, "rev") != nullptr ? *get(prepare, "rev") : Json(nullptr);
    line["actor"] = get(prepare, "actor") != nullptr ? *get(prepare, "actor") : Json(nullptr);
    line["at"] = get(prepare, "at") != nullptr ? *get(prepare, "at") : Json(nullptr);
    line["changes"] = *changes;
    const std::string action = string_at(prepare, "action");
    if (action == "undo" || action == "redo") line["via"] = action;
    return line;
}

Stacks replay(const Lines& journal, const Json* legacy_map) {
    Stacks s;
    if (legacy_map != nullptr) {
        const Json* entries = get(*legacy_map, "entries");
        if (entries != nullptr && entries->is_array()) {
            for (std::size_t n = 0; n < entries->size(); ++n) {
                const Json& entry = (*entries)[n];
                const std::string kind = get(entry, "kind") != nullptr ? string_at(entry, "kind") : "commit";
                if (kind == "commit") {
                    s.undo.push_back(legacy_item(entry, n));
                    s.redo.clear();
                } else if (kind == "undo" && !s.undo.empty()) {
                    s.redo.push_back(std::move(s.undo.back()));
                    s.undo.pop_back();
                } else if (kind == "redo" && !s.redo.empty()) {
                    s.undo.push_back(std::move(s.redo.back()));
                    s.redo.pop_back();
                }
            }
        }
    }
    for (const Transaction& t : transactions(journal)) {
        if (t.status != Transaction::Status::committed) continue;
        const std::string target = string_at(t.prepare, "target");
        if (t.action == "edit") {
            s.undo.push_back(item_of(t));
            s.redo.clear();
        } else if (t.action == "undo" || t.action == "redo") {
            auto& from = t.action == "undo" ? s.undo : s.redo;
            auto& to = t.action == "undo" ? s.redo : s.undo;
            if (from.empty()) {
                s.problems.push_back("transaction " + t.txn + " (" + t.action + ") has nothing to " + t.action);
                continue;
            }
            if (!target.empty() && from.back().id != target) {
                s.problems.push_back("transaction " + t.txn + " (" + t.action + ") names " + target + " but the stack has " +
                                     from.back().id);
            }
            to.push_back(std::move(from.back()));
            from.pop_back();
        } else if (t.action != "recover" && t.action != "migrate") {
            s.problems.push_back("transaction " + t.txn + " has an unknown action " + core::py_repr_str(t.action));
        }
    }
    return s;
}

Stacks stacks(const fs::path& dir) {
    const Lines journal = read_lines(journal_file(dir));
    std::error_code ec;
    if (!fs::is_regular_file(map_file(dir), ec)) return replay(journal, nullptr);
    Json map;
    try {
        map = core::parse_python_json(read_file(map_file(dir)));
    } catch (const core::Error& error) {
        Stacks s = replay(journal, nullptr);
        s.problems.push_back("legacy/map.json cannot be read: " + std::string(error.what()));
        return s;
    }
    return replay(journal, &map);
}

bool Repair::needed() const {
    return !journal_tail.empty() || !audit_tail.empty() || !committed.empty() || !aborted.empty() || !audited.empty();
}

Json Repair::to_json() const {
    Json out = Json::object();
    const auto list = [](const std::vector<std::string>& items) {
        Json array = Json::array();
        for (const auto& item : items) array.push_back(item);
        return array;
    };
    out["journal_tail_bytes"] = static_cast<std::int64_t>(journal_tail.size());
    out["audit_tail_bytes"] = static_cast<std::int64_t>(audit_tail.size());
    out["committed"] = list(committed);
    out["aborted"] = list(aborted);
    out["audited"] = list(audited);
    return out;
}

Repair plan_repair(const fs::path& dir) { return run_repair(dir, false); }

Repair repair(const fs::path& dir) { return run_repair(dir, true); }

std::vector<Json> audit_entries(const fs::path& dir) {
    std::vector<Json> out;
    std::set<std::string> seen;
    for (Json& line : read_lines(audit_file(dir)).values) {
        if (is_v4(line)) {
            const std::string txn = string_at(line, "txn");
            if (!txn.empty() && !seen.insert(txn).second) continue;
        }
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace genko::storage::journal
