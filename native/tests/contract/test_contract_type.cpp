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
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/type_text.hpp"
#include "formats/export.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/pcraft_read.hpp"
#include "render/psd.hpp"
#include "render/type_layer.hpp"
#include "render/text/lettering.hpp"
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

// PC5 G-a: where the page's letters are drawn (pixels darker than half, at 200 dpi): x0, y0, x1, y1 mm; none when none.
std::optional<std::array<double, 4>> ink_mm(const core::Document& doc) {
    const render::Image image = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.convert("RGB");
    const std::string bytes = image.tobytes();
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + (std::size_t(y) * image.width() + x) * 3;
            if (p[0] + p[1] + p[2] >= 3 * 128) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 < 0) return std::nullopt;
    const double k = kInch / 200.0;
    return std::array<double, 4>{x0 * k, y0 * k, x1 * k, y1 * k};
}

// The grey levels a page shows (its distinct ones).
std::size_t levels(const core::Document& doc) {
    const std::string bytes = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.convert("L").tobytes();
    std::array<bool, 256> seen{};
    for (const char c : bytes) seen[static_cast<unsigned char>(c)] = true;
    return static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
}

core::Document with_point(const core::Document& doc, const std::string& wrap, const Json& at, const std::string& text, double size = 5,
                          const Json& more = Json::object()) {
    Json op{{"op", "add_type_layer"}, {"page", 1}, {"text", text}, {"point", at}, {"wrap", wrap}, {"new_id", "p1"}, {"style", Json{{"size_mm", size}}}};
    for (const auto& [key, value] : more.items()) op[key] = value;
    return run(doc, op);
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
        // (the same bytes as never typed)
        QVERIFY(after.find("type@1") == std::string::npos);
        QCOMPARE(after, before);
        // a line given native_type and then none again: as before
        const core::Document set = run(doc, Json{{"op", "edit_line"}, {"id", "l1"}, {"native_type", Json{{"antialias", "none"}}}});
        QVERIFY(has_feature(set) && set.story[0].native_type);
        const core::Document unset = run(set, Json{{"op", "edit_line"}, {"id", "l1"}, {"native_type", nullptr}});
        QVERIFY(!unset.story[0].native_type && !core::type_text::uses_type(unset));
        QCOMPARE(project_of(unset, scratch, "plain3.genko"), before);
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
        QVERIFY(!opened("\"shape\": \"box\"", "\"shape\": \"box\", \"antialias\": \"smooth\"").empty());  // (G-a: a box text — the default written)
        QVERIFY(!opened("\"shape\": \"box\"", "\"shape\": \"box\", \"char_style\": \"cs-1\"").empty());  // (a style not kept)
        QVERIFY(!opened("\"type@1\"", "\"type@2\"").empty());  // (a version this build does not know)
        std::ofstream(dir / "project.json", std::ios::binary) << good;
        QVERIFY(storage::load_document(dir).document.read_only_reason.empty());
    }

    // drawn as the line of its record is drawn (its own picture: within its rounding), among the layers
    void drawnAsItsLine() {
        // (G-a: add_type_layer's box makes a box text — 段落テキスト —, the line holding the same record)
        const core::Document as_line = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 8}, {"y_mm", 8},
                                                              {"w_mm", 30}, {"h_mm", 30}, {"balloon", "none"}, {"style", Json{{"size_mm", 8}}},
                                                              {"native_type", Json{{"shape", "box"}}}});
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
        QVERIFY2(eight > 0.005, qPrintable(QString::number(eight)));
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
        QVERIFY(doc.story[0].native_type);
        QCOMPARE((*doc.story[0].native_type)["runs"][0]["start"], Json(2));
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"text", "あいうえ"}});
        QCOMPARE(layer_of(doc, "t1")->type->text, std::string("あいうえ"));
        const Json info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "t1"}}})).results[0];
        QCOMPARE(info["text"], Json("あいうえ"));
        QCOMPARE(info["graphemes"], Json(4));
        QCOMPARE(info["shape"], Json("box"));  // (G-a: add_type_layer's box is a box text)
        QCOMPARE(info["runs"][0]["char"]["size_mm"], Json(8));
        QCOMPARE(info["paragraphs"].size(), std::size_t{1});
        const Json line_info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"line", "l1"}}})).results[0];
        QCOMPARE(line_info["runs"][1]["char"]["underline"], Json(true));
        // refused: no text, not a type layer, empty text
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "paint"}, {"text", "x"}}})).find("is not a type layer") !=
                std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"text", ""}}})).find("must not be empty") !=
                std::string::npos);
        // a type layer takes no lines (add_stroke says so; the bus refuses any op that leaves pixels or lines on one)
        const std::string drawn_on = refusal(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"layer_id", "t1"}, {"stabilize", 0}, {"points", Json::array({Json::array({1, 1}), Json::array({5, 5})})}}}));
        QVERIFY2(drawn_on.find("this layer cannot take pen lines") != std::string::npos, drawn_on.c_str());
        core::Document patched = doc;
        for (core::Layer& l : patched.edit_page(0).layers)
            if (l.id == "t1") l.patches.push_back(core::Patch{});
        bool refused = false;
        try {
            core::type_text::validate_type(patched);
        } catch (const core::Error& error) {
            refused = std::string(error.what()).find("rasterize the type layer t1 first") != std::string::npos;
        }
        QVERIFY(refused);
    }

    // moved (transform_layers' move, align), rasterized (rasterize_layer), its page duplicated; turned: its matrix (G-a)
    void otherOps() {
        const core::Document doc = with_type(plain_book());
        const core::Document moved = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"move", Json::array({5, 3})}});
        QCOMPARE(layer_of(moved, "t1")->type->x_mm.value(), 13.0);
        QCOMPARE(layer_of(moved, "t1")->type->y_mm.value(), 11.0);
        // (G-a: turned, the text kept as set — its box where it was —, the turn in its matrix)
        const core::Document turned = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"turn", "180"}});
        QCOMPARE(layer_of(turned, "t1")->type->x_mm.value(), 8.0);
        QVERIFY(core::type_text::transform_of(*layer_of(turned, "t1")->type).has_value());
        const core::Document flat = run(doc, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "t1"}});
        const core::Layer* r = layer_of(flat, "t1");
        QVERIFY(r != nullptr && r->kind == core::LayerKind::Raster && !r->type);
        QVERIFY(!core::type_text::uses_type(flat));
        QVERIFY2(worst(proof_bytes(flat), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(flat), proof_bytes(doc)))));
        // テキストレイヤーをラスタライズ (rasterize_lines' layer_id): the same
        const core::Document flat2 = run(doc, Json{{"op", "rasterize_lines"}, {"page", 1}, {"layer_id", "t1"}});
        QCOMPARE(proof_bytes(flat2), proof_bytes(flat));
        // 作業用パスを作成 (text_to_path) of a type layer's text: the page's work path
        const core::Document path = run(doc, Json{{"op", "text_to_path"}, {"page", 1}, {"id", "t1"}});
        QVERIFY(path.page(0).work_path && !path.page(0).work_path->subpaths.empty());
        const core::Document two = run(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(two.pages.size(), std::size_t{2});
        bool copied = false;
        for (const core::Layer& l : two.page(1).layers) copied = copied || (l.kind == core::LayerKind::Type && l.type && l.type->text == "あいう");
        QVERIFY(copied);
    }

    // the later groups' ops: known by name, refused as not yet in this build
    void laterOpsRefused() {
        const core::Document doc = with_type(plain_book());
        for (const char* name : {"set_type", "type_style", "replace_fonts"}) {  // (G-a wrote line_to_type_layer, type_layer_to_line)
            const std::string why = refusal(doc, Json::array({Json{{"op", name}, {"page", 1}, {"layer_id", "t1"}}}));
            QVERIFY2(why.starts_with("not_yet_ported") && why.find(std::string(name) + " is not in the C++ build yet") != std::string::npos, why.c_str());
        }
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}}})).find("point [x, y] or box [x, y, w, h] (mm) is required") !=
                std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"box", kBox}, {"balloon", "foo"}}})).find("balloon must be one of") !=
                std::string::npos);
    }

    // (review P1-1) the layered exports: the type layer a pixel layer of its name, its picture its letters — PSD and .pcraft
    void layeredExports() {
        const core::Document doc = with_type(plain_book());
        QTemporaryDir scratch;
        const fs::path psd = core::path_from_utf8(scratch.path().toStdString()) / "typed.psd";
        formats::export_page_psd(doc, doc.page(0), psd, 200);
        const render::psd::File file = render::psd::File::read(read_text(psd));
        bool found = false;
        for (const render::psd::Layer& layer : file.layers()) {
            if (layer.name != "あいう") continue;
            found = true;
            QCOMPARE(layer.kind, std::string("pixels"));
            QVERIFY(file.image(layer).getbbox().has_value());
        }
        QVERIFY(found);
        std::vector<std::string> notes;
        const render::pcraft::Read read = render::pcraft::read(formats::pcraft_bytes(doc, 0, formats::PcraftOptions{}, &notes), "typed");
        bool kept = false;
        for (const core::Layer& layer : read.doc.page(0).layers)
            kept = kept || (layer.title == "あいう" && layer.raster_png && !layer.raster_png->empty());
        QVERIFY(kept);
    }

    // (review P1-2, P2-1) flattened, merged down (into the type layer or onto it), converted: the letters in the pixels,
    // no type layer left behind
    void mergesAndConversions() {
        const core::Document doc = with_type(plain_book());
        const double dark = darkness(shown(doc));
        const core::Document flat = run(doc, Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}, {"flatten", true}});
        QVERIFY2(std::abs(darkness(shown(flat)) - dark) < 0.1 * dark, qPrintable(QString::number(darkness(shown(flat)))));
        QVERIFY(!core::type_text::uses_type(flat));
        const core::Document down = run(doc, Json{{"op", "merge_down"}, {"page", 1}, {"id", "t1"}});
        QVERIFY(layer_of(down, "t1") == nullptr && !core::type_text::uses_type(down));
        QVERIFY2(worst(proof_bytes(down), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(down), proof_bytes(doc)))));
        core::Document over = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"name", "上"}});
        const std::string top = over.page(0).layers.back().id;
        over = run(over, Json{{"op", "merge_down"}, {"page", 1}, {"id", top}});
        QVERIFY(layer_of(over, "t1") != nullptr && layer_of(over, "t1")->kind == core::LayerKind::Raster && !layer_of(over, "t1")->type);
        QVERIFY2(worst(proof_bytes(over), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(over), proof_bytes(doc)))));
        const core::Document paint = run(doc, Json{{"op", "convert_layer"}, {"page", 1}, {"id", "t1"}, {"to", "paint"}});
        QVERIFY(layer_of(paint, "t1")->kind == core::LayerKind::Raster && !layer_of(paint, "t1")->type);
        QVERIFY2(worst(proof_bytes(paint), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(paint), proof_bytes(doc)))));
    }

    // (review P2-2, P2-3) one type layer a smart object (its contents a page with it: type@1 named); moved with its linked mask
    void smartObjectAndMask() {
        const core::Document doc = with_type(plain_book());
        const core::Document smart = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        QVERIFY(core::type_text::uses_type(smart));
        QVERIFY2(worst(proof_bytes(smart), proof_bytes(doc)) <= 2, qPrintable(QString::number(worst(proof_bytes(smart), proof_bytes(doc)))));
        const core::Document masked = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "t1"}, {"linked", true},
                                                    {"area", Json{{"rect", Json::array({22, 8, 16, 30})}}}});  // (G-a: a box text down, its column at the box's right)
        const core::Document moved = run(masked, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"move", Json::array({5, 0})}});
        QVERIFY(layer_of(moved, "t1")->mask && layer_of(masked, "t1")->mask);
        QVERIFY(*layer_of(moved, "t1")->mask->png != *layer_of(masked, "t1")->mask->png);  // (the mask went with the letters)
    }

    // (review P2-4) Canvas Size: the type layer moves with the page as the line does
    void pageSize() {
        core::Document doc = with_type(plain_book());
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"x_mm", 8}, {"y_mm", 8}, {"w_mm", 30}, {"h_mm", 30}});
        const core::Document wide = run(doc, Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_mm", 10}, {"relative", true}, {"anchor", "right"}});
        QCOMPARE(layer_of(wide, "t1")->type->x_mm.value(), wide.story[0].x_mm.value());
        QVERIFY2(layer_of(wide, "t1")->type->x_mm.value() > 8.5, qPrintable(QString::number(layer_of(wide, "t1")->type->x_mm.value())));  // (moved with the page)
    }

    // (review P1-3) the type ops that name a line look at its page's lock
    void lockedPage() {
        core::Document doc = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"x_mm", 8}, {"y_mm", 8}});
        doc = core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", "lock_page"}, {"page", 1}, {"agent", "human:a"}}}), core::Actor("human:a")).doc;
        try {
            core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", "edit_type"}, {"line", "l1"}, {"text", "あ"}}}), core::Actor("ai:x"));
            QFAIL("edit_type on a page another has locked");
        } catch (const core::ApplyError& error) {
            QVERIFY2(std::string(error.what()).find("locked") != std::string::npos, error.what());
        }
        // (its own page given or not: the line's page is the one looked at)
        try {
            core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", "edit_type"}, {"page", 2}, {"line", "l1"}, {"text", "あ"}}}), core::Actor("ai:x"));
            QFAIL("edit_type with another page given");
        } catch (const core::ApplyError& error) {
            QVERIFY2(std::string(error.what()).find("locked") != std::string::npos, error.what());
        }
    }

    // (review P2-8) type_info is a question: the book as it was, nothing to journal; never with ops that change the book
    void questions() {
        const core::Document doc = with_type(plain_book());
        const core::ApplyResult asked = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "t1"}}}));
        QVERIFY(asked.unchanged);
        QVERIFY(asked.journal_ops.empty());
        QCOMPARE(asked.doc.revision, doc.revision);
        QVERIFY(asked.doc.pages[0] == doc.pages[0]);  // (the same page: nothing copied)
        QCOMPARE(asked.results.size(), std::size_t{1});
        QVERIFY(!run_ops(doc, Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}})).unchanged);
        const std::string mixed = refusal(doc, Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}, Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "t1"}}}));
        QVERIFY2(mixed.find("type_info is a question") != std::string::npos, mixed.c_str());
        // through the command line (no --dry-run): the answer, the book's revision and journal as they were
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "asked.genko";
        write_book(doc, dir);
        const std::string project = read_text(dir / "project.json");
        const std::string journal = read_text(dir / "studio" / "journal.jsonl");
        const QString book = QString::fromStdString(core::path_to_utf8(dir));
        const test::Run asked_cli = test::run_genko({QStringLiteral("apply"), book, QStringLiteral("-")}, {},
                                                    QByteArray(R"([{"op": "type_info", "page": 1, "layer_id": "t1"}])"));
        QVERIFY2(asked_cli.exit_code == 0, qPrintable(asked_cli.described()));
        const Json reply = test::one_line(asked_cli.out);
        QCOMPARE(reply["results"][0]["text"], Json("あいう"));
        QCOMPARE(read_text(dir / "project.json"), project);
        QCOMPARE(read_text(dir / "studio" / "journal.jsonl"), journal);
    }

    // --- PC5 G-a -------------------------------------------------------------------------------------------------------------

    // a point text (ポイントテキスト): across, its first row's baseline on its anchor, from its left (centred, right by its
    // alignment), never broken; down, its first column's middle on the anchor, from its top; its box round its letters
    void pointText() {
        const core::Document across = with_point(plain_book(), "horizontal", Json::array({10, 20}), "HIH");
        const auto ink = ink_mm(across);
        QVERIFY(ink.has_value());
        QVERIFY2(std::abs((*ink)[0] - 10) < 0.7 && std::abs((*ink)[3] - 20) < 0.7, qPrintable(QString("%1 %2").arg((*ink)[0]).arg((*ink)[3])));
        const core::StoryLine& record = *layer_of(across, "p1")->type;
        QCOMPARE(core::type_text::transform_of(record).has_value(), false);
        QVERIFY2(std::abs(record.x_mm.value() - (*ink)[0]) < 0.6 && std::abs(record.y_mm.value() + record.h_mm.value() - (*ink)[3]) < 1.5,
                 qPrintable(QString("%1 %2").arg(record.x_mm.value()).arg(record.y_mm.value())));
        const core::Document centred = with_point(plain_book(), "horizontal", Json::array({25, 20}), "HIH", 5, Json{{"style", Json{{"size_mm", 5}, {"align", "center"}}}});
        const auto mid = ink_mm(centred);
        QVERIFY2(std::abs(((*mid)[0] + (*mid)[2]) / 2 - 25) < 0.7, qPrintable(QString::number(((*mid)[0] + (*mid)[2]) / 2)));
        // never broken: a long row stays one row (past the page's edge)
        const core::Document longer = with_point(plain_book(), "horizontal", Json::array({5, 20}), "HHHHHHHHHHHHHHHHHHHH");
        QVERIFY2((*ink_mm(longer))[3] - (*ink_mm(longer))[1] < 6, "one row");
        // down: its first column's middle on the anchor, its top there
        const core::Document down = with_point(plain_book(), "vertical", Json::array({30, 10}), "国口");
        const auto col = ink_mm(down);
        QVERIFY2(std::abs(((*col)[0] + (*col)[2]) / 2 - 30) < 0.8 && std::abs((*col)[1] - 10) < 1.2,
                 qPrintable(QString("%1 %2").arg(((*col)[0] + (*col)[2]) / 2).arg((*col)[1])));
        // its anchor moved: the letters with it
        const core::Document moved = run(across, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"anchor", Json::array({15, 25})}});
        const auto there = ink_mm(moved);
        QVERIFY2(std::abs((*there)[0] - 15) < 0.7 && std::abs((*there)[3] - 25) < 0.7, qPrintable(QString("%1 %2").arg((*there)[0]).arg((*there)[3])));
    }

    // a box text (段落テキスト): broken at its box, what runs past it hidden (type_info's overflow)
    void boxText() {
        const core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "HHHHHHHHHHHHHHHHHHHHHHHHHHHH"}, {"wrap", "horizontal"},
                                                          {"box", Json::array({8, 8, 20, 10})}, {"style", Json{{"size_mm", 4}}}, {"new_id", "b1"}});
        const auto ink = ink_mm(doc);
        QVERIFY(ink.has_value());
        QVERIFY2((*ink)[0] >= 7.9 && (*ink)[1] >= 7.9 && (*ink)[2] <= 28.1 && (*ink)[3] <= 18.1,
                 qPrintable(QString("%1 %2 %3 %4").arg((*ink)[0]).arg((*ink)[1]).arg((*ink)[2]).arg((*ink)[3])));
        QVERIFY2((*ink)[3] - (*ink)[1] > 5, "broken into rows");
        const Json info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "b1"}}})).results[0];
        QCOMPARE(info["shape"], Json("box"));
        QCOMPARE(info["overflow"], Json(true));
        QVERIFY(info["lines"].size() >= 2);
        const core::Document fits = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "b1"}, {"text", "HH"}});
        QCOMPARE(run_ops(fits, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "b1"}}})).results[0]["overflow"], Json(false));
    }

    // アンチエイリアス なし: each pixel the letters' colour or none; the others drawn smooth
    void antialias() {
        const core::Document smooth = with_point(plain_book(), "horizontal", Json::array({10, 30}), "あO", 9);
        QVERIFY2(levels(smooth) > 2, qPrintable(QString::number(levels(smooth))));
        const core::Document none = run(smooth, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"antialias", "none"}});
        QCOMPARE(levels(none), std::size_t{2});
        QCOMPARE(run_ops(none, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "p1"}}})).results[0]["antialias"], Json("none"));
        const core::Document sharp = run(smooth, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"antialias", "sharp"}});
        QCOMPARE(proof_bytes(sharp), proof_bytes(smooth));  // (drawn smooth; PSD keeps its name — G-e)
        QVERIFY(refusal(smooth, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"antialias", "soft"}}})).find("antialias must be one of") !=
                std::string::npos);
    }

    // the book's text gamma (P3-4): a type layer mixes its letters' edges as a line of the same record does
    void textGamma() {
        core::Document plain = plain_book();
        plain.text_gamma = 1.45;
        const core::Document as_line = run(plain, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 8}, {"y_mm", 8}, {"w_mm", 30},
                                                       {"h_mm", 30}, {"balloon", "none"}, {"style", Json{{"size_mm", 8}}}, {"native_type", Json{{"shape", "box"}}}});
        const core::Document as_layer = with_type(plain);
        QVERIFY2(worst(proof_bytes(as_line), proof_bytes(as_layer)) <= 2, qPrintable(QString::number(worst(proof_bytes(as_line), proof_bytes(as_layer)))));
        QVERIFY(proof_bytes(as_layer) != proof_bytes(with_type(plain_book())));  // (the gamma shows)
    }

    // turned, scaled or slanted: the text kept, the map in its matrix (transform_layers, edit_type's transform, the page
    // turned); moved after: its matrix moves
    void turned() {
        const core::Document doc = with_point(plain_book(), "horizontal", Json::array({10, 20}), "HHHHHH", 5);
        const auto flat = ink_mm(doc);
        const core::Document quarter = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"turn", "90cw"}});
        const core::StoryLine& record = *layer_of(quarter, "p1")->type;
        QVERIFY(core::type_text::transform_of(record).has_value());
        const auto up = ink_mm(quarter);
        const double w = (*flat)[2] - (*flat)[0], h = (*flat)[3] - (*flat)[1];
        QVERIFY2(std::abs(((*up)[2] - (*up)[0]) - h) < 1.0 && std::abs(((*up)[3] - (*up)[1]) - w) < 1.0,
                 qPrintable(QString("%1×%2 against %3×%4").arg((*up)[2] - (*up)[0]).arg((*up)[3] - (*up)[1]).arg(w).arg(h)));
        // type_info's outline is where it is drawn
        const Json info = run_ops(quarter, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "p1"}}})).results[0];
        QVERIFY2(std::abs(info["outline"][0].get<double>() - (*up)[0]) < 0.8 && std::abs(info["outline"][3].get<double>() - (*up)[3]) < 0.8, info["outline"].dump().c_str());
        // moved after: by its matrix
        const core::Document moved = run(quarter, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"move", Json::array({4, 6})}});
        const auto there = ink_mm(moved);
        QVERIFY2(std::abs((*there)[0] - (*up)[0] - 4) < 0.3 && std::abs((*there)[1] - (*up)[1] - 6) < 0.3, "moved by its matrix");
        // its matrix taken away: upright where it was set
        const core::Document back = run(quarter, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"transform", nullptr}});
        QCOMPARE(proof_bytes(back), proof_bytes(doc));
        // the page turned: a type layer turns with it (its matrix); a line stays upright and refuses a matrix
        const core::Document page_turned = run(doc, Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}, {"text", "upright"}});
        QVERIFY(core::type_text::transform_of(*layer_of(page_turned, "p1")->type).has_value());
        const core::Document lined = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"x_mm", 8}, {"y_mm", 8}, {"balloon", "none"}});
        QVERIFY(refusal(lined, Json::array({Json{{"op", "edit_type"}, {"line", "l1"}, {"transform", Json::array({0, 1, -1, 0, 0, 0})}}})).find("a line stays upright") !=
                std::string::npos);
        // a scale and a slant
        const core::Document bigger = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"matrix", Json::array({1.5, 0, 0, 1.5, -5, -10})}});
        QVERIFY2((*ink_mm(bigger))[2] - (*ink_mm(bigger))[0] > 1.4 * w, "scaled");
    }

    // edit_type's replace and insert in graphemes (TEXT-01): an emoji of several code points (ZWJ) one, a CRLF one; the
    // ranges follow (an insertion before moves them, at their end leaves them, what an edit takes away goes — said)
    void editsInGraphemes() {
        const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";  // 👨‍👩‍👧
        core::Document doc = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "あ" + family + "い"}, {"box", kBox}, {"new_id", "e1"},
                                                    {"native_type", Json{{"runs", Json::array({Json{{"start", 2}, {"end", 3}, {"char", Json{{"underline", true}}}}})}}}});
        const auto info = [&](const core::Document& d) { return run_ops(d, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "e1"}}})).results[0]; };
        QCOMPARE(info(doc)["graphemes"], Json(3));
        const auto runs = [](const core::Document& d) { return (*layer_of(d, "e1")->type->native_type)["runs"]; };
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"insert", Json{{"at", 1}, {"text", "\r\n"}}}});
        QCOMPARE(layer_of(doc, "e1")->type->text, "あ\r\n" + family + "い");
        QCOMPARE(info(doc)["graphemes"], Json(4));
        QCOMPARE(info(doc)["paragraphs"].size(), std::size_t{2});
        QCOMPARE(runs(doc)[0]["start"], Json(3));
        QCOMPARE(runs(doc)[0]["end"], Json(4));
        // the emoji replaced by another: the range after it stays where it is
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"replace", Json{{"start", 2}, {"end", 3}, {"text", "\xF0\x9F\x98\x80"}}}});
        QCOMPARE(runs(doc)[0]["start"], Json(3));
        // at the range's end: not in it
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"insert", Json{{"at", 4}, {"text", "う"}}}});
        QCOMPARE(runs(doc)[0]["end"], Json(4));
        QCOMPARE(info(doc)["graphemes"], Json(5));
        // the range's letter replaced: the range goes, said
        const core::ApplyResult took = run_ops(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"replace", Json{{"start", 3}, {"end", 4}, {"text", "え"}}}}}));
        QCOMPARE(took.results[0]["type_removed"], Json(1));
        QVERIFY(!layer_of(took.doc, "e1")->type->native_type || !layer_of(took.doc, "e1")->type->native_type->contains("runs"));
        // refused: outside the text, two edits at once
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"insert", Json{{"at", 9}, {"text", "x"}}}}})).find("within the text") !=
                std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "e1"}, {"text", "x"}, {"insert", Json{{"at", 0}, {"text", "x"}}}}}))
                    .find("one of them") != std::string::npos);
        // a line's text the same way (through edit_line: its rubies followed)
        core::Document lined = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"x_mm", 8}, {"y_mm", 8}});
        lined = run(lined, Json{{"op", "edit_type"}, {"line", "l1"}, {"insert", Json{{"at", 1}, {"text", family}}}});
        QCOMPARE(lined.story[0].text, "か" + family + "き");
    }

    // ポイントテキストに変換・段落テキストに変換: the letters where they were; a text in a balloon refused
    void conversions() {
        const core::Document box = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "HIH HIH"}, {"wrap", "horizontal"},
                                                          {"box", Json::array({10, 10, 40, 20})}, {"style", Json{{"size_mm", 5}}}, {"new_id", "c1"}});
        const auto was = ink_mm(box);
        const core::Document point = run(box, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "c1"}, {"point", true}});
        QCOMPARE(render::text::shape_of(*layer_of(point, "c1")->type), std::string("point"));
        const auto now = ink_mm(point);
        QVERIFY2(std::abs((*now)[0] - (*was)[0]) < 0.6 && std::abs((*now)[1] - (*was)[1]) < 0.6,
                 qPrintable(QString("%1,%2 against %3,%4").arg((*now)[0]).arg((*now)[1]).arg((*was)[0]).arg((*was)[1])));
        const core::Document again = run(point, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "c1"}, {"box", true}});
        QCOMPARE(render::text::shape_of(*layer_of(again, "c1")->type), std::string("box"));
        const auto then = ink_mm(again);
        QVERIFY2(std::abs((*then)[0] - (*was)[0]) < 0.6 && std::abs((*then)[1] - (*was)[1]) < 0.6, "the box round its letters");
        // a box that broke a row: a line break there (Photoshop's hard return), the rows as they were
        const core::Document narrow = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "HHHHHHHHHH"}, {"wrap", "horizontal"},
                                                             {"box", Json::array({10, 10, 16, 30})}, {"style", Json{{"size_mm", 5}}}, {"new_id", "n1"}});
        const auto rows = [](const core::Document& d) {
            return run_ops(d, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "n1"}}})).results[0]["lines"].size();
        };
        QVERIFY(rows(narrow) >= 2);
        const core::Document hard = run(narrow, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "n1"}, {"point", true}});
        QVERIFY(layer_of(hard, "n1")->type->text.find('\n') != std::string::npos);
        QCOMPARE(rows(hard), rows(narrow));
        // a line fitted to its box (no shape) keeps its size as it becomes point text
        const core::Document fitted = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 8}, {"y_mm", 8},
                                                             {"w_mm", 10}, {"h_mm", 30}, {"balloon", "none"}});
        const core::Document pointed = run(fitted, Json{{"op", "edit_type"}, {"line", "l1"}, {"point", true}});
        QVERIFY(pointed.story[0].style.contains("size_mm"));
        const auto line_was = ink_mm(fitted), line_now = ink_mm(pointed);
        QVERIFY2(std::abs(((*line_now)[3] - (*line_now)[1]) - ((*line_was)[3] - (*line_was)[1])) < 0.8, "its size kept");
        // in a balloon: refused
        const core::Document spoken = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l2"}, {"text", "かき"}, {"x_mm", 8}, {"y_mm", 8}});
        QVERIFY(refusal(spoken, Json::array({Json{{"op", "edit_type"}, {"line", "l2"}, {"point", true}}})).find("a text in a balloon") != std::string::npos);
    }

    // 台詞 ⇄ テキストレイヤー: the record carried over (its speaker and panel stay the story's), each kept when asked; a
    // turned type layer stays one
    void lineAndLayer() {
        core::Document doc = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かきく"}, {"speaker", "太郎"}, {"x_mm", 8},
                                                    {"y_mm", 8}, {"balloon", "none"}, {"style", Json{{"size_mm", 6}}},
                                                    {"ruby_runs", Json::array({Json::array({"かき", "KK"})})}});
        const std::string before = proof_bytes(doc);
        const core::ApplyResult made = run_ops(doc, Json::array({Json{{"op", "line_to_type_layer"}, {"line", "l1"}, {"new_id", "tl"}}}));
        QCOMPARE(made.results[0]["layer_id"], Json("tl"));
        const core::Document layered = made.doc;
        QVERIFY(layered.story.empty());
        const core::Layer* layer = layer_of(layered, "tl");
        QVERIFY(layer != nullptr && layer->kind == core::LayerKind::Type && layer->role == core::LayerRole::Text);
        QCOMPARE(layer->type->text, std::string("かきく"));
        QVERIFY(layer->type->speaker.empty());
        QCOMPARE(layer->type->ruby_runs, doc.story[0].ruby_runs);
        QVERIFY(has_feature(layered));
        QVERIFY2(worst(proof_bytes(layered), before) <= 2, qPrintable(QString::number(worst(proof_bytes(layered), before))));  // (no panels: the same look)
        const core::ApplyResult back = run_ops(layered, Json::array({Json{{"op", "type_layer_to_line"}, {"page", 1}, {"layer_id", "tl"}, {"new_id", "l2"}}}));
        QCOMPARE(back.results[0]["id"], Json("l2"));
        QCOMPARE(back.doc.story.size(), std::size_t{1});
        QCOMPARE(back.doc.story[0].text, std::string("かきく"));
        QCOMPARE(back.doc.story[0].ruby_runs, doc.story[0].ruby_runs);
        QVERIFY(layer_of(back.doc, "tl") == nullptr);
        QCOMPARE(proof_bytes(back.doc), before);
        // kept
        const core::Document both = run(doc, Json{{"op", "line_to_type_layer"}, {"line", "l1"}, {"new_id", "tl"}, {"keep", true}});
        QCOMPARE(both.story.size(), std::size_t{1});
        QVERIFY(layer_of(both, "tl") != nullptr);
        // a turned one stays a type layer
        const core::Document turned = run(layered, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"tl"})}, {"turn", "90cw"}});
        QVERIFY(refusal(turned, Json::array({Json{{"op", "type_layer_to_line"}, {"page", 1}, {"layer_id", "tl"}}})).find("stays a type layer") != std::string::npos);
    }

    // add_type_layer's own: size_pt, parent, above, antialias, a title by its text (following it until it is given one),
    // the marks a line takes (rubies), a point text in a balloon refused
    void addOptions() {
        core::Document doc = run(plain_book(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"name", "F"}});
        const std::string folder = doc.page(0).layers.back().id;
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "こんにちは"}, {"point", Json::array({10, 10})}, {"size_pt", 12}, {"parent", folder},
                            {"above", "paint"}, {"antialias", "none"}, {"new_id", "a1"}, {"ruby_runs", Json::array({Json::array({"こんにちは", "hello"})})}});
        const core::Layer* layer = layer_of(doc, "a1");
        QCOMPARE(layer->title, std::string("こんにちは"));
        QCOMPARE(layer->parent_id, Json(folder));
        QCOMPARE(doc.page(0).layers[1].id, std::string("a1"));  // (just above paint)
        QVERIFY(std::abs(layer->type->style["size_mm"].get<double>() - 4.2333) < 1e-4);
        QCOMPARE((*layer->type->native_type)["antialias"], Json("none"));
        QCOMPARE(layer->type->ruby_runs.size(), std::size_t{1});
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "a1"}, {"text", "さようなら"}});
        QCOMPARE(layer_of(doc, "a1")->title, std::string("さようなら"));
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "a1"}, {"title", "題"}});
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "a1"}, {"text", "またね"}});
        QCOMPARE(layer_of(doc, "a1")->title, std::string("題"));
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}, {"balloon", "speech"}}}))
                    .find("a text in a balloon") != std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}, {"size_pt", 9},
                                               {"style", Json{{"size_mm", 3}}}}}))
                    .find("not both") != std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({1, 1})}, {"parent", "paint"}}}))
                    .find("parent must be a folder") != std::string::npos);
        // a box in a balloon: a line's setting in it (no shape)
        const core::Document spoken = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"box", kBox}, {"balloon", "speech"}, {"new_id", "s1"}});
        QVERIFY(!layer_of(spoken, "s1")->type->native_type);
    }

    // type_info as set: its rows on the page (across: each row's baseline from its left to its right) and its size
    void infoAsSet() {
        const core::Document doc = with_point(plain_book(), "horizontal", Json::array({10, 20}), "AB\nCD", 5);
        const Json info = run_ops(doc, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "p1"}}})).results[0];
        QCOMPARE(info["shape"], Json("point"));
        QCOMPARE(info["anchor"], Json::array({10, 20}));
        QCOMPARE(info["lines"].size(), std::size_t{2});
        QCOMPARE(info["lines"][0]["text"], Json("AB"));
        QVERIFY2(std::abs(info["lines"][0]["from"][1].get<double>() - 20) < 0.3 && std::abs(info["lines"][0]["from"][0].get<double>() - 10) < 0.3,
                 info["lines"].dump().c_str());
        QVERIFY(info["lines"][1]["from"][1].get<double>() > 24);
        QVERIFY(std::abs(info["size_mm"].get<double>() - 5) < 0.1);
    }

    // --- PC5 G-a review ------------------------------------------------------------------------------------------------------

    // (P1-1) a turned type layer draws only what a part of the page takes: a 512 px tile at 600 dpi a small upright piece,
    // the whole page in bands within the bound — and the part the same pixels as the whole cut there
    void turnedInParts() {
        core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(210, 297, 40, 40, 1, 2, 2, 2, 2, 600, "color"));
        doc.edit_page(0).frames.clear();
        doc.edit_page(0).numero = false;
        std::string text;
        for (int i = 0; i < 60; ++i) text += "あいうえおかきくけこ";  // (about the box's fill: 34 a column, 23 columns)
        doc = run(doc, Json{{"op", "add_type_layer"}, {"page", 1}, {"text", text}, {"box", Json::array({10, 10, 190, 277})}, {"style", Json{{"size_mm", 8}}},
                            {"new_id", "big"}});
        doc = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"big"})}, {"turn", "90cw"}});
        render::RenderOptions options = render::proof_options();
        options.region = render::RenderRegion{2000, 3000, 512, 512};
        (void)render::type_layer::largest_upright_pixels(true);
        const render::Image tile = render::render_page(doc.page(0), 600, options, &doc).image;
        const std::int64_t piece = render::type_layer::largest_upright_pixels(true);
        QVERIFY2(piece > 0 && piece < 600'000, qPrintable(QString::number(piece)));  // (about the tile's own, not the 4500 × 6500 px text)
        options.region.reset();
        const render::Image whole = render::render_page(doc.page(0), 600, options, &doc).image;
        const std::int64_t most = render::type_layer::largest_upright_pixels(true);
        QVERIFY2(most <= 20'000'000, qPrintable(QString::number(most)));
        QVERIFY2(worst(tile.tobytes(), whole.crop(render::Box{2000, 3000, 2512, 3512}).tobytes()) <= 2, "the tile as the whole cut there");
    }

    // (P2-3) a balloon of a type layer set where the turned page no longer reaches: drawn whole
    void balloonOffThePaper() {
        core::Document doc = core::new_episode("文字", core::Num(1), 1, core::PageSpec::custom(100, 150, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        doc.edit_page(0).frames.clear();
        doc.edit_page(0).numero = false;
        doc = run(doc, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "あいう"}, {"x_mm", 10}, {"y_mm", 115}, {"w_mm", 40}, {"h_mm", 25}});
        doc = run(doc, Json{{"op", "line_to_type_layer"}, {"line", "l1"}, {"new_id", "t1"}});
        const double before = darkness(shown(doc));
        const core::Document turned = run(doc, Json{{"op", "rotate_pages"}, {"page", 1}, {"turn", "90cw"}, {"text", "upright"}});
        QVERIFY(core::type_text::transform_of(*layer_of(turned, "t1")->type).has_value());
        const double after = darkness(shown(turned));
        QVERIFY2(std::abs(after - before) < 0.08 * before, qPrintable(QString("%1 against %2").arg(after).arg(before)));
    }

    // (P2-4) places and sizes within bounds; a text too large to set refused
    void bounds() {
        const core::Document doc = plain_book();
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"box", Json::array({0, 0, 5000, 10})}}}))
                    .find("within a metre") != std::string::npos);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "a"}, {"point", Json::array({-3000, 0})}}}))
                    .find("within a metre") != std::string::npos);
        std::string longer;
        for (int i = 0; i < 20000; ++i) longer += "H";
        const std::string why = refusal(doc, Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", longer}, {"point", Json::array({1, 10})}, {"wrap", "horizontal"}}}));
        QVERIFY2(why.find("too large to set") != std::string::npos, why.c_str());
    }

    // (P2-5) a point or box text with no alignment is Photoshop's left: every row from the anchor's (the box's) left
    void leftByDefault() {
        const core::Document point = with_point(plain_book(), "horizontal", Json::array({8, 15}), "H\nHIHIH", 4);
        const Json info = run_ops(point, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "p1"}}})).results[0];
        QVERIFY2(std::abs(info["lines"][0]["from"][0].get<double>() - 8) < 0.4 && std::abs(info["lines"][1]["from"][0].get<double>() - 8) < 0.4,
                 info["lines"].dump().c_str());
        const core::Document box = run(plain_book(), Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "HHHHHHHHHHH"}, {"wrap", "horizontal"},
                                                          {"box", Json::array({5, 5, 20, 30})}, {"style", Json{{"size_mm", 4}}}, {"new_id", "b1"}});
        const Json rows = run_ops(box, Json::array({Json{{"op", "type_info"}, {"page", 1}, {"layer_id", "b1"}}})).results[0]["lines"];
        QVERIFY(rows.size() >= 2);
        for (const Json& row : rows) QVERIFY2(std::abs(row["from"][0].get<double>() - 5) < 0.4, rows.dump().c_str());
    }

    // (P2-2) a line made point text: moved (move_line), its letters with it; turned (rotate_deg), still drawn
    void pointLines() {
        core::Document doc = run(plain_book(), Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "HIH"}, {"x_mm", 8}, {"y_mm", 8}, {"w_mm", 20},
                                                    {"h_mm", 10}, {"balloon", "none"}, {"wrap", "horizontal"}, {"style", Json{{"size_mm", 5}}}});
        doc = run(doc, Json{{"op", "edit_type"}, {"line", "l1"}, {"point", true}});
        const auto was = ink_mm(doc);
        const core::StoryLine& line = doc.story[0];
        const core::Document moved = run(doc, Json{{"op", "move_line"}, {"id", "l1"}, {"x_mm", line.x_mm.value() + 6}, {"y_mm", line.y_mm.value() + 4}});
        const auto now = ink_mm(moved);
        QVERIFY2(std::abs((*now)[0] - (*was)[0] - 6) < 0.3 && std::abs((*now)[1] - (*was)[1] - 4) < 0.3, "moved with its box");
        const core::Document leant = run(doc, Json{{"op", "edit_line"}, {"id", "l1"}, {"style", Json{{"rotate_deg", 30}}}});
        const auto turned = ink_mm(leant);
        QVERIFY(turned.has_value());
        QVERIFY2(std::abs(((*turned)[0] + (*turned)[2]) / 2 - ((*was)[0] + (*was)[2]) / 2) < 2, "drawn where it was, turned");
    }

    // (P2-6) a turned type layer's work path where it is shown; ワープテキスト on a type layer (edit_type's warp)
    void pathsAndWarp() {
        const core::Document doc = with_point(plain_book(), "horizontal", Json::array({8, 20}), "HIH", 6);
        const core::Document turned = run(doc, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"turn", "90cw"}});
        const auto ink = ink_mm(turned);
        const core::Document path = run(turned, Json{{"op", "text_to_path"}, {"page", 1}, {"id", "p1"}});
        const auto bounds = path.page(0).work_path->control_bounds();
        QVERIFY(bounds.has_value());
        QVERIFY2(std::abs((*bounds)[0] - (*ink)[0]) < 0.8 && std::abs((*bounds)[1] - (*ink)[1]) < 0.8 && std::abs((*bounds)[2] - (*ink)[2]) < 0.8,
                 qPrintable(QString("%1 %2 %3 against %4 %5 %6").arg((*bounds)[0]).arg((*bounds)[1]).arg((*bounds)[2]).arg((*ink)[0]).arg((*ink)[1]).arg((*ink)[2])));
        const Json arc{{"style", "arc"}, {"bend", 50}, {"h_distort", 0}, {"v_distort", 0}, {"vertical", false}};
        const core::Document bent = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"warp", arc}});
        QVERIFY(layer_of(bent, "p1")->type->style.contains("native_warp"));
        QVERIFY(proof_bytes(bent) != proof_bytes(doc));
        const core::Document straight = run(bent, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"warp", nullptr}});
        QCOMPARE(proof_bytes(straight), proof_bytes(doc));
    }

    // (P3-1, P3-2, P3-4) the position's lock; what a line leaves behind said, and given back; アンチエイリアス なし turned
    void reviewSmall() {
        core::Document doc = with_point(plain_book(), "horizontal", Json::array({8, 20}), "HIH", 6);
        const core::Document held = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "p1"}, {"lock_position", true}});
        QVERIFY(refusal(held, Json::array({Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"anchor", Json::array({9, 9})}}})).find("is locked") !=
                std::string::npos);
        (void)run(held, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"text", "HI"}});  // (its text is not its position)
        // a line's speaker, panel and place in the reading order: in the report and a note; type_layer_to_line gives them back
        core::Document lined = plain_book();
        lined.edit_page(0).frames.clear();
        lined = run(lined, Json{{"op", "add_line"}, {"page", 1}, {"id", "l0"}, {"text", "まえ"}, {"x_mm", 30}, {"y_mm", 30}});
        lined = run(lined, Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"speaker", "太郎"}, {"x_mm", 8}, {"y_mm", 8}});
        lined = run(lined, Json{{"op", "add_line"}, {"page", 1}, {"id", "l2"}, {"text", "あと"}, {"x_mm", 30}, {"y_mm", 8}});
        const core::ApplyResult made = run_ops(lined, Json::array({Json{{"op", "line_to_type_layer"}, {"line", "l1"}, {"new_id", "tl"}}}));
        QCOMPARE(made.results[0]["dropped"]["speaker"], Json("太郎"));
        QCOMPARE(made.results[0]["dropped"]["after"], Json("l0"));
        const core::Document back = run(made.doc, Json{{"op", "type_layer_to_line"}, {"page", 1}, {"layer_id", "tl"}, {"new_id", "l1"}, {"speaker", "太郎"},
                                                       {"after", "l0"}});
        QCOMPARE(back.story.size(), std::size_t{3});
        QCOMPARE(back.story[1].id, std::string("l1"));
        QCOMPARE(back.story[1].speaker, std::string("太郎"));
        // アンチエイリアス なし, turned: still each pixel the letters' or none
        doc = run(doc, Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "p1"}, {"antialias", "none"}, {"transform", Json::array({0.866, 0.5, -0.5, 0.866, 15, -5})}});
        const std::string bytes = render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.convert("L").tobytes();
        int middle = 0;
        for (const char c : bytes) middle += static_cast<unsigned char>(c) > 40 && static_cast<unsigned char>(c) < 215 ? 1 : 0;
        QVERIFY2(middle < 5, qPrintable(QString::number(middle)));
    }

    // (P3-15) a turned type layer through the other ways: a layered PSD, flattened, a 16-bit and a CMYK page, a smart object
    void turnedElsewhere() {
        const core::Document doc = run(with_type(plain_book()), Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"turn", "90cw"}});
        const double dark = darkness(shown(doc));
        QVERIFY(dark > 0.003);
        QTemporaryDir scratch;
        const fs::path psd = core::path_from_utf8(scratch.path().toStdString()) / "turned.psd";
        formats::export_page_psd(doc, doc.page(0), psd, 200);
        const render::psd::File file = render::psd::File::read(read_text(psd));
        bool found = false;
        for (const render::psd::Layer& layer : file.layers()) found = found || (layer.name == "あいう" && file.image(layer).getbbox().has_value());
        QVERIFY(found);
        const core::Document flat = run(doc, Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}, {"flatten", true}});
        QVERIFY2(std::abs(darkness(shown(flat)) - dark) < 0.1 * dark, "flattened");
        const core::Document smart = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}});
        QVERIFY2(worst(proof_bytes(smart), proof_bytes(doc)) <= 2, "a smart object");
        for (const Json& mode : {Json{{"mode", "rgb"}, {"depth", 16}}, Json{{"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}}}) {
            Json op = mode;
            op["op"] = "set_color_mode";
            op["page"] = 1;
            const core::Document deep =
                run(with_type(run(plain_book(), op)), Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"turn", "90cw"}});
            const double d = darkness(shown(deep));
            QVERIFY2(std::abs(d - dark) < 0.15 * dark, qPrintable(QString::fromStdString(mode.dump()) + QStringLiteral(": %1 against %2").arg(d).arg(dark)));
        }
    }
};

QTEST_GUILESS_MAIN(TestContractType)
#include "test_contract_type.moc"
