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

// A font's PostScript name (name ID 6, each record) given a backslash for its last character, in place.
bool spoil_postscript_name(std::string& font) {
    const auto be16 = [&](std::size_t at) { return (static_cast<unsigned char>(font[at]) << 8) | static_cast<unsigned char>(font[at + 1]); };
    const auto be32 = [&](std::size_t at) { return (static_cast<std::size_t>(be16(at)) << 16) | static_cast<std::size_t>(be16(at + 2)); };
    bool spoilt = false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(be16(4)); ++i) {
        const std::size_t rec = 12 + i * 16;
        if (font.compare(rec, 4, "name") != 0) continue;
        const std::size_t name = be32(rec + 8);
        const std::size_t strings = name + static_cast<std::size_t>(be16(name + 4));
        for (std::size_t k = 0; k < static_cast<std::size_t>(be16(name + 2)); ++k) {
            const std::size_t r = name + 6 + k * 12;
            if (be16(r + 6) != 6) continue;
            const std::size_t end = strings + static_cast<std::size_t>(be16(r + 10)) + static_cast<std::size_t>(be16(r + 8));
            if (be16(r) == 1) {
                font[end - 1] = '\\';
            } else {
                font[end - 2] = '\0';
                font[end - 1] = '\\';
            }
            spoilt = true;
        }
    }
    return spoilt;
}

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

