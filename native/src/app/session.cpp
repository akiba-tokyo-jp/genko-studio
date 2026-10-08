#include "app/session.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <utility>

#include "app/config.hpp"
#include "app/perf.hpp"
#include "core/actor.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "render/ops_registry.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "storage/writer.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

struct Session::Job {
    enum class Kind { Save, Probe, Rebase, Recovery, DeleteRecovery, SaveAs, ReadRest };
    struct Step {
        Action::Kind kind = Action::Kind::Edit;
        DocPtr doc;            // Edit: the book to save
        Json journal_ops = Json::array();
        std::string action = "edit";
        std::string txn;
        std::string expect_top;  // Undo/Redo: the transaction that must be on top of the journal's stack
    };
    Kind kind = Kind::Save;
    fs::path dir;
    std::string actor;
    std::chrono::milliseconds lock_wait{1500};
    std::int64_t base_revision = 0;
    std::vector<Step> steps;
    // Recovery / DeleteRecovery / SaveAs
    fs::path root;
    std::string book_id;
    DocPtr doc;
    std::optional<fs::path> book;
    std::uint64_t generation = 0;
    std::uint64_t ticket = 0;
    std::shared_ptr<storage::LoadCache> cache;  // ReadRest: what was read already
};

struct Session::JobResult {
    struct Step {
        std::int64_t revision = 0;
        std::string txn;
        bool repaired = false;
        DocPtr reloaded;  // DiskUndo/DiskRedo: the book as it is now
        std::int64_t undo_depth = 0;
        std::int64_t redo_depth = 0;
    };
    Job::Kind kind = Job::Kind::Save;
    bool ok = false;
    QString code;
    QString message;
    std::vector<Step> steps;  // the steps that succeeded
    std::int64_t revision = 0;
    std::int64_t disk_revision = -1;  // Save: the book's revision when the job ended (still under its lock)
    DocPtr doc;
    std::int64_t undo_depth = 0;
    std::int64_t redo_depth = 0;
    fs::path recovery_path;
    QDateTime recovery_time;
    std::uint64_t generation = 0;
    std::uint64_t ticket = 0;
};

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

// The journal's Undo and Redo depths (saved changes that have a state before them, and the ones undone).
std::pair<std::int64_t, std::int64_t> journal_depths(const fs::path& dir) {
    const auto stacks = storage::journal::stacks(dir);
    std::int64_t undo = 0;
    for (const auto& item : stacks.undo) {
        if (item.before) ++undo;
    }
    return {undo, static_cast<std::int64_t>(stacks.redo.size())};
}

// The revision a committed transaction made, if it is committed.
std::optional<std::int64_t> committed_revision(const fs::path& dir, const std::string& txn) {
    if (txn.empty()) return std::nullopt;
    const auto txns = storage::journal::transactions(storage::journal::read_lines(storage::journal::journal_file(dir)));
    if (const auto* done = storage::journal::find_committed(txns, txn)) return done->rev;
    return std::nullopt;
}

DocPtr load_shared(const fs::path& dir) {
    storage::LoadResult loaded = storage::load_document(dir);
    return std::make_shared<const core::Document>(std::move(loaded.document));
}

// recovery.json: what a recovery point holds and where it belongs (shown before it is taken).
Json recovery_meta(const core::Document& doc, const std::optional<fs::path>& book, std::int64_t base_revision,
                   std::uint64_t generation, const std::string& actor, const std::string& state) {
    Json meta = Json::object();
    meta["book"] = book ? Json(core::path_to_utf8(*book)) : Json(nullptr);
    meta["book_id"] = doc.book_id;
    meta["base_revision"] = base_revision;
    meta["generation"] = static_cast<std::int64_t>(generation);
    meta["written"] = storage::journal::now_seconds();
    meta["actor"] = actor;
    meta["title"] = doc.title;
    meta["pages"] = static_cast<std::int64_t>(doc.pages.size());
    meta["state"] = state;
    return meta;
}

// The bytes of the precise colour pictures `before` has and `after` no longer has (each picture once).
std::size_t replaced_precise_bytes(const core::Document& before, const core::Document& after) {
    std::vector<const std::string*> gone;
    for (std::size_t i = 0; i < before.pages.size(); ++i) {
        if (i < after.pages.size() && after.pages[i] == before.pages[i]) continue;
        for (const core::Layer& layer : before.pages[i]->layers)
            if (layer.color_raster) gone.push_back(layer.color_raster.get());
    }
    if (gone.empty()) return 0;
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    for (const core::PagePtr& page : after.pages) {
        for (const core::Layer& layer : page->layers) {
            if (!layer.color_raster) continue;
            const auto at = std::lower_bound(gone.begin(), gone.end(), layer.color_raster.get());
            if (at != gone.end() && *at == layer.color_raster.get()) gone.erase(at);
        }
    }
    std::size_t bytes = 0;
    for (const std::string* picture : gone) bytes += picture->size();
    return bytes;
}

}  // namespace

// --- the worker's side -------------------------------------------------------------------------------------------

