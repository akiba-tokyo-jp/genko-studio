#include "service/book.hpp"

#include <QRegularExpression>
#include <QString>

#include "core/actor.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyops.hpp"
#include "render/ops_registry.hpp"
#include "render/timelapse.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"

namespace genko::service {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

core::Document load_episode(const std::filesystem::path& dir) { return storage::load_document(dir).document; }

void rethrow_python(const core::ApplyError& error) {
    if (error.code() != "python_error") return;
    // "ops[i] <op>: <Type>: <message>" (or "<Type>" alone), "<Type>: <message>", or a ValueError's message alone
    static const QRegularExpression prefix(QStringLiteral(R"(^ops\[\d+\] \S+: )"));
    static const QRegularExpression typed(QStringLiteral(R"(^([A-Za-z_][A-Za-z0-9_]*(?:Error|Exception|Interrupt))(?:: (.*))?$)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    QString text = QString::fromStdString(error.what());
    text.remove(prefix);
    const auto m = typed.match(text);
    if (!m.hasMatch()) throw core::PyValueError(text.toStdString());
    const std::string type = m.captured(1).toStdString();
    const std::string message = m.captured(2).toStdString();
    if (type == "TypeError") throw core::PyTypeError(message);
    if (type == "ValueError") throw core::PyValueError(message);
    throw core::PyUncaught(type, message);
}

core::ApplyResult apply_ops(const core::Document& doc, const Json& ops, const std::string& actor, bool dry_run) {
    core::ApplyResult result;
    try {
        result = bus().apply(doc, ops, core::Actor(actor), dry_run);
    } catch (const core::ApplyError& error) {
        rethrow_python(error);
        throw;
    }
    (void)core::new_id();  // (Python's apply_ops answers a "job_id", new_id(), after every batch it applies)
    return result;
}

core::ApplyResult Work::apply(const Json& ops, const std::string& actor) {
    core::ApplyResult result = apply_ops(doc, ops, actor);
    doc = result.doc;
    for (const Json& op : result.journal_ops) journal.push_back(op);
    return result;
}

std::optional<std::size_t> page_at(const core::Document& doc, const Json& index) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (core::py_equals(doc.pages[i]->index.json(), index)) return i;
    }
    return std::nullopt;
}

void refuse_old_book(const std::filesystem::path& dir) {
    storage::DiskState disk;
    try {
        disk = storage::read_disk_state(dir);
    } catch (const std::exception&) {  // (a project.json that cannot be read: the reading says so, as Python's)
        return;
    }
    if (disk.exists && disk.version < 4) throw storage::NeedsMigration(dir, disk.version);
}

Session::Session(const std::filesystem::path& dir, const std::string& agent) : dir_(dir), lock_(dir, agent) {
    refuse_old_book(dir_);  // (before the lock file is made and the journal settled: nothing written)
    try {
        lock_.try_acquire();
    } catch (const storage::LockedError& locked) {  // (Python's ProjectLock raises ApplyError)
        throw core::ApplyError(locked.what(), "locked");
    }
    storage::journal::repair(dir_);  // (the book is read at its last consistent point)
    work_.doc = storage::load_document(dir_).document;
    base_revision_ = work_.doc.revision;
}

storage::SaveResult Session::save(const std::string& actor) {
    storage::SaveRequest request;
    request.actor = actor;
    request.base_revision = base_revision_;
    request.ops = work_.journal;
    core::Document& doc_ = work_.doc;
    const bool lapse = render::timelapse::is_on(doc_);  // (タイムラプス: the pages this save changes)
    Json before;
    if (lapse) {
        try {
            before = storage::read_disk_state(dir_).payload;
        } catch (const std::exception&) {  // (the timelapse never stops a save: every page is recorded then)
        }
    }
    const storage::SaveResult saved = storage::Saver(lock_).save(doc_, request);
    if (lapse && !saved.already_committed) render::timelapse::after_save(dir_, doc_, before);
    doc_.revision = saved.revision;
    base_revision_ = saved.revision;
    work_.journal = Json::array();
    return saved;
}

}  // namespace genko::service
