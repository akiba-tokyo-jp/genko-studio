#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "core/json.hpp"

// pixel-aspect@1 (PC5 H-f, the user's decision 10; SPEC.md VIEW-01「ピクセル縦横比」, docs/cpp-migration/schema-v4.md
// 「ピクセル縦横比（pixel-aspect@1）」): Photoshop's 表示 → ピクセル縦横比 — a page's pixels taken as wider (or narrower) than
// they are tall, as a video frame's are. For the view only: ピクセル縦横比を補正 widens the canvas by it; the pixels, what is
// drawn and where (every tool works in the page's own square pixels) and every file written (pictures, PDF, PSD's pixels,
// prints) stay as they are. The page's "pixel_aspect": a number, 0.1..10 (1: square — nothing written, no feature); a PSD's
// image resource 1064 (render/psd_pixel_aspect). The ratios of the menu as PhotoCraft v0.2.0's view_cmds.rs pixel_aspect_ratio
// (MIT, native/third_party/photocraft/NOTICE.md).

namespace genko::core {

class OpRegistry;
struct Document;

inline constexpr std::string_view kPixelAspectFeature = "pixel-aspect@1";
inline constexpr std::string_view kPixelAspectKey = "pixel_aspect";
// (カスタムピクセル縦横比…'s range, PhotoCraft's custom_par)
inline constexpr double kPixelAspectMin = 0.1;
inline constexpr double kPixelAspectMax = 10.0;

// 表示 → ピクセル縦横比's ratios, in the menu's order: their id (the ledger's view.pixelAspectRatio.<id>), their name (Photoshop's
// Japanese words) and their ratio.
struct PixelAspectPreset {
    std::string_view id;
    std::string_view name;
    double ratio = 1.0;
};
std::span<const PixelAspectPreset> pixel_aspect_presets();
// The preset of this ratio (none: a ratio of its own).
std::optional<PixelAspectPreset> pixel_aspect_preset_of(double ratio);

// A ratio is square (written as none).
bool square_pixels(double ratio);
// A ratio a page may have (finite, 0.1..10).
bool pixel_aspect_allowed(double ratio);
// The page's "pixel_aspect" read (core::Error("value") for anything but a number in range).
double pixel_aspect_from_json(const Json& value);
// Some page's pixels are not square: the book names pixel-aspect@1.
bool uses_pixel_aspect(const Document& doc);
// Every page's ratio is one a page may have (core::Error("value") otherwise: a book that cannot be written so).
void validate_pixel_aspect(const Document& doc);

// set_pixel_aspect {"page", "ratio": 0.1..10 | "preset": id} → {"page", "pixel_aspect"}: the page's pixel aspect ratio (a
// change of the book, undone as any; nothing drawn changes). Not an agent's op (MCP): the window's and the CLI's.
void register_pixel_aspect_ops(OpRegistry& registry);

}  // namespace genko::core
