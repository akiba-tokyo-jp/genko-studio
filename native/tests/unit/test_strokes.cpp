// Strokes in their saved forms (Python's stroke_to_packed / coerce_stroke): packed blobs byte for byte, the v2
// dict form and the v1 point lists.

#include <QtTest>

#include <string>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/strokes.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::core::Stroke;

namespace {

const char* kV3Blob = "legacy/book-v3.genko/assets/90/9036e17cadbc44ed96aa3ee57411c74f062d06acb589fb0075de1e8ad6fb0258.strokes.json";

bool same_stroke(const Stroke& a, const Stroke& b) {
    if (a.points.size() != b.points.size()) return false;
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y) return false;
    }
    return a.id == b.id && a.pressure == b.pressure && a.width_mm == b.width_mm && a.kind == b.kind &&
           a.rgb == b.rgb && a.opacity == b.opacity && a.rotation == b.rotation &&
           a.pressure_opacity == b.pressure_opacity;
}

std::string error_of(const Json& raw) {
    try {
        genko::core::coerce_stroke(raw);
    } catch (const genko::core::Error& error) {
        return error.what();
    }
    return "(no error)";
}

}  // namespace

class TestStrokes : public QObject {
    Q_OBJECT

private slots:
    void packedValuesAsPython() {
        Stroke s;
        s.id = "acacf6c33381";
        s.points = {{30.0, 30.0}, {60.25, 45.5}, {90.0, 40.0}};
        s.pressure = {0.2, 0.9, 0.5};
        s.width_mm = 0.5;
        const Json packed = genko::core::stroke_to_packed(s);
        // the v3 test book's blob holds this very stroke (written by Python)
        QCOMPARE(packed["xy"].get<std::string>(), std::string("AAAAAAAAPkAAAAAAAAA+QAAAAAAAIE5AAAAAAADARkAAAAAAAIBWQAAAAAAAAERA"));
        QCOMPARE(packed["p"].get<std::string>(), std::string("mpmZmZmZyT/NzMzMzMzsPwAAAAAAAOA/"));
        QCOMPARE(genko::core::dump_python(packed),
                 std::string("{\"id\": \"acacf6c33381\", \"kind\": \"gpen\", \"width_mm\": 0.5, \"xy\": "
                             "\"AAAAAAAAPkAAAAAAAAA+QAAAAAAAIE5AAAAAAADARkAAAAAAAIBWQAAAAAAAAERA\", \"p\": "
                             "\"mpmZmZmZyT/NzMzMzMzsPwAAAAAAAOA/\"}"));
    }

    void packedRoundTrip() {
        Stroke s;
        s.id = "s1";
        s.points = {{-1.5, 1e-300}, {1.7976931348623157e308, 5e-324}, {0.1 + 0.2, -0.0}};
        s.pressure = {0.0, 0.5, 1.0};
        s.width_mm = 0.1 + 0.2;
        s.kind = "毛筆";
        s.rgb = std::vector<std::int64_t>{200, 10, 10};
        s.opacity = 1.0 / 3;
        s.rotation = {-180.0, 0.0, 179.5};
        s.pressure_opacity = 0.3;
        const Json packed = genko::core::stroke_to_packed(s);
        std::vector<std::string> keys;
        for (const auto& [key, value] : packed.items()) keys.push_back(key);
        QCOMPARE(keys, std::vector<std::string>({"id", "kind", "width_mm", "xy", "p", "rgb", "opacity", "r", "po"}));
        QVERIFY(same_stroke(genko::core::coerce_stroke(packed), s));
        QVERIFY(same_stroke(genko::core::coerce_stroke(genko::core::parse_python_json(genko::core::dump_canonical(packed))), s));

        Stroke plain;
        plain.id = "p";
        plain.points = {{1, 2}};
        const Json minimal = genko::core::stroke_to_packed(plain);
        QCOMPARE(minimal.size(), std::size_t{4});  // id, kind, width_mm, xy: the defaults are not written
        QVERIFY(same_stroke(genko::core::coerce_stroke(minimal), plain));
    }

    void blobBytesMatchTheV3Fixture() {
        const std::string blob = genko::test::read_bytes(genko::test::test_data(kV3Blob));
        QVERIFY(!blob.empty());
        genko::core::StrokeList list;
        for (const Json& raw : genko::core::parse_python_json(blob)) {
            list.items.push_back(std::make_shared<const Stroke>(genko::core::coerce_stroke(raw)));
        }
        QCOMPARE(list.items.size(), std::size_t{2});
        QCOMPARE(list.items[1]->rgb.value(), std::vector<std::int64_t>({200, 10, 10}));
        QCOMPARE(genko::core::strokes_blob(list), blob);  // Python's canonical JSON, byte for byte
    }

