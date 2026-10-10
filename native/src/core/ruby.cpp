// TEXT-01: a line's ruby held by place — graphemes (Qt's QTextBoundaryFinder), Python's ruby_runs by the explicit rule,
// the spans an op gives and project.json holds, an edit's rules followed (edit_line's text by the longest common
// subsequence, replace_text by its matches), each ruby's own settings and the feature.

#include "core/ruby.hpp"

#include <QChar>
#include <QString>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core::ruby {

namespace {

constexpr std::array<std::string_view, 3> kReasons{"edited", "ambiguous", "unplaced"};

bool reason(std::string_view value) { return std::find(kReasons.begin(), kReasons.end(), value) != kReasons.end(); }

// The grapheme a code-point offset begins (exact: -1 when it falls inside one).
std::int64_t grapheme_at(const std::vector<std::size_t>& b, std::size_t cp) {
    const auto it = std::lower_bound(b.begin(), b.end(), cp);
    return it != b.end() && *it == cp ? static_cast<std::int64_t>(it - b.begin()) : -1;
}

// The grapheme a code-point offset falls in (floor) and the first one wholly after it (ceil).
std::int64_t grapheme_floor(const std::vector<std::size_t>& b, std::size_t cp) {
    const auto it = std::upper_bound(b.begin(), b.end(), cp);
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(it - b.begin()) - 1);
}
std::int64_t grapheme_ceil(const std::vector<std::size_t>& b, std::size_t cp) {
    return static_cast<std::int64_t>(std::lower_bound(b.begin(), b.end(), cp) - b.begin());
}

std::int64_t graphemes_of(const std::vector<std::size_t>& b) { return static_cast<std::int64_t>(b.size()) - 1; }

// The graphemes of a text as ids (the same cluster, the same id) shared by two texts.
struct Clusters {
    std::vector<int> before;
    std::vector<int> after;
};

std::vector<std::u32string> clusters(std::u32string_view text) {
    const std::vector<std::size_t> b = bounds(text);
    std::vector<std::u32string> out;
    for (std::size_t i = 0; i + 1 < b.size(); ++i) out.emplace_back(text.substr(b[i], b[i + 1] - b[i]));
    return out;
}

Clusters ids_of(std::u32string_view before, std::u32string_view after) {
    std::map<std::u32string, int> ids;
    Clusters out;
    for (std::u32string& c : clusters(before)) out.before.push_back(ids.emplace(std::move(c), static_cast<int>(ids.size())).first->second);
    for (std::u32string& c : clusters(after)) out.after.push_back(ids.emplace(std::move(c), static_cast<int>(ids.size())).first->second);
    return out;
}

// The common start and end of two texts (in graphemes): the end never reaching into the start.
struct Window {
    std::int64_t p = 0;
    std::int64_t q = 0;
    std::int64_t n = 0;
    std::int64_t m = 0;
    // where a place of the text before lies in the text after: before the changed part, after it, or (inside it) at its
    // start (for a range's start) or its end (for a range's end)
    std::int64_t start(std::int64_t x) const { return x <= p ? x : x >= n - q ? x + (m - n) : p; }
    std::int64_t end(std::int64_t x) const { return x <= p ? x : x >= n - q ? x + (m - n) : m - q; }
};

Window window_of(const std::vector<int>& a, const std::vector<int>& b) {
    Window w;
    w.n = static_cast<std::int64_t>(a.size());
    w.m = static_cast<std::int64_t>(b.size());
    while (w.p < std::min(w.n, w.m) && a[static_cast<std::size_t>(w.p)] == b[static_cast<std::size_t>(w.p)]) ++w.p;
    while (w.q < std::min(w.n, w.m) - w.p && a[static_cast<std::size_t>(w.n - 1 - w.q)] == b[static_cast<std::size_t>(w.m - 1 - w.q)]) ++w.q;
    return w;
}

// The same, the common end taken first (then the common start, never reaching into it): with the window above, the two
// ends of the places a single edit may have been (a word there more than once around it).
Window window_from_end(const std::vector<int>& a, const std::vector<int>& b) {
    Window w;
    w.n = static_cast<std::int64_t>(a.size());
    w.m = static_cast<std::int64_t>(b.size());
    while (w.q < std::min(w.n, w.m) && a[static_cast<std::size_t>(w.n - 1 - w.q)] == b[static_cast<std::size_t>(w.m - 1 - w.q)]) ++w.q;
    while (w.p < std::min(w.n, w.m) - w.q && a[static_cast<std::size_t>(w.p)] == b[static_cast<std::size_t>(w.p)]) ++w.p;
    return w;
}

