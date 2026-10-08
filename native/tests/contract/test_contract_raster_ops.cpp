// The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) against the Python baseline (src/genko run by the
// reference Python, tools/migration/pyref_harness.py), through every op of this build (render::ops_registry):
//   1. fixed cases (contract/raster_cases.json, on the book `pyref_harness.py make-rasterbook` makes): for each step the
//      reply (applied, snapshot, job_id, warnings, results — or the error's words), the full snapshot and the
//      project.json payload are the same as Python's apply_ops gives, ids counted the same on both sides; the PNGs the
//      payload refers to (a layer's pixels, its mask, its patches, an area kept on a page) hold the same pixels (this
//      build writes other PNG bytes for them), the JSON assets are the same bytes. Every op of M3-A1 has 8 or more
//      cases that succeed and 4 or more that fail. A case marked "cpp": "not_yet_ported" draws a page with what this
//      build does not draw yet: C++ must refuse it with not_yet_ported (page 3's nombre, tone and line of dialogue are
//      drawn since M4: the fill that looks at that page is compared with Python's).
//   2. random op sequences (150, made by `pyref_harness.py make-raster-sequences` with a fixed seed: these ops and the
//      basic ones of M2, 1 to 10 ops each, by several actors, some dry runs, strict_gates and page locks) on 15 random
//      books for drawing (`render_harness.py make-books`, without what this build does not draw yet): each step the
//      same as Python's.
//   4. saved and read back: the book after each successful fixed case, saved by storage::Saver and read again, has the
//      same full snapshot and payload as before the save and as Python's book saved by save_episode and read again.
//   And the filters' float32 decisions at their edges (filterEdges): pictures made to sit on them, the same pixels as
//   Python's filters.apply_filter.
// (3, the pages drawn: test_contract_raster_render; the command line: test_contract_raster_cli.)
// The one place the same bits cannot be asked for: a perspective warp (transform_area with warp.perspective). Python
// takes its homography from numpy's SVD (LAPACK through OpenBLAS, whose last bits depend on its kernels and the
// machine it runs on); this build solves the same equations by elimination. Such a step is compared with its numbers
// within 1e-9 (relative), the lines of a layer read on each side and compared so, and the pictures within
// ARCHITECTURE.md §9 (genko::test::compare_step_near); a random sequence stops there (the books differ in their last
// bits from then on). The counts are reported.
// Skipped when there is no reference Python ($GENKO_PYREF or /opt/pyref/bin/python).
// GENKO_RASTER_CASES=<text> runs only the fixed cases whose name holds the text or whose op it is (and does not count).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/ids.hpp"
#include "opsupport.hpp"
#include "render/filters.hpp"
#include "render/png.hpp"
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

// The ops of M3-A1.
const char* const kOps[] = {"convert_layer", "merge_down",    "merge_layers", "merge_visible", "move_layers",
                            "group_layers",  "set_layer_mask", "paint_mask",   "put_raster",    "filter_raster",
                            "fill",          "fill_area",     "fill_enclosed", "fill_gaps",     "flood_fill",
                            "gradient_fill", "delete_area",   "transform_area", "paste",        "store_area",
                            "forget_area",   "set_paper",     "set_timelapse", "erase",         "erase_raster",
                            "set_stroke_width", "reshape_stroke", "lt_convert"};

// How many strings in `value` start with `prefix` (the pictures a payload refers to: "png:…", "unreadable:…").
int count_prefixed(const Json& value, const std::string& prefix) {
    if (value.is_string()) return value.get<std::string>().starts_with(prefix) ? 1 : 0;
    int n = 0;
    if (value.is_array() || value.is_object()) {
        for (const Json& item : value) n += count_prefixed(item, prefix);
    }
    return n;
}

// Only some of the fixed cases (GENKO_RASTER_CASES).
bool chosen(const Json& c) {
    const QByteArray only = qgetenv("GENKO_RASTER_CASES");
    if (only.isEmpty()) return true;
    const std::string text = only.toStdString();
    return c["n"].get<std::string>().find(text) != std::string::npos || c["op"].get<std::string>() == text;
}

}  // namespace

class TestContractRasterOps : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString rasterbook_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run py(const QStringList& args, int timeout_ms = 3600000) {
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

    Json fixed_cases() const {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/raster_cases.json");
        return file;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        // (the plugin filters are looked for in the same empty folder on both sides)
        QDir().mkpath(path("pyenv/config"));
        qputenv("GENKO_CONFIG_DIR", path("pyenv/config").toUtf8());
        rasterbook_ = path("rasterbook.genko");
        const auto made = py({"make-rasterbook", rasterbook_});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
    }

