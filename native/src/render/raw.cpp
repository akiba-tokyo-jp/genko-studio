// Camera RAW with LibRaw (PC4 F-e): render/raw.hpp. The colour science of the white balance is PhotoCraft v0.2.0's
// raw/src/color.rs (MIT: native/third_party/photocraft/NOTICE.md); the embedded preview's search its raw/src/preview.rs.

#include "render/raw.hpp"

#include <libraw/libraw.h>
#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/codecs/codecs.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/page.hpp"

namespace genko::render::raw {

using core::Json;

namespace {

// --- bytes ---------------------------------------------------------------------------------------------------------------

unsigned u8(std::string_view b, std::size_t at) { return static_cast<unsigned char>(b[at]); }

struct TiffReader {
    std::string_view b;
    bool big = false;
    std::uint32_t u16(std::size_t at) const {
        if (at + 2 > b.size()) return 0;
        return big ? (u8(b, at) << 8) | u8(b, at + 1) : u8(b, at) | (u8(b, at + 1) << 8);
    }
    std::uint32_t u32(std::size_t at) const {
        if (at + 4 > b.size()) return 0;
        return big ? (std::uint32_t(u16(at)) << 16) | u16(at + 2) : u16(at) | (std::uint32_t(u16(at + 2)) << 16);
    }
};

bool tiff_shaped(std::string_view b) { return b.size() >= 8 && (b.substr(0, 4) == std::string_view("II*\0", 4) || b.substr(0, 4) == std::string_view("MM\0*", 4)); }

// The first value of each entry of an IFD (bounded: a few hundred entries, the offsets inside the file).
struct Entry {
    std::uint32_t tag = 0, type = 0, count = 0, value = 0, at = 0;
};
std::vector<Entry> ifd_entries(const TiffReader& t, std::uint32_t offset) {
    std::vector<Entry> out;
    if (offset < 8 || std::uint64_t(offset) + 2 > t.b.size()) return out;
    const std::uint32_t n = std::min<std::uint32_t>(t.u16(offset), 1024);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = std::size_t(offset) + 2 + std::size_t(i) * 12;
        if (e + 12 > t.b.size()) break;
        Entry entry;
        entry.tag = t.u16(e);
        entry.type = t.u16(e + 2);
        entry.count = t.u32(e + 4);
        entry.at = static_cast<std::uint32_t>(e + 8);
        entry.value = entry.type == 3 && entry.count == 1 ? t.u16(e + 8) : t.u32(e + 8);
        out.push_back(entry);
    }
    return out;
}
std::uint32_t next_ifd(const TiffReader& t, std::uint32_t offset) {
    if (offset < 8 || std::uint64_t(offset) + 2 > t.b.size()) return 0;
    const std::uint32_t n = std::min<std::uint32_t>(t.u16(offset), 1024);
    return t.u32(std::size_t(offset) + 2 + std::size_t(n) * 12);
}

// --- LibRaw ----------------------------------------------------------------------------------------------------------------

// One LibRaw of its own a call (libraw_r: reentrant), quiet (its data errors counted, never printed), stopped when asked.
struct Run {
    std::unique_ptr<LibRaw> lr = std::make_unique<LibRaw>(LIBRAW_OPTIONS_NONE);
    const Options* options = nullptr;
    int data_errors = 0;

