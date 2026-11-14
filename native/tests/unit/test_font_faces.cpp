// fonts@1 (書体の追加 F2): the forty added lettering faces as the program draws them (render/font_files, render/text/fonts,
// render/draw, render/text/tategaki), each of the forty in turn.
//
//   - the files: each face opens and is the file the catalogue lists (bytes, SHA-256, family, style, PostScript name);
//     its letters are counted through the font's cmap and are what the catalogue says (kana, JIS level 1 and 2, jōyō);
//   - "the face has this letter" is its cmap's answer (Pillow's look at the letter's box, which the six fonts of before
//     and a person's own font files keep, misreads real letters of several of the forty — measured below);
//   - a face without kanji draws them in its partner; a face without the full-width ！？ draws its own ASCII marks;
//   - set down: a bracket is the font's vertical form or the bracket turned, never the "no glyph" box; the lettering
//     down and across, with ruby, outlined and heavy; the pixel faces at their own sizes;
//   - the pictures are pinned: tests/fixtures/render/font-faces-f2.json holds, for every face, the masks of single
//     letters and the lettering of three lines as THIS BUILD drew them on Linux (FreeType is the copy in the source
//     tree, the same on every system; no outside program is their reference — Python and Pillow are not the reference
//     for anything new). GENKO_FONT_FACES_PIN=<file> writes what this run drew to <file>, to be looked at and committed;
//   - a face whose file is not on the computer: drawn as the same line with no font is, and listed; a file of another
//     size is not used; the bytes read are shared and bounded; no file is read before a face is asked for; the program
//     finds the files in share/genko-studio/fonts beside itself.
//
// GENKO_FONTS_OUT=<folder> also writes the sample sheets there (the pictures a person looks at).

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/font_catalog.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/ruby.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/font_files.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/shaping.hpp"
#include "render/text/tategaki.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
using genko::render::Image;
namespace text = genko::render::text;
namespace files = genko::render::font_files;
namespace fonts = genko::core::fonts;

namespace {

constexpr int kDpi = 150;
// A line that has what goes wrong first in a face: brackets, a long vowel mark, small kana, ！？ together and alone, a
// comma and a full stop, kanji.
const char* const kWords = "「ぎゃーっ！？」\nちょっと、待って！\n漢字です。";

std::string source_fonts() { return (genko::test::repo_root() + QStringLiteral("/native/resources/fonts")).toStdString(); }

void use_source_fonts() {
    files::set_dir_for_testing(source_fonts());
    text::clear_layout_cache();
}

void added_rows() {
    QTest::addColumn<QString>("key");
    for (const fonts::Entry& e : fonts::entries()) {
        if (e.added) QTest::newRow(e.key.c_str()) << QString::fromStdString(e.key);
    }
}

// A line of lettering alone (no balloon) in `font` (empty: none named), 5 mm letters.
genko::core::StoryLine lettering(const std::string& font, bool down = true, const char* words = kWords) {
    genko::core::StoryLine line;
    line.id = "L1";
    line.page_index = genko::core::Num(1);
    line.text = words;
    line.balloon = "none";
    line.wrap = down ? "vertical" : "horizontal";
    line.x_mm = genko::core::Num(30.0);
    line.y_mm = genko::core::Num(30.0);
    line.w_mm = genko::core::Num(down ? 40.0 : 70.0);
    line.h_mm = genko::core::Num(down ? 50.0 : 40.0);
    line.style = Json{{"size_mm", 5.0}};
    if (!font.empty()) line.style["font"] = font;
    return line;
}

// The same with a reading over 漢字 in the face itself, a white outline's width of black around white letters, heavy.
genko::core::StoryLine dressed(const std::string& font, bool ruby = true, bool outline = true, bool heavy = true) {
    genko::core::StoryLine line = lettering(font);
    if (ruby) {
        line.ruby_runs = {Json::array({"漢字", "かんじ"})};
        std::vector<genko::core::RubySpan> spans = genko::core::ruby::from_runs(line.text, line.ruby_runs, false);
        for (genko::core::RubySpan& span : spans) {
            if (!font.empty()) span.font = font;
        }
        line.ruby_spans = std::move(spans);
    }
    if (outline) {
        line.style["outline_mm"] = 0.6;
        line.style["rgb"] = Json::array({255, 255, 255});
        line.style["outline_rgb"] = Json::array({0, 0, 0});
    }
    if (heavy) line.style["weight"] = "heavy";
    return line;
}

std::string sha256(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

bool has_ink(const Image& image) {
    if (image.width() <= 0 || image.height() <= 0) return false;
    return image.getchannel(image.bands() - 1).getbbox().has_value();
}

text::Layout drawn(const genko::core::StoryLine& line, const std::optional<std::string>& usual = std::nullopt) {
    text::clear_layout_cache();
    return text::text_layout(line, kDpi, usual);
}

Json layout_pin(const text::Layout& layout) {
    return Json{{"size", Json::array({layout.image.width(), layout.image.height()})},
                {"em", layout.em},
                {"corner", Json::array({layout.corner_x, layout.corner_y})},
                {"sha256", sha256(layout.image.tobytes())}};
}

// What this build draws of a face, to be pinned: single letters' masks (the font's own letters among a few, at two
// sizes — and at their own sizes, the pixel faces), and three lines' lettering.
Json pins_of(const fonts::Entry& e) {
    Json out = Json::object();
    text::Fonts f;
    Json masks = Json::array();
    std::vector<int> sizes = {20, 48};
    if (e.key == "dot12") sizes = {12, 20, 48};
    if (e.key == "dot8") sizes = {8, 16, 20, 48};
    if (e.key == "dot16") sizes = {16, 20, 48};
    for (const int size : sizes) {
        const genko::render::TrueTypeFont& font = f.truetype(e.key, size);
        for (const char32_t c : std::u32string(U"あアっー漢！？!「。A7")) {
            if (font.glyph_index(c) == 0) continue;  // (drawn in the partner, pinned by the lines below)
            const std::u32string one(1, c);
            const auto [mask, offset] = font.getmask2(one);
            const std::array<int, 4> box = font.getbbox(one);
            masks.push_back(Json{{"text", text::utf8(one)},
                                 {"size", size},
                                 {"bbox", Json::array({box[0], box[1], box[2], box[3]})},
                                 {"mask", Json::array({mask.width(), mask.height()})},
                                 {"offset", Json::array({offset.x, offset.y})},
                                 {"advance", font.getlength(one)},
                                 {"sha256", sha256(mask.tobytes())}});
        }
    }
    out["masks"] = std::move(masks);
    out["down"] = layout_pin(drawn(lettering(e.key)));
    out["across"] = layout_pin(drawn(lettering(e.key, false)));
    out["dressed"] = layout_pin(drawn(dressed(e.key)));
    return out;
}

const Json& pinned() {
    static const Json data = genko::test::read_json(genko::test::repo_root() + QStringLiteral("/native/tests/fixtures/render/font-faces-f2.json"));
    return data;
}

std::u32string set_of(const Json& sets, const char* name) { return text::u32(sets.at(name).get<std::string>()); }

const Json& catalog_font(const std::string& key) {
    for (const Json& f : fonts::catalog()["fonts"]) {
        if (f["key"] == Json(key)) return f;
    }
    static const Json none;
    return none;
}

genko::core::ApplyResult run_ops(const Document& doc, const Json& ops) {
    return genko::core::CommandBus(genko::render::ops_registry()).apply(doc, ops, genko::core::Actor("human:作者"));
}

Document blank_book(int pages = 1) { return genko::core::new_episode("書体", genko::core::Num(1), pages, genko::core::PageSpec::a4_mono()); }

}  // namespace

class TestFontFaces : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { use_source_fonts(); }
    void init() {
        // (a slot that looked elsewhere puts the folder back itself; this is for one that failed half way)
        if (files::dir() != source_fonts()) use_source_fonts();
    }

