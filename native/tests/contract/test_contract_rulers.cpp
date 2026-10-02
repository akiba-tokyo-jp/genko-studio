// core::rulers against Python's genko.rulers, to the last bit: 500 random cases (tools/migration/tone_harness.py
// snaps: one to three rulers of every kind, with angles, ratios, grids, reaches, symmetry copies and mirrors, some
// for one layer, some inactive; strokes of 2 to 17 points with and without pressures, some starting at a ruler):
// snap (with `only` and the layer), symmetry_copies, and per ruler outline (three page sizes), perspective_grid (and
// with 5 lines), horizon, shape_outline, directions, smooth_curve; snap_to_grid. Where Python raises, the same kind
// of error. Skipped without the Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <map>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/rulers.hpp"
#include "rendertest.hpp"

using genko::core::Json;
using genko::core::PenPoint;
using genko::core::PenPoints;
using genko::test::bits_of;
using genko::test::from_bits;
namespace rulers = genko::core::rulers;

namespace {

PenPoints pen_points(const Json& list) {
    PenPoints out;
    for (const Json& p : list) {
        PenPoint q{from_bits(p[0]), from_bits(p[1]), std::nullopt};
        if (p.size() > 2) q.p = from_bits(p[2]);
        out.push_back(q);
    }
    return out;
}

Json xy_bits(double x, double y) { return Json::array({bits_of(x), bits_of(y)}); }

Json bits(const PenPoints& points) {
    Json out = Json::array();
    for (const PenPoint& p : points) {
        Json q = xy_bits(p.x, p.y);
        if (p.p) q.push_back(bits_of(*p.p));
        out.push_back(std::move(q));
    }
    return out;
}

Json bits(const rulers::Polyline& points) {
    Json out = Json::array();
    for (const rulers::XY& p : points) out.push_back(xy_bits(p.x, p.y));
    return out;
}

// The Python exception this build's error stands for.
std::string python_name(const std::exception& error) {
    if (dynamic_cast<const genko::core::OpKeyError*>(&error) != nullptr) return "KeyError";
    if (const auto* e = dynamic_cast<const genko::core::Error*>(&error)) {
        if (e->code() == "value") return "ValueError";
        if (e->code() == "type") return "TypeError";
        if (e->code() == "index") return "IndexError";
        return "Error(" + e->code() + ")";
    }
    return std::string("std::exception: ") + error.what();
}

template <class F>
Json outcome(F&& f) {
    try {
        return f();
    } catch (const std::exception& error) {
        return Json::object({{"raised", python_name(error)}});
    }
}

std::optional<std::string> str_or_none(const Json& value) {
    if (value.is_null()) return std::nullopt;
    return value.get<std::string>();
}

}  // namespace

