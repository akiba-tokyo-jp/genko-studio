// Python 3.12's _sre matcher for str patterns (CPython v3.12.3 Modules/_sre/sre_lib.h: SRE(at), SRE(charset),
// SRE(count), SRE(match), SRE(search); PSF License, native/third_party/cpython): the same code walked the same way, its
// backtracking points, marks and repeat contexts kept as sre keeps them (contexts and the data pushed between them on
// stacks, never on the C++ stack), so a pattern matches the same spans with the same groups as in Python. Text
// positions stand for sre's pointers (-1 for NULL). The search tries every position (sre's INFO prefix and charset
// skipping only make it faster). Beyond sre: every step is counted against a Budget (sre's signal check is where the
// count is made), and so is the work sre does between two steps, in proportion to it: the marks saved and given back at
// a place to go back to (as many as the highest mark set), the marks set to nothing below a new one, each choice of a
// branch looked at, each item of a set looked through, each character a repeat walks over, the marks of a match found.
// One Matcher (sre's state) serves the searches of one call over a text: its marks are made once, as sre's are.
// Secret Labs' Regular Expression Engine: Copyright (c) 1997-2001 by Secret Labs AB. All rights reserved. This version
// of the SRE library can be redistributed under CNRI's Python 1.6 license (native/third_party/cpython/LICENSE-SRE).

#include <cstring>

#include "core/pyre_internal.hpp"

