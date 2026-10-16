#include "render/proof.hpp"

#include <lcms2.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "render/colour.hpp"

namespace genko::render::proof {

namespace {

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
using TransformPtr = std::shared_ptr<const Transform>;

[[noreturn]] void refuse(const std::string& why) { throw core::Error("value", why); }

Profile open(const core::Bytes& icc) {
    cmsHPROFILE handle = icc && !icc->empty() ? cmsOpenProfileFromMem(icc->data(), static_cast<cmsUInt32Number>(icc->size())) : nullptr;
    if (handle == nullptr) refuse("the proof profile cannot be read");
    return Profile(handle);
}

cmsUInt32Number intent_number(std::string_view intent) {
    if (intent == "perceptual") return INTENT_PERCEPTUAL;
    if (intent == "saturation") return INTENT_SATURATION;
    if (intent == "absolute") return INTENT_ABSOLUTE_COLORIMETRIC;
    return INTENT_RELATIVE_COLORIMETRIC;
}

// The transforms made for a proof, kept while the same profile, kind and options are asked for (each tile of a page
// goes through them, on the pool's threads: made without Little CMS's one-pixel cache, which is not shared safely).
std::mutex cache_lock;
std::vector<std::pair<std::string, TransformPtr>> cache;

std::string bytes_key(const core::Bytes& icc) {
    if (!icc) return "-";
    return std::to_string(icc->size()) + ":" + std::to_string(std::hash<std::string>{}(*icc));
}

TransformPtr kept(const std::string& key, const std::function<cmsHTRANSFORM()>& make) {
    {
        const std::lock_guard<std::mutex> held(cache_lock);
        for (const auto& [k, t] : cache)
            if (k == key) return t;
    }
    auto made = std::make_shared<const Transform>(make());
    if (made->handle == nullptr) refuse("the proof cannot be made with this profile");
    const std::lock_guard<std::mutex> held(cache_lock);
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.emplace_back(key, made);
    return made;
}

constexpr cmsUInt32Number kNoCache = cmsFLAGS_NOCACHE;

cmsUInt32Number format_of(cmsColorSpaceSignature space, bool floats) {
    switch (space) {
        case cmsSigCmykData: return floats ? TYPE_CMYK_FLT : TYPE_CMYK_8;
        case cmsSigGrayData: return floats ? TYPE_GRAY_FLT : TYPE_GRAY_8;
        case cmsSigRgbData: return floats ? TYPE_RGB_FLT : TYPE_RGB_8;
        default: refuse("the proof profile is not RGB, CMYK or grey");
    }
}

// The picture's bytes (`in_bytes` a pixel: RGB 3) through a transform to `out_bytes` a pixel.
std::string through(const std::string& rgb, cmsHTRANSFORM transform, std::size_t pixels, std::size_t out_bytes, std::size_t in_bytes = 3) {
    std::string out(pixels * out_bytes, '\0');
    constexpr std::size_t kRun = 1 << 16;
    for (std::size_t at = 0; at < pixels; at += kRun) {
        const std::size_t n = std::min(kRun, pixels - at);
        cmsDoTransform(transform, rgb.data() + at * in_bytes, out.data() + at * out_bytes, static_cast<cmsUInt32Number>(n));
    }
    return out;
}

// 16 bits a value of the profile's space (its device values between the two steps of a soft proof: kept within 0..1).
cmsUInt32Number format16_of(cmsColorSpaceSignature space) {
    switch (space) {
        case cmsSigCmykData: return TYPE_CMYK_16;
        case cmsSigGrayData: return TYPE_GRAY_16;
        case cmsSigRgbData: return TYPE_RGB_16;
        default: refuse("the proof profile is not RGB, CMYK or grey");
    }
}

float srgb_decode(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

float srgb_encode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

const std::array<float, 256>& decode_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = srgb_decode(static_cast<float>(i) / 255.0f);
        return t;
    }();
    return table;
}

// Linear light → sRGB-encoded 8-bit codes, in 1/65535 steps (upstream's encode_table).
const std::vector<unsigned char>& encode_table() {
    static const std::vector<unsigned char> table = [] {
        std::vector<unsigned char> t(65536);
        for (std::size_t i = 0; i < t.size(); ++i)
            t[i] = static_cast<unsigned char>(srgb_encode(static_cast<float>(i) / 65535.0f) * 255.0f + 0.5f);
        return t;
    }();
    return table;
}

unsigned char encode8(float linear) {
    const float v = std::clamp(linear, 0.0f, 1.0f);
    return encode_table()[static_cast<std::size_t>(v * 65535.0f + 0.5f)];
}

// The page's numbers, 3 bytes a pixel: its own values where they are given, else from the screen's sRGB (back through the
// source profile: relative, black point compensated, as the page was shown).
std::string numbers(const std::string& rgb, std::size_t pixels, const Source& source) {
    if (source.values && !source.cmyk && source.values->size() == pixels * source.channels()) {
        const std::vector<float>& v = *source.values;
        std::string out(pixels * 3, '\0');
        const auto byte = [](float x) { return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f))); };
        for (std::size_t i = 0; i < pixels; ++i) {
            for (unsigned c = 0; c < 3; ++c) out[i * 3 + c] = byte(source.gray ? v[i] : v[i * 3 + c]);
        }
        return out;
    }
    if (!source.icc || source.cmyk) return rgb;
    const Profile page = open(source.icc);
    const cmsColorSpaceSignature space = cmsGetColorSpace(page.handle);
    if (space != (source.gray ? cmsSigGrayData : cmsSigRgbData)) refuse("the page's profile is not of its mode");
    const TransformPtr back = kept("numbers:" + bytes_key(source.icc) + (source.gray ? ":g" : ":rgb"), [&] {
        const Profile srgb(cmsCreate_sRGBProfile());
        return cmsCreateTransform(srgb.handle, TYPE_RGB_8, page.handle, source.gray ? TYPE_GRAY_8 : TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_BLACKPOINTCOMPENSATION | kNoCache);
    });
    if (!source.gray) return through(rgb, back->handle, pixels, 3);
    const std::string grey = through(rgb, back->handle, pixels, 1);
    std::string out(pixels * 3, '\0');
    for (std::size_t i = 0; i < pixels; ++i) out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = grey[i];
    return out;
}