    explicit Run(const Options* o) : options(o) {
        lr->set_dataerror_handler(&Run::data_error, this);
        lr->set_progress_handler(&Run::progress, this);
    }
    ~Run() {
        if (lr) lr->recycle();
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    static void data_error(void* data, const char*, const INT64) { ++static_cast<Run*>(data)->data_errors; }
    static int progress(void* data, enum LibRaw_progress, int, int) {
        const Run* run = static_cast<const Run*>(data);
        return run->options != nullptr && run->options->stopped && run->options->stopped() ? 1 : 0;
    }
    void check_stop() const {
        if (options != nullptr && options->stopped && options->stopped()) throw Cancelled();
    }
};

[[noreturn]] void failed(int code, const char* stage) {
    if (code == LIBRAW_CANCELLED_BY_CALLBACK) throw Cancelled();
    const std::string why = std::string(libraw_strerror(code)) + " (" + stage + ")";
    if (code == LIBRAW_UNSUFFICIENT_MEMORY || code == LIBRAW_TOO_BIG || code == LIBRAW_MEMPOOL_OVERFLOW)
        throw core::Error("image_too_large", "the camera RAW file is too large to read at once (LibRaw: " + why + ")");
    if (code == LIBRAW_FILE_UNSUPPORTED || code == LIBRAW_NOT_IMPLEMENTED)
        throw core::Error("unidentified_image", "the camera RAW file is not one this build reads (LibRaw: " + why + ")");
    throw core::Error("format", "the camera RAW file cannot be read (LibRaw: " + why + ")");
}

std::string format_name(std::string_view b, const libraw_data_t& d) {
    if (d.idata.dng_version != 0) return "DNG";
    if (b.size() >= 12 && b.substr(4, 4) == "ftyp" && b.substr(8, 4) == "crx ") return "CR3";
    if (b.starts_with("FUJIFILMCCD-RAW")) return "RAF";
    if (b.starts_with("FOVb")) return "X3F";
    if (b.size() >= 4 && b.substr(0, 4) == std::string_view("\0MRM", 4)) return "MRW";
    if (b.size() >= 14 && b.substr(6, 8) == "HEAPCCDR") return "CRW";
    if (b.starts_with("IIRO") || b.starts_with("IIRS") || b.starts_with("MMOR")) return "ORF";
    if (b.size() >= 4 && b.substr(0, 4) == std::string_view("IIU\0", 4)) return "RW2";
    if (b.size() >= 12 && tiff_shaped(b) && b.substr(8, 2) == "CR") return "CR2";
    if (tiff_shaped(b)) return "TIFF/EP";
    return "RAW";
}

std::string text_of(const char* chars, std::size_t n) {
    std::string out(chars, strnlen(chars, n));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return core::utf8_replaced(out);
}

Info info_of(std::string_view bytes, const LibRaw& lr) {
    const libraw_data_t& d = lr.imgdata;
    Info out;
    out.camera.make = text_of(d.idata.make, sizeof d.idata.make);
    out.camera.model = text_of(d.idata.model, sizeof d.idata.model);
    out.camera.format = format_name(bytes, d);
    if (d.other.iso_speed > 0 && std::isfinite(d.other.iso_speed)) out.camera.iso = d.other.iso_speed;
    if (d.other.shutter > 0 && std::isfinite(d.other.shutter)) out.camera.shutter = d.other.shutter;
    if (d.other.aperture > 0 && std::isfinite(d.other.aperture)) out.camera.aperture = d.other.aperture;
    if (d.other.focal_len > 0 && std::isfinite(d.other.focal_len)) out.camera.focal = d.other.focal_len;
    out.camera.taken = static_cast<std::int64_t>(d.other.timestamp);
    out.raw_width = d.sizes.raw_width;
    out.raw_height = d.sizes.raw_height;
    out.width = d.sizes.width;
    out.height = d.sizes.height;
    out.flip = d.sizes.flip;
    out.colors = d.idata.colors;
    out.mosaic = d.idata.filters != 0;
    const float* cam = d.color.cam_mul;
    if (cam[0] > 0 && cam[1] > 0 && cam[2] > 0 && std::isfinite(cam[0]) && std::isfinite(cam[1]) && std::isfinite(cam[2]))
        out.as_shot = {cam[0] / cam[1], 1.0, cam[2] / cam[1]};
    const float* pre = d.color.pre_mul;
    if (pre[0] > 0 && pre[1] > 0 && pre[2] > 0 && std::isfinite(pre[0]) && std::isfinite(pre[1]) && std::isfinite(pre[2]))
        out.daylight = {pre[0] / pre[1], 1.0, pre[2] / pre[1]};
    bool any = false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            out.xyz_to_camera[r][c] = d.color.cam_xyz[r][c];
            any = any || (std::isfinite(d.color.cam_xyz[r][c]) && d.color.cam_xyz[r][c] != 0);
        }
    if (!any) {  // (a DNG's own: its ColorMatrix of D65's light, else the later calibration's, else the first)
        const auto filled = [&](int i) {
            bool some = false;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    if (!std::isfinite(d.color.dng_color[i].colormatrix[r][c])) return false;
                    some = some || d.color.dng_color[i].colormatrix[r][c] != 0;
                }
            return some;
        };
        int pick = -1;
        for (int i = 0; i < 2; ++i)
            if (filled(i) && d.color.dng_color[i].illuminant == 21) pick = i;
        if (pick < 0) pick = filled(1) ? 1 : filled(0) ? 0 : -1;
        if (pick >= 0) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) out.xyz_to_camera[r][c] = d.color.dng_color[pick].colormatrix[r][c];
            any = true;
        }
    }
    out.matrix = any;
    if (!any)  // (no matrix: the camera's colours taken as sRGB's primaries, as LibRaw's rgb_cam then is)
        out.xyz_to_camera = Mat3{{{3.2404542, -1.5371385, -0.4985314}, {-0.9692660, 1.8760108, 0.0415560}, {0.0556434, -0.2040259, 1.0572252}}};
    out.black = d.color.black;
    out.white = d.color.maximum;
    if (std::isfinite(d.color.dng_levels.baseline_exposure) && std::abs(d.color.dng_levels.baseline_exposure) < 10)
        out.baseline_exposure = d.color.dng_levels.baseline_exposure;
    out.preview = d.thumbnail.tlength > 0 || d.thumbs_list.thumbcount > 0;
    return out;
}

// The development's size, its pixels and what it will hold at once, within the limits — before LibRaw unpacks the
// sensor data (core::Error("image_too_large") past them).
void check_size(const Info& info, bool half, const codecs::Limits& limits, bool adjust) {
    const std::int64_t div = half ? 2 : 1;
    const std::int64_t w = std::max<std::int64_t>(1, info.width / div), h = std::max<std::int64_t>(1, info.height / div);
    if (info.width < 1 || info.height < 1 || info.raw_width < 1 || info.raw_height < 1)
        throw core::Error("format", "the camera RAW file cannot be read (it says no size)");
    limits.check(std::max(w, h), std::max(w, h) == w ? h : w, codecs::Layout::Rgb, codecs::Sample::U16);
    limits.check(info.raw_width, info.raw_height, codecs::Layout::Gray, codecs::Sample::U16);
    // (LibRaw: the sensor data — 4 samples a pixel for a sensor of more colours or none of a mosaic — and its picture of 4
    // samples, its 16-bit RGB; then Genko's: that RGB and — Camera Raw's adjustments — two precise rasters)
    const std::uint64_t raw_px = std::uint64_t(info.raw_width) * std::uint64_t(info.raw_height);
    const std::uint64_t px = std::uint64_t(info.width) * std::uint64_t(info.height) / std::uint64_t(div * div);
    const std::uint64_t sensor = raw_px * (info.mosaic ? 2 : 8);
    const std::uint64_t libraw = sensor + px * 8 + px * 6;
    const std::uint64_t genko = px * 6 + (adjust ? px * 16 : px * 2);
    const std::uint64_t peak = std::max(libraw, genko);
    if (peak > limits.max_alloc) {
        throw core::Error("image_too_large", "the picture is too large to read at once (" + std::to_string(info.width) + "×" + std::to_string(info.height) +
                                                 " pixels; at most " + std::to_string(limits.max_alloc) + " bytes of work)");
    }
}

void open(Run& run, std::string_view bytes, const codecs::Limits& limits) {
    if (bytes.size() > kFileMaxBytes) throw core::Error("too_large", "the file is larger than 256 MiB");
    if (bytes.empty()) throw core::Error("unidentified_image", "the camera RAW file is empty");
    // (LibRaw's own limit on what it allocates for the sensor data: the budget's)
    run.lr->imgdata.rawparams.max_raw_memory_mb = static_cast<unsigned>(std::clamp<std::uint64_t>(limits.max_alloc >> 20, 1, 2048));
    const int code = run.lr->open_buffer(bytes.data(), bytes.size());
    if (code != LIBRAW_SUCCESS) failed(code, "open");
}

