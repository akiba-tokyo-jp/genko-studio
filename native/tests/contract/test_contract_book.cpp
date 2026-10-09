// The book and page ops of M4 (Python's ops._apply_one with pagespec.py, bookops.py and merge.py: set_page_spec,
// set_spread, add_cover, for_pages, set_assignee, import_pages) against Python's apply_ops: the cases of
// contract/book_cases.json on the books `pyref_harness.py make-pagesbook` makes (the op book with briefs' regions, 3D
// guides, effect lines, an old tone's region, a mask and a hand-drawn balloon besides, and a book to take pages from),
// each step's reply, full snapshot, project.json payload and what covers.py makes of the book (pages_in_order,
// reading_order, file_stem, folds), and the book after the case saved and read back. import_pages also reads the
// legacy books (data/legacy) and books this test makes that cannot be read (none there, an empty folder, broken JSON, a
// newer version, JSON that is not a book, a book missing a picture). A case marked "cpp" is one this build refuses on
// purpose (a paper that is not a finite number, pixels moved onto a basic frame of no finite size or onto a paper too
// large to hold them, a book it would open read-only, a version only this build reads): C++ must refuse it with that
// code, its error holding "cpp_says". The command line copies none of the other book's assets with import_pages, as
// Python's does (its app and MCP tools copy them first, merge.copy_assets): checked on both command lines. Skipped
// without the Python reference.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "opsupport.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

// The steps of a case (ops_cases.json's format): "steps", or one step of "ops", "agent", "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

// Every text of `value` with the placeholders ($OTHER, $PAGESBOOK, $LEGACY, $SCRATCH) put in.
Json placed(const Json& value, const std::map<std::string, std::string>& paths) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (const auto& [key, path] : paths) {
            for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + path.size())) {
                text.replace(at, key.size(), path);
            }
        }
        return text;
    }
    Json out = value;
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) out[i] = placed(value[i], paths);
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) out[key] = placed(item, paths);
    }
    return out;
}

// What covers.py makes of the book (the harness's _covers_of).
Json covers_of(const genko::core::Document& doc) {
    Json order = Json::array();
    for (const genko::core::Page* page : genko::core::pages_in_order(doc)) order.push_back(page->index.json());
    Json reading = Json::array();
    for (const auto& [page, part] : genko::core::reading_order(doc)) reading.push_back(Json::array({page->index.json(), part}));
    Json stems = Json::array();
    for (const auto& page : doc.pages) stems.push_back(genko::core::file_stem(*page));
    Json folds = Json::object();
    for (const char* binding : {"right", "left"}) {
        Json pages = Json::array();
        for (const auto& page : doc.pages) {
            Json parts = Json::array();
            for (const genko::core::Fold& fold : genko::core::folds(*page, binding)) {
                parts.push_back(Json::array({fold.x0, fold.x1, fold.name}));
            }
            pages.push_back(std::move(parts));
        }
        folds[binding] = std::move(pages);
    }
    return Json::object({{"order", order}, {"reading", reading}, {"stems", stems}, {"folds", folds}});
}

}  // namespace

