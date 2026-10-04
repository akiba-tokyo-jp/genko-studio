// Saving and converting against the Python baseline (src/genko run by the reference Python):
//   1. project.lock: Python's genko.lock.ProjectLock and the C++ lock exclude each other, both ways and across
//      processes; while Python holds a book, `genko apply` / `undo` / `gc` are "locked" and `genko migrate` of an old
//      book is "source_locked"; each side reads the other's content (who holds it, "released").
//   2. The M1 ops: the same batches through Python's apply_ops and the C++ CommandBus give the same reply (applied,
//      snapshot, job_id, warnings — or the same error) and the same project.json.
//   3. A v3 book converted by `genko migrate`: Undo and Redo into its old history (legacy/map.json) give the same
//      book and the same refusals as Python's journal.restore on the original, right after the conversion and after
//      a new edit on both.
//   4. The converted v1/v2/v3 books read as Python reads the originals; a v2 book that Python upgraded in place keeps
//      its v2 snapshot and its pages/ picture reachable.
//   5. Python writing (apply, gc) while `genko migrate` copies the book: refused when the book has project.lock, else
//      the conversion stops ("source_changed"); a GC that changes only project.lock does not stop it.
// Skipped when there is no reference Python ($GENKO_PYREF or /opt/pyref/bin/python).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

// project.json v4 as Python's v3 writer would write it (no min_reader, writer, book_id, features; version 3; no
// revision).
Json as_v3(Json payload) {
    for (const char* key : {"min_reader", "writer", "book_id", "features", "revision"}) payload.erase(key);
    payload["version"] = 3;
    return payload;
}

// A v1 snapshot without the layer ids made while reading (both sides make their own).
Json without_layer_ids(Json snapshot) {
    for (Json& page : snapshot["pages"]) {
        int n = 0;
        for (Json& layer : page["layers"]) layer["id"] = "layer-" + std::to_string(n++);
    }
    return snapshot;
}

Json without(Json value, const char* key) {
    value.erase(key);
    return value;
}

struct Case {
    const char* name;
    const char* ops;
    const char* agent;
    bool dry_run;
};

