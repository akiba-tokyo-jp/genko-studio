// PC5 G-c (type@1, core/type_styles.hpp): the book's character and paragraph styles and what applying, redefining,
// deleting and clearing them does to a text's record — PhotoCraft v0.2.0's engine/src/type_styles_cmds.rs and
// doc/src/text_styles.rs (MIT; native/third_party/photocraft/NOTICE.md), remade for Genko's records, which keep only what
// a text says of its own past its styles.

#include "core/type_styles.hpp"

#include <QString>

#include <algorithm>
#include <map>
#include <utility>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/smart.hpp"

namespace genko::core::type_styles {

namespace {

constexpr std::int64_t kMaxStyleName = 64;

const char* prefix_of(Kind kind) { return kind == Kind::Character ? "cs-" : "ps-"; }

// The record's native_type to change (none: empty) and back in its normal form (none when it holds nothing).
Json native_of(const StoryLine& record) { return record.native_type ? *record.native_type : Json::object(); }
void set_native(StoryLine& record, const Json& nt) {
    try {
        const Json normal = type_text::check_native_type(nt, record.text);
        if (normal.is_null()) record.native_type.reset();
        else record.native_type = normal;
    } catch (const Error& error) {
        throw OpError(error.what());
    }
}

std::int64_t int_at(const Json& item, const char* key) { return item.value(key, std::int64_t{0}); }

// The runs with [start, end) cut out and `change` done to each piece of it (stretches no run covers given runs of their own);
// runs left holding nothing dropped, neighbours alike joined.
template <typename Change>
Json lay_runs(const Json& runs, std::int64_t start, std::int64_t end, Change change) {
    std::vector<Json> out;
    const auto piece = [&](const Json& run, std::int64_t s, std::int64_t e, bool inside) {
        if (e <= s) return;
        Json item = run;
        item["start"] = s;
        item["end"] = e;
        if (inside) change(item);
        out.push_back(std::move(item));
    };
    std::int64_t covered = start;
    std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
    if (runs.is_array()) {
        for (const Json& run : runs) {
            const std::int64_t s = int_at(run, "start"), e = int_at(run, "end");
            if (e <= start || s >= end) {
                out.push_back(run);
                continue;
            }
            if (s > covered) gaps.emplace_back(covered, s);
            covered = std::max(covered, e);
            piece(run, s, start, false);
            piece(run, std::max(s, start), std::min(e, end), true);
            piece(run, end, e, false);
        }
    }
    if (covered < end) gaps.emplace_back(covered, end);
    for (const auto& [s, e] : gaps) piece(Json::object(), s, e, true);
    std::sort(out.begin(), out.end(), [](const Json& a, const Json& b) { return int_at(a, "start") < int_at(b, "start"); });
    Json joined = Json::array();
    for (Json& run : out) {
        if (run.contains("char") && (!run["char"].is_object() || run["char"].empty())) run.erase("char");
        if (!run.contains("char") && !run.contains("style")) continue;  // (a run of nothing is none)
        if (!joined.empty()) {
            Json& last = joined.back();
            if (int_at(last, "end") == int_at(run, "start") && last.value("style", Json()) == run.value("style", Json()) &&
                last.value("char", Json()) == run.value("char", Json())) {
                last["end"] = run["end"];
                continue;
            }
        }
        joined.push_back(std::move(run));
    }
    return joined;
}

// The paragraph entries of native_type by index (`made`: entries for the paragraphs given, made empty where none).
std::map<std::int64_t, Json> paragraph_entries(const Json& nt) {
    std::map<std::int64_t, Json> out;
    if (const Json* ps = get(nt, "paragraphs"); ps != nullptr && ps->is_array()) {
        for (const Json& p : *ps) out[int_at(p, "index")] = p;
    }
    return out;
}
// The entry of paragraph `p` (made, an object, when there is none).
Json& entry_at(std::map<std::int64_t, Json>& entries, std::int64_t p) {
    Json& item = entries[p];
    if (!item.is_object()) item = Json::object({{"index", p}});
    return item;
}

void put_paragraphs(Json& nt, std::map<std::int64_t, Json>& entries) {
    Json list = Json::array();
    for (auto& [index, item] : entries) {
        item["index"] = index;
        if (item.contains("para") && (!item["para"].is_object() || item["para"].empty())) item.erase("para");
        if (item.size() > 1) list.push_back(std::move(item));
    }
    if (list.empty()) nt.erase("paragraphs");
    else nt["paragraphs"] = std::move(list);
}

// Every attribute of `a` whose value `b` does not have (a value of null left out: nothing to lay).
Json differing(const Json& a, const Json& b) {
    Json out = Json::object();
    for (const auto& [key, value] : a.items()) {
        if (value.is_null()) continue;
        const auto it = b.find(key);
        if (it == b.end() || *it != value) out[key] = value;
    }
    return out;
}

// The attributes of the stretch holding grapheme `at` (a text with no letters: the one stretch).
const Json& chars_at(const std::vector<type_text::CharRun>& runs, std::int64_t at) {
    auto it = std::upper_bound(runs.begin(), runs.end(), at, [](std::int64_t g, const type_text::CharRun& r) { return g < r.start; });
    return it == runs.begin() ? runs.front().attrs : (it - 1)->attrs;
}

// Do two looks' characters differ anywhere in [start, end)?
bool chars_differ(const std::vector<type_text::CharRun>& a, const std::vector<type_text::CharRun>& b, std::int64_t start, std::int64_t end) {
    if (end <= start) return chars_at(a, start) != chars_at(b, start);
    std::vector<std::int64_t> edges{start, end};
    for (const auto* runs : {&a, &b}) {
        for (const type_text::CharRun& r : *runs) {
            if (r.start > start && r.start < end) edges.push_back(r.start);
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        if (chars_at(a, edges[i]) != chars_at(b, edges[i])) return true;
    }
    return false;
}

// The record without its own character attributes (the whole's char, the ranges' char) — or paragraph attributes.
StoryLine without_own(const StoryLine& record, Kind kind) {
    StoryLine out = record;
    if (!record.native_type) return out;
    Json nt = *record.native_type;
    if (kind == Kind::Character) {
        nt.erase("char");
        if (nt.contains("runs")) {
            Json runs = Json::array();
            for (Json run : nt["runs"]) {
                run.erase("char");
                if (run.contains("style")) runs.push_back(std::move(run));
            }
            if (runs.empty()) nt.erase("runs");
            else nt["runs"] = std::move(runs);
        }
    } else {
        nt.erase("para");
        auto entries = paragraph_entries(nt);
        for (auto& [index, item] : entries) item.erase("para");
        put_paragraphs(nt, entries);
    }
    out.native_type = nt.empty() ? std::nullopt : std::optional<Json>(nt);
    return out;
}

// Is the byte at `i` (forwards) or before `i` (backwards) a space — ASCII's or the ideographic one? Its length, 0 for none.
std::size_t space_at(const std::string& name, std::size_t i, bool back) {
    static const std::string ideographic = "\xE3\x80\x80";
    const auto ascii = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    if (!back) {
        if (i < name.size() && ascii(name[i])) return 1;
        if (name.compare(i, 3, ideographic) == 0) return 3;
        return 0;
    }
    if (i > 0 && ascii(name[i - 1])) return 1;
    if (i >= 3 && name.compare(i - 3, 3, ideographic) == 0) return 3;
    return 0;
}

// A name without the spaces round it.
std::string trimmed_name(const std::string& name) {
    std::size_t a = 0, b = name.size();
    while (a < b) {
        const std::size_t k = space_at(name, a, false);
        if (k == 0) break;
        a += k;
    }
    while (b > a) {
        const std::size_t k = space_at(name, b, true);
        if (k == 0) break;
        b -= k;
    }
    return name.substr(a, b - a);
}

// The text without its last code point (UTF-8).
std::string without_last(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        --end;
        if ((static_cast<unsigned char>(text[end]) & 0xC0) != 0x80) break;
    }
    return text.substr(0, end);
}

// A style's own attributes (an object; none: empty).
Json own_of(const Json* style, const char* part) {
    if (style == nullptr) return Json::object();
    const Json* v = get(*style, part);
    return v != nullptr && v->is_object() ? *v : Json::object();
}

}  // namespace

std::string_view kind_name(Kind kind) { return kind == Kind::Character ? "character" : "paragraph"; }

const Json* find(const Json* styles, Kind kind, std::string_view id) {
    if (kind == Kind::Paragraph && id == kBasic) {  // (a book always has Basic Paragraph, its attributes none or its own)
        static const Json empty = Json::object();
        const Json* basic = styles != nullptr && styles->is_object() ? get(*styles, "basic") : nullptr;
        return basic != nullptr && basic->is_object() ? basic : &empty;
    }
    if (styles == nullptr || !styles->is_object()) return nullptr;
    const Json* list = get(*styles, kind_name(kind));
    if (list == nullptr || !list->is_array()) return nullptr;
    for (const Json& s : *list) {
        if (s.is_object() && s.value("id", Json()) == Json(std::string(id))) return &s;
    }
    return nullptr;
}

std::string next_id(const Json* styles, Kind kind) {
    std::set<std::string> taken;
    if (styles != nullptr && styles->is_object()) {
        if (const Json* list = get(*styles, kind_name(kind)); list != nullptr && list->is_array()) {
            for (const Json& s : *list) taken.insert(s.value("id", std::string()));
        }
    }
    for (int n = 1;; ++n) {
        std::string id = prefix_of(kind) + std::to_string(n);
        if (!taken.contains(id)) return id;
    }
}

std::string unique_name(const Json* styles, Kind kind, const std::string& base, bool numbered) {
    std::set<std::string> taken{std::string(kind == Kind::Character ? kNoneName : kBasicName)};
    if (styles != nullptr && styles->is_object()) {
        if (const Json* list = get(*styles, kind_name(kind)); list != nullptr && list->is_array()) {
            for (const Json& s : *list) taken.insert(s.value("name", std::string()));
        }
    }
    if (!numbered && !taken.contains(base) && ruby::count(base) <= kMaxStyleName) return base;
    // (a long base cut so the number fits the 64 characters a name may have)
    for (int n = numbered ? 1 : 2;; ++n) {
        const std::string tail = " " + std::to_string(n);
        std::string head = base;
        while (ruby::count(head + tail) > kMaxStyleName && !head.empty()) head = without_last(head);
        const std::string name = head + tail;
        if (!taken.contains(name)) return name;
    }
}

std::string checked_name(const Json* styles, Kind kind, const Json& name, std::string_view except) {
    if (!name.is_string()) throw OpError("the style's name must be a string");
    // (review P3-8: one spelling of the same letters — NFC —, and no control characters: a line break in a panel's row or a
    // PSD's style sheet name)
    const QString normal = QString::fromStdString(name.get<std::string>()).normalized(QString::NormalizationForm_C);
    for (const char32_t c : normal.toStdU32String()) {
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029)
            throw OpError("the style's name must not hold a line break or a control character");
    }
    const std::string trimmed = trimmed_name(normal.toStdString());
    if (trimmed.empty() || ruby::count(trimmed) > kMaxStyleName) throw OpError("the style's name is 1 to 64 characters");
    if (trimmed == kNoneName || trimmed == kBasicName) throw OpError("the name " + trimmed + " is the panel's own: choose another name");
    if (styles != nullptr && styles->is_object()) {
        if (const Json* list = get(*styles, kind_name(kind)); list != nullptr && list->is_array()) {
            for (const Json& s : *list) {
                if (s.value("name", std::string()) == trimmed && s.value("id", std::string()) != except)
                    throw OpError("the book has a " + std::string(kind_name(kind)) + " style named " + trimmed + " already");
            }
        }
    }
    return trimmed;
}

