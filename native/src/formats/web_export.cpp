// Web 用に保存（従来）…, 書き出しの環境設定… and クイック書き出し (formats/web_export.hpp; PhotoCraft v0.2.0 web_cmds.rs, MIT).

#include "formats/web_export.hpp"

#include <QString>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "core/slice_layout.hpp"
#include "render/codecs/codecs.hpp"
#include "render/ops_slices.hpp"
#include "render/op_limits.hpp"

namespace genko::formats {

namespace {

namespace fs = std::filesystem;
using core::Json;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;
using render::quantize::Dither;
using render::quantize::PaletteKind;
using render::quantize::Rgb8;
using render::quantize::Rgba;

constexpr std::array<std::string_view, 12> kPresets{"GIF 128 Dithered", "GIF 128 No Dither", "GIF 32 Dithered", "GIF 32 No Dither",
                                                    "GIF 64 Dithered",  "GIF 64 No Dither",  "GIF Restrictive", "JPEG High",
                                                    "JPEG Low",         "JPEG Medium",       "PNG-24",          "PNG-8 128 Dithered"};

std::uint8_t to8(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

std::optional<std::array<float, 3>> hex3(std::string_view text) {
    if (!text.empty() && text[0] == '#') text.remove_prefix(1);
    if (text.size() != 6) return std::nullopt;
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        int v = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const char c = text[i * 2 + k];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * 16 + d;
        }
        out[i] = float(v) / 255.0f;
    }
    return out;
}

const Json* field(const Json& params, const char* key) {
    if (!params.is_object()) return nullptr;
    const auto it = params.find(key);
    return it == params.end() || it->is_null() ? nullptr : &*it;
}

std::string text_of(const Json& params, const char* key) {
    const Json* v = field(params, key);
    if (v == nullptr) return {};
    if (!v->is_string()) throw core::Error("value", std::string(key) + " is a string");
    return v->get<std::string>();
}

std::optional<double> number_of(const Json& params, const char* key) {
    const Json* v = field(params, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_number() || v->is_boolean()) throw core::Error("value", std::string(key) + " is a number");
    const double d = v->get<double>();
    if (!std::isfinite(d)) throw core::Error("value", std::string(key) + " is a number");
    return d;
}

std::optional<bool> flag_of(const Json& params, const char* key) {
    const Json* v = field(params, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_boolean()) throw core::Error("value", std::string(key) + " is true or false");
    return v->get<bool>();
}

// Nearest palette entry through a lazily filled 6-bit-per-channel inverse colour map (PhotoCraft's InverseMap): each
// 4×4×4 cell resolved once by its centre — a table lookup a pixel, not a palette scan —; `t` (the transparent entry)
// never returned.
class InverseMap {
public:
    InverseMap(const std::vector<Rgb8>& palette, std::optional<std::size_t> t) : palette_(palette), t_(t), lut_(std::size_t(1) << 18, 0xffff) {}
    std::size_t get(const std::array<float, 3>& c) {
        std::array<std::size_t, 3> q{};
        for (std::size_t k = 0; k < 3; ++k) q[k] = static_cast<std::size_t>(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f)) >> 2;
        const std::size_t key = q[0] << 12 | q[1] << 6 | q[2];
        if (lut_[key] != 0xffff) return lut_[key];
        float best = std::numeric_limits<float>::max();
        std::size_t at = 0;
        for (std::size_t i = 0; i < palette_.size(); ++i) {
            if (t_ && i == *t_) continue;
            float d = 0.0f;
            for (std::size_t k = 0; k < 3; ++k) {
                const float e = float(q[k] * 4 + 2) - float(palette_[i][k]);
                d += e * e;
            }
            if (d < best) {
                best = d;
                at = i;
            }
        }
        lut_[key] = static_cast<std::uint16_t>(at);
        return at;
    }

private:
    const std::vector<Rgb8>& palette_;
    std::optional<std::size_t> t_;
    std::vector<std::uint16_t> lut_;
};

float hash01(std::size_t x, std::size_t y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ 0x2545f491u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65535.0f;
}

// What a pixel becomes before its colours are reduced (PhotoCraft's work picture, made pixel by pixel here — never held as
// floats —): under ½ alpha with transparency on, clear; with transparency and no matte, its colour opaque; else over the
// matte.
struct Work {
    const Rgba8& picture;
    const WebSettings& st;
    std::array<float, 3> matte;
    Rgba at(std::size_t i) const {
        const std::uint8_t* p = picture.px.data() + i * 4;
        const float a = float(p[3]) / 255.0f;
        const std::array<float, 3> c{float(p[0]) / 255.0f, float(p[1]) / 255.0f, float(p[2]) / 255.0f};
        if (st.transparency && a < 0.5f) return {0.0f, 0.0f, 0.0f, 0.0f};
        if (st.transparency && !st.matte) return {c[0], c[1], c[2], 1.0f};
        return {c[0] * a + matte[0] * (1.0f - a), c[1] * a + matte[1] * (1.0f - a), c[2] * a + matte[2] * (1.0f - a), 1.0f};
    }
};

// Palette mapping with the dither (Floyd–Steinberg diffusion, 8×8 Bayer pattern, noise): PhotoCraft's quantize_band over
// the whole picture; a pixel under ½ alpha takes `t`.
std::vector<std::uint8_t> map_colours(const Work& work, std::size_t w, std::size_t h, const std::vector<Rgb8>& pal, Dither dither, float amount,
                                      std::optional<std::size_t> t) {
    InverseMap map(pal, t);
    std::vector<std::uint8_t> idx(w * h, 0);
    const std::size_t opaque_n = pal.size() - (t ? 1 : 0);
    const float a = std::clamp(amount, 0.0f, 1.0f);
    const float spread = std::min(1.0f / std::max(1.0f, std::cbrt(float(opaque_n))), 0.5f) * a;
    std::vector<std::array<float, 3>> cur(dither == Dither::Diffusion ? w + 2 : 0), next(cur.size());
    for (std::size_t y = 0; y < h; ++y) {
        if (dither == Dither::Diffusion) {
            std::swap(cur, next);
            std::fill(next.begin(), next.end(), std::array<float, 3>{0.0f, 0.0f, 0.0f});
        }
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            const Rgba p = work.at(i);
            if (p[3] < 0.5f && t) {
                idx[i] = static_cast<std::uint8_t>(*t);
                continue;
            }
            std::array<float, 3> c{p[0], p[1], p[2]};
            switch (dither) {
            case Dither::None: break;
            case Dither::Diffusion:
                for (std::size_t k = 0; k < 3; ++k) c[k] += cur[x + 1][k];
                break;
            case Dither::Pattern: {
                const float d = render::quantize::bayer8(x, y) * spread;
                for (float& v : c) v += d;
                break;
            }
            case Dither::Noise: {
                const float d = (hash01(x, y) - 0.5f) * spread;
                for (float& v : c) v += d;
                break;
            }
            }
            for (float& v : c) v = std::clamp(v, 0.0f, 1.0f);
            const std::size_t k = map.get(c);
            idx[i] = static_cast<std::uint8_t>(k);
            if (dither == Dither::Diffusion) {
                for (std::size_t q = 0; q < 3; ++q) {
                    const float d = (c[q] - float(pal[k][q]) / 255.0f) * a;
                    cur[x + 2][q] += d * 7.0f / 16.0f;
                    next[x][q] += d * 3.0f / 16.0f;
                    next[x + 1][q] += d * 5.0f / 16.0f;
                    next[x + 2][q] += d / 16.0f;
                }
            }
        }
    }
    return idx;
}

}  // namespace

