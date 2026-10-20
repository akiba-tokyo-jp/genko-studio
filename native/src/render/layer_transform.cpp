#include "render/layer_transform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>

#include "core/areas.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/fill_patches.hpp"
#include "render/image_ops.hpp"
#include "render/layer_masks.hpp"
#include "render/own_pixels.hpp"
#include "render/page.hpp"
#include "render/perspective_warp.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/smart_filters.hpp"
#include "render/warp.hpp"

// The warps' params and splits: PhotoCraft v0.2.0 crates/engine/src/warp_cmds.rs (75-260), the layers' transforms:
// transform_cmds.rs (22-175) and distort_cmds.rs (40-160); a smart object's warp in its contents' mm and its distortions
// kept as smart filters (PC3 A5): warp_cmds.rs (48-72, 128-150) and distort_cmds.rs (233-272). MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::layer_transform {

namespace pg = core::page_geometry;
using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpError;
using core::Page;
using own_pixels::clamp_window;
using own_pixels::OwnPixels;
using own_pixels::picture_of_plane;
using own_pixels::plane_of_picture;
using own_pixels::Window;
using own_pixels::window_of;
using own_pixels::work_bytes;

namespace {

constexpr int kDpi = 200;                       // (the working grid: what the content box and a puppet's mesh are on)
constexpr double kPx = kDpi / 25.4;             // (working pixels a mm)
constexpr double kStep = 1.5;                   // (mm: the longest piece of a line or a segment sent through a bent map)

double r4(double v) { return core::py_round(v, 4); }

double page_w(const Page& page) { return page.spec.width_mm.value(); }
double page_h(const Page& page) { return page.spec.height_mm.value(); }

std::size_t layer_index(const Page& page, const std::string& id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i)
        if (page.layers[i].id == id) return i;
    throw core::OpKeyError(core::py_repr_str(id));
}

// --- a map on a grid -------------------------------------------------------------------------------------------------

// The map's homography in a grid's pixels (sx, sy pixels a mm), from a window whose pixel (0, 0) is the grid's (gx, gy).
resample::Homography grid_homography(const Map& m, double sx, double sy, int gx, int gy) {
    const resample::Homography s{{sx, 0, 0, 0, sy, 0, 0, 0, 1}};
    const resample::Homography s_inv{{1.0 / sx, 0, 0, 0, 1.0 / sy, 0, 0, 0, 1}};
    const resample::Homography shift{{1, 0, static_cast<double>(gx), 0, 1, static_cast<double>(gy), 0, 0, 1}};
    const resample::Homography mm = m.kind == Map::Kind::Affine ? resample::Homography::affine(m.affine) : m.projective;
    return s.mul(mm).mul(s_inv).mul(shift);
}

// Does the homography take the grid's pixels onto its pixels (a whole move, a right-angle turn or a flip)?
bool keeps_grid(const resample::Homography& h) {
    if (!h.is_affine()) return false;
    const auto unit = [](double v) { return std::fabs(v) < 1e-12 || std::fabs(std::fabs(v) - 1.0) < 1e-12; };
    const auto whole = [](double v) { return std::fabs(v - std::round(v)) < 1e-6; };
    const auto& a = h.m;
    const bool perm = unit(a[0]) && unit(a[1]) && unit(a[3]) && unit(a[4]) &&
                      ((std::fabs(a[0]) > 0.5 && std::fabs(a[4]) > 0.5 && std::fabs(a[1]) < 0.5 && std::fabs(a[3]) < 0.5) ||
                       (std::fabs(a[1]) > 0.5 && std::fabs(a[3]) > 0.5 && std::fabs(a[0]) < 0.5 && std::fabs(a[4]) < 0.5));
    return perm && whole(a[2]) && whole(a[5]);
}

// A plane (its pixel (0, 0) at the grid's (gx, gy)) drawn through the map onto the grid (width × height, sx × sy pixels
// a mm); the window of the grid it lands on.
struct Landed {
    resample::Plane plane;
    Window at;
};

Landed through(const resample::Plane& src, int gx, int gy, double sx, double sy, int width, int height, const Map& m, resample::Interp interp) {
    Landed out;
    if (src.width <= 0 || src.height <= 0) return out;
    if (m.kind != Map::Kind::Mesh) {
        const resample::Homography h = grid_homography(m, sx, sy, gx, gy);
        double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
        for (const auto& [u, v] : {std::pair{0.0, 0.0}, std::pair{static_cast<double>(src.width), 0.0},
                                   std::pair{static_cast<double>(src.width), static_cast<double>(src.height)}, std::pair{0.0, static_cast<double>(src.height)}}) {
            const auto [px, py] = h.apply(u, v);
            if (!std::isfinite(px) || !std::isfinite(py)) return out;
            x0 = std::min(x0, px);
            y0 = std::min(y0, py);
            x1 = std::max(x1, px);
            y1 = std::max(y1, py);
        }
        out.at = clamp_window(window_of(x0, y0, x1, y1, 2), width, height);
        if (out.at.empty()) return out;
        // (a map taking the grid onto itself moves the pixels as they are: each value copied, none sampled)
        if (keeps_grid(h)) {
            if (const auto back = h.inverse()) {
                out.plane = resample::Plane(out.at.width(), out.at.height(), src.channels, src.alpha);
                out.plane.hdr = src.hdr;
                for (int y = 0; y < out.at.height(); ++y) {
                    for (int x = 0; x < out.at.width(); ++x) {
                        const auto [u, v] = back->apply(out.at.x0 + x + 0.5, out.at.y0 + y + 0.5);
                        const long i = std::lround(std::floor(u)), j = std::lround(std::floor(v));
                        if (i < 0 || j < 0 || i >= src.width || j >= src.height) continue;
                        std::copy_n(src.at(static_cast<int>(i), static_cast<int>(j)), src.channels, out.plane.at(x, y));
                    }
                }
                return out;
            }
        }
        out.plane = resample::project(src, h, out.at.width(), out.at.height(), interp, out.at.x0, out.at.y0);
        return out;
    }
    if (m.puppet) {
        const puppet::Deformed& d = *m.puppet;
        const double kx = sx / kPx, ky = sy / kPx;
        std::vector<resample::Corner> corners;
        corners.reserve(d.verts.size());
        double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
        for (std::size_t i = 0; i < d.verts.size(); ++i) {
            const double dx = d.verts[i][0] * kx, dy = d.verts[i][1] * ky;
            corners.push_back(resample::Corner{dx, dy, d.mesh.verts[i][0] * kx - gx, d.mesh.verts[i][1] * ky - gy});
            x0 = std::min(x0, dx);
            y0 = std::min(y0, dy);
            x1 = std::max(x1, dx);
            y1 = std::max(y1, dy);
        }
        if (corners.empty()) return out;
        std::vector<std::array<std::size_t, 3>> tris;
        tris.reserve(d.order.size());
        for (const std::size_t t : d.order) tris.push_back(d.mesh.tris[t]);
        out.at = clamp_window(window_of(x0, y0, x1, y1, 2), width, height);
        if (out.at.empty()) return out;
        out.plane = resample::triangles(src, corners, tris, out.at.width(), out.at.height(), interp, out.at.x0, out.at.y0);
        return out;
    }
    const auto f = [&](double u, double v) {
        const XY p = m.forward((u + gx) / sx, (v + gy) / sy);
        return std::pair<double, double>{p.x * sx, p.y * sy};
    };
    // (where the window lands: its edges and a grid inside it, sampled)
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    constexpr int kSamples = 24;
    for (int j = 0; j <= kSamples; ++j) {
        for (int i = 0; i <= kSamples; ++i) {
            const auto [px, py] = f(src.width * static_cast<double>(i) / kSamples, src.height * static_cast<double>(j) / kSamples);
            if (!std::isfinite(px) || !std::isfinite(py)) continue;
            x0 = std::min(x0, px);
            y0 = std::min(y0, py);
            x1 = std::max(x1, px);
            y1 = std::max(y1, py);
        }
    }
    if (x0 > x1) return out;
    const int pad = 4 + static_cast<int>(std::ceil(std::max(src.width, src.height) / static_cast<double>(kSamples)));
    out.at = clamp_window(window_of(x0, y0, x1, y1, pad), width, height);
    if (out.at.empty()) return out;
    out.plane = resample::mesh(src, f, out.at.width(), out.at.height(), interp, out.at.x0, out.at.y0);
    return out;
}

