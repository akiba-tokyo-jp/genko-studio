// Saving through storage::Saver in one process (schema-v4 §4.2–4.4): the five steps and what they leave, the
// revision (monotonic, never reused, checked against the base), a transaction saved once, v1–v3 and read-only
// books refused, big op records as assets, approvals audited; failures injected at each step and retried in the
// same process (AC-SAVE 10: the repair settles the failed save, a write is refused until it can, the retry is saved
// once); Undo and Redo (their rules, the exact state back); GC (what it keeps); doctor (what it reports).

#include <QtTest>

#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "storage/asset_store.hpp"
#include "storage/doctor.hpp"
#include "storage/fault.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
namespace journal = genko::storage::journal;
using genko::core::Json;
using genko::storage::ProjectLock;
using genko::storage::SaveRequest;
using genko::storage::SaveResult;
using genko::storage::Saver;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }
QString to_qstring(const fs::path& path) { return QString::fromStdString(genko::storage::path_to_utf8(path)); }

fs::path make_book(const fs::path& dir, const genko::core::PageSpec& spec = genko::core::PageSpec::a4_mono(), int pages = 2) {
    ProjectLock lock(dir);
    lock.try_acquire();
    SaveRequest request;
    request.ops = Json::array();
    Saver(lock).save(genko::core::new_episode("本", 1, pages, spec), request);
    return dir;
}

// Python's `genko apply`: read under the lock, apply, save as the next revision.
SaveResult edit(const fs::path& dir, const char* ops, const std::string& actor = "genko", const std::string& txn = {},
                std::optional<std::int64_t> base = std::nullopt) {
    ProjectLock lock(dir, actor);
    lock.try_acquire();
    journal::repair(dir);
    const auto loaded = genko::storage::load_document(dir);
    const auto result = genko::core::CommandBus().apply(loaded.document, genko::core::parse_python_json(ops), genko::core::Actor(actor));
    SaveRequest request;
    request.actor = actor;
    request.base_revision = base.value_or(loaded.document.revision);
    request.ops = result.journal_ops;
    request.txn = txn;
    return Saver(lock).save(result.doc, request);
}

genko::core::Document read(const fs::path& dir) { return genko::storage::load_document(dir).document; }

SaveRequest at_revision(std::int64_t base) {
    SaveRequest request;
    request.base_revision = base;
    return request;
}

std::string code_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const genko::core::Error& error) {
        return error.code() + ": " + error.what();
    }
    return "(no error)";
}

std::size_t line_count(const fs::path& dir) { return journal::read_lines(journal::journal_file(dir)).values.size(); }

std::map<std::string, std::string> files(const fs::path& dir) { return genko::test::tree_hashes(to_qstring(dir), false); }

void age(const fs::path& file, std::chrono::hours hours) {
    fs::last_write_time(file, fs::file_time_type::clock::now() - hours);
}

}  // namespace

class TestTransaction : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int books_ = 0;

    fs::path book_path() { return to_path(tmp_.path()) / ("book" + std::to_string(++books_) + ".genko"); }

