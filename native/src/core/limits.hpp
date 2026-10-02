#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/error.hpp"

// The sizes past which the C++ build refuses an op or a drawing where Python goes on until it stops, or until it has
// no memory left (docs/cpp-migration/SPEC.md COMP-01a). One table for every module: what each bounds, and how the
// refusal reads.

namespace genko::core::limits {

// --- lines and shapes ----------------------------------------------------------------------------------------------

// The steps of a loop along a length (the points of a pen line, of a panel's edge, …: Python's int(length / step)):
// past this Python's loop runs until it has no memory left. Refused as its MemoryError (core::loop_count).
inline constexpr double kLoopSteps = 1e7;
// The points made along a ruler, a curve or an effect line: PyValueError "too many points (N): the shape is far too
// large" (core::checked_count).
inline constexpr std::int64_t kShapePoints = 4'000'000;
// The lines of one effect (集中線, 流線, フラッシュ).
inline constexpr std::int64_t kEffectLines = 100'000;

// --- tones and fills -----------------------------------------------------------------------------------------------

// A screen's threshold tile, pixels a side (a screen this coarse takes Python minutes and gigabytes).
inline constexpr int kScreenTile = 4096;
// A screen cell's step, pixels (a screen far too coarse for the resolution).
inline constexpr std::int64_t kScreenCell = 100'000;
// The pixels of a fill's patch or of an area's mask.
inline constexpr std::int64_t kPatchPixels = 400'000'000;
// A fill's coordinate, pixels from the page's corner.
inline constexpr std::int64_t kFillCoordinate = 1'000'000'000;

// --- 3D ------------------------------------------------------------------------------------------------------------

// The vertices and the face corners of an imported model (Python's MAX_FACES bounds only the faces).
inline constexpr std::int64_t kModelVertices = 240'000;
inline constexpr std::int64_t kModelCorners = 480'000;
// How deep a glTF's nodes may nest, and how many of them are visited (a node inside itself never ends in Python).
inline constexpr std::size_t kModelNodeDepth = 900;
inline constexpr std::int64_t kModelNodeVisits = 100'000;
// The pixels of the 3D's shaded surfaces, and of the depth picture its pen lines are traced with.
inline constexpr std::int64_t kSurfacePixels = 200'000'000;
inline constexpr double kLineDepthPixels = 64'000'000.0;
// The floor lines of one building of a street scene (分割数: a building kilometres tall).
inline constexpr std::int64_t kStoreyLines = 10'000;
// The poses a person keeps (ポーズ素材, the config folder's poses.json).
inline constexpr std::size_t kUserPoses = 1'000;

}  // namespace genko::core::limits

namespace genko::core {

// A count of points made from a length (about one per mm, per pixel, …): more than `limit` is refused (PyValueError)
// rather than filling memory for minutes, as Python would for a ruler or an effect a light year long.
inline std::int64_t checked_count(std::int64_t n, std::int64_t limit = limits::kShapePoints) {
    if (n > limit) throw PyValueError("too many points (" + std::to_string(n) + "): the shape is far too large");
    return n;
}

}  // namespace genko::core
