#include "render/colour.hpp"

#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/imaging.hpp"

namespace genko::render::colour {

namespace {

constexpr std::array<std::string_view, 4> kIntents{"perceptual", "relative", "saturation", "absolute"};

struct Profile {
    cmsHPROFILE handle = nullptr;
    explicit Profile(cmsHPROFILE h) : handle(h) {}
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
    ~Profile() {
        if (handle != nullptr) cmsCloseProfile(handle);
    }
};

struct Transform {
    cmsHTRANSFORM handle = nullptr;
    explicit Transform(cmsHTRANSFORM h) : handle(h) {}
    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;
    ~Transform() {
        if (handle != nullptr) cmsDeleteTransform(handle);
    }
};

// colour._profile: the file read as an ICC profile (Python: ValueError when it cannot be)
Profile open_profile(const std::filesystem::path& icc) {
    const std::string name = core::path_to_utf8(icc);
    cmsHPROFILE handle = cmsOpenProfileFromFile(name.c_str(), "r");
    if (handle == nullptr) throw core::PyValueError("the colour profile cannot be read (cannot open profile file)");
    return Profile(handle);
}

std::uint32_t intent_of(std::string_view intent) {
    const auto at = std::find(kIntents.begin(), kIntents.end(), intent);
    if (at == kIntents.end()) throw core::PyValueError("tuple.index(x): x not in tuple");
    return static_cast<std::uint32_t>(at - kIntents.begin());
}

// ImageCms.applyTransform: each row through the transform (Pillow's RGB rows are four bytes a pixel, as TYPE_RGBA_8
// reads them; CMYK rows TYPE_CMYK_8)
Image apply(const Image& in, std::string_view out_mode, cmsHTRANSFORM transform) {
    Image out = Image::create(std::string(out_mode), in.size(), out_mode == "CMYK" ? Ink{0, 0, 0, 0} : Ink{0, 0, 0});
    for (int y = 0; y < in.height(); ++y)
        cmsDoTransform(transform, in.raw()->image[y], out.raw()->image[y], static_cast<cmsUInt32Number>(in.width()));
    return out;
}

}  // namespace

std::string srgb_icc() {
    const Profile profile(cmsCreate_sRGBProfile());
    cmsUInt32Number size = 0;
    if (profile.handle == nullptr || !cmsSaveProfileToMem(profile.handle, nullptr, &size)) throw core::Error("value", "the sRGB profile cannot be made");
    std::string out(size, '\0');
    if (!cmsSaveProfileToMem(profile.handle, out.data(), &size)) throw core::Error("value", "the sRGB profile cannot be made");
    out.resize(size);
    return out;
}

std::string profile_name(const std::filesystem::path& icc) {
    const Profile profile = open_profile(icc);
    std::string text;
    if (cmsIsTag(profile.handle, cmsSigProfileDescriptionTag)) {
        if (const auto* mlu = static_cast<const cmsMLU*>(cmsReadTag(profile.handle, cmsSigProfileDescriptionTag))) {
            const cmsUInt32Number bytes = cmsMLUgetWide(mlu, "en", cmsNoCountry, nullptr, 0);
            std::vector<wchar_t> wide(bytes / sizeof(wchar_t) + 1, L'\0');
            if (bytes > 0) cmsMLUgetWide(mlu, "en", cmsNoCountry, wide.data(), bytes);
            for (const wchar_t c : wide) {  // (UTF-8)
                if (c == L'\0') break;
                const auto u = static_cast<std::uint32_t>(c);
                if (u < 0x80) text += static_cast<char>(u);
                else if (u < 0x800) { text += static_cast<char>(0xc0 | (u >> 6)); text += static_cast<char>(0x80 | (u & 0x3f)); }
                else if (u < 0x10000) { text += static_cast<char>(0xe0 | (u >> 12)); text += static_cast<char>(0x80 | ((u >> 6) & 0x3f)); text += static_cast<char>(0x80 | (u & 0x3f)); }
                else { text += static_cast<char>(0xf0 | (u >> 18)); text += static_cast<char>(0x80 | ((u >> 12) & 0x3f)); text += static_cast<char>(0x80 | ((u >> 6) & 0x3f)); text += static_cast<char>(0x80 | (u & 0x3f)); }
            }
        }
    }
    // .strip() or the file's name
    const auto first = text.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return core::path_to_utf8(icc.filename());
    const auto last = text.find_last_not_of(" \t\n\r\f\v");
    return text.substr(first, last - first + 1);
}

bool is_cmyk_profile(const std::filesystem::path& icc) {
    const Profile profile = open_profile(icc);
    return cmsGetColorSpace(profile.handle) == cmsSigCmykData;
}

Image to_cmyk(const Image& image, const std::optional<std::filesystem::path>& icc, int ink_limit, std::string_view intent) {
    const Image rgb = image.convert("RGB");
    if (icc) {
        if (!is_cmyk_profile(*icc)) throw core::PyValueError("the profile is not a CMYK printing profile");
        const Profile in(cmsCreate_sRGBProfile());
        const Profile out = open_profile(*icc);
        const Transform transform(cmsCreateTransform(in.handle, TYPE_RGBA_8, out.handle, TYPE_CMYK_8, intent_of(intent),
                                                     cmsFLAGS_BLACKPOINTCOMPENSATION));
        if (transform.handle == nullptr) throw core::PyValueError("the colour profile cannot be used for CMYK");
        return apply(rgb, "CMYK", transform.handle);
    }
    // grey component replacement, in numpy's float32 steps
    const std::string bytes = rgb.tobytes();
    const std::size_t n = static_cast<std::size_t>(rgb.width()) * static_cast<std::size_t>(rgb.height());
    const float limit = static_cast<float>(ink_limit / 100.0);
    std::string cmyk(n * 4, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        std::array<float, 3> a{};
        for (unsigned c = 0; c < 3; ++c) a[c] = static_cast<float>(static_cast<unsigned char>(bytes[i * 3 + c])) / 255.0f;
        const float k = 1.0f - std::max({a[0], a[1], a[2]});
        const float denom = k < 1.0f ? 1.0f - k : 1.0f;
        std::array<float, 4> out{};
        for (unsigned c = 0; c < 3; ++c) out[c] = ((1.0f - a[c]) - k) / denom;
        out[3] = k;
        const float total = ((out[0] + out[1]) + out[2]) + out[3];
        if (total > limit) {
            const float room = std::max(limit - k, 0.0f);
            const float cmy = (out[0] + out[1]) + out[2];
            const float scale = cmy > 0.0f ? room / std::max(cmy, 1e-6f) : 1.0f;
            for (unsigned c = 0; c < 3; ++c) out[c] = out[c] * scale;
        }
        for (unsigned c = 0; c < 4; ++c)
            cmyk[i * 4 + c] = static_cast<char>(static_cast<unsigned char>(std::clamp(out[c], 0.0f, 1.0f) * 255.0f + 0.5f));
    }
    return Image::frombytes("CMYK", rgb.size(), cmyk);
}

Image from_cmyk(const Image& image, const std::optional<std::filesystem::path>& icc, std::string_view intent) {
    if (icc) {
        const Profile in = open_profile(*icc);
        const Profile out(cmsCreate_sRGBProfile());
        const Transform transform(cmsCreateTransform(in.handle, TYPE_CMYK_8, out.handle, TYPE_RGBA_8, intent_of(intent), 0));
        if (transform.handle == nullptr) throw core::PyValueError("the colour profile cannot be used for CMYK");
        return apply(image.convert("CMYK"), "RGB", transform.handle);
    }
    return image.convert("RGB");
}

Image proof(const Image& image, const std::optional<std::filesystem::path>& icc) {
    const bool rgba = image.mode() == "RGBA";
    Image out = from_cmyk(to_cmyk(image, icc), icc);
    if (rgba) {
        out = out.convert("RGBA");
        out.putalpha(image.getchannel(3));
    }
    return out;
}

double ink_coverage(const Image& image) {
    const std::string bytes = image.convert("CMYK").tobytes();
    int most = 0;
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        int sum = 0;
        for (unsigned c = 0; c < 4; ++c) sum += static_cast<unsigned char>(bytes[i + c]);
        most = std::max(most, sum);
    }
    return static_cast<double>(most) / 255 * 100;
}

}  // namespace genko::render::colour