// The layer's own pixels moved: within `keep` (the window's share left behind: none, all taken).
void move_pixels(Page& page, Layer& layer, const Map& m, const std::optional<Json>& area, resample::Interp interp) {
    std::optional<OwnPixels> own = OwnPixels::of(page, layer);
    if (!own) return;
    const double sx = own->width() / page_w(page), sy = own->height() / page_h(page);
    std::optional<Window> win;
    std::vector<float> keep;
    if (area) {
        const std::vector<float> cov = image_ops::coverage(page, *area, static_cast<std::uint32_t>(own->width()),
                                                           static_cast<std::uint32_t>(own->height()), layer.raster_png != nullptr && !layer.color_raster);
        Window b{own->width(), own->height(), 0, 0};
        for (int y = 0; y < own->height(); ++y) {
            for (int x = 0; x < own->width(); ++x) {
                if (!(cov[std::size_t(y) * std::size_t(own->width()) + std::size_t(x)] > 0.0f)) continue;
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1);
                b.y1 = std::max(b.y1, y + 1);
            }
        }
        if (b.empty()) return;
        win = b;
        keep.resize(std::size_t(b.width()) * std::size_t(b.height()));
        for (int y = 0; y < b.height(); ++y)
            for (int x = 0; x < b.width(); ++x)
                keep[std::size_t(y) * std::size_t(b.width()) + std::size_t(x)] = 1.0f - cov[std::size_t(b.y0 + y) * std::size_t(own->width()) + std::size_t(b.x0 + x)];
    } else {
        win = own->shown();
        if (!win) return;
    }
    const image_ops::Held held(work_bytes(std::uint64_t(win->width()) * std::uint64_t(win->height()) * 2, 5));
    resample::Plane lifted = own->read(*win);
    if (area) {
        const int a = lifted.channels - 1;
        for (int y = 0; y < lifted.height; ++y)
            for (int x = 0; x < lifted.width; ++x) lifted.at(x, y)[a] *= 1.0f - keep[std::size_t(y) * std::size_t(lifted.width) + std::size_t(x)];
    }
    const Landed landed = through(lifted, win->x0, win->y0, sx, sy, own->width(), own->height(), m, interp);
    own->leave(*win, area ? &keep : nullptr);
    if (!landed.at.empty()) own->over(landed.plane, landed.at.x0, landed.at.y0);
    own->store(page, layer);
}

// --- patches ---------------------------------------------------------------------------------------------------------

std::array<double, 4> box_of_patch(const core::Patch& patch) {
    const Json* box = core::get(patch.attrs, "box");
    if (box == nullptr || !box->is_array() || box->size() != 4) throw OpError("a patch has no box");
    return {(*box)[0].get<double>(), (*box)[1].get<double>(), (*box)[2].get<double>(), (*box)[3].get<double>()};
}

void set_box(core::Patch& patch, const std::array<double, 4>& b) { patch.attrs["box"] = Json::array({b[0], b[1], b[2], b[3]}); }

// A patch through the map (none: nothing of it shows after).
std::optional<core::Patch> patch_through(const core::Patch& patch, const Map& m, resample::Interp interp) {
    if (!patch.png) return patch;
    const std::array<double, 4> box = box_of_patch(patch);
    if (m.is_affine()) {
        const pg::Mapping mapping = pg::mapping(m.affine);
        const auto& a = m.affine;
        // (moved and scaled along its axes: its box; turned by a right angle or flipped: its picture's pixels turned)
        if (std::fabs(a[1]) < 1e-12 && std::fabs(a[2]) < 1e-12 && a[0] > 0 && a[3] > 0) {
            core::Patch out = patch;
            set_box(out, pg::map_box(box, mapping));
            return out;
        }
        const bool axes = std::fabs(a[1]) < 1e-12 && std::fabs(a[2]) < 1e-12;
        const bool swapped = std::fabs(a[0]) < 1e-12 && std::fabs(a[3]) < 1e-12;
        if (axes || swapped) {
            std::optional<resample::Turn> turn;
            if (axes && a[0] < 0 && a[3] > 0) turn = resample::Turn::FlipH;
            if (axes && a[0] > 0 && a[3] < 0) turn = resample::Turn::FlipV;
            if (axes && a[0] < 0 && a[3] < 0) turn = resample::Turn::Rotate180;
            if (swapped && a[1] > 0 && a[2] < 0) turn = resample::Turn::Cw90;
            if (swapped && a[1] < 0 && a[2] > 0) turn = resample::Turn::Ccw90;
            if (turn) {
                bool grey = false;
                const resample::Plane turned = resample::turn(plane_of_picture(selection::open_picture(*patch.png), grey), *turn);
                core::Patch out = patch;
                out.png = fills::png_bytes(picture_of_plane(turned, grey));
                out.asset.reset();
                out.memo = {};
                set_box(out, pg::map_box(box, mapping));
                return out;
            }
        }
    }
    // (drawn again at the patches' resolution)
    const int dpi = fills::kFillDpi;
    const double s = dpi / 25.4;
    const selection::PatchPicture pic = selection::patch_px(patch, dpi);
    bool grey = false;
    const resample::Plane plane = plane_of_picture(pic.image, grey);
    const Landed landed = through(plane, static_cast<int>(pic.x0), static_cast<int>(pic.y0), s, s, 0, 0, m, interp);
    if (landed.at.empty() || landed.plane.width <= 0) return std::nullopt;
    auto out = selection::to_patch(picture_of_plane(landed.plane, grey), landed.at.x0, landed.at.y0, patch, dpi);
    if (out) {
        if (const Json* id = core::get(patch.attrs, "id"); id != nullptr) out->attrs["id"] = *id;  // (the same patch, moved)
    }
    return out;
}

// --- lines and shape parts ---------------------------------------------------------------------------------------------

warp::Go go_of(const Map& m) {
    return [&m](double x, double y) {
        const XY p = m.apply(x, y);
        return std::pair<double, double>{p.x, p.y};
    };
}

// --- masks -------------------------------------------------------------------------------------------------------------

void move_mask(const Page& page, core::Mask& mask, const Map& m) {
    if (!mask.png || mask.png->empty()) return;
    Image picture;
    try {
        picture = selection::open_picture(*mask.png).convert("L");
    } catch (const core::Error&) {
        return;  // (a mask that cannot be read is left as it is)
    }
    const double sx = picture.width() / page_w(page), sy = picture.height() / page_h(page);
    if (m.kind != Map::Kind::Mesh) {
        // (the whole mask through the map, its edge carried on past it: as a page's masks move)
        const resample::Homography h = grid_homography(m, sx, sy, 0, 0);
        const resample::Interp way = keeps_grid(h) ? resample::Interp::Nearest : resample::Interp::Bilinear;
        const resample::Plane out = resample::project(resample::from_gray(picture), h, picture.width(), picture.height(), way);
        mask.png = fills::png_bytes(resample::to_gray(out));
        mask.memo = {};
        return;
    }
    // (a bent map: what the mask is over the moved box drawn through it, laid over the mask as it was)
    const Window w = clamp_window(window_of(m.box[0] * sx, m.box[1] * sy, m.box[2] * sx, m.box[3] * sy, 2), picture.width(), picture.height());
    if (w.empty()) return;
    const std::string bytes = picture.tobytes();
    resample::Plane lifted(w.width(), w.height(), 2, true);
    for (int y = 0; y < w.height(); ++y) {
        for (int x = 0; x < w.width(); ++x) {
            float* v = lifted.at(x, y);
            v[0] = static_cast<unsigned char>(bytes[std::size_t(w.y0 + y) * std::size_t(picture.width()) + std::size_t(w.x0 + x)]) / 255.0f;
            v[1] = 1.0f;
        }
    }
    const Landed landed = through(lifted, w.x0, w.y0, sx, sy, picture.width(), picture.height(), m, resample::Interp::Bilinear);
    if (landed.at.empty()) return;
    std::string out = bytes;
    for (int y = 0; y < landed.plane.height; ++y) {
        for (int x = 0; x < landed.plane.width; ++x) {
            const float* v = landed.plane.at(x, y);
            if (!(v[1] > 0.0f)) continue;
            const std::size_t i = std::size_t(landed.at.y0 + y) * std::size_t(picture.width()) + std::size_t(landed.at.x0 + x);
            const double old = static_cast<unsigned char>(out[i]) / 255.0;
            const double now = v[0] * v[1] + old * (1.0 - v[1]);
            out[i] = static_cast<char>(std::lround(std::clamp(now, 0.0, 1.0) * 255.0));
        }
    }
    mask.png = fills::png_bytes(Image::frombytes("L", picture.size(), out));
    mask.memo = {};
}

// A layer's linked mask (masks@1 "linked") moved with the layer: the one entry for it. The whole mask through the map
// when no area is given (move_mask above); within an area, render/layer_masks' (PC3 B2) move_mask for an affine map and
// warp_mask for a bent one — the area's part of the mask lifted and laid through the map, its background left behind, as
// transform_area's "mask": "follow" moves it.
void move_linked_mask(const Page& page, Layer& layer, const Map& m, const std::optional<Json>& area) {
    if (!layer.mask || !layer.mask->linked) return;
    if (!area) {
        move_mask(page, *layer.mask, m);
        return;
    }
    // (the area's part of it, lifted by the area's coverage and laid through the map over what stays: B2's
    // render/layer_masks, as transform_area's "mask": "follow")
    if (m.is_affine()) {
        const std::array<double, 6> a{m.affine[0], m.affine[1], m.affine[2], m.affine[3], m.affine[4], m.affine[5]};
        *layer.mask = layer_masks::move_mask(page, *layer.mask, *area, a);
    } else {
        *layer.mask = layer_masks::warp_mask(page, *layer.mask, *area, [&m](double x, double y) {
            const XY q = m.apply(x, y);
            return std::pair<double, double>{q.x, q.y};
        });
    }
}

