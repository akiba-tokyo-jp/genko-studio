#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/artboards.hpp"

// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」): a PSD's artboard block — a top-level group's artb (Photoshop's older
// artd and abdd read too), the descriptor of its board's rectangle on the canvas, its preset's name and its background —
// read and written. After PhotoCraft v0.2.0's crates/io/src/comps_map.rs (67-110, MIT, native/third_party/photocraft/
// NOTICE.md).

namespace genko::render::psd_artboards {

// The global tagged block that marks a PSD of artboards this build wrote (formats::export_artboards_psd): opening it leaves
// out the paper and the panels' empty layer every page's PSD has (formats::split_psd_artboards). Its payload, even in length.
inline constexpr std::string_view kGenkoMarkKey = "GnkA";
inline constexpr std::string_view kGenkoMark{"genko artboards\0", 16};

// The keys of an artboard's block.
bool artboard_key(std::string_view key);

struct Board {
    std::array<std::int64_t, 4> rect{};  // left, top, right, bottom (the canvas's pixels)
    std::string preset;
    std::string background = "white";    // white | black | transparent | custom
    std::optional<std::array<int, 3>> rgb;
};

// The block's board, or none (a block that cannot be read: no board).
std::optional<Board> read(std::string_view block);
// The artb block of an artboard lying at `rect` (left, top, right, bottom) on the canvas: its payload, even in length.
std::string write(const core::Artboard& artboard, const std::array<std::int64_t, 4>& rect);

}  // namespace genko::render::psd_artboards