Session::JobResult Session::execute(const Job& job) {
    JobResult r;
    r.kind = job.kind;
    r.generation = job.generation;
    r.ticket = job.ticket;
    try {
        switch (job.kind) {
        case Job::Kind::Save: {
            storage::ProjectLock lock(job.dir, job.actor);
            lock.acquire(job.lock_wait);
            std::int64_t base = job.base_revision;
            for (const Job::Step& step : job.steps) {
                JobResult::Step done;
                if (step.kind == Action::Kind::Edit) {
                    storage::SaveRequest request;
                    request.actor = job.actor;
                    request.base_revision = base;
                    request.ops = step.journal_ops;
                    request.action = step.action;
                    request.txn = step.txn;
                    const storage::SaveResult saved = storage::Saver(lock).save(*step.doc, request);
                    done.revision = saved.revision;
                    done.txn = saved.txn;
                    done.repaired = saved.repaired;
                } else {
                    const bool redo = step.kind == Action::Kind::Redo || step.kind == Action::Kind::DiskRedo;
                    storage::journal::repair(job.dir);
                    if (const auto rev = committed_revision(job.dir, step.txn)) {
                        done.revision = *rev;  // (saved before a failure was reported: once only)
                        done.txn = step.txn;
                    } else {
                        const storage::DiskState disk = storage::read_disk_state(job.dir);
                        if (disk.revision != base) throw storage::RevisionConflict(base, disk.revision);
                        if (!step.expect_top.empty()) {
                            const auto stacks = storage::journal::stacks(job.dir);
                            const auto& stack = redo ? stacks.redo : stacks.undo;
                            if (stack.empty() || stack.back().id != step.expect_top) {
                                throw core::Error("external_change",
                                                  "the latest saved change is not the one this session " +
                                                      std::string(redo ? "undid" : "made"));
                            }
                        }
                        const storage::RestoreResult restored = storage::restore(lock, job.actor, redo, false, step.txn);
                        done.revision = restored.revision;
                        done.txn = restored.txn;
                    }
                    if (step.kind == Action::Kind::DiskUndo || step.kind == Action::Kind::DiskRedo) {
                        done.reloaded = load_shared(job.dir);
                        std::tie(done.undo_depth, done.redo_depth) = journal_depths(job.dir);
                    }
                }
                base = done.revision;
                r.steps.push_back(std::move(done));
            }
            r.revision = base;
            // (a step found already committed — by a repair, after a failure was reported — leaves base at that
            // revision: the book may have moved on since, by another writer)
            r.disk_revision = storage::read_disk_state(job.dir).revision;
            r.ok = true;
            break;
        }
        case Job::Kind::Probe: {
            const storage::DiskState disk = storage::read_disk_state(job.dir);
            r.revision = disk.exists ? disk.revision : -1;
            r.ok = true;
            break;
        }
        case Job::Kind::Rebase: {
            storage::ProjectLock lock(job.dir, job.actor);
            lock.acquire(job.lock_wait);
            storage::journal::repair(job.dir);
            r.doc = load_shared(job.dir);
            r.revision = r.doc->revision;
            std::tie(r.undo_depth, r.redo_depth) = journal_depths(job.dir);
            r.ok = true;
            break;
        }
        case Job::Kind::Recovery: {
            const fs::path folder = job.root / job.book_id;
            storage::make_dirs_durable(folder);
            storage::AssetStore store(folder);
            Json payload = storage::project_payload_v4(*job.doc, store);
            payload["revision"] = job.base_revision;
            const std::string state = storage::state_ref(payload);
            storage::write_atomic(folder / "state.json", core::dump_python_indent2(payload));
            storage::write_atomic(folder / "recovery.json",
                                  core::dump_python_indent2(
                                      recovery_meta(*job.doc, job.book, job.base_revision, job.generation, job.actor, state)));
            r.recovery_path = folder;
            r.recovery_time = QDateTime::currentDateTime();
            r.ok = true;
            break;
        }
        case Job::Kind::DeleteRecovery: {
            std::error_code ec;
            fs::remove_all(job.root / job.book_id, ec);
            r.ok = !ec;
            break;
        }
        case Job::Kind::SaveAs: {
            std::error_code ec;
            if (fs::exists(job.dir / "project.json", ec)) {
                throw core::Error("exists", "a book is there already: " + core::path_to_utf8(job.dir / "project.json"));
            }
            storage::ProjectLock lock(job.dir, job.actor);
            lock.acquire(job.lock_wait);
            if (fs::exists(job.dir / "project.json", ec)) {
                throw core::Error("exists", "a book is there already: " + core::path_to_utf8(job.dir / "project.json"));
            }
            storage::SaveRequest request;
            request.actor = job.actor;
            request.base_revision = 0;
            request.ops = Json::array();
            const storage::SaveResult saved = storage::Saver(lock).save(*job.doc, request);
            r.revision = saved.revision;
            r.doc = job.doc;
            r.ok = true;
            break;
        }
        case Job::Kind::ReadRest: {
            storage::LoadOptions load;
            load.cache = job.cache;
            r.doc = std::make_shared<const core::Document>(std::move(storage::load_document(job.dir, load).document));
            r.revision = r.doc->revision;
            try {
                std::tie(r.undo_depth, r.redo_depth) = journal_depths(job.dir);
            } catch (const core::Error&) {
            }
            r.ok = true;
            break;
        }
        }
    } catch (const storage::LockedError& error) {
        r.code = QStringLiteral("locked");
        r.message = QString::fromUtf8(error.what());
    } catch (const core::Error& error) {
        r.code = qs(error.code());
        r.message = QString::fromUtf8(error.what());
    } catch (const std::exception& error) {
        r.code = QStringLiteral("internal");
        r.message = QString::fromUtf8(error.what());
    }
    return r;
}

// --- opening ---------------------------------------------------------------------------------------------------

std::shared_ptr<Session> Session::open(const fs::path& dir) { return open(dir, Options{}); }

Session::Opened Session::read(const fs::path& dir, const Options& options) {
    const std::string actor = options.actor.empty() ? default_actor() : options.actor;
    const storage::DiskState disk = storage::read_disk_state(dir);
    if (!disk.exists) {
        throw core::Error("not_found", storage::os_error_text(ENOENT, dir / "project.json"), core::path_to_utf8(dir / "project.json"));
    }
    if (disk.version >= 4) {
        // a save that stopped half way is settled before the book is read (when no one else is writing it now)
        try {
            storage::ProjectLock lock(dir, actor, storage::ProjectLock::Options{false, true});
            lock.acquire(options.lock_wait);
            storage::journal::repair(dir);
        } catch (const core::Error&) {
            // (another writer has it: the book is read as it is)
        }
    }
    Opened out;
    storage::LoadOptions load;
    if (options.defer_pages && disk.version >= 4) {
        // (the first page now, the others on the session's worker: read_rest())
        load.cache = std::make_shared<storage::LoadCache>();
        load.assets_of_page = 0;
    }
    out.doc = std::move(storage::load_document(dir, load).document);
    if (!out.doc.deferred.empty()) out.read_so_far = load.cache;
    if (disk.version >= 4) {
        try {
            std::tie(out.undo_depth, out.redo_depth) = journal_depths(dir);
        } catch (const core::Error&) {
        }
    }
    // a recovery point written after the book's last save (a save that failed, a crash after it)
    const fs::path root = options.recovery_root.empty() ? recovery_root() : options.recovery_root;
    if (!out.doc.book_id.empty()) {
        if (auto point = find_recovery(root, out.doc.book_id)) {
            out.recovery_present = true;
            out.recovery_folder = point->folder;
            const storage::DiskState now = storage::read_disk_state(dir);
            const QDateTime file_time = QFileInfo(QString::fromStdString(core::path_to_utf8(dir / "project.json"))).lastModified();
            if (point->state_ref != now.state_ref && (point->base_revision >= now.revision || point->written > file_time)) {
                out.offer = std::move(point);
            }
        }
    }
    return out;
}

std::shared_ptr<Session> Session::from(Opened opened, const fs::path& dir, Options options) {
    if (options.actor.empty()) options.actor = default_actor();
    auto session = std::make_shared<Session>(std::move(opened.doc), dir, options);
    session->disk_undo_ = opened.undo_depth;
    session->disk_redo_ = opened.redo_depth;
    // (a recovery point is kept until the book holds everything: the next save that leaves nothing unsaved removes it)
    session->recovery_written_ = opened.recovery_present;
    session->recovery_path_ = opened.recovery_folder;
    session->recovery_offer_ = std::move(opened.offer);
    session->read_so_far_ = std::move(opened.read_so_far);
    if (options.read_rest_now) session->read_rest();
    return session;
}

