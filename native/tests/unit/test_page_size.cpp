// PC3 C-e (SPEC.md COLOR-07「判型」, docs/cpp-migration/ARCHITECTURE.md「判型」): the page size ops on a book in memory —
// each thing on a page moved as the map of the page takes it (a paint layer's pixels, a mask, a patch, a channel and a
// saved mask area exactly by a turn or a move of whole pixels; lines, panels, their splits, guides, rulers, paths, lines of
// dialogue with their letters upright), four quarter turns and two flips bringing the page back, Image Size scaling the
// lines' widths and letters, Canvas Size's anchor and extension colour, Crop and Trim and Reveal All on the working grid,
// Arbitrary's paper round the turned one, the finished size and basic frame staying with what the page holds (refused when
// they would not fit, unless given anew), a page's own paper (page-spec@1) and the whole book's, a placed picture turned as
// a smart object, a 3D prop, a cover and one page of a spread refused.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "core/vector.hpp"
#include "render/bookpages.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

using namespace genko;
using core::Document;
using core::Json;
using core::Num;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

core::ApplyResult run(const Document& doc, const Json& ops, const std::string& actor = "human:test") {
    return bus().apply(doc, ops, core::Actor(actor));
}

Document after(const Document& doc, const Json& ops) { return run(doc, ops).doc; }

std::string refusal(const Document& doc, const Json& ops, const std::string& actor = "human:test") {
    try {
        bus().apply(doc, ops, core::Actor(actor));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return "(applied)";
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// 4 × 6 inches (800 × 1200 pixels at the working 200 dpi): moves on its working grid are whole pixels of the test's
// pictures too.
core::PageSpec paper() { return core::PageSpec::custom(101.6, 152.4, 88.9, 139.7, 3, 10, 10, 12, 8, 600, "color"); }

// A picture whose every pixel says where it is (red 10·x, green 10·y): a move can be followed pixel by pixel.
render::Image coded(int w, int h) {
    std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(x * 10);
            bytes[i + 1] = static_cast<char>(y * 10);
            bytes[i + 2] = 7;
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    return render::Image::frombytes("RGBA", render::Size{w, h}, bytes);
}

render::Image grey_coded(int w, int h) {
    std::string bytes(std::size_t(w) * std::size_t(h), '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) bytes[std::size_t(y) * std::size_t(w) + std::size_t(x)] = static_cast<char>(x * 16 + y);
    }
    return render::Image::frombytes("L", render::Size{w, h}, bytes);
}

const core::Layer& layer_of(const core::Page& page, std::string_view id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + std::string(id));
}

std::string pixels(const core::Bytes& png, const char* mode = "RGBA") { return render::read_png(*png).convert(mode).tobytes(); }

// A book of two pages, the first holding something of every kind.
Document furnished() {
    Document doc = core::new_episode("判型", Num(1), 2, paper(), core::Binding::Right);
    doc = after(doc, Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"name", "α"}}}));
    core::Page& page = doc.edit_page(0);
    page.split_frame(page.frames.at(0).id, "vertical", Num(0.4), Num(3));
    core::Layer paint;
    paint.id = "paint";
    paint.kind = core::LayerKind::Raster;
    paint.panel_clip = false;
    paint.raster_png = std::make_shared<const std::string>(render::write_png(coded(16, 24)));
    paint.mask.emplace();
    paint.mask->png = std::make_shared<const std::string>(render::write_png(grey_coded(12, 18)));
    core::Patch patch;
    patch.attrs = Json{{"id", "pa"}, {"box", Json::array({12.7, 25.4, 6.35, 12.7})}, {"mode", "paste"}};
    patch.png = std::make_shared<const std::string>(render::write_png(coded(2, 4)));
    paint.patches.push_back(patch);
    page.layers.push_back(paint);
    core::Stroke stroke;
    stroke.id = "s1";
    stroke.points = {{20, 30}, {40, 35}, {60, 32}};
    stroke.pressure = {0.5, 0.6, 0.7};
    stroke.width_mm = 0.6;
    page.layers.at(2).strokes = core::make_strokes({std::make_shared<const core::Stroke>(stroke)});
    page.rulers = Json::array({Json{{"id", "g1"}, {"kind", "guide"}, {"axis", "h"}, {"at", 25.4}},
                               Json{{"id", "r1"}, {"kind", "line"}, {"points", Json::array({Json::array({10.0, 20.0}), Json::array({30.0, 40.0})})}}});
    core::vector::Subpath tri;
    tri.knots = {core::vector::Knot::corner(10, 10), core::vector::Knot::corner(30, 10), core::vector::Knot::corner(10, 50)};
    page.work_path = core::vector::Path{};
    page.work_path->subpaths.push_back(tri);
    page.extra["saved_areas"] = Json{{"m", Json{{"mask", Json{{"box", Json::array({25.4, 50.8, 12.7, 25.4})},
                                                               {"png", core::b64encode(render::write_png(grey_coded(4, 8)))}}}}}};
    core::InkPixels alpha = core::blank_ink_pixels(core::InkSpace::Mask, 8, 8, 12);
    for (std::size_t i = 0; i < alpha.pixel_count(); ++i) alpha.set(i, 0, double(i % 8) / 8.0 + double(i / 8) / 100.0);
    page.channels.at(0).raster = std::make_shared<const std::string>(core::encode_ink_raster(alpha));
    core::StoryLine& line = doc.add_line(Num(1), "セリフ", "", std::nullopt, "", Num(20), Num(30), Num(30), Num(10));
    line.style = Json{{"size_mm", 4.0}};
    line.tail = core::Point{Num(25), Num(50)};
    return doc;
}

Json one(const Json& op) { return Json::array({op}); }

}  // namespace