class TestContractBook : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString pages_;
    QString other_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    // Books import_pages cannot read (or reads, where this build does not): under <scratch>/scratch.
    void write_unreadable_books() {
        const QString dir = path("scratch");
        QDir().mkpath(dir + "/empty.genko");
        QDir().mkpath(dir + "/broken.genko");
        QDir().mkpath(dir + "/newer.genko");
        QDir().mkpath(dir + "/notbook.genko");
        genko::test::write_bytes(dir + "/broken.genko/project.json", "{");
        genko::test::write_bytes(dir + "/newer.genko/project.json", R"({"version": 99, "title": "x", "pages": []})");
        genko::test::write_bytes(dir + "/notbook.genko/project.json", "[]");
        // the other book without the picture of its paint layer (page 2's o-paint)
        genko::test::copy_tree(other_, dir + "/damaged.genko");
        const Json payload = genko::test::read_json(other_ + "/project.json");
        for (const Json& layer : payload["pages"][1]["layers"]) {
            if (layer["id"] != Json("o-paint")) continue;
            const std::string rel = genko::storage::AssetStore::relpath(layer["asset"].get<std::string>(), ".png");
            QVERIFY(QFile::remove(dir + "/damaged.genko/" + QString::fromStdString(rel)));
        }
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        pages_ = path("pages.genko");
        other_ = path("other.genko");
        const auto made = genko::test::harness({"make-pagesbook", pages_, other_}, path("pyenv"), 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        write_unreadable_books();
    }

    void bookOpsLikePython() {
        const std::map<std::string, std::string> paths{
            {"$OTHER", other_.toStdString()},
            {"$PAGESBOOK", pages_.toStdString()},
            {"$LEGACY", genko::test::test_data(QStringLiteral("legacy")).toStdString()},
            {"$SCRATCH", path("scratch").toStdString()}};
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/book_cases.json");
        const Json cases = placed(file["cases"], paths);
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        const QString dir = path("ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = pages_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            job["covers"] = true;
            if (cases[n]["ok"].get<bool>() && !cases[n].contains("cpp")) {
                job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            }
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, path("pyenv"), 1800000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(pages_.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, std::pair<int, int>> same;  // op → (successes, refusals) the same as Python's
        int read_back = 0;
        int refused_here = 0;
        int covers_compared = 0;
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const std::string op = c["op"].get<std::string>();
            const bool expect_ok = c["ok"].get<bool>();
            Json records = genko::test::read_json(QString::fromStdString(jobs[n]["out"].get<std::string>()));
            Json reread;
            if (!records.empty() && records.back().contains("reread")) {
                reread = records.back();
                records.erase(records.size() - 1);
            }
            genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-store-%1").arg(n)).toStdString()));
            genko::core::Document last;
            std::vector<Json> covers;  // what covers.py makes of the book after each step, on this side
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &last,
                                                         [&covers](genko::core::Document& doc, std::size_t, const genko::test::StepOutcome&) {
                                                             covers.push_back(covers_of(doc));
                                                         });
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            // the case does what it says: its last step succeeds or fails in Python as marked, and a failure is the op's
            // own (Python's traceback aside)
            const Json& last_reply = records.back()["reply"];
            if (last_reply["ok"].get<bool>() != expect_ok) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(last_reply).substr(0, 400));
                continue;
            }
            if (!expect_ok && !last_reply.contains("uncaught")) {
                const std::string error = last_reply["error"].get<std::string>();
                if (error.find("] " + op + ": ") == std::string::npos) {
                    failures.push_back(name + ": fails in another op: " + error.substr(0, 300));
                    continue;
                }
            }
            if (c.contains("cpp")) {  // refused here on purpose
                const auto& final = outcomes.back();
                const std::string error = final.reply.value("error", std::string());
                if (final.code != c["cpp"].get<std::string>() || error.find(c["cpp_says"].get<std::string>()) == std::string::npos) {
                    failures.push_back(name + ": C++ should refuse it with " + c["cpp"].get<std::string>() + " (" +
                                       c["cpp_says"].get<std::string>() + "), gave " +
                                       genko::core::dump_python(final.reply).substr(0, 400) + " (" + final.code + ")");
                    continue;
                }
                ++refused_here;
                continue;
            }
            bool ok = true;
            for (std::size_t s = 0; s < outcomes.size() && ok; ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    ok = false;
                    continue;
                }
                const std::string want = genko::core::dump_python(records[s]["covers"]);
                const std::string got = genko::core::dump_python(covers[s]);
                if (got != want) {
                    failures.push_back(name + " step " + std::to_string(s) + ": covers " + got.substr(0, 600) + " != " +
                                       want.substr(0, 600));
                    ok = false;
                    continue;
                }
                ++covers_compared;
            }
            if (!ok) continue;
            (expect_ok ? same[op].first : same[op].second) += 1;
            if (!expect_ok) continue;
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [op, n] : same) qInfo("%s: %d successes and %d refusals the same as Python", op.c_str(), n.first, n.second);
        qInfo("saved and read back as they were: %d; refused here on purpose: %d; covers compared after %d steps", read_back,
              refused_here, covers_compared);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        const std::map<std::string, std::pair<int, int>> needed{
            {"set_page_spec", {36, 26}}, {"set_spread", {19, 13}},   {"add_cover", {14, 20}},
            {"for_pages", {6, 5}},       {"set_assignee", {16, 11}}, {"import_pages", {23, 14}}};
        for (const auto& [op, least] : needed) {
            const auto got = same[op];
            QVERIFY2(got.first >= least.first && got.second >= least.second,
                     (op + ": " + std::to_string(got.first) + "/" + std::to_string(got.second) + " matched, " +
                      std::to_string(least.first) + "/" + std::to_string(least.second) + " needed").c_str());
        }
        QVERIFY(refused_here >= 8);
    }

    // `genko apply` with import_pages copies none of the other book's asset files, as Python's `genko apply` does (its
    // app and MCP tools copy them before the op, merge.copy_assets: storage::copy_assets here): the page taken in refers
    // to the other book's placed picture, which neither book saved by the command line has.
    void commandLineCopiesNoAssets() {
        const Json payload = genko::test::read_json(other_ + "/project.json");
        std::string art;
        for (const Json& layer : payload["pages"][0]["layers"]) {
            if (layer.value("kind", "") == "placed") art = layer["asset"].get<std::string>();
        }
        QVERIFY(!art.empty());
        const QString ops = path("import.json");
        genko::test::write_bytes(ops, genko::core::dump_python(Json::array({Json{{"op", "import_pages"}, {"from", other_.toStdString()},
                                                                                  {"pages", Json::array({1})}}})));
        // this build, on the book converted to v4
        const QString v4 = path("cli.genko");
        const auto converted = genko::test::run_genko({"migrate", pages_, v4});
        QVERIFY2(converted.exit_code == 0, (converted.out + converted.err).constData());
        const genko::storage::AssetStore store(genko::storage::path_from_utf8(v4.toStdString()));
        QVERIFY(!store.has(art, ".png"));
        for (const bool dry : {true, false}) {
            const auto applied = dry ? genko::test::run_genko({"apply", v4, ops, "--dry-run"}) : genko::test::run_genko({"apply", v4, ops});
            QVERIFY2(applied.exit_code == 0, (applied.out + applied.err).constData());
            QVERIFY(!store.has(art, ".png"));
        }
        const auto again = genko::storage::load_document(genko::storage::path_from_utf8(v4.toStdString()));
        QCOMPARE(again.document.pages.size(), std::size_t(8));
        bool refers = false;
        for (const auto& layer : again.document.pages[6]->layers) refers = refers || (layer.asset && *layer.asset == art);
        QVERIFY(refers);  // (the page went in before the front cover)
        // Python's command line, on a copy of the v3 book
        const QString v3 = path("py-cli.genko");
        genko::test::copy_tree(pages_, v3);
        const genko::storage::AssetStore py_store(genko::storage::path_from_utf8(v3.toStdString()));
        QVERIFY(!py_store.has(art, ".png"));
        const auto py = genko::test::run(genko::test::python_ref(), {"-m", "genko", "apply", v3, ops},
                                         genko::test::python_env(path("pyenv")));
        QVERIFY2(py.finished && py.exit_code == 0, (py.out + py.err).constData());
        QVERIFY(genko::core::parse_python_json(py.out.toStdString()).value("ok", false));
        QVERIFY(!py_store.has(art, ".png"));
    }
};

QTEST_GUILESS_MAIN(TestContractBook)
#include "test_contract_book.moc"
