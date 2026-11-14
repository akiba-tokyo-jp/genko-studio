// fonts@1 (書体の追加 F2, 群 2): the forty added lettering faces as the service and the ops tell of them.
//
//   - inspect target=fonts: the seven faces of before as they were, then the forty with what an agent chooses by (the
//     category, the use, how much kanji, the partner, whether the file is on this computer — one stat a face, nothing
//     read) and the categories;
//   - lint: a look of the book's bible that names one of the forty is known (never unknown_font), and said when its file
//     is not on this computer;
//   - replace_fonts: a face Genko comes with is not a missing font — neither listed as one nor taken out of the book by
//     "all" or a map when its file is not on this computer —, and is told apart (not_installed);
//   - the editable PSD of a line in a face whose file is not here names the face asked for, by its PostScript name;
//   - what a person or an agent reads of all this is Japanese with no run of four Latin letters or more.

#include <QtTest>

#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/font_catalog.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "formats/psd_text.hpp"
#include "render/font_files.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "render/text/lettering.hpp"
#include "service/briefs.hpp"
#include "service/lint.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
namespace files = genko::render::font_files;
namespace fonts = genko::core::fonts;
namespace service = genko::service;

namespace {

std::string source_fonts() { return (genko::test::repo_root() + QStringLiteral("/native/resources/fonts")).toStdString(); }

void use_source_fonts() {
    files::set_dir_for_testing(source_fonts());
    genko::render::text::clear_layout_cache();
}

genko::core::ApplyResult run_ops(const Document& doc, const Json& ops) {
    return genko::core::CommandBus(genko::render::ops_registry()).apply(doc, ops, genko::core::Actor("human:作者"));
}

Document book_of(const std::vector<std::pair<const char*, const char*>>& lines) {
    Document doc = genko::core::new_episode("書体", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
    Json ops = Json::array();
    double x = 20;
    for (const auto& [id, font] : lines) {
        Json op{{"op", "add_line"}, {"page", 1}, {"id", id}, {"balloon", "none"}, {"x_mm", x}, {"y_mm", 30}, {"w_mm", 24}, {"h_mm", 60},
                {"wrap", "vertical"}, {"text", "かなと漢字"}, {"style", Json{{"size_mm", 5.0}}}};
        if (*font != '\0') op["style"]["font"] = font;
        ops.push_back(op);
        x += 28;
    }
    return run_ops(doc, ops).doc;
}

const genko::core::StoryLine& line_of(const Document& doc, const std::string& id) {
    for (const auto& line : doc.story) {
        if (line.id == id) return line;
    }
    qFatal("no line %s", id.c_str());
}

bool reads_in_japanese(const std::string& text) {
    static const QRegularExpression latin(QStringLiteral("[A-Za-z]{4,}"));
    static const QRegularExpression kana(QStringLiteral("[ぁ-んァ-ヶ一-龥]"));
    const QString t = QString::fromStdString(text);
    return kana.match(t).hasMatch() && !latin.match(t).hasMatch();
}

}  // namespace

class TestFontService : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { use_source_fonts(); }
    void init() {
        if (files::dir() != source_fonts()) use_source_fonts();
    }
    void cleanupTestCase() { files::set_dir_for_testing(std::nullopt); }

