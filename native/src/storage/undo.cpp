#include "storage/undo.hpp"

#include <cerrno>

#include "core/actor.hpp"
#include "core/gates.hpp"
#include "core/pyconv.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;

Json RestoreResult::to_json() const {
    Json out = Json::object();
    out["ok"] = true;
    out["kind"] = kind;
    out["rev"] = rev;
    out["revision"] = revision;
    return out;
}

RestoreResult restore(const ProjectLock& lock, const std::string& actor, bool redo, bool force) {
    const fs::path dir = lock.project();
    const std::string kind = redo ? "redo" : "undo";
    const DiskState disk = read_disk_state(dir);
    if (!disk.exists) {
        throw core::Error("not_found", os_error_text(ENOENT, dir / "project.json"), path_to_utf8(dir / "project.json"));
    }
    if (disk.version < 4) throw NeedsMigration(dir, disk.version);  // (before anything is written)
    journal::repair(dir);

    journal::Stacks stacks = journal::stacks(dir);
    auto& stack = redo ? stacks.redo : stacks.undo;
    if (stack.empty()) throw core::Error(redo ? "nothing_to_redo" : "nothing_to_undo", "nothing to " + kind);
    const journal::HistoryItem target = stack.back();
    const std::optional<std::string>& expected = redo ? target.before : target.after;
    if (!(expected && *expected == disk.state_ref) && !force) {
        throw core::Error("external_change", "project.json changed outside the journal; use --force to restore anyway");
    }
    const bool same_actor = target.actor.is_string() && target.actor.get_ref<const std::string&>() == actor;
    if (!redo && !same_actor && !force) {
        throw core::Error("other_actor", "the latest change is by " + core::py_str(target.actor) + "; use --force to undo it as " + actor);
    }
    const std::optional<std::string>& wanted = redo ? target.after : target.before;
    if (!wanted) {
        const std::string& old = redo ? target.after_old : target.before_old;
        if (!old.empty()) {  // (a v3 snapshot that could not be converted when the book was)
            throw core::Error("missing_snapshot", "snapshot " + old + " is missing from assets/");
        }
        throw core::Error("no_before", "the project did not exist before this change");
    }
    AssetStore store(dir);
    const auto data = store.get_bytes(*wanted, kStateSuffix);
    if (!data) throw core::Error("missing_snapshot", "snapshot " + *wanted + " is missing from assets/");
    const Json state = core::parse_python_json(*data);
    if (AssetStore::ref(*data) != *wanted || !state.is_object()) {
        throw core::Error("missing_snapshot", "snapshot " + *wanted + " does not hold the state its name says");
    }
    const Json changes = core::gate_changes(&disk.payload, state);
    if (!changes.empty() && !core::can_approve(actor)) {
        std::string what;
        for (std::size_t i = 0; i < changes.size() && i < 3; ++i) {
            if (i > 0) what += ", ";
            what += changes[i]["what"].get<std::string>();
        }
        throw core::Error("needs_person", "this " + kind + " changes approvals (" + what + "); only a person can do that");
    }
    // The state goes back as it was saved; the book read from it gives project.json's key order.
    const LoadResult loaded = load_document_payload(state, dir);
    if (!loaded.report.clean()) {
        throw core::Error("missing_snapshot", "snapshot " + *wanted + " cannot be restored: " + loaded.document.read_only_reason);
    }
    const Json payload = ordered_like(state, project_payload_v4(loaded.document, store));

    SaveRequest request;
    request.actor = actor;
    request.base_revision = disk.revision;
    request.action = kind;
    request.target_txn = target.id;
    request.expect_state = *wanted;
    Saver saver(lock);
    const SaveResult saved = saver.save_payload(payload, request);
    RestoreResult result;
    result.kind = kind;
    result.rev = target.rev;
    result.revision = saved.revision;
    result.txn = saved.txn;
    return result;
}

RestoreResult restore(const fs::path& dir, const std::string& actor, bool redo, bool force) {
    ProjectLock lock(dir, actor);
    lock.try_acquire();
    return restore(lock, actor, redo, force);
}

}  // namespace genko::storage