// What the single edit of a window does to a range of the text before: keeps it (where it lies after), takes it away,
// or reaches into it.
struct Fate {
    enum class Kind { kept, removed, edited };
    Kind kind = Kind::kept;
    std::int64_t start = 0;
    std::int64_t end = 0;
    bool operator==(const Fate&) const = default;
};

Fate fate_in(const Window& w, std::int64_t s, std::int64_t e) {
    if (e <= w.p) return Fate{Fate::Kind::kept, s, e};
    if (s >= w.n - w.q) return Fate{Fate::Kind::kept, s + (w.m - w.n), e + (w.m - w.n)};
    if (s >= w.p && e <= w.n - w.q && w.m - w.q == w.p) return Fate{Fate::Kind::removed, 0, 0};
    return Fate{Fate::Kind::edited, 0, 0};
}

RubySpan needing_review(const RubySpan& span, std::int64_t start, std::int64_t end, std::string_view why) {
    RubySpan out = span;
    out.start = std::max<std::int64_t>(0, start);
    out.end = std::max(out.start, end);
    if (out.review.empty()) out.review = std::string(why);
    return out;
}

// A follow's spans in the order of their starts, each keeping its origin.
void sort_followed(Followed& f) {
    std::vector<std::size_t> order(f.spans.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&f](std::size_t x, std::size_t y) { return f.spans[x].start < f.spans[y].start; });
    std::vector<RubySpan> spans;
    std::vector<std::size_t> origin;
    for (const std::size_t i : order) {
        spans.push_back(std::move(f.spans[i]));
        origin.push_back(f.origin[i]);
    }
    f.spans = std::move(spans);
    f.origin = std::move(origin);
}

// Longest-common-subsequence tables of two grapheme lists: F[i][j] of a[:i], b[:j]; B[i][j] of a[i:], b[j:].
class Lcs {
public:
    Lcs(const std::vector<int>& a, const std::vector<int>& b) : a_(a), b_(b), n_(a.size()), m_(b.size()) {
        f_.assign((n_ + 1) * (m_ + 1), 0);
        r_.assign((n_ + 1) * (m_ + 1), 0);
        for (std::size_t i = 1; i <= n_; ++i) {
            for (std::size_t j = 1; j <= m_; ++j) {
                f_[at(i, j)] = a_[i - 1] == b_[j - 1] ? f_[at(i - 1, j - 1)] + 1 : std::max(f_[at(i - 1, j)], f_[at(i, j - 1)]);
            }
        }
        for (std::size_t i = n_; i-- > 0;) {
            for (std::size_t j = m_; j-- > 0;) {
                r_[at(i, j)] = a_[i] == b_[j] ? r_[at(i + 1, j + 1)] + 1 : std::max(r_[at(i + 1, j)], r_[at(i, j + 1)]);
            }
        }
        length_ = f_[at(n_, m_)];
    }
    std::uint32_t front(std::size_t i, std::size_t j) const { return f_[at(i, j)]; }
    std::uint32_t back(std::size_t i, std::size_t j) const { return r_[at(i, j)]; }
    std::uint32_t length() const { return length_; }

private:
    std::size_t at(std::size_t i, std::size_t j) const { return i * (m_ + 1) + j; }
    const std::vector<int>& a_;
    const std::vector<int>& b_;
    std::size_t n_;
    std::size_t m_;
    std::vector<std::uint32_t> f_;
    std::vector<std::uint32_t> r_;
    std::uint32_t length_ = 0;
};

[[noreturn]] void bad_spans() { throw OpError("ruby_spans is a list of {start, end, ruby} (the places of the readings: graphemes of the text)"); }

std::int64_t whole(const Json& value) {
    if (!value.is_number_integer()) return -1;
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return -1;
    return value.get<std::int64_t>();
}

}  // namespace

bool unicode_is_fixed() { return QChar::currentUnicodeVersion() == QChar::Unicode_17_0; }

