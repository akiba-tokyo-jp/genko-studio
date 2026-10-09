// The app's editing session (app/session.hpp), without a window: the five save states and their changes, edits made
// while a save runs, the autosave's timing (a pause of 2 s, at most 10 s), undo and redo in memory and through the
// journal (in Python's order, also while the journal's undo is written), rebasing on a change made by another writer
// (and its conflicts), the recovery point when the book cannot be written (and when neither can be), a read-only book,
// ids chosen before a change is applied, and save-as.

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
#include "storage/asset_store.hpp"
#include "storage/fault.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
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

// A saved book with `lines` ink lines on each of its pages (their strokes in .strokes.json assets).
void make_drawn_book(const fs::path& dir, const fs::path& recovery, int pages, int lines) {
    make_book(dir, pages);
    Session::Options options = quick(recovery);
    options.autosave = false;
    auto session = Session::open(dir, options);
    Json ops = Json::array();
    for (int page = 1; page <= pages; ++page) {
        for (int i = 0; i < lines; ++i) ops.push_back(stroke_op(20.0 + i, page)[0]);
    }
    session->apply(ops);
    session->save_now();
    if (!session->wait_saved(10000ms)) throw genko::core::Error("test", "the drawn book was not saved");
}

// Opened with the first page's strokes and pictures only; the rest is read when the test says (read_rest()).
Session::Options first_page_first(const fs::path& recovery) {
    Session::Options options = quick(recovery);
    options.defer_pages = true;
    options.read_rest_now = false;
    return options;
}

// project.json as this book would be written (its assets go to a scratch folder).
Json payload_of(const genko::core::Document& doc, const fs::path& scratch) {
    genko::storage::AssetStore store(scratch);
    return genko::storage::project_payload_v4(doc, store);
}

std::string apply_error_code(Session& session, const Json& ops) {
    try {
        session.apply(ops);
    } catch (const genko::core::ApplyError& error) {
        return error.code();
    }
    return "applied";
}

