// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 2.1-2.3, 2.8): フィルター's kinds in the table without Python — each in
// the correction kinds' table (group PC4: filter_raster's, local, a smart filter), its settings read as an op's (clamped,
// defaults filled) and a book's (out of range refused), its side (PhotoCraft's command id, Photoshop's name, its
// submenu, how a page of inks takes it), its kernel in this build; the presets; .pcraft's commands read and written back
// (D0's blurs; D7's table: every kind written as PhotoCraft's command and read back, PhotoCraft's words and units); a
// kind's places moved with a smart object; filters@1 named while a smart filter is one of them and not otherwise; the
// catalogue of genko inspect --filters.

#include <QtTest/QtTest>

#include <cmath>
#include <string>
#include <tuple>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/ops_registry.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

std::string refused(const std::function<void()>& work) {
    try {
        work();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

// (D7) Two settings the same, their numbers within `tolerance` (a way and back through other units).
bool alike(const Json& a, const Json& b, double tolerance = 1e-9) {
    if (a.is_number() && b.is_number() && !a.is_boolean() && !b.is_boolean()) return std::fabs(a.get<double>() - b.get<double>()) <= tolerance;
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!alike(a[i], b[i], tolerance)) return false;
        return true;
    }
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, value] : a.items())
            if (!b.contains(key) || !alike(value, b[key], tolerance)) return false;
        return true;
    }
    return a == b;
}

// (D7) A kind's settings at its defaults (the ones it needs given).
Json defaults_of(const std::string& kind) {
    Json given = Json::object();
    if (kind == "filter_gallery") given["effects"] = Json::array({Json{{"filter", "colored_pencil"}}});
    return core::normalize_filter(kind, given, nullptr);
}

}  // namespace

class TestFilterKinds : public QObject {
    Q_OBJECT

private slots:
    void theKindsAreInTheTable() {
        for (const char* name : {"blur_gaussian", "blur_box"}) {
            const core::AdjustKind* kind = core::adjust_kind(name);
            QVERIFY2(kind != nullptr, name);
            QCOMPARE(kind->group, std::string_view("PC4"));
            QVERIFY(kind->filter && kind->local && kind->smart && !kind->layer);
            QVERIFY(core::smart::filter_kind(name));
            QVERIFY(core::native_filter(name, Json::object()));
            QVERIFY(core::pc4_filter(name));
            QVERIFY2(render::fx::kernel(name) != nullptr, name);
            QVERIFY(!core::smart::placed_filter(name));  // (no places: nothing moves with a smart object)
        }
        // every PC4 kind has its side, its name and its kernel (a group adds both)
        for (const core::FilterInfo& info : core::filter_infos()) {
            QVERIFY(info.upstream.starts_with("filter."));
            QVERIFY(!info.name.empty());
            // (a kind of the フィルター menu itself — フィルターギャラリー…, Camera Raw フィルター…, レンズ補正…, 広角補正… — has no
            // submenu: FilterInfo::submenu empty)
            QVERIFY(!info.submenu.empty() || info.kind == "filter_gallery" || info.kind == "camera_raw" || info.kind == "lens_correction" ||
                    info.kind == "adaptive_wide_angle");
            QVERIFY2(render::fx::kernel(info.kind) != nullptr, std::string(info.kind).c_str());
            QVERIFY(core::adjust_kind(info.kind) != nullptr);
        }
        const core::FilterInfo* gaussian = core::filter_info("blur_gaussian");
        QCOMPARE(gaussian->upstream, std::string_view("filter.blur.gaussianBlur"));
        QCOMPARE(gaussian->name, std::string_view("ぼかし（ガウス）"));
        QCOMPARE(gaussian->submenu, std::string_view("ぼかし"));
        QCOMPARE(gaussian->inks, core::FilterInks::Plates);
        QCOMPARE(core::filter_info("blur_box")->upstream, std::string_view("filter.blur.boxBlur"));
        // (Python's 22 stay Python's: none of them is a PC4 kind)
        for (const char* python : {"blur", "sharpen", "mosaic", "motion_blur", "radial_blur", "zoom_blur", "noise", "wave", "twirl", "glow"}) {
            QVERIFY(!core::pc4_filter(python));
            QVERIFY(core::adjust_kind(python) == nullptr || core::adjust_kind(python)->group == std::string_view("Python"));
        }
        QVERIFY(core::smart::placed_filter("radial_blur"));
    }

