// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 2.4, 4.2): フィルターギャラリー (filter_gallery) without Python — its
// table (47 effects in 6 folders, PhotoCraft's settings, ranges and defaults; algo/src/artistic_fx.rs params_parse_and_clamp,
// engine gallery_cmds/tests.rs gallery_validates_and_lists), its settings read as an op's; each effect changing a picture
// at every depth within 0..1 (every_effect_changes_pixels / every_gallery_filter_runs_at_every_depth), the same bytes
// whatever the tiling and the threads (results_do_not_depend_on_tiling: PhotoCraft within 1e-3, Genko the same bytes),
// a flat picture flat after the effects that put no marks of their own, the stack laid on in order and a hidden effect not
// (stack_applies_in_order_and_skips_hidden_layers), the colours by default and given (sketch_filters_record_and_use_the_
// current_colours), the three ways through filter_raster (8 bits, precise u16 and f32, CMYK, Lab, grey and multichannel:
// gallery_filters_run_in_other_colour_models), the same look at 200 and 600 dpi, a smart filter of it
// (gallery_is_a_smart_filter_on_smart_objects), a heavy stack told.

#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/gallery_kinds.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::fx::Rect;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi

std::string refused(const std::function<void()>& work) {
    try {
        work();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

// A precise raster (GKCR) of w × h from straight RGBA floats.
std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<std::array<float, 4>>& px, bool f32) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) {
        return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]};
    });
}

// PhotoCraft's test picture of the gallery (artistic_fx.rs tests: pattern) over w × h.
std::vector<std::array<float, 4>> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<std::array<float, 4>> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float v = static_cast<float>((x * 7 + y * 3) % 23) / 22.0f;
            out.push_back((x / 9 + y / 7) % 2 == 0 ? std::array<float, 4>{v, 0.3f, 1.0f - v, 1.0f} : std::array<float, 4>{0.9f, v * 0.5f, 0.2f, 1.0f});
        }
    return out;
}

// A disc over a ramp, side × side: the same picture at any resolution (the 200 and 600 dpi looks compared).
std::vector<std::array<float, 4>> disc_on_ramp(std::uint32_t side) {
    std::vector<std::array<float, 4>> px;
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const double u = (x + 0.5) / side - 0.5, v = (y + 0.5) / side - 0.5;
            const float ramp = static_cast<float>((x + 0.5) / side);
            px.push_back(u * u + v * v < 0.08 ? std::array<float, 4>{0.92f, 0.75f * ramp, 0.15f, 1.0f}
                                              : std::array<float, 4>{0.15f, 0.35f + 0.3f * ramp, 0.75f, 1.0f});
        }
    return px;
}

// PhotoCraft's test picture of the gallery's commands (engine gallery_cmds/tests.rs: session), light and dark squares.
std::vector<std::array<float, 4>> squares(std::uint32_t w, std::uint32_t h) {
    std::vector<std::array<float, 4>> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float v = static_cast<float>((x * 7 + y * 3) % 23) / 22.0f;
            std::array<float, 4> px{1, 1, 1, 1};
            for (unsigned c = 0; c < 3; ++c) px[c] = (x / 10 + y / 8) % 2 == 0 ? v : 1.0f - v * static_cast<float>(c + 1) / 4.0f;
            out.push_back(px);
        }
    return out;
}

std::vector<std::array<float, 4>> flat(std::uint32_t w, std::uint32_t h, std::array<float, 4> px) { return std::vector<std::array<float, 4>>(std::size_t(w) * h, px); }

const render::fx::Kernel& gallery() {
    const render::fx::Kernel* k = render::fx::kernel("filter_gallery");
    if (k == nullptr) throw core::Error("value", "no kernel filter_gallery");
    return *k;
}

// The stack's settings in their normal form (an op's).
Json stack(const Json& effects, const Json& more = Json::object()) {
    Json given{{"effects", effects}};
    given.update(more);
    return core::normalize_filter("filter_gallery", given, nullptr);
}
Json one(const char* filter, const Json& params = Json::object()) { return stack(Json::array({Json{{"filter", filter}, {"params", params}}})); }

// The stack on a precise raster (its new bytes), at scale s.
std::string run_precise(const std::string& raster, const Json& params, render::fx::Tiling tiling = {}, double s = 1) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::apply(*surface, gallery(), params, core::FilterInks::Meaning, grid, nullptr, s, nullptr, tiling);
    return render::fx::take_bytes(*surface);
}

// An 8-bit picture's RGBA bytes after the stack.
std::string run_eight(const render::Image& picture, const Json& params, render::fx::Tiling tiling = {}, double s = 1) {
    const auto surface = render::fx::eight_surface(picture);
    render::fx::apply(*surface, gallery(), params, core::FilterInks::Meaning, Rect{0, 0, picture.width(), picture.height()}, nullptr, s, nullptr, tiling);
    return render::fx::take_image(*surface).tobytes();
}

render::Image eight_pattern(int w, int h) {
    std::string bytes;
    for (const auto& p : pattern(std::uint32_t(w), std::uint32_t(h)))
        for (const float v : p) bytes.push_back(static_cast<char>(std::lround(v * 255)));
    return render::Image::frombytes("RGBA", render::Size{w, h}, bytes);
}

