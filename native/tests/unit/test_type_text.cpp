// PC5 G-0 (type@1, SPEC.md TEXT-02, core/type_text.hpp): the record of Photoshop's type without Python — the attributes'
// tables and their normal form (each known and in its bounds, in the table's order, null taken away), native_type checked
// against its text (ranges inside it, in order, none overlapping; paragraphs by number), the book's type styles, the order
// the attributes are resolved in (defaults → the line's style → Basic Paragraph → the paragraph's style → its own → the
// whole text's character style → its own char → the range's style → the range's char), the ranges following an edit of the
// text (before, after, inside, across, taken away; replace_text's matches; paragraphs joined), a line's style_runs as ranges,
// a line's record as a type layer holds it, and the HarfBuzz way in (glyph ids and advances of the bundled faces, across and
// down, a feature turned on).

#include <QElapsedTimer>
#include <QtTest>

#include <functional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/ops_registry.hpp"
#include "render/text/shaping.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::core::StoryLine;
namespace tt = genko::core::type_text;

namespace {

std::string refusal_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const genko::core::Error& error) {
        return error.what();
    }
    return "(none)";
}

StoryLine line_of(const std::string& text, const Json& native = nullptr, const Json& style = Json::object()) {
    StoryLine line;
    line.id = "l1";
    line.page_index = genko::core::Num(1);
    line.text = text;
    line.style = style;
    if (!native.is_null()) line.native_type = tt::check_native_type(native, text);
    return line;
}

const Json* attr_at(const std::vector<tt::CharRun>& runs, std::int64_t i) {
    for (const tt::CharRun& r : runs)
        if (r.start <= i && i < r.end) return &r.attrs;
    return nullptr;
}

}  // namespace

class TestTypeText : public QObject {
    Q_OBJECT

private slots:
    // the tables: G-b's and G-c's names, each once, and the defaults name every attribute
    void tables() {
        QCOMPARE(tt::char_attrs().size(), std::size_t{25});
        QCOMPARE(tt::para_attrs().size(), std::size_t{12});
        for (const tt::Attr& a : tt::char_attrs()) QVERIFY2(tt::char_defaults().contains(std::string(a.name)), std::string(a.name).c_str());
        for (const tt::Attr& a : tt::para_attrs()) QVERIFY2(tt::para_defaults().contains(std::string(a.name)), std::string(a.name).c_str());
        QCOMPARE(tt::char_defaults().size(), tt::char_attrs().size());
        QCOMPARE(tt::para_defaults().size(), tt::para_attrs().size());
        QVERIFY(tt::char_attr("size_mm") != nullptr && tt::char_attr("leading") == nullptr);
        QCOMPARE(tt::antialias_methods().size(), std::size_t{7});
        QCOMPARE(tt::para_defaults()["auto_leading"].get<double>(), 1.2);
    }

    // the normal form: the table's order, numbers kept to 4 decimals (a whole one an integer), features sorted, a list of
    // tags taken as each on, faux_bold's toggle, null taken away; what is out of bounds or unknown refused
    void normalForm() {
        const Json c = tt::check_char(Json{{"underline", true}, {"size_mm", 4.000001}, {"features", Json::array({"liga", "frac"})}, {"faux_bold", true},
                                           {"tracking", nullptr}, {"rgb", Json::array({1, 2, 3})}});
        QCOMPARE(c.dump(), std::string(R"({"size_mm":4,"rgb":[1,2,3],"faux_bold":"bold","underline":true,"features":{"frac":1,"liga":1}})"));
        QVERIFY(refusal_of([] { tt::check_char(Json{{"size_mm", 0}}); }).find("char size_mm must be between 0.1 and 1000") != std::string::npos);
        QVERIFY(refusal_of([] { tt::check_char(Json{{"leading", 1}}); }).find("unknown char attribute leading") != std::string::npos);
        QVERIFY(refusal_of([] { tt::check_char(Json{{"caps", "huge"}}); }).find("char caps must be one of normal, small, all") != std::string::npos);
        QVERIFY(refusal_of([] { tt::check_char(Json{{"features", Json{{"li", 1}}}}); }).find("is not a tag of 4 letters") != std::string::npos);
        QVERIFY(refusal_of([] { tt::check_char(Json{{"rgb", Json::array({0, 0, 256})}}); }).find("three integers") != std::string::npos);
        QCOMPARE(tt::check_char(Json{{"leading_mm", "auto"}}).dump(), std::string(R"({"leading_mm":"auto"})"));
        QVERIFY(refusal_of([] { tt::check_para(Json{{"kinsoku", "loose"}}); }).find("para kinsoku must be one of genko, none, weak, strong") != std::string::npos);
        QCOMPARE(tt::check_para(Json{{"direction", "rtl"}, {"align", "justify_all"}}).dump(), std::string(R"({"align":"justify_all","direction":"rtl"})"));
    }