// Kind::Plate, Kind::CmyPlate through a CMYK profile: separated (the setup's intent and black point compensation), the
// plates kept, printed back (relative colorimetric, black point compensated).
struct PlateTransforms {
    TransformPtr sep;
    TransformPtr print;
};

PlateTransforms plate_transforms(const core::Bytes& icc, const Setup& setup, bool separate) {
    const Profile proof = open(icc);
    if (cmsGetColorSpace(proof.handle) != cmsSigCmykData) refuse("plate previews need a CMYK proof profile");
    PlateTransforms out;
    if (separate) {
        out.sep = kept("sep:" + bytes_key(icc) + ":" + setup.intent + (setup.bpc ? ":bpc" : ""), [&] {
            const Profile srgb(cmsCreate_sRGBProfile());
            return cmsCreateTransform(srgb.handle, TYPE_RGB_8, proof.handle, TYPE_CMYK_8, intent_number(setup.intent),
                                      (setup.bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | kNoCache);
        });
    }
    out.print = kept("print:" + bytes_key(icc), [&] {
        const Profile srgb(cmsCreate_sRGBProfile());
        return cmsCreateTransform(proof.handle, TYPE_CMYK_8, srgb.handle, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_BLACKPOINTCOMPENSATION | kNoCache);
    });
    return out;
}

// CMYK bytes (4 a pixel) of the plates kept (one: as black ink alone; CMY: without the black) printed to RGB.
std::string print_plates(std::string cmyk, std::size_t pixels, const Setup& setup, cmsHTRANSFORM print) {
    for (std::size_t i = 0; i < pixels; ++i) {
        char* px = cmyk.data() + i * 4;
        if (setup.kind == Kind::Plate) {
            const char v = px[std::clamp(setup.plate, 0, 3)];
            px[0] = px[1] = px[2] = 0;
            px[3] = v;
        } else {
            px[3] = 0;
        }
    }
    return through(cmyk, print, pixels, 3);
}

}  // namespace

std::string kind_id(Kind kind, int plate) {
    switch (kind) {
        case Kind::Profile: return "profile";
        case Kind::Plate: return plate == 0 ? "workingCyanPlate" : plate == 1 ? "workingMagentaPlate" : plate == 2 ? "workingYellowPlate" : "workingBlackPlate";
        case Kind::CmyPlate: return "workingCmyPlate";
        case Kind::Srgb: return "internetStandardRgb";
        case Kind::Monitor: return "monitorRgb";
        case Kind::LegacyMac: return "legacyMacintoshRgb";
        case Kind::Protan: return "colorBlindnessProtanopia";
        case Kind::Deutan: return "colorBlindnessDeuteranopia";
    }
    return "profile";
}

std::optional<std::pair<Kind, int>> kind_from_id(std::string_view id) {
    if (id == "profile" || id == "workingCmyk") return std::pair{Kind::Profile, 3};
    if (id == "workingCyanPlate") return std::pair{Kind::Plate, 0};
    if (id == "workingMagentaPlate") return std::pair{Kind::Plate, 1};
    if (id == "workingYellowPlate") return std::pair{Kind::Plate, 2};
    if (id == "workingBlackPlate") return std::pair{Kind::Plate, 3};
    if (id == "workingCmyPlate") return std::pair{Kind::CmyPlate, 3};
    if (id == "internetStandardRgb") return std::pair{Kind::Srgb, 3};
    if (id == "monitorRgb") return std::pair{Kind::Monitor, 3};
    if (id == "legacyMacintoshRgb") return std::pair{Kind::LegacyMac, 3};
    if (id == "colorBlindnessProtanopia") return std::pair{Kind::Protan, 3};
    if (id == "colorBlindnessDeuteranopia") return std::pair{Kind::Deutan, 3};
    return std::nullopt;
}

namespace {
bool same_bytes(const core::Bytes& a, const core::Bytes& b) { return a == b || (a && b && *a == *b); }
}  // namespace

bool Setup::operator==(const Setup& other) const {
    return kind == other.kind && (kind != Kind::Plate || plate == other.plate) && same_bytes(icc, other.icc) && export_icc == other.export_icc &&
           intent == other.intent && bpc == other.bpc && simulate_paper == other.simulate_paper;
}

bool Source::operator==(const Source& other) const { return gray == other.gray && cmyk == other.cmyk && same_bytes(icc, other.icc); }

