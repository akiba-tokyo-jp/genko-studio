#include "render/ink.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/error.hpp"
#include "render/imaging.hpp"
#include "storage/reader.hpp"

namespace genko::render::ink {

namespace {

using core::InkPixels;
using core::InkSpace;
using core::PageColor;

struct ProfileHandle {
    cmsHPROFILE handle = nullptr;
    explicit ProfileHandle(cmsHPROFILE h) : handle(h) {}
    ProfileHandle(const ProfileHandle&) = delete;
    ProfileHandle& operator=(const ProfileHandle&) = delete;
    ~ProfileHandle() {
        if (handle != nullptr) cmsCloseProfile(handle);
    }
};

struct TransformHandle {
    cmsHTRANSFORM handle = nullptr;
    explicit TransformHandle(cmsHTRANSFORM h) : handle(h) {}
    TransformHandle(const TransformHandle&) = delete;
    TransformHandle& operator=(const TransformHandle&) = delete;
    ~TransformHandle() {
        if (handle != nullptr) cmsDeleteTransform(handle);
    }
};
using Transform = std::shared_ptr<const TransformHandle>;

std::uint32_t intent_number(std::string_view intent) {
    const auto at = std::find(core::kIntents.begin(), core::kIntents.end(), intent);
    if (at == core::kIntents.end()) throw core::Error("value", "unknown rendering intent " + std::string(intent));
    return static_cast<std::uint32_t>(at - core::kIntents.begin());
}

// The page's profile (CMYK: its ICC bytes; Lab: Little CMS's D50 Lab v4).
std::unique_ptr<ProfileHandle> page_profile(const PageColor& color) {
    if (color.space == InkSpace::Lab) return std::make_unique<ProfileHandle>(cmsCreateLab4Profile(nullptr));
    if (!color.icc) throw core::Error("value", "the page's ICC profile " + color.profile + " is not held");
    cmsHPROFILE handle = cmsOpenProfileFromMem(color.icc->data(), static_cast<cmsUInt32Number>(color.icc->size()));
    if (handle == nullptr) throw core::Error("value", "the page's ICC profile cannot be read");
    return std::make_unique<ProfileHandle>(handle);
}

std::string page_key(const PageColor& color) { return color.space == InkSpace::Lab ? std::string("lab") : color.profile; }

cmsUInt32Number float_format(InkSpace space) { return space == InkSpace::Cmyk ? TYPE_CMYK_FLT : TYPE_Lab_FLT; }
cmsUInt32Number int_format(InkSpace space, int depth) {
    if (space == InkSpace::Cmyk) return depth == 16 ? TYPE_CMYK_16 : TYPE_CMYK_8;
    return depth == 16 ? TYPE_Lab_16 : TYPE_Lab_8;
}

std::mutex cache_lock;
std::unordered_map<std::string, Transform> cache;

// A transform made once (its key says what it is), or made now by `make`.
template <class Make>
Transform cached(const std::string& key, Make make) {
    {
        const std::lock_guard<std::mutex> held(cache_lock);
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
    }
    cmsHTRANSFORM handle = make();
    if (handle == nullptr) throw core::Error("value", "the colour transform cannot be made (" + key + ")");
    auto made = std::make_shared<const TransformHandle>(handle);
    const std::lock_guard<std::mutex> held(cache_lock);
    if (cache.size() >= 64) cache.clear();
    return cache.emplace(key, std::move(made)).first->second;
}

constexpr cmsUInt32Number kFlags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE;

// sRGB (`in`) → the page (`out`), the page's intent.
Transform from_srgb_transform(const PageColor& color, cmsUInt32Number in, cmsUInt32Number out) {
    const std::string key = "srgb>" + page_key(color) + ":" + color.intent + ":" + std::to_string(in) + ":" + std::to_string(out);
    return cached(key, [&] {
        const ProfileHandle srgb(cmsCreate_sRGBProfile());
        const auto page = page_profile(color);
        return cmsCreateTransform(srgb.handle, in, page->handle, out, intent_number(color.intent), kFlags);
    });
}

// The page (`in`) → sRGB (`out`) for the screen: relative colorimetric.
Transform to_srgb_transform(const PageColor& color, cmsUInt32Number in, cmsUInt32Number out) {
    const std::string key = page_key(color) + ">srgb:" + std::to_string(in) + ":" + std::to_string(out);
    return cached(key, [&] {
        const ProfileHandle srgb(cmsCreate_sRGBProfile());
        const auto page = page_profile(color);
        return cmsCreateTransform(page->handle, in, srgb.handle, out, INTENT_RELATIVE_COLORIMETRIC, kFlags);
    });
}

// One page mode → another (CMYK → CMYK keeps the black plate).
Transform between_transform(const PageColor& from, const PageColor& to, cmsUInt32Number in, cmsUInt32Number out) {
    const std::string key = page_key(from) + ">" + page_key(to) + ":" + to.intent + ":" + std::to_string(in) + ":" + std::to_string(out);
    return cached(key, [&] {
        const auto a = page_profile(from);
        const auto b = page_profile(to);
        std::uint32_t intent = intent_number(to.intent);
        if (from.space == InkSpace::Cmyk && to.space == InkSpace::Cmyk && intent != INTENT_ABSOLUTE_COLORIMETRIC) {
            intent = intent == INTENT_PERCEPTUAL ? INTENT_PRESERVE_K_ONLY_PERCEPTUAL
                     : intent == INTENT_SATURATION ? INTENT_PRESERVE_K_ONLY_SATURATION
                                                   : INTENT_PRESERVE_K_ONLY_RELATIVE_COLORIMETRIC;
        }
        return cmsCreateTransform(a->handle, in, b->handle, out, intent, kFlags);
    });
}

// The ink of a grey v (0..255) by the rules (CMYK: K alone; Lab: its lightness, no colour).
void grey_ink(const PageColor& color, int v, double* out) {
    if (color.space == InkSpace::Cmyk) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = v == 0 || v == 10 || v == 15 || v == 20 ? 1.0 : 1.0 - v / 255.0;
        return;
    }
    if (v == 0 || v == 10 || v == 15 || v == 20) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    if (v == 255) {
        out[0] = 100.0;
        out[1] = out[2] = 0.0;
        return;
    }
    const unsigned char rgb[3] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v), static_cast<unsigned char>(v)};
    float lab[3];
    cmsDoTransform(from_srgb_transform(color, TYPE_RGB_8, TYPE_Lab_FLT)->handle, rgb, lab, 1);
    out[0] = std::clamp<double>(lab[0], 0.0, 100.0);
    out[1] = out[2] = 0.0;
}