void Session::read_rest() {
    if (!loading_ || reading_rest_ || !path_ || discarded_) return;
    reading_rest_ = true;
    Job job;
    job.kind = Job::Kind::ReadRest;
    job.dir = *path_;
    job.cache = read_so_far_;
    run(std::move(job));
}

void Session::finish_reading(const JobResult& r) {
    if (!r.ok) {
        if (++read_failures_ < 3) {
            // (a drive back, a folder reachable again: read again a little later)
            emit notice(QStringLiteral("原稿の残りのページを読み込めませんでした。もう一度読み込みます（%1）").arg(r.message), false);
            read_retry_pending_ = true;
            QTimer::singleShot(options_.read_retry * read_failures_, this, [this] {
                read_retry_pending_ = false;
                read_rest();
            });
            emit statusChanged();
            return;
        }
        // the pages read stay shown; nothing can be changed or written, as for any book that cannot be read whole
        read_failed_ = true;
        read_so_far_.reset();
        read_only_ = "the rest of the book could not be read: " + r.message.toStdString();
        idle_timer_.stop();
        longest_timer_.stop();
        save_wanted_ = false;
        emit notice(QStringLiteral("原稿の残りのページを読み込めませんでした。読み取り専用で開いています（%1）").arg(r.message), true);
        if (save_as_after_reading_) {
            save_as_after_reading_.reset();
            emit savedAs(false, QString::fromStdString(read_only_));
        }
        emit statusChanged();
        return;
    }
    read_so_far_.reset();
    loading_ = false;
    const DocPtr before = doc_;
    base_revision_ = r.revision;
    last_revision_ = r.revision;
    disk_undo_ = r.undo_depth;
    disk_redo_ = r.redo_depth;
    if (!r.doc->read_only_reason.empty()) {
        // A problem on a page read just now: the book opens read-only, as it would have at once. The changes made
        // meanwhile cannot be saved to it, so they are not kept as if they could be.
        const bool told = !before->read_only_reason.empty();  // (read-only from its first page on: nothing new to say)
        const std::size_t dropped = done_.size();
        doc_ = r.doc;
        done_.clear();
        undone_.clear();
        queue_.clear();
        read_only_ = doc_->read_only_reason;
        idle_timer_.stop();
        longest_timer_.stop();
        save_wanted_ = false;
        ++generation_;
        touch(BookChange{BookChange::Why::Reload, Json::array(), before, doc_});
        if (!told) {
            emit notice(dropped == 0 ? QStringLiteral("後のページに問題が見つかったため、原稿を読み取り専用で開きました。")
                                     : QStringLiteral("後のページに問題が見つかったため、原稿を読み取り専用で開きました。読み込み中の変更 %1 件は保存できないため取り消しました。")
                                           .arg(dropped),
                        true);
        }
        if (const auto target = std::exchange(save_as_after_reading_, std::nullopt)) save_as(*target);  // (refused: read-only)
        return;
    }
    // The changes made meanwhile, again on the whole book: they kept to the page that was read (CommandBus refused
    // any other), so they apply as they did. (Nothing else is queued while pages are read: no save has run.) Another
    // writer may have saved meanwhile: a page number that now names another page is not written to (as in a rebase).
    const core::CommandBus bus(*registry_);
    QStringList conflicts_found;
    QStringList* failures = &conflicts_found;
    QStringList redo_lost;
    const auto replay = [&](const std::shared_ptr<Change>& change, const DocPtr& base) -> std::shared_ptr<Change> {
        auto again = std::make_shared<Change>();
        again->id = new_change_id();
        again->ops = change->ops;
        again->recover = change->recover;
        again->before = base;
        if (change->recover) {
            // (an adopted recovery point is the whole book already)
            again->journal_ops = change->journal_ops;
            again->after = change->after;
            return again;
        }
        try {
            // (the pages its ops name by number — while pages are read, only ops on a read page carry one)
            for (const Json& op : change->ops) {
                const auto named = op.is_object() ? op.find("page") : op.end();
                if (!op.is_object() || named == op.end()) continue;
                const auto page_named = [&](const DocPtr& doc) -> const core::Page* {
                    for (const auto& page : doc->pages) {
                        if (page->index.json() == *named) return page.get();
                    }
                    return nullptr;
                };
                const core::Page* then = page_named(change->before);
                const core::Page* now = page_named(base);
                if ((then == nullptr) != (now == nullptr) || (then != nullptr && then->id != now->id)) {
                    throw core::ApplyError("ページの番号とIDの対応が変わったため、読み込み中の変更を適用しませんでした。", "rebase_conflict");
                }
            }
            core::ScopedIdScript script(change->ids);
            core::ApplyResult result = bus.apply(*base, change->ops, core::Actor(actor_));
            again->journal_ops = result.journal_ops;
            again->ids = script.taken();
            again->after = std::make_shared<const core::Document>(std::move(result.doc));
            return again;
        } catch (const core::Error& error) {
            *failures << QString::fromUtf8(error.what());
            return nullptr;
        }
    };
    const bool changed_meanwhile = !done_.empty() || !undone_.empty();
    DocPtr current = r.doc;
    std::vector<std::shared_ptr<Change>> replayed;
    std::deque<Action> keep;
    for (const Action& action : queue_) {
        if (action.kind != Action::Kind::Edit || !action.change) continue;
        if (auto again = replay(action.change, current)) {
            current = again->after;
            replayed.push_back(again);
            keep.push_back(Action{Action::Kind::Edit, again, core::new_txn_id(), ++next_seq_, false});
        }
    }
    // what was undone meanwhile (in memory) can be redone, on top of the changes kept
    std::vector<std::shared_ptr<Change>> redo_chain;
    DocPtr tip = current;
    failures = &redo_lost;  // (not in the book: only the chance to redo them is lost)
    for (auto it = undone_.rbegin(); it != undone_.rend(); ++it) {
        auto again = replay(*it, tip);
        if (!again) break;  // (the ones after it were made on top of it)
        tip = again->after;
        redo_chain.push_back(again);
    }
    doc_ = current;
    done_ = std::move(replayed);
    undone_.assign(redo_chain.rbegin(), redo_chain.rend());
    queue_ = std::move(keep);
    // (a change made meanwhile, kept or undone in memory, replaced the journal's redo, as apply() does)
    if (changed_meanwhile) disk_redo_ = 0;
    read_only_ = doc_->read_only_reason;
    ++generation_;
    touch(BookChange{BookChange::Why::Reload, Json::array(), before, doc_});
    if (!conflicts_found.isEmpty()) emit conflicts(conflicts_found);
    if (const auto target = std::exchange(save_as_after_reading_, std::nullopt)) {
        save_as(*target);  // (the changes made meanwhile go with it)
    } else if (!queue_.empty()) {
        if (save_wanted_) {
            save_now();
        } else {
            schedule_save();
        }
    }
    // (the window's file watcher was not heeded while pages were read: the book may have moved on since)
    check_outside();
}

