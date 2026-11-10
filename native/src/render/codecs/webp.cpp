// WebP (render/codecs/formats.hpp; PhotoCraft's crates/codecs/src/codecs/webp.rs): read through this build's WebP reader
// (its first frame, as the drawing reads it), its profile (ICCP), EXIF and XMP chunks from the file; written with libwebp —
// lossless (the colours under a transparent pixel kept exactly) or lossy at the quality asked —, and the profile, EXIF and
// XMP put in by libwebp's mux (an extended file, VP8X).

#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/codecs/metadata.hpp"
#include "render/proof.hpp"

namespace genko::render::codecs::detail {

namespace {

struct Picture {
    WebPPicture p{};
    WebPMemoryWriter out{};
    bool ready = false;
    Picture() {
        ready = WebPPictureInit(&p) != 0;
        WebPMemoryWriterInit(&out);
    }
    ~Picture() {
        WebPPictureFree(&p);
        WebPMemoryWriterClear(&out);
    }
    Picture(const Picture&) = delete;
    Picture& operator=(const Picture&) = delete;
};

struct Mux {
    WebPMux* mux = WebPMuxNew();
    WebPData out{};
    ~Mux() {
        WebPMuxDelete(mux);
        WebPDataClear(&out);
    }
    Mux() { WebPDataInit(&out); }
    Mux(const Mux&) = delete;
    Mux& operator=(const Mux&) = delete;
};

}  // namespace

CodecImage read_webp(std::string_view bytes, const Limits& limits) {
    CodecImage out = this_builds_picture(bytes, limits, false);
    out.icc = proof::embedded_profile(bytes);
    const FileMetadata meta = read_metadata(Format::WebP, bytes);
    out.meta.exif = meta.exif;
    out.meta.xmp = meta.xmp;
    return out;
}

std::string write_webp(const CodecImage& stored, const EncodeOptions& options) {
    if (stored.sample() != Sample::U8 || !is_rgb(stored.layout())) throw core::Error("value", "WebP holds 8-bit RGB, with or without an alpha");
    WebPConfig config;
    if (WebPConfigInit(&config) == 0) throw core::Error("memory", "the WebP writer cannot be made");
    if (options.webp_lossless) {
        WebPConfigLosslessPreset(&config, 6);
        config.exact = 1;  // (the colours under a transparent pixel are the picture's too)
    } else {
        config.quality = static_cast<float>(std::clamp(options.jpeg_quality, 1, 100));
        config.method = 4;
    }
    config.thread_level = 0;
    Picture picture;
    if (!picture.ready) throw core::Error("memory", "the WebP writer cannot be made");
    picture.p.use_argb = 1;
    picture.p.width = static_cast<int>(stored.width());
    picture.p.height = static_cast<int>(stored.height());
    const auto* data = reinterpret_cast<const uint8_t*>(stored.data().data());
    const bool alpha = has_alpha(stored.layout());
    const int stride = static_cast<int>(stored.width() * (alpha ? 4 : 3));
    const int imported = alpha ? WebPPictureImportRGBA(&picture.p, data, stride) : WebPPictureImportRGB(&picture.p, data, stride);
    if (imported == 0) throw core::Error("memory", "the picture is too large to write as WebP");
    picture.p.writer = WebPMemoryWrite;
    picture.p.custom_ptr = &picture.out;
    if (WebPEncode(&config, &picture.p) == 0) throw core::Error("format", "cannot write WebP (libwebp's error " + std::to_string(picture.p.error_code) + ")");
    std::string plain(reinterpret_cast<const char*>(picture.out.mem), picture.out.size);
    const bool icc = stored.icc && !stored.icc->empty();
    if (!icc && !stored.meta.exif && !stored.meta.xmp) return plain;
    // (an extended file: the picture, then its profile, EXIF and XMP as chunks)
    Mux mux;
    if (mux.mux == nullptr) throw core::Error("memory", "the WebP writer cannot be made");
    const WebPData image{reinterpret_cast<const uint8_t*>(plain.data()), plain.size()};
    if (WebPMuxSetImage(mux.mux, &image, 1) != WEBP_MUX_OK) throw core::Error("format", "cannot write WebP (its picture)");
    const auto put = [&](const char* fourcc, const std::string& value) {
        const WebPData chunk{reinterpret_cast<const uint8_t*>(value.data()), value.size()};
        if (WebPMuxSetChunk(mux.mux, fourcc, &chunk, 1) != WEBP_MUX_OK) throw core::Error("format", std::string("cannot write WebP (its ") + fourcc + ")");
    };
    if (icc) put("ICCP", *stored.icc);
    if (stored.meta.exif) put("EXIF", *stored.meta.exif);
    if (stored.meta.xmp) put("XMP ", *stored.meta.xmp);
    if (WebPMuxAssemble(mux.mux, &mux.out) != WEBP_MUX_OK) throw core::Error("format", "cannot write WebP (its chunks)");
    return std::string(reinterpret_cast<const char*>(mux.out.bytes), mux.out.size);
}

}  // namespace genko::render::codecs::detail