// Little CMS's float CMYK is 0..100: ours 0..1.
void from_lcms(InkSpace space, float* values, std::size_t count) {
    if (space != InkSpace::Cmyk) return;
    for (std::size_t i = 0; i < count * 4; ++i) values[i] = std::clamp(values[i] / 100.0f, 0.0f, 1.0f);
}

std::uint16_t clamp_sample(double v, double most) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, most))); }

// The samples of one pixel's colour (not its alpha) of an 8- or 16-bit raster, as a 16-bit transform reads them.
std::vector<std::uint16_t> widened(const InkPixels& pixels) {
    const unsigned ch = core::colour_channels(pixels.space), all = pixels.channels();
    std::vector<std::uint16_t> out(pixels.pixel_count() * ch);
    for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
        for (unsigned c = 0; c < ch; ++c) {
            const std::uint16_t v = pixels.samples[i * all + c];
            out[i * ch + c] = pixels.depth == 8 ? static_cast<std::uint16_t>(v * 257) : v;
        }
    }
    return out;
}

std::uint16_t narrowed(std::uint16_t v16, int depth) {
    return depth == 16 ? v16 : static_cast<std::uint16_t>((static_cast<unsigned>(v16) * 255 + 32767) / 65535);
}

std::uint16_t alpha_to(std::uint16_t a, int from, int to) {
    if (from == to) return a;
    return from == 8 ? static_cast<std::uint16_t>(a * 257) : narrowed(a, 8);
}

// The profile check of the book reader (storage/reader.hpp set_profile_check). (Set when this file is linked: every
// program that draws a CMYK page draws it through it.)
const bool kReaderCheck = [] {
    storage::set_profile_check([](std::string_view icc) { return profile_problem(icc); });
    return true;
}();

}  // namespace

