// fonts@1 (書体の追加 F2, core/font_catalog.hpp): the catalogue of the lettering faces and the feature a book names.
//
//   - the catalogue: the 46 fonts of native/resources/fonts/FONTS.json — the six of before, compiled into the program, and
//     the forty added, installed as files — embedded byte for byte; keys that are safe as names on any system; every
//     face without kanji with a partner that has them;
//   - the feature: a book names fonts@1 while it names one of the forty (a line's style, a ruby, a record's character
//     attributes, a type layer's record, a type style, the book's usual face) and at no other time — never for the seven
//     keys of before, a font file's path or a name nobody knows —; a book that names it is read by this build, writable,
//     whatever font files the computer has (the reader looks at none: this test does not even link the drawing).

#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/font_catalog.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/type_text.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
namespace fonts = genko::core::fonts;

namespace {

Document blank_book() { return genko::core::new_episode("書体", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono()); }

genko::core::StoryLine line_of(const char* id, const std::string& text) {
    genko::core::StoryLine line;
    line.id = id;
    line.page_index = genko::core::Num(1);
    line.text = text;
    // (floats, as a book read has them: written and read again, the same bytes)
    line.x_mm = genko::core::Num(40.0);
    line.y_mm = genko::core::Num(40.0);
    line.w_mm = genko::core::Num(40.0);
    line.h_mm = genko::core::Num(20.0);
    return line;
}

Json payload_of(const Document& doc) {
    QTemporaryDir dir;
    genko::storage::AssetStore store(genko::core::path_from_utf8(dir.path().toStdString()));
    return genko::storage::project_payload_v4(doc, store);
}

bool names_feature(const Json& payload) {
    const Json& features = payload["features"];
    return std::find(features.begin(), features.end(), Json(std::string(fonts::kFontsFeature))) != features.end();
}

// A book that names `font` in one of the places a book names a face.
Document book_naming(const std::string& place, const std::string& font) {
    Document doc = blank_book();
    genko::core::StoryLine line = line_of("L1", "漢字とかな");
    if (place == "line") {
        line.style["font"] = font;
    } else if (place == "ruby") {
        genko::core::RubySpan span;
        span.start = 0;
        span.end = 2;
        span.ruby = "かんじ";
        span.base = "漢字";
        span.font = font;
        line.ruby_spans = std::vector<genko::core::RubySpan>{span};
        line.ruby_runs = {Json::array({"漢字", "かんじ"})};
    } else if (place == "record") {
        line.native_type = genko::core::type_text::check_native_type(Json{{"char", Json{{"font", font}}}}, line.text);
    } else if (place == "range") {
        line.native_type =
            genko::core::type_text::check_native_type(Json{{"runs", Json::array({Json{{"start", 0}, {"end", 2}, {"char", Json{{"font", font}}}}})}}, line.text);
    } else if (place == "type layer") {
        genko::core::StoryLine record = line_of("", "文字のレイヤー");
        record.style["font"] = font;
        genko::core::Layer layer;
        layer.id = "ly_type";
        layer.kind = genko::core::LayerKind::Type;
        layer.title = "文字";
        layer.type = std::make_shared<const genko::core::StoryLine>(std::move(record));
        doc.edit_page(0).layers.push_back(std::move(layer));
    } else if (place == "type style") {
        doc.type_styles = genko::core::type_text::check_type_styles(
            Json{{"character", Json::array({Json{{"id", "cs-1"}, {"name", "見出し"}, {"char", Json{{"font", font}}}}})}});
    } else if (place == "usual face") {
        doc.font_path = font;
    }
    if (place != "type layer") doc.story.push_back(std::move(line));
    return doc;
}

const char* const kPlaces[] = {"line", "ruby", "record", "range", "type layer", "type style", "usual face"};

}  // namespace

