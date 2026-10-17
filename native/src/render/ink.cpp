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
#include "render/png.hpp"
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

// The page's profile (CMYK, Gray: its ICC bytes; Lab: Little CMS's D50 Lab v4).
std::unique_ptr<ProfileHandle> page_profile(const PageColor& color) {
    if (color.space == InkSpace::Lab) return std::make_unique<ProfileHandle>(cmsCreateLab4Profile(nullptr));
    if (!color.icc) throw core::Error("value", "the page's ICC profile " + color.profile + " is not held");
    cmsHPROFILE handle = cmsOpenProfileFromMem(color.icc->data(), static_cast<cmsUInt32Number>(color.icc->size()));
    if (handle == nullptr) throw core::Error("value", "the page's ICC profile cannot be read");
    return std::make_unique<ProfileHandle>(handle);
}

std::string page_key(const PageColor& color) { return color.space == InkSpace::Lab ? std::string("lab") : color.profile; }

cmsUInt32Number float_format(InkSpace space) {
    switch (space) {
        case InkSpace::Cmyk: return TYPE_CMYK_FLT;
        case InkSpace::Lab: return TYPE_Lab_FLT;
        case InkSpace::Gray: return TYPE_GRAY_FLT;
        default: break;
    }
    throw core::Error("not_yet_ported", "the colours of a " + std::string(core::to_string(space)) + " raster are not in this build yet");
}
cmsUInt32Number int_format(InkSpace space, int depth) {
    if (depth == 32) return float_format(space);
    if (space == InkSpace::Cmyk) return depth == 16 ? TYPE_CMYK_16 : TYPE_CMYK_8;
    if (space == InkSpace::Gray) return depth == 16 ? TYPE_GRAY_16 : TYPE_GRAY_8;
    if (space == InkSpace::Lab) return depth == 16 ? TYPE_Lab_16 : TYPE_Lab_8;
    return float_format(space);
}

// A normalized value (a GKIR sample's meaning, 0..1: CMYK's ink, Lab's encoding, Gray's lightness) as Little CMS's
// float encoding of the space (CMYK 0..100, Lab L 0..100 and a, b -128..127, Gray 0..1), and back.
float to_lcms(InkSpace space, unsigned c, double v) {
    if (space == InkSpace::Cmyk) return static_cast<float>(v * 100.0);
    if (space == InkSpace::Lab) return static_cast<float>(c == 0 ? v * 100.0 : v * 255.0 - 128.0);
    return static_cast<float>(v);
}
double from_lcms_value(InkSpace space, unsigned c, float v) {
    if (space == InkSpace::Cmyk) return std::clamp(v / 100.0, 0.0, 1.0);
    if (space == InkSpace::Lab) return std::clamp(c == 0 ? v / 100.0 : (v + 128.0) / 255.0, 0.0, 1.0);
    return std::clamp(static_cast<double>(v), 0.0, 1.0);
}

// Is the page's profile sGray (a grey v shown as sRGB's (v, v, v), numerically)?
bool sgray(const PageColor& color) {
    static const std::string ref = core::builtin_profile("sgray")->ref();
    return color.space == InkSpace::Gray && color.profile == ref;
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

// The flags colours come into the page with (its black point compensation).
cmsUInt32Number flags_in(const PageColor& color) { return color.bpc ? kFlags : cmsFLAGS_NOCACHE; }

// An RGB profile (its bytes; null: Little CMS's sRGB) opened, and its key in the transforms' cache.
std::unique_ptr<ProfileHandle> rgb_profile(const core::Bytes& icc) {
    if (!icc) return std::make_unique<ProfileHandle>(cmsCreate_sRGBProfile());
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc->data(), static_cast<cmsUInt32Number>(icc->size()));
    if (handle == nullptr) throw core::Error("value", "the RGB ICC profile cannot be read");
    return std::make_unique<ProfileHandle>(handle);
}
std::string rgb_key(const core::Bytes& icc) { return icc ? core::bytes_ref(*icc) : std::string("srgb"); }

// sRGB, or the RGB profile `source` (`in`) → the page (`out`), the page's intent and black point compensation.
Transform from_srgb_transform(const PageColor& color, cmsUInt32Number in, cmsUInt32Number out, const core::Bytes& source = nullptr) {
    const std::string key = rgb_key(source) + ">" + page_key(color) + ":" + color.intent + (color.bpc ? "" : ":nobpc") + ":" + std::to_string(in) +
                            ":" + std::to_string(out);
    return cached(key, [&] {
        const auto rgb = rgb_profile(source);
        const auto page = page_profile(color);
        return cmsCreateTransform(rgb->handle, in, page->handle, out, intent_number(color.intent), flags_in(color));
    });
}

// The page (`in`) → an RGB page's profile (`out`; null: sRGB), that page's intent and black point compensation.
Transform to_rgb_transform(const PageColor& color, const core::RgbMode& target, cmsUInt32Number in, cmsUInt32Number out) {
    const std::string key = page_key(color) + ">" + rgb_key(target.icc) + ":" + target.intent + (target.bpc ? "" : ":nobpc") + ":" +
                            std::to_string(in) + ":" + std::to_string(out);
    return cached(key, [&] {
        const auto page = page_profile(color);
        const auto rgb = rgb_profile(target.icc);
        return cmsCreateTransform(page->handle, in, rgb->handle, out, intent_number(target.intent),
                                  target.bpc ? kFlags : cmsFLAGS_NOCACHE);
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

// One page mode → another (CMYK → CMYK keeps the black plate), the target's intent and black point compensation.
Transform between_transform(const PageColor& from, const PageColor& to, cmsUInt32Number in, cmsUInt32Number out) {
    const std::string key = page_key(from) + ">" + page_key(to) + ":" + to.intent + (to.bpc ? "" : ":nobpc") + ":" + std::to_string(in) + ":" +
                            std::to_string(out);
    return cached(key, [&] {
        const auto a = page_profile(from);
        const auto b = page_profile(to);
        std::uint32_t intent = intent_number(to.intent);
        if (from.space == InkSpace::Cmyk && to.space == InkSpace::Cmyk && intent != INTENT_ABSOLUTE_COLORIMETRIC) {
            intent = intent == INTENT_PERCEPTUAL ? INTENT_PRESERVE_K_ONLY_PERCEPTUAL
                     : intent == INTENT_SATURATION ? INTENT_PRESERVE_K_ONLY_SATURATION
                                                   : INTENT_PRESERVE_K_ONLY_RELATIVE_COLORIMETRIC;
        }
        return cmsCreateTransform(a->handle, in, b->handle, out, intent, flags_in(to));
    });
}

// The ink of a grey v (0..255) by the rules (CMYK: K alone; Gray: k = 1 − v/255, as K; Lab: its lightness, no colour).
void grey_ink(const PageColor& color, int v, double* out) {
    if (color.space == InkSpace::Cmyk) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = v == 0 || v == 10 || v == 15 || v == 20 ? 1.0 : 1.0 - v / 255.0;
        return;
    }
    if (color.space == InkSpace::Gray) {
        out[0] = v == 0 || v == 10 || v == 15 || v == 20 ? 1.0 : 1.0 - v / 255.0;
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

// Little CMS's float CMYK is 0..100: ours 0..1. Its float grey is lightness: ours the black ink, k = 1 − it.
void from_lcms(InkSpace space, float* values, std::size_t count) {
    if (space == InkSpace::Gray) {
        for (std::size_t i = 0; i < count; ++i) values[i] = std::clamp(1.0f - values[i], 0.0f, 1.0f);
        return;
    }
    if (space != InkSpace::Cmyk) return;
    for (std::size_t i = 0; i < count * 4; ++i) values[i] = std::clamp(values[i] / 100.0f, 0.0f, 1.0f);
}

std::uint16_t clamp_sample(double v, double most) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, most))); }

