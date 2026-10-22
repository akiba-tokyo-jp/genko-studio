// PC3 C-0 (SPEC.md COLOR-07, schema-v4.md「パス・ベクトル（vector@1）」「ペンの層の形の部品（pen-shapes@1）」): a path's
// coverage as PhotoCraft's vector/src/tests.rs and its modules' tests check it (a circle's and a rectangle's area, turned
// too; the same coverage for any part of the plane; even-odd and non-zero; the four path operations, the first always
// combining, inverted; stroke widths, caps, joins, the miter limit and dashes; flattening within its tolerance; the live
// shapes' knots; a vector mask's values), and the vector data's JSON and the book's frames for it: written in their
// normal form, read back, refused when not well formed, a book without them the same bytes and features as before.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/vector.hpp"
#include "render/path_raster.hpp"
#include "render/path_trace.hpp"
#include "render/psd_paths.hpp"
#include "render/vector_masks.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace pr = render::path_raster;
namespace vec = core::vector;

namespace {

double area(const std::vector<float>& v) {
    double s = 0;
    for (const float x : v) s += x;
    return s;
}

std::vector<float> cov(const vec::Path& path, const pr::PixelBox& box) { return pr::path_coverage(path, box); }

vec::Subpath polygon(const std::vector<vec::XY>& pts, vec::PathOp op = vec::PathOp::Combine, bool closed = true) {
    vec::Subpath s;
    s.closed = closed;
    s.op = op;
    for (const auto& p : pts) s.knots.push_back(vec::Knot::corner(p.x, p.y));
    return s;
}

vec::Path one(vec::Subpath s) {
    vec::Path p;
    p.subpaths.push_back(std::move(s));
    return p;
}

vec::Path pentagram() {
    std::vector<vec::XY> pts;
    for (int i = 0; i < 5; ++i) {
        const double a = -std::numbers::pi / 2 + 2 * std::numbers::pi * ((i * 2) % 5) / 5.0;
        pts.push_back({50.0 + 40.0 * std::cos(a), 50.0 + 40.0 * std::sin(a)});
    }
    return one(polygon(pts));
}

vec::Path two_squares(vec::PathOp op) {
    vec::Path p = pr::rect_path(10, 10, 40, 40);
    vec::Subpath second = pr::rect_path(30, 30, 40, 40).subpaths[0];
    second.op = op;
    p.subpaths.push_back(second);
    return p;
}

double stroke_area(const vec::Path& path, const pr::StrokeStyle& st) {
    return area(pr::stroke_rasterizer(path, st).render(pr::PixelBox{-50, -50, 250, 250}));
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("pc3", core::Num(1), 1, spec, core::Binding::Right);
    return doc;
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::string text_of(const fs::path& file) { return test::read_bytes(QString::fromStdString(core::path_to_utf8(file))); }

}  // namespace

class TestPathRaster : public QObject {
    Q_OBJECT

private slots:
    // raster.rs: axis_aligned_square_exact, clipped_rect_matches_full_render, inverted_empty_is_full
    void rasterizer() {
        pr::Rasterizer r(false);
        r.add_component({{{2.25, 3.5}, {6.25, 3.5}, {6.25, 7.5}, {2.25, 7.5}}}, vec::PathOp::Combine, vec::FillRule::NonZero);
        const auto c = r.render(pr::PixelBox{0, 0, 10, 10});
        QVERIFY(std::fabs(area(c) - 16.0) < 1e-4);
        QVERIFY(std::fabs(c[3 * 10 + 2] - 0.75 * 0.5) < 1e-5);
        QVERIFY(std::fabs(c[5 * 10 + 4] - 1.0) < 1e-6);
        QCOMPARE(c[0], 0.0f);
        pr::Rasterizer t(false);
        t.add_component({{{1.3, 0.7}, {17.9, 5.2}, {8.1, 19.6}}}, vec::PathOp::Combine, vec::FillRule::NonZero);
        const auto full = t.render(pr::PixelBox{0, 0, 20, 20});
        const auto part = t.render(pr::PixelBox{7, 4, 13, 15});
        for (int y = 4; y < 15; ++y)
            for (int x = 7; x < 13; ++x) QVERIFY(std::fabs(full[std::size_t(y * 20 + x)] - part[std::size_t((y - 4) * 6 + (x - 7))]) < 1e-5f);
        const double tri = 0.5 * std::fabs((17.9 - 1.3) * (19.6 - 0.7) - (8.1 - 1.3) * (5.2 - 0.7));
        QVERIFY(std::fabs(area(full) - tri) < 1e-3);
        const pr::Rasterizer inverted(true);
        const auto all = inverted.render(pr::PixelBox{0, 0, 4, 4});
        QVERIFY(std::all_of(all.begin(), all.end(), [](float v) { return v == 1.0f; }));
    }

