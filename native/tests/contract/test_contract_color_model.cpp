// color-model@1 (SPEC.md COLOR-03, schema-v4.md「色モデル（color-model@1）」) through the command line, as a person or
// an agent uses it: a book made, its pages put in grey of 32 bits, RGB of Display P3 and 16 bits and RGB of 32 bits
// (apply), saved and read again (the features, the pages' colour, the profiles as assets), undone and done again,
// drawn (render), asked (inspect --pixel), written (export: PSD Grayscale 32 and RGB 16/32 with their profiles, TIFF of
// floats; the notes); a book with a feature this build does not know is read-only. (Python has no colour model: this
// build's own contract.)

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

unsigned be16(const std::string& b, std::size_t at) { return unsigned(static_cast<unsigned char>(b[at])) << 8 | static_cast<unsigned char>(b[at + 1]); }
std::uint32_t be32(const std::string& b, std::size_t at) { return std::uint32_t(be16(b, at)) << 16 | be16(b, at + 2); }

// A PSD's header (channels, depth, mode) and its ICC profile (image resource 1039).
struct Psd {
    unsigned channels = 0, depth = 0, mode = 0;
    std::string icc;
};

Psd read_psd(const QString& file) {
    const std::string b = genko::test::read_bytes(file);
    Psd p;
    if (b.size() < 30 || b.compare(0, 4, "8BPS") != 0) return p;
    p.channels = be16(b, 12);
    p.depth = be16(b, 22);
    p.mode = be16(b, 24);
    std::size_t at = 26;
    at += 4 + be32(b, at);
    const std::size_t end = at + 4 + be32(b, at);
    for (std::size_t r = at + 4; r + 12 <= end;) {
        const unsigned id = be16(b, r + 4);
        const std::size_t data = r + 6 + ((static_cast<unsigned char>(b[r + 6]) + 2) & ~std::size_t(1));
        const std::size_t size = be32(b, data);
        if (id == 1039) p.icc = b.substr(data + 4, size);
        r = data + 4 + ((size + 1) & ~std::size_t(1));
    }
    return p;
}

// A little-endian TIFF's first IFD: each tag's value (the first of several; the ICC profile's: its size).
std::map<unsigned, std::uint32_t> tiff_tags(const QString& file) {
    const std::string b = genko::test::read_bytes(file);
    const auto le = [&](std::size_t at, int n) {
        std::uint32_t v = 0;
        for (int k = n - 1; k >= 0; --k) v = v << 8 | static_cast<unsigned char>(b[at + k]);
        return v;
    };
    std::map<unsigned, std::uint32_t> tags;
    if (b.size() < 8 || b.compare(0, 4, std::string("II*\0", 4)) != 0) return tags;
    const std::uint32_t ifd = le(4, 4);
    for (unsigned e = 0; e < le(ifd, 2); ++e) {
        const std::size_t at = ifd + 2 + e * 12;
        const unsigned tag = le(at, 2), type = le(at + 2, 2);
        const std::uint32_t count = le(at + 4, 4);
        std::uint32_t value = type == 3 && count <= 2 ? le(at + 8, 2) : le(at + 8, 4);
        if (type == 3 && count > 2) value = le(le(at + 8, 4), 2);
        tags[tag] = tag == 34675 ? count : value;
    }
    return tags;
}

}  // namespace

class TestContractColorModel : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    // A command of this build that prints one JSON line (ok or not); `exit` its exit code.
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

