// PC5 G-d: 編集 → スペルチェック… — the English dictionary (ported from PhotoCraft v0.2.0's crates/text/src/spell.rs, MIT:
// the list's levels, the lowercase index, the words by length, the case rules and the suggestions by Damerau–Levenshtein
// distance and level) and Genko's notation table (表記の点検), over a book's lines and type layers. See spell.hpp.

#include "service/spell.hpp"

#include <QChar>
#include <QFile>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <tuple>

#include "core/error.hpp"
#include "core/layer_basics.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"

// (Q_INIT_RESOURCE declares the resource's symbol in the namespace it is used in: outside any)
static void init_spell_resources() {
    static const bool ready = [] {
        Q_INIT_RESOURCE(genko_spell);
        return true;
    }();
    (void)ready;
}

namespace genko::service::spell {

using core::Json;

namespace {

constexpr std::size_t kMostNotations = 2000;
constexpr std::size_t kLongestNotation = 200;  // (code points of either side)

std::u32string u32(std::string_view text) { return core::ruby::code_points(text); }
std::string utf8(std::u32string_view text) { return core::ruby::to_utf8(text); }

bool is_upper(char32_t c) { return QChar::isUpper(c); }
char32_t lower(char32_t c) { return QChar::toLower(c); }
char32_t upper(char32_t c) { return QChar::toUpper(c); }
bool is_letter(char32_t c) { return QChar::isLetter(c); }
bool is_number(char32_t c) { return QChar::isNumber(c); }
bool is_mark(char32_t c) {
    const QChar::Category cat = QChar::category(c);
    return cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_Enclosing;
}
// A letter of the Latin script (the dictionary's): Basic Latin, Latin-1, Latin Extended-A and -B, IPA, Latin Extended
// Additional and the fullwidth forms' are not words for it.
bool latin_letter(char32_t c) {
    if (!is_letter(c)) return false;
    return c < 0x0250 || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x2C60 && c <= 0x2C7F) || (c >= 0xA720 && c <= 0xA7FF);
}
bool apostrophe(char32_t c) { return c == U'\'' || c == U'’' || c == U'ʼ'; }

std::u32string lowered(std::u32string_view w) {
    std::u32string out(w);
    for (char32_t& c : out) c = lower(c);
    return out;
}
std::u32string uppered(std::u32string_view w) {
    std::u32string out(w);
    for (char32_t& c : out) c = upper(c);
    return out;
}
std::u32string capitalised(std::u32string_view w) {
    std::u32string out(w);
    if (!out.empty()) out[0] = upper(out[0]);
    return out;
}
// Typographic apostrophes as '.
std::u32string normal(std::u32string_view w) {
    std::u32string out(w);
    for (char32_t& c : out) {
        if (c == U'’' || c == U'‘' || c == U'ʼ') c = U'\'';
    }
    return out;
}

// Damerau–Levenshtein (optimal string alignment), or none past `most`.
std::optional<std::size_t> distance(const std::u32string& a, const std::u32string& b, std::size_t most, std::vector<std::size_t>& prev2,
                                    std::vector<std::size_t>& prev, std::vector<std::size_t>& cur) {
    const std::size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > most) return std::nullopt;
    prev2.assign(m + 1, 0);
    prev.resize(m + 1);
    for (std::size_t j = 0; j <= m; ++j) prev[j] = j;
    cur.assign(m + 1, 0);
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        std::size_t row_min = cur[0];
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] != b[j - 1] ? 1 : 0;
            std::size_t v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, prev2[j - 2] + 1);
            cur[j] = v;
            row_min = std::min(row_min, v);
        }
        if (row_min > most) return std::nullopt;
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    if (prev[m] <= most) return prev[m];
    return std::nullopt;
}

std::string gunzip(const QByteArray& packed) {
    std::string out;
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw core::Error("file", "the dictionary cannot be read");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.constData()));
    zs.avail_in = static_cast<uInt>(packed.size());
    char buffer[1 << 16];
    int status = Z_OK;
    while (status == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof buffer;
        status = inflate(&zs, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) break;
        out.append(buffer, sizeof buffer - zs.avail_out);
        if (out.size() > 64U * 1024 * 1024) break;
    }
    inflateEnd(&zs);
    if (status != Z_STREAM_END) throw core::Error("file", "the dictionary cannot be read");
    return out;
}

}  // namespace

const Dictionary& Dictionary::english() {
    static const Dictionary dictionary = [] {
        init_spell_resources();
        QFile source(QStringLiteral(":/genko/dict/en_US-scowl-50.txt.gz"));
        if (!source.open(QIODevice::ReadOnly)) throw core::Error("file", "the dictionary cannot be read");
        return from_list(gunzip(source.readAll()));
    }();
    return dictionary;
}

