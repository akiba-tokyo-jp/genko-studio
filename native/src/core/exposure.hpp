#pragma once
#include "core/json.hpp"
namespace genko::core {
// Formula and bounds: PhotoCraft v0.2.0 compose/adjust.rs + engine/adjust_params.rs.
// Attribution and full upstream MIT license: native/third_party/photocraft/LICENSE-MIT.
inline constexpr const char* kExposureFeature = "native.exposure_v1";
struct Exposure {
    double stops = 0, offset = 0, gamma = 1;
    static Exposure parse(const Json& spec);
    double apply(double linear) const;
};
double srgb_to_linear(double x);
double linear_to_srgb(double x);
} // namespace genko::core
