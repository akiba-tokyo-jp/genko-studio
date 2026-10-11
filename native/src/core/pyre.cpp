#include "core/pyre.hpp"

#include <QByteArray>

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <vector>

#include "core/error.hpp"
#include "core/pyre_internal.hpp"

namespace genko::core {

namespace pyre {

namespace {

// re._casefix._EXTRA_CASES: a lowercase letter → the other lowercase letters with the same uppercase
struct ExtraCase {
    char32_t lower;
    std::array<char32_t, 2> others;  // (0: none)
};

constexpr ExtraCase kExtraCases[] = {
    {0x69, {0x131}}, {0x73, {0x17f}}, {0xb5, {0x3bc}}, {0x131, {0x69}}, {0x17f, {0x73}}, {0x345, {0x3b9, 0x1fbe}},
    {0x390, {0x1fd3}}, {0x3b0, {0x1fe3}}, {0x3b2, {0x3d0}}, {0x3b5, {0x3f5}}, {0x3b8, {0x3d1}},
    {0x3b9, {0x345, 0x1fbe}}, {0x3ba, {0x3f0}}, {0x3bc, {0xb5}}, {0x3c0, {0x3d6}}, {0x3c1, {0x3f1}}, {0x3c2, {0x3c3}},
    {0x3c3, {0x3c2}}, {0x3c6, {0x3d5}}, {0x3d0, {0x3b2}}, {0x3d1, {0x3b8}}, {0x3d5, {0x3c6}}, {0x3d6, {0x3c0}},
    {0x3f0, {0x3ba}}, {0x3f1, {0x3c1}}, {0x3f5, {0x3b5}}, {0x432, {0x1c80}}, {0x434, {0x1c81}}, {0x43e, {0x1c82}},
    {0x441, {0x1c83}}, {0x442, {0x1c84, 0x1c85}}, {0x44a, {0x1c86}}, {0x463, {0x1c87}}, {0x1c80, {0x432}},
    {0x1c81, {0x434}}, {0x1c82, {0x43e}}, {0x1c83, {0x441}}, {0x1c84, {0x442, 0x1c85}}, {0x1c85, {0x442, 0x1c84}},
    {0x1c86, {0x44a}}, {0x1c87, {0x463}}, {0x1c88, {0xa64b}}, {0x1e61, {0x1e9b}}, {0x1e9b, {0x1e61}},
    {0x1fbe, {0x345, 0x3b9}}, {0x1fd3, {0x390}}, {0x1fe3, {0x3b0}}, {0xa64b, {0x1c88}}, {0xfb05, {0xfb06}},
    {0xfb06, {0xfb05}},
};

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

// --- unicodedata.lookup (Modules/unicodedata.c: _getcode) ---------------------------------------------------------------

// Hangul syllables, made from their jamo (hangul_syllables: the leading, vowel and trailing short names)
constexpr const char* kJamo[][3] = {
    {"G", "A", ""},     {"GG", "AE", "G"},  {"N", "YA", "GG"},   {"D", "YAE", "GS"},  {"DD", "EO", "N"},  {"R", "E", "NJ"},
    {"M", "YEO", "NH"}, {"B", "YE", "D"},   {"BB", "O", "L"},    {"S", "WA", "LG"},   {"SS", "WAE", "LM"}, {"", "OE", "LB"},
    {"J", "YO", "LS"},  {"JJ", "U", "LT"},  {"C", "WEO", "LP"},  {"K", "WE", "LH"},   {"T", "WI", "M"},   {"P", "YU", "B"},
    {"H", "EU", "BS"},  {nullptr, "YI", "S"}, {nullptr, "I", "SS"}, {nullptr, nullptr, "NG"}, {nullptr, nullptr, "J"},
    {nullptr, nullptr, "C"}, {nullptr, nullptr, "K"}, {nullptr, nullptr, "T"}, {nullptr, nullptr, "P"}, {nullptr, nullptr, "H"},
};
constexpr int kLCount = 19;
constexpr int kVCount = 21;
constexpr int kTCount = 28;

// find_syllable: the longest jamo name of the column the text starts with (its length; its index, or -1)
std::pair<std::size_t, int> find_syllable(std::string_view text, int count, int column) {
    int found = -1;
    std::ptrdiff_t len = -1;
    for (int i = 0; i < count; ++i) {
        const char* s = kJamo[i][column];
        const auto len1 = static_cast<std::ptrdiff_t>(std::string_view(s).size());
        if (len1 <= len) continue;
        if (text.substr(0, static_cast<std::size_t>(len1)) == s) {
            len = len1;
            found = i;
        }
    }
    return {static_cast<std::size_t>(std::max<std::ptrdiff_t>(len, 0)), found};
}

bool unified_ideograph(char32_t code) {
    return (0x3400 <= code && code <= 0x4DBF) || (0x4E00 <= code && code <= 0x9FFF) || (0x20000 <= code && code <= 0x2A6DF) ||
           (0x2A700 <= code && code <= 0x2B739) || (0x2B740 <= code && code <= 0x2B81D) || (0x2B820 <= code && code <= 0x2CEA1) ||
           (0x2CEB0 <= code && code <= 0x2EBE0) || (0x30000 <= code && code <= 0x3134A) || (0x31350 <= code && code <= 0x323AF);
}

}  // namespace

}  // namespace pyre

// (made by CMake from data/unicode_names.bin: tools/migration/pyre_tables.py writes it)
extern const unsigned char kPyreUnicodeNames[];
extern const std::size_t kPyreUnicodeNamesSize;

namespace pyre {

namespace {

// Every name the hash of unicodedata finds (the names but the made ones, and the aliases), upper case, sorted.
const std::vector<std::pair<std::string, char32_t>>& names() {
    static const std::vector<std::pair<std::string, char32_t>> table = [] {
        std::vector<std::pair<std::string, char32_t>> out;
        const QByteArray text = qUncompress(reinterpret_cast<const uchar*>(kPyreUnicodeNames), static_cast<qsizetype>(kPyreUnicodeNamesSize));
        qsizetype at = 0;
        while (at < text.size()) {
            const qsizetype newline = text.indexOf('\n', at);
            const qsizetype stop = newline < 0 ? text.size() : newline;
            const qsizetype semi = text.indexOf(';', at);
            if (semi > at && semi < stop) {
                bool ok = false;
                const char32_t code = static_cast<char32_t>(text.mid(at, semi - at).toUInt(&ok, 16));
                if (ok) out.emplace_back(text.mid(semi + 1, stop - semi - 1).toStdString(), code);
            }
            at = stop + 1;
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return table;
}

}  // namespace

std::optional<char32_t> lookup_name(std::string_view name) {
    if (name.size() > 256) return std::nullopt;  // (NAME_MAXLEN: "name too long")
    // Hangul syllables (the prefix as written: upper case)
    if (name.starts_with("HANGUL SYLLABLE ")) {
        std::string_view rest = name.substr(16);
        const auto [l_len, l] = find_syllable(rest, kLCount, 0);
        rest.remove_prefix(l_len);
        const auto [v_len, v] = find_syllable(rest, kVCount, 1);
        rest.remove_prefix(v_len);
        const auto [t_len, t] = find_syllable(rest, kTCount, 2);
        rest.remove_prefix(t_len);
        if (l != -1 && v != -1 && t != -1 && rest.empty()) return static_cast<char32_t>(0xAC00 + (l * kVCount + v) * kTCount + t);
        return std::nullopt;  // (an illegal syllable name)
    }
    // unified ideographs: four or five upper-case hex digits
    if (name.starts_with("CJK UNIFIED IDEOGRAPH-")) {
        const std::string_view digits = name.substr(22);
        if (digits.size() != 4 && digits.size() != 5) return std::nullopt;
        char32_t v = 0;
        for (const char c : digits) {
            v *= 16;
            if (c >= '0' && c <= '9') v += static_cast<char32_t>(c - '0');
            else if (c >= 'A' && c <= 'F') v += static_cast<char32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        if (!unified_ideograph(v)) return std::nullopt;
        return v;
    }
    // the hash: the name compared in upper case (Py_TOUPPER: ASCII letters only)
    std::string upper_name(name);
    for (char& c : upper_name) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    const auto& table = names();
    const auto at = std::lower_bound(table.begin(), table.end(), upper_name,
                                     [](const std::pair<std::string, char32_t>& item, const std::string& value) { return item.first < value; });
    if (at == table.end() || at->first != upper_name) return std::nullopt;
    return at->second;
}

const std::array<char32_t, 2>* extra_cases(char32_t lower) {
    const auto* at = std::lower_bound(std::begin(kExtraCases), std::end(kExtraCases), lower,
                                      [](const ExtraCase& item, char32_t value) { return item.lower < value; });
    return at != std::end(kExtraCases) && at->lower == lower ? &at->others : nullptr;
}

bool is_identifier(std::u32string_view name) {
    if (name.empty() || !(xid_start(name.front()) || name.front() == U'_')) return false;
    return std::all_of(name.begin() + 1, name.end(), xid_continue);
}

std::u32string decode(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > text.size()) n = text.size() - i;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string encode(std::u32string_view text) {
    std::string out;
    for (const char32_t cp : text) append_utf8(out, cp);
    return out;
}

std::string repr(std::u32string_view text) {
    // (unicode_repr: the quote the text needs least, escapes for what is not printable)
    const bool has_single = text.find(U'\'') != std::u32string_view::npos;
    const bool has_double = text.find(U'"') != std::u32string_view::npos;
    const char32_t quote = has_single && !has_double ? U'"' : U'\'';
    static constexpr char kHex[] = "0123456789abcdef";
    const auto hex = [](std::string& out, char32_t c, int digits) {
        for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) out += kHex[(c >> shift) & 0xF];
    };
    std::string out(1, static_cast<char>(quote));
    for (const char32_t c : text) {
        if (c == quote || c == U'\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == U'\t') {
            out += "\\t";
        } else if (c == U'\n') {
            out += "\\n";
        } else if (c == U'\r') {
            out += "\\r";
        } else if (c < U' ' || c == 0x7F) {
            out += "\\x";
            hex(out, c, 2);
        } else if (c < 0x7F) {
            out += static_cast<char>(c);
        } else if (!printable(c)) {
            if (c <= 0xFF) {
                out += "\\x";
                hex(out, c, 2);
            } else if (c <= 0xFFFF) {
                out += "\\u";
                hex(out, c, 4);
            } else {
                out += "\\U";
                hex(out, c, 8);
            }
        } else {
            append_utf8(out, c);
        }
    }
    out += static_cast<char>(quote);
    return out;
}

void re_error(const std::string& msg, const std::u32string& pattern, std::int64_t pos) {
    std::string text = msg + " at position " + std::to_string(pos);
    if (pattern.find(U'\n') != std::u32string::npos) {
        // lineno = pattern.count("\n", 0, pos) + 1; colno = pos - pattern.rfind("\n", 0, pos)
        const auto end = static_cast<std::size_t>(std::clamp<std::int64_t>(pos, 0, static_cast<std::int64_t>(pattern.size())));
        const auto lines = std::count(pattern.begin(), pattern.begin() + static_cast<std::ptrdiff_t>(end), U'\n');
        const std::size_t last = end == 0 ? std::u32string::npos : pattern.rfind(U'\n', end - 1);
        const std::int64_t column = pos - (last == std::u32string::npos ? -1 : static_cast<std::int64_t>(last));
        text += " (line " + std::to_string(lines + 1) + ", column " + std::to_string(column) + ")";
    }
    throw PyUncaught("re.error", text);
}

void re_error(const std::string& msg) { throw PyUncaught("re.error", msg); }

}  // namespace pyre

namespace {

using pyre::decode;
using pyre::encode;

// --- the replacement template (re._parser.parse_template) ---------------------------------------------------------------

bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool is_octal(char32_t c) { return c >= U'0' && c <= U'7'; }
bool is_ascii_letter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

// re._parser.Tokenizer over a str: one character at a time, a backslash with the character after it
class Tokenizer {
public:
    explicit Tokenizer(const std::u32string& text) : text_(text) { advance(); }