// The batches compared with Python (book: the v3 test book, two pages, page 1 approved, one line on page 1).
const Case kCases[] = {
    {"set_note", R"([{"op": "set_note", "page": 2, "note": "メモ\n二行目"}])", "human:作者", false},
    {"set_note no page", R"([{"op": "set_note", "page": 9, "note": "x"}])", "genko", false},
    {"set_note page missing", R"([{"op": "set_note", "note": "x"}])", "genko", false},
    {"set_note page as text, unknown key", R"([{"op": "set_note", "page": "1", "note": 5, "colour": "red"}])", "genko", false},
    {"set_note page float", R"([{"op": "set_note", "page": 1.9, "note": null}])", "genko", false},
    {"set_meta", R"([{"op": "set_meta", "title": "新しい題", "episode": "7", "binding": "left", "start_side": "right",
                     "strict_gates": 1, "font_path": "/fonts/明朝.otf"}])", "genko", false},
    {"set_meta start_side null", R"([{"op": "set_meta", "start_side": null}])", "genko", false},
    {"set_meta bad episode", R"([{"op": "set_meta", "episode": "x"}])", "genko", false},
    {"set_meta bad binding", R"([{"op": "set_meta", "binding": "up"}])", "genko", false},
    {"set_meta bad start_side", R"([{"op": "set_meta", "start_side": "top"}])", "genko", false},
    {"set_meta preset", R"([{"op": "set_meta", "preset": " Shueisha "}])", "genko", false},
    {"set_meta webtoon", R"([{"op": "set_meta", "webtoon": true}])", "genko", false},
    {"name_ok page", R"([{"op": "name_ok", "page": 2}])", "human:作者", false},
    {"name_ok by an AI", R"([{"op": "name_ok", "page": 2}])", "ai:hermes", false},
    {"name_ok all", R"([{"op": "name_ok"}])", "genko", false},
    {"lock then edit", R"([{"op": "lock_page", "page": 1, "agent": "ai:x"}, {"op": "set_note", "page": 1}])", "genko", false},
    {"lock as another", R"([{"op": "lock_page", "page": 1, "agent": "ai:y"}])", "ai:x", false},
    {"lock and unlock", R"([{"op": "lock_page", "page": 2}, {"op": "set_note", "page": 2, "note": "n"},
                           {"op": "unlock_page", "page": 2}])", "ai:x", false},
    {"person takes an AI's lock", R"([{"op": "lock_page", "page": 1, "agent": "ai:z"}, {"op": "lock_page", "page": 1, "agent": "human:a"},
                                     {"op": "unlock_page", "page": 1}])", "genko", false},
    // (a page that is not there: refused here, where Python locks nothing; test_ops, test_command_bus)
    {"lock a page given as text", R"([{"op": "lock_page", "page": "2"}])", "ai:x", false},
    {"set_autosave", R"([{"op": "set_autosave"}, {"op": "set_autosave", "enabled": 0}])", "genko", false},
    {"add_page after", R"([{"op": "add_page", "count": 2, "after": 1}])", "genko", false},
    {"add_page", R"([{"op": "add_page"}])", "genko", false},
    {"add_page after 0", R"([{"op": "add_page", "after": 0}])", "genko", false},
    {"add_page after float", R"([{"op": "add_page", "after": 1.5}])", "genko", false},
    {"add_page bad count", R"([{"op": "add_page", "count": 0}])", "genko", false},
    {"add_page count text", R"([{"op": "add_page", "count": "many"}])", "genko", false},
    {"add_page no page", R"([{"op": "add_page", "after": 9}])", "genko", false},
    {"delete_page", R"([{"op": "delete_page", "page": 1}])", "genko", false},
    {"delete_page twice", R"([{"op": "delete_page", "page": 2}, {"op": "delete_page", "page": 1}])", "genko", false},
    {"delete_page no page", R"([{"op": "delete_page", "page": 9}])", "genko", false},
    {"add then delete", R"([{"op": "add_page", "count": 3}, {"op": "delete_page", "page": 2}, {"op": "set_note", "page": 4, "note": "4"}])",
     "genko", false},
    {"unknown op", R"([{"op": "frobnicate", "page": 1}])", "genko", false},
    {"op missing", R"([{"page": 1}])", "genko", false},
    {"not an object", R"([{"op": "set_autosave"}, 5])", "genko", false},
    {"not a list", R"({"op": "set_autosave"})", "genko", false},
    {"undo", R"([{"op": "undo"}])", "genko", false},
    {"undo dry run", R"([{"op": "undo"}])", "genko", true},
    {"dry run", R"([{"op": "set_note", "page": 1, "note": "dry"}, {"op": "add_page"}])", "genko", true},
    {"failure after changes", R"([{"op": "set_note", "page": 1, "note": "x"}, {"op": "add_page"}, {"op": "set_note", "page": 9}])",
     "genko", false},
    {"onion source string", R"([{"op":"set_onion","page":2,"from":"1"}])", "genko", false},
    {"onion source float", R"([{"op":"set_onion","page":2,"from":1.9}])", "genko", false},
    {"onion source bool", R"([{"op":"set_onion","page":2,"from":true}])", "genko", false},
    {"onion source self", R"([{"op":"set_onion","page":2,"from":2}])", "genko", false},
    {"onion source outside", R"([{"op":"set_onion","page":2,"from":-8}])", "genko", false},
    {"onion clear absent", R"([{"op":"set_onion","page":2,"from":1},{"op":"set_onion","page":2}])", "genko", false},
    {"onion clear null", R"([{"op":"set_onion","page":2,"from":1},{"op":"set_onion","page":2,"from":null}])", "genko", false},
    {"onion clear empty", R"([{"op":"set_onion","page":2,"from":1},{"op":"set_onion","page":2,"from":""}])", "genko", false},
    {"onion clear false", R"([{"op":"set_onion","page":2,"from":1},{"op":"set_onion","page":2,"from":false}])", "genko", false},
    {"onion zero text is int", R"([{"op":"set_onion","page":2,"from":"0"}])", "genko", false},
    {"onion invalid list atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_onion","page":2,"from":[]}])", "genko", false},
    {"onion invalid text atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_onion","page":2,"from":"bad"}])", "genko", false},
    {"onion no page", R"([{"op":"set_onion","page":9,"from":1}])", "genko", false},
    {"onion locked page", R"([{"op":"lock_page","page":2,"agent":"ai:other"},{"op":"set_onion","page":2,"from":1}])", "genko", false},
    {"onion dry run", R"([{"op":"set_onion","page":2,"from":1}])", "ai:mine", true},
    {"onion step default", R"([{"op":"step_onion","page":2}])", "genko", false},
    {"onion step zero defaults", R"([{"op":"step_onion","page":2,"delta":0}])", "genko", false},
    {"onion step zero text", R"([{"op":"step_onion","page":2,"delta":"0"}])", "genko", false},
    {"onion step fractional", R"([{"op":"step_onion","page":2,"delta":-0.9}])", "genko", false},
    {"onion step wide plus", R"([{"op":"step_onion","page":1,"delta":1e100}])", "genko", false},
    {"onion step wide minus", R"([{"op":"step_onion","page":2,"delta":"-9999999999999999999999999999"}])", "genko", false},
    {"onion step cancellation", R"([{"op":"set_onion","page":1,"from":-9223372036854775808},{"op":"step_onion","page":1,"delta":"9223372036854775810"}])", "genko", false},
    {"onion step after source", R"([{"op":"set_onion","page":2,"from":1},{"op":"step_onion","page":2,"delta":1}])", "genko", false},
    {"onion step invalid atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"step_onion","page":2,"delta":{"bad":1}}])", "genko", false},
    {"onion step dry run", R"([{"op":"step_onion","page":2,"delta":100}])", "ai:mine", true},
    {"lt finite", R"([{"op":"set_lt","page":2,"threshold":0.25}])", "genko", false},
    {"lt negative no clamp", R"([{"op":"set_lt","page":2,"threshold":"-17.25"}])", "genko", false},
    {"lt finite large", R"([{"op":"set_lt","page":2,"threshold":1e308}])", "genko", false},
    {"lt finite large text", R"([{"op":"set_lt","page":2,"threshold":"1e308"}])", "genko", false},
    {"lt zero float", R"([{"op":"set_lt","page":2,"threshold":0}])", "genko", false},
    {"lt bool", R"([{"op":"set_lt","page":2,"threshold":false},{"op":"set_lt","page":1,"threshold":true}])", "genko", false},
    {"lt overwrite", R"([{"op":"set_lt","page":2,"threshold":5},{"op":"set_lt","page":2,"threshold":0.5}])", "genko", false},
    {"lt missing atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2}])", "genko", false},
    {"lt null atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2,"threshold":null}])", "genko", false},
    {"lt list atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2,"threshold":[]}])", "genko", false},
    {"lt dict atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2,"threshold":{"bad":1}}])", "genko", false},
    {"lt invalid text atomic", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2,"threshold":"wrong"}])", "genko", false},
    {"lt wrong page", R"([{"op":"set_lt","page":9,"threshold":0.25}])", "genko", false},
    {"lt dry run", R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_lt","page":2,"threshold":0.25}])", "genko", true},
    {"lt existing other lock", R"([{"op":"lock_page","page":2,"agent":"ai:other"},{"op":"set_lt","page":2,"threshold":0.25}])", "genko", false},
};

}  // namespace

