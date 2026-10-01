// core::PyRandom against Python 3.12's random.Random: every call of the table (tools/migration/render_harness.py
// unit-tables) gives the same number, floats bit for bit, for int, str and bytes seeds.

#include <QtTest>

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/pyrandom.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::core::PyRandom;

namespace {

double from_bits(const Json& hex) { return std::bit_cast<double>(std::stoull(hex.get<std::string>(), nullptr, 16)); }

std::string bits_of(double x) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(x)));
    return buf;
}

const Json& tables() {
    static const Json data = genko::test::read_json(genko::test::test_data("pyref/render_unit_tables.json"));
    return data;
}

}  // namespace

class TestPyRandom : public QObject {
    Q_OBJECT

private slots:
    void sequences_data() {
        QTest::addColumn<int>("index");
        const Json& cases = tables()["random"];
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const Json& seed = cases[i]["seed"];
            const std::string label = seed[0].get<std::string>() + ":" + genko::core::dump_python(seed[1]).substr(0, 24);
            QTest::newRow(label.c_str()) << static_cast<int>(i);
        }
    }

    void sequences() {
        QFETCH(int, index);
        const Json& entry = tables()["random"][static_cast<std::size_t>(index)];
        const Json& seed = entry["seed"];
        PyRandom rng;
        const std::string kind = seed[0].get<std::string>();
        if (kind == "int") {
            rng = PyRandom(seed[1].get<std::int64_t>());
        } else if (kind == "str") {
            rng = PyRandom::from_str(seed[1].get<std::string>());
        } else {
            rng = PyRandom::from_bytes(genko::core::a2b_base64(seed[1].get<std::string>()));
        }
        int n = 0;
        for (const Json& call : entry["calls"]) {
            const std::string what = call[0].get<std::string>();
            const std::string where = what + " #" + std::to_string(n++);
            if (what == "random") {
                QCOMPARE(bits_of(rng.random()), call[1].get<std::string>());
            } else if (what == "uniform") {
                QCOMPARE(bits_of(rng.uniform(from_bits(call[1]), from_bits(call[2]))), call[3].get<std::string>());
            } else if (what == "gauss") {
                QCOMPARE(bits_of(rng.gauss(from_bits(call[1]), from_bits(call[2]))), call[3].get<std::string>());
            } else if (what == "randint") {
                QCOMPARE(rng.randint(call[1].get<std::int64_t>(), call[2].get<std::int64_t>()), call[3].get<std::int64_t>());
            } else if (what == "randrange3") {
                QCOMPARE(rng.randrange(call[1].get<std::int64_t>(), call[2].get<std::int64_t>(), call[3].get<std::int64_t>()),
                         call[4].get<std::int64_t>());
            } else if (what == "randrange") {
                QCOMPARE(rng.randrange(call[1].get<std::int64_t>()), call[2].get<std::int64_t>());
            } else if (what == "getrandbits") {
                QCOMPARE(std::to_string(rng.getrandbits(call[1].get<int>())), call[2].get<std::string>());
            } else if (what == "choice") {
                const std::vector<int> items{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
                QCOMPARE(rng.choice(items), call[1].get<int>());
            } else if (what == "shuffle") {
                std::vector<int> items{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
                rng.shuffle(items);
                QCOMPARE(items, call[1].get<std::vector<int>>());
            } else {
                QFAIL(("unknown call " + where).c_str());
            }
        }
    }

    void errors() {
        PyRandom rng(1);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, rng.randrange(0));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, rng.randrange(5, 5));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, rng.randrange(0, 10, 0));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, rng.choice(std::vector<int>{}));
        try {
            rng.randrange(3, 1);
            QFAIL("no error");
        } catch (const genko::core::Error& e) {
            QCOMPARE(std::string(e.what()), std::string("empty range in randrange(3, 1)"));
        }
    }
};

QTEST_GUILESS_MAIN(TestPyRandom)
#include "test_pyrandom.moc"
