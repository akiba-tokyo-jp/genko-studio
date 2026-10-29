// PC5 G-b (SPEC.md TEXT-02「OpenType の組み方」「右から左」, render/text/shaping, bidi, opentype; render/draw's glyphs by id):
// HarfBuzz's glyphs and advances with the test fonts (native/tests/data/type: Inter, OFL — ligatures, fractions, stylistic
// alternates, ordinals, kerning and the weight axis; Noto Sans Hebrew, OFL — a right-to-left script): a feature turned on
// and off, an axis set, a font's names, axes and features; SheenBidi's levels, a row's runs in the order they are drawn and
// the letters' scripts; a paragraph shaped run by run and a row's glyphs placed (a right-to-left run's letters right to
// left, a justified row's room, tracking); and the glyphs drawn by their ids (a ligature's ink, an axis's weight, a stroke).
// HarfBuzz's version is the system's: the ids are compared with what the same font gives, the advances within a pixel.

#include <QtTest>

#include <algorithm>
#include <string>
#include <vector>

#include "render/draw.hpp"
#include "render/image.hpp"
#include "render/text/bidi.hpp"
#include "render/text/fonts.hpp"
#include "render/text/opentype.hpp"
#include "render/text/shaping.hpp"
#include "testsupport.hpp"

namespace text = genko::render::text;
using genko::render::GlyphPlace;
using genko::render::GlyphShape;
using genko::render::Image;
using genko::render::Ink;
using genko::render::PointD;

namespace {

std::string font(const char* name) { return genko::test::test_data(QStringLiteral("type/%1").arg(QString::fromLatin1(name))).toStdString(); }
std::string inter() { return font("inter-subset.ttf"); }
std::string hebrew() { return font("noto-sans-hebrew-subset.ttf"); }

std::vector<text::ShapedGlyph> shaped(const std::string& words, const std::string& face, std::vector<std::pair<std::string, std::uint32_t>> features = {},
                                      std::vector<std::pair<std::string, float>> axes = {}, const std::string& direction = "") {
    text::ShapeRequest r;
    r.text = words;
    r.font = face;
    r.size_px = 100;
    r.direction = direction;
    r.features = std::move(features);
    r.variations = std::move(axes);
    return text::shape(r);
}

std::vector<std::uint32_t> ids(const std::vector<text::ShapedGlyph>& glyphs) {
    std::vector<std::uint32_t> out;
    for (const auto& g : glyphs) out.push_back(g.glyph);
    return out;
}

double width(const std::vector<text::ShapedGlyph>& glyphs) {
    double w = 0;
    for (const auto& g : glyphs) w += g.x_advance;
    return w;
}

std::int64_t ink(const Image& image) {
    std::int64_t sum = 0;
    const Image alpha = image.mode() == "L" ? image : image.getchannel(3);
    for (const char c : alpha.tobytes()) sum += static_cast<unsigned char>(c);
    return sum;
}

}  // namespace

class TestShaping : public QObject {
    Q_OBJECT

private slots:
    // 標準合字 (liga): "office" set with its ligature (fewer glyphs, its clusters kept) and without it (a glyph a letter)
    void ligatures() {
        const auto on = shaped("office", inter());
        const auto off = shaped("office", inter(), {{"liga", 0}});
        QCOMPARE(off.size(), std::size_t{6});
        QVERIFY2(on.size() < off.size(), std::to_string(on.size()).c_str());
        QCOMPARE(on.front().cluster, 0U);
        QCOMPARE(on.back().cluster, 5U);  // ("e", the last letter)
    }

    // 分数字・序数表記・デザインのバリエーション・前後関係に依存する字形: each changes the glyphs only where it is on
    void features() {
        QVERIFY(ids(shaped("1/2", inter(), {{"frac", 1}})) != ids(shaped("1/2", inter())));
        QVERIFY(ids(shaped("2nd", inter(), {{"ordn", 1}})) != ids(shaped("2nd", inter())));
        QVERIFY(ids(shaped("a", inter(), {{"salt", 1}})) != ids(shaped("a", inter())) ||
                ids(shaped("a", inter(), {{"ss01", 1}})) != ids(shaped("a", inter())));
        QCOMPARE(ids(shaped("xyz", inter(), {{"frac", 1}})), ids(shaped("xyz", inter())));  // (nothing to make a fraction of)
        // kerning (kern) on by default, off when asked: "AV" closer with it
        QVERIFY(width(shaped("AV", inter())) < width(shaped("AV", inter(), {{"kern", 0}})) - 0.5);
    }