Dictionary Dictionary::from_list(std::string_view list) {
    Dictionary d;
    std::uint8_t level = 50;
    std::size_t at = 0;
    while (at <= list.size()) {
        std::size_t end = list.find('\n', at);
        if (end == std::string_view::npos) end = list.size();
        std::string_view line = list.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) {
            if (end == list.size()) break;
            continue;
        }
        if (line.front() == '#') {
            int n = 0;
            bool digits = line.size() > 1;
            for (const char c : line.substr(1)) {
                if (c < '0' || c > '9') digits = false;
                else n = n * 10 + (c - '0');
            }
            if (digits && n <= 255) level = static_cast<std::uint8_t>(n);
            continue;
        }
        std::u32string word;
        try {
            word = u32(line);
        } catch (const core::Error&) {
            continue;  // (not UTF-8: left out)
        }
        if (d.words_.contains(word)) continue;
        d.words_.emplace(word, level);
        d.lower_[lowered(word)].push_back(word);
    }
    for (const auto& [lower_word, forms] : d.lower_) {
        const std::size_t n = lower_word.size();
        if (d.by_length_.size() <= n) d.by_length_.resize(n + 1);
        std::uint8_t best = 50;
        for (const std::u32string& f : forms) best = std::min(best, d.words_.at(f));
        d.by_length_[n].push_back(Entry{lower_word, best});
    }
    for (auto& bucket : d.by_length_) {
        std::sort(bucket.begin(), bucket.end(), [](const Entry& a, const Entry& b) { return std::tie(a.lower, a.level) < std::tie(b.lower, b.level); });
    }
    return d;
}

bool Dictionary::check(std::string_view word, const std::set<std::string>& user) const {
    std::u32string w = normal(u32(word));
    if (w.size() > 2 && w.ends_with(U"'s")) w.erase(w.end() - 2, w.end());
    while (!w.empty() && w.front() == U'\'') w.erase(w.begin());
    while (!w.empty() && w.back() == U'\'') w.pop_back();
    if (w.empty() || std::any_of(w.begin(), w.end(), is_number) || std::none_of(w.begin(), w.end(), is_letter)) return true;
    const std::u32string low = lowered(w);
    if (user.contains(utf8(low)) || words_.contains(w)) return true;
    const auto found = lower_.find(low);
    if (found == lower_.end()) return false;
    bool all_caps = true;
    for (const char32_t c : w) {
        if (is_letter(c) && !is_upper(c)) all_caps = false;
    }
    const bool first_cap = is_upper(w.front()) && std::none_of(w.begin() + 1, w.end(), is_upper);
    for (const std::u32string& f : found->second) {
        const bool f_lower = std::none_of(f.begin(), f.end(), is_upper);
        // "the" → "The"/"THE"; "Paris" → "PARIS"; "NASA" stays as listed (the exact match above)
        if (all_caps || (f_lower && first_cap) || (first_cap && capitalised(f) == w)) return true;
    }
    return false;
}

std::vector<std::string> Dictionary::suggest(std::string_view word, std::size_t max) const {
    const std::u32string w = normal(u32(word));
    const std::u32string low = lowered(w);
    const std::size_t n = low.size();
    const std::size_t most = n <= 4 ? 1 : 2;
    std::u32string sorted = low;
    std::sort(sorted.begin(), sorted.end());
    const auto anagram = [&](const std::u32string& c) {
        std::u32string v = c;
        std::sort(v.begin(), v.end());
        return v == sorted;
    };
    struct Found {
        std::size_t distance;
        bool not_anagram;
        std::uint8_t level;
        const std::u32string* word;
    };
    std::vector<Found> found;
    std::vector<std::size_t> prev2, prev, cur;
    for (std::size_t len = n > most ? n - most : 0; len <= n + most; ++len) {
        if (len >= by_length_.size()) break;
        for (const Entry& e : by_length_[len]) {
            if (const auto d = distance(low, e.lower, most, prev2, prev, cur)) found.push_back(Found{*d, !anagram(e.lower), e.level, &e.lower});
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return std::tie(a.distance, a.not_anagram, a.level, *a.word) < std::tie(b.distance, b.not_anagram, b.level, *b.word);
    });
    std::size_t letters = 0;
    bool all_caps = true;
    for (const char32_t c : w) {
        if (!is_letter(c)) continue;
        ++letters;
        if (!is_upper(c)) all_caps = false;
    }
    all_caps = all_caps && letters > 1;
    const bool first_cap = !w.empty() && is_upper(w.front());
    std::vector<std::string> out;
    for (const Found& f : found) {
        // (common — lowercase — forms before names: "brwon" → "brown", then "Brown")
        std::vector<std::u32string> forms = lower_.at(*f.word);
        std::stable_sort(forms.begin(), forms.end(), [](const std::u32string& a, const std::u32string& b) {
            return std::any_of(a.begin(), a.end(), is_upper) < std::any_of(b.begin(), b.end(), is_upper);
        });
        for (const std::u32string& form : forms) {
            const std::u32string s = all_caps ? uppered(form) : first_cap ? capitalised(form) : form;
            const std::string text = utf8(s);
            if (s != w && std::find(out.begin(), out.end(), text) == out.end()) out.push_back(text);
        }
        if (out.size() >= max) break;
    }
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> words(std::u32string_view text) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t c = text[i];
        const bool inner = apostrophe(c) && start && i + 1 < text.size() && latin_letter(text[i + 1]);
        const bool joined = start && is_mark(c);  // (a mark on the word's letter: still the word)
        const bool latin_digit = c >= U'0' && c <= U'9';
        if (latin_letter(c) || latin_digit || inner || joined) {
            if (!start) start = i;
        } else if (start) {
            out.emplace_back(*start, i);
            start.reset();
        }
    }
    if (start) out.emplace_back(*start, text.size());
    return out;
}

