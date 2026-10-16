// channels@1 (SPEC.md COLOR-03, schema-v4.md「チャンネル（channels@1）」) through the command line, as a person or an agent
// uses it: a book made, an alpha and a spot channel added to a page (apply), saved and read again (the feature, the
// channels and their rasters as assets), kept by the asset collection, undone and done again, a selection made from a
// channel, a fill on the alpha channel alone, drawn (render: the spot printed), listed (inspect), written (export: the
// PSD's extra channels with their names and settings read back by this build's PSD reader, the PDF's separation, the
// notes of the other formats); a book with a channels feature this build does not know is read-only. (Python has no
// channels: this build's own contract.)

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>
#include <string>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "export_files.hpp"
#include "render/psd.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

unsigned be16(const std::string& b, std::size_t at) { return unsigned(static_cast<unsigned char>(b[at])) << 8 | static_cast<unsigned char>(b[at + 1]); }

// The ids of a PSD's image resources, in order.
std::vector<unsigned> psd_resources(const std::string& b) {
    std::vector<unsigned> out;
    std::size_t at = 26;
    const auto be32 = [&](std::size_t p) { return std::uint32_t(be16(b, p)) << 16 | be16(b, p + 2); };
    at += 4 + be32(at);
    const std::size_t end = at + 4 + be32(at);
    for (std::size_t r = at + 4; r + 12 <= end;) {
        out.push_back(be16(b, r + 4));
        const std::size_t data = r + 6 + ((static_cast<unsigned char>(b[r + 6]) + 2) & ~std::size_t(1));
        const std::size_t size = be32(data);
        r = data + 4 + ((size + 1) & ~std::size_t(1));
    }
    return out;
}

}  // namespace

