// pen-shapes@1 (PC3 C-b, SPEC.md COLOR-07「シェイプ」, schema-v4.md「ペンの層の形の部品（pen-shapes@1）」): a shape part as
// PhotoCraft v0.2.0's vector/src (CompiledShape) and its vector_cmds/tests.rs and presets/tests.rs check it — the live
// shapes' paths (a rectangle, its corners rounded, an ellipse, a polygon, a star, a triangle, a line drawn as a bar);
// the four path operations' coverage; a stroke inside, centred and outside, its dashes; the fill's gradient forms
// (angle, reflected, diamond, reverse); a part baked (Merge Shape Components) keeping its area, cut in two by an area
// (the halves' areas adding up to the whole), a band taken out by the eraser; a part bent by a map of the page (its
// anchors where a line's points go), through an affine map (its live shape kept or let go, its stroke scaled); the
// presets' path language, the built-in groups, a shape defined, renamed, deleted and the groups reset.

#include <QtTest>

#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"
#include "render/path_raster.hpp"
#include "render/shape_parts.hpp"
#include "render/shape_presets.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace pr = render::path_raster;
namespace sp = render::shape_parts;
namespace presets = render::shape_presets;
namespace vec = core::vector;

namespace {

constexpr double kDpi = 254.0;  // (10 px a mm)

// The path's filled area (mm²): its coverage summed on a grid of `dpi`.
double area_of(const vec::Path& path, double dpi = kDpi) {
    const auto b = path.control_bounds();
    if (!b) return 0;
    const double k = dpi / 25.4;
    const pr::PixelBox box{static_cast<int>(std::floor((*b)[0] * k)) - 2, static_cast<int>(std::floor((*b)[1] * k)) - 2,
                           static_cast<int>(std::ceil((*b)[2] * k)) + 2, static_cast<int>(std::ceil((*b)[3] * k)) + 2};
    const std::vector<float> cover = pr::path_coverage(pr::to_pixels(path, dpi), box);
    return std::accumulate(cover.begin(), cover.end(), 0.0) / (k * k);
}

vec::Path square(double x, double y, double w, vec::PathOp op = vec::PathOp::Combine) {
    vec::Path p = pr::rect_path(x, y, w, w);
    for (vec::Subpath& s : p.subpaths) s.op = op;
    return p;
}

vec::Path joined(vec::Path a, const vec::Path& b, vec::PathOp op) {
    for (vec::Subpath s : b.subpaths) {
        s.op = op;
        a.subpaths.push_back(std::move(s));
    }
    return a;
}

// The drawn picture's alpha summed (mm² of full cover) at 10 px a mm, over [x, y, x + w, y + h] mm.
double inked(const vec::ShapePart& part, double x, double y, double w, double h) {
    const render::Box area{static_cast<int>(x * 10), static_cast<int>(y * 10), static_cast<int>((x + w) * 10), static_cast<int>((y + h) * 10)};
    const render::Image picture = sp::picture(area, 254, {&part});
    const std::string bytes = picture.tobytes();
    double sum = 0;
    for (std::size_t i = 3; i < bytes.size(); i += 4) sum += static_cast<unsigned char>(bytes[i]) / 255.0;
    return sum / 100.0;
}

// The picture's red at (x, y) mm (10 px a mm).
int red_at(const vec::ShapePart& part, double x, double y) {
    const render::Box area{static_cast<int>(x * 10), static_cast<int>(y * 10), static_cast<int>(x * 10) + 1, static_cast<int>(y * 10) + 1};
    return static_cast<int>(sp::picture(area, 254, {&part}).getpixel(0, 0)[0]);
}

vec::ShapePart part_of(vec::Path path, Json fill = Json{{"rgb", Json::array({0, 0, 0})}}) {
    vec::ShapePart part;
    part.id = "s1";
    part.path = std::move(path);
    part.fill = std::move(fill);
    return part;
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

}  // namespace

class TestShapes : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // The live shapes' paths (PhotoCraft's vector/src/shapes.rs): the areas a rectangle, a rounded one, an ellipse, a
    // polygon, a star, a triangle and a line's bar cover.
    void liveShapes() {
        vec::LiveShape live;
        live.rect = {10, 10, 40, 20};
        QVERIFY(near(area_of(pr::live_path(live)), 800, 0.5));
        live.radii = {5, 5, 5, 5};  // (each corner a quarter circle less than its square)
        QVERIFY(near(area_of(pr::live_path(live)), 800 - 4 * (25 - std::numbers::pi * 25 / 4), 0.8));
        live = vec::LiveShape{};
        live.kind = vec::LiveShape::Kind::Ellipse;
        live.rect = {0, 0, 40, 20};
        QVERIFY(near(area_of(pr::live_path(live)), std::numbers::pi * 20 * 10, 2.0));  // (Bézier quarters: within 0.03 %)
        live.kind = vec::LiveShape::Kind::Polygon;
        live.rect = {0, 0, 20, 20};
        live.sides = 6;
        const double hexagon = area_of(pr::live_path(live));
        QVERIFY2(near(hexagon, 3 * std::sqrt(3.0) / 2 * 100, 1.0), std::to_string(hexagon).c_str());  // (inscribed in the box's circle)
        live.star_ratio = 0.5;
        const vec::Path star = pr::live_path(live);
        QCOMPARE(star.subpaths[0].knots.size(), std::size_t{12});  // (an inner point between each two)
        QVERIFY(area_of(star) < hexagon);
        live.sides = 3;
        live.star_ratio = 1.0;
        QCOMPARE(pr::live_path(live).subpaths[0].knots.size(), std::size_t{3});
        live = vec::LiveShape{};
        live.kind = vec::LiveShape::Kind::Line;
        live.from = {0, 0};
        live.to = {30, 40};
        live.weight_mm = 2;
        QVERIFY(near(area_of(pr::live_path(live)), 50 * 2, 0.5));  // (a bar 50 mm long, 2 mm thick)
    }

