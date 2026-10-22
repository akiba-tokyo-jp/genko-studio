// AVIF (render/codecs/formats.hpp; libavif with libaom, BSD-2-Clause): the first picture read — its header parsed and its
// size checked against the limits before its pixels are decoded (libavif's own limits set to them too) — as RGB or RGBA of
// 8 bits (16 for a deeper file), with its ICC profile, EXIF and XMP; written as 8-bit 4:4:4 at the quality asked (libaom,
// speed 6), its colours sRGB's (BT.709 primaries, sRGB's curve) unless it carries a profile, with its EXIF and XMP. libavif
// is asked for libaom by name both ways, so that a file reads and writes the same on every computer.

#include <avif/avif.h>

#include <algorithm>
#include <cstring>

#include "core/error.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs {

std::string libavif_version() { return avifVersion(); }

std::string aom_version() {
    // (libavif names the codecs it was built with and their versions: "… aom [enc/dec]:v3.8.2, …")
    char text[256] = {0};
    avifCodecVersions(text);
    const std::string_view all(text);
    const std::size_t at = all.find("aom [");
    if (at == std::string_view::npos) return "unavailable";
    const std::size_t colon = all.find(':', at);
    if (colon == std::string_view::npos) return "unavailable";
    std::size_t end = all.find_first_of(", ", colon);
    std::string_view version = all.substr(colon + 1, end == std::string_view::npos ? std::string_view::npos : end - colon - 1);
    if (version.starts_with('v')) version.remove_prefix(1);
    return std::string(version);
}