std::uint64_t estimated_bytes(const Rgba8& proxy, const WebSettings& st, const std::optional<std::string>& icc, const std::optional<std::string>& xmp,
                              double dpi, std::uint64_t bytes, double ratio, const std::optional<std::string>& exif) {
    // (what the file has whatever its size — headers, a palette, a profile, metadata — from the same file of the proxy's
    // first pixel, not scaled with the pixels)
    Rgba8 one;
    one.width = 1;
    one.height = 1;
    one.px.assign(proxy.px.begin(), proxy.px.begin() + std::min<std::size_t>(4, proxy.px.size()));
    std::uint64_t fixed = 0;
    if (one.px.size() == 4) fixed = std::min<std::uint64_t>(bytes, optimize(one, st, icc, xmp, dpi, false, nullptr, exif).bytes);
    return fixed + static_cast<std::uint64_t>(std::llround(double(bytes - fixed) * ratio));
}

std::optional<WebFormat> web_format_of(std::string_view id) {
    std::string s(id);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "gif") return WebFormat::Gif;
    if (s == "png8" || s == "png-8") return WebFormat::Png8;
    if (s == "png24" || s == "png-24" || s == "png" || s == "png32") return WebFormat::Png24;
    if (s == "jpeg" || s == "jpg") return WebFormat::Jpeg;
    if (s == "wbmp") return WebFormat::Wbmp;
    return std::nullopt;
}

std::string_view to_string(WebFormat format) {
    switch (format) {
    case WebFormat::Gif: return "gif";
    case WebFormat::Png8: return "png8";
    case WebFormat::Png24: return "png24";
    case WebFormat::Jpeg: return "jpeg";
    case WebFormat::Wbmp: return "wbmp";
    }
    return "png24";
}

std::string_view web_extension(WebFormat format) {
    switch (format) {
    case WebFormat::Gif: return "gif";
    case WebFormat::Png8:
    case WebFormat::Png24: return "png";
    case WebFormat::Jpeg: return "jpg";
    case WebFormat::Wbmp: return "wbmp";
    }
    return "png";
}

std::span<const std::string_view> web_presets() { return kPresets; }

std::optional<WebSettings> web_preset(std::string_view name) {
    const WebSettings d;
    const auto gif = [](std::size_t colors, bool dither) {
        WebSettings s;
        s.format = WebFormat::Gif;
        s.colors = colors;
        s.dither = dither ? Dither::Diffusion : Dither::None;
        return s;
    };
    const auto jpeg = [&](int quality) {
        WebSettings s = d;
        s.format = WebFormat::Jpeg;
        s.quality = quality;
        return s;
    };
    if (name == "GIF 128 Dithered") return gif(128, true);
    if (name == "GIF 128 No Dither") return gif(128, false);
    if (name == "GIF 32 Dithered") return gif(32, true);
    if (name == "GIF 32 No Dither") return gif(32, false);
    if (name == "GIF 64 Dithered") return gif(64, true);
    if (name == "GIF 64 No Dither") return gif(64, false);
    if (name == "GIF Restrictive") {
        WebSettings s = gif(256, true);
        s.palette = PaletteKind::Web;
        return s;
    }
    if (name == "JPEG High") return jpeg(60);
    if (name == "JPEG Low") return jpeg(10);
    if (name == "JPEG Medium") return jpeg(30);
    if (name == "JPEG Maximum") return jpeg(100);
    if (name == "JPEG Very High") return jpeg(80);
    if (name == "PNG-24") return d;
    if (name == "PNG-8 128 Dithered") {
        WebSettings s = d;
        s.format = WebFormat::Png8;
        s.colors = 128;
        return s;
    }
    return std::nullopt;
}