    // The path operations of a part's subpaths (PhotoCraft's vector/src/tests.rs): two squares overlapping by half.
    void pathOperations() {
        const vec::Path a = square(0, 0, 10);
        const vec::Path b = square(5, 0, 10);
        QVERIFY(near(area_of(joined(a, b, vec::PathOp::Combine)), 150, 0.5));
        QVERIFY(near(area_of(joined(a, b, vec::PathOp::Subtract)), 50, 0.5));
        QVERIFY(near(area_of(joined(a, b, vec::PathOp::Intersect)), 50, 0.5));
        QVERIFY(near(area_of(joined(a, b, vec::PathOp::Exclude)), 100, 0.5));
        // baked (Merge Shape Components): one plain outline of the same area
        for (const vec::PathOp op : {vec::PathOp::Combine, vec::PathOp::Subtract, vec::PathOp::Intersect, vec::PathOp::Exclude}) {
            const vec::Path both = joined(a, b, op);
            const vec::Path baked = sp::baked(both);
            QVERIFY(!baked.empty());
            for (std::size_t i = 0; i < baked.subpaths.size(); ++i) QVERIFY(i == 0 || baked.subpaths[i].op == vec::PathOp::Exclude);
            QVERIFY2(near(area_of(baked), area_of(both), 0.3), std::to_string(area_of(baked) - area_of(both)).c_str());
        }
    }

