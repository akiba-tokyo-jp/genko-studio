// color-mode@1 (SPEC.md COLOR-02, schema-v4.md「色モード」): CMYK and Lab pages without Python — the GKIR pixels and
// what is refused, the colour mode's JSON, the rules that give an RGB colour its ink, the blends of the ink canvas, a
// page drawn in parts and whole, set_color_mode and back, set_inks, what the book keeps on disk (and what makes it
// read-only), Undo and Redo, the blends a Lab page refuses and the ops that do not know inks yet. (The conversions
// against Pillow's ImageCms and the exports read back by Pillow: test_contract_ink.)

#include <QtTest>

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

std::string icc_bytes() { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))); }

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document small_book(int pages = 1) {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("ink", core::Num(1), pages, spec, core::Binding::Right);
}

std::string ink_layer_id(const core::Document& doc, std::size_t page = 0) {
    for (const auto& layer : doc.page(page).layers) {
        if (layer.role == core::LayerRole::Ink) return layer.id;
    }
    return {};
}

Json to_cmyk_op(int depth = 8, const std::string& gray = "profile") {
    return Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(icc_bytes())}, {"depth", depth}, {"gray", gray}};
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

void save_next(const core::Document& doc, const fs::path& dir, const Json& ops) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = storage::read_disk_state(dir).revision;
    request.ops = ops;
    storage::Saver(lock).save(doc, request);
}

core::InkPixels page_inks(const core::Document& doc, std::size_t page, int dpi, std::optional<render::RenderRegion> region = {}) {
    render::RenderOptions options;
    options.inks = true;
    options.region = region;
    auto made = render::render_page(doc.page(page), dpi, options, &doc);
    if (!made.inks) throw core::Error("value", "no inks");
    return std::move(*made.inks);
}

const std::uint16_t* at(const core::InkPixels& p, int x, int y) { return p.samples.data() + (std::size_t(y) * p.width + x) * p.channels(); }

}  // namespace