    // tests.rs: circle_area_matches_analytic, rect_area_exact_and_rotated, coverage_is_tile_independent
    void areas() {
        for (const double r : {3.3, 10.0, 47.5, 200.25}) {
            const double c = 256.0 + 0.37;
            const double got = area(cov(pr::ellipse_path(c - r, c - r, 2 * r, 2 * r), pr::PixelBox{0, 0, 512, 512}));
            const double want = std::numbers::pi * r * r;
            QVERIFY2(std::fabs(got - want) / want < 0.005, qPrintable(QString::number(r)));
            if (r >= 10.0) QVERIFY(std::fabs(got - want) / want < 0.0015);
        }
        const vec::Path p = pr::rect_path(10.3, 20.7, 100.45, 50.2);
        QVERIFY(std::fabs(area(cov(p, pr::PixelBox{0, 0, 200, 100})) - 100.45 * 50.2) < 1e-2);
        const double a = 0.4, cs = std::cos(a), sn = std::sin(a);
        const vec::Path rot = p.transformed({cs, sn, -sn, cs, 60.0, 30.0});
        QVERIFY(std::fabs(area(cov(rot, pr::PixelBox{-100, -100, 400, 400})) - 100.45 * 50.2) / (100.45 * 50.2) < 1e-4);
        const vec::Path star = pr::polygon_path(3, 5, 90, 80, 7, 0.45);
        const auto whole = cov(star, pr::PixelBox{0, 0, 100, 100});
        for (const auto& [x0, y0] : {std::pair{0, 0}, std::pair{13, 29}, std::pair{50, 50}}) {
            const auto part = cov(star, pr::PixelBox{x0, y0, x0 + 37, y0 + 23});
            for (int y = y0; y < y0 + 23; ++y)
                for (int x = x0; x < x0 + 37; ++x) QVERIFY(std::fabs(whole[std::size_t(y * 100 + x)] - part[std::size_t((y - y0) * 37 + (x - x0))]) < 1e-5f);
        }
    }

    // tests.rs: even_odd_vs_non_zero, boolean_ops_areas
    void rulesAndOperations() {
        const pr::PixelBox r{0, 0, 100, 100};
        const auto nz = cov(pentagram(), r);
        vec::Path eo_path = pentagram();
        eo_path.fill_rule = vec::FillRule::EvenOdd;
        const auto eo = cov(eo_path, r);
        QCOMPARE(nz[50 * 100 + 50], 1.0f);
        QCOMPARE(eo[50 * 100 + 50], 0.0f);
        QCOMPARE(nz[16 * 100 + 50], 1.0f);
        QCOMPARE(eo[16 * 100 + 50], 1.0f);
        const double r_in = 40.0 * std::cos(72.0 * std::numbers::pi / 180) / std::cos(36.0 * std::numbers::pi / 180);
        const double inner = 2.5 * r_in * r_in * std::sin(72.0 * std::numbers::pi / 180);
        QVERIFY(std::fabs((area(nz) - area(eo)) - inner) / inner < 0.01);
        const auto a = [&](vec::PathOp op) { return area(cov(two_squares(op), r)); };
        QVERIFY(std::fabs(a(vec::PathOp::Combine) - (1600.0 * 2 - 400)) < 1e-2);
        QVERIFY(std::fabs(a(vec::PathOp::Subtract) - 1200.0) < 1e-2);
        QVERIFY(std::fabs(a(vec::PathOp::Intersect) - 400.0) < 1e-2);
        QVERIFY(std::fabs(a(vec::PathOp::Exclude) - 2400.0) < 1e-2);
        vec::Path first = two_squares(vec::PathOp::Combine);
        first.subpaths[0].op = vec::PathOp::Subtract;  // (the first always combines)
        QVERIFY(std::fabs(area(cov(first, r)) - 2800.0) < 1e-2);
        vec::Path inv = two_squares(vec::PathOp::Intersect);
        inv.inverted = true;
        QVERIFY(std::fabs(area(cov(inv, r)) - (10000.0 - 400)) < 1e-2);
        vec::Path donut = pr::ellipse_path(10, 10, 80, 80);
        vec::Subpath hole = pr::ellipse_path(30, 30, 40, 40).subpaths[0];
        hole.op = vec::PathOp::Subtract;
        donut.subpaths.push_back(hole);
        const double want = std::numbers::pi * (1600.0 - 400.0);
        QVERIFY(std::fabs(area(cov(donut, r)) - want) / want < 0.002);
    }