std::u32string code_points(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    const auto bad = [] { return Error("value", "invalid UTF-8 text"); };
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::size_t n = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            n = 1;
            cp = c;
        } else if ((c >> 5) == 6) {
            n = 2;
            cp = c & 0x1f;
        } else if ((c >> 4) == 14) {
            n = 3;
            cp = c & 0x0f;
        } else if ((c >> 3) == 30) {
            n = 4;
            cp = c & 0x07;
        } else {
            throw bad();
        }
        if (i + n > utf8.size()) throw bad();
        for (std::size_t k = 1; k < n; ++k) {
            const auto d = static_cast<unsigned char>(utf8[i + k]);
            if ((d >> 6) != 2) throw bad();
            cp = (cp << 6) | (d & 0x3f);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string to_utf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char32_t cp : text) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

std::vector<std::size_t> bounds(std::u32string_view text) {
    std::vector<std::size_t> out{0};
    if (text.empty()) return out;
    // (QString holds UTF-16: each UTF-16 place's code point)
    const QString q = QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size()));
    std::vector<std::size_t> cp_of(static_cast<std::size_t>(q.size()) + 1, 0);
    std::size_t unit = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        cp_of[unit] = i;
        unit += text[i] > 0xFFFF ? 2 : 1;
    }
    cp_of[unit] = text.size();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, q);
    finder.toStart();
    for (qsizetype at = finder.toNextBoundary(); at != -1; at = finder.toNextBoundary()) {
        const std::size_t cp = cp_of[static_cast<std::size_t>(std::clamp<qsizetype>(at, 0, q.size()))];
        if (cp > out.back()) out.push_back(cp);
    }
    if (out.back() != text.size()) out.push_back(text.size());
    return out;
}

std::int64_t count(std::string_view utf8) { return graphemes_of(bounds(code_points(utf8))); }

std::string slice(std::string_view utf8, std::int64_t start, std::int64_t end) {
    const std::u32string text = code_points(utf8);
    const std::vector<std::size_t> b = bounds(text);
    const std::int64_t n = graphemes_of(b);
    start = std::clamp<std::int64_t>(start, 0, n);
    end = std::clamp<std::int64_t>(end, start, n);
    const std::size_t from = b[static_cast<std::size_t>(start)];
    return to_utf8(std::u32string_view(text).substr(from, b[static_cast<std::size_t>(end)] - from));
}

bool set_setting(RubySpan& span, std::string_view key, const Json& value) {
    const auto number = [&](std::optional<double>& into, double lo, double hi, const char* bounds_text) {
        if (value.is_null()) {
            into.reset();
            return;
        }
        // (a number: not a text or true / false, never clamped)
        const double v = value.is_number() ? value.get<double>() : std::nan("");
        if (!(lo <= v && v <= hi)) throw OpError("a ruby's " + std::string(key) + " must be between " + bounds_text);
        into = v;
    };
    if (key == "font") {
        if (value.is_null()) {
            span.font.reset();
        } else if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            throw OpError("a ruby's font must be a face (gothic, mincho, …) or a font file's path");
        } else {
            span.font = value.get<std::string>();
        }
    } else if (key == "scale") {
        number(span.scale, 0.25, 0.8, "0.25 and 0.8");
    } else if (key == "align") {
        if (value.is_null()) {
            span.align.reset();
        } else if (!is_one_of(value, {"head", "center", "tail"})) {
            throw OpError("a ruby's align must be head, center or tail");
        } else {
            span.align = value.get<std::string>();
        }
    } else if (key == "gap_mm") {
        number(span.gap_mm, -1.0, 10.0, "-1 and 10");
    } else if (key == "tracking") {
        number(span.tracking, -0.5, 2.0, "-0.5 and 2");
    } else {
        return false;
    }
    return true;
}

Json settings_json(const RubySpan& span) {
    Json out = Json::object();
    if (span.font) out["font"] = *span.font;
    if (span.scale) out["scale"] = *span.scale;
    if (span.align) out["align"] = *span.align;
    if (span.gap_mm) out["gap_mm"] = *span.gap_mm;
    if (span.tracking) out["tracking"] = *span.tracking;
    return out;
}

void take_settings(RubySpan& to, const RubySpan& from) {
    to.font = from.font;
    to.scale = from.scale;
    to.align = from.align;
    to.gap_mm = from.gap_mm;
    to.tracking = from.tracking;
}

