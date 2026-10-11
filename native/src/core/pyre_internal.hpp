#pragma once

// The inside of core/pyre (Python 3.12's re for str patterns, ported from CPython v3.12.3: Lib/re/_parser.py,
// _compiler.py, _casefix.py, _constants.py and Modules/_sre/sre_lib.h, sre.c; PSF License, native/third_party/cpython).
// Each part is the Python (or C) it is named after, decision for decision: the same pattern compiles to the same code,
// the same code matches the same spans with the same groups, and a pattern Python cannot read fails with Python's words.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genko::core::pyre {

// --- Unicode (Python 3.12's database, 15.0.0: pyre_unicode.cpp, made by tools/migration/pyre_tables.py) ---------------

char32_t lower(char32_t c);   // Py_UNICODE_TOLOWER (_sre.unicode_tolower)
char32_t upper(char32_t c);   // Py_UNICODE_TOUPPER
bool cased(char32_t c);       // _sre.unicode_iscased
bool decimal(char32_t c);     // Py_UNICODE_ISDECIMAL
bool space(char32_t c);       // Py_UNICODE_ISSPACE
bool alnum(char32_t c);       // Py_UNICODE_ISALNUM
bool linebreak(char32_t c);   // Py_UNICODE_ISLINEBREAK
bool alpha(char32_t c);       // str.isalpha of one character
bool printable(char32_t c);   // str.isprintable of one character
bool xid_start(char32_t c);   // XID_Start (str.isidentifier's first character, "_" aside)
bool xid_continue(char32_t c);  // XID_Continue

// unicodedata.lookup(name) when it names one character (an alias too; a named sequence is not one character).
std::optional<char32_t> lookup_name(std::string_view name);

// re._casefix._EXTRA_CASES: a lowercase letter → the other lowercase letters with the same uppercase (0: none).
const std::array<char32_t, 2>* extra_cases(char32_t lower);

// str.isidentifier
bool is_identifier(std::u32string_view name);

// --- text --------------------------------------------------------------------------------------------------------

std::u32string decode(std::string_view text);
std::string encode(std::u32string_view text);
// repr(text) as Python writes a str
std::string repr(std::u32string_view text);

// re.error(msg, pattern, pos): "<msg> at position <pos>" (and the line and column when the pattern has more than one
// line): PyUncaught("re.error", …).
[[noreturn]] void re_error(const std::string& msg, const std::u32string& pattern, std::int64_t pos);
// re.error(msg) without a place
[[noreturn]] void re_error(const std::string& msg);

// --- _constants.py ------------------------------------------------------------------------------------------------

using Code = std::uint32_t;  // SRE_CODE (CODESIZE 4)

enum Op : Code {
    FAILURE = 0, SUCCESS = 1, ANY = 2, ANY_ALL = 3, ASSERT = 4, ASSERT_NOT = 5, AT = 6, BRANCH = 7, CATEGORY = 8,
    CHARSET = 9, BIGCHARSET = 10, GROUPREF = 11, GROUPREF_EXISTS = 12, IN = 13, INFO = 14, JUMP = 15, LITERAL = 16,
    MARK = 17, MAX_UNTIL = 18, MIN_UNTIL = 19, NOT_LITERAL = 20, NEGATE = 21, RANGE = 22, REPEAT = 23, REPEAT_ONE = 24,
    SUBPATTERN = 25, MIN_REPEAT_ONE = 26, ATOMIC_GROUP = 27, POSSESSIVE_REPEAT = 28, POSSESSIVE_REPEAT_ONE = 29,
    GROUPREF_IGNORE = 30, IN_IGNORE = 31, LITERAL_IGNORE = 32, NOT_LITERAL_IGNORE = 33, GROUPREF_LOC_IGNORE = 34,
    IN_LOC_IGNORE = 35, LITERAL_LOC_IGNORE = 36, NOT_LITERAL_LOC_IGNORE = 37, GROUPREF_UNI_IGNORE = 38,
    IN_UNI_IGNORE = 39, LITERAL_UNI_IGNORE = 40, NOT_LITERAL_UNI_IGNORE = 41, RANGE_UNI_IGNORE = 42,
    // (the parser's own codes, never compiled)
    MIN_REPEAT = 100, MAX_REPEAT = 101,
};

enum At : Code {
    AT_BEGINNING = 0, AT_BEGINNING_LINE = 1, AT_BEGINNING_STRING = 2, AT_BOUNDARY = 3, AT_NON_BOUNDARY = 4, AT_END = 5,
    AT_END_LINE = 6, AT_END_STRING = 7, AT_LOC_BOUNDARY = 8, AT_LOC_NON_BOUNDARY = 9, AT_UNI_BOUNDARY = 10,
    AT_UNI_NON_BOUNDARY = 11,
};

