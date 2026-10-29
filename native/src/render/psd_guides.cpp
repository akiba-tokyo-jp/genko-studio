// PC5 H-b: the PSD's guides (image resource 1032) — render/psd_guides.hpp.

#include "render/psd_guides.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::render::psd_guides {

namespace {

constexpr std::size_t kHeader = 16;      // version, the grid's cycles, the count
constexpr std::size_t kEntry = 5;        // the place (1/32 px) and the way
constexpr std::uint32_t kMaxGuides = 4096;  // (more than a page holds: a hostile count is bounded)

std::uint32_t u32(std::string_view data, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(data[at + k]);
    return v;
}

void put32(std::string& out, std::uint32_t v) {
    for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (8 * k)) & 0xff);
}

// The page's guides: (axis "v" | "h", at mm), as the rulers hold them.
std::vector<std::pair<std::string, double>> guides_of(const core::Page& page) {
    std::vector<std::pair<std::string, double>> out;
    if (!page.rulers.is_array()) return out;
    for (const core::Json& r : page.rulers) {
        if (!r.is_object() || !core::py_equals(core::py_get(r, "kind"), core::Json("guide"))) continue;
        const core::Json at = core::py_get(r, "at");
        const core::Json axis = core::py_get(r, "axis");
        if (!at.is_number() || !axis.is_string()) continue;
        const std::string way = axis.get<std::string>();
        if (way != "h" && way != "v") continue;
        out.emplace_back(way, at.get<double>());
    }
    return out;
}

}  // namespace

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context) {
    // (a page made of the file only — a PSD read onto a page brings no guides, as Photoshop's 配置 —; version 1, Photoshop's)
    if (!context.opened || context.page_had || context.onto == nullptr || data.size() < kHeader || u32(data, 0) != 1) return false;
    if (!(context.width_px > 0) || !(context.height_px > 0)) return false;
    const std::uint32_t count = std::min(u32(data, 12), kMaxGuides);
    const core::vector::Affine& onto = *context.onto;
    std::vector<std::pair<std::string, double>> had = guides_of(page);
    std::size_t made = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = kHeader + static_cast<std::size_t>(i) * kEntry;
        if (data.size() < at + kEntry) break;
        const double px = static_cast<double>(static_cast<std::int32_t>(u32(data, at))) / 32.0;
        const bool across = static_cast<unsigned char>(data[at + 4]) == 1;
        // (one far off the canvas — more than its own size beyond an edge — is left out: of no use, and a hostile number)
        const double side = across ? context.height_px : context.width_px;
        if (px < -side || px > 2 * side) continue;
        // (the file's canvas, fractions of it, where its picture is placed on the page)
        const double mm = across ? onto[3] * (px / context.height_px) + onto[5] : onto[0] * (px / context.width_px) + onto[4];
        if (!std::isfinite(mm)) continue;
        const double placed = core::py_round(mm, 3);
        const std::string axis = across ? "h" : "v";
        if (std::any_of(had.begin(), had.end(), [&](const auto& g) { return g.first == axis && std::abs(g.second - placed) < 0.01; })) continue;
        had.emplace_back(axis, placed);
        page.rulers.push_back(core::Json{{"id", core::new_id()}, {"kind", "guide"}, {"points", core::Json::array()}, {"active", true},
                                         {"visible", true}, {"axis", axis}, {"at", placed}});
        ++made;
    }
    if (made > 0 && context.report != nullptr) (*context.report)["guides"] = made;
    return true;
}

std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context) {
    const auto guides = guides_of(page);
    if (guides.empty()) return std::nullopt;
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!(w_mm > 0) || !(h_mm > 0) || !(context.width_px > 0) || !(context.height_px > 0)) return std::nullopt;
    // (the picture's pixels: mm at the resolution it is drawn at — its whole width rounded only at the edge —; without one,
    // the canvas's share)
    const auto to_px = [&](double at, bool across) {
        if (context.dpi > 0) return at / 25.4 * context.dpi;
        return across ? at / h_mm * context.height_px : at / w_mm * context.width_px;
    };
    std::string out;
    put32(out, 1);
    put32(out, 576);  // (the grid's cycles: Photoshop's own, as upstream writes them)
    put32(out, 576);
    put32(out, static_cast<std::uint32_t>(guides.size()));
    const auto place = [](double px) {
        const double v = std::round(px * 32.0);
        const double lo = static_cast<double>(std::numeric_limits<std::int32_t>::min());
        const double hi = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp(v, lo, hi)));
    };
    // (upright guides first, then those across — upstream's order)
    for (const char* way : {"v", "h"}) {
        for (const auto& [axis, at] : guides) {
            if (axis != way) continue;
            const bool across = axis == "h";
            const double px = to_px(at, across);
            put32(out, place(px));
            out += static_cast<char>(across ? 1 : 0);
        }
    }
    return out;
}

}  // namespace genko::render::psd_guides