    // tests.rs: stroke_widths_caps_and_joins, closed_rect_stroke, dashes_cover_expected_fraction; stroke.rs:
    // dashes_split_by_length, pieces_are_positive
    void strokes() {
        const vec::Path line = one(polygon({{20, 50}, {120, 50}}, vec::PathOp::Combine, false));
        const auto st = [](double w, vec::LineCap cap) {
            pr::StrokeStyle s;
            s.width = w;
            s.cap = cap;
            return s;
        };
        for (const double w : {1.0, 2.5, 10.0}) QVERIFY(std::fabs(stroke_area(line, st(w, vec::LineCap::Butt)) - 100.0 * w) < 1e-2);
        QVERIFY(std::fabs(stroke_area(line, st(10, vec::LineCap::Square)) - 1100.0) < 1e-2);
        QVERIFY(std::fabs(stroke_area(line, st(10, vec::LineCap::Round)) - (1000.0 + std::numbers::pi * 25.0)) < 0.5);
        const vec::Path corner = one(polygon({{20, 20}, {120, 20}, {120, 120}}, vec::PathOp::Combine, false));
        const auto j = [&](vec::LineJoin join) {
            pr::StrokeStyle s;
            s.width = 10;
            s.join = join;
            return stroke_area(corner, s);
        };
        const double base = 2.0 * 100.0 * 10.0;
        QVERIFY(std::fabs(j(vec::LineJoin::Miter) - base) < 1e-2);
        QVERIFY(std::fabs(j(vec::LineJoin::Bevel) - (base - 12.5)) < 1e-2);
        QVERIFY(std::fabs(j(vec::LineJoin::Round) - (base - 25.0 + std::numbers::pi * 25.0 / 4.0)) < 0.1);
        const vec::Path sharp = one(polygon({{0, 0}, {100, 5}, {0, 10}}, vec::PathOp::Combine, false));
        const auto lim = [&](double m) {
            pr::StrokeStyle s;
            s.width = 4;
            s.miter_limit = m;
            return stroke_area(sharp, s);
        };
        QVERIFY(lim(100) > lim(4) + 10);
        pr::StrokeStyle ten;
        ten.width = 10;
        QVERIFY(std::fabs(stroke_area(pr::rect_path(20, 20, 100, 100), ten) - (110.0 * 110.0 - 90.0 * 90.0)) < 1e-2);
        const vec::Path dashed_line = one(polygon({{0, 50}, {120, 50}}, vec::PathOp::Combine, false));
        pr::StrokeStyle dash;
        dash.width = 4;
        dash.dashes = {8, 4};
        QVERIFY(std::fabs(stroke_area(dashed_line, dash) - 320.0) < 1e-2);
        vec::ShapeStroke shape_stroke;
        shape_stroke.width_mm = 4;
        shape_stroke.dashes = {2, 1};
        QCOMPARE(pr::stroke_style(shape_stroke, 1.0).dashes, (std::vector<double>{8, 4}));
        pr::StrokeStyle dots;
        dots.width = 4;
        dots.cap = vec::LineCap::Round;
        dots.dashes = {0, 12};
        const double one_dot = std::numbers::pi * 4.0;
        const double d = stroke_area(dashed_line, dots);
        QVERIFY(d > 9 * one_dot && d < 11.5 * one_dot);
        pr::Polyline pl{{{0, 0}, {10, 0}}, {true, true}, false};
        const auto parts = pr::dash_polyline(pl, {2, 1}, 0);
        QCOMPARE(parts.size(), std::size_t(4));
        QVERIFY(std::fabs(parts[1].pts.front().first - 3.0) < 1e-9 && std::fabs(parts[1].pts.back().first - 5.0) < 1e-9);
        QVERIFY(std::fabs(pr::dash_polyline(pl, {2, 1}, 1)[0].pts.back().first - 1.0) < 1e-9);
        const pr::Polyline bend{{{0, 0}, {10, 0}, {10, 10}}, {true, true, true}, false};
        for (const auto join : {vec::LineJoin::Miter, vec::LineJoin::Round, vec::LineJoin::Bevel}) {
            for (const auto cap : {vec::LineCap::Butt, vec::LineCap::Round, vec::LineCap::Square}) {
                pr::StrokeStyle s;
                s.width = 2;
                s.join = join;
                s.cap = cap;
                for (const auto& piece : pr::stroke_polygons({bend}, s, 0.1)) {
                    double sum = 0;
                    for (std::size_t i = 0; i < piece.size(); ++i) {
                        const auto& p = piece[i];
                        const auto& q = piece[(i + 1) % piece.size()];
                        sum += p.first * q.second - p.second * q.first;
                    }
                    QVERIFY(sum > 0);
                }
            }
        }
    }