    // native_type against its text: the default anti-alias not written, ranges inside the text and in order, paragraphs by
    // number, a run or paragraph of nothing gone, nothing at all null
    void nativeType() {
        const std::string text = "あいう\nえお";  // (6 graphemes, 2 paragraphs)
        QCOMPARE(tt::paragraph_count(text), std::int64_t{2});
        QCOMPARE(tt::paragraph_start(text, 1), std::int64_t{4});
        const Json n = tt::check_native_type(Json{{"runs", Json::array({Json{{"start", 0}, {"end", 2}, {"char", Json{{"underline", true}}}},
                                                                        Json{{"start", 3}, {"end", 4}}})},
                                                  {"antialias", "smooth"}, {"shape", "box"}, {"paragraphs", Json::array({Json{{"index", 1}, {"para", Json{{"align", "center"}}}}})}},
                                             text);
        QCOMPARE(n.dump(), std::string(R"({"shape":"box","runs":[{"start":0,"end":2,"char":{"underline":true}}],"paragraphs":[{"index":1,"para":{"align":"center"}}]})"));
        QVERIFY(tt::check_native_type(Json{{"antialias", "smooth"}}, text).is_null());
        QVERIFY(tt::native_type_normal(n, text));
        Json sorted = Json::object();  // (a history snapshot's keys sorted: still the normal form)
        for (const char* key : {"paragraphs", "runs", "shape"}) sorted[key] = n[key];
        QVERIFY(tt::native_type_normal(sorted, text));
        QVERIFY(!tt::native_type_normal(Json{{"antialias", "smooth"}, {"shape", "box"}}, text));
        QVERIFY(refusal_of([&] { tt::check_native_type(Json{{"runs", Json::array({Json{{"start", 0}, {"end", 7}, {"char", Json{{"tcy", true}}}}})}}, text); })
                    .find("the runs must lie in the text") != std::string::npos);
        QVERIFY(refusal_of([&] {
                    tt::check_native_type(Json{{"runs", Json::array({Json{{"start", 0}, {"end", 3}, {"char", Json{{"tcy", true}}}},
                                                                     Json{{"start", 2}, {"end", 4}, {"char", Json{{"tcy", true}}}}})}},
                                          text);
                }).find("none overlapping") != std::string::npos);
        QVERIFY(refusal_of([&] { tt::check_native_type(Json{{"paragraphs", Json::array({Json{{"index", 2}, {"para", Json{{"yakumono", false}}}}})}}, text); })
                    .find("the paragraphs must be the text's") != std::string::npos);
        QVERIFY(refusal_of([&] { tt::check_native_type(Json{{"char_style", "ps-1"}}, text); }).find("a style's id") != std::string::npos);
        QVERIFY(refusal_of([&] { tt::check_native_type(Json{{"bold", true}}, text); }).find("unknown native_type key bold") != std::string::npos);
        QVERIFY(refusal_of([&] { tt::check_native_type(Json{{"transform", Json::array({1, 0, 2, 0, 0, 0})}}, text); }).find("flat") != std::string::npos);
    }

