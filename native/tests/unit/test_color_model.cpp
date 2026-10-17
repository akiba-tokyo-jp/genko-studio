// color-model@1 (SPEC.md COLOR-03, schema-v4.md「色モデル（color-model@1）」): the built-in ICC profiles (their bytes
// against their sha256, what Little CMS reads them as, PhotoCraft's ids and aliases).

#include <QtTest>

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <lcms2.h>
#include <malloc.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/base64.hpp"
#include "core/channels.hpp"
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
#include "render/selection.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

// What the program holds through operator new now and at most since reset_peak (review F, O: a refusal before the work
// allocates nothing large).
namespace allocations {
std::atomic<std::int64_t> live{0}, top{0};
void note(std::int64_t n) {
    const std::int64_t now = live.fetch_add(n) + n;
    std::int64_t seen = top.load();
    while (now > seen && !top.compare_exchange_weak(seen, now)) {
    }
}
std::int64_t reset_peak() {
    const std::int64_t now = live.load();
    top.store(now);
    return now;
}
std::int64_t peak() { return top.load(); }
}  // namespace allocations

void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    allocations::note(static_cast<std::int64_t>(malloc_usable_size(p)));
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
// (the nothrow forms too: what the libraries take with them is given back through this delete — test_image_split_oom's way)
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { return ::operator new(n, tag); }
void operator delete(void* p) noexcept {
    if (p == nullptr) return;
    allocations::note(-static_cast<std::int64_t>(malloc_usable_size(p)));
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

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

// The page drawn (proof, 100 dpi) and one of its pixels.
render::Image drawn_page(const core::Document& doc, int page = 0, int dpi = 100) {
    render::RenderOptions options;
    return render::render_page(doc.page(page), dpi, options, &doc).image.convert("RGB");
}
std::array<int, 3> rgb_at(const render::Image& image, int x, int y) {
    const std::string b = image.crop(render::Box{x, y, x + 1, y + 1}).convert("RGB").tobytes();
    return {static_cast<unsigned char>(b[0]), static_cast<unsigned char>(b[1]), static_cast<unsigned char>(b[2])};
}
bool near_rgb(const std::array<int, 3>& a, const std::array<int, 3>& b, int most) {
    for (unsigned c = 0; c < 3; ++c) {
        if (std::abs(a[c] - b[c]) > most) return false;
    }
    return true;
}
QString rgb_text(const std::array<int, 3>& a) { return QStringLiteral("(%1, %2, %3)").arg(a[0]).arg(a[1]).arg(a[2]); }

// An opaque picture at the paint layers' working resolution (200 dpi over the 60 × 80 mm page): a grey (128) on the
// left, a green on the right.
render::Image working_picture(render::Ink green = render::Ink{30, 160, 90, 255}) {
    const int w = render::mm_to_px(60, 200), h = render::mm_to_px(80, 200);
    render::Image out = render::Image::create("RGBA", render::Size{w, h}, render::Ink{128, 128, 128, 255});
    out.paste(green, render::Box{w / 2, 0, w, h});
    return out;
}

// A layer of the page by its id.
const core::Layer& layer_named(const core::Document& doc, const std::string& id, int page = 0) {
    for (const core::Layer& layer : doc.page(page).layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + id);
}

// The first layer of a role on the page.
const core::Layer& role_layer(const core::Document& doc, core::LayerRole role, int page = 0) {
    for (const core::Layer& layer : doc.page(page).layers) {
        if (layer.role == role) return layer;
    }
    throw std::runtime_error("no layer of the role");
}

// The layer a flatten (merge_visible's) leaves the first page's pixels in: its first shown layer.
std::string flat_id(const core::Document& doc) { return render::first_shown_layer(doc.page(0)).value(); }

// A layer of lines as a v2 book made it: its raster (a red band at the bottom of the page) and no paint kind.
core::Layer v2_lines_layer() {
    render::Image band = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
    band.paste(render::Ink{200, 40, 40, 255}, render::Box{0, 30, 60, 40});
    core::Layer layer;
    layer.id = "old";
    layer.kind = core::LayerKind::Strokes;
    layer.role = core::LayerRole::User;
    layer.panel_clip = false;
    layer.panel_each = false;
    layer.raster_png = std::make_shared<const std::string>(render::write_png(band));
    return layer;
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
        // an RGB mode of every default, a grey ink of two values
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
        QVERIFY(refused(core::to_json(core::RgbMode{})));  // (every default: written as no color, never so)
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
    // whose layer cannot be drawn in precise colour yet is refused with its name (nothing changed; PC2d draws the ones it
    // refused before).
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
        // a layer that keeps its lines inside the panels is precise too (PC2d, document:color-depth-model: it was refused
        // with its name before): its pixels shown as the unclipped layer's — the panels keep its lines, it has none
        const core::Document clipped = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"panel_clip", true}}})).doc;
        const std::string why = refusal(clipped, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        QVERIFY2(why.empty(), why.c_str());
        const core::Document clipped16 = run_ops(clipped, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).doc;
        QVERIFY(layer_named(clipped16, id).color_raster && layer_named(clipped16, id).panel_clip);
        QCOMPARE(drawn_page(clipped16).tobytes(), drawn_page(deep.doc).tobytes());
        // refused: a depth, a mode
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "rgb"}, {"depth", 12}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "hsv"}}})).empty());
        // (multichannel: PC1d's — the page becomes multichannel, test_color_model's multichannel slots)
        QVERIFY(refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"mode", "multichannel"}}})).empty());
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
    // color-model@1, one rule (review A, R): on an RGB page of a profile every paint pixel is the page's value, whatever
    // its layer's kind — a v2 book's layer of lines with a raster is converted, drawn and merged as a paint layer is —;
    // an op that lends the page's pictures as sRGB leaves the layers it did not work as they were (their bytes); a merge
    // composites the page's values (no lending); a tint's colour and an effect's are sRGB colours, shown as themselves.
    void profiledPagesOneRule() {
        core::Document doc = painted(working_picture());
        doc.edit_page(0).layers.push_back(v2_lines_layer());
        const std::string pic = doc.page(0).layers[doc.page(0).layers.size() - 2].id;
        const int w = drawn_page(doc).width(), h = drawn_page(doc).height();
        const std::array<std::pair<int, int>, 3> spots{{{w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 2, 7 * h / 8}}};  // (grey, green, red)
        const render::Image plain = drawn_page(doc);
        // converted into Display P3: the page shows as it did, the v2 layer's red too
        const core::Document p3 = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        QVERIFY(*layer_named(p3, "old").raster_png != *layer_named(doc, "old").raster_png);
        const render::Image shown = drawn_page(p3);
        for (const auto& [x, y] : spots) {
            QVERIFY2(near_rgb(rgb_at(shown, x, y), rgb_at(plain, x, y), 2), qPrintable(rgb_text(rgb_at(shown, x, y)) + " " + rgb_text(rgb_at(plain, x, y))));
        }
        // an op lending the pictures (a smudge on the paint layer, at its top): the v2 layer's bytes as they were
        const core::Document smudged = run_ops(p3, Json::array({Json{{"op", "smudge"}, {"page", 1}, {"layer_id", pic},
                                                                     {"points", Json::array({Json::array({10, 10}), Json::array({40, 12})})}}})).doc;
        QCOMPARE(*layer_named(smudged, "old").raster_png, *layer_named(p3, "old").raster_png);
        QVERIFY(smudged.page(0).rgb && smudged.page(0).rgb->profile == p3.page(0).rgb->profile);
        // merged: the page's values composited (shown as before)
        for (const Json& merge : {Json{{"op", "merge_down"}, {"page", 1}, {"id", "old"}},
                                  Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}, {"flatten", true}}}) {
            const core::Document merged = run_ops(p3, Json::array({merge})).doc;
            const render::Image after = drawn_page(merged);
            for (const auto& [x, y] : spots) {
                QVERIFY2(near_rgb(rgb_at(after, x, y), rgb_at(shown, x, y), 2),
                         qPrintable(QString::fromStdString(merge["op"].get<std::string>()) + " " + rgb_text(rgb_at(after, x, y)) + " " + rgb_text(rgb_at(shown, x, y))));
            }
        }
        // a tint (the layer's colour) is sRGB: shown as itself on a page of a profile too
        {
            const Json tint = Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", pic}, {"color", Json::array({30, 60, 200})}}});
            const auto was = rgb_at(drawn_page(run_ops(doc, tint).doc), w / 4, h / 4);
            const auto now = rgb_at(drawn_page(run_ops(p3, tint).doc), w / 4, h / 4);
            QVERIFY2(near_rgb(now, was, 1), qPrintable(rgb_text(now) + " " + rgb_text(was)));
        }
        // an effect's colour (a border) is sRGB too; the pixels it is around still the page's values
        {
            render::Image dot = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
            dot.paste(render::Ink{200, 40, 40, 255}, render::Box{20, 10, 40, 20});
            core::Document spot = painted(dot);
            const std::string id = spot.page(0).layers.back().id;
            spot = run_ops(spot, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
            const auto inside = rgb_at(drawn_page(spot), w / 2, h * 3 / 8);
            const core::Document bordered = run_ops(spot, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id},
                                                                            {"effect", Json{{"border", Json{{"width_mm", 3}, {"rgb", Json::array({30, 60, 200})}}}}}}})).doc;
            const render::Image b = drawn_page(bordered);
            const auto border = rgb_at(b, w / 2, h / 4 - 4);  // (just above the dot)
            QVERIFY2(near_rgb(border, {30, 60, 200}, 1), qPrintable(rgb_text(border)));
            QVERIFY2(near_rgb(rgb_at(b, w / 2, h * 3 / 8), inside, 1), qPrintable(rgb_text(rgb_at(b, w / 2, h * 3 / 8)) + " " + rgb_text(inside)));
        }
    }

    // An op lending an RGB page of a profile its pictures as sRGB (review G): what it moves keeps its own values (a colour
    // of Display P3 past sRGB's gamut stays itself), what it leaves within 2 a channel of how it was shown keeps its value.
    void lendingKeepsOwnValues() {
        core::Document doc = painted(working_picture(render::Ink{0, 255, 0, 255}));  // (P3's own green: past sRGB)
        const std::string id = doc.page(0).layers.back().id;
        doc = run_ops(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const core::Document pushed = run_ops(doc, Json::array({Json{{"op", "liquify"}, {"page", 1}, {"layer_id", id},
                                                                     {"points", Json::array({Json::array({40, 40}), Json::array({15, 40})})}, {"width_mm", 16}}})).doc;
        const std::string before = render::open_image(*layer_named(doc, id).raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        const std::string after = render::open_image(*layer_named(pushed, id).raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        QCOMPARE(after.size(), before.size());
        // (P3's green shown as sRGB's green, and that back in P3: what a moved pixel must not become)
        double back[3] = {0.0, 1.0, 0.0};
        render::RgbSpace(*doc.page(0).rgb).from_srgb(back, 1);
        std::array<int, 3> through{};
        for (unsigned c = 0; c < 3; ++c) through[c] = static_cast<int>(std::lround(std::clamp(back[c], 0.0, 1.0) * 255));
        QVERIFY(through != (std::array<int, 3>{0, 255, 0}));
        int moved = 0, wrong = 0;
        for (std::size_t i = 0; i < after.size(); i += 4) {
            const auto* a = reinterpret_cast<const unsigned char*>(after.data()) + i;
            const auto* b = reinterpret_cast<const unsigned char*>(before.data()) + i;
            if (b[1] != 128 || a[3] != 255) continue;  // (a grey before: the green pushed over it)
            if (a[0] == 0 && a[1] == 255 && a[2] == 0) ++moved;
            else if (a[0] == through[0] && a[1] == through[1] && a[2] == through[2]) ++wrong;
        }
        QVERIFY2(moved > 20, qPrintable(QString::number(moved)));
        QVERIFY2(wrong == 0, qPrintable(QString::number(wrong)));
    }
    // The page's values drawn in parts (review B): a page that is drawn whole for a part of it (a correction that moves
    // shapes) and one whose nombre is drawn from a wider part (print) give the values of that part, the same size as its
    // picture and shown as it is; a pixel looked at is the page's there.
    void preciseValuesOfAPart() {
      try {
        core::Document doc = run_ops(painted(picture()), Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const Json plain = render::pixel_at(doc.page(0), &doc, 50, 20, std::nullopt, 100);
        core::Document blurred = doc;  // (a correction that moves shapes, as a book may hold one: drawn whole, left out here)
        {
            core::Layer blur;
            blur.id = "blur";
            blur.kind = core::LayerKind::Adjust;
            blur.role = core::LayerRole::User;
            blur.adjust = Json{{"kind", "blur"}, {"radius", 2}};
            blurred.edit_page(0).layers.push_back(blur);
        }
        const Json seen = render::pixel_at(blurred.page(0), &blurred, 50, 20, std::nullopt, 100);
        QCOMPARE(seen["values"], plain["values"]);
        render::RenderOptions part;
        part.precise = true;
        part.skip_unported = true;
        part.region = render::RenderRegion{30, 40, 50, 60};
        const render::RenderResult cut = render::render_page(blurred.page(0), 100, part, &blurred);
        QVERIFY(cut.precise);
        QCOMPARE(int(core::ColorRasterView(*cut.precise).width()), 50);
        QCOMPARE(int(core::ColorRasterView(*cut.precise).height()), 60);
        // print: a page drawn in tiles, its nombre across a tile's edge
        const int dpi = 433;
        render::RenderOptions print;
        print.mode = "print";
        print.precise = true;
        const render::RenderResult made = render::render_page(doc.page(0), dpi, print, &doc);
        QVERIFY(made.precise);
        const core::ColorRasterView view(*made.precise);
        QCOMPARE(int(view.width()), made.image.width());
        QCOMPARE(int(view.height()), made.image.height());
        QVERIFY(made.image.width() * made.image.height() > 1024 * 1024);  // (drawn in tiles)
        const std::vector<render::Box> boxes = render::nombre_areas(doc.page(0), dpi, &doc);
        QVERIFY(!boxes.empty());
        bool across = false;
        const render::RgbSpace space = render::RgbSpace::of(doc.page(0));
        int off = 0;
        for (const render::Box& b : boxes) {
            across = across || (b.x0 < 512 && b.x1 > 512);
            for (int y = std::max(0, b.y0); y < std::min(made.image.height(), b.y1); ++y) {
                for (int x = std::max(0, b.x0); x < std::min(made.image.width(), b.x1); ++x) {
                    const auto p = view.pixel(std::size_t(y) * view.width() + std::size_t(x));
                    double rgb[3] = {p[0], p[1], p[2]};
                    space.to_srgb(rgb, 1);
                    const auto shown = rgb_at(made.image, x, y);
                    for (unsigned c = 0; c < 3; ++c) {
                        if (std::abs(static_cast<int>(std::lround(std::clamp(rgb[c], 0.0, 1.0) * 255)) - shown[c]) > 2) ++off;
                    }
                }
            }
        }
        QVERIFY(across);
        QCOMPARE(off, 0);
      } catch (const std::exception& error) {
        QFAIL(error.what());
      }
    }

    // A grey page of 32 bits holds HDR (review C): into RGB at 8 or 16 bits, into RGB of a profile, into another grey
    // profile — refused unless clip_hdr (the pixels counted); into sRGB at 32 bits, kept (sGray: its value).
    void greyHdrRefused() {
        core::Document doc = small_book();
        core::PageColor grey;
        grey.space = core::InkSpace::Gray;
        grey.depth = 32;
        grey.profile = core::builtin_profile("sgray")->ref();
        grey.icc = core::builtin_profile("sgray")->bytes;
        core::InkPixels pixels = core::blank_ink_pixels(core::InkSpace::Gray, 32, 2, 1);
        pixels.set(0, 0, 2.0);
        pixels.set(0, 1, 1.0);
        pixels.set(1, 0, 0.5);
        pixels.set(1, 1, 1.0);
        core::Layer layer;
        layer.id = "hdr";
        layer.kind = core::LayerKind::Raster;
        layer.role = core::LayerRole::User;
        layer.panel_clip = false;
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
        doc.edit_page(0).layers.push_back(layer);
        doc.edit_page(0).color = grey;
        for (const Json& op : {Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}},
                               Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                               Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}, {"profile", "display-p3"}},
                               Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "gray-gamma-2.2"}}}) {
            const std::string why = refusal(doc, Json::array({op}));
            QVERIFY2(why.find("clip_hdr") != std::string::npos, qPrintable(QString::fromStdString(op.dump() + " → " + why)));
            Json cut = op;
            cut["clip_hdr"] = true;
            const core::ApplyResult done = run_ops(doc, Json::array({cut}));
            QCOMPARE(done.results.at(0)["clipped"], Json(1));
        }
        const core::Document kept = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}})).doc;
        QCOMPARE(core::ColorRasterView(*layer_named(kept, "hdr").color_raster).pixel(0)[0], 2.0);
    }

    // A page that becomes an RGB page of a profile is drawn in precise colour (review D, J). PC2d (document:color-depth-
    // model): what it could not draw before — a tone layer, a v2 layer of lines with its raster (at 16 bits: precise
    // pixels on a layer of lines) — is drawn now, so the ops that make the page so go through and the page shows them as
    // the 8-bit page does (the op refused them, naming the layer, before PC2d: SPEC.md COLOR-03). (A correction layer in
    // a blend mode was refused too: its own blend is not looked at, as Python does not — the user's decision 2 of PC2.)
    void profiledPagesDrawWhatTheyHold() {
        const core::Document doc = painted(picture());
        const core::Document toned = run_ops(doc, Json::array({Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"area", Json{{"rect", Json::array({10, 20, 30, 40})}}}}})).doc;
        const core::Document blended = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"}, {"blend", "multiply"},
                                                                      {"adjust", Json{{"kind", "levels"}, {"black", 10}, {"white", 240}, {"gamma", 1.0}}}}})).doc;
        const render::Image plain_tone = drawn_page(toned);
        for (const Json& op : {Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}},
                               Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "adobe-rgb-compat"}},
                               Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"profile", "rec2020"}}}) {
            const std::string why = refusal(toned, Json::array({op}));
            QVERIFY2(why.empty(), qPrintable(QString::fromStdString(why)));
            QVERIFY2(refusal(blended, Json::array({op})).empty(), qPrintable(QString::fromStdString(op.dump())));
        }
        // (converted: the tone's dots where they were, over the opaque grey as it showed — within the profiles' round
        // trip; the green, half clear, is mixed in linear light on a page of a profile)
        const core::Document p3 = run_ops(toned, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        render::RenderOptions options;
        const render::RenderResult shown = render::render_page(p3.page(0), 100, options, &p3);
        QVERIFY(shown.omitted.empty());
        QCOMPARE(shown.image.size(), plain_tone.size());
        const render::Box over_grey{render::mm_to_px(11, 100), render::mm_to_px(21, 100), render::mm_to_px(29, 100), render::mm_to_px(59, 100)};
        const std::string a = shown.image.convert("RGB").crop(over_grey).tobytes(), b = plain_tone.crop(over_grey).tobytes();
        int worst = 0;
        std::size_t dots = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i]))));
            dots += static_cast<unsigned char>(b[i]) < 100;
        }
        QVERIFY2(worst <= 2, qPrintable(QString::number(worst)));
        QVERIFY(dots > 0);  // (the tone is there)
        core::Document lines = doc;
        lines.edit_page(0).layers.push_back(v2_lines_layer());
        const core::ApplyResult deep = run_ops(lines, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        QVERIFY(layer_named(deep.doc, "old").color_raster && layer_named(deep.doc, "old").kind == core::LayerKind::Strokes);
        const render::Image red = drawn_page(deep.doc);
        const int w = red.width(), h = red.height();
        QVERIFY2(near_rgb(rgb_at(red, w / 2, h - 3), {200, 40, 40}, 1), qPrintable(rgb_text(rgb_at(red, w / 2, h - 3))));  // (its band)
        QVERIFY(near_rgb(rgb_at(red, w / 4, h / 4), rgb_at(drawn_page(lines), w / 4, h / 4), 1));
        // (at 8 bits it is drawn: its raster the page's values)
        QVERIFY(refusal(lines, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).empty());
    }

    // PC2d (document:color-depth-model, design 4.3): a page in precise colour — 16 bits, 32 bits, an RGB page of a profile
    // — draws what the 8-bit page draws where set_color_mode refused it before (not_yet_ported): a tone layer, a layer in
    // a folder, a correction that moves shapes, a precise paint layer with lines and pictures laid on it, an effect and a
    // colour of its own. The op goes through, nothing is left out, and the page is the 8-bit page's picture: where no
    // edge of partial cover lies (a precise canvas mixes those in linear light, the 8-bit page in sRGB samples) within a
    // step of 8 bits (a page of a profile: two, the profile's round trip), a shape-moving filter's few rounding steps
    // aside (as test_color_filters' corrections).
    void preciseDrawsWhatTheEightBitPageDraws_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("into");
        for (const char* what : {"tone", "folder", "blur", "lines", "pictures", "border", "water", "tint"})
            for (const char* into : {"16", "32", "p3"}) QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(what, into))) << QString(what) << QString(into);
    }
    void preciseDrawsWhatTheEightBitPageDraws() {
        QFETCH(QString, what);
        QFETCH(QString, into);
        // (a picture of the page's pixels at the 100 dpi it is drawn at: nothing resampled; a grey and a green
        // rectangle, opaque, clear around them)
        const int w = render::mm_to_px(60, 100), h = render::mm_to_px(80, 100);
        render::Image art = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        art.paste(render::Ink{128, 128, 128, 255}, render::Box{w / 6, h / 6, w / 2, 5 * h / 6});
        art.paste(render::Ink{30, 160, 90, 255}, render::Box{w / 2, h / 6, 5 * w / 6, 5 * h / 6});
        core::Document doc = painted(art);
        const std::string id = doc.page(0).layers.back().id;
        Json ops = Json::array();
        if (what == "tone") ops.push_back(Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"area", Json{{"rect", Json::array({12, 20, 30, 40})}}}});
        if (what == "folder") ops.push_back(Json{{"op", "group_layers"}, {"page", 1}, {"ids", Json::array({id})}});
        if (what == "blur") {  // (as an old book holds one: no op makes it)
            core::Layer soft;
            soft.id = "soft";
            soft.kind = core::LayerKind::Adjust;
            soft.role = core::LayerRole::User;
            soft.adjust = Json{{"kind", "blur"}, {"radius", 3}};
            doc.edit_page(0).layers.push_back(soft);
        }
        if (what == "lines") {
            ops.push_back(Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", id}, {"points", Json::array({Json::array({8, 40}), Json::array({52, 40})})},
                               {"width_mm", 2.5}, {"kind", "mili"}, {"rgb", Json::array({200, 30, 30})}});
        }
        if (what == "pictures") {
            ops.push_back(Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", id}, {"area", Json{{"rect", Json::array({20, 30, 20, 12})}}},
                               {"rgb", Json::array({40, 60, 200})}});
        }
        if (what == "border") ops.push_back(Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"effect", Json{{"border", Json{{"width_mm", 1.5}, {"rgb", Json::array({250, 200, 20})}}}}}});
        if (what == "water") ops.push_back(Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"effect", Json{{"water_edge", Json{{"width_mm", 1.2}, {"strength", 1.0}}}}}});
        if (what == "tint") ops.push_back(Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"color", Json::array({200, 40, 40})}});
        doc = run_ops(doc, ops).doc;
        const render::Image eight = drawn_page(doc);
        const Json to = into == "16"   ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}
                        : into == "32" ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}
                                       : Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}};
        const std::string why = refusal(doc, Json::array({to}));
        QVERIFY2(why.empty(), why.c_str());
        const core::Document precise = run_ops(doc, Json::array({to})).doc;
        QVERIFY(render::uses_color_precision(precise.page(0), false));
        render::RenderOptions options;  // (nothing left out: an unported part would stop the render)
        const render::RenderResult got = render::render_page(precise.page(0), 100, options, &precise);
        QVERIFY(got.omitted.empty());
        const render::Image shown = got.image.convert("RGB");
        QCOMPARE(shown.size(), eight.size());
        const int most = into == "p3" ? 2 : 1;
        if (what == "lines" || what == "pictures" || what == "water" || (what == "blur" && into == "p3")) {
            // (their edges cover part of a pixel: the line's middle, the picture's, the rim against the plain inside; a
            // page of a profile blurs its own values, which mix the edges' colours otherwise than sRGB's: the insides)
            const auto at = [&](double x_mm, double y_mm) { return std::pair{render::mm_to_px(x_mm, 100), render::mm_to_px(y_mm, 100)}; };
            std::vector<std::pair<int, int>> spots{at(15, 25), at(45, 60), at(30, 70)};
            if (what == "lines") spots.push_back(at(30, 40));
            if (what == "pictures") spots.push_back(at(30, 36));
            for (const auto& [x, y] : spots) {
                QVERIFY2(near_rgb(rgb_at(shown, x, y), rgb_at(eight, x, y), most),
                         qPrintable(QStringLiteral("%1,%2: %3 %4").arg(x).arg(y).arg(rgb_text(rgb_at(shown, x, y)), rgb_text(rgb_at(eight, x, y)))));
            }
            if (what == "lines") QVERIFY(near_rgb(rgb_at(shown, at(30, 40).first, at(30, 40).second), {200, 30, 30}, most));
            if ((what == "lines" || what == "pictures") && into != "p3") {
                // merged down: the precise layer as the page draws it, lines and pictures in its pixels
                const std::string lower = [&] {
                    const auto& layers = precise.page(0).layers;
                    for (std::size_t i = 1; i < layers.size(); ++i)
                        if (layers[i].id == id) return layers[i - 1].id;
                    return std::string();
                }();
                const core::Document merged = run_ops(precise, Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", id}}})).doc;
                QVERIFY(layer_named(merged, lower).color_raster && layer_named(merged, lower).stroke_count() == 0 && layer_named(merged, lower).patches.empty());
                const render::Image flat = drawn_page(merged);
                for (const auto& [x, y] : spots) QVERIFY2(near_rgb(rgb_at(flat, x, y), rgb_at(shown, x, y), 2), qPrintable(rgb_text(rgb_at(flat, x, y))));
            }
            if (what == "pictures") QVERIFY(near_rgb(rgb_at(shown, at(30, 36).first, at(30, 36).second), {40, 60, 200}, most));
            if (what == "water") {  // (the rim darker than the inside, on both pages)
                const auto [rx, ry] = std::pair{w / 6 + 1, h / 2};
                const auto [ix, iy] = at(20, 40);
                QVERIFY(rgb_at(shown, rx, ry)[0] < rgb_at(shown, ix, iy)[0] - 20);
                QVERIFY(rgb_at(eight, rx, ry)[0] < rgb_at(eight, ix, iy)[0] - 20);
            }
                // (and as dark as there, across the rim's band — the grey's left and top edges, the green's right one —:
                // its black laid over the page's encoded values as the 8-bit page lays it over sRGB's; independent review
                // B (B))
                const int rim = render::mm_to_px(1.2, 100) + 2;
                std::vector<std::pair<int, int>> band;
                for (int k = 0; k < rim; ++k) {
                    band.emplace_back(w / 6 + k, h / 2);
                    band.emplace_back(w / 3, h / 6 + k);
                    band.emplace_back(5 * w / 6 - 1 - k, h / 2);
                }
                for (const auto& [x, y] : band) {
                    QVERIFY2(near_rgb(rgb_at(shown, x, y), rgb_at(eight, x, y), most),
                             qPrintable(QStringLiteral("rim %1,%2: %3 %4").arg(x).arg(y).arg(rgb_text(rgb_at(shown, x, y)), rgb_text(rgb_at(eight, x, y)))));
                }
            return;
        }
        // (the rest: every pixel; a shape-moving filter's 8-bit passes round otherwise in a few)
        const std::string a = shown.tobytes(), b = eight.tobytes();
        std::size_t off = 0;
        int worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const int diff = std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i])));
            worst = std::max(worst, diff);
            if (diff > most) ++off;
        }
        const std::size_t allowed = what == "blur" ? a.size() / 500 : 0;
        QVERIFY2(off <= allowed, qPrintable(QStringLiteral("%1 samples off by more than %2 (worst %3)").arg(off).arg(most).arg(worst)));
        // a part of the page is the whole page's cut (a page with a correction that moves shapes is drawn whole for it)
        render::RenderOptions part;
        part.region = render::RenderRegion{31, 47, 90, 70};
        QCOMPARE(render::render_page(precise.page(0), 100, part, &precise).image.convert("RGB").tobytes(),
                 got.image.convert("RGB").crop(render::Box{31, 47, 121, 117}).tobytes());
    }

    // Independent review B (A): a paint layer that keeps its lines inside the panels (はみ出し off, add_layer's
    // default) keeps what is drawn on it there on a page of 16 or 32 bits and on a CMYK page, as the 8-bit page does —
    // a line, a fill, a line that stays in the panel it begins in (コマごと) —, whether the 8-bit page with the mark is
    // made so or the mark is drawn on the page made so: where the 8-bit page shows nothing of the mark (the panels cut
    // it), the page shows nothing of it either; the mark's inside is the 8-bit page's (16, 32 bits: within a step); and
    // the page made 8 bits again is the 8-bit page's (within a step: the mark is in its pixels then).
    void marksStayInsideThePanels_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("into");
        for (const char* what : {"line", "fill", "each"})
            for (const char* into : {"16", "32", "cmyk"}) QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(what, into))) << QString(what) << QString(into);
    }
    void marksStayInsideThePanels() {
        QFETCH(QString, what);
        QFETCH(QString, into);
        const int w = render::mm_to_px(60, 100), h = render::mm_to_px(80, 100);
        render::Image art = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        art.paste(render::Ink{128, 128, 128, 255}, render::Box{w / 6, h / 6, w / 2, 5 * h / 6});
        art.paste(render::Ink{30, 160, 90, 255}, render::Box{w / 2, h / 6, 5 * w / 6, 5 * h / 6});
        core::Document doc = small_book();
        if (what == "each") {  // (two panels, the line begun in the first crossing into the second)
            doc = run_ops(doc, Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", doc.page(0).frames.at(0).id}, {"axis", "horizontal"},
                                                 {"ratio", 0.5}}})).doc;
        }
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                        Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(art))}}})).doc;
        QVERIFY(layer_named(doc, "pic").panel_clip);
        if (what == "each") doc = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", true}}})).doc;
        const auto at = [&](double x_mm, double y_mm) { return std::pair{render::mm_to_px(x_mm, 100), render::mm_to_px(y_mm, 100)}; };
        Json mark;
        std::pair<int, int> inside;
        if (what == "fill") {
            mark = Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", Json{{"rect", Json::array({0, 30, 60, 12})}}}, {"rgb", Json::array({40, 60, 200})}};
            inside = at(30, 36);
        } else {
            const Json points = what == "each" ? Json::array({Json::array({20, 20}), Json::array({40, 60})}) : Json::array({Json::array({2, 40}), Json::array({58, 45})});
            mark = Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pic"}, {"points", points}, {"width_mm", 2.5}, {"kind", "mili"}, {"rgb", Json::array({200, 30, 30})}};
            inside = what == "each" ? at(25, 30) : at(30, 42.5);
        }
        const core::Document eight = run_ops(doc, Json::array({mark})).doc;
        const render::Image plain = drawn_page(doc), marked = drawn_page(eight);
        const Json to = into == "16"   ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}
                        : into == "32" ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}
                                       : Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}};
        const core::Document deep = run_ops(doc, Json::array({to})).doc;
        const render::Image deep_plain = drawn_page(deep);
        const std::string p = plain.tobytes(), m = marked.tobytes(), d = deep_plain.tobytes();
        const core::Document made = run_ops(eight, Json::array({to})).doc;  // (the 8-bit page with its mark, made so)
        const core::Document drawn_on = run_ops(deep, Json::array({mark})).doc;  // (the mark drawn on the page made so)
        for (const core::Document* page : {&made, &drawn_on}) {
            const render::Image shown = drawn_page(*page);
            QCOMPARE(shown.size(), marked.size());
            const std::string s = shown.tobytes();
            std::size_t cut = 0, out = 0;
            int worst = 0;
            for (std::size_t i = 0; i + 2 < s.size(); i += 3) {
                const auto same = [](const std::string& a, const std::string& b, std::size_t k) { return a[k] == b[k] && a[k + 1] == b[k + 1] && a[k + 2] == b[k + 2]; };
                if (!same(p, m, i)) continue;  // (the 8-bit page shows the mark here)
                ++cut;
                for (std::size_t c = i; c < i + 3; ++c) {
                    const int diff = std::abs(int(static_cast<unsigned char>(s[c])) - int(static_cast<unsigned char>(d[c])));
                    worst = std::max(worst, diff);
                    if (diff > 1) ++out;
                }
            }
            QVERIFY(cut > std::size_t(w) * h / 2);
            QVERIFY2(out == 0, qPrintable(QStringLiteral("%1 samples of the mark where the 8-bit page shows none (worst %2)").arg(out).arg(worst)));
            if (into != "cmyk") {
                QVERIFY2(near_rgb(rgb_at(shown, inside.first, inside.second), rgb_at(marked, inside.first, inside.second), 1),
                         qPrintable(rgb_text(rgb_at(shown, inside.first, inside.second)) + rgb_text(rgb_at(marked, inside.first, inside.second))));
            }
        }
        if (into == "cmyk") return;
        // back at 8 bits: the 8-bit page's
        const core::Document back = run_ops(made, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 8}}})).doc;
        const std::string b = drawn_page(back).tobytes();
        QCOMPARE(b.size(), m.size());
        std::size_t off = 0;
        int worst = 0;
        for (std::size_t i = 0; i < b.size(); ++i) {
            const int diff = std::abs(int(static_cast<unsigned char>(b[i])) - int(static_cast<unsigned char>(m[i])));
            worst = std::max(worst, diff);
            if (diff > 1) ++off;
        }
        QVERIFY2(off == 0, qPrintable(QStringLiteral("%1 samples off the 8-bit page (worst %2)").arg(off).arg(worst)));
    }

    // A page with no colour model holding precise pixels (put_color_raster) keeps their precision when an op does not say
    // a depth (review E): a change of intent, a conversion into a profile; one that changes nothing changes nothing.
    void precisionKeptWithoutADepth() {
        core::Document doc = small_book();
        doc = run_ops(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
                                              {"pixels", Json::array({0.25, 0.5, 0.75, 1.0, 0.1, 0.2, 0.3, 1.0})}}})).doc;
        const core::Bytes was = doc.page(0).layers.back().color_raster;
        const core::ApplyResult nothing = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}}));
        QCOMPARE(nothing.results.at(0)["converted"], Json(0));
        QVERIFY(!nothing.doc.page(0).rgb);
        const core::Document intent = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"intent", "perceptual"}}})).doc;
        QVERIFY(intent.page(0).layers.back().color_raster);
        QCOMPARE(*intent.page(0).layers.back().color_raster, *was);
        QVERIFY(intent.page(0).rgb && intent.page(0).rgb->depth == 32);
        const core::Document p3 = run_ops(doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        QVERIFY(p3.page(0).layers.back().color_raster);
        QCOMPARE(static_cast<int>((*p3.page(0).layers.back().color_raster)[5]), 4);
        QVERIFY(p3.page(0).rgb && p3.page(0).rgb->depth == 32);
        // a depth said: as said
        const core::Document eight = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 8}}})).doc;
        QVERIFY(!eight.page(0).layers.back().color_raster && eight.page(0).layers.back().raster_png);
    }

    // The precise pixels' budget (1 GiB, review F): a conversion past it is refused before its pixels are made (no
    // large allocation on the way).
    void budgetBeforeTheWork() {
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
        const std::int64_t start = allocations::reset_peak();
        const std::string why = refusal(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}}));
        const std::int64_t grew = allocations::peak() - start;
        QVERIFY2(why.find("1 GiB") != std::string::npos, why.c_str());
        QVERIFY2(grew < 128LL * 1024 * 1024, qPrintable(QString::number(grew)));
    }

    // document.inspect never stops on a profile's description or a layer's pixels it cannot read (review H): a v2
    // description's bytes past ASCII and a v4 one's surrogate pairs are text, lone halves the replacement character.
    void inspectNeverStops() {
        const auto be32 = [](std::uint32_t v) {
            return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
        };
        const auto icc_with = [&](const std::string& tag) {
            std::string out(128, '\0');
            out += be32(1) + "desc" + be32(144) + be32(static_cast<std::uint32_t>(tag.size()));
            out += tag;
            return out;
        };
        const std::string v2 = icc_with("desc" + std::string(4, '\0') + be32(13) + std::string("Caf\xe9 profil\0", 13));
        QCOMPARE(core::icc_description(v2), std::string("Caf? profil"));
        const std::string text("\xd8\x3d\xde\x00\x00\x41\xd8\x00\x00\x42", 10);  // (😀 A, a lone high half, B)
        const std::string v4 = icc_with("mluc" + std::string(4, '\0') + be32(1) + be32(12) + "enUS" + be32(10) + be32(28) + text);
        QCOMPARE(core::icc_description(v4), std::string("\xf0\x9f\x98\x80" "A\xef\xbf\xbd" "B"));
        // a CMYK page of such a profile, a grey page whose layer's pixels are not GKIR: inspected
        core::Document doc = small_book(2);
        core::PageColor cmyk;
        cmyk.icc = std::make_shared<const std::string>(v2);
        cmyk.profile = core::bytes_ref(v2);
        doc.edit_page(0).color = cmyk;
        core::PageColor grey;
        grey.space = core::InkSpace::Gray;
        grey.profile = core::builtin_profile("sgray")->ref();
        grey.icc = core::builtin_profile("sgray")->bytes;
        doc.edit_page(1).color = grey;
        core::Layer broken;
        broken.id = "broken";
        broken.kind = core::LayerKind::Raster;
        broken.ink_raster = std::make_shared<const std::string>("GKIR\x09 not one");
        doc.edit_page(1).layers.push_back(broken);
        const Json seen = storage::snapshot(doc, false);
        QCOMPARE(seen["pages"][0]["color"]["profile_description"], Json("Caf? profil"));
        QVERIFY(!seen["pages"][1]["layers"].back().contains("precision"));
        QVERIFY(!core::dump_python(seen).empty());
    }

    // Export of the layers (review K): an RGB page of a profile's paint layer written in sRGB, as the page shows it.
    void layersExportShowsSrgb() {
        QTemporaryDir tmp;
        const fs::path out = core::path_from_utf8(tmp.path().toStdString());
        const core::Document p3 = run_ops(painted(picture()), Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const auto files = formats::export_layers(p3, out, 100, "paper");
        double rgb[3] = {30 / 255.0, 160 / 255.0, 90 / 255.0};  // (the picture's green, its value in Display P3)
        render::RgbSpace(*p3.page(0).rgb).to_srgb(rgb, 1);
        std::array<int, 3> want{};
        for (unsigned c = 0; c < 3; ++c) want[c] = static_cast<int>(std::lround(std::clamp(rgb[c], 0.0, 1.0) * 255));
        bool found = false;
        for (const fs::path& file : files) {
            if (core::path_to_utf8(file.filename()).find("_layer") == std::string::npos) continue;  // (the paint layer's)
            const render::Image layer = render::open_image(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(file))), render::kPillowOpenLimits);
            const auto green = rgb_at(layer, 3 * layer.width() / 4, layer.height() / 4);
            QVERIFY2(near_rgb(green, want, 1), qPrintable(rgb_text(green) + " " + rgb_text(want)));
            found = true;
        }
        QVERIFY(found);
    }

    // A picture copied off an RGB page of a profile (its values) and pasted (review L): on a page of the same profile
    // laid as they are (shown as the original), on an sRGB page brought into sRGB; an area moved on the page stays
    // itself; the key it carries is never kept.
    void copiedPicturesKeepTheirSpace() {
        const core::Document p3 = run_ops(painted(working_picture()), Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        const core::Page& page = p3.page(0);
        const core::Layer& source = page.layers.back();
        const Json area{{"poly", Json::array({Json::array({35, 5}), Json::array({55, 5}), Json::array({55, 25}), Json::array({35, 25})})}};
        core::Layer copy = source;
        const render::selection::Items items = render::selection::lift(copy, area, page);
        QCOMPARE(items.patches.size(), std::size_t{1});
        const Json clip = render::selection::items_to_json(items);
        const auto paste = [&](const core::Document& book) {
            return run_ops(book, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pasted"}},
                                              Json{{"op", "paste"}, {"page", 1}, {"layer_id", "pasted"}, {"items", clip},
                                                   {"matrix", Json::array({1, 0, 0, 1, -30, 50})}}})).doc;
        };
        const render::Image was = drawn_page(p3);
        const int w = was.width(), h = was.height();
        const auto green = rgb_at(was, 3 * w / 4, h / 4);
        const core::Document same = paste(p3);
        for (const core::Layer& layer : same.page(0).layers) {
            for (const core::Patch& patch : layer.patches) QVERIFY(!patch.attrs.contains("rgb_profile"));
        }
        const auto pasted = rgb_at(drawn_page(same), w / 4, 13 * h / 16);  // (moved to x 5..25 mm, y 55..75 mm)
        QVERIFY2(near_rgb(pasted, green, 2), qPrintable(rgb_text(pasted) + " " + rgb_text(green)));
        // onto an sRGB page: its colour as P3's shown
        const core::Document srgb = paste(painted(working_picture()));
        const auto there = rgb_at(drawn_page(srgb), w / 4, 13 * h / 16);
        QVERIFY2(near_rgb(there, green, 2), qPrintable(rgb_text(there) + " " + rgb_text(green)));
        for (const core::Layer& layer : srgb.page(0).layers) {
            for (const core::Patch& patch : layer.patches) QVERIFY(!patch.attrs.contains("rgb_profile"));
        }
        // moved on the page: itself
        const core::Document moved = run_ops(p3, Json::array({Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", source.id}, {"area", area},
                                                                   {"matrix", Json::array({1, 0, 0, 1, -30, 50})}}})).doc;
        const auto there_now = rgb_at(drawn_page(moved), w / 4, 13 * h / 16);
        QVERIFY2(near_rgb(there_now, green, 2), qPrintable(rgb_text(there_now) + " " + rgb_text(green)));
        for (const core::Layer& layer : moved.page(0).layers) {
            for (const core::Patch& patch : layer.patches) QVERIFY(!patch.attrs.contains("rgb_profile"));
        }
    }

    // A grey page's corrections on a profile other than sGray (review M): the colours inks mean and back are the same
    // inks (no step darker or lighter), so a correction that changes nothing changes no pixel.
    void greyMeaningGoesBack() {
        core::PageColor color;
        color.space = core::InkSpace::Gray;
        color.profile = core::builtin_profile("gray-gamma-2.2")->ref();
        color.icc = core::builtin_profile("gray-gamma-2.2")->bytes;
        for (const int depth : {8, 16, 32}) {
            color.depth = depth;
            std::vector<double> inks, rgb(256 * 3), back(256);
            for (int v = 0; v < 256; ++v) inks.push_back(v / 255.0);
            render::ink::meaning_of(color, inks.data(), rgb.data(), 256);
            render::ink::inks_by_rules(color, rgb.data(), back.data(), 256);
            for (int v = 0; v < 256; ++v) {
                if (v == 245 || v == 240 || v == 235) continue;  // (lightness 10, 15, 20: Genko's blacks, the rules' own)
                QVERIFY2(std::abs(back[v] - inks[v]) < 1e-9, qPrintable(QString("%1: %2").arg(v).arg(back[v])));
            }
        }
    }

    // A page of a profile drawn as fast as an sRGB one (review N): the paper and an 8-bit layer of a few colours come
    // into its space without a transform each pixel.
    void profiledCanvasIsQuick() {
        render::Image paper = render::Image::create("RGBA", render::Size{1500, 1500}, render::Ink{255, 255, 255, 255});
        render::Image layer = render::Image::create("RGBA", render::Size{1500, 1500}, render::Ink{0, 0, 0, 0});
        for (int i = 0; i < 30; ++i) layer.paste(render::Ink{i * 8, 200 - i * 5, 40 + i * 6, 255}, render::Box{i * 50, 0, i * 50 + 40, 1500});
        const auto time = [&](const render::RgbSpace& space) {
            QElapsedTimer t;
            t.start();
            render::ColorCanvas canvas(paper, space);
            canvas.blend(layer, 0.8, false);
            return t.elapsed();
        };
        const render::RgbSpace p3(*run_ops(small_book(), Json::array({Json{{"op", "assign_profile"}, {"profile", "display-p3"}}})).doc.page(0).rgb);
        const qint64 plain = time(render::RgbSpace());
        const qint64 profiled = time(p3);
        QVERIFY2(profiled < 3 * plain + 200, qPrintable(QString("%1 ms vs %2 ms").arg(profiled).arg(plain)));
    }

    // A very large page's values (review O): refused before they are made (the most precise pixels a raster holds).
    void hugeValuesRefusedFirst() {
        const core::Document p3 = run_ops(painted(picture()), Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        render::RenderOptions options;
        options.mode = "print";
        std::stop_source stop;
        options.stop = stop.get_token();
        std::thread watchdog([&] {
            for (int i = 0; i < 100 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        const std::int64_t start = allocations::reset_peak();
        std::string code;
        try {
            (void)formats::detail::page_values(p3.page(0), p3, 3100, options);
        } catch (const core::Error& error) {
            code = error.code();
        } catch (const std::exception& error) {
            code = std::string("other: ") + error.what();
        }
        stop.request_stop();
        watchdog.join();
        QCOMPARE(code, std::string("image_too_large"));
        QVERIFY2(allocations::peak() - start < 256LL * 1024 * 1024, qPrintable(QString::number(allocations::peak() - start)));
    }

    // 16 → 8 bits dithered (review Q): an 8-bit value kept as itself, where the noise is just under 1 too.
    void ditherKeepsEightBitValues() {
        Json pixels = Json::array();
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 1936; ++x) {
                const bool that = x == 1935 && y == 1;
                for (const int v : {0, that ? 128 * 257 : 0, 0, 65535}) pixels.push_back(v);
            }
        }
        const core::Document doc = run_ops(small_book(), Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 1936}, {"height", 2},
                                                                         {"precision", "u16"}, {"pixels", pixels}}})).doc;
        const core::Document eight = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 8}, {"dither", true}}})).doc;
        const std::string rgba = render::open_image(*eight.page(0).layers.back().raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        QCOMPARE(int(static_cast<unsigned char>(rgba[(std::size_t(1) * 1936 + 1935) * 4 + 1])), 128);
    }

    // A PDF of a 32-bit page holding HDR (review S): its note says those values were cut.
    void pdfNotesTheHdrCut() {
        core::Document doc = run_ops(small_book(2), Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
                                                                     {"pixels", Json::array({2.0, 0.5, 0.25, 1.0, 0.5, 0.5, 0.5, 1.0})}},
                                                                Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}},
                                                                Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 32}}})).doc;
        const std::vector<std::string> notes = formats::export_notes(doc, "pdf", "auto");
        QCOMPARE(notes.size(), std::size_t{2});
        QCOMPARE(notes[0], std::string("32 bit のページ 2 ページは、この形式では 16 bit にして書き出しました"));
        QVERIFY2(notes[1].find("HDR") != std::string::npos && notes[1].find("1 ページ") != std::string::npos, notes[1].c_str());
        QCOMPARE(formats::export_notes(doc, "tiff", "auto"), std::vector<std::string>{});
    }

    // An RGB `color` written with every default (review T) is not the writer's: read-only, as "bpc": true is.
    void explicitDefaultIsReadOnly() {
        bool refused = false;
        try {
            (void)core::color_model_from_json(Json::parse(R"({"mode":"rgb","depth":8,"intent":"relative"})"));
        } catch (const core::Error& error) {
            refused = error.code() == "value";
        }
        QVERIFY(refused);
    }

    // The precise pixels checked once (review U): an op after another does not scan again the rasters it did not change.
    void preciseRastersCheckedOnce() {
        core::Document doc = small_book();
        doc = run_ops(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 64}, {"height", 64}, {"precision", "f32"},
                                              {"pixels", Json(std::vector<double>(64 * 64 * 4, 0.5))}}})).doc;
        const std::uint64_t before = core::color_raster_checks();
        for (int i = 0; i < 3; ++i) doc = run_ops(doc, Json::array({Json{{"op", "set_meta"}, {"title", "t" + std::to_string(i)}}})).doc;
        QVERIFY2(core::color_raster_checks() - before <= 1, qPrintable(QString::number(core::color_raster_checks() - before)));
    }

    // --- PC1d: インデックスカラー・カラーテーブル・2 階調・ダブルトーン・マルチチャンネル (color-model@1) ------------------

    // The JSON of the modes of their own: written in the key order, read back the same; what is not one refused.
    void specialModesJson() {
        const std::string sgray = core::builtin_profile("sgray")->ref();
        const std::string coated = core::builtin_profile("coated-cmyk")->ref();
        // (moc reads no raw string that ends in a quote: the profiles go in for @names)
        const auto fill = [&](std::string text) {
            for (const auto& [name, ref] : {std::pair<std::string, std::string>{"@sgray", sgray}, {"@coated", coated},
                                            {"@p3", core::builtin_profile("display-p3")->ref()}}) {
                for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name)) text.replace(at, name.size(), ref);
            }
            return text;
        };
        const std::vector<std::string> good{
            R"({"mode":"indexed","depth":8,"intent":"relative","table":{"colors":[[0,0,0],[255,255,255],[255,0,0]],"transparent":2}})",
            R"({"mode":"indexed","depth":8,"intent":"relative","bpc":false,"table":{"colors":[[10,20,30]],"transparent":null}})",
            R"({"mode":"bitmap","depth":8,"profile":"@sgray","intent":"relative","inks":[],"bitmap":{"method":"halftone","frequency":53.0,"angle":45.0,"shape":"round"}})",
            R"({"mode":"duotone","depth":16,"profile":"@sgray","intent":"relative","inks":[],"duotone":[{"name":"Black","color":[0,0,0],"curve":[[0.0,0.0],[100.0,100.0]]},{"name":"Orange","color":[255,128,0],"curve":[[0.0,0.0],[50.0,30.0],[100.0,80.0]]}]})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[[[200,0,0],[0.5,0.25,0.0]]],"plates":[{"name":"Cyan","color":[0,174,239],"solidity":0.0,"key":true},{"name":"Magenta","color":[236,0,140],"solidity":0.0,"key":true},{"name":"Yellow","color":[255,242,0],"solidity":0.5,"key":true}],"from":"rgb"})",
            R"({"mode":"multichannel","depth":32,"profile":"@coated","intent":"perceptual","inks":[],"plates":[{"name":"Cyan","color":[0,174,239],"solidity":0.0},{"name":"Black","color":[35,31,32],"solidity":0.0,"key":true}],"from":"cmyk"})",
            R"({"mode":"multichannel","depth":8,"profile":"@sgray","intent":"relative","inks":[],"plates":[{"name":"Gold","color":[219,173,46],"solidity":0.0,"key":true,"curve":[[0.0,0.0],[100.0,70.0]]}],"from":"duotone"})",
        };
        for (const std::string& text : good) {
            const Json value = Json::parse(fill(text));
            const core::ColorModel model = core::color_model_from_json(value);
            const Json back = model.rgb ? core::to_json(*model.rgb) : core::to_json(*model.inks);
            QVERIFY2(back.dump() == value.dump(), (back.dump() + " ≠ " + value.dump()).c_str());
        }
        const auto bad = [](const std::string& text) {
            try {
                (void)core::color_model_from_json(Json::parse(text));
            } catch (const core::Error& error) {
                return error.code() == "value";
            }
            return false;
        };
        const std::vector<std::string> wrong{
            R"({"mode":"indexed","depth":16,"intent":"relative","table":{"colors":[[0,0,0]],"transparent":null}})",
            R"({"mode":"indexed","depth":8,"profile":"@p3","intent":"relative","table":{"colors":[[0,0,0]],"transparent":null}})",
            R"({"mode":"indexed","depth":8,"intent":"relative","table":{"colors":[],"transparent":null}})",
            R"({"mode":"indexed","depth":8,"intent":"relative","table":{"colors":[[0,0,0]],"transparent":1}})",
            R"({"mode":"indexed","depth":8,"intent":"relative","table":{"colors":[[0,0,0]],"transparent":null,"x":1}})",
            // (review E: no depth — looked for, not read past the object's end)
            R"({"mode":"indexed","intent":"relative","bpc":false,"table":{"colors":[[0,0,0]],"transparent":null}})",
            R"({"mode":"indexed","intent":"relative","table":{"colors":[[0,0,0]],"transparent":null}})",
            R"({"mode":"rgb","depth":8,"intent":"relative","table":{"colors":[[0,0,0]],"transparent":null}})",
            R"({"mode":"bitmap","depth":16,"profile":"@sgray","intent":"relative","inks":[],"bitmap":{"method":"threshold","frequency":53,"angle":45,"shape":"round"}})",
            R"({"mode":"gray","depth":8,"profile":"@sgray","intent":"relative","inks":[],"bitmap":{"method":"threshold","frequency":53,"angle":45,"shape":"round"}})",
            R"({"mode":"bitmap","depth":8,"profile":"@sgray","intent":"relative","inks":[],"bitmap":{"method":"x","frequency":53,"angle":45,"shape":"round"}})",
            R"({"mode":"duotone","depth":8,"profile":"@sgray","intent":"relative","inks":[],"duotone":[{"name":"A","color":[0,0,0],"curve":[[50,0],[20,100]]}]})",
            R"({"mode":"duotone","depth":8,"profile":"@sgray","intent":"relative","inks":[],"duotone":[]})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[],"plates":[],"from":"rgb"})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[],"plates":[{"name":"A","color":[0,0,0],"solidity":0},{"name":"A","color":[0,0,0],"solidity":0}],"from":"rgb"})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[],"plates":[{"name":"A","color":[0,0,0],"solidity":0}],"from":"cmyk"})",
            R"({"mode":"multichannel","depth":8,"profile":"@coated","intent":"relative","inks":[],"plates":[{"name":"A","color":[0,0,0],"solidity":0}],"from":"rgb"})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[[[1,2,3],[0.5,0.5]]],"plates":[{"name":"A","color":[0,0,0],"solidity":0}],"from":"rgb"})",
            R"({"mode":"multichannel","depth":8,"intent":"relative","inks":[],"plates":[{"name":"A","color":[0,0,0],"solidity":0,"key":false}],"from":"rgb"})",
        };
        for (const std::string& text : wrong) QVERIFY2(bad(fill(text)), text.c_str());
    }

    // PhotoCraft's doc/src/mode.rs tests: the table's nearest colour (ties to the lower index), a black-ink duotone is
    // grey and a warm second ink warms the middle; the curve straight between its points; plates printed over paper
    // (compose/src/multichannel.rs inks_multiply_over_paper).
    void tablesInksAndPlates() {
        core::ColorTable t;
        t.colors = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}};
        QCOMPARE(t.nearest(230, 26, 26), std::size_t(2));
        QCOMPARE(t.nearest(153, 153, 153), std::size_t(1));
        t.colors = {{10, 10, 10}, {10, 10, 10}};
        QCOMPARE(t.nearest(10, 10, 10), std::size_t(0));
        const std::vector<core::DuotoneInk> black{core::DuotoneInk{"Black", {0, 0, 0}}};
        for (const double g : {0.0, 0.25, 1.0}) {
            const auto c = core::duotone_print(black, g);
            QVERIFY(std::abs(c[0] - g) < 1e-12 && c[0] == c[2] && c[1] == c[2]);
        }
        const auto warm = core::duotone_print({core::DuotoneInk{"Black", {0, 0, 0}}, core::DuotoneInk{"Orange", {255, 128, 0}}}, 0.5);
        QVERIFY(warm[0] > warm[2]);
        QCOMPARE(core::curve_at({{0, 0}, {50, 80}, {100, 100}}, 0.25), 0.4);
        QCOMPARE(core::curve_at({{20, 10}, {80, 90}}, 0.0), 0.1);
        QCOMPARE(core::curve_at({{20, 10}, {80, 90}}, 1.0), 0.9);
        QCOMPARE(core::curve_at({}, 0.3), 0.3);
        std::vector<core::Plate> plates(2);
        plates[0].color = {0, 255, 255};
        plates[1].color = {255, 0, 255};
        const std::array<std::array<double, 2>, 4> density{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        const std::array<std::array<double, 3>, 4> want{{{1, 1, 1}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}}};
        for (std::size_t i = 0; i < 4; ++i) {
            double p[3] = {1, 1, 1};
            core::print_plates(plates, density[i].data(), p);
            for (unsigned c = 0; c < 3; ++c) QCOMPARE(p[c], want[i][c]);
        }
    }

    // multichannel_cmds tests.rs rgb_becomes_cmy_inks_and_back_exactly: an RGB page's plates Cyan, Magenta, Yellow (each
    // 1 − its value), back to RGB the same pixels (8 bits exactly; 16 and 32 within a step), the book before untouched.
    void multichannelFromRgbAndBack() {
        const render::Image image = working_picture();
        const core::Document plain = painted(image);
        const std::string id = flat_id(plain);
        for (const int depth : {8, 16, 32}) {
            const core::Document doc = depth == 8 ? plain : run_ops(plain, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}})).doc;
            const core::ApplyResult multi = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}}));
            const core::Page& page = multi.doc.page(0);
            QVERIFY(page.color && page.color->space == core::InkSpace::Multi && page.color->depth == depth);
            QCOMPARE(page.color->from, std::string("rgb"));
            QCOMPARE(core::plate_labels(page), (std::vector<std::string>{"Cyan", "Magenta", "Yellow"}));
            QCOMPARE(multi.results.at(0)["flattened"], Json(1));
            const core::InkPixels plates = core::decode_ink_raster(*layer_named(multi.doc, id).ink_raster);
            QCOMPARE(plates.planes, 3u);
            const std::size_t at = std::size_t(10) * plates.width + plates.width - 5;  // (the green)
            QVERIFY(std::abs(plates.value(at, 0) - (1.0 - 30 / 255.0)) <= 0.5 / 255 + 1e-6);
            QVERIFY(std::abs(plates.value(at, 1) - (1.0 - 160 / 255.0)) <= 0.5 / 255 + 1e-6);
            // (shown: the plates printed over white — the paper's white no ink —, PhotoCraft's formula: each plate at its
            // density v takes p to (1 − s·v)·p·(1 − v + v·c) + s·v·c; the grey 128 is 1 − 128/255 on all three)
            const auto shown = rgb_at(drawn_page(multi.doc, 0, 200), 3, 3);
            std::array<double, 3> printed{1, 1, 1};
            for (const core::Plate& plate : page.color->plates) {
                const double v = 1.0 - 128 / 255.0, s = plate.solidity;
                for (unsigned c = 0; c < 3; ++c) printed[c] = (1 - s * v) * printed[c] * (1 - v + v * plate.color[c] / 255.0) + s * v * plate.color[c] / 255.0;
            }
            const std::array<int, 3> want{static_cast<int>(std::lround(printed[0] * 255)), static_cast<int>(std::lround(printed[1] * 255)),
                                          static_cast<int>(std::lround(printed[2] * 255))};
            QVERIFY2(near_rgb(shown, want, 1), qPrintable(rgb_text(shown) + QStringLiteral(" ≠ ") + rgb_text(want)));
            const core::ApplyResult back = run_ops(multi.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}}));
            const core::Layer& again = layer_named(back.doc, id);
            if (depth == 8) {
                QCOMPARE(render::open_image(*again.raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(), image.tobytes());
            } else {
                const core::ColorRasterView view(*again.color_raster);
                const auto p = view.pixel(at);
                QVERIFY(std::abs(p[0] - 30 / 255.0) < 1e-4 && std::abs(p[1] - 160 / 255.0) < 1e-4 && std::abs(p[2] - 90 / 255.0) < 1e-4);
            }
            QVERIFY(!doc.page(0).color);  // (the book before: as it was — Undo)
        }
    }

    // cmyk_lab_and_gray_channel_sets: CMYK's four plates, Lab's Alpha 1..3, grey's Black, a duotone's inks; back to the
    // mode the same pixels within a step.
    void multichannelSets() {
        const core::Document plain = painted(working_picture());
        const std::string id = plain.page(0).layers.back().id;
        const std::vector<std::tuple<Json, std::vector<std::string>, std::string>> cases{
            {Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}, {"Cyan", "Magenta", "Yellow", "Black"}, "cmyk"},
            {Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}}, {"Alpha 1", "Alpha 2", "Alpha 3"}, "lab"},
            {Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}, {"Black"}, "gray"},
        };
        for (const auto& [to, names, mode] : cases) {
            for (const int depth : {8, 16}) {
                Json first = to;
                first["depth"] = depth;
                const core::Document doc = run_ops(plain, Json::array({first})).doc;
                const core::InkPixels before = core::decode_ink_raster(*layer_named(doc, id).ink_raster);
                const core::Document multi = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).doc;
                QCOMPARE(core::plate_labels(multi.page(0)), names);
                QCOMPARE(multi.page(0).color->depth, depth);
                Json back = to;
                back["depth"] = depth;
                if (mode == "cmyk") back["profile"] = multi.page(0).color->profile;
                const core::InkPixels after = core::decode_ink_raster(*layer_named(run_ops(multi, Json::array({back})).doc, flat_id(plain)).ink_raster);
                QCOMPARE(after.space, before.space);
                for (std::size_t i = 0; i < before.pixel_count(); i += 97) {
                    for (unsigned c = 0; c < before.colours(); ++c) {
                        QVERIFY2(std::abs(after.value(i, c) - before.value(i, c)) <= 2.0 / 255,
                                 qPrintable(QStringLiteral("%1/%2 %3").arg(QString::fromStdString(mode)).arg(depth).arg(i)));
                    }
                }
            }
        }
        // a duotone's inks: a plate each, through its curve
        const core::Document grey = run_ops(plain, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const core::Document duo = run_ops(grey, Json::array({Json{{"op", "set_duotone"}, {"page", 1}, {"type", "tritone"}}})).doc;
        const core::Document multi = run_ops(duo, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).doc;
        QCOMPARE(multi.page(0).color->plates.size(), std::size_t(3));
        QCOMPARE(multi.page(0).color->from, std::string("duotone"));
        QCOMPARE(core::plate_labels(multi.page(0)), (std::vector<std::string>{"Black", "Warm Brown", "Gold"}));
    }

    // flattens_layers_and_keeps_alpha_channels: two paint layers flattened into one (the black on the upper solid in all
    // three plates), the page's frames and lines kept as elements, its alpha channel an alpha channel; back to RGB it
    // stays.
    void multichannelFlattensAndKeepsElements() {
        core::Document doc = painted(working_picture());
        doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "top"}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "top"}, {"panel_each", false}, {"panel_clip", false}},
                                        Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "top"}, {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({10, 0}), Json::array({10, 10}), Json::array({0, 10})})}}},
                                             {"rgb", Json::array({0, 0, 0})}},
                                        Json{{"op", "add_channel"}, {"page", 1}, {"name", "a"}},
                                        Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}, {"x_mm", 30}, {"y_mm", 40}}})).doc;
        const std::size_t frames = doc.page(0).frames.size();
        const std::size_t lines = doc.story.size();
        const core::ApplyResult multi = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}}));
        const core::Page& page = multi.doc.page(0);
        int pixels = 0;
        for (const core::Layer& layer : page.layers) pixels += layer.ink_raster != nullptr;
        QCOMPARE(pixels, 1);
        QCOMPARE(page.frames.size(), frames);
        QCOMPARE(multi.doc.story.size(), lines);
        QCOMPARE(page.channels.size(), std::size_t(1));
        const core::Layer* flat = nullptr;
        for (const core::Layer& layer : page.layers) {
            if (layer.ink_raster) flat = &layer;
        }
        const core::InkPixels plates = core::decode_ink_raster(*flat->ink_raster);
        const std::size_t corner = std::size_t(20) * plates.width + 20;  // (inside the black fill: solid ink on all three)
        for (unsigned c = 0; c < 3; ++c) QVERIFY(plates.value(corner, c) > 0.99);
        const core::Document back = run_ops(multi.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        QCOMPARE(back.page(0).channels.size(), std::size_t(1));
    }

    // mismatched_channel_count_converts_the_printed_look: two plates left (Yellow deleted) are no RGB page's: back to RGB
    // the page is the plates as they print.
    void multichannelMismatchedCountPrintsTheLook() {
        const core::Document doc = painted(working_picture());
        const std::string id = flat_id(doc);
        const core::Document multi = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}},
                                                               Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "Yellow"}}})).doc;
        QCOMPARE(multi.page(0).color->plates.size(), std::size_t(2));
        const core::InkPixels two = core::decode_ink_raster(*layer_named(multi, id).ink_raster);
        QCOMPARE(two.planes, 2u);
        const render::Image shown = render::ink::to_rgba8(two, *multi.page(0).color);
        const core::Document back = run_ops(multi, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        const render::Image now = render::open_image(*layer_named(back, id).raster_png, render::kPillowOpenLimits).convert("RGBA");
        for (const auto& [x, y] : {std::pair{3, 3}, std::pair{now.width() - 4, 10}}) QVERIFY(near_rgb(rgb_at(now, x, y), rgb_at(shown, x, y), 1));
    }

    // The refusals (nothing changed): an indexed or bitmap page, an RGB page of a profile, a blend multichannel does not
    // composite; delete_channel of a plate without to_multichannel, the last plate, a grey page's one.
    void multichannelRefusals() {
        const core::Document doc = painted(working_picture());
        const core::Document p3 = run_ops(doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).doc;
        QVERIFY(refusal(p3, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).find("of a profile") != std::string::npos);
        const core::Document indexed = run_ops(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}}})).doc;
        QVERIFY(refusal(indexed, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).find("indexed page") != std::string::npos);
        // (the name layer: kept as it is by the flatten, its blend one the plates cannot composite)
        const core::Document hue = run_ops(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"layer", "name"}, {"blend", "hue"}}})).doc;
        const std::string why = refusal(hue, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}}));
        QVERIFY2(why.find("is not one a multichannel page composites") != std::string::npos, why.c_str());
        const core::Document grey = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        QVERIFY(refusal(grey, Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "gray"}, {"to_multichannel", true}}})).find("only one") != std::string::npos);
        const core::Document one = run_ops(grey, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).doc;
        QVERIFY(refusal(one, Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "Black"}}})).find("one plate at least") != std::string::npos);
    }

    // A colour plate deleted (Photoshop's Delete Channel): the RGB page multichannel first (flattened), the plate taken out
    // of its pixels — red's is Cyan's —, the others as they were.
    void deleteColourPlate() {
        const core::Document doc = painted(working_picture());
        const std::string id = flat_id(doc);
        QVERIFY(refusal(doc, Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "green"}}})).find("to_multichannel") != std::string::npos);
        const core::ApplyResult done = run_ops(doc, Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "green"}, {"to_multichannel", true}}}));
        const core::Page& page = done.doc.page(0);
        QVERIFY(page.color && page.color->space == core::InkSpace::Multi);
        QCOMPARE(core::plate_labels(page), (std::vector<std::string>{"Cyan", "Yellow"}));
        QCOMPARE(done.results.at(0)["plates"], Json(2));
        const core::InkPixels two = core::decode_ink_raster(*layer_named(done.doc, id).ink_raster);
        const core::Document three = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).doc;
        const core::InkPixels all = core::decode_ink_raster(*layer_named(three, id).ink_raster);
        for (std::size_t i = 0; i < all.pixel_count(); i += 131) {
            QCOMPARE(two.value(i, 0), all.value(i, 0));
            QCOMPARE(two.value(i, 1), all.value(i, 2));
            QCOMPARE(two.value(i, 2), all.value(i, 3));  // (the alpha)
        }
    }

    // indexed_color_palettes_and_dithers: every palette and dither, the page indexed (8 bits, its table), its pixels the
    // table's colours only; a computed palette within its count.
    void indexedPalettesAndDithers() {
        // (a gradient of many colours at the working resolution)
        const int w = render::mm_to_px(60, 200), h = render::mm_to_px(80, 200);
        std::string bytes;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                bytes += static_cast<char>(x * 255 / (w - 1));
                bytes += static_cast<char>(y * 255 / (h - 1));
                bytes += static_cast<char>(128);
                bytes += static_cast<char>(255);
            }
        }
        const core::Document doc = painted(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
        const std::string id = flat_id(doc);
        for (const char* palette : {"selective", "perceptual", "adaptive", "web", "uniform", "systemMac", "systemWindows"}) {
            for (const char* dither : {"none", "diffusion", "pattern", "noise"}) {
                if (std::string(dither) != "none" && std::string(palette) != "selective" && std::string(palette) != "web") continue;  // (each once)
                const core::ApplyResult made = run_ops(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"palette", palette}, {"colors", 16},
                                                                             {"dither", dither}, {"forced", "none"}}}));
                const core::Page& page = made.doc.page(0);
                QVERIFY(page.rgb && page.rgb->table && page.rgb->depth == 8);
                const core::ColorTable& table = *page.rgb->table;
                QCOMPARE(Json(table.colors.size()), made.results.at(0)["colors"]);
                QCOMPARE(made.results.at(0)["flattened"], Json(1));
                if (std::string(palette) != "web" && std::string(palette) != "systemMac" && std::string(palette) != "systemWindows") QVERIFY(table.colors.size() <= 16);
                const std::string px = render::open_image(*layer_named(made.doc, id).raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
                std::set<std::array<int, 3>> own(table.colors.begin(), table.colors.end());
                for (std::size_t i = 0; i < px.size(); i += 4 * 37) {
                    const std::array<int, 3> c{static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i + 1]), static_cast<unsigned char>(px[i + 2])};
                    QVERIFY2(own.contains(c), (std::string(palette) + "/" + dither).c_str());
                }
            }
        }
        // Exact refuses many colours
        QVERIFY(refusal(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"palette", "exact"}}})).find("Exact needs 256 or fewer") != std::string::npos);
        // the arguments' ranges
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 300}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"palette", "rainbow"}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"amount", 101}}})).empty());
    }

    // indexed_exact_transparency_and_sources: an exact palette of a page of two colours (flattened over its paper: no
    // transparent entry — Genko's flatten keeps the paper); grey, CMYK and Lab pages converted to sRGB first (said); back
    // to RGB the table goes, the pixels stay.
    void indexedSourcesAndBack() {
        const core::Document doc = painted(working_picture());
        const std::string id = flat_id(doc);
        const core::ApplyResult exact = run_ops(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"palette", "exact"}, {"forced", "none"}}}));
        const core::ColorTable& table = *exact.doc.page(0).rgb->table;
        QCOMPARE(table.colors, (std::vector<std::array<int, 3>>{{30, 160, 90}, {128, 128, 128}}));
        QVERIFY(!table.transparent);
        QCOMPARE(exact.results.at(0)["transparent"], Json());
        QCOMPARE(render::open_image(*layer_named(exact.doc, id).raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes(), working_picture().tobytes());
        for (const Json& mode : {Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 16}},
                                 Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}},
                                 Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}}}) {
            const core::Document other = run_ops(doc, Json::array({mode})).doc;
            const core::ApplyResult made = run_ops(other, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}}}));
            QVERIFY(!made.doc.page(0).color && made.doc.page(0).rgb && made.doc.page(0).rgb->table);
            QCOMPARE(made.results.at(0)["converted"], Json(1));
        }
        const core::Document back = run_ops(exact.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}}})).doc;
        QVERIFY(!back.page(0).rgb);
        QCOMPARE(*layer_named(back, id).raster_png, *layer_named(exact.doc, id).raster_png);
        // (a profile is not assigned to nor converted on an indexed page)
        QVERIFY(refusal(exact.doc, Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}}})).find("indexed page") != std::string::npos);
        QVERIFY(refusal(exact.doc, Json::array({Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "display-p3"}}})).find("indexed page") != std::string::npos);
    }

    // An indexed page shown in its table (its frames and words too: every pixel drawn is one of its colours); what an op
    // draws on it taken to the table's nearest colours.
    void indexedShowsAndDrawsInItsTable() {
        core::Document doc = painted(working_picture());
        doc = run_ops(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞です"}, {"x_mm", 20}, {"y_mm", 30}},
                                        Json{{"op", "to_indexed"}, {"page", 1}, {"palette", "exact"}, {"forced", "blackWhite"}}})).doc;
        const core::ColorTable table = *doc.page(0).rgb->table;
        const std::set<std::array<int, 3>> own(table.colors.begin(), table.colors.end());
        const std::string shown = drawn_page(doc, 0, 100).tobytes();
        for (std::size_t i = 0; i < shown.size(); i += 3) {
            QVERIFY(own.contains({static_cast<unsigned char>(shown[i]), static_cast<unsigned char>(shown[i + 1]), static_cast<unsigned char>(shown[i + 2])}));
        }
        const std::string id = flat_id(doc);
        const core::Document drawn = run_ops(doc, Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", id},
                                                                     {"area", Json{{"poly", Json::array({Json::array({5, 5}), Json::array({15, 5}), Json::array({15, 15}), Json::array({5, 15})})}}},
                                                                     {"rgb", Json::array({200, 30, 40})}}})).doc;
        const std::string px = render::open_image(*layer_named(drawn, id).raster_png, render::kPillowOpenLimits).convert("RGBA").tobytes();
        for (std::size_t i = 0; i < px.size(); i += 4 * 11) {
            if (px[i + 3] == 0) continue;
            QVERIFY(own.contains({static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i + 1]), static_cast<unsigned char>(px[i + 2])}));
        }
    }

    // color_table_presets_and_entries: an entry replaced (its pixels the new colour), a preset, the transparent entry
    // (its pixels clear); a page not indexed, an unknown preset and an index past the table refused.
    void colorTableEntries() {
        const core::Document doc = run_ops(painted(working_picture()), Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"palette", "exact"}, {"forced", "none"}}})).doc;
        const std::string id = flat_id(doc);
        QVERIFY(refusal(painted(working_picture()), Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"table", "web"}}})).find("not an indexed page") != std::string::npos);
        const core::ApplyResult same = run_ops(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}}}));
        QCOMPARE(same.results.at(0)["colors"], (Json::array({Json::array({30, 160, 90}), Json::array({128, 128, 128})})));
        const core::Document magenta = run_ops(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"entries", Json{{"1", "#ff00ff"}}}}})).doc;
        QCOMPARE(rgb_at(render::open_image(*layer_named(magenta, id).raster_png, render::kPillowOpenLimits), 3, 3), (std::array<int, 3>{255, 0, 255}));
        const core::Document black_body = run_ops(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"table", "blackBody"}}})).doc;
        QCOMPARE(black_body.page(0).rgb->table->colors.size(), std::size_t(256));
        const core::Document clear = run_ops(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"transparent", 0}}})).doc;
        QCOMPARE(clear.page(0).rgb->table->transparent, std::optional<int>(0));
        const render::Image cleared = render::open_image(*layer_named(clear, id).raster_png, render::kPillowOpenLimits).convert("RGBA");
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(cleared.crop(render::Box{cleared.width() - 3, 3, cleared.width() - 2, 4}).tobytes()[3])), 0);
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"table", "nope"}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"entries", Json{{"999", "#000000"}}}}})).empty());
    }

    // bitmap_methods_need_grayscale: from a grey page only; each method's pixels 0 or full (its alpha too), the page 8
    // bits with its look recorded; shown in two (its words too); what an op draws taken to two at 50%.
    void bitmapMethods() {
        const core::Document rgb = painted(working_picture());
        QVERIFY(refusal(rgb, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}}})).find("not a grey page") != std::string::npos);
        const core::Document grey = run_ops(rgb, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 16}}})).doc;
        const std::string id = flat_id(grey);
        for (const char* method : {"threshold", "pattern", "diffusion", "halftone"}) {
            const core::ApplyResult made = run_ops(grey, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}, {"method", method}, {"frequency", 12}, {"shape", "diamond"}}}));
            const core::Page& page = made.doc.page(0);
            QVERIFY(page.color && page.color->bitmap && page.color->depth == 8);
            QCOMPARE(page.color->bitmap->method, std::string(method));
            QCOMPARE(page.color->bitmap->shape, std::string("diamond"));
            const core::InkPixels px = core::decode_ink_raster(*layer_named(made.doc, id).ink_raster);
            for (const std::uint16_t s : px.samples) QVERIFY(s == 0 || s == 255);
        }
        QVERIFY(!refusal(grey, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}, {"method", "nope"}}})).empty());
        core::Document two = run_ops(grey, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}, {"method", "threshold"}}})).doc;
        two = run_ops(two, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}, {"x_mm", 20}, {"y_mm", 30}},
                                        Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"layer_id", id}, {"points", Json::array({Json::array({5, 5}), Json::array({50, 70})})},
                                             {"width_mm", 1.3}, {"rgb", Json::array({100, 100, 100})}}})).doc;
        const std::string shown = drawn_page(two, 0, 100).tobytes();
        for (const char c : shown) QVERIFY(static_cast<unsigned char>(c) == 0 || static_cast<unsigned char>(c) == 255);
        const core::InkPixels px = core::decode_ink_raster(*layer_named(two, id).ink_raster);
        for (const std::uint16_t s : px.samples) QVERIFY(s == 0 || s == 255);
        // back to grey: nothing lost (the pixels as they are)
        const core::Document back = run_ops(two, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"},
                                                                    {"profile", two.page(0).color->profile}}})).doc;
        QVERIFY(!back.page(0).color->bitmap);
        QCOMPARE(*layer_named(back, id).ink_raster, *layer_named(two, id).ink_raster);
    }

    // duotone_inks_and_display: a type it does not know refused; the inks named in the report; shown warm in the middle
    // (an orange ink) where the grey is neutral, the pixels as they were; tritone's defaults; set_color_mode gray takes
    // the inks off.
    void duotoneInksAndDisplay() {
        const core::Document grey = run_ops(painted(working_picture()), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const std::string id = grey.page(0).layers.back().id;
        QVERIFY(!refusal(grey, Json::array({Json{{"op", "set_duotone"}, {"page", 1}, {"type", "quintone"}}})).empty());
        const core::ApplyResult made = run_ops(grey, Json::array({Json{{"op", "set_duotone"}, {"page", 1}, {"type", "duotone"},
                                                                      {"inks", Json::array({Json{{"name", "Black"}},
                                                                                            Json{{"name", "Orange"}, {"color", "#ff8000"}, {"curve", Json::array({Json::array({0, 0}), Json::array({100, 80})})}}})}}}));
        QCOMPARE(made.results.at(0)["inks"], (Json::array({"Black", "Orange"})));
        const core::Page& page = made.doc.page(0);
        QCOMPARE(page.color->duotone[1].curve.back()[1], 80.0);
        QCOMPARE(*layer_named(made.doc, id).ink_raster, *layer_named(grey, id).ink_raster);
        const auto mid = rgb_at(drawn_page(made.doc, 0, 100), 10, 10);  // (the grey 128)
        QVERIFY2(mid[0] > mid[2] + 10, qPrintable(rgb_text(mid)));
        const core::Document three = run_ops(made.doc, Json::array({Json{{"op", "set_duotone"}, {"page", 1}, {"type", "tritone"}}})).doc;
        QCOMPARE(three.page(0).color->duotone.size(), std::size_t(3));
        const core::Document off = run_ops(three, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        QVERIFY(off.page(0).color->duotone.empty());
        QCOMPARE(*layer_named(off, id).ink_raster, *layer_named(grey, id).ink_raster);
        QVERIFY(refusal(painted(working_picture()), Json::array({Json{{"op", "set_duotone"}, {"page", 1}}})).find("not a grey page") != std::string::npos);
    }

    // The display maps of these modes are pixel by pixel: a part of the page drawn the same as the whole (the tiles).
    void partsDrawAsTheWhole() {
        core::Document base = painted(working_picture());
        base = run_ops(base, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞です"}, {"x_mm", 20}, {"y_mm", 30}}})).doc;
        const core::Document grey = run_ops(base, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const std::vector<core::Document> books{
            run_ops(base, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 6}}})).doc,
            run_ops(grey, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}}})).doc,
            run_ops(grey, Json::array({Json{{"op", "set_duotone"}, {"page", 1}}})).doc,
            run_ops(base, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}})).doc,
        };
        for (const core::Document& doc : books) {
            render::RenderOptions options;
            const render::Image whole = render::render_page(doc.page(0), 120, options, &doc).image.convert("RGB");
            options.region = render::RenderRegion{37, 61, 90, 70};
            const render::Image part = render::render_page(doc.page(0), 120, options, &doc).image.convert("RGB");
            QCOMPARE(part.tobytes(), whole.crop(render::Box{37, 61, 127, 131}).tobytes());
        }
    }

    // design 4.5 (PC1d F1): an RGB page of 16 or 32 bits takes the 8-bit pixels an op makes into its depth exactly
    // (v × 257, v / 255): a paint layer's put_raster.
    void deepPagesWidenNewPixels() {
        for (const int depth : {16, 32}) {
            core::Document doc = run_ops(small_book(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}})).doc;
            doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}},
                                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "p"}, {"panel_each", false}, {"panel_clip", false}},
                                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", core::b64encode(render::write_png(picture()))}}})).doc;
            const core::Layer layer = layer_named(doc, "p");
            QVERIFY(!layer.raster_png && layer.color_raster);
            const core::ColorRasterView view(*layer.color_raster);
            QCOMPARE(view.width(), 60u);
            const auto grey = view.pixel(5), green = view.pixel(45);
            if (depth == 16) {
                QCOMPARE(std::lround(grey[0] * 65535), 128L * 257);
                QCOMPARE(std::lround(green[3] * 65535), 200L * 257);
            } else {
                QCOMPARE(static_cast<float>(grey[0]), static_cast<float>(128 / 255.0));
                QCOMPARE(static_cast<float>(green[1]), static_cast<float>(160 / 255.0));
            }
        }
        // (an 8-bit sRGB page: Python's PNG as it is)
        QVERIFY(painted(picture()).page(0).layers.back().raster_png);
    }

    // The book on disk: every mode written and read back the same (features color-model@1), its GKIR plates version 2.
    void specialModesOnDisk() {
        const core::Document base = painted(working_picture());
        core::Document doc = run_ops(core::new_episode("m", core::Num(1), 4, base.spec, core::Binding::Right), Json::array()).doc;
        doc = run_ops(base, Json::array({Json{{"op", "add_page"}}, Json{{"op", "add_page"}}, Json{{"op", "add_page"}}})).doc;
        doc = run_ops(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 4}},
                                        Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "gray"}},
                                        Json{{"op", "to_bitmap"}, {"page", 2}},
                                        Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "gray"}},
                                        Json{{"op", "set_duotone"}, {"page", 3}},
                                        Json{{"op", "set_color_mode"}, {"page", 4}, {"mode", "multichannel"}}})).doc;
        QTemporaryDir dir;
        const fs::path book = core::path_from_utf8(dir.path().toStdString()) / "b.genko";
        write_book(doc, book);
        const Json saved = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(book / "project.json"))));
        QVERIFY2(saved["features"] == Json::array({"color-mode@1", "color-model@1"}), saved["features"].dump().c_str());
        QCOMPARE(saved["pages"][0]["color"]["mode"], Json("indexed"));
        QCOMPARE(saved["pages"][1]["color"]["mode"], Json("bitmap"));
        QCOMPARE(saved["pages"][2]["color"]["mode"], Json("duotone"));
        QCOMPARE(saved["pages"][3]["color"]["mode"], Json("multichannel"));
        const storage::LoadResult read = storage::load_document(book);
        QVERIFY2(read.report.clean() && read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        for (std::size_t p = 0; p < 4; ++p) {
            QVERIFY(read.document.page(p).color == doc.page(p).color);
            QVERIFY(read.document.page(p).rgb == doc.page(p).rgb);
        }
    }

    // Review A: the name and draft layers a conversion keeps as layers (they do not print) keep their pixels as they are
    // — not taken to the table's colours, not made 0 or full —, and so do what is drawn on them afterwards and a change of
    // the table; the page shows them through its last map as it shows the rest (an indexed page: each pixel of the page
    // drawn in RGB its table's nearest; a bitmap page: its lightness 0 or full).
    void namesAndDraftsKeepTheirPixels() {
        const int w = render::mm_to_px(60, 200), h = render::mm_to_px(80, 200);
        // (the paint layer black on the left, white on the right; a pale blue pencil across it on the draft layer and a
        // dark blue sketch on the name layer, both seen through)
        render::Image picture = render::Image::create("RGBA", render::Size{w, h}, render::Ink{255, 255, 255, 255});
        picture.paste(render::Ink{0, 0, 0, 255}, render::Box{0, 0, w / 2, h});
        render::Image pencil = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        pencil.paste(render::Ink{120, 170, 255, 170}, render::Box{0, h / 4, w, h / 4 + 40});
        render::Image sketch = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        sketch.paste(render::Ink{40, 60, 120, 200}, render::Box{0, h / 2, w, h / 2 + 40});
        const core::Document doc =
            run_ops(painted(picture), Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"layer", "draft"}, {"png_base64", core::b64encode(render::write_png(pencil))}},
                                                   Json{{"op", "put_raster"}, {"page", 1}, {"layer", "name"}, {"png_base64", core::b64encode(render::write_png(sketch))}}}))
                .doc;
        const auto own = [](const core::Document& d, core::LayerRole role) -> std::string {
            const core::Layer& layer = role_layer(d, role);
            if (layer.ink_raster) return *layer.ink_raster;
            return layer.raster_png ? *layer.raster_png : std::string();
        };
        QVERIFY(!own(doc, core::LayerRole::Draft).empty() && !own(doc, core::LayerRole::Name).empty());
        const auto proof = [](const core::Document& d) {  // (the page as the screen shows it: its name and draft layers too)
            render::RenderOptions options;
            options.mode = "proof";
            return render::render_page(d.page(0), 100, options, &d).image.convert("RGB");
        };
        // (pixels drawn on the layer afterwards: the pencil again with a block of a colour at 12 × 30 mm)
        const int fx = static_cast<int>(12 * 200 / 25.4), fy = static_cast<int>(30 * 200 / 25.4);
        const auto draw_on = [&](const core::Document& d, const char* role, render::Ink colour) {
            render::Image again = pencil;
            again.paste(colour, render::Box{fx - 20, fy - 20, fx + 20, fy + 20});
            return run_ops(d, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"layer", role}, {"png_base64", core::b64encode(render::write_png(again))}}})).doc;
        };
        // インデックスカラー: the name and draft layers as they were; the page shows the RGB page's colours in its table
        const core::ApplyResult indexed = run_ops(doc, Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}}}));
        QCOMPARE(indexed.results.at(0)["flattened"], Json(1));
        for (const auto role : {core::LayerRole::Draft, core::LayerRole::Name}) QVERIFY(own(indexed.doc, role) == own(doc, role));
        const auto shown_in_table = [&](const core::Document& d) {
            const core::ColorTable& table = *d.page(0).rgb->table;
            core::Document plain = d;
            plain.edit_page(0).rgb.reset();
            std::string px = proof(plain).tobytes();
            for (std::size_t i = 0; i + 2 < px.size(); i += 3) {
                const std::array<int, 3>& c = table.colors[table.nearest(static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i + 1]),
                                                                         static_cast<unsigned char>(px[i + 2]))];
                for (unsigned k = 0; k < 3; ++k) px[i + k] = static_cast<char>(c[k]);
            }
            return proof(d).tobytes() == px;
        };
        QVERIFY(shown_in_table(indexed.doc));
        // … drawn on afterwards: its own colour (10, 200, 250 is none of the table's)
        const core::Document drawn = draw_on(indexed.doc, "draft", render::Ink{10, 200, 250, 255});
        const core::Layer& pencil_now = role_layer(drawn, core::LayerRole::Draft);
        QCOMPARE(rgb_at(render::open_image(*pencil_now.raster_png, render::kPillowOpenLimits), fx, fy), (std::array<int, 3>{10, 200, 250}));
        QVERIFY(shown_in_table(drawn));
        // … the table changed: the name and draft layers as they were
        const core::Document retabled = run_ops(drawn, Json::array({Json{{"op", "set_color_table"}, {"page", 1}, {"entries", Json{{"0", "#ff00ff"}}}}})).doc;
        for (const auto role : {core::LayerRole::Draft, core::LayerRole::Name}) QVERIFY(own(retabled, role) == own(drawn, role));
        QVERIFY(shown_in_table(retabled));
        // 2 階調: the name and draft layers as they were (their grey and alpha); the page shows 0 or full of what it draws —
        // the pale pencil over the black is dark (not a white line), over the white white; the dark sketch dark
        const core::Document grey = run_ops(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})).doc;
        const core::ApplyResult two = run_ops(grey, Json::array({Json{{"op", "to_bitmap"}, {"page", 1}, {"method", "threshold"}}}));
        for (const auto role : {core::LayerRole::Draft, core::LayerRole::Name}) QVERIFY(own(two.doc, role) == own(grey, role));
        const render::Image shown = proof(two.doc);
        for (const char c : shown.tobytes()) QVERIFY(static_cast<unsigned char>(c) == 0 || static_cast<unsigned char>(c) == 255);
        const int pencil_y = (h / 4 + 20) / 2, sketch_y = (h / 2 + 20) / 2, left = w / 8, right = w / 2 - w / 8;  // (100 dpi)
        QCOMPARE(rgb_at(shown, left, pencil_y), (std::array<int, 3>{0, 0, 0}));
        QCOMPARE(rgb_at(shown, right, pencil_y), (std::array<int, 3>{255, 255, 255}));
        QCOMPARE(rgb_at(shown, right, sketch_y), (std::array<int, 3>{0, 0, 0}));
        // … drawn on afterwards: the grey as drawn (100: not 0 nor full)
        const core::Document greyed = draw_on(two.doc, "draft", render::Ink{100, 100, 100, 255});
        const core::InkPixels inks = core::decode_ink_raster(*role_layer(greyed, core::LayerRole::Draft).ink_raster);
        const std::size_t at = std::size_t(fy) * inks.width + std::size_t(fx);
        QCOMPARE(inks.samples.at(at * 2), std::uint16_t(100));
        QCOMPARE(inks.samples.at(at * 2 + 1), std::uint16_t(255));
        for (const char c : proof(greyed).tobytes()) QVERIFY(static_cast<unsigned char>(c) == 0 || static_cast<unsigned char>(c) == 255);
    }

    // Review C: the hidden paint layers a conversion's flatten discards are said in its result ("discarded"; not written
    // when there are none): to_indexed, multichannel, a colour plate deleted, to_bitmap, a spot channel merged.
    void discardedHiddenLayersAreSaid() {
        const core::Document plain = painted(working_picture());
        const core::Document doc = run_ops(plain, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "hid"}},
                                                               Json{{"op", "set_layer"}, {"page", 1}, {"id", "hid"}, {"panel_each", false}, {"panel_clip", false}},
                                                               Json{{"op", "put_raster"}, {"page", 1}, {"id", "hid"}, {"png_base64", core::b64encode(render::write_png(picture()))}},
                                                               Json{{"op", "set_layer"}, {"page", 1}, {"id", "hid"}, {"visible", false}}}))
                                       .doc;
        const auto kept = [](const core::Document& d) {
            return std::any_of(d.page(0).layers.begin(), d.page(0).layers.end(), [](const core::Layer& l) { return l.id == "hid"; });
        };
        QVERIFY(kept(doc));
        const Json grey{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}};
        const std::vector<Json> conversions{
            Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}}}),
            Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}}),
            Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "green"}, {"to_multichannel", true}}}),
            Json::array({grey, Json{{"op", "to_bitmap"}, {"page", 1}}}),
            Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}}, Json{{"op", "merge_spot_channel"}, {"page", 1}}}),
        };
        for (const Json& ops : conversions) {
            const core::ApplyResult done = run_ops(doc, ops);
            QVERIFY2(done.results.back().value("discarded", Json()) == Json(1), (ops.back().dump() + " → " + done.results.back().dump()).c_str());
            QVERIFY(!kept(done.doc));
            const core::ApplyResult none = run_ops(plain, ops);
            QVERIFY2(!none.results.back().contains("discarded"), none.results.back().dump().c_str());
        }
    }

    // Review D: an indexed page holds 8-bit pixels only (design 2.3): put_color_raster on it is refused, nothing changed;
    // a book with such a layer on an indexed page opens read-only.
    void indexedPagesHoldEightBitPixelsOnly() {
        const Json precise{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
                           {"pixels", Json::array({0.3, 0.7, 0.1, 1.0, 0.9, 0.2, 0.6, 1.0})}};
        const core::Document indexed = run_ops(painted(working_picture()), Json::array({Json{{"op", "to_indexed"}, {"page", 1}, {"colors", 8}}})).doc;
        const std::string why = refusal(indexed, Json::array({precise}));
        QVERIFY2(why.find("indexed page") != std::string::npos, why.c_str());
        // (on disk: an sRGB page's precise layer, the page then written indexed by hand)
        const core::Document doc = run_ops(painted(working_picture()), Json::array({precise})).doc;
        QTemporaryDir dir;
        const fs::path book = core::path_from_utf8(dir.path().toStdString()) / "b.genko";
        write_book(doc, book);
        const QString project = QString::fromStdString(core::path_to_utf8(book / "project.json"));
        Json saved = Json::parse(genko::test::read_bytes(project));
        const auto write = [&](const Json& value) {
            QFile file(project);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(value.dump()));
        };
        for (const char* feature : {"color-mode@1", "color-model@1"}) saved["features"].push_back(feature);
        saved["pages"][0]["color"] = Json::parse(R"({"mode":"indexed","depth":8,"intent":"relative","table":{"colors":[[0,0,0],[255,255,255]],"transparent":null}})");
        Json without = saved;  // (the same page without the precise layer: it reads)
        Json& layers = without["pages"][0]["layers"];
        layers.erase(std::remove_if(layers.begin(), layers.end(), [](const Json& l) { return l.contains("color_raster"); }), layers.end());
        QVERIFY(layers.size() + 1 == saved["pages"][0]["layers"].size());
        write(without);
        const storage::LoadResult clean = storage::load_document(book);
        QVERIFY2(clean.document.read_only_reason.empty(), clean.document.read_only_reason.c_str());
        QVERIFY(clean.document.page(0).rgb && clean.document.page(0).rgb->table);
        write(saved);
        const storage::LoadResult read = storage::load_document(book);
        QVERIFY2(read.document.read_only_reason.find("indexed page") != std::string::npos, read.document.read_only_reason.c_str());
    }
};

QTEST_GUILESS_MAIN(TestColorModel)
#include "test_color_model.moc"