std::vector<RubySpan> from_runs(std::string_view text_utf8, const std::vector<Json>& runs, bool keep_unplaced) {
    const std::u32string text = code_points(text_utf8);
    const std::vector<std::size_t> b = bounds(text);
    std::vector<RubySpan> out;
    std::size_t pos = 0;
    for (const Json& run : runs) {
        if (!py_truthy(run) || !run.is_array() || run.size() < 2 || !py_truthy(run[0]) || !py_truthy(run[1])) continue;
        const std::string base = py_str(run[0]);
        const std::string reading = py_str(run[1]);
        const std::u32string words = code_points(base);
        const std::size_t at = text.find(words, pos);
        std::int64_t start = -1;
        std::int64_t end = -1;
        if (at != std::u32string::npos) {
            pos = at + words.size();
            start = grapheme_at(b, at);
            end = grapheme_at(b, at + words.size());
        }
        if (start >= 0 && end > start) {
            RubySpan span;
            span.start = start;
            span.end = end;
            span.ruby = reading;
            span.base = base;
            out.push_back(std::move(span));
        } else if (keep_unplaced) {
            RubySpan span;
            span.ruby = reading;
            span.base = base;
            span.review = "unplaced";
            out.push_back(std::move(span));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const RubySpan& x, const RubySpan& y) { return x.start < y.start; });
    return out;
}

std::vector<RubySpan> from_runs_keeping(std::string_view text, const std::vector<Json>& runs, const std::vector<RubySpan>& had,
                                        const std::vector<Json>& mirror) {
    // (the line's own runs sent back: its rubies as they are)
    bool same = mirror.size() == runs.size();
    for (std::size_t i = 0; same && i < runs.size(); ++i) same = py_equals(mirror[i], runs[i]);
    if (same) return had;
    std::vector<RubySpan> out = from_runs(text, runs, true);
    std::vector<bool> used(had.size(), false);
    std::vector<bool> given(out.size(), false);
    // the settings of the ruby with the same base and reading (in order), else of the one at the same place
    for (std::size_t k = 0; k < out.size(); ++k) {
        for (std::size_t i = 0; i < had.size(); ++i) {
            if (used[i] || had[i].base != out[k].base || had[i].ruby != out[k].ruby) continue;
            used[i] = true;
            given[k] = true;
            take_settings(out[k], had[i]);
            break;
        }
    }
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (given[k] || !out[k].review.empty()) continue;
        for (std::size_t i = 0; i < had.size(); ++i) {
            if (used[i] || had[i].start != out[k].start || had[i].end != out[k].end || had[i].base != out[k].base) continue;
            used[i] = true;
            take_settings(out[k], had[i]);
            break;
        }
    }
    return out;
}

std::vector<Json> runs_of(const std::vector<RubySpan>& spans) {
    std::vector<Json> out;
    for (const RubySpan& span : spans) {
        if (span.review.empty()) out.push_back(Json::array({span.base, span.ruby}));
    }
    return out;
}

