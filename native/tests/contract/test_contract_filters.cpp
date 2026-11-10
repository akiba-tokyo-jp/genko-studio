// core::adjustment (core/filters.hpp: the colour adjustments of correction layers, one reading of their settings for
// the ops and the renderer) against Python's filters.apply_filter: for good, bad and odd settings of every adjustment
// (render_harness.py ADJUST_CASES), the tables Pillow makes of what Python hands Image.point, call by call, or the same
// exception with the same message.

#include <QtTest>

#include <QTemporaryDir>

#include <string>
#include <set>
#include <numeric>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace core = genko::core;

namespace {

// A table as Image.point keeps it (each value in 0..255)
Json kept(const std::vector<int>& table) {
    Json out = Json::array();
    for (const int v : table) out.push_back(v <= 0 ? 0 : v < 256 ? v : 255);
    return out;
}

// The calls apply_filter makes of Image.point for the adjustment, each with its bands' tables: one on the RGB picture
// (three bands); one on each of hue, saturation and value; on the grey picture, one for each colour of a gradient map
// and one for the others (the same table for red, green and blue).
Json point_calls(std::string_view kind, const core::Adjustment& adjustment) {
    const auto& [r, g, b] = adjustment.tables;
    switch (adjustment.way) {
        case core::Adjustment::Way::Rgb: return Json::array({Json::array({kept(r), kept(g), kept(b)})});
        case core::Adjustment::Way::Hsv: break;
        case core::Adjustment::Way::Grey:
            if (kind != "gradient_map" && r == g && g == b) return Json::array({Json::array({kept(r)})});
            break;
    }
    return Json::array({Json::array({kept(r)}), Json::array({kept(g)}), Json::array({kept(b)})});
}

// What Python's apply_filter gives for the settings, as the harness writes it: {"tables": …} or {"error": [type, message]}
Json outcome(const std::string& kind, const Json& params) {
    const auto error = [](const std::string& type, const char* message) {
        return Json::object({{"error", Json::array({type, message})}});
    };
    try {
        return Json::object({{"tables", point_calls(kind, core::adjustment(kind, params))}});
    } catch (const core::PyValueError& e) {
        return error("ValueError", e.what());
    } catch (const core::PyTypeError& e) {
        return error("TypeError", e.what());
    } catch (const core::PyUncaught& e) {
        return error(e.type(), e.what());
    } catch (const core::OpKeyError& e) {
        return error("KeyError", e.what());
    } catch (const core::Error& e) {
        return error("core::Error(" + e.code() + ")", e.what());  // (not one of Python's: always a difference)
    }
}

}  // namespace

class TestContractFilters : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json cases_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + "/adjust.json";
        const auto py = genko::test::render_harness({"adjust-cases", out}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        cases_ = genko::test::read_json(out)["cases"];
        QVERIFY(cases_.size() > 100);
    }

    void sameAsPython() {
        int failures = 0;
        int tables = 0;
        int errors = 0;
        for (const Json& c : cases_) {
            const std::string kind = c["kind"].get<std::string>();
            const Json want = c.contains("tables") ? Json::object({{"tables", c["tables"]}}) : Json::object({{"error", c["error"]}});
            const Json got = outcome(kind, c["params"]);
            if (got != want) {
                ++failures;
                qWarning("%s %s: C++ %s, Python %s", kind.c_str(), c["params"].dump().c_str(),
                         got.dump().substr(0, 400).c_str(), want.dump().substr(0, 400).c_str());
                continue;
            }
            ++(want.contains("tables") ? tables : errors);
        }
        QCOMPARE(failures, 0);
        // Python's reference kinds retain all original good/bad coverage. Native-only extensions
        // are classified explicitly and tested here without inventing Python reference cases.
        std::set<std::string> python_kinds;
        std::set<std::string> native_kinds;
        for (const Json& c : cases_) python_kinds.insert(c["kind"].get<std::string>());
        for (const std::string_view kind : core::kAdjustments) {
            if (!python_kinds.contains(std::string(kind))) {
                native_kinds.insert(std::string(kind));
                continue;
            }
            bool good = false;
            bool bad = false;
            for (const Json& c : cases_) {
                if (c["kind"].get<std::string>() != kind) continue;
                (c.contains("tables") ? good : bad) = true;
            }
            QVERIFY2(good && (bad || kind == "invert"), std::string(kind).c_str());
        }
        QCOMPARE(core::kAdjustments.size() - native_kinds.size(), std::size_t(9));
        QVERIFY(native_kinds == std::set<std::string>{"exposure"});
        std::vector<int> identity(256);
        std::iota(identity.begin(), identity.end(), 0);
        const auto native_identity = core::adjustment("exposure", Json::object());
        for (const auto& band : native_identity.tables) QCOMPARE(band, identity);
        for (const Json& bad : {Json{{"gamma", "nan"}}, Json{{"unknown-key", 1}}}) {
            bool refused = false;
            try { (void)core::adjustment("exposure", bad); }
            catch (const core::PyValueError&) { refused = true; }
            QVERIFY(refused);
        }
        qInfo("native exposure: identity on all 3x256 entries and both invalid-input controls passed");
        qInfo("%d settings the same as Python: %d of them tables, %d an exception", tables + errors, tables, errors);
    }
};

QTEST_GUILESS_MAIN(TestContractFilters)
#include "test_contract_filters.moc"