    // flatten.rs: lines_are_not_subdivided, circle_flattening_respects_tolerance; shapes.rs: rounded_rect_knots,
    // star_vertices; a live shape's path made again
    void flattenAndShapes() {
        const auto p = pr::flatten_subpath(polygon({{0, 0}, {10, 0}, {10, 10}}), 0.1);
        QCOMPARE(p.pts.size(), std::size_t(3));
        QVERIFY(std::all_of(p.knot.begin(), p.knot.end(), [](bool k) { return k; }));
        for (const double tol : {1.0, 0.25, 0.01}) {
            const auto f = pr::flatten_subpath(pr::ellipse_path(0, 0, 200, 200).subpaths[0], tol);
            for (std::size_t i = 0; i + 1 < f.pts.size(); ++i) {
                const double mx = (f.pts[i].first + f.pts[i + 1].first) / 2 - 100, my = (f.pts[i].second + f.pts[i + 1].second) / 2 - 100;
                QVERIFY(100.0 - std::hypot(mx, my) <= tol + 0.03);
            }
        }
        QCOMPARE(pr::rounded_rect(0, 0, 100, 50, {10, 10, 10, 10}).subpaths[0].knots.size(), std::size_t(8));
        QCOMPARE(pr::rounded_rect(0, 0, 100, 50, {0, 0, 0, 0}).subpaths[0].knots.size(), std::size_t(4));
        QCOMPARE(pr::rounded_rect(0, 0, 100, 50, {10, 0, 10, 0}).subpaths[0].knots.size(), std::size_t(6));
        QCOMPARE(pr::clamp_radii(10, 10, {10, 10, 10, 10}), (std::array<double, 4>{5, 5, 5, 5}));
        QCOMPARE(pr::polygon_path(0, 0, 10, 10, 5, 0.5).subpaths[0].knots.size(), std::size_t(10));
        QCOMPARE(pr::polygon_path(0, 0, 10, 10, 6, 1.0).subpaths[0].knots.size(), std::size_t(6));
        const auto top = pr::polygon_path(0, 0, 10, 10, 3, 1.0).subpaths[0].knots[0].anchor;
        QVERIFY(std::fabs(top.x - 5) < 1e-9 && std::fabs(top.y) < 1e-9);
        vec::LiveShape live;
        live.kind = vec::LiveShape::Kind::Polygon;
        live.rect = {0, 0, 10, 10};
        live.sides = 4;
        live.angle = 45;
        const auto turned = pr::live_path(live).subpaths[0].knots[0].anchor;  // (the top vertex turned 45° clockwise)
        QVERIFY(std::fabs(turned.x - (5 + 5 * std::sqrt(0.5))) < 1e-9 && std::fabs(turned.y - (5 - 5 * std::sqrt(0.5))) < 1e-9);
        vec::LiveShape bar;
        bar.kind = vec::LiveShape::Kind::Line;
        bar.from = {0, 5};
        bar.to = {10, 5};
        bar.weight_mm = 2;
        QVERIFY(std::fabs(area(cov(pr::live_path(bar), pr::PixelBox{0, 0, 20, 20})) - 20.0) < 1e-6);
    }

    // tests.rs: coverage_surface_and_vector_mask_values; a path's mask over a page in mm
    void vectorMasks() {
        vec::VectorMask m;
        m.path = pr::rect_path(10, 10, 20, 20);
        m.density = 0.25;
        const auto v = pr::vector_mask_values(m, pr::PixelBox{0, 0, 40, 40});
        QVERIFY(std::fabs(v[0] - 0.75f) < 1e-6f && std::fabs(v[15 * 40 + 15] - 1.0f) < 1e-6f);
        m.enabled = false;
        const auto off = pr::vector_mask_values(m, pr::PixelBox{0, 0, 4, 4});
        QVERIFY(std::all_of(off.begin(), off.end(), [](float x) { return x == 1.0f; }));
        vec::VectorMask empty;
        const auto all = pr::vector_mask_values(empty, pr::PixelBox{0, 0, 4, 4});
        QVERIFY(std::all_of(all.begin(), all.end(), [](float x) { return x == 1.0f; }));
        empty.path.inverted = true;
        const auto none = pr::vector_mask_values(empty, pr::PixelBox{0, 0, 4, 4});
        QVERIFY(std::all_of(none.begin(), none.end(), [](float x) { return x == 0.0f; }));
        const render::Image mask = pr::path_mask(pr::rect_path(25.4, 25.4, 25.4, 25.4), 100, render::Size{200, 200});
        QCOMPARE(static_cast<int>(mask.getpixel(150, 150)[0]), 255);
        QCOMPARE(static_cast<int>(mask.getpixel(99, 150)[0]), 0);
        QCOMPARE(static_cast<int>(mask.getpixel(100, 150)[0]), 255);
    }

