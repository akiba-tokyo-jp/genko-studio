// PSD-01 (AC-PSD as ACCEPTANCE.md §6 has it, the structure checks with this build's own reader; the same files read by
// psd-tools are test_contract_psd_text's): the editable PSD. Photoshop's text structures written and read back (the
// TySh block, its descriptor and EngineData, the lfx2 stroke), the PSD book (data/psd_text_book.json, F4's kinds of
// lines: down and across, 「東京と東京」 with a reading on each, styled parts, 縦中横, several rubies, a reading broken
// over two columns, effect letters, turned, narrowed and leant lines, balloons, the nombre) written editable and read
// back: each line a group of its own with its balloon, its text as a text layer — the characters as set, a line for
// each column or row, each character's font (the PostScript name, never the file), size, weight, slant, colour, the
// spacing, the alignment and where it lies and how it is moved, turned or narrowed, an outline as the layer's stroke —
// and each ruby as a text layer of its own tied to the text by its name; the merged picture as the image way's. What
// PSD text cannot keep stops the strict export before anything is written and is drawn as a picture only when asked;
// the image way is Python's file; the command line's --psd-text; the support table that goes out with the docs.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "formats/psd_text.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/psd_text.hpp"
#include "render/text/lettering.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::render::Image;
namespace psd = genko::render::psd;

namespace {

constexpr int kDpi = 150;

const CommandBus& bus() {
    static const CommandBus b(genko::render::ops_registry());
    return b;
}

Document run_ops(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")).doc; }

// The PSD book: four A4 pages (as `genko new --pages 4` makes them) and the ops of data/psd_text_book.json.
Document psd_book() {
    const Document blank = genko::core::new_episode("PSD文字", genko::core::Num(1), 4, genko::core::PageSpec::a4_mono());
    return run_ops(blank, genko::test::read_json(genko::test::test_data("psd_text_book.json")));
}

const genko::core::StoryLine& line_of(const Document& doc, const std::string& id) {
    for (const auto& line : doc.story) {
        if (line.id == id) return line;
    }
    qFatal("no line %s", id.c_str());
}

std::u16string u16(const std::string& utf8) { return psd::utf16(utf8); }

// One PSD read back: its layers, and for each its folder's name and its text / stroke.
struct Read {
    psd::File file;
    std::vector<psd::Layer> layers;

    const psd::Layer* named(const std::string& name) const {
        for (const auto& layer : layers) {
            if (layer.name == name) return &layer;
        }
        return nullptr;
    }
    std::string folder_of(const psd::Layer& layer) const { return layer.parent ? layers.at(*layer.parent).name : std::string(); }
    std::vector<const psd::Layer*> in(const std::string& folder) const {
        std::vector<const psd::Layer*> out;
        for (const auto& layer : layers) {
            if (folder_of(layer) == folder) out.push_back(&layer);
        }
        return out;
    }
};

Read read_psd(const std::string& path) {
    Read r{psd::File::read(genko::test::read_bytes(QString::fromStdString(path))), {}};
    r.layers = r.file.layers();
    return r;
}

// The box of a picture's ink (alpha > 40) inside `b`: how many pixels.
int ink_in(const Image& image, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(image.height(), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(image.width(), x1); ++x) {
            if (image.getpixel(x, y).back() > 40) ++n;
        }
    }
    return n;
}
int ink_in(const Image& image, const genko::render::Box& b) { return ink_in(image, b.x0, b.y0, b.x1, b.y1); }

bool near(double a, double b, double within) { return std::fabs(a - b) <= within; }

// The JSON a command printed last on a stream (stderr may hold Qt's words about the locale before it), as an object.
Json printed(const QByteArray& stream) {
    QList<QByteArray> lines = stream.trimmed().split('\n');
    const Json value = genko::test::one_line(lines.isEmpty() ? QByteArray() : lines.back() + '\n');
    return value.is_object() ? value : Json{{"not JSON", value}};
}

std::string fonts_of(const psd::TextLayer& text) {
    std::string out;
    for (const auto& run : text.runs) out += (out.empty() ? "" : ",") + run.style.font;
    return out;
}

constexpr const char* kGothic = "ZenKakuGothicNew-Bold";
constexpr const char* kMincho = "ZenOldMincho-SemiBold";
constexpr const char* kMaru = "ZenMaruGothic-Bold";
constexpr const char* kSfx = "DelaGothicOne-Regular";

}  // namespace