namespace detail {

namespace {

struct Decoder {
    avifDecoder* d = avifDecoderCreate();
    ~Decoder() {
        if (d != nullptr) avifDecoderDestroy(d);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder() = default;
};

struct Rgb {
    avifRGBImage rgb{};
    bool allocated = false;
    ~Rgb() {
        if (allocated) avifRGBImageFreePixels(&rgb);
    }
};

struct Encoding {
    avifImage* image = nullptr;
    avifEncoder* encoder = avifEncoderCreate();
    avifRWData out = AVIF_DATA_EMPTY;
    ~Encoding() {
        avifRWDataFree(&out);
        if (encoder != nullptr) avifEncoderDestroy(encoder);
        if (image != nullptr) avifImageDestroy(image);
    }
};

[[noreturn]] void failed(avifResult r) { broken("AVIF", avifResultToString(r)); }

// The decoder set up on the bytes and the file parsed (no pixel decoded yet).
void parse(Decoder& decoder, std::string_view bytes, const Limits& limits) {
    if (decoder.d == nullptr) throw core::Error("memory", "the AVIF reader cannot be made");
    decoder.d->codecChoice = AVIF_CODEC_CHOICE_AOM;
    decoder.d->strictFlags = AVIF_STRICT_DISABLED;
    decoder.d->imageSizeLimit = static_cast<uint32_t>(std::clamp<std::int64_t>(limits.max_pixels, 1, 0xffffffffLL));
    decoder.d->imageDimensionLimit = static_cast<uint32_t>(std::clamp<std::int64_t>(std::max(limits.max_width, limits.max_height), 1, 0xffffffffLL));
    avifResult r = avifDecoderSetIOMemory(decoder.d, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (r != AVIF_RESULT_OK) failed(r);
    r = avifDecoderParse(decoder.d);
    if (r == AVIF_RESULT_NOT_IMPLEMENTED || r == AVIF_RESULT_NO_CODEC_AVAILABLE) throw core::Error("format", std::string("the AVIF file cannot be read: ") + avifResultToString(r));
    if (r != AVIF_RESULT_OK) failed(r);
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> avif_size(std::string_view bytes) {
    Decoder decoder;
    parse(decoder, bytes, Limits::none());
    return {decoder.d->image->width, decoder.d->image->height};
}

CodecImage read_avif(std::string_view bytes, const Limits& limits) {
    // (parsed with libavif's own bounds wide, so that a picture past the limits is said too large by its size here — not as
    // libavif's parse error —, then decoded only within them)
    Decoder decoder;
    parse(decoder, bytes, Limits::none());
    const std::uint32_t width = decoder.d->image->width, height = decoder.d->image->height;
    const bool alpha = decoder.d->alphaPresent != AVIF_FALSE;
    const Layout layout = alpha ? Layout::Rgba : Layout::Rgb;
    const Sample sample = decoder.d->image->depth > 8 ? Sample::U16 : Sample::U8;
    limits.check(width, height, layout, sample);
    avifResult r = avifDecoderNextImage(decoder.d);
    if (r != AVIF_RESULT_OK) failed(r);
    const avifImage* image = decoder.d->image;
    if (image->width != width || image->height != height) broken("AVIF", "its picture is not the size its header says");
    Rgb rgb;
    avifRGBImageSetDefaults(&rgb.rgb, image);
    rgb.rgb.format = alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.rgb.depth = sample == Sample::U16 ? 16 : 8;
    r = avifRGBImageAllocatePixels(&rgb.rgb);
    if (r != AVIF_RESULT_OK) throw core::Error("image_too_large", "the AVIF picture is too large to read");
    rgb.allocated = true;
    r = avifImageYUVToRGB(image, &rgb.rgb);
    if (r != AVIF_RESULT_OK) failed(r);
    const std::size_t row = std::size_t(image->width) * channels(layout) * sample_bytes(sample);
    std::string data(row * image->height, '\0');
    for (std::uint32_t y = 0; y < image->height; ++y) std::memcpy(data.data() + y * row, rgb.rgb.pixels + std::size_t(y) * rgb.rgb.rowBytes, row);
    CodecImage out = CodecImage::from_bytes(image->width, image->height, layout, sample, std::move(data));
    if (image->icc.size > 0) out.icc = std::string(reinterpret_cast<const char*>(image->icc.data), image->icc.size);
    if (image->exif.size > 0) out.meta.exif = std::string(reinterpret_cast<const char*>(image->exif.data), image->exif.size);
    if (image->xmp.size > 0) out.meta.xmp = std::string(reinterpret_cast<const char*>(image->xmp.data), image->xmp.size);
    return out;
}

std::string write_avif(const CodecImage& stored, const EncodeOptions& options) {
    if (stored.sample() != Sample::U8 || !is_rgb(stored.layout())) throw core::Error("value", "AVIF is written from 8-bit RGB, with or without an alpha");
    Encoding e;
    e.image = avifImageCreate(stored.width(), stored.height(), 8, AVIF_PIXEL_FORMAT_YUV444);
    if (e.image == nullptr || e.encoder == nullptr) throw core::Error("memory", "the AVIF writer cannot be made");
    const bool icc = stored.icc && !stored.icc->empty();
    e.image->colorPrimaries = icc ? AVIF_COLOR_PRIMARIES_UNSPECIFIED : AVIF_COLOR_PRIMARIES_BT709;
    e.image->transferCharacteristics = icc ? AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED : AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    e.image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    e.image->yuvRange = AVIF_RANGE_FULL;
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, e.image);
    const bool alpha = stored.layout() == Layout::Rgba;
    rgb.format = alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.depth = 8;
    rgb.pixels = reinterpret_cast<uint8_t*>(const_cast<char*>(stored.data().data()));
    rgb.rowBytes = stored.width() * (alpha ? 4 : 3);
    avifResult r = avifImageRGBToYUV(e.image, &rgb);
    if (r != AVIF_RESULT_OK) throw core::Error("format", std::string("cannot write AVIF: ") + avifResultToString(r));
    const auto bytes = [](const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); };
    if (icc && avifImageSetProfileICC(e.image, bytes(*stored.icc), stored.icc->size()) != AVIF_RESULT_OK)
        throw core::Error("memory", "cannot write AVIF (its profile)");
    if (stored.meta.exif && avifImageSetMetadataExif(e.image, bytes(*stored.meta.exif), stored.meta.exif->size()) != AVIF_RESULT_OK)
        throw core::Error("value", "cannot write AVIF (its EXIF)");
    if (stored.meta.xmp && avifImageSetMetadataXMP(e.image, bytes(*stored.meta.xmp), stored.meta.xmp->size()) != AVIF_RESULT_OK)
        throw core::Error("memory", "cannot write AVIF (its XMP)");
    e.encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
    e.encoder->maxThreads = 1;
    e.encoder->speed = 6;
    e.encoder->quality = std::clamp(options.jpeg_quality, 0, 100);
    e.encoder->qualityAlpha = std::clamp(options.jpeg_quality, 0, 100);
    r = avifEncoderWrite(e.encoder, e.image, &e.out);
    if (r != AVIF_RESULT_OK) throw core::Error("format", std::string("cannot write AVIF: ") + avifResultToString(r));
    return std::string(reinterpret_cast<const char*>(e.out.data), e.out.size);
}

}  // namespace detail

}  // namespace genko::render::codecs