    // D1's kinds (ぼかし・シャープ・ノイズ・その他・ビデオ): in the table with PhotoCraft's command ids, Photoshop's names and
    // submenus; how a page of inks takes them (the lightness: 明るさの最大値・最小値; the lightness, a Lab page's a and b too:
    // カスタム・ハイパス; the colours it means: HSB/HSL・NTSC カラー; each plate: the rest); ぼかし（放射状）'s centre a place that
    // moves with a smart object.
    void theKindsOfD1() {
        const std::vector<std::tuple<const char*, const char*, const char*, const char*, core::FilterInks>> want{
            {"blur_average", "filter.blur.average", "平均", "ぼかし", core::FilterInks::Plates},
            {"blur_motion", "filter.blur.motionBlur", "ぼかし（移動）", "ぼかし", core::FilterInks::Plates},
            {"blur_radial", "filter.blur.radialBlur", "ぼかし（放射状）", "ぼかし", core::FilterInks::Plates},
            {"blur_surface", "filter.blur.surfaceBlur", "ぼかし（表面）", "ぼかし", core::FilterInks::Plates},
            {"blur_smart", "filter.blur.smartBlur", "ぼかし（詳細）", "ぼかし", core::FilterInks::Plates},
            {"blur_shape", "filter.blur.shapeBlur", "ぼかし（シェイプ）", "ぼかし", core::FilterInks::Plates},
            {"blur_lens", "filter.blur.lensBlur", "ぼかし（レンズ）", "ぼかし", core::FilterInks::Plates},
            {"sharpen_unsharp_mask", "filter.sharpen.unsharpMask", "アンシャープマスク", "シャープ", core::FilterInks::Plates},
            {"sharpen_smart", "filter.sharpen.smartSharpen", "スマートシャープ", "シャープ", core::FilterInks::Plates},
            {"noise_add", "filter.noise.addNoise", "ノイズを加える", "ノイズ", core::FilterInks::Plates},
            {"noise_dust_and_scratches", "filter.noise.dustAndScratches", "ダスト＆スクラッチ", "ノイズ", core::FilterInks::Plates},
            {"noise_median", "filter.noise.median", "明るさの中間値", "ノイズ", core::FilterInks::Plates},
            {"noise_reduce", "filter.noise.reduceNoise", "ノイズを軽減", "ノイズ", core::FilterInks::Plates},
            {"other_custom", "filter.other.custom", "カスタム", "その他", core::FilterInks::LightnessAndColour},
            {"other_high_pass", "filter.other.highPass", "ハイパス", "その他", core::FilterInks::LightnessAndColour},
            {"other_hsb_hsl", "filter.other.hsbHsl", "HSB/HSL", "その他", core::FilterInks::Meaning},
            {"other_maximum", "filter.other.maximum", "明るさの最大値", "その他", core::FilterInks::Lightness},
            {"other_minimum", "filter.other.minimum", "明るさの最小値", "その他", core::FilterInks::Lightness},
            {"other_offset", "filter.other.offset", "スクロール", "その他", core::FilterInks::Plates},
            {"video_de_interlace", "filter.video.deInterlace", "インターレース解除", "ビデオ", core::FilterInks::Plates},
            {"video_ntsc_colors", "filter.video.ntscColors", "NTSC カラー", "ビデオ", core::FilterInks::Meaning},
        };
        for (const auto& [kind, upstream, name, submenu, inks] : want) {
            const core::FilterInfo* info = core::filter_info(kind);
            QVERIFY2(info != nullptr, kind);
            QCOMPARE(info->upstream, std::string_view(upstream));
            QCOMPARE(info->name, std::string_view(name));
            QCOMPARE(info->submenu, std::string_view(submenu));
            QCOMPARE(info->inks, inks);
            const core::AdjustKind* table = core::adjust_kind(kind);
            QVERIFY(table != nullptr && table->group == std::string_view("PC4") && table->filter && table->local && table->smart);
            QVERIFY2(render::fx::kernel(kind) != nullptr, kind);
            QCOMPARE(core::smart::placed_filter(kind), std::string_view(kind) == "blur_radial");
        }
        // PhotoCraft's defaults (filters.rs params_for, filters_ext.rs) and ranges, clamped as an op's
        QCOMPARE(core::normalize_filter("blur_motion", Json::object(), nullptr), (Json{{"angle", 0}, {"distance", 10}}));
        QCOMPARE(core::normalize_filter("blur_motion", Json{{"distance", 5000}}, nullptr)["distance"], Json(2000));
        QCOMPARE(core::normalize_filter("blur_radial", Json::object(), nullptr),
                 (Json{{"amount", 10}, {"method", "spin"}, {"center_x", 0.5}, {"center_y", 0.5}}));
        QCOMPARE(core::normalize_filter("sharpen_unsharp_mask", Json::object(), nullptr), (Json{{"amount", 50}, {"radius", 1}, {"threshold", 0}}));
        QCOMPARE(core::normalize_filter("noise_add", Json::object(), nullptr),
                 (Json{{"amount", 12.5}, {"distribution", "uniform"}, {"monochromatic", false}, {"seed", 0}}));
        QCOMPARE(core::normalize_filter("noise_reduce", Json::object(), nullptr),
                 (Json{{"strength", 6}, {"preserve_details", 60}, {"reduce_color_noise", 45}, {"sharpen_details", 25}, {"remove_jpeg_artifact", false}}));
        QCOMPARE(core::normalize_filter("blur_smart", Json::object(), nullptr)["quality"], Json("high"));
        QCOMPARE(core::normalize_filter("other_hsb_hsl", Json::object(), nullptr), (Json{{"input_mode", "rgb"}, {"row_order", "hsb"}}));
        QCOMPARE(core::normalize_filter("other_maximum", Json{{"radius", 0.1}}, nullptr), (Json{{"radius", 0.2}, {"preserve", "squareness"}}));
        QCOMPARE(core::normalize_filter("video_de_interlace", Json::object(), nullptr), (Json{{"eliminate", "odd_fields"}, {"create_by", "interpolation"}}));
        QCOMPARE(core::normalize_filter("blur_average", Json::object(), nullptr), Json::object());
        const Json custom = core::normalize_filter("other_custom", Json::object(), nullptr);
        QCOMPARE(custom["kernel"].size(), std::size_t(25));
        QCOMPARE(custom["kernel"][12], Json(1));
        QCOMPARE(custom["scale"], Json(1));
        // refused: a kernel not of 25 numbers, a choice it has not
        QVERIFY(refused([] { (void)core::normalize_filter("other_custom", Json{{"kernel", Json::array({1, 2, 3})}}, nullptr); }).find("25 numbers") != std::string::npos);
        QVERIFY(!refused([] { (void)core::normalize_filter("blur_shape", Json{{"shape", "moon"}}, nullptr); }).empty());
        QVERIFY(!refused([] { (void)core::normalize_filter("blur_lens", Json{{"depth_map", "far"}}, nullptr); }).empty());
    }

