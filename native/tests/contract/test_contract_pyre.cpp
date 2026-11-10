// core/pyre — Python 3.12's re for str patterns, ported from CPython (replace_text's "regex", the find dialog) — against
// the reference Python (tools/migration/pyre_harness.py):
//   1. unicodeLikePython: every code point's properties that re reads (its lower and upper case, cased, \d, \s, \w, line
//      breaks, letters, identifiers' characters, printable) and _casefix's extra cases as Python 3.12 has them, and the
//      names \N{…} takes (unicodedata.lookup: every name, in lower case, changed a little; Hangul and CJK names made
//      from the code point; aliases), with the names table the build embeds checked by Python;
//   2. curatedLikePython: contract/pyre_cases.json — each construct of re's syntax and each error of its parser,
//      compiler and template parser — over its texts and templates;
//   3. corpusLikePython: 30000 random patterns (seeded: valid ones, and some broken on purpose) × Japanese and ASCII
//      texts with the characters re's cases and categories turn on.
// For each case: re.compile's error (its type and words), pattern.groups, every match's group spans (finditer), subn's
// text and count or its error, findall's count or its error. Skipped without the Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <map>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pyre.hpp"
#include "core/pyre_internal.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

namespace pyre = genko::core::pyre;

// [type, message] of what a call raised, as Python names it
Json raised(const std::exception& error) {
    if (const auto* uncaught = dynamic_cast<const genko::core::PyUncaught*>(&error)) {
        return Json::array({uncaught->type() == "re.error" ? std::string("error") : uncaught->type(), std::string(error.what())});
    }
    if (dynamic_cast<const genko::core::PyValueError*>(&error) != nullptr) return Json::array({"ValueError", std::string(error.what())});
    if (const auto* own = dynamic_cast<const genko::core::Error*>(&error)) return Json::array({"C++ " + own->code(), std::string(error.what())});
    return Json::array({"C++", std::string(error.what())});
}

// The harness's run_case, here.
Json run_case(const Json& record) {
    const std::string pattern = record["pattern"].get<std::string>();
    const int flags = record["flags"].get<int>();
    const std::string repl = record["repl"].get<std::string>();
    const std::string text = record["text"].get<std::string>();
    Json out = Json::object();
    out["pattern"] = pattern;
    out["flags"] = flags;
    out["repl"] = repl;
    out["text"] = text;
    std::optional<genko::core::PyPattern> compiled;
    try {
        compiled.emplace(pattern, flags);
    } catch (const std::exception& error) {
        out["compile"] = raised(error);
        return out;
    }
    out["compile"] = nullptr;
    out["groups"] = compiled->groups();
    try {
        genko::core::PyReBudget budget;
        Json spans = Json::array();
        for (const auto& match : compiled->spans(text, budget)) {
            Json groups = Json::array();
            for (const auto& [s, e] : match) groups.push_back(Json::array({s, e}));
            spans.push_back(std::move(groups));
        }
        out["spans"] = std::move(spans);
    } catch (const std::exception& error) {
        out["spans"] = raised(error);
    }
    try {
        genko::core::PyReBudget budget;
        const auto [replaced, n] = compiled->subn(repl, text, nullptr, budget);
        out["subn"] = Json::array({replaced, n});
    } catch (const std::exception& error) {
        out["subn"] = raised(error);
    }
    try {
        genko::core::PyReBudget budget;
        out["findall"] = compiled->count(text, budget);
    } catch (const std::exception& error) {
        out["findall"] = raised(error);
    }
    return out;
}

// Every record the same here (failures listed), and how they came out in Python.
void compare(const Json& records, int least_matched) {
    std::vector<std::string> failures;
    std::map<std::string, int> outcomes;
    for (const Json& record : records) {
        const Json got = run_case(record);
        std::string where;
        if (!genko::test::strict_equal(got, record, &where)) {
            failures.push_back(genko::core::dump_python(record).substr(0, 600) + "\n    C++: " + genko::core::dump_python(got).substr(0, 600) +
                               " (" + where + ")");
            continue;
        }
        if (!record["compile"].is_null()) {
            outcomes["compile " + record["compile"][0].get<std::string>()] += 1;
        } else if (record["subn"][1].is_number() && record["subn"][1].get<std::int64_t>() > 0) {
            outcomes["matched"] += 1;
        } else if (record["subn"][1].is_number()) {
            outcomes["no match"] += 1;
        } else {
            outcomes["subn " + record["subn"][0].get<std::string>()] += 1;
        }
    }
    for (std::size_t i = 0; i < failures.size() && i < 60; ++i) qWarning("%s", failures[i].c_str());
    for (const auto& [what, count] : outcomes) qInfo("%6d  %s", count, what.c_str());
    QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(records.size()) + " cases differ").c_str());
    QVERIFY(outcomes["matched"] >= least_matched);
}

genko::test::Run harness(const QStringList& args, const QString& scratch) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/pyre_harness.py")};
    full += args;
    QProcessEnvironment env = genko::test::python_env(scratch);
    env.insert(QStringLiteral("PYTHONWARNINGS"), QStringLiteral("ignore"));
    return genko::test::run(genko::test::python_ref(), full, env, 1800000);
}

}  // namespace

