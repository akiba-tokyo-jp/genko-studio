// Python 3.12's re._compiler for str patterns (CPython v3.12.3 Lib/re/_compiler.py; PSF License,
// native/third_party/cpython): the parsed pattern as _sre's code, set for set and jump for jump as Python makes it (the
// sets' bitmaps and the case-insensitive codes decide which characters match, quirks and all). Not made: the INFO block
// before the code, which only lets the search skip ahead (pyre_match.cpp searches every position instead, with the same
// result).
// Secret Labs' Regular Expression Engine: Copyright (c) 1997-2001 by Secret Labs AB. All rights reserved. This version
// of the SRE library can be redistributed under CNRI's Python 1.6 license (native/third_party/cpython/LICENSE-SRE).

#include <algorithm>
#include <cstring>

#include "core/error.hpp"
#include "core/pyre_internal.hpp"

namespace genko::core::pyre {

namespace {

constexpr int kTypeFlags = FLAG_ASCII | FLAG_LOCALE | FLAG_UNICODE;

int combine_flags(int flags, int add_flags, int del_flags) {
    if ((add_flags & kTypeFlags) != 0) flags &= ~kTypeFlags;
    return (flags | add_flags) & ~del_flags;
}

// _sre.ascii_iscased / ascii_tolower
bool ascii_cased(Code c) { return c < 128 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); }
Code ascii_lower(Code c) { return c < 128 && c >= 'A' && c <= 'Z' ? c + 32 : c; }

// The case functions of a compile: none, ASCII's, or Unicode's with _casefix's extra cases.
struct Cases {
    bool on = false;
    bool unicode = false;
    bool iscased(Code c) const { return unicode ? cased(static_cast<char32_t>(c)) : ascii_cased(c); }
    Code tolower(Code c) const { return unicode ? static_cast<Code>(lower(static_cast<char32_t>(c))) : ascii_lower(c); }
    const std::array<char32_t, 2>* fixes(Code lo) const { return unicode ? extra_cases(static_cast<char32_t>(lo)) : nullptr; }
};

Cases cases_of(int flags) {
    Cases out;
    if ((flags & FLAG_IGNORECASE) != 0 && (flags & FLAG_LOCALE) == 0) {
        out.on = true;
        out.unicode = (flags & FLAG_UNICODE) != 0;
    }
    return out;
}

// CH_UNICODE (the categories in Unicode mode)
Code unicode_category(Code category) {
    switch (category) {
        case CATEGORY_DIGIT: return CATEGORY_UNI_DIGIT;
        case CATEGORY_NOT_DIGIT: return CATEGORY_UNI_NOT_DIGIT;
        case CATEGORY_SPACE: return CATEGORY_UNI_SPACE;
        case CATEGORY_NOT_SPACE: return CATEGORY_UNI_NOT_SPACE;
        case CATEGORY_WORD: return CATEGORY_UNI_WORD;
        case CATEGORY_NOT_WORD: return CATEGORY_UNI_NOT_WORD;
        case CATEGORY_LINEBREAK: return CATEGORY_UNI_LINEBREAK;
        case CATEGORY_NOT_LINEBREAK: return CATEGORY_UNI_NOT_LINEBREAK;
        default: return category;
    }
}

// _mk_bitmap: a map of 0/1 bytes as words of 32 bits, the lowest bit first
std::vector<Code> mk_bitmap(const std::vector<std::uint8_t>& bits) {
    std::vector<Code> out(bits.size() / 32, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] != 0) out[i / 32] |= Code{1} << (i % 32);
    }
    return out;
}

