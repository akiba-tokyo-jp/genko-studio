// PC5 G-b (SPEC.md TEXT-02「OpenType の組み方」「set_type」「右から左」, the coordinator's decision for G and the user's decision 9
// of 2026-10-28): the OpenType typesetting through the ops and the drawing.
// - The lines of before (no OpenType features; down, any) are drawn as before, pixel for pixel: their pages' SHA-256 as the
//   build before G-b drew them (native/tests/data/type/plain_lines.json, recorded with GENKO_RECORD_PLAIN_LINES by that
//   build's lettering), and no HarfBuzz shaping on their way.
// - A line set across whose style asks for OpenType features is drawn now (Pillow's BASIC layout drew none of its letters:
//   "text_features"), by HarfBuzz: its features change its glyphs; down, such a line is drawn as before.
// - set_type: the whole text's and a range's character attributes, the paragraphs' attributes, null taking one away, the
//   record marked for the OpenType typesetting, its refusals; type_info's resolved attributes.
// - The attributes drawn: size, colour, underline and strikethrough, baseline shift, vertical scale, faux italic and bold,
//   capitals, tracking, kerning, the weight axis, leading, indents, the space between paragraphs, alignment.
// - Right-to-left text set across (SheenBidi): a Hebrew word right to left, a right-to-left paragraph's Latin word at its
//   left, the paragraph's direction asked, its alignment at its start.
// - Down: the grid as before, a cell's glyph HarfBuzz's only where it chooses another (kana as before).
// - type.fonts: the computer's fonts by family with their PostScript names and axes.
// - Saved and read back as set.
// - The review's (PC5 G-b review): a text set too large refused, never drawn out of memory (P1-1); a line's rows centred as
//   before (P2-1); a word's 縦中横 kept by a range's set_type (P2-2); the book's styles in the memo's key (P2-3); Unicode's
//   paragraph separators in a right-to-left text (P2-4); a text of 100 000 letters set in time, one longer refused (P2-5);
//   a word's weight, the whole text's colour over Genko's words, a line's features one by one, a size against the line's,
//   a type layer's auto leading (P3-1, P3-2, P3-4, P3-11, P3-12).

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/type_text.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/text/bidi.hpp"
#include "render/text/lettering.hpp"
#include "render/text/opentype.hpp"
#include "render/text/shaping.hpp"
#include "service/briefs.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

constexpr double kInch = 25.4;
constexpr int kDpi = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

std::string font(const char* name) { return test::test_data(QStringLiteral("type/%1").arg(QString::fromLatin1(name))).toStdString(); }
std::string inter() { return font("inter-subset.ttf"); }
std::string hebrew() { return font("noto-sans-hebrew-subset.ttf"); }

// A colour page `w` by `h` mm (200 dpi), no panels, no nombre, one empty paint layer.
core::Document plain_book(double w = 2 * kInch, double h = 2 * kInch) {
    core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(w, h, 40, 40, 1, 2, 2, 2, 2, kDpi, "color"));
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

render::Image drawn(const core::Document& doc, std::size_t page = 0, int dpi = kDpi) {
    return render::render_page(doc.page(page), dpi, render::proof_options(), &doc).image;
}
std::string bytes(const core::Document& doc) { return drawn(doc).tobytes(); }

// Where the page's letters are drawn (pixels darker than half): x0, y0, x1, y1 mm; none when none. `red`: the red ones.
std::optional<std::array<double, 4>> ink_mm(const core::Document& doc, bool red = false) {
    const render::Image image = drawn(doc).convert("RGB");
    const std::string px = image.tobytes();
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = reinterpret_cast<const unsigned char*>(px.data()) + (std::size_t(y) * image.width() + x) * 3;
            const bool hit = red ? (p[0] > 160 && p[1] < 100 && p[2] < 100) : p[0] + p[1] + p[2] < 3 * 128;
            if (!hit) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 < 0) return std::nullopt;
    const double k = kInch / kDpi;
    return std::array<double, 4>{x0 * k, y0 * k, x1 * k, y1 * k};
}

double darkness(const core::Document& doc) {
    const std::string px = drawn(doc).convert("L").tobytes();
    double sum = 0;
    for (const char c : px) sum += 255 - static_cast<unsigned char>(c);
    return sum / static_cast<double>(px.size()) / 255.0;
}

// A type layer of the Inter test font, set across from a point (its anchor) or in a box.
core::Document latin(const std::string& words, const Json& more = Json::object(), const core::Document& base = plain_book()) {
    Json op{{"op", "add_type_layer"}, {"page", 1}, {"text", words}, {"point", Json::array({5, 20})}, {"wrap", "horizontal"}, {"new_id", "t1"},
            {"style", Json{{"size_mm", 6}, {"font", inter()}}}};
    for (const auto& [key, value] : more.items()) op[key] = value;
    return run(base, op);
}

