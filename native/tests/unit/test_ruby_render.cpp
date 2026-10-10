// TEXT-01 drawn (AC-TEXT, ruby-layout@1): a line's ruby by place in the lettering, the page, PNG and PDF. With nothing
// of the new layout set a line held by place draws exactly as Python's ruby_runs line; each ruby's own font, size
// against the text, head / centre / tail, distance from the text and letter spacing move its reading where they say
// (the reading's ink measured in the ruby lane), two rubies of one line each as its own settings say, through edits;
// the second of two same words gets its reading beside it, not the first; where each drawn ruby and its base lie on
// the page (PSD-01's render::text::ruby_places);
// columns broken by kinsoku and the box never change the ranges; turned across, the reading sits above the row. The
// PNG and the PDF pages (its pictures inflated) are the same pixels. And the F4 book (FIXTURES.md: four pages, down
// and across, 「東京と東京」, kinsoku, 縦中横 「AI」「12」, styled parts, variation selectors, several rubies, balloons,
// effect letters, the nombre, CMYK, draft and non-printing layers) built here, saved, read back and written out.

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <zlib.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/ruby.hpp"
#include "formats/export.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/text/lettering.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::CommandBus;
using genko::core::Document;
using genko::core::Json;
using genko::render::Image;

namespace {

constexpr int kDpi = 150;  // em: size_mm 5 → 30 px; the ruby 0.5 em → 15 px

const CommandBus& bus() {
    static const CommandBus b(genko::render::ops_registry());
    return b;
}

Document run_ops(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")).doc; }

Document blank_book(int pages = 1) {
    return genko::core::new_episode("ルビ", genko::core::Num(1), pages, genko::core::PageSpec::a4_mono());
}

// A book with one line (its id L1): `fields` over a line of 60 × 60 mm at (20, 20), lettered at 5 mm.
Document with_line(const Json& fields) {
    Json op{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"x_mm", 20}, {"y_mm", 20}, {"w_mm", 60}, {"h_mm", 60}, {"wrap", "vertical"},
            {"style", Json{{"size_mm", 5}}}};
    for (const auto& [key, value] : fields.items()) {
        if (key == "style") {
            for (const auto& [k, v] : value.items()) op["style"][k] = v;
        } else {
            op[key] = value;
        }
    }
    return run_ops(blank_book(), Json::array({op}));
}

const genko::core::StoryLine& line_of(const Document& doc) { return doc.story.front(); }

Image lettering(const Json& fields) {
    const Document doc = with_line(fields);
    return genko::render::text::text_image(line_of(doc), kDpi).first;
}

// The box of the ink (alpha > 40) inside `area` of an RGBA picture: x0, y0, x1, y1 (in the picture), or none.
std::optional<std::array<int, 4>> ink_box(const Image& image, int x0, int y0, int x1, int y1) {
    std::optional<std::array<int, 4>> box;
    for (int y = std::max(0, y0); y < std::min(image.height(), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(image.width(), x1); ++x) {
            if (image.getpixel(x, y).back() <= 40) continue;
            if (!box) {
                box = std::array<int, 4>{x, y, x + 1, y + 1};
            } else {
                (*box)[0] = std::min((*box)[0], x);
                (*box)[1] = std::min((*box)[1], y);
                (*box)[2] = std::max((*box)[2], x + 1);
                (*box)[3] = std::max((*box)[3], y + 1);
            }
        }
    }
    return box;
}

// The dark pixels (< 128) of a page picture (RGB or L) inside a box.
int dark_in(const Image& page, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(page.height(), y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(page.width(), x1); ++x) {
            if (page.getpixel(x, y).front() < 128) ++n;
        }
    }
    return n;
}

std::string read_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

std::string inflate_all(std::string_view data) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return {};
    std::string out;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    char buffer[65536];
    int result = Z_OK;
    while (result == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buffer);
        z.avail_out = sizeof buffer;
        result = inflate(&z, Z_NO_FLUSH);
        out.append(buffer, sizeof buffer - z.avail_out);
    }
    inflateEnd(&z);
    return result == Z_STREAM_END ? out : std::string();
}

// The PDF's page pictures: (width, height, the pixels inflated), in the order they are written.
std::vector<std::tuple<int, int, std::string>> pdf_pictures(const std::string& pdf) {
    std::vector<std::tuple<int, int, std::string>> out;
    std::size_t at = 0;
    while ((at = pdf.find("/Subtype /Image /Width ", at)) != std::string::npos) {
        const int width = std::stoi(pdf.substr(at + 23));
        const std::size_t h = pdf.find("/Height ", at);
        const int height = std::stoi(pdf.substr(h + 8));
        const std::size_t length = pdf.find("/Length ", at);
        const std::size_t size = std::stoul(pdf.substr(length + 8));
        const std::size_t start = pdf.find("stream\n", length) + 7;
        out.emplace_back(width, height, inflate_all(std::string_view(pdf).substr(start, size)));
        at = start + size;
    }
    return out;
}

void save_book(const Document& doc, const std::filesystem::path& dir) {
    genko::storage::ProjectLock lock(dir, "genko");
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = "genko";
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
}

Json story_of(const Document& doc, const QTemporaryDir& dir) {
    genko::storage::AssetStore store(genko::storage::path_from_utf8(dir.path().toStdString() + "/payload-assets"));
    return genko::storage::project_payload_v4(doc, store)["story"];
}

}  // namespace