// --- the box -----------------------------------------------------------------------------------------------------------

Json area_of_box(const std::array<double, 4>& b) {
    return Json{{"poly", Json::array({Json::array({b[0], b[1]}), Json::array({b[2], b[1]}), Json::array({b[2], b[3]}), Json::array({b[0], b[3]})})}};
}

}  // namespace

// --- Map ---------------------------------------------------------------------------------------------------------------

XY Map::apply(double x, double y) const {
    switch (kind) {
        case Kind::Affine: return XY{affine[0] * x + affine[2] * y + affine[4], affine[1] * x + affine[3] * y + affine[5]};
        case Kind::Projective: {
            const auto [px, py] = projective.apply(x, y);
            return XY{px, py};
        }
        case Kind::Mesh: return forward ? forward(x, y) : XY{x, y};
    }
    return XY{x, y};
}

Map affine_map(const core::vector::Affine& m, const std::array<double, 4>& box) {
    Map out;
    out.kind = Map::Kind::Affine;
    out.affine = m;
    out.projective = resample::Homography::affine(m);
    out.box = box;
    return out;
}

std::optional<std::array<double, 4>> content_box(const Document& doc, const Page& page, const std::vector<std::string>& ids) {
    std::optional<Box> all;
    for (const std::string& id : ids) {
        const Layer& layer = page.layers[layer_index(page, id)];
        if (layer.kind == LayerKind::Folder || layer.kind == LayerKind::Adjust) continue;
        Layer shown = layer;
        shown.visible = true;
        shown.opacity = 1.0;
        const Image picture = layer_image(page, shown, kDpi, &doc, true);
        const auto b = picture.getbbox();
        if (!b) continue;
        all = all ? Box{std::min(all->x0, b->x0), std::min(all->y0, b->y0), std::max(all->x1, b->x1), std::max(all->y1, b->y1)} : *b;
    }
    if (!all) return std::nullopt;
    const double mm = 25.4 / kDpi;
    return std::array<double, 4>{all->x0 * mm, all->y0 * mm, all->x1 * mm, all->y1 * mm};
}

std::vector<std::string> layers_moved(const Page& page, const std::vector<std::string>& ids) {
    std::set<std::string> chosen;
    for (const std::string& id : ids) {
        (void)layer_index(page, id);
        chosen.insert(id);
    }
    // (the layers linked to any of them, and what is inside a folder, until nothing more joins)
    for (bool more = true; more;) {
        more = false;
        std::set<std::string> links;
        for (const Layer& layer : page.layers)
            if (chosen.contains(layer.id) && layer.link) links.insert(*layer.link);
        for (const Layer& layer : page.layers) {
            if (chosen.contains(layer.id)) continue;
            const bool linked = layer.link && links.contains(*layer.link);
            const bool inside = layer.parent_id.is_string() && chosen.contains(layer.parent_id.get<std::string>());
            if (linked || inside) {
                chosen.insert(layer.id);
                more = true;
            }
        }
    }
    std::vector<std::string> out;
    for (const Layer& layer : page.layers) {
        if (!chosen.contains(layer.id)) continue;
        if (layer.locked) throw OpError("the layer " + layer.id + " is locked");
        if (layer.lock_position) throw OpError("the position of the layer " + layer.id + " is locked");
        out.push_back(layer.id);
    }
    return out;
}

namespace {

// move_layer's work. `whole`: the op names no area — the layer moves whole (a warp lifting only what is in its box all
// the same) and its linked vector mask goes through the map with it.
void move_layer_in(Document& doc, std::size_t at, const std::string& id, const Map& m, const std::optional<Json>& area, bool whole,
                   resample::Interp interp, const std::vector<std::string>* parts) {
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[layer_index(page, id)];
    if (parts != nullptr) {  // (only some shape parts: the Path Selection tool's)
        std::vector<core::vector::ShapePart> items = layer.shapes->items;
        for (core::vector::ShapePart& part : items)
            if (std::find(parts->begin(), parts->end(), part.id) != parts->end()) part = part_through(part, m);
        layer.shapes = core::vector::make_shapes(std::move(items));
        return;
    }
    if (layer.kind == LayerKind::Smart) {
        // (PC3 A5: its placing, never its contents — a warp's own box is no selection: transform_layers gives a smart
        // object only the op's area)
        if (area) throw OpError("a smart object moves whole: transform it without a selection");
        const core::SmartContents* contents = core::smart::contents_of(doc, layer);
        if (contents == nullptr) throw OpError("the smart object has no contents");
        if (m.filter) {  // (a perspective warp, a puppet warp: its smart filter; the smart object, its masks stay)
            (void)smart_filters::push_filter(doc, page, layer, core::py_str((*m.filter)["kind"]), (*m.filter)["params"], std::nullopt);
            return;
        }
        // (its filters that name places still in the page's mm — a book of before — kept in its own first, so that they go
        // with it: independent review A P2-1)
        smart_filters::settle_places(doc, page, layer);
        if (m.warp) {  // (its one warp replaced, in its contents' mm)
            const auto kept = warp_in_contents(*m.warp, smart_placing(layer, *contents));
            core::smart::put_setting(layer, "warp", kept ? warp_to_json(*kept) : Json());
            core::smart::put_setting(layer, "psd", Json());
        } else if (m.kind == Map::Kind::Affine) {
            const auto& a = m.affine;
            core::smart::transform_layer(layer, core::smart::Matrix{a[0], a[1], a[2], a[3], a[4], a[5]});
            const core::smart::Matrix now = core::smart::snapped(core::smart::matrix_of(layer));
            if (std::abs(now.det()) <= core::smart::kDetMin) throw OpError("the transform squashes the smart object flat");
            core::smart::set_matrix(layer, now);
        } else if (m.kind == Map::Kind::Projective) {  // (its corners through the quad's homography)
            core::smart::Corners corners = core::smart::corners_on_page(layer, *contents);
            for (auto& [x, y] : corners) {
                const auto [px, py] = m.projective.apply(x, y);
                x = px;
                y = py;
            }
            try {
                core::smart::set_corners(layer, *contents, corners);
            } catch (const core::Error& error) {
                throw OpError(error.what());
            }
        } else {
            throw OpError("a smart object is bent by a warp, a perspective warp or a puppet warp");
        }
        move_filter_mask(doc, page, layer, m);
    } else if (layer.kind == LayerKind::Placed && layer.placement_mm) {
        const auto& a = m.affine;
        if (!m.is_affine() || std::fabs(a[1]) > 1e-12 || std::fabs(a[2]) > 1e-12 || a[0] <= 0 || a[3] <= 0) {
            throw OpError("a placed picture is turned and bent as a smart object: convert it to a smart object first");
        }
        if (area) throw OpError("a placed picture moves whole: transform it without a selection");
        const core::Rect& r = *layer.placement_mm;
        const auto b = pg::map_box({r.x.value(), r.y.value(), r.width.value(), r.height.value()}, pg::mapping(a));
        layer.placement_mm = core::Rect{core::Num(b[0]), core::Num(b[1]), core::Num(b[2]), core::Num(b[3])};
    }
    // its lines and shape parts
    std::optional<selection::AreaTest> inside;
    if (area) inside.emplace(*area);
    if (layer.stroke_count() > 0) {
        std::vector<core::StrokePtr> items = layer.strokes->items;
        for (core::StrokePtr& s : items) {
            if (inside && !inside->stroke_inside(*s)) continue;
            s = std::make_shared<const core::Stroke>(stroke_through(*s, m));
        }
        layer.strokes = core::make_strokes(std::move(items));
    }
    if (!layer.shapes->items.empty()) {
        std::vector<core::vector::ShapePart> items = layer.shapes->items;
        for (core::vector::ShapePart& part : items) {
            if (inside) {
                std::size_t in = 0, n = 0;
                for (const auto& sub : part.path.subpaths) {
                    for (const auto& k : sub.knots) {
                        ++n;
                        if (inside->contains(k.anchor.x, k.anchor.y)) ++in;
                    }
                }
                if (n == 0 || 2 * in <= n) continue;  // (most of its anchors in the area: it moves whole)
            }
            part = part_through(part, m);
        }
        layer.shapes = core::vector::make_shapes(std::move(items));
    }
    // its patches (lifted by the area: a patch it cuts split in two)
    if (!layer.patches.empty()) {
        std::vector<core::Patch> moving;
        if (area) {
            Layer only;
            only.kind = LayerKind::Strokes;
            only.patches = layer.patches;
            const selection::Items items = selection::lift(only, *area, page);
            layer.patches = only.patches;
            moving = items.patches;
        } else {
            moving = std::move(layer.patches);
            layer.patches.clear();
        }
        for (const core::Patch& patch : moving) {
            if (auto moved = patch_through(patch, m, interp)) layer.patches.push_back(std::move(*moved));
        }
    }
    // its own pixels
    move_pixels(page, layer, m, area, interp);
    // a fill's gradient, a tone's region and screen
    if (layer.fill && m.is_affine()) pg::map_fill(*layer.fill, pg::mapping(m.affine));
    if (layer.region && !layer.region->empty()) {
        for (core::Point& p : *layer.region) {
            const XY q = m.apply(p.x.value(), p.y.value());
            p = core::Point{core::Num(core::py_round(q.x, 3)), core::Num(core::py_round(q.y, 3))};
        }
    }
    if (layer.tone && layer.tone->is_object()) {
        if (const auto g = layer.tone->find("gradient"); g != layer.tone->end() && g->is_object()) {
            for (const char* key : {"from", "to"}) {
                const auto p = g->find(key);
                if (p == g->end() || !p->is_array() || p->size() < 2 || !(*p)[0].is_number() || !(*p)[1].is_number()) continue;
                const XY q = m.apply((*p)[0].get<double>(), (*p)[1].get<double>());
                *p = Json::array({core::py_round(q.x, 3), core::py_round(q.y, 3)});
            }
        }
    }
    if (layer.kind == LayerKind::Tone && m.is_affine()) {
        const pg::Mapping mapping = pg::mapping(m.affine);
        const double turned = mapping.reflects() ? mapping.angle_deg() - layer.angle : layer.angle + mapping.angle_deg();
        layer.angle = core::py_round(std::fmod(std::fmod(turned, 360.0) + 360.0, 360.0), 3);
    }
    // its mask and vector mask when they are linked (masks@1, vector@1)
    move_linked_mask(page, layer, m, area);
    if (whole && layer.vector_mask && layer.vector_mask->linked) {
        layer.vector_mask->path = m.is_affine() ? pg::map_path(layer.vector_mask->path, pg::mapping(m.affine)) : bend_path(layer.vector_mask->path, m);
    }
}

}  // namespace

