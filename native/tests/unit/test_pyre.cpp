// core/pyre's own bounds (SPEC COMP-01a), without Python: Python 3.12's re searches on for hours (or until its memory
// runs out) where a pattern backtracks without end, and stops with RecursionError at about 490 groups inside one
// another; this build refuses such a search when it has spent kRegexSteps steps (sre's steps and the work between
// them) or holds a million places to go back to or 4 million entries of data kept to go back — for the whole book in
// replace_text, which leaves the book as it was and says so — and refuses groups more than 200 deep. And a few of
// Python's results (test_contract_pyre has them all, against Python). How long a refusal may take is measured against
// this build here (regex_timing.hpp: the steps given at the rate measured, times 3), so a sanitizer build at -O0 is
// held to the same proportion as a release one; an optimized build is also held to 10 s.

#include <QtTest>

#include <QElapsedTimer>

#include <string>
#include <vector>

#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyre.hpp"
#include "regex_timing.hpp"
#include "render/ops_registry.hpp"

using genko::core::Json;
using genko::core::PyPattern;
using genko::core::PyReBudget;

namespace {

std::string code_of(const std::function<void()>& work) {
    try {
        work();
    } catch (const genko::core::Error& error) {
        return error.code() + ": " + error.what();
    }
    return "(done)";
}

}  // namespace