// --- colour ----------------------------------------------------------------------------------------------------------------

std::array<double, 3> times(const Mat3& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

std::optional<Mat3> invert(const Mat3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return std::nullopt;
    const double r = 1.0 / det;
    Mat3 o{{{(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r},
            {(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r},
            {(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}}};
    for (const auto& row : o)
        for (const double v : row)
            if (!std::isfinite(v)) return std::nullopt;
    return o;
}

std::array<double, 3> xy_to_xyz(const std::array<double, 2>& xy) {
    const double y = std::max(xy[1], 1e-6);
    return {xy[0] / y, 1.0, (1.0 - xy[0] - xy[1]) / y};
}

// The Planckian locus (Kim et al. 2002's cubic splines, 1667..25000 K; past 25000 K the last one goes on).
std::array<double, 2> planck_xy(double t) {
    t = std::clamp(t, 1667.0, 1e6);
    const double t1 = 1e3 / t, t2 = t1 * t1, t3 = t2 * t1;
    const double x = t <= 4000 ? -0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t1 + 0.179910
                               : -3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t1 + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    const double y = t <= 2222   ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
                     : t <= 4000 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
                                 : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    return {x, y};
}
std::array<double, 2> uv_of(const std::array<double, 2>& xy) {
    const double d = -2 * xy[0] + 12 * xy[1] + 3;
    return {4 * xy[0] / d, 6 * xy[1] / d};
}
std::array<double, 2> xy_of(const std::array<double, 2>& uv) {
    const double d = 2 * uv[0] - 8 * uv[1] + 4;
    return {3 * uv[0] / d, 2 * uv[1] / d};
}
// The locus at t in uv, and its normal there toward green (+v).
std::pair<std::array<double, 2>, std::array<double, 2>> locus_at(double t) {
    const auto at = uv_of(planck_xy(t));
    const auto a = uv_of(planck_xy(t * (1 - 1e-4))), b = uv_of(planck_xy(t * (1 + 1e-4)));
    double du = b[0] - a[0], dv = b[1] - a[1];
    const double len = std::hypot(du, dv);
    if (len > 0) {
        du /= len;
        dv /= len;
    }
    std::array<double, 2> n{-dv, du};
    if (n[1] < 0) n = {dv, -du};
    return {at, n};
}

constexpr double kTintScale = 3000;  // (DNG's: a tint of 1 is 1/3000 of a uv unit)

// --- Little CMS -----------------------------------------------------------------------------------------------------------

struct Profile {
    cmsHPROFILE p = nullptr;
    explicit Profile(cmsHPROFILE profile) : p(profile) {}
    ~Profile() {
        if (p != nullptr) cmsCloseProfile(p);
    }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};
struct Transform {
    cmsHTRANSFORM t = nullptr;
    explicit Transform(cmsHTRANSFORM transform) : t(transform) {}
    ~Transform() {
        if (t != nullptr) cmsDeleteTransform(t);
    }
    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;
};

// Linear ProPhoto RGB (ROMM primaries, D50, a straight curve): what LibRaw develops into.
cmsHPROFILE linear_prophoto() {
    const cmsCIExyYTRIPLE primaries{{0.7347, 0.2653, 1.0}, {0.1596, 0.8404, 1.0}, {0.0366, 0.0001, 1.0}};
    cmsToneCurve* line = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3] = {line, line, line};
    cmsHPROFILE out = cmsCreateRGBProfile(cmsD50_xyY(), &primaries, curves);
    cmsFreeToneCurve(line);
    return out;
}

// A built-in output space (srgb: Little CMS's own; the others' bytes built into the program).
cmsHPROFILE space_profile(std::string_view id) {
    if (id == "srgb") return cmsCreate_sRGBProfile();
    const core::BuiltinProfile* builtin = core::builtin_profile(id);
    if (builtin == nullptr || !builtin->bytes) throw core::Error("value", "the built-in profile " + std::string(id) + " is not in this build");
    return cmsOpenProfileFromMem(builtin->bytes->data(), static_cast<cmsUInt32Number>(builtin->bytes->size()));
}

std::optional<std::string> space_icc(std::string_view id) {
    if (id == "srgb") return std::nullopt;
    const core::BuiltinProfile* builtin = core::builtin_profile(id);
    if (builtin == nullptr || !builtin->bytes) throw core::Error("value", "the built-in profile " + std::string(id) + " is not in this build");
    return *builtin->bytes;
}

// From one space to another in floats (unbounded: values past 1 kept), relative colorimetric.
std::unique_ptr<Transform> float_transform(cmsHPROFILE from, cmsHPROFILE to) {
    auto made = std::make_unique<Transform>(cmsCreateTransform(from, TYPE_RGB_FLT, to, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE));
    if (made->t == nullptr) throw core::Error("value", "Little CMS cannot make the camera RAW's colour transform");
    return made;
}

// --- the RAW stage -----------------------------------------------------------------------------------------------------

int quality_of(std::string_view demosaic) {
    if (demosaic == "linear") return 0;
    if (demosaic == "vng") return 1;
    if (demosaic == "ppg") return 2;
    if (demosaic == "dcb") return 4;
    if (demosaic == "dht") return 11;
    if (demosaic == "aahd") return 12;
    return 3;  // ahd
}
int highlight_of(std::string_view highlights) {
    if (highlights == "unclip") return 1;
    if (highlights == "blend") return 2;
    if (highlights == "rebuild") return 5;
    return 0;
}

struct MemImage {
    libraw_processed_image_t* image = nullptr;
    ~MemImage() {
        if (image != nullptr) LibRaw::dcraw_clear_mem(image);
    }
    MemImage() = default;
    MemImage(const MemImage&) = delete;
    MemImage& operator=(const MemImage&) = delete;
    void reset() {
        if (image != nullptr) LibRaw::dcraw_clear_mem(image);
        image = nullptr;
    }
};

// LibRaw's development: linear ProPhoto of 16 bits (`mem`), what it read, the multipliers, and the gain Genko gives it
// (the exposure, and back what a highlight mode other than clip left below white).
struct Stage {
    MemImage mem;
    Info info;
    std::array<double, 3> multipliers{1, 1, 1};
    double gain = 1;
    std::vector<std::string> notes;
};

void raw_stage(Stage& stage, std::string_view bytes, const core::raw::Develop& develop, const Options& options, bool adjust) {
    Run run(&options);
    open(run, bytes, options.limits);
    stage.info = info_of(bytes, *run.lr);
    check_size(stage.info, options.half_size, options.limits, adjust);
    libraw_output_params_t& p = run.lr->imgdata.params;
    p.output_color = 4;  // (ProPhoto)
    p.output_bps = 16;
    p.gamm[0] = 1.0;
    p.gamm[1] = 1.0;
    p.no_auto_bright = 1;
    p.half_size = options.half_size ? 1 : 0;
    p.user_flip = develop.orientation == "none" ? 0 : -1;
    p.user_qual = quality_of(develop.demosaic);
    p.highlight = highlight_of(develop.highlights);
    p.use_camera_wb = 0;
    p.use_auto_wb = 0;
    if (develop.white_balance == "as_shot") {
        p.use_camera_wb = 1;
        if (stage.info.as_shot[0] <= 0) stage.notes.push_back("ファイルに撮影時のホワイトバランスが無いため、昼光のホワイトバランスで現像しました");
    } else if (develop.white_balance == "auto") {
        p.use_auto_wb = 1;
    } else {
        const auto m = multipliers_for(stage.info, develop.temperature_k, develop.tint);
        p.user_mul[0] = static_cast<float>(m[0]);
        p.user_mul[1] = static_cast<float>(m[1]);
        p.user_mul[2] = static_cast<float>(m[2]);
        p.user_mul[3] = static_cast<float>(m[1]);
    }
    if (!stage.info.matrix && stage.info.colors > 1) stage.notes.push_back("このカメラの色の行列が無いため、カメラの色を sRGB の原色として現像しました");
    run.check_stop();
    int code = run.lr->unpack();
    if (code != LIBRAW_SUCCESS) failed(code, "unpack");
    run.check_stop();
    code = run.lr->dcraw_process();
    if (code != LIBRAW_SUCCESS) failed(code, "process");
    run.check_stop();
    const float* pre = run.lr->imgdata.color.pre_mul;
    if (pre[1] > 0) stage.multipliers = {pre[0] / pre[1], 1.0, pre[2] / pre[1]};
    double least = std::numeric_limits<double>::infinity();
    for (int c = 0; c < 4; ++c)
        if (pre[c] > 1e-6f && std::isfinite(pre[c])) least = std::min<double>(least, pre[c]);
    const double headroom = std::isfinite(least) && least < 1 && least > 1e-6 ? 1.0 / least : 1.0;
    stage.gain = std::exp2(develop.exposure_ev + stage.info.baseline_exposure) * headroom;
    stage.mem.image = run.lr->dcraw_make_mem_image(&code);
    if (stage.mem.image == nullptr) failed(code == 0 ? LIBRAW_UNSUFFICIENT_MEMORY : code, "image");
    if (stage.mem.image->type != LIBRAW_IMAGE_BITMAP || stage.mem.image->bits != 16 || (stage.mem.image->colors != 3 && stage.mem.image->colors != 1))
        throw core::Error("format", "the camera RAW file cannot be read (LibRaw made a picture of " + std::to_string(stage.mem.image->colors) + " colours)");
    if (run.data_errors > 0) throw core::Error("format", "the camera RAW file cannot be read (its data is cut short or broken)");
    run.lr->recycle();  // (its sensor data and picture freed before Genko's work)
}

// The row y of the stage's picture as linear ProPhoto floats with its gain (RGB).
void linear_row(const Stage& stage, int y, float* rgb) {
    const libraw_processed_image_t& m = *stage.mem.image;
    const auto* row = reinterpret_cast<const std::uint16_t*>(m.data) + std::size_t(y) * m.width * m.colors;
    const float k = static_cast<float>(stage.gain / 65535.0);
    for (int x = 0; x < m.width; ++x) {
        if (m.colors == 3) {
            for (int c = 0; c < 3; ++c) rgb[x * 3 + c] = row[x * 3 + c] * k;
        } else {
            const float v = row[x] * k;
            rgb[x * 3] = rgb[x * 3 + 1] = rgb[x * 3 + 2] = v;
        }
    }
}

std::uint16_t to_u16(float v) { return static_cast<std::uint16_t>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 65535.0f)); }

// --- the output sharpening -------------------------------------------------------------------------------------------------

// An unsharp mask of the RGB of 16 bits in place (a Gaussian of `sigma` pixels, `amount`), a band of rows held at a time.
void sharpen(std::string& rgb16, int w, int h, double sigma, double amount, const Options& options) {
    const int r = std::max(1, static_cast<int>(std::ceil(sigma * 3)));
    std::vector<float> kernel(std::size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += kernel[std::size_t(i + r)] = static_cast<float>(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& k : kernel) k = static_cast<float>(k / sum);
    auto* px = reinterpret_cast<std::uint16_t*>(rgb16.data());
    const std::size_t row = std::size_t(w) * 3;
    // (the rows blurred across, kept in a ring of 2r + 1, and the originals of the rows waiting to be written)
    const std::size_t ring = std::size_t(2 * r + 1);
    std::vector<float> across(ring * row), original(ring * row);
    const auto blur_across = [&](int y) {
        float* out = &across[std::size_t(y % static_cast<int>(ring)) * row];
        float* keep = &original[std::size_t(y % static_cast<int>(ring)) * row];
        const std::uint16_t* in = px + std::size_t(y) * row;
        for (std::size_t i = 0; i < row; ++i) keep[i] = in[i] / 65535.0f;
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int i = -r; i <= r; ++i) s += kernel[std::size_t(i + r)] * keep[std::size_t(std::clamp(x + i, 0, w - 1)) * 3 + std::size_t(c)];
                out[std::size_t(x) * 3 + std::size_t(c)] = s;
            }
    };
    for (int y = 0; y < std::min(h, r); ++y) blur_across(y);
    for (int y = 0; y < h; ++y) {
        if (y % 64 == 0 && options.stopped && options.stopped()) throw Cancelled();
        if (y + r < h) blur_across(y + r);
        std::uint16_t* out = px + std::size_t(y) * row;
        const float* self = &original[std::size_t(y % static_cast<int>(ring)) * row];
        for (std::size_t i = 0; i < row; ++i) {
            float s = 0;
            for (int k = -r; k <= r; ++k) {
                const int yy = std::clamp(y + k, 0, h - 1);
                s += kernel[std::size_t(k + r)] * across[std::size_t(yy % static_cast<int>(ring)) * row + i];
            }
            out[i] = to_u16(self[i] + static_cast<float>(amount) * (self[i] - s));
        }
    }
}

