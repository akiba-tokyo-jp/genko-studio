// PC2c (局所の補正と画像の演算, SPEC.md COLOR-04) through the command line, as a person or an agent uses it: a book made, a
// picture put on page 1, filter_raster's shadows_highlights applied, saved and undone and done again; fade_layer taking it
// half back from the pixels before (an asset the book keeps, the asset collection too), refused for an asset it does not
// have; apply_image and calculations (a new alpha channel, channels@1 named; a new grey page); hdr_toning flattening
// the page (the counts reported, the panels and lines kept); trap refused on an RGB page and done on a CMYK one; the page
// drawn after. (Python has none of these ops: this build's own contract.)

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

class TestContractImageOps : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

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
    Json apply(const QString& book, const QString& name, const Json& ops, int* exit = nullptr) {
        int code = -1;
        const Json out = genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &code);
        if (exit != nullptr) *exit = code;
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    bool asset(const QString& book, const std::string& ref, const char* suffix) {
        return QFile::exists(book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") +
                             QString::fromStdString(ref.substr(7)) + QString::fromLatin1(suffix));
    }
    static Json layer_of(const Json& saved, std::size_t page, const std::string& id) {
        for (const Json& layer : saved["pages"][page]["layers"])
            if (layer.value("id", "") == id) return layer;
        return Json();
    }

