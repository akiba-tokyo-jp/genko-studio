// PC5 G-d (SPEC.md TEXT-02「書体の置き換え」「検索と置換」「縦組みの属性」, render/ops_type_fonts.cpp, core/ops_lines.cpp
// replace_text, render/text/lettering.cpp):
// - replace_fonts: asked (no map, no all), the missing fonts of the book's texts — a line's style, a type layer's own
//   attributes with their names, a ruby's, a style's — and nothing changed; a map puts its fonts in their place (the
//   names of the old one taken away), null and all the default face; a font this computer has, a locked type layer, left as
//   they are; a map to a font the computer has not, an unknown key, refused;
// - a face of a font collection: a .ttc made of the test fonts — its second face (face_index 1) drawn as that font alone,
//   its characters and its family read; a face_index not a whole number refused;
// - replace_text: whole_word; a type@1 book's type layers too (where: page and layer_id), a locked one left; a book of
//   before reports as before;
// - down: the whole text's tracking, leading and alignment and a range's 縦中横 drawn by a type layer set down;
// - a smart object's contents: drawn anew when the book's text gamma changes (their key holds it).

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <cstring>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/type_text.hpp"
#include "render/draw.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/text/shaping.hpp"
#include "service/wording.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

constexpr double kInch = 25.4;
constexpr int kDpi = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
Json report(const core::Document& doc, const Json& op, core::Document* after = nullptr) {
    core::ApplyResult result = run_ops(doc, Json::array({op}));
    if (after != nullptr) *after = result.doc;
    Json out = result.results.empty() ? Json::object() : result.results[0];
    out.erase("index");
    out.erase("op");
    return out;
}
std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.what();
    }
    return {};
}

std::string font(const char* name) { return test::test_data(QStringLiteral("type/%1").arg(QString::fromLatin1(name))).toStdString(); }

core::Document plain_book() {
    core::Document doc = core::new_episode("書体", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, kDpi, "color"));
    auto& p = doc.edit_page(0);
    p.frames.clear();
    p.numero = false;
    p.layers.clear();
    core::Layer paint;
    paint.id = "paint";
    paint.role = core::LayerRole::User;
    paint.kind = core::LayerKind::Raster;
    p.layers.push_back(paint);
    return doc;
}

render::Image drawn(const core::Document& doc) { return render::render_page(doc.page(0), kDpi, render::proof_options(), &doc).image.convert("RGB"); }

// Where the page's letters are (dark pixels): x0, y0, x1, y1 px; none when none.
std::optional<std::array<int, 4>> ink(const render::Image& image, int from_y = 0, int to_y = 1 << 30) {
    const std::string px = image.tobytes();
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = std::max(0, from_y); y < std::min(image.height(), to_y); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = reinterpret_cast<const unsigned char*>(px.data()) + (std::size_t(y) * image.width() + x) * 3;
            if (p[0] + p[1] + p[2] >= 3 * 128) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < 0) return std::nullopt;
    return std::array<int, 4>{x0, y0, x1, y1};
}

const core::StoryLine& layer_record(const core::Document& doc, const std::string& id = "t1") {
    for (const core::Layer& l : doc.page(0).layers) {
        if (l.id == id) return *l.type;
    }
    throw std::runtime_error("no layer " + id);
}

// A font collection (.ttc) of two fonts: each font's tables after the collection's header, their offsets moved.
QByteArray collection(const QByteArray& a, const QByteArray& b) {
    const auto be32 = [](QByteArray& out, std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.append(static_cast<char>((v >> s) & 0xff));
    };
    const auto read16 = [](const QByteArray& f, int at) { return (static_cast<std::uint8_t>(f[at]) << 8) | static_cast<std::uint8_t>(f[at + 1]); };
    const auto read32 = [](const QByteArray& f, int at) {
        return (std::uint32_t(static_cast<std::uint8_t>(f[at])) << 24) | (std::uint32_t(static_cast<std::uint8_t>(f[at + 1])) << 16) |
               (std::uint32_t(static_cast<std::uint8_t>(f[at + 2])) << 8) | std::uint32_t(static_cast<std::uint8_t>(f[at + 3]));
    };
    const auto moved = [&](QByteArray f, std::uint32_t by) {
        const int tables = read16(f, 4);
        for (int i = 0; i < tables; ++i) {
            const int at = 12 + 16 * i + 8;
            QByteArray v;
            be32(v, read32(f, at) + by);
            f.replace(at, 4, v);
        }
        return f;
    };
    const std::uint32_t header = 12 + 4 * 2;
    const std::uint32_t second = header + static_cast<std::uint32_t>((a.size() + 3) / 4 * 4);
    QByteArray out("ttcf");
    be32(out, 0x00010000);
    be32(out, 2);
    be32(out, header);
    be32(out, second);
    out += moved(a, header);
    while (static_cast<std::uint32_t>(out.size()) < second) out.append('\0');
    out += moved(b, second);
    return out;
}