std::pair<double, double> sharpening_of(std::string_view sharpen) {
    if (sharpen == "screen") return {0.6, 0.4};
    if (sharpen == "glossy") return {0.8, 0.6};
    if (sharpen == "matte") return {1.0, 0.8};
    return {0, 0};
}

// --- the embedded preview ------------------------------------------------------------------------------------------------

// A DCT JPEG's size (baseline, extended, progressive), none for another (a lossless JPEG's sensor data, not a JPEG).
std::optional<std::pair<std::uint32_t, std::uint32_t>> dct_jpeg_size(std::string_view b) {
    if (b.size() < 4 || u8(b, 0) != 0xFF || u8(b, 1) != 0xD8 || u8(b, 2) != 0xFF) return std::nullopt;
    std::size_t pos = 2;
    for (int i = 0; i < 256; ++i) {
        if (pos + 4 > b.size() || u8(b, pos) != 0xFF) return std::nullopt;
        const unsigned m = u8(b, pos + 1);
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        const std::size_t len = (u8(b, pos + 2) << 8) | u8(b, pos + 3);
        if (m >= 0xC0 && m <= 0xC2) {
            if (pos + 9 > b.size()) return std::nullopt;
            const std::uint32_t h = (u8(b, pos + 5) << 8) | u8(b, pos + 6), w = (u8(b, pos + 7) << 8) | u8(b, pos + 8);
            if (w == 0 || h == 0) return std::nullopt;
            return std::make_pair(w, h);
        }
        if (m == 0xC3 || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF) || m == 0xDA || m == 0xD9 || len < 2)
            return std::nullopt;
        pos += 2 + len;
    }
    return std::nullopt;
}