void move_linked_masks(Page& page, Layer& layer, const Map& m) {
    move_linked_mask(page, layer, m, std::nullopt);
    if (layer.vector_mask && layer.vector_mask->linked) {
        layer.vector_mask->path = m.is_affine() ? pg::map_path(layer.vector_mask->path, pg::mapping(m.affine)) : bend_path(layer.vector_mask->path, m);
    }
}

void move_layer(Document& doc, std::size_t at, const std::string& id, const Map& m, const std::optional<Json>& area, resample::Interp interp,
                const std::vector<std::string>* parts) {
    move_layer_in(doc, at, id, m, area, !area, interp, parts);
}

void move_line(core::StoryLine& line, const Map& m) {
    if (m.is_affine()) {
        pg::map_line(line, pg::mapping(m.affine));
        return;
    }
    const double w = line.w_mm.value(), h = line.h_mm.value();
    const XY c = m.apply(line.x_mm.value() + w / 2, line.y_mm.value() + h / 2);
    line.x_mm = core::Num(core::py_round(c.x - w / 2, 3));
    line.y_mm = core::Num(core::py_round(c.y - h / 2, 3));
    const auto moved = [&m](const Json& p) {
        const XY q = m.apply(p[0].get<double>(), p[1].get<double>());
        return Json::array({core::py_round(q.x, 3), core::py_round(q.y, 3)});
    };
    if (line.tail) {
        const XY t = m.apply(line.tail->x.value(), line.tail->y.value());
        line.tail = core::Point{core::Num(core::py_round(t.x, 3)), core::Num(core::py_round(t.y, 3))};
    }
    for (Json& tail : line.tails) {
        if (!tail.is_object()) continue;
        for (const char* key : {"to", "via"}) {
            const auto p = tail.find(key);
            if (p != tail.end() && p->is_array() && p->size() >= 2) *p = moved(*p);
        }
        if (const auto vias = tail.find("vias"); vias != tail.end() && vias->is_array())
            for (Json& p : *vias)
                if (p.is_array() && p.size() >= 2) p = moved(p);
    }
    if (line.path) {
        for (core::Point& p : *line.path) {
            const XY q = m.apply(p.x.value(), p.y.value());
            p = core::Point{core::Num(core::py_round(q.x, 3)), core::Num(core::py_round(q.y, 3))};
        }
    }
}

namespace {

XY lerp(const XY& a, const XY& b, double t) { return XY{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

// How lengths scale near a point under the map (the square root of its Jacobian's determinant).
double local_scale(const Map& m, double x, double y) {
    const double h = 0.05;
    const XY a = m.apply(x - h, y), b = m.apply(x + h, y), c = m.apply(x, y - h), d = m.apply(x, y + h);
    const double j = ((b.x - a.x) * (d.y - c.y) - (b.y - a.y) * (d.x - c.x)) / (4 * h * h);
    return std::isfinite(j) && j != 0.0 ? std::sqrt(std::fabs(j)) : 1.0;
}

}  // namespace

core::Stroke stroke_through(const core::Stroke& s, const Map& m) {
    if (m.is_affine()) return pg::map_stroke(s, pg::mapping(m.affine));
    core::Stroke out = warp::warp_stroke(s, go_of(m));
    for (core::PointF& p : out.points) p = core::PointF{r4(p.x), r4(p.y)};
    return out;
}


// A path bent by a map: each segment cut (on the same curve) into pieces kStep long at most, their anchors and
// handles through the map.
core::vector::Path bend_path(const core::vector::Path& path, const Map& m) {
    core::vector::Path out;
    out.fill_rule = path.fill_rule;
    out.inverted = path.inverted;
    const auto f = [&m](const XY& p) {
        const XY q = m.apply(p.x, p.y);
        return XY{r4(q.x), r4(q.y)};
    };
    for (const core::vector::Subpath& sub : path.subpaths) {
        core::vector::Subpath bent;
        bent.closed = sub.closed;
        bent.op = sub.op;
        if (sub.knots.empty()) {
            out.subpaths.push_back(bent);
            continue;
        }
        std::vector<std::array<XY, 4>> pieces;
        for (const auto& seg : sub.segments()) {
            const double length = std::hypot(seg[1].x - seg[0].x, seg[1].y - seg[0].y) + std::hypot(seg[2].x - seg[1].x, seg[2].y - seg[1].y) +
                                  std::hypot(seg[3].x - seg[2].x, seg[3].y - seg[2].y);
            const int n = std::clamp(static_cast<int>(std::ceil(length / kStep)), 1, 4096);
            std::array<XY, 4> rest = seg;
            for (int k = 0; k < n; ++k) {
                if (k == n - 1) {
                    pieces.push_back(rest);
                    break;
                }
                const double t = 1.0 / static_cast<double>(n - k);
                const XY a = lerp(rest[0], rest[1], t), b = lerp(rest[1], rest[2], t), c = lerp(rest[2], rest[3], t);
                const XY d = lerp(a, b, t), e = lerp(b, c, t), g = lerp(d, e, t);
                pieces.push_back({rest[0], a, d, g});
                rest = {g, e, c, rest[3]};
            }
        }
        if (pieces.empty()) {  // (a single knot)
            const XY p = f(sub.knots[0].anchor);
            bent.knots.push_back(core::vector::Knot::corner(p.x, p.y));
            out.subpaths.push_back(bent);
            continue;
        }
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            core::vector::Knot k;
            k.anchor = f(pieces[i][0]);
            k.out = f(pieces[i][1]);
            const std::size_t before = i == 0 ? (sub.closed ? pieces.size() - 1 : pieces.size()) : i - 1;
            k.in = before < pieces.size() ? f(pieces[before][2]) : k.anchor;
            bent.knots.push_back(k);
        }
        if (!sub.closed) {
            core::vector::Knot last;
            last.anchor = f(pieces.back()[3]);
            last.in = f(pieces.back()[2]);
            last.out = last.anchor;
            bent.knots.push_back(last);
        }
        out.subpaths.push_back(std::move(bent));
    }
    return out;
}

core::vector::ShapePart part_through(const core::vector::ShapePart& part, const Map& m) {
    core::vector::ShapePart out = part;
    if (m.is_affine()) {
        pg::map_shape_part(out, pg::mapping(m.affine));
        return out;
    }
    const auto bounds = part.path.control_bounds();
    const double k = bounds ? local_scale(m, ((*bounds)[0] + (*bounds)[2]) / 2, ((*bounds)[1] + (*bounds)[3]) / 2) : 1.0;
    out.path = bend_path(part.path, m);
    if (out.stroke) out.stroke->width_mm = r4(std::min(out.stroke->width_mm * k, core::vector::kStrokeWidthMax));
    out.live.reset();  // (the path is the shape now)
    for (Json* fill : {&out.fill, out.stroke ? &out.stroke->fill : nullptr}) {
        if (fill == nullptr || !fill->is_object()) continue;
        const auto g = fill->find("gradient");
        if (g == fill->end() || !g->is_object()) continue;
        for (const char* key : {"from", "to"}) {
            const auto p = g->find(key);
            if (p == g->end() || !p->is_array() || p->size() < 2) continue;
            const XY q = m.apply((*p)[0].get<double>(), (*p)[1].get<double>());
            *p = Json::array({r4(q.x), r4(q.y)});
        }
    }
    return out;
}

// --- the warps' forms ------------------------------------------------------------------------------------------------

namespace {

double number_in(const Json& object, const char* key, double fallback, double lo, double hi, const std::string& what) {
    const Json* v = core::get(object, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_number()) throw OpError(what + "'s " + key + " must be a number");
    const double x = v->get<double>();
    if (!std::isfinite(x) || x < lo || x > hi) throw OpError(what + "'s " + key + " must be between " + core::py_repr(Json(lo)) + " and " + core::py_repr(Json(hi)));
    return x;
}

XY point_of(const Json& value, const std::string& what) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) throw OpError(what + " is [x, y] in mm");
    const XY p{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::fabs(p.x) > core::vector::kCoordinateMax || std::fabs(p.y) > core::vector::kCoordinateMax)
        throw OpError(what + " must be finite numbers");
    return p;
}