    std::optional<std::u32string> get() {
        std::optional<std::u32string> token = next_;
        advance();
        return token;
    }

    bool match(char32_t c) {
        if (next_ && *next_ == std::u32string(1, c)) {
            advance();
            return true;
        }
        return false;
    }

    // whether the next token is the one character `c` passes
    template <class Pred>
    bool next_is(Pred pred) const {
        return next_ && next_->size() == 1 && pred((*next_)[0]);
    }

    std::u32string getuntil(char32_t terminator, const std::string& name) {
        std::u32string result;
        for (;;) {
            const std::optional<std::u32string> c = next_;
            advance();
            if (!c) {
                if (result.empty()) error("missing " + name);
                error("missing " + encode(std::u32string(1, terminator)) + ", unterminated name",
                      static_cast<std::int64_t>(result.size()));
            }
            if (*c == std::u32string(1, terminator)) {
                if (result.empty()) error("missing " + name, 1);
                break;
            }
            result += *c;
        }
        return result;
    }

    [[noreturn]] void error(const std::string& msg, std::int64_t offset = 0) const { pyre::re_error(msg, text_, tell() - offset); }
    std::int64_t tell() const {
        return static_cast<std::int64_t>(index_) - static_cast<std::int64_t>(next_ ? next_->size() : 0);
    }

private:
    void advance() {
        std::size_t index = index_;
        if (index >= text_.size()) {
            next_.reset();
            return;
        }
        std::u32string token(1, text_[index]);
        if (text_[index] == U'\\') {
            ++index;
            if (index >= text_.size()) pyre::re_error("bad escape (end of pattern)", text_, static_cast<std::int64_t>(text_.size()) - 1);
            token += text_[index];
        }
        index_ = index + 1;
        next_ = std::move(token);
    }