std::optional<std::string> profile_problem(std::string_view icc) {
    if (icc.empty()) return std::string("the profile is empty");
    if (icc.size() > core::kProfileMaxBytes) return std::string("the profile is larger than 16 MiB");
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) return std::string("Little CMS cannot read it as an ICC profile");
    const ProfileHandle held(handle);
    if (cmsGetColorSpace(handle) != cmsSigCmykData) return std::string("it is not a CMYK profile");
    const ProfileHandle srgb(cmsCreate_sRGBProfile());
    cmsHTRANSFORM probe = cmsCreateTransform(srgb.handle, TYPE_RGB_8, handle, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC, kFlags);
    if (probe == nullptr) return std::string("it cannot take colours in (no transform from sRGB)");
    cmsDeleteTransform(probe);
    probe = cmsCreateTransform(handle, TYPE_CMYK_8, srgb.handle, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, kFlags);
    if (probe == nullptr) return std::string("its colours cannot be shown (no transform to sRGB)");
    cmsDeleteTransform(probe);
    return std::nullopt;
}

std::string profile_description(std::string_view icc) {
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) return {};
    const ProfileHandle held(handle);
    char buffer[512] = {};
    const cmsUInt32Number n = cmsGetProfileInfoASCII(handle, cmsInfoDescription, "en", "US", buffer, sizeof(buffer));
    if (n == 0) return {};
    std::string out(buffer);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

std::vector<double> ink_of(const PageColor& color, const std::array<int, 3>& rgb) {
    const unsigned char bytes[3] = {static_cast<unsigned char>(std::clamp(rgb[0], 0, 255)), static_cast<unsigned char>(std::clamp(rgb[1], 0, 255)),
                                    static_cast<unsigned char>(std::clamp(rgb[2], 0, 255))};
    double out[4] = {};
    inks_of(color, bytes, nullptr, out, 1);
    return std::vector<double>(out, out + core::colour_channels(color.space));
}

std::vector<double> ink_of_srgb(const PageColor& color, const std::array<double, 3>& srgb) {
    float in[3];
    for (unsigned c = 0; c < 3; ++c) in[c] = static_cast<float>(std::clamp(srgb[c], 0.0, 1.0));
    float out[4] = {};
    cmsDoTransform(from_srgb_transform(color, TYPE_RGB_FLT, float_format(color.space))->handle, in, out, 1);
    from_lcms(color.space, out, 1);
    return std::vector<double>(out, out + core::colour_channels(color.space));
}

void inks_of(const PageColor& color, const unsigned char* rgb, const unsigned char* alpha, double* out, std::size_t count) {
    const unsigned ch = core::colour_channels(color.space);
    std::vector<unsigned char> pending;
    std::vector<std::size_t> where;
    std::unordered_map<std::uint32_t, std::array<double, 4>> known;
    for (std::size_t i = 0; i < count; ++i) {
        if (alpha != nullptr && alpha[i] == 0) continue;
        const std::array<int, 3> c{rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
        const std::uint32_t key = (std::uint32_t(c[0]) << 16) | (std::uint32_t(c[1]) << 8) | std::uint32_t(c[2]);
        if (const auto it = known.find(key); it != known.end()) {
            std::copy_n(it->second.begin(), ch, out + i * ch);
            continue;
        }
        std::array<double, 4> ink{};
        if (const core::InkEntry* entry = core::ink_for(color, c)) {
            for (unsigned k = 0; k < ch; ++k) ink[k] = entry->ink[k];
        } else if (c[0] == c[1] && c[1] == c[2]) {
            grey_ink(color, c[0], ink.data());
        } else {
            pending.insert(pending.end(), {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]});
            where.push_back(i);
            continue;
        }
        known.emplace(key, ink);
        std::copy_n(ink.begin(), ch, out + i * ch);
    }
    if (where.empty()) return;
    std::vector<float> made(where.size() * ch);
    cmsDoTransform(from_srgb_transform(color, TYPE_RGB_8, float_format(color.space))->handle, pending.data(), made.data(),
                   static_cast<cmsUInt32Number>(where.size()));
    from_lcms(color.space, made.data(), where.size());
    for (std::size_t j = 0; j < where.size(); ++j) {
        for (unsigned k = 0; k < ch; ++k) out[where[j] * ch + k] = made[j * ch + k];
    }
}