std::vector<Notation> notations_of(const Json& raw) {
    std::vector<Notation> out;
    if (!raw.is_array()) return out;
    for (const Json& entry : raw) {
        Notation n;
        if (entry.is_object() && entry.value("from", Json()).is_string() && entry.value("to", Json()).is_string()) {
            n.from = entry["from"].get<std::string>();
            n.to = entry["to"].get<std::string>();
        } else if (entry.is_array() && entry.size() == 2 && entry[0].is_string() && entry[1].is_string()) {
            n.from = entry[0].get<std::string>();
            n.to = entry[1].get<std::string>();
        } else {
            continue;
        }
        try {
            if (n.from.empty() || n.from == n.to || u32(n.from).size() > kLongestNotation || u32(n.to).size() > kLongestNotation) continue;
        } catch (const core::Error&) {
            continue;
        }
        out.push_back(std::move(n));
        if (out.size() >= kMostNotations) break;
    }
    return out;
}

Json notations_json(const std::vector<Notation>& table) {
    Json out = Json::array();
    for (const Notation& n : table) out.push_back(Json{{"from", n.from}, {"to", n.to}});
    return out;
}

std::set<std::string> user_words(std::string_view joined) {
    std::set<std::string> out;
    std::string word;
    const auto flush = [&] {
        std::size_t a = 0, b = word.size();
        while (a < b && (word[a] == ' ' || word[a] == '\r')) ++a;
        while (b > a && (word[b - 1] == ' ' || word[b - 1] == '\r')) --b;
        if (b > a) {
            try {
                out.insert(utf8(lowered(u32(std::string_view(word).substr(a, b - a)))));
            } catch (const core::Error&) {
            }
        }
        word.clear();
    };
    for (const char c : joined) {
        if (c == '\n' || c == '\t') flush();
        else word.push_back(c);
    }
    flush();
    return out;
}

