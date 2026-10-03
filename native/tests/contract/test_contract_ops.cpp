// The ops of M2-O1 against the Python baseline (src/genko run by the reference Python, tools/migration/
// pyref_harness.py):
//   1. fixed cases (contract/ops_cases.json, on the book `pyref_harness.py make-opsbook` makes): for each step the
//      reply (applied, snapshot, job_id, warnings, results — or the error's words), the full snapshot and the
//      project.json payload are the same as Python's apply_ops gives, ids counted the same on both sides. Every op of
//      M2-O1 has 10 or more cases that succeed and 5 or more that fail (undo: test_contract_ops_cli). A case marked
//      "cpp": "not_yet_ported" is a part of an op this build refuses on purpose (the colour mixing under a line): C++
//      must refuse it with not_yet_ported. (The pixels of a paint layer the erasers reach, and the areas the bus
//      resolves, come with the ops of M3-A1: render::ops_registry.)
//   2. random op sequences (300, made by `pyref_harness.py make-sequences` with a fixed seed: 1 to 12 ops each, with
//      for_pages, strict_gates, page locks and other actors) on 20 random books: each step the same as Python's. (The
//      sequences do not ask for what this build refuses where Python breaks the book: a reorder_layers names the layers
//      its page has when it runs. Should the ops before an op still make it one, the books go apart there: listed.)
//   4. saved and read back: the book after each successful fixed case, saved by storage::Saver and read again, has
//      the same full snapshot and payload as before the save (but for what Python does not keep either: the frame
//      selected, the layers of a page that has none, int margins) and as Python's book saved by save_episode and read
//      again.
// (3, the command line against `python -m genko apply`, and undo: test_contract_ops_cli.)
// Skipped when there is no reference Python ($GENKO_PYREF or /opt/pyref/bin/python).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/ids.hpp"
#include "opsupport.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

// The steps of a case: "steps", or one step made of "ops", "agent" and "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

// The 34 ops of M2-O1 (undo is compared through the command line: test_contract_ops_cli).
const char* const kOps[] = {"split_frame", "cut_frame",   "move_gutter",    "merge_frame",  "resize_frame", "set_frame",
                            "add_frame",   "delete_frame", "select_frame",  "add_page",     "delete_page",  "duplicate_page",
                            "reorder",     "advance",      "name_ok",       "lock_page",    "unlock_page",  "set_note",
                            "set_meta",    "set_autosave", "add_stroke",    "delete_stroke", "edit_stroke", "simplify_stroke",
                            "erase",       "erase_raster", "add_layer",     "delete_layer", "duplicate_layer", "set_layer",
                            "set_layers",  "reorder_layers", "set_brush"};

