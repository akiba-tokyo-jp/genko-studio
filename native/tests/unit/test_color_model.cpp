// color-model@1 (SPEC.md COLOR-03, schema-v4.md「色モデル（color-model@1）」): the built-in ICC profiles (their bytes
// against their sha256, what Little CMS reads them as, PhotoCraft's ids and aliases).

#include <QtTest>

#include <lcms2.h>

#include <array>
#include <cmath>
#include <string>

#include "core/color_model.hpp"
#include "core/ink.hpp"
#include "testsupport.hpp"

using namespace genko;

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
};

QTEST_GUILESS_MAIN(TestColorModel)
#include "test_color_model.moc"
