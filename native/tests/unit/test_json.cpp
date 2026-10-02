// core::dump_* write the bytes Python's json.dumps writes; core::parse_python_json reads what json.loads reads.
// The table cases are Python's own output (tools/migration/pyref_harness.py unit-tables →
// tests/data/pyref/unit_tables.json).

#include <QtTest>

#include <cmath>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

std::string error_of(std::string_view text, const genko::core::ParseOptions& options = {}) {
    try {
        genko::core::parse_python_json(text, nullptr, options);
    } catch (const genko::core::Error& error) {
        return error.what();
    }
    return "(no error)";
}

}  // namespace

class TestJson : public QObject {
    Q_OBJECT

private slots:
    void pythonDumpsTable() {
        const Json table = genko::test::read_json(genko::test::test_data("pyref/unit_tables.json"));
        QVERIFY(table["json"].size() >= 30);
        int checked = 0;
        for (const Json& item : table["json"]) {
            const std::string text = item["text"].get<std::string>();
            const Json value = genko::core::parse_python_json(text);
            QCOMPARE(genko::core::dump_python_indent2(value), item["indent2"].get<std::string>());
            QCOMPARE(genko::core::dump_python(value), item["default"].get<std::string>());
            QCOMPARE(genko::core::dump_canonical(value), item["canonical"].get<std::string>());
            QCOMPARE(genko::core::dump_python(value, true), item["ascii"].get<std::string>());
            ++checked;
        }
        QVERIFY(checked >= 30);
    }

    void floatsAsPythonRepr() {
        const auto one = [](double v) { return genko::core::dump_python(Json(v)); };
        QCOMPARE(one(1.0), std::string("1.0"));
        QCOMPARE(one(0.35), std::string("0.35"));
        QCOMPARE(one(1e-5), std::string("1e-05"));
        QCOMPARE(one(1e-4), std::string("0.0001"));
        QCOMPARE(one(1e16), std::string("1e+16"));
        QCOMPARE(one(1e15), std::string("1000000000000000.0"));
        QCOMPARE(one(0.1 + 0.2), std::string("0.30000000000000004"));
        QCOMPARE(one(-0.0), std::string("-0.0"));
        QCOMPARE(one(2e16 + 8), std::string("2.000000000000001e+16"));
        QCOMPARE(one(123456789.0), std::string("123456789.0"));
        QCOMPARE(one(5e-324), std::string("5e-324"));
        QCOMPARE(genko::core::dump_python(Json(std::int64_t{9223372036854775807})), std::string("9223372036854775807"));
        QCOMPARE(genko::core::dump_python(Json(std::int64_t{-9223372036854775807 - 1})), std::string("-9223372036854775808"));
        QCOMPARE(genko::core::dump_python(Json(std::uint64_t{18446744073709551615ULL})), std::string("18446744073709551615"));
    }

    void stringsAsPython() {
        QCOMPARE(genko::core::dump_python(Json("改行\n\"引用\" \\ /")), std::string("\"改行\\n\\\"引用\\\" \\\\ /\""));
        QCOMPARE(genko::core::dump_python(Json(std::string("\x01\x1f\x7f", 3))), std::string("\"\\u0001\\u001f\x7f\""));
        QCOMPARE(genko::core::dump_python(Json(std::string("a\0b", 3))), std::string("\"a\\u0000b\""));
        QCOMPARE(genko::core::dump_python(Json("🙂"), true), std::string("\"\\ud83d\\ude42\""));
        QCOMPARE(genko::core::dump_python(Json("\x7f"), true), std::string("\"\\u007f\""));
        QCOMPARE(genko::core::dump_python(Json("\u2028")), std::string("\"\u2028\""));
    }

    void layouts() {
        Json value = Json::object();
        value["b"] = Json::array({1, 2.0, "x"});
        value["a"] = Json::object();
        value["c"] = Json::array();
        QCOMPARE(genko::core::dump_python_indent2(value),
                 std::string("{\n  \"b\": [\n    1,\n    2.0,\n    \"x\"\n  ],\n  \"a\": {},\n  \"c\": []\n}"));
        QCOMPARE(genko::core::dump_python(value), std::string("{\"b\": [1, 2.0, \"x\"], \"a\": {}, \"c\": []}"));
        QCOMPARE(genko::core::dump_canonical(value), std::string("{\"a\":{},\"b\":[1,2.0,\"x\"],\"c\":[]}"));
    }

    void canonicalSortsKeysByCodePoint() {
        Json value = Json::object();
        value["日本"] = 1;
        value["Z"] = 2;
        value["a"] = 3;
        value["é"] = 4;
        value[""] = 5;
        QCOMPARE(genko::core::dump_canonical(value), std::string("{\"\":5,\"Z\":2,\"a\":3,\"é\":4,\"日本\":1}"));
    }

