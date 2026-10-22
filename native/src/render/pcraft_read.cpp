// A PhotoCraft document read as a Genko page (render/pcraft_read.hpp).

#include "render/pcraft_read.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include "core/adjust.hpp"
#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/filter_kinds.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "core/masks.hpp"
#include "core/patterns.hpp"
#include "core/psd_keep.hpp"
#include "core/smart.hpp"
#include "core/vector.hpp"
#include "render/blake3.hpp"
#include "render/codecs/limits.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/psd_adjust.hpp"
#include "render/smart_decode.hpp"
#include "render/zip.hpp"
#include "render/zstd.hpp"

namespace genko::render::pcraft {

namespace {

using core::Json;

[[noreturn]] void broken(const std::string& why) { throw core::Error("format", "the .pcraft is broken (" + why + ")"); }

// --- where the bytes are: a zip, or a folder -------------------------------------------------------------------------

class Source {
public:
    virtual ~Source() = default;
    // The file `name` of the bundle, at most `max` bytes (core::Error("image_too_large") past it, "format" when missing).
    virtual std::string get(const std::string& name, std::uint64_t max) const = 0;
};

class ZipSource final : public Source {
public:
    explicit ZipSource(std::string_view bytes) : zip_(bytes) {}
    std::string get(const std::string& name, std::uint64_t max) const override { return zip_.read(name, max); }

private:
    zip::NamedReader zip_;
};

// A .pcraft folder: only the names a bundle has (manifest.json, tiles/<hash>.zst, blobs/<hash>.zst) are ever read — a name
// from the manifest never reaches outside the folder.
class FolderSource final : public Source {
public:
    explicit FolderSource(std::filesystem::path root) : root_(std::move(root)) {}
    std::string get(const std::string& name, std::uint64_t max) const override {
        const bool object = (name.starts_with("tiles/") || name.starts_with("blobs/")) && name.size() == 6 + 64 + 4 && name.ends_with(".zst") &&
                            blake3::is_hex_hash(std::string_view(name).substr(6, 64));
        if (name != kManifest && !object) broken("a name that is not a bundle's (" + name + ")");
        const std::filesystem::path path = root_ / std::filesystem::path(name);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) broken("no " + name);
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) broken("no " + name);
        if (size > max) {
            throw core::Error("image_too_large", "the .pcraft holds a file too large to read (" + name + ": " + std::to_string(size) +
                                                     " bytes; at most " + std::to_string(max) + ")");
        }
        std::ifstream in(path, std::ios::binary);
        std::string out(static_cast<std::size_t>(size), '\0');
        if (!in.read(out.data(), static_cast<std::streamsize>(out.size()))) broken("cannot read " + name);
        return out;
    }

private:
    std::filesystem::path root_;
};

// --- the bundle's objects: each checked against its hash, all of them against the budget ----------------------------

class Loader {
public:
    Loader(const Source& source, const ReadLimits& limits) : source_(source), limits_(limits) {}

    // n more bytes unpacked: refused past the budget before they are made.
    void account(std::uint64_t n) {
        if (n > limits_.max_unpacked_bytes || unpacked_ > limits_.max_unpacked_bytes - n) {
            throw core::Error("image_too_large", "the .pcraft unpacks to more than this build reads at once (at most " +
                                                     std::to_string(limits_.max_unpacked_bytes) + " bytes)");
        }
        unpacked_ += n;
    }

    std::string tile(const std::string& hash, std::size_t length) {
        if (!blake3::is_hex_hash(hash)) broken("a tile named by something that is not a hash");
        account(length);
        const std::string name = "tiles/" + hash + ".zst";
        const std::string packed = source_.get(name, length + length / 8 + 4096);
        std::string data;
        try {
            data = zstd::decompress_exact(packed, length);
        } catch (const core::Error& error) {
            if (error.code() != "format") throw;
            broken(name + ": " + error.what());
        }
        if (blake3::hex(data) != hash) broken(name + ": its content does not match its hash");
        return data;
    }

    core::Bytes blob(const std::string& hash) {
        if (const auto known = blobs_.find(hash); known != blobs_.end()) return known->second;
        if (!blake3::is_hex_hash(hash)) broken("a blob named by something that is not a hash");
        const std::string name = "blobs/" + hash + ".zst";
        const std::string packed = source_.get(name, limits_.max_blob_bytes + limits_.max_blob_bytes / 8 + 4096);
        const std::uint64_t room = limits_.max_unpacked_bytes - unpacked_;
        std::string data;
        try {
            data = zstd::decompress_bounded(packed, static_cast<std::size_t>(std::min<std::uint64_t>(limits_.max_blob_bytes, room)));
        } catch (const core::Error& error) {
            if (error.code() != "format") throw;
            broken(name + ": " + error.what());
        }
        account(data.size());
        if (blake3::hex(data) != hash) broken(name + ": its content does not match its hash");
        auto bytes = std::make_shared<const std::string>(std::move(data));
        blobs_.emplace(hash, bytes);
        return bytes;
    }

    const ReadLimits& limits() const { return limits_; }
    std::uint64_t unpacked() const { return unpacked_; }
    std::uint64_t remaining() const { return limits_.max_unpacked_bytes - unpacked_; }

private:
    const Source& source_;
    ReadLimits limits_;
    std::uint64_t unpacked_ = 0;
    std::map<std::string, core::Bytes> blobs_;
};

Json parse_manifest(const Source& source, const ReadLimits& limits) {
    const std::string raw = source.get(std::string(kManifest), limits.max_manifest_bytes);
    Json m;
    try {
        core::ParseOptions options;
        options.bytes = true;
        options.max_depth = 512;
        m = core::parse_python_json(raw, nullptr, options);
    } catch (const core::Error& error) {
        broken(std::string("manifest.json is not JSON: ") + error.what());
    }
    if (!m.is_object()) broken("manifest.json is not an object");
    const auto version = m.find("format_version");
    if (version == m.end() || !version->is_number_integer() || version->get<std::int64_t>() < 1) broken("manifest.json has no format_version");
    const std::int64_t found = version->get<std::int64_t>();
    if (found > static_cast<std::int64_t>(kFormatVersion)) {
        throw core::Error("unsupported_version", "the .pcraft was written by a newer PhotoCraft (format " + std::to_string(found) +
                                                     "; this build reads format " + std::to_string(kFormatVersion) + ")");
    }
    return m;
}

// --- notes: what could not be kept, each said once ------------------------------------------------------------------

class Notes {
public:
    void add(std::string text) {
        if (seen_.insert(text).second) list_.push_back(std::move(text));
    }
    // "<what>（「a」「b」…）" for the names met, said once at the end.
    void name(const std::string& what, const std::string& layer) {
        auto& names = named_[what];
        if (std::find(names.begin(), names.end(), layer) == names.end()) names.push_back(layer);
    }
    std::vector<std::string> take() {
        for (const auto& [what, names] : named_) {
            std::string list;
            for (std::size_t i = 0; i < names.size() && i < 8; ++i) list += "「" + names[i] + "」";
            if (names.size() > 8) list += " ほか " + std::to_string(names.size() - 8) + " 枚";
            add(what + "（" + list + "）");
        }
        named_.clear();
        return std::move(list_);
    }

private:
    std::vector<std::string> list_;
    std::set<std::string> seen_;
    std::map<std::string, std::vector<std::string>> named_;
};

// --- surfaces over the canvas ----------------------------------------------------------------------------------------

// A surface's tiles that meet the canvas [0, w) × [0, h), unpacked (the others, wholly outside, are never read).
class Sampled {
public:
    Sampled(Loader& load, const Surface& surface, int w, int h) : format_(surface.format), w_(w), h_(h) {
        background_.assign(format_.pixel_bytes(), '\0');
        const unsigned sb = sample_bytes(format_.sample);
        for (unsigned c = 0; c < format_.channels() && c < surface.background.size(); ++c) {
            write_sample(reinterpret_cast<unsigned char*>(background_.data()) + c * sb, format_.sample, surface.background[c]);
        }
        const std::size_t length = format_.tile_bytes();
        for (const TileRef& t : surface.tiles) {
            const std::int64_t x0 = std::int64_t(t.tx) * kTileSize, y0 = std::int64_t(t.ty) * kTileSize;
            if (x0 >= w || y0 >= h || x0 + kTileSize <= 0 || y0 + kTileSize <= 0) {
                cut_ = true;  // (a tile wholly outside the canvas: its pixels are not the page's)
                continue;
            }
            if (x0 < 0 || y0 < 0 || x0 + kTileSize > w || y0 + kTileSize > h) edge_.push_back({t.tx, t.ty});
            tiles_.emplace(std::make_pair(t.tx, t.ty), load.tile(t.hash, length));
        }
        // (a tile across the canvas's edge: cut when a pixel of it outside is not the untouched one)
        const std::size_t pb = format_.pixel_bytes();
        for (const auto& [tx, ty] : edge_) {
            if (cut_) break;
            const std::string& bytes = tiles_.at({tx, ty});
            for (int y = 0; y < kTileSize && !cut_; ++y) {
                for (int x = 0; x < kTileSize; ++x) {
                    const std::int64_t px = std::int64_t(tx) * kTileSize + x, py = std::int64_t(ty) * kTileSize + y;
                    if (px >= 0 && py >= 0 && px < w && py < h) continue;
                    if (std::string_view(bytes).substr((std::size_t(y) * kTileSize + x) * pb, pb) != background_) {
                        cut_ = true;
                        break;
                    }
                }
            }
        }
    }

    const PixelFormat& format() const { return format_; }
    bool cut() const { return cut_; }
    // Each meeting tile's part on the canvas, row by row: f(x0, y, count, bytes of its first pixel) for a run of `count`
    // pixels; and the background for the canvas's pixels no tile covers (`fill`).
    template <class Run>
    void each_row(Run&& run) const {
        const std::size_t pb = format_.pixel_bytes();
        for (int y = 0; y < h_; ++y) {
            const int ty = floor_div(y, kTileSize);
            int x = 0;
            while (x < w_) {
                const int tx = floor_div(x, kTileSize);
                const int end = std::min(w_, (tx + 1) * kTileSize);
                const auto it = tiles_.find({tx, ty});
                if (it == tiles_.end()) {
                    run(x, y, end - x, nullptr);
                } else {
                    const int lx = x - tx * kTileSize, ly = y - ty * kTileSize;
                    run(x, y, end - x, reinterpret_cast<const unsigned char*>(it->second.data()) + (std::size_t(ly) * kTileSize + lx) * pb);
                }
                x = end;
            }
        }
    }
    const unsigned char* background() const { return reinterpret_cast<const unsigned char*>(background_.data()); }
    double sample(const unsigned char* pixel, unsigned c) const { return read_sample(pixel + c * sample_bytes(format_.sample), format_.sample); }

private:
    static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    PixelFormat format_;
    int w_, h_;
    std::string background_;
    std::map<std::pair<int, int>, std::string> tiles_;
    std::vector<std::pair<int, int>> edge_;
    bool cut_ = false;
};

// The page being made.
enum class Way { Rgb8, Precise, Inks };

struct Ctx {
    Loader& load;
    Notes notes;
    core::Document doc;
    int w = 1, h = 1;
    double dpi = 72;
    double mm = 25.4 / 72;  // mm a pixel
    Mode mode = Mode::Rgb;
    Sample sample = Sample::U8;
    Way way = Way::Rgb8;
    std::optional<core::PageColor> inks;
    std::map<std::string, std::string> patterns;  // (upstream's id, and "name:" its name → the book's pattern id)
    std::string title;
    int depth = 0;
    std::vector<std::string> cut;  // (layers whose pixels beyond the canvas were cut)