// The colours of a 32-bit raster's row as Little CMS's float encoding of its space.
void lcms_row(const InkPixels& pixels, std::uint32_t y, float* out) {
    const unsigned ch = pixels.colours();
    for (std::uint32_t x = 0; x < pixels.width; ++x) {
        const std::size_t i = std::size_t(y) * pixels.width + x;
        for (unsigned c = 0; c < ch; ++c) out[x * ch + c] = to_lcms(pixels.space, c, pixels.value(i, c));
    }
}

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

// --- multichannel pages (color-model@1, PC1d; PhotoCraft's multichannel_cmds.rs) -----------------------------------
// A multichannel page's planes are ink densities (0 no ink, 1 solid), printed over white paper in their colours. Its
// elements' colours go onto them by its rules: its table, then a grey on its key plates (Genko's blacks solid, through
// their curves), then the colour through the mode the page was made from (RGB: 1 − its values; CMYK: through its
// profile; Lab: D50, 1 − the encoding; grey and duotone: the grey the profile gives, on the key plates).

bool genko_black(int v) { return v == 0 || v == 10 || v == 15 || v == 20; }

// The mode a multichannel page was made from as a page of its own (its profile, intent and black point compensation),
// for the transforms its elements' colours go through. RGB has none.
PageColor source_mode(const PageColor& color) {
    PageColor out;
    out.space = color.from == "cmyk" ? InkSpace::Cmyk : color.from == "lab" ? InkSpace::Lab : InkSpace::Gray;
    out.depth = 16;
    out.profile = color.profile;
    out.icc = color.icc;
    out.intent = color.intent;
    out.bpc = color.bpc;
    return out;
}

// Which plane of the mode it came from a plate is, by the name Multichannel gave it (Cyan, Magenta, Yellow, Black;
// Alpha 1..3), or -1 (a grey's and a duotone's plates take the grey through their curves instead).
int source_plane(const std::string& from, const std::string& name) {
    if (from == "rgb" || from == "cmyk") {
        if (name == "Cyan") return 0;
        if (name == "Magenta") return 1;
        if (name == "Yellow") return 2;
        if (from == "cmyk" && name == "Black") return 3;
        return -1;
    }
    if (from == "lab") {
        if (name == "Alpha 1") return 0;
        if (name == "Alpha 2") return 1;
        if (name == "Alpha 3") return 2;
    }
    return -1;
}

// A pixel of the mode the page came from (its normalized values: RGB 0..1, CMYK's inks, Lab's encoding, a grey's
// lightness) as the plates' densities: CMYK's plates as they are, RGB's and Lab's 1 − the value, a grey's density
// (1 − its lightness) through each plate's curve (PhotoCraft's inks_of).
void plates_from(const PageColor& color, const double* source, double* out) {
    const bool grey = color.from == "gray" || color.from == "duotone";
    for (std::size_t k = 0; k < color.plates.size(); ++k) {
        const core::Plate& plate = color.plates[k];
        if (grey) {
            const double d = 1.0 - std::clamp(source[0], 0.0, 1.0);
            out[k] = plate.curve.empty() ? d : std::clamp(core::curve_at(plate.curve, d), 0.0, 1.0);
            continue;
        }
        const int at = source_plane(color.from, plate.name);
        if (at < 0) {
            out[k] = 0.0;
            continue;
        }
        const double v = std::clamp(source[at], 0.0, 1.0);
        out[k] = color.from == "cmyk" ? v : 1.0 - v;
    }
}

bool has_key(const PageColor& color) {
    return std::any_of(color.plates.begin(), color.plates.end(), [](const core::Plate& p) { return p.key; });
}

// A grey's density (0 white .. 1 solid) on the key plates, through their curves; nothing on the others.
void multi_grey(const PageColor& color, double d, double* out) {
    for (std::size_t k = 0; k < color.plates.size(); ++k) {
        const core::Plate& plate = color.plates[k];
        out[k] = !plate.key ? 0.0 : plate.curve.empty() ? d : std::clamp(core::curve_at(plate.curve, d), 0.0, 1.0);
    }
}

// sRGB colours (3 floats 0..1 each) through the mode the page came from, as the plates' densities (planes each); a Lab
// page's greys (`flat`, or null) with a and b 0.
void multi_through(const PageColor& color, const float* srgb, double* out, std::size_t count, const std::vector<bool>* flat = nullptr) {
    const unsigned n = color.planes();
    double source[4] = {};
    if (color.from == "rgb") {
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < 3; ++c) source[c] = std::clamp(static_cast<double>(srgb[i * 3 + c]), 0.0, 1.0);
            plates_from(color, source, out + i * n);
        }
        return;
    }
    const PageColor mode = source_mode(color);
    const unsigned ch = core::colour_channels(mode.space);
    std::vector<float> clamped(srgb, srgb + count * 3), made(count * ch);
    for (float& v : clamped) v = std::clamp(v, 0.0f, 1.0f);
    cmsDoTransform(from_srgb_transform(mode, TYPE_RGB_FLT, float_format(mode.space))->handle, clamped.data(), made.data(),
                   static_cast<cmsUInt32Number>(count));
    for (std::size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < ch; ++c) source[c] = from_lcms_value(mode.space, c, made[i * ch + c]);
        if (flat != nullptr && (*flat)[i] && mode.space == InkSpace::Lab) source[1] = source[2] = 128.0 / 255.0;  // (a Lab grey)
        plates_from(color, source, out + i * n);
    }
}

// The display of plate densities (planes each): printed over white paper, sRGB 0..1.
void multi_shown(const PageColor& color, const double* density, float* srgb, std::size_t count) {
    const unsigned n = color.planes();
    for (std::size_t i = 0; i < count; ++i) {
        double p[3] = {1.0, 1.0, 1.0};
        core::print_plates(color.plates, density + i * n, p);
        for (unsigned c = 0; c < 3; ++c) srgb[i * 3 + c] = static_cast<float>(std::clamp(p[c], 0.0, 1.0));
    }
}

// The pixels of a multichannel page from straight RGBA-like pixels of the mode it came from: `source(i, values,
// alpha)` gives pixel i's normalized values and its alpha.
template <class Source>
InkPixels multi_pixels(const PageColor& color, std::uint32_t width, std::uint32_t height, Source source) {
    const unsigned n = color.planes();
    InkPixels out = core::blank_ink_pixels(InkSpace::Multi, color.depth, width, height, n);
    std::vector<double> values(4), plates(n);
    for (std::size_t i = 0; i < out.pixel_count(); ++i) {
        double alpha = 1.0;
        source(i, values.data(), alpha);
        plates_from(color, values.data(), plates.data());
        for (unsigned k = 0; k < n; ++k) out.set(i, k, plates[k]);
        out.set(i, n, alpha);
    }
    return out;
}

std::uint16_t alpha_to(std::uint16_t a, int from, int to) {
    if (from == to) return a;
    return from == 8 ? static_cast<std::uint16_t>(a * 257) : narrowed(a, 8);
}

// The profile check of the book reader (storage/reader.hpp set_profile_check). (Set when this file is linked: every
// program that draws a CMYK page draws it through it.)
const bool kReaderCheck = [] {
    storage::set_profile_check([](std::string_view icc, core::ProfileSpace space) { return profile_problem(icc, space); });
    return true;
}();

}  // namespace

