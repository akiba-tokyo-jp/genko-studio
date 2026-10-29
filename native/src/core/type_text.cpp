// PC5 G-0 (type@1, core/type_text.hpp): the record of Photoshop's type on a line and a type layer — its attributes'
// tables, checks and normal form, how they are resolved and how their ranges follow an edit of the text.

#include "core/type_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "core/vector.hpp"

namespace genko::core::type_text {

namespace {

[[noreturn]] void bad(const std::string& message) { throw Error("value", message); }

constexpr std::array<std::string_view, 3> kBoldLevels{"normal", "bold", "heavy"};
constexpr std::array<std::string_view, 3> kCaps{"normal", "small", "all"};
constexpr std::array<std::string_view, 3> kKerning{"metrics", "optical", "none"};
constexpr std::array<std::string_view, 10> kAligns{"left",   "center", "right", "justify", "justify_center", "justify_right",
                                                   "justify_all", "top", "bottom", "start"};
constexpr std::array<std::string_view, 4> kKinsoku{"genko", "none", "weak", "strong"};
constexpr std::array<std::string_view, 3> kDirections{"auto", "ltr", "rtl"};
constexpr std::array<std::string_view, 2> kComposers{"japanese", "world"};
constexpr std::array<std::string_view, 7> kAntialias{"none", "sharp", "crisp", "strong", "smooth", "windowsLcd", "windows"};
constexpr std::array<std::string_view, 2> kShapes{"point", "box"};

constexpr double kMaxMm = 1000.0;  // a length (a page is at most 1 m: the paper's bound)

const std::array<Attr, 25> kCharAttrs{{
    {"font", AttrKind::Name, 0, 4096},
    {"family", AttrKind::Name, 0, 256},
    {"font_style", AttrKind::Name, 0, 256},
    {"postscript", AttrKind::Name, 0, 256},
    {"size_mm", AttrKind::Number, 0.1, kMaxMm},
    {"scale", AttrKind::Number, 0.1, 10},  // (the size against the line's: Genko's style_runs' scale)
    {"rgb", AttrKind::Rgb},
    {"tracking", AttrKind::Number, -1, 10},
    {"leading_mm", AttrKind::Leading, 0, kMaxMm},
    {"baseline_shift_mm", AttrKind::Number, -kMaxMm, kMaxMm},
    {"scale_x", AttrKind::Number, 0.01, 10},
    {"scale_y", AttrKind::Number, 0.01, 10},
    {"faux_bold", AttrKind::Choice, 0, 0, kBoldLevels},
    {"faux_italic", AttrKind::Bool},
    {"underline", AttrKind::Bool},
    {"strikethrough", AttrKind::Bool},
    {"caps", AttrKind::Choice, 0, 0, kCaps},
    {"kerning", AttrKind::Choice, 0, 0, kKerning},
    {"ligatures", AttrKind::Bool},
    {"discretionary_ligatures", AttrKind::Bool},
    {"features", AttrKind::Tags},
    {"variations", AttrKind::Axes},
    {"language", AttrKind::Language},
    {"proportional", AttrKind::Bool},
    {"tcy", AttrKind::Bool},
}};

const std::array<Attr, 12> kParaAttrs{{
    {"align", AttrKind::Choice, 0, 0, kAligns},
    {"first_indent_mm", AttrKind::Number, -kMaxMm, kMaxMm},
    {"start_indent_mm", AttrKind::Number, -kMaxMm, kMaxMm},
    {"end_indent_mm", AttrKind::Number, -kMaxMm, kMaxMm},
    {"space_before_mm", AttrKind::Number, 0, kMaxMm},
    {"space_after_mm", AttrKind::Number, 0, kMaxMm},
    {"auto_leading", AttrKind::Number, 0, 5},
    {"kinsoku", AttrKind::Choice, 0, 0, kKinsoku},
    {"yakumono", AttrKind::Bool},
    {"hyphenate", AttrKind::Bool},
    {"direction", AttrKind::Choice, 0, 0, kDirections},
    {"composer", AttrKind::Choice, 0, 0, kComposers},
}};

constexpr std::size_t kMaxTags = 64;
constexpr std::size_t kMaxRuns = 4096;
constexpr std::size_t kMaxStyles = 512;
constexpr std::size_t kMaxStyleName = 64;
constexpr std::int64_t kMaxEditSteps = 2048;  // (edits_between: the differences Myers' script reads, at most)

// A number as the normal form keeps it: 4 decimals, a whole one as an integer.
Json num(double v) {
    const double r = std::round(v * 10000.0) / 10000.0;
    if (r == std::floor(r) && std::abs(r) < 1e15) return Json(static_cast<std::int64_t>(r));
    return Json(r);
}

double number_of(const Json& v, const std::string& what, double lo, double hi) {
    if (!v.is_number() || v.is_boolean()) bad(what + " must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) bad(what + " must be between " + py_float_repr(lo) + " and " + py_float_repr(hi));
    return d;
}

bool tag_char(char c, bool space) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (space && c == ' ');
}

std::string joined(std::span<const std::string_view> names) {
    std::string out;
    for (const std::string_view n : names) out += (out.empty() ? "" : ", ") + std::string(n);
    return out;
}

// One attribute's value in its normal form.
Json check_value(const Attr& attr, const Json& v, const std::string& what) {
    switch (attr.kind) {
        case AttrKind::Number: return num(number_of(v, what, attr.lo, attr.hi));
        case AttrKind::Leading:
            if (v == Json("auto")) return v;
            return num(number_of(v, what, attr.lo, attr.hi));
        case AttrKind::Bool:
            if (!v.is_boolean()) bad(what + " must be true or false");
            return v;
        case AttrKind::Choice: {
            if (attr.name == "faux_bold" && v.is_boolean()) return Json(v.get<bool>() ? "bold" : "normal");  // (Photoshop's toggle)
            if (!v.is_string() || std::find(attr.choices.begin(), attr.choices.end(), v.get_ref<const std::string&>()) == attr.choices.end())
                bad(what + " must be one of " + joined(attr.choices));
            return v;
        }
        case AttrKind::Name: {
            if (!v.is_string() || v.get_ref<const std::string&>().empty() || ruby::count(v.get_ref<const std::string&>()) > static_cast<std::int64_t>(attr.hi))
                bad(what + " must be a name of 1 to " + std::to_string(static_cast<int>(attr.hi)) + " characters");
            return v;
        }
        case AttrKind::Rgb: {
            if (!v.is_array() || v.size() != 3) bad(what + " must be three integers 0..255");
            Json out = Json::array();
            for (const Json& c : v) {
                if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) bad(what + " must be three integers 0..255");
                out.push_back(c.get<std::int64_t>());
            }
            return out;
        }
        case AttrKind::Tags:
        case AttrKind::Axes: {
            const bool tags = attr.kind == AttrKind::Tags;
            // (Genko's style.features, a list of tags, is taken as each of them on)
            Json given = v;
            if (tags && v.is_array()) {
                given = Json::object();
                for (const Json& t : v) {
                    if (!t.is_string()) bad(what + " must be {tag: value}");
                    given[t.get<std::string>()] = 1;
                }
            }
            if (!given.is_object()) bad(what + (tags ? " must be {tag: value}" : " must be {axis: value}"));
            if (given.size() > kMaxTags) bad(what + " holds 64 at most");
            std::vector<std::pair<std::string, Json>> items;
            for (const auto& [key, value] : given.items()) {
                if (key.size() != 4 || !std::all_of(key.begin(), key.end(), [&](char c) { return tag_char(c, tags); }))
                    bad(what + ": " + py_repr_str(key) + " is not a tag of 4 letters");
                if (tags) {
                    if (!value.is_number_integer() || value.get<std::int64_t>() < 0 || value.get<std::int64_t>() > 65535)
                        bad(what + " " + key + " must be an integer 0..65535");
                    items.emplace_back(key, value.get<std::int64_t>());
                } else {
                    items.emplace_back(key, num(number_of(value, what + " " + key, -32768, 32767)));
                }
            }
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            Json out = Json::object();
            for (auto& [key, value] : items) out[key] = std::move(value);
            return out;
        }
        case AttrKind::Language: {
            if (!v.is_string()) bad(what + " must be a language tag (BCP 47)");
            const std::string& s = v.get_ref<const std::string&>();
            if (s.empty() || s.size() > 35 || !std::all_of(s.begin(), s.end(), [](char c) { return tag_char(c, false) || c == '-'; }))
                bad(what + " must be a language tag (BCP 47)");
            return v;
        }
    }
    return v;
}

Json check_attrs(const Json& raw, std::span<const Attr> table, std::string_view where) {
    if (!raw.is_object()) bad(std::string(where) + " must be an object");
    for (const auto& [key, value] : raw.items()) {
        if (std::none_of(table.begin(), table.end(), [&](const Attr& a) { return a.name == key; }))
            bad("unknown " + std::string(where) + " attribute " + key);
    }
    Json out = Json::object();
    for (const Attr& attr : table) {
        const auto it = raw.find(std::string(attr.name));
        if (it == raw.end() || it->is_null()) continue;
        out[std::string(attr.name)] = check_value(attr, *it, std::string(where) + " " + std::string(attr.name));
    }
    return out;
}

// A style's id: its kind's prefix and a number from 1.
bool style_id(const Json& v, std::string_view prefix) {
    if (!v.is_string()) return false;
    const std::string& s = v.get_ref<const std::string&>();
    if (!s.starts_with(prefix) || s.size() == prefix.size() || s.size() > prefix.size() + 9) return false;
    if (s[prefix.size()] == '0') return false;
    return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(prefix.size()), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

Json check_style_ref(const Json& v, std::string_view prefix, const std::string& what) {
    if (!style_id(v, prefix)) bad(what + " must be a style's id (" + std::string(prefix) + "1, " + std::string(prefix) + "2, …)");
    return v;
}

// The graphemes of a text, each as its code points.
std::vector<std::u32string> graphemes(std::string_view utf8) {
    const std::u32string cps = ruby::code_points(utf8);
    const std::vector<std::size_t> b = ruby::bounds(cps);
    std::vector<std::u32string> out;
    for (std::size_t i = 0; i + 1 < b.size(); ++i) out.emplace_back(cps.substr(b[i], b[i + 1] - b[i]));
    return out;
}

// Where each paragraph begins (in graphemes).
std::vector<std::int64_t> paragraph_starts(std::string_view text) {
    std::vector<std::int64_t> out{0};
    const std::vector<std::u32string> g = graphemes(text);
    for (std::size_t i = 0; i < g.size(); ++i) {
        // (a grapheme holding a line break ends the paragraph: "\r\n" is one grapheme)
        if (g[i].find(U'\n') != std::u32string::npos) out.push_back(static_cast<std::int64_t>(i + 1));
    }
    return out;
}

// Places of the text before through its edits (left to right, none overlapping), each found by a binary search: the edits
// wholly before a place move it by their lengths' change (their running sum), the one around it decides the rest.
//  - a range's start: an edit before it (an insertion just before it included) moves it; one around it takes it to the end
//    of what the edit put in;
//  - a range's end: an edit before it moves it (an insertion at it does not: one at a range's end is not in it); one around
//    it takes it to the edit's start (what the edit put in is not the range's);
//  - a paragraph's start: an edit before it moves it (an insertion at it does not); one around it takes it away (-1).
class Places {
public:
    explicit Places(std::vector<Edit> edits) : edits_(std::move(edits)) {
        std::sort(edits_.begin(), edits_.end(), [](const Edit& x, const Edit& y) { return x.start < y.start; });
        sums_.assign(edits_.size() + 1, 0);
        for (std::size_t i = 0; i < edits_.size(); ++i) sums_[i + 1] = sums_[i] + edits_[i].inserted - (edits_[i].end - edits_[i].start);
    }
    std::int64_t start(std::int64_t a) const {
        const std::size_t i = before(a, [](const Edit& e, std::int64_t p) { return e.end <= p; });
        if (i < edits_.size() && edits_[i].start <= a) return edits_[i].start + edits_[i].inserted + sums_[i];
        return a + sums_[i];
    }
    std::int64_t end(std::int64_t b) const {
        const std::size_t i = before(b, [](const Edit& e, std::int64_t p) { return e.end < p; });
        if (i < edits_.size() && edits_[i].start < b) return edits_[i].start + sums_[i];
        return b + sums_[i];
    }
    std::int64_t paragraph(std::int64_t p) const {
        const std::size_t i = before(p, [](const Edit& e, std::int64_t q) { return q >= e.end && q > e.start; });
        if (i < edits_.size() && edits_[i].start < p) return -1;
        return p + sums_[i];
    }

private:
    // How many edits (a prefix of them, in order) the test says lie wholly before the place.
    template <class Test>
    std::size_t before(std::int64_t place, Test test) const {
        std::size_t lo = 0, hi = edits_.size();
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (test(edits_[mid], place)) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    std::vector<Edit> edits_;
    std::vector<std::int64_t> sums_;
};

void merge_into(Json& into, const Json* from) {
    if (from == nullptr || !from->is_object()) return;
    for (const auto& [key, value] : from->items()) into[key] = value;
}

// The paragraphs' entry of paragraph `index` (none: none) — the list in order of their index (the normal form), so halved
// (PC5 G-c review P2-4: thousands of paragraphs resolved one by one).
const Json* paragraph_item(const Json& nt, std::int64_t index) {
    const Json* ps = get(nt, "paragraphs");
    if (ps == nullptr || !ps->is_array() || ps->empty()) return nullptr;
    const auto at = std::lower_bound(ps->begin(), ps->end(), index,
                                     [](const Json& p, std::int64_t i) { return p.value("index", std::int64_t{-1}) < i; });
    if (at != ps->end() && at->value("index", std::int64_t{-1}) == index) return &*at;
    return nullptr;
}

const Json* style_named(const Json* styles, std::string_view kind, const Json* id) {
    if (styles == nullptr || id == nullptr || !id->is_string() || !styles->is_object()) return nullptr;
    const Json* list = get(*styles, kind);
    if (list == nullptr || !list->is_array()) return nullptr;
    for (const Json& s : *list) {
        if (s.is_object() && s.value("id", Json()) == *id) return &s;
    }
    return nullptr;
}

}  // namespace

std::span<const Attr> char_attrs() { return kCharAttrs; }
std::span<const Attr> para_attrs() { return kParaAttrs; }

const Attr* char_attr(std::string_view name) {
    for (const Attr& a : kCharAttrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

const Attr* para_attr(std::string_view name) {
    for (const Attr& a : kParaAttrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

const Json& char_defaults() {
    static const Json defaults = Json::parse(R"({"font": null, "family": null, "font_style": null, "postscript": null,
        "size_mm": null, "scale": 1, "rgb": null, "tracking": 0, "leading_mm": "auto", "baseline_shift_mm": 0, "scale_x": 1,
        "scale_y": 1, "faux_bold": "normal", "faux_italic": false, "underline": false, "strikethrough": false, "caps": "normal",
        "kerning": "metrics", "ligatures": true, "discretionary_ligatures": false, "features": {}, "variations": {},
        "language": "ja", "proportional": false, "tcy": true})");
    return defaults;
}

const Json& para_defaults() {
    static const Json defaults = Json::parse(R"({"align": null, "first_indent_mm": 0, "start_indent_mm": 0, "end_indent_mm": 0,
        "space_before_mm": 0, "space_after_mm": 0, "auto_leading": 1.2, "kinsoku": "genko", "yakumono": true, "hyphenate": false,
        "direction": "auto", "composer": "japanese"})");
    return defaults;
}

std::span<const std::string_view> antialias_methods() { return kAntialias; }

Json check_char(const Json& raw, std::string_view where) { return check_attrs(raw, kCharAttrs, where); }
Json check_para(const Json& raw, std::string_view where) { return check_attrs(raw, kParaAttrs, where); }

std::int64_t paragraph_count(std::string_view text) { return static_cast<std::int64_t>(paragraph_starts(text).size()); }

std::vector<std::int64_t> paragraph_starts_of(std::string_view text) { return paragraph_starts(text); }

std::int64_t paragraph_start(std::string_view text, std::int64_t index) {
    const std::vector<std::int64_t> starts = paragraph_starts(text);
    if (index < 0 || index >= static_cast<std::int64_t>(starts.size())) bad("no paragraph " + std::to_string(index));
    return starts[static_cast<std::size_t>(index)];
}

Json check_native_type(const Json& raw, std::string_view text) {
    if (raw.is_null()) return nullptr;
    if (!raw.is_object()) bad("native_type must be an object");
    static constexpr std::array<std::string_view, 11> kKeys{"shape", "anchor_mm", "antialias", "shaping", "transform", "char_style",
                                                            "para_style", "char", "para", "runs", "paragraphs"};
    for (const auto& [key, value] : raw.items()) {
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) bad("unknown native_type key " + key);
    }
    Json out = Json::object();
    const auto given = [&](std::string_view key) -> const Json* {
        const auto it = raw.find(std::string(key));
        return it == raw.end() || it->is_null() ? nullptr : &*it;
    };
    if (const Json* v = given("shape")) {
        if (!v->is_string() || std::find(kShapes.begin(), kShapes.end(), v->get_ref<const std::string&>()) == kShapes.end())
            bad("native_type shape must be point or box");
        out["shape"] = *v;
    }
    if (const Json* v = given("anchor_mm")) {
        if (!v->is_array() || v->size() != 2) bad("native_type anchor_mm must be [x, y] (mm)");
        out["anchor_mm"] = Json::array({num(number_of((*v)[0], "native_type anchor_mm", -10 * kMaxMm, 10 * kMaxMm)),
                                        num(number_of((*v)[1], "native_type anchor_mm", -10 * kMaxMm, 10 * kMaxMm))});
    }
    if (const Json* v = given("antialias")) {
        if (!v->is_string() || std::find(kAntialias.begin(), kAntialias.end(), v->get_ref<const std::string&>()) == kAntialias.end())
            bad("native_type antialias must be one of " + joined(kAntialias));
        if (*v != Json("smooth")) out["antialias"] = *v;  // (the default: not written)
    }
    if (const Json* v = given("shaping")) {
        if (*v != Json("opentype")) bad("native_type shaping must be opentype");
        out["shaping"] = *v;
    }
    if (const Json* v = given("transform")) {
        if (!v->is_array() || v->size() != 6) bad("native_type transform must be [a, b, c, d, e, f]");
        Json m = Json::array();
        for (const Json& x : *v) m.push_back(num(number_of(x, "native_type transform", -1e6, 1e6)));
        // (as kept — 4 decimals —: what the reader checks again; review P3-11)
        const double det = m[0].get<double>() * m[3].get<double>() - m[1].get<double>() * m[2].get<double>();
        if (std::abs(det) < 1e-9) bad("native_type transform must not squash the text flat");
        out["transform"] = std::move(m);
    }
    if (const Json* v = given("char_style")) out["char_style"] = check_style_ref(*v, "cs-", "native_type char_style");
    if (const Json* v = given("para_style")) out["para_style"] = check_style_ref(*v, "ps-", "native_type para_style");
    if (const Json* v = given("char")) {
        Json c = check_char(*v);
        if (!c.empty()) out["char"] = std::move(c);
    }
    if (const Json* v = given("para")) {
        Json p = check_para(*v);
        if (!p.empty()) out["para"] = std::move(p);
    }
    if (const Json* v = given("runs")) {
        if (!v->is_array()) bad("native_type runs must be a list");
        if (v->size() > kMaxRuns) bad("native_type holds 4096 runs at most");
        const std::int64_t n = ruby::count(text);
        std::int64_t last = 0;
        Json runs = Json::array();
        for (const Json& run : *v) {
            if (!run.is_object()) bad("a run must be {start, end, style?, char?}");
            for (const auto& [key, value] : run.items()) {
                if (key != "start" && key != "end" && key != "style" && key != "char") bad("unknown run key " + key);
            }
            const Json* s = get(run, "start");
            const Json* e = get(run, "end");
            if (s == nullptr || e == nullptr || !s->is_number_integer() || !e->is_number_integer()) bad("a run's start and end are integers");
            const std::int64_t start = s->get<std::int64_t>(), end = e->get<std::int64_t>();
            if (start < last || end <= start || end > n) bad("the runs must lie in the text, in order, none overlapping (graphemes [start, end))");
            last = end;
            Json item = Json::object({{"start", start}, {"end", end}});
            if (const Json* st = get(run, "style"); st != nullptr && !st->is_null()) item["style"] = check_style_ref(*st, "cs-", "a run's style");
            if (const Json* ch = get(run, "char"); ch != nullptr && !ch->is_null()) {
                Json c = check_char(*ch, "run char");
                if (!c.empty()) item["char"] = std::move(c);
            }
            if (item.size() > 2) runs.push_back(std::move(item));  // (a run of nothing is none)
        }
        if (!runs.empty()) out["runs"] = std::move(runs);
    }
    if (const Json* v = given("paragraphs")) {
        if (!v->is_array()) bad("native_type paragraphs must be a list");
        const std::int64_t n = paragraph_count(text);
        std::int64_t last = -1;
        Json paragraphs = Json::array();
        for (const Json& p : *v) {
            if (!p.is_object()) bad("a paragraph must be {index, style?, para?}");
            for (const auto& [key, value] : p.items()) {
                if (key != "index" && key != "style" && key != "para") bad("unknown paragraph key " + key);
            }
            const Json* i = get(p, "index");
            if (i == nullptr || !i->is_number_integer() || i->get<std::int64_t>() <= last || i->get<std::int64_t>() >= n)
                bad("the paragraphs must be the text's, in order, each once (index)");
            last = i->get<std::int64_t>();
            Json item = Json::object({{"index", last}});
            if (const Json* st = get(p, "style"); st != nullptr && !st->is_null()) item["style"] = check_style_ref(*st, "ps-", "a paragraph's style");
            if (const Json* pa = get(p, "para"); pa != nullptr && !pa->is_null()) {
                Json c = check_para(*pa, "paragraph para");
                if (!c.empty()) item["para"] = std::move(c);
            }
            if (item.size() > 1) paragraphs.push_back(std::move(item));
        }
        if (!paragraphs.empty()) out["paragraphs"] = std::move(paragraphs);
    }
    if (out.empty()) return nullptr;
    return out;
}

Json check_type_styles(const Json& raw) {
    if (raw.is_null()) return nullptr;
    if (!raw.is_object()) bad("type_styles must be an object");
    for (const auto& [key, value] : raw.items()) {
        if (key != "character" && key != "paragraph" && key != "basic") bad("unknown type_styles key " + key);
    }
    Json out = Json::object();
    for (const std::string_view kind : {std::string_view("character"), std::string_view("paragraph")}) {
        const Json* list = get(raw, kind);
        if (list == nullptr || list->is_null()) continue;
        const bool character = kind == "character";
        const std::string_view prefix = character ? "cs-" : "ps-";
        if (!list->is_array()) bad("type_styles " + std::string(kind) + " must be a list");
        if (list->size() > kMaxStyles) bad("a book keeps 512 " + std::string(kind) + " styles at most");
        std::set<std::string> ids, names;
        Json styles = Json::array();
        for (const Json& s : *list) {
            if (!s.is_object()) bad("a style must be {id, name, …}");
            for (const auto& [key, value] : s.items()) {
                if (key != "id" && key != "name" && key != "char" && !(key == "para" && !character)) bad("unknown style key " + key);
            }
            const Json id = check_style_ref(s.value("id", Json()), prefix, "a style's id");
            const Json name = s.value("name", Json());
            if (!name.is_string() || name.get_ref<const std::string&>().empty() ||
                ruby::count(name.get_ref<const std::string&>()) > static_cast<std::int64_t>(kMaxStyleName))
                bad("a style's name is 1 to 64 characters");
            if (!ids.insert(id.get<std::string>()).second) bad("two styles have the id " + id.get<std::string>());
            if (!names.insert(name.get<std::string>()).second) bad("two " + std::string(kind) + " styles have the name " + name.get<std::string>());
            Json item = Json::object({{"id", id}, {"name", name}});
            if (!character) {
                if (const Json* p = get(s, "para"); p != nullptr && !p->is_null()) {
                    Json c = check_para(*p);
                    if (!c.empty()) item["para"] = std::move(c);
                }
            }
            if (const Json* c = get(s, "char"); c != nullptr && !c->is_null()) {
                Json checked = check_char(*c);
                if (!checked.empty()) item["char"] = std::move(checked);
            }
            styles.push_back(std::move(item));
        }
        if (!styles.empty()) out[std::string(kind)] = std::move(styles);
    }
    if (const Json* basic = get(raw, "basic"); basic != nullptr && !basic->is_null()) {
        if (!basic->is_object()) bad("type_styles basic must be {para?, char?}");
        for (const auto& [key, value] : basic->items()) {
            if (key != "para" && key != "char") bad("unknown type_styles basic key " + key);
        }
        Json item = Json::object();
        if (const Json* p = get(*basic, "para"); p != nullptr && !p->is_null()) {
            Json c = check_para(*p);
            if (!c.empty()) item["para"] = std::move(c);
        }
        if (const Json* c = get(*basic, "char"); c != nullptr && !c->is_null()) {
            Json checked = check_char(*c);
            if (!checked.empty()) item["char"] = std::move(checked);
        }
        if (!item.empty()) out["basic"] = std::move(item);
    }
    if (out.empty()) return nullptr;
    return out;
}

bool native_type_normal(const Json& value, std::string_view text) {
    try {
        const Json normal = check_native_type(value, text);
        return !normal.is_null() && same_json_unordered(normal, value);
    } catch (const Error&) {
        return false;
    }
}

bool type_styles_normal(const Json& value) {
    try {
        const Json normal = check_type_styles(value);
        return !normal.is_null() && same_json_unordered(normal, value);
    } catch (const Error&) {
        return false;
    }
}

Json from_style(const Json& style) {
    Json chars = Json::object(), para = Json::object();
    if (!style.is_object()) return Json::object({{"char", chars}, {"para", para}});
    const auto take_value = [&](Json& into, std::string_view as, const Json& v) {
        const Attr* attr = into.is_object() && &into == &chars ? char_attr(as) : para_attr(as);
        try {
            into[std::string(as)] = check_value(*attr, v, std::string(as));
        } catch (const Error&) {  // (a style's value past the attribute's bounds: the style alone draws it)
        }
    };
    const auto take = [&](std::string_view key, Json& into, std::string_view as) {
        const Json* v = get(style, key);
        if (v == nullptr || v->is_null()) return;
        take_value(into, as, *v);
    };
    take("font", chars, "font");
    take("size_mm", chars, "size_mm");
    take("tracking", chars, "tracking");
    take("rgb", chars, "rgb");
    take("scale_x", chars, "scale_x");
    take("tcy", chars, "tcy");
    // (a line's features, tag by tag: one that is not a tag of 4 letters left out, not all of them — Pillow's "-liga" and
    // "+liga" read as off and on; review P3-4)
    if (const Json* f = get(style, "features"); f != nullptr && (f->is_array() || f->is_object())) {
        Json own = Json::object();
        const auto keep = [&](std::string tag, std::int64_t value) {
            if (tag.size() == 5 && (tag[0] == '-' || tag[0] == '+')) {
                value = tag[0] == '-' ? 0 : value;
                tag.erase(0, 1);
            }
            if (tag.size() == 4 && std::all_of(tag.begin(), tag.end(), [](char c) { return tag_char(c, true); }) && value >= 0 && value <= 65535 &&
                own.size() < kMaxTags)
                own[tag] = value;
        };
        if (f->is_array()) {
            for (const Json& t : *f) {
                if (t.is_string()) keep(t.get<std::string>(), 1);
            }
        } else {
            for (const auto& [tag, value] : f->items()) {
                if (value.is_number_integer()) keep(tag, value.get<std::int64_t>());
                else if (value.is_boolean()) keep(tag, value.get<bool>() ? 1 : 0);
            }
        }
        if (!own.empty()) take_value(chars, "features", own);
    }
    if (const Json* w = get(style, "weight"); w != nullptr && w->is_string() && (*w == "bold" || *w == "heavy")) {
        chars["faux_bold"] = *w;
    } else if (const Json* b = get(style, "bold"); b != nullptr && b->is_boolean() && b->get<bool>()) {
        chars["faux_bold"] = "bold";
    }
    if (const Json* i = get(style, "italic"); i != nullptr && i->is_boolean() && i->get<bool>()) chars["faux_italic"] = true;
    take("align", para, "align");
    take("yakumono", para, "yakumono");
    return Json::object({{"char", chars}, {"para", para}});
}

Json resolve_para(const StoryLine& record, const Json* styles, std::int64_t index) {
    Json out = para_defaults();
    const Json own = from_style(record.style);
    merge_into(out, &own["para"]);
    if (styles != nullptr && styles->is_object()) {
        if (const Json* basic = get(*styles, "basic")) merge_into(out, get(*basic, "para"));
    }
    if (!record.native_type) return out;
    const Json& nt = *record.native_type;
    const Json* paragraph = paragraph_item(nt, index);
    const Json* style_id = paragraph != nullptr && get(*paragraph, "style") != nullptr ? get(*paragraph, "style") : get(nt, "para_style");
    if (const Json* style = style_named(styles, "paragraph", style_id)) merge_into(out, get(*style, "para"));
    merge_into(out, get(nt, "para"));
    if (paragraph != nullptr) merge_into(out, get(*paragraph, "para"));
    return out;
}

std::vector<CharRun> resolve_chars(const StoryLine& record, const Json* styles) {
    const std::int64_t n = ruby::count(record.text);
    // the attributes every letter starts from, then each paragraph's, then the ranges' — worked out once for each stretch
    // between the paragraphs' and the ranges' edges (never a letter at a time)
    Json base = char_defaults();
    const Json own = from_style(record.style);
    merge_into(base, &own["char"]);
    if (styles != nullptr && styles->is_object()) {
        if (const Json* basic = get(*styles, "basic")) merge_into(base, get(*basic, "char"));
    }
    if (n == 0) return {CharRun{0, 0, base}};  // (an empty text: what a letter typed there would take)
    const Json empty = Json::object();
    const Json& nt = record.native_type ? *record.native_type : empty;
    const std::vector<std::int64_t> starts = paragraph_starts(record.text);
    std::vector<Json> paragraph_chars;
    for (std::size_t p = 0; p < starts.size(); ++p) {
        Json chars = base;
        const Json* paragraph = paragraph_item(nt, static_cast<std::int64_t>(p));
        const Json* style_id = paragraph != nullptr && get(*paragraph, "style") != nullptr ? get(*paragraph, "style") : get(nt, "para_style");
        if (const Json* style = style_named(styles, "paragraph", style_id)) merge_into(chars, get(*style, "char"));
        if (const Json* style = style_named(styles, "character", get(nt, "char_style"))) merge_into(chars, get(*style, "char"));
        merge_into(chars, get(nt, "char"));
        paragraph_chars.push_back(std::move(chars));
    }
    std::vector<const Json*> runs;
    if (const Json* given = get(nt, "runs"); given != nullptr && given->is_array())
        for (const Json& run : *given) runs.push_back(&run);
    std::vector<std::int64_t> edges(starts.begin(), starts.end());
    edges.push_back(n);
    for (const Json* run : runs) {
        edges.push_back(std::clamp<std::int64_t>(run->value("start", std::int64_t{0}), 0, n));
        edges.push_back(std::clamp<std::int64_t>(run->value("end", std::int64_t{0}), 0, n));
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<CharRun> out;
    std::size_t r = 0;
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        const std::int64_t s = edges[i], e = edges[i + 1];
        const std::size_t p = static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(), s) - starts.begin()) - 1;
        Json attrs = paragraph_chars[p];
        while (r < runs.size() && runs[r]->value("end", std::int64_t{0}) <= s) ++r;
        if (r < runs.size() && runs[r]->value("start", std::int64_t{0}) <= s) {
            if (const Json* style = style_named(styles, "character", get(*runs[r], "style"))) merge_into(attrs, get(*style, "char"));
            merge_into(attrs, get(*runs[r], "char"));
        }
        if (!out.empty() && out.back().attrs == attrs) {
            out.back().end = e;
        } else {
            out.push_back(CharRun{s, e, std::move(attrs)});
        }
    }
    return out;
}

Edit edit_between(std::string_view before, std::string_view after) {
    const std::vector<std::u32string> a = graphemes(before), b = graphemes(after);
    std::size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p]) ++p;
    std::size_t s = 0;
    while (s < a.size() - p && s < b.size() - p && a[a.size() - 1 - s] == b[b.size() - 1 - s]) ++s;
    return Edit{static_cast<std::int64_t>(p), static_cast<std::int64_t>(a.size() - s), static_cast<std::int64_t>(b.size() - s - p)};
}

