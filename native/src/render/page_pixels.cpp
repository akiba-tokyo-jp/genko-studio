#include "render/page_pixels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyops.hpp"
#include "render/fill.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "render/smart_filters.hpp"

// The page's pixels as PhotoCraft v0.2.0 moves a document's surfaces (crates/engine/src/image_cmds.rs 98-330: Image Size's
// resize_surface, Canvas Size's and Crop's translate_surface; canvas_geom.rs 200-235: Turn::pixel_map; mode_cmds.rs 56-140:
// Arbitrary's warp_surface), MIT: native/third_party/photocraft/NOTICE.md — on Genko's grids, each stretched over the page.

namespace genko::render::page_pixels {

namespace {

using core::page_geometry::Mapping;

// An exact move of a grid of w × h onto one of nw × nh: a turn of the whole of it, or a shift by whole pixels.
struct Exact {
    int w = 0, h = 0, nw = 0, nh = 0;
    bool turned = false;
    resample::Turn turn = resample::Turn::FlipH;
    int dx = 0, dy = 0;

    // The old pixel (x, y) the new one (X, Y) takes: false past the old grid (a shift's; `carry`: its nearest edge pixel).
    bool from(int X, int Y, bool carry, int& x, int& y) const {
        if (turned) {
            switch (turn) {
                case resample::Turn::FlipH: x = w - 1 - X; y = Y; break;
                case resample::Turn::FlipV: x = X; y = h - 1 - Y; break;
                case resample::Turn::Rotate180: x = w - 1 - X; y = h - 1 - Y; break;
                case resample::Turn::Cw90: x = Y; y = h - 1 - X; break;
                case resample::Turn::Ccw90: x = w - 1 - Y; y = X; break;
            }
            return true;
        }
        x = X - dx;
        y = Y - dy;
        if (x >= 0 && y >= 0 && x < w && y < h) return true;
        if (!carry || w <= 0 || h <= 0) return false;
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return true;
    }
};

bool swaps(const How& how) { return how.kind == How::Kind::Turn && (how.turn == resample::Turn::Cw90 || how.turn == resample::Turn::Ccw90); }

int whole(double v) { return static_cast<int>(std::lround(v)); }

Exact exact_of(const How& how, const Mapping& m, int w, int h, const core::PageSpec& before, const core::PageSpec& after) {
    const Grid g = grid_after(how, w, h, before, after);
    Exact e{w, h, g.width, g.height};
    if (how.kind == How::Kind::Turn) {
        e.turned = true;
        e.turn = how.turn;
        return e;
    }
    // (the move in the grid's own pixels, to the nearest whole one)
    e.dx = whole(m.m[4] * w / before.width_mm.value());
    e.dy = whole(m.m[5] * h / before.height_mm.value());
    return e;
}

// The pixels (each `px` bytes) of a grid moved exactly; what the move leaves bare is zero (or the edge carried on).
std::string moved_bytes(std::string_view in, std::size_t px, const Exact& e, bool carry) {
    std::string out(std::size_t(e.nw) * std::size_t(e.nh) * px, '\0');
    for (int Y = 0; Y < e.nh; ++Y) {
        for (int X = 0; X < e.nw; ++X) {
            int x = 0, y = 0;
            if (!e.from(X, Y, carry, x, y)) continue;
            std::memcpy(out.data() + (std::size_t(Y) * std::size_t(e.nw) + std::size_t(X)) * px,
                        in.data() + (std::size_t(y) * std::size_t(e.w) + std::size_t(x)) * px, px);
        }
    }
    return out;
}

// A grid's pixels → the page's mm (an origin and the mm a pixel spans across and down), and back (an origin and the
// pixels a mm spans).
struct GridFrame {
    double x = 0, y = 0, sx = 1, sy = 1;
};

// The homography taking the old grid's pixels to the new one's through the page's map.
resample::Homography grid_map(const Mapping& m, const GridFrame& from, const GridFrame& to) {
    const auto& a = m.m;
    // mm = from.(x, y) + px · from.(sx, sy); mm' = m(mm); px' = (mm' − to.(x, y)) · to.(sx, sy)
    const double ex = a[0] * from.x + a[2] * from.y + a[4] - to.x;
    const double ey = a[1] * from.x + a[3] * from.y + a[5] - to.y;
    return resample::Homography{{to.sx * a[0] * from.sx, to.sx * a[2] * from.sy, to.sx * ex, to.sy * a[1] * from.sx, to.sy * a[3] * from.sy,
                                 to.sy * ey, 0.0, 0.0, 1.0}};
}

resample::Homography page_grid_map(const Mapping& m, int w, int h, const Grid& g, const core::PageSpec& before, const core::PageSpec& after) {
    return grid_map(m, GridFrame{0, 0, before.width_mm.value() / w, before.height_mm.value() / h},
                    GridFrame{0, 0, g.width / after.width_mm.value(), g.height / after.height_mm.value()});
}

// A plane drawn again as `how` draws it (Resize or Warp; `bilinear`: a mask's or a channel's, resized bilinear): its work
// held in the imaging budget.
resample::Plane redrawn(const resample::Plane& plane, const How& how, const resample::Homography& h, const Grid& g, bool bilinear) {
    const std::uint64_t n = std::uint64_t(plane.channels);
    const image_ops::Held held((std::uint64_t(plane.width) * std::uint64_t(plane.height) * 8 + std::uint64_t(g.width) * std::uint64_t(g.height) * 4) * n);
    if (how.kind == How::Kind::Resize) {
        // (the picture is as big as its page: its edge pixels carried on past it, so the resized one has no clear rim)
        return resample::resize(plane, g.width, g.height, bilinear ? resample::Filter::Bilinear : how.filter, true);
    }
    return resample::project(plane, h, g.width, g.height, how.interp);
}

std::uint64_t plane_bytes(int w, int h, int n) { return std::uint64_t(w) * std::uint64_t(h) * std::uint64_t(n) * 4; }

std::string gkcr_header(std::uint32_t w, std::uint32_t h, unsigned sample_bytes) {
    std::string out("GKCR\1\2\0\0", 8);
    out[5] = static_cast<char>(sample_bytes);
    for (const std::uint32_t v : {w, h}) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
    return out;
}

// A patch's picture turned exactly (its mode kept: L, LA, RGB, RGBA; anything else as RGBA).
core::Bytes turned_patch(const std::string& png, resample::Turn turn) {
    Image picture = selection::open_picture(png);
    const std::string mode(picture.mode());
    std::size_t px = mode == "L" ? 1 : mode == "LA" ? 2 : mode == "RGB" ? 3 : mode == "RGBA" ? 4 : 0;
    if (px == 0) {
        picture = picture.convert("RGBA");
        px = 4;
    }
    How how;
    how.kind = How::Kind::Turn;
    how.turn = turn;
    const bool swap = swaps(how);
    Exact e{picture.width(), picture.height(), swap ? picture.height() : picture.width(), swap ? picture.width() : picture.height()};
    e.turned = true;
    e.turn = turn;
    const std::string bytes = picture.tobytes();
    const std::string out_mode = px == 4 ? std::string("RGBA") : mode;
    return fills::png_bytes(Image::frombytes(out_mode, Size{e.nw, e.nh}, moved_bytes(bytes, px, e, false)));
}

std::array<double, 4> box_of(const core::Json& value) {
    const std::vector<double> b = core::unpack_floats(value, 4);
    return {b[0], b[1], b[2], b[3]};
}

// A saved mask area ({"mask": {"box", "png"}}) through a map that turns or mirrors it: its picture turned exactly, or
// projected into the box of its corners (outside: not selected).
void move_mask_area(core::Json& area, const How& how, const Mapping& m) {
    const auto mask = area.find("mask");
    if (mask == area.end() || !mask->is_object() || !mask->contains("box") || !mask->contains("png")) return;
    const std::array<double, 4> box = box_of((*mask)["box"]);
    const Image picture = selection::open_picture(selection::b64decode((*mask)["png"])).convert("L");
    const std::array<double, 4> moved = core::page_geometry::map_box(box, m);
    Image out;
    if (how.kind == How::Kind::Turn) {
        const bool swap = swaps(how);
        Exact e{picture.width(), picture.height(), swap ? picture.height() : picture.width(), swap ? picture.width() : picture.height()};
        e.turned = true;
        e.turn = how.turn;
        out = Image::frombytes("L", Size{e.nw, e.nh}, moved_bytes(picture.tobytes(), 1, e, false));
    } else {
        const double pw = box[2] > 0 ? picture.width() / box[2] : 1.0, ph = box[3] > 0 ? picture.height() / box[3] : 1.0;
        const Grid g{std::max(1, whole(moved[2] * pw)), std::max(1, whole(moved[3] * ph))};
        limits::check_picture(g.width, g.height, "the saved area on the new paper");
        resample::Plane plane(picture.width(), picture.height(), 1, true);  // (one value as an alpha: outside 0)
        const std::string in = picture.tobytes();
        for (std::size_t i = 0; i < in.size(); ++i) plane.v[i] = static_cast<unsigned char>(in[i]) / 255.0f;
        const resample::Homography h =
            grid_map(m, GridFrame{box[0], box[1], 1.0 / pw, 1.0 / ph}, GridFrame{moved[0], moved[1], g.width / std::max(moved[2], 1e-9), g.height / std::max(moved[3], 1e-9)});
        const resample::Plane done = redrawn(plane, how, h, g, true);
        std::string bytes(std::size_t(g.width) * std::size_t(g.height), '\0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(static_cast<double>(done.v[i]), 0.0, 1.0) * 255.0)));
        }
        out = Image::frombytes("L", Size{g.width, g.height}, bytes);
    }
    (*mask)["box"] = core::Json::array({moved[0], moved[1], moved[2], moved[3]});
    (*mask)["png"] = core::b64encode(fills::png_data(out));
}

}  // namespace

Grid grid_after(const How& how, int width, int height, const core::PageSpec& before, const core::PageSpec& after) {
    if (how.kind == How::Kind::Stretch) return Grid{width, height};
    if (how.kind == How::Kind::Turn) return swaps(how) ? Grid{height, width} : Grid{width, height};
    const double w = after.width_mm.value() * width / before.width_mm.value();
    const double h = after.height_mm.value() * height / before.height_mm.value();
    if (!std::isfinite(w) || !std::isfinite(h)) throw core::OpError("the paper must be a finite size");
    return Grid{std::max(1, whole(w)), std::max(1, whole(h))};
}

std::string move_png(const std::string& png, const How& how, const Mapping& m, const core::PageSpec& before, const core::PageSpec& after) {
    if (how.kind == How::Kind::Stretch) return png;
    const Image picture = selection::open_picture(png).convert("RGBA");
    const Grid g = grid_after(how, picture.width(), picture.height(), before, after);
    limits::check_picture(g.width, g.height, "the paint layer on the new paper");
    if (how.kind == How::Kind::Turn || how.kind == How::Kind::Shift) {
        const Exact e = exact_of(how, m, picture.width(), picture.height(), before, after);
        return write_png(Image::frombytes("RGBA", Size{e.nw, e.nh}, moved_bytes(picture.tobytes(), 4, e, false)));
    }
    const resample::Plane plane = resample::from_rgba8(picture);
    return write_png(resample::to_rgba8(redrawn(plane, how, page_grid_map(m, picture.width(), picture.height(), g, before, after), g, false)));
}

std::string move_mask(const std::string& png, const How& how, const Mapping& m, const core::PageSpec& before, const core::PageSpec& after) {
    if (how.kind == How::Kind::Stretch) return png;
    const Image picture = selection::open_picture(png).convert("L");
    const Grid g = grid_after(how, picture.width(), picture.height(), before, after);
    limits::check_picture(g.width, g.height, "the layer mask on the new paper");
    if (how.kind == How::Kind::Turn || how.kind == How::Kind::Shift) {
        const Exact e = exact_of(how, m, picture.width(), picture.height(), before, after);
        return write_png(Image::frombytes("L", Size{e.nw, e.nh}, moved_bytes(picture.tobytes(), 1, e, true)));
    }
    const resample::Plane plane = resample::from_gray(picture);  // (grey: its edge carried on)
    return write_png(resample::to_gray(redrawn(plane, how, page_grid_map(m, picture.width(), picture.height(), g, before, after), g, true)));
}

std::string move_precise(const std::string& gkcr, const How& how, const Mapping& m, const core::PageSpec& before, const core::PageSpec& after) {
    if (how.kind == How::Kind::Stretch) return gkcr;
    const core::ColorRasterView view(gkcr);
    const unsigned sample_bytes = static_cast<unsigned char>(gkcr[5]);
    const int w = static_cast<int>(view.width()), h = static_cast<int>(view.height());
    const Grid g = grid_after(how, w, h, before, after);
    if (double(g.width) * g.height > double(core::kColorRasterMaxPixels)) throw core::OpError("the precise layer on the new paper is too large");
    if (how.kind == How::Kind::Turn || how.kind == How::Kind::Shift) {
        const Exact e = exact_of(how, m, w, h, before, after);
        return gkcr_header(static_cast<std::uint32_t>(e.nw), static_cast<std::uint32_t>(e.nh), sample_bytes) +
               moved_bytes(std::string_view(gkcr).substr(16), 4 * sample_bytes, e, false);
    }
    const image_ops::Held held(plane_bytes(w, h, 4));
    resample::Plane plane(w, h, 4, true);
    plane.hdr = sample_bytes == 4;  // (f32: its colours above 1 kept)
    for (std::size_t i = 0; i < std::size_t(w) * std::size_t(h); ++i) {
        const std::array<double, 4> p = view.pixel(i);
        for (std::size_t c = 0; c < 4; ++c) plane.v[i * 4 + c] = static_cast<float>(p[c]);
    }
    const resample::Plane done = redrawn(plane, how, page_grid_map(m, w, h, g, before, after), g, false);
    return core::encode_color_pixels(static_cast<std::uint32_t>(g.width), static_cast<std::uint32_t>(g.height), sample_bytes == 4 ? "f32" : "u16",
                                     [&](std::size_t i) {
                                         const float* p = done.v.data() + i * 4;
                                         return std::array<double, 4>{p[0], p[1], p[2], std::clamp(static_cast<double>(p[3]), 0.0, 1.0)};
                                     });
}

std::string move_inks(const std::string& gkir, const How& how, const Mapping& m, const core::PageSpec& before, const core::PageSpec& after) {
    if (how.kind == How::Kind::Stretch) return gkir;
    const core::InkPixels in = core::decode_ink_raster(gkir);
    const int w = static_cast<int>(in.width), h = static_cast<int>(in.height);
    const Grid g = grid_after(how, w, h, before, after);
    if (std::uint64_t(g.width) > core::kInkRasterMaxSide || std::uint64_t(g.height) > core::kInkRasterMaxSide ||
        std::uint64_t(g.width) * std::uint64_t(g.height) > core::kInkRasterMaxPixels) {
        throw core::OpError("the inks on the new paper are too large");
    }
    core::InkPixels out = core::blank_ink_pixels(in.space, in.depth, static_cast<std::uint32_t>(g.width), static_cast<std::uint32_t>(g.height), in.planes);
    if (how.kind == How::Kind::Turn || how.kind == How::Kind::Shift) {
        const Exact e = exact_of(how, m, w, h, before, after);
        for (int Y = 0; Y < e.nh; ++Y) {
            for (int X = 0; X < e.nw; ++X) {
                int x = 0, y = 0;
                if (!e.from(X, Y, false, x, y)) continue;
                out.copy_pixel(std::size_t(Y) * std::size_t(e.nw) + std::size_t(X), in, std::size_t(y) * std::size_t(w) + std::size_t(x));
            }
        }
        return core::encode_ink_raster(out);
    }
    // (with an alpha: premultiplied as a layer's colours; a channel's one value as an alpha of its own: 0 outside)
    const unsigned n = in.channels();
    const bool alpha = in.has_alpha() || n == 1;
    const image_ops::Held held(plane_bytes(w, h, static_cast<int>(n)));
    resample::Plane plane(w, h, static_cast<int>(n), alpha);
    plane.hdr = in.depth == 32 && in.space == core::InkSpace::Gray && in.has_alpha();
    for (std::size_t i = 0; i < in.pixel_count(); ++i) {
        for (unsigned c = 0; c < n; ++c) plane.v[i * n + c] = static_cast<float>(in.value(i, c));
    }
    const resample::Plane done = redrawn(plane, how, page_grid_map(m, w, h, g, before, after), g, !in.has_alpha());
    for (std::size_t i = 0; i < out.pixel_count(); ++i) {
        for (unsigned c = 0; c < n; ++c) out.set(i, c, done.v[i * n + c]);
    }
    return core::encode_ink_raster(out);
}

core::page_geometry::PixelHooks hooks(core::Document& doc, const How& how, const core::PageSpec& before, const core::PageSpec& after) {
    core::page_geometry::PixelHooks out;
    core::Document* book = &doc;
    out.layer = [book, how, before, after](core::Layer& layer, const Mapping& m, const core::Page& was, const core::Page&) {
        if (how.kind == How::Kind::Stretch) return;
        if (layer.raster_png && !layer.raster_png->empty()) {
            layer.raster_png = std::make_shared<const std::string>(move_png(*layer.raster_png, how, m, before, after));
            layer.raster_memo = {};
            layer.raster_relpath.reset();
        }
        if (layer.color_raster) layer.color_raster = std::make_shared<const std::string>(move_precise(*layer.color_raster, how, m, before, after));
        if (layer.ink_raster) {
            layer.ink_raster = std::make_shared<const std::string>(move_inks(*layer.ink_raster, how, m, before, after));
            layer.ink_memo = {};
        }
        if (layer.mask && layer.mask->png && !layer.mask->png->empty()) {
            layer.mask->png = std::make_shared<const std::string>(move_mask(*layer.mask->png, how, m, before, after));
            layer.mask->memo = {};
        }
        // (smart-objects@1, PC3 A2: a smart object's filter mask, a page's mask as a layer's is; one that is not in the book
        // stays as it is — the book says so where it is read)
        if (layer.kind == core::LayerKind::Smart && layer.smart && layer.smart->is_object()) {
            const auto mask = layer.smart->find("filter_mask");
            if (mask != layer.smart->end() && mask->is_object() && mask->contains("asset") && (*mask)["asset"].is_string()) {
                if (const core::Bytes bytes = smart_filters::mask_bytes(*book, (*mask)["asset"].get<std::string>())) {
                    std::string moved = move_mask(*bytes, how, m, before, after);
                    const std::string ref = core::bytes_ref(moved);
                    book->smart_files[ref] = std::make_shared<const std::string>(std::move(moved));
                    (*mask)["asset"] = ref;
                }
            }
        }
        // (the pictures of its patches: turned with the page, or drawn again in the box of their corners; a move or a
        // scale only stretches them over their boxes, map_page's)
        if ((how.kind == How::Kind::Turn || how.kind == How::Kind::Warp) && !layer.patches.empty()) {
            const core::Layer* old = nullptr;
            for (const core::Layer& l : was.layers) {
                if (l.id == layer.id) old = &l;
            }
            std::vector<core::Patch> patches;
            for (std::size_t i = 0; i < layer.patches.size(); ++i) {
                core::Patch& patch = layer.patches[i];
                if (!patch.png) {
                    patches.push_back(std::move(patch));
                    continue;
                }
                if (how.kind == How::Kind::Turn) {
                    patch.png = turned_patch(*patch.png, how.turn);
                    patch.asset.reset();
                    patch.memo = {};
                    patches.push_back(std::move(patch));
                    continue;
                }
                if (old == nullptr || i >= old->patches.size()) continue;
                const core::Patch& was_patch = old->patches[i];
                std::optional<core::Patch> moved = selection::transform_patch(was_patch, selection::Matrix{m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5]},
                                                                              Resample::Bicubic);
                if (!moved) continue;  // (nothing of it left)
                if (const core::Json* id = core::get(was_patch.attrs, "id")) moved->attrs["id"] = *id;
                patches.push_back(std::move(*moved));
            }
            layer.patches = std::move(patches);
        }
    };
    out.channel = [how, before, after](core::Channel& channel, const Mapping& m, const core::Page&, const core::Page&) {
        if (how.kind == How::Kind::Stretch || !channel.raster) return;
        channel.raster = std::make_shared<const std::string>(move_inks(*channel.raster, how, m, before, after));
        channel.memo = {};
    };
    out.area = [how](core::Json& area, const Mapping& m, const core::Page&, const core::Page&) {
        move_mask_area(area, how, m);
        core::page_geometry::map_area(area, m);  // (its other parts; the mask is done)
    };
    return out;
}

}  // namespace genko::render::page_pixels
