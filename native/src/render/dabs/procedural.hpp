#pragma once

#include <cstdint>
#include <string_view>

#include "render/dabs/tile.hpp"

// PhotoCraft v0.2.0 crates/paint/src/procedural.rs (MIT: native/third_party/photocraft/NOTICE.md): texture patterns and
// sampled tips made by the program (seeded value noise and simple geometry: no brush or pattern files are shipped).
// The angles are the engine's (render/dabs/maths.hpp), so the same pictures on every computer.

namespace genko::render::dabs {

// A seamless pattern tile, `size` (8..1024) pixels a side: style noise | canvas | paper | dots.
PatternImage pattern(std::string_view style, std::uint32_t size, std::uint32_t seed);

// The built-in sampled tips (E2's presets use them): chalk, spatter, bristle, charcoal, leaf, grass, sponge, star, rake.
GrayTile chalk_tip(std::uint32_t n, std::uint32_t seed);
GrayTile spatter_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t drops);
GrayTile bristle_tip(std::uint32_t n, std::uint32_t seed);
GrayTile charcoal_tip(std::uint32_t n, std::uint32_t seed);
GrayTile leaf_tip(std::uint32_t n);
GrayTile grass_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t blades);
GrayTile sponge_tip(std::uint32_t n, std::uint32_t seed);
GrayTile star_tip(std::uint32_t n);
GrayTile rake_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t bristles);

}  // namespace genko::render::dabs