std::vector<Edit> edits_between(std::string_view before, std::string_view after) {
    const std::vector<std::u32string> ga = graphemes(before), gb = graphemes(after);
    if (ga == gb) return {};
    // (each grapheme as a number, the same for the same grapheme: compared as numbers)
    std::map<std::u32string, std::int32_t> ids;
    const auto numbered = [&](const std::vector<std::u32string>& g) {
        std::vector<std::int32_t> out;
        out.reserve(g.size());
        for (const std::u32string& x : g) out.push_back(ids.emplace(x, static_cast<std::int32_t>(ids.size())).first->second);
        return out;
    };
    const std::vector<std::int32_t> A = numbered(ga), B = numbered(gb);
    const std::int64_t n = static_cast<std::int64_t>(A.size()), m = static_cast<std::int64_t>(B.size());
    // Myers' shortest edit script (O((n + m)·D) time, the D bands of its furthest reaches kept to read the script back): up
    // to kMaxEditSteps differences; past them their common start and end alone (edit_between)
    const std::int64_t most = std::min<std::int64_t>(n + m, kMaxEditSteps);
    std::vector<std::int64_t> v(static_cast<std::size_t>(2 * most + 3), 0);
    const auto at = [&](std::int64_t k) -> std::int64_t& { return v[static_cast<std::size_t>(k + most + 1)]; };
    std::vector<std::vector<std::int64_t>> bands;  // (band d: the reaches of diagonals −d, −d + 2, …, d)
    std::int64_t found = -1;
    for (std::int64_t d = 0; d <= most && found < 0; ++d) {
        std::vector<std::int64_t> band;
        band.reserve(static_cast<std::size_t>(d + 1));
        for (std::int64_t k = -d; k <= d; k += 2) {
            std::int64_t x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
            std::int64_t y = x - k;
            while (x < n && y < m && A[static_cast<std::size_t>(x)] == B[static_cast<std::size_t>(y)]) {
                ++x;
                ++y;
            }
            at(k) = x;
            band.push_back(x);
            if (x >= n && y >= m && found < 0) found = d;
        }
        bands.push_back(std::move(band));
    }
    if (found < 0) return {edit_between(before, after)};
    // read the script back: each step a deletion of A[x] or an insertion of B[y] before A[x]
    struct Step {
        std::int64_t x;
        bool insert;
    };
    std::vector<Step> steps;
    std::int64_t x = n, y = m;
    for (std::int64_t d = found; d > 0; --d) {
        const std::vector<std::int64_t>& prev = bands[static_cast<std::size_t>(d - 1)];
        const auto reach = [&](std::int64_t k) { return prev[static_cast<std::size_t>((k + d - 1) / 2)]; };
        const std::int64_t k = x - y;
        const bool down = k == -d || (k != d && reach(k - 1) < reach(k + 1));
        const std::int64_t pk = down ? k + 1 : k - 1;
        const std::int64_t px = reach(pk), py = px - pk;
        steps.push_back(Step{px, down});
        x = px;
        y = py;
    }
    std::reverse(steps.begin(), steps.end());
    std::vector<Edit> out;
    for (const Step& s : steps) {
        if (out.empty() || out.back().end != s.x) out.push_back(Edit{s.x, s.x, 0});
        if (s.insert) {
            ++out.back().inserted;
        } else {
            ++out.back().end;
        }
    }
    return out;
}