class TestContractRulers : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json cases_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + QStringLiteral("/snaps.json");
        const auto made = genko::test::run(
            genko::test::python_ref(),
            {genko::test::repo_root() + QStringLiteral("/tools/migration/tone_harness.py"), "snaps", out, "--seed", "20261003", "--count", "500"},
            genko::test::python_env(scratch_.path()), 1800000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        cases_ = genko::test::read_json(out);
        QCOMPARE(cases_.size(), std::size_t{500});
    }

    void snapping() {
        std::map<std::string, int> compared;
        std::map<std::string, int> kinds;
        int raised = 0;
        int moved = 0;
        int failures = 0;
        for (std::size_t i = 0; i < cases_.size(); ++i) {
            const Json& c = cases_[i];
            const Json& list = c["rulers"];
            const PenPoints points = pen_points(c["points"]);
            const auto layer = str_or_none(c["layer_id"]);
            const auto check = [&](const std::string& what, const Json& want, const Json& got) {
                ++compared[what];
                if (want.is_object() && want.contains("raised")) ++raised;
                if (got != want) {
                    qWarning("case %zu %s: Python %s, this build %s", i, what.c_str(), want.dump().substr(0, 800).c_str(),
                             got.dump().substr(0, 800).c_str());
                    ++failures;
                }
            };
            const Json snapped = outcome([&] { return bits(rulers::snap(points, list, {}, str_or_none(c["only"]), layer)); });
            check("snap", c["snap"], snapped);
            if (snapped != c["points"]) ++moved;
            check("symmetry_copies", c["symmetry"], outcome([&] {
                      Json out = Json::array();
                      for (const PenPoints& copy : rulers::symmetry_copies(points, list, {}, layer)) out.push_back(bits(copy));
                      return out;
                  }));
            const rulers::PageSize page{*genko::core::Num::from_json(c["page_size"][0]), *genko::core::Num::from_json(c["page_size"][1])};
            for (std::size_t k = 0; k < list.size(); ++k) {
                const Json& ruler = list[k];
                const Json& want = c["per_ruler"][k];
                const std::string kind = ruler["kind"].get<std::string>();
                ++kinds[kind];
                check("outline", want["outline"], outcome([&] {
                          Json out = Json::array();
                          for (const auto& line : rulers::outline(ruler, page)) {
                              Json pts = Json::array();
                              for (const auto& p : line) pts.push_back(xy_bits(p.x.value(), p.y.value()));
                              out.push_back(std::move(pts));
                          }
                          return out;
                      }));
                if (kind == "perspective") {
                    const auto grid = [&](std::optional<std::int64_t> lines) {
                        return outcome([&] {
                            Json out = Json::array();
                            for (const auto& seg : rulers::perspective_grid(ruler, page, lines)) {
                                out.push_back(Json::array({xy_bits(seg[0].x, seg[0].y), xy_bits(seg[1].x, seg[1].y)}));
                            }
                            return out;
                        });
                    };
                    check("perspective_grid", want["grid"], grid(std::nullopt));
                    check("perspective_grid(5)", want["grid5"], grid(5));
                    check("horizon", want["horizon"], outcome([&] {
                              const auto eye = rulers::horizon(ruler);
                              return eye ? Json::array({xy_bits(eye->first.x, eye->first.y), xy_bits(eye->second.x, eye->second.y)}) : Json();
                          }));
                }
                if (kind == "rect" || kind == "ellipse" || kind == "polygon") {
                    check("shape_outline", want["shape"], outcome([&] { return bits(rulers::shape_outline(ruler)); }));
                }
                if (kind == "parallel" || kind == "radial" || kind == "perspective") {
                    check("directions", want["directions"],
                          outcome([&] { return bits(rulers::directions(ruler, rulers::XY{points[0].x, points[0].y})); }));
                }
                if (kind == "curve" || kind == "parallel_curve" || kind == "multi_curve" || kind == "radial_curve") {
                    check("smooth_curve", want["smooth"], outcome([&] { return bits(rulers::smooth_curve(ruler["points"], from_bits(want["per_mm"]))); }));
                }
            }
            const Json& grid = c["grid_snap"];
            const rulers::XY at = rulers::snap_to_grid(rulers::XY{points.back().x, points.back().y}, from_bits(grid["spacing"]),
                                                       rulers::XY{from_bits(grid["origin"][0]), from_bits(grid["origin"][1])});
            check("snap_to_grid", grid["result"], xy_bits(at.x, at.y));
        }
        for (const auto& [what, n] : compared) qInfo("%s: %d", what.c_str(), n);
        for (const auto& [kind, n] : kinds) qInfo("ruler %s: %d", kind.c_str(), n);
        qInfo("strokes moved by a ruler: %d of %zu; Python raised in %d checks", moved, cases_.size(), raised);
        QCOMPARE(kinds.size(), rulers::kinds().size());
        QVERIFY(moved > 150);
        QCOMPARE(failures, 0);
    }
};

QTEST_GUILESS_MAIN(TestContractRulers)
#include "test_contract_rulers.moc"