    // A stroke (Photoshop's shape stroke): centred straddles the outline, inside stays in the fill, outside out of it;
    // a part's opacity on the whole; dashes leave gaps.
    void strokes() {
        vec::ShapePart part = part_of(pr::rect_path(10, 10, 20, 20), nullptr);
        part.stroke = vec::ShapeStroke{};
        part.stroke->width_mm = 2;
        part.stroke->fill = Json{{"rgb", Json::array({0, 0, 0})}};
        part.stroke->align = vec::StrokeAlign::Center;
        QVERIFY(near(inked(part, 0, 0, 40, 40), 22 * 22 - 18 * 18, 1.0));
        part.stroke->align = vec::StrokeAlign::Inside;
        QVERIFY(near(inked(part, 0, 0, 40, 40), 20 * 20 - 16 * 16, 1.0));
        part.stroke->align = vec::StrokeAlign::Outside;
        QVERIFY(near(inked(part, 0, 0, 40, 40), 24 * 24 - 20 * 20, 1.0));
        part.opacity = 0.5;
        QVERIFY(near(inked(part, 0, 0, 40, 40), (24 * 24 - 20 * 20) / 2.0, 1.0));
        part.opacity = 1;
        part.stroke->align = vec::StrokeAlign::Center;
        part.stroke->dashes = {2, 2};  // (in widths: 4 mm on, 4 mm off)
        const double dashed = inked(part, 0, 0, 40, 40);
        QVERIFY2(dashed > 0.3 * 160 && dashed < 0.7 * 160, std::to_string(dashed).c_str());
        // a fill under its stroke: the fill's colour inside, the stroke's on the outline
        vec::ShapePart filled = part_of(pr::rect_path(10, 10, 20, 20), Json{{"rgb", Json::array({255, 0, 0})}});
        filled.stroke = vec::ShapeStroke{};
        filled.stroke->width_mm = 2;
        filled.stroke->fill = Json{{"rgb", Json::array({0, 0, 0})}};
        QCOMPARE(red_at(filled, 20, 20), 255);
        QCOMPARE(red_at(filled, 10.05, 20), 0);
        // the layer's ink (rgb null)
        vec::ShapePart ink = part_of(pr::rect_path(10, 10, 20, 20), Json{{"rgb", nullptr}});
        QCOMPARE(red_at(ink, 20, 20), static_cast<int>(sp::kInkColour[0]));
    }

    // A fill's gradient (a fill layer's, render/composite): the new forms of PC3 C-b — reflected (the same both ways from
    // its start), diamond (the same on its axes), angle (round its start), reverse (its colours the other way).
    void gradientForms() {
        const auto with = [](const char* shape, bool reverse) {
            Json g{{"shape", shape}, {"from", Json::array({50, 50})}, {"to", Json::array({90, 50})}, {"rgb_from", Json::array({0, 0, 0})},
                   {"rgb_to", Json::array({255, 255, 255})}};
            if (reverse) g["reverse"] = true;
            return part_of(pr::rect_path(0, 0, 100, 100), Json{{"gradient", g}});
        };
        const vec::ShapePart reflected = with("reflected", false);
        QVERIFY(near(red_at(reflected, 30, 50), red_at(reflected, 70, 50), 2));
        QVERIFY(red_at(reflected, 50.05, 50) < 10 && red_at(reflected, 89, 50) > 240);
        const vec::ShapePart diamond = with("diamond", false);
        QVERIFY(near(red_at(diamond, 70, 50), red_at(diamond, 50, 70), 2));
        QVERIFY(near(red_at(diamond, 60, 60), red_at(diamond, 70, 50), 3));  // (|dx| + |dy| alike)
        const vec::ShapePart angle = with("angle", false);
        QVERIFY(red_at(angle, 50, 30) != red_at(angle, 50, 70));  // (round its start)
        const vec::ShapePart linear = with("linear", false);
        const vec::ShapePart back = with("linear", true);
        QVERIFY(near(red_at(linear, 60, 50) + red_at(back, 60, 50), 255, 3));
    }

    // A part cut in two by an area (a selection's edge: render/selection lift): the halves' areas add up to the whole;
    // the eraser's band taken out: the band's area less.
    void cutAndErased() {
        vec::LiveShape live;
        live.kind = vec::LiveShape::Kind::Ellipse;
        live.rect = {0, 0, 30, 20};
        const vec::Path oval = pr::live_path(live);
        const vec::Path area = pr::rect_path(15, -5, 30, 40);
        const vec::Path in = sp::cut(oval, area, true);
        const vec::Path out = sp::cut(oval, area, false);
        const double whole = area_of(oval);
        QVERIFY2(near(area_of(in) + area_of(out), whole, 0.5), (std::to_string(area_of(in)) + " " + std::to_string(area_of(out))).c_str());
        QVERIFY(near(area_of(in), whole / 2, 0.5));
        const auto [inside, outside] = sp::shares(oval, area);
        QVERIFY(near(inside, whole / 2, 0.5) && near(outside, whole / 2, 0.5));
        // a band 2 mm wide across a 20 mm square
        const vec::Path box = pr::rect_path(0, 0, 20, 20);
        const std::vector<std::vector<pr::XY>> band{{{9, -5}, {11, -5}, {11, 25}, {9, 25}}};
        const vec::Path less = sp::without(box, band);
        QVERIFY2(near(area_of(less), 400 - 40, 0.5), std::to_string(area_of(less)).c_str());
        QCOMPARE(less.subpaths.size(), std::size_t{2});  // (two pieces apart)
    }