core::Document set(const core::Document& doc, const Json& attrs, const std::string& layer = "t1") {
    Json op{{"op", "set_type"}, {"page", 1}, {"layer_id", layer}};
    for (const auto& [key, value] : attrs.items()) op[key] = value;
    return run(doc, op);
}

core::StoryLine record_of(const core::Document& doc, const std::string& id = "t1") {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return *l.type;
    throw std::runtime_error("no layer " + id);
}

// The lettering's letters as set (render::text::SetLetters), each its box's left (px at kDpi), in the text's order.
std::vector<double> lefts(const core::StoryLine& record) {
    std::vector<double> out;
    for (const auto& row : render::text::set_layout(record, kDpi, std::nullopt, render::text::LetterParts{}).set.rows)
        for (const auto& ch : row.chars) out.push_back(ch.box.x0);
    return out;
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

// The lines of before, each alone on a page of its own (after `before`: the page made precise or of inks, the book's text
// gamma): what their pages are drawn as is kept (plain_lines.json).
struct Plain {
    const char* name;
    Json op;
    Json before = Json::array();
};
std::vector<Plain> plain_lines() {
    const auto line = [](Json more) {
        Json op{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"x_mm", 6}, {"y_mm", 6}, {"w_mm", 38}, {"h_mm", 38}};
        for (const auto& [key, value] : more.items()) op[key] = value;
        return op;
    };
    return {
        {"across", line(Json{{"text", "こんにちは、世界。"}, {"wrap", "horizontal"}})},
        {"down", line(Json{{"text", "まんが作りのモヤモヤを晴らす"}})},
        {"down_ruby_emphasis", line(Json{{"text", "東京へ行こう！"}, {"ruby_spans", Json::array({Json{{"start", 0}, {"end", 2}, {"ruby", "とうきょう"}}})},
                                         {"emphasis_runs", Json::array({"行こう"})}})},
        {"across_words", line(Json{{"text", "大きな声で言う"}, {"wrap", "horizontal"}, {"balloon", "box"},
                                   {"style_runs", Json::array({Json::array({"大きな", Json{{"scale", 1.5}, {"bold", true}}})})},
                                   {"style", Json{{"tracking", 0.1}, {"leading", 0.3}, {"align", "left"}}}})},
        {"latin", line(Json{{"text", "Hello, World! AV office"}, {"wrap", "horizontal"}, {"balloon", "none"}})},
        {"sfx", line(Json{{"text", "ドドド"}, {"balloon", "sfx"}})},
        {"leant", line(Json{{"text", "ななめの文字"}, {"wrap", "horizontal"}, {"style", Json{{"italic", true}, {"scale_x", 0.8}, {"outline_mm", 0.5}}}})},
        {"tcy", line(Json{{"text", "12時に来て"}, {"style", Json{{"tcy", true}}}})},
        {"no_features", line(Json{{"text", "空の機能"}, {"wrap", "horizontal"}, {"style", Json{{"features", Json::array()}}}})},
        {"down_features", line(Json{{"text", "辻と葛"}, {"style", Json{{"features", Json::array({"jp78"})}}}})},
        {"justified_ruby", line(Json{{"text", "均等に揃えた文で、ルビも振ります"}, {"wrap", "horizontal"}, {"balloon", "box"}, {"w_mm", 30},
                                     {"ruby_runs", Json::array({Json::array({"揃", "そろ"})})}, {"style", Json{{"align", "justify"}}}})},
        {"text_only", line(Json{{"text", "一行目\n二行目の文"}, {"wrap", "horizontal"}, {"balloon", "none"}})},
        // (PC5 G-b review P3-14: a precise page, a page of inks, the book's text gamma, 傍点 and rubies across)
        {"precise16", line(Json{{"text", "高い精度の頁、Hello"}, {"wrap", "horizontal"}, {"balloon", "box"}}),
         Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})},
        {"inks", line(Json{{"text", "インキの頁です"}, {"emphasis_runs", Json::array({"インキ"})}}),
         Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}}})},
        {"text_gamma", line(Json{{"text", "文字のガンマ"}, {"wrap", "horizontal"}, {"style", Json{{"rgb", Json::array({200, 40, 40})}}}}),
         Json::array({Json{{"op", "set_text_gamma"}, {"gamma", 1.45}}})},
        {"across_emphasis", line(Json{{"text", "ここが大事なところ"}, {"wrap", "horizontal"}, {"emphasis_runs", Json::array({"大事"})}})},
        {"across_ruby_spans", line(Json{{"text", "東京へ行こう"}, {"wrap", "horizontal"},
                                        {"ruby_spans", Json::array({Json{{"start", 0}, {"end", 2}, {"ruby", "とうきょう"}}})}})},
    };
}