private slots:
    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a picture over page 1: a dark ramp on the left, light on the right
        std::string bytes;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 90; ++x)
                for (const int v : {x * 255 / 89, x * 200 / 89, 40 + x, 255}) bytes.push_back(static_cast<char>(v));
        const genko::render::Image picture = genko::render::Image::frombytes("RGBA", genko::render::Size{90, 60}, bytes);
        Json done = apply(book, QStringLiteral("paint.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}},
                                       Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const std::string before = layer_of(project(book), 0, "pic")["asset"].get<std::string>();
        QVERIFY(asset(book, before, ".png"));
        // shadows_highlights: the layer's pixels another asset; undone and done again
        done = apply(book, QStringLiteral("shadows.json"),
                     Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "shadows_highlights"}, {"shadow_amount", 70}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const std::string lifted = layer_of(project(book), 0, "pic")["asset"].get<std::string>();
        QVERIFY(lifted != before);
        QVERIFY(!project(book).contains("features") || project(book)["features"].empty());  // (an op's settings: nothing stored of its own)
        Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(layer_of(project(book), 0, "pic")["asset"].get<std::string>(), before);
        Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        QCOMPARE(layer_of(project(book), 0, "pic")["asset"].get<std::string>(), lifted);
        // the asset collection keeps the pixels before (the journal's states name them): fade_layer reads them there
        const Json collected = genko({QStringLiteral("gc"), book}, &exit);
        QVERIFY2(exit == 0, collected.dump().c_str());
        QVERIFY(asset(book, before, ".png"));
        done = apply(book, QStringLiteral("fade.json"), Json::array({Json{{"op", "fade_layer"}, {"page", 1}, {"id", "pic"}, {"from", before}, {"opacity", 50}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QCOMPARE(done["results"][0]["faded"], Json("pic"));
        const std::string faded = layer_of(project(book), 0, "pic")["asset"].get<std::string>();
        QVERIFY(faded != lifted && faded != before);
        done = apply(book, QStringLiteral("fade_missing.json"),
                     Json::array({Json{{"op", "fade_layer"}, {"page", 1}, {"id", "pic"}, {"from", "sha256:" + std::string(64, '0')}}}), &exit);
        QVERIFY(exit != 0);
        QVERIFY2(done.dump().find("the book has no asset") != std::string::npos, done.dump().c_str());
        QCOMPARE(layer_of(project(book), 0, "pic")["asset"].get<std::string>(), faded);  // (refused: nothing changed)
        // apply_image (the page merged, screen) and calculations into a new alpha channel and a new grey page
        done = apply(book, QStringLiteral("calc.json"),
                     Json::array({Json{{"op", "apply_image"}, {"page", 1}, {"id", "pic"}, {"blending", "screen"}, {"opacity", 40}},
                                  Json{{"op", "calculations"}, {"page", 1}, {"source1", Json{{"layer", "pic"}}}, {"blending", "multiply"}, {"name", "演算"}},
                                  Json{{"op", "calculations"}, {"page", 1}, {"result", "page"}, {"source1", Json{{"layer", "pic"}, {"channel", "transparency"}}}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        Json saved = project(book);
        const auto names = [&saved](const char* feature) {
            for (const Json& f : saved["features"])
                if (f == Json(feature)) return true;
            return false;
        };
        QVERIFY(names("channels@1"));
        QCOMPARE(saved["pages"][0]["channels"][0]["name"], Json("演算"));
        QCOMPARE(saved["pages"].size(), std::size_t(3));
        QCOMPARE(saved["pages"][1]["color"]["mode"], Json("gray"));
        QCOMPARE(done["results"][2]["page"], Json(2));
        // hdr_toning: the page's pixel layers flattened (the hidden one discarded), its panels and lines kept
        done = apply(book, QStringLiteral("more.json"),
                     Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "top"}},
                                  Json{{"op", "put_raster"}, {"page", 1}, {"id", "top"},
                                       {"png_base64", genko::core::b64encode(genko::render::write_png(genko::render::Image::create("RGBA", genko::render::Size{20, 20},
                                                                                                                                 genko::render::Ink{30, 160, 90, 255})))}},
                                  Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "hidden"}},
                                  Json{{"op", "put_raster"}, {"page", 1}, {"id", "hidden"},
                                       {"png_base64", genko::core::b64encode(genko::render::write_png(genko::render::Image::create("RGBA", genko::render::Size{20, 20},
                                                                                                                                 genko::render::Ink{1, 2, 3, 255})))}},
                                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "hidden"}, {"visible", false}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book);
        const std::size_t frames = saved["pages"][0]["frames"].size();
        const std::size_t lines = saved["story"].size();
        done = apply(book, QStringLiteral("hdr.json"), Json::array({Json{{"op", "hdr_toning"}, {"page", 1}, {"strength", 1.5}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QVERIFY(done["results"][0]["flattened"].get<int>() >= 2);
        QCOMPARE(done["results"][0]["discarded"], Json(1));
        saved = project(book);
        QCOMPARE(saved["pages"][0]["frames"].size(), frames);
        QCOMPARE(saved["story"].size(), lines);
        QVERIFY(layer_of(saved, 0, "hidden").is_null());
        int pixels = 0;
        for (const Json& layer : saved["pages"][0]["layers"]) pixels += layer.contains("asset") ? 1 : 0;
        QCOMPARE(pixels, 1);
        // trap: refused on an RGB page, done on a CMYK one
        done = apply(book, QStringLiteral("trap_rgb.json"), Json::array({Json{{"op", "trap"}, {"page", 1}, {"id", "pic"}}}), &exit);
        QVERIFY(exit != 0);
        QVERIFY2(done.dump().find("CMYK") != std::string::npos, done.dump().c_str());
        done = apply(book, QStringLiteral("cmyk.json"),
                     Json::array({Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}},
                                  Json{{"op", "add_layer"}, {"page", 3}, {"kind", "paint"}, {"id", "inks"}},
                                  Json{{"op", "put_raster"}, {"page", 3}, {"id", "inks"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        done = apply(book, QStringLiteral("trap.json"), Json::array({Json{{"op", "trap"}, {"page", 3}, {"id", "inks"}, {"width_mm", 0.2}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        QCOMPARE(done["results"][0]["trapped"], Json(true));
        // drawn
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("20"), QStringLiteral("--out"), path(QStringLiteral("p1.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
    }
};

QTEST_GUILESS_MAIN(TestContractImageOps)
#include "test_contract_image_ops.moc"