    // Bent by a map of the page (ゆがみ, a warp: render/warp's way with a pen line's points): every anchor where the map
    // takes it; through an affine map the live shape kept when the axes are (a move, a scale), the stroke scaled by
    // √|det| when asked.
    void bentAndTransformed() {
        const vec::Path box = pr::rect_path(0, 0, 10, 10);
        const auto go = [](double x, double y) { return std::pair<double, double>{x + 0.1 * y * y, y}; };
        const vec::Path bent = sp::bent(box, go, 1.5);
        QVERIFY(bent.subpaths[0].knots.size() > box.subpaths[0].knots.size());  // (each side in pieces of 1.5 mm at most)
        for (const vec::Knot& k : box.subpaths[0].knots) {
            const auto [x, y] = go(k.anchor.x, k.anchor.y);
            bool found = false;
            for (const vec::Knot& b : bent.subpaths[0].knots) found = found || (near(b.anchor.x, x, 1e-6) && near(b.anchor.y, y, 1e-6));
            QVERIFY(found);
        }
        vec::ShapePart part = part_of(box);
        vec::LiveShape live;
        live.rect = {0, 0, 10, 10};
        part.live = live;
        part.stroke = vec::ShapeStroke{};
        part.stroke->width_mm = 1;
        const vec::ShapePart moved = sp::transformed(part, {2, 0, 0, 2, 5, 5}, true);
        QVERIFY(moved.live.has_value());
        QVERIFY((moved.live->rect == std::array<double, 4>{5, 5, 20, 20}));
        QVERIFY(near(moved.stroke->width_mm, 2, 1e-9));
        QCOMPARE(sp::transformed(part, {2, 0, 0, 2, 5, 5}, false).stroke->width_mm, 1.0);
        const double t = std::numbers::pi / 6;  // (a turn of 30°: no rectangle any more)
        const vec::ShapePart turned = sp::transformed(part, {std::cos(t), std::sin(t), -std::sin(t), std::cos(t), 0, 0}, true);
        QVERIFY(!turned.live.has_value());
        QVERIFY(near(area_of(turned.path), 100, 0.3));
    }

