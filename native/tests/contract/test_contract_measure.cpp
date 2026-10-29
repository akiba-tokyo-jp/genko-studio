// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」) as an agent or a person uses it: through the command line — the
// measurement scale, the count groups and marks, the ruler line set with `genko apply` and undone with `genko undo`, the book
// naming measure@1 only while a page has measuring (none: its features and project.json as before), `genko inspect
// --measure` and the question measure_info (nothing saved), レイヤーの角度補正 through `genko apply` the same as in the
// window's op (the 8-bit and 16-bit pages); the PSD's resource 1074 as psd-tools reads it (written from the page's scale,
// in the file's pixels; read back; written again as read while unchanged, anew when changed, not at all when gone), 1080
// (the count) kept as it was until the page has its own marks; a .pcraft's measurement written and read back (with
// $GENKO_PCRAFT_CHECK, upstream's reader reads it).

#include <QtTest>

#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/measure.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "render/raster.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::Document run(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor(actor)).doc;
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// A 96 × 72 mm page at 200 dpi, without panels; with a paint layer "a" (a bar across its middle) when asked.
core::Document book(bool bar = false) {
    auto doc = core::new_episode("計測", core::Num(1), 1, core::PageSpec::custom(96, 72, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    if (bar) {
        p.layers.clear();
        const int w = 756, h = 567;  // (96 × 72 mm at the working 200 dpi)
        std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
        for (int y = h / 2 - 6; y < h / 2 + 6; ++y)
            for (int x = 100; x < w - 100; ++x) {
                const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
                bytes[i + 2] = static_cast<char>(180);
                bytes[i + 3] = static_cast<char>(255);
            }
        core::Layer layer;
        layer.id = "a";
        layer.kind = core::LayerKind::Raster;
        layer.panel_clip = false;
        render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
        p.layers.push_back(std::move(layer));
    }
    return doc;
}

}  // namespace

class TestContractMeasure : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int views_ = 0;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops, const char* agent = "human:test") {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QString::fromLatin1(agent)}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool names_measure(const Json& project) {
        const Json features = project.value("features", Json::array());
        return std::find(features.begin(), features.end(), Json("measure@1")) != features.end();
    }
    Json measure_tool(const QStringList& args) {
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          QStringList{genko::test::repo_root() + QStringLiteral("/tools/migration/psd_measure_check.py")} + args,
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return Json::object();
    }
    Json measure_view(const QString& psd) {
        const QString out = path(QStringLiteral("measure-%1.json").arg(++views_));
        const Json ran = measure_tool({QStringLiteral("view"), out, psd});
        if (ran.contains("error")) return ran;
        return genko::test::read_json(out);
    }
    static std::string resource_of(const QString& psd, unsigned id) {
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(psd));
        for (const auto& r : file.resources())
            if (r.id == id) return std::string(r.data);
        return {};
    }
    QString export_psd(const core::Document& doc, const QString& name) {
        const QString file = path(name);
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), 200);
        return file;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Through the command line: the scale, the count and the ruler line saved with measure@1, listed, asked, undone; all
    // taken away: the book's project.json as before.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(), fs::path(book_path.toStdString()));
        const std::string before = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        QVERIFY(!names_measure(project(book_path)));
        Json done = apply(book_path, QStringLiteral("a.json"),
                          Json::array({op("set_measurement_scale", {{"preset", "paper_mm"}}), op("add_count", {{"at_mm", {10, 10}}}),
                                       op("add_count", {{"at_mm", {20, 15}}}), op("new_count_group", {{"name", "B"}}),
                                       op("add_count", {{"at_mm", {30, 30}}}), op("set_ruler_line", {{"start", {10, 40}}, {"end", {40, 0}}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book_path);
        QVERIFY2(names_measure(saved), saved.value("features", Json::array()).dump().c_str());
        const Json& kept = saved["pages"][0]["measure"];
        QCOMPARE(kept["scale"]["units"], Json("mm"));
        QCOMPARE(kept["count_groups"].size(), std::size_t(2));
        QCOMPARE(kept["count_groups"][0]["points"], Json::parse("[[10, 10], [20, 15]]"));
        QCOMPARE(kept["active_group"], Json(1));
        int exit = -1;
        Json listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--measure")}, &exit);
        QVERIFY2(exit == 0, listed.dump().c_str());
        const Json& page = listed["pages"][0];
        QCOMPARE(page["page"], Json(1));
        QCOMPARE(page["dpi"], Json(200.0));
        QCOMPARE(page["count"]["total"], Json(3));
        QCOMPARE(page["scale"]["units"], Json("mm"));
        QCOMPARE(page["ruler"]["info"]["l1_mm"], Json(50.0));
        QCOMPARE(page["ruler"]["info"]["length"], Json(50.0));  // (the paper's mm)
        QVERIFY(std::fabs(page["ruler"]["info"]["angle"].get<double>() - 53.1301) < 1e-3);
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--measure"), QStringLiteral("--page"), QStringLiteral("7")}, &exit);
        QVERIFY(exit != 0);
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--measure"), QStringLiteral("--annotations")}, &exit);
        QVERIFY(exit != 0);
        // the question: answered, the book as it was
        const std::string with = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        done = apply(book_path, QStringLiteral("q.json"), Json::array({op("measure_info")}));
        QVERIFY2(done.dump().find("\"total\":3") != std::string::npos, done.dump().c_str());
        QCOMPARE(genko::test::read_bytes(book_path + QStringLiteral("/project.json")), with);
        // an agent's apply: the same ops (the C++ build's own: the window, `genko apply`, HTTP)
        done = apply(book_path, QStringLiteral("ai.json"), Json::array({op("add_count", {{"at_mm", {5, 5}}})}), "ai:claude");
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // all taken away: no measure@1, project.json's pages as before
        done = apply(book_path, QStringLiteral("e.json"),
                     Json::array({op("set_measurement_scale", {{"preset", "default"}}), op("delete_count_group"), op("delete_count_group"),
                                  op("set_ruler_line", {{"clear", true}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book_path);
        QVERIFY(!names_measure(saved));
        QVERIFY(saved.dump().find("\"measure\"") == std::string::npos);
        QVERIFY2(saved["pages"] == Json::parse(before)["pages"], (saved["pages"].dump() + "\n" + Json::parse(before)["pages"].dump()).c_str());
        const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QVERIFY(names_measure(project(book_path)));
        listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--measure"), QStringLiteral("--page"), QStringLiteral("1")}, &exit);
        QCOMPARE(listed["pages"][0]["count"]["total"], Json(4));
    }

    // レイヤーの角度補正 through `genko apply` the same as the op in the window (the 8-bit and 16-bit pages): the layer's
    // pixels, and the page turned and cut.
    void straightenThroughTheCommandLine() {
        for (const bool deep : {false, true}) {
            core::Document doc = book(true);
            if (deep) doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
            doc = run(doc, op("set_ruler_line", {{"start", {10, 40}}, {"end", {80, 34}}}));
            for (const bool crop : {false, true}) {
                const QString book_path = path(QStringLiteral("turn-%1-%2.genko").arg(deep).arg(crop));
                genko::test::write_project(doc, fs::path(book_path.toStdString()));
                const Json asked = crop ? op("straighten_layer", {{"crop", true}}) : op("straighten_layer", {{"ids", {"a"}}});
                const Json done = apply(book_path, QStringLiteral("turn.json"), Json::array({asked}));
                QVERIFY2(done.value("ok", false), done.dump().c_str());
                const auto loaded = storage::load_document(fs::path(book_path.toStdString()));
                const core::Document here = run(doc, asked);
                const core::Page& a = loaded.document.page(0);
                const core::Page& b = here.page(0);
                QCOMPARE(a.spec.width_mm.value(), b.spec.width_mm.value());
                QCOMPARE(a.spec.height_mm.value(), b.spec.height_mm.value());
                QCOMPARE(a.layers.size(), b.layers.size());
                QVERIFY(!a.measure && !b.measure);  // (the line cleared)
                if (deep) {
                    QVERIFY(a.layers[0].color_raster && *a.layers[0].color_raster == *b.layers[0].color_raster);
                } else {
                    QVERIFY(a.layers[0].raster_png && *a.layers[0].raster_png == *b.layers[0].raster_png);
                }
                if (crop) QVERIFY(a.spec.width_mm.value() < 96);
            }
        }
    }

    // PSD: 1074 as psd-tools reads it (the page's scale in the file's pixels); read back; written again as read while
    // unchanged, anew when changed, not when gone; 1080 kept until the page has its own marks.
    void psdScaleAndCount() {
        // (the file at 200 dpi too: its 756 pixels across the 96 mm a hair more than the page's 755.9 — the length in the file's
        // pixels as near)
        const core::Document doc = run(book(), op("set_measurement_scale", {{"pixel_length", 50}, {"logical_length", 2}, {"units", "cm"}}));
        const QString first = export_psd(doc, QStringLiteral("scale.psd"));
        Json view = measure_view(first);
        QVERIFY2(!view.contains("error") && view["scale"].is_object(), view.dump().c_str());
        QVERIFY2(std::fabs(view["scale"]["pixelLength"].get<double>() - 50.0) < 0.01, view.dump().c_str());
        QCOMPARE(view["scale"]["logicalLength"], Json(2.0));
        QCOMPARE(view["scale"]["logicalUnits"], Json("cm"));
        QCOMPARE(view["scale_version"], Json(16));
        QCOMPARE(view["count"], Json(false));
        // (a page without one: no 1074, as before)
        QVERIFY(resource_of(export_psd(book(), QStringLiteral("plain.psd")), 1074).empty());
        // a file psd-tools made: its scale read, written again as read; its count kept
        const QString made = path(QStringLiteral("made.psd"));
        QVERIFY(!measure_tool({QStringLiteral("make"), made}).contains("error"));
        formats::Opened opened = formats::open_file(genko::test::read_bytes(made), "made.psd");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.measure && page.measure->scale);
        // (its 40 × 30 pixels on the page at the file's resolution: the page's pixels the file's)
        QVERIFY2(std::fabs(page.measure->scale->pixel_length - 150.0) < 1e-6, std::to_string(page.measure->scale->pixel_length).c_str());
        QCOMPARE(page.measure->scale->units, std::string("mm"));
        QVERIFY(page.native_psd && std::any_of(page.native_psd->copied.begin(), page.native_psd->copied.end(),
                                                [](const core::PsdCopied& c) { return c.resource == 1074; }));
        const int dpi = static_cast<int>(std::lround(page.spec.dpi.value()));
        const auto again_path = path(QStringLiteral("made-again.psd"));
        (void)formats::export_page_psd(opened.doc, page, fs::path(again_path.toStdString()), dpi);
        QCOMPARE(resource_of(again_path, 1074), resource_of(made, 1074));
        QCOMPARE(resource_of(again_path, 1080), resource_of(made, 1080));
        // changed: anew
        const core::Document changed = run(opened.doc, op("set_measurement_scale", {{"units", "cm"}}));
        const auto changed_path = path(QStringLiteral("made-changed.psd"));
        (void)formats::export_page_psd(changed, changed.page(0), fs::path(changed_path.toStdString()), dpi);
        view = measure_view(changed_path);
        QCOMPARE(view["scale"]["logicalUnits"], Json("cm"));
        QVERIFY(std::fabs(view["scale"]["pixelLength"].get<double>() - 150.0) < 1e-6);
        QCOMPARE(view["count"], Json(true));  // (no marks of its own: the count kept)
        // gone: no 1074; the page's own marks: no 1080
        const core::Document gone = run(run(opened.doc, op("set_measurement_scale", {{"preset", "default"}})), op("add_count", {{"at_mm", {1, 1}}}));
        const auto gone_path = path(QStringLiteral("made-gone.psd"));
        (void)formats::export_page_psd(gone, gone.page(0), fs::path(gone_path.toStdString()), dpi);
        view = measure_view(gone_path);
        QVERIFY2(view["scale"].is_null(), view.dump().c_str());
        QCOMPARE(view["count"], Json(false));
        QVERIFY(resource_of(gone_path, 1074).empty() && resource_of(gone_path, 1080).empty());
    }

    // .pcraft: written and read back; upstream's reader reads it.
    void pcraftMeasurement() {
        core::Document doc = book();
        doc = run(doc, op("set_measurement_scale", {{"pixel_length", 100}, {"logical_length", 2.5}, {"units", "cm"}}));
        doc = run(doc, op("add_count", {{"at_mm", {12.7, 25.4}}}));
        doc = run(doc, op("new_count_group", {{"name", "細胞"}, {"color", "#00ff00"}}));
        doc = run(doc, op("add_count", {{"at_mm", {25.4, 12.7}}}));
        doc = run(doc, op("set_ruler_line", {{"start", {0, 0}}, {"end", {25.4, 0}}, {"protractor", {0, 25.4}}}));
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        const QString file = path(QStringLiteral("measure.pcraft"));
        genko::test::write_bytes(file, bytes);
        formats::Opened opened = formats::open_file(bytes, "measure.pcraft");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.measure);
        QCOMPARE(page.measure->count_groups.size(), std::size_t(2));
        QCOMPARE(page.measure->count_groups[1].name, std::string("細胞"));
        QCOMPARE(page.measure->count_groups[1].color, (std::array<int, 3>{0, 255, 0}));
        QVERIFY(std::fabs(page.measure->count_groups[0].points[0][0] - 12.7) < 0.01);
        QVERIFY(std::fabs(page.measure->ruler->end[0] - 25.4) < 0.01);
        QVERIFY(page.measure->ruler->protractor);
        QCOMPARE(page.measure->scale->units, std::string("cm"));
        // (the same pixels a mm on both pages: the scale as it was)
        QVERIFY2(std::fabs(page.measure->scale->pixel_length * 25.4 / page.spec.dpi.value() - 100 * 25.4 / 200) < 1e-3,
                 std::to_string(page.measure->scale->pixel_length).c_str());
        for (const std::string& n : opened.notes) QVERIFY2(n.find("計測") == std::string::npos, n.c_str());
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) return;
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check"), file});
        QVERIFY(process.waitForFinished(600000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardOutput().constData());
    }
};

QTEST_GUILESS_MAIN(TestContractMeasure)
#include "test_contract_measure.moc"
