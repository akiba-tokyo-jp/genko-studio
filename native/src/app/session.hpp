#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// The GUI's editing session (Python's genko/app/session.py), without widgets so it can be tested alone.
//
// The book in memory is the truth while a person works: every change is a batch of ops applied to it at once
// through the CommandBus, as human:<name>. Changes reach the disk on a worker thread (storage::Saver, one transaction
// per change, so Undo takes back exactly one): after a pause in the work (2 s), and at the latest 10 s after the
// first change not yet saved, or when asked. Undo and Redo of a change not saved yet happen in memory; of a saved
// one, through the book's journal (a new transaction). When someone else saved the book in between, the session reads
// it again and applies its own unsaved changes on top (rebase); the ones that no longer apply are reported as
// conflicts instead of overwriting the other change.
//
// The save state (SPEC SAVE-01) is one of five: saved, dirty, saving, failed, recovery copy only. A save that fails
// leaves the changes in memory and writes a recovery point (the book's state and the assets it needs) in the config
// folder's recovery/<book_id>/; when neither can be written the state says so.

namespace genko::storage {
struct LoadCache;
}

namespace genko::app {

using DocPtr = std::shared_ptr<const core::Document>;

enum class SaveKind { Saved, Dirty, Saving, Failed, RecoveryOnly };

struct SaveStatus {
    SaveKind kind = SaveKind::Saved;
    QString reason;          // why the last save failed (the core's words; wording::error gives the person's)
    QString code;            // its error code ("io", "locked", "read_only", …)
    std::optional<std::filesystem::path> target;  // the book's folder (none: never saved anywhere)
    QDateTime last_saved;    // the last save that succeeded (invalid: none in this session)
    std::int64_t last_revision = 0;  // the committed revision it made (or the one the book was opened at)
    QDateTime recovery_time;          // RecoveryOnly: when the recovery point was written
    std::filesystem::path recovery_path;
    bool nowhere = false;    // the book and the recovery area could not be written: the changes are in memory only
    bool newer_waiting = false;  // Saving: changes made after this save started wait for the next one
    bool retrying = false;       // Saving: again, after a save that failed (reason and code: that failure)
};

// A recovery point found in the config folder for a book that is being opened.
struct RecoveryPoint {
    std::filesystem::path folder;  // recovery/<book_id>
    QDateTime written;
    std::int64_t base_revision = 0;  // the book's revision the changes were made on
    std::size_t pages = 0;
    QString title;
    std::string state_ref;  // of the recovered state
};

// What changed (for the windows showing the book).
struct BookChange {
    enum class Why { Edit, Undo, Redo, Rebase, Reload, Recover, SaveAs };
    Why why = Why::Edit;
    core::Json ops;  // Edit, Redo: the ops of the change
    DocPtr before;
    DocPtr after;
};

class Session : public QObject {
    Q_OBJECT

public:
    struct Options {
        std::string actor;                         // empty: default_actor()
        std::chrono::milliseconds idle{2000};      // the pause after a change before it is saved
        std::chrono::milliseconds longest{10000};  // the longest a change waits while the work goes on
        std::chrono::milliseconds lock_wait{1500}; // how long a save waits for project.lock
        std::filesystem::path recovery_root;       // empty: <config>/recovery
        bool autosave = true;
        const core::OpRegistry* registry = nullptr;  // null: render::ops_registry()
        // read(): the strokes and pictures of the first page only, the rest on the worker after from() (SPEC
        // PERF-01, ACCEPTANCE PERF-A: the first page shown and taking the pen without waiting for the others). Until
        // then the book takes ops on that page only, and nothing is written.
        bool defer_pages = false;
        // from(): start reading the rest at once (tests: when read_rest() is called).
        bool read_rest_now = true;
        // a read of the rest that failed is tried again after this (twice, the second wait twice as long)
        std::chrono::milliseconds read_retry{2000};
    };

