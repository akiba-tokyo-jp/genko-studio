// The ops of M2-O1 through the command line: `genko apply` against `python -m genko apply` (the baseline's own command
// line), on the book of the op contract tests (`pyref_harness.py make-opsbook`, a v3 book: Python gets a copy of it,
// C++ a copy of its `genko migrate` conversion).
//   1. The fixed cases of contract/ops_cases.json that are one batch (for each op its first 5 that succeed and first 3
//      that fail, and every batch-level case): the same exit code and the same JSON on stdout, and then the same book
//      (`inspect --full` of each). Not compared:
//        - the keys only a v4 book has (docs/cpp-migration/ARCHITECTURE.md §8): "revision" and "txn" of a success
//          ("revision" alone for a dry run), "code" of a failure. They are checked to be there and of their kind.
//        - "job_id", a new id on both sides (checked to be one), and the ids each side makes at random for what the ops
//          add: replaced by the order in which they first appear, on each side (contract test 1 of
//          test_contract_ops compares them exactly, with ids counted the same on both sides).
//      Where Python stops with a traceback (an exception apply_ops lets through), C++ prints {"ok": false, "error",
//      "code": "python_error"} and exits 1 as Python does: the error ends with the traceback's last line.
//      Left out: the parts this build refuses on purpose ("cpp": "not_yet_ported") and [{"op": "undo"}] (below).
//   2. [{"op": "undo"}]: `genko apply` undoes the latest saved change as `genko undo --as <agent>` does (the same
//      reply keys kind and rev, the same book after, the same refusals and exit codes) and as Python's `genko undo`
//      (journal.restore) does on the v3 book. Python's own apply_ops undoes only its session's changes, so its
//      `genko apply` with this op always answers "nothing to undo" (a command is a new session): this build's
//      `genko apply` takes the book's saved history instead.
// Skipped when there is no reference Python ($GENKO_PYREF or /opt/pyref/bin/python).

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/ids.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

bool is_hex(const std::string& text, std::size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// An id as Python's new_id makes them (12 hex digits; a page's id is "pg_" and 12).
bool is_id(const std::string& text) { return is_hex(text, 12) || (text.starts_with("pg_") && is_hex(text.substr(3), 12)); }

void collect_strings(const Json& value, std::set<std::string>& out) {
    if (value.is_string()) {
        out.insert(value.get<std::string>());
    } else if (value.is_array()) {
        for (const Json& item : value) collect_strings(item, out);
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            out.insert(it.key());
            collect_strings(it.value(), out);
        }
    }
}

// The ids that are not in the book (`known`) replaced by "new-1", "new-2", … in the order they first appear.
Json with_new_ids_numbered(const Json& value, const std::set<std::string>& known, std::map<std::string, std::string>& seen) {
    const auto renamed = [&](const std::string& text) {
        if (!is_id(text) || known.contains(text)) return text;
        const auto [it, added] = seen.emplace(text, "new-" + std::to_string(seen.size() + 1));
        return it->second;
    };
    if (value.is_string()) return Json(renamed(value.get<std::string>()));
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(with_new_ids_numbered(item, known, seen));
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) out[renamed(it.key())] = with_new_ids_numbered(it.value(), known, seen);
        return out;
    }
    return value;
}

Json numbered(const Json& value, const std::set<std::string>& known) {
    std::map<std::string, std::string> seen;
    return with_new_ids_numbered(value, known, seen);
}

// The last line of a traceback ("KeyError: 'page'").
std::string last_line(const QByteArray& err) {
    const QList<QByteArray> lines = err.trimmed().split('\n');
    return lines.isEmpty() ? std::string() : lines.back().trimmed().toStdString();
}

}  // namespace

class TestContractOpsCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString v3_;  // the book as Python made it
    QString v4_;  // its conversion
    std::set<std::string> known_;  // every string in the book's files (its ids among them)

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run python(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args,
                                genko::test::python_env(path("pyenv")));
    }

    // `inspect --full` of a book on each side, the new ids numbered. `any_key_order`: the keys of objects in any order
    // (a book restored by Undo or Redo: see undoAsOp).
    std::string same_books(const QString& cpp_book, const QString& py_book, bool any_key_order = false) {
        const auto cpp = genko::test::run_genko({"inspect", cpp_book, "--full"});
        const auto py = python({"inspect", py_book, "--full"});
        if (cpp.exit_code != 0 || py.exit_code != 0) return "inspect failed: " + (cpp.out + cpp.err + py.out + py.err).toStdString();
        const Json a = numbered(genko::test::one_line(cpp.out), known_), b = numbered(genko::test::one_line(py.out), known_);
        std::string where;
        if (!(any_key_order ? genko::test::same_content(a, b, &where) : genko::test::strict_equal(a, b, &where))) {
            return "the books differ: " + where;
        }
        return {};
    }

    static std::string same_content(const QByteArray& a, const QByteArray& b) {
        std::string where;
        return genko::test::same_content(genko::test::one_line(a), genko::test::one_line(b), &where) ? std::string() : where;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        v3_ = path("opsbook.genko");
        v4_ = path("opsbook-v4.genko");
        const auto made = genko::test::harness({"make-opsbook", v3_}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const auto converted = genko::test::run_genko({"migrate", v3_, v4_});
        QVERIFY2(converted.exit_code == 0, (converted.out + converted.err).constData());
        QDirIterator it(v3_, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            if (file.endsWith(QLatin1String(".json"))) collect_strings(genko::test::read_json(file), known_);
        }
        QVERIFY(known_.size() > 50);
    }

    void applyAgainstPython() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/ops_cases.json");
        std::map<std::string, std::pair<int, int>> taken;  // op → (successes, failures) chosen
        std::map<std::string, std::pair<int, int>> matched;
        std::vector<std::string> failures;
        int n = 0, tracebacks = 0;
        for (const Json& c : file["cases"]) {
            ++n;
            if (!c.contains("ops") || c.contains("cpp")) continue;
            const Json& ops = c["ops"];
            if (ops.is_array() && ops.size() == 1 && ops[0].is_object() && ops[0].value("op", Json()) == Json("undo") &&
                !c.value("dry", false)) {
                continue;  // (undoAsOp)
            }
            const std::string op = c["op"].get<std::string>();
            const bool ok = c["ok"].get<bool>();
            auto& [oks, fails] = taken[op];
            if (op != "_bus" && (ok ? oks >= 5 : fails >= 3)) continue;
            (ok ? oks : fails) += 1;

            const std::string name = c["n"].get<std::string>();
            const QString cpp_book = path(QStringLiteral("cpp-%1.genko").arg(n));
            const QString py_book = path(QStringLiteral("py-%1.genko").arg(n));
            genko::test::copy_tree(v4_, cpp_book);
            genko::test::copy_tree(v3_, py_book);
            const QString ops_file = path(QStringLiteral("ops-%1.json").arg(n));
            genko::test::write_bytes(ops_file, genko::core::dump_python(ops));
            const bool dry = c.value("dry", false);
            const QString agent = QString::fromStdString(c.value("agent", std::string("genko")));
            QStringList tail{ops_file, "--agent", agent};
            if (dry) tail << "--dry-run";
            const auto cpp = genko::test::run_genko(QStringList{"apply", cpp_book} + tail);
            const auto py = python(QStringList{"apply", py_book} + tail);

            const auto fail = [&](const std::string& why) {
                failures.push_back(name + ": " + why.substr(0, 1500) + " | C++ " + cpp.out.left(300).toStdString() + cpp.err.left(300).toStdString() +
                                   " | Python " + py.out.left(300).toStdString() + py.err.right(300).toStdString());
            };
            if (!cpp.finished || !py.finished) {
                fail("did not finish");
                continue;
            }
            if (cpp.exit_code != py.exit_code) {
                fail("exit codes " + std::to_string(cpp.exit_code) + " and " + std::to_string(py.exit_code));
                continue;
            }
            Json got = genko::test::one_line(cpp.out);
            if (!got.is_object()) {
                fail("C++ printed " + got.dump());
                continue;
            }
            if (py.out.isEmpty()) {  // a traceback
                const std::string line = last_line(py.err);
                if (!py.err.contains("Traceback") || got.value("code", std::string()) != "python_error" ||
                    !got.value("error", std::string()).ends_with(line)) {
                    fail("Python stopped with " + line);
                    continue;
                }
                ++tracebacks;
            } else {
                Json want = genko::test::one_line(py.out);
                if (got.value("ok", false)) {
                    // the keys of a v4 book, and the job's new id
                    const bool has_revision = got.contains("revision") && got["revision"].is_number_integer();
                    const bool has_txn = dry || (got.contains("txn") && is_hex(got["txn"].get<std::string>(), 32));
                    if (!has_revision || !has_txn || !is_hex(got.value("job_id", std::string()), 12) ||
                        !is_hex(want.value("job_id", std::string()), 12)) {
                        fail("revision, txn or job_id missing");
                        continue;
                    }
                    for (const char* key : {"revision", "txn", "job_id"}) got.erase(key);
                    want.erase("job_id");
                } else {
                    if (!got.contains("code") || got["code"].get<std::string>().empty()) {
                        fail("no code");
                        continue;
                    }
                    got.erase("code");
                }
                std::string where;
                if (!genko::test::strict_equal(numbered(got, known_), numbered(want, known_), &where)) {
                    fail("stdout: " + where);
                    continue;
                }
            }
            if (const std::string differ = same_books(cpp_book, py_book); !differ.empty()) {
                fail(differ);
                continue;
            }
            auto& [ok_matched, fail_matched] = matched[op];
            (ok ? ok_matched : fail_matched) += 1;
            QDir(cpp_book).removeRecursively();
            QDir(py_book).removeRecursively();
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " batches differ from Python's command line").c_str());
        std::string summary;
        int total = 0;
        for (const auto& [op, counts] : matched) {
            summary += op + " " + std::to_string(counts.first) + "/" + std::to_string(counts.second) + ", ";
            total += counts.first + counts.second;
            if (op != "_bus") QVERIFY2(counts.first >= 3 && counts.second >= 3, (op + ": too few cases").c_str());
        }
        QCOMPARE(matched.size(), std::size_t{34});  // the 33 ops and the batch-level cases
        qInfo("%d batches the same as `python -m genko apply` (ok/failing): %s%d of them a traceback in Python", total,
              summary.c_str(), tracebacks);
    }

    void undoAsOp() {
        const QString note_ops = path("note.json"), undo_ops = path("undo.json");
        genko::test::write_bytes(note_ops, R"([{"op": "set_note", "page": 2, "note": "メモ"}])");
        genko::test::write_bytes(undo_ops, R"([{"op": "undo"}])");
        const auto original = genko::test::run_genko({"inspect", v4_, "--full"});
        QCOMPARE(original.exit_code, 0);

        struct Case {
            const char* name;
            const char* editor;  // who made the change undone
            const char* undoer;
            bool outside_edit;   // project.json edited by hand after the change
            const char* code;    // nullptr: it is undone
        };
        const Case cases[] = {
            {"own change", "human:作者", "human:作者", false, nullptr},
            {"own change, AI", "ai:hermes", "ai:hermes", false, nullptr},
            {"another actor's change", "human:作者", "ai:hermes", false, "other_actor"},
            {"another person's change", "ai:hermes", "human:作者", false, "other_actor"},
            {"edited outside", "human:作者", "human:作者", true, "external_change"},
        };
        int k = 0;
        for (const Case& c : cases) {
            ++k;
            const QString via_apply = path(QStringLiteral("undo-a-%1.genko").arg(k));
            const QString via_undo = path(QStringLiteral("undo-u-%1.genko").arg(k));
            const QString py_book = path(QStringLiteral("undo-p-%1.genko").arg(k));
            genko::test::copy_tree(v4_, via_apply);
            genko::test::copy_tree(v4_, via_undo);
            genko::test::copy_tree(v3_, py_book);
            QCOMPARE(genko::test::run_genko({"apply", via_apply, note_ops, "--agent", c.editor}).exit_code, 0);
            QCOMPARE(genko::test::run_genko({"apply", via_undo, note_ops, "--agent", c.editor}).exit_code, 0);
            QCOMPARE(python({"apply", py_book, note_ops, "--agent", c.editor}).exit_code, 0);
            const auto edited = genko::test::run_genko({"inspect", via_apply, "--full"});
            if (c.outside_edit) {
                for (const QString& book : {via_apply, via_undo, py_book}) {
                    std::string text = genko::test::read_bytes(book + "/project.json");
                    const std::string from = "操作の試験", to = "外で変えた";
                    text.replace(text.find(from), from.size(), to);
                    genko::test::write_bytes(book + "/project.json", text);
                }
            }
            const auto applied = genko::test::run_genko({"apply", via_apply, undo_ops, "--agent", c.undoer});
            const auto undone = genko::test::run_genko({"undo", via_undo, "--as", c.undoer});
            const auto py = python({"undo", py_book, "--as", c.undoer});
            const Json a = genko::test::one_line(applied.out), u = genko::test::one_line(undone.out), p = genko::test::one_line(py.out);
            const QByteArray what = QByteArray(c.name) + ": " + applied.out + " | " + undone.out + " | " + py.out + py.err.right(400);
            QVERIFY2(applied.exit_code == undone.exit_code && applied.exit_code == py.exit_code, what.constData());
            if (c.code != nullptr) {
                QVERIFY2(applied.exit_code == 1, what.constData());
                QCOMPARE(a["code"], Json(c.code));
                QCOMPARE(u["code"], Json(c.code));
                QCOMPARE(a["error"], p["error"]);
                QCOMPARE(u["error"], p["error"]);
                QCOMPARE(p.size(), std::size_t{2});  // {"ok": false, "error"}
                // nothing changed
                const auto after = genko::test::run_genko({"inspect", via_apply, "--full"});
                if (!c.outside_edit) QCOMPARE(after.out, edited.out);
                continue;
            }
            QVERIFY2(applied.exit_code == 0, what.constData());
            // the reply: the op's (applied, snapshot, job_id), and what `genko undo` says (kind, rev, revision)
            QCOMPARE(a["applied"], Json::array({"undo"}));
            QVERIFY(is_hex(a["job_id"].get<std::string>(), 12));
            QVERIFY(is_hex(a["txn"].get<std::string>(), 32));
            for (const char* key : {"ok", "kind", "rev", "revision"}) QCOMPARE(a[key], u[key]);
            QCOMPARE(a["kind"], p["kind"]);
            QCOMPARE(a["rev"], p["rev"]);
            QCOMPARE(p.size(), std::size_t{3});  // {"ok", "kind", "rev"}
            // the book: as before the change, the same on the three sides, and the reply's snapshot is the book's. (A
            // book restored by Undo or Redo is a state of its history, which is canonical JSON: the keys of what the
            // book keeps as it is given, such as tickets, come back sorted, where Python writes the old bytes back —
            // so the books are compared with their keys in any order. `genko undo` does the same: M1.)
            const auto after = genko::test::run_genko({"inspect", via_apply, "--full"});
            QVERIFY2(same_content(after.out, original.out).empty(), same_content(after.out, original.out).c_str());
            QCOMPARE(genko::test::run_genko({"inspect", via_undo, "--full"}).out, after.out);
            const std::string differ = same_books(via_apply, py_book, true);
            QVERIFY2(differ.empty(), differ.c_str());
            QCOMPARE(genko::test::one_line(genko::test::run_genko({"inspect", via_apply}).out), a["snapshot"]);
            // and both are redone alike
            const auto redone_a = genko::test::run_genko({"redo", via_apply, "--as", c.undoer});
            const auto redone_u = genko::test::run_genko({"redo", via_undo, "--as", c.undoer});
            const auto redone_p = python({"redo", py_book, "--as", c.undoer});
            QCOMPARE(redone_a.exit_code, 0);
            QCOMPARE(redone_u.exit_code, 0);
            QCOMPARE(redone_p.exit_code, 0);
            QCOMPARE(genko::test::one_line(redone_a.out)["rev"], genko::test::one_line(redone_p.out)["rev"]);
            const auto redone = genko::test::run_genko({"inspect", via_apply, "--full"});
            QVERIFY2(same_content(redone.out, edited.out).empty(), same_content(redone.out, edited.out).c_str());
            QCOMPARE(genko::test::run_genko({"inspect", via_undo, "--full"}).out, redone.out);
            const std::string redone_differ = same_books(via_apply, py_book, true);
            QVERIFY2(redone_differ.empty(), redone_differ.c_str());
        }

        // approvals: an AI's change that took one back cannot be undone by the AI (needs a person), through either
        // command; a person can
        const QString via_apply = path("undo-approval-a.genko"), via_undo = path("undo-approval-u.genko");
        for (const QString& book : {via_apply, via_undo}) {
            genko::test::copy_tree(v4_, book);
            const fs::path dir = genko::storage::path_from_utf8(book.toStdString());
            genko::storage::ProjectLock lock(dir, "ai:hermes");
            lock.try_acquire();
            genko::core::Document doc = genko::storage::load_document(dir).document;
            std::size_t approved = doc.pages.size();
            for (std::size_t i = 0; i < doc.pages.size() && approved == doc.pages.size(); ++i) {
                if (doc.page(i).name_ok) approved = i;
            }
            QVERIFY(approved < doc.pages.size());
            doc.edit_page(approved).name_ok = false;
            genko::storage::SaveRequest request;
            request.actor = "ai:hermes";
            request.base_revision = doc.revision;
            request.ops = Json::array({Json::object({{"op", "edit"}})});
            genko::storage::Saver(lock).save(doc, request);
        }
        const auto applied = genko::test::run_genko({"apply", via_apply, undo_ops, "--agent", "ai:hermes"});
        const auto undone = genko::test::run_genko({"undo", via_undo, "--as", "ai:hermes"});
        QCOMPARE(applied.exit_code, 1);
        QCOMPARE(undone.exit_code, 1);
        const Json a = genko::test::one_line(applied.out), u = genko::test::one_line(undone.out);
        QCOMPARE(a["code"], Json("needs_person"));
        QCOMPARE(a, u);
        QVERIFY2(a["error"].get<std::string>().starts_with("this undo changes approvals (page:"), applied.out.constData());
        QCOMPARE(genko::test::run_genko({"apply", via_apply, undo_ops, "--agent", "human:作者"}).exit_code, 1);  // (another actor's)
        // a dry run only shows the book
        const auto dry = genko::test::run_genko({"apply", via_apply, undo_ops, "--agent", "ai:hermes", "--dry-run"});
        QCOMPARE(dry.exit_code, 0);
        QCOMPARE(genko::test::one_line(dry.out)["applied"], Json::array({"undo"}));
    }
};

int main(int argc, char** argv) {
    // (applyAgainstPython runs some 1,200 programs: with the sanitizers that takes longer than QtTest's 5 minutes for
    // a test function; CTest's TIMEOUT still bounds the whole)
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "3000000");
    QCoreApplication app(argc, argv);
    TestContractOpsCli test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_ops_cli.moc"