std::optional<std::string> profile_problem(std::string_view icc, core::ProfileSpace space) {
    if (icc.empty()) return std::string("the profile is empty");
    if (icc.size() > core::kProfileMaxBytes) return std::string("the profile is larger than 16 MiB");
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) return std::string("Little CMS cannot read it as an ICC profile");
    const ProfileHandle held(handle);
    cmsUInt32Number format = TYPE_CMYK_8;
    switch (space) {
        case core::ProfileSpace::Cmyk:
            if (cmsGetColorSpace(handle) != cmsSigCmykData) return std::string("it is not a CMYK profile");
            break;
        case core::ProfileSpace::Gray:
            if (cmsGetColorSpace(handle) != cmsSigGrayData) return std::string("it is not a grey profile");
            format = TYPE_GRAY_8;
            break;
        case core::ProfileSpace::Rgb:
            if (cmsGetColorSpace(handle) != cmsSigRgbData) return std::string("it is not an RGB profile");
            // (an RGB page's values go to and from linear light by the profile's curves: a matrix and its tone curves)
            if (!cmsIsMatrixShaper(handle)) return std::string("it is not a matrix-shaper RGB profile");
            format = TYPE_RGB_8;
            break;
        case core::ProfileSpace::Lab: return std::string("a Lab page has no profile");
    }
    const ProfileHandle srgb(cmsCreate_sRGBProfile());
    cmsHTRANSFORM probe = cmsCreateTransform(srgb.handle, TYPE_RGB_8, handle, format, INTENT_RELATIVE_COLORIMETRIC, kFlags);
    if (probe == nullptr) return std::string("it cannot take colours in (no transform from sRGB)");
    cmsDeleteTransform(probe);
    probe = cmsCreateTransform(handle, format, srgb.handle, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, kFlags);
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
    std::vector<double> out(std::max(4u, color.planes()), 0.0);
    inks_of(color, bytes, nullptr, out.data(), 1);
    out.resize(color.planes());
    return out;
}

std::vector<double> ink_of_srgb(const PageColor& color, const std::array<double, 3>& srgb) {
    float in[3];
    for (unsigned c = 0; c < 3; ++c) in[c] = static_cast<float>(std::clamp(srgb[c], 0.0, 1.0));
    if (color.space == InkSpace::Multi) {
        std::vector<double> plates(color.planes());
        multi_through(color, in, plates.data(), 1);
        return plates;
    }
    float out[4] = {};
    cmsDoTransform(from_srgb_transform(color, TYPE_RGB_FLT, float_format(color.space))->handle, in, out, 1);
    from_lcms(color.space, out, 1);
    return std::vector<double>(out, out + core::colour_channels(color.space));
}