    void typeStyles() {
        const Json s = tt::check_type_styles(Json{{"basic", Json{{"char", Json{{"size_mm", 3}}}}},
                                                  {"character", Json::array({Json{{"name", "強調"}, {"id", "cs-1"}, {"char", Json{{"underline", true}}}}})},
                                                  {"paragraph", Json::array()}});
        QCOMPARE(s.dump(), std::string(R"({"character":[{"id":"cs-1","name":"強調","char":{"underline":true}}],"basic":{"char":{"size_mm":3}}})"));
        QVERIFY(tt::type_styles_normal(s));
        QVERIFY(tt::check_type_styles(Json::object()).is_null());
        QVERIFY(refusal_of([] {
                    tt::check_type_styles(Json{{"character", Json::array({Json{{"id", "cs-1"}, {"name", "a"}}, Json{{"id", "cs-2"}, {"name", "a"}}})}});
                }).find("two character styles have the name a") != std::string::npos);
        QVERIFY(refusal_of([] { tt::check_type_styles(Json{{"paragraph", Json::array({Json{{"id", "cs-1"}, {"name", "a"}}})}}); }).find("ps-1") !=
                std::string::npos);
        QVERIFY(refusal_of([] { tt::check_type_styles(Json{{"character", Json::array({Json{{"id", "cs-1"}, {"name", ""}}})}}); }).find("1 to 64") !=
                std::string::npos);
    }

    // the order of resolution: each later one over the one before, where it says something
    void resolutionOrder() {
        const Json styles = tt::check_type_styles(Json{
            {"character", Json::array({Json{{"id", "cs-1"}, {"name", "全体"}, {"char", Json{{"size_mm", 5}, {"underline", true}}}},
                                       Json{{"id", "cs-2"}, {"name", "範囲"}, {"char", Json{{"size_mm", 6}, {"caps", "all"}}}}})},
            {"paragraph", Json::array({Json{{"id", "ps-1"}, {"name", "語り"}, {"para", Json{{"align", "center"}}}, {"char", Json{{"tracking", 0.5}}}}})},
            {"basic", Json{{"para", Json{{"kinsoku", "strong"}, {"align", "right"}}}, {"char", Json{{"size_mm", 4}, {"tracking", 0.2}}}}}});
        const std::string text = "あいうえ\nかき";
        const StoryLine line = line_of(text,
                                       Json{{"char_style", "cs-1"},
                                            {"char", Json{{"strikethrough", true}}},
                                            {"para", Json{{"space_after_mm", 2}}},
                                            {"runs", Json::array({Json{{"start", 1}, {"end", 3}, {"style", "cs-2"}, {"char", Json{{"size_mm", 7}}}}})},
                                            {"paragraphs", Json::array({Json{{"index", 1}, {"style", "ps-1"}, {"para", Json{{"align", "left"}}}}})}},
                                       Json{{"size_mm", 3}, {"align", "top"}, {"weight", "heavy"}});
        // the paragraphs: 0 the basic over the style's align, 1 its style over the basic, its own over its style
        const Json p0 = tt::resolve_para(line, &styles, 0);
        QCOMPARE(p0["align"], Json("right"));
        QCOMPARE(p0["kinsoku"], Json("strong"));
        QCOMPARE(p0["space_after_mm"], Json(2));
        const Json p1 = tt::resolve_para(line, &styles, 1);
        QCOMPARE(p1["align"], Json("left"));
        QCOMPARE(p1["kinsoku"], Json("strong"));
        // the letters
        const std::vector<tt::CharRun> runs = tt::resolve_chars(line, &styles);
        QCOMPARE((*attr_at(runs, 0))["size_mm"], Json(5));        // (the whole text's style over the basic over the line's style)
        QCOMPARE((*attr_at(runs, 0))["underline"], Json(true));
        QCOMPARE((*attr_at(runs, 0))["strikethrough"], Json(true));
        QCOMPARE((*attr_at(runs, 0))["faux_bold"], Json("heavy"));  // (the line's style: its weight)
        QCOMPARE((*attr_at(runs, 0))["tracking"], Json(0.2));
        QCOMPARE((*attr_at(runs, 1))["size_mm"], Json(7));  // (the range's own over its style)
        QCOMPARE((*attr_at(runs, 1))["caps"], Json("all"));
        QCOMPARE((*attr_at(runs, 3))["size_mm"], Json(5));
        QCOMPARE((*attr_at(runs, 5))["tracking"], Json(0.5));  // (paragraph 1's style's letters)
        QCOMPARE(runs.front().start, std::int64_t{0});
        QCOMPARE(runs.back().end, std::int64_t{7});
        for (std::size_t i = 1; i < runs.size(); ++i) QVERIFY(runs[i].start == runs[i - 1].end && runs[i].attrs != runs[i - 1].attrs);
        // a line of before (no native_type): its style alone
        const std::vector<tt::CharRun> plain = tt::resolve_chars(line_of("あい", nullptr, Json{{"size_mm", 3}}), nullptr);
        QCOMPARE(plain.size(), std::size_t{1});
        QCOMPARE(plain.front().attrs["size_mm"], Json(3));
        QCOMPARE(plain.front().attrs["language"], Json("ja"));
    }