Json point_json(const XY& p) { return Json::array({r4(p.x), r4(p.y)}); }

std::array<XY, 4> quad_of(const Json& value, const std::string& what) {
    if (!value.is_array() || value.size() != 4) throw OpError(what + " is four corners [[x, y] × 4]");
    std::array<XY, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = point_of(value[i], what);
    return out;
}

}  // namespace

pg::Warp warp_from_json(const Json& value, const std::array<double, 4>& box) {
    if (!value.is_object()) throw OpError("warp is {style, bend, h_distort, v_distort, vertical, mesh?, grid?}");
    for (const auto& [key, v] : value.items()) {
        static const std::set<std::string> keys{"style", "bend", "h_distort", "v_distort", "vertical", "bounds", "mesh", "grid"};
        if (!keys.contains(key)) throw OpError("warp has no key " + key);
    }
    pg::Warp w;
    const Json* style = core::get(value, "style");
    if (style == nullptr || style->is_null()) {
        w.style = core::get(value, "mesh") != nullptr || core::get(value, "grid") != nullptr ? pg::WarpStyle::Custom : pg::WarpStyle::None;
    } else {
        const auto found = style->is_string() ? pg::warp_style_from(style->get<std::string>()) : std::nullopt;
        if (!found) throw OpError("the warp's style is one of none, custom, arc, arcLower, arcUpper, arch, bulge, shellLower, shellUpper, flag, wave, fish, rise, fisheye, inflate, squeeze, twist");
        w.style = *found;
    }
    w.bend = number_in(value, "bend", pg::is_preset(w.style) ? 50.0 : 0.0, -100, 100, "the warp");
    w.h_distort = number_in(value, "h_distort", 0.0, -100, 100, "the warp");
    w.v_distort = number_in(value, "v_distort", 0.0, -100, 100, "the warp");
    if (const Json* v = core::get(value, "vertical"); v != nullptr && !v->is_null()) {
        if (!v->is_boolean()) throw OpError("the warp's vertical is true or false");
        w.vertical = v->get<bool>();
    }
    w.box = box;
    if (const Json* b = core::get(value, "bounds"); b != nullptr && !b->is_null()) {
        if (!b->is_array() || b->size() != 4) throw OpError("the warp's bounds are [x0, y0, x1, y1] in mm");
        for (std::size_t i = 0; i < 4; ++i) {
            if (!(*b)[i].is_number() || !std::isfinite((*b)[i].get<double>())) throw OpError("the warp's bounds are [x0, y0, x1, y1] in mm");
            w.box[i] = (*b)[i].get<double>();
        }
    }
    if (!(w.box[2] > w.box[0] && w.box[3] > w.box[1])) throw OpError("the warp's box has no size");
    if (w.style == pg::WarpStyle::Custom) {
        if (const Json* mesh = core::get(value, "mesh"); mesh != nullptr && !mesh->is_null()) {
            if (!mesh->is_object()) throw OpError("the warp's mesh is {us, vs, points}");
            pg::BezierMesh bm;
            for (const auto& [key, list] : {std::pair{"us", &bm.us}, std::pair{"vs", &bm.vs}}) {
                const Json* k = core::get(*mesh, key);
                if (k == nullptr || !k->is_array()) throw OpError("the warp's mesh is {us, vs, points}");
                for (const Json& x : *k) {
                    if (!x.is_number()) throw OpError("the warp's mesh's " + std::string(key) + " are numbers from 0 to 1");
                    list->push_back(x.get<double>());
                }
            }
            if (bm.us.size() > 65 || bm.vs.size() > 65) throw OpError("a warp's mesh has at most 64 patches each way");
            const Json* points = core::get(*mesh, "points");
            if (points == nullptr || !points->is_array()) throw OpError("the warp's mesh is {us, vs, points}");
            for (const Json& p : *points) bm.points.push_back(point_of(p, "a point of the warp's mesh"));
            if (!bm.valid()) throw OpError("the warp's mesh needs (3·columns + 1) × (3·rows + 1) points and its us and vs from 0 to 1, rising");
            w.mesh = std::move(bm);
        } else {
            int cols = 1, rows = 1;
            if (const Json* grid = core::get(value, "grid"); grid != nullptr && !grid->is_null()) {
                if (!grid->is_array() || grid->size() != 2 || !(*grid)[0].is_number_integer() || !(*grid)[1].is_number_integer())
                    throw OpError("the warp's grid is [columns, rows]");
                cols = static_cast<int>((*grid)[0].get<std::int64_t>());
                rows = static_cast<int>((*grid)[1].get<std::int64_t>());
                if (cols < 1 || rows < 1 || cols > 64 || rows > 64) throw OpError("the warp's grid is 1 to 64 each way");
            }
            w.mesh = pg::BezierMesh::identity(w.box, cols, rows);
        }
    }
    return w;
}

Json warp_to_json(const pg::Warp& w) {
    Json out{{"style", std::string(pg::to_string(w.style))},
             {"bend", r4(w.bend)},
             {"h_distort", r4(w.h_distort)},
             {"v_distort", r4(w.v_distort)},
             {"vertical", w.vertical},
             {"bounds", Json::array({r4(w.box[0]), r4(w.box[1]), r4(w.box[2]), r4(w.box[3])})}};
    if (w.style == pg::WarpStyle::Custom && w.mesh) {
        Json us = Json::array(), vs = Json::array(), points = Json::array();
        for (const double u : w.mesh->us) us.push_back(core::py_round(u, 6));
        for (const double v : w.mesh->vs) vs.push_back(core::py_round(v, 6));
        for (const XY& p : w.mesh->points) points.push_back(point_json(p));
        out["mesh"] = Json{{"us", us}, {"vs", vs}, {"points", points}};
    }
    return out;
}

std::optional<pg::Warp> split_warp(const pg::Warp& w, const std::optional<XY>& at, Split how) {
    pg::BezierMesh mesh = w.to_mesh(1, 1);
    auto [s, t] = at ? mesh.param_at(*at) : std::pair<double, double>{0.5, 0.5};
    if (!at) {  // (the middle of the patch holding the point)
        const auto mid = [](const std::vector<double>& k, double v) {
            std::size_t i = 0;
            while (i + 2 < k.size() && v > k[i + 1]) ++i;
            return (k[i] + k[i + 1]) / 2.0;
        };
        s = mid(mesh.us, s);
        t = mid(mesh.vs, t);
    }
    bool ok = false;
    switch (how) {
        case Split::Crosswise: {
            const bool a = mesh.split_u(s);
            const bool b = mesh.split_v(t);
            ok = a || b;
            break;
        }
        // (a horizontal split line divides the rows; a vertical one the columns)
        case Split::Horizontal: ok = mesh.split_v(t); break;
        case Split::Vertical: ok = mesh.split_u(s); break;
        case Split::Remove: {
            const auto nearest = [](const std::vector<double>& k, double v) -> std::optional<std::size_t> {
                std::optional<std::size_t> best;
                for (std::size_t i = 1; i + 1 < k.size(); ++i)
                    if (!best || std::fabs(k[i] - v) < std::fabs(k[*best] - v)) best = i;
                return best;
            };
            const auto iu = nearest(mesh.us, s), iv = nearest(mesh.vs, t);
            const double du = iu ? std::fabs(mesh.us[*iu] - s) : std::numeric_limits<double>::max();
            const double dv = iv ? std::fabs(mesh.vs[*iv] - t) : std::numeric_limits<double>::max();
            if (iu && du <= dv) {
                ok = mesh.remove_split_u(*iu);
            } else if (iv) {
                ok = mesh.remove_split_v(*iv);
            }
            break;
        }
    }
    if (!ok) return std::nullopt;
    pg::Warp out;
    out.style = pg::WarpStyle::Custom;
    out.box = w.box;
    out.mesh = std::move(mesh);
    return out;
}

