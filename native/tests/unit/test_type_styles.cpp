// PC5 G-c (SPEC.md TEXT-02「文字スタイル・段落スタイル」, core/type_styles.hpp, render/ops_type_styles.cpp): the book's character and
// paragraph styles through type_style — upstream's type_styles_cmds tests' ideas (PhotoCraft v0.2.0, MIT) on Genko's records:
// - a character style's life: made, listed, applied to a range (the rest none), an override (+) made and told, the style
//   changed (the override kept), the override cleared (the style kept), renamed, duplicated, deleted (the text's look kept);
// - a style made from the text chosen and applied; redefined from the text: no override left;
// - paragraph styles and Basic Paragraph: a paragraph's style, its characters on the style's, Basic Paragraph changed moves
//   both, Basic Paragraph neither deleted nor renamed, bad ids and attributes refused, a paragraph style deleted (the look kept);
// - Genko's own: one character style for each letter — a whole text's style laid on its ranges when part of it takes
//   another (the look elsewhere kept to the value); なし on a range; an override cleared on a range; a style deleted keeps
//   every look; the styles inside a smart object's contents; Genko's words cleared;
// - list a question (nothing journalled; with a change refused); names (numbered, trimmed, one per kind, the panels' own
//   refused, 64 characters); the default formatting kept and read back (its form).

#include <QtTest>

#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ruby.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "render/ops_registry.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace ts = core::type_styles;

namespace {

constexpr double kInch = 25.4;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
// What the op reported (its result, without "index" and "op").
Json report(const core::Document& doc, const Json& op, core::Document* after = nullptr) {
    core::ApplyResult result = run_ops(doc, Json::array({op}));
    if (after != nullptr) *after = result.doc;
    Json out = result.results.empty() ? Json::object() : result.results[0];
    out.erase("index");
    out.erase("op");
    return out;
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document plain_book() {
    core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.frames.clear();
    p.numero = false;
    p.layers.clear();
    core::Layer paint;
    paint.id = "paint";
    paint.role = core::LayerRole::User;
    paint.kind = core::LayerKind::Raster;
    p.layers.push_back(paint);
    return doc;
}

// A type layer t1 (a point text across, 4 mm).
core::Document typed(const std::string& text, const core::Document& base = plain_book()) {
    return run(base, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", text}, {"point", Json::array({4, 10})}, {"wrap", "horizontal"},
                          {"new_id", "t1"}, {"style", Json{{"size_mm", 4}}}});
}

const core::StoryLine& record_of(const core::Document& doc, const std::string& id = "t1") {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return *l.type;
    throw std::runtime_error("no layer " + id);
}

const core::Json* styles_of(const core::Document& doc) { return doc.type_styles ? &*doc.type_styles : nullptr; }

// The attributes the letter at `at` takes (resolved), and its paragraph's.
Json chars_at(const core::Document& doc, std::int64_t at, const std::string& id = "t1") {
    for (const auto& run : core::type_text::resolve_chars(record_of(doc, id), styles_of(doc)))
        if (run.start <= at && at < run.end) return run.attrs;
    return Json();
}
Json para_of(const core::Document& doc, std::int64_t p, const std::string& id = "t1") {
    return core::type_text::resolve_para(record_of(doc, id), styles_of(doc), p);
}

Json style_op(const char* kind, const char* action, Json more = Json::object(), bool on_text = true) {
    Json op{{"op", "type_style"}, {"kind", kind}, {"action", action}};
    if (on_text) {
        op["page"] = 1;
        op["layer_id"] = "t1";
    }
    for (const auto& [key, value] : more.items()) op[key] = value;
    return op;
}

Json current(const core::Document& doc, const char* kind, Json range = Json()) {
    Json more = Json::object();
    if (!range.is_null()) more["range"] = range;
    return report(doc, style_op(kind, "list", more))["current"];
}

// The text's look (each stretch's and paragraph's attributes, resolved).
std::string look(const core::Document& doc, const std::string& id = "t1") {
    const ts::Look l = ts::look_of(record_of(doc, id), styles_of(doc));
    Json out = Json::array();
    for (const auto& r : l.chars) out.push_back(Json{{"s", r.start}, {"e", r.end}, {"a", r.attrs}});
    return out.dump() + Json(l.paras).dump();
}

}  // namespace

