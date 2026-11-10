// Animation (M3③, Python's genko/anim.py and animops.py) against Python:
//  - the ops (set_animation, add_anim_folder, add_cel, set_exposure, set_exposures, set_camera_key, set_light_table):
//    the cases of contract/anim_cases.json on the book `pyref_harness.py make-opsbook` makes, against Python's
//    apply_ops: each step's reply, full snapshot and project.json payload, and the book after the case saved and read
//    back, as test_contract_rulers does with its cases;
//  - an animation page drawn: cels drawn on in two animation folders (one shown from a later frame), its exposure sheet
//    set, on the book `pyref_harness.py make-drawbook` makes; drawn by `genko render` and by `python -m genko render`
//    (an animation page draws as its first frame): the same JSON and every pixel the same, in print and proof;
//  - that page written out (anim.export at 20 dpi, the camera's crop) as GIF, APNG, WebP, numbered PNGs and MP4 (when
//    ffmpeg is there), each file played by Pillow (ffprobe for the MP4) against Python's own files: the frames, how long
//    each shows and the loop the same, and for GIF, APNG and PNGs every pixel (WebP is lossy: its frames, their sizes
//    and timing);
//  - and beyond Python: a camera rect that is not finite is refused (Python would write NaN into the book); PC5 J-e1's
//    timeline@1 — set_animation's work area and fps of 0.5..120 (anim_cases.json's timeline_cases), a range of the frames
//    and another speed written out, `genko export --format animation --work-area`, `genko render --frame`, `genko inspect
//    --timeline`. (set_animation's refusal of an fps names 0.5 to 120: opsupport's with_decided_words.)
// What is compared with Python is skipped without the Python reference; what is not (the GIF codes, timeline@1, the work
// area written out, the camera's refusal) runs without it.

#include <QtTest>

#include <algorithm>
#include <array>
#include <optional>
#include <random>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/video.hpp"
#include "opsupport.hpp"
#include "render/anim.hpp"
#include "render/movie.hpp"
#include "render/png.hpp"
#include "render/timelapse.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

#ifdef Q_OS_LINUX
#include <signal.h>
#endif

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

constexpr std::uint64_t kFirstId = 0x1000;

// The steps of a case of anim_cases.json (ops_cases.json's format): "steps", or one step of "ops", "agent", "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

// Page 2 of the draw book as an animation: folder af (c1 from frame 1, c2 from 3, a paint cel c3 from 5) and folder bf
// (d1 from frame 2: nothing at frame 1), each drawn on; then the sheet changed so frame 1 shows c2.
const char* const kSteps = R"([
{"agent": "human:作者", "ops": [
  {"op": "set_animation", "page": 2, "frames": 6, "fps": 8},
  {"op": "add_anim_folder", "page": 2, "id": "af", "name": "動き"},
  {"op": "add_cel", "page": 2, "folder": "af", "id": "c1"},
  {"op": "add_stroke", "page": 2, "layer_id": "c1", "points": [[40, 50, 0.4], [80, 90, 0.9], [120, 80, 0.6]], "kind": "gpen", "width_mm": 1.5},
  {"op": "add_cel", "page": 2, "folder": "af", "id": "c2", "at": 3},
  {"op": "add_stroke", "page": 2, "layer_id": "c2", "points": [[40, 150, 0.5], [90, 170, 0.8], [150, 140, 0.6]], "kind": "maru", "width_mm": 2},
  {"op": "add_cel", "page": 2, "folder": "af", "id": "c3", "kind": "paint", "at": 5},
  {"op": "fill_area", "page": 2, "layer_id": "c3", "area": {"poly": [[30, 200], [120, 200], [80, 260]]}, "rgb": [80, 80, 80]},
  {"op": "add_anim_folder", "page": 2, "id": "bf"},
  {"op": "add_cel", "page": 2, "folder": "bf", "id": "d1", "at": 2},
  {"op": "add_stroke", "page": 2, "layer_id": "d1", "points": [[140, 220], [180, 270]], "kind": "gpen", "width_mm": 3},
  {"op": "add_stroke", "page": 2, "layer": "ink", "points": [[20, 30], [190, 35]], "kind": "mili"}
]},
{"agent": "human:作者", "ops": [
  {"op": "set_exposures", "page": 2, "folder": "af", "cels": [[1, "c2"], [4, "c1"], [6, null]]},
  {"op": "set_light_table", "page": 2, "cels": ["c3"]},
  {"op": "set_camera_key", "page": 2, "frame": 1, "rect": [10, 10, 150, 200]}
]}
])";

}  // namespace

class TestContractAnim : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString py_book_;                 // (drawnAsTheFirstFrame's books, for writtenOutLikePython)
    std::optional<genko::core::Document> doc_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    // (a test function compared with Python: skipped without the reference — the others run)
