// A smart object's stack mode's statistics (render/stack.hpp, PC3 A4): PhotoCraft v0.2.0's algo/src/stack.rs (MIT,
// third_party/photocraft/NOTICE.md), in doubles.

#include "render/stack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace genko::render::stack {

namespace {

constexpr std::array<std::pair<std::string_view, Stat>, 11> kStats{{{"entropy", Stat::Entropy},
                                                                   {"kurtosis", Stat::Kurtosis},
                                                                   {"maximum", Stat::Maximum},
                                                                   {"mean", Stat::Mean},
                                                                   {"median", Stat::Median},
                                                                   {"minimum", Stat::Minimum},
                                                                   {"range", Stat::Range},
                                                                   {"skewness", Stat::Skewness},
                                                                   {"standard_deviation", Stat::StandardDeviation},
                                                                   {"summation", Stat::Summation},
                                                                   {"variance", Stat::Variance}}};

}  // namespace

std::optional<Stat> stat_of(std::string_view mode) {
    for (const auto& [name, stat] : kStats) {
        if (name == mode) return stat;
    }
    return std::nullopt;
}

double statistic(std::vector<double>& v, Stat s) {
    if (v.empty()) return 0.0;
    const double n = static_cast<double>(v.size());
    double sum = 0;
    for (const double x : v) sum += x;
    const double mean = sum / n;
    const auto moment = [&](int k) {
        double total = 0;
        for (const double x : v) total += std::pow(x - mean, k);
        return total / n;
    };
    switch (s) {
    case Stat::Mean: return mean;
    case Stat::Summation: return sum;
    case Stat::Minimum: return *std::min_element(v.begin(), v.end());
    case Stat::Maximum: return *std::max_element(v.begin(), v.end());
    case Stat::Range: return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
    case Stat::Median: {
        std::sort(v.begin(), v.end());
        const std::size_t k = v.size();
        return k % 2 == 1 ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2.0;
    }
    case Stat::Variance: return moment(2);
    case Stat::StandardDeviation: return std::sqrt(moment(2));
    case Stat::Skewness: {  // (−2..2 to 0..1, 0.5 symmetric)
        const double sd = std::sqrt(moment(2));
        return sd < 1e-6 ? 0.5 : std::clamp(moment(3) / (sd * sd * sd) / 4.0 + 0.5, 0.0, 1.0);
    }
    case Stat::Kurtosis: {  // (0..10 to 0..1)
        const double var = moment(2);
        return var < 1e-12 ? 0.0 : std::clamp(moment(4) / (var * var) / 10.0, 0.0, 1.0);
    }
    case Stat::Entropy: {
        if (v.size() < 2) return 0.0;
        std::array<unsigned, 256> counts{};
        for (const double x : v) ++counts[static_cast<std::size_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255.0))];
        double h = 0;
        for (const unsigned c : counts) {
            if (c == 0) continue;
            const double p = c / n;
            h -= p * std::log2(p);
        }
        return h / std::log2(n);
    }
    }
    return 0.0;
}

std::optional<double> of_one(Stat stat) {
    switch (stat) {
    case Stat::Mean:
    case Stat::Median:
    case Stat::Minimum:
    case Stat::Maximum:
    case Stat::Summation: return std::nullopt;
    case Stat::Skewness: return 0.5;
    default: return 0.0;
    }
}

void combine_pixel(const double* values, std::size_t layers, unsigned colours, Stat stat, double* out, std::vector<double>& scratch) {
    const unsigned all = colours + 1;
    double alpha = 0;
    for (std::size_t k = 0; k < layers; ++k) alpha = std::max(alpha, values[k * all + colours]);
    std::fill(out, out + all, 0.0);
    if (alpha <= 0) return;
    for (unsigned c = 0; c < colours; ++c) {
        scratch.clear();
        for (std::size_t k = 0; k < layers; ++k) {
            if (values[k * all + colours] > 0) scratch.push_back(values[k * all + c]);
        }
        out[c] = std::clamp(statistic(scratch, stat), 0.0, 1.0);
    }
    out[colours] = std::min(alpha, 1.0);
}

}  // namespace genko::render::stack
