// core/stroke_geom (the one C++ stroke.py: the ops draw and erase with it, the renderer smooths with it) against
// Python's genko/stroke.py (and brushes.smoothed): the table of tools/migration/render_harness.py unit-tables, every
// coordinate bit for bit; and where Python fails, the same exceptions as the ops report them.

#include <QtTest>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/pyops.hpp"
#include "core/stroke_geom.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::core::PenPoint;
using genko::core::PenPoints;

namespace {

double from_bits(const Json& hex) { return std::bit_cast<double>(std::stoull(hex.get<std::string>(), nullptr, 16)); }

std::string bits_of(double x) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(x)));
    return buf;
}

std::optional<double> opt(const Json& v) {
    if (v.is_null()) return std::nullopt;
    return from_bits(v);
}

PenPoints points_of(const Json& list) {
    PenPoints out;
    for (const Json& p : list) {
        PenPoint q{from_bits(p[0]), from_bits(p[1]), std::nullopt};
        if (p.size() > 2) q.p = from_bits(p[2]);
        out.push_back(q);
    }
    return out;
}

Json json_of(const PenPoints& points) {
    Json out = Json::array();
    for (const PenPoint& p : points) {
        Json q = Json::array({bits_of(p.x), bits_of(p.y)});
        if (p.p) q.push_back(bits_of(*p.p));
        out.push_back(q);
    }
    return out;
}

const Json& tables() {
    static const Json data = genko::test::read_json(genko::test::test_data("pyref/render_unit_tables.json"));
    return data;
}

void same(const Json& got, const Json& want, const std::string& what) {
    std::string where;
    QVERIFY2(genko::test::strict_equal(got, want, &where), (what + ": " + where).c_str());
}

}  // namespace

class TestStrokeGeom : public QObject {
    Q_OBJECT

private slots:
    void lines() {
        int n = 0;
        for (const Json& c : tables()["stroke"]["cases"]) {
            const std::string at = "case " + std::to_string(n++);
            const PenPoints points = points_of(c["points"]);
            same(json_of(genko::core::stabilize_points(points, c["window"].get<std::int64_t>(), c["by_speed"].get<bool>())),
                 c["stabilize"], at + " stabilize");
            same(json_of(genko::core::taper_points(points, opt(c["in_mm"]), opt(c["out_mm"]))), c["taper"], at + " taper");
            same(json_of(genko::core::fit_curve(points, from_bits(c["tolerance"]), from_bits(c["step"]))), c["fit"],
                 at + " fit_curve");
            same(json_of(genko::core::apply_pressure_curve(points, c["curve"].get<std::string>())), c["pressure_curve"],
                 at + " apply_pressure_curve");
            same(json_of(genko::core::smoothed(points, c["strength"].get<std::int64_t>())), c["smoothed"], at + " smoothed");
        }
    }

    void packs() {
        for (const Json& c : tables()["stroke"]["packs"]) {
            const PenPoint p = genko::core::pack_point(from_bits(c["x"]), from_bits(c["y"]), opt(c["pressure"]), from_bits(c["tilt"]));
            same(json_of({p})[0], c["out"], "pack_point");
        }
    }

    void erasers() {
        int n = 0;
        for (const Json& c : tables()["stroke"]["erasers"]) {
            const auto pieces = genko::core::split_by_eraser(points_of(c["points"]), points_of(c["eraser"]), from_bits(c["radius"]));
            Json got = Json::array();
            for (const PenPoints& piece : pieces) got.push_back(json_of(piece));
            same(got, c["pieces"], "split_by_eraser " + std::to_string(n++));
        }
    }

    void crosses() {
        for (const Json& c : tables()["stroke"]["crosses"]) {
            const PenPoints abcd = points_of(c["abcd"]);
            const auto xy = [&](std::size_t i) { return genko::core::PointF{abcd[i].x, abcd[i].y}; };
            const auto t = genko::core::seg_cross(xy(0), xy(1), xy(2), xy(3));
            if (c["t"].is_null()) {
                QVERIFY(!t);
            } else {
                QVERIFY(t);
                QCOMPARE(bits_of(*t), c["t"].get<std::string>());
            }
        }
    }