class TestFontCatalog : public QObject {
    Q_OBJECT

private slots:
    // The catalogue holds 46 fonts: the six of before first, in their order and compiled in, then the forty added; the
    // composite of before (antique: kana from mincho, the rest from gothic) is not a font but is written down; the 11
    // categories every font is in one of.
    void catalogHolds46FacesAndTheSevenOfBefore() {
        const std::vector<fonts::Entry>& all = fonts::entries();
        QCOMPARE(all.size(), std::size_t(46));
        const char* const six[] = {"gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"};
        for (std::size_t i = 0; i < 6; ++i) {
            QCOMPARE(all[i].key, std::string(six[i]));
            QVERIFY(!all[i].added);
            QVERIFY(!fonts::added(six[i]));
            QVERIFY(all[i].fallback.empty());
            QVERIFY(all[i].lookalike.empty());
        }
        std::size_t added = 0;
        for (std::size_t i = 6; i < all.size(); ++i) {
            QVERIFY2(all[i].added, all[i].key.c_str());
            QVERIFY(fonts::added(all[i].key));
            QCOMPARE(fonts::find(all[i].key), &all[i]);
            ++added;
        }
        QCOMPARE(added, std::size_t(40));
        // (antique is the face of a line that names none: a composite of two of the six, never one of the forty)
        QVERIFY(fonts::find("antique") == nullptr);
        QVERIFY(!fonts::added("antique"));
        const Json& raw = fonts::catalog();
        QCOMPARE(raw["schema"], Json("genko-fonts@1"));
        QCOMPARE(raw["composites"].size(), std::size_t(1));
        QCOMPARE(raw["composites"][0]["key"], Json("antique"));
        QCOMPARE(raw["composites"][0]["kana"], Json("mincho"));
        QCOMPARE(raw["composites"][0]["other"], Json("gothic"));
        QVERIFY(raw["composites"][0]["feature"].is_null());
        QVERIFY(!raw.contains("composite_suggestions"));  // (folded into each font's fallback)
        QCOMPARE(raw["counts"]["fonts"], Json(46));
        QCOMPARE(raw["counts"]["embedded"], Json(6));
        QCOMPARE(raw["counts"]["file"], Json(40));
        std::int64_t bytes_file = 0, bytes_total = 0;
        for (const Json& f : raw["fonts"]) {
            // (what the program reads of each font is said in so many words)
            QVERIFY(f["storage"] == Json("embedded") || f["storage"] == Json("file"));
            QCOMPARE(f["feature"], f["storage"] == Json("file") ? Json("fonts@1") : Json(nullptr));
            QVERIFY(f.contains("fallback"));
            QVERIFY(f["vertical"]["presentation_forms"].is_boolean());
            QVERIFY(!f.contains("status"));
            bytes_total += f["bytes"].get<std::int64_t>();
            if (f["storage"] == Json("file")) bytes_file += f["bytes"].get<std::int64_t>();
        }
        QCOMPARE(raw["counts"]["bytes_total"], Json(bytes_total));
        QCOMPARE(raw["counts"]["bytes_file"], Json(bytes_file));

        const std::vector<fonts::Category>& categories = fonts::categories();
        const char* const ids[] = {"dialogue", "narration", "shout", "maru", "cute", "hand", "brush", "horror", "machine", "sfx", "other"};
        QCOMPARE(categories.size(), std::size_t(11));
        std::set<std::string> known;
        for (std::size_t i = 0; i < categories.size(); ++i) {
            QCOMPARE(categories[i].id, std::string(ids[i]));
            QVERIFY(!categories[i].label.empty());
            known.insert(categories[i].id);
        }
        std::set<std::string> used;
        for (const fonts::Entry& e : all) {
            QVERIFY2(known.contains(e.category), e.key.c_str());
            used.insert(e.category);
            QVERIFY2(!e.label.empty() && !e.use.empty() && !e.family.empty() && !e.postscript.empty(), e.key.c_str());
            QVERIFY2(e.kanji == "full" || e.kanji == "partial" || e.kanji == "none", e.key.c_str());
        }
        QCOMPARE(used, known);  // (no category without a face)
    }

