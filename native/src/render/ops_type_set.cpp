// PC5 G-b (type.setStyle, type.openType.*; render/ops_type.hpp, SPEC.md TEXT-02「set_type」): set_type — Photoshop's
// character and paragraph panels as an op. The attributes go into the record's native_type (the whole text's own, or a
// range's, or the paragraphs a range touches), checked and kept in their normal form by core::type_text; the record is set
// by the OpenType typesetting from then on (native_type.shaping "opentype": the coordinator's decision, design 3 — a line
// touched by the character panel's OpenType attributes is set so, down too).

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/ops_type.hpp"

namespace genko::render::type_ops {

namespace {

using core::Json;
using core::OpError;
using core::StoryLine;

// Attributes a set_type gives (null: taken away) laid over `into` (an object).
void lay(Json& into, const Json& given) {
    if (!into.is_object()) into = Json::object();
    for (const auto& [key, value] : given.items()) {
        if (value.is_null()) into.erase(key);
        else into[key] = value;
    }
}

// The attributes' keys taken away from `from` (an object; nothing when it is not one).
void take_away(Json& from, const Json& given) {
    if (!from.is_object()) return;
    for (const auto& [key, value] : given.items()) from.erase(key);
}

// Each given attribute checked as its table says (null: kept, to take one away): core::OpError otherwise.
void check(const Json& attrs, bool character) {
    Json present = Json::object();
    for (const auto& [key, value] : attrs.items()) {
        if (!value.is_null()) present[key] = value;
    }
    try {
        if (character) (void)core::type_text::check_char(present);
        else (void)core::type_text::check_para(present);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

// The runs of native_type with the character attributes laid over [start, end): the runs there split at its edges, the
// stretches no run covers given runs of their own.
Json runs_with(const Json& runs, std::int64_t start, std::int64_t end, const Json& chars) {
    std::vector<Json> out;
    const auto piece = [&](const Json& run, std::int64_t s, std::int64_t e, bool inside) {
        if (e <= s) return;
        Json item = run;
        item["start"] = s;
        item["end"] = e;
        if (inside) {
            Json c = item.value("char", Json::object());
            lay(c, chars);
            if (c.empty()) item.erase("char");
            else item["char"] = std::move(c);
        }
        out.push_back(std::move(item));
    };
    std::int64_t covered = start;
    std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
    if (runs.is_array()) {
        for (const Json& run : runs) {
            const std::int64_t s = run.value("start", std::int64_t{0}), e = run.value("end", std::int64_t{0});
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
    std::sort(out.begin(), out.end(), [](const Json& a, const Json& b) { return a.value("start", std::int64_t{0}) < b.value("start", std::int64_t{0}); });
    // (neighbours alike joined: the normal form keeps them apart only where they differ)
    Json joined = Json::array();
    for (Json& run : out) {
        if (!joined.empty()) {
            Json& last = joined.back();
            if (last.value("end", std::int64_t{0}) == run.value("start", std::int64_t{0}) && last.value("style", Json()) == run.value("style", Json()) &&
                last.value("char", Json()) == run.value("char", Json())) {
                last["end"] = run["end"];
                continue;
            }
        }
        joined.push_back(std::move(run));
    }
    return joined;
}

// Genko's words set larger, bolder or in a colour (style_runs) written as ranges, the first time a character attribute is
// set (on a range or the whole: the whole's then takes the words' own away as a range's, review P3-2): the lettering reads
// the ranges from then on. A word found keeps its place in style_runs with its 縦中横 alone (the lettering's 縦中横 is
// chosen word by word — review P2-2 —, each word found from where the one before it was); a word not found in the text is
// kept whole (it takes effect where the text comes to hold it, as before).
void words_to_runs(StoryLine& record, Json& nt) {
    if (nt.contains("runs") || record.style_runs.empty()) return;
    std::vector<std::size_t> taken;
    nt["runs"] = core::type_text::runs_from_style_runs(record, &taken);
    std::vector<bool> found(record.style_runs.size(), false);
    for (const std::size_t i : taken) found[i] = true;
    bool keeps = false;
    for (std::size_t i = 0; i < record.style_runs.size(); ++i) {
        Json& style = record.style_runs[i].second;
        if (!found[i]) {
            keeps = true;
            continue;
        }
        Json left = Json::object();
        if (style.is_object() && style.contains("tcy") && !style["tcy"].is_null()) {
            left["tcy"] = style["tcy"];
            keeps = true;
        }
        style = std::move(left);
    }
    if (!keeps) record.style_runs.clear();
}

// The paragraphs a range of graphemes [start, end) touches (an empty range: the one it is in).
std::vector<std::int64_t> paragraphs_of(const std::string& text, std::int64_t start, std::int64_t end) {
    std::vector<std::int64_t> out;
    const std::int64_t count = core::type_text::paragraph_count(text);
    const std::int64_t n = core::ruby::count(text);
    for (std::int64_t p = 0; p < count; ++p) {
        const std::int64_t first = core::type_text::paragraph_start(text, p);
        const std::int64_t last = p + 1 < count ? core::type_text::paragraph_start(text, p + 1) - 1 : n;  // (its line break's place)
        if (first <= std::max(start, end - 1) && start <= last) out.push_back(p);
    }
    return out;
}

}  // namespace

void set_type(core::OpContext& c) {
    core::Document& doc = c.doc;
    const TypeTarget t = target(doc, c.op, true);
    Json chars = Json::object(), paras = Json::object();
    if (const Json* given = core::get(c.op, "char"); given != nullptr && !given->is_null()) {
        if (!given->is_object()) throw OpError("char must be {attribute: value}");
        chars = *given;
    }
    if (const Json* given = core::get(c.op, "para"); given != nullptr && !given->is_null()) {
        if (!given->is_object()) throw OpError("para must be {attribute: value}");
        paras = *given;
    }
    static const std::vector<std::string> kOwn{"op", "line", "page", "layer_id", "range", "char", "para", "size_pt"};
    for (const auto& [key, value] : c.op.items()) {
        if (std::find(kOwn.begin(), kOwn.end(), key) != kOwn.end()) continue;
        if (core::type_text::char_attr(key) != nullptr) chars[key] = value;
        else if (core::type_text::para_attr(key) != nullptr) paras[key] = value;
        else throw OpError("unknown attribute " + key + " (a character or paragraph attribute of TEXT-02)");
    }
    if (const Json* pt = core::get(c.op, "size_pt"); pt != nullptr && !pt->is_null()) {  // (Photoshop's size: points)
        if (chars.contains("size_mm")) throw OpError("size_pt or size_mm sizes the text, not both");
        const double size = core::finite_float(*pt, "size_pt");
        if (!(size > 0) || size > 1296) throw OpError("size_pt must be above 0 and at most 1296");
        chars["size_mm"] = core::py_round(size * 25.4 / 72.0, 4);
    }
    if (chars.empty() && paras.empty()) throw OpError("set_type sets a character or paragraph attribute (char, para or an attribute's name)");
    check(chars, true);
    check(paras, false);
    StoryLine record = t.line != nullptr ? *t.line : *doc.page(t.page).layers[t.layer].type;
    bounded_text(record.text);  // (a text the OpenType typesetting sets: kMaxTypeText at most, review P2-5)
    const std::int64_t n = core::ruby::count(record.text);
    std::optional<std::pair<std::int64_t, std::int64_t>> range;
    if (const Json* r = core::get(c.op, "range"); r != nullptr && !r->is_null()) {
        if (!r->is_array() || r->size() != 2 || !(*r)[0].is_number_integer() || !(*r)[1].is_number_integer())
            throw OpError("range is [start, end] (graphemes)");
        const std::int64_t s = (*r)[0].get<std::int64_t>(), e = (*r)[1].get<std::int64_t>();
        if (s < 0 || e > n || e < s) throw OpError("range must lie in the text, its end not before its start (0.." + std::to_string(n) + " graphemes)");
        if (s == e && !chars.empty()) throw OpError("an empty range takes paragraph attributes only");
        range = std::make_pair(s, e);
    }
    Json nt = record.native_type ? *record.native_type : Json::object();
    if (!chars.empty()) words_to_runs(record, nt);
    if (!range || (range->first == 0 && range->second == n)) {  // (the whole text: its own, every range's and paragraph's own of these taken away)
        if (!chars.empty()) {
            Json own = nt.value("char", Json::object());
            lay(own, chars);
            nt["char"] = own;
            if (nt.contains("runs")) {
                for (Json& run : nt["runs"]) {
                    if (run.contains("char")) take_away(run["char"], chars);
                }
            }
        }
        if (!paras.empty()) {
            Json own = nt.value("para", Json::object());
            lay(own, paras);
            nt["para"] = own;
            if (nt.contains("paragraphs")) {
                for (Json& p : nt["paragraphs"]) {
                    if (p.contains("para")) take_away(p["para"], paras);
                }
            }
        }
    } else {
        if (!chars.empty()) {
            nt["runs"] = runs_with(nt.value("runs", Json::array()), range->first, range->second, chars);
        }
        if (!paras.empty()) {
            std::map<std::int64_t, Json> by_index;
            if (nt.contains("paragraphs")) {
                for (const Json& p : nt["paragraphs"]) by_index[p.value("index", std::int64_t{0})] = p;
            }
            for (const std::int64_t p : paragraphs_of(record.text, range->first, range->second)) {
                Json& item = by_index[p];
                item["index"] = p;
                Json own = item.value("para", Json::object());
                lay(own, paras);
                item["para"] = own;
            }
            Json list = Json::array();
            for (auto& [index, item] : by_index) list.push_back(std::move(item));
            nt["paragraphs"] = std::move(list);
        }
    }
    nt["shaping"] = "opentype";
    set_native_type(record, nt);
    fit_point_text(record, doc);
    measure(record, doc);  // (a line in its balloon too: one too large to set is refused, review P1-1)
    if (t.line != nullptr) {
        t.line->native_type = record.native_type;
        t.line->style_runs = record.style_runs;
        t.line->x_mm = record.x_mm;
        t.line->y_mm = record.y_mm;
        t.line->w_mm = record.w_mm;
        t.line->h_mm = record.h_mm;
    } else {
        core::Page& page = doc.edit_page(t.page);
        page.layers[t.layer].type = core::type_text::shared_record(std::move(record));
    }
    core::type_text::add_feature(doc);
    c.report = Json::object();
}

}  // namespace genko::render::type_ops