    void settingsAreReadAsAnOpsAndABooks() {
        // an op's: its default filled, out of its range clamped (PhotoCraft's 0.1..1000 and 1..2000)
        QCOMPARE(core::normalize_filter("blur_gaussian", Json::object(), nullptr), (Json{{"radius", 1}}));
        QCOMPARE(core::normalize_filter("blur_gaussian", Json{{"radius", 5000}}, nullptr), (Json{{"radius", 1000}}));
        QCOMPARE(core::normalize_filter("blur_gaussian", Json{{"radius", 0.01}}, nullptr)["radius"].get<double>(), 0.1);
        QCOMPARE(core::normalize_filter("blur_box", Json{{"radius", 0}}, nullptr), (Json{{"radius", 1}}));
        QCOMPARE(core::normalize_filter("blur_box", Json{{"radius", 7.5}}, nullptr), (Json{{"radius", 7.5}}));
        // refused: the wrong type, not finite, a key it has not, a native
        QVERIFY(refused([] { (void)core::normalize_filter("blur_gaussian", Json{{"radius", "big"}}, nullptr); }).find("must be a number") != std::string::npos);
        QVERIFY(refused([] { (void)core::normalize_filter("blur_gaussian", Json{{"radius", true}}, nullptr); }).find("must be a number") != std::string::npos);
        QVERIFY(refused([] { (void)core::normalize_filter("blur_gaussian", Json{{"sigma", 2}}, nullptr); }).find("has no setting 'sigma'") != std::string::npos);
        QVERIFY(!refused([] { (void)core::normalize_filter("blur_box", Json{{"native", Json{{"blend", "multiply"}}}}, nullptr); }).empty());
        // a book's: out of range refused, nothing filled in
        QVERIFY(!refused([] { (void)core::read_params(core::adjust_kind("blur_gaussian")->params, Json{{"radius", 5000}}, core::ReadMode::Stored, "f"); }).empty());
        QCOMPARE(core::read_params(core::adjust_kind("blur_gaussian")->params, Json::object(), core::ReadMode::Stored, "f"), Json::object());
    }

    void thePresets() {
        int usable = 0;
        for (const core::FilterPreset& preset : core::filter_presets()) {
            QVERIFY(preset.upstream.starts_with("filter."));
            if (core::pc4_filter(preset.kind)) ++usable;
        }
        QCOMPARE(core::filter_presets().size(), std::size_t(6));
        QCOMPARE(usable, 6);  // (D1: the sharpening ones and 輪郭以外をぼかす are kinds of this build too)
        QCOMPARE(core::filter_presets()[0].name, std::string_view("ぼかし"));
        QCOMPARE(core::filter_presets()[0].params, (Json{{"radius", 0.6}}));
        QCOMPARE(core::filter_presets()[1].params, (Json{{"radius", 1.4}}));
    }