Attrs split_attrs(const Json& given) {
    Attrs out;
    if (given.is_null()) return out;
    if (!given.is_object()) throw OpError("attrs gives the attributes as {attribute: value}");
    for (const auto& [key, value] : given.items()) {
        if (key == "size_pt") {
            if (given.contains("size_mm")) throw OpError("size_pt or size_mm sizes the text, not both");
            if (value.is_null()) {
                out.chars["size_mm"] = nullptr;
                continue;
            }
            const double size = finite_float(value, "size_pt");
            if (!(size > 0) || size > 1296) throw OpError("size_pt must be above 0 and at most 1296");
            out.chars["size_mm"] = py_round(size * 25.4 / 72.0, 4);
        } else if (type_text::char_attr(key) != nullptr) {
            out.chars[key] = value;
        } else if (type_text::para_attr(key) != nullptr) {
            out.paras[key] = value;
        } else {
            throw OpError("unknown attribute " + key + " (a character or paragraph attribute of TEXT-02)");
        }
    }
    // (each value checked as its table says; nulls kept, to take one away)
    const auto check = [](const Json& attrs, bool character) {
        Json present = Json::object();
        for (const auto& [key, value] : attrs.items()) {
            if (!value.is_null()) present[key] = value;
        }
        try {
            const Json normal = character ? type_text::check_char(present) : type_text::check_para(present);
            Json laid = attrs;
            for (const auto& [key, value] : normal.items()) laid[key] = value;
            return laid;
        } catch (const Error& error) {
            throw OpError(error.what());
        }
    };
    out.chars = check(out.chars, true);
    out.paras = check(out.paras, false);
    return out;
}