std::shared_ptr<const std::vector<float>> values_of(std::string_view gkcr) {
    const core::ColorRasterView view(gkcr);
    const std::size_t pixels = static_cast<std::size_t>(view.width()) * view.height();
    auto out = std::make_shared<std::vector<float>>(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto p = view.pixel(i);
        for (unsigned c = 0; c < 3; ++c) (*out)[i * 3 + c] = static_cast<float>(p[c]);
    }
    return out;
}

std::shared_ptr<const std::vector<float>> values_of(const core::InkPixels& inks) {
    const std::size_t pixels = inks.pixel_count();
    const unsigned colours = inks.colours();
    auto out = std::make_shared<std::vector<float>>(pixels * colours);
    for (std::size_t i = 0; i < pixels; ++i)
        for (unsigned c = 0; c < colours; ++c) (*out)[i * colours + c] = static_cast<float>(inks.value(i, c));
    return out;
}

std::array<float, 3> dichromat(std::array<float, 3> linear, bool protan) {
    // (the paper's matrices, kept verbatim: linear RGB → LMS, and back)
    static constexpr float kRgbLms[3][3] = {{17.8824f, 43.5161f, 4.11935f}, {3.45565f, 27.1554f, 3.86714f}, {0.0299566f, 0.184309f, 1.46709f}};
    static constexpr float kLmsRgb[3][3] = {{0.08094445f, -0.13050441f, 0.11672107f},
                                            {-0.010248533f, 0.054019327f, -0.11361471f},
                                            {-0.000365297f, -0.0041216147f, 0.6935114f}};
    const auto mul = [](const float (&m)[3][3], std::array<float, 3> v) {
        return std::array<float, 3>{m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                                    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
    };
    // (the gamut first reduced, so the colours seen stay displayable)
    for (float& c : linear) c = 0.992052f * c + 0.003974f;
    const auto [l, m, s] = mul(kRgbLms, linear);
    const std::array<float, 3> lms = protan ? std::array<float, 3>{2.02344f * m - 2.52581f * s, m, s} : std::array<float, 3>{l, 0.494207f * l + 1.24827f * s, s};
    std::array<float, 3> out = mul(kLmsRgb, lms);
    for (float& c : out) c = std::clamp(c, 0.0f, 1.0f);
    return out;
}

Image apply(const Image& image, const Setup& setup, const Source& source) {
    const bool rgba = image.mode() == "RGBA";
    const Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::size_t pixels = static_cast<std::size_t>(rgb.width()) * static_cast<std::size_t>(rgb.height());
    Image out;
    if (setup.kind == Kind::Profile && !setup.icc) {
        out = colour::proof(rgb, setup.export_icc);  // (Python's 「CMYK で見る」, as it was)
    } else if ((setup.kind == Kind::Plate || setup.kind == Kind::CmyPlate) && !setup.icc) {
        // (Genko's own conversion: the plates of render/colour::to_cmyk, printed back by its from_cmyk)
        std::string cmyk = colour::to_cmyk(rgb, setup.export_icc).tobytes();
        for (std::size_t i = 0; i < pixels; ++i) {
            char* px = cmyk.data() + i * 4;
            if (setup.kind == Kind::Plate) {
                const char v = px[std::clamp(setup.plate, 0, 3)];
                px[0] = px[1] = px[2] = 0;
                px[3] = v;
            } else {
                px[3] = 0;
            }
        }
        out = colour::from_cmyk(Image::frombytes("CMYK", rgb.size(), cmyk), setup.export_icc);
    } else {
        const std::string bytes = rgb.tobytes();
        std::string shown;
        switch (setup.kind) {
            case Kind::Profile: {
                // (upstream's Transform::proof: the screen's sRGB to the proof profile with the setup's intent and black
                // point compensation, kept within the device's values, then to the screen relative with black point
                // compensation — or absolute without it, the paper simulated. Little CMS's own soft proof never compensates
                // the second step)
                const Profile proof = open(setup.icc);
                const cmsColorSpaceSignature space = cmsGetColorSpace(proof.handle);
                const cmsUInt32Number device = format16_of(space);
                const std::size_t device_bytes = 2 * static_cast<std::size_t>(space == cmsSigCmykData ? 4 : space == cmsSigGrayData ? 1 : 3);
                const std::string key = bytes_key(setup.icc);
                const TransformPtr to = kept("proof-to:" + key + ":" + setup.intent + (setup.bpc ? ":bpc" : ""), [&] {
                    const Profile srgb(cmsCreate_sRGBProfile());
                    return cmsCreateTransform(srgb.handle, TYPE_RGB_8, proof.handle, device, intent_number(setup.intent),
                                              (setup.bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | kNoCache);
                });
                const TransformPtr back = kept("proof-back:" + key + (setup.simulate_paper ? ":paper" : ""), [&] {
                    const Profile srgb(cmsCreate_sRGBProfile());
                    return cmsCreateTransform(proof.handle, device, srgb.handle, TYPE_RGB_8,
                                              setup.simulate_paper ? INTENT_ABSOLUTE_COLORIMETRIC : INTENT_RELATIVE_COLORIMETRIC,
                                              (setup.simulate_paper ? 0 : cmsFLAGS_BLACKPOINTCOMPENSATION) | kNoCache);
                });
                shown = through(through(bytes, to->handle, pixels, device_bytes), back->handle, pixels, 3, device_bytes);
                break;
            }
            case Kind::Plate:
            case Kind::CmyPlate: {
                const PlateTransforms t = plate_transforms(setup.icc, setup, true);
                shown = print_plates(through(bytes, t.sep->handle, pixels, 4), pixels, setup, t.print->handle);
                break;
            }
            case Kind::Srgb:
            case Kind::Monitor:
                shown = numbers(bytes, pixels, source);
                break;
            case Kind::LegacyMac: {
                // the numbers kept, shown with a gamma of 1.8 instead of the sRGB curve
                static const std::array<unsigned char, 256> table = [] {
                    std::array<unsigned char, 256> t{};
                    for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = encode8(std::pow(static_cast<float>(i) / 255.0f, 1.8f));
                    return t;
                }();
                shown = numbers(bytes, pixels, source);
                for (char& c : shown) c = static_cast<char>(table[static_cast<unsigned char>(c)]);
                break;
            }
            case Kind::Protan:
            case Kind::Deutan: {
                const auto& decode = decode_table();
                const bool protan = setup.kind == Kind::Protan;
                shown = bytes;
                for (std::size_t i = 0; i < pixels; ++i) {
                    char* px = shown.data() + i * 3;
                    const auto sim = dichromat({decode[static_cast<unsigned char>(px[0])], decode[static_cast<unsigned char>(px[1])],
                                                decode[static_cast<unsigned char>(px[2])]},
                                               protan);
                    for (int c = 0; c < 3; ++c) px[c] = static_cast<char>(encode8(sim[static_cast<std::size_t>(c)]));
                }
                break;
            }
        }
        out = Image::frombytes("RGB", rgb.size(), shown);
    }
    if (rgba) {
        out = out.convert("RGBA");
        out.putalpha(image.getchannel(3));
    }
    return out;
}

Image page_plates(const core::InkPixels& inks, const core::Bytes& page_icc, const Setup& setup) {
    if (inks.space != core::InkSpace::Cmyk) refuse("the page's plates are a CMYK page's");
    const PlateTransforms t = plate_transforms(page_icc, setup, false);
    const std::size_t pixels = inks.pixel_count();
    std::string cmyk(pixels * 4, '\0');
    for (std::size_t i = 0; i < pixels; ++i)
        for (unsigned c = 0; c < 4; ++c)
            cmyk[i * 4 + c] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(inks.value(i, c), 0.0, 1.0) * 255.0)));
    return Image::frombytes("RGB", Size{static_cast<int>(inks.width), static_cast<int>(inks.height)}, print_plates(std::move(cmyk), pixels, setup, t.print->handle));
}