    // 可変軸 (variations): the weight axis widens the letters' advances
    void axes() {
        const double thin = width(shaped("mono", inter(), {}, {{"wght", 100}}));
        const double black = width(shaped("mono", inter(), {}, {{"wght", 900}}));
        QVERIFY2(black > thin + 1, (std::to_string(thin) + " " + std::to_string(black)).c_str());
        // (an axis the font has not: as without it)
        QCOMPARE(width(shaped("mono", inter(), {}, {{"XXXX", 5}})), width(shaped("mono", inter())));
    }

    // type.fonts: the font's family, style, PostScript name, its axes and the features its GSUB and GPOS hold
    void fontInfo() {
        const text::FontInfo info = text::font_info(inter());
        QCOMPARE(QString::fromStdString(info.family), QStringLiteral("Inter"));
        QVERIFY(!info.postscript.empty());
        bool weight = false;
        for (const text::FontAxis& a : info.axes) {
            if (a.tag == "wght") {
                weight = true;
                QCOMPARE(a.min, 100.0);
                QCOMPARE(a.max, 900.0);
            }
        }
        QVERIFY(weight);
        for (const char* tag : {"liga", "frac", "calt", "kern", "ordn"}) QVERIFY2(std::binary_search(info.features.begin(), info.features.end(), std::string(tag)), tag);
        QVERIFY(std::is_sorted(info.features.begin(), info.features.end()));
        QCOMPARE(info.faces, 1U);
        // a bundled font: its names, no axes
        const text::FontInfo gothic = text::font_info("gothic");
        QVERIFY(!gothic.family.empty());
        QVERIFY(gothic.axes.empty());
    }

    // 右から左: SheenBidi's levels — Hebrew odd, Latin even; the base from the first strong letter (auto) or as asked
    void levels() {
        const std::u32string mixed = U"abc אבג def";
        const text::BidiParagraph ltr(mixed, "auto");
        QCOMPARE(int(ltr.base()), 0);
        QVERIFY(ltr.mixed());
        QCOMPARE(int(ltr.levels()[0]), 0);
        QCOMPARE(int(ltr.levels()[4]), 1);
        const text::BidiParagraph rtl(U"אבג abc", "auto");
        QCOMPARE(int(rtl.base()), 1);
        QCOMPARE(int(rtl.levels()[4]), 2);
        QCOMPARE(int(text::BidiParagraph(U"abc", "rtl").base()), 1);
        QCOMPARE(int(text::BidiParagraph(U"אבג", "ltr").base()), 0);
        QVERIFY(!text::BidiParagraph(U"漢字かな", "auto").mixed());
    }

    // a row's runs left to right: in a right-to-left paragraph the Latin word is drawn at the left, the Hebrew to its right
    void visualRuns() {
        const text::BidiParagraph p(U"אבג abc", "auto");
        const auto runs = p.visual_runs(0, 7);
        QVERIFY(runs.size() >= 2);
        QCOMPARE(runs.front().start, std::size_t{4});  // ("abc")
        QCOMPARE(runs.front().end, std::size_t{7});
        QCOMPARE(runs.back().start, std::size_t{0});
        QVERIFY((runs.back().level & 1) != 0);
        // a left-to-right paragraph of Latin: one run
        const auto plain = text::BidiParagraph(U"plain words", "auto").visual_runs(0, 11);
        QCOMPARE(plain.size(), std::size_t{1});
        // part of the paragraph (a row): only its letters
        const auto part = p.visual_runs(4, 7);
        QCOMPARE(part.size(), std::size_t{1});
        QCOMPARE(part.front().start, std::size_t{4});
    }

    void scripts() {
        const auto s = text::scripts_of(U"aא漢 1");
        QCOMPARE(QString::fromStdString(s[0]), QStringLiteral("Latn"));
        QCOMPARE(QString::fromStdString(s[1]), QStringLiteral("Hebr"));
        QCOMPARE(QString::fromStdString(s[2]), QStringLiteral("Hani"));
        QCOMPARE(text::sheenbidi_version(), std::string("2.9.0"));
    }

    // Hebrew shaped right to left: the glyphs in the order they are drawn (the last letter first)
    void rightToLeftShaped() {
        const auto g = shaped("אבג", hebrew(), {}, {}, "rtl");
        QCOMPARE(g.size(), std::size_t{3});
        QCOMPARE(g.front().cluster, 2U);
        QCOMPARE(g.back().cluster, 0U);
    }