    // The tables this test reads are the ones their manifests fix: the kanji a face is counted against
    // (tests/data/MANIFEST.json) and the pinned pictures (tests/fixtures/render/MANIFEST.json).
    void tablesAreFixed() {
        const auto hash_of = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return std::string();
            return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        };
        const Json data = genko::test::read_json(genko::test::test_data(QStringLiteral("MANIFEST.json")));
        QCOMPARE(hash_of(genko::test::test_data(QStringLiteral("fonts/kanji-sets.json"))), data["fonts/kanji-sets.json"].get<std::string>());
        const QString fixtures = genko::test::repo_root() + QStringLiteral("/native/tests/fixtures/render/");
        const Json pinned_by = genko::test::read_json(fixtures + QStringLiteral("MANIFEST.json"));
        QCOMPARE(hash_of(fixtures + QStringLiteral("font-faces-f2.json")), pinned_by["font-faces-f2.json"].get<std::string>());
    }

    // --- the files ------------------------------------------------------------------------------------------------------

    // Each face opens, and is the file the catalogue lists: its size, its SHA-256, the family, style and PostScript name
    // the font gives itself. A face opened by its key reads its letters from its cmap; the same file opened by its path
    // — a person's own font file — does not, and draws the same pixels.
    void everyFaceOpensAndIsTheFileFetched_data() { added_rows(); }
    void everyFaceOpensAndIsTheFileFetched() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        QVERIFY(e != nullptr);
        QVERIFY(files::present(e->key));
        QVERIFY(text::face_exists(e->key));
        QCOMPARE(QString::fromStdString(files::path(e->key)), QString::fromStdString(source_fonts() + "/" + e->file));
        const std::shared_ptr<const QByteArray> bytes = files::bytes(e->key);
        QCOMPARE(static_cast<std::int64_t>(bytes->size()), e->bytes);
        QCOMPARE(QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex().toStdString(), e->sha256);
        const text::FontInfo info = text::font_info(e->key);
        QCOMPARE(info.family, e->family);
        QCOMPARE(info.style, e->style);
        QCOMPARE(info.postscript, e->postscript);
        text::Fonts f;
        const genko::render::TrueTypeFont& by_key = f.truetype(e->key, 32);
        QVERIFY(by_key.strict_cmap());
        QVERIFY(by_key.getname().first.has_value());
        QCOMPARE(by_key.glyph_index(0xFFFF), std::uint32_t(0));  // (the letter Pillow asks for a font's "no glyph" box with)
        const genko::render::TrueTypeFont& by_path = f.truetype(files::path(e->key), 32);
        QVERIFY(!by_path.strict_cmap());
        for (const char32_t c : std::u32string(U"あA")) {
            const std::u32string one(1, c);
            QCOMPARE(by_key.getmask2(one).first.tobytes(), by_path.getmask2(one).first.tobytes());
            QCOMPARE(by_key.getbbox(one), by_path.getbbox(one));
            QCOMPARE(by_key.getlength(one), by_path.getlength(one));
        }
        // (the face: one font for everything, its partner where the catalogue names one)
        const text::Face face = text::face_of(Json(e->key));
        QCOMPARE(face.key, e->key);
        QCOMPARE(face.kana, e->key);
        QCOMPARE(face.other, e->key);
        QVERIFY(!face.composite());
        QCOMPARE(face.fallback, e->fallback);
        // (for effects too: the kind's default is only for a line that names no face, or one that is not there)
        QCOMPARE(text::face_of(Json(e->key), text::kDefaultSfx).key, e->key);
    }

    // All forty files at once, as the doctor will look at them: there, of their size, of their SHA-256.
    void verifyFindsEveryFileAsFetched() {
        const std::vector<files::Checked> checked = files::verify(true);
        QCOMPARE(checked.size(), std::size_t(40));
        for (const files::Checked& c : checked) {
            QVERIFY2(c.exists && c.size_ok && c.hash_ok.has_value() && *c.hash_ok, c.key.c_str());
            QCOMPARE(c.file, fonts::find(c.key)->file);
        }
        QVERIFY(files::missing().empty());
        for (const files::Checked& c : files::verify(false)) QVERIFY(c.exists && c.size_ok && !c.hash_ok.has_value());
    }

    // The letters each face has, counted through the font's cmap as the program reads it, are what the catalogue says:
    // the kana (and which are missing), the punctuation, the ASCII letters and digits, the kanji of JIS level 1 and 2 and
    // the jōyō kanji (tests/data/fonts/kanji-sets.json), and so its kind: full, partial or none.
    void coverageIsAsTheCatalogSays_data() { added_rows(); }
    void coverageIsAsTheCatalogSays() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        const Json& listed = catalog_font(e->key);
        static const Json sets = genko::test::read_json(genko::test::test_data(QStringLiteral("fonts/kanji-sets.json")));
        text::Fonts f;
        const genko::render::TrueTypeFont& font = f.truetype(e->key, 32);
        const auto has = [&](char32_t c) { return font.glyph_index(c) != 0; };
        const auto missing_of = [&](const std::u32string& letters) {
            std::u32string out;
            for (const char32_t c : letters) {
                if (!has(c)) out.push_back(c);
            }
            return text::utf8(out);
        };
        std::u32string kana, core_kana;
        for (char32_t c = 0x3041; c <= 0x3096; ++c) kana.push_back(c);
        for (char32_t c = 0x30A1; c <= 0x30FA; ++c) kana.push_back(c);
        kana += U"ー゛゜";
        for (char32_t c = 0x3041; c <= 0x3094; ++c) core_kana.push_back(c);
        for (char32_t c = 0x30A1; c <= 0x30F6; ++c) core_kana.push_back(c);
        core_kana += U"ー゛゜";
        QCOMPARE(missing_of(kana), listed["kana_missing"].get<std::string>());
        QCOMPARE(missing_of(core_kana).empty(), listed["kana_complete"].get<bool>());
        QCOMPARE(missing_of(U"、。「」『』！？…―～・"), listed["punctuation_missing"].get<std::string>());
        std::u32string ascii;
        for (char32_t c = U'0'; c <= U'9'; ++c) ascii.push_back(c);
        for (char32_t c = U'A'; c <= U'Z'; ++c) ascii.push_back(c);
        for (char32_t c = U'a'; c <= U'z'; ++c) ascii.push_back(c);
        QCOMPARE(missing_of(ascii).empty(), listed["ascii_complete"].get<bool>());
        const auto count = [&](const std::u32string& letters) {
            return static_cast<std::int64_t>(std::count_if(letters.begin(), letters.end(), has));
        };
        const std::u32string level1 = set_of(sets, "jis_level1"), level2 = set_of(sets, "jis_level2"), joyo = set_of(sets, "joyo");
        QCOMPARE(level1.size(), std::size_t(2965));
        QCOMPARE(level2.size(), std::size_t(3390));
        QCOMPARE(joyo.size(), std::size_t(2136));
        std::map<char32_t, char32_t> usual;
        for (const auto& [official, form] : sets["joyo_usual_form"].items()) usual[text::u32(official)[0]] = text::u32(form.get<std::string>())[0];
        QCOMPARE(usual.size(), std::size_t(4));
        std::int64_t joyo_has = 0;
        for (const char32_t c : joyo) joyo_has += has(c) || (usual.contains(c) && has(usual[c])) ? 1 : 0;
        const std::int64_t n1 = count(level1), n2 = count(level2);
        QCOMPARE(n1, listed["kanji_jis_level1"].get<std::int64_t>());
        QCOMPARE(n2, listed["kanji_jis_level2"].get<std::int64_t>());
        QCOMPARE(joyo_has, listed["kanji_joyo"].get<std::int64_t>());
        const std::string kind = n1 == 2965 && joyo_has == 2136 && n2 * 100 >= 3390 * 99 ? "full" : n1 == 0 && n2 == 0 ? "none" : "partial";
        QCOMPARE(kind, e->kanji);
        // (every face draws the ASCII ! and ?: two or three ！？ in a row are set across in one cell with them)
        QVERIFY(has(U'!') && has(U'?') && has(U'0'));
    }

    // "The face has this letter" is its cmap's answer, for every letter of the kana, the punctuation, ASCII, some symbols
    // and both levels of kanji. Pillow's way — the letter's box against the box of the "no glyph" letter —, which the
    // fonts of before keep, would misread real letters of these faces: how many is pinned here (measured in this build),
    // 源暎きわみゴ and Yusei Magic by the hundred (their letters fill the same box as their "no glyph" box does). The
    // pixel faces, where the plan feared it, are not among them.
    void hasGlyphFollowsTheCmap_data() { added_rows(); }
    void hasGlyphFollowsTheCmap() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        static const Json sets = genko::test::read_json(genko::test::test_data(QStringLiteral("fonts/kanji-sets.json")));
        std::u32string all;
        for (char32_t c = 0x3041; c <= 0x3096; ++c) all.push_back(c);
        for (char32_t c = 0x30A1; c <= 0x30FA; ++c) all.push_back(c);
        all += U"ー゛゜、。「」『』！？…―～・〜—‥々〆（）〈〉【】［］《》♪♡♥★☆※→←↑↓■□◆◇○●◎△▲▽▼─━│┃　";
        for (char32_t c = 0x20; c <= 0x7e; ++c) all.push_back(c);
        all += set_of(sets, "jis_level1") + set_of(sets, "jis_level2");
        text::Fonts f;
        const genko::render::TrueTypeFont& font = f.truetype(e->key, 32);
        const auto no_glyph_box = font.getmask2(std::u32string(1, char32_t{0xFFFF})).first.getbbox();
        int misread = 0, has = 0;
        for (const char32_t c : all) {
            const std::u32string one(1, c);
            const bool cmap = font.glyph_index(c) != 0;
            has += cmap ? 1 : 0;
            if (f.has_glyph(e->key, one) != cmap) QFAIL(qPrintable(QStringLiteral("has_glyph is not the cmap's answer for U+%1").arg(static_cast<uint>(c), 4, 16, QLatin1Char('0'))));
            // (Pillow's way, as Fonts::has_glyph does it for a font that is not one of the forty)
            const auto box = font.getmask2(one).first.getbbox();
            const bool looked = !box ? text::py_isspace(c) : box != no_glyph_box;
            if (cmap && !looked) ++misread;
            if (!cmap && looked) QFAIL("a letter the cmap lacks is not drawn as the font's no-glyph box");
        }
        QVERIFY(has > 280);
        // (a letter with its variation selector is the letter's; no letter at all is none)
        QVERIFY(f.has_glyph(e->key, std::u32string(U"あ︀")));
        QVERIFY(!f.has_glyph(e->key, std::u32string()));
        QVERIFY(!f.has_glyph(e->key, std::u32string(1, char32_t{0xFFFF})));
        static const std::map<std::string, int> kMisread = {{"antique_shippori", 1}, {"mincho_shippori", 1}, {"gothic_kiwami", 1074}, {"pop_mochiy", 1},
                                                            {"cute_kiwi", 1},        {"marker_yusei", 652},  {"hand_slackside", 6},    {"techno_monomaniac", 8},
                                                            {"sfx_outline", 6},      {"sfx_aoboshi", 1}};
        const auto expected = kMisread.find(e->key);
        QCOMPARE(misread, expected == kMisread.end() ? 0 : expected->second);
        // (the same file by its path is a person's font file: read Pillow's way, as before — the reason for the rule)
        if (e->key == "gothic_kiwami") {
            const std::string path = files::path(e->key);
            QVERIFY(f.has_glyph(e->key, U"か"));
            QVERIFY(!f.has_glyph(path, U"か"));
            QVERIFY(f.truetype(path, 32).glyph_index(U'か') != 0);
        }
    }

    // The fonts of before are as they were: the seven faces' halves, no partner, their letters read Pillow's way (the
    // bundled effects face still "lacks" its ろ, whose ink fills the box of its "no glyph" box), no swap of ！？.
    void theFacesOfBeforeAreAsTheyWere() {
        const struct {
            const char* key;
            const char* kana;
            const char* other;
        } seven[] = {{"antique", "mincho", "gothic"}, {"gothic", "gothic", "gothic"}, {"mincho", "mincho", "mincho"}, {"maru", "maru", "maru"},
                     {"hand", "hand", "hand"},        {"sfx", "sfx", "sfx"},          {"sfx_pop", "sfx_pop", "sfx_pop"}};
        text::Fonts f;
        for (const auto& s : seven) {
            const text::Face face = text::face_of(Json(s.key));
            QCOMPARE(face.key, std::string(s.key));
            QCOMPARE(face.kana, std::string(s.kana));
            QCOMPARE(face.other, std::string(s.other));
            QVERIFY(face.fallback.empty());
            QVERIFY(text::face_exists(s.key));
            QVERIFY(!f.truetype(face.kana, 32).strict_cmap());
            QVERIFY(!f.truetype(face.other, 32).strict_cmap());
            QCOMPARE(f.normalize(face, U"なに！？"), std::u32string(U"なに！？"));
        }
        QCOMPARE(text::face_of(Json()).key, std::string("antique"));
        QCOMPARE(text::face_of(Json("no_such_face")).key, std::string("antique"));
        QCOMPARE(text::face_of(Json("no_such_face"), text::kDefaultSfx).key, std::string("sfx"));
        QVERIFY(!text::face_exists("no_such_face"));
        QVERIFY(f.truetype("sfx", 32).glyph_index(U'ろ') != 0);
        QVERIFY(!f.has_glyph("sfx", U"ろ"));
        // (the fallback of before, in its order: the other half, the bundled Gothic, the effects face)
        const text::Face antique = text::face_of(Json("antique"));
        QCOMPARE(f.path_for(antique, U"あ"), std::string("mincho"));
        QCOMPARE(f.path_for(antique, U"漢"), std::string("gothic"));
        // (a font file by its path: one font, no partner, no look-alikes of its own — a kana-only font's kanji go to the
        // bundled Gothic as they always did, its ！？ too)
        const std::string daruma = source_fonts() + "/DarumadropOne-Regular.ttf";
        const text::Face file = text::face_of(Json(daruma));
        QCOMPARE(file.key, daruma);
        QVERIFY(file.fallback.empty());
        QCOMPARE(f.normalize(file, U"なに！？"), std::u32string(U"なに！？"));
        QCOMPARE(f.path_for(file, U"漢"), std::string("gothic"));
    }

    // --- partners and look-alikes -------------------------------------------------------------------------------------

    // A face without kanji draws its kana itself and its kanji in its partner (a face with some kanji: those it lacks),
    // before the bundled Gothic is thought of; the partner's letters are the face's own, not stand-ins. A face with all
    // the kanji has no partner and draws them itself.
    void kanjiOfAKanaFaceIsDrawnInItsPartner_data() { added_rows(); }
    void kanjiOfAKanaFaceIsDrawnInItsPartner() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        text::Fonts f;
        const text::Face face = text::face_of(Json(e->key));
        QCOMPARE(f.path_for(face, U"あ"), e->key);
        QCOMPARE(f.path_for(face, U"ア"), e->key);
        QCOMPARE(f.path_for(face, U"A"), e->key);
        QVERIFY(face.own(e->key));
        QVERIFY(!face.own("gothic") || e->fallback == "gothic");
        if (e->fallback.empty()) {
            QCOMPARE(e->kanji, std::string("full"));
            QCOMPARE(f.path_for(face, U"漢"), e->key);
            QCOMPARE(f.path_for(face, U"鬱"), e->key);
            return;
        }
        QCOMPARE(face.fallback, e->fallback);
        QVERIFY(face.own(e->fallback));
        QCOMPARE(f.path_for(face, U"鬱"), e->fallback);  // (level 2: no partial face has it)
        QCOMPARE(f.path_for(face, U"漢"), e->kanji == "partial" ? e->key : e->fallback);
        // (a letter neither has goes on to the bundled Gothic, as before: ㋿ is in none of them, nor in the Gothic — the
        // face's own font is named, and draws its "no glyph" box)
        QCOMPARE(f.path_for(face, U"\U0001F600"), e->key);
        // (said of a ruby in that face: nothing — its kanji in the partner are not a stand-in, and no letter is missing)
        genko::core::StoryLine line = lettering("mincho");
        line.ruby_runs = {Json::array({"漢字", "漢じ"})};
        std::vector<genko::core::RubySpan> spans = genko::core::ruby::from_runs(line.text, line.ruby_runs, false);
        QCOMPARE(spans.size(), std::size_t(1));
        spans[0].font = e->key;
        line.ruby_spans = spans;
        text::clear_layout_cache();
        // (a partial face draws 漢 itself; either way nothing is said)
        for (const text::RubyNote& note : text::ruby_notes(line, kDpi)) QVERIFY2(note.kind != "font_fallback" && note.kind != "glyph_missing", note.kind.c_str());
        // (and a ruby in a face of before whose letter only another font has is still said, as it always was)
        spans[0].font = "hand";
        spans[0].ruby = "か♨";
        line.ruby_spans = spans;
        text::clear_layout_cache();
        bool said = false;
        for (const text::RubyNote& note : text::ruby_notes(line, kDpi)) said = said || note.kind == "font_fallback" || note.kind == "glyph_missing";
        QVERIFY(said);
    }

    // ！ and ？ become the face's own ASCII marks in the three faces drawn without the full-width ones, and nowhere else;
    // the look-alikes of before (― → —, ～ → 〜 …) work in the added faces as in the others.
    void bangsBecomeAsciiOnlyWhereTheFaceSaysSo_data() { added_rows(); }
    void bangsBecomeAsciiOnlyWhereTheFaceSaysSo() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        text::Fonts f;
        const text::Face face = text::face_of(Json(e->key));
        const genko::render::TrueTypeFont& font = f.truetype(e->key, 32);
        const bool swaps = e->key == "cute_daruma" || e->key == "hand_slackside" || e->key == "techno_monomaniac";
        QCOMPARE(!e->lookalike.empty(), swaps);
        QCOMPARE(font.glyph_index(U'！') == 0 && font.glyph_index(U'？') == 0, swaps);
        QCOMPARE(f.normalize(face, U"え！なに？"), swaps ? std::u32string(U"え!なに?") : std::u32string(U"え！なに？"));
        // (so the marks are the face's own, set upright in their cell like any letter)
        if (swaps) {
            QCOMPARE(f.path_for(face, U"!"), e->key);
            const Image mark = text::glyph(U"!", font, 32, {0, 0, 0});
            QCOMPARE(mark.size(), (genko::render::Size{32, 32}));
            QVERIFY(has_ink(mark));
        }
        // (LOOKALIKE: a dash or wave the face lacks swapped for the one it has, when it has it)
        const std::u32string given = U"―～〜";
        const std::u32string set = f.normalize(face, given);
        QCOMPARE(set.size(), given.size());
        for (std::size_t i = 0; i < given.size(); ++i) {
            const bool had = font.glyph_index(given[i]) != 0;
            if (had) QCOMPARE(set[i], given[i]);
            else if (set[i] != given[i]) QVERIFY(font.glyph_index(set[i]) != 0);
        }
    }

    // --- set down and across ------------------------------------------------------------------------------------------

    // A bracket set down is the font's own vertical form where its cmap has it, and the bracket turned a quarter where it
    // has not — never the font's "no glyph" box, which is what six of these faces would have drawn (they lack some or all
    // of the forms, and a "no glyph" box is larger than a speck): all 16 brackets, in the font each is drawn in. The
    // catalogue says which faces have every form.
    void bracketsDownAreAFormOrTurnedNeverTofu_data() { added_rows(); }
    void bracketsDownAreAFormOrTurnedNeverTofu() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        const std::pair<char32_t, char32_t> forms[] = {{U'「', U'﹁'}, {U'」', U'﹂'}, {U'『', U'﹃'}, {U'』', U'﹄'}, {U'（', U'︵'}, {U'）', U'︶'},
                                                        {U'(', U'︵'},  {U')', U'︶'},  {U'【', U'︻'}, {U'】', U'︼'}, {U'［', U'﹇'}, {U'］', U'﹈'},
                                                        {U'〈', U'︿'}, {U'〉', U'﹀'}, {U'《', U'︽'}, {U'》', U'︾'}};
        text::Fonts f;
        const text::Face face = text::face_of(Json(e->key));
        const genko::render::TrueTypeFont& own = f.truetype(e->key, 48);
        bool every_form = true;
        for (const auto& [bracket, form] : forms) every_form = every_form && own.glyph_index(form) != 0;
        QCOMPARE(every_form, e->presentation_forms);
        static const std::set<std::string> kTurned = {"cute_daruma", "hand_slackside", "techno_monomaniac", "dot12", "dot8", "sfx_aoboshi"};
        QCOMPARE(!every_form, kTurned.contains(e->key));
        const text::Rgb black{0, 0, 0};
        for (const auto& [bracket, form] : forms) {
            const std::u32string one(1, bracket);
            const std::string used = f.path_for(face, one);
            QVERIFY2(face.own(used), used.c_str());  // (a bracket the face lacks is its partner's, never a stranger's)
            const genko::render::TrueTypeFont& font = f.truetype(used, 48);
            QVERIFY(font.glyph_index(bracket) != 0);
            const Image set = text::glyph(one, font, 48, black);
            QCOMPARE(set.size(), (genko::render::Size{48, 48}));
            QVERIFY(has_ink(set));
            // the "no glyph" box of that font, as a letter and turned as a long mark is: the bracket is neither
            const Image tofu = text::glyph(std::u32string(1, char32_t{0xFFFF}), font, 48, black);
            QVERIFY(set.tobytes() != tofu.tobytes());
            QVERIFY(set.tobytes() != tofu.rotate(-90, genko::render::Resample::Bicubic, false).tobytes());
            if (font.glyph_index(form) != 0) {
                // (the form, set in the em square as any letter)
                QCOMPARE(set.tobytes(), text::glyph(std::u32string(1, form), font, 48, black).tobytes());
            } else {
                // (turned: not the bracket as it is written across)
                QVERIFY(font.strict_cmap());
                const Image across = text::glyph(U"あ", font, 48, black);  // (a letter set upright, for the canvas's size)
                QCOMPARE(across.size(), set.size());
                const Image upright = text::tcy_glyph(one, font, 48, black);
                QVERIFY(set.tobytes() != upright.tobytes());
            }
        }
    }

    // The lettering of a line down and across in each face: something is drawn, the same when drawn again, not what the
    // default face draws, and every letter in a font of the face (its own, its partner's) but the marks only another has.
    void composedDownAndAcross_data() { added_rows(); }
    void composedDownAndAcross() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        for (const bool down : {true, false}) {
            const genko::core::StoryLine line = lettering(e->key, down);
            const text::Layout first = drawn(line);
            QVERIFY(has_ink(first.image));
            QVERIFY(first.em > 0);
            const text::Layout again = drawn(line);
            QCOMPARE(again.image.size(), first.image.size());
            QCOMPARE(again.image.tobytes(), first.image.tobytes());
            QCOMPARE(again.em, first.em);
            const text::Layout usual = drawn(lettering("", down));
            QVERIFY(usual.image.size() != first.image.size() || usual.image.tobytes() != first.image.tobytes());
            // (the face the line is set in, and that it was found)
            const auto [face, missing] = text::line_face(line);
            QCOMPARE(face.key, e->key);
            QVERIFY(!missing);
            // (every cell's font is the face's own: the line has no letter that neither the face nor its partner has)
            const text::SetLayout set = text::set_layout(line, kDpi, std::nullopt, text::LetterParts{});
            std::size_t cells = 0;
            for (const text::SetRow& row : set.set.rows) {
                for (const text::SetChar& c : row.chars) {
                    QVERIFY2(face.own(c.font), (text::utf8(c.text) + " in " + c.font).c_str());
                    ++cells;
                }
            }
            QVERIFY(cells >= 15);
        }
    }

    // The OpenType typesetting (a record of type@1 that asks for it) set down takes the font's vertical alternate where
    // the font has the feature: the glyph for 「 is not the cmap's in 38 of the faces; k8x12 and Misaki have no such
    // feature and keep the cmap's (and nothing is thrown); every face advances down — Darumadrop, which has the feature
    // but no vertical metrics, too. Glyph ids and signs only: HarfBuzz is the system's.
    void shapedDownTakesVertWhereTheFontHasIt_data() { added_rows(); }
    void shapedDownTakesVertWhereTheFontHasIt() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        text::Fonts f;
        const genko::render::TrueTypeFont& font = f.truetype(e->key, 32);
        text::ShapeRequest request;
        request.text = "「";
        request.font = e->key;
        request.size_px = 32;
        request.direction = "ttb";
        request.features = {{"vert", 1}, {"vrt2", 1}};
        const std::vector<text::ShapedGlyph> glyphs = text::shape(request);
        QCOMPARE(glyphs.size(), std::size_t(1));
        QVERIFY(glyphs[0].glyph != 0);
        const bool plain = e->key == "dot12" || e->key == "dot8";
        QCOMPARE(glyphs[0].glyph == font.glyph_index(U'「'), plain);
        QVERIFY(glyphs[0].y_advance < 0);
        const Json& vertical = catalog_font(e->key)["vertical"];
        QCOMPARE(vertical["gsub_vert"].get<bool>(), !plain);
        QCOMPARE(e->vertical, e->key != "dot12" && e->key != "dot8" && e->key != "cute_daruma");
        // (the features the font says it has, as the 文字 panel lists them)
        const std::vector<std::string> features = text::font_info(e->key).features;
        QCOMPARE(std::find(features.begin(), features.end(), std::string("vert")) != features.end(), !plain);
        // (and across, the cmap's own glyph)
        request.direction = "ltr";
        request.features.clear();
        const std::vector<text::ShapedGlyph> across = text::shape(request);
        QCOMPARE(across.size(), std::size_t(1));
        QCOMPARE(across[0].glyph, font.glyph_index(U'「'));
        QVERIFY(across[0].x_advance > 0);
    }

    // With a reading in the face itself, outlined and heavy: each is drawn, changes the lettering, and nothing is said
    // of the reading (its kana are the face's own); the reading is set in the face.
    void rubyOutlineAndHeavyAreDrawn_data() { added_rows(); }
    void rubyOutlineAndHeavyAreDrawn() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        const text::Layout plain = drawn(dressed(e->key, false, false, false));
        QVERIFY(has_ink(plain.image));
        const genko::core::StoryLine with_ruby = dressed(e->key, true, false, false);
        const text::Layout ruby = drawn(with_ruby);
        QVERIFY(has_ink(ruby.image));
        QVERIFY(ruby.image.width() > plain.image.width());  // (the reading's lane beside the columns)
        text::clear_layout_cache();
        QVERIFY(text::ruby_notes(with_ruby, kDpi).empty());
        const text::RubyPlaces places = text::ruby_places(with_ruby, kDpi);
        QCOMPARE(places.places.size(), std::size_t(1));
        QCOMPARE(places.places[0].reading, std::string("かんじ"));
        QCOMPARE(places.places[0].face.key, e->key);
        QVERIFY(places.places[0].vertical);
        const text::Layout outline = drawn(dressed(e->key, false, true, false));
        QVERIFY(has_ink(outline.image));
        QVERIFY(outline.image.width() > plain.image.width() && outline.image.height() > plain.image.height());  // (the outline's room)
        const text::Layout heavy = drawn(dressed(e->key, false, false, true));
        QVERIFY(has_ink(heavy.image));
        QVERIFY(heavy.image.size() != plain.image.size() || heavy.image.tobytes() != plain.image.tobytes());
        // (heavier: more ink than the plain line has)
        const auto ink = [](const Image& image) {
            std::int64_t sum = 0;
            for (const char v : image.getchannel(image.bands() - 1).tobytes()) sum += static_cast<unsigned char>(v);
            return sum;
        };
        QVERIFY(ink(heavy.image) > ink(plain.image));
        const text::Layout all = drawn(dressed(e->key));
        QVERIFY(has_ink(all.image));
        // (a reading named in the face over a line of the default face, and the other way round)
        genko::core::StoryLine mixed = dressed("", true, false, false);
        (*mixed.ruby_spans)[0].font = e->key;
        const text::Layout over_default = drawn(mixed);
        const text::Layout default_ruby = drawn(dressed("", true, false, false));
        QCOMPARE(over_default.image.size(), default_ruby.image.size());
        QVERIFY(over_default.image.tobytes() != default_ruby.image.tobytes());
        text::clear_layout_cache();
        QVERIFY(text::ruby_notes(mixed, kDpi).empty());
    }

    // The pixel faces at the sizes they were drawn for use the bitmaps the fonts carry: every pixel of a letter is ink
    // or paper, nothing between (k8x12 at 12 px, Misaki at 8 px); at other sizes their outlines are drawn smooth.
    void pixelFacesAtTheirStrikes() {
        text::Fonts f;
        const auto levels = [&](const char* key, int size) {
            std::set<int> seen;
            for (const char32_t c : std::u32string(U"あ漢Aア！")) {
                const Image mask = f.truetype(key, size).getmask2(std::u32string(1, c)).first;
                if (mask.width() > size || !mask.getbbox()) return std::set<int>{-1};
                for (const char v : mask.tobytes()) seen.insert(static_cast<unsigned char>(v));
            }
            return seen;
        };
        QCOMPARE(levels("dot12", 12), (std::set<int>{0, 255}));
        QCOMPARE(levels("dot8", 8), (std::set<int>{0, 255}));
        // (a letter of k8x12 — 8 × 12 — is 8 px wide at 12 px, one of Misaki 8 at 8, one of DotGothic16 16 at 16: their cells)
        QCOMPARE(f.truetype("dot12", 12).getlength(U"あ"), 8.0);
        QCOMPARE(f.truetype("dot8", 8).getlength(U"あ"), 8.0);
        QCOMPARE(f.truetype("dot16", 16).getlength(U"あ"), 16.0);
        // (set in a line at a size that is none of theirs they are drawn all the same)
        for (const char* key : {"dot12", "dot8", "dot16"}) {
            for (const int size : {7, 13, 31, 100}) {
                const Image mask = f.truetype(key, size).getmask2(U"あ漢").first;
                QVERIFY2(mask.getbbox().has_value(), key);
            }
        }
        // (the outline — 白フチ — and the heavy weight are drawn from the outlines, never the bitmaps: something wider)
        const auto ink = [](const Image& mask) {
            std::int64_t sum = 0;
            for (const char v : mask.tobytes()) sum += static_cast<unsigned char>(v);
            return sum;
        };
        for (const auto& [key, size] : {std::pair<const char*, int>{"dot12", 12}, {"dot8", 8}}) {
            const Image plain = f.truetype(key, size).getmask2(U"あ").first;
            const Image stroked = f.truetype(key, size).getmask2(U"あ", 1).first;
            QVERIFY2(stroked.getbbox().has_value() && ink(stroked) > ink(plain), key);
        }
    }

    // --- the pictures, pinned --------------------------------------------------------------------------------------------

    // Every face's letters and lines are what this build drew when they were pinned (font-faces-f2.json: made by this
    // test on Linux, GENKO_FONT_FACES_PIN; exact — the masks' and the letterings' SHA-256, their boxes, sizes and
    // advances).
    void pinnedFaces_data() { added_rows(); }
    void pinnedFaces() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        const Json& all = pinned();
        QCOMPARE(all["schema"], Json("genko-font-faces@1"));
        QCOMPARE(all["faces"].size(), std::size_t(40));
        QCOMPARE(all["words"], Json(kWords));
        QCOMPARE(all["dpi"], Json(kDpi));
        QVERIFY2(all["faces"].contains(e->key), e->key.c_str());
        const Json now = pins_of(*e);
        const Json& was = all["faces"][e->key];
        QVERIFY(now["masks"].size() >= 16);
        QCOMPARE(now["masks"].size(), was["masks"].size());
        for (std::size_t i = 0; i < now["masks"].size(); ++i) {
            std::string where;
            if (!genko::test::strict_equal(now["masks"][i], was["masks"][i], &where)) {
                QFAIL(qPrintable(QStringLiteral("mask %1 (%2 at %3 px) differs: %4")
                                     .arg(i)
                                     .arg(QString::fromStdString(now["masks"][i]["text"].get<std::string>()))
                                     .arg(now["masks"][i]["size"].get<int>())
                                     .arg(QString::fromStdString(where))));
            }
        }
        for (const char* name : {"down", "across", "dressed"}) {
            std::string where;
            QVERIFY2(genko::test::strict_equal(now[name], was[name], &where), (std::string(name) + ": " + where).c_str());
        }
    }

    // --- a face whose file is not there -------------------------------------------------------------------------------

    // With no font file on the computer every one of the forty is missing and listed; a line that names one is drawn
    // exactly as the same line with no font is — the default face for a line, the effects face for a sound effect —,
    // is said to name a font that was not found, and a reading in it is set in the line's face and said too. The book
    // is not touched: the op that asks what is missing lists the face, with how many texts name it.
    void aMissingFileIsDrawnAsTheDefaultFaceAndListed_data() { added_rows(); }
    void aMissingFileIsDrawnAsTheDefaultFaceAndListed() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        [&] {
            QCOMPARE(QString::fromStdString(files::dir()), empty.path());
            QVERIFY(!files::present(e->key));
            QVERIFY(!text::face_exists(e->key));
            QCOMPARE(files::missing().size(), std::size_t(40));
            QCOMPARE(text::face_of(Json(e->key)).key, std::string("antique"));
            QCOMPARE(text::face_of(Json(e->key), text::kDefaultSfx).key, std::string("sfx"));
            for (const bool down : {true, false}) {
                const genko::core::StoryLine named = lettering(e->key, down);
                const text::Layout a = drawn(named);
                const text::Layout b = drawn(lettering("", down));
                QCOMPARE(a.image.size(), b.image.size());
                QCOMPARE(a.image.tobytes(), b.image.tobytes());
                QCOMPARE(a.em, b.em);
                const auto [face, missing] = text::line_face(named);
                QCOMPARE(face.key, std::string("antique"));
                QVERIFY(missing);
            }
            genko::core::StoryLine effect = lettering(e->key);
            effect.balloon = "sfx";
            genko::core::StoryLine plain_effect = lettering("");
            plain_effect.balloon = "sfx";
            QCOMPARE(drawn(effect).image.tobytes(), drawn(plain_effect).image.tobytes());
            QCOMPARE(text::line_face(effect).first.key, std::string("sfx"));
            // (the book's usual face named by a key that is not there: the default, as a path that is not there)
            QCOMPARE(drawn(lettering(""), e->key).image.tobytes(), drawn(lettering("")).image.tobytes());
            // (a reading in the missing face: the line's face, said)
            genko::core::StoryLine ruby = dressed("", true, false, false);
            (*ruby.ruby_spans)[0].font = e->key;
            QCOMPARE(drawn(ruby).image.tobytes(), drawn(dressed("", true, false, false)).image.tobytes());
            text::clear_layout_cache();
            const std::vector<text::RubyNote> notes = text::ruby_notes(ruby, kDpi);
            QCOMPARE(notes.size(), std::size_t(1));
            QCOMPARE(notes[0].kind, std::string("font_fallback"));
            QCOMPARE(notes[0].detail, "font:" + e->key);
            // (listed by 見つからないフォントを解決: a question, the book as it was)
            Document doc = blank_book();
            doc.story.push_back(lettering(e->key));
            const genko::core::ApplyResult asked = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
            QVERIFY(asked.unchanged);
            QCOMPARE(asked.results.size(), std::size_t(1));
            QCOMPARE(asked.results[0]["missing"], Json::array({Json{{"font", e->key}, {"texts", 1}}}));
            QCOMPARE(fonts::named(asked.doc), std::vector<std::string>{e->key});
        }();
        use_source_fonts();
        // (and with the files back the same book names nothing missing)
        Document doc = blank_book();
        doc.story.push_back(lettering(e->key));
        QCOMPARE(run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}})).results[0]["missing"], Json::array());
    }

    // A face whose own file is there but whose partner's is not: its kana are its own, and its kanji go to the bundled
    // Gothic as any face's missing letters do — never to a font that cannot be read.
    void aMissingPartnerIsLeftOut() {
        QTemporaryDir some;
        for (const char* file : {"CherryBombOne-Regular.ttf", "SlacksideOne-Regular.ttf", "YuseiMagic-Regular.ttf"}) {
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/" + file, some.path() + "/" + file));
        }
        files::set_dir_for_testing(some.path().toStdString());
        text::clear_layout_cache();
        [&] {
            QCOMPARE(files::missing().size(), std::size_t(37));
            text::Fonts f;
            const text::Face cherry = text::face_of(Json("cute_cherry"));  // (partner: maru_black, not there)
            QCOMPARE(cherry.key, std::string("cute_cherry"));
            QVERIFY(cherry.fallback.empty());
            QCOMPARE(f.path_for(cherry, U"あ"), std::string("cute_cherry"));
            QCOMPARE(f.path_for(cherry, U"漢"), std::string("gothic"));
            QVERIFY(has_ink(drawn(lettering("cute_cherry")).image));
            const text::Face slackside = text::face_of(Json("hand_slackside"));  // (partner: marker_yusei, there)
            QCOMPARE(slackside.fallback, std::string("marker_yusei"));
            QCOMPARE(f.path_for(slackside, U"漢"), std::string("marker_yusei"));
            QVERIFY(!text::face_exists("maru_black"));
        }();
        use_source_fonts();
    }

    // A file of the face's name that is not of the catalogue's size is not the face's file: not used, the line drawn in
    // the default face; nor is a folder of that name. A file named as the key itself is never read as a font file.
    void aFileOfAnotherSizeIsNotUsed() {
        QTemporaryDir other;
        const QString klee = QString::fromStdString(fonts::find("klee")->file);
        // (another font under Klee's name; a cut copy under Yuji Syuku's; a folder under Stick's)
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/Stick-Regular.ttf", other.path() + "/" + klee));
        {
            QFile whole(QString::fromStdString(source_fonts()) + "/YujiSyuku-Regular.ttf");
            QVERIFY(whole.open(QIODevice::ReadOnly));
            QFile cut(other.path() + "/YujiSyuku-Regular.ttf");
            QVERIFY(cut.open(QIODevice::WriteOnly));
            cut.write(whole.read(1024 * 1024));
        }
        QVERIFY(QDir(other.path()).mkdir(QStringLiteral("Stick-Regular.ttf")));
        files::set_dir_for_testing(other.path().toStdString());
        text::clear_layout_cache();
        [&] {
            for (const char* key : {"klee", "brush_syuku", "stick"}) {
                QVERIFY2(!files::present(key), key);
                QVERIFY(!text::face_exists(key));
                QCOMPARE(drawn(lettering(key)).image.tobytes(), drawn(lettering("")).image.tobytes());
                QVERIFY_THROWS_EXCEPTION(genko::render::NotYetPorted, files::bytes(key));
            }
            for (const files::Checked& c : files::verify(true)) {
                if (c.key == "klee" || c.key == "brush_syuku") QVERIFY(c.exists && !c.size_ok && !c.hash_ok.has_value());
                else QVERIFY2(!c.exists && !c.size_ok, c.key.c_str());
            }
        }();
        // (a key is a key: with a font file of that very name in the working folder, the face is still the catalogue's)
        const QString was = QDir::currentPath();
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/Stick-Regular.ttf", other.path() + "/dot16"));
        QVERIFY(QDir::setCurrent(other.path()));
        [&] {
            QVERIFY(QFileInfo(QStringLiteral("dot16")).isFile());
            QVERIFY(!text::face_exists("dot16"));
            QCOMPARE(text::face_of(Json("dot16")).key, std::string("antique"));
            use_source_fonts();
            const text::Face face = text::face_of(Json("dot16"));
            QCOMPARE(face.kana, std::string("dot16"));
            text::Fonts f;
            QCOMPARE(f.truetype("dot16", 16).getlength(U"あ"), 16.0);  // (DotGothic16's cell, not Stick's)
            QVERIFY(f.truetype("dot16", 16).strict_cmap());
        }();
        QVERIFY(QDir::setCurrent(was));
        use_source_fonts();
    }

    // --- the bytes ---------------------------------------------------------------------------------------------------------

    // No added face's file is read before one is asked for: the faces of before drawn, the forty looked up (whether they
    // are there, their faces, the feature) — nothing read; then one line in one face reads that face's file alone.
    void theAddedFacesAreNotReadUntilAsked() {
        use_source_fonts();
        files::clear_cache();
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        for (const char* key : {"", "gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"}) QVERIFY(has_ink(drawn(lettering(key)).image));
        for (const fonts::Entry& e : fonts::entries()) {
            QVERIFY(text::face_exists(e.key));
            QCOMPARE(text::face_of(Json(e.key)).key, e.key);
        }
        QVERIFY(files::missing().empty());
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        QVERIFY(has_ink(drawn(lettering("antigothic_bold")).image));
        // (its kana and kanji are its own; the wave and dash it lacks are not in this line)
        QCOMPARE(files::cached_bytes(), static_cast<std::uint64_t>(fonts::find("antigothic_bold")->bytes));
        // (a kana-only face reads its partner's file with its own when the line has kanji)
        QVERIFY(has_ink(drawn(lettering("horror_chokoku")).image));
        QCOMPARE(files::cached_bytes(), static_cast<std::uint64_t>(fonts::find("antigothic_bold")->bytes + fonts::find("horror_chokoku")->bytes +
                                                                   fonts::find("mincho_black")->bytes));
    }

    // The bytes of a file are read once and shared by everything that draws with them; what is kept never passes 64 MB
    // (the forty are 157 MB): the faces used longest ago are let go, and read again when asked for — the same bytes.
    void bytesAreSharedAndBounded() {
        use_source_fonts();
        files::clear_cache();
        const std::shared_ptr<const QByteArray> first = files::bytes("klee");
        QCOMPARE(files::bytes("klee").get(), first.get());
        {
            text::Fonts a, b;
            (void)a.truetype("klee", 20);
            (void)b.truetype("klee", 33);
            QCOMPARE(files::bytes("klee").get(), first.get());
            QVERIFY(first.use_count() >= 4);  // (this, the kept one, and the two letterings)
            QCOMPARE(genko::render::font_bytes("klee").get(), first.get());  // (the OpenType typesetting's bytes: the same)
        }
        QCOMPARE(first.use_count(), 2L);
        std::uint64_t total = 0;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added) continue;
            const std::shared_ptr<const QByteArray> bytes = files::bytes(e.key);
            total += static_cast<std::uint64_t>(bytes->size());
            QVERIFY2(files::cached_bytes() <= files::kCacheBytes, e.key.c_str());
            QVERIFY(files::cached_bytes() >= static_cast<std::uint64_t>(bytes->size()));  // (the one just read is kept)
        }
        QCOMPARE(total, std::uint64_t(157549348));
        QVERIFY(files::cached_bytes() > files::kCacheBytes / 2);
        // (Klee, read first, was let go — this holder keeps its copy — and is read again: other bytes, the same content)
        QCOMPARE(first.use_count(), 1L);
        const std::shared_ptr<const QByteArray> again = files::bytes("klee");
        QVERIFY(again.get() != first.get());
        QCOMPARE(*again, *first);
        files::clear_cache();
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        QCOMPARE(again.use_count(), 1L);
        // (not a face: never bytes)
        QVERIFY_THROWS_EXCEPTION(genko::render::NotYetPorted, files::bytes("gothic"));
        QVERIFY_THROWS_EXCEPTION(genko::render::NotYetPorted, files::bytes("no_such_face"));
        QVERIFY(files::path("gothic").empty());
        QVERIFY(!files::present("gothic"));
    }

    // --- beside the program ---------------------------------------------------------------------------------------------

    // The program's own rule: <the folder it is in>/../share/genko-studio/fonts. This test program has no such folder
    // (the faces are missing for it until it names one); the genko of this build has it — a line in an added face it
    // renders is not the line it renders with no font, which is what a face that was not found would be.
    void theProgramFindsTheFacesBesideItself() {
        files::set_dir_for_testing(std::nullopt);
        text::clear_layout_cache();
        const QString own = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../share/genko-studio/fonts"));
        const QString looked = QString::fromStdString(files::dir());
        const bool here = files::present("klee");
        use_source_fonts();
        QCOMPARE(looked, own);
        QCOMPARE(here, QFileInfo(own + QStringLiteral("/KleeOne-SemiBold.ttf")).isFile());

        QTemporaryDir dir;
        const auto rendered = [&](const char* name, const std::string& font) {
            Document doc = blank_book();
            doc.story.push_back(lettering(font));
            const QString book = dir.path() + QStringLiteral("/") + QString::fromLatin1(name) + QStringLiteral(".genko");
            genko::test::write_project(doc, genko::core::path_from_utf8(book.toStdString()));
            const QString out = dir.path() + QStringLiteral("/") + QString::fromLatin1(name) + QStringLiteral(".png");
            const genko::test::Run run = genko::test::run_genko(
                {QStringLiteral("render"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"), QStringLiteral("72"),
                 QStringLiteral("--out"), out});
            if (!run.finished || run.exit_code != 0) qWarning().noquote() << run.described();
            return genko::test::read_bytes(out);
        };
        const std::string none = rendered("none", "");
        const std::string kiwami = rendered("kiwami", "gothic_kiwami");
        const std::string unknown = rendered("unknown", "no_such_face");
        QVERIFY(!none.empty() && !kiwami.empty() && !unknown.empty());
        QCOMPARE(unknown, none);    // (a name nobody knows: the default face, as ever)
        QVERIFY(kiwami != none);    // (the added face was found beside the program)
    }

    // --- the sample sheets and the pins (written when asked) ---------------------------------------------------------------
    // (not a test of their own, and never a SKIP: the audit of a stage's run refuses every skipped slot)
    void cleanupTestCase() {
        use_source_fonts();
        const QByteArray pin = qgetenv("GENKO_FONT_FACES_PIN");
        if (!pin.isEmpty()) write_pins(pin);
        const QByteArray out = qgetenv("GENKO_FONTS_OUT");
        if (!out.isEmpty()) write_sample_sheets(out);
        files::set_dir_for_testing(std::nullopt);
    }

private:
    static void write_pins(const QByteArray& file) {
        Json faces = Json::object();
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.added) faces[e.key] = pins_of(e);
        }
        const Json all{{"schema", "genko-font-faces@1"},
                       {"about",
                        "The forty added lettering faces as Genko's own Linux build drew them (native/tests/unit/test_font_faces.cpp, GENKO_FONT_FACES_PIN): "
                        "single letters' masks (TrueTypeFont::getmask2: their box, size, offset, advance and the SHA-256 of their bytes) and three lines' "
                        "lettering (text_layout: down, across, and down with a reading, an outline and the heavy weight). Self-pinned and exact: no outside "
                        "program is their reference."},
                       {"words", kWords},
                       {"dpi", kDpi},
                       {"faces", std::move(faces)}};
        // (a face a block, a mask a line: a change shows as the lines it changes)
        std::string out = "{\n";
        for (const char* name : {"schema", "about", "words", "dpi"}) out += " " + genko::core::dump_python(Json(name)) + ": " + genko::core::dump_python(all[name]) + ",\n";
        out += " \"faces\": {\n";
        std::size_t n = 0;
        for (const auto& [key, face] : all["faces"].items()) {
            out += "  " + genko::core::dump_python(Json(key)) + ": {\n   \"masks\": [\n";
            for (std::size_t i = 0; i < face["masks"].size(); ++i) {
                out += "    " + genko::core::dump_python(face["masks"][i]) + (i + 1 < face["masks"].size() ? ",\n" : "\n");
            }
            out += "   ],\n";
            out += "   \"down\": " + genko::core::dump_python(face["down"]) + ",\n";
            out += "   \"across\": " + genko::core::dump_python(face["across"]) + ",\n";
            out += "   \"dressed\": " + genko::core::dump_python(face["dressed"]) + "\n";
            out += std::string("  }") + (++n < all["faces"].size() ? ",\n" : "\n");
        }
        out += " }\n}\n";
        genko::storage::write_atomic(std::filesystem::path(file.toStdString()), out);
    }

    // The sheets: every one of the forty and the seven of before, a line down in a balloon — plain, with a reading, and
    // outlined —, each under its key; parts of them at 600 dpi; and the plain sheet as a computer without the files draws it.
    static void write_sample_sheets(const QByteArray& out) {
        const std::filesystem::path dir(out.toStdString());
        std::filesystem::create_directories(dir);
        const auto write = [&](const std::string& name, const Image& image) {
            genko::storage::write_atomic(dir / name, genko::formats::png_bytes(image.convert("RGB")));
        };
        std::vector<std::string> keys = {"antique", "gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"};
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.added) keys.push_back(e.key);
        }
        // 18 faces a page: 6 across (from the right, as a page is read), 3 down; a cell is 30 × 90 mm
        const int per_page = 18;
        const int pages = static_cast<int>((keys.size() + per_page - 1) / per_page);
        const auto cell_x = [](std::size_t i) { return 18.0 + (5 - static_cast<int>(i % 18) % 6) * 30.0; };
        const auto cell_y = [](std::size_t i) { return 16.0 + (static_cast<int>(i % 18) / 6) * 90.0; };
        const auto book = [&](const std::string& kind) {
            Json ops = Json::array();
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const int page = static_cast<int>(i) / per_page + 1;
                const double x = cell_x(i), y = cell_y(i);
                Json style{{"font", keys[i]}, {"size_mm", 5.0}};
                Json op{{"op", "add_line"}, {"page", page}, {"id", "s" + std::to_string(i)}, {"balloon", kind == "outline" ? "none" : "speech"},
                        {"x_mm", x}, {"y_mm", y}, {"w_mm", 27.0}, {"h_mm", 70.0}, {"wrap", "vertical"},
                        {"text", "「ぎゃーっ！？」\nちょっと、待って！\n漢字（かんじ）です。"}};
                if (kind == "ruby") op["ruby_runs"] = Json::array({Json::array({"待", "ま"}), Json::array({"漢字", "かんじ"})});
                if (kind == "outline") {  // (white letters in a black outline, on the paper)
                    style["outline_mm"] = 0.35;
                    style["rgb"] = Json::array({255, 255, 255});
                    style["outline_rgb"] = Json::array({0, 0, 0});
                    style["size_mm"] = 6.0;
                }
                op["style"] = style;
                ops.push_back(op);
                // (under it: its key — with its partner's —, and its name)
                const fonts::Entry* e = fonts::find(keys[i]);
                std::string label = keys[i];
                if (e != nullptr && !e->fallback.empty()) label += " + " + e->fallback;
                label += "\n" + (e != nullptr ? e->label : std::string("アンチック（いつもの書体）"));
                ops.push_back(Json{{"op", "add_line"}, {"page", page}, {"id", "k" + std::to_string(i)}, {"balloon", "none"}, {"x_mm", x - 0.5}, {"y_mm", y + 72.0},
                                   {"w_mm", 28.0}, {"h_mm", 12.0}, {"wrap", "horizontal"}, {"text", label}, {"style", Json{{"font", "gothic"}, {"size_mm", 1.9}}}});
            }
            Document doc = blank_book(pages);
            for (int p = 0; p < pages; ++p) doc.edit_page(static_cast<std::size_t>(p)).numero = false;
            return run_ops(doc, ops).doc;
        };
        const auto page_picture = [](const Document& doc, int dpi, std::size_t page) {
            text::clear_layout_cache();
            return genko::render::render_page(doc.page(page), dpi, {}, &doc).image;
        };
        const auto part = [](const Document& doc, int dpi, std::size_t page, double x, double y, double w, double h) {
            genko::render::RenderOptions options;
            const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
            options.region = genko::render::RenderRegion{px(x), px(y), px(w), px(h)};
            return genko::render::render_page(doc.page(page), dpi, options, &doc).image;
        };
        for (const char* kind : {"plain", "ruby", "outline"}) {
            const Document doc = book(kind);
            for (int p = 0; p < pages; ++p) {
                write(std::string("fonts-150-") + kind + "-" + std::to_string(p + 1) + ".png", page_picture(doc, 150, static_cast<std::size_t>(p)));
            }
            // (600 dpi, pixel for pixel: one cell each of a true antique, a brush, a handwriting, a pixel face, a kana-only
            // face with its partner and a display face)
            for (const char* key : {"antique_genei", "brush_syuku", "hand_kurenaido", "dot12", "cute_daruma", "gothic_kiwami", "sfx_outline", "horror_chokoku"}) {
                const std::size_t i = static_cast<std::size_t>(std::find(keys.begin(), keys.end(), std::string(key)) - keys.begin());
                write(std::string("fonts-600-") + kind + "-" + key + ".png", part(doc, 600, i / per_page, cell_x(i) - 1.5, cell_y(i) - 1.0, 30.0, 72.0));
            }
        }
        // (the same plain sheets on a computer that has none of the files: every added face in the default face —
        // the effects faces' keys too: these are lines, not sound effects —, and a page of sound effects in the effects face)
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        const Document doc = book("plain");
        for (int p = 0; p < pages; ++p) write("fonts-150-missing-files-" + std::to_string(p + 1) + ".png", page_picture(doc, 150, static_cast<std::size_t>(p)));
        use_source_fonts();
    }
};

QTEST_GUILESS_MAIN(TestFontFaces)
#include "test_font_faces.moc"
