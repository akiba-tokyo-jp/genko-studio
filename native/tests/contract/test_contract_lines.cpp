// The line ops of M4 (Python's ops._apply_one: add_line, edit_line, move_line, delete_line, reorder_lines, cut_balloon,
// set_balloon_path; bookops.replace_text; and stamp_material's lines: a lettering material, 描き文字, and a picture
// material made a line's balloon, 画像のフキダシ) against Python's apply_ops: the cases of contract/lines_cases.json on the
// book `pyref_harness.py make-opsbook` makes (every field of a line and its refusals: the style keys and their bounds,
// tails, ruby, dots, styled words, hand-drawn outlines, the balloon eraser, a moved balloon pushing its tail out, the
// order of a page's lines, find and replace with Python's replacement templates; the built-in letterings and a material
// library's own, with what a library can hold that Python refuses), with a material library both sides read
// (write_library): each step's reply, full snapshot and project.json payload, and the book after the case saved and read
// back. A case marked "cpp" is one this build refuses on purpose (what Python keeps that this build's book cannot hold,
// a picture it does not read or cannot draw, or a regex): C++ must refuse it with that code, its error holding
// "cpp_says". Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "opsupport.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
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

// The person's material library (<config>/materials) the cases stamp from, read by both sides: pictures (u-pic, and ones
// that are not there, hold nothing, are a folder, are not a picture, lie outside the library or are named by a whole
// path), letterings in folders of their own (with a library's odd values: numbers as text, a style as pairs, keys
// and sizes Python refuses), and materials of a kind there is not.
void write_library(const QString& config) {
    const QString root = config + QStringLiteral("/materials");
    QDir().mkpath(root + QStringLiteral("/sub"));
    genko::render::Image picture = genko::render::Image::create("RGBA", {6, 3}, genko::render::Ink{20, 120, 220, 255});
    picture.paste(genko::render::Ink{250, 40, 10, 128}, genko::render::Box{0, 0, 3, 3});
    genko::render::save_png(picture, genko::storage::path_from_utf8((root + QStringLiteral("/u-pic.png")).toStdString()));
    genko::render::save_png(picture, genko::storage::path_from_utf8((config + QStringLiteral("/outside.png")).toStdString()));
    genko::test::write_bytes(root + QStringLiteral("/empty.png"), "");
    genko::test::write_bytes(root + QStringLiteral("/notes.png"), "not a picture");
    const std::string outside = (config + QStringLiteral("/outside.png")).toStdString();
    const Json png = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGM8ISf3n4GBgYGJAQoAHxICB2m00JwAAAAASUVORK5CYII=";
    const Json items = Json::array({
        Json{{"id", "u-pic"}, {"kind", "image"}, {"name", "青い絵"}, {"folder", "画像"}, {"file", "u-pic.png"}, {"width_mm", 30},
             {"aspect", 0.5}},
        Json{{"id", "u-pic-missing"}, {"kind", "image"}, {"name", "ない絵"}, {"file", "nothing.png"}},
        Json{{"id", "u-pic-nofile"}, {"kind", "image"}, {"name", "ファイルのない絵"}},
        Json{{"id", "u-pic-empty"}, {"kind", "image"}, {"name", "空の絵"}, {"file", "empty.png"}},
        Json{{"id", "u-pic-number"}, {"kind", "image"}, {"name", "番号の絵"}, {"file", 5}},
        Json{{"id", "u-pic-folder"}, {"kind", "image"}, {"name", "フォルダの絵"}, {"file", "sub"}},
        Json{{"id", "u-pic-text"}, {"kind", "image"}, {"name", "絵でない"}, {"file", "notes.png"}},
        Json{{"id", "u-pic-outside"}, {"kind", "image"}, {"name", "外の絵"}, {"file", "../outside.png"}},
        Json{{"id", "u-pic-whole-path"}, {"kind", "image"}, {"name", "全体のパスの絵"}, {"file", outside}},
        Json{{"id", "u-let-plain"}, {"kind", "lettering"}, {"name", "ただの描き文字"}, {"folder", "自作の描き文字"}},
        Json{{"id", "u-let-vertical"}, {"kind", "lettering"}, {"name", "ズキューン"}, {"folder", "効果音/強い"}, {"text", "ズキューン"},
             {"balloon", "shout"}, {"wrap", "vertical"},
             {"style", Json{{"size_mm", "7.5"}, {"bold", 1}, {"rgb", Json::array({200, 0, 0})}}},
             {"w_mm", "30"}, {"h_mm", 80}},
        Json{{"id", "u-let-odd"}, {"kind", "lettering"}, {"name", "変わった描き文字"}, {"folder", "自作の描き文字"}, {"text", 123},
             {"balloon", "bogus"}, {"wrap", "diagonal"}, {"w_mm", 0}, {"h_mm", ""},
             {"style", Json::array({Json::array({"font", "gothic"}), Json::array({"size_mm", 5}), Json::array({"font", "mincho"})})}},
        Json{{"id", "u-let-style-letters"}, {"kind", "lettering"}, {"name", "2 文字の組"},
             {"style", Json::array({Json::array({"font", "gothic"}), "fx"})}},
        Json{{"id", "u-let-picture"}, {"kind", "lettering"}, {"name", "絵の描き文字"}, {"text", "絵"},
             {"style", Json{{"picture", png}, {"font", nullptr}, {"tracking", ""}}}},
        Json{{"id", "u-let-unknown-key"}, {"kind", "lettering"}, {"name", "知らない項目"}, {"style", Json{{"size_mm", 4}, {"colour", "red"}}}},
        Json{{"id", "u-let-arc"}, {"kind", "lettering"}, {"name", "曲げすぎ"}, {"style", Json{{"arc", 2}}}},
        Json{{"id", "u-let-zero-width"}, {"kind", "lettering"}, {"name", "幅 0"}, {"w_mm", "0"}},
        Json{{"id", "u-let-height-text"}, {"kind", "lettering"}, {"name", "高さが文字"}, {"h_mm", "abc"}},
        Json{{"id", "u-let-style-text"}, {"kind", "lettering"}, {"name", "設定が文字"}, {"style", "ab"}},
        Json{{"id", "u-let-style-number"}, {"kind", "lettering"}, {"name", "設定が数"}, {"style", 5}},
        Json{{"id", "u-let-style-pair"}, {"kind", "lettering"}, {"name", "設定の組が 3 つ"},
             {"style", Json::array({Json::array({"font", "a", "b"})})}},
        Json{{"id", "u-let-style-not-pair"}, {"kind", "lettering"}, {"name", "設定の組でない"}, {"style", Json::array({7})}},
        Json{{"id", "u-let-style-number-key"}, {"kind", "lettering"}, {"name", "数の項目"},
             {"style", Json::array({Json::array({"size_mm", 3}), Json::array({1, "x"})})}},
        Json{{"id", "u-let-style-list-key"}, {"kind", "lettering"}, {"name", "リストの項目"},
             {"style", Json::array({Json::array({Json::array({1}), "x"})})}},
        Json{{"id", "u-let-infinite"}, {"kind", "lettering"}, {"name", "無限の幅"}, {"w_mm", "inf"}},
        Json{{"id", "u-bogus"}, {"kind", "bogus"}, {"name", "知らない種類"}},
        Json{{"id", "u-no-kind"}, {"name", "種類のない素材"}},
    });
    genko::test::write_bytes(root + QStringLiteral("/library.json"), genko::core::dump_python(items));
}

}  // namespace