class TestContractSave : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run py(const QStringList& args) { return genko::test::harness(args, path("pyenv")); }
    Json py_json(const QStringList& args) {
        const auto r = py(args);
        if (!r.finished || r.exit_code != 0) return Json("python failed: " + r.err.right(2000).toStdString());
        return genko::test::one_line(r.out);
    }
    QString harness_script() const { return genko::test::repo_root() + "/tools/migration/pyref_harness.py"; }
    QProcessEnvironment pyenv() { return genko::test::python_env(path("pyenv")); }

    // `python -m genko <args>` (the baseline's own command line).
    genko::test::Run py_cli(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args, pyenv());
    }

    Json py_snapshot(const QString& book) {
        const auto r = py({"snapshot", book, "--full"});
        if (!r.finished || r.exit_code != 0) return Json("python failed: " + r.err.right(2000).toStdString());
        return genko::core::parse_python_json(r.out.toStdString());
    }

    Json cpp_snapshot(const QString& book) {
        const auto r = genko::test::run_genko({"inspect", book, "--full"});
        return genko::test::one_line(r.out);
    }

    void expect_same(const Json& got, const Json& want, const std::string& what) {
        std::string where;
        QVERIFY2(genko::test::strict_equal(got, want, &where), (what + ": " + where).c_str());
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void pythonLockStopsCpp() {
        const QString v4 = path("held-v4.genko");
        QCOMPARE(genko::test::run_genko({"new", v4, "--pages", "2"}).exit_code, 0);
        const QString v3 = path("held-v3.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), v3);
        genko::test::Holder python_v4;
        QCOMPARE(python_v4.start(genko::test::python_ref(), {harness_script(), "lock-hold", v4, "--agent", "ai:python"}, pyenv()),
                 QString("locked"));
        // in this process
        genko::storage::ProjectLock mine(to_path(v4), "human:cpp");
        try {
            mine.try_acquire();
            QFAIL("took a lock Python holds");
        } catch (const genko::storage::LockedError& error) {
            QCOMPARE(std::string(error.what()), "project locked: " + v4.toStdString() + "/project.lock (by ai:python)");
        }
        // the command line
        genko::test::write_bytes(path("note.json"), R"([{"op": "set_note", "page": 1, "note": "x"}])");
        const auto before = genko::test::tree_hashes(v4, false);
        for (const QStringList& args : {QStringList{"apply", v4, path("note.json")}, QStringList{"undo", v4},
                                        QStringList{"redo", v4}, QStringList{"gc", v4}}) {
            const auto r = genko::test::run_genko(args);
            QCOMPARE(r.exit_code, 1);
            const Json out = genko::test::one_line(r.out);
            QCOMPARE(out["code"], Json("locked"));
            QVERIFY2(out["error"].get<std::string>().ends_with("(by ai:python)"), r.out.constData());
        }
        QVERIFY(genko::test::tree_hashes(v4, false) == before);
        // converting an old book Python holds
        genko::test::Holder python_v3;
        QCOMPARE(python_v3.start(genko::test::python_ref(), {harness_script(), "lock-hold", v3, "--agent", "ai:python"}, pyenv()),
                 QString("locked"));
        const auto source = genko::test::tree_hashes(v3);
        const auto converting = genko::test::run_genko({"migrate", v3, path("held-new.genko")});
        QCOMPARE(converting.exit_code, 1);
        QCOMPARE(genko::test::one_line(converting.out)["code"], Json("source_locked"));
        QVERIFY(!QFileInfo::exists(path("held-new.genko")));
        QVERIFY(genko::test::tree_hashes(v3) == source);  // (not even project.lock: the converter writes nothing)
        QVERIFY(python_v3.stop().contains("released"));
        QVERIFY(python_v4.stop().contains("released"));
        // free again
        mine.try_acquire();
        QVERIFY(mine.held());
        mine.release();
        QCOMPARE(genko::test::run_genko({"migrate", v3, path("held-new.genko")}).exit_code, 0);
    }

    void cppLockStopsPython() {
        const QString dir = path("cpp-held.genko");
        genko::storage::ProjectLock mine(to_path(dir), "human:cpp");
        mine.try_acquire();
        QCOMPARE(py_json({"lock-try", dir, "--agent", "ai:py"}),
                 Json::object({{"ok", false}, {"error", "project locked: " + dir.toStdString() + "/project.lock (by human:cpp)"}}));
        mine.release();
        QCOMPARE(py_json({"lock-try", dir, "--agent", "ai:py"}), Json::object({{"ok", true}}));  // (our "released" is understood)
        genko::test::Holder helper;
        QCOMPARE(helper.start(genko::test::lock_helper(), {dir, "human:helper"}), QString("locked"));
        QCOMPARE(py_json({"lock-try", dir, "--agent", "ai:py"})["error"],
                 Json("project locked: " + dir.toStdString() + "/project.lock (by human:helper)"));
        helper.stop();
        // Python's content (written by its lock) is read by ours
        genko::test::Holder python;
        QCOMPARE(python.start(genko::test::python_ref(), {harness_script(), "lock-hold", dir, "--agent", "human:作者"}, pyenv()),
                 QString("locked"));
        const Json content = genko::storage::ProjectLock::read_content(to_path(dir) / "project.lock");
        QCOMPARE(content["agent"], Json("human:作者"));
        QVERIFY(content["token"].is_string() && content["pid"].is_number_integer() && content["acquired_at"].is_number_float());
        python.stop();
        QCOMPARE(genko::storage::ProjectLock::read_content(to_path(dir) / "project.lock")["released"], Json(true));
    }

    void opsMatchPython() {
        const QString book = genko::test::test_data("legacy/book-v3.genko");
        const auto digit_batch = [](const char* name, const char* key, const std::string& value) {
            Json op = Json::object({{"op", name}, {"page", 2}});
            op[key] = value;
            return genko::core::dump_python(Json::array({
                Json::object({{"op", "set_note"}, {"page", 2}, {"note", "前置"}}), op}));
        };
        const std::string from_digits = digit_batch("set_onion", "from", std::string(4300, '0') + "1");
        const std::string delta_digits = digit_batch("step_onion", "delta", std::string(4301, '9'));
        const std::string nonzero_from_digits = digit_batch("set_onion", "from", std::string(4301, '9'));
        const std::string boundary_from = digit_batch("set_onion", "from", std::string(4299, '0') + "1");
        const std::string boundary_delta = digit_batch("step_onion", "delta", std::string(4299, '0') + "1");
        std::vector<Case> cases;
        for (const Case& c : kCases) cases.push_back(c);
        cases.push_back({"onion source digit limit atomic", from_digits.c_str(), "genko", false});
        cases.push_back({"onion delta digit limit atomic", delta_digits.c_str(), "genko", false});
        cases.push_back({"onion source nonzero digit limit atomic", nonzero_from_digits.c_str(), "genko", false});
        cases.push_back({"onion source digit boundary", boundary_from.c_str(), "genko", false});
        cases.push_back({"onion delta digit boundary", boundary_delta.c_str(), "genko", false});
        Json jobs = Json::array();
        int n = 0;
        for (const Case& c : cases) {
            Json job = Json::object();
            job["op"] = "apply";
            job["book"] = book.toStdString();
            job["ops"] = genko::core::parse_python_json(c.ops);
            job["agent"] = c.agent;
            job["dry_run"] = c.dry_run;
            job["ids"] = true;
            job["out"] = path(QStringLiteral("apply/%1.json").arg(n)).toStdString();
            job["dest"] = path(QStringLiteral("apply/%1.genko").arg(n)).toStdString();
            jobs.push_back(job);
            ++n;
        }
        QDir().mkpath(path("apply"));
        genko::test::write_bytes(path("apply/jobs.json"), genko::core::dump_python(jobs));
        const auto ran = py({"batch", path("apply/jobs.json")});
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(3000).constData());

        n = 0;
        for (const Case& c : cases) {
            const QString out = path(QStringLiteral("apply/%1").arg(n));
            const Json want = genko::test::read_json(out + ".json");
            Json got;
            genko::core::Document applied;
            {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                const auto loaded = genko::storage::load_document(to_path(book));
                try {
                    const auto result = genko::core::CommandBus(genko::render::ops_registry()).apply(loaded.document, genko::core::parse_python_json(c.ops),
                                                                        genko::core::Actor(c.agent), c.dry_run);
                    got = Json::object();
                    got["ok"] = true;
                    got["applied"] = result.applied;
                    got["snapshot"] = genko::storage::snapshot(result.doc);
                    got["job_id"] = genko::core::new_id();
                    if (result.has_warnings) got["warnings"] = result.warnings;
                    applied = result.doc;
                } catch (const genko::core::ApplyError& error) {
                    got = Json::object({{"ok", false}, {"error", error.what()}});
                }
            }
            expect_same(got, want, c.name);
            if (QTest::currentTestFailed()) return;
            if (got["ok"] == Json(true) && !c.dry_run) {
                const Json py_project = genko::test::read_json(out + ".genko/project.json");
                genko::storage::AssetStore store(to_path(out + ".cpp-assets"));
                const Json mine = as_v3(genko::storage::project_payload_v4(applied, store));
                expect_same(mine, without(py_project, "revision"), std::string(c.name) + " project.json");
                if (QTest::currentTestFailed()) return;
            }
            ++n;
        }
        QCOMPARE(n, static_cast<int>(cases.size()));
        qInfo("Compared %d cases, original 40 retained, metadata additions %d", n, n - 40);
    }

    void legacyUndoMatchesPython() {
        const QString python = path("undo-py.genko");
        const QString source = path("undo-src.genko");
        const QString cpp = path("undo-cpp.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), python);
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), source);
        const auto converted = genko::test::run_genko({"migrate", source, cpp, "--as", "human:作者"});
        QCOMPARE(converted.exit_code, 0);
        expect_same(cpp_snapshot(cpp), py_snapshot(python), "converted");

        struct Step {
            bool redo;
            const char* actor;
            bool force;
        };
        // (after the new edit, "rev" differs where the change is the new edit's: Python's revision goes back with
        // Undo, a v4 revision never does — compared only for the old history's changes)
        const auto run_steps = [&](const std::vector<Step>& steps, const char* stage, bool compare_rev = true) {
            int i = 0;
            for (const Step& step : steps) {
                const std::string what = std::string(stage) + " step " + std::to_string(i++) + (step.redo ? " redo " : " undo ") + step.actor;
                QStringList py_args{"restore", python, "--actor", step.actor};
                if (step.redo) py_args << "--redo";
                if (step.force) py_args << "--force";
                const Json want = py_json(py_args);
                QStringList cpp_args{step.redo ? "redo" : "undo", cpp, "--as", step.actor};
                if (step.force) cpp_args << "--force";
                const auto r = genko::test::run_genko(cpp_args);
                Json got = genko::test::one_line(r.out);
                Json expected = want;
                QCOMPARE(r.exit_code, want["ok"] == Json(true) ? 0 : 1);
                if (got["ok"] == Json(true)) {
                    QVERIFY(got["revision"].is_number_integer());
                    got.erase("revision");
                    if (!compare_rev) {
                        got.erase("rev");
                        expected.erase("rev");
                    }
                } else {
                    got.erase("code");
                }
                expect_same(got, expected, what + " reply");
                if (QTest::currentTestFailed()) return;
                expect_same(cpp_snapshot(cpp), py_snapshot(python), what + " book");
                if (QTest::currentTestFailed()) return;
                QCOMPARE(genko::test::book_problems(cpp), std::string());
            }
        };
        // right after the conversion: the old history of edits, an Undo and a Redo, by a person and an AI
        run_steps({{false, "ai:hermes", false},
                   {false, "ai:hermes", false},
                   {false, "ai:hermes", true},  // (forced: still not an AI's to undo an approval)
                   {false, "human:作者", false},
                   {true, "ai:hermes", false},
                   {true, "human:作者", false},
                   {true, "human:作者", false},
                   {true, "human:作者", false},
                   {false, "ai:hermes", false},
                   {false, "human:作者", false},
                   {false, "human:作者", false},
                   {false, "human:作者", false}},
                  "converted");
        if (QTest::currentTestFailed()) return;
        // redo back to the top, then a new edit on both, then Undo through it into the old history
        run_steps({{true, "human:作者", false}, {true, "human:作者", false}, {true, "human:作者", false}}, "redo");
        if (QTest::currentTestFailed()) return;
        genko::test::write_bytes(path("undo-note.json"), R"([{"op": "set_note", "page": 1, "note": "変換後の編集"}])");
        const auto py_edit = py_cli({"apply", python, path("undo-note.json"), "--agent", "human:作者"});
        QVERIFY2(py_edit.exit_code == 0, py_edit.out.constData());
        const auto cpp_edit = genko::test::run_genko({"apply", cpp, path("undo-note.json"), "--agent", "human:作者"});
        QVERIFY2(cpp_edit.exit_code == 0, cpp_edit.out.constData());
        expect_same(cpp_snapshot(cpp), py_snapshot(python), "after the new edit");
        run_steps({{false, "ai:hermes", false},
                   {false, "human:作者", false},
                   {false, "human:作者", false},
                   {false, "human:作者", false},
                   {true, "human:作者", false},
                   {true, "human:作者", false}},
                  "after the edit", false);
        if (QTest::currentTestFailed()) return;
        const Json project = genko::test::read_json(cpp + "/project.json");
        QCOMPARE(project["version"], Json(4));
        QVERIFY(project["revision"].get<std::int64_t>() > 10);
    }

    void convertedBooksReadAsPythonReadsTheOriginals() {
        for (const char* version : {"v1", "v2", "v3"}) {
            const QString source = genko::test::test_data(QStringLiteral("legacy/book-%1.genko").arg(version));
            const QString cpp = path(QStringLiteral("read-%1.genko").arg(version));
            const auto r = genko::test::run_genko({"migrate", source, cpp});
            QVERIFY2(r.exit_code == 0, r.out.constData());
            const Json mine = cpp_snapshot(cpp);
            const Json theirs = py_snapshot(source);
            if (std::string(version) == "v1") {
                expect_same(without_layer_ids(mine), without_layer_ids(theirs), version);
            } else {
                expect_same(mine, theirs, version);
            }
        }
    }

    void upgradedV2HistoryStaysReachable() {
        const QString book = path("upgraded.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v2.genko"), book);
        const auto upgraded = py({"upgrade", book});
        QVERIFY2(upgraded.exit_code == 0, upgraded.err.constData());
        QVERIFY(QFileInfo::exists(book + "/project.v2.bak.json"));
        const QString cpp = path("upgraded-cpp.genko");
        const auto r = genko::test::run_genko({"migrate", book, cpp});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        const Json report = genko::test::one_line(r.out);
        QCOMPARE(report["history"]["consistent"], Json(true));
        QCOMPARE(report["map_entries"], Json(1));
        QCOMPARE(report["assets"]["pages"].size(), std::size_t{1});
        const std::string bg = genko::test::read_bytes(book + "/pages/001/bg.png");
        QCOMPARE(report["assets"]["pages"][0]["ref"].get<std::string>(), genko::storage::AssetStore::ref(bg));
        QCOMPARE(genko::test::read_bytes(cpp + "/legacy/project.v2.bak.json"), genko::test::read_bytes(book + "/project.v2.bak.json"));
        // Undo the upgrade: the v2 book, its picture from pages/ now an asset with the same bytes
        QCOMPARE(genko::test::run_genko({"undo", cpp}).exit_code, 0);
        const auto loaded = genko::storage::load_document(to_path(cpp));
        QVERIFY(loaded.report.clean());
        const auto& page = loaded.document.page(0);
        const auto bg_layer = std::find_if(page.layers.begin(), page.layers.end(),
                                           [](const auto& layer) { return layer.role == genko::core::LayerRole::Bg; });
        QVERIFY(bg_layer != page.layers.end() && bg_layer->raster_png);
        QCOMPARE(*bg_layer->raster_png, bg);
        const QString python = path("upgraded-py.genko");
        genko::test::copy_tree(book, python);
        QCOMPARE(py_json({"restore", python})["ok"], Json(true));
        expect_same(cpp_snapshot(cpp), py_snapshot(python), "the v2 book back");  // (v2 layers keep their ids)
        QCOMPARE(genko::test::run_genko({"redo", cpp}).exit_code, 0);
        QCOMPARE(genko::test::book_problems(cpp), std::string());
    }

    void newMatchesPython() {
        // the same paper, title, pages and panels; only the ids made at random differ
        const auto without_ids = [](Json snapshot) {
            for (Json& page : snapshot["pages"]) {
                for (Json& leaf : page["leaves"]) leaf["id"] = "x";
                for (Json& layer : page["layers"]) layer["id"] = "x";
            }
            return snapshot;
        };
        const std::vector<QStringList> variants{{}, {"--b4", "--pages", "3", "--title", "題名", "--episode", "2"},
                                                {"--paper", "b5"}, {"--paper", "webtoon", "--pages", "1"}, {"--webtoon"},
                                                {"--preset", "Shueisha"}, {"--plain"}};
        int n = 0;
        for (const QStringList& variant : variants) {
            const QString py_dest = path(QStringLiteral("new-py-%1.genko").arg(n));
            const QString cpp_dest = path(QStringLiteral("new-cpp-%1.genko").arg(n));
            ++n;
            const auto py = py_cli(QStringList{"new", py_dest} + variant);
            const auto cpp = genko::test::run_genko(QStringList{"new", cpp_dest} + variant);
            QCOMPARE(cpp.exit_code, py.exit_code);
            if (variant.contains("--plain")) {
                QCOMPARE(cpp.out, cpp_dest.toUtf8() + "\n");
                QCOMPARE(py.out, py_dest.toUtf8() + "\n");
                continue;
            }
            const Json mine = genko::test::one_line(cpp.out);
            const Json theirs = genko::test::one_line(py.out);
            QCOMPARE(mine["path"].get<std::string>(), cpp_dest.toStdString());
            expect_same(without_ids(mine["snapshot"]), without_ids(theirs["snapshot"]), variant.join(' ').toStdString());
            if (QTest::currentTestFailed()) return;
            const Json project = genko::test::read_json(cpp_dest + "/project.json");
            QCOMPARE(project["revision"], Json(1));
            QCOMPARE(genko::test::book_problems(cpp_dest), std::string());
        }
        // a wrong paper: Python's message, exit 1, nothing made
        const auto wrong = genko::test::run_genko({"new", path("new-wrong.genko"), "--paper", "a3"});
        QCOMPARE(wrong.exit_code, 1);
        QVERIFY(wrong.err.contains("--paper must be one of b4, b5, a5, a4, webtoon"));
        QCOMPARE(py_cli({"new", path("new-wrong-py.genko"), "--paper", "a3"}).exit_code, 1);
        QVERIFY(!QFileInfo::exists(path("new-wrong.genko")));
        // never over a book
        const auto again = genko::test::run_genko({"new", path("new-cpp-0.genko")});
        QCOMPARE(again.exit_code, 1);
        QCOMPARE(genko::test::one_line(again.out)["code"], Json("exists"));
    }

    void oldWriterWhileConverting() {
        if (!genko::test::fault_injection()) QSKIP("the hook that runs Python mid-copy is only in builds with fault injection");
        genko::test::write_bytes(path("writer-ops.json"), R"([{"op": "set_note", "page": 2, "note": "Python が書いた"}])");
        const auto hook_env = [&](const QString& source, const QString& tag, bool gc_only) {
            const QString python = genko::test::python_ref();
            QString command;
            if (!gc_only) {
                command += "'" + python + "' -m genko apply '" + source + "' '" + path("writer-ops.json") + "' --agent ai:old > '" +
                           path(tag + "-apply.out") + "' 2>&1; ";
            }
            command += "'" + python + "' -m genko gc '" + source + "' > '" + path(tag + "-gc.out") + "' 2>&1";
            QProcessEnvironment env = pyenv();
            env.insert("GENKO_TEST_MIGRATE_HOOK", QString::fromStdString(genko::core::dump_python(
                                                      Json::array({"/bin/sh", "-c", command.toStdString()}))));
            return env;
        };
        // (a) the book has project.lock (Python has written it before): Python waits its turn, here it fails
        const QString locked = path("writer-locked.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), locked);
        QCOMPARE(py_json({"lock-try", locked, "--agent", "ai:old"}), Json::object({{"ok", true}}));
        const auto before = genko::test::tree_hashes(locked);
        auto r = genko::test::run_genko({"migrate", locked, path("writer-locked-new.genko")}, hook_env(locked, "a", false));
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QVERIFY(genko::test::read_bytes(path("a-apply.out")).find("project locked:") != std::string::npos);
        QVERIFY(genko::test::read_bytes(path("a-gc.out")).find("project locked:") != std::string::npos);
        QVERIFY(genko::test::tree_hashes(locked) == before);
        // (b) no project.lock: the converter makes none, Python writes, the conversion stops and publishes nothing
        const QString open = path("writer-open.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), open);
        r = genko::test::run_genko({"migrate", open, path("writer-open-new.genko")}, hook_env(open, "b", false));
        QCOMPARE(r.exit_code, 1);
        const Json out = genko::test::one_line(r.out);
        QCOMPARE(out["code"], Json("source_changed"));
        QVERIFY(genko::test::read_bytes(path("b-apply.out")).find("\"ok\": true") != std::string::npos);
        QVERIFY(!QFileInfo::exists(path("writer-open-new.genko")));
        QCOMPARE(QDir(scratch_.path()).entryList({".writer-open-new.genko.migrating-*"}, QDir::AllEntries | QDir::Hidden).size(), 0);
        // (c) a Python GC that only takes the lock (no asset is old enough to go) changes project.lock, not the book
        const QString gc = path("writer-gc.genko");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), gc);
        r = genko::test::run_genko({"migrate", gc, path("writer-gc-new.genko")}, hook_env(gc, "c", true));
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QVERIFY(genko::test::read_bytes(path("c-gc.out")).find("\"ok\": true") != std::string::npos);
        QVERIFY(QFileInfo::exists(gc + "/project.lock"));
    }
};

QTEST_GUILESS_MAIN(TestContractSave)
#include "test_contract_save.moc"