    core::Page& page() { return doc.edit_page(0); }
};

// The colour channels the page's pixels take from a surface (its colours as the page holds them, then its alpha).
unsigned page_colours(const Ctx& ctx) {
    if (ctx.way != Way::Inks) return 3;
    return core::colour_channels(ctx.inks->space);
}

// Can a surface of this format be the page's pixels (the same colours: an RGB page's from RGB or grey, a grey page's from
// grey, a CMYK page's from CMYK, a Lab page's from Lab)?
bool fits(const Ctx& ctx, const PixelFormat& f) {
    const unsigned colours = colour_channels(f.mode);
    if (ctx.way != Way::Inks) return colours == 3 ? (f.mode != Mode::Cmyk && f.mode != Mode::Lab) : colours == 1;
    switch (ctx.inks->space) {
    case core::InkSpace::Gray:
        return colours == 1;
    case core::InkSpace::Cmyk:
        return f.mode == Mode::Cmyk;
    case core::InkSpace::Lab:
        return f.mode == Mode::Lab;
    default:
        return false;
    }
}

// A pixel of the surface as the page's colours and alpha (out: page_colours() + 1 values, normalized).
void page_pixel(const Ctx& ctx, const Sampled& s, const unsigned char* px, double* out) {
    const PixelFormat& f = s.format();
    const unsigned colours = colour_channels(f.mode), wanted = page_colours(ctx);
    for (unsigned c = 0; c < wanted; ++c) out[c] = s.sample(px, colours == 1 ? 0 : c);
    out[wanted] = f.alpha ? s.sample(px, colours) : 1.0;
}

// The surface as a layer's pixels on the page (its picture, precise raster or inks).
void put_pixels(Ctx& ctx, core::Layer& layer, const Surface& surface, const std::string& name) {
    if (!fits(ctx, surface.format)) throw Unreadable("pixels of another colour mode than the page's");
    const Sampled s(ctx.load, surface, ctx.w, ctx.h);
    if (s.cut()) ctx.notes.name("用紙の外にあった画素を切りました", name);
    const unsigned colours = page_colours(ctx);
    std::array<double, 5> v{};
    if (ctx.way == Way::Rgb8) {
        std::string rgba(std::size_t(ctx.w) * ctx.h * 4, '\0');
        s.each_row([&](int x, int y, int n, const unsigned char* px) {
            for (int i = 0; i < n; ++i) {
                page_pixel(ctx, s, px != nullptr ? px + std::size_t(i) * s.format().pixel_bytes() : s.background(), v.data());
                char* out = rgba.data() + (std::size_t(y) * ctx.w + x + i) * 4;
                for (unsigned c = 0; c < 4; ++c) out[c] = static_cast<char>(std::lround(std::clamp(v[c], 0.0, 1.0) * 255.0));
            }
        });
        const Image image = Image::frombytes("RGBA", Size{ctx.w, ctx.h}, rgba);
        layer.raster_png = std::make_shared<const std::string>(write_png(image));
    } else if (ctx.way == Way::Precise) {
        std::vector<float> rgba(std::size_t(ctx.w) * ctx.h * 4);
        s.each_row([&](int x, int y, int n, const unsigned char* px) {
            for (int i = 0; i < n; ++i) {
                page_pixel(ctx, s, px != nullptr ? px + std::size_t(i) * s.format().pixel_bytes() : s.background(), v.data());
                float* out = rgba.data() + (std::size_t(y) * ctx.w + x + i) * 4;
                for (unsigned c = 0; c < 3; ++c) out[c] = static_cast<float>(std::isfinite(v[c]) ? v[c] : 0.0);
                out[3] = static_cast<float>(std::clamp(v[3], 0.0, 1.0));
            }
        });
        const int depth = ctx.sample == Sample::F32 ? 32 : 16;
        layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
            static_cast<std::uint32_t>(ctx.w), static_cast<std::uint32_t>(ctx.h), depth == 16 ? "u16" : "f32", [&](std::size_t i) {
                const float* p = rgba.data() + i * 4;
                return std::array<double, 4>{p[0], p[1], p[2], p[3]};
            }));
    } else {
        core::InkPixels inks = core::blank_ink_pixels(ctx.inks->space, ctx.inks->depth, static_cast<std::uint32_t>(ctx.w), static_cast<std::uint32_t>(ctx.h));
        s.each_row([&](int x, int y, int n, const unsigned char* px) {
            for (int i = 0; i < n; ++i) {
                page_pixel(ctx, s, px != nullptr ? px + std::size_t(i) * s.format().pixel_bytes() : s.background(), v.data());
                const std::size_t at = std::size_t(y) * ctx.w + x + i;
                for (unsigned c = 0; c <= colours; ++c) inks.set(at, c, v[c]);
            }
        });
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(inks));
    }
}

// A grey surface (a mask, a channel) over the canvas: its first channel, 0..1.
std::vector<float> grey_plane(Ctx& ctx, const Surface& surface, bool& cut) {
    const Sampled s(ctx.load, surface, ctx.w, ctx.h);
    cut = s.cut();
    std::vector<float> out(std::size_t(ctx.w) * ctx.h);
    s.each_row([&](int x, int y, int n, const unsigned char* px) {
        for (int i = 0; i < n; ++i) {
            const unsigned char* p = px != nullptr ? px + std::size_t(i) * s.format().pixel_bytes() : s.background();
            out[std::size_t(y) * ctx.w + x + i] = static_cast<float>(std::clamp(s.sample(p, 0), 0.0, 1.0));
        }
    });
    return out;
}

core::Bytes grey_png(const Ctx& ctx, const std::vector<float>& plane) {
    std::string bytes(plane.size(), '\0');
    for (std::size_t i = 0; i < plane.size(); ++i) bytes[i] = static_cast<char>(std::lround(plane[i] * 255.0));
    return std::make_shared<const std::string>(write_png(Image::frombytes("L", Size{ctx.w, ctx.h}, bytes)));
}

// --- colours -----------------------------------------------------------------------------------------------------

// A colour as sRGB (0..1): an RGB one as it is, a grey one's lightness, a CMYK or Lab one through the page's profile
// (the built-in coated-cmyk on a page of another mode).
std::array<double, 3> srgb_of(const Ctx& ctx, const Color& c) {
    switch (c.mode) {
    case Mode::Grayscale:
    case Mode::Bitmap:
    case Mode::Duotone:
        return {c.c[0], c.c[0], c.c[0]};
    case Mode::Cmyk:
    case Mode::Lab: {
        core::PageColor color;
        if (ctx.inks && ((c.mode == Mode::Cmyk && ctx.inks->space == core::InkSpace::Cmyk) || (c.mode == Mode::Lab && ctx.inks->space == core::InkSpace::Lab))) {
            color = *ctx.inks;
        } else if (c.mode == Mode::Cmyk) {
            const core::BuiltinProfile* builtin = core::builtin_profile("coated-cmyk");
            color.space = core::InkSpace::Cmyk;
            if (builtin != nullptr && builtin->bytes) {
                color.icc = builtin->bytes;
                color.profile = core::bytes_ref(*builtin->bytes);
            }
        } else {
            color.space = core::InkSpace::Lab;
        }
        std::vector<double> ink = c.mode == Mode::Cmyk ? std::vector<double>{c.c[0], c.c[1], c.c[2], c.c[3]}
                                                       : std::vector<double>{c.c[0] * 100.0, c.c[1] * 255.0 - 128.0, c.c[2] * 255.0 - 128.0};
        for (double& v : ink) v = std::isfinite(v) ? v : 0.0;
        if (c.mode == Mode::Cmyk) {
            for (double& v : ink) v = std::clamp(v, 0.0, 1.0);
        } else {
            ink[0] = std::clamp(ink[0], 0.0, 100.0);
            ink[1] = std::clamp(ink[1], -128.0, 127.0);
            ink[2] = std::clamp(ink[2], -128.0, 127.0);
        }
        return ink::srgb_of(color, ink);
    }
    default:
        return {c.c[0], c.c[1], c.c[2]};
    }
}

Json rgb_ints(const std::array<double, 3>& rgb) {
    return Json::array({std::lround(std::clamp(rgb[0], 0.0, 1.0) * 255.0), std::lround(std::clamp(rgb[1], 0.0, 1.0) * 255.0),
                        std::lround(std::clamp(rgb[2], 0.0, 1.0) * 255.0)});
}

std::array<int, 3> rgb_array(const std::array<double, 3>& rgb) {
    return {static_cast<int>(std::lround(std::clamp(rgb[0], 0.0, 1.0) * 255.0)), static_cast<int>(std::lround(std::clamp(rgb[1], 0.0, 1.0) * 255.0)),
            static_cast<int>(std::lround(std::clamp(rgb[2], 0.0, 1.0) * 255.0))};
}

double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

// --- adjustments (upstream's doc::Adjustment → a correction's adjust) ------------------------------------------------

std::string tone_space(const std::string& upstream, const core::Page& page) {
    const std::string_view page_space = core::default_tone_space(&page);
    if (upstream == "Cmyk") return "cmyk";
    if (upstream == "Lab") return "lab";
    return page_space == "gray" ? "gray" : "rgb";
}

Json levels_record(const Json& r) {
    return Json{{"black", r4(number(field(r, "in_black", "levels"), "levels") * 255.0)},
                {"white", r4(number(field(r, "in_white", "levels"), "levels") * 255.0)},
                {"gamma", r4(number(field(r, "gamma", "levels"), "levels"))},
                {"out_black", r4(number(field(r, "out_black", "levels"), "levels") * 255.0)},
                {"out_white", r4(number(field(r, "out_white", "levels"), "levels") * 255.0)}};
}

bool identity_levels(const Json& record) {
    return record["black"] == Json(0.0) && record["white"] == Json(255.0) && record["gamma"] == Json(1.0) && record["out_black"] == Json(0.0) &&
           record["out_white"] == Json(255.0);
}

// A curve's points in 0..255, at most 19 (more taken at even steps: `thinned`); none for an identity or too short one.
std::optional<Json> curve_points(const Json& list, bool& thinned) {
    if (!list.is_array()) throw Unreadable("curves: not a list");
    std::vector<std::array<double, 2>> pts;
    for (const Json& p : list) pts.push_back({r4(number(field(p, "input", "curves"), "curves") * 255.0), r4(number(field(p, "output", "curves"), "curves") * 255.0)});
    if (pts.size() < 2) return std::nullopt;
    if (pts.size() == 2 && pts[0] == std::array<double, 2>{0, 0} && pts[1] == std::array<double, 2>{255, 255}) return std::nullopt;
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    if (pts.size() > 19) {
        thinned = true;
        std::vector<std::array<double, 2>> out;
        for (std::size_t k = 0; k < 19; ++k) out.push_back(pts[k * (pts.size() - 1) / 18]);
        pts = std::move(out);
    }
    Json out = Json::array();
    for (const auto& p : pts) out.push_back(Json::array({std::clamp(p[0], 0.0, 255.0), std::clamp(p[1], 0.0, 255.0)}));
    return out;
}

Json numbers_times(const Json& list, std::size_t n, double k, std::string_view what) {
    if (!list.is_array() || list.size() != n) throw Unreadable(std::string(what) + ": not " + std::to_string(n) + " numbers");
    Json out = Json::array();
    for (const Json& v : list) out.push_back(r4(number(v, what) * k));
    return out;
}

std::array<double, 3> rgb3(const Json& list, std::string_view what) {
    if (!list.is_array() || list.size() != 3) throw Unreadable(std::string(what) + ": not 3 numbers");
    return {number(list[0], what), number(list[1], what), number(list[2], what)};
}