std::vector<RubySpan> parse(const Json& raw, std::string_view text) {
    if (!raw.is_array()) bad_spans();
    const std::int64_t n = count(text);
    std::vector<RubySpan> out;
    for (const Json& item : raw) {
        if (!item.is_object()) bad_spans();
        RubySpan span;
        for (const auto& [key, value] : item.items()) {
            if (key == "start" || key == "end" || key == "ruby" || key == "base" || key == "review") continue;
            if (!set_setting(span, key, value)) {
                throw OpError("a ruby span has start, end, ruby, base, review, font, scale, align, gap_mm and tracking, not " + key);
            }
        }
        const Json* review = get(item, "review");
        if (review != nullptr && py_truthy(*review)) {
            if (!review->is_string() || !reason(review->get_ref<const std::string&>())) {
                throw OpError("a ruby span's review must be edited, ambiguous or unplaced");
            }
            span.review = review->get<std::string>();
        }
        span.start = item.contains("start") ? whole(item["start"]) : -1;
        span.end = item.contains("end") ? whole(item["end"]) : -1;
        const bool fits = span.review.empty() ? (0 <= span.start && span.start < span.end && span.end <= n)
                                              : (0 <= span.start && span.start <= span.end && span.end <= n);
        if (!fits) {
            throw OpError("a ruby span must be [start, end) of whole numbers with 0 <= start < end <= " + std::to_string(n) +
                          " (the graphemes of the text): " + item.dump(-1, ' ', false));
        }
        const Json* reading = get(item, "ruby");
        if (reading == nullptr || !reading->is_string() || reading->get_ref<const std::string&>().empty()) {
            throw OpError("a ruby span needs its reading (ruby: a text)");
        }
        span.ruby = reading->get<std::string>();
        if (span.ruby.find('\n') != std::string::npos) throw OpError("a ruby span's reading must be one line");
        (void)code_points(span.ruby);
        const std::string there = slice(text, span.start, span.end);
        const Json* base = get(item, "base");
        if (base != nullptr && !base->is_null() && !base->is_string()) throw OpError("a ruby span's base must be a text");
        if (span.review.empty()) {
            if (base != nullptr && base->is_string() && base->get<std::string>() != there) {
                throw OpError("a ruby span's base " + py_repr_str(base->get<std::string>()) + " is not the text at [" +
                              std::to_string(span.start) + ", " + std::to_string(span.end) + "): " + py_repr_str(there));
            }
            span.base = there;
        } else {
            span.base = base != nullptr && base->is_string() && !base->get_ref<const std::string&>().empty() ? base->get<std::string>() : there;
            if (span.base.empty()) throw OpError("a ruby span that needs review keeps its base (a text)");
        }
        out.push_back(std::move(span));
    }
    std::stable_sort(out.begin(), out.end(), [](const RubySpan& x, const RubySpan& y) { return x.start != y.start ? x.start < y.start : x.end < y.end; });
    const RubySpan* last = nullptr;
    for (const RubySpan& span : out) {
        if (!span.review.empty()) continue;
        if (last != nullptr && span.start < last->end) {
            throw OpError("rubies overlap: [" + std::to_string(last->start) + ", " + std::to_string(last->end) + ") and [" +
                          std::to_string(span.start) + ", " + std::to_string(span.end) + ")");
        }
        last = &span;
    }
    return out;
}

Json to_json(const std::vector<RubySpan>& spans) {
    Json out = Json::array();
    for (const RubySpan& span : spans) {
        Json item = Json::object();
        item["start"] = span.start;
        item["end"] = span.end;
        item["base"] = span.base;
        item["ruby"] = span.ruby;
        const Json settings = settings_json(span);  // (named: the loop reads into it)
        for (const auto& [key, value] : settings.items()) item[key] = value;
        if (!span.review.empty()) item["review"] = span.review;
        out.push_back(std::move(item));
    }
    return out;
}

