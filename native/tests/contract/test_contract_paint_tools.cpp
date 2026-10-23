// PC4 E5 (SPEC.md COLOR-09「画素を変える筆」) through the command line, as a person or an agent uses it: the nine brushes that
// change pixels (dodge, burn, sponge, blur_brush, sharpen_brush, smudge_brush, mixer_brush, background_eraser,
// color_replacement) applied and saved on an 8-bit page, a 16-bit page and a CMYK page, undone and done again; the same
// steps in another book give the same bytes; the Mixer Brush's reservoir handed to the next stroke; the page drawn after;
// a refused op changes nothing. (Python has none of these: this build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <array>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

class TestContractPaintTools : public QObject {
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

    // A new book (`name`) of one page, a paint layer "pic" (stripes of colour, a black square, white around), the page's
    // mode `mode` ("rgb8", "rgb16" or "cmyk").
    QString make_book(const QString& name, const std::string& mode) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        if (made.exit_code != 0) return {};
        std::string bytes;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const bool square = x >= 24 && x < 32 && y >= 24 && y < 32;
                const std::array<int, 4> c = square ? std::array<int, 4>{0, 0, 0, 255}
                                             : y < 20 ? ((x / 4) % 2 == 0 ? std::array<int, 4>{200, 80, 60, 255} : std::array<int, 4>{60, 120, 200, 255})
                                                      : std::array<int, 4>{255, 255, 255, 255};
                for (const int v : c) bytes.push_back(static_cast<char>(v));
            }
        const genko::render::Image picture = genko::render::Image::frombytes("RGBA", genko::render::Size{40, 40}, bytes)
                                                 .resize(genko::render::Size{1654, 2339}, genko::render::Resample::Nearest);
        Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}});
        if (mode == "rgb16") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        if (mode == "cmyk") ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
        const Json done = apply(book, name + QStringLiteral("-make.json"), ops);
        return done.value("ok", false) ? book : QString();
    }

    // The nine brushes, one stroke each (mm on an A4 page: the stripes are its top half, the black square 126..168 mm across and
    // 178..238 mm down, white around it).
    static Json strokes() {
        const auto line = [](const char* op, double x0, double y0, double x1, double y1, double width) {
            return Json{{"op", op}, {"page", 1}, {"layer_id", "pic"}, {"points", Json::array({Json::array({x0, y0}), Json::array({x1, y1})})},
                        {"width_mm", width}, {"hardness", 50}};
        };
        Json ops = Json::array();
        ops.push_back(line("dodge", 20, 40, 180, 40, 8));
        Json burn = line("burn", 20, 60, 180, 60, 8);
        burn["range"] = "highlights";
        ops.push_back(burn);
        ops.push_back(line("sponge", 20, 80, 180, 80, 8));
        Json blur = line("blur_brush", 20, 100, 180, 100, 10);
        blur["strength"] = 100;
        ops.push_back(blur);
        Json sharpen = line("sharpen_brush", 20, 120, 180, 120, 10);
        sharpen["strength"] = 100;
        sharpen["protect_detail"] = false;  // (the stripes' edges are steps: ディテールを保持 keeps a step within its own range)
        ops.push_back(sharpen);
        ops.push_back(line("smudge_brush", 110, 200, 150, 200, 6));  // (white into the black square)
        Json mixer = line("mixer_brush", 20, 20, 180, 20, 6);
        mixer["rgb"] = Json::array({20, 160, 40});
        mixer["wet"] = 80;
        mixer["clean_after"] = false;
        ops.push_back(mixer);
        Json eraser = line("background_eraser", 120, 210, 200, 210, 12);
        eraser["sampling"] = "once";
        ops.push_back(eraser);
        Json replace = line("color_replacement", 20, 130, 180, 130, 8);
        replace["rgb"] = Json::array({250, 200, 0});
        replace["tolerance"] = 40;
        ops.push_back(replace);
        return ops;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void throughTheCommandLine() {
        for (const std::string mode : {"rgb8", "rgb16", "cmyk"}) {
            const QString m = QString::fromStdString(mode);
            const QString book = make_book(QStringLiteral("book-") + m, mode);
            QVERIFY2(!book.isEmpty(), mode.c_str());
            const Json before = layer_of(project(book), "pic");
            int exit = -1;
            Json done = apply(book, QStringLiteral("strokes-") + m + QStringLiteral(".json"), strokes(), &exit);
            QVERIFY2(exit == 0 && done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
            QCOMPARE(done["results"].size(), std::size_t{9});
            for (const Json& r : done["results"]) {
                QVERIFY2(r["changed"] == Json(true), (mode + " " + r.dump()).c_str());
                QVERIFY(r["box_mm"].is_array());
            }
            const Json reservoir = done["results"][6]["reservoir"];
            QVERIFY2(reservoir["paint"].is_array() && reservoir["pickup"].is_array(), reservoir.dump().c_str());
            const Json after = layer_of(project(book), "pic");
            QVERIFY(after != before);
            // undone and done again
            Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
            QVERIFY2(exit == 0, undone.dump().c_str());
            QCOMPARE(layer_of(project(book), "pic"), before);
            Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
            QVERIFY2(exit == 0, redone.dump().c_str());
            QCOMPARE(layer_of(project(book), "pic"), after);
            // the same steps in another book: the same bytes
            const QString twin = make_book(QStringLiteral("twin-") + m, mode);
            QVERIFY(!twin.isEmpty());
            QCOMPARE(layer_of(project(twin), "pic"), before);
            done = apply(twin, QStringLiteral("strokes-twin-") + m + QStringLiteral(".json"), strokes(), &exit);
            QVERIFY2(exit == 0, done.dump().c_str());
            QCOMPARE(layer_of(project(twin), "pic"), after);
            // the Mixer Brush's paint handed to the next stroke
            Json next = Json{{"op", "mixer_brush"}, {"page", 1}, {"layer_id", "pic"}, {"points", Json::array({Json::array({30, 230}), Json::array({90, 230})})},
                             {"width_mm", 6}, {"reservoir", reservoir}, {"load_after", false}};
            done = apply(book, QStringLiteral("next-") + m + QStringLiteral(".json"), Json::array({next}), &exit);
            QVERIFY2(exit == 0 && done.value("ok", false), done.dump().c_str());
            QCOMPARE(done["results"][0]["reservoir"]["paint"], reservoir["paint"]);
            // the page is drawn
            const auto ran = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                     QStringLiteral("20"), QStringLiteral("--out"), path(QStringLiteral("page-") + m + QStringLiteral(".png"))});
            QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
            // refused: nothing changed
            const Json saved = project(book);
            done = apply(book, QStringLiteral("bad-") + m + QStringLiteral(".json"),
                         Json::array({Json{{"op", "dodge"}, {"page", 1}, {"layer_id", "pic"}, {"points", Json::array({Json::array({30, 30})})}, {"range", "bogus"}}}),
                         &exit);
            QVERIFY(exit != 0);
            QCOMPARE(project(book), saved);
        }
    }
};

QTEST_MAIN(TestContractPaintTools)
#include "test_contract_paint_tools.moc"