// --- 色域外警告 ---------------------------------------------------------------------------------------------------------

struct GamutMaker {
    static std::shared_ptr<Gamut> make(unsigned inputs, bool of_source, std::vector<float> grid) {
        auto out = std::make_shared<Gamut>();
        out->inputs_ = inputs;
        out->of_source_ = of_source;
        out->points_ = points_for(inputs);
        out->stride_.assign(inputs, 1);
        for (unsigned d = inputs - 1; d > 0; --d) out->stride_[d - 1] = out->stride_[d] * static_cast<std::size_t>(out->points_);
        out->grid_ = std::move(grid);
        return out;
    }
    // (upstream's GamutCheck: 256 points for one value, 33³ for three, 17⁴ for four)
    static int points_for(unsigned inputs) { return inputs == 1 ? 256 : inputs == 4 ? 17 : Gamut::kGrid; }
};

namespace {

// The grid's points (`inputs` values each, the first slowest), 0..1.
std::vector<float> grid_points(unsigned inputs) {
    const auto n = static_cast<std::size_t>(GamutMaker::points_for(inputs));
    std::size_t total = 1;
    for (unsigned d = 0; d < inputs; ++d) total *= n;
    std::vector<float> out(total * inputs);
    for (std::size_t node = 0; node < total; ++node) {
        std::size_t rest = node;
        for (unsigned d = inputs; d-- > 0;) {
            out[node * inputs + d] = static_cast<float>(rest % n) / static_cast<float>(n - 1);
            rest /= n;
        }
    }
    return out;
}

cmsUInt32Number float_format(unsigned inputs) { return inputs == 1 ? TYPE_GRAY_FLT : inputs == 4 ? TYPE_CMYK_FLT : TYPE_RGB_FLT; }

std::vector<float> delta_es(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a.size() / 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double dl = a[i * 3] - b[i * 3], da = a[i * 3 + 1] - b[i * 3 + 1], db = a[i * 3 + 2] - b[i * 3 + 2];
        out[i] = static_cast<float>(std::sqrt(dl * dl + da * da + db * db));
    }
    return out;
}

