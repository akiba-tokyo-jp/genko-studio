// PC4 節 D (SPEC.md COLOR-08「フィルター」, schema-v4.md「フィルター（filters@1）」) through the command line, as a person or
// an agent uses it: フィルター's kinds (D0: ぼかし（ガウス）・ぼかし（ボックス）) given to filter_raster on an 8-bit page and a
// 16-bit one, inside a selection and on an alpha channel, saved and opened again, undone and done again, the journal's
// Undo stack; on a smart object as a smart filter — the book then names filters@1, and a reader that does not know it
// opens the book read-only —; a book without them written as it was (no feature added, the same bytes when written
// again); refusals (a kind not in the table, a setting it has not); genko inspect --filters. Python's 22 filters keep
// their own contracts (raster_cases.json, test_color_filters, test_ink_filters: run as they are).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace storage = genko::storage;

class TestContractFiltersPc4 : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    static std::filesystem::path path_of(const QString& book) { return std::filesystem::path(book.toStdString()); }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString write_ops(const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        return file;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    Json apply(const QString& book, const QString& name, const Json& ops, int* exit = nullptr) {
        return genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, exit);
    }
    static Json layer_in(const Json& saved, int page, const std::string& id) {
        for (const Json& layer : saved["pages"][page - 1]["layers"])
            if (layer.value("id", std::string()) == id) return layer;
        return Json();
    }
    static bool has(const Json& list, const char* item) { return std::find(list.begin(), list.end(), Json(item)) != list.end(); }
    // A book of two pages, a picture on each: page 1 at 8 bits, page 2 at 16 (precise).
    QString two_pages(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        // (a paint layer's picture is stretched over the page: 600 × 400 pixels over A4, a filter's working pixel some third of
        // one of them)
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{600, 400}, genko::render::Ink{40, 40, 40, 255});
        picture.paste(genko::render::Ink{220, 200, 30, 255}, genko::render::Box{200, 100, 400, 300});
        const std::string png = genko::core::b64encode(genko::render::write_png(picture));
        Json ops = Json::array();
        for (int p = 1; p <= 2; ++p) {
            const std::string id = "pic" + std::to_string(p);
            ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "paint"}, {"id", id}});
            ops.push_back(Json{{"op", "set_layer"}, {"page", p}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
            ops.push_back(Json{{"op", "put_raster"}, {"page", p}, {"id", id}, {"png_base64", png}});
        }
        ops.push_back(Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}});
        const Json done = apply(book, name + QStringLiteral(".ops.json"), ops);
        if (!done.value("ok", false)) qWarning("%s", done.dump().c_str());
        return book;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void filtersOfEveryWayThroughTheCommandLine() {
        const QString book = two_pages(QStringLiteral("blur.genko"));
        const Json before = project(book);
        const std::string precise_layer = [&] {
            for (const Json& layer : before["pages"][1]["layers"])
                if (layer.contains("color_raster")) return layer["id"].get<std::string>();
            return std::string();
        }();
        QVERIFY2(!precise_layer.empty(), before["pages"][1]["layers"].dump().c_str());
        // ぼかし（ガウス） on the 8-bit page, ぼかし（ボックス） inside a selection on the 16-bit one
        const Json done = apply(book, QStringLiteral("blur.json"),
                                Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "blur_gaussian"}, {"radius", 3}},
                                             Json{{"op", "filter_raster"}, {"page", 2}, {"id", precise_layer}, {"kind", "blur_box"}, {"radius", 4},
                                                  {"area", Json{{"rect", Json::array({0, 0, 100, 300})}}}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // (the reply: each the settings it was drawn with)
        QCOMPARE(done["results"][0]["filter"], (Json{{"kind", "blur_gaussian"}, {"radius", 3}}));
        QCOMPARE(done["results"][1]["filter"], (Json{{"kind", "blur_box"}, {"radius", 4}}));
        const Json after = project(book);
        QVERIFY(layer_in(after, 1, "pic1")["asset"] != layer_in(before, 1, "pic1")["asset"]);
        QVERIFY(layer_in(after, 2, precise_layer)["color_raster"] != layer_in(before, 2, precise_layer)["color_raster"]);
        // (a destructive filter leaves pixels: no feature of its own)
        QVERIFY2(!has(after["features"], "filters@1"), after["features"].dump().c_str());
        // opened again: writable, the same pages
        const storage::LoadResult loaded = storage::load_document(path_of(book));
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        // undone (both at once: one apply) and done again
        int exit = -1;
        (void)genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(layer_in(project(book), 1, "pic1")["asset"], layer_in(before, 1, "pic1")["asset"]);
        (void)genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        const Json differs = Json::diff(after["pages"], project(book)["pages"]);
        QVERIFY2(differs.empty(), differs.dump().substr(0, 1000).c_str());
        // the journal: the Undo stack replayed from it
        const storage::journal::Stacks stacks = storage::journal::stacks(path_of(book));
        QVERIFY(stacks.problems.empty());
        QVERIFY(stacks.undo.size() >= 2);
        // on an alpha channel (PC1c's target): the channel blurred, the layer as it was
        const Json chan = apply(book, QStringLiteral("chan.json"),
                                Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"name", "α"}, {"fill", "area"}, {"area", Json{{"rect", Json::array({20, 20, 40, 40})}}}}}));
        QVERIFY2(chan.value("ok", false), chan.dump().c_str());
        const Json with_channel = project(book);
        const Json blurred = apply(book, QStringLiteral("chanblur.json"),
                                   Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "blur_gaussian"}, {"radius", 5}, {"channel", "α"}}}));
        QVERIFY2(blurred.value("ok", false), blurred.dump().c_str());
        const Json channelled = project(book);
        QCOMPARE(layer_in(channelled, 1, "pic1")["asset"], layer_in(with_channel, 1, "pic1")["asset"]);
        QVERIFY2(channelled["pages"][0]["channels"] != with_channel["pages"][0]["channels"], channelled["pages"][0]["channels"].dump().c_str());
        // refused: a kind not in this build's table yet, a setting the kind has not
        const Json unknown = apply(book, QStringLiteral("unknown.json"), Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "blur_motion"}}}), &exit);
        QVERIFY2(exit != 0 && !unknown.value("ok", true), unknown.dump().c_str());
        const Json wrong = apply(book, QStringLiteral("wrong.json"),
                                 Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "blur_gaussian"}, {"sigma", 2}}}), &exit);
        QVERIFY2(exit != 0 && wrong.dump().find("sigma") != std::string::npos, wrong.dump().c_str());
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void aSmartFilterNamesFiltersAt1() {
        const QString book = two_pages(QStringLiteral("smart.genko"));
        const Json converted = apply(book, QStringLiteral("so.json"),
                                     Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic1"})}, {"new_id", "so"}},
                                                  Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur"}, {"radius", 2}}}));
        QVERIFY2(converted.value("ok", false), converted.dump().c_str());
        // (Python's blur as a smart filter: smart-objects@1 alone, as before)
        Json saved = project(book);
        QVERIFY(has(saved["features"], "smart-objects@1"));
        QVERIFY2(!has(saved["features"], "filters@1"), saved["features"].dump().c_str());
        const Json added = apply(book, QStringLiteral("sf.json"),
                                 Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur_gaussian"}, {"radius", 4}},
                                              Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur_box"}, {"radius", 3000}}}));
        QVERIFY2(added.value("ok", false), added.dump().c_str());
        saved = project(book);
        QVERIFY2(has(saved["features"], "filters@1"), saved["features"].dump().c_str());
        const Json filters = layer_in(saved, 1, "so")["smart"]["filters"];
        QCOMPARE(filters.size(), std::size_t(3));
        QCOMPARE(filters[1]["kind"], Json("blur_gaussian"));
        QCOMPARE(filters[1]["params"], (Json{{"radius", 4}}));
        QCOMPARE(filters[2]["params"], (Json{{"radius", 2000}}));  // (an op's: clamped, as PhotoCraft's)
        // drawn, opened again writable
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("smart.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
        // at the page's own 200 dpi too (review P2-1: a box of 2000 working pixels over the smart object's 1654 × 2339 — its
        // window cut to the grid — drawn, the smart object not left out)
        const auto full = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                  QStringLiteral("200"), QStringLiteral("--out"), path(QStringLiteral("smart200.png"))});
        QVERIFY2(full.exit_code == 0, qPrintable(full.described()));
        const genko::render::Image page = genko::render::read_png(genko::test::read_bytes(path(QStringLiteral("smart200.png"))));
        const auto shown = page.convert("RGB").getpixel(page.width() / 3, page.height() / 3);
        QVERIFY2(shown[0] < 200 && shown[2] < 200, qPrintable(QStringLiteral("%1 %2 %3").arg(shown[0]).arg(shown[1]).arg(shown[2])));  // (not the paper)
        QVERIFY(storage::load_document(path_of(book)).document.read_only_reason.empty());
        // undone: the feature goes with the filters
        int exit = -1;
        (void)genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY(!has(project(book)["features"], "filters@1"));
        (void)genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY(has(project(book)["features"], "filters@1"));
        // a reader that does not know filters@1 (a later version of it stands for it) opens the book read-only
        Json later = project(book);
        for (Json& feature : later["features"])
            if (feature == Json("filters@1")) feature = "filters@2";
        genko::test::write_bytes(book + QStringLiteral("/project.json"), later.dump());
        const storage::LoadResult unknown = storage::load_document(path_of(book));
        QVERIFY2(unknown.document.read_only_reason.find("filters@2") != std::string::npos, unknown.document.read_only_reason.c_str());
    }

    void aBookWithoutThemIsWrittenAsItWas() {
        // (no feature added; written again, the same bytes — the pages, their smart objects with Python's filters)
        const QString book = two_pages(QStringLiteral("plain.genko"));
        const Json made = apply(book, QStringLiteral("plain.json"),
                                Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "blur"}, {"radius", 2}},
                                             Json{{"op", "smart_convert"}, {"page", 2}, {"ids", Json::array({"pic2"})}, {"new_id", "so2"}},
                                             Json{{"op", "smart_filter_add"}, {"page", 2}, {"id", "so2"}, {"kind", "motion_blur"}, {"distance", 6}}}));
        QVERIFY2(made.value("ok", false), made.dump().c_str());
        const std::string written = genko::test::read_bytes(book + QStringLiteral("/project.json"));
        const Json saved = Json::parse(written);
        QVERIFY(!has(saved["features"], "filters@1"));
        const storage::LoadResult loaded = storage::load_document(path_of(book));
        QVERIFY(loaded.document.read_only_reason.empty());
        storage::AssetStore store(path_of(book));
        const std::string again = storage::project_json_v4(loaded.document, store);
        QCOMPARE(Json::parse(again)["features"], saved["features"]);
        const Json differs = Json::diff(saved, Json::parse(again));
        QVERIFY2(differs.empty(), differs.dump().substr(0, 1000).c_str());
        QVERIFY2(again == written, "written again, project.json is not the same bytes");
    }

    void theCatalogue() {
        const QString book = two_pages(QStringLiteral("catalogue.genko"));
        int exit = -1;
        const Json catalogue = genko({QStringLiteral("inspect"), book, QStringLiteral("--filters")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(catalogue["units"]["length"], Json("px200"));
        bool gaussian = false, box = false;
        for (const Json& kind : catalogue["kinds"]) {
            gaussian = gaussian || kind["kind"] == Json("blur_gaussian");
            box = box || kind["kind"] == Json("blur_box");
        }
        QVERIFY(gaussian && box);
        QCOMPARE(catalogue["presets"][0]["upstream"], Json("filter.blur.blur"));
    }

    // フィルターギャラリー (D3) through the command line: filter_raster's filter_gallery on the 8-bit page and the 16-bit one
    // (the reply: the stack in its normal form), saved and opened again, undone and done again; an effect not known refused;
    // as a smart filter — the book names filters@1, drawn at the page's 200 dpi —; genko inspect --filters with the 47.
    void theGalleryThroughTheCommandLine() {
        const QString book = two_pages(QStringLiteral("gallery.genko"));
        const Json before = project(book);
        std::string precise_layer;
        for (const Json& layer : before["pages"][1]["layers"])
            if (layer.contains("color_raster")) precise_layer = layer["id"].get<std::string>();
        QVERIFY(!precise_layer.empty());
        const Json effects = Json::array({Json{{"filter", "cutout"}, {"params", Json{{"number_of_levels", 3}}}},
                                          Json{{"filter", "texturizer"}, {"params", Json{{"relief", 20}}}}});
        const Json done = apply(book, QStringLiteral("gallery.json"),
                                Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "filter_gallery"}, {"effects", effects}},
                                             Json{{"op", "filter_raster"}, {"page", 2}, {"id", precise_layer}, {"kind", "filter_gallery"}, {"effects", effects},
                                                  {"foreground", Json::array({200, 0, 0})}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(done["results"][0]["filter"]["kind"], Json("filter_gallery"));
        QCOMPARE(done["results"][0]["filter"]["effects"][0]["params"], (Json{{"number_of_levels", 3}, {"edge_simplicity", 4}, {"edge_fidelity", 2}}));
        QCOMPARE(done["results"][0]["filter"]["effects"][1]["visible"], Json(true));
        QCOMPARE(done["results"][1]["filter"]["foreground"], (Json::array({200, 0, 0})));
        QCOMPARE(done["results"][1]["filter"]["background"], (Json::array({255, 255, 255})));
        const Json after = project(book);
        QVERIFY(layer_in(after, 1, "pic1")["asset"] != layer_in(before, 1, "pic1")["asset"]);
        QVERIFY(layer_in(after, 2, precise_layer)["color_raster"] != layer_in(before, 2, precise_layer)["color_raster"]);
        QVERIFY(!has(after["features"], "filters@1"));
        QVERIFY(storage::load_document(path_of(book)).document.read_only_reason.empty());
        int exit = -1;
        (void)genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(layer_in(project(book), 1, "pic1")["asset"], layer_in(before, 1, "pic1")["asset"]);
        (void)genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(layer_in(project(book), 1, "pic1")["asset"], layer_in(after, 1, "pic1")["asset"]);
        // refused: an effect the gallery has not (the book as it was)
        const Json unknown = apply(book, QStringLiteral("nope.json"),
                                   Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic1"}, {"kind", "filter_gallery"},
                                                     {"effects", Json::array({Json{{"filter", "nope"}}})}}}), &exit);
        QVERIFY2(exit != 0 && unknown.dump().find("the gallery has no effect") != std::string::npos, unknown.dump().c_str());
        // a smart filter: filters@1, its stack in its normal form, drawn at 200 dpi
        const Json smart = apply(book, QStringLiteral("smart.json"),
                                 Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic1"})}, {"new_id", "so"}},
                                              Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "filter_gallery"},
                                                   {"effects", Json::array({Json{{"filter", "stamp"}}, Json{{"filter", "grain"}, {"visible", false}}})}}}));
        QVERIFY2(smart.value("ok", false), smart.dump().c_str());
        const Json saved = project(book);
        QVERIFY2(has(saved["features"], "filters@1"), saved["features"].dump().c_str());
        const Json filter = layer_in(saved, 1, "so")["smart"]["filters"][0];
        QCOMPARE(filter["kind"], Json("filter_gallery"));
        QCOMPARE(filter["params"]["effects"][0]["params"], (Json{{"light_dark_balance", 25}, {"smoothness", 5}}));
        QCOMPARE(filter["params"]["effects"][1]["visible"], Json(false));
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("200"), QStringLiteral("--out"), path(QStringLiteral("gallery200.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
        QVERIFY(storage::load_document(path_of(book)).document.read_only_reason.empty());
        // a gallery smart filter that cannot be read (an effect the gallery has not, none, 33, visible not true or false, a key
        // an effect has not, a setting it has not): the book opens read-only; written back as it was, writable again
        const std::string written = genko::test::read_bytes(book + QStringLiteral("/project.json"));
        const auto broken = [&](const std::function<void(Json& stack)>& change) {
            Json saved_again = Json::parse(written);
            for (Json& layer : saved_again["pages"][0]["layers"])
                if (layer.value("id", std::string()) == "so") change(layer["smart"]["filters"][0]["params"]["effects"]);
            genko::test::write_bytes(book + QStringLiteral("/project.json"), saved_again.dump());
            const std::string why = storage::load_document(path_of(book)).document.read_only_reason;
            genko::test::write_bytes(book + QStringLiteral("/project.json"), written);
            return why;
        };
        QVERIFY(!broken([](Json& stack) { stack[0]["filter"] = "nope"; }).empty());
        QVERIFY(!broken([](Json& stack) { stack = Json::array(); }).empty());
        QVERIFY(!broken([](Json& stack) {
                    while (stack.size() < 33) stack.push_back(Json{{"filter", "cutout"}});
                }).empty());
        QVERIFY(!broken([](Json& stack) { stack[0]["visible"] = "yes"; }).empty());
        QVERIFY(!broken([](Json& stack) { stack[0]["opacity"] = 1; }).empty());
        QVERIFY(!broken([](Json& stack) { stack[0]["params"]["pencil_width"] = 3; }).empty());
        QVERIFY(storage::load_document(path_of(book)).document.read_only_reason.empty());
        // the catalogue: the 47 effects with PhotoCraft's commands
        const Json catalogue = genko({QStringLiteral("inspect"), book, QStringLiteral("--filters")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(catalogue["gallery"].size(), std::size_t(47));
        QCOMPARE(catalogue["gallery"][11]["command"], Json("filter.gallery.smudgeStick"));
        QCOMPARE(genko::test::book_problems(book), std::string());
    }
};

QTEST_GUILESS_MAIN(TestContractFiltersPc4)
#include "test_contract_filters_pc4.moc"