    void nearOracleRejectsMissingMarks() {
        using genko::render::Image;
        using genko::render::Ink;
        const Image blank = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        Image marked = blank.copy();
        marked.paste(Ink{20, 40, 70, 255}, {43, 51, 44, 52});
        Image moved = blank.copy();
        moved.paste(Ink{20, 40, 70, 255}, {44, 51, 45, 52});
        Image notMoved = blank.copy();
        notMoved.paste(Ink{20, 40, 70, 255}, {60, 51, 61, 52});
        const QString a = path("oracle-a.png"), b = path("oracle-b.png");
        genko::test::write_bytes(a, genko::render::write_png(marked));
        genko::test::write_bytes(b, genko::render::write_png(moved));
        QVERIFY(genko::test::compare_picture_near(a, b).empty());
        genko::test::write_bytes(b, genko::render::write_png(blank));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        genko::test::write_bytes(b, genko::render::write_png(notMoved));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        Image thin = blank.copy();
        thin.paste(Ink{0, 0, 0, 255}, {90, 20, 91, 25});
        genko::test::write_bytes(a, genko::render::write_png(thin));
        genko::test::write_bytes(b, genko::render::write_png(blank));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        Image mask = Image::create("L", {128, 128}, Ink{0});
        mask.paste(Ink{255}, {31, 60, 32, 61});
        genko::test::write_bytes(a, genko::render::write_png(mask));
        genko::test::write_bytes(b, genko::render::write_png(Image::create("L", {128, 128}, Ink{0})));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
    }

    void nearOracleRejectsThinning() {
        using genko::render::Image;
        using genko::render::Ink;
        Image two = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        two.paste(Ink{20, 40, 70, 255}, {43, 51, 45, 52});
        Image one = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        one.paste(Ink{20, 40, 70, 255}, {43, 51, 44, 52});
        const QString a = path("thinning-a.png"), b = path("thinning-b.png");
        genko::test::write_bytes(a, genko::render::write_png(two));
        genko::test::write_bytes(b, genko::render::write_png(one));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        Image shifted = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        shifted.paste(Ink{20, 40, 70, 255}, {44, 51, 46, 52});
        genko::test::write_bytes(b, genko::render::write_png(shifted));
        QVERIFY(genko::test::compare_picture_near(a, b).empty());
        Image line = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        line.paste(Ink{0, 0, 0, 255}, {40, 20, 42, 45});
        Image thin = Image::create("RGBA", {128, 128}, Ink{0, 0, 0, 0});
        thin.paste(Ink{0, 0, 0, 255}, {40, 20, 41, 45});
        genko::test::write_bytes(a, genko::render::write_png(line));
        genko::test::write_bytes(b, genko::render::write_png(thin));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        line.paste(Ink{0, 0, 0, 255}, {40, 20, 44, 45});
        thin.paste(Ink{0, 0, 0, 255}, {40, 20, 43, 45});
        genko::test::write_bytes(a, genko::render::write_png(line));
        genko::test::write_bytes(b, genko::render::write_png(thin));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
    }