Look look_of(const StoryLine& record, const Json* styles) {
    Look out;
    out.chars = type_text::resolve_chars(record, styles);
    const std::int64_t count = type_text::paragraph_count(record.text);
    for (std::int64_t p = 0; p < count; ++p) out.paras.push_back(type_text::resolve_para(record, styles, p));
    return out;
}

void keep_look(StoryLine& record, const Json* styles, const Look& before, std::optional<Range> except, const std::set<std::int64_t>& except_paragraphs) {
    Json nt = native_of(record);
    // the paragraphs: what differs laid on each as its own (a paragraph's para comes last)
    {
        auto entries = paragraph_entries(nt);
        bool changed = false;
        for (std::size_t p = 0; p < before.paras.size(); ++p) {
            const auto index = static_cast<std::int64_t>(p);
            if (except_paragraphs.contains(index)) continue;
            const Json now = type_text::resolve_para(record, styles, index);
            const Json lay = differing(before.paras[p], now);
            if (lay.empty()) continue;
            Json& item = entry_at(entries, index);
            Json own = item.value("para", Json::object());
            for (const auto& [key, value] : lay.items()) own[key] = value;
            item["para"] = std::move(own);
            changed = true;
        }
        if (changed) {
            put_paragraphs(nt, entries);
            set_native(record, nt);
            nt = native_of(record);
        }
    }
    // the characters: each stretch where they differ given its differences as a range's own (a range's char comes last)
    const std::vector<type_text::CharRun> now = type_text::resolve_chars(record, styles);
    const std::int64_t n = ruby::count(record.text);
    if (n == 0) return;
    std::vector<std::int64_t> edges{0, n};
    for (const auto* runs : {&before.chars, &now}) {
        for (const type_text::CharRun& r : *runs) edges.push_back(std::clamp<std::int64_t>(r.start, 0, n));
    }
    if (except) {
        edges.push_back(std::clamp<std::int64_t>(except->start, 0, n));
        edges.push_back(std::clamp<std::int64_t>(except->end, 0, n));
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    Json runs = nt.value("runs", Json::array());
    bool changed = false;
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        const std::int64_t s = edges[i], e = edges[i + 1];
        if (except && s >= except->start && e <= except->end && except->start < except->end) continue;
        const Json lay = differing(chars_at(before.chars, s), chars_at(now, s));
        if (lay.empty()) continue;
        runs = lay_runs(runs, s, e, [&](Json& run) {
            Json own = run.value("char", Json::object());
            for (const auto& [key, value] : lay.items()) own[key] = value;
            run["char"] = std::move(own);
        });
        changed = true;
    }
    if (!changed) return;
    if (runs.empty()) nt.erase("runs");
    else nt["runs"] = std::move(runs);
    set_native(record, nt);
}

