#include "storage/transaction.hpp"

#include <algorithm>
#include <system_error>

#include "core/gates.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "storage/asset_store.hpp"
#include "storage/fault.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/writer.hpp"

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;

namespace {

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::string string_at(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Python's _journal_ops: the ops inline, or (a batch too big for one journal line) a brief of each op inline and
// the whole list as an asset.
Json journal_ops(const Json& ops, AssetStore& store, std::string& ops_asset) {
    if (code_points(core::dump_python(ops)) <= kJournalOpsInline) return ops;
    Json brief = Json::array();
    for (const Json& op : ops) {
        Json item = Json::object();
        if (op.is_object()) {
            for (const auto& [key, value] : op.items()) {
                const bool plain = value.is_string() || value.is_number() || value.is_boolean();
                if (plain && code_points(core::py_str(value)) < 80) item[key] = value;
            }
        }
        brief.push_back(std::move(item));
    }
    ops_asset = store.put_bytes(core::dump_canonical(ops), kOpsSuffix);
    return brief;
}

}  // namespace

RevisionConflict::RevisionConflict(std::int64_t expected, std::int64_t found)
    : core::Error("revision_conflict",
                  "revision conflict: expected " + std::to_string(expected) + ", found " + std::to_string(found)) {}

NeedsMigration::NeedsMigration(const fs::path& dir, int version)
    : core::Error("needs_migration",
                  "this book is in the version " + std::to_string(version) + " format, which this build does not write; "
                  "convert it to a new folder with `genko migrate " + path_to_utf8(dir) +
                  " <new folder>` (the book itself is left as it is)",
                  path_to_utf8(dir)) {}

DiskState read_disk_state(const fs::path& dir) {
    DiskState out;
    const fs::path file = dir / "project.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return out;
    core::ParseOptions parse;
    parse.universal_newlines = true;
    out.payload = core::parse_python_json(read_file(file), nullptr, parse);
    if (!out.payload.is_object()) throw core::Error("format", "project.json must hold an object, not " + core::py_repr(out.payload));
    out.exists = true;
    out.version = project_version(out.payload);
    const auto revision = out.payload.find("revision");
    out.revision = revision != out.payload.end() && core::py_truthy(*revision) ? core::py_int(*revision) : 0;
    out.state_ref = state_ref(out.payload);
    return out;
}

Json ordered_like(const Json& value, const Json& model) {
    const auto order = [](const auto& self, const Json& v, const Json& m, bool top) -> Json {
        if (v.is_object() && m.is_object()) {
            Json out = Json::object();
            for (const auto& [key, model_value] : m.items()) {
                const auto it = v.find(key);
                if (it != v.end()) {
                    out[key] = self(self, *it, model_value, false);
                } else if (top && (key == "revision" || key == "writer")) {
                    out[key] = model_value;
                }
            }
            for (const auto& [key, item] : v.items()) {
                if (!out.contains(key)) out[key] = item;
            }
            return out;
        }
        if (v.is_array() && m.is_array() && v.size() == m.size()) {
            Json out = Json::array();
            for (std::size_t i = 0; i < v.size(); ++i) out.push_back(self(self, v[i], m[i], false));
            return out;
        }
        return v;
    };
    return order(order, value, model, true);
}

Saver::Saver(const ProjectLock& lock) : lock_(lock) {}

SaveResult Saver::save(const core::Document& doc, const SaveRequest& request) {
    if (!doc.read_only_reason.empty()) throw core::Error("read_only", doc.read_only_reason);
    return run(request, [&doc](AssetStore& store) { return project_payload_v4(doc, store); });
}

SaveResult Saver::save_payload(const Json& payload, const SaveRequest& request) {
    if (!payload.is_object()) throw core::Error("value", "a project.json payload is an object");
    return run(request, [&payload](AssetStore&) { return payload; });
}

SaveResult Saver::run(const SaveRequest& request, const std::function<Json(AssetStore&)>& make_payload) {
    if (!lock_.held()) throw core::Error("value", "a book is saved only under its project lock");
    const fs::path dir = lock_.project();
    if (!request.txn.empty() && !core::is_txn_id(request.txn)) {
        throw core::Error("value", "a transaction id is 32 lowercase hex digits, not " + core::py_repr_str(request.txn));
    }

    const DiskState disk = read_disk_state(dir);
    if (disk.exists && disk.version < 4) throw NeedsMigration(dir, disk.version);  // (before anything is written)
    journal::repair(dir);  // (§4.3: before every write; nothing is written when it fails)
    const journal::Lines lines = journal::read_lines(journal::journal_file(dir));
    if (!request.txn.empty()) {
        const auto txns = journal::transactions(lines);
        if (const journal::Transaction* done = journal::find_committed(txns, request.txn)) {
            SaveResult result;  // (§4.3: a transaction is committed once; a retry gets its revision)
            result.revision = done->rev;
            result.txn = done->txn;
            result.state_ref = string_at(done->prepare, "after");
            result.project_sha256 = string_at(done->prepare, "project_sha256");
            result.already_committed = true;
            const auto audit = done->prepare.find("audit");
            if (audit != done->prepare.end()) result.audit = *audit;
            return result;
        }
    }
    if (disk.revision != request.base_revision) throw RevisionConflict(request.base_revision, disk.revision);
    const std::int64_t revision = std::max(disk.revision, journal::max_revision(lines)) + 1;
    const std::string txn = request.txn.empty() ? core::new_txn_id() : request.txn;

    AssetStore store(dir);
    Json payload;
    std::string text;
    std::string after;
    std::optional<std::string> before;
    Json ops_record;
    std::string ops_asset;
    {
        fault::StageScope stage(fault::Stage::assets);
        payload = make_payload(store);
        payload["revision"] = revision;  // (in their places)
        payload["writer"] = writer_info();
        text = core::dump_python_indent2(payload);
        after = store.put_bytes(state_text(payload), kStateSuffix);
        if (disk.exists) before = store.put_bytes(state_text(disk.payload), kStateSuffix);  // (kept for Undo)
        if (!request.ops.is_null()) ops_record = journal_ops(request.ops, store, ops_asset);
    }
    if (request.expect_state && *request.expect_state != after) {
        throw core::Error("internal", "the book would not come out as the state asked for (" + after + ", not " +
                                          *request.expect_state + ")");
    }
    const Json changes = core::gate_changes(disk.exists ? &disk.payload : nullptr, payload);
    const std::string project_sha = sha256_hex(text);

    Json prepare = Json::object();
    prepare["v"] = 4;
    prepare["kind"] = "prepare";
    prepare["txn"] = txn;
    prepare["action"] = request.action;
    prepare["rev"] = revision;
    prepare["base_rev"] = disk.revision;
    prepare["actor"] = request.actor;
    prepare["at"] = journal::now_seconds();
    prepare["before"] = before ? Json(*before) : Json(nullptr);
    prepare["after"] = after;
    prepare["project_sha256"] = project_sha;
    if (!request.target_txn.empty()) prepare["target"] = request.target_txn;
    if (!request.ops.is_null()) prepare["ops"] = ops_record;
    if (!ops_asset.empty()) prepare["ops_asset"] = ops_asset;
    if (!changes.empty()) prepare["audit"] = changes;

    SaveResult result;
    result.revision = revision;
    result.txn = txn;
    result.state_ref = after;
    result.project_sha256 = project_sha;
    result.audit = changes;
    try {
        {
            fault::StageScope stage(fault::Stage::prepare);
            journal::append(journal::journal_file(dir), prepare);
        }
        {
            fault::StageScope stage(fault::Stage::project);
            write_atomic(dir / "project.json", text);
        }
        if (const Json line = journal::audit_line(prepare); !line.is_null()) {
            fault::StageScope stage(fault::Stage::audit);
            journal::append(journal::audit_file(dir), line);
        }
        {
            fault::StageScope stage(fault::Stage::commit);
            journal::append(journal::journal_file(dir), journal::commit_line(txn, revision));
        }
    } catch (const core::Error&) {
        // Settle the transaction now (§4.3: right after a failed write): project.json may have been replaced.
        bool committed = false;
        try {
            journal::repair(dir);
            committed = journal::find_committed(journal::transactions(journal::read_lines(journal::journal_file(dir))), txn) !=
                        nullptr;
        } catch (const core::Error&) {
            // (the repair failed too: the next write repairs first, and writes nothing until it can)
        }
        if (!committed) throw;
        result.repaired = true;
    }
    return result;
}

}  // namespace genko::storage
