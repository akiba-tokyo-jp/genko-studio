// Saving through the genko command line, process by process (ACCEPTANCE.md AC-SAVE, at the level of files and
// processes; the GUI parts come with M2):
//   - a crash (GENKO_FAULT=<stage>:crash, the process stops with 77) at each of the five steps: another process
//     (doctor, inspect) finds the old or the new book, whole; the next write settles it; nothing is applied twice
//     and the approval is audited once (AC-SAVE 5);
//   - a cut journal or audit line (torn) is kept apart, not deleted (AC-SAVE 5, 9);
//   - a failed write (fail: ENOSPC) at each step, then the same command again: saved once (--txn: the same
//     transaction; --expect-revision: no second application) (AC-SAVE 2, 10);
//   - a failure after project.json was replaced settled at once (late), writes refused until the repair can be
//     written, then Undo and another process's save (AC-SAVE 10);
//   - the revision never comes back: edit, Undo, edit — the old base revision is refused (AC-SAVE 11);
//   - two processes, the OS lock, an older Genko's lock file, many writers at once (AC-SAVE 7);
//   - a folder that cannot be written; a v1–v3 book is never written (exit 3);
//   - after a save and a new process, the text, ruby, lines, pressure and coordinates are all there (AC-SAVE 1).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <string>
#include <vector>

#include "core/ids.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
using genko::test::run_genko;
using genko::test::one_line;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

Json project(const QString& book) { return genko::test::read_json(book + "/project.json"); }

std::vector<Json> journal_lines(const QString& book) {
    return genko::storage::journal::read_lines(to_path(book) / "studio" / "journal.jsonl").values;
}

// The transactions of the journal that are not among `known` (prepared by the command just run).
std::vector<std::string> new_txns(const QString& book, const std::set<std::string>& known) {
    std::vector<std::string> out;
    for (const Json& line : journal_lines(book)) {
        if (line.value("kind", "") == "prepare" && !known.contains(line["txn"].get<std::string>())) {
            out.push_back(line["txn"].get<std::string>());
        }
    }
    return out;
}

std::set<std::string> txns_of(const QString& book) {
    std::set<std::string> out;
    for (const Json& line : journal_lines(book)) {
        if (line.contains("txn")) out.insert(line["txn"].get<std::string>());
    }
    return out;
}

// "committed" | "aborted" | "pending" | "absent" for one txn (its last attempt).
std::string outcome(const QString& book, const std::string& txn) {
    std::string state = "absent";
    for (const Json& line : journal_lines(book)) {
        if (line.value("txn", "") != txn) continue;
        const std::string kind = line.value("kind", "");
        state = kind == "prepare" ? "pending" : kind == "commit" ? "committed" : kind == "abort" ? "aborted" : state;
    }
    return state;
}

int audit_lines_with(const QString& book, const std::string& what) {
    int n = 0;
    for (const Json& line : genko::storage::journal::audit_entries(to_path(book))) {
        if (line.dump().find(what) != std::string::npos) ++n;
    }
    return n;
}

QString ops_file(const QString& dir, const QString& name, const std::string& json) {
    const QString file = dir + "/" + name + ".json";
    genko::test::write_bytes(file, json);
    return file;
}

}  // namespace

class TestSaveE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int books_ = 0;

    QString new_book(int pages = 2) {
        const QString book = tmp_.path() + "/book" + QString::number(++books_) + ".genko";
        const auto r = run_genko({"new", book, "--pages", QString::number(pages)});
        if (r.exit_code != 0) qWarning("genko new failed: %s", r.out.constData());
        return book;
    }

    QString ops(const char* name, const std::string& json) { return ops_file(tmp_.path(), name, json); }