WebSettings web_settings_from_json(const Json& params) {
    if (!params.is_null() && !params.is_object()) throw core::Error("value", "the Save for Web settings are an object");
    WebSettings st;
    if (const std::string preset = text_of(params, "preset"); !preset.empty()) {
        const auto found = web_preset(preset);
        if (!found) throw core::Error("value", "unknown preset `" + preset + "`");
        st = *found;
    }
    if (const std::string f = text_of(params, "format"); !f.empty()) {
        const auto found = web_format_of(f);
        if (!found) throw core::Error("value", "unknown format `" + f + "` (gif|png8|png24|jpeg|wbmp)");
        st.format = *found;
    }
    if (const std::string k = text_of(params, "palette"); !k.empty()) {
        const auto found = k == "restrictive" || k == "web" ? std::optional<PaletteKind>(PaletteKind::Web) : render::quantize::palette_kind(k);
        if (!found) throw core::Error("value", "unknown palette `" + k + "` (perceptual|selective|adaptive|restrictive|exact|systemMac|systemWindows|uniform)");
        st.palette = *found;
    }
    if (const auto n = number_of(params, "colors"); n && *n > 0) st.colors = static_cast<std::size_t>(std::clamp(std::trunc(*n), 2.0, 256.0));
    if (const std::string d = text_of(params, "dither"); !d.empty()) {
        if (d == "none" || d == "noDither") st.dither = Dither::None;
        else if (d == "diffusion") st.dither = Dither::Diffusion;
        else if (d == "pattern") st.dither = Dither::Pattern;
        else if (d == "noise") st.dither = Dither::Noise;
        else throw core::Error("value", "unknown dither `" + d + "` (none|diffusion|pattern|noise)");
    }
    if (const auto a = number_of(params, "ditherAmount")) st.dither_amount = std::clamp(float(*a) / 100.0f, 0.0f, 1.0f);
    if (const auto b = flag_of(params, "transparency")) st.transparency = *b;
    if (const std::string m = text_of(params, "matte"); !m.empty()) {
        if (m == "none") {
            st.matte.reset();
        } else {
            const auto c = hex3(m);
            if (!c) throw core::Error("value", "matte is \"none\" or \"#rrggbb\"");
            st.matte = *c;
        }
    }
    for (const auto& [key, slot] : {std::pair<const char*, bool*>{"interlaced", &st.interlaced}, {"progressive", &st.progressive},
                                    {"optimized", &st.optimized}, {"embedIcc", &st.embed_icc}, {"convertToSrgb", &st.convert_to_srgb}}) {
        if (const auto b = flag_of(params, key)) *slot = *b;
    }
    if (const auto w = number_of(params, "webSnap")) st.web_snap = std::clamp(float(*w) / 100.0f, 0.0f, 1.0f);
    if (const auto q = number_of(params, "quality")) st.quality = static_cast<int>(std::lround(std::clamp(*q, 0.0, 100.0)));
    if (const std::string m = text_of(params, "metadata"); !m.empty()) {
        if (m == "none") st.metadata = WebMetadata::None;
        else if (m == "copyright") st.metadata = WebMetadata::Copyright;
        else if (m == "copyrightAndContact" || m == "contact") st.metadata = WebMetadata::CopyrightAndContact;
        else if (m == "all" || m == "allExceptCameraInfo") st.metadata = WebMetadata::All;
        else throw core::Error("value", "unknown metadata `" + m + "` (none|copyright|copyrightAndContact|all)");
    }
    return st;
}

Json to_json(const WebSettings& st) {
    const char* palette = "selective";
    switch (st.palette) {
    case PaletteKind::Exact: palette = "exact"; break;
    case PaletteKind::SystemMac: palette = "systemMac"; break;
    case PaletteKind::SystemWindows: palette = "systemWindows"; break;
    case PaletteKind::Web: palette = "restrictive"; break;
    case PaletteKind::Uniform: palette = "uniform"; break;
    case PaletteKind::Perceptual: palette = "perceptual"; break;
    case PaletteKind::Selective: palette = "selective"; break;
    case PaletteKind::Adaptive: palette = "adaptive"; break;
    }
    const char* dither = st.dither == Dither::None ? "none" : st.dither == Dither::Diffusion ? "diffusion" : st.dither == Dither::Pattern ? "pattern" : "noise";
    const char* metadata = st.metadata == WebMetadata::None        ? "none"
                           : st.metadata == WebMetadata::Copyright ? "copyright"
                           : st.metadata == WebMetadata::All       ? "all"
                                                                   : "copyrightAndContact";
    std::string matte = "none";
    if (st.matte) {
        static constexpr char kHex[] = "0123456789abcdef";
        matte = "#";
        for (const float v : *st.matte) {
            const std::uint8_t b = to8(v);
            matte += kHex[b >> 4];
            matte += kHex[b & 15];
        }
    }
    Json out = Json::object();
    out["format"] = std::string(to_string(st.format));
    out["palette"] = palette;
    out["colors"] = static_cast<std::int64_t>(st.colors);
    out["dither"] = dither;
    out["ditherAmount"] = std::round(st.dither_amount * 100.0);
    out["transparency"] = st.transparency;
    out["matte"] = matte;
    out["interlaced"] = st.interlaced;
    out["webSnap"] = std::round(st.web_snap * 100.0);
    out["quality"] = static_cast<std::int64_t>(st.quality);
    out["progressive"] = st.progressive;
    out["optimized"] = st.optimized;
    out["embedIcc"] = st.embed_icc;
    out["metadata"] = metadata;
    out["convertToSrgb"] = st.convert_to_srgb;
    return out;
}

WebSize web_size_from_json(const Json& params) {
    WebSize size;
    if (const auto p = number_of(params, "percent"); p && *p > 0) size.percent = *p;
    for (const auto& [key, slot] : {std::pair<const char*, std::optional<std::uint32_t>*>{"width", &size.width}, {"height", &size.height}}) {
        if (const auto v = number_of(params, key); v && *v > 0) {
            if (*v > double(render::limits::kSide)) throw core::Error("value", std::string(key) + " is 1 to 100000 pixels");
            *slot = static_cast<std::uint32_t>(std::max(1.0, std::round(*v)));
        }
    }
    if (const std::string r = text_of(params, "resample"); !r.empty()) {
        const auto found = render::codecs::resize_filter(r);
        if (!found) throw core::Error("value", "unknown resample `" + r + "` (bicubic|bilinear|nearest)");
        size.resample = *found;
    }
    return size;
}

