// PC5 G-0 (type@1, SPEC.md TEXT-02, schema-v4.md「文字」): the type layer and the type record through the ops, the disk and
// the drawing — a book without type@1 written and drawn as before (a type layer added and taken away again: the same bytes);
// a type layer, a line's native_type and the book's type styles saved and read back as they were, the book naming type@1;
// a record not in its normal form or a type layer holding pixels: the book read-only; a type layer drawn as the line of its
// record is (the same pixels within the rounding of its own picture), among the layers (its opacity, under a layer over
// it), alike on an 8-bit, a 16-bit, a grey and a CMYK page; its text edited (the ranges followed), its attributes resolved
// (type_info), moved, rasterized, its page duplicated; strokes refused on it; the later groups' ops registered and refused
// by name.

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/type_text.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::image_ops::Rgba;
namespace fs = std::filesystem;

namespace {

constexpr double kInch = 25.4;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A colour page of 2 inches square (200 dpi), no panels, no nombre, one empty paint layer.
core::Document plain_book() {
    core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
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

const Json kBox = Json::array({8, 8, 30, 30});

core::Document with_type(const core::Document& doc, const std::string& id = "t1", const std::string& text = "あいう") {
    return run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", text}, {"box", kBox}, {"new_id", id}, {"style", Json{{"size_mm", 8}}}});
}

const core::Layer* layer_of(const core::Document& doc, const std::string& id, std::size_t page = 0) {
    for (const core::Layer& l : doc.page(page).layers)
        if (l.id == id) return &l;
    return nullptr;
}

std::string proof_bytes(const core::Document& doc) { return render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.tobytes(); }

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

// The largest difference of two pictures' bytes.
int worst(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return 256;
    int out = 0;
    for (std::size_t i = 0; i < a.size(); ++i) out = std::max(out, std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i]))));
    return out;
}

std::vector<Rgba> shown(const core::Document& doc) { return render::image_ops::rgba_of(render::image_ops::page_pixels(doc, doc.page(0))); }

double darkness(const std::vector<Rgba>& px) {
    double sum = 0;
    for (const Rgba& c : px) sum += 1.0 - (c[0] + c[1] + c[2]) / 3.0;
    return sum / static_cast<double>(px.size());
}

bool has_feature(const core::Document& doc) {
    return std::find(doc.features.begin(), doc.features.end(), core::kTypeFeature) != doc.features.end();
}

}  // namespace