PerspectiveSpec perspective_from_json(const Json& value) {
    if (!value.is_object()) throw OpError("perspective is {planes: [{src, dst}…], straighten?}");
    PerspectiveSpec out;
    const Json* planes = core::get(value, "planes");
    if (planes == nullptr || !planes->is_array() || planes->empty()) throw OpError("perspective's planes are [{src: [[x, y] × 4], dst: [[x, y] × 4]}…]");
    if (planes->size() > 64) throw OpError("a perspective warp has at most 64 planes");
    for (const Json& p : *planes) {
        if (!p.is_object() || core::get(p, "src") == nullptr || core::get(p, "dst") == nullptr)
            throw OpError("perspective's planes are [{src: [[x, y] × 4], dst: [[x, y] × 4]}…]");
        out.src.push_back(quad_of(p["src"], "a plane's src"));
        out.dst.push_back(quad_of(p["dst"], "a plane's dst"));
    }
    if (const Json* s = core::get(value, "straighten"); s != nullptr && !s->is_null()) {
        if (!s->is_string() || !perspective::straighten_from(s->get<std::string>())) throw OpError("straighten is horizontal, vertical or auto");
        out.straighten = s->get<std::string>();
    }
    return out;
}

Json perspective_to_json(const PerspectiveSpec& spec) {
    Json planes = Json::array();
    for (std::size_t i = 0; i < spec.src.size(); ++i) {
        Json src = Json::array(), dst = Json::array();
        for (const XY& p : spec.src[i]) src.push_back(point_json(p));
        for (const XY& p : spec.dst[i]) dst.push_back(point_json(p));
        planes.push_back(Json{{"src", src}, {"dst", dst}});
    }
    Json out{{"planes", planes}};
    if (spec.straighten) out["straighten"] = *spec.straighten;
    return out;
}

puppet::Warp puppet_from_json(const Json& value) {
    if (!value.is_object()) throw OpError("puppet is {pins: [{src, dst, rotate?, depth?}…], mode?, density?, expansion_mm?}");
    puppet::Warp w;
    const Json* pins = core::get(value, "pins");
    if (pins != nullptr && !pins->is_null()) {
        if (!pins->is_array()) throw OpError("puppet's pins are [{src: [x, y], dst: [x, y], rotate?, depth?}…]");
        if (pins->size() > 1000) throw OpError("a puppet warp has at most 1000 pins");
        for (const Json& p : *pins) {
            if (!p.is_object() || core::get(p, "src") == nullptr || core::get(p, "dst") == nullptr)
                throw OpError("puppet's pins are [{src: [x, y], dst: [x, y], rotate?, depth?}…]");
            puppet::Pin pin;
            const XY s = point_of(p["src"], "a pin's src"), d = point_of(p["dst"], "a pin's dst");
            pin.src = {s.x, s.y};
            pin.dst = {d.x, d.y};
            if (const Json* r = core::get(p, "rotate"); r != nullptr && !r->is_null()) {
                if (!r->is_number() || !std::isfinite(r->get<double>())) throw OpError("a pin's rotate is degrees");
                pin.rotate = r->get<double>();
            }
            if (const Json* d2 = core::get(p, "depth"); d2 != nullptr && !d2->is_null()) {
                if (!d2->is_number_integer() || std::abs(d2->get<std::int64_t>()) > 1000) throw OpError("a pin's depth is a whole number");
                pin.depth = static_cast<int>(d2->get<std::int64_t>());
            }
            w.pins.push_back(pin);
        }
    }
    if (const Json* m = core::get(value, "mode"); m != nullptr && !m->is_null()) {
        const auto mode = m->is_string() ? puppet::mode_from(m->get<std::string>()) : std::nullopt;
        if (!mode) throw OpError("puppet's mode is rigid, normal or distort");
        w.mode = *mode;
    }
    if (const Json* d = core::get(value, "density"); d != nullptr && !d->is_null()) {
        const auto density = d->is_string() ? puppet::density_from(d->get<std::string>()) : std::nullopt;
        if (!density) throw OpError("puppet's density is fewer, normal or more");
        w.density = *density;
    }
    w.expansion = number_in(value, "expansion_mm", 2.0 / kPx, -200.0 / kPx, 200.0 / kPx, "puppet");
    return w;
}

Json puppet_to_json(const puppet::Warp& w) {
    Json pins = Json::array();
    for (const puppet::Pin& p : w.pins) {
        Json pin{{"src", point_json(XY{p.src[0], p.src[1]})}, {"dst", point_json(XY{p.dst[0], p.dst[1]})}};
        if (p.rotate) pin["rotate"] = r4(*p.rotate);
        if (p.depth != 0) pin["depth"] = p.depth;
        pins.push_back(pin);
    }
    return Json{{"pins", pins}, {"mode", std::string(puppet::to_string(w.mode))}, {"density", std::string(puppet::to_string(w.density))},
                {"expansion_mm", r4(w.expansion)}};
}

// --- smart objects (PC3 A5) --------------------------------------------------------------------------------------------

resample::Homography smart_placing(const Layer& layer, const core::SmartContents& contents) {
    if (const Json* quad = layer.smart ? core::get(*layer.smart, "perspective") : nullptr; quad != nullptr && !quad->is_null()) {
        const core::smart::Corners c = core::smart::corners_on_page(layer, contents);
        const auto [w, h] = core::smart::size_mm(contents);
        if (const auto made = resample::Homography::rect_to_quad(
                {0, 0, w, h}, {{{c[0].first, c[0].second}, {c[1].first, c[1].second}, {c[2].first, c[2].second}, {c[3].first, c[3].second}}}))
            return *made;
    }
    const core::smart::Matrix m = core::smart::matrix_of(layer);
    return resample::Homography::affine({m.a, m.b, m.c, m.d, m.e, m.f});
}

std::optional<pg::Warp> warp_in_contents(const pg::Warp& on_page, const resample::Homography& place) {
    const auto back = place.inverse();
    if (!back || on_page.is_identity()) return std::nullopt;
    // (the box its bounds' corners come back to)
    const auto& b = on_page.box;
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const auto& [x, y] : {std::pair{b[0], b[1]}, std::pair{b[2], b[1]}, std::pair{b[2], b[3]}, std::pair{b[0], b[3]}}) {
        const auto [u, v] = back->apply(x, y);
        x0 = std::min(x0, u);
        y0 = std::min(y0, v);
        x1 = std::max(x1, u);
        y1 = std::max(y1, v);
    }
    if (!(x1 > x0 && y1 > y0) || !std::isfinite(x0) || !std::isfinite(y1)) return std::nullopt;
    pg::Warp out = on_page;
    out.box = {x0, y0, x1, y1};
    if (on_page.style == pg::WarpStyle::Custom) {
        pg::BezierMesh mesh = on_page.to_mesh(1, 1);
        for (XY& p : mesh.points) {
            const auto [u, v] = back->apply(p.x, p.y);
            p = XY{u, v};
        }
        out.mesh = std::move(mesh);
    }
    if (out.is_identity()) return std::nullopt;
    return out;
}

std::optional<pg::Warp> smart_warp_on_page(const Document& doc, const Layer& layer) {
    const Json* stored = layer.smart ? core::get(*layer.smart, "warp") : nullptr;
    const core::SmartContents* contents = core::smart::contents_of(doc, layer);
    if (stored == nullptr || stored->is_null() || contents == nullptr) return std::nullopt;
    pg::Warp w;
    try {
        w = warp_from_json(*stored, {0, 0, 1, 1});
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (w.is_identity()) return std::nullopt;
    const resample::Homography place = smart_placing(layer, *contents);
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const auto& [x, y] : {std::pair{w.box[0], w.box[1]}, std::pair{w.box[2], w.box[1]}, std::pair{w.box[2], w.box[3]}, std::pair{w.box[0], w.box[3]}}) {
        const auto [u, v] = place.apply(x, y);
        x0 = std::min(x0, u);
        y0 = std::min(y0, v);
        x1 = std::max(x1, u);
        y1 = std::max(y1, v);
    }
    const auto& h = place.m;
    const bool even = place.is_affine() && std::fabs(h[1]) < 1e-12 && std::fabs(h[3]) < 1e-12 && h[0] > 0 && std::fabs(h[0] - h[4]) < 1e-12;
    pg::Warp out;
    out.box = {x0, y0, x1, y1};
    if (pg::is_preset(w.style) && even) {  // (moved and scaled evenly: the same style over the box it lands on)
        out = w;
        out.box = {x0, y0, x1, y1};
        return out;
    }
    pg::BezierMesh mesh = w.to_mesh(1, 1);
    for (XY& p : mesh.points) {
        const auto [u, v] = place.apply(p.x, p.y);
        p = XY{u, v};
    }
    out.style = pg::WarpStyle::Custom;
    out.mesh = std::move(mesh);
    return out;
}

