// PC3 C-0 (SPEC.md COLOR-07, ARCHITECTURE.md「要素を写す」): everything on a page moved by one map — a quarter turn
// swaps a guide's axis and a panel split's, a flip mirrors a split's children and its cut, two quarter turns are a half
// turn, four bring the page back; a line of dialogue's box turned with its text upright; a tone's screen turned and
// mirrored with the page; a 3D prop refused a turn — and
// the warps of PhotoCraft's geom/src/warp.rs tests (the styles' names, a preset at bend 0 the identity, the identity
// mesh exact and an affine map fitted exactly, splits exact and removable, the arc's known points, a preset fitted
// closely as a mesh).

#include <QtTest>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/frames.hpp"
#include "core/model.hpp"
#include "core/page_geometry.hpp"
#include "core/strokes.hpp"

using namespace genko;
using core::Json;
namespace pg = core::page_geometry;

namespace {

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("pc3", core::Num(1), 1, spec, core::Binding::Right);
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

bool same_affine(const pg::Affine& a, const pg::Affine& b) {
    for (std::size_t i = 0; i < 6; ++i)
        if (!near(a[i], b[i], 1e-12)) return false;
    return true;
}

// The book with something of every kind on its page.
core::Document furnished() {
    core::Document doc = small_book();
    core::Page& page = doc.edit_page(0);
    // a panel split side by side, its children left then right
    core::Frame& root = page.frames.at(0);
    page.split_frame(root.id, "vertical", core::Num(0.4), core::Num(3));
    core::Stroke stroke;
    stroke.id = "s1";
    stroke.points = {{10, 10}, {20, 15}, {30, 12}};
    stroke.pressure = {0.5, 0.6, 0.7};
    stroke.rotation = {0, 10, 20};
    stroke.width_mm = 0.5;
    page.layers.at(2).strokes = core::make_strokes({std::make_shared<const core::Stroke>(stroke)});
    page.rulers = Json::array({Json{{"id", "g1"}, {"kind", "guide"}, {"axis", "h"}, {"at", 10.0}},
                               Json{{"id", "g2"}, {"kind", "guide"}, {"axis", "v"}, {"at", 20.0}},
                               Json{{"id", "r1"}, {"kind", "line"}, {"points", Json::array({Json::array({1.0, 2.0}), Json::array({3.0, 4.0})})}}});
    page.paths.push_back(core::vector::NamedPath{"p", {}});
    core::vector::Subpath tri;
    tri.knots = {core::vector::Knot::corner(5, 5), core::vector::Knot::corner(15, 5), core::vector::Knot::corner(5, 25)};
    page.paths[0].path.subpaths.push_back(tri);
    page.extra["saved_areas"] = Json{{"a", Json{{"poly", Json::array({Json::array({1.0, 1.0}), Json::array({5.0, 1.0}), Json::array({5.0, 9.0})})}}}};
    doc.add_line(core::Num(1), "セリフ", "", std::nullopt, "", core::Num(10), core::Num(20), core::Num(30), core::Num(10));
    return doc;
}

}  // namespace

class TestPageGeometry : public QObject {
    Q_OBJECT

private slots:
    // A quarter turn: a horizontal guide becomes vertical where the page's turn takes it (and the other way); a line ruler's
    // points are turned; a general turn makes a guide a line ruler through the same place.
    void guidesTurn() {
        const pg::Mapping cw = pg::turn_mapping(pg::Turn::Cw90, 60, 80);
        Json h{{"kind", "guide"}, {"axis", "h"}, {"at", 10.0}};
        pg::map_ruler(h, cw);
        QCOMPARE(h["axis"].get<std::string>(), std::string("v"));
        QCOMPARE(h["at"].get<double>(), 70.0);  // (x' = −y + 80)
        Json v{{"kind", "guide"}, {"axis", "v"}, {"at", 20.0}};
        pg::map_ruler(v, cw);
        QCOMPARE(v["axis"].get<std::string>(), std::string("h"));
        QCOMPARE(v["at"].get<double>(), 20.0);  // (y' = x)
        Json flipped{{"kind", "guide"}, {"axis", "v"}, {"at", 20.0}};
        pg::map_ruler(flipped, pg::turn_mapping(pg::Turn::FlipH, 60, 80));
        QCOMPARE(flipped["at"].get<double>(), 40.0);
        Json tilted{{"kind", "guide"}, {"axis", "h"}, {"at", 10.0}};
        const double a = 30 * std::numbers::pi / 180;
        pg::map_ruler(tilted, pg::mapping({std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0}));
        QCOMPARE(tilted["kind"].get<std::string>(), std::string("line"));
        QVERIFY(!tilted.contains("at"));
        QCOMPARE(tilted["points"].size(), std::size_t(2));
    }