    // The JSON: a path's normal form (a corner as [x, y], defaults left out, 4 decimals, the numbers as the book's other mm),
    // read back the same; what is
    // not well formed refused; a shape part and its stroke and live shape; the shapes blob.
    void json() {
        vec::Path p;
        vec::Subpath s = polygon({{1.23456, 2}, {5, 2}, {5, 9}});
        s.knots[1] = vec::Knot{{5, 2}, {4, 2}, {5, 3}, true};
        p.subpaths.push_back(s);
        vec::Subpath second = polygon({{0, 0}, {1, 0}, {0, 1}}, vec::PathOp::Exclude, false);
        p.subpaths.push_back(second);
        const Json j = vec::to_json(p);
        QCOMPARE(core::dump_python(j["subpaths"][0]["knots"][0]), std::string("[1.2346, 2.0]"));
        QCOMPARE(core::dump_python(j["subpaths"][0]["knots"][1]), std::string(R"({"anchor": [5.0, 2.0], "in": [4.0, 2.0], "out": [5.0, 3.0], "smooth": true})"));
        QCOMPARE(j["subpaths"][1]["op"].get<std::string>(), std::string("exclude"));
        const vec::Path back = vec::path_from_json(j);
        QCOMPARE(vec::to_json(back), j);
        QCOMPARE(back.subpaths[0].knots[1], s.knots[1]);
        QVERIFY(!back.subpaths[1].closed);
        QCOMPARE(back.effective_op(0), vec::PathOp::Combine);
        for (const char* bad : {R"({"subpaths": [{"knots": [[1]]}]})", R"({"subpaths": [{"knots": [[1, 2]], "op": "xor"}]})",
                                R"({"subpaths": [], "mystery": 1})", R"({"subpaths": [{"knots": [[1e9, 2]]}]})", R"({"fill_rule": "evenodd"})"}) {
            QVERIFY_THROWS_EXCEPTION(core::Error, vec::path_from_json(core::parse_python_json(bad)));
        }
        vec::ShapePart part;
        part.id = "sp1";
        part.name = "形 1";
        part.path = pr::rect_path(1, 2, 3, 4);
        part.fill = Json{{"rgb", Json::array({10, 20, 30})}};
        vec::ShapeStroke stroke;
        stroke.width_mm = 0.5;
        stroke.align = vec::StrokeAlign::Inside;
        stroke.dashes = {2, 1};
        part.stroke = stroke;
        vec::LiveShape live;
        live.rect = {1, 2, 3, 4};
        part.live = live;
        part.over = true;
        const vec::ShapePart again = vec::part_from_json(vec::to_json(part));
        QCOMPARE(again, part);
        QVERIFY_THROWS_EXCEPTION(core::Error, vec::part_from_json(core::parse_python_json(R"({"id": "x", "path": {"subpaths": []}, "fill": {"pattern": "p"}})")));
        QVERIFY_THROWS_EXCEPTION(core::Error, vec::part_from_json(core::parse_python_json(R"({"id": "x", "path": {"subpaths": []}, "opacity": 2})")));
        const vec::ShapeList list{{part}, ""};
        const std::string blob = vec::shapes_blob(list);
        QCOMPARE(vec::shapes_from_blob(blob).front(), part);
        QVERIFY_THROWS_EXCEPTION(core::Error, vec::shapes_from_blob("[" + core::dump_canonical(vec::to_json(part)) + "," + core::dump_canonical(vec::to_json(part)) + "]"));
    }

    // The book: a page's paths and work path, a layer's vector mask, link and position lock, a pen layer's shape parts
    // (their own asset), a page of its own paper — each written only when held, read back, each naming its feature; a
    // book with none of them the same bytes and features as before; one not well formed opened read-only.
    void bookFrames() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        core::Document plain = small_book();
        write_book(plain, root / "plain");
        const std::string before = text_of(root / "plain" / "project.json");
        for (const char* f : {"vector@1", "pen-shapes@1", "transform@1", "page-spec@1", "masks@1"}) QVERIFY(before.find(f) == std::string::npos);
        // (read and written again: the same, but its revision)
        const storage::LoadResult read = storage::load_document(root / "plain");
        QVERIFY(read.document.read_only_reason.empty());
        write_book(read.document, root / "again");
        Json first = core::parse_python_json(before), second = core::parse_python_json(text_of(root / "again" / "project.json"));
        first.erase("revision");
        second.erase("revision");
        QCOMPARE(core::dump_python_indent2(second), core::dump_python_indent2(first));