namespace {

// The text's paragraphs, split once (review P2-4: a text of thousands of paragraphs asked of each): where each begins, and
// how many graphemes the text has.
struct Paragraphs {
    std::vector<std::int64_t> starts;
    std::int64_t n = 0;
    explicit Paragraphs(std::string_view text) : starts(type_text::paragraph_starts_of(text)), n(ruby::count(text)) {}
    std::int64_t count() const { return static_cast<std::int64_t>(starts.size()); }
    // the paragraph grapheme `at` is in
    std::int64_t of(std::int64_t at) const {
        return static_cast<std::int64_t>(std::upper_bound(starts.begin(), starts.end(), at) - starts.begin()) - 1;
    }
    Range span(std::int64_t p) const {
        return Range{starts[static_cast<std::size_t>(p)], p + 1 < count() ? starts[static_cast<std::size_t>(p + 1)] : n};
    }
    // those [start, end) touches (an empty range: the one it is in): a paragraph's line break counts as its own
    std::vector<std::int64_t> touched(std::int64_t start, std::int64_t end) const {
        const std::int64_t first = std::max<std::int64_t>(0, of(std::max<std::int64_t>(0, start) - 1) );
        std::vector<std::int64_t> out;
        for (std::int64_t p = first; p < count(); ++p) {
            const std::int64_t s = starts[static_cast<std::size_t>(p)];
            const std::int64_t last = p + 1 < count() ? starts[static_cast<std::size_t>(p + 1)] - 1 : n;  // (its line break's place)
            if (s > std::max(start, end - 1)) break;
            if (start <= last) out.push_back(p);
        }
        return out;
    }
};

// The entry of paragraph `p` in native_type's paragraphs (in order of their index: halved), or none.
const Json* entry_of(const Json& nt, std::int64_t p) {
    const Json* ps = get(nt, "paragraphs");
    if (ps == nullptr || !ps->is_array() || ps->empty()) return nullptr;
    const auto at = std::lower_bound(ps->begin(), ps->end(), p, [](const Json& item, std::int64_t i) { return int_at(item, "index") < i; });
    return at != ps->end() && int_at(*at, "index") == p ? &*at : nullptr;
}

// The paragraph style of paragraph `p` (its own, else the whole text's, else Basic Paragraph).
std::string paragraph_style_of(const StoryLine& record, std::int64_t p) {
    if (record.native_type && record.native_type->is_object()) {
        const Json& nt = *record.native_type;
        if (const Json* item = entry_of(nt, p); item != nullptr && item->contains("style")) return (*item)["style"].get<std::string>();
        if (const Json* whole = get(nt, "para_style"); whole != nullptr && whole->is_string()) return whole->get<std::string>();
    }
    return std::string(kBasic);
}

// Does the record's native_type set it by the OpenType typesetting (render::text::sets_opentype's own part)? A record that
// was keeps it (review P2-2: a style taken away, the text set as before — never the typesetting of before).
bool opentype_record(const StoryLine& record) {
    if (!record.native_type || !record.native_type->is_object()) return false;
    for (const char* key : {"shaping", "char", "para", "runs", "paragraphs", "char_style", "para_style"}) {
        if (record.native_type->contains(key)) return true;
    }
    return false;
}
void keep_shaping(StoryLine& record, bool was) {
    if (!was || (record.native_type && record.native_type->contains("shaping"))) return;
    Json nt = native_of(record);
    nt["shaping"] = "opentype";
    set_native(record, nt);
}

}  // namespace

std::vector<std::int64_t> paragraphs_of(std::string_view text, std::int64_t start, std::int64_t end) { return Paragraphs(text).touched(start, end); }

Range paragraph_span(std::string_view text, std::int64_t p) { return Paragraphs(text).span(p); }

namespace {
void restyle_inner(StoryLine& record, const Json* styles, Kind kind, std::optional<Range> range, const std::optional<std::optional<std::string>>& set,
                   bool clear);
}  // namespace

void restyle(StoryLine& record, const Json* styles, Kind kind, std::optional<Range> range, const std::optional<std::optional<std::string>>& set,
             bool clear) {
    const bool was = opentype_record(record);
    restyle_inner(record, styles, kind, range, set, clear);
    keep_shaping(record, was);
}

