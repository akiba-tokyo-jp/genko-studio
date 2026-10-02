// render::brushes against Python's genko.brushes: every built-in brush and twenty custom ones (J3: flat and image
// tips, spacing, scatter, patterns, speed, anti-aliasing, textures), each with and without pressure at 72, 150 and
// 600 dpi; brushes.draw's coverage picture and its origin must be the same, every pixel. The custom brushes are
// registered the same way on both sides (bad definitions skipped) and to_dict gives the same settings (the brushes
// themselves are core/brushes.hpp's, the ops' too).

#include <QtTest>

#include <QTemporaryDir>

#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/brushes.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace brushes = genko::render::brushes;
using genko::test::from_bits;

namespace {

genko::core::PenPoints points_of(const Json& list) {
    genko::core::PenPoints out;
    for (const Json& p : list) {
        genko::core::PenPoint q{from_bits(p[0]), from_bits(p[1]), std::nullopt};
        if (p.size() > 2) q.p = from_bits(p[2]);
        out.push_back(q);
    }
    return out;
}

}  // namespace

class TestContractBrushes : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json data_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const auto py = genko::test::render_harness({"brush-cases", scratch_.path() + "/out", "--seed", "20261002"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        data_ = genko::test::read_json(scratch_.path() + "/out/brushes.json");
        brushes::clear_custom();
        brushes::register_brushes(data_["definitions"]);
    }

    void cleanupTestCase() { brushes::clear_custom(); }

    void registered() {
        std::vector<std::string> got;
        for (const brushes::Brush& b : brushes::everything()) {
            bool builtin = false;
            for (const brushes::Brush& o : genko::core::builtin_brushes()) builtin = builtin || o.key == b.key;
            if (!builtin) got.push_back(b.key);
        }
        QCOMPARE(got, data_["registered"].get<std::vector<std::string>>());
        for (const auto& [key, want] : data_["to_dict"].items()) {
            std::string where;
            QVERIFY2(genko::test::strict_equal(genko::core::brush_to_dict(brushes::brush(key)), want, &where), (key + ": " + where).c_str());
        }
        QCOMPARE(brushes::brush("oil").key, std::string("marker"));
        QCOMPARE(brushes::brush("").key, std::string("gpen"));
        QCOMPARE(brushes::brush("no such brush").key, std::string("gpen"));
    }

    void coverage() {
        int failures = 0;
        int drawn = 0;
        const Json& cases = data_["cases"];
        // every brush, with and without pressure, at three resolutions
        QCOMPARE(cases.size(), (genko::core::builtin_brushes().size() + data_["registered"].size()) * 6);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const Json& c = cases[i];
            std::vector<double> rotation;
            if (!c["rotation"].is_null()) {
                for (const Json& r : c["rotation"]) rotation.push_back(from_bits(r));
            }
            const auto got = brushes::draw({c["size"][0].get<int>(), c["size"][1].get<int>()}, points_of(c["points"]),
                                           c["dpi"].get<int>(), from_bits(c["width_mm"]), c["kind"].get<std::string>(),
                                           c["seed"].get<std::string>(), rotation, from_bits(c["pressure_opacity"]));
            const std::string label = c["kind"].get<std::string>() + " @" + std::to_string(c["dpi"].get<int>()) + " #" + std::to_string(i);
            if (c["result"].is_null()) {
                if (got) {
                    qWarning("%s: drawn here, nothing in Python", label.c_str());
                    ++failures;
                }
                continue;
            }
            if (!got) {
                qWarning("%s: nothing drawn here", label.c_str());
                ++failures;
                continue;
            }
            ++drawn;
            const Json& r = c["result"];
            const render::Image want = render::read_png(
                genko::test::read_bytes(scratch_.path() + "/out/" + QString::fromStdString(r["file"].get<std::string>())));
            if (got->origin.x != r["origin"][0].get<int>() || got->origin.y != r["origin"][1].get<int>() ||
                got->mask.size() != want.size()) {
                qWarning("%s: origin (%d, %d) size %dx%d, Python (%d, %d) %dx%d", label.c_str(), got->origin.x, got->origin.y,
                         got->mask.width(), got->mask.height(), r["origin"][0].get<int>(), r["origin"][1].get<int>(),
                         want.width(), want.height());
                ++failures;
                continue;
            }
            if (got->mask.tobytes() != want.tobytes()) {
                const auto diff = genko::test::pixel_diff(got->mask, want);
                genko::test::keep_pictures(QStringLiteral("brush-%1").arg(i), got->mask, want);
                qWarning("%s: %lld pixels differ (largest %d)", label.c_str(), diff.pixels, diff.largest);
                ++failures;
            }
        }
        qInfo("%zu strokes, %d drawn (the rest draw nothing on both sides)", cases.size(), drawn);
        QVERIFY(drawn > 200);
        QCOMPARE(failures, 0);
    }

    void library_not_yet() {
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, brushes::library_path());
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, brushes::save_to_library("x", std::nullopt));
    }
};

QTEST_GUILESS_MAIN(TestContractBrushes)
#include "test_contract_brushes.moc"