    // A flip mirrors a split: side-by-side children reversed so the first is again the left one, the cut mirrored; a quarter
    // turn makes a side-by-side split a stacked one; the bows kept on their edges.
    void framesTurn() {
        core::Document doc = small_book();
        core::Page& page = doc.edit_page(0);
        const auto [left, right] = page.split_frame(page.frames.at(0).id, "vertical", core::Num(0.3), core::Num(4));
        const std::string left_id = left->id, right_id = right->id;
        page.frames.at(0).children.at(0).curves = std::vector<double>{1, 2, 3, 4};
        core::Frame flipped = page.frames.at(0);
        const double left_x = flipped.children.at(0).rect.x.value();
        pg::map_frame(flipped, pg::turn_mapping(pg::Turn::FlipH, 60, 80));
        QCOMPARE(flipped.split_axis.value(), std::string("vertical"));
        QCOMPARE(flipped.children.at(0).id, right_id);  // (the right one is now on the left: first)
        QCOMPARE(flipped.children.at(1).id, left_id);
        QVERIFY(flipped.children.at(0).rect.x.value() < flipped.children.at(1).rect.x.value());
        QVERIFY(near(flipped.children.at(1).rect.x.value() + flipped.children.at(1).rect.width.value(), 60 - left_x, 1e-3));
        // (a rect's bows: top and bottom stay, left and right swap)
        QCOMPARE(*flipped.children.at(1).curves, (std::vector<double>{1, 4, 3, 2}));
        if (flipped.split) {
            const double a = (*flipped.split)["a"][0].get<double>();
            const double b = (*page.frames.at(0).split)["a"][0].get<double>();
            QVERIFY(near(a, 1 - b, 1e-5));
        }
        core::Frame turned = page.frames.at(0);
        pg::map_frame(turned, pg::turn_mapping(pg::Turn::Cw90, 60, 80));
        QCOMPARE(turned.split_axis.value(), std::string("horizontal"));
        QVERIFY(turned.children.at(0).rect.y.value() < turned.children.at(1).rect.y.value());
        QCOMPARE(turned.children.at(0).id, left_id);  // (the left one turned to the top)
        QCOMPARE(*turned.children.at(0).curves, (std::vector<double>{4, 1, 2, 3}));
        // (no right-angle map: free corners)
        core::Frame free = page.frames.at(0);
        pg::map_frame(free, pg::mapping({std::cos(0.2), std::sin(0.2), -std::sin(0.2), std::cos(0.2), 0, 0}));
        QCOMPARE(free.split_axis.value(), std::string(core::kFreeSplit));
        QVERIFY(free.children.at(0).poly.has_value());
    }