    // The presets' path language (PhotoCraft's presets/tests.rs): lines, cubics, quadratics, a circle, a hole; what is
    // not well formed refused; the built-in groups all read and lie in the 100 box; a shape fitted into a rect.
    void presetLanguage() {
        const vec::Path tri = presets::parse("M 50 0 L 100 100 L 0 100 Z");
        QCOMPARE(tri.subpaths.size(), std::size_t{1});
        QVERIFY(tri.subpaths[0].closed);
        QCOMPARE(tri.subpaths[0].knots.size(), std::size_t{3});
        QVERIFY(near(area_of(tri), 5000, 2));
        const vec::Path ring = presets::parse("O 50 50 50 !O 50 50 25");
        QCOMPARE(ring.subpaths.size(), std::size_t{2});
        QVERIFY(ring.subpaths[1].op == vec::PathOp::Subtract);
        QVERIFY(near(area_of(ring), std::numbers::pi * (2500 - 625), 8));
        const vec::Path curve = presets::parse("M 0 0 C 30 0 70 0 100 0 Q 100 50 100 100");
        QVERIFY(curve.subpaths[0].closed);  // (a preset is a shape to fill: every subpath closed, Z or not)
        QCOMPARE(curve.subpaths[0].knots.size(), std::size_t{3});
        for (const char* bad : {"M 0", "L 10 10", "X 1 2", "M 0 0 C 1 2 3"}) {
            bool refused = false;
            try {
                (void)presets::parse(bad);
            } catch (const core::Error&) {
                refused = true;
            }
            QVERIFY2(refused, bad);
        }
        const auto groups = presets::builtin();
        QVERIFY(groups.size() >= 5);
        for (const presets::Group& g : groups) {
            QVERIFY(!g.shapes.empty());
            for (const presets::Preset& p : g.shapes) {
                // (what it covers lies in the 100 box: its combining subpaths; one that cuts — a crescent's — may reach out)
                vec::Path drawn = p.path;
                std::erase_if(drawn.subpaths, [](const vec::Subpath& s) { return s.op != vec::PathOp::Combine; });
                const auto b = drawn.control_bounds();
                QVERIFY2(b && (*b)[0] >= -0.01 && (*b)[1] >= -0.01 && (*b)[2] <= 100.01 && (*b)[3] <= 100.01, p.name.c_str());
                QVERIFY2(area_of(p.path, 25.4) > 0, p.name.c_str());
            }
        }
        const vec::Path fitted = presets::fit(tri, {10, 20, 40, 20}, true);  // (kept alike both ways: 20 × 20 in the middle)
        const auto fb = fitted.control_bounds();
        QVERIFY(near((*fb)[0], 20, 1e-6) && near((*fb)[2], 40, 1e-6) && near((*fb)[1], 20, 1e-6) && near((*fb)[3], 40, 1e-6));
        const auto sb = presets::fit(tri, {10, 20, 40, 20}, false).control_bounds();
        QVERIFY(near((*sb)[0], 10, 1e-6) && near((*sb)[2], 50, 1e-6));
    }

    // The presets kept in the settings folder (shapes.json): a shape defined (its name taken: a number after it),
    // renamed, deleted; the groups reset; the file read back; a file not well formed: the built-in groups.
    void presetsKept() {
        const std::filesystem::path dir = core::path_from_utf8(tmp_.path().toStdString() + "/config");
        std::filesystem::create_directories(dir);
        std::vector<presets::Group> groups = presets::load(dir);
        QCOMPARE(groups.size(), presets::builtin().size());
        const vec::Path tri = presets::parse("M 50 0 L 100 100 L 0 100 Z");
        Json made = presets::add(groups, "三角", std::nullopt, tri);
        QCOMPARE(made["group"], Json(std::string(presets::kCustomGroup)));
        QCOMPARE(made["name"], Json("三角"));
        made = presets::add(groups, "三角", std::nullopt, tri);
        QCOMPARE(made["name"], Json("三角 2"));
        presets::edit(groups, Json{{"action", "rename"}, {"preset", "三角 2"}, {"name", "山"}});
        QVERIFY(presets::find(groups, "山") != nullptr);
        presets::edit(groups, Json{{"action", "delete"}, {"preset", "山"}});
        QVERIFY(presets::find(groups, "山") == nullptr);
        presets::save(dir, groups);
        const auto again = presets::load(dir);
        QVERIFY(presets::find(again, "三角", std::string(presets::kCustomGroup)) != nullptr);
        QCOMPARE(presets::list(again)["groups"].size(), again.size());
        const auto reset = presets::reset(again, false);
        QCOMPARE(reset.size(), presets::builtin().size());
        QVERIFY(presets::find(reset, "三角") == nullptr);
        QVERIFY(presets::find(presets::reset(again, true), "三角") != nullptr);  // (append: the person's kept)
        genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(dir / "shapes.json")), "{not json");
        std::string problem;
        QCOMPARE(presets::load(dir, &problem).size(), presets::builtin().size());
        QVERIFY(!problem.empty());
        const render::Image swatch = presets::thumbnail(tri, 32);
        QCOMPARE(swatch.size().width, 32);
        QVERIFY(swatch.getpixel(16, 20)[0] > 200 && swatch.getpixel(1, 1)[0] == 0);
    }
};

QTEST_GUILESS_MAIN(TestShapes)
#include "test_shapes.moc"