class TestContractLines : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void linesLikePython() {
        const QString pyenv = path("pyenv");
        const QString opsbook = path("opsbook.genko");
        const auto made = genko::test::harness({"make-opsbook", opsbook}, pyenv, 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        // (the harness gives Python <pyenv>/config: this side reads the same folder)
        write_library(pyenv + QStringLiteral("/config"));
        qputenv("GENKO_CONFIG_DIR", (pyenv + QStringLiteral("/config")).toUtf8());
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/lines_cases.json");
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
            if (cases[n]["ok"].get<bool>() && !cases[n].contains("cpp")) {
                job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            }
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
        std::map<std::string, std::pair<int, int>> same;  // op → (successes, refusals) the same as Python's
        int read_back = 0;
        int refused_here = 0;
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
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &last);
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            // the case does what it says: its last step succeeds or fails in Python as marked, and a failure is the op's
            // own (but for a case marked "other")
            const Json& last_reply = records.back()["reply"];
            if (last_reply["ok"].get<bool>() != expect_ok) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(last_reply).substr(0, 400));
                continue;
            }
            if (!expect_ok && !last_reply.contains("uncaught") && !c.contains("other")) {
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
                }
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
        qInfo("saved and read back as they were: %d; refused here on purpose: %d", read_back, refused_here);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        const std::map<std::string, std::pair<int, int>> needed{
            {"add_line", {28, 104}},    {"edit_line", {15, 14}},  {"move_line", {17, 8}},         {"delete_line", {5, 6}},
            {"reorder_lines", {9, 10}}, {"cut_balloon", {6, 10}}, {"set_balloon_path", {6, 8}}, {"replace_text", {19, 25}},
            {"stamp_material", {28, 38}}};
        for (const auto& [op, least] : needed) {
            const auto got = same[op];
            QVERIFY2(got.first >= least.first && got.second >= least.second,
                     (op + ": " + std::to_string(got.first) + "/" + std::to_string(got.second) + " matched, " +
                      std::to_string(least.first) + "/" + std::to_string(least.second) + " needed").c_str());
        }
        QVERIFY(refused_here >= 16);
    }
};

QTEST_GUILESS_MAIN(TestContractLines)
#include "test_contract_lines.moc"
