// The app's editing session (app/session.hpp), without a window: the five save states and their changes, edits made
// while a save runs, the autosave's timing (a pause of 2 s, at most 10 s), undo and redo in memory and through the
// journal, rebasing on a change made by another writer (and its conflicts), the recovery point when the book cannot be
// written (and when neither can be), a read-only book, ids chosen before a change is applied, and save-as.

#include <QtTest>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "app/session.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "storage/fault.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using genko::app::SaveKind;
using genko::app::Session;
using genko::core::Json;

namespace {

fs::path path_of(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }

// A new book on disk (as `genko new` writes it): revision 1.
void make_book(const fs::path& dir, int pages = 2) {
    genko::core::Document doc = genko::core::new_episode("試し", genko::core::Num(1), pages, genko::core::PageSpec::a4_mono());
    genko::storage::ProjectLock lock(dir, "genko");
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = "genko";
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
}

std::size_t ink_strokes(const genko::core::Document& doc, std::size_t page = 0) {
    for (const auto& layer : doc.page(page).layers) {
        if (layer.role == genko::core::LayerRole::Ink) return layer.stroke_count();
    }
    return 0;
}

std::size_t ink_strokes_on_disk(const fs::path& dir, std::size_t page = 0) {
    return ink_strokes(genko::storage::load_document(dir).document, page);
}

std::int64_t disk_revision(const fs::path& dir) { return genko::storage::read_disk_state(dir).revision; }

Json stroke_op(double y = 30.0, int page = 1) {
    return Json::array({Json::object({{"op", "add_stroke"},
                                      {"page", page},
                                      {"layer", "ink"},
                                      {"points", Json::array({Json::array({20.0, y, 0.7}), Json::array({60.0, y + 5, 0.7})})},
                                      {"stabilize", 0}})});
}

Session::Options quick(const fs::path& recovery) {
    Session::Options options;
    options.actor = "human:tester";
    options.idle = 50ms;
    options.longest = 400ms;
    options.lock_wait = 3000ms;
    options.recovery_root = recovery;
    return options;
}

bool wait_for(const std::function<bool()>& done, int ms = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

// Every path and byte, including the mutex record. Only its normal release
// timestamp is volatile; a retained lock or changed actor remains a failure.
std::map<fs::path, std::string> book_files(const fs::path& root) {
    std::map<fs::path, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto path = fs::relative(entry.path(), root);
        if (entry.is_directory()) {
            files.emplace(path, "<directory>");
        } else if (entry.is_regular_file()) {
            auto bytes = genko::storage::read_file(entry.path());
            if (path == fs::path("project.lock")) {
                Json lock = genko::core::parse_python_json(bytes);
                if (lock.value("released", false) != true) throw genko::core::Error("lock", "project lock was not released");
                lock.erase("released_at");
                bytes = lock.dump();
            }
            files.emplace(path, std::move(bytes));
        } else {
            throw genko::core::Error("path", "unexpected path type in saved book");
        }
    }
    return files;
}

// Fault injection for this process while it lives (the build must have it).
struct Fault {
    explicit Fault(const char* spec) { genko::storage::fault::set_for_testing(spec); }
    ~Fault() { genko::storage::fault::set_for_testing(""); }
};

}  // namespace

class TestSession : public QObject {
    Q_OBJECT

private slots:
    void defaultsAreTheSpecsTimes() {
        const Session::Options options;
        QCOMPARE(options.idle, 2000ms);
        QCOMPARE(options.longest, 10000ms);
    }

