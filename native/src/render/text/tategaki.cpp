// genko/tategaki.py: cells, columns_of (kinsoku), glyph / tcy_glyph / latin_glyph, ruby spans, 傍点, style runs and
// compose; phrases, phrase_columns and without_periods.

#include "render/text/tategaki.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/draw.hpp"

namespace genko::render::text {

using core::Json;

namespace {

// SMALL_KANA, PUNCT_TR, ROTATE_CW, LINE_START_KINSOKU, LINE_END_KINSOKU, TCY_HALF, LATIN_JOINERS
constexpr std::u32string_view kSmallKana =
    U"ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャ"
    U"ュョヮヵヶ";
constexpr std::u32string_view kPunctTr = U"、。，．";
constexpr std::u32string_view kRotateCw =
    U":;=‐–—―‥…−─━〜〰ー︱：；＝～";
constexpr std::u32string_view kLineStartKinsoku =
    U")、。〉》」』】ぁぃぅぇぉっゃゅょゎァ"
    U"ィゥェォッャュョヮー），．］";
constexpr std::u32string_view kLineEndKinsoku = U"(〈《「『【〔（［｛";
constexpr std::u32string_view kTcyHalf = U"!0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::u32string_view kLatinJoiners = U" &'+,-./:";
constexpr std::size_t kLatinRun = 4;  // LATIN_RUN

// VERTICAL_FORMS
constexpr std::array<std::pair<char32_t, char32_t>, 16> kVerticalForms{{{U'「', U'﹁'},
                                                                        {U'」', U'﹂'},
                                                                        {U'『', U'﹃'},
                                                                        {U'』', U'﹄'},
                                                                        {U'（', U'︵'},
                                                                        {U'）', U'︶'},
                                                                        {U'(', U'︵'},
                                                                        {U')', U'︶'},
                                                                        {U'【', U'︻'},
                                                                        {U'】', U'︼'},
                                                                        {U'［', U'﹇'},
                                                                        {U'］', U'﹈'},
                                                                        {U'〈', U'︿'},
                                                                        {U'〉', U'﹀'},
                                                                        {U'《', U'︽'},
                                                                        {U'》', U'︾'}}};

// the phrases (_OPENING, _CLOSING, _BREAKS_AFTER, _KANA_WORDS)
constexpr std::u32string_view kOpening = U"([‘“〈《「『【〔（［";
constexpr std::u32string_view kClosing =
    U"!),.?]’”‥…、。〉》」』】〕〜ぁぃぅぇぉ"
    U"っゃゅょァィゥェォッャュョ・ー！），．"
    U"？］～";
constexpr std::u32string_view kBreaksAfter = U"!),?‥…、。」』！），．？";
constexpr std::array<std::u32string_view, 27> kKanaWords{
    U"この", U"その", U"あの", U"どの", U"これ", U"それ",
    U"あれ", U"どれ", U"ここ", U"そこ", U"とても", U"もう",
    U"まだ", U"すごく", U"ちょっと", U"なんか", U"ずっと",
    U"やっぱり", U"きっと", U"たぶん", U"ぜんぶ",
    U"みんな", U"ありがとう", U"ございます",
    U"ください", U"ごめん", U"よろしく"};

bool in(std::u32string_view set, char32_t c) { return set.find(c) != std::u32string_view::npos; }
// `cell in SET` for a frozenset of characters: a cell of one character that is in it
bool member(std::u32string_view set, std::u32string_view cell) { return cell.size() == 1 && in(set, cell[0]); }

bool latin_letter(char32_t c) { return in(kTcyHalf, c) && c != U'!' && c != U'?'; }  // LATIN_LETTERS
bool full_digit(char32_t c) { return c >= 0xFF10 && c <= 0xFF19; }                   // FULL_DIGITS
bool bang(char32_t c) { return c == U'！' || c == U'？' || c == U'!' || c == U'?'; }
char32_t bang_of(char32_t c) { return c == U'！' ? U'!' : c == U'？' ? U'?' : c; }

std::optional<char32_t> vertical_form(std::u32string_view cell) {
    if (cell.size() != 1) return std::nullopt;
    for (const auto& [from, to] : kVerticalForms) {
        if (from == cell[0]) return to;
    }
    return std::nullopt;
}

// Python's a // b of ints
std::int64_t floordiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::u32string slice(std::u32string_view s, std::int64_t start, std::int64_t stop) {
    // s[start:stop] for 0 <= start (stop past the end, or before start, as Python clamps them)
    const auto n = static_cast<std::int64_t>(s.size());
    start = std::clamp<std::int64_t>(start, 0, n);
    stop = std::clamp<std::int64_t>(stop, 0, n);
    if (stop <= start) return {};
    return std::u32string(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(stop - start)));
}

// `style.get(key)` of a dict (null when missing)
const Json& style_get(const Json& style, const char* key) {
    static const Json none;
    if (!style.is_object()) return none;
    const auto it = style.find(key);
    return it == style.end() ? none : *it;
}

}  // namespace