void inks_of(const PageColor& color, const unsigned char* rgb, const unsigned char* alpha, double* out, std::size_t count) {
    if (color.space == InkSpace::Multi) {  // (the table, a grey on the key plates, the rest through the mode it came from)
        const unsigned n = color.planes();
        const bool key = has_key(color) && color.from != "lab";
        std::unordered_map<std::uint32_t, std::vector<double>> known;
        std::vector<float> pending;
        std::vector<std::size_t> where;
        std::vector<bool> flat;
        for (std::size_t i = 0; i < count; ++i) {
            if (alpha != nullptr && alpha[i] == 0) continue;
            const std::array<int, 3> c{rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
            const std::uint32_t at = (std::uint32_t(c[0]) << 16) | (std::uint32_t(c[1]) << 8) | std::uint32_t(c[2]);
            if (const auto it = known.find(at); it != known.end()) {
                std::copy_n(it->second.begin(), n, out + i * n);
                continue;
            }
            std::vector<double> ink(n, 0.0);
            const bool grey = c[0] == c[1] && c[1] == c[2];
            if (const core::InkEntry* entry = core::ink_for(color, c); entry != nullptr && entry->ink.size() == n) {
                ink = entry->ink;
            } else if (grey && key) {
                multi_grey(color, genko_black(c[0]) ? 1.0 : 1.0 - c[0] / 255.0, ink.data());
            } else if (grey && (genko_black(c[0]) || c[0] == 255) && color.from == "lab") {  // (Genko's blacks L 0, white L 100)
                const double l = c[0] == 255 ? 1.0 : 0.0, neutral = 128.0 / 255.0;
                const double source[3] = {l, neutral, neutral};
                plates_from(color, source, ink.data());
            } else {
                for (unsigned k = 0; k < 3; ++k) pending.push_back(c[k] / 255.0f);
                where.push_back(i);
                flat.push_back(grey);
                continue;
            }
            std::copy_n(ink.begin(), n, out + i * n);
            known.emplace(at, std::move(ink));
        }
        if (where.empty()) return;
        std::vector<double> made(where.size() * n);
        multi_through(color, pending.data(), made.data(), where.size(), &flat);
        for (std::size_t j = 0; j < where.size(); ++j) std::copy_n(made.data() + j * n, n, out + where[j] * n);
        return;
    }
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
    if (color.space == InkSpace::Multi) {  // (its plates printed over white paper)
        multi_shown(color, inks, srgb, count);
        return;
    }
    const unsigned ch = core::colour_channels(color.space);
    std::vector<float> scaled(inks, inks + count * ch);
    if (color.space == InkSpace::Gray) {  // (its lightness, 1 − k; sGray's numerically: (v, v, v))
        for (float& v : scaled) v = 1.0f - std::clamp(v, 0.0f, 1.0f);
        if (sgray(color)) {
            for (std::size_t i = 0; i < count; ++i) srgb[i * 3] = srgb[i * 3 + 1] = srgb[i * 3 + 2] = scaled[i];
        } else {
            cmsDoTransform(to_srgb_transform(color, TYPE_GRAY_FLT, TYPE_RGB_FLT)->handle, scaled.data(), srgb, static_cast<cmsUInt32Number>(count));
        }
    } else if (color.space == InkSpace::Cmyk) {
        for (float& v : scaled) v = std::clamp(v, 0.0f, 1.0f) * 100.0f;
        cmsDoTransform(to_srgb_transform(color, TYPE_CMYK_FLT, TYPE_RGB_FLT)->handle, scaled.data(), srgb, static_cast<cmsUInt32Number>(count));
    } else {
        cmsDoTransform(to_srgb_transform(color, TYPE_Lab_FLT, TYPE_RGB_FLT)->handle, scaled.data(), srgb, static_cast<cmsUInt32Number>(count));
    }
    for (std::size_t i = 0; i < count * 3; ++i) srgb[i] = std::clamp(srgb[i], 0.0f, 1.0f);
}

std::array<double, 3> srgb_of(const PageColor& color, const std::vector<double>& ink) {
    std::vector<double> in(std::max(4u, color.planes()), 0.0);
    for (std::size_t c = 0; c < ink.size() && c < in.size(); ++c) in[c] = ink[c];
    float out[3];
    to_srgb(color, in.data(), out, 1);
    return {out[0], out[1], out[2]};
}

namespace {

// Inks by their samples at the page's depth (a key of up to four 16-bit samples).
std::uint64_t ink_key(const PageColor& color, const double* ink) {
    std::uint64_t key = 0;
    for (unsigned c = 0; c < core::colour_channels(color.space); ++c) key = (key << 16) | encode_sample(color.space, c, ink[c], color.depth);
    return key;
}

// A multichannel page's plates by their samples at the page's depth.
std::string plate_key(const PageColor& color, const double* ink) {
    std::string key;
    for (unsigned c = 0; c < color.planes(); ++c) {
        const std::uint16_t v = encode_sample(InkSpace::Multi, c, ink[c], color.depth);
        key.push_back(static_cast<char>(v >> 8));
        key.push_back(static_cast<char>(v & 0xff));
    }
    return key;
}

// An sRGB value as an 8-bit level when it is one exactly (within a float's rounding), else -1.
int level_of(double v) {
    const double k = std::round(v * 255.0);
    return k >= 0 && k <= 255 && std::abs(v * 255.0 - k) < 1e-6 ? static_cast<int>(k) : -1;
}

}  // namespace

Rules::Rules(const PageColor& color) : color_(color) {
    const unsigned ch = color.planes();
    for (const core::InkEntry& entry : color.inks) {  // (the first entry of a colour, as core::ink_for finds it)
        if (entry.ink.size() < ch) continue;
        if (color.space == InkSpace::Multi) by_plates_.try_emplace(plate_key(color, entry.ink.data()), entry.rgb);
        else by_inks_.try_emplace(ink_key(color, entry.ink.data()), entry.rgb);
        const std::uint32_t key = (std::uint32_t(entry.rgb[0] & 0xff) << 16) | (std::uint32_t(entry.rgb[1] & 0xff) << 8) | std::uint32_t(entry.rgb[2] & 0xff);
        by_rgb_.try_emplace(key, std::vector<double>(entry.ink.begin(), entry.ink.begin() + ch));
    }
}

void Rules::meaning(const double* inks, double* rgb, std::size_t count) const {
    const PageColor& color = color_;
    if (color.space == InkSpace::Multi) {  // (the table's colour, else the plates as they print)
        const unsigned n = color.planes();
        std::vector<float> shown(count * 3);
        multi_shown(color, inks, shown.data(), count);
        for (std::size_t i = 0; i < count; ++i) {
            if (const auto it = by_plates_.find(plate_key(color, inks + i * n)); it != by_plates_.end()) {
                for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = it->second[c] / 255.0;
                continue;
            }
            for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = shown[i * 3 + c];
        }
        return;
    }
    const unsigned ch = core::colour_channels(color.space);
    const bool cmyk = color.space == InkSpace::Cmyk;
    const auto& table = by_inks_;
    const std::uint16_t neutral = encode_sample(InkSpace::Lab, 1, 0.0, color.depth);
    std::vector<double> shown;  // (the inks the screen colour stands for; a Lab grey with a = b = 0)
    std::vector<std::size_t> where;
    std::vector<bool> grey;
    for (std::size_t i = 0; i < count; ++i) {
        const double* p = inks + i * ch;
        double* out = rgb + i * 3;
        if (const auto it = table.find(ink_key(color, p)); it != table.end()) {
            for (unsigned c = 0; c < 3; ++c) out[c] = it->second[c] / 255.0;
            continue;
        }
        if (cmyk && encode_sample(color.space, 0, p[0], color.depth) == 0 && encode_sample(color.space, 1, p[1], color.depth) == 0 &&
            encode_sample(color.space, 2, p[2], color.depth) == 0) {
            const double v = 1.0 - std::clamp(p[3], 0.0, 1.0);
            out[0] = out[1] = out[2] = v;
            continue;
        }
        if (color.space == InkSpace::Gray) {  // (a grey page's value is its grey, whatever its profile: back by its value)
            const double v = 1.0 - std::clamp(p[0], 0.0, 1.0);
            out[0] = out[1] = out[2] = v;
            continue;
        }
        const bool flat = color.space == InkSpace::Lab && encode_sample(color.space, 1, p[1], color.depth) == neutral &&
                          encode_sample(color.space, 2, p[2], color.depth) == neutral;
        shown.insert(shown.end(), p, p + ch);
        if (flat) shown[shown.size() - 2] = shown[shown.size() - 1] = 0.0;
        where.push_back(i);
        grey.push_back(flat);
    }
    if (where.empty()) return;
    std::vector<float> srgb(where.size() * 3);
    to_srgb(color, shown.data(), srgb.data(), where.size());
    for (std::size_t j = 0; j < where.size(); ++j) {
        double* out = rgb + where[j] * 3;
        if (grey[j]) {  // (a Lab grey: the grey its lightness shows)
            const double v = (double(srgb[j * 3]) + srgb[j * 3 + 1] + srgb[j * 3 + 2]) / 3.0;
            out[0] = out[1] = out[2] = v;
        } else {
            for (unsigned c = 0; c < 3; ++c) out[c] = srgb[j * 3 + c];
        }
    }
}

void Rules::inks(const double* rgb, double* inks, std::size_t count) const {
    const PageColor& color = color_;
    if (color.space == InkSpace::Multi) {  // (the table, a grey on the key plates, the rest through the mode it came from)
        const unsigned n = color.planes();
        const bool key = has_key(color) && color.from != "lab";
        std::vector<float> pending;
        std::vector<std::size_t> where;
        std::vector<bool> flat;
        for (std::size_t i = 0; i < count; ++i) {
            double v[3];
            for (unsigned c = 0; c < 3; ++c) v[c] = std::isfinite(rgb[i * 3 + c]) ? std::clamp(rgb[i * 3 + c], 0.0, 1.0) : 0.0;
            double* out = inks + i * n;
            const int l0 = level_of(v[0]), l1 = level_of(v[1]), l2 = level_of(v[2]);
            if (l0 >= 0 && l1 >= 0 && l2 >= 0) {
                if (const auto it = by_rgb_.find((std::uint32_t(l0) << 16) | (std::uint32_t(l1) << 8) | std::uint32_t(l2)); it != by_rgb_.end()) {
                    std::copy_n(it->second.begin(), n, out);
                    continue;
                }
            }
            const bool grey = std::abs(v[0] - v[1]) < 1e-9 && std::abs(v[1] - v[2]) < 1e-9;
            if (grey && key) {
                multi_grey(color, l0 >= 0 && genko_black(l0) ? 1.0 : 1.0 - v[0], out);
                continue;
            }
            if (grey && l0 >= 0 && (genko_black(l0) || l0 == 255) && color.from == "lab") {  // (Genko's blacks L 0, white L 100)
                const double neutral = 128.0 / 255.0;
                const double source[3] = {l0 == 255 ? 1.0 : 0.0, neutral, neutral};
                plates_from(color, source, out);
                continue;
            }
            for (unsigned c = 0; c < 3; ++c) pending.push_back(static_cast<float>(v[c]));
            where.push_back(i);
            flat.push_back(grey);
        }
        if (where.empty()) return;
        std::vector<double> made(where.size() * n);
        multi_through(color, pending.data(), made.data(), where.size(), &flat);
        for (std::size_t j = 0; j < where.size(); ++j) std::copy_n(made.data() + j * n, n, inks + where[j] * n);
        return;
    }
    const unsigned ch = core::colour_channels(color.space);
    const bool cmyk = color.space == InkSpace::Cmyk;
    std::vector<float> profile, lab_greys;
    std::vector<std::size_t> where, grey_where;
    for (std::size_t i = 0; i < count; ++i) {
        double v[3];
        for (unsigned c = 0; c < 3; ++c) v[c] = std::isfinite(rgb[i * 3 + c]) ? std::clamp(rgb[i * 3 + c], 0.0, 1.0) : 0.0;
        double* out = inks + i * ch;
        const int l0 = level_of(v[0]), l1 = level_of(v[1]), l2 = level_of(v[2]);
        if (l0 >= 0 && l1 >= 0 && l2 >= 0) {
            if (const auto it = by_rgb_.find((std::uint32_t(l0) << 16) | (std::uint32_t(l1) << 8) | std::uint32_t(l2)); it != by_rgb_.end()) {
                std::copy_n(it->second.begin(), ch, out);
                continue;
            }
        }
        if (std::abs(v[0] - v[1]) < 1e-9 && std::abs(v[1] - v[2]) < 1e-9) {
            if (l0 == 0 || l0 == 10 || l0 == 15 || l0 == 20 || l0 == 255) {  // (Genko's blacks and white: the rules' own)
                grey_ink(color, l0, out);
                continue;
            }
            if (cmyk) {
                out[0] = out[1] = out[2] = 0.0;
                out[3] = 1.0 - v[0];
                continue;
            }
            if (color.space == InkSpace::Gray) {  // (as K: its value kept)
                out[0] = 1.0 - v[0];
                continue;
            }
            lab_greys.insert(lab_greys.end(), {static_cast<float>(v[0]), static_cast<float>(v[0]), static_cast<float>(v[0])});
            grey_where.push_back(i);
            continue;
        }
        profile.insert(profile.end(), {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
        where.push_back(i);
    }
    if (!grey_where.empty()) {  // (a Lab grey: its lightness, a = b = 0)
        std::vector<float> lab(grey_where.size() * 3);
        cmsDoTransform(from_srgb_transform(color, TYPE_RGB_FLT, TYPE_Lab_FLT)->handle, lab_greys.data(), lab.data(),
                       static_cast<cmsUInt32Number>(grey_where.size()));
        for (std::size_t j = 0; j < grey_where.size(); ++j) {
            double* out = inks + grey_where[j] * ch;
            out[0] = std::clamp<double>(lab[j * 3], 0.0, 100.0);
            out[1] = out[2] = 0.0;
        }
    }
    if (!where.empty()) {
        std::vector<float> made(where.size() * ch);
        cmsDoTransform(from_srgb_transform(color, TYPE_RGB_FLT, float_format(color.space))->handle, profile.data(), made.data(),
                       static_cast<cmsUInt32Number>(where.size()));
        from_lcms(color.space, made.data(), where.size());
        for (std::size_t j = 0; j < where.size(); ++j) {
            for (unsigned c = 0; c < ch; ++c) inks[where[j] * ch + c] = made[j * ch + c];
        }
    }
}

void Rules::correct(const double* inks, double* out, std::size_t count, const Correction& change) const {
    const unsigned ch = color_.planes();
    std::copy_n(inks, count * ch, out);
    std::vector<double> meant(count * 3);
    meaning(inks, meant.data(), count);
    std::vector<double> changed;
    std::vector<std::size_t> where;
    for (std::size_t i = 0; i < count; ++i) {
        const std::array<double, 3> was{meant[i * 3], meant[i * 3 + 1], meant[i * 3 + 2]};
        const std::array<double, 3> now = change(was);
        // (its colour as it was — within a float's rounding of the way there and back —: its inks)
        if (std::abs(now[0] - was[0]) < 1e-9 && std::abs(now[1] - was[1]) < 1e-9 && std::abs(now[2] - was[2]) < 1e-9) continue;
        changed.insert(changed.end(), now.begin(), now.end());
        where.push_back(i);
    }
    if (where.empty()) return;
    std::vector<double> made(where.size() * ch);
    this->inks(changed.data(), made.data(), where.size());
    for (std::size_t j = 0; j < where.size(); ++j) std::copy_n(made.data() + j * ch, ch, out + where[j] * ch);
}

void meaning_of(const PageColor& color, const double* inks, double* rgb, std::size_t count) { Rules(color).meaning(inks, rgb, count); }
void inks_by_rules(const PageColor& color, const double* rgb, double* inks, std::size_t count) { Rules(color).inks(rgb, inks, count); }
void correct(const PageColor& color, const double* inks, double* out, std::size_t count, const Correction& change) {
    Rules(color).correct(inks, out, count, change);
}

InkPixels from_rgb(const Image& image, const PageColor& color, bool gray_on_k, const core::Bytes& source) {
    const Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    const auto width = static_cast<std::uint32_t>(rgba.width()), height = static_cast<std::uint32_t>(rgba.height());
    if (color.space == InkSpace::Multi) {  // (RGB's values as inks, 1 − each: PhotoCraft's Multichannel of an RGB image)
        const std::string bytes = rgba.tobytes();
        return multi_pixels(color, width, height, [&](std::size_t i, double* values, double& alpha) {
            for (unsigned c = 0; c < 3; ++c) values[c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0;
            alpha = static_cast<unsigned char>(bytes[i * 4 + 3]) / 255.0;
        });
    }
    const unsigned ch = core::colour_channels(color.space), all = ch + 1;
    // (a grey page's greys by their value — sGray's, or asked for —, as a CMYK page's on K alone; from sRGB only)
    gray_on_k = gray_on_k && !source;
    const bool grey_by_value = !source && color.space == InkSpace::Gray && (gray_on_k || sgray(color));
    if (color.depth == 32) {
        InkPixels out = core::blank_ink_pixels(color.space, 32, width, height);
        const Transform transform = from_srgb_transform(color, TYPE_RGBA_8, float_format(color.space), source);
        std::vector<float> row(static_cast<std::size_t>(width) * ch);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* src = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
            cmsDoTransform(transform->handle, src, row.data(), width);
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                const unsigned char* p = src + x * 4;
                for (unsigned c = 0; c < ch; ++c) out.set(i, c, from_lcms_value(color.space, c, row[x * ch + c]));
                if (p[0] == p[1] && p[1] == p[2]) {
                    if (gray_on_k && color.space == InkSpace::Cmyk) {
                        for (unsigned c = 0; c < 3; ++c) out.set(i, c, 0.0);
                        out.set(i, 3, 1.0 - p[0] / 255.0);
                    } else if (grey_by_value) {
                        out.set(i, 0, p[0] / 255.0);
                    }
                }
                out.set(i, ch, p[3] / 255.0);
            }
        }
        return out;
    }
    InkPixels out;
    out.space = color.space;
    out.depth = color.depth;
    out.width = width;
    out.height = height;
    out.samples.assign(out.pixel_count() * all, 0);
    const Transform transform = from_srgb_transform(color, TYPE_RGBA_8, int_format(color.space, color.depth), source);
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
            if (grey_by_value && p[0] == p[1] && p[1] == p[2]) samples[x * all] = color.depth == 16 ? static_cast<std::uint16_t>(p[0] * 257) : p[0];
            samples[x * all + ch] = color.depth == 16 ? static_cast<std::uint16_t>(p[3] * 257) : p[3];
        }
    }
    return out;
}