std::vector<Edit> edits_of(std::string_view before, std::string_view after, const std::vector<ruby::Replacement>& replacements) {
    const std::vector<std::size_t> b = ruby::bounds(ruby::code_points(before));
    const std::vector<std::size_t> a = ruby::bounds(ruby::code_points(after));
    // (where a code point lies among the graphemes: the grapheme it begins in, or the next edge)
    const auto floor_in = [](const std::vector<std::size_t>& bounds, std::size_t cp) {
        const auto it = std::upper_bound(bounds.begin(), bounds.end(), cp);
        return static_cast<std::int64_t>(it == bounds.begin() ? 0 : (it - bounds.begin()) - 1);
    };
    const auto ceil_in = [](const std::vector<std::size_t>& bounds, std::size_t cp) {
        const auto it = std::lower_bound(bounds.begin(), bounds.end(), cp);
        return static_cast<std::int64_t>(std::min<std::ptrdiff_t>(it - bounds.begin(), static_cast<std::ptrdiff_t>(bounds.size()) - 1));
    };
    const auto edge = [](const std::vector<std::size_t>& bounds, std::size_t cp) { return std::binary_search(bounds.begin(), bounds.end(), cp); };
    // Each match as code points of both texts, widened until both its ends are edges of graphemes in both (the text around
    // it is the same in both: a mark it puts in joins the letter before it, one it takes away parts it), matches that then
    // touch made one; then counted as graphemes.
    struct Region {
        std::size_t b0, b1, a0, a1;
    };
    std::vector<Region> regions;
    std::int64_t shift = 0;  // (code points the replacements before this one added)
    for (const ruby::Replacement& r : replacements) {
        std::size_t back = 0, ahead = 0;
        const std::size_t as = static_cast<std::size_t>(static_cast<std::int64_t>(r.start) + shift), ae = as + r.with.size();
        for (bool moved = true; moved;) {
            moved = false;
            // (never past either text's start or end: a text that is not as the matches say is counted as it is)
            if (back > std::min(r.start, as) || r.end + ahead > b.back() || ae + ahead > a.back()) {
                back = std::min(back, std::min(r.start, as));
                ahead = std::min(b.back() - std::min(b.back(), r.end), a.back() - std::min(a.back(), ae));
                break;
            }
            const std::size_t b0 = r.start - back, a0 = as - back, b1 = r.end + ahead, a1 = ae + ahead;
            if (!edge(b, b0) && b0 > 0) {
                back += b0 - b[static_cast<std::size_t>(floor_in(b, b0))];
                moved = true;
            } else if (!edge(a, a0) && a0 > 0) {
                back += a0 - a[static_cast<std::size_t>(floor_in(a, a0))];
                moved = true;
            } else if (!edge(b, b1)) {
                ahead += b[static_cast<std::size_t>(ceil_in(b, b1))] - b1;
                moved = true;
            } else if (!edge(a, a1)) {
                ahead += a[static_cast<std::size_t>(ceil_in(a, a1))] - a1;
                moved = true;
            }
        }
        const Region here{r.start - back, r.end + ahead, as - back, ae + ahead};
        if (!regions.empty() && here.b0 < regions.back().b1) {
            regions.back().b1 = std::max(regions.back().b1, here.b1);
            regions.back().a1 = std::max(regions.back().a1, here.a1);
        } else {
            regions.push_back(here);
        }
        shift += static_cast<std::int64_t>(r.with.size()) - static_cast<std::int64_t>(r.end - r.start);
    }
    std::vector<Edit> out;
    for (const Region& g : regions) {
        const std::int64_t start = ceil_in(b, g.b0), end = ceil_in(b, g.b1);
        out.push_back(Edit{start, end, std::max<std::int64_t>(0, ceil_in(a, g.a1) - ceil_in(a, g.a0))});
    }
    // (edits that still share a grapheme are one)
    std::vector<Edit> merged;
    for (const Edit& e : out) {
        if (!merged.empty() && e.start < merged.back().end) {
            merged.back().end = std::max(merged.back().end, e.end);
            merged.back().inserted += e.inserted;
        } else {
            merged.push_back(e);
        }
    }
    return merged;
}

