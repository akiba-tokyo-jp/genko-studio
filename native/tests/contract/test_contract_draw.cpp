// render::Draw against Pillow's ImageDraw: 300 random calls (wide lines of 1–40 px with and without joint="curve",
// polygons, ellipses, rectangles, points, arcs, pie slices, chords; L, RGB, RGBA and RGB drawn with RGBA ink) from
// tools/migration/render_harness.py draw-cases. Every pixel must be the same. Skipped without the Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <optional>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "render/draw.hpp"
#include "render/stroke.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace render = genko::render;
using genko::test::from_bits;
using genko::test::ink_of;

namespace {

std::vector<render::PointD> points_of(const Json& list) {
    std::vector<render::PointD> out;
    for (const Json& p : list) out.push_back({from_bits(p[0]), from_bits(p[1])});
    return out;
}

std::optional<render::Ink> opt_ink(const Json& j) {
    if (j.is_null()) return std::nullopt;
    return ink_of(j);
}

render::BoxF box_of(const Json& b) { return {from_bits(b[0]), from_bits(b[1]), from_bits(b[2]), from_bits(b[3])}; }

}  // namespace

class TestContractDraw : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json cases_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + QStringLiteral("/draw.json");
        const auto py = genko::test::render_harness({"draw-cases", out, "--seed", "20261002", "--count", "300"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        cases_ = genko::test::read_json(out);
        QCOMPARE(cases_.size(), std::size_t{320});  // 300 random calls and 20 stamp_polyline lines
    }

    void draw() {
        int failures = 0;
        for (std::size_t i = 0; i < cases_.size(); ++i) {
            const Json& c = cases_[i];
            const std::string mode = c["mode"].get<std::string>();
            render::Image im = render::Image::create(mode, {c["size"][0].get<int>(), c["size"][1].get<int>()}, ink_of(c["background"]));
            {
                render::Draw d(im, c["blend"].get<bool>() ? "RGBA" : "");
                for (const Json& op : c["ops"]) {
                    const std::string kind = op["op"].get<std::string>();
                    if (kind == "line") {
                        const auto joint = op["joint"].is_null() ? render::Joint::None : render::Joint::Curve;
                        d.line(points_of(op["xy"]), ink_of(op["fill"]), op["width"].get<int>(), joint);
                    } else if (kind == "polygon") {
                        d.polygon(points_of(op["xy"]), opt_ink(op["fill"]), opt_ink(op["outline"]), op["width"].get<int>());
                    } else if (kind == "ellipse") {
                        d.ellipse(box_of(op["box"]), opt_ink(op["fill"]), opt_ink(op["outline"]), op["width"].get<int>());
                    } else if (kind == "rectangle") {
                        d.rectangle(box_of(op["box"]), opt_ink(op["fill"]), opt_ink(op["outline"]), op["width"].get<int>());
                    } else if (kind == "point") {
                        d.point(points_of(op["xy"]), ink_of(op["fill"]));
                    } else if (kind == "arc") {
                        d.arc(box_of(op["box"]), from_bits(op["start"]), from_bits(op["end"]), ink_of(op["fill"]), op["width"].get<int>());
                    } else if (kind == "pieslice") {
                        d.pieslice(box_of(op["box"]), from_bits(op["start"]), from_bits(op["end"]), opt_ink(op["fill"]),
                                   opt_ink(op["outline"]), op["width"].get<int>());
                    } else if (kind == "chord") {
                        d.chord(box_of(op["box"]), from_bits(op["start"]), from_bits(op["end"]), opt_ink(op["fill"]),
                                opt_ink(op["outline"]), op["width"].get<int>());
                    } else if (kind == "stamp") {
                        genko::core::PenPoints pts;
                        for (const Json& p : op["xy"]) {
                            genko::core::PenPoint q{from_bits(p[0]), from_bits(p[1]), std::nullopt};
                            if (p.size() > 2) q.p = from_bits(p[2]);
                            pts.push_back(q);
                        }
                        render::stamp_polyline(d, pts, 96, from_bits(op["width_mm"]), render::Ink(200), op["coords"] == "mm");
                    } else {
                        QFAIL(("unknown op " + kind).c_str());
                    }
                }
            }
            const render::Image want = render::Image::frombytes(mode, im.size(), genko::core::a2b_base64(c["result"].get<std::string>()));
            if (im.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(im, want);
                genko::test::keep_pictures(QStringLiteral("draw-%1").arg(i), im, want);
                qWarning("case %zu (%s, %s): %lld pixels differ, first at %lld", i, mode.c_str(),
                         genko::core::dump_python(c["ops"]).substr(0, 300).c_str(), diff.pixels, diff.first);
                ++failures;
            }
        }
        QCOMPARE(failures, 0);
    }

    void page_canvas() {
        // drawing on a part of a page (a render region) gives the same pixels as drawing on the whole page
        const render::Size page{97, 61};
        const auto paint = [](render::Draw& d) {
            const std::vector<render::PointD> line{{-3.7, 5.2}, {40.4, 33.9}, {12.0, 58.5}, {90.2, 2.1}};
            d.line(line, render::Ink{200, 30, 30}, 9, render::Joint::Curve);
            const std::vector<render::PointD> poly{{10.5, 10.5}, {80.2, 15.0}, {60.0, 55.7}, {5.0, 40.0}};
            d.polygon(poly, std::nullopt, render::Ink{20, 20, 20}, 5);
            d.ellipse({30.2, 20.1, 70.9, 50.5}, render::Ink{10, 200, 10});
            d.rectangle({2, 2, 94.5, 58}, std::nullopt, render::Ink{0, 0, 255}, 3);
        };
        render::Image whole = render::Image::create("RGB", page, render::Ink{255, 255, 255});
        {
            render::Draw d(whole);
            paint(d);
        }
        for (const render::Box area : {render::Box{0, 0, 97, 61}, render::Box{0, 13, 97, 40}, render::Box{20, 7, 61, 52},
                                       render::Box{90, 50, 97, 61}, render::Box{0, 0, 1, 1}}) {
            render::Image part = render::Image::create("RGB", {area.width(), area.height()}, render::Ink{255, 255, 255});
            render::PageCanvas canvas(part, area, page);
            paint(canvas.draw());
            canvas.commit();
            QVERIFY(part.tobytes() == whole.crop(area).tobytes());
        }
    }
};

QTEST_GUILESS_MAIN(TestContractDraw)
#include "test_contract_draw.moc"
