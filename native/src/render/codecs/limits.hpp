#pragma once

#include <cstdint>

#include "render/codecs/image.hpp"
#include "render/op_limits.hpp"

// What a picture the codecs read may be (PC4 F-0; PhotoCraft's Limits, crates/codecs/src/options.rs, with Genko's own
// bounds): its sides and pixels as an op's picture (render/op_limits.hpp: 100000 a side, 120 million pixels) and its
// decoded bytes as the work's memory (render::ImageAllocationBudget's 2 GiB). A reader checks them on the file's header,
// before any pixel is allocated (a decompression bomb is refused), and once more on what it made.

namespace genko::render::codecs {

inline constexpr std::uint64_t kAllocBudget = 2ULL * 1024 * 1024 * 1024;

struct Limits {
    std::int64_t max_width = limits::kSide;
    std::int64_t max_height = limits::kSide;
    std::int64_t max_pixels = limits::kPixels;
    std::uint64_t max_alloc = kAllocBudget;  // the decoded picture's bytes

    // No bounds but what the machine can count (tests).
    static Limits none();
    // A picture of w × h of this layout and sample within the limits: core::Error("value") for a side of 0,
    // core::Error("image_too_large") past a limit ("the picture is too large (W×H pixels; …)").
    void check(std::int64_t width, std::int64_t height, Layout layout, Sample sample) const;
    void check_bytes(std::int64_t width, std::int64_t height, std::uint64_t bytes_per_pixel) const;
};

}  // namespace genko::render::codecs