// A change saved by `actor` in a session of its own (before the session under test is opened).
void saved_by(const fs::path& book, Session::Options options, const std::string& actor, const Json& ops) {
    options.actor = actor;
    auto other = Session::open(book, options);
    other->apply(ops);
    other->save_now();
    if (!other->wait_saved(10000ms)) throw genko::core::Error("test", "the change was not saved");
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

    // A precise colour picture is copied whole by each change to it: past the history's budget, the oldest changes
    // already saved leave memory and are undone and redone through the journal, the book coming back as it was.
    void aLongPreciseHistoryIsUndoneThroughTheJournal() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        options.history_bytes = 1;
        auto session = Session::open(book, options);
        Json pixels = Json::array();
        for (int i = 0; i < 16; ++i) for (const int v : {1000 * i, 2000, 3000 + i, 65535}) pixels.push_back(v);
        session->apply(Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 4}, {"height", 4}, {"precision", "u16"}, {"pixels", pixels}}}));
        const auto precise = [&]() -> std::string {
            for (const auto& layer : session->document().page(0).layers) if (layer.color_raster) return *layer.color_raster;
            return {};
        };
        const std::string id = session->document().page(0).layers.back().id;
        std::vector<std::string> states{precise()};
        for (const double x : {0.0, 105.0}) {
            const Json half{{"poly", Json::array({Json::array({x, 0.0}), Json::array({x + 105, 0.0}), Json::array({x + 105, 297.0}), Json::array({x, 297.0})})}};
            session->apply(Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", id}, {"area", half}}}));
            states.push_back(precise());
            QVERIFY(states.back() != states[states.size() - 2]);
        }
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(session->changes_in_memory(), std::size_t{1});  // (the last one: the others through the journal)
        for (int i = 2; i >= 0; --i) {
            QVERIFY(session->can_undo());
            session->undo();
            QVERIFY(session->wait_saved(10000ms));
            QCOMPARE(precise(), i == 0 ? std::string() : states[std::size_t(i) - 1]);
            const auto on_disk = genko::storage::load_document(book).document;  // (kept: the loop reads its layers)
            std::string saved;
            for (const auto& layer : on_disk.page(0).layers) if (layer.color_raster) saved = *layer.color_raster;
            QCOMPARE(precise(), saved);
        }
        QVERIFY(!session->can_undo());
        for (std::size_t i = 0; i < 3; ++i) {
            QVERIFY(session->can_redo());
            session->redo();
            QVERIFY(session->wait_saved(10000ms));
            QCOMPARE(precise(), states[i]);
        }
    }

    // After the history went to the journal, a change undone before it was saved still comes back with Redo, after
    // an Undo that reached through the journal (and read the book again).
    void aChangeUndoneBeforeItWasSavedIsRedoneAfterTheJournal() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        options.history_bytes = 1;
        auto session = Session::open(book, options);
        Json pixels = Json::array();
        for (int i = 0; i < 16; ++i) for (const int v : {1000 * i, 2000, 3000 + i, 65535}) pixels.push_back(v);
        session->apply(Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 4}, {"height", 4}, {"precision", "u16"}, {"pixels", pixels}}}));
        const std::string id = session->document().page(0).layers.back().id;
        const auto precise = [&]() -> std::string {
            for (const auto& layer : session->document().page(0).layers) if (layer.color_raster) return *layer.color_raster;
            return {};
        };
        const auto cut = [&](double x) {
            const Json part{{"poly", Json::array({Json::array({x, 0.0}), Json::array({x + 70, 0.0}), Json::array({x + 70, 297.0}), Json::array({x, 297.0})})}};
            session->apply(Json::array({Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", id}, {"area", part}}}));
        };
        cut(0);
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(session->changes_in_memory(), std::size_t{1});  // (the raster put: through the journal now)
        const std::string first = precise();
        cut(70);
        const std::string second = precise();
        session->undo();  // not saved yet
        QCOMPARE(precise(), first);
        session->undo();  // the first cut, in memory
        session->undo();  // the raster put: through the journal
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(precise(), std::string());
        for (const std::string& expected : {std::string(), first, second}) {
            if (expected.empty()) {  // (the raster again)
                session->redo();
                QVERIFY(session->wait_saved(10000ms));
                QVERIFY(!precise().empty());
                continue;
            }
            QVERIFY2(session->can_redo(), "a change undone before it was saved must still be redone");
            session->redo();
            QVERIFY(session->wait_saved(10000ms));
            QCOMPARE(precise(), expected);
        }
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

    // A recovery point taken is the book's work again, in the book's folder: its asset_dir (where import_psd reads a
    // relative path from, as Python's Episode.asset_dir) stays the book's, not the recovery point's folder.
    void aRecoveryTakenKeepsTheBooksFolder() {
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
        auto again = Session::open(book, options);
        QVERIFY(again->recovery_offer().has_value());
        const fs::path point = again->recovery_offer()->folder;
        QVERIFY(again->document().asset_dir == book);
        again->adopt_recovery();
        QCOMPARE(ink_strokes(again->document()), std::size_t{1});
        QVERIFY2(again->document().asset_dir == book,
                 genko::core::path_to_utf8(again->document().asset_dir.value_or(fs::path("(none)"))).c_str());
        // a relative path is read from the book's folder
        try {
            again->apply(Json::array({Json::object({{"op", "import_psd"}, {"page", 1}, {"path", "art/nowhere.psd"}})}));
            QFAIL("read a file that is not there");
        } catch (const genko::core::ApplyError& error) {
            const std::string said = error.what();
            QVERIFY2(said.find(genko::core::path_to_utf8(book / "art" / "nowhere.psd")) != std::string::npos, said.c_str());
            QVERIFY2(said.find(genko::core::path_to_utf8(point)) == std::string::npos, said.c_str());
        }
        QVERIFY(again->wait_saved(10000ms));
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

    // --- the pages read when needed (SPEC PERF-01; ACCEPTANCE PERF-A: the first page does not wait for the others) ---

    void theFirstPageIsReadFirstAndTheRestAfter() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 3, 4);
        const Session::Options options = first_page_first(recovery);
        Session::Opened opened = Session::read(book, options);
        QCOMPARE(ink_strokes(opened.doc, 0), std::size_t{4});
        QVERIFY(!opened.doc.is_deferred(0));
        QVERIFY(opened.doc.is_deferred(1));
        QVERIFY(opened.doc.is_deferred(2));
        QCOMPARE(ink_strokes(opened.doc, 1), std::size_t{0});  // (not read yet)
        auto session = Session::from(std::move(opened), book, options);
        QVERIFY(session->loading());
        QVERIFY(session->read_only_reason().empty());
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QVERIFY(session->document().deferred.empty());
        for (std::size_t page = 0; page < 3; ++page) QCOMPARE(ink_strokes(session->document(), page), std::size_t{4});
        // the whole book, as reading it at once gives it
        QCOMPARE(payload_of(session->document(), path_of(tmp.filePath("a"))),
                 payload_of(genko::storage::load_document(book).document, path_of(tmp.filePath("b"))));
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(session->base_revision(), disk_revision(book));
    }

    void editsMadeWhileTheRestIsReadAreKeptAndSavedWhole() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 3, 2);
        const std::int64_t revision = disk_revision(book);
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(90, 1));
        session->apply(stroke_op(95, 1));
        session->undo();  // (in memory: it never reached the disk)
        session->redo();
        session->undo();
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{3});
        QCOMPARE(session->status().kind, SaveKind::Dirty);
        // the autosave's pause (50 ms here) comes and goes: nothing is written, nor a recovery point, while pages are
        // missing
        QTest::qWait(300);
        QCOMPARE(disk_revision(book), revision);
        QCOMPARE(session->status().kind, SaveKind::Dirty);
        QVERIFY(!fs::exists(recovery));
        session->read_rest();
        QVERIFY(session->wait_saved(10000ms));
        QVERIFY(!session->loading());
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{3});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{3});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 2), std::size_t{2});
        QCOMPARE(disk_revision(book), revision + 1);
        // and the change made while the rest was read is undone as any other
        session->undo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 2), std::size_t{2});
    }

    void nothingTouchesThePagesNotReadYet() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        const fs::path copy = path_of(tmp.filePath("写し.genko"));
        make_drawn_book(book, recovery, 3, 1);
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        const auto before = session->snapshot();
        // a line on a page not read yet, a change of the pages, and a batch that reaches one: refused, book as it was
        QCOMPARE(apply_error_code(*session, stroke_op(40, 2)), std::string("page_not_loaded"));
        QCOMPARE(apply_error_code(*session, Json::array({Json::object({{"op", "add_page"}})})), std::string("page_not_loaded"));
        QCOMPARE(apply_error_code(*session, Json::array({stroke_op(40, 1)[0], stroke_op(40, 3)[0]})), std::string("page_not_loaded"));
        QCOMPARE(session->snapshot(), before);
        QCOMPARE(session->status().kind, SaveKind::Saved);
        // the book's saved history waits too
        try {
            session->undo();
            QFAIL("a saved change was undone while pages were missing");
        } catch (const genko::core::ApplyError& error) {
            QCOMPARE(QString::fromStdString(error.code()), QStringLiteral("loading"));
        }
        // a copy elsewhere waits for the rest: nothing is written while pages are missing
        QSignalSpy saved(session.get(), &Session::savedAs);
        session->save_as(copy);
        QTest::qWait(200);
        QCOMPARE(saved.count(), 0);
        QVERIFY(!fs::exists(copy));
        // and a book with pages not read is never written by anything (save, save-as, recovery point, undo, convert)
        try {
            payload_of(session->document(), path_of(tmp.filePath("scratch")));
            QFAIL("a book with pages not read was written");
        } catch (const genko::core::Error& error) {
            QCOMPARE(QString::fromStdString(error.code()), QStringLiteral("partial"));
        }
        session->read_rest();
        QVERIFY(wait_for([&] { return saved.count() == 1; }));
        QCOMPARE(saved.at(0).at(0).toBool(), true);
        QVERIFY(!session->loading());
        QCOMPARE(*session->path(), copy);
        for (std::size_t page = 0; page < 3; ++page) QCOMPARE(ink_strokes_on_disk(copy, page), std::size_t{1});  // (every page)
        QCOMPARE(apply_error_code(*session, stroke_op(40, 2)), std::string("applied"));
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(copy, 1), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});  // (the first place keeps what it had)
    }

    void aProblemFoundOnALaterPageOpensTheBookReadOnly() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        std::string ref;
        const genko::core::Document whole = genko::storage::load_document(book).document;
        for (const auto& layer : whole.page(1).layers) {
            if (layer.role == genko::core::LayerRole::Ink) ref = layer.strokes->blob_ref;
        }
        QVERIFY(!ref.empty());
        QVERIFY(fs::remove(book / genko::core::path_from_utf8(genko::storage::AssetStore::relpath(ref, ".strokes.json"))));
        const auto files = book_files(book);
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        QVERIFY(session->read_only_reason().empty());  // (the missing lines are on the second page)
        session->apply(stroke_op(50, 1));
        QSignalSpy notices(session.get(), &Session::notice);
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QVERIFY(!session->read_only_reason().empty());
        // the book cannot be saved: the line drawn meanwhile is not kept as if it could, and the person is told
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QVERIFY(!session->unsaved());
        QVERIFY(notices.count() >= 1);
        QCOMPARE(notices.last().at(1).toBool(), true);
        QVERIFY(session->wait_idle(5000ms));
        QCOMPARE(book_files(book), files);
    }

    void aBookReadOnlyFromItsFirstPageSaysNothingMore() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        Json project = genko::core::parse_python_json(genko::storage::read_file(book / "project.json"));
        project["features"] = Json::array({"zz.unknown@1"});
        genko::storage::write_atomic(book / "project.json", genko::core::dump_python_indent2(project));
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        QVERIFY(!session->read_only_reason().empty());
        QSignalSpy notices(session.get(), &Session::notice);
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QVERIFY(!session->read_only_reason().empty());
        QCOMPARE(notices.count(), 0);  // (it said so when it opened: no "a later page" warning)
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
    }

    void aReadOfTheRestThatFailsIsTriedAgain() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path away = path_of(tmp.filePath("away.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        Session::Options options = first_page_first(recovery);
        options.read_retry = 500ms;
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(50, 1));
        // the book's folder is out of reach for a moment (a drive taken out, a folder renamed)
        fs::rename(book, away);
        QSignalSpy notices(session.get(), &Session::notice);
        session->read_rest();
        QVERIFY(wait_for([&] { return notices.count() >= 1; }));
        QCOMPARE(notices.at(0).at(1).toBool(), false);  // (said, and tried again)
        QVERIFY(session->loading());
        fs::rename(away, book);
        // (a save asked for while the read waits to be tried again waits for it: it is not a failure)
        QVERIFY(session->wait_saved(10000ms));
        QVERIFY(!session->loading());
        QVERIFY(session->read_only_reason().empty());
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
    }

    void aReadOfTheRestThatKeepsFailingLeavesTheBookReadOnly() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path away = path_of(tmp.filePath("away.genko"));
        const fs::path copy = path_of(tmp.filePath("写し.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        Session::Options options = first_page_first(recovery);
        options.read_retry = 20ms;
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(50, 1));
        QSignalSpy saved(session.get(), &Session::savedAs);
        session->save_as(copy);  // (waits for the rest)
        fs::rename(book, away);
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->read_only_reason().empty(); }));
        // given up: the pages read stay shown, nothing is written anywhere, and nothing waits for ever
        QVERIFY(session->loading());
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.at(0).at(0).toBool(), false);
        session->save_as(copy);
        QCOMPARE(saved.count(), 2);
        QCOMPARE(saved.at(1).at(0).toBool(), false);
        QVERIFY(!fs::exists(copy));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(!session->wait_saved(30000ms));
        QVERIFY(clock.elapsed() < 5000);
        QCOMPARE(apply_error_code(*session, stroke_op(60, 1)), std::string("read_only"));
        fs::rename(away, book);
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
    }

    void aPageNumberThatNamesAnotherPageMeanwhileIsNotWrittenTo() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        const std::string first_id = genko::storage::load_document(book).document.page(0).id;
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(70, 1));  // (on the page it shows: page 1)
        // meanwhile another writer swaps the pages: page 1 is now the other page
        {
            Session::Options other = quick(path_of(tmp.filePath("recovery-other")));
            other.actor = "agent:other";
            auto writer = Session::open(book, other);
            writer->apply(Json::array({Json::object({{"op", "reorder"}, {"order", Json::array({2, 1})}})}));
            QVERIFY(writer->wait_saved(10000ms));
        }
        QCOMPARE(genko::storage::load_document(book).document.page(1).id, first_id);
        QSignalSpy conflicts(session.get(), &Session::conflicts);
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QCOMPARE(conflicts.count(), 1);
        QVERIFY(session->wait_idle(10000ms));
        // the line was not put on the page that now has its number; neither page got it
        const genko::core::Document disk = genko::storage::load_document(book).document;
        QCOMPARE(ink_strokes(disk, 0), std::size_t{1});
        QCOMPARE(ink_strokes(disk, 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
    }

    void aLineUndoneMeanwhileStillReplacesTheJournalsRedo() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        {
            auto first = Session::open(book, quick(recovery));
            first->undo();  // (the journal has something to redo)
            QVERIFY(first->wait_saved(10000ms));
        }
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(80, 1));
        session->undo();  // (in memory: nothing kept, one change to redo)
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QVERIFY(session->can_redo());
        session->redo();
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        // the line drawn replaced what the journal could redo, as any new change does
        QVERIFY(!session->can_redo());
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{0});
    }

    void anUnrelatedReorderMeanwhileKeepsTheChange() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 3, 1);
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        session->apply(stroke_op(70, 1));
        {
            Session::Options other = quick(path_of(tmp.filePath("recovery-other")));
            other.actor = "agent:other";
            auto writer = Session::open(book, other);
            writer->apply(Json::array({Json::object({{"op", "reorder"}, {"order", Json::array({1, 3, 2})}})}));
            QVERIFY(writer->wait_saved(10000ms));
        }
        QSignalSpy conflicts(session.get(), &Session::conflicts);
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(conflicts.count(), 0);  // (page 1 is still the page the line was drawn on)
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 2), std::size_t{1});
    }

    void undoAndRedoAfterTheRestIsRead() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_drawn_book(book, recovery, 2, 1);
        // the lines undone through the journal: the book on disk has something to redo
        {
            auto first = Session::open(book, quick(recovery));
            first->undo();
            QVERIFY(first->wait_saved(10000ms));
        }
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        const Session::Options options = first_page_first(recovery);
        auto session = Session::from(Session::read(book, options), book, options);
        QVERIFY(!session->can_redo());  // (the journal's redo waits for the rest)
        session->apply(stroke_op(80, 1));
        session->apply(stroke_op(85, 1));
        session->undo();  // (in memory)
        session->read_rest();
        QVERIFY(wait_for([&] { return !session->loading(); }));
        // the line undone meanwhile can be redone; the journal's redo is gone with the line drawn since
        QVERIFY(session->can_redo());
        session->redo();
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{2});
        QVERIFY(!session->can_redo());
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{2});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{0});
    }

    // Redo while the journal is still undoing a change saved before this session (Undo, Undo, Redo in quick
    // succession): that change is the next to come back (Python's order: the journal's redo stack, the latest undone
    // first), before the ones undone in memory. While its undo is being written, Redo waits (said, nothing changed);
    // the session never ends up holding a redo the book read again cannot take (a conflict every later save hits).
    void aRedoWhileTheJournalUndoesWaitsItsTurn() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        {
            auto before = Session::open(book, options);
            before->apply(stroke_op(30, 2));
            before->save_now();
            QVERIFY(before->wait_saved(10000ms));
        }
        auto session = Session::open(book, options);
        session->apply(stroke_op(30, 1));
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->undo();  // this session's line (page 1), in memory
        session->undo();  // the line saved before (page 2): through the journal, held up by the other writer's lock
        QVERIFY(session->job_running());
        // what can be redone, the next first: the line saved before, then this session's
        const Session::History waiting = session->history();
        QCOMPARE(waiting.later.size(), std::size_t{2});
        QCOMPARE(waiting.later[0].ops[0].value("page", 0), 2);
        QCOMPARE(waiting.later[1].ops[0].value("page", 0), 1);
        std::string said;
        try {
            session->redo();
        } catch (const genko::core::ApplyError& error) {
            said = error.code();
        }
        other->release();
        QVERIFY(session->wait_idle(10000ms));
        session->save_now();
        QVERIFY(session->wait_idle(10000ms));
        QVERIFY2(session->status().kind == SaveKind::Saved, qPrintable(session->status().code + QStringLiteral(": ") + session->status().reason));
        QCOMPARE(said, std::string("busy"));
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{0});
        // then in Python's order: the line saved before, then this session's
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
        QVERIFY(!session->can_redo());
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    // The journal's undo still waiting (another save runs first): Redo takes it back before it is written — the change
    // saved before this session comes back first, as Python's journal gives it — and the change undone in memory stays
    // undone (not redone in its place, leaving the journal to undo the wrong one).
    void aRedoBeforeTheJournalUndoStartsTakesItBack() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        {
            auto before = Session::open(book, options);
            before->apply(stroke_op(30, 2));
            before->save_now();
            QVERIFY(before->wait_saved(10000ms));
        }
        auto session = Session::open(book, options);
        session->apply(stroke_op(30, 1));
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->save_now();
        QVERIFY(session->job_running());  // (the line's save, held up)
        session->undo();  // this session's line
        session->undo();  // the line saved before: its journal undo waits behind the save
        session->redo();
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        other->release();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
        QVERIFY(!session->can_redo());
    }

    // Two changes saved before this session undone one after the other (a click two rows back in 履歴): both are
    // undone through the journal, the second after the book was read again after the first; and redone again.
    void twoJournalUndosInARow() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        for (const int page : {1, 2}) {
            auto before = Session::open(book, options);
            before->apply(stroke_op(30, page));
            before->save_now();
            QVERIFY(before->wait_saved(10000ms));
        }
        auto session = Session::open(book, options);
        session->undo();
        session->undo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{0});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        QCOMPARE(session->history().later.size(), std::size_t{2});
        QVERIFY(!session->can_undo());
        session->redo();
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
    }

    // Undoing back through another person's change: the journal refuses it, said once, and the undos asked after it
    // are not tried (Python's 履歴 stops at the first refusal); the book and what can be undone stay as they were.
    void undoingThroughAnotherPersonsChangeStopsAtTheFirstRefusal() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        for (const char* who : {"human:tester", "ai:other"}) {
            Session::Options as = options;
            as.actor = who;
            auto before = Session::open(book, as);
            before->apply(stroke_op(30, std::string(who) == "ai:other" ? 2 : 1));
            before->save_now();
            QVERIFY(before->wait_saved(10000ms));
        }
        const std::int64_t revision = disk_revision(book);
        auto session = Session::open(book, options);
        QSignalSpy notices(session.get(), &Session::notice);
        session->undo();
        session->undo();
        QVERIFY(session->wait_idle(10000ms));
        QCOMPARE(notices.count(), 1);
        QCOMPARE(disk_revision(book), revision);
        QCOMPARE(session->waiting(), std::size_t{0});
        QVERIFY(session->can_undo());
        QCOMPARE(session->history().done.size(), std::size_t{2});
        QCOMPARE(session->history().later.size(), std::size_t{0});
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    // A journal undo the journal refuses because the book is not what its change left (here: a recovery point taken
    // after it, which the journal does not stack): said once, and the book goes on being saved — not read again and
    // tried again for ever, every later change waiting behind it.
    void aJournalUndoOfABookChangedOutsideTheJournalIsSaid() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        Session::Options options = quick(recovery);
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 1));
        {
            auto lost = Session::open(book, options);
            lost->apply(stroke_op(40, 2));
            lost->write_recovery_copy();
            QVERIFY(lost->wait_idle(10000ms));
        }
        {
            auto taken = Session::open(book, options);
            QVERIFY(taken->recovery_offer().has_value());
            taken->adopt_recovery();
            QVERIFY(taken->wait_saved(10000ms));
        }
        auto session = Session::open(book, options);
        QSignalSpy notices(session.get(), &Session::notice);
        QSignalSpy changes(session.get(), &Session::changed);
        QVERIFY(session->can_undo());
        session->undo();
        QVERIFY2(session->wait_idle(5000ms), "the journal undo is tried again and again");
        QCOMPARE(notices.count(), 1);
        QVERIFY(notices.front().front().toString().startsWith(QStringLiteral("project.json changed outside the journal")));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        // and a change after it is saved
        session->apply(stroke_op(60, 1));
        session->save_now();
        QVERIFY(session->wait_saved(5000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{2});
    }

    // Undo of an adopted recovery point once it is saved: the journal cannot take it back (it is not on its stacks),
    // so it is refused at once, in the person's words — not tried, ending in a conflict every later save hits.
    void anAdoptedRecoveryPointIsNotUndone() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        const fs::path recovery = path_of(tmp.filePath("recovery"));
        make_book(book);
        Session::Options options = quick(recovery);
        options.autosave = false;
        {
            auto lost = Session::open(book, options);
            lost->apply(stroke_op(40, 2));
            lost->write_recovery_copy();
            QVERIFY(lost->wait_idle(10000ms));
        }
        auto session = Session::open(book, options);
        session->adopt_recovery();
        QVERIFY(session->wait_saved(10000ms));
        std::string said;
        try {
            session->undo();
        } catch (const genko::core::ApplyError& error) {
            said = error.code();
        }
        QVERIFY(session->wait_idle(5000ms));
        QVERIFY2(session->status().kind == SaveKind::Saved, qPrintable(session->status().code));
        QCOMPARE(said, std::string("recover_undo"));
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        session->apply(stroke_op(60, 1));
        session->save_now();
        QVERIFY(session->wait_saved(5000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
    }

    // A journal redo asked while another writer changed the journal (the agent undid its own change, which is now
    // on top of the redo stack): not made on the book as it is now — it would redo the agent's change instead of the
    // person's —, but dropped and said.
    void aJournalRedoIsNotMadeOnABookChangedMeanwhile() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "ai:bot", stroke_op(30, 2));         // Z
        saved_by(book, options, "human:tester", stroke_op(30, 1));   // X
        auto session = Session::open(book, options);
        session->undo();  // X, through the journal
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        genko::storage::restore(book, "ai:bot", false, false);  // the agent undoes Z: the redo stack is [X, Z]
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{0});
        QSignalSpy notices(session.get(), &Session::notice);
        session->redo();  // (before the window's watcher has read the book again)
        QVERIFY(session->wait_idle(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{0});  // (the agent's change stays undone)
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        QCOMPARE(notices.count(), 1);
        QCOMPARE(session->status().kind, SaveKind::Saved);
        QCOMPARE(session->history().later.size(), std::size_t{2});
    }

    // A journal undo given to the disk and failed there (the journal could not be written) may be on the disk all
    // the same: Redo does not take it back as if it never was, it waits; once the disk takes it, Redo brings it back.
    void aJournalUndoThatReachedTheDiskIsNotTakenBack() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 1));
        auto session = Session::open(book, options);
        const fs::path journal = genko::storage::journal::journal_file(book);
        fs::permissions(journal, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        session->undo();
        QVERIFY(session->wait_idle(10000ms));
        fs::permissions(journal, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
        QVERIFY(session->unsaved());  // (failed: the recovery copy holds the change)
        std::string said;
        try {
            session->redo();
        } catch (const genko::core::ApplyError& error) {
            said = error.code();
        }
        QCOMPARE(said, std::string("busy"));
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        session->redo();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{1});
    }

    // A change made while a journal undo is written ends the journal's redo (as Python's journal, where the change is
    // written first): read again after the undo, nothing can be redone from the journal.
    void aChangeMadeWhileTheJournalUndoesEndsItsRedo() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 1));  // X
        auto session = Session::open(book, options);
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->undo();  // X, through the journal: held up
        QVERIFY(session->job_running());
        session->apply(stroke_op(50, 2));  // E
        other->release();
        QVERIFY(wait_for([&] { return !session->job_running() && ink_strokes(session->document(), 0) == 0; }));
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        QVERIFY(!session->can_redo());
        QCOMPARE(session->history().later.size(), std::size_t{0});
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        QVERIFY(!session->can_redo());
    }

    // A change made and undone while a journal undo is written, the undo read back after it: Redo brings back that
    // change (as Python's journal, where it was written and undone, ending the journal's redo) — not the journal's.
    void aChangeUndoneWhileTheJournalUndoesIsTheOneRedone() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 1));  // X
        auto session = Session::open(book, options);
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->undo();  // X, through the journal: held up
        session->apply(stroke_op(50, 2));  // E
        session->undo();  // E
        other->release();
        QVERIFY(wait_for([&] { return !session->job_running() && ink_strokes(session->document(), 0) == 0; }));
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{0});
        QVERIFY(session->can_redo());
        session->redo();
        QCOMPARE(ink_strokes(session->document(), 1), std::size_t{1});
        QCOMPARE(ink_strokes(session->document(), 0), std::size_t{0});
        session->save_now();
        QVERIFY(session->wait_saved(10000ms));
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});
        QCOMPARE(ink_strokes_on_disk(book, 0), std::size_t{0});
        QVERIFY(!session->can_redo());
        // and undone again: it alone can be redone (X's redo is gone, as in Python)
        session->undo();
        QVERIFY(session->wait_saved(10000ms));
        const Session::History h = session->history();
        QCOMPARE(h.later.size(), std::size_t{1});
        QCOMPARE(h.later[0].ops[0].value("page", 0), 2);
    }

    // Redo, then Undo while that journal redo is written, the redo refused by the journal: the undo meant to take it
    // back is not made either (it would undo the change below); a redo not given to the disk yet is simply taken back.
    void anUndoOfARefusedJournalRedoIsNotMade() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 2));  // Y
        saved_by(book, options, "human:tester", stroke_op(30, 1));  // X
        auto session = Session::open(book, options);
        session->undo();  // X
        QVERIFY(session->wait_saved(10000ms));
        // (X's state is gone from the book's records: the journal refuses its redo)
        const auto stacks = genko::storage::journal::stacks(book);
        QVERIFY(!stacks.redo.empty() && stacks.redo.back().after);
        QVERIFY(fs::remove(genko::storage::AssetStore(book).path(*stacks.redo.back().after, genko::storage::kStateSuffix)));
        const std::int64_t revision = disk_revision(book);
        QSignalSpy notices(session.get(), &Session::notice);
        auto other = std::make_unique<genko::storage::ProjectLock>(book, "ai:other");
        other->try_acquire();
        session->redo();  // given to the disk, held up
        QVERIFY(session->job_running());
        session->undo();  // to take it back
        other->release();
        QVERIFY(session->wait_idle(10000ms));
        QCOMPARE(notices.count(), 1);
        QCOMPARE(disk_revision(book), revision);
        QCOMPARE(ink_strokes_on_disk(book, 1), std::size_t{1});  // (Y not undone)
        QCOMPARE(session->waiting(), std::size_t{0});
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }

    // A journal redo asked while another job runs (not given to the disk yet), then Undo: the redo is taken back,
    // nothing written for either.
    void anUndoTakesBackAJournalRedoNotWrittenYet() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("book.genko"));
        make_book(book);
        Session::Options options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        saved_by(book, options, "human:tester", stroke_op(30, 1));
        {
            auto first = Session::open(book, options);
            first->undo();
            QVERIFY(first->wait_saved(10000ms));
        }
        auto session = Session::open(book, options);
        QVERIFY(session->can_redo());
        // another writer's note: the session reads the book again, held up by that writer's lock
        genko::storage::ProjectLock lock(book, "ai:other");
        lock.try_acquire();
        {
            const auto loaded = genko::storage::load_document(book);
            const genko::core::CommandBus bus;
            const auto result = bus.apply(loaded.document, Json::array({Json::object({{"op", "set_note"}, {"page", 2}, {"note", "外"}})}),
                                          genko::core::Actor("ai:other"));
            genko::storage::SaveRequest request;
            request.actor = "ai:other";
            request.base_revision = loaded.document.revision;
            request.ops = result.journal_ops;
            genko::storage::Saver(lock).save(result.doc, request);
        }
        session->check_outside();
        QVERIFY(wait_for([&] { return session->status().kind == SaveKind::Saving; }));
        session->redo();
        QCOMPARE(session->waiting(), std::size_t{1});
        session->undo();
        QCOMPARE(session->waiting(), std::size_t{0});
        lock.release();
        QVERIFY(session->wait_idle(10000ms));
        QCOMPARE(session->status().kind, SaveKind::Saved);
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "test_session.moc"
