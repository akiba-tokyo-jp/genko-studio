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
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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
#include "core/type_text.hpp"
#include "formats/checks.hpp"
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

// The page's check issues about a font file that is not there (formats/checks: font_not_installed).
std::vector<Json> font_issues(const Document& doc) {
    std::vector<Json> out;
    for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
        if (issue.value("code", std::string()) == "font_not_installed") out.push_back(issue);
    }
    return out;
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
        const std::pair<const char*, int> strikes[] = {{"dot12", 12}, {"dot8", 8}};
        for (const auto& [key, size] : strikes) {
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
            // (見つからないフォントを解決 — a question, the book as it was — tells it apart from the missing fonts: one of
            // Genko's own faces, not installed here, which is never replaced or taken out of the book: test_font_service)
            Document doc = blank_book();
            doc.story.push_back(lettering(e->key));
            const genko::core::ApplyResult asked = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
            QVERIFY(asked.unchanged);
            QCOMPARE(asked.results.size(), std::size_t(1));
            QCOMPARE(asked.results[0]["missing"], Json::array());
            QCOMPARE(asked.results[0]["not_installed"], Json::array({Json{{"font", e->key}, {"label", e->label}}}));
            QCOMPARE(fonts::named(asked.doc), std::vector<std::string>{e->key});
            // (the page's check says so, once for the line, by the face's name: font_not_installed, a warning)
            const std::vector<Json> said = font_issues(doc);
            QCOMPARE(said.size(), std::size_t(1));
            QCOMPARE(said[0]["level"], Json("warning"));
            QCOMPARE(said[0]["target"], (Json{{"kind", "line"}, {"id", "L1"}}));
            QCOMPARE(said[0]["message"].get<std::string>(),
                     "書体「" + e->label + "」のファイルがこのパソコンに無いため、代わりにアンチックで描いた（アプリを入れ直すと元の書体で描ける）");
            // (nothing of its file was read into the cache, and a list for the screen says the same with one stat)
            QVERIFY(!files::file_there(e->key));
            QCOMPARE(files::files_not_there().size(), std::size_t(40));
            QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        }();
        use_source_fonts();
        // (and with the files back the same book names nothing missing, and its check says nothing of fonts)
        Document doc = blank_book();
        doc.story.push_back(lettering(e->key));
        const genko::core::ApplyResult with_files = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
        QCOMPARE(with_files.results[0]["missing"], Json::array());
        QVERIFY(!with_files.results[0].contains("not_installed"));
        QVERIFY(font_issues(doc).empty());
    }

    // The check of a book of before says what it said: a font file that is not there, a name nobody knows and the faces of
    // before are no font_not_installed — with or without the added faces' files —, and the issues of such a page are the
    // same either way.
    void theCheckOfABookOfBeforeSaysWhatItSaid() {
        Document doc = blank_book();
        int n = 0;
        for (const char* font : {"", "gothic", "sfx_pop", "/no/such/font.otf", "no_such_face", "Dot16"}) {
            genko::core::StoryLine line = lettering(font);
            line.id = "B" + std::to_string(++n);
            line.x_mm = genko::core::Num(20.0 + 28.0 * n);
            line.w_mm = genko::core::Num(24.0);
            doc.story.push_back(std::move(line));
        }
        const Json with_files = genko::formats::checks::page_issues(doc, doc.page(0));
        QVERIFY(font_issues(doc).empty());
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        const Json without = genko::formats::checks::page_issues(doc, doc.page(0));
        const bool none = font_issues(doc).empty();
        use_source_fonts();
        QVERIFY(none);
        QCOMPARE(without, with_files);
    }

    // A missing face is said wherever the page's letters name it — a line's style (a sound effect's by what it was drawn
    // in), a record's own character attributes and its ranges', a type layer's record and its ruby, a line's ruby (the
    // warning rubies always had, now by the face's name), the book's usual face (once a book, on its first page) —, once a
    // face a page, by its name and never its key.
    void theMissingFaceIsSaidWhereverItIsNamed() {
        Document doc = blank_book(2);
        const auto add = [&](const char* id, const std::string& font, double x) -> genko::core::StoryLine& {
            genko::core::StoryLine line = lettering(font);
            line.id = id;
            line.x_mm = genko::core::Num(x);
            line.w_mm = genko::core::Num(20.0);
            doc.story.push_back(std::move(line));
            return doc.story.back();
        };
        add("A1", "klee", 20);
        add("A2", "klee", 45);  // (the same face again: said once)
        add("B", "sfx_outline", 70).balloon = "sfx";
        genko::core::StoryLine& ranges = add("C", "", 95);
        ranges.native_type = genko::core::type_text::check_native_type(
            Json{{"char", Json{{"font", "dot16"}}}, {"runs", Json::array({Json{{"start", 0}, {"end", 2}, {"char", Json{{"font", "stick"}}}}})}}, ranges.text);
        genko::core::StoryLine& ruby = add("D", "", 120);
        ruby.ruby_runs = {Json::array({"漢字", "かんじ"})};
        ruby.ruby_spans = genko::core::ruby::from_runs(ruby.text, ruby.ruby_runs, false);
        (*ruby.ruby_spans)[0].font = "gothic_kiwami";
        add("E", "sfx_outline", 145);  // (a line, not a sound effect, in the face B names: one warning for both)
        {
            genko::core::StoryLine record = lettering("brush_syuku");
            record.id.clear();
            record.ruby_runs = {Json::array({"漢字", "かんじ"})};
            record.ruby_spans = genko::core::ruby::from_runs(record.text, record.ruby_runs, false);
            (*record.ruby_spans)[0].font = "maru_black";
            genko::core::Layer layer;
            layer.id = "ly_type";
            layer.kind = genko::core::LayerKind::Type;
            layer.title = "文字";
            layer.type = std::make_shared<const genko::core::StoryLine>(std::move(record));
            doc.edit_page(0).layers.push_back(std::move(layer));
        }
        doc.font_path = "antique_genei";
        const auto said_on = [&](std::size_t page) {
            std::vector<std::string> out;
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(page))) {
                const std::string code = issue.value("code", std::string());
                if (code.find("font") == std::string::npos) continue;
                QString target = QString::fromStdString(issue["target"]["kind"].get<std::string>());
                if (issue["target"]["id"].is_string()) target += QLatin1Char(' ') + QString::fromStdString(issue["target"]["id"].get<std::string>());
                out.push_back(issue["level"].get<std::string>() + " " + code + " [" + target.toStdString() + "] " + issue["message"].get<std::string>());
            }
            return out;
        };
        QVERIFY(said_on(0).empty());  // (every file is there)
        QVERIFY(said_on(1).empty());
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        const std::vector<std::string> first = said_on(0);
        const std::vector<std::string> second = said_on(1);
        use_source_fonts();
        const std::string tail = "（アプリを入れ直すと元の書体で描ける）";
        const std::vector<std::string> expected = {
            "warning ruby_font_fallback [line D] 台詞「「ぎゃーっ！？」ちょっと、待…」のルビ「かんじ」の書体「源暎きわみゴ」のファイルがこのパソコンに無いため、台詞の書体で描いた" + tail,
            "warning font_not_installed [line A1] 書体「クレー One SB」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail,
            "warning font_not_installed [line B] 書体「ランパート（立体の袋文字）」のファイルがこのパソコンに無いため、代わりにアンチックと極太の書体で描いた" + tail,
            "warning font_not_installed [line C] 書体「ドットゴシック16」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail,
            "warning font_not_installed [line C] 書体「ステッキ」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail,
            "warning font_not_installed [layer ly_type] 書体「佑字 肅」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail,
            "warning font_not_installed [layer ly_type] 書体「ZEN丸ゴシック Bk」のファイルがこのパソコンに無いため、代わりに本文の書体で描いた" + tail,
            "warning font_not_installed [page] この本のいつもの書体「源暎アンチック」のファイルがこのパソコンに無いため、書体を決めていない台詞をアンチックで描いた" + tail,
        };
        QCOMPARE(first.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) QCOMPARE(QString::fromStdString(first[i]), QString::fromStdString(expected[i]));
        QVERIFY(second.empty());  // (the book's usual face is said once a book: on its first page)
        // (and the whole book's check holds each once)
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        int about_fonts = 0;
        const Json whole = genko::formats::checks::book(doc);
        for (const Json& issue : whole["issues"]) about_fonts += issue.value("code", std::string()).find("font") != std::string::npos ? 1 : 0;
        use_source_fonts();
        QCOMPARE(about_fonts, 8);
    }

    // The places the first round of this check missed (the second review's P3-2): a face a record's letters take from
    // one of the book's type styles; a type layer inside the contents of a smart object the page shows; the partner of
    // the book's usual face. Each is said, once; with the files there nothing is.
    void theMissingFaceIsSaidThroughStylesContentsAndTheUsualFace() {
        const auto said_on = [](const Document& doc) {
            std::vector<std::string> out;
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
                const std::string code = issue.value("code", std::string());
                if (code.find("font") != std::string::npos) out.push_back(code + " " + issue["message"].get<std::string>());
            }
            return out;
        };
        const std::string tail = "（アプリを入れ直すと元の書体で描ける）";
        // (a type style: the record names the style, the style names the face)
        Document styled = blank_book();
        styled.type_styles = genko::core::type_text::check_type_styles(
            Json{{"character", Json::array({Json{{"id", "cs-1"}, {"name", "手紙"}, {"char", Json{{"font", "klee"}}}}})}});
        {
            genko::core::StoryLine line = lettering("");
            line.native_type = genko::core::type_text::check_native_type(Json{{"runs", Json::array({Json{{"start", 0}, {"end", 3}, {"style", "cs-1"}}})}}, line.text);
            styled.story.push_back(std::move(line));
        }
        QCOMPARE(fonts::named(styled), std::vector<std::string>{"klee"});
        // (a smart object's contents: a type layer made one, its record inside the contents' page)
        Document nested = blank_book();
        {
            genko::core::StoryLine record = lettering("brush_syuku");
            record.id.clear();
            genko::core::Layer layer;
            layer.id = "ly_type";
            layer.kind = genko::core::LayerKind::Type;
            layer.title = "文字";
            layer.type = std::make_shared<const genko::core::StoryLine>(std::move(record));
            nested.edit_page(0).layers.push_back(std::move(layer));
        }
        nested = run_ops(nested, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "ly_type"}, {"new_id", "so1"}}})).doc;
        {
            bool on_page = false;
            for (const genko::core::Layer& layer : nested.page(0).layers) on_page = on_page || layer.type != nullptr;
            QVERIFY(!on_page);  // (the record is in the contents now, not on the page)
            QCOMPARE(nested.smart_contents.size(), std::size_t(1));
            QCOMPARE(fonts::named(nested), std::vector<std::string>{"brush_syuku"});
        }
        // (the book's usual face a face without kanji: its partner)
        Document usual = blank_book();
        usual.font_path = "cute_daruma";
        usual.story.push_back(lettering(""));
        Document usual_kana = blank_book();
        usual_kana.font_path = "cute_daruma";
        usual_kana.story.push_back(lettering("", true, "そこはだめ！いくよ"));
        genko::core::StoryLine own = lettering("gothic");  // (a line with a face of its own is not the usual face's)
        own.id = "L2";
        usual_kana.story.push_back(std::move(own));

        for (const Document* doc : {&styled, &nested, &usual, &usual_kana}) QVERIFY(said_on(*doc).empty());  // (every file is there)
        QTemporaryDir some;
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/DarumadropOne-Regular.ttf", some.path() + "/DarumadropOne-Regular.ttf"));
        files::set_dir_for_testing(some.path().toStdString());
        text::clear_layout_cache();
        const std::vector<std::string> by_style = said_on(styled), in_contents = said_on(nested), partner = said_on(usual), kana = said_on(usual_kana);
        use_source_fonts();
        QCOMPARE(by_style, std::vector<std::string>{"font_not_installed 書体「クレー One SB」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail});
        QCOMPARE(in_contents, std::vector<std::string>{"font_not_installed 書体「佑字 肅」のファイルがこのパソコンに無いため、代わりにアンチックで描いた" + tail});
        QCOMPARE(partner, std::vector<std::string>{"font_partner_not_installed この本のいつもの書体「だるまどろっぷ」の漢字などを受け持つ書体「モッチーポップ」の"
                                                   "ファイルがこのパソコンに無いため、その字はゴシックで描いた" + tail});
        QVERIFY(kana.empty());  // (no line of the usual face has a letter the partner draws)
    }

    // A file that could not be opened, or is not there, is "not installed" for that asking only — not for the run, as a
    // file of another content is: put there, or made readable, the face is installed at the next asking, and what was
    // drawn without it is drawn again (generation() moves each time the answer changes). While it cannot be read the
    // line is in the default face and the check says so — never another font, never silently.
    void aFileThatCouldNotBeReadIsAskedForAgain() {
        QTemporaryDir live;
        files::set_dir_for_testing(live.path().toStdString());
        text::clear_layout_cache();
        [&] {
            const QString klee = live.path() + "/KleeOne-SemiBold.ttf";
            const genko::core::StoryLine line = lettering("klee");
            Document doc = blank_book();
            doc.story.push_back(line);
            const std::string plain = text::text_layout(lettering(""), kDpi).image.tobytes();
            // (not there)
            QVERIFY(!files::present("klee"));
            QVERIFY(!files::damaged("klee"));
            QCOMPARE(text::remembered_layout(line, kDpi, std::nullopt)->image.tobytes(), plain);
            QCOMPARE(font_issues(doc).size(), std::size_t(1));
            const std::uint64_t absent = files::generation();
            // (put there: installed at the next asking, the line set again in it, nothing said)
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/KleeOne-SemiBold.ttf", klee));
            QVERIFY(files::present("klee"));
            QVERIFY(files::generation() > absent);
            const std::string in_klee = text::remembered_layout(line, kDpi, std::nullopt)->image.tobytes();
            QVERIFY(in_klee != plain);
            QVERIFY(font_issues(doc).empty());
#ifdef Q_OS_UNIX
            // (there, of its size, and not to be opened — the tests run without the right to pass a file's permissions —:
            // tried twice, not installed this time, said as 「無い」 and not as damaged)
            const std::uint64_t readable = files::generation();
            files::clear_cache();
            QVERIFY(QFile::setPermissions(klee, QFileDevice::Permissions()));
            QVERIFY(!QFile(klee).open(QIODevice::ReadOnly));
            QCOMPARE(text::text_layout(lettering("klee", true, "いま読めない"), kDpi).image.tobytes(),
                     text::text_layout(lettering("gothic", true, "いま読めない"), kDpi).image.tobytes());  // (caught in the middle: the bundled Gothic)
            QVERIFY(files::generation() > readable);
            QVERIFY(!files::present("klee"));
            QVERIFY(files::file_there("klee"));  // (a file of the size is there: not found to be another's)
            QVERIFY(!files::damaged("klee"));
            QCOMPARE(text::remembered_layout(line, kDpi, std::nullopt)->image.tobytes(), plain);
            QCOMPARE(font_issues(doc).size(), std::size_t(1));
            QVERIFY(font_issues(doc)[0]["message"].get<std::string>().find("のファイルがこのパソコンに無いため") != std::string::npos);
            // (readable again: installed again, the line in the face again)
            const std::uint64_t unreadable = files::generation();
            QVERIFY(QFile::setPermissions(klee, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            QVERIFY(files::present("klee"));
            QVERIFY(files::generation() > unreadable);
            QCOMPARE(text::remembered_layout(line, kDpi, std::nullopt)->image.tobytes(), in_klee);
            QVERIFY(font_issues(doc).empty());
#endif
            // (a file of the size with another content is not asked again: for the run, whatever is put there after)
            files::clear_cache();
            QVERIFY(QFile::remove(klee));
            {
                QFile zeros(klee);
                QVERIFY(zeros.open(QIODevice::WriteOnly));
                zeros.write(QByteArray(static_cast<qsizetype>(fonts::find("klee")->bytes), '\0'));
            }
            QVERIFY_THROWS_EXCEPTION(genko::render::NotYetPorted, files::bytes("klee"));
            QVERIFY(!files::present("klee") && files::damaged("klee"));
            QVERIFY(QFile::remove(klee));
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/KleeOne-SemiBold.ttf", klee));
            QVERIFY(!files::present("klee"));
            QVERIFY(!files::file_there("klee"));
        }();
        use_source_fonts();
    }

    // What a person reads about the faces is Japanese in one register, with no run of four Latin letters or more, no
    // key and no word of the program's insides: every face's name and use, every category, and every warning about a
    // missing face, for all forty.
    void everyLabelAndWarningReadsInJapanese() {
        static const QRegularExpression latin(QStringLiteral("[A-Za-z]{4,}"));
        static const QRegularExpression kana(QStringLiteral("[ぁ-んァ-ヶ一-龥]"));
        const auto reads = [&](const std::string& text) {
            const QString t = QString::fromStdString(text);
            return !t.isEmpty() && kana.match(t).hasMatch() && !latin.match(t).hasMatch();
        };
        for (const fonts::Category& c : fonts::categories()) QVERIFY2(reads(c.label), c.label.c_str());
        for (const fonts::Entry& e : fonts::entries()) {
            QVERIFY2(reads(e.label), e.label.c_str());
            QVERIFY2(reads(e.use), e.use.c_str());
        }
        // (the two whose own names are long Latin words have a Japanese name on the screen, their own kept beside it)
        QCOMPARE(fonts::find("gothic_seed")->label, std::string("ラインシード B"));
        QCOMPARE(fonts::find("narrow_lubrifont")->label, std::string("滑油字（長体）"));
        for (const Json& f : fonts::catalog()["fonts"]) {
            const bool renamed = f["key"] == Json("gothic_seed") || f["key"] == Json("narrow_lubrifont");
            QCOMPARE(f.contains("name_original"), renamed);
        }
        QCOMPARE(catalog_font("gothic_seed")["name_original"], Json("LINE Seed JP B"));

        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        text::clear_layout_cache();
        std::vector<std::string> messages;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added) continue;
            Document doc = blank_book();
            doc.story.push_back(lettering(e.key));
            genko::core::StoryLine effect = lettering(e.key);
            effect.id = "S";
            effect.balloon = "sfx";
            effect.x_mm = genko::core::Num(90.0);
            doc.story.push_back(std::move(effect));
            genko::core::StoryLine ruby = dressed("", true, false, false);
            ruby.id = "R";
            ruby.x_mm = genko::core::Num(140.0);
            (*ruby.ruby_spans)[0].font = e.key;
            doc.story.push_back(std::move(ruby));
            genko::core::StoryLine record = dressed(e.key, true, false, false);
            record.id.clear();
            genko::core::Layer layer;
            layer.id = "ly_type";
            layer.kind = genko::core::LayerKind::Type;
            layer.type = std::make_shared<const genko::core::StoryLine>(std::move(record));
            doc.edit_page(0).layers.push_back(std::move(layer));
            doc.font_path = e.key;
            int n = 0;
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
                if (issue.value("code", std::string()).find("font") == std::string::npos) continue;
                messages.push_back(issue["message"].get<std::string>());
                QCOMPARE(issue["level"], Json("warning"));
                ++n;
            }
            // (the ruby's; the face's — once for the line, the sound effect and the type layer —; the book's usual face)
            if (n != 3) QFAIL(qPrintable(QStringLiteral("%1: %2 warnings").arg(QString::fromStdString(e.key)).arg(n)));
        }
        // (a face whose partner is missing)
        QTemporaryDir some;
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.fallback.empty()) QVERIFY(QFile::copy(QString::fromStdString(source_fonts() + "/" + e.file), some.path() + "/" + QString::fromStdString(e.file)));
        }
        files::set_dir_for_testing(some.path().toStdString());
        text::clear_layout_cache();
        std::vector<std::string> partners;
        const auto partner_said = [&](const fonts::Entry& e, const char* words) {
            Document doc = blank_book();
            doc.story.push_back(lettering(e.key, true, words));
            int n = 0;
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
                if (issue.value("code", std::string()) != "font_partner_not_installed") continue;
                partners.push_back(issue["message"].get<std::string>());
                messages.push_back(partners.back());
                ++n;
            }
            return n;
        };
        // (a kanji the face with only some kanji does not have: looked for in its own cmap)
        std::string rare;
        {
            text::Fonts f;
            for (const char* kanji : {"鬱", "麤", "驫", "龘"}) {
                if (rare.empty() && !f.has_glyph("sfx_inline", text::u32(kanji))) rare = kanji;
            }
        }
        QVERIFY(!rare.empty());
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.fallback.empty()) continue;
            // (said only when a letter of the words is the partner's: words of kana alone say nothing — nothing was drawn
            // otherwise —, and the face with some kanji says it for a kanji it lacks, not for the ones it has)
            QCOMPARE(partner_said(e, "そこはだめ！いくよ"), 0);
            if (e.kanji == "partial") {
                QCOMPARE(partner_said(e, kWords), 0);
                QCOMPARE(partner_said(e, ("憂" + rare).c_str()), 1);
            } else {
                QCOMPARE(partner_said(e, kWords), 1);
            }
        }
        use_source_fonts();
        QCOMPARE(partners.size(), std::size_t(7));
        QCOMPARE(partners[0], std::string("書体「チェリーボム」の漢字などを受け持つ書体「ZEN丸ゴシック Bk」のファイルがこのパソコンに無いため、"
                                          "その字はゴシックで描いた（アプリを入れ直すと元の書体で描ける）"));
        QCOMPARE(messages.size(), std::size_t(40 * 3 + 7));
        for (const std::string& message : messages) {
            const QString m = QString::fromStdString(message);
            QVERIFY2(reads(message), message.c_str());
            // one register (plain), no polite ending mixed in; no key, no name of the program, no word of its insides
            QVERIFY2(m.endsWith(QStringLiteral("描いた（アプリを入れ直すと元の書体で描ける）")), message.c_str());
            QVERIFY2(!m.contains(QStringLiteral("ます")) && !m.contains(QStringLiteral("です")) && !m.contains(QStringLiteral("ください")), message.c_str());
            QVERIFY2(!m.contains(QLatin1Char('_')) && !m.contains(QStringLiteral("Genko"), Qt::CaseInsensitive), message.c_str());
            for (const fonts::Entry& e : fonts::entries()) {
                if (e.added && e.key.size() >= 5) QVERIFY2(!m.contains(QString::fromStdString(e.key)), message.c_str());
            }
        }
    }

    // A file of the face's name and size that is not the face's file — all zeros, another font padded to the size, the
    // real file's first bytes and then zeros — is not the face: its SHA-256 is looked at the first time the face is asked
    // for, the face is not installed, the line is drawn in the default face (never in the other font, never in Pillow's),
    // and it is said. A list for the screen, which only stats, still sees a file there — until the face has been asked for.
    void aFileOfTheSameSizeButOtherContentIsNotInstalled() {
        QTemporaryDir other;
        const auto write = [&](const char* key, const QByteArray& bytes) {
            const fonts::Entry* e = fonts::find(key);
            QByteArray sized = bytes.left(static_cast<qsizetype>(e->bytes));
            sized.append(QByteArray(static_cast<qsizetype>(e->bytes) - sized.size(), '\0'));
            QFile file(other.path() + "/" + QString::fromStdString(e->file));
            return file.open(QIODevice::WriteOnly) && file.write(sized) == sized.size();
        };
        const auto source = [](const char* file) { return QByteArray::fromStdString(genko::test::read_bytes(QString::fromStdString(source_fonts() + "/" + file))); };
        QVERIFY(write("cute_daruma", QByteArray()));                                   // (all zeros)
        QVERIFY(write("pop_mochiy", source("Stick-Regular.ttf")));                    // (another font, padded)
        QVERIFY(write("klee", source("KleeOne-SemiBold.ttf").left(64 * 1024)));        // (its own head, then zeros)
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/Stick-Regular.ttf", other.path() + "/Stick-Regular.ttf"));  // (and a good one)
        files::set_dir_for_testing(other.path().toStdString());
        text::clear_layout_cache();
        [&] {
            for (const char* key : {"cute_daruma", "pop_mochiy", "klee"}) {
                QVERIFY2(files::file_there(key), key);  // (one stat: a file of the size is there)
                QVERIFY2(!files::present(key), key);    // (read and looked at: not the catalogue's)
                QVERIFY2(!files::file_there(key), key);  // (and from then on not counted as there)
                QVERIFY(!text::face_exists(key));
                QCOMPARE(text::face_of(Json(key)).key, std::string("antique"));
                QCOMPARE(drawn(lettering(key)).image.tobytes(), drawn(lettering("")).image.tobytes());
                QVERIFY_THROWS_EXCEPTION(genko::render::NotYetPorted, files::bytes(key));
                Document doc = blank_book();
                doc.story.push_back(lettering(key));
                QCOMPARE(font_issues(doc).size(), std::size_t(1));
                // (said as what it is: something is there that is not the face's file — not 「このパソコンに無い」)
                QVERIFY(files::damaged(key));
                QCOMPARE(font_issues(doc)[0]["message"].get<std::string>(),
                         "書体「" + fonts::find(key)->label + "」のファイルが無いか、壊れているため、代わりにアンチックで描いた（アプリを入れ直すと元の書体で描ける）");
            }
            // (a file of another size is the same; one that is simply not there is 「無い」)
            {
                QFile cut(other.path() + "/YujiBoku-Regular.ttf");
                QVERIFY(cut.open(QIODevice::WriteOnly));
                cut.write("not a font");
            }
            QVERIFY(!files::present("brush_boku") && files::damaged("brush_boku"));
            QVERIFY(!files::present("brush_syuku") && !files::damaged("brush_syuku"));
            QVERIFY(!files::damaged("stick") && !files::damaged("gothic") && !files::damaged("no_such_face"));
            QCOMPARE(files::cached_bytes(), std::uint64_t(0));
            QVERIFY(files::present("stick"));
            QVERIFY(drawn(lettering("stick")).image.tobytes() != drawn(lettering("")).image.tobytes());
            // (looked at all at once, as the doctor does: there, of the size, not of the SHA-256)
            int wrong = 0, good = 0;
            for (const files::Checked& c : files::verify(true)) {
                if (c.key == "cute_daruma" || c.key == "pop_mochiy" || c.key == "klee") {
                    QVERIFY2(c.exists && c.size_ok && c.hash_ok.has_value() && !*c.hash_ok, c.key.c_str());
                    ++wrong;
                } else if (c.key == "stick") {
                    QVERIFY(c.exists && c.size_ok && c.hash_ok.has_value() && *c.hash_ok);
                    ++good;
                } else if (c.key == "brush_boku") {
                    QVERIFY(c.exists && !c.size_ok && !c.hash_ok.has_value());
                } else {
                    QVERIFY2(!c.exists && !c.hash_ok.has_value(), c.key.c_str());
                }
            }
            QCOMPARE(wrong, 3);
            QCOMPARE(good, 1);
            QCOMPARE(files::missing().size(), std::size_t(39));
        }();
        use_source_fonts();
    }

    // A file that was good and is gone, or another file, when its bytes are read again (they were let go; the program was
    // upgraded while it runs): the face is not installed from then on. The piece of lettering being set at that moment is
    // drawn in the bundled Gothic — never Pillow's own font —, and everything set afterwards, the lines set before it too,
    // in the default face, with the warning.
    void aFileThatChangesWhileGenkoRuns() {
        QTemporaryDir live;
        const QString klee = live.path() + "/KleeOne-SemiBold.ttf";
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/KleeOne-SemiBold.ttf", klee));
        files::set_dir_for_testing(live.path().toStdString());
        text::clear_layout_cache();
        [&] {
            const genko::core::StoryLine before = lettering("klee");
            const std::shared_ptr<const text::Layout> good = text::remembered_layout(before, kDpi, std::nullopt);
            QVERIFY(good->image.tobytes() != text::text_layout(lettering(""), kDpi).image.tobytes());
            const std::uint64_t generation = files::generation();
            // (its bytes let go, as when many faces are in use; the file swapped for zeros of the same size)
            files::clear_cache();
            {
                QFile file(klee);
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write(QByteArray(static_cast<qsizetype>(fonts::find("klee")->bytes), '\0'));
            }
            QVERIFY(files::present("klee"));  // (the answer kept: nothing has read the file again yet)
            const genko::core::StoryLine during = lettering("klee", true, "かなと漢字を、\nいま組む！");
            const text::Layout caught = text::text_layout(during, kDpi);
            QVERIFY(files::generation() > generation);
            QVERIFY(!files::present("klee"));
            QVERIFY(!files::file_there("klee"));
            // (the lettering caught in the middle: every letter in the bundled Gothic)
            const text::Layout gothic = text::text_layout(lettering("gothic", true, "かなと漢字を、\nいま組む！"), kDpi);
            QCOMPARE(caught.image.size(), gothic.image.size());
            QCOMPARE(caught.image.tobytes(), gothic.image.tobytes());
            // (after it: the default face, for a new line and for the line remembered before — not the picture kept)
            QCOMPARE(text::face_of(Json("klee")).key, std::string("antique"));
            QCOMPARE(text::text_layout(during, kDpi).image.tobytes(), text::text_layout(lettering("", true, "かなと漢字を、\nいま組む！"), kDpi).image.tobytes());
            const std::shared_ptr<const text::Layout> again = text::remembered_layout(before, kDpi, std::nullopt);
            QVERIFY(again->image.tobytes() != good->image.tobytes());
            QCOMPARE(again->image.tobytes(), text::text_layout(lettering(""), kDpi).image.tobytes());
            Document doc = blank_book();
            doc.story.push_back(before);
            QCOMPARE(font_issues(doc).size(), std::size_t(1));
            // (the same when the file is simply gone, for a face read for the first time then)
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/Stick-Regular.ttf", live.path() + "/Stick-Regular.ttf"));
            QVERIFY(files::file_there("stick"));
            QVERIFY(QFile::remove(live.path() + "/Stick-Regular.ttf"));
            QVERIFY(!files::present("stick"));
            QCOMPARE(text::text_layout(lettering("stick"), kDpi).image.tobytes(), text::text_layout(lettering(""), kDpi).image.tobytes());
        }();
        use_source_fonts();
    }

    // A word set larger (a style run) that ends in ！ is found in its line in the faces drawn without the full-width
    // mark too — there the text is set with the ASCII mark, and the word goes the same way —, as are the words marked
    // to be set across in one cell (縦中横): the lettering with the run is not the lettering without it, down and across.
    void aShoutedWordWithMarksKeepsItsStyle_data() {
        QTest::addColumn<QString>("key");
        for (const char* key : {"cute_daruma", "hand_slackside", "techno_monomaniac", "pop_mochiy", "klee", "dot8", "sfx_aoboshi"}) QTest::newRow(key) << QString::fromLatin1(key);
    }
    void aShoutedWordWithMarksKeepsItsStyle() {
        QFETCH(QString, key);
        const std::string face = key.toStdString();
        for (const bool down : {true, false}) {
            genko::core::StoryLine plain = lettering(face, down, "そこはだめ！いくよ");
            genko::core::StoryLine shouted = plain;
            shouted.style_runs = {{"だめ！", Json{{"scale", 1.6}}}};
            const text::Layout a = drawn(plain);
            const text::Layout b = drawn(shouted);
            QVERIFY2(b.image.size() != a.image.size(), down ? "down" : "across");  // (the word larger: the lettering wider or taller)
            // (as large as the same word without its mark styled in a line without the mark makes its letters)
            genko::core::StoryLine coloured = plain;
            coloured.style_runs = {{"だめ！", Json{{"rgb", Json::array({200, 0, 0})}}}};
            const text::Layout c = drawn(coloured);
            QCOMPARE(c.image.size(), a.image.size());
            QVERIFY(c.image.tobytes() != a.image.tobytes());
            // (a dash the face lacks, swapped for the one it has, is found too — in an added face)
            genko::core::StoryLine dashed = lettering(face, down, "まさか―そんな");
            genko::core::StoryLine dashed_run = dashed;
            dashed_run.style_runs = {{"―そんな", Json{{"scale", 1.5}}}};
            QVERIFY(drawn(dashed_run).image.size() != drawn(dashed).image.size());
        }
        // (縦中横 by the words: ？！ chosen with a letter is one cell — the column a cell shorter)
        genko::core::StoryLine marks = lettering(face, true, "えっA？だ");
        genko::core::StoryLine marked = marks;
        marked.style_runs = {{"A？", Json{{"tcy", true}}}};
        const text::Layout one = drawn(marks);
        const text::Layout two = drawn(marked);
        QVERIFY(two.image.height() < one.image.height());
    }

    // A kanji with a variation selector in a face without kanji goes to the face's partner, as the kanji alone does,
    // and is drawn there — not as the face's "no glyph" mark; a kana with a selector stays the face's own. A face of
    // before takes its half for such a cell as it always did.
    void aLetterWithASelectorGoesWhereTheLetterGoes_data() { added_rows(); }
    void aLetterWithASelectorGoesWhereTheLetterGoes() {
        QFETCH(QString, key);
        const fonts::Entry* e = fonts::find(key.toStdString());
        text::Fonts f;
        const text::Face face = text::face_of(Json(e->key));
        QVERIFY(face.added);
        const std::u32string kanji = U"漢\U000E0100", level2 = U"鬱\U000E0101", kana = U"あ︀";
        QCOMPARE(f.path_for(face, kanji), f.path_for(face, U"漢"));
        QCOMPARE(f.path_for(face, level2), f.path_for(face, U"鬱"));
        QCOMPARE(f.path_for(face, kana), e->key);
        QVERIFY(face.own(f.path_for(face, level2)));
        // (drawn in a line: the cell is not the "no glyph" mark of the font it is drawn in, and is its letter's picture
        // or that font's own variant of it)
        const std::string used = f.path_for(face, kanji);
        const genko::render::TrueTypeFont& font = f.truetype(used, 48);
        const Image cell = text::glyph(kanji, font, 48, {0, 0, 0});
        QVERIFY(has_ink(cell));
        QVERIFY(cell.tobytes() != text::glyph(std::u32string(1, char32_t{0xFFFF}), font, 48, {0, 0, 0}).tobytes());
        // (set one glyph a code point, a selector the font has no glyph for would be a "no glyph" box beside the letter:
        // in an added face it is left out — the letter alone, down and across)
        if (font.glyph_index(0xE0100) == 0) {
            QCOMPARE(cell.tobytes(), text::glyph(U"漢", font, 48, {0, 0, 0}).tobytes());
            QCOMPARE(text::drawable(kanji, font), std::u32string(U"漢"));
            for (const bool down : {true, false}) {
                QCOMPARE(drawn(lettering(e->key, down, "漢\U000E0100字だ")).image.tobytes(), drawn(lettering(e->key, down, "漢字だ")).image.tobytes());
            }
        }
        QCOMPARE(text::drawable(kanji, f.truetype("gothic", 48)), kanji);  // (a font of before: as it is)
        QVERIFY(has_ink(drawn(lettering(e->key, true, "漢\U000E0100字と鬱\U000E0101")).image));
        const text::SetLayout set = text::set_layout(lettering(e->key, true, "漢\U000E0100字"), kDpi, std::nullopt, text::LetterParts{});
        QCOMPARE(set.set.rows.size(), std::size_t(1));
        QCOMPARE(set.set.rows[0].chars.size(), std::size_t(2));
        QCOMPARE(set.set.rows[0].chars[0].font, set.set.rows[0].chars[1].font);
        // (the faces of before: the half the cell's first letter picks, whatever the font has — as it always was)
        const text::Face antique = text::face_of(Json("antique"));
        QVERIFY(!antique.added);
        QCOMPARE(f.path_for(antique, kanji), std::string("gothic"));
        QCOMPARE(f.path_for(antique, kana), std::string("mincho"));
        const text::Face effects = text::face_of(Json("sfx"));
        QCOMPARE(f.path_for(effects, U"☃️"), std::string("sfx"));
    }

    // A record's face number (the face of a font collection another font left on it) is not an added face's: the text
    // is set in the face, as without the number — not in a font that cannot be opened.
    void aFaceNumberLeftOnAnAddedFaceIsNotRead() {
        for (const char* key : {"klee", "cute_daruma", "dot12"}) {
            genko::core::StoryLine record = lettering(key, false, "かなとカナと漢字");
            record.native_type = genko::core::type_text::check_native_type(Json{{"shaping", "opentype"}, {"char", Json{{"font", key}}}}, record.text);
            genko::core::StoryLine numbered = record;
            numbered.native_type =
                genko::core::type_text::check_native_type(Json{{"shaping", "opentype"}, {"char", Json{{"font", key}, {"face_index", 3}}}}, record.text);
            const text::Layout plain = drawn(record);
            const text::Layout with_number = drawn(numbered);
            QVERIFY(has_ink(plain.image));
            QCOMPARE(with_number.image.size(), plain.image.size());
            QCOMPARE(with_number.image.tobytes(), plain.image.tobytes());
            // (and it is the face: not what the default face sets)
            genko::core::StoryLine usual = record;
            usual.style.erase("font");
            usual.native_type = genko::core::type_text::check_native_type(Json{{"shaping", "opentype"}}, record.text);
            QVERIFY(drawn(usual).image.tobytes() != plain.image.tobytes());
            // (a range's own too, down)
            genko::core::StoryLine down = lettering("", true, "かなとカナと漢字");
            down.native_type = genko::core::type_text::check_native_type(
                Json{{"shaping", "opentype"}, {"runs", Json::array({Json{{"start", 0}, {"end", 3}, {"char", Json{{"font", key}, {"face_index", 2}}}}})}}, down.text);
            genko::core::StoryLine down_plain = lettering("", true, "かなとカナと漢字");
            down_plain.native_type = genko::core::type_text::check_native_type(
                Json{{"shaping", "opentype"}, {"runs", Json::array({Json{{"start", 0}, {"end", 3}, {"char", Json{{"font", key}}}}})}}, down.text);
            QCOMPARE(drawn(down).image.tobytes(), drawn(down_plain).image.tobytes());
        }
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

    // No added face's file is read before a face is asked for: the faces of before drawn, the catalogue looked through,
    // a list of the files for the screen (forty stats) — nothing read; a face asked for reads its own file alone, and a
    // line in a face without kanji its partner's with it.
    void theAddedFacesAreNotReadUntilAsked() {
        use_source_fonts();
        files::clear_cache();
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        for (const char* key : {"", "gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"}) QVERIFY(has_ink(drawn(lettering(key)).image));
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.added) QVERIFY(files::file_there(e.key));
            QVERIFY(fonts::find(e.key) != nullptr);
            QVERIFY(e.added == !files::path(e.key).empty());
        }
        QVERIFY(files::files_not_there().empty());
        Document doc = blank_book();
        doc.story.push_back(lettering("klee"));
        QVERIFY(fonts::uses_feature(doc));
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        // (whether a face is installed: its file read and looked at, once)
        QVERIFY(files::present("antigothic_bold"));
        QCOMPARE(files::cached_bytes(), static_cast<std::uint64_t>(fonts::find("antigothic_bold")->bytes));
        QVERIFY(has_ink(drawn(lettering("antigothic_bold")).image));
        // (its kana and kanji are its own; the wave and dash it lacks are not in this line)
        QCOMPARE(files::cached_bytes(), static_cast<std::uint64_t>(fonts::find("antigothic_bold")->bytes));
        // (a kana-only face reads its partner's file with its own)
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
        // (Klee, read first, was let go — this holder keeps its copy —; asked for again while someone holds it, it is the
        // holder's bytes that are given and kept again, not the file read once more: what a sample being made holds, a
        // worker having read it, is there for the screen's thread)
        QCOMPARE(first.use_count(), 1L);
        const std::shared_ptr<const QByteArray> again = files::bytes("klee");
        QCOMPARE(again.get(), first.get());
        QCOMPARE(first.use_count(), 3L);  // (first, again, the kept one)
        // (forgotten here altogether — clear_cache —, it is read again: other bytes, the same content)
        const QByteArray content = *first;
        files::clear_cache();
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        QCOMPARE(again.use_count(), 2L);
        const std::shared_ptr<const QByteArray> fresh = files::bytes("klee");
        QVERIFY(fresh.get() != again.get());
        QCOMPARE(*fresh, content);
        files::clear_cache();
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
        const QString file = QString::fromStdString(files::path("klee"));
        const bool there = files::file_there("klee");
        const bool here = files::present("klee");
        files::prime();
        const QString again = QString::fromStdString(files::dir());
        use_source_fonts();
        QCOMPARE(looked, own);
        QCOMPARE(again, own);  // (asked of Qt once, the same answer after)
        QVERIFY(QDir::isAbsolutePath(looked));
        QCOMPARE(file, own + QStringLiteral("/KleeOne-SemiBold.ttf"));
        // (no such folder beside a test program: nothing there, nothing installed, nothing read for it)
        QVERIFY2(!QFileInfo::exists(own), qPrintable(own));
        QVERIFY(!there);
        QVERIFY(!here);

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

    // The step that puts the folder beside a build tree's program (cmake/GenkoDevFonts.cmake): a link to the source's
    // folder where one can be made; where the place is taken by a real folder (or links cannot be made: Windows) the forty
    // files and the catalogue copied — only those —, left alone when they are right, a missing one put back. Both ways the
    // program finds every face installed there.
    void theBuildTreeStepLinksOrCopies() {
        QTemporaryDir dir;
        const QString script = genko::test::repo_root() + QStringLiteral("/native/cmake/GenkoDevFonts.cmake");
        const auto step = [&](const QString& dest) {
            const genko::test::Run run =
                genko::test::run(QStringLiteral(GENKO_CMAKE_COMMAND),
                                 {QStringLiteral("-DGENKO_FONTS_SOURCE=") + QString::fromStdString(source_fonts()), QStringLiteral("-DGENKO_FONTS_DEST=") + dest,
                                  QStringLiteral("-P"), script},
                                 QProcessEnvironment::systemEnvironment());
            if (!run.finished || run.exit_code != 0) qWarning().noquote() << run.described();
            return run.finished && run.exit_code == 0;
        };
        const auto installed = [&](const QString& dest) {
            files::set_dir_for_testing(dest.toStdString());
            const std::size_t gone = files::files_not_there().size();
            const bool klee = files::present("klee") && files::present("dot8");
            use_source_fonts();
            return gone == 0 && klee;
        };
#ifndef _WIN32
        const QString linked = dir.path() + QStringLiteral("/a/share/genko-studio/fonts");
        QVERIFY(step(linked));
        QVERIFY(QFileInfo(linked).isSymLink());
        QCOMPARE(QFileInfo(linked).canonicalFilePath(), QFileInfo(QString::fromStdString(source_fonts())).canonicalFilePath());
        QVERIFY(step(linked));  // (again: left as it is)
        QVERIFY(QFileInfo(linked).isSymLink());
        QVERIFY(installed(linked));
#endif
        const QString copied = dir.path() + QStringLiteral("/b/share/genko-studio/fonts");
        QVERIFY(QDir().mkpath(copied));  // (a real folder is there already: no link over it)
        QVERIFY(step(copied));
        QVERIFY(!QFileInfo(copied).isSymLink());
        QStringList expected{QStringLiteral("FONTS.json")};
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.added) expected << QString::fromStdString(e.file);
        }
        expected.sort();
        QCOMPARE(QDir(copied).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name), expected);  // (not the six, the licences, the screen's fonts)
        for (const fonts::Entry& e : fonts::entries()) {
            if (e.added) QCOMPARE(QFileInfo(copied + "/" + QString::fromStdString(e.file)).size(), static_cast<qint64>(e.bytes));
        }
        QVERIFY(installed(copied));
        // (again: nothing is written over — whatever the files' times: a copy as old as its source, as a copy that keeps
        // its source's time is, or older than it, is not made again —; one taken away or cut short is put back, and the
        // catalogue when its words are not the source's)
        const QString klee = copied + QStringLiteral("/KleeOne-SemiBold.ttf"), stick = copied + QStringLiteral("/Stick-Regular.ttf");
        const QString dot = copied + QStringLiteral("/DotGothic16-Regular.ttf");
        const auto set_time = [](const QString& path, const QDateTime& when) {
            QFile file(path);
            return file.open(QIODevice::ReadWrite) && file.setFileTime(when, QFileDevice::FileModificationTime);
        };
        const QDateTime source_time = QFileInfo(QString::fromStdString(source_fonts()) + QStringLiteral("/KleeOne-SemiBold.ttf")).lastModified();
        QVERIFY(set_time(klee, source_time));
        const QDateTime long_ago = QDateTime::fromSecsSinceEpoch(946684800);  // (2000-01-01: older than any source)
        QVERIFY(set_time(dot, long_ago));
        QVERIFY(QFile::remove(stick));
        {
            QFile cut(copied + QStringLiteral("/YujiBoku-Regular.ttf"));
            QVERIFY(cut.resize(1000));
        }
        const std::string catalogue = genko::test::read_bytes(copied + QStringLiteral("/FONTS.json"));
        {
            std::string other = catalogue;
            other[other.size() / 2] = other[other.size() / 2] == 'x' ? 'y' : 'x';  // (the same size, other words)
            genko::test::write_bytes(copied + QStringLiteral("/FONTS.json"), other);
        }
        QVERIFY(step(copied));
        QCOMPARE(QFileInfo(klee).lastModified().toSecsSinceEpoch(), source_time.toSecsSinceEpoch());
        QCOMPARE(QFileInfo(dot).lastModified().toSecsSinceEpoch(), long_ago.toSecsSinceEpoch());
        QCOMPARE(QFileInfo(stick).size(), static_cast<qint64>(fonts::find("stick")->bytes));
        QCOMPARE(QFileInfo(copied + QStringLiteral("/YujiBoku-Regular.ttf")).size(), static_cast<qint64>(fonts::find("brush_boku")->bytes));
        QCOMPARE(genko::test::read_bytes(copied + QStringLiteral("/FONTS.json")), catalogue);
        QVERIFY(installed(copied));
    }

    // A font file the catalogue lists that is not in the source stops the configure of a package and the build tree's
    // step, by its name (cmake/GenkoFontList.cmake: what GenkoPackage.cmake and GenkoDevFonts.cmake both read the list
    // with) — not the install, later, or only a test. The source as it is passes.
    void aMissingFontFileStopsTheConfigure() {
        const QString script = genko::test::repo_root() + QStringLiteral("/native/cmake/GenkoFontList.cmake");
        const auto check = [&](const QString& folder) {
            return genko::test::run(QStringLiteral(GENKO_CMAKE_COMMAND), {QStringLiteral("-DGENKO_CHECK_FONTS=") + folder, QStringLiteral("-P"), script},
                                    QProcessEnvironment::systemEnvironment());
        };
        const genko::test::Run whole = check(QString::fromStdString(source_fonts()));
        QVERIFY2(whole.finished && whole.exit_code == 0, qPrintable(whole.described()));
        // (a folder with the catalogue and every file but one — the files' names are enough: nothing is read of them)
        QTemporaryDir part;
        QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + QStringLiteral("/FONTS.json"), part.path() + QStringLiteral("/FONTS.json")));
        for (const fonts::Entry& e : fonts::entries()) {
            if (!e.added || e.key == "klee") continue;
            QFile empty(part.path() + QLatin1Char('/') + QString::fromStdString(e.file));
            QVERIFY(empty.open(QIODevice::WriteOnly));
        }
        const genko::test::Run stopped = check(part.path());
        QVERIFY2(stopped.finished && stopped.exit_code != 0, qPrintable(stopped.described()));
        QVERIFY2(stopped.err.contains("a font file the catalogue lists is not there") && stopped.err.contains("KleeOne-SemiBold.ttf"),
                 qPrintable(stopped.described()));
        // (the build tree's step with that source, where it copies — a real folder is in the place —: stopped the same
        // way, nothing copied half)
        QVERIFY(QDir().mkpath(part.path() + QStringLiteral("/out/fonts")));
        const genko::test::Run step = genko::test::run(QStringLiteral(GENKO_CMAKE_COMMAND),
                                                       {QStringLiteral("-DGENKO_FONTS_SOURCE=") + part.path(),
                                                        QStringLiteral("-DGENKO_FONTS_DEST=") + part.path() + QStringLiteral("/out/fonts"), QStringLiteral("-P"),
                                                        genko::test::repo_root() + QStringLiteral("/native/cmake/GenkoDevFonts.cmake")},
                                                       QProcessEnvironment::systemEnvironment());
        QVERIFY2(step.finished && step.exit_code != 0, qPrintable(step.described()));
        QVERIFY(QDir(part.path() + QStringLiteral("/out/fonts")).entryList(QDir::Files).isEmpty());
        // (and a catalogue that is not there)
        QTemporaryDir none;
        const genko::test::Run bare = check(none.path());
        QVERIFY2(bare.finished && bare.exit_code != 0 && bare.err.contains("the font catalogue is not there"), qPrintable(bare.described()));
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
        genko::storage::write_atomic(genko::core::path_from_utf8(file.toStdString()), out);
    }

    // The sheets: every one of the forty and the seven of before, a line down — in a balloon, alone with readings, and
    // alone outlined —, each under its key; parts of them at 600 dpi; and the first sheets as a computer without the files
    // draws them.
    static void write_sample_sheets(const QByteArray& out) {
        const std::filesystem::path dir = genko::core::path_from_utf8(out.toStdString());
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
                Json op{{"op", "add_line"}, {"page", page}, {"id", "s" + std::to_string(i)}, {"balloon", kind == "plain" ? "speech" : "none"},
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
