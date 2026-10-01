// Python's numbers in C++: round(), repr(float), int/float arithmetic and comparison, sum(), %, the math module
// functions the frame geometry uses, base64 and the built-in conversions the reader applies. Table cases are
// Python's own results (tests/data/pyref/unit_tables.json, from tools/migration/pyref_harness.py unit-tables).

#include <QtTest>

#include <cmath>
#include <limits>
#include <string>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::core::Num;

namespace {

double parse(const std::string& repr) { return genko::core::parse_python_json(repr).get<double>(); }

Num num_of(const Json& item) {
    if (item[0] == "i") return Num(item[1].get<std::int64_t>());
    return Num(parse(item[1].get<std::string>()));
}

}  // namespace

class TestPyNum : public QObject {
    Q_OBJECT

private slots:
    void roundTable() {
        const Json table = genko::test::read_json(genko::test::test_data("pyref/unit_tables.json"));
        int checked = 0;
        for (const Json& row : table["round"]) {
            const double x = parse(row[0].get<std::string>());
            const int n = row[1].get<int>();
            const std::string expected = row[2].get<std::string>();
            if (expected == "OverflowError") {
                QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::py_round(x, n));
            } else {
                QCOMPARE(genko::core::py_float_repr(genko::core::py_round(x, n)), expected);
            }
            ++checked;
        }
        QVERIFY(checked >= 40);
    }

    void roundExplicit() {
        QCOMPARE(genko::core::py_round(2.675, 2), 2.67);   // 2.675 is 2.67499999… in binary
        QCOMPARE(genko::core::py_round(0.125, 2), 0.12);   // an exact tie: to even
        QCOMPARE(genko::core::py_round(0.375, 2), 0.38);
        QCOMPARE(genko::core::py_round(2.5, 0), 2.0);
        QCOMPARE(genko::core::py_round(150.0, -2), 200.0);
        QVERIFY(std::signbit(genko::core::py_round(-0.001, 2)));
        QVERIFY(std::isnan(genko::core::py_round(std::nan(""), 2)));
        QCOMPARE(genko::core::py_round(HUGE_VAL, 2), HUGE_VAL);
        // round() of an int stays an int (ties to the even multiple)
        QVERIFY(genko::core::py_round(Num(1250), 2).same(Num(1250)));
        QVERIFY(genko::core::py_round(Num(1250), -2).same(Num(1200)));
        QVERIFY(genko::core::py_round(Num(1350), -2).same(Num(1400)));
        QVERIFY(genko::core::py_round(Num(-1251), -2).same(Num(-1300)));
        QVERIFY(genko::core::py_round(Num(2.675), 2).same(Num(2.67)));
    }

    void reprAndFormat() {
        QCOMPARE(genko::core::py_float_repr(100.0), std::string("100.0"));
        QCOMPARE(genko::core::py_float_repr(1e-05), std::string("1e-05"));
        QCOMPARE(genko::core::py_float_repr(-1e+100), std::string("-1e+100"));
        QCOMPARE(genko::core::py_float_repr(1.5e16), std::string("1.5e+16"));
        QCOMPARE(genko::core::py_float_repr(9999999999999998.0), std::string("9999999999999998.0"));
        QCOMPARE(genko::core::py_float_repr(0.001), std::string("0.001"));
        QCOMPARE(genko::core::py_float_repr(std::nan("")), std::string("nan"));
        QCOMPARE(genko::core::py_float_repr(-HUGE_VAL), std::string("-inf"));
        QCOMPARE(genko::core::py_format_g(182.0), std::string("182"));
        QCOMPARE(genko::core::py_format_g(0.35), std::string("0.35"));
        QCOMPARE(genko::core::py_format_g(1234567.0), std::string("1.23457e+06"));
        QCOMPARE(genko::core::py_format_g(-0.0), std::string("-0"));
        QCOMPARE(genko::core::py_format_g(260.25), std::string("260.25"));
        QCOMPARE(Num(3).repr(), std::string("3"));
        QCOMPARE(Num(3.0).repr(), std::string("3.0"));
    }

    void arithmeticKeepsIntsAndFloatsApart() {
        QVERIFY((Num(210) - Num(2) * Num(3)).same(Num(204)));        // a4: paper less bleed stays an int
        QVERIFY((Num(210) - Num(2) * Num(3.0)).same(Num(204.0)));
        QVERIFY((Num(7) / Num(2)).same(Num(3.5)));                   // true division
        QVERIFY((Num(6) / Num(2)).same(Num(3.0)));
        QVERIFY((Num(13) + Num(0.5)).same(Num(13.5)));
        QVERIFY((-Num(0)).same(Num(0)));
        QVERIFY((-Num(0.0)).same(Num(-0.0)));
        QVERIFY((Num(std::numeric_limits<std::int64_t>::max()) + Num(1)).same(Num(9223372036854775808.0)));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, Num(1) / Num(0));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, Num(1.0) / Num(0.0));
        QVERIFY(genko::core::py_abs(Num(-3)).same(Num(3)));
        QVERIFY(Num(0.0).truthy() == false && Num(-0.0).truthy() == false && Num(2).truthy());
    }

    void comparisonsAreExact() {
        QVERIFY(Num(1) == Num(1.0));
        QVERIFY(!(Num(1).same(Num(1.0))));
        QVERIFY(Num(-0.0) == Num(0));
        const Num big(std::int64_t{9007199254740993});  // 2**53 + 1: not a double
        QVERIFY(big > Num(9007199254740992.0));
        QVERIFY(Num(9007199254740992.0) < big);
        QVERIFY(!(big == Num(9007199254740992.0)));
        QVERIFY(Num(std::numeric_limits<std::int64_t>::max()) < Num(9223372036854775808.0));
        QVERIFY(!(Num(std::nan("")) == Num(std::nan(""))));
        QVERIFY(!(Num(1) < Num(std::nan(""))));
        QVERIFY(Num(2) <= Num(2.0) && Num(2) >= Num(2.0));
    }

    void numFromJson() {
        QVERIFY(Num::from_json(Json(3))->same(Num(3)));
        QVERIFY(Num::from_json(Json(3.0))->same(Num(3.0)));
        QVERIFY(!Num::from_json(Json(true)).has_value());
        QVERIFY(!Num::from_json(Json("3")).has_value());
        QVERIFY(!Num::from_json(Json(std::uint64_t{18446744073709551615ULL})).has_value());
        QVERIFY(Num(5).json().is_number_integer());
        QVERIFY(Num(5.0).json().is_number_float());
    }

    void sumTable() {
        const Json table = genko::test::read_json(genko::test::test_data("pyref/unit_tables.json"));
        int checked = 0;
        for (const Json& row : table["sum"]) {
            std::vector<Num> items;
            for (const Json& item : row[0]) items.push_back(num_of(item));
            const Num expected = num_of(row[1]);
            const Num got = genko::core::py_sum(items);
            QVERIFY2(got.same(expected), (got.repr() + " != " + expected.repr()).c_str());
            ++checked;
        }
        QVERIFY(checked >= 10);
    }

    void moduloAsPython() {
        QVERIFY(genko::core::py_mod(Num(-1), Num(2)).same(Num(1)));
        QVERIFY(genko::core::py_mod(Num(5), Num(-3)).same(Num(-1)));
        QVERIFY(genko::core::py_mod(Num(-1.0), Num(2)).same(Num(1.0)));
        QVERIFY(genko::core::py_mod(Num(3.0), Num(2)).same(Num(1.0)));
        QVERIFY(genko::core::py_mod(Num(-0.0), Num(2)).same(Num(0.0)));
        QVERIFY(genko::core::py_mod(Num(4.0), Num(-2)).same(Num(-0.0)));
        QCOMPARE(genko::core::py_fmod(-0.5, 2 * genko::core::kPi), -0.5 + 2 * genko::core::kPi);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::py_mod(Num(1), Num(0)));
    }

    void mathAsCPython() {
        QCOMPARE(genko::core::py_hypot(3, 4), 5.0);
        QCOMPARE(genko::core::py_hypot(0.1, 0.2), 0.223606797749979);
        QCOMPARE(genko::core::py_dist(0, 0, 1, 1), 1.4142135623730951);
        QCOMPARE(genko::core::py_hypot(0, 0), 0.0);
        QCOMPARE(genko::core::py_atan2(0.0, -1.0), genko::core::kPi);
        QCOMPARE(genko::core::py_atan2(-0.0, -1.0), -genko::core::kPi);
        QCOMPARE(genko::core::py_pow(0.25, 2), 0.0625);
        QCOMPARE(genko::core::py_pow(-2, 3), -8.0);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::py_pow(-2, 0.5));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::py_acos(1.5));
    }

    void base64AsPython() {
        QCOMPARE(genko::core::b64encode(""), std::string(""));
        QCOMPARE(genko::core::b64encode("f"), std::string("Zg=="));
        QCOMPARE(genko::core::b64encode("fo"), std::string("Zm8="));
        QCOMPARE(genko::core::b64encode("foo"), std::string("Zm9v"));
        QCOMPARE(genko::core::b64encode(std::string("\xff\xfe\x00", 3)), std::string("//4A"));
        QCOMPARE(genko::core::a2b_base64("Zm9v"), std::string("foo"));
        QCOMPARE(genko::core::a2b_base64("Zm 9\nv"), std::string("foo"));      // other characters are skipped
        QCOMPARE(genko::core::a2b_base64("Zg==trailing"), std::string("f"));   // a complete padding ends the data
        QCOMPARE(genko::core::a2b_base64("Zg=*="), std::string("f"));
        QCOMPARE(genko::core::a2b_base64(""), std::string(""));
        try {
            genko::core::a2b_base64("Zg");
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("Incorrect padding"));
        }
        try {
            genko::core::a2b_base64("Zm9vZ");
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.what()),
                     std::string("Invalid base64-encoded string: number of data characters (5) cannot be 1 more than a multiple of 4"));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::a2b_base64("Zm9v\xc3\xa9"));
    }

    void conversionsAsPython() {
        using namespace genko::core;
        QCOMPARE(py_float(Json(" 1_000.5 ")), 1000.5);
        QCOMPARE(py_float(Json("7")), 7.0);
        QCOMPARE(py_float(Json(true)), 1.0);
        QCOMPARE(py_float(Json(3)), 3.0);
        QVERIFY(std::isinf(py_float(Json("-Infinity"))));
        QVERIFY_THROWS_EXCEPTION(Error, py_float(Json("1__0")));
        QVERIFY_THROWS_EXCEPTION(Error, py_float(Json("0x10")));
        try {
            py_float(Json(nullptr));
            QFAIL("no error");
        } catch (const Error& error) {
            QCOMPARE(std::string(error.what()), std::string("float() argument must be a string or a real number, not 'NoneType'"));
        }
        QCOMPARE(py_int(Json(2.9)), std::int64_t{2});
        QCOMPARE(py_int(Json(-2.9)), std::int64_t{-2});
        QCOMPARE(py_int(Json(" +12 ")), std::int64_t{12});
        QCOMPARE(py_int(Json(false)), std::int64_t{0});
        try {
            py_int(Json("1.5"));
            QFAIL("no error");
        } catch (const Error& error) {
            QCOMPARE(std::string(error.what()), std::string("invalid literal for int() with base 10: '1.5'"));
        }
        QCOMPARE(py_str(Json(nullptr)), std::string("None"));
        QCOMPARE(py_str(Json(true)), std::string("True"));
        QCOMPARE(py_str(Json(1.0)), std::string("1.0"));
        QCOMPARE(py_str(parse_python_json(R"([1, "a", {"k": null}])")), std::string("[1, 'a', {'k': None}]"));
        QCOMPARE(py_repr_str("it's"), std::string("\"it's\""));
        QCOMPARE(py_repr_str("a'\"b"), std::string("'a\\'\"b'"));
        QCOMPARE(py_repr_str("東京\t\x01"), std::string("'東京\\t\\x01'"));
        QVERIFY(!py_truthy(Json::object()) && !py_truthy(Json("")) && !py_truthy(Json(0.0)) && py_truthy(Json("0")));
        QCOMPARE(py_list(Json("東京")).size(), std::size_t{2});
        QCOMPARE(py_dict(parse_python_json(R"([["a", 1], ["b", 2]])"))["b"].get<int>(), 2);
        QVERIFY_THROWS_EXCEPTION(Error, py_dict(Json(5)));
    }
};

QTEST_GUILESS_MAIN(TestPyNum)
#include "test_pynum.moc"