    // A key is a plain name on every system: lowercase ASCII letters, digits and _ from a letter on (no slash, backslash,
    // colon, dot or space: never a path, a drive or an NTFS stream), not a Windows device name, at most 32 characters, one
    // per face; the files are plain names too, one per face, as are the PostScript names a PSD names the faces by.
    void keysAreLowercaseAsciiAndNoDeviceName() {
        const std::set<std::string> devices = {"con", "prn", "aux", "nul", "com0", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                               "lpt0", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", "conin$", "conout$"};
        std::set<std::string> keys, files, lower_files, names;
        for (const fonts::Entry& e : fonts::entries()) {
            QVERIFY2(!e.key.empty() && e.key.size() <= 32, e.key.c_str());
            QVERIFY2(e.key[0] >= 'a' && e.key[0] <= 'z', e.key.c_str());
            for (const char c : e.key) QVERIFY2((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', e.key.c_str());
            QVERIFY2(!devices.contains(e.key), e.key.c_str());
            QVERIFY2(keys.insert(e.key).second, e.key.c_str());
            QVERIFY2(e.key != "antique", e.key.c_str());
            for (const char c : e.file) {
                QVERIFY2((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.', e.file.c_str());
            }
            QVERIFY2(e.file.size() > 4 && e.file.ends_with(".ttf") && e.file[0] != '.', e.file.c_str());
            QVERIFY2(files.insert(e.file).second, e.file.c_str());
            // (two files that differ only in case would be one file on Windows)
            QVERIFY2(lower_files.insert(QString::fromStdString(e.file).toLower().toStdString()).second, e.file.c_str());
            QVERIFY2(names.insert(e.postscript).second, e.postscript.c_str());
            QCOMPARE(e.sha256.size(), std::size_t(64));
            for (const char c : e.sha256) QVERIFY((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            QVERIFY(e.bytes > 0 && e.bytes <= 12LL * 1024 * 1024);  // (F1's rule: no file past 12 MB)
            if (e.added) QCOMPARE(fonts::by_postscript(e.postscript), &e);
            else QVERIFY(fonts::by_postscript(e.postscript) == nullptr);  // (the six are named by the drawing's own table)
        }
        QVERIFY(fonts::by_postscript("") == nullptr);
        QVERIFY(fonts::by_postscript("NoSuchFont-Regular") == nullptr);
        // (keys are compared exactly: another case is another name)
        QVERIFY(fonts::find("Dot16") == nullptr);
        QVERIFY(fonts::find("dot16 ") == nullptr);
        QVERIFY(fonts::find("") == nullptr);
    }

    // A face that has no kanji, or only some, names a partner; the partner is another face, has all the kanji and no
    // partner of its own (never a chain); a face with all the kanji names none. The faces drawn without the full-width
    // ！？ — and only they — name the ASCII marks as their look-alikes.
    void everyPartnerIsAFullFaceThatIsNotItself() {
        std::set<std::string> partnered, alike;
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.kanji != "full") {
                QVERIFY2(!e.fallback.empty(), e.key.c_str());
            } else {
                QVERIFY2(e.fallback.empty(), e.key.c_str());
            }
            if (!e.fallback.empty()) {
                const fonts::Entry* partner = fonts::find(e.fallback);
                QVERIFY2(partner != nullptr, e.key.c_str());
                QVERIFY2(partner->key != e.key, e.key.c_str());
                QCOMPARE(partner->kanji, std::string("full"));
                QVERIFY2(partner->kana_complete || partner->key == "pop_mochiy", partner->key.c_str());  // (pop_mochiy lacks only ゛゜ alone)
                QVERIFY2(partner->fallback.empty(), e.key.c_str());
                // (a partner is one of the forty: its letters are read from its cmap as the face's own are. A font of before
                // is read Pillow's way, which draws the "no glyph" box for a bracket set down whose vertical form it lacks —
                // ［］ in the handwriting of before, most brackets in its effects face —, and that must stay as it is for the
                // books of before: so no added face leans on one)
                QVERIFY2(partner->added, e.key.c_str());
                partnered.insert(e.key);
            }
            if (!e.lookalike.empty()) {
                QVERIFY2(e.added, e.key.c_str());
                QCOMPARE(e.lookalike.size(), std::size_t(2));
                QCOMPARE(e.lookalike[0], std::make_pair(U'！', U'!'));
                QCOMPARE(e.lookalike[1], std::make_pair(U'？', U'?'));
                alike.insert(e.key);
            }
        }
        QCOMPARE(partnered, (std::set<std::string>{"cute_cherry", "cute_daruma", "hand_slackside", "horror_chokoku", "techno_monomaniac", "sfx_inline",
                                                    "sfx_aoboshi"}));
        QCOMPARE(alike, (std::set<std::string>{"cute_daruma", "hand_slackside", "techno_monomaniac"}));
        QCOMPARE(fonts::find("hand_slackside")->fallback, std::string("marker_yusei"));  // (a marker's weight beside a marker's kana)
    }

    // What the program holds is native/resources/fonts/FONTS.json, byte for byte (the file a package installs beside the
    // fonts for people to read), and every font it lists is in that folder, of the size it says.
    void embeddedCatalogIsFontsJsonByteForByte() {
        const QString folder = genko::test::repo_root() + QStringLiteral("/native/resources/fonts/");
        QFile file(folder + QStringLiteral("FONTS.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray on_disk = file.readAll();
        const std::string_view held = fonts::catalog_text();
        QCOMPARE(QByteArray(held.data(), static_cast<qsizetype>(held.size())), on_disk);
        for (const fonts::Entry& e : fonts::entries()) {
            const QFileInfo info(folder + QString::fromStdString(e.file));
            QVERIFY2(info.isFile(), e.file.c_str());
            QCOMPARE(info.size(), static_cast<qint64>(e.bytes));
        }
    }

    // The feature is named by each place a book names a face — and only by one of the forty keys there: not by a face of
    // before, the composite of before, a font file's path, a name nobody knows, a key in another case.
    void featureIsNamedOnlyByTheAddedKeys_data() {
        QTest::addColumn<QString>("place");
        QTest::addColumn<QString>("font");
        QTest::addColumn<bool>("named");
        for (const char* place : kPlaces) {
            const auto row = [place](const char* font, bool named) {
                QTest::newRow((std::string(place) + ": " + font).c_str()) << QString::fromUtf8(place) << QString::fromUtf8(font) << named;
            };
            row("antique_genei", true);
            row("dot8", true);
            row("sfx_aoboshi", true);
            row("narrow_lubrifont", true);
            row("gothic", false);
            row("sfx_pop", false);
            row("antique", false);
            row("/usr/share/fonts/truetype/NotoSansCJK-Regular.ttc", false);
            row("C:\\Windows\\Fonts\\meiryo.ttc", false);
            row("gothic_ultra", false);
            row("Dot16", false);
            row("antique_genei.ttf", false);
        }
    }
    void featureIsNamedOnlyByTheAddedKeys() {
        QFETCH(QString, place);
        QFETCH(QString, font);
        QFETCH(bool, named);
        const Document doc = book_naming(place.toStdString(), font.toStdString());
        QCOMPARE(fonts::uses_feature(doc), named);
        QCOMPARE(fonts::named(doc), named ? std::vector<std::string>{font.toStdString()} : std::vector<std::string>{});
        const Json payload = payload_of(doc);
        QCOMPARE(names_feature(payload), named);
        // (the book made here said no feature when it was "read": what it names is news to its reader)
        QCOMPARE(fonts::named_unannounced(doc), fonts::named(doc));
    }

    // Each of the forty names it, each of the six does not; the nombre's font never does (it stays the faces of before:
    // set_meta refuses another), nor do a line's words.
    void everyAddedKeyNamesItAndNothingElseDoes() {
        for (const fonts::Entry& e : fonts::entries()) {
            const Document doc = book_naming("line", e.key);
            QCOMPARE(names_feature(payload_of(doc)), e.added);
        }
        Document doc = blank_book();
        doc.nombre = Json{{"font", "dot16"}, {"position", "bottom_center"}};
        doc.story.push_back(line_of("L1", "dot16 antique_genei"));
        doc.story.back().speaker = "gothic_black";
        doc.title = "mincho_black";
        QVERIFY(!fonts::uses_feature(doc));
        QVERIFY(!names_feature(payload_of(doc)));
    }

    // The feature goes when the last use goes, and the faces named are listed once each, sorted.
    void featureGoesWhenTheLastUseGoes() {
        Document doc = book_naming("line", "klee");
        doc.story.push_back(line_of("L2", "二つ目"));
        doc.story.back().style["font"] = "brush_syuku";
        doc.story.push_back(line_of("L3", "三つ目"));
        doc.story.back().style["font"] = "klee";
        doc.font_path = "antique_genei";
        QCOMPARE(fonts::named(doc), (std::vector<std::string>{"antique_genei", "brush_syuku", "klee"}));
        QVERIFY(names_feature(payload_of(doc)));
        doc.font_path.clear();
        doc.story[0].style.erase("font");
        doc.story[2].style["font"] = "mincho";
        QCOMPARE(fonts::named(doc), std::vector<std::string>{"brush_syuku"});
        QVERIFY(names_feature(payload_of(doc)));
        doc.story[1].style["font"] = nullptr;
        QVERIFY(fonts::named(doc).empty());
        const Json payload = payload_of(doc);
        QVERIFY(!names_feature(payload));
        // (a book that carried the name from its last save loses it with the last use: the list is made, not kept)
        doc.features.emplace_back(fonts::kFontsFeature);
        QVERIFY(!names_feature(payload_of(doc)));
        doc.story[1].style["font"] = "stick";
        QVERIFY(names_feature(payload_of(doc)));
        QVERIFY(fonts::named_unannounced(doc).empty());  // (it said the feature when it was read)
    }

    // Written and read: the feature is in project.json while a face of the forty is named, between the other features in
    // their order; this build knows it; the book opens writable — the reader looks at no font file —, names what it named
    // and is written again to the same bytes.
    void fontsFeatureIsWrittenWhileInUse() {
        QVERIFY(genko::storage::is_known_feature("fonts@1"));
        QVERIFY(genko::storage::is_known_feature(fonts::kFontsFeature));
        QVERIFY(!genko::storage::is_known_feature("fonts@2"));
        QVERIFY(!genko::storage::is_known_feature("fonts"));
        Document doc = book_naming("line", "horror_tegomin");
        doc.story.push_back(line_of("L2", "ルビ"));
        {
            genko::core::RubySpan span;
            span.start = 0;
            span.end = 2;
            span.ruby = "るび";
            span.base = "ルビ";
            span.font = "maru_regular";
            doc.story.back().ruby_spans = std::vector<genko::core::RubySpan>{span};
            doc.story.back().ruby_runs = {Json::array({"ルビ", "るび"})};
        }
        QTemporaryDir dir;
        const std::filesystem::path book = genko::core::path_from_utf8(dir.path().toStdString());
        genko::test::write_project(doc, book / "a");
        const std::string first = genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(book / "a" / "project.json")));
        const Json written = genko::core::parse_python_json(first);
        QCOMPARE(written["features"], Json::parse(R"(["fonts@1", "ruby-layout@1"])"));
        QCOMPARE(written["story"][0]["style"]["font"], Json("horror_tegomin"));

        const genko::storage::LoadResult read = genko::storage::load_document(book / "a");
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(read.report.issues.empty());
        QCOMPARE(fonts::named(read.document), (std::vector<std::string>{"horror_tegomin", "maru_regular"}));
        QVERIFY(fonts::named_unannounced(read.document).empty());
        genko::test::write_project(read.document, book / "b");
        QCOMPARE(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(book / "b" / "project.json"))), first);

        // (the same book with the name taken out of its features — what a build of before F2 would have written for the
        // same words —: read, it names the faces unannounced; saved, it says the feature)
        Json before = written;
        before["features"] = Json::parse(R"(["ruby-layout@1"])");
        const genko::storage::LoadResult old = genko::storage::load_document_payload(before, book / "a");
        QVERIFY2(old.document.read_only_reason.empty(), old.document.read_only_reason.c_str());
        QCOMPARE(fonts::named_unannounced(old.document), (std::vector<std::string>{"horror_tegomin", "maru_regular"}));
        QVERIFY(names_feature(payload_of(old.document)));
    }

    // A book of before is written as it was: the books of the last builds (tests/data/fukidashi) read and written name no
    // fonts@1, nor does a new book, nor one lettered in the seven faces, a font file and a name nobody knows.
    void aBookOfBeforeIsWrittenAsItWas() {
        for (const char* name : {"old_book", "old_book2"}) {
            const std::filesystem::path dir = genko::core::path_from_utf8(genko::test::test_data(QStringLiteral("fukidashi/") + QString::fromLatin1(name)).toStdString());
            const Json file = Json::parse(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(dir / "project.json"))));
            const genko::storage::LoadResult read = genko::storage::load_document(dir);
            QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
            QVERIFY(fonts::named(read.document).empty());
            const Json payload = payload_of(read.document);
            QVERIFY(!names_feature(payload));
            QCOMPARE(payload["font_path"], file["font_path"]);
            for (std::size_t i = 0; i < file["story"].size(); ++i) {
                const Json& was = file["story"][i];
                const Json& now = payload["story"][i];
                QCOMPARE(now.value("style", Json::object()).value("font", Json()), was.value("style", Json::object()).value("font", Json()));
            }
        }
        QVERIFY(!names_feature(payload_of(blank_book())));
        Document doc = blank_book();
        const char* const seven[] = {"antique", "gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"};
        int n = 0;
        for (const char* key : seven) {
            doc.story.push_back(line_of(("L" + std::to_string(++n)).c_str(), "あいう"));
            doc.story.back().style["font"] = key;
        }
        doc.story.push_back(line_of("L8", "あいう"));
        doc.story.back().style["font"] = (genko::test::repo_root() + QStringLiteral("/native/resources/fonts/ZenMaruGothic-Black.ttf")).toStdString();
        doc.story.push_back(line_of("L9", "あいう"));
        doc.story.back().style["font"] = "no_such_face";
        doc.font_path = "mincho";
        const Json payload = payload_of(doc);
        QCOMPARE(payload["features"], Json::array());
        QVERIFY(fonts::named_unannounced(doc).empty());
    }

    // A book a build of before F2 wrote is written back to the very bytes it had: tests/data/fonts/book_before, made by
    // the packaged build of 2026-11-13 (before the added faces, and before fukidashi@1 — it holds no oval and no border of
    // 0, which that feature reads otherwise) with lines in each of the seven faces, a font file that is not there, a name
    // nobody knows, a ruby's own face, words set larger, the book's usual face. Read by this build and written: the same
    // project.json, byte for byte (its SHA-256 in tests/data/MANIFEST.json).
    void aBookOfBeforeIsWrittenToTheSameBytes() {
        const QString fixture = genko::test::test_data(QStringLiteral("fonts/book_before"));
        const std::string was = genko::test::read_bytes(fixture + QStringLiteral("/project.json"));
        const Json manifest = genko::test::read_json(genko::test::test_data(QStringLiteral("MANIFEST.json")));
        QCOMPARE(QCryptographicHash::hash(QByteArray::fromStdString(was), QCryptographicHash::Sha256).toHex().toStdString(),
                 manifest["fonts/book_before/project.json"].get<std::string>());
        const genko::storage::LoadResult read = genko::storage::load_document(genko::core::path_from_utf8(fixture.toStdString()));
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(read.report.issues.empty());
        QCOMPARE(read.document.story.size(), std::size_t(11));
        QVERIFY(fonts::named(read.document).empty());
        QTemporaryDir dir;
        const QString out = dir.path() + QStringLiteral("/書体の本");  // (a folder whose name is not ASCII)
        genko::test::write_project(read.document, genko::core::path_from_utf8(out.toStdString()));
        const std::string now = genko::test::read_bytes(out + QStringLiteral("/project.json"));
        if (now != was) {
            // (where they part, for the one who reads the failure)
            std::size_t at = 0;
            while (at < now.size() && at < was.size() && now[at] == was[at]) ++at;
            QFAIL(qPrintable(QStringLiteral("project.json differs from byte %1: was …%2… now …%3…")
                                 .arg(at)
                                 .arg(QString::fromStdString(was.substr(at > 40 ? at - 40 : 0, 120)))
                                 .arg(QString::fromStdString(now.substr(at > 40 ? at - 40 : 0, 120)))));
        }
    }

    // A book whose face's file is missing opens writable: nothing here has any font file to look at (the folder the
    // files would be in does not exist for this program), every one of the forty is named, and the book is read without a
    // word, saved to the same bytes, and still names them.
    void aBookWhoseFaceFileIsMissingOpensWritable() {
        Document doc = blank_book();
        int n = 0;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added) continue;
            doc.story.push_back(line_of(("L" + std::to_string(++n)).c_str(), "書体"));
            doc.story.back().style["font"] = e.key;
        }
        QCOMPARE(n, 40);
        QTemporaryDir dir;
        const std::filesystem::path book = genko::core::path_from_utf8(dir.path().toStdString());
        genko::test::write_project(doc, book / "a");
        const genko::storage::LoadResult read = genko::storage::load_document(book / "a");
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(read.report.issues.empty());
        QCOMPARE(fonts::named(read.document).size(), std::size_t(40));
        genko::test::write_project(read.document, book / "b");
        QCOMPARE(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(book / "b" / "project.json"))),
                 genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(book / "a" / "project.json"))));
    }
};

QTEST_GUILESS_MAIN(TestFontCatalog)
#include "test_font_catalog.moc"
