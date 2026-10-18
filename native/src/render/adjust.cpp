#include "render/adjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "render/page_internal.hpp"

namespace genko::render::adjust {

namespace {

unsigned char quantized(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// (PC2a: the records' conversions known to core before any correction is drawn — every way draws through this file)
[[maybe_unused]] const bool kToneBridge = (install_tone_bridge(), true);

}  // namespace

Image apply_8bit(const Image& rgba_in, const core::AdjustFunction& f, Point origin) {
    const Image rgba = rgba_in.mode() == "RGBA" ? rgba_in : rgba_in.convert("RGBA");
    std::string bytes = rgba.tobytes();
    auto* p = reinterpret_cast<unsigned char*>(bytes.data());
    const std::size_t n = bytes.size() / 4;
    if (const auto tables = f.tables()) {
        for (std::size_t i = 0; i < n; ++i)
            for (unsigned c = 0; c < 3; ++c) p[i * 4 + c] = tables->rgb[c][p[i * 4 + c]];
        return Image::frombytes("RGBA", rgba.size(), bytes);
    }
    const auto through = [&f](const unsigned char* q, int x, int y) {
        const auto out = f.apply({q[0] / 255.0, q[1] / 255.0, q[2] / 255.0}, x, y);
        return std::array<unsigned char, 3>{quantized(out[0]), quantized(out[1]), quantized(out[2])};
    };
    if (f.positional()) {
        const int w = std::max(1, rgba.width());
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char* q = p + i * 4;
            const auto out = through(q, origin.x + static_cast<int>(i % static_cast<std::size_t>(w)), origin.y + static_cast<int>(i / static_cast<std::size_t>(w)));
            std::copy(out.begin(), out.end(), q);
        }
        return Image::frombytes("RGBA", rgba.size(), bytes);
    }
    std::unordered_map<std::uint32_t, std::array<unsigned char, 3>> known;  // (each colour once: a page has few)
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char* q = p + i * 4;
        const std::uint32_t key = (std::uint32_t(q[0]) << 16) | (std::uint32_t(q[1]) << 8) | q[2];
        auto it = known.find(key);
        if (it == known.end()) {
            if (known.size() > (1u << 20)) known.clear();  // (a picture of very many colours: kept to a bound)
            it = known.emplace(key, through(q, 0, 0)).first;
        }
        std::copy(it->second.begin(), it->second.end(), q);
    }
    return Image::frombytes("RGBA", rgba.size(), bytes);
}

Image blend_8bit(const Image& below, const Image& corrected, std::string_view mode) {
    if (mode.empty() || mode == "normal") return corrected;
    const std::vector<Image> b = (below.mode() == "RGBA" ? below : below.convert("RGBA")).split();
    const std::vector<Image> c = (corrected.mode() == "RGBA" ? corrected : corrected.convert("RGBA")).split();
    Image mixed = detail::blend_colours(Image::merge("RGB", {b[0], b[1], b[2]}), Image::merge("RGB", {c[0], c[1], c[2]}), mode).convert("RGBA");
    mixed.putalpha(b[3]);
    return mixed;
}

}  // namespace genko::render::adjust