Json follow(const Json& native_type, std::string_view before, std::string_view after, const std::vector<Edit>& edits) {
    if (!native_type.is_object()) return native_type;
    Json out = native_type;
    const Places places(edits);
    if (const Json* runs = get(native_type, "runs"); runs != nullptr && runs->is_array()) {
        Json kept = Json::array();
        for (Json run : *runs) {
            const std::int64_t s = places.start(run.value("start", std::int64_t{0})), e = places.end(run.value("end", std::int64_t{0}));
            if (e <= s) continue;
            run["start"] = s;
            run["end"] = e;
            kept.push_back(std::move(run));
        }
        if (kept.empty()) out.erase("runs");
        else out["runs"] = std::move(kept);
    }
    if (const Json* paragraphs = get(native_type, "paragraphs"); paragraphs != nullptr && paragraphs->is_array()) {
        const std::vector<std::int64_t> was = paragraph_starts(before), now = paragraph_starts(after);
        Json kept = Json::array();
        std::int64_t last = -1;
        for (Json p : *paragraphs) {
            const std::int64_t index = p.value("index", std::int64_t{-1});
            if (index < 0 || index >= static_cast<std::int64_t>(was.size())) continue;
            const std::int64_t at = places.paragraph(was[static_cast<std::size_t>(index)]);
            const auto found = std::lower_bound(now.begin(), now.end(), at);
            if (at < 0 || found == now.end() || *found != at) continue;  // (its line break taken away: joined to the one before it)
            const std::int64_t moved = found - now.begin();
            if (moved <= last) continue;
            last = moved;
            p["index"] = moved;
            kept.push_back(std::move(p));
        }
        if (kept.empty()) out.erase("paragraphs");
        else out["paragraphs"] = std::move(kept);
    }
    if (out.empty()) return nullptr;
    return out;
}