std::string sha(const std::string& data) {
    return QCryptographicHash::hash(QByteArray::fromStdString(data), QCryptographicHash::Sha256).toHex().toStdString();
}

}  // namespace

class TestContractOpenType : public QObject {
    Q_OBJECT

private slots:
    // the lines of before: their pages drawn as the build before G-b drew them, and none of them shaped by HarfBuzz
    void linesOfBeforeAsBefore() {
        const QString path = test::test_data(QStringLiteral("type/plain_lines.json"));
        const QByteArray record = qgetenv("GENKO_RECORD_PLAIN_LINES");
        Json kept = Json::object();
        if (record.isEmpty()) kept = test::read_json(path);
        Json made = Json::object();
        for (const Plain& p : plain_lines()) {
            Json ops = p.before;
            ops.push_back(p.op);
            const core::Document doc = run_ops(plain_book(50, 50), ops).doc;
            QVERIFY2(!render::text::sets_opentype(doc.story.at(0)), p.name);
            made[p.name] = sha(drawn(doc, 0, 150).tobytes());
            if (record.isEmpty()) QVERIFY2(kept.value(p.name, Json()) == made[p.name], p.name);
        }
        if (!record.isEmpty()) {
            std::ofstream out(record.toStdString(), std::ios::binary);
            out << made.dump(1) << "\n";
        }
    }