// _optimize_charset: the set as LITERAL/RANGE runs, a CHARSET bitmap or a BIGCHARSET, and whether a member is cased
std::pair<std::vector<SetItem>, bool> optimize_charset(const std::vector<SetItem>& charset, const Cases& cases) {
    std::vector<SetItem> out;
    std::vector<SetItem> tail;
    std::vector<std::uint8_t> charmap(256, 0);
    bool hascased = false;
    for (const SetItem& member : charset) {
        SetItem item = member;
        for (;;) {
            // (Python's IndexError: a character past the map; the map grows to 65536 once, then the member goes to the tail)
            bool overflow = false;
            const auto mark = [&](Code c) {
                if (c >= charmap.size()) {
                    overflow = true;
                    return false;
                }
                charmap[c] = 1;
                return true;
            };
            if (item.op == LITERAL) {
                if (cases.on) {
                    const Code lo = cases.tolower(item.a);
                    if (mark(lo)) {
                        if (const auto* extra = cases.fixes(lo)) {
                            for (const char32_t k : *extra) {
                                if (k != 0 && !mark(static_cast<Code>(k))) break;
                            }
                        }
                        if (!overflow && !hascased && cases.iscased(item.a)) hascased = true;
                    }
                } else {
                    mark(item.a);
                }
            } else if (item.op == RANGE) {
                if (cases.on) {
                    for (Code c = item.a;; ++c) {
                        const Code i = cases.tolower(c);
                        if (!mark(i)) break;
                        if (const auto* extra = cases.fixes(i)) {
                            for (const char32_t k : *extra) {
                                if (k != 0 && !mark(static_cast<Code>(k))) break;
                            }
                            if (overflow) break;
                        }
                        if (c == item.b) break;
                    }
                    if (!overflow && !hascased) {
                        for (Code c = item.a;; ++c) {
                            if (cases.iscased(c)) {
                                hascased = true;
                                break;
                            }
                            if (c == item.b) break;
                        }
                    }
                } else {
                    for (Code c = item.a;; ++c) {
                        if (!mark(c)) break;
                        if (c == item.b) break;
                    }
                }
            } else if (item.op == NEGATE) {
                out.push_back(item);
            } else {
                tail.push_back(item);
            }
            if (overflow) {
                if (charmap.size() == 256) {
                    // character set contains non-UCS1 character codes
                    charmap.resize(65536, 0);
                    continue;
                }
                // Character set contains non-BMP character codes. For range, all BMP characters in the range are
                // already proceeded.
                if (cases.on) {
                    hascased = true;
                    if (item.op == RANGE) item.op = RANGE_UNI_IGNORE;
                }
                tail.push_back(item);
            }
            break;
        }
    }
    // compress character map
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    bool use_runs = true;
    std::size_t q = 0;
    for (;;) {
        const auto p = std::find(charmap.begin() + static_cast<std::ptrdiff_t>(q), charmap.end(), 1);
        if (p == charmap.end()) break;
        if (runs.size() >= 2) {
            use_runs = false;
            break;
        }
        const auto start = static_cast<std::size_t>(p - charmap.begin());
        const auto z = std::find(p, charmap.end(), 0);
        if (z == charmap.end()) {
            runs.emplace_back(start, charmap.size());
            break;
        }
        q = static_cast<std::size_t>(z - charmap.begin());
        runs.emplace_back(start, q);
    }
    if (use_runs) {
        // use literal/range
        for (const auto& [p, end] : runs) {
            if (end - p == 1) out.push_back(SetItem{LITERAL, static_cast<Code>(p), 0, {}});
            else out.push_back(SetItem{RANGE, static_cast<Code>(p), static_cast<Code>(end - 1), {}});
        }
        out.insert(out.end(), tail.begin(), tail.end());
        // if the case was changed or new representation is more compact
        if (hascased || out.size() < charset.size()) return {out, hascased};
        // else original character set is good enough
        return {charset, hascased};
    }
    // use bitmap
    if (charmap.size() == 256) {
        out.push_back(SetItem{CHARSET, 0, 0, mk_bitmap(charmap)});
        out.insert(out.end(), tail.begin(), tail.end());
        return {out, hascased};
    }
    // a big charset: the map in chunks of 256, each different chunk once, numbered in order
    std::vector<std::vector<std::uint8_t>> comps;
    std::vector<std::uint8_t> mapping(256, 0);
    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i < 65536; i += 256) {
        const std::vector<std::uint8_t> chunk(charmap.begin() + static_cast<std::ptrdiff_t>(i), charmap.begin() + static_cast<std::ptrdiff_t>(i + 256));
        const auto found = std::find(comps.begin(), comps.end(), chunk);
        if (found != comps.end()) {
            mapping[i / 256] = static_cast<std::uint8_t>(found - comps.begin());
        } else {
            mapping[i / 256] = static_cast<std::uint8_t>(comps.size());
            comps.push_back(chunk);
            data.insert(data.end(), chunk.begin(), chunk.end());
        }
    }
    std::vector<Code> words;
    words.push_back(static_cast<Code>(comps.size()));
    // _bytes_to_codes(mapping): the 256 chunk numbers as 64 words in the machine's order (each word's first byte is the
    // lowest chunk number on a little-endian machine; the matcher reads them back as bytes, so the order is moot here)
    for (std::size_t i = 0; i < 256; i += 4) {
        Code word = 0;
        std::memcpy(&word, mapping.data() + i, 4);
        words.push_back(word);
    }
    const std::vector<Code> bitmap = mk_bitmap(data);
    words.insert(words.end(), bitmap.begin(), bitmap.end());
    out.push_back(SetItem{BIGCHARSET, 0, 0, std::move(words)});
    out.insert(out.end(), tail.begin(), tail.end());
    return {out, hascased};
}