    // Maps applied one after the other: two quarter turns a half turn (the paper turned between), a flip twice nothing;
    // the whole page turned four times is the page again (its lines, panels, rulers, paths, saved areas and its line of
    // dialogue, whose box comes back and whose text never turned).
    void turnsCompose() {
        const pg::Mapping two = pg::then(pg::turn_mapping(pg::Turn::Cw90, 60, 80), pg::turn_mapping(pg::Turn::Cw90, 80, 60));
        QVERIFY(same_affine(two.m, pg::turn_mapping(pg::Turn::Rotate180, 60, 80).m));
        const pg::Mapping flips = pg::then(pg::turn_mapping(pg::Turn::FlipV, 60, 80), pg::turn_mapping(pg::Turn::FlipV, 60, 80));
        QVERIFY(same_affine(flips.m, {1, 0, 0, 1, 0, 0}));
        QCOMPARE(pg::turn_mapping(pg::Turn::Cw90, 60, 80).angle_deg(), 90.0);
        QVERIFY(pg::turn_mapping(pg::Turn::FlipH, 60, 80).reflects());
        QVERIFY(pg::turn_mapping(pg::Turn::Cw90, 60, 80).swaps_axes());

        const core::Document before = furnished();
        core::Document doc = before;
        double w = 60, h = 80;
        for (int i = 0; i < 4; ++i) {
            pg::map_page(doc, 0, pg::turn_mapping(pg::Turn::Cw90, w, h), {});
            std::swap(w, h);
        }
        const core::Page& a = before.page(0);
        const core::Page& b = doc.page(0);
        const core::Stroke& s0 = *a.layers.at(2).strokes->items.at(0);
        const core::Stroke& s1 = *b.layers.at(2).strokes->items.at(0);
        for (std::size_t i = 0; i < s0.points.size(); ++i) QVERIFY(near(s0.points[i].x, s1.points[i].x, 1e-3) && near(s0.points[i].y, s1.points[i].y, 1e-3));
        QCOMPARE(s1.width_mm, s0.width_mm);
        QVERIFY(near(s1.rotation[1], s0.rotation[1] + 360));  // (its pen's turn: four quarter turns more)
        QCOMPARE(b.rulers[0]["axis"], a.rulers[0]["axis"]);
        QVERIFY(near(b.rulers[0]["at"].get<double>(), 10.0));
        QVERIFY(near(b.rulers[2]["points"][1][0].get<double>(), 3.0));
        QVERIFY(near(b.paths[0].path.subpaths[0].knots[2].anchor.y, 25.0));
        QVERIFY(near(b.extra["saved_areas"]["a"]["poly"][2][1].get<double>(), 9.0));
        QCOMPARE(b.frames.at(0).split_axis, a.frames.at(0).split_axis);
        QCOMPARE(b.frames.at(0).children.at(0).id, a.frames.at(0).children.at(0).id);
        QVERIFY(near(b.frames.at(0).children.at(1).rect.x.value(), a.frames.at(0).children.at(1).rect.x.value(), 1e-3));
        const core::StoryLine& line = doc.story.at(0);
        QVERIFY(near(line.x_mm.value(), 10) && near(line.y_mm.value(), 20) && near(line.w_mm.value(), 30) && near(line.h_mm.value(), 10));
        // (one quarter turn: the balloon's box turned — its middle moved, its sides swapped — its text upright)
        core::Document once = before;
        pg::map_page(once, 0, pg::turn_mapping(pg::Turn::Cw90, 60, 80), {});
        const core::StoryLine& l = once.story.at(0);
        QVERIFY(near(l.w_mm.value(), 10) && near(l.h_mm.value(), 30));
        QVERIFY(near(l.x_mm.value() + l.w_mm.value() / 2, 80 - 25) && near(l.y_mm.value() + l.h_mm.value() / 2, 25));
    }

    // A tone's screen, its linear gradient's way and a layer's トーン化 screen turned and mirrored with the page (the
    // coordinator's decision after the PC3 review: they stayed as they were, the look changed): a quarter turn adds 90°, a
    // mirror mirrors the angle (a screen's to −a, a gradient's way to 180 − a), each kept in its range; four quarter
    // turns bring them back; a move and a scale leave them as they were, written as they were.
    void screensTurn() {
        const auto with_screens = [](double tone_angle, const Json& gradient_angle, double screen_angle) {
            core::Document doc = small_book();
            core::Page& page = doc.edit_page(0);
            core::Layer tone;
            tone.id = "t";
            tone.kind = core::LayerKind::Tone;
            tone.angle = tone_angle;
            tone.tone = Json{{"pattern", "line"}, {"gradient", Json{{"start", 0}, {"end", 1}}}};
            if (!gradient_angle.is_null()) (*tone.tone)["gradient"]["angle"] = gradient_angle;
            core::Layer round = tone;
            round.id = "r";
            round.tone = Json{{"pattern", "dot"}, {"gradient", Json{{"shape", "radial"}, {"start", 0}, {"end", 1}}}};
            core::Layer screened;
            screened.id = "s";
            screened.kind = core::LayerKind::Strokes;
            screened.screen = Json{{"pattern", "line"}, {"lpi", 60.0}, {"angle", screen_angle}, {"black", 0.1}, {"white", 0.95}};
            page.layers.push_back(tone);
            page.layers.push_back(round);
            page.layers.push_back(screened);
            return doc;
        };
        const auto layer = [](const core::Document& doc, const char* id) -> const core::Layer& {
            for (const core::Layer& l : doc.page(0).layers)
                if (l.id == id) return l;
            throw std::runtime_error(id);
        };
        struct Case {
            pg::Turn turn;
            double tone, gradient, screen;
        };
        // (a tone at 30°, its gradient's way 0°, a screen at 30°)
        for (const Case& c : {Case{pg::Turn::Cw90, 120, 90, 120}, Case{pg::Turn::Ccw90, 300, 270, 120}, Case{pg::Turn::Rotate180, 210, 180, 30},
                              Case{pg::Turn::FlipH, 330, 180, 150}, Case{pg::Turn::FlipV, 150, 0, 150}}) {
            core::Document doc = with_screens(30, Json(0), 30);
            pg::map_page(doc, 0, pg::turn_mapping(c.turn, 60, 80), {});
            QVERIFY2(near(layer(doc, "t").angle, c.tone), qPrintable(QString::number(layer(doc, "t").angle)));
            QVERIFY2(near(layer(doc, "t").tone->at("gradient").at("angle").get<double>(), c.gradient), layer(doc, "t").tone->dump().c_str());
            QVERIFY2(near(layer(doc, "s").screen->at("angle").get<double>(), c.screen), layer(doc, "s").screen->dump().c_str());
            QVERIFY(!layer(doc, "r").tone->at("gradient").contains("angle"));  // (a radial gradient has no way)
        }
        // (a gradient with no angle — 90° — and one given as a number's text, as the op keeps it)
        {
            core::Document doc = with_screens(45, Json(), 45);
            pg::map_page(doc, 0, pg::turn_mapping(pg::Turn::Cw90, 60, 80), {});
            QVERIFY(near(layer(doc, "t").tone->at("gradient").at("angle").get<double>(), 180));
            core::Document text = with_screens(45, Json("30"), 45);
            pg::map_page(text, 0, pg::turn_mapping(pg::Turn::FlipH, 60, 80), {});
            QVERIFY(near(text.page(0).layers.back().screen->at("angle").get<double>(), 135));
            QVERIFY(near(layer(text, "t").tone->at("gradient").at("angle").get<double>(), 150));
        }
        // four quarter turns: as they were
        {
            core::Document doc = with_screens(30, Json(15), 30);
            double w = 60, h = 80;
            for (int i = 0; i < 4; ++i) {
                pg::map_page(doc, 0, pg::turn_mapping(pg::Turn::Cw90, w, h), {});
                std::swap(w, h);
            }
            QVERIFY(near(layer(doc, "t").angle, 30) && near(layer(doc, "s").screen->at("angle").get<double>(), 30));
            QVERIFY(near(layer(doc, "t").tone->at("gradient").at("angle").get<double>(), 15));
        }
        // a move and a scale: written as they were (an angle out of the range too)
        {
            const core::Document before = with_screens(-30, Json(400), 30);
            core::Document doc = before;
            pg::map_page(doc, 0, pg::mapping({2, 0, 0, 1.5, 3, 4}), {});
            QCOMPARE(layer(doc, "t").angle, -30.0);
            QCOMPARE(*layer(doc, "t").tone, *layer(before, "t").tone);
            QCOMPARE(*layer(doc, "s").screen, *layer(before, "s").screen);
        }
    }