std::int64_t removed_by(const Json& before, const Json& after) {
    const auto count = [](const Json& nt) {
        std::int64_t n = 0;
        for (const char* key : {"runs", "paragraphs"})
            if (const Json* list = nt.is_object() ? get(nt, key) : nullptr; list != nullptr && list->is_array()) n += static_cast<std::int64_t>(list->size());
        return n;
    };
    return std::max<std::int64_t>(0, count(before) - count(after));
}

Json runs_from_style_runs(const StoryLine& line, std::vector<std::size_t>* taken) {
    const std::u32string text = ruby::code_points(line.text);
    const std::vector<std::size_t> b = ruby::bounds(text);
    const auto grapheme_at = [&](std::size_t cp, bool up) {  // (by halving: a run of 4096 words over a long text)
        const auto it = std::lower_bound(b.begin(), b.end(), cp);
        if (it == b.end()) return static_cast<std::int64_t>(b.size() - 1);
        const auto i = static_cast<std::int64_t>(it - b.begin());
        return *it == cp || up ? i : i - 1;
    };
    std::size_t index = 0;
    Json runs = Json::array();
    std::size_t pos = 0;
    std::int64_t last = 0;
    for (const auto& [run_words, run_style] : line.style_runs) {
        const std::size_t this_one = index++;
        if (run_words.empty()) continue;
        const std::u32string words = ruby::code_points(run_words);
        const std::size_t at = text.find(words, pos);
        if (at == std::u32string::npos) continue;
        pos = at + words.size();
        if (taken != nullptr) taken->push_back(this_one);
        if (!run_style.is_object()) continue;
        Json chars = Json::object();
        const auto put = [&](std::string_view key, std::string_view as) {
            const Json* v = get(run_style, key);
            if (v == nullptr || v->is_null()) return;
            try {
                chars[std::string(as)] = check_value(*char_attr(as), *v, std::string(as));
            } catch (const Error&) {
            }
        };
        put("scale", "scale");
        // (a word's scale as the lettering draws Genko's words: within 0.3..3 — a range's own reaches 0.1..10; review P3-1)
        if (chars.contains("scale")) chars["scale"] = num(std::clamp(chars["scale"].get<double>(), 0.3, 3.0));
        put("rgb", "rgb");
        put("tcy", "tcy");
        if (const Json* w = get(run_style, "weight"); w != nullptr && w->is_string() && (*w == "bold" || *w == "heavy" || *w == "normal")) {
            chars["faux_bold"] = *w;
        } else if (const Json* bold = get(run_style, "bold"); bold != nullptr && bold->is_boolean()) {
            chars["faux_bold"] = bold->get<bool>() ? "bold" : "normal";
        }
        const std::int64_t s = std::max(grapheme_at(at, false), last), e = grapheme_at(pos, true);
        if (chars.empty() || e <= s) continue;
        last = e;
        runs.push_back(Json::object({{"start", s}, {"end", e}, {"char", check_char(chars, "run char")}}));
    }
    return runs;
}