InkPixels from_srgb(const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height, const PageColor& color, bool gray_on_k,
                    const core::Bytes& source) {
    if (color.space == InkSpace::Multi) {
        return multi_pixels(color, width, height, [&](std::size_t i, double* values, double& alpha) {
            for (unsigned c = 0; c < 3; ++c) values[c] = std::clamp(static_cast<double>(rgba[i * 4 + c]), 0.0, 1.0);
            alpha = std::clamp(static_cast<double>(rgba[i * 4 + 3]), 0.0, 1.0);
        });
    }
    const unsigned ch = core::colour_channels(color.space), all = ch + 1;
    gray_on_k = gray_on_k && !source;
    const bool grey_by_value = !source && color.space == InkSpace::Gray && (gray_on_k || sgray(color));
    std::vector<float> clamped(rgba.size());
    for (std::size_t i = 0; i < rgba.size(); ++i) clamped[i] = std::clamp(rgba[i], 0.0f, 1.0f);
    if (color.depth == 32) {
        InkPixels out = core::blank_ink_pixels(color.space, 32, width, height);
        const Transform transform = from_srgb_transform(color, TYPE_RGBA_FLT, float_format(color.space), source);
        std::vector<float> row(static_cast<std::size_t>(width) * ch);
        for (std::uint32_t y = 0; y < height; ++y) {
            const float* src = clamped.data() + std::size_t(y) * width * 4;
            cmsDoTransform(transform->handle, src, row.data(), width);
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                const float* p = src + x * 4;
                for (unsigned c = 0; c < ch; ++c) out.set(i, c, from_lcms_value(color.space, c, row[x * ch + c]));
                if (p[0] == p[1] && p[1] == p[2]) {
                    if (gray_on_k && color.space == InkSpace::Cmyk) {
                        for (unsigned c = 0; c < 3; ++c) out.set(i, c, 0.0);
                        out.set(i, 3, 1.0 - p[0]);
                    } else if (grey_by_value) {
                        // (an HDR grey keeps its value: what came in clamped is the original's)
                        const float v = rgba[(std::size_t(y) * width + x) * 4];
                        out.set(i, 0, rgba[(std::size_t(y) * width + x) * 4 + 1] == v && rgba[(std::size_t(y) * width + x) * 4 + 2] == v ? v : p[0]);
                    }
                }
                out.set(i, ch, p[3]);
            }
        }
        return out;
    }
    InkPixels out;
    out.space = color.space;
    out.depth = color.depth;
    out.width = width;
    out.height = height;
    out.samples.assign(out.pixel_count() * all, 0);
    const Transform transform = from_srgb_transform(color, TYPE_RGBA_FLT, int_format(color.space, 16), source);
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
            if (grey_by_value && p[0] == p[1] && p[1] == p[2]) samples[x * all] = clamp_sample(p[0] * most, most);
            samples[x * all + ch] = clamp_sample(p[3] * most, most);
        }
    }
    return out;
}