Optimized optimize(const Rgba8& picture, const WebSettings& st, const std::optional<std::string>& icc, const std::optional<std::string>& xmp, double dpi,
                   bool preview, const render::codecs::web::Sink* sink, const std::optional<std::string>& exif) {
    const std::size_t w = picture.width, h = picture.height, n = w * h;
    if (w == 0 || h == 0 || picture.px.size() != n * 4) throw core::Error("value", "a picture of no pixels");
    Optimized out;
    out.width = picture.width;
    out.height = picture.height;
    out.ext = std::string(web_extension(st.format));
    const render::codecs::web::Sink count = [&out, sink](std::string_view bytes) {
        out.bytes += bytes.size();
        if (sink != nullptr) (*sink)(bytes);
    };
    const std::array<float, 3> matte = st.matte.value_or(std::array<float, 3>{1.0f, 1.0f, 1.0f});
    const Work work{picture, st, matte};
    const auto over = [&](std::size_t i) -> std::array<std::uint8_t, 3> {
        const std::uint8_t* p = picture.px.data() + i * 4;
        const float a = float(p[3]) / 255.0f;
        std::array<std::uint8_t, 3> c{};
        for (std::size_t k = 0; k < 3; ++k) c[k] = to8(float(p[k]) / 255.0f * a + matte[k] * (1.0f - a));
        return c;
    };
    switch (st.format) {
    case WebFormat::Png24:
    case WebFormat::Jpeg: {
        bool alpha = false;
        if (st.format == WebFormat::Png24 && st.transparency) {
            for (std::size_t i = 0; i < n && !alpha; ++i) alpha = picture.px[i * 4 + 3] < 255;
        }
        const unsigned k = alpha ? 4 : 3;
        std::string data(n * k, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            if (alpha) {
                std::copy_n(picture.px.data() + i * 4, 4, reinterpret_cast<std::uint8_t*>(data.data()) + i * 4);
            } else {
                const auto c = over(i);
                std::copy_n(c.data(), 3, reinterpret_cast<std::uint8_t*>(data.data()) + i * 3);
            }
        }
        CodecImage image = CodecImage::from_bytes(picture.width, picture.height, alpha ? Layout::Rgba : Layout::Rgb, Sample::U8, std::move(data));
        if (st.embed_icc && icc && !icc->empty()) image.icc = *icc;
        image.meta.dpi = std::make_pair(dpi, dpi);
        image.meta.xmp = xmp;
        if (st.format == WebFormat::Jpeg) image.meta.exif = exif;
        render::codecs::EncodeOptions encode;
        encode.embed_icc = st.embed_icc && icc.has_value();
        encode.embed_metadata = true;
        encode.png_interlaced = st.interlaced;
        encode.png_compression = render::codecs::PngCompression::Best;
        encode.jpeg_quality = std::max(1, st.quality);
        encode.jpeg_chroma_subsampling = st.quality < 90;
        encode.jpeg_progressive = st.progressive;
        encode.jpeg_optimize = st.optimized || st.progressive;
        const std::string bytes = render::codecs::encode(image, st.format == WebFormat::Jpeg ? render::codecs::Format::Jpeg : render::codecs::Format::Png, encode);
        count(bytes);
        if (preview) {
            if (st.format == WebFormat::Jpeg) {  // (the preview shows the compression)
                const CodecImage back = render::codecs::decode(bytes).convert(Layout::Rgba, Sample::U8);
                out.preview.assign(back.data().begin(), back.data().end());
            } else if (alpha) {
                out.preview = picture.px;
            } else {
                out.preview.resize(n * 4);
                for (std::size_t i = 0; i < n; ++i) {
                    const auto c = over(i);
                    std::copy_n(c.data(), 3, out.preview.data() + i * 4);
                    out.preview[i * 4 + 3] = 255;
                }
            }
        }
        return out;
    }
    case WebFormat::Wbmp: {
        std::vector<float> grey(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto c = over(i);
            grey[i] = 0.299f * c[0] / 255.0f + 0.587f * c[1] / 255.0f + 0.114f * c[2] / 255.0f;
        }
        render::quantize::BitmapMethod method;
        method.kind = st.dither == Dither::None      ? render::quantize::BitmapMethod::Kind::Threshold
                      : st.dither == Dither::Pattern ? render::quantize::BitmapMethod::Kind::Pattern
                                                     : render::quantize::BitmapMethod::Kind::Diffusion;
        render::quantize::to_bitmap(grey, w, method);
        std::vector<std::uint8_t> white(n);
        for (std::size_t i = 0; i < n; ++i) white[i] = grey[i] > 0.5f ? 1 : 0;
        grey = {};
        if (preview) {
            out.preview.resize(n * 4);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t v = white[i] ? 255 : 0;
                out.preview[i * 4] = out.preview[i * 4 + 1] = out.preview[i * 4 + 2] = v;
                out.preview[i * 4 + 3] = 255;
            }
        }
        render::codecs::web::write_wbmp(picture.width, picture.height, white, count);
        out.colors = 2;
        return out;
    }
    case WebFormat::Gif:
    case WebFormat::Png8: break;
    }
    // GIF and PNG-8: a palette of the picture (from an even sample of about a million pixels; Exact from all of them),
    // Web Snap, the transparent entry, the pixels mapped and dithered
    bool transparent = false;
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const bool clear = picture.px[i * 4 + 3] < 128;  // (alpha under ½)
        if (clear && st.transparency) transparent = true;
        if (work.at(i)[3] >= 0.5f) ++opaque;
    }
    const std::size_t room = transparent ? std::max<std::size_t>(st.colors > 0 ? st.colors - 1 : 0, 2) : st.colors;
    const std::size_t step = opaque > (std::size_t(1) << 20) && st.palette != PaletteKind::Exact ? opaque / (std::size_t(1) << 20) + 1 : 1;
    std::vector<Rgba> sample;
    sample.reserve(std::min(opaque / step + 1, n));
    for (std::size_t i = 0, seen = 0; i < n; ++i) {
        const Rgba p = work.at(i);
        if (p[3] < 0.5f) continue;
        if (seen++ % step == 0) sample.push_back(p);
    }
    std::vector<Rgb8> pal = render::quantize::build_palette(sample, st.palette, room, render::quantize::Forced::None);
    sample = {};
    if (pal.empty()) pal.push_back({0, 0, 0});  // (nothing shows: one entry)
    if (st.web_snap > 0.0f) {
        const auto tol = static_cast<int>(st.web_snap * 255.0f);
        for (Rgb8& c : pal) {
            Rgb8 snap{};
            bool near = true;
            for (std::size_t k = 0; k < 3; ++k) {
                snap[k] = static_cast<std::uint8_t>(std::lround(float(c[k]) / 51.0f) * 51);
                if (std::abs(int(c[k]) - int(snap[k])) > tol) near = false;
            }
            if (near) c = snap;
        }
    }
    if (transparent && pal.size() > 255) pal.resize(255);
    std::optional<std::size_t> t;
    if (transparent) {
        pal.push_back(pal[0]);
        t = pal.size() - 1;
    }
    const std::vector<std::uint8_t> idx = map_colours(work, w, h, pal, st.dither, st.dither_amount, t);
    if (preview) {
        out.preview.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            const Rgb8& c = pal[idx[i]];
            std::copy_n(c.data(), 3, out.preview.data() + i * 4);
            out.preview[i * 4 + 3] = t && idx[i] == *t ? 0 : 255;
        }
    }
    const std::optional<std::uint8_t> tt = t ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*t)) : std::nullopt;
    if (st.format == WebFormat::Gif) {
        render::codecs::web::write_gif_indexed(picture.width, picture.height, idx, pal, tt, st.interlaced, count);
    } else {
        render::codecs::web::write_png_indexed(picture.width, picture.height, idx, pal, tt, st.interlaced, std::nullopt, count);
    }
    out.colors = pal.size();
    return out;
}