constexpr std::array<std::string_view, 6> kHueRanges{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
constexpr std::array<std::string_view, 9> kSelectiveRanges{"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};

// The adjust of an upstream adjustment on the page (none: one this build does not read — `why` says why).
std::optional<Json> adjust_of(const Json& adjustment, const core::Page& page, core::LookupTables& made, std::string& why, bool& approximated) {
    const auto [kind, data] = variant(adjustment, "adjustment");
    const Json& d = data != nullptr ? *data : Json::object();
    if (kind == "Invert") return Json{{"kind", "invert"}};
    if (kind == "BrightnessContrast") {
        const bool legacy = boolean(field(d, "legacy", kind), kind);
        return Json{{"kind", "brightness_contrast"}, {"brightness", r4(std::clamp(number(field(d, "brightness", kind), kind), -150.0, 150.0))},
                    {"contrast", r4(std::clamp(number(field(d, "contrast", kind), kind), legacy ? -100.0 : -50.0, 100.0))},
                    {"native", Json{{"method", legacy ? "legacy" : "modern"}}}};
    }
    if (kind == "Levels") {
        const std::string space = tone_space(optional_field(d, "space") ? text(d["space"], kind) : std::string("Rgb"), page);
        const Json master = space == "lab" ? Json{{"black", 0.0}, {"white", 255.0}, {"gamma", 1.0}, {"out_black", 0.0}, {"out_white", 255.0}}
                                           : levels_record(field(d, "master", kind));
        const Json& per = field(d, "per_channel", kind);
        if (!per.is_array() || per.size() != 3) throw Unreadable("levels: per_channel is 3 records");
        Json channels = Json::object();
        if (space != "gray") {
            const std::vector<std::string_view> names = core::tone_channels(space);
            for (std::size_t i = 0; i < names.size(); ++i) {
                const Json* rec = i < 3 ? &per[i] : optional_field(d, "black");
                if (rec == nullptr) continue;
                Json r = levels_record(*rec);
                if (!identity_levels(r)) channels[std::string(names[i])] = std::move(r);
            }
        }
        Json out{{"kind", "levels"}};
        for (const auto& [key, value] : master.items()) out[key] = value;
        Json native{{"method", "upstream"}};
        if (!channels.empty()) {
            native["space"] = space;
            native["channels"] = core::read_tone_channels("levels", channels, space, core::ReadMode::Op, "levels");
        }
        out["native"] = std::move(native);
        return out;
    }
    if (kind == "Curves") {
        const std::string space = tone_space(optional_field(d, "space") ? text(d["space"], kind) : std::string("Rgb"), page);
        bool thinned = false;
        const std::optional<Json> master = space == "lab" ? std::nullopt : curve_points(field(d, "master", kind), thinned);
        const Json& per = field(d, "per_channel", kind);
        if (!per.is_array() || per.size() != 3) throw Unreadable("curves: per_channel is 3 curves");
        Json channels = Json::object();
        if (space != "gray") {
            const std::vector<std::string_view> names = core::tone_channels(space);
            for (std::size_t i = 0; i < names.size(); ++i) {
                const Json* list = i < 3 ? &per[i] : optional_field(d, "black");
                if (list == nullptr) continue;
                if (auto pts = curve_points(*list, thinned)) channels[std::string(names[i])] = std::move(*pts);
            }
        }
        approximated = approximated || thinned;
        Json out{{"kind", "curve"}, {"points", master ? *master : Json::array({Json::array({0.0, 0.0}), Json::array({255.0, 255.0})})}};
        Json native{{"method", "upstream"}};
        if (!channels.empty()) {
            native["space"] = space;
            native["channels"] = core::read_tone_channels("curve", channels, space, core::ReadMode::Op, "curve");
        }
        out["native"] = std::move(native);
        return out;
    }
    if (kind == "Exposure") {
        return Json{{"kind", "exposure"}, {"exposure", r4(std::clamp(number(field(d, "exposure", kind), kind), -20.0, 20.0))},
                    {"offset", r4(std::clamp(number(field(d, "offset", kind), kind), -0.5, 0.5))},
                    {"gamma", r4(std::clamp(number(field(d, "gamma", kind), kind), 0.01, 9.99))}};
    }
    if (kind == "Vibrance") {
        return Json{{"kind", "vibrance"}, {"vibrance", r4(number(field(d, "vibrance", kind), kind))}, {"saturation", r4(number(field(d, "saturation", kind), kind))}};
    }
    if (kind == "HueSaturation") {
        Json out{{"kind", "hue_saturation"}, {"hue", r4(number(field(d, "hue", kind), kind))}, {"saturation", r4(number(field(d, "saturation", kind), kind))},
                 {"lightness", r4(number(field(d, "lightness", kind), kind))}, {"colorize", boolean(field(d, "colorize", kind), kind)}};
        if (const Json* ranges = optional_field(d, "ranges")) {
            if (!ranges->is_array() || ranges->size() != 6) throw Unreadable("hue/saturation: ranges are 6");
            for (std::size_t i = 0; i < 6; ++i) {
                const Json& r = (*ranges)[i];
                out[std::string(kHueRanges[i])] = Json{{"hue", r4(number(field(r, "hue", kind), kind))}, {"saturation", r4(number(field(r, "saturation", kind), kind))},
                                                      {"lightness", r4(number(field(r, "lightness", kind), kind))},
                                                      {"range", numbers_times(field(r, "bounds", kind), 4, 1.0, kind)}};
            }
        }
        return out;
    }
    if (kind == "ColorBalance") {
        return Json{{"kind", "color_balance"}, {"shadows", numbers_times(field(d, "shadows", kind), 3, 1.0, kind)},
                    {"midtones", numbers_times(field(d, "midtones", kind), 3, 1.0, kind)},
                    {"highlights", numbers_times(field(d, "highlights", kind), 3, 1.0, kind)},
                    {"preserve_luminosity", boolean(field(d, "preserve_luminosity", kind), kind)}};
    }
    if (kind == "BlackWhite") {
        const Json& w = field(d, "weights", kind);
        if (!w.is_array() || w.size() != 6) throw Unreadable("black & white: weights are 6");
        Json out{{"kind", "black_white"}};
        for (std::size_t i = 0; i < 6; ++i) out[std::string(kHueRanges[i])] = r4(number(w[i], kind));
        const Json* tint = optional_field(d, "tint");
        out["tint"] = tint != nullptr;
        out["tint_color"] = tint != nullptr ? rgb_ints(rgb3(*tint, kind)) : Json::array({225, 211, 179});
        return out;
    }
    if (kind == "PhotoFilter") {
        return Json{{"kind", "photo_filter"}, {"color", rgb_ints(rgb3(field(d, "color", kind), kind))},
                    {"density", r4(std::clamp(number(field(d, "density", kind), kind), 0.0, 1.0) * 100.0)},
                    {"preserve_luminosity", boolean(field(d, "preserve_luminosity", kind), kind)}};
    }
    if (kind == "ChannelMixer") {
        const Json& m = field(d, "matrix", kind);
        if (!m.is_array() || m.size() != 3) throw Unreadable("channel mixer: matrix is 3 rows");
        const bool mono = boolean(field(d, "monochrome", kind), kind);
        Json out{{"kind", "channel_mixer"}, {"red", numbers_times(m[0], 4, 100.0, kind)}, {"green", numbers_times(m[1], 4, 100.0, kind)},
                 {"blue", numbers_times(m[2], 4, 100.0, kind)}};
        if (mono) out["gray"] = numbers_times(m[0], 4, 100.0, kind);
        out["monochrome"] = mono;
        return out;
    }
    if (kind == "ColorLookup") {
        const Json* lut = optional_field(d, "lut");
        if (lut == nullptr) {
            why = "表を持たない（プロファイルによる）";
            return std::nullopt;
        }
        const std::int64_t size = whole(field(d, "size", kind), kind);
        if (size < 2 || size > static_cast<std::int64_t>(core::lut3d::kMaxSize) || !lut->is_array() ||
            lut->size() != static_cast<std::size_t>(size * size * size * 3)) {
            why = "表の大きさが読めない";
            return std::nullopt;
        }
        core::lut3d::Table table;
        table.title = optional_field(d, "name") ? text(d["name"], kind) : std::string();
        table.size = static_cast<std::size_t>(size);
        table.data.reserve(lut->size());
        for (const Json& v : *lut) table.data.push_back(static_cast<float>(number(v, kind)));
        const std::string bytes = core::lut3d::encode(table);
        const std::string ref = core::bytes_ref(bytes);
        made[ref] = std::make_shared<const std::string>(bytes);
        Json out{{"kind", "color_lookup"}, {"lut", ref}, {"size", size}};
        if (!table.title.empty() && table.title.size() <= 255) out["name"] = table.title;
        const Json* tet = optional_field(d, "tetrahedral");
        const Json* dither = optional_field(d, "dither");
        out["interpolation"] = tet != nullptr && boolean(*tet, kind) ? "tetrahedral" : "trilinear";
        out["dither"] = dither != nullptr && boolean(*dither, kind);
        return out;
    }
    if (kind == "Posterize") {
        return Json{{"kind", "posterize"}, {"levels", std::clamp<std::int64_t>(whole(field(d, "levels", kind), kind), 2, 255)}, {"native", Json{{"method", "floor"}}}};
    }
    if (kind == "Threshold") {
        return Json{{"kind", "threshold"}, {"threshold", std::clamp<std::int64_t>(std::llround(number(field(d, "level", kind), kind) * 255.0), 1, 255)}};
    }
    if (kind == "GradientMap") {
        const Json& stops = field(d, "stops", kind);
        if (!stops.is_array() || stops.size() < 2) {
            why = "色の分岐点が足りない";
            return std::nullopt;
        }
        Json list = Json::array();
        for (const Json& s : stops) {
            if (!s.is_array() || s.size() != 2) throw Unreadable("gradient map: a stop is [position, colour]");
            list.push_back(Json::array({r4(std::clamp(number(s[0], kind), 0.0, 1.0)), rgb_ints(rgb3(s[1], kind))}));
        }
        Json out{{"kind", "gradient_map"}, {"stops", std::move(list)}};
        Json native = Json::object();
        if (boolean(field(d, "reverse", kind), kind)) native["reverse"] = true;
        if (const Json* dither = optional_field(d, "dither"); dither != nullptr && boolean(*dither, kind)) native["dither"] = true;
        if (!native.empty()) out["native"] = std::move(native);
        return out;
    }
    if (kind == "SelectiveColor") {
        const Json& a = field(d, "adjustments", kind);
        if (!a.is_array() || a.size() != 9) throw Unreadable("selective color: 9 ranges");
        Json out{{"kind", "selective_color"}, {"method", boolean(field(d, "relative", kind), kind) ? "relative" : "absolute"}};
        for (std::size_t i = 0; i < 9; ++i) out[std::string(kSelectiveRanges[i])] = numbers_times(a[i], 4, 1.0, kind);
        return out;
    }
    if (kind == "Unsupported") {
        why = "PhotoCraft も読めない調整（" + (optional_field(d, "psd_key") ? text(d["psd_key"], kind) : std::string("?")) + "）";
        return std::nullopt;
    }
    why = "この版の Genko が知らない種類（" + kind + "）";
    return std::nullopt;
}

// --- fills -----------------------------------------------------------------------------------------------------------

// A gradient fill's from and to over a frame (x0, y0, w, h, pixels), as upstream lays its angle and scale out over it
// (compose/effects.rs gradient_t): a linear and a reflected one along the chord through the frame's centre, a radial and a
// diamond one out to half the frame's extent along the angle.
std::pair<std::array<double, 2>, std::array<double, 2>> gradient_line(const std::string& style, double angle, double scale, const std::array<double, 4>& frame) {
    const double w = std::max(1.0, frame[2]), h = std::max(1.0, frame[3]);
    const double cx = frame[0] + w / 2, cy = frame[1] + h / 2;
    const double a = angle * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double k = std::max(scale, 1e-3);
    const double chord = std::max(1.0, std::min(w / std::max(std::fabs(c), 1e-6), h / std::max(std::fabs(s), 1e-6))) * k;
    const double len = std::max(1.0, std::hypot(c * w, s * h)) * k;
    const double ux = c, uy = -s;
    if (style == "Linear") return {{cx - ux * chord / 2, cy - uy * chord / 2}, {cx + ux * chord / 2, cy + uy * chord / 2}};
    if (style == "Reflected") return {{cx, cy}, {cx + ux * chord / 2, cy + uy * chord / 2}};
    return {{cx, cy}, {cx + ux * len / 2, cy + uy * len / 2}};
}

std::string gradient_shape(const std::string& style) {
    if (style == "Radial") return "radial";
    if (style == "Angle") return "angle";
    if (style == "Reflected") return "reflected";
    if (style == "Diamond") return "diamond";
    if (style == "Linear") return "linear";
    throw Unreadable("gradient: not a style this build knows");
}

// A book's pattern for upstream's (by id, then name): none when the document holds no such pattern.
std::optional<std::string> book_pattern(const Ctx& ctx, const std::string& id, const std::string& name) {
    if (const auto found = ctx.patterns.find(id); !id.empty() && found != ctx.patterns.end()) return found->second;
    if (const auto found = ctx.patterns.find("name:" + name); found != ctx.patterns.end()) return found->second;
    return std::nullopt;
}

// A fill (upstream's Fill) as Genko's fill spec over a frame of pixels; the alpha of a colour's in `alpha`.
Json fill_of(Ctx& ctx, const Json& fill, const std::array<double, 4>& frame, double& alpha) {
    const auto [kind, data] = variant(fill, "fill");
    if (data == nullptr) throw Unreadable("fill: no data");
    alpha = 1.0;
    if (kind == "Solid") {
        const Color c = color_from(*data);
        alpha = c.alpha;
        return Json{{"rgb", rgb_ints(srgb_of(ctx, c))}};
    }
    if (kind == "Gradient") {
        const std::string style = text(field(*data, "style", "gradient"), "gradient.style");
        const auto [from, to] = gradient_line(style, number(field(*data, "angle", "gradient"), "gradient.angle"), number(field(*data, "scale", "gradient"), "gradient.scale"), frame);
        Json stops = Json::array();
        const Json& list = field(*data, "stops", "gradient");
        if (!list.is_array() || list.size() < 2 || list.size() > 16) throw Unreadable("gradient: 2 to 16 stops");
        for (const Json& s : list) {
            if (!s.is_array() || s.size() != 2) throw Unreadable("gradient: a stop is [position, colour]");
            const Color c = color_from(s[1]);
            stops.push_back(Json::array({r4(std::clamp(number(s[0], "gradient"), 0.0, 1.0)), rgb_ints(srgb_of(ctx, c)), std::round(c.alpha * 1000.0) / 1000.0}));
        }
        Json g{{"from", Json::array({r4(from[0] * ctx.mm), r4(from[1] * ctx.mm)})},
               {"to", Json::array({r4(to[0] * ctx.mm), r4(to[1] * ctx.mm)})},
               {"shape", gradient_shape(style)},
               {"stops", std::move(stops)}};
        if (boolean(field(*data, "reverse", "gradient"), "gradient.reverse")) g["reverse"] = true;
        return core::fill_spec(Json{{"gradient", std::move(g)}});
    }
    if (kind == "Pattern") {
        const std::string id = optional_field(*data, "id") ? text((*data)["id"], "pattern.id") : std::string();
        const std::string name = text(field(*data, "name", "pattern"), "pattern.name");
        const std::optional<std::string> book = book_pattern(ctx, id, name);
        if (!book) throw Unreadable("a pattern the document does not hold");
        double phase_x = 0, phase_y = 0;
        if (const Json* phase = optional_field(*data, "phase")) {
            if (!phase->is_array() || phase->size() != 2) throw Unreadable("pattern: phase is 2 numbers");
            phase_x = number((*phase)[0], "pattern.phase");
            phase_y = number((*phase)[1], "pattern.phase");
        }
        const Json* link = optional_field(*data, "link");
        Json spec{{"id", *book},
                  {"scale", r4(std::clamp(number(field(*data, "scale", "pattern"), "pattern.scale") * 100.0, core::kPatternScaleMin, core::kPatternScaleMax))},
                  {"angle", r4(std::clamp(optional_field(*data, "angle") ? number((*data)["angle"], "pattern.angle") : 0.0, -360.0, 360.0))},
                  {"phase_mm", Json::array({r4(phase_x * ctx.mm), r4(phase_y * ctx.mm)})},
                  {"link", link == nullptr || boolean(*link, "pattern.link")}};
        return Json{{"pattern", core::pattern_fill_spec(spec)}};
    }
    throw Unreadable("fill: not a kind this build knows");
}

// The document's patterns into the book (each a pattern of the book at the document's resolution: one pattern pixel a
// document pixel at 100 %).
void take_patterns(Ctx& ctx, const Json& list) {
    if (!list.is_array()) return;
    for (const Json& p : list) {
        std::string name;
        try {
            name = text(field(p, "name", "pattern"), "pattern.name");
            const std::string id = text(field(p, "id", "pattern"), "pattern.id");
            const std::int64_t w = whole(field(p, "width", "pattern"), "pattern.width"), h = whole(field(p, "height", "pattern"), "pattern.height");
            if (w < 1 || h < 1 || w > core::kMaxPatternSide || h > core::kMaxPatternSide || w * h > core::kMaxPatternPixels) throw Unreadable("pattern: its size");
            const Surface surface = surface_from(field(p, "surface", "pattern"));
            Ctx sub{ctx.load, {}, {}, static_cast<int>(w), static_cast<int>(h)};
            sub.way = ctx.way;
            sub.inks = ctx.inks;
            sub.sample = ctx.sample;
            core::Pattern made;
            made.id = id.size() <= 64 && !id.empty() &&
                              std::all_of(id.begin(), id.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_'; })
                          ? id
                          : std::string();
            std::string shown = name;
            if (const std::size_t eq = shown.find('='); shown.starts_with("$$$/") && eq != std::string::npos) shown = shown.substr(eq + 1);
            made.name = shown.empty() ? std::string("パターン") : shown.substr(0, 255);
            made.width = w;
            made.height = h;
            made.dpi = std::clamp(ctx.dpi, 50.0, 2400.0);
            core::Layer holder;
            put_pixels(sub, holder, surface, name);
            core::Bytes bytes;
            if (holder.raster_png) {
                made.mode = "rgb";
                made.depth = 8;
                bytes = holder.raster_png;
            } else if (holder.color_raster) {
                made.mode = "rgb";
                made.depth = sub.sample == Sample::F32 ? 32 : 16;
                bytes = holder.color_raster;
            } else {
                made.mode = std::string(core::to_string(sub.inks->space));
                made.depth = sub.inks->depth;
                // (a grey pattern of 8 bits is a PNG LA; of more bits, and CMYK and Lab, GKIR version 2)
                core::InkPixels inks = core::decode_ink_raster(*holder.ink_raster);
                if (made.mode == "gray" && made.depth == 8) {
                    std::string la(std::size_t(w) * h * 2, '\0');
                    for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
                        la[i * 2] = static_cast<char>(std::lround(inks.value(i, 0) * 255.0));
                        la[i * 2 + 1] = static_cast<char>(std::lround(inks.value(i, 1) * 255.0));
                    }
                    bytes = std::make_shared<const std::string>(write_png(Image::frombytes("LA", Size{static_cast<int>(w), static_cast<int>(h)}, la)));
                } else {
                    bytes = holder.ink_raster;
                }
            }
            made.asset = core::bytes_ref(*bytes);
            const auto [book, added] = core::carry_pattern(ctx.doc, made, bytes);
            if (!id.empty()) ctx.patterns[id] = book;
            ctx.patterns.emplace("name:" + name, book);
        } catch (const Unreadable&) {
            ctx.notes.name("読めないパターンを外しました", name.empty() ? std::string("?") : name);
        } catch (const core::Error& error) {
            if (error.code() == "image_too_large" || error.code() == "format" || error.code() == "unsupported_version") throw;
            ctx.notes.name("読めないパターンを外しました", name.empty() ? std::string("?") : name);
        }
    }
}

// --- layers ----------------------------------------------------------------------------------------------------------

void read_layers(Ctx& ctx, const Json& list, const Json& parent, bool hidden, std::vector<core::Layer>& out);

std::string layer_name(const Json& m) {
    const Json* name = optional_field(m, "name");
    std::string text = name != nullptr && name->is_string() ? name->get<std::string>() : std::string();
    text = core::utf8_replaced(text);
    std::size_t at = 0, count = 0;
    while (at < text.size() && count < 255) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
        ++count;
    }
    text.resize(at);
    return text.empty() ? std::string("レイヤー") : text;
}

// A layer's mask as Genko's (a PNG "L" of the page's pixels).
core::Mask mask_of(Ctx& ctx, const Json& m, const std::string& name) {
    core::Mask mask;
    const Surface surface = surface_from(field(m, "surface", "mask"));
    bool cut = false;
    mask.png = grey_png(ctx, grey_plane(ctx, surface, cut));
    mask.enabled = boolean(field(m, "enabled", "mask"), "mask.enabled");
    mask.linked = boolean(field(m, "linked", "mask"), "mask.linked");
    mask.density = r4(std::clamp(number(field(m, "density", "mask"), "mask.density"), 0.0, 1.0));
    mask.feather_mm = r4(std::clamp(number(field(m, "feather", "mask"), "mask.feather") * ctx.mm, 0.0, core::kMaskFeatherMax));
    (void)name;
    return mask;
}

// What a layer holds beyond Genko's (effects, Blend If, left-out channels, a label, a video's frames): said, left out.
void note_extras(Ctx& ctx, const Json& m, const std::string& name) {
    if (const Json* effects = optional_field(m, "effects")) {
        const Json* items = optional_field(*effects, "items");
        if ((items != nullptr && items->is_array() && !items->empty()) || optional_field(*effects, "psd_raw") != nullptr) {
            ctx.notes.name("レイヤー効果は Genko で描けないため外しました", name);
        }
    }
    if (const Json* blend_if = optional_field(m, "blend_if"); blend_if != nullptr && !(blend_if->is_object() && blend_if->value("ranges", Json::array()).empty())) {
        ctx.notes.name("ブレンド条件（Blend If）は Genko に無いため外しました", name);
    }
    if (const Json* excluded = optional_field(m, "excluded_channels"); excluded != nullptr && *excluded != Json(0)) {
        ctx.notes.name("高度な合成のチャンネルの指定は Genko に無いため外しました", name);
    }
    if (const Json* label = optional_field(m, "label"); label != nullptr && *label != Json("None")) ctx.notes.name("レイヤーのラベルの色は外しました", name);
    if (optional_field(m, "video") != nullptr) ctx.notes.name("ビデオレイヤーのコマはタイムライン（PC5）が入るまで読めないため外しました", name);
}

// The settings every layer has: its name, visibility, opacity (its fill opacity with it when it has no effects), blend,
// clipping, locks, link, masks and the PSD blocks it carried.
core::Layer base_layer(Ctx& ctx, const Json& m, const std::string& name, const Json& parent, bool hidden) {
    core::Layer layer;
    layer.id = core::new_id();
    layer.role = core::LayerRole::User;
    layer.title = name;
    layer.exportable = true;
    layer.panel_clip = false;
    layer.parent_id = parent;
    layer.visible = boolean(field(m, "visible", "layer"), "layer.visible") && !hidden;
    double opacity = std::clamp(number(field(m, "opacity", "layer"), "layer.opacity"), 0.0, 1.0);
    const double fill = std::clamp(number(field(m, "fill_opacity", "layer"), "layer.fill_opacity"), 0.0, 1.0);
    if (fill < 1.0) opacity *= fill;  // (its effects, which the fill opacity spares, are left out)
    layer.opacity = std::round(opacity * 1000.0) / 1000.0;
    const std::string blend = blend_from(field(m, "blend", "layer"));
    if (const auto mapped = genko_blend(blend)) {
        layer.blend = *mapped;
    } else if (blend != "PassThrough") {
        ctx.notes.name("描画モード「" + blend + "」は Genko に無いため「通常」にしました", name);
    }
    layer.clip = boolean(field(m, "clipped", "layer"), "layer.clipped");
    const Json& locks = field(m, "locks", "layer");
    const bool all = boolean(field(locks, "all", "locks"), "locks.all");
    layer.lock_alpha = boolean(field(locks, "transparency", "locks"), "locks.transparency");
    layer.locked = all || boolean(field(locks, "pixels", "locks"), "locks.pixels");
    layer.lock_position = all || boolean(field(locks, "position", "locks"), "locks.position");
    if (const Json* group = optional_field(m, "link_group")) layer.link = "pcraft-" + std::to_string(whole(*group, "layer.link_group"));
    if (const Json* mask = optional_field(m, "mask")) layer.mask = mask_of(ctx, *mask, name);
    if (const Json* vm = optional_field(m, "vector_mask")) layer.vector_mask = vector_mask_from(*vm, ctx.mm);
    // (psd-keep@1: the PSD's blocks it carried, and its PSD layer id)
    core::PsdLayerKeep keep;
    if (const Json* blocks = optional_field(m, "psd_blocks"); blocks != nullptr && blocks->is_array()) {
        for (const Json& b : *blocks) {
            if (!b.is_array() || b.size() != 2) throw Unreadable("psd_blocks: [key, blob]");
            const std::optional<std::string> key = key_from_hex(text(b[0], "psd_blocks"));
            if (!key || !std::all_of(key->begin(), key->end(), [](char c) { return c >= 0x20 && c < 0x7f; })) continue;
            const core::Bytes bytes = ctx.load.blob(text(b[1], "psd_blocks"));
            const std::string ref = core::bytes_ref(*bytes);
            ctx.doc.native_files[ref] = bytes;
            if (keep.blocks.size() < core::kPsdKeptMax) keep.blocks.emplace_back(*key, ref);
        }
    }
    if (const Json* id = optional_field(m, "psd_id")) keep.id = whole(*id, "layer.psd_id");
    if (!keep.empty()) layer.native_psd = std::move(keep);
    note_extras(ctx, m, name);
    return layer;
}

// The layer's own frame in pixels (upstream's fill_frame: its hiding mask's content, its vector mask's box, else the canvas).
std::array<double, 4> frame_of(const Ctx& ctx, const core::Layer& layer) {
    std::array<double, 4> frame{0, 0, static_cast<double>(ctx.w), static_cast<double>(ctx.h)};
    if (layer.vector_mask && layer.vector_mask->enabled && !layer.vector_mask->path.inverted) {
        if (const auto b = layer.vector_mask->path.control_bounds()) {
            const double x0 = std::max(0.0, std::floor(b->at(0) / ctx.mm)), y0 = std::max(0.0, std::floor(b->at(1) / ctx.mm));
            const double x1 = std::min<double>(ctx.w, std::ceil(b->at(2) / ctx.mm)), y1 = std::min<double>(ctx.h, std::ceil(b->at(3) / ctx.mm));
            if (x1 > x0 && y1 > y0) frame = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    return frame;
}

// A layer of pixels made of a surface upstream drew (a text's, a shape's or a smart object's picture of it).
bool as_pixels(Ctx& ctx, core::Layer& layer, const Json* cache, const std::string& name) {
    if (cache == nullptr) return false;
    layer.kind = core::LayerKind::Raster;
    put_pixels(ctx, layer, surface_from(*cache), name);
    return true;
}

// A stroke or fill of a shape as a shape part's fill (a colour's alpha into the part's opacity).
Json part_fill(Ctx& ctx, const Json& fill, const std::array<double, 4>& frame, double& alpha) { return fill_of(ctx, fill, frame, alpha); }

std::optional<core::vector::ShapePart> shape_part(Ctx& ctx, const Json& d) {
    core::vector::ShapePart part;
    part.id = "s1";
    part.path = path_from(field(d, "path", "shape"), ctx.mm);
    const auto b = part.path.control_bounds();
    const std::array<double, 4> frame = b ? std::array<double, 4>{b->at(0) / ctx.mm, b->at(1) / ctx.mm, (b->at(2) - b->at(0)) / ctx.mm, (b->at(3) - b->at(1)) / ctx.mm}
                                          : std::array<double, 4>{0, 0, static_cast<double>(ctx.w), static_cast<double>(ctx.h)};
    double alpha = 1.0;
    if (const Json* fill = optional_field(d, "fill")) {
        part.fill = part_fill(ctx, *fill, frame, alpha);
        part.opacity = std::round(alpha * 1000.0) / 1000.0;
    }
    if (const Json* stroke = optional_field(d, "stroke")) {
        core::vector::ShapeStroke s;
        s.width_mm = r4(std::clamp(number(field(*stroke, "width", "stroke"), "stroke.width") * ctx.mm, 0.0, core::vector::kStrokeWidthMax));
        double paint_alpha = 1.0;
        s.fill = part_fill(ctx, field(*stroke, "paint", "stroke"), frame, paint_alpha);
        s.opacity = std::round(std::clamp(number(field(*stroke, "opacity", "stroke"), "stroke.opacity") * paint_alpha, 0.0, 1.0) * 1000.0) / 1000.0;
        const std::string align = text(field(*stroke, "align", "stroke"), "stroke.align");
        s.align = align == "Inside" ? core::vector::StrokeAlign::Inside : align == "Outside" ? core::vector::StrokeAlign::Outside : core::vector::StrokeAlign::Center;
        const std::string cap = text(field(*stroke, "cap", "stroke"), "stroke.cap");
        s.cap = cap == "Round" ? core::vector::LineCap::Round : cap == "Square" ? core::vector::LineCap::Square : core::vector::LineCap::Butt;
        const std::string join = text(field(*stroke, "join", "stroke"), "stroke.join");
        s.join = join == "Round" ? core::vector::LineJoin::Round : join == "Bevel" ? core::vector::LineJoin::Bevel : core::vector::LineJoin::Miter;
        s.miter_limit = std::clamp(number(field(*stroke, "miter_limit", "stroke"), "stroke.miter_limit"), 1.0, 1000.0);
        const Json& dashes = field(*stroke, "dashes", "stroke");
        if (!dashes.is_array() || dashes.size() > 64) throw Unreadable("stroke: dashes");
        for (const Json& v : dashes) s.dashes.push_back(r4(std::max(0.0, number(v, "stroke.dashes"))));
        s.dash_offset = r4(number(field(*stroke, "dash_offset", "stroke"), "stroke.dash_offset"));
        part.stroke = std::move(s);
    }
    if (const Json* live = optional_field(d, "live")) {
        const auto [kind, data] = variant(*live, "live");
        if (data == nullptr) throw Unreadable("live: no data");
        core::vector::LiveShape shape;
        const auto four = [&](const char* key) {
            const Json& v = field(*data, key, "live");
            if (!v.is_array() || v.size() != 4) throw Unreadable("live: 4 numbers");
            return std::array<double, 4>{number(v[0], "live"), number(v[1], "live"), number(v[2], "live"), number(v[3], "live")};
        };
        if (kind == "Rect" || kind == "Ellipse" || kind == "Polygon") {
            const auto r = four("rect");
            shape.rect = {r4(r[0] * ctx.mm), r4(r[1] * ctx.mm), r4(r[2] * ctx.mm), r4(r[3] * ctx.mm)};
            if (kind == "Rect") {
                shape.kind = core::vector::LiveShape::Kind::Rect;
                const auto radii = four("radii");
                shape.radii = {r4(radii[0] * ctx.mm), r4(radii[1] * ctx.mm), r4(radii[2] * ctx.mm), r4(radii[3] * ctx.mm)};
            } else if (kind == "Ellipse") {
                shape.kind = core::vector::LiveShape::Kind::Ellipse;
            } else {
                shape.kind = core::vector::LiveShape::Kind::Polygon;
                shape.sides = static_cast<int>(whole(field(*data, "sides", "live"), "live.sides"));
                shape.star_ratio = number(field(*data, "star_ratio", "live"), "live.star_ratio");
            }
        } else if (kind == "Line") {
            shape.kind = core::vector::LiveShape::Kind::Line;
            const Json& from = field(*data, "from", "live");
            const Json& to = field(*data, "to", "live");
            if (!from.is_array() || from.size() != 2 || !to.is_array() || to.size() != 2) throw Unreadable("live: a line's ends");
            shape.from = {r4(number(from[0], "live") * ctx.mm), r4(number(from[1], "live") * ctx.mm)};
            shape.to = {r4(number(to[0], "live") * ctx.mm), r4(number(to[1], "live") * ctx.mm)};
            shape.weight_mm = r4(number(field(*data, "weight", "live"), "live.weight") * ctx.mm);
        } else {
            throw Unreadable("live: not a shape this build knows");
        }
        part.live = shape;
    }
    try {
        return core::vector::part_from_json(core::vector::to_json(part));  // (its normal form, checked)
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

// The contents of an embedded file (a picture or PSD the smart objects read, or a .pcraft: its page), and their dpi.
std::optional<std::string> contents_of(Ctx& ctx, const std::string& file_name, const core::Bytes& bytes, double& dpi, std::string& why) {
    const auto suffix_at = file_name.rfind('.');
    std::string suffix = suffix_at == std::string::npos ? std::string() : file_name.substr(suffix_at);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (suffix == ".pcraft" || is_pcraft(*bytes)) {
        if (ctx.depth + 1 >= core::smart::kDepthMax) {
            why = "入れ子が深すぎる";
            return std::nullopt;
        }
        // (the .pcraft inside unpacks within what is left of this one's budget, and counts in it: nesting never
        // multiplies what may be unpacked)
        ReadLimits inner = ctx.load.limits();
        inner.depth = ctx.depth + 1;
        inner.max_unpacked_bytes = ctx.load.remaining();
        Read read = pcraft::read(*bytes, file_name, inner);
        ctx.load.account(read.unpacked);
        for (std::string& note : read.notes) ctx.notes.add("埋め込まれた .pcraft「" + file_name + "」: " + note);
        core::Page page = read.doc.page(0);
        page.index = core::Num(std::int64_t{1});
        page.native_source.reset();
        core::smart::bring_contents(ctx.doc, read.doc, page);
        auto contents = std::make_shared<core::SmartContents>();
        contents->kind = "page";
        contents->name = file_name.substr(0, core::smart::kNameMax);
        contents->page = std::make_shared<core::Page>(std::move(page));
        dpi = contents->page->spec.dpi.value();
        const std::string id = smart_ops::fresh_contents_id(ctx.doc);
        ctx.doc.smart_contents[id] = std::move(contents);
        return id;
    }
    if (bytes->size() > smart::kFileMaxBytes) {
        why = "大きすぎる";
        return std::nullopt;
    }
    smart::FileInfo info;
    try {
        info = smart::look_at(*bytes, "");
    } catch (const NotYetPorted&) {
        why = "この版の Genko が読めない形式の";
        return std::nullopt;
    } catch (const core::Error& error) {
        if (error.code() == "image_too_large") throw;
        why = "読めない";
        return std::nullopt;
    }
    if (info.width > core::smart::kSideMax || info.height > core::smart::kSideMax) {
        why = "大きすぎる";
        return std::nullopt;
    }
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "file";
    const auto named = core::smart::suffix_of_name(file_name);
    contents->suffix = named && core::smart::format_of_suffix(*named) == info.format ? *named : info.suffix;
    contents->name = (file_name.empty() ? std::string("スマートオブジェクト") + contents->suffix : file_name).substr(0, core::smart::kNameMax);
    contents->asset = core::bytes_ref(*bytes);
    ctx.doc.smart_files[contents->asset] = bytes;
    contents->format = info.format;
    contents->width_px = info.width;
    contents->height_px = info.height;
    contents->dpi = std::isfinite(info.dpi) && info.dpi >= 1 && info.dpi <= 100000 ? info.dpi : ctx.dpi;
    contents->depth = info.depth;
    contents->mode = info.mode;
    contents->profile = Json::object({{"use", "keep"}});
    dpi = contents->dpi;
    const std::string id = smart_ops::fresh_contents_id(ctx.doc);
    ctx.doc.smart_contents[id] = std::move(contents);
    return id;
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 11> kStacks{{{"Entropy", "entropy"},
                                                                                  {"Kurtosis", "kurtosis"},
                                                                                  {"Maximum", "maximum"},
                                                                                  {"Mean", "mean"},
                                                                                  {"Median", "median"},
                                                                                  {"Minimum", "minimum"},
                                                                                  {"Range", "range"},
                                                                                  {"Skewness", "skewness"},
                                                                                  {"StandardDeviation", "standard_deviation"},
                                                                                  {"Summation", "summation"},
                                                                                  {"Variance", "variance"}}};

// A smart object (upstream's Smart content) as Genko's: none (`why`) when its source cannot be the contents.
bool smart_layer(Ctx& ctx, core::Layer& layer, const Json& d, const std::string& name, std::string& why) {
    const Json& source = field(d, "source", "smart");
    const std::string kind = text(field(source, "kind", "smart.source"), "smart.source.kind");
    if (kind == "linked") {
        why = "リンクされたスマートオブジェクトは元のファイルを読まずに画素にしました";
        return false;
    }
    if (kind != "embedded") throw Unreadable("smart: a source this build does not know");
    const std::string file_name = core::utf8_replaced(text(field(source, "file_name", "smart.source"), "smart.source.file_name"));
    const core::Bytes bytes = ctx.load.blob(text(field(source, "blob", "smart.source"), "smart.source.blob"));
    double contents_dpi = ctx.dpi;
    std::string unread;
    std::optional<std::string> id;
    try {
        id = contents_of(ctx, file_name, bytes, contents_dpi, unread);
    } catch (const core::Error& error) {
        if (error.code() == "image_too_large") throw;
        unread = "読めない";
    }
    if (!id) {
        why = "スマートオブジェクトの中身（" + file_name + "）が" + unread + "ため画素にしました";
        return false;
    }
    // (its transform: the source's pixels → the document's; Genko's matrix: the contents' mm → the page's)
    const Affine t = affine_from(field(d, "transform", "smart"));
    const double k = contents_dpi / ctx.dpi;
    core::smart::Matrix m{t[0] * k, t[1] * k, t[2] * k, t[3] * k, t[4] * ctx.mm, t[5] * ctx.mm};
    m = core::smart::snapped(m);
    if (!(std::fabs(m.det()) > core::smart::kDetMin)) {
        why = "スマートオブジェクトの変形が潰れているため画素にしました";
        return false;
    }
    Json settings = core::smart::settings(*id, m, ctx.doc.smart_contents.at(*id)->kind == "page" ? "bicubic" : "lanczos");
    layer.kind = core::LayerKind::Smart;
    layer.smart = std::move(settings);
    layer.clip_to = "none";
    if (const Json* stack = optional_field(d, "stack_mode")) {
        const std::string s = text(*stack, "smart.stack_mode");
        for (const auto& [up, genko] : kStacks) {
            if (s == up) core::smart::put_setting(layer, "stack", Json(std::string(genko)));
        }
    }
    Json filters = Json::array();
    if (const Json* list = optional_field(d, "smart_filters"); list != nullptr && list->is_array()) {
        for (const Json& f : *list) {
            const std::string command = text(field(f, "command", "smart filter"), "smart filter.command");
            const auto mapped = core::from_upstream(command, field(f, "params", "smart filter"), ctx.dpi);
            if (!mapped) {
                ctx.notes.name("スマートフィルター「" + command + "」はこの版の Genko に無いため外しました", name);
                continue;
            }
            const std::string blend = blend_from(field(f, "blend", "smart filter"));
            const auto genko = genko_blend(blend);
            filters.push_back(Json{{"kind", mapped->first},
                                   {"params", mapped->second},
                                   {"blend", genko ? *genko : std::string("normal")},
                                   {"opacity", std::round(std::clamp(number(field(f, "opacity", "smart filter"), "smart filter.opacity"), 0.0, 1.0) * 1000.0) / 1000.0},
                                   {"visible", boolean(field(f, "visible", "smart filter"), "smart filter.visible")}});
            if (filters.size() >= core::smart::kFiltersMax) break;
        }
    }
    if (!filters.empty()) {
        core::smart::put_setting(layer, "filters", std::move(filters));
        if (const Json* on = optional_field(d, "filters_enabled"); on != nullptr && !boolean(*on, "smart.filters_enabled")) {
            core::smart::put_setting(layer, "filters_on", false);
        }
        if (const Json* fm = optional_field(d, "filter_mask")) {
            const Surface surface = surface_from(field(*fm, "surface", "filter mask"));
            bool cut = false;
            const core::Bytes png = grey_png(ctx, grey_plane(ctx, surface, cut));
            const std::string ref = core::bytes_ref(*png);
            ctx.doc.smart_files[ref] = png;
            core::smart::put_setting(layer, "filter_mask", Json{{"asset", ref}, {"enabled", boolean(field(*fm, "enabled", "filter mask"), "filter mask.enabled")}});
        }
    }
    if (optional_field(d, "warp") != nullptr) ctx.notes.name("スマートオブジェクトのワープは読めないため外しました", name);
    return true;
}

void read_layer(Ctx& ctx, const Json& m, const Json& parent, bool hidden, std::vector<core::Layer>& out) {
    const std::string name = layer_name(m);
    const Json& content = field(m, "content", "layer");
    const std::string kind = text(field(content, "kind", "layer.content"), "layer.content.kind");
    core::Layer layer = base_layer(ctx, m, name, parent, hidden);
    if (kind == "group") {
        layer.kind = core::LayerKind::Folder;
        if (layer.opacity < 1.0 || (layer.blend != "normal" && blend_from(m["blend"]) != "PassThrough") || layer.mask || layer.vector_mask) {
            ctx.notes.name("フォルダーの不透明度・描画モード・マスクは Genko のフォルダーに無いため外しました", name);
        }
        if (optional_field(content, "artboard") != nullptr) ctx.notes.name("アートボードは PC5 で入るまで普通のフォルダーにしました", name);
        layer.opacity = 1.0;
        layer.blend = "normal";
        layer.mask.reset();
        layer.vector_mask.reset();
        layer.clip = false;
        const bool shut = !layer.visible;
        read_layers(ctx, field(content, "children", "group"), Json(layer.id), shut, out);
        out.push_back(std::move(layer));
        return;
    }
    if (kind == "raster") {
        layer.kind = core::LayerKind::Raster;
        put_pixels(ctx, layer, surface_from(field(content, "surface", "raster")), name);
    } else if (kind == "adjustment") {
        std::string why;
        bool approximated = false;
        core::LookupTables made;
        std::optional<Json> adjust;
        try {
            adjust = adjust_of(field(content, "adjustment", "adjustment"), ctx.doc.page(0), made, why, approximated);
            if (adjust) adjust = psd::imported_adjust(*adjust, layer.blend, ctx.doc.page(0));
        } catch (const core::Error& error) {
            if (error.code() == "image_too_large" || error.code() == "format") throw;
            why = "値が読めない";
            adjust.reset();
        }
        if (!adjust) {
            ctx.notes.name("調整レイヤーは" + why + "ため外しました", name);
            return;
        }
        if (approximated) ctx.notes.name("トーンカーブの点が 19 を超えるため、等しい間隔の 19 点にしました", name);
        layer.kind = core::LayerKind::Adjust;
        layer.adjust = std::move(*adjust);
        core::lut3d::keep_luts(ctx.doc.luts, made, *layer.adjust);
        layer.blend = "normal";
    } else if (kind == "fill") {
        double alpha = 1.0;
        Json fill;
        try {
            fill = fill_of(ctx, field(content, "fill", "fill"), frame_of(ctx, layer), alpha);
        } catch (const core::Error& error) {
            throw Unreadable(error.what());
        }
        layer.kind = core::LayerKind::Fill;
        layer.fill = std::move(fill);
        layer.opacity = std::round(layer.opacity * alpha * 1000.0) / 1000.0;
    } else if (kind == "text") {
        if (!as_pixels(ctx, layer, optional_field(content, "cache"), name)) {
            ctx.notes.name("文字レイヤーは画素を持たないため外しました（文字として読めません）", name);
            return;
        }
        ctx.notes.name("文字レイヤーは画素にしました（文字として編集はできません）", name);
    } else if (kind == "shape") {
        std::optional<core::vector::ShapePart> part;
        try {
            part = shape_part(ctx, content);
        } catch (const Unreadable&) {
            part.reset();
        } catch (const core::Error& error) {
            if (error.code() == "image_too_large" || error.code() == "format") throw;
            part.reset();
        }
        if (part) {
            layer.kind = core::LayerKind::Strokes;
            layer.shapes = core::vector::make_shapes({std::move(*part)});
        } else if (as_pixels(ctx, layer, optional_field(content, "cache"), name)) {
            ctx.notes.name("シェイプレイヤーは Genko の形の部品にできないため画素にしました", name);
        } else {
            ctx.notes.name("シェイプレイヤーは Genko の形の部品にできず、画素も持たないため外しました", name);
            return;
        }
    } else if (kind == "smart") {
        std::string why;
        bool made = false;
        try {
            made = smart_layer(ctx, layer, content, name, why);
        } catch (const Unreadable&) {
            why = "スマートオブジェクトが読めないため画素にしました";
        }
        if (!made) {
            layer.smart.reset();
            if (!as_pixels(ctx, layer, optional_field(content, "cache"), name)) {
                ctx.notes.name("スマートオブジェクトは読めず、画素も持たないため外しました", name);
                return;
            }
            ctx.notes.name(why, name);
        }
    } else {
        throw Unreadable("a layer kind this build does not know (" + kind + ")");
    }
    out.push_back(std::move(layer));
}

void read_layers(Ctx& ctx, const Json& list, const Json& parent, bool hidden, std::vector<core::Layer>& out) {
    if (!list.is_array()) throw Unreadable("layers: not a list");
    for (const Json& m : list) {
        try {
            read_layer(ctx, m, parent, hidden, out);
        } catch (const Unreadable&) {
            ctx.notes.name("この版の Genko が読めない形のレイヤーを外しました", layer_name(m));
        }
    }
}

// --- the document ----------------------------------------------------------------------------------------------------

bool empty_value(const Json& v) {
    if (v.is_null()) return true;
    if (v.is_array()) return v.empty();
    if (v.is_object()) return std::all_of(v.begin(), v.end(), [](const Json& item) { return empty_value(item); });
    return false;
}

Read build(Loader& load, const Json& manifest, std::string_view title, int depth) {
    const Json& d = field(manifest, "document", "manifest");
    Ctx ctx{load};
    ctx.depth = depth;
    ctx.title = title.empty() ? std::string("無題") : std::string(title);
    const Json& size = field(d, "size", "document");
    const std::int64_t w = whole(field(size, "width", "size"), "size.width"), h = whole(field(size, "height", "size"), "size.height");
    if (w < 1 || h < 1) broken("a document of no pixels");
    ctx.mode = mode_from(field(d, "mode", "document"));
    ctx.sample = sample_from(field(d, "depth", "document"));
    codecs::Limits limits;
    limits.check(w, h, ctx.mode == Mode::Cmyk ? codecs::Layout::Cmyka : codecs::Layout::Rgba, ctx.sample == Sample::U8 ? codecs::Sample::U8 : ctx.sample == Sample::U16 ? codecs::Sample::U16 : codecs::Sample::F32);
    ctx.w = static_cast<int>(w);
    ctx.h = static_cast<int>(h);
    const double said = number(field(d, "resolution_dpi", "document"), "document.resolution_dpi");
    ctx.dpi = static_cast<double>(std::clamp<std::int64_t>(std::llround(said > 0 ? said : 72.0), 72, 100000));
    ctx.mm = 25.4 / ctx.dpi;

    // the page: the picture's paper at its resolution (formats/open_file's picture_book), its colour model the document's
    core::PageSpec spec;
    spec.width_mm = core::Num(static_cast<double>(w) * 25.4 / ctx.dpi);
    spec.height_mm = core::Num(static_cast<double>(h) * 25.4 / ctx.dpi);
    spec.dpi = core::Num(static_cast<std::int64_t>(ctx.dpi));
    spec.bleed_mm = core::Num(std::int64_t{0});
    spec.inner_margin_mm = core::Num(std::int64_t{0});
    spec.trim_w_mm = spec.width_mm;
    spec.trim_h_mm = spec.height_mm;
    spec.margins_mm = core::NumList{core::Num(0.0), core::Num(0.0), core::Num(0.0), core::Num(0.0)};
    const bool grey = ctx.mode == Mode::Grayscale || ctx.mode == Mode::Bitmap || ctx.mode == Mode::Duotone;
    spec.expression = grey ? "mono" : "color";
    const bool rgb = ctx.mode == Mode::Rgb || ctx.mode == Mode::Indexed || ctx.mode == Mode::Multichannel;
    if (rgb && ctx.sample == Sample::U8) spec.grid = "finest";  // (image-grid@1, as an opened picture's)
    ctx.doc = core::new_episode(ctx.title, core::Num(std::int64_t{1}), 1, spec, core::Binding::Left);
    {
        core::Page& page = ctx.page();
        page.frames.clear();
        page.numero = false;
        page.layers.clear();
    }
    core::Bytes icc;
    if (const Json* h_icc = optional_field(d, "icc_profile")) icc = load.blob(text(*h_icc, "document.icc_profile"));
    if (ctx.mode == Mode::Multichannel) ctx.notes.add("マルチチャンネルの原稿は RGB として開きました");
    if (rgb) {
        ctx.way = ctx.sample == Sample::U8 ? Way::Rgb8 : Way::Precise;
        std::optional<core::RgbMode> mode;
        const bool own = icc && ink::profile_space(*icc) == core::ProfileSpace::Rgb && !proof::same_as_srgb(*icc);
        if (icc && !own && !(ink::profile_space(*icc) == core::ProfileSpace::Rgb)) ctx.notes.add("埋め込みプロファイルが画像の色と合わないため、sRGB にしました");
        if (ctx.way == Way::Precise || own) {
            mode = core::RgbMode{};
            mode->depth = ctx.way == Way::Rgb8 ? 8 : ctx.sample == Sample::U16 ? 16 : 32;
            if (own) {
                mode->icc = icc;
                mode->profile = core::bytes_ref(*icc);
            }
        }
        ctx.page().rgb = mode;
        ctx.doc.rgb = mode;
    } else {
        ctx.way = Way::Inks;
        core::PageColor color;
        color.space = grey ? core::InkSpace::Gray : ctx.mode == Mode::Cmyk ? core::InkSpace::Cmyk : core::InkSpace::Lab;
        color.depth = ctx.sample == Sample::U8 ? 8 : ctx.sample == Sample::U16 ? 16 : 32;
        if (color.depth == 32 && color.space != core::InkSpace::Gray) {
            ctx.notes.add(std::string(color.space == core::InkSpace::Cmyk ? "CMYK" : "Lab") + " の 32 bit の原稿は 16 bit で開きました");
            color.depth = 16;
            ctx.sample = Sample::U16;
        }
        if (color.space != core::InkSpace::Lab) {
            const core::ProfileSpace wanted = grey ? core::ProfileSpace::Gray : core::ProfileSpace::Cmyk;
            core::Bytes profile = icc && ink::profile_space(*icc) == wanted ? icc : nullptr;
            if (icc && !profile) ctx.notes.add("埋め込みプロファイルが画像の色と合わないため、内蔵のプロファイルにしました");
            if (!profile) {
                const core::BuiltinProfile* builtin = core::builtin_profile(grey ? "sgray" : "coated-cmyk");
                if (builtin == nullptr || !builtin->bytes) throw core::Error("value", "a built-in profile is not in this build");
                profile = builtin->bytes;
            }
            color.icc = profile;
            color.profile = core::bytes_ref(*profile);
        }
        if (ctx.mode == Mode::Bitmap) {
            if (color.depth == 8) {
                core::BitmapLook look;
                look.method = "threshold";
                color.bitmap = look;
            } else {
                ctx.notes.add("2 階調の原稿はグレースケールで開きました");
            }
        }
        if (ctx.mode == Mode::Duotone) {
            if (const Json* duo = optional_field(d, "duotone")) {
                try {
                    for (const Json& ink : field(*duo, "inks", "duotone")) {
                        core::DuotoneInk made;
                        made.name = text(field(ink, "name", "duotone"), "duotone.name").substr(0, core::kInkNameMax);
                        made.color = rgb_array(rgb3(field(ink, "color", "duotone"), "duotone.color"));
                        made.curve.clear();
                        for (const Json& p : field(ink, "curve", "duotone")) {
                            made.curve.push_back({r4(number(field(p, "input", "duotone"), "duotone") * 100.0), r4(number(field(p, "output", "duotone"), "duotone") * 100.0)});
                        }
                        if (made.curve.size() < 2) made.curve = {{0.0, 0.0}, {100.0, 100.0}};
                        if (color.duotone.size() < core::kDuotoneInksMax && !made.name.empty()) color.duotone.push_back(std::move(made));
                    }
                } catch (const Unreadable&) {
                    color.duotone.clear();
                }
            }
            if (color.duotone.empty()) ctx.notes.add("ダブルトーンのインキが読めないため、グレースケールで開きました");
        }
        ctx.inks = color;
        ctx.page().color = color;
        ctx.doc.color = color;
    }

    take_patterns(ctx, optional_field(d, "patterns") ? d["patterns"] : Json::array());

    std::vector<core::Layer> layers;
    read_layers(ctx, field(d, "layers", "document"), Json(nullptr), false, layers);
    ctx.page().layers = std::move(layers);

    // its indexed colours (the page's table: the pixels are the document's, expanded)
    if (ctx.mode == Mode::Indexed && ctx.way == Way::Rgb8) {
        if (const Json* table = optional_field(d, "color_table")) {
            try {
                core::ColorTable made;
                for (const Json& c : field(*table, "colors", "color_table")) {
                    if (!c.is_array() || c.size() != 3) throw Unreadable("color_table: [r, g, b]");
                    made.colors.push_back({static_cast<int>(whole(c[0], "color_table")), static_cast<int>(whole(c[1], "color_table")), static_cast<int>(whole(c[2], "color_table"))});
                }
                if (const Json* t = optional_field(*table, "transparent")) made.transparent = static_cast<int>(whole(*t, "color_table.transparent"));
                core::check_color_table(made);
                core::RgbMode mode;
                mode.table = made;
                ctx.page().rgb = mode;
                ctx.doc.rgb = mode;
            } catch (const std::exception&) {
                ctx.notes.add("カラーテーブルが読めないため、RGB で開きました");
            }
        } else {
            ctx.notes.add("カラーテーブルが無いため、RGB で開きました");
        }
    }

    // its channels and Quick Mask (channels@1: GKIR masks of the page's pixels at its depth)
    const int channel_depth = core::page_depth(ctx.doc.page(0));
    const auto channel_of = [&](const Json& c, bool quick) -> std::optional<core::Channel> {
        core::Channel ch;
        ch.id = core::new_channel_id();
        std::string name = core::utf8_replaced(text(field(c, "name", "channel"), "channel.name"));
        try {
            name = core::channel_name(name.substr(0, 64 * 4));
        } catch (const core::Error&) {
            name = quick ? "クイックマスク" : core::next_channel_name(ctx.doc.page(0), core::ChannelKind::Alpha);
        }
        for (int n = 2; core::find_channel(ctx.doc.page(0), name) != nullptr; ++n) name = core::channel_name(name.substr(0, 50) + " " + std::to_string(n));
        ch.name = name;
        const Surface surface = surface_from(field(c, "surface", "channel"));
        bool cut = false;
        const std::vector<float> plane = grey_plane(ctx, surface, cut);
        core::InkPixels mask = core::blank_ink_pixels(core::InkSpace::Mask, channel_depth, static_cast<std::uint32_t>(ctx.w), static_cast<std::uint32_t>(ctx.h));
        bool any = false;
        for (std::size_t i = 0; i < plane.size(); ++i) {
            mask.set(i, 0, plane[i]);
            any = any || plane[i] != 0.0f;
        }
        if (any) ch.raster = std::make_shared<const std::string>(core::encode_ink_raster(mask));
        if (const Json* spot = optional_field(c, "spot")) {
            if (!spot->is_array() || spot->size() != 2) throw Unreadable("channel: spot is [colour, solidity]");
            ch.kind = core::ChannelKind::Spot;
            ch.color = rgb_array(srgb_of(ctx, color_from((*spot)[0])));
            ch.solidity = std::round(std::clamp(number((*spot)[1], "channel.spot"), 0.0, 1.0) * 1000.0) / 1000.0;
        } else {
            ch.kind = core::ChannelKind::Alpha;
            if (const Json* colour = optional_field(c, "color")) ch.color = rgb_array(srgb_of(ctx, color_from(*colour)));
            if (const Json* opacity = optional_field(c, "opacity")) ch.opacity = std::round(std::clamp(number(*opacity, "channel.opacity"), 0.0, 1.0) * 1000.0) / 1000.0;
            if (const Json* indicates = optional_field(c, "indicates")) ch.indicates = *indicates == Json("selectedAreas") ? "selected" : "masked";
            ch.quick_mask = quick;
        }
        return ch;
    };
    if (const Json* channels = optional_field(d, "channels"); channels != nullptr && channels->is_array()) {
        for (const Json& c : *channels) {
            if (ctx.doc.page(0).channels.size() >= core::kChannelsMax) {
                ctx.notes.add("チャンネルが多すぎるため、" + std::to_string(core::kChannelsMax) + " 個までにしました");
                break;
            }
            try {
                if (auto ch = channel_of(c, false)) ctx.page().channels.push_back(std::move(*ch));
            } catch (const Unreadable&) {
                ctx.notes.add("読めないチャンネルを外しました");
            }
        }
    }
    if (const Json* quick = optional_field(d, "quick_mask"); quick != nullptr && ctx.doc.page(0).channels.size() < core::kChannelsMax) {
        try {
            if (auto ch = channel_of(*quick, true)) ctx.page().channels.push_back(std::move(*ch));
        } catch (const Unreadable&) {
            ctx.notes.add("読めないクイックマスクを外しました");
        }
    }

    // its guides (rulers of kind guide, the page's mm)
    if (const Json* guides = optional_field(d, "guides")) {
        for (const auto& [key, axis] : {std::pair<const char*, const char*>{"horizontal", "h"}, {"vertical", "v"}}) {
            const Json* list = optional_field(*guides, key);
            if (list == nullptr || !list->is_array()) continue;
            for (const Json& at : *list) {
                const double v = number(at, "guides");
                ctx.page().rulers.push_back(Json{{"id", core::new_id()}, {"kind", "guide"}, {"axis", axis}, {"at", std::round(v * ctx.mm * 1000.0) / 1000.0}});
            }
        }
    }

    // its paths (vector@1: names one each, at most kPathsMax; the clipping path when it names a saved one)
    std::map<std::string, std::string> renamed;
    if (const Json* paths = optional_field(d, "paths"); paths != nullptr && paths->is_array()) {
        for (const Json& p : *paths) {
            if (ctx.doc.page(0).paths.size() >= core::vector::kPathsMax) break;
            try {
                std::string given = core::utf8_replaced(text(field(p, "name", "path"), "path.name"));
                std::string name;
                try {
                    name = core::vector::path_name(given.substr(0, 200));
                } catch (const core::Error&) {
                    name = "パス";
                }
                const std::string base = name;
                for (int n = 2; std::any_of(ctx.doc.page(0).paths.begin(), ctx.doc.page(0).paths.end(), [&](const core::vector::NamedPath& q) { return q.name == name; }); ++n)
                    name = base.substr(0, 56) + " " + std::to_string(n);
                renamed.emplace(given, name);
                ctx.page().paths.push_back(core::vector::NamedPath{name, path_from(field(p, "path", "path"), ctx.mm)});
            } catch (const Unreadable&) {
                ctx.notes.add("読めないパスを外しました");
            }
        }
    }
    if (const Json* work = optional_field(d, "work_path")) {
        try {
            ctx.page().work_path = path_from(*work, ctx.mm);
        } catch (const Unreadable&) {
            ctx.notes.add("読めない作業用パスを外しました");
        }
    }
    if (const Json* clip = optional_field(d, "clipping_path")) {
        const std::string name = optional_field(*clip, "name") ? text((*clip)["name"], "clipping_path.name") : std::string();
        if (const auto found = renamed.find(name); found != renamed.end()) {
            ctx.page().clipping_path = core::vector::ClippingPath{found->second, std::clamp(optional_field(*clip, "flatness") ? number((*clip)["flatness"], "clipping_path") : 0.0, 0.0, 100.0)};
        }
    }

    // what it was read from (file-info@1), its XMP's File Info, its EXIF, the PSD's resources and blocks it carried
    core::NativeSource source;
    source.format = "pcraft";
    source.name = ctx.title + ".pcraft";
    source.width_px = w;
    source.height_px = h;
    if (said > 0 && said <= 1000000) source.dpi = std::make_pair(said, said);
    source.depth = ctx.mode == Mode::Bitmap ? 1 : ctx.sample == Sample::U8 ? 8 : ctx.sample == Sample::U16 ? 16 : 32;
    source.mode = ctx.mode == Mode::Grayscale ? "gray"
                  : ctx.mode == Mode::Bitmap  ? "bitmap"
                  : ctx.mode == Mode::Duotone ? "duotone"
                  : ctx.mode == Mode::Indexed ? "indexed"
                  : ctx.mode == Mode::Cmyk    ? "cmyk"
                  : ctx.mode == Mode::Lab     ? "lab"
                  : ctx.mode == Mode::Multichannel ? "multichannel"
                                                   : "rgb";
    const auto keep = [&](const core::Bytes& bytes) {
        const std::string ref = core::bytes_ref(*bytes);
        ctx.doc.native_files[ref] = bytes;
        return ref;
    };
    core::PsdPageKeep psd;
    if (const Json* meta = optional_field(d, "metadata")) {
        if (const Json* xmp = optional_field(*meta, "xmp"); xmp != nullptr && xmp->is_string()) {
            source.xmp = keep(std::make_shared<const std::string>(xmp->get<std::string>()));
            core::FileInfo info = core::file_info_from_xmp(xmp->get<std::string>());
            info.xmp = source.xmp;
            try {
                core::check_file_info(info);
            } catch (const core::Error&) {
                info = core::FileInfo{};
                info.xmp = source.xmp;
            }
            ctx.doc.file_info = info;
        }
        if (const Json* exif = optional_field(*meta, "exif")) source.exif = keep(load.blob(text(*exif, "metadata.exif")));
        if (const Json* resources = optional_field(*meta, "psd_resources"); resources != nullptr && resources->is_array()) {
            for (const Json& r : *resources) {
                if (!r.is_array() || r.size() != 3 || psd.resources.size() >= core::kPsdKeptMax) continue;
                const std::int64_t id = whole(r[0], "psd_resources");
                std::string name = text(r[1], "psd_resources");
                if (id < 0 || id > 65535 || name.size() > 255) continue;
                psd.resources.push_back(core::PsdResource{static_cast<int>(id), std::move(name), keep(load.blob(text(r[2], "psd_resources")))});
            }
        }
        if (const Json* globals = optional_field(*meta, "psd_global_blocks"); globals != nullptr && globals->is_array()) {
            for (const Json& g : *globals) {
                if (!g.is_array() || g.size() != 3 || psd.globals.size() >= core::kPsdKeptMax) continue;
                const auto signature = key_from_hex(text(g[0], "psd_global_blocks")), key = key_from_hex(text(g[1], "psd_global_blocks"));
                if (!signature || !key || (*signature != "8BIM" && *signature != "8B64")) continue;
                if (!std::all_of(key->begin(), key->end(), [](char c) { return c >= 0x20 && c < 0x7f; })) continue;
                psd.globals.push_back(core::PsdGlobal{*signature, *key, keep(load.blob(text(g[2], "psd_global_blocks")))});
            }
        }
    }
    if (!psd.empty()) ctx.page().native_psd = std::move(psd);
    ctx.page().native_source = std::move(source);

    // what Genko has no place for yet (PC5's), said
    const auto said_of = [&](const char* key, const char* what) {
        if (const Json* v = optional_field(d, key); v != nullptr && !empty_value(*v)) ctx.notes.add(std::string(what) + "は Genko に無いため外しました");
    };
    said_of("layer_comps", "レイヤーカンプ");
    said_of("notes", "注釈");
    said_of("slices", "スライス");
    said_of("timeline", "タイムライン");
    if (const Json* variables = optional_field(d, "variables"); variables != nullptr && !empty_value(*variables)) ctx.notes.add("変数とデータセットは Genko に無いため外しました");
    if (const Json* measurement = optional_field(d, "measurement")) {
        const Json* counts = optional_field(*measurement, "counts");
        const Json* ruler = optional_field(*measurement, "ruler");
        if ((counts != nullptr && !empty_value(*counts)) || ruler != nullptr) ctx.notes.add("計測は Genko に無いため外しました");
    }
    if (const Json* styles = optional_field(d, "text_styles"); styles != nullptr && !empty_value(*styles)) ctx.notes.add("文字スタイルは Genko に無いため外しました");

    // the book as a book must be (what an op leaves is checked so too)
    try {
        core::validate_color_document(ctx.doc);
        core::validate_ink_document(ctx.doc);
        core::validate_color_model(ctx.doc);
        core::validate_channels(ctx.doc);
        core::validate_adjustments(ctx.doc);
        core::smart::validate_smart(ctx.doc);
        core::validate_masks(ctx.doc);
        core::vector::validate_vector(ctx.doc);
        core::validate_patterns(ctx.doc);
        core::validate_pattern_fills(ctx.doc);
        core::validate_psd_keep(ctx.doc);
        core::validate_file_info(ctx.doc);
    } catch (const core::Error& error) {
        if (error.code() == "not_yet_ported") throw;
        broken(std::string("it does not make a Genko page: ") + error.what());
    }
    Read out;
    out.doc = std::move(ctx.doc);
    out.notes = ctx.notes.take();
    return out;
}

Read read_from(const Source& source, std::string_view title, const ReadLimits& limits) {
    Loader load(source, limits);
    const Json manifest = parse_manifest(source, limits);
    try {
        Read out = build(load, manifest, title, limits.depth);
        out.unpacked = load.unpacked();
        return out;
    } catch (const Unreadable& error) {
        broken(std::string("its document cannot be read: ") + error.what());
    }
}

}  // namespace

bool is_pcraft(std::string_view bytes) {
    if (bytes.size() <= 30 || bytes.substr(0, 4) != std::string_view("PK\x03\x04", 4)) return false;
    const std::size_t n = static_cast<unsigned char>(bytes[26]) | static_cast<std::size_t>(static_cast<unsigned char>(bytes[27])) << 8;
    return bytes.size() >= 30 + n && bytes.substr(30, n) == kManifest;
}

Read read(std::string_view bytes, std::string_view title, const ReadLimits& limits) {
    const ZipSource source(bytes);
    return read_from(source, title, limits);
}

Read read_folder(const std::filesystem::path& folder, std::string_view title, const ReadLimits& limits) {
    const FolderSource source(folder);
    return read_from(source, title, limits);
}

Json read_manifest(std::string_view bytes) {
    const ZipSource source(bytes);
    return parse_manifest(source, ReadLimits{});
}

namespace {

std::mutex remembered_lock;
std::optional<std::pair<std::string, Read>> remembered;

}  // namespace

void remember_read(const std::string& ref, Read read) {
    const std::lock_guard<std::mutex> hold(remembered_lock);
    remembered.emplace(ref, std::move(read));
}

std::string place_as_contents(core::Document& doc, std::string_view bytes, std::string_view name, std::vector<std::string>& notes, const ReadLimits& limits) {
    std::string stem(name);
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    std::optional<Read> kept;
    {
        const std::lock_guard<std::mutex> hold(remembered_lock);
        if (remembered && limits.depth == 0 && remembered->first == core::bytes_ref(bytes)) {
            kept = std::move(remembered->second);
            remembered.reset();
        }
    }
    Read read = kept ? std::move(*kept) : pcraft::read(bytes, stem, limits);
    for (std::string& note : read.notes) notes.push_back(std::move(note));
    core::Page page = read.doc.page(0);
    page.index = core::Num(std::int64_t{1});
    page.native_source.reset();
    core::smart::bring_contents(doc, read.doc, page);
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "page";
    contents->name = std::string(name).substr(0, core::smart::kNameMax);
    contents->page = std::make_shared<core::Page>(std::move(page));
    const std::string id = smart_ops::fresh_contents_id(doc);
    doc.smart_contents[id] = std::move(contents);
    return id;
}

}  // namespace genko::render::pcraft