// The source's colours (`inputs` values each, 0..1; its profile, none: sRGB) to Lab, and through an ICC proof profile:
// the source to the profile (relative, no black point compensation), clipped, to Lab — their ΔE76.
std::vector<float> gamut_grid(const core::Bytes& icc, const core::Bytes& source_icc, unsigned inputs) {
    const Profile proof = open(icc);
    const cmsColorSpaceSignature space = cmsGetColorSpace(proof.handle);
    const cmsUInt32Number format = format_of(space, true);
    const unsigned channels = space == cmsSigCmykData ? 4 : space == cmsSigGrayData ? 1 : 3;
    const float most = space == cmsSigCmykData ? 100.0f : 1.0f;  // (Little CMS's float CMYK is in %)
    cmsHPROFILE from = source_icc ? cmsOpenProfileFromMem(source_icc->data(), static_cast<cmsUInt32Number>(source_icc->size())) : cmsCreate_sRGBProfile();
    if (from == nullptr) refuse("the page's profile cannot be read");
    const Profile source(from);
    std::vector<float> points = grid_points(inputs);
    if (inputs == 4)
        for (float& v : points) v *= 100.0f;
    const std::size_t count = points.size() / inputs;
    const Profile lab(cmsCreateLab4Profile(nullptr));
    const cmsUInt32Number in = float_format(inputs);
    const Transform direct(cmsCreateTransform(source.handle, in, lab.handle, TYPE_Lab_FLT, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    const Transform to(cmsCreateTransform(source.handle, in, proof.handle, format, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    const Transform back(cmsCreateTransform(proof.handle, format, lab.handle, TYPE_Lab_FLT, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    if (direct.handle == nullptr || to.handle == nullptr || back.handle == nullptr) refuse("the gamut check cannot be made with this profile");
    std::vector<float> a(count * 3), device(count * channels), b(count * 3);
    cmsDoTransform(direct.handle, points.data(), a.data(), static_cast<cmsUInt32Number>(count));
    cmsDoTransform(to.handle, points.data(), device.data(), static_cast<cmsUInt32Number>(count));
    for (float& v : device) v = std::clamp(v, 0.0f, most);
    cmsDoTransform(back.handle, device.data(), b.data(), static_cast<cmsUInt32Number>(count));
    return delta_es(a, b);
}

std::vector<float> lab_of(const std::vector<float>& rgb) {
    const Profile srgb(cmsCreate_sRGBProfile());
    const Profile lab(cmsCreateLab4Profile(nullptr));
    const Transform t(cmsCreateTransform(srgb.handle, TYPE_RGB_FLT, lab.handle, TYPE_Lab_FLT, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    if (t.handle == nullptr) refuse("the gamut check cannot be made");
    std::vector<float> out(rgb.size());
    cmsDoTransform(t.handle, rgb.data(), out.data(), static_cast<cmsUInt32Number>(rgb.size() / 3));
    return out;
}

// Through Genko's plain conversion for print (no profile): render/colour::proof's round trip of sRGB, in 8 bits.
std::vector<float> plain_grid() {
    const std::vector<float> rgb = grid_points(3);
    const std::size_t count = rgb.size() / 3;
    std::string bytes(rgb.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(rgb[i] * 255.0f)));
    const std::string proofed = colour::proof(Image::frombytes("RGB", Size{static_cast<int>(count), 1}, bytes)).tobytes();
    std::vector<float> a(rgb.size()), b(rgb.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<float>(static_cast<unsigned char>(bytes[i])) / 255.0f;
        b[i] = static_cast<float>(static_cast<unsigned char>(proofed[i])) / 255.0f;
    }
    return delta_es(lab_of(a), lab_of(b));
}

core::Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return bytes.empty() ? nullptr : std::make_shared<const std::string>(std::move(bytes));
}

// The export profile's file as it is (its path, size and time), without reading it; none when it is not there.
std::optional<std::string> file_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;
    const auto when = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return "file:" + path.string() + ":" + std::to_string(size) + ":" + std::to_string(when.time_since_epoch().count());
}

std::mutex gamut_lock;
std::vector<std::pair<std::string, std::shared_ptr<const Gamut>>> gamuts;

// Grid coordinates of the 8-bit values on the 33-point grid: the cell and the weight of its far side.
struct Axis {
    std::array<int, 256> cell{};
    std::array<float, 256> t{};
};
const Axis& axis() {
    static const Axis a = [] {
        Axis out;
        for (int v = 0; v < 256; ++v) {
            const float x = static_cast<float>(v) / 255.0f * (Gamut::kGrid - 1);
            const int i = std::min(static_cast<int>(x), Gamut::kGrid - 2);
            out.cell[static_cast<std::size_t>(v)] = i;
            out.t[static_cast<std::size_t>(v)] = x - static_cast<float>(i);
        }
        return out;
    }();
    return a;
}

// Upstream's Clut::tetra: inside one cube of three dimensions (its corner at `base`, the strides along x, y, z), the
// tetrahedron holding (rx, ry, rz) and the path through the cube's corners.
float tetra(const std::vector<float>& d, std::size_t base, std::size_t sx, std::size_t sy, std::size_t sz, float rx, float ry, float rz) {
    const std::size_t c100 = base + sx, c010 = base + sy, c001 = base + sz;
    const std::size_t c110 = base + sx + sy, c101 = base + sx + sz, c011 = base + sy + sz, c111 = base + sx + sy + sz;
    std::size_t p1 = 0, p2 = 0;
    float w1 = 0, w2 = 0, w3 = 0;
    if (rx >= ry) {
        if (ry >= rz) p1 = c100, p2 = c110, w1 = rx, w2 = ry, w3 = rz;
        else if (rx >= rz) p1 = c100, p2 = c101, w1 = rx, w2 = rz, w3 = ry;
        else p1 = c001, p2 = c101, w1 = rz, w2 = rx, w3 = ry;
    } else if (rx >= rz) {
        p1 = c010, p2 = c110, w1 = ry, w2 = rx, w3 = rz;
    } else if (ry >= rz) {
        p1 = c010, p2 = c011, w1 = ry, w2 = rz, w3 = rx;
    } else {
        p1 = c001, p2 = c011, w1 = rz, w2 = ry, w3 = rx;
    }
    const float v0 = d[base], v1 = d[p1], v2 = d[p2], v3 = d[c111];
    return v0 + (v1 - v0) * w1 + (v2 - v1) * w2 + (v3 - v2) * w3;
}

}  // namespace

std::shared_ptr<const Gamut> Gamut::of(const Setup& setup, const Source& source) {
    // (the proof profile: its bytes, or the export profile's file — read only when its check is made)
    std::optional<std::string> proof_key;
    if (setup.icc) proof_key = bytes_key(setup.icc);
    else if (setup.export_icc) proof_key = file_key(*setup.export_icc);
    // (the plain conversion checks sRGB alone: the screen's colours)
    const bool of_source = proof_key && source.icc;
    const unsigned inputs = of_source ? source.channels() : 3;
    const std::string key = (proof_key ? *proof_key : std::string("plain")) + "|" +
                            (of_source ? bytes_key(source.icc) + ":" + std::to_string(inputs) : std::string("srgb"));
    {
        const std::lock_guard<std::mutex> held(gamut_lock);
        for (const auto& [k, g] : gamuts)
            if (k == key) return g;
    }
    const core::Bytes icc = setup.icc ? setup.icc : proof_key ? read_file(*setup.export_icc) : nullptr;
    std::vector<float> grid = icc ? gamut_grid(icc, of_source ? source.icc : nullptr, inputs) : plain_grid();
    std::shared_ptr<const Gamut> made = GamutMaker::make(icc ? inputs : 3, icc && of_source, std::move(grid));
    const std::lock_guard<std::mutex> held(gamut_lock);
    if (gamuts.size() >= 8) gamuts.erase(gamuts.begin());
    gamuts.emplace_back(key, made);
    return made;
}

float Gamut::delta_e(int r, int g, int b) const {
    if (inputs_ != 3) {
        const std::array<float, 3> v{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f};
        return delta_e(v.data());
    }
    const Axis& a = axis();
    const auto u = [](int v) { return static_cast<std::size_t>(std::clamp(v, 0, 255)); };
    const std::size_t base = static_cast<std::size_t>(a.cell[u(r)]) * stride_[0] + static_cast<std::size_t>(a.cell[u(g)]) * stride_[1] +
                             static_cast<std::size_t>(a.cell[u(b)]) * stride_[2];
    return tetra(grid_, base, stride_[0], stride_[1], stride_[2], a.t[u(r)], a.t[u(g)], a.t[u(b)]);
}

float Gamut::delta_e(const float* values) const {
    // (upstream's Clut::locate: each value's cell, the weight of its far side, and the stride along it)
    std::array<std::size_t, 4> base{};
    std::array<float, 4> r{};
    for (unsigned d = 0; d < inputs_; ++d) {
        const float p = std::clamp(std::isfinite(values[d]) ? values[d] : 0.0f, 0.0f, 1.0f) * static_cast<float>(points_ - 1);
        const int i = std::min(static_cast<int>(p), points_ - 2);
        base[d] = static_cast<std::size_t>(i) * stride_[d];
        r[d] = p - static_cast<float>(i);
    }
    if (inputs_ == 1) return grid_[base[0]] + (grid_[base[0] + stride_[0]] - grid_[base[0]]) * r[0];
    if (inputs_ == 3) return tetra(grid_, base[0] + base[1] + base[2], stride_[0], stride_[1], stride_[2], r[0], r[1], r[2]);
    // (four: two tetrahedral readings of the last three, along the first)
    const std::size_t at = base[0] + base[1] + base[2] + base[3];
    const float lo = tetra(grid_, at, stride_[1], stride_[2], stride_[3], r[1], r[2], r[3]);
    if (r[0] <= 0.0f) return lo;
    const float hi = tetra(grid_, at + stride_[0], stride_[1], stride_[2], stride_[3], r[1], r[2], r[3]);
    return lo + (hi - lo) * r[0];
}

std::vector<unsigned char> Gamut::mask(const Image& image, double threshold, std::size_t* out) const {
    const bool rgba = image.mode() == "RGBA";
    const std::string bytes = (rgba || image.mode() == "RGB" ? image : image.convert("RGB")).tobytes();
    const std::size_t step = rgba ? 4 : 3;
    const std::size_t pixels = bytes.size() / step;
    std::vector<unsigned char> mask(pixels, 0);
    std::size_t count = 0;
    const auto u = [&](std::size_t i) { return static_cast<int>(static_cast<unsigned char>(bytes[i])); };
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t at = i * step;
        if (rgba && bytes[at + 3] == 0) continue;
        if (delta_e(u(at), u(at + 1), u(at + 2)) > threshold) {
            mask[i] = 255;
            ++count;
        }
    }
    if (out != nullptr) *out = count;
    return mask;
}

std::vector<unsigned char> Gamut::mask(const std::vector<float>& values, const Image& image, double threshold, std::size_t* out) const {
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height());
    if (values.size() != pixels * inputs_) refuse("the page's values are not the picture's");
    const std::string alpha = image.mode() == "RGBA" ? image.getchannel(3).tobytes() : std::string();
    std::vector<unsigned char> mask(pixels, 0);
    std::size_t count = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!alpha.empty() && alpha[i] == 0) continue;
        if (delta_e(values.data() + i * inputs_) > threshold) {
            mask[i] = 255;
            ++count;
        }
    }
    if (out != nullptr) *out = count;
    return mask;
}