WebPicture web_picture(const core::Document& doc, std::size_t page, const WebSettings& st, const WebSize& size, const std::optional<std::string>& layer,
                       std::optional<std::uint32_t> proxy) {
    CopyOptions copy;
    if (size.percent) copy.scale = *size.percent;
    copy.width = size.width;
    copy.height = size.height;
    copy.resample = size.resample;
    copy.layer = layer;
    copy.metadata = Metadata::None;  // (Save for Web's own: web_xmp)
    WebPicture out;
    const CopySize full = copy_size(doc, page, copy);
    out.full_width = full.width;
    out.full_height = full.height;
    // (a page larger than a GIF or a JPEG holds: refused before it is drawn)
    if (!layer && st.format == WebFormat::Gif && (full.width > 65535 || full.height > 65535)) {
        throw core::Error("image_too_large", "GIF holds at most 65535×65535 pixels");
    }
    if (!layer && st.format == WebFormat::Jpeg) {
        if (const auto most = render::codecs::max_dimensions(render::codecs::Format::Jpeg); most && (full.width > most->first || full.height > most->second)) {
            throw core::Error("image_too_large", "JPEG holds at most " + std::to_string(most->first) + "×" + std::to_string(most->second) + " pixels");
        }
    }
    const bool proxied = proxy && *proxy > 0 && std::max(full.width, full.height) > *proxy;
    if (proxied) {
        const double k = double(*proxy) / double(std::max(full.width, full.height));
        copy.scale = std::clamp(copy.scale * k, 1.0, 1000.0);
        if (copy.width) copy.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(*copy.width * k)));
        if (copy.height) copy.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(*copy.height * k)));
    }
    PagePicture picture = copy_picture(doc, page, copy);
    out.notes = std::move(picture.notes);
    out.dpi = picture.image.meta.dpi ? picture.image.meta.dpi->first : 72.0;
    CodecImage image = std::move(picture.image);
    const bool profiled = image.icc && !image.icc->empty();
    // (a page of a profile with its colours kept — sRGB's not asked — keeps them and its profile; any other — a grey or
    // CMYK page among them — goes to sRGB through Little CMS, as every way out converts it)
    if (render::codecs::is_rgb(image.layout()) && !(st.convert_to_srgb && profiled)) {
        out.icc = profiled ? image.icc : std::nullopt;
        // (PC4 F-c's review P3-12: its values written without the profile they mean — a GIF, a PNG-8 or a WBMP holds none,
        // a JPEG or a PNG-24 not embedding it —: said, as Photoshop's own would not)
        const bool holds = (st.format == WebFormat::Jpeg || st.format == WebFormat::Png24) && st.embed_icc;
        if (profiled && !holds) out.notes.emplace_back("カラープロファイルを持たないファイルに、sRGB に変換せずにページの値をそのまま書きます（色が変わって見えることがあります）");
    } else if (profiled || !render::codecs::is_rgb(image.layout())) {
        image = srgb_picture(image, false, false);
        out.icc = std::nullopt;
    }
    const CodecImage rgba = image.convert(Layout::Rgba, Sample::U8);
    image = CodecImage();
    out.rgba.width = rgba.width();
    out.rgba.height = rgba.height();
    out.rgba.px.assign(rgba.data().begin(), rgba.data().end());
    if (st.format == WebFormat::Jpeg && st.metadata == WebMetadata::All) {
        // (the size the file is written at: the picture's, or — a proxy — the page's)
        const bool page_size = proxied && !layer;
        out.exif = copy_exif(doc, doc.page(page), page_size ? full.width : out.rgba.width, page_size ? full.height : out.rgba.height);
    }
    return out;
}

std::optional<std::string> web_xmp(const core::Document& doc, WebMetadata metadata) {
    if (metadata == WebMetadata::None) return std::nullopt;
    if (metadata == WebMetadata::All) return book_xmp(doc);
    if (!doc.file_info) return std::nullopt;
    core::FileInfo kept;
    kept.copyright = doc.file_info->copyright;
    kept.copyright_status = doc.file_info->copyright_status;
    kept.copyright_url = doc.file_info->copyright_url;
    if (metadata == WebMetadata::CopyrightAndContact) {
        kept.author = doc.file_info->author;
        kept.author_title = doc.file_info->author_title;
    }
    if (kept.empty()) return std::nullopt;
    return core::write_xmp(std::nullopt, kept, std::nullopt);
}

WebResult save_for_web(const core::Document& doc, std::size_t page, const WebSettings& st, const WebSize& size, const std::optional<fs::path>& dest,
                       const std::optional<std::string>& layer, std::optional<std::uint32_t> proxy) {
    WebPicture picture = web_picture(doc, page, st, size, layer, dest ? std::nullopt : proxy);
    // (embedded: the page's own profile, or — converted, and a profile asked — sRGB's)
    std::optional<std::string> icc = picture.icc;
    if (st.embed_icc && !icc) icc = srgb_icc_once();
    const std::optional<std::string> xmp = st.format == WebFormat::Jpeg || st.format == WebFormat::Png24 ? web_xmp(doc, st.metadata) : std::nullopt;
    WebResult out;
    out.notes = std::move(picture.notes);
    out.ext = std::string(web_extension(st.format));
    if (!dest) {
        const Optimized o = optimize(picture.rgba, st, icc, xmp, picture.dpi, false, nullptr, picture.exif);
        const bool scaled = o.width != picture.full_width || o.height != picture.full_height;
        const double ratio = scaled ? double(picture.full_width) * picture.full_height / (double(o.width) * o.height) : 1.0;
        out.bytes = o.bytes;
        if (scaled) out.bytes = estimated_bytes(picture.rgba, st, icc, xmp, picture.dpi, o.bytes, ratio, picture.exif);
        out.width = picture.full_width;
        out.height = picture.full_height;
        out.colors = o.colors;
        out.exact = !scaled;
        return out;
    }
    if (dest->has_parent_path()) detail::make_dirs(dest->parent_path());
    // (written as it is made, beside its place; moved in when it is whole — removed if it is not)
    detail::PartFile part(*dest);
    const render::codecs::web::Sink sink = [&part](std::string_view bytes) { part.write(bytes); };
    const Optimized o = optimize(picture.rgba, st, icc, xmp, picture.dpi, false, &sink, picture.exif);
    part.close();
    part.commit();
    out.file = *dest;
    out.bytes = o.bytes;
    out.width = o.width;
    out.height = o.height;
    out.colors = o.colors;
    return out;
}

// --- (PC5 J-b) by the page's slices — PhotoCraft's save_for_web with "dir", and html_table ---