Image to_rgba8(const InkPixels& pixels, const PageColor& color) {
    Image out = Image::create("RGBA", Size{static_cast<int>(pixels.width), static_cast<int>(pixels.height)}, Ink{0, 0, 0, 0});
    const unsigned ch = pixels.colours(), all = pixels.channels();
    const auto byte = [](double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    if (pixels.space == InkSpace::Multi) {  // (its plates printed over white paper)
        std::vector<double> density(static_cast<std::size_t>(pixels.width) * ch);
        std::vector<float> srgb(static_cast<std::size_t>(pixels.width) * 3);
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                for (unsigned c = 0; c < ch; ++c) density[x * ch + c] = pixels.value(std::size_t(y) * pixels.width + x, c);
            }
            multi_shown(color, density.data(), srgb.data(), pixels.width);
            auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                for (unsigned c = 0; c < 3; ++c) dst[x * 4 + c] = byte(srgb[x * 3 + c]);
                dst[x * 4 + 3] = byte(pixels.value(std::size_t(y) * pixels.width + x, ch));
            }
        }
        return out;
    }
    if (sgray(color) || pixels.is_float()) {
        // (sGray: a grey v is sRGB's (v, v, v), numerically; 32 bits: through the float transform)
        std::vector<float> lcms(static_cast<std::size_t>(pixels.width) * ch);
        std::vector<unsigned char> row(static_cast<std::size_t>(pixels.width) * 3);
        const bool numeric = sgray(color);
        const Transform transform = numeric ? nullptr : to_srgb_transform(color, float_format(pixels.space), TYPE_RGB_8);
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            if (!numeric) {
                lcms_row(pixels, y, lcms.data());
                cmsDoTransform(transform->handle, lcms.data(), row.data(), pixels.width);
            }
            auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                const std::size_t i = std::size_t(y) * pixels.width + x;
                if (numeric) {
                    // (8 and 16 bits as their samples narrow: 16 to 8 by rounding)
                    const unsigned char v = pixels.is_float() ? byte(pixels.value(i, 0))
                                            : pixels.depth == 8 ? static_cast<unsigned char>(pixels.samples[i * all])
                                                                : static_cast<unsigned char>(narrowed(pixels.samples[i * all], 8));
                    dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = v;
                } else {
                    for (unsigned c = 0; c < 3; ++c) dst[x * 4 + c] = row[x * 3 + c];
                }
                dst[x * 4 + 3] = pixels.is_float() ? byte(pixels.value(i, ch))
                                 : pixels.depth == 8 ? static_cast<unsigned char>(pixels.samples[i * all + ch])
                                                     : static_cast<unsigned char>(narrowed(pixels.samples[i * all + ch], 8));
            }
        }
        return out;
    }
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
    const unsigned ch = pixels.colours(), all = pixels.channels();
    std::vector<float> out(pixels.pixel_count() * 4);
    if (pixels.space == InkSpace::Multi) {  // (its plates printed over white paper)
        std::vector<double> density(ch);
        float srgb[3];
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            for (unsigned c = 0; c < ch; ++c) density[c] = pixels.value(i, c);
            multi_shown(color, density.data(), srgb, 1);
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = srgb[c];
            out[i * 4 + 3] = static_cast<float>(pixels.value(i, ch));
        }
        return out;
    }
    if (sgray(color)) {  // (numerically: an HDR grey of 32 bits stays one)
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            const auto v = static_cast<float>(pixels.is_float() ? pixels.value(i, 0) : std::clamp(pixels.value(i, 0), 0.0, 1.0));
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v;
            out[i * 4 + 3] = static_cast<float>(pixels.value(i, ch));
        }
        return out;
    }
    std::vector<float> row(static_cast<std::size_t>(pixels.width) * 3);
    if (pixels.is_float()) {
        const Transform transform = to_srgb_transform(color, float_format(pixels.space), TYPE_RGB_FLT);
        std::vector<float> lcms(static_cast<std::size_t>(pixels.width) * ch);
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            lcms_row(pixels, y, lcms.data());
            cmsDoTransform(transform->handle, lcms.data(), row.data(), pixels.width);
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                const std::size_t i = std::size_t(y) * pixels.width + x;
                for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = std::clamp(row[x * 3 + c], 0.0f, 1.0f);
                out[i * 4 + 3] = static_cast<float>(pixels.value(i, ch));
            }
        }
        return out;
    }
    const Transform transform = to_srgb_transform(color, int_format(pixels.space, 16), TYPE_RGB_FLT);
    const std::vector<std::uint16_t> wide = widened(pixels);
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

std::vector<float> to_rgb_floats(const InkPixels& pixels, const PageColor& color, const core::RgbMode& target) {
    if (pixels.space == InkSpace::Multi) {
        // (PhotoCraft's convert_from: three plates read back as the values they were, 1 − each — in the RGB page's
        // space as they are —; another count as the plates print, from sRGB into the page's profile)
        if (pixels.colours() == 3) {
            std::vector<float> out(pixels.pixel_count() * 4);
            for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
                for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = static_cast<float>(1.0 - pixels.value(i, c));
                out[i * 4 + 3] = static_cast<float>(pixels.value(i, 3));
            }
            return out;
        }
        std::vector<float> out = to_srgb_floats(pixels, color);
        if (target.icc || target.intent != "relative" || !target.bpc) rgb_to_rgb(out, nullptr, target);
        return out;
    }
    if (!target.icc && target.intent == "relative" && target.bpc) return to_srgb_floats(pixels, color);  // (as the screen shows it)
    const unsigned ch = pixels.colours(), all = pixels.channels();
    std::vector<float> out(pixels.pixel_count() * 4);
    std::vector<float> row(static_cast<std::size_t>(pixels.width) * 3);
    if (pixels.is_float()) {
        const Transform transform = to_rgb_transform(color, target, float_format(pixels.space), TYPE_RGB_FLT);
        std::vector<float> lcms(static_cast<std::size_t>(pixels.width) * ch);
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            lcms_row(pixels, y, lcms.data());
            cmsDoTransform(transform->handle, lcms.data(), row.data(), pixels.width);
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                const std::size_t i = std::size_t(y) * pixels.width + x;
                for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = std::clamp(row[x * 3 + c], 0.0f, 1.0f);
                out[i * 4 + 3] = static_cast<float>(pixels.value(i, ch));
            }
        }
        return out;
    }
    const Transform transform = to_rgb_transform(color, target, int_format(pixels.space, 16), TYPE_RGB_FLT);
    const std::vector<std::uint16_t> wide = widened(pixels);
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