    void crossings() {
        int n = 0;
        for (const Json& c : tables()["stroke"]["crossings"]) {
            std::vector<genko::core::StrokePtr> strokes;
            for (const Json& s : c["strokes"]) {
                auto stroke = std::make_shared<genko::core::Stroke>();
                stroke->id = s["id"].get<std::string>();
                for (const PenPoint& p : points_of(s["points"])) stroke->points.push_back({p.x, p.y});
                stroke->width_mm = from_bits(s["width_mm"]);
                stroke->kind = s["kind"].get<std::string>();
                if (!s["rgb"].is_null()) stroke->rgb = s["rgb"].get<std::vector<std::int64_t>>();
                stroke->opacity = from_bits(s["opacity"]);
                strokes.push_back(stroke);
            }
            const auto result = genko::core::erase_to_crossing(strokes, points_of(c["eraser"]), from_bits(c["radius"]));
            const Json& want = c["result"];
            QCOMPARE(result.size(), want.size());
            for (std::size_t i = 0; i < result.size(); ++i) {
                const std::string at = "crossing " + std::to_string(n) + " stroke " + std::to_string(i);
                const genko::core::Stroke& got = *result[i];
                if (!want[i]["kept"].is_null()) {
                    QCOMPARE(got.id, want[i]["kept"].get<std::string>());  // the same stroke, untouched
                } else {
                    QCOMPARE(got.id.size(), std::size_t{12});
                }
                PenPoints pts;
                for (const auto& p : got.points) pts.push_back(PenPoint{p.x, p.y, std::nullopt});
                same(json_of(pts), want[i]["points"], at);
                QCOMPARE(bits_of(got.width_mm), want[i]["width_mm"].get<std::string>());
                QCOMPARE(got.kind, want[i]["kind"].get<std::string>());
                QCOMPARE(bits_of(got.opacity), want[i]["opacity"].get<std::string>());
                if (want[i]["rgb"].is_null()) {
                    QVERIFY(!got.rgb);
                } else {
                    QVERIFY(got.rgb);
                    QCOMPARE(*got.rgb, want[i]["rgb"].get<std::vector<std::int64_t>>());
                }
            }
            ++n;
        }
    }

    // Where Python raises, the exception the ops report (an op's "a value of the wrong type (…)", or Python stopping).
    void errorsAsPython() {
        using genko::core::PyTypeError;
        using genko::core::PyUncaught;
        using genko::core::PyValueError;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        // (-0.5) ** 1.8 is a complex number, which min() refuses; -inf ** 1.8 is inf, so the pressure is 1.0
        const PenPoints negative{{1, 1, 0.5}, {2, 2, -0.5}};
        QVERIFY_THROWS_EXCEPTION(PyTypeError, genko::core::apply_pressure_curve(negative));
        const PenPoints minus_inf{{1, 1, -inf}, {2, 2, -0.0}};
        const PenPoints curved = genko::core::apply_pressure_curve(minus_inf);
        QCOMPARE(*curved[0].p, 1.0);
        QCOMPARE(*curved[1].p, 0.05);
        QVERIFY(genko::core::apply_pressure_curve(negative, "linear") == negative);
        // int(nan) is a ValueError, int(inf) an OverflowError (not caught by apply_ops), a walk of 1e30 steps
        // Python's MemoryError
        const PenPoints far{{0, 0, std::nullopt}, {1, 1, std::nullopt}, {nan, 2, std::nullopt}, {3, 3, std::nullopt},
                            {4, 4, std::nullopt}};
        QVERIFY_THROWS_EXCEPTION(PyValueError, genko::core::fit_curve(far, 0.1));
        const PenPoints huge{{0, 0, std::nullopt}, {1e30, 5, std::nullopt}, {2e30, 0, std::nullopt}, {3e30, 7, std::nullopt}};
        try {
            genko::core::fit_curve(huge, 0.1);
            QFAIL("no error");
        } catch (const PyUncaught& error) {
            QCOMPARE(error.type(), std::string("MemoryError"));
        }
        const PenPoints endless{{0, 0, std::nullopt}, {inf, 0, std::nullopt}};
        try {
            genko::core::split_by_eraser(endless, PenPoints{{1, 1, std::nullopt}}, 1.0);
            QFAIL("no error");
        } catch (const PyUncaught& error) {
            QCOMPARE(error.type(), std::string("OverflowError"));
        }
        // the defaults are Python's: stabilize_points(points) is window 5, fit_curve(points) tolerance 0.3, step 0.5
        const PenPoints wavy{{0, 0, 0.5}, {1, 2, 0.6}, {2, 0, 0.7}, {3, 2, 0.8}, {4, 0, 0.9}, {5, 2, 1.0}};
        QVERIFY(genko::core::stabilize_points(wavy) == genko::core::stabilize_points(wavy, 5, false));
        QVERIFY(genko::core::fit_curve(wavy) == genko::core::fit_curve(wavy, 0.3, 0.5));
        QVERIFY(genko::core::taper_points(wavy) == genko::core::taper_points(wavy, std::nullopt, std::nullopt));
        QVERIFY(genko::core::apply_pressure_curve(wavy) == genko::core::apply_pressure_curve(wavy, "gpen"));
        QVERIFY(genko::core::pack_point(1, 2) == genko::core::pack_point(1, 2, std::nullopt, 0.0));
    }
};

QTEST_GUILESS_MAIN(TestStrokeGeom)
#include "test_stroke_geom.moc"
