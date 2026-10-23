#pragma once

#include <nlohmann/json.hpp>

namespace genko::api {

// The versions of Genko and of every third-party library linked into this build (for `genko --version`,
// the About box and the SBOM check). Keys: genko, format, qt, zlib, libpng, libjpeg (the API's version), libjpeg_turbo, libtiff, freetype,
// harfbuzz, webp, lcms2, openexr, libavif, aom (PC4 F-a: the codecs' OpenEXR, libavif and its AV1 codec), zstd, blake3 (PC4 F-f:
// the .pcraft's compression and hashes), nlohmann_json.
nlohmann::json build_info();

}  // namespace genko::api