#define needs_python()                                                                                                    \
    do {                                                                                                                  \
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python"); \
    } while (false)

    genko::test::Run python(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args, genko::test::python_env(path("pyenv")));
    }

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

    void opsLikePython() {
        needs_python();
        const QString pyenv = path("pyenv");
        const QString opsbook = path("opsbook.genko");
        const auto made = genko::test::harness({"make-opsbook", opsbook}, pyenv, 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/anim_cases.json");
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        const QString dir = path("ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = opsbook.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            if (cases[n]["ok"].get<bool>()) job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, pyenv, 1200000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(opsbook.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, int> same;  // op → cases the same as Python's
        int read_back = 0;
        int refused = 0;
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            Json records = genko::test::read_json(QString::fromStdString(jobs[n]["out"].get<std::string>()));
            Json reread;
            if (!records.empty() && records.back().contains("reread")) {
                reread = records.back();
                records.erase(records.size() - 1);
            }
            genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-store-%1").arg(n)).toStdString()));
            genko::core::Document last;
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &last);
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            if (records.back()["reply"]["ok"].get<bool>() != c["ok"].get<bool>()) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(records.back()["reply"]).substr(0, 400));
                continue;
            }
            bool ok = true;
            for (std::size_t s = 0; s < outcomes.size() && ok; ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    ok = false;
                }
            }
            if (!ok) continue;
            ++same[c["op"].get<std::string>()];
            if (!c["ok"].get<bool>()) {
                ++refused;
                continue;
            }
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [op, n] : same) qInfo("%s: %d cases the same as Python", op.c_str(), n);
        qInfo("refused as Python refuses: %d; saved and read back as they were: %d", refused, read_back);
        QVERIFY(failures.empty());
        for (const char* op : {"set_animation", "add_anim_folder", "add_cel", "set_exposure", "set_exposures", "set_camera_key",
                               "set_light_table"}) {
            QVERIFY2(same[op] >= 3, op);
        }
    }

    void drawnAsTheFirstFrame() {
        needs_python();
        const QString base = path("base.genko");
        const auto made = genko::test::harness({"make-drawbook", base}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json steps = genko::core::parse_python_json(kSteps);

        const QString py_book = py_book_ = path("py.genko");
        Json job = Json::object();
        job["op"] = "steps";
        job["book"] = base.toStdString();
        job["ids"] = true;
        job["first_id"] = kFirstId;
        job["store"] = path("py-store").toStdString();
        job["steps"] = steps;
        job["out"] = path("py-steps.json").toStdString();
        job["reread"] = py_book.toStdString();
        genko::test::write_bytes(path("jobs.json"), genko::core::dump_python(Json::array({job})));
        const auto ran = genko::test::harness({"batch", path("jobs.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(path("py-steps.json"));

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
            const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
            QVERIFY2(diff.empty(), ("step " + std::to_string(s) + ": " + diff.substr(0, 1500)).c_str());
        }
        const QString cpp_book = path("cpp.genko");
        const fs::path dir = genko::storage::path_from_utf8(cpp_book.toStdString());
        fs::create_directories(dir);
        genko::storage::ProjectLock lock(dir, "human:作者");
        lock.try_acquire();
        genko::storage::SaveRequest request;
        request.actor = "human:作者";
        request.ops = Json::array();
        genko::storage::Saver(lock).save(doc, request);
        doc_ = doc;

        int compared = 0;
        for (const char* mode : {"print", "proof"}) {
            const QString what = QStringLiteral("page 2 %1").arg(mode);
            const QString out = path("out/page.png");
            const QStringList tail{"--page", "2", "--dpi", "100", "--mode", mode, "--out", out};
            QFile::remove(out);
            const auto py = python(QStringList{"render", py_book} + tail);
            QVERIFY2(py.finished && py.exit_code == 0, (what + ": " + py.err.right(2000)).toUtf8().constData());
            const genko::render::Image want = genko::render::read_png(genko::test::read_bytes(out));
            QFile::remove(out);
            const auto got = genko::test::run_genko(QStringList{"render", cpp_book} + tail);
            QVERIFY2(got.finished && got.exit_code == 0, (what + ": " + got.out + got.err).toUtf8().constData());
            QCOMPARE(got.out, py.out);  // the same JSON, byte for byte
            const genko::render::Image image = genko::render::read_png(genko::test::read_bytes(out));
            if (image.size() != want.size() || image.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(image, want);
                genko::test::keep_pictures(QStringLiteral("anim-p2-%1").arg(mode), image, want);
                QFAIL(qPrintable(what + QStringLiteral(": %1 pixels differ (largest %2)").arg(diff.pixels).arg(diff.largest)));
            }
            ++compared;
        }
        QCOMPARE(compared, 2);

        // (the cels not shown are left out: every cel drawn is another picture)
        const genko::core::Page& page = doc.page(1);
        QVERIFY(genko::core::anim::is_animation(page));
        QCOMPARE(genko::core::anim::cel_at(page, Json("af"), 1), Json("c2"));
        QCOMPARE(genko::core::anim::cel_at(page, Json("bf"), 1), Json());
        genko::render::RenderOptions as_it_is;
        as_it_is.at_frame = true;  // (every cel at once)
        const auto framed = genko::render::render_page(page, 100, {}, &doc).image;
        const auto every = genko::render::render_page(page, 100, as_it_is, &doc).image;
        QVERIFY(genko::test::pixel_diff(framed, every).pixels > 500);
        const auto first = genko::render::render_page(genko::core::anim::at_frame(page, 1), 100, as_it_is, &doc).image;
        QCOMPARE(genko::test::pixel_diff(framed, first).pixels, 0);
    }

    void writtenOutLikePython() {
        needs_python();
        QVERIFY2(doc_.has_value(), "drawnAsTheFirstFrame makes the books");
        const QString py_dir = path("movies-py"), cpp_dir = path("movies-cpp");
        QDir().mkpath(py_dir);
        QDir().mkpath(cpp_dir);
        const auto py = genko::test::harness({"anim-movies", py_book_, "2", py_dir, path("movies-py.json")}, path("pyenv"));
        QVERIFY2(py.finished && py.exit_code == 0, py.err.right(4000).constData());
        const Json want = genko::test::read_json(path("movies-py.json"));
        Json cpp_errors = Json::object();
        std::map<std::string, std::string> written;
        QStringList paths;
        for (const char* fmt : {"gif", "png", "webp", "frames", "mp4"}) {
            const QString dest = cpp_dir + (std::string(fmt) == "frames" ? QStringLiteral("/frames") : QStringLiteral("/movie.%1").arg(fmt));
            try {
                const auto files = genko::render::anim::export_animation(doc_->page(1), genko::storage::path_from_utf8(dest.toStdString()),
                                                                         &*doc_, 20, std::string(fmt));
                written[fmt] = std::string(fmt) == "frames" ? dest.toStdString() : genko::core::path_to_utf8(files.front());
                paths << QString::fromStdString(written[fmt]);
            } catch (const genko::core::Error& e) {
                cpp_errors[fmt] = Json::array({"ValueError", e.what()});
            }
        }
        const auto read = genko::test::harness(QStringList{"movie-info", path("movies-cpp.json")} + paths, path("pyenv"));
        QVERIFY2(read.finished && read.exit_code == 0, read.err.right(4000).constData());
        const Json got = genko::test::read_json(path("movies-cpp.json"));
        int compared = 0;
        for (const char* fmt : {"gif", "png", "webp", "frames", "mp4"}) {
            const Json& expected = want[fmt];
            if (expected.contains("error")) {  // (MP4 without ffmpeg: the same refusal)
                QVERIFY2(cpp_errors.contains(fmt), fmt);
                QCOMPARE(QString::fromStdString(cpp_errors[fmt][1].get<std::string>()), QString::fromStdString(expected["error"][1].get<std::string>()));
                continue;
            }
            QVERIFY2(written.contains(fmt), (std::string(fmt) + ": " + cpp_errors.dump()).c_str());
            const Json& mine = got[written[fmt]];
            std::vector<std::string> keys;
            if (std::string(fmt) == "mp4") {
                keys = {"probed", "nb_read_frames", "r_frame_rate", "width", "height", "codec_name", "pix_fmt"};
            } else if (std::string(fmt) == "frames") {
                keys = {"names", "sizes", "frames"};
            } else if (std::string(fmt) == "webp") {
                keys = {"format", "size", "n_frames", "loop", "durations", "sizes"};
            } else {  // (strict: a reader stricter than Pillow — every PNG chunk's CRC, every GIF code — finds nothing wrong)
                keys = {"format", "size", "n_frames", "loop", "durations", "sizes", "frames", "strict"};
            }
            for (const std::string& key : keys) {
                QVERIFY2(mine.value(key, Json()) == expected.value(key, Json()),
                         (std::string(fmt) + " " + key + ": Python " + expected.value(key, Json()).dump() + ", this build " + mine.value(key, Json()).dump()).c_str());
            }
            ++compared;
        }
        QVERIFY(compared >= 4);
        QVERIFY(want["gif"]["n_frames"].get<int>() >= 2);  // (6 frames, the same ones in a row shown longer)
    }

    // The timelapse: the same ops through `genko apply` and `python -m genko apply` (turned on: every page; a line on
    // page 1, a note on page 2: that page; the same note again, the book's title: none), each save leaving its picture,
    // then written out as a GIF: the recording the same (the order, the pages, the file names, each picture's size: about
    // 720 px on the long side), and the GIF's frames and timing the same. (The JPEG pixels are each encoder's own.)
    void timelapseLikePython() {
        needs_python();
        const QString py_book = path("lapse-py"), cpp_book = path("lapse-cpp");
        const auto made_py = python({"new", py_book, "--pages", "2"});
        QVERIFY2(made_py.finished && made_py.exit_code == 0, made_py.err.right(2000).constData());
        // a line of dialogue on page 2 first (written by Python), and this build's book converted from that one: a page
        // with lines is recorded too
        {
            const QString file = path("lapse-line.json");
            genko::test::write_bytes(file, R"([{"op": "add_line", "page": 2, "text": "台詞です", "x_mm": 50, "y_mm": 50}])");
            const auto line = python({"apply", py_book, file});
            QVERIFY2(line.finished && line.exit_code == 0, (line.out + line.err.right(2000)).constData());
        }
        const auto made_cpp = genko::test::run_genko({"migrate", py_book, cpp_book});
        QVERIFY2(made_cpp.finished && made_cpp.exit_code == 0, (made_cpp.out + made_cpp.err).constData());
        const char* const steps[] = {
            R"([{"op": "set_timelapse", "on": true}])",
            R"([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 20], [80, 90]]}])",
            R"([{"op": "set_note", "page": 2, "note": "x"}])",
            R"([{"op": "set_note", "page": 2, "note": "x"}])",
            R"([{"op": "set_meta", "title": "記録"}])",
        };
        for (const char* ops : steps) {
            const QString file = path("lapse-ops.json");
            genko::test::write_bytes(file, ops);
            const auto py = python({"apply", py_book, file});
            QVERIFY2(py.finished && py.exit_code == 0, (QByteArray(ops) + ": " + py.out + py.err.right(2000)).constData());
            const auto cpp = genko::test::run_genko({"apply", cpp_book, file});
            QVERIFY2(cpp.finished && cpp.exit_code == 0, (QByteArray(ops) + ": " + cpp.out + cpp.err).constData());
        }
        const auto frames_of = [](const QString& book) {
            Json out = Json::array();
            for (const Json& item : genko::render::timelapse::frames(genko::storage::path_from_utf8(book.toStdString()))) {
                out.push_back(Json::array({item["n"], item["page"], item["file"]}));
            }
            return out;
        };
        const Json want = frames_of(py_book), got = frames_of(cpp_book);
        QCOMPARE(QString::fromStdString(got.dump()), QString::fromStdString(want.dump()));
        QCOMPARE(want.size(), std::size_t{4});
        const auto size_of = [](const QString& book, const Json& item) {
            const auto file = genko::render::timelapse::folder(genko::storage::path_from_utf8(book.toStdString())) /
                              genko::storage::path_from_utf8(item[2].get<std::string>());
            return genko::render::open_image(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(file)))).size();
        };
        for (const Json& item : got) {
            const genko::render::Size mine = size_of(cpp_book, item), theirs = size_of(py_book, item);
            QCOMPARE(mine.width, theirs.width);  // (the long side about 720 px: its dpi rounded, as Python's)
            QCOMPARE(mine.height, theirs.height);
            QVERIFY(std::abs(std::max(mine.width, mine.height) - 720) <= 10);
        }
        // written out
        const QString py_gif = path("lapse-py.gif"), cpp_gif = path("lapse-cpp.gif");
        const auto py = python({"export", py_book, py_gif, "--format", "timelapse"});
        QVERIFY2(py.finished && py.exit_code == 0, py.err.right(2000).constData());
        int frames = 0;
        genko::render::timelapse::export_timelapse(genko::storage::path_from_utf8(cpp_book.toStdString()),
                                                   genko::storage::path_from_utf8(cpp_gif.toStdString()), std::nullopt, 12, std::nullopt,
                                                   std::nullopt, 2.0, &frames);
        QCOMPARE(frames, 4);
        const auto read = genko::test::harness({"movie-info", path("lapse.json"), py_gif, cpp_gif}, path("pyenv"));
        QVERIFY2(read.finished && read.exit_code == 0, read.err.right(2000).constData());
        const Json info = genko::test::read_json(path("lapse.json"));
        for (const char* key : {"format", "size", "n_frames", "loop", "durations", "sizes", "strict"}) {
            QVERIFY2(info[py_gif.toStdString()][key] == info[cpp_gif.toStdString()][key],
                     (std::string(key) + ": Python " + info[py_gif.toStdString()][key].dump() + ", this build " +
                      info[cpp_gif.toStdString()][key].dump()).c_str());
        }
    }

    // GIF's codes read as strictly as giflib reads them (Pillow forgives a code of the wrong width at the end): random
    // pictures of every kind of colour count and length, the table filled and cleared, each decoded to the same pixels.
    void gifCodesReadStrictly() {
        std::mt19937 rng(11);
        const auto decode = [](const std::string& stream, int min_bits) -> std::optional<std::vector<std::uint8_t>> {
            std::string data;
            for (std::size_t at = 0; at < stream.size() && stream[at] != 0;) {
                const auto n = static_cast<std::size_t>(static_cast<unsigned char>(stream[at]));
                data.append(stream, at + 1, n);
                at += n + 1;
            }
            const int clear = 1 << min_bits, end = clear + 1;
            int size = min_bits + 1;
            std::vector<std::string> table;
            const auto reset = [&] {
                table.clear();
                for (int i = 0; i < clear; ++i) table.push_back(std::string(1, static_cast<char>(i)));
                table.emplace_back();
                table.emplace_back();
                size = min_bits + 1;
            };
            reset();
            std::vector<std::uint8_t> out;
            int prev = -1;
            std::size_t bit = 0;
            for (;;) {
                if (bit + static_cast<std::size_t>(size) > data.size() * 8) return std::nullopt;
                int code = 0;
                for (int k = 0; k < size; ++k, ++bit)
                    code |= ((static_cast<unsigned char>(data[bit / 8]) >> (bit % 8)) & 1) << k;
                if (code == clear) {
                    reset();
                    prev = -1;
                    continue;
                }
                if (code == end) break;
                std::string entry;
                if (prev < 0) {
                    if (code >= clear) return std::nullopt;
                    entry = table[static_cast<std::size_t>(code)];
                } else if (code < static_cast<int>(table.size())) {
                    entry = table[static_cast<std::size_t>(code)];
                    if (table.size() < 4096) table.push_back(table[static_cast<std::size_t>(prev)] + entry.substr(0, 1));
                } else if (code == static_cast<int>(table.size()) && table.size() < 4096) {
                    entry = table[static_cast<std::size_t>(prev)] + table[static_cast<std::size_t>(prev)].substr(0, 1);
                    table.push_back(entry);
                } else {
                    return std::nullopt;
                }
                for (const char c : entry) out.push_back(static_cast<std::uint8_t>(c));
                prev = code;
                if (prev >= 0 && static_cast<int>(table.size()) == (1 << size) && size < 12) ++size;
            }
            return out;
        };
        int checked = 0;
        for (int t = 0; t < 3000; ++t) {
            const int min_bits = std::array<int, 5>{2, 3, 4, 7, 8}[rng() % 5];
            const std::size_t n = std::array<std::size_t, 7>{1, 2, 5, 50, 300, 3000, 20000}[rng() % 7];
            const int colours = std::array<int, 3>{2, 3, 1 << min_bits}[rng() % 3];
            std::vector<std::uint8_t> indices(n);
            for (auto& v : indices) v = static_cast<std::uint8_t>(rng() % static_cast<unsigned>(std::min(colours, 1 << min_bits)));
            const auto back = decode(genko::render::movie::gif_lzw(indices, min_bits), min_bits);
            QVERIFY2(back.has_value(), qPrintable(QStringLiteral("stream %1 (%2 bits, %3 pixels) does not decode").arg(t).arg(min_bits).arg(n)));
            QVERIFY2(*back == indices, qPrintable(QStringLiteral("stream %1 decodes to other pixels").arg(t)));
            ++checked;
        }
        QCOMPARE(checked, 3000);
    }

    // PC5 J-e1 (timeline@1, SPEC.md PROD-01), beyond Python: anim_cases.json's timeline_cases — set_animation's work area
    // and its fps of 0.5..120 on a new book: what the anim holds after them, a refusal's words, the question timeline_info
    // answering the same work area and changing nothing, and the book saved naming timeline@1 only while the page has a
    // work area or an fps past 1..60 — read back as written, writable, the same bytes written again. A book whose anim was
    // made with the values before (none, null) is written byte for byte as without them.
    void timelineBeyondPython() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/anim_cases.json");
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor actor("human:作者");
        const genko::core::Document base = genko::core::new_episode("タイムライン", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        int checked = 0, named = 0;
        for (const Json& c : file["timeline_cases"]) {
            const std::string name = c["n"].get<std::string>();
            genko::core::Document doc = base;
            std::string error;
            bool ok = true;
            for (const Json& step : c["steps"]) {
                try {
                    doc = bus.apply(doc, step, actor, false).doc;
                } catch (const genko::core::ApplyError& e) {
                    ok = false;
                    error = e.what();
                    break;
                }
            }
            QVERIFY2(ok == c["ok"].get<bool>(), (name + ": " + error).c_str());
            if (!ok) {
                QVERIFY2(error.find(c["error"].get<std::string>()) != std::string::npos, (name + ": " + error).c_str());
                ++checked;
                continue;
            }
            const genko::core::Page& page = doc.page(0);
            const Json* anim = genko::core::anim::spec(page);
            QVERIFY2(anim != nullptr, name.c_str());
            for (const auto& [key, value] : c["anim"].items()) {
                QVERIFY2(anim->contains(key) && anim->at(key) == value, (name + ": " + key + " " + anim->dump()).c_str());
            }
            if (c.value("no_work", false)) QVERIFY2(!anim->contains("work"), (name + ": " + anim->dump()).c_str());
            // the question: the work area as the anim holds it, nothing changed
            const auto asked = bus.apply(doc, Json::array({Json{{"op", "timeline_info"}, {"page", 1}}}), actor, false);
            QVERIFY(asked.unchanged);
            QCOMPARE(asked.results.size(), std::size_t{1});
            QCOMPARE(asked.results[0]["work"], anim->contains("work") ? anim->at("work") : Json());
            QCOMPARE(asked.results[0]["timeline@1"], c["timeline"]);
            // saved and read back
            const fs::path dir = genko::storage::path_from_utf8(path(QStringLiteral("timeline/%1").arg(checked)).toStdString());
            genko::test::write_project(doc, dir);
            const std::string bytes = genko::storage::read_file(dir / "project.json");
            const Json project = genko::core::parse_python_json(bytes);
            const auto features = project.value("features", Json::array());
            const bool names = std::find(features.begin(), features.end(), Json("timeline@1")) != features.end();
            QVERIFY2(names == c["timeline"].get<bool>(), (name + ": " + features.dump()).c_str());
            named += names ? 1 : 0;
            const auto back = genko::storage::load_document(dir);
            QVERIFY2(back.document.read_only_reason.empty(), (name + ": " + back.document.read_only_reason).c_str());
            QVERIFY2(genko::core::anim::spec(back.document.page(0)) != nullptr && *genko::core::anim::spec(back.document.page(0)) == *anim, name.c_str());
            genko::test::write_project(back.document, dir);
            QVERIFY2(genko::storage::read_file(dir / "project.json") == bytes, name.c_str());
            ++checked;
        }
        QCOMPARE(checked, static_cast<int>(file["timeline_cases"].size()));
        QVERIFY(named >= 8);
        // the values before: an anim made with "work": null written as one made without it
        const auto written = [&](const char* ops, const QString& where) {
            const genko::core::Document doc = bus.apply(base, Json::parse(ops), actor, false).doc;
            const fs::path dir = genko::storage::path_from_utf8(path(where).toStdString());
            genko::test::write_project(doc, dir);
            return genko::storage::read_file(dir / "project.json");
        };
        const std::string plain = written(R"([{"op": "set_animation", "page": 1, "fps": 24, "frames": 30}])", QStringLiteral("timeline/plain"));
        QCOMPARE(written(R"([{"op": "set_animation", "page": 1, "fps": 24, "frames": 30, "work": null}])", QStringLiteral("timeline/null")), plain);
        QVERIFY(plain.find("timeline@1") == std::string::npos && plain.find("\"work\"") == std::string::npos);
        // the work area taken: never said to be ignored
        QVERIFY(bus.apply(base, Json::parse(R"([{"op": "set_animation", "page": 1, "work": [1, 3]}])"), actor, false).warnings.empty());
        // an agent through MCP (the coordinator's decision on the review's P2-1: core::PublicAnimationOps): the public list's
        // set_animation — an fps past 1..60 refused with Python's words, a work area an unknown key, ignored and said so
        {
            const genko::core::PublicAnimationOps agents;
            for (const char* fps : {"61", "0.5", "120"}) {
                try {
                    (void)bus.apply(base, Json::parse(std::string(R"([{"op": "set_animation", "page": 1, "fps": )") + fps + "}]"), actor, false);
                    QFAIL(fps);
                } catch (const genko::core::ApplyError& e) {
                    QVERIFY2(std::string(e.what()).find("set_animation: fps is 1 to 60") != std::string::npos, e.what());
                }
            }
            const auto ignored = bus.apply(base, Json::parse(R"([{"op": "set_animation", "page": 1, "fps": 24, "work": [1, 3]}])"), actor, false);
            QVERIFY(!genko::core::anim::spec(ignored.doc.page(0))->contains("work"));
            QCOMPARE(ignored.warnings, Json::array({"ops[0] set_animation: unknown keys ignored: work"}));
        }
        QVERIFY(!genko::core::public_animation_ops());
    }

    // PC5 J-e1 (file.export.renderVideo, timeline.setFrame, timeline.info): the page of drawnAsTheFirstFrame (6 frames at 8
    // fps; made here by this build's ops, without Python) written out for a range of its frames — the numbered PNGs of frames
    // 2..4 the same files as frames 2..4 of all of them, counted from frame_0001 —, at another speed (each frame shown 1/24 s,
    // 2 s at 0.5 fps; a picture shown past GIF's 655.35 s written again for the rest), given up while it is written (told
    // each frame, nothing left), a range, a speed or a width outside refused; with a work area of 2..4, `genko export --format animation --work-area` writes the same GIF as the range
    // (without it, all the frames as before; --work-area only for an animation); `genko render --frame 3` draws the page at
    // frame 3 (the frame in its JSON; one past the frames or a page that is not an animation refused); `genko inspect
    // --timeline` says its timeline.
    void workAreaWrittenOut() {
        // (the animation of drawnAsTheFirstFrame, made here on a book of this build: no Python needed)
        const genko::core::CommandBus bus(genko::render::ops_registry());
        genko::core::Document made = genko::core::new_episode("動き", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono());
        for (const Json& step : genko::core::parse_python_json(kSteps)) made = bus.apply(made, step["ops"], genko::core::Actor("human:作者"), false).doc;
        const genko::core::Document& animated = made;
        const genko::core::Page& page = animated.page(1);
        namespace ranim = genko::render::anim;
        const QString all_dir = path("range/all"), part_dir = path("range/part");
        const auto all = ranim::export_animation(page, genko::storage::path_from_utf8(all_dir.toStdString()), &animated, 20, std::string("frames"));
        const auto part = ranim::export_animation(page, genko::storage::path_from_utf8(part_dir.toStdString()), &animated, 20, std::string("frames"), true,
                                                  "trim", std::nullopt, std::array<std::int64_t, 2>{2, 4});
        QCOMPARE(all.size(), std::size_t{6});
        QCOMPARE(part.size(), std::size_t{3});
        for (std::size_t i = 0; i < part.size(); ++i) {
            QCOMPARE(genko::core::path_to_utf8(part[i].filename()), "frame_000" + std::to_string(i + 1) + ".png");
            QVERIFY(genko::storage::read_file(part[i]) == genko::storage::read_file(all[i + 1]));
        }
        // the speed
        const QString fast = path("range/fast.gif"), slow = path("range/slow.gif");
        (void)ranim::export_animation(page, genko::storage::path_from_utf8(fast.toStdString()), &animated, 20, std::nullopt, true, "trim", std::nullopt,
                                      std::array<std::int64_t, 2>{2, 5}, 24.0);
        (void)ranim::export_animation(page, genko::storage::path_from_utf8(slow.toStdString()), &animated, 20, std::nullopt, true, "trim", std::nullopt,
                                      std::nullopt, 0.5);
        // (each GIF frame's delay, in 1/100 s, from its graphic control block)
        const auto delays = [](const QString& gif) {
            const std::string bytes = genko::test::read_bytes(gif);
            std::vector<int> out;
            for (std::size_t at = bytes.find("\x21\xf9\x04", 0, 3); at != std::string::npos && at + 6 <= bytes.size();
                 at = bytes.find("\x21\xf9\x04", at + 6, 3)) {
                out.push_back(static_cast<unsigned char>(bytes[at + 4]) | static_cast<unsigned char>(bytes[at + 5]) << 8);
            }
            return out;
        };
        const auto total = [](const std::vector<int>& cs) {
            int sum = 0;
            for (const int d : cs) sum += d * 10;
            return sum;
        };
        QCOMPARE(total(delays(fast)), 4 * 40);  // (1/24 s: 42 ms, in GIF's 1/100 s)
        QCOMPARE(total(delays(slow)), 6 * 2000);
        // (a picture shown longer than GIF's 655.35 s — the same one 400 times at 0.5 fps —: written twice, its time whole)
        {
            genko::core::Document still = genko::core::new_episode("静止", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
            still = bus.apply(still, Json::parse(R"([{"op": "set_animation", "page": 1, "frames": 400, "fps": 0.5}])"), genko::core::Actor("human:作者"), false).doc;
            const QString long_gif = path("range/long.gif");
            (void)ranim::export_animation(still.page(0), genko::storage::path_from_utf8(long_gif.toStdString()), &still, 5);
            QCOMPARE(delays(long_gif), (std::vector<int>{65535, 400 * 200 - 65535}));
        }
        // given up while it is written: nothing left; told each frame
        {
            std::vector<std::int64_t> told;
            const QString stopped = path("range/stopped.gif");
            try {
                (void)ranim::export_animation(page, genko::storage::path_from_utf8(stopped.toStdString()), &animated, 20, std::nullopt, true, "trim",
                                              std::nullopt, std::nullopt, std::nullopt, [&told](std::int64_t done, std::int64_t of) {
                                                  told.push_back(done * 100 + of);
                                                  return done < 3;
                                              });
                QFAIL("written");
            } catch (const genko::core::Error& e) {
                QCOMPARE(std::string(e.code()), std::string("cancelled"));
            }
            QCOMPARE(told, (std::vector<std::int64_t>{106, 206, 306}));
            QVERIFY(!QFile::exists(stopped));
            QCOMPARE(QDir(path("range")).entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden).size(), 0);
        }
        // a width past an op's picture: refused before it is made
        try {
            (void)ranim::export_animation(page, genko::storage::path_from_utf8(path("range/wide.gif").toStdString()), &animated, 20, std::nullopt, true,
                                          "trim", 200000);
            QFAIL("written");
        } catch (const genko::core::Error& e) {
            QVERIFY2(std::string(e.what()).find("the video's frame is too large") != std::string::npos, e.what());
        }
        for (const auto& [range, fps] : {std::pair{std::array<std::int64_t, 2>{0, 3}, 12.0}, std::pair{std::array<std::int64_t, 2>{4, 7}, 12.0},
                                         std::pair{std::array<std::int64_t, 2>{4, 2}, 12.0}, std::pair{std::array<std::int64_t, 2>{1, 6}, 121.0}}) {
            try {
                (void)ranim::export_animation(page, genko::storage::path_from_utf8(path("range/no.gif").toStdString()), &animated, 20, std::nullopt, true,
                                              "trim", std::nullopt, range, fps);
                QFAIL("written");
            } catch (const genko::core::Error& e) {
                const std::string said = e.what();
                QVERIFY2(said == "range is [first, last] of the frames 1 to 6" || said == "fps is 0.5 to 120", e.what());
            }
        }
        QVERIFY(!QFile::exists(path("range/no.gif")));
        // a work area of 2..4: the command line's --work-area
        const genko::core::Document worked =
            bus.apply(animated, Json::parse(R"([{"op": "set_animation", "page": 2, "work": [2, 4]}])"), genko::core::Actor("human:作者"), false).doc;
        const QString book = path("worked.genko");
        genko::test::write_project(worked, genko::storage::path_from_utf8(book.toStdString()));
        const QString want = path("range/want.gif");
        (void)ranim::export_animation(worked.page(1), genko::storage::path_from_utf8(want.toStdString()), &worked, 100, std::nullopt, true, "trim",
                                      std::nullopt, std::array<std::int64_t, 2>{2, 4});
        const QString got = path("range/cli.gif");
        const auto cli = genko::test::run_genko({"export", book, got, "--format", "animation", "--page", "2", "--work-area"});
        QVERIFY2(cli.finished && cli.exit_code == 0, (cli.out + cli.err).constData());
        QVERIFY(genko::test::read_bytes(got) == genko::test::read_bytes(want));
        const QString every = path("range/every.gif"), every_want = path("range/every-want.gif");
        const auto plain = genko::test::run_genko({"export", book, every, "--format", "animation", "--page", "2"});
        QVERIFY2(plain.finished && plain.exit_code == 0, (plain.out + plain.err).constData());
        (void)ranim::export_animation(worked.page(1), genko::storage::path_from_utf8(every_want.toStdString()), &worked, 100);
        QVERIFY(genko::test::read_bytes(every) == genko::test::read_bytes(every_want));
        QVERIFY(genko::test::read_bytes(every) != genko::test::read_bytes(got));
#ifdef Q_OS_LINUX
        {  // (PC6, the review's P3-4) genko killed by Ctrl+C while ffmpeg — in a process group of its own — works on its MP4:
           // ffmpeg goes with it (its parent's death its SIGKILL), not left running for half an hour
            const QString bin = path("fake-ffmpeg");
            QVERIFY(QDir().mkpath(bin));
            const QString started = bin + "/pid";
            {
                QFile script(bin + "/ffmpeg");
                QVERIFY(script.open(QIODevice::WriteOnly));
                script.write(("#!/bin/sh\necho $$ > '" + started + "'\nexec sleep 120\n").toUtf8());
                script.close();
                QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            }
            QProcess killed;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PATH"), bin + ":" + env.value(QStringLiteral("PATH")));
            killed.setProcessEnvironment(env);
            killed.start(genko::test::genko_cli(), {"export", book, path("range/killed.mp4"), "--format", "animation", "--page", "2"});
            QVERIFY(killed.waitForStarted(30000));
            QTRY_VERIFY_WITH_TIMEOUT(QFile(started).size() > 0, 120000);
            QFile pid_file(started);
            QVERIFY(pid_file.open(QIODevice::ReadOnly));
            const QString pid = QString::fromLatin1(pid_file.readAll()).trimmed();
            QVERIFY(::kill(static_cast<pid_t>(killed.processId()), SIGINT) == 0);
            QVERIFY(killed.waitForFinished(30000));
            const auto gone = [&pid] {
                QFile stat(QStringLiteral("/proc/") + pid + QStringLiteral("/stat"));
                if (!stat.open(QIODevice::ReadOnly)) return true;
                const QByteArray line = stat.readAll();
                const qsizetype close = line.lastIndexOf(')');
                return close >= 0 && close + 2 < line.size() && (line[close + 2] == 'Z' || line[close + 2] == 'X');
            };
            QTRY_VERIFY_WITH_TIMEOUT(gone(), 10000);
        }
#endif
        const auto wrong = genko::test::run_genko({"export", book, path("range/x.png"), "--format", "png", "--work-area"});
        QCOMPARE(wrong.exit_code, 2);
        QVERIFY2(wrong.err.contains("--work-area: only with --format animation"), wrong.err.constData());
        // genko render --frame
        const QString out = path("range/frame3.png");
        const auto drawn = genko::test::run_genko({"render", book, "--page", "2", "--dpi", "40", "--frame", "3", "--out", out});
        QVERIFY2(drawn.finished && drawn.exit_code == 0, (drawn.out + drawn.err).constData());
        QCOMPARE(genko::core::parse_python_json(drawn.out.toStdString())["frame"], Json(3));
        genko::render::RenderOptions at;
        at.at_frame = true;
        const auto expected = genko::render::render_page(genko::core::anim::at_frame(worked.page(1), 3), 40, at, &worked).image;
        const auto image = genko::render::read_png(genko::test::read_bytes(out));
        QVERIFY(image.size() == expected.size() && image.tobytes() == expected.tobytes());
        const auto first = genko::render::render_page(worked.page(1), 40, {}, &worked).image;  // (without --frame: frame 1, as before)
        QVERIFY(genko::test::pixel_diff(image, first).pixels > 0);
        const auto past = genko::test::run_genko({"render", book, "--page", "2", "--frame", "7", "--out", out});
        QCOMPARE(past.exit_code, 1);
        QVERIFY2((past.out + past.err).contains("frame is 1 to 6"), (past.out + past.err).constData());
        const auto plain_page = genko::test::run_genko({"render", book, "--page", "1", "--frame", "1", "--out", out});
        QCOMPARE(plain_page.exit_code, 1);
        QVERIFY2((plain_page.out + plain_page.err).contains("not an animation"), (plain_page.out + plain_page.err).constData());
        // genko inspect --timeline
        const auto inspected = genko::test::run_genko({"inspect", book, "--timeline", "--page", "2"});
        QVERIFY2(inspected.finished && inspected.exit_code == 0, (inspected.out + inspected.err).constData());
        const Json said = genko::core::parse_python_json(inspected.out.toStdString())["pages"][0];
        QCOMPARE(said["page"], Json(2));
        QCOMPARE(said["animation"], Json(true));
        QCOMPARE(said["frames"], Json(6));
        QCOMPARE(said["fps"].get<double>(), 8.0);
        QCOMPARE(said["work"], Json::array({2, 4}));
        QCOMPARE(said["played"], Json::array({2, 4}));
        QCOMPARE(said["seconds"].get<double>(), 0.75);
        QCOMPARE(said["tracks"].size(), std::size_t{2});
        QCOMPARE(said["tracks"][0]["folder"], Json("af"));
        QCOMPARE(said["tracks"][0]["name"], Json("動き"));
        QCOMPARE(said["camera_keys"], Json::array({1}));
        QCOMPARE(said["light_table"], Json::array({"c3"}));
        QCOMPARE(said["timeline@1"], Json(true));
        const auto pages = genko::test::run_genko({"inspect", book, "--timeline"});
        QVERIFY2(pages.finished && pages.exit_code == 0, (pages.out + pages.err).constData());
        QCOMPARE(genko::core::parse_python_json(pages.out.toStdString())["pages"][0], (Json{{"page", 1}, {"animation", false}}));
        const auto mixed = genko::test::run_genko({"inspect", book, "--timeline", "--slices"});
        QCOMPARE(mixed.exit_code, 2);
    }

    // Beyond Python: a camera rect that is not finite is refused (Python keeps NaN and writes it into the book).
    void cameraNotFiniteRefused() {
        genko::core::Document doc = genko::core::new_episode("カメラ", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        const genko::core::CommandBus bus(genko::render::ops_registry());
        doc = bus.apply(doc, Json::parse(R"([{"op": "set_animation", "page": 1}])"), genko::core::Actor("human:作者"), false).doc;
        for (const char* rect : {R"(["nan", 0, 50, 50])", R"([0, "inf", 50, 50])", R"([0, 0, "inf", 50])"}) {
            const Json op = Json::array({Json{{"op", "set_camera_key"}, {"page", 1}, {"frame", 1}, {"rect", Json::parse(rect)}}});
            try {
                (void)bus.apply(doc, op, genko::core::Actor("human:作者"), false);
                QFAIL(rect);
            } catch (const genko::core::ApplyError& e) {
                QVERIFY2(QString::fromUtf8(e.what()).contains(QStringLiteral("rect must be a finite number")), e.what());
            }
        }
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "1700000");
    QCoreApplication app(argc, argv);
    TestContractAnim test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_anim.moc"