private slots:
    void cleanup() { genko::storage::fault::set_for_testing(""); }

    void savesInFiveSteps() {
        const fs::path dir = make_book(book_path());
        auto lines = journal::read_lines(journal::journal_file(dir)).values;
        QCOMPARE(lines.size(), std::size_t{2});
        QCOMPARE(lines[0]["kind"], Json("prepare"));
        QCOMPARE(lines[0]["action"], Json("edit"));
        QCOMPARE(lines[0]["rev"], Json(1));
        QCOMPARE(lines[0]["base_rev"], Json(0));
        QVERIFY(lines[0]["before"].is_null());
        QCOMPARE(lines[0]["ops"], Json::array());
        QCOMPARE(lines[1], journal::commit_line(lines[0]["txn"].get<std::string>(), 1));
        const Json project = genko::core::parse_python_json(genko::storage::read_file(dir / "project.json"));
        QCOMPARE(project["revision"], Json(1));
        QCOMPARE(project["version"], Json(4));
        QCOMPARE(project["writer"]["app"], Json("genko-native"));
        QCOMPARE(lines[0]["after"].get<std::string>(), genko::storage::state_ref(project));
        QCOMPARE(lines[0]["project_sha256"].get<std::string>(), genko::storage::sha256_file(dir / "project.json"));
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());

        const SaveResult second = edit(dir, R"([{"op": "set_note", "page": 1, "note": "一"}])", "human:a");
        QCOMPARE(second.revision, std::int64_t{2});
        QVERIFY(core_is_txn(second.txn));
        lines = journal::read_lines(journal::journal_file(dir)).values;
        QCOMPARE(lines[2]["before"], lines[0]["after"]);
        QCOMPARE(lines[2]["base_rev"], Json(1));
        QCOMPARE(lines[2]["actor"], Json("human:a"));
        QCOMPARE(lines[2]["ops"], genko::core::parse_python_json(R"([{"op": "set_note", "page": 1, "note": "一"}])"));
        QVERIFY(!lines[2].contains("audit") && !lines[2].contains("target"));
        QCOMPARE(read(dir).page(0).note, std::string("一"));
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void revisionConflictWritesNothing() {
        const fs::path dir = make_book(book_path());
        edit(dir, R"([{"op": "set_autosave"}])");
        const auto before = files(dir);
        QCOMPARE(code_of([&] { edit(dir, R"([{"op": "set_meta", "title": "x"}])", "genko", {}, 7); }),
                 std::string("revision_conflict: revision conflict: expected 7, found 2"));
        QVERIFY(files(dir) == before);
    }

    void revisionsAreNeverReused() {
        const fs::path dir = make_book(book_path());
        // a prepared revision 2 that was aborted (its project.json never appeared): the next save is revision 3
        const std::string txn = genko::core::new_txn_id();
        Json prepare = journal::read_lines(journal::journal_file(dir)).values[0];
        prepare["txn"] = txn;
        prepare["rev"] = 2;
        prepare["base_rev"] = 1;
        prepare["project_sha256"] = std::string(64, '0');
        journal::append(journal::journal_file(dir), prepare);
        const SaveResult saved = edit(dir, R"([{"op": "set_autosave"}])");
        QCOMPARE(saved.revision, std::int64_t{3});
        const auto lines = journal::read_lines(journal::journal_file(dir)).values;
        QCOMPARE(lines[3], journal::abort_line(txn));  // (the repair before the write)
        QCOMPARE(lines[4]["base_rev"], Json(1));
        QCOMPARE(lines[4]["rev"], Json(3));
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void aTransactionIsSavedOnce() {
        const fs::path dir = make_book(book_path());
        const std::string txn = genko::core::new_txn_id();
        const SaveResult first = edit(dir, R"([{"op": "add_page"}])", "genko", txn);
        const auto after_first = files(dir);
        const SaveResult again = edit(dir, R"([{"op": "add_page"}])", "genko", txn, 1);  // (a retry from revision 1)
        QVERIFY(again.already_committed);
        QCOMPARE(again.revision, first.revision);
        QCOMPARE(again.state_ref, first.state_ref);
        QVERIFY(files(dir) == after_first);
        QCOMPARE(read(dir).pages.size(), std::size_t{3});
        QCOMPARE(code_of([&] { edit(dir, R"([])", "genko", "not-a-txn"); }).substr(0, 6), std::string("value:"));
    }

    void legacyAndReadOnlyBooksAreNotWritten() {
        const QString copy = tmp_.path() + "/v3-copy.genko";
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), copy);
        const auto before = files(to_path(copy));
        ProjectLock lock(to_path(copy));
        lock.try_acquire();
        // (a cut last line in the old journal: even the repair must leave it alone)
        genko::storage::append_durable(to_path(copy) / "studio" / "journal.jsonl", "{\"kind\": \"comm");
        const auto before_cut = files(to_path(copy));
        const auto loaded = genko::storage::load_document(to_path(copy));
        QVERIFY(code_of([&] { Saver(lock).save(loaded.document, at_revision(4)); })
                    .starts_with("needs_migration: this book is in the version 3 format"));
        QVERIFY(code_of([&] { genko::storage::restore(lock, "genko", false, false); }).starts_with("needs_migration:"));
        lock.release();
        QVERIFY(files(to_path(copy)) == before_cut);
        QVERIFY(before_cut != before);

        const fs::path dir = make_book(book_path());
        ProjectLock own(dir);
        own.try_acquire();
        auto doc = read(dir);
        doc.read_only_reason = "this book uses features this build does not support: x@1";
        QVERIFY(code_of([&] { Saver(own).save(doc, at_revision(1)); }).starts_with("read_only:"));
        ProjectLock other(dir);
        doc.read_only_reason.clear();
        QVERIFY(code_of([&] { Saver(other).save(doc, at_revision(1)); }).starts_with("value:"));  // (no lock)
    }

    void bigOpRecordsBecomeAssets() {
        // (no raw strings here: moc cannot read one that ends with a quote)
        const auto set_note = [](const std::string& note) {
            Json op = Json::object();
            op["op"] = "set_note";
            op["page"] = 1;
            op["note"] = note;
            return genko::core::dump_python(Json::array({op}));
        };
        const fs::path dir = make_book(book_path());
        const std::string ops = set_note(std::string(20000, 'n'));
        edit(dir, ops.c_str());
        const Json prepare = journal::read_lines(journal::journal_file(dir)).values[2];
        Json brief = Json::object();
        brief["op"] = "set_note";
        brief["page"] = 1;
        QCOMPARE(prepare["ops"], Json::array({brief}));  // (a brief: the note is 80 characters or more)
        const std::string ref = prepare["ops_asset"].get<std::string>();
        QCOMPARE(genko::storage::AssetStore(dir).get_bytes(ref, ".ops.json").value(),
                 genko::core::dump_canonical(genko::core::parse_python_json(ops)));
        // the rule counts characters, as Python's len(): 15000 × "東" is under 16000 (though 45000 bytes)
        std::string wide;
        for (int i = 0; i < 15000; ++i) wide += "東";
        edit(dir, set_note(wide).c_str());
        QVERIFY(!journal::read_lines(journal::journal_file(dir)).values[4].contains("ops_asset"));
    }

    void approvalsAreAudited() {
        const fs::path dir = make_book(book_path());
        const SaveResult approved = edit(dir, R"([{"op": "name_ok", "page": 1}])", "human:作者");
        QCOMPARE(approved.audit.size(), std::size_t{1});
        const auto entries = journal::audit_entries(dir);
        QCOMPARE(entries.size(), std::size_t{1});
        QCOMPARE(entries[0]["txn"].get<std::string>(), approved.txn);
        QCOMPARE(entries[0]["rev"], Json(2));
        QCOMPARE(entries[0]["actor"], Json("human:作者"));
        const std::string what = "page:" + read(dir).page(0).id + ":name_ok";
        QCOMPARE(entries[0]["changes"], Json::array({Json::object({{"what", what}, {"from", false}, {"to", true}})}));
        edit(dir, R"([{"op": "set_note", "page": 1}])", "human:作者");
        QCOMPARE(journal::audit_entries(dir).size(), std::size_t{1});  // (no change of approvals: no line)
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void failuresAtEachStepRetriedInTheSameProcess_data() {
        QTest::addColumn<QString>("stage");
        for (const char* stage : {"assets", "prepare", "project", "audit", "commit"}) QTest::newRow(stage) << QString(stage);
    }

    void failuresAtEachStepRetriedInTheSameProcess() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        QFETCH(QString, stage);
        const fs::path dir = make_book(book_path());
        const std::string txn = genko::core::new_txn_id();
        genko::storage::fault::set_for_testing((stage + ":fail").toStdString());
        const std::string error = code_of([&] { edit(dir, R"([{"op": "name_ok", "page": 1}])", "human:a", txn, 1); });
        QVERIFY2(error.starts_with("io: [Errno 28] No space left on device:"), error.c_str());
        const bool replaced = stage == "audit" || stage == "commit";
        QCOMPARE(read(dir).page(0).name_ok, replaced);  // (project.json: the old book, or the new one waiting for its commit)
        // while the failure lasts, nothing is written on top of an unsettled book
        const std::size_t lines = line_count(dir);
        const std::string refused = code_of([&] { edit(dir, R"([{"op": "set_autosave"}])", "human:a"); });
        QVERIFY2(refused.starts_with("io:"), refused.c_str());
        if (replaced) QCOMPARE(line_count(dir), lines);  // (the repair cannot write: no prepare either)

        // the failure ends: the retry of the same transaction is saved once
        genko::storage::fault::set_for_testing("");
        const SaveResult retried = edit(dir, R"([{"op": "name_ok", "page": 1}])", "human:a", txn, 1);
        QCOMPARE(retried.already_committed, replaced);  // (the repair committed it: its revision is returned)
        QVERIFY(read(dir).page(0).name_ok);
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
        int commits = 0;
        for (const auto& t : journal::transactions(journal::read_lines(journal::journal_file(dir)))) {
            if (t.txn == txn && t.status == journal::Transaction::Status::committed) {
                ++commits;
                QCOMPARE(t.recovered, replaced);
            }
        }
        QCOMPARE(commits, 1);
        QCOMPARE(journal::audit_entries(dir).size(), std::size_t{1});  // (the approval, once)
        // and the book goes on: Undo of the change, the next revision
        const auto undone = genko::storage::restore(dir, "human:a", false, false);
        QVERIFY(undone.revision > retried.revision);
        QVERIFY(!read(dir).page(0).name_ok);
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void lateFailuresAreSettledAtOnce_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("saved");
        QTest::newRow("assets") << "assets" << false;
        QTest::newRow("prepare") << "prepare" << false;
        QTest::newRow("project") << "project" << true;  // (renamed, then the folder's fsync failed)
        QTest::newRow("audit") << "audit" << true;
        QTest::newRow("commit") << "commit" << true;
    }

    void lateFailuresAreSettledAtOnce() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        QFETCH(QString, stage);
        QFETCH(bool, saved);
        const fs::path dir = make_book(book_path());
        genko::storage::fault::set_for_testing((stage + ":late").toStdString());
        SaveResult result;
        const std::string error = code_of([&] { result = edit(dir, R"([{"op": "name_ok", "page": 1}])", "human:a"); });
        genko::storage::fault::set_for_testing("");
        if (saved) {
            QCOMPARE(error, std::string("(no error)"));
            QVERIFY(result.repaired || stage == "commit");  // (commit:late: the commit line is there; nothing to repair but the report)
            QCOMPARE(result.revision, std::int64_t{2});
        } else {
            QVERIFY2(error.starts_with("io: [Errno 28]"), error.c_str());
        }
        QCOMPARE(read(dir).page(0).name_ok, saved);
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void undoAndRedoRules() {
        const fs::path dir = make_book(book_path());
        QCOMPARE(code_of([&] { genko::storage::restore(dir, "genko", true, false); }), std::string("nothing_to_redo: nothing to redo"));
        QCOMPARE(code_of([&] { genko::storage::restore(dir, "genko", false, false); }),
                 std::string("no_before: the project did not exist before this change"));
        edit(dir, R"([{"op": "set_note", "page": 1, "note": "AI"}])", "ai:x");
        QCOMPARE(code_of([&] { genko::storage::restore(dir, "ai:y", false, false); }),
                 std::string("other_actor: the latest change is by ai:x; use --force to undo it as ai:y"));
        // a change made outside the journal
        std::string text = genko::storage::read_file(dir / "project.json");
        const std::string title = "\"title\": \"本\"";
        text.replace(text.find(title), title.size(), "\"title\": \"外\"");
        genko::storage::write_atomic(dir / "project.json", text);
        QCOMPARE(code_of([&] { genko::storage::restore(dir, "ai:x", false, false); }),
                 std::string("external_change: project.json changed outside the journal; use --force to restore anyway"));
        const auto forced = genko::storage::restore(dir, "ai:y", false, true);
        QCOMPARE(forced.kind, std::string("undo"));
        QCOMPARE(forced.rev, Json(2));
        QCOMPARE(forced.revision, std::int64_t{3});
        QCOMPARE(read(dir).title, std::string("本"));
        QCOMPARE(read(dir).page(0).note, std::string(""));
        // approvals: only a person may undo or redo them
        edit(dir, R"([{"op": "name_ok", "page": 2}])", "human:作者");
        QCOMPARE(code_of([&] { genko::storage::restore(dir, "ai:x", false, true); }).substr(0, 13), std::string("needs_person:"));
        QVERIFY(code_of([&] { genko::storage::restore(dir, "ai:x", false, true); }).ends_with(":name_ok); only a person can do that"));
        genko::storage::restore(dir, "human:作者", false, false);
        QVERIFY(!read(dir).page(1).name_ok);
        QVERIFY(code_of([&] { genko::storage::restore(dir, "ai:x", true, false); }).starts_with("needs_person: this redo changes approvals"));
        genko::storage::restore(dir, "human:作者", true, false);
        QVERIFY(read(dir).page(1).name_ok);
        const auto audit = journal::audit_entries(dir);
        QCOMPARE(audit.size(), std::size_t{3});
        QCOMPARE(audit[1]["via"], Json("undo"));
        QCOMPARE(audit[2]["via"], Json("redo"));
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void undoBringsBackTheExactState() {
        // a new B4 book writes its margins as ints; once read again they are floats (as in Python): Undo must still
        // bring back the first state exactly, and Redo the second
        const fs::path dir = make_book(book_path(), genko::core::PageSpec::b4_comic());
        const Json first = journal::read_lines(journal::journal_file(dir)).values[0];
        QVERIFY(genko::storage::read_file(dir / "project.json").find("\"margins_mm\": [\n      20,") != std::string::npos);
        const SaveResult edited = edit(dir, R"([{"op": "set_note", "page": 1, "note": "x"}])");
        QVERIFY(genko::storage::read_file(dir / "project.json").find("\"margins_mm\": [\n      20.0,") != std::string::npos);
        // undo the creation is refused; undo the edit gives the first state back
        const auto undone = genko::storage::restore(dir, "genko", false, false);
        const Json project = genko::core::parse_python_json(genko::storage::read_file(dir / "project.json"));
        QCOMPARE(genko::storage::state_ref(project), first["after"].get<std::string>());
        QCOMPARE(project["revision"], Json(undone.revision));
        // project.json keeps the writer's order (not the state's sorted keys)
        const auto keys = [](const Json& o) {
            std::vector<std::string> out;
            for (const auto& [k, v] : o.items()) out.push_back(k);
            return out;
        };
        const std::vector<std::string> top = keys(project);
        QCOMPARE(std::vector<std::string>(top.begin(), top.begin() + 7),
                 std::vector<std::string>({"version", "min_reader", "writer", "book_id", "features", "revision", "title"}));
        QCOMPARE(keys(project["pages"][0])[0], std::string("id"));
        QCOMPARE(keys(project["pages"][0])[1], std::string("index"));
        genko::storage::restore(dir, "genko", true, false);
        QCOMPARE(genko::storage::state_ref(genko::core::parse_python_json(genko::storage::read_file(dir / "project.json"))),
                 edited.state_ref);
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void recoverLeavesTheStacks() {
        const fs::path dir = make_book(book_path());
        edit(dir, R"([{"op": "set_note", "page": 1, "note": "a"}])");
        const auto before = journal::stacks(dir);
        ProjectLock lock(dir);
        lock.try_acquire();
        auto doc = read(dir);
        doc.edit_page(0).note = "recovered";
        SaveRequest request;
        request.base_revision = doc.revision;
        request.action = "recover";
        const SaveResult recovered = Saver(lock).save(doc, request);
        QCOMPARE(recovered.revision, std::int64_t{3});
        const auto after = journal::stacks(dir);
        QCOMPARE(after.undo.size(), before.undo.size());
        QCOMPARE(after.undo.back().id, before.undo.back().id);
        QVERIFY(after.problems.empty());
    }

    void gcKeepsWhatTheBookNeeds() {
        const fs::path dir = make_book(book_path());
        for (int i = 0; i < 3; ++i) edit(dir, (R"([{"op": "set_note", "page": 1, "note": "n)" + std::to_string(i) + R"("}])").c_str());
        genko::storage::AssetStore store(dir);
        const std::string orphan_old = store.put_bytes("old orphan", ".png");
        const std::string orphan_young = store.put_bytes("young orphan", ".png");
        const std::string kept_by_autosave = store.put_bytes("autosaved", ".png");
        const std::string kept_by_pending = store.put_bytes("{\"pending\": true, \"ref\": \"" + store.put_bytes("inside", ".png") + "\"}", ".state.json");
        genko::test::write_bytes(to_qstring(dir / "assets" / "readme.txt"), "not an asset");
        genko::test::write_bytes(to_qstring(dir / "studio" / "autosave" / "point.json"), "{\"asset\": \"" + kept_by_autosave + "\"}");
        for (const fs::path& file : store.all_files()) age(file, std::chrono::hours(48));
        age(dir / "assets" / "readme.txt", std::chrono::hours(48));
        age(store.path(orphan_young, ".png"), std::chrono::hours(1));
        // an unfinished transaction whose state is that asset
        Json prepare = journal::read_lines(journal::journal_file(dir)).values[0];
        prepare["txn"] = genko::core::new_txn_id();
        prepare["rev"] = 5;
        prepare["after"] = kept_by_pending;
        journal::append(journal::journal_file(dir), prepare);

        const Json dry = genko::storage::gc(dir, true);
        QCOMPARE(dry["removed"], Json::array({genko::storage::AssetStore::relpath(orphan_old, ".png")}));
        QVERIFY(store.has(orphan_old, ".png"));  // (dry run)
        QCOMPARE(dry["dry_run"], Json(true));
        const Json done = genko::storage::gc(dir, false);  // (a write: the pending transaction is aborted first…)
        QVERIFY(!store.has(orphan_old, ".png"));
        QVERIFY(store.has(orphan_young, ".png") && store.has(kept_by_autosave, ".png"));
        QVERIFY(fs::exists(dir / "assets" / "readme.txt"));
        QVERIFY(!store.has(kept_by_pending, ".state.json"));  // (…so its state is not kept any more)
        // every state the history needs is there: undo all the way back
        for (int i = 0; i < 3; ++i) genko::storage::restore(dir, "genko", false, false);
        QCOMPARE(read(dir).page(0).note, std::string(""));
        QCOMPARE(genko::test::book_problems(to_qstring(dir)), std::string());
    }

    void gcKeepsTheLastHundredCommits() {
        const fs::path dir = make_book(book_path());
        // edit and undo: the edits leave the stacks, so only the last 100 commits keep their states
        for (int i = 0; i < 55; ++i) {
            edit(dir, (R"([{"op": "set_note", "page": 1, "note": "n)" + std::to_string(i) + R"("}])").c_str());
            genko::storage::restore(dir, "genko", false, false);
        }
        edit(dir, R"([{"op": "set_autosave"}])");
        genko::storage::AssetStore store(dir);
        for (const fs::path& file : store.all_files()) age(file, std::chrono::hours(48));
        const auto txns = journal::transactions(journal::read_lines(journal::journal_file(dir)));
        QCOMPARE(txns.size(), std::size_t{112});
        const std::string oldest_note = txns[1].prepare["after"].get<std::string>();  // ("n0", the first edit)
        const std::string recent_note = txns[99].prepare["after"].get<std::string>();  // ("n49", within the last 100)
        genko::storage::gc(dir, false);
        QVERIFY(!store.has(oldest_note, ".state.json"));
        QVERIFY(store.has(recent_note, ".state.json"));
        for (const auto& item : journal::stacks(dir).undo) {
            if (item.before) QVERIFY(store.has(*item.before, ".state.json"));
            if (item.after) QVERIFY(store.has(*item.after, ".state.json"));
        }
    }

    void doctorTellsWhatIsWrong() {
        const fs::path dir = make_book(book_path());
        edit(dir, R"([{"op": "set_note", "page": 1, "note": "x"}])");
        Json report = genko::storage::doctor(dir);
        QCOMPARE(report["ok"], Json(true));
        QCOMPARE(report["problems"], Json::array());
        QCOMPARE(report["revision"], Json(2));
        // a cut line, a pending transaction (project.json does not hold it), a missing history state
        Json prepare = journal::read_lines(journal::journal_file(dir)).values[0];
        prepare["txn"] = std::string(32, 'c');
        prepare["rev"] = 3;
        journal::append(journal::journal_file(dir), prepare);
        genko::storage::append_durable(journal::journal_file(dir), "{\"v\":4,\"ki");
        const std::string state = journal::read_lines(journal::journal_file(dir)).values[0]["after"].get<std::string>();
        fs::remove(genko::storage::AssetStore(dir).path(state, ".state.json"));
        report = genko::storage::doctor(dir);
        QCOMPARE(report["ok"], Json(false));
        const std::string problems = report["problems"].dump();
        QVERIFY2(problems.find("ends with a cut line (10 bytes)") != std::string::npos, problems.c_str());
        QVERIFY2(problems.find("transaction " + std::string(32, 'c') + " is unfinished; project.json does not hold its state") != std::string::npos,
                 problems.c_str());
        QVERIFY2(problems.find("state(s) of the Undo/Redo history are missing") != std::string::npos, problems.c_str());
        QCOMPARE(report["journal"]["repair"]["aborted"], Json::array({std::string(32, 'c')}));
        // and doctor wrote nothing
        QVERIFY(genko::storage::read_file(journal::journal_file(dir)).ends_with("{\"v\":4,\"ki"));
        // a change outside the journal
        const fs::path other = make_book(book_path());
        std::string text = genko::storage::read_file(other / "project.json");
        text.replace(text.find("\"autosave\": false"), 17, "\"autosave\": true");
        genko::storage::write_atomic(other / "project.json", text);
        report = genko::storage::doctor(other);
        QVERIFY(report["problems"].dump().find("changed outside the journal") != std::string::npos);
    }

private:
    static bool core_is_txn(const std::string& text) { return genko::core::is_txn_id(text); }
};

QTEST_GUILESS_MAIN(TestTransaction)
#include "test_transaction.moc"
