#include "api/buildinfo.hpp"

#include <QtGlobal>
#include <cstdio>
#include <string>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <jpeglib.h>
#include <lcms2.h>
#include <png.h>
#include <tiffio.h>
#include <webp/decode.h>
#include <zlib.h>

#include "core/version.hpp"
#include "render/blake3.hpp"
#include "render/codecs/codecs.hpp"
#include "render/zstd.hpp"

namespace genko::api {

namespace {

std::string freetype_version() {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return "unavailable";
    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(lib, &major, &minor, &patch);
    FT_Done_FreeType(lib);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::string webp_version() {
    const int v = WebPGetDecoderVersion();
    return std::to_string((v >> 16) & 0xff) + "." + std::to_string((v >> 8) & 0xff) + "." + std::to_string(v & 0xff);
}

std::string lcms_version() {
    const int v = cmsGetEncodedCMMversion();  // e.g. 2160 for 2.16
    return std::to_string(v / 1000) + "." + std::to_string((v % 1000) / 10);
}

std::string tiff_version() {
    // TIFFGetVersion(): "LIBTIFF, Version 4.5.1\nCopyright ..." — the number after "Version ".
    const std::string text = TIFFGetVersion();
    const auto at = text.find("Version ");
    if (at == std::string::npos) return "unknown";
    const auto start = at + 8;
    const auto end = text.find_first_of("\n ", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string jpeg_turbo_version() {
#ifdef LIBJPEG_TURBO_VERSION_NUMBER
    const int v = LIBJPEG_TURBO_VERSION_NUMBER;  // e.g. 2001005 for 2.1.5
    return std::to_string(v / 1000000) + "." + std::to_string((v / 1000) % 1000) + "." + std::to_string(v % 1000);
#else
    return "unknown";
#endif
}

}  // namespace

nlohmann::json build_info() {
    return {
        {"genko", std::string(genko::version())},
        {"format", genko::kWriteFormatVersion},
        {"qt", qVersion()},
        {"zlib", zlibVersion()},
        {"libpng", png_get_libpng_ver(nullptr)},
        {"libjpeg", std::to_string(JPEG_LIB_VERSION)},
        {"libjpeg_turbo", jpeg_turbo_version()},
        {"libtiff", tiff_version()},
        {"freetype", freetype_version()},
        {"harfbuzz", hb_version_string()},
        {"webp", webp_version()},
        {"lcms2", lcms_version()},
        {"openexr", render::codecs::openexr_version()},
        {"libavif", render::codecs::libavif_version()},
        {"aom", render::codecs::aom_version()},
        {"zstd", render::zstd::version()},      // (PC4 F-f: the .pcraft's compression)
        {"blake3", render::blake3::version()},  // (PC4 F-f: the .pcraft's hashes, built in)
        {"nlohmann_json", std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." + std::to_string(NLOHMANN_JSON_VERSION_MINOR) +
                              "." + std::to_string(NLOHMANN_JSON_VERSION_PATCH)},
    };
}

}  // namespace genko::api