namespace genko::core::pyre {

namespace {

constexpr std::int64_t kStepsSpent = -10;  // (SRE_ERROR_INTERRUPTED's place)
// The places to go back to a search may hold (sre's data stack grows until memory runs out: a group repeated a billion
// times at least, say, nests a context for each time): past these, the search stops as if its steps were spent.
constexpr std::size_t kMostContexts = 1'000'000;
constexpr std::size_t kMostData = 4'000'000;

struct Repeat {
    std::int64_t count = -1;
    std::size_t pattern = 0;  // the REPEAT's code (its <skip>)
    std::int64_t last_ptr = -1;
    std::int64_t prev = -1;
};

struct Context {
    std::int64_t count = 0;
    Code chr = 0;
    std::int64_t rep = -1;
    std::int64_t lastmark = -1;
    std::int64_t lastindex = -1;
    std::size_t pattern = 0;
    std::int64_t ptr = 0;
    bool toplevel = false;
    int jump = 0;
    std::int64_t last_ctx_pos = -1;
};

enum Jump {
    JUMP_NONE, JUMP_MAX_UNTIL_1, JUMP_MAX_UNTIL_2, JUMP_MAX_UNTIL_3, JUMP_MIN_UNTIL_1, JUMP_MIN_UNTIL_2, JUMP_MIN_UNTIL_3,
    JUMP_REPEAT, JUMP_REPEAT_ONE_1, JUMP_REPEAT_ONE_2, JUMP_MIN_REPEAT_ONE, JUMP_BRANCH, JUMP_ASSERT, JUMP_ASSERT_NOT,
    JUMP_POSS_REPEAT_1, JUMP_POSS_REPEAT_2, JUMP_ATOMIC_GROUP,
};

bool ascii_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool ascii_space(char32_t c) { return c == U' ' || (c >= U'\t' && c <= U'\r'); }
bool ascii_word(char32_t c) { return c <= U'z' && (ascii_digit(c) || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_'); }
bool is_linebreak(char32_t c) { return c == U'\n'; }   // SRE_IS_LINEBREAK
bool uni_word(char32_t c) { return alnum(c) || c == U'_'; }  // SRE_UNI_IS_WORD
char32_t lower_ascii(char32_t c) { return c < 128 && c >= U'A' && c <= U'Z' ? c + 32 : c; }  // sre_lower_ascii

bool category(Code code, char32_t ch) {
    switch (code) {
        case CATEGORY_DIGIT: return ascii_digit(ch);
        case CATEGORY_NOT_DIGIT: return !ascii_digit(ch);
        case CATEGORY_SPACE: return ascii_space(ch);
        case CATEGORY_NOT_SPACE: return !ascii_space(ch);
        case CATEGORY_WORD: return ascii_word(ch);
        case CATEGORY_NOT_WORD: return !ascii_word(ch);
        case CATEGORY_LINEBREAK: return is_linebreak(ch);
        case CATEGORY_NOT_LINEBREAK: return !is_linebreak(ch);
        case CATEGORY_UNI_DIGIT: return decimal(ch);
        case CATEGORY_UNI_NOT_DIGIT: return !decimal(ch);
        case CATEGORY_UNI_SPACE: return space(ch);
        case CATEGORY_UNI_NOT_SPACE: return !space(ch);
        case CATEGORY_UNI_WORD: return uni_word(ch);
        case CATEGORY_UNI_NOT_WORD: return !uni_word(ch);
        case CATEGORY_UNI_LINEBREAK: return linebreak(ch);
        case CATEGORY_UNI_NOT_LINEBREAK: return !linebreak(ch);
        default: return false;  // (the locale's: never compiled for a str)
    }
}

}  // namespace

class Matcher {
public:
    Matcher(const std::vector<Code>& code, std::int64_t groups, std::u32string_view text, Budget& budget)
        : code_(code), text_(text), end_(static_cast<std::int64_t>(text.size())), budget_(budget) {
        mark_.assign(static_cast<std::size_t>(std::max<std::int64_t>(groups, 0) * 2), -1);
    }

    // SRE(search) from `start` (state_reset first: as pattern_subx and findall call it)
    std::int64_t search(std::int64_t start, bool must_advance) {
        reset();
        start_ = start;
        ptr_ = start;
        must_advance_ = must_advance;
        std::int64_t ptr = start_;
        const std::int64_t end = end_;
        if (ptr > end) return 0;
        // general case
        start_ = ptr_ = ptr;
        std::int64_t status = match(0, true);
        if (budget_.spent) return kStepsSpent;  // (spent between two steps: never a match found past the budget)
        must_advance_ = false;
        if (status == 0 && code_[0] == AT && (code_[1] == AT_BEGINNING || code_[1] == AT_BEGINNING_STRING)) {
            start_ = ptr_ = end;
            return 0;
        }
        while (status == 0 && ptr < end) {
            ++ptr;
            lastmark_ = lastindex_ = -1;  // RESET_CAPTURE_GROUP
            start_ = ptr_ = ptr;
            if (!spend(1)) return kStepsSpent;
            status = match(0, false);
            if (budget_.spent) return kStepsSpent;
        }
        if (status > 0 && !spend(static_cast<std::uint64_t>(lastmark_ + 1))) return kStepsSpent;  // (the marks taken)
        return status;
    }

    std::int64_t start() const { return start_; }
    std::int64_t ptr() const { return ptr_; }
    std::vector<std::int64_t> marks() const {
        return std::vector<std::int64_t>(mark_.begin(), mark_.begin() + static_cast<std::ptrdiff_t>(lastmark_ + 1));
    }

private:
    void reset() {
        lastmark_ = -1;
        lastindex_ = -1;
        repeat_ = -1;
        ctxs_.clear();
        data_.clear();
        repeats_.clear();
        free_repeats_.clear();
    }

    bool spend(std::uint64_t steps) {
        if (budget_.spent || budget_.left < steps) {
            budget_.left = 0;
            budget_.spent = true;
            return false;
        }
        budget_.left -= steps;
        return true;
    }
    // Work counted where its op goes on: once spent, the next step (or the search) stops.
    void charge(std::uint64_t steps) { (void)spend(steps); }

    char32_t ch(std::int64_t at) const { return text_[static_cast<std::size_t>(at)]; }

    // SRE(at)
    bool at(std::int64_t ptr, Code at) const {
        const auto boundary = [&](bool (*word)(char32_t)) {
            const bool thatp = ptr > 0 ? word(ch(ptr - 1)) : false;
            const bool thisp = ptr < end_ ? word(ch(ptr)) : false;
            return std::pair{thatp, thisp};
        };
        switch (at) {
            case AT_BEGINNING:
            case AT_BEGINNING_STRING: return ptr == 0;
            case AT_BEGINNING_LINE: return ptr == 0 || is_linebreak(ch(ptr - 1));
            case AT_END: return (end_ - ptr == 1 && is_linebreak(ch(ptr))) || ptr == end_;
            case AT_END_LINE: return ptr == end_ || is_linebreak(ch(ptr));
            case AT_END_STRING: return ptr == end_;
            case AT_BOUNDARY: {
                if (end_ == 0) return false;
                const auto [a, b] = boundary(ascii_word);
                return a != b;
            }
            case AT_NON_BOUNDARY: {
                if (end_ == 0) return false;
                const auto [a, b] = boundary(ascii_word);
                return a == b;
            }
            case AT_UNI_BOUNDARY: {
                if (end_ == 0) return false;
                const auto [a, b] = boundary(uni_word);
                return a != b;
            }
            case AT_UNI_NON_BOUNDARY: {
                if (end_ == 0) return false;
                const auto [a, b] = boundary(uni_word);
                return a == b;
            }
            default: return false;
        }
    }

    // SRE(charset): whether ch is in the set at code_[set] (each item looked through counted)
    bool charset(std::size_t set, Code chr) {
        std::uint64_t looked = 0;
        const bool in = charset_scan(set, chr, looked);
        charge(looked);
        return in;
    }
    bool charset_scan(std::size_t set, Code chr, std::uint64_t& looked) const {
        bool ok = true;
        for (;;) {
            ++looked;
            switch (code_[set++]) {
                case FAILURE: return !ok;
                case LITERAL:
                    if (chr == code_[set]) return ok;
                    ++set;
                    break;
                case CATEGORY:
                    if (category(code_[set], static_cast<char32_t>(chr))) return ok;
                    ++set;
                    break;
                case CHARSET:
                    if (chr < 256 && (code_[set + chr / 32] & (Code{1} << (chr & 31))) != 0) return ok;
                    set += 256 / 32;
                    break;
                case RANGE:
                    if (code_[set] <= chr && chr <= code_[set + 1]) return ok;
                    set += 2;
                    break;
                case RANGE_UNI_IGNORE: {
                    // ch is already lower cased
                    if (code_[set] <= chr && chr <= code_[set + 1]) return ok;
                    const auto uch = static_cast<Code>(upper(static_cast<char32_t>(chr)));
                    if (code_[set] <= uch && uch <= code_[set + 1]) return ok;
                    set += 2;
                    break;
                }
                case NEGATE: ok = !ok; break;
                case BIGCHARSET: {
                    // <BIGCHARSET> <blockcount> <256 blockindices> <blocks>
                    const Code count = code_[set++];
                    std::int64_t block = -1;
                    if (chr < 0x10000u) {
                        const Code word = code_[set + (chr >> 8) / 4];
                        unsigned char bytes[4];
                        std::memcpy(bytes, &word, 4);
                        block = bytes[(chr >> 8) % 4];
                    }
                    set += 256 / 4;
                    if (block >= 0) {
                        const auto bit = static_cast<std::size_t>(block) * 256 + (chr & 255);
                        if ((code_[set + bit / 32] & (Code{1} << (chr & 31))) != 0) return ok;
                    }
                    set += static_cast<std::size_t>(count) * (256 / 32);
                    break;
                }
                default: return false;  // internal error: pretend it didn't match
            }
        }
    }

    // SRE(count): how many times the single-character item at code_[pattern] matches from ptr_ (at most maxcount)
    std::int64_t count(std::size_t pattern, std::int64_t maxcount) {
        std::int64_t ptr = ptr_;
        std::int64_t end = end_;
        if (maxcount < end - ptr && maxcount != kMaxRepeat) end = ptr + maxcount;
        const auto walk = [&](auto&& keep) -> std::int64_t {
            const std::int64_t from = ptr;
            while (ptr < end && !budget_.spent && keep(ch(ptr))) ++ptr;
            if (!spend(static_cast<std::uint64_t>(ptr - from) + 1)) return kStepsSpent;
            return ptr - ptr_;
        };
        const Code chr = code_[pattern + 1];
        switch (code_[pattern]) {
            case IN: return walk([&](char32_t c) { return charset(pattern + 2, c); });
            case ANY: return walk([](char32_t c) { return !is_linebreak(c); });
            case ANY_ALL:
                ptr = end;
                return ptr - ptr_;
            case LITERAL: return walk([chr](char32_t c) { return c == chr; });
            case LITERAL_IGNORE: return walk([chr](char32_t c) { return lower_ascii(c) == chr; });
            case LITERAL_UNI_IGNORE: return walk([chr](char32_t c) { return lower(c) == chr; });
            case NOT_LITERAL: return walk([chr](char32_t c) { return c != chr; });
            case NOT_LITERAL_IGNORE: return walk([chr](char32_t c) { return lower_ascii(c) != chr; });
            case NOT_LITERAL_UNI_IGNORE: return walk([chr](char32_t c) { return lower(c) != chr; });
            default: {
                // repeated single character pattern
                while (ptr_ < end) {
                    const std::int64_t i = match(pattern, false);
                    if (i < 0) return i;
                    if (i == 0) break;
                }
                return ptr_ - ptr;
            }
        }
    }

    // (the marks copied counted: a pattern of many groups copies them all at every place to go back to)
    void mark_push(std::int64_t lastmark) {
        if (lastmark < 0) return;
        charge(static_cast<std::uint64_t>(lastmark + 1));
        data_.insert(data_.end(), mark_.begin(), mark_.begin() + static_cast<std::ptrdiff_t>(lastmark + 1));
    }
    void mark_pop(std::int64_t lastmark, bool discard) {
        if (lastmark < 0) return;
        const auto n = static_cast<std::size_t>(lastmark + 1);
        charge(n);
        std::copy(data_.end() - static_cast<std::ptrdiff_t>(n), data_.end(), mark_.begin());
        if (discard) data_.resize(data_.size() - n);
    }
    void mark_pop_discard(std::int64_t lastmark) {
        if (lastmark >= 0) data_.resize(data_.size() - static_cast<std::size_t>(lastmark + 1));
    }

    std::int64_t new_repeat() {
        if (!free_repeats_.empty()) {
            const std::int64_t at = free_repeats_.back();
            free_repeats_.pop_back();
            repeats_[static_cast<std::size_t>(at)] = Repeat{};
            return at;
        }
        repeats_.emplace_back();
        return static_cast<std::int64_t>(repeats_.size()) - 1;
    }
    Repeat& rep(std::int64_t at) { return repeats_[static_cast<std::size_t>(at)]; }

    // SRE(match): 1 when the code from `start` matches at ptr_ (ptr_ then its end), 0 when not, <0 for an error.
    std::int64_t match(std::size_t start, bool toplevel);

    const std::vector<Code>& code_;
    std::u32string_view text_;
    std::int64_t end_;
    Budget& budget_;
    std::int64_t start_ = 0;
    std::int64_t ptr_ = 0;
    bool must_advance_ = false;
    std::vector<std::int64_t> mark_;
    std::int64_t lastmark_ = -1;
    std::int64_t lastindex_ = -1;
    std::int64_t repeat_ = -1;
    std::vector<Context> ctxs_;
    std::vector<std::int64_t> data_;
    std::vector<Repeat> repeats_;
    std::vector<std::int64_t> free_repeats_;
};

std::int64_t Matcher::match(std::size_t start, bool toplevel) {
    const std::int64_t end = end_;
    std::int64_t ret = 0;
    std::int64_t ctx_pos = -1;
    std::size_t pattern = start;
    std::int64_t ptr = 0;
    int jump = JUMP_NONE;
    // (the locals of the ops, here so that no jump into an op passes their start)
    std::int64_t groupref = 0;
    std::int64_t repeat_of_tail = -1;

    const auto ctx = [this, &ctx_pos]() -> Context& { return ctxs_[static_cast<std::size_t>(ctx_pos)]; };
    const auto lastmark_save = [&] {
        ctx().lastmark = lastmark_;
        ctx().lastindex = lastindex_;
    };
    const auto lastmark_restore = [&] {
        lastmark_ = ctx().lastmark;
        lastindex_ = ctx().lastindex;
    };
    // DO_JUMPX: a new context for the code at `next`, entered; its result comes back in ret at the label of `value`
    const auto push_context = [&](int value, std::size_t next, bool top) {
        ctx().pattern = pattern;
        ctx().ptr = ptr;
        Context child;
        child.pattern = next;
        child.toplevel = top;
        child.jump = value;
        child.last_ctx_pos = ctx_pos;
        ctxs_.push_back(child);
        ctx_pos = static_cast<std::int64_t>(ctxs_.size()) - 1;
        pattern = next;
    };
    const auto restore_after_jump = [&] {
        pattern = ctx().pattern;
        ptr = ctx().ptr;
    };

#define PYRE_DO_JUMPX(value, label, next, top) \
    push_context(value, next, top);           \
    goto entrance;                            \
    label:                                    \
    restore_after_jump();
#define PYRE_DO_JUMP(value, label, next) PYRE_DO_JUMPX(value, label, next, ctx().toplevel)
#define PYRE_DO_JUMP0(value, label, next) PYRE_DO_JUMPX(value, label, next, false)
#define PYRE_RETURN_FAILURE \
    do {                    \
        ret = 0;            \
        goto exit;          \
    } while (0)
#define PYRE_RETURN_SUCCESS \
    do {                    \
        ret = 1;            \
        goto exit;          \
    } while (0)
#define PYRE_RETURN_ERROR(i) return (i)
#define PYRE_RETURN_ON_ERROR(i) \
    do {                        \
        if ((i) < 0) PYRE_RETURN_ERROR(i); \
    } while (0)

    {
        Context root;
        root.last_ctx_pos = -1;
        root.jump = JUMP_NONE;
        root.toplevel = toplevel;
        ctxs_.push_back(root);
        ctx_pos = static_cast<std::int64_t>(ctxs_.size()) - 1;
    }

entrance:
    ptr = ptr_;

dispatch:
    if (!spend(1) || ctxs_.size() > kMostContexts || data_.size() > kMostData) {
        budget_.spent = true;
        PYRE_RETURN_ERROR(kStepsSpent);
    }
    switch (code_[pattern++]) {
        case MARK: {
            // set mark: <MARK> <gid>
            const auto i = static_cast<std::int64_t>(code_[pattern]);
            if ((i & 1) != 0) lastindex_ = i / 2 + 1;
            if (i > lastmark_) {
                // lastmark is the highest valid index in the mark array: the marks it skips are NULL (counted)
                if (i > lastmark_ + 1) charge(static_cast<std::uint64_t>(i - lastmark_ - 1));
                for (std::int64_t j = lastmark_ + 1; j < i; ++j) mark_[static_cast<std::size_t>(j)] = -1;
                lastmark_ = i;
            }
            mark_[static_cast<std::size_t>(i)] = ptr;
            ++pattern;
            goto dispatch;
        }
        case LITERAL:
            // match literal string: <LITERAL> <code>
            if (ptr >= end || static_cast<Code>(ch(ptr)) != code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case NOT_LITERAL:
            if (ptr >= end || static_cast<Code>(ch(ptr)) == code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case SUCCESS:
            // end of pattern
            if (ctx().toplevel && must_advance_ && ptr == start_) PYRE_RETURN_FAILURE;  // (match_all: never asked)
            ptr_ = ptr;
            PYRE_RETURN_SUCCESS;
        case AT:
            if (!at(ptr, code_[pattern])) PYRE_RETURN_FAILURE;
            ++pattern;
            goto dispatch;
        case CATEGORY:
            if (ptr >= end || !category(code_[pattern], ch(ptr))) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case ANY:
            // match anything (except a newline)
            if (ptr >= end || is_linebreak(ch(ptr))) PYRE_RETURN_FAILURE;
            ++ptr;
            goto dispatch;
        case ANY_ALL:
            if (ptr >= end) PYRE_RETURN_FAILURE;
            ++ptr;
            goto dispatch;
        case IN:
            // match set member (or non_member): <IN> <skip> <set>
            if (ptr >= end || !charset(pattern + 1, ch(ptr))) PYRE_RETURN_FAILURE;
            pattern += code_[pattern];
            ++ptr;
            goto dispatch;
        case LITERAL_IGNORE:
            if (ptr >= end || lower_ascii(ch(ptr)) != code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case LITERAL_UNI_IGNORE:
            if (ptr >= end || lower(ch(ptr)) != code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case NOT_LITERAL_IGNORE:
            if (ptr >= end || lower_ascii(ch(ptr)) == code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case NOT_LITERAL_UNI_IGNORE:
            if (ptr >= end || lower(ch(ptr)) == code_[pattern]) PYRE_RETURN_FAILURE;
            ++pattern;
            ++ptr;
            goto dispatch;
        case IN_IGNORE:
            if (ptr >= end || !charset(pattern + 1, lower_ascii(ch(ptr)))) PYRE_RETURN_FAILURE;
            pattern += code_[pattern];
            ++ptr;
            goto dispatch;
        case IN_UNI_IGNORE:
            if (ptr >= end || !charset(pattern + 1, lower(ch(ptr)))) PYRE_RETURN_FAILURE;
            pattern += code_[pattern];
            ++ptr;
            goto dispatch;
        case JUMP:
        case INFO:
            // jump forward: <JUMP> <offset>
            pattern += code_[pattern];
            goto dispatch;
        case BRANCH:
            // alternation: <BRANCH> <0=skip> code <JUMP> ... <NULL>
            lastmark_save();
            if (repeat_ >= 0) mark_push(ctx().lastmark);
            for (; code_[pattern] != 0; pattern += code_[pattern]) {
                if (!spend(1)) PYRE_RETURN_ERROR(kStepsSpent);  // (each choice looked at)
                if (code_[pattern + 1] == LITERAL && (ptr >= end || static_cast<Code>(ch(ptr)) != code_[pattern + 2])) continue;
                if (code_[pattern + 1] == IN && (ptr >= end || !charset(pattern + 3, ch(ptr)))) continue;
                ptr_ = ptr;
                PYRE_DO_JUMP(JUMP_BRANCH, jump_branch, pattern + 1);
                if (ret != 0) {
                    if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_SUCCESS;
                }
                if (repeat_ >= 0) mark_pop(ctx().lastmark, false);
                lastmark_restore();
            }
            if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
            PYRE_RETURN_FAILURE;
        case REPEAT_ONE:
            // match repeated sequence (maximizing regexp): <REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail
            if (static_cast<std::int64_t>(code_[pattern + 1]) > end - ptr) PYRE_RETURN_FAILURE;  // cannot match
            ptr_ = ptr;
            ret = count(pattern + 3, code_[pattern + 2]);
            PYRE_RETURN_ON_ERROR(ret);
            ctx().count = ret;
            ptr += ctx().count;
            // count holds the number of matches and ptr points to the tail: check if the rest of the pattern matches,
            // and backtrack if not
            if (ctx().count < static_cast<std::int64_t>(code_[pattern + 1])) PYRE_RETURN_FAILURE;
            if (code_[pattern + code_[pattern]] == SUCCESS && ptr == end_ && !(ctx().toplevel && must_advance_ && ptr == start_)) {
                // tail is empty.  we're finished
                ptr_ = ptr;
                PYRE_RETURN_SUCCESS;
            }
            lastmark_save();
            if (repeat_ >= 0) mark_push(ctx().lastmark);
            if (code_[pattern + code_[pattern]] == LITERAL) {
                // tail starts with a literal: skip positions where the rest of the pattern cannot possibly match
                ctx().chr = code_[pattern + code_[pattern] + 1];
                for (;;) {
                    while (ctx().count >= static_cast<std::int64_t>(code_[pattern + 1]) &&
                           (ptr >= end || static_cast<Code>(ch(ptr)) != ctx().chr)) {
                        --ptr;
                        --ctx().count;
                    }
                    if (ctx().count < static_cast<std::int64_t>(code_[pattern + 1])) break;
                    ptr_ = ptr;
                    PYRE_DO_JUMP(JUMP_REPEAT_ONE_1, jump_repeat_one_1, pattern + code_[pattern]);
                    if (ret != 0) {
                        if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
                        PYRE_RETURN_ON_ERROR(ret);
                        PYRE_RETURN_SUCCESS;
                    }
                    if (repeat_ >= 0) mark_pop(ctx().lastmark, false);
                    lastmark_restore();
                    --ptr;
                    --ctx().count;
                }
                if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
            } else {
                // general case
                while (ctx().count >= static_cast<std::int64_t>(code_[pattern + 1])) {
                    ptr_ = ptr;
                    PYRE_DO_JUMP(JUMP_REPEAT_ONE_2, jump_repeat_one_2, pattern + code_[pattern]);
                    if (ret != 0) {
                        if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
                        PYRE_RETURN_ON_ERROR(ret);
                        PYRE_RETURN_SUCCESS;
                    }
                    if (repeat_ >= 0) mark_pop(ctx().lastmark, false);
                    lastmark_restore();
                    --ptr;
                    --ctx().count;
                }
                if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
            }
            PYRE_RETURN_FAILURE;
        case MIN_REPEAT_ONE:
            // match repeated sequence (minimizing regexp): <MIN_REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail
            if (static_cast<std::int64_t>(code_[pattern + 1]) > end - ptr) PYRE_RETURN_FAILURE;  // cannot match
            ptr_ = ptr;
            if (code_[pattern + 1] == 0) {
                ctx().count = 0;
            } else {
                // count using pattern min as the maximum
                ret = count(pattern + 3, code_[pattern + 1]);
                PYRE_RETURN_ON_ERROR(ret);
                if (ret < static_cast<std::int64_t>(code_[pattern + 1])) PYRE_RETURN_FAILURE;  // didn't match minimum number of times
                // advance past minimum matches of repeat
                ctx().count = ret;
                ptr += ctx().count;
            }
            if (code_[pattern + code_[pattern]] == SUCCESS && !(ctx().toplevel && must_advance_ && ptr == start_)) {
                // tail is empty.  we're finished
                ptr_ = ptr;
                PYRE_RETURN_SUCCESS;
            }
            // general case
            lastmark_save();
            if (repeat_ >= 0) mark_push(ctx().lastmark);
            while (code_[pattern + 2] == kMaxRepeat || ctx().count <= static_cast<std::int64_t>(code_[pattern + 2])) {
                ptr_ = ptr;
                PYRE_DO_JUMP(JUMP_MIN_REPEAT_ONE, jump_min_repeat_one, pattern + code_[pattern]);
                if (ret != 0) {
                    if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_SUCCESS;
                }
                if (repeat_ >= 0) mark_pop(ctx().lastmark, false);
                lastmark_restore();
                ptr_ = ptr;
                ret = count(pattern + 3, 1);
                PYRE_RETURN_ON_ERROR(ret);
                if (ret == 0) break;
                ++ptr;
                ++ctx().count;
            }
            if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
            PYRE_RETURN_FAILURE;
        case POSSESSIVE_REPEAT_ONE:
            // match repeated sequence (maximizing regexp) without backtracking:
            // <POSSESSIVE_REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail
            if (ptr + static_cast<std::int64_t>(code_[pattern + 1]) > end) PYRE_RETURN_FAILURE;  // cannot match
            ptr_ = ptr;
            ret = count(pattern + 3, code_[pattern + 2]);
            PYRE_RETURN_ON_ERROR(ret);
            ctx().count = ret;
            ptr += ctx().count;
            // Test for not enough repetitions in match
            if (ctx().count < static_cast<std::int64_t>(code_[pattern + 1])) PYRE_RETURN_FAILURE;
            // Update the pattern to point to the next op code
            pattern += code_[pattern];
            // Let the tail be evaluated separately and consider this match successful
            if (code_[pattern] == SUCCESS && ptr == end_ && !(ctx().toplevel && must_advance_ && ptr == start_)) {
                // tail is empty.  we're finished
                ptr_ = ptr;
                PYRE_RETURN_SUCCESS;
            }
            // Attempt to match the rest of the string
            goto dispatch;
        case REPEAT:
            // create repeat context (the UNTIL operators do the work): <REPEAT> <skip> <1=min> <2=max> item <UNTIL> tail
            ctx().rep = new_repeat();
            rep(ctx().rep).count = -1;
            rep(ctx().rep).pattern = pattern;
            rep(ctx().rep).prev = repeat_;
            rep(ctx().rep).last_ptr = -1;
            repeat_ = ctx().rep;
            ptr_ = ptr;
            PYRE_DO_JUMP(JUMP_REPEAT, jump_repeat, pattern + code_[pattern]);
            repeat_ = rep(ctx().rep).prev;
            free_repeats_.push_back(ctx().rep);
            if (ret != 0) {
                PYRE_RETURN_ON_ERROR(ret);
                PYRE_RETURN_SUCCESS;
            }
            PYRE_RETURN_FAILURE;
        case MAX_UNTIL:
            // maximizing repeat: <REPEAT> <skip> <1=min> <2=max> item <MAX_UNTIL> tail
            ctx().rep = repeat_;
            if (ctx().rep < 0) PYRE_RETURN_ERROR(-2);  // (SRE_ERROR_STATE)
            ptr_ = ptr;
            ctx().count = rep(ctx().rep).count + 1;
            if (ctx().count < static_cast<std::int64_t>(code_[rep(ctx().rep).pattern + 1])) {
                // not enough matches
                rep(ctx().rep).count = ctx().count;
                PYRE_DO_JUMP(JUMP_MAX_UNTIL_1, jump_max_until_1, rep(ctx().rep).pattern + 3);
                if (ret != 0) {
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_SUCCESS;
                }
                rep(ctx().rep).count = ctx().count - 1;
                ptr_ = ptr;
                PYRE_RETURN_FAILURE;
            }
            if ((ctx().count < static_cast<std::int64_t>(code_[rep(ctx().rep).pattern + 2]) ||
                 code_[rep(ctx().rep).pattern + 2] == kMaxRepeat) &&
                ptr_ != rep(ctx().rep).last_ptr) {
                // we may have enough matches, but if we can match another item, do so
                rep(ctx().rep).count = ctx().count;
                lastmark_save();
                mark_push(ctx().lastmark);
                // zero-width match protection
                data_.push_back(rep(ctx().rep).last_ptr);
                rep(ctx().rep).last_ptr = ptr_;
                PYRE_DO_JUMP(JUMP_MAX_UNTIL_2, jump_max_until_2, rep(ctx().rep).pattern + 3);
                rep(ctx().rep).last_ptr = data_.back();
                data_.pop_back();
                if (ret != 0) {
                    mark_pop_discard(ctx().lastmark);
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_SUCCESS;
                }
                mark_pop(ctx().lastmark, true);
                lastmark_restore();
                rep(ctx().rep).count = ctx().count - 1;
                ptr_ = ptr;
            }
            // cannot match more repeated items here.  make sure the tail matches
            repeat_ = rep(ctx().rep).prev;
            PYRE_DO_JUMP(JUMP_MAX_UNTIL_3, jump_max_until_3, pattern);
            repeat_ = ctx().rep;  // restore repeat before return
            PYRE_RETURN_ON_ERROR(ret);
            if (ret > 0) PYRE_RETURN_SUCCESS;
            ptr_ = ptr;
            PYRE_RETURN_FAILURE;
        case MIN_UNTIL:
            // minimizing repeat: <REPEAT> <skip> <1=min> <2=max> item <MIN_UNTIL> tail
            ctx().rep = repeat_;
            if (ctx().rep < 0) PYRE_RETURN_ERROR(-2);  // (SRE_ERROR_STATE)
            ptr_ = ptr;
            ctx().count = rep(ctx().rep).count + 1;
            if (ctx().count < static_cast<std::int64_t>(code_[rep(ctx().rep).pattern + 1])) {
                // not enough matches
                rep(ctx().rep).count = ctx().count;
                PYRE_DO_JUMP(JUMP_MIN_UNTIL_1, jump_min_until_1, rep(ctx().rep).pattern + 3);
                if (ret != 0) {
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_SUCCESS;
                }
                rep(ctx().rep).count = ctx().count - 1;
                ptr_ = ptr;
                PYRE_RETURN_FAILURE;
            }
            // see if the tail matches
            repeat_ = rep(ctx().rep).prev;
            lastmark_save();
            if (repeat_ >= 0) mark_push(ctx().lastmark);
            PYRE_DO_JUMP(JUMP_MIN_UNTIL_2, jump_min_until_2, pattern);
            repeat_of_tail = repeat_;
            repeat_ = ctx().rep;  // restore repeat before return
            if (ret != 0) {
                if (repeat_of_tail >= 0) mark_pop_discard(ctx().lastmark);
                PYRE_RETURN_ON_ERROR(ret);
                PYRE_RETURN_SUCCESS;
            }
            if (repeat_of_tail >= 0) mark_pop(ctx().lastmark, true);
            lastmark_restore();
            ptr_ = ptr;
            if ((ctx().count >= static_cast<std::int64_t>(code_[rep(ctx().rep).pattern + 2]) &&
                 code_[rep(ctx().rep).pattern + 2] != kMaxRepeat) ||
                ptr_ == rep(ctx().rep).last_ptr) {
                PYRE_RETURN_FAILURE;
            }
            rep(ctx().rep).count = ctx().count;
            // zero-width match protection
            data_.push_back(rep(ctx().rep).last_ptr);
            rep(ctx().rep).last_ptr = ptr_;
            PYRE_DO_JUMP(JUMP_MIN_UNTIL_3, jump_min_until_3, rep(ctx().rep).pattern + 3);
            rep(ctx().rep).last_ptr = data_.back();
            data_.pop_back();
            if (ret != 0) {
                PYRE_RETURN_ON_ERROR(ret);
                PYRE_RETURN_SUCCESS;
            }
            rep(ctx().rep).count = ctx().count - 1;
            ptr_ = ptr;
            PYRE_RETURN_FAILURE;
        case POSSESSIVE_REPEAT:
            // create possessive repeat contexts: <POSSESSIVE_REPEAT> <skip> <1=min> <2=max> pattern <SUCCESS> tail
            ptr_ = ptr;
            ctx().count = 0;
            // Check for minimum required matches.
            while (ctx().count < static_cast<std::int64_t>(code_[pattern + 1])) {
                // not enough matches
                PYRE_DO_JUMP0(JUMP_POSS_REPEAT_1, jump_poss_repeat_1, pattern + 3);
                if (ret != 0) {
                    PYRE_RETURN_ON_ERROR(ret);
                    ++ctx().count;
                } else {
                    ptr_ = ptr;
                    PYRE_RETURN_FAILURE;
                }
            }
            // Clear the context's input position so that it does not match the global one and the loop is entered.
            ptr = -1;
            // Keep trying to parse the <pattern> sub-pattern until the end is reached, creating a new context each time.
            while ((ctx().count < static_cast<std::int64_t>(code_[pattern + 2]) || code_[pattern + 2] == kMaxRepeat) && ptr_ != ptr) {
                // Save the capture group markers and the highest one.
                lastmark_save();
                mark_push(ctx().lastmark);
                // zero-width match protection: the last known good position
                ptr = ptr_;
                // We have not reached the maximum matches, so try to match once more.
                PYRE_DO_JUMP0(JUMP_POSS_REPEAT_2, jump_poss_repeat_2, pattern + 3);
                if (ret != 0) {
                    // Drop the saved markers and use the newly updated ones.
                    mark_pop_discard(ctx().lastmark);
                    PYRE_RETURN_ON_ERROR(ret);
                    // Success, increment the count.
                    ++ctx().count;
                } else {
                    // The last attempted match failed: restore the markers and the input position.
                    mark_pop(ctx().lastmark, true);
                    lastmark_restore();
                    ptr_ = ptr;
                    // We have sufficient matches, so exit loop.
                    break;
                }
            }
            // Evaluate Tail: jump to the end of the pattern indicated by skip, and skip the SUCCESS after it.
            pattern += code_[pattern] + 1;
            ptr = ptr_;
            goto dispatch;
        case ATOMIC_GROUP:
            // Atomic Group Sub Pattern: <ATOMIC_GROUP> <skip> pattern <SUCCESS> tail
            ptr_ = ptr;
            // Evaluate the Atomic Group in a new context, terminating at the SUCCESS at its end.
            PYRE_DO_JUMP0(JUMP_ATOMIC_GROUP, jump_atomic_group, pattern + 1);
            PYRE_RETURN_ON_ERROR(ret);
            if (ret == 0) {
                // Atomic Group failed to Match.
                ptr_ = ptr;
                PYRE_RETURN_FAILURE;
            }
            // Evaluate Tail: jump to the end of the pattern indicated by skip, and skip the SUCCESS op code after it.
            pattern += code_[pattern];
            ptr = ptr_;
            goto dispatch;
        case GROUPREF:
        case GROUPREF_IGNORE:
        case GROUPREF_UNI_IGNORE: {
            // match backreference
            const Code op = code_[pattern - 1];
            groupref = static_cast<std::int64_t>(code_[pattern]) * 2;
            if (groupref >= lastmark_) PYRE_RETURN_FAILURE;
            std::int64_t p = mark_[static_cast<std::size_t>(groupref)];
            const std::int64_t e = mark_[static_cast<std::size_t>(groupref + 1)];
            if (p < 0 || e < 0 || e < p) PYRE_RETURN_FAILURE;
            if (!spend(static_cast<std::uint64_t>(e - p))) PYRE_RETURN_ERROR(kStepsSpent);
            while (p < e) {
                if (ptr >= end) PYRE_RETURN_FAILURE;
                const char32_t a = ch(ptr);
                const char32_t b = ch(p);
                const bool same = op == GROUPREF ? a == b : op == GROUPREF_IGNORE ? lower_ascii(a) == lower_ascii(b) : lower(a) == lower(b);
                if (!same) PYRE_RETURN_FAILURE;
                ++p;
                ++ptr;
            }
            ++pattern;
            goto dispatch;
        }
        case GROUPREF_EXISTS:
            // <GROUPREF_EXISTS> <group> <skip> codeyes <JUMP> codeno ...
            groupref = static_cast<std::int64_t>(code_[pattern]) * 2;
            if (groupref >= lastmark_) {
                pattern += code_[pattern + 1];
                goto dispatch;
            }
            if (mark_[static_cast<std::size_t>(groupref)] < 0 || mark_[static_cast<std::size_t>(groupref + 1)] < 0 ||
                mark_[static_cast<std::size_t>(groupref + 1)] < mark_[static_cast<std::size_t>(groupref)]) {
                pattern += code_[pattern + 1];
                goto dispatch;
            }
            pattern += 2;
            goto dispatch;
        case ASSERT:
            // assert subpattern: <ASSERT> <skip> <back> <pattern>
            if (ptr < static_cast<std::int64_t>(code_[pattern + 1])) PYRE_RETURN_FAILURE;
            ptr_ = ptr - static_cast<std::int64_t>(code_[pattern + 1]);
            PYRE_DO_JUMP0(JUMP_ASSERT, jump_assert, pattern + 2);
            PYRE_RETURN_ON_ERROR(ret);
            if (ret == 0) PYRE_RETURN_FAILURE;
            pattern += code_[pattern];
            goto dispatch;
        case ASSERT_NOT:
            // assert not subpattern: <ASSERT_NOT> <skip> <back> <pattern>
            if (ptr >= static_cast<std::int64_t>(code_[pattern + 1])) {
                ptr_ = ptr - static_cast<std::int64_t>(code_[pattern + 1]);
                lastmark_save();
                if (repeat_ >= 0) mark_push(ctx().lastmark);
                PYRE_DO_JUMP0(JUMP_ASSERT_NOT, jump_assert_not, pattern + 2);
                if (ret != 0) {
                    if (repeat_ >= 0) mark_pop_discard(ctx().lastmark);
                    PYRE_RETURN_ON_ERROR(ret);
                    PYRE_RETURN_FAILURE;
                }
                if (repeat_ >= 0) mark_pop(ctx().lastmark, true);
                lastmark_restore();
            }
            pattern += code_[pattern];
            goto dispatch;
        case FAILURE:
            // immediate failure
            PYRE_RETURN_FAILURE;
        default:
            PYRE_RETURN_ERROR(-1);  // (SRE_ERROR_ILLEGAL: never compiled)
    }

exit:
    ctx_pos = ctx().last_ctx_pos;
    jump = ctxs_.back().jump;
    ctxs_.pop_back();
    if (ctx_pos == -1) return ret;
    switch (jump) {
        case JUMP_MAX_UNTIL_2: goto jump_max_until_2;
        case JUMP_MAX_UNTIL_3: goto jump_max_until_3;
        case JUMP_MIN_UNTIL_2: goto jump_min_until_2;
        case JUMP_MIN_UNTIL_3: goto jump_min_until_3;
        case JUMP_BRANCH: goto jump_branch;
        case JUMP_MAX_UNTIL_1: goto jump_max_until_1;
        case JUMP_MIN_UNTIL_1: goto jump_min_until_1;
        case JUMP_POSS_REPEAT_1: goto jump_poss_repeat_1;
        case JUMP_POSS_REPEAT_2: goto jump_poss_repeat_2;
        case JUMP_REPEAT: goto jump_repeat;
        case JUMP_REPEAT_ONE_1: goto jump_repeat_one_1;
        case JUMP_REPEAT_ONE_2: goto jump_repeat_one_2;
        case JUMP_MIN_REPEAT_ONE: goto jump_min_repeat_one;
        case JUMP_ATOMIC_GROUP: goto jump_atomic_group;
        case JUMP_ASSERT: goto jump_assert;
        case JUMP_ASSERT_NOT: goto jump_assert_not;
        default: break;
    }
    return ret;  // should never get here

#undef PYRE_DO_JUMPX
#undef PYRE_DO_JUMP
#undef PYRE_DO_JUMP0
#undef PYRE_RETURN_FAILURE
#undef PYRE_RETURN_SUCCESS
#undef PYRE_RETURN_ERROR
#undef PYRE_RETURN_ON_ERROR
}

Searcher::Searcher(const std::vector<Code>& code, std::int64_t groups, std::u32string_view text, Budget& budget)
    : matcher_(std::make_unique<Matcher>(code, groups, text, budget)), budget_(budget) {}

Searcher::~Searcher() = default;

Searched Searcher::search(std::int64_t start, bool must_advance) {
    const std::int64_t status = matcher_->search(start, must_advance);
    Searched out;
    if (status <= 0) {
        if (status < 0 && !budget_.spent) budget_.spent = true;  // (an internal error: refused as too much, never a match)
        return out;
    }
    out.found = true;
    out.start = matcher_->start();
    out.end = matcher_->ptr();
    out.marks = matcher_->marks();
    return out;
}

}  // namespace genko::core::pyre