std::shared_ptr<Session> Session::open(const fs::path& dir, Options options) {
    Opened opened = read(dir, options);
    return from(std::move(opened), dir, std::move(options));
}

std::optional<RecoveryPoint> find_recovery(const fs::path& root, const std::string& book_id) {
    if (book_id.empty() || !core::is_book_id(book_id)) return std::nullopt;
    const fs::path folder = root / book_id;
    std::error_code ec;
    if (!fs::exists(folder / "recovery.json", ec) || !fs::exists(folder / "state.json", ec)) return std::nullopt;
    try {
        const Json meta = core::parse_python_json(storage::read_file(folder / "recovery.json"));
        if (!meta.is_object()) return std::nullopt;
        RecoveryPoint point;
        point.folder = folder;
        const double written = meta.value("written", 0.0);
        point.written = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(written * 1000.0));
        point.base_revision = meta.value("base_revision", std::int64_t{0});
        point.pages = static_cast<std::size_t>(meta.value("pages", std::int64_t{0}));
        point.title = qs(meta.value("title", std::string()));
        point.state_ref = meta.value("state", std::string());
        return point;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Session::Session(core::Document doc, std::optional<fs::path> dir) : Session(std::move(doc), std::move(dir), Options{}) {}

Session::Session(core::Document doc, std::optional<fs::path> dir, Options options) : options_(std::move(options)) {
    registry_ = options_.registry != nullptr ? options_.registry : &render::ops_registry();
    actor_ = options_.actor.empty() ? default_actor() : options_.actor;
    path_ = std::move(dir);
    base_revision_ = doc.revision;
    last_revision_ = doc.revision;
    read_only_ = doc.read_only_reason;
    loading_ = !doc.deferred.empty();
    read_in_parts_ = loading_;
    doc_ = std::make_shared<const core::Document>(std::move(doc));
    idle_timer_.setSingleShot(true);
    longest_timer_.setSingleShot(true);
    retry_timer_.setSingleShot(true);
    // (Qt's usual timers may fire 5 % late: the autosave's 2 s and 10 s are kept to the millisecond)
    idle_timer_.setTimerType(Qt::PreciseTimer);
    longest_timer_.setTimerType(Qt::PreciseTimer);
    const auto checkpoint_due = [this] {
        // A lock on the book must not prevent the independently stored recovery
        // snapshot from being queued at the idle/continuous-edit deadline.
        request_recovery();
        save_now();
    };
    connect(&idle_timer_, &QTimer::timeout, this, checkpoint_due);
    connect(&longest_timer_, &QTimer::timeout, this, checkpoint_due);
    connect(&retry_timer_, &QTimer::timeout, this, &Session::save_now);
    worker_ = new QObject;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("genko-save"));
    thread_.start();
}

Session::~Session() {
    idle_timer_.stop();
    longest_timer_.stop();
    retry_timer_.stop();
    // The jobs already asked for are done first, then the thread stops: a save that has begun is finished (the book is
    // never left half written by closing), and the recovery point of work thrown away is removed.
    QThread* thread = &thread_;
    QMetaObject::invokeMethod(worker_, [thread] { thread->quit(); }, Qt::QueuedConnection);
    thread_.wait();
    delete worker_;
}

// --- editing -------------------------------------------------------------------------------------------------------

core::ApplyResult Session::apply(const Json& ops, const std::vector<std::string>& ids) {
    if (saving_as_) throw core::ApplyError("別名保存中です。完了してから編集してください。", "save_as_busy");
    if (ops.is_array() && ops.size() == 1 && ops[0].is_object()) {
        const auto name = ops[0].find("op");
        if (name != ops[0].end() && *name == Json("undo")) {
            undo();
            core::ApplyResult out{*doc_};
            out.applied = Json::array({"undo"});
            out.has_warnings = false;
            return out;
        }
    }
    if (!read_only_.empty()) throw core::ApplyError("this book is open read-only: " + read_only_, "read_only");
    if (discarded_) throw core::ApplyError("the book was closed", "closed");
    const core::CommandBus bus(*registry_);
    core::ScopedIdScript script(ids);
    core::ApplyResult result = bus.apply(*doc_, ops, core::Actor(actor_));
    auto change = std::make_shared<Change>();
    change->id = new_change_id();
    change->ops = ops;
    change->journal_ops = result.journal_ops;
    change->ids = script.taken();
    change->before = doc_;
    change->after = std::make_shared<const core::Document>(std::move(result.doc));
    change->held = replaced_precise_bytes(*change->before, *change->after);
    doc_ = change->after;
    done_.push_back(change);
    undone_.clear();
    disk_redo_ = 0;
    queue_.push_back(Action{Action::Kind::Edit, change, core::new_txn_id(), ++next_seq_, false});
    ++generation_;
    touch(BookChange{BookChange::Why::Edit, ops, change->before, change->after});
    schedule_save();
    return result;
}


void Session::trim_history() {
    std::size_t held = 0;
    for (const auto& change : done_) held += change->held;
    while (held > options_.history_bytes && done_.size() > 1 && path_) {
        const std::shared_ptr<Change> oldest = done_.front();
        if (!oldest->on_disk || oldest->undone_on_disk || oldest->recover) break;
        if (std::any_of(queue_.begin(), queue_.end(), [&](const Action& a) { return a.change == oldest; })) break;
        held -= oldest->held;
        done_.erase(done_.begin());
        ++disk_undo_;  // (in the journal, under the changes still in memory)
        trimmed_ = true;
    }
}