void compile_charset(const std::vector<SetItem>& charset, int flags, std::vector<Code>& code) {
    for (const SetItem& item : charset) {
        code.push_back(item.op);
        switch (item.op) {
            case NEGATE: break;
            case LITERAL: code.push_back(item.a); break;
            case RANGE:
            case RANGE_UNI_IGNORE:
                code.push_back(item.a);
                code.push_back(item.b);
                break;
            case CHARSET:
            case BIGCHARSET: code.insert(code.end(), item.words.begin(), item.words.end()); break;
            case CATEGORY: code.push_back((flags & FLAG_UNICODE) != 0 ? unicode_category(item.a) : item.a); break;
            default: re_error("internal: unsupported set operator");
        }
    }
    code.push_back(FAILURE);
}

// _simple: a subpattern of one character's width at most, without groups
bool simple(const SubPattern& p) {
    if (p.data.size() != 1) return false;
    const Item& item = p.data[0];
    if (item.op == SUBPATTERN) return item.group < 0 && simple(*item.sub);
    return item.op == LITERAL || item.op == NOT_LITERAL || item.op == ANY || item.op == IN;
}

std::string op_name(Code op) {
    switch (op) {
        case MIN_REPEAT: return "MIN_REPEAT";
        case MAX_REPEAT: return "MAX_REPEAT";
        default: return "POSSESSIVE_REPEAT";
    }
}

