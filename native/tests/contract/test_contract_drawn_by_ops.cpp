// The ops and the drawing together: a book made by the ops of M2-O1 — pen lines added with add_stroke (steadied,
// tapered, fitted to a curve, smoothed, on a pressure curve, with every kind of pen and brush), cut by erase (and up to
// their crossings), edited and simplified, on layers made by add_layer and set by set_layer — drawn by `genko render`,
// against the same ops applied by Python's apply_ops and the book drawn by `python -m genko render`: the same JSON and
// every pixel the same, for both pages in print, proof and name at 100 and 200 dpi.
//
// The book the ops run on: `pyref_harness.py make-drawbook` (panels cut, a fill and a pen layer, a rounded panel with a
// double border, nombres off: nothing this build does not draw yet). New ids are counted the same on both sides (the
// pencil's grain and the spray's drops are seeded with the line's id), the C++ book saved by storage::Saver (v4), the
// Python one by save_episode (v3). Skipped without the reference Python.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "core/ids.hpp"
#include "opsupport.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

constexpr std::uint64_t kFirstId = 0x1000;

// Two batches: the lines of page 1, then those of page 2 with the edits.
const char* const kSteps = R"([
{"agent": "human:作者", "ops": [
  {"op": "add_layer", "page": 1, "kind": "pen", "id": "pen-a", "name": "線", "blend": "multiply"},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 40, 0.2], [45, 52, 0.5], [60, 58, 0.9], [75, 61, 0.8], [90, 70, 0.4], [105, 74, 0.1]],
   "kind": "gpen", "width_mm": 1.2, "taper": true, "stabilize": 5},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[120, 40, 0.6], [140, 45, 0.7], [160, 60, 0.9], [175, 80, 0.5]],
   "kind": "maru", "width_mm": 0.9, "taper": true, "taper_in_mm": 6, "taper_out_mm": 12, "curve": "gpen"},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 90], [34, 92], [40, 95], [60, 99], [100, 104], [104, 106], [108, 107]],
   "kind": "kabura", "stabilize": 7, "stabilize_speed": true},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[120, 100], [128, 103], [133, 99], [141, 104], [150, 101], [158, 106], [170, 102]],
   "kind": "mili", "post_fit": 0.4, "width_mm": 0.5},
  {"op": "add_stroke", "page": 1, "layer_id": "pen-a", "points": [[30, 160, 0.4], [50, 170, 0.6], [70, 165, 0.8], [90, 180, 0.7], [110, 175, 0.5]],
   "kind": "pencil", "post_smooth": 3, "width_mm": 1.0},
  {"op": "add_stroke", "page": 1, "layer_id": "pen-a", "points": [[130, 160, 0.1], [140, 175, 0.5], [150, 195, 1.0], [160, 210, 0.6]],
   "kind": "fude", "pressure_gamma": 1.6, "pressure_opacity": 0.7, "width_mm": 2.5},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 200], [180, 205]], "kind": "marker", "rgb": [210, 40, 40], "opacity": 0.6},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 230, 0.6], [80, 220, 0.8], [120, 240, 0.7], [160, 225, 0.5]],
   "kind": "calligraphy", "rotation": [0, 25, 50, 75]},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[40, 255], [90, 262], [140, 255]], "kind": "spray", "width_mm": 4},
  {"op": "add_stroke", "page": 1, "layer": "name", "points": [[25, 25], [185, 25], [185, 280]], "width_mm": 0.5},
  {"op": "erase", "page": 1, "layer": "ink", "points": [[60, 30], [62, 80]], "width_mm": 3},
  {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[100, 215], [100, 250]], "kind": "gpen", "width_mm": 0.6},
  {"op": "erase", "page": 1, "layer": "ink", "points": [[100, 246], [101, 247]], "width_mm": 2, "mode": "to_crossing"}
]},
{"agent": "human:作者", "ops": [
  {"op": "add_stroke", "page": 2, "layer_id": "pen-2", "points": [[40, 50, 0.3], [70, 80, 0.9], [100, 70, 0.6], [130, 120, 0.4]],
   "kind": "water", "width_mm": 3},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 150], [60, 170], [90, 150], [120, 170]], "kind": "dashline"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 200], [170, 210]], "kind": "hearts"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 230], [170, 240]], "kind": "stars"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[40, 260], [160, 255]], "kind": "grass", "width_mm": 5},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[150, 50], [170, 90]], "kind": "leaves"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[150, 140], [180, 145]], "kind": "airbrush", "width_mm": 6},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 280], [100, 282]], "kind": "stipple"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[110, 280], [180, 278]], "kind": "dotline"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 125], [100, 128]], "kind": "lace"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[110, 185], [140, 190]], "kind": "fill_pen"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[50, 30], [60, 35], [70, 33]], "kind": "fx"},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[20, 100], [190, 102]], "kind": "white", "width_mm": 2},
  {"op": "edit_stroke", "page": 2, "layer": "ink", "index": 0, "points": [[30, 150], [50, 175], [80, 150], [110, 175], [130, 150]]},
  {"op": "simplify_stroke", "page": 2, "layer": "ink", "index": 0, "epsilon_mm": 3},
  {"op": "set_layer", "page": 2, "id": "pen-2", "opacity": 0.7}
]}
])";

}  // namespace