void to_srgb(const PageColor& color, const double* inks, float* srgb, std::size_t count) {
    const unsigned ch = core::colour_channels(color.space);
    std::vector<float> scaled(inks, inks + count * ch);
    if (color.space == InkSpace::Cmyk) {
        for (float& v : scaled) v = std::clamp(v, 0.0f, 1.0f) * 100.0f;
        cmsDoTransform(to_srgb_transform(color, TYPE_CMYK_FLT, TYPE_RGB_FLT)->handle, scaled.data(), srgb, static_cast<cmsUInt32Number>(count));
    } else {
        cmsDoTransform(to_srgb_transform(color, TYPE_Lab_FLT, TYPE_RGB_FLT)->handle, scaled.data(), srgb, static_cast<cmsUInt32Number>(count));
    }
    for (std::size_t i = 0; i < count * 3; ++i) srgb[i] = std::clamp(srgb[i], 0.0f, 1.0f);
}

std::array<double, 3> srgb_of(const PageColor& color, const std::vector<double>& ink) {
    double in[4] = {};
    for (std::size_t c = 0; c < ink.size() && c < 4; ++c) in[c] = ink[c];
    float out[3];
    to_srgb(color, in, out, 1);
    return {out[0], out[1], out[2]};
}

InkPixels from_rgb(const Image& image, const PageColor& color, bool gray_on_k) {
    const Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    InkPixels out;
    out.space = color.space;
    out.depth = color.depth;
    out.width = static_cast<std::uint32_t>(rgba.width());
    out.height = static_cast<std::uint32_t>(rgba.height());
    const unsigned ch = core::colour_channels(color.space), all = ch + 1;
    out.samples.assign(out.pixel_count() * all, 0);
    const Transform transform = from_srgb_transform(color, TYPE_RGBA_8, int_format(color.space, color.depth));
    std::vector<std::uint16_t> row16(static_cast<std::size_t>(out.width) * ch);
    std::vector<unsigned char> row8(static_cast<std::size_t>(out.width) * ch);
    const double most = out.max();
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const auto* src = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        void* dst = color.depth == 16 ? static_cast<void*>(row16.data()) : static_cast<void*>(row8.data());
        cmsDoTransform(transform->handle, src, dst, out.width);
        std::uint16_t* samples = out.samples.data() + std::size_t(y) * out.width * all;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            for (unsigned c = 0; c < ch; ++c) samples[x * all + c] = color.depth == 16 ? row16[x * ch + c] : row8[x * ch + c];
            const unsigned char* p = src + x * 4;
            if (gray_on_k && color.space == InkSpace::Cmyk && p[0] == p[1] && p[1] == p[2]) {
                samples[x * all] = samples[x * all + 1] = samples[x * all + 2] = 0;
                samples[x * all + 3] = clamp_sample((1.0 - p[0] / 255.0) * most, most);
            }
            samples[x * all + ch] = color.depth == 16 ? static_cast<std::uint16_t>(p[3] * 257) : p[3];
        }
    }
    return out;
}

InkPixels from_srgb(const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height, const PageColor& color, bool gray_on_k) {
    InkPixels out;
    out.space = color.space;
    out.depth = color.depth;
    out.width = width;
    out.height = height;
    const unsigned ch = core::colour_channels(color.space), all = ch + 1;
    out.samples.assign(out.pixel_count() * all, 0);
    std::vector<float> clamped(rgba.size());
    for (std::size_t i = 0; i < rgba.size(); ++i) clamped[i] = std::clamp(rgba[i], 0.0f, 1.0f);
    const Transform transform = from_srgb_transform(color, TYPE_RGBA_FLT, int_format(color.space, 16));
    std::vector<std::uint16_t> row(static_cast<std::size_t>(width) * ch);
    const double most = out.max();
    for (std::uint32_t y = 0; y < height; ++y) {
        const float* src = clamped.data() + std::size_t(y) * width * 4;
        cmsDoTransform(transform->handle, src, row.data(), width);
        std::uint16_t* samples = out.samples.data() + std::size_t(y) * width * all;
        for (std::uint32_t x = 0; x < width; ++x) {
            for (unsigned c = 0; c < ch; ++c) samples[x * all + c] = narrowed(row[x * ch + c], color.depth);
            const float* p = src + x * 4;
            if (gray_on_k && color.space == InkSpace::Cmyk && p[0] == p[1] && p[1] == p[2]) {
                samples[x * all] = samples[x * all + 1] = samples[x * all + 2] = 0;
                samples[x * all + 3] = clamp_sample((1.0 - p[0]) * most, most);
            }
            samples[x * all + ch] = clamp_sample(p[3] * most, most);
        }
    }
    return out;
}