// The largest DCT JPEG the file's containers point at (TIFF-shaped RAW files' IFDs, a RAF's header), searched as
// PhotoCraft's preview.rs searches: what LibRaw cannot open may still carry the camera's preview.
std::optional<std::string_view> container_preview(std::string_view b) {
    std::optional<std::string_view> best;
    std::uint64_t most = 0;
    const auto candidate = [&](std::uint64_t offset, std::uint64_t length) {
        if (offset >= b.size() || length > b.size() - offset || length < 4) return;
        const std::string_view jpeg = b.substr(offset, length);
        if (const auto size = dct_jpeg_size(jpeg); size && std::uint64_t(size->first) * size->second > most) {
            most = std::uint64_t(size->first) * size->second;
            best = jpeg;
        }
    };
    if (b.starts_with("FUJIFILMCCD-RAW") && b.size() >= 92) {
        const TiffReader be{b, true};
        candidate(be.u32(84), be.u32(88));
        return best;
    }
    if (!tiff_shaped(b) && !(b.size() >= 8 && (b.starts_with("IIRO") || b.starts_with("IIRS") || b.starts_with("MMOR") ||
                                               b.substr(0, 4) == std::string_view("IIU\0", 4))))
        return best;
    const TiffReader t{b, b[0] == 'M'};
    std::vector<std::uint32_t> todo{t.u32(4)};
    std::vector<std::uint32_t> seen;
    while (!todo.empty() && seen.size() < 64) {
        const std::uint32_t at = todo.back();
        todo.pop_back();
        if (at < 8 || at >= b.size() || std::find(seen.begin(), seen.end(), at) != seen.end()) continue;
        seen.push_back(at);
        const std::vector<Entry> entries = ifd_entries(t, at);
        std::optional<std::uint32_t> jif, jif_len, compression;
        std::vector<std::uint32_t> strips, lengths;
        for (const Entry& e : entries) {
            if (e.tag == 0x201) jif = e.value;
            if (e.tag == 0x202) jif_len = e.value;
            if (e.tag == 259) compression = e.value;
            if (e.tag == 273 && e.count == 1) strips = {e.value};
            if (e.tag == 279 && e.count == 1) lengths = {e.value};
            if ((e.tag == 330 || e.tag == 34665) && e.count >= 1) {  // (SubIFDs, EXIF)
                if (e.count == 1) {
                    todo.push_back(e.value);
                } else {
                    for (std::uint32_t k = 0; k < std::min<std::uint32_t>(e.count, 16); ++k) todo.push_back(t.u32(std::size_t(e.value) + std::size_t(k) * 4));
                }
            }
            if (e.tag == 0x002E && e.type == 7) candidate(e.value, e.count);  // (a Panasonic RW2's JpgFromRaw)
        }
        if (jif && jif_len) candidate(*jif, *jif_len);
        if (compression && (*compression == 6 || *compression == 7) && strips.size() == 1 && lengths.size() == 1) candidate(strips[0], lengths[0]);
        if (const std::uint32_t next = next_ifd(t, at); next != 0) todo.push_back(next);
    }
    return best;
}

}  // namespace

// --- what the file is -------------------------------------------------------------------------------------------------------

std::string libraw_version() {
    // (the LibRaw that runs — one put in its place too —, "0.22.2" of its "0.22.2-Release")
    std::string version = LibRaw::version();
    if (version.ends_with("-Release")) version.resize(version.size() - std::string_view("-Release").size());
    return version;
}

bool raw_suffix(std::string_view suffix) {
    std::string lower(suffix);
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    const auto later = core::smart::later_format_of_suffix(lower);
    return later && *later == "raw";
}