// The image-way PSD of each page of the PSD book at 150 dpi (imageWayIsAsBefore).
const QString kImageWay[4] = {QStringLiteral("1b6d11a43f8606e655dfa18495011edda404e609290241b9f7fa1e91914aa7bd"),
                              QStringLiteral("1f1457dfff7b5c7782f9d8532a37d242d9385a714dd26f97dd0199dd1cc99ca4"),
                              QStringLiteral("f7d33547e0049c31cfd8ec16e5128a01dbca3a7aeecbb42155dfced6fcda7af7"),
                              QStringLiteral("5a8b99f44a399f5542db7ced98d789069da421975dcb7f81097110d5bb3fe665")};

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

    // The image way (the default) is Python's PSD, unchanged by PSD-01: the fixture book's pages are the bytes the build
    // before the editable PSD wrote (their sha256, below: written by e146b96's genko, whose image way test_contract_export
    // holds to Python's, and the same as Python's own for the book without what only this build has —
    // test_contract_psd_text's imageWayIsPythons), with no text layer and no group.
    void imageWayIsAsBefore() {
        const Document doc = psd_book();
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("image")), kDpi);
        QCOMPARE(files.size(), std::size_t{4});
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(files[i].string()));
            const QString sha = QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex());
            QVERIFY2(sha == kImageWay[i], qPrintable(QString::number(i + 1) + ": " + sha));
            QVERIFY(bytes.find("TySh") == std::string::npos);
            QVERIFY(bytes.find("lsct") == std::string::npos);
            const Read r = read_psd(files[i].string());
            for (const auto& layer : r.layers) QVERIFY(!r.file.text(layer).has_value());
        }
    }

    // Each line's layers put together with the renderer's own operation — its balloon layer, then each letter layer
    // pasted on with its alpha as the mask, as render::page_layers draws a line for the image way — are exactly that
    // line's image-way layer, every pixel. (A line with an outline is left out: the PSD draws it as the layer's stroke.
    // A turned line's sheet is resampled whole in the image way — bicubic, each pixel from the 4×4 sheet pixels about
    // its place, which turned lie within 2√2 < 3 pixels of it, clipped to 0..255, after the letters were pasted on the
    // balloon with their alpha as the mask — and its balloon and letters apart here, so the two can be the same only
    // away from the letters. There: a pixel whose 7×7 square no letter layer reaches is exactly the balloon layer's
    // (most of the line); every pixel where the image way is not the balloon has a letter layer's pixel in its 7×7
    // square, and every letter layer's pixel such a pixel in its own; and a pixel one letter layer covers opaquely is,
    // in the image way, nearer that layer's pixel than the balloon's.)
    void layersMakeTheLine() {
        const Document doc = psd_book();
        const auto editable = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("composite")), kDpi,
                                                               genko::formats::PsdText{true, false});
        const auto images = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("composite-images")), kDpi);
        int groups = 0, exact = 0, turned = 0, interior = 0;
        for (std::size_t page = 0; page < editable.size(); ++page) {
            const Read e = read_psd(editable[page].string());
            const Read im = read_psd(images[page].string());
            for (const auto& group : e.layers) {
                if (!group.folder) continue;
                bool halo = false;
                const psd::Layer* balloon = nullptr;
                std::vector<Image> letters;
                std::string id;
                for (const psd::Layer* layer : e.in(group.name)) {
                    if (e.file.stroke(*layer)) halo = true;
                    if (layer->name == "フキダシ") {
                        balloon = layer;
                        continue;
                    }
                    letters.push_back(e.file.image(*layer));
                    if (layer->name.rfind("本文 #", 0) == 0) id = layer->name.substr(std::string("本文 #").size());
                }
                if (halo) continue;
                const psd::Layer* whole = im.named(group.name);
                QVERIFY2(whole != nullptr, group.name.c_str());
                const Image line = im.file.image(*whole);
                ++groups;
                const genko::core::Json& rotate = line_of(doc, id).style.is_object() && line_of(doc, id).style.contains("rotate_deg")
                                                      ? line_of(doc, id).style["rotate_deg"]
                                                      : genko::core::Json(0);
                if (rotate == genko::core::Json(0)) {
                    Image rebuilt = balloon != nullptr ? e.file.image(*balloon) : Image::create("RGBA", line.size(), genko::render::Ink{0, 0, 0, 0});
                    for (const Image& l : letters) rebuilt.paste(l, genko::render::Point{0, 0}, &l);
                    const std::string a = rebuilt.tobytes();
                    const std::string b = line.tobytes();
                    if (a != b) {
                        std::size_t at = 0;
                        while (a[at] == b[at]) ++at;
                        QFAIL((group.name + " differs at " + std::to_string(at / 4 % static_cast<std::size_t>(line.width())) + "," +
                               std::to_string(at / 4 / static_cast<std::size_t>(line.width())))
                                  .c_str());
                    }
                    ++exact;
                    continue;
                }
                ++turned;
                const std::string want = line.tobytes();
                const std::string base = balloon != nullptr ? e.file.image(*balloon).tobytes() : std::string(want.size(), '\0');
                const int w = line.width(), h = line.height();
                const auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
                std::vector<std::string> marks;
                for (const Image& l : letters) marks.push_back(l.tobytes());
                // per pixel: whether a letter layer reaches it, the one that covers it opaquely (alone), whether the image
                // way differs there from the balloon layer
                std::vector<unsigned char> reach(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0), changed(reach.size(), 0);
                std::vector<int> opaque(reach.size(), -1);
                for (std::size_t p = 0; p < reach.size(); ++p) {
                    int count = 0, full = -1;
                    for (std::size_t m = 0; m < marks.size(); ++m) {
                        const auto alpha = static_cast<unsigned char>(marks[m][p * 4 + 3]);
                        if (alpha == 0) continue;
                        ++count;
                        if (alpha == 255) full = static_cast<int>(m);
                    }
                    reach[p] = count > 0 ? 1 : 0;
                    if (count == 1) opaque[p] = full;
                    changed[p] = want.compare(p * 4, 4, base, p * 4, 4) != 0 ? 1 : 0;
                }
                const auto near = [&](const std::vector<unsigned char>& mask, int x, int y) {  // (in the 7×7 square about it)
                    for (int yy = std::max(0, y - 3); yy <= std::min(h - 1, y + 3); ++yy) {
                        for (int xx = std::max(0, x - 3); xx <= std::min(w - 1, x + 3); ++xx) {
                            if (mask[at(xx, yy)] != 0) return true;
                        }
                    }
                    return false;
                };
                const auto distance = [](const std::string& a, const std::string& b, std::size_t p) {
                    int d = 0;
                    for (std::size_t c = 0; c < 4; ++c) d += std::abs(int(static_cast<unsigned char>(a[p * 4 + c])) - int(static_cast<unsigned char>(b[p * 4 + c])));
                    return d;
                };
                std::int64_t inked = 0, exactly = 0;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const std::size_t p = at(x, y);
                        const std::string where = group.name + " at " + std::to_string(x) + "," + std::to_string(y);
                        if (want[p * 4 + 3] != 0) ++inked;
                        if (!near(reach, x, y)) {  // the balloon, exactly (letters there would be ones the letter layers lack)
                            if (changed[p] != 0) QFAIL(("not the balloon: " + where).c_str());
                            if (want[p * 4 + 3] != 0) ++exactly;
                        }
                        if (reach[p] != 0 && !near(changed, x, y)) QFAIL(("letters the image way does not have: " + where).c_str());
                        if (opaque[p] >= 0) {  // the letters, not the balloon
                            const std::string& mark = marks[static_cast<std::size_t>(opaque[p])];
                            if (distance(want, mark, p) >= distance(want, base, p)) QFAIL(("not the letters: " + where).c_str());
                            ++interior;
                        }
                    }
                }
                // most of the line is compared exactly
                QVERIFY2(exactly * 4 > inked * 3, qPrintable(QString::number(exactly) + " of " + QString::number(inked)));
            }
        }
        QCOMPARE(groups, 17);  // (every line but the two with an outline)
        QCOMPARE(turned, 1);
        QCOMPARE(exact, 16);
        QVERIFY(interior > 100);
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
            QCOMPARE(run.style.baseline_direction, 1);  // (upright in its column, as drawn)
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
        QCOMPARE(t.style_at(2)->baseline_direction, 1);
        QCOMPARE(t.style_at(5)->baseline_direction, 1);
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
        const auto issues = genko::formats::psd_text_issues(doc, kDpi);
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
        const auto written = genko::test::tree_hashes(tmp_.path() + "/cli-default");
        QVERIFY(written == genko::test::tree_hashes(tmp_.path() + "/cli-image"));
        QCOMPARE(written.size(), std::size_t{4});
        std::size_t page = 0;
        for (const auto& [name, sha] : written) QCOMPARE(QString::fromStdString(sha), kImageWay[page++]);  // (Python's PSD)
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

    // An outline is the layer's stroke as wide as the halo the lettering draws: the halo's filter stops at 61 px (30 px
    // round the letters), so a wider outline — effect letters' own at 600 dpi (em / 8 = 35 px), outline_mm 3 (71 px), a
    // 網かけの箱's (em / 7) — is 30 px in the PSD too.
    void strokeIsTheHaloDrawn() {
        genko::core::PageSpec spec = genko::core::PageSpec::a4_mono();
        spec.width_mm = genko::core::Num(90);
        spec.height_mm = genko::core::Num(110);
        spec.preset.reset();
        Document doc = genko::core::new_episode("縁", genko::core::Num(1), 1, spec);
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "sfx", "text": "ドン", "x_mm": 5, "y_mm": 5, "w_mm": 80, "h_mm": 30, "wrap": "horizontal",
             "balloon": "sfx"},
            {"op": "add_line", "page": 1, "id": "wide", "text": "縁", "x_mm": 30, "y_mm": 45, "w_mm": 20, "h_mm": 20, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5, "outline_mm": 3}},
            {"op": "add_line", "page": 1, "id": "thin", "text": "細", "x_mm": 30, "y_mm": 75, "w_mm": 20, "h_mm": 20, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5, "outline_mm": 0.5}}])"));
        constexpr int kFine = 600;
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("halo")), kFine, genko::formats::PsdText{true, false});
        const Read r = read_psd(files.front().string());
        for (const auto& [id, width] : std::vector<std::pair<std::string, double>>{{"sfx", 30}, {"wide", 30}, {"thin", 12}}) {
            const auto stroke = r.file.stroke(*r.named("本文 #" + id));
            QVERIFY2(stroke.has_value(), id.c_str());
            QVERIFY2(stroke->size == width, (id + ": " + std::to_string(stroke->size)).c_str());
            // the halo the lettering draws reaches that far round the letters, every side
            const auto& line = line_of(doc, id);
            const Image whole = genko::render::text::text_layout(line, kFine).image;
            const Image letters = genko::render::text::set_layout(line, kFine, std::nullopt, genko::render::text::LetterParts{false, true, true, false}).layout.image;
            QCOMPARE(whole.size(), letters.size());
            const auto a = whole.getbbox().value();
            const auto b = letters.getbbox().value();
            QVERIFY2(b.x0 - a.x0 == width && b.y0 - a.y0 == width && a.x1 - b.x1 == width && a.y1 - b.y1 == width, id.c_str());
        }
    }

    // Down, a cell of half-width letters the lettering keeps upright (a lone letter or digit, a lone !) is upright in the
    // PSD too (BaselineDirection 1), 縦中横 across in its cell (3), a Latin word of four letters or more laid on its side (2).
    void halfWidthUprightDown() {
        Document doc = genko::core::new_episode("向き", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "a", "text": "え!?3日後だ!", "x_mm": 150, "y_mm": 20, "w_mm": 30, "h_mm": 90, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5}},
            {"op": "add_line", "page": 1, "id": "b", "text": "第5話", "x_mm": 110, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5, "tcy": true}},
            {"op": "add_line", "page": 1, "id": "c", "text": "Genkoで描く", "x_mm": 70, "y_mm": 20, "w_mm": 30, "h_mm": 90, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5}}])"));
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("upright")), kDpi, genko::formats::PsdText{true, false});
        const Read r = read_psd(files.front().string());
        const psd::TextLayer a = r.file.text(*r.named("本文 #a")).value();
        QVERIFY(a.text == u"え!?3日後だ!");
        const std::vector<int> want_a{1, 3, 3, 1, 1, 1, 1, 1};
        for (std::size_t i = 0; i < want_a.size(); ++i) QVERIFY2(a.style_at(i)->baseline_direction == want_a[i], std::to_string(i).c_str());
        const psd::TextLayer b = r.file.text(*r.named("本文 #b")).value();
        QVERIFY(b.text == u"第5話");
        QCOMPARE(b.style_at(1)->baseline_direction, 1);
        const psd::TextLayer c = r.file.text(*r.named("本文 #c")).value();
        QVERIFY(c.text == u"Genkoで描く");
        for (std::size_t i = 0; i < 5; ++i) QCOMPARE(c.style_at(i)->baseline_direction, 2);
        QCOMPARE(c.style_at(5)->baseline_direction, 1);
    }

    // A line of Python's (ruby_runs) that the rubies by place set otherwise than its own lettering — a reading wider than
    // its text: Python's way cuts it, by place the lettering widens — is not kept as text (its layers would not be the
    // line drawn): listed (strict stops), drawn as a picture when asked; one set alike stays text.
    void pythonRubySetOtherwise() {
        Document doc = genko::core::new_episode("旧ルビ", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "long", "text": "字だ", "x_mm": 20, "y_mm": 100, "w_mm": 40, "h_mm": 20, "wrap": "horizontal",
             "ruby_runs": [["字", "ながいながいよみがなです"]]},
            {"op": "add_line", "page": 1, "id": "fine", "text": "漢字の読み", "x_mm": 60, "y_mm": 20, "w_mm": 30, "h_mm": 60, "wrap": "vertical",
             "ruby_runs": [["漢字", "かんじ"]]}])"));
        QVERIFY(!genko::render::text::ruby_places(line_of(doc, "long"), kDpi).exact);
        QCOMPARE(genko::formats::issues_json(genko::formats::psd_text_issues(doc, kDpi)),
                 Json::parse(R"([{"page": 1, "line": "long", "text": "字だ", "features": ["ruby_runs"]}])"));
        QVERIFY_THROWS_EXCEPTION(genko::formats::PsdTextRefused, genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("old-strict")), kDpi,
                                                                                                genko::formats::PsdText{true, false}));
        genko::formats::PsdTextReport report;
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("old-raster")), kDpi,
                                                            genko::formats::PsdText{true, true}, &report);
        QCOMPARE(report.rasterized.size(), std::size_t{1});
        const Read r = read_psd(files.front().string());
        const Read images = read_psd(genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("old-images")), kDpi)[0].string());
        QVERIFY(r.file.image(*r.named("台詞 字だ")).tobytes() == images.file.image(*images.named("台詞 字だ")).tobytes());
        QVERIFY(r.file.text(*r.named("ルビ「かんじ」 #fine-r0")).has_value());
        const auto& table = genko::formats::psd_text_unsupported();
        QVERIFY(std::any_of(table.begin(), table.end(), [](const auto& f) { return f.key == "ruby_runs"; }));
    }

    // Control characters the lettering draws in a row (a carriage return, a tab, U+0003 — Photoshop's paragraph and
    // forced line breaks) are spaces in the PSD's text: one paragraph a row, the runs as long as the text.
    void controlCharactersAreSpaces() {
        Document doc = genko::core::new_episode("制御", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        Json op{{"op", "add_line"}, {"page", 1}, {"id", "c"}, {"text", "横の\r改行\tタブ\x03終わり"}, {"x_mm", 20}, {"y_mm", 20}, {"w_mm", 150},
                {"h_mm", 20}, {"wrap", "horizontal"}, {"balloon", "none"}, {"style", Json{{"size_mm", 5}}}};
        doc = run_ops(doc, Json::array({op}));
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("controls")), kDpi, genko::formats::PsdText{true, false});
        const Read r = read_psd(files.front().string());
        const psd::TextLayer t = r.file.text(*r.named("本文 #c")).value();
        QVERIFY(t.text == u"横の 改行 タブ 終わり");
        QVERIFY(t.engine_text == t.text + u"\r");
        QCOMPARE(t.paragraphs.size(), std::size_t{1});
        std::size_t length = 0;
        for (const auto& run : t.runs) length += run.length;
        QCOMPARE(length, t.text.size() + 1);
    }

    // A font's PostScript name is written with only A–Z, a–z, 0–9 and . _ + - (a backslash at its end would end the
    // text engine's text for psd-tools).
    void fontNamesAreTokens() {
        std::string font = genko::test::read_bytes(genko::test::repo_root() + "/src/genko/fonts/Yomogi-Regular.ttf");
        QVERIFY(!font.empty());
        QVERIFY(spoil_postscript_name(font));
        const QString file = tmp_.path() + "/spoilt-name.ttf";
        genko::test::write_bytes(file, font);
        Document doc = genko::core::new_episode("書体名", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        doc = run_ops(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "f"}, {"text", "ふでの字"}, {"x_mm", 140}, {"y_mm", 20},
                                             {"w_mm", 30}, {"h_mm", 60}, {"wrap", "vertical"}, {"balloon", "none"},
                                             {"style", Json{{"size_mm", 5}, {"font", file.toStdString()}}}}}));
        const auto files = genko::formats::export_psd_pages(doc, genko::core::path_from_utf8(path("font-name")), kDpi, genko::formats::PsdText{true, false});
        const Read r = read_psd(files.front().string());
        const psd::TextLayer t = r.file.text(*r.named("本文 #f")).value();
        QCOMPARE(t.runs.front().style.font, std::string("Yomogi-Regula"));
    }

    // A descriptor nested without end (lists in lists) is refused, not followed until the stack runs out.
    void deepDescriptorsAreRefused() {
        std::string payload;
        const auto u16 = [&](unsigned v) { payload += static_cast<char>(v >> 8); payload += static_cast<char>(v & 0xff); };
        const auto u32 = [&](std::uint32_t v) { u16(v >> 16); u16(v & 0xffff); };
        u16(1);
        payload.append(48, '\0');  // (the transform)
        u16(50);
        u32(16);
        u32(1);
        u16(0);  // (the descriptor's name: "")
        u32(0);
        payload += "TxLr";
        u32(1);
        u32(0);
        payload += "Txt ";
        for (int i = 0; i < 300000; ++i) {
            payload += "VlLs";
            u32(1);
        }
        payload += "long";
        u32(0);
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, psd::read_tysh(payload));
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