class TestTypeStyles : public QObject {
    Q_OBJECT

private slots:
    // upstream's character_style_lifecycle
    void characterLifecycle() {
        core::Document doc = typed("Hello world");
        core::Document made;
        const Json r = report(doc, style_op("character", "new", Json{{"attrs", Json{{"size_mm", 6}, {"underline", true}}}, {"from_selection", false}}), &made);
        QCOMPARE(r["id"], Json("cs-1"));
        QCOMPARE(r["name"], Json("文字スタイル 1"));
        doc = made;
        QVERIFY(std::find(doc.features.begin(), doc.features.end(), core::kTypeFeature) != doc.features.end());
        const Json listed = report(doc, style_op("character", "list", Json::object(), false));
        QCOMPARE(listed["styles"][0]["name"], Json("[なし]"));
        QCOMPARE(listed["styles"][0]["id"], Json(nullptr));
        QCOMPARE(listed["styles"][1]["char"], (Json{{"size_mm", 6}, {"underline", true}}));
        QCOMPARE(listed["styles"][1]["texts"], Json(0));
        QVERIFY(!listed.contains("current"));
        // applied to "world": its letters take the style; "Hello " keeps none
        doc = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({6, 11})}}));
        QCOMPARE(chars_at(doc, 7)["size_mm"], Json(6));
        QCOMPARE(chars_at(doc, 7)["underline"], Json(true));
        QCOMPARE(chars_at(doc, 0)["underline"], Json(false));
        QCOMPARE(chars_at(doc, 0)["size_mm"], Json(4));
        QVERIFY(!core::type_styles::character_style_at(record_of(doc), 0).has_value());
        Json cur = current(doc, "character", Json::array({6, 11}));
        QCOMPARE(cur["character"], Json("cs-1"));
        QCOMPARE(cur["character_override"], Json(false));
        QCOMPARE(current(doc, "character", Json::array({0, 11}))["character_mixed"], Json(true));
        QCOMPARE(report(doc, style_op("character", "list", Json::object(), false))["styles"][1]["texts"], Json(1));
        // a change of its own is an override (+); the style changed changes the text too, the override kept
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"range", Json::array({6, 8})}, {"faux_italic", true}});
        QCOMPARE(current(doc, "character", Json::array({6, 8}))["character_override"], Json(true));
        QCOMPARE(current(doc, "character", Json::array({8, 11}))["character_override"], Json(false));
        doc = run(doc, style_op("character", "set", Json{{"id", "cs-1"}, {"attrs", Json{{"size_mm", 8}}}}, false));
        QCOMPARE(chars_at(doc, 6)["size_mm"], Json(8));
        QCOMPARE(chars_at(doc, 6)["faux_italic"], Json(true));
        QCOMPARE(chars_at(doc, 9)["size_mm"], Json(8));
        QCOMPARE(chars_at(doc, 9)["faux_italic"], Json(false));
        QCOMPARE(chars_at(doc, 0)["size_mm"], Json(4));
        // 上書きを消去: the italic gone, the style kept
        doc = run(doc, style_op("character", "clear_override", Json{{"range", Json::array({6, 11})}}));
        QCOMPARE(chars_at(doc, 6)["faux_italic"], Json(false));
        QVERIFY(core::type_styles::character_style_at(record_of(doc), 6) == std::optional<std::string>("cs-1"));
        // renamed, duplicated (its copy just after it), deleted (the text keeps its look)
        doc = run(doc, style_op("character", "rename", Json{{"id", "cs-1"}, {"name", "強調"}}, false));
        core::Document copied;
        const Json d = report(doc, style_op("character", "duplicate", Json{{"id", "cs-1"}}, false), &copied);
        QCOMPARE(d["name"], Json("強調 のコピー"));
        QCOMPARE(d["id"], Json("cs-2"));
        doc = copied;
        const std::string before = look(doc);
        core::Document gone;
        const Json deleted = report(doc, style_op("character", "delete", Json{{"id", "cs-1"}}, false), &gone);
        QCOMPARE(deleted["texts"], Json(1));
        QCOMPARE(look(gone), before);
        QCOMPARE(chars_at(gone, 7)["size_mm"], Json(8));
        QVERIFY(!core::type_styles::character_style_at(record_of(gone), 7).has_value());
        QCOMPARE(report(gone, style_op("character", "list", Json::object(), false))["styles"].size(), std::size_t{2});
        // its texts no longer name it: the book's check holds
        core::type_text::validate_type(gone);
    }

    // upstream's new_style_from_selection_and_redefine
    void fromTheTextAndRedefined() {
        core::Document doc = typed("Title");
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"size_mm", 12}, {"rgb", Json::array({255, 0, 0})}});
        core::Document made;
        const Json r = report(doc, style_op("character", "new", Json{{"apply", true}}), &made);
        const Json* style = ts::find(styles_of(made), ts::Kind::Character, r["id"].get<std::string>());
        QVERIFY(style != nullptr);
        QCOMPARE((*style)["char"]["size_mm"], Json(12));
        QCOMPARE((*style)["char"]["rgb"], Json::array({255, 0, 0}));
        QVERIFY(!(*style)["char"].contains("tracking"));  // (what the book gives anyway is not the style's)
        Json cur = current(made, "character");
        QCOMPARE(cur["character"], r["id"]);
        QCOMPARE(cur["character_override"], Json(false));
        // the text changed, then the style redefined from it: no override left, the style says it now
        doc = run(made, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"range", Json::array({0, 5})}, {"size_mm", 15}});
        QCOMPARE(current(doc, "character")["character_override"], Json(true));
        const std::string before = look(doc);
        core::Document redefined;
        const Json rd = report(doc, style_op("character", "redefine"), &redefined);
        QCOMPARE(rd["id"], r["id"]);
        QCOMPARE(current(redefined, "character")["character_override"], Json(false));
        QCOMPARE((*ts::find(styles_of(redefined), ts::Kind::Character, "cs-1"))["char"]["size_mm"], Json(15));
        QCOMPARE(look(redefined), before);
        // a text with no character style: redefine names one
        const core::Document none = typed("abc");
        QVERIFY(refusal(none, style_op("character", "redefine")).find("takes no character style") != std::string::npos);
    }

    // upstream's paragraph_styles_and_basic_paragraph
    void paragraphsAndBasic() {
        core::Document doc = typed("One\nTwo");
        Json cur = current(doc, "paragraph");
        QCOMPARE(cur["paragraph"], Json("basic"));
        QCOMPARE(cur["paragraph_override"], Json(false));
        const Json listed = report(doc, style_op("paragraph", "list", Json::object(), false));
        QCOMPARE(listed["styles"][0]["id"], Json("basic"));
        QCOMPARE(listed["styles"][0]["name"], Json("[基本段落]"));
        core::Document made;
        const Json r = report(doc, style_op("paragraph", "new", Json{{"name", "見出し"}, {"from_selection", false},
                                                                    {"attrs", Json{{"align", "center"}, {"size_mm", 6}, {"faux_bold", "bold"}}}}),
                              &made);
        QCOMPARE(r["id"], Json("ps-1"));
        doc = run(made, style_op("paragraph", "apply", Json{{"id", "ps-1"}, {"range", Json::array({0, 1})}}));
        QCOMPARE(para_of(doc, 0)["align"], Json("center"));
        QVERIFY(para_of(doc, 1)["align"] != Json("center"));
        QCOMPARE(ts::paragraph_style_at(record_of(doc), 0), std::string("ps-1"));
        QCOMPARE(ts::paragraph_style_at(record_of(doc), 5), std::string("basic"));
        // the first paragraph's letters on the style's
        QCOMPARE(chars_at(doc, 0)["size_mm"], Json(6));
        QCOMPARE(chars_at(doc, 0)["faux_bold"], Json("bold"));
        QCOMPARE(chars_at(doc, 5)["size_mm"], Json(4));
        // Basic Paragraph changed: the second paragraph, and the first where its style says nothing
        doc = run(doc, style_op("paragraph", "set", Json{{"id", "basic"}, {"attrs", Json{{"space_before_mm", 2}}}}, false));
        QCOMPARE(para_of(doc, 1)["space_before_mm"], Json(2));
        QCOMPARE(para_of(doc, 0)["space_before_mm"], Json(2));
        // refused: Basic Paragraph deleted or renamed, a style the book has not, attributes not known or out of bounds
        QVERIFY(refusal(doc, style_op("paragraph", "delete", Json{{"id", "basic"}}, false)).find("Basic Paragraph cannot be deleted or renamed") != std::string::npos);
        QVERIFY(refusal(doc, style_op("paragraph", "rename", Json{{"id", "basic"}, {"name", "x"}}, false)).find("Basic Paragraph") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "apply", Json{{"id", "cs-99"}})).find("no character style cs-99") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"attrs", Json{{"bogus", 1}}}}, false)).find("unknown attribute bogus") != std::string::npos);
        QVERIFY(!refusal(doc, style_op("character", "new", Json{{"attrs", Json{{"size_mm", "big"}}}}, false)).empty());
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"attrs", Json{{"align", "center"}}}}, false)).find("no paragraph attributes") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"bogus", 1}})).find("unknown key bogus") != std::string::npos);
        QVERIFY(refusal(doc, style_op("letters", "list")).find("kind is character or paragraph") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "make")).find("action is new") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "apply", Json{{"range", Json::array({2, 2})}})).find("an empty range") != std::string::npos);
        QVERIFY(refusal(doc, style_op("paragraph", "apply", Json{{"id", "ps-1"}}, false)).find("names the text") != std::string::npos);
        // a paragraph style deleted: the look kept (center its own now)
        const std::string before = look(doc);
        const core::Document gone = run(doc, style_op("paragraph", "delete", Json{{"id", "ps-1"}}, false));
        QCOMPARE(look(gone), before);
        QCOMPARE(para_of(gone, 0)["align"], Json("center"));
        QCOMPARE(ts::paragraph_style_at(record_of(gone), 0), std::string("basic"));
        // Basic Paragraph applied to the first paragraph with its overrides cleared: as the second
        const core::Document plain = run(doc, style_op("paragraph", "apply", Json{{"range", Json::array({0, 0})}, {"clear_overrides", true}}));
        QCOMPARE(para_of(plain, 0), para_of(plain, 1));
        QCOMPARE(chars_at(plain, 0)["size_mm"], Json(4));
    }

    // Genko's: a whole text's style laid on its ranges when part of it takes another; the rest looks as it did
    void oneStyleALetter() {
        core::Document doc = typed("abcdef");
        doc = run(doc, style_op("character", "new", Json{{"name", "W"}, {"from_selection", false}, {"attrs", Json{{"size_mm", 5}, {"tracking", 0.2}}}}, false));
        doc = run(doc, style_op("character", "new", Json{{"name", "S"}, {"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        doc = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}}));  // (the whole: its char_style)
        QCOMPARE(record_of(doc).native_type->value("char_style", Json()), Json("cs-1"));
        // the whole text's own size over the style's
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"size_mm", 7}});
        const ts::Look before = ts::look_of(record_of(doc), styles_of(doc));
        const core::Document s = run(doc, style_op("character", "apply", Json{{"id", "cs-2"}, {"range", Json::array({2, 4})}}));
        // outside: as before, to the value
        QCOMPARE(chars_at(s, 0), before.chars.front().attrs);
        QCOMPARE(chars_at(s, 5), before.chars.front().attrs);
        // inside: S alone (Photoshop's runs take one style), the text's own size kept (an override)
        QCOMPARE(chars_at(s, 2)["underline"], Json(true));
        QCOMPARE(chars_at(s, 2)["tracking"], Json(0));
        QCOMPARE(chars_at(s, 2)["size_mm"], Json(7));
        QCOMPARE(current(s, "character", Json::array({2, 4}))["character"], Json("cs-2"));
        QCOMPARE(current(s, "character", Json::array({0, 2}))["character"], Json("cs-1"));
        // なし on a range: W's attributes gone there, the size its own
        const core::Document n = run(doc, style_op("character", "apply", Json{{"range", Json::array({0, 1})}}));
        QCOMPARE(chars_at(n, 0)["tracking"], Json(0));
        QCOMPARE(chars_at(n, 0)["size_mm"], Json(7));
        QCOMPARE(chars_at(n, 3), before.chars.front().attrs);
        QCOMPARE(current(n, "character", Json::array({0, 1}))["character"], Json(nullptr));
        QCOMPARE(current(n, "character", Json::array({0, 1}))["character_mixed"], Json(false));
        // an override cleared on a range: the size there the style's, elsewhere the text's own
        const core::Document c = run(doc, style_op("character", "clear_override", Json{{"range", Json::array({0, 3})}}));
        QCOMPARE(chars_at(c, 0)["size_mm"], Json(5));
        QCOMPARE(chars_at(c, 4)["size_mm"], Json(7));
        // with apply's clear_overrides (Alt): the same
        const core::Document a = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({0, 3})}, {"clear_overrides", true}}));
        QCOMPARE(chars_at(a, 0)["size_mm"], Json(5));
        QCOMPARE(chars_at(a, 4)["size_mm"], Json(7));
        // the style deleted: every look kept
        for (const core::Document* d : {&s, &n, &c}) {
            const std::string was = look(*d);
            QCOMPARE(look(run(*d, style_op("character", "delete", Json{{"id", "cs-1"}}, false))), was);
            QCOMPARE(look(run(*d, style_op("character", "delete", Json{{"id", "cs-2"}}, false))), was);
        }
    }

    // paragraphs: a whole text's paragraph style laid on its paragraphs when one takes another; its own cleared on one
    void paragraphsApart() {
        core::Document doc = typed("a\nb\nc");
        doc = run(doc, style_op("paragraph", "new", Json{{"name", "P"}, {"from_selection", false}, {"attrs", Json{{"first_indent_mm", 2}, {"rgb", Json::array({0, 0, 255})}}}}, false));
        doc = run(doc, style_op("paragraph", "new", Json{{"name", "Q"}, {"from_selection", false}, {"attrs", Json{{"space_after_mm", 3}}}}, false));
        doc = run(doc, style_op("paragraph", "apply", Json{{"id", "ps-1"}}));
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"end_indent_mm", 1}});
        const std::string was0 = para_of(doc, 0).dump(), was2 = para_of(doc, 2).dump();
        const core::Document q = run(doc, style_op("paragraph", "apply", Json{{"id", "ps-2"}, {"range", Json::array({2, 3})}}));
        QCOMPARE(para_of(q, 0).dump(), was0);
        QCOMPARE(para_of(q, 2).dump(), was2);
        QCOMPARE(para_of(q, 1)["space_after_mm"], Json(3));
        QCOMPARE(para_of(q, 1)["first_indent_mm"], Json(0));
        QCOMPARE(para_of(q, 1)["end_indent_mm"], Json(1));  // (the text's own, kept)
        QCOMPARE(chars_at(q, 0)["rgb"], Json::array({0, 0, 255}));
        QCOMPARE(chars_at(q, 2)["rgb"], Json(nullptr));
        QCOMPARE(current(q, "paragraph", Json::array({0, 5}))["paragraph_mixed"], Json(true));
        // the paragraph's override cleared: its own end indent gone there only
        const core::Document c = run(doc, style_op("paragraph", "clear_override", Json{{"range", Json::array({2, 2})}}));
        QCOMPARE(para_of(c, 1)["end_indent_mm"], Json(0));
        QCOMPARE(para_of(c, 0)["end_indent_mm"], Json(1));
        QCOMPARE(para_of(c, 2)["end_indent_mm"], Json(1));
        QCOMPARE(current(c, "paragraph", Json::array({2, 2}))["paragraph_override"], Json(false));
        QCOMPARE(current(c, "paragraph", Json::array({0, 0}))["paragraph_override"], Json(true));
        // a paragraph style redefined from the text: what its paragraph says past the style
        const core::Document r = run(doc, style_op("paragraph", "redefine", Json{{"range", Json::array({0, 0})}}));
        QCOMPARE((*ts::find(styles_of(r), ts::Kind::Paragraph, "ps-1"))["para"]["end_indent_mm"], Json(1));
        QCOMPARE(current(r, "paragraph", Json::array({0, 0}))["paragraph_override"], Json(false));
        QCOMPARE(para_of(r, 2)["end_indent_mm"], Json(1));
    }

    // list changes nothing: a question, asked alone (no journal); with an op that changes the book refused
    void listIsAQuestion() {
        const core::Document doc = typed("abc");
        const core::ApplyResult asked = run_ops(doc, Json::array({style_op("character", "list")}));
        QVERIFY(asked.unchanged);
        QVERIFY(asked.journal_ops.empty());
        QVERIFY(!asked.doc.type_styles);
        const core::ApplyResult made = run_ops(doc, Json::array({style_op("character", "new", Json::object(), false)}));
        QVERIFY(!made.unchanged);
        QCOMPARE(made.journal_ops.size(), std::size_t{1});
        try {
            run_ops(doc, Json::array({style_op("character", "list"), style_op("character", "new", Json::object(), false)}));
            QFAIL("a question with a change");
        } catch (const core::ApplyError& error) {
            QVERIFY(std::string(error.what()).find("is a question") != std::string::npos);
        }
    }

    // the styles' names: numbered, trimmed, one per kind, the panels' own refused, 64 characters at most
    void names() {
        core::Document doc = typed("abc");
        doc = run(doc, style_op("character", "new", Json::object(), false));
        QCOMPARE(report(doc, style_op("character", "new", Json::object(), false))["name"], Json("文字スタイル 2"));
        QCOMPARE(report(doc, style_op("paragraph", "new", Json::object(), false))["name"], Json("段落スタイル 1"));
        QCOMPARE(report(doc, style_op("character", "new", Json{{"name", "　強調 "}}, false))["name"], Json("強調"));
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", "文字スタイル 1"}}, false)).find("named 文字スタイル 1 already") != std::string::npos);
        QVERIFY(report(doc, style_op("paragraph", "new", Json{{"name", "文字スタイル 1"}}, false)).contains("id"));  // (one per kind)
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", "[なし]"}}, false)).find("the panel's own") != std::string::npos);
        QVERIFY(refusal(doc, style_op("paragraph", "new", Json{{"name", "[基本段落]"}}, false)).find("the panel's own") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", " "}}, false)).find("1 to 64 characters") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", std::string(65, 'x')}}, false)).find("1 to 64 characters") != std::string::npos);
        // a copy of a name of 64 characters: cut so its number fits
        doc = run(doc, style_op("character", "new", Json{{"name", std::string(64, 'y')}}, false));
        const Json copy = report(doc, style_op("character", "duplicate", Json{{"id", "cs-2"}}, false));
        QVERIFY(core::ruby::count(copy["name"].get<std::string>()) <= 64);
        // renamed to its own name: fine; to another's: refused
        QVERIFY(report(doc, style_op("character", "rename", Json{{"id", "cs-1"}, {"name", "文字スタイル 1"}}, false)).contains("id"));
        QVERIFY(!refusal(doc, style_op("character", "rename", Json{{"id", "cs-1"}, {"name", std::string(64, 'y')}}, false)).empty());
        // the copy of Basic Paragraph: a paragraph style first in the list
        core::Document based;
        const Json b = report(doc, style_op("paragraph", "duplicate", Json{{"id", "basic"}}, false), &based);
        QCOMPARE(b["name"], Json("基本段落 のコピー"));
        QCOMPARE((*based.type_styles)["paragraph"][0]["id"], b["id"]);
    }

    // a style deleted: the type layers inside a smart object's contents no longer name it, their look kept
    void insideSmartObjects() {
        core::Document doc = typed("abc");
        doc = run(doc, style_op("character", "new", Json{{"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        doc = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({0, 1})}}));
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        const core::Document gone = run(doc, style_op("character", "delete", Json{{"id", "cs-1"}}, false));
        bool seen = false;
        for (const core::StoryLine* r : ts::texts(gone)) {
            QVERIFY(!ts::takes(*r, ts::Kind::Character, "cs-1"));
            seen = true;
            QCOMPARE(core::type_text::resolve_chars(*r, styles_of(gone)).front().attrs["underline"], Json(true));
        }
        QVERIFY(seen);
    }

    // a line takes styles as a type layer does; Genko's words (style_runs) cleared as its overrides
    void linesAndWords() {
        core::Document doc = plain_book();
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "大きな声"}, {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 30}, {"h_mm", 30},
                            {"style_runs", Json::array({Json::array({"大きな", Json{{"scale", 1.5}}})})}});
        doc = run(doc, style_op("character", "new", Json{{"from_selection", false}, {"attrs", Json{{"rgb", Json::array({200, 0, 0})}}}}, false));
        Json op{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}, {"id", "cs-1"}, {"line", "l1"}};
        doc = run(doc, op);
        const core::StoryLine& line = doc.story[0];
        QCOMPARE(line.native_type->value("char_style", Json()), Json("cs-1"));
        QCOMPARE(core::type_text::resolve_chars(line, styles_of(doc)).front().attrs["rgb"], Json::array({200, 0, 0}));
        const core::Document cleared = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "clear_override"}, {"line", "l1"}});
        QVERIFY(cleared.story[0].style_runs.empty());
        QCOMPARE(core::type_text::resolve_chars(cleared.story[0], styles_of(cleared)).front().attrs["scale"], Json(1));
    }

    // 初期設定の書式: a text's formatting at its start, in its form; what is not one read as none
    void defaultFormatting() {
        core::Document doc = typed("abc");
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"size_mm", 9}, {"align", "center"}});
        const Json kept = ts::defaults_of(record_of(doc), styles_of(doc));
        QCOMPARE(kept["char"]["size_mm"], Json(9));
        QCOMPARE(kept["para"]["align"], Json("center"));
        QCOMPARE(ts::checked_defaults(kept), kept);
        QVERIFY(ts::checked_defaults(Json{{"char", Json{{"size_mm", "big"}}}}).is_null());
        QVERIFY(ts::checked_defaults(Json(7)).is_null());
        QVERIFY(ts::checked_defaults(Json::object()).is_null());
        // laid on another text as the window does (its styles and own attributes cleared, then set_type): it looks the same
        core::Document other = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "xyz"}, {"point", Json::array({4, 30})}, {"new_id", "t2"}});
        other = run(other, style_op("paragraph", "new", Json{{"from_selection", false}, {"attrs", Json{{"space_after_mm", 4}}}}, false));
        other = run(other, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "apply"}, {"id", "ps-1"}, {"page", 1}, {"layer_id", "t2"}});
        const Json target{{"page", 1}, {"layer_id", "t2"}};
        Json clear{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "apply"}, {"clear_overrides", true}};
        Json none{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}};
        Json set{{"op", "set_type"}, {"char", kept["char"]}, {"para", kept["para"]}};
        for (Json* op : {&clear, &none, &set})
            for (const auto& [key, value] : target.items()) (*op)[key] = value;
        const core::Document laid = run_ops(other, Json::array({clear, none, set})).doc;
        QCOMPARE(ts::defaults_of(record_of(laid, "t2"), styles_of(laid)), kept);
    }

    // --- the review's (PC5 G-c review) -----------------------------------------------------------------------------------

    // P1-1: a smart object's contents change no style of the book (refused, the book still whole); a style applied there
    void insideContentsRefused() {
        core::Document doc = typed("abc");
        doc = run(doc, style_op("character", "new", Json{{"name", "W"}, {"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        QCOMPARE(doc.smart_contents.size(), std::size_t{1});
        const std::string contents = doc.smart_contents.begin()->first;
        const auto inside = [&](Json op) {
            op["contents"] = contents;
            return op;
        };
        for (const Json& op : {style_op("character", "new", Json{{"apply", true}}), style_op("character", "new", Json::object(), false),
                               style_op("character", "set", Json{{"id", "cs-1"}, {"attrs", Json{{"underline", false}}}}, false),
                               style_op("character", "delete", Json{{"id", "cs-1"}}, false), style_op("character", "rename", Json{{"id", "cs-1"}, {"name", "x"}}, false),
                               style_op("character", "duplicate", Json{{"id", "cs-1"}}, false), style_op("character", "redefine", Json{{"id", "cs-1"}}),
                               style_op("paragraph", "set", Json{{"id", "basic"}, {"attrs", Json{{"align", "center"}}}}, false)}) {
            const std::string why = refusal(doc, inside(op));
            QVERIFY2(why.find("cannot be used inside smart object contents") != std::string::npos, (op.dump() + " → " + why).c_str());
        }
        // what changes the record alone: in the contents, the book whole (its texts name only the styles it keeps)
        const core::Document applied = run(doc, inside(style_op("character", "apply", Json{{"id", "cs-1"}})));
        core::type_text::validate_type(applied);
        bool took = false;
        for (const core::StoryLine* r : ts::texts(applied)) took = took || ts::takes(*r, ts::Kind::Character, "cs-1");
        QVERIFY(took);
        const Json asked = report(applied, inside(style_op("character", "list")));
        QCOMPARE(asked["current"]["character"], Json("cs-1"));
    }

    // P2-1: a style of the book changed asks the locks of the pages whose texts take it (another's lock refuses)
    void otherLocksAsked() {
        core::Document doc = typed("abc");
        doc = run(doc, style_op("character", "new", Json{{"name", "W"}, {"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        doc = run(doc, style_op("character", "new", Json{{"name", "free"}, {"from_selection", false}}, false));
        doc = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}}));
        doc = core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", "lock_page"}, {"page", 1}}}), core::Actor("human:bob")).doc;
        for (const Json& op : {style_op("character", "set", Json{{"id", "cs-1"}, {"attrs", Json{{"underline", false}}}}, false),
                               style_op("character", "delete", Json{{"id", "cs-1"}}, false),
                               style_op("paragraph", "set", Json{{"id", "basic"}, {"attrs", Json{{"space_after_mm", 1}}}}, false)}) {
            const std::string why = refusal(doc, op);
            QVERIFY2(why.find("locked") != std::string::npos, (op.dump() + " → " + why).c_str());
        }
        // a style no text of a locked page takes: changed
        QVERIFY(refusal(doc, style_op("character", "set", Json{{"id", "cs-2"}, {"attrs", Json{{"underline", true}}}}, false)).empty());
        QVERIFY(refusal(doc, style_op("character", "rename", Json{{"id", "cs-1"}, {"name", "強調"}}, false)).empty());
    }

    // P2-2: a style taken away from a line set by the OpenType typesetting leaves it so (never the typesetting of before)
    void deletedKeepsTheTypesetting() {
        core::Document doc = plain_book();
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l0"}, {"text", "abc"}, {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 30}, {"h_mm", 20}, {"wrap", "horizontal"}});
        doc = run(doc, style_op("character", "new", Json{{"from_selection", false}}, false));
        doc = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}, {"id", "cs-1"}, {"line", "l0"}});
        const core::Document gone = run(doc, style_op("character", "delete", Json{{"id", "cs-1"}}, false));
        QVERIFY(gone.story[0].native_type.has_value());
        QCOMPARE((*gone.story[0].native_type)["shaping"], Json("opentype"));
        // [なし] on the whole: the same
        const core::Document none = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}, {"line", "l0"}});
        QCOMPARE((*none.story[0].native_type)["shaping"], Json("opentype"));
        // a line of before given a style and back: as a line of before is never touched by restyle's taking away
        const core::Document plain = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l0"}, {"text", "abc"}, {"x_mm", 5}, {"y_mm", 5}});
        QVERIFY(!plain.story[0].native_type.has_value());
    }

    // P2-3: redefined from a text whose whole says it: nothing of its own left, the style changed changes it
    void redefinedLeavesNothing() {
        core::Document doc = typed("abcdef");
        doc = run(doc, style_op("character", "new", Json{{"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        doc = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}}));
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"rgb", Json::array({255, 0, 0})}});
        const std::string before = look(doc);
        doc = run(doc, style_op("character", "redefine"));
        QCOMPARE(look(doc), before);
        QVERIFY(!record_of(doc).native_type->contains("char") || !(*record_of(doc).native_type)["char"].contains("rgb"));
        doc = run(doc, style_op("character", "set", Json{{"id", "cs-1"}, {"attrs", Json{{"rgb", Json::array({0, 0, 255})}}}}, false));
        QCOMPARE(chars_at(doc, 3)["rgb"], Json::array({0, 0, 255}));
        // part of the text: the rest keeps the whole text's own, the stretch takes the style's
        core::Document part = typed("abcdef");
        part = run(part, style_op("character", "new", Json{{"from_selection", false}, {"attrs", Json{{"underline", true}}}}, false));
        part = run(part, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"rgb", Json::array({255, 0, 0})}});
        part = run(part, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"range", Json::array({0, 2})}, {"underline", true}});
        part = run(part, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({0, 2})}}));
        const std::string was = look(part);
        part = run(part, style_op("character", "redefine", Json{{"range", Json::array({0, 2})}}));
        QCOMPARE(look(part), was);
        part = run(part, style_op("character", "set", Json{{"id", "cs-1"}, {"attrs", Json{{"rgb", Json::array({0, 0, 255})}}}}, false));
        QCOMPARE(chars_at(part, 0)["rgb"], Json::array({0, 0, 255}));
        QCOMPARE(chars_at(part, 4)["rgb"], Json::array({255, 0, 0}));
        // a paragraph style redefined from a text whose whole says it, on one paragraph of two
        core::Document paras = typed("a\nb");
        paras = run(paras, style_op("paragraph", "new", Json{{"from_selection", false}, {"attrs", Json{{"space_after_mm", 1}}}}, false));
        paras = run(paras, style_op("paragraph", "apply", Json{{"id", "ps-1"}, {"range", Json::array({0, 0})}}));
        paras = run(paras, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"end_indent_mm", 2}});
        const std::string had = look(paras);
        paras = run(paras, style_op("paragraph", "redefine", Json{{"range", Json::array({0, 0})}}));
        QCOMPARE(look(paras), had);
        QVERIFY(!record_of(paras).native_type->contains("para"));
        paras = run(paras, style_op("paragraph", "set", Json{{"id", "ps-1"}, {"attrs", Json{{"end_indent_mm", 3}}}}, false));
        QCOMPARE(para_of(paras, 0)["end_indent_mm"], Json(3));
        QCOMPARE(para_of(paras, 1)["end_indent_mm"], Json(2));
    }

    // P2-4: a text of 2000 paragraphs and 100 000 letters: a style applied, its look asked, the styles listed — quickly
    void longTexts() {
        std::string text;
        for (int p = 0; p < 2000; ++p) text += std::string(49, 'a') + "\n";
        core::StoryLine record;
        record.text = text;
        record.style = Json::object();
        Json styles = Json{{"paragraph", Json::array({Json{{"id", "ps-1"}, {"name", "P"}, {"para", Json{{"space_after_mm", 1}}}}})},
                           {"character", Json::array({Json{{"id", "cs-1"}, {"name", "C"}, {"char", Json{{"underline", true}}}}})}};
        QElapsedTimer clock;
        clock.start();
        ts::restyle(record, &styles, ts::Kind::Paragraph, std::nullopt, std::optional<std::string>("ps-1"), false);
        ts::restyle(record, &styles, ts::Kind::Paragraph, ts::Range{50, 120}, std::optional<std::string>(), false);  // (two paragraphs Basic: laid on each)
        ts::restyle(record, &styles, ts::Kind::Character, ts::Range{10, 90000}, std::optional<std::string>("cs-1"), false);
        const qint64 applied = clock.elapsed();
        clock.restart();
        const Json now = ts::current(record, &styles, std::nullopt);
        (void)ts::current(record, &styles, ts::Range{40, 60});
        const qint64 asked = clock.elapsed();
        QCOMPARE(now["paragraph_mixed"], Json(true));
        QVERIFY2(applied < 3000, qPrintable(QString::number(applied)));
        QVERIFY2(asked < 1000, qPrintable(QString::number(asked)));
        // through the book: list (a question) of a text so long
        core::Document doc = plain_book();
        doc.type_styles = core::type_text::check_type_styles(styles);
        core::StoryLine& line = doc.add_line(core::Num(1), text, "", std::nullopt, "", core::Num(5), core::Num(5), core::Num(40), core::Num(40), "none");
        line.id = "long";
        line.native_type = record.native_type;
        clock.restart();
        const core::ApplyResult listed = run_ops(doc, Json::array({Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "list"}, {"line", "long"}}}));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(listed.results[0]["current"]["paragraph_mixed"], Json(true));
    }

    // P2-5: a colour changed sets nothing anew: Basic Paragraph's colour on a book of 500 lines, quickly
    void coloursSetNothingAnew() {
        core::Document doc = plain_book();
        Json ops = Json::array();
        for (int i = 0; i < 500; ++i)
            ops.push_back(Json{{"op", "add_line"}, {"page", 1}, {"id", "l" + std::to_string(i)}, {"text", "あいうえお"}, {"x_mm", 5}, {"y_mm", 5},
                               {"native_type", Json{{"shaping", "opentype"}}}});
        doc = run_ops(doc, ops).doc;
        QElapsedTimer clock;
        clock.start();
        core::Document set;
        const Json r = report(doc, style_op("paragraph", "set", Json{{"id", "basic"}, {"attrs", Json{{"rgb", Json::array({200, 0, 0})}}}}, false), &set);
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(r["texts"], Json(500));
    }

    // P3-2: a point text in a smart object's contents set again when its style grows (its box round its letters)
    void contentsSetAnew() {
        core::Document doc = typed("abc");
        doc = run(doc, style_op("paragraph", "new", Json{{"from_selection", false}, {"attrs", Json{{"size_mm", 4}}}}, false));
        doc = run(doc, style_op("paragraph", "apply", Json{{"id", "ps-1"}}));
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        const auto width = [](const core::Document& d) {
            for (const core::StoryLine* r : ts::texts(d))
                if (r->native_type && r->native_type->contains("anchor_mm")) return r->w_mm.value();
            return 0.0;
        };
        const double w = width(doc);
        const core::Document big = run(doc, style_op("paragraph", "set", Json{{"id", "ps-1"}, {"attrs", Json{{"size_mm", 10}}}}, false));
        QVERIFY2(width(big) > 2 * w, qPrintable(QString::number(width(big))));
    }

    // P3-5: applied without clearing, the whole text's own still wins in the range; a whole style and a range's style both
    // held (a PSD's, by hand): the new style's attributes win in the range
    void overridesKept() {
        core::Document doc = typed("abcdef");
        doc = run(doc, style_op("character", "new", Json{{"from_selection", false}, {"attrs", Json{{"rgb", Json::array({0, 0, 255})}, {"underline", true}}}}, false));
        doc = run(doc, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"rgb", Json::array({255, 0, 0})}});
        const core::Document a = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({0, 2})}}));
        QCOMPARE(chars_at(a, 0)["rgb"], Json::array({255, 0, 0}));
        QCOMPARE(chars_at(a, 0)["underline"], Json(true));
        const core::Document cleared = run(doc, style_op("character", "apply", Json{{"id", "cs-1"}, {"range", Json::array({0, 2})}, {"clear_overrides", true}}));
        QCOMPARE(chars_at(cleared, 0)["rgb"], Json::array({0, 0, 255}));
        QCOMPARE(chars_at(cleared, 4)["rgb"], Json::array({255, 0, 0}));
        // a whole style W and a range's S both held; T applied over the range: T's size there
        core::Document both = typed("abcdef");
        for (const auto& [name, size] : {std::pair{"W", 5}, std::pair{"S", 6}, std::pair{"T", 7}})
            both = run(both, style_op("character", "new", Json{{"name", name}, {"from_selection", false}, {"attrs", Json{{"size_mm", size}}}}, false));
        both = run(both, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"text", "abcdef"}});
        Json nt = *record_of(both).native_type;
        nt["char_style"] = "cs-1";
        nt["runs"] = Json::array({Json{{"start", 0}, {"end", 2}, {"style", "cs-2"}}});
        for (core::Layer& l : both.edit_page(0).layers)
            if (l.id == "t1") {
                core::StoryLine r = *l.type;
                r.native_type = core::type_text::check_native_type(nt, r.text);
                l.type = core::type_text::shared_record(r);
            }
        const core::Document t = run(both, style_op("character", "apply", Json{{"id", "cs-3"}, {"range", Json::array({0, 2})}}));
        QCOMPARE(chars_at(t, 0)["size_mm"], Json(7));
        QCOMPARE(chars_at(t, 4)["size_mm"], Json(5));
    }

    // P3-7: list is a question: asked while the book's pages are still being read; a change is not
    void askedWhileReading() {
        core::Document doc = typed("abc");
        doc = run(doc, Json{{"op", "add_page"}});
        doc.deferred = {doc.pages.back()};
        QVERIFY(refusal(doc, style_op("character", "list", Json::object(), false)).empty());
        QVERIFY(refusal(doc, style_op("character", "list")).empty());  // (of a page read)
        QVERIFY(refusal(doc, style_op("character", "new", Json::object(), false)).find("still being read") != std::string::npos);
    }

    // P3-8: names without control characters, one spelling (NFC)
    void namesNormal() {
        core::Document doc = typed("abc");
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", "a\nb"}}, false)).find("control character") != std::string::npos);
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", "a\tb"}}, false)).find("control character") != std::string::npos);
        doc = run(doc, style_op("character", "new", Json{{"name", "か\xE3\x82\x99"}}, false));  // (か + the combining voiced mark)
        QCOMPARE((*doc.type_styles)["character"][0]["name"], Json("が"));
        QVERIFY(refusal(doc, style_op("character", "new", Json{{"name", "が"}}, false)).find("already") != std::string::npos);
    }

    // P3-11: an op a question by its arguments — asked alone, a dry run; asks false: a change; registered again by add: no question
    void questionsByArguments() {
        core::OpRegistry registry;
        int ran = 0;
        registry.add_query("probe", [&ran](core::OpContext& c) {
            ++ran;
            if (c.op.value("action", Json()) != Json("ask")) c.doc.title = "changed";
            c.report = Json{{"ran", ran}};
        }, [](const Json& op) { return op.value("action", Json()) == Json("ask"); });
        QVERIFY(registry.is_query(Json{{"op", "probe"}, {"action", "ask"}}));
        QVERIFY(!registry.is_query(Json{{"op", "probe"}, {"action", "do"}}));
        QVERIFY(!registry.is_query(Json{{"op", "other"}}));
        QVERIFY(!registry.is_query(Json(3)));
        const core::Document doc = plain_book();
        const core::CommandBus bus(registry);
        const core::ApplyResult asked = bus.apply(doc, Json::array({Json{{"op", "probe"}, {"action", "ask"}}}), core::Actor("human:test"));
        QVERIFY(asked.unchanged && asked.journal_ops.empty());
        const core::ApplyResult done = bus.apply(doc, Json::array({Json{{"op", "probe"}, {"action", "do"}}}), core::Actor("human:test"));
        QVERIFY(!done.unchanged);
        QCOMPARE(done.journal_ops.size(), std::size_t{1});
        QCOMPARE(done.doc.title, std::string("changed"));
        // (its function given again — render::ops_registry wraps each op —: still a question by its arguments)
        registry.add("probe", [](core::OpContext&) {});
        QVERIFY(registry.is_query(Json{{"op", "probe"}, {"action", "ask"}}));
        QVERIFY(!registry.is_query(Json{{"op", "probe"}, {"action", "do"}}));
        // made a question by its name: whatever its arguments
        registry.add_query("probe", [](core::OpContext&) {});
        QVERIFY(registry.is_query(Json{{"op", "probe"}, {"action", "do"}}));
        // the built registry (wrapped): type_style's list a question, its other actions not
        QVERIFY(render::ops_registry().is_query(Json{{"op", "type_style"}, {"action", "list"}}));
        QVERIFY(!render::ops_registry().is_query(Json{{"op", "type_style"}, {"action", "set"}}));
        QVERIFY(render::ops_registry().is_query(Json{{"op", "type_info"}}));
    }
};

QTEST_GUILESS_MAIN(TestTypeStyles)
#include "test_type_styles.moc"