private slots:
    void crashAtEachStep_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("after");  // what the next process finds: old | new
        QTest::newRow("assets") << "assets" << "old";
        QTest::newRow("prepare") << "prepare" << "old";
        QTest::newRow("project") << "project" << "new";
        QTest::newRow("audit") << "audit" << "new";
        QTest::newRow("commit") << "commit" << "new";
    }

    void crashAtEachStep() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        QFETCH(QString, stage);
        QFETCH(QString, after);
        const QString book = new_book();
        const auto known = txns_of(book);
        const QString approve = ops("approve", R"([{"op": "name_ok", "page": 1}, {"op": "set_note", "page": 1, "note": "新"}])");
        const auto crashed = run_genko({"apply", book, approve, "--agent", "human:作者"}, genko::test::fault_env(stage + ":crash"));
        QCOMPARE(crashed.exit_code, 77);
        QVERIFY(crashed.out.isEmpty());  // (it stopped before it could answer)
        const auto txns = new_txns(book, known);
        const bool is_new = after == "new";

        // another process: doctor and inspect find a whole book, old or new
        const auto doctor = run_genko({"doctor", book});
        const Json report = one_line(doctor.out);
        const std::string problems = report["problems"].dump();
        if (stage == "prepare") {
            QVERIFY2(problems.find("does not hold its state, so the next write aborts it") != std::string::npos, problems.c_str());
        } else if (stage == "project" || stage == "audit") {
            QVERIFY2(problems.find("holds its new state, so the next write commits it") != std::string::npos, problems.c_str());
        } else {
            QCOMPARE(report["problems"], Json::array());
            QCOMPARE(doctor.exit_code, 0);
        }
        const Json snapshot = one_line(run_genko({"inspect", book}).out);
        QCOMPARE(snapshot["pages"][0]["note"], Json(is_new ? "新" : ""));
        QCOMPARE(snapshot["pages"][0]["name_ok"], Json(is_new));
        const auto loaded = genko::storage::load_document(to_path(book));
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());  // (no asset missing)

        // the next write settles it: once, with its approval audited once
        const auto next = run_genko({"apply", book, ops("next", R"([{"op": "set_note", "page": 2, "note": "次"}])"), "--agent", "human:作者"});
        QVERIFY2(next.exit_code == 0, next.out.constData());
        QCOMPARE(genko::test::book_problems(book), std::string());
        QCOMPARE(txns.size(), std::size_t{stage == "assets" ? 0u : 1u});
        if (!txns.empty()) QCOMPARE(outcome(book, txns[0]), std::string(is_new ? "committed" : "aborted"));
        QCOMPARE(audit_lines_with(book, ":name_ok"), is_new ? 1 : 0);
        QCOMPARE(project(book)["pages"][0]["note"], Json(is_new ? "新" : ""));
        QCOMPARE(project(book)["pages"][1]["note"], Json("次"));
        // and Undo walks back through it
        QCOMPARE(run_genko({"undo", book, "--as", "human:作者"}).exit_code, 0);
        if (is_new) {
            QCOMPARE(run_genko({"undo", book, "--as", "human:作者"}).exit_code, 0);
            QCOMPARE(project(book)["pages"][0]["name_ok"], Json(false));
            QCOMPARE(audit_lines_with(book, "\"via\":\"undo\""), 1);
        }
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void cutLinesAreKeptApart_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("file");  // which file is cut
        QTest::newRow("prepare") << "prepare" << "journal";
        QTest::newRow("audit") << "audit" << "audit";
        QTest::newRow("commit") << "commit" << "journal";
    }

    void cutLinesAreKeptApart() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        QFETCH(QString, stage);
        QFETCH(QString, file);
        const QString book = new_book();
        const std::string journal_before = genko::test::read_bytes(book + "/studio/journal.jsonl");
        const auto cut = run_genko({"apply", book, ops("cut", R"([{"op": "name_ok", "page": 2}])"), "--agent", "human:a"},
                               genko::test::fault_env(stage + ":torn"));
        QCOMPARE(cut.exit_code, 77);
        const QString target = book + "/studio/" + file + ".jsonl";
        const std::string bytes = genko::test::read_bytes(target);
        QVERIFY(!bytes.empty() && bytes.back() != '\n');
        const std::string fragment = bytes.substr(bytes.rfind('\n') + 1);
        QVERIFY(one_line(run_genko({"doctor", book}).out)["problems"].dump().find("ends with a cut line") != std::string::npos);
        // the next write keeps the cut line apart, and nothing before it goes
        const auto next = run_genko({"apply", book, ops("cut-next", R"([{"op": "set_autosave"}])"), "--agent", "human:a"});
        QVERIFY2(next.exit_code == 0, next.out.constData());
        QCOMPARE(genko::test::read_bytes(book + "/studio/" + file + ".partial"), fragment + "\n");
        QVERIFY(genko::test::read_bytes(book + "/studio/journal.jsonl").starts_with(journal_before));
        QCOMPARE(genko::test::book_problems(book), std::string());
        QCOMPARE(project(book)["pages"][1]["name_ok"], Json(stage != "prepare"));
        QCOMPARE(audit_lines_with(book, ":name_ok"), stage == "prepare" ? 0 : 1);
    }

    void gcKeepsACutJournal() {
        // GC is a write: it repairs first, keeps the cut line apart, and deletes neither journal lines nor what they need
        const QString book = new_book();
        QCOMPARE(run_genko({"apply", book, ops("gc-cut", R"([{"op": "set_note", "page": 1, "note": "残る"}])")}).exit_code, 0);
        const std::string complete = genko::test::read_bytes(book + "/studio/journal.jsonl");
        genko::storage::append_durable(to_path(book) / "studio" / "journal.jsonl", "{\"v\":4,\"kind\":\"prep");
        const auto dry = run_genko({"gc", book, "--dry-run"});
        QCOMPARE(dry.exit_code, 0);
        QCOMPARE(one_line(dry.out)["removed"], Json::array());
        QVERIFY(genko::test::read_bytes(book + "/studio/journal.jsonl").ends_with("prep"));  // (a dry run writes nothing)
        const auto done = run_genko({"gc", book});
        QCOMPARE(done.exit_code, 0);
        QCOMPARE(one_line(done.out)["removed"], Json::array());
        QCOMPARE(genko::test::read_bytes(book + "/studio/journal.jsonl"), complete);
        QCOMPARE(genko::test::read_bytes(book + "/studio/journal.partial"), std::string("{\"v\":4,\"kind\":\"prep\n"));
        QCOMPARE(run_genko({"undo", book}).exit_code, 0);  // (its history still whole)
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void failureThenTheSameCommandAgain_data() {
        QTest::addColumn<QString>("stage");
        for (const char* stage : {"assets", "prepare", "project", "audit", "commit"}) QTest::newRow(stage) << QString(stage);
    }

    void failureThenTheSameCommandAgain() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        QFETCH(QString, stage);
        const QString book = new_book();
        const QString txn = QString::fromStdString(genko::core::new_txn_id());
        const QString add = ops("add", R"([{"op": "add_page"}, {"op": "name_ok", "page": 1}])");
        const QStringList command{"apply", book, add, "--expect-revision", "1", "--txn", txn, "--agent", "human:a"};
        const auto failed = run_genko(command, genko::test::fault_env(stage + ":fail"));
        QCOMPARE(failed.exit_code, 1);
        const Json error = one_line(failed.out);
        QCOMPARE(error["code"], Json("io"));
        QVERIFY2(error["error"].get<std::string>().starts_with("[Errno 28] No space left on device:"), failed.out.constData());
        const bool replaced = stage == "audit" || stage == "commit";
        QCOMPARE(project(book)["pages"].size(), std::size_t{replaced ? 3u : 2u});
        QCOMPARE(outcome(book, txn.toStdString()), std::string(replaced ? "pending" : stage == "project" ? "aborted" : "absent"));

        // the same command, the failure gone: saved once
        const auto again = run_genko(command);
        QVERIFY2(again.exit_code == 0, again.out.constData());
        const Json reply = one_line(again.out);
        QCOMPARE(reply["txn"], Json(txn.toStdString()));
        QCOMPARE(reply.value("already_committed", false), replaced);
        QCOMPARE(project(book)["pages"].size(), std::size_t{3});
        QCOMPARE(outcome(book, txn.toStdString()), std::string("committed"));
        // and again: the same revision, nothing more
        const auto third = run_genko(command);
        QCOMPARE(one_line(third.out)["already_committed"], Json(true));
        QCOMPARE(one_line(third.out)["revision"], reply["revision"]);
        QCOMPARE(project(book)["pages"].size(), std::size_t{3});
        // without the txn, the base revision guards it
        const auto stale = run_genko({"apply", book, add, "--expect-revision", "1"});
        QCOMPARE(stale.exit_code, 1);
        QCOMPARE(one_line(stale.out)["code"], Json("revision_conflict"));
        QCOMPARE(audit_lines_with(book, ":name_ok"), 1);
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void lateFailureIsSettledAtOnce() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        const QString book = new_book();
        const auto r = run_genko({"apply", book, ops("late", R"([{"op": "add_page"}])")}, genko::test::fault_env("project:late"));
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QCOMPARE(one_line(r.out)["repaired"], Json(true));
        QCOMPARE(project(book)["pages"].size(), std::size_t{3});
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void writesWaitForTheRepair() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        const QString book = new_book();
        const auto first = run_genko({"apply", book, ops("w1", R"([{"op": "set_note", "page": 1, "note": "一"}])")},
                                 genko::test::fault_env("commit:fail"));
        QCOMPARE(first.exit_code, 1);
        const std::string journal = genko::test::read_bytes(book + "/studio/journal.jsonl");
        // still failing: the repair cannot be written, so nothing is
        const auto second = run_genko({"apply", book, ops("w2", R"([{"op": "set_note", "page": 2, "note": "二"}])")},
                                  genko::test::fault_env("commit:fail"));
        QCOMPARE(second.exit_code, 1);
        QCOMPARE(one_line(second.out)["code"], Json("io"));
        QCOMPARE(genko::test::read_bytes(book + "/studio/journal.jsonl"), journal);
        QCOMPARE(project(book)["pages"][1]["note"], Json(""));
        // the failure gone: the first is committed by the repair, the second saved after it
        const auto third = run_genko({"apply", book, ops("w2", R"([{"op": "set_note", "page": 2, "note": "二"}])")});
        QVERIFY2(third.exit_code == 0, third.out.constData());
        QCOMPARE(one_line(third.out)["revision"], Json(3));
        QCOMPARE(project(book)["pages"][0]["note"], Json("一"));
        QCOMPARE(project(book)["pages"][1]["note"], Json("二"));
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void undoAndAnotherSaveAfterAFailure() {
        if (!genko::test::fault_injection()) QSKIP("this build has no fault injection (a release build)");
        const QString book = new_book();
        const auto failed = run_genko({"apply", book, ops("u1", R"([{"op": "name_ok", "page": 1}])"), "--agent", "human:作者"},
                                  genko::test::fault_env("audit:fail"));
        QCOMPARE(failed.exit_code, 1);
        // Undo in another process: the failed save is settled first (committed: project.json held it), then undone
        const auto undo = run_genko({"undo", book, "--as", "human:作者"});
        QVERIFY2(undo.exit_code == 0, undo.out.constData());
        QCOMPARE(one_line(undo.out)["rev"], Json(2));
        QCOMPARE(project(book)["pages"][0]["name_ok"], Json(false));
        QCOMPARE(audit_lines_with(book, ":name_ok"), 2);  // (approved once, undone once)
        QCOMPARE(audit_lines_with(book, "\"via\":\"undo\""), 1);
        const auto other = run_genko({"apply", book, ops("u2", R"([{"op": "set_note", "page": 1, "note": "x"}])"), "--agent", "ai:x"});
        QVERIFY2(other.exit_code == 0, other.out.constData());
        QCOMPARE(one_line(other.out)["revision"], Json(4));
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void oldRevisionIsRefusedAfterUndo() {
        const QString book = new_book();
        const std::int64_t a_read = project(book)["revision"].get<std::int64_t>();  // client A reads revision 1
        const QString b_edit = ops("b", R"([{"op": "set_note", "page": 1, "note": "B"}])");
        QCOMPARE(one_line(run_genko({"apply", book, b_edit, "--expect-revision", "1"}).out)["revision"], Json(2));
        QCOMPARE(one_line(run_genko({"undo", book}).out)["revision"], Json(3));
        // the book is as A read it, but not at A's revision
        const auto lines = journal_lines(book);
        QCOMPARE(lines.back()["kind"], Json("commit"));
        QCOMPARE(lines[lines.size() - 2]["after"], lines[0]["after"]);
        const QString a_edit = ops("a", R"([{"op": "set_note", "page": 1, "note": "A"}])");
        auto refused = run_genko({"apply", book, a_edit, "--expect-revision", QString::number(a_read)});
        QCOMPARE(refused.exit_code, 1);
        QCOMPARE(one_line(refused.out), Json::object({{"ok", false}, {"error", "revision conflict: expected 1, found 3"},
                                                      {"code", "revision_conflict"}}));
        QCOMPARE(run_genko({"apply", book, a_edit, "--expect-revision", "1", "--dry-run"}).exit_code, 1);
        QCOMPARE(one_line(run_genko({"apply", book, ops("b2", R"([{"op": "set_autosave"}])")}).out)["revision"], Json(4));
        refused = run_genko({"apply", book, a_edit, "--expect-revision", "1"});
        QCOMPARE(one_line(refused.out)["error"], Json("revision conflict: expected 1, found 4"));
        QCOMPARE(project(book)["pages"][0]["note"], Json(""));
        QCOMPARE(one_line(run_genko({"apply", book, a_edit, "--expect-revision", "4"}).out)["revision"], Json(5));
    }

    void twoProcessesAndTheLock() {
        const QString book = new_book();
        const QString edit = ops("lock-edit", R"([{"op": "set_autosave"}])");
        genko::test::Holder holder;
        QCOMPARE(holder.start(genko::test::lock_helper(), {book, "helper-agent"}), QString("locked"));
        auto r = run_genko({"apply", book, edit});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out),
                 Json::object({{"ok", false},
                               {"error", "project locked: " + genko::test::lock_file_text(book) + " (by " +
                                             genko::test::holder_seen("helper-agent") + ")"},
                               {"code", "locked"}}));
        QCOMPARE(run_genko({"undo", book}).exit_code, 1);
        holder.stop();
        QCOMPARE(run_genko({"apply", book, edit}).exit_code, 0);
        // an older Genko's lock file: respected for 15 minutes
        const double now = genko::storage::journal::now_seconds();
        genko::test::write_bytes(book + "/project.lock", "{\"agent\": \"old genko\", \"acquired_at\": " + std::to_string(now - 60) + "}");
        r = run_genko({"apply", book, edit});
        QCOMPARE(one_line(r.out)["error"], Json("project locked: " + genko::test::lock_file_text(book) + " (by old genko)"));
        genko::test::write_bytes(book + "/project.lock", "{\"agent\": \"old genko\", \"acquired_at\": " + std::to_string(now - 1000) + "}");
        QCOMPARE(run_genko({"apply", book, edit}).exit_code, 0);
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void manyWritersAtOnce() {
        const QString book = new_book(1);
        const QString add = ops("many", R"([{"op": "add_page"}])");
        constexpr int kWriters = 10;
        int saved = 0;
        for (int round = 0; round < 40 && saved < kWriters; ++round) {
            std::vector<std::unique_ptr<QProcess>> running;
            for (int i = saved; i < kWriters; ++i) {
                auto process = std::make_unique<QProcess>();
                process->start(genko::test::genko_cli(), {"apply", book, add, "--agent", QString("ai:w%1").arg(i)});
                running.push_back(std::move(process));
            }
            for (auto& process : running) {
                QVERIFY(process->waitForFinished(120000));
                const Json reply = one_line(process->readAllStandardOutput());
                if (process->exitCode() == 0) {
                    ++saved;
                } else {
                    QCOMPARE(reply["code"], Json("locked"));  // (the only reason to fail here)
                }
            }
        }
        QCOMPARE(saved, kWriters);
        QCOMPARE(project(book)["pages"].size(), std::size_t{1 + kWriters});  // (no write lost, none applied twice)
        QCOMPARE(project(book)["revision"], Json(1 + kWriters));
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void undoWhileOthersSave() {
        // saves and Undo from many processes at once: each one whole, in one order, no revision used twice
        const QString book = new_book(1);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(run_genko({"apply", book, ops("seed", R"([{"op": "set_note", "page": 1, "note": "seed"}])")}).exit_code, 0);
        }
        const QString add = ops("mixed-add", R"([{"op": "add_page"}])");
        std::vector<QStringList> commands;
        for (int i = 0; i < 5; ++i) {
            commands.push_back({"apply", book, add, "--agent", QString("ai:w%1").arg(i)});
            commands.push_back({"undo", book, "--force"});
        }
        int done = 0;
        for (int round = 0; round < 60 && !commands.empty(); ++round) {
            std::vector<std::pair<QStringList, std::unique_ptr<QProcess>>> running;
            for (const QStringList& command : commands) {
                auto process = std::make_unique<QProcess>();
                process->start(genko::test::genko_cli(), command);
                running.emplace_back(command, std::move(process));
            }
            commands.clear();
            for (auto& [command, process] : running) {
                QVERIFY(process->waitForFinished(120000));
                const Json reply = one_line(process->readAllStandardOutput());
                if (process->exitCode() == 0) {
                    ++done;
                } else if (reply["code"] == Json("locked")) {
                    commands.push_back(command);  // (try again)
                } else {  // (Undo went back to the book's creation)
                    QVERIFY2(reply["code"] == Json("no_before") || reply["code"] == Json("nothing_to_undo"), reply.dump().c_str());
                }
            }
        }
        QVERIFY(commands.empty());
        QVERIFY(done >= 5);
        QCOMPARE(genko::test::book_problems(book), std::string());
        const Json report = one_line(run_genko({"doctor", book}).out);
        QCOMPARE(report["problems"], Json::array());
        std::int64_t commits = 0;
        for (const Json& line : journal_lines(book)) commits += line.value("kind", "") == "commit" ? 1 : 0;
        QCOMPARE(project(book)["revision"], Json(commits));  // (one revision per saved change, none reused)
    }

    void foldersThatCannotBeWritten() {
        const QString book = new_book();
        const auto before = genko::test::tree_hashes(book, false);
        // the journal cannot be written: nothing is saved
        QFile::setPermissions(book + "/studio/journal.jsonl", QFileDevice::ReadOwner);
        auto r = run_genko({"apply", book, ops("ro", R"([{"op": "set_note", "page": 1, "note": "x"}])")});
        QCOMPARE(r.exit_code, 1);
        QVERIFY2(one_line(r.out)["error"].get<std::string>().starts_with("[Errno 13] Permission denied:"), r.out.constData());
        QFile::setPermissions(book + "/studio/journal.jsonl", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(project(book)["pages"][0]["note"], Json(""));
        QCOMPARE(genko::test::book_problems(book), std::string());
#ifndef Q_OS_WIN  // (a folder's read-only flag does not stop new files on Windows)
        // project.json cannot be replaced (the folder is read-only): the transaction is aborted, the book is the old one
        QFile::setPermissions(book, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        r = run_genko({"apply", book, ops("ro", R"([{"op": "set_note", "page": 1, "note": "x"}])")});
        QFile::setPermissions(book, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QCOMPARE(r.exit_code, 1);
        QVERIFY2(one_line(r.out)["error"].get<std::string>().find("Permission denied") != std::string::npos, r.out.constData());
        QCOMPARE(one_line(run_genko({"inspect", book}).out)["pages"][0]["note"], Json(""));
        QCOMPARE(genko::test::book_problems(book), std::string());
#endif
        QCOMPARE(run_genko({"apply", book, ops("ro", R"([{"op": "set_note", "page": 1, "note": "x"}])")}).exit_code, 0);
        QCOMPARE(project(book)["pages"][0]["note"], Json("x"));
        QVERIFY(genko::test::tree_hashes(book, false) != before);
    }

    void oldBooksAreNeverWritten_data() {
        QTest::addColumn<QString>("version");
        for (const char* v : {"v1", "v2", "v3"}) QTest::newRow(v) << QString(v);
    }

    void oldBooksAreNeverWritten() {
        QFETCH(QString, version);
        const QString book = tmp_.path() + "/old-" + version + ".genko";
        genko::test::copy_tree(genko::test::test_data("legacy/book-" + version + ".genko"), book);
        const auto before = genko::test::tree_hashes(book);
        const QString edit = ops("old", R"([{"op": "set_autosave"}])");
        for (const QStringList& args : {QStringList{"apply", book, edit}, QStringList{"apply", book, edit, "--dry-run"},
                                        QStringList{"undo", book}, QStringList{"redo", book}, QStringList{"gc", book}}) {
            const auto r = run_genko(args);
            QCOMPARE(r.exit_code, 3);
            const Json out = one_line(r.out);
            QCOMPARE(out["code"], Json("needs_migration"));
            QVERIFY2(out["error"].get<std::string>().find("genko migrate") != std::string::npos, r.out.constData());
        }
        QVERIFY(genko::test::tree_hashes(book) == before);  // (no project.lock made either)
        QCOMPARE(run_genko({"doctor", book}).exit_code, 0);  // (reading is fine)
        QCOMPARE(one_line(run_genko({"doctor", book}).out)["needs_migration"], Json(true));
    }

    void savedBookReadsBackWhole() {
        const QString book = tmp_.path() + "/readback.genko";
        QCOMPARE(run_genko({"migrate", genko::test::test_data("legacy/book-v3.genko"), book}).exit_code, 0);
        const auto r = run_genko({"apply", book, ops("rb", R"([{"op": "set_note", "page": 2, "note": "保存の確認"}])"), "--agent", "human:作者"});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        // a new process
        const Json snapshot = one_line(run_genko({"inspect", book, "--full"}).out);
        QCOMPARE(snapshot["pages"][1]["note"], Json("保存の確認"));
        QCOMPARE(snapshot["pages"][0]["story"][0]["text"], Json("東京と東京へ🙂"));
        QCOMPARE(snapshot["pages"][0]["ink_strokes"][0], genko::core::parse_python_json("[[30.0, 30.0, 0.2], [60.25, 45.5, 0.9], [90.0, 40.0, 0.5]]"));
        const auto loaded = genko::storage::load_document(to_path(book));
        QVERIFY(loaded.report.clean());
        const auto& line = loaded.document.story.at(0);
        QCOMPARE(line.ruby_runs.at(0), genko::core::parse_python_json(R"(["東京", "とうきょう"])"));
        QCOMPARE(line.emphasis_runs, std::vector<std::string>({"へ"}));
        QCOMPARE(line.wrap, std::string("vertical"));
        QVERIFY(line.x_mm.same(genko::core::Num(120.0)) && line.h_mm.same(genko::core::Num(60.0)));
        const auto* ink = loaded.document.page(0).first_layer(genko::core::LayerRole::Ink);
        QVERIFY(ink != nullptr && ink->stroke_count() == 2);
        QCOMPARE(ink->strokes->items[0]->pressure, std::vector<double>({0.2, 0.9, 0.5}));
        QCOMPARE(ink->strokes->items[0]->points[1].x, 60.25);
        QCOMPARE(ink->strokes->items[1]->rgb.value(), std::vector<std::int64_t>({200, 10, 10}));
    }
};

QTEST_GUILESS_MAIN(TestSaveE2e)
#include "test_save_e2e.moc"
