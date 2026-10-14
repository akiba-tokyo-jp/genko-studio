// color-mode@1 (SPEC.md COLOR-02) against the reference Python (tools/migration/ink_check.py): the conversions as
// Pillow's ImageCms makes them (sRGB → CMYK and back through a CMYK profile, relative colorimetric with black point
// compensation; Pillow's LAB → RGB), the files of CMYK and Lab pages as Pillow, psd-tools and poppler read them (TIFF,
// PSD, PDF: their colour, depth, profile and inks), and a Lab PSD on an RGB page as Pillow opens it. Little CMS may be
// another version on each side: a sample may differ by one.

#include <QtTest>

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/psd_text.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

QString icc_path() { return genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")); }
std::string icc_bytes() { return genko::test::read_bytes(icc_path()); }

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(100);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("ink", core::Num(1), 1, spec, core::Binding::Right);
}

core::PageColor cmyk_mode(int depth = 8) {
    core::PageColor c;
    c.space = core::InkSpace::Cmyk;
    c.depth = depth;
    c.icc = std::make_shared<const std::string>(icc_bytes());
    c.profile = core::bytes_ref(*c.icc);
    return c;
}

bool near(const Json& a, const std::vector<int>& b, int tolerance = 1) {
    if (!a.is_array() || a.size() != b.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (std::abs(a[i].get<int>() - b[i]) > tolerance) return false;
    }
    return true;
}

}  // namespace