    // A book read from its folder, not yet a session (read() may run on any thread; the session is made on the thread
    // it will live on).
    struct Opened {
        core::Document doc;
        std::int64_t undo_depth = 0;
        std::int64_t redo_depth = 0;
        std::optional<RecoveryPoint> offer;   // a recovery point newer than the book
        bool recovery_present = false;        // a recovery point of this book is there (removed by the next full save)
        std::filesystem::path recovery_folder;
        // defer_pages: what was read already (the rest of the read takes it instead of reading it again)
        std::shared_ptr<storage::LoadCache> read_so_far;
    };
    // Read a book: its journal repaired first when the lock can be had, then project.json and its assets. Throws
    // core::Error (and storage::UnsupportedProjectVersion) when it cannot be read. A book of an older version, or one
    // the reader reported problems in, opens read-only.
    static Opened read(const std::filesystem::path& dir, const Options& options);
    static std::shared_ptr<Session> from(Opened opened, const std::filesystem::path& dir, Options options);
    // read() and from() on the calling thread.
    static std::shared_ptr<Session> open(const std::filesystem::path& dir, Options options);
    static std::shared_ptr<Session> open(const std::filesystem::path& dir);

    // A book in memory (dir: where it is saved; none for a book not saved yet).
    Session(core::Document doc, std::optional<std::filesystem::path> dir, Options options);
    Session(core::Document doc, std::optional<std::filesystem::path> dir);
    ~Session() override;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const core::Document& document() const { return *doc_; }
    DocPtr snapshot() const { return doc_; }
    const std::optional<std::filesystem::path>& path() const { return path_; }
    const std::string& actor() const { return actor_; }
    std::uint64_t generation() const { return generation_; }
    std::int64_t base_revision() const { return base_revision_; }
    // Why this book cannot be changed (empty: it can).
    const std::string& read_only_reason() const { return read_only_; }
    // Pages of the book are still being read (Options::defer_pages): ops only on the pages read, nothing written.
    bool loading() const { return loading_; }
    // The book was opened with its first page first (Options::defer_pages), whether or not the rest is read now.
    bool read_in_parts() const { return read_in_parts_; }
    // Read the pages not read yet, on the worker; when they are in, the changes made meanwhile are applied to the
    // whole book again (BookChange::Why::Reload) and saved. A no-op when nothing is missing or it has begun.
    void read_rest();

    // --- editing ----------------------------------------------------------------------------------------------
    // Apply ops at once in memory (throws core::ApplyError and leaves the book as it was). `ids`: the ids new_id()
    // gives first while they are applied (a pen line's id chosen before it was drawn). [{"op": "undo"}] is undo().
    core::ApplyResult apply(const core::Json& ops, const std::vector<std::string>& ids = {});
    // Take back the latest change (in memory when it is not saved yet, else through the journal). Throws
    // core::ApplyError("nothing to undo").
    void undo();
    void redo();
    bool can_undo() const;
    bool can_redo() const;

    // --- the disk -------------------------------------------------------------------------------------------
    SaveStatus status() const;
    // Something not durably in the book: dirty, saving, failed or a recovery copy only.
    bool unsaved() const { return status().kind != SaveKind::Saved; }
    // Save what is not saved now (the autosave's wait skipped); a no-op when everything is.
    void save_now();
    // Write the book to a new folder (a copy with a new book id, schema-v4 §2) and go on there. Asynchronous:
    // savedAs(true) when it is written. Changes made meanwhile go to the new place.
    void save_as(const std::filesystem::path& dir);
    // Write a recovery point now (the 復旧用コピーの作成 button).
    void write_recovery_copy();
    // The book on disk may have changed (the window's file watcher): read its revision, and rebase when it moved.
    void check_outside();
    // Throw away the changes not saved (the explicit 保存せずに閉じる): nothing more is written, and the recovery
    // point of these changes is removed.
    void discard();
    // Process events until no job runs and nothing waits to be saved, or the time is up; true when idle and saved.
    bool wait_saved(std::chrono::milliseconds timeout);
    // Process events until no job runs or waits on the worker (saved or not); true when idle.
    bool wait_idle(std::chrono::milliseconds timeout);
    // A recovery point newer than the book, found when it was opened (adopt_recovery() takes it).
    const std::optional<RecoveryPoint>& recovery_offer() const { return recovery_offer_; }
    // Take the recovery point as the book's content: a change that is saved as a new revision ("recover").
    void adopt_recovery();
    void decline_recovery();