    void dictForm() {
        genko::core::ScopedIdSource ids(genko::core::counting_ids());
        const Json v2 = genko::core::parse_python_json(
            R"({"id": "s1", "points": [[30.0, 30.0], [60.25, 45.5]], "pressure": [0.2, 0.9], "width_mm": 0.5, "kind": "gpen"})");
        const Stroke s = genko::core::coerce_stroke(v2);
        QCOMPARE(s.id, std::string("s1"));
        QCOMPARE(s.points.size(), std::size_t{2});
        QCOMPARE(s.pressure, std::vector<double>({0.2, 0.9}));
        QCOMPARE(s.width_mm, 0.5);
        // pressure from the points' third values, as many as there are (Python does not check the count here)
        const Stroke partial = genko::core::coerce_stroke(genko::core::parse_python_json(
            R"({"points": [[1, 2, 0.5], [3, 4]], "pressure_opacity": 0.4, "rgb": [1.9, "2", true], "kind": null})"));
        QCOMPARE(partial.id, std::string("000000000001"));
        QCOMPARE(partial.pressure, std::vector<double>({0.5}));
        QCOMPARE(partial.pressure_opacity, 0.4);
        QCOMPARE(partial.rgb.value(), std::vector<std::int64_t>({1, 2, 1}));
        QCOMPARE(partial.kind, std::string("gpen"));
        QCOMPARE(genko::core::dump_python(genko::core::stroke_points_json(partial)), std::string("[[1.0, 2.0], [3.0, 4.0]]"));
        QCOMPARE(error_of(genko::core::parse_python_json(R"({"points": [[1]]})")),
                 std::string("not enough values to unpack (expected 2, got 1)"));
    }

    void listForm() {
        genko::core::ScopedIdSource ids(genko::core::counting_ids(7));
        const Stroke v1 = genko::core::coerce_stroke(genko::core::parse_python_json("[[10.0, 10.0], [20.5, 22.25], [31.125, 40.0]]"));
        QCOMPARE(v1.id, std::string("000000000007"));
        QCOMPARE(v1.points.size(), std::size_t{3});
        QVERIFY(v1.pressure.empty());
        QCOMPARE(v1.width_mm, 0.35);
        QCOMPARE(v1.kind, std::string("gpen"));
        const Stroke with = genko::core::coerce_stroke(genko::core::parse_python_json("[[1, 2, 0.25], [3, 4, 0.75]]"));
        QCOMPARE(with.pressure, std::vector<double>({0.25, 0.75}));
        QCOMPARE(genko::core::dump_python(genko::core::stroke_points_json(with)), std::string("[[1.0, 2.0, 0.25], [3.0, 4.0, 0.75]]"));
        // a pressure for only some points is dropped
        const Stroke some = genko::core::coerce_stroke(genko::core::parse_python_json("[[1, 2, 0.25], [3, 4]]"));
        QVERIFY(some.pressure.empty());
        QVERIFY(error_of(Json(5)).find("not iterable") != std::string::npos);
    }

    void packedEdgeCases() {
        // an odd number of values: the last x has no y and is dropped (Python's zip)
        Json packed = Json::object();
        packed["xy"] = genko::core::pack_doubles(std::vector<double>{1, 2, 3});
        packed["id"] = "odd";
        QCOMPARE(genko::core::coerce_stroke(packed).points.size(), std::size_t{1});
        packed["xy"] = "abc";
        QCOMPARE(error_of(packed), std::string("Incorrect padding"));
        packed["xy"] = "AAAAAAA=";
        QCOMPARE(error_of(packed), std::string("bytes length not a multiple of item size"));
        packed["xy"] = 5;
        QCOMPARE(error_of(packed), std::string("argument should be bytes, buffer or ASCII string, not 'int'"));
        packed["xy"] = "";
        packed["p"] = "";      // empty: no pressure
        packed["po"] = 0;      // zero: none
        packed["opacity"] = 1;  // an int, as an old writer might have left it
        const Stroke s = genko::core::coerce_stroke(packed);
        QVERIFY(s.points.empty() && s.pressure.empty() && s.pressure_opacity == 0.0 && s.opacity == 1.0);
        QCOMPARE(genko::core::stroke_to_packed(s).size(), std::size_t{4});
    }
};

QTEST_GUILESS_MAIN(TestStrokes)
#include "test_strokes.moc"
