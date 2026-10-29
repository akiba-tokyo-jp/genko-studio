// PC5 G-c (type.characterStyle.*, type.paragraphStyle.*; render/ops_type.hpp, SPEC.md TEXT-02「文字スタイル・段落スタイル」):
// type_style — Photoshop's 文字スタイル and 段落スタイル panels as one op. The book's styles (Document::type_styles) and
// what a text takes of them (core/type_styles); a style changed, the texts that take it are set again where they are
// drawn (the lettering's memo key holds the styles' digest) and their point texts' boxes kept round their letters here.
// PhotoCraft v0.2.0's engine/src/type_styles_cmds.rs (MIT; native/third_party/photocraft/NOTICE.md).

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "render/ops_type.hpp"
#include "render/text/opentype.hpp"

namespace genko::render::type_ops {

namespace {

using core::Document;
using core::Json;
using core::OpError;
using core::StoryLine;
namespace ts = core::type_styles;

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v == nullptr || v->is_null() ? nullptr : v;
}

bool flag(const Json& op, std::string_view key, bool fallback) {
    const Json* v = given(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(std::string(key) + " is true or false");
    return v->get<bool>();
}

// The book's styles in their normal form (none when they hold nothing), or core::OpError.
void keep_styles(Document& doc, const Json& styles) {
    try {
        const Json normal = core::type_text::check_type_styles(styles);
        if (normal.is_null()) doc.type_styles.reset();
        else doc.type_styles = normal;
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

const Json* styles_of(const Document& doc) { return doc.type_styles ? &*doc.type_styles : nullptr; }

// The style an op names by its id (kind Paragraph: "basic" too, when `basic`), or core::OpError.
std::string style_id(const Json* styles, ts::Kind kind, const Json& op, bool basic) {
    const Json* id = given(op, "id");
    if (id == nullptr || !id->is_string()) throw OpError("id names the style");
    const std::string named = id->get<std::string>();
    if (kind == ts::Kind::Paragraph && named == ts::kBasic) {
        if (!basic) throw OpError("Basic Paragraph cannot be deleted or renamed");
        return named;
    }
    if (ts::find(styles, kind, named) == nullptr) throw OpError("no " + std::string(ts::kind_name(kind)) + " style " + named);
    return named;
}

// The text an op names (line, or page and layer_id), its record, and the range of it (graphemes; none: the whole).
struct Text {
    TypeTarget target;
    StoryLine record;
    std::optional<ts::Range> range;
};
std::optional<Text> text_of(Document& doc, const Json& op, bool edit) {
    if (given(op, "line") == nullptr && given(op, "layer_id") == nullptr) {
        if (given(op, "range") != nullptr) throw OpError("range is a range of the text named (line, or page and layer_id)");
        return std::nullopt;
    }
    Text out;
    out.target = target(doc, op, edit);
    out.record = out.target.line != nullptr ? *out.target.line : *doc.page(out.target.page).layers[out.target.layer].type;
    if (const Json* r = given(op, "range")) {
        if (!r->is_array() || r->size() != 2 || !(*r)[0].is_number_integer() || !(*r)[1].is_number_integer())
            throw OpError("range is [start, end] (graphemes)");
        const std::int64_t n = core::ruby::count(out.record.text);
        const std::int64_t s = (*r)[0].get<std::int64_t>(), e = (*r)[1].get<std::int64_t>();
        if (s < 0 || e > n || e < s) throw OpError("range must lie in the text, its end not before its start (0.." + std::to_string(n) + " graphemes)");
        out.range = ts::Range{s, e};
    }
    return out;
}
Text needed(std::optional<Text> text, std::string_view action) {
    if (!text) throw OpError("type_style " + std::string(action) + " names the text: line, or page and layer_id");
    return std::move(*text);
}

// The record back where it came from (a line: what a type op changes of it; a type layer: its record).
void put(Document& doc, const Text& text, StoryLine record) {
    if (text.target.line != nullptr) {
        StoryLine& line = *text.target.line;
        line.native_type = record.native_type;
        line.style_runs = record.style_runs;
        line.x_mm = record.x_mm;
        line.y_mm = record.y_mm;
        line.w_mm = record.w_mm;
        line.h_mm = record.h_mm;
        return;
    }
    doc.edit_page(text.target.page).layers[text.target.layer].type = core::type_text::shared_record(std::move(record));
}

// A record set again under the book's styles: a point text's box kept round its letters, one too large refused.
void refit(StoryLine& record, const Document& doc) {
    bounded_text(record.text);
    fit_point_text(record, doc);
    measure(record, doc);
}

// The attributes whose change sets no letter elsewhere: a style changed in these alone is not set again (review P2-5).
bool sets_nothing_anew(const std::set<std::string>& changed) {
    static const std::set<std::string> kLooks{"rgb", "underline", "strikethrough"};
    return std::all_of(changed.begin(), changed.end(), [](const std::string& key) { return kLooks.contains(key); });
}

// The attributes two styles' (or a style's and what is laid on it) differ in.
std::set<std::string> differing_keys(const Json& before, const Json& after) {
    std::set<std::string> out;
    for (const char* part : {"char", "para"}) {
        const Json a = before.is_object() ? before.value(part, Json::object()) : Json::object();
        const Json b = after.is_object() ? after.value(part, Json::object()) : Json::object();
        for (const auto& [key, value] : a.items()) {
            if (!b.contains(key) || b[key] != value) out.insert(key);
        }
        for (const auto& [key, value] : b.items()) {
            if (!a.contains(key)) out.insert(key);
        }
    }
    return out;
}

// Every text of the book that takes the style — its lines, its pages' type layers and the type layers in the contents of
// its smart objects (review P3-2) —, set again under the book's styles now when `measure` (a point text's box kept round
// its letters, one too large refused); else only counted (review P2-5: a colour changed sets nothing anew). How many.
std::int64_t refit_takers(Document& doc, ts::Kind kind, std::string_view id, bool measure) {
    std::int64_t count = 0;
    const auto touched = [&](const StoryLine& record) { return text::sets_opentype(record) && ts::takes(record, kind, id); };
    const auto same_box = [](const StoryLine& a, const StoryLine& b) { return a.x_mm == b.x_mm && a.y_mm == b.y_mm && a.w_mm == b.w_mm && a.h_mm == b.h_mm; };
    for (StoryLine& line : doc.story) {
        if (!touched(line)) continue;
        ++count;
        if (!measure) continue;
        StoryLine record = line;
        refit(record, doc);
        line.x_mm = record.x_mm;
        line.y_mm = record.y_mm;
        line.w_mm = record.w_mm;
        line.h_mm = record.h_mm;
    }
    const auto on_page = [&](const core::Page& page, const std::function<core::Page&()>& edit) {
        for (std::size_t j = 0; j < page.layers.size(); ++j) {
            const core::Layer& layer = page.layers[j];
            if (!core::type_text::is_type_layer(layer) || !layer.type || !touched(*layer.type)) continue;
            ++count;
            if (!measure) continue;
            StoryLine record = *layer.type;
            refit(record, doc);
            if (same_box(record, *layer.type)) continue;
            edit().layers[j].type = core::type_text::shared_record(std::move(record));
        }
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        on_page(doc.page(i), [&]() -> core::Page& { return doc.edit_page(i); });
    }
    std::vector<std::string> contents;
    for (const auto& [cid, sc] : doc.smart_contents) {
        if (sc && sc->page) contents.push_back(cid);
    }
    for (const std::string& cid : contents) {
        const std::shared_ptr<core::Page> held = doc.smart_contents.at(cid)->page;  // (kept while its copy is made)
        on_page(*held, [&]() -> core::Page& { return doc.edit_contents_page(cid); });
    }
    return count;
}

// Every text of the book that takes the style (the contents of smart objects too) with no reference to it, its look kept.
std::int64_t take_style_away(Document& doc, ts::Kind kind, std::string_view id, const Json* before, const Json* after) {
    std::int64_t count = 0;
    for (StoryLine& line : doc.story) {
        if (ts::take_away(line, kind, id, before, after)) ++count;
    }
    const auto on_page = [&](const core::Page& page, const std::function<core::Page&()>& edit) {
        for (std::size_t j = 0; j < page.layers.size(); ++j) {
            const core::Layer& layer = page.layers[j];
            if (!core::type_text::is_type_layer(layer) || !layer.type || !ts::takes(*layer.type, kind, id)) continue;
            StoryLine record = *layer.type;
            if (!ts::take_away(record, kind, id, before, after)) continue;
            ++count;
            edit().layers[j].type = core::type_text::shared_record(std::move(record));
        }
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) on_page(doc.page(i), [&]() -> core::Page& { return doc.edit_page(i); });
    std::vector<std::string> contents;
    for (const auto& [cid, sc] : doc.smart_contents) {
        if (sc && sc->page) contents.push_back(cid);
    }
    for (const std::string& cid : contents) {
        const core::Page& page = *doc.smart_contents.at(cid)->page;
        on_page(page, [&]() -> core::Page& { return doc.edit_contents_page(cid); });
    }
    return count;
}

// A style's attributes with those given laid on (null: taken away; `replace`: none of the old kept), `clear` taken away.
Json laid(const Json& own, const Json& attrs, bool replace, const std::vector<std::string>& clear) {
    Json out = replace || !own.is_object() ? Json::object() : own;
    for (const auto& [key, value] : attrs.items()) {
        if (value.is_null()) out.erase(key);
        else out[key] = value;
    }
    for (const std::string& key : clear) out.erase(key);
    return out;
}

Json without_nulls(const Json& attrs) {
    Json out = Json::object();
    for (const auto& [key, value] : attrs.items()) {
        if (!value.is_null()) out[key] = value;
    }
    return out;
}

// The kind's list in the styles (made when it is not there).
Json& list_of(Json& styles, ts::Kind kind) {
    if (!styles.is_object()) styles = Json::object();
    Json& list = styles[std::string(ts::kind_name(kind))];
    if (!list.is_array()) list = Json::array();
    return list;
}

// The style to change in the styles (Basic Paragraph: its "basic", made when it is not there).
Json& style_in(Json& styles, ts::Kind kind, const std::string& id) {
    if (kind == ts::Kind::Paragraph && id == ts::kBasic) {
        if (!styles.is_object()) styles = Json::object();
        Json& basic = styles["basic"];
        if (!basic.is_object()) basic = Json::object();
        return basic;
    }
    for (Json& s : list_of(styles, kind)) {
        if (s.value("id", std::string()) == id) return s;
    }
    throw OpError("no " + std::string(ts::kind_name(kind)) + " style " + id);
}

// Genko's words set in other sizes or colours (style_runs) written as ranges before their own attributes are cleared.
void words_first(StoryLine& record) {
    if (record.style_runs.empty()) return;
    Json nt = record.native_type ? *record.native_type : Json::object();
    words_to_runs(record, nt);
    set_native_type(record, nt);
}

constexpr std::array<std::string_view, 15> kKeys{"op", "kind", "action", "id", "name", "attrs", "replace", "clear", "line", "page", "layer_id",
                                                 "range", "from_selection", "apply", "clear_overrides"};

}  // namespace

bool type_style_asks(const Json& op) { return op.is_object() && op.value("action", Json()) == Json("list"); }

void type_style(core::OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    for (const auto& [key, value] : op.items()) {
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) throw OpError("unknown key " + key + " for type_style");
    }
    const Json* kind_given = given(op, "kind");
    if (kind_given == nullptr || (*kind_given != Json("character") && *kind_given != Json("paragraph")))
        throw OpError("kind is character or paragraph");
    const ts::Kind kind = *kind_given == Json("character") ? ts::Kind::Character : ts::Kind::Paragraph;
    const Json* action_given = given(op, "action");
    const std::string action = action_given != nullptr && action_given->is_string() ? action_given->get<std::string>() : std::string();
    static const std::vector<std::string> kActions{"new", "duplicate", "delete", "rename", "set", "apply", "redefine", "clear_override", "list"};
    if (std::find(kActions.begin(), kActions.end(), action) == kActions.end())
        throw OpError("action is new, duplicate, delete, rename, set, apply, redefine, clear_override or list");
    const Json* before = styles_of(doc);
    Json styles = before != nullptr ? *before : Json::object();

    if (action == "list") {  // (a question: OpRegistry::add_query)
        std::optional<Text> text = text_of(doc, op, false);
        Json out = Json{{"styles", ts::list(doc, kind)}};
        if (text) out["current"] = ts::current(text->record, before, text->range);
        c.report = std::move(out);
        return;
    }

    if (action == "new") {
        std::optional<Text> text = text_of(doc, op, flag(op, "apply", false));
        const ts::Attrs attrs = ts::split_attrs(given(op, "attrs") != nullptr ? op["attrs"] : Json());
        if (kind == ts::Kind::Character && !without_nulls(attrs.paras).empty()) throw OpError("a character style has no paragraph attributes");
        const std::string name = given(op, "name") != nullptr ? ts::checked_name(before, kind, op["name"])
                                                               : ts::unique_name(before, kind, std::string(kind == ts::Kind::Character ? ts::kCharacterBase : ts::kParagraphBase), true);
        Json chars = Json::object(), paras = Json::object();
        if (text && flag(op, "from_selection", true)) {  // (Photoshop starts a style from the text chosen)
            const Json f = ts::formatting_at(text->record, before, kind, text->range ? text->range->start : 0);
            chars = f["char"];
            paras = f["para"];
        }
        chars = laid(chars, attrs.chars, false, {});
        paras = laid(paras, attrs.paras, false, {});
        const std::string id = ts::next_id(before, kind);
        Json style = Json{{"id", id}, {"name", name}};
        if (!chars.empty()) style["char"] = chars;
        if (kind == ts::Kind::Paragraph && !paras.empty()) style["para"] = paras;
        list_of(styles, kind).push_back(std::move(style));
        keep_styles(doc, styles);
        if (flag(op, "apply", false)) {
            Text t = needed(std::move(text), "new with apply");
            if (kind == ts::Kind::Character && flag(op, "clear_overrides", false)) words_first(t.record);
            ts::restyle(t.record, styles_of(doc), kind, t.range, std::optional<std::string>(id), flag(op, "clear_overrides", false));
            refit(t.record, doc);
            put(doc, t, t.record);
        }
        core::type_text::add_feature(doc);
        c.report = Json{{"id", id}, {"name", name}};
        return;
    }

    if (action == "duplicate") {
        const std::string from = style_id(before, kind, op, true);
        const Json source = *ts::find(before, kind, from);
        const std::string base = (from == ts::kBasic ? std::string("基本段落") : source.value("name", std::string())) + " のコピー";
        const std::string name = given(op, "name") != nullptr ? ts::checked_name(before, kind, op["name"]) : ts::unique_name(before, kind, base, false);
        const std::string id = ts::next_id(before, kind);
        Json style = Json{{"id", id}, {"name", name}};
        if (source.contains("char")) style["char"] = source["char"];
        if (kind == ts::Kind::Paragraph && source.contains("para")) style["para"] = source["para"];
        Json& list = list_of(styles, kind);
        auto at = std::find_if(list.begin(), list.end(), [&](const Json& s) { return s.value("id", std::string()) == from; });
        list.insert(at == list.end() ? list.begin() : std::next(at), std::move(style));  // (just after it; Basic Paragraph's: first)
        keep_styles(doc, styles);
        core::type_text::add_feature(doc);
        c.report = Json{{"id", id}, {"name", name}};
        return;
    }

    if (action == "delete") {
        const std::string id = style_id(before, kind, op, false);
        Json& list = list_of(styles, kind);
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Json& s) { return s.value("id", std::string()) == id; }), list.end());
        const Json was = before != nullptr ? *before : Json(nullptr);
        keep_styles(doc, styles);
        // (the texts that took it keep their look: its attributes their own, as upstream's delete)
        const std::int64_t count = take_style_away(doc, kind, id, was.is_null() ? nullptr : &was, styles_of(doc));
        core::type_text::add_feature(doc);
        c.report = Json{{"deleted", id}, {"texts", count}};
        return;
    }

    if (action == "rename") {
        const std::string id = style_id(before, kind, op, false);
        if (given(op, "name") == nullptr) throw OpError("name gives the style its new name");
        const std::string name = ts::checked_name(before, kind, op["name"], id);
        style_in(styles, kind, id)["name"] = name;
        keep_styles(doc, styles);
        c.report = Json{{"id", id}, {"name", name}};
        return;
    }

    if (action == "set") {  // (スタイルオプション: the texts that take it are set again, their own attributes kept)
        const std::string id = style_id(before, kind, op, true);
        const ts::Attrs attrs = ts::split_attrs(given(op, "attrs") != nullptr ? op["attrs"] : Json());
        if (kind == ts::Kind::Character && !without_nulls(attrs.paras).empty()) throw OpError("a character style has no paragraph attributes");
        std::vector<std::string> clear;
        if (const Json* cl = given(op, "clear")) {
            if (!cl->is_array()) throw OpError("clear is a list of attribute names");
            for (const Json& name : *cl) {
                if (!name.is_string() || (core::type_text::char_attr(name.get<std::string>()) == nullptr && core::type_text::para_attr(name.get<std::string>()) == nullptr))
                    throw OpError("clear is a list of attribute names");
                clear.push_back(name.get<std::string>());
            }
        }
        const bool replace = flag(op, "replace", false);
        std::string name;
        if (given(op, "name") != nullptr) {
            if (id == ts::kBasic) throw OpError("Basic Paragraph cannot be deleted or renamed");
            name = ts::checked_name(before, kind, op["name"], id);
        }
        Json& style = style_in(styles, kind, id);
        const Json was = style;
        const Json chars = laid(style.value("char", Json::object()), attrs.chars, replace, clear);
        if (chars.empty()) style.erase("char");
        else style["char"] = chars;
        if (kind == ts::Kind::Paragraph) {
            const Json paras = laid(style.value("para", Json::object()), attrs.paras, replace, clear);
            if (paras.empty()) style.erase("para");
            else style["para"] = paras;
        }
        if (!name.empty()) style["name"] = name;
        const bool measure = !sets_nothing_anew(differing_keys(was, style));
        keep_styles(doc, styles);
        const std::int64_t count = refit_takers(doc, kind, id, measure);
        core::type_text::add_feature(doc);
        Json out = Json{{"id", id}, {"texts", count}};
        if (id != ts::kBasic) out["name"] = ts::find(styles_of(doc), kind, id)->value("name", std::string());
        c.report = std::move(out);
        return;
    }

    Text text = needed(text_of(doc, op, true), action);
    const bool clear = action == "clear_override" || flag(op, "clear_overrides", false);

    if (action == "apply" || action == "clear_override") {
        std::optional<std::optional<std::string>> set;
        if (action == "apply") {
            set = std::optional<std::string>();
            if (given(op, "id") != nullptr) {
                const std::string id = style_id(before, kind, op, true);
                if (id != ts::kBasic) set = std::optional<std::string>(id);
            }
        }
        if (clear && kind == ts::Kind::Character) words_first(text.record);
        if (clear && kind == ts::Kind::Paragraph) words_first(text.record);
        ts::restyle(text.record, before, kind, text.range, set, clear);
        refit(text.record, doc);
        const Json now = ts::current(text.record, before, text.range);
        put(doc, text, std::move(text.record));
        core::type_text::add_feature(doc);
        c.report = Json{{"current", now}};
        return;
    }

    // redefine (スタイルを再定義: the style takes the formatting at the start of the text chosen; that stretch then takes it
    // with nothing of its own left that the style now says; the other texts that take it change with it)
    const std::int64_t at = text.range ? text.range->start : 0;
    std::string id;
    if (given(op, "id") != nullptr) {
        id = style_id(before, kind, op, true);
    } else if (kind == ts::Kind::Character) {
        const std::optional<std::string> found = ts::character_style_at(text.record, at);
        if (!found) throw OpError("the chosen text takes no character style: name the style to redefine (id)");
        id = *found;
    } else {
        id = ts::paragraph_style_at(text.record, at);
    }
    const Json over = ts::redefinition_at(text.record, before, kind, id, at);
    Json& style = style_in(styles, kind, id);
    const Json chars = laid(style.value("char", Json::object()), over["char"], false, {});
    if (!chars.empty()) style["char"] = chars;
    if (kind == ts::Kind::Paragraph) {
        const Json paras = laid(style.value("para", Json::object()), over["para"], false, {});
        if (!paras.empty()) style["para"] = paras;
    }
    const ts::Range stretch = ts::stretch_at(text.record, before, at);
    keep_styles(doc, styles);
    const Json* now = styles_of(doc);
    const std::optional<std::optional<std::string>> set =
        std::optional<std::string>(id == ts::kBasic ? std::optional<std::string>() : std::optional<std::string>(id));
    if (kind == ts::Kind::Character) {
        ts::restyle(text.record, now, kind, stretch.end > stretch.start ? std::optional<ts::Range>(stretch) : std::nullopt, set, false);
    } else {
        ts::restyle(text.record, now, kind, ts::Range{at, at}, set, false);
    }
    const ts::Look applied = ts::look_of(text.record, now);
    if (kind == ts::Kind::Paragraph) {
        const std::vector<std::int64_t> ps = ts::paragraphs_of(text.record.text, at, at);
        ts::drop_same(text.record, now, kind, ts::paragraph_span(text.record.text, ps.empty() ? 0 : ps.front()), over["para"]);
    }
    ts::drop_same(text.record, now, ts::Kind::Character, stretch, over["char"]);
    ts::keep_look(text.record, now, applied);  // (what it drops says nothing the style does not: the look stays)
    put(doc, text, std::move(text.record));
    const std::int64_t count = refit_takers(doc, kind, id, !sets_nothing_anew(differing_keys(Json::object(), over)));
    core::type_text::add_feature(doc);
    c.report = Json{{"id", id}, {"texts", count}};
}

}  // namespace genko::render::type_ops