class TestContractDrawnByOps : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString base_;
    QString cpp_book_;
    QString py_book_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    genko::test::Run python(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args, genko::test::python_env(path("pyenv")));
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString base = base_ = path("base.genko");
        const auto made = genko::test::harness({"make-drawbook", base}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json steps = genko::core::parse_python_json(kSteps);

        // Python: the ops on the book read, then the book saved by save_episode (the harness's steps job, "reread")
        py_book_ = path("py.genko");
        Json job = Json::object();
        job["op"] = "steps";
        job["book"] = base.toStdString();
        job["ids"] = true;
        job["first_id"] = kFirstId;
        job["store"] = path("py-store").toStdString();
        job["steps"] = steps;
        job["out"] = path("py-steps.json").toStdString();
        job["reread"] = py_book_.toStdString();
        genko::test::write_bytes(path("jobs.json"), genko::core::dump_python(Json::array({job})));
        const auto ran = genko::test::harness({"batch", path("jobs.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(path("py-steps.json"));

        // C++: the same ops through the CommandBus, the book saved by storage::Saver
        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(base.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        genko::storage::AssetStore store(genko::storage::path_from_utf8(path("cpp-store").toStdString()));
        genko::core::Document doc;
        const auto outcomes = genko::test::run_steps(loaded.document, steps, kFirstId, store, false, &doc);
        QCOMPARE(outcomes.size(), steps.size());
        for (std::size_t s = 0; s < outcomes.size(); ++s) {
            QVERIFY2(outcomes[s].reply.value("ok", false), genko::core::dump_python(outcomes[s].reply).substr(0, 600).c_str());
            const std::string diff = genko::test::compare_step(outcomes[s], records[s]);  // (the books are the same)
            QVERIFY2(diff.empty(), ("step " + std::to_string(s) + ": " + diff.substr(0, 1500)).c_str());
        }
        cpp_book_ = path("cpp.genko");
        const fs::path dir = genko::storage::path_from_utf8(cpp_book_.toStdString());
        fs::create_directories(dir);
        genko::storage::ProjectLock lock(dir, "human:作者");
        lock.try_acquire();
        genko::storage::SaveRequest request;
        request.actor = "human:作者";
        request.ops = Json::array();
        genko::storage::Saver(lock).save(doc, request);
    }

    void pages() {
        int compared = 0;
        for (const char* page : {"1", "2"}) {
            for (const char* mode : {"print", "proof", "name"}) {
                for (const char* dpi : {"100", "200"}) {
                    const QString what = QStringLiteral("page %1 %2 @%3").arg(page, mode, dpi);
                    const QString out = path("out/page.png");
                    const QStringList tail{"--page", page, "--dpi", dpi, "--mode", mode, "--out", out};
                    QFile::remove(out);
                    const auto py = python(QStringList{"render", py_book_} + tail);
                    QVERIFY2(py.finished && py.exit_code == 0, (what + ": " + py.err.right(2000)).toUtf8().constData());
                    const genko::render::Image want = genko::render::read_png(genko::test::read_bytes(out));
                    QFile::remove(out);
                    const auto got = genko::test::run_genko(QStringList{"render", cpp_book_} + tail);
                    QVERIFY2(got.finished && got.exit_code == 0, (what + ": " + got.out + got.err).toUtf8().constData());
                    QCOMPARE(got.out, py.out);  // the same JSON, byte for byte
                    const genko::render::Image image = genko::render::read_png(genko::test::read_bytes(out));
                    if (image.size() != want.size() || image.tobytes() != want.tobytes()) {
                        const auto diff = genko::test::pixel_diff(image, want);
                        genko::test::keep_pictures(QStringLiteral("drawn-by-ops-p%1-%2-%3").arg(page, mode, dpi), image, want);
                        QFAIL(qPrintable(what + QStringLiteral(": %1 pixels differ (largest %2)").arg(diff.pixels).arg(diff.largest)));
                    }
                    // (the lines are there: the page is not the page before the ops)
                    QFile::remove(out);
                    QCOMPARE(genko::test::run_genko(QStringList{"render", base_} + tail).exit_code, 0);
                    const genko::render::Image before = genko::render::read_png(genko::test::read_bytes(out));
                    QVERIFY2(genko::test::pixel_diff(image, before).pixels > 1000, qPrintable(what + ": hardly a line drawn"));
                    ++compared;
                }
            }
        }
        qInfo("%d pages drawn by `genko render` the same as by `python -m genko render`", compared);
        QCOMPARE(compared, 12);
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "1700000");
    QCoreApplication app(argc, argv);
    TestContractDrawnByOps test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_drawn_by_ops.moc"
