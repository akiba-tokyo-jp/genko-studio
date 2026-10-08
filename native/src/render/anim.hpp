#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "render/image.hpp"

// The pictures of an animation page (Python's genko/anim.py: onion, with_onion, render_frame, export; the timeline
// itself is core's: core/anim.hpp): one frame as it plays (the cels its exposure sheet shows, cut to the camera), the
// onion skin and the light table while drawing, and the whole animation written out as a moving picture or numbered
// PNGs (render/movie.hpp).

namespace genko::render::anim {

inline constexpr std::array<std::string_view, 5> kFormats{"gif", "webp", "png", "mp4", "frames"};

// The colours of the onion skin: the frames before in red, after in blue (as CLIP STUDIO PAINT shows them); the light
// table's cels in green.
inline constexpr std::array<int, 3> kBeforeRgb{220, 60, 60};
inline constexpr std::array<int, 3> kAfterRgb{50, 110, 230};
inline constexpr std::array<int, 3> kLightRgb{120, 170, 120};

// The cels before and after the one shown, faint and tinted, and the light table's cels ("RGBA" of the page's size at
// this resolution); nothing for a page that is not an animation, or when there is nothing to show.
std::optional<Image> onion(const core::Page& page, std::int64_t frame, int dpi, std::int64_t before = 1, std::int64_t after = 1,
                           double strength = 0.35, const core::Document* episode = nullptr);

// The picture with the onion skin over it (in its own mode when RGB or RGBA, else RGB); the picture as it is when
// there is none.
Image with_onion(const Image& image, const core::Page& page, std::int64_t frame, int dpi, const core::Document* episode = nullptr,
                 std::int64_t before = 1, std::int64_t after = 1, double strength = 0.35);

// One frame as it plays (RGB): the page at that frame drawn for print (without the monochrome finish), cut to the
// camera (or, without camera work, to the area: trim | bleed | paper), at `size` when given (Lanczos). `skip_unported`:
// what this build does not draw yet is left out (a preview on screen only).
Image render_frame(const core::Page& page, std::int64_t frame, int dpi, const core::Document* episode = nullptr, bool camera = true,
                   std::string_view area = "trim", std::optional<Size> size = std::nullopt, bool skip_unported = false);

// anim.export: the animation as a moving picture (gif | webp | png | mp4) or a folder of numbered PNGs (frames: dest is
// the folder, frame_0001.png …). The format is `fmt`, else dest's suffix, else gif; `width` scales it to that many
// pixels across. The files written. PyValueError "the page is not an animation (set_animation first)", "format must
// be one of gif, webp, png, mp4, frames"; the moving picture's own errors.
std::vector<std::filesystem::path> export_animation(const core::Page& page, const std::filesystem::path& dest,
                                                    const core::Document* episode = nullptr, int dpi = 100,
                                                    std::optional<std::string> fmt = std::nullopt, bool camera = true,
                                                    std::string_view area = "trim", std::optional<int> width = std::nullopt);

}  // namespace genko::render::anim