    // the ranges through edits: before moves, after leaves, inside widens, taken away goes; an insertion at a range's
    // start moves it, at its end leaves it
    void following() {
        const Json n{{"runs", Json::array({Json{{"start", 2}, {"end", 4}, {"char", Json{{"underline", true}}}}})}};
        const auto after_edit = [&](const std::string& before, const std::string& after) {
            const Json f = tt::follow(n, before, after, {tt::edit_between(before, after)});
            return f.is_null() ? std::string("null") : f["runs"].dump();
        };
        QCOMPARE(after_edit("あいうえお", "Xあいうえお"), std::string(R"([{"start":3,"end":5,"char":{"underline":true}}])"));
        QCOMPARE(after_edit("あいうえお", "あいうえおX"), std::string(R"([{"start":2,"end":4,"char":{"underline":true}}])"));
        QCOMPARE(after_edit("あいうえお", "あいXうえお"), std::string(R"([{"start":3,"end":5,"char":{"underline":true}}])"));  // (just before)
        QCOMPARE(after_edit("あいうえお", "あいうえXお"), std::string(R"([{"start":2,"end":4,"char":{"underline":true}}])"));  // (at its end)
        QCOMPARE(after_edit("あいうえお", "あいうXYえお"), std::string(R"([{"start":2,"end":6,"char":{"underline":true}}])"));  // (inside)
        QCOMPARE(after_edit("あいうえお", "あいお"), std::string("null"));  // (taken away)
        QCOMPARE(after_edit("あいうえお", "あいうお"), std::string(R"([{"start":2,"end":3,"char":{"underline":true}}])"));  // (its end cut)
        QCOMPARE(after_edit("あいうえお", "あXえお"), std::string(R"([{"start":2,"end":3,"char":{"underline":true}}])"));  // (its start replaced)
        // edits at both ends (as edit_line's whole new text gives them): each found by the longest common subsequence
        const std::vector<tt::Edit> both = tt::edits_between("あいうえお", "XあいうえおY");
        QCOMPARE(both.size(), std::size_t{2});
        QCOMPARE(tt::follow(n, "あいうえお", "XあいうえおY", both)["runs"].dump(), std::string(R"([{"start":3,"end":5,"char":{"underline":true}}])"));
        QCOMPARE(tt::follow(n, "あいうえお", "XあいZうえおY", tt::edits_between("あいうえお", "XあいZうえおY"))["runs"].dump(),
                 std::string(R"([{"start":4,"end":6,"char":{"underline":true}}])"));
        QVERIFY(tt::edits_between("あい", "あい").empty());
        // graphemes, not code points: a letter with its variation selector is one
        QCOMPARE(after_edit("葛\U000E0100いうえお", "X葛\U000E0100いうえお"), std::string(R"([{"start":3,"end":5,"char":{"underline":true}}])"));
        // replace_text's matches, as code points
        const std::vector<genko::core::ruby::Replacement> edits{{0, 1, U"XYZ"}, {4, 5, U""}};
        const Json r = tt::follow(n, "あいうえお", "XYZいうえ", tt::edits_of("あいうえお", "XYZいうえ", edits));
        QCOMPARE(r["runs"].dump(), std::string(R"([{"start":4,"end":6,"char":{"underline":true}}])"));
        // paragraphs: a line break added before moves its number, one taken away joins it to the one before (its own gone)
        const Json p{{"paragraphs", Json::array({Json{{"index", 1}, {"para", Json{{"align", "center"}}}}})}};
        QCOMPARE(tt::follow(p, "あ\nい", "\nあ\nい", {tt::edit_between("あ\nい", "\nあ\nい")})["paragraphs"].dump(),
                 std::string(R"([{"index":2,"para":{"align":"center"}}])"));
        QVERIFY(tt::follow(p, "あ\nい", "あい", {tt::edit_between("あ\nい", "あい")}).is_null());
        QCOMPARE(tt::follow(p, "あ\nい", "あ\nいう", {tt::edit_between("あ\nい", "あ\nいう")})["paragraphs"].dump(),
                 std::string(R"([{"index":1,"para":{"align":"center"}}])"));
    }

