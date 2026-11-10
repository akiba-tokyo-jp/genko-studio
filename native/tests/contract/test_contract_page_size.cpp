// PC3 C-e (SPEC.md COLOR-07「判型」, schema-v4.md「ページだけの用紙（page-spec@1）」) through the command line: the page size
// ops on every kind of page (8, 16 and 32 bits, a profile's, grey, two levels, indexed, duotone, CMYK, Lab, multichannel),
// each page's pixels where the turn takes them (inspect --pixel), undone and done again; the whole book turned alike changing
// the book's paper (no extension named), one page cut to its own paper (page-spec@1, written and read again, the book not
// read-only), the PDF's and the PSD's pages as big as each page's paper, Image Size's pixels drawn again at the new size,
// Canvas Size's extension colour, Image Size and Canvas Size on a book with covers (the covers left out and named), and
// set_page_spec moving a precise layer's pixels with the page (it was refused before C-e: this build moves them now). (This
// build's own contract.)

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

constexpr int kPages = 11;
const std::vector<std::string> kModes{"rgb8", "rgb16", "rgb32", "p3", "gray", "bitmap", "indexed", "duotone", "cmyk", "lab", "multichannel"};

bool has_feature(const Json& saved, const char* feature) {
    return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
}

unsigned be32(const std::string& b, std::size_t at) {
    return (unsigned(static_cast<unsigned char>(b[at])) << 24) | (unsigned(static_cast<unsigned char>(b[at + 1])) << 16) |
           (unsigned(static_cast<unsigned char>(b[at + 2])) << 8) | unsigned(static_cast<unsigned char>(b[at + 3]));
}

// The PDF's pages' MediaBoxes, in order: [x0, y0, x1, y1] in points.
std::vector<std::vector<double>> media_boxes(const std::string& pdf) {
    std::vector<std::vector<double>> out;
    const std::regex box(R"(/MediaBox \[([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\])");
    for (auto it = std::sregex_iterator(pdf.begin(), pdf.end(), box); it != std::sregex_iterator(); ++it) {
        out.push_back({std::stod((*it)[1]), std::stod((*it)[2]), std::stod((*it)[3]), std::stod((*it)[4])});
    }
    return out;
}

}  // namespace