enum Category : Code {
    CATEGORY_DIGIT = 0, CATEGORY_NOT_DIGIT = 1, CATEGORY_SPACE = 2, CATEGORY_NOT_SPACE = 3, CATEGORY_WORD = 4,
    CATEGORY_NOT_WORD = 5, CATEGORY_LINEBREAK = 6, CATEGORY_NOT_LINEBREAK = 7, CATEGORY_LOC_WORD = 8,
    CATEGORY_LOC_NOT_WORD = 9, CATEGORY_UNI_DIGIT = 10, CATEGORY_UNI_NOT_DIGIT = 11, CATEGORY_UNI_SPACE = 12,
    CATEGORY_UNI_NOT_SPACE = 13, CATEGORY_UNI_WORD = 14, CATEGORY_UNI_NOT_WORD = 15, CATEGORY_UNI_LINEBREAK = 16,
    CATEGORY_UNI_NOT_LINEBREAK = 17,
};

enum Flag : int {
    FLAG_TEMPLATE = 1, FLAG_IGNORECASE = 2, FLAG_LOCALE = 4, FLAG_MULTILINE = 8, FLAG_DOTALL = 16, FLAG_UNICODE = 32,
    FLAG_VERBOSE = 64, FLAG_DEBUG = 128, FLAG_ASCII = 256,
};

inline constexpr Code kMaxRepeat = 0xFFFFFFFFu;          // _sre.MAXREPEAT
inline constexpr std::int64_t kMaxGroups = 1073741823;  // _sre.MAXGROUPS
inline constexpr Code kMaxCode = 0xFFFFFFFFu;            // _compiler.MAXCODE
// _parser.MAXWIDTH (1 << 64), held as the largest uint64: every width past it is it (no width in between matters)
inline constexpr std::uint64_t kMaxWidth = ~std::uint64_t{0};
// This build's own bound (beyond Python, which stops with RecursionError at about 490): groups and look-arounds
// inside one another, at most this deep.
inline constexpr int kMaxNesting = 200;

// --- _parser.py ---------------------------------------------------------------------------------------------------

struct SubPattern;
using Sub = std::shared_ptr<SubPattern>;

// A member of a set (IN): (LITERAL, c), (RANGE, (lo, hi)), (CATEGORY, code), (NEGATE, None); after _optimize_charset
// also (RANGE_UNI_IGNORE, (lo, hi)), (CHARSET, words) and (BIGCHARSET, words).
struct SetItem {
    Code op = LITERAL;
    Code a = 0;
    Code b = 0;
    std::vector<Code> words;
    bool operator==(const SetItem&) const = default;
};

// One (op, av) of a subpattern.
struct Item {
    Code op = LITERAL;
    Code value = 0;              // LITERAL, NOT_LITERAL: the character; AT, CATEGORY: the code; GROUPREF: the group
    std::vector<SetItem> set;    // IN
    Code min = 0;                // MIN_REPEAT, MAX_REPEAT, POSSESSIVE_REPEAT
    Code max = 0;
    std::int64_t group = -1;     // SUBPATTERN (-1: None), GROUPREF_EXISTS (the group asked for)
    int add_flags = 0;           // SUBPATTERN
    int del_flags = 0;
    int dir = 1;                 // ASSERT, ASSERT_NOT: 1 ahead, -1 behind
    Sub sub;                     // the repeated item, SUBPATTERN's p, ATOMIC_GROUP, ASSERT's p, GROUPREF_EXISTS' yes
    Sub no;                      // GROUPREF_EXISTS' no (null: None)
    std::vector<Sub> branches;   // BRANCH
};

using Width = std::pair<std::uint64_t, std::uint64_t>;

struct State {
    int flags = 0;
    std::vector<std::pair<std::u32string, std::int64_t>> groupdict;  // (name, group), in the order defined
    std::vector<std::optional<Width>> groupwidths{std::nullopt};       // (group 0)
    std::optional<std::int64_t> lookbehindgroups;
    std::vector<std::pair<std::int64_t, std::int64_t>> grouprefpos;    // (group, position), in the order first seen
    std::int64_t groups() const { return static_cast<std::int64_t>(groupwidths.size()); }
    std::optional<std::int64_t> group_named(std::u32string_view name) const;
};

struct SubPattern {
    State* state = nullptr;
    std::vector<Item> data;
    std::optional<Width> width;
    Width getwidth();
};

// _parser.parse(str, flags): the pattern and its state (flags as fixed by fix_flags, groups, names).
struct Parsed {
    std::unique_ptr<State> state;
    Sub pattern;
};
Parsed parse(const std::u32string& pattern, int flags);

// --- _compiler.py -------------------------------------------------------------------------------------------------

// _compiler._code without the INFO block (it only speeds the search up): the code of the parsed pattern.
std::vector<Code> compile(const Parsed& parsed, int flags);

// --- sre_lib.h / sre.c --------------------------------------------------------------------------------------------

// What a search may spend (this build's bound: Python has none): each opcode dispatched and each character a
// repeat or a backreference walks over is a step.
struct Budget {
    std::uint64_t left = 0;
    bool spent = false;
};

struct Searched {
    bool found = false;
    std::int64_t start = 0;   // state.start (the match)
    std::int64_t end = 0;     // state.ptr
    std::vector<std::int64_t> marks;  // state.mark (-1: NULL), lastmark + 1 of them
};

// SRE(search) from `start` over text[0, end) (state.must_advance as asked): Budget::spent when the steps ran out.
Searched search(const std::vector<Code>& code, std::int64_t groups, std::u32string_view text, std::int64_t start, bool must_advance,
                Budget& budget);

}  // namespace genko::core::pyre