    void pcraftsCommandsAndBack() {
        // a 3-pixel Gaussian of a 300 dpi document: 2 pixels of the working 200 dpi, and back
        const auto in = core::from_upstream("filter.blur.gaussianBlur", Json{{"radius", 3}}, 300);
        QVERIFY(in.has_value());
        QCOMPARE(in->first, std::string("blur_gaussian"));
        QVERIFY(std::fabs(in->second["radius"].get<double>() - 2.0) < 1e-12);
        const auto out = core::to_upstream(in->first, in->second, 300);
        QVERIFY(out.has_value());
        QCOMPARE(out->first, std::string("filter.blur.gaussianBlur"));
        QVERIFY(std::fabs(out->second["radius"].get<double>() - 3.0) < 1e-12);
        const auto box = core::from_upstream("filter.blur.boxBlur", Json{{"radius", 4}}, 200);
        QCOMPARE(box->first, std::string("blur_box"));
        QCOMPARE(box->second["radius"].get<double>(), 4.0);
        // a preset: the Gaussian of its radius in the document's pixels
        const auto blur = core::from_upstream("filter.blur.blur", Json::object(), 400);
        QCOMPARE(blur->first, std::string("blur_gaussian"));
        QVERIFY(std::fabs(blur->second["radius"].get<double>() - 0.3) < 1e-12);
        QVERIFY(!core::from_upstream("filter.render.tree", Json::object(), 200).has_value());
        QVERIFY(!core::to_upstream("blur", Json::object(), 200).has_value());
    }

    // D7: every kind of フィルター with PhotoCraft's command written as it and read back the same (at 300 dpi: its lengths
    // through the document's pixels), at its defaults and at settings of its own; the one whose file the book holds
    // (置き換え's map) not.
    void everyKindToPhotoCraftAndBack() {
        const core::UpstreamCanvas canvas{120, 160};
        std::size_t mapped = 0;
        for (const core::FilterInfo& info : core::filter_infos()) {
            const std::string kind(info.kind);
            if (kind == "distort_displace") {
                QVERIFY(!core::to_upstream(kind, Json{{"map", "sha256:" + std::string(64, 'a')}, {"map_suffix", ".png"}}, 300, canvas));
                QVERIFY(!core::from_upstream(info.upstream, Json{{"horizontal", 10}}, 300, canvas));
                QVERIFY(core::upstream_filter(info.upstream));
                continue;
            }
            if (kind == "blur_average" || kind == "render_clouds" || kind == "render_difference_clouds") {
                // (review P2-3: not PhotoCraft's smart filters — it leaves the pixels alone for them —: not written, and a
                // .pcraft's one draws nothing; a PSD's Photoshop filter of them is read)
                QVERIFY2(!core::to_upstream(kind, defaults_of(kind), 300, canvas), kind.c_str());
                QVERIFY2(!core::upstream_filter(info.upstream), kind.c_str());
                QVERIFY2(core::from_upstream(info.upstream, Json::object(), 300, canvas).has_value(), kind.c_str());
                continue;
            }
            Json settings = defaults_of(kind);
            if (kind == "blur_path") {  // (Genko's own path across the middle, written out as upstream's — review P2-2)
                settings["paths"] = Json::array({Json{{"points", Json::array({Json::array({24, 80}), Json::array({96, 80})})}, {"speed", 50}, {"taper", 0}}});
            }
            const auto out = core::to_upstream(kind, settings, 300, canvas);
            QVERIFY2(out.has_value(), kind.c_str());
            QCOMPARE(out->first, std::string(info.upstream));
            QVERIFY2(core::upstream_filter(out->first), kind.c_str());
            const auto back = core::from_upstream(out->first, out->second, 300, canvas);
            QVERIFY2(back.has_value(), (kind + " " + out->second.dump()).c_str());
            QCOMPARE(back->first, kind);
            QVERIFY2(alike(back->second, settings), (kind + "\n" + settings.dump() + "\n" + back->second.dump()).c_str());
            ++mapped;
        }
        QCOMPARE(mapped + 4, core::filter_infos().size());  // (all but 置き換え and the three upstream draws not as smart filters)
        // (settings of their own: places, lists, colours, choices, lengths)
        const std::vector<std::pair<std::string, Json>> own{
            {"blur_radial", Json{{"amount", 30}, {"method", "zoom"}, {"center_x", 0.25}, {"center_y", 0.75}}},
            {"stylize_wind", Json{{"method", "stagger"}, {"direction", "from_left"}}},
            {"pixelate_mezzotint", Json{{"type", "medium_lines"}, {"seed", 7}}},
            {"pixelate_pointillize", Json{{"cell_size", 9}, {"background", Json::array({10, 20, 30})}}},
            {"stylize_tiles", Json{{"fill", "inverse"}, {"foreground", Json::array({255, 0, 0})}}},
            {"other_offset", Json{{"horizontal", 12}, {"vertical", -6}, {"undefined_areas", "transparent"}}},
            {"distort_wave", Json{{"wavelength_min", 20}, {"wavelength_max", 40}, {"type", "square"}}},
            {"blur_iris", Json{{"pins", Json::array({Json{{"x", 0.3}, {"y", 0.6}, {"radius_x", 0.2}, {"blur", 24}}})}}},
            {"blur_spin", Json{{"pins", Json::array({Json{{"x", 0.5}, {"y", 0.5}, {"blur_angle", 40}}, Json{{"x", 0.1}, {"y", 0.9}}})}}},
            {"blur_path", Json{{"paths", Json::array({Json{{"points", Json::array({Json::array({12, 40}), Json::array({60, 80})})}, {"speed", 30}, {"taper", 20}}})}}},
            {"render_lighting_effects", Json{{"lights", Json::array({Json{{"kind", "point"}, {"x", 0.4}, {"color", Json::array({255, 128, 0})}}})}, {"texture", "luminance"}}},
            {"adaptive_wide_angle", Json{{"model", "full_spherical"}, {"constraints", Json::array({Json{{"a", Json::array({10, 20})}, {"b", Json::array({50, 22})}, {"orientation", "horizontal"}}})}}},
            {"lens_correction", Json{{"profile", "generic"}, {"focal_length", 24}, {"correct_ca", false}, {"edge", "color"}, {"edge_color", Json::array({0, 0, 255})}}},
            {"camera_raw", Json{{"exposure", 0.5}, {"sharpen_amount", 40}, {"sharpen_radius", 1.5}, {"vignette_style", "paint_overlay"},
                                {"grade_shadows", Json{{"hue", 200}, {"sat", 30}, {"lum", -10}}}, {"point_curve", Json::array({Json::array({0, 10}), Json::array({255, 240})})}}},
            {"filter_gallery", Json{{"effects", Json::array({Json{{"filter", "neon_glow"}, {"params", Json{{"glow_color", Json::array({0, 255, 0})}}}},
                                                             Json{{"filter", "rough_pastels"}, {"params", Json{{"light", "bottomLeft"}}}, {"visible", false},
                                                                  {"foreground", Json::array({1, 2, 3})}}})},
                                    {"background", Json::array({250, 240, 230})}}},
        };
        for (const auto& [kind, given] : own) {
            const Json settings = core::normalize_filter(kind, given, nullptr);
            const auto out = core::to_upstream(kind, settings, 300, canvas);
            QVERIFY2(out.has_value(), kind.c_str());
            const auto back = core::from_upstream(out->first, out->second, 300, canvas);
            QVERIFY2(back.has_value() && back->first == kind, (kind + " " + out->second.dump()).c_str());
            QVERIFY2(alike(back->second, settings, 1e-9), (kind + "\n" + settings.dump() + "\n" + back->second.dump()).c_str());
        }
    }

