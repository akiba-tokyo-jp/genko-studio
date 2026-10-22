// PC4 節 D2 (SPEC.md COLOR-08「フィルター」: 変形・ピクセレート・表現手法) without Python — PhotoCraft's own tests of them
// (algo/src/tests.rs 335-378: mosaic's cells, stylize's basics, distortions at zero, twirl's centre; tests_ext.rs 221-375:
// identities at zero strength, flat stays flat, mezzotint, halftone, pointillize, diffuse, trace contour, tiles, extrude;
// 380-406 and 638-668: displace and shear, wind, oil paint) on Genko's surfaces; then each kind the same bytes whatever the
// tiling and threads at a finer grid's scale, a part of the grid worked alone (PlaceScope) as the whole there, a clear
// pixel's colour never brought in, a distortion centred on the selection's box, カラーハーフトーン's four screens of a CMYK
// page, the lightness of a page of inks (an edge, a relief, a grey on K alone; a Lab page's a and b grey), the same look at
// 200 and 600 dpi, and 置き換え's map — a file's data, a layer of the book, its merged picture, a map the book holds —,
// refused when it cannot be read, made the book's in a smart filter (Document::filter_files) and carried to another book
// with its smart object (bring_contents); and each kind on an opened picture's page (its grid "finest", 300 ppi).

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "formats/pillow_save.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "storage/gc.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::fx::Rect;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi
using Px = std::array<float, 4>;

std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32 = true) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) { return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]}; });
}

// PhotoCraft's test picture over w × h (opaque).
std::vector<Px> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<Px> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            out.push_back({static_cast<float>((x * 7 + y * 3) % 64) / 63.0f, static_cast<float>((x * x + y) % 50) / 49.0f, static_cast<float>(y % 9) / 8.0f, 1.0f});
    return out;
}

std::vector<Px> flat(std::uint32_t w, std::uint32_t h, Px px) { return std::vector<Px>(std::size_t(w) * h, px); }

void fill(std::vector<Px>& px, std::uint32_t w, const Rect& r, Px v) {
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) px[std::size_t(y) * w + std::size_t(x)] = v;
}

const render::fx::Kernel& kernel(const std::string& kind) {
    const render::fx::Kernel* k = render::fx::kernel(kind);
    if (k == nullptr) throw core::Error("value", "no kernel " + kind);
    return *k;
}

// The kind with its settings in their normal form (defaults filled; 置き換え's map: none — the test gives it).
Json normal(const std::string& kind, const Json& given) {
    Json params = given;
    if (kind == "distort_displace" && !params.contains("map") && !params.contains("map_data")) params["map"] = Json{{"layer", "merged"}};
    return core::normalize_filter(kind, params, nullptr);
}

struct Run {
    render::fx::Tiling tiling{};
    double s = 1;
    const Rect* bounds = nullptr;
    const std::string* cover = nullptr;
    const render::fx::Picture* map = nullptr;
};

// A kind on a precise raster of w × h (its new bytes).
std::string run_precise(const std::string& raster, const std::string& kind, const Json& given, const Run& how = {}) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::Reads reads;
    reads.map = how.map;
    render::fx::apply(*surface, kernel(kind), normal(kind, given), core::FilterInks::Plates, how.bounds != nullptr ? *how.bounds : grid, how.cover, how.s,
                      nullptr, how.tiling, reads);
    return render::fx::take_bytes(*surface);
}

std::array<double, 4> pixel(const std::string& raster, std::uint32_t x, std::uint32_t y) {
    const core::ColorRasterView view(raster);
    return view.pixel(std::size_t(y) * view.width() + x);
}

double variance(const std::string& raster, const Rect& r, unsigned c) {
    std::vector<double> v;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) v.push_back(pixel(raster, std::uint32_t(x), std::uint32_t(y))[c]);
    double m = 0, var = 0;
    for (const double e : v) m += e / double(v.size());
    for (const double e : v) var += (e - m) * (e - m) / double(v.size());
    return var;
}

// An 8-bit picture of PhotoCraft's test pattern, run through a kind (its new bytes).
std::string eight_bytes(const render::Image& picture) { return picture.tobytes(); }
render::Image eight_pattern(int w, int h) {
    std::string bytes;
    for (const Px& p : pattern(std::uint32_t(w), std::uint32_t(h)))
        for (const float v : p) bytes.push_back(static_cast<char>(std::lround(v * 255.0f)));
    return render::Image::frombytes("RGBA", render::Size{w, h}, bytes);
}
std::string run_eight(const render::Image& picture, const std::string& kind, const Json& given, const render::fx::Picture* map = nullptr) {
    const auto surface = render::fx::eight_surface(picture);
    render::fx::Reads reads;
    reads.map = map;
    render::fx::apply(*surface, kernel(kind), normal(kind, given), core::FilterInks::Plates, Rect{0, 0, picture.width(), picture.height()}, nullptr, 1, nullptr, {},
                      reads);
    return render::fx::take_image(*surface).tobytes();
}

// 置き換え's map of one colour (w × h).
render::fx::Picture map_of(std::array<int, 4> rgba, int w = 8, int h = 8) {
    render::fx::Picture out;
    out.width = w;
    out.height = h;
    for (int i = 0; i < w * h; ++i)
        for (const int v : rgba) out.rgba.push_back(static_cast<char>(v));
    return out;
}

core::ApplyResult apply_ops(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")); }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        apply_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(double side_mm = kInch, double dpi = 200) {
    auto doc = core::new_episode("フィルター", core::Num(1), 1, core::PageSpec::custom(side_mm, side_mm, 20, 20, 1, 2, 2, 2, 2, dpi, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const char* id, std::size_t page = 0) {
    for (const auto& layer : doc.page(page).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", std::string("no layer ") + id);
}

// A page of one inch at 200 dpi with an 8-bit paint layer "paint" of `picture` (200 × 200) and, when given, a paint layer
// "map" of `map` over the page.
core::Document eight_page(const render::Image& picture, const std::optional<render::Image>& map = std::nullopt) {
    auto doc = blank();
    for (const auto& [id, image] : {std::pair{"paint", std::optional<render::Image>(picture)}, std::pair{"map", map}}) {
        if (!image) continue;
        core::Layer layer;
        layer.id = id;
        layer.kind = core::LayerKind::Raster;
        layer.panel_clip = false;
        render::raster::save_raster(doc.page(0), layer, *image);
        doc.edit_page(0).layers.push_back(layer);
    }
    return doc;
}

render::Image layer_picture(const core::Document& doc, const char* id) { return render::raster::layer_pixels(doc.page(0), layer_of(doc, id)).convert("RGBA"); }

// A precise page of `side_mm` with one paint layer "paint" (u16) of w × h.
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, double side_mm = kInch) {
    auto doc = blank(side_mm);
    Json pixels = Json::array();
    for (const auto& p : px)
        for (const float v : p) pixels.push_back(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535)));
    doc = apply_ops(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h}, {"precision", "u16"}, {"pixels", pixels}}})).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