        core::Document doc = small_book();
        core::Page& page = doc.edit_page(0);
        page.paths.push_back(vec::NamedPath{"輪郭", pr::rect_path(1, 1, 10, 10)});
        page.work_path = pr::ellipse_path(5, 5, 20, 10);
        page.clipping_path = vec::ClippingPath{"輪郭", 0.5};
        core::Layer& ink = page.layers.at(2);  // (the ink pen layer)
        QCOMPARE(ink.kind, core::LayerKind::Strokes);
        vec::ShapePart part;
        part.id = "sp1";
        part.path = pr::rect_path(2, 3, 4, 5);
        part.fill = Json{{"rgb", nullptr}};
        ink.shapes = vec::make_shapes({part});
        ink.vector_mask = vec::VectorMask{pr::rect_path(0, 0, 30, 30), true, true, 0.8, 1.5};
        ink.link = "group1";
        ink.lock_position = true;
        page.own_spec = true;
        page.spec.width_mm = core::Num(70);
        page.spec.trim_origin_mm = std::make_pair(4.0, 5.0);
        write_book(doc, root / "full");
        const storage::LoadResult back = storage::load_document(root / "full");
        QVERIFY2(back.document.read_only_reason.empty(), back.document.read_only_reason.c_str());
        for (const char* f : {"vector@1", "pen-shapes@1", "transform@1", "page-spec@1"}) {
            QVERIFY2(std::find(back.document.features.begin(), back.document.features.end(), f) != back.document.features.end(), f);
        }
        const core::Page& got = back.document.page(0);
        QCOMPARE(got.paths.size(), std::size_t(1));
        QCOMPARE(got.paths[0], page.paths[0]);
        QCOMPARE(*got.work_path, vec::path_from_json(vec::to_json(*page.work_path)));  // (its normal form: 4 decimals)
        QCOMPARE(got.clipping_path->name, std::string("輪郭"));
        QCOMPARE(got.layers.at(2).shapes->items.front(), part);
        QVERIFY(!got.layers.at(2).shapes->blob_ref.empty());
        QCOMPARE(*got.layers.at(2).vector_mask, *ink.vector_mask);
        QCOMPARE(*got.layers.at(2).link, std::string("group1"));
        QVERIFY(got.layers.at(2).lock_position);
        QVERIFY(got.own_spec);
        QCOMPARE(got.spec.width_mm.value(), 70.0);
        QCOMPARE(got.spec.trim_origin_mm->first, 4.0);
        QCOMPARE(back.document.spec.width_mm.value(), 60.0);
        // (the shapes asset kept by the garbage collection's reading: a ref in project.json)
        QVERIFY(text_of(root / "full" / "project.json").find(got.layers.at(2).shapes->blob_ref) != std::string::npos);
        // a clipping path naming no saved path: read-only; a shape part on a paint layer: refused by the check
        Json payload = test::read_json(QString::fromStdString(core::path_to_utf8(root / "full" / "project.json")));
        payload["pages"][0]["clipping_path"]["name"] = "ない";
        test::write_bytes(QString::fromStdString(core::path_to_utf8(root / "full" / "project.json")), core::dump_python_indent2(payload));
        QVERIFY(!storage::load_document(root / "full").document.read_only_reason.empty());
        core::Document wrong = small_book();
        wrong.edit_page(0).layers.at(0).shapes = vec::make_shapes({part});  // (the background fill layer)
        QVERIFY_THROWS_EXCEPTION(core::Error, vec::validate_vector(wrong));
    }