// --- a picture's own profile -------------------------------------------------------------------------------------------

namespace {

std::uint32_t be32(std::string_view b, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
}
std::uint32_t le32(std::string_view b, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at]));
}

constexpr std::size_t kMostProfile = core::kProfileMaxBytes;

std::optional<std::string> inflated(std::string_view data) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return std::nullopt;
    std::string out;
    std::array<char, 65536> buffer{};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    int status = Z_OK;
    while (status == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buffer.data());
        z.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&z, Z_NO_FLUSH);
        out.append(buffer.data(), buffer.size() - z.avail_out);
        if (out.size() > kMostProfile) status = Z_DATA_ERROR;
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) return std::nullopt;
    return out;
}

std::optional<std::string> png_profile(std::string_view b) {
    std::size_t at = 8;
    while (at + 12 <= b.size()) {
        const std::size_t length = be32(b, at);
        const std::string_view type = b.substr(at + 4, 4);
        if (length > b.size() - at - 12) return std::nullopt;
        if (type == "iCCP") {
            const std::string_view data = b.substr(at + 8, length);
            const std::size_t name_end = data.find('\0');
            if (name_end == std::string_view::npos || name_end + 2 > data.size() || data[name_end + 1] != 0) return std::nullopt;
            return inflated(data.substr(name_end + 2));
        }
        if (type == "IDAT" || type == "IEND") return std::nullopt;  // (iCCP comes before the pixels)
        at += 12 + length;
    }
    return std::nullopt;
}

