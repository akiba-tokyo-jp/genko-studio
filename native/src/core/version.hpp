#pragma once

#include <string_view>

namespace genko {

// The product version of the native build (CMake project version).
std::string_view version();

// The project.json format this build writes (docs/cpp-migration/schema-v4.md).
inline constexpr int kWriteFormatVersion = 4;

// The oldest project.json format this build can read (and convert).
inline constexpr int kOldestReadableFormat = 1;

}  // namespace genko
