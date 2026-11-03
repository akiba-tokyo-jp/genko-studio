// PC5 G-d (edit.checkSpelling, SPEC.md TEXT-02「スペルチェック」, service/spell.hpp): the English dictionary — upstream
// spell.rs's tests (PhotoCraft v0.2.0, MIT): the bundled list loads, its case rules, suggestions by distance then level, the
// misspellings and the user's words, the tokenizer and the distance — and Genko's own: Japanese ends a word and is never said
// to be misspelt, the notation table (表記の点検) — a place already right left alone —, a book's lines and type layers in
// order with their graphemes, the sound effects left out unless asked, one page, the words a whole word stands, the settings'
// forms read.

#include <QtTest>

#include <set>
#include <string>

#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/ops_registry.hpp"
#include "service/spell.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace sp = service::spell;

namespace {

std::set<std::string> none() { return {}; }

std::vector<std::string> words_of(const std::string& text) {
    const std::u32string cps = core::ruby::code_points(text);
    std::vector<std::string> out;
    for (const auto& [a, b] : sp::words(cps)) out.push_back(core::ruby::to_utf8(std::u32string_view(cps).substr(a, b - a)));
    return out;
}

core::Document book() {
    core::Document doc = core::new_episode("綴り", core::Num(1), 2, core::PageSpec::custom(50.8, 50.8, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        auto& p = doc.edit_page(i);
        p.frames.clear();
        p.layers.clear();
        core::Layer paint;
        paint.id = "paint";
        paint.role = core::LayerRole::User;
        paint.kind = core::LayerKind::Raster;
        p.layers.push_back(paint);
    }
    const auto run = [&](const Json& op) { doc = core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc; };
    run(Json{{"op", "add_line"}, {"page", 1}, {"text", "Teh quick brwon fox 出来る"}, {"x_mm", 2}, {"y_mm", 2}, {"w_mm", 30}, {"h_mm", 10}, {"wrap", "horizontal"}, {"id", "l1"}});
    run(Json{{"op", "add_line"}, {"page", 1}, {"text", "Doooon"}, {"x_mm", 2}, {"y_mm", 20}, {"w_mm", 30}, {"h_mm", 10}, {"balloon", "sfx"}, {"id", "l2"}});
    run(Json{{"op", "add_type_layer"}, {"page", 2}, {"text", "😀 recieve 3ヶ月"}, {"point", Json::array({4, 10})}, {"wrap", "horizontal"}, {"new_id", "t1"}});
    return doc;
}

}  // namespace

class TestSpell : public QObject {
    Q_OBJECT

private slots:
    // upstream: bundled_dictionary_loads
    void theDictionaryLoads() {
        const sp::Dictionary& d = sp::Dictionary::english();
        QVERIFY2(d.size() > 70000, std::to_string(d.size()).c_str());
        for (const char* w : {"the", "The", "THE", "color", "colors", "running", "don't", "don’t", "Paris", "PARIS", "photographer's", "NASA"})
            QVERIFY2(d.check(w, none()), w);
        for (const char* w : {"teh", "recieve", "paris", "Photograpy", "zxqv"}) QVERIFY2(!d.check(w, none()), w);
        QVERIFY(d.check("2026", none()) && d.check("B2B", none()));  // (numbers and symbols are never said)
    }

    // upstream: suggestions_rank_by_distance_then_frequency
    void suggestionsNearestFirst() {
        const sp::Dictionary& d = sp::Dictionary::english();
        QCOMPARE(QString::fromStdString(d.suggest("teh", 3).at(0)), QStringLiteral("the"));
        const auto receive = d.suggest("recieve", 5);
        QVERIFY(std::find(receive.begin(), receive.end(), "receive") != receive.end());
        QCOMPARE(d.suggest("Wrold", 1), std::vector<std::string>{"World"});
        QCOMPARE(d.suggest("brwon", 1), std::vector<std::string>{"brown"});
        const auto helo = d.suggest("HELO", 5);
        QVERIFY(!helo.empty());
        QCOMPARE(QString::fromStdString(helo.front()), QString::fromStdString(helo.front()).toUpper());
    }

