// fukidashi@1 (SPEC BALLOON-01, AC-BALLOON), the ops and the auto-lettering:
//
//   - the ops take the kinds capsule and polygon, the style keys corner_mm, squircle, sides, wave_mm, spike_alt and
//     dash_mm, a tail of kind inward and a tail's fillet_mm; what they refuse is said in Japanese (no key's name, no
//     English word of four letters or more); the keys of before (hand, wobble) are taken without a word;
//   - the auto-lettering sizes an oval as the smallest true ellipse round its words (the letters set in it at their
//     size), says a tall line of one or two columns in a capsule sized for it, and leaves every other line its kind; a
//     balloon moved over its notch (an inward tail) keeps it.

#include <QtTest>

#include <QRegularExpression>

#include <cmath>
#include <string>
#include <vector>

#include "core/balloon_tails.hpp"
#include "core/command_bus.hpp"
#include "core/fukidashi.hpp"
#include "core/model.hpp"
#include "formats/checks.hpp"
#include "render/ops_registry.hpp"
#include "render/text/balloon_clean.hpp"
#include "render/text/lettering.hpp"
#include "service/book.hpp"
#include "service/layout.hpp"
#include "service/letter.hpp"
#include "service/wording.hpp"
#include "studio/schemas.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
namespace letter = genko::service::letter;

namespace {

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(genko::render::ops_registry());
    return b;
}

Document blank_book() { return genko::core::new_episode("フキダシ", genko::core::Num(1), 1, genko::core::PageSpec::b5_doujin()); }

Document run_ops(const Document& doc, const Json& ops, const char* actor = "human:作者") { return bus().apply(doc, ops, genko::core::Actor(actor)).doc; }

Json line_op(const char* kind, Json more = Json::object()) {
    Json op{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"balloon", kind}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 30}, {"text", "今日は\nいい天気"}};
    for (const auto& [key, value] : more.items()) op[key] = value;
    return op;
}

// What an op batch is refused with, as the person reads it (empty: it was not refused).
QString refusal(const Json& ops) {
    try {
        (void)run_ops(blank_book(), ops);
    } catch (const genko::core::ApplyError& error) {
        return genko::wording::error(QString::fromUtf8(error.what()));
    }
    return {};
}

// The sentence of a refusal: without the op's usage the bus adds after it (（add_line の書き方: {…}）).
QString sentence(const QString& said) {
    const qsizetype usage = said.indexOf(QStringLiteral("の書き方:"));
    if (usage < 0) return said;
    const qsizetype bracket = said.lastIndexOf(QStringLiteral("（"), usage);
    return bracket < 0 ? said : said.left(bracket);
}

}  // namespace

