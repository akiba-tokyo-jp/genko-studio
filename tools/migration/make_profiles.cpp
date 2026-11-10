// The built-in RGB and grey ICC profiles of the C++ build (color-model@1, docs/cpp-migration/schema-v4.md「色モデル」):
// made once with Little CMS from the published primaries, white points and tone curves PhotoCraft v0.2.0 names
// (crates/cms/src/builtin.rs:229-268, MIT), their header date fixed, so the same bytes come out each time. The bytes are
// kept in native/src/core/data/profiles/ and checked against their sha256 (core/color_model.cpp, test_color_model):
// run this only to make them again, and change those constants with them.
//
//   c++ -std=c++20 -O2 tools/migration/make_profiles.cpp -llcms2 -o /tmp/make_profiles
//   /tmp/make_profiles native/src/core/data/profiles
//
// sRGB is not written (it is Little CMS's own, as a page with no profile), nor Lab D50 (Little CMS's Lab v4), nor the
// coated CMYK profile (PhotoCraft's crates/cms/profiles/photocraft-coated-cmyk.icc, CC0-1.0, copied as it is).

#include <lcms2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kCopyright = "No copyright: CC0-1.0 public domain dedication. Made by Genko Studio with Little CMS.";

// The date every profile's header says (2026-10-16 00:00:00 UTC).
constexpr unsigned kDate[6] = {2026, 10, 16, 0, 0, 0};

[[noreturn]] void fail(const std::string& why) {
    std::fprintf(stderr, "make_profiles: %s\n", why.c_str());
    std::exit(1);
}

void set_text(cmsHPROFILE profile, cmsTagSignature tag, const char* text) {
    cmsMLU* mlu = cmsMLUalloc(nullptr, 1);
    if (mlu == nullptr || !cmsMLUsetASCII(mlu, "en", "US", text) || !cmsWriteTag(profile, tag, mlu)) fail("cannot write a text tag");
    cmsMLUfree(mlu);
}

std::vector<unsigned char> save(cmsHPROFILE profile) {
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size)) fail("cannot size the profile");
    std::vector<unsigned char> out(size);
    if (!cmsSaveProfileToMem(profile, out.data(), &size)) fail("cannot write the profile");
    out.resize(size);
    return out;
}

// The profile's bytes with the fixed date, and its profile ID (MD5) computed over them.
std::vector<unsigned char> finished(cmsHPROFILE profile, const char* description) {
    set_text(profile, cmsSigProfileDescriptionTag, description);
    set_text(profile, cmsSigCopyrightTag, kCopyright);
    cmsSetHeaderRenderingIntent(profile, INTENT_PERCEPTUAL);
    std::vector<unsigned char> bytes = save(profile);
    cmsCloseProfile(profile);
    for (unsigned i = 0; i < 6; ++i) {  // (dateTimeNumber at 24: six big-endian u16)
        bytes[24 + 2 * i] = static_cast<unsigned char>(kDate[i] >> 8);
        bytes[25 + 2 * i] = static_cast<unsigned char>(kDate[i] & 255);
    }
    cmsHPROFILE again = cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size()));
    if (again == nullptr || !cmsMD5computeID(again)) fail("cannot compute the profile ID");
    std::vector<unsigned char> out = save(again);
    cmsCloseProfile(again);
    return out;
}

cmsToneCurve* srgb_trc() {
    const cmsFloat64Number p[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    return cmsBuildParametricToneCurve(nullptr, 4, p);
}

cmsToneCurve* gamma(double g) { return cmsBuildGamma(nullptr, g); }

// Rec. 709's inverse OETF: V = 1.099 L^0.45 − 0.099 (L ≥ 0.018), 4.5 L below (ICC parametric type 3).
cmsToneCurve* rec709_trc() {
    const double a = 1.099;
    const cmsFloat64Number p[5] = {1.0 / 0.45, 1.0 / a, (a - 1.0) / a, 1.0 / 4.5, 0.081};
    return cmsBuildParametricToneCurve(nullptr, 4, p);
}

cmsCIExyY d65() { return cmsCIExyY{0.3127, 0.3290, 1.0}; }

std::vector<unsigned char> rgb(const char* description, const double prim[3][2], cmsCIExyY white, cmsToneCurve* trc) {
    const cmsCIExyYTRIPLE primaries{{prim[0][0], prim[0][1], 1.0}, {prim[1][0], prim[1][1], 1.0}, {prim[2][0], prim[2][1], 1.0}};
    cmsToneCurve* curves[3] = {trc, trc, trc};
    cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(trc);
    if (profile == nullptr) fail(std::string("cannot make ") + description);
    return finished(profile, description);
}

std::vector<unsigned char> grey(const char* description, cmsToneCurve* trc) {
    cmsHPROFILE profile = cmsCreateGrayProfile(cmsD50_xyY(), trc);
    cmsFreeToneCurve(trc);
    if (profile == nullptr) fail(std::string("cannot make ") + description);
    return finished(profile, description);
}

void write(const std::string& folder, const char* id, const std::vector<unsigned char>& bytes) {
    const std::string path = folder + "/" + id + ".icc";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) fail("cannot write " + path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) fail("usage: make_profiles <folder>");
    const std::string folder = argv[1];
    const double srgb[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
    const double p3[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
    const double adobe[3][2] = {{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}};
    const double romm[3][2] = {{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}};
    const double bt2020[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
    write(folder, "display-p3", rgb("Display P3", p3, d65(), srgb_trc()));
    write(folder, "adobe-rgb-compat", rgb("Adobe RGB (1998)-compatible (Photocraft)", adobe, d65(), gamma(563.0 / 256.0)));
    write(folder, "prophoto-compat", rgb("ProPhoto RGB-compatible (ROMM primaries, Photocraft)", romm, *cmsD50_xyY(), gamma(1.8)));
    write(folder, "linear-srgb", rgb("Linear sRGB (Photocraft)", srgb, d65(), gamma(1.0)));
    write(folder, "rec2020", rgb("Rec. ITU-R BT.2020 (Photocraft)", bt2020, d65(), rec709_trc()));
    write(folder, "gray-gamma-2.2", grey("Gray Gamma 2.2 (Photocraft)", gamma(2.2)));
    write(folder, "sgray", grey("sGray (sRGB tone curve, Photocraft)", srgb_trc()));
    return 0;
}