class TestContractPyre : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void unicodeLikePython() {
        const auto ran = harness({QStringLiteral("tables"), path("tables.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json tables = genko::test::read_json(path("tables.json"));
        // each property's ranges as Python gives them, against this build's answer for every code point
        const std::vector<std::pair<std::string, bool (*)(char32_t)>> properties{
            {"cased", pyre::cased},   {"decimal", pyre::decimal},     {"space", pyre::space},
            {"alnum", pyre::alnum},   {"linebreak", pyre::linebreak}, {"alpha", pyre::alpha},
            {"printable", pyre::printable}, {"xid_start", pyre::xid_start}, {"xid_continue", pyre::xid_continue}};
        for (const auto& [name, test] : properties) {
            std::vector<bool> python(0x110000, false);
            for (const Json& range : tables[name]) {
                for (auto c = range[0].get<std::uint32_t>(); c <= range[1].get<std::uint32_t>(); ++c) python[c] = true;
            }
            int differ = 0;
            for (char32_t c = 0; c < 0x110000; ++c) {
                if (test(c) != python[c] && differ++ < 5) qWarning("%s U+%04X: Python %d", name.c_str(), static_cast<unsigned>(c), python[c] ? 1 : 0);
            }
            QVERIFY2(differ == 0, (name + ": " + std::to_string(differ) + " code points differ").c_str());
        }
        // the case mappings: runs (first, last, step, delta)
        for (const auto& [name, map] : std::vector<std::pair<std::string, char32_t (*)(char32_t)>>{{"lower", pyre::lower}, {"upper", pyre::upper}}) {
            std::vector<char32_t> python(0x110000);
            for (char32_t c = 0; c < 0x110000; ++c) python[c] = c;
            for (const Json& run : tables[name]) {
                const auto first = run[0].get<std::uint32_t>();
                const auto last = run[1].get<std::uint32_t>();
                const auto step = run[2].get<std::uint32_t>();
                const auto delta = run[3].get<std::int64_t>();
                for (std::uint32_t c = first; c <= last; c += step) python[c] = static_cast<char32_t>(c + delta);
            }
            int differ = 0;
            for (char32_t c = 0; c < 0x110000; ++c) {
                if (map(c) != python[c] && differ++ < 5) qWarning("%s U+%04X", name.c_str(), static_cast<unsigned>(c));
            }
            QVERIFY2(differ == 0, (name + ": " + std::to_string(differ) + " code points differ").c_str());
        }
        // _casefix._EXTRA_CASES
        int extra = 0;
        for (const Json& item : tables["extra_cases"]) {
            const auto lower = item[0].get<char32_t>();
            const auto* mine = pyre::extra_cases(lower);
            QVERIFY2(mine != nullptr, std::to_string(lower).c_str());
            std::vector<char32_t> others;
            for (const char32_t c : *mine) {
                if (c != 0) others.push_back(c);
            }
            QCOMPARE(others, item[1].get<std::vector<char32_t>>());
            ++extra;
        }
        int none = 0;
        for (char32_t c = 0; c < 0x110000; ++c) none += pyre::extra_cases(c) != nullptr ? 1 : 0;
        QCOMPARE(none, extra);
        // the names
        int names = 0;
        int differ = 0;
        for (const Json& item : tables["names"]) {
            const std::string name = item[0].get<std::string>();
            const std::optional<char32_t> mine = pyre::lookup_name(name);
            const std::optional<char32_t> python = item[1].is_null() ? std::nullopt : std::optional<char32_t>(item[1].get<char32_t>());
            if (mine != python && differ++ < 10) qWarning("\\N{%s}: Python %d, here %d", name.c_str(), python ? static_cast<int>(*python) : -1, mine ? static_cast<int>(*mine) : -1);
            ++names;
        }
        qInfo("%d names asked (the build's table: %d names and aliases, each checked by Python)", names, tables["listed"].get<int>());
        QVERIFY2(differ == 0, (std::to_string(differ) + " names differ").c_str());
        QVERIFY(names > 150000);
    }

    void curatedLikePython() {
        const auto ran = harness({QStringLiteral("cases"), genko::test::repo_root() + QStringLiteral("/native/tests/contract/pyre_cases.json"),
                                  path("curated.json")},
                                 path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(path("curated.json"));
        QVERIFY(records.size() > 13000);
        compare(records, 5000);
    }

    // (GENKO_PYRE_SEED and GENKO_PYRE_COUNT ask for another corpus, to look further than the fixed one)
    void corpusLikePython() {
        const QString seed = qEnvironmentVariable("GENKO_PYRE_SEED", QStringLiteral("20261011"));
        const QString count = qEnvironmentVariable("GENKO_PYRE_COUNT", QStringLiteral("30000"));
        const auto ran = harness({QStringLiteral("corpus"), path("corpus.json"), QStringLiteral("--seed"), seed, QStringLiteral("--count"), count},
                                 path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json corpus = genko::test::read_json(path("corpus.json"));
        qInfo("seed %s: %d cases Python takes more than a second for are left out", qPrintable(seed), corpus["too_slow"].get<int>());
        QCOMPARE(corpus["records"].size(), static_cast<std::size_t>(count.toLongLong()));
        compare(corpus["records"], count.toInt() / 3);
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "1700000");
    QCoreApplication app(argc, argv);
    TestContractPyre test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_pyre.moc"