namespace {

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

Rgba8 cut(const Rgba8& from, const core::SliceBox& b) {
    Rgba8 out;
    out.width = static_cast<std::uint32_t>(b.width());
    out.height = static_cast<std::uint32_t>(b.height());
    out.px.resize(std::size_t(out.width) * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const std::uint8_t* row = from.px.data() + ((std::size_t(b.y0) + y) * from.width + std::size_t(b.x0)) * 4;
        std::copy(row, row + std::size_t(out.width) * 4, out.px.data() + std::size_t(y) * out.width * 4);
    }
    return out;
}

}  // namespace

std::string web_slices_html(const std::string& title, std::uint32_t width, std::uint32_t height,
                            const std::vector<std::tuple<std::array<std::int64_t, 4>, std::string, std::string>>& cells,
                            const std::optional<std::string>& spacer) {
    std::vector<std::int64_t> xs, ys;
    for (const auto& [r, attrs, body] : cells) {
        xs.insert(xs.end(), {r[0], r[2]});
        ys.insert(ys.end(), {r[1], r[3]});
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    const auto col = [&](std::int64_t x) { return static_cast<std::size_t>(std::find(xs.begin(), xs.end(), x) - xs.begin()); };
    const auto row = [&](std::int64_t y) { return static_cast<std::size_t>(std::find(ys.begin(), ys.end(), y) - ys.begin()); };
    const std::size_t nc = xs.empty() ? 0 : xs.size() - 1, nr = ys.empty() ? 0 : ys.size() - 1;
    std::vector<char> taken(nc * nr, 0);
    std::vector<std::vector<std::pair<std::size_t, std::string>>> by_row(nr);
    for (const auto& [r, attrs, body] : cells) {
        const std::size_t c0 = col(r[0]), c1 = col(r[2]), r0 = row(r[1]), r1 = row(r[3]);
        bool over = false;
        for (std::size_t j = r0; j < r1 && !over; ++j)
            for (std::size_t i = c0; i < c1 && !over; ++i) over = taken[j * nc + i] != 0;
        if (over) continue;  // (under an earlier slice)
        for (std::size_t j = r0; j < r1; ++j)
            for (std::size_t i = c0; i < c1; ++i) taken[j * nc + i] = 1;
        std::string td = "\t\t<td";
        if (c1 - c0 > 1) td += " colspan=\"" + std::to_string(c1 - c0) + "\"";
        if (r1 - r0 > 1) td += " rowspan=\"" + std::to_string(r1 - r0) + "\"";
        td += attrs + ">" + body + "</td>\n";
        by_row[r0].emplace_back(c0, std::move(td));
    }
    const std::string t = html_escape(title);
    std::string s = "<html>\n<head>\n<title>" + t +
                    "</title>\n<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n</head>\n<body bgcolor=\"#FFFFFF\" "
                    "leftmargin=\"0\" topmargin=\"0\" marginwidth=\"0\" marginheight=\"0\">\n<!-- Save for Web Slices (" +
                    t + ") -->\n<table id=\"Table_01\" width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) +
                    "\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n";
    for (std::size_t j = 0; j < nr; ++j) {
        std::stable_sort(by_row[j].begin(), by_row[j].end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        s += "\t<tr>\n";
        for (const auto& [c, td] : by_row[j]) s += td;
        if (spacer) s += "\t\t<td>\n\t\t\t<img src=\"" + *spacer + "\" width=\"1\" height=\"" + std::to_string(ys[j + 1] - ys[j]) + "\" alt=\"\"></td>\n";
        s += "\t</tr>\n";
    }
    if (spacer) {
        s += "\t<tr>\n";
        for (std::size_t i = 0; i < nc; ++i)
            s += "\t\t<td>\n\t\t\t<img src=\"" + *spacer + "\" width=\"" + std::to_string(xs[i + 1] - xs[i]) + "\" height=\"1\" alt=\"\"></td>\n";
        s += "\t\t<td></td>\n\t</tr>\n";
    }
    s += "</table>\n<!-- End Save for Web Slices -->\n</body>\n</html>\n";
    return s;
}

namespace {

// A name Windows keeps for a device (CON, PRN, AUX, NUL, COM1–9, LPT1–9, any case): never a file's.
bool windows_device(std::string_view name) {
    std::string low;
    for (const char c : name) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "con" || low == "prn" || low == "aux" || low == "nul") return true;
    return low.size() == 4 && (low.starts_with("com") || low.starts_with("lpt")) && low[3] >= '1' && low[3] <= '9';
}

// (PC5 J-b review P3-4) A link the HTML may carry: http, https, mailto or one without a scheme (relative); javascript:,
// data:, vbscript: and any other scheme not — the spaces and controls a browser drops before its scheme dropped first.
bool safe_link(std::string_view url) {
    std::string plain;
    for (const char c : url)
        if (static_cast<unsigned char>(c) > 0x20 && c != 0x7f) plain += c;
    const std::size_t colon = plain.find(':');
    if (colon == std::string::npos) return true;
    const std::size_t stop = plain.find_first_of("/?#");
    if (stop != std::string::npos && stop < colon) return true;  // (a path with a colon in it)
    std::string scheme = plain.substr(0, colon);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return scheme == "http" || scheme == "https" || scheme == "mailto";
}

// What Web 用に保存 by slices writes: each chosen slice's parts (a slice under another cut to what shows — review P2-3 —,
// none when nothing of it shows), the file of each (none: a no-image slice), the spacer and the HTML.
struct SlicePart {
    const core::ResolvedSlice* slice = nullptr;
    core::SliceBox box;
    fs::path file;    // (none: a no-image slice)
    std::string rel;  // (its src in the HTML)
};
struct SlicePlan {
    std::vector<core::ResolvedSlice> all;
    std::vector<SlicePart> parts;
    std::size_t chosen = 0;
    bool table = false;
    std::string spacer_rel;
    fs::path html_path;
    std::string base;
    std::vector<std::string> notes;
};

SlicePlan plan_slices(const core::Document& doc, std::size_t page, std::uint32_t width, std::uint32_t height, const fs::path& folder,
                      const WebSliceOptions& options, const std::string& ext) {
    const core::Page& p = doc.page(page);
    SlicePlan plan;
    // (the picture's pixels a mm: the paper drawn at its size — each slice's edges its mm in them, rounded alone)
    const core::SliceGrid grid = core::slice_grid(p.spec.width_mm.value(), p.spec.height_mm.value(), width, height);
    plan.all = render::slices::resolve(doc, p, grid);
    std::vector<const core::ResolvedSlice*> chosen;
    for (const core::ResolvedSlice& r : plan.all) {
        if (!options.numbers.empty()) {
            if (std::find(options.numbers.begin(), options.numbers.end(), r.number) == options.numbers.end()) continue;
        } else if (options.user_only && r.origin == "auto") {
            continue;
        }
        chosen.push_back(&r);
    }
    if (chosen.empty()) throw core::Error("value", "none of the slices asked for is on the page");
    plan.chosen = chosen.size();
    // (the stored slices in the list's order — a later one on top —, each cut to what of it shows)
    std::map<std::int64_t, const core::ResolvedSlice*> by_id;
    for (const core::ResolvedSlice& r : plan.all)
        if (r.id) by_id[*r.id] = &r;
    std::vector<const core::ResolvedSlice*> stacked;
    std::vector<core::SliceBox> boxes;
    if (p.slices) {
        for (const core::Slice& s : p.slices->list) {
            const auto found = by_id.find(s.id);
            if (found == by_id.end()) continue;
            stacked.push_back(found->second);
            boxes.push_back(found->second->box);
        }
    }
    const std::vector<std::vector<core::SliceBox>> shown = core::owned_pieces(boxes, grid.canvas);
    std::map<const core::ResolvedSlice*, std::vector<core::SliceBox>> pieces;
    for (std::size_t i = 0; i < stacked.size(); ++i) pieces[stacked[i]] = shown[i];
    const std::string base = render::slices::base_name(p);
    plan.base = base;
    // (each file's name its own — review P3-2: one name once whatever its case, as macOS and Windows take them; never a
    // device of Windows —, a second of a name -2, -3 …)
    std::set<std::string> used;
    const auto unique = [&](std::string name) {
        if (windows_device(name)) name += "_";
        std::string candidate = name;
        for (int n = 2; !used.insert(QString::fromStdString(candidate).toCaseFolded().toStdString()).second; ++n) candidate = name + "-" + std::to_string(n);
        return candidate;
    };
    const auto file_of = [&](const std::string& name) {
        const std::string rel = options.images.empty() ? name + "." + ext : options.images + "/" + name + "." + ext;
        return std::pair{folder / core::path_from_utf8(rel), rel};
    };
    // (the spacer's and the HTML's names first: no slice takes them)
    plan.table = options.html && chosen.size() > 1;
    if (plan.table) (void)used.insert("spacer");
    for (const core::ResolvedSlice* r : chosen) {
        std::vector<core::SliceBox> cut{r->box.intersect(grid.canvas)};
        if (const auto found = pieces.find(r); found != pieces.end()) cut = found->second;
        std::erase_if(cut, [](const core::SliceBox& b) { return b.empty(); });
        if (cut.empty()) {
            plan.notes.push_back("スライス " + std::to_string(r->number) + " は上のスライスに隠れているため、書き出しませんでした");
            continue;
        }
        if (cut.size() > 1) plan.notes.push_back("スライス " + std::to_string(r->number) + " は上のスライスと重なるため、" + std::to_string(cut.size()) + " つに分けて書き出しました");
        const std::string name = core::slice_file_name(r->name, "slice");
        for (std::size_t k = 0; k < cut.size(); ++k) {
            SlicePart part{r, cut[k], fs::path(), std::string()};
            if (r->kind != "noImage") {
                const auto [file, rel] = file_of(unique(cut.size() == 1 ? name : name + "_" + std::to_string(k + 1)));
                part.file = file;
                part.rel = rel;
            }
            plan.parts.push_back(std::move(part));
        }
    }
    plan.spacer_rel = options.images.empty() ? std::string("spacer.gif") : options.images + "/spacer.gif";
    plan.html_path = folder / core::path_from_utf8(base + ".html");
    return plan;
}

std::vector<fs::path> plan_files(const SlicePlan& plan, const fs::path& folder, const WebSliceOptions& options) {
    std::vector<fs::path> out;
    for (const SlicePart& part : plan.parts)
        if (!part.file.empty()) out.push_back(part.file);
    if (plan.table) out.push_back(folder / core::path_from_utf8(plan.spacer_rel));
    if (options.html) out.push_back(plan.html_path);
    return out;
}

}  // namespace

std::vector<fs::path> web_slice_files(const core::Document& doc, std::size_t page, const WebSettings& st, const WebSize& size, const fs::path& folder,
                                      const WebSliceOptions& options) {
    CopyOptions copy;
    if (size.percent) copy.scale = *size.percent;
    copy.width = size.width;
    copy.height = size.height;
    copy.resample = size.resample;
    copy.metadata = Metadata::None;
    const CopySize full = copy_size(doc, page, copy);
    const SlicePlan plan = plan_slices(doc, page, full.width, full.height, folder, options, std::string(web_extension(st.format)));
    return plan_files(plan, folder, options);
}

WebSlicesResult save_slices_for_web(const core::Document& doc, std::size_t page, const WebSettings& st, const WebSize& size, const fs::path& folder,
                                    const WebSliceOptions& options) {
    WebPicture picture = web_picture(doc, page, st, size);
    std::optional<std::string> icc = picture.icc;
    if (st.embed_icc && !icc) icc = srgb_icc_once();
    const std::optional<std::string> xmp = st.format == WebFormat::Jpeg || st.format == WebFormat::Png24 ? web_xmp(doc, st.metadata) : std::nullopt;
    const core::Page& p = doc.page(page);
    const SlicePlan plan = plan_slices(doc, page, picture.rgba.width, picture.rgba.height, folder, options, std::string(web_extension(st.format)));
    WebSlicesResult out;
    out.notes = std::move(picture.notes);
    out.notes.insert(out.notes.end(), plan.notes.begin(), plan.notes.end());
    out.slices = plan.chosen;
    out.width = picture.rgba.width;
    out.height = picture.rgba.height;
    // (every place asked before anything is made)
    if (options.check) {
        options.check(folder);
        for (const fs::path& file : plan_files(plan, folder, options)) options.check(file);
    }
    detail::make_dirs(folder);
    if (!options.images.empty()) detail::make_dirs(folder / core::path_from_utf8(options.images));
    detail::Output output;
    std::vector<std::tuple<std::array<std::int64_t, 4>, std::string, std::string>> cells;
    const core::Slices none;
    const core::Slices& stored = p.slices ? *p.slices : none;
    std::set<int> unsafe;  // (the slices whose link was not written)
    for (const SlicePart& part : plan.parts) {
        const core::ResolvedSlice* r = part.slice;
        const core::SliceBox& box = part.box;
        const core::Slice* s = r->id ? core::find_slice(stored, *r->id) : nullptr;
        const std::array<std::int64_t, 4> edges{box.x0, box.y0, box.x1, box.y1};
        if (part.file.empty()) {  // (no image: its cell text, its background)
            std::string text = s != nullptr ? (s->cell_text_is_html ? s->cell_text : html_escape(s->cell_text)) : std::string();
            std::string attrs;
            if (s != nullptr && s->background) {
                std::string hex = s->background->substr(1);
                std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                attrs = " bgcolor=\"#" + hex + "\"";
            }
            cells.emplace_back(edges, attrs, std::move(text));
            continue;
        }
        detail::PartFile file(part.file);
        const render::codecs::web::Sink sink = [&file](std::string_view bytes) { file.write(bytes); };
        const Optimized o = optimize(cut(picture.rgba, box), st, icc, xmp, picture.dpi, false, &sink, picture.exif);
        file.close();
        output.put(std::move(file));
        out.bytes += o.bytes;
        out.files.push_back(part.file);
        const std::string alt = s != nullptr ? html_escape(s->alt) : std::string();
        const std::string img = "\n\t\t\t<img src=\"" + html_escape(part.rel) + "\" width=\"" + std::to_string(box.width()) + "\" height=\"" +
                                std::to_string(box.height()) + "\" alt=\"" + alt + "\">";
        std::string body = img;
        if (s != nullptr && !s->url.empty()) {
            if (safe_link(s->url)) {
                const std::string target = s->target.empty() ? std::string() : " target=\"" + html_escape(s->target) + "\"";
                body = "\n\t\t\t<a href=\"" + html_escape(s->url) + "\"" + target + ">" + img + "</a>";
            } else if (options.html) {
                unsafe.insert(r->number);
            }
        }
        cells.emplace_back(edges, std::string(), std::move(body));
    }
    for (const int number : unsafe)
        out.notes.push_back("スライス " + std::to_string(number) + " の URL は http・https・mailto でも相対でもないため、HTML にリンクを書きませんでした");
    if (options.html) {
        std::optional<std::string> spacer;
        if (plan.table) {
            spacer = plan.spacer_rel;
            const fs::path spacer_path = folder / core::path_from_utf8(plan.spacer_rel);
            detail::PartFile file(spacer_path);
            const render::codecs::web::Sink sink = [&file](std::string_view bytes) { file.write(bytes); };
            const std::array<std::uint8_t, 1> index{0};
            const std::array<Rgb8, 1> palette{Rgb8{std::uint8_t(0), std::uint8_t(0), std::uint8_t(0)}};
            render::codecs::web::write_gif_indexed(1, 1, index, palette, std::uint8_t(0), false, sink);
            file.close();
            output.put(std::move(file));
            out.files.push_back(spacer_path);
        }
        output.put(plan.html_path, web_slices_html(plan.base, picture.rgba.width, picture.rgba.height, cells, spacer));
        out.html = plan.html_path;
    }
    output.commit();
    return out;
}

std::optional<QuickLocation> quick_location_of(std::string_view id) {
    if (id == "ask") return QuickLocation::Ask;
    if (id == "sameFolder") return QuickLocation::SameFolder;
    if (id == "exports") return QuickLocation::Exports;
    return std::nullopt;
}

std::string_view to_string(QuickLocation location) {
    switch (location) {
    case QuickLocation::Ask: return "ask";
    case QuickLocation::SameFolder: return "sameFolder";
    case QuickLocation::Exports: return "exports";
    }
    return "ask";
}

std::optional<fs::path> quick_export_place(const ExportPreferences& prefs, const std::optional<fs::path>& book, const core::Page& page,
                                           const core::Layer* layer) {
    if (prefs.location == QuickLocation::Ask || !book || book->empty()) return std::nullopt;
    const fs::path folder = book->filename().empty() ? book->parent_path() : *book;
    const std::string stem = core::path_to_utf8(folder.stem());
    std::string name = stem + "_" + core::file_stem(page);
    if (layer != nullptr) name += "_" + layer->title;
    // (a name every OS takes, cut by characters as the exports' names are)
    const std::string base = safe_name(name, "genko");
    const fs::path where = prefs.location == QuickLocation::SameFolder ? folder.parent_path() : folder.parent_path() / core::path_from_utf8(stem + "_書き出し");
    // (the coordinator's P3-8: a file already there never written over — the next free name-2, name-3 … —)
    fs::path place = where / core::path_from_utf8(base + "." + prefs.format);
    std::error_code ec;
    for (int n = 2; fs::exists(place, ec) || fs::is_symlink(place, ec); ++n) {
        if (n > 9999) throw core::Error("refused_target", "too many files of that name: " + core::path_to_utf8(place));
        place = where / core::path_from_utf8(base + "-" + std::to_string(n) + "." + prefs.format);
    }
    return place;
}

CopyResult quick_export(const core::Document& doc, std::size_t page, const fs::path& dest, const ExportPreferences& prefs, const std::optional<std::string>& layer) {
    if (std::find(kQuickFormats.begin(), kQuickFormats.end(), prefs.format) == kQuickFormats.end()) {
        throw core::Error("value", "the quick export's format is png, jpg, gif or webp");
    }
    if (prefs.format == "webp") {  // (no Save for Web optimiser of its own: the codecs' WebP, lossless)
        CopyOptions copy;
        copy.layer = layer;
        copy.metadata = prefs.metadata;
        copy.srgb = prefs.srgb;
        return write_copy(doc, page, dest, copy);
    }
    WebSettings st;
    st.transparency = true;
    st.convert_to_srgb = prefs.srgb;
    st.embed_icc = !prefs.srgb;  // (the page's colours kept: its profile with them)
    st.metadata = prefs.metadata == Metadata::None ? WebMetadata::None : prefs.metadata == Metadata::All ? WebMetadata::All : WebMetadata::CopyrightAndContact;
    if (prefs.format == "jpg") {
        st.format = WebFormat::Jpeg;
        st.quality = std::clamp(prefs.jpeg_quality, 1, 100);
    } else if (prefs.format == "gif") {
        st.format = WebFormat::Gif;
    } else {
        st.format = WebFormat::Png24;
    }
    WebResult written = save_for_web(doc, page, st, {}, dest, layer);
    CopyResult out;
    out.file = dest;
    out.notes = std::move(written.notes);
    out.width = written.width;
    out.height = written.height;
    out.bytes = written.bytes;
    return out;
}

}  // namespace genko::formats