// An op this build refuses where Python goes on and breaks the book (a layer listed twice or lost, a lock on no page,
// a selection or a parent that is not there, page numbers read from a text): test_ops checks each.
bool refused_where_python_breaks(const genko::test::StepOutcome& cpp) {
    if (cpp.code != "apply") return false;
    const std::string error = cpp.reply.value("error", std::string());
    for (const char* words : {": ids must not repeat", ": order must list every layer of the page once", "lock_page: no page ",
                              "unlock_page: no page ", "select_frame: no frame ", ": parent must be a folder",
                              ": a folder cannot hold itself", "for_pages: pages must be all, body or a list of page numbers"}) {
        if (error.find(words) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

class TestContractOps : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString opsbook_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run py(const QStringList& args, int timeout_ms = 1200000) {
        return genko::test::harness(args, path("pyenv"), timeout_ms);
    }

    // Python's records of `jobs` ("steps" jobs), in order.
    std::vector<Json> python_steps(const Json& jobs, const QString& name) {
        genko::test::write_bytes(path(name + "-jobs.json"), genko::core::dump_python(jobs));
        const auto ran = py({"batch", path(name + "-jobs.json")});
        if (!ran.finished || ran.exit_code != 0) {
            qWarning("%s", ran.err.right(4000).constData());
            return {};
        }
        std::vector<Json> out;
        for (const Json& job : jobs) out.push_back(genko::test::read_json(QString::fromStdString(job["out"].get<std::string>())));
        return out;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        opsbook_ = path("opsbook.genko");
        const auto made = py({"make-opsbook", opsbook_});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
    }

    void fixedCases() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/ops_cases.json");
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        QDir().mkpath(path("fixed"));
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = opsbook_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = path(QStringLiteral("fixed/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = path(QStringLiteral("fixed/%1.json").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        const std::vector<Json> python = python_steps(jobs, "fixed");
        QCOMPARE(python.size(), cases.size());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(to_path(opsbook_));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, std::pair<int, int>> counts;  // op → (successes, failures) matched with Python
        int not_ported = 0;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const std::string op = c["op"].get<std::string>();
            const bool expect_ok = c["ok"].get<bool>();
            genko::storage::AssetStore store(to_path(path(QStringLiteral("fixed/cpp-store-%1").arg(n))));
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store);
            const Json& records = python[n];
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            // the case does what it says: its last step succeeds or fails in Python as marked, and a failure is the
            // op's own
            const Json& last = records.back()["reply"];
            if (last["ok"].get<bool>() != expect_ok) {
                failures.push_back(name + ": marked " + (expect_ok ? "ok" : "failing") + " but Python gave " +
                                   genko::core::dump_python(last).substr(0, 400));
                continue;
            }
            if (!expect_ok && !last.contains("uncaught") && !c.contains("other")) {
                const std::string error = last["error"].get<std::string>();
                if (error.find("] " + op + ": ") == std::string::npos) {
                    failures.push_back(name + ": fails in another op: " + error.substr(0, 300));
                    continue;
                }
            }
            if (c.contains("cpp")) {  // a part this build refuses on purpose
                const auto& final = outcomes.back();
                if (final.code != c["cpp"].get<std::string>()) {
                    failures.push_back(name + ": C++ should refuse it with " + c["cpp"].get<std::string>() + ", gave " +
                                       genko::core::dump_python(final.reply).substr(0, 400));
                }
                ++not_ported;
                continue;
            }
            bool same = true;
            for (std::size_t s = 0; s < outcomes.size(); ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    same = false;
                    break;
                }
            }
            if (same) (expect_ok ? counts[op].first : counts[op].second) += 1;
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        std::string summary;
        for (const char* op : kOps) {
            const auto [ok, failing] = counts[op];
            summary += std::string(op) + " " + std::to_string(ok) + "/" + std::to_string(failing) + ", ";
            QVERIFY2(ok >= 10, (std::string(op) + ": " + std::to_string(ok) + " successes match Python (10 needed)").c_str());
            QVERIFY2(failing >= 5, (std::string(op) + ": " + std::to_string(failing) + " failures match Python (5 needed)").c_str());
        }
        qInfo("matched (ok/failing): %s%d refused as not yet ported", summary.c_str(), not_ported);
    }

    void randomSequences() {
        const QString books = path("random");
        const auto made = py({"make-random", books, "--seed", "11", "--count", "20"});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const auto sequenced = py({"make-sequences", path("sequences.json"), "--books", books, "--seed", "5", "--count", "300"});
        QVERIFY2(sequenced.finished && sequenced.exit_code == 0, sequenced.err.right(4000).constData());
        const Json sequences = genko::test::read_json(path("sequences.json"));
        QCOMPARE(sequences.size(), std::size_t{300});
        QDir().mkpath(path("seq"));
        const auto job_of = [&](std::size_t n, bool digest) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = sequences[n]["book"];
            job["ids"] = true;
            job["first_id"] = sequences[n]["first_id"];
            job["store"] = path(QStringLiteral("seq/py-store-%1").arg(n)).toStdString();
            job["steps"] = sequences[n]["steps"];
            job["out"] = path(QStringLiteral("seq/%1%2.json").arg(n).arg(digest ? "" : "-full")).toStdString();
            job["digest"] = digest;
            return job;
        };
        Json jobs = Json::array();
        for (std::size_t n = 0; n < sequences.size(); ++n) jobs.push_back(job_of(n, true));
        const std::vector<Json> python = python_steps(jobs, "seq");
        QCOMPARE(python.size(), sequences.size());

        std::map<std::string, genko::core::Document> loaded;  // (each book read once)
        int steps = 0, ops = 0, stopped = 0, refused = 0, refused_earlier = 0, failed_steps = 0;
        std::set<std::string> books_used;
        std::vector<std::pair<std::size_t, std::string>> differ;
        std::vector<std::string> refusals;  // (where this build refuses what Python goes on with)
        for (std::size_t n = 0; n < sequences.size(); ++n) {
            const std::string book = sequences[n]["book"].get<std::string>();
            books_used.insert(book);
            if (!loaded.contains(book)) {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                loaded[book] = genko::storage::load_document(fs::path(book)).document;
            }
            genko::storage::AssetStore store(to_path(path(QStringLiteral("seq/cpp-store-%1").arg(n))));
            const auto outcomes = genko::test::run_steps(loaded[book], sequences[n]["steps"],
                                                         sequences[n]["first_id"].get<std::uint64_t>(), store, true);
            for (std::size_t s = 0; s < outcomes.size(); ++s) {
                const Json& record = python[n][s];
                // a part this build refuses on purpose (the colour under a line, what a page has that is not drawn yet): the
                // books go apart from here
                if (outcomes[s].code == "not_yet_ported" && record["reply"]["ok"] == Json(true)) {
                    ++stopped;
                    break;
                }
                // a refusal of this build where Python goes on (the ops before made it one): apart from here too; where
                // Python's batch fails at a later op, both books are as they were (only the books are compared)
                std::string diff;
                if (refused_where_python_breaks(outcomes[s])) {
                    refusals.push_back("sequence " + std::to_string(n) + " step " + std::to_string(s) + ": " +
                                       outcomes[s].reply.value("error", std::string()).substr(0, 200) +
                                       (record["reply"]["ok"] == Json(true) ? " (Python: applied)" : " (Python: refused later)"));
                    if (record["reply"]["ok"] == Json(true)) {
                        ++refused;
                        break;
                    }
                    std::string where;
                    if (!genko::test::strict_equal(outcomes[s].full, record["full"], &where)) diff = "full snapshot: " + where;
                    if (!genko::test::strict_equal(outcomes[s].payload, record["payload"], &where)) diff = "payload: " + where;
                    ++refused_earlier;
                } else {
                    diff = genko::test::compare_step(outcomes[s], record);
                }
                if (!diff.empty()) {
                    differ.emplace_back(n, "step " + std::to_string(s) + ": " + diff.substr(0, 800));
                    break;
                }
                ++steps;
                ops += static_cast<int>(sequences[n]["steps"][s]["ops"].size());
                if (record["reply"]["ok"] == Json(false)) ++failed_steps;
            }
        }
        // where they differ, the whole snapshots and payloads say where
        for (std::size_t k = 0; k < differ.size() && k < 4; ++k) {
            const std::size_t n = differ[k].first;
            const std::vector<Json> full = python_steps(Json::array({job_of(n, false)}), QStringLiteral("seq-full-%1").arg(n));
            std::string detail;
            if (!full.empty()) {
                genko::storage::AssetStore store(to_path(path(QStringLiteral("seq/cpp-full-store-%1").arg(n))));
                const auto outcomes = genko::test::run_steps(loaded[sequences[n]["book"].get<std::string>()],
                                                             sequences[n]["steps"], sequences[n]["first_id"].get<std::uint64_t>(),
                                                             store, false);
                for (std::size_t s = 0; s < outcomes.size() && detail.empty(); ++s) {
                    detail = genko::test::compare_step(outcomes[s], full.front()[s]);
                    if (!detail.empty()) {
                        detail = "step " + std::to_string(s) + " " + genko::core::dump_python(sequences[n]["steps"][s]).substr(0, 600) +
                                 ": " + detail.substr(0, 1500);
                    }
                }
            }
            differ[k].second += " | " + detail;
        }
        for (const auto& [n, why] : differ) qWarning("sequence %zu: %s", n, why.c_str());
        for (const std::string& refusal : refusals) qInfo("refused here, where Python breaks the book: %s", refusal.c_str());
        QVERIFY2(differ.empty(), (std::to_string(differ.size()) + " of 300 sequences differ from Python").c_str());
        QCOMPARE(books_used.size(), std::size_t{20});
        qInfo("%d steps (%d ops, %d of the steps refused by both) the same as Python; %d sequences stopped at a part not "
              "ported yet, %d at a refusal of this build where Python breaks the book (and %d steps refused by both, here "
              "at an earlier op of the batch: the books compared)",
              steps, ops, failed_steps, stopped, refused, refused_earlier);
        QVERIFY2(stopped <= 30, "too many sequences stop at a part not ported yet: the sequences test too little");
        QVERIFY2(refused <= 30, "too many sequences stop at a refusal of this build: the sequences test too little");
    }

    // 4. Each successful fixed case's book saved by storage::Saver as a new book and read back is the book before the
    //    save, but for what Python does not keep either: which frame is selected (not saved), a page left with no
    //    layers (read back with the four default layers, as Python's Page starts) and the int margins of a paper
    //    preset (read back as floats). It is also the same as Python's book after the case, saved with save_episode
    //    and read back.
    void savedAndReadBack() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/ops_cases.json");
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        QDir().mkpath(path("saved"));
        Json jobs = Json::array();
        std::vector<std::size_t> chosen;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            if (!cases[n]["ok"].get<bool>() || cases[n].contains("cpp")) continue;
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = opsbook_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = path(QStringLiteral("saved/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = path(QStringLiteral("saved/%1.json").arg(n)).toStdString();
            job["reread"] = path(QStringLiteral("saved/py-%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
            chosen.push_back(n);
        }
        const std::vector<Json> python = python_steps(jobs, "saved");
        QCOMPARE(python.size(), chosen.size());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(to_path(opsbook_));
        }();
        std::map<std::string, int> counts;  // op → cases saved and read back as they were
        int unselected = 0, refilled = 0, floated = 0;
        std::vector<std::string> failures;
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            const Json& c = cases[chosen[k]];
            const std::string name = c["n"].get<std::string>();
            genko::storage::AssetStore store(to_path(path(QStringLiteral("saved/cpp-store-%1").arg(chosen[k]))));
            genko::core::Document doc;
            genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &doc);
            genko::test::StepOutcome before = genko::test::state_of(doc, store);

            const fs::path dir = to_path(path(QStringLiteral("saved/cpp-%1.genko").arg(chosen[k])));
            fs::create_directories(dir);
            {
                genko::storage::ProjectLock lock(dir, "genko");
                lock.try_acquire();
                genko::storage::SaveRequest request;
                request.ops = Json::array();
                genko::storage::Saver(lock).save(doc, request);
            }
            const auto reread = [&] {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                return genko::storage::load_document(dir);
            }();
            if (!reread.report.clean()) {
                failures.push_back(name + ": read back with " + genko::core::dump_python(reread.report.to_json()).substr(0, 600));
                continue;
            }
            const genko::test::StepOutcome after = genko::test::state_of(reread.document, store);

            // the same as Python's, saved and read back
            const Json& py = python[k].back();
            std::string where;
            if (!py.contains("reread")) {
                failures.push_back(name + ": Python did not read it back");
                continue;
            }
            if (!genko::test::strict_equal(after.full, py["full"], &where)) {
                failures.push_back(name + ": read back, full snapshot (Python's read back): " + where.substr(0, 1200));
                continue;
            }
            if (!genko::test::strict_equal(after.payload, py["payload"], &where)) {
                failures.push_back(name + ": read back, payload (Python's read back): " + where.substr(0, 1200));
                continue;
            }
            // the same as before the save, but for the selection, the default layers of a page that had none and the
            // margins of a paper preset (ints there; Python's reader makes them floats)
            for (Json* spec : {&before.full["spec"], &before.payload["spec"]}) {
                if (!spec->contains("margins_mm")) continue;
                for (Json& margin : (*spec)["margins_mm"]) {
                    if (margin.is_number_integer()) {
                        margin = static_cast<double>(margin.get<std::int64_t>());
                        ++floated;
                    }
                }
            }
            for (std::size_t p = 0; p < before.full["pages"].size(); ++p) {
                Json& page = before.full["pages"][p];
                if (!page["selected_frame_id"].is_null()) ++unselected;
                page["selected_frame_id"] = nullptr;
                if (page["layers"].empty() && p < after.full["pages"].size()) {
                    std::string roles;
                    for (const Json& layer : after.full["pages"][p]["layers"]) roles += layer["role"].get<std::string>() + " ";
                    if (roles != "bg name ink finish ") {
                        failures.push_back(name + ": page " + std::to_string(p + 1) + " had no layers, read back with " + roles);
                    }
                    page["layers"] = after.full["pages"][p]["layers"];
                    before.payload["pages"][p]["layers"] = after.payload["pages"][p]["layers"];
                    ++refilled;
                }
            }
            if (!genko::test::strict_equal(after.full, before.full, &where)) {
                failures.push_back(name + ": read back, full snapshot: " + where.substr(0, 1200));
                continue;
            }
            if (!genko::test::strict_equal(after.payload, before.payload, &where)) {
                failures.push_back(name + ": read back, payload: " + where.substr(0, 1200));
                continue;
            }
            counts[c["op"].get<std::string>()] += 1;
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(chosen.size()) +
                                    " books read back differently (see the warnings)").c_str());
        std::string summary;
        for (const char* op : kOps) {
            summary += std::string(op) + " " + std::to_string(counts[op]) + ", ";
            QVERIFY2(counts[op] >= 10, (std::string(op) + ": only " + std::to_string(counts[op]) + " cases saved and read back").c_str());
        }
        qInfo("saved and read back as they were: %s(%d selections not saved, %d pages without layers read back with the "
              "default ones, %d int margins read back as floats)",
              summary.c_str(), unselected, refilled, floated);
    }
};

int main(int argc, char** argv) {
    // (savedAndReadBack saves and reads some 500 books: with the sanitizers that may take longer than QtTest's 5
    // minutes for a test function; CTest's TIMEOUT still bounds the whole)
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "3000000");
    QCoreApplication app(argc, argv);
    TestContractOps test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_ops.moc"