    void anEditIsSavedAfterThePause() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        auto session = Session::open(book, quick(path_of(tmp.filePath("recovery"))));
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(session->base_revision(), std::int64_t{1});
        session->apply(stroke_op());
        QCOMPARE(session->status().kind, SaveKind::Dirty);
        QCOMPARE(ink_strokes(session->document()), std::size_t{1});
        QVERIFY(wait_for([&] { return session->status().kind == SaveKind::Saved; }));
        QCOMPARE(disk_revision(book), std::int64_t{2});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        QCOMPARE(session->status().last_revision, std::int64_t{2});
        QVERIFY(session->status().last_saved.isValid());
        // one transaction per change: the journal's undo stack has the book's creation and this edit
        QCOMPARE(genko::storage::journal::stacks(book).undo.size(), std::size_t{2});
    }

    void savingThenSaved() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        QSignalSpy states(session.get(), &Session::statusChanged);
        session->apply(stroke_op());
        QCOMPARE(session->status().kind, SaveKind::Dirty);
        // the book held by another writer: the save waits for its lock (it is running, not saved)
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->save_now();
        QCOMPARE(session->status().kind, SaveKind::Saving);
        other->release();
        QVERIFY(session->wait_idle(10000ms));
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QVERIFY(states.count() >= 3);
    }

    void anEditMadeWhileSavingStaysDirty() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30));
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->save_now();  // (waits for the lock: the first edit is being saved)
        QVERIFY(session->job_running());
        session->apply(stroke_op(60));  // a newer generation, made while the older one is saved
        QCOMPARE(session->status().kind, SaveKind::Saving);
        QVERIFY(session->status().newer_waiting);
        other->release();
        QVERIFY(wait_for([&] { return !session->job_running(); }));
        // the save that finished was the first edit's: the second is still not on disk
        QCOMPARE(session->status().kind, SaveKind::Dirty);
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        QCOMPARE(session->waiting(), std::size_t{1});
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{2});
        QCOMPARE(disk_revision(book), std::int64_t{3});
    }

    void theAutosaveWaitsForAPauseAndAtMostTenSeconds() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options;  // (the real times: 2 s and 10 s)
        options.actor = "human:tester";
        options.recovery_root = path_of(tmp.filePath("recovery"));
        auto session = Session::open(book, options);
        // a single change: saved about 2 s later
        QElapsedTimer clock;
        clock.start();
        session->apply(stroke_op(20));
        QVERIFY(wait_for([&] { return session->job_running() || session->status().kind == SaveKind::Saved; }, 6000));
        const qint64 first = clock.elapsed();
        QVERIFY2(first >= 1900 && first <= 3000, qPrintable(QString::number(first)));
        QVERIFY(session->wait_idle(10000ms));
        // changes every 0.4 s without a pause: the first save begins 10 s after the first change (when it begins is the
        // rule; how long the writing takes depends on the machine)
        const QDateTime before = session->status().last_saved;
        clock.restart();
        qint64 saved = -1;
        for (int i = 0; i < 32 && saved < 0; ++i) {
            session->apply(stroke_op(30 + i));
            QElapsedTimer step;
            step.start();
            while (step.elapsed() < 400) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                if (saved < 0 && (session->job_running() || session->status().last_saved != before)) saved = clock.elapsed();
                QThread::msleep(2);
            }
        }
        QVERIFY2(saved >= 9800 && saved <= 10600, qPrintable(QString::number(saved)));
        QVERIFY(session->wait_saved(20000ms));
    }

    void undoInMemoryThenThroughTheJournal() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30));
        session->apply(stroke_op(60));
        session->undo();  // not saved: in memory, nothing for the disk
        QCOMPARE(ink_strokes(session->document()), std::size_t{1});
        QCOMPARE(session->waiting(), std::size_t{1});
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        QCOMPARE(disk_revision(book), std::int64_t{2});
        // the saved change: through the journal (a new revision)
        QVERIFY(session->can_undo());
        session->undo();
        QCOMPARE(ink_strokes(session->document()), std::size_t{0});
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
        QCOMPARE(disk_revision(book), std::int64_t{3});
        session->redo();
        QCOMPARE(ink_strokes(session->document()), std::size_t{1});
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        QCOMPARE(disk_revision(book), std::int64_t{4});
        const auto stacks = genko::storage::journal::stacks(book);
        QCOMPARE(stacks.undo.size(), std::size_t{2});
        QCOMPARE(stacks.redo.size(), std::size_t{0});
    }

    void undoWhileTheChangeIsBeingSaved() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30));
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->save_now();
        QVERIFY(session->job_running());
        session->undo();  // its save is running: the undo goes to the journal after it
        QCOMPARE(ink_strokes(session->document()), std::size_t{0});
        other->release();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
        QCOMPARE(disk_revision(book), std::int64_t{3});  // (saved, then undone: a new revision each)
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
    }

    void anUndoOfABookOpenedAgainGoesThroughTheJournal() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        {
            auto first = Session::open(book, quick(path_of(tmp.filePath("recovery"))));
            first->apply(stroke_op(30));
            QVERIFY(first->wait_saved(10000ms));
        }
        auto session = Session::open(book, quick(path_of(tmp.filePath("recovery"))));
        QCOMPARE(ink_strokes(session->document()), std::size_t{1});
        QVERIFY(session->can_undo());
        QSignalSpy changes(session.get(), &Session::changed);
        session->undo();  // (read again from the disk when the journal has undone it)
        QVERIFY(wait_for([&] { return ink_strokes(session->document()) == 0 && !session->job_running(); }));
        QVERIFY(changes.count() >= 1);
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    void anotherWritersChangeIsKeptAndConflictsAreReported() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book, 3);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30, 1));
        session->apply(stroke_op(30, 3));  // (page 3 goes away in the other change: this one cannot be applied)
        // another writer saves the book meanwhile: page 2 gets a note, page 3 is deleted
        {
            genko::storage::ProjectLock lock(book, "ai:other");
            lock.try_acquire();
            const auto loaded = genko::storage::load_document(book);
            const genko::core::CommandBus bus;
            const Json ops = Json::array({Json::object({{"op", "set_note"}, {"page", 2}, {"note", "外の変更"}}),
                                          Json::object({{"op", "delete_page"}, {"page", 3}})});
            const auto result = bus.apply(loaded.document, ops, genko::core::Actor("ai:other"));
            genko::storage::SaveRequest request;
            request.actor = "ai:other";
            request.base_revision = loaded.document.revision;
            request.ops = result.journal_ops;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        QSignalSpy conflicts(session.get(), &Session::conflicts);
        session->save_now();
        QVERIFY(wait_for([&] { return conflicts.count() == 1; }));
        QVERIFY(session->wait_idle(5s));
        QVERIFY(session->unsaved());
        QCOMPARE(session->document().pages.size(), std::size_t{3});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 2), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
        // Only an explicit undo removes the conflicting local change.
        session->undo();
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        QCOMPARE(conflicts.count(), 1);
        const auto disk = genko::storage::load_document(book).document;
        QCOMPARE(disk.pages.size(), std::size_t{2});
        QCOMPARE(disk.page(1).note, std::string("外の変更"));
        QCOMPARE(ink_strokes(disk, 0), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
    }

    void rebaseNeverRetargetsPagesCreatedInsideABatch_data() {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("new-page-note") << QStringLiteral("set_note");
        QTest::newRow("new-page-delete") << QStringLiteral("delete_page");
        QTest::newRow("new-page-duplicate") << QStringLiteral("duplicate_page");
        QTest::newRow("new-page-stroke") << QStringLiteral("add_stroke");
    }

    void normalCreatedPageBatch_data() { rebaseNeverRetargetsPagesCreatedInsideABatch_data(); }

    void normalCreatedPageBatch() {
        QFETCH(QString, suffix);
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        make_book(book, 3);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        Json last = Json::object({{"op", suffix.toStdString()}, {"page", 4}});
        if (suffix == QStringLiteral("set_note")) last["note"] = "新しい自分のページ";
        if (suffix == QStringLiteral("add_stroke")) last = stroke_op(30, 4).front();
        session->apply(Json::array({Json::object({{"op", "add_page"}}), last}));
        const auto local = session->snapshot();
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        const auto disk = genko::storage::load_document(book).document;
        QCOMPARE(disk.pages.size(), local->pages.size());
        for (std::size_t page = 0; page < disk.pages.size(); ++page) {
            QCOMPARE(disk.page(page).id, local->page(page).id);
            QCOMPARE(disk.page(page).note, local->page(page).note);
            QCOMPARE(ink_strokes(disk, page), ink_strokes(*local, page));
        }
    }

    void rebaseNeverRetargetsPagesCreatedInsideABatch() {
        QFETCH(QString, suffix);
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        make_book(book, 3);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        Json last = Json::object({{"op", suffix.toStdString()}, {"page", 4}});
        if (suffix == QStringLiteral("set_note")) last["note"] = "新しい自分のページ";
        if (suffix == QStringLiteral("add_stroke")) last = stroke_op(30, 4).front();
        session->apply(Json::array({Json::object({{"op", "add_page"}}), last}));
        const auto local = session->snapshot();
        const auto revision = session->base_revision();
        const auto waiting = session->waiting();
        {
            genko::storage::ProjectLock lock(book, "human:test");
            lock.try_acquire();
            const auto result = genko::core::CommandBus().apply(genko::storage::load_document(book).document,
                Json::array({Json::object({{"op", "add_page"}})}), genko::core::Actor("human:test"));
            genko::storage::SaveRequest request;
            request.actor = "human:test";
            request.base_revision = genko::storage::load_document(book).document.revision;
            request.ops = result.journal_ops;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        {
            genko::storage::ProjectLock lock(book, session->actor());
            lock.try_acquire();
        }
        const auto disk = book_files(book);
        const auto other = genko::storage::load_document(book).document.pages.back()->id;
        session->save_now();
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(session->snapshot(), local);
        QCOMPARE(session->base_revision(), revision);
        QCOMPARE(session->waiting(), waiting);
        QCOMPARE(session->status().kind, SaveKind::Failed);
        QVERIFY(!session->wait_saved(100ms));
        QVERIFY(book_files(book) == disk);
        QCOMPARE(genko::storage::load_document(book).document.pages.back()->id, other);
        const auto copy = path_of(tmp.filePath("my-copy.genko"));
        QSignalSpy copied(session.get(), &Session::savedAs);
        session->save_as(copy);
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(copied.count(), 1);
        QVERIFY(copied.front().at(0).toBool());
        const auto copied_doc = genko::storage::load_document(copy).document;
        QCOMPARE(copied_doc.pages.size(), local->pages.size());
        QVERIFY(copied_doc.book_id != local->book_id);
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QTemporaryDir oracle;
        const auto oracle_book = path_of(oracle.path()) / "book.genko";
        auto expected_doc = *local;
        expected_doc.book_id = copied_doc.book_id;
        expected_doc.revision = 0;
        {
            genko::storage::ProjectLock lock(oracle_book, session->actor());
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = session->actor();
            request.base_revision = 0;
            request.ops = Json::array();
            genko::storage::Saver(lock).save(expected_doc, request);
        }
        const auto expected_payload = genko::core::parse_python_json(genko::storage::read_file(oracle_book / "project.json"));
        const auto actual_payload = genko::core::parse_python_json(genko::storage::read_file(copy / "project.json"));
        QVERIFY(expected_payload == actual_payload);
        const auto assets = [](const fs::path& directory) {
            return fs::exists(directory) ? book_files(directory) : std::map<fs::path, std::string>{};
        };
        // Include the before/after journal-state assets created by a real save,
        // not just the current project's pictures/strokes from the serializer.
        QVERIFY(assets(oracle_book / "assets") == assets(copy / "assets"));
        QVERIFY(book_files(book) == disk);
    }

    void pendingRecoverySurvivesSaveAsOutcome_data() {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("save-as-refused") << QStringLiteral("fail");
        QTest::newRow("save-as-saved") << QStringLiteral("success");
        QTest::newRow("explicit-discard") << QStringLiteral("discard");
    }

    void pendingRecoverySurvivesSaveAsOutcome() {
        QFETCH(QString, outcome);
        QTemporaryDir tmp;
        const auto recovery = path_of(tmp.filePath("recovery"));
        const auto target = path_of(tmp.filePath("copy.genko"));
        Session::Options options;  // Keep the real default 2 s / 10 s deadlines.
        options.actor = "human:tester";
        options.recovery_root = recovery;
        Session session(genko::core::new_episode("無題", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono()), std::nullopt, options);
        if (outcome == QStringLiteral("fail")) make_book(target);
        const auto disk = fs::exists(target) ? book_files(target) : std::map<fs::path, std::string>{};
        session.apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "A"}})}));
        session.write_recovery_copy();
        session.apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "C"}})}));
        session.write_recovery_copy();
        const auto local = session.snapshot();
        const auto waiting = session.waiting();
        const auto revision = session.base_revision();
        const auto state = recovery / session.document().book_id / "state.json";
        QSignalSpy copied(&session, &Session::savedAs);
        if (outcome == QStringLiteral("discard")) session.discard();
        else session.save_as(target);
        QVERIFY(session.wait_idle(5s));
        if (outcome == QStringLiteral("discard")) {
            QCOMPARE(copied.count(), 0);
            QCOMPARE(session.waiting(), std::size_t{0});
            QVERIFY(!fs::exists(state));
            QVERIFY(!fs::exists(target));
        } else if (outcome == QStringLiteral("success")) {
            QCOMPARE(copied.count(), 1);
            QVERIFY(copied.front().at(0).toBool());
            QVERIFY(session.wait_saved(5s));
            QCOMPARE(session.status().kind, SaveKind::Saved);
            QCOMPARE(genko::storage::load_document(target).document.page(0).note, std::string("C"));
            QVERIFY(!fs::exists(state));
        } else {
            QCOMPARE(copied.count(), 1);
            QVERIFY(!copied.front().at(0).toBool());
            QVERIFY(!session.path().has_value());
            QCOMPARE(session.snapshot().get(), local.get());
            QCOMPARE(session.waiting(), waiting);
            QCOMPARE(session.base_revision(), revision);
            QVERIFY(book_files(target) == disk);
            QVERIFY(fs::exists(state));
            const auto protected_doc = genko::storage::load_document_payload(
                genko::core::parse_python_json(genko::storage::read_file(state)), state.parent_path()).document;
            QCOMPARE(protected_doc.page(0).note, std::string("C"));
            QCOMPARE(session.status().kind, SaveKind::RecoveryOnly);
        }
    }

    void latestRecoveryDeadlineIsNotLostBehindAnOlderJob() {
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        const auto recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        auto options = quick(recovery);
        options.autosave = true;
        options.idle = 40ms;
        options.longest = 60ms;
        options.lock_wait = 700ms;
        auto session = Session::open(book, options);
        genko::storage::ProjectLock locked(book, "human:other");
        locked.try_acquire();
        const auto change = [&](const char* note) {
            session->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", note}})}));
        };
        const auto contains = [&](const char* note) {
            const auto state = recovery / session->document().book_id / "state.json";
            if (!fs::exists(state)) return false;
            return genko::storage::load_document_payload(
                genko::core::parse_python_json(genko::storage::read_file(state)), state.parent_path()).document.page(0).note == note;
        };
        change("A");
        QVERIFY(wait_for([&] { return contains("A") && session->status().kind == SaveKind::Saving; }, 500));
        change("B");
        QTest::qWait(120);
        change("C");
        QTest::qWait(120);
        QVERIFY(wait_for([&] { return contains("C"); }, 2200));
        QCOMPARE(genko::storage::load_document(book).document.page(0).note, std::string());
        locked.release();
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        QCOMPARE(genko::storage::load_document(book).document.page(0).note, std::string("C"));
    }

    void rebaseNeverRetargetsNumberedPages_data() {
        QTest::addColumn<QString>("local_op");
        QTest::addColumn<QString>("external_op");
        QTest::newRow("delete-after-head-deletion") << QStringLiteral("delete_page") << QStringLiteral("delete_page");
        QTest::newRow("duplicate-after-head-deletion") << QStringLiteral("duplicate_page") << QStringLiteral("delete_page");
        QTest::newRow("note-after-head-insertion") << QStringLiteral("set_note") << QStringLiteral("add_page");
    }
    void rebaseNeverRetargetsNumberedPages() {
        QFETCH(QString, local_op);
        QFETCH(QString, external_op);
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book, 3);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        Json op = Json::object({{"op", local_op.toStdString()}, {"page", 2}, {"note", "未保存"}});
        session->apply(Json::array({op}));
        const auto before = session->snapshot();
        const auto revision = session->base_revision();
        const auto waiting = session->waiting();
        {
            genko::storage::ProjectLock lock(book, "ai:other");
            lock.try_acquire();
            const auto loaded = genko::storage::load_document(book);
            Json other = Json::object({{"op", external_op.toStdString()}, {"page", 1}, {"after", 0}});
            const auto result = genko::core::CommandBus().apply(loaded.document, Json::array({other}), genko::core::Actor("ai:other"));
            genko::storage::SaveRequest request;
            request.actor = "ai:other";
            request.base_revision = loaded.document.revision;
            request.ops = result.journal_ops;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        // Establish this session's normal mutex actor before comparing all paths.
        {
            genko::storage::ProjectLock lock(book, session->actor());
            lock.try_acquire();
        }
        const auto disk_files = book_files(book);
        const auto disk = genko::storage::read_file(book / "project.json");
        QSignalSpy conflicts(session.get(), &Session::conflicts);
        session->save_now();
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(conflicts.count(), 1);
        QCOMPARE(session->snapshot().get(), before.get());
        QCOMPARE(session->base_revision(), revision);
        QCOMPARE(session->waiting(), waiting);
        QCOMPARE(session->status().kind, SaveKind::Failed);
        QVERIFY(!session->wait_saved(100ms));
        QVERIFY(book_files(book) == disk_files);
        QCOMPARE(genko::storage::read_file(book / "project.json"), disk);
        // Explicitly undoing the local change permits adopting the other writer's version.
        session->undo();
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        QVERIFY(book_files(book) == disk_files);
        QCOMPARE(genko::storage::read_file(book / "project.json"), disk);
        QCOMPARE(session->document().pages.size(), external_op == QStringLiteral("add_page") ? std::size_t{4} : std::size_t{2});
    }

    void rebaseConflictPreservesEveryUnsavedChange_data() {
        QTest::addColumn<bool>("undo_all");
        QTest::newRow("undo-conflicting-only") << false;
        QTest::newRow("undo-all") << true;
    }
    void rebaseConflictPreservesEveryUnsavedChange() {
        QFETCH(bool, undo_all);
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book, 3);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30, 1));
        session->apply(stroke_op(30, 3));
        const auto unsaved = session->snapshot();
        {
            genko::storage::ProjectLock lock(book, "ai:other");
            lock.try_acquire();
            const auto loaded = genko::storage::load_document(book);
            const Json ops = Json::array({Json::object({{"op", "delete_page"}, {"page", 3}})});
            const auto result = genko::core::CommandBus().apply(loaded.document, ops, genko::core::Actor("ai:other"));
            genko::storage::SaveRequest request;
            request.actor = "ai:other";
            request.base_revision = loaded.document.revision;
            request.ops = result.journal_ops;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        const std::string project = genko::storage::read_file(book / "project.json");
        QSignalSpy conflicts(session.get(), &Session::conflicts);
        session->save_now();
        QVERIFY(wait_for([&] { return conflicts.count() > 0; }));
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(session->snapshot().get(), unsaved.get());
        QVERIFY(session->unsaved());
        QVERIFY(session->status().kind != SaveKind::Saved);
        QVERIFY(session->can_undo());
        QCOMPARE(genko::storage::read_file(book / "project.json"), project);
        session->undo();
        QCOMPARE(ink_strokes(session->document(), 2), std::size_t{0});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        if (undo_all) {
            session->undo();
            QVERIFY(session->unsaved());
            QCOMPARE(session->status().code, QString("rebase_conflict"));
            const auto restored = session->snapshot();
            const auto revision = session->base_revision();
            QVERIFY(!session->wait_saved(100ms));
            QCOMPARE(session->snapshot().get(), restored.get());
            QCOMPARE(session->base_revision(), revision);
            QCOMPARE(genko::storage::read_file(book / "project.json"), project);
        }
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        QCOMPARE(session->document().pages.size(), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t(undo_all ? 0 : 1));
    }

    void aChangeOnDiskIsReadWhenNothingWaits() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        auto session = Session::open(book, quick(path_of(tmp.filePath("recovery"))));
        {
            genko::storage::ProjectLock lock(book, "ai:other");
            lock.try_acquire();
            const auto loaded = genko::storage::load_document(book);
            const auto result = genko::core::CommandBus().apply(
                loaded.document, Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "やあ"}})}),
                genko::core::Actor("ai:other"));
            genko::storage::SaveRequest request;
            request.actor = "ai:other";
            request.base_revision = loaded.document.revision;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        session->check_outside();
        QVERIFY(wait_for([&] { return session->document().page(0).note == "やあ"; }));
        QCOMPARE(session->base_revision(), std::int64_t{2});
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    void aFailedSaveWritesARecoveryPoint() {
        if (!genko::storage::fault::compiled_in()) QSKIP("this build has no fault injection");
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        Session::Options options = quick(recovery);
        options.autosave = false;
        std::string book_id;
        {
            auto session = Session::open(book, options);
            book_id = session->document().book_id;
            session->apply(stroke_op(30));
            {
                Fault full("assets:fail");  // (a full disk: ENOSPC before anything is written)
                session->save_now();
                QVERIFY(session->wait_idle(10000ms));
                QVERIFY(wait_for([&] { return session->status().kind == SaveKind::RecoveryOnly; }));
            }
            const auto status = session->status();
            QCOMPARE(status.code, QStringLiteral("io"));
            QVERIFY(status.reason.contains(QStringLiteral("No space left")));
            QVERIFY(status.recovery_time.isValid());
            QVERIFY(fs::exists(recovery / book_id / "state.json"));
            QCOMPARE(disk_revision(book), std::int64_t{1});
            QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
            QVERIFY(session->unsaved());
        }
        // opened again: the recovery point is newer than the book, and taking it saves a new revision
        auto again = Session::open(book, options);
        QVERIFY(again->recovery_offer().has_value());
        QCOMPARE(again->recovery_offer()->base_revision, std::int64_t{1});
        again->adopt_recovery();
        QCOMPARE(ink_strokes(again->document()), std::size_t{1});
        QVERIFY(again->wait_saved(10000ms));
        QCOMPARE(disk_revision(book), std::int64_t{2});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        // the book holds it now: the recovery point is gone
        QVERIFY(wait_for([&] { return !fs::exists(recovery / book_id); }));
        const auto lines = genko::storage::journal::read_lines(genko::storage::journal::journal_file(book));
        QCOMPARE(QString::fromStdString(lines.values[lines.values.size() - 2]["action"].get<std::string>()), QStringLiteral("recover"));
    }

    void recoveryCannotOverwriteUnknownFeatures() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        auto options = quick(recovery);
        options.autosave = false;
        {
            auto original = Session::open(book, options);
            original->apply(stroke_op(30));
            original->write_recovery_copy();
            QVERIFY(original->wait_idle(10000ms));
            QVERIFY(fs::exists(recovery / original->document().book_id / "state.json"));
        }
        Json payload = genko::core::parse_python_json(genko::storage::read_file(book / "project.json"));
        payload["features"].push_back("future:recovery-regression");
        genko::storage::write_atomic(book / "project.json", genko::core::dump_python_indent2(payload));
        auto current = Session::open(book, options);
        QVERIFY(!current->read_only_reason().empty());
        QVERIFY(current->recovery_offer().has_value());
        const auto files = [](const fs::path& root) {
            std::map<fs::path, std::string> result;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) result.emplace(fs::relative(entry.path(), root), genko::storage::read_file(entry.path()));
            }
            return result;
        };
        const auto before_book = files(book), before_recovery = files(recovery);
        const auto before_doc = current->snapshot();
        const auto before_generation = current->generation();
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, current->adopt_recovery());
        QVERIFY(current->wait_idle(10000ms));
        QVERIFY(current->snapshot() == before_doc);
        QCOMPARE(current->generation(), before_generation);
        QVERIFY(current->recovery_offer().has_value());
        QVERIFY(files(book) == before_book);
        QVERIFY(files(recovery) == before_recovery);
    }

    void malformedRecoveryKeepsOfferAndMemory() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        auto options = quick(recovery);
        options.autosave = false;
        {
            auto original = Session::open(book, options);
            original->apply(stroke_op(30));
            original->write_recovery_copy();
            QVERIFY(original->wait_idle(10000ms));
        }
        auto current = Session::open(book, options);
        QVERIFY(current->recovery_offer().has_value());
        const auto state = current->recovery_offer()->folder / "state.json";
        genko::storage::write_atomic(state, "{broken json");
        const auto before_doc = current->snapshot();
        const auto before_generation = current->generation();
        const auto before_project = genko::storage::read_file(book / "project.json");
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, current->adopt_recovery());
        QVERIFY(current->snapshot() == before_doc);
        QCOMPARE(current->generation(), before_generation);
        QVERIFY(current->recovery_offer().has_value());
        QCOMPARE(genko::storage::read_file(book / "project.json"), before_project);
        QCOMPARE(genko::storage::read_file(state), std::string("{broken json"));
    }

    void recoveryWithUnknownMeaningIsRefused() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        auto options = quick(recovery);
        options.autosave = false;
        {
            auto original = Session::open(book, options);
            original->apply(stroke_op(30));
            original->write_recovery_copy();
            QVERIFY(original->wait_idle(10000ms));
        }
        auto current = Session::open(book, options);
        QVERIFY(current->recovery_offer().has_value());
        const auto state = current->recovery_offer()->folder / "state.json";
        Json payload = genko::core::parse_python_json(genko::storage::read_file(state));
        payload["features"].push_back("future:recovery-payload");
        genko::storage::write_atomic(state, genko::core::dump_python_indent2(payload));
        const auto files = [](const fs::path& root) {
            std::map<fs::path, std::string> result;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) result.emplace(fs::relative(entry.path(), root), genko::storage::read_file(entry.path()));
            }
            return result;
        };
        const auto before_book = files(book), before_recovery = files(recovery);
        const auto before_doc = current->snapshot();
        const auto before_generation = current->generation();
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, current->adopt_recovery());
        QVERIFY(current->wait_idle(10000ms));
        QVERIFY(current->snapshot() == before_doc);
        QCOMPARE(current->generation(), before_generation);
        QVERIFY(current->recovery_offer().has_value());
        QVERIFY(files(book) == before_book);
        QVERIFY(files(recovery) == before_recovery);
    }

    void aRetryAfterTheDiskIsBackSavesOnce() {
        if (!genko::storage::fault::compiled_in()) QSKIP("this build has no fault injection");
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30));
        {
            Fault late("commit:fail");  // (project.json replaced, the commit line not written: repaired once)
            session->save_now();
            QVERIFY(session->wait_idle(10000ms));
        }
        // whether the repair committed it or not, a retry gives one committed revision for the change
        if (session->status().kind != SaveKind::Saved) {
            session->save_now();
            QVERIFY(session->wait_saved(10000ms));
        }
        QCOMPARE(disk_revision(book), std::int64_t{2});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{1});
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    void neitherTheBookNorTheRecoveryAreaCanBeWritten() {
        if (!genko::storage::fault::compiled_in()) QSKIP("this build has no fault injection");
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        // the recovery area is a file: no folder can be made in it
        QFile blocker(tmp.filePath("recovery"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op(30));
        Fault full("assets:fail");
        session->save_now();
        QVERIFY(wait_for([&] { return !session->job_running() && session->status().nowhere; }));
        QCOMPARE(session->status().kind, SaveKind::Failed);
    }

    void aReadOnlyBookRefusesEdits() {
        genko::core::Document doc = genko::core::new_episode("読むだけ", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc.read_only_reason = "missing assets";
        Session session(std::move(doc), std::nullopt);
        QVERIFY(!session.read_only_reason().empty());
        try {
            session.apply(stroke_op());
            QFAIL("an edit went through");
        } catch (const genko::core::ApplyError& error) {
            QCOMPARE(QString::fromStdString(error.code()), QStringLiteral("read_only"));
        }
        QCOMPARE(ink_strokes(session.document()), std::size_t{0});
        QVERIFY(!session.can_undo());
    }

    void aLineGetsTheIdChosenBeforeIt() {
        genko::core::Document doc = genko::core::new_episode("線", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        Session session(std::move(doc), std::nullopt);
        session.apply(stroke_op(), {"abcdef012345"});
        for (const auto& layer : session.document().page(0).layers) {
            if (layer.role == genko::core::LayerRole::Ink) {
                QCOMPARE(layer.strokes->items.back()->id, std::string("abcdef012345"));
            }
        }
        // (and only inside that apply: the next id is random again)
        QVERIFY(genko::core::new_id() != "abcdef012345");
    }

    void lockedBookStillGetsAnAutomaticRecoveryPoint() {
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        const auto recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        auto options = quick(recovery);
        options.lock_wait = 150ms;
        auto session = Session::open(book, options);
        genko::storage::ProjectLock lock(book, "ai:other");
        lock.try_acquire();
        const auto project = genko::storage::read_file(book / "project.json");
        session->apply(stroke_op());
        QVERIFY(wait_for([&] { return fs::exists(recovery / session->document().book_id / "state.json"); }, 2000));
        const auto state = recovery / session->document().book_id / "state.json";
        auto protected_doc = genko::storage::load_document_payload(
            genko::core::parse_python_json(genko::storage::read_file(state)), state.parent_path()).document;
        QCOMPARE(ink_strokes(protected_doc), std::size_t{1});
        QCOMPARE(genko::storage::read_file(book / "project.json"), project);
        // A second dirty generation must advance the independent checkpoint too.
        session->apply(stroke_op(60));
        QVERIFY(wait_for([&] {
            protected_doc = genko::storage::load_document_payload(
                genko::core::parse_python_json(genko::storage::read_file(state)), state.parent_path()).document;
            return ink_strokes(protected_doc) == 2;
        }, 2000));
        QCOMPARE(genko::storage::read_file(book / "project.json"), project);
        QVERIFY(session->unsaved());
        lock.release();
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{2});
    }

    void untitledRecoveryFailureRemainsVisibleUntilRetry() {
        QTemporaryDir tmp;
        const auto recovery = path_of(tmp.filePath("recovery"));
        genko::storage::write_atomic(recovery, "blocked");
        auto options = quick(recovery);
        options.autosave = false;
        Session session(genko::core::new_episode("無題", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono()), std::nullopt, options);
        session.apply(stroke_op());
        const auto before = session.snapshot();
        session.save_now();
        QVERIFY(session.wait_idle(5s));
        QCOMPARE(session.status().kind, SaveKind::Failed);
        QVERIFY(session.status().nowhere);
        QVERIFY(!session.status().reason.isEmpty());
        QVERIFY(!session.status().code.isEmpty());
        const auto reason = session.status().reason;
        QTest::qWait(100);
        QCOMPARE(session.status().reason, reason);
        QCOMPARE(session.snapshot().get(), before.get());
        QCOMPARE(genko::storage::read_file(recovery), std::string("blocked"));
        fs::remove(recovery);
        session.save_now();
        QVERIFY(session.wait_idle(5s));
        QCOMPARE(session.status().kind, SaveKind::RecoveryOnly);
        QVERIFY(!session.status().nowhere);
        QVERIFY(session.status().reason.isEmpty());
        const auto recovered = genko::storage::load_document_payload(
            genko::core::parse_python_json(genko::storage::read_file(recovery / session.document().book_id / "state.json")),
            recovery / session.document().book_id).document;
        QCOMPARE(ink_strokes(recovered), std::size_t{1});
        QCOMPARE(session.snapshot().get(), before.get());
    }

    void saveAsFreezesHistoryUntilCompletion_data() {
        QTest::addColumn<QString>("attempt");
        QTest::newRow("undo") << QStringLiteral("undo");
        QTest::newRow("redo") << QStringLiteral("redo");
        QTest::newRow("edit") << QStringLiteral("edit");
    }
    void saveAsFreezesHistoryUntilCompletion() {
        QFETCH(QString, attempt);
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        const auto target = path_of(tmp.filePath("copy.genko"));
        make_book(book);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op());
        if (attempt == QStringLiteral("redo")) session->undo();
        const auto before = session->snapshot();
        const auto generation = session->generation();
        const auto waiting = session->waiting();
        genko::storage::ProjectLock lock(target, "ai:other");
        lock.try_acquire();
        QSignalSpy saved(session.get(), &Session::savedAs);
        session->save_as(target);
        QCOMPARE(session->status().kind, SaveKind::Saving);
        try {
            if (attempt == QStringLiteral("undo")) session->undo();
            else if (attempt == QStringLiteral("redo")) session->redo();
            else session->apply(stroke_op(60));
            QFAIL("Save As must not accept edits/history against its frozen snapshot");
        } catch (const genko::core::ApplyError& error) {
            QCOMPARE(QString::fromStdString(error.code()), QStringLiteral("save_as_busy"));
        }
        QCOMPARE(session->snapshot().get(), before.get());
        QCOMPARE(session->generation(), generation);
        QCOMPARE(session->waiting(), waiting);
        QVERIFY(!session->can_undo());
        QVERIFY(!session->can_redo());
        lock.release();
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(saved.count(), 1);
        QVERIFY(saved.at(0).at(0).toBool());
        QCOMPARE(*session->path(), target);
        QCOMPARE(ink_strokes_on_disk(target), ink_strokes(*before));
        QCOMPARE(ink_strokes(session->document()), ink_strokes(*before));
        session->apply(stroke_op(60));
        QVERIFY(session->wait_saved(5s));
        QCOMPARE(ink_strokes_on_disk(target), ink_strokes(*before) + 1);
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});
    }

    void saveAsRefusesAnOldSaveStillInFlight() {
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("book.genko"));
        const auto target = path_of(tmp.filePath("copy.genko"));
        make_book(book);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        session->apply(stroke_op());
        genko::storage::ProjectLock lock(book, "ai:other");
        lock.try_acquire();
        session->save_now();
        QVERIFY(session->job_running());
        const auto before = session->snapshot();
        QSignalSpy saved(session.get(), &Session::savedAs);
        session->save_as(target);
        QCOMPARE(saved.count(), 1);
        QVERIFY(!saved.at(0).at(0).toBool());
        QCOMPARE(*session->path(), book);
        QCOMPARE(session->snapshot().get(), before.get());
        QVERIFY(!fs::exists(target));
        lock.release();
        QVERIFY(session->wait_saved(5s));
        session->save_as(target);
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(saved.count(), 2);
        QVERIFY(saved.at(1).at(0).toBool());
        QCOMPARE(*session->path(), target);
        QCOMPARE(ink_strokes_on_disk(target), std::size_t{1});
    }

    void saveAsMakesANewBookAndGoesOnThere() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path copy = path_of(tmp.filePath("別の場所/写し.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;  // (the change goes to the new place only)
        auto session = Session::open(book, options);
        const std::string first_id = session->document().book_id;
        session->apply(stroke_op(30));
        QSignalSpy saved(session.get(), &Session::savedAs);
        session->save_as(copy);
        QVERIFY(wait_for([&] { return saved.count() == 1; }));
        QCOMPARE(saved.at(0).at(0).toBool(), true);
        QCOMPARE(*session->path(), copy);
        const auto disk = genko::storage::load_document(copy).document;
        QVERIFY(disk.book_id != first_id);
        QCOMPARE(disk.revision, std::int64_t{1});
        QCOMPARE(ink_strokes(disk), std::size_t{1});
        session->apply(stroke_op(60));
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(copy), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book), std::size_t{0});  // (the first place keeps what it had)
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "test_session.moc"