void move_filter_mask(Document& doc, const Page& page, Layer& layer, const Map& m) {
    if (!layer.smart || !layer.smart->is_object()) return;
    const auto at = layer.smart->find("filter_mask");
    if (at == layer.smart->end() || !at->is_object() || !at->contains("asset") || !(*at)["asset"].is_string()) return;
    if (m.is_affine() && m.affine == core::vector::Affine{1, 0, 0, 1, 0, 0}) return;
    core::Mask mask;
    mask.png = smart_filters::mask_bytes(doc, (*at)["asset"].get<std::string>());
    if (!mask.png) return;  // (not in the book: the reader says so where it is read)
    move_mask(page, mask, m);
    if (!mask.png) return;
    const std::string ref = core::bytes_ref(*mask.png);
    doc.smart_files[ref] = mask.png;
    (*at)["asset"] = ref;
}

// --- the op ----------------------------------------------------------------------------------------------------------

namespace {

constexpr std::array<const char*, 7> kWays{"move", "matrix", "quad", "turn", "warp", "perspective", "puppet"};

std::vector<std::string> ids_of(const Json& op) {
    const Json* ids = core::get(op, "ids");
    if (ids == nullptr || ids->is_null()) {
        if (const Json* id = core::get(op, "id"); id != nullptr && id->is_string()) return {id->get<std::string>()};
        throw OpError("ids is [layer id, …]");
    }
    if (!ids->is_array() || ids->empty()) throw OpError("ids is [layer id, …]");
    std::vector<std::string> out;
    for (const Json& id : *ids) {
        if (!id.is_string()) throw OpError("ids is [layer id, …]");
        out.push_back(id.get<std::string>());
    }
    return out;
}

core::vector::Affine affine_of(const Json& value) {
    if (!value.is_array() || value.size() != 6) throw OpError("matrix is [a, b, c, d, e, f]");
    core::vector::Affine m{};
    for (std::size_t i = 0; i < 6; ++i) {
        if (!value[i].is_number() || !std::isfinite(value[i].get<double>())) throw OpError("matrix is [a, b, c, d, e, f] of finite numbers");
        m[i] = value[i].get<double>();
    }
    if (std::fabs(m[0] * m[3] - m[1] * m[2]) < 1e-12) throw OpError("the transform squashes the layers flat");
    return m;
}

// The coverage of the layers (their alphas drawn at the working 200 dpi) over the box, for a puppet's mesh.
puppet::Coverage coverage_of(const Document& doc, const Page& page, const std::vector<std::string>& ids, const std::array<double, 4>& box) {
    puppet::Coverage cov;
    const Window w = clamp_window(window_of(box[0] * kPx, box[1] * kPx, box[2] * kPx, box[3] * kPx, 0), static_cast<int>(std::ceil(page_w(page) * kPx)),
                                  static_cast<int>(std::ceil(page_h(page) * kPx)));
    if (w.empty()) return cov;
    cov.width = w.width();
    cov.height = w.height();
    cov.x0 = w.x0;
    cov.y0 = w.y0;
    cov.alpha.assign(std::size_t(cov.width) * std::size_t(cov.height), 0.0f);
    for (const std::string& id : ids) {
        const Layer& layer = page.layers[layer_index(page, id)];
        if (layer.kind == LayerKind::Folder || layer.kind == LayerKind::Adjust) continue;
        Layer shown = layer;
        shown.visible = true;
        shown.opacity = 1.0;
        const Image picture = layer_image(page, shown, kDpi, &doc, true);
        if (picture.mode() != "RGBA") continue;
        const std::string bytes = picture.tobytes();
        for (int y = 0; y < cov.height; ++y) {
            const int py = cov.y0 + y;
            if (py >= picture.height()) break;
            for (int x = 0; x < cov.width; ++x) {
                const int px = cov.x0 + x;
                if (px >= picture.width()) break;
                const float a = static_cast<unsigned char>(bytes[(std::size_t(py) * std::size_t(picture.width()) + std::size_t(px)) * 4 + 3]) / 255.0f;
                float& c = cov.alpha[std::size_t(y) * std::size_t(cov.width) + std::size_t(x)];
                c = std::max(c, a);
            }
        }
    }
    return cov;
}

// A right-angle turn or flip about the box's middle, its middle moved by half a working pixel when the turn would
// otherwise leave the 200 dpi grid between its pixels (then the pixels move exactly).
core::vector::Affine turn_about(pg::Turn turn, const std::array<double, 4>& b) {
    const core::vector::Affine t = pg::turn_affine(turn, 1.0, 1.0);
    const double a = t[0], bb = t[1], c = t[2], d = t[3];
    double cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2;
    core::vector::Affine out{a, bb, c, d, cx - (a * cx + c * cy), cy - (bb * cx + d * cy)};
    const double ex = out[4] * kPx, ey = out[5] * kPx;
    const auto half = [](double v) { return std::fabs(std::fabs(v - std::floor(v)) - 0.5) < 1e-6; };
    if (half(ex) && half(ey)) {
        cx += 0.5 / kPx;
        out[4] = cx - (a * cx + c * cy);
        out[5] = cy - (bb * cx + d * cy);
    }
    return out;
}

}  // namespace