    // inspect target=fonts: `bundled` begins with the seven rows of before, each {key, label} and nothing else, as they
    // always were; then the forty, in the catalogue's order, each with its category, use, kanji, its partner where it has
    // one, and whether its file is there; `categories` names the eleven. Nothing is read to say whether a file is there.
    void inspectListsTheSevenThenTheForty() {
        files::clear_cache();
        const Json listed = service::briefs::fonts_list();
        const Json& bundled = listed["bundled"];
        QCOMPARE(bundled.size(), std::size_t(47));
        const auto& seven = service::lint::bundled_fonts();
        QCOMPARE(seven.size(), std::size_t(7));
        for (std::size_t i = 0; i < 7; ++i) {
            QCOMPARE(bundled[i], (Json{{"key", seven[i].first}, {"label", seven[i].second}}));
            QCOMPARE(bundled[i].size(), std::size_t(2));
        }
        std::size_t at = 7, partners = 0;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added) continue;
            const Json& row = bundled[at++];
            Json expected{{"key", e.key}, {"label", e.label}, {"category", e.category}, {"use", e.use}, {"kanji", e.kanji}};
            if (!e.fallback.empty()) {
                expected["fallback"] = e.fallback;
                ++partners;
            }
            expected["available"] = true;
            std::string where;
            QVERIFY2(genko::test::strict_equal(row, expected, &where), (e.key + ": " + where).c_str());
            QVERIFY2(reads_in_japanese(e.label) && reads_in_japanese(e.use), e.key.c_str());
        }
        QCOMPARE(at, std::size_t(47));
        QCOMPARE(partners, std::size_t(7));
        QCOMPARE(listed["categories"].size(), std::size_t(11));
        QCOMPARE(listed["categories"][0], (Json{{"id", "dialogue"}, {"label", "台詞"}}));
        for (const Json& c : listed["categories"]) QVERIFY(reads_in_japanese(c["label"].get<std::string>()));
        // (the keys an agent may use are one list: every key once)
        std::vector<std::string> keys;
        for (const Json& row : bundled) keys.push_back(row["key"].get<std::string>());
        std::sort(keys.begin(), keys.end());
        QVERIFY(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
        // (the other parts as they were)
        QVERIFY(listed.contains("system") && listed.contains("families"));
        QCOMPARE(listed["note"], Json("style.font takes a bundled key or a system font's path; style.weight normal|bold|heavy thickens any face"));
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));  // (no font file was read to say all this)

        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        const Json without = service::briefs::fonts_list();
        const std::uint64_t read = files::cached_bytes();
        use_source_fonts();
        QCOMPARE(read, std::uint64_t(0));
        QCOMPARE(without["bundled"].size(), std::size_t(47));
        for (std::size_t i = 0; i < 47; ++i) {
            if (i < 7) QVERIFY(!without["bundled"][i].contains("available"));
            else QCOMPARE(without["bundled"][i]["available"], Json(false));
        }
    }

    // lint: a look that names one of the forty is a face Genko knows — never unknown_font —; when its file is not on this
    // computer that is said (font_not_installed), by the face's name, in Japanese. A name nobody knows is unknown as before.
    void lintKnowsTheAddedFaces() {
        const Json bible = Json::parse(R"({"lettering": {"shout": {"font": "gothic_kiwami"}, "whisper": {"font": "gothic"},
                                                          "aside": {"font": "no_such_face"}, "narration": {"font": "klee"}}})");
        const auto codes = [&] {
            std::vector<std::string> out;
            for (const genko::studio::Issue& issue : service::lint::lint_bible(bible)) {
                out.push_back(issue.code + " " + issue.path + " " + issue.message + " / " + issue.hint);
            }
            return out;
        };
        const std::vector<std::string> unknown = {"unknown_font /lettering/aside/font 書体 no_such_face が見つからない（台詞の書体になる） / "
                                                  "inspect target=fonts の bundled の key か、パソコンの書体のパス"};
        QCOMPARE(codes(), unknown);
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        const std::vector<std::string> without = codes();
        use_source_fonts();
        QCOMPARE(without.size(), std::size_t(3));
        QCOMPARE(without[0], std::string("font_not_installed /lettering/shout/font 書体「源暎きわみゴ」のファイルがこのパソコンに無い（アンチックで描かれる） / "
                                         "アプリを入れ直すと元の書体で描ける"));
        QCOMPARE(without[1], unknown[0]);
        QCOMPARE(without[2], std::string("font_not_installed /lettering/narration/font 書体「クレー One SB」のファイルがこのパソコンに無い（アンチックで描かれる） / "
                                         "アプリを入れ直すと元の書体で描ける"));
        // (every one of the forty, said in Japanese without its key)
        files::set_dir_for_testing(empty.path().toStdString());
        std::vector<std::string> said;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added) continue;
            const auto issues = service::lint::lint_bible(Json{{"lettering", Json{{"shout", Json{{"font", e.key}}}}}});
            if (issues.size() == 1 && issues[0].code == "font_not_installed") said.push_back(issues[0].message + issues[0].hint);
        }
        use_source_fonts();
        QCOMPARE(said.size(), std::size_t(40));
        for (const std::string& text : said) {
            QVERIFY2(reads_in_japanese(text), text.c_str());
            QVERIFY2(text.find('_') == std::string::npos, text.c_str());
        }
    }

    // replace_fonts and the faces Genko comes with: on a computer without their files they are not "missing fonts". The
    // question lists only the font file that is not there, and tells the faces apart (not_installed, by key and name);
    // "all" takes the missing font out and leaves the face in the book; a map that names the face changes nothing of it.
    // With the files there, the answer has no such key at all.
    void replaceAllNeverStripsAnAddedFace() {
        Document doc = book_of({{"a", "klee"}, {"b", "/no/such/font.otf"}, {"c", "gothic_kiwami"}, {"d", ""}, {"e", "klee"}});
        doc.font_path = "antique_genei";
        {
            const genko::core::ApplyResult asked = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
            QCOMPARE(asked.results[0]["missing"], Json::array({Json{{"font", "/no/such/font.otf"}, {"texts", 1}}}));
            QVERIFY(!asked.results[0].contains("not_installed"));
        }
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        genko::render::text::clear_layout_cache();
        [&] {
            const genko::core::ApplyResult asked = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
            QVERIFY(asked.unchanged);
            QCOMPARE(asked.results[0]["missing"], Json::array({Json{{"font", "/no/such/font.otf"}, {"texts", 1}}}));
            QCOMPARE(asked.results[0]["not_installed"],
                     Json::array({Json{{"font", "antique_genei"}, {"label", "源暎アンチック"}}, Json{{"font", "klee"}, {"label", "クレー One SB"}},
                                  Json{{"font", "gothic_kiwami"}, {"label", "源暎きわみゴ"}}}));
            const Document all = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}, {"all", true}}})).doc;
            QCOMPARE(line_of(all, "a").style.value("font", std::string()), std::string("klee"));
            QCOMPARE(line_of(all, "c").style.value("font", std::string()), std::string("gothic_kiwami"));
            QCOMPARE(line_of(all, "e").style.value("font", std::string()), std::string("klee"));
            QVERIFY(!line_of(all, "b").style.contains("font"));  // (the font file that is not there: taken out, as before)
            QCOMPARE(all.font_path, std::string("antique_genei"));
            QCOMPARE(fonts::named(all), (std::vector<std::string>{"antique_genei", "gothic_kiwami", "klee"}));
            // (a map that names the face: it is not a missing font, so it stays)
            const Document mapped = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}, {"map", Json{{"klee", "mincho"}, {"/no/such/font.otf", "maru"}}}}})).doc;
            QCOMPARE(line_of(mapped, "a").style.value("font", std::string()), std::string("klee"));
            QCOMPARE(line_of(mapped, "b").style.value("font", std::string()), std::string("maru"));
        }();
        use_source_fonts();
    }

    // The editable PSD of a line in a face whose file is not on this computer: its letters are set in the default face
    // here — and its text layer names the face the line asks for, by the PostScript name the catalogue holds, as does
    // the list of fonts the export tells; the missing font is warned of as before. A line in a font file that is not
    // there is named by the default face, as it always was.
    void psdNamesTheFaceAskedForWithoutItsFile() {
        const Document doc = book_of({{"a", "klee"}, {"b", "/no/such/font.otf"}, {"c", "cute_daruma"}});
        QTemporaryDir empty, out;
        files::set_dir_for_testing(empty.path().toStdString());
        genko::render::text::clear_layout_cache();
        [&] {
            genko::formats::PsdTextReport report;
            const auto written = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8((out.path() + QStringLiteral("/本")).toStdString()), 150,
                                                                  genko::formats::PsdText{true, false}, &report);
            QCOMPARE(written.size(), std::size_t(1));
            const genko::render::psd::File file = genko::render::psd::File::read(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(written.front()))));
            const auto fonts_in = [&](const char* name) {
                std::vector<std::string> names;
                for (const genko::render::psd::Layer& layer : file.layers()) {
                    if (layer.name != name) continue;
                    const std::optional<genko::render::psd::TextLayer> text = file.text(layer);
                    if (!text) continue;
                    for (const auto& run : text->runs) {
                        if (std::find(names.begin(), names.end(), run.style.font) == names.end()) names.push_back(run.style.font);
                    }
                }
                return names;
            };
            QCOMPARE(fonts_in("本文 #a"), std::vector<std::string>{"KleeOne-SemiBold"});
            QCOMPARE(fonts_in("本文 #c"), std::vector<std::string>{"DarumadropOne-Regular"});
            QCOMPARE(fonts_in("本文 #b"), (std::vector<std::string>{"ZenOldMincho-SemiBold", "ZenKakuGothicNew-Bold"}));
            std::vector<std::string> warned;
            for (const Json& w : report.warnings) warned.push_back(w["line"].get<std::string>() + ":" + w["kind"].get<std::string>());
            QCOMPARE(warned, (std::vector<std::string>{"a:font_missing", "b:font_missing", "c:font_missing"}));
            QCOMPARE(report.fonts, (std::vector<std::string>{"KleeOne-SemiBold", "ZenOldMincho-SemiBold", "ZenKakuGothicNew-Bold", "DarumadropOne-Regular"}));
            const auto listed = genko::formats::psd_text_fonts(doc);
            QCOMPARE(listed.fonts, report.fonts);
        }();
        use_source_fonts();
    }
};

QTEST_GUILESS_MAIN(TestFontService)
#include "test_font_service.moc"
