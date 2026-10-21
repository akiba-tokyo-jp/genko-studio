// PC2a (SPEC.md COLOR-04, schema-v4.md「補正（adjustments@1）」) through the command line, as a person or an agent uses
// it: a book of every kind of page — RGB 8, 16 and 32 bits, RGB of a profile, grey, bitmap, indexed, duotone, CMYK, Lab,
// multichannel — corrected (add_layer, set_adjustment, filter_raster, auto_adjust: apply), its representative pixel asked
// before and after (inspect --pixel), drawn (render), saved and read again (adjustments@1), undone and done again; a 3D
// LUT's table kept as the book's asset, still there after Undo and gc, and asked (inspect --looks); the page's
// corrections as a .cube (export --format cube); the PSD's adjustment layers (the user's decision 5) read back by
// psd-tools (the reference Python: tools/migration/psd_adjust_check.py) and by this build's import_psd. (Python has no
// such corrections: this build's own contract.)

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "core/lut3d.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

constexpr int kPages = 11;
// The pages' modes, in order (the set_color_mode and the other ops that make them).
const std::vector<std::string> kModes{"rgb8", "rgb16", "rgb32", "p3", "gray", "bitmap", "indexed", "duotone", "cmyk", "lab", "multichannel"};

}  // namespace

class TestContractAdjust : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        // (a command over the eleven pages at print resolution takes minutes in ASan with tests run beside it: stopped as
        // hung only past the function's own limit, QTEST_FUNCTION_TIMEOUT 1700 s)
        const auto ran = genko::test::run_genko(args, {}, {}, {}, 1500000);
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
    Json apply(const QString& book, const QString& name, const Json& ops) {
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    // The page's representative pixel: the grey (128) at 10 mm; on the indexed page the green at 150 mm (an inverted grey,
    // 127, is the table's grey again there).
    Json pixel(const QString& book, int page) {
        const int x = kModes[std::size_t(page - 1)] == "indexed" ? 150 : 10;
        return genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("%1:%2,20").arg(page).arg(x), QStringLiteral("--dpi"),
                      QStringLiteral("40")});
    }
    // The id of the page's one layer of pixels (after a mode that flattens them, its new one).
    std::string pixel_layer(const Json& saved, int page) {
        for (const Json& layer : saved["pages"][page - 1]["layers"]) {
            if (layer.contains("asset") || layer.contains("ink_raster") || layer.contains("color_raster")) return layer["id"].get<std::string>();
        }
        return {};
    }
    QString asset(const QString& book, const std::string& ref, const char* suffix) {
        return book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") + QString::fromStdString(ref.substr(7)) +
               QString::fromLatin1(suffix);
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void everyPageThroughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QString::number(kPages)});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a picture on each page: a grey (128) on the left, a green on the right
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 40}, genko::render::Ink{128, 128, 128, 255});
        picture.paste(genko::render::Ink{30, 160, 90, 255}, genko::render::Box{30, 0, 60, 40});
        const std::string png = genko::core::b64encode(genko::render::write_png(picture));
        Json ops = Json::array();
        for (int p = 1; p <= kPages; ++p) {
            const std::string id = "pic" + std::to_string(p);
            ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "paint"}, {"id", id}});
            ops.push_back(Json{{"op", "set_layer"}, {"page", p}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
            ops.push_back(Json{{"op", "put_raster"}, {"page", p}, {"id", id}, {"png_base64", png}});
        }
        QVERIFY(apply(book, QStringLiteral("paint.json"), ops).value("ok", false));
        const std::string icc = genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")));
        const Json modes = apply(book, QStringLiteral("modes.json"),
                                 Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}},
                                              Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "rgb"}, {"depth", 32}},
                                              Json{{"op", "set_color_mode"}, {"page", 4}, {"mode", "rgb"}, {"depth", 16}, {"profile", "display-p3"}},
                                              Json{{"op", "set_color_mode"}, {"page", 5}, {"mode", "gray"}, {"depth", 16}},
                                              Json{{"op", "set_color_mode"}, {"page", 6}, {"mode", "gray"}},
                                              Json{{"op", "to_bitmap"}, {"page", 6}, {"method", "threshold"}},
                                              Json{{"op", "to_indexed"}, {"page", 7}, {"colors", 8}},
                                              Json{{"op", "set_color_mode"}, {"page", 8}, {"mode", "gray"}},
                                              Json{{"op", "set_duotone"}, {"page", 8}},
                                              Json{{"op", "set_color_mode"}, {"page", 9}, {"mode", "cmyk"}, {"icc", genko::core::b64encode(icc)}, {"depth", 16}},
                                              Json{{"op", "set_color_mode"}, {"page", 10}, {"mode", "lab"}, {"depth", 16}},
                                              Json{{"op", "set_color_mode"}, {"page", 11}, {"mode", "multichannel"}}}));
        QVERIFY2(modes.value("ok", false), modes.dump().c_str());
        const Json moded = project(book);
        std::vector<Json> before;
        for (int p = 1; p <= kPages; ++p) before.push_back(pixel(book, p));
        // an invert over every page: an RGB page's representative pixel its complement, the others' changed
        ops = Json::array();
        for (int p = 1; p <= kPages; ++p) ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "adjust"}, {"id", "inv" + std::to_string(p)}, {"adjust", Json{{"kind", "invert"}}}});
        QVERIFY(apply(book, QStringLiteral("invert.json"), ops).value("ok", false));
        for (int p = 1; p <= kPages; ++p) {
            const Json after = pixel(book, p);
            const std::string mode = kModes[std::size_t(p - 1)];
            QVERIFY2(after.contains("values") && after["values"] != before[std::size_t(p - 1)]["values"], (mode + ": " + after.dump()).c_str());
            if (p <= 4) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const double was = before[std::size_t(p - 1)]["values"][c].get<double>(), now = after["values"][c].get<double>();
                    QVERIFY2(std::abs(now - (1 - was)) <= 1.0 / 255 + 1e-6, (mode + ": " + after.dump()).c_str());
                }
            }
        }
        // the tone corrections of PC2a on every page: a correction layer set again, the pixels' own (filter_raster,
        // auto_adjust), a lookup
        ops = Json::array();
        for (int p = 1; p <= kPages; ++p) {
            const std::string n = std::to_string(p);
            const std::string pic = pixel_layer(moded, p);
            QVERIFY2(!pic.empty(), n.c_str());
            ops.push_back(Json{{"op", "set_layer"}, {"page", p}, {"id", "inv" + n}, {"visible", false}});
            ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "adjust"}, {"id", "lv" + n},
                               {"adjust", Json{{"kind", "levels"}, {"black", 30}, {"native", Json{{"method", "upstream"}}}}}});
            ops.push_back(Json{{"op", "set_adjustment"}, {"page", p}, {"id", "lv" + n}, {"gamma", 1.5}});
            ops.push_back(Json{{"op", "filter_raster"}, {"page", p}, {"id", pic}, {"kind", "posterize"}, {"levels", 6}, {"native", Json{{"method", "floor"}}}});
            ops.push_back(Json{{"op", "auto_adjust"}, {"page", p}, {"id", pic}, {"mode", "contrast"}});
            ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "adjust"}, {"id", "look" + n}, {"adjust", Json{{"kind", "color_lookup"}, {"look", "warm"}}}});
        }
        const Json toned = apply(book, QStringLiteral("tones.json"), ops);
        QVERIFY2(toned.value("ok", false), toned.dump().c_str());
        Json saved = project(book);
        QVERIFY2(std::find(saved["features"].begin(), saved["features"].end(), Json("adjustments@1")) != saved["features"].end(), saved["features"].dump().c_str());
        for (int p = 1; p <= kPages; ++p) {
            bool found = false;
            for (const Json& layer : saved["pages"][p - 1]["layers"]) {
                if (layer.value("id", std::string()) != "lv" + std::to_string(p)) continue;
                found = true;
                QCOMPARE(layer["adjust"]["gamma"], Json(1.5));
                QCOMPARE(layer["adjust"]["native"]["method"], Json("upstream"));
            }
            QVERIFY(found);
            const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QString::number(p), QStringLiteral("--dpi"),
                                                       QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("p%1.png").arg(p))});
            QVERIFY2(drawn.exit_code == 0 && QFile::exists(path(QStringLiteral("p%1.png").arg(p))), qPrintable(kModes[std::size_t(p - 1)].c_str() + drawn.described()));
        }
        // undone and done again
        int exit = -1;
        (void)genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        QVERIFY(project(book)["pages"][0]["layers"].dump().find("\"lv1\"") == std::string::npos);
        (void)genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        // (the same pages: the history's states are canonical JSON, keys sorted, as Python's are — a correction's settings
        // come back the same, their keys in that order)
        const Json redone = project(book);
        const Json differs = Json::diff(saved["pages"], redone["pages"]);
        QVERIFY2(differs.empty(), differs.dump().substr(0, 2000).c_str());

        // a 3D LUT's table: the book's asset, kept after Undo and gc, asked with --looks
        std::string cube = "TITLE \"inv\"\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
        const Json lut = apply(book, QStringLiteral("lut.json"),
                               Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "lut1"},
                                                 {"adjust", Json{{"kind", "color_lookup"}, {"data", cube}, {"file_name", "inv.cube"}}}}}));
        QVERIFY2(lut.value("ok", false), lut.dump().c_str());
        saved = project(book);
        std::string ref;
        for (const Json& layer : saved["pages"][0]["layers"])
            if (layer.value("id", std::string()) == "lut1") ref = layer["adjust"]["lut"].get<std::string>();
        QVERIFY(ref.starts_with("sha256:"));
        QVERIFY(QFile::exists(asset(book, ref, ".gklut")));
        const Json looks = genko({QStringLiteral("inspect"), book, QStringLiteral("--looks")}, &exit);
        QCOMPARE(exit, 0);
        QCOMPARE(looks["looks"].size(), std::size_t(8));
        QVERIFY2(looks["layers"].dump().find(ref) != std::string::npos, looks.dump().c_str());
        (void)genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        const Json gc = genko({QStringLiteral("gc"), book}, &exit);
        QVERIFY2(exit == 0, gc.dump().c_str());
        QVERIFY2(QFile::exists(asset(book, ref, ".gklut")), "the table kept: Redo still needs it");
        (void)genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("lut.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));

        // the page's corrections as a .cube
        const auto exported = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("look.cube")), QStringLiteral("--format"), QStringLiteral("cube"),
                                                      QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--lut-size"), QStringLiteral("17"), QStringLiteral("--json")});
        QVERIFY2(exported.exit_code == 0, qPrintable(exported.described()));
        const Json exported_reply = genko::test::one_line(exported.out);
        QCOMPARE(exported_reply["size"], Json(17));
        const genko::core::lut3d::Table table = genko::core::lut3d::parse_cube(genko::test::read_bytes(path(QStringLiteral("look.cube"))));
        QCOMPARE(table.size, std::size_t(17));
        const auto bad_size = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("x.cube")), QStringLiteral("--format"), QStringLiteral("cube"),
                                                      QStringLiteral("--lut-size"), QStringLiteral("1")});
        QVERIFY(bad_size.exit_code != 0);
        // (Python's formats as they were: --lut-size is the cube's only)
        const auto not_cube = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("png")), QStringLiteral("--lut-size"), QStringLiteral("9")});
        QVERIFY(not_cube.exit_code != 0);
    }

    // The PSD's adjustment layers (the user's decision 5: always written and read): an RGB page's tone corrections
    // written as Photoshop's adjustment layers in their places — read by psd-tools, the same kinds and settings — and read
    // back by this build's import_psd as the same corrections.
    void psdAdjustmentLayers() {
        const QString book = path(QStringLiteral("psd.genko"));
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 40}, genko::render::Ink{128, 128, 128, 255});
        const std::vector<Json> kinds{
            Json{{"kind", "levels"}, {"black", 10}, {"white", 240}, {"gamma", 1.2}, {"out_black", 5}, {"out_white", 250},
                 {"native", Json{{"method", "upstream"}, {"space", "rgb"}, {"channels", Json{{"red", Json{{"black", 3}, {"white", 255}, {"gamma", 0.8}, {"out_black", 0}, {"out_white", 255}}}}}}}},
            Json{{"kind", "curve"}, {"points", Json::array({Json::array({0, 0}), Json::array({128, 150}), Json::array({255, 255})})}, {"native", Json{{"method", "upstream"}}}},
            Json{{"kind", "brightness_contrast"}, {"brightness", 30}, {"contrast", 20}, {"native", Json{{"method", "modern"}}}},
            Json{{"kind", "posterize"}, {"levels", 7}, {"native", Json{{"method", "floor"}}}},
            Json{{"kind", "threshold"}, {"threshold", 100}},
            Json{{"kind", "invert"}},
            Json{{"kind", "exposure"}, {"exposure", 0.5}, {"offset", 0.0}, {"gamma", 1.0}},
            Json{{"kind", "gradient_map"}, {"native", Json{{"reverse", true}}}},
            Json{{"kind", "color_lookup"}, {"look", "warm"}},
        };
        Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}});
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj" + std::to_string(i)}, {"name", "補正" + std::to_string(i)},
                               {"adjust", kinds[i]}});
        }
        QVERIFY(apply(book, QStringLiteral("psd-ops.json"), ops).value("ok", false));
        const auto exported = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("p1.psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                      QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(exported.exit_code == 0, qPrintable(exported.described()));
        // read back by this build: the same corrections, in their places
        const Json imported = apply(book, QStringLiteral("import.json"),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 2}, {"path", path(QStringLiteral("p1.psd")).toStdString()}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        std::vector<Json> read;
        const Json back = project(book);
        for (const Json& layer : back["pages"][1]["layers"])
            if (layer.value("kind", std::string()) == "adjust") read.push_back(layer["adjust"]);
        QCOMPARE(read.size(), kinds.size());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i]["kind"] == Json("color_lookup")) {  // (a look comes back as its table)
                QCOMPARE(read[i]["kind"], Json("color_lookup"));
                QCOMPARE(read[i]["name"], Json("Warm Filter"));
                continue;
            }
            if (kinds[i]["kind"] == Json("curve")) {  // (the PSD holds every channel's curve: the straight ones are not records)
                QCOMPARE(read[i]["points"], kinds[i]["points"]);
                continue;
            }
            if (kinds[i]["kind"] == Json("gradient_map")) {  // (Python's default colours as their two stops)
                QCOMPARE(read[i]["native"], kinds[i]["native"]);
                QCOMPARE(read[i]["stops"].size(), std::size_t(2));
                continue;
            }
            QCOMPARE(read[i], kinds[i]);
        }
        // read by psd-tools
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {QStringLiteral("-I"), genko::test::repo_root() + QStringLiteral("/tools/migration/psd_adjust_check.py"), path(QStringLiteral("answer.json")),
                                           path(QStringLiteral("p1.psd"))},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        const Json answer = genko::test::read_json(path(QStringLiteral("answer.json")));
        QVERIFY2(!answer[0].contains("error"), answer.dump().c_str());
        std::vector<Json> adjustments;
        for (const Json& layer : answer[0]["layers"])
            if (layer["name"].get<std::string>().starts_with("補正")) adjustments.push_back(layer);
        QCOMPARE(adjustments.size(), kinds.size());
        QCOMPARE(adjustments[0]["kind"], Json("levels"));
        QCOMPARE(adjustments[0]["records"][0], Json::array({10, 240, 5, 250, 120}));
        QCOMPARE(adjustments[0]["records"][1], Json::array({3, 255, 0, 255, 80}));
        QCOMPARE(adjustments[1]["kind"], Json("curves"));
        QCOMPARE(adjustments[1]["curves"][0], Json::array({Json::array({0, 0}), Json::array({150, 128}), Json::array({255, 255})}));  // ([out, in])
        QCOMPARE(adjustments[2]["kind"], Json("brightnesscontrast"));
        QCOMPARE(adjustments[2]["brightness"], Json(30));
        QCOMPARE(adjustments[2]["contrast"], Json(20));
        QCOMPARE(adjustments[2]["legacy"], Json(false));
        QCOMPARE(adjustments[3]["levels"], Json(7));
        QCOMPARE(adjustments[4]["level"], Json(100));
        QCOMPARE(adjustments[5]["kind"], Json("invert"));
        QCOMPARE(adjustments[6]["exposure"], Json(0.5));
        QCOMPARE(adjustments[7]["kind"], Json("gradientmap"));
        QCOMPARE(adjustments[7]["reversed"], Json(true));
        QCOMPARE(adjustments[8]["kind"], Json("colorlookup"));
        QCOMPARE(adjustments[8]["size"], Json(33));
    }

    // A 2³ .cube whose every entry is scaled by k (another k: another table).
    static std::string scaled_cube(double k) {
        std::string out = "LUT_3D_SIZE 2\n";
        char line[64];
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) {
                    std::snprintf(line, sizeof line, "%.4f %.4f %.4f\n", r * k, g * k, b * k);
                    out += line;
                }
        return out;
    }
    // A new book of `pages` with a picture of one colour on page 1 (a paint layer "pic").
    QString picture_book(const QString& name, int pages = 1) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QString::number(pages)});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        const genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 40}, genko::render::Ink{200, 120, 60, 255});
        (void)apply(book, name + QStringLiteral("-pic.json"),
                    Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}}));
        return book;
    }
    // The book is read again and can be changed (not read-only).
    bool opens(const QString& book, const QString& name) {
        return apply(book, name, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}}})).value("ok", false);
    }
    std::vector<std::string> lookup_refs(const Json& saved, int page) {
        std::vector<std::string> out;
        for (const Json& layer : saved["pages"][page - 1]["layers"])
            if (layer.value("kind", std::string()) == "adjust" && layer["adjust"].contains("lut")) out.push_back(layer["adjust"]["lut"].get<std::string>());
        return out;
    }

    // A page of another book whose correction uses a 3D LUT's table (import_pages, 作品の結合): the table comes with the
    // page — the book is written, read again (not read-only) and drawn as the other book draws that page —, each command
    // a process of its own (no table an earlier op made in it).
    void importPagesTakeTheirTables() {
        const QString a = picture_book(QStringLiteral("lut-a.genko"));
        const Json made = apply(a, QStringLiteral("lut-a.json"),
                                Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "half"},
                                                  {"adjust", Json{{"kind", "color_lookup"}, {"data", scaled_cube(0.5)}, {"file_name", "half.cube"}}}}}));
        QVERIFY2(made.value("ok", false), made.dump().c_str());
        const QString b = path(QStringLiteral("lut-b.genko"));
        QCOMPARE(genko::test::run_genko({QStringLiteral("new"), b, QStringLiteral("--pages"), QStringLiteral("1")}).exit_code, 0);
        const Json imported = apply(b, QStringLiteral("lut-import.json"), Json::array({Json{{"op", "import_pages"}, {"from", a.toStdString()}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        const std::vector<std::string> refs = lookup_refs(project(b), 2);
        QCOMPARE(refs.size(), std::size_t(1));
        QCOMPARE(refs, lookup_refs(project(a), 1));
        QVERIFY(QFile::exists(asset(b, refs[0], ".gklut")));
        const Json there = genko({QStringLiteral("inspect"), a, QStringLiteral("--pixel"), QStringLiteral("1:10,20"), QStringLiteral("--dpi"), QStringLiteral("40")});
        const Json here = genko({QStringLiteral("inspect"), b, QStringLiteral("--pixel"), QStringLiteral("2:10,20"), QStringLiteral("--dpi"), QStringLiteral("40")});
        QVERIFY2(there.contains("values") && here["values"] == there["values"], (here.dump() + " / " + there.dump()).c_str());
        QVERIFY2(std::abs(there["values"][0].get<double>() - 100 / 255.0) < 3 / 255.0, there.dump().c_str());  // (the table: half of 200)
        QVERIFY(opens(b, QStringLiteral("lut-b-again.json")));
    }

    // A PSD of nine color_lookup adjustment layers, each its own table, read into a book that holds none of them: every
    // table comes into the book with its correction (written as its asset), each command a process of its own.
    void psdOfNineTables() {
        const QString c = picture_book(QStringLiteral("nine.genko"));
        Json ops = Json::array();
        for (int i = 0; i < 9; ++i)
            ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "lut" + std::to_string(i)}, {"name", "表" + std::to_string(i)},
                               {"adjust", Json{{"kind", "color_lookup"}, {"data", scaled_cube(0.5 + i * 0.05)}, {"file_name", "t" + std::to_string(i) + ".cube"}}}});
        QVERIFY(apply(c, QStringLiteral("nine.json"), ops).value("ok", false));
        const std::vector<std::string> made = lookup_refs(project(c), 1);
        QCOMPARE(std::set<std::string>(made.begin(), made.end()).size(), std::size_t(9));
        const auto exported = genko::test::run_genko({QStringLiteral("export"), c, path(QStringLiteral("nine.psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                      QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(exported.exit_code == 0, qPrintable(exported.described()));
        const QString d = picture_book(QStringLiteral("nine-in.genko"));
        const Json imported = apply(d, QStringLiteral("nine-import.json"),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", path(QStringLiteral("nine.psd")).toStdString()}}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        const std::vector<std::string> read = lookup_refs(project(d), 1);
        QCOMPARE(read.size(), std::size_t(9));
        for (const std::string& ref : read) QVERIFY2(QFile::exists(asset(d, ref, ".gklut")), ref.c_str());
        QVERIFY(opens(d, QStringLiteral("nine-again.json")));
    }
};

QTEST_GUILESS_MAIN(TestContractAdjust)
#include "test_contract_adjust.moc"