void transform_layers(core::OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::vector<std::string> asked = ids_of(op);
    std::vector<std::string> ids = layers_moved(doc.page(at), asked);
    std::vector<const char*> given;
    for (const char* way : kWays)
        if (const Json* v = core::get(op, way); v != nullptr && !v->is_null()) given.push_back(way);
    if (given.size() != 1) throw OpError("give one of move, matrix, quad, turn, warp, perspective or puppet");
    const std::string way = given.front();
    resample::Interp interp = resample::Interp::Bicubic;
    if (const Json* i = core::get(op, "interp"); i != nullptr && !i->is_null()) {
        const auto found = i->is_string() ? resample::interp_from(i->get<std::string>()) : std::nullopt;
        if (!found) throw OpError("interp must be nearest, bilinear or bicubic");
        interp = *found;
    }
    std::optional<Json> area;
    if (const Json* a = core::get(op, "area"); a != nullptr && !a->is_null()) area = core::op_area(op);
    // (shape parts only: the Path Selection tool's)
    std::map<std::string, std::vector<std::string>> parts;
    if (const Json* p = core::get(op, "parts"); p != nullptr && !p->is_null()) {
        if (!p->is_object()) throw OpError("parts is {layer id: [part id, …]}");
        for (const auto& [id, list] : p->items()) {
            if (!list.is_array()) throw OpError("parts is {layer id: [part id, …]}");
            const Layer& layer = doc.page(at).layers[layer_index(doc.page(at), id)];
            std::vector<std::string> named;
            for (const Json& part : list) {
                if (!part.is_string()) throw OpError("parts is {layer id: [part id, …]}");
                const auto& items = layer.shapes->items;
                if (std::none_of(items.begin(), items.end(), [&](const core::vector::ShapePart& s) { return s.id == part.get<std::string>(); }))
                    throw core::OpKeyError(core::py_repr_str(part.get<std::string>()));
                named.push_back(part.get<std::string>());
            }
            parts[id] = std::move(named);
        }
    }
    // the box: given, or what the layers show (within the area)
    std::array<double, 4> box{};
    if (const Json* b = core::get(op, "box"); b != nullptr && !b->is_null()) {
        if (!b->is_array() || b->size() != 4) throw OpError("box is [x, y, w, h] in mm");
        for (std::size_t i = 0; i < 4; ++i)
            if (!(*b)[i].is_number() || !std::isfinite((*b)[i].get<double>())) throw OpError("box is [x, y, w, h] in mm");
        box = {(*b)[0].get<double>(), (*b)[1].get<double>(), (*b)[0].get<double>() + (*b)[2].get<double>(), (*b)[1].get<double>() + (*b)[3].get<double>()};
        if (!(box[2] > box[0] && box[3] > box[1])) throw OpError("the box has no size");
    } else {
        std::vector<std::string> shown = ids;
        auto found = content_box(doc, doc.page(at), shown);
        if (area) {
            const std::vector<double> ab = selection::area_bbox(*area);
            if (ab.size() == 4 && found) {
                found = std::array<double, 4>{std::max((*found)[0], ab[0]), std::max((*found)[1], ab[1]), std::min((*found)[2], ab[0] + ab[2]),
                                              std::min((*found)[3], ab[1] + ab[3])};
                if (!((*found)[2] > (*found)[0] && (*found)[3] > (*found)[1])) found.reset();
            }
        }
        if (!found) throw OpError("the layers show nothing to transform");
        box = *found;
    }
    Map m;
    m.box = box;
    Json report{{"moved", Json::array()}, {"box", Json::array({r4(box[0]), r4(box[1]), r4(box[2] - box[0]), r4(box[3] - box[1])})}};
    const Json& spec = op[way];
    std::optional<Json> lift = area;
    if (way == "move") {
        const XY d = point_of(spec, "move");
        m = affine_map({1, 0, 0, 1, d.x, d.y}, box);
    } else if (way == "matrix") {
        m = affine_map(affine_of(spec), box);
    } else if (way == "turn") {
        const auto turn = spec.is_string() ? pg::turn_from(spec.get<std::string>()) : std::nullopt;
        if (!turn) throw OpError("turn is 90cw, 90ccw, 180, flip_h or flip_v");
        m = affine_map(turn_about(*turn, box), box);
    } else if (way == "quad") {
        const std::array<XY, 4> q = quad_of(spec, "quad");
        const auto h = resample::Homography::rect_to_quad(box, {{{q[0].x, q[0].y}, {q[1].x, q[1].y}, {q[2].x, q[2].y}, {q[3].x, q[3].y}}});
        if (!h || !h->inverse()) throw OpError("the quad's corners must make a quad");
        const auto& hm = h->m;
        if (std::fabs(hm[6]) < 1e-12 && std::fabs(hm[7]) < 1e-12) {  // (a parallelogram: affine, exactly)
            m = affine_map({hm[0] / hm[8], hm[3] / hm[8], hm[1] / hm[8], hm[4] / hm[8], hm[2] / hm[8], hm[5] / hm[8]}, box);
        } else {
            m.kind = Map::Kind::Projective;
            m.projective = *h;
        }
    } else if (way == "warp") {
        const pg::Warp w = warp_from_json(spec, box);
        report["warp"] = warp_to_json(w);
        auto shared = std::make_shared<const pg::Warp>(w);
        if (w.is_identity()) {  // (a warp that bends nothing: everything as it was, exactly)
            m = affine_map({1, 0, 0, 1, 0, 0}, w.box);
        } else {
            m.box = w.box;
            m.kind = Map::Kind::Mesh;
            m.forward = [shared](double x, double y) { return shared->map(x, y); };
            if (!lift) lift = area_of_box(w.box);  // (what is in the warp's box moves; the rest stays)
        }
        m.warp = shared;  // (PC3 A5: a smart object's warp, even one that bends nothing — it takes the warp it holds away)
    } else if (way == "perspective") {
        PerspectiveSpec ps = perspective_from_json(spec);
        std::vector<perspective::Plane> planes;
        for (std::size_t i = 0; i < ps.src.size(); ++i) {
            perspective::Plane p;
            for (std::size_t k = 0; k < 4; ++k) {
                p.src[k] = {ps.src[i][k].x, ps.src[i][k].y};
                p.dst[k] = {ps.dst[i][k].x, ps.dst[i][k].y};
            }
            planes.push_back(p);
        }
        const double link = 0.5 / kPx;  // (corners within half a working pixel are one)
        if (ps.straighten) {
            perspective::straighten(planes, *perspective::straighten_from(*ps.straighten), link);
            for (std::size_t i = 0; i < planes.size(); ++i)
                for (std::size_t k = 0; k < 4; ++k) ps.dst[i][k] = XY{planes[i].dst[k][0], planes[i].dst[k][1]};
        }
        auto map = perspective::Map::make(planes, link);
        if (!map) throw OpError("a plane is degenerate: its corners must make a quad");
        report["perspective"] = perspective_to_json(ps);
        if (!perspective::is_identity(planes)) {  // (planes left where they were: everything as it was, exactly)
            auto shared = std::make_shared<const perspective::Map>(std::move(*map));
            m.kind = Map::Kind::Mesh;
            m.forward = [shared](double x, double y) {
                const auto p = shared->map(x, y);
                return XY{p[0], p[1]};
            };
            // (PC3 A5: a smart object's smart filter — its planes straightened already, and the interpolation)
            PerspectiveSpec kept = ps;
            kept.straighten.reset();
            Json params = perspective_to_json(kept);
            params["interp"] = std::string(interp == resample::Interp::Nearest ? "nearest" : interp == resample::Interp::Bilinear ? "bilinear" : "bicubic");
            m.filter = Json{{"kind", "perspective_warp"}, {"params", std::move(params)}};
        }
    } else {  // puppet
        puppet::Warp w = puppet_from_json(spec);
        report["puppet"] = puppet_to_json(w);
        if (!w.is_identity()) {
            // (PC3 A5: a smart object's puppet warp is its smart filter, meshed over its own picture: alone)
            const bool smart = std::any_of(ids.begin(), ids.end(), [&](const std::string& id) {
                return doc.page(at).layers[layer_index(doc.page(at), id)].kind == LayerKind::Smart;
            });
            const bool others = std::any_of(ids.begin(), ids.end(), [&](const std::string& id) {
                const Layer& l = doc.page(at).layers[layer_index(doc.page(at), id)];
                return l.kind != LayerKind::Smart && l.kind != LayerKind::Folder;
            });
            if (smart && (others || ids.size() > 1)) throw OpError("a smart object is puppet-warped alone (its puppet warp is its smart filter)");
            Json params = puppet_to_json(w);
            params["interp"] = std::string(interp == resample::Interp::Nearest ? "nearest" : interp == resample::Interp::Bilinear ? "bilinear" : "bicubic");
            m.filter = Json{{"kind", "puppet_warp"}, {"params", std::move(params)}};
        }
        if (!w.is_identity()) {  // (no pin moved: everything as it was, exactly)
            puppet::Warp px = w;  // (solved in the working 200 dpi pixels)
            px.expansion = w.expansion * kPx;
            for (puppet::Pin& p : px.pins) {
                p.src = {p.src[0] * kPx, p.src[1] * kPx};
                p.dst = {p.dst[0] * kPx, p.dst[1] * kPx};
            }
            const double grow = std::max(px.expansion, 0.0) / kPx + 1.0;
            const std::array<double, 4> reach{box[0] - grow, box[1] - grow, box[2] + grow, box[3] + grow};
            auto deformed = std::make_shared<const puppet::Deformed>(puppet::deform(coverage_of(doc, doc.page(at), ids, reach), px));
            if (deformed->mesh.empty()) throw OpError("the layers show nothing to pin");
            m.kind = Map::Kind::Mesh;
            m.puppet = deformed;
            m.forward = [deformed](double x, double y) {
                const auto p = deformed->map(x * kPx, y * kPx);
                return XY{p[0] / kPx, p[1] / kPx};
            };
        }
    }
    if (m.kind == Map::Kind::Projective) {
        if (!m.projective.inverse()) throw OpError("the quad's corners must make a quad");
    }
    if (m.kind == Map::Kind::Affine) {  // (the map as it was made: a turn's snapped middle, a parallelogram's matrix)
        Json made = Json::array();
        for (const double v : m.affine) made.push_back(core::py_round(v, 6));
        report["matrix"] = std::move(made);
    }
    for (const std::string& id : ids) {
        const auto named = parts.find(id);
        if (!parts.empty() && named == parts.end()) continue;  // (parts given: only their layers)
        // (a smart object is given the op's own area only: a warp's box is no selection of it)
        const bool smart = doc.page(at).layers[layer_index(doc.page(at), id)].kind == LayerKind::Smart;
        // (a warp's box lifts only what is in it, yet without the op's own area the layer moves whole: its vector mask bent)
        move_layer_in(doc, at, id, m, smart ? area : lift, !area, interp, named != parts.end() ? &named->second : nullptr);
        report["moved"].push_back(id);
    }
    if (const Json* with = core::get(op, "with"); with != nullptr && !with->is_null()) {
        if (!with->is_object()) throw OpError("with is {lines: [line id, …]}");
        if (const Json* lines = core::get(*with, "lines"); lines != nullptr && !lines->is_null()) {
            if (!lines->is_array()) throw OpError("with is {lines: [line id, …]}");
            for (const Json& id : *lines) {
                if (!id.is_string()) throw OpError("with is {lines: [line id, …]}");
                auto line = std::find_if(doc.story.begin(), doc.story.end(), [&](const core::StoryLine& l) { return l.id == id.get<std::string>(); });
                if (line == doc.story.end() || !core::py_equals(line->page_index.json(), doc.page(at).index.json()))
                    throw core::OpKeyError(core::py_repr_str(id.get<std::string>()));
                move_line(*line, m);
            }
        }
    }
    c.report = std::move(report);
}

}  // namespace genko::render::layer_transform