Image to_rgba8(const InkPixels& pixels, const PageColor& color) {
    Image out = Image::create("RGBA", Size{static_cast<int>(pixels.width), static_cast<int>(pixels.height)}, Ink{0, 0, 0, 0});
    const unsigned ch = core::colour_channels(pixels.space), all = pixels.channels();
    const Transform transform = to_srgb_transform(color, int_format(pixels.space, 16), TYPE_RGB_8);
    const std::vector<std::uint16_t> wide = widened(pixels);
    std::vector<unsigned char> row(static_cast<std::size_t>(pixels.width) * 3);
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        cmsDoTransform(transform->handle, wide.data() + std::size_t(y) * pixels.width * ch, row.data(), pixels.width);
        auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            dst[x * 4] = row[x * 3];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            const std::uint16_t a = pixels.samples[(std::size_t(y) * pixels.width + x) * all + ch];
            dst[x * 4 + 3] = static_cast<unsigned char>(pixels.depth == 8 ? a : narrowed(a, 8));
        }
    }
    return out;
}

std::vector<float> to_srgb_floats(const InkPixels& pixels, const PageColor& color) {
    const unsigned ch = core::colour_channels(pixels.space), all = pixels.channels();
    const Transform transform = to_srgb_transform(color, int_format(pixels.space, 16), TYPE_RGB_FLT);
    const std::vector<std::uint16_t> wide = widened(pixels);
    std::vector<float> out(pixels.pixel_count() * 4);
    std::vector<float> row(static_cast<std::size_t>(pixels.width) * 3);
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        cmsDoTransform(transform->handle, wide.data() + std::size_t(y) * pixels.width * ch, row.data(), pixels.width);
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            const std::size_t i = std::size_t(y) * pixels.width + x;
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = std::clamp(row[x * 3 + c], 0.0f, 1.0f);
            out[i * 4 + 3] = static_cast<float>(pixels.samples[i * all + ch]) / static_cast<float>(pixels.max());
        }
    }
    return out;
}

InkPixels convert(const InkPixels& pixels, const PageColor& from, const PageColor& to) {
    InkPixels out;
    out.space = to.space;
    out.depth = to.depth;
    out.width = pixels.width;
    out.height = pixels.height;
    const unsigned in_ch = core::colour_channels(pixels.space), in_all = pixels.channels();
    const unsigned out_ch = core::colour_channels(to.space), out_all = out_ch + 1;
    out.samples.assign(out.pixel_count() * out_all, 0);
    const bool same = from.space == to.space && (from.space == InkSpace::Lab || from.profile == to.profile);
    std::vector<std::uint16_t> wide = widened(pixels);
    std::vector<std::uint16_t> made;
    if (same) {
        made = std::move(wide);
    } else {
        made.resize(out.pixel_count() * out_ch);
        const Transform transform = between_transform(from, to, int_format(from.space, 16), int_format(to.space, 16));
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            cmsDoTransform(transform->handle, wide.data() + std::size_t(y) * pixels.width * in_ch,
                           made.data() + std::size_t(y) * pixels.width * out_ch, pixels.width);
        }
    }
    for (std::size_t i = 0; i < out.pixel_count(); ++i) {
        for (unsigned c = 0; c < out_ch; ++c) out.samples[i * out_all + c] = narrowed(made[i * out_ch + c], to.depth);
        out.samples[i * out_all + out_ch] = alpha_to(pixels.samples[i * in_all + in_ch], pixels.depth, to.depth);
    }
    return out;
}

