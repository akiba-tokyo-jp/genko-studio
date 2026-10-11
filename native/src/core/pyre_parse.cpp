// Python 3.12's re._parser for str patterns (CPython v3.12.3 Lib/re/_parser.py; PSF License,
// native/third_party/cpython): the pattern read into subpatterns, with Python's errors at Python's positions. Python's
// warnings (FutureWarning for a possible nested set, DeprecationWarning for an odd group name) change nothing and are
// not given.

#include <algorithm>
#include <limits>

#include "core/error.hpp"
#include "core/pyre_internal.hpp"

namespace genko::core::pyre {

namespace {

bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool is_octal(char32_t c) { return c >= U'0' && c <= U'7'; }
bool is_hex(char32_t c) { return is_digit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F'); }
bool is_ascii_letter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
bool is_whitespace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\v' || c == U'\f'; }

std::string text_of(std::u32string_view s) { return encode(s); }

// int(digits) for ASCII digits, as Python makes it: ValueError past 4300 digits (sys.int_info's limit), else the value
// held as far as it matters (anything past 2^62 is past every bound it is compared with)
std::uint64_t int_of(std::u32string_view digits) {
    if (digits.size() > 4300) {
        throw PyValueError("Exceeds the limit (4300 digits) for integer string conversion: value has " + std::to_string(digits.size()) +
                           " digits; use sys.set_int_max_str_digits() to increase the limit");
    }
    std::uint64_t value = 0;
    for (const char32_t c : digits) {
        value = value * 10 + static_cast<std::uint64_t>(c - U'0');
        if (value > (std::uint64_t{1} << 62)) value = std::uint64_t{1} << 62;
    }
    return value;
}

// "%d" of int(digits) (no leading zeros)
std::string decimal_text(std::u32string_view digits) {
    std::size_t first = 0;
    while (first + 1 < digits.size() && digits[first] == U'0') ++first;
    return encode(digits.substr(first));
}

std::uint32_t parse_radix(std::u32string_view digits, int radix) {
    std::uint64_t value = 0;
    for (const char32_t c : digits) {
        const int d = is_digit(c) ? static_cast<int>(c - U'0') : (c >= U'a' && c <= U'f') ? static_cast<int>(c - U'a' + 10) : static_cast<int>(c - U'A' + 10);
        value = value * static_cast<std::uint64_t>(radix) + static_cast<std::uint64_t>(d);
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t add_width(std::uint64_t a, std::uint64_t b) { return a > kMaxWidth - b ? kMaxWidth : a + b; }
std::uint64_t mul_width(std::uint64_t a, std::uint64_t b) {
    if (a == 0 || b == 0) return 0;
    return a > kMaxWidth / b ? kMaxWidth : a * b;
}

// _parser.Tokenizer over a str
class Tokenizer {
public:
    explicit Tokenizer(const std::u32string& text) : text_(text) { advance(); }

    const std::optional<std::u32string>& next() const { return next_; }
    bool next_is(char32_t c) const { return next_ && next_->size() == 1 && (*next_)[0] == c; }
    template <class Pred>
    bool next_in(Pred pred) const {
        return next_ && next_->size() == 1 && pred((*next_)[0]);
    }

    bool match(char32_t c) {
        if (next_is(c)) {
            advance();
            return true;
        }
        return false;
    }
    std::optional<std::u32string> get() {
        std::optional<std::u32string> token = next_;
        advance();
        return token;
    }
    template <class Pred>
    std::u32string getwhile(int n, Pred pred) {
        std::u32string result;
        for (int i = 0; i < n; ++i) {
            if (!next_in(pred)) break;
            result += *next_;
            advance();
        }
        return result;
    }
    std::u32string getuntil(char32_t terminator, const std::string& name) {
        std::u32string result;
        for (;;) {
            const std::optional<std::u32string> c = next_;
            advance();
            if (!c) {
                if (result.empty()) error("missing " + name);
                error("missing " + encode(std::u32string(1, terminator)) + ", unterminated name", static_cast<std::int64_t>(result.size()));
            }
            if (*c == std::u32string(1, terminator)) {
                if (result.empty()) error("missing " + name, 1);
                break;
            }
            result += *c;
        }
        return result;
    }
    std::int64_t tell() const { return static_cast<std::int64_t>(index_) - static_cast<std::int64_t>(next_ ? next_->size() : 0); }
    void seek(std::int64_t index) {
        index_ = static_cast<std::size_t>(index);
        advance();
    }
    [[noreturn]] void error(const std::string& msg, std::int64_t offset = 0) const { re_error(msg, text_, tell() - offset); }
    void checkgroupname(const std::u32string& name, std::int64_t offset) const {
        if (!is_identifier(name)) error("bad character in group name " + repr(name), static_cast<std::int64_t>(name.size()) + offset);
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
            if (index >= text_.size()) re_error("bad escape (end of pattern)", text_, static_cast<std::int64_t>(text_.size()) - 1);
            token += text_[index];
        }
        index_ = index + 1;
        next_ = std::move(token);
    }

    const std::u32string& text_;
    std::size_t index_ = 0;
    std::optional<std::u32string> next_;
};

Item literal(char32_t c) {
    Item item;
    item.op = LITERAL;
    item.value = static_cast<Code>(c);
    return item;
}

Item at(Code code) {
    Item item;
    item.op = AT;
    item.value = code;
    return item;
}

Item in_category(Code category) {
    Item item;
    item.op = IN;
    item.set.push_back(SetItem{CATEGORY, category, 0, {}});
    return item;
}

// CATEGORIES (\A \b \B \d \D \s \S \w \W \Z)
std::optional<Item> category_escape(char32_t c) {
    switch (c) {
        case U'A': return at(AT_BEGINNING_STRING);
        case U'b': return at(AT_BOUNDARY);
        case U'B': return at(AT_NON_BOUNDARY);
        case U'd': return in_category(CATEGORY_DIGIT);
        case U'D': return in_category(CATEGORY_NOT_DIGIT);
        case U's': return in_category(CATEGORY_SPACE);
        case U'S': return in_category(CATEGORY_NOT_SPACE);
        case U'w': return in_category(CATEGORY_WORD);
        case U'W': return in_category(CATEGORY_NOT_WORD);
        case U'Z': return at(AT_END_STRING);
        default: return std::nullopt;
    }
}

// ESCAPES (\a \b \f \n \r \t \v \\)
std::optional<char32_t> plain_escape(char32_t c) {
    switch (c) {
        case U'a': return U'\a';
        case U'b': return U'\b';
        case U'f': return U'\f';
        case U'n': return U'\n';
        case U'r': return U'\r';
        case U't': return U'\t';
        case U'v': return U'\v';
        case U'\\': return U'\\';
        default: return std::nullopt;
    }
}

// The escapes both places read alike: \x, \u, \U and \N (nothing for the others)
std::optional<Item> code_escape(Tokenizer& source, std::u32string& escape) {
    const char32_t c = escape[1];
    if (c == U'x') {
        escape += source.getwhile(2, is_hex);
        if (escape.size() != 4) source.error("incomplete escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
        return literal(parse_radix(std::u32string_view(escape).substr(2), 16));
    }
    if (c == U'u') {
        escape += source.getwhile(4, is_hex);
        if (escape.size() != 6) source.error("incomplete escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
        return literal(parse_radix(std::u32string_view(escape).substr(2), 16));
    }
    if (c == U'U') {
        escape += source.getwhile(8, is_hex);
        if (escape.size() != 10) source.error("incomplete escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
        const std::uint32_t value = parse_radix(std::u32string_view(escape).substr(2), 16);
        if (value > 0x10FFFF) source.error("bad escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));  // (chr's ValueError)
        return literal(value);
    }
    if (c == U'N') {
        if (!source.match(U'{')) source.error("missing {");
        const std::u32string charname = source.getuntil(U'}', "character name");
        const std::optional<char32_t> found = lookup_name(encode(charname));
        if (!found) {
            source.error("undefined character name " + repr(charname), static_cast<std::int64_t>(charname.size()) + 4);
        }
        return literal(*found);
    }
    return std::nullopt;
}

// _class_escape: an escape inside a set
Item class_escape(Tokenizer& source, std::u32string escape) {
    if (const auto e = plain_escape(escape[1])) return literal(*e);
    if (auto code = category_escape(escape[1]); code && code->op == IN) return *code;
    const char32_t c = escape[1];
    if (auto code = code_escape(source, escape)) return *code;
    if (is_octal(c)) {
        escape += source.getwhile(2, is_octal);
        const std::uint32_t value = parse_radix(std::u32string_view(escape).substr(1), 8);
        if (value > 0377) {
            source.error("octal escape value " + text_of(escape) + " outside of range 0-0o377", static_cast<std::int64_t>(escape.size()));
        }
        return literal(value);
    }
    if (!is_digit(c) && escape.size() == 2) {
        if (is_ascii_letter(c)) source.error("bad escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
        return literal(escape[1]);
    }
    source.error("bad escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
}

void checklookbehindgroup(const State& state, std::int64_t gid, const Tokenizer& source);

bool checkgroup(const State& state, std::int64_t gid) {
    return gid < state.groups() && state.groupwidths[static_cast<std::size_t>(gid)].has_value();
}

void checklookbehindgroup(const State& state, std::int64_t gid, const Tokenizer& source) {
    if (state.lookbehindgroups) {
        if (!checkgroup(state, gid)) source.error("cannot refer to an open group");
        if (gid >= *state.lookbehindgroups) source.error("cannot refer to group defined in the same lookbehind subpattern");
    }
}

// _escape: an escape in the pattern
Item escape_item(Tokenizer& source, std::u32string escape, State& state) {
    if (auto code = category_escape(escape[1])) return *code;
    if (const auto e = plain_escape(escape[1])) return literal(*e);
    const char32_t c = escape[1];
    if (auto code = code_escape(source, escape)) return *code;
    if (c == U'0') {
        escape += source.getwhile(2, is_octal);
        return literal(parse_radix(std::u32string_view(escape).substr(1), 8));
    }
    if (is_digit(c)) {
        // octal escape *or* decimal group reference (sigh)
        if (source.next_in(is_digit)) {
            escape += *source.get();
            if (is_octal(escape[1]) && is_octal(escape[2]) && source.next_in(is_octal)) {
                escape += *source.get();
                const std::uint32_t value = parse_radix(std::u32string_view(escape).substr(1), 8);
                if (value > 0377) {
                    source.error("octal escape value " + text_of(escape) + " outside of range 0-0o377", static_cast<std::int64_t>(escape.size()));
                }
                return literal(value);
            }
        }
        const auto group = static_cast<std::int64_t>(parse_radix(std::u32string_view(escape).substr(1), 10));
        if (group < state.groups()) {
            if (!checkgroup(state, group)) source.error("cannot refer to an open group", static_cast<std::int64_t>(escape.size()));
            checklookbehindgroup(state, group, source);
            Item item;
            item.op = GROUPREF;
            item.value = static_cast<Code>(group);
            return item;
        }
        source.error("invalid group reference " + std::to_string(group), static_cast<std::int64_t>(escape.size()) - 1);
    }
    if (escape.size() == 2) {
        if (is_ascii_letter(c)) source.error("bad escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
        return literal(escape[1]);
    }
    source.error("bad escape " + text_of(escape), static_cast<std::int64_t>(escape.size()));
}

// (op, av) == (op, av) as Python's tuples compare them: subpatterns by identity (never equal here: each is its own)
bool same_item(const Item& a, const Item& b) {
    if (a.op != b.op) return false;
    switch (a.op) {
        case LITERAL:
        case NOT_LITERAL:
        case AT:
        case GROUPREF:
        case CATEGORY: return a.value == b.value;
        case ANY: return true;
        case IN: return a.set == b.set;
        default: return false;
    }
}

// _uniq: the set's members once each, in their first order
std::vector<SetItem> uniq(const std::vector<SetItem>& items) {
    std::vector<SetItem> out;
    for (const SetItem& item : items) {
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
    return out;
}

Sub new_sub(State& state) {
    auto sub = std::make_shared<SubPattern>();
    sub->state = &state;
    return sub;
}

std::optional<int> flag_of(char32_t c) {
    switch (c) {
        case U'i': return FLAG_IGNORECASE;
        case U'L': return FLAG_LOCALE;
        case U'm': return FLAG_MULTILINE;
        case U's': return FLAG_DOTALL;
        case U'x': return FLAG_VERBOSE;
        case U'a': return FLAG_ASCII;
        case U't': return FLAG_TEMPLATE;
        case U'u': return FLAG_UNICODE;
        default: return std::nullopt;
    }
}

constexpr int kTypeFlags = FLAG_ASCII | FLAG_LOCALE | FLAG_UNICODE;
constexpr int kGlobalFlags = FLAG_DEBUG | FLAG_TEMPLATE;

std::optional<int> token_flag(const std::optional<std::u32string>& token) {
    if (!token || token->size() != 1) return std::nullopt;
    return flag_of((*token)[0]);
}

bool token_is(const std::optional<std::u32string>& token, char32_t c) { return token && token->size() == 1 && (*token)[0] == c; }

bool token_alpha(const std::u32string& token) {
    return !token.empty() && std::all_of(token.begin(), token.end(), [](char32_t c) { return alpha(c); });
}

// _parse_flags: (add_flags, del_flags), or nothing for global flags
std::optional<std::pair<int, int>> parse_flags(Tokenizer& source, State& state, std::u32string ch) {
    int add_flags = 0;
    int del_flags = 0;
    std::optional<std::u32string> c = ch;
    if (!token_is(c, U'-')) {
        for (;;) {
            const int flag = *token_flag(c);
            if (token_is(c, U'L')) source.error("bad inline flags: cannot use 'L' flag with a str pattern");
            add_flags |= flag;
            if ((flag & kTypeFlags) != 0 && (add_flags & kTypeFlags) != flag) {
                source.error("bad inline flags: flags 'a', 'u' and 'L' are incompatible");
            }
            c = source.get();
            if (!c) source.error("missing -, : or )");
            if (token_is(c, U')') || token_is(c, U'-') || token_is(c, U':')) break;
            if (!token_flag(c)) {
                source.error(token_alpha(*c) ? "unknown flag" : "missing -, : or )", static_cast<std::int64_t>(c->size()));
            }
        }
    }
    if (token_is(c, U')')) {
        state.flags |= add_flags;
        return std::nullopt;
    }
    if ((add_flags & kGlobalFlags) != 0) source.error("bad inline flags: cannot turn on global flag", 1);
    if (token_is(c, U'-')) {
        c = source.get();
        if (!c) source.error("missing flag");
        if (!token_flag(c)) source.error(token_alpha(*c) ? "unknown flag" : "missing flag", static_cast<std::int64_t>(c->size()));
        for (;;) {
            const int flag = *token_flag(c);
            if ((flag & kTypeFlags) != 0) source.error("bad inline flags: cannot turn off flags 'a', 'u' and 'L'");
            del_flags |= flag;
            c = source.get();
            if (!c) source.error("missing :");
            if (token_is(c, U':')) break;
            if (!token_flag(c)) source.error(token_alpha(*c) ? "unknown flag" : "missing :", static_cast<std::int64_t>(c->size()));
        }
    }
    if ((del_flags & kGlobalFlags) != 0) source.error("bad inline flags: cannot turn off global flag", 1);
    if ((add_flags & del_flags) != 0) source.error("bad inline flags: flag turned on and off", 1);
    return std::pair{add_flags, del_flags};
}

std::int64_t opengroup(State& state, const std::optional<std::u32string>& name, Tokenizer& source) {
    const std::int64_t gid = state.groups();
    state.groupwidths.emplace_back(std::nullopt);
    if (state.groups() > kMaxGroups) source.error("too many groups", name ? static_cast<std::int64_t>(name->size()) + 1 : 0);
    if (name) {
        if (const auto ogid = state.group_named(*name)) {
            source.error("redefinition of group name " + repr(*name) + " as group " + std::to_string(gid) + "; was group " +
                             std::to_string(*ogid),
                         static_cast<std::int64_t>(name->size()) + 1);
        }
        state.groupdict.emplace_back(*name, gid);
    }
    return gid;
}

Sub parse_sub(Tokenizer& source, State& state, bool verbose, int nested);

Sub parse_item(Tokenizer& source, State& state, bool verbose, int nested, bool first = false) {
    if (nested > 2 * kMaxNesting + 1) {
        throw Error("regex_nested", "the pattern nests too deeply (groups and look-arounds at most " + std::to_string(kMaxNesting) +
                                        " levels inside one another)");
    }
    Sub subpattern = new_sub(state);
    for (;;) {
        const std::optional<std::u32string> peek = source.next();
        if (!peek) break;  // end of pattern
        if (token_is(peek, U'|') || token_is(peek, U')')) break;  // end of subpattern
        std::u32string self = *source.get();
        if (verbose) {
            // skip whitespace and comments
            if (self.size() == 1 && is_whitespace(self[0])) continue;
            if (self == U"#") {
                for (;;) {
                    const std::optional<std::u32string> t = source.get();
                    if (!t || *t == U"\n") break;
                }
                continue;
            }
        }
        if (self[0] == U'\\') {
            subpattern->data.push_back(escape_item(source, self, state));
            continue;
        }
        const char32_t c = self[0];
        if (std::u32string_view(U".\\[{()*+?^$|").find(c) == std::u32string_view::npos) {
            subpattern->data.push_back(literal(c));
            continue;
        }
        if (c == U'[') {
            const std::int64_t here = source.tell() - 1;
            // character set
            std::vector<SetItem> set;
            const bool negate = source.match(U'^');
            for (;;) {
                const std::optional<std::u32string> t = source.get();
                if (!t) source.error("unterminated character set", source.tell() - here);
                const std::u32string& thing = *t;
                if (thing == U"]" && !set.empty()) break;
                Item code1;
                if (thing[0] == U'\\') {
                    code1 = class_escape(source, thing);
                } else {
                    code1 = literal(thing[0]);
                }
                if (source.match(U'-')) {
                    // potential range
                    const std::optional<std::u32string> that = source.get();
                    if (!that) source.error("unterminated character set", source.tell() - here);
                    if (*that == U"]") {
                        if (code1.op == IN) set.push_back(code1.set[0]);
                        else set.push_back(SetItem{LITERAL, code1.value, 0, {}});
                        set.push_back(SetItem{LITERAL, U'-', 0, {}});
                        break;
                    }
                    Item code2;
                    if ((*that)[0] == U'\\') code2 = class_escape(source, *that);
                    else code2 = literal((*that)[0]);
                    const auto bad = [&] {
                        source.error("bad character range " + text_of(thing) + "-" + text_of(*that),
                                     static_cast<std::int64_t>(thing.size() + 1 + that->size()));
                    };
                    if (code1.op != LITERAL || code2.op != LITERAL) bad();
                    if (code2.value < code1.value) bad();
                    set.push_back(SetItem{RANGE, code1.value, code2.value, {}});
                } else {
                    if (code1.op == IN) set.push_back(code1.set[0]);
                    else set.push_back(SetItem{LITERAL, code1.value, 0, {}});
                }
            }
            set = uniq(set);
            if (set.size() == 1 && set[0].op == LITERAL) {
                Item item = literal(set[0].a);
                if (negate) item.op = NOT_LITERAL;
                subpattern->data.push_back(item);
            } else {
                if (negate) set.insert(set.begin(), SetItem{NEGATE, 0, 0, {}});
                Item item;
                item.op = IN;
                item.set = std::move(set);
                subpattern->data.push_back(std::move(item));
            }
            continue;
        }
        if (c == U'*' || c == U'+' || c == U'?' || c == U'{') {
            // repeat previous item
            const std::int64_t here = source.tell();
            Code min = 0;
            Code max = kMaxRepeat;
            if (c == U'?') {
                max = 1;
            } else if (c == U'+') {
                min = 1;
            } else if (c == U'{') {
                if (source.next_is(U'}')) {
                    subpattern->data.push_back(literal(c));
                    continue;
                }
                std::u32string lo;
                std::u32string hi;
                while (source.next_in(is_digit)) lo += *source.get();
                if (source.match(U',')) {
                    while (source.next_in(is_digit)) hi += *source.get();
                } else {
                    hi = lo;
                }
                if (!source.match(U'}')) {
                    subpattern->data.push_back(literal(c));
                    source.seek(here);
                    continue;
                }
                if (!lo.empty()) {
                    const std::uint64_t value = int_of(lo);
                    if (value >= kMaxRepeat) throw PyUncaught("OverflowError", "the repetition number is too large");
                    min = static_cast<Code>(value);
                }
                if (!hi.empty()) {
                    const std::uint64_t value = int_of(hi);
                    if (value >= kMaxRepeat) throw PyUncaught("OverflowError", "the repetition number is too large");
                    max = static_cast<Code>(value);
                    if (max < min) source.error("min repeat greater than max repeat", source.tell() - here);
                }
            }
            // figure out which item to repeat
            if (subpattern->data.empty() || subpattern->data.back().op == AT) {
                source.error("nothing to repeat", source.tell() - here + static_cast<std::int64_t>(self.size()));
            }
            const Code last = subpattern->data.back().op;
            if (last == MIN_REPEAT || last == MAX_REPEAT || last == POSSESSIVE_REPEAT) {
                source.error("multiple repeat", source.tell() - here + static_cast<std::int64_t>(self.size()));
            }
            Sub item;
            const Item& previous = subpattern->data.back();
            if (previous.op == SUBPATTERN && previous.group < 0 && previous.add_flags == 0 && previous.del_flags == 0) {
                item = previous.sub;
            } else {
                item = new_sub(state);
                item->data.push_back(previous);
            }
            Item repeat;
            repeat.min = min;
            repeat.max = max;
            repeat.sub = std::move(item);
            if (source.match(U'?')) repeat.op = MIN_REPEAT;  // Non-Greedy Match
            else if (source.match(U'+')) repeat.op = POSSESSIVE_REPEAT;  // Possessive Match (Always Greedy)
            else repeat.op = MAX_REPEAT;  // Greedy Match
            subpattern->data.back() = std::move(repeat);
            continue;
        }
        if (c == U'.') {
            Item item;
            item.op = ANY;
            subpattern->data.push_back(item);
            continue;
        }
        if (c == U'(') {
            const std::int64_t start = source.tell() - 1;
            bool capture = true;
            bool atomic = false;
            std::optional<std::u32string> name;
            int add_flags = 0;
            int del_flags = 0;
            bool sub_verbose_set = false;
            if (source.match(U'?')) {
                // options
                std::optional<std::u32string> ch = source.get();
                if (!ch) source.error("unexpected end of pattern");
                if (token_is(ch, U'P')) {
                    // python extensions
                    if (source.match(U'<')) {
                        // named group: skip forward to end of name
                        name = source.getuntil(U'>', "group name");
                        source.checkgroupname(*name, 1);
                    } else if (source.match(U'=')) {
                        // named backreference
                        const std::u32string ref = source.getuntil(U')', "group name");
                        source.checkgroupname(ref, 1);
                        const auto gid = state.group_named(ref);
                        if (!gid) source.error("unknown group name " + repr(ref), static_cast<std::int64_t>(ref.size()) + 1);
                        if (!checkgroup(state, *gid)) source.error("cannot refer to an open group", static_cast<std::int64_t>(ref.size()) + 1);
                        checklookbehindgroup(state, *gid, source);
                        Item item;
                        item.op = GROUPREF;
                        item.value = static_cast<Code>(*gid);
                        subpattern->data.push_back(item);
                        continue;
                    } else {
                        const std::optional<std::u32string> more = source.get();
                        if (!more) source.error("unexpected end of pattern");
                        source.error("unknown extension ?P" + text_of(*more), static_cast<std::int64_t>(more->size()) + 2);
                    }
                } else if (token_is(ch, U':')) {
                    // non-capturing group
                    capture = false;
                } else if (token_is(ch, U'#')) {
                    // comment
                    for (;;) {
                        if (!source.next()) source.error("missing ), unterminated comment", source.tell() - start);
                        if (token_is(source.get(), U')')) break;
                    }
                    continue;
                } else if (token_is(ch, U'=') || token_is(ch, U'!') || token_is(ch, U'<')) {
                    // lookahead assertions
                    int dir = 1;
                    std::optional<std::int64_t> lookbehindgroups;
                    if (token_is(ch, U'<')) {
                        ch = source.get();
                        if (!ch) source.error("unexpected end of pattern");
                        if (!token_is(ch, U'=') && !token_is(ch, U'!')) source.error("unknown extension ?<" + text_of(*ch), static_cast<std::int64_t>(ch->size()) + 2);
                        dir = -1;  // lookbehind
                        lookbehindgroups = state.lookbehindgroups;
                        if (!lookbehindgroups) state.lookbehindgroups = state.groups();
                    }
                    Sub p = parse_sub(source, state, verbose, nested + 1);
                    if (dir < 0 && !lookbehindgroups) state.lookbehindgroups.reset();
                    if (!source.match(U')')) source.error("missing ), unterminated subpattern", source.tell() - start);
                    Item item;
                    item.op = token_is(ch, U'=') ? ASSERT : ASSERT_NOT;
                    item.dir = dir;
                    item.sub = std::move(p);
                    subpattern->data.push_back(std::move(item));
                    continue;
                } else if (token_is(ch, U'(')) {
                    // conditional backreference group
                    const std::u32string condname = source.getuntil(U')', "group name");
                    std::int64_t condgroup = 0;
                    if (!(!condname.empty() && std::all_of(condname.begin(), condname.end(), is_digit))) {
                        source.checkgroupname(condname, 1);
                        const auto gid = state.group_named(condname);
                        if (!gid) source.error("unknown group name " + repr(condname), static_cast<std::int64_t>(condname.size()) + 1);
                        condgroup = *gid;
                    } else {
                        const std::uint64_t value = int_of(condname);
                        if (value == 0) source.error("bad group number", static_cast<std::int64_t>(condname.size()) + 1);
                        if (value >= static_cast<std::uint64_t>(kMaxGroups)) {
                            source.error("invalid group reference " + decimal_text(condname), static_cast<std::int64_t>(condname.size()) + 1);
                        }
                        condgroup = static_cast<std::int64_t>(value);
                        const bool seen = std::any_of(state.grouprefpos.begin(), state.grouprefpos.end(),
                                                      [&](const auto& g) { return g.first == condgroup; });
                        if (!seen) state.grouprefpos.emplace_back(condgroup, source.tell() - static_cast<std::int64_t>(condname.size()) - 1);
                    }
                    checklookbehindgroup(state, condgroup, source);
                    Sub item_yes = parse_item(source, state, verbose, nested + 1);
                    Sub item_no;
                    if (source.match(U'|')) {
                        item_no = parse_item(source, state, verbose, nested + 1);
                        if (source.next_is(U'|')) source.error("conditional backref with more than two branches");
                    }
                    if (!source.match(U')')) source.error("missing ), unterminated subpattern", source.tell() - start);
                    Item item;
                    item.op = GROUPREF_EXISTS;
                    item.group = condgroup;
                    item.sub = std::move(item_yes);
                    item.no = std::move(item_no);
                    subpattern->data.push_back(std::move(item));
                    continue;
                } else if (token_is(ch, U'>')) {
                    // non-capturing, atomic group
                    capture = false;
                    atomic = true;
                } else if (token_flag(ch) || token_is(ch, U'-')) {
                    // flags
                    const auto flags = parse_flags(source, state, *ch);
                    if (!flags) {  // global flags
                        if (!first || !subpattern->data.empty()) {
                            source.error("global flags not at the start of the expression", source.tell() - start);
                        }
                        verbose = (state.flags & FLAG_VERBOSE) != 0;
                        continue;
                    }
                    add_flags = flags->first;
                    del_flags = flags->second;
                    capture = false;
                } else {
                    source.error("unknown extension ?" + text_of(*ch), static_cast<std::int64_t>(ch->size()) + 1);
                }
            }
            (void)sub_verbose_set;
            // parse group contents
            std::int64_t group = -1;
            if (capture) group = opengroup(state, name, source);
            const bool sub_verbose = (verbose || (add_flags & FLAG_VERBOSE) != 0) && (del_flags & FLAG_VERBOSE) == 0;
            Sub p = parse_sub(source, state, sub_verbose, nested + 1);
            if (!source.match(U')')) source.error("missing ), unterminated subpattern", source.tell() - start);
            if (group >= 0) state.groupwidths[static_cast<std::size_t>(group)] = p->getwidth();  // (closegroup)
            Item item;
            if (atomic) {
                item.op = ATOMIC_GROUP;
                item.sub = std::move(p);
            } else {
                item.op = SUBPATTERN;
                item.group = group;
                item.add_flags = add_flags;
                item.del_flags = del_flags;
                item.sub = std::move(p);
            }
            subpattern->data.push_back(std::move(item));
            continue;
        }
        if (c == U'^') {
            subpattern->data.push_back(at(AT_BEGINNING));
            continue;
        }
        if (c == U'$') {
            subpattern->data.push_back(at(AT_END));
            continue;
        }
    }
    // unpack non-capturing groups
    for (std::size_t i = subpattern->data.size(); i-- > 0;) {
        const Item& item = subpattern->data[i];
        if (item.op == SUBPATTERN && item.group < 0 && item.add_flags == 0 && item.del_flags == 0) {
            const Sub p = item.sub;
            subpattern->data.erase(subpattern->data.begin() + static_cast<std::ptrdiff_t>(i));
            subpattern->data.insert(subpattern->data.begin() + static_cast<std::ptrdiff_t>(i), p->data.begin(), p->data.end());
        }
    }
    return subpattern;
}

// _parse_sub: an alternation a|b|c
Sub parse_sub(Tokenizer& source, State& state, bool verbose, int nested) {
    std::vector<Sub> items;
    for (;;) {
        items.push_back(parse_item(source, state, verbose, nested + 1, nested == 0 && items.empty()));
        if (!source.match(U'|')) break;
        if (nested == 0) verbose = (state.flags & FLAG_VERBOSE) != 0;
    }
    if (items.size() == 1) return items[0];
    Sub subpattern = new_sub(state);
    // check if all items share a common prefix
    for (;;) {
        const Item* prefix = nullptr;
        bool all = true;
        for (const Sub& item : items) {
            if (item->data.empty()) {
                all = false;
                break;
            }
            if (prefix == nullptr) {
                prefix = &item->data[0];
            } else if (!same_item(item->data[0], *prefix)) {
                all = false;
                break;
            }
        }
        if (!all) break;
        // all subitems start with a common "prefix": move it out of the branch
        const Item common = *prefix;
        for (const Sub& item : items) item->data.erase(item->data.begin());
        subpattern->data.push_back(common);
    }
    // check if the branch can be replaced by a character set
    std::vector<SetItem> set;
    bool as_set = true;
    for (const Sub& item : items) {
        if (item->data.size() != 1) {
            as_set = false;
            break;
        }
        const Item& only = item->data[0];
        if (only.op == LITERAL) {
            set.push_back(SetItem{LITERAL, only.value, 0, {}});
        } else if (only.op == IN && only.set[0].op != NEGATE) {
            set.insert(set.end(), only.set.begin(), only.set.end());
        } else {
            as_set = false;
            break;
        }
    }
    if (as_set) {
        // we can store this as a character set instead of a branch (the compiler may optimize this even more)
        Item item;
        item.op = IN;
        item.set = uniq(set);
        subpattern->data.push_back(std::move(item));
        return subpattern;
    }
    Item branch;
    branch.op = BRANCH;
    branch.branches = std::move(items);
    subpattern->data.push_back(std::move(branch));
    return subpattern;
}

}  // namespace

std::optional<std::int64_t> State::group_named(std::u32string_view name) const {
    for (const auto& [n, gid] : groupdict) {
        if (n == name) return gid;
    }
    return std::nullopt;
}

Width SubPattern::getwidth() {
    // determine the width (min, max) for this subpattern
    if (width) return *width;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    for (const Item& item : data) {
        if (item.op == BRANCH) {
            std::uint64_t i = kMaxWidth;
            std::uint64_t j = 0;
            for (const Sub& b : item.branches) {
                const auto [l, h] = b->getwidth();
                i = std::min(i, l);
                j = std::max(j, h);
            }
            lo = add_width(lo, i);
            hi = add_width(hi, j);
        } else if (item.op == ATOMIC_GROUP || item.op == SUBPATTERN) {
            const auto [i, j] = item.sub->getwidth();
            lo = add_width(lo, i);
            hi = add_width(hi, j);
        } else if (item.op == MIN_REPEAT || item.op == MAX_REPEAT || item.op == POSSESSIVE_REPEAT) {
            const auto [i, j] = item.sub->getwidth();
            lo = add_width(lo, mul_width(i, item.min));
            if (item.max == kMaxRepeat && j != 0) hi = kMaxWidth;
            else hi = add_width(hi, mul_width(j, item.max));
        } else if (item.op == ANY || item.op == RANGE || item.op == IN || item.op == LITERAL || item.op == NOT_LITERAL ||
                   item.op == CATEGORY) {
            lo = add_width(lo, 1);
            hi = add_width(hi, 1);
        } else if (item.op == GROUPREF) {
            const auto [i, j] = *state->groupwidths[item.value];
            lo = add_width(lo, i);
            hi = add_width(hi, j);
        } else if (item.op == GROUPREF_EXISTS) {
            auto [i, j] = item.sub->getwidth();
            if (item.no) {
                const auto [l, h] = item.no->getwidth();
                i = std::min(i, l);
                j = std::max(j, h);
            } else {
                i = 0;
            }
            lo = add_width(lo, i);
            hi = add_width(hi, j);
        } else if (item.op == SUCCESS) {
            break;
        }
    }
    width = Width{lo, hi};
    return *width;
}

Parsed parse(const std::u32string& pattern, int flags) {
    Tokenizer source(pattern);
    Parsed out;
    out.state = std::make_unique<State>();
    State& state = *out.state;
    state.flags = flags;
    out.pattern = parse_sub(source, state, (flags & FLAG_VERBOSE) != 0, 0);
    // fix_flags for a str
    if ((state.flags & FLAG_LOCALE) != 0) throw PyValueError("cannot use LOCALE flag with a str pattern");
    if ((state.flags & FLAG_ASCII) == 0) state.flags |= FLAG_UNICODE;
    else if ((state.flags & FLAG_UNICODE) != 0) throw PyValueError("ASCII and UNICODE flags are incompatible");
    if (source.next()) source.error("unbalanced parenthesis");
    for (const auto& [g, pos] : state.grouprefpos) {
        if (g >= state.groups()) re_error("invalid group reference " + std::to_string(g), pattern, pos);
    }
    return out;
}

}  // namespace genko::core::pyre
