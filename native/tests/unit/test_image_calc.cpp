// PC2c (画像の演算・トラッピング・フェード, SPEC.md COLOR-04) without Python: Apply Image and Calculations' Blending
// (add and subtract with their scale and offset, the separable blends, what is refused), apply_image on an 8-bit layer
// (a layer's composite, the opacity, the transparency kept or painted, a mask from a channel, the area, a plate, the
// transparency, inverted; a page of another size refused), on an alpha channel and on a CMYK layer's inks (multiplied as
// ink amounts); calculations into a new alpha channel, a new grey page and a selection (the book as it was); trapping
// (PhotoCraft's algo/trap.rs:67-125: both inks at the seam, the far side and the paper as they were; the op on a CMYK
// page only); and the fade (half way at 50 % normal, multiply; the op reading the pixels before from the book's assets,
// none of them at 0 %, what it refuses).

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "render/channels.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::image_ops::Rgba;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
constexpr std::uint32_t kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::ApplyResult run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})); }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(double width = kInch) {
    auto doc = core::new_episode("演算", core::Num(1), 2, core::PageSpec::custom(width, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        auto& p = doc.edit_page(i);
        p.numero = false;
        p.frames.clear();
        p.layers.clear();
    }
    return doc;
}

using Colour = std::function<std::array<int, 4>(std::uint32_t, std::uint32_t)>;

render::Image picture(const Colour& at) {
    std::string bytes;
    for (std::uint32_t y = 0; y < kSide; ++y)
        for (std::uint32_t x = 0; x < kSide; ++x)
            for (const int v : at(x, y)) bytes.push_back(static_cast<char>(v));
    return render::Image::frombytes("RGBA", render::Size{int(kSide), int(kSide)}, bytes);
}

core::Layer paint_layer(const core::Page& page, const std::string& id, const render::Image& image) {
    core::Layer layer;
    layer.id = id;
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(page, layer, image);
    return layer;
}

// Page 1: "paint" (a ramp of colours, clear in its top quarter) under "src" (a flat colour, hidden).
core::Document two_layers() {
    auto doc = blank();
    doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "paint", picture([](std::uint32_t x, std::uint32_t y) {
        return y < 50 ? std::array<int, 4>{0, 0, 0, 0} : std::array<int, 4>{static_cast<int>(x), 128, 255 - static_cast<int>(x), 255};
    })));
    doc.edit_page(0).layers.push_back(paint_layer(doc.page(0), "src", picture([](std::uint32_t, std::uint32_t) { return std::array<int, 4>{128, 64, 255, 255}; })));
    doc.edit_page(0).layers.back().visible = false;
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id, std::size_t page = 0) {
    for (const auto& layer : doc.page(page).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", "no layer " + id);
}

std::string bytes_of(const core::Layer& layer) { return render::open_image(*layer.raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(); }
int at(const std::string& b, std::uint32_t x, std::uint32_t y, unsigned c) { return static_cast<unsigned char>(b[(std::size_t(y) * kSide + x) * 4 + c]); }

// A CMYK page (16 bits) with a paint layer "pic" and a hidden one "src", each of inks made by `inks` (alpha 1).
core::Document cmyk_page(const std::function<std::vector<double>(std::uint32_t, std::uint32_t)>& pic,
                         const std::function<std::vector<double>(std::uint32_t, std::uint32_t)>& src) {
    auto doc = run_ops(blank(), Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}},
                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "src"}}})).doc;
    const core::PageColor& color = *doc.page(0).color;
    for (const auto& [id, made] : {std::pair{std::string("pic"), pic}, std::pair{std::string("src"), src}}) {
        core::InkPixels p = core::blank_ink_pixels(color.space, 16, kSide, kSide);
        for (std::uint32_t y = 0; y < kSide; ++y)
            for (std::uint32_t x = 0; x < kSide; ++x) {
                const std::vector<double> ink = made(x, y);
                for (unsigned c = 0; c < 4; ++c) p.set(std::size_t(y) * kSide + x, c, ink[c]);
                p.set(std::size_t(y) * kSide + x, 4, 1.0);
            }
        for (auto& layer : doc.edit_page(0).layers)
            if (layer.id == id) layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    }
    for (auto& layer : doc.edit_page(0).layers)
        if (layer.id == "src") layer.visible = false;
    return doc;
}

