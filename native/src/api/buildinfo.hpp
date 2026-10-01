#pragma once

#include <nlohmann/json.hpp>

namespace genko::api {

// The versions of Genko and of every third-party library linked into this build (for `genko --version`,
// the About box and the SBOM check). Keys: genko, format, qt, zlib, libpng, libjpeg, libtiff, freetype,
// harfbuzz, webp, lcms2, nlohmann_json.
nlohmann::json build_info();

}  // namespace genko::api