InkPixels from_cmyk_image(const Image& cmyk, const Image* alpha, std::string_view picture_icc, const PageColor& color) {
    const Image in = cmyk.mode() == "CMYK" ? cmyk : cmyk.convert("CMYK");
    InkPixels source;
    source.space = InkSpace::Cmyk;
    source.depth = 8;
    source.width = static_cast<std::uint32_t>(in.width());
    source.height = static_cast<std::uint32_t>(in.height());
    source.samples.assign(source.pixel_count() * 5, 0);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(in.raw()->image[y]);
        const auto* a = alpha ? reinterpret_cast<const unsigned char*>(alpha->raw()->image[y]) : nullptr;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            std::uint16_t* s = source.samples.data() + (std::size_t(y) * source.width + x) * 5;
            for (unsigned c = 0; c < 4; ++c) s[c] = row[x * 4 + c];
            s[4] = a ? a[x] : 255;
        }
    }
    PageColor picture;
    picture.space = InkSpace::Cmyk;
    picture.depth = 8;
    picture.intent = color.intent;
    if (!picture_icc.empty() && !profile_problem(picture_icc)) {
        picture.icc = std::make_shared<const std::string>(picture_icc);
        picture.profile = core::bytes_ref(picture_icc);
    } else if (color.space == InkSpace::Cmyk) {
        picture.icc = color.icc;  // (no profile of its own: the page's)
        picture.profile = color.profile;
    } else {
        // (a Lab page and a CMYK picture without a profile: Pillow's plain CMYK → RGB, then into Lab)
        Image rgba = in.convert("RGB").convert("RGBA");
        if (alpha) rgba.putalpha(*alpha);
        return from_rgb(rgba, color);
    }
    return convert(source, picture, color);
}

InkPixels from_lab_image(const Image& lab, const Image* alpha, const PageColor& color) {
    InkPixels source;
    source.space = InkSpace::Lab;
    source.depth = 8;
    source.width = static_cast<std::uint32_t>(lab.width());
    source.height = static_cast<std::uint32_t>(lab.height());
    source.samples.assign(source.pixel_count() * 4, 0);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(lab.raw()->image[y]);
        const auto* a = alpha ? reinterpret_cast<const unsigned char*>(alpha->raw()->image[y]) : nullptr;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            std::uint16_t* s = source.samples.data() + (std::size_t(y) * source.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) s[c] = row[x * 4 + c];
            s[3] = a ? a[x] : 255;
        }
    }
    PageColor lab_mode;
    lab_mode.space = InkSpace::Lab;
    lab_mode.depth = 8;
    lab_mode.intent = color.intent;
    return convert(source, lab_mode, color);
}

Image lab_to_rgb_as_pillow(const Image& lab) {
    static const Transform transform = [] {
        const ProfileHandle srgb(cmsCreate_sRGBProfile());
        const ProfileHandle lab_profile(cmsCreateLab2Profile(nullptr));
        cmsHTRANSFORM handle = cmsCreateTransform(lab_profile.handle, TYPE_Lab_8, srgb.handle, TYPE_RGB_8, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
        if (handle == nullptr) throw core::Error("value", "the Lab to sRGB transform cannot be made");
        return std::make_shared<const TransformHandle>(handle);
    }();
    Image out = Image::create("RGB", lab.size(), Ink{0, 0, 0});
    std::vector<unsigned char> in(static_cast<std::size_t>(lab.width()) * 3), rgb(static_cast<std::size_t>(lab.width()) * 3);
    for (int y = 0; y < lab.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(lab.raw()->image[y]);
        for (int x = 0; x < lab.width(); ++x) {
            for (unsigned c = 0; c < 3; ++c) in[x * 3 + c] = row[x * 4 + c];
        }
        cmsDoTransform(transform->handle, in.data(), rgb.data(), static_cast<cmsUInt32Number>(lab.width()));
        auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < lab.width(); ++x) {
            for (unsigned c = 0; c < 3; ++c) dst[x * 4 + c] = rgb[x * 3 + c];
        }
    }
    return out;
}

std::uint16_t encode_sample(InkSpace space, unsigned channel, double value, int depth) {
    const double most = depth == 16 ? 65535.0 : 255.0;
    const unsigned ch = core::colour_channels(space);
    if (channel >= ch || space == InkSpace::Cmyk) return clamp_sample(value * most, most);
    if (channel == 0) return clamp_sample(value / 100.0 * most, most);
    return clamp_sample(depth == 16 ? (value + 128.0) * 257.0 : value + 128.0, most);
}

double decode_sample(InkSpace space, unsigned channel, std::uint16_t sample, int depth) {
    const double most = depth == 16 ? 65535.0 : 255.0;
    const unsigned ch = core::colour_channels(space);
    if (channel >= ch || space == InkSpace::Cmyk) return sample / most;
    if (channel == 0) return sample / most * 100.0;
    return depth == 16 ? sample / 257.0 - 128.0 : sample - 128.0;
}

}  // namespace genko::render::ink
