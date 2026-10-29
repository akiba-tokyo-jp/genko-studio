// PC5 G-c (SPEC.md TEXT-02「文字スタイル・段落スタイル」, render/ops_type_styles.cpp): the book's styles as they are drawn and kept.
// - A style changed (スタイルオプション, 再定義) sets the line and the type layer that take it anew: the page's pixels change
//   (the lettering's memo key holds the styles' digest), a point text's box is kept round its letters (and the type layer's
//   extent found anew); Basic Paragraph changed moves the texts set by the OpenType typesetting, never a line of before.
// - A style made and applied in a smart object's contents: drawn through the smart object with the book's styles.
// - Saved and read back: the styles, the texts' references and the pixels; a book whose styles all went written as one that
//   never had any (no type@1); a book without styles listed (a question) as before, byte for byte.
// - The refusals in the window's words (service/wording).

#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "service/wording.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace ts = core::type_styles;

namespace {

constexpr double kInch = 25.4;
constexpr int kDpi = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.what();
    }
    return {};
}

core::Document plain_book() {
    core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, kDpi, "color"));
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

// Pixels in the box (mm: x0, y0, x1, y1) that are red, and that are dark.
struct Count {
    int red = 0;
    int dark = 0;
};
Count count(const render::Image& image, std::array<double, 4> box) {
    const std::string px = image.tobytes();
    const double k = kDpi / kInch;
    Count out;
    for (int y = std::max(0, int(box[1] * k)); y < std::min(image.height(), int(box[3] * k)); ++y) {
        for (int x = std::max(0, int(box[0] * k)); x < std::min(image.width(), int(box[2] * k)); ++x) {
            const auto* p = reinterpret_cast<const unsigned char*>(px.data()) + (std::size_t(y) * image.width() + x) * 3;
            if (p[0] > 150 && p[1] < 90 && p[2] < 90) ++out.red;
            if (p[0] + p[1] + p[2] < 3 * 100) ++out.dark;
        }
    }
    return out;
}
// The box's pixels.
std::string region(const render::Image& image, std::array<double, 4> box) {
    const std::string px = image.tobytes();
    const double k = kDpi / kInch;
    std::string out;
    for (int y = std::max(0, int(box[1] * k)); y < std::min(image.height(), int(box[3] * k)); ++y) {
        const int x0 = std::max(0, int(box[0] * k)), x1 = std::min(image.width(), int(box[2] * k));
        out += px.substr((std::size_t(y) * image.width() + x0) * 3, std::size_t(x1 - x0) * 3);
    }
    return out;
}

constexpr std::array<double, 4> kLayerBox{2, 2, 48, 14};   // (the type layer's letters)
constexpr std::array<double, 4> kLineBox{2, 20, 48, 34};   // (the line's)
constexpr std::array<double, 4> kOldBox{2, 38, 48, 50};    // (a line of before)

// A type layer t1 (a box text across), a line l1 across and a line of before l0, the first two taking a character style
// cs-1 (nothing of its own yet).
core::Document styled_book() {
    core::Document doc = plain_book();
    doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "ABC abc"}, {"box", Json::array({3, 3, 44, 10})}, {"wrap", "horizontal"},
                        {"new_id", "t1"}, {"style", Json{{"size_mm", 6}}}});
    doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "ABC abc"}, {"x_mm", 3}, {"y_mm", 21}, {"w_mm", 44}, {"h_mm", 12},
                        {"balloon", "none"}, {"wrap", "horizontal"}, {"style", Json{{"size_mm", 6}}}});
    doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l0"}, {"text", "ABC abc"}, {"x_mm", 3}, {"y_mm", 39}, {"w_mm", 44}, {"h_mm", 10},
                        {"balloon", "none"}, {"wrap", "horizontal"}, {"style", Json{{"size_mm", 6}}}});
    doc = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "new"}, {"from_selection", false}, {"attrs", Json{{"tracking", 0.05}}}});
    doc = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}, {"id", "cs-1"}, {"page", 1}, {"layer_id", "t1"}});
    doc = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}, {"id", "cs-1"}, {"line", "l1"}});
    return doc;
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string project_of(const core::Document& doc, const QTemporaryDir& scratch, const std::string& name) {
    const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / name;
    write_book(doc, dir);
    return read_text(dir / "project.json");
}

const core::StoryLine& layer_record(const core::Document& doc, const std::string& id = "t1") {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return *l.type;
    throw std::runtime_error("no layer " + id);
}

}  // namespace