namespace {
void restyle_inner(StoryLine& record, const Json* styles, Kind kind, std::optional<Range> range, const std::optional<std::optional<std::string>>& set,
                   bool clear) {
    const std::int64_t n = ruby::count(record.text);
    if (range && (range->start < 0 || range->end > n || range->end < range->start))
        throw OpError("range must lie in the text, its end not before its start (0.." + std::to_string(n) + " graphemes)");
    const bool whole = !range || (range->start <= 0 && range->end >= n && n > 0) || (n == 0);
    std::optional<std::string> id;  // (the style given: none for なし / 基本段落)
    if (set && *set && !(kind == Kind::Paragraph && **set == kBasic)) id = **set;
    Json nt = native_of(record);
    if (kind == Kind::Character) {
        if (whole) {
            if (set) {
                if (id) nt["char_style"] = *id;
                else nt.erase("char_style");
            }
            if (clear) nt.erase("char");
            if (nt.contains("runs")) {
                nt["runs"] = lay_runs(nt["runs"], 0, n, [&](Json& run) {
                    if (set) run.erase("style");
                    if (clear) run.erase("char");
                });
                if (nt["runs"].empty()) nt.erase("runs");
            }
            set_native(record, nt);
            return;
        }
        if (range->start == range->end) throw OpError("an empty range takes a paragraph style only");
        const Look before = look_of(record, styles);
        // (one character style for each letter, as Photoshop's runs have: the whole text's laid on its ranges first)
        if (set && nt.contains("char_style")) {
            const Json whole_style = nt["char_style"];
            nt["runs"] = lay_runs(nt.value("runs", Json::array()), 0, n, [&](Json& run) {
                if (!run.contains("style")) run["style"] = whole_style;
            });
            nt.erase("char_style");
        }
        if (clear) nt.erase("char");
        set_native(record, nt);
        // (the look kept outside the range — review P3-5: inside, what the whole text's style said goes with it)
        keep_look(record, styles, before, *range);
        nt = native_of(record);
        // (applied without clearing: the whole text's own attributes still win over the new style there, as Photoshop keeps
        // the overrides — the style's attributes the whole text says otherwise laid on the range as its own; review P3-5)
        Json keep = Json::object();
        if (id && !clear && nt.contains("char")) {
            const Json says = own_of(find(styles, Kind::Character, *id), "char");
            for (const auto& [key, value] : nt["char"].items()) {
                if (says.contains(key)) keep[key] = value;
            }
        }
        nt["runs"] = lay_runs(nt.value("runs", Json::array()), range->start, range->end, [&](Json& run) {
            if (set) {
                if (id) run["style"] = *id;
                else run.erase("style");
            }
            if (clear) run.erase("char");
            if (!keep.empty()) {
                Json own = run.value("char", Json::object());
                for (const auto& [key, value] : keep.items()) {
                    if (!own.contains(key)) own[key] = value;
                }
                run["char"] = std::move(own);
            }
        });
        if (nt["runs"].empty()) nt.erase("runs");
        set_native(record, nt);
        return;
    }
    // paragraphs
    if (whole) {
        if (set) {
            if (id) nt["para_style"] = *id;
            else nt.erase("para_style");
        }
        auto entries = paragraph_entries(nt);
        for (auto& [index, item] : entries) {
            if (set) item.erase("style");
            if (clear) item.erase("para");
        }
        put_paragraphs(nt, entries);
        if (clear) {
            nt.erase("para");
            nt.erase("char");
            if (nt.contains("runs")) {
                nt["runs"] = lay_runs(nt["runs"], 0, n, [](Json& run) { run.erase("char"); });
                if (nt["runs"].empty()) nt.erase("runs");
            }
        }
        set_native(record, nt);
        return;
    }
    const std::vector<std::int64_t> touched = paragraphs_of(record.text, range->start, range->end);
    const Look before = look_of(record, styles);
    const std::int64_t count = type_text::paragraph_count(record.text);
    auto entries = paragraph_entries(nt);
    // (one paragraph style for each paragraph: the whole text's laid on its paragraphs first; its own paragraph attributes
    // too when they are cleared somewhere — a paragraph's own come after the whole's, so the look stays)
    if (set && nt.contains("para_style")) {
        for (std::int64_t p = 0; p < count; ++p) {
            Json& item = entry_at(entries, p);
            if (!item.contains("style")) item["style"] = nt["para_style"];
        }
        nt.erase("para_style");
    }
    if (clear && nt.contains("para")) {
        for (std::int64_t p = 0; p < count; ++p) {
            Json& item = entry_at(entries, p);
            Json own = nt["para"];
            const Json mine = item.value("para", Json::object());
            for (const auto& [key, value] : mine.items()) own[key] = value;
            item["para"] = std::move(own);
        }
        nt.erase("para");
    }
    for (const std::int64_t p : touched) {
        Json& item = entry_at(entries, p);
        if (set) {
            if (id) item["style"] = *id;
            else item.erase("style");
        }
        if (clear) item.erase("para");
    }
    put_paragraphs(nt, entries);
    if (!clear) {
        set_native(record, nt);
        return;
    }
    // (cleared: the characters of the paragraphs touched lose their own attributes too)
    const Range span{paragraph_span(record.text, touched.front()).start, paragraph_span(record.text, touched.back()).end};
    nt.erase("char");
    set_native(record, nt);
    std::set<std::int64_t> except(touched.begin(), touched.end());
    keep_look(record, styles, before, span, except);
    nt = native_of(record);
    if (nt.contains("runs")) {
        nt["runs"] = lay_runs(nt["runs"], span.start, span.end, [](Json& run) { run.erase("char"); });
        if (nt["runs"].empty()) nt.erase("runs");
    }
    set_native(record, nt);
}
}  // namespace