void Session::undo() {
    if (saving_as_) throw core::ApplyError("別名保存中です。完了してからUndoしてください。", "save_as_busy");
    if (!read_only_.empty()) throw core::ApplyError("this book is open read-only: " + read_only_, "read_only");
    if (!done_.empty()) {
        const std::shared_ptr<Change> change = done_.back();
        done_.pop_back();
        undone_.push_back(change);
        const auto queued = [&](Action::Kind kind) {
            return std::find_if(queue_.rbegin(), queue_.rend(),
                                [&](const Action& a) { return a.kind == kind && a.change == change && !a.in_flight; });
        };
        if (auto it = queued(Action::Kind::Edit); it != queue_.rend() && !trimmed_) {
            queue_.erase(std::next(it).base());  // (it never reached the disk: undone in memory only)
        } else if (it != queue_.rend()) {
            // Once changes of this session are undone through the journal (trim_history), an Undo reaching past
            // them reads the book again and its redo comes from the journal: a change undone before it was saved
            // goes to the journal too (made and undone), so that redo still finds it.
            queue_.push_back(Action{Action::Kind::Undo, change, core::new_txn_id(), ++next_seq_, false});
        } else if (auto redo = queued(Action::Kind::Redo); redo != queue_.rend()) {
            queue_.erase(std::next(redo).base());
        } else {
            queue_.push_back(Action{Action::Kind::Undo, change, core::new_txn_id(), ++next_seq_, false});
        }
        doc_ = change->before;
        ++generation_;
        touch(BookChange{BookChange::Why::Undo, change->ops, change->after, change->before});
        schedule_save();
        return;
    }
    if (loading_) throw core::ApplyError("原稿の残りのページを読み込み中です。読み込みが終わってから元に戻してください。", "loading");
    if (path_ && disk_undo_ > 0) {
        // a change saved before this session: undone through the journal, and the book read again after it
        queue_.push_back(Action{Action::Kind::DiskUndo, nullptr, core::new_txn_id(), ++next_seq_, false});
        --disk_undo_;
        ++disk_redo_;
        save_now();
        emit statusChanged();
        return;
    }
    throw core::ApplyError("nothing to undo", "nothing_to_undo");
}

void Session::redo() {
    if (saving_as_) throw core::ApplyError("別名保存中です。完了してからRedoしてください。", "save_as_busy");
    if (!read_only_.empty()) throw core::ApplyError("this book is open read-only: " + read_only_, "read_only");
    if (!undone_.empty()) {
        const std::shared_ptr<Change> change = undone_.back();
        undone_.pop_back();
        done_.push_back(change);
        const auto queued = std::find_if(queue_.rbegin(), queue_.rend(), [&](const Action& a) {
            return a.kind == Action::Kind::Undo && a.change == change && !a.in_flight;
        });
        const bool undo_in_flight =
            std::any_of(queue_.begin(), queue_.end(), [&](const Action& a) { return a.change == change && a.in_flight; });
        if (queued != queue_.rend()) {
            queue_.erase(std::next(queued).base());
        } else if (change->on_disk || undo_in_flight) {
            queue_.push_back(Action{Action::Kind::Redo, change, core::new_txn_id(), ++next_seq_, false});
        } else {
            queue_.push_back(Action{Action::Kind::Edit, change, core::new_txn_id(), ++next_seq_, false});
        }
        doc_ = change->after;
        ++generation_;
        touch(BookChange{BookChange::Why::Redo, change->ops, change->before, change->after});
        schedule_save();
        return;
    }
    if (loading_) throw core::ApplyError("原稿の残りのページを読み込み中です。読み込みが終わってからやり直してください。", "loading");
    if (path_ && disk_redo_ > 0) {
        queue_.push_back(Action{Action::Kind::DiskRedo, nullptr, core::new_txn_id(), ++next_seq_, false});
        --disk_redo_;
        ++disk_undo_;
        save_now();
        emit statusChanged();
        return;
    }
    throw core::ApplyError("nothing to redo", "nothing_to_redo");
}

bool Session::can_undo() const { return !saving_as_ && read_only_.empty() && (!done_.empty() || (path_ && disk_undo_ > 0 && !loading_)); }

bool Session::can_redo() const { return !saving_as_ && read_only_.empty() && (!undone_.empty() || (path_ && disk_redo_ > 0 && !loading_)); }

void Session::touch(BookChange change) {
    emit changed(change);
    emit statusChanged();
}

// --- saving --------------------------------------------------------------------------------------------------------

void Session::schedule_save() {
    if (!options_.autosave || discarded_) {
        emit statusChanged();
        return;
    }
    // (a book without a folder yet gets a recovery point at the same moments instead)
    idle_timer_.start(options_.idle);
    if (!longest_timer_.isActive()) longest_timer_.start(options_.longest);
    emit statusChanged();
}

void Session::save_now() {
    idle_timer_.stop();
    longest_timer_.stop();
    if (discarded_) return;
    if (loading_) {
        // (written when the rest of the book is read: a book missing pages is never written)
        save_wanted_ = true;
        emit statusChanged();
        return;
    }
    if (!path_) {
        if (!done_.empty()) request_recovery();
        return;
    }
    if (failure_code_ == QLatin1String("rebase_conflict") && !job_running_ && !rebasing_ && !saving_as_) {
        start_rebase();
        return;
    }
    if (job_running_ || rebasing_ || saving_as_) {
        save_wanted_ = true;
        return;
    }
    start_job();
}