class TestPageSize : public QObject {
    Q_OBJECT

private slots:
    // 90° clockwise: the paper on its side; each pixel where the turn takes it (a paint layer's, its mask's, a patch's, a
    // channel's, a saved mask area's); a line's points, a guide's axis, a split's axis, the work path; a line of dialogue's
    // box turned with its letters upright; the finished size and basic frame turned with the page (the margins from the
    // turned frame, the binding side's the inner one); the page alone of its book: its own paper.
    void aQuarterTurnTakesEachThingWithIt() {
        const Document doc = furnished();
        const core::ApplyResult done = run(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}}));
        const core::Page& page = done.doc.page(0);
        QVERIFY(near(page.spec.width_mm.value(), 152.4) && near(page.spec.height_mm.value(), 101.6));
        QVERIFY(page.own_spec);
        QVERIFY(!done.doc.page(1).own_spec);
        QVERIFY(near(done.doc.spec.width_mm.value(), 101.6));
        QVERIFY(core::vector::uses_page_spec(done.doc));
        // the paint layer: 24 × 16, new (X, Y) the old (Y, 23 − X)
        const render::Image turned = render::read_png(*layer_of(page, "paint").raster_png).convert("RGBA");
        QCOMPARE(turned.width(), 24);
        QCOMPARE(turned.height(), 16);
        const std::string px = turned.tobytes();
        for (int Y = 0; Y < 16; ++Y) {
            for (int X = 0; X < 24; ++X) {
                const std::size_t i = (std::size_t(Y) * 24 + std::size_t(X)) * 4;
                QCOMPARE(static_cast<int>(static_cast<unsigned char>(px[i])), Y * 10);
                QCOMPARE(static_cast<int>(static_cast<unsigned char>(px[i + 1])), (23 - X) * 10);
            }
        }
        const render::Image mask = render::read_png(*layer_of(page, "paint").mask->png).convert("L");
        QCOMPARE(mask.width(), 18);
        QCOMPARE(mask.height(), 12);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(mask.tobytes()[0])), 0 * 16 + 17);  // (old (0, 17))
        // the patch: its box turned, its picture 4 × 2
        const core::Patch& patch = layer_of(page, "paint").patches.at(0);
        const Json& box = patch.attrs["box"];
        QVERIFY2(near(box[0].get<double>(), 152.4 - 25.4 - 12.7, 1e-3) && near(box[1].get<double>(), 12.7, 1e-3), box.dump().c_str());
        QVERIFY(near(box[2].get<double>(), 12.7, 1e-3) && near(box[3].get<double>(), 6.35, 1e-3));
        QCOMPARE(render::read_png(*patch.png).width(), 4);
        // the channel: 12 × 8, new (X, Y) the old (Y, 11 − X)
        const core::InkPixels alpha = core::decode_ink_raster(*page.channels.at(0).raster);
        QCOMPARE(alpha.width, 12u);
        QCOMPARE(alpha.height, 8u);
        const core::InkPixels was = core::decode_ink_raster(*doc.page(0).channels.at(0).raster);
        for (std::uint32_t Y = 0; Y < 8; ++Y) {
            for (std::uint32_t X = 0; X < 12; ++X) QCOMPARE(alpha.value(Y * 12 + X, 0), was.value((11 - X) * 8 + Y, 0));
        }
        // the saved mask area: its box turned, its picture 8 × 4
        const Json& area = page.extra["saved_areas"]["m"]["mask"];
        QVERIFY2(near(area["box"][0].get<double>(), 152.4 - 50.8 - 25.4, 1e-3) && near(area["box"][1].get<double>(), 25.4, 1e-3), area.dump().c_str());
        // lines, rulers, guides, panels, paths
        const core::Stroke& s = *page.layers.at(2).strokes->items.at(0);
        QVERIFY(near(s.points[0].x, 152.4 - 30) && near(s.points[0].y, 20));
        QVERIFY(near(s.width_mm, 0.6));
        QCOMPARE(page.rulers[0]["axis"], Json("v"));
        QVERIFY(near(page.rulers[0]["at"].get<double>(), 152.4 - 25.4, 1e-3));
        QCOMPARE(*page.frames.at(0).split_axis, std::string("horizontal"));
        QVERIFY(near(page.work_path->subpaths[0].knots[0].anchor.x, 152.4 - 10) && near(page.work_path->subpaths[0].knots[0].anchor.y, 10));
        const core::StoryLine& line = done.doc.story.at(0);
        QVERIFY(near(line.w_mm.value(), 10) && near(line.h_mm.value(), 30));
        QVERIFY(near(line.x_mm.value() + line.w_mm.value() / 2, 152.4 - 35) && near(line.y_mm.value() + line.h_mm.value() / 2, 35));
        QVERIFY(!line.style.contains("rotate_deg"));  // (its letters upright)
        // the finished size turned (still in the middle), the basic frame turned: the old left (outer, 8) on top
        const auto margins = page.spec.margins();
        QVERIFY(!page.spec.trim_origin_mm);
        QVERIFY(near(page.spec.trim_size().first.value(), 139.7) && near(page.spec.trim_size().second.value(), 88.9));
        QVERIFY2(near(margins.top, 8) && near(margins.bottom, 12), page.spec.describe().c_str());
        QVERIFY(near(margins.inner + margins.outer, 20));
        // the report
        QCOMPARE(done.results.at(0)["pages"][0]["paper_mm"], (Json::array({152.4, 101.6})));
    }

    // Four quarter turns (either way) and two flips or half turns bring the page back as it was: its pixels exact, its
    // paper the book's again.
    void turnsComeBack() {
        const Document doc = furnished();
        for (const char* turn : {"90cw", "90ccw", "180", "flip_h", "flip_v"}) {
            const int times = std::string(turn).starts_with("90") ? 4 : 2;
            Json ops = Json::array();
            for (int k = 0; k < times; ++k) ops.push_back(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", turn}});
            const Document back = after(doc, ops);
            const core::Page& page = back.page(0);
            QVERIFY2(!page.own_spec, turn);
            QVERIFY(near(page.spec.width_mm.value(), 101.6) && near(page.spec.height_mm.value(), 152.4));
            QCOMPARE(pixels(layer_of(page, "paint").raster_png), pixels(layer_of(doc.page(0), "paint").raster_png));
            QCOMPARE(pixels(layer_of(page, "paint").mask->png, "L"), pixels(layer_of(doc.page(0), "paint").mask->png, "L"));
            QCOMPARE(pixels(layer_of(page, "paint").patches.at(0).png), pixels(layer_of(doc.page(0), "paint").patches.at(0).png));
            QCOMPARE(core::decode_ink_raster(*page.channels.at(0).raster).samples, core::decode_ink_raster(*doc.page(0).channels.at(0).raster).samples);
            const core::Stroke& s = *page.layers.at(2).strokes->items.at(0);
            QVERIFY2(near(s.points[1].x, 40, 1e-3) && near(s.points[1].y, 35, 1e-3), turn);
            QVERIFY(near(page.frames.at(0).children.at(0).rect.x.value(), doc.page(0).frames.at(0).children.at(0).rect.x.value(), 1e-3));
            QCOMPARE(page.rulers[0]["axis"], Json("h"));
            const core::StoryLine& line = back.story.at(0);
            QVERIFY(near(line.x_mm.value(), 20, 1e-3) && near(line.w_mm.value(), 30, 1e-3));
        }
    }

    // A saved mask area's picture turns with the page by every turn — a quarter either way, a half (once: the picture upside
    // down, not only its box moved), a flip either way —, and by a crop tilted by 180°.
    void aSavedAreaTurnsWithEveryTurn() {
        const Document doc = furnished();
        const std::string was = grey_coded(4, 8).tobytes();  // (x·16 + y)
        struct Case {
            const char* turn;
            int w, h;
            double x, y;
            std::pair<int, int> (*from)(int, int);
        };
        const Case cases[] = {
            {"90cw", 8, 4, 76.2, 25.4, [](int X, int Y) { return std::pair{Y, 7 - X}; }},
            {"90ccw", 8, 4, 50.8, 63.5, [](int X, int Y) { return std::pair{3 - Y, X}; }},
            {"180", 4, 8, 63.5, 76.2, [](int X, int Y) { return std::pair{3 - X, 7 - Y}; }},
            {"flip_h", 4, 8, 63.5, 50.8, [](int X, int Y) { return std::pair{3 - X, Y}; }},
            {"flip_v", 4, 8, 25.4, 76.2, [](int X, int Y) { return std::pair{X, 7 - Y}; }},
        };
        const auto check = [&](const Document& turned, const Case& c, int off) {
            const Json& mask = turned.page(0).extra["saved_areas"]["m"]["mask"];
            QVERIFY2(near(mask["box"][0].get<double>(), c.x, 1e-3) && near(mask["box"][1].get<double>(), c.y, 1e-3), (c.turn + mask["box"].dump()).c_str());
            const render::Image picture = render::read_png(core::a2b_base64(mask["png"].get<std::string>())).convert("L");
            QCOMPARE(picture.width(), c.w);
            QCOMPARE(picture.height(), c.h);
            const std::string got = picture.tobytes();
            for (int Y = 0; Y < c.h; ++Y) {
                for (int X = 0; X < c.w; ++X) {
                    const auto [x, y] = c.from(X, Y);
                    const int value = static_cast<unsigned char>(got[std::size_t(Y) * std::size_t(c.w) + std::size_t(X)]);
                    QVERIFY2(std::abs(value - static_cast<unsigned char>(was[std::size_t(y) * 4 + std::size_t(x)])) <= off,
                             qPrintable(QStringLiteral("%1 (%2, %3): %4").arg(c.turn).arg(X).arg(Y).arg(value)));
                }
            }
        };
        for (const Case& c : cases) check(after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", c.turn}})), c, 0);
        // (the angle 180: the same half turn)
        check(after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"angle", 180}})), cases[2], 0);
        // (a crop of the whole page tilted by 180°: its pixels projected — on their own centres here)
        check(after(doc, one(Json{{"op", "crop_page"}, {"page", 1}, {"rect", Json::array({0, 0, 101.6, 152.4})}, {"angle", 180}})), cases[2], 1);
    }

    // set_page_spec (原稿用紙の設定, move): what PC3 adds to a page goes with the basic frame by the same map as the lines — a
    // pen layer's shape parts, a vector mask, the work and saved paths, the saved areas (a mask area's box), a smart object's
    // filter mask —; a paint layer's pixels and a mask keep the resolution they have (a picture that is not at the working
    // 200 dpi is not taken for one), and those at 200 dpi (a mask at 150) are moved to the very pixels they were before.
    void setPageSpecMovesWhatPC3Adds() {
        Document doc = furnished();
        {
            core::Page& page = doc.edit_page(0);
            const auto square = [](double x, double y, double w, double h) {
                core::vector::Subpath s;
                s.knots = {core::vector::Knot::corner(x, y), core::vector::Knot::corner(x + w, y), core::vector::Knot::corner(x + w, y + h),
                           core::vector::Knot::corner(x, y + h)};
                core::vector::Path p;
                p.subpaths.push_back(s);
                return p;
            };
            core::vector::ShapePart part;
            part.id = "part1";
            part.path = square(20, 30, 10, 10);
            part.fill = Json{{"rgb", Json::array({10, 20, 30})}};
            part.stroke = core::vector::ShapeStroke{};
            part.stroke->width_mm = 1.0;
            page.layers.at(2).shapes = core::vector::make_shapes({part});
            for (core::Layer& layer : page.layers) {
                if (layer.id == "paint") layer.vector_mask = core::vector::VectorMask{square(15, 25, 20, 20)};
            }
            page.paths.push_back(core::vector::NamedPath{"輪郭", square(40, 60, 5, 5)});
            page.extra["saved_areas"]["r"] = Json{{"rect", Json::array({10, 20, 30, 40})}};
            // (a paint layer at the working 200 dpi, its mask at 150 dpi: 800 × 1200 and 600 × 900 on this paper)
            core::Layer working;
            working.id = "p200";
            working.kind = core::LayerKind::Raster;
            working.panel_clip = false;
            std::string rgba(std::size_t(800) * 1200 * 4, '\0');
            for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<char>((i * 7 + i / 3200) % 251);
            working.raster_png = std::make_shared<const std::string>(render::write_png(render::Image::frombytes("RGBA", render::Size{800, 1200}, rgba)));
            std::string grey(std::size_t(600) * 900, '\0');
            for (std::size_t i = 0; i < grey.size(); ++i) grey[i] = static_cast<char>((i / 600 + i % 600) % 256);
            working.mask.emplace();
            working.mask->png = std::make_shared<const std::string>(render::write_png(render::Image::frombytes("L", render::Size{600, 900}, grey)));
            page.layers.push_back(working);
        }
        // (a smart object with a filter mask over the left half of the page)
        doc = after(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "src"}, {"name", "絵"}},
                                      Json{{"op", "put_raster"}, {"page", 1}, {"id", "src"}, {"png_base64", core::b64encode(render::write_png(coded(20, 30)))}},
                                      Json{{"op", "smart_convert"}, {"page", 1}, {"id", "src"}, {"new_id", "so"}},
                                      Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "invert"},
                                           {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({50.8, 0}), Json::array({50.8, 152.4}),
                                                                               Json::array({0, 152.4})})}}}}}));
        const std::string filter_mask = (*layer_of(doc.page(0), "so").smart)["filter_mask"]["asset"].get<std::string>();
        const Document moved = after(doc, one(Json{{"op", "set_page_spec"}, {"margins", Json{{"top", 20}, {"bottom", 5}, {"inner", 15}, {"outer", 20}}}}));
        const core::Page& was = doc.page(0);
        const core::Page& page = moved.page(0);
        // (the 200-dpi layer and its 150-dpi mask: the pixels the move gave them before)
        const std::string working_ref = core::bytes_ref(pixels(layer_of(page, "p200").raster_png));
        const std::string mask_ref = core::bytes_ref(pixels(layer_of(page, "p200").mask->png, "L"));
        QCOMPARE(working_ref, std::string("sha256:cf56fc9832abd7b60eefed91fd9ff6e22aa0078ac93b6dae6c30a4f935598332"));
        QCOMPARE(mask_ref, std::string("sha256:3af34be915e554ca18f08f3fc3f6b0178ae08cd5234f85a69f2655a00f1759ce"));
        const std::string side = doc.start_side.value_or("");
        const core::Rect a = was.inner_rect_mm(side), b = page.inner_rect_mm(side);
        const double sx = (b.width / a.width).value(), sy = (b.height / a.height).value();
        QVERIFY(!near(sx, 1) && !near(sy, 1));
        const auto X = [&](double x) { return b.x.value() + (x - a.x.value()) * sx; };
        const auto Y = [&](double y) { return b.y.value() + (y - a.y.value()) * sy; };
        // the lines (as before), and by the same map: the shape part, the vector mask, the paths, the saved areas
        const core::Stroke& s = *page.layers.at(2).strokes->items.at(0);
        QVERIFY(near(s.points[0].x, X(20), 1e-3) && near(s.points[0].y, Y(30), 1e-3));
        const core::vector::ShapePart& part = page.layers.at(2).shapes->items.at(0);
        QVERIFY2(near(part.path.subpaths[0].knots[2].anchor.x, X(30)) && near(part.path.subpaths[0].knots[2].anchor.y, Y(40)),
                 core::vector::to_json(part.path).dump().c_str());
        QVERIFY(near(part.stroke->width_mm, std::sqrt(sx * sy), 1e-4));
        const core::vector::Knot& corner = layer_of(page, "paint").vector_mask->path.subpaths[0].knots[0];
        QVERIFY(near(corner.anchor.x, X(15)) && near(corner.anchor.y, Y(25)));
        QVERIFY(near(page.work_path->subpaths[0].knots[1].anchor.x, X(30)) && near(page.work_path->subpaths[0].knots[1].anchor.y, Y(10)));
        QVERIFY(near(page.paths.at(0).path.subpaths[0].knots[0].anchor.x, X(40)) && near(page.paths.at(0).path.subpaths[0].knots[0].anchor.y, Y(60)));
        const Json& r = page.extra["saved_areas"]["r"]["rect"];
        QVERIFY2(near(r[0].get<double>(), X(10), 1e-3) && near(r[1].get<double>(), Y(20), 1e-3) && near(r[2].get<double>(), 30 * sx, 1e-3) &&
                     near(r[3].get<double>(), 40 * sy, 1e-3),
                 r.dump().c_str());
        const Json& m = page.extra["saved_areas"]["m"]["mask"]["box"];
        QVERIFY2(near(m[0].get<double>(), X(25.4), 1e-3) && near(m[1].get<double>(), Y(50.8), 1e-3) && near(m[2].get<double>(), 12.7 * sx, 1e-3),
                 m.dump().c_str());
        // the filter mask moved as a layer's mask is
        const std::string moved_mask = (*layer_of(page, "so").smart)["filter_mask"]["asset"].get<std::string>();
        QVERIFY(moved_mask != filter_mask && moved.smart_files.contains(moved_mask));
        const auto expected = render::relayout_mask(*doc.smart_files.at(filter_mask), was.spec, page.spec, a, b);
        QVERIFY(expected.has_value());
        QCOMPARE(pixels(moved.smart_files.at(moved_mask), "L"), pixels(std::make_shared<const std::string>(*expected), "L"));
        // a paint layer of 16 × 24 (not 200 dpi) and its mask of 12 × 18 keep their resolution: each pixel the old one
        // that moved there (red 10·x of the old place, bilinear)
        const render::Image picture = render::read_png(*layer_of(page, "paint").raster_png).convert("RGBA");
        QCOMPARE(picture.width(), 16);
        QCOMPARE(picture.height(), 24);
        const std::string got = picture.tobytes();
        int seen = 0;
        for (int v = 0; v < 24; ++v) {
            for (int u = 0; u < 16; ++u) {
                const double old_x = (a.x.value() + ((u + 0.5) * 6.35 - b.x.value()) / sx) / 6.35 - 0.5;
                const double old_y = (a.y.value() + ((v + 0.5) * 6.35 - b.y.value()) / sy) / 6.35 - 0.5;
                if (old_x < 0 || old_y < 0 || old_x > 15 || old_y > 23) continue;
                const std::size_t i = (std::size_t(v) * 16 + std::size_t(u)) * 4;
                QVERIFY2(std::fabs(static_cast<unsigned char>(got[i]) - old_x * 10) <= 2 && std::fabs(static_cast<unsigned char>(got[i + 1]) - old_y * 10) <= 2,
                         qPrintable(QStringLiteral("(%1, %2): %3 %4").arg(u).arg(v).arg(static_cast<unsigned char>(got[i])).arg(old_x * 10)));
                ++seen;
            }
        }
        QVERIFY(seen > 200);
        const render::Image mask = render::read_png(*layer_of(page, "paint").mask->png).convert("L");
        QCOMPARE(mask.width(), 12);
        QCOMPARE(mask.height(), 18);
        // on a larger paper: the same resolution, so more pixels
        const Document b5 = after(doc, one(Json{{"op", "set_page_spec"}, {"preset", "b5"}}));
        const render::Image wide = render::read_png(*layer_of(b5.page(0), "paint").raster_png);
        QCOMPARE(wide.width(), static_cast<int>(std::lround(b5.page(0).spec.width_mm.value() / 6.35)));
        QCOMPARE(render::read_png(*layer_of(b5.page(0), "paint").mask->png).width(), static_cast<int>(std::lround(b5.page(0).spec.width_mm.value() / 101.6 * 12)));
    }

    // A flip mirrors a split's children (their order along it) and a line's place, never its letters.
    void aFlipMirrorsTheSplit() {
        const Document doc = furnished();
        const Document flipped = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "flip_h"}}));
        const core::Frame& root = flipped.page(0).frames.at(0);
        const core::Frame& was = doc.page(0).frames.at(0);
        QCOMPARE(root.children.at(0).id, was.children.at(1).id);
        QVERIFY(near(flipped.story.at(0).x_mm.value(), 101.6 - 50, 1e-3));
        // the basic frame mirrored: the inner and outer margins change sides — the page keeps a paper of its own
        const auto m = flipped.page(0).spec.margins();
        QVERIFY2(near(m.inner, 8) && near(m.outer, 12), flipped.page(0).spec.describe().c_str());
        QVERIFY(flipped.page(0).own_spec);
    }

    // Image Size: the paper, its finished size, bleed and basic frame and everything on it × k; the pixels drawn again at
    // the size that keeps their resolution; the lines' widths and the letters' size × k. The whole book alike: the book's
    // paper changes (no page of its own). Without resampling the pixels stay as they are and the resolution changes;
    // the resolution alone changes nothing on the pages; a resolution for some pages only is refused.
    void imageSizeScalesTheBook() {
        const Document doc = furnished();
        const Document half = after(doc, one(Json{{"op", "resize_pages"}, {"width_mm", 50.8}}));
        QVERIFY(near(half.spec.width_mm.value(), 50.8) && near(half.spec.height_mm.value(), 76.2));
        QVERIFY(near(half.spec.trim_size().first.value(), 44.45) && near(half.spec.bleed_mm.value(), 1.5));
        QVERIFY(!half.page(0).own_spec && !half.page(1).own_spec && !core::vector::uses_page_spec(half));
        const render::Image picture = render::read_png(*layer_of(half.page(0), "paint").raster_png);
        QCOMPARE(picture.width(), 8);
        QCOMPARE(picture.height(), 12);
        // (no clear rim: the edge of an opaque picture stays opaque)
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(picture.convert("RGBA").tobytes()[3])), 255);
        const core::Stroke& s = *half.page(0).layers.at(2).strokes->items.at(0);
        QVERIFY(near(s.points[0].x, 10) && near(s.width_mm, 0.3));
        QVERIFY(near(half.story.at(0).style["size_mm"].get<double>(), 2.0, 1e-3));
        QVERIFY(near(half.story.at(0).w_mm.value(), 15, 1e-3));
        // without resampling: 600 → 300 dpi keeps the pixels and doubles the mm
        const Document kept = after(doc, one(Json{{"op", "resize_pages"}, {"resample", "none"}, {"dpi", 300}}));
        QVERIFY(near(kept.spec.width_mm.value(), 203.2) && near(kept.spec.dpi.value(), 300));
        QVERIFY(layer_of(kept.page(0), "paint").raster_png == layer_of(doc.page(0), "paint").raster_png);
        QVERIFY(near(kept.page(0).layers.at(2).strokes->items.at(0)->points[0].x, 40));
        // the resolution alone
        const Document dpi = after(doc, one(Json{{"op", "resize_pages"}, {"dpi", 350}}));
        QVERIFY(near(dpi.spec.dpi.value(), 350) && near(dpi.spec.width_mm.value(), 101.6));
        QVERIFY(dpi.page(0).layers.at(2).strokes == doc.page(0).layers.at(2).strokes);
        QVERIFY2(refusal(doc, one(Json{{"op", "resize_pages"}, {"pages", Json::array({1})}, {"dpi", 300}})).find("pages \"all\"") != std::string::npos,
                 refusal(doc, one(Json{{"op", "resize_pages"}, {"pages", Json::array({1})}, {"dpi", 300}})).c_str());
        // in pixels at the page's resolution: 1200 px at 600 dpi = 50.8 mm
        const Document by_px = after(doc, one(Json{{"op", "resize_pages"}, {"width_px", 1200}}));
        QVERIFY(near(by_px.spec.width_mm.value(), 50.8));
        // one page: its own paper
        const Document one_page = after(doc, one(Json{{"op", "resize_pages"}, {"pages", Json::array({2})}, {"width_mm", 50.8}}));
        QVERIFY(one_page.page(1).own_spec && !one_page.page(0).own_spec && near(one_page.spec.width_mm.value(), 101.6));
    }

    // Canvas Size: the paper grows or shrinks about its anchor; everything moves by whole pixels of the working grid, the
    // pixels by whole pixels of their own (the new strip clear, a mask's edge carried on); the finished size stays with
    // what the page holds (off the middle: the page's own paper, never the book's); the extension colour on the bottom
    // paint layer; a canvas that cuts the finished size is refused unless it is given anew.
    void canvasSizeMovesByWholePixels() {
        const Document doc = furnished();
        const Document wide = after(doc, one(Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 25.4}, {"relative", true}, {"anchor", "right"}}));
        const core::Page& page = wide.page(0);
        QVERIFY(near(page.spec.width_mm.value(), 127.0));
        const render::Image picture = render::read_png(*layer_of(page, "paint").raster_png).convert("RGBA");
        QCOMPARE(picture.width(), 20);
        const std::string px = picture.tobytes();
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(px[3])), 0);  // (the new strip on the left: clear)
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(px[4 * 4])), 0);  // (old (0, 0) now at (4, 0))
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(px[4 * 4 + 3])), 255);
        const render::Image mask = render::read_png(*layer_of(page, "paint").mask->png).convert("L");
        QCOMPARE(mask.width(), 15);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(mask.tobytes()[0])), 0);  // (its left edge carried on: old (0, 0))
        QVERIFY(near(page.layers.at(2).strokes->items.at(0)->points[0].x, 45.4));
        QVERIFY(page.spec.trim_origin_mm && near(page.spec.trim_origin_mm->first, 31.75) && near(page.spec.trim_origin_mm->second, 6.35));
        QVERIFY(page.own_spec);
        // the whole book from the top left: the book's paper keeps its finished size in the middle, each page its own
        const Document every = after(doc, one(Json{{"op", "set_canvas_size"}, {"width_mm", 127}, {"height_mm", 177.8}, {"anchor", "top_left"}}));
        QVERIFY(near(every.spec.width_mm.value(), 127) && !every.spec.trim_origin_mm);
        QVERIFY(every.page(0).own_spec && every.page(1).own_spec);
        // the extension colour
        const Document red = after(doc, one(Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 127}, {"anchor", "right"}, {"extension", Json::array({255, 0, 0})}}));
        const std::string filled = render::read_png(*layer_of(red.page(0), "paint").raster_png).convert("RGBA").tobytes();
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(filled[0])), 255);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(filled[3])), 255);
        // a canvas that cuts the finished size, then with it given anew
        const Json small{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 50.8}, {"height_mm", 76.2}};
        QVERIFY2(refusal(doc, one(small)).find("finished size") != std::string::npos, refusal(doc, one(small)).c_str());
        Json anew = small;
        anew["trim"] = Json::array({40, 70});
        anew["bleed_mm"] = 2;
        anew["margins"] = Json::array({5, 5, 5, 5});
        const Document cut = after(doc, one(anew));
        QVERIFY(near(cut.page(0).spec.trim_size().first.value(), 40) && !cut.page(0).spec.trim_origin_mm);
    }

    // Crop: the page cut to a box (rounded to the working grid) — a box past the paper too —, or to an area's box; tilted:
    // turned back about the box's middle first (the panels then free corners); the cut pixels are not kept.
    void cropCutsThePage() {
        const Document doc = furnished();
        Json crop{{"op", "crop_page"}, {"page", 1}, {"rect", Json::array({12.7, 12.7, 76.2, 127.0})}, {"trim", Json::array({70, 120})},
                  {"bleed_mm", 2}, {"margins", Json::array({5, 5, 5, 5})}};
        const core::ApplyResult done = run(doc, one(crop));
        const core::Page& page = done.doc.page(0);
        QVERIFY(near(page.spec.width_mm.value(), 76.2) && near(page.spec.height_mm.value(), 127));
        QVERIFY(near(page.layers.at(2).strokes->items.at(0)->points[0].x, 20 - 12.7));
        const render::Image picture = render::read_png(*layer_of(page, "paint").raster_png).convert("RGBA");
        QCOMPARE(picture.width(), 12);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(picture.tobytes()[0])), 2 * 10);  // (old (2, 2))
        QCOMPARE(done.results.at(0)["pages"][0]["offset_mm"], (Json::array({-12.7, -12.7})));
        // the finished size would be cut: refused
        crop.erase("trim");
        crop.erase("margins");
        crop.erase("bleed_mm");
        QVERIFY(refusal(doc, one(crop)).find("finished size") != std::string::npos);
        // pixels kept off the page: refused
        QVERIFY(refusal(doc, one(Json{{"op", "crop_page"}, {"page", 1}, {"rect", Json::array({0, 0, 101.6, 152.4})}, {"delete_pixels", false}}))
                    .find("delete_pixels") != std::string::npos);
        // an area's box (a rectangle, resolved by the bus)
        const Document by_area = after(doc, one(Json{{"op", "crop_page"}, {"page", 1}, {"area", Json{{"rect", Json::array({0, 0, 101.6, 152.4})}}}}));
        QVERIFY(near(by_area.page(0).spec.width_mm.value(), 101.6));
        // tilted 10°: the box upright, the panels free corners
        const Document tilted = after(doc, one(Json{{"op", "crop_page"}, {"page", 1}, {"rect", Json::array({0, 0, 101.6, 152.4})}, {"angle", 10},
                                                     {"trim", Json::array({80, 130})}, {"margins", Json::array({5, 5, 5, 5})}}));
        QVERIFY(near(tilted.page(0).spec.width_mm.value(), 101.6));
        QVERIFY(tilted.page(0).frames.at(0).poly.has_value());
        QCOMPARE(render::read_png(*layer_of(tilted.page(0), "paint").raster_png).width(), 16);
    }

    // Trim: the page cut to what is drawn on it (over nothing; or unlike its top left corner), side by side as asked.
    void trimCutsToWhatIsDrawn() {
        Document doc = core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right);
        doc.edit_page(0).frames.clear();
        core::Stroke stroke;
        stroke.id = "s1";
        stroke.points = {{25.4, 50.8}, {76.2, 50.8}};
        stroke.pressure = {1.0, 1.0};
        stroke.width_mm = 1.0;
        doc.edit_page(0).layers.at(2).strokes = core::make_strokes({std::make_shared<const core::Stroke>(stroke)});
        Json trim{{"op", "trim_page"}, {"page", 1}, {"trim", Json::array({40, 0.5})}, {"bleed_mm", 0}, {"margins", Json::array({0.1, 0.1, 0.1, 0.1})}};
        const Document cut = after(doc, one(trim));
        const core::Page& page = cut.page(0);
        QVERIFY2(page.spec.width_mm.value() > 50.8 && page.spec.width_mm.value() < 53, page.spec.describe().c_str());
        QVERIFY2(page.spec.height_mm.value() > 0.9 && page.spec.height_mm.value() < 2, page.spec.describe().c_str());
        const core::Stroke& s = *page.layers.at(2).strokes->items.at(0);
        QVERIFY(s.points[0].x > 0 && s.points[0].x < 1.5 && s.points[0].y > 0 && s.points[0].y < 1.5);
        // the top and bottom only, by the top left corner's colour
        trim["based_on"] = "top_left";
        trim["left"] = false;
        trim["right"] = false;
        const Document rows = after(doc, one(trim));
        QVERIFY(near(rows.page(0).spec.width_mm.value(), 101.6));
        // nothing drawn: refused
        Document empty = core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right);
        empty.edit_page(0).frames.clear();
        QVERIFY(refusal(empty, one(Json{{"op", "trim_page"}, {"page", 1}})).find("nothing to trim to") != std::string::npos);
    }

    // Reveal All: the paper grown (on the working grid) to what lies past it; nothing past it: nothing changes.
    void revealAllGrowsThePaper() {
        Document doc = core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right);
        core::Stroke stroke;
        stroke.id = "s1";
        stroke.points = {{-10, 50}, {50, 50}};
        stroke.pressure = {1.0, 1.0};
        stroke.width_mm = 1.0;
        doc.edit_page(0).layers.at(2).strokes = core::make_strokes({std::make_shared<const core::Stroke>(stroke)});
        const core::ApplyResult done = run(doc, one(Json{{"op", "reveal_all"}, {"page", 1}}));
        const core::Page& page = done.doc.page(0);
        QVERIFY2(page.spec.width_mm.value() >= 101.6 + 10.5 - 1e-6 && page.spec.width_mm.value() < 101.6 + 10.5 + 0.13, page.spec.describe().c_str());
        QVERIFY(near(page.spec.height_mm.value(), 152.4));
        QVERIFY(page.layers.at(2).strokes->items.at(0)->points[0].x >= 0.5 - 1e-6);
        QVERIFY(page.spec.trim_origin_mm.has_value());
        const Document plain = after(core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right), one(Json{{"op", "reveal_all"}, {"page", 1}}));
        QVERIFY(near(plain.page(0).spec.width_mm.value(), 101.6) && !plain.page(0).own_spec);
    }

    // Arbitrary: the paper round the turned one, the finished size in its middle; panels free corners, a guide a line
    // ruler through the same place; a right angle given as an angle a turn (its pixels exact); a 3D prop refused.
    void arbitraryTurnsAboutTheMiddle() {
        const Document doc = furnished();
        const Document turned = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"angle", 30}, {"interp", "bilinear"}}));
        const core::Page& page = turned.page(0);
        const double c = std::cos(std::numbers::pi / 6), s = std::sin(std::numbers::pi / 6);
        QVERIFY(near(page.spec.width_mm.value(), 101.6 * c + 152.4 * s, 1e-3) && near(page.spec.height_mm.value(), 101.6 * s + 152.4 * c, 1e-3));
        QVERIFY(!page.spec.trim_origin_mm);
        QVERIFY(near(page.spec.trim_size().first.value(), 88.9));
        QVERIFY(page.frames.at(0).poly.has_value());
        QCOMPARE(page.rulers[0]["kind"], Json("line"));
        const render::Image picture = render::read_png(*layer_of(page, "paint").raster_png).convert("RGBA");
        QCOMPARE(picture.width(), static_cast<int>(std::lround(page.spec.width_mm.value() * 16 / 101.6)));
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(picture.tobytes()[3])), 0);  // (a corner outside the old paper)
        const Document right = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"angle", -90}}));
        const Document ccw = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90ccw"}}));
        QCOMPARE(pixels(layer_of(right.page(0), "paint").raster_png), pixels(layer_of(ccw.page(0), "paint").raster_png));
        Document props = doc;
        props.edit_page(0).prims = Json::array({Json{{"kind", "box"}, {"pos", Json::array({10, 10, 0})}, {"size", Json::array({5, 5, 5})}}});
        QVERIFY(refusal(props, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}})).find("3D") != std::string::npos);
        QVERIFY(refusal(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}, {"text", "rotate"}})).find("upright") != std::string::npos);
    }

    // A cover keeps the paper its cover settings give (refused a size; a half turn or a flip it takes); one page of a
    // spread may not change its size alone; a page someone else holds is refused.
    void coversSpreadsAndLocks() {
        Document doc = core::new_episode("判型", Num(1), 3, paper(), core::Binding::Right);
        doc = after(doc, Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}, Json{{"op", "set_spread"}, {"page", 2}, {"with", 3}},
                                      Json{{"op", "set_spread"}, {"page", 3}, {"with", 2}}}));
        const Num cover = doc.pages.back()->index;
        QVERIFY(refusal(doc, one(Json{{"op", "crop_page"}, {"page", cover.json()}, {"rect", Json::array({0, 0, 50, 50})}})).find("cover") != std::string::npos);
        QCOMPARE(refusal(doc, one(Json{{"op", "rotate_pages"}, {"page", cover.json()}, {"turn", "180"}})), std::string("(applied)"));
        QVERIFY(refusal(doc, one(Json{{"op", "rotate_pages"}, {"page", 2}, {"turn", "90cw"}})).find("spread") != std::string::npos);
        QCOMPARE(refusal(doc, one(Json{{"op", "rotate_pages"}, {"page", 2}, {"turn", "flip_h"}})), std::string("(applied)"));
        QCOMPARE(refusal(doc, one(Json{{"op", "rotate_pages"}, {"pages", Json::array({2, 3})}, {"turn", "90cw"}})), std::string("(applied)"));
        // the whole book (the coordinator's decision after the PC3 review): a cover left out where the op would change its
        // size, the other pages changed (each on a paper of its own), the book's paper and the cover's as they were; named,
        // a cover is refused a size; a new resolution, the whole book's, refused while a cover keeps its size
        const std::size_t at_cover = doc.pages.size() - 1;
        for (const Json& op : {Json{{"op", "resize_pages"}, {"width_mm", 50.8}}, Json{{"op", "resize_pages"}, {"pages", "all"}, {"width_mm", 50.8}},
                               Json{{"op", "set_canvas_size"}, {"width_mm", 25.4}, {"relative", true}}}) {
            const core::ApplyResult done = run(doc, one(op));
            QCOMPARE(done.results.size(), std::size_t(1));
            QCOMPARE(done.results[0]["skipped_covers"], Json::array({cover.json()}));
            QCOMPARE(done.results[0]["pages"].size(), doc.pages.size() - 1);
            QVERIFY(done.doc.page(at_cover).spec.width_mm == doc.page(at_cover).spec.width_mm && !done.doc.page(at_cover).own_spec);
            QVERIFY(done.doc.spec.width_mm == doc.spec.width_mm);
            for (std::size_t i = 0; i < at_cover; ++i) {
                QVERIFY(done.doc.page(i).own_spec);
                QVERIFY(!(done.doc.page(i).spec.width_mm == doc.page(i).spec.width_mm));
            }
        }
        QVERIFY(refusal(doc, one(Json{{"op", "resize_pages"}, {"page", cover.json()}, {"width_mm", 50.8}})).find("cover") != std::string::npos);
        QVERIFY(refusal(doc, one(Json{{"op", "resize_pages"}, {"pages", Json::array({1, cover.json()})}, {"width_mm", 50.8}})).find("cover") != std::string::npos);
        QVERIFY(refusal(doc, one(Json{{"op", "resize_pages"}, {"dpi", 300}, {"resample", "none"}})).find("cover") != std::string::npos);
        QVERIFY(refusal(doc, one(Json{{"op", "resize_pages"}, {"dpi", 300}, {"width_mm", 50.8}})).find("cover") != std::string::npos);
        {  // (a new resolution alone keeps every size: the cover with the rest, the book's paper too, nothing left out)
            const core::ApplyResult finer = run(doc, one(Json{{"op", "resize_pages"}, {"dpi", 300}}));
            QVERIFY(!finer.results[0].contains("skipped_covers"));
            QVERIFY(finer.doc.spec.dpi == Num(300) && finer.doc.page(at_cover).spec.dpi == Num(300));
        }
        {  // (a book of covers alone: refused)
            Document covers_only = doc;
            covers_only.pages.erase(covers_only.pages.begin(), covers_only.pages.begin() + static_cast<std::ptrdiff_t>(at_cover));
            QVERIFY(refusal(covers_only, one(Json{{"op", "resize_pages"}, {"width_mm", 50.8}})).find("cover") != std::string::npos);
        }
        doc.page_locks = Json{{doc.page(0).id, "human:other"}};
        QVERIFY(refusal(doc, one(Json{{"op", "rotate_pages"}, {"pages", "all"}, {"turn", "180"}})).find("locked by human:other") != std::string::npos);
    }

    // A tone's screen turned with its page, its look kept (the coordinator's decision after the PC3 review): lines across
    // the page run down it after a quarter turn, as the turned page shows them (they stayed across before).
    void aTonesScreenTurnsWithItsPage() {
        Document doc = core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right);
        doc = after(doc, one(Json{{"op", "add_tone"}, {"page", 1}, {"id", "t"}, {"area", Json{{"rect", Json::array({0, 0, 101.6, 152.4})}}},
                                  {"pattern", "line"}, {"angle", 0}, {"lpi", 10}, {"density", 0.5}}));
        // (how much the middle of the page changes along its rows and down its columns)
        const auto changes = [](const Document& d) {
            render::RenderOptions print;
            print.finish = true;  // (its dots: the print finish of a colour page)
            const render::Image grey = render::render_page(d.page(0), 100, print, &d).image.convert("L");
            const std::string px = grey.tobytes();
            const int w = grey.width(), h = grey.height();
            double along = 0, down = 0;
            for (int y = h * 3 / 10; y < h * 7 / 10; ++y) {
                for (int x = w * 3 / 10; x < w * 7 / 10; ++x) {
                    const auto at = [&](int i, int j) { return static_cast<double>(static_cast<unsigned char>(px[std::size_t(j) * std::size_t(w) + std::size_t(i)])); };
                    along += std::fabs(at(x + 1, y) - at(x, y));
                    down += std::fabs(at(x, y + 1) - at(x, y));
                }
            }
            return std::pair{along, down};
        };
        const auto [along0, down0] = changes(doc);
        QVERIFY2(down0 > 10 * along0 && down0 > 0, qPrintable(QString("%1 %2").arg(along0).arg(down0)));  // (lines across)
        const Document turned = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}}));
        QCOMPARE(layer_of(turned.page(0), "t").angle, 90.0);
        const auto [along1, down1] = changes(turned);
        QVERIFY2(along1 > 10 * down1 && along1 > 0, qPrintable(QString("%1 %2").arg(along1).arg(down1)));  // (lines down it)
    }

    // A placed picture turned with its page: it becomes a smart object of its file (the same id), its matrix turned.
    void aPlacedPictureTurnsAsASmartObject() {
        QTemporaryDir dir;
        Document doc = core::new_episode("判型", Num(1), 1, paper(), core::Binding::Right);
        doc.asset_dir = core::path_from_utf8(dir.path().toStdString());
        const std::string png = render::write_png(coded(8, 4));
        const std::string ref = storage::AssetStore(*doc.asset_dir).put_bytes(png, ".png");
        core::Layer placed;
        placed.id = "pic";
        placed.kind = core::LayerKind::Placed;
        placed.asset = ref;
        placed.placement_mm = core::Rect{Num(10), Num(20), Num(40), Num(20)};
        placed.clip_to = "none";
        doc.edit_page(0).layers.push_back(placed);
        const Document turned = after(doc, one(Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}}));
        const core::Layer& layer = layer_of(turned.page(0), "pic");
        QVERIFY(layer.kind == core::LayerKind::Smart);
        const core::smart::Matrix m = core::smart::matrix_of(layer);
        const auto [x, y] = m.apply(0, 0);  // (the picture's top left: (10, 20) turned)
        QVERIFY2(near(x, 152.4 - 20, 1e-6) && near(y, 10, 1e-6), core::smart::to_json(m).dump().c_str());
        // a move keeps it a placed picture
        const Document moved = after(doc, one(Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 25.4}, {"relative", true}, {"anchor", "right"}}));
        QVERIFY(layer_of(moved.page(0), "pic").kind == core::LayerKind::Placed);
        QVERIFY(near(layer_of(moved.page(0), "pic").placement_mm->x.value(), 35.4));
    }
};

QTEST_GUILESS_MAIN(TestPageSize)
#include "test_page_size.moc"