bool takes(const StoryLine& record, Kind kind, std::string_view id) {
    if (kind == Kind::Paragraph && id == kBasic) return true;
    if (!record.native_type || !record.native_type->is_object()) return false;
    const Json& nt = *record.native_type;
    const Json want(std::string{id});
    if (kind == Kind::Character) {
        if (nt.value("char_style", Json()) == want) return true;
        if (const Json* runs = get(nt, "runs"); runs != nullptr && runs->is_array()) {
            for (const Json& run : *runs) {
                if (run.value("style", Json()) == want) return true;
            }
        }
        return false;
    }
    if (nt.value("para_style", Json()) == want) return true;
    if (const Json* ps = get(nt, "paragraphs"); ps != nullptr && ps->is_array()) {
        for (const Json& p : *ps) {
            if (p.value("style", Json()) == want) return true;
        }
    }
    return false;
}

bool take_away(StoryLine& record, Kind kind, std::string_view id, const Json* before, const Json* after) {
    if (!takes(record, kind, id) || (kind == Kind::Paragraph && id == kBasic)) return false;
    const bool was = opentype_record(record);
    const Look look = look_of(record, before);
    Json nt = native_of(record);
    const Json want(std::string{id});
    if (kind == Kind::Character) {
        if (nt.value("char_style", Json()) == want) nt.erase("char_style");
        if (nt.contains("runs")) {
            for (Json& run : nt["runs"]) {
                if (run.value("style", Json()) == want) run.erase("style");
            }
        }
    } else {
        if (nt.value("para_style", Json()) == want) nt.erase("para_style");
        auto entries = paragraph_entries(nt);
        for (auto& [index, item] : entries) {
            if (item.value("style", Json()) == want) item.erase("style");
        }
        put_paragraphs(nt, entries);
    }
    set_native(record, nt);
    keep_look(record, after, look);
    keep_shaping(record, was);
    return true;
}

std::optional<std::string> character_style_at(const StoryLine& record, std::int64_t at) {
    if (!record.native_type || !record.native_type->is_object()) return std::nullopt;
    const Json& nt = *record.native_type;
    if (const Json* runs = get(nt, "runs"); runs != nullptr && runs->is_array() && !runs->empty()) {
        // (the runs in order, none overlapping: the last that starts at or before it — halved)
        auto it = std::upper_bound(runs->begin(), runs->end(), at, [](std::int64_t g, const Json& run) { return g < int_at(run, "start"); });
        if (it != runs->begin()) {
            const Json& run = *std::prev(it);
            if (at < int_at(run, "end") && run.contains("style")) return run["style"].get<std::string>();
        }
    }
    if (const Json* whole = get(nt, "char_style"); whole != nullptr && whole->is_string()) return whole->get<std::string>();
    return std::nullopt;
}

std::string paragraph_style_at(const StoryLine& record, std::int64_t at) {
    return paragraph_style_of(record, std::max<std::int64_t>(0, Paragraphs(record.text).of(at)));
}

Json current(const StoryLine& record, const Json* styles, std::optional<Range> range) {
    const Paragraphs paragraphs(record.text);
    const std::int64_t n = paragraphs.n;
    std::int64_t s = range ? std::clamp<std::int64_t>(range->start, 0, n) : 0;
    std::int64_t e = range ? std::clamp<std::int64_t>(range->end, s, n) : n;
    const std::vector<std::int64_t> touched = paragraphs.touched(s, e);
    if (s == e && n > 0) {  // (an empty range: the letter at it — at the end, the one before it)
        if (s >= n) s = n - 1;
        e = s + 1;
    }
    // the character styles over the range: each range's, else the whole's
    std::set<std::string> chars;
    bool none = false;
    if (n == 0) {
        const std::optional<std::string> id = character_style_at(record, 0);
        if (id) chars.insert(*id);
        else none = true;
    } else {
        std::vector<std::int64_t> edges{s, e};
        if (record.native_type && record.native_type->contains("runs")) {
            for (const Json& run : (*record.native_type)["runs"]) {
                for (const char* key : {"start", "end"}) {
                    const std::int64_t at = int_at(run, key);
                    if (at > s && at < e) edges.push_back(at);
                }
            }
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
            const std::optional<std::string> id = character_style_at(record, edges[i]);
            if (id) chars.insert(*id);
            else none = true;
        }
    }
    const std::size_t char_kinds = chars.size() + (none ? 1 : 0);
    Json out = Json::object();
    out["character"] = char_kinds == 1 && !none ? Json(*chars.begin()) : Json(nullptr);
    out["character_mixed"] = char_kinds > 1;
    out["character_override"] =
        chars_differ(type_text::resolve_chars(record, styles), type_text::resolve_chars(without_own(record, Kind::Character), styles), s, e);
    // the paragraph styles of the paragraphs it touches
    std::set<std::string> paras;
    bool para_override = false;
    const StoryLine bare = without_own(record, Kind::Paragraph);
    for (const std::int64_t p : touched) {
        paras.insert(paragraph_style_of(record, p));
        if (type_text::resolve_para(record, styles, p) != type_text::resolve_para(bare, styles, p)) para_override = true;
    }
    out["paragraph"] = paras.size() == 1 ? Json(*paras.begin()) : Json(nullptr);
    out["paragraph_mixed"] = paras.size() > 1;
    out["paragraph_override"] = para_override;
    return out;
}

