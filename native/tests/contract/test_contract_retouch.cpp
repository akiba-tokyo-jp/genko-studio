// PC3 B4 (SPEC.md COLOR-06「修復」) through the command line, as a person or an agent uses it: the retouching ops applied
// and saved (clone_stamp, healing_brush, spot_healing), undone and done again; history_brush painting back the pixels of
// an earlier state the book holds as an asset (its `from`), which the asset collection keeps while the journal names it;
// content_aware_fill on the layer, on a new layer and on a copy; the page drawn after. (Python has none of these: this
// build's own contract.)

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using genko::core::Json;

class TestContractRetouch : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops, int* exit = nullptr) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int code = -1;
        const Json out = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &code);
        if (exit != nullptr) *exit = code;
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static Json layer_of(const Json& saved, const std::string& id) {
        for (const Json& layer : saved["pages"][0]["layers"])
            if (layer.value("id", "") == id) return layer;
        return Json();
    }
    std::filesystem::path asset_file(const QString& book, const std::string& ref, const char* suffix) {
        return std::filesystem::path(book.toStdString()) / "assets" / ref.substr(7, 2) / (ref.substr(7) + suffix);
    }
    // An asset made old (the collection leaves a young one alone).
    void age(const std::filesystem::path& file) {
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a picture over page 1 (A4 at the working 200 dpi): stripes, a dark blot in the middle
        std::string bytes;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const bool blot = std::abs(x - 20) < 3 && std::abs(y - 20) < 3;
                const int v = blot ? 0 : (x / 4) % 2 == 0 ? 60 : 200;
                for (const int c : {v, v, v, 255}) bytes.push_back(static_cast<char>(c));
            }
        const genko::render::Image picture = genko::render::Image::frombytes("RGBA", genko::render::Size{40, 40}, bytes)
                                                 .resize(genko::render::Size{1654, 2339}, genko::render::Resample::Nearest);
        Json done = apply(book, QStringLiteral("paint.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const std::string first = layer_of(project(book), "pic")["asset"].get<std::string>();

        // clone_stamp: saved (the layer another asset), its report; undone and done again
        done = apply(book, QStringLiteral("clone.json"),
                     Json::array({Json{{"op", "clone_stamp"},
                                       {"page", 1},
                                       {"layer_id", "pic"},
                                       {"points", Json::array({Json::array({30, 30}), Json::array({60, 30})})},
                                       {"width_mm", 8},
                                       {"source_mm", Json::array({40, 200})}}}));  // (across the stripes: the same x would copy them as they are)
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QCOMPARE(done["results"][0]["changed"], Json(true));
        QCOMPARE(done["results"][0]["offset_mm"], Json::array({10, 170}));
        const std::string cloned = layer_of(project(book), "pic")["asset"].get<std::string>();
        QVERIFY(cloned != first);
        Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(layer_of(project(book), "pic")["asset"].get<std::string>(), first);
        Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        QCOMPARE(layer_of(project(book), "pic")["asset"].get<std::string>(), cloned);

        // healing_brush and spot_healing (the blot at the page's middle)
        done = apply(book, QStringLiteral("heal.json"),
                     Json::array({Json{{"op", "healing_brush"},
                                       {"page", 1},
                                       {"layer_id", "pic"},
                                       {"points", Json::array({Json::array({150, 60})})},
                                       {"width_mm", 20},
                                       {"source_mm", Json::array({50, 250})},
                                       {"aligned", false}},
                                  Json{{"op", "spot_healing"},
                                       {"page", 1},
                                       {"layer_id", "pic"},
                                       {"points", Json::array({Json::array({107, 151})})},
                                       {"width_mm", 50},
                                       {"type", "content_aware"}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QCOMPARE(done["results"][0]["next_source_mm"], Json::array({50, 250}));
        QCOMPARE(done["results"][1]["changed"], Json(true));
        QVERIFY(done["results"][1]["box_mm"].is_array());

        // history_brush: painted back from the first pixels, an asset the book holds; refused for an asset it does not
        const Json brush{{"op", "history_brush"},
                         {"page", 1},
                         {"layer_id", "pic"},
                         {"points", Json::array({Json::array({30, 30}), Json::array({60, 30})})},
                         {"width_mm", 10},
                         {"from", first}};
        done = apply(book, QStringLiteral("history.json"), Json::array({brush}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(done["results"][0]["changed"], Json(true));
        Json missing = brush;
        missing["from"] = "sha256:" + std::string(64, '0');
        done = apply(book, QStringLiteral("history-missing.json"), Json::array({missing}), &exit);
        QVERIFY(exit != 0);
        QVERIFY2(done.dump().find("the book has no asset") != std::string::npos, done.dump().c_str());
        // the asset collection keeps a `from` the journal names, though no state holds it (an old one, black, put here)
        std::string black;
        for (int i = 0; i < 40 * 40; ++i) black += std::string("\x00\x00\x00\xff", 4);
        const std::string dark = genko::render::write_png(genko::render::Image::frombytes("RGBA", genko::render::Size{40, 40}, black)
                                                              .resize(genko::render::Size{1654, 2339}));
        const std::string named = genko::storage::AssetStore(book.toStdString()).put_bytes(dark, ".png");
        Json painted = brush;
        painted["from"] = named;
        done = apply(book, QStringLiteral("history-dark.json"), Json::array({painted}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        std::string stray_png = dark;
        stray_png[stray_png.size() / 2] ^= 1;  // (not a PNG any more: only an asset nothing names)
        const std::string stray = genko::storage::AssetStore(book.toStdString()).put_bytes(stray_png, ".png");
        age(asset_file(book, named, ".png"));
        age(asset_file(book, stray, ".png"));
        const Json collected = genko({QStringLiteral("gc"), book}, &exit);
        QVERIFY2(exit == 0, collected.dump().c_str());
        QVERIFY(std::filesystem::exists(asset_file(book, named, ".png")));
        QVERIFY2(!std::filesystem::exists(asset_file(book, stray, ".png")), collected.dump().c_str());

        // content_aware_fill: on a new layer (its id given), on a copy, and on the layer itself
        const Json area{{"rect", Json::array({95, 140, 25, 25})}};
        done = apply(book, QStringLiteral("fill.json"),
                     Json::array({Json{{"op", "content_aware_fill"}, {"page", 1}, {"layer_id", "pic"}, {"area", area}, {"output", "new"}, {"new_id", "filled"}},
                                  Json{{"op", "content_aware_fill"}, {"page", 1}, {"layer_id", "pic"}, {"area", area}, {"output", "duplicate"}, {"new_id", "twin"}},
                                  Json{{"op", "content_aware_fill"}, {"page", 1}, {"layer_id", "pic"}, {"area", area}, {"seed", 7}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QCOMPARE(done["results"][0]["layer"], Json("filled"));
        QCOMPARE(done["results"][1]["layer"], Json("twin"));
        QCOMPARE(done["results"][2]["layer"], Json("pic"));
        QVERIFY(done["results"][2]["filled"].get<std::int64_t>() > 0);
        const Json saved = project(book);
        QVERIFY(layer_of(saved, "filled").is_object());
        QVERIFY(layer_of(saved, "twin").is_object());
        QCOMPARE(layer_of(saved, "filled")["title"], Json("コンテンツに応じた塗りつぶし"));
        // the page is drawn
        const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                 QStringLiteral("20"), QStringLiteral("--out"), path(QStringLiteral("page.png"))});
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        // refused: nothing changed
        done = apply(book, QStringLiteral("bad.json"),
                     Json::array({Json{{"op", "clone_stamp"}, {"page", 1}, {"layer_id", "pic"}, {"points", Json::array({Json::array({30, 30})})}}}), &exit);
        QVERIFY(exit != 0);
        QCOMPARE(project(book), saved);
    }
};

QTEST_MAIN(TestContractRetouch)
#include "test_contract_retouch.moc"
