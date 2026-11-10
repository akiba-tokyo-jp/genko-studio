// PC5 H-f: the PSD's pixel aspect ratio (image resource 1064) — render/psd_pixel_aspect.hpp.

#include "render/psd_pixel_aspect.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

#include "core/pixel_aspect.hpp"

namespace genko::render::psd_pixel_aspect {

std::string bytes_of(double ratio) {
    std::string out;
    for (int k = 3; k >= 0; --k) out += static_cast<char>((std::uint32_t{2} >> (8 * k)) & 0xff);
    const auto bits = std::bit_cast<std::uint64_t>(ratio);
    for (int k = 7; k >= 0; --k) out += static_cast<char>((bits >> (8 * k)) & 0xff);
    return out;
}

std::optional<double> ratio_of(std::string_view data) {
    if (data.size() < 12) return std::nullopt;
    std::uint32_t version = 0;
    for (std::size_t k = 0; k < 4; ++k) version = (version << 8) | static_cast<unsigned char>(data[k]);
    if (version != 1 && version != 2) return std::nullopt;
    std::uint64_t bits = 0;
    for (std::size_t k = 4; k < 12; ++k) bits = (bits << 8) | static_cast<unsigned char>(data[k]);
    const double ratio = std::bit_cast<double>(bits);
    if (!std::isfinite(ratio)) return std::nullopt;
    return ratio;
}

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context) {
    // (a page made of the file only, as the guides — a PSD read onto a page keeps the page's own ratio —; a ratio of a page's)
    if (!context.opened || context.page_had) return false;
    const std::optional<double> ratio = ratio_of(data);
    if (!ratio || core::square_pixels(*ratio) || !core::pixel_aspect_allowed(*ratio)) return false;
    page.pixel_aspect = *ratio;
    if (context.report != nullptr) (*context.report)["pixel_aspect"] = *ratio;
    return true;
}

std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext&) {
    if (core::square_pixels(page.pixel_aspect)) return std::nullopt;
    return bytes_of(page.pixel_aspect);
}

std::string fingerprint(const core::Page& page, const core::Document*) { return psd_production::fingerprint_of(core::Json(page.pixel_aspect)); }

}  // namespace genko::render::psd_pixel_aspect