class TestContractType : public QObject {
    Q_OBJECT

private slots:
    // a book without type@1: nothing new written; a type layer added and taken away again: the same bytes, no feature
    void withoutTypeAsBefore() {
        QTemporaryDir scratch;
        core::Document doc = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "やあ"}, {"x_mm", 10}, {"y_mm", 10}});
        const std::string before = project_of(doc, scratch, "plain.genko");
        for (const char* key : {"type@1", "native_type", "type_styles", "\"type\""}) QVERIFY2(before.find(key) == std::string::npos, key);
        QVERIFY(!core::type_text::uses_type(doc));
        const std::string drawn = proof_bytes(doc);
        const core::Document typed = with_type(doc);
        QVERIFY(has_feature(typed));
        QVERIFY(proof_bytes(typed) != drawn);
        const core::Document gone = run(typed, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "t1"}});
        QVERIFY(!core::type_text::uses_type(gone));
        QCOMPARE(proof_bytes(gone), drawn);
        std::string after = project_of(gone, scratch, "plain2.genko");
        // (the revision and the book id are the book's own: the rest the same bytes)
        QVERIFY(after.find("type@1") == std::string::npos);
        QCOMPARE(after.size(), before.size());
        // a line given native_type and then none again: as before
        const core::Document set = run(doc, Json{{"op", "edit_line"}, {"id", "l1"}, {"native_type", Json{{"antialias", "none"}}}});
        QVERIFY(has_feature(set) && set.story[0].native_type);
        const core::Document unset = run(set, Json{{"op", "edit_line"}, {"id", "l1"}, {"native_type", nullptr}});
        QVERIFY(!unset.story[0].native_type && !core::type_text::uses_type(unset));
        QCOMPARE(project_of(unset, scratch, "plain3.genko").size(), before.size());
    }

    // saved and read back as they were: the type layer's record and settings, the line's native_type, the styles
    void savedAndRead() {
        QTemporaryDir scratch;
        core::Document doc = with_type(plain_book());
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かきく"}, {"x_mm", 10}, {"y_mm", 10},
                            {"native_type", Json{{"runs", Json::array({Json{{"start", 1}, {"end", 2}, {"char", Json{{"underline", true}}}}})}}}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "t1"}, {"opacity", 0.5}});
        doc.type_styles = core::type_text::check_type_styles(Json{{"character", Json::array({Json{{"id", "cs-1"}, {"name", "強調"}, {"char", Json{{"caps", "all"}}}}})}});
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "typed.genko";
        write_book(doc, dir);
        const std::string written = read_text(dir / "project.json");
        QVERIFY(written.find("\"type@1\"") != std::string::npos);
        const storage::LoadResult read = storage::load_document(dir);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        const core::Layer* t = layer_of(read.document, "t1");
        QVERIFY(t != nullptr && t->kind == core::LayerKind::Type && t->type);
        QCOMPARE(t->type->text, std::string("あいう"));
        QCOMPARE(t->type->balloon, std::string("none"));
        QCOMPARE(t->type->style, (Json{{"size_mm", 8}}));
        QCOMPARE(t->opacity, 0.5);
        QCOMPARE(t->role, core::LayerRole::Text);
        QCOMPARE(read.document.story[0].native_type->dump(), doc.story[0].native_type->dump());
        QCOMPARE(read.document.type_styles->dump(), doc.type_styles->dump());
        QCOMPARE(proof_bytes(read.document), proof_bytes(doc));
        // written again: the same bytes
        const fs::path again = core::path_from_utf8(scratch.path().toStdString()) / "again.genko";
        write_book(read.document, again);
        std::string a = read_text(again / "project.json"), b = written;
        const auto revision = [](std::string& s) {  // (the book's revision and when it was written are its own)
            for (const char* key : {"\"revision\"", "\"written_at\""}) {
                const auto at = s.find(key);
                if (at != std::string::npos) s.erase(at, s.find('\n', at) - at);
            }
        };
        revision(a);
        revision(b);
        QCOMPARE(a, b);
    }

    // what is not as the writer writes it: the book opens read-only (never written without it)
    void readOnlyWhenBroken() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "broken.genko";
        write_book(with_type(plain_book()), dir);
        const std::string good = read_text(dir / "project.json");
        const auto opened = [&](const std::string& from, const std::string& to) {
            std::string text = good;
            const auto at = text.find(from);
            if (at == std::string::npos) return std::string("(not found)");
            text.replace(at, from.size(), to);
            std::ofstream(dir / "project.json", std::ios::binary) << text;
            return storage::load_document(dir).document.read_only_reason;
        };
        QVERIFY(!opened("\"balloon\": \"none\"", "\"balloon\": \"none\", \"native_type\": {\"antialias\": \"smooth\"}").empty());
        QVERIFY(!opened("\"balloon\": \"none\"", "\"balloon\": \"none\", \"native_type\": {\"char_style\": \"cs-1\"}").empty());  // (a style not kept)
        QVERIFY(!opened("\"type@1\"", "\"type@2\"").empty());  // (a version this build does not know)
        std::ofstream(dir / "project.json", std::ios::binary) << good;
        QVERIFY(storage::load_document(dir).document.read_only_reason.empty());
    }

    // drawn as the line of its record is drawn (its own picture: within its rounding), among the layers
    void drawnAsItsLine() {
        const core::Document as_line = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 8}, {"y_mm", 8},
                                                              {"w_mm", 30}, {"h_mm", 30}, {"balloon", "none"}, {"style", Json{{"size_mm", 8}}}});
        const core::Document as_layer = with_type(plain_book());
        const std::string line_px = proof_bytes(as_line), layer_px = proof_bytes(as_layer);
        QVERIFY(line_px != proof_bytes(plain_book()));
        QVERIFY2(worst(line_px, layer_px) <= 2, qPrintable(QString::number(worst(line_px, layer_px))));
        // its opacity is a layer's
        const core::Document faint = run(as_layer, Json{{"op", "set_layer"}, {"page", 1}, {"id", "t1"}, {"opacity", 0.0}});
        QCOMPARE(proof_bytes(faint), proof_bytes(plain_book()));
        // a part of the page: the whole page cut there
        render::RenderOptions options = render::proof_options();
        const render::Image whole = render::render_page(as_layer.page(0), 200, options, &as_layer).image;
        options.region = render::RenderRegion{100, 90, 150, 120};
        const render::Image part = render::render_page(as_layer.page(0), 200, options, &as_layer).image;
        QCOMPARE(part.tobytes(), whole.crop(render::Box{100, 90, 250, 210}).tobytes());
    }

    // the three ways alike: an 8-bit, a 16-bit, a grey and a CMYK page show its letters as dark
    void threeWays() {
        const core::Document base = with_type(plain_book());
        const double eight = darkness(shown(base));
        QVERIFY(eight > 0.01);
        for (const Json& mode : {Json{{"mode", "rgb"}, {"depth", 16}}, Json{{"mode", "gray"}}, Json{{"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}}}) {
            Json op = mode;
            op["op"] = "set_color_mode";
            op["page"] = 1;
            const core::Document doc = run(plain_book(), op);
            const core::Document typed = with_type(doc);
            const double d = darkness(shown(typed));
            QVERIFY2(std::abs(d - eight) < 0.15 * eight, qPrintable(QString::fromStdString(mode.dump()) + QStringLiteral(": %1 against %2").arg(d).arg(eight)));
        }
    }

    // edit_type's text: a type layer's ranges and rubies followed; a line's through edit_line; type_info's attributes
    void editAndInfo() {
        core::Document doc = with_type(plain_book());
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かきく"}, {"x_mm", 10}, {"y_mm", 10},
                            {"native_type", Json{{"runs", Json::array({Json{{"start", 1}, {"end", 2}, {"char", Json{{"underline", true}}}}})}}}});
        doc = run(doc, Json{{"op", "edit_type"}, {"line", "l1"}, {"text", "XかきくY"}});
        QCOMPARE(doc.story[0].text, std::string("XかきくY"));
        QCOMPARE((*doc.story[0].native_type)["runs"][0]["start"], Json(2));
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"text", "あいうえ"}});
        QCOMPARE(layer_of(doc, "t1")->type->text, std::string("あいうえ"));
        const Json info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "t1"}}})).report;
        QCOMPARE(info["text"], Json("あいうえ"));
        QCOMPARE(info["graphemes"], Json(4));
        QCOMPARE(info["shape"], Json("line"));
        QCOMPARE(info["runs"][0]["char"]["size_mm"], Json(8));
        QCOMPARE(info["paragraphs"].size(), std::size_t{1});
        const Json line_info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"line", "l1"}}})).report;
        QCOMPARE(line_info["runs"][1]["char"]["underline"], Json(true));
        // refused: no text, not a type layer, empty text
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "paint"}, {"text", "x"}}})).find("is not a type layer") !=
                std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"text", ""}}})).find("must not be empty") !=
                std::string::npos);
        // a type layer holds no lines of its own
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "t1"}, {"stabilize", 0}, {"points", Json::array({Json::array({1, 1}), Json::array({5, 5})})}}}))
                    .find("rasterize the type layer") != std::string::npos);
    }

    // moved (transform_layers' move, align), rasterized (rasterize_layer), its page duplicated; turned: G-a's
    void otherOps() {
        const core::Document doc = with_type(plain_book());
        const core::Document moved = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"move", Json::array({5, 3})}});
        QCOMPARE(layer_of(moved, "t1")->type->x_mm.value(), 13.0);
        QCOMPARE(layer_of(moved, "t1")->type->y_mm.value(), 11.0);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"turn", "180"}}}))
                    .starts_with("not_yet_ported"));
        const core::Document flat = run(doc, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "t1"}});
        const core::Layer* r = layer_of(flat, "t1");
        QVERIFY(r != nullptr && r->kind == core::LayerKind::Raster && !r->type);
        QVERIFY(!core::type_text::uses_type(flat));
        QVERIFY2(worst(proof_bytes(flat), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(flat), proof_bytes(doc)))));
        const core::Document two = run(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(two.pages.size(), std::size_t{2});
        bool copied = false;
        for (const core::Layer& l : two.page(1).layers) copied = copied || (l.kind == core::LayerKind::Type && l.type && l.type->text == "あいう");
        QVERIFY(copied);
    }

    // the later groups' ops: known by name, refused as not yet in this build
    void laterOpsRefused() {
        const core::Document doc = with_type(plain_book());
        for (const char* name : {"set_type", "type_style", "line_to_type_layer", "type_layer_to_line", "replace_fonts"}) {
            const std::string why = refusal(doc, Json::array({Json{{"op", name}, {"page", 1}, {"layer_id", "t1"}}}));
            QVERIFY2(why.starts_with("not_yet_ported") && why.find(std::string(name) + " is not in the C++ build yet") != std::string::npos, why.c_str());
        }
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}}})).starts_with("not_yet_ported"));
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}}})).find("box [x, y, w, h] (mm) is required") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestContractType)
#include "test_contract_type.moc"