    // PC3 C-a — trace.rs: square_contour_orientation_and_count (the contour through the pixels' outer half, its corners cut
    // at 45°: 12 − 4·0.125, the inside on its left), traced_rect_has_corners; a circle traced within its tolerance, a ring
    // (the hole excluded), the box's offset kept.
    void trace() {
        const int w = 6, h = 5;
        std::vector<float> v(std::size_t(w) * h, 0.0f);
        for (int y = 1; y < 4; ++y)
            for (int x = 1; x < 5; ++x) v[std::size_t(y) * w + x] = 1.0f;
        const auto loops = render::path_trace::contours(v, pr::PixelBox{0, 0, w, h});
        QCOMPARE(loops.size(), std::size_t(1));
        double shoelace = 0;
        for (std::size_t i = 0; i < loops[0].size(); ++i) {
            const auto& p = loops[0][i];
            const auto& q = loops[0][(i + 1) % loops[0].size()];
            shoelace += p.first * q.second - q.first * p.second;
        }
        shoelace *= 0.5;
        QVERIFY2(shoelace < 0, std::to_string(shoelace).c_str());
        QVERIFY(std::fabs(-shoelace - 11.5) < 1e-9);
        const int rw = 40, rh = 30;
        std::vector<float> rect(std::size_t(rw) * rh, 0.0f);
        for (int y = 5; y < 25; ++y)
            for (int x = 5; x < 35; ++x) rect[std::size_t(y) * rw + x] = 1.0f;
        const vec::Path traced = render::path_trace::trace_mask(rect, pr::PixelBox{0, 0, rw, rh}, 0.0f, 1.0);
        QCOMPARE(traced.subpaths.size(), std::size_t(1));
        const auto& knots = traced.subpaths[0].knots;
        QVERIFY2(knots.size() >= 4 && knots.size() <= 12, std::to_string(knots.size()).c_str());
        QVERIFY(std::count_if(knots.begin(), knots.end(), [](const vec::Knot& k) { return !k.smooth; }) >= 4);
        // (a disc of radius 20 traced within 0.5 px: its area within 1 % of the disc's coverage; a ring: two subpaths, the
        // hole excluded, the same area as the coverage)
        const int dw = 60;
        std::vector<float> disc(std::size_t(dw) * dw, 0.0f), ring(std::size_t(dw) * dw, 0.0f);
        for (int y = 0; y < dw; ++y) {
            for (int x = 0; x < dw; ++x) {
                const double r = std::hypot(x + 0.5 - 30, y + 0.5 - 30);
                disc[std::size_t(y) * dw + x] = r <= 20 ? 1.0f : 0.0f;
                ring[std::size_t(y) * dw + x] = r <= 20 && r >= 10 ? 1.0f : 0.0f;
            }
        }
        const auto traced_area = [&](const vec::Path& p) { return area(cov(p, pr::PixelBox{0, 0, dw, dw})); };
        const vec::Path round = render::path_trace::trace_mask(disc, pr::PixelBox{0, 0, dw, dw}, 0.0f, 0.5);
        QVERIFY(std::fabs(traced_area(round) - area(disc)) / area(disc) < 0.01);
        QVERIFY(std::all_of(round.subpaths[0].knots.begin(), round.subpaths[0].knots.end(), [](const vec::Knot& k) { return k.smooth; }));
        const vec::Path holed = render::path_trace::trace_mask(ring, pr::PixelBox{0, 0, dw, dw}, 0.0f, 0.5);
        QCOMPARE(holed.subpaths.size(), std::size_t(2));
        QCOMPARE(holed.subpaths[1].op, vec::PathOp::Exclude);
        QVERIFY(std::fabs(traced_area(holed) - area(ring)) / area(ring) < 0.02);
        // (a box elsewhere on the grid: the same path, moved to where its pixels are; its anchors on the pixels' edges)
        const vec::Path moved = render::path_trace::trace_mask(rect, pr::PixelBox{100, 50, 100 + rw, 50 + rh}, 0.0f, 1.0);
        const vec::Path expected = traced.transformed({1.0, 0.0, 0.0, 1.0, 100.0, 50.0});
        QCOMPARE(moved.subpaths.size(), expected.subpaths.size());
        QCOMPARE(moved.subpaths[0].knots.size(), expected.subpaths[0].knots.size());
        for (std::size_t i = 0; i < expected.subpaths[0].knots.size(); ++i) {
            const vec::Knot& a = moved.subpaths[0].knots[i];
            const vec::Knot& b = expected.subpaths[0].knots[i];
            QCOMPARE(a.smooth, b.smooth);
            for (const auto& [p, q] : {std::pair{a.anchor, b.anchor}, std::pair{a.in, b.in}, std::pair{a.out, b.out}}) {
                QVERIFY(std::fabs(p.x - q.x) < 1e-6 && std::fabs(p.y - q.y) < 1e-6);
            }
            QVERIFY(a.anchor.x >= 105 - 1e-9 && a.anchor.x <= 135 + 1e-9 && a.anchor.y >= 55 - 1e-9 && a.anchor.y <= 75 + 1e-9);
        }
    }

