#pragma once

// Secret Labs' Regular Expression Engine: Copyright (c) 1997-2001 by Secret Labs AB. All rights reserved. This version
// of the SRE library can be redistributed under CNRI's Python 1.6 license (native/third_party/cpython/LICENSE-SRE).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Python's re for what the ops ask of it (bookops.replace_text, 一括置換): Python 3.12's re for str patterns, ported
// from CPython v3.12.3 (core/pyre_*.cpp; native/third_party/cpython): a pattern found as it is (re.compile(re.escape(find),
// flags)), or a pattern in re's own syntax (re.compile(find, flags), "regex": true) read, compiled and matched as
// CPython's _sre matches it — its groups, backreferences, look-arounds, atomic groups and possessive repeats, its inline
// flags (?aimsx) and Unicode 15.0's categories and cases — with pattern.subn(repl, text) and Python's replacement
// templates (\g<0>, \g<name>, \1, \n, octal escapes, …) and their errors.
//
// Beyond Python (SPEC COMP-01a): a search is bounded in steps (Python's can run for hours), and groups inside one
// another at most 200 deep (Python stops with RecursionError at about 490).

namespace genko::core {

// The steps a replacement or a search of a book may take (each opcode the matcher runs and each character a repeat or a
// backreference walks over): one budget for one op, shared by every line it searches. About a second here; a pattern
// that needs more is refused instead of holding the window.
inline constexpr std::uint64_t kRegexSteps = 100'000'000;

struct PyReBudget {
    std::uint64_t steps = kRegexSteps;
};

// A match where it was (code points of the text) and what replaced it.
struct PyReMatch {
    std::size_t start = 0;
    std::size_t end = 0;
    std::u32string with;
};

class PyLiteralPattern {
public:
    // re.compile(re.escape(find), re.IGNORECASE if ignore_case else 0)
    PyLiteralPattern(std::string_view find, bool ignore_case);

    // pattern.subn(repl, text): the text with every match (left to right, none overlapping) replaced by the template,
    // and how many matches there were. As subn does, the template is read before the text is searched, so a template
    // Python cannot read fails whatever the text: re.error (PyUncaught "re.error": "invalid group reference 1 at
    // position 1", "bad escape \q at position 0", …) or IndexError ("unknown group name 'x'").
    std::pair<std::string, std::int64_t> subn(std::string_view repl, std::string_view text) const;
    // The same, with each match where it was and what replaced it, in order.
    using Match = PyReMatch;
    std::pair<std::string, std::int64_t> subn(std::string_view repl, std::string_view text, std::vector<Match>* matches) const;

private:
    std::u32string find_;
    bool ignore_case_;
};

// re.compile(pattern, flags) for a str (flags: Python's re.IGNORECASE = 2, re.MULTILINE = 8, re.DOTALL = 16,
// re.VERBOSE = 64, re.ASCII = 256; the pattern's own (?…) flags as Python reads them). Throws what re.compile raises:
// PyUncaught "re.error" with re.error's message for a pattern Python cannot read ("missing ), unterminated subpattern at
// position 0", "look-behind requires fixed-width pattern", …), PyUncaught "OverflowError" ("the repetition number is
// too large"), PyValueError (Python's ValueError: "ASCII and UNICODE flags are incompatible", an int of more than 4300
// digits); core::Error "regex_nested" for groups more than 200 deep.
class PyPattern {
public:
    static constexpr int kIgnoreCase = 2;
    static constexpr int kMultiline = 8;
    static constexpr int kDotAll = 16;
    static constexpr int kVerbose = 64;
    static constexpr int kAscii = 256;

    PyPattern(std::string_view pattern, int flags);
    ~PyPattern();
    PyPattern(PyPattern&&) noexcept;
    PyPattern& operator=(PyPattern&&) noexcept;

    // pattern.groups
    std::int64_t groups() const;

    // pattern.subn(repl, text), as PyLiteralPattern::subn (each match where it was and what replaced it in `matches`).
    // Its template's errors as Python's (re.error, IndexError for an unknown name, and SystemError "The span of
    // capturing group is wrong…" where _sre's groups come out reversed); core::Error "regex_too_slow" when the search
    // spends the budget.
    std::pair<std::string, std::int64_t> subn(std::string_view repl, std::string_view text, std::vector<PyReMatch>* matches,
                                              PyReBudget& budget) const;
    // len(pattern.findall(text))
    std::int64_t count(std::string_view text, PyReBudget& budget) const;
    // [m.span(g) for g in range(groups + 1)] for each m of pattern.finditer(text) ((-1, -1): a group that did not take
    // part); SystemError as Python's match objects raise it.
    std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>> spans(std::string_view text, PyReBudget& budget) const;

private:
    struct Compiled;
    std::unique_ptr<Compiled> compiled_;
};

}  // namespace genko::core