class TestContractInk : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json check(const Json& request) {
        {
            QFile file(path(QStringLiteral("request.json")));
            if (!file.open(QIODevice::WriteOnly)) return Json();
            file.write(QByteArray::fromStdString(request.dump()));
        }
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/ink_check.py"), path(QStringLiteral("request.json")),
                                           path(QStringLiteral("answer.json"))},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) qWarning("%s", qPrintable(ran.described()));
        return ran.exit_code == 0 ? genko::test::read_json(path(QStringLiteral("answer.json"))) : Json();
    }

    // A CMYK page with a black line, a red one from the table of inks and a picture (a paint layer of inks).
    core::Document cmyk_book(int depth) {
        core::Document doc = small_book();
        std::string ink;
        for (const auto& layer : doc.page(0).layers) {
            if (layer.role == core::LayerRole::Ink) ink = layer.id;
        }
        render::Image picture = render::Image::create("RGBA", render::Size{60, 80}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{30, 160, 90, 255}, render::Box{0, 50, 60, 80});
        const Json ops = Json::array(
            {Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({10, 10}), Json::array({50, 40})})}, {"width_mm", 3}},
             Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({10, 45}), Json::array({50, 45})})}, {"width_mm", 3}, {"rgb", Json::array({200, 30, 30})}},
             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
             Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", depth}},
             Json{{"op", "set_inks"}, {"inks", Json::array({Json::array({Json::array({200, 30, 30}), Json::array({0.1, 0.9, 0.8, 0.0})})})}},
             Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}});
        return run_ops(doc, ops).doc;
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
    }

    // sRGB → CMYK and CMYK → sRGB through the profile, and LAB → RGB, as Pillow's ImageCms.
    void conversionsAsPillow() {
        Json rgb = Json::array(), cmyk = Json::array(), lab = Json::array();
        for (int r = 0; r <= 255; r += 51)
            for (int g = 0; g <= 255; g += 51)
                for (int b = 0; b <= 255; b += 51) rgb.push_back(Json::array({r, g, b}));
        for (int v = 0; v <= 255; v += 17) rgb.push_back(Json::array({v, v, v}));
        for (int c = 0; c <= 255; c += 85)
            for (int m = 0; m <= 255; m += 85)
                for (int y = 0; y <= 255; y += 85)
                    for (int k = 0; k <= 255; k += 85) cmyk.push_back(Json::array({c, m, y, k}));
        for (int l = 0; l <= 255; l += 51)
            for (int a = 28; a <= 228; a += 50)
                for (int b = 28; b <= 228; b += 50) lab.push_back(Json::array({l, a, b}));
        const Json answer = check(Json{{"convert", Json{{"icc", icc_path().toStdString()}, {"rgb", rgb}, {"cmyk", cmyk}, {"lab", lab}}}});
        QVERIFY(answer.contains("convert"));
        const Json& pillow = answer["convert"];
        // ours: the same rows as pictures
        render::Image in = render::Image::create("RGBA", render::Size{static_cast<int>(rgb.size()), 1}, render::Ink{0, 0, 0, 255});
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            in.paste(render::Ink{rgb[i][0].get<int>(), rgb[i][1].get<int>(), rgb[i][2].get<int>(), 255}, render::Box{int(i), 0, int(i) + 1, 1});
        }
        const core::InkPixels ours = render::ink::from_rgb(in, cmyk_mode());
        int worst = 0;
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            for (unsigned c = 0; c < 4; ++c) worst = std::max(worst, std::abs(int(ours.samples[i * 5 + c]) - pillow["cmyk"][i][c].get<int>()));
        }
        QVERIFY2(worst <= 1, qPrintable(QStringLiteral("sRGB → CMYK differs by %1").arg(worst)));
        core::InkPixels inks = core::blank_ink_pixels(core::InkSpace::Cmyk, 8, static_cast<std::uint32_t>(cmyk.size()), 1);
        for (std::size_t i = 0; i < cmyk.size(); ++i) {
            for (unsigned c = 0; c < 4; ++c) inks.samples[i * 5 + c] = static_cast<std::uint16_t>(cmyk[i][c].get<int>());
            inks.samples[i * 5 + 4] = 255;
        }
        const std::string shown = render::ink::to_rgba8(inks, cmyk_mode()).tobytes();
        worst = 0;
        for (std::size_t i = 0; i < cmyk.size(); ++i) {
            for (unsigned c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(static_cast<unsigned char>(shown[i * 4 + c])) - pillow["srgb"][i][c].get<int>()));
        }
        QVERIFY2(worst <= 1, qPrintable(QStringLiteral("CMYK → sRGB differs by %1").arg(worst)));
        std::string lab_bytes;
        // (Pillow's "LAB" raw mode: a and b signed — the bytes XOR 128 — as putdata's values are not)
        for (const Json& v : lab) lab_bytes += std::string{char(v[0].get<int>()), char(v[1].get<int>() ^ 128), char(v[2].get<int>() ^ 128)};
        const render::Image lab_image = render::Image::frombytes("LAB", render::Size{static_cast<int>(lab.size()), 1}, lab_bytes);
        const std::string lab_rgb = lab_image.convert("RGB").tobytes();
        worst = 0;
        QString where;
        for (std::size_t i = 0; i < lab.size(); ++i) {
            for (unsigned c = 0; c < 3; ++c) {
                const int d = std::abs(int(static_cast<unsigned char>(lab_rgb[i * 3 + c])) - pillow["lab_rgb"][i][c].get<int>());
                if (d > worst) {
                    worst = d;
                    where = QString::fromStdString(lab[i].dump() + " Pillow " + pillow["lab_rgb"][i].dump() + " ours ") +
                            QString::number(static_cast<unsigned char>(lab_rgb[i * 3])) + QStringLiteral(",") + QString::number(static_cast<unsigned char>(lab_rgb[i * 3 + 1])) +
                            QStringLiteral(",") + QString::number(static_cast<unsigned char>(lab_rgb[i * 3 + 2]));
                }
            }
        }
        QVERIFY2(worst <= 1, qPrintable(QStringLiteral("LAB → RGB differs by %1 (%2)").arg(worst).arg(where)));
    }

    // (群D) The editable PSD of a CMYK and a Lab page as psd-tools reads it: its text layer (in its group, whose records
    // carry the page's channels), a CMYK page's text in Genko's black FillColor Type 2 on K alone and its white outline
    // CMYC with no ink; a Lab page's text RGB, its outline LbCl (L 100).
    void editableTextKeepsTheInks() {
        const fs::path out = core::path_from_utf8(path(QStringLiteral("editable")).toStdString());
        for (const std::string mode : {"cmyk", "lab"}) {
            const Json to = mode == "cmyk" ? Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}}
                                           : Json{{"op", "set_color_mode"}, {"mode", "lab"}};
            const core::Document doc =
                run_ops(small_book(), Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "t1"}, {"text", "ドン"}, {"x_mm", 5}, {"y_mm", 5},
                                                        {"w_mm", 50}, {"h_mm", 20}, {"wrap", "horizontal"}, {"balloon", "sfx"},
                                                        {"style", Json{{"outline_mm", 1}, {"rgb", Json::array({20, 20, 20})}, {"outline_rgb", Json::array({255, 255, 255})}}}},
                                                   to}))
                    .doc;
            formats::PsdTextReport report;
            const auto psds = formats::export_psd_pages(doc, out / mode, 100, formats::PsdText{true, false}, &report);
            const std::string psd = core::path_to_utf8(psds.at(0));
            const Json answer = check(Json{{"read", Json{{"files", Json::array({psd})}, {"points", Json::array()}}}});
            const Json& texts = answer["read"][psd]["psd"]["text"];
            const Json* body = nullptr;
            for (const Json& t : texts) {
                if (t["name"] == "本文 #t1") body = &t;
            }
            QVERIFY2(body != nullptr, texts.dump().c_str());
            QCOMPARE((*body)["fill"]["type"].get<int>(), mode == "cmyk" ? 2 : 1);
            const std::vector<double> fill = (*body)["fill"]["values"].get<std::vector<double>>();
            if (mode == "cmyk") {
                QVERIFY2((fill == std::vector<double>{1, 0, 0, 0, 1}), body->dump().c_str());
                QCOMPARE((*body)["stroke"]["class"].get<std::string>(), std::string("CMYC"));
                for (const char* key : {"Cyn ", "Mgnt", "Ylw ", "Blck"}) QCOMPARE((*body)["stroke"]["values"][key].get<double>(), 0.0);
            } else {
                QVERIFY2(fill.size() == 4 && std::abs(fill[1] - 20 / 255.0) < 1e-6, body->dump().c_str());
                QCOMPARE((*body)["stroke"]["class"].get<std::string>(), std::string("LbCl"));
                QVERIFY(std::abs((*body)["stroke"]["values"]["Lmnc"].get<double>() - 100.0) < 1e-6);
            }
        }
    }

    // A CMYK page's TIFF, PSD and PDF keep its inks (8 and 16 bits) with its profile; a Lab page's are Lab.
    void filesKeepTheInks() {
        const fs::path out = core::path_from_utf8(path(QStringLiteral("out")).toStdString());
        const int dpi = 100;
        const auto px = [&](double mm) { return render::mm_to_px(mm, dpi); };
        const Json points = Json::array({Json::array({px(30), px(25)}), Json::array({px(30), px(45)}), Json::array({px(55), px(5)}), Json::array({px(30), px(70)})});
        for (const int depth : {8, 16}) {
            const core::Document doc = cmyk_book(depth);
            const auto tiffs = formats::export_print(doc, out / ("tiff" + std::to_string(depth)), "tiff", dpi, 180, false, "paper", "auto", "", std::nullopt);
            const auto psds = formats::export_psd_pages(doc, out / ("psd" + std::to_string(depth)), dpi);
            const auto pdfs = formats::export_print(doc, out / ("pdf" + std::to_string(depth)), "pdf", dpi, 180, false, "paper", "auto", "", std::nullopt);
            const std::string tiff = core::path_to_utf8(tiffs.at(0)), psd = core::path_to_utf8(psds.at(0));
            const Json answer = check(Json{{"read", Json{{"files", Json::array({tiff, psd})}, {"points", points}}}});
            const Json& t = answer["read"][tiff];
            QCOMPARE(QString::fromStdString(t["mode"].get<std::string>()), QStringLiteral("CMYK"));
            QCOMPARE(t["icc"].get<std::size_t>(), icc_bytes().size());
            QCOMPARE(t["tiff"]["photometric"].get<int>(), 5);
            QCOMPARE(t["tiff"]["bits"][0].get<int>(), depth);
            QCOMPARE(t["tiff"]["inkset"].get<int>(), 1);
            QVERIFY2(near(t["pixels"][0], {0, 0, 0, 255}, 0), t["pixels"][0].dump().c_str());   // (the black line: K alone)
            // (the table's ink, 0.1, 0.9, 0.8, 0: Pillow reads a 16-bit CMYK TIFF's high bytes — 6554 is 25)
            const std::vector<int> red = depth == 8 ? std::vector<int>{26, 230, 204, 0} : std::vector<int>{6554 >> 8, 58982 >> 8, 52428 >> 8, 0};
            QVERIFY2(near(t["pixels"][1], red, 0), t["pixels"][1].dump().c_str());
            QVERIFY2(near(t["pixels"][2], {0, 0, 0, 0}, 0), t["pixels"][2].dump().c_str());       // (the paper: no ink)
            const Json& p = answer["read"][psd];
            QCOMPARE(p["psd"]["mode"].get<int>(), 4);
            QCOMPARE(p["psd"]["depth"].get<int>(), depth);
            QCOMPARE(p["psd"]["icc"].get<std::size_t>(), icc_bytes().size());
            QVERIFY(p["psd"]["layers"].dump().find("絵") != std::string::npos);
            // (psd-tools: the stored values, the inks inverted — the black line K alone, the table's ink)
            const int most = depth == 16 ? 65535 : 255;
            QVERIFY2(near(p["psd"]["values"][0], {most, most, most, 0}, 0), p["psd"]["values"][0].dump().c_str());
            const std::vector<int> table = depth == 16 ? std::vector<int>{6554, 58982, 52428, 0} : std::vector<int>{26, 230, 204, 0};
            QVERIFY2(near(p["psd"]["values"][1], {most - table[0], most - table[1], most - table[2], most - table[3]}, 0),
                     p["psd"]["values"][1].dump().c_str());
            if (depth == 8) {
                QCOMPARE(p["icc"].get<std::size_t>(), icc_bytes().size());
                QVERIFY2(near(p["pixels"][0], {0, 0, 0, 255}, 0), p["pixels"][0].dump().c_str());  // (Pillow: the inks)
            }
            // the picture in the file is the paint layer's own inks
            const auto layer_inks = core::decode_ink_raster(*doc.page(0).layers.back().ink_raster);
            const auto at = [&](int x, int y) { return layer_inks.samples.data() + (std::size_t(y) * layer_inks.width + x) * 5; };
            const auto* green = at(30, 70);  // (the paint layer is the page's size at 1 px/mm here: 60 × 80)
            const double scale = depth == 16 ? 257.0 : 1.0;
            QVERIFY2(near(t["pixels"][3], {int(std::lround(green[0] / scale)), int(std::lround(green[1] / scale)), int(std::lround(green[2] / scale)),
                                           int(std::lround(green[3] / scale))}, 1),
                     t["pixels"][3].dump().c_str());
            // the PDF: one image, ICC-based (the profile's four inks), at the page's depth
            QProcess list;
            list.start(QStringLiteral("pdfimages"), {QStringLiteral("-list"), QString::fromStdString(core::path_to_utf8(pdfs.at(0)))});
            QVERIFY(list.waitForFinished(60000));
            const QString text = QString::fromUtf8(list.readAllStandardOutput());
            QVERIFY2(text.contains(QStringLiteral(" icc ")) && text.contains(QStringLiteral(" 4 ")) &&
                         text.contains(QStringLiteral(" %1 ").arg(depth)),
                     qPrintable(text));
        }
        // a Lab page: TIFF in CIELab (8 bits: Pillow reads it), PSD mode 9, PDF /Lab
        core::Document lab = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}}})).doc;
        const auto tiffs = formats::export_print(lab, out / "labtiff", "tiff", dpi, 180, false, "paper", "auto", "", std::nullopt);
        const auto psds = formats::export_psd_pages(lab, out / "labpsd", dpi);
        const auto pdfs = formats::export_print(lab, out / "labpdf", "pdf", dpi, 180, false, "paper", "auto", "", std::nullopt);
        const std::string tiff = core::path_to_utf8(tiffs.at(0)), psd = core::path_to_utf8(psds.at(0));
        const Json answer = check(Json{{"read", Json{{"files", Json::array({tiff, psd})}, {"points", Json::array({Json::array({5, 5})})}}}});
        QCOMPARE(QString::fromStdString(answer["read"][tiff]["mode"].get<std::string>()), QStringLiteral("LAB"));
        QCOMPARE(answer["read"][tiff]["tiff"]["photometric"].get<int>(), 8);
        QVERIFY2(near(answer["read"][tiff]["pixels"][0], {255, 128, 128}, 0), answer["read"][tiff].dump().c_str());
        QCOMPARE(answer["read"][psd]["psd"]["mode"].get<int>(), 9);
        QProcess list;
        list.start(QStringLiteral("pdfimages"), {QStringLiteral("-list"), QString::fromStdString(core::path_to_utf8(pdfs.at(0)))});
        QVERIFY(list.waitForFinished(60000));
        QVERIFY2(QString::fromUtf8(list.readAllStandardOutput()).contains(QStringLiteral(" lab ")), "the PDF's image is not Lab");
    }

    // A Lab PSD: put on an RGB page it is the picture Pillow opens (its colours through Little CMS as Pillow converts
    // them); imported onto a Lab page its layers keep their values; onto an RGB page import_psd refuses it (Python's).
    void labPsd() {
        core::Document lab = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}},
                                                                Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}}}))
                                 .doc;
        render::Image picture = render::Image::create("RGBA", render::Size{60, 80}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{200, 120, 40, 255}, render::Box{0, 0, 30, 80});
        picture.paste(render::Ink{40, 90, 200, 255}, render::Box{30, 0, 60, 80});
        lab = run_ops(lab, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}})).doc;
        const fs::path out = core::path_from_utf8(path(QStringLiteral("labsrc")).toStdString());
        const auto psds = formats::export_psd_pages(lab, out, 100);
        const std::string psd_bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(psds.at(0))));
        // on an RGB page, as Pillow opens it
        const render::Image opened = render::open_image(psd_bytes, render::kPillowOpenLimits);
        QCOMPARE(QString::fromStdString(std::string(opened.mode())), QStringLiteral("LAB"));
        const std::string rgba = opened.convert("RGBA").tobytes();
        const Json answer = check(Json{{"read", Json{{"files", Json::array({core::path_to_utf8(psds.at(0))})},
                                                        {"points", Json::array({Json::array({20, 100}), Json::array({200, 100})})}}}});
        const Json& pillow = answer["read"][core::path_to_utf8(psds.at(0))]["rgba"];
        const int w = opened.width();
        for (int i = 0; i < 2; ++i) {
            const int x = pillow.size() > 0 ? (i == 0 ? 20 : 200) : 0;
            const auto* p = reinterpret_cast<const unsigned char*>(rgba.data()) + (std::size_t(100) * w + x) * 4;
            QVERIFY2(near(pillow[i], {p[0], p[1], p[2], p[3]}), pillow[i].dump().c_str());
        }
        const core::Document rgb = small_book();
        const auto placed = run_ops(rgb, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"layer", "ink"}, {"png_base64", core::b64encode(psd_bytes)}}})).doc;
        QVERIFY(placed.page(0).layers.size() > 0);
        // import_psd: onto a Lab page, the same values; onto an RGB page, refused as Python refuses it
        const core::Document target = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}}})).doc;
        const core::Document imported =
            run_ops(target, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(psd_bytes)}, {"fit", "paper"}}})).doc;
        const core::Layer* kept = nullptr;
        for (const auto& layer : imported.page(0).layers) {
            if (layer.title == "絵") kept = &layer;
        }
        QVERIFY(kept && kept->ink_raster);
        const auto src = core::decode_ink_raster(*lab.page(0).layers.back().ink_raster);
        const auto got = render::sample_ink_raster(core::decode_ink_raster(*kept->ink_raster), render::Size{60, 80}, render::Box{0, 0, 60, 80});
        for (const auto& [x, y] : {std::pair{10, 40}, std::pair{50, 40}}) {
            for (unsigned c = 0; c < 4; ++c) {
                const double want = src.samples[(std::size_t(y) * src.width + x) * 4 + c] / 255.0;
                QVERIFY2(std::abs(got.values[(std::size_t(y) * 60 + x) * 4 + c] - want) <= 1.5 / 255.0, "the Lab layer did not keep its values");
            }
        }
        QVERIFY_THROWS_EXCEPTION(core::ApplyError,
                                 run_ops(small_book(), Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(psd_bytes)}}})));
    }
};

QTEST_GUILESS_MAIN(TestContractInk)
#include "test_contract_ink.moc"