Followed follow_text(std::string_view before_utf8, std::string_view after_utf8, const std::vector<RubySpan>& spans) {
    Followed out;
    if (before_utf8 == after_utf8) {
        out.spans = spans;
        for (std::size_t i = 0; i < spans.size(); ++i) out.origin.push_back(i);
        return out;
    }
    const std::u32string before = code_points(before_utf8);
    const std::u32string after = code_points(after_utf8);
    const std::vector<std::size_t> b1 = bounds(after);
    const Clusters ids = ids_of(before, after);
    const Window w = window_of(ids.before, ids.after);
    const auto n = static_cast<std::size_t>(w.n);
    const auto m = static_cast<std::size_t>(w.m);
    const bool small = static_cast<std::int64_t>(n + 1) * static_cast<std::int64_t>(m + 1) <= kLcsCells;
    std::optional<Lcs> lcs;
    if (small) lcs.emplace(ids.before, ids.after);
    // (a long text: its common start and end, taken both ways round — where the two disagree, the edit could have been
    // anywhere between them)
    const Window w2 = small ? w : window_from_end(ids.before, ids.after);
    // a one-letter-or-more base whose letters are still there, a mark, selector or joiner now joined to its last one: an
    // edit in it, not its removal
    const auto joined = [&](const RubySpan& span, std::int64_t g) {
        if (g < 0 || g >= w.m) return false;
        const std::u32string base = code_points(span.base);
        const std::size_t at = b1[static_cast<std::size_t>(g)];
        return !base.empty() && std::u32string_view(after).substr(at, base.size()) == base && at + base.size() < after.size() &&
               grapheme_at(b1, at + base.size()) < 0;
    };
    // the base's letters also in what either window says changed, in either text: the common start and end alone cannot
    // tell which of those the ruby was on (one edit or several around a word there more than once)
    const auto in_changed = [&](std::int64_t s, std::int64_t e) {
        const auto has = [&](const std::vector<int>& text, std::int64_t from, std::int64_t to) {
            if (to - from < e - s) return false;
            const auto lo = text.begin() + from;
            const auto hi = text.begin() + to;
            return std::search(lo, hi, ids.before.begin() + s, ids.before.begin() + e) != hi;
        };
        for (const Window* x : {&w, &w2}) {
            if (has(ids.before, x->p, x->n - x->q) || has(ids.after, x->p, x->m - x->q)) return true;
        }
        return false;
    };
    const auto keep = [&](const RubySpan& span, std::size_t i) {
        out.spans.push_back(span);
        out.origin.push_back(i);
    };
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const RubySpan& span = spans[i];
        const std::int64_t s = std::clamp<std::int64_t>(span.start, 0, w.n);
        const std::int64_t e = std::clamp<std::int64_t>(span.end, s, w.n);
        if (!span.review.empty() || e <= s) {
            keep(needing_review(span, w.start(s), w.end(e), span.review.empty() ? "edited" : span.review), i);
            continue;
        }
        if (!lcs) {  // (a long text)
            const Fate one = fate_in(w, s, e);
            const Fate two = fate_in(w2, s, e);
            if (!(one == two) || (one.kind == Fate::Kind::kept && in_changed(s, e))) {
                keep(needing_review(span, w.start(s), w.end(e), "ambiguous"), i);
            } else if (one.kind == Fate::Kind::kept) {
                RubySpan moved = span;
                moved.start = one.start;
                moved.end = one.end;
                keep(moved, i);
            } else if (one.kind == Fate::Kind::removed && !joined(span, w.start(s))) {
                ++out.removed;
            } else {
                keep(needing_review(span, w.start(s), w.end(e), "edited"), i);
            }
            continue;
        }
        const auto us = static_cast<std::size_t>(s);
        const auto ue = static_cast<std::size_t>(e);
        const std::size_t k = ue - us;
        const std::uint32_t total = lcs->length();
        std::vector<std::size_t> places;  // where a best alignment may keep the base whole
        for (std::size_t t = 0; t + k <= m; ++t) {
            if (!std::equal(ids.after.begin() + static_cast<std::ptrdiff_t>(t), ids.after.begin() + static_cast<std::ptrdiff_t>(t + k),
                            ids.before.begin() + static_cast<std::ptrdiff_t>(us))) {
                continue;
            }
            if (lcs->front(us, t) + k + lcs->back(ue, t + k) == total) places.push_back(t);
        }
        bool deletable = false;  // a best alignment keeps none of it
        for (std::size_t j = 0; j <= m && !deletable; ++j) deletable = lcs->front(us, j) + lcs->back(ue, j) == total;
        bool kept_any = false;  // a best alignment keeps some of it
        for (std::size_t x = us; x < ue && !kept_any; ++x) {
            for (std::size_t j = 0; j < m && !kept_any; ++j) {
                kept_any = ids.before[x] == ids.after[j] && lcs->front(x, j) + 1 + lcs->back(x + 1, j + 1) == total;
            }
        }
        // every best alignment keeps each of its letters there: none leaves one out, none puts one elsewhere
        const auto firm = [&](std::size_t t) {
            for (std::size_t x = us; x < ue; ++x) {
                const std::size_t there = t + (x - us);
                for (std::size_t j = 0; j <= m; ++j) {
                    if (lcs->front(x, j) + lcs->back(x + 1, j) == total) return false;
                    if (j < m && j != there && ids.before[x] == ids.after[j] && lcs->front(x, j) + 1 + lcs->back(x + 1, j + 1) == total) return false;
                }
            }
            return true;
        };
        if (places.size() == 1 && !deletable && firm(places.front())) {
            RubySpan moved = span;
            moved.start = static_cast<std::int64_t>(places.front());
            moved.end = moved.start + static_cast<std::int64_t>(k);
            keep(moved, i);
        } else if (places.empty() && !kept_any && !joined(span, w.start(s))) {
            ++out.removed;
        } else {
            keep(needing_review(span, w.start(s), w.end(e), places.empty() ? "edited" : "ambiguous"), i);
        }
    }
    sort_followed(out);
    // (each ruby is placed alone: two that different alignments put over the same letters cannot both be right)
    RubySpan* last = nullptr;
    for (RubySpan& span : out.spans) {
        if (!span.review.empty()) continue;
        if (last != nullptr && span.start < last->end) {
            last->review = "ambiguous";
            span.review = "ambiguous";
        }
        last = &span;
    }
    return out;
}

