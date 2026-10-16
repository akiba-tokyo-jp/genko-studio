// color-model@1 (SPEC.md COLOR-03, schema-v4.md「色モデル（color-model@1）」): the built-in ICC profiles (their bytes
// against their sha256, what Little CMS reads them as, PhotoCraft's ids and aliases).

#include <QtTest>

#include <QTemporaryDir>

#include <lcms2.h>

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_adjust.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "render/color_canvas.hpp"
#include "render/image.hpp"
#include "render/inspect.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

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
    return core::new_episode("model", core::Num(1), pages, spec, core::Binding::Right);
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

// A picture of a grey (128) on the left and a green (alpha 200) on the right.
render::Image picture() {
    render::Image out = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
    out.paste(render::Ink{128, 128, 128, 255}, render::Box{0, 0, 30, 40});
    out.paste(render::Ink{30, 160, 90, 200}, render::Box{30, 0, 60, 40});
    return out;
}

// A book of one page with a paint layer holding the picture, its lines free of the panels (as a precise layer needs).
core::Document painted(const render::Image& image) {
    core::Document doc = small_book();
    doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"title", "絵"}}})).doc;
    const std::string id = doc.page(0).layers.back().id;
    return run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"panel_each", false}, {"panel_clip", false}},
                                     Json{{"op", "put_raster"}, {"page", 1}, {"id", id}, {"png_base64", core::b64encode(render::write_png(image))}}})).doc;
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

}  // namespace

