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
//  - and beyond Python: a camera rect that is not finite is refused (Python would write NaN into the book).
// Skipped without the Python reference.

#include <QtTest>

#include <array>
#include <optional>
#include <random>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
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

    genko::test::Run python(const QStringList& args) {
        return genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args, genko::test::python_env(path("pyenv")));
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void opsLikePython() {
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