class TestContractChannels : public QObject {
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
    Json apply(const QString& book, const QString& name, const Json& ops) {
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    bool asset(const QString& book, const std::string& ref, const char* suffix) {
        return QFile::exists(book + QStringLiteral("/assets/") + QString::fromStdString(ref.substr(7, 2)) + QStringLiteral("/") +
                             QString::fromStdString(ref.substr(7)) + QString::fromLatin1(suffix));
    }

private slots:
    void throughTheCommandLine() {
        const QString book = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        // a paint layer of one colour over page 1
        const genko::render::Image picture = genko::render::Image::create("RGBA", genko::render::Size{60, 40}, genko::render::Ink{200, 150, 100, 255});
        Json done = apply(book, QStringLiteral("paint.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"title", "絵"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture))}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // an alpha channel from an area (the left half), a spot channel (the top half)
        done = apply(book, QStringLiteral("channels.json"),
                     Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"name", "空"}, {"fill", "area"}, {"area", Json{{"rect", Json::array({0, 0, 105, 297})}}}},
                                  Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "金"}, {"color", Json::array({200, 160, 40})}, {"solidity", 0.5},
                                       {"area", Json{{"rect", Json::array({0, 0, 210, 148})}}}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(done["results"].size(), std::size_t{2});
        const std::string alpha_id = done["results"][0]["channel"].get<std::string>();
        Json saved = project(book);
        QCOMPARE(saved["features"], Json::array({"channels@1"}));
        const Json channels = saved["pages"][0]["channels"];
        QCOMPARE(channels.size(), std::size_t{2});
        QCOMPARE(channels[0]["id"], Json(alpha_id));
        QCOMPARE(channels[0]["kind"], Json("alpha"));
        QCOMPARE(channels[1]["kind"], Json("spot"));
        QCOMPARE(channels[1]["solidity"], Json(0.5));
        const std::string alpha_raster = channels[0]["raster"].get<std::string>();
        const std::string spot_raster = channels[1]["raster"].get<std::string>();
        QVERIFY(asset(book, alpha_raster, ".gkir") && asset(book, spot_raster, ".gkir"));
        QVERIFY(!saved["pages"][1].contains("channels"));
        // the asset collection keeps them
        const Json collected = genko({QStringLiteral("gc"), book}, &exit);
        QVERIFY2(exit == 0, collected.dump().c_str());
        QVERIFY(asset(book, alpha_raster, ".gkir") && asset(book, spot_raster, ".gkir"));
        // undone and done again
        Json undone = genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        saved = project(book);
        QVERIFY(!saved["pages"][0].contains("channels"));
        QVERIFY(!saved.contains("features") || saved["features"].empty());
        QVERIFY(asset(book, alpha_raster, ".gkir"));  // (kept for the redo)
        Json redone = genko({QStringLiteral("redo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        QCOMPARE(project(book)["pages"][0]["channels"], channels);
        // a fill on the alpha channel alone (the layer as it was), a selection from it
        done = apply(book, QStringLiteral("fill.json"),
                     Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pic"}, {"channel", alpha_id},
                                       {"area", Json{{"rect", Json::array({0, 0, 210, 30})}}}, {"rgb", Json::array({255, 255, 255})}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        saved = project(book);
        QVERIFY(saved["pages"][0]["channels"][0]["raster"] != Json(alpha_raster));
        done = apply(book, QStringLiteral("select.json"),
                     Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"name", "反転"}, {"fill", "area"}, {"area", Json{{"channel", "空"}, {"invert", true}}}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        // listed
        const Json inspected = genko({QStringLiteral("inspect"), book}, &exit);
        QVERIFY2(exit == 0, inspected.dump().c_str());
        const Json listed = inspected["pages"][0]["channels"];
        QCOMPARE(listed.size(), std::size_t{3});
        QCOMPARE(listed[1]["name"], Json("金"));
        QVERIFY(!listed[0].contains("raster"));
        // drawn: the spot printed over the top half
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("20"), QStringLiteral("--out"), path(QStringLiteral("p1.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
        const genko::render::Image shown = genko::render::open_image(genko::test::read_bytes(path(QStringLiteral("p1.png")))).convert("RGB");
        const std::vector<double> top = shown.getpixel(120, 40), bottom = shown.getpixel(120, 200);
        QCOMPARE(bottom, (std::vector<double>{200, 150, 100}));
        QVERIFY2(top != bottom, "the spot is printed");
        // written: PSD with two… three extra channels, read back
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("psd")), QStringLiteral("--format"), QStringLiteral("psd"),
                                                 QStringLiteral("--dpi"), QStringLiteral("20"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const Json psd_reply = genko::test::one_line(psd.out);
        QVERIFY2(!psd_reply.contains("notes"), psd_reply.dump().c_str());
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(psd_reply["files"][0].get<std::string>()));
        QCOMPARE(be16(bytes, 12), 6u);  // (RGB and three channels)
        const std::vector<unsigned> ids = psd_resources(bytes);
        for (const unsigned id : {1006u, 1045u, 1077u}) QVERIFY2(std::find(ids.begin(), ids.end(), id) != ids.end(), qPrintable(QString::number(id)));
        const genko::render::psd::File file = genko::render::psd::File::read(bytes);
        const auto extra = file.extra_channels();
        QCOMPARE(extra.size(), std::size_t{3});
        QCOMPARE(extra[0].name, std::string("空"));
        QCOMPARE(extra[0].kind, 1);  // (masked areas)
        QCOMPARE(extra[0].opacity, 50);
        QCOMPARE(extra[0].color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(extra[1].name, std::string("金"));
        QCOMPARE(extra[1].kind, 2);  // (spot)
        QCOMPARE(extra[1].opacity, 50);
        QCOMPARE(extra[1].color, (std::array<int, 3>{200, 160, 40}));
        QCOMPARE(extra[2].name, std::string("反転"));
        const int w = static_cast<int>(file.width());
        const auto at = [&](const std::string& samples, int x, int y) { return static_cast<unsigned char>(samples[std::size_t(y) * w + x]); };
        QCOMPARE(at(extra[0].samples, 10, 200), 255);
        QCOMPARE(at(extra[0].samples, 120, 200), 0);
        QCOMPARE(at(extra[1].samples, 120, 40), 255);
        QCOMPARE(at(extra[1].samples, 120, 200), 0);
        QCOMPARE(at(extra[2].samples, 120, 200), 255);
        // the PSD's merged picture without the spot (a channel of its own)
        const auto merged = file.merged();
        QVERIFY(merged.has_value());
        QCOMPARE(merged->getpixel(120, 40), (std::vector<double>{200, 150, 100, 255}));
        // PDF: the spot as a separation, overprinting, multiplied over the picture where a viewer does not overprint, its
        // soft mask clear where there is no ink (so the page shows through); a spot named as PDF's own colorants (Cyan)
        // written under another name (said); the alpha channels left out (said)
        done = apply(book, QStringLiteral("cyan.json"),
                     Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "Cyan"}, {"color", Json::array({0, 160, 233})},
                                       {"area", Json{{"rect", Json::array({0, 250, 50, 47})}}}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const auto pdf = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("pdf")), QStringLiteral("--format"), QStringLiteral("pdf"),
                                                 QStringLiteral("--dpi"), QStringLiteral("20"), QStringLiteral("--json")});
        QVERIFY2(pdf.exit_code == 0, qPrintable(pdf.described()));
        const Json pdf_reply = genko::test::one_line(pdf.out);
        QCOMPARE(pdf_reply["notes"].dump(),
                 Json::array({"アルファチャンネルのあるページ 1 ページは、この形式ではアルファチャンネルを書けないため、書き出していません（PSD なら書けます）",
                              "特色チャンネルの名前 Cyan は PDF では決まった版（プロセスカラー・All・None）の名前のため、Cyan Spot として書き出しました"})
                     .dump());
        const std::string pdf_bytes = genko::test::read_bytes(QString::fromStdString(pdf_reply["files"][0].get<std::string>()));
        const std::size_t gold = pdf_bytes.find("/Separation /#E9#87#91 /DeviceRGB << /FunctionType 2 /Domain [0 1] /C0 [1 1 1] /C1 [0.784 0.627 0.157] /N 1 >>");
        QVERIFY(gold != std::string::npos);
        QVERIFY(pdf_bytes.find("/Separation /Cyan#20Spot /DeviceRGB") != std::string::npos);
        QVERIFY(pdf_bytes.find("/Separation /Cyan /") == std::string::npos);
        QVERIFY(pdf_bytes.find("<< /Type /ExtGState /OP true /op true /OPM 1 /BM /Multiply >>") != std::string::npos);
        QVERIFY(pdf_bytes.find("/GS0 gs") != std::string::npos);
        // (the gold separation's soft mask: 0 where the channel is 0 — the bottom —, full where it has ink — the top)
        const std::size_t dict_end = pdf_bytes.find(">>", pdf_bytes.find("/N 1 >>]", gold) + 8);
        const std::string gold_dict = pdf_bytes.substr(gold, dict_end - gold);
        const std::size_t smask_at = gold_dict.find("/SMask ");
        QVERIFY2(smask_at != std::string::npos, gold_dict.c_str());
        const std::string smask_ref = gold_dict.substr(smask_at + 7, gold_dict.find(" 0 R", smask_at) - smask_at - 7);
        const std::size_t smask_obj = pdf_bytes.find("\n" + smask_ref + " 0 obj\n");
        QVERIFY(smask_obj != std::string::npos);
        const std::size_t smask_dict = pdf_bytes.find("<<", smask_obj);
        const std::string smask_head = pdf_bytes.substr(smask_dict, pdf_bytes.find("stream\n", smask_dict) - smask_dict);
        QVERIFY2(smask_head.find("/Subtype /Image") != std::string::npos && smask_head.find("/ColorSpace /DeviceGray") != std::string::npos &&
                     smask_head.find("/BitsPerComponent 8") != std::string::npos,
                 smask_head.c_str());
        const auto number_after = [&](const std::string& text, const std::string& key) {
            const std::size_t found = text.find(key);
            return found == std::string::npos ? -1 : std::stoi(text.substr(found + key.size()));
        };
        const int mw = number_after(smask_head, "/Width "), mh = number_after(smask_head, "/Height "), mlen = number_after(smask_head, "/Length ");
        const std::size_t data_at = pdf_bytes.find("stream\n", smask_dict) + 7;
        const std::string mask = genko::test::exports::inflated(std::string_view(pdf_bytes).substr(data_at, std::size_t(mlen)));
        QCOMPARE(mask.size(), std::size_t(mw) * std::size_t(mh));
        QCOMPARE(static_cast<unsigned char>(mask[std::size_t(mh / 8) * mw + mw / 2]), 255);
        QCOMPARE(static_cast<unsigned char>(mask[std::size_t(mh - mh / 8) * mw + mw / 2]), 0);
        // PNG: the spot printed as shown, the alpha channels left out (said)
        const auto png = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("png")), QStringLiteral("--format"), QStringLiteral("png"),
                                                 QStringLiteral("--dpi"), QStringLiteral("20"), QStringLiteral("--json")});
        QVERIFY2(png.exit_code == 0, qPrintable(png.described()));
        const Json png_notes = genko::test::one_line(png.out)["notes"];
        QCOMPARE(png_notes.size(), std::size_t{2});
        QVERIFY(png_notes[0].get<std::string>().starts_with("特色チャンネルのあるページ 1 ページは、この形式では特色を版として書けないため"));
        // a channels feature this build does not know: read-only
        {
            Json raw = project(book);
            raw["features"].push_back("channels@99");
            genko::test::write_bytes(book + QStringLiteral("/project.json"), raw.dump());
        }
        const Json refused = genko({QStringLiteral("apply"), book,
                                    write_ops(QStringLiteral("more.json"), Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "空"}}})),
                                    QStringLiteral("--agent"), QStringLiteral("human:test")},
                                   &exit);
        QVERIFY(exit != 0);
        QVERIFY2(refused.dump().find("read_only") != std::string::npos && refused.dump().find("channels@99") != std::string::npos, refused.dump().c_str());
        QCOMPARE(project(book)["pages"][0]["channels"].size(), std::size_t{4});  // (空, 金, 反転, Cyan)
    }
};

QTEST_GUILESS_MAIN(TestContractChannels)
#include "test_contract_channels.moc"