class TestContractTypeStyles : public QObject {
    Q_OBJECT

private slots:
    // スタイルオプション: the line and the type layer that take the style drawn anew; the line of before as it was
    void changedStylesDrawnAnew() {
        const core::Document doc = styled_book();
        const render::Image before = drawn(doc);
        QVERIFY(count(before, kLayerBox).dark > 0 && count(before, kLineBox).dark > 0 && count(before, kOldBox).dark > 0);
        QCOMPARE(count(before, kLayerBox).red, 0);
        core::ApplyResult set = run_ops(doc, Json::array({Json{{"op", "type_style"}, {"kind", "character"}, {"action", "set"}, {"id", "cs-1"},
                                                                {"attrs", Json{{"rgb", Json::array({220, 0, 0})}}}}}));
        QCOMPARE(set.results[0]["texts"], Json(2));
        const render::Image after = drawn(set.doc);
        QVERIFY(count(after, kLayerBox).red > 50);
        QVERIFY(count(after, kLineBox).red > 50);
        QCOMPARE(count(after, kOldBox).red, 0);
        QCOMPARE(region(after, kOldBox), region(before, kOldBox));
        // the same book drawn again: the same (the memo's key)
        QCOMPARE(drawn(set.doc).tobytes(), after.tobytes());
        // undone (the book before): as before
        QCOMPARE(drawn(doc).tobytes(), before.tobytes());
        // Basic Paragraph's colour: the texts the OpenType typesetting sets change, never the line of before
        const core::Document basic = run(doc, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "set"}, {"id", "basic"},
                                                   {"attrs", Json{{"rgb", Json::array({220, 0, 0})}}}});
        const render::Image based = drawn(basic);
        QVERIFY(count(based, kLayerBox).red > 50);
        QVERIFY(count(based, kLineBox).red > 50);
        QCOMPARE(region(based, kOldBox), region(before, kOldBox));
        // a style redefined from the text: the others that take it change with it
        core::Document own = run(doc, Json{{"op", "set_type"}, {"line", "l1"}, {"rgb", Json::array({220, 0, 0})}});
        QCOMPARE(count(drawn(own), kLayerBox).red, 0);
        own = run(own, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "redefine"}, {"line", "l1"}});
        QVERIFY(count(drawn(own), kLayerBox).red > 50);
    }

    // a point text taking a style made larger: its box kept round its letters, its letters drawn whole
    void pointTextsGrow() {
        core::Document doc = plain_book();
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "abc"}, {"point", Json::array({4, 12})}, {"wrap", "horizontal"},
                            {"new_id", "t1"}, {"style", Json{{"size_mm", 4}}}});
        doc = run(doc, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "new"}, {"from_selection", false}, {"attrs", Json{{"size_mm", 4}}}});
        doc = run(doc, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "apply"}, {"id", "ps-1"}, {"page", 1}, {"layer_id", "t1"}});
        const double w = layer_record(doc).w_mm.value();
        const int dark = count(drawn(doc), {0, 0, 50, 50}).dark;
        const core::Document big = run(doc, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "set"}, {"id", "ps-1"}, {"attrs", Json{{"size_mm", 10}}}});
        QVERIFY2(layer_record(big).w_mm.value() > 2 * w, qPrintable(QString::number(layer_record(big).w_mm.value())));
        QVERIFY(count(drawn(big), {0, 0, 50, 50}).dark > 4 * dark);
        // the letters drawn whole: none cut at the box of before
        const double right = layer_record(big).x_mm.value() + layer_record(big).w_mm.value();
        QVERIFY(count(drawn(big), {4 + 2 * w, 0, right, 50}).dark > 0);
        // one too large to set: refused
        QVERIFY(refusal(doc, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "set"}, {"id", "ps-1"}, {"attrs", Json{{"size_mm", 1000}}}})
                    .find("too large") != std::string::npos);
    }

    // a smart object's contents: their type layer drawn with the book's styles, a style changed changes it
    void insideASmartObject() {
        core::Document doc = styled_book();
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        QCOMPARE(count(drawn(doc), kLayerBox).red, 0);
        const core::Document red = run(doc, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "set"}, {"id", "cs-1"},
                                                 {"attrs", Json{{"rgb", Json::array({220, 0, 0})}}}});
        QVERIFY(count(drawn(red), kLayerBox).red > 50);
        // deleted: its look kept inside too
        const core::Document gone = run(red, Json{{"op", "type_style"}, {"kind", "character"}, {"action", "delete"}, {"id", "cs-1"}});
        QCOMPARE(region(drawn(gone), kLayerBox), region(drawn(red), kLayerBox));
    }

    // saved and read back; a book whose styles all went written as one that never had any; a question writes nothing
    void savedAndRead() {
        QTemporaryDir scratch;
        const core::Document doc = styled_book();
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "styled.genko";
        write_book(doc, dir);
        const core::Document back = storage::load_document(dir).document;
        QVERIFY(back.type_styles.has_value());
        QCOMPARE(back.type_styles->dump(), doc.type_styles->dump());
        QCOMPARE(*layer_record(back).native_type, *layer_record(doc).native_type);
        QVERIFY(std::find(back.features.begin(), back.features.end(), core::kTypeFeature) != back.features.end());
        QCOMPARE(drawn(back).tobytes(), drawn(doc).tobytes());
        // a book of lines only: a style made and deleted leaves it as it was, byte for byte
        core::Document plain = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l0"}, {"text", "やあ"}, {"x_mm", 10}, {"y_mm", 10}});
        const std::string was = project_of(plain, scratch, "plain.genko");
        core::Document made = run(plain, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "new"}});
        QVERIFY(core::type_text::uses_type(made));
        QVERIFY(project_of(made, scratch, "made.genko").find("type_styles") != std::string::npos);
        made = run(made, Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "delete"}, {"id", "ps-1"}});
        QVERIFY(!made.type_styles);
        QCOMPARE(project_of(made, scratch, "unmade.genko"), was);
        // asked (list): the book given back as it was
        const core::ApplyResult asked = run_ops(plain, Json::array({Json{{"op", "type_style"}, {"kind", "character"}, {"action", "list"}}}));
        QVERIFY(asked.unchanged);
        QCOMPARE(project_of(asked.doc, scratch, "asked.genko"), was);
        QCOMPARE(drawn(asked.doc).tobytes(), drawn(plain).tobytes());
    }

    // the refusals in the window's words
    void refusalsInWords() {
        const core::Document doc = styled_book();
        const auto words = [&](const Json& op) { return wording::error(QString::fromStdString(refusal(doc, op))); };
        const Json base{{"op", "type_style"}, {"kind", "character"}};
        const auto with = [&](Json more) {
            Json op = base;
            for (const auto& [key, value] : more.items()) op[key] = value;
            return op;
        };
        const std::vector<std::pair<Json, QString>> cases{
            {with(Json{{"action", "make"}}), QStringLiteral("スタイルの操作（action）")},
            {Json{{"op", "type_style"}, {"kind", "x"}, {"action", "list"}}, QStringLiteral("スタイルの種類（kind）")},
            {with(Json{{"action", "delete"}}), QStringLiteral("スタイル（id）を指定します")},
            {with(Json{{"action", "delete"}, {"id", "cs-9"}}), QStringLiteral("文字スタイル（cs-9）は原稿にありません")},
            {Json{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "delete"}, {"id", "basic"}}, QStringLiteral("[基本段落] は削除")},
            {with(Json{{"action", "new"}, {"attrs", Json{{"align", "center"}}}}), QStringLiteral("文字スタイルには段落の属性")},
            {with(Json{{"action", "new"}, {"name", "[なし]"}}), QStringLiteral("パネルが使う名前")},
            {with(Json{{"action", "new"}, {"name", "文字スタイル 1"}}), QStringLiteral("という名前の文字スタイルはもうあります")},
            {with(Json{{"action", "new"}, {"name", ""}}), QStringLiteral("スタイルの名前は 1〜64 文字です")},
            {with(Json{{"action", "apply"}, {"id", "cs-1"}}), QStringLiteral("台詞の line か、テキストレイヤーの page と layer_id")},
            {with(Json{{"action", "apply"}, {"line", "l0"}, {"range", Json::array({1, 1})}}), QStringLiteral("空の範囲には段落スタイルだけ")},
            {with(Json{{"action", "redefine"}, {"line", "l0"}}), QStringLiteral("文字スタイルがありません")},
            {with(Json{{"action", "list"}, {"bogus", 1}}), QStringLiteral("知らない鍵（bogus）")},
            {with(Json{{"action", "new"}, {"attrs", 3}}), QStringLiteral("スタイルの属性（attrs）")},
            {with(Json{{"action", "set"}, {"id", "cs-1"}, {"clear", 3}}), QStringLiteral("消す属性（clear）")},
            {with(Json{{"action", "rename"}, {"id", "cs-1"}}), QStringLiteral("新しい名前（name）")},
            {with(Json{{"action", "new"}, {"apply", 3}}), QStringLiteral("apply は true か false")},
            {with(Json{{"action", "list"}, {"range", Json::array({0, 1})}}), QStringLiteral("範囲（range）は")},
        };
        for (const auto& [op, expected] : cases) {
            const QString said = words(op);
            QVERIFY2(said.contains(expected), qPrintable(QString::fromStdString(op.dump()) + " → " + said));
        }
    }
};

QTEST_GUILESS_MAIN(TestContractTypeStyles)
#include "test_contract_type_styles.moc"
