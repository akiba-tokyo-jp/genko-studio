// color-model@1 (SPEC.md COLOR-03, schema-v4.md「色モデル（color-model@1）」): the built-in ICC profiles (their bytes
// against their sha256, what Little CMS reads them as, PhotoCraft's ids and aliases).

#include <QtTest>

#include <QTemporaryDir>

#include <lcms2.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "render/ink.hpp"
#include "render/ops_registry.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

[[maybe_unused]] core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

[[maybe_unused]] std::string refusal(const core::Document& doc, const Json& ops) {
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
};

QTEST_GUILESS_MAIN(TestColorModel)
#include "test_color_model.moc"
