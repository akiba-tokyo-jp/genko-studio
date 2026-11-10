// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」) as an agent or a person uses it: through the command line — user slices,
// an auto slice made a user one, a slice divided, the slices from the guides, a layer-based slice following its layer and
// going with it, set with `genko apply` and undone with `genko undo`, the book naming slices@1 only while a page has slices
// (a book without them: project.json's bytes as before), `genko inspect --slices` and the question slices_info (nothing
// saved); the PSD's resource 1050 as psd-tools reads it (written from the page's slices in the file's pixels with the auto
// ones and a layer-based slice naming its layer's lyid; read back; written again as read while unchanged, anew when
// changed, not at all when gone; a file psd-tools made read); Web 用に保存 by slices (`genko export --format slices`: a
// picture a slice in images/, the no-image slice's text in its cell, the HTML table with its links, alts and spacer, the
// pictures and the cell tiling the page — PhotoCraft's web_cmds/tests.rs slices_export_with_html_table —, the user slices
// only, scaled, never into the book); a .pcraft's slices written and read back (with $GENKO_PCRAFT_CHECK, upstream's reader
// reads it).

#include <QtTest>

#include <QDir>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/slice_layout.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_slices.hpp"
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

// A 60.96 × 45.72 mm page at 200 dpi (480 × 360 pixels) without panels; with a paint layer "a" (a block at 120..200 ×
// 240..320 of the working 200 dpi) when asked.
core::Document book(bool block = false) {
    auto doc = core::new_episode("スライス", core::Num(1), 1, core::PageSpec::custom(60.96, 45.72, 5, 5, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    if (block) {
        p.layers.clear();
        const int w = 480, h = 360;
        std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
        for (int y = 240; y < 320; ++y)
            for (int x = 120; x < 200; ++x) {
                const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
                bytes[i] = static_cast<char>(200);
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

// A PNG's width and height (its IHDR).
std::pair<int, int> png_size(const std::string& bytes) {
    const auto be = [&](std::size_t at) {
        return (static_cast<unsigned char>(bytes[at]) << 24) | (static_cast<unsigned char>(bytes[at + 1]) << 16) |
               (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
    };
    if (bytes.size() < 24) return {0, 0};
    return {static_cast<int>(be(16)), static_cast<int>(be(20))};
}

}  // namespace

class TestContractSlices : public QObject {
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
    static bool names_slices(const Json& project) {
        const Json features = project.value("features", Json::array());
        return std::find(features.begin(), features.end(), Json("slices@1")) != features.end();
    }
    Json slices_tool(const QStringList& args) {
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          QStringList{genko::test::repo_root() + QStringLiteral("/tools/migration/psd_slices_check.py")} + args,
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return Json::object();
    }
    Json slices_view(const QString& psd) {
        const QString out = path(QStringLiteral("slices-%1.json").arg(++views_));
        const Json ran = slices_tool({QStringLiteral("view"), out, psd});
        if (ran.contains("error")) return ran;
        return genko::test::read_json(out);
    }
    static std::string resource_of(const QString& psd, unsigned id) {
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(psd));
        for (const auto& r : file.resources())
            if (r.id == id) return std::string(r.data);
        return {};
    }
    QString export_psd(const core::Document& doc, const QString& name, int dpi = 200) {
        const QString file = path(name);
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), dpi);
        return file;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Through the command line: slices saved with slices@1, listed, asked, undone; a book without them as before.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(true), fs::path(book_path.toStdString()));
        const std::string before = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        QVERIFY(!names_slices(project(book_path)));
        // (a question alone: the book as it was, byte for byte)
        Json done = apply(book_path, QStringLiteral("q0.json"), Json::array({op("slices_info")}));
        QVERIFY2(done.dump().find("\"slices\"") != std::string::npos, done.dump().c_str());
        QCOMPARE(genko::test::read_bytes(book_path + QStringLiteral("/project.json")), before);
        done = apply(book_path, QStringLiteral("a.json"),
                     Json::array({op("add_slice", {{"rect_px", {40, 20, 160, 80}}, {"name", "hero"}, {"url", "https://example.org/"}, {"alt", "Hero"}}),
                                  op("add_slice", {{"layer", "a"}}), op("set_slice", {{"number", 1}, {"name", "top"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book_path);
        QVERIFY2(names_slices(saved), saved.value("features", Json::array()).dump().c_str());
        const Json& kept = saved["pages"][0]["slices"];
        QCOMPARE(kept["list"].size(), std::size_t(3));
        QCOMPARE(kept["list"][0]["name"], Json("hero"));
        QCOMPARE(kept["list"][0]["rect_mm"], Json::array({5.08, 2.54, 20.32, 10.16}));
        QCOMPARE(kept["list"][1]["origin"], Json("layer"));
        QCOMPARE(kept["list"][1]["layer"], Json("a"));
        QCOMPARE(kept["list"][2]["name"], Json("top"));  // (the auto slice above made a user one)
        int exit = -1;
        Json listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--slices")}, &exit);
        QVERIFY2(exit == 0, listed.dump().c_str());
        const Json& page = listed["pages"][0];
        QCOMPARE(page["page"], Json(1));
        QCOMPARE(page["dpi"], Json(200.0));
        QCOMPARE(page["width_px"], Json(480));
        const Json& slices = page["slices"];
        QVERIFY(std::any_of(slices.begin(), slices.end(), [](const Json& s) { return s["name"] == Json("hero") && s["rect_px"] == Json::array({40, 20, 160, 80}); }));
        QVERIFY(std::any_of(slices.begin(), slices.end(), [](const Json& s) { return s["origin"] == Json("layer") && s["rect_px"] == Json::array({120, 240, 80, 80}); }));
        QVERIFY(std::any_of(slices.begin(), slices.end(), [](const Json& s) { return s["origin"] == Json("auto"); }));
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--slices"), QStringLiteral("--page"), QStringLiteral("7")}, &exit);
        QVERIFY(exit != 0);
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--slices"), QStringLiteral("--measure")}, &exit);
        QVERIFY(exit != 0);
        // the question: answered, the book as it was
        const std::string with = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        done = apply(book_path, QStringLiteral("q.json"), Json::array({op("slices_info")}));
        QVERIFY2(done.dump().find("hero") != std::string::npos, done.dump().c_str());
        QCOMPARE(genko::test::read_bytes(book_path + QStringLiteral("/project.json")), with);
        // the layer moved: its slice follows; the layer deleted: its slice too
        done = apply(book_path, QStringLiteral("m.json"),
                     Json::array({op("transform_layers", {{"ids", {"a"}}, {"matrix", {1, 0, 0, 1, 2.54, 0}}, {"box", {0, 0, 60.96, 45.72}}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--slices")}, &exit);
        QVERIFY(std::any_of(listed["pages"][0]["slices"].begin(), listed["pages"][0]["slices"].end(),
                            [](const Json& s) { return s["origin"] == Json("layer") && s["rect_px"] == Json::array({140, 240, 80, 80}); }));
        done = apply(book_path, QStringLiteral("d.json"), Json::array({Json{{"op", "delete_layer"}, {"page", 1}, {"id", "a"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(project(book_path)["pages"][0]["slices"]["list"].size(), std::size_t(2));
        // undone: the layer and its slice back
        Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(project(book_path)["pages"][0]["slices"]["list"].size(), std::size_t(3));
        // an agent's apply: the same ops (the C++ build's own: the window, `genko apply`, HTTP)
        done = apply(book_path, QStringLiteral("ai.json"), Json::array({op("add_slice", {{"rect_mm", {0, 40, 10, 5}}})}), "ai:claude");
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // the guides' cells in place of them all; スライスを消去: the numbering kept (slices@1 still named)
        done = apply(book_path, QStringLiteral("c.json"),
                     Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "guide"}, {"axis", "v"}, {"at", 30.48}}, op("slices_from_guides"),
                                  op("divide_slice", {{"number", 1}, {"rows", 2}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(project(book_path)["pages"][0]["slices"]["list"].size(), std::size_t(3));
        done = apply(book_path, QStringLiteral("e.json"), Json::array({op("delete_slices", {{"slices", "all"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book_path);
        QVERIFY(names_slices(saved));
        QVERIFY(saved["pages"][0]["slices"]["list"].empty());
        QVERIFY(saved["pages"][0]["slices"]["next_id"].get<int>() > 1);
    }

    // PSD: 1050 as psd-tools reads it; read back; written again as read while unchanged, anew when changed, not when gone;
    // a file psd-tools made read.
    void psdSlices() {
        core::Document doc = book(true);
        doc = run(doc, op("add_slice", {{"rect_px", {40, 20, 160, 80}}, {"name", "hero"}, {"url", "https://example.org/"}, {"target", "_blank"},
                                         {"alt", "ヒーロー"}, {"background", "#123456"}}));
        doc = run(doc, op("add_slice", {{"layer", "a"}, {"outsets", {1, 2, 3, 4}}}));
        const QString first = export_psd(doc, QStringLiteral("slices.psd"));
        Json view = slices_view(first);
        QVERIFY2(!view.contains("error") && view["slices"].is_array(), view.dump().c_str());
        QCOMPARE(view["version"], Json(6));
        QCOMPARE(view["bounds"], Json::array({0, 0, 360, 480}));
        QCOMPARE(view["group"], Json("p001"));
        const Json& items = view["slices"];
        QCOMPARE(items[0]["name"], Json("hero"));
        QCOMPARE(items[0]["origin"], Json(2));
        QCOMPARE(items[0]["rect"], Json::array({40, 20, 200, 100}));
        QCOMPARE(items[0]["url"], Json("https://example.org/"));
        QCOMPARE(items[0]["target"], Json("_blank"));
        QCOMPARE(items[0]["alt"], Json("ヒーロー"));
        QCOMPARE(items[0]["color"], Json::array({255, 0x12, 0x34, 0x56}));
        QCOMPARE(items[1]["origin"], Json(1));
        QVERIFY(items[1]["layer_id"].is_number());
        QCOMPARE(items[1]["rect"], Json::array({118, 239, 204, 323}));  // (the layer's box and its outsets: left 2, top 1, right 4, bottom 3)
        QVERIFY(std::any_of(items.begin(), items.end(), [](const Json& s) { return s["origin"] == Json(0); }));
        QCOMPARE(view["outsets"][items[1]["id"].dump()], Json::array({1, 2, 3, 4}));
        // (a page without slices: no 1050, as before)
        QVERIFY(resource_of(export_psd(book(true), QStringLiteral("plain.psd")), 1050).empty());
        // read back: the slices as they were, the layer-based one naming the layer read
        formats::Opened opened = formats::open_file(genko::test::read_bytes(first), "slices.psd");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.slices);
        QCOMPARE(page.slices->list.size(), std::size_t(2));
        QCOMPARE(page.slices->list[0].name, std::string("hero"));
        QCOMPARE(page.slices->list[0].alt, std::string("ヒーロー"));
        QCOMPARE(page.slices->list[1].origin, std::string("layer"));
        QVERIFY(page.slices->list[1].layer && std::any_of(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == *page.slices->list[1].layer; }));
        QCOMPARE(page.slices->list[1].outsets, (std::array<int, 4>{1, 2, 3, 4}));
        QVERIFY(page.native_psd && std::any_of(page.native_psd->copied.begin(), page.native_psd->copied.end(),
                                                [](const core::PsdCopied& c) { return c.resource == 1050; }));
        const QString again = export_psd(opened.doc, QStringLiteral("slices-again.psd"));
        QCOMPARE(resource_of(again, 1050), resource_of(first, 1050));
        // changed: anew
        const core::Document changed = run(opened.doc, op("set_slice", {{"slice", page.slices->list[0].id}, {"name", "logo"}}));
        view = slices_view(export_psd(changed, QStringLiteral("slices-changed.psd")));
        QCOMPARE(view["slices"][0]["name"], Json("logo"));
        QCOMPARE(view["slices"][1]["origin"], Json(1));
        // gone: none
        const core::Document gone = run(opened.doc, op("delete_slices", {{"slices", "all"}}));
        QVERIFY(resource_of(export_psd(gone, QStringLiteral("slices-gone.psd")), 1050).empty());
        // a file psd-tools made: its user slice and options read, the auto one made again
        const QString made = path(QStringLiteral("made.psd"));
        QVERIFY(!slices_tool({QStringLiteral("make"), made}).contains("error"));
        formats::Opened theirs = formats::open_file(genko::test::read_bytes(made), "made.psd");
        const core::Page& their = theirs.doc.page(0);
        QVERIFY(their.slices);
        QCOMPARE(their.slices->list.size(), std::size_t(1));
        QCOMPARE(their.slices->list[0].name, std::string("hero"));
        QCOMPARE(their.slices->list[0].target, std::string("_blank"));
        QCOMPARE(*their.slices->list[0].background, std::string("#123456"));
        QCOMPARE(their.slices->group, std::string("site"));
        const auto listed = render::slices::listing(theirs.doc, their);
        QVERIFY(std::any_of(listed["slices"].begin(), listed["slices"].end(), [](const Json& s) { return s["name"] == Json("hero") && s["rect_px"] == Json::array({20, 10, 40, 20}); }));
    }

    // Web 用に保存 by slices (PhotoCraft's slices_export_with_html_table): a picture a slice but the no-image one, whose
    // text is its cell; the pictures and the cell tile the page; the HTML's table, links, alts and spacer; the user slices
    // only, scaled; never into the book.
    void saveForWebBySlices() {
        const QString book_path = path(QStringLiteral("web.genko"));
        core::Document doc = book(true);
        doc = run(doc, op("add_slice", {{"rect_px", {32, 32, 96, 96}}, {"name", "logo"}, {"url", "https://example.org/"}, {"alt", "Logo"}}));
        doc = run(doc, op("add_slice", {{"rect_px", {160, 0, 96, 80}}, {"kind", "noImage"}, {"cell_text", "Hello & bye"}, {"background", "#ff0000"}}));
        genko::test::write_project(doc, fs::path(book_path.toStdString()));
        const QString out = path(QStringLiteral("web-out"));
        int exit = -1;
        const Json done = genko({QStringLiteral("export"), book_path, out, QStringLiteral("--format"), QStringLiteral("slices"), QStringLiteral("--html"),
                                 QStringLiteral("--web"), QStringLiteral("{\"format\": \"png24\"}"), QStringLiteral("--json")},
                                &exit);
        QVERIFY2(exit == 0, done.dump().c_str());
        const Json info = render::slices::listing(doc, doc.page(0));
        const std::size_t all = info["slices"].size();
        QCOMPARE(done["slices"], Json(all));
        // (a picture each but the no-image one, and the spacer)
        QCOMPARE(done["files"].size(), all);
        std::int64_t covered = 0;
        for (const Json& f : done["files"]) {
            const std::string file = f.get<std::string>();
            QVERIFY2(fs::exists(fs::path(file)), file.c_str());
            if (file.find("spacer.gif") != std::string::npos) continue;
            const auto [w, h] = png_size(genko::test::read_bytes(QString::fromStdString(file)));
            covered += std::int64_t(w) * h;
        }
        QCOMPARE(covered + 96 * 80, std::int64_t(480) * 360);
        QVERIFY(fs::exists(fs::path(out.toStdString()) / "images" / "logo.png"));
        const std::string html = genko::test::read_bytes(QString::fromStdString(done["html"].get<std::string>()));
        QVERIFY(html.find("<a href=\"https://example.org/\">") != std::string::npos);
        QVERIFY(html.find("src=\"images/logo.png\" width=\"96\" height=\"96\" alt=\"Logo\"") != std::string::npos);
        QVERIFY(html.find("Hello &amp; bye") != std::string::npos);
        QVERIFY(html.find("bgcolor=\"#FF0000\"") != std::string::npos);
        QVERIFY(html.find("images/spacer.gif") != std::string::npos);
        QVERIFY(html.find("<table id=\"Table_01\" width=\"480\" height=\"360\"") != std::string::npos);
        QVERIFY(QString::fromStdString(done["html"].get<std::string>()).endsWith(QStringLiteral("p001.html")));
        // the user slices only, at 50 %
        const QString half = path(QStringLiteral("web-half"));
        const Json small = genko({QStringLiteral("export"), book_path, half, QStringLiteral("--format"), QStringLiteral("slices"), QStringLiteral("--slices"),
                                  QStringLiteral("user"), QStringLiteral("--web"), QStringLiteral("{\"format\": \"png24\", \"percent\": 50}"), QStringLiteral("--json")},
                                 &exit);
        QVERIFY2(exit == 0, small.dump().c_str());
        QCOMPARE(small["slices"], Json(2));
        QCOMPARE(small["files"].size(), std::size_t(1));  // (the no-image one writes none)
        QCOMPARE(png_size(genko::test::read_bytes(QString::fromStdString(small["files"][0].get<std::string>()))), (std::pair<int, int>(48, 48)));
        QVERIFY(small["html"].is_null());
        // never into the book (F-c's write guard), nor an option without the format
        genko({QStringLiteral("export"), book_path, book_path + QStringLiteral("/inside"), QStringLiteral("--format"), QStringLiteral("slices")}, &exit);
        QCOMPARE(exit, 2);
        QVERIFY(!fs::exists(fs::path((book_path + QStringLiteral("/inside")).toStdString())));
        genko({QStringLiteral("export"), book_path, path(QStringLiteral("x")), QStringLiteral("--html")}, &exit);
        QCOMPARE(exit, 2);
        // a page without slices: one picture, the page (its auto slice, named after the page)
        const QString plain_path = path(QStringLiteral("plain.genko"));
        genko::test::write_project(book(), fs::path(plain_path.toStdString()));
        const Json one = genko({QStringLiteral("export"), plain_path, path(QStringLiteral("web-plain")), QStringLiteral("--format"), QStringLiteral("slices"),
                                QStringLiteral("--json")},
                               &exit);
        QVERIFY2(exit == 0, one.dump().c_str());
        QCOMPARE(one["files"].size(), std::size_t(1));
        QVERIFY(one["files"][0].get<std::string>().find("images/p001.png") != std::string::npos);
    }

    // (PC5 J-b review P2-3, P3-2, P3-4, P3-13) Web 用に保存 by slices over one another and of the same names: the later in the
    // list on top, the one under it cut into the parts that show (said); the HTML's table with no hole and no cell twice
    // (each row's cells, their colspans and the rowspans from above, fill its columns); the pictures and the cells tile the
    // page; the files' names one each whatever their case, a device of Windows not a name; a link only of http, https,
    // mailto or a relative one (the others left out, said); the alt's and the target's quotes escaped.
    void overlapsNamesAndLinks() {
        const QString book_path = path(QStringLiteral("overlaps.genko"));
        core::Document doc = book(true);
        const auto add = [&](Json rect, const char* name, const char* url, Json more = Json::object()) {
            Json o = op("add_slice", {{"rect_px", rect}, {"name", name}, {"url", url}});
            for (auto& [k, v] : more.items()) o[k] = v;
            doc = run(doc, o);
        };
        add({0, 0, 200, 200}, "a", "javascript:alert(1)");
        add({100, 100, 200, 200}, "a", "mailto:x@example.org", {{"alt", "say \"hi\" <b>"}, {"target", "_\"top"}});
        add({300, 0, 40, 40}, "a-2", " JavaScript:alert(2)");
        add({350, 0, 40, 40}, "CON", "data:text/html,x");
        add({400, 0, 40, 40}, "Logo", "../x.html");
        add({300, 100, 40, 40}, "logo", "https://example.org/?q=a:b");
        genko::test::write_project(doc, fs::path(book_path.toStdString()));
        const QString out = path(QStringLiteral("overlaps-out"));
        int exit = -1;
        const Json done = genko({QStringLiteral("export"), book_path, out, QStringLiteral("--format"), QStringLiteral("slices"), QStringLiteral("--html"),
                                 QStringLiteral("--web"), QStringLiteral("{\"format\": \"png24\"}"), QStringLiteral("--json")},
                                &exit);
        QVERIFY2(exit == 0, done.dump().c_str());
        // (one name each, whatever its case; CON not a name)
        std::set<std::string> names;
        std::int64_t covered = 0;
        for (const Json& f : done["files"]) {
            const fs::path file(f.get<std::string>());
            std::string low = file.filename().string();
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            QVERIFY2(names.insert(low).second, low.c_str());
            QVERIFY2(fs::exists(file), file.string().c_str());
            if (low == "spacer.gif") continue;
            const auto [w, h] = png_size(genko::test::read_bytes(QString::fromStdString(file.string())));
            covered += std::int64_t(w) * h;
        }
        QCOMPARE(covered, std::int64_t(480) * 360);
        QVERIFY(names.contains("con_.png") && !names.contains("con.png"));
        QVERIFY(names.contains("logo.png") && names.contains("logo-2.png"));
        QVERIFY(names.contains("a-2.png"));
        QVERIFY(names.contains("a_1.png"));  // (the slice under the other, in parts)
        const std::string notes = done.value("notes", Json::array()).dump();
        QVERIFY2(notes.find("つに分けて書き出しました") != std::string::npos, notes.c_str());
        QVERIFY2(notes.find("HTML にリンクを書きませんでした") != std::string::npos, notes.c_str());
        // the links: only the safe ones
        const std::string html = genko::test::read_bytes(QString::fromStdString(done["html"].get<std::string>()));
        std::string lower = html;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        QVERIFY(lower.find("javascript:") == std::string::npos);
        QVERIFY(lower.find("data:") == std::string::npos);
        QVERIFY(html.find("href=\"mailto:x@example.org\" target=\"_&quot;top\"") != std::string::npos);
        QVERIFY(html.find("alt=\"say &quot;hi&quot; &lt;b&gt;\"") != std::string::npos);
        QVERIFY(html.find("href=\"../x.html\"") != std::string::npos);
        QVERIFY(html.find("href=\"https://example.org/?q=a:b\"") != std::string::npos);
        // the table: every row filled, no cell twice (HTML's own placing of the colspans and rowspans)
        std::vector<std::vector<int>> grid;
        std::size_t columns = 0;
        std::size_t row = 0;
        for (std::size_t tr = html.find("<tr>"); tr != std::string::npos; tr = html.find("<tr>", tr + 4), ++row) {
            const std::size_t end = html.find("</tr>", tr);
            if (grid.size() <= row) grid.resize(row + 1);
            std::size_t col = 0;
            for (std::size_t td = html.find("<td", tr); td != std::string::npos && td < end; td = html.find("<td", td + 3)) {
                const std::size_t close = html.find('>', td);
                const std::string tag = html.substr(td, close - td);
                const auto span = [&](const char* key) {
                    const std::size_t at = tag.find(key);
                    return at == std::string::npos ? 1 : std::stoi(tag.substr(at + std::strlen(key)));
                };
                const int cs = span("colspan=\""), rs = span("rowspan=\"");
                while (col < grid[row].size() && grid[row][col] != 0) ++col;
                for (int j = 0; j < rs; ++j) {
                    if (grid.size() <= row + j) grid.resize(row + j + 1);
                    auto& line = grid[row + j];
                    if (line.size() < col + cs) line.resize(col + cs, 0);
                    for (int i = 0; i < cs; ++i) ++line[col + i];
                }
                col += cs;
            }
            columns = std::max(columns, grid[row].size());
        }
        QVERIFY(row > 2 && columns > 2);
        for (std::size_t j = 0; j < row; ++j) {
            QCOMPARE(grid[j].size(), columns);
            for (const int n : grid[j]) QCOMPARE(n, 1);
        }
    }
    // .pcraft: written and read back; upstream's reader reads it.
    void pcraftSlices() {
        core::Document doc = book(true);
        doc = run(doc, op("add_slice", {{"rect_px", {40, 20, 160, 80}}, {"name", "表紙"}, {"kind", "noImage"}, {"cell_text", "<b>x</b>"}, {"cell_text_is_html", true}}));
        doc = run(doc, op("add_slice", {{"layer", "a"}}));
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        const QString file = path(QStringLiteral("slices.pcraft"));
        genko::test::write_bytes(file, bytes);
        formats::Opened opened = formats::open_file(bytes, "slices.pcraft");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(page.slices);
        QCOMPARE(page.slices->list.size(), std::size_t(2));
        QCOMPARE(page.slices->list[0].name, std::string("表紙"));
        QCOMPARE(page.slices->list[0].kind, std::string("noImage"));
        QVERIFY(page.slices->list[0].cell_text_is_html);
        QCOMPARE(page.slices->list[1].origin, std::string("layer"));
        const auto listed = render::slices::listing(opened.doc, page);
        QVERIFY(std::any_of(listed["slices"].begin(), listed["slices"].end(), [](const Json& s) { return s["name"] == Json("表紙") && s["rect_px"] == Json::array({40, 20, 160, 80}); }));
        for (const std::string& n : opened.notes) QVERIFY2(n.find("スライス") == std::string::npos, n.c_str());
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) return;
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check"), file});
        QVERIFY(process.waitForFinished(600000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardOutput().constData());
        QProcess dump;
        dump.start(tool, QStringList{QStringLiteral("dump"), file});
        QVERIFY(dump.waitForFinished(600000));
        const QByteArray said = dump.readAllStandardOutput();
        QVERIFY2(said.contains("表紙") || said.contains("slices"), said.left(2000).constData());
    }
};

QTEST_GUILESS_MAIN(TestContractSlices)
#include "test_contract_slices.moc"