QByteArray file_bytes(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

}  // namespace

class TestContractTypeFonts : public QObject {
    Q_OBJECT

private slots:
    void replaceFonts() {
        core::Document doc = plain_book();
        const std::string gone = "/no/such/folder/Missing-Bold.ttf";
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "台詞"}, {"x_mm", 5}, {"y_mm", 5}, {"style", Json{{"font", gone}}}});
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l2"}, {"text", "ルビ"}, {"x_mm", 5}, {"y_mm", 25}, {"style", Json{{"font", "gothic"}}}});
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "Abc"}, {"point", Json::array({4, 40})}, {"wrap", "horizontal"}, {"new_id", "t1"},
                            {"char", Json{{"font", "/gone/Other.otf"}, {"family", "Other"}, {"postscript", "Other-Regular"}}}});
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "Locked"}, {"point", Json::array({4, 45})}, {"wrap", "horizontal"}, {"new_id", "t2"},
                            {"char", Json{{"font", "/gone/Other.otf"}}}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "t2"}, {"locked", true}});
        doc = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "new"}, {"name", "強調"}, {"attrs", Json{{"font", gone}}}});
        // asked: what is missing, nothing changed
        const Json asked = report(doc, Json{{"op", "replace_fonts"}});
        QCOMPARE(asked["missing"].size(), std::size_t{2});
        QCOMPARE(asked["missing"][0]["font"], Json("/gone/Other.otf"));
        QCOMPARE(asked["missing"][0]["texts"], Json(2));
        QCOMPARE(asked["missing"][0]["postscript"], Json("Other-Regular"));
        QCOMPARE(asked["missing"][1]["font"], Json(gone));
        QCOMPARE(asked["missing"][1]["texts"], Json(1));
        const core::ApplyResult question = run_ops(doc, Json::array({Json{{"op", "replace_fonts"}}}));
        QVERIFY(question.unchanged);
        // a map: its fonts in their place, the old one's names taken away; the locked layer and the font the computer has kept
        core::Document mapped;
        const Json done = report(doc, Json{{"op", "replace_fonts"}, {"map", Json{{"/gone/Other.otf", Json{{"font", "mincho"}}}, {gone, "maru"}}}}, &mapped);
        QCOMPARE(done["replaced"], Json(2));
        QCOMPARE(done["styles"], Json(true));
        QCOMPARE(mapped.story[0].style["font"], Json("maru"));
        QCOMPARE(mapped.story[1].style["font"], Json("gothic"));
        QCOMPARE((*layer_record(mapped).native_type)["char"], (Json{{"font", "mincho"}}));
        QCOMPARE((*layer_record(mapped, "t2").native_type)["char"]["font"], Json("/gone/Other.otf"));
        QCOMPARE((*mapped.type_styles)["character"][0]["char"]["font"], Json("maru"));
        // all: the default face (the font taken away)
        core::Document all;
        report(doc, Json{{"op", "replace_fonts"}, {"all", true}}, &all);
        QVERIFY(!all.story[0].style.contains("font"));
        QVERIFY(!layer_record(all).native_type->contains("char") || !(*layer_record(all).native_type)["char"].contains("font"));
        QVERIFY(!(*all.type_styles)["character"][0].contains("char") || !(*all.type_styles)["character"][0]["char"].contains("font"));
        // refused: a font the computer has not, an unknown key, a map that is not one
        QVERIFY(!refusal(doc, Json{{"op", "replace_fonts"}, {"map", Json{{gone, "/also/missing.ttf"}}}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "replace_fonts"}, {"maps", Json::object()}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "replace_fonts"}, {"map", Json::array()}}).empty());
        // (the window's words)
        const auto words = [&](const Json& op) { return wording::error(QString::fromStdString(refusal(doc, op))); };
        QCOMPARE(words(Json{{"op", "replace_fonts"}, {"maps", 1}}), QStringLiteral("書体の置き換えに知らない鍵（maps）があります"));
        QCOMPARE(words(Json{{"op", "replace_fonts"}, {"map", Json{{gone, "/also/missing.ttf"}}}}), QStringLiteral("置き換える書体「/also/missing.ttf」が、このパソコンにありません"));
        QCOMPARE(words(Json{{"op", "replace_fonts"}, {"map", Json{{gone, Json{{"font", "gothic"}, {"face_index", -1}}}}}}), QStringLiteral("書体の面の番号（face_index）は 0〜255 の整数です"));
        QCOMPARE(words(Json{{"op", "replace_fonts"}, {"map", Json{{gone, 7}}}}), QStringLiteral("置き換える書体の指定が正しくありません"));
        QCOMPARE(words(Json{{"op", "replace_fonts"}, {"all", "yes"}}), QStringLiteral("allの指定が正しくありません"));
        // a book with nothing missing: nothing to say
        QCOMPARE(report(plain_book(), Json{{"op", "replace_fonts"}})["missing"], Json::array());
    }

    void aFaceOfACollection() {
        QTemporaryDir scratch;
        const std::string ttc = scratch.filePath(QStringLiteral("pair.ttc")).toStdString();
        {
            QFile out(QString::fromStdString(ttc));
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(collection(file_bytes(font("inter-subset.ttf")), file_bytes(font("noto-sans-hebrew-subset.ttf"))));
        }
        QCOMPARE(render::text::font_info(ttc, 0).faces, 2U);
        QCOMPARE(QString::fromStdString(render::text::font_info(ttc, 1).family), QString::fromStdString(render::text::font_info(font("noto-sans-hebrew-subset.ttf")).family));
        const auto mapped = render::text::charmap(ttc, 1);
        QVERIFY(std::find(mapped.begin(), mapped.end(), U'א') != mapped.end());
        QVERIFY(render::text::variants_of(ttc, U'א', 1).empty());
        QCOMPARE(render::font_face(render::face_font(ttc, 1)), std::make_pair(ttc, 1U));
        QCOMPARE(render::face_font(ttc, 0), ttc);
        // drawn: the second face as the font alone
        const auto layer = [&](const Json& chars) {
            return run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "אבג"}, {"point", Json::array({10, 20})}, {"wrap", "horizontal"},
                                          {"new_id", "t1"}, {"style", Json{{"size_mm", 8}}}, {"char", chars}});
        };
        const std::string second = drawn(layer(Json{{"font", ttc}, {"face_index", 1}})).tobytes();
        const std::string alone = drawn(layer(Json{{"font", font("noto-sans-hebrew-subset.ttf")}})).tobytes();
        const std::string first = drawn(layer(Json{{"font", ttc}})).tobytes();
        QVERIFY(second == alone);
        QVERIFY(second != first);
        // a face that is not a whole number
        QVERIFY(!refusal(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}, {"char", Json{{"face_index", 1.5}}}}).empty());
        QVERIFY(!refusal(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}, {"char", Json{{"face_index", 256}}}}).empty());
        // replace_fonts gives a face too
        core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "אבג"}, {"point", Json::array({10, 20})}, {"wrap", "horizontal"},
                                                    {"new_id", "t1"}, {"style", Json{{"size_mm", 8}}}, {"char", Json{{"font", "/gone/Hebrew.ttf"}}}});
        doc = run(doc, Json{{"op", "replace_fonts"}, {"map", Json{{"/gone/Hebrew.ttf", Json{{"font", ttc}, {"face_index", 1}, {"family", "Noto Sans Hebrew"}}}}}});
        QCOMPARE((*layer_record(doc).native_type)["char"], (Json{{"font", ttc}, {"family", "Noto Sans Hebrew"}, {"face_index", 1}}));
        QVERIFY(drawn(doc).tobytes() == second);
    }

    void replaceTextWholeWordsAndTypeLayers() {
        core::Document doc = plain_book();
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "cat category cat."}, {"x_mm", 5}, {"y_mm", 5}});
        // a book of before: the report as before
        const Json before = report(doc, Json{{"op", "replace_text"}, {"find", "cat"}, {"replace", "dog"}});
        QCOMPARE(before, (Json{{"replaced", 3}, {"where", Json::array({Json{{"line", "l1"}, {"page", 1}, {"count", 3}, {"text", "dog dogegory dog."}}})}}));
        // whole words
        core::Document whole;
        QCOMPARE(report(doc, Json{{"op", "replace_text"}, {"find", "cat"}, {"replace", "dog"}, {"whole_word", true}}, &whole)["replaced"], Json(2));
        QCOMPARE(whole.story[0].text, std::string("dog category dog."));
        QCOMPARE(report(doc, Json{{"op", "replace_text"}, {"find", "CAT"}, {"replace", "dog"}, {"whole_word", true}, {"case", false}})["replaced"], Json(2));
        QCOMPARE(report(doc, Json{{"op", "replace_text"}, {"find", "c.t"}, {"replace", "x"}, {"whole_word", true}})["replaced"], Json(0));  // (its words as they are)
        // type layers (type@1): replaced and said where; a locked one left
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a cat"}, {"point", Json::array({4, 40})}, {"wrap", "horizontal"}, {"new_id", "t1"}});
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "the cat"}, {"point", Json::array({4, 45})}, {"wrap", "horizontal"}, {"new_id", "t2"}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "t2"}, {"locked", true}});
        core::Document after;
        const Json said = report(doc, Json{{"op", "replace_text"}, {"find", "cat"}, {"replace", "dog"}, {"whole_word", true}}, &after);
        QCOMPARE(said["replaced"], Json(3));
        QCOMPARE(said["where"][1], (Json{{"page", 1}, {"layer_id", "t1"}, {"count", 1}, {"text", "a dog"}}));
        QCOMPARE(layer_record(after).text, std::string("a dog"));
        QCOMPARE(layer_record(after, "t2").text, std::string("the cat"));
        // (only some pages)
        QCOMPARE(report(doc, Json{{"op", "replace_text"}, {"find", "cat"}, {"replace", "dog"}, {"pages", Json::array({2})}})["replaced"], Json(0));
    }

    void downAttributes() {
        const auto vertical = [](const Json& extra) {
            core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "1234時です"}, {"point", Json::array({30, 5})}, {"wrap", "vertical"},
                                                        {"new_id", "t1"}, {"style", Json{{"size_mm", 5}}}});
            if (!extra.is_null()) {
                Json op = extra;
                op["op"] = "set_type";
                op["page"] = 1;
                op["layer_id"] = "t1";
                doc = run(doc, op);
            }
            return doc;
        };
        const auto plain = ink(drawn(vertical(Json())));
        QVERIFY(plain.has_value());
        // a range's 縦中横: two of four digits side by side in one cell — the top cell wider than one upright digit (two or three
        // digits are set so already: the lines' style says so by default)
        const int letter = static_cast<int>(5 / kInch * kDpi);
        const render::Image across = drawn(vertical(Json{{"range", Json::array({0, 2})}, {"tcy", true}}));
        const auto top = [&](const render::Image& image) {
            const auto all = ink(image);
            const auto cell = ink(image, (*all)[1], (*all)[1] + letter * 3 / 4);
            return (*cell)[2] - (*cell)[0];
        };
        const render::Image upright = drawn(vertical(Json()));
        QVERIFY2(across.tobytes() != upright.tobytes(), "the same pixels");
        QVERIFY2(top(across) > top(upright), (std::to_string(top(across)) + " " + std::to_string(top(upright))).c_str());  // (squeezed into the em)
        // the whole text's tracking: a longer column
        const auto tracked = ink(drawn(vertical(Json{{"tracking", 0.5}})));
        QVERIFY((*tracked)[3] - (*plain)[3] > letter);
        // its leading: two columns further apart
        const auto two = [&](const Json& extra) {
            core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "あい\nうえ"}, {"point", Json::array({30, 5})}, {"wrap", "vertical"},
                                                        {"new_id", "t1"}, {"style", Json{{"size_mm", 5}}}});
            if (!extra.is_null()) {
                Json op = extra;
                op["op"] = "set_type";
                op["page"] = 1;
                op["layer_id"] = "t1";
                doc = run(doc, op);
            }
            return ink(drawn(doc));
        };
        const auto near = two(Json());
        const auto far = two(Json{{"leading_mm", 15}});
        QVERIFY(((*far)[2] - (*far)[0]) - ((*near)[2] - (*near)[0]) > letter);
        // its alignment (a box text): bottom
        const auto boxed = [&](const Json& extra) {
            core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "あい"}, {"box", Json::array({20, 5, 10, 40})}, {"wrap", "vertical"},
                                                        {"new_id", "t1"}, {"style", Json{{"size_mm", 5}}}});
            if (!extra.is_null()) {
                Json op = extra;
                op["op"] = "set_type";
                op["page"] = 1;
                op["layer_id"] = "t1";
                doc = run(doc, op);
            }
            return ink(drawn(doc));
        };
        QVERIFY((*boxed(Json{{"align", "right"}}))[1] - (*boxed(Json()))[1] > 2 * letter);
    }

    void smartContentsKeyedByTheBooksLook() {
        core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "あいう"}, {"point", Json::array({5, 20})}, {"wrap", "horizontal"},
                                                    {"new_id", "t1"}, {"style", Json{{"size_mm", 8}, {"rgb", Json::array({200, 40, 40})}}}});
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        const std::string before = drawn(doc).tobytes();
        core::Document gamma = doc;
        gamma.text_gamma = 2.2;
        QVERIFY(drawn(gamma).tobytes() != before);  // (the contents drawn anew with the book's text gamma, not their memory of before)
        QCOMPARE(drawn(doc).tobytes(), before);
    }
};

QTEST_GUILESS_MAIN(TestContractTypeFonts)
#include "test_contract_type_fonts.moc"