    // A 3D prop moves and scales evenly with the page; a turn or a mirror is refused (render it to pixels first). A page
    // with pixels needs the drawing build's hooks.
    void propsAndPixels() {
        Json prim{{"kind", "cube"}, {"pos", Json::array({10.0, 20.0, 0.0})}, {"size", Json::array({5.0, 5.0, 5.0})}};
        pg::map_prim(prim, pg::mapping({2, 0, 0, 2, 1, 1}));
        QVERIFY(near(prim["pos"][0].get<double>(), 21) && near(prim["size"][0].get<double>(), 10));
        QVERIFY_THROWS_EXCEPTION(core::OpError, pg::map_prim(prim, pg::turn_mapping(pg::Turn::Cw90, 60, 80)));
        core::Document doc = small_book();
        core::Layer paint;
        paint.id = "p";
        paint.kind = core::LayerKind::Raster;
        paint.raster_png = std::make_shared<const std::string>("x");
        doc.edit_page(0).layers.push_back(paint);
        try {
            pg::map_page(doc, 0, pg::turn_mapping(pg::Turn::Cw90, 60, 80), {});
            QFAIL("pixels moved without the drawing build");
        } catch (const core::Error& error) {
            QCOMPARE(error.code(), std::string("not_yet_ported"));
        }
        int called = 0;
        pg::PixelHooks hooks;
        hooks.layer = [&](core::Layer&, const pg::Mapping&, const core::Page&, const core::Page&) { ++called; };
        pg::map_page(doc, 0, pg::turn_mapping(pg::Turn::Cw90, 60, 80), hooks);
        QCOMPARE(called, 1);
    }