    void refusesWhatPythonCannotWrite() {
        try {
            genko::core::dump_python(Json(std::nan("")));
            QFAIL("NaN was written");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("value"));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::dump_canonical(Json::array({HUGE_VAL})));
        try {
            genko::core::dump_python(Json(std::string("\xff")));
            QFAIL("invalid UTF-8 was written");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("json"));
        }
    }

    void readsNanAndInfinityAsNullWithTheirPlaces() {
        genko::core::ParseRepairs repairs;
        const Json value = genko::core::parse_python_json(
            R"({"a": {"b": [1, NaN]}, "a/b": {"~": Infinity}, "c": [-Infinity, 1e400, -1e999]})", &repairs);
        QVERIFY(value["a"]["b"][1].is_null());
        QVERIFY(value["c"][0].is_null() && value["c"][1].is_null() && value["c"][2].is_null());
        const std::vector<std::string> expected{"/a/b/1", "/a~1b/~0", "/c/0", "/c/1", "/c/2"};
        QCOMPARE(repairs.nonfinite, expected);
        QVERIFY(repairs.inexact.empty());
    }

    void readsIntegersAsPythonDoes() {
        genko::core::ParseRepairs repairs;
        const Json value = genko::core::parse_python_json(
            "[-0, 9223372036854775807, -9223372036854775808, 18446744073709551615, 123456789012345678901234567890, "
            "-9223372036854775809]",
            &repairs);
        QVERIFY(value[0].is_number_integer() && !value[0].is_number_unsigned());
        QCOMPARE(value[0].get<std::int64_t>(), std::int64_t{0});
        QCOMPARE(value[1].get<std::int64_t>(), std::int64_t{9223372036854775807});
        QCOMPARE(value[2].get<std::int64_t>(), std::int64_t{-9223372036854775807 - 1});
        QVERIFY(value[3].is_number_unsigned());
        QCOMPARE(value[3].get<std::uint64_t>(), std::uint64_t{18446744073709551615ULL});
        // beyond 64 bits: the nearest double, reported (Python keeps the exact int)
        QVERIFY(value[4].is_number_float());
        QCOMPARE(value[4].get<double>(), 1.2345678901234568e+29);
        QVERIFY(value[5].is_number_float());
        const std::vector<std::string> inexact{"/4", "/5"};
        QCOMPARE(repairs.inexact, inexact);
    }

    void repeatedKeysKeepTheFirstPlaceAndTheLastValue() {
        Json value = genko::core::parse_python_json(R"({"a": 1, "b": 2, "a": 3})");
        QCOMPARE(genko::core::dump_python(value), std::string("{\"a\": 3, \"b\": 2}"));
        std::string text = "{";
        for (int i = 0; i < 40; ++i) text += "\"k" + std::to_string(i) + "\": " + std::to_string(i) + ", ";
        text += "\"k3\": \"x\", \"k39\": \"y\", \"new\": 1}";
        value = genko::core::parse_python_json(text);
        QCOMPARE(value.size(), std::size_t{41});
        QCOMPARE(value["k3"].get<std::string>(), std::string("x"));
        QCOMPARE(value["k39"].get<std::string>(), std::string("y"));
        std::vector<std::string> keys;
        for (const auto& [key, item] : value.items()) keys.push_back(key);
        QCOMPARE(keys[0], std::string("k0"));
        QCOMPARE(keys[3], std::string("k3"));
        QCOMPARE(keys[40], std::string("new"));
    }

    void errorMessagesAsPython() {
        QCOMPARE(error_of(""), std::string("Expecting value: line 1 column 1 (char 0)"));
        QCOMPARE(error_of("[1,]"), std::string("Expecting value: line 1 column 4 (char 3)"));
        QCOMPARE(error_of("{\"a\" 1}"), std::string("Expecting ':' delimiter: line 1 column 6 (char 5)"));
        QCOMPARE(error_of("{\"a\":1,}"), std::string("Expecting property name enclosed in double quotes: line 1 column 8 (char 7)"));
        QCOMPARE(error_of("[1 2]"), std::string("Expecting ',' delimiter: line 1 column 4 (char 3)"));
        QCOMPARE(error_of("\"東京\" 1"), std::string("Extra data: line 1 column 6 (char 5)"));
        QCOMPARE(error_of("\"abc"), std::string("Unterminated string starting at: line 1 column 1 (char 0)"));
        QCOMPARE(error_of("\"a\\x\""), std::string("Invalid \\escape: line 1 column 3 (char 2)"));
        QCOMPARE(error_of("\"a\nb\""), std::string("Invalid control character at: line 1 column 3 (char 2)"));
        QCOMPARE(error_of("[1,\n2,\n]"), std::string("Expecting value: line 3 column 1 (char 7)"));
        QCOMPARE(error_of("[1,\r\n2,\r\n]"), std::string("Expecting value: line 3 column 1 (char 9)"));
        genko::core::ParseOptions read_text;
        read_text.universal_newlines = true;  // (project.json comes through Path.read_text)
        QCOMPARE(error_of("[1,\r\n2,\r\n]", read_text), std::string("Expecting value: line 3 column 1 (char 7)"));
        QCOMPARE(error_of("\xff"), std::string("'utf-8' codec can't decode byte 0xff in position 0: invalid start byte"));
        QCOMPARE(error_of("[\"\xe3\x81\"]"), std::string("'utf-8' codec can't decode bytes in position 2-3: invalid continuation byte"));
        QCOMPARE(error_of("\xe3\x81"), std::string("'utf-8' codec can't decode bytes in position 0-1: unexpected end of data"));
        QCOMPARE(error_of("\xed\xa0\x80"), std::string("'utf-8' codec can't decode byte 0xed in position 0: invalid continuation byte"));
    }

    void byteOrderMark() {
        QCOMPARE(error_of("\xEF\xBB\xBF[]"), std::string("Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)"));
        genko::core::ParseOptions bytes;
        bytes.bytes = true;  // json.loads(bytes) decodes with utf-8-sig
        QCOMPARE(genko::core::parse_python_json("\xEF\xBB\xBF[1]", nullptr, bytes).size(), std::size_t{1});
    }

    void nestingIsLimited() {
        const std::string ok = std::string(999, '[') + std::string(999, ']');
        QCOMPARE(genko::core::parse_python_json(ok).size(), std::size_t{1});
        const std::string deep = std::string(1001, '[') + std::string(1001, ']');
        QVERIFY(error_of(deep).starts_with("JSON nested too deeply"));
    }

    void surrogates() {
        // (escaped backslashes, not raw strings: MSVC reads \uD800-style names in raw strings as characters and
        // rejects the surrogates)
        QCOMPARE(genko::core::parse_python_json("\"\\ud83d\\ude00\"").get<std::string>(), std::string("😀"));
        // Python keeps a lone surrogate in a str but cannot write it out again; UTF-8 has no way to hold one.
        QVERIFY(error_of("\"\\ud800\"").starts_with("Lone surrogate \\ud800 cannot be read"));
        QVERIFY(error_of("\"\\ud800\\u0041\"").starts_with("Lone surrogate"));
        QVERIFY(error_of("\"\\udc00\"").starts_with("Lone surrogate"));
    }

    void subnormalsAndExtremes() {
        const Json value = genko::core::parse_python_json("[5e-324, 1e-320, 2e-324, 3e-324, 1e-400, -1e-400, 2.2250738585072014e-308, 1.7976931348623157e308]");
        QCOMPARE(value[0].get<double>(), 5e-324);
        QCOMPARE(value[1].get<double>(), 1e-320);
        QCOMPARE(value[2].get<double>(), 0.0);
        QCOMPARE(value[3].get<double>(), 5e-324);
        QCOMPARE(value[4].get<double>(), 0.0);
        QVERIFY(std::signbit(value[5].get<double>()));
        QCOMPARE(value[6].get<double>(), 2.2250738585072014e-308);
        QCOMPARE(value[7].get<double>(), 1.7976931348623157e308);
    }

    void strictComparisonOfTheContractTests() {
        using genko::core::parse_python_json;
        using genko::test::strict_equal;
        QVERIFY(strict_equal(parse_python_json(R"({"a": [1, 2.0, null]})"), parse_python_json(R"({"a": [1, 2.0, null]})")));
        QVERIFY(!strict_equal(Json(1), Json(1.0)));
        QVERIFY(!strict_equal(Json(true), Json(1)));
        QVERIFY(!strict_equal(Json(0.0), Json(-0.0)));
        QVERIFY(!strict_equal(Json(0.1 + 0.2), Json(0.3)));
        QVERIFY(!strict_equal(parse_python_json(R"({"a": 1, "b": 2})"), parse_python_json(R"({"b": 2, "a": 1})")));
        QVERIFY(strict_equal(Json(std::uint64_t{5}), Json(std::int64_t{5})));
        std::string where;
        QVERIFY(!strict_equal(parse_python_json("[1, [2, 3]]"), parse_python_json("[1, [2, 4]]"), &where));
        QVERIFY(where.starts_with("/1/1"));
    }

    void pointers() {
        QCOMPARE(genko::core::json_pointer_append("", "a/b~c"), std::string("/a~1b~0c"));
        QCOMPARE(genko::core::json_pointer_append("/pages", std::size_t{3}), std::string("/pages/3"));
    }
};

QTEST_GUILESS_MAIN(TestJson)
#include "test_json.moc"