private slots:
    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("3")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a picture on page 1: a grey (128) on the left, a green on the right
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 40}, genko::render::Ink{0, 0, 0, 0});
        picture.paste(genko::render::Ink{128, 128, 128, 255}, genko::render::Box{0, 0, 30, 40});
        picture.paste(genko::render::Ink{30, 160, 90, 255}, genko::render::Box{30, 0, 60, 40});
        const Json painted = genko(
            {QStringLiteral("apply"), book,
             write_ops(QStringLiteral("paint.json"),
                       Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}})),
             QStringLiteral("--agent"), QStringLiteral("human:test")},
            &exit);
        QVERIFY2(exit == 0 && painted.value("ok", false), painted.dump().c_str());
        QVERIFY(!project(book).contains("features") || project(book)["features"].empty());
        // the modes: grey 32, Display P3 16, sRGB 32
        const Json modes = genko({QStringLiteral("apply"), book,
                                  write_ops(QStringLiteral("modes.json"),
                                            Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 32}},
                                                         Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}, {"profile", "display-p3"}},
                                                         Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "rgb"}, {"depth", 32}}})),
                                  QStringLiteral("--agent"), QStringLiteral("human:test")},
                                 &exit);
        QVERIFY2(exit == 0 && modes.value("ok", false), modes.dump().c_str());
        const std::string sgray = genko::core::builtin_profile("sgray")->ref();
        const std::string p3 = genko::core::builtin_profile("display-p3")->ref();
        const auto check_modes = [&](const Json& saved) {
            QVERIFY2(saved["features"] == Json::array({"color-mode@1", "color-model@1"}), saved["features"].dump().c_str());
            QCOMPARE(saved["pages"][0]["color"].dump(), (Json{{"mode", "gray"}, {"depth", 32}, {"profile", sgray}, {"intent", "relative"}, {"inks", Json::array()}}.dump()));
            QCOMPARE(saved["pages"][1]["color"].dump(), (Json{{"mode", "rgb"}, {"depth", 16}, {"profile", p3}, {"intent", "relative"}}.dump()));
            QCOMPARE(saved["pages"][2]["color"].dump(), (Json{{"mode", "rgb"}, {"depth", 32}, {"intent", "relative"}}.dump()));
        };
        check_modes(project(book));
        for (const std::string& ref : {sgray, p3}) {  // (the profiles kept as the book's assets)
            QVERIFY2(QFile::exists(book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") +
                                   QString::fromStdString(ref.substr(7)) + QStringLiteral(".icc")),
                     ref.c_str());
        }
        // undone and done again
        const Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        {
            const Json saved = project(book);
            for (const Json& page : saved["pages"]) QVERIFY(!page.contains("color"));
            QVERIFY(!saved.contains("features") || saved["features"].empty());
        }
        const Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        check_modes(project(book));
        // drawn
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("2"), QStringLiteral("--dpi"),
                                                   QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("p2.png"))});
        QVERIFY2(drawn.exit_code == 0 && QFile::exists(path(QStringLiteral("p2.png"))), qPrintable(drawn.described()));
        // asked: a pixel of the grey page (its lightness, 32 bits) and of the P3 page's paper
        const Json grey = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("1:10,20"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0, grey.dump().c_str());
        QCOMPARE(grey["mode"], Json("gray"));
        QCOMPARE(grey["depth"], Json(32));
        QVERIFY2(grey["values"].is_array() && grey["values"].size() == 1, grey.dump().c_str());
        const Json white = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("2:1,1"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0, white.dump().c_str());
        QCOMPARE(white["mode"], Json("rgb"));
        QCOMPARE(white["depth"], Json(16));
        QCOMPARE(white["values"], Json::array({1.0, 1.0, 1.0}));
        // written: PSD
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const Json psd_reply = genko::test::one_line(psd.out);
        QCOMPARE(psd_reply["count"], Json(3));
        QVERIFY2(!psd_reply.contains("notes"), psd_reply.dump().c_str());
        const std::string sgray_bytes = *genko::core::builtin_profile("sgray")->bytes;
        const std::string p3_bytes = *genko::core::builtin_profile("display-p3")->bytes;
        Psd p = read_psd(QString::fromStdString(psd_reply["files"][0].get<std::string>()));
        QCOMPARE(p.channels, 1u);
        QCOMPARE(p.depth, 32u);
        QCOMPARE(p.mode, 1u);
        QVERIFY(p.icc == sgray_bytes);
        p = read_psd(QString::fromStdString(psd_reply["files"][1].get<std::string>()));
        QCOMPARE(p.channels, 3u);
        QCOMPARE(p.depth, 16u);
        QCOMPARE(p.mode, 3u);
        QVERIFY(p.icc == p3_bytes);
        p = read_psd(QString::fromStdString(psd_reply["files"][2].get<std::string>()));
        QCOMPARE(p.depth, 32u);
        QCOMPARE(p.mode, 3u);
        QVERIFY(!p.icc.empty());  // (sRGB's)
        // TIFF in RGB: the RGB pages in their values (16 bits; 32 as floats), the grey page from the screen (said)
        const auto tiff = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("tiff")), QStringLiteral("--format"), QStringLiteral("tiff"),
                                                  QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(tiff.exit_code == 0, qPrintable(tiff.described()));
        const Json tiff_reply = genko::test::one_line(tiff.out);
        QCOMPARE(tiff_reply["notes"].dump(),
                 Json::array({"色モード・プロファイルを持つページ 1 ページは、この形式・色ではその値のまま書けないため、画面の色（sRGB）から変換して書き出しました"}).dump());
        auto t = tiff_tags(QString::fromStdString(tiff_reply["files"][1].get<std::string>()));
        QCOMPARE(t[258], 16u);
        QCOMPARE(t[262], 2u);
        QCOMPARE(t[34675], std::uint32_t(p3_bytes.size()));
        t = tiff_tags(QString::fromStdString(tiff_reply["files"][2].get<std::string>()));
        QCOMPARE(t[258], 32u);
        QCOMPARE(t[339], 3u);  // (IEEE floats)
        // TIFF in grey: the grey page's lightness as floats with its profile
        const auto grey_tiff = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("grey")), QStringLiteral("--format"), QStringLiteral("tiff"),
                                                       QStringLiteral("--color"), QStringLiteral("gray"), QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(grey_tiff.exit_code == 0, qPrintable(grey_tiff.described()));
        t = tiff_tags(QString::fromStdString(genko::test::one_line(grey_tiff.out)["files"][0].get<std::string>()));
        QCOMPARE(t[258], 32u);
        QCOMPARE(t[262], 1u);
        QCOMPARE(t[277], 1u);
        QCOMPARE(t[339], 3u);
        QCOMPARE(t[34675], std::uint32_t(sgray_bytes.size()));
        // PDF: 16 bits a channel at most (said)
        const auto pdf = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("pdf")), QStringLiteral("--format"), QStringLiteral("pdf"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(pdf.exit_code == 0, qPrintable(pdf.described()));
        QVERIFY2(genko::test::one_line(pdf.out)["notes"].dump().find("32 bit のページ 1 ページ") != std::string::npos, pdf.out.constData());
        // a feature this build does not know: read-only — nothing applied, the book as it was
        {
            Json saved = project(book);
            saved["features"].push_back("color-model@99");
            genko::test::write_bytes(book + QStringLiteral("/project.json"), saved.dump());
        }
        const Json refused = genko({QStringLiteral("apply"), book,
                                    write_ops(QStringLiteral("more.json"), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})),
                                    QStringLiteral("--agent"), QStringLiteral("human:test")},
                                   &exit);
        QVERIFY(exit != 0);
        QVERIFY2(refused.dump().find("read_only") != std::string::npos && refused.dump().find("color-model@99") != std::string::npos, refused.dump().c_str());
        QCOMPARE(project(book)["pages"][0]["color"]["mode"], Json("gray"));
    }

    // PC2d (document:color-depth-model): a page holding what a page in precise colour refused before — a tone layer, a
    // layer in a folder with its lines, an effect and a colour of its own — made 16 bits through the command line: the
    // op goes through (it was not_yet_ported), saved, undone and done again, drawn and written with nothing left out.
    void preciseLayersThroughTheCommandLine() {
        const QString book = path(QStringLiteral("precise.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 80}, genko::render::Ink{0, 0, 0, 0});
        picture.paste(genko::render::Ink{128, 128, 128, 255}, genko::render::Box{10, 10, 50, 70});
        const Json held = genko(
            {QStringLiteral("apply"), book,
             write_ops(QStringLiteral("held.json"),
                       Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}},
                                    Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pic"}, {"points", Json::array({Json::array({20, 40}), Json::array({150, 120})})},
                                         {"width_mm", 1.5}, {"rgb", Json::array({200, 30, 30})}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"effect", Json{{"border", Json{{"width_mm", 1}, {"rgb", Json::array({20, 60, 200})}}}}},
                                         {"color", Json::array({40, 120, 60})}},
                                    Json{{"op", "group_layers"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"id", "fold"}},
                                    Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"area", Json{{"rect", Json::array({30, 50, 60, 60})}}}}})),
             QStringLiteral("--agent"), QStringLiteral("human:test")},
            &exit);
        QVERIFY2(exit == 0 && held.value("ok", false), held.dump().c_str());
        const Json deep = genko({QStringLiteral("apply"), book,
                                 write_ops(QStringLiteral("deep.json"), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})),
                                 QStringLiteral("--agent"), QStringLiteral("human:test")},
                                &exit);
        QVERIFY2(exit == 0 && deep.value("ok", false), deep.dump().c_str());
        const auto layer_of = [&](const std::string& id) {
            const Json saved = project(book);
            for (const Json& l : saved["pages"][0]["layers"]) if (l.value("id", std::string()) == id) return l;
            return Json();
        };
        QVERIFY2(layer_of("pic").contains("color_raster") && layer_of("pic").value("parent_id", std::string()) == "fold", layer_of("pic").dump().c_str());
        QCOMPARE(project(book)["pages"][0]["color"]["depth"], Json(16));
        // undone and done again
        QVERIFY(genko::test::run_genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}).exit_code == 0);
        QVERIFY(!project(book)["pages"][0].contains("color"));
        QVERIFY(genko::test::run_genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}).exit_code == 0);
        QCOMPARE(project(book)["pages"][0]["color"]["depth"], Json(16));
        // drawn and written, nothing refused
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("60"), QStringLiteral("--out"), path(QStringLiteral("precise.png"))});
        QVERIFY2(drawn.exit_code == 0 && QFile::exists(path(QStringLiteral("precise.png"))), qPrintable(drawn.described()));
        const auto tiff = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("precise-tiff")), QStringLiteral("--format"),
                                                  QStringLiteral("tiff"), QStringLiteral("--dpi"), QStringLiteral("60"), QStringLiteral("--json")});
        QVERIFY2(tiff.exit_code == 0, qPrintable(tiff.described()));
        QCOMPARE(tiff_tags(QString::fromStdString(genko::test::one_line(tiff.out)["files"][0].get<std::string>()))[258], 16u);
    }

    // PC1d (SPEC.md COLOR-03「特別な色モード」): a book's pages made indexed, bitmap, duotone and multichannel through the
    // command line — the pixel layers flattened into one (the lines kept as lines), saved and read again, undone and
    // done again, drawn, asked (--pixel), written (PSD Indexed, Bitmap, Grayscale and Multichannel; TIFF's palette and
    // 1 bit; PDF's /Indexed and 1 bpc; the notes).
    void specialModesThroughTheCommandLine() {
        namespace r = genko::render;
        const QString book = path(QStringLiteral("special.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("4")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // page 1: two paint layers (a grey and a green; a red square over them) and a line; pages 2..4: one paint layer
        r::Image under = r::Image::create("RGBA", r::Size{60, 40}, r::Ink{128, 128, 128, 255});
        under.paste(r::Ink{30, 160, 90, 255}, r::Box{30, 0, 60, 40});
        r::Image over = r::Image::create("RGBA", r::Size{60, 40}, r::Ink{0, 0, 0, 0});
        over.paste(r::Ink{200, 40, 40, 255}, r::Box{10, 10, 20, 20});
        const auto paint = [](int page, const std::string& id, const r::Image& picture) {
            return std::vector<Json>{Json{{"op", "add_layer"}, {"page", page}, {"kind", "paint"}, {"id", id}},
                                     Json{{"op", "set_layer"}, {"page", page}, {"id", id}, {"panel_each", false}, {"panel_clip", false}},
                                     Json{{"op", "put_raster"}, {"page", page}, {"id", id}, {"png_base64", genko::core::b64encode(r::write_png(picture))}}};
        };
        Json ops = Json::array();
        for (const auto& [page, id, picture] : std::vector<std::tuple<int, std::string, r::Image>>{
                 {1, "under", under}, {1, "over", over}, {2, "p2", under}, {3, "p3", under}, {4, "p4", under}}) {
            for (Json& op : paint(page, id, picture)) ops.push_back(std::move(op));
        }
        ops.push_back(Json{{"op", "add_line"}, {"page", 1}, {"id", "t1"}, {"text", "台詞"}, {"x_mm", 60}, {"y_mm", 60}});
        const Json painted = genko({QStringLiteral("apply"), book, write_ops(QStringLiteral("special-paint.json"), ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0 && painted.value("ok", false), painted.dump().c_str());
        const auto pixel_layers = [](const Json& page) {
            int n = 0;
            for (const Json& layer : page["layers"]) n += layer.contains("asset") || layer.contains("ink_raster") || layer.contains("color_raster");
            return n;
        };
        const Json before = project(book);
        QCOMPARE(pixel_layers(before["pages"][0]), 2);
        const std::size_t layers_before = before["pages"][0]["layers"].size();
        const Json modes = genko({QStringLiteral("apply"), book,
                                  write_ops(QStringLiteral("special-modes.json"),
                                            Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}},
                                                         Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "gray"}},
                                                         Json{{"op", "to_bitmap"}, {"page", 2}, {"method", "threshold"}},
                                                         Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "gray"}},
                                                         Json{{"op", "set_duotone"}, {"page", 3}},
                                                         Json{{"op", "set_color_mode"}, {"page", 4}, {"mode", "multichannel"}}})),
                                  QStringLiteral("--agent"), QStringLiteral("human:test")},
                                 &exit);
        QVERIFY2(exit == 0 && modes.value("ok", false), modes.dump().c_str());
        const auto check_modes = [&](const Json& saved) {
            QVERIFY2(saved["features"] == Json::array({"color-mode@1", "color-model@1"}), saved["features"].dump().c_str());
            const Json& indexed = saved["pages"][0]["color"];
            QCOMPARE(indexed["mode"], Json("indexed"));
            QVERIFY2(indexed["table"]["colors"].size() >= 4 && indexed["table"]["colors"].size() <= 8, indexed.dump().c_str());
            QCOMPARE(saved["pages"][1]["color"]["mode"], Json("bitmap"));
            QCOMPARE(saved["pages"][1]["color"]["bitmap"]["method"], Json("threshold"));
            QCOMPARE(saved["pages"][2]["color"]["mode"], Json("duotone"));
            QCOMPARE(saved["pages"][2]["color"]["duotone"].size(), std::size_t(2));
            QCOMPARE(saved["pages"][3]["color"]["mode"], Json("multichannel"));
            QCOMPARE(saved["pages"][3]["color"]["from"], Json("rgb"));
            QCOMPARE(saved["pages"][3]["color"]["plates"].size(), std::size_t(3));
            // (page 1 flattened: one layer of pixels, the two paint layers gone, its line a line)
            QCOMPARE(pixel_layers(saved["pages"][0]), 1);
            QCOMPARE(saved["pages"][0]["layers"].size(), layers_before - 2);
            QCOMPARE(saved["story"].size(), std::size_t(1));
            QCOMPARE(saved["story"][0]["id"], Json("t1"));
        };
        check_modes(project(book));
        // undone (the pages as they were: no modes, the two layers again) and done again
        const Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        {
            const Json saved = project(book);
            for (const Json& page : saved["pages"]) QVERIFY(!page.contains("color"));
            QCOMPARE(pixel_layers(saved["pages"][0]), 2);
            QCOMPARE(saved["pages"][0]["layers"].size(), layers_before);
        }
        const Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        check_modes(project(book));
        // drawn: the bitmap page black and white only, the indexed page in its table's colours only
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("2"), QStringLiteral("--dpi"),
                                                   QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("bitmap.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
        const std::string bits = r::open_image(genko::test::read_bytes(path(QStringLiteral("bitmap.png"))), r::kPillowOpenLimits).convert("RGB").tobytes();
        for (const char c : bits) QVERIFY(static_cast<unsigned char>(c) == 0 || static_cast<unsigned char>(c) == 255);
        const auto drawn_indexed = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                           QStringLiteral("40"), QStringLiteral("--out"), path(QStringLiteral("indexed.png"))});
        QVERIFY2(drawn_indexed.exit_code == 0, qPrintable(drawn_indexed.described()));
        {
            std::set<std::array<int, 3>> table;
            const Json saved = project(book);  // (kept: a range over a temporary's part would outlive it)
            for (const Json& c : saved["pages"][0]["color"]["table"]["colors"]) table.insert({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
            const std::string px = r::open_image(genko::test::read_bytes(path(QStringLiteral("indexed.png"))), r::kPillowOpenLimits).convert("RGB").tobytes();
            for (std::size_t i = 0; i + 2 < px.size(); i += 3) {
                const std::array<int, 3> c{static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i + 1]), static_cast<unsigned char>(px[i + 2])};
                QVERIFY2(table.contains(c), qPrintable(QStringLiteral("(%1, %2, %3) at %4 not in %5")
                                                           .arg(c[0]).arg(c[1]).arg(c[2]).arg(i / 3)
                                                           .arg(QString::fromStdString(saved["pages"][0]["color"].dump()))));
            }
        }
        // asked
        const Json at_indexed = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("1:150,150"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0 && at_indexed["mode"] == Json("indexed") && at_indexed["index"].is_number_integer(), at_indexed.dump().c_str());
        const Json at_bitmap = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("2:150,150"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0 && at_bitmap["mode"] == Json("bitmap") && at_bitmap["values"].size() == 1, at_bitmap.dump().c_str());
        const Json at_duotone = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("3:150,150"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0 && at_duotone["mode"] == Json("duotone"), at_duotone.dump().c_str());
        const Json at_plates = genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("4:150,150"), QStringLiteral("--dpi"), QStringLiteral("40")}, &exit);
        QVERIFY2(exit == 0 && at_plates["mode"] == Json("multichannel") && at_plates["values"].size() == 3, at_plates.dump().c_str());
        // written: PSD Indexed (2), Bitmap (0, 1 bit), Grayscale (1: the duotone's grey) and Multichannel (7, three plates)
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("special-psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const Json psd_reply = genko::test::one_line(psd.out);
        QCOMPARE(psd_reply["count"], Json(4));
        const std::array<std::array<unsigned, 3>, 4> want{{{1, 8, 2}, {1, 1, 0}, {1, 8, 1}, {3, 8, 7}}};
        for (std::size_t k = 0; k < 4; ++k) {
            const Psd p = read_psd(QString::fromStdString(psd_reply["files"][k].get<std::string>()));
            QVERIFY2(p.channels == want[k][0] && p.depth == want[k][1] && p.mode == want[k][2],
                     qPrintable(QStringLiteral("page %1: %2 %3 %4").arg(k + 1).arg(p.channels).arg(p.depth).arg(p.mode)));
        }
        QCOMPARE(psd_reply["notes"].dump(),
                 Json::array({"ダブルトーンのページ 1 ページは、この形式ではダブルトーンのインキのまま書けないため、グレーの値で書き出しました（インキの色と曲線は書いていません）",
                              "マルチチャンネルのページ 1 ページは、PSD ではレイヤーを持てないため、版を統合した画像だけを書き出しました",
                              "2 階調・インデックスカラーのページ 2 ページは、PSD ではレイヤーを持てないため、統合した画像だけを書き出しました"})
                     .dump());
        // TIFF in RGB (the command line's colour): the indexed page as its palette; in grey: the bitmap page in 1 bit
        const auto tiff = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("special-tiff")), QStringLiteral("--format"), QStringLiteral("tiff"),
                                                  QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(tiff.exit_code == 0, qPrintable(tiff.described()));
        const Json tiff_reply = genko::test::one_line(tiff.out);
        auto t = tiff_tags(QString::fromStdString(tiff_reply["files"][0].get<std::string>()));
        QCOMPARE(t[262], 3u);  // (Photometric: palette)
        QCOMPARE(t[258], 8u);
        QVERIFY(t.contains(320));  // (its ColorMap)
        QCOMPARE(tiff_reply["notes"].dump(),
                 Json::array({"色モード・プロファイルを持つページ 1 ページは、この形式・色ではその値のまま書けないため、画面の色（sRGB）から変換して書き出しました",
                              "ダブルトーンのページ 1 ページは、この形式・色ではダブルトーンのインキのまま書けないため、刷った見え方（画面の色、sRGB）で書き出しました",
                              "マルチチャンネルのページ 1 ページは、この形式では版のまま書けないため、刷った見え方（画面の色、sRGB）で書き出しました（PSD なら版のまま書けます）"})
                     .dump());
        const auto grey_tiff = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("special-grey")), QStringLiteral("--format"), QStringLiteral("tiff"),
                                                       QStringLiteral("--color"), QStringLiteral("gray"), QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(grey_tiff.exit_code == 0, qPrintable(grey_tiff.described()));
        const Json grey_reply = genko::test::one_line(grey_tiff.out);
        t = tiff_tags(QString::fromStdString(grey_reply["files"][1].get<std::string>()));
        QCOMPARE(t[258], 1u);  // (1 bit)
        QVERIFY2(grey_reply["notes"].dump().find("グレーの値で書き出しました") != std::string::npos, grey_reply.dump().c_str());
        // PDF: the indexed page /Indexed in RGB, the bitmap page 1 bit a component in grey
        const auto pdf = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("special-pdf")), QStringLiteral("--format"), QStringLiteral("pdf"),
                                                 QStringLiteral("--dpi"), QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(pdf.exit_code == 0, qPrintable(pdf.described()));
        const std::string pdf_bytes = genko::test::read_bytes(QString::fromStdString(genko::test::one_line(pdf.out)["files"][0].get<std::string>()));
        QVERIFY(pdf_bytes.find("/Indexed /DeviceRGB") != std::string::npos);
        QVERIFY(pdf_bytes.find("/BitsPerComponent 1") == std::string::npos);
        const auto grey_pdf = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("special-grey-pdf")), QStringLiteral("--format"),
                                                      QStringLiteral("pdf"), QStringLiteral("--color"), QStringLiteral("gray"), QStringLiteral("--dpi"),
                                                      QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(grey_pdf.exit_code == 0, qPrintable(grey_pdf.described()));
        const std::string grey_pdf_bytes =
            genko::test::read_bytes(QString::fromStdString(genko::test::one_line(grey_pdf.out)["files"][0].get<std::string>()));
        QVERIFY(grey_pdf_bytes.find("/BitsPerComponent 1") != std::string::npos);
        QVERIFY(grey_pdf_bytes.find("/Indexed") == std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestContractColorModel)
#include "test_contract_color_model.moc"