    // (review P2-5, P2-6, P3-11) a long text edited at both ends keeps the ranges between (Myers' script, no table of its
    // square); many matches of replace_text followed in linear time; a combining mark a replacement puts in counted where it
    // joins; past the script's bound the ranges an edit takes away are counted (removed_by)
    void longTexts() {
        std::string body;
        for (int i = 0; i < 2500; ++i) body += "あ";
        const Json n{{"runs", Json::array({Json{{"start", 100}, {"end", 200}, {"char", Json{{"underline", true}}}}})},
                     {"paragraphs", Json::array()}};
        const std::string after = "X" + body + "Y";
        const Json f = tt::follow(n, body, after, tt::edits_between(body, after));
        QCOMPARE(f["runs"].dump(), std::string(R"([{"start":101,"end":201,"char":{"underline":true}}])"));
        // many matches: 20000 letters, 10000 replaced, 2000 ranges — in well under a second
        std::string many, replaced;
        Json runs = Json::array();
        std::vector<genko::core::ruby::Replacement> reps;
        for (int i = 0; i < 10000; ++i) {
            many += "あ。";
            replaced += "あ！？";
            reps.push_back(genko::core::ruby::Replacement{static_cast<std::size_t>(2 * i + 1), static_cast<std::size_t>(2 * i + 2), U"！？"});
        }
        for (int i = 0; i < 2000; ++i) runs.push_back(Json{{"start", 10 * i}, {"end", 10 * i + 2}, {"char", Json{{"tcy", true}}}});
        QElapsedTimer clock;
        clock.start();
        const Json m = tt::follow(Json{{"runs", runs}}, many, replaced, tt::edits_of(many, replaced, reps));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(m["runs"].size(), std::size_t{2000});
        QCOMPARE(m["runs"][1]["start"], Json(15));  // (letter 10 of the text before is letter 15 after: 5 more "？" before it)
        // a combining mark put in after a letter joins it: one grapheme made of the letter, nothing new
        const std::vector<genko::core::ruby::Replacement> mark{{1, 1, U"́"}};
        const std::vector<tt::Edit> joined = tt::edits_of("eab", "éab", mark);
        QCOMPARE(joined.size(), std::size_t{1});
        QCOMPARE(tt::follow(Json{{"runs", Json::array({Json{{"start", 1}, {"end", 3}, {"char", Json{{"tcy", true}}}}})}}, "eab", "éab", joined)["runs"].dump(),
                 std::string(R"([{"start":1,"end":3,"char":{"tcy":true}}])"));
        QCOMPARE(tt::removed_by(n, Json()), std::int64_t{1});
    }