class TestInk : public QObject {
    Q_OBJECT

private slots:
    // GKIR: the same pixels back, the same bytes for the same pixels, and bytes that are not a whole raster refused.
    void rasterCodec() {
        std::mt19937 random(7);
        for (const auto space : {core::InkSpace::Cmyk, core::InkSpace::Lab}) {
            for (const int depth : {8, 16}) {
                core::InkPixels p = core::blank_ink_pixels(space, depth, 37, 23);
                for (auto& s : p.samples) s = static_cast<std::uint16_t>(random() % (p.max() + 1));
                const std::string bytes = core::encode_ink_raster(p);
                QCOMPARE(core::encode_ink_raster(p), bytes);
                const core::InkPixels back = core::decode_ink_raster(bytes);
                QCOMPARE(back.width, 37u);
                QCOMPARE(back.height, 23u);
                QCOMPARE(back.depth, depth);
                QVERIFY(back.space == space);
                QVERIFY(back.samples == p.samples);
                const core::InkRasterHeader header = core::ink_raster_header(bytes);
                QCOMPARE(header.width, 37u);
                // refused: bytes after the pixels, a stream cut short, a header saying more pixels, an unknown filter
                QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(bytes + "x"));
                QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(bytes.substr(0, bytes.size() - 8)));
                std::string taller = bytes;
                taller[12] = static_cast<char>(24);
                QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(taller));
                std::string version = bytes;
                version[4] = 2;
                QVERIFY_THROWS_EXCEPTION(core::Error, core::ink_raster_header(version));
            }
        }
        // a header past the limits is refused before anything is inflated
        std::string huge = core::encode_ink_raster(core::blank_ink_pixels(core::InkSpace::Cmyk, 8, 1, 1));
        huge[8] = huge[9] = huge[10] = static_cast<char>(0xff);
        QVERIFY_THROWS_EXCEPTION(core::Error, core::ink_raster_header(huge));
        // a Lab blank is white, transparent
        const auto lab = core::blank_ink_pixels(core::InkSpace::Lab, 8, 2, 1);
        QCOMPARE(lab.samples[0], std::uint16_t(255));
        QCOMPARE(lab.samples[1], std::uint16_t(128));
        QCOMPARE(lab.samples[3], std::uint16_t(0));
    }

    void colourModeJson() {
        core::PageColor c;
        c.space = core::InkSpace::Cmyk;
        c.depth = 16;
        c.profile = core::bytes_ref(icc_bytes());
        c.intent = "perceptual";
        c.inks.push_back(core::InkEntry{{200, 30, 30}, {0.1, 0.9, 0.8, 0.0}});
        const Json j = core::to_json(c);
        QCOMPARE(QString::fromStdString(j.dump()),
                 QString::fromStdString(R"({"mode":"cmyk","depth":16,"profile":")" + c.profile +
                                        R"(","intent":"perceptual","inks":[[[200,30,30],[0.1,0.9,0.8,0.0]]]})"));
        QVERIFY(core::page_color_from_json(j) == c);
        const auto refused = [](Json value) {
            try {
                (void)core::page_color_from_json(value);
            } catch (const core::Error&) {
                return true;
            }
            return false;
        };
        Json bad = j;
        bad["extra"] = 1;
        QVERIFY(refused(bad));
        bad = j;
        bad["depth"] = 12;
        QVERIFY(refused(bad));
        bad = j;
        bad["depth"] = true;
        QVERIFY(refused(bad));
        bad = j;
        bad.erase("profile");
        QVERIFY(refused(bad));
        bad = j;
        bad["mode"] = "lab";
        QVERIFY(refused(bad));  // (a Lab page has no profile)
        bad = j;
        bad["inks"][0][1][0] = 1.5;
        QVERIFY(refused(bad));
        bad = j;
        bad["inks"].push_back(bad["inks"][0]);
        QVERIFY(refused(bad));  // (an RGB colour twice)
        bad = j;
        bad["intent"] = "nice";
        QVERIFY(refused(bad));
        Json lab{{"mode", "lab"}, {"depth", 8}, {"intent", "relative"}, {"inks", Json::array({Json::array({Json::array({1, 2, 3}), Json::array({50, -20, 127})})})}};
        QVERIFY(!refused(lab));
        lab["inks"][0][1][1] = -129;
        QVERIFY(refused(lab));
    }

    void profileChecks() {
        QVERIFY(!render::ink::profile_problem(icc_bytes()));
        QVERIFY(render::ink::profile_problem("not a profile"));
        const auto rgb = render::ink::profile_problem(render::colour::srgb_icc());
        QVERIFY(rgb);
        QVERIFY(rgb->find("CMYK") != std::string::npos);
    }

    // schema-v4「色モード」: the table, Genko's blacks and white, greys on K alone (Lab: no colour), the rest through the
    // profile.
    void inkRules() {
        core::PageColor c;
        c.space = core::InkSpace::Cmyk;
        c.icc = std::make_shared<const std::string>(icc_bytes());
        c.profile = core::bytes_ref(*c.icc);
        c.inks.push_back(core::InkEntry{{200, 30, 30}, {0.1, 0.9, 0.8, 0.05}});
        const auto close = [](const std::vector<double>& a, const std::vector<double>& b) {
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::abs(a[i] - b[i]) > 1e-6) return false;
            }
            return a.size() == b.size();
        };
        for (const int v : {0, 10, 15, 20}) QVERIFY(close(render::ink::ink_of(c, {v, v, v}), {0, 0, 0, 1}));
        QVERIFY(close(render::ink::ink_of(c, {255, 255, 255}), {0, 0, 0, 0}));
        QVERIFY(close(render::ink::ink_of(c, {128, 128, 128}), {0, 0, 0, 1 - 128 / 255.0}));
        QVERIFY(close(render::ink::ink_of(c, {200, 30, 30}), {0.1, 0.9, 0.8, 0.05}));
        const auto red = render::ink::ink_of(c, {220, 20, 20});  // (through the profile: magenta and yellow)
        QVERIFY(red[1] > 0.7 && red[2] > 0.7 && red[0] < 0.2);
        core::PageColor lab;
        lab.space = core::InkSpace::Lab;
        const auto black = render::ink::ink_of(lab, {20, 20, 20});
        QVERIFY(close(black, {0, 0, 0}));
        const auto grey = render::ink::ink_of(lab, {128, 128, 128});
        QCOMPARE(grey[1], 0.0);
        QCOMPARE(grey[2], 0.0);
        QVERIFY(std::abs(grey[0] - 53.6) < 1.0);  // (sRGB 128 is L 53.6)
        QVERIFY(close(render::ink::ink_of(lab, {255, 255, 255}), {100, 0, 0}));
    }

    // The ink canvas's blends: CMYK on the inverted plates (multiply adds ink), Lab only in the modes it has.
    void canvasBlends() {
        core::PageColor c;
        c.space = core::InkSpace::Cmyk;
        c.icc = std::make_shared<const std::string>(icc_bytes());
        c.profile = core::bytes_ref(*c.icc);
        const render::Image white = render::Image::create("RGB", render::Size{2, 1}, render::Ink{255, 255, 255});
        render::InkPicture half{render::Size{2, 1}, 4, {0, 0, 0, 0.5, 1, 0, 0, 0, 0.5, 1}};
        {
            render::InkCanvas canvas(c, white);
            canvas.blend(half, 1, false, "normal");
            canvas.blend(half, 1, false, "multiply");
            const auto inks = canvas.ink_pixels(16);
            QCOMPARE(at(inks, 0, 0)[3], std::uint16_t(std::lround(0.75 * 65535)));
        }
        {
            render::InkCanvas canvas(c, white);
            canvas.blend(half, 1, false, "normal");
            canvas.blend(half, 1, false, "screen");
            QCOMPARE(at(canvas.ink_pixels(16), 0, 0)[3], std::uint16_t(std::lround(0.25 * 65535)));
        }
        {  // clipped to the layer before: nothing where it was transparent
            render::InkCanvas canvas(c, white);
            render::InkPicture left{render::Size{2, 1}, 4, {0, 0, 0, 1, 1, 0, 0, 0, 0, 0}};
            render::InkPicture cyan{render::Size{2, 1}, 4, {1, 0, 0, 0, 1, 1, 0, 0, 0, 1}};
            canvas.blend(left, 1, false, "normal");
            canvas.blend(cyan, 1, true, "normal");
            const auto inks = canvas.ink_pixels(8);
            QCOMPARE(at(inks, 0, 0)[0], std::uint16_t(255));
            QCOMPARE(at(inks, 1, 0)[0], std::uint16_t(0));
        }
        core::PageColor lab;
        lab.space = core::InkSpace::Lab;
        render::InkCanvas canvas(lab, white);
        QVERIFY(canvas.supports_blend("multiply"));
        QVERIFY(!canvas.supports_blend("darken"));
        QVERIFY(!canvas.supports_blend("hue"));
        const auto inks = canvas.ink_pixels(8);
        QCOMPARE(at(inks, 0, 0)[0], std::uint16_t(255));  // (white paper: L 100)
        QCOMPARE(at(inks, 0, 0)[1], std::uint16_t(128));
    }

    // A CMYK page drawn: a black line on K alone at 100%, a colour through the table exactly, the paper without ink; a
    // part drawn alone is the same as the whole cut there (inks and the screen's picture).
    void cmykPageDrawn() {
        core::Document doc = small_book();
        const std::string ink = ink_layer_id(doc);
        doc = run_ops(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({10, 10}), Json::array({50, 70})})}, {"width_mm", 3}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::array({Json::array({10, 60}), Json::array({50, 60})})}, {"width_mm", 3}, {"rgb", Json::array({200, 30, 30})}},
                                        to_cmyk_op(),
                                        Json{{"op", "set_inks"}, {"inks", Json::array({Json::array({Json::array({200, 30, 30}), Json::array({0.1, 0.9, 0.8, 0.0})})})}}}))
                  .doc;
        QVERIFY(doc.page(0).color);
        QCOMPARE(doc.page(0).color->inks.size(), std::size_t(1));
        QVERIFY(doc.color);  // (every page: the book's mode too)
        const int dpi = 150;
        const core::InkPixels inks = page_inks(doc, 0, dpi);
        const auto px = [&](double x_mm, double y_mm) { return at(inks, render::mm_to_px(x_mm, dpi), render::mm_to_px(y_mm, dpi)); };
        const std::uint16_t* line = px(30, 40);  // (on the black diagonal)
        QCOMPARE(line[0], std::uint16_t(0));
        QCOMPARE(line[1], std::uint16_t(0));
        QCOMPARE(line[2], std::uint16_t(0));
        QCOMPARE(line[3], std::uint16_t(255));
        const std::uint16_t* red = px(20, 60);
        QCOMPARE(red[0], std::uint16_t(std::lround(0.1 * 255)));
        QCOMPARE(red[1], std::uint16_t(std::lround(0.9 * 255)));
        QCOMPARE(red[2], std::uint16_t(std::lround(0.8 * 255)));
        QCOMPARE(red[3], std::uint16_t(0));
        const std::uint16_t* paper = px(55, 5);
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(paper[c], std::uint16_t(0));
        // a part alone is the whole cut there
        const render::RenderRegion part{40, 50, 140, 160};
        const core::InkPixels piece = page_inks(doc, 0, dpi, part);
        const core::InkPixels cut = render::crop_inks(inks, render::Box{part.x, part.y, part.x + part.w, part.y + part.h});
        QVERIFY(piece.samples == cut.samples);
        render::RenderOptions options;
        const render::Image whole = render::render_page(doc.page(0), dpi, options, &doc).image;
        options.region = part;
        const render::Image shown = render::render_page(doc.page(0), dpi, options, &doc).image;
        QCOMPARE(shown.tobytes(), whole.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes());
    }

    // set_color_mode: a paint layer's pixels into the page's space (gray "k": exact greys on K alone) and back; 16 bits
    // back to RGB keep 16 (precise u16); the lines and everything else as they were.
    void convertAndBack() {
        core::Document doc = small_book();
        render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{128, 128, 128, 255}, render::Box{0, 0, 30, 40});
        picture.paste(render::Ink{30, 160, 90, 200}, render::Box{30, 0, 60, 40});
        const std::string png = render::write_png(picture);
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"title", "絵"}}})).doc;
        const std::string paint = doc.page(0).layers.back().id;
        doc = run_ops(doc, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", paint}, {"png_base64", core::b64encode(png)}}})).doc;
        const core::Document rgb = doc;
        doc = run_ops(doc, Json::array({to_cmyk_op(8, "k")})).doc;
        const core::Layer& layer = doc.page(0).layers.back();
        QVERIFY(layer.ink_raster);
        QVERIFY(!layer.raster_png);
        const core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
        QCOMPARE(pixels.width, 60u);
        const std::uint16_t* grey = at(pixels, 5, 5);
        QCOMPARE(grey[0], std::uint16_t(0));
        QCOMPARE(grey[3], std::uint16_t(std::lround((1 - 128 / 255.0) * 255)));
        QCOMPARE(grey[4], std::uint16_t(255));
        QCOMPARE(at(pixels, 40, 5)[4], std::uint16_t(200));  // (its alpha kept)
        // back to RGB: close to the colours it had (through the profile and back)
        const core::Document back = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "rgb"}}})).doc;
        QVERIFY(!back.page(0).color);
        QVERIFY(!back.color);
        const core::Layer& again = back.page(0).layers.back();
        QVERIFY(again.raster_png && !again.ink_raster);
        const std::string seen = render::open_image(*again.raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        const std::string began = picture.tobytes();
        const auto pixel = [](const std::string& rgba, int x, int y) { return reinterpret_cast<const unsigned char*>(rgba.data()) + (y * 60 + x) * 4; };
        for (unsigned ch = 0; ch < 3; ++ch) QVERIFY2(std::abs(int(pixel(seen, 40, 5)[ch]) - int(pixel(began, 40, 5)[ch])) <= 12, "a colour far from where it began");
        QCOMPARE(int(pixel(seen, 40, 5)[3]), 200);
        // 16 bits back to RGB, asked for 16: precise pixels (8, the default: a PNG)
        const core::Document deep = run_ops(rgb, Json::array({to_cmyk_op(16)})).doc;
        QCOMPARE(core::ink_raster_header(*deep.page(0).layers.back().ink_raster).depth, 16);
        core::Document precise;
        try {
            precise = run_ops(deep, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", deep.page(0).layers.back().id}, {"panel_each", false}, {"panel_clip", false}},
                                                 Json{{"op", "set_color_mode"}, {"mode", "rgb"}, {"depth", 16}}})).doc;
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
        QVERIFY(precise.page(0).layers.back().color_raster);
    }

    void refusals() {
        core::Document doc = small_book();
        // a Lab page cannot composite darken: the op is refused, and so is a page holding such a layer becoming Lab
        const std::string ink = ink_layer_id(doc);
        const core::Document lab = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}}})).doc;
        QVERIFY(lab.page(0).color && lab.page(0).color->space == core::InkSpace::Lab);
        QVERIFY(!refusal(lab, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", ink}, {"blend", "darken"}}})).empty());
        const core::Document dark = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", ink}, {"blend", "darken"}}})).doc;
        QVERIFY(refusal(dark, Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}}})).find("Lab") != std::string::npos);
        // CMYK needs its profile; a Lab page has none; a profile that is not CMYK is refused
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "lab"}, {"icc", core::b64encode(icc_bytes())}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode("nope")}}})).empty());
        // an op that does not know inks yet: not_yet_ported, nothing done
        const core::Document cmyk = run_ops(doc, Json::array({to_cmyk_op()})).doc;
        const std::string erased = refusal(cmyk, Json::array({Json{{"op", "erase_raster"}, {"page", 1}, {"layer", "ink"}, {"width_mm", 1}, {"points", Json::array({Json::array({1, 1}), Json::array({5, 5})})}}}));
        QVERIFY2(erased.rfind("not_yet_ported", 0) == 0, erased.c_str());
        // set_inks on an RGB page
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_inks"}, {"page", 1}, {"inks", Json::array()}}})).empty());
    }

    // The book on disk: the mode, the profile (one asset), the ink pixels, the feature; read back the same; what makes it
    // read-only (a profile that is not one, a broken mode); Undo and Redo of the conversion.
    void onDisk() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8(tmp.filePath(QStringLiteral("book.genko")).toStdString());
        core::Document doc = small_book(2);
        render::Image picture = render::Image::create("RGBA", render::Size{20, 20}, render::Ink{30, 160, 90, 255});
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}}})).doc;
        const std::string paint = doc.page(0).layers.back().id;
        doc = run_ops(doc, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", paint}, {"png_base64", core::b64encode(render::write_png(picture))}}})).doc;
        write_book(doc, dir);
        const core::Document cmyk = run_ops(doc, Json::array({to_cmyk_op(16)})).doc;
        save_next(cmyk, dir, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}}}));
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(std::find(loaded.document.features.begin(), loaded.document.features.end(), core::kColorModeFeature) != loaded.document.features.end());
        QVERIFY(loaded.document.page(0).color == cmyk.page(0).color);
        QCOMPARE(*loaded.document.page(0).color->icc, icc_bytes());
        QCOMPARE(*loaded.document.page(0).layers.back().ink_raster, *cmyk.page(0).layers.back().ink_raster);
        QVERIFY(loaded.document.color == cmyk.color);
        const fs::path icc_file = dir / storage::AssetStore::relpath(cmyk.page(0).color->profile, core::kProfileSuffix);
        QVERIFY(fs::exists(icc_file));
        // Undo: the RGB book again; Redo: CMYK again
        storage::restore(dir, "human:test", false, false);
        const auto undone = storage::load_document(dir);
        QVERIFY(!undone.document.page(0).color);
        QVERIFY(undone.document.page(0).layers.back().raster_png);
        storage::restore(dir, "human:test", true, false);
        QVERIFY(storage::load_document(dir).document.page(0).color);
        // a profile that is not the bytes its name says: read-only, the mode kept (never drawn with another)
        {
            QFile file(QString::fromStdString(core::path_to_utf8(icc_file)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("broken");
        }
        const auto broken = storage::load_document(dir);
        QVERIFY(!broken.document.read_only_reason.empty());
        QVERIFY(broken.document.page(0).color && !broken.document.page(0).color->icc);
        // a mode that is not as the writer writes it: read-only
        {
            Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json"))));
            payload["pages"][1]["color"]["depth"] = 12;
            QFile file(QString::fromStdString(core::path_to_utf8(dir / "project.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(payload.dump()));
        }
        QVERIFY(!storage::load_document(dir).document.read_only_reason.empty());
        QVERIFY(storage::is_known_feature(core::kColorModeFeature));
    }
};

QTEST_GUILESS_MAIN(TestInk)
#include "test_ink.moc"