void Session::run(Job job) {
    Session* self = this;
    ++worker_jobs_;
    QMetaObject::invokeMethod(
        worker_,
        [self, job = std::move(job)]() {
            JobResult result = execute(job);
            QMetaObject::invokeMethod(self, [self, result = std::move(result)]() { self->job_done(result); }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Session::start_job() {
    if (job_running_ || rebasing_ || saving_as_ || queue_.empty() || !path_ || discarded_) return;
    if (failure_code_ == QLatin1String("rebase_conflict")) return;
    simplify_queue();
    if (queue_.empty()) {
        emit statusChanged();
        return;
    }
    Job job;
    job.kind = Job::Kind::Save;
    job.dir = *path_;
    job.actor = actor_;
    job.lock_wait = options_.lock_wait;
    job.base_revision = base_revision_;
    job.generation = generation_;
    for (Action& action : queue_) {
        Job::Step step;
        step.kind = action.kind;
        step.txn = action.txn;
        if (action.kind == Action::Kind::Edit) {
            step.doc = action.change->after;
            step.journal_ops = action.change->journal_ops;
            step.action = action.change->recover ? "recover" : "edit";
        } else if (action.kind == Action::Kind::Undo || action.kind == Action::Kind::Redo) {
            step.expect_top = action.change->txn;
        }
        action.in_flight = true;
        job.steps.push_back(std::move(step));
        // (the book is read again after an undo or redo of a change from before this session: what comes after it is
        // applied to that book first)
        if (action.kind == Action::Kind::DiskUndo || action.kind == Action::Kind::DiskRedo) break;
    }
    job_running_ = true;
    save_wanted_ = false;
    perf::event("save_start", {{"steps", static_cast<std::int64_t>(job.steps.size())}});
    emit statusChanged();
    run(std::move(job));
}

void Session::job_done(const JobResult& r) {
    if (worker_jobs_ > 0) --worker_jobs_;
    switch (r.kind) {
    case Job::Kind::Save: {
        job_running_ = false;
        perf::event("save_end", {{"ok", r.ok}, {"steps", static_cast<std::int64_t>(r.steps.size())}});
        DocPtr reloaded;
        std::int64_t undo_depth = disk_undo_;
        std::int64_t redo_depth = disk_redo_;
        for (const JobResult::Step& step : r.steps) {
            if (queue_.empty() || !queue_.front().in_flight) break;
            const Action action = queue_.front();
            queue_.pop_front();
            if (const std::shared_ptr<Change>& change = action.change) {
                switch (action.kind) {
                case Action::Kind::Edit:
                    if (change->txn.empty() || !change->on_disk) change->txn = step.txn;
                    change->on_disk = true;
                    change->undone_on_disk = false;
                    break;
                case Action::Kind::Undo:
                    change->undone_on_disk = true;
                    break;
                case Action::Kind::Redo:
                    change->undone_on_disk = false;
                    break;
                default:
                    break;
                }
            }
            if (step.reloaded) {
                reloaded = step.reloaded;
                undo_depth = step.undo_depth;
                redo_depth = step.redo_depth;
            }
            base_revision_ = step.revision;
            last_revision_ = step.revision;
            last_saved_ = QDateTime::currentDateTime();
        }
        std::optional<Action> refused;
        for (Action& action : queue_) {
            if (action.in_flight && !r.ok && !refused) refused = action;
            action.in_flight = false;
        }
        if (r.ok) {
            failed_ = false;
            failure_.clear();
            failure_code_.clear();
            recovery_failed_ = false;
        } else {
            static const QStringList kRefusals = {QStringLiteral("other_actor"), QStringLiteral("needs_person"),
                                                  QStringLiteral("nothing_to_undo"), QStringLiteral("nothing_to_redo"),
                                                  QStringLiteral("no_before"), QStringLiteral("missing_snapshot")};
            const bool undo_refused = refused && refused->kind != Action::Kind::Edit && kRefusals.contains(r.code);
            if (undo_refused) {
                // the journal will not take this undo or redo back (another person's change, an approval…): the book stays
                // as the disk has it
                queue_.erase(std::find_if(queue_.begin(), queue_.end(), [&](const Action& a) { return a.seq == refused->seq; }));
                emit notice(r.message, true);
                if (refused->kind == Action::Kind::DiskUndo) {
                    ++disk_undo_;
                    --disk_redo_;
                } else if (refused->kind == Action::Kind::DiskRedo) {
                    ++disk_redo_;
                    --disk_undo_;
                }
                // (an undo of this session's change that was shown already: the book is read again)
                if (refused->kind == Action::Kind::Undo || refused->kind == Action::Kind::Redo) start_rebase();
            } else if (r.code == QLatin1String("revision_conflict") || r.code == QLatin1String("external_change")) {
                start_rebase();
            } else if (r.code == QLatin1String("locked")) {
                failed_ = true;
                failure_code_ = r.code;
                failure_ = r.message;
                retry_timer_.start(3000);  // (another writer: try again soon)
            } else {
                after_failure(r.code, r.message);
            }
        }
        if (reloaded) {
            rebase_onto(reloaded, base_revision_, undo_depth, redo_depth);
        } else if (r.ok && r.disk_revision >= 0 && r.disk_revision != base_revision_) {
            start_rebase();  // (another writer's change after ours: read the book again, ours kept on top)
        }
        simplify_queue();
        trim_history();  // (the changes just written may leave memory now)
        if (r.ok && queue_.empty() && recovery_written_ && doc_ && !doc_->book_id.empty()) {
            // the book holds everything now: its recovery point is of no more use
            Job remove;
            remove.kind = Job::Kind::DeleteRecovery;
            remove.root = options_.recovery_root.empty() ? recovery_root() : options_.recovery_root;
            remove.book_id = doc_->book_id;
            recovery_written_ = false;
            run(std::move(remove));
        }
        if (save_wanted_ && !queue_.empty() && !rebasing_) start_job();
        emit statusChanged();
        break;
    }
    case Job::Kind::Probe:
        if (r.ok && r.revision >= 0 && r.revision != base_revision_ && !job_running_ && !rebasing_ && !saving_as_) start_rebase();
        break;
    case Job::Kind::Rebase:
        rebasing_ = false;
        if (r.ok) {
            rebase_onto(r.doc, r.revision, r.undo_depth, r.redo_depth);
        } else if (r.code == QLatin1String("locked")) {
            retry_timer_.start(3000);
            failed_ = true;
            failure_code_ = r.code;
            failure_ = r.message;
        } else {
            after_failure(r.code, r.message);
        }
        if (!queue_.empty() && !rebasing_) start_job();
        emit statusChanged();
        break;
    case Job::Kind::Recovery:
        recovery_job_ = false;
        if (r.ok) {
            recovery_written_ = true;
            recovery_failed_ = false;
            recovery_failure_.clear();
            recovery_failure_code_.clear();
            recovery_generation_ = r.generation;
            recovery_time_ = r.recovery_time;
            recovery_path_ = r.recovery_path;
        } else {
            recovery_failed_ = true;
            recovery_failure_ = r.message;
            recovery_failure_code_ = r.code;
            emit notice(r.message, true);
        }
        if (recovery_requested_ && !saving_as_) {
            recovery_requested_ = false;
            if (!discarded_ && unsaved() && recovery_generation_ != generation_) request_recovery();
        }
        emit statusChanged();
        break;
    case Job::Kind::DeleteRecovery:
        break;
    case Job::Kind::SaveAs: {
        const std::optional<fs::path> target = saving_as_;
        saving_as_.reset();
        if (r.ok && target && r.ticket == save_as_ticket_) {
            const std::string old_book = doc_ ? doc_->book_id : std::string();
            // the changes asked for before the copy are in it; the ones made since go on in the new place
            std::erase_if(queue_, [&](const Action& a) {
                return std::find(before_save_as_.begin(), before_save_as_.end(), a.seq) != before_save_as_.end();
            });
            before_save_as_.clear();
            path_ = target;
            failed_ = false;
            failure_.clear();
            failure_code_.clear();
            disk_undo_ = 0;
            disk_redo_ = 0;
            auto copy = std::make_shared<core::Document>(*r.doc);
            copy->revision = r.revision;
            last_saved_ = QDateTime::currentDateTime();
            last_revision_ = r.revision;
            rebase_onto(copy, r.revision, 0, 0);
            recovery_requested_ = false;  // the latest snapshot is durable in the confirmed copy
            if (recovery_written_ && !old_book.empty()) {
                Job remove;
                remove.kind = Job::Kind::DeleteRecovery;
                remove.root = options_.recovery_root.empty() ? recovery_root() : options_.recovery_root;
                remove.book_id = old_book;
                recovery_written_ = false;
                run(std::move(remove));
            }
            emit savedAs(true, QString());
        } else {
            before_save_as_.clear();
            if (recovery_requested_) {
                recovery_requested_ = false;
                if (!discarded_) request_recovery();
            }
            if (!discarded_ && unsaved()) schedule_save();
            emit savedAs(false, r.message);
            emit notice(r.message, true);
        }
        if (save_wanted_ || !queue_.empty()) start_job();
        emit statusChanged();
        break;
    }
    case Job::Kind::ReadRest:
        reading_rest_ = false;
        if (loading_ && !discarded_) finish_reading(r);
        break;
    }
}

void Session::simplify_queue() {
    // an edit and its undo, an undo and its redo (neither written yet) cancel out
    bool again = true;
    while (again) {
        again = false;
        for (std::size_t i = 0; i + 1 < queue_.size(); ++i) {
            const Action& a = queue_[i];
            const Action& b = queue_[i + 1];
            if (a.in_flight || b.in_flight || !a.change || a.change != b.change) continue;
            const bool pair = (a.kind == Action::Kind::Edit && b.kind == Action::Kind::Undo) ||
                              (a.kind == Action::Kind::Undo && b.kind == Action::Kind::Redo) ||
                              (a.kind == Action::Kind::Redo && b.kind == Action::Kind::Undo);
            if (!pair) continue;
            if (a.kind == Action::Kind::Edit && (a.change->on_disk || trimmed_)) continue;  // (trimmed_: see undo())
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i), queue_.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            again = true;
            break;
        }
    }
}

void Session::after_failure(const QString& code, const QString& message) {
    failed_ = true;
    failure_code_ = code;
    failure_ = message;
    request_recovery();
    emit statusChanged();
}

void Session::request_recovery() {
    if (discarded_ || loading_ || !doc_ || doc_->book_id.empty()) return;
    if (recovery_job_) {
        recovery_requested_ = true;
        return;
    }
    if (recovery_written_ && recovery_generation_ == generation_) return;
    Job job;
    job.kind = Job::Kind::Recovery;
    job.root = options_.recovery_root.empty() ? recovery_root() : options_.recovery_root;
    job.book_id = doc_->book_id;
    job.doc = doc_;
    job.book = path_;
    job.base_revision = base_revision_;
    job.generation = generation_;
    job.actor = actor_;
    recovery_job_ = true;
    run(std::move(job));
}

void Session::write_recovery_copy() {
    recovery_written_ = recovery_written_ && recovery_generation_ == generation_;
    request_recovery();
}

void Session::start_rebase() {
    if (rebasing_ || saving_as_ || loading_ || !path_ || discarded_) return;
    rebasing_ = true;
    Job job;
    job.kind = Job::Kind::Rebase;
    job.dir = *path_;
    job.actor = actor_;
    job.lock_wait = options_.lock_wait;
    run(std::move(job));
    emit statusChanged();
}

void Session::rebase_onto(DocPtr fresh, std::int64_t revision, std::int64_t undo_depth, std::int64_t redo_depth) {
    const core::CommandBus bus(*registry_);
    QStringList conflicts_found;
    DocPtr current = std::move(fresh);
    std::vector<std::shared_ptr<Change>> replayed;
    std::deque<Action> keep;
    for (const Action& action : queue_) {
        if (action.kind != Action::Kind::Edit) {
            if (action.kind == Action::Kind::Undo || action.kind == Action::Kind::Redo) {
                conflicts_found << QStringLiteral("%1 could not be applied to the book as it is now")
                                       .arg(action.kind == Action::Kind::Undo ? QStringLiteral("undo") : QStringLiteral("redo"));
            }
            continue;
        }
        const std::shared_ptr<Change>& change = action.change;
        if (!change) continue;
        if (change->recover) {
            conflicts_found << QStringLiteral("the recovery point was not taken: the book changed meanwhile");
            continue;
        }
        try {
            // Page numbers are positional, not identity. Never replay against a
            // number that another writer has assigned to a different page.
            // Refuse changed placement instead of guessing the author's intent;
            // the conflict path below retains the complete local snapshot/history.
            const bool creates_page = std::any_of(change->ops.begin(), change->ops.end(), [](const Json& op) {
                const auto name = op.value("op", std::string());
                return name == "add_page" || name == "duplicate_page";
            });
            if (creates_page && change->ops.size() > 1 && current->pages.size() != change->before->pages.size()) {
                // A later op can reference a page created inside this same batch.
                // Its number did not exist in before and cannot be checked below.
                throw core::ApplyError("ページ構成が変わったため、一括操作の対象を変更せず保持しました。", "rebase_conflict");
            }
            for (const auto& original : change->before->pages) {
                const auto at_number = std::find_if(current->pages.begin(), current->pages.end(),
                    [&](const core::PagePtr& page) { return page->index == original->index; });
                if (at_number != current->pages.end() && (*at_number)->id != original->id) {
                    throw core::ApplyError("ページの番号とIDの対応が変わったため、未保存の操作を保持しました。", "rebase_conflict");
                }
            }
            core::ScopedIdScript script(change->ids);
            core::ApplyResult result = bus.apply(*current, change->ops, core::Actor(actor_));
            auto again = std::make_shared<Change>();
            again->id = new_change_id();
            again->ops = change->ops;
            again->journal_ops = result.journal_ops;
            again->ids = script.taken();
            again->before = current;
            again->after = std::make_shared<const core::Document>(std::move(result.doc));
            again->held = replaced_precise_bytes(*again->before, *again->after);
            current = again->after;
            replayed.push_back(again);
            keep.push_back(Action{Action::Kind::Edit, again, core::new_txn_id(), ++next_seq_, false});
        } catch (const core::Error& error) {
            conflicts_found << QString::fromUtf8(error.what());
        }
    }
    if (!conflicts_found.isEmpty()) {
        save_wanted_ = false;
        retry_timer_.stop();
        after_failure(QStringLiteral("rebase_conflict"),
            QStringLiteral("他の変更と競合したため、未保存の原稿を保持しています。"));
        emit conflicts(conflicts_found);
        return;
    }
    const DocPtr before = doc_;
    doc_ = current;
    done_ = std::move(replayed);
    undone_.clear();
    queue_ = std::move(keep);
    base_revision_ = revision;
    disk_undo_ = undo_depth;
    disk_redo_ = redo_depth;
    read_only_ = doc_->read_only_reason;
    failed_ = false;
    failure_.clear();
    failure_code_.clear();
    ++generation_;
    touch(BookChange{BookChange::Why::Rebase, Json::array(), before, doc_});
    if (!conflicts_found.isEmpty()) emit conflicts(conflicts_found);
}

void Session::check_outside() {
    // (while pages are read, the rest of the read sees the book as it is on disk)
    if (!path_ || job_running_ || rebasing_ || saving_as_ || loading_ || discarded_) return;
    Job job;
    job.kind = Job::Kind::Probe;
    job.dir = *path_;
    run(std::move(job));
}

void Session::save_as(const fs::path& dir) {
    if (saving_as_ || discarded_) return;
    if (loading_) {
        if (read_failed_) {
            emit savedAs(false, QString::fromStdString(read_only_));
            return;
        }
        // (written when the rest of the book is read: a book missing pages is never written)
        save_as_after_reading_ = dir;
        emit statusChanged();
        return;
    }
    if (job_running_ || rebasing_) {
        emit savedAs(false, QStringLiteral("現在の保存・再照合が完了してから、別名保存を再試行してください。"));
        return;
    }
    auto copy = std::make_shared<core::Document>(*doc_);
    copy->book_id = core::new_book_id();  // (a copy is another book: schema-v4 §2)
    copy->revision = 0;
    copy->read_only_reason.clear();
    if (!read_only_.empty()) {
        // (a book open read-only for want of something can be saved elsewhere only when nothing would be lost)
        emit savedAs(false, qs(read_only_));
        return;
    }
    saving_as_ = dir;
    save_as_ticket_ = ++next_ticket_;
    before_save_as_.clear();
    for (const Action& action : queue_) before_save_as_.push_back(action.seq);
    idle_timer_.stop();
    longest_timer_.stop();
    Job job;
    job.kind = Job::Kind::SaveAs;
    job.dir = dir;
    job.actor = actor_;
    job.lock_wait = options_.lock_wait;
    job.doc = copy;
    job.ticket = save_as_ticket_;
    run(std::move(job));
    emit statusChanged();
}

void Session::discard() {
    discarded_ = true;
    idle_timer_.stop();
    longest_timer_.stop();
    retry_timer_.stop();
    queue_.clear();
    if ((recovery_written_ || recovery_job_) && doc_ && !doc_->book_id.empty()) {
        Job remove;
        remove.kind = Job::Kind::DeleteRecovery;
        remove.root = options_.recovery_root.empty() ? recovery_root() : options_.recovery_root;
        remove.book_id = doc_->book_id;
        run(std::move(remove));
    }
    recovery_written_ = false;
    emit statusChanged();
}

void Session::adopt_recovery() {
    if (!recovery_offer_) return;
    if (!read_only_.empty()) throw core::Error("read_only", read_only_);
    const RecoveryPoint point = *recovery_offer_;
    const Json payload = core::parse_python_json(storage::read_file(point.folder / "state.json"));
    storage::LoadResult loaded = storage::load_document_payload(payload, point.folder);
    if (!loaded.document.read_only_reason.empty()) throw core::Error("read_only", loaded.document.read_only_reason);
    core::Document doc = std::move(loaded.document);
    doc.revision = doc_->revision;
    doc.book_id = doc_->book_id;
    auto change = std::make_shared<Change>();
    change->id = new_change_id();
    change->ops = Json::array({Json::object({{"op", "recover"}})});
    change->journal_ops = Json::array();
    change->recover = true;
    change->before = doc_;
    change->after = std::make_shared<const core::Document>(std::move(doc));
    recovery_offer_.reset();
    doc_ = change->after;
    // (a recovery is not an undoable edit in the journal: the history starts again from it)
    done_.clear();
    undone_.clear();
    done_.push_back(change);
    queue_.push_back(Action{Action::Kind::Edit, change, core::new_txn_id(), ++next_seq_, false});
    ++generation_;
    touch(BookChange{BookChange::Why::Recover, Json::array(), change->before, change->after});
    save_now();
}

void Session::decline_recovery() { recovery_offer_.reset(); }

bool Session::wait_idle(std::chrono::milliseconds timeout) {
    QElapsedTimer clock;
    clock.start();
    while (job_running_ || rebasing_ || saving_as_ || recovery_job_ || reading_rest_ || worker_jobs_ > 0) {
        if (clock.elapsed() >= timeout.count()) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

bool Session::wait_saved(std::chrono::milliseconds timeout) {
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        // (pages not read and no read of them under way — it failed, or was not asked for: nothing will be written)
        if (loading_ && !reading_rest_ && !read_retry_pending_) return false;
        if (!job_running_ && !rebasing_ && !saving_as_ && !loading_) {
            if (queue_.empty()) return path_.has_value() && status().kind == SaveKind::Saved;
            if (failed_ || !path_ || discarded_) return false;
            save_now();
        }
        if (clock.elapsed() >= timeout.count()) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

SaveStatus Session::status() const {
    SaveStatus s;
    s.target = path_;
    s.last_saved = last_saved_;
    s.last_revision = last_revision_;
    const bool waiting_now = std::any_of(queue_.begin(), queue_.end(), [](const Action& a) { return !a.in_flight; });
    if (job_running_ || rebasing_ || saving_as_) {
        s.kind = SaveKind::Saving;
        s.newer_waiting = waiting_now;
        if (failed_) {
            s.retrying = true;
            s.reason = failure_;
            s.code = failure_code_;
        }
        return s;
    }
    if (!path_) {
        // a book never given a folder: only in memory (or in a recovery copy)
        s.kind = recovery_failed_ ? SaveKind::Failed :
            (recovery_written_ && recovery_generation_ == generation_ ? SaveKind::RecoveryOnly : SaveKind::Dirty);
        if (recovery_failed_) {
            s.reason = recovery_failure_;
            s.code = recovery_failure_code_;
            s.nowhere = true;
        }
        s.recovery_time = recovery_time_;
        s.recovery_path = recovery_path_;
        return s;
    }
    if (failure_code_ == QLatin1String("rebase_conflict")) {
        s.kind = SaveKind::Failed;
        s.reason = failure_;
        s.code = failure_code_;
        s.nowhere = recovery_failed_;
        s.recovery_time = recovery_time_;
        s.recovery_path = recovery_path_;
        return s;
    }
    if (queue_.empty()) {
        s.kind = SaveKind::Saved;
        return s;
    }
    if (failed_) {
        s.reason = failure_;
        s.code = failure_code_;
        s.nowhere = recovery_failed_;
        if (recovery_written_ && recovery_generation_ == generation_) {
            s.kind = SaveKind::RecoveryOnly;
            s.recovery_time = recovery_time_;
            s.recovery_path = recovery_path_;
        } else {
            s.kind = SaveKind::Failed;
            if (recovery_written_) {  // (an older recovery point: the newest changes are not in it)
                s.recovery_time = recovery_time_;
                s.recovery_path = recovery_path_;
            }
        }
        return s;
    }
    s.kind = SaveKind::Dirty;
    return s;
}

}  // namespace genko::app
