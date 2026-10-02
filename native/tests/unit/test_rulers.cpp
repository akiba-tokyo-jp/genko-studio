// Rulers without the Python reference (M3-B): the snapping of core::rulers for every kind (the cases of Python's
// test_w10_rulers, test_j2, test_j6 and test_m14), and the ruler ops through the CommandBus — each op's success, its
// refusals (the book unchanged), the page lock, dry-run, Undo and Redo through the journal, and the book saved and
// read again. The exact numbers against Python are in contract/test_contract_rulers.cpp.

#include <QtTest>

#include <QTemporaryDir>

#include <cmath>
#include <string>
#include <vector>

#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/rulers.hpp"
#include "m3b_support.hpp"
#include "render/brushes.hpp"
#include "storage/snapshot.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

namespace rulers = genko::core::rulers;
using genko::core::Document;
using genko::core::Json;
using genko::core::PenPoint;
using genko::core::PenPoints;
namespace m3b = genko::test::m3b;

namespace {

Json j(const char* text) { return genko::core::parse_python_json(text); }

PenPoints pts(std::initializer_list<std::pair<double, double>> xy) {
    PenPoints out;
    for (const auto& [x, y] : xy) out.push_back(PenPoint{x, y, std::nullopt});
    return out;
}

Document book() {
    Document doc = genko::core::new_episode("定規", genko::core::Num(1), 2, genko::core::PageSpec::b4_comic());
    return doc;
}

// The error of a batch ("" when it succeeds); the book given is never changed.
std::string error_of(const Document& doc, const char* ops, const std::string& actor = "genko") {
    const Json before = genko::storage::snapshot(doc, true);
    std::string out;
    try {
        (void)m3b::bus().apply(doc, j(ops), genko::core::Actor(actor));
    } catch (const genko::core::ApplyError& error) {
        out = error.what();
    }
    if (genko::storage::snapshot(doc, true) != before) out += " [the book changed]";
    return out;
}

Document applied(const Document& doc, const char* ops, const std::string& actor = "genko") {
    return m3b::bus().apply(doc, j(ops), genko::core::Actor(actor)).doc;
}

double off_line(const PenPoints& points, rulers::XY a, rulers::XY b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double n = std::hypot(dx, dy);
    double worst = 0;
    for (const PenPoint& p : points) worst = std::max(worst, std::fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / n);
    return worst;
}

}  // namespace