class TestPsdText : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    std::string path(const std::string& rel) const { return tmp_.path().toStdString() + "/" + rel; }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // The book is the one MANIFEST.json fixes.
    void bookIsFixed() {
        QFile file(genko::test::test_data("psd_text_book.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const Json manifest = genko::test::read_json(genko::test::test_data("MANIFEST.json"));
        QCOMPARE(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex().toStdString(),
                 manifest.at("psd_text_book.json").get<std::string>());
    }

    // TySh and lfx2 written and read back: the text (brackets, a backslash, ぜ and 小 whose UTF-16 holds the bytes the
    // text engine escapes, a character past the BMP, a variation selector), the runs, the paragraphs, the transform.
    void structuresReadBack() {
        psd::TextLayer layer;
        layer.text = u16("（括弧）(x)\\ぜ小𠮷葛\xf3\xa0\x84\x80\r二行目");
        layer.vertical = true;
        layer.transform = {0.9659258262890683, 0.25881904510252074, -0.25881904510252074, 0.9659258262890683, 123.5, -7.25};
        psd::TextStyle a;
        a.font = kGothic;
        a.size = 31;
        a.fill = {10, 10, 10};
        a.tracking = 100;
        a.leading = 44.5;
        psd::TextStyle b = a;
        b.font = kMincho;
        b.faux_bold = true;
        b.faux_italic = true;
        b.fill = {200, 30, 30};
        b.baseline_direction = 3;
        b.horizontal_scale = 0.8;
        b.size = 43;
        const std::size_t n = layer.text.size() + 1;
        layer.runs = {{5, a}, {n - 5, b}};
        layer.paragraphs = {{layer.text.find(u'\r') + 1, 2}, {n - layer.text.find(u'\r') - 1, 6}};
        layer.box = std::array<double, 4>{0, 0, 120, 340};
        layer.bounds = {0, 0, 120, 340};
        const psd::TextLayer back = psd::read_tysh(psd::tysh(layer));
        QVERIFY(back.text == layer.text);
        QVERIFY(back.engine_text == layer.text + u"\r");
        QVERIFY(back.vertical);
        QVERIFY(back.transform == layer.transform);
        QCOMPARE(back.runs.size(), std::size_t{2});
        QVERIFY(back.runs == layer.runs);
        QVERIFY(back.paragraphs == layer.paragraphs);
        QVERIFY(back.box.has_value() && *back.box == *layer.box);
        QCOMPARE(back.warp, std::string("warpNone"));
        QCOMPARE(back.style_at(0)->font, std::string(kGothic));
        QCOMPARE(back.style_at(6)->font, std::string(kMincho));
        QVERIFY(back.style_at(n) == nullptr);
        psd::TextLayer across;
        across.text = u"ドーン";
        across.runs = {{4, a}};
        across.paragraphs = {{4, 0}};
        const psd::TextLayer point = psd::read_tysh(psd::tysh(across));
        QVERIFY(!point.vertical);
        QVERIFY(!point.box.has_value());
        const psd::Stroke stroke{6, {255, 255, 255}, "outside", 100, true};
        QCOMPARE(psd::read_lfx2(psd::lfx2(stroke)).value(), stroke);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, psd::read_tysh("not a type layer"));
    }

    // The image way (the default) is Python's PSD: no text layers, nothing in a group.
    void imageWayIsAsBefore() {
        const Document doc = psd_book();
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("image")), kDpi);
        QCOMPARE(files.size(), std::size_t{4});
        const auto again = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("image-again")), kDpi, genko::formats::PsdText{});
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(files[i].string()));
            QVERIFY(bytes == genko::test::read_bytes(QString::fromStdString(again[i].string())));
            QVERIFY(bytes.find("TySh") == std::string::npos);
            QVERIFY(bytes.find("lsct") == std::string::npos);
            const Read r = read_psd(files[i].string());
            for (const auto& layer : r.layers) QVERIFY(!r.file.text(layer).has_value());
        }
    }

    // The book written editable and read back with this build's reader.
    void editableBook() {
        const Document doc = psd_book();
        genko::formats::PsdTextReport report;
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("editable")), kDpi,
                                                            genko::formats::PsdText{true, false}, &report);
        QCOMPARE(files.size(), std::size_t{4});
        QVERIFY(report.rasterized.empty());
        QCOMPARE(report.text_layers, std::int64_t{19});
        QCOMPARE(report.ruby_layers, std::int64_t{13});
        for (const char* font : {kGothic, kMincho, kMaru, kSfx}) {
            QVERIFY2(std::find(report.fonts.begin(), report.fonts.end(), font) != report.fonts.end(), font);
        }
        const auto image_way = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("as-images")), kDpi);
        std::vector<Read> pages;
        for (std::size_t i = 0; i < files.size(); ++i) {
            pages.push_back(read_psd(files[i].string()));
            // the merged picture is the page as drawn, the same as the image way's
            const Read pictures = read_psd(image_way[i].string());
            QVERIFY(pages.back().file.merged()->tobytes() == pictures.file.merged()->tobytes());
            QCOMPARE(pages.back().file.dpi(), double{kDpi});
            // no font file in the PSD
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(files[i].string()));
            QVERIFY(bytes.find("glyf") == std::string::npos && bytes.find("OTTO") == std::string::npos);
        }
        const auto text_of = [&](const Read& r, const std::string& name) -> psd::TextLayer {
            const psd::Layer* layer = r.named(name);
            if (layer == nullptr) qFatal("no layer %s", name.c_str());
            const auto text = r.file.text(*layer);
            if (!text) qFatal("no text in %s", name.c_str());
            return *text;
        };
        const auto em_of = [&](const std::string& id) { return genko::render::text::text_layout(line_of(doc, id), kDpi).em; };
        const auto places = [&](const std::string& id) { return genko::render::text::ruby_places(line_of(doc, id), kDpi); };

        // page 1, down: 「東京と東京」, a reading beside each 東京, in the line's group with its balloon
        const Read& p1 = pages[0];
        const psd::Layer* body = p1.named("本文 #p1a");
        QVERIFY(body != nullptr);
        QCOMPARE(body->kind, std::string("text"));
        QCOMPARE(p1.folder_of(*body), std::string("台詞 東京と東京"));
        std::vector<std::string> names;
        for (const psd::Layer* layer : p1.in("台詞 東京と東京")) names.push_back(layer->name);
        QCOMPARE(names, (std::vector<std::string>{"フキダシ", "本文 #p1a", "ルビ「とうきょう」 #p1a-r0", "ルビ「とうきょう」 #p1a-r1"}));
        QVERIFY(!p1.file.text(*p1.named("フキダシ")).has_value());
        psd::TextLayer t = text_of(p1, "本文 #p1a");
        QVERIFY(t.text == u"東京と東京");
        QVERIFY(t.engine_text == u"東京と東京\r");
        QVERIFY(t.vertical);
        QVERIFY(!t.box);
        // the face アンチック: kana in the Mincho, kanji in the Gothic, each run named by its PostScript name
        QCOMPARE(fonts_of(t), std::string(kGothic) + "," + kMincho + "," + kGothic);
        QCOMPARE(t.runs[0].length + t.runs[1].length + t.runs[2].length, std::size_t{6});
        const std::int64_t em = em_of("p1a");
        for (const auto& run : t.runs) {
            QCOMPARE(run.style.size, double(em));
            QVERIFY(!run.style.faux_bold && !run.style.faux_italic);
            QVERIFY((run.style.fill == std::array<int, 3>{10, 10, 10}));
            QCOMPARE(run.style.tracking, 0.0);
            QCOMPARE(run.style.baseline_direction, 2);
        }
        QCOMPARE(t.paragraphs.size(), std::size_t{1});
        QCOMPARE(t.paragraphs[0].justification, 0);
        // the text's anchor: its column's middle line, at the top of its first cell (the first reading's base there)
        const auto p1a = places("p1a");
        QVERIFY(p1a.exact);
        QCOMPARE(p1a.places.size(), std::size_t{2});
        QVERIFY2(near(t.transform[4], (p1a.places[0].base.x0 + p1a.places[0].base.x1) / 2.0, 1.0), std::to_string(t.transform[4]).c_str());
        QVERIFY2(near(t.transform[5], p1a.places[0].base.y0, 1.0), std::to_string(t.transform[5]).c_str());
        QVERIFY(t.transform[0] == 1.0 && t.transform[1] == 0.0 && t.transform[2] == 0.0 && t.transform[3] == 1.0);
        // each reading a text layer of its own beside its own word (the second beside the second 東京, not the first)
        for (std::size_t i = 0; i < 2; ++i) {
            const psd::TextLayer ruby = text_of(p1, "ルビ「とうきょう」 #p1a-r" + std::to_string(i));
            QVERIFY(ruby.text == u"とうきょう");
            QVERIFY(ruby.vertical);
            QCOMPARE(ruby.runs.front().style.size, double(p1a.places[i].size));
            QCOMPARE(ruby.runs.front().style.font, std::string(kMincho));
            QVERIFY(near(ruby.transform[4], (p1a.places[i].ruby.x0 + p1a.places[i].ruby.x1) / 2.0, 1.0));
            QVERIFY(near(ruby.transform[5], p1a.places[i].ruby.y0, 1.0));
            // its pixels: the reading only (the text's layer has none of it)
            const Image ruby_picture = p1.file.image(*p1.named("ルビ「とうきょう」 #p1a-r" + std::to_string(i)));
            QVERIFY(ink_in(ruby_picture, p1a.places[i].ruby) > 20);
            QCOMPARE(ink_in(ruby_picture, p1a.places[i].base), 0);
            QCOMPARE(ink_in(p1.file.image(*body), p1a.places[i].ruby), 0);
            QVERIFY(ink_in(p1.file.image(*body), p1a.places[i].base) > 20);
        }
        QVERIFY(p1a.places[1].base.y0 > p1a.places[0].base.y1);
        // the shout: its columns as broken, its closing 。 left off as drawn
        t = text_of(p1, "本文 #p1b");
        QVERIFY(t.text.find(u'\r') != std::u16string::npos);
        QVERIFY(t.paragraphs.size() >= 2);
        std::u16string flat;
        for (const char16_t c : t.text) {
            if (c != u'\r') flat.push_back(c);
        }
        QVERIFY(flat == u"「そうだ。」っていうか、ちょっと待って…！");
        // 縦中横: 「AI」 and 「12」 set across in their cells
        t = text_of(p1, "本文 #p1c");
        QVERIFY(t.text == u"AIと12時");
        for (const std::size_t at : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}}) QCOMPARE(t.style_at(at)->baseline_direction, 3);
        QCOMPARE(t.style_at(2)->baseline_direction, 2);
        QCOMPARE(t.style_at(5)->baseline_direction, 2);
        // the styled parts: 「大きな」 larger, 「声」 bold and red; two readings
        t = text_of(p1, "本文 #p1d");
        const std::int64_t em_d = em_of("p1d");
        QCOMPARE(t.style_at(0)->size, double(std::llround(em_d * 1.4)));
        QCOMPARE(t.style_at(2)->size, double(std::llround(em_d * 1.4)));
        QCOMPARE(t.style_at(3)->size, double(em_d));
        QVERIFY(t.style_at(3)->faux_bold);
        QVERIFY((t.style_at(3)->fill == std::array<int, 3>{200, 30, 30}));
        QVERIFY(!t.style_at(4)->faux_bold);
        QVERIFY((t.style_at(4)->fill == std::array<int, 3>{10, 10, 10}));
        QVERIFY(text_of(p1, "ルビ「こえ」 #p1d-r0").text == u"こえ");
        QVERIFY(text_of(p1, "ルビ「やくそく」 #p1d-r1").text == u"やくそく");
        // a line of Python's (ruby_runs): its reading a layer too; the spacing and the columns' distance
        t = text_of(p1, "本文 #p1e");
        const std::int64_t em_e = em_of("p1e");
        QVERIFY(near(t.runs.front().style.tracking, std::llround(em_e * 0.1) * 1000.0 / double(em_e), 1e-9));
        QVERIFY(text_of(p1, "ルビ「かんじ」 #p1e-r0").vertical);
        // page 2, across: variation selectors kept, a reading in the Mincho from the head, one to the tail
        const Read& p2 = pages[1];
        t = text_of(p2, "本文 #p2a");
        QVERIFY(!t.vertical);
        QVERIFY(t.text == u16("葛\xf3\xa0\x84\x80飾区と辻\xef\xb8\x80堂へ"));
        psd::TextLayer ruby = text_of(p2, "ルビ「かつしかく」 #p2a-r0");
        QVERIFY(!ruby.vertical);
        QCOMPARE(fonts_of(ruby), std::string(kMincho));
        const auto p2a = places("p2a");
        QVERIFY(near(ruby.transform[4], p2a.places[0].letters[0].x, 1.0));
        QVERIFY(ruby.transform[5] > p2a.places[0].ruby.y0 && ruby.transform[5] < p2a.places[0].ruby.y1);  // (its baseline)
        QVERIFY(text_of(p2, "ルビ「つじどう」 #p2a-r1").text == u"つじどう");
        // two rows to the right, leant (italic), spaced 0.1 em
        t = text_of(p2, "本文 #p2b");
        QVERIFY(t.text == u"横書きの台詞\r右に揃えて");
        QCOMPARE(t.paragraphs.size(), std::size_t{2});
        QCOMPARE(t.paragraphs[0].justification, 1);
        const std::int64_t em_b = em_of("p2b");
        for (const auto& run : t.runs) {
            QVERIFY(run.style.faux_italic);
            QVERIFY(near(run.style.tracking, 100.0, 1e-9));
        }
        QVERIFY(t.style_at(8)->leading > double(em_b));  // (the second row's baseline below the first's)
        QVERIFY(text_of(p2, "ルビ「せりふ」 #p2b-r0").text == u"せりふ");
        t = text_of(p2, "本文 #p2c");
        QVERIFY(t.style_at(0)->faux_bold);
        QVERIFY((t.style_at(0)->fill == std::array<int, 3>{30, 90, 200}));
        QCOMPARE(t.style_at(3)->size, double(std::llround(em_of("p2c") * 1.3)));
        t = text_of(p2, "本文 #p2d");
        QCOMPARE(t.paragraphs[0].justification, 0);
        QVERIFY(t.text == u"左に揃える行\rふたつめ");
        QCOMPARE(p2.folder_of(*p2.named("本文 #p2d")), std::string("台詞 左に揃える行ふたつめ"));
        QVERIFY(p2.named("フキダシ") != nullptr);
        // page 3: effect letters (fill and a single-colour outline as the layer's stroke), turned, narrowed, leant
        const Read& p3 = pages[2];
        t = text_of(p3, "本文 #p3a");
        QCOMPARE(fonts_of(t), std::string(kSfx));
        QVERIFY((t.runs.front().style.fill == std::array<int, 3>{200, 30, 30}));
        QCOMPARE(p3.file.stroke(*p3.named("本文 #p3a")).value(), (psd::Stroke{6, {255, 255, 255}, "outside", 100, true}));
        QVERIFY(p3.in("台詞 ドーン").size() == 1);  // (no balloon)
        QCOMPARE(p3.file.stroke(*p3.named("本文 #p3e")).value(), (psd::Stroke{5, {20, 20, 20}, "outside", 100, true}));
        QVERIFY((text_of(p3, "本文 #p3e").runs.front().style.fill == std::array<int, 3>{255, 255, 255}));
        QVERIFY(!p3.file.stroke(*p3.named("本文 #p3b")).has_value());
        const double c15 = std::cos(15 * M_PI / 180), s15 = std::sin(15 * M_PI / 180);
        for (const char* name : {"本文 #p3b", "ルビ「まわ」 #p3b-r0"}) {
            t = text_of(p3, name);
            QVERIFY2(near(t.transform[0], c15, 1e-9) && near(t.transform[1], s15, 1e-9) && near(t.transform[2], -s15, 1e-9) &&
                         near(t.transform[3], c15, 1e-9),
                     name);
        }
        t = text_of(p3, "本文 #p3c");
        QVERIFY2(near(t.transform[0], 0.8, 0.02) && t.transform[1] == 0.0 && t.transform[2] == 0.0 && t.transform[3] == 1.0,
                 std::to_string(t.transform[0]).c_str());
        t = text_of(p3, "本文 #p3d");
        QVERIFY(near(t.transform[2], -std::tan(10 * M_PI / 180), 1e-9) && t.transform[0] == 1.0 && t.transform[3] == 1.0);
        QVERIFY(!t.runs.front().style.faux_italic);
        // nothing of the draft or the layer that is not written
        for (const auto& layer : p3.layers) QVERIFY(layer.name.find("hidden") == std::string::npos);
        // page 4: a reading broken over two columns (a layer for each part), each ruby its own font and spacing, the
        // columns justified (area text), across justified, down to the foot
        const Read& p4 = pages[3];
        QVERIFY(text_of(p4, "ルビ「えき」 #p4a-r0.1").text == u"えき");
        QVERIFY(text_of(p4, "ルビ「まえ」 #p4a-r0.2").text == u"まえ");
        ruby = text_of(p4, "ルビ「たてがき」 #p4b-r0");
        QVERIFY(near(ruby.runs.front().style.tracking, 300.0, 1000.0 / ruby.runs.front().style.size));
        QCOMPARE(fonts_of(text_of(p4, "ルビ「よこがき」 #p4b-r1")), std::string(kMaru));
        t = text_of(p4, "本文 #p4c");
        QVERIFY(t.box.has_value());
        QCOMPARE(t.paragraphs[0].justification, 6);
        t = text_of(p4, "本文 #p4d");
        QVERIFY(t.box.has_value());
        QCOMPARE(t.paragraphs[0].justification, 3);
        t = text_of(p4, "本文 #p4e");
        QVERIFY(!t.box.has_value());
        QCOMPARE(t.paragraphs[0].justification, 1);
        QVERIFY(t.text == u"下に\r揃える行");
        // the nombre on every page, outside the lines' groups
        for (const Read& r : pages) {
            const psd::Layer* nombre = r.named("ノンブル");
            QVERIFY(nombre != nullptr);
            QVERIFY(!nombre->parent.has_value());
            // every text layer has its letters' pixels and sits in a line's group
            for (const auto& layer : r.layers) {
                if (layer.kind != "text") continue;
                QVERIFY2(r.folder_of(layer).rfind("台詞 ", 0) == 0, layer.name.c_str());
                const Image picture = r.file.image(layer);
                QVERIFY2(ink_in(picture, 0, 0, picture.width(), picture.height()) > 0, layer.name.c_str());
            }
        }
    }

    // What PSD text cannot keep: listed before anything is written; the strict way stops (nothing written), the
    // appearance way draws those lines as pictures (as the image way does) and keeps the others as text.
    void strictStopsAndAppearanceRasterizes() {
        const std::string png = genko::render::write_png(Image::create("RGBA", {4, 4}, genko::render::Ink{200, 30, 30, 255}));
        Document doc = genko::core::new_episode("装飾", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono());
        Json ops = Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "ok", "text": "ふつうの台詞", "x_mm": 140, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical"},
            {"op": "add_line", "page": 1, "id": "grad", "text": "グラデ", "x_mm": 100, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "style": {"gradient": {"rgb_from": [0, 0, 0], "rgb_to": [200, 0, 0]}}},
            {"op": "add_line", "page": 1, "id": "path", "text": "道の上", "x_mm": 20, "y_mm": 200, "w_mm": 80, "h_mm": 20, "balloon": "none",
             "style": {"text_path": [[0, 10], [80, 10]]}},
            {"op": "add_line", "page": 2, "id": "arc", "text": "曲がる", "x_mm": 20, "y_mm": 20, "w_mm": 60, "h_mm": 20, "wrap": "horizontal",
             "balloon": "none", "style": {"arc": 0.5}},
            {"op": "add_line", "page": 2, "id": "warp", "text": "ゆがむ", "x_mm": 20, "y_mm": 60, "w_mm": 60, "h_mm": 20, "wrap": "horizontal",
             "balloon": "none", "style": {"warp": [[0, 0], [1, 0.1], [1, 0.9], [0, 1]]}},
            {"op": "add_line", "page": 2, "id": "dots", "text": "大事な約束", "x_mm": 140, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "emphasis_runs": ["約束"]}])");
        ops.push_back(Json{{"op", "add_line"}, {"page", 2}, {"id", "fill"}, {"text", "絵の字"}, {"x_mm", 20}, {"y_mm", 100}, {"w_mm", 60},
                           {"h_mm", 20}, {"wrap", "horizontal"}, {"balloon", "none"},
                           {"style", Json{{"fill_png", genko::core::b64encode(png)}}}});
        doc = run_ops(doc, ops);
        const auto issues = genko::formats::psd_text_issues(doc);
        Json listed = genko::formats::issues_json(issues);
        QCOMPARE(listed, Json::parse(R"([
            {"page": 1, "line": "grad", "text": "グラデ", "features": ["gradient"]},
            {"page": 1, "line": "path", "text": "道の上", "features": ["text_path"]},
            {"page": 2, "line": "arc", "text": "曲がる", "features": ["arc"]},
            {"page": 2, "line": "warp", "text": "ゆがむ", "features": ["warp"]},
            {"page": 2, "line": "dots", "text": "大事な約束", "features": ["emphasis"]},
            {"page": 2, "line": "fill", "text": "絵の字", "features": ["picture_fill"]}])"));
        for (const auto& issue : issues) {
            for (const auto& key : issue.features) {
                const auto& table = genko::formats::psd_text_unsupported();
                QVERIFY2(std::any_of(table.begin(), table.end(), [&](const auto& f) { return f.key == key; }), key.c_str());
            }
        }
        // strict: refused before anything is drawn or written
        const auto strict_out = genko::core::path_from_utf8(path("strict"));
        try {
            (void)genko::formats::export_psd_pages(doc, strict_out, kDpi, genko::formats::PsdText{true, false});
            QFAIL("not refused");
        } catch (const genko::formats::PsdTextRefused& e) {
            QCOMPARE(std::string(e.code()), std::string("psd_text_unsupported"));
            QCOMPARE(e.issues().size(), std::size_t{6});
            QVERIFY(std::string(e.what()).rfind("the editable PSD stops: 6 lines use what PSD text cannot keep", 0) == 0);
        }
        QVERIFY(!QDir(QString::fromStdString(strict_out.string())).exists() || QDir(QString::fromStdString(strict_out.string())).isEmpty());
        // formats::run says the same (the app and the command line)
        genko::formats::RunOptions options;
        options.dpi = kDpi;
        options.psd_text = "editable";
        Json reply = genko::formats::run(doc, std::nullopt, "psd", genko::core::path_from_utf8(path("strict-run")), options);
        QCOMPARE(reply["ok"], Json(false));
        QCOMPARE(reply["code"], Json("psd_text_unsupported"));
        QCOMPARE(reply["items"], listed);
        // appearance first: those lines as pictures, the others text
        options.psd_rasterize = true;
        reply = genko::formats::run(doc, std::nullopt, "psd", genko::core::path_from_utf8(path("rasterized")), options);
        QVERIFY2(reply["ok"] == Json(true), reply.dump().c_str());
        QCOMPARE(reply["psd_text"]["rasterized"], listed);
        QCOMPARE(reply["psd_text"]["text_layers"], Json(1));
        const Read p1 = read_psd(reply["files"][0].get<std::string>());
        QVERIFY(p1.file.text(*p1.named("本文 #ok")).has_value());
        const psd::Layer* grad = p1.named("台詞 グラデ");
        QVERIFY(grad != nullptr);
        QCOMPARE(grad->kind, std::string("pixels"));
        QVERIFY(!grad->parent.has_value());
        const Read images = read_psd(genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("doc-images")), kDpi)[0].string());
        QVERIFY(p1.file.image(*grad).tobytes() == images.file.image(*images.named("台詞 グラデ")).tobytes());
    }

    // Fonts are named, never embedded; a font a line asks for that is not there is warned of.
    void fontsAreWarnedOf() {
        Document doc = genko::core::new_episode("書体", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "a", "text": "ある書体", "x_mm": 140, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "style": {"font": "maru"}},
            {"op": "add_line", "page": 1, "id": "b", "text": "ない書体", "x_mm": 100, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "style": {"font": "/no/such/font.otf"}}])"));
        auto fonts = genko::formats::psd_text_fonts(doc);  // (not const: a key it lacks reads as null)
        QVERIFY(std::find(fonts.fonts.begin(), fonts.fonts.end(), kMaru) != fonts.fonts.end());
        QCOMPARE(fonts.warnings.size(), std::size_t{1});
        QCOMPARE(fonts.warnings[0]["line"], Json("b"));
        QCOMPARE(fonts.warnings[0]["kind"], Json("font_missing"));
        genko::formats::PsdTextReport report;
        (void)genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("fonts")), kDpi, genko::formats::PsdText{true, false}, &report);
        QCOMPARE(report.warnings.size(), std::size_t{1});
        QCOMPARE(report.to_json()["warnings"][0]["kind"], Json("font_missing"));
    }

    // `genko export --format psd --psd-text editable [--rasterize-unsupported]`: the same files as the export; the
    // default the image way; what it refuses.
    void commandLine() {
        const QString book = tmp_.path() + "/cli.genko";
        auto ran = genko::test::run_genko({"new", book, "--pages", "4", "--title", "PSD文字", "--plain"});
        QCOMPARE(ran.exit_code, 0);
        ran = genko::test::run_genko({"apply", book, genko::test::test_data("psd_text_book.json")});
        QVERIFY2(ran.exit_code == 0, ran.out.constData());
        ran = genko::test::run_genko({"export", book, tmp_.path() + "/cli-editable", "--format", "psd", "--dpi", "150", "--psd-text", "editable", "--json"});
        QVERIFY2(ran.exit_code == 0, ran.out.constData());
        Json out = printed(ran.out);
        QCOMPARE(out["count"], Json(4));
        QCOMPARE(out["psd_text"]["text_layers"], Json(19));
        QCOMPARE(out["psd_text"]["ruby_layers"], Json(13));
        const Read r = read_psd(out["files"][0].get<std::string>());
        QVERIFY(r.file.text(*r.named("本文 #p1a")).has_value());
        // the default: the image way, the same bytes as --psd-text image
        ran = genko::test::run_genko({"export", book, tmp_.path() + "/cli-default", "--format", "psd", "--dpi", "150"});
        QCOMPARE(ran.exit_code, 0);
        ran = genko::test::run_genko({"export", book, tmp_.path() + "/cli-image", "--format", "psd", "--dpi", "150", "--psd-text", "image"});
        QCOMPARE(ran.exit_code, 0);
        QVERIFY(genko::test::tree_hashes(tmp_.path() + "/cli-default") == genko::test::tree_hashes(tmp_.path() + "/cli-image"));
        // what it refuses
        const QString unsupported = tmp_.path() + "/cli-unsupported.genko";
        ran = genko::test::run_genko({"new", unsupported, "--pages", "1", "--plain"});
        QCOMPARE(ran.exit_code, 0);
        const QString ops = tmp_.path() + "/ops.json";
        genko::test::write_bytes(ops, R"([{"op": "add_line", "page": 1, "id": "g", "text": "グラデ", "x_mm": 100, "y_mm": 20, "w_mm": 30,
            "h_mm": 60, "wrap": "vertical", "style": {"gradient": {"rgb_from": [0, 0, 0], "rgb_to": [200, 0, 0]}}}])");
        ran = genko::test::run_genko({"apply", unsupported, ops});
        QCOMPARE(ran.exit_code, 0);
        ran = genko::test::run_genko({"export", unsupported, tmp_.path() + "/cli-strict", "--format", "psd", "--psd-text", "editable"});
        QCOMPARE(ran.exit_code, 1);
        out = printed(ran.out);
        QCOMPARE(out["code"], Json("psd_text_unsupported"));
        QCOMPARE(out["report"], Json::parse(R"([{"page": 1, "line": "g", "text": "グラデ", "features": ["gradient"]}])"));
        QVERIFY(!QDir(tmp_.path() + "/cli-strict").exists() || QDir(tmp_.path() + "/cli-strict").isEmpty());
        ran = genko::test::run_genko({"export", unsupported, tmp_.path() + "/cli-raster", "--format", "psd", "--psd-text", "editable",
                                      "--rasterize-unsupported", "--json"});
        QVERIFY2(ran.exit_code == 0, ran.out.constData());
        QCOMPARE(printed(ran.out)["psd_text"]["rasterized"][0]["line"], Json("g"));
        for (const QStringList& wrong : {QStringList{"--psd-text", "pictures"}, QStringList{"--rasterize-unsupported"},
                                         QStringList{"--psd-text", "editable", "--format", "png"}}) {
            QStringList args{"export", book, tmp_.path() + "/cli-wrong"};
            if (!wrong.contains("--format")) args << "--format" << "psd";
            ran = genko::test::run_genko(args + wrong);
            QVERIFY2(ran.exit_code == 2, qPrintable(wrong.join(' ')));
            QCOMPARE(printed(ran.err)["code"], Json("usage"));
        }
    }

    // The support table goes out with the docs (docs/PSD_TEXT_SUPPORT.md): every feature this build cannot keep as text,
    // what is kept, and that a ruby does not follow its text in other apps.
    void supportTableIsShipped() {
        const std::string doc = genko::test::read_bytes(genko::test::repo_root() + "/docs/PSD_TEXT_SUPPORT.md");
        QVERIFY(!doc.empty());
        for (const auto& feature : genko::formats::psd_text_unsupported()) {
            QVERIFY2(doc.find(std::string(feature.label)) != std::string::npos, std::string(feature.label).c_str());
            QVERIFY2(doc.find("`" + std::string(feature.key) + "`") != std::string::npos, std::string(feature.key).c_str());
        }
        for (const char* kept : {"縦書き", "横書き", "改行", "部分書式", "字間", "段落揃え", "回転", "縁取り", "ルビ", "自動では追従しません", "埋め込みません"}) {
            QVERIFY2(doc.find(kept) != std::string::npos, kept);
        }
    }
};

QTEST_GUILESS_MAIN(TestPsdText)
#include "test_psd_text.moc"