    // geom/src/warp.rs: styles_parse, presets_at_bend_zero_are_identity, identity_mesh_is_exact_and_fit_reproduces_affine,
    // splits_are_exact_and_removable, arc_maps_known_points, preset_fits_closely_as_mesh
    void warps() {
        for (const char* name : {"none", "custom", "arc", "arcLower", "arcUpper", "arch", "bulge", "shellLower", "shellUpper", "flag", "wave",
                                 "fish", "rise", "fisheye", "inflate", "squeeze", "twist"}) {
            const auto style = pg::warp_style_from(name);
            QVERIFY(style.has_value());
            QCOMPARE(std::string(pg::to_string(*style)), std::string(name));
        }
        QVERIFY(!pg::warp_style_from("spiral").has_value());
        const std::array<double, 4> b{10, 20, 110, 70};
        for (const char* name : {"arc", "bulge", "twist", "wave"}) {
            pg::Warp w;
            w.style = *pg::warp_style_from(name);
            w.box = b;
            QVERIFY(w.is_identity());
            const auto p = w.map(33, 44);
            QVERIFY(p.x == 33 && p.y == 44);
        }
        const pg::BezierMesh id = pg::BezierMesh::identity(b, 1, 1);
        QCOMPARE(id.nx(), std::size_t(4));
        pg::Warp custom;
        custom.style = pg::WarpStyle::Custom;
        custom.box = b;
        custom.mesh = id;
        QVERIFY(custom.is_identity());
        const auto c = custom.map(37.5, 61.25);
        QVERIFY(near(c.x, 37.5, 1e-9) && near(c.y, 61.25, 1e-9));
        const auto f = [](double s, double t) { return core::vector::XY{3 + 2 * s - t, 1 + 0.5 * s + 4 * t}; };
        const pg::BezierMesh fitted = pg::BezierMesh::fit(f, {0, 1}, {0, 0.4, 1});
        for (const auto& [s, t] : {std::pair{0.1, 0.9}, std::pair{0.5, 0.2}, std::pair{0.77, 0.41}}) {
            const auto p = fitted.eval(s, t), e = f(s, t);
            QVERIFY(near(p.x, e.x, 1e-9) && near(p.y, e.y, 1e-9));
        }
        pg::BezierMesh m = pg::BezierMesh::identity(b, 1, 1);
        m.points[5] = {50, 10};
        std::vector<core::vector::XY> before;
        for (int j = 0; j <= 10; ++j)
            for (int i = 0; i <= 10; ++i) before.push_back(m.eval(i / 10.0, j / 10.0));
        const pg::BezierMesh orig = m;
        QVERIFY(m.split_u(0.3));
        QVERIFY(m.split_v(0.6));
        QVERIFY(!m.split_u(0.3));
        QCOMPARE(m.nx(), std::size_t(7));
        QCOMPARE(m.ny(), std::size_t(7));
        QVERIFY(m.valid());
        std::size_t k = 0;
        for (int j = 0; j <= 10; ++j)
            for (int i = 0; i <= 10; ++i, ++k) {
                const auto p = m.eval(i / 10.0, j / 10.0);
                QVERIFY(near(p.x, before[k].x, 1e-9) && near(p.y, before[k].y, 1e-9));
            }
        const auto [ls, lt] = m.param_at(m.eval(0.42, 0.77));
        QVERIFY(near(ls, 0.42, 1e-6) && near(lt, 0.77, 1e-6));
        QVERIFY(m.remove_split_u(1));
        QVERIFY(m.remove_split_v(1));
        QCOMPARE(m.us, orig.us);
        for (std::size_t i = 0; i < m.points.size(); ++i) QVERIFY(near(m.points[i].x, orig.points[i].x, 1e-9) && near(m.points[i].y, orig.points[i].y, 1e-9));
        pg::Warp arc;
        arc.style = pg::WarpStyle::Arc;
        arc.bend = 50;
        arc.box = {0, -40, 200, 10};
        const auto mid = arc.map(100, -15);
        QVERIFY(near(mid.x, 100, 1e-9) && near(mid.y, -15, 1e-9));
        const double theta = std::numbers::pi / 4, r = 4.0 / theta;
        const auto end = arc.map(0, -15);
        QVERIFY(near(end.x, 100 - r * std::sin(theta) * 100 / 4, 1e-6) && near(end.y, -15 + (r - r * std::cos(theta)) * 25, 1e-6));
        pg::Warp bulge;
        bulge.style = pg::WarpStyle::Bulge;
        bulge.bend = 40;
        bulge.box = b;
        pg::Warp as_mesh;
        as_mesh.style = pg::WarpStyle::Custom;
        as_mesh.box = b;
        as_mesh.mesh = bulge.to_mesh(4, 4);
        for (const auto& [x, y] : {std::pair{20.0, 30.0}, std::pair{60.0, 45.0}, std::pair{100.0, 65.0}}) {
            const auto p = bulge.map(x, y), q = as_mesh.map(x, y);
            QVERIFY(near(p.x, q.x, 0.5) && near(p.y, q.y, 0.5));
        }
    }
};

QTEST_GUILESS_MAIN(TestPageGeometry)
#include "test_page_geometry.moc"