class TestRulers : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

    void huge_sample_counts_are_refused_before_integer_arithmetic() {
        for (const char* kind : {"line", "curve", "parallel_curve", "multi_curve", "radial_curve"}) {
            Json ruler = j(R"({"id":"r", "points":[[0,0],[1e19,0]], "points2":[[0,1],[1e19,1]], "center":[0,-1], "active":true})");
            ruler["kind"] = kind;
            QVERIFY_EXCEPTION_THROWN(rulers::snap(pts({{0, 0}, {1e19, 0}}), Json::array({ruler})), genko::core::PyValueError);
        }
    }

    void ruler_pen_refuses_computed_nonfinite_points() {
        const Document doc = applied(book(), R"([{"op":"add_ruler","page":1,"id":"r","kind":"rect","points":[[1e308,0],[1.1e308,1]]}])");
        const std::string error = error_of(doc, R"([{"op":"set_note","page":1,"note":"rollback"},{"op":"ruler_to_layer","page":1,"id":"r"}])");
        QVERIFY2(error.starts_with("ops[1] ruler_to_layer: points must be finite"), error.c_str());
        QVERIFY(!error.ends_with("[the book changed]"));
    }

    // --- the snapping ---------------------------------------------------------------------------------------------

    void a_line_runs_round_a_rectangle_keeping_its_corners() {
        const Json rect = j(R"({"kind": "rect", "points": [[10, 10], [50, 30]]})");
        rulers::validate(rect);
        const PenPoints out = rulers::snap(pts({{12, 9.5}, {30, 10.5}, {49, 11}, {50.5, 20}, {49, 29}}), Json::array({rect}));
        bool corner = false;
        for (const PenPoint& p : out) {
            if (std::round(p.x * 100) / 100 == 50.0 && std::round(p.y * 100) / 100 == 10.0) corner = true;
            const bool on = std::min(std::fabs(p.x - 10), std::fabs(p.x - 50)) < 1e-6 || std::min(std::fabs(p.y - 10), std::fabs(p.y - 30)) < 1e-6;
            QVERIFY(on);
        }
        QVERIFY(corner);
        QCOMPARE(out.back().x, 50.0);
        QCOMPARE(out.back().y, 29.0);
        const Json big = j(R"({"kind": "rect", "points": [[10, 10], [90, 70]]})");
        const PenPoints far = rulers::snap(pts({{50, 40}, {55, 42}}), Json::array({big}));  // (started far from the outline)
        QCOMPARE(far.size(), std::size_t{2});
        QCOMPARE(far[0].x, 50.0);
        QCOMPARE(far[1].y, 42.0);
    }

    void ellipse_and_polygon_rulers() {
        const Json ellipse = j(R"({"kind": "ellipse", "points": [[0, 0], [40, 20]]})");
        for (const PenPoint& p : rulers::snap(pts({{20, 0.5}, {35, 3}, {39.5, 10}}), Json::array({ellipse}))) {
            QVERIFY(std::fabs(std::pow((p.x - 20) / 20, 2) + std::pow((p.y - 10) / 10, 2) - 1) < 0.02);
        }
        const Json tri = j(R"({"kind": "polygon", "points": [[0, 0], [30, 0], [15, 20]]})");
        rulers::validate(tri);
        const auto line = rulers::outline(tri).front();
        QCOMPARE(line.size(), std::size_t{4});
        QVERIFY(line.front().x == line.back().x && line.front().y == line.back().y);
        QVERIFY_THROWS_EXCEPTION(genko::core::PyValueError, rulers::validate(j(R"({"kind": "rect", "points": [[0, 0], [0.2, 10]]})")));
    }

    void the_perspective_grid() {
        const Json one = j(R"({"kind": "perspective", "points": [[90, 80]], "grid": 8})");
        const auto lines = rulers::perspective_grid(one, rulers::PageSize{genko::core::Num(180), genko::core::Num(250)});
        int to_vp = 0;
        std::vector<double> depths;
        for (const auto& seg : lines) {
            if (seg[0].x == 90.0 && seg[0].y == 80.0) ++to_vp;
            if (seg[0].y == seg[1].y) depths.push_back(seg[0].y);
        }
        QCOMPARE(to_vp, 9);
        QVERIFY(!depths.empty());
        std::sort(depths.begin(), depths.end());
        for (std::size_t i = 2; i < depths.size(); ++i) QVERIFY(depths[i] - depths[i - 1] >= depths[i - 1] - depths[i - 2] - 1e-6);
        const Json two = j(R"({"kind": "perspective", "points": [[-80, 70], [260, 70]], "grid": 6})");
        QCOMPARE(rulers::perspective_grid(two, rulers::PageSize{genko::core::Num(180), genko::core::Num(250)}).size(), std::size_t{14});
        QVERIFY(rulers::perspective_grid(j(R"({"kind": "perspective", "points": [[1, 1]]})"), rulers::PageSize{genko::core::Num(10), genko::core::Num(10)}).empty());
    }

    void guides_snap_lines_near_them() {
        const Json guide = j(R"({"id": "g", "kind": "guide", "axis": "h", "at": 100.0, "active": true})");
        rulers::validate(guide);
        for (const PenPoint& p : rulers::snap(pts({{20, 101.5}, {120, 108}}), Json::array({guide}))) QVERIFY(std::fabs(p.y - 100.0) < 1e-6);
        QCOMPARE(rulers::snap(pts({{20, 110}, {120, 118}}), Json::array({guide}))[0].y, 110.0);
    }

    void straight_radial_concentric_and_perspective() {
        const Json line = j(R"({"kind": "line", "points": [[20, 100], [200, 160]], "id": "r1"})");
        PenPoints wobbly;
        for (int i = 0; i <= 20; ++i) wobbly.push_back(PenPoint{40 + 5.5 * i, 108 + 1.85 * i + (i % 2 ? 0.7 : -0.7), 0.3 + 0.02 * i});
        QVERIFY(off_line(rulers::snap(wobbly, Json::array({line})), {20, 100}, {200, 160}) < 0.01);
        const Json inactive = j(R"({"kind": "line", "points": [[20, 100], [200, 160]], "active": false})");
        QVERIFY(off_line(rulers::snap(wobbly, Json::array({inactive})), {20, 100}, {200, 160}) > 0.5);
        const Json parallel = j(R"({"kind": "parallel", "angle": 30})");
        const PenPoints p = rulers::snap(pts({{60, 200}, {80, 215}, {140, 250}}), Json::array({parallel}));
        QVERIFY(std::fabs(std::atan2(p.back().y - p.front().y, p.back().x - p.front().x) * 180 / genko::core::kPi - 30) < 0.01);
        const Json radial = j(R"({"kind": "radial", "points": [[120, 180]]})");
        const PenPoints r = rulers::snap(pts({{40, 100}, {60, 128}, {80, 150}}), Json::array({radial}));
        QVERIFY(off_line(r, {r.front().x, r.front().y}, {120, 180}) < 0.01);
        const Json concentric = j(R"({"kind": "concentric", "points": [[120, 180]], "ratio": 0.5})");
        PenPoints arc;
        for (int a = 0; a < 25; ++a) arc.push_back(PenPoint{120 + 40 * std::cos(a / 10.0) + 0.8 * std::sin(a), 180 + 20 * std::sin(a / 10.0), std::nullopt});
        const PenPoints c = rulers::snap(arc, Json::array({concentric}));
        double lo = 1e9, hi = -1e9;
        for (const PenPoint& q : c) {
            const double k = std::pow((q.x - 120) / 40, 2) + std::pow((q.y - 180) / 20, 2);
            lo = std::min(lo, k);
            hi = std::max(hi, k);
        }
        QVERIFY(hi - lo < 1e-3 && c.size() > 20);
        const Json two = j(R"({"kind": "perspective", "points": [[-100, 120], [350, 120]]})");
        const PenPoints left = rulers::snap(pts({{120, 200}, {90, 190}, {60, 180}}), Json::array({two}));
        QVERIFY(off_line(left, {left.front().x, left.front().y}, {-100, 120}) < 0.01);
        const PenPoints up = rulers::snap(pts({{120, 200}, {120.5, 230}, {121, 260}}), Json::array({two}));
        QVERIFY(std::fabs(up.front().x - up.back().x) < 0.01);
        const auto eye = rulers::horizon(two);
        QVERIFY(eye && eye->second.x == 1.0 && eye->second.y == 0.0);
    }

    void curve_rulers() {
        const Json parallel = j(R"({"id": "p", "kind": "parallel_curve", "points": [[0, 0], [50, 20], [100, 0]]})");
        const PenPoints p = rulers::snap(pts({{10, 40}, {50, 60}, {90, 40}}), Json::array({parallel}));
        const auto mid = *std::min_element(p.begin(), p.end(), [](const PenPoint& a, const PenPoint& b) { return std::fabs(a.x - 50) < std::fabs(b.x - 50); });
        QVERIFY(mid.y > 43 && std::fabs(p.front().y - 40) < 8);
        const Json radial = j(R"({"id": "r", "kind": "radial_curve", "points": [[0, 0], [50, 20], [100, 0]], "center": [50, -100]})");
        const PenPoints r = rulers::snap(pts({{50, 70}, {80, 60}}), Json::array({radial}));
        QVERIFY(std::fabs(r.front().y - 70) < 2 && r.size() > 2);
        const Json multi = j(R"({"id": "m", "kind": "multi_curve", "points": [[0, 0], [100, 0]], "points2": [[0, 100], [50, 140], [100, 100]]})");
        const PenPoints m = rulers::snap(pts({{0, 50}, {50, 55}, {100, 50}}), Json::array({multi}));
        const auto middle = *std::min_element(m.begin(), m.end(), [](const PenPoint& a, const PenPoint& b) { return std::fabs(a.x - 50) < std::fabs(b.x - 50); });
        QVERIFY(middle.y > 60 && middle.y < 80);
    }

    void symmetry_and_the_grid() {
        const Json mirror = j(R"([{"kind": "symmetry", "points": [[120, 0], [120, 300]]}])");
        const auto copies = rulers::symmetry_copies(pts({{80, 100}, {100, 120}, {110, 150}}), mirror);
        QCOMPARE(copies.size(), std::size_t{1});
        QCOMPARE(copies[0][0].x, 160.0);
        QCOMPARE(copies[0][0].y, 100.0);
        const Json six = j(R"([{"kind": "symmetry", "points": [[120, 150], [120, 100]], "copies": 6}])");
        QCOMPARE(rulers::symmetry_copies(pts({{150, 150}, {160, 150}}), six).size(), std::size_t{5});
        const Json mirrored = j(R"([{"kind": "symmetry", "points": [[120, 150], [120, 100]], "copies": 4, "mirror": true}])");
        QCOMPARE(rulers::symmetry_copies(pts({{150, 150}, {160, 150}}), mirrored).size(), std::size_t{7});
        // panel rulers stay in their panel; a ruler of a layer only for that layer
        const Json in_panel = j(R"([{"kind": "parallel", "angle": 0, "frame_id": "top"}])");
        const auto inside = [](const Json& frame_id, double, double y) { return frame_id == Json("top") && y < 100; };
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), in_panel, inside).back().y - 20) < 1e-9);
        QVERIFY(std::fabs(rulers::snap(pts({{60, 120}, {150, 140}}), in_panel, inside).back().y - 140) < 1e-9);
        const Json for_layer = j(R"([{"kind": "parallel", "angle": 0, "layer_id": "ink"}])");
        const Json other("other");
        const Json ink("ink");
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), for_layer, {}, Json(), &other).back().y - 40) < 1e-9);
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), for_layer, {}, Json(), &ink).back().y - 20) < 1e-9);
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), for_layer).back().y - 20) < 1e-9);  // (no layer: every ruler)
        // one ruler by its id (any value Python compares with ==)
        const Json two_ids = j(R"([{"id": 7, "kind": "parallel", "angle": 0}, {"id": "b", "kind": "parallel", "angle": 90}])");
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), two_ids, {}, Json(7)).back().y - 20) < 1e-9);
        QVERIFY(std::fabs(rulers::snap(pts({{60, 20}, {150, 40}}), two_ids, {}, Json("b")).back().x - 60) < 1e-9);
        const auto g = rulers::snap_to_grid(rulers::XY{12.4, 17.6}, 5);
        QCOMPARE(g.x, 10.0);
        QCOMPARE(g.y, 20.0);
        QCOMPARE(rulers::snap_to_grid(rulers::XY{12.4, 17.6}, 0).x, 12.4);
    }

    // ruler_to_layer's kinds are the brushes the lines are drawn with (core/brushes.hpp for both).
    void the_brush_kinds_are_the_drawings() {
        const Document doc = book();
        for (const auto& b : genko::core::builtin_brushes()) {
            QCOMPARE(genko::core::brush_kind(Json(b.key), doc), b.key);
            QCOMPARE(genko::render::brushes::brush(b.key).key, b.key);
        }
        QCOMPARE(genko::core::brush_kind(Json("oil"), doc), std::string("marker"));
    }

    // What apply_ops lets through (IndexError, ZeroDivisionError, …: a traceback in Python's command line) is the
    // CommandBus's python_error, its error ending with the traceback's last line.
    void python_errors() {
        const Document doc = book();
        const auto error = [&](const char* ops) {
            try {
                (void)m3b::bus().apply(doc, j(ops), genko::core::Actor("genko"));
            } catch (const genko::core::ApplyError& e) {
                return e.code() + ": " + e.what();
            }
            return std::string("(applied)");
        };
        QCOMPARE(error(R"([{"op": "add_ruler", "page": 1, "kind": "radial_curve", "points": [[0, 0], [5, 5], [9, 0]], "center": [1]}])"),
                 std::string("python_error: ops[0] add_ruler: IndexError: list index out of range"));
        QCOMPARE(error(R"([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[0, 0], [5]]}])"),
                 std::string("python_error: ops[0] add_ruler: IndexError: list index out of range"));
        // a symmetry ruler of copies 0.5 (int 0) a book may hold: 2 * math.pi / 0
        Document held = book();
        held.edit_page(0).rulers = j(R"([{"kind": "symmetry", "points": [[10, 0], [10, 50]], "copies": 0.5}])");
        QVERIFY_THROWS_EXCEPTION(genko::core::PyUncaught,
                                 rulers::symmetry_copies(pts({{1, 1}, {2, 2}}), held.page(0).rulers));
        try {
            (void)m3b::bus().apply(held, j(R"([{"op": "add_stroke", "page": 1, "points": [[1, 1], [2, 2]], "snap_ruler": true}])"),
                                   genko::core::Actor("genko"));
            QFAIL("applied");
        } catch (const genko::core::ApplyError& e) {
            QCOMPARE(std::string(e.what()), std::string("ops[0] add_stroke: ZeroDivisionError: float division by zero"));
            QCOMPARE(e.code(), std::string("python_error"));
        }
    }

    // --- the ops ------------------------------------------------------------------------------------------------------

    void add_ruler() {
        const Document doc = book();
        Document out = applied(doc, R"([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 160]], "id": "r1"},
                                         {"op": "add_ruler", "page": 1, "kind": "guide", "axis": "v", "at": 64},
                                         {"op": "add_ruler", "page": 2, "kind": "symmetry", "points": [[120, 0], [120, 300]], "copies": 6,
                                          "mirror": 1, "id": "s"}])");
        QCOMPARE(out.page(0).rulers.size(), std::size_t{2});
        QCOMPARE(out.page(0).rulers[0], j(R"({"id": "r1", "kind": "line", "points": [[20.0, 100.0], [200.0, 160.0]], "active": true, "visible": true})"));
        QCOMPARE(out.page(0).rulers[1]["axis"], Json("v"));
        QCOMPARE(out.page(0).rulers[1]["at"], Json(64.0));
        QVERIFY(out.page(0).rulers[1]["id"].get<std::string>().size() == 12);
        QCOMPARE(out.page(1).rulers[0]["copies"], Json(6));
        QCOMPARE(out.page(1).rulers[0]["mirror"], Json(true));
        QVERIFY(doc.page(0).rulers.empty());  // (the book given is not changed)
        // refusals: the book stays as it was
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "spiral"}])").starts_with("ops[0] add_ruler: kind must be one of line, curve"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[1, 1]]}])").starts_with("ops[0] add_ruler: a line ruler needs 2 point(s)"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "guide", "axis": "z", "at": 1}])").starts_with("ops[0] add_ruler: a guide's axis is h or v"));
        QVERIFY(error_of(out, R"([{"op": "add_ruler", "page": 1, "kind": "parallel", "id": "r1"}])").starts_with("ops[0] add_ruler: ruler r1 already exists"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "parallel", "frame_id": "nope"}])").starts_with("ops[0] add_ruler: no panel nope"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "parallel", "layer_id": "nope"}])").starts_with("ops[0] add_ruler: no layer nope"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "multi_curve", "points": [[0, 0], [50, 20], [100, 0]]}])")
                    .starts_with("ops[0] add_ruler: a multi_curve ruler needs points2"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "perspective", "points": [[0, 0], [0, 0], [0, 0], [0, 0]]}])")
                    .starts_with("ops[0] add_ruler: a perspective ruler has 1 to 3 vanishing points"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "concentric", "points": [[1, 1]], "ratio": "x"}])")
                    .starts_with("ops[0] add_ruler: a value of the wrong type (could not convert string to float: 'x')"));
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 9, "kind": "parallel"}])").starts_with("ops[0] add_ruler: no page 9"));
        // a number that is not finite is not kept (Python would write Infinity into the book)
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "parallel", "angle": "inf"}])").starts_with("ops[0] add_ruler: angle must be a finite number"));
    }

    void edit_and_delete_rulers() {
        Document doc = applied(book(), R"([{"op": "add_ruler", "page": 1, "kind": "perspective", "points": [[20, 150], [220, 150]], "id": "p",
                                             "lock_horizon": true}])");
        doc = applied(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "points": [[40, 170], [220, 150]]}])");
        QCOMPARE(doc.page(0).rulers[0]["points"][0], j("[40.0, 150.0]"));  // slid along the eye level
        doc = applied(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "horizon_y": 120}])");
        QCOMPARE(doc.page(0).rulers[0]["points"], j("[[40.0, 120.0], [220.0, 120.0]]"));
        doc = applied(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "fixed": true, "grid": 8, "active": 0}])");
        QCOMPARE(doc.page(0).rulers[0]["grid"], Json(8));
        QCOMPARE(doc.page(0).rulers[0]["active"], Json(false));
        QVERIFY(error_of(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "points": [[0, 0], [10, 0]]}])").starts_with("ops[0] edit_ruler: the ruler is fixed (unfix it first)"));
        QVERIFY(error_of(doc, R"([{"op": "edit_ruler", "page": 1, "id": "q"}])").starts_with("ops[0] edit_ruler: no ruler q"));
        QVERIFY(error_of(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "grid": 61}])").starts_with("ops[0] edit_ruler: grid is 0 (none) to 60 lines"));
        doc = applied(doc, R"([{"op": "edit_ruler", "page": 1, "id": "p", "grid": 0}])");
        QVERIFY(!doc.page(0).rulers[0].contains("grid"));
        doc = applied(doc, R"([{"op": "add_ruler", "page": 1, "kind": "parallel", "angle": 30, "id": "d"},
                               {"op": "set_ruler", "page": 1, "kind": "perspective", "points": [[100, 20]]}])");
        QCOMPARE(*doc.page(0).ruler, j(R"({"kind": "perspective", "points": [[100, 20]]})"));
        QVERIFY(error_of(doc, R"([{"op": "delete_ruler", "page": 1, "id": "q"}])").starts_with("ops[0] delete_ruler: no ruler q"));
        Document one = applied(doc, R"([{"op": "delete_ruler", "page": 1, "id": "p"}])");
        QCOMPARE(one.page(0).rulers.size(), std::size_t{1});
        QVERIFY(one.page(0).ruler.has_value());
        Document none = applied(doc, R"([{"op": "delete_ruler", "page": 1}])");
        QVERIFY(none.page(0).rulers.empty() && !none.page(0).ruler.has_value());
    }

    void ruler_to_layer() {
        Document doc = book();
        const std::string ink = doc.page(0).layers[2].id;
        Json ops = j(R"([{"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 100]], "id": "r"},
                         {"op": "add_ruler", "page": 1, "kind": "guide", "axis": "h", "at": 64, "id": "g"},
                         {"op": "add_ruler", "page": 1, "kind": "parallel", "angle": 30, "id": "d"},
                         {"op": "ruler_to_layer", "page": 1, "id": "r", "width_mm": 0.3, "rgb": [200, 10, 10, 9]},
                         {"op": "ruler_to_layer", "page": 1, "id": "g", "kind": "oil"}])");
        ops[3]["layer_id"] = ink;
        doc = m3b::bus().apply(doc, ops, genko::core::Actor("genko")).doc;
        const auto& strokes = doc.page(0).layers[2].strokes->items;
        QCOMPARE(strokes.size(), std::size_t{2});
        QCOMPARE(strokes[0]->points.front().x, 20.0);
        QCOMPARE(strokes[0]->points.back().x, 200.0);
        QCOMPARE(strokes[0]->width_mm, 0.3);
        QCOMPARE(strokes[0]->kind, std::string("mili"));
        QVERIFY(strokes[0]->rgb == (std::vector<std::int64_t>{200, 10, 10}));
        QVERIFY(strokes[0]->pressure == (std::vector<double>{1.0, 1.0}));
        QCOMPARE(strokes[1]->kind, std::string("marker"));  // (an old name as its new one)
        QCOMPARE(strokes[1]->points.back().x, 257.0);       // (out to the paper's edge)
        QVERIFY(error_of(doc, R"([{"op": "ruler_to_layer", "page": 1, "id": "d"}])").starts_with("ops[0] ruler_to_layer: this ruler has no line to draw (only directions)"));
        QVERIFY(error_of(doc, R"([{"op": "ruler_to_layer", "page": 1, "id": "r", "kind": "bogus"}])").starts_with("ops[0] ruler_to_layer: kind must be one of gpen, maru"));
        QVERIFY(error_of(doc, R"([{"op": "ruler_to_layer", "page": 1, "id": "r", "layer": "bogus"}])")
                    .starts_with("ops[0] ruler_to_layer: a value of the wrong type ('bogus' is not a valid LayerRole)"));
        const std::string fill_layer = "[{\"op\": \"ruler_to_layer\", \"page\": 1, \"id\": \"r\", \"layer_id\": \"" + doc.page(0).layers[0].id + "\"}]";
        QVERIFY(error_of(doc, fill_layer.c_str()).starts_with("ops[0] ruler_to_layer: this layer cannot be painted on"));
        QVERIFY(error_of(doc, R"([{"op": "ruler_to_layer", "page": 1, "id": "x"}])").starts_with("ops[0] ruler_to_layer: no ruler x"));
    }

    void the_page_lock_and_dry_run() {
        const Document doc = applied(book(), R"([{"op": "lock_page", "page": 1, "agent": "ai:other"}, {"op": "add_ruler", "page": 2, "kind": "parallel", "id": "a"}])");
        for (const char* ops : {R"([{"op": "add_ruler", "page": 1, "kind": "parallel"}])", R"([{"op": "set_ruler", "page": 1, "points": []}])",
                                R"([{"op": "delete_ruler", "page": 1}])", R"([{"op": "edit_ruler", "page": 1, "id": "a"}])",
                                R"([{"op": "ruler_to_layer", "page": 1, "id": "a"}])"}) {
            QVERIFY2(error_of(doc, ops).find(": page 1 locked by ai:other") != std::string::npos, ops);
        }
        QVERIFY(error_of(doc, R"([{"op": "add_ruler", "page": 1, "kind": "parallel"}])", "ai:other").empty());  // (its own page)
        const auto dry = m3b::bus().apply(doc, j(R"([{"op": "add_ruler", "page": 2, "kind": "radial", "points": [[1, 2]], "id": "b"}])"),
                                          genko::core::Actor("genko"), true);
        QCOMPARE(dry.doc.page(1).rulers.size(), std::size_t{2});
        QCOMPARE(doc.page(1).rulers.size(), std::size_t{1});
    }

    void undo_redo_and_reopen() {
        const auto dir = m3b::to_path(scratch_.path() + "/undo.genko");
        m3b::save_new(dir, book());
        const Json before = m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/s1"));
        const auto e = m3b::edit(dir, j(R"([{"op": "add_ruler", "page": 1, "kind": "curve", "points": [[30, 60], [90, 40], [150, 70]], "id": "c"},
                                             {"op": "set_ruler", "page": 2, "points": [[1, 2]]},
                                             {"op": "ruler_to_layer", "page": 1, "id": "c"}])"));
        const Json after = m3b::state_of(e.result.doc, m3b::to_path(scratch_.path() + "/s2"));
        // read again: the same book
        QCOMPARE(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/s3")), after);
        QCOMPARE(m3b::read(dir).page(0).layers[2].strokes->items.size(), std::size_t{1});
        // Undo through the journal: the book before; Redo: the book after
        genko::storage::restore(dir, "genko", false, false);
        QCOMPARE(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/s4")), before);
        genko::storage::restore(dir, "genko", true, false);
        // (a Redo brings back the saved state, whose objects the current book does not have keep the state's sorted key
        // order — storage::ordered_like: the same content)
        std::string where;
        QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/s5")), after, &where), where.c_str());
        m3b::edit(dir, j(R"([{"op": "delete_ruler", "page": 1, "id": "c"}])"));
        QVERIFY(m3b::read(dir).page(0).rulers.empty());
        genko::storage::restore(dir, "genko", false, false);
        QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/s6")), after, &where), where.c_str());
        QCOMPARE(genko::test::book_problems(QString::fromStdString(genko::storage::path_to_utf8(dir))), std::string());
    }
};

QTEST_GUILESS_MAIN(TestRulers)
#include "test_rulers.moc"