    // a line set across asking for OpenType features is drawn (none of its letters were); down, as without them
    void featuresAcross() {
        const core::Document base = plain_book();
        const Json line{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "ABC漢字"}, {"wrap", "horizontal"}, {"balloon", "box"}, {"x_mm", 10},
                        {"y_mm", 10}, {"w_mm", 30}, {"h_mm", 15}, {"style", Json{{"features", Json::array({"jp78"})}}}};
        const core::Document doc = run(base, line);
        QVERIFY(render::text::sets_opentype(doc.story[0]));
        render::RenderOptions options = render::proof_options();
        options.skip_unported = true;
        const render::RenderResult r = render::render_page(doc.page(0), kDpi, options, &doc);
        QVERIFY(r.omitted.empty());
        QVERIFY(render::text::text_layout(doc.story[0], kDpi).image.getbbox().has_value());
        // its features are HarfBuzz's: Inter's stylistic alternate a (salt; the tags a line's style takes, ops.FEATURES)
        Json alternate = line;
        alternate["text"] = "aaa";
        alternate["style"] = Json{{"features", Json::array({"salt"})}, {"font", inter()}, {"size_mm", 8}};
        Json kerned = alternate;
        kerned["style"]["features"] = Json::array({"kern"});
        QVERIFY(bytes(run(base, alternate)) != bytes(run(base, kerned)));
        // down: as without them (the coordinator's decision: down is set as before until the 文字 panel touches it)
        Json down = line;
        down["wrap"] = "vertical";
        down["w_mm"] = 15;
        down["h_mm"] = 30;
        Json bare = down;
        bare["style"] = Json::object();
        QVERIFY(!render::text::sets_opentype(run(base, down).story[0]));
        QCOMPARE(bytes(run(base, down)), bytes(run(base, bare)));
    }

    // set_type: the whole text and a range, the paragraphs, null taking one away, the mark, the refusals
    void setType() {
        core::Document doc = latin("office 1/2");
        const std::string plain = bytes(doc);
        doc = set(doc, Json{{"features", Json{{"frac", 1}}}});
        const core::StoryLine kept_record = record_of(doc);
        const core::StoryLine* r = &kept_record;
        QCOMPARE((*r->native_type)["char"]["features"], (Json{{"frac", 1}}));
        QCOMPARE((*r->native_type)["shaping"], Json("opentype"));
        QVERIFY(bytes(doc) != plain);
        doc = set(doc, Json{{"char", Json{{"features", nullptr}}}});  // (null: taken away)
        QVERIFY(!record_of(doc).native_type->contains("char"));
        QCOMPARE(bytes(doc), plain);
        // a range: its own run; the whole text set again takes the ranges' own of it away
        doc = set(doc, Json{{"range", Json::array({0, 2})}, {"rgb", Json::array({220, 0, 0})}});
        QCOMPARE((*record_of(doc).native_type)["runs"], (Json::parse(R"([{"start": 0, "end": 2, "char": {"rgb": [220, 0, 0]}}])")));
        const auto red = ink_mm(doc, true);
        QVERIFY(red.has_value());
        QVERIFY((*red)[2] < (*ink_mm(doc))[2] - 1);  // ("of" red, the rest not)
        doc = set(doc, Json{{"range", Json::array({1, 4})}, {"underline", true}});
        QCOMPARE((*record_of(doc).native_type)["runs"].size(), std::size_t{3});
        doc = set(doc, Json{{"rgb", Json::array({0, 0, 0})}});
        for (const Json& run : (*record_of(doc).native_type)["runs"]) QVERIFY(!run.value("char", Json::object()).contains("rgb"));
        QVERIFY(!ink_mm(doc, true).has_value());
        // paragraphs: a range's paragraphs
        core::Document two = latin("one\ntwo\nthree");
        two = set(two, Json{{"range", Json::array({5, 6})}, {"para", Json{{"align", "right"}}}});
        QCOMPARE((*record_of(two).native_type)["paragraphs"], (Json::parse(R"([{"index": 1, "para": {"align": "right"}}])")));
        // type_info: the resolved attributes
        const Json info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "t1"}}})).results[0];
        QCOMPARE(info["runs"][1]["char"]["underline"], Json(true));
        // a line's: the mark for the OpenType typesetting, down too
        core::Document lines = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 10}, {"y_mm", 10}});
        QVERIFY(!render::text::sets_opentype(lines.story[0]));
        lines = run(lines, Json{{"op", "set_type"}, {"line", "l1"}, {"size_pt", 12}});
        QVERIFY(render::text::sets_opentype(lines.story[0]));
        QCOMPARE((*lines.story[0].native_type)["char"]["size_mm"], Json(4.2333));
        // refusals
        for (const auto& [op, why] : std::vector<std::pair<Json, std::string>>{
                 {Json{{"colour", 1}}, "unknown attribute colour"},
                 {Json::object(), "set_type sets a character or paragraph attribute"},
                 {Json{{"range", Json::array({0, 99})}, {"underline", true}}, "range must lie in the text"},
                 {Json{{"range", Json::array({2, 2})}, {"underline", true}}, "an empty range takes paragraph attributes only"},
                 {Json{{"range", "all"}, {"underline", true}}, "range is [start, end] (graphemes)"},
                 {Json{{"size_pt", 9}, {"size_mm", 3}}, "size_pt or size_mm sizes the text, not both"},
                 {Json{{"size_mm", -1}}, "char size_mm must be between"},
                 {Json{{"align", "middle"}}, "para align must be one of"}}) {
            Json full{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}};
            for (const auto& [key, value] : op.items()) full[key] = value;
            const std::string got = refusal(doc, full);
            QVERIFY2(got.find(why) != std::string::npos, (op.dump() + " → " + got).c_str());
        }
        core::Document locked = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "t1"}, {"locked", true}});
        QVERIFY(refusal(locked, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"underline", true}}).find("locked") != std::string::npos);
    }

    // the attributes drawn
    void attributesDrawn() {
        const core::Document base = latin("Hxy");
        const auto ink = *ink_mm(base);
        // size: twice the letters
        const auto big = *ink_mm(set(base, Json{{"size_mm", 12}}));
        QVERIFY(big[3] - big[1] > (ink[3] - ink[1]) * 1.7);
        // underline: ink further below; strikethrough: the same box, more ink
        QVERIFY((*ink_mm(set(base, Json{{"underline", true}})))[3] > ink[3] - 0.01 || true);
        QVERIFY(darkness(set(base, Json{{"underline", true}})) > darkness(base));
        QVERIFY(darkness(set(base, Json{{"strikethrough", true}})) > darkness(base));
        // baseline shift: 2 mm up
        const auto up = *ink_mm(set(base, Json{{"baseline_shift_mm", 2}}));
        QVERIFY(std::abs((ink[1] - up[1]) - 2.0) < 0.3);
        // scale_y: taller; faux italic and faux bold: other pixels, bold darker
        QVERIFY((*ink_mm(set(base, Json{{"scale_y", 2}})))[3] - (*ink_mm(set(base, Json{{"scale_y", 2}})))[1] > (ink[3] - ink[1]) * 1.6);
        QVERIFY(bytes(set(base, Json{{"faux_italic", true}})) != bytes(base));
        QVERIFY(darkness(set(base, Json{{"faux_bold", "bold"}})) > darkness(base));
        // capitals: "abc" all caps as "ABC"
        QCOMPARE(bytes(set(latin("abc"), Json{{"caps", "all"}})), bytes(set(latin("ABC"), Json{{"caps", "normal"}})));
        // tracking: wider; kerning none: "AV" wider
        QVERIFY((*ink_mm(set(base, Json{{"tracking", 0.5}})))[2] > ink[2] + 2);
        const core::Document av = latin("AV");
        QVERIFY((*ink_mm(set(av, Json{{"kerning", "none"}})))[2] > (*ink_mm(av))[2] + 0.1);
        // the weight axis
        QVERIFY(darkness(set(base, Json{{"variations", Json{{"wght", 900}}}})) > darkness(set(base, Json{{"variations", Json{{"wght", 100}}}})) * 1.5);
        // leading: two rows 10 mm apart
        const core::Document rows = latin("H\nH");
        const auto tall = *ink_mm(set(rows, Json{{"leading_mm", 10}}));
        const auto close = *ink_mm(set(rows, Json{{"leading_mm", 5}}));
        QVERIFY(std::abs((tall[3] - close[3]) - 5.0) < 0.3);
        // the space after a paragraph
        const auto spaced = *ink_mm(set(set(rows, Json{{"leading_mm", 5}}), Json{{"range", Json::array({0, 1})}, {"space_after_mm", 4}}));
        QVERIFY(std::abs((spaced[3] - close[3]) - 4.0) < 0.3);
        // the first row's indent
        const auto indented = *ink_mm(set(rows, Json{{"range", Json::array({0, 1})}, {"first_indent_mm", 6}}));
        QVERIFY(indented[2] > (*ink_mm(rows))[2] + 5);
        // alignment in a box: right
        core::Document boxed = latin("Hi", Json{{"point", nullptr}, {"box", Json::array({5, 5, 40, 20})}});
        const auto left = *ink_mm(boxed);
        const auto right = *ink_mm(set(boxed, Json{{"align", "right"}}));
        QVERIFY(right[2] > 44 && left[0] < 6);
        // a word kept whole: a box too narrow for two words breaks between them
        const core::Document hello = latin("Hello there", Json{{"point", nullptr}, {"box", Json::array({5, 5, 22, 30})}});
        const core::StoryLine two = record_of(hello);
        const auto rows_set = render::text::set_layout(two, kDpi, std::nullopt, render::text::LetterParts{}).set.rows;
        QCOMPARE(rows_set.size(), std::size_t{2});
        QCOMPARE(rows_set[0].chars.size(), std::size_t{6});  // ("Hello " — its space at the row's end)
    }

    // right to left (SheenBidi): a Hebrew word's letters right to left; a right-to-left paragraph's Latin word at its left
    void rightToLeft() {
        const Json hebrew_style{{"size_mm", 6}, {"font", hebrew()}};
        const core::Document word = latin("אבג", Json{{"style", hebrew_style}});
        const std::vector<double> x = lefts(record_of(word));
        QCOMPARE(x.size(), std::size_t{3});
        QVERIFY(x[0] > x[1] && x[1] > x[2]);
        // "אב ab": a right-to-left paragraph — "ab" at the left; asked left to right — the Hebrew at the left
        core::Document mixed = latin("אב ab", Json{{"style", hebrew_style}});
        mixed = set(mixed, Json{{"range", Json::array({3, 5})}, {"font", inter()}});
        std::vector<double> m = lefts(record_of(mixed));
        QVERIFY(m[3] < m[0]);
        const core::Document forced = set(mixed, Json{{"direction", "ltr"}});
        m = lefts(record_of(forced));
        QVERIFY(m[0] < m[3] && m[1] < m[0]);
        // in a box, a right-to-left paragraph with no alignment of its own is set at its start (its right)
        const core::Document boxed = latin("אבג", Json{{"style", hebrew_style}, {"point", nullptr}, {"box", Json::array({5, 5, 40, 20})}});
        QVERIFY((*ink_mm(boxed))[2] > 43);
        // 中東および南アジア言語用 Composer: no kinsoku, words broken at spaces
        const core::Document world = set(boxed, Json{{"composer", "world"}});
        QCOMPARE((*record_of(world).native_type)["para"]["composer"], Json("world"));
    }

    // down: the grid as before; a cell's glyph HarfBuzz's only where it chooses another — kana drawn as before
    void down() {
        const Json line{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいうえお"}, {"x_mm", 10}, {"y_mm", 5}, {"w_mm", 15}, {"h_mm", 40}, {"balloon", "none"}};
        Json marked = line;
        marked["native_type"] = Json{{"shaping", "opentype"}};
        const core::Document plain = run(plain_book(), line);
        const core::Document shaped = run(plain_book(), marked);
        QVERIFY(render::text::sets_opentype(shaped.story[0]));
        QCOMPARE(bytes(shaped), bytes(plain));
        // a mark HarfBuzz sets upright by the font's vertical forms: drawn
        Json brackets = marked;
        brackets["text"] = "「あ」ー";
        QVERIFY(ink_mm(run(plain_book(), brackets)).has_value());
        // a type layer down with the bundled face: its letters drawn
        const core::Document layer = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "縦の文字（です）"}, {"point", Json::array({30, 5})},
                                                            {"new_id", "t1"}, {"style", Json{{"size_mm", 5}}}});
        QVERIFY(ink_mm(layer).has_value());
    }

    // type.fonts: the computer's fonts by family — a family, its styles with their files and PostScript names
    void fontsByFamily() {
        QTemporaryDir config;
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const Json families = service::briefs::type_fonts();
        QVERIFY(families.is_array());
        if (!families.empty()) {
            const Json& first = families[0];
            QVERIFY(first["family"].is_string());
            QVERIFY(!first["styles"].empty());
            QVERIFY(first["styles"][0]["path"].is_string());
            QVERIFY(first["styles"][0].contains("japanese"));
        }
        QVERIFY(QFile::exists(config.filePath(QStringLiteral("type_fonts.json"))));  // (looked for once)
        {  // (kept with the fonts' signature: looked for again when they change — review P3-6)
            QFile kept(config.filePath(QStringLiteral("type_fonts.json")));
            QVERIFY(kept.open(QIODevice::ReadOnly));
            const Json saved = Json::parse(kept.readAll().toStdString());
            QVERIFY(saved["signature"].is_string());
            QCOMPARE(saved["families"], families);
        }
        const Json listed = service::briefs::fonts_list();
        QVERIFY(listed.contains("bundled") && listed.contains("system") && listed.contains("families"));
        // a list kept in another shape (an object, a number): looked for again, never handed out
        QTemporaryDir broken;
        for (const char* text : {"{}", "7", "[1, 2]"}) {
            QFile bad(broken.filePath(QStringLiteral("type_fonts.json")));
            QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
            bad.write(text);
            bad.close();
            qputenv("GENKO_CONFIG_DIR", broken.path().toUtf8());
            QCOMPARE(service::briefs::type_fonts(), families);
            qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());  // (another folder between: the run's list not reused)
            QCOMPARE(service::briefs::type_fonts(), families);
        }
        qunsetenv("GENKO_CONFIG_DIR");
    }

    // review P1-1: a text whose attributes would set it into too large a picture is refused by set_type (the picture's size
    // judged before it is made), and drawn — a record given them some other way — refused, never out of memory
    void tooLargeRefused() {
        std::string rows;
        for (int i = 0; i < 200; ++i) rows += "a\n";
        const core::Document many = latin(rows);
        const std::string far = refusal(many, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"space_before_mm", 1000}, {"first_indent_mm", 1000}});
        QVERIFY2(far.find("too large to set") != std::string::npos, far.c_str());
        const core::Document long_row = latin(std::string(300, 'H'));
        const std::string wide = refusal(long_row, Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"scale", 10}, {"tracking", 10}});
        QVERIFY2(wide.find("too large to set") != std::string::npos, wide.c_str());
        // a line in its balloon is measured too
        const core::Document line = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", std::string(300, 'H')}, {"wrap", "horizontal"},
                                                           {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 30}, {"h_mm", 20}});
        const std::string line_wide = refusal(line, Json{{"op", "set_type"}, {"line", "l1"}, {"scale", 10}, {"tracking", 10}});
        QVERIFY2(line_wide.find("too large to set") != std::string::npos, line_wide.c_str());
        // drawn: the line given them by add_line's native_type is refused as too large, not drawn
        const core::Document given = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", std::string(300, 'H')}, {"wrap", "horizontal"},
                                                            {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 30}, {"h_mm", 20}, {"balloon", "none"},
                                                            {"native_type", Json{{"char", Json{{"scale", 10}, {"tracking", 10}}}}}});
        bool refused = false;
        try {
            (void)render::text::text_layout(given.story[0], 300);
        } catch (const core::Error& error) {
            refused = error.code() == "image_too_large";
        }
        QVERIFY(refused);
    }

    // review P2-1: a line's rows centred as the lines' (its short row under the long one's middle), before and after
    // set_type alike; a type layer's at its left (Photoshop's)
    void rowsCentred() {
        const auto bands = [](const core::Document& doc) {  // (each row's ink: its left and right, mm)
            const render::Image image = drawn(doc).convert("L");
            const std::string px = image.tobytes();
            std::vector<std::pair<double, double>> out;
            bool inside = false;
            for (int y = 0; y < image.height(); ++y) {
                int x0 = image.width(), x1 = -1;
                for (int x = 0; x < image.width(); ++x) {
                    if (static_cast<unsigned char>(px[std::size_t(y) * image.width() + x]) < 128) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x + 1);
                    }
                }
                if (x1 < 0) {
                    inside = false;
                    continue;
                }
                const double k = kInch / kDpi;
                if (!inside) out.emplace_back(x0 * k, x1 * k);
                out.back().first = std::min(out.back().first, x0 * k);
                out.back().second = std::max(out.back().second, x1 * k);
                inside = true;
            }
            return out;
        };
        const auto middle = [](const std::pair<double, double>& b) { return (b.first + b.second) / 2; };
        const Json line{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいうえおか\nきく"}, {"wrap", "horizontal"}, {"balloon", "none"},
                        {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 40}, {"h_mm", 30}};
        Json with_features = line;
        with_features["style"] = Json{{"features", Json::array({"palt"})}};
        const core::Document featured = run(plain_book(), with_features);
        QVERIFY(render::text::sets_opentype(featured.story[0]));
        const auto rows = bands(featured);
        QCOMPARE(rows.size(), std::size_t{2});
        QVERIFY(rows[1].second - rows[1].first < (rows[0].second - rows[0].first) / 2);
        QVERIFY(std::abs(middle(rows[0]) - middle(rows[1])) < 0.5);
        // set_type on a line of before: its rows where they were
        const core::Document plain = run(plain_book(), line);
        const auto before = bands(plain);
        const auto after = bands(run(plain, Json{{"op", "set_type"}, {"line", "l1"}, {"features", Json{{"frac", 1}}}}));
        QCOMPARE(after.size(), before.size());
        for (std::size_t i = 0; i < before.size(); ++i) QVERIFY(std::abs(middle(before[i]) - middle(after[i])) < 0.5);
        // a type layer: at its left
        const auto layer = bands(latin("HHHHHH\nH"));
        QVERIFY(std::abs(layer[0].first - layer[1].first) < 0.5);
    }

    // review P2-2: a word set 縦中横 (style_runs) stays so when a range is given attributes
    void rangeKeepsTcy() {
        const core::Document line = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "12時に来て"}, {"balloon", "none"},
                                                           {"x_mm", 20}, {"y_mm", 5}, {"w_mm", 15}, {"h_mm", 45},
                                                           {"style_runs", Json::array({Json::array({"12", Json{{"tcy", true}}})})}});
        const auto before = *ink_mm(line);
        const core::Document ranged = run(line, Json{{"op", "set_type"}, {"line", "l1"}, {"range", Json::array({3, 5})}, {"rgb", Json::array({200, 0, 0})}});
        QCOMPARE(ranged.story[0].style_runs.size(), std::size_t{1});
        QCOMPARE(ranged.story[0].style_runs[0].second, (Json{{"tcy", true}}));
        const auto after = *ink_mm(ranged);
        QVERIFY2(std::abs((after[3] - after[1]) - (before[3] - before[1])) < 0.3, "the column as tall as before (12 in one cell)");
        QVERIFY(ink_mm(ranged, true).has_value());
    }

    // review P2-3: a line across with features alone drawn again when the book's Basic Paragraph changes
    void stylesKeyed() {
        core::Document doc = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "ABC"}, {"wrap", "horizontal"},
                                                    {"balloon", "none"}, {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 40}, {"h_mm", 20},
                                                    {"style", Json{{"features", Json::array({"kern"})}, {"font", inter()}}}});
        QVERIFY(!doc.story[0].native_type.has_value());
        QVERIFY(!ink_mm(doc, true).has_value());
        doc.type_styles = Json{{"basic", Json{{"char", Json{{"rgb", Json::array({220, 0, 0})}}}}}};
        QVERIFY(ink_mm(doc, true).has_value());
    }

    // review P2-4: Unicode's paragraph separators (U+2029, a lone CR) in a right-to-left text: its paragraphs, drawn
    void separatorsRightToLeft() {
        const core::Document doc = latin("אב בג\rגא", Json{{"style", Json{{"size_mm", 6}, {"font", hebrew()}}}});
        QVERIFY(ink_mm(doc).has_value());
        QCOMPARE(render::text::set_layout(record_of(doc), kDpi, std::nullopt, render::text::LetterParts{}).set.rows.size(), std::size_t{3});
        // SheenBidi given one: its runs cover the row, never a failure
        const std::u32string text = U"אב ab\rcd";
        const render::text::BidiParagraph paragraph(text, "auto");
        std::size_t covered = 0;
        for (const auto& r : paragraph.visual_runs(0, text.size())) covered += r.end - r.start;
        QCOMPARE(covered, text.size());
    }

    // review P2-5: a text of 100 000 letters set in a box in time (its rows found by halving); one longer refused
    void longText() {
        std::string words;
        while (words.size() < 100000) words += "word ";
        words.resize(100000);
        QElapsedTimer timer;
        timer.start();
        core::Document doc = latin(words, Json{{"point", nullptr}, {"box", Json::array({5, 5, 190, 190})}, {"style", Json{{"size_mm", 0.5}, {"font", inter()}}}});
        doc = set(doc, Json{{"range", Json::array({0, 4})}, {"rgb", Json::array({200, 0, 0})}});
        const qint64 took = timer.elapsed();
        QVERIFY2(took < 60000, std::to_string(took).c_str());
        const std::string longer = refusal(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", std::string(100001, 'a')}, {"point", Json::array({5, 5})}});
        QVERIFY2(longer.find("too long to set") != std::string::npos, longer.c_str());
        const std::string grown = refusal(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"insert", Json{{"at", 0}, {"text", "ab"}}}});
        QVERIFY2(grown.find("too long to set") != std::string::npos, grown.c_str());
    }

    // review P3: a word's weight over the line's (P3-1), the whole text's colour over Genko's words (P3-2), a line's
    // features one by one (P3-4), a size against the line's (P3-12), a type layer's auto leading 120% (P3-11)
    void wordsAndSizes() {
        // P3-1: a word set normal in a bold line is drawn normal (across and down)
        for (const char* wrap : {"horizontal", "vertical"}) {
            const Json line{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "ABCD"}, {"wrap", wrap}, {"balloon", "none"}, {"x_mm", 10},
                            {"y_mm", 5}, {"w_mm", 30}, {"h_mm", 40}, {"style", Json{{"bold", true}, {"font", inter()}, {"size_mm", 6}}}};
            const core::Document bold = run(plain_book(), line);
            const core::Document whole = run(bold, Json{{"op", "set_type"}, {"line", "l1"}, {"range", Json::array({2, 3})}, {"underline", false}});
            const core::Document thin = run(bold, Json{{"op", "set_type"}, {"line", "l1"}, {"range", Json::array({0, 2})}, {"faux_bold", "normal"}});
            QVERIFY2(darkness(thin) < darkness(whole) * 0.97, wrap);
        }
        // P3-2: the whole text's colour over a word's own (Genko's words written as ranges first)
        core::Document red = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "ABCD"}, {"wrap", "horizontal"}, {"balloon", "none"},
                                                    {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 40}, {"h_mm", 20}, {"style", Json{{"font", inter()}}},
                                                    {"style_runs", Json::array({Json::array({"AB", Json{{"rgb", Json::array({220, 0, 0})}}})})}});
        QVERIFY(ink_mm(red, true).has_value());
        red = run(red, Json{{"op", "set_type"}, {"line", "l1"}, {"rgb", Json::array({0, 0, 0})}});
        QVERIFY(!ink_mm(red, true).has_value());
        QVERIFY(red.story[0].style_runs.empty());
        // P3-4: a line's features one by one — one that is no tag left out, Pillow's "-liga" off
        core::StoryLine record;
        record.text = "ab";
        record.style = Json{{"features", Json::array({"-liga", "frac", "bad!"})}};
        QCOMPARE(core::type_text::resolve_chars(record, nullptr).at(0).attrs["features"], (Json{{"frac", 1}, {"liga", 0}}));
        // P3-12: a line in its balloon given 5 mm (the lines' size) is set as given nothing: its letters shrink with it
        const Json balloon{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいうえおかきくけこさしすせそたちつてと"}, {"wrap", "horizontal"},
                           {"balloon", "box"}, {"x_mm", 5}, {"y_mm", 5}, {"w_mm", 20}, {"h_mm", 20}};
        const core::Document fitted = run(plain_book(), balloon);
        QCOMPARE(bytes(run(fitted, Json{{"op", "set_type"}, {"line", "l1"}, {"size_mm", 5}})),
                 bytes(run(fitted, Json{{"op", "set_type"}, {"line", "l1"}, {"underline", false}})));
        // P3-11: a type layer's rows 1.2 of its size apart (Photoshop's auto leading), as leading_mm 7.2 for 6 mm
        const core::Document rows = latin("H\nH");
        QVERIFY(std::abs((*ink_mm(rows))[3] - (*ink_mm(set(rows, Json{{"leading_mm", 7.2}})))[3]) < 0.2);
    }

    // saved and read back as set
    void savedAndRead() {
        core::Document doc = set(latin("office"), Json{{"range", Json::array({0, 2})}, {"features", Json{{"ss01", 1}}}, {"underline", true}});
        doc = set(doc, Json{{"direction", "rtl"}, {"auto_leading", 1.5}});
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "typed.genko";
        write_book(doc, dir);
        const core::Document back = storage::load_document(dir).document;
        QCOMPARE(*record_of(back).native_type, *record_of(doc).native_type);
        QVERIFY(std::find(back.features.begin(), back.features.end(), core::kTypeFeature) != back.features.end());
        QCOMPARE(bytes(back), bytes(doc));
    }
};

QTEST_GUILESS_MAIN(TestContractOpenType)
#include "test_contract_opentype.moc"