Rgb rgb_tuple(const Json& value) {
    Rgb out;
    for (const Json& v : core::iterate(value)) {
        if (v.is_boolean()) {
            out.push_back(v.get<bool>() ? 1 : 0);
        } else if (v.is_number_integer()) {
            out.push_back(core::to_int(v));
        } else {
            throw core::PyTypeError("color must be int or tuple");
        }
    }
    return out;
}

namespace {

// tuple(style.get("rgb") or fill)
Rgb style_rgb(const Json& style, const Rgb& fill) {
    const Json& rgb = style_get(style, "rgb");
    return core::py_truthy(rgb) ? rgb_tuple(rgb) : fill;
}

Ink opaque(const Rgb& fill) { return Ink::with_alpha(fill, 255); }  // fill + (255,)

}  // namespace

bool is_vs(char32_t c) { return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF); }

namespace {

// _latin_run(text, i)
std::size_t latin_run(std::u32string_view text, std::size_t i) {
    std::size_t j = i;
    std::size_t end = i;
    while (j < text.size() && (latin_letter(text[j]) || in(kLatinJoiners, text[j]))) {
        if (latin_letter(text[j])) end = j + 1;
        ++j;
    }
    return end;
}

}  // namespace

std::vector<Cell> cells(std::u32string_view text, bool tcy, bool latin) {
    std::vector<Cell> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t c = text[i];
        if (c == kTcyOpen) {  // (words chosen for 縦中横: one cell, whatever they are)
            std::size_t end = text.find(kTcyClose, i + 1);
            if (end == std::u32string_view::npos) end = text.size();
            if (end > i + 1) out.emplace_back(text.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        if (latin && latin_letter(c)) {
            const std::size_t j = latin_run(text, i);
            const auto letters = static_cast<std::size_t>(std::count_if(text.begin() + static_cast<std::ptrdiff_t>(i),
                                                                        text.begin() + static_cast<std::ptrdiff_t>(j), latin_letter));
            if (letters >= kLatinRun) {
                out.push_back(Cell(1, kRot) + Cell(text.substr(i, j - i)));
                i = j;
                continue;
            }
        }
        if (tcy && bang(c)) {
            std::size_t j = i;
            while (j < text.size() && bang(text[j]) && j - i < 3) ++j;
            if (j - i >= 2) {
                Cell cell;
                for (std::size_t k = i; k < j; ++k) cell.push_back(bang_of(text[k]));
                out.push_back(cell);
                i = j;
                continue;
            }
        }
        if (tcy && full_digit(c)) {
            std::size_t j = i;
            while (j < text.size() && full_digit(text[j])) ++j;
            if (j - i >= 2 && j - i <= 3) {  // "１２" reads as one number: side by side, like "12"
                Cell cell;
                for (std::size_t k = i; k < j; ++k) cell.push_back(U'0' + (text[k] - 0xFF10));
                out.push_back(cell);
            } else {
                for (std::size_t k = i; k < j; ++k) out.emplace_back(1, text[k]);
            }
            i = j;
            continue;
        }
        if (tcy && in(kTcyHalf, c) && c != U'!' && c != U'?') {
            std::size_t j = i;
            while (j < text.size() && in(kTcyHalf, text[j]) && text[j] != U'!' && text[j] != U'?') ++j;
            if (j - i >= 2 && j - i <= 3) {
                out.emplace_back(text.substr(i, j - i));
            } else {
                for (std::size_t k = i; k < j; ++k) out.emplace_back(1, text[k]);
            }
            i = j;
            continue;
        }
        if (is_vs(c)) {
            if (!out.empty() && out.back() != U"\n") out.back().push_back(c);  // (a variation selector stays with its letter)
            ++i;
            continue;
        }
        out.emplace_back(1, c);
        ++i;
    }
    return out;
}

bool has_vs(std::u32string_view cell) { return cell.size() == 2 && is_vs(cell[1]); }

std::u32string_view cell_text(std::u32string_view cell) {
    return !cell.empty() && cell[0] == kRot ? cell.substr(1) : cell;
}

std::vector<Column> columns_of(std::u32string_view text, std::int64_t per_col, bool tcy, bool latin,
                               const std::function<std::int64_t(const Cell&)>& units) {
    per_col = std::max<std::int64_t>(1, per_col);
    std::vector<Column> cols;
    Column cur;
    std::int64_t used = 0;
    for (Cell& cell : cells(text, tcy, latin)) {
        if (cell == U"\n") {
            cols.push_back(std::move(cur));
            cur.clear();
            used = 0;
            continue;
        }
        const std::int64_t need = units ? units(cell) : 1;
        if (!cur.empty() && used + need > per_col) {
            cols.push_back(std::move(cur));
            cur.clear();
            used = 0;
        }
        cur.push_back(std::move(cell));
        used += need;
    }
    cols.push_back(std::move(cur));
    // closing marks and small kana never start a column (ぶら下げ); opening brackets never end one
    for (std::size_t i = 1; i < cols.size(); ++i) {
        while (!cols[i].empty() && member(kLineStartKinsoku, cols[i].front()) && !cols[i - 1].empty()) {
            cols[i - 1].push_back(cols[i].front());
            cols[i].erase(cols[i].begin());
        }
    }
    for (std::size_t i = 0; i + 1 < cols.size(); ++i) {
        while (!cols[i].empty() && member(kLineEndKinsoku, cols[i].back()) && cols[i].size() > 1) {
            cols[i + 1].insert(cols[i + 1].begin(), cols[i].back());
            cols[i].pop_back();
        }
    }
    std::vector<Column> out;
    for (Column& col : cols) {
        if (!col.empty()) out.push_back(std::move(col));
    }
    return out;
}

namespace {

// _kind(char)
char kind_of(char32_t c) {
    if (c >= 0x3041 && c <= 0x309F) return 'h';
    if ((c >= 0x30A0 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F)) return 'k';
    if ((c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || c == U'々' || c == U'〆' || c == U'ヶ') return 'c';
    return 'o';
}

}  // namespace

std::vector<std::u32string> phrases(std::u32string_view text) {
    std::vector<std::u32string> out;
    std::u32string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t c = text[i];
        if (!cur.empty()) {
            const char32_t prev = cur.back();
            bool start = false;
            if (in(kOpening, c)) {
                start = true;
            } else if (in(kClosing, c)) {
                start = false;
            } else if (in(kBreaksAfter, prev)) {
                start = true;
            } else if (kind_of(prev) == 'h' && kind_of(c) != 'h' && !py_isspace(c)) {
                start = true;
            } else if (kind_of(prev) == 'h' && kind_of(c) == 'h' &&
                       std::any_of(kKanaWords.begin(), kKanaWords.end(), [&](std::u32string_view w) { return text.substr(i).starts_with(w); })) {
                start = true;
            }
            if (start) {
                out.push_back(cur);
                cur.clear();
            }
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

namespace {

// str.strip()
std::u32string_view py_strip(std::u32string_view s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && py_isspace(s[a])) ++a;
    while (b > a && py_isspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::vector<std::u32string> phrase_columns(std::u32string_view text, std::int64_t per,
                                           const std::function<std::int64_t(std::u32string_view)>& size_in) {
    const auto size = [&](std::u32string_view part) {
        return size_in ? size_in(part) : static_cast<std::int64_t>(part.size());
    };
    per = std::max<std::int64_t>(1, per);
    std::vector<std::u32string> pieces;
    for (std::u32string phrase : phrases(py_strip(text))) {
        if (size(phrase) > per) {  // (a phrase longer than a column: cut into even parts, 5 / 5 not 8 / 2)
            const auto len = static_cast<std::int64_t>(phrase.size());
            const std::int64_t parts = -floordiv(-size(phrase), per);
            const std::int64_t even = -floordiv(-len, parts);
            while (size(phrase) > per) {
                std::int64_t cut = std::min<std::int64_t>(even, static_cast<std::int64_t>(phrase.size()) - 1);
                while (cut > 1 && in(kClosing, phrase[static_cast<std::size_t>(cut)])) --cut;  // (no 、 or っ at a column's head)
                pieces.push_back(phrase.substr(0, static_cast<std::size_t>(cut)));
                phrase = phrase.substr(static_cast<std::size_t>(cut));
            }
        }
        pieces.push_back(phrase);
    }
    std::vector<std::u32string> cols;
    std::u32string cur;
    for (const std::u32string& piece : pieces) {
        if (!cur.empty() && size(cur + piece) > per) {
            cols.push_back(cur);
            cur.clear();
        }
        cur += piece;
    }
    if (!cur.empty()) cols.push_back(cur);
    if (cols.size() > 1 && size(cols.back()) <= 1 && size(cols[cols.size() - 2] + cols.back()) <= per + 1) {
        cols[cols.size() - 2] += cols.back();
        cols.pop_back();
    }
    if (cols.empty()) cols.emplace_back();
    return cols;
}

std::u32string without_periods(std::u32string_view text_in) {
    const auto period = [](char32_t c) { return c == U'。' || c == U'．'; };              // [。．]
    const auto closer = [](char32_t c) { return in(U"」』）)", c); };              // [」』）)]
    // re.sub(r"[。．]+(?=[」』）)]*\s*$)", "", text): the run of periods with nothing but closing brackets and
    // whitespace after it (the last one; `$` also before a final "\n", which \s* takes anyway)
    std::u32string text(text_in);
    {
        std::u32string out;
        std::size_t i = 0;
        while (i < text.size()) {
            if (!period(text[i])) {
                out.push_back(text[i++]);
                continue;
            }
            std::size_t j = i;
            while (j < text.size() && period(text[j])) ++j;
            std::size_t k = j;
            while (k < text.size() && closer(text[k])) ++k;
            while (k < text.size() && py_isspace(text[k])) ++k;
            if (k != text.size()) out.append(text, i, j - i);
            i = j;
        }
        text = std::move(out);
    }
    // re.sub(r"[。．]+(?=[」』）)]*$)", "", text, flags=re.M): a run before the closing brackets that end a line
    {
        std::u32string out;
        std::size_t i = 0;
        while (i < text.size()) {
            if (!period(text[i])) {
                out.push_back(text[i++]);
                continue;
            }
            std::size_t j = i;
            while (j < text.size() && period(text[j])) ++j;
            std::size_t k = j;
            while (k < text.size() && closer(text[k])) ++k;
            if (!(k == text.size() || text[k] == U'\n')) out.append(text, i, j - i);
            i = j;
        }
        text = std::move(out);
    }
    // re.sub(r"[。．](?![」』）)\n])", "\n", text): a period inside a line starts the next column
    std::u32string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (period(text[i]) && !(i + 1 < text.size() && (closer(text[i + 1]) || text[i + 1] == U'\n'))) {
            out.push_back(U'\n');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::u32string mark_tcy(std::u32string_view text, const StyleRuns& style_runs) {
    std::u32string out(text);
    std::size_t pos = 0;
    for (const auto& [run_words, run_style] : style_runs) {
        if (!core::py_truthy(core::py_get(run_style, "tcy")) || run_words.empty()) continue;
        const std::u32string words = u32(run_words);
        const std::size_t at = out.find(words, pos);
        if (at == std::u32string::npos || words.find(U'\n') != std::u32string::npos) continue;
        out = out.substr(0, at) + kTcyOpen + words + kTcyClose + out.substr(at + words.size());
        pos = at + words.size() + 2;
    }
    return out;
}

std::vector<Json> mono_runs(const std::vector<Json>& ruby_runs) {
    std::vector<Json> out;
    for (const Json& run : ruby_runs) {
        if (!core::py_truthy(run) || core::length(run) < 2) continue;
        const std::u32string base = u32(core::py_str(core::subscript(run, 0)));
        const std::u32string ruby = u32(core::py_str(core::subscript(run, 1)));
        if (base.size() > 1 && ruby.size() % base.size() == 0) {
            const std::size_t k = ruby.size() / base.size();
            for (std::size_t i = 0; i < base.size(); ++i) {
                out.push_back(Json::array({utf8(base.substr(i, 1)), utf8(ruby.substr(i * k, k))}));
            }
        } else {
            out.push_back(Json::array({utf8(base), utf8(ruby)}));
        }
    }
    return out;
}

std::vector<Json> char_styles(std::u32string_view text, const StyleRuns& style_runs, const Json& base) {
    const Json first = base.is_object() ? base : Json::object();
    std::vector<Json> out(text.size(), first);
    std::size_t pos = 0;
    for (const auto& [run_words, run_style] : style_runs) {
        if (run_words.empty()) continue;
        const std::u32string words = u32(run_words);
        const std::size_t at = text.find(words, pos);
        if (at == std::u32string_view::npos) continue;
        pos = at + words.size();
        if (!run_style.is_object()) continue;
        for (std::size_t i = at; i < pos; ++i) {
            for (const auto& [key, value] : run_style.items()) out[i][key] = value;
        }
    }
    return out;
}

int weight_level(const Json& value) {
    if (value.is_string()) {  // WEIGHTS: 標準・太・極太
        const auto& s = value.get_ref<const std::string&>();
        return s == "bold" ? 1 : s == "heavy" ? 2 : 0;
    }
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number()) return static_cast<int>(std::clamp<std::int64_t>(core::to_int(value), 0, 2));
    return 0;
}

int bold_px(std::int64_t em, int level) {
    level = std::clamp(level, 0, 2);
    if (level == 0) return 0;
    // (the outline goes round both sides of every stroke: more than this fills the inside of dense kanji)
    const double share = level == 1 ? 1.0 / 45 : 1.0 / 34;
    return static_cast<int>(std::max<std::int64_t>(level, core::py_round_int(static_cast<double>(em) * share)));
}

int bold_px(std::int64_t em, const Json& level) { return bold_px(em, weight_level(level)); }

namespace {

// _flat_cells(cols): the text the cells hold and, for each character, its (column, row)
std::pair<std::u32string, std::vector<std::pair<std::size_t, std::size_t>>> flat_cells(const std::vector<Column>& cols) {
    std::u32string chars;
    std::vector<std::pair<std::size_t, std::size_t>> where;
    for (std::size_t c = 0; c < cols.size(); ++c) {
        for (std::size_t r = 0; r < cols[c].size(); ++r) {
            for (const char32_t ch : cell_text(cols[c][r])) {
                chars.push_back(ch);
                where.emplace_back(c, r);
            }
        }
    }
    return {chars, where};
}

}  // namespace

std::vector<RubySpan> ruby_spans(const std::vector<Column>& cols, const std::vector<Json>& ruby_runs) {
    const auto [text, where] = flat_cells(cols);
    std::vector<RubySpan> spans;
    std::size_t pos = 0;
    for (const Json& run : ruby_runs) {
        if (!core::py_truthy(run) || core::length(run) < 2) continue;
        const Json base_value = core::subscript(run, 0);
        const Json ruby_value = core::subscript(run, 1);
        if (!core::py_truthy(base_value) || !core::py_truthy(ruby_value)) continue;
        const std::u32string base = u32(core::py_str(base_value));
        const std::u32string ruby = u32(core::py_str(ruby_value));
        const std::size_t at = text.find(base, pos);
        if (at == std::u32string::npos) continue;
        pos = at + base.size();
        // a base split over two columns gets its ruby split in proportion
        std::map<std::size_t, std::vector<std::size_t>> by_col;
        for (std::size_t i = at; i < at + base.size(); ++i) {
            auto& rows = by_col[where[i].first];
            if (std::find(rows.begin(), rows.end(), where[i].second) == rows.end()) rows.push_back(where[i].second);
        }
        std::int64_t count = 0;
        for (const auto& [c, rows] : by_col) count += static_cast<std::int64_t>(rows.size());
        std::int64_t taken = 0;
        std::size_t n = 0;
        const auto len = static_cast<std::int64_t>(ruby.size());
        for (const auto& [c, rows] : by_col) {
            const std::int64_t share = n == by_col.size() - 1
                                           ? len - taken
                                           : core::py_round_int(static_cast<double>(len * static_cast<std::int64_t>(rows.size())) / static_cast<double>(count));
            spans.push_back({c, *std::min_element(rows.begin(), rows.end()), *std::max_element(rows.begin(), rows.end()),
                             slice(ruby, taken, taken + share)});
            taken += share;
            ++n;
        }
    }
    return spans;
}

std::set<std::pair<std::size_t, std::size_t>> emphasis_cells(const std::vector<Column>& cols, const std::vector<std::u32string>& runs) {
    const auto [text, where] = flat_cells(cols);
    std::set<std::pair<std::size_t, std::size_t>> out;
    std::size_t pos = 0;
    for (const std::u32string& base : runs) {
        if (base.empty()) continue;
        const std::size_t at = text.find(base, pos);
        if (at == std::u32string::npos) continue;
        pos = at + base.size();
        for (std::size_t i = at; i < at + base.size(); ++i) {
            const auto [c, r] = where[i];
            if ((cols[c][r].empty() || cols[c][r][0] != kRot) && !py_isspace(text[i])) out.emplace(c, r);
        }
    }
    return out;
}

void composite_at(Image& base, const Image& im, std::int64_t x, std::int64_t y) {
    // (Pillow 12.3 checks only the source: a destination left of or above the image is cropped and pasted there, so
    // what falls outside is lost — arched's last strip can be lifted below zero)
    base.alpha_composite(im, Point{static_cast<int>(x), static_cast<int>(y)});
}

namespace {

Image blank(std::int64_t w, std::int64_t h) {
    return Image::create("RGBA", {static_cast<int>(w), static_cast<int>(h)}, Ink{0, 0, 0, 0});
}

// _has_glyph(font, char): the font draws it larger than a speck
bool has_drawn_glyph(const TrueTypeFont& font, std::u32string_view ch) {
    const auto box = font.getmask2(ch).first.getbbox();
    return box && box->x1 - box->x0 > 1 && box->y1 - box->y0 > 1;
}

// _raw_glyph(char, font, fill, bold): the letter drawn alone and cut to its ink; bold is the letter's own outline in
// its colour (a heavier weight made from any face)
Image raw_glyph(std::u32string_view ch, const TrueTypeFont& font, const Rgb& fill, int bold) {
    const int size = std::max(16, font.size());
    Image canvas = blank(size * 4, size * 4);
    {
        Draw draw(canvas);
        const Ink ink = opaque(fill);
        draw.text(PointD{static_cast<double>(size), static_cast<double>(size)}, ch, font, ink, {}, bold,
                  bold != 0 ? std::optional<Ink>(ink) : std::nullopt);
    }
    const auto bbox = canvas.getbbox();
    if (!bbox) return blank(size / 2, size / 2);
    return canvas.crop(*bbox);
}

}  // namespace

Image glyph(std::u32string_view ch, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold) {
    Image img = blank(em, em);
    bool rotate = member(kRotateCw, ch);
    std::u32string drawn(ch);
    const auto form = vertical_form(ch);
    if (form && has_drawn_glyph(font, std::u32string(1, *form))) {
        drawn = std::u32string(1, *form);
        rotate = false;
    } else if (form) {
        rotate = true;
    }
    Image raw = raw_glyph(drawn, font, fill, bold);
    std::int64_t gw = raw.width();
    std::int64_t gh = raw.height();
    const std::int64_t margin = std::max<std::int64_t>(1, floordiv(em, 12));
    if (member(kPunctTr, drawn)) {  // 、。 in the upper right
        const std::int64_t ox = std::max<std::int64_t>(0, em - margin - gw);
        const std::int64_t oy = margin;
        img.paste(raw, Point{static_cast<int>(ox), static_cast<int>(oy)}, &raw);
        return img;
    }
    if (member(kSmallKana, drawn)) {
        // Some fonts (e.g. the bundled Dela Gothic) draw small kana nearly full size; shrink them so they read as
        // small and sit in the upper right of the em box.
        const auto limit = core::py_trunc_int(static_cast<double>(em) * 0.62);
        if (std::max(gw, gh) > limit) {
            const double scale = static_cast<double>(limit) / static_cast<double>(std::max(gw, gh));
            raw = raw.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(static_cast<double>(gw) * scale))),
                              static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(static_cast<double>(gh) * scale)))},
                             Resample::Lanczos);
            gw = raw.width();
            gh = raw.height();
        }
        const std::int64_t ox = std::max<std::int64_t>(0, em - margin - gw);
        const std::int64_t oy = std::max<std::int64_t>(0, floordiv(em - gh, 2) - floordiv(em, 10));
        img.paste(raw, Point{static_cast<int>(ox), static_cast<int>(oy)}, &raw);
        return img;
    }
    const std::int64_t ox = std::max<std::int64_t>(0, floordiv(em - gw, 2));
    const std::int64_t oy = std::max<std::int64_t>(0, floordiv(em - gh, 2));
    img.paste(raw, Point{static_cast<int>(ox), static_cast<int>(oy)}, &raw);
    if (rotate) img = img.rotate(-90, Resample::Bicubic, false);
    return img;
}

Image tcy_glyph(std::u32string_view cell, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold) {
    // two or three characters side by side in one em (condensed to fit)
    Image raw = raw_glyph(cell, font, fill, bold);
    const std::int64_t gw = raw.width();
    const std::int64_t gh = raw.height();
    const auto limit = static_cast<double>(core::py_trunc_int(static_cast<double>(em) * 0.92));
    const double scale = core::py_min(core::py_min(1.0, limit / static_cast<double>(std::max<std::int64_t>(1, gw))),
                                      limit / static_cast<double>(std::max<std::int64_t>(1, gh)));
    if (scale < 1.0) {
        raw = raw.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(static_cast<double>(gw) * scale))),
                          static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(static_cast<double>(gh) * scale)))},
                         Resample::Lanczos);
    }
    Image img = blank(em, em);
    img.paste(raw, Point{static_cast<int>(floordiv(em - raw.width(), 2)), static_cast<int>(floordiv(em - raw.height(), 2))}, &raw);
    return img;
}

