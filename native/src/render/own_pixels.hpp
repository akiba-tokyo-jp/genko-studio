#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/resample.hpp"

// PC3 C-c (SPEC.md COLOR-07): a paint layer's own pixels — not its lines, patches or shape parts — held to be moved
// and drawn again (render/layer_transform: the transforms; render/distort_ops: Content-Aware Scale and Liquify): its
// 8-bit PNG's RGBA, its precise raster (GKCR) or its inks (GKIR), on its own grid stretched over the page, each pixel's
// colours and alpha as floats (an ink raster's values as stored, normalized: moved and mixed premultiplied they are the
// same in any of its encodings; a 32-bit precise raster's colours past 1 kept).

namespace genko::render::own_pixels {

// A window of a grid (pixels, [x0, x1) × [y0, y1)).
struct Window {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
// The window kept on a width × height grid (none of its size: as it is).
Window clamp_window(Window w, int width, int height);
// The window of the box from (x0, y0) to (x1, y1), its edges rounded out and `pad` pixels added round it.
Window window_of(double x0, double y0, double x1, double y1, int pad);

class OwnPixels {
public:
    enum class Kind { Eight, Precise, Inks };

    // The layer's own pixels; none when it has none (or they have no alpha).
    static std::optional<OwnPixels> of(const core::Page& page, const core::Layer& layer);

    Kind kind() const { return kind_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    bool hdr() const { return hdr_; }

    // Pixel i's channels (the alpha last), and set.
    void get(std::size_t i, float* v) const;
    void put(std::size_t i, const float* v);
    // The window as a plane (its alpha last).
    resample::Plane read(const Window& w) const;
    // The box of the pixels that show (an alpha above 0); none: none.
    std::optional<Window> shown() const;
    // What stays in the window: each pixel's alpha × keep (window-sized, rows from the top; none: all taken).
    void leave(const Window& w, const std::vector<float>* keep);
    // A plane laid over the pixels with its pixel (0, 0) at (x0, y0): normal, premultiplied.
    void over(const resample::Plane& p, int x0, int y0);
    // The plane written in the window as it is (the window the plane's size).
    void write(const resample::Plane& p, int x0, int y0);
    // Back into the layer (when anything changed).
    void store(core::Page& page, core::Layer& layer);

private:
    std::size_t index(int x, int y) const { return std::size_t(y) * std::size_t(width_) + std::size_t(x); }

    Kind kind_ = Kind::Eight;
    int width_ = 0, height_ = 0, channels_ = 4;
    bool hdr_ = false;
    bool changed_ = false;
    std::string bytes_;
    std::shared_ptr<core::ColorRasterEdit> precise_;
    core::InkPixels inks_;
};

// A picture as a plane with its alpha (an "L" picture — a patch's mask — as a white of that alpha: `grey` says so), and
// back.
resample::Plane plane_of_picture(const Image& picture, bool& grey);
Image picture_of_plane(const resample::Plane& plane, bool grey);

// The memory a move of `pixels` pixels of `channels` floats holds while it works (a source window, its premultiplied
// doubles, the destination): what is held in the imaging budget.
inline std::uint64_t work_bytes(std::uint64_t pixels, int channels) { return pixels * std::uint64_t(channels) * (4 + 8 + 4); }

}  // namespace genko::render::own_pixels