std::optional<std::string> jpeg_profile(std::string_view b) {
    std::map<int, std::string> parts;
    int count = 0;
    std::size_t at = 2;
    while (at + 4 <= b.size() && static_cast<unsigned char>(b[at]) == 0xff) {
        const unsigned marker = static_cast<unsigned char>(b[at + 1]);
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
            at += 2;
            continue;
        }
        if (marker == 0xda || marker == 0xd9) break;  // (the pixels: the profile comes before)
        const std::size_t length = (static_cast<std::size_t>(static_cast<unsigned char>(b[at + 2])) << 8) | static_cast<unsigned char>(b[at + 3]);
        if (length < 2 || at + 2 + length > b.size()) break;
        const std::string_view data = b.substr(at + 4, length - 2);
        if (marker == 0xe2 && data.size() > 14 && data.substr(0, 12) == std::string_view("ICC_PROFILE\0", 12)) {
            const int seq = static_cast<unsigned char>(data[12]);
            count = static_cast<unsigned char>(data[13]);
            parts[seq] = std::string(data.substr(14));
        }
        at += 2 + length;
    }
    if (parts.empty() || count == 0 || static_cast<int>(parts.size()) != count) return std::nullopt;
    std::string out;
    for (int i = 1; i <= count; ++i) {
        const auto it = parts.find(i);
        if (it == parts.end()) return std::nullopt;
        out += it->second;
    }
    return out;
}

std::optional<std::string> tiff_profile(std::string_view b) {
    const bool little = b.substr(0, 2) == "II";
    const auto u16 = [&](std::size_t at) -> std::uint32_t {
        const auto x = static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])), y = static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1]));
        return little ? (y << 8) | x : (x << 8) | y;
    };
    const auto u32 = [&](std::size_t at) { return little ? le32(b, at) : be32(b, at); };
    if (b.size() < 8) return std::nullopt;
    const std::size_t ifd = u32(4);
    if (ifd + 2 > b.size()) return std::nullopt;
    const std::size_t entries = u16(ifd);
    for (std::size_t i = 0; i < entries && ifd + 2 + (i + 1) * 12 <= b.size(); ++i) {
        const std::size_t e = ifd + 2 + i * 12;
        if (u16(e) != 34675) continue;
        const std::size_t count = u32(e + 4);
        if (count <= 4) return std::string(b.substr(e + 8, count));
        const std::size_t offset = u32(e + 8);
        if (count > kMostProfile || offset > b.size() || count > b.size() - offset) return std::nullopt;
        return std::string(b.substr(offset, count));
    }
    return std::nullopt;
}

std::optional<std::string> webp_profile(std::string_view b) {
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const std::string_view type = b.substr(at, 4);
        const std::size_t length = le32(b, at + 4);
        if (length > b.size() - at - 8) return std::nullopt;
        if (type == "ICCP") return std::string(b.substr(at + 8, length));
        at += 8 + length + (length & 1);
    }
    return std::nullopt;
}