    // D7: PhotoCraft's words and units as Genko's — camelCase keys and choices, colours 0..1 or hexadecimal, the document's
    // pixels, a path's shares of its canvas, its flat settings of one pin, light or path, its one-step presets, the
    // gallery's commands —; what is not mapped.
    void photoCraftsWordsAndUnits() {
        const auto in = [](const char* command, const Json& params, double dpi = 300, core::UpstreamCanvas canvas = {}) {
            const auto read = core::from_upstream(command, params, dpi, canvas);
            return read ? read->second : Json(nullptr);
        };
        Json wind = in("filter.stylize.wind", Json{{"method", "blast"}, {"direction", "fromLeft"}, {"seed", 3}});
        QCOMPARE(wind["direction"], Json("from_left"));
        QCOMPARE(wind["method"], Json("blast"));
        QCOMPARE(in("filter.pixelate.mezzotint", Json{{"type", "mediumLines"}})["type"], Json("medium_lines"));
        QCOMPARE(in("filter.blur.smartBlur", Json{{"mode", "overlayEdge"}})["mode"], Json("overlay_edge"));
        // (keys it has none of: left out, as PhotoCraft reads only its own)
        QCOMPARE(in("filter.cameraRaw", Json{{"pixelScale", 3}, {"vignetteStyle", "paintOverlay"}})["vignette_style"], Json("paint_overlay"));
        const Json lens = in("filter.lensCorrection", Json{{"correctCA", false}, {"edge", "edgeExtension"}, {"profile", "generic"}, {"focalLength", 35}});
        QCOMPARE(lens["correct_ca"], Json(false));
        QCOMPARE(lens["edge"], Json("extension"));
        // colours: 0..1 and #rrggbb
        std::string green(1, '#');
        green += "00ff00";
        const Json tiles = in("filter.stylize.tiles", Json{{"foreground", Json::array({1.0, 0.5, 0.0, 1.0})}, {"background", green}});
        QCOMPARE(tiles["foreground"], Json::array({255, 128, 0}));
        QCOMPARE(tiles["background"], Json::array({0, 255, 0}));
        QVERIFY(in("filter.stylize.tiles", Json{{"foreground", "red"}}).is_null());
        // lengths: the document's pixels at 300 dpi as the working 200 dpi's
        QVERIFY(std::fabs(in("filter.noise.median", Json{{"radius", 6}})["radius"].get<double>() - 4) < 1e-12);
        QVERIFY(std::fabs(in("filter.distort.wave", Json{{"wavelengthMax", 150}})["wavelength_max"].get<double>() - 100) < 1e-12);
        QVERIFY(std::fabs(in("filter.cameraRaw", Json{{"sharpenRadius", 2.4}}, 400)["sharpen_radius"].get<double>() - 1.2) < 1e-12);
        // (one pin of the flat settings; an empty list is upstream's one pin)
        const Json iris = in("filter.blurGallery.irisBlur", Json{{"centerX", 0.3}, {"radiusX", 0.2}, {"blur", 30}}, 600);
        QCOMPARE(iris["pins"].size(), std::size_t(1));
        QVERIFY(std::fabs(iris["pins"][0]["x"].get<double>() - 0.3) < 1e-12 && std::fabs(iris["pins"][0]["blur"].get<double>() - 10) < 1e-12);
        // (an empty list: upstream's one pin, its blur 15 of the document's pixels — 10 working pixels at 300 dpi)
        const Json field = in("filter.blurGallery.fieldBlur", Json{{"pins", Json::array()}});
        QCOMPARE(field["pins"].size(), std::size_t(1));
        QCOMPARE(field["pins"][0]["x"].get<double>(), 0.5);
        QVERIFY(std::fabs(field["pins"][0]["blur"].get<double>() - 10) < 1e-12);
        // review P2-2: lengths not given are upstream's defaults of the document's pixels (300 dpi: × 2/3)
        QVERIFY(std::fabs(in("filter.blur.gaussianBlur", Json::object())["radius"].get<double>() - 2.0 / 3.0) < 1e-12);
        QVERIFY(std::fabs(in("filter.blur.motionBlur", Json{{"angle", 45}})["distance"].get<double>() - 20.0 / 3.0) < 1e-12);
        QVERIFY(std::fabs(in("filter.cameraRaw", Json::object())["sharpen_radius"].get<double>() - 2.0 / 3.0) < 1e-12);
        QVERIFY(std::fabs(in("filter.blurGallery.irisBlur", Json{{"centerX", 0.3}})["pins"][0]["blur"].get<double>() - 10) < 1e-12);
        // review P2-1: シアー without points is its window's amount — upstream's three points
        QCOMPARE(in("filter.distort.shear", Json{{"amount", 40}, {"undefinedAreas", "repeat"}})["points"],
                 Json::array({Json::array({0, 0}), Json::array({0.5, 0.4}), Json::array({1, 0})}));
        QCOMPARE(in("filter.distort.shear", Json{{"amount", 40}, {"undefinedAreas", "repeat"}})["undefined_areas"], Json("repeat"));
        QCOMPARE(in("filter.distort.shear", Json{{"points", Json::array({Json::array({0, 0.1}), Json::array({1, -0.1})})}})["points"],
                 Json::array({Json::array({0, 0.1}), Json::array({1, -0.1})}));
        const Json spin = in("filter.blurGallery.spinBlur", Json{{"pins", Json::array({Json{{"x", 0.1}, {"blurAngle", 90}, {"radiusY", 0.4}}})}});
        QCOMPARE(spin["pins"][0]["blur_angle"].get<double>(), 90.0);
        QCOMPARE(spin["pins"][0]["radius_y"].get<double>(), 0.4);
        // (a path's points shares of the canvas: the page's mm; without the canvas, not mapped)
        const Json path = in("filter.blurGallery.pathBlur", Json{{"startX", 0.25}, {"endX", 0.75}, {"speed", 60}}, 400, core::UpstreamCanvas{100, 200});
        QCOMPARE(path["paths"][0]["points"], Json::array({Json::array({25.0, 100.0}), Json::array({75.0, 100.0})}));
        QVERIFY(std::fabs(path["paths"][0]["speed"].get<double>() - 30) < 1e-12);
        QVERIFY(in("filter.blurGallery.pathBlur", Json{{"paths", Json::array({Json{{"points", Json::array({Json::array({0, 0}), Json::array({1, 1})})}}})}}).is_null());
        // (upstream's own path across the middle, its speed 50 of the document's pixels: written out; without the canvas, not)
        const Json middle = in("filter.blurGallery.pathBlur", Json::object(), 400, core::UpstreamCanvas{100, 200});
        QCOMPARE(middle["paths"][0]["points"], Json::array({Json::array({20.0, 100.0}), Json::array({80.0, 100.0})}));
        QVERIFY(std::fabs(middle["paths"][0]["speed"].get<double>() - 25) < 1e-12);
        QVERIFY(in("filter.blurGallery.pathBlur", Json::object()).is_null());
        // (a light of the flat settings, its colour 0..1; a constraint's ends the document's pixels as mm)
        const Json light = in("filter.render.lightingEffects", Json{{"lightType", "infinite"}, {"color", Json::array({0, 0.5, 1})}, {"whiteIsHigh", false}});
        QCOMPARE(light["lights"][0]["kind"], Json("infinite"));
        QCOMPARE(light["lights"][0]["color"], Json::array({0, 128, 255}));
        QCOMPARE(light["white_is_high"], Json(false));
        const Json wide = in("filter.adaptiveWideAngle", Json{{"model", "fullSpherical"}, {"constraints", Json::array({Json{{"a", Json::array({300, 0})}, {"b", Json::array({600, 300})}}})}});
        QCOMPARE(wide["model"], Json("full_spherical"));
        QVERIFY(std::fabs(wide["constraints"][0]["a"][0].get<double>() - 25.4) < 1e-12 && std::fabs(wide["constraints"][0]["b"][1].get<double>() - 25.4) < 1e-12);
        // the presets and the gallery
        const auto sharpen = core::from_upstream("filter.sharpen.sharpenMore", Json::object(), 200);
        QVERIFY(sharpen && sharpen->first == "sharpen_unsharp_mask");
        QCOMPARE(sharpen->second["amount"].get<double>(), 150.0);
        const auto pencil = core::from_upstream("filter.gallery.coloredPencil", Json{{"pencilWidth", 6}, {"foreground", Json::array({1, 0, 0})}}, 300);
        QVERIFY(pencil && pencil->first == "filter_gallery");
        QCOMPARE(pencil->second["effects"][0]["filter"], Json("colored_pencil"));
        QCOMPARE(pencil->second["effects"][0]["params"]["pencil_width"].get<double>(), 6.0);
        QCOMPARE(pencil->second["effects"][0]["foreground"], Json::array({255, 0, 0}));
        const auto neon = core::from_upstream("filter.filterGallery",
                                              Json{{"effects", Json::array({Json{{"filter", "noSuchEffect"}, {"visible", false}}, Json{{"filter", "neonGlow"}, {"glowSize", 8}}})},
                                                   {"glowColor", Json::array({1, 1, 0})}},
                                              300);
        QVERIFY(neon && neon->second["effects"].size() == 1);  // (a hidden effect upstream does not know: drawn by neither)
        QCOMPARE(neon->second["effects"][0]["params"]["glow_color"], Json::array({255, 255, 0}));
        QCOMPARE(neon->second["effects"][0]["params"]["glow_size"].get<double>(), 8.0);
        QVERIFY(!core::from_upstream("filter.filterGallery", Json{{"effects", Json::array({Json{{"filter", "noSuchEffect"}}})}}, 300));
        // not mapped: PhotoCraft's filters Genko has no kind of, a displacement map's file, settings the kind refuses, a
        // texture of a channel, a resolution of none; commands PhotoCraft does not know
        for (const char* command : {"filter.liquify", "edit.puppetWarp", "layer.smartObjects.perspectiveWarp", "plugin.run"}) {
            QVERIFY(!core::from_upstream(command, Json::object(), 300));
            QVERIFY(core::upstream_filter(command));
        }
        QVERIFY(!core::from_upstream("filter.distort.displace", Json{{"map", "/tmp/map.psd"}}, 300));
        QVERIFY(!core::from_upstream("filter.blur.gaussianBlur", Json{{"radius", "big"}}, 300));
        QVERIFY(!core::from_upstream("filter.blur.gaussianBlur", Json{{"radius", 1}}, 0));
        QVERIFY(!core::upstream_filter("filter.unknown.later"));
        QVERIFY(!core::upstream_filter("filter.render.tree"));
        QVERIFY(!core::to_upstream("render_lighting_effects", Json{{"texture", Json{{"channel", "a1"}}}}, 300));
        QVERIFY(!core::to_upstream("adaptive_wide_angle", Json{{"interpolation", "bilinear"}}, 300));
        QVERIFY(!core::to_upstream("blur_path", Json{{"paths", Json::array({Json{{"points", Json::array({Json::array({0, 0}), Json::array({5, 5})})}}})}}, 300));
        // written: camelCase, colours 0..1, the document's pixels
        const auto written = core::to_upstream("stylize_tiles", Json{{"fill", "foreground"}, {"background", Json::array({0, 0, 255})}, {"max_offset", 20}}, 300);
        QVERIFY(written);
        QCOMPARE(written->second["maxOffset"].get<double>(), 20.0);
        QCOMPARE(written->second["background"], Json::array({0.0, 0.0, 1.0}));
        QCOMPARE(written->second["fill"], Json("foreground"));
        const auto moved = core::to_upstream("blur_motion", Json{{"distance", 10}}, 600);
        QCOMPARE(moved->second["distance"].get<double>(), 30.0);
    }