StoryLine record_of(const StoryLine& line) {
    StoryLine out = line;
    out.id.clear();
    out.page_index = Num(0);
    out.speaker.clear();
    out.frame_id.reset();
    return out;
}

StoryLine line_of(const Layer& layer, const Num& page_index) {
    StoryLine out = layer.type ? *layer.type : StoryLine{};
    out.id = layer.id;
    out.page_index = page_index;
    return out;
}

std::optional<std::array<double, 6>> transform_of(const StoryLine& record) {
    if (!record.native_type || !record.native_type->is_object()) return std::nullopt;
    const auto it = record.native_type->find("transform");
    if (it == record.native_type->end() || !it->is_array() || it->size() != 6) return std::nullopt;
    std::array<double, 6> t{};
    for (std::size_t i = 0; i < 6; ++i) {
        if (!(*it)[i].is_number()) return std::nullopt;
        t[i] = (*it)[i].get<double>();
    }
    return t;
}

StoryLine moved(const StoryLine& record, double dx, double dy) {
    StoryLine out = record;
    if (const std::optional<std::array<double, 6>> t = transform_of(record)) {
        // (PC5 G-a: a record its matrix turns moves by its matrix — where its letters are set stays)
        Json nt = *out.native_type;
        nt["transform"] = Json::array({num((*t)[0]), num((*t)[1]), num((*t)[2]), num((*t)[3]), num((*t)[4] + dx), num((*t)[5] + dy)});
        out.native_type = std::move(nt);
        return out;
    }
    const auto shift = [&](Num& v, double d) { v = Num(v.value() + d); };
    shift(out.x_mm, dx);
    shift(out.y_mm, dy);
    if (out.tail) {
        shift(out.tail->x, dx);
        shift(out.tail->y, dy);
    }
    if (out.path) {
        for (Point& p : *out.path) {
            shift(p.x, dx);
            shift(p.y, dy);
        }
    }
    const auto shift_pair = [&](Json& pair) {
        if (pair.is_array() && pair.size() == 2 && pair[0].is_number() && pair[1].is_number())
            pair = Json::array({pair[0].get<double>() + dx, pair[1].get<double>() + dy});
    };
    for (Json& tail : out.tails) {
        if (!tail.is_object()) continue;
        if (tail.contains("to")) shift_pair(tail["to"]);
        if (tail.contains("via") && tail["via"].is_array()) {
            if (!tail["via"].empty() && tail["via"][0].is_array()) {
                for (Json& p : tail["via"]) shift_pair(p);
            } else {
                shift_pair(tail["via"]);
            }
        }
    }
    if (out.native_type && out.native_type->contains("anchor_mm")) {
        Json& anchor = (*out.native_type)["anchor_mm"];
        anchor = Json::array({num(anchor[0].get<double>() + dx), num(anchor[1].get<double>() + dy)});
    }
    return out;
}