Json formatting_at(const StoryLine& record, const Json* styles, Kind kind, std::int64_t at) {
    const std::int64_t n = ruby::count(record.text);
    at = std::clamp<std::int64_t>(at, 0, std::max<std::int64_t>(0, n - 1));
    // (what the book gives with nothing of the text's own — no style of its own but, kind Character, its paragraph's —
    // and not the line's style either: a style made of a text set in a face takes the face)
    StoryLine bare = record;
    bare.style = Json::object();
    bare.style_runs.clear();
    Json nt = native_of(record);
    for (const char* key : {"char_style", "char", "runs"}) nt.erase(key);
    if (kind == Kind::Paragraph) {
        for (const char* key : {"para_style", "para", "paragraphs"}) nt.erase(key);
    }
    bare.native_type = nt.empty() ? std::nullopt : std::optional<Json>(nt);
    const std::vector<std::int64_t> ps = paragraphs_of(record.text, at, at);
    const std::int64_t p = ps.empty() ? 0 : ps.front();
    Json out = Json::object();
    out["char"] = differing(chars_at(type_text::resolve_chars(record, styles), at), chars_at(type_text::resolve_chars(bare, styles), at));
    out["para"] = kind == Kind::Paragraph ? differing(type_text::resolve_para(record, styles, p), type_text::resolve_para(bare, styles, p)) : Json::object();
    return out;
}

Range stretch_at(const StoryLine& record, const Json* styles, std::int64_t at) {
    const std::vector<type_text::CharRun> runs = type_text::resolve_chars(record, styles);
    for (const type_text::CharRun& r : runs) {
        if (r.start <= at && at < r.end) return Range{r.start, r.end};
    }
    return runs.empty() ? Range{0, 0} : Range{runs.back().start, runs.back().end};
}

Json redefinition_at(const StoryLine& record, const Json* styles, Kind kind, std::string_view id, std::int64_t at) {
    const std::int64_t n = ruby::count(record.text);
    at = std::clamp<std::int64_t>(at, 0, std::max<std::int64_t>(0, n - 1));
    const std::optional<std::optional<std::string>> set = std::optional<std::string>(std::string(id));
    StoryLine bare = record;
    Json out = Json::object();
    if (kind == Kind::Character) {
        const Range span = stretch_at(record, styles, at);
        if (span.end > span.start) restyle(bare, styles, kind, span, set, true);
        else restyle(bare, styles, kind, std::nullopt, set, true);
        out["char"] = differing(chars_at(type_text::resolve_chars(record, styles), at), chars_at(type_text::resolve_chars(bare, styles), at));
        out["para"] = Json::object();
        return out;
    }
    const std::vector<std::int64_t> ps = paragraphs_of(record.text, at, at);
    const std::int64_t p = ps.empty() ? 0 : ps.front();
    restyle(bare, styles, kind, Range{at, at}, set, true);
    Json chars = differing(chars_at(type_text::resolve_chars(record, styles), at), chars_at(type_text::resolve_chars(bare, styles), at));
    // (what a character style there sets is that style's, not the paragraph style's)
    if (record.native_type) {
        const Json& nt = *record.native_type;
        std::vector<const Json*> char_styles{find(styles, Kind::Character, nt.value("char_style", std::string()))};
        if (const std::optional<std::string> run_style = character_style_at(record, at)) char_styles.push_back(find(styles, Kind::Character, *run_style));
        for (const Json* style : char_styles) {
            const Json own = own_of(style, "char");
            for (const auto& [key, value] : own.items()) chars.erase(key);
        }
    }
    out["char"] = std::move(chars);
    out["para"] = differing(type_text::resolve_para(record, styles, p), type_text::resolve_para(bare, styles, p));
    return out;
}

void drop_same(StoryLine& record, const Json* styles, Kind kind, Range range, const Json& values) {
    if (!record.native_type || values.empty()) return;
    const bool was = opentype_record(record);
    const Paragraphs paragraphs(record.text);
    Json nt = *record.native_type;
    // (review P2-3: the whole text's own too — over the whole text taken away; over part of it, laid on the rest first)
    const char* whole_key = kind == Kind::Character ? "char" : "para";
    Json same = Json::object();
    if (const Json* own = get(nt, whole_key); own != nullptr && own->is_object()) {
        for (const auto& [key, value] : values.items()) {
            if (own->contains(key) && (*own)[key] == value) same[key] = value;
        }
    }
    if (!same.empty()) {
        if (kind == Kind::Character) {
            const bool everything = range.start <= 0 && range.end >= paragraphs.n;
            const Look before = look_of(record, styles);
            for (const auto& [key, value] : same.items()) nt[whole_key].erase(key);
            if (nt[whole_key].empty()) nt.erase(whole_key);
            set_native(record, nt);
            if (!everything) keep_look(record, styles, before, range);  // (the rest as it was: those values its ranges' own)
            nt = native_of(record);
        } else {
            const std::vector<std::int64_t> touched = paragraphs.touched(range.start, range.end);
            if (static_cast<std::int64_t>(touched.size()) < paragraphs.count()) {
                auto entries = paragraph_entries(nt);
                const std::set<std::int64_t> in(touched.begin(), touched.end());
                for (std::int64_t p = 0; p < paragraphs.count(); ++p) {
                    if (in.contains(p)) continue;
                    Json& item = entry_at(entries, p);
                    Json own = item.value("para", Json::object());
                    for (const auto& [key, value] : same.items()) {
                        if (!own.contains(key)) own[key] = value;
                    }
                    item["para"] = std::move(own);
                }
                put_paragraphs(nt, entries);
            }
            for (const auto& [key, value] : same.items()) nt[whole_key].erase(key);
            if (nt[whole_key].empty()) nt.erase(whole_key);
        }
    }
    if (kind == Kind::Character) {
        if (!nt.contains("runs")) {
            set_native(record, nt);
            keep_shaping(record, was);
            return;
        }
        Json runs = Json::array();
        for (Json run : nt["runs"]) {
            if (int_at(run, "end") <= range.start || int_at(run, "start") >= range.end || !run.contains("char")) {
                runs.push_back(std::move(run));
                continue;
            }
            // (only where the range reaches: a run reaching past it is cut there)
            Json cut = lay_runs(Json::array({run}), std::max(range.start, int_at(run, "start")), std::min(range.end, int_at(run, "end")), [&](Json& piece) {
                Json own = piece.value("char", Json::object());
                for (const auto& [key, value] : values.items()) {
                    if (own.value(key, Json()) == value) own.erase(key);
                }
                piece["char"] = std::move(own);
            });
            for (Json& piece : cut) runs.push_back(std::move(piece));
        }
        if (runs.empty()) nt.erase("runs");
        else nt["runs"] = std::move(runs);
    } else {
        auto entries = paragraph_entries(nt);
        for (const std::int64_t p : paragraphs.touched(range.start, range.end)) {
            const auto it = entries.find(p);
            if (it == entries.end() || !it->second.contains("para")) continue;
            Json& own = it->second["para"];
            for (const auto& [key, value] : values.items()) {
                if (own.value(key, Json()) == value) own.erase(key);
            }
        }
        put_paragraphs(nt, entries);
    }
    set_native(record, nt);
    keep_shaping(record, was);
}