    void placesGoWithASmartObject() {
        // (D1…D6's places, by the table's kinds: points, lists of points and lengths in the page's mm, in the settings and
        // in each object of a list)
        const std::vector<core::FilterPlace> places{
            {core::FilterPlace::Kind::Point, "at", {}, {}},
            {core::FilterPlace::Kind::Points, "path", {}, {}},
            {core::FilterPlace::Kind::Length, "size_mm", {}, {}},
            {core::FilterPlace::Kind::Point, "a", {}, "pins"},
            {core::FilterPlace::Kind::Length, "reach_mm", {}, "pins"},
            {core::FilterPlace::Kind::Centre, "cx", "cy", {}},
        };
        const core::smart::PlaceMap twice{[](double x, double y) { return std::make_pair(2 * x + 10, 2 * y); },
                                          [](double, double) { return std::array<double, 4>{2, 0, 0, 2}; }};
        const Json given{{"at", Json::array({1, 2})},
                         {"path", Json::array({Json::array({0, 0}), Json::array({3, 4})})},
                         {"size_mm", 5},
                         {"pins", Json::array({Json{{"a", Json::array({1, 1})}, {"reach_mm", 2}}, Json{{"a", "not a point"}}})},
                         {"cx", 0.25},
                         {"amount", 7}};
        const Json moved = core::smart::map_places(places, given, twice);
        QCOMPARE(moved["at"], (Json::array({12.0, 4.0})));
        QCOMPARE(moved["path"][1], (Json::array({16.0, 8.0})));
        QCOMPARE(moved["size_mm"].get<double>(), 10.0);
        QCOMPARE(moved["pins"][0]["a"], (Json::array({12.0, 2.0})));
        QCOMPARE(moved["pins"][0]["reach_mm"].get<double>(), 4.0);
        QCOMPARE(moved["pins"][1]["a"], Json("not a point"));  // (what is not a place: as it was)
        QCOMPARE(moved["cx"], Json(0.25));                      // (shares: render/smart_filters maps them with the boxes)
        QCOMPARE(moved["amount"], Json(7));
    }

