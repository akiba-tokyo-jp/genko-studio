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

// The picture's RGB bytes (3 a pixel) through an 8-bit transform to `out_bytes` a pixel.
std::string through(const std::string& rgb, cmsHTRANSFORM transform, std::size_t pixels, std::size_t out_bytes) {
    std::string out(pixels * out_bytes, '\0');
    constexpr std::size_t kRun = 1 << 16;
    for (std::size_t at = 0; at < pixels; at += kRun) {
        const std::size_t n = std::min(kRun, pixels - at);
        cmsDoTransform(transform, rgb.data() + at * 3, out.data() + at * out_bytes, static_cast<cmsUInt32Number>(n));
    }
    return out;
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

// The page's numbers from the screen's sRGB (back through the source profile: relative, black point compensated, as
// the page was shown), 3 bytes a pixel.
std::string numbers(const std::string& rgb, std::size_t pixels, const Source& source) {
    if (!source.icc) return rgb;
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

bool Source::operator==(const Source& other) const { return gray == other.gray && same_bytes(icc, other.icc); }

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
                const Profile proof = open(setup.icc);
                const cmsColorSpaceSignature space = cmsGetColorSpace(proof.handle);
                (void)format_of(space, false);  // (refused when not RGB, CMYK or grey)
                const TransformPtr t = kept("proof:" + bytes_key(setup.icc) + ":" + setup.intent + (setup.bpc ? ":bpc" : "") + (setup.simulate_paper ? ":paper" : ""), [&] {
                    const Profile srgb(cmsCreate_sRGBProfile());
                    return cmsCreateProofingTransform(srgb.handle, TYPE_RGB_8, srgb.handle, TYPE_RGB_8, proof.handle, intent_number(setup.intent),
                                                      setup.simulate_paper ? INTENT_ABSOLUTE_COLORIMETRIC : INTENT_RELATIVE_COLORIMETRIC,
                                                      cmsFLAGS_SOFTPROOFING | (setup.bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | kNoCache);
                });
                shown = through(bytes, t->handle, pixels, 3);
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
    static std::shared_ptr<Gamut> make(std::vector<float> grid) {
        auto out = std::make_shared<Gamut>();
        out->grid_ = std::move(grid);
        return out;
    }
};

namespace {

constexpr int kG = Gamut::kGrid;
constexpr std::size_t kPoints = static_cast<std::size_t>(kG) * kG * kG;

// The grid's sRGB points (r fastest), 0..1.
std::vector<float> grid_points() {
    std::vector<float> rgb(kPoints * 3);
    for (int b = 0; b < kG; ++b)
        for (int g = 0; g < kG; ++g)
            for (int r = 0; r < kG; ++r) {
                const std::size_t i = static_cast<std::size_t>(r + g * kG + b * kG * kG) * 3;
                rgb[i] = static_cast<float>(r) / (kG - 1);
                rgb[i + 1] = static_cast<float>(g) / (kG - 1);
                rgb[i + 2] = static_cast<float>(b) / (kG - 1);
            }
    return rgb;
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

std::vector<float> delta_es(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a.size() / 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double dl = a[i * 3] - b[i * 3], da = a[i * 3 + 1] - b[i * 3 + 1], db = a[i * 3 + 2] - b[i * 3 + 2];
        out[i] = static_cast<float>(std::sqrt(dl * dl + da * da + db * db));
    }
    return out;
}

// Through an ICC proof profile: sRGB → the profile (relative, no black point compensation), clipped, → Lab.
std::vector<float> gamut_grid(const core::Bytes& icc) {
    const Profile proof = open(icc);
    const cmsColorSpaceSignature space = cmsGetColorSpace(proof.handle);
    const cmsUInt32Number format = format_of(space, true);
    const unsigned channels = space == cmsSigCmykData ? 4 : space == cmsSigGrayData ? 1 : 3;
    const float most = space == cmsSigCmykData ? 100.0f : 1.0f;  // (Little CMS's float CMYK is in %)
    const std::vector<float> rgb = grid_points();
    const Profile srgb(cmsCreate_sRGBProfile());
    const Profile lab(cmsCreateLab4Profile(nullptr));
    const Transform to(cmsCreateTransform(srgb.handle, TYPE_RGB_FLT, proof.handle, format, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    const Transform back(cmsCreateTransform(proof.handle, format, lab.handle, TYPE_Lab_FLT, INTENT_RELATIVE_COLORIMETRIC, kNoCache));
    if (to.handle == nullptr || back.handle == nullptr) refuse("the gamut check cannot be made with this profile");
    std::vector<float> device(kPoints * channels);
    cmsDoTransform(to.handle, rgb.data(), device.data(), static_cast<cmsUInt32Number>(kPoints));
    for (float& v : device) v = std::clamp(v, 0.0f, most);
    std::vector<float> via(kPoints * 3);
    cmsDoTransform(back.handle, device.data(), via.data(), static_cast<cmsUInt32Number>(kPoints));
    return delta_es(lab_of(rgb), via);
}

// Through Genko's plain conversion for print (no profile): render/colour::proof's round trip, in 8 bits.
std::vector<float> plain_grid() {
    std::string bytes(kPoints * 3, '\0');
    const std::vector<float> rgb = grid_points();
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(rgb[i] * 255.0f)));
    const Size size{static_cast<int>(kPoints), 1};
    const std::string proofed = colour::proof(Image::frombytes("RGB", size, bytes)).tobytes();
    std::vector<float> a(kPoints * 3), b(kPoints * 3);
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

std::mutex gamut_lock;
std::vector<std::pair<std::string, std::shared_ptr<const Gamut>>> gamuts;

// Grid coordinates of the 8-bit values: the cell and the weight of its far side.
struct Axis {
    std::array<int, 256> cell{};
    std::array<float, 256> t{};
};
const Axis& axis() {
    static const Axis a = [] {
        Axis out;
        for (int v = 0; v < 256; ++v) {
            const float x = static_cast<float>(v) / 255.0f * (kG - 1);
            const int i = std::min(static_cast<int>(x), kG - 2);
            out.cell[static_cast<std::size_t>(v)] = i;
            out.t[static_cast<std::size_t>(v)] = x - static_cast<float>(i);
        }
        return out;
    }();
    return a;
}

}  // namespace

std::shared_ptr<const Gamut> Gamut::of(const Setup& setup) {
    core::Bytes icc = setup.icc;
    if (!icc && setup.export_icc) icc = read_file(*setup.export_icc);
    const std::string key = icc ? bytes_key(icc) : std::string("plain");
    {
        const std::lock_guard<std::mutex> held(gamut_lock);
        for (const auto& [k, g] : gamuts)
            if (k == key) return g;
    }
    std::shared_ptr<const Gamut> made = GamutMaker::make(icc ? gamut_grid(icc) : plain_grid());
    const std::lock_guard<std::mutex> held(gamut_lock);
    if (gamuts.size() >= 8) gamuts.erase(gamuts.begin());
    gamuts.emplace_back(key, made);
    return made;
}

float Gamut::delta_e(int r, int g, int b) const {
    const Axis& a = axis();
    const auto at = [&](int x, int y, int z) { return grid_[static_cast<std::size_t>(x + y * kG + z * kG * kG)]; };
    const int x = a.cell[static_cast<std::size_t>(std::clamp(r, 0, 255))], y = a.cell[static_cast<std::size_t>(std::clamp(g, 0, 255))],
              z = a.cell[static_cast<std::size_t>(std::clamp(b, 0, 255))];
    const float tx = a.t[static_cast<std::size_t>(std::clamp(r, 0, 255))], ty = a.t[static_cast<std::size_t>(std::clamp(g, 0, 255))],
                tz = a.t[static_cast<std::size_t>(std::clamp(b, 0, 255))];
    const auto lerp = [](float p, float q, float t) { return p + (q - p) * t; };
    const float c00 = lerp(at(x, y, z), at(x + 1, y, z), tx), c10 = lerp(at(x, y + 1, z), at(x + 1, y + 1, z), tx);
    const float c01 = lerp(at(x, y, z + 1), at(x + 1, y, z + 1), tx), c11 = lerp(at(x, y + 1, z + 1), at(x + 1, y + 1, z + 1), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
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
