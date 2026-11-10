#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

// smart-objects@1, PC3 A4 (docs/cpp-migration/SPEC.md COLOR-05「スタックモード」): a smart object's stack mode — its
// contents' layers combined pixel by pixel by a statistic (レイヤー → スマートオブジェクト → スタックモード: the median to
// take passers-by out of shots of one place, the mean to quieten noise). PhotoCraft v0.2.0's algo/src/stack.rs (MIT,
// third_party/photocraft/NOTICE.md): only the samples whose alpha is more than 0 count and the alpha is the largest;
// the moments are the population's; the skewness and the kurtosis are standardized and scaled into 0..1 to be seen
// (the skewness −2..2, 0.5 symmetric; the kurtosis 0..10, a normal sample about 0.3); the entropy is the Shannon entropy
// in bits of the 8-bit values over log2 of their count; every result is taken into 0..1. Where the layers are drawn is
// render/smart.cpp.

namespace genko::render::stack {

enum class Stat { Entropy, Kurtosis, Maximum, Mean, Median, Minimum, Range, Skewness, StandardDeviation, Summation, Variance };

// The statistic of a stack mode (core::smart::kStackModes' names), or none for another name.
std::optional<Stat> stat_of(std::string_view mode);

// The statistic of samples (each 0..1; reordered: the median sorts them), not yet taken into 0..1; 0 for none.
double statistic(std::vector<double>& samples, Stat stat);

// What a stack of one layer makes of its one sample: the sample itself (the mean, median, minimum, maximum and sum of
// one), or a value of its own for every sample (none, or the same layer many times: the range, variance and standard
// deviation 0, the skewness 0.5, the kurtosis and the entropy 0) — that value.
std::optional<double> of_one(Stat stat);

// One pixel of the stack: `values` holds `layers` samples of the pixel, each `colours` values then its straight alpha
// (0..1); `out` gets `colours` values then the alpha (all 0 when no layer shows there). `scratch` is the caller's, kept
// between pixels.
void combine_pixel(const double* values, std::size_t layers, unsigned colours, Stat stat, double* out, std::vector<double>& scratch);

}  // namespace genko::render::stack