class TestColorModel : public QObject {
    Q_OBJECT

private slots:
    // The built-in profiles: their bytes are the files of data/profiles (by their sha256, fixed here), each read by Little
    // CMS as the space it is for; RGB ones are matrix-shaper profiles; sGray shows a grey v as (v, v, v) of sRGB; the
    // coated CMYK profile is PhotoCraft's (the test data's).
    void builtinProfiles() {
        const std::array<std::pair<const char*, const char*>, 8> fixed{{
            {"display-p3", "354bc401f5df673a19a88edc1e3146145dbb278d3e3ca9062e400f68954414dc"},
            {"adobe-rgb-compat", "b0a02be9fd60b859d2fcefda9460589005ffee222479635e8fb9b768f25d5fa1"},
            {"prophoto-compat", "ada9d9fd72d1f3684b75716c85e1f53dd20c5aeb12895c7dfeded986cb1afc7a"},
            {"linear-srgb", "1edb3a21bc500a43eb7761ba9342e7d19d5d4cb3cc8e682599463da67fbc73d9"},
            {"rec2020", "0c848ef7e623414254b49addeae705dfb19a54a1717c241b8c32b6d952e23164"},
            {"gray-gamma-2.2", "62389e1c7f7a354075cdef6ebd66324b1b25b28154cf30a18082ad93914dc04b"},
            {"sgray", "52eea84d014a3b408cb50ac04a3f6f1a58eab0b2a721f4233635b3ad654d7128"},
            {"coated-cmyk", "f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc"},
        }};
        QCOMPARE(core::builtin_profiles().size(), std::size_t{10});
        for (const auto& [id, sha] : fixed) {
            const core::BuiltinProfile* p = core::builtin_profile(id);
            QVERIFY2(p != nullptr && p->bytes, id);
            QCOMPARE(core::bytes_ref(*p->bytes), "sha256:" + std::string(sha));
            QCOMPARE(p->ref(), "sha256:" + std::string(sha));
            QCOMPARE(core::builtin_profile_by_ref(p->ref()), p);
            const std::string file = genko::test::read_bytes(genko::test::repo_root() + QStringLiteral("/native/src/core/data/profiles/") +
                                                            QString::fromLatin1(id) + QStringLiteral(".icc"));
            QCOMPARE(*p->bytes, file);
            cmsHPROFILE handle = cmsOpenProfileFromMem(p->bytes->data(), static_cast<cmsUInt32Number>(p->bytes->size()));
            QVERIFY2(handle != nullptr, id);
            const cmsColorSpaceSignature space = cmsGetColorSpace(handle);
            switch (p->space) {
                case core::ProfileSpace::Rgb:
                    QCOMPARE(space, cmsSigRgbData);
                    QVERIFY(cmsIsMatrixShaper(handle));
                    break;
                case core::ProfileSpace::Gray: QCOMPARE(space, cmsSigGrayData); break;
                case core::ProfileSpace::Cmyk: QCOMPARE(space, cmsSigCmykData); break;
                case core::ProfileSpace::Lab: QFAIL("a Lab profile with bytes"); break;
            }
            char description[256] = {};
            cmsGetProfileInfoASCII(handle, cmsInfoDescription, "en", "US", description, sizeof(description));
            QCOMPARE(std::string(description), std::string(p->description));
            cmsCloseProfile(handle);
            // (the page's check: a profile of its space; another space's refused)
            QVERIFY(!render::ink::profile_problem(*p->bytes, p->space));
            for (const auto other : {core::ProfileSpace::Rgb, core::ProfileSpace::Gray, core::ProfileSpace::Cmyk}) {
                if (other != p->space) QVERIFY(render::ink::profile_problem(*p->bytes, other));
            }
        }
        QCOMPARE(*core::builtin_profile("coated-cmyk")->bytes,
                 genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))));
        // srgb and lab-d50: Little CMS's own, no bytes
        for (const char* id : {"srgb", "lab-d50"}) {
            const core::BuiltinProfile* p = core::builtin_profile(id);
            QVERIFY(p != nullptr && !p->bytes && p->ref().empty());
        }
        // sGray: a grey v shows as (v, v, v) in sRGB (within a step of 8 bits)
        {
            const core::BuiltinProfile* sgray = core::builtin_profile("sgray");
            cmsHPROFILE gray = cmsOpenProfileFromMem(sgray->bytes->data(), static_cast<cmsUInt32Number>(sgray->bytes->size()));
            cmsHPROFILE srgb = cmsCreate_sRGBProfile();
            cmsHTRANSFORM t = cmsCreateTransform(gray, TYPE_GRAY_8, srgb, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
            QVERIFY(t != nullptr);
            for (int v = 0; v < 256; v += 5) {
                const unsigned char in = static_cast<unsigned char>(v);
                unsigned char out[3] = {};
                cmsDoTransform(t, &in, out, 1);
                for (const unsigned char c : out) QVERIFY2(std::abs(int(c) - v) <= 1, qPrintable(QString::number(v)));
            }
            cmsDeleteTransform(t);
            cmsCloseProfile(srgb);
            cmsCloseProfile(gray);
        }
        // PhotoCraft's names: any case, its aliases, a description
        QCOMPARE(core::builtin_profile("P3")->id, std::string_view("display-p3"));
        QCOMPARE(core::builtin_profile("Adobe RGB (1998)")->id, std::string_view("adobe-rgb-compat"));
        QCOMPARE(core::builtin_profile("bt2020")->id, std::string_view("rec2020"));
        QCOMPARE(core::builtin_profile("Gray")->id, std::string_view("gray-gamma-2.2"));
        QCOMPARE(core::builtin_profile("CMYK")->id, std::string_view("coated-cmyk"));
        QCOMPARE(core::builtin_profile("sGray (sRGB tone curve, Photocraft)")->id, std::string_view("sgray"));
        QVERIFY(core::builtin_profile("nope") == nullptr);
        QVERIFY(core::builtin_profile_by_ref("sha256:" + std::string(64, '0')) == nullptr);
    }

    // A page's `color` (color-model@1): an RGB mode and a grey mode as the writer writes them (their keys in order, "bpc"
    // only when false, "profile" only for an RGB page that has one), read back; the RGB mode with every default is none;
    // what is not as the writer writes it is refused.
    void colourJson() {
        core::RgbMode rgb;
        rgb.depth = 16;
        rgb.profile = core::builtin_profile("display-p3")->ref();
        rgb.bpc = false;
        QCOMPARE(core::to_json(rgb).dump(), "{\"mode\":\"rgb\",\"depth\":16,\"profile\":\"" + rgb.profile + "\",\"intent\":\"relative\",\"bpc\":false}");
        core::RgbMode plain;
        plain.depth = 32;
        QCOMPARE(core::to_json(plain).dump(), std::string(R"({"mode":"rgb","depth":32,"intent":"relative"})"));
        const core::ColorModel back = core::color_model_from_json(core::to_json(rgb));
        QVERIFY(back.rgb && *back.rgb == rgb && !back.inks);
        QVERIFY(!core::color_model_from_json(core::to_json(core::RgbMode{})).rgb);  // (every default: none)
        core::PageColor gray;
        gray.space = core::InkSpace::Gray;
        gray.depth = 32;
        gray.profile = core::builtin_profile("sgray")->ref();
        gray.inks.push_back(core::InkEntry{{200, 0, 0}, {0.25}});
        QCOMPARE(core::to_json(gray).dump(), "{\"mode\":\"gray\",\"depth\":32,\"profile\":\"" + gray.profile + "\",\"intent\":\"relative\",\"inks\":[[[200,0,0],[0.25]]]}");
        QVERIFY(!gray.color_mode_only());
        const core::ColorModel grey_back = core::color_model_from_json(core::to_json(gray));
        QVERIFY(grey_back.inks && *grey_back.inks == gray);
        core::PageColor cmyk;
        cmyk.profile = core::builtin_profile("coated-cmyk")->ref();
        QVERIFY(cmyk.color_mode_only());
        cmyk.bpc = false;
        QVERIFY(!cmyk.color_mode_only());
        QCOMPARE(core::to_json(cmyk)["bpc"], Json(false));
        // refused: an unknown key, a depth, a grey page without its profile, an RGB profile that is not a ref, "bpc": true,
        // a grey ink of two values
        const auto refused = [](const Json& value) {
            try {
                (void)core::color_model_from_json(value);
            } catch (const core::Error& error) {
                return error.code() == "value";
            }
            return false;
        };
        Json j = core::to_json(rgb);
        j["extra"] = 1;
        QVERIFY(refused(j));
        j = core::to_json(rgb);
        j["depth"] = 12;
        QVERIFY(refused(j));
        j = core::to_json(rgb);
        j["profile"] = "display-p3";
        QVERIFY(refused(j));
        j = core::to_json(rgb);
        j["bpc"] = true;
        QVERIFY(refused(j));
        j = core::to_json(gray);
        j.erase("profile");
        QVERIFY(refused(j));
        j = core::to_json(gray);
        j["inks"] = Json::array({Json::array({Json::array({1, 2, 3}), Json::array({0.5, 0.5})})});
        QVERIFY(refused(j));
        j = core::to_json(gray);
        j["mode"] = "indexed";  // (PC1d's)
        QVERIFY(refused(j));
    }

    // GKIR: what version 1 wrote is written to the same bytes (fixed hashes of the codec before color-model@1); version 2
    // (grey, multichannel, mask; 32-bit floats, a grey's HDR) back as it was, the same bytes each time; what is refused.
    void gkirVersions() {
        core::InkPixels a = core::blank_ink_pixels(core::InkSpace::Cmyk, 8, 37, 23);
        for (std::size_t i = 0; i < a.samples.size(); ++i) a.samples[i] = static_cast<std::uint16_t>((i * 37 + 11) % 256);
        core::InkPixels b = core::blank_ink_pixels(core::InkSpace::Lab, 16, 19, 7);
        for (std::size_t i = 0; i < b.samples.size(); ++i) b.samples[i] = static_cast<std::uint16_t>((i * 2654435761u) % 65536);
        QCOMPARE(core::bytes_ref(core::encode_ink_raster(a)), std::string("sha256:fcbc2c0bc70df5504ee468b1ae45057db94eef554469286c914c72dd67120be4"));
        QCOMPARE(core::bytes_ref(core::encode_ink_raster(b)), std::string("sha256:ae4d84bc99d5be88bec66b1822892cd3d1356f72c5edcd57c5c9841401f44cce"));
        QCOMPARE(core::ink_raster_header(core::encode_ink_raster(a)).version, 1u);
        struct Case {
            core::InkSpace space;
            int depth;
            unsigned planes;
        };
        for (const Case& c : {Case{core::InkSpace::Gray, 8, 0}, Case{core::InkSpace::Gray, 16, 0}, Case{core::InkSpace::Gray, 32, 0},
                              Case{core::InkSpace::Cmyk, 32, 0}, Case{core::InkSpace::Lab, 32, 0}, Case{core::InkSpace::Multi, 8, 5},
                              Case{core::InkSpace::Multi, 32, 2}, Case{core::InkSpace::Mask, 8, 0}, Case{core::InkSpace::Mask, 16, 0},
                              Case{core::InkSpace::Mask, 32, 0}}) {
            core::InkPixels p = core::blank_ink_pixels(c.space, c.depth, 13, 9, c.planes);
            const unsigned all = p.channels();
            QCOMPARE(all, (c.space == core::InkSpace::Multi ? c.planes : core::colour_channels(c.space)) + (c.space == core::InkSpace::Mask ? 0u : 1u));
            for (std::size_t i = 0; i < p.pixel_count(); ++i) {
                for (unsigned k = 0; k < all; ++k) p.set(i, k, std::fmod(i * 0.137 + k * 0.29, 1.0));
            }
            if (c.space == core::InkSpace::Gray && c.depth == 32) p.set(3, 0, 4.5);  // (a grey's HDR)
            const std::string bytes = core::encode_ink_raster(p);
            QCOMPARE(core::encode_ink_raster(p), bytes);
            const core::InkRasterHeader header = core::ink_raster_header(bytes);
            QCOMPARE(header.version, 2u);
            QVERIFY(header.space == c.space);
            QCOMPARE(header.depth, c.depth);
            const core::InkPixels back = core::decode_ink_raster(bytes);
            QCOMPARE(back.channels(), all);
            QVERIFY(back.samples == p.samples);
            QVERIFY(back.floats == p.floats);
            if (c.space == core::InkSpace::Gray && c.depth == 32) QCOMPARE(back.value(3, 0), 4.5);
            QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(bytes + "x"));
            std::string v1 = bytes;
            v1[4] = 1;  // (version 1 holds CMYK and Lab at 8 and 16 bits only)
            QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(v1));
            if (c.depth == 32) {  // (not finite; a CMYK value out of 0..1)
                core::InkPixels wrong = p;
                wrong.floats[0] = std::numeric_limits<float>::quiet_NaN();
                QVERIFY_THROWS_EXCEPTION(core::Error, core::encode_ink_raster(wrong));
                wrong.floats[0] = std::numeric_limits<float>::infinity();
                QVERIFY_THROWS_EXCEPTION(core::Error, core::encode_ink_raster(wrong));
                if (c.space != core::InkSpace::Gray) {
                    wrong.floats[0] = 1.5f;
                    QVERIFY_THROWS_EXCEPTION(core::Error, core::encode_ink_raster(wrong));
                }
                wrong = p;
                wrong.floats[all - 1] = 1.5f;  // (an alpha past 1; a mask's value)
                if (c.space != core::InkSpace::Gray || all > 1) QVERIFY_THROWS_EXCEPTION(core::Error, core::encode_ink_raster(wrong));
            }
        }
        // version 2 does not hold what version 1 does (the same pixels, the same bytes)
        std::string twice = core::encode_ink_raster(a);
        twice[4] = 2;
        twice[7] = 4;
        QVERIFY_THROWS_EXCEPTION(core::Error, core::decode_ink_raster(twice));
        // a multichannel raster of 0 or 33 planes
        QVERIFY_THROWS_EXCEPTION(core::Error, core::blank_ink_pixels(core::InkSpace::Multi, 8, 4, 4, 33));
        // blank: grey white, transparent
        const core::InkPixels white = core::blank_ink_pixels(core::InkSpace::Gray, 16, 2, 2);
        QCOMPARE(white.samples[0], std::uint16_t(65535));
        QCOMPARE(white.samples[1], std::uint16_t(0));
    }

    // set_color_mode by depth (image.mode.bits8/16/32): 8 → 16 is v×257 and 8 → 32 is v/255, sample by sample; back to
    // 8 bits the same PNG pixels; the page's mode follows (16, 32: color-model@1), 8 bits sRGB is none again. A page
    // whose layer cannot be drawn in precise colour yet is refused with its name (nothing changed).
    void depths() {
        core::Document doc = painted(picture());
        const std::string id = doc.page(0).layers.back().id;
        const std::string began = render::open_image(*doc.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        const core::ApplyResult deep = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        QCOMPARE(deep.results.at(0)["converted"], Json(1));
        const core::Page& p16 = deep.doc.page(0);
        QVERIFY(p16.rgb && p16.rgb->depth == 16 && p16.rgb->profile.empty());
        QVERIFY(!p16.layers.back().raster_png && p16.layers.back().color_raster);
        {
            const core::ColorRasterView view(*p16.layers.back().color_raster);
            QCOMPARE(view.width(), 60u);
            for (std::size_t i = 0; i < std::size_t(60) * 40; ++i) {
                const auto px = view.pixel(i);
                for (unsigned c = 0; c < 4; ++c) {
                    const int v = static_cast<unsigned char>(began[i * 4 + c]);
                    QCOMPARE(static_cast<int>(std::lround(px[c] * 65535)), v * 257);
                }
            }
        }
        const core::Document d32 = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}})).doc;
        {
            const core::ColorRasterView view(*d32.page(0).layers.back().color_raster);
            for (std::size_t i = 0; i < std::size_t(60) * 40; ++i) {
                const auto px = view.pixel(i);
                for (unsigned c = 0; c < 4; ++c) {
                    const int v = static_cast<unsigned char>(began[i * 4 + c]);
                    QCOMPARE(px[c], static_cast<double>(static_cast<float>(v / 255.0)));
                }
            }
        }
        // 16 → 32 → 8: the PNG's pixels again; the page none (Python's)
        const core::Document back = run_ops(deep.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}},
                                                                   Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        QVERIFY(!back.page(0).rgb && !back.page(0).color);
        QCOMPARE(render::open_image(*back.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(), began);
        // dithered: an 8-bit value stays itself
        const core::Document dithered = run_ops(deep.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"dither", true}}})).doc;
        QCOMPARE(render::open_image(*dithered.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(), began);
        // the same mode again: nothing converted
        QCOMPARE(run_ops(deep.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).results.at(0)["converted"], Json(0));
        // a layer that keeps its lines inside the panels cannot be precise yet: refused with its name, nothing changed
        const core::Document clipped = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"panel_clip", true}}})).doc;
        const std::string why = refusal(clipped, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        QVERIFY2(why.starts_with("not_yet_ported") && why.find(id) != std::string::npos, why.c_str());
        // refused: a depth, a mode
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "rgb"}, {"depth", 12}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "hsv"}}})).empty());
        QVERIFY(refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "multichannel"}}})).starts_with("not_yet_ported"));
    }

    // HDR values going where they cannot be kept (32 → 16 or 8 bits, into inks): refused, nothing changed; cut when
    // asked (clip_hdr), the pixels counted.
    void clipHdr() {
        core::Document doc = small_book();
        doc = run_ops(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
                                              {"pixels", Json::array({2.0, 0.5, 0.25, 1.0, 0.5, 0.5, 0.5, 1.0})}}})).doc;
        const std::string why = refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        QVERIFY2(why.find("clip_hdr") != std::string::npos, why.c_str());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).empty());
        const core::ApplyResult cut = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}, {"clip_hdr", true}}}));
        QCOMPARE(cut.results.at(0)["clipped"], Json(1));
        const core::ColorRasterView view(*cut.doc.page(0).layers.back().color_raster);
        QCOMPARE(view.pixel(0)[0], 1.0);
        QCOMPARE(view.pixel(1)[0], std::lround(0.5 * 65535) / 65535.0);
        // 32 bits keeps them
        const core::Document kept = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}})).doc;
        QCOMPARE(core::ColorRasterView(*kept.page(0).layers.back().color_raster).pixel(0)[0], 2.0);
    }

    // The book's precise pixels past its budget (1 GiB): the conversion is refused (nothing changed).
    void budget() {
        core::Document doc = small_book();
        render::Image big = render::Image::create("RGBA", render::Size{4096, 4096}, render::Ink{0, 0, 0, 0});
        big.paste(render::Ink{10, 20, 30, 255}, render::Box{0, 0, 8, 8});
        const auto png = std::make_shared<const std::string>(render::write_png(big));
        for (int i = 0; i < 5; ++i) {
            core::Layer layer;
            layer.id = "big" + std::to_string(i);
            layer.kind = core::LayerKind::Raster;
            layer.role = core::LayerRole::User;
            layer.panel_clip = false;
            layer.raster_png = png;
            doc.edit_page(0).layers.push_back(std::move(layer));
        }
        const std::string why = refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}}));
        QVERIFY2(why.find("1 GiB") != std::string::npos, why.c_str());
    }

    // assign_profile: the pixels as they are (byte for byte), the page's profile named; "none" is sRGB (an RGB page) or
    // sGray (a grey page); a profile of another space, a Lab page, none on a CMYK page: refused.
    void assignProfile() {
        core::Document doc = painted(picture());
        const core::Bytes png = doc.page(0).layers.back().raster_png;
        const core::ApplyResult p3 = run_ops(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}}));
        QCOMPARE(p3.results.at(0)["assigned"], Json(1));
        QVERIFY(p3.doc.page(0).rgb && p3.doc.page(0).rgb->profile == core::builtin_profile("display-p3")->ref() && p3.doc.page(0).rgb->depth == 8);
        QCOMPARE(*p3.doc.page(0).layers.back().raster_png, *png);
        const core::Document none = run_ops(p3.doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "none"}}})).doc;
        QVERIFY(!none.page(0).rgb);
        QCOMPARE(*none.page(0).layers.back().raster_png, *png);
        // a precise page: its GKCR bytes as they were
        const core::Document deep = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).doc;
        const core::Document deep_p3 = run_ops(deep, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "adobe-rgb-compat"}}})).doc;
        QCOMPARE(*deep_p3.page(0).layers.back().color_raster, *deep.page(0).layers.back().color_raster);
        QCOMPARE(deep_p3.page(0).rgb->depth, 16);
        // a grey page: another grey profile, none = sGray
        const core::Document grey = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        QCOMPARE(grey.page(0).color->profile, core::builtin_profile("sgray")->ref());
        const core::Document g22 = run_ops(grey, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "gray-gamma-2.2"}}})).doc;
        QCOMPARE(g22.page(0).color->profile, core::builtin_profile("gray-gamma-2.2")->ref());
        QCOMPARE(*g22.page(0).layers.back().ink_raster, *grey.page(0).layers.back().ink_raster);
        QCOMPARE(run_ops(g22, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "none"}}})).doc.page(0).color->profile,
                 core::builtin_profile("sgray")->ref());
        // refused
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "sgray"}}})).empty());
        QVERIFY(!refusal(grey, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).empty());
        const core::Document lab = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}}})).doc;
        QVERIFY(!refusal(lab, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "none"}}})).empty());
        const core::Document cmyk = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})).doc;
        QVERIFY(!refusal(cmyk, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "none"}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "nope"}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}}})).empty());
        // every page and the book's mode
        const core::Document all = run_ops(small_book(2), Json::array({Json{{"op", "assign_profile"}, {"profile", "rec2020"}}})).doc;
        QVERIFY(all.rgb && all.rgb->profile == core::builtin_profile("rec2020")->ref());
        QVERIFY(all.page(1).rgb && all.page(1).rgb->profile == all.rgb->profile);
    }

    // convert_profile: the pixels into the profile's space (its mode from it: RGB, grey, CMYK, Lab) at each page's depth;
    // sRGB → Display P3 → sRGB at 16 bits comes back within a step of 8 bits.
    void convertProfile() {
        core::Document doc = painted(picture());
        const std::string began = render::open_image(*doc.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        const core::ApplyResult p3 = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}}}));
        QCOMPARE(p3.results.at(0)["converted"], Json(1));
        QVERIFY(p3.doc.page(0).rgb && p3.doc.page(0).rgb->profile == core::builtin_profile("display-p3")->ref() && p3.doc.page(0).rgb->depth == 8);
        const std::string in_p3 = render::open_image(*p3.doc.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        QVERIFY(in_p3 != began);  // (the green's values in P3)
        const core::Document back = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                             Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}},
                                                             Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "srgb"}},
                                                             Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        QVERIFY(!back.page(0).rgb);
        const std::string again = render::open_image(*back.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        for (std::size_t i = 0; i < again.size(); ++i) {
            QVERIFY2(std::abs(int(static_cast<unsigned char>(again[i])) - int(static_cast<unsigned char>(began[i]))) <= 1, qPrintable(QString::number(i)));
        }
        // into CMYK and grey by the profile's space
        const core::Document cmyk = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "coated-cmyk"}}})).doc;
        QVERIFY(cmyk.page(0).color && cmyk.page(0).color->space == core::InkSpace::Cmyk);
        const core::Document grey = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "sgray"}}})).doc;
        QVERIFY(grey.page(0).color && grey.page(0).color->space == core::InkSpace::Gray);
        const core::Document lab = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "lab-d50"}}})).doc;
        QVERIFY(lab.page(0).color && lab.page(0).color->space == core::InkSpace::Lab);
        // at the page's depth
        const core::Document deep = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                             Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "rec2020"}}})).doc;
        QVERIFY(deep.page(0).rgb && deep.page(0).rgb->depth == 16 && deep.page(0).rgb->profile == core::builtin_profile("rec2020")->ref());
        QVERIFY(deep.page(0).layers.back().color_raster);
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}}})).empty());
    }

    // The conversion's way (intent, bpc) and what the page keeps as its own (the way the colours drawn come into it) can be
    // given apart: page_intent, page_bpc (without them, the same as the conversion's). A CMYK page taken to an 8-bit sRGB
    // page without black point compensation, keeping the page's defaults, is a page without a colour model (Python's).
    // convert_profile and assign_profile take the book's default too when asked (default: true), as set_color_mode does.
    void theConversionsWayIsNotThePages() {
        render::Image image = picture();
        image.paste(render::Ink{0, 0, 0, 255}, render::Box{0, 0, 10, 10});
        const core::Document cmyk = run_ops(painted(image), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})).doc;
        const core::Document own = run_ops(cmyk, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"intent", "perceptual"}, {"bpc", false},
                                                                    {"page_intent", "relative"}, {"page_bpc", true}}})).doc;
        QVERIFY(!own.page(0).color && !own.page(0).rgb);
        const core::Document plain = run_ops(cmyk, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        QVERIFY(*own.page(0).layers.back().raster_png != *plain.page(0).layers.back().raster_png);  // (converted without black point compensation)
        const core::Document both = run_ops(cmyk, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"intent", "perceptual"}, {"bpc", false}}})).doc;
        QVERIFY(both.page(0).rgb && both.page(0).rgb->intent == "perceptual" && !both.page(0).rgb->bpc);
        QCOMPARE(*both.page(0).layers.back().raster_png, *own.page(0).layers.back().raster_png);
        const core::Document grey = run_ops(cmyk, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "sgray"}, {"intent", "saturation"},
                                                                     {"page_intent", "perceptual"}, {"page_bpc", false}}})).doc;
        QVERIFY(grey.page(0).color && grey.page(0).color->intent == "perceptual" && !grey.page(0).color->bpc);
        QVERIFY(!refusal(cmyk, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"page_intent", "nope"}}})).empty());
        QVERIFY(!refusal(cmyk, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "srgb"}, {"page_bpc", 1}}})).empty());
        // the book's default with one page
        const core::Document p3 = run_ops(small_book(2), Json::array({Json{{"op", "assign_profile"}, {"profile", "display-p3"}}})).doc;
        QVERIFY(p3.rgb && p3.page(0).rgb && p3.page(1).rgb);
        const core::Document converted = run_ops(p3, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "srgb"}, {"default", true}}})).doc;
        QVERIFY(!converted.rgb && !converted.page(0).rgb && converted.page(1).rgb);
        const core::Document assigned = run_ops(p3, Json::array({Json{{"op", "assign_profile"}, {"page", 2}, {"profile", "rec2020"}, {"default", true}}})).doc;
        QVERIFY(assigned.rgb && assigned.rgb->profile == core::builtin_profile("rec2020")->ref());
        QVERIFY(assigned.page(0).rgb->profile == core::builtin_profile("display-p3")->ref());
        const core::Document kept = run_ops(p3, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "srgb"}}})).doc;
        QVERIFY(kept.rgb);  // (without it: the pages alone)
        QVERIFY(!refusal(p3, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "srgb"}, {"default", "yes"}}})).empty());
    }

    // A grey page (image.mode.grayscale): sGray's greys by their value both ways (the picture's grey 128 is lightness
    // 128 and comes back (128, 128, 128)), a colour by its lightness; 16 and 32 bits; gray "k" on another profile.
    void greyPages() {
        core::Document doc = painted(picture());
        for (const int depth : {8, 16, 32}) {
            const core::Document grey = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", depth}}})).doc;
            const core::Page& page = grey.page(0);
            QVERIFY(page.color && page.color->space == core::InkSpace::Gray && page.color->depth == depth);
            const core::InkPixels pixels = core::decode_ink_raster(*page.layers.back().ink_raster);
            QCOMPARE(pixels.depth, depth);
            QCOMPARE(pixels.value(5 * 60 + 5, 0), depth == 32 ? static_cast<double>(static_cast<float>(128 / 255.0))
                                                              : depth == 16 ? 128 * 257 / 65535.0 : 128 / 255.0);
            QCOMPARE(pixels.value(5 * 60 + 40, 1), depth == 32 ? static_cast<double>(static_cast<float>(200 / 255.0))
                                                               : depth == 16 ? 200 * 257 / 65535.0 : 200 / 255.0);
            const core::Document back = run_ops(grey, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
            const std::string rgba = render::open_image(*back.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
            const auto* p = reinterpret_cast<const unsigned char*>(rgba.data()) + (5 * 60 + 5) * 4;
            QCOMPARE(int(p[0]), 128);
            QCOMPARE(int(p[1]), 128);
            QCOMPARE(int(p[2]), 128);
            const auto* q = reinterpret_cast<const unsigned char*>(rgba.data()) + (5 * 60 + 40) * 4;
            QVERIFY(q[0] == q[1] && q[1] == q[2]);  // (the colour: a grey of its lightness)
            QCOMPARE(int(q[3]), 200);
        }
        // gray "k" on Gray Gamma 2.2: the greys by their value all the same
        const core::Document g22 = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"profile", "gray-gamma-2.2"}, {"gray", "k"}}})).doc;
        QCOMPARE(core::decode_ink_raster(*g22.page(0).layers.back().ink_raster).samples[(5 * 60 + 5) * 2], std::uint16_t(128));
        // a grey page's element colours: Genko's black 0, white 1, another grey its value, the table first
        core::PageColor color = *g22.page(0).color;
        QCOMPARE(render::ink::ink_of(color, {20, 20, 20})[0], 1.0);
        QCOMPARE(render::ink::ink_of(color, {255, 255, 255})[0], 0.0);
        QCOMPARE(render::ink::ink_of(color, {51, 51, 51})[0], 1.0 - 51 / 255.0);
        color.inks.push_back(core::InkEntry{{51, 51, 51}, {0.5}});
        QCOMPARE(render::ink::ink_of(color, {51, 51, 51})[0], 0.5);
        // to CMYK on K alone (gray "k"): K = 1 − lightness
        const core::Document grey = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const core::Document k = run_ops(grey, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"gray", "k"}}})).doc;
        const core::InkPixels on_k = core::decode_ink_raster(*k.page(0).layers.back().ink_raster);
        const std::uint16_t* s = on_k.samples.data() + (5 * 60 + 5) * 5;
        QCOMPARE(s[0] + s[1] + s[2], 0);
        QCOMPARE(int(s[3]), 255 - 128);
    }

    // The precise canvas's sRGB path is as it was before RgbSpace (its fixed pictures' hashes, taken from the canvas of
    // the commit before: what it shows and the rasters it makes).
    void srgbCanvasUnchanged() {
        const render::Size size{37, 23};
        std::string paper(std::size_t(37) * 23 * 4, '\0');
        for (std::size_t i = 0; i < paper.size(); ++i) paper[i] = static_cast<char>((i * 37 + 11) % 256);
        for (std::size_t i = 3; i < paper.size(); i += 4) paper[i] = static_cast<char>(255);
        render::ColorCanvas canvas(render::Image::frombytes("RGBA", size, paper));
        QVERIFY(canvas.space().is_srgb());
        Json pixels = Json::array();
        for (int i = 0; i < 19 * 11 * 4; ++i) pixels.push_back((i * 2654435761u) % 65536);
        for (int i = 3; i < 19 * 11 * 4; i += 4) pixels[i] = (i * 977) % 65536;
        const std::string raster = core::encode_color_raster(Json{{"precision", "u16"}, {"width", 19}, {"height", 11}, {"pixels", pixels}});
        const core::ColorRasterView view(raster);
        const render::Box all{0, 0, 37, 23};
        for (const char* mode : {"normal", "multiply", "screen", "soft_light", "hue", "luminosity", "difference", "color_dodge"}) {
            canvas.blend(view, size, all, 0.7, false, mode);
        }
        std::string mask(std::size_t(37) * 23, '\0');
        for (std::size_t i = 0; i < mask.size(); ++i) mask[i] = static_cast<char>((i * 13) % 256);
        canvas.blend_stroke(render::Image::frombytes("L", size, mask), Json{{"precision", "f32"}, {"values", Json::array({0.2, 0.5, 0.9})}}, 0.8);
        std::string over(std::size_t(37) * 23 * 4, '\0');
        for (std::size_t i = 0; i < over.size(); ++i) over[i] = static_cast<char>((i * 7 + 3) % 256);
        canvas.blend(render::Image::frombytes("RGBA", size, over), 0.5, true, "overlay");
        const render::Image shown = render::Image::frombytes("L", size, mask);
        canvas.adjust(core::PreciseAdjustment("brightness_contrast", Json{{"brightness", 15}, {"contrast", 10}}), 0.9, false, &shown);
        canvas.expose(core::Exposure::parse(Json{{"kind", "exposure"}, {"exposure", 0.5}, {"offset", 0.01}, {"gamma", 1.1}}), 0.6, true);
        canvas.draw_8bit([](render::Image& picture) { picture.paste(render::Ink{200, 40, 40, 255}, render::Box{3, 4, 12, 9}); });
        QCOMPARE(core::bytes_ref(canvas.image().tobytes()), std::string("sha256:6c457a9d268976b1c49943f32a9e8191c7b21a0d8c8bcc53eab5eb6ac4141928"));
        QCOMPARE(core::bytes_ref(canvas.color_raster("u16")), std::string("sha256:794f902e5edb47769de42dca6e914f0a2d2e0e19336175e598427216d54b5c5f"));
        QCOMPARE(core::bytes_ref(canvas.color_raster("f32")), std::string("sha256:c8e667dcd329c9f96972f8f48e321822dc69e225a10125c9045c67428dd6fc88"));
    }

    // An RGB page of a profile (image.mode.rgb, edit.assignProfile): drawn in its space (ColorCanvas with RgbSpace) —
    // its paint layer's values shown through the profile, a grey of a profile with sRGB's curve as itself, sRGB elements
    // (a line) as their own colour; the page in parts the same as whole; a linear profile's 0.5 shown as sRGB's 188.
    void profiledPages() {
        core::Document doc = painted(picture());
        doc = run_ops(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                             {"points", Json::array({Json::array({5, 70}), Json::array({50, 70})})}, {"width_mm", 3}, {"rgb", Json::array({200, 40, 40})}}})).doc;
        const int dpi = 100;
        const auto draw_part = [&](const core::Document& book, const render::RenderRegion* region) {
            render::RenderOptions options;
            if (region != nullptr) options.region = *region;
            return render::render_page(book.page(0), dpi, options, &book).image;
        };
        const auto draw = [&](const core::Document& book) { return draw_part(book, nullptr); };
        const render::Image plain = draw(doc);
        const core::Document p3 = run_ops(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const render::Image shown = draw(p3);
        QCOMPARE(shown.size(), plain.size());
        const auto at = [](const render::Image& image, int x, int y) {
            const std::string b = image.crop(render::Box{x, y, x + 1, y + 1}).convert("RGB").tobytes();
            return std::array<int, 3>{static_cast<unsigned char>(b[0]), static_cast<unsigned char>(b[1]), static_cast<unsigned char>(b[2])};
        };
        // (the picture lies stretched over the page: its grey on the left, its green on the right)
        const int w = shown.width(), h = shown.height();
        const auto grey = at(shown, w / 4, h / 4), was_grey = at(plain, w / 4, h / 4);
        for (unsigned c = 0; c < 3; ++c) QVERIFY(std::abs(grey[c] - was_grey[c]) <= 1);
        QVERIFY(at(shown, 3 * w / 4, h / 4) != at(plain, 3 * w / 4, h / 4));  // (the green's values as P3's)
        // (a line alone on a page of the profile: its sRGB colour, where it covers all)
        {
            core::Document lined = run_ops(small_book(), Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                                          {"points", Json::array({Json::array({5, 70}), Json::array({50, 70})})},
                                                                          {"width_mm", 3}, {"rgb", Json::array({200, 40, 40})}}})).doc;
            const int line_y = static_cast<int>(70 / 25.4 * dpi);
            const auto was_line = at(draw(lined), w / 2, line_y);
            lined = run_ops(lined, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "adobe-rgb-compat"}}})).doc;
            const auto line = at(draw(lined), w / 2, line_y);
            for (unsigned c = 0; c < 3; ++c) QVERIFY2(std::abs(line[c] - was_line[c]) <= 1, "an sRGB line keeps its colour");
        }
        const render::RenderRegion part{7, 9, 50, 60};
        QCOMPARE(draw_part(p3, &part).tobytes(), shown.crop(render::Box{7, 9, 57, 69}).tobytes());
        // linear sRGB: a value of 0.5 is sRGB's 188
        render::Image half = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{128, 128, 128, 255});
        const core::Document linear = run_ops(painted(half), Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "linear-srgb"}}})).doc;
        const auto lit = at(draw(linear), w / 2, h / 4);
        for (unsigned c = 0; c < 3; ++c) QVERIFY2(std::abs(lit[c] - 188) <= 1, qPrintable(QString::number(lit[c])));
        // 16 bits of the profile: the same picture
        const core::Document deep = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                             Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const render::Image deep_shown = draw(deep);
        for (const int x : {w / 4, 3 * w / 4}) {
            const auto a = at(deep_shown, x, h / 4), b = at(shown, x, h / 4);
            for (unsigned c = 0; c < 3; ++c) QVERIFY(std::abs(a[c] - b[c]) <= 1);
        }
        // a fill on its paint layer: sRGB's colour into the page's values (shown as itself)
        const std::string id = p3.page(0).layers.back().id;
        const Json area{{"poly", Json::array({Json::array({0, 60}), Json::array({30, 60}), Json::array({30, 80}), Json::array({0, 80})})}};
        const core::Document filled = run_ops(p3, Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", id}, {"area", area},
                                                               {"rgb", Json::array({40, 90, 200})}}})).doc;
        QVERIFY(!filled.page(0).layers.back().patches.empty());  // (a fill laid on it: drawn as the page's values)
        const auto blue = at(draw(filled), w / 6, static_cast<int>(70 / 25.4 * dpi));
        QVERIFY2(std::abs(blue[0] - 40) <= 1 && std::abs(blue[1] - 90) <= 1 && std::abs(blue[2] - 200) <= 1,
                 qPrintable(QString("%1 %2 %3").arg(blue[0]).arg(blue[1]).arg(blue[2])));
    }

    // A grey page drawn (InkCanvas in one plane: its lightness): shown as sGray's numerical grey, its blends, in parts as
    // whole; its inks at 32 bits as floats.
    void greyCanvas() {
        core::Document doc = painted(picture());
        doc = run_ops(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                             {"points", Json::array({Json::array({5, 70}), Json::array({50, 70})})}, {"width_mm", 3}},
                                        Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 32}}})).doc;
        const std::string id = doc.page(0).layers.back().id;
        doc = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"blend", "multiply"}}})).doc;
        render::RenderOptions options;
        options.inks = true;
        const int dpi = 100;
        const render::RenderResult whole = render::render_page(doc.page(0), dpi, options, &doc);
        QVERIFY(whole.inks && whole.inks->depth == 32 && whole.inks->space == core::InkSpace::Gray && !whole.inks->floats.empty());
        const std::string rgb = whole.image.convert("RGB").tobytes();
        for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) QVERIFY(rgb[i] == rgb[i + 1] && rgb[i + 1] == rgb[i + 2]);  // (greys)
        // the grey picture's 128 multiplied over white paper: 128 (its lightness)
        const int w = whole.image.width(), h = whole.image.height();
        QCOMPARE(int(static_cast<unsigned char>(rgb[(std::size_t(h / 4) * w + w / 4) * 3])), 128);
        QCOMPARE(whole.inks->value(std::size_t(h / 4) * w + w / 4, 0), static_cast<double>(static_cast<float>(128 / 255.0)));
        // the line: Genko's black, lightness 0
        QCOMPARE(int(static_cast<unsigned char>(rgb[(std::size_t(70 / 25.4 * dpi) * w + w / 2) * 3])), 0);
        options.region = render::RenderRegion{3, 5, 70, 80};
        const render::RenderResult part = render::render_page(doc.page(0), dpi, options, &doc);
        QCOMPARE(part.image.tobytes(), whole.image.crop(render::Box{3, 5, 73, 85}).tobytes());
        QVERIFY(part.inks->floats == render::crop_inks(*whole.inks, render::Box{3, 5, 73, 85}).floats);
    }

    // document.inspect: the snapshot of a page with a colour model holds its `color` (as the book keeps it, and its
    // profile's description) and each layer's precision; an RGB book's snapshot is as it was (none of them).
    // document.pixel: a layer's own pixel (its values in the page's space, its samples, alpha, the screen's colour) and
    // the page's as drawn.
    void inspectAndPixel() {
        const core::Document plain = painted(picture());
        const Json before = storage::snapshot(plain, false);
        QVERIFY(!before["pages"][0].contains("color"));
        for (const Json& layer : before["pages"][0]["layers"]) QVERIFY(!layer.contains("precision"));
        const core::Document grey = run_ops(plain, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 16}}})).doc;
        const Json seen = storage::snapshot(grey, false);
        QCOMPARE(seen["pages"][0]["color"]["mode"], Json("gray"));
        QCOMPARE(seen["pages"][0]["color"]["profile_description"], Json("sGray (sRGB tone curve, Photocraft)"));
        QCOMPARE(seen["pages"][0]["layers"].back()["precision"], Json(16));
        const core::Document p3 = run_ops(plain, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}, {"profile", "display-p3"}}})).doc;
        const Json deep = storage::snapshot(p3, false);
        QCOMPARE(deep["pages"][0]["color"]["profile_description"], Json("Display P3"));
        QCOMPARE(deep["pages"][0]["layers"].back()["precision"], Json("f32"));
        QCOMPARE(core::icc_description(*core::builtin_profile("coated-cmyk")->bytes), std::string(core::builtin_profile("coated-cmyk")->description));
        QCOMPARE(core::icc_description(*core::builtin_profile("rec2020")->bytes), std::string("Rec. ITU-R BT.2020 (Photocraft)"));
        // a layer's pixel: the grey 128 on the left (lightness 128, k = 1 − 128/255)
        const std::string id = grey.page(0).layers.back().id;
        const Json px = render::pixel_at(grey.page(0), &grey, 10, 20, id, 100);
        QCOMPARE(px["mode"], Json("gray"));
        QCOMPARE(px["depth"], Json(16));
        QCOMPARE(px["stored"], Json::array({128 * 257}));
        QVERIFY(std::abs(px["values"][0].get<double>() - (1 - 128 / 255.0)) < 1e-9);
        QCOMPARE(px["alpha"], Json(1.0));
        QCOMPARE(px["srgb"], Json::array({128, 128, 128}));
        // the page as drawn there
        const Json drawn = render::pixel_at(grey.page(0), &grey, 10, 20, std::nullopt, 100);
        QCOMPARE(drawn["srgb"], Json::array({128, 128, 128}));
        QCOMPARE(drawn["stored"], Json::array({128 * 257}));
        // a precise RGB page of P3: the page's values (f32) and the screen's colour
        const Json f32 = render::pixel_at(p3.page(0), &p3, 50, 20, p3.page(0).layers.back().id, 100);
        QCOMPARE(f32["precision"], Json("f32"));
        QCOMPARE(f32["alpha"].get<double>(), static_cast<double>(static_cast<float>(200 / 255.0)));
        const Json composite = render::pixel_at(p3.page(0), &p3, 10, 20, std::nullopt, 100);
        QCOMPARE(composite["srgb"], Json::array({128, 128, 128}));
        QVERIFY(composite["stored"][0].is_number_float());
        // off the page, an unknown layer
        QVERIFY_THROWS_EXCEPTION(core::Error, render::pixel_at(grey.page(0), &grey, 100, 20, std::nullopt, 100));
        QVERIFY_THROWS_EXCEPTION(core::Error, render::pixel_at(grey.page(0), &grey, 10, 20, std::string("nope"), 100));
    }

    // The book on disk (color-model@1): an RGB page of a profile and 16 bits, a grey page, the book's RGB mode; the
    // profiles as assets; color-mode@1 and color-model@1 in `features`; read back the same. A page of the default RGB
    // mode writes no `color` and the book no color-model@1.
    void onDisk() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8(tmp.filePath(QStringLiteral("book.genko")).toStdString());
        core::Document doc = small_book(3);
        const core::BuiltinProfile* p3 = core::builtin_profile("display-p3");
        const core::BuiltinProfile* sgray = core::builtin_profile("sgray");
        write_book(doc, dir);
        {
            const Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json"))));
            QVERIFY(payload["features"].empty());
        }
        core::RgbMode rgb;
        rgb.depth = 16;
        rgb.profile = p3->ref();
        rgb.icc = p3->bytes;
        doc.edit_page(0).rgb = rgb;
        core::PageColor gray;
        gray.space = core::InkSpace::Gray;
        gray.profile = sgray->ref();
        gray.icc = sgray->bytes;
        doc.edit_page(1).color = gray;
        core::RgbMode book;
        book.depth = 32;
        doc.rgb = book;
        save_next(doc, dir, Json::array());
        const Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json"))));
        QCOMPARE(payload["features"], Json::array({"color-mode@1", "color-model@1"}));
        QCOMPARE(payload["pages"][0]["color"].dump(), core::to_json(rgb).dump());
        QCOMPARE(payload["pages"][1]["color"].dump(), core::to_json(gray).dump());
        QVERIFY(!payload["pages"][2].contains("color"));
        QCOMPARE(payload["color"].dump(), std::string(R"({"mode":"rgb","depth":32,"intent":"relative"})"));
        QVERIFY(fs::exists(dir / storage::AssetStore::relpath(p3->ref(), core::kProfileSuffix)));
        QVERIFY(fs::exists(dir / storage::AssetStore::relpath(sgray->ref(), core::kProfileSuffix)));
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(loaded.document.page(0).rgb && *loaded.document.page(0).rgb == rgb && !loaded.document.page(0).color);
        QCOMPARE(*loaded.document.page(0).rgb->icc, *p3->bytes);
        QVERIFY(loaded.document.page(1).color && *loaded.document.page(1).color == gray);
        QCOMPARE(*loaded.document.page(1).color->icc, *sgray->bytes);
        QVERIFY(!loaded.document.page(2).rgb && !loaded.document.page(2).color);
        QVERIFY(loaded.document.rgb && *loaded.document.rgb == book);
        QVERIFY(storage::is_known_feature(core::kColorModelFeature));
        // a grey page's profile that is an RGB one: read-only, the mode kept (never drawn with another)
        {
            Json changed = payload;
            changed["pages"][1]["color"]["profile"] = p3->ref();
            QFile file(QString::fromStdString(core::path_to_utf8(dir / "project.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(changed.dump()));
        }
        const auto wrong = storage::load_document(dir);
        QVERIFY(!wrong.document.read_only_reason.empty());
        QVERIFY(wrong.document.page(1).color && !wrong.document.page(1).color->icc);
    }

    // The exports (COLOR-03, design 4.7): a grey page of 32 bits, an RGB page of Display P3 and 16 bits, a CMYK page of 32
    // bits. PSD: Grayscale 32 (floats: the page's lightness), RGB 16 (its values) with the profiles as resource 1039,
    // CMYK at 16; TIFF: grey MinIsBlack floats (SampleFormat 3), RGB 16 with the profile, CMYK at 16; PDF: ICCBased grey
    // (N 1) and RGB (N 3), 16 bits a channel at most. The notes: 32 bits at 16, pages from the screen's colours.
    void exports() {
        QTemporaryDir tmp;
        const fs::path out = core::path_from_utf8(tmp.path().toStdString());
        core::Document doc = small_book(3);
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"title", "絵"}, {"id", "pic"}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                        Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture()))}},
                                        Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 32}},
                                        Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}, {"profile", "display-p3"}},
                                        Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"depth", 32}, {"profile", "coated-cmyk"}}}))
                  .doc;
        const int dpi = 40;
        const std::string sgray = *core::builtin_profile("sgray")->bytes;
        const std::string p3 = *core::builtin_profile("display-p3")->bytes;
        const std::string coated = *core::builtin_profile("coated-cmyk")->bytes;
        const auto be16 = [](const std::string& b, std::size_t at) { return unsigned(static_cast<unsigned char>(b[at])) << 8 | static_cast<unsigned char>(b[at + 1]); };
        const auto be32 = [&](const std::string& b, std::size_t at) { return std::uint32_t(be16(b, at)) << 16 | be16(b, at + 2); };
        // PSD: (channels, depth, mode), resource 1039, the merged picture's start
        struct Psd {
            unsigned channels, depth, mode;
            std::string icc;
            std::size_t merged;  // (after the compression)
            unsigned compression;
        };
        const auto psd = [&](const fs::path& file) {
            const std::string b = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(file)));
            Psd p{be16(b, 12), be16(b, 22), be16(b, 24), {}, 0, 0};
            std::size_t at = 26;
            at += 4 + be32(b, at);  // (colour mode data)
            const std::size_t resources = be32(b, at);
            std::size_t r = at + 4;
            while (r < at + 4 + resources) {
                const unsigned id = be16(b, r + 4);
                const std::size_t name = static_cast<unsigned char>(b[r + 6]);
                std::size_t data = r + 6 + ((name + 2) & ~std::size_t(1));
                const std::size_t size = be32(b, data);
                if (id == 1039) p.icc = b.substr(data + 4, size);
                r = data + 4 + ((size + 1) & ~std::size_t(1));
            }
            at += 4 + resources;
            at += 4 + be32(b, at);  // (layers and masks)
            p.compression = be16(b, at);
            p.merged = at + 2;
            return p;
        };
        const auto psds = formats::export_psd_pages(doc, out / "psd", dpi);
        QCOMPARE(psds.size(), std::size_t{3});
        {
            const Psd grey = psd(psds[0]);
            QCOMPARE(grey.channels, 1u);
            QCOMPARE(grey.depth, 32u);
            QCOMPARE(grey.mode, 1u);
            QVERIFY(grey.icc == sgray);
            QCOMPARE(grey.compression, 0u);
            // (its lightness, as the page's inks are: the grey 128 on the left)
            render::RenderOptions options;
            options.mode = "print";
            options.inks = true;
            const auto made = render::render_page(doc.page(0), dpi, options, &doc);
            const std::string b = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(psds[0])));
            const std::size_t i = std::size_t(render::mm_to_px(20, dpi)) * made.inks->width + render::mm_to_px(10, dpi);
            const float v = std::bit_cast<float>(be32(b, grey.merged + i * 4));
            QCOMPARE(v, made.inks->floats[i * made.inks->channels()]);
            QVERIFY(std::abs(v - 128 / 255.0) < 0.01);
        }
        {
            const Psd rgb = psd(psds[1]);
            QCOMPARE(rgb.channels, 3u);
            QCOMPARE(rgb.depth, 16u);
            QCOMPARE(rgb.mode, 3u);
            QVERIFY(rgb.icc == p3);
            QCOMPARE(rgb.compression, 0u);
            // (its values: the paper white is 1 in every channel)
            const std::string b = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(psds[1])));
            QCOMPARE(be16(b, rgb.merged), 65535u);
        }
        {
            const Psd cmyk = psd(psds[2]);
            QCOMPARE(cmyk.channels, 4u);
            QCOMPARE(cmyk.depth, 16u);
            QCOMPARE(cmyk.mode, 4u);
            QVERIFY(cmyk.icc == coated);
        }
        QCOMPARE(formats::export_notes(doc, "psd", "auto"), std::vector<std::string>{"32 bit のページ 1 ページは、この形式では 16 bit にして書き出しました"});
        // TIFF: its tags (little-endian: libtiff's on this machine)
        const auto tiff_tags = [](const fs::path& file) {
            const std::string b = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(file)));
            const auto le = [&](std::size_t at, int n) {
                std::uint32_t v = 0;
                for (int k = n - 1; k >= 0; --k) v = v << 8 | static_cast<unsigned char>(b[at + k]);
                return v;
            };
            std::map<unsigned, std::uint32_t> tags;
            if (b.substr(0, 4) != std::string("II*\0", 4)) return tags;
            const std::uint32_t ifd = le(4, 4);
            for (unsigned e = 0; e < le(ifd, 2); ++e) {
                const std::size_t at = ifd + 2 + e * 12;
                const unsigned tag = le(at, 2), type = le(at + 2, 2);
                const std::uint32_t count = le(at + 4, 4);
                std::uint32_t value = type == 3 && count == 1 ? le(at + 8, 2) : le(at + 8, 4);
                if (type == 3 && count > 1) value = le(le(at + 8, 4), 2);  // (the first of them)
                tags[tag] = tag == 34675 ? count : value;
            }
            return tags;
        };
        const auto tiffs = formats::export_print(doc, out / "tiff", "tiff", dpi);
        QCOMPARE(tiffs.size(), std::size_t{3});
        {
            auto t = tiff_tags(tiffs[0]);
            QCOMPARE(t[258], 32u);    // BitsPerSample
            QCOMPARE(t[262], 1u);     // MinIsBlack
            QCOMPARE(t[277], 1u);     // SamplesPerPixel
            QCOMPARE(t[339], 3u);     // SampleFormat: IEEE floats
            QCOMPARE(t[34675], std::uint32_t(sgray.size()));
            t = tiff_tags(tiffs[1]);
            QCOMPARE(t[258], 16u);
            QCOMPARE(t[262], 2u);  // RGB
            QCOMPARE(t[277], 3u);
            QVERIFY(!t.contains(339));
            QCOMPARE(t[34675], std::uint32_t(p3.size()));
            t = tiff_tags(tiffs[2]);
            QCOMPARE(t[258], 16u);
            QCOMPARE(t[262], 5u);  // separated
            QCOMPARE(t[34675], std::uint32_t(coated.size()));
        }
        QCOMPARE(formats::export_notes(doc, "tiff", "auto"), std::vector<std::string>{"32 bit のページ 1 ページは、この形式では 16 bit にして書き出しました"});
        // PDF: every page by its values or inks, 16 bits a channel
        const auto pdfs = formats::export_print(doc, out / "pdf", "pdf", dpi);
        QCOMPARE(pdfs.size(), std::size_t{1});
        const std::string pdf = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(pdfs[0])));
        QVERIFY(pdf.find("/N 1 /Filter") != std::string::npos);
        QVERIFY(pdf.find("/N 3 /Filter") != std::string::npos);
        QVERIFY(pdf.find("/N 4 /Filter") != std::string::npos);
        std::size_t images = 0;
        for (std::size_t at = pdf.find("/BitsPerComponent "); at != std::string::npos; at = pdf.find("/BitsPerComponent ", at + 1)) {
            QCOMPARE(pdf.substr(at, 21), std::string("/BitsPerComponent 16 "));
            ++images;
        }
        QCOMPARE(images, std::size_t{3});
        QCOMPARE(formats::export_notes(doc, "pdf", "auto"), std::vector<std::string>{"32 bit のページ 2 ページは、この形式では 16 bit にして書き出しました"});
        // the formats of the screen's colours: said
        QCOMPARE(formats::export_notes(doc, "png", "auto"),
                 std::vector<std::string>{"色モード・プロファイルを持つページ 3 ページは、この形式・色ではその値のまま書けないため、画面の色（sRGB）から変換して書き出しました"});
        QCOMPARE(formats::export_notes(doc, "tiff", "bitonal").size(), std::size_t{1});
        QVERIFY(formats::ink_note(doc.page(1)));
        // a grey page of 8 bits: its TIFF of 8 bits with its profile, a PNG from the screen
        const core::Document grey8 = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const auto grey_tiff = formats::export_print(grey8, out / "grey8", "tiff", dpi);
        auto t = tiff_tags(grey_tiff[0]);
        QCOMPARE(t[258], 8u);
        QCOMPARE(t[262], 1u);
        QVERIFY(formats::export_notes(grey8, "tiff", "auto").empty());
        QVERIFY(formats::export_notes(grey8, "tiff", "gray").empty());
    }
};

QTEST_GUILESS_MAIN(TestColorModel)
#include "test_color_model.moc"
