// core::rulers against Python's genko.rulers, to the last bit: 500 random cases (tools/migration/tone_harness.py
// snaps: one to three rulers of every kind, with angles, ratios, grids, reaches, symmetry copies and mirrors, some
// for one layer, some inactive; strokes of 2 to 17 points with and without pressures, some starting at a ruler):
// snap (with `only` and the layer), symmetry_copies, and per ruler outline (three page sizes), perspective_grid (and
// with 5 lines), horizon, shape_outline, directions, smooth_curve; snap_to_grid. Where Python raises, the same kind
// of error.
// And the pen lines and erasers that snap to the rulers through the ops (add_stroke with snap_ruler / ruler_id, erase
// with snap_ruler: the cases of contract/ruler_cases.json, moved from M2-O1's ops_cases.json when its rulers became
// these), against Python's apply_ops on the book `pyref_harness.py make-opsbook` makes: each step's reply, full snapshot
// and project.json payload; and the book after the case saved and read back, as test_contract_ops does with its cases
// (as it was, and as Python's saved and read back). Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/rulers.hpp"
#include "opsupport.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

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
    if (const auto* e = dynamic_cast<const genko::core::PyUncaught*>(&error)) return e->type();  // (IndexError, …)
    if (const auto* e = dynamic_cast<const genko::core::Error*>(&error)) {
        if (e->code() == "value") return "ValueError";
        if (e->code() == "type") return "TypeError";
        return "Error(" + e->code() + ")";
    }
    return std::string("std::exception: ") + error.what();
}

// The steps of a case of ruler_cases.json (ops_cases.json's format): "steps", or one step of "ops", "agent", "dry".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

template <class F>
Json outcome(F&& f) {
    try {
        return f();
    } catch (const std::exception& error) {
        return Json::object({{"raised", python_name(error)}});
    }
}

// A layer id as Python passes it (None: nullptr).
const Json* none_or(const Json& value) { return value.is_null() ? nullptr : &value; }

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
            const Json* layer = none_or(c["layer_id"]);
            const auto check = [&](const std::string& what, const Json& want, const Json& got) {
                ++compared[what];
                if (want.is_object() && want.contains("raised")) ++raised;
                if (got != want) {
                    qWarning("case %zu %s: Python %s, this build %s", i, what.c_str(), want.dump().substr(0, 800).c_str(),
                             got.dump().substr(0, 800).c_str());
                    ++failures;
                }
            };
            const Json snapped = outcome([&] { return bits(rulers::snap(points, list, {}, c["only"], layer)); });
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

    // The pen lines and erasers that snap to the rulers, through the ops, against Python's apply_ops.
    void penLinesAlongTheRulers() {
        const QString pyenv = scratch_.path() + QStringLiteral("/pyenv");
        const QString opsbook = scratch_.path() + QStringLiteral("/opsbook.genko");
        const auto made = genko::test::harness({"make-opsbook", opsbook}, pyenv, 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/ruler_cases.json");
        const Json& cases = file["cases"];
        const std::uint64_t first_id = file["first_id"].get<std::uint64_t>();
        const QString dir = scratch_.path() + QStringLiteral("/ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = opsbook.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            if (cases[n]["ok"].get<bool>()) job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, pyenv, 1200000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const auto loaded = [&] {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            return genko::storage::load_document(genko::storage::path_from_utf8(opsbook.toStdString()));
        }();
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, int> same;  // op → cases the same as Python's
        int read_back = 0;  // books saved and read back as they were
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            Json records = genko::test::read_json(QString::fromStdString(jobs[n]["out"].get<std::string>()));
            Json reread;  // (the record of Python's book saved and read back, after the steps')
            if (!records.empty() && records.back().contains("reread")) {
                reread = records.back();
                records.erase(records.size() - 1);
            }
            genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-store-%1").arg(n)).toStdString()));
            genko::core::Document last;
            const auto outcomes = genko::test::run_steps(loaded.document, steps_of(c), first_id, store, false, &last);
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            if (records.back()["reply"]["ok"].get<bool>() != c["ok"].get<bool>()) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(records.back()["reply"]).substr(0, 400));
                continue;
            }
            bool ok = true;
            for (std::size_t s = 0; s < outcomes.size() && ok; ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    ok = false;
                }
            }
            if (!ok) continue;
            ++same[c["op"].get<std::string>()];
            if (!c["ok"].get<bool>()) continue;
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [op, n] : same) qInfo("%s along the rulers: %d cases the same as Python", op.c_str(), n);
        qInfo("saved and read back as they were: %d (%d selections not saved, %d pages without layers read back with the "
              "default ones, %d int margins read back as floats)",
              read_back, notes.unselected, notes.refilled, notes.floated);
        QVERIFY(failures.empty());
        QCOMPARE(same["add_stroke"] + same["erase"], static_cast<int>(cases.size()));
        int applied = 0;
        for (const Json& c : cases) applied += c["ok"].get<bool>() ? 1 : 0;
        QCOMPARE(read_back, applied);
    }
};

QTEST_GUILESS_MAIN(TestContractRulers)
#include "test_contract_rulers.moc"
