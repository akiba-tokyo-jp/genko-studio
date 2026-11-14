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
};

QTEST_GUILESS_MAIN(TestFukidashiService)
#include "test_fukidashi_service.moc"
