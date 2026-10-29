#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// slices@1 (PC5 節 J、SPEC.md PROD-01「スライス」・schema-v4.md「制作の補助」): a page's slices (Web 用に保存's rectangles).
// The page's `slices`:
//   {"group": str, "next_id": 1.., "list": [{"id": 1.., "origin": "user" | "layer", "layer"?: layer id, "name": str,
//     "rect_mm": [x, y, w, h], "kind": "image" | "noImage" | "table", "url": str, "target": str, "message": str,
//     "alt": str, "cell_text": str, "cell_text_is_html": bool, "align": [0..4, 0..4], "background": "#rrggbb" | null,
//     "outsets": [t, l, b, r]}]}
// The user's slices and the layer-based ones (their rect the layer's bounds with its effects and the outsets, made
// again each time: J-b); the auto slices are never kept (made again around them, as Photoshop does). After PhotoCraft
// v0.2.0's doc/src/slices.rs (MIT). The book names slices@1 while a page has one. PC5 J-0 is the frame; the slices'
// ops, tools, export and PSD 1050 are J-b's (render/ops_slices, app/canvas_slices, formats/web_export,
// render/psd_production).

namespace genko::core {

struct Document;

inline constexpr std::string_view kSlicesFeature = "slices@1";
inline constexpr std::string_view kSlicesKey = "slices";
inline constexpr std::size_t kSlicesMax = 4096;              // (as many as a PSD's kept resources: psd-keep@1)
inline constexpr std::int64_t kSliceIdMax = 2147483647;
inline constexpr std::size_t kSliceTextMax = 65536;          // bytes of one of its texts
inline constexpr int kSliceOutsetMax = 100000;               // page pixels

struct Slice {
    std::int64_t id = 1;
    std::string origin = "user";  // user | layer
    std::optional<std::string> layer;
    std::string name;
    std::array<double, 4> rect_mm{};
    std::string kind = "image";  // image | noImage | table
    std::string url, target, message, alt, cell_text;
    bool cell_text_is_html = false;
    std::array<int, 2> align{};  // horizontal, vertical (0 default …4)
    std::optional<std::string> background;  // "#rrggbb"
    std::array<int, 4> outsets{};           // top, left, bottom, right

    bool operator==(const Slice&) const = default;
};

struct Slices {
    std::string group;
    std::int64_t next_id = 1;
    std::vector<Slice> list;

    // (none at all: every slice deleted keeps the numbering — next_id, group — the page had, written with the page)
    bool empty() const { return list.empty() && next_id == 1 && group.empty(); }
    bool operator==(const Slices&) const = default;
};

Json to_json(const Slices& slices);
// core::Error("value") says what is not so.
Slices slices_from_json(const Json& value);
void check_slices(const Slices& slices);

bool uses_slices(const Document& doc);
void validate_slices(const Document& doc);

}  // namespace genko::core