    // upstream: misspellings_and_user_words (with the list's own #level lines)
    void userWords() {
        const sp::Dictionary d = sp::Dictionary::from_list("#10\nhello\nworld\nParis\n");
        QCOMPARE(d.size(), std::size_t{3});
        QVERIFY(d.check("Hello", none()) && !d.check("wrld", none()) && !d.check("paris", none()) && d.check("PARIS", none()));
        QVERIFY(!d.check("photocraft", none()));
        QVERIFY(d.check("Photocraft", std::set<std::string>{"photocraft"}));
        // (the settings' words: one a line or a tab, lowercase)
        QCOMPARE(sp::user_words("Genko\tPhotoCraft\n  manga \n\n"), (std::set<std::string>{"genko", "manga", "photocraft"}));
    }

    // upstream: tokenizer_and_distance; Genko's: kana, kanji and a fullwidth word end a word, never one
    void words() {
        QCOMPARE(words_of("it's well-known 'quoted' café"), (std::vector<std::string>{"it's", "well", "known", "quoted", "café"}));
        QCOMPARE(words_of("これはTestですhello世界"), (std::vector<std::string>{"Test", "hello"}));
        QCOMPARE(words_of("ＡＢＣ abc"), (std::vector<std::string>{"abc"}));
        QCOMPARE(words_of("café ok"), (std::vector<std::string>{"café", "ok"}));  // (a mark on a letter: the word's)
        QVERIFY(words_of("出来る　ヶ月").empty());
    }

    // 表記の表: read from the settings' forms, entries that say nothing left out
    void theNotationTable() {
        const auto table = sp::notations_of(Json::parse(R"([{"from": "出来る", "to": "できる"}, ["ヶ月", "か月"], {"from": "", "to": "x"},
            {"from": "同じ", "to": "同じ"}, 7, {"from": 1, "to": "a"}])"));
        QCOMPARE(table.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(table[1].from), QStringLiteral("ヶ月"));
        QCOMPARE(sp::notations_json(table).dump(), std::string(R"([{"from":"出来る","to":"できる"},{"from":"ヶ月","to":"か月"}])"));
        QVERIFY(sp::notations_of(Json("not a list")).empty());
    }

    // A book: its lines in reading order, then its type layers; graphemes; the sound effects left out; the notation table
    void aBook() {
        const core::Document doc = book();
        sp::Options options;
        options.notations = sp::notations_of(Json::parse(R"([["出来る", "できる"], ["ヶ月", "か月"]])"));
        const auto found = sp::check(doc, options);
        QStringList seen;
        for (const auto& f : found) seen << QStringLiteral("%1:%2:%3-%4:%5").arg(QString::fromStdString(f.kind), QString::fromStdString(f.line.empty() ? f.layer_id : f.line)).arg(f.start).arg(f.end).arg(QString::fromStdString(f.word));
        QCOMPARE(seen, (QStringList{QStringLiteral("spelling:l1:0-3:Teh"), QStringLiteral("spelling:l1:10-15:brwon"), QStringLiteral("notation:l1:20-23:出来る"),
                                    QStringLiteral("spelling:t1:2-9:recieve"), QStringLiteral("notation:t1:11-13:ヶ月")}));
        QCOMPARE(QString::fromStdString(found[0].suggestions.at(0)), QStringLiteral("The"));
        QCOMPARE(found[2].suggestions, std::vector<std::string>{"できる"});
        QCOMPARE(found[3].page.json(), Json(2));
        // (Doooon, a sound effect: asked for)
        options.sfx = true;
        options.notation = false;
        QStringList words;
        for (const auto& f : sp::check(doc, options)) words << QString::fromStdString(f.word);
        QVERIFY(words.contains(QStringLiteral("Doooon")));
        // (one page; the user's words; ignored ones)
        options.page = core::Num(2);
        options.user = {"recieve"};
        QVERIFY(sp::check(doc, options).empty());
        options.page.reset();
        options.user.clear();
        options.ignore = {"teh", "brwon"};
        words.clear();
        for (const auto& f : sp::check(doc, options)) words << QString::fromStdString(f.word);
        QCOMPARE(words, (QStringList{QStringLiteral("Doooon"), QStringLiteral("recieve")}));
        // (JSON)
        const Json out = sp::to_json(found);
        QCOMPARE(out["count"], Json(5));
        QCOMPARE(out["findings"][3]["layer_id"], Json("t1"));
        QVERIFY(!out["findings"][3].contains("line"));
    }