Image to_rgb8(const InkPixels& pixels, const PageColor& color, const core::RgbMode& target) {
    if (!target.icc && target.intent == "relative" && target.bpc && pixels.space != InkSpace::Multi) return to_rgba8(pixels, color);  // (as shown)
    const std::vector<float> rgba = to_rgb_floats(pixels, color, target);
    Image out = Image::create("RGBA", Size{static_cast<int>(pixels.width), static_cast<int>(pixels.height)}, Ink{0, 0, 0, 0});
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (std::uint32_t x = 0; x < pixels.width * 4; ++x) {
            dst[x] = static_cast<unsigned char>(std::lround(std::clamp(rgba[std::size_t(y) * pixels.width * 4 + x], 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

void rgb_to_rgb(std::vector<float>& rgba, const core::Bytes& from, const core::RgbMode& to) {
    const std::string key = "rgb:" + rgb_key(from) + ">" + rgb_key(to.icc) + ":" + to.intent + (to.bpc ? "" : ":nobpc");
    const Transform transform = cached(key, [&] {
        const auto a = rgb_profile(from);
        const auto b = rgb_profile(to.icc);
        return cmsCreateTransform(a->handle, TYPE_RGBA_FLT, b->handle, TYPE_RGBA_FLT, intent_number(to.intent),
                                  to.bpc ? kFlags | cmsFLAGS_COPY_ALPHA : cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
    });
    for (float& v : rgba) v = std::clamp(v, 0.0f, 1.0f);
    constexpr std::size_t kRow = 4096;
    for (std::size_t at = 0; at < rgba.size() / 4; at += kRow) {
        const auto n = static_cast<cmsUInt32Number>(std::min(kRow, rgba.size() / 4 - at));
        cmsDoTransform(transform->handle, rgba.data() + at * 4, rgba.data() + at * 4, n);
    }
    for (float& v : rgba) v = std::clamp(v, 0.0f, 1.0f);
}

std::optional<core::ProfileSpace> profile_space(std::string_view icc) {
    if (icc.empty() || icc.size() > core::kProfileMaxBytes) return std::nullopt;
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) return std::nullopt;
    const ProfileHandle held(handle);
    switch (cmsGetColorSpace(handle)) {
        case cmsSigRgbData: return core::ProfileSpace::Rgb;
        case cmsSigGrayData: return core::ProfileSpace::Gray;
        case cmsSigCmykData: return core::ProfileSpace::Cmyk;
        case cmsSigLabData: return core::ProfileSpace::Lab;
        default: return std::nullopt;
    }
}

InkPixels convert(const InkPixels& pixels, const PageColor& from, const PageColor& to, bool gray_on_k) {
    if (to.space == InkSpace::Multi && from.space != InkSpace::Multi) {
        // (PhotoCraft's Multichannel: CMYK's plates as they are, Lab's 1 − its encoding, a grey's density through each
        // plate's curve; a 32-bit grey's HDR taken as 0..1)
        const unsigned ch = pixels.colours();
        return multi_pixels(to, pixels.width, pixels.height, [&](std::size_t i, double* values, double& alpha) {
            for (unsigned c = 0; c < ch && c < 4; ++c) values[c] = std::clamp(pixels.value(i, c), 0.0, 1.0);
            alpha = pixels.value(i, ch);
        });
    }
    if (from.space == InkSpace::Multi && to.space == InkSpace::Multi) {  // (the same plates: the depth only)
        InkPixels out = core::blank_ink_pixels(InkSpace::Multi, to.depth, pixels.width, pixels.height, pixels.planes);
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            for (unsigned c = 0; c < pixels.channels(); ++c) out.set(i, c, pixels.value(i, c));
        }
        return out;
    }
    if (from.space == InkSpace::Multi) {
        // (PhotoCraft's convert_from: as many plates as the mode has read back — CMYK's as they are, Lab's and a grey's
        // 1 − each —; another count as the plates print, through sRGB)
        const unsigned n = pixels.colours(), out_ch = core::colour_channels(to.space);
        if (n == out_ch) {
            InkPixels out = core::blank_ink_pixels(to.space, to.depth, pixels.width, pixels.height);
            for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
                for (unsigned c = 0; c < n; ++c) out.set(i, c, to.space == InkSpace::Cmyk ? pixels.value(i, c) : 1.0 - pixels.value(i, c));
                out.set(i, n, pixels.value(i, n));
            }
            return out;
        }
        return from_srgb(to_srgb_floats(pixels, from), pixels.width, pixels.height, to);
    }
    const unsigned in_ch = pixels.colours(), in_all = pixels.channels();
    const unsigned out_ch = core::colour_channels(to.space), out_all = out_ch + 1;
    const bool same = from.space == to.space && (from.space == InkSpace::Lab || from.profile == to.profile);
    // (a grey page's greys onto K alone, asked for: K = 1 − its lightness)
    const bool onto_k = gray_on_k && from.space == InkSpace::Gray && to.space == InkSpace::Cmyk;
    if (pixels.is_float() || to.depth == 32) {
        InkPixels out = core::blank_ink_pixels(to.space, to.depth, pixels.width, pixels.height);
        std::optional<Transform> transform;
        if (!same && !onto_k) transform = between_transform(from, to, float_format(from.space), float_format(to.space));
        std::vector<float> in(static_cast<std::size_t>(pixels.width) * in_ch), made(static_cast<std::size_t>(pixels.width) * out_ch);
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            if (transform) {
                lcms_row(pixels, y, in.data());
                cmsDoTransform((*transform)->handle, in.data(), made.data(), pixels.width);
            }
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                const std::size_t i = std::size_t(y) * pixels.width + x;
                if (same) {
                    for (unsigned c = 0; c < out_ch; ++c) out.set(i, c, pixels.value(i, c));
                } else if (onto_k) {
                    for (unsigned c = 0; c < 3; ++c) out.set(i, c, 0.0);
                    out.set(i, 3, 1.0 - std::clamp(pixels.value(i, 0), 0.0, 1.0));
                } else {
                    for (unsigned c = 0; c < out_ch; ++c) out.set(i, c, from_lcms_value(to.space, c, made[x * out_ch + c]));
                }
                out.set(i, out_ch, pixels.value(i, in_ch));
            }
        }
        return out;
    }
    InkPixels out;
    out.space = to.space;
    out.depth = to.depth;
    out.width = pixels.width;
    out.height = pixels.height;
    out.samples.assign(out.pixel_count() * out_all, 0);
    std::vector<std::uint16_t> wide = widened(pixels);
    std::vector<std::uint16_t> made;
    if (same) {
        made = std::move(wide);
    } else if (onto_k) {
        made.assign(out.pixel_count() * out_ch, 0);
        for (std::size_t i = 0; i < out.pixel_count(); ++i) made[i * 4 + 3] = static_cast<std::uint16_t>(65535 - wide[i]);
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

Image lab_to_rgb_as_pillow(const Image& lab, std::string_view mode) {
    // (Pillow's _imagingcms: a "LAB" image is Lab V2 with one extra byte, "RGB", "RGBA" and "RGBX" are TYPE_RGBA_8)
    static const Transform transform = [] {
        const ProfileHandle srgb(cmsCreate_sRGBProfile());
        const ProfileHandle lab_profile(cmsCreateLab2Profile(nullptr));
        const cmsUInt32Number lab_v2 = COLORSPACE_SH(PT_LabV2) | CHANNELS_SH(3) | BYTES_SH(1) | EXTRA_SH(1);
        cmsHTRANSFORM handle = cmsCreateTransform(lab_profile.handle, lab_v2, srgb.handle, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
        if (handle == nullptr) throw core::Error("value", "the Lab to sRGB transform cannot be made");
        return std::make_shared<const TransformHandle>(handle);
    }();
    Image out = Image::create_blank(mode, lab.size());
    for (int y = 0; y < lab.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(lab.raw()->image[y]);
        auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        cmsDoTransform(transform->handle, row, dst, static_cast<cmsUInt32Number>(lab.width()));
        for (int x = 0; x < lab.width(); ++x) dst[x * 4 + 3] = row[x * 4 + 3];  // (pyCMScopyAux: the extra byte as it is)
    }
    return out;
}

std::string picture_icc(std::string_view b) {
    const auto u8 = [&](std::size_t at) { return static_cast<unsigned>(static_cast<unsigned char>(b[at])); };
    const auto be16 = [&](std::size_t at) { return (u8(at) << 8) | u8(at + 1); };
    const auto be32 = [&](std::size_t at) { return (std::uint32_t(be16(at)) << 16) | be16(at + 2); };
    if (b.size() > 4 && u8(0) == 0xff && u8(1) == 0xd8) {  // JPEG: the APP2 ICC_PROFILE chunks in their order
        std::vector<std::pair<unsigned, std::string>> chunks;
        std::size_t at = 2;
        while (at + 4 <= b.size() && u8(at) == 0xff) {
            const unsigned marker = u8(at + 1);
            if (marker == 0xda || marker == 0xd9) break;
            const std::size_t length = be16(at + 2);
            if (length < 2 || at + 2 + length > b.size()) break;
            if (marker == 0xe2 && length >= 16 && b.substr(at + 4, 12) == std::string_view("ICC_PROFILE\0", 12)) {
                chunks.emplace_back(u8(at + 16), std::string(b.substr(at + 18, length - 16)));
            }
            at += 2 + length;
        }
        std::sort(chunks.begin(), chunks.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        std::string out;
        for (const auto& [n, data] : chunks) out += data;
        return out;
    }
    if (b.size() > 30 && b.substr(0, 4) == "8BPS") {  // PSD: the image resources after the colour mode data
        std::size_t at = 26;
        const std::size_t colour_data = be32(at);
        at += 4 + colour_data;
        if (at + 4 > b.size()) return {};
        const std::size_t resources = be32(at);
        at += 4;
        const std::size_t end = std::min(b.size(), at + resources);
        while (at + 12 <= end && b.substr(at, 4) == "8BIM") {
            const unsigned id = be16(at + 4);
            std::size_t name = u8(at + 6) + 1;
            if (name % 2) ++name;
            const std::size_t size_at = at + 6 + name;
            if (size_at + 4 > end) break;
            const std::size_t size = be32(size_at);
            if (size_at + 4 + size > end) break;
            if (id == 1039) return std::string(b.substr(size_at + 4, size));
            at = size_at + 4 + size + (size % 2);
        }
    }
    return {};
}

InkPixels picture_inks(std::string_view bytes, const PageColor& color) {
    const Image picture = open_image(bytes, kPillowOpenLimits);
    if (picture.mode() == "CMYK") return from_cmyk_image(picture, nullptr, picture_icc(bytes), color);
    if (picture.mode() == "LAB") return from_lab_image(picture, nullptr, color);
    return from_rgb(picture.convert("RGBA"), color);
}

std::uint16_t encode_sample(InkSpace space, unsigned channel, double value, int depth) {
    const double most = depth == 8 ? 255.0 : 65535.0;  // (32 bits: compared as 16)
    const unsigned ch = core::colour_channels(space);
    if (channel >= ch || space == InkSpace::Cmyk) return clamp_sample(value * most, most);
    if (space == InkSpace::Gray) return clamp_sample((1.0 - value) * most, most);  // (its lightness)
    if (channel == 0) return clamp_sample(value / 100.0 * most, most);
    return clamp_sample(depth != 8 ? (value + 128.0) * 257.0 : value + 128.0, most);
}

double decode_sample(InkSpace space, unsigned channel, std::uint16_t sample, int depth) {
    const double most = depth == 8 ? 255.0 : 65535.0;
    const unsigned ch = core::colour_channels(space);
    if (channel >= ch || space == InkSpace::Cmyk) return sample / most;
    if (space == InkSpace::Gray) return 1.0 - sample / most;
    if (channel == 0) return sample / most * 100.0;
    return depth != 8 ? sample / 257.0 - 128.0 : sample - 128.0;
}

bool same_inks(const PageColor& color, const std::vector<double>& a, const std::vector<double>& b) {
    const unsigned ch = color.planes();
    if (a.size() < ch || b.size() < ch) return false;
    for (unsigned c = 0; c < ch; ++c) {
        if (encode_sample(color.space, c, a[c], color.depth) != encode_sample(color.space, c, b[c], color.depth)) return false;
    }
    return true;
}

std::array<int, 3> rgb_for_inks(const PageColor& color, const std::vector<double>& ink) {
    // (a grey the rules give these inks — Genko's black for K 100% alone, white for no ink —: that grey, which every
    // page prints so; Genko's default black first)
    for (const int v : {20, 255, 0, 10, 15}) {
        if (same_inks(color, ink_of(color, {v, v, v}), ink)) return {v, v, v};
    }
    if ((color.space == InkSpace::Cmyk && ink.size() == 4 && ink[0] == 0 && ink[1] == 0 && ink[2] == 0) || color.space == InkSpace::Gray) {
        for (int v = 1; v < 255; ++v) {  // (K alone, a grey page's k: the grey whose rules give it)
            if (same_inks(color, ink_of(color, {v, v, v}), ink)) return {v, v, v};
        }
    }
    const std::array<double, 3> shown = srgb_of(color, ink);
    std::array<int, 3> rgb{};
    for (unsigned c = 0; c < 3; ++c) rgb[c] = static_cast<int>(std::lround(std::clamp(shown[c], 0.0, 1.0) * 255.0));
    // (a colour free for these inks: not one the rules keep — Genko's blacks, white — nor one the table holds for others)
    const auto free_for = [&](const std::array<int, 3>& c) {
        if (c[0] == c[1] && c[1] == c[2] && (c[0] == 0 || c[0] == 10 || c[0] == 15 || c[0] == 20 || c[0] == 255)) return false;
        const core::InkEntry* held = core::ink_for(color, c);
        return held == nullptr || same_inks(color, held->ink, ink);
    };
    if (free_for(rgb)) return rgb;
    for (int step = 1; step <= 8; ++step) {
        for (const int sign : {1, -1}) {
            for (int channel = 2; channel >= 0; --channel) {
                std::array<int, 3> moved = rgb;
                moved[channel] += sign * step;
                if (moved[channel] < 0 || moved[channel] > 255) continue;
                if (free_for(moved)) return moved;
            }
        }
    }
    throw core::Error("value", "no RGB colour near these inks is free in the page's table");
}

double gamut_distance(const PageColor& color, const std::array<int, 3>& rgb) {
    // (Lab holds every sRGB colour; a grey page none but its greys; a multichannel page's plates are not a gamut)
    if (color.space == InkSpace::Lab || color.space == InkSpace::Multi) return 0.0;
    const std::array<double, 3> srgb{rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0};
    const std::array<double, 3> back = srgb_of(color, ink_of_srgb(color, srgb));
    const Transform lab = cached("srgb>lab:dbl", [] {
        const ProfileHandle srgb_profile(cmsCreate_sRGBProfile());
        const ProfileHandle lab_profile(cmsCreateLab4Profile(nullptr));
        return cmsCreateTransform(srgb_profile.handle, TYPE_RGB_DBL, lab_profile.handle, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
    });
    double in[6] = {srgb[0], srgb[1], srgb[2], back[0], back[1], back[2]};
    cmsCIELab out[2];
    cmsDoTransform(lab->handle, in, out, 2);
    return cmsDeltaE(&out[0], &out[1]);
}

}  // namespace genko::render::ink