class TestFukidashiService : public QObject {
    Q_OBJECT

private slots:
    // The ops take what is new, and keep it as given.
    void theOpsTakeTheNewKindsAndKeys() {
        const Document capsule = run_ops(blank_book(), Json::array({line_op("capsule")}));
        QCOMPARE(capsule.story.front().balloon, std::string("capsule"));
        const Document polygon = run_ops(blank_book(), Json::array({line_op("polygon", Json{{"style", Json{{"sides", 6}, {"corner_mm", 1.5}}}})}));
        QCOMPARE(polygon.story.front().balloon, std::string("polygon"));
        QCOMPARE(polygon.story.front().style["sides"], Json(6));
        QCOMPARE(polygon.story.front().style["corner_mm"], Json(1.5));
        const Document styled = run_ops(blank_book(), Json::array({line_op("rounded", Json{{"style", Json{{"squircle", 1}, {"wave_mm", 0.4}, {"dash_mm", 2}, {"spike_alt", 0.5}}}})}));
        QCOMPARE(styled.story.front().style["squircle"], Json(true));
        QCOMPARE(styled.story.front().style["wave_mm"], Json(0.4));
        QCOMPARE(styled.story.front().style["dash_mm"], Json(2.0));
        QCOMPARE(styled.story.front().style["spike_alt"], Json(0.5));
        // (a key set to null goes back to its default: gone from the style)
        const Document reset = run_ops(styled, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"wave_mm", nullptr}, {"squircle", nullptr}}}}}));
        QVERIFY(!reset.story.front().style.contains("wave_mm"));
        QVERIFY(!reset.story.front().style.contains("squircle"));
        // (tails: the kind inward, and how far the join is rounded — 0 kept, a corner)
        const Document tails = run_ops(blank_book(), Json::array({line_op("speech", Json{{"tails", Json::array({Json{{"to", Json::array({60, 55})}, {"kind", "inward"}},
                                                                                                                 Json{{"to", Json::array({90, 80})}, {"fillet_mm", 0}},
                                                                                                                 Json{{"to", Json::array({30, 80})}, {"fillet_mm", 2.5}}})}})}));
        QCOMPARE(tails.story.front().tails[0]["kind"], Json("inward"));
        QCOMPARE(tails.story.front().tails[1]["fillet_mm"], Json(0.0));
        QCOMPARE(tails.story.front().tails[2]["fillet_mm"], Json(2.5));
        // (the keys of before: taken, kept, no warning — a call or a book of before goes on working)
        const genko::core::ApplyResult old = bus().apply(blank_book(), Json::array({line_op("speech", Json{{"style", Json{{"hand", false}, {"wobble", 0.4}, {"spike_jitter", 0.2}}}})}),
                                                         genko::core::Actor("ai:letterer"));
        QCOMPARE(old.doc.story.front().style["hand"], Json(false));
        QCOMPARE(old.doc.story.front().style["wobble"], Json(0.4));
        QVERIFY2(old.warnings.empty(), Json(old.warnings).dump().c_str());
        // (an agent's ops are the same ops: the new kinds through the same door, a page another holds refused as ever)
        Document held = run_ops(blank_book(), Json::array({line_op("speech")}));
        held = run_ops(held, Json::array({Json{{"op", "lock_page"}, {"page", 1}}}));
        try {
            (void)run_ops(held, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"balloon", "capsule"}}}), "ai:letterer");
            QFAIL("a page a person holds was changed by an agent");
        } catch (const genko::core::ApplyError& error) {
            QVERIFY(QString::fromUtf8(error.what()).contains(QStringLiteral("locked by")));
        }
    }

    // What they refuse, said in Japanese.
    void refusalsReadInJapanese_data() {
        QTest::addColumn<QString>("ops");
        QTest::addColumn<QString>("says");
        const auto style = [](const char* kind, const Json& st) { return QString::fromStdString(Json::array({line_op(kind, Json{{"style", st}})}).dump()); };
        const auto tail = [](const Json& t) { return QString::fromStdString(Json::array({line_op("speech", Json{{"tails", Json::array({t})}})}).dump()); };
        QTest::newRow("corner_mm") << style("rounded", Json{{"corner_mm", 80}}) << QStringLiteral("フキダシの角の丸みは 0〜50 mm です");
        QTest::newRow("corner_mm below") << style("rounded", Json{{"corner_mm", -1}}) << QStringLiteral("フキダシの角の丸みは 0〜50 mm です");
        QTest::newRow("sides") << style("polygon", Json{{"sides", 4}}) << QStringLiteral("多角形のフキダシの角の数は 5〜12 です");
        QTest::newRow("sides above") << style("polygon", Json{{"sides", 13}}) << QStringLiteral("多角形のフキダシの角の数は 5〜12 です");
        QTest::newRow("wave_mm") << style("speech", Json{{"wave_mm", 5}}) << QStringLiteral("フキダシの線の震えは 0〜3 mm です");
        QTest::newRow("spike_alt") << style("shout", Json{{"spike_alt", 0.9}}) << QStringLiteral("トゲの長短の差は 0〜0.8 です");
        QTest::newRow("dash_mm") << style("whisper", Json{{"dash_mm", 0.05}}) << QStringLiteral("破線の 1 本の長さは 0.2〜10 mm です");
        QTest::newRow("fillet_mm") << tail(Json{{"to", Json::array({90, 80})}, {"fillet_mm", 30}}) << QStringLiteral("しっぽの付け根の丸みは 0〜20 mm です");
        QTest::newRow("tail kind") << tail(Json{{"to", Json::array({90, 80})}, {"kind", "hook"}})
                                   << QStringLiteral("しっぽの形は、くさび・まっすぐ・ギザギザ・消える・泡・内向きから選びます");
    }
    void refusalsReadInJapanese() {
        QFETCH(QString, ops);
        QFETCH(QString, says);
        const QString said = refusal(Json::parse(ops.toStdString()));
        QVERIFY2(said.contains(says), qPrintable(said));
        // (no key's or op's name left in the sentence the person reads: no English word of four letters or more — the
        // op's usage the bus adds after it, in brackets, is the caller's)
        QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(sentence(said)).hasMatch(), qPrintable(said));
    }
    // (a kind that is none of them: said too, with nothing English left)
    void anUnknownKindIsRefusedInJapanese() {
        const QString said = refusal(Json::array({line_op("bubble")}));
        QVERIFY(!said.isEmpty());
        QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(sentence(said)).hasMatch(), qPrintable(said));
    }

    // A balloon moved or resized without saying its tails: a tail whose tip it now covers is pushed out past its outline;
    // a notch (an inward tail) is a place in the balloon and goes with it — moved by as much, or the same share of the
    // way across a resized box, with the point it bends through —; the other tails stay where they point on the page.
    // The same op on the same book gives the same book (a replay), and the book's checks do not call a notch a hidden
    // tail. A move_line that says its tails leaves them as said.
    void aNotchGoesWithItsBalloon() {
        const Json tails = Json::array({Json{{"to", Json::array({58, 50})}, {"kind", "inward"}}, Json{{"to", Json::array({84, 74})}}});
        const Document placed = run_ops(blank_book(), Json::array({line_op("speech", Json{{"tails", tails}})}));  // (40, 40, 40 × 30)
        // made bigger from its corner (60 × 50): the notch the same share across (18/40 of 60, 10/30 of 50)
        const Json grow = Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"w_mm", 60}, {"h_mm", 50}}});
        Document doc = run_ops(placed, grow);
        {
            const genko::core::StoryLine& line = doc.story.front();
            QCOMPARE(line.tails[0], (Json{{"to", Json::array({67.0, 56.67})}, {"kind", "inward"}}));
            QVERIFY(genko::core::tail_hidden(line, line.tails[0]["to"]));
            QVERIFY(line.tails[1]["to"] != Json::array({84.0, 74.0}));  // (the tail the bigger balloon covered: moved out)
            QVERIFY(!genko::core::tail_hidden(line, line.tails[1]["to"]));
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) QVERIFY2(issue.value("code", std::string()) != "tail_hidden", issue.dump().c_str());
        }
        // (the same op on the same book again — a journal's replay: the same lines)
        const Document again = run_ops(placed, grow);
        QCOMPARE(again.story.front().tails, doc.story.front().tails);
        QCOMPARE(again.story.front().w_mm.value(), 60.0);
        // moved by (4, 3) mm: the notch by as much, the other tail where it points
        doc = run_ops(placed, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 44}, {"y_mm", 43}}}));
        QCOMPARE(doc.story.front().tails[0], (Json{{"to", Json::array({62.0, 53.0})}, {"kind", "inward"}}));
        QCOMPARE(doc.story.front().tails[1], (Json{{"to", Json::array({84.0, 74.0})}}));
        QCOMPARE(doc.story.front().tail->x.value(), 62.0);  // (the line's first tail, as the single tail of before reads it)
        // (moved and back: where it was)
        doc = run_ops(doc, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 40}, {"y_mm", 40}}}));
        QCOMPARE(doc.story.front().tails, placed.story.front().tails);
        // a notch that bends through a point: the point with it; moved and made wider at once
        const Json bent = Json::array({Json{{"to", Json::array({58, 50})}, {"via", Json::array({70, 52})}, {"kind", "inward"}}});
        doc = run_ops(placed, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", bent}},
                                           Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 50}, {"w_mm", 80}}}));
        QCOMPARE(doc.story.front().tails[0], (Json{{"to", Json::array({86.0, 50.0})}, {"via", Json::array({110.0, 52.0})}, {"kind", "inward"}}));
        // tails said with the move: as said, the notch too
        doc = run_ops(placed, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 60}, {"tails", tails}}}));
        QCOMPARE(doc.story.front().tails[0], (Json{{"to", Json::array({58.0, 50.0})}, {"kind", "inward"}}));
        // a move_line that changes nothing of the box: nothing of the tails
        doc = run_ops(placed, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 40}, {"w_mm", 40}}}));
        QCOMPARE(doc.story.front().tails, placed.story.front().tails);
    }

    // The auto-lettering's sizes: an oval is the smallest true ellipse round its words — the letters set in a balloon of
    // that size are not made smaller, and a balloon a tenth smaller would be too small for them.
    void anOvalIsSizedForATrueEllipse() {
        const std::vector<std::string> breaks{"あいうえおか", "きくけこさ", "しすせ"};
        for (const char* kind : {"speech", "thought", "whisper"}) {
            const auto size = letter::measure(breaks, kind);
            // (the block's corners, the fit's pad round them, lie on the ellipse: (x/a)² + (y/b)² = 1 at the worst corner)
            const double text_w = 5.0 * 3 + 5.0 * 0.4 * 2, text_h = 5.0 * 6;
            QVERIFY2(size[0] > text_w && size[1] > text_h, kind);
            Json op = line_op(kind, Json{{"w_mm", size[0]}, {"h_mm", size[1]}, {"text", "あいうえおか\nきくけこさ\nしすせ"}});
            const Document doc = run_ops(blank_book(), Json::array({op}));
            QCOMPARE(genko::render::text::text_layout(doc.story.front(), 600).em, std::int64_t{118});  // (5 mm at 600 dpi)
            op["w_mm"] = size[0] * 0.88;
            op["h_mm"] = size[1] * 0.88;
            const Document tight = run_ops(blank_book(), Json::array({op}));
            QVERIFY(genko::render::text::text_layout(tight.story.front(), 600).em < 118);
        }
        // (an oval of now is a little bigger than the hand-drawn one was for the same words — by what a book of before
        // grows when it is read: the two agree)
        const auto now = letter::measure({"あいうえお"}, "speech");
        QVERIFY(now[1] > 5.0 * 5 * 1.2);
    }

    // A line that says no kind, one column of five characters or more, is said in a capsule; a kind that is said is kept —
    // a line that says speech stays an oval — and two columns stay an oval.
    void aTallLineIsSaidInACapsule() {
        const Json none;
        QCOMPARE(letter::shape_for({"そうなんだよね"}, none), std::string("capsule"));  // (one column of seven)
        QCOMPARE(letter::shape_for({"そうなんだ"}, none), std::string("capsule"));      // (five: 5 × 25 mm, five times as tall as wide)
        QCOMPARE(letter::shape_for({"そうだね"}, none), std::string("speech"));         // (four)
        QCOMPARE(letter::shape_for({"うん"}, none), std::string("speech"));
        QCOMPARE(letter::shape_for({"そうなんだよねえ", "ほんとうにそうだ"}, none), std::string("speech"));  // (two columns: an oval)
        QCOMPARE(letter::shape_for({"ああああああああ", "いいいいいいいい", "うううううううう"}, none), std::string("speech"));
        for (const char* kind : {"speech", "thought", "whisper", "shout", "rounded", "box", "narration", "sfx", "none", "capsule"}) {
            QCOMPARE(letter::shape_for({"そうなんだよね"}, Json(kind)), std::string(kind));  // (said: kept)
        }
        // (its size: round ends on the narrow side, the words reaching a little into them — narrower than the oval is, and
        // the letters set in it at their size)
        const auto capsule = letter::measure({"そうなんだよね"}, "capsule");
        const auto oval = letter::measure({"そうなんだよね"}, "speech");
        QVERIFY(capsule[0] < oval[0] && capsule[1] < oval[1]);
        QVERIFY(capsule[0] >= 5.0 + 2 * 0.75 && capsule[1] >= 35.0);
        const Document doc = run_ops(blank_book(), Json::array({line_op("capsule", Json{{"w_mm", capsule[0]}, {"h_mm", capsule[1]}, {"text", "そうなんだよね"}})}));
        const genko::render::text::Layout layout = genko::render::text::text_layout(doc.story.front(), 600);
        QCOMPARE(layout.em, std::int64_t{118});
        QVERIFY(layout.image.height() > 6 * 118);  // (one column: not broken in two)
        const auto two = letter::measure({"そうなんだよねえ", "ほんとうにそうだ"}, "capsule");
        const Document doc2 = run_ops(blank_book(), Json::array({line_op("capsule", Json{{"w_mm", two[0]}, {"h_mm", two[1]}, {"text", "そうなんだよねえ\nほんとうにそうだ"}})}));
        QCOMPARE(genko::render::text::text_layout(doc2.story.front(), 600).em, std::int64_t{118});
    }

    // A page lettered from its plan: a line that says speech is an oval however tall; a line that says no kind, one tall
    // column, is placed as a capsule; the ops made of the placements add them so.
    void aPlannedPageIsLetteredAsItSays() {
        const Json plan = Json::parse(R"({"page": 1, "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": null}]}],
            "panels": [{"slot": "p1", "shot": "MS", "characters": [{"id": "a", "pos": "right"}, {"id": "b", "pos": "left"}],
                        "lines": [{"beat_id": "b1", "balloon": "speech", "breaks": ["そうなんだよね"]},
                                  {"beat_id": "b2", "breaks": ["ほんとうにそう"]},
                                  {"beat_id": "b3", "balloon": "thought", "breaks": ["どうなんだろうね"]},
                                  {"beat_id": "b4", "breaks": ["そうかな", "どうかな"]}]}]})");
        genko::service::Work work{blank_book(), Json::array()};
        const genko::service::layout::CompiledLayout compiled = genko::service::layout::apply_layout(work, Json(1), plan, "ai:test");
        const auto [placements, issues] = letter::place_page(plan, compiled, Json::parse(R"({"characters": [{"id": "a", "name": "A"}, {"id": "b", "name": "B"}]})"),
                                                             Json{{"b1", "a"}, {"b2", "b"}, {"b3", "a"}, {"b4", "b"}});
        QCOMPARE(placements.size(), std::size_t{4});
        QCOMPARE(placements[0].balloon, std::string("speech"));   // (said: an oval, one column of seven though it is)
        QCOMPARE(placements[1].balloon, std::string("capsule"));  // (not said: one tall column)
        QVERIFY(placements[1].h_mm > placements[1].w_mm * 2.5);
        QCOMPARE(placements[2].balloon, std::string("thought"));
        QCOMPARE(placements[3].balloon, std::string("speech"));   // (not said, two columns: an oval)
        work.apply(letter::placements_to_ops(Json(1), placements, compiled), "genko");
        QCOMPARE(work.doc.story.size(), std::size_t{4});
        QCOMPARE(work.doc.story[1].balloon, std::string("capsule"));
        for (const genko::core::StoryLine& line : work.doc.story) {
            QVERIFY(!line.style.contains("hand"));  // (nothing of the look of before is written)
            QCOMPARE(genko::render::text::text_layout(line, 600).em, std::int64_t{118});
        }
        QVERIFY(genko::core::uses_fukidashi(work.doc));
    }

    // A name plan may say capsule and polygon (name_plan@1's kinds): the plan is taken as it is, the page lettered so —
    // each in the box measured for its shape, its letters at their size —, and the hint of a line that does not fit
    // counts the letters a capsule and a polygon take in the panel as their boxes are measured.
    void aPlanSaysCapsuleAndPolygon() {
        const Json plan = Json::parse(R"({"page": 1, "turn_role": "setup", "template": null,
            "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": null}]}],
            "panels": [{"slot": "p1", "shot": "MS", "angle": "eye", "characters": [{"id": "a", "pos": "right"}, {"id": "b", "pos": "left"}],
                        "lines": [{"beat_id": "b1", "balloon": "capsule", "breaks": ["そうなんだ"]},
                                  {"beat_id": "b2", "balloon": "polygon", "breaks": ["ジュシン", "カンリョウ"]},
                                  {"beat_id": "b3", "balloon": "capsule", "breaks": ["うん"]}]}]})");
        // (the plan's schema takes the two kinds, and still refuses a kind it does not know)
        const Json& schema = genko::studio::schema("name_plan@1");
        for (const auto& issue : genko::studio::validate(genko::studio::fill_nulls(plan, schema), schema)) {
            QVERIFY2(issue.path.find("balloon") == std::string::npos, (issue.path + ": " + issue.message).c_str());
        }
        Json unknown = plan;
        unknown["panels"][0]["lines"][0]["balloon"] = "pill";
        bool refused = false;
        for (const auto& issue : genko::studio::validate(genko::studio::fill_nulls(unknown, schema), schema)) refused = refused || issue.path.find("balloon") != std::string::npos;
        QVERIFY(refused);
        genko::service::Work work{blank_book(), Json::array()};
        const genko::service::layout::CompiledLayout compiled = genko::service::layout::apply_layout(work, Json(1), plan, "ai:test");
        const auto [placements, issues] = letter::place_page(plan, compiled, Json::parse(R"({"characters": [{"id": "a", "name": "A"}, {"id": "b", "name": "B"}]})"),
                                                             Json{{"b1", "a"}, {"b2", "b"}, {"b3", "a"}});
        for (const auto& issue : issues) QVERIFY2(issue.severity != "error", issue.message.c_str());
        QCOMPARE(placements.size(), std::size_t{3});
        QCOMPARE(placements[0].balloon, std::string("capsule"));
        QCOMPARE(placements[0].w_mm, 9.5);     // (one column of five: 5 + 4.5 mm wide, 25 + 4.07 mm tall)
        QCOMPARE(placements[0].h_mm, 29.07);
        QCOMPARE(placements[1].balloon, std::string("polygon"));
        QCOMPARE(placements[1].w_mm, 19.09);   // (13.5 × 26.5 mm of words, √2 times)
        QCOMPARE(placements[1].h_mm, 37.48);
        QCOMPARE(placements[2].balloon, std::string("capsule"));
        work.apply(letter::placements_to_ops(Json(1), placements, compiled), "genko");
        QCOMPARE(work.doc.story.size(), std::size_t{3});
        QCOMPARE(work.doc.story[0].balloon, std::string("capsule"));
        QCOMPARE(work.doc.story[1].balloon, std::string("polygon"));
        for (const genko::core::StoryLine& line : work.doc.story) QCOMPARE(genko::render::text::text_layout(line, 600).em, std::int64_t{118});
        QVERIFY(genko::core::uses_fukidashi(work.doc));
        // the hint's count: a panel 30 × 62 mm (26 × 58 of room) takes a capsule of ten letters in a column
        // ((58 − 4.07) / 5) and three columns ((26 − 4.5 + 2) / 7), a polygon of seven ((58 / √2 − 1.5) / 5) and two
        // columns ((26 / √2 − 1.5 + 2) / 7) — a box, eleven and three
        const genko::service::Box panel{0.0, 0.0, 30.0, 62.0};
        QVERIFY2(letter::fits(panel, "capsule").find("1 列 10 字・3 列まで") != std::string::npos, letter::fits(panel, "capsule").c_str());
        QVERIFY2(letter::fits(panel, "polygon").find("1 列 7 字・2 列まで") != std::string::npos, letter::fits(panel, "polygon").c_str());
        QVERIFY2(letter::fits(panel, "box").find("1 列 11 字・3 列まで") != std::string::npos, letter::fits(panel, "box").c_str());
        // (what the hint counts is what the measure gives: ten in a capsule's column fit the room's 58 mm, eleven do not;
        // seven in a polygon's, not eight)
        QVERIFY(letter::measure({"あいうえおかきくけこ"}, "capsule")[1] <= 58.0);
        QVERIFY(letter::measure({"あいうえおかきくけこさ"}, "capsule")[1] > 58.0);
        QVERIFY(letter::measure({"あいうえおかき"}, "polygon")[1] <= 58.0);
        QVERIFY(letter::measure({"あいうえおかきく"}, "polygon")[1] > 58.0);
    }

    // A tail's tip is hidden by a capsule and a polygon where they are — not where the ellipse of their box would be: the
    // corner of a capsule's box and the cut corner of a polygon's are outside them; a balloon grown over a tail pushes
    // its tip out past its own outline.
    void aTipIsHiddenByTheShapeItself() {
        using genko::core::tail_hidden;
        Document doc = run_ops(blank_book(), Json::array({line_op("capsule", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 14}, {"h_mm", 40}})}));
        const genko::core::StoryLine& capsule = doc.story.front();
        QVERIFY(tail_hidden(capsule, Json::array({47.0, 60.0})));    // (its middle)
        QVERIFY(tail_hidden(capsule, Json::array({53.0, 72.0})));    // (by its straight side: inside it, outside the ellipse of its box)
        QVERIFY(!tail_hidden(capsule, Json::array({41.0, 41.0})));   // (its box's corner: outside its round end)
        QVERIFY(tail_hidden(capsule, Json::array({47.0, 41.0})));    // (the round end's top)
        QVERIFY(!tail_hidden(capsule, Json::array({55.0, 60.0})));
        Document sided = run_ops(blank_book(), Json::array({line_op("polygon", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 40}})}));
        const genko::core::StoryLine& octagon = sided.story.front();
        QVERIFY(tail_hidden(octagon, Json::array({60.0, 60.0})));
        QVERIFY(tail_hidden(octagon, Json::array({79.5, 66.0})));    // (by its flat side: inside it, outside the circle of its box)
        QVERIFY(!tail_hidden(octagon, Json::array({43.0, 43.0})));   // (its box's corner: cut off)
        QVERIFY(!tail_hidden(octagon, Json::array({81.0, 60.0})));
        // (a capsule moved over its tail: the tip goes out past the capsule's own edge, the way it lay from the middle)
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", Json::array({Json{{"to", Json::array({62.0, 60.0})}}})}}}));
        doc = run_ops(doc, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"w_mm", 40}}}));  // (now 40 × 40: a circle, 20 mm to its edge)
        const Json tip = doc.story.front().tails.front()["to"];
        QVERIFY2(std::fabs(tip[0].get<double>() - (60.0 + 20.0 + 3.0)) < 0.02 && std::fabs(tip[1].get<double>() - 60.0) < 0.02, tip.dump().c_str());
    }

    // --- fukidashi@2 (SPEC BALLOON-02): the additions ------------------------------------------------------------------------

    // The ops take the symbol kinds and the additions' keys, and keep them in their normal form; a key set to null goes.
    void theOpsTakeTheAdditions() {
        for (const char* kind : {"heart", "star", "flower", "diamond"}) {
            const Document doc = run_ops(blank_book(), Json::array({line_op(kind)}));
            QCOMPARE(doc.story.front().balloon, std::string(kind));
        }
        const Json style{{"pen_swell", 0.45}, {"pen_angle", 30}, {"pen_taper", 1}, {"pen_strokes", 3}, {"drip_mm", 6}, {"drips", 7},
                         {"drip_width_mm", 2}, {"drip_seed", 12}, {"points", 4}, {"fill_tone", Json{{"density", 0.1}}},
                         {"fill_gradient", Json{{"to", Json::array({200.0, 220, 255})}}}};
        Document doc = run_ops(blank_book(), Json::array({line_op("star", Json{{"style", style}})}));
        const Json& kept = doc.story.front().style;
        QCOMPARE(kept["pen_swell"], Json(0.45));
        QCOMPARE(kept["pen_angle"], Json(30.0));
        QCOMPARE(kept["pen_taper"], Json(1.0));
        QCOMPARE(kept["pen_strokes"], Json(3));
        QCOMPARE(kept["drip_mm"], Json(6.0));
        QCOMPARE(kept["drips"], Json(7));
        QCOMPARE(kept["drip_width_mm"], Json(2.0));
        QCOMPARE(kept["drip_seed"], Json(12));
        QCOMPARE(kept["points"], Json(4));
        // (a tone: its density as given, the screen it has when none is said; a blend: its end, down the page)
        QCOMPARE(kept["fill_tone"], (Json{{"density", 0.1}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}}));
        QCOMPARE(kept["fill_gradient"], (Json{{"to", Json::array({200, 220, 255})}, {"dir", 90.0}}));
        // (a tone that grows darker, of lines, from the middle outward, white left inside the line)
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"},
                                             {"style", Json{{"fill_tone", Json{{"density", 0.05}, {"to", 0.5}, {"lpi", 42.5}, {"angle", 0}, {"pattern", "line"}, {"radial", 1}, {"inset_mm", 0.8}}},
                                                            {"fill_gradient", Json{{"to", Json::array({0, 0, 0})}, {"dir", 0}, {"radial", true}}}}}}}));
        QCOMPARE(doc.story.front().style["fill_tone"],
                 (Json{{"density", 0.05}, {"lpi", 42.5}, {"angle", 0.0}, {"pattern", "line"}, {"to", 0.5}, {"dir", 90.0}, {"radial", true}, {"inset_mm", 0.8}}));
        QCOMPARE(doc.story.front().style["fill_gradient"], (Json{{"to", Json::array({0, 0, 0})}, {"dir", 0.0}, {"radial", true}}));
        // (what does not apply is kept and ignored: a drip on a line that has no body to drip from, points on an oval)
        const Document odd = run_ops(blank_book(), Json::array({line_op("sfx", Json{{"style", Json{{"drip_mm", 5}, {"points", 7}, {"pen_swell", 0.5}}}})}));
        QCOMPARE(odd.story.front().style["drip_mm"], Json(5.0));
        // (null: back to none)
        Json none = Json::object();
        for (const auto& [key, value] : style.items()) none[key] = nullptr;
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", none}}}));
        QVERIFY2(doc.story.front().style.empty(), doc.story.front().style.dump().c_str());
        // (an agent's ops are the same ops: a page a person holds is not changed by one)
        Document held = run_ops(blank_book(), Json::array({line_op("speech")}));
        held = run_ops(held, Json::array({Json{{"op", "lock_page"}, {"page", 1}}}));
        try {
            (void)run_ops(held, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"balloon", "heart"}}}), "ai:letterer");
            QFAIL("a page a person holds was changed by an agent");
        } catch (const genko::core::ApplyError& error) {
            QVERIFY(QString::fromUtf8(error.what()).contains(QStringLiteral("locked by")));
        }
    }

    // What they refuse of the additions, said in Japanese — each range at both ends, and what is not a number or not a
    // tone at all.
    void theAdditionsRefusalsReadInJapanese_data() {
        QTest::addColumn<QString>("ops");
        QTest::addColumn<QString>("says");
        const auto style = [](const Json& st) { return QString::fromStdString(Json::array({line_op("speech", Json{{"style", st}})}).dump()); };
        const auto tone = [&](const Json& t) { return style(Json{{"fill_tone", t}}); };
        const QString swell = QStringLiteral("フキダシの線の強弱は 0〜0.9 です");
        QTest::newRow("pen_swell above") << style(Json{{"pen_swell", 0.95}}) << swell;
        QTest::newRow("pen_swell below") << style(Json{{"pen_swell", -0.1}}) << swell;
        QTest::newRow("pen_angle above") << style(Json{{"pen_angle", 361}}) << QStringLiteral("線の太い側の向きは 0〜360 度です");
        QTest::newRow("pen_angle below") << style(Json{{"pen_angle", -1}}) << QStringLiteral("線の太い側の向きは 0〜360 度です");
        QTest::newRow("pen_taper above") << style(Json{{"pen_taper", 1.2}}) << QStringLiteral("線の入り抜きは 0〜1 です");
        QTest::newRow("pen_taper below") << style(Json{{"pen_taper", -0.2}}) << QStringLiteral("線の入り抜きは 0〜1 です");
        QTest::newRow("pen_strokes above") << style(Json{{"pen_strokes", 9}}) << QStringLiteral("フキダシの線を何筆で描くかは 1〜8 です");
        QTest::newRow("pen_strokes below") << style(Json{{"pen_strokes", -2}}) << QStringLiteral("フキダシの線を何筆で描くかは 1〜8 です");
        QTest::newRow("drip_mm above") << style(Json{{"drip_mm", 31}}) << QStringLiteral("しずくの長さは 0〜30 mm です");
        QTest::newRow("drip_mm below") << style(Json{{"drip_mm", -1}}) << QStringLiteral("しずくの長さは 0〜30 mm です");
        QTest::newRow("drips above") << style(Json{{"drips", 25}}) << QStringLiteral("しずくの数は 1〜24 です");
        QTest::newRow("drips below") << style(Json{{"drips", -1}}) << QStringLiteral("しずくの数は 1〜24 です");
        QTest::newRow("drip_width_mm above") << style(Json{{"drip_width_mm", 11}}) << QStringLiteral("しずくの太さは 0.3〜10 mm です");
        QTest::newRow("drip_width_mm below") << style(Json{{"drip_width_mm", 0.1}}) << QStringLiteral("しずくの太さは 0.3〜10 mm です");
        QTest::newRow("drip_seed above") << style(Json{{"drip_seed", 1000000}}) << QStringLiteral("しずくの並びの番号は 0〜999999 です");
        QTest::newRow("drip_seed below") << style(Json{{"drip_seed", -1}}) << QStringLiteral("しずくの並びの番号は 0〜999999 です");
        QTest::newRow("points above") << style(Json{{"points", 13}}) << QStringLiteral("星の先と花びらの数は 4〜12 です");
        QTest::newRow("points below") << style(Json{{"points", 3}}) << QStringLiteral("星の先と花びらの数は 4〜12 です");
        QTest::newRow("tone density above") << tone(Json{{"density", 0.95}}) << QStringLiteral("フキダシの中のトーンの濃さは 0〜0.9（90%）です");
        QTest::newRow("tone density below") << tone(Json{{"density", -0.1}}) << QStringLiteral("フキダシの中のトーンの濃さは 0〜0.9（90%）です");
        QTest::newRow("tone density said in per cent") << tone(Json{{"density", 30}}) << QStringLiteral("フキダシの中のトーンの濃さは 0〜0.9（90%）です");
        QTest::newRow("tone to above") << tone(Json{{"density", 0.1}, {"to", 1.0}}) << QStringLiteral("フキダシの中のトーンの濃さは 0〜0.9（90%）です");
        QTest::newRow("tone lpi above") << tone(Json{{"density", 0.1}, {"lpi", 90}}) << QStringLiteral("フキダシの中のトーンの線数は 20〜85 です");
        QTest::newRow("tone lpi below") << tone(Json{{"density", 0.1}, {"lpi", 10}}) << QStringLiteral("フキダシの中のトーンの線数は 20〜85 です");
        QTest::newRow("tone pattern") << tone(Json{{"density", 0.1}, {"pattern", "cross"}}) << QStringLiteral("フキダシの中のトーンの柄は、点か線から選びます");
        const QString bad_tone = QStringLiteral("フキダシの中のトーンの指定が正しくありません");
        QTest::newRow("tone not an object") << style(Json{{"fill_tone", 0.3}}) << bad_tone;
        QTest::newRow("tone without a density") << tone(Json{{"lpi", 60}}) << QStringLiteral("フキダシの中のトーンには濃さが要ります");
        QTest::newRow("tone with a key it has not") << tone(Json{{"density", 0.1}, {"colour", "red"}}) << bad_tone;
        QTest::newRow("tone angle") << tone(Json{{"density", 0.1}, {"angle", 120}}) << QStringLiteral("フキダシの中のトーンの角度は 0〜90 度です");
        QTest::newRow("tone direction") << tone(Json{{"density", 0.1}, {"to", 0.5}, {"dir", 400}}) << QStringLiteral("フキダシの中のトーンが濃くなる向きは −360〜360 度です");
        QTest::newRow("tone inset") << tone(Json{{"density", 0.1}, {"inset_mm", 9}}) << QStringLiteral("フキダシの中のトーンを線から離す幅は 0〜5 mm です");
        QTest::newRow("tone density not a number") << tone(Json{{"density", "dark"}}) << bad_tone;
        const QString bad_blend = QStringLiteral("フキダシの中のグラデーションの指定が正しくありません");
        QTest::newRow("blend not an object") << style(Json{{"fill_gradient", "blue"}}) << bad_blend;
        QTest::newRow("blend without an end") << style(Json{{"fill_gradient", Json{{"dir", 90}}}}) << QStringLiteral("フキダシの中のグラデーションの先の色は、0〜255 の数 3 つで指定します");
        const QString colour = QStringLiteral("フキダシの中のグラデーションの先の色は、0〜255 の数 3 つで指定します");
        QTest::newRow("blend of two numbers") << style(Json{{"fill_gradient", Json{{"to", Json::array({1, 2})}}}}) << colour;
        QTest::newRow("blend past 255") << style(Json{{"fill_gradient", Json{{"to", Json::array({300, 0, 0})}}}}) << colour;
        QTest::newRow("blend to words") << style(Json{{"fill_gradient", Json{{"to", Json::array({1, 2, "x"})}}}}) << colour;
        QTest::newRow("blend direction") << style(Json{{"fill_gradient", Json{{"to", Json::array({1, 2, 3})}, {"dir", -400}}}}) << QStringLiteral("フキダシの中のグラデーションの向きは −360〜360 度です");
        QTest::newRow("blend with a key it has not") << style(Json{{"fill_gradient", Json{{"to", Json::array({1, 2, 3})}, {"from", Json::array({1, 2, 3})}}}}) << bad_blend;
        QTest::newRow("swell not a number") << style(Json{{"pen_swell", "heavy"}}) << QStringLiteral("フキダシの線の強弱は数で指定します");
        QTest::newRow("angle not a number") << style(Json{{"pen_angle", "down"}}) << QStringLiteral("線の太い側の向きは数で指定します");
        QTest::newRow("taper not a number") << style(Json{{"pen_taper", "x"}}) << QStringLiteral("線の入り抜きは数で指定します");
        QTest::newRow("strokes not a number") << style(Json{{"pen_strokes", "two"}}) << QStringLiteral("フキダシの線を何筆で描くかは整数で指定します");
        QTest::newRow("drip length not a number") << style(Json{{"drip_mm", "long"}}) << QStringLiteral("しずくの長さは数で指定します");
        QTest::newRow("drips not a number") << style(Json{{"drips", "many"}}) << QStringLiteral("しずくの数は整数で指定します");
        QTest::newRow("drip width not a number") << style(Json{{"drip_width_mm", "wide"}}) << QStringLiteral("しずくの太さは数で指定します");
        QTest::newRow("seed not a number") << style(Json{{"drip_seed", "x"}}) << QStringLiteral("しずくの並びの番号は整数で指定します");
        QTest::newRow("points not a number") << style(Json{{"points", "five"}}) << QStringLiteral("星の先と花びらの数は整数で指定します");
    }
    void theAdditionsRefusalsReadInJapanese() {
        QFETCH(QString, ops);
        QFETCH(QString, says);
        const QString said = refusal(Json::parse(ops.toStdString()));
        QVERIFY2(said.contains(says), qPrintable(said));
        QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(sentence(said)).hasMatch(), qPrintable(said));
    }

    // A symbol is sized for its words: the letters set in a balloon of that size are not made smaller, at any number of
    // points; its box is no narrower and no flatter than the symbol reads at; a box two thirds the size is too small. And a
    // line that drips is that much taller: its body is the box the words were measured for.
    void aSymbolIsSizedForItsWords() {
        struct Case {
            const char* kind;
            Json style;
            double narrow, flat;
        };
        const Case cases[] = {{"heart", Json::object(), 0.95, 1.35}, {"star", Json::object(), 0.85, 1.2}, {"star", Json{{"points", 4}, {"spike_depth", 0.6}}, 0.85, 1.2},
                              {"star", Json{{"points", 9}, {"spike_depth", 0.2}}, 0.85, 1.2}, {"flower", Json::object(), 0.85, 1.2},
                              {"flower", Json{{"points", 9}}, 0.85, 1.2}, {"diamond", Json::object(), 0.6, 1.8}};
        const std::vector<std::pair<std::vector<std::string>, std::string>> words = {
            {{"すき"}, "すき"}, {{"あいうえお", "かきくけ"}, "あいうえお\nかきくけ"}, {{"あ", "い", "う", "え", "お"}, "あ\nい\nう\nえ\nお"}};
        for (const Case& c : cases) {
            for (const auto& [breaks, text] : words) {
                const auto size = letter::measure_styled(breaks, c.kind, letter::kEmMm, c.style);
                const QString what = QStringLiteral("%1 %2 「%3」: %4 × %5").arg(c.kind).arg(QString::fromStdString(c.style.dump())).arg(QString::fromStdString(breaks.front())).arg(size[0]).arg(size[1]);
                QVERIFY2(size[0] >= size[1] * c.narrow - 1e-9 && size[0] <= size[1] * c.flat + 1e-9, qPrintable(what));
                Json op = line_op(c.kind, Json{{"w_mm", size[0]}, {"h_mm", size[1]}, {"text", text}});
                if (!c.style.empty()) op["style"] = c.style;
                const Document doc = run_ops(blank_book(), Json::array({op}));
                QVERIFY2(genko::render::text::text_layout(doc.story.front(), 600).em == 118, qPrintable(what));  // (5 mm at 600 dpi)
                // (and it is not far bigger than they need: in a box two thirds the size they are made smaller — the
                // words' ink is slimmer than the block they are measured as, and a column may be broken again, so a
                // box only a little smaller can still hold them)
                op["w_mm"] = size[0] * 0.66;
                op["h_mm"] = size[1] * 0.66;
                const Document tight = run_ops(blank_book(), Json::array({op}));
                QVERIFY2(genko::render::text::text_layout(tight.story.front(), 600).em < 118, qPrintable(what));
            }
        }
        // (the kind's own points without a style: measure and measure_styled agree; a star needs about twice its words)
        QCOMPARE(letter::measure({"あいうえお", "かきくけ"}, "star"), letter::measure_styled({"あいうえお", "かきくけ"}, "star", letter::kEmMm, Json::object()));
        QVERIFY(letter::symbol_room("star") > 1.6 && letter::symbol_room("star") < 2.6);
        QVERIFY(std::fabs(letter::symbol_room("diamond") - 2.0 * 1.02) < 1e-9);  // (a diamond holds a block half its box)
        QVERIFY(letter::symbol_room("star", 4, 0.4) > letter::symbol_room("star", 4, 0.8));
        QVERIFY(letter::symbol_room("flower") < letter::symbol_room("star"));
        // (a polygon's sides from the style, as before)
        QCOMPARE(letter::measure_styled({"ロッカク"}, "polygon", letter::kEmMm, Json{{"sides", 6}}), letter::measure({"ロッカク"}, "polygon", letter::kEmMm, 6));
        // (drips: the box that much taller — the body, core::body_box, is what the words were measured for; and the
        // letters are set in the body, at their size)
        const std::vector<std::string> breaks{"こわい", "よう"};
        const auto plain = letter::measure(breaks, "speech");
        const auto dripping = letter::measure_styled(breaks, "speech", letter::kEmMm, Json{{"drip_mm", 6}});
        QCOMPARE(dripping[0], plain[0]);
        QVERIFY(std::fabs(dripping[1] - (plain[1] + 6)) < 1e-9);
        const Document doc = run_ops(blank_book(), Json::array({line_op("speech", Json{{"w_mm", dripping[0]}, {"h_mm", dripping[1]}, {"text", "こわい\nよう"},
                                                                                         {"style", Json{{"drip_mm", 6}}}})}));
        const genko::core::StoryLine& line = doc.story.front();
        QVERIFY(std::fabs(genko::core::drip_reach(line) - 6) < 1e-9);
        const std::array<double, 4> body = genko::core::body_box(line);
        QVERIFY(std::fabs(body[3] - plain[1]) < 1e-6 && body[1] == 40.0 && body[2] == dripping[0]);
        QCOMPARE(genko::render::text::text_layout(line, 600).em, std::int64_t{118});
        // (the letters' box is the body's, an oval's own inside it: the checks measure the same)
        const std::array<double, 4> letters = genko::core::letter_box(line);
        QVERIFY(letters[1] + letters[3] <= body[1] + body[3] + 1e-9 && letters[3] < body[3]);
        // (at most 0.6 of the box; none for a balloon drawn by hand, or a kind with no body to drip from)
        Document tall = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"drip_mm", 30}}}}}));
        QVERIFY(std::fabs(genko::core::drip_reach(tall.story.front()) - 0.6 * dripping[1]) < 1e-6);
        tall = run_ops(tall, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"balloon", "flash"}}}));
        QCOMPARE(genko::core::drip_reach(tall.story.front()), 0.0);
        QCOMPARE(genko::core::body_box(tall.story.front())[3], dripping[1]);
        // (a line without drips: its own box, the very numbers)
        const Document none = run_ops(blank_book(), Json::array({line_op("speech", Json{{"x_mm", 12.345678901}, {"h_mm", 33.3333333333}})}));
        QCOMPARE(genko::core::body_box(none.story.front()), (std::array<double, 4>{12.345678901, 40.0, 40.0, 33.3333333333}));
    }

    // A tail's tip is hidden by a diamond and by a star where they are: the corner of a diamond's box and the valley
    // between a star's points are outside them; grown over a tail, they push its tip out past their own outline. (A
    // heart and a flower: the ellipse of their box, as a cloud.)
    void aTipIsHiddenByASymbolItself() {
        using genko::core::tail_hidden;
        Document doc = run_ops(blank_book(), Json::array({line_op("diamond", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 40}})}));
        const genko::core::StoryLine& diamond = doc.story.front();
        QVERIFY(tail_hidden(diamond, Json::array({60.0, 60.0})));
        QVERIFY(tail_hidden(diamond, Json::array({60.0, 41.0})));     // (its top corner)
        QVERIFY(!tail_hidden(diamond, Json::array({72.0, 48.0})));    // (inside the circle of its box, outside the diamond)
        QVERIFY(tail_hidden(diamond, Json::array({69.0, 51.5})));
        Document star_doc = run_ops(blank_book(), Json::array({line_op("star", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 40},
                                                                                     {"style", Json{{"points", 4}, {"spike_depth", 0.6}}}})}));
        const genko::core::StoryLine& sparkle = star_doc.story.front();
        QVERIFY(tail_hidden(sparkle, Json::array({60.0, 60.0})));
        QVERIFY(tail_hidden(sparkle, Json::array({60.0, 42.0})));     // (in its top point)
        QVERIFY(tail_hidden(sparkle, Json::array({78.0, 60.0})));     // (in its right point)
        QVERIFY(!tail_hidden(sparkle, Json::array({70.0, 50.0})));    // (between two points: inside the circle, outside the star)
        QVERIFY(!tail_hidden(sparkle, Json::array({52.0, 68.0})));
        // (a diamond moved over its tail: the tip goes out past the diamond's own edge that way — 45° down and right of a
        // square diamond of 40: its edge is 10√2 = 14.14 mm from the middle, 3 mm more)
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", Json::array({Json{{"to", Json::array({90.0, 90.0})}}})}}}));
        doc = run_ops(doc, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 66}, {"y_mm", 66}}}));  // (its middle now at 86, 86: over the tip)
        const Json tip = doc.story.front().tails.front()["to"];
        const double out = (14.142 + 3.0) / std::sqrt(2.0);
        QVERIFY2(std::fabs(tip[0].get<double>() - (86.0 + out)) < 0.15 && std::fabs(tip[1].get<double>() - (86.0 + out)) < 0.15, tip.dump().c_str());
        QVERIFY(!tail_hidden(doc.story.front(), tip));
        // (a star: out past the point it lies in)
        star_doc = run_ops(star_doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", Json::array({Json{{"to", Json::array({60.0, 90.0})}}})}}}));
        star_doc = run_ops(star_doc, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"y_mm", 62}}}));  // (its middle at 60, 82; its lower point reaches 102)
        const Json star_tip = star_doc.story.front().tails.front()["to"];
        QVERIFY2(std::fabs(star_tip[0].get<double>() - 60.0) < 0.02 && star_tip[1].get<double>() > 102.0 + 2.5 && star_tip[1].get<double>() < 102.0 + 3.5, star_tip.dump().c_str());
        // (the review's P3-6: a five-point star's middle — where its points stand round and its tails leave from — lies
        // below its box's; a tip moved out goes the way it lay from that middle, out past the star)
        Document five = run_ops(blank_book(), Json::array({line_op("star", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 40}})}));
        five = run_ops(five, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", Json::array({Json{{"to", Json::array({60.0, 74.0})}}})}}}));
        five = run_ops(five, Json::array({Json{{"op", "move_line"}, {"id", "L1"}, {"w_mm", 41}}}));  // (40..81 × 40..80)
        const genko::render::text::clean::P middle = genko::render::text::clean::star_centre(genko::render::text::clean::Box{40, 40, 81, 80}, 5, 0.62);
        QVERIFY(middle.y > 60.5);
        const Json moved = five.story.front().tails.front()["to"];
        const double mx = moved[0].get<double>() - middle.x, my = moved[1].get<double>() - middle.y;
        const double ox = 60.0 - middle.x, oy = 74.0 - middle.y;
        QVERIFY2(std::fabs(mx * oy - my * ox) / std::hypot(ox, oy) < 0.02, moved.dump().c_str());  // (on the way from the star's middle through where it was)
        QVERIFY2(my > 0 && !genko::core::tail_hidden(five.story.front(), moved), moved.dump().c_str());
        // (a line that drips: its tails are measured against its body — the box less the drips' reach)
        Document dripping = run_ops(blank_book(), Json::array({line_op("speech", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 40}, {"style", Json{{"drip_mm", 10}}}})}));
        const genko::core::StoryLine& line = dripping.story.front();  // (its body 40..80 × 40..70)
        QVERIFY(genko::core::tail_hidden(line, Json::array({60.0, 69.0})));
        QVERIFY(!genko::core::tail_hidden(line, Json::array({60.0, 74.0})));  // (inside the box, below the body: among the drips)
        dripping = run_ops(dripping, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"tails", Json::array({Json{{"to", Json::array({60.0, 74.0})}}})}},
                                                  Json{{"op", "move_line"}, {"id", "L1"}, {"x_mm", 40.5}}}));
        QCOMPARE(dripping.story.front().tails.front()["to"], Json::array({60.0, 74.0}));  // (not hidden: left where it points)
    }
};

QTEST_GUILESS_MAIN(TestFukidashiService)
#include "test_fukidashi_service.moc"