    // a paragraph shaped run by run (a Latin and a Hebrew font), its row's glyphs placed left to right
    void paragraphAndRow() {
        std::vector<text::Look> looks(2);
        looks[0].font = inter();
        looks[0].size = 100;
        looks[1].font = hebrew();
        looks[1].size = 100;
        const std::u32string words = U"ab אב";
        const std::vector<std::size_t> look_of{0, 0, 0, 1, 1};
        const text::ShapedParagraph p = text::shape_paragraph(words, looks, look_of, "ltr");
        QCOMPARE(p.runs.size(), std::size_t{3});  // ("ab", " " — another script's —, "אב": each its own run)
        for (bool s : p.starts) QVERIFY(s);
        const text::RowGlyphs row = text::row_glyphs(p, 0, words.size());
        QCOMPARE(row.glyphs.size(), std::size_t{5});
        // (left to right: a, b, the space, then the Hebrew drawn right to left — its second letter at the left)
        QVERIFY(row.span[0].first < row.span[1].first);
        QVERIFY(row.span[4].first < row.span[3].first);
        QVERIFY(row.span[1].second <= row.span[4].first + 0.01);
        double sum = 0;
        for (double a : p.advance) sum += a;
        QVERIFY(std::abs(sum - row.width) < 0.01);
        // a justified row: the room shared between its clusters (none after the last)
        const text::RowGlyphs wide = text::row_glyphs(p, 0, words.size(), 10.0);
        QVERIFY(std::abs(wide.width - (row.width + 40.0)) < 0.01);
        // tracking: each cluster's look's, after it
        looks[0].tracking = 5;
        const text::ShapedParagraph tracked = text::shape_paragraph(U"ab", looks, {0, 0}, "ltr");
        const text::ShapedParagraph plain = text::shape_paragraph(U"ab", std::vector<text::Look>{looks[1], looks[1]}, {0, 0}, "ltr");
        (void)plain;
        const text::RowGlyphs t = text::row_glyphs(tracked, 0, 2);
        looks[0].tracking = 0;
        const text::RowGlyphs u = text::row_glyphs(text::shape_paragraph(U"ab", looks, {0, 0}, "ltr"), 0, 2);
        QVERIFY(std::abs(t.width - (u.width + 10.0)) < 0.01);
    }

    // glyphs drawn by id: a ligature's picture differs from its letters', the heavier axis draws more ink, a stroke widens it
    void drawnById() {
        genko::render::TrueTypeFonts fonts;
        const genko::render::TrueTypeFont& face = fonts.truetype(inter(), 60);
        const auto place = [](const std::vector<text::ShapedGlyph>& glyphs) {
            std::vector<GlyphPlace> out;
            double x = 0;
            for (const auto& g : glyphs) {
                out.push_back(GlyphPlace{g.glyph, x + g.x_offset * 0.6, -g.y_offset * 0.6});
                x += g.x_advance * 0.6;
            }
            return out;
        };
        const auto lig = face.glyph_mask(place(shaped("fi", inter())));
        const auto apart = face.glyph_mask(place(shaped("fi", inter(), {{"liga", 0}})));
        QVERIFY(lig.first.width() > 0 && apart.first.width() > 0);
        QVERIFY(lig.first.tobytes() != apart.first.tobytes() || lig.first.size() != apart.first.size());
        const genko::render::TrueTypeFont& heavy = fonts.truetype(inter(), 60, {{"wght", 900}});
        const genko::render::TrueTypeFont& light = fonts.truetype(inter(), 60, {{"wght", 100}});
        const std::vector<GlyphPlace> o{GlyphPlace{heavy.glyph_index(U'o'), 0, 0}};
        QVERIFY(ink(heavy.glyph_mask(o).first) > ink(light.glyph_mask(o).first) * 2);
        QVERIFY(ink(face.glyph_mask(o, 3).first) > ink(face.glyph_mask(o).first));
        // its box from the origin on the baseline: the letter above it
        const auto [mask, at] = face.glyph_mask(o);
        QVERIFY(at.y < 0 && at.y + mask.height() <= 2);
        // scaled to half its height, leant: less ink, the same glyph
        GlyphShape half;
        half.scale_y = 0.5;
        QVERIFY(face.glyph_mask(o, 0, half).first.height() < mask.height() * 0.7);
        // on a picture: drawn as text() draws letters (the ink in the fill's colour)
        Image canvas = Image::create("RGBA", {200, 100}, Ink{0, 0, 0, 0});
        {
            genko::render::Draw draw(canvas);
            draw.glyphs(PointD{10.5, 70.25}, o, face, Ink::with_alpha(std::vector<std::int64_t>{200, 0, 0}, 255));
        }
        QVERIFY(canvas.getbbox().has_value());
        QVERIFY(face.lines().underline > 0);
        QVERIFY(face.lines().strikeout > 0);
    }
};

QTEST_GUILESS_MAIN(TestShaping)
#include "test_shaping.moc"