class TestRubyRender : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Nothing of the new layout set: by place or by Python's ruby_runs, the same pixels (down and across).
    void byPlaceDrawsAsPython() {
        for (const char* wrap : {"vertical", "horizontal"}) {
            const Image python = lettering(Json{{"text", "東京と大阪へ"}, {"wrap", wrap},
                                                {"ruby_runs", Json::parse(R"([["東京", "とうきょう"], ["大阪", "おおさか"]])")}});
            const Image placed = lettering(Json{{"text", "東京と大阪へ"}, {"wrap", wrap},
                                                {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう"},
                                                    {"start": 3, "end": 5, "ruby": "おおさか"}])")}});
            QCOMPARE(placed.size(), python.size());
            QVERIFY2(placed.tobytes() == python.tobytes(), wrap);
            const Image bare = lettering(Json{{"text", "東京と大阪へ"}, {"wrap", wrap}});
            QVERIFY(bare.tobytes() != python.tobytes());
        }
        // mono ruby (モノルビ) the same way
        const Image python = lettering(Json{{"text", "東京"}, {"style", Json{{"mono_ruby", true}}},
                                            {"ruby_runs", Json::parse(R"([["東京", "とうきょ"]])")}});
        const Image placed = lettering(Json{{"text", "東京"}, {"style", Json{{"mono_ruby", true}}},
                                            {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょ"}])")}});
        QVERIFY(placed.tobytes() == python.tobytes());
        // (across, Python's lettering has no モノルビ: one reading over the whole base — 「さか」 centred over 山川, not さ
        // over 山 and か over 川 — by place too)
        const Image python_across = lettering(Json{{"text", "山川へ"}, {"wrap", "horizontal"}, {"style", Json{{"mono_ruby", true}}},
                                                   {"ruby_runs", Json::parse(R"([["山川", "さか"]])")}});
        const Image placed_across = lettering(Json{{"text", "山川へ"}, {"wrap", "horizontal"}, {"style", Json{{"mono_ruby", true}}},
                                                   {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "さか"}])")}});
        QCOMPARE(placed_across.size(), python_across.size());
        QVERIFY(placed_across.tobytes() == python_across.tobytes());
        // (but a reading longer than all the text: Python's is cut at the lettering's end; by place it grows to hold it)
        const Image cut = lettering(Json{{"text", "東京"}, {"ruby_runs", Json::parse(R"([["東京", "とうきょうと"]])")}});
        const Image whole = lettering(Json{{"text", "東京"}, {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょうと"}])")}});
        QCOMPARE(cut.height(), 60);
        QCOMPARE(whole.height(), 90);
        QVERIFY(ink_box(whole, 30, 75, 45, 90));
    }

    // 「東京と東京」: the reading over the second word (Python's ruby_runs always finds the first).
    void theSecondOfTwoSameWords() {
        const Image second = lettering(Json{{"text", "東京と東京"}, {"ruby_spans", Json::parse(R"([{"start": 3, "end": 5, "ruby": "とうきょう"}])")}});
        const Image first = lettering(Json{{"text", "東京と東京"}, {"ruby_runs", Json::parse(R"([["東京", "とうきょう"]])")}});
        // one column of five 30 px cells; the ruby lane right of it (x 30..45)
        QCOMPARE(second.width(), 45);
        const auto on_second = ink_box(second, 30, 0, 45, second.height());
        const auto on_first = ink_box(first, 30, 0, 45, first.height());
        QVERIFY(on_second && on_first);
        QVERIFY2((*on_second)[1] >= 75, std::to_string((*on_second)[1]).c_str());  // beside rows 3 and 4 (90..150, centred)
        QVERIFY((*on_first)[3] <= 75);
    }

    // 先頭・中央・末尾 (each ruby's own align): 「えき」 over 「東京駅前」 (rows 0..120).
    void headCentreTail() {
        const auto lane = [](const char* align, const char* wrap) {
            Json one = Json::parse(R"({"start": 0, "end": 4, "ruby": "えき"})");
            if (align[0] != '\0') one["align"] = align;
            return lettering(Json{{"text", "東京駅前"}, {"wrap", wrap}, {"ruby_spans", Json::array({one})}});
        };
        const Image head = lane("head", "vertical");
        const Image centre = lane("center", "vertical");
        const Image tail = lane("tail", "vertical");
        QVERIFY(lane("", "vertical").tobytes() == centre.tobytes());  // (centre: the default, as Python)
        const auto h = ink_box(head, 30, 0, 45, 120);
        const auto c = ink_box(centre, 30, 0, 45, 120);
        const auto t = ink_box(tail, 30, 0, 45, 120);
        QVERIFY(h && c && t);
        QVERIFY2((*h)[1] < 8 && (*h)[3] <= 32, "head: from the top of the base");
        QVERIFY2((*c)[1] >= 43 && (*c)[3] <= 77, "centre: around the middle of the base");
        QVERIFY2((*t)[1] >= 88 && (*t)[3] > 112, "tail: to the end of the base");
        // across: the reading above the row, from the base's left / centred / to its right
        const Image left = lane("head", "horizontal");
        const Image right = lane("tail", "horizontal");
        const auto l = ink_box(left, 0, 0, left.width(), 15);
        const auto r = ink_box(right, 0, 0, right.width(), 15);
        QVERIFY(l && r);
        QVERIFY((*l)[0] < 6);
        QVERIFY((*r)[2] > right.width() - 6);
    }

    // Each ruby's own size against the text, distance from it, letter spacing and face.
    void sizeDistanceSpacingFont() {
        const auto with = [](const Json& settings) {
            Json one = Json::parse(R"({"start": 0, "end": 4, "ruby": "えき"})");
            for (const auto& [key, value] : settings.items()) one[key] = value;
            return lettering(Json{{"text", "東京駅前"}, {"ruby_spans", Json::array({one})}});
        };
        const Image plain = with(Json::object());
        QCOMPARE(plain.width(), 45);
        // the size against the text: 0.8 em → a 24 px lane, bigger letters
        const Image big = with(Json{{"scale", 0.8}});
        QCOMPARE(big.width(), 54);
        const auto small_ink = ink_box(plain, 30, 0, 45, 120);
        const auto big_ink = ink_box(big, 30, 0, 54, 120);
        QVERIFY(small_ink && big_ink);
        QVERIFY(((*big_ink)[3] - (*big_ink)[1]) > ((*small_ink)[3] - (*small_ink)[1]) + 10);
        // 2 mm from the text: 12 px between the column and the lane
        const Image apart = with(Json{{"gap_mm", 2}});
        QCOMPARE(apart.width(), 57);
        const auto apart_ink = ink_box(apart, 30, 0, 57, 120);
        QVERIFY(apart_ink && (*apart_ink)[0] >= 42);
        QVERIFY(!ink_box(apart, 30, 0, 42, 120));
        QVERIFY(apart.crop(genko::render::Box{0, 0, 30, 120}).tobytes() == plain.crop(genko::render::Box{0, 0, 30, 120}).tobytes());
        // spacing 1.0: a ruby's length between its letters added (15 px)
        const Image spaced = with(Json{{"tracking", 1.0}});
        const auto spaced_ink = ink_box(spaced, 30, 0, 45, 120);
        QVERIFY(spaced_ink);
        QVERIFY(((*spaced_ink)[3] - (*spaced_ink)[1]) >= ((*small_ink)[3] - (*small_ink)[1]) + 13);
        // another face for the reading only: the text's pixels the same, the reading's not
        const Image gothic = with(Json{{"font", "gothic"}});
        QCOMPARE(gothic.size(), plain.size());
        QVERIFY(gothic.crop(genko::render::Box{0, 0, 30, 120}).tobytes() == plain.crop(genko::render::Box{0, 0, 30, 120}).tobytes());
        QVERIFY(gothic.crop(genko::render::Box{30, 0, 45, 120}).tobytes() != plain.crop(genko::render::Box{30, 0, 45, 120}).tobytes());
        // a ruby without a size of its own is the line's ruby_scale (Python's, down; across Python's is half the em)
        const Image lined = lettering(Json{{"text", "東京駅前"}, {"style", Json{{"size_mm", 5}, {"ruby_scale", 0.8}}},
                                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 4, "ruby": "えき"}])")}});
        QVERIFY(lined.tobytes() == big.tobytes());
        for (const char* wrap : {"vertical", "horizontal"}) {
            const Image python = lettering(Json{{"text", "東京駅前"}, {"wrap", wrap}, {"style", Json{{"size_mm", 5}, {"ruby_scale", 0.7}}},
                                                {"ruby_runs", Json::parse(R"([["東京駅前", "えき"]])")}});
            const Image placed = lettering(Json{{"text", "東京駅前"}, {"wrap", wrap}, {"style", Json{{"size_mm", 5}, {"ruby_scale", 0.7}}},
                                                {"ruby_spans", Json::parse(R"([{"start": 0, "end": 4, "ruby": "えき"}])")}});
            QVERIFY2(placed.tobytes() == python.tobytes(), wrap);
        }
    }

    // Two rubies on one line, each set as it says: 「えき」 from the head of 「東京駅前」 in the line's face and size, 「しろ」
    // to the tail of 「大阪城前」 in Gothic at 0.4 em, 2 mm from the text (rows 0..240; the lane as wide as the widest needs).
    void twoRubiesEachTheirOwn() {
        const Image both = lettering(Json{{"text", "東京駅前大阪城前"}, {"h_mm", 80}, {"ruby_spans", Json::parse(R"([
            {"start": 0, "end": 4, "ruby": "えき", "align": "head"},
            {"start": 4, "end": 8, "ruby": "しろ", "align": "tail", "font": "gothic", "scale": 0.4, "gap_mm": 2}])")}});
        QCOMPARE(both.height(), 240);
        QCOMPARE(both.width(), 54);  // (30 px letters, then max(15 + 0, 12 + 12) for the lane)
        const auto first = ink_box(both, 30, 0, 54, 120);
        const auto second = ink_box(both, 30, 120, 54, 240);
        QVERIFY(first && second);
        QVERIFY2((*first)[0] >= 30 && (*first)[2] <= 46 && (*first)[1] < 8 && (*first)[3] <= 32, "えき: beside the text, from the head");
        QVERIFY2((*second)[0] >= 42 && (*second)[1] >= 212 && (*second)[3] > 228, "しろ: 12 px off, to the tail");
        // each ruby's letters its own: the second's drawn as the first's settings would differ
        const Image plain = lettering(Json{{"text", "東京駅前大阪城前"}, {"h_mm", 80}, {"ruby_spans", Json::parse(R"([
            {"start": 0, "end": 4, "ruby": "えき", "align": "head"}, {"start": 4, "end": 8, "ruby": "しろ", "align": "tail"}])")}});
        QVERIFY(plain.crop(genko::render::Box{30, 0, 45, 120}).tobytes() == both.crop(genko::render::Box{30, 0, 45, 120}).tobytes());
        QVERIFY(plain.crop(genko::render::Box{30, 120, 45, 240}).tobytes() != both.crop(genko::render::Box{30, 120, 54, 240}).tobytes());
    }

    // 「東京と東京」: each word's ruby keeps its own settings through an edit (head for the first, tail for the second).
    void repeatedWordsKeepTheirOwn() {
        Document doc = with_line(Json{{"text", "東京と東京"}, {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とう", "align": "head"},
                                                                                    {"start": 3, "end": 5, "ruby": "とう", "align": "tail"}])")}});
        doc = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "text": "新東京と東京"}])"));
        const Image image = genko::render::text::text_image(line_of(doc), kDpi).first;
        // rows: 新 0, 東京 30..90, と 90, 東京 120..180
        const auto first = ink_box(image, 30, 0, 45, 105);
        const auto second = ink_box(image, 30, 105, 45, 180);
        QVERIFY(first && second);
        QVERIFY2((*first)[1] >= 30 && (*first)[1] < 38 && (*first)[3] <= 62, "the first from the head of its word");
        QVERIFY2((*second)[1] >= 148 && (*second)[3] > 170, "the second to the tail of its word");
    }

    // What the lettering says of rubies it could not set as asked: モノルビ's parts of one reading that meet each other
    // (down: 「やまやま」 beside 山 and 「かわかわ」 beside 川, each longer than its letter) overlap; a ruby's font that
    // is not there is the line's face (said: font_fallback), never another default; a horizontal part drawn in the font
    // of its first letter says a letter that font lacks as missing, not as drawn in another face.
    void notesAsDrawn() {
        const auto notes = [](const Json& fields) {
            const Document doc = with_line(fields);
            std::set<std::string> out;
            for (const auto& note : genko::render::text::ruby_notes(line_of(doc), kDpi)) out.insert(note.kind + ":" + note.detail);
            return out;
        };
        const auto mono = notes(Json{{"text", "山川へ"}, {"style", Json{{"mono_ruby", true}}},
                                     {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "やまやまかわかわ"}])")}});
        QVERIFY(std::any_of(mono.begin(), mono.end(), [](const std::string& n) { return n.rfind("overlap:", 0) == 0; }));
        // the ruby's font not found: the line's face (Mincho here), as if none were given
        const Json lost = Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう", "font": "nosuchface"}])");
        const Json none = Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう"}])");
        for (const char* wrap : {"vertical", "horizontal"}) {
            const Json style{{"size_mm", 5}, {"font", "mincho"}};
            const Image missing = lettering(Json{{"text", "東京へ"}, {"wrap", wrap}, {"style", style}, {"ruby_spans", lost}});
            const Image plain = lettering(Json{{"text", "東京へ"}, {"wrap", wrap}, {"style", style}, {"ruby_spans", none}});
            QVERIFY2(missing.tobytes() == plain.tobytes(), wrap);
            QVERIFY2(notes(Json{{"text", "東京へ"}, {"wrap", wrap}, {"style", style}, {"ruby_spans", lost}}).contains("font_fallback:font:nosuchface"), wrap);
        }
        // across, 「かゐ」 in the SFX face is drawn in the face of か: ゐ, which it lacks, is missing (no other face drew it)
        const auto across = notes(Json{{"text", "井戸"}, {"wrap", "horizontal"},
                                       {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "かゐ", "font": "sfx"}])")}});
        QVERIFY2(across.contains("glyph_missing:ゐ") && !across.contains("font_fallback:ゐ"), (*across.begin()).c_str());
    }

    // A distance from the text below the ruby's own size (every ruby of the row): the lane above the row is never
    // less than nothing — the 傍点 and the letters where a line without the ruby has them, the reading into the text.
    void negativeDistanceAcross() {
        const auto at600 = [](const Json& spans) {
            Json op{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"x_mm", 20}, {"y_mm", 20}, {"w_mm", 60}, {"h_mm", 20},
                    {"wrap", "horizontal"}, {"balloon", "none"}, {"style", Json{{"size_mm", 2}}}, {"text", "東京へ"},
                    {"emphasis_runs", Json::array({"東京"})}};
            if (!spans.is_null()) op["ruby_spans"] = spans;
            const Document doc = run_ops(blank_book(), Json::array({op}));
            return genko::render::text::text_image(line_of(doc), 600).first;
        };
        const Image under = at600(Json::parse(R"([{"start": 0, "end": 2, "ruby": "あ", "scale": 0.25, "gap_mm": -1}])"));
        const Image bare = at600(Json());
        QCOMPARE(under.size(), bare.size());
        // (the reading 12 px wide over the middle of 東京: x 41..53; left of it the same pixels)
        QVERIFY(under.crop(genko::render::Box{0, 0, 35, under.height()}).tobytes() == bare.crop(genko::render::Box{0, 0, 35, bare.height()}).tobytes());
        QVERIFY(ink_box(under, 38, 0, 56, under.height()));
    }

    // 禁則 and the box break the columns: never the ranges. A base broken over two columns gets its reading in both.
    void kinsokuAndTheBoxNeverMoveRanges() {
        Document doc = with_line(Json{{"text", "あいうえお東京へ"}, {"h_mm", 25},
                                      {"ruby_spans", Json::parse(R"([{"start": 5, "end": 7, "ruby": "とうきょう"}])")}});
        const Json before = story_of(doc, tmp_);
        const Image narrow = genko::render::text::text_image(line_of(doc), kDpi).first;
        doc = run_ops(doc, Json::parse(R"([{"op": "move_line", "id": "L1", "h_mm": 60}])"));
        const Json after = story_of(doc, tmp_);
        QCOMPARE(after[0]["ruby_spans"].dump(), before[0]["ruby_spans"].dump());
        QVERIFY(!before[0]["ruby_spans"].is_null());
        const Image tall = genko::render::text::text_image(line_of(doc), kDpi).first;
        QVERIFY(narrow.width() > tall.width());  // (more columns)
        // the columns right to left, each its 30 px cells, its 15 px lane and 12 px (0.4 em) before the next: the reading
        // in a lane wherever the base went
        const int columns = (narrow.width() + 12) / 57;
        QVERIFY(columns >= 2);
        int lanes = 0;
        for (int c = 0; c < columns; ++c) {
            const int right = narrow.width() - c * 57;
            if (ink_box(narrow, right - 15, 0, right, narrow.height())) ++lanes;
        }
        QVERIFY(lanes >= 1);
    }

    // 縦横切替: the ranges kept, the reading above the row.
    void turnedAcross() {
        Document doc = with_line(Json{{"text", "東京へ"}, {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とう"}])")}});
        const Json before = story_of(doc, tmp_);
        doc = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "L1", "wrap": "horizontal"}])"));
        QCOMPARE(story_of(doc, tmp_)[0]["ruby_spans"].dump(), before[0]["ruby_spans"].dump());
        const Image across = genko::render::text::text_image(line_of(doc), kDpi).first;
        const auto above = ink_box(across, 0, 0, across.width(), 15);
        QVERIFY(above);
        QVERIFY2((*above)[0] >= 12 && (*above)[2] <= 48, std::to_string((*above)[0]).c_str());  // centred over 東京 (x 0..60), not over へ
    }

    // The page as PNG and PDF: the ruby where the line's settings put it, the PDF's picture the PNG's pixels.
    void pngAndPdf() {
        Document doc = run_ops(blank_book(), Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "A", "text": "東京と東京", "x_mm": 120, "y_mm": 30, "w_mm": 40, "h_mm": 60,
             "wrap": "vertical", "balloon": "none", "style": {"size_mm": 6}, "ruby_spans": [{"start": 3, "end": 5, "ruby": "とうきょう",
             "font": "gothic", "scale": 0.6, "align": "head", "gap_mm": 1, "tracking": 0.2}]},
            {"op": "add_line", "page": 1, "id": "B", "text": "大阪へ行く", "x_mm": 30, "y_mm": 150, "w_mm": 70, "h_mm": 20,
             "wrap": "horizontal", "balloon": "none", "style": {"size_mm": 6},
             "ruby_spans": [{"start": 0, "end": 2, "ruby": "おおさか", "align": "tail"}]}])"));
        const auto out = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/out");
        const auto pngs = genko::formats::export_print(doc, out, "png", kDpi, 180, false, "paper", "gray");
        const auto pdfs = genko::formats::export_print(doc, out, "pdf", kDpi, 180, false, "paper", "gray");
        QCOMPARE(pngs.size(), std::size_t{1});
        QCOMPARE(pdfs.size(), std::size_t{1});
        const Image png = genko::render::read_png(read_file(QString::fromStdString(pngs.front().string())));
        const auto pictures = pdf_pictures(read_file(QString::fromStdString(pdfs.front().string())));
        QCOMPARE(pictures.size(), std::size_t{1});
        QCOMPARE(std::get<0>(pictures.front()), png.width());
        QCOMPARE(std::get<1>(pictures.front()), png.height());
        QVERIFY(std::get<2>(pictures.front()) == png.convert("L").tobytes());
        // the same page without the readings: the difference only beside the second 東京 (A) and above 大阪 (B)
        Document bare = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "A", "ruby_spans": null},
                                                    {"op": "edit_line", "id": "B", "ruby_spans": null}])"));
        const auto bare_pngs = genko::formats::export_print(bare, genko::storage::path_from_utf8(tmp_.path().toStdString() + "/bare"), "png", kDpi,
                                                            180, false, "paper", "gray");
        const Image without = genko::render::read_png(read_file(QString::fromStdString(bare_pngs.front().string())));
        QCOMPARE(without.size(), png.size());
        int top = png.height(), bottom = 0;
        int changed = 0;
        for (int y = 0; y < png.height(); ++y) {
            for (int x = 0; x < png.width(); ++x) {
                if (png.getpixel(x, y).front() == without.getpixel(x, y).front()) continue;
                ++changed;
                if (x > png.width() / 2) {  // (line A: the right half of the page)
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        QVERIFY(changed > 100);
        QVERIFY(bottom > top);
        // A's column: 5 cells of 6 mm (35 px) from its top; the reading starts at the second 東京 (row 3: about 105 px
        // below the column's top), not beside the first
        QVERIFY2(top - genko::render::mm_to_px(30 - 3, kDpi) > 60, std::to_string(top).c_str());
    }

    // PSD-01 (internal, render::text::ruby_places): where each drawn ruby lies on the page and where its base does. The
    // page's pixels that change when the readings are blanked (the same layout, no ink) all lie in the ruby boxes, those
    // that change with the base's words in the base box; each place carries its ruby's own size and face; a base broken
    // over two columns gives a place for each part; a ruby that needs review has none; in a balloon (the letters
    // fitted) the boxes hold the ruby's and the base's ink; a turned line's boxes are not exact; a line on a path has none.
    void rubyPlacesOnThePage() {
        using genko::render::Box;
        using genko::render::text::ruby_places;
        const Document doc = run_ops(blank_book(), Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "A", "text": "東京と大阪", "x_mm": 120, "y_mm": 30, "w_mm": 40, "h_mm": 60, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 6}, "ruby_spans": [{"start": 0, "end": 2, "ruby": "とうきょう", "align": "head"},
             {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic", "scale": 0.4, "gap_mm": 1}]},
            {"op": "add_line", "page": 1, "id": "B", "text": "大阪へ行く", "x_mm": 30, "y_mm": 150, "w_mm": 70, "h_mm": 20, "wrap": "horizontal",
             "balloon": "none", "style": {"size_mm": 6}, "ruby_spans": [{"start": 0, "end": 2, "ruby": "おおさか", "align": "tail", "tracking": 0.2}]},
            {"op": "add_line", "page": 1, "id": "C", "text": "心の声をきく", "x_mm": 40, "y_mm": 40, "w_mm": 45, "h_mm": 50,
             "ruby_spans": [{"start": 0, "end": 1, "ruby": "こころ"}, {"start": 2, "end": 3, "ruby": "こえ", "scale": 0.6}]},
            {"op": "add_line", "page": 1, "id": "D", "text": "東京駅前", "x_mm": 140, "y_mm": 200, "w_mm": 40, "h_mm": 12, "wrap": "vertical",
             "balloon": "none", "style": {"size_mm": 5}, "ruby_spans": [{"start": 0, "end": 4, "ruby": "えきまえ"}]},
            {"op": "add_line", "page": 1, "id": "E", "text": "東京へ", "x_mm": 100, "y_mm": 200, "w_mm": 30, "h_mm": 40, "style": {"rotate_deg": 15},
             "ruby_spans": [{"start": 0, "end": 2, "ruby": "とうきょう"}]},
            {"op": "add_line", "page": 1, "id": "F", "text": "東京へ", "x_mm": 20, "y_mm": 230, "w_mm": 60, "h_mm": 20, "balloon": "none",
             "style": {"text_path": [[0, 10], [60, 10]]}, "ruby_spans": [{"start": 0, "end": 2, "ruby": "とうきょう"}]},
            {"op": "add_line", "page": 1, "id": "G", "text": "東新京へ", "x_mm": 60, "y_mm": 200, "w_mm": 30, "h_mm": 40, "balloon": "none",
             "ruby_spans": [{"start": 0, "end": 3, "ruby": "とうきょう", "base": "東京", "review": "edited"}]}])"));
        const auto line = [](const Document& d, const char* id) -> const genko::core::StoryLine& {
            for (const auto& l : d.story) {
                if (l.id == id) return l;
            }
            return d.story.front();
        };
        const auto page = [](const Document& d) { return genko::render::render_page(d.page(0), kDpi, {}, &d).image.convert("L"); };
        const auto inside = [](const std::vector<Box>& boxes, int x, int y) {
            return std::any_of(boxes.begin(), boxes.end(), [&](const Box& b) { return b.x0 <= x && x < b.x1 && b.y0 <= y && y < b.y1; });
        };
        // every pixel that differs between two pages lies in the boxes, and each box has some
        const auto changed_within = [&](const Image& a, const Image& b, const std::vector<Box>& boxes) {
            std::vector<int> hits(boxes.size(), 0);
            for (int y = 0; y < a.height(); ++y) {
                for (int x = 0; x < a.width(); ++x) {
                    if (a.getpixel(x, y).front() == b.getpixel(x, y).front()) continue;
                    if (!inside(boxes, x, y)) return "a change at " + std::to_string(x) + "," + std::to_string(y) + " outside";
                    for (std::size_t i = 0; i < boxes.size(); ++i) {
                        if (inside({boxes[i]}, x, y)) ++hits[i];
                    }
                }
            }
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                if (hits[i] == 0) return "nothing changed in box " + std::to_string(i);
            }
            return std::string();
        };
        const Image drawn = page(doc);
        const auto blank_readings = [&](const char* id) {
            Json spans = genko::core::ruby::to_json(*line(doc, id).ruby_spans);
            for (Json& span : spans) {
                const auto letters = static_cast<std::size_t>(genko::core::ruby::count(span["ruby"].get<std::string>()));
                std::string blanks;
                for (std::size_t i = 0; i < letters; ++i) blanks += "　";
                span["ruby"] = blanks;
                span.erase("base");
            }
            return run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", id}, {"ruby_spans", spans}}}));
        };
        // A, down: two rubies, each its own
        const auto a = ruby_places(line(doc, "A"), kDpi);
        QVERIFY(a.exact);
        QCOMPARE(a.places.size(), std::size_t{2});
        QCOMPARE(a.places[0].span, std::size_t{0});
        QCOMPARE(a.places[0].reading, std::string("とうきょう"));
        QCOMPARE(a.places[0].size, std::int64_t{18});  // (6 mm at 150 dpi: a 35 px em, half of it)
        QCOMPARE(a.places[1].size, std::int64_t{14});  // (0.4 em)
        QVERIFY(a.places[0].vertical);
        QCOMPARE(a.places[0].letters.size(), std::size_t{5});
        QCOMPARE(a.places[1].face.key, std::string("gothic"));
        QVERIFY(a.places[0].face.key != "gothic");
        QCOMPARE(a.places[0].ruby.y0, a.places[0].base.y0);  // (head)
        QCOMPARE(a.places[1].ruby.x0 - a.places[1].base.x1, 6);  // (1 mm from the text)
        QCOMPARE(a.places[0].base.x0, genko::render::mm_to_px(120, kDpi));  // (text only: from the box's corner)
        std::string why = changed_within(drawn, page(blank_readings("A")), {a.places[0].ruby, a.places[1].ruby});
        QVERIFY2(why.empty(), why.c_str());
        const Document other_words = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "A", "text": "南北と大阪", "ruby_spans": [
            {"start": 0, "end": 2, "ruby": "とうきょう", "align": "head"}, {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic", "scale": 0.4,
             "gap_mm": 1}]}])"));
        why = changed_within(drawn, page(other_words), {a.places[0].base});
        QVERIFY2(why.empty(), why.c_str());
        // B, across: the reading above its base, to its tail
        const auto b = ruby_places(line(doc, "B"), kDpi);
        QVERIFY(b.exact);
        QCOMPARE(b.places.size(), std::size_t{1});
        QVERIFY(!b.places[0].vertical);
        QCOMPARE(b.places[0].letters.size(), std::size_t{4});
        QVERIFY(b.places[0].ruby.y1 <= b.places[0].base.y0 + 2);
        why = changed_within(drawn, page(blank_readings("B")), {b.places[0].ruby});
        QVERIFY2(why.empty(), why.c_str());
        const Document other_base = run_ops(doc, Json::parse(R"([{"op": "edit_line", "id": "B", "text": "京都へ行く", "ruby_spans": [
            {"start": 0, "end": 2, "ruby": "おおさか", "align": "tail", "tracking": 0.2}]}])"));
        why = changed_within(drawn, page(other_base), {b.places[0].base});
        QVERIFY2(why.empty(), why.c_str());
        // C, in a balloon: the boxes hold the ink; each ruby its own size
        const auto c = ruby_places(line(doc, "C"), kDpi);
        QVERIFY(c.exact);
        QCOMPARE(c.places.size(), std::size_t{2});
        QVERIFY(c.places[1].size > c.places[0].size);
        for (const auto& place : c.places) {
            QVERIFY(dark_in(drawn, place.ruby.x0, place.ruby.y0, place.ruby.x1, place.ruby.y1) > 0);
            QVERIFY(dark_in(drawn, place.base.x0, place.base.y0, place.base.x1, place.base.y1) > 0);
            QVERIFY(place.ruby.x0 >= place.base.x1);
            QVERIFY(place.base.x0 >= genko::render::mm_to_px(40, kDpi) && place.base.x1 <= genko::render::mm_to_px(85, kDpi));
            QVERIFY(place.base.y0 >= genko::render::mm_to_px(40, kDpi) && place.base.y1 <= genko::render::mm_to_px(90, kDpi));
        }
        // D: a base over two columns, a place for each part (right to left)
        const auto d = ruby_places(line(doc, "D"), kDpi);
        QCOMPARE(d.places.size(), std::size_t{2});
        QCOMPARE(d.places[0].reading, std::string("えき"));
        QCOMPARE(d.places[1].reading, std::string("まえ"));
        QCOMPARE(d.places[0].span, d.places[1].span);
        QVERIFY(d.places[1].ruby.x1 <= d.places[0].base.x0);
        why = changed_within(drawn, page(blank_readings("D")), {d.places[0].ruby, d.places[1].ruby});
        QVERIFY2(why.empty(), why.c_str());
        // E turned, F on a path, G needing review
        const auto e = ruby_places(line(doc, "E"), kDpi);
        QCOMPARE(e.places.size(), std::size_t{1});
        QVERIFY(!e.exact);
        QVERIFY(ruby_places(line(doc, "F"), kDpi).places.empty());
        QVERIFY(ruby_places(line(doc, "G"), kDpi).places.empty());
    }

    // F4 (FIXTURES.md): four pages built here, saved, read back the same, written as PNG, PDF and CMYK.
    void f4Book() {
        Document doc = blank_book(4);
        doc = run_ops(doc, Json::parse(R"([
            {"op": "add_line", "page": 1, "id": "p1a", "text": "東京と東京", "x_mm": 140, "y_mm": 20, "w_mm": 30, "h_mm": 70, "wrap": "vertical",
             "ruby_spans": [{"start": 3, "end": 5, "ruby": "とうきょう"}]},
            {"op": "add_line", "page": 1, "id": "p1b", "text": "「そうだ。」っていうか、ちょっと待って…！", "x_mm": 100, "y_mm": 20, "w_mm": 35, "h_mm": 60,
             "wrap": "vertical", "balloon": "shout"},
            {"op": "add_line", "page": 1, "id": "p1c", "text": "AIと12時", "x_mm": 60, "y_mm": 20, "w_mm": 30, "h_mm": 50, "wrap": "vertical",
             "style": {"tcy": true}, "style_runs": [["AI", {"tcy": true}]]},
            {"op": "add_line", "page": 1, "id": "p1d", "text": "大きな声で約束する", "x_mm": 20, "y_mm": 20, "w_mm": 35, "h_mm": 70, "wrap": "vertical",
             "style_runs": [["大きな", {"scale": 1.4}], ["声", {"bold": true, "rgb": [200, 30, 30]}]], "emphasis_runs": ["約束"],
             "ruby_spans": [{"start": 4, "end": 5, "ruby": "こえ"}, {"start": 6, "end": 8, "ruby": "やくそく"}]},
            {"op": "add_line", "page": 2, "id": "p2a", "text": "葛\udb40\udd00飾区と辻\ufe00堂へ", "x_mm": 30, "y_mm": 30, "w_mm": 90, "h_mm": 25, "wrap": "horizontal",
             "balloon": "box", "ruby_spans": [{"start": 0, "end": 3, "ruby": "かつしかく", "font": "mincho", "align": "head", "gap_mm": 0.5},
             {"start": 4, "end": 6, "ruby": "つじどう", "align": "tail"}]},
            {"op": "add_line", "page": 2, "id": "p2b", "text": "心の声", "x_mm": 120, "y_mm": 60, "w_mm": 40, "h_mm": 40, "balloon": "thought",
             "ruby_spans": [{"start": 0, "end": 1, "ruby": "こころ"}]},
            {"op": "add_line", "page": 3, "id": "p3a", "text": "ドーン", "x_mm": 40, "y_mm": 40, "w_mm": 80, "h_mm": 40, "wrap": "horizontal",
             "balloon": "sfx", "style": {"outline_mm": 1}},
            {"op": "add_stroke", "page": 3, "layer": "name", "points": [[20, 200], [180, 200]]},
            {"op": "add_layer", "page": 3, "kind": "pen", "id": "hidden-pen"},
            {"op": "set_layers", "page": 3, "ids": ["hidden-pen"], "exportable": false},
            {"op": "add_stroke", "page": 3, "layer_id": "hidden-pen", "points": [[20, 240], [180, 240]], "width_mm": 2},
            {"op": "add_line", "page": 4, "id": "p4a", "text": "縦書きと横書き", "x_mm": 120, "y_mm": 40, "w_mm": 40, "h_mm": 70, "wrap": "vertical",
             "style": {"ruby_scale": 0.4},
             "ruby_spans": [{"start": 0, "end": 3, "ruby": "たてがき", "align": "tail", "tracking": 0.3},
             {"start": 4, "end": 7, "ruby": "よこがき", "scale": 0.6, "font": "maru"}]},
            {"op": "add_line", "page": 4, "id": "p4b", "text": "横書きの台詞", "x_mm": 20, "y_mm": 150, "w_mm": 80, "h_mm": 20, "wrap": "horizontal",
             "ruby_spans": [{"start": 4, "end": 6, "ruby": "せりふ"}]},
            {"op": "set_nombre", "position": "bottom_center", "show": true}
        ])"));
        const auto dir = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/f4.genko");
        save_book(doc, dir);
        const auto loaded = genko::storage::load_document(dir);
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        QVERIFY(loaded.document.read_only_reason.empty());
        QCOMPARE(loaded.document.features, std::vector<std::string>({"ruby-layout@1"}));
        std::string where;
        QVERIFY2(genko::test::strict_equal(story_of(loaded.document, tmp_), story_of(doc, tmp_), &where), where.c_str());
        // the text and its ruby read back character for character
        const Json story = story_of(loaded.document, tmp_);
        QCOMPARE(story[0]["text"].get<std::string>(), std::string("東京と東京"));
        QCOMPARE(story[0]["ruby_spans"][0]["start"], Json(3));
        QCOMPARE(story[4]["ruby_spans"][0]["base"], Json("葛\xf3\xa0\x84\x80飾区"));
        QCOMPARE(story[4]["ruby_spans"][1]["base"], Json("辻\xef\xb8\x80堂"));
        const auto out = genko::storage::path_from_utf8(tmp_.path().toStdString() + "/f4-out");
        const auto pngs = genko::formats::export_print(loaded.document, out, "png", kDpi, 180, true, "paper", "gray");
        const auto pdfs = genko::formats::export_print(loaded.document, out, "pdf", kDpi, 180, true, "paper", "gray");
        const auto cmyk = genko::formats::export_print(loaded.document, out, "cmyk", kDpi, 180, true, "paper");
        QCOMPARE(pngs.size(), std::size_t{4});
        QCOMPARE(pdfs.size(), std::size_t{1});
        QCOMPARE(cmyk.size(), std::size_t{4});
        for (const auto& file : cmyk) {
            const std::string bytes = read_file(QString::fromStdString(file.string()));
            QVERIFY(bytes.size() > 1000 && (bytes.rfind("II*", 0) == 0 || bytes.rfind("MM", 0) == 0));
        }
        const auto pictures = pdf_pictures(read_file(QString::fromStdString(pdfs.front().string())));
        QCOMPARE(pictures.size(), std::size_t{4});
        std::vector<Image> pages;
        for (std::size_t i = 0; i < 4; ++i) {
            pages.push_back(genko::render::read_png(read_file(QString::fromStdString(pngs[i].string()))).convert("L"));
            QVERIFY2(std::get<2>(pictures[i]) == pages[i].tobytes(), std::to_string(i).c_str());
        }
        // page 3: the draft (name) line and the non-printing layer's line are not there, the effect letters are
        const auto px = [](double mm) { return genko::render::mm_to_px(mm, kDpi); };
        QCOMPARE(dark_in(pages[2], px(30), px(198), px(170), px(202)), 0);
        QCOMPARE(dark_in(pages[2], px(30), px(238), px(170), px(242)), 0);
        QVERIFY(dark_in(pages[2], px(40), px(40), px(120), px(80)) > 200);
        // the nombre at the foot of every page
        for (const Image& page : pages) QVERIFY(dark_in(page, px(90), page.height() - px(20), page.width() - px(90), page.height()) > 0);
        // page 1: the reading of 「東京と東京」 beside its second word (the line's lower half), none beside the first
        const Document no_ruby = run_ops(loaded.document, Json::parse(R"([{"op": "edit_line", "id": "p1a", "ruby_spans": null}])"));
        const Image bare = genko::render::render_page(no_ruby.page(0), kDpi, {}, &no_ruby).image.convert("L");
        const Image with = genko::render::render_page(loaded.document.page(0), kDpi, {}, &loaded.document).image.convert("L");
        int top = with.height(), bottom = 0;
        for (int y = px(20); y < px(90); ++y) {
            for (int x = px(140); x < px(170); ++x) {
                if (with.getpixel(x, y).front() != bare.getpixel(x, y).front()) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        QVERIFY(bottom > top);
        QVERIFY2(top > px(20) + px(35) / 2, std::to_string(top).c_str());
    }
};

QTEST_GUILESS_MAIN(TestRubyRender)
#include "test_ruby_render.moc"