class TestPyre : public QObject {
    Q_OBJECT

private slots:
    // A few of Python's own answers (re.subn in Python 3.12), as a first check.
    void likePython() {
        PyReBudget budget;
        QCOMPARE(PyPattern("(\\w)(\\w)", 0).subn("\\2\\1", "abcd", nullptr, budget), (std::pair<std::string, std::int64_t>{"badc", 2}));
        QCOMPARE(PyPattern("x*", 0).subn("-", "abxd", nullptr, budget), (std::pair<std::string, std::int64_t>{"-a-b--d-", 5}));
        QCOMPARE(PyPattern("(?i)σ", 0).subn("s", "Σσς", nullptr, budget), (std::pair<std::string, std::int64_t>{"sss", 3}));
        QCOMPARE(PyPattern("[𐐀a]", PyPattern::kIgnoreCase).subn("x", "𐐀𐐨", nullptr, budget), (std::pair<std::string, std::int64_t>{"𐐀𐐨", 0}));
        QCOMPARE(PyPattern("(?:(a)|b)*", 0).spans("ab", budget).front(), (std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 2}, {0, 1}}));
        QCOMPARE(PyPattern("\\N{HIRAGANA LETTER A}+", 0).subn("ア", "あああい", nullptr, budget), (std::pair<std::string, std::int64_t>{"アい", 1}));
        QCOMPARE(code_of([] { PyPattern("(?<=a+)b", 0); }), std::string("python_error: look-behind requires fixed-width pattern"));
        QCOMPARE(code_of([] { PyPattern("a\n(", 0); }), std::string("python_error: missing ), unterminated subpattern at position 2 (line 2, column 1)"));
    }

    // A pattern that backtracks without end (Python's runs for hours) stops when its steps are spent: in proportion to
    // them (regex_timing.hpp), and in an optimized build within 10 s (0.7 s in a release build).
    void searchesAreBounded() {
        const std::int64_t calibration = genko::test::regex_step_cost();
        const std::int64_t bound = genko::test::regex_steps_bound_ms(calibration, genko::core::kRegexSteps);
        qInfo("%llu steps measured in %lld us: %lld ms for a refusal at most", static_cast<unsigned long long>(genko::test::kCalibrationSteps),
              static_cast<long long>(calibration / 1000), static_cast<long long>(bound));
        const std::string text = std::string(60, 'a') + "!";
        QElapsedTimer clock;
        clock.start();
        PyReBudget budget;
        QCOMPARE(code_of([&] { PyPattern("(a|aa)+$", 0).subn("x", text, nullptr, budget); }).substr(0, 54),
                 std::string("regex_too_slow: the pattern takes too long to search ("));
        QCOMPARE(budget.steps, std::uint64_t{0});
        qInfo("stopped after %lld ms", static_cast<long long>(clock.elapsed()));
        QVERIFY2(clock.elapsed() < bound, qPrintable(QString::number(clock.elapsed())));
        if constexpr (genko::test::kOptimizedBuild) QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
        // the budget is the whole search's: what one line spent the next has no more
        PyReBudget shared{1000};
        const PyPattern any(".*a", 0);
        QCOMPARE(code_of([&] {
                     for (int i = 0; i < 1000; ++i) any.subn("x", std::string(40, 'b'), nullptr, shared);
                 }).substr(0, 14),
                 std::string("regex_too_slow"));
        // a group repeated without end at least, whose every time Python would hold: stopped long before the memory goes
        clock.restart();
        PyReBudget deep;
        QCOMPARE(code_of([&] { PyPattern("(?:a|){4294967294}", 0).subn("x", "aaa", nullptr, deep); }).substr(0, 14), std::string("regex_too_slow"));
        qInfo("a group repeated without end: stopped after %lld ms", static_cast<long long>(clock.elapsed()));
        QVERIFY2(clock.elapsed() < bound, qPrintable(QString::number(clock.elapsed())));
        if constexpr (genko::test::kOptimizedBuild) QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
        // the find dialog's count is bounded the same way
        PyReBudget counted;
        QCOMPARE(code_of([&] { PyPattern("(a+)+b", 0).count(text, counted); }).substr(0, 14), std::string("regex_too_slow"));
    }

    // Work sre does between two of its steps is counted too: the marks saved and given back at each place to go back to
    // (a pattern of many groups copies them all), the marks set to nothing below a new one, each choice of a branch
    // looked at, each item of a set looked through. So a pattern of 1000 or 3000 groups, of a set of 5000 characters
    // outside the BMP (no bitmap: a list) or of 5000 choices, backtracking without end, stops when its steps are spent,
    // within the time its budget is worth at the rate measured for steps without such work (regex_timing.hpp; counted as
    // sre's steps alone a release build took 25 s, 100 s and minutes, past that bound's 2 s), and in an optimized build
    // within 10 s (under half a second).
    void costlyStepsAreCounted() {
        const std::int64_t calibration = genko::test::regex_step_cost();
        const std::int64_t bound = genko::test::regex_steps_bound_ms(calibration, genko::core::kRegexSteps);
        qInfo("%llu steps measured in %lld us: %lld ms for a refusal at most", static_cast<unsigned long long>(genko::test::kCalibrationSteps),
              static_cast<long long>(calibration / 1000), static_cast<long long>(bound));
        const std::string text = std::string(60, 'a');
        std::string groups1000;
        for (int i = 0; i < 1000; ++i) groups1000 += "()";
        std::string groups3000;
        for (int i = 0; i < 3000; ++i) groups3000 += "()";
        std::string wide;  // (U+10000 and on, each written as itself)
        for (char32_t c = 0x10000; c < 0x10000 + 5000; ++c) {
            wide += static_cast<char>(0xF0 | (c >> 18));
            wide += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            wide += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            wide += static_cast<char>(0x80 | (c & 0x3F));
        }
        std::string choices;
        for (int i = 0; i < 5000; ++i) choices += "b" + std::to_string(i) + "|";
        const std::vector<std::pair<const char*, std::string>> patterns{
            {"1000 groups", groups1000 + "(?:aa|a)*c"},
            {"3000 groups", groups3000 + "(?:aa|a)*c"},
            {"a set of 5000", "(?:[" + wide + "a]|aa)*c"},
            {"5000 choices", "(?:" + choices + "aa|a)*c"},
            {"a mark far up", "(?:x" + groups1000 + "|(a)|aa)*c"},  // (the 1000 groups' marks set to nothing each time)
        };
        std::string slow;
        for (const auto& [what, pattern] : patterns) {
            QElapsedTimer clock;
            clock.start();
            PyReBudget budget;
            QCOMPARE(code_of([&] { PyPattern(pattern, 0).subn("x", text, nullptr, budget); }).substr(0, 14), std::string("regex_too_slow"));
            qInfo("%s: stopped after %lld ms", what, static_cast<long long>(clock.elapsed()));
            if (clock.elapsed() >= bound || (genko::test::kOptimizedBuild && clock.elapsed() >= 10000)) {
                slow += std::string(what) + ": " + std::to_string(clock.elapsed()) + " ms; ";
            }
        }
        QVERIFY2(slow.empty(), slow.c_str());
        // and they find what Python finds where they are quick
        PyReBudget budget;
        QCOMPARE(PyPattern(groups1000 + "(?:aa|a)*c", 0).subn("x", "aaac", nullptr, budget), (std::pair<std::string, std::int64_t>{"x", 1}));
        QCOMPARE(PyPattern("(?:[" + wide + "a]|aa)*c", 0).subn("x", "aaac", nullptr, budget), (std::pair<std::string, std::int64_t>{"x", 1}));
        QCOMPARE(PyPattern("(?:" + choices + "aa|a)*c", 0).subn("x", "b4999aac", nullptr, budget), (std::pair<std::string, std::int64_t>{"x", 1}));
    }

    // Groups inside one another: 200 deep are read, 201 refused (Python reads about 490, then RecursionError).
    void nestingIsBounded() {
        PyReBudget budget;
        const std::string deep = std::string(200, '(') + "a" + std::string(200, ')');
        QCOMPARE(PyPattern(deep, 0).subn("x", "a", nullptr, budget), (std::pair<std::string, std::int64_t>{"x", 1}));
        QCOMPARE(PyPattern(deep, 0).groups(), std::int64_t{200});
        const std::string deeper = std::string(201, '(') + "a" + std::string(201, ')');
        QCOMPARE(code_of([&] { PyPattern(deeper, 0); }),
                 std::string("regex_nested: the pattern nests too deeply (groups and look-arounds at most 200 levels inside one another)"));
        const std::string looks = "(?=" + std::string(250, '(') + "a" + std::string(250, ')') + ")";
        QCOMPARE(code_of([&] { PyPattern(looks, 0); }).substr(0, 12), std::string("regex_nested"));
    }

    // replace_text: the refusal is the op's, the book as it was (Python's would hold the window for hours).
    void replaceTextRefusesWithoutChanges() {
        const std::int64_t bound = genko::test::regex_steps_bound_ms(genko::test::regex_step_cost(), genko::core::kRegexSteps);
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor person("human:作者");
        genko::core::Document doc = genko::core::new_episode("p", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono());
        doc = bus.apply(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", std::string(54, 'a') + "!"}},
                                          Json{{"op", "add_line"}, {"page", 2}, {"text", "台詞"}}}),
                        person)
                  .doc;
        const auto refused = [&](const Json& op) {
            try {
                (void)bus.apply(doc, Json::array({op}), person);
            } catch (const genko::core::ApplyError& error) {
                return std::string(error.what());
            }
            return std::string("(applied)");
        };
        QElapsedTimer clock;
        clock.start();
        const std::string slow = refused(Json{{"op", "replace_text"}, {"find", "(a|aa)+$"}, {"replace", "x"}, {"regex", true}});
        QVERIFY2(slow.starts_with("ops[0] replace_text: the pattern takes too long to search (more than 100000000 steps"), slow.c_str());
        qInfo("replace_text: refused after %lld ms (%lld ms at most)", static_cast<long long>(clock.elapsed()), static_cast<long long>(bound));
        QVERIFY2(clock.elapsed() < bound, qPrintable(QString::number(clock.elapsed())));
        if constexpr (genko::test::kOptimizedBuild) QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
        const std::string nested = refused(Json{{"op", "replace_text"}, {"find", std::string(201, '(') + std::string(201, ')')}, {"regex", true}});
        QVERIFY2(nested.starts_with("ops[0] replace_text: the pattern nests too deeply"), nested.c_str());
        // refused at a later line after an earlier one was replaced, in a batch after another op: nothing of it is kept
        {
            genko::core::Document book = genko::core::new_episode("q", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono());
            book = bus.apply(book, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}},
                                                Json{{"op", "add_line"}, {"page", 2}, {"text", std::string(54, 'a') + "!"}}}),
                             person)
                       .doc;
            const auto story = book.story;
            const auto pages = book.pages;
            try {
                (void)bus.apply(book, Json::array({Json{{"op", "edit_line"}, {"id", story[0].id}, {"text", "台詞です"}},
                                                   Json{{"op", "replace_text"}, {"find", "台|(a|aa)+$"}, {"replace", "x"}, {"regex", true}}}),
                                person);
                QFAIL("applied");
            } catch (const genko::core::ApplyError& error) {
                QVERIFY2(std::string(error.what()).starts_with("ops[1] replace_text: the pattern takes too long to search"), error.what());
            }
            QCOMPARE(book.story.size(), story.size());
            QCOMPARE(book.story[0].text, std::string("台詞"));
            QCOMPARE(book.story[1].text, std::string(54, 'a') + "!");
            QVERIFY(book.pages == pages);
        }
        // a pattern that reads well replaces on every page, as Python's
        const auto done = bus.apply(doc, Json::parse(R"J([{"op": "replace_text", "find": "(a+)!|台(詞)", "replace": "<\\1\\2>", "regex": true}])J"), person);
        QCOMPARE(done.doc.story[0].text, "<" + std::string(54, 'a') + ">");
        QCOMPARE(done.doc.story[1].text, std::string("<詞>"));
    }
};

QTEST_GUILESS_MAIN(TestPyre)
#include "test_pyre.moc"