    const std::u32string& text_;
    std::size_t index_ = 0;
    std::optional<std::u32string> next_;
};

// A part of the replacement: words as they are, or a group of the match (\g<0> for the whole of it)
struct Piece {
    std::int64_t group = -1;  // (-1: words)
    std::string words;
};

// int(digits) as Python prints it (no leading zeros; digits of any length)
std::string decimal(std::u32string_view digits) {
    std::size_t first = 0;
    while (first + 1 < digits.size() && digits[first] == U'0') ++first;
    return encode(digits.substr(first));
}

std::uint32_t octal(std::u32string_view digits) {
    std::uint32_t value = 0;
    for (const char32_t c : digits) value = value * 8 + static_cast<std::uint32_t>(c - U'0');
    return value;
}

// int(digits) of ASCII digits: ValueError past Python's 4300 digits; held up to 2^62 (past every group number)
std::int64_t int_of(std::u32string_view digits) {
    if (digits.size() > 4300) {
        throw PyValueError("Exceeds the limit (4300 digits) for integer string conversion: value has " + std::to_string(digits.size()) +
                           " digits; use sys.set_int_max_str_digits() to increase the limit");
    }
    std::int64_t value = 0;
    for (const char32_t c : digits) {
        value = value * 10 + static_cast<std::int64_t>(c - U'0');
        if (value > (std::int64_t{1} << 62)) value = std::int64_t{1} << 62;
    }
    return value;
}

// parse_template(source, pattern): its literals and groups (the pattern's number of groups and its names)
std::vector<Piece> parse_template(const std::u32string& source, std::int64_t groups,
                                  const std::vector<std::pair<std::u32string, std::int64_t>>& groupindex) {
    Tokenizer s(source);
    std::vector<Piece> result;
    std::u32string literal;
    const auto addliteral = [&] {
        result.push_back(Piece{-1, encode(literal)});
        literal.clear();
    };
    const auto addgroup = [&](std::int64_t index, std::int64_t pos) {
        if (index > groups) s.error("invalid group reference " + std::to_string(index), pos);
        addliteral();
        result.push_back(Piece{index, {}});
    };
    for (;;) {
        std::optional<std::u32string> token = s.get();
        if (!token) break;
        std::u32string& t = *token;
        if (t[0] != U'\\') {
            literal += t;
            continue;
        }
        const char32_t c = t[1];
        if (c == U'g') {
            if (!s.match(U'<')) s.error("missing <");
            const std::u32string name = s.getuntil(U'>', "group name");
            const auto offset = static_cast<std::int64_t>(name.size()) + 1;
            std::int64_t index = 0;
            if (!std::all_of(name.begin(), name.end(), is_digit)) {
                if (!pyre::is_identifier(name)) s.error("bad character in group name " + pyre::repr(name), offset);
                const auto found = std::find_if(groupindex.begin(), groupindex.end(), [&](const auto& g) { return g.first == name; });
                if (found == groupindex.end()) throw PyUncaught("IndexError", "unknown group name " + pyre::repr(name));
                index = found->second;
            } else {
                index = int_of(name);
                if (index >= pyre::kMaxGroups) s.error("invalid group reference " + decimal(name), offset);
            }
            addgroup(index, offset);
        } else if (c == U'0') {
            if (s.next_is(is_octal)) {
                t += *s.get();
                if (s.next_is(is_octal)) t += *s.get();
            }
            literal += static_cast<char32_t>(octal(std::u32string_view(t).substr(1)) & 0xff);
        } else if (is_digit(c)) {
            bool isoctal = false;
            if (s.next_is(is_digit)) {
                t += *s.get();
                if (is_octal(c) && is_octal(t[2]) && s.next_is(is_octal)) {
                    t += *s.get();
                    isoctal = true;
                    const std::uint32_t value = octal(std::u32string_view(t).substr(1));
                    if (value > 0377) {
                        s.error("octal escape value " + encode(t) + " outside of range 0-0o377", static_cast<std::int64_t>(t.size()));
                    }
                    literal += static_cast<char32_t>(value);
                }
            }
            if (!isoctal) addgroup(int_of(std::u32string_view(t).substr(1)), static_cast<std::int64_t>(t.size()) - 1);
        } else {
            // ESCAPES: \a \b \f \n \r \t \v \\; another ASCII letter is refused; anything else stays as written
            static constexpr std::pair<char32_t, char32_t> kEscapes[] = {{U'a', U'\a'}, {U'b', U'\b'}, {U'f', U'\f'},
                                                                         {U'n', U'\n'}, {U'r', U'\r'}, {U't', U'\t'},
                                                                         {U'v', U'\v'}, {U'\\', U'\\'}};
            const auto* escape = std::find_if(std::begin(kEscapes), std::end(kEscapes), [c](const auto& e) { return e.first == c; });
            if (escape != std::end(kEscapes)) {
                literal += escape->second;
            } else {
                if (is_ascii_letter(c)) s.error("bad escape " + encode(t), static_cast<std::int64_t>(t.size()));
                literal += t;
            }
        }
    }
    addliteral();
    return result;
}

// _sre's template for subn: the replacement as it is when it has no backslash; else compiled, and as it is again when it
// refers to no group (filter_type LITERAL), else its pieces (TEMPLATE)
struct Template {
    bool literal = true;
    std::string words;
    std::vector<Piece> pieces;
};

Template template_of(std::string_view repl, std::int64_t groups, const std::vector<std::pair<std::u32string, std::int64_t>>& groupindex) {
    Template out;
    if (repl.find('\\') == std::string_view::npos) {
        out.words = std::string(repl);
        return out;
    }
    out.pieces = parse_template(decode(repl), groups, groupindex);
    if (std::none_of(out.pieces.begin(), out.pieces.end(), [](const Piece& p) { return p.group >= 0; })) {
        out.words = out.pieces.empty() ? std::string() : out.pieces.front().words;
        out.pieces.clear();
        return out;
    }
    out.literal = false;
    return out;
}

[[noreturn]] void too_slow() {
    throw Error("regex_too_slow", "the pattern takes too long to search (more than " + std::to_string(kRegexSteps) +
                                      " steps, or a million places to go back to): this build stops it rather than hold the window");
}

}  // namespace

PyLiteralPattern::PyLiteralPattern(std::string_view find, bool ignore_case) : find_(decode(find)), ignore_case_(ignore_case) {}

std::pair<std::string, std::int64_t> PyLiteralPattern::subn(std::string_view repl, std::string_view text) const {
    return subn(repl, text, nullptr);
}

std::pair<std::string, std::int64_t> PyLiteralPattern::subn(std::string_view repl, std::string_view text, std::vector<Match>* matches) const {
    // _sre's pattern_subx: a template without a backslash is used as it is; any other is compiled first
    const Template tpl = template_of(repl, 0, {});
    const std::u32string chars = decode(text);
    const std::size_t n = find_.size();
    // each character of the pattern as _sre compiles it: LITERAL (the same character), or, with IGNORECASE and a cased
    // character, LITERAL_UNI_IGNORE (a character whose lowercase is the pattern's) or IN_UNI_IGNORE (one of the
    // lowercases _EXTRA_CASES gives it)
    const auto matches_at = [&](std::size_t at) {
        for (std::size_t k = 0; k < n; ++k) {
            const char32_t p = find_[k];
            const char32_t t = chars[at + k];
            if (!ignore_case_ || !pyre::cased(p)) {
                if (t != p) return false;
                continue;
            }
            const char32_t lo = pyre::lower(p);
            const char32_t seen = pyre::lower(t);
            if (seen == lo) continue;
            const auto* extra = pyre::extra_cases(lo);
            if (extra == nullptr || std::find(extra->begin(), extra->end(), seen) == extra->end() || seen == 0) return false;
        }
        return true;
    };
    std::string out;
    std::int64_t count = 0;
    std::size_t i = 0;
    while (i < chars.size()) {
        if (n > 0 && i + n <= chars.size() && matches_at(i)) {
            const std::size_t from = out.size();
            if (tpl.literal) {
                out += tpl.words;
            } else {
                const std::string matched = encode(std::u32string_view(chars).substr(i, n));
                for (const Piece& piece : tpl.pieces) out += piece.group >= 0 ? matched : piece.words;
            }
            if (matches != nullptr) matches->push_back(Match{i, i + n, decode(std::string_view(out).substr(from))});
            ++count;
            i += n;
            continue;
        }
        pyre::append_utf8(out, chars[i]);
        ++i;
    }
    return {std::move(out), count};
}

// --- re.compile and the pattern's methods ---------------------------------------------------------------------------------

struct PyPattern::Compiled {
    std::int64_t groups = 0;  // pattern.groups
    std::vector<std::pair<std::u32string, std::int64_t>> groupindex;
    std::vector<pyre::Code> code;
};

PyPattern::PyPattern(std::string_view pattern, int flags) : compiled_(std::make_unique<Compiled>()) {
    const std::u32string text = decode(pattern);
    const pyre::Parsed parsed = pyre::parse(text, flags);
    compiled_->code = pyre::compile(parsed, flags);
    compiled_->groups = parsed.state->groups() - 1;
    compiled_->groupindex = parsed.state->groupdict;
}

PyPattern::~PyPattern() = default;
PyPattern::PyPattern(PyPattern&&) noexcept = default;
PyPattern& PyPattern::operator=(PyPattern&&) noexcept = default;

std::int64_t PyPattern::groups() const { return compiled_->groups; }

namespace {

// A match as pattern_new_match makes it: each group's span ((-1, -1) when it did not take part), SystemError where a
// group's start lies after its end.
std::vector<std::pair<std::int64_t, std::int64_t>> match_spans(const pyre::Searched& s, std::int64_t groups) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    out.emplace_back(s.start, s.end);
    const auto lastmark = static_cast<std::int64_t>(s.marks.size()) - 1;
    for (std::int64_t i = 0, j = 0; i < groups; ++i, j += 2) {
        if (j + 1 <= lastmark && s.marks[static_cast<std::size_t>(j)] >= 0 && s.marks[static_cast<std::size_t>(j + 1)] >= 0) {
            const std::int64_t a = s.marks[static_cast<std::size_t>(j)];
            const std::int64_t b = s.marks[static_cast<std::size_t>(j + 1)];
            if (a > b) throw PyUncaught("SystemError", "The span of capturing group is wrong, please report a bug for the re module.");
            out.emplace_back(a, b);
        } else {
            out.emplace_back(-1, -1);
        }
    }
    return out;
}

// The matches of a search over the text, as pattern_subx and findall walk it: `found` for each (false: stop).
template <class Found>
void each_match(const std::vector<pyre::Code>& code, std::int64_t groups, const std::u32string& chars, PyReBudget& budget, Found&& found) {
    pyre::Budget left{budget.steps, false};
    std::int64_t start = 0;
    bool must_advance = false;
    const auto end = static_cast<std::int64_t>(chars.size());
    while (start <= end) {
        const pyre::Searched s = pyre::search(code, groups, chars, start, must_advance, left);
        budget.steps = left.left;
        if (left.spent) too_slow();
        if (!s.found) break;
        found(s);
        must_advance = s.end == s.start;
        start = s.end;
    }
}

}  // namespace

std::pair<std::string, std::int64_t> PyPattern::subn(std::string_view repl, std::string_view text, std::vector<PyReMatch>* matches,
                                                     PyReBudget& budget) const {
    const Template tpl = template_of(repl, compiled_->groups, compiled_->groupindex);
    const std::u32string chars = decode(text);
    std::string out;
    std::int64_t n = 0;
    std::int64_t i = 0;
    each_match(compiled_->code, compiled_->groups, chars, budget, [&](const pyre::Searched& s) {
        if (i < s.start) out += encode(std::u32string_view(chars).substr(static_cast<std::size_t>(i), static_cast<std::size_t>(s.start - i)));
        const std::size_t from = out.size();
        if (tpl.literal) {
            out += tpl.words;
        } else {
            const auto spans = match_spans(s, compiled_->groups);
            for (const Piece& piece : tpl.pieces) {
                if (piece.group < 0) {
                    out += piece.words;
                    continue;
                }
                const auto [a, b] = spans[static_cast<std::size_t>(piece.group)];
                if (a >= 0) out += encode(std::u32string_view(chars).substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a)));
            }
        }
        if (matches != nullptr) {
            matches->push_back(PyReMatch{static_cast<std::size_t>(s.start), static_cast<std::size_t>(s.end), decode(std::string_view(out).substr(from))});
        }
        i = s.end;
        ++n;
    });
    if (i < static_cast<std::int64_t>(chars.size())) out += encode(std::u32string_view(chars).substr(static_cast<std::size_t>(i)));
    return {std::move(out), n};
}

std::int64_t PyPattern::count(std::string_view text, PyReBudget& budget) const {
    std::int64_t n = 0;
    each_match(compiled_->code, compiled_->groups, decode(text), budget, [&](const pyre::Searched& s) {
        // (findall takes each group's text when there are groups: SystemError where a span came out reversed)
        if (compiled_->groups > 0) (void)match_spans(s, compiled_->groups);
        ++n;
    });
    return n;
}

std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>> PyPattern::spans(std::string_view text, PyReBudget& budget) const {
    std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>> out;
    each_match(compiled_->code, compiled_->groups, decode(text), budget,
               [&](const pyre::Searched& s) { out.push_back(match_spans(s, compiled_->groups)); });
    return out;
}

}  // namespace genko::core