void move_anchor(StoryLine& record, double dx, double dy) {
    if (!record.native_type || !record.native_type->is_object() || (dx == 0 && dy == 0)) return;
    const auto it = record.native_type->find("anchor_mm");
    if (it == record.native_type->end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return;
    *it = Json::array({num((*it)[0].get<double>() + dx), num((*it)[1].get<double>() + dy)});
}

std::shared_ptr<const StoryLine> shared_record(StoryLine record) { return std::make_shared<const StoryLine>(std::move(record)); }

bool is_type_layer(const Layer& layer) { return layer.kind == LayerKind::Type; }

bool uses_type(const Document& doc) {
    if (doc.type_styles && !doc.type_styles->is_null()) return true;
    for (const StoryLine& line : doc.story) {
        if (line.native_type) return true;
    }
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (is_type_layer(layer)) return true;
        }
    }
    return false;
}

void add_feature(Document& doc) {
    if (!uses_type(doc)) return;
    if (std::find(doc.features.begin(), doc.features.end(), kTypeFeature) == doc.features.end()) doc.features.emplace_back(kTypeFeature);
}

void validate_type(const Document& doc) {
    const auto styles_kept = [&](const Json& nt, const std::string& whose) {
        const auto kept = [&](std::string_view kind, const Json* id) {
            if (id == nullptr || id->is_null()) return;
            if (style_named(doc.type_styles ? &*doc.type_styles : nullptr, kind, id) == nullptr)
                bad(whose + " takes the style " + py_str(*id) + ", which the book does not keep");
        };
        kept("character", get(nt, "char_style"));
        kept("paragraph", get(nt, "para_style"));
        if (const Json* runs = get(nt, "runs"); runs != nullptr && runs->is_array()) {
            for (const Json& run : *runs) kept("character", get(run, "style"));
        }
        if (const Json* ps = get(nt, "paragraphs"); ps != nullptr && ps->is_array()) {
            for (const Json& p : *ps) kept("paragraph", get(p, "style"));
        }
    };
    const auto record_ok = [&](const StoryLine& record, const std::string& whose) {
        if (!record.native_type) return;
        if (!native_type_normal(*record.native_type, record.text)) {
            try {
                (void)check_native_type(*record.native_type, record.text);
            } catch (const Error& error) {
                bad(whose + "'s " + error.what());
            }
            bad(whose + "'s native_type is not in its normal form");
        }
        styles_kept(*record.native_type, whose);
    };
    if (doc.type_styles && !type_styles_normal(*doc.type_styles)) {
        try {
            (void)check_type_styles(*doc.type_styles);
        } catch (const Error& error) {
            bad(std::string("the book's ") + error.what());
        }
        bad("the book's type_styles are not in their normal form");
    }
    for (const StoryLine& line : doc.story) {
        record_ok(line, "the line " + line.id);
        // (PC5 G-a: a line stays upright — turned, scaled or bent, it is a type layer's matrix)
        if (line.native_type && line.native_type->is_object() && line.native_type->contains("transform"))
            bad("the line " + line.id + " stays upright: its native_type holds no transform (a type layer's does)");
    }
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (!is_type_layer(layer)) {
                if (layer.type) bad("the layer " + layer.id + " holds a type record but is not a type layer");
                continue;
            }
            if (!layer.type) bad("the type layer " + layer.id + " holds no text");
            if (layer.stroke_count() > 0 || !layer.patches.empty() || (layer.raster_png && !layer.raster_png->empty()) || layer.color_raster ||
                layer.ink_raster || (layer.shapes && !layer.shapes->items.empty()))
                bad("a type layer holds no pixels or lines of its own: rasterize the type layer " + layer.id + " first");
            record_ok(*layer.type, "the type layer " + layer.id);
        }
    }
}

}  // namespace genko::core::type_text