Image latin_glyph(std::u32string_view word, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold) {
    // half-width letters set across, then turned a quarter clockwise to lie along the column
    Image line = blank(std::max<std::int64_t>(1, core::py_trunc_int(font.getlength(word)) + em), em);
    const auto [ascent, descent] = font.getmetrics();
    {
        Draw draw(line);
        const Ink ink = opaque(fill);
        draw.text(PointD{0.0, static_cast<double>(floordiv(em - ascent - descent, 2))}, word, font, ink, {}, bold,
                  bold != 0 ? std::optional<Ink>(ink) : std::nullopt);
    }
    const auto box = line.getbbox();
    if (!box) return blank(em, em);
    line = line.crop(Box{box->x0, 0, box->x1, static_cast<int>(em)});
    return line.rotate(-90, Resample::Nearest, true);
}

void draw_mark(Image& image, double cx, double cy, double size, std::string_view kind, const Rgb& fill, bool vertical) {
    Draw draw(image);
    const Ink colour = opaque(fill);
    if (kind == "dot") {
        const double r = core::py_max(1.0, size * 0.2);
        draw.ellipse(BoxF{cx - r, cy - r, cx + r, cy + r}, colour);
        return;
    }
    const double r = core::py_max(1.2, size * 0.26);
    // a teardrop: round below, pointed toward the upper right (turned for horizontal text)
    std::vector<PointD> points;
    points.push_back(vertical ? PointD{cx + r * 1.1, cy - r * 1.6} : PointD{cx + r * 1.6, cy - r * 1.1});
    for (int k = 0; k < 13; ++k) {
        const double a = (-30 + k * 22.5) * (core::kPi / 180.0);  // math.radians
        points.push_back({cx + r * core::py_cos(a + core::kPi / 2), cy + r * core::py_sin(a + core::kPi / 2) * 0.95});
    }
    draw.polygon(points, colour);
    draw.ellipse(BoxF{cx - r, cy - r * 0.9, cx + r, cy + r}, colour);
}

