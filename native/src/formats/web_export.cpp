// Web 用に保存（従来）…, 書き出しの環境設定… and クイック書き出し (formats/web_export.hpp; PhotoCraft v0.2.0 web_cmds.rs, MIT).

#include "formats/web_export.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "render/codecs/codecs.hpp"
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
                              double dpi, std::uint64_t bytes, double ratio) {
    // (what the file has whatever its size — headers, a palette, a profile, metadata — from the same file of the proxy's
    // first pixel, not scaled with the pixels)
    Rgba8 one;
    one.width = 1;
    one.height = 1;
    one.px.assign(proxy.px.begin(), proxy.px.begin() + std::min<std::size_t>(4, proxy.px.size()));
    std::uint64_t fixed = 0;
    if (one.px.size() == 4) fixed = std::min<std::uint64_t>(bytes, optimize(one, st, icc, xmp, dpi, false, nullptr).bytes);
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
                   bool preview, const render::codecs::web::Sink* sink) {
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
    // (a page larger than a GIF holds: refused before it is drawn)
    if (!layer && st.format == WebFormat::Gif && (full.width > 65535 || full.height > 65535)) {
        throw core::Error("image_too_large", "GIF holds at most 65535×65535 pixels");
    }
    if (proxy && *proxy > 0 && std::max(full.width, full.height) > *proxy) {
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
    } else if (profiled || !render::codecs::is_rgb(image.layout())) {
        image = srgb_picture(image, false, false);
        out.icc = std::nullopt;
    }
    const CodecImage rgba = image.convert(Layout::Rgba, Sample::U8);
    image = CodecImage();
    out.rgba.width = rgba.width();
    out.rgba.height = rgba.height();
    out.rgba.px.assign(rgba.data().begin(), rgba.data().end());
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
        const Optimized o = optimize(picture.rgba, st, icc, xmp, picture.dpi, false, nullptr);
        const bool scaled = o.width != picture.full_width || o.height != picture.full_height;
        const double ratio = scaled ? double(picture.full_width) * picture.full_height / (double(o.width) * o.height) : 1.0;
        out.bytes = o.bytes;
        if (scaled) out.bytes = estimated_bytes(picture.rgba, st, icc, xmp, picture.dpi, o.bytes, ratio);
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
    const Optimized o = optimize(picture.rgba, st, icc, xmp, picture.dpi, false, &sink);
    part.close();
    part.commit();
    out.file = *dest;
    out.bytes = o.bytes;
    out.width = o.width;
    out.height = o.height;
    out.colors = o.colors;
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
    // (a name every OS takes, cut by characters — never inside one —, as the exports' names are)
    name = safe_name(name, "genko") + "." + prefs.format;
    const fs::path where = prefs.location == QuickLocation::SameFolder ? folder.parent_path() : folder.parent_path() / core::path_from_utf8(stem + "_書き出し");
    return where / core::path_from_utf8(name);
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