Followed follow_replacements(std::string_view before_utf8, std::string_view after_utf8, const std::vector<Replacement>& replacements,
                             const std::vector<RubySpan>& spans) {
    Followed out;
    const std::u32string before = code_points(before_utf8);
    const std::u32string after = code_points(after_utf8);
    const std::vector<std::size_t> b0 = bounds(before);
    const std::vector<std::size_t> b1 = bounds(after);
    // each match narrowed to what it changes (its common start and end with the replacement taken off): what tells
    // letters added beside a base from an edit of it; the match itself [from, to) is what it took away
    struct Edit {
        std::size_t a = 0;
        std::size_t b = 0;
        std::int64_t delta = 0;
        std::size_t added = 0;
        std::size_t from = 0;
        std::size_t to = 0;
        std::u32string_view with;
    };
    std::vector<Edit> edits;
    for (const Replacement& r : replacements) {
        const std::u32string_view was = std::u32string_view(before).substr(r.start, r.end - r.start);
        const std::u32string_view with = r.with;
        std::size_t p = 0;
        while (p < was.size() && p < with.size() && was[p] == with[p]) ++p;
        std::size_t q = 0;
        while (q < was.size() - p && q < with.size() - p && was[was.size() - 1 - q] == with[with.size() - 1 - q]) ++q;
        const std::size_t a = r.start + p;
        const std::size_t b = r.end - q;
        const std::size_t added = with.size() - p - q;
        if (a == b && added == 0) continue;
        edits.push_back(Edit{a, b, static_cast<std::int64_t>(added) - static_cast<std::int64_t>(b - a), added, r.start, r.end, r.with});
    }
    // where a place of the text before lies after the edits (a range's start inside an edit: the edit's new start; its
    // end: the edit's new end; an insertion at a range's start moves it, one at its end does not)
    const auto start_after = [&](std::size_t x) {
        std::int64_t shift = 0;
        for (const Edit& e : edits) {
            if (e.b <= x) {
                shift += e.delta;
            } else if (e.a < x) {
                return static_cast<std::int64_t>(e.a) + shift;
            } else {
                break;
            }
        }
        return static_cast<std::int64_t>(x) + shift;
    };
    const auto end_after = [&](std::size_t x) {
        std::int64_t shift = 0;
        for (const Edit& e : edits) {
            if (e.b < x || (e.b == x && e.a < e.b)) {
                shift += e.delta;
            } else if (e.a < x) {
                return static_cast<std::int64_t>(e.a) + shift + static_cast<std::int64_t>(e.added);
            } else {
                break;
            }
        }
        return static_cast<std::int64_t>(x) + shift;
    };
    const auto hint = [&](const RubySpan& span, std::size_t s, std::size_t e, std::string_view why) {
        const std::int64_t from = std::clamp<std::int64_t>(start_after(s), 0, static_cast<std::int64_t>(after.size()));
        const std::int64_t to = std::clamp<std::int64_t>(end_after(e), from, static_cast<std::int64_t>(after.size()));
        return needing_review(span, grapheme_floor(b1, static_cast<std::size_t>(from)), grapheme_ceil(b1, static_cast<std::size_t>(to)), why);
    };
    const std::int64_t n = graphemes_of(b0);
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const RubySpan& span = spans[i];
        const std::int64_t g0 = std::clamp<std::int64_t>(span.start, 0, n);
        const std::size_t s = b0[static_cast<std::size_t>(g0)];
        const std::size_t e = b0[static_cast<std::size_t>(std::clamp<std::int64_t>(span.end, g0, n))];
        if (!span.review.empty() || e <= s) {
            out.spans.push_back(hint(span, s, e, span.review.empty() ? "edited" : span.review));
            out.origin.push_back(i);
            continue;
        }
        std::int64_t shift = 0;
        std::vector<const Edit*> reaching;  // the matches whose edit reaches the base
        for (const Edit& edit : edits) {
            if (edit.b <= s) {
                shift += edit.delta;
            } else if (edit.a >= e) {
                break;
            } else {
                reaching.push_back(&edit);
            }
        }
        // the whole base covered by the matches that reach it (they are in order and never overlap): one match whose
        // replacement holds the base once puts it there (the rest of the replacement letters added around it, as
        // Python's run finds it there), twice or more cannot tell where (ambiguous); else it is taken away
        std::size_t covered = s;
        for (const Edit* edit : reaching) {
            if (edit->from <= covered) covered = std::max(covered, edit->to);
        }
        if (!reaching.empty() && covered >= e) {
            const Edit* whole = reaching.size() == 1 ? reaching.front() : nullptr;
            const std::u32string_view base = std::u32string_view(before).substr(s, e - s);
            std::vector<std::size_t> found;
            for (std::size_t at = whole != nullptr ? whole->with.find(base) : std::u32string_view::npos; at != std::u32string_view::npos;
                 at = whole->with.find(base, at + 1)) {
                found.push_back(at);
            }
            if (found.empty()) {
                ++out.removed;
                continue;
            }
            if (found.size() == 1) {
                const auto from = static_cast<std::size_t>(static_cast<std::int64_t>(whole->from) + shift) + found.front();
                const std::int64_t h0 = grapheme_at(b1, from);
                const std::int64_t h1 = grapheme_at(b1, from + base.size());
                if (h0 >= 0 && h1 > h0) {
                    RubySpan moved = span;
                    moved.start = h0;
                    moved.end = h1;
                    out.spans.push_back(moved);
                    out.origin.push_back(i);
                    continue;
                }
            }
            out.spans.push_back(hint(span, s, e, found.size() == 1 ? "edited" : "ambiguous"));
            out.origin.push_back(i);
            continue;
        }
        if (reaching.empty()) {
            const auto from = static_cast<std::size_t>(static_cast<std::int64_t>(s) + shift);
            const auto to = static_cast<std::size_t>(static_cast<std::int64_t>(e) + shift);
            const std::int64_t h0 = grapheme_at(b1, from);
            const std::int64_t h1 = grapheme_at(b1, to);
            // (the base the same letters, still whole graphemes: a combining mark added at its end joins it)
            if (h0 >= 0 && h1 > h0 && std::u32string_view(after).substr(from, to - from) == std::u32string_view(before).substr(s, e - s)) {
                RubySpan moved = span;
                moved.start = h0;
                moved.end = h1;
                out.spans.push_back(moved);
                out.origin.push_back(i);
                continue;
            }
        }
        out.spans.push_back(hint(span, s, e, "edited"));
        out.origin.push_back(i);
    }
    sort_followed(out);
    return out;
}