    // A notation already right where it stands is left alone (「出来」 inside 「出来る」 told to be 出来る)
    void aPlaceAlreadyRight() {
        core::Document doc = book();
        sp::Options options;
        options.spelling = false;
        options.notations = sp::notations_of(Json::parse(R"([["出来", "出来る"]])"));
        QVERIFY(sp::check(doc, options).empty());
        options.notations = sp::notations_of(Json::parse(R"([["来る", "くる"]])"));
        QCOMPARE(sp::check(doc, options).size(), std::size_t{1});
    }

    // Where a word stands whole (すべて変更): not inside another word; a notation word where it stands
    void placesOf() {
        QCOMPARE(sp::places_of("teh tehteh teh.", "teh"), (std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 3}, {11, 14}}));
        QCOMPARE(sp::places_of("😀ヶ月と2ヶ月", "ヶ月"), (std::vector<std::pair<std::int64_t, std::int64_t>>{{1, 3}, {5, 7}}));
        QVERIFY(sp::places_of("abc", "").empty());
    }

    // The dictionary in a thousand lines: under a few seconds (it is read once)
    void aThousandLines() {
        core::Document doc = book();
        std::string many;
        for (int i = 0; i < 200; ++i) many += "The quick brown fox jumps over teh lazy dog. ";
        for (auto& line : doc.story) line.text = many;
        QElapsedTimer clock;
        clock.start();
        sp::Options options;
        options.sfx = true;
        const auto found = sp::check(doc, options);
        QVERIFY2(clock.elapsed() < 10000, std::to_string(clock.elapsed()).c_str());
        QVERIFY(found.size() >= 400);
    }

    // (review P1-1) a type layer with no record — a broken book, opened read-only —: passed over, never read
    void aTypeLayerWithNoRecord() {
        core::Document doc = book();
        core::Layer broken;
        broken.id = "t_broken";
        broken.role = core::LayerRole::User;
        broken.kind = core::LayerKind::Type;
        doc.edit_page(1).layers.push_back(broken);
        QVERIFY(core::type_text::is_type_layer(doc.page(1).layers.back()) && !doc.page(1).layers.back().type);
        const auto found = sp::check(doc, sp::Options{});
        QVERIFY(std::none_of(found.begin(), found.end(), [](const sp::Finding& f) { return f.layer_id == "t_broken"; }));
        QVERIFY(std::any_of(found.begin(), found.end(), [](const sp::Finding& f) { return f.layer_id == "t1"; }));
    }

    // (review P2-7) none asked: no suggestions made in the check — the window makes the shown place's (suggestions_for);
    // (P3-9) a one-letter notation over a long text: no more places made than the findings take
    void suggestionsWhenShownAndTheRoom() {
        sp::Options options;
        options.suggestions = 0;
        options.notation = false;
        const auto found = sp::check(book(), options);
        QVERIFY(!found.empty());
        for (const sp::Finding& f : found) QVERIFY(f.suggestions.empty());
        const auto teh = std::find_if(found.begin(), found.end(), [](const sp::Finding& f) { return f.word == "Teh"; });
        QVERIFY(teh != found.end());
        QVERIFY(sp::suggestions_for(*teh).front() == "The");
        core::Document doc = book();
        std::string kana;
        for (int i = 0; i < 20000; ++i) kana += "の";
        doc.story[0].text = kana;
        sp::Options few;
        few.spelling = false;
        few.most = 50;
        for (int i = 0; i < 40; ++i) few.notations.push_back(sp::Notation{"の", "ノ"});
        QElapsedTimer clock;
        clock.start();
        const auto places = sp::check(doc, few);
        QCOMPARE(places.size(), std::size_t{50});
        QCOMPARE(places.front().start, std::int64_t{0});
        QCOMPARE(places.back().start, std::int64_t{1});  // (each of the 40 rows found at 0 first, then at 1: the first 50 by place)
        QVERIFY2(clock.elapsed() < 5000, std::to_string(clock.elapsed()).c_str());
    }
};

QTEST_GUILESS_MAIN(TestSpell)
#include "test_spell.moc"