bool looks_raw(std::string_view b) {
    if (b.size() < 16) return false;
    if (b.substr(4, 4) == "ftyp" && b.substr(8, 4) == "crx ") return true;  // (CR3)
    if (b.starts_with("FUJIFILMCCD-RAW") || b.starts_with("FOVb") || b.substr(0, 4) == std::string_view("\0MRM", 4) || b.substr(6, 8) == "HEAPCCDR")
        return true;
    if (b.starts_with("IIRO") || b.starts_with("IIRS") || b.starts_with("MMOR") || b.substr(0, 4) == std::string_view("IIU\0", 4)) return true;
    if (!tiff_shaped(b)) return false;
    if (b.substr(8, 2) == "CR") return true;  // (CR2)
    const TiffReader t{b, b[0] == 'M'};
    for (const Entry& e : ifd_entries(t, t.u32(4)))
        if (e.tag == 50706) return true;  // (DNGVersion)
    return false;
}

bool is_raw(std::string_view bytes, std::string_view suffix) {
    if (!raw_suffix(suffix) && !looks_raw(bytes)) return false;
    try {
        Options none;
        Run run(&none);
        open(run, bytes, codecs::Limits{});
        return true;
    } catch (const core::Error&) {
        return false;
    }
}

Info identify(std::string_view bytes, const codecs::Limits& limits) {
    Options options;
    options.limits = limits;
    Run run(&options);
    open(run, bytes, limits);
    Info info = info_of(bytes, *run.lr);
    check_size(info, false, limits, false);
    return info;
}

Info unpacked(std::string_view bytes, const codecs::Limits& limits) {
    Options options;
    options.limits = limits;
    Run run(&options);
    open(run, bytes, limits);
    Info info = info_of(bytes, *run.lr);
    check_size(info, false, limits, false);
    const int code = run.lr->unpack();
    if (code != LIBRAW_SUCCESS) failed(code, "unpack");
    if (run.data_errors > 0) throw core::Error("format", "the camera RAW file cannot be read (its data is cut short or broken)");
    return info;
}

std::string suffix_of(const Info& info) {
    const std::string& f = info.camera.format;
    if (f == "DNG") return ".dng";
    if (f == "CR2") return ".cr2";
    if (f == "CR3") return ".cr3";
    if (f == "RAF") return ".raf";
    if (f == "ORF") return ".orf";
    if (f == "RW2") return ".rw2";
    if (f == "MRW") return ".mrw";
    if (f == "X3F") return ".x3f";
    std::string make = info.camera.make;
    for (char& c : make) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    static constexpr std::pair<std::string_view, std::string_view> kMakers[] = {
        {"nikon", ".nef"},  {"sony", ".arw"},    {"pentax", ".pef"},  {"ricoh", ".pef"},  {"samsung", ".srw"}, {"kodak", ".dcr"},
        {"minolta", ".mrw"}, {"leaf", ".mos"},  {"mamiya", ".mos"},  {"phase one", ".iiq"}, {"hasselblad", ".3fr"}, {"epson", ".erf"},
        {"canon", ".cr2"},  {"fujifilm", ".raf"}, {"olympus", ".orf"}, {"panasonic", ".rw2"}, {"sigma", ".x3f"}};
    for (const auto& [maker, suffix] : kMakers)
        if (make.starts_with(maker)) return std::string(suffix);
    return ".dng";
}

std::optional<std::string> space_bytes(std::string_view space) { return space_icc(space); }

std::pair<std::int64_t, std::int64_t> developed_size(const Info& info, const core::raw::Develop& develop) {
    const bool turned = develop.orientation != "none" && (info.flip & 4) != 0;
    return turned ? std::make_pair(info.height, info.width) : std::make_pair(info.width, info.height);
}

// --- the white balance -------------------------------------------------------------------------------------------------

std::array<double, 2> white_xy(double temperature_k, double tint) {
    const auto [at, n] = locus_at(std::clamp(temperature_k, core::raw::kTemperatureMin, core::raw::kTemperatureMax));
    const double offset = tint / kTintScale;
    return xy_of({at[0] + n[0] * offset, at[1] + n[1] * offset});
}

std::pair<double, double> temperature_tint(const std::array<double, 2>& xy) {
    const auto uv = uv_of(xy);
    const auto distance = [&](double mired) {
        const auto at = uv_of(planck_xy(1e6 / mired));
        return std::hypot(uv[0] - at[0], uv[1] - at[1]);
    };
    // (the nearest point of the locus between 2000 and 50000 K: by mired, a coarse look then a golden section)
    const double lo = 1e6 / core::raw::kTemperatureMax, hi = 1e6 / core::raw::kTemperatureMin;
    double best = lo, least = distance(lo);
    for (int i = 1; i <= 480; ++i) {
        const double m = lo + (hi - lo) * i / 480;
        if (const double d = distance(m); d < least) {
            least = d;
            best = m;
        }
    }
    const double step = (hi - lo) / 480;
    double a = std::max(lo, best - step), b = std::min(hi, best + step);
    const double g = (std::sqrt(5.0) - 1) / 2;
    for (int i = 0; i < 80; ++i) {
        const double c = b - g * (b - a), d = a + g * (b - a);
        if (distance(c) < distance(d)) {
            b = d;
        } else {
            a = c;
        }
    }
    const double t = 1e6 / ((a + b) / 2);
    const auto [at, n] = locus_at(t);
    const double tint = ((uv[0] - at[0]) * n[0] + (uv[1] - at[1]) * n[1]) * kTintScale;
    return {t, tint};
}

std::array<double, 3> multipliers_for(const Info& info, double temperature_k, double tint) {
    const auto cam = times(info.xyz_to_camera, xy_to_xyz(white_xy(temperature_k, tint)));
    if (!(cam[0] > 1e-9 && cam[1] > 1e-9 && cam[2] > 1e-9) || !std::isfinite(cam[0] + cam[1] + cam[2])) return info.daylight;
    return {cam[1] / cam[0], 1.0, cam[1] / cam[2]};
}