    // style_runs as ranges: found by their words from where the last one was, their scale, colour, weight and tcy
    void styleRunsAsRanges() {
        StoryLine line = line_of("ドーンと来たドーン");
        line.style_runs = {{"ドーン", Json{{"scale", 1.5}, {"bold", true}}}, {"ドーン", Json{{"rgb", Json::array({200, 0, 0})}}}, {"無い", Json{{"scale", 2}}}};
        QCOMPARE(tt::runs_from_style_runs(line).dump(),
                 std::string(R"([{"start":0,"end":3,"char":{"scale":1.5,"faux_bold":"bold"}},{"start":6,"end":9,"char":{"rgb":[200,0,0]}}])"));
    }

    // a line's record as a type layer holds it, and back as a line to draw
    void records() {
        StoryLine line = line_of("あ", Json{{"shape", "box"}});
        line.speaker = "太郎";
        line.frame_id = "f1";
        const StoryLine record = tt::record_of(line);
        QVERIFY(record.id.empty() && record.speaker.empty() && !record.frame_id);
        QCOMPARE(record.text, line.text);
        QVERIFY(record.native_type == line.native_type);
        genko::core::Layer layer;
        layer.id = "t1";
        layer.kind = genko::core::LayerKind::Type;
        layer.type = tt::shared_record(record);
        const StoryLine drawn = tt::line_of(layer, genko::core::Num(3));
        QCOMPARE(drawn.id, std::string("t1"));
        QVERIFY(drawn.page_index == genko::core::Num(3));
        const StoryLine moved = tt::moved(record, 5, -2);
        QCOMPARE(moved.x_mm.value(), record.x_mm.value() + 5);
        QCOMPARE(moved.y_mm.value(), record.y_mm.value() - 2);
        QCOMPARE(genko::core::to_string(genko::core::LayerKind::Type), std::string_view("type"));
        QVERIFY(genko::core::layer_kind_from("type") == genko::core::LayerKind::Type);
    }

    // the HarfBuzz way in: the bundled faces' glyphs and advances, across and down; ids that name glyphs, a run's clusters
    // its code points; a feature turned off changes nothing a face has not; a size not above 0 refused
    void shaping() {
        namespace text = genko::render::text;
        QVERIFY(!text::harfbuzz_version().empty());
        text::ShapeRequest across;
        across.text = "あいA";
        across.font = "gothic";
        across.size_px = 100;
        across.direction = "ltr";
        const std::vector<text::ShapedGlyph> a = text::shape(across);
        QCOMPARE(a.size(), std::size_t{3});
        for (const text::ShapedGlyph& g : a) QVERIFY(g.glyph != 0 && g.x_advance > 0);
        QCOMPARE(a[0].cluster, std::uint32_t{0});
        QCOMPARE(a[2].cluster, std::uint32_t{2});
        QVERIFY(std::abs(a[0].x_advance - 100) < 1);  // (a full-width kana: one em)
        text::ShapeRequest down = across;
        down.text = "あ。";
        down.direction = "ttb";
        down.language = "ja";
        const std::vector<text::ShapedGlyph> d = text::shape(down);
        QCOMPARE(d.size(), std::size_t{2});
        QVERIFY(d[0].y_advance < 0);
        text::ShapeRequest off = across;
        off.features = {{"kern", 0}};
        QCOMPARE(text::shape(off).size(), a.size());
        text::ShapeRequest none = across;
        none.size_px = 0;
        QVERIFY(refusal_of([&] { text::shape(none); }).find("above 0") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestTypeText)
#include "test_type_text.moc"