core::InkPixels inks_of(const core::Document& doc, const std::string& id) { return core::decode_ink_raster(*layer_of(doc, id).ink_raster); }

const Json kLeftHalf{{"poly", Json::array({Json::array({0, 0}), Json::array({12.7, 0}), Json::array({12.7, 25.4}), Json::array({0, 25.4})})}};

}  // namespace

class TestImageCalc : public QObject {
    Q_OBJECT

private slots:
    // --- Blending (channel_cmds.rs:403-427) ---------------------------------------------------------------------------

    void blending() {
        using render::image_ops::blending_of;
        const auto multiply = blending_of(Json::object());
        QCOMPARE(multiply.mode, std::string("multiply"));
        QVERIFY(std::fabs(multiply.apply(0.5f, 0.5f) - 0.25f) < 1e-6f);
        const auto add = blending_of(Json{{"blending", "add"}, {"scale", 2}, {"offset", 51}});
        QVERIFY(std::fabs(add.apply(0.4f, 0.2f) - (0.3f + 0.2f)) < 1e-6f);
        const auto sub = blending_of(Json{{"blending", "subtract"}, {"offset", 128}});
        QVERIFY(std::fabs(sub.apply(0.2f, 0.5f) - (-0.3f + 128 / 255.0f)) < 1e-6f);
        QCOMPARE(sub.apply(0.0f, 1.0f), 0.0f);  // (cut to 0..1)
        // scale and offset clamped as PhotoCraft clamps them
        const auto wide = blending_of(Json{{"blending", "add"}, {"scale", 9}, {"offset", -999}});
        QCOMPARE(wide.scale, 2.0f);
        QVERIFY(std::fabs(wide.offset + 1.0f) < 1e-6f);
        QVERIFY(std::fabs(blending_of(Json{{"blending", "screen"}}).apply(0.5f, 0.5f) - 0.75f) < 1e-6f);
        QVERIFY(std::fabs(blending_of(Json{{"blending", "overlay"}}).apply(0.25f, 0.5f) - 0.25f) < 1e-6f);
        QCOMPARE(blending_of(Json{{"blending", "difference"}}).apply(0.2f, 0.7f), 0.5f);
        QVERIFY_EXCEPTION_THROWN(blending_of(Json{{"blending", "hue"}}), core::OpError);
        QVERIFY_EXCEPTION_THROWN(blending_of(Json{{"blending", "linearLight"}}), core::OpError);
        QVERIFY_EXCEPTION_THROWN(blending_of(Json{{"blending", "add"}, {"scale", "2"}}), core::OpError);
    }

    // --- apply_image ----------------------------------------------------------------------------------------------------