    void filtersAreNamedWhileUsed() {
        // a book whose smart object holds a Gaussian names filters@1; with Python's blur or none, not
        auto doc = core::new_episode("f", core::Num(1), 1, core::PageSpec::custom(50, 50, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        QVERIFY(!core::uses_filters(doc));
        core::Layer so;
        so.id = "so";
        so.kind = core::LayerKind::Smart;
        so.smart = Json{{"contents", "sc_000000000000"}, {"filters", Json::array({Json{{"kind", "blur"}, {"params", Json{{"radius", 2}}}}})}};
        doc.edit_page(0).layers.push_back(so);
        // (written_pages: the book's pages read so far)
        QVERIFY(!core::uses_filters(doc));
        (*doc.edit_page(0).layers.back().smart)["filters"].push_back(Json{{"kind", "blur_gaussian"}, {"params", Json{{"radius", 2}}}});
        QVERIFY(core::uses_filters(doc));
        QVERIFY(core::filter_file_refs(doc).empty());
        core::validate_filters(doc);  // (no files: nothing to hold)
    }

    // filters@1's files (D2's displacement map, Document::filter_files) given to an op inside a smart object's contents come
    // out to the book, as its luts and smart_files do (review P2-5: else the writer would not write them).
    void filesGivenInsideContentsComeOut() {
        auto doc = core::new_episode("f", core::Num(1), 1, core::PageSpec::custom(50, 50, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        const core::Actor actor("human:test");
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 2}, {"precision", "u16"},
                                                {"pixels", Json::array({1000, 2000, 3000, 65535, 1000, 2000, 3000, 65535, 1000, 2000, 3000, 65535, 1000,
                                                                        2000, 3000, 65535})}}}),
                         actor)
                  .doc;
        const std::string painted = doc.page(0).layers.back().id;
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({painted})}, {"new_id", "so"}}}), actor)
                  .doc;
        std::string contents;
        for (const core::Layer& layer : doc.page(0).layers)
            if (layer.id == "so") contents = *core::smart::contents_id(layer);
        QVERIFY(!contents.empty());
        const std::string ref = "sha256:" + std::string(64, 'a');
        core::OpRegistry registry = render::ops_registry();
        registry.add("test_filter_file", [ref](core::OpContext& c) { c.doc.filter_files[ref] = std::make_shared<const std::string>("map"); });
        const core::Document done =
            core::CommandBus(registry).apply(doc, Json::array({Json{{"op", "test_filter_file"}, {"page", 1}, {"contents", contents}}}), actor).doc;
        QVERIFY(done.filter_files.contains(ref));
        QCOMPARE(*done.filter_files.at(ref), std::string("map"));
    }

    void theCatalogue() {
        const Json catalogue = core::filter_catalogue();
        QCOMPARE(catalogue["units"]["length"], Json("px200"));
        bool gaussian = false;
        for (const Json& kind : catalogue["kinds"]) {
            if (kind["kind"] != Json("blur_gaussian")) continue;
            gaussian = true;
            QCOMPARE(kind["upstream"], Json("filter.blur.gaussianBlur"));
            QCOMPARE(kind["name"], Json("ぼかし（ガウス）"));
            QCOMPARE(kind["smart"], Json(true));
            QCOMPARE(kind["inks"], Json("plates"));
            QCOMPARE(kind["settings"][0], (Json{{"key", "radius"}, {"type", "number"}, {"default", 1}, {"min", 0.1}, {"max", 1000}}));
        }
        QVERIFY(gaussian);
        QCOMPARE(catalogue["presets"].size(), std::size_t(6));
        QCOMPARE(catalogue["presets"][0]["usable"], Json(true));
        QCOMPARE(catalogue["presets"][2]["usable"], Json(true));  // (シャープ: D1's sharpen_unsharp_mask)
        QVERIFY(catalogue["gallery"].is_array());
    }
};

QTEST_GUILESS_MAIN(TestFilterKinds)
#include "test_filter_kinds.moc"