Image compose(Fonts& fonts, const Face& face, const ComposeArgs& args) {
    // Vertical text, columns right to left. 傍点 sit right of their characters and ruby right of those (ruby moves
    // out when both are there), centred on their base, for every run.
    const std::int64_t em = args.em;
    const std::int64_t gap_px = std::max<std::int64_t>(0, core::py_round_int(static_cast<double>(em) * args.tracking));
    const auto font_for = [&](std::u32string_view ch, std::int64_t size) -> const TrueTypeFont& { return fonts.font(face, size, ch); };

    std::u32string plain;
    for (const char32_t c : args.text) {
        if (c != U'\n') plain.push_back(c);
    }
    const std::u32string text = mark_tcy(args.text, args.style_runs);  // (縦中横 chosen word by word)
    const std::vector<Json> ruby_runs = args.mono_ruby ? mono_runs(args.ruby_runs) : args.ruby_runs;
    // the style of each cell, in reading order (cells never change order when they wrap)
    std::vector<Cell> seq;
    for (Cell& cell : cells(text, args.tcy, args.latin)) {
        if (cell != U"\n") seq.push_back(std::move(cell));
    }
    const int level = std::clamp(args.bold, 0, 2);
    const std::vector<Json> per_char = char_styles(plain, args.style_runs, level ? Json{{"bold", level}} : Json());
    std::vector<Json> styles;
    std::size_t at = 0;
    for (const Cell& cell : seq) {
        styles.push_back(at < per_char.size() ? per_char[at] : Json::object());
        at += cell_text(cell).size();
    }
    const auto size_of = [&](const Json& style) -> std::int64_t {
        const Json* scale = style.is_object() && style.contains("scale") ? &style["scale"] : nullptr;
        const double s = scale != nullptr ? core::to_float(*scale) : 1.0;
        return std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(em) * core::py_max(0.3, core::py_min(3.0, s))));
    };

    std::map<std::size_t, Image> turned;
    std::vector<std::int64_t> heights;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        const std::int64_t size = size_of(styles[i]);
        if (seq[i][0] == kRot) {
            const Rgb rgb = style_rgb(styles[i], args.fill);
            Image image = latin_glyph(std::u32string_view(seq[i]).substr(1), font_for(U"A", size), size, rgb,
                                      bold_px(size, style_get(styles[i], "bold")));
            if (image.width() > size) {
                const auto h = core::py_trunc_int(static_cast<double>(static_cast<std::int64_t>(image.height()) * size) /
                                                  static_cast<double>(image.width()));
                image = image.resize({static_cast<int>(size), static_cast<int>(std::max<std::int64_t>(1, h))}, Resample::Lanczos);
            }
            heights.push_back(std::max<std::int64_t>(size, image.height()));
            turned.emplace(i, std::move(image));
        } else {
            heights.push_back(size);
        }
    }
    std::size_t order = 0;
    const std::vector<Column> cols = columns_of(text, args.max_height + gap_px, args.tcy, args.latin,
                                                [&](const Cell&) { return heights.at(order++) + gap_px; });
    if (cols.empty()) return blank(em, em);
    std::vector<std::vector<std::size_t>> index_of;
    std::size_t n = 0;
    for (const Column& col : cols) {
        std::vector<std::size_t> idx;
        for (std::size_t k = 0; k < col.size(); ++k) idx.push_back(n + k);
        n += col.size();
        index_of.push_back(std::move(idx));
    }
    const std::vector<RubySpan> spans = !ruby_runs.empty() ? ruby_spans(cols, ruby_runs) : std::vector<RubySpan>{};
    const auto marked = !args.emphasis_runs.empty() ? emphasis_cells(cols, args.emphasis_runs)
                                                    : std::set<std::pair<std::size_t, std::size_t>>{};
    const double ruby_scale = args.ruby_scale != 0.0 ? args.ruby_scale : 0.5;  // (ruby_scale or 0.5)
    const std::int64_t ruby_w =
        !spans.empty() ? std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(em) * core::py_max(0.25, core::py_min(0.8, ruby_scale))))
                       : 0;
    const std::int64_t mark_w = !marked.empty() ? std::max<std::int64_t>(3, core::py_round_int(static_cast<double>(em) * 0.36)) : 0;
    const std::int64_t gap = std::max<std::int64_t>(0, core::py_round_int(static_cast<double>(em) * args.leading));
    std::vector<std::int64_t> widths;
    for (const auto& idx : index_of) {
        std::int64_t w = 0;
        for (const std::size_t i : idx) w = std::max(w, size_of(styles[i]));
        widths.push_back(w);
    }
    std::int64_t width = gap * static_cast<std::int64_t>(cols.size() - 1);
    for (const std::int64_t w : widths) width += w + mark_w + ruby_w;
    std::vector<std::vector<std::int64_t>> ys;
    for (const auto& idx : index_of) {
        std::int64_t pos = 0;
        std::vector<std::int64_t> rows;
        for (const std::size_t i : idx) {
            rows.push_back(pos);
            pos += heights[i] + gap_px;
        }
        rows.push_back(pos - gap_px);
        ys.push_back(std::move(rows));
    }
    std::int64_t height = 0;
    for (std::size_t c = 0; c < ys.size(); ++c) height = c == 0 ? ys[c].back() : std::max(height, ys[c].back());
    if (args.align == "justify") {  // 均等揃え: every column spread over the whole height, its characters evenly apart
        height = std::max(height, args.max_height);
        for (std::size_t c = 0; c < index_of.size(); ++c) {
            if (index_of[c].size() < 2) continue;
            const double extra = static_cast<double>(height - ys[c].back()) / static_cast<double>(index_of[c].size() - 1);
            std::vector<std::int64_t> spread;
            for (std::size_t k = 0; k + 1 < ys[c].size(); ++k) {
                spread.push_back(core::py_round_int(static_cast<double>(ys[c][k]) + static_cast<double>(k) * extra));
            }
            spread.push_back(height);
            ys[c] = std::move(spread);
        }
    }
    Image out = blank(width, height);
    std::vector<std::int64_t> lefts;
    std::vector<std::int64_t> tops;
    std::int64_t right = width;
    const std::string_view mark_kind = args.emphasis_mark == "dot" ? "dot" : "sesame";  // MARKS.get(…, "sesame")
    for (std::size_t c = 0; c < cols.size(); ++c) {
        const std::int64_t cx = right - ruby_w - mark_w - widths[c];
        lefts.push_back(cx);
        right = cx - gap;
        const std::int64_t col_h = ys[c].back();
        const std::int64_t top =
            (args.align == "top" || args.align == "justify") ? 0 : floordiv(height - col_h, args.align == "center" ? 2 : 1);
        tops.push_back(top);
        for (std::size_t row = 0; row < cols[c].size(); ++row) {
            const Cell& cell = cols[c][row];
            const std::size_t i = index_of[c][row];
            const Json& style = styles[i];
            const std::int64_t size = size_of(style);
            const Rgb rgb = style_rgb(style, args.fill);
            const int thick = bold_px(size, style_get(style, "bold"));
            const std::int64_t y = top + ys[c][row];
            if (cell[0] == kRot) {
                const Image& image = turned.at(i);
                composite_at(out, image, cx + floordiv(widths[c] - image.width(), 2), y);
                continue;
            }
            const Image image = cell.size() > 1 && !has_vs(cell) ? tcy_glyph(cell, font_for(U"0", size), size, rgb, thick)
                                                                  : glyph(cell, font_for(cell, size), size, rgb, thick);
            composite_at(out, image, cx + floordiv(widths[c] - size, 2), y);
            if (marked.contains({c, row})) {
                draw_mark(out, static_cast<double>(cx + widths[c]) + static_cast<double>(mark_w) / 2,
                          static_cast<double>(y) + static_cast<double>(size) / 2, static_cast<double>(mark_w), mark_kind, rgb);
            }
        }
    }
    for (const RubySpan& span : spans) {
        if (span.ruby.empty()) continue;
        const std::int64_t rx = lefts[span.column] + widths[span.column] + mark_w;
        const std::int64_t y0 = ys[span.column][span.first];
        const std::int64_t y1 = ys[span.column][span.last] + heights[index_of[span.column][span.last]];
        const double centre = static_cast<double>(tops[span.column]) + static_cast<double>(y0 + y1) / 2;
        const auto count = static_cast<std::int64_t>(span.ruby.size());
        const std::int64_t top = std::max<std::int64_t>(
            0, std::min<std::int64_t>(height - ruby_w * count,
                                      core::py_round_int(centre - static_cast<double>(ruby_w * count) / 2)));
        for (std::int64_t i = 0; i < count; ++i) {
            const std::u32string ch(1, span.ruby[static_cast<std::size_t>(i)]);
            composite_at(out, glyph(ch, font_for(ch, std::max<std::int64_t>(8, ruby_w)), ruby_w, args.fill), rx, top + i * ruby_w);
        }
    }
    return out;
}

}  // namespace genko::render::text