std::optional<std::string> psd_profile(std::string_view b) {
    if (b.size() < 30) return std::nullopt;
    std::size_t at = 26;
    const std::size_t mode_data = be32(b, at);  // (the colour mode's data)
    if (mode_data > b.size() - at - 4) return std::nullopt;
    at += 4 + mode_data;
    if (at + 4 > b.size()) return std::nullopt;
    const std::size_t resources = be32(b, at);
    at += 4;
    const std::size_t end = std::min(b.size(), at + resources);
    while (at + 12 <= end && b.substr(at, 4) == "8BIM") {
        const std::uint32_t id = (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 4])) << 8) | static_cast<unsigned char>(b[at + 5]);
        std::size_t name = static_cast<unsigned char>(b[at + 6]) + 1;
        name += name & 1;  // (a Pascal string padded to an even size)
        const std::size_t size_at = at + 6 + name;
        if (size_at + 4 > end) return std::nullopt;
        const std::size_t size = be32(b, size_at);
        if (size > end - size_at - 4) return std::nullopt;
        if (id == 1039) return std::string(b.substr(size_at + 4, size));
        at = size_at + 4 + size + (size & 1);
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> embedded_profile(std::string_view b) {
    std::optional<std::string> out;
    if (b.size() > 8 && b.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) out = png_profile(b);
    else if (b.size() > 4 && static_cast<unsigned char>(b[0]) == 0xff && static_cast<unsigned char>(b[1]) == 0xd8) out = jpeg_profile(b);
    else if (b.size() > 8 && (b.substr(0, 4) == std::string_view("II*\0", 4) || b.substr(0, 4) == std::string_view("MM\0*", 4))) out = tiff_profile(b);
    else if (b.size() > 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") out = webp_profile(b);
    else if (b.size() > 4 && b.substr(0, 4) == "8BPS") out = psd_profile(b);
    if (out && (out->size() < 132 || out->size() > kMostProfile)) out.reset();
    return out;
}

bool same_as_srgb(std::string_view icc) {
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) return false;
    const Profile picture(handle);
    const cmsColorSpaceSignature space = cmsGetColorSpace(picture.handle);
    if (space != cmsSigRgbData && space != cmsSigGrayData) return false;
    const bool grey = space == cmsSigGrayData;
    const Profile srgb(cmsCreate_sRGBProfile());
    const Transform t(cmsCreateTransform(picture.handle, grey ? TYPE_GRAY_DBL : TYPE_RGB_DBL, srgb.handle, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                         cmsFLAGS_BLACKPOINTCOMPENSATION | kNoCache));
    if (t.handle == nullptr) return false;
    for (int b = 0; b < 9; ++b)
        for (int g = 0; g < 9; ++g)
            for (int r = 0; r < 9; ++r) {
                if (grey && (g > 0 || b > 0)) continue;
                const std::array<double, 3> in{r / 8.0, g / 8.0, b / 8.0};
                std::array<double, 3> out{};
                cmsDoTransform(t.handle, in.data(), out.data(), 1);
                for (int c = 0; c < 3; ++c) {
                    const double want = grey ? in[0] : in[static_cast<std::size_t>(c)];
                    if (std::abs(out[static_cast<std::size_t>(c)] - want) > 0.5 / 255.0) return false;
                }
            }
    return true;
}

Image convert_picture(const Image& picture, std::string_view icc, std::string_view intent, bool bpc) {
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (handle == nullptr) refuse("the picture's profile cannot be read");
    const Profile from(handle);
    const cmsColorSpaceSignature space = cmsGetColorSpace(from.handle);
    const std::string_view mode = picture.mode();
    const bool cmyk = mode == "CMYK";
    const bool grey = !cmyk && (mode == "L" || mode == "LA" || mode == "1" || mode.starts_with("I"));
    if (space != (cmyk ? cmsSigCmykData : grey ? cmsSigGrayData : cmsSigRgbData)) refuse("the picture's profile is not of its colours");
    const bool alpha = mode == "RGBA" || mode == "LA" || mode == "PA" || (mode == "P" && picture.convert("RGBA").getextrema().back().first < 255);
    const Image in = cmyk ? picture : grey ? picture.convert(alpha ? "LA" : "L") : picture.convert(alpha ? "RGBA" : "RGB");
    const cmsUInt32Number in_format = cmyk ? TYPE_CMYK_8 : grey ? (alpha ? TYPE_GRAYA_8 : TYPE_GRAY_8) : (alpha ? TYPE_RGBA_8 : TYPE_RGB_8);
    const Profile srgb(cmsCreate_sRGBProfile());
    const Transform t(cmsCreateTransform(from.handle, in_format, srgb.handle, alpha ? TYPE_RGBA_8 : TYPE_RGB_8, intent_number(intent),
                                         (bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_COPY_ALPHA | kNoCache));
    if (t.handle == nullptr) refuse("the picture cannot be converted with its profile");
    const std::string bytes = in.tobytes();
    const std::size_t pixels = static_cast<std::size_t>(in.width()) * static_cast<std::size_t>(in.height());
    std::string out(pixels * (alpha ? 4 : 3), '\0');
    constexpr std::size_t kRun = 1 << 16;
    const std::size_t step = bytes.size() / std::max<std::size_t>(pixels, 1);
    for (std::size_t at = 0; at < pixels; at += kRun) {
        const std::size_t n = std::min(kRun, pixels - at);
        cmsDoTransform(t.handle, bytes.data() + at * step, out.data() + at * (alpha ? 4 : 3), static_cast<cmsUInt32Number>(n));
    }
    return Image::frombytes(alpha ? "RGBA" : "RGB", in.size(), out);
}

}  // namespace genko::render::proof