    // Another layer's composite multiplied into a layer: each colour plane by the source's; at 50 % half way; the area
    // and a channel's mask keep it to themselves; the transparency kept or painted (alpha the weight).
    void applyImageOnAnEightBitLayer() {
        const auto doc = two_layers();
        const Json base{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"source", Json{{"layer", "src"}}}, {"blending", "multiply"}};
        const std::string before = bytes_of(layer_of(doc, "paint"));
        const std::string multiplied = bytes_of(layer_of(run(doc, base).doc, "paint"));
        for (const std::uint32_t x : {10u, 100u, 190u}) {
            QVERIFY(std::abs(at(multiplied, x, 100, 0) - static_cast<int>(std::lround(at(before, x, 100, 0) * 128 / 255.0))) <= 1);
            QVERIFY(std::abs(at(multiplied, x, 100, 1) - static_cast<int>(std::lround(128 * 64 / 255.0))) <= 1);
            QCOMPARE(at(multiplied, x, 100, 2), at(before, x, 100, 2));  // (× 255)
        }
        // the clear top painted: opaque now (not kept)
        QCOMPARE(at(multiplied, 100, 10, 3), 255);
        Json keep = base;
        keep["preserve_transparency"] = true;
        const std::string kept = bytes_of(layer_of(run(doc, keep).doc, "paint"));
        QCOMPARE(at(kept, 100, 10, 3), 0);
        QCOMPARE(at(kept, 100, 100, 1), at(multiplied, 100, 100, 1));
        // half way
        Json half = base;
        half["opacity"] = 50;
        const std::string halfway = bytes_of(layer_of(run(doc, half).doc, "paint"));
        QVERIFY(std::abs(at(halfway, 100, 100, 1) - (128 + at(multiplied, 100, 100, 1)) / 2) <= 1);
        // the area: only its half
        Json inside = base;
        inside["area"] = kLeftHalf;
        const std::string some = bytes_of(layer_of(run(doc, inside).doc, "paint"));
        QCOMPARE(at(some, 50, 100, 1), at(multiplied, 50, 100, 1));
        QCOMPARE(at(some, 150, 100, 1), at(before, 150, 100, 1));
        // a mask from an alpha channel (the right half selected), and the same inverted
        const auto masked_doc = run(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "右"}, {"fill", "area"},
                                              {"area", Json{{"poly", Json::array({Json::array({12.7, 0}), Json::array({25.4, 0}), Json::array({25.4, 25.4}),
                                                                                  Json::array({12.7, 25.4})})}}}}).doc;
        Json masked = base;
        masked["mask"] = Json{{"channel", "右"}};
        const std::string right = bytes_of(layer_of(run(masked_doc, masked).doc, "paint"));
        QCOMPARE(at(right, 150, 100, 1), at(multiplied, 150, 100, 1));
        QCOMPARE(at(right, 50, 100, 1), at(before, 50, 100, 1));
        masked["mask"]["invert"] = true;
        const std::string left = bytes_of(layer_of(run(masked_doc, masked).doc, "paint"));
        QCOMPARE(at(left, 50, 100, 1), at(multiplied, 50, 100, 1));
        QCOMPARE(at(left, 150, 100, 1), at(before, 150, 100, 1));
        // a mask without its channel: none (as PhotoCraft reads it)
        Json no_mask = base;
        no_mask["mask"] = Json{{"layer", "src"}};
        QCOMPARE(bytes_of(layer_of(run(doc, no_mask).doc, "paint")), multiplied);
    }

    // The sources: one plate of the page as it shows (merged), a layer's transparency, inverted; a source of another
    // page of the book (the same size), refused when its size differs; what else is refused.
    void applyImageSources() {
        const auto doc = two_layers();
        const std::string before = bytes_of(layer_of(doc, "paint"));
        // the green plate of the page merged (the hidden layer not in it), normal: every plane takes it
        const std::string green = bytes_of(layer_of(run(doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"blending", "normal"},
                                                                   {"source", Json{{"channel", "green"}}}}).doc, "paint"));
        for (unsigned c = 0; c < 3; ++c) QCOMPARE(at(green, 100, 100, c), 128);
        // the layer's own transparency, inverted, added: its clear top white, its opaque rest as it was
        const std::string clear = bytes_of(layer_of(run(doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"blending", "add"},
                                                                   {"source", Json{{"layer", "paint"}, {"channel", "transparency"}, {"invert", true}}},
                                                                   {"preserve_transparency", false}}).doc, "paint"));
        QCOMPARE(at(clear, 100, 10, 0), 255);
        QCOMPARE(at(clear, 100, 100, 0), at(before, 100, 100, 0));
        // page 2 (the same size) as the source: its paper white, multiplied: as it was
        auto two = doc;
        const std::string same = bytes_of(layer_of(run(two, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"source", Json{{"page", 2}}}}).doc, "paint"));
        QCOMPARE(at(same, 100, 100, 0), at(before, 100, 100, 0));
        // refused: no target, two targets, a page of another size, an unknown channel, the transparency of the merged page
        auto wide = blank(30);
        wide.edit_page(0).layers.push_back(paint_layer(wide.page(0), "paint", picture([](std::uint32_t, std::uint32_t) { return std::array<int, 4>{1, 2, 3, 255}; })));
        wide.edit_page(1).spec = core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color");
        for (const auto& [book, op] : std::vector<std::pair<core::Document, Json>>{
                 {doc, Json{{"op", "apply_image"}, {"page", 1}}},
                 {doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"channel", "x"}}},
                 {wide, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"source", Json{{"page", 2}}}}},
                 {doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"source", Json{{"channel", "nothing"}}}}},
                 {doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"source", Json{{"channel", "transparency"}}}}},
                 {doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "paint"}, {"blending", "color"}}},
                 {doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "nothing"}}}}) {
            const std::string why = refusal(book, op);
            QVERIFY2(why.starts_with("apply: "), (op.dump() + " → " + why).c_str());
        }
    }

    // An alpha channel as the target: a colour source as the mean of its planes (a grey target), the channel's own
    // resolution kept.
    void applyImageOnAChannel() {
        const auto doc = run(two_layers(), Json{{"op", "add_channel"}, {"page", 1}, {"name", "α"}}).doc;
        const auto made = run(doc, Json{{"op", "apply_image"}, {"page", 1}, {"channel", "α"}, {"blending", "normal"}, {"source", Json{{"layer", "src"}}}});
        QCOMPARE(made.results.at(0)["channel"], Json(doc.page(0).channels[0].id));
        const render::channels::Plane plane = render::channels::plane_of(made.doc.page(0), made.doc.page(0).channels[0]);
        QVERIFY(std::fabs(plane.values[plane.values.size() / 2] - (128 + 64 + 255) / 3.0f / 255.0f) < 1.0f / 255);
        QVERIFY(refusal(doc, Json{{"op", "apply_image"}, {"page", 1}, {"channel", "red"}}).starts_with("apply: "));
    }

    // On a CMYK layer the planes are its inks (0..1, PhotoCraft's planes): multiplied as ink amounts.
    void applyImageOnInks() {
        const auto doc = cmyk_page([](std::uint32_t, std::uint32_t) { return std::vector<double>{0.5, 0.8, 0, 0.2}; },
                                   [](std::uint32_t, std::uint32_t) { return std::vector<double>{0.5, 0.5, 1, 1}; });
        const auto made = run(doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "pic"}, {"source", Json{{"layer", "src"}}}}).doc;
        const core::InkPixels inks = inks_of(made, "pic");
        const std::size_t i = std::size_t(100) * kSide + 100;
        QVERIFY(std::fabs(inks.value(i, 0) - 0.25) < 1e-3);
        QVERIFY(std::fabs(inks.value(i, 1) - 0.4) < 1e-3);
        QVERIFY(std::fabs(inks.value(i, 2)) < 1e-3);
        QVERIFY(std::fabs(inks.value(i, 3) - 0.2) < 1e-3);
        // one plate of the source (its cyan) into every plate
        const auto plate = run(doc, Json{{"op", "apply_image"}, {"page", 1}, {"id", "pic"}, {"blending", "normal"}, {"source", Json{{"layer", "src"}, {"channel", "cyan"}}}}).doc;
        QVERIFY(std::fabs(inks_of(plate, "pic").value(i, 3) - 0.5) < 1e-3);
    }

    // --- calculations ------------------------------------------------------------------------------------------------

    // Two grey sources blended into a new alpha channel (named, or the next free name), a new grey page after the page,
    // or a selection (the book as it was).
    void calculationsResults() {
        const auto doc = two_layers();
        const Json base{{"op", "calculations"}, {"page", 1}, {"source1", Json{{"layer", "src"}}}, {"source2", Json{{"layer", "paint"}}}, {"blending", "multiply"}};
        const float s = 0.299f * 128 / 255 + 0.587f * 64 / 255 + 0.114f;  // (the source's luminosity)
        // a new alpha channel
        const auto channel = run(doc, base);
        const core::Page& page = channel.doc.page(0);
        QCOMPARE(page.channels.size(), std::size_t(1));
        QCOMPARE(page.channels[0].name, std::string("Alpha 1"));
        QCOMPARE(channel.results.at(0)["channel"], Json(page.channels[0].id));
        const render::channels::Plane plane = render::channels::plane_of(page, page.channels[0]);
        const std::size_t i = std::size_t(100) * plane.width + 100;
        const float p = 0.299f * 100 / 255 + 0.587f * 128 / 255 + 0.114f * 155 / 255;
        QVERIFY2(std::fabs(plane.values[i] - s * p) < 1.5f / 255, qPrintable(QString::number(plane.values[i]) + " " + QString::number(s * p)));
        Json named = base;
        named["name"] = "合成";
        QCOMPARE(run(doc, named).doc.page(0).channels[0].name, std::string("合成"));
        // a new grey page after page 1
        Json as_page = base;
        as_page["result"] = "page";
        as_page["name"] = "結果";
        const auto paged = run(doc, as_page);
        QCOMPARE(paged.doc.pages.size(), doc.pages.size() + 1);
        QCOMPARE(paged.results.at(0)["page"], Json(2));
        const core::Page& made = paged.doc.page(1);
        QVERIFY(made.color && made.color->space == core::InkSpace::Gray);
        QCOMPARE(made.spec.width_mm.value(), doc.page(0).spec.width_mm.value());
        const core::InkPixels grey = core::decode_ink_raster(*made.layers[1].ink_raster);
        QVERIFY(std::fabs(grey.value(std::size_t(100) * grey.width + 100, 0) - s * p) < 1.5 / 255);
        // a selection: the book as it was
        Json as_selection = base;
        as_selection["result"] = "selection";
        as_selection["blending"] = "normal";
        as_selection["source1"] = Json{{"layer", "paint"}, {"channel", "transparency"}};
        const auto selected = run(doc, as_selection);
        QVERIFY(selected.results.at(0)["area"].is_object() && selected.results.at(0)["area"].contains("mask"));
        QCOMPARE(selected.doc.page(0).channels.size(), std::size_t(0));
        QCOMPARE(selected.doc.pages.size(), doc.pages.size());
        // refused: a result it does not make, a name taken
        Json wrong = base;
        wrong["result"] = "document";
        QVERIFY(refusal(doc, wrong).starts_with("apply: "));
        const auto taken = run(doc, named).doc;
        QVERIFY(refusal(taken, named).starts_with("apply: "));
    }

    // --- trap ----------------------------------------------------------------------------------------------------------

    // PhotoCraft's tests: two blocks of ink meeting (cyan | magenta) overlap at the seam, each far side keeps its one ink;
    // the paper gains no ink; a width of 0 changes nothing.
    void trapAlgorithm() {
        const std::size_t w = 8, h = 4, ch = 4;
        std::vector<float> px(w * h * ch, 0.0f);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) px[(y * w + x) * ch + (x < 4 ? 0 : 1)] = 1.0f;
        const auto orig = px;
        render::image_ops::trap(px, w, h, ch, 1);
        const auto v = [&](std::size_t x, std::size_t y, std::size_t c) { return px[(y * w + x) * ch + c]; };
        QVERIFY(v(3, 0, 1) > 0.5f);
        QVERIFY(v(4, 0, 0) > 0.5f);
        QCOMPARE(v(0, 0, 1), 0.0f);
        QCOMPARE(v(7, 0, 0), 0.0f);
        auto none = orig;
        render::image_ops::trap(none, w, h, ch, 0);
        QVERIFY(none == orig);
        // the paper (no ink) gains none
        std::vector<float> paper(6 * 3 * 4, 0.0f);
        paper[(6 + 3) * 4] = 1.0f;
        const auto was = paper;
        render::image_ops::trap(paper, 6, 3, 4, 1);
        for (std::size_t i = 0; i < 18; ++i)
            if (was[i * 4] == 0.0f) QCOMPARE(paper[i * 4], 0.0f);
        // a wide trap: the same as the max over the window (any width, in time of its own)
        std::vector<float> wide(40 * 5 * 4, 0.0f);
        for (std::size_t x = 0; x < 40; ++x)
            for (std::size_t y = 0; y < 5; ++y) wide[(y * 40 + x) * 4 + (x < 20 ? 0 : 1)] = 0.3f + 0.01f * static_cast<float>(x);
        render::image_ops::trap(wide, 40, 5, 4, 7);
        QVERIFY(std::fabs(wide[(2 * 40 + 13) * 4 + 1] - (0.3f + 0.01f * 20)) < 1e-6f);  // (magenta reaches 7 to the left)
        QCOMPARE(wide[(2 * 40 + 12) * 4 + 1], 0.0f);
        QVERIFY(std::fabs(wide[(2 * 40 + 26) * 4 + 0] - (0.3f + 0.01f * 19)) < 1e-6f);
    }

    // The op on a CMYK page's paint layer: the seam trapped by width_px (or width_mm at the layer's resolution); refused
    // on an RGB page.
    void trapOp() {
        const auto doc = cmyk_page([](std::uint32_t x, std::uint32_t) { return x < 100 ? std::vector<double>{1, 0, 0, 0} : std::vector<double>{0, 1, 0, 0}; },
                                   [](std::uint32_t, std::uint32_t) { return std::vector<double>{0, 0, 0, 0}; });
        const auto made = run(doc, Json{{"op", "trap"}, {"page", 1}, {"id", "pic"}, {"width_px", 2}});
        QCOMPARE(made.results.at(0)["trapped"], Json(true));
        QCOMPARE(made.results.at(0)["width_px"], Json(2));
        const core::InkPixels inks = inks_of(made.doc, "pic");
        const auto v = [&](std::uint32_t x, unsigned c) { return inks.value(std::size_t(50) * kSide + x, c); };
        QVERIFY(v(98, 1) > 0.99 && v(97, 1) == 0);
        QVERIFY(v(101, 0) > 0.99 && v(102, 0) == 0);
        QCOMPARE(v(10, 1), 0.0);
        const auto by_mm = run(doc, Json{{"op", "trap"}, {"page", 1}, {"id", "pic"}, {"width_mm", 0.381}});  // (3 px of 200 dpi)
        QCOMPARE(by_mm.results.at(0)["width_px"], Json(3));
        QVERIFY(refusal(doc, Json{{"op", "trap"}, {"page", 1}, {"id", "pic"}, {"width_px", 1}, {"width_mm", 1}}).starts_with("apply: "));
        QVERIFY(refusal(two_layers(), Json{{"op", "trap"}, {"page", 1}, {"id", "paint"}}).starts_with("apply: "));
    }

    // --- fade ----------------------------------------------------------------------------------------------------------

    void fadeMix() {
        const Rgba before{0.2f, 0.4f, 0.6f, 1}, after{0.6f, 0.8f, 1.0f, 1};
        const Rgba half = render::image_ops::fade_rgba(before, after, "normal", 0.5f);
        for (unsigned c = 0; c < 3; ++c) QVERIFY(std::fabs(half[c] - (before[c] + after[c]) / 2) < 1e-6f);
        const Rgba multiplied = render::image_ops::fade_rgba(before, after, "multiply", 1.0f);
        for (unsigned c = 0; c < 3; ++c) QVERIFY(std::fabs(multiplied[c] - before[c] * after[c]) < 1e-6f);
        QVERIFY((render::image_ops::fade_rgba(before, after, "screen", 0.0f) == before));
        // premultiplied: a clear pixel fading in takes the colour of the result, its alpha half way
        const Rgba in = render::image_ops::fade_rgba({0, 0, 0, 0}, {1, 0, 0, 1}, "normal", 0.5f);
        QVERIFY(std::fabs(in[0] - 1) < 1e-6f && std::fabs(in[3] - 0.5f) < 1e-6f);
    }

    // The op: the pixels before read from the book's assets, laid by the blend at the opacity; 0 % the very bytes before,
    // 100 % normal the layer as it is; refused without the book's folder, without the asset, with the pixels of another
    // size, with a blend it does not know.
    void fadeLayerOp() {
        QTemporaryDir tmp;
        const std::filesystem::path dir(tmp.path().toStdString());
        auto doc = two_layers();
        doc.asset_dir = dir;
        const std::string before_png = *layer_of(doc, "paint").raster_png;
        const std::string ref = storage::AssetStore(dir).put_bytes(before_png, ".png");
        const auto filtered = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "invert"}}).doc;
        const std::string before = bytes_of(layer_of(doc, "paint")), after = bytes_of(layer_of(filtered, "paint"));
        const Json base{{"op", "fade_layer"}, {"page", 1}, {"id", "paint"}, {"from", ref}};
        Json half = base;
        half["opacity"] = 50;
        const auto faded = run(filtered, half);
        QCOMPARE(faded.results.at(0)["faded"], Json("paint"));
        const std::string mid = bytes_of(layer_of(faded.doc, "paint"));
        for (const std::uint32_t x : {10u, 100u})
            for (unsigned c = 0; c < 3; ++c) QVERIFY(std::abs(at(mid, x, 100, c) - (at(before, x, 100, c) + at(after, x, 100, c)) / 2) <= 1);
        Json none = base;
        none["opacity"] = 0;
        QCOMPARE(*layer_of(run(filtered, none).doc, "paint").raster_png, before_png);
        QCOMPARE(layer_of(run(filtered, base).doc, "paint").raster_png, layer_of(filtered, "paint").raster_png);
        Json multiply = base;
        multiply["blend"] = "multiply";
        const std::string mul = bytes_of(layer_of(run(filtered, multiply).doc, "paint"));
        QVERIFY(std::abs(at(mul, 100, 100, 1) - static_cast<int>(std::lround(at(before, 100, 100, 1) * at(after, 100, 100, 1) / 255.0))) <= 1);
        // refused
        auto unsaved = filtered;
        unsaved.asset_dir.reset();
        QVERIFY(refusal(unsaved, base).starts_with("apply: "));
        Json missing = base;
        missing["from"] = "sha256:" + std::string(64, 'a');
        QVERIFY(refusal(filtered, missing).starts_with("apply: "));
        Json other = base;
        other["from"] = storage::AssetStore(dir).put_bytes(render::write_png(picture([](std::uint32_t, std::uint32_t) { return std::array<int, 4>{0, 0, 0, 255}; })
                                                                                 .resize(render::Size{20, 20})),
                                                           ".png");
        QVERIFY(refusal(filtered, other).starts_with("apply: "));
        Json strange = base;
        strange["blend"] = "dissolve";
        QVERIFY(refusal(filtered, strange).starts_with("apply: "));
        // inks: the pixels before as GKIR, half way in their ink amounts
        auto inks = cmyk_page([](std::uint32_t, std::uint32_t) { return std::vector<double>{0.2, 0, 0, 0}; },
                              [](std::uint32_t, std::uint32_t) { return std::vector<double>{0, 0, 0, 0}; });
        inks.asset_dir = dir;
        const std::string was = *layer_of(inks, "pic").ink_raster;
        const std::string ink_ref = storage::AssetStore(dir).put_bytes(was, core::kInkRasterSuffix);
        auto changed = inks;
        core::InkPixels more = inks_of(inks, "pic");
        for (std::size_t i = 0; i < more.pixel_count(); ++i) more.set(i, 0, 0.6);
        for (auto& layer : changed.edit_page(0).layers)
            if (layer.id == "pic") layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(more));
        const auto ink_faded = run(changed, Json{{"op", "fade_layer"}, {"page", 1}, {"id", "pic"}, {"from", ink_ref}, {"opacity", 50}}).doc;
        QVERIFY(std::fabs(inks_of(ink_faded, "pic").value(500, 0) - 0.4) < 1e-3);
    }

    // Independent review B (E): a precise layer's pixels before are given as the book saves them — its tiles, the
    // layer's color_raster as project.json holds it ({"tiles", "tile", "width", "height", "precision", "space",
    // "alpha"}) —, so a layer of any size fades, from the window or the command line; half way at 50 %, the very bytes
    // before at 0 %; tiles of another size or space, or one missing, refused. A whole asset larger than the store reads
    // of one (256 MiB) is refused in plain words before it is read.
    void fadeOfAPreciseLayerInTiles() {
        QTemporaryDir tmp;
        const std::filesystem::path dir(tmp.path().toStdString());
        constexpr std::uint32_t w = 300, h = 200;  // (two tiles across)
        Json pixels = Json::array();
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                for (const std::int64_t v : {std::int64_t(x) * 65535 / (w - 1), std::int64_t(32768), std::int64_t(y) * 65535 / (h - 1), std::int64_t(65535)})
                    pixels.push_back(v);
        auto doc = run(blank(), Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h}, {"precision", "u16"}, {"pixels", pixels}}).doc;
        const std::string id = doc.page(0).layers.back().id;
        doc.asset_dir = dir;
        const std::string was = *layer_of(doc, id).color_raster;
        storage::AssetStore store(dir);
        std::vector<std::string> refs;
        for (const std::string& tile : core::color_tiles(was)) refs.push_back(store.put_bytes(tile, core::kColorRasterSuffix));
        const Json tiles = core::tiled_metadata(core::ColorRasterView(was), refs, core::color_raster_space(doc.page(0).rgb));
        QCOMPARE(tiles["tiles"].size(), std::size_t(2));
        const auto filtered = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "invert"}}).doc;
        const Json base{{"op", "fade_layer"}, {"page", 1}, {"id", id}, {"from", tiles}};
        Json half = base;
        half["opacity"] = 50;
        const auto faded = run(filtered, half);
        QCOMPARE(faded.results.at(0)["faded"], Json(id));
        const core::ColorRasterView before(was), after(*layer_of(filtered, id).color_raster), mid(*layer_of(faded.doc, id).color_raster);
        for (const std::size_t i : {std::size_t(10), std::size_t(w * 100 + 280)})
            for (unsigned c = 0; c < 3; ++c) QVERIFY(std::fabs(mid.pixel(i)[c] - (before.pixel(i)[c] + after.pixel(i)[c]) / 2) < 1e-4);
        Json none = base;
        none["opacity"] = 0;
        QCOMPARE(*layer_of(run(filtered, none).doc, id).color_raster, was);
        // refused: tiles of another size, another space, a tile the book does not have, a form it does not know
        Json narrower = base;
        narrower["from"]["width"] = w - 1;
        QVERIFY(refusal(filtered, narrower).starts_with("apply: "));
        Json profiled = base;
        profiled["from"]["space"] = core::builtin_profile("display-p3")->ref();
        QVERIFY(refusal(filtered, profiled).starts_with("apply: "));
        Json lost = base;
        lost["from"]["tiles"][1] = "sha256:" + std::string(64, 'c');
        QVERIFY(refusal(filtered, lost).starts_with("apply: "));
        Json strange = base;
        strange["from"] = Json{{"asset", refs[0]}};
        QVERIFY(refusal(filtered, strange).starts_with("apply: "));
        // a whole asset past 256 MiB (a sparse file: nothing of it is read): refused, saying so
        const std::string big = "sha256:" + std::string(64, 'b');
        const std::filesystem::path file = store.path(big, core::kColorRasterSuffix);
        std::filesystem::create_directories(file.parent_path());
        { std::ofstream made(file, std::ios::binary); }
        std::filesystem::resize_file(file, storage::kAssetMaxBytes + 1);
        Json whole = base;
        whole["from"] = big;
        const std::string why = refusal(filtered, whole);
        QVERIFY2(why.starts_with("apply: ") && why.find("too large to read whole") != std::string::npos, why.c_str());
    }
};

QTEST_GUILESS_MAIN(TestImageCalc)
#include "test_image_calc.moc"