std::pair<double, double> temperature_of(const Info& info, const std::array<double, 3>& m) {
    const auto inverse = invert(info.xyz_to_camera);
    if (!inverse || !(m[0] > 0 && m[1] > 0 && m[2] > 0)) return {5500, 0};
    const auto xyz = times(*inverse, {1.0 / m[0], 1.0 / m[1], 1.0 / m[2]});
    const double s = xyz[0] + xyz[1] + xyz[2];
    if (!std::isfinite(s) || s <= 1e-9) return {5500, 0};
    return temperature_tint({xyz[0] / s, xyz[1] / s});
}

// --- development -------------------------------------------------------------------------------------------------------------

Developed develop(std::string_view bytes, const Json& raw, const Options& options) {
    const Json normal = core::raw::normal_raw(raw.is_null() ? Json::object() : raw);
    const core::raw::Develop d = core::raw::develop_from(core::get(normal, "develop"));
    const Json* adjust = core::get(normal, "adjust");
    Stage stage;
    raw_stage(stage, bytes, d, options, adjust != nullptr);
    const int w = stage.mem.image->width, h = stage.mem.image->height;
    // (linear ProPhoto → the output space, or — Camera Raw's adjustments — prophoto-compat first)
    const Profile linear(linear_prophoto());
    const Profile working(space_profile("prophoto-compat"));
    const Profile output(space_profile(d.space));
    const auto first = float_transform(linear.p, adjust != nullptr ? working.p : output.p);
    std::string rgb16(std::size_t(w) * std::size_t(h) * 6, '\0');
    {
        std::vector<float> row(std::size_t(w) * 3);
        auto* out = reinterpret_cast<std::uint16_t*>(rgb16.data());
        for (int y = 0; y < h; ++y) {
            if (y % 64 == 0 && options.stopped && options.stopped()) throw Cancelled();
            linear_row(stage, y, row.data());
            cmsDoTransform(first->t, row.data(), row.data(), static_cast<cmsUInt32Number>(w));
            for (std::size_t i = 0; i < row.size(); ++i) out[std::size_t(y) * row.size() + i] = to_u16(row[i]);
        }
    }
    stage.mem.reset();
    if (adjust != nullptr) {
        // (Camera Raw's adjustments: 節 D's kernel over the picture in prophoto-compat — its lengths in the file's pixels —,
        // then into the output space)
        const fx::Kernel* kernel = fx::kernel("camera_raw");
        if (kernel == nullptr) throw core::Error("value", "the camera_raw kernel is not in this build");
        std::string worked;
        {
            const auto* in = reinterpret_cast<const std::uint16_t*>(rgb16.data());
            const std::string raster = core::encode_color_pixels(std::uint32_t(w), std::uint32_t(h), "u16", [in](std::size_t i) {
                return std::array<double, 4>{in[i * 3] / 65535.0, in[i * 3 + 1] / 65535.0, in[i * 3 + 2] / 65535.0, 1.0};
            });
            std::string().swap(rgb16);
            const ImageAllocationBudget budget{options.limits.max_alloc};
            const auto surface = fx::precise_surface(raster);
            fx::apply(*surface, *kernel, *adjust, core::FilterInks::Meaning, fx::Rect{0, 0, w, h}, nullptr, 1.0);
            worked = fx::take_bytes(*surface);
        }
        const core::ColorRasterView view(worked);
        const auto second = float_transform(working.p, output.p);
        rgb16.assign(std::size_t(w) * std::size_t(h) * 6, '\0');
        auto* out = reinterpret_cast<std::uint16_t*>(rgb16.data());
        std::vector<float> row(std::size_t(w) * 3);
        for (int y = 0; y < h; ++y) {
            if (y % 64 == 0 && options.stopped && options.stopped()) throw Cancelled();
            for (int x = 0; x < w; ++x) {
                const auto p = view.pixel(std::size_t(y) * std::size_t(w) + std::size_t(x));
                for (int c = 0; c < 3; ++c) row[std::size_t(x) * 3 + std::size_t(c)] = static_cast<float>(p[std::size_t(c)]);
            }
            cmsDoTransform(second->t, row.data(), row.data(), static_cast<cmsUInt32Number>(w));
            for (std::size_t i = 0; i < row.size(); ++i) out[std::size_t(y) * row.size() + i] = to_u16(row[i]);
        }
    }
    if (const auto [sigma, amount] = sharpening_of(d.sharpen); amount > 0) sharpen(rgb16, w, h, sigma, amount, options);
    Developed out;
    out.info = stage.info;
    out.multipliers = stage.multipliers;
    out.notes = std::move(stage.notes);
    if (d.depth == 8) {
        std::string rgb8(std::size_t(w) * std::size_t(h) * 3, '\0');
        const auto* in = reinterpret_cast<const std::uint16_t*>(rgb16.data());
        for (std::size_t i = 0; i < rgb8.size(); ++i) rgb8[i] = static_cast<char>((in[i] * 255u + 32767u) / 65535u);
        std::string().swap(rgb16);
        out.picture = codecs::CodecImage::from_bytes(std::uint32_t(w), std::uint32_t(h), codecs::Layout::Rgb, codecs::Sample::U8, std::move(rgb8));
    } else {
        out.picture = codecs::CodecImage::from_bytes(std::uint32_t(w), std::uint32_t(h), codecs::Layout::Rgb, codecs::Sample::U16, std::move(rgb16));
    }
    out.picture.icc = space_icc(d.space);
    return out;
}