    void nearOracleRejectsGrayBackgroundLoss_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("compensate");
        QTest::addColumn<int>("contrast");
        QTest::addColumn<int>("opacity");
        for (const QString& mode : {QString("RGBA"), QString("RGB"), QString("L")}) {
            for (const int contrast : {8, 128}) {
                const QByteArray label = mode.toLatin1() + QByteArray::number(contrast);
                QTest::newRow((label + "-lost").constData()) << mode << false << contrast << 255;
                QTest::newRow((label + "-remote-compensation").constData()) << mode << true << contrast << 255;
            }
        }
        QTest::newRow("RGBA-alpha8-lost") << QString("RGBA") << false << 128 << 8;
        QTest::newRow("RGBA-alpha8-remote-compensation") << QString("RGBA") << true << 128 << 8;
    }
    void nearOracleRejectsGrayBackgroundLoss() {
        QFETCH(QString, mode);
        QFETCH(bool, compensate);
        QFETCH(int, contrast);
        QFETCH(int, opacity);
        using genko::render::Image;
        using genko::render::Ink;
        const Ink background = mode == "L" ? Ink{128} : mode == "RGB" ? Ink{128,128,128} : Ink{128,128,128,opacity};
        const int light = std::min(255, 128 + contrast), dark = 128 - contrast;
        const Ink foreground = mode == "L" ? Ink{light} : (mode == "RGB" ? Ink{dark,dark,dark} : Ink{dark,dark,dark,opacity});
        const Image blank = Image::create(mode.toStdString(), {128,128}, background);
        Image expected = blank.copy();
        expected.paste(foreground, {8,8,10,9});
        expected.paste(foreground, {50,50,51,51});
        Image actual = expected.copy();
        actual.paste(background, {9,8,10,9});
        if (compensate) actual.paste(foreground, {51,50,52,51});
        const QString a = path("gray-a.png"), b = path("gray-b.png");
        genko::test::write_bytes(a, genko::render::write_png(expected));
        genko::test::write_bytes(b, genko::render::write_png(actual));
        QVERIFY(!genko::test::compare_picture_near(a, b).empty());
        Image shifted = blank.copy();
        shifted.paste(foreground, {9,8,11,9});
        shifted.paste(foreground, {51,50,52,51});
        genko::test::write_bytes(b, genko::render::write_png(shifted));
        QVERIFY(genko::test::compare_picture_near(a, b).empty());
    }

    void coverageGuardRejectsMissingRepeatedAndExtraSteps() {
        const Json generated = Json::parse(R"([{"steps":[{"ops":[{}]},{"ops":[{},{}]}]}])");
        const std::vector<std::size_t> taken{0};
        const std::set<std::pair<std::size_t, std::size_t>> complete{{0,0}, {0,1}};
        QVERIFY(genko::test::step_coverage_error(generated, taken, complete, 2, 3).empty());
        const std::set<std::pair<std::size_t, std::size_t>> missing{{0,0}};
        QVERIFY(!genko::test::step_coverage_error(generated, taken, missing, 2, 3).empty());
        auto extra = complete;
        extra.emplace(0,2);
        QVERIFY(!genko::test::step_coverage_error(generated, taken, extra, 3, 3).empty());
        QVERIFY(!genko::test::step_coverage_error(generated, taken, complete, 3, 3).empty());
        QVERIFY(!genko::test::step_coverage_error(generated, taken, complete, 2, 2).empty());
    }

    void nearOracleStrictOutsideTarget() {
        genko::test::StepOutcome out;
        out.reply = Json::object();
        out.full = Json::object({{"spec", Json::object({{"angle", 0.0}})}});
        out.payload = Json::object();
        Json record = Json::object({{"reply", out.reply}, {"full", out.full}, {"payload", out.payload}});
        record["full"]["spec"]["angle"] = -0.0;
        genko::test::NearSides sides;
        QVERIFY(!genko::test::compare_step_near(out, record, sides).empty());
    }

    void fixedCases() {
        const Json file = fixed_cases();
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        QDir().mkpath(path("fixed"));
        Json jobs = Json::array();
        std::vector<std::size_t> taken;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            if (!chosen(cases[n])) continue;
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = rasterbook_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = path(QStringLiteral("fixed/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = path(QStringLiteral("fixed/%1.json").arg(n)).toStdString();
            if (cases[n].contains("near")) job["dump"] = path(QStringLiteral("fixed/dump-py-%1").arg(n)).toStdString();
            jobs.push_back(std::move(job));
            taken.push_back(n);
        }
        const std::vector<Json> python = python_steps(jobs, "fixed");
        QCOMPARE(python.size(), taken.size());
        int near = 0, tolerated = 0;

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(to_path(rasterbook_));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, std::pair<int, int>> counts;  // op → (successes, failures) matched with Python
        int not_ported = 0, safety_refused = 0, pictures = 0, unreadable = 0;
        std::vector<std::string> failures;
        for (std::size_t k = 0; k < taken.size(); ++k) {
            const std::size_t n = taken[k];
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const std::string op = c["op"].get<std::string>();
            const bool expect_ok = c["ok"].get<bool>();
            genko::storage::AssetStore store(to_path(path(QStringLiteral("fixed/cpp-store-%1").arg(n))));
            genko::core::Document after;
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &after);
            const Json& records = python[k];
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
            if (!expect_ok && !last.contains("uncaught") && !c.contains("other") && !op.starts_with("_")) {
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
                if (c.contains("cpp_error")) {
                    const auto before = genko::test::state_of(loaded.document, store);
                    std::string where;
                    if (final.reply.value("error", std::string()).find(c["cpp_error"].get<std::string>()) == std::string::npos ||
                        !genko::test::strict_equal(final.full, before.full, &where) ||
                        !genko::test::strict_equal(final.payload, before.payload, &where)) {
                        failures.push_back(name + ": intentional refusal changed the book or gave another error: " + where);
                    }
                }
                if (final.code == "not_yet_ported") ++not_ported;
                else ++safety_refused;
                continue;
            }
            // a perspective warp (one step): compared within the tolerance (see randomSequences)
            genko::test::NearSides sides;
            if (c.contains("near")) {
                sides.cpp_store = &store;
                sides.py_store = path(QStringLiteral("fixed/py-store-%1").arg(n));
                sides.cpp_dump = path(QStringLiteral("fixed/dump-cpp-%1").arg(n));
                sides.py_dump = path(QStringLiteral("fixed/dump-py-%1").arg(n));
                sides.prefix = std::to_string(outcomes.size() - 1) + "-";
                genko::test::dump_pictures(after, store, sides.cpp_dump, sides.prefix);
                genko::test::affect_perspective(sides, steps_of(c).back()["ops"], outcomes.back().payload);
                ++near;
            }
            bool same = true;
            for (std::size_t s = 0; s < outcomes.size(); ++s) {
                const std::string diff = c.contains("near") && s + 1 == outcomes.size()
                                             ? genko::test::compare_step_near(outcomes[s], records[s], sides, &tolerated)
                                             : genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    same = false;
                    break;
                }
            }
            if (same) (expect_ok ? counts[op].first : counts[op].second) += 1;
            if (same && expect_ok) {
                pictures += count_prefixed(outcomes.back().payload, "png:");
                unreadable += count_prefixed(outcomes.back().payload, "unreadable:") + count_prefixed(outcomes.back().payload, "missing");
            }
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(taken.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        std::string summary;
        for (const char* op : kOps) {
            const auto [ok, failing] = counts[op];
            summary += std::string(op) + " " + std::to_string(ok) + "/" + std::to_string(failing) + ", ";
            if (!qEnvironmentVariableIsEmpty("GENKO_RASTER_CASES")) continue;
            QVERIFY2(ok >= 8, (std::string(op) + ": " + std::to_string(ok) + " successes match Python (8 needed)").c_str());
            QVERIFY2(failing >= 4, (std::string(op) + ": " + std::to_string(failing) + " failures match Python (4 needed)").c_str());
        }
        const auto [bus_ok, bus_failing] = counts["_bus"];
        qInfo("matched (ok/failing): %sthe bus's area resolution %d/%d; %d refused as not yet ported, %d refused for data safety. The books after the "
              "successful cases refer to %d pictures, compared by their pixels (%d unreadable or missing on both sides). %d "
              "perspective warps compared within the tolerance (%d numbers, lines and pictures differing within it)",
              summary.c_str(), bus_ok, bus_failing, not_ported, safety_refused, pictures, unreadable, near, tolerated);
        QCOMPARE(unreadable, 0);
    }

    void randomSequences() {
        if (!qEnvironmentVariableIsEmpty("GENKO_RASTER_CASES")) QSKIP("only some fixed cases (GENKO_RASTER_CASES)");
        // (GENKO_RASTER_REUSE=<folder>: the books and sequences made there once and used again, for looking into them)
        const QByteArray reuse = qgetenv("GENKO_RASTER_REUSE");
        const QString made_in = reuse.isEmpty() ? scratch_.path() : QString::fromUtf8(reuse);
        const QString books = made_in + QStringLiteral("/random");
        const QString listed = made_in + QStringLiteral("/sequences.json");
        if (reuse.isEmpty() || !QFile::exists(listed)) {
            QDir(books).removeRecursively();
            const auto made = py({"make-raster-books", books, "--seed", "23", "--count", "15"});
            QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
            const auto sequenced = py({"make-raster-sequences", listed, "--books", books, "--seed", "7", "--count", "150"});
            QVERIFY2(sequenced.finished && sequenced.exit_code == 0, sequenced.err.right(4000).constData());
        }
        Json sequences = genko::test::read_json(listed);
        QCOMPARE(sequences.size(), std::size_t{150});
        const Json original_sequences = sequences;
        // Keep every original generated sequence; append stroke edits on five generated books.
        for (std::size_t n = 0; n < 5; ++n) {
            const std::string book = original_sequences[n]["book"].get<std::string>();
            const auto loaded_book = genko::storage::load_document(genko::storage::path_from_utf8(book));
            bool added = false;
            for (std::size_t page = 0; page < loaded_book.document.pages.size() && !added; ++page) {
                for (const auto& layer : loaded_book.document.page(page).layers) {
                    if (layer.locked || !layer.strokes || layer.strokes->items.empty()) continue;
                    const std::string id = layer.strokes->items.front()->id;
                    const double delta = static_cast<double>(n);
                    const Json width = Json::object({{"op","set_stroke_width"},{"page",page+1},
                        {"layer_id",layer.id},{"ids",Json::array({id})},{"width_mm",0.1+0.05*delta},{"scale",0.5+0.2*delta}});
                    const Json reshape = Json::object({{"op","reshape_stroke"},{"page",page+1},
                        {"layer_id",layer.id},{"stroke_id",id},{"points",Json::array({Json::array({10+delta,20,0.2}),Json::array({30+delta,40,0.8})})}});
                    sequences.push_back(Json::object({{"book",book},{"first_id",original_sequences[n]["first_id"]},
                        {"steps",Json::array({Json::object({{"ops",Json::array({width})},{"agent","human:作者"}}),
                                             Json::object({{"ops",Json::array({reshape})},{"agent","human:作者"}})})}}));
                    added = true;
                    break;
                }
            }
            QVERIFY2(added, "generated book needs an unlocked pen line for the appended edits");
        }
        // Keep the original corpus and the five stroke-edit series; append LT series on five generated books.
        const std::string lt_picture = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAGCAAAAADbboAnAAAAFElEQVR4nGNgYFiw4M4dBhAgngEA3mUR0cIRCv0AAAAASUVORK5CYII=";
        const char* lt_methods[] = {"adaptive", "edges", "sobel", "unknown", "adaptive"};
        for (std::size_t n = 0; n < 5; ++n) {
            const std::string book = original_sequences[n]["book"].get<std::string>();
            const auto loaded_book = genko::storage::load_document(genko::storage::path_from_utf8(book));
            QVERIFY(!loaded_book.document.pages.empty());
            const auto& page = loaded_book.document.page(loaded_book.document.pages.size() - 1);
            const Json page_index = page.index.json();
            const Json put = Json::object({{"op", "put_raster"}, {"page", page_index}, {"layer", "bg"}, {"png_base64", lt_picture}});
            const Json threshold = Json::object({{"op", "set_lt"}, {"page", page_index}, {"threshold", 80.0 + 30.0 * static_cast<double>(n)}});
            const Json convert = Json::object({{"op", "lt_convert"}, {"page", page_index}, {"layer", "bg"}, {"to", "draft"}, {"method", lt_methods[n]}});
            const Json again = Json::object({{"op", "lt_convert"}, {"page", page_index}, {"layer", "bg"}, {"to", "draft"}, {"threshold", 0}});
            sequences.push_back(Json::object({{"book", book}, {"first_id", original_sequences[n]["first_id"]},
                {"steps", Json::array({
                    Json::object({{"ops", Json::array({put, threshold})}, {"agent", "human:作者"}}),
                    Json::object({{"ops", Json::array({convert})}, {"agent", "human:作者"}}),
                    Json::object({{"ops", Json::array({again})}, {"agent", "human:作者"}})})}}));
        }
        QCOMPARE(sequences.size(), original_sequences.size() + std::size_t{10});
        for (std::size_t n = 0; n < original_sequences.size(); ++n) QVERIFY(sequences[n] == original_sequences[n]);
        QDir().mkpath(path("seq"));
        const auto job_of = [&](std::size_t n, bool digest) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = sequences[n]["book"];
            job["ids"] = true;
            job["first_id"] = sequences[n]["first_id"];
            job["store"] = path(QStringLiteral("seq/py-store-%1%2").arg(n).arg(digest ? "" : "-full")).toStdString();
            job["steps"] = sequences[n]["steps"];
            job["out"] = path(QStringLiteral("seq/%1%2.json").arg(n).arg(digest ? "" : "-full")).toStdString();
            job["digest"] = digest;
            return job;
        };
        // (GENKO_RASTER_SEQUENCES=3,8,97: only those, for looking into them)
        std::set<std::size_t> only;
        for (const QByteArray& part : qgetenv("GENKO_RASTER_SEQUENCES").split(',')) {
            if (!part.trimmed().isEmpty()) only.insert(static_cast<std::size_t>(part.trimmed().toULong()));
        }
        std::vector<std::size_t> taken;
        Json jobs = Json::array();
        for (std::size_t n = 0; n < sequences.size(); ++n) {
            if (!only.empty() && !only.contains(n)) continue;
            jobs.push_back(job_of(n, true));
            taken.push_back(n);
        }
        const std::vector<Json> ran = python_steps(jobs, "seq");
        QCOMPARE(ran.size(), taken.size());
        std::map<std::size_t, Json> python;
        for (std::size_t k = 0; k < taken.size(); ++k) python[taken[k]] = ran[k];

        std::map<std::string, genko::core::Document> loaded;  // (each book read once)
        int steps = 0, ops = 0, failed_steps = 0, stopped = 0, warped = 0, tolerated = 0;
        std::map<std::string, int> op_counts;  // Input ops in compared batches, including jointly refused batches.
        std::set<std::pair<std::size_t, std::size_t>> compared_steps;
        std::set<std::string> books_used;
        std::vector<std::pair<std::size_t, std::string>> differ;
        std::vector<std::pair<std::size_t, std::size_t>> near;  // (sequence, step): a perspective warp to compare closer
        const auto counted = [&](std::size_t n, std::size_t s, const Json& record) {
            compared_steps.emplace(n, s);
            ++steps;
            for (const Json& op : sequences[n]["steps"][s]["ops"]) {
                ++ops;
                if (op.is_object() && op.contains("op") && op["op"].is_string()) op_counts[op["op"].get<std::string>()] += 1;
            }
            if (record["reply"]["ok"] == Json(false)) ++failed_steps;
        };
        for (const std::size_t n : taken) {
            const std::string book = sequences[n]["book"].get<std::string>();
            books_used.insert(book);
            if (!loaded.contains(book)) {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                loaded[book] = genko::storage::load_document(fs::path(book)).document;
            }
            genko::storage::AssetStore store(to_path(path(QStringLiteral("seq/cpp-store-%1").arg(n))));
            const auto outcomes = genko::test::run_steps(loaded[book], sequences[n]["steps"],
                                                         sequences[n]["first_id"].get<std::uint64_t>(), store, true);
            QCOMPARE(outcomes.size(), sequences[n]["steps"].size());
            QCOMPARE(python.at(n).size(), sequences[n]["steps"].size());
            for (std::size_t s = 0; s < outcomes.size(); ++s) {
                const Json& record = python[n][s];
                // a part this build refuses on purpose: the books go apart from here
                if (outcomes[s].code == "not_yet_ported" && record["reply"]["ok"] == Json(true)) {
                    ++stopped;
                    differ.emplace_back(n, "unported operation prevents full generated trace comparison");
                    break;
                }
                const std::string diff = genko::test::compare_step(outcomes[s], record);
                if (!diff.empty() && genko::test::warps_in_perspective(sequences[n]["steps"][s]["ops"])) {
                    near.emplace_back(n, s);  // (compared closer below; the books go apart in their last bits here)
                    break;
                }
                if (!diff.empty()) {
                    differ.emplace_back(n, "step " + std::to_string(s) + ": " + diff.substr(0, 800));
                    break;
                }
                counted(n, s, record);
            }
        }
        // a perspective warp: Python's homography comes from numpy's SVD (LAPACK through OpenBLAS, whose last bits depend
        // on its kernels and the machine); this build solves the same equations by elimination. The points it moves
        // agree to about 1e-13 mm, the pictures it redraws to a pixel or so: the step is compared within 1e-9 for the
        // Numbers use the specified 1e-9 and pictures the original aggregate
        // bounds. Near warps are followed by every suffix step after exact-state
        // rebase; they are never an acceptance shortcut.
        const QByteArray keep = qgetenv("GENKO_RASTER_DUMP");
        const QString dumps = keep.isEmpty() ? path("dump") : QString::fromUtf8(keep);
        for (const auto& [n, s] : near) {
            const QString py_dump = dumps + QStringLiteral("/near-py-%1").arg(n);
            const QString cpp_dump = dumps + QStringLiteral("/near-cpp-%1").arg(n);
            QDir(py_dump).removeRecursively();
            QDir(cpp_dump).removeRecursively();
            Json job = job_of(n, false);
            job["dump"] = py_dump.toStdString();
            const std::vector<Json> full = python_steps(Json::array({job}), QStringLiteral("seq-near-%1").arg(n));
            if (full.empty()) {
                differ.emplace_back(n, "Python did not run it again");
                continue;
            }
            QCOMPARE(full.front().size(), sequences[n]["steps"].size());
            const genko::core::Document& book = loaded[sequences[n]["book"].get<std::string>()];
            genko::storage::AssetStore store(to_path(path(QStringLiteral("seq/cpp-near-store-%1").arg(n))));
            const QString rebased = path(QStringLiteral("seq/reference-state-%1").arg(n));
            genko::test::copy_tree(QString::fromStdString(job["store"].get<std::string>()), rebased);
            genko::test::NearSides sides;
            sides.cpp_store = &store;
            sides.py_store = QString::fromStdString(job["store"].get<std::string>());
            sides.cpp_dump = cpp_dump;
            sides.py_dump = py_dump;
            bool sequence_ok = true;
            const auto outcomes = genko::test::run_steps(
                book, sequences[n]["steps"], sequences[n]["first_id"].get<std::uint64_t>(), store, false, nullptr,
                [&](genko::core::Document& current, std::size_t t, const genko::test::StepOutcome& outcome) {
                    if (!sequence_ok) return;
                    const Json& record = full.front()[t];
                    if (outcome.code == "not_yet_ported" && record["reply"]["ok"] == Json(true)) {
                        differ.emplace_back(n, "step " + std::to_string(t) + ": unported op after perspective; change generator without losing this rejection fixture");
                        sequence_ok = false;
                        return;
                    }
                    const bool perspective = genko::test::warps_in_perspective(sequences[n]["steps"][t]["ops"]);
                    std::string diff = genko::test::compare_step(outcome, record);
                    if (perspective && !diff.empty()) {
                        sides.numeric_paths.clear();
                        sides.picture_paths.clear();
                        sides.prefix = std::to_string(t) + "-";
                        genko::test::affect_perspective(sides, sequences[n]["steps"][t]["ops"], outcome.payload);
                        genko::test::dump_pictures(current, store, cpp_dump, sides.prefix);
                        diff = genko::test::compare_step_near(outcome, record, sides, &tolerated);
                        if (diff.empty()) {
                            // Continue all suffix steps from the exact Python perspective state.
                            // The comparison above accepts only the affected fields; everything else is exact.
                            genko::test::write_bytes(rebased + "/project.json", genko::test::read_bytes(py_dump + "/" +
                                                     QString::number(t) + "-project.json"));
                            const genko::core::ScopedIdSource reading(genko::core::counting_ids());
                            auto reference = genko::storage::load_document(to_path(rebased));
                            if (!reference.report.clean()) diff = "reference state failed to read";
                            else {
                                for (std::size_t page = 0; page < current.pages.size(); ++page) {
                                    reference.document.edit_page(page).selected_frame_id = current.page(page).selected_frame_id;
                                }
                                const auto rebased_state = genko::test::state_of(reference.document, store);
                                genko::test::StepOutcome aligned;
                                aligned.reply = outcome.reply;
                                aligned.code = outcome.code;
                                aligned.full = rebased_state.full;
                                aligned.payload = rebased_state.payload;
                                diff = genko::test::compare_step(aligned, record);
                                if (diff.empty()) current = std::move(reference.document);
                            }
                            ++warped;
                        }
                    }
                    if (!diff.empty()) {
                        differ.emplace_back(n, "step " + std::to_string(t) + " (full suffix): " + diff.substr(0, 1500));
                        sequence_ok = false;
                        return;
                    }
                    // Prefix steps were counted on the first pass; every suffix step is now counted.
                    if (t >= s) counted(n, t, record);
                });
            QCOMPARE(outcomes.size(), sequences[n]["steps"].size());
        }
        // where they differ, the whole snapshots and payloads say where, and the pictures how (written into
        // $GENKO_RASTER_DUMP when it is set, to be looked at: cpp-<n>/ and py-<n>/)
        const std::size_t looked_into = qEnvironmentVariableIsSet("GENKO_RASTER_DETAIL")
                                            ? static_cast<std::size_t>(qEnvironmentVariableIntValue("GENKO_RASTER_DETAIL"))
                                            : 4;
        for (std::size_t k = 0; k < differ.size() && k < looked_into; ++k) {
            const std::size_t n = differ[k].first;
            const QString py_dump = dumps + QStringLiteral("/py-%1").arg(n);
            const QString cpp_dump = dumps + QStringLiteral("/cpp-%1").arg(n);
            QDir(py_dump).removeRecursively();
            QDir(cpp_dump).removeRecursively();
            Json job = job_of(n, false);
            job["dump"] = py_dump.toStdString();
            const std::vector<Json> full = python_steps(Json::array({job}), QStringLiteral("seq-full-%1").arg(n));
            std::string detail;
            if (!full.empty()) {
                const genko::core::Document& book = loaded[sequences[n]["book"].get<std::string>()];
                const std::uint64_t first_id = sequences[n]["first_id"].get<std::uint64_t>();
                genko::storage::AssetStore store(to_path(path(QStringLiteral("seq/cpp-full-store-%1").arg(n))));
                const auto outcomes = genko::test::run_steps(book, sequences[n]["steps"], first_id, store, false);
                for (std::size_t s = 0; s < outcomes.size() && detail.empty(); ++s) {
                    detail = genko::test::compare_step(outcomes[s], full.front()[s]);
                    if (detail.empty()) continue;
                    detail = "step " + std::to_string(s) + " " + genko::core::dump_python(sequences[n]["steps"][s]).substr(0, 900) +
                             ": " + detail.substr(0, 1500);
                    // the book after this step, its pictures beside Python's
                    Json head = Json::array();
                    for (std::size_t t = 0; t <= s; ++t) head.push_back(sequences[n]["steps"][t]);
                    genko::core::Document after;
                    genko::test::run_steps(book, head, first_id, store, false, &after);
                    genko::test::dump_pictures(after, store, cpp_dump, std::to_string(s) + "-");
                    const std::string pictures = genko::test::picture_differences(cpp_dump, py_dump, std::to_string(s) + "-");
                    if (!pictures.empty()) detail += " | pictures: " + pictures.substr(0, 1500);
                }
            }
            differ[k].second += " | " + detail;
        }
        for (const auto& [n, why] : differ) qWarning("sequence %zu: %s", n, why.c_str());
        QVERIFY2(differ.empty(), (std::to_string(differ.size()) + " of " + std::to_string(taken.size()) +
                                  " sequences differ from Python").c_str());
        const auto coverage = genko::test::step_coverage_error(sequences, taken, compared_steps,
            static_cast<std::size_t>(steps), static_cast<std::size_t>(ops));
        QVERIFY2(coverage.empty(), coverage.c_str());
        QCOMPARE(stopped, 0);
        if (!only.empty()) return;
        QCOMPARE(books_used.size(), std::size_t{15});
        std::string summary;
        for (const char* op : kOps) {
            summary += std::string(op) + " " + std::to_string(op_counts[op]) + ", ";
            QVERIFY2(op_counts[op] >= 5, (std::string(op) + ": only " + std::to_string(op_counts[op]) + " in the sequences").c_str());
        }
        qInfo("%d steps (%d ops, %d of the steps refused by both) the same as Python; %d sequences stopped at a part not "
              "ported yet; %d at a perspective warp the same within the tolerance (%d numbers, lines and pictures differing "
              "within it). Ops of M3-A1 run: %s",
              steps, ops, failed_steps, stopped, warped, tolerated, summary.c_str());
        // Stop-free coverage is asserted above; all perspective suffixes are compared.
        QVERIFY2(warped <= 20, "too many perspective warps: investigate generated workload");
    }

    // The filters' float32 decisions at their edges (pyref_harness.py filter-edges): despeckle's ink (numpy's float32
    // luminance below 128) for every colour within 0.03 of it, and lineart's ratio against thresholds that float32
    // rounds up or down — the same pixels as Python's filters.apply_filter.
    void filterEdges() {
        if (!qEnvironmentVariableIsEmpty("GENKO_RASTER_CASES")) QSKIP("only some fixed cases (GENKO_RASTER_CASES)");
        const QString dir = path("edges");
        const auto made = py({"filter-edges", dir});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json cases = genko::test::read_json(dir + "/cases.json");
        QCOMPARE(cases.size(), std::size_t{12});
        int pixels = 0;
        for (const Json& c : cases) {
            const auto read = [&](const char* key) {
                return genko::render::read_png(genko::test::read_bytes(dir + "/" + QString::fromStdString(c[key].get<std::string>())),
                                               genko::render::kPillowOpenLimits)
                    .convert("RGBA");
            };
            const genko::render::Image input = read("input");
            const genko::render::Image want = read("output");
            const genko::render::Image got =
                genko::render::filters::apply_filter(input, c["kind"].get<std::string>(), c["params"]).convert("RGBA");
            QVERIFY2(got.size() == want.size(), c["name"].get<std::string>().c_str());
            const std::string a = got.tobytes();
            const std::string b = want.tobytes();
            long differ = 0;
            for (std::size_t i = 0; i < a.size(); i += 4) differ += a.compare(i, 4, b, i, 4) != 0 ? 1 : 0;
            QVERIFY2(differ == 0, (c["name"].get<std::string>() + ": " + std::to_string(differ) + " pixels differ").c_str());
            pixels += got.width() * got.height();
        }
        qInfo("%zu pictures on the edges of the filters' float32 decisions (%d pixels) the same as Python's", cases.size(), pixels);
    }

    // 4. Each successful fixed case's book saved by storage::Saver as a new book and read back is the book before the
    //    save (but for the selected panel, which is not saved) and the same as Python's book after the case, saved with
    //    save_episode and read back.
    void savedAndReadBack() {
        if (!qEnvironmentVariableIsEmpty("GENKO_RASTER_CASES")) QSKIP("only some fixed cases (GENKO_RASTER_CASES)");
        const Json file = fixed_cases();
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        QDir().mkpath(path("saved"));
        Json jobs = Json::array();
        std::vector<std::size_t> picked;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            if (!cases[n]["ok"].get<bool>() || cases[n].contains("cpp")) continue;
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = rasterbook_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = path(QStringLiteral("saved/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = path(QStringLiteral("saved/%1.json").arg(n)).toStdString();
            job["reread"] = path(QStringLiteral("saved/py-%1.genko").arg(n)).toStdString();
            if (cases[n].contains("near")) job["dump"] = path(QStringLiteral("saved/dump-py-%1").arg(n)).toStdString();
            jobs.push_back(std::move(job));
            picked.push_back(n);
        }
        const std::vector<Json> python = python_steps(jobs, "saved");
        QCOMPARE(python.size(), picked.size());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(to_path(rasterbook_));
        }();
        std::map<std::string, int> counts;  // op → cases saved and read back as they were
        int unselected = 0, reordered = 0;
        std::vector<std::string> failures;
        for (std::size_t k = 0; k < picked.size(); ++k) {
            const Json& c = cases[picked[k]];
            const std::string name = c["n"].get<std::string>();
            genko::storage::AssetStore store(to_path(path(QStringLiteral("saved/cpp-store-%1").arg(picked[k]))));
            genko::core::Document doc;
            genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &doc);
            genko::test::StepOutcome before = genko::test::state_of(doc, store);

            const fs::path dir = to_path(path(QStringLiteral("saved/cpp-%1.genko").arg(picked[k])));
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

            // the same as Python's, saved and read back (but for a perspective warp: the same within the tolerance, which
            // fixedCases checks)
            const Json& py = python[k].back();
            std::string where;
            if (!py.contains("reread")) {
                failures.push_back(name + ": Python did not read it back");
                continue;
            }
            if (c.contains("near")) {
                genko::test::NearSides sides;
                sides.cpp_store = &store;
                sides.py_store = path(QStringLiteral("saved/py-store-%1").arg(picked[k]));
                sides.cpp_dump = path(QStringLiteral("saved/dump-cpp-%1").arg(picked[k]));
                sides.py_dump = path(QStringLiteral("saved/dump-py-%1").arg(picked[k]));
                sides.prefix = "reread-";
                genko::test::dump_pictures(reread.document, store, sides.cpp_dump, sides.prefix);
                genko::test::affect_perspective(sides, steps_of(c).back()["ops"], after.payload);
                genko::test::StepOutcome checked = after;
                checked.reply = Json::object({{"ok", true}});
                Json expected = py;
                expected["reply"] = checked.reply;
                const std::string diff = genko::test::compare_step_near(checked, expected, sides);
                if (!diff.empty()) {
                    failures.push_back(name + ": read back, perspective: " + diff.substr(0, 1200));
                    continue;
                }
            }
            if (!c.contains("near") && !genko::test::strict_equal(after.full, py["full"], &where)) {
                failures.push_back(name + ": read back, full snapshot (Python's read back): " + where.substr(0, 1200));
                continue;
            }
            if (!c.contains("near") && !genko::test::strict_equal(after.payload, py["payload"], &where)) {
                failures.push_back(name + ": read back, payload (Python's read back): " + where.substr(0, 1200));
                continue;
            }
            // the same as before the save, but for the selection
            for (Json& page : before.full["pages"]) {
                if (!page["selected_frame_id"].is_null()) ++unselected;
                page["selected_frame_id"] = nullptr;
            }
            if (!genko::test::strict_equal(after.full, before.full, &where)) {
                failures.push_back(name + ": read back, full snapshot: " + where.substr(0, 1200));
                continue;
            }
            if (!genko::test::strict_equal(after.payload, before.payload, &where)) {
                // (a pasted patch that came with an "asset" key keeps it in its place until the book is read again,
                // then has it last: Python's book does the same — compared with Python's above — so only the order
                // may differ here)
                std::string why;
                if (where.find("key order differs") == std::string::npos ||
                    !genko::test::same_content(after.payload, before.payload, &why)) {
                    failures.push_back(name + ": read back, payload: " + where.substr(0, 1200));
                    continue;
                }
                ++reordered;
            }
            counts[c["op"].get<std::string>()] += 1;
        }
        for (const auto& failure : failures) qWarning("%s", failure.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(picked.size()) +
                                    " books read back differently (see the warnings)").c_str());
        std::string summary;
        for (const char* op : kOps) {
            summary += std::string(op) + " " + std::to_string(counts[op]) + ", ";
            QVERIFY2(counts[op] >= 8, (std::string(op) + ": only " + std::to_string(counts[op]) + " cases saved and read back").c_str());
        }
        qInfo("saved and read back as they were: %s(%d selections not saved; %d books with a patch's keys in Python's order "
              "after reading)",
              summary.c_str(), unselected, reordered);
    }
};

int main(int argc, char** argv) {
    // (each test function runs hundreds of op batches on both sides: with the sanitizers that takes longer than QtTest's
    // 5 minutes for a test function; CTest's TIMEOUT still bounds the whole)
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "7000000");
    QCoreApplication app(argc, argv);
    TestContractRasterOps test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_raster_ops.moc"