namespace {

// Code point offsets → graphemes of the text.
struct Graphemes {
    std::vector<std::size_t> starts;
    explicit Graphemes(std::u32string_view text) : starts(core::ruby::bounds(text)) {}
    std::int64_t at(std::size_t cp) const {
        const auto it = std::upper_bound(starts.begin(), starts.end(), cp);
        return static_cast<std::int64_t>(it - starts.begin()) - 1;
    }
    // (an end inside a grapheme takes the whole of it)
    std::int64_t end_at(std::size_t cp) const {
        const auto it = std::lower_bound(starts.begin(), starts.end(), cp);
        return static_cast<std::int64_t>(it - starts.begin());
    }
};

void check_text(const std::string& text, const Options& options, Finding base, std::vector<Finding>& out,
                std::map<std::string, std::vector<std::string>>& suggested) {
    if (out.size() >= options.most) return;
    std::u32string cps;
    try {
        cps = u32(text);
    } catch (const core::Error&) {
        return;
    }
    const Graphemes graphemes(cps);
    struct Hit {
        std::size_t start, end;
        Finding finding;
    };
    std::vector<Hit> hits;
    // (the room left in the findings: the first places of the text, by where they start, kept — the rest let go as they come,
    // so a one-letter notation over a long text makes no more than this)
    const std::size_t room = options.most - out.size();
    const auto order = [](const Hit& a, const Hit& b) { return std::tie(a.start, a.end) < std::tie(b.start, b.end); };
    const auto trim = [&] {
        if (hits.size() <= 2 * room) return;
        std::stable_sort(hits.begin(), hits.end(), order);
        hits.erase(hits.begin() + static_cast<std::ptrdiff_t>(room), hits.end());
    };
    if (options.spelling) {
        const Dictionary& dictionary = Dictionary::english();
        std::set<std::string> accepted = options.user;
        accepted.insert(options.ignore.begin(), options.ignore.end());
        for (const auto& [a, b] : words(cps)) {
            const std::string word = utf8(std::u32string_view(cps).substr(a, b - a));
            if (dictionary.check(word, accepted)) continue;
            Finding f = base;
            f.kind = "spelling";
            f.word = word;
            if (options.suggestions > 0) {  // (none asked — the window's —: made for the place shown alone, suggestions_for)
                auto known = suggested.find(word);  // (a word said again: its suggestions once)
                if (known == suggested.end()) known = suggested.emplace(word, dictionary.suggest(word, options.suggestions)).first;
                f.suggestions = known->second;
            }
            hits.push_back(Hit{a, b, std::move(f)});
            if (hits.size() >= room) break;  // (the words come in order: the first ones enough)
        }
    }
    if (options.notation) {
        for (const Notation& n : options.notations) {
            const std::u32string from = u32(n.from), to = u32(n.to);
            // (where `from` stands inside what it should become — 「出来」 → 「出来る」 — it is already right there)
            const std::size_t inside = to.find(from);
            std::size_t at = 0, seen = 0;
            while (seen < room && (at = cps.find(from, at)) != std::u32string::npos) {  // (in order: its first ones enough)
                const bool settled = inside != std::u32string::npos && at >= inside && cps.compare(at - inside, to.size(), to) == 0;
                if (!settled) {
                    Finding f = base;
                    f.kind = "notation";
                    f.word = n.from;
                    f.suggestions = {n.to};
                    hits.push_back(Hit{at, at + from.size(), std::move(f)});
                    ++seen;
                    trim();
                }
                at += std::max<std::size_t>(1, from.size());
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), order);
    for (Hit& h : hits) {
        h.finding.start = graphemes.at(h.start);
        h.finding.end = graphemes.end_at(h.end);
        out.push_back(std::move(h.finding));
        if (out.size() >= options.most) return;
    }
}

}  // namespace

std::vector<Finding> check(const core::Document& doc, const Options& options) {
    std::vector<Finding> out;
    std::map<std::string, std::vector<std::string>> suggested;
    for (const auto& page : doc.pages) {
        if (options.page && !(page->index == *options.page)) continue;
        for (const core::StoryLine* line : doc.story_for_page(page->index)) {
            if (!options.sfx && line->balloon == "sfx") continue;  // (描き文字: its sounds are no words — unless asked)
            Finding base;
            base.line = line->id;
            base.page = page->index;
            check_text(line->text, options, base, out, suggested);
        }
        for (auto it = page->layers.rbegin(); it != page->layers.rend(); ++it) {
            // (a layer with no record — a broken book —, a locked one, すべてをロック too, passed over)
            if (!core::type_text::is_type_layer(*it) || !it->type || it->locked ||
                core::locked_all(page->layers, static_cast<std::size_t>(std::distance(it, page->layers.rend())) - 1))
                continue;
            Finding base;
            base.layer_id = it->id;
            base.page = page->index;
            check_text(it->type->text, options, base, out, suggested);
        }
        if (out.size() >= options.most) break;
    }
    return out;
}

std::vector<std::string> suggestions_for(const Finding& finding, std::size_t max) {
    if (finding.kind != "spelling") return finding.suggestions;
    if (!finding.suggestions.empty()) return finding.suggestions;
    return Dictionary::english().suggest(finding.word, max);
}

Json to_json(const std::vector<Finding>& findings) {
    Json list = Json::array();
    for (const Finding& f : findings) {
        Json item{{"kind", f.kind}};
        if (!f.line.empty()) item["line"] = f.line;
        item["page"] = f.page.json();
        if (!f.layer_id.empty()) item["layer_id"] = f.layer_id;
        item["start"] = f.start;
        item["end"] = f.end;
        item["word"] = f.word;
        item["suggestions"] = f.suggestions;
        list.push_back(std::move(item));
    }
    return Json{{"findings", list}, {"count", findings.size()}};
}

std::vector<std::pair<std::int64_t, std::int64_t>> places_of(std::string_view text, std::string_view word) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    std::u32string cps, w;
    try {
        cps = u32(text);
        w = u32(word);
    } catch (const core::Error&) {
        return out;
    }
    if (w.empty()) return out;
    const Graphemes graphemes(cps);
    for (const auto& [a, b] : words(cps)) {
        if (cps.compare(a, b - a, w) == 0 && b - a == w.size()) out.emplace_back(graphemes.at(a), graphemes.end_at(b));
    }
    // (a word of the notation table is no word of letters: found where it stands)
    if (out.empty() && std::none_of(w.begin(), w.end(), latin_letter)) {
        std::size_t at = 0;
        while ((at = cps.find(w, at)) != std::u32string::npos) {
            out.emplace_back(graphemes.at(at), graphemes.end_at(at + w.size()));
            at += w.size();
        }
    }
    return out;
}

}  // namespace genko::service::spell