core::ApplyResult run(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run(doc, op);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(double side_mm = kInch) {
    auto doc = core::new_episode("ギャラリー", core::Num(1), 1, core::PageSpec::custom(side_mm, side_mm, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// A page with one precise paint layer "paint" (u16 or f32) holding `px` on a grid of w × h.
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<std::array<float, 4>>& px, bool f32, double side_mm = kInch) {
    auto doc = blank(side_mm);
    Json pixels = Json::array();
    for (const auto& p : px)
        for (const float v : p) pixels.push_back(f32 ? Json(static_cast<double>(v)) : Json(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h},
                                                          {"precision", f32 ? "f32" : "u16"}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const char* id) {
    for (const auto& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", std::string("no layer ") + id);
}

// A page of inks (cmyk, lab, gray or multichannel) with a paint layer "pic" of side × side pixels: `ink(x, y, c)` its values.
core::Document ink_page(const std::string& mode, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink, std::uint32_t side = 48) {
    Json set{{"op", "set_color_mode"}, {"mode", mode}, {"depth", mode == "multichannel" ? 8 : 16}};
    if (mode == "cmyk") set["profile"] = "coated-cmyk";
    auto doc = core::CommandBus(render::ops_registry())
                   .apply(blank(), Json::array({set, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}}), core::Actor("human:test"))
                   .doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, color.depth, side, side, color.space == core::InkSpace::Multi ? unsigned(color.plates.size()) : 0);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            for (unsigned c = 0; c < p.colours(); ++c) p.set(std::size_t(y) * side + x, c, ink(x, y, c));
            p.set(std::size_t(y) * side + x, p.colours(), 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

// Every pixel's colours of a precise raster.
std::vector<std::array<double, 4>> pixels_of(const std::string& raster) {
    const core::ColorRasterView view(raster);
    std::vector<std::array<double, 4>> out;
    for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) out.push_back(view.pixel(i));
    return out;
}

// The effects that put no marks of their own (no strokes, paper, cells nor grain): a flat picture stays flat.
const std::set<std::string> kUnmarked{"cutout", "neon_glow", "paint_daubs", "palette_knife", "plastic_wrap", "poster_edges", "smudge_stick", "watercolor",
                                      "accented_edges", "angled_strokes", "dark_strokes", "ink_outlines", "spatter", "sprayed_strokes", "sumi_e", "glass",
                                      "ocean_ripple", "bas_relief", "chrome", "photocopy", "plaster", "stamp", "glowing_edges"};
// The effects whose grain is one of the layer's pixels, and グラフィックペン's two tones in strokes a working pixel wide
// (soft-edged on a finer grid): compared at 200 and 600 dpi by their mean and spread.
const std::set<std::string> kGrained{"dry_brush", "film_grain", "fresco", "diffuse_glow", "note_paper", "reticulation", "grain", "graphic_pen"};

}  // namespace

class TestFxGallery : public QObject {
    Q_OBJECT

private slots:
    // --- the table ---------------------------------------------------------------------------------------------------------

    void fortySevenEffectsInSixFolders() {
        QCOMPARE(core::gallery_effects().size(), std::size_t(47));
        QCOMPARE(core::gallery_categories().size(), std::size_t(6));
        QCOMPARE(core::gallery_categories().front(), std::string_view("アーティスティック"));
        std::set<std::string_view> keys, upstream, names;
        std::vector<std::size_t> per(6, 0);
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            keys.insert(e.filter);
            upstream.insert(e.upstream);
            names.insert(e.name);
            QVERIFY2(!e.params.empty(), std::string(e.filter).c_str());
            const auto folder = std::find(core::gallery_categories().begin(), core::gallery_categories().end(), e.category);
            QVERIFY2(folder != core::gallery_categories().end(), std::string(e.filter).c_str());
            ++per[std::size_t(folder - core::gallery_categories().begin())];
            QCOMPARE(core::gallery_effect(e.filter), &e);
            for (const char c : e.filter) QVERIFY2(c == '_' || (c >= 'a' && c <= 'z'), std::string(e.filter).c_str());  // (snake_case)
        }
        QCOMPARE(keys.size(), std::size_t(47));
        QCOMPARE(upstream.size(), std::size_t(47));
        QCOMPARE(names.size(), std::size_t(47));
        QCOMPARE(per, (std::vector<std::size_t>{15, 8, 3, 14, 1, 6}));  // (Photoshop's folders)
        QVERIFY(core::gallery_effect("coloredPencil") == nullptr);   // (PhotoCraft's key is not the op's)
        QCOMPARE(core::gallery_effect("colored_pencil")->upstream, std::string_view("coloredPencil"));
        QCOMPARE(core::gallery_effect("colored_pencil")->name, std::string_view("色鉛筆"));
        QCOMPARE(core::gallery_effect("glowing_edges")->category, std::string_view("表現手法"));
        // the kind itself: in the table, a smart filter, the colours the inks mean, in the フィルター menu itself
        const core::FilterInfo* info = core::filter_info("filter_gallery");
        QVERIFY(info != nullptr);
        QCOMPARE(info->upstream, std::string_view("filter.filterGallery"));
        QCOMPARE(info->name, std::string_view("フィルターギャラリー"));
        QVERIFY(info->submenu.empty());
        QCOMPARE(info->inks, core::FilterInks::Meaning);
        QVERIFY(core::smart::filter_kind("filter_gallery"));
        QVERIFY(render::fx::kernel("filter_gallery") != nullptr);
        // the catalogue (genko inspect --filters): the 47 with PhotoCraft's commands
        const Json catalogue = core::filter_catalogue();
        QCOMPARE(catalogue["gallery"].size(), std::size_t(47));
        QCOMPARE(catalogue["gallery"][0]["command"], Json("filter.gallery.coloredPencil"));
        QCOMPARE(catalogue["gallery"][0]["settings"][0], (Json{{"key", "pencil_width"}, {"type", "number"}, {"default", 4}, {"min", 1}, {"max", 24}}));
        const Json& texturizer = catalogue["gallery"][46];
        QCOMPARE(texturizer["filter"], Json("texturizer"));
        QCOMPARE(texturizer["settings"][0]["choices"], (Json::array({"canvas", "brick", "burlap", "sandstone"})));
        QCOMPARE(texturizer["settings"][0]["default"], Json("canvas"));
        QCOMPARE(texturizer["settings"][3]["default"], Json("top"));
        QCOMPARE(catalogue["gallery"][5]["settings"][2], (Json{{"key", "glow_color"}, {"type", "rgb"}, {"default", Json::array({0, 158, 255})}}));
    }

    void theSettingsAreReadAsAnOps() {
        // defaults filled, visible filled in, the stack's colours black and white
        const Json pen = one("graphic_pen");
        QCOMPARE(pen["effects"][0], (Json{{"filter", "graphic_pen"},
                                          {"params", Json{{"stroke_length", 15}, {"light_dark_balance", 50}, {"stroke_direction", "rightDiagonal"}}},
                                          {"visible", true}}));
        QCOMPARE(pen["foreground"], (Json::array({0, 0, 0})));
        QCOMPARE(pen["background"], (Json::array({255, 255, 255})));
        // out of range clamped (PhotoCraft's Photoshop ranges), a choice by its name, an effect's own colours and visibility kept
        const Json given = stack(Json::array({Json{{"filter", "graphic_pen"}, {"params", Json{{"stroke_length", 99}, {"stroke_direction", "vertical"}}},
                                                   {"visible", false}, {"foreground", Json::array({255, 0, 0})}}}),
                                 Json{{"background", Json::array({0, 0, 255})}});
        QCOMPARE(given["effects"][0]["params"]["stroke_length"], Json(15));
        QCOMPARE(given["effects"][0]["params"]["stroke_direction"], Json("vertical"));
        QCOMPARE(given["effects"][0]["visible"], Json(false));
        QCOMPARE(given["effects"][0]["foreground"], (Json::array({255, 0, 0})));
        QVERIFY(!given["effects"][0].contains("background"));
        QCOMPARE(given["background"], (Json::array({0, 0, 255})));
        QCOMPARE(one("texturizer", Json{{"relief", 500}})["effects"][0]["params"]["relief"], Json(50));
        // refused: no effects, none at all, too many, an effect not known (PhotoCraft's key neither), a setting it has not, a
        // choice not one of its, an effect not an object, a key an effect has not, visible not true or false
        QVERIFY(refused([] { (void)core::normalize_filter("filter_gallery", Json::object(), nullptr); }).find("effects is required") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array()); }).find("1 to 32 effects") != std::string::npos);
        Json many = Json::array();
        for (int i = 0; i < 33; ++i) many.push_back(Json{{"filter", "cutout"}});
        QVERIFY(refused([&] { (void)stack(many); }).find("1 to 32 effects") != std::string::npos);
        many.erase(many.begin());
        QCOMPARE(stack(many)["effects"].size(), std::size_t(32));
        QVERIFY(refused([] { (void)stack(Json::array({Json{{"filter", "nope"}}})); }).find("the gallery has no effect 'nope'") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array({Json{{"filter", "coloredPencil"}}})); }).find("no effect") != std::string::npos);
        QVERIFY(refused([] { (void)one("cutout", Json{{"pencil_width", 3}}); }).find("has no setting 'pencil_width'") != std::string::npos);
        QVERIFY(refused([] { (void)one("texturizer", Json{{"texture", "wood"}}); }).find("must be one of canvas, brick, burlap, sandstone") != std::string::npos);
        QVERIFY(refused([] { (void)one("cutout", Json{{"number_of_levels", "four"}}); }).find("must be a number") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array({7})); }).find("must be an effect") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array({Json{{"filter", "cutout"}, {"opacity", 1}}})); }).find("has no setting 'opacity'") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array({Json{{"filter", "cutout"}, {"visible", 1}}})); }).find("visible must be true or false") != std::string::npos);
        QVERIFY(refused([] { (void)stack(Json::array({Json{{"filter", "cutout"}, {"foreground", "red"}}})); }).find("[r, g, b]") != std::string::npos);
        // (through the op: its words name the effect)
        const auto doc = precise_page(8, 8, pattern(8, 8), false);
        const std::string op_refused = refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "filter_gallery"},
                                                         {"effects", Json::array({Json{{"filter", "nope"}}})}});
        QVERIFY2(op_refused.find("filter_gallery.effects[0].filter") != std::string::npos, op_refused.c_str());
    }

    // --- the pixels ----------------------------------------------------------------------------------------------------------

    // Each effect alone with its defaults changes PhotoCraft's test picture at 8 bits, 16 and 32, its values finite and
    // within 0..1, its alpha as it was.
    void everyEffectChangesThePictureAtEveryDepth() {
        const render::Image eight = eight_pattern(56, 40);
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            const std::string name(e.filter);
            const Json params = one(e.filter.data());
            QVERIFY2(run_eight(eight, params) != eight.tobytes(), (name + " @8 changed nothing").c_str());
            for (const bool f32 : {false, true}) {
                const std::string raster = precise(56, 40, pattern(56, 40), f32);
                const std::string out = run_precise(raster, params);
                QVERIFY2(out != raster, (name + (f32 ? " @32" : " @16") + " changed nothing").c_str());
                for (const auto& p : pixels_of(out)) {
                    for (unsigned c = 0; c < 3; ++c) QVERIFY2(std::isfinite(p[c]) && p[c] >= -1e-6 && p[c] <= 1 + 1e-6, (name + " out of range").c_str());
                    QVERIFY2(std::fabs(p[3] - 1.0) < 1e-9, (name + ": its alpha changed").c_str());
                }
            }
        }
        // (a half-clear picture: the clear pixels stay clear)
        std::vector<std::array<float, 4>> half = pattern(40, 30);
        for (std::size_t i = 0; i < half.size(); i += 2) half[i][3] = 0.0f;
        const std::string raster = precise(40, 30, half, true);
        const auto out = pixels_of(run_precise(raster, stack(Json::array({Json{{"filter", "cutout"}}, Json{{"filter", "glass"}}}))));
        for (std::size_t i = 0; i < out.size(); ++i) QCOMPARE(out[i][3], static_cast<double>(half[i][3]));
    }

    // The same bytes whatever the tiling and the threads, at the working scale and on finer grids (PhotoCraft's results are
    // within 1e-3; Genko's blurs sum in whole numbers).
    void everyEffectIsTileAndThreadIndependent() {
        const std::string raster = precise(72, 52, pattern(72, 52), true);
        const std::string deep = precise(72, 52, pattern(72, 52), false);
        const render::Image eight = eight_pattern(72, 52);
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            const std::string name(e.filter);
            const Json params = one(e.filter.data());
            for (const double s : {1.0, 1.7, 3.0}) {
                const std::string a = run_precise(raster, params, {256, 1}, s);
                QVERIFY2(a == run_precise(raster, params, {7, 3}, s), qPrintable(QStringLiteral("%1 at %2: tiles of 7").arg(QString::fromStdString(name)).arg(s)));
                QVERIFY2(a == run_precise(raster, params, {16, 2}, s), qPrintable(QStringLiteral("%1 at %2: tiles of 16").arg(QString::fromStdString(name)).arg(s)));
            }
            QVERIFY2(run_precise(deep, params, {256, 1}) == run_precise(deep, params, {11, 4}), (name + " u16").c_str());
            QVERIFY2(run_eight(eight, params, {256, 1}) == run_eight(eight, params, {9, 2}), (name + " 8 bits").c_str());
        }
        // a stack of several, each reading round its pixels
        const Json several = stack(Json::array({Json{{"filter", "dry_brush"}}, Json{{"filter", "spatter"}}, Json{{"filter", "accented_edges"}},
                                                Json{{"filter", "stained_glass"}, {"params", Json{{"cell_size", 4}}}}}));
        QVERIFY(run_precise(raster, several, {256, 1}) == run_precise(raster, several, {5, 4}));
        QVERIFY(run_precise(raster, several, {256, 1}, 2.0) == run_precise(raster, several, {13, 3}, 2.0));
    }

    // A flat picture stays flat after the effects that put no marks of their own; the others put theirs.
    void aFlatPictureStaysFlat() {
        const std::array<float, 4> colour{0.3f, 0.55f, 0.8f, 1.0f};
        const std::string raster = precise(48, 40, flat(48, 40, colour), true);
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            const std::string name(e.filter);
            const auto out = pixels_of(run_precise(raster, one(e.filter.data())));
            double spread = 0;
            for (const auto& p : out)
                for (unsigned c = 0; c < 3; ++c) spread = std::max(spread, std::fabs(p[c] - out[0][c]));
            if (kUnmarked.contains(name)) {
                QVERIFY2(spread < 1e-6, qPrintable(QStringLiteral("%1 is not flat (%2)").arg(QString::fromStdString(name)).arg(spread)));
            } else {
                QVERIFY2(spread > 1e-3, qPrintable(QStringLiteral("%1 put no marks (%2)").arg(QString::fromStdString(name)).arg(spread)));
            }
        }
    }

    // The stack in order (the first laid on first), a hidden effect not laid on, all hidden: the picture as it was.
    void theStackIsInOrderAndHiddenOnesAreNotLaidOn() {
        const std::string raster = precise(56, 40, pattern(56, 40), true);
        const Json cutout{{"filter", "cutout"}, {"params", Json{{"number_of_levels", 3}}}};
        const Json edges{{"filter", "glowing_edges"}};
        Json hidden = edges;
        hidden["visible"] = false;
        const std::string ab = run_precise(raster, stack(Json::array({cutout, edges})));
        const std::string ba = run_precise(raster, stack(Json::array({edges, cutout})));
        QVERIFY(ab != ba);
        QCOMPARE(run_precise(raster, stack(Json::array({cutout, hidden}))), run_precise(raster, stack(Json::array({cutout}))));
        Json off = cutout;
        off["visible"] = false;
        QCOMPARE(run_precise(raster, stack(Json::array({off, hidden}))), raster);
        // (the stack of two is the second laid on what the first made, within the window's colours)
        QVERIFY(ab != run_precise(raster, stack(Json::array({cutout}))));
    }

    // The colours: black ink on white paper by default; the stack's (the window's main and sub colours) and an effect's own.
    void theColoursByDefaultAndGiven() {
        const std::string raster = precise(40, 32, squares(40, 32), true);
        // スタンプ is two tones: black and white by default
        for (const auto& p : pixels_of(run_precise(raster, one("stamp")))) {
            const bool black = p[0] < 0.05 && p[1] < 0.05 && p[2] < 0.05, white = p[0] > 0.95 && p[1] > 0.95 && p[2] > 0.95;
            QVERIFY2(black || white || std::fabs(p[0] - p[1]) < 1e-6, "not black and white");
        }
        // red ink on blue paper (PhotoCraft's sketch_filters_record_and_use_the_current_colours)
        const auto coloured = pixels_of(run_precise(raster, stack(Json::array({Json{{"filter", "stamp"}}}),
                                                                   Json{{"foreground", Json::array({255, 0, 0})}, {"background", Json::array({0, 0, 255})}})));
        QVERIFY(std::all_of(coloured.begin(), coloured.end(), [](const auto& p) { return p[1] < 0.05; }));
        QVERIFY(std::any_of(coloured.begin(), coloured.end(), [](const auto& p) { return p[0] > 0.9; }));
        QVERIFY(std::any_of(coloured.begin(), coloured.end(), [](const auto& p) { return p[2] > 0.9; }));
        // an effect's own over the stack's
        const auto own = pixels_of(run_precise(raster, stack(Json::array({Json{{"filter", "stamp"}, {"foreground", Json::array({0, 255, 0})}}}),
                                                              Json{{"foreground", Json::array({255, 0, 0})}, {"background", Json::array({0, 0, 255})}})));
        QVERIFY(std::any_of(own.begin(), own.end(), [](const auto& p) { return p[1] > 0.9; }));
        QVERIFY(std::all_of(own.begin(), own.end(), [](const auto& p) { return p[0] < 0.05; }));
        // ネオン光彩's glow colour
        QVERIFY(run_precise(raster, one("neon_glow")) != run_precise(raster, one("neon_glow", Json{{"glow_color", Json::array({255, 40, 0})}})));
    }

    // --- the three ways (filter_raster) ----------------------------------------------------------------------------------------

    void everyWayThroughFilterRaster() {
        const Json effects = Json::array({Json{{"filter", "cutout"}}, Json{{"filter", "texturizer"}, {"params", Json{{"relief", 20}}}}});
        Json op{{"op", "filter_raster"}, {"page", 1}, {"kind", "filter_gallery"}, {"effects", effects}};
        // 8 bits: a paint layer of a picture
        {
            auto doc = blank();
            render::Image picture = render::Image::create("RGBA", render::Size{200, 200}, render::Ink{40, 60, 140, 255});
            picture.paste(render::Ink{220, 180, 30, 255}, render::Box{60, 60, 140, 140});
            core::Layer layer;
            layer.id = "paint";
            layer.kind = core::LayerKind::Raster;
            layer.panel_clip = false;
            render::raster::save_raster(doc.page(0), layer, picture);
            doc.edit_page(0).layers.push_back(layer);
            Json on = op;
            on["id"] = "paint";
            const core::ApplyResult done = run(doc, on);
            const render::Image after = render::raster::layer_pixels(done.doc.page(0), layer_of(done.doc, "paint")).convert("RGBA");
            QVERIFY(after.tobytes() != picture.tobytes());
            // (the reply: the stack it was drawn with, in its normal form)
            QCOMPARE(done.results[0]["filter"]["kind"], Json("filter_gallery"));
            QCOMPARE(done.results[0]["filter"]["effects"][1]["params"]["relief"], Json(20));
            QCOMPARE(done.results[0]["filter"]["background"], (Json::array({255, 255, 255})));
        }
        // precise u16 and f32
        for (const bool f32 : {false, true}) {
            const auto doc = precise_page(32, 32, pattern(32, 32), f32);
            Json on = op;
            on["id"] = "paint";
            const auto done = run(doc, on).doc;
            QVERIFY(*layer_of(done, "paint").color_raster != *layer_of(doc, "paint").color_raster);
            QCOMPARE(core::ColorRasterView(*layer_of(done, "paint").color_raster).metadata("")["precision"], Json(f32 ? "f32" : "u16"));
        }
        // pages of inks: the colours they mean, back by the page's rules (PhotoCraft's gallery_filters_run_in_other_colour_models)
        for (const std::string mode : {"cmyk", "lab", "gray", "multichannel"}) {
            const auto doc = ink_page(mode, [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 7 + y * 3 + c * 11) % 23) / 22.0; });
            for (const char* filter : {"cutout", "graphic_pen", "texturizer", "stained_glass"}) {
                Json on{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "filter_gallery"}, {"effects", Json::array({Json{{"filter", filter}}})}};
                const std::string why = refusal(doc, on);
                QVERIFY2(why.empty(), (mode + " " + filter + ": " + why).c_str());
                QVERIFY2(*layer_of(run(doc, on).doc, "pic").ink_raster != *layer_of(doc, "pic").ink_raster, (mode + " " + filter).c_str());
            }
        }
        // a colour left as it was keeps its inks: a CMYK page's K-only grey through an effect that paints the paper white —
        // where the result is the paper, the inks the rules give white (none)
        const auto k_only = ink_page("cmyk", [](std::uint32_t x, std::uint32_t, unsigned c) { return c == 3 ? (x < 24 ? 0.85 : 0.1) : 0.0; });
        const auto stamped = run(k_only, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "filter_gallery"},
                                             {"effects", Json::array({Json{{"filter", "stamp"}}})}}).doc;
        const core::InkPixels inks = core::decode_ink_raster(*layer_of(stamped, "pic").ink_raster);
        QVERIFY(inks.value(10 * 48 + 40, 3) < 0.02 && inks.value(10 * 48 + 40, 0) < 0.02);  // (the light side: paper)
        QVERIFY(inks.value(10 * 48 + 5, 3) > 0.5);                                         // (the dark side: ink)
    }

    // --- the look at 200 and 600 dpi ---------------------------------------------------------------------------------------

    // The same picture on a precise layer of 200 × 200 (the page at 200 dpi) and of 600 × 600 (600 dpi): each effect's
    // marks the same size on the paper — the 600 dpi one shrunk to a third near the 200 dpi one; an effect whose grain is
    // one of the layer's pixels, near in its mean and its spread.
    void theSameLookAt200And600Dpi() {
        const auto low_doc = precise_page(200, 200, disc_on_ramp(200), false);
        const auto high_doc = precise_page(600, 600, disc_on_ramp(600), false);
        QString table, far;
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            const std::string name(e.filter);
            const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "filter_gallery"}, {"effects", Json::array({Json{{"filter", name}}})}};
            const auto low = pixels_of(*layer_of(run(low_doc, op).doc, "paint").color_raster);
            const auto high = pixels_of(*layer_of(run(high_doc, op).doc, "paint").color_raster);
            double differs = 0, count = 0;
            std::array<double, 3> mean_low{}, mean_high{}, var_low{}, var_high{};
            for (std::uint32_t y = 0; y < 200; ++y)
                for (std::uint32_t x = 0; x < 200; ++x) {
                    std::array<double, 3> shrunk{};
                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx)
                            for (unsigned c = 0; c < 3; ++c) shrunk[c] += high[std::size_t(y * 3 + dy) * 600 + x * 3 + dx][c] / 9;
                    const auto& q = low[std::size_t(y) * 200 + x];
                    for (unsigned c = 0; c < 3; ++c) {
                        differs += std::fabs(shrunk[c] - q[c]);
                        count += 1;
                        mean_low[c] += q[c];
                    }
                }
            for (const auto& p : high)
                for (unsigned c = 0; c < 3; ++c) mean_high[c] += p[c];
            for (unsigned c = 0; c < 3; ++c) {
                mean_low[c] /= double(low.size());
                mean_high[c] /= double(high.size());
            }
            for (const auto& p : low)
                for (unsigned c = 0; c < 3; ++c) var_low[c] += (p[c] - mean_low[c]) * (p[c] - mean_low[c]) / double(low.size());
            for (const auto& p : high)
                for (unsigned c = 0; c < 3; ++c) var_high[c] += (p[c] - mean_high[c]) * (p[c] - mean_high[c]) / double(high.size());
            const double mad = differs / count;
            double mean_gap = 0, spread = 1;
            for (unsigned c = 0; c < 3; ++c) {
                mean_gap = std::max(mean_gap, std::fabs(mean_low[c] - mean_high[c]));
                if (var_low[c] > 1e-6) spread = std::max(spread, std::max(std::sqrt(var_high[c] / var_low[c]), std::sqrt(var_low[c] / std::max(var_high[c], 1e-12))));
            }
            table += QStringLiteral("%1 %2 %3 %4\n").arg(QString::fromStdString(name)).arg(mad, 0, 'f', 4).arg(mean_gap, 0, 'f', 4).arg(spread, 0, 'f', 3);
            const bool near = kGrained.contains(name) ? mean_gap < 0.03 && spread < 1.35 : mad < 0.06 && mean_gap < 0.03;
            if (!near) far += QStringLiteral("%1 (shrunk %2, mean %3, spread %4); ").arg(QString::fromStdString(name)).arg(mad).arg(mean_gap).arg(spread);
        }
        if (qEnvironmentVariableIsSet("GENKO_GALLERY_TABLE")) qInfo("%s", qPrintable(table));
        QVERIFY2(far.isEmpty(), qPrintable(far));
    }

    // --- a smart filter --------------------------------------------------------------------------------------------------------

    // On a smart object a smart filter of its stack (PhotoCraft's gallery_is_a_smart_filter_on_smart_objects): kept in its
    // normal form, the book naming filters@1, the smart object drawn with it; its settings checked as a book's are.
    void aSmartFilterOfTheGallery() {
        auto doc = precise_page(64, 64, pattern(64, 64), false);
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}).doc;
        const render::Image plain = render::layer_image(doc.page(0), layer_of(doc, "so"), 200, &doc);
        doc = run(doc, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "filter_gallery"},
                            {"effects", Json::array({Json{{"filter", "texturizer"}, {"params", Json{{"relief", 20}}}}})}}).doc;
        const Json& filters = (*layer_of(doc, "so").smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("filter_gallery"));
        QCOMPARE(filters[0]["params"]["effects"][0]["filter"], Json("texturizer"));
        QCOMPARE(filters[0]["params"]["effects"][0]["params"]["relief"], Json(20));
        QCOMPARE(filters[0]["params"]["effects"][0]["params"]["texture"], Json("canvas"));
        QCOMPARE(filters[0]["params"]["foreground"], (Json::array({0, 0, 0})));
        QVERIFY(core::uses_filters(doc));
        const render::Image drawn = render::layer_image(doc.page(0), layer_of(doc, "so"), 200, &doc);
        QVERIFY(drawn.tobytes() != plain.tobytes());
        // set again: its stack replaced
        doc = run(doc, Json{{"op", "smart_filter_set"}, {"page", 1}, {"id", "so"}, {"index", 0},
                            {"params", Json{{"effects", Json::array({Json{{"filter", "cutout"}}, Json{{"filter", "stamp"}, {"visible", false}}})}}}}).doc;
        QCOMPARE((*layer_of(doc, "so").smart)["filters"][0]["params"]["effects"].size(), std::size_t(2));
        QCOMPARE((*layer_of(doc, "so").smart)["filters"][0]["params"]["effects"][1]["visible"], Json(false));
        // refused as an op refuses: an effect not known
        QVERIFY(!refusal(doc, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "filter_gallery"},
                                   {"effects", Json::array({Json{{"filter", "nope"}}})}}).empty());
    }

    // The clear pixels bring no colour in (review P2-1): a disc on a clear layer — the effects that put no marks of their own
    // leave it one flat colour to its edge (no dark rim from the clear pixels' black) —, and whatever colours the clear
    // pixels keep unseen, what shows is the same bytes (every effect, at 1 and at 3 times the working pixels).
    void theClearPixelsBringNoColour() {
        const auto disc = [](std::uint32_t side, bool noisy) {
            std::vector<std::array<float, 4>> px;
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const double u = (x + 0.5) / side - 0.5, v = (y + 0.5) / side - 0.5;
                    if (u * u + v * v < 0.09) {
                        px.push_back({0.85f, 0.25f, 0.2f, 1.0f});
                    } else if (noisy) {
                        px.push_back({static_cast<float>((x * 37 + y * 11) % 17) / 16.0f, static_cast<float>((x * 5 + y * 23) % 13) / 12.0f,
                                      static_cast<float>((x + y) % 2), 0.0f});
                    } else {
                        px.push_back({0.0f, 0.0f, 0.0f, 0.0f});
                    }
                }
            return px;
        };
        const std::string clear = precise(64, 64, disc(64, false), true), unseen = precise(64, 64, disc(64, true), true);
        for (const char* filter : {"cutout", "watercolor", "stamp", "palette_knife", "sumi_e", "ink_outlines", "bas_relief", "photocopy"}) {
            const auto out = pixels_of(run_precise(clear, one(filter)));
            std::array<double, 4> first{-1, -1, -1, -1};
            double spread = 0;
            for (const auto& p : out) {
                if (p[3] <= 0) continue;
                if (first[3] < 0) first = p;
                for (unsigned c = 0; c < 3; ++c) spread = std::max(spread, std::fabs(p[c] - first[c]));
            }
            QVERIFY2(spread < 1e-6, qPrintable(QStringLiteral("%1: the disc's edge took another colour (%2)").arg(filter).arg(spread)));
        }
        const auto shown = [](const std::string& raster) {
            std::vector<std::array<double, 4>> out;
            for (const auto& p : pixels_of(raster))
                if (p[3] > 0) out.push_back(p);
            return out;
        };
        for (const core::GalleryEffectKind& e : core::gallery_effects()) {
            const Json params = one(e.filter.data());
            for (const double s : {1.0, 3.0})
                QVERIFY2(shown(run_precise(clear, params, {}, s)) == shown(run_precise(unseen, params, {}, s)),
                         qPrintable(QStringLiteral("%1 at %2: the clear pixels' colours showed").arg(QString::fromLatin1(e.filter.data())).arg(s)));
        }
    }

    // A part of the layer's grid worked alone (フィルターギャラリー's preview: PlaceScope — its origin, the grid's base box): its
    // marks where the whole grid's run puts them, the same pixels away from its edges (review P2-2).
    void aPartWorkedAloneIsTheWholesPart() {
        const std::uint32_t w = 120, h = 90;
        const auto whole_px = pattern(w, h);
        const std::string whole = precise(w, h, whole_px, true);
        const Rect part{30, 20, 100, 76};
        std::vector<std::array<float, 4>> part_px;
        for (int y = part.y0; y < part.y1; ++y)
            for (int x = part.x0; x < part.x1; ++x) part_px.push_back(whole_px[std::size_t(y) * w + std::size_t(x)]);
        const std::string piece = precise(std::uint32_t(part.width()), std::uint32_t(part.height()), part_px, true);
        for (const Json& params : {one("texturizer", Json{{"relief", 30}}), one("stained_glass", Json{{"cell_size", 6}}), one("craquelure"), one("graphic_pen"),
                                   one("halftone_pattern", Json{{"pattern_type", "circle"}}), one("mosaic_tiles"), one("grain"), one("film_grain")}) {
            const std::string name = params["effects"][0]["filter"].get<std::string>();
            const std::string all = run_precise(whole, params);
            std::string alone;
            {
                const render::fx::PlaceScope place(part.x0, part.y0, Rect{0, 0, int(w), int(h)});
                alone = run_precise(piece, params);
            }
            const int halo = render::fx::kernel("filter_gallery")->halo(params, render::fx::Ctx{}).radius;
            const auto all_px = pixels_of(all), alone_px = pixels_of(alone);
            for (int y = part.y0 + halo; y < part.y1 - halo; ++y)
                for (int x = part.x0 + halo; x < part.x1 - halo; ++x) {
                    const auto& a = all_px[std::size_t(y) * w + std::size_t(x)];
                    const auto& b = alone_px[std::size_t(y - part.y0) * std::size_t(part.width()) + std::size_t(x - part.x0)];
                    QVERIFY2(a == b, qPrintable(QStringLiteral("%1 at %2, %3").arg(QString::fromStdString(name)).arg(x).arg(y)));
                }
            // (without the place: the marks elsewhere — what the preview showed before)
            QVERIFY2(pixels_of(run_precise(piece, params)) != alone_px, name.c_str());
        }
    }

    // Large settings (the reach widest: ステンドグラス 50, はね・スプレー 25, パッチワーク 10, ガラス 20, 塗料 50 wide, 底塗り 40)
    // at three times the working pixels: still the same bytes whatever the tiling.
    void largeSettingsAreTileIndependent() {
        const std::string raster = precise(120, 90, pattern(120, 90), true);
        for (const Json& params : {one("stained_glass", Json{{"cell_size", 50}, {"border_thickness", 20}}), one("spatter", Json{{"spray_radius", 25}}),
                                   one("sprayed_strokes", Json{{"spray_radius", 25}, {"stroke_length", 20}}), one("patchwork", Json{{"square_size", 10}}),
                                   one("glass", Json{{"distortion", 20}}), one("paint_daubs", Json{{"brush_size", 50}, {"brush_type", "wideSharp"}}),
                                   one("underpainting", Json{{"brush_size", 40}}), one("ink_outlines", Json{{"stroke_length", 50}})}) {
            const std::string name = params["effects"][0]["filter"].get<std::string>();
            const std::string a = run_precise(raster, params, {256, 1}, 3.0);
            QVERIFY2(a == run_precise(raster, params, {7, 3}, 3.0), name.c_str());
            QVERIFY2(a == run_precise(raster, params, {23, 2}, 3.0), name.c_str());
        }
    }

    // Every effect hidden: a smart filter keeps them so, the destructive op refuses (it would change nothing).
    void everyEffectHidden() {
        auto doc = precise_page(32, 32, pattern(32, 32), false);
        const Json hidden = Json::array({Json{{"filter", "cutout"}, {"visible", false}}, Json{{"filter", "stamp"}, {"visible", false}}});
        const std::string why = refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "filter_gallery"}, {"effects", hidden}});
        QVERIFY2(why.find("every effect is hidden") != std::string::npos, why.c_str());
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}).doc;
        doc = run(doc, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "filter_gallery"}, {"effects", hidden}}).doc;
        QCOMPARE((*layer_of(doc, "so").smart)["filters"][0]["params"]["effects"].size(), std::size_t(2));
    }

    // A smart object's gallery drawn at the screen's resolution (200 dpi) and at the print's (600): the same look, the
    // print's shrunk to a third near the screen's, the same mean colour (design 4.2; theSameLookAt200And600Dpi's measures).
    void aSmartObjectAtTheScreensAndThePrintsResolution() {
        auto doc = precise_page(200, 200, disc_on_ramp(200), false);
        doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}).doc;
        doc = run(doc, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "filter_gallery"},
                            {"effects", Json::array({Json{{"filter", "accented_edges"}}, Json{{"filter", "texturizer"}, {"params", Json{{"relief", 20}}}}})}}).doc;
        const render::Image screen = render::layer_image(doc.page(0), layer_of(doc, "so"), 200, &doc).convert("RGBA");
        const render::Image print = render::layer_image(doc.page(0), layer_of(doc, "so"), 600, &doc).convert("RGBA");
        QCOMPARE(screen.width(), 200);
        QCOMPARE(print.width(), 600);
        const std::string a = screen.tobytes(), b = print.tobytes();
        double differs = 0, count = 0;
        std::array<double, 3> mean_screen{}, mean_print{};
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x)
                for (int c = 0; c < 3; ++c) {
                    double shrunk = 0;
                    for (int dy = 0; dy < 3; ++dy)
                        for (int dx = 0; dx < 3; ++dx) shrunk += static_cast<unsigned char>(b[(std::size_t(y * 3 + dy) * 600 + std::size_t(x * 3 + dx)) * 4 + c]);
                    const double q = static_cast<unsigned char>(a[(std::size_t(y) * 200 + std::size_t(x)) * 4 + c]);
                    differs += std::fabs(shrunk / 9 - q) / 255.0;
                    count += 1;
                    mean_screen[std::size_t(c)] += q / 255.0 / 40000;
                    mean_print[std::size_t(c)] += shrunk / 9 / 255.0 / 40000;
                }
        double mean_gap = 0;
        for (std::size_t c = 0; c < 3; ++c) mean_gap = std::max(mean_gap, std::fabs(mean_screen[c] - mean_print[c]));
        if (qEnvironmentVariableIsSet("GENKO_GALLERY_TABLE")) qInfo("smart object 200 / 600: shrunk %f, mean %f", differs / count, mean_gap);
        QVERIFY2(differs / count < 0.06 && mean_gap < 0.03, qPrintable(QStringLiteral("shrunk %1, mean %2").arg(differs / count).arg(mean_gap)));
    }

    // A heavy stack: B4 at 600 dpi (the window then works it on a thread with 中止); a small layer at 200 dpi not.
    void aHeavyStackIsTold() {
        const Json params = stack(Json::array({Json{{"filter", "dry_brush"}}, Json{{"filter", "texturizer"}}}));
        QVERIFY(render::fx::work("filter_gallery", params, 6071ULL * 8598, 3) >= render::fx::kHeavyWork);
        QVERIFY(render::fx::work("filter_gallery", params, 400ULL * 400, 1) < render::fx::kHeavyWork);
        QVERIFY(render::fx::work("filter_gallery", one("film_grain"), 6071ULL * 8598, 3) < render::fx::work("filter_gallery", params, 6071ULL * 8598, 3));
    }
};

QTEST_GUILESS_MAIN(TestFxGallery)
#include "test_fx_gallery.moc"