Preview preview(std::string_view bytes, const core::raw::Develop& develop, int max_side, const Options& options) {
    Options half = options;
    half.half_size = true;
    Stage stage;
    raw_stage(stage, bytes, develop, half, false);
    const int w = stage.mem.image->width, h = stage.mem.image->height;
    const double scale = max_side > 0 ? std::min(1.0, double(max_side) / std::max(w, h)) : 1.0;
    Preview out;
    out.width = std::max(1, static_cast<int>(std::lround(w * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(h * scale)));
    out.info = stage.info;
    out.multipliers = stage.multipliers;
    out.notes = stage.notes;
    // (each pixel of the preview the mean of the developed pixels it covers, in linear light)
    std::vector<double> sums(std::size_t(out.width) * std::size_t(out.height) * 3, 0.0);
    std::vector<double> counts(std::size_t(out.width) * std::size_t(out.height), 0.0);
    std::vector<float> row(std::size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        if (y % 64 == 0 && options.stopped && options.stopped()) throw Cancelled();
        linear_row(stage, y, row.data());
        const int py = std::min(out.height - 1, static_cast<int>(y * double(out.height) / h));
        for (int x = 0; x < w; ++x) {
            const int px = std::min(out.width - 1, static_cast<int>(x * double(out.width) / w));
            const std::size_t at = std::size_t(py) * std::size_t(out.width) + std::size_t(px);
            for (int c = 0; c < 3; ++c) sums[at * 3 + std::size_t(c)] += row[std::size_t(x) * 3 + std::size_t(c)];
            counts[at] += 1;
        }
    }
    stage.mem.reset();
    std::vector<float> rgb(sums.size());
    for (std::size_t i = 0; i < counts.size(); ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + std::size_t(c)] = counts[i] > 0 ? static_cast<float>(sums[i * 3 + std::size_t(c)] / counts[i]) : 0.0f;
    const Profile linear(linear_prophoto());
    const Profile working(space_profile("prophoto-compat"));
    const auto to_working = float_transform(linear.p, working.p);
    cmsDoTransform(to_working->t, rgb.data(), rgb.data(), static_cast<cmsUInt32Number>(counts.size()));
    out.rgba.resize(counts.size() * 4);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        for (int c = 0; c < 3; ++c) out.rgba[i * 4 + std::size_t(c)] = std::isfinite(rgb[i * 3 + std::size_t(c)]) ? std::max(0.0f, rgb[i * 3 + std::size_t(c)]) : 0.0f;
        out.rgba[i * 4 + 3] = 1.0f;
    }
    return out;
}

void working_to_screen(float* rgba, std::size_t pixels) {
    static std::once_flag once;
    static cmsHTRANSFORM shown = nullptr;
    std::call_once(once, [] {
        const Profile working(space_profile("prophoto-compat"));
        const Profile srgb(cmsCreate_sRGBProfile());
        shown = cmsCreateTransform(working.p, TYPE_RGBA_FLT, srgb.p, TYPE_RGBA_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
    });
    if (shown == nullptr || rgba == nullptr || pixels == 0) return;
    cmsDoTransform(shown, rgba, rgba, static_cast<cmsUInt32Number>(std::min<std::size_t>(pixels, std::numeric_limits<cmsUInt32Number>::max())));
}

double auto_exposure(const Preview& preview) {
    // (prophoto-compat's curve undone — gamma 1.8 — and ROMM's luminance)
    double logs = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < preview.rgba.size(); i += 4) {
        if (!(preview.rgba[i + 3] > 0)) continue;
        const auto lin = [&](std::size_t c) { return std::pow(std::max(0.0f, preview.rgba[i + c]), 1.8f); };
        const double y = 0.2880402 * lin(0) + 0.7118741 * lin(1) + 0.0000857 * lin(2);
        if (!(y > 1e-4) || !std::isfinite(y)) continue;
        logs += std::log2(y);
        ++n;
    }
    if (n == 0) return 0;
    const double ev = std::log2(0.18) - logs / static_cast<double>(n);
    return std::round(std::clamp(ev, -3.0, 3.0) * 20) / 20;
}

// --- the embedded preview, the defaults, the drawing's file ---------------------------------------------------------------

std::optional<codecs::CodecImage> embedded_preview(std::string_view bytes, const codecs::Limits& limits) {
    if (bytes.size() > kFileMaxBytes) throw core::Error("too_large", "the file is larger than 256 MiB");
    // (LibRaw's, where it opens the file: its JPEG, or its bitmap)
    try {
        Options options;
        options.limits = limits;
        Run run(&options);
        open(run, bytes, limits);
        if (run.lr->unpack_thumb() == LIBRAW_SUCCESS) {
            const libraw_thumbnail_t& t = run.lr->imgdata.thumbnail;
            if (t.tformat == LIBRAW_THUMBNAIL_JPEG && t.thumb != nullptr && t.tlength > 0) {
                const std::string_view jpeg(t.thumb, t.tlength);
                if (dct_jpeg_size(jpeg)) return codecs::decode_as(codecs::Format::Jpeg, jpeg, limits);
            }
            if (t.tformat == LIBRAW_THUMBNAIL_BITMAP && t.thumb != nullptr && t.tcolors == 3 && t.twidth > 0 && t.theight > 0 &&
                std::uint64_t(t.tlength) >= std::uint64_t(t.twidth) * t.theight * 3) {
                limits.check(t.twidth, t.theight, codecs::Layout::Rgb, codecs::Sample::U8);
                return codecs::CodecImage::from_bytes(t.twidth, t.theight, codecs::Layout::Rgb, codecs::Sample::U8,
                                                      std::string(t.thumb, std::size_t(t.twidth) * t.theight * 3));
            }
        }
    } catch (const core::Error& error) {
        if (error.code() == "image_too_large" || error.code() == "too_large") throw;
    }
    // (else the largest JPEG its containers point at)
    if (const auto jpeg = container_preview(bytes)) {
        try {
            return codecs::decode_as(codecs::Format::Jpeg, *jpeg, limits);
        } catch (const core::Error& error) {
            if (error.code() == "image_too_large") throw;
        }
    }
    return std::nullopt;
}

Json default_raw(std::string_view bytes) {
    const Info info = identify(bytes);
    Json camera = Json::object();
    camera["make"] = info.camera.make.substr(0, core::raw::kCameraTextMax);
    camera["model"] = info.camera.model.substr(0, core::raw::kCameraTextMax);
    camera["format"] = info.camera.format;
    camera["libraw"] = libraw_version();
    return core::raw::normal_raw(Json{{"develop", Json::object()}, {"camera", camera}});
}

std::string developed_file(std::string_view bytes, const Json& raw, const Options& options) {
    const Developed made = develop(bytes, raw, options);
    codecs::EncodeOptions how;
    how.tiff_compression = codecs::TiffCompression::None;
    return codecs::encode(made.picture, codecs::Format::Tiff, how);
}

}  // namespace genko::render::raw
