// What a picture the codecs read may be (render/codecs/limits.hpp).

#include "render/codecs/limits.hpp"

#include <limits>
#include <string>

#include "core/error.hpp"

namespace genko::render::codecs {

Limits Limits::none() {
    Limits out;
    out.max_width = std::numeric_limits<std::int64_t>::max();
    out.max_height = std::numeric_limits<std::int64_t>::max();
    out.max_pixels = std::numeric_limits<std::int64_t>::max();
    out.max_alloc = std::numeric_limits<std::uint64_t>::max();
    return out;
}

void Limits::check(std::int64_t width, std::int64_t height, Layout layout, Sample sample) const {
    check_bytes(width, height, std::uint64_t(channels(layout)) * sample_bytes(sample));
}

void Limits::check_bytes(std::int64_t width, std::int64_t height, std::uint64_t bytes_per_pixel) const {
    const std::string size = std::to_string(width) + "×" + std::to_string(height);
    if (width <= 0 || height <= 0) throw core::Error("value", "the picture has no pixels (" + size + ")");
    if (width > max_width || height > max_height) {
        throw core::Error("image_too_large", "the picture is too large (" + size + " pixels; at most " + std::to_string(max_width) + "×" +
                                                 std::to_string(max_height) + ")");
    }
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);  // (two sides of at most 2^63)
    if (width > 0xffffffffLL || height > 0xffffffffLL || pixels > static_cast<std::uint64_t>(max_pixels)) {
        throw core::Error("image_too_large", "the picture is too large (" + size + " pixels; at most " + std::to_string(max_pixels) + " pixels)");
    }
    if (bytes_per_pixel > 0 && pixels > std::numeric_limits<std::uint64_t>::max() / bytes_per_pixel) {
        throw core::Error("image_too_large", "the picture is too large to count in this computer's memory");
    }
    const std::uint64_t bytes = pixels * bytes_per_pixel;
    if (bytes > max_alloc || bytes > std::numeric_limits<std::size_t>::max() / 2) {
        throw core::Error("image_too_large", "the picture is too large to read at once (" + std::to_string(bytes) + " bytes; at most " +
                                                 std::to_string(max_alloc) + ")");
    }
}

}  // namespace genko::render::codecs