    // Tests: how many changes wait to be written (an action each: an edit, an undo or a redo).
    std::size_t waiting() const { return queue_.size(); }
    bool job_running() const { return job_running_; }

signals:
    void changed(const genko::app::BookChange& change);
    void statusChanged();
    void conflicts(const QStringList& messages);
    // A notice for the status line (error: in red).
    void notice(const QString& message, bool error);
    void savedAs(bool ok, const QString& message);

private:
    // One change made in this session: the ops, the book before and after (shared: pages are copied only where the
    // change touched them), and where it stands on disk.
    struct Change {
        std::uint64_t id = 0;
        core::Json ops = core::Json::array();
        core::Json journal_ops = core::Json::array();
        std::vector<std::string> ids;  // the ids new_id() gave while it was applied (a rebase applies it with them again)
        DocPtr before;
        DocPtr after;
        bool recover = false;          // adopting a recovery point (saved as action "recover")
        std::string txn;               // the transaction that first saved it (the journal's entry for it)
        bool on_disk = false;          // that transaction is committed: undo and redo now go through the journal
        bool undone_on_disk = false;   // a journal undo of it is committed (and no redo since)
    };

    // What the disk still has to get, in order.
    struct Action {
        enum class Kind { Edit, Undo, Redo, DiskUndo, DiskRedo };
        Kind kind = Kind::Edit;
        std::shared_ptr<Change> change;  // none for DiskUndo/DiskRedo: a change saved before this session
        std::string txn;                 // this action's own transaction id: a retry after a failure is saved once
        std::uint64_t seq = 0;
        bool in_flight = false;          // part of the job running now
    };

    struct Job;
    struct JobResult;

    // A job on the worker thread (static: it sees only what the job carries).
    static JobResult execute(const Job& job);

    void touch(BookChange change);
    void schedule_save();
    void start_job();
    void run(Job job);
    void job_done(const JobResult& result);
    void simplify_queue();
    void after_failure(const QString& code, const QString& message);
    void start_rebase();
    void rebase_onto(DocPtr fresh, std::int64_t revision, std::int64_t undo_depth, std::int64_t redo_depth);
    void finish_reading(const JobResult& result);
    void request_recovery();
    std::uint64_t new_change_id() { return ++next_change_; }

    Options options_;
    const core::OpRegistry* registry_ = nullptr;
    std::string actor_;
    DocPtr doc_;
    std::optional<std::filesystem::path> path_;
    std::int64_t base_revision_ = 0;
    std::uint64_t generation_ = 0;
    std::string read_only_;
    bool loading_ = false;          // Document::deferred pages wait for the rest of the read
    bool reading_rest_ = false;     // its job is on the worker
    bool read_in_parts_ = false;
    int read_failures_ = 0;
    bool read_failed_ = false;      // the rest could not be read (after the retries): read-only, nothing written
    bool read_retry_pending_ = false;  // a read that failed waits to be tried again
    std::optional<std::filesystem::path> save_as_after_reading_;  // save_as asked while pages were read
    std::shared_ptr<storage::LoadCache> read_so_far_;

    std::vector<std::shared_ptr<Change>> done_;    // this session's changes, oldest first
    std::vector<std::shared_ptr<Change>> undone_;  // the ones undone, next to redo last
    std::deque<Action> queue_;                     // what the disk still has to get, in order
    std::int64_t disk_undo_ = 0;  // saved changes before this session that the journal can undo
    std::int64_t disk_redo_ = 0;

    bool job_running_ = false;
    bool save_wanted_ = false;
    bool failed_ = false;
    bool rebasing_ = false;
    bool discarded_ = false;
    QString failure_code_;
    QString failure_;
    bool recovery_written_ = false;
    bool recovery_failed_ = false;
    QString recovery_failure_;
    QString recovery_failure_code_;
    std::uint64_t recovery_generation_ = 0;
    QDateTime recovery_time_;
    std::filesystem::path recovery_path_;
    QDateTime last_saved_;
    std::int64_t last_revision_ = 0;
    std::optional<RecoveryPoint> recovery_offer_;
    std::optional<std::filesystem::path> saving_as_;
    std::uint64_t next_change_ = 0;

    QTimer idle_timer_;
    QTimer longest_timer_;
    QTimer retry_timer_;
    QThread thread_;
    QObject* worker_ = nullptr;  // lives on thread_: the jobs run there, one at a time, in order
    std::uint64_t next_seq_ = 0;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t save_as_ticket_ = 0;
    std::vector<std::uint64_t> before_save_as_;  // the actions queued when save_as was asked (the old place's)
    bool recovery_job_ = false;
    bool recovery_requested_ = false;
    std::size_t worker_jobs_ = 0;  // jobs given to the worker and not back yet (every kind)
};

// The recovery point of a book in `root` (recovery/<book_id>), if one is there.
std::optional<RecoveryPoint> find_recovery(const std::filesystem::path& root, const std::string& book_id);

}  // namespace genko::app

Q_DECLARE_METATYPE(genko::app::BookChange)