    // PC3 C-a — psd/src/path.rs: subpaths_roundtrip_and_bytes_are_stable, vector_mask_block_roundtrip, fixed_point (a
    // path's records and the vmsk block read back the same, in mm of the paper; the operations as psd-tools has them);
    // the resources of a page's paths; a name of other letters kept as UTF-8.
    void psdPaths() {
        namespace pp = render::psd_paths;
        vec::Path p;
        p.subpaths.push_back(polygon({{25, 25}, {75, 25}, {50, 75}}));
        p.subpaths.back().knots[1] = vec::Knot{{75, 25}, {62.5, 12.5}, {87.5, 37.5}, true};
        p.subpaths.push_back(polygon({{0, 100}, {6.25, 93.75}}, vec::PathOp::Subtract, false));
        p.inverted = true;
        const std::string bytes = pp::records(p, 100, 100);
        QCOMPARE(bytes.size(), std::size_t((2 + 2 + 5) * 26));
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(bytes[26 * 2 + 5])), 1);    // (the first subpath: combine, 1)
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(bytes[26 * 6 + 5])), 2);    // (subtract: 2)
        const auto back = pp::path_of(bytes, 100, 100);
        QVERIFY(back.has_value());
        QCOMPARE(back->subpaths.size(), std::size_t(2));
        QVERIFY(back->inverted);
        QCOMPARE(back->subpaths[1].op, vec::PathOp::Subtract);
        QVERIFY(!back->subpaths[1].closed);
        for (std::size_t s = 0; s < 2; ++s) {
            for (std::size_t k = 0; k < p.subpaths[s].knots.size(); ++k) {
                const vec::Knot& a = p.subpaths[s].knots[k];
                const vec::Knot& b = back->subpaths[s].knots[k];
                QVERIFY(std::fabs(a.anchor.x - b.anchor.x) < 1e-5 && std::fabs(a.in.y - b.in.y) < 1e-5 && std::fabs(a.out.x - b.out.x) < 1e-5);
                QCOMPARE(a.smooth, b.smooth);
            }
        }
        QCOMPARE(pp::records(*back, 100, 100), bytes);
        vec::VectorMask m;
        m.path = p;
        m.linked = false;
        m.enabled = false;
        const std::string block = pp::vmsk(m, 100, 100);
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(block[7])), 1 | 2 | 4);  // (inverted, not linked, disabled)
        const auto mask = pp::vector_mask_of(block, 100, 100);
        QVERIFY(mask && mask->path.inverted && !mask->linked && !mask->enabled);
        QVERIFY(!pp::vector_mask_of(std::string(4, '\0'), 100, 100));
        core::Page page;
        page.spec.width_mm = core::Num(100);
        page.spec.height_mm = core::Num(100);
        page.paths.push_back(vec::NamedPath{"輪郭", p});
        page.work_path = p;
        page.clipping_path = vec::ClippingPath{"輪郭", 0.5};
        const std::string resources = pp::resources(page);
        QCOMPARE(resources.substr(0, 4), std::string("8BIM"));
        QCOMPARE(pp::name_of(pp::resource_name("輪郭")), std::string("輪郭"));
        QCOMPARE(pp::name_of(std::string("\xe9t\xe9")), std::string("\xc3\xa9t\xc3\xa9"));  // (Latin-1 bytes, not UTF-8)
        const auto clip = pp::clipping_of(pp::clipping(*page.clipping_path));
        QVERIFY(clip && clip->name == "輪郭" && std::fabs(clip->flatness - 0.5) < 1e-4);
    }

    // PC3 C-a: a vector mask as the drawing lays it (render/vector_masks): its coverage over a part the same as over the
    // whole there, its density, its feather softening the edge, nothing when it is off.
    void vectorMaskShown() {
        core::Page page;
        page.spec.width_mm = core::Num(100);
        page.spec.height_mm = core::Num(100);
        vec::VectorMask m;
        m.path = pr::rect_path(0, 0, 50, 100);
        const render::Size size{100, 100};
        const render::Box all{0, 0, 100, 100};
        const render::Image sharp = render::vector_masks::shown(page, m, size, all);
        QCOMPARE(static_cast<int>(sharp.getpixel(10, 50)[0]), 255);
        QCOMPARE(static_cast<int>(sharp.getpixel(60, 50)[0]), 0);
        const render::Box part{40, 40, 70, 60};
        QCOMPARE(render::vector_masks::shown(page, m, size, part).tobytes(), sharp.crop(part).tobytes());
        m.density = 0.4;
        QCOMPARE(static_cast<int>(render::vector_masks::shown(page, m, size, all).getpixel(60, 50)[0]), 153);  // (255 − 0.4 × 255)
        m.density = 1.0;
        m.feather_mm = 6.0;  // (σ = 3 mm: 3 px)
        const render::Image soft = render::vector_masks::shown(page, m, size, all);
        QVERIFY(soft.getpixel(50, 50)[0] > 100 && soft.getpixel(50, 50)[0] < 155);
        QVERIFY(soft.getpixel(45, 50)[0] > soft.getpixel(50, 50)[0] && soft.getpixel(55, 50)[0] < soft.getpixel(50, 50)[0]);
        QCOMPARE(render::vector_masks::shown(page, m, size, part).tobytes(), soft.crop(part).tobytes());
        m.enabled = false;
        QCOMPARE(static_cast<int>(render::vector_masks::shown(page, m, size, all).getpixel(90, 50)[0]), 255);
        core::Layer layer;
        QVERIFY(!render::vector_masks::with_vector_mask(page, layer, size, all, std::nullopt));
        layer.vector_mask = m;
        layer.vector_mask->enabled = true;
        layer.vector_mask->feather_mm = 0;
        const auto both = render::vector_masks::with_vector_mask(page, layer, size, all, render::Image::create("L", size, render::Ink(128)));
        QVERIFY(both && both->getpixel(10, 50)[0] == 128 && both->getpixel(90, 50)[0] == 0);
    }
};

QTEST_GUILESS_MAIN(TestPathRaster)
#include "test_path_raster.moc"