bool same_drawn(const std::vector<RubySpan>& a, const std::vector<RubySpan>& b) {
    const auto drawn = [](const std::vector<RubySpan>& spans, bool& reviewed) {
        std::vector<RubySpan> out;
        for (const RubySpan& span : spans) {
            if (span.review.empty()) {
                out.push_back(span);
            } else if (span.review != "unplaced") {
                reviewed = true;
            }
        }
        return out;
    };
    bool reviewed = false;
    const std::vector<RubySpan> x = drawn(a, reviewed);
    const std::vector<RubySpan> y = drawn(b, reviewed);
    return !reviewed && x == y;
}

std::int64_t review_count(const StoryLine& line) {
    if (!line.ruby_spans) return 0;
    return static_cast<std::int64_t>(std::count_if(line.ruby_spans->begin(), line.ruby_spans->end(), [](const RubySpan& s) { return !s.review.empty(); }));
}

std::int64_t review_count(const Document& doc) {
    std::int64_t n = 0;
    for (const StoryLine& line : doc.story) n += review_count(line);
    return n;
}

bool uses_feature(const StoryLine& line) { return line.ruby_spans.has_value(); }

void add_feature(Document& doc) {
    const std::string feature(kRubyLayoutFeature);
    if (std::find(doc.features.begin(), doc.features.end(), feature) == doc.features.end()) {
        doc.features.push_back(feature);
        std::sort(doc.features.begin(), doc.features.end());
    }
}

void settle(Document& doc, StoryLine& line) {
    if (line.ruby_spans) {
        std::stable_sort(line.ruby_spans->begin(), line.ruby_spans->end(),
                         [](const RubySpan& x, const RubySpan& y) { return x.start != y.start ? x.start < y.start : x.end < y.end; });
        line.ruby_runs = runs_of(*line.ruby_spans);
        if (line.ruby_spans->empty()) line.ruby_spans.reset();
    }
    if (uses_feature(line)) add_feature(doc);
}

}  // namespace genko::core::ruby