// A page of inks (cmyk, lab, gray or multichannel) with a paint layer "pic" of side × side: `ink(x, y, c)` its values.
core::Document ink_page(const std::string& mode, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink, std::uint32_t side = 64) {
    Json set{{"op", "set_color_mode"}, {"mode", mode}, {"depth", mode == "multichannel" ? 8 : 16}};
    if (mode == "cmyk") set["profile"] = "coated-cmyk";
    auto doc = apply_ops(blank(), Json::array({set, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
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

core::InkPixels inks_of(const core::Document& doc) { return core::decode_ink_raster(*layer_of(doc, "pic").ink_raster); }

std::string png_b64(const render::Image& image) { return core::b64encode(render::write_png(image)); }

// The D2 kinds and settings (PhotoCraft's tests' settings, tests.rs 37-60, tests_ext.rs 44-66).
std::vector<std::pair<std::string, Json>> d2_kinds() {
    return {
        {"distort_displace", Json{{"horizontal", 10}, {"vertical", 10}, {"fit", "stretch"}, {"undefined_areas", "wrap"}}},
        {"distort_pinch", Json{{"amount", 50}}},
        {"distort_polar_coordinates", Json{{"mode", "rectangular_to_polar"}}},
        {"distort_polar_coordinates", Json{{"mode", "polar_to_rectangular"}}},
        {"distort_ripple", Json{{"amount", 200}, {"size", "small"}}},
        {"distort_shear", Json{{"points", Json::array({Json::array({0, 0}), Json::array({0.5, 0.3}), Json::array({1, 0})})}, {"undefined_areas", "repeat"}}},
        {"distort_spherize", Json{{"amount", -60}, {"mode", "horizontal_only"}}},
        {"distort_twirl", Json{{"angle", 90}}},
        {"distort_wave", Json{{"generators", 3}, {"wavelength_min", 5}, {"wavelength_max", 20}, {"amplitude_min", 1}, {"amplitude_max", 4},
                              {"type", "triangle"}, {"undefined_areas", "repeat"}, {"seed", 9}}},
        {"distort_zigzag", Json{{"amount", 50}, {"ridges", 4}, {"style", "pond_ripples"}}},
        {"pixelate_color_halftone", Json{{"max_radius", 4}}},
        {"pixelate_crystallize", Json{{"cell_size", 6}, {"seed", 1}}},
        {"pixelate_facet", Json::object()},
        {"pixelate_fragment", Json::object()},
        {"pixelate_mezzotint", Json{{"type", "medium_lines"}, {"seed", 2}}},
        {"pixelate_mezzotint", Json{{"type", "grainy_dots"}, {"seed", 2}}},
        {"pixelate_mosaic", Json{{"cell_size", 6}}},
        {"pixelate_pointillize", Json{{"cell_size", 5}, {"seed", 3}}},
        {"stylize_diffuse", Json{{"mode", "normal"}, {"seed", 4}}},
        {"stylize_diffuse", Json{{"mode", "anisotropic"}, {"seed", 4}}},
        {"stylize_emboss", Json{{"angle", 135}, {"height", 2}, {"amount", 100}}},
        {"stylize_extrude", Json{{"type", "blocks"}, {"size", 6}, {"depth", 20}, {"seed", 5}}},
        {"stylize_extrude", Json{{"type", "pyramids"}, {"size", 6}, {"depth", 30}, {"depth_mode", "level_based"}, {"mask_incomplete_blocks", true}, {"seed", 5}}},
        {"stylize_find_edges", Json::object()},
        {"stylize_oil_paint", Json{{"stylization", 3}, {"cleanliness", 4}, {"scale", 1}, {"bristle_detail", 5}, {"shine", 2}}},
        {"stylize_solarize", Json::object()},
        {"stylize_tiles", Json{{"count", 4}, {"max_offset", 20}, {"seed", 6}}},
        {"stylize_trace_contour", Json{{"level", 128}, {"edge", "upper"}}},
        {"stylize_wind", Json{{"method", "blast"}, {"direction", "from_left"}, {"seed", 7}}},
        {"stylize_wind", Json{{"method", "stagger"}, {"direction", "from_right"}, {"seed", 7}}},
    };
}

}  // namespace

class TestFxD2 : public QObject {
    Q_OBJECT

private slots:
    // --- the table ---------------------------------------------------------------------------------------------------------

    void theKindsOfD2() {
        // 25 kinds (9 + 7 + 9) in their submenus, PhotoCraft's commands, each a smart filter, how a page of inks takes it
        std::map<std::string, int> per;
        for (const core::FilterInfo& info : core::filter_infos()) {
            const std::string kind(info.kind);
            if (!kind.starts_with("distort_") && !kind.starts_with("pixelate_") && !kind.starts_with("stylize_")) continue;
            ++per[std::string(info.submenu)];
            QVERIFY2(std::string(info.upstream).starts_with("filter." + kind.substr(0, kind.find('_')) + "."), kind.c_str());
            QVERIFY2(core::smart::filter_kind(kind), kind.c_str());
            QVERIFY2(render::fx::kernel(kind) != nullptr, kind.c_str());
        }
        QCOMPARE(per["変形"], 9);
        QCOMPARE(per["ピクセレート"], 7);
        QCOMPARE(per["表現手法"], 9);
        QVERIFY(core::filter_info("distort_twirl")->inks == core::FilterInks::Plates);
        QVERIFY(core::filter_info("pixelate_color_halftone")->inks == core::FilterInks::Halftone);
        QVERIFY(core::filter_info("pixelate_mezzotint")->inks == core::FilterInks::Halftone);
        QVERIFY(core::filter_info("pixelate_pointillize")->inks == core::FilterInks::Meaning);
        QVERIFY(core::filter_info("stylize_emboss")->inks == core::FilterInks::LightnessAndColour);
        QVERIFY(core::filter_info("stylize_tiles")->inks == core::FilterInks::Meaning);
        QVERIFY(core::filter_info("distort_displace")->files == (std::vector<std::string_view>{"map"}));
        QVERIFY(core::filter_info("stylize_tiles")->colours == (std::vector<std::string_view>{"foreground", "background"}));
        QVERIFY(core::filter_info("pixelate_pointillize")->colours == (std::vector<std::string_view>{"background"}));
        // PhotoCraft's defaults and ranges (filters.rs, filters_ext.rs), Genko's names
        QCOMPARE(core::normalize_filter("distort_twirl", Json::object(), nullptr), (Json{{"angle", 50}}));
        QCOMPARE(core::normalize_filter("distort_twirl", Json{{"angle", 5000}}, nullptr), (Json{{"angle", 999}}));
        QCOMPARE(core::normalize_filter("distort_ripple", Json::object(), nullptr), (Json{{"amount", 100}, {"size", "medium"}}));
        QCOMPARE(core::normalize_filter("pixelate_mosaic", Json{{"cell_size", 1}}, nullptr), (Json{{"cell_size", 2}}));
        QCOMPARE(core::normalize_filter("stylize_wind", Json::object(), nullptr), (Json{{"method", "wind"}, {"direction", "from_right"}, {"seed", 0}}));
        QCOMPARE(core::normalize_filter("pixelate_pointillize", Json::object(), nullptr)["background"], (Json::array({255, 255, 255})));
        // シアー's curve: sorted, clamped, the second of two at one place left out; none: the straight line; refused past 64
        QCOMPARE(core::normalize_filter("distort_shear", Json{{"points", Json::array({Json::array({1, 2}), Json::array({0, -0.5}), Json::array({1, 0})})}}, nullptr)["points"],
                 (Json::array({Json::array({0, -0.5}), Json::array({1, 1})})));
        QCOMPARE(core::normalize_filter("distort_shear", Json::object(), nullptr)["points"], (Json::array({Json::array({0, 0}), Json::array({1, 0})})));
        Json many = Json::array();
        for (int i = 0; i < 65; ++i) many.push_back(Json::array({i / 64.0, 0}));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_shear", Json{{"points", many}}, nullptr));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_shear", Json{{"points", Json::array({Json::array({0, 0})})}}, nullptr));
        // 置き換え's map: required; a file's data (a PNG or PSD) its ref and suffix added; a layer; a file the book holds
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_displace", Json::object(), nullptr));
        const std::string png = render::write_png(render::Image::create("RGBA", render::Size{2, 2}, render::Ink{200, 128, 128, 255}));
        const Json by_data = core::normalize_filter("distort_displace", Json{{"map_data", core::b64encode(png)}}, nullptr);
        QCOMPARE(by_data["map"], Json(core::bytes_ref(png)));
        QCOMPARE(by_data["map_suffix"], Json(".png"));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_displace", Json{{"map_data", core::b64encode("not a picture")}}, nullptr));
        QCOMPARE(core::normalize_filter("distort_displace", Json{{"map", Json{{"layer", "a"}, {"page", 2}}}, {"map_suffix", ".png"}}, nullptr),
                 (Json{{"horizontal", 10}, {"vertical", 10}, {"fit", "stretch"}, {"undefined_areas", "repeat"}, {"map", Json{{"page", 2}, {"layer", "a"}}}}));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_displace", Json{{"map", Json{{"layer", "a"}, {"name", "x"}}}}, nullptr));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_filter("distort_displace", Json{{"map", "sha256:" + std::string(64, 'a')}}, nullptr));
        QCOMPARE(core::normalize_filter("distort_displace", Json{{"map", "sha256:" + std::string(64, 'a')}, {"map_suffix", ".psd"}}, nullptr)["map_suffix"], Json(".psd"));
    }

    // --- PhotoCraft's tests (tests.rs 335-378) ------------------------------------------------------------------------------

    void distortionsAreIdentityAtZero() {
        const render::Image s = eight_pattern(40, 30);
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{{"distort_twirl", Json{{"angle", 0}}},
                                                                                    {"distort_pinch", Json{{"amount", 0}}},
                                                                                    {"distort_spherize", Json{{"amount", 0}, {"mode", "normal"}}},
                                                                                    {"distort_ripple", Json{{"amount", 0}, {"size", "large"}}}})
            QVERIFY2(run_eight(s, kind, params) == eight_bytes(s), kind.c_str());
        // (tests_ext.rs 221: identities at zero strength — 置き換え at 0 % and without a map, a straight シアー, ジグザグ at 0)
        const render::fx::Picture strong = map_of({230, 26, 0, 255});
        QVERIFY(run_eight(s, "distort_displace", Json{{"horizontal", 0}, {"vertical", 0}}, &strong) == eight_bytes(s));
        QVERIFY(run_eight(s, "distort_displace", Json{{"horizontal", 50}, {"vertical", 50}}) == eight_bytes(s));
        QVERIFY(run_eight(s, "distort_shear", Json{{"points", Json::array({Json::array({0, 0}), Json::array({1, 0})})}}) == eight_bytes(s));
        QVERIFY(run_eight(s, "distort_zigzag", Json{{"amount", 0}, {"ridges", 5}, {"style", "around_center"}}) == eight_bytes(s));
    }

    void twirlKeepsCentreAndOutside() {
        const std::string s = precise(40, 40, pattern(40, 40));
        const std::string out = run_precise(s, "distort_twirl", Json{{"angle", 180}});
        QCOMPARE(pixel(out, 0, 0), pixel(s, 0, 0));  // (the corner is outside the twirl's circle)
        QVERIFY(pixel(out, 20, 12) != pixel(s, 20, 12));
    }

    void mosaicCellsAreUniform() {
        const std::string m = run_precise(precise(40, 30, pattern(40, 30)), "pixelate_mosaic", Json{{"cell_size", 5}});
        QCOMPARE(pixel(m, 0, 0), pixel(m, 4, 4));
        QVERIFY(pixel(m, 4, 4) != pixel(m, 5, 4));
    }

    void stylizeBasics() {
        const std::string s = precise(40, 30, flat(40, 30, {0.8f, 0.3f, 0.1f, 1.0f}));
        const auto e = pixel(run_precise(s, "stylize_emboss", Json{{"angle", 45}, {"height", 3}, {"amount", 100}}), 20, 15);
        QVERIFY(std::fabs(e[0] - 0.5) < 1e-5 && std::fabs(e[1] - 0.5) < 1e-5);
        QVERIFY(std::fabs(pixel(run_precise(s, "stylize_find_edges", Json::object()), 20, 15)[0] - 1.0) < 1e-6);
        const auto so = pixel(run_precise(s, "stylize_solarize", Json::object()), 3, 3);
        QVERIFY(std::fabs(so[0] - 0.2) < 1e-6 && std::fabs(so[1] - 0.3) < 1e-6);
    }

    // --- PhotoCraft's tests (tests_ext.rs) ----------------------------------------------------------------------------------

    void flatImagesStayFlat() {
        const Px c{0.3f, 0.55f, 0.8f, 1.0f};
        const std::string s = precise(40, 30, flat(40, 30, c));
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{
                 {"pixelate_crystallize", Json{{"cell_size", 6}, {"seed", 1}}},
                 {"pixelate_facet", Json::object()},
                 {"stylize_oil_paint", Json{{"stylization", 3}, {"cleanliness", 4}, {"scale", 1}, {"bristle_detail", 0}, {"lighting", true}, {"shine", 2}}},
                 {"stylize_wind", Json{{"method", "blast"}, {"direction", "from_left"}}}}) {
            const auto px = pixel(run_precise(s, kind, params), 20, 15);
            for (unsigned k = 0; k < 4; ++k) QVERIFY2(std::fabs(px[k] - c[k]) < 2e-3, kind.c_str());
        }
    }

    void mezzotintIsBinaryAndTracksBrightness() {
        const std::string out = run_precise(precise(40, 30, flat(40, 30, {0.25f, 0.5f, 0.75f, 1.0f})), "pixelate_mezzotint", Json{{"type", "fine_dots"}, {"seed", 1}});
        std::array<double, 3> mean{};
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto p = pixel(out, x, y);
                for (unsigned k = 0; k < 3; ++k) {
                    QVERIFY(p[k] == 0.0 || p[k] == 1.0);
                    mean[k] += p[k] / 1200.0;
                }
            }
        QVERIFY2(std::fabs(mean[0] - 0.25) < 0.06 && std::fabs(mean[1] - 0.5) < 0.06 && std::fabs(mean[2] - 0.75) < 0.06,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(mean[0]).arg(mean[1]).arg(mean[2])));
    }

    void halftoneCoversByInk() {
        const Json params{{"max_radius", 4}};
        const std::string w = run_precise(precise(40, 30, flat(40, 30, {1, 1, 1, 1})), "pixelate_color_halftone", params);
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x)
                for (unsigned k = 0; k < 4; ++k) QVERIFY(std::fabs(pixel(w, x, y)[k] - 1.0) < 1e-6);  // (white paper stays white)
        const auto mean = [](const std::string& r, unsigned k) {
            double m = 0;
            for (std::uint32_t y = 0; y < 30; ++y)
                for (std::uint32_t x = 0; x < 40; ++x) m += pixel(r, x, y)[k] / 1200.0;
            return m;
        };
        QVERIFY(mean(run_precise(precise(40, 30, flat(40, 30, {0, 0, 0, 1})), "pixelate_color_halftone", params), 0) < 0.05);  // (black fully inked)
        QVERIFY(std::fabs(mean(run_precise(precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1})), "pixelate_color_halftone", params), 1) - 0.5) < 0.12);
    }

    void pointillizeShowsCanvas() {
        const std::string out =
            run_precise(precise(40, 30, flat(40, 30, {0, 0, 1, 1})), "pixelate_pointillize", Json{{"cell_size", 10}, {"seed", 3}, {"background", Json::array({255, 0, 0})}});
        bool canvas = false, dots = false;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto p = pixel(out, x, y);
                canvas = canvas || (p[0] > 0.99 && p[2] < 0.01);
                dots = dots || p[2] > 0.9;
            }
        QVERIFY(canvas);
        QVERIFY(dots);
    }

    void diffuseOnlyMovesPixelsLocally() {
        const std::string s = precise(40, 30, pattern(40, 30));
        const std::string out = run_precise(s, "stylize_diffuse", Json{{"mode", "normal"}, {"seed", 1}});
        for (std::uint32_t y = 1; y < 29; ++y)
            for (std::uint32_t x = 1; x < 39; ++x) {
                const auto p = pixel(out, x, y);
                bool found = false;
                for (int dy = -1; dy <= 1 && !found; ++dy)
                    for (int dx = -1; dx <= 1 && !found; ++dx) found = pixel(s, std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy)) == p;
                QVERIFY2(found, qPrintable(QStringLiteral("(%1, %2)").arg(x).arg(y)));
            }
        const std::string dark = run_precise(s, "stylize_diffuse", Json{{"mode", "darken_only"}, {"seed", 1}});
        const auto l = [](const std::array<double, 4>& p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; };
        for (std::uint32_t y = 1; y < 29; ++y)
            for (std::uint32_t x = 1; x < 39; ++x) QVERIFY(l(pixel(dark, x, y)) <= l(pixel(s, x, y)) + 1e-6);
    }

    void traceContourDrawsLineAtLevelCrossing() {
        std::vector<Px> px = flat(40, 30, {0.2f, 0.2f, 0.2f, 1});
        fill(px, 40, Rect{20, 0, 40, 30}, {0.8f, 0.8f, 0.8f, 1});
        const std::string s = precise(40, 30, px);
        const std::string up = run_precise(s, "stylize_trace_contour", Json{{"level", 128}, {"edge", "upper"}});
        QCOMPARE(pixel(up, 20, 10)[0], 0.0);  // (the line on the bright side)
        QCOMPARE(pixel(up, 19, 10)[0], 1.0);
        QCOMPARE(pixel(up, 30, 10)[0], 1.0);
        QCOMPARE(pixel(run_precise(s, "stylize_trace_contour", Json{{"level", 128}, {"edge", "lower"}}), 19, 10)[0], 0.0);  // (on the dark side)
    }

    void tilesLeaveGapsWithFill() {
        const std::string out = run_precise(precise(40, 30, flat(40, 30, {0, 0, 1, 1})), "stylize_tiles",
                                            Json{{"count", 3}, {"max_offset", 40}, {"fill", "foreground"}, {"foreground", Json::array({0, 255, 0})}, {"seed", 2}});
        int green = 0, blue = 0;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto p = pixel(out, x, y);
                green += p[1] > 0.99;
                blue += p[2] > 0.99;
            }
        QVERIFY(green > 0);   // (the foreground in the gaps)
        QVERIFY(blue > 600);  // (mostly tiles)
    }

    void extrudeBlocksAndPyramidsShadeCells() {
        const std::string blocks = run_precise(precise(40, 30, pattern(40, 30)), "stylize_extrude",
                                               Json{{"type", "blocks"}, {"size", 8}, {"depth", 40}, {"solid_front_faces", true}, {"seed", 1}});
        bool same = false;  // (a solid front face is one colour: two equal neighbours in the middle)
        for (std::uint32_t x = 5; x < 35 && !same; ++x) same = pixel(blocks, x, 12) == pixel(blocks, x + 1, 12);
        QVERIFY(same);
        const std::string pyr = run_precise(precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1})), "stylize_extrude",
                                            Json{{"type", "pyramids"}, {"size", 10}, {"depth", 100}, {"seed", 1}});
        QVERIFY(pixel(pyr, 11, 15)[0] > pixel(pyr, 18, 15)[0]);  // (the light from the top left: the left face brighter)
    }

    void displaceShiftsByMapAndShearByCurve() {
        const std::string s = precise(40, 30, pattern(40, 30));
        // (a map's red at full — 255 — moves a pixel across by the whole of its scale: 2.34375 % of 128 = 3 pixels; green at
        // the middle, none down)
        const render::fx::Picture map = map_of({255, 128, 128, 255});
        Run how;
        how.map = &map;
        const std::string out = run_precise(s, "distort_displace", Json{{"horizontal", 2.34375}, {"vertical", 0}, {"fit", "tile"}, {"undefined_areas", "wrap"}}, how);
        for (std::uint32_t x = 0; x < 30; ++x) QVERIFY2(pixel(out, x, 7) == pixel(s, x + 3, 7), qPrintable(QString::number(x)));
        const std::string sh = run_precise(s, "distort_shear", Json{{"points", Json::array({Json::array({0, 0.1}), Json::array({1, 0.1})})}, {"undefined_areas", "wrap"}});
        QCOMPARE(pixel(sh, 10, 4), pixel(s, 8, 4));
    }

    void windStreaksDownwindFromEdges() {
        std::vector<Px> px = flat(40, 30, {0, 0, 0, 1});
        fill(px, 40, Rect{10, 0, 12, 30}, {1, 1, 1, 1});
        const std::string right = run_precise(precise(40, 30, px), "stylize_wind", Json{{"method", "blast"}, {"direction", "from_left"}, {"seed", 1}});
        double left_side = 0, right_side = 0;
        for (std::uint32_t y = 0; y < 30; ++y) {
            left_side += pixel(right, 8, y)[0];
            right_side += pixel(right, 14, y)[0];
        }
        QVERIFY2(right_side > left_side + 0.5, qPrintable(QStringLiteral("%1 %2").arg(left_side).arg(right_side)));
    }

    void oilPaintSmoothsAlongStrokes() {
        const std::string noisy = run_precise(precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1})), "noise_add",
                                              Json{{"amount", 20}, {"distribution", "gaussian"}, {"monochromatic", false}, {"seed", 4}});
        const std::string out = run_precise(noisy, "stylize_oil_paint",
                                            Json{{"stylization", 5}, {"cleanliness", 5}, {"scale", 2}, {"bristle_detail", 0}, {"lighting", false}, {"shine", 0}});
        const Rect inner{8, 8, 32, 22};
        QVERIFY(variance(out, inner, 0) < variance(noisy, inner, 0) * 0.5);
    }

    // --- Genko's own -------------------------------------------------------------------------------------------------------

    // Each kind the same bytes whatever the tiling and the threads, at 200 dpi and a finer grid's scales (s 1.7, 3: its
    // lengths, patterns and windows), and with a reach past a tile's window (the whole base box read once).
    void everyKindIsTileAndThreadIndependent() {
        const std::string raster = precise(48, 36, pattern(48, 36));
        const render::fx::Picture map = map_of({204, 77, 128, 255}, 5, 3);
        for (const auto& [kind, params] : d2_kinds()) {
            for (const double s : {1.0, 1.7, 3.0}) {
                Run a, b, c;
                a.s = b.s = c.s = s;
                a.map = b.map = c.map = &map;
                a.tiling = {256, 1};
                b.tiling = {7, 3};
                c.tiling = {13, 2};
                const std::string one = run_precise(raster, kind, params, a);
                QVERIFY2(one == run_precise(raster, kind, params, b), qPrintable(QStringLiteral("%1 %2 at %3").arg(QString::fromStdString(kind), QString::fromStdString(params.dump())).arg(s)));
                QVERIFY2(one == run_precise(raster, kind, params, c), qPrintable(QStringLiteral("%1 at %2").arg(QString::fromStdString(kind)).arg(s)));
                QVERIFY2(one != raster || kind == "stylize_solarize", qPrintable(QStringLiteral("%1 did nothing at %2").arg(QString::fromStdString(kind)).arg(s)));
            }
        }
        // (a reach past a tile's window — cells of 300, blocks of 255 at a scale of 3 —: the whole base box read once)
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{{"pixelate_crystallize", Json{{"cell_size", 300}, {"seed", 2}}},
                                                                                    {"stylize_extrude", Json{{"size", 255}, {"depth", 255}, {"seed", 2}}},
                                                                                    {"pixelate_color_halftone", Json{{"max_radius", 127}}}}) {
            QVERIFY(kernel(kind).halo(normal(kind, params), [] {
                         render::fx::Ctx ctx;
                         ctx.s = 3;
                         return ctx;
                     }())
                        .bounds);
            Run a, b;
            a.s = b.s = 3;
            a.tiling = {256, 1};
            b.tiling = {9, 4};
            QVERIFY2(run_precise(raster, kind, params, a) == run_precise(raster, kind, params, b), kind.c_str());
        }
    }

    // A part of the grid worked alone (フィルターギャラリー's way, fx::PlaceScope: its place on the grid and the whole's base
    // box) has the whole's pixels there, away from its edges: the patterns and grains are of the grid's places, the cells
    // and screens anchored at the base box.
    void aPartWorkedAloneIsTheWholeThere() {
        const std::string whole = precise(128, 96, pattern(128, 96));
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{
                 {"pixelate_mezzotint", Json{{"type", "medium_lines"}, {"seed", 2}}},
                 {"pixelate_crystallize", Json{{"cell_size", 4}, {"seed", 1}}},
                 {"pixelate_mosaic", Json{{"cell_size", 6}}},
                 {"pixelate_color_halftone", Json{{"max_radius", 4}}},
                 {"pixelate_pointillize", Json{{"cell_size", 5}, {"seed", 3}}},
                 {"stylize_diffuse", Json{{"seed", 4}}},
                 {"stylize_wind", Json{{"method", "stagger"}, {"seed", 7}}},
                 {"stylize_tiles", Json{{"count", 5}, {"seed", 2}}}}) {
            for (const double s : {1.0, 1.5}) {
                Run all;
                all.s = s;
                const std::string full = run_precise(whole, kind, params, all);
                // (the part 32..96 × 24..72 of the grid)
                std::vector<Px> part;
                for (std::uint32_t y = 24; y < 72; ++y)
                    for (std::uint32_t x = 32; x < 96; ++x) {
                        const auto p = pixel(whole, x, y);
                        part.push_back({float(p[0]), float(p[1]), float(p[2]), float(p[3])});
                    }
                std::string alone;
                {
                    const render::fx::PlaceScope place(32, 24, Rect{0, 0, 128, 96});
                    Run here;
                    here.s = s;
                    alone = run_precise(precise(64, 48, part), kind, params, here);
                }
                const std::uint32_t margin = 20;  // (past what each reaches at 1.5: its pixels read inside the part)
                for (std::uint32_t y = margin; y < 48 - margin; ++y)
                    for (std::uint32_t x = margin; x < 64 - margin; ++x)
                        QVERIFY2(pixel(alone, x, y) == pixel(full, x + 32, y + 24),
                                 qPrintable(QStringLiteral("%1 at %2: (%3, %4)").arg(QString::fromStdString(kind)).arg(s).arg(x).arg(y)));
            }
        }
    }

    // A clear pixel's colour never comes in (D3's review P2-1): a red disc on clear pixels of black — every pixel that shows
    // is red after the kinds that average, move or mix pixels.
    void clearPixelsBringNoColour() {
        std::vector<Px> px = flat(48, 36, {0, 0, 0, 0});
        for (std::uint32_t y = 0; y < 36; ++y)
            for (std::uint32_t x = 0; x < 48; ++x)
                if ((x - 24.0) * (x - 24.0) + (y - 18.0) * (y - 18.0) < 100) px[std::size_t(y) * 48 + x] = {1, 0, 0, 1};
        const std::string disc = precise(48, 36, px);
        const render::fx::Picture map = map_of({220, 40, 128, 255});
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{
                 {"pixelate_mosaic", Json{{"cell_size", 5}}},
                 {"pixelate_crystallize", Json{{"cell_size", 5}, {"seed", 1}}},
                 {"pixelate_fragment", Json::object()},
                 {"pixelate_facet", Json::object()},
                 {"pixelate_pointillize", Json{{"cell_size", 4}, {"background", Json::array({255, 0, 0})}}},
                 {"stylize_oil_paint", Json{{"lighting", false}}},
                 {"stylize_diffuse", Json{{"mode", "anisotropic"}}},
                 {"stylize_wind", Json{{"method", "blast"}}},
                 {"distort_twirl", Json{{"angle", 120}}},
                 {"distort_wave", Json{{"generators", 3}}},
                 {"distort_displace", Json{{"horizontal", 20}, {"vertical", 20}}}}) {
            Run how;
            how.map = &map;
            const std::string out = run_precise(disc, kind, params, how);
            for (std::uint32_t y = 0; y < 36; ++y)
                for (std::uint32_t x = 0; x < 48; ++x) {
                    const auto p = pixel(out, x, y);
                    if (p[3] < 0.01) continue;
                    const double jitter = kind == "pixelate_pointillize" ? 0.041 : 0.01;  // (点描's dots: ±4 % of their colour, PhotoCraft's)
                    QVERIFY2(p[0] > 1.0 - jitter && p[1] < jitter && p[2] < jitter,
                             qPrintable(QStringLiteral("%1 (%2, %3): %4 %5 %6 %7").arg(QString::fromStdString(kind)).arg(x).arg(y).arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3])));
                }
        }
    }

    // A distortion is centred on the base box — a selection's box —: inside it as the kind on that box alone, outside it as it
    // was; through the op too (filter_raster with an area).
    void aDistortionIsCentredOnTheSelection() {
        const std::string whole = precise(48, 36, pattern(48, 36));
        const Rect box{10, 6, 34, 30};
        std::string cover(48 * 36, '\0');
        for (int y = box.y0; y < box.y1; ++y)
            for (int x = box.x0; x < box.x1; ++x) cover[std::size_t(y) * 48 + std::size_t(x)] = static_cast<char>(255);
        std::vector<Px> part;
        for (int y = box.y0; y < box.y1; ++y)
            for (int x = box.x0; x < box.x1; ++x) {
                const auto p = pixel(whole, std::uint32_t(x), std::uint32_t(y));
                part.push_back({float(p[0]), float(p[1]), float(p[2]), float(p[3])});
            }
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{{"distort_twirl", Json{{"angle", 140}}},
                                                                                    {"distort_pinch", Json{{"amount", 80}}},
                                                                                    {"distort_spherize", Json{{"amount", 70}}},
                                                                                    {"distort_polar_coordinates", Json::object()},
                                                                                    {"distort_zigzag", Json{{"amount", 60}}}}) {
            Run inside;
            inside.bounds = &box;
            inside.cover = &cover;
            const std::string got = run_precise(whole, kind, params, inside);
            const std::string alone = run_precise(precise(std::uint32_t(box.width()), std::uint32_t(box.height()), part), kind, params);
            for (std::uint32_t y = 0; y < 36; ++y)
                for (std::uint32_t x = 0; x < 48; ++x) {
                    const bool in = box.contains(int(x), int(y));
                    const auto want = in ? pixel(alone, x - std::uint32_t(box.x0), y - std::uint32_t(box.y0)) : pixel(whole, x, y);
                    const auto have = pixel(got, x, y);
                    double apart = 0;  // (inside: the same within float's rounding of the box's place; outside: the same)
                    for (unsigned c = 0; c < 4; ++c) apart = std::max(apart, std::fabs(have[c] - want[c]));
                    QVERIFY2(in ? apart < 1e-4 : apart == 0.0, qPrintable(QStringLiteral("%1 (%2, %3): %4").arg(QString::fromStdString(kind)).arg(x).arg(y).arg(apart)));
                }
        }
        // the op: 渦巻き inside a selection on an 8-bit layer — outside it as it was, inside it changed
        const auto doc = eight_page(eight_pattern(200, 200));
        const auto done = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "distort_twirl"}, {"angle", 200},
                                                           {"area", Json{{"rect", Json::array({kInch / 4, kInch / 4, kInch / 2, kInch / 2})}}}}}))
                              .doc;
        const render::Image was = layer_picture(doc, "paint"), now = layer_picture(done, "paint");
        QCOMPARE(now.getpixel(20, 20), was.getpixel(20, 20));
        QCOMPARE(now.getpixel(180, 180), was.getpixel(180, 180));
        QVERIFY(now.getpixel(100, 80) != was.getpixel(100, 80));
    }

    // カラーハーフトーン on a CMYK page: each of the four plates its own screen (its angle), its dots' area its ink's.
    void colourHalftoneScreensTheFourPlates() {
        const std::array<double, 4> ink{0.3, 0.5, 0.7, 0.2};
        const auto doc = ink_page("cmyk", [&](std::uint32_t, std::uint32_t, unsigned c) { return ink[c]; }, 96);
        const auto done = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "pixelate_color_halftone"}, {"max_radius", 16}}})).doc;
        const core::InkPixels now = inks_of(done);
        std::array<std::vector<double>, 4> plates;
        for (std::size_t i = 0; i < now.pixel_count(); ++i)
            for (unsigned c = 0; c < 4; ++c) plates[c].push_back(now.value(i, c));
        for (unsigned c = 0; c < 4; ++c) {
            double mean = 0;
            int solid = 0, none = 0;
            for (const double v : plates[c]) {
                mean += v / double(plates[c].size());
                solid += v > 0.99;
                none += v < 0.01;
            }
            QVERIFY2(std::fabs(mean - ink[c]) < 0.1, qPrintable(QStringLiteral("plate %1: %2").arg(c).arg(mean)));  // (its ink)
            QVERIFY2(solid > 0 && none > 0, qPrintable(QString::number(c)));                                       // (dots, not a tint)
            for (unsigned d = 0; d < c; ++d) QVERIFY2(plates[c] != plates[d], qPrintable(QStringLiteral("%1 %2").arg(c).arg(d)));  // (its own screen)
        }
        // a grey page: one screen; a Lab page: the colours it means, screened (a, b greyed or kept as the rules give back)
        for (const std::string mode : {"gray", "lab"}) {
            const auto other = ink_page(mode, [](std::uint32_t, std::uint32_t, unsigned c) { return c == 0 ? 0.5 : 0.5; });
            QVERIFY2(refusal(other, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "pixelate_color_halftone"}}).empty(), mode.c_str());
        }
    }

    // A page of inks by its lightness (design 4.1): on CMYK an edge is ink on the paper (no ink where flat), a relief a grey on
    // K alone, 明るさ above the middle turned as on RGB; a Lab page's relief and edges grey (a and b their middle).
    void theLightnessOfAPageOfInks() {
        // (a K 100% square on no ink)
        const auto cmyk = ink_page("cmyk", [](std::uint32_t x, std::uint32_t y, unsigned c) { return c == 3 && x >= 24 && x < 40 && y >= 24 && y < 40 ? 1.0 : 0.0; });
        const auto edges = inks_of(apply_ops(cmyk, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "stylize_find_edges"}}})).doc);
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(edges.value(5 * 64 + 5, c), 0.0);  // (flat paper: no ink)
        QVERIFY(edges.value(32 * 64 + 24, 3) > 0.5);                                  // (the edge: K)
        QCOMPARE(edges.value(32 * 64 + 32, 3), 0.0);                                  // (flat black: no edge)
        const auto relief = inks_of(apply_ops(cmyk, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "stylize_emboss"}}})).doc);
        QVERIFY(std::fabs(relief.value(5 * 64 + 5, 3) - 0.5) < 0.01);  // (flat: a middle grey, on K alone)
        for (unsigned c = 0; c < 3; ++c) QCOMPARE(relief.value(5 * 64 + 5, c), 0.0);
        const auto turned = inks_of(apply_ops(cmyk, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "stylize_solarize"}}})).doc);
        QCOMPARE(turned.value(5 * 64 + 5, 3), 1.0);  // (no ink — light 1, past the middle — turned to K 100%, as RGB's white turns black)
        QCOMPARE(turned.value(32 * 64 + 32, 3), 1.0);
        // a Lab page (its samples as stored, normalized: L 20 and 90 as 0.2 and 0.9, a 76 and b −51.5 as 0.8 and 0.3 —
        // (a + 128) / 255 —): a relief and the edges grey (a = b = 0: 128 / 255), solarized its L alone
        const auto lab = ink_page("lab", [](std::uint32_t x, std::uint32_t, unsigned c) { return c == 0 ? (x < 32 ? 0.2 : 0.9) : c == 1 ? 0.8 : 0.3; });
        for (const char* kind : {"stylize_emboss", "stylize_find_edges", "stylize_trace_contour"}) {
            const auto now = inks_of(apply_ops(lab, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", kind}}})).doc);
            for (unsigned c = 1; c < 3; ++c)
                QVERIFY2(std::fabs(now.value(10 * 64 + 10, c) - 128.0 / 255.0) < 0.002, qPrintable(QStringLiteral("%1: %2").arg(kind).arg(now.value(10 * 64 + 10, c))));
        }
        {
            const auto now = inks_of(apply_ops(lab, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "stylize_solarize"}}})).doc);
            const auto said = [&](std::size_t i, unsigned c) { return QString::number(now.value(i, c)).toUtf8(); };
            QVERIFY2(std::fabs(now.value(10 * 64 + 40, 0) - 0.1) < 0.002, said(10 * 64 + 40, 0).constData());  // (L 90 past the middle: 10)
            QVERIFY2(std::fabs(now.value(10 * 64 + 10, 0) - 0.2) < 0.002, said(10 * 64 + 10, 0).constData());  // (L 20: as it was)
            QVERIFY2(std::fabs(now.value(10 * 64 + 10, 1) - 0.8) < 0.002, said(10 * 64 + 10, 1).constData());  // (its a and b as they were)
            QVERIFY2(std::fabs(now.value(10 * 64 + 10, 2) - 0.3) < 0.002, said(10 * 64 + 10, 2).constData());
        }
        // a multichannel page and a grey one take each of them
        for (const std::string mode : {"multichannel", "gray"}) {
            const auto doc = ink_page(mode, [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 7 + y * 3 + c * 11) % 64) / 63.0; });
            for (const auto& [kind, params] : d2_kinds()) {
                if (kind == "distort_displace") continue;
                Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", kind}};
                op.update(params);
                QVERIFY2(refusal(doc, op).empty(), (kind + " " + mode + ": " + refusal(doc, op)).c_str());
            }
        }
    }

    // The same disc at 200 and 600 dpi: the 600 dpi one shrunk to a third near the 200 dpi one (its lengths, cells and
    // patterns in working pixels).
    void theSameLookAtEveryResolution() {
        const auto disc = [](std::uint32_t side) {
            std::vector<Px> px;
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const double u = (x + 0.5) / side - 0.5, v = (y + 0.5) / side - 0.5;
                    px.push_back(u * u + v * v < 0.09 ? Px{0.9f, 0.2f, 0.1f, 1.0f} : Px{0.1f, 0.3f, 0.8f, 1.0f});
                }
            return px;
        };
        const std::vector<std::pair<Json, double>> cases{
            {Json{{"kind", "pixelate_mosaic"}, {"cell_size", 8}}, 0.03},
            {Json{{"kind", "pixelate_crystallize"}, {"cell_size", 8}, {"seed", 3}}, 0.06},
            {Json{{"kind", "pixelate_mezzotint"}, {"type", "coarse_dots"}, {"seed", 3}}, 0.06},
            {Json{{"kind", "pixelate_pointillize"}, {"cell_size", 8}, {"seed", 3}}, 0.12},
            {Json{{"kind", "pixelate_fragment"}}, 0.06},
            {Json{{"kind", "stylize_emboss"}, {"height", 2}}, 0.06},
            {Json{{"kind", "stylize_find_edges"}}, 0.08},
            {Json{{"kind", "distort_twirl"}, {"angle", 120}}, 0.04},
            {Json{{"kind", "distort_ripple"}, {"amount", 200}}, 0.06},
            {Json{{"kind", "distort_wave"}, {"generators", 3}, {"seed", 2}}, 0.06},
        };
        for (const auto& [given, bound] : cases) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
            op.update(given);
            const auto low = apply_ops(precise_page(200, 200, disc(200)), Json::array({op})).doc;
            const auto high = apply_ops(precise_page(600, 600, disc(600)), Json::array({op})).doc;
            const std::string& a = *layer_of(low, "paint").color_raster;
            const std::string& b = *layer_of(high, "paint").color_raster;
            double total = 0, apart = 0;
            for (std::uint32_t y = 0; y < 200; ++y)
                for (std::uint32_t x = 0; x < 200; ++x) {
                    std::array<double, 4> mean{};
                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx) {
                            const auto p = pixel(b, x * 3 + dx, y * 3 + dy);
                            for (unsigned c = 0; c < 3; ++c) mean[c] += p[c] / 9;
                        }
                    const auto q = pixel(a, x, y);
                    double here = 0;
                    for (unsigned c = 0; c < 3; ++c) {
                        total += std::fabs(mean[c] - q[c]) / (200.0 * 200.0 * 3.0);
                        here = std::max(here, std::fabs(mean[c] - q[c]));
                    }
                    if (here > 0.25) apart += 1.0 / (200.0 * 200.0);
                }
            QVERIFY2(total < bound, qPrintable(QStringLiteral("%1: mean %2").arg(QString::fromStdString(given.dump())).arg(total)));
            QVERIFY2(apart < 0.08, qPrintable(QStringLiteral("%1: apart %2").arg(QString::fromStdString(given.dump())).arg(apart)));
        }
    }

    // 置き換え's map through the op: a file's data (a PNG), a layer of the book, its merged picture — the reply the map's file
    // and kind, not its data —; a map's clear pixels move nothing; refused: none, not a picture, a layer or a page not there.
    void theMapsOfDisplace() {
        const render::Image picture = eight_pattern(200, 200);
        render::Image right = render::Image::create("RGBA", render::Size{20, 20}, render::Ink{255, 128, 128, 255});
        const auto doc = eight_page(picture, render::Image::create("RGBA", render::Size{200, 200}, render::Ink{255, 128, 128, 255}));
        const Json base{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "distort_displace"}, {"horizontal", 2.34375}, {"vertical", 0},
                        {"undefined_areas", "wrap"}};
        // a file's data: every pixel from 3 to its right
        Json by_data = base;
        by_data["map_data"] = png_b64(right);
        const auto data_done = apply_ops(doc, Json::array({by_data}));
        const render::Image moved = layer_picture(data_done.doc, "paint");
        QCOMPARE(moved.getpixel(50, 60), picture.getpixel(53, 60));
        QCOMPARE(data_done.results[0]["filter"]["map"], Json(core::bytes_ref(render::write_png(right))));
        QCOMPARE(data_done.results[0]["filter"]["map_suffix"], Json(".png"));
        QVERIFY(!data_done.results[0]["filter"].contains("map_data"));
        // a PSD's (grey, 1 × 1, raw: its grey both ways)
        const std::string psd = std::string("8BPS\0\x01", 6) + std::string(6, '\0') + std::string("\0\x01\0\0\0\x01\0\0\0\x01\0\x08\0\x01", 14) +
                                std::string(12, '\0') + std::string("\0\0\xff", 3);
        Json by_psd = base;
        by_psd["vertical"] = 2.34375;
        by_psd["map_data"] = core::b64encode(psd);
        const auto psd_done = apply_ops(doc, Json::array({by_psd}));
        QCOMPARE(psd_done.results[0]["filter"]["map_suffix"], Json(".psd"));
        QCOMPARE(layer_picture(psd_done.doc, "paint").getpixel(50, 60), picture.getpixel(53, 63));
        // a layer of the page (the reply: that layer): the same as the data; the page's merged picture
        Json by_layer = base;
        by_layer["map"] = Json{{"layer", "map"}};
        const auto layer_done = apply_ops(doc, Json::array({by_layer}));
        QCOMPARE(layer_picture(layer_done.doc, "paint").tobytes(), moved.tobytes());
        QCOMPARE(layer_done.results[0]["filter"]["map"], (Json{{"layer", "map"}}));
        Json by_merged = base;
        by_merged["map"] = Json{{"layer", "merged"}, {"page", 1}};
        QVERIFY(layer_picture(apply_ops(doc, Json::array({by_merged})).doc, "paint").tobytes() != picture.tobytes());
        // a map's clear pixels move nothing (laid over mid-grey)
        Json clear = base;
        clear["map_data"] = png_b64(render::Image::create("RGBA", render::Size{20, 20}, render::Ink{255, 0, 0, 0}));
        QCOMPARE(layer_picture(apply_ops(doc, Json::array({clear})).doc, "paint").tobytes(), picture.tobytes());
        // tiled: the map's pixel a working pixel (a map of two columns, red then grey: every other pixel moved)
        render::Image stripes = render::Image::create("RGBA", render::Size{2, 1}, render::Ink{128, 128, 128, 255});
        stripes.paste(render::Ink{255, 128, 128, 255}, render::Box{0, 0, 1, 1});
        Json tiled = base;
        tiled["fit"] = "tile";
        tiled["map_data"] = png_b64(stripes);
        const render::Image striped = layer_picture(apply_ops(doc, Json::array({tiled})).doc, "paint");
        QCOMPARE(striped.getpixel(50, 60), picture.getpixel(53, 60));
        QCOMPARE(striped.getpixel(51, 60), picture.getpixel(51, 60));
        // refused, the book as it was
        QVERIFY(refusal(doc, base).find("map is required") != std::string::npos);
        Json junk = base;
        junk["map_data"] = core::b64encode("GIF89a…");
        QVERIFY(refusal(doc, junk).find("PNG or PSD") != std::string::npos);
        Json broken = base;
        broken["map_data"] = core::b64encode(std::string("\x89PNG\r\n\x1a\n", 8) + "cut short");
        QVERIFY2(refusal(doc, broken).find("the displacement map cannot be read") != std::string::npos, refusal(doc, broken).c_str());
        Json missing = base;
        missing["map"] = Json{{"layer", "nope"}};
        QVERIFY(refusal(doc, missing).find("the displacement map's layer nope is not on the page") != std::string::npos);
        missing["map"] = Json{{"layer", "map"}, {"page", 7}};
        QVERIFY(refusal(doc, missing).find("the displacement map's page 7 is not in the book") != std::string::npos);
        Json unheld = base;
        unheld["map"] = "sha256:" + std::string(64, 'b');
        unheld["map_suffix"] = ".png";
        QVERIFY(refusal(doc, unheld).find("is not in the book") != std::string::npos);
    }

    // On a smart object 置き換え's map is the book's (design 2.6): its data or the layer made a file the book holds
    // (Document::filter_files, the smart filter its ref and kind) — the layer changed afterwards, the smart object as it was
    // —; set again with another; a smart filter holding the data itself refused as read; carried to another book with its
    // smart object (bring_contents); saved, one of the book's assets gc keeps, read back and drawn the same.
    void aSmartFilterHoldsItsMapAsTheBooks() {
        const render::Image picture = eight_pattern(200, 200);
        auto doc = eight_page(picture, render::Image::create("RGBA", render::Size{200, 200}, render::Ink{255, 60, 128, 255}));
        doc = apply_ops(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}})).doc;
        // (the smart object drawn alone: the map layer over it covers the page)
        const auto drawn = [](const core::Document& d) { return render::layer_image(d.page(0), layer_of(d, "so"), 100, &d).tobytes(); };
        const std::string plain = drawn(doc);
        // the layer as the map: made a PNG the book holds
        const auto by_layer = apply_ops(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "distort_displace"},
                                                               {"horizontal", 5}, {"map", Json{{"layer", "map"}}}}}))
                                  .doc;
        const Json f = (*layer_of(by_layer, "so").smart)["filters"][0];
        const std::string ref = f["params"]["map"].get<std::string>();
        QVERIFY2(ref.starts_with("sha256:"), f.dump().c_str());
        QCOMPARE(f["params"]["map_suffix"], Json(".png"));
        QVERIFY(by_layer.filter_files.contains(ref));
        QCOMPARE(core::bytes_ref(*by_layer.filter_files.at(ref)), ref);
        QVERIFY(core::uses_filters(by_layer));
        QVERIFY(core::filter_file_refs(by_layer) == (std::vector<std::pair<std::string, std::string>>{{ref, ".png"}}));
        const std::string once = drawn(by_layer);
        QVERIFY(once != plain);
        // the map layer changed afterwards: the smart object drawn as it was
        const auto changed = apply_ops(by_layer, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", "map"},
                                                                   {"png_base64", png_b64(render::Image::create("RGBA", render::Size{20, 20}, render::Ink{0, 255, 128, 255}))}}}))
                                 .doc;
        QCOMPARE(drawn(changed), drawn(by_layer));
        // the data as the map, set again on the filter: another file of the book
        const render::Image other = render::Image::create("RGBA", render::Size{4, 4}, render::Ink{40, 200, 128, 255});
        const auto set = apply_ops(by_layer, Json::array({Json{{"op", "smart_filter_set"}, {"page", 1}, {"id", "so"}, {"index", 0},
                                                               {"params", Json{{"map_data", png_b64(other)}}}}}))
                             .doc;
        const Json g = (*layer_of(set, "so").smart)["filters"][0]["params"];
        QCOMPARE(g["map"], Json(core::bytes_ref(render::write_png(other))));
        QVERIFY(!g.contains("map_data"));
        QVERIFY(set.filter_files.contains(g["map"].get<std::string>()));
        QVERIFY(drawn(set) != once);
        // saved: one of the book's assets (its ref in the smart filter: gc keeps it), read back and drawn from it the same
        {
            QTemporaryDir tmp;
            const std::filesystem::path dir = std::filesystem::path(tmp.path().toStdString()) / "b";
            {
                storage::ProjectLock lock(dir, "human:test");
                lock.try_acquire();
                storage::SaveRequest request;
                request.actor = "human:test";
                request.ops = Json::array();
                storage::Saver(lock).save(set, request);
            }
            const std::string kept = g["map"].get<std::string>();
            QVERIFY(storage::AssetStore(dir).has(kept, ".png"));
            QVERIFY(storage::referenced_assets(dir).contains(kept));
            const auto loaded = storage::load_document(dir);
            QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
            QCOMPARE(drawn(loaded.document), drawn(set));
        }
        // a smart filter holding the map's data itself, or a layer: not the book's file (read-only when read)
        core::Document bad = set;
        Json smart = *layer_of(bad, "so").smart;
        smart["filters"][0]["params"]["map_data"] = png_b64(other);
        for (core::Layer& layer : bad.edit_page(0).layers)
            if (layer.id == "so") layer.smart = smart;
        QVERIFY_THROWS_EXCEPTION(core::Error, core::validate_filters(bad));
        smart["filters"][0]["params"].erase("map_data");
        smart["filters"][0]["params"]["map"] = Json{{"layer", "map"}};
        for (core::Layer& layer : bad.edit_page(0).layers)
            if (layer.id == "so") layer.smart = smart;
        QVERIFY_THROWS_EXCEPTION(core::Error, core::validate_filters(bad));
        // carried to another book with its smart object: the map comes along (bring_contents)
        core::Document there = blank();
        core::Page page = set.page(0);
        core::smart::bring_contents(there, set, page);
        QVERIFY(there.filter_files.contains(g["map"].get<std::string>()));
        there.pages.push_back(std::make_shared<core::Page>(std::move(page)));
        core::validate_filters(there, [](const std::string&, const std::string&) { return false; });  // (held by the book itself)
    }

    // A whole kind's base box on a 32-bit precise layer is read in place (design 4.3: no float copy held in the imaging
    // budget), the same bytes as a copy of it; on 16 bits it is copied, held, and refused past the budget — a CMYK page's
    // 16-bit plates too (their floats, five a pixel).
    void aWholeKindReadsA32BitLayerInPlace() {
        const std::string f32 = precise(600, 400, pattern(600, 400), true);
        const std::string u16 = precise(600, 400, pattern(600, 400), false);
        const Json twirl{{"angle", 90}};
        const std::string free_f32 = run_precise(f32, "distort_twirl", twirl);
        {
            // (less than the base box's 600 × 400 floats — 3.84 MB —, more than the threads' tiles)
            const render::ImageAllocationBudget budget{3'500'000};
            Run one;
            one.tiling = {0, 1};
            QCOMPARE(run_precise(f32, "distort_twirl", twirl, one), free_f32);
            QVERIFY_THROWS_EXCEPTION(core::OpError, run_precise(u16, "distort_twirl", twirl, one));
        }
        // (a selection's box of whole rows from the 100th: read in place from there — the pixels the kind makes on those rows
        // alone)
        const Rect half{0, 100, 600, 300};
        std::string cover(600 * 400, '\0');
        std::fill(cover.begin() + 600 * 100, cover.begin() + 600 * 300, static_cast<char>(255));
        Run boxed;
        boxed.bounds = &half;
        boxed.cover = &cover;
        const std::string got = run_precise(f32, "distort_twirl", twirl, boxed);
        const std::vector<Px> all = pattern(600, 400);
        const std::string alone = run_precise(precise(600, 200, std::vector<Px>(all.begin() + 600 * 100, all.begin() + 600 * 300), true), "distort_twirl", twirl);
        for (std::uint32_t y = 0; y < 200; y += 7)
            for (std::uint32_t x = 0; x < 600; x += 5) {
                const auto a = pixel(got, x, y + 100), b = pixel(alone, x, y);
                for (unsigned c = 0; c < 4; ++c) QVERIFY2(std::fabs(a[c] - b[c]) < 1e-4, qPrintable(QStringLiteral("(%1, %2)").arg(x).arg(y)));
            }
        QCOMPARE(pixel(got, 10, 10), pixel(f32, 10, 10));
        // (a CMYK page of 128 × 128 at 16 bits: its whole base box as floats — 327 680 bytes — past a budget of 250 000; a
        // kind of a margin's tiles of 32 within it — a window and a tile, about 212 000 bytes)
        const auto cmyk = ink_page("cmyk", [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 7 + y * 3 + c * 11) % 64) / 63.0; }, 128);
        const core::InkPixels inks = inks_of(cmyk);
        const Rect grid{0, 0, 128, 128};
        {
            const render::ImageAllocationBudget budget{250'000};
            const auto whole = render::fx::ink_surface(inks, *cmyk.page(0).color);
            QVERIFY_THROWS_EXCEPTION(core::OpError, render::fx::apply(*whole, kernel("distort_twirl"), normal("distort_twirl", twirl), core::FilterInks::Plates, grid, nullptr,
                                                                      1, nullptr, {0, 1}));
            const auto tiled = render::fx::ink_surface(inks, *cmyk.page(0).color);
            render::fx::apply(*tiled, kernel("stylize_find_edges"), normal("stylize_find_edges", Json::object()), core::FilterInks::LightnessAndColour, grid, nullptr, 1,
                              nullptr, {32, 1});
        }
    }

    // How heavy (render::fx::work): 油彩 on a B4 page at 600 dpi is heavy (a thread of its own, 中止), メゾティント on an 8-bit
    // page is not.
    // An opened picture's page (PC4 F-b's grid "finest", 300 ppi; the integration 441a58f): each kind works on the picture's
    // own pixels, its lengths the working 200 dpi's scaled (モザイク's 10: a cell of 15 pixels).
    void onAnOpenedPicture() {
        const render::Image picture = eight_pattern(300, 200).convert("RGB");
        const std::string png = formats::png_bytes(picture, formats::PngSave{std::array<double, 2>{300, 300}, {}, false});
        const core::Document doc = formats::open_file(png, "picture.png").doc;
        const std::string back = doc.page(0).layers[0].id;
        QVERIFY(std::fabs(render::fx::layer_grid(doc.page(0), doc.page(0).layers[0]).s - 1.5) < 1e-9);
        const auto picture_of = [](const core::Document& d) { return render::raster::layer_pixels(d.page(0), d.page(0).layers[0]).convert("RGBA"); };
        for (const auto& [kind, params] : d2_kinds()) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", back}, {"kind", kind}};
            op.update(params);
            if (kind == "distort_displace") op["map"] = Json{{"layer", "merged"}};
            const render::Image done = picture_of(apply_ops(doc, Json::array({op})).doc);
            QCOMPARE(done.size(), (render::Size{300, 200}));
            QVERIFY2(done.tobytes() != picture.convert("RGBA").tobytes(), kind.c_str());
        }
        const Json mosaic{{"op", "filter_raster"}, {"page", 1}, {"id", back}, {"kind", "pixelate_mosaic"}, {"cell_size", 10}};
        const render::Image cells = picture_of(apply_ops(doc, Json::array({mosaic})).doc);
        for (int y = 15; y < 30; ++y)
            for (int x = 15; x < 30; ++x) QCOMPARE(cells.getpixel(x, y), cells.getpixel(15, 15));
        QVERIFY(cells.getpixel(29, 15) != cells.getpixel(30, 15));
        QVERIFY(cells.getpixel(15, 29) != cells.getpixel(15, 30));
    }

    void howHeavy() {
        const Json oil = normal("stylize_oil_paint", Json::object());
        QVERIFY(render::fx::work("stylize_oil_paint", oil, 6071ull * 8598ull, 3) >= render::fx::kHeavyWork);
        QVERIFY(render::fx::work("pixelate_mezzotint", normal("pixelate_mezzotint", Json::object()), 1400ull * 2000ull, 1) < render::fx::kHeavyWork);
        QVERIFY(render::fx::work("distort_twirl", normal("distort_twirl", Json::object()), 6071ull * 8598ull, 3) >= render::fx::kHeavyWork);
    }
};

QTEST_GUILESS_MAIN(TestFxD2)
#include "test_fx_d2.moc"