Json defaults_of(const StoryLine& record, const Json* styles) {
    const std::vector<type_text::CharRun> chars = type_text::resolve_chars(record, styles);
    Json out = Json::object({{"char", Json::object()}, {"para", Json::object()}});
    for (const auto& [key, value] : chars.front().attrs.items()) {
        if (!value.is_null()) out["char"][key] = value;
    }
    const Json para = type_text::resolve_para(record, styles, 0);
    for (const auto& [key, value] : para.items()) {
        if (!value.is_null()) out["para"][key] = value;
    }
    return out;
}

Json checked_defaults(const Json& saved) {
    if (!saved.is_object()) return nullptr;
    try {
        Json out = Json::object();
        out["char"] = type_text::check_char(saved.value("char", Json::object()));
        out["para"] = type_text::check_para(saved.value("para", Json::object()));
        if (out["char"].empty() && out["para"].empty()) return nullptr;
        return out;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::vector<const StoryLine*> texts(const Document& doc) {
    std::vector<const StoryLine*> out;
    for (const StoryLine& line : doc.story) out.push_back(&line);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (type_text::is_type_layer(layer) && layer.type) out.push_back(layer.type.get());
        }
    }
    return out;
}

Json list(const Document& doc, Kind kind) {
    const Json* styles = doc.type_styles ? &*doc.type_styles : nullptr;
    const std::vector<const StoryLine*> all = texts(doc);
    const auto count = [&](std::string_view id) {
        return static_cast<std::int64_t>(std::count_if(all.begin(), all.end(), [&](const StoryLine* r) {
            // (Basic Paragraph: the texts whose paragraphs take no style of their own)
            if (kind == Kind::Paragraph && id == kBasic) return r->native_type.has_value() && paragraph_style_of(*r, 0) == kBasic;
            return takes(*r, kind, id);
        }));
    };
    // (a style's attributes resolved over Basic Paragraph: a text of one letter that takes it)
    const auto resolved = [&](const Json& nt) {
        StoryLine probe;
        probe.text = "a";
        probe.style = Json::object();
        if (!nt.is_null()) probe.native_type = nt;
        return probe;
    };
    Json out = Json::array();
    if (kind == Kind::Character) {
        out.push_back(Json{{"id", nullptr}, {"name", std::string(kNoneName)}});
        if (styles != nullptr && styles->contains("character")) {
            for (const Json& s : (*styles)["character"]) {
                const StoryLine probe = resolved(Json{{"char_style", s["id"]}});
                out.push_back(Json{{"id", s["id"]}, {"name", s["name"]}, {"char", s.value("char", Json::object())},
                                   {"resolved", type_text::resolve_chars(probe, styles).front().attrs}, {"texts", count(s["id"].get<std::string>())}});
            }
        }
        return out;
    }
    const auto entry = [&](const Json& id, const Json& name, const Json& style, const Json& nt) {
        const StoryLine probe = resolved(nt);
        return Json{{"id", id}, {"name", name}, {"para", style.value("para", Json::object())}, {"char", style.value("char", Json::object())},
                    {"resolved", Json{{"para", type_text::resolve_para(probe, styles, 0)}, {"char", type_text::resolve_chars(probe, styles).front().attrs}}},
                    {"texts", count(id.get<std::string>())}};
    };
    static const Json kNoStyles = Json::object();
    out.push_back(entry(Json(std::string(kBasic)), Json(std::string(kBasicName)), *find(styles != nullptr ? styles : &kNoStyles, Kind::Paragraph, kBasic),
                        nullptr));
    if (styles != nullptr && styles->contains("paragraph")) {
        for (const Json& s : (*styles)["paragraph"]) out.push_back(entry(s["id"], s["name"], s, Json{{"para_style", s["id"]}}));
    }
    return out;
}

}  // namespace genko::core::type_styles