class TestContractPageSize : public QObject {
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
        const Json reply = genko({QStringLiteral("apply"), book, write_ops(name, ops), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    bool restore(const QString& book, const char* what) {
        int exit = -1;
        const Json reply = genko({QString::fromLatin1(what), book, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) qWarning("%s", reply.dump().c_str());
        return exit == 0;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    // The page's pixel at (x, y) mm, drawn at 40 dpi.
    Json pixel(const QString& book, int page, double x, double y) {
        return genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("%1:%2,%3").arg(page).arg(x).arg(y), QStringLiteral("--dpi"),
                      QStringLiteral("40")});
    }
    // The book is read again and can be changed (not read-only).
    bool opens(const QString& book, const QString& name) {
        return apply(book, name, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}}})).value("ok", false);
    }

    // A book of a page of each kind, each with a picture over the whole page: dark grey (64) on the left, green on the right
    // (the two-level page: the grey black, the green — 140 as grey — white).
    QString every_kind(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QString::number(kPages)});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{64, 64, 64, 255});
        picture.paste(render::Ink{30, 160, 90, 255}, render::Box{30, 0, 60, 40});
        const std::string png = genko::core::b64encode(render::write_png(picture));
        Json ops = Json::array();
        for (int p = 1; p <= kPages; ++p) {
            const std::string id = "pic" + std::to_string(p);
            ops.push_back(Json{{"op", "add_layer"}, {"page", p}, {"kind", "paint"}, {"id", id}});
            ops.push_back(Json{{"op", "set_layer"}, {"page", p}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
            ops.push_back(Json{{"op", "put_raster"}, {"page", p}, {"id", id}, {"png_base64", png}});
        }
        (void)apply(book, name + QStringLiteral("-paint.json"), ops);
        const std::string icc = genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")));
        (void)apply(book, name + QStringLiteral("-modes.json"),
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
        return book;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Every kind of page turned 90° clockwise, the whole book alike: each page's pixels where the turn takes them (the
    // grey left half on top), the book's paper on its side and no page of its own (no extension named); undone, the pages
    // as they were; done again; then turned back, the pages as they were.
    void everyKindTurns() {
        const QString book = every_kind(QStringLiteral("kinds.genko"));
        const Json before = project(book);
        QCOMPARE(before["pages"].size(), std::size_t(kPages));
        const double w = before["spec"]["width_mm"].get<double>(), h = before["spec"]["height_mm"].get<double>();
        std::vector<Json> grey, green;
        for (int p = 1; p <= kPages; ++p) {
            grey.push_back(pixel(book, p, 60, 100));
            green.push_back(pixel(book, p, w - 60, h / 2));
            QVERIFY2(grey.back().contains("values") && grey.back()["values"] != green.back()["values"], (kModes[p - 1] + ": " + grey.back().dump()).c_str());
        }
        const Json turned = apply(book, QStringLiteral("turn.json"), Json::array({Json{{"op", "rotate_pages"}, {"pages", "all"}, {"turn", "90cw"}}}));
        QVERIFY2(turned.value("ok", false), turned.dump().c_str());
        QCOMPARE(turned["results"][0]["pages"].size(), std::size_t(kPages));
        const auto check_turned = [&](const Json& saved) {
            QVERIFY(std::fabs(saved["spec"]["width_mm"].get<double>() - h) < 1e-6 && std::fabs(saved["spec"]["height_mm"].get<double>() - w) < 1e-6);
            QVERIFY2(!has_feature(saved, "page-spec@1"), saved["features"].dump().c_str());
            for (const Json& page : saved["pages"]) QVERIFY(!page.contains("native_spec"));
        };
        check_turned(project(book));
        // (a point (x, y) of the page is at (h − y, x) now)
        for (int p = 1; p <= kPages; ++p) {
            const std::string mode = kModes[std::size_t(p - 1)];
            const Json now_grey = pixel(book, p, h - 100, 60), now_green = pixel(book, p, h / 2, w - 60);
            QVERIFY2(now_grey["values"] == grey[std::size_t(p - 1)]["values"], (mode + ": " + now_grey.dump() + " / " + grey[std::size_t(p - 1)].dump()).c_str());
            QVERIFY2(now_green["values"] == green[std::size_t(p - 1)]["values"], (mode + ": " + now_green.dump()).c_str());
        }
        QVERIFY(restore(book, "undo"));
        QCOMPARE(project(book)["spec"], before["spec"]);
        for (int p = 1; p <= kPages; ++p) QVERIFY2(pixel(book, p, 60, 100)["values"] == grey[std::size_t(p - 1)]["values"], kModes[std::size_t(p - 1)].c_str());
        QVERIFY(restore(book, "redo"));
        check_turned(project(book));
        const Json back = apply(book, QStringLiteral("back.json"), Json::array({Json{{"op", "rotate_pages"}, {"pages", "all"}, {"turn", "90ccw"}}}));
        QVERIFY2(back.value("ok", false), back.dump().c_str());
        QCOMPARE(project(book)["spec"]["width_mm"], before["spec"]["width_mm"]);
        for (int p = 1; p <= kPages; ++p) {
            QVERIFY2(pixel(book, p, 60, 100)["values"] == grey[std::size_t(p - 1)]["values"], kModes[std::size_t(p - 1)].c_str());
            QVERIFY2(pixel(book, p, w - 60, h / 2)["values"] == green[std::size_t(p - 1)]["values"], kModes[std::size_t(p - 1)].c_str());
        }
        QVERIFY(opens(book, QStringLiteral("kinds-again.json")));
    }

    // One page cut to its own paper (page-spec@1, its native_spec written and read again, the book not read-only); the PDF
    // and the PSD of it as big as that paper; Image Size on every page (the pixels drawn again at the new size); Canvas
    // Size's extension colour; then set_page_spec puts the book on one paper again (no page of its own).
    void aPageOfItsOwn() {
        const QString book = every_kind(QStringLiteral("own.genko"));
        const Json before = project(book);
        const Json cut = apply(book, QStringLiteral("crop.json"),
                               Json::array({Json{{"op", "crop_page"}, {"page", 2}, {"rect", Json::array({12.7, 25.4, 190.5, 254.0})},
                                                 {"trim", Json::array({180, 240})}, {"bleed_mm", 3}, {"margins", Json::array({15, 15, 15, 15})}}}));
        QVERIFY2(cut.value("ok", false), cut.dump().c_str());
        Json saved = project(book);
        QVERIFY2(has_feature(saved, "page-spec@1"), saved["features"].dump().c_str());
        const Json& own = saved["pages"][1]["native_spec"];
        QCOMPARE(own["width_mm"], Json(190.5));
        QCOMPARE(own["height_mm"], Json(254.0));
        QVERIFY(!own.contains("trim_origin_mm"));
        QVERIFY(!saved["pages"][0].contains("native_spec"));
        QCOMPARE(saved["spec"], before["spec"]);
        QVERIFY(opens(book, QStringLiteral("own-again.json")));
        // the PDF: each page as big as its paper
        const auto pdf = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("own-pdf")), QStringLiteral("--format"),
                                                 QStringLiteral("pdf"), QStringLiteral("--dpi"),
                                                 QStringLiteral("20"), QStringLiteral("--json")});
        QVERIFY2(pdf.exit_code == 0, qPrintable(pdf.described()));
        const Json pdf_reply = genko::test::one_line(pdf.out);
        const std::vector<std::vector<double>> boxes = media_boxes(genko::test::read_bytes(QString::fromStdString(pdf_reply["files"][0].get<std::string>())));
        QCOMPARE(boxes.size(), std::size_t(kPages));
        const double pt = 72 / 25.4;
        QVERIFY2(std::fabs(boxes[0][2] - before["spec"]["width_mm"].get<double>() * pt) < 0.01, pdf_reply.dump().c_str());
        QVERIFY(std::fabs(boxes[1][2] - 190.5 * pt) < 0.01 && std::fabs(boxes[1][3] - 254.0 * pt) < 0.01);
        // the PSD of the page (one a page): its paper at 40 dpi
        const auto psd = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("own-psd")), QStringLiteral("--format"),
                                                 QStringLiteral("psd"), QStringLiteral("--dpi"),
                                                 QStringLiteral("40"), QStringLiteral("--json")});
        QVERIFY2(psd.exit_code == 0, qPrintable(psd.described()));
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(genko::test::one_line(psd.out)["files"][1].get<std::string>()));
        QCOMPARE(bytes.substr(0, 4), std::string("8BPS"));
        QCOMPARE(be32(bytes, 18), 300u);  // (190.5 mm at 40 dpi)
        QCOMPARE(be32(bytes, 14), 400u);  // (254 mm)
        // Image Size on every page: half as big, the pixels drawn again (the 16-bit page's precise raster too); the cut page
        // keeps a paper of its own
        const Json half = apply(book, QStringLiteral("half.json"), Json::array({Json{{"op", "resize_pages"}, {"width_mm", 128.5}, {"resample", "bilinear"}}}));
        QVERIFY2(half.value("ok", false), half.dump().c_str());
        saved = project(book);
        QVERIFY(std::fabs(saved["spec"]["width_mm"].get<double>() - 128.5) < 1e-6);
        QCOMPARE(saved["pages"][1]["native_spec"]["width_mm"], Json(128.5));
        for (int p = 1; p <= kPages; ++p) QVERIFY2(pixel(book, p, 10, 15).contains("values"), kModes[std::size_t(p - 1)].c_str());
        // Canvas Size's extension colour: the new strip of page 1 red
        const Json wider = apply(book, QStringLiteral("wider.json"),
                                 Json::array({Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 25.4}, {"relative", true}, {"anchor", "right"},
                                                   {"extension", Json::array({255, 0, 0})}}}));
        QVERIFY2(wider.value("ok", false), wider.dump().c_str());
        const Json red = pixel(book, 1, 5, 50);
        QVERIFY2(red["values"][0].get<double>() > 0.99 && red["values"][1].get<double>() < 0.01, red.dump().c_str());
        // set_page_spec: one paper for the whole book again; the precise pages' pixels moved with them
        const Json repapered = apply(book, QStringLiteral("repaper.json"), Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}}));
        QVERIFY2(repapered.value("ok", false), repapered.dump().c_str());
        saved = project(book);
        QVERIFY2(!has_feature(saved, "page-spec@1"), saved["features"].dump().c_str());
        for (const Json& page : saved["pages"]) QVERIFY(!page.contains("native_spec"));
        for (const int p : {2, 3, 4}) QVERIFY2(pixel(book, p, 30, 60).contains("values"), kModes[std::size_t(p - 1)].c_str());
        QVERIFY(opens(book, QStringLiteral("own-b5.json")));
    }

    // A book with covers (the coordinator's decision after the PC3 review): Image Size and Canvas Size on the whole book
    // leave out the covers (their paper set by their cover settings) and say so; the book's paper and the covers' saved
    // as they were, the other pages each on a paper of its own, the book read again; undone, as it was. A cover named is
    // refused.
    void aBookWithCovers() {
        const QString book = path(QStringLiteral("covers.genko"));
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const Json covered = apply(book, QStringLiteral("covers-add.json"),
                                   Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}, Json{{"op", "add_cover"}, {"kind", "jacket"}, {"spine_mm", 12}}}));
        QVERIFY2(covered.value("ok", false), covered.dump().c_str());
        const Json before = project(book);
        std::vector<Json> covers;
        for (const Json& page : before["pages"])
            if (page.contains("cover")) covers.push_back(page["index"]);
        QCOMPARE(covers.size(), std::size_t(2));
        const Json half = apply(book, QStringLiteral("covers-half.json"), Json::array({Json{{"op", "resize_pages"}, {"width_mm", 128.5}}}));
        QVERIFY2(half.value("ok", false), half.dump().c_str());
        QCOMPARE(half["results"][0]["skipped_covers"], Json(covers));
        Json saved = project(book);
        QCOMPARE(saved["spec"], before["spec"]);
        QVERIFY2(has_feature(saved, "page-spec@1"), saved["features"].dump().c_str());
        for (const Json& page : saved["pages"]) {
            if (page.contains("cover")) {
                QVERIFY(!page.contains("native_spec"));
            } else {
                QCOMPARE(page["native_spec"]["width_mm"], Json(128.5));
            }
        }
        QVERIFY(opens(book, QStringLiteral("covers-again.json")));
        const Json wider = apply(book, QStringLiteral("covers-wider.json"),
                                 Json::array({Json{{"op", "set_canvas_size"}, {"width_mm", 10}, {"relative", true}}}));
        QVERIFY2(wider.value("ok", false), wider.dump().c_str());
        QCOMPARE(wider["results"][0]["skipped_covers"], Json(covers));
        // a cover named: refused
        int exit = -1;
        const Json named = genko({QStringLiteral("apply"), book,
                                  write_ops(QStringLiteral("covers-named.json"),
                                            Json::array({Json{{"op", "resize_pages"}, {"page", covers[0]}, {"width_mm", 100}}})),
                                  QStringLiteral("--agent"), QStringLiteral("human:test")},
                                 &exit);
        QVERIFY2(exit != 0 && !named.value("ok", true) && named.dump().find("is a cover") != std::string::npos, named.dump().c_str());
        QVERIFY(restore(book, "undo"));
        QVERIFY(restore(book, "undo"));
        QVERIFY(restore(book, "undo"));
        saved = project(book);
        QCOMPARE(saved["spec"], before["spec"]);
        for (const Json& page : saved["pages"]) QVERIFY(!page.contains("native_spec"));
    }

    // set_page_spec moving a precise layer's pixels (refused before C-e): the 16-bit page's raster as big as the new paper
    // at the resolution it had, its grey where the basic frame took it (A4's frame onto B5's, smaller and further in: a
    // point well inside it grey, the new paper's top left corner — beyond the old paper's edge — bare); undone, as it was.
    // (The raster is coarse — 60 × 40 over the page —: the points kept clear of its bare edge's neighbours.)
    void setPageSpecMovesPrecisePixels() {
        const QString book = path(QStringLiteral("precise.genko"));
        QCOMPARE(genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")}).exit_code, 0);
        render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{128, 128, 128, 255});
        picture.paste(render::Ink{30, 160, 90, 255}, render::Box{30, 0, 60, 40});
        QVERIFY(apply(book, QStringLiteral("p-paint.json"),
                      Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                   Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", genko::core::b64encode(render::write_png(picture))}},
                                   Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}))
                    .value("ok", false));
        const Json grey = pixel(book, 1, 40, 60);
        QVERIFY2(std::fabs(pixel(book, 1, 5, 5)["values"][0].get<double>() - grey["values"][0].get<double>()) < 0.01, grey.dump().c_str());
        const Json moved = apply(book, QStringLiteral("p-b5.json"), Json::array({Json{{"op", "set_page_spec"}, {"preset", "b5"}}}));
        QVERIFY2(moved.value("ok", false), moved.dump().c_str());
        const Json saved = project(book);
        bool precise = false;
        for (const Json& layer : saved["pages"][0]["layers"]) precise = precise || layer.contains("color_raster");
        QVERIFY(precise);
        const Json now = pixel(book, 1, 40, 60);
        QVERIFY2(std::fabs(now["values"][0].get<double>() - grey["values"][0].get<double>()) < 0.01, (now.dump() + " / " + grey.dump()).c_str());
        const Json corner = pixel(book, 1, 5, 5);
        QVERIFY2(corner["values"][0].get<double>() > 0.99, corner.dump().c_str());
        QVERIFY(restore(book, "undo"));
        QVERIFY2(pixel(book, 1, 5, 5)["values"] == grey["values"], grey.dump().c_str());
        QVERIFY(restore(book, "redo"));
        QVERIFY2(pixel(book, 1, 5, 5)["values"][0].get<double>() > 0.99, grey.dump().c_str());
    }
};

QTEST_GUILESS_MAIN(TestContractPageSize)
#include "test_contract_page_size.moc"