void compile(std::vector<Code>& code, const std::vector<Item>& pattern, int flags) {
    const Cases cases = cases_of(flags);
    const auto skip_at = [&code] {
        code.push_back(0);
        return code.size() - 1;
    };
    const auto fix = [&code](std::size_t skip, std::size_t extra = 0) { code[skip] = static_cast<Code>(code.size() - skip + extra); };
    for (const Item& item : pattern) {
        const Code op = item.op;
        if (op == LITERAL || op == NOT_LITERAL) {
            if ((flags & FLAG_IGNORECASE) == 0 || !cases.iscased(item.value)) {
                code.push_back(op);
                code.push_back(item.value);
            } else {
                const Code lo = cases.tolower(item.value);
                const auto* extra = cases.fixes(lo);
                if (!cases.unicode) {  // ascii
                    code.push_back(op == LITERAL ? LITERAL_IGNORE : NOT_LITERAL_IGNORE);
                    code.push_back(lo);
                } else if (extra == nullptr) {
                    code.push_back(op == LITERAL ? LITERAL_UNI_IGNORE : NOT_LITERAL_UNI_IGNORE);
                    code.push_back(lo);
                } else {
                    code.push_back(IN_UNI_IGNORE);
                    const std::size_t skip = skip_at();
                    if (op == NOT_LITERAL) code.push_back(NEGATE);
                    code.push_back(LITERAL);
                    code.push_back(lo);
                    for (const char32_t k : *extra) {
                        if (k == 0) continue;
                        code.push_back(LITERAL);
                        code.push_back(static_cast<Code>(k));
                    }
                    code.push_back(FAILURE);
                    fix(skip);
                }
            }
        } else if (op == IN) {
            const auto [charset, hascased] = optimize_charset(item.set, cases);
            if (!hascased) code.push_back(IN);
            else if (!cases.unicode) code.push_back(IN_IGNORE);  // ascii
            else code.push_back(IN_UNI_IGNORE);
            const std::size_t skip = skip_at();
            compile_charset(charset, flags, code);
            fix(skip);
        } else if (op == ANY) {
            code.push_back((flags & FLAG_DOTALL) != 0 ? ANY_ALL : ANY);
        } else if (op == MIN_REPEAT || op == MAX_REPEAT || op == POSSESSIVE_REPEAT) {
            if ((flags & FLAG_TEMPLATE) != 0) re_error("internal: unsupported template operator " + op_name(op));
            if (simple(*item.sub)) {
                code.push_back(op == MIN_REPEAT ? MIN_REPEAT_ONE : op == MAX_REPEAT ? REPEAT_ONE : POSSESSIVE_REPEAT_ONE);
                const std::size_t skip = skip_at();
                code.push_back(item.min);
                code.push_back(item.max);
                compile(code, item.sub->data, flags);
                code.push_back(SUCCESS);
                fix(skip);
            } else {
                code.push_back(op == POSSESSIVE_REPEAT ? POSSESSIVE_REPEAT : REPEAT);
                const std::size_t skip = skip_at();
                code.push_back(item.min);
                code.push_back(item.max);
                compile(code, item.sub->data, flags);
                fix(skip);
                code.push_back(op == MIN_REPEAT ? MIN_UNTIL : op == MAX_REPEAT ? MAX_UNTIL : SUCCESS);
            }
        } else if (op == SUBPATTERN) {
            if (item.group > 0) {
                code.push_back(MARK);
                code.push_back(static_cast<Code>((item.group - 1) * 2));
            }
            compile(code, item.sub->data, combine_flags(flags, item.add_flags, item.del_flags));
            if (item.group > 0) {
                code.push_back(MARK);
                code.push_back(static_cast<Code>((item.group - 1) * 2 + 1));
            }
        } else if (op == ATOMIC_GROUP) {
            code.push_back(ATOMIC_GROUP);
            const std::size_t skip = skip_at();
            compile(code, item.sub->data, flags);
            code.push_back(SUCCESS);
            fix(skip);
        } else if (op == SUCCESS || op == FAILURE) {
            code.push_back(op);
        } else if (op == ASSERT || op == ASSERT_NOT) {
            code.push_back(op);
            const std::size_t skip = skip_at();
            if (item.dir >= 0) {
                code.push_back(0);  // look ahead
            } else {
                const auto [lo, hi] = item.sub->getwidth();
                if (lo > kMaxCode) re_error("looks too much behind");
                if (lo != hi) re_error("look-behind requires fixed-width pattern");
                code.push_back(static_cast<Code>(lo));  // look behind
            }
            compile(code, item.sub->data, flags);
            code.push_back(SUCCESS);
            fix(skip);
        } else if (op == AT) {
            code.push_back(op);
            Code av = item.value;
            if ((flags & FLAG_MULTILINE) != 0) {
                if (av == AT_BEGINNING) av = AT_BEGINNING_LINE;
                else if (av == AT_END) av = AT_END_LINE;
            }
            if ((flags & FLAG_UNICODE) != 0) {
                if (av == AT_BOUNDARY) av = AT_UNI_BOUNDARY;
                else if (av == AT_NON_BOUNDARY) av = AT_UNI_NON_BOUNDARY;
            }
            code.push_back(av);
        } else if (op == BRANCH) {
            code.push_back(op);
            std::vector<std::size_t> tails;
            for (const Sub& branch : item.branches) {
                const std::size_t skip = skip_at();
                compile(code, branch->data, flags);
                code.push_back(JUMP);
                tails.push_back(skip_at());
                fix(skip);
            }
            code.push_back(FAILURE);  // end of branch
            for (const std::size_t tail : tails) fix(tail);
        } else if (op == CATEGORY) {
            code.push_back(op);
            code.push_back((flags & FLAG_UNICODE) != 0 ? unicode_category(item.value) : item.value);
        } else if (op == GROUPREF) {
            if ((flags & FLAG_IGNORECASE) == 0) code.push_back(op);
            else if (!cases.unicode) code.push_back(GROUPREF_IGNORE);  // ascii
            else code.push_back(GROUPREF_UNI_IGNORE);
            code.push_back(item.value - 1);
        } else if (op == GROUPREF_EXISTS) {
            code.push_back(op);
            code.push_back(static_cast<Code>(item.group - 1));
            const std::size_t skipyes = skip_at();
            compile(code, item.sub->data, flags);
            if (item.no && !item.no->data.empty()) {  // (`if av[2]`: an empty "no" is none)
                code.push_back(JUMP);
                const std::size_t skipno = skip_at();
                fix(skipyes, 1);
                compile(code, item.no->data, flags);
                fix(skipno);
            } else {
                fix(skipyes, 1);
            }
        } else {
            re_error("internal: unsupported operand type");
        }
    }
}

}  // namespace

std::vector<Code> compile(const Parsed& parsed, int flags) {
    const int all = parsed.state->flags | flags;
    std::vector<Code> code;
    compile(code, parsed.pattern->data, all);
    code.push_back(SUCCESS);
    return code;
}

}  // namespace genko::core::pyre
