// PC3 節 B (SPEC.md COLOR-06「修復」): the retouching ops (B4) — clone_stamp (コピースタンプツール), healing_brush (修復ブラシ
// ツール), spot_healing (スポット修復ブラシツール), history_brush (ヒストリーブラシツール) and content_aware_fill (編集 →
// コンテンツに応じた塗りつぶし…). PhotoCraft v0.2.0's engine/src/retouch_cmds.rs (lines 1-434: the brushes' parameters,
// sampling, the clone's mapping, heal_region, spot healing) and engine/src/edit_menu_cmds.rs (lines 288-427: Content-Aware
// Fill), MIT (native/third_party/photocraft/NOTICE.md), with Genko's mm, layers and areas: a stroke's points and sizes
// are in mm and land on the layer's own grid; the pixels are the layer's own values of any kind (image_ops::Pixels: 8
// bits, precise 16 and 32 bits, the inks of a CMYK, Lab, grey or multichannel page), read and written back only around
// the work (the window's read and write: what is outside keeps its bytes); the selection is the op's `area` (each pixel
// weighted by its coverage); a layer whose transparency is protected (lock_alpha) keeps its alpha. Dabs and coverage are
// render/retouch_dabs, the Poisson blend render/poisson, the fills render/inpaint and render/content_aware.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/pyops.hpp"
#include "core/stroke_tools.hpp"
#include "render/content_aware.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/inpaint.hpp"
#include "render/ops_pc3.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/pixels_before.hpp"
#include "render/png.hpp"
#include "render/poisson.hpp"
#include "render/raster.hpp"
#include "render/brushwork.hpp"
#include "render/retouch_dabs.hpp"
#include "render/selection.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using retouch::Coverage;

using namespace brushwork;  // (B4's brushes' arguments, grid, windows and selection: render/brushwork, shared with PC4 E5)

// --- the values ----------------------------------------------------------------------------------------------------------

// The values interleaved (each pixel's colours and alpha), and back.
std::vector<float> values_of(const Pixels& p) {
    std::vector<float> out(p.size() * p.all());
    for (std::size_t i = 0; i < p.size(); ++i) std::copy(p.at(i), p.at(i) + p.all(), out.begin() + static_cast<std::ptrdiff_t>(i * p.all()));
    return out;
}

void put_values(Pixels& p, const std::vector<float>& v) {
    for (std::size_t i = 0; i < p.size(); ++i) std::copy_n(v.begin() + static_cast<std::ptrdiff_t>(i * p.all()), p.all(), p.at(i));
}

// Results kept in the range the layer stores: the alpha 0..1; the colours too, but a float layer's (a precise f32 one,
// an ink page's 32-bit grey) only above 0 (its HDR kept).
void clamp_values(const Pixels& like, std::vector<float>& v) {
    const unsigned n = like.colours(), all = like.all();
    const bool hdr = (like.kind() == Pixels::Kind::Precise && like.floating) ||
                     (like.kind() == Pixels::Kind::Inks && like.ink && like.ink->depth == 32 && like.ink->space == core::InkSpace::Gray);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const bool alpha = i % all == n;
        v[i] = std::isfinite(v[i]) ? (alpha || !hdr ? std::clamp(v[i], 0.0f, 1.0f) : std::max(v[i], 0.0f)) : 0.0f;
    }
}

// Seamless-clone `src` into `dst` (both over `box`) where `mask` is set: the solve on the mask's box and a pixel more (a
// Dirichlet ring), the rest `dst` (heal_region).
Pixels heal(const Pixels& src, const Pixels& dst, const Box& box, const std::vector<bool>& mask) {
    Pixels out(dst);
    const auto w = std::size_t(box.width());
    Box b{box.x1, box.y1, box.x0, box.y0};
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (!mask[i]) continue;
        const int x = box.x0 + static_cast<int>(i % w), y = box.y0 + static_cast<int>(i / w);
        b = Box{std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x + 1), std::max(b.y1, y + 1)};
    }
    if (empty(b)) return out;
    const Box r = meet(grown(b, 1), box);
    const auto rw = std::size_t(r.width()), rh = std::size_t(r.height());
    const unsigned n = dst.all();
    const image_ops::Held held(std::uint64_t(rw) * rh * n * sizeof(float) * 8);
    std::vector<float> s(rw * rh * n), d(rw * rh * n);
    std::vector<bool> m(rw * rh);
    for (std::size_t y = 0; y < rh; ++y) {
        for (std::size_t x = 0; x < rw; ++x) {
            const std::size_t i = (std::size_t(r.y0 - box.y0) + y) * w + std::size_t(r.x0 - box.x0) + x;
            std::copy_n(src.at(i), n, s.begin() + static_cast<std::ptrdiff_t>((y * rw + x) * n));
            std::copy_n(dst.at(i), n, d.begin() + static_cast<std::ptrdiff_t>((y * rw + x) * n));
            m[y * rw + x] = mask[i];
        }
    }
    std::vector<float> made = poisson::seamless_clone(rw, rh, n, s, d, m);
    clamp_values(dst, made);
    for (std::size_t y = 0; y < rh; ++y) {
        for (std::size_t x = 0; x < rw; ++x) {
            const std::size_t i = (std::size_t(r.y0 - box.y0) + y) * w + std::size_t(r.x0 - box.x0) + x;
            std::copy_n(made.begin() + static_cast<std::ptrdiff_t>((y * rw + x) * n), n, out.at(i));
        }
    }
    return out;
}

// A mask grown by r pixels (a square).
std::vector<bool> dilate(std::size_t w, std::size_t h, const std::vector<bool>& m, std::size_t r) {
    std::vector<bool> tmp(w * h, false), out(w * h, false);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            if (!m[y * w + x]) continue;
            for (std::size_t xx = x >= r ? x - r : 0; xx < std::min(x + r + 1, w); ++xx) tmp[y * w + xx] = true;
        }
    }
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            if (!tmp[y * w + x]) continue;
            for (std::size_t yy = y >= r ? y - r : 0; yy < std::min(y + r + 1, h); ++yy) out[yy * w + x] = true;
        }
    }
    return out;
}

// --- Clone Stamp and Healing Brush --------------------------------------------------------------------------------------

// Where a clone stroke samples: `source_mm` (sampled under the first point, or `anchor_mm` when given) or `offset_mm`,
// with the clone source's transform (width and height %, rotation, flips).
struct Cloning {
    retouch::CloneMapping map;
    bool aligned = true;
    Sample which = Sample::Current;
    std::optional<std::pair<double, double>> source_mm;
};

Cloning cloning(const Json& op, const Brushwork& b, const Grid& g) {
    Cloning out;
    out.aligned = flag(op, "aligned", true);
    out.which = sample_of(op);
    const auto first = std::make_pair(b.points.front().x, b.points.front().y);
    const std::pair<double, double> anchor = pair_mm(op, "anchor_mm").value_or(first);
    const std::optional<std::pair<double, double>> offset = pair_mm(op, "offset_mm");
    out.source_mm = pair_mm(op, "source_mm");
    if (offset && out.source_mm) throw OpError("give source_mm or offset_mm, not both");
    if (!offset && !out.source_mm) throw OpError("source_mm (where to sample from) or offset_mm is required");
    const std::pair<double, double> source = offset ? std::make_pair(anchor.first + offset->first, anchor.second + offset->second) : *out.source_mm;
    out.map.anchor = {anchor.first * g.sx, anchor.second * g.sy};
    out.map.source = {source.first * g.sx, source.second * g.sy};
    if (const Json* t = field(op, "transform")) {
        if (!t->is_object()) throw OpError("transform must be {scale, rotation, flip_h, flip_v}");
        double w = 100.0, h = 100.0;
        if (const Json* scale = field(*t, "scale")) {
            if (!scale->is_array() || scale->size() != 2 || !(*scale)[0].is_number() || !(*scale)[1].is_number() || (*scale)[0].is_boolean() ||
                (*scale)[1].is_boolean())
                throw OpError("transform.scale must be [width %, height %]");
            w = (*scale)[0].get<double>();
            h = (*scale)[1].get<double>();
            if (!std::isfinite(w) || !std::isfinite(h)) throw OpError("transform.scale must be [width %, height %]");
        }
        w = std::clamp(std::fabs(w), 1.0, 1000.0);
        h = std::clamp(std::fabs(h), 1.0, 1000.0);
        const double rotation = number(*t, "rotation", 0.0);
        out.map.m = retouch::clone_matrix(w, h, rotation, flag(*t, "flip_h", false), flag(*t, "flip_v", false));
    }
    return out;
}

// The source's pixels for a destination box: a translated read, or the transformed source resampled.
Pixels clone_source(core::OpContext& c, const Brushwork& b, const Cloning& k, const Grid& g, const Box& dst, const Pixels& like) {
    if (k.map.is_translation()) {
        const auto [dx, dy] = k.map.offset();
        return sampled(c, b, k.which, g, Box{dst.x0 + dx, dst.y0 + dy, dst.x1 + dx, dst.y1 + dy}, like);
    }
    const Box from = k.map.source_box(dst);
    const Box inside = meet(from, g.whole());
    if (empty(inside)) return clear_like(like, dst);
    const Pixels src = sampled(c, b, k.which, g, inside, like);
    return retouch::resample(src, inside, dst, k.map);
}

Json clone_report(bool changed, const Cloning& k, const Brushwork& b, const Grid& g, const Box& damaged) {
    const double ox = (k.map.source.first - k.map.anchor.first) / g.sx, oy = (k.map.source.second - k.map.anchor.second) / g.sy;
    Json next = nullptr;
    if (!k.aligned) {
        const std::pair<double, double> s = k.source_mm ? *k.source_mm : std::make_pair(b.points.front().x + ox, b.points.front().y + oy);
        next = Json::array({rounded(s.first), rounded(s.second)});
    }
    return Json{{"changed", changed},
                {"offset_mm", Json::array({rounded(ox), rounded(oy)})},
                {"anchor_mm", Json::array({rounded(k.map.anchor.first / g.sx), rounded(k.map.anchor.second / g.sy)})},
                {"aligned", k.aligned},
                {"next_source_mm", next},
                {"box_mm", box_mm(damaged, g)}};
}

void clone_or_heal(core::OpContext& c, bool healing) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const Brushwork b = brushwork(c.doc, c.op);
    const Ready r = ready(c, b.at, b.id);
    const Grid& g = r.grid;
    const Cloning k = cloning(c.op, b, g);
    Coverage cov = retouch::stroke_coverage(brush_of(b, g), on_grid(b.points, g));
    if (healing) cov = retouch::padded(cov, 2);  // (nothing round the stroke: the boundary of the solve)
    const Box box = meet(cov.box, g.whole());
    if (empty(box)) {
        c.report = clone_report(false, k, b, g, {});
        return;
    }
    Pixels dst = layer_window(c, b.at, b.id, box, r.like, g);
    const Pixels src = clone_source(c, b, k, g, box, r.like);
    const std::optional<std::vector<float>> chosen = selection_over(c.doc.page(b.at), b, g, r.like, box);
    const bool lock = lock_alpha_of(c.doc, b.at, b.id);
    Box damaged;
    if (!healing) {
        // (sampled from the pixels before the stroke: what it painted is never cloned again)
        damaged = retouch::apply_coverage(dst, box, cov, b.opacity, chosen ? &*chosen : nullptr, lock, src, b.blend);
    } else {
        // the texture fitted to the layer, or to what shows when sampling more than it (healing onto an empty layer
        // matches the page)
        const Pixels fit = k.which == Sample::Current ? dst : shown_window(c.doc, b.at, b.id, k.which, g, box, r.like);
        std::vector<bool> mask(std::size_t(box.width()) * std::size_t(box.height()));
        for (int y = box.y0; y < box.y1; ++y) {
            for (int x = box.x0; x < box.x1; ++x) mask[std::size_t(y - box.y0) * std::size_t(box.width()) + std::size_t(x - box.x0)] = cov.at(x, y) > 0.0f;
        }
        const Pixels healed = heal(src, fit, box, mask);
        damaged = retouch::apply_coverage(dst, box, cov, b.opacity, chosen ? &*chosen : nullptr, lock, healed, b.blend);
    }
    if (!empty(damaged)) image_ops::put_layer_pixels(c.doc, b.at, b.id, dst, box);
    c.report = clone_report(!empty(damaged), k, b, g, damaged);
}

void clone_stamp(core::OpContext& c) { clone_or_heal(c, false); }
void healing_brush(core::OpContext& c) { clone_or_heal(c, true); }

// --- Spot Healing Brush ------------------------------------------------------------------------------------------------

void spot_healing(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const Brushwork b = brushwork(c.doc, c.op);
    const std::string type = choice(c.op, "type", {"content_aware", "create_texture", "proximity_match"});
    const Ready r = ready(c, b.at, b.id);
    const Grid& g = r.grid;
    const retouch::Brush brush = brush_of(b, g);
    const Coverage cov = retouch::stroke_coverage(brush, on_grid(b.points, g));
    const float size = brush.size;
    const int margin = static_cast<int>(std::ceil(std::max(size, 8.0f))) + 8;
    const Box region = empty(cov.box) ? Box{} : meet(grown(cov.box, margin), g.whole());
    Json report{{"changed", false}, {"box_mm", nullptr}};
    if (empty(region)) {
        c.report = report;
        return;
    }
    Pixels img = layer_window(c, b.at, b.id, region, r.like, g);
    const auto w = std::size_t(region.width()), h = std::size_t(region.height());
    const unsigned ch = img.all();
    std::vector<bool> hole(w * h, false);
    for (int y = std::max(region.y0, cov.box.y0); y < std::min(region.y1, cov.box.y1); ++y) {
        for (int x = std::max(region.x0, cov.box.x0); x < std::min(region.x1, cov.box.x1); ++x) {
            hole[std::size_t(y - region.y0) * w + std::size_t(x - region.x0)] = cov.at(x, y) > 0.0f;
        }
    }
    if (std::none_of(hole.begin(), hole.end(), [](bool v) { return v; })) {
        c.report = report;
        return;
    }
    // Made over a slightly larger domain than the Poisson region, so the solve's boundary holds made values that differ
    // from the surroundings: that mismatch is what the membrane corrects (else the blend would be nothing).
    const std::vector<bool> domain = dilate(w, h, hole, 2);
    const std::vector<bool> heal_mask = dilate(w, h, hole, 1);
    const image_ops::Held held(std::uint64_t(w) * h * ch * sizeof(float) * 12);
    const std::vector<float> data = values_of(img);
    const auto content_aware = [&]() {
        std::optional<std::vector<float>> made = inpaint::complete(w, h, ch, data, domain);
        return made ? std::move(*made) : poisson::membrane_fill(w, h, ch, data, domain);
    };
    std::vector<float> filled;
    if (type == "content_aware") {
        filled = content_aware();
    } else if (type == "create_texture") {
        filled = inpaint::synthesize(w, h, ch, data, domain, static_cast<std::size_t>(std::clamp(size / 4.0f, 6.0f, 32.0f)), 0x5eed);
    } else {
        const auto ring = static_cast<std::size_t>(std::clamp(size / 8.0f, 3.0f, 16.0f));
        if (const std::optional<std::pair<int, int>> off = inpaint::best_offset(w, h, ch, data, domain, ring, margin)) {
            filled = data;
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    if (!domain[y * w + x]) continue;
                    const std::size_t j = (std::size_t(static_cast<long>(y) + off->second) * w + std::size_t(static_cast<long>(x) + off->first)) * ch;
                    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(j), ch, filled.begin() + static_cast<std::ptrdiff_t>((y * w + x) * ch));
                }
            }
        } else {
            filled = content_aware();
        }
    }
    clamp_values(img, filled);
    Pixels made = clear_like(img, region);
    put_values(made, filled);
    std::vector<bool> mask(heal_mask.begin(), heal_mask.end());
    const Pixels healed = heal(made, img, region, mask);
    const std::optional<std::vector<float>> chosen = selection_over(c.doc.page(b.at), b, g, r.like, region);
    const Box damaged = retouch::apply_coverage(img, region, cov, b.opacity, chosen ? &*chosen : nullptr, lock_alpha_of(c.doc, b.at, b.id), healed,
                                                "normal");
    if (!empty(damaged)) image_ops::put_layer_pixels(c.doc, b.at, b.id, img, region);
    c.report = Json{{"changed", !empty(damaged)}, {"box_mm", box_mm(damaged, g)}};
}

// --- History Brush -----------------------------------------------------------------------------------------------------

// The pixels before (`from`'s bytes) over `box` of the grid, as the layer's kind; `refused()` when they are not this
// layer's (another kind or size).
Pixels before_window(const core::Page& page, const std::string& bytes, const Pixels& like, const Grid& g, const Box& box,
                     const std::function<OpError()>& refused) {
    Pixels out = clear_like(like, box);
    const auto w = std::size_t(box.width());
    try {
        switch (like.kind()) {
            case Pixels::Kind::Precise: {
                const core::ColorRasterView view(bytes);
                if (static_cast<int>(view.width()) != g.size.width || static_cast<int>(view.height()) != g.size.height) throw refused();
                for (int y = box.y0; y < box.y1; ++y) {
                    for (int x = box.x0; x < box.x1; ++x) {
                        const auto px = view.pixel(std::size_t(y) * view.width() + std::size_t(x));
                        float* v = out.at(std::size_t(y - box.y0) * w + std::size_t(x - box.x0));
                        for (unsigned k = 0; k < 4; ++k) v[k] = static_cast<float>(px[k]);
                    }
                }
                break;
            }
            case Pixels::Kind::Eight: {
                const Image picture = open_image(bytes, kPillowOpenLimits);
                if (picture.width() != g.size.width || picture.height() != g.size.height) throw refused();
                const Pixels part = image_ops::from_image(picture.crop(box));
                place(out, box, part, box);
                break;
            }
            case Pixels::Kind::Inks: {
                if (!page.color) throw refused();
                const core::InkPixels was = core::decode_ink_raster(bytes);
                if (static_cast<int>(was.width) != g.size.width || static_cast<int>(was.height) != g.size.height || was.space != page.color->space ||
                    was.colours() != like.colours())
                    throw refused();
                core::InkPixels part = core::blank_ink_pixels(was.space, was.depth, static_cast<std::uint32_t>(box.width()),
                                                              static_cast<std::uint32_t>(box.height()), was.planes);
                for (int y = box.y0; y < box.y1; ++y) {
                    for (int x = box.x0; x < box.x1; ++x) {
                        part.copy_pixel(std::size_t(y - box.y0) * w + std::size_t(x - box.x0), was, std::size_t(y) * was.width + std::size_t(x));
                    }
                }
                const Pixels values = image_ops::from_inks(part, *page.color);
                place(out, box, values, box);
                break;
            }
        }
    } catch (const OpError&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "memory") throw;
        throw refused();
    }
    return out;
}

void history_brush(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const Brushwork b = brushwork(c.doc, c.op);
    const Json* from = field(c.op, "from");
    if (from == nullptr || !(from->is_object() || (from->is_string() && storage::AssetStore::is_ref(from->get<std::string>()))))
        throw OpError("from (the layer's pixels of the state painted back: their asset sha256:…, or a precise layer's tiles as project.json holds "
                      "them) is required");
    const Ready r = ready(c, b.at, b.id);
    const Grid& g = r.grid;
    const auto refused = [] { return OpError("from is not the pixels of this layer (another kind or size): that state cannot be painted back"); };
    const core::Page& page = c.doc.page(b.at);
    const std::string bytes = adjust_ops::pixels_before(c.doc, page, layer_of(page, b.id), *from, "history_brush", refused);
    const Coverage cov = retouch::stroke_coverage(brush_of(b, g), on_grid(b.points, g));
    const Box box = meet(cov.box, g.whole());
    if (empty(box)) {
        c.report = Json{{"changed", false}, {"box_mm", nullptr}};
        return;
    }
    const Pixels paint = before_window(c.doc.page(b.at), bytes, r.like, g, box, refused);
    Pixels dst = layer_window(c, b.at, b.id, box, r.like, g);
    const std::optional<std::vector<float>> chosen = selection_over(c.doc.page(b.at), b, g, r.like, box);
    const Box damaged = retouch::apply_coverage(dst, box, cov, b.opacity, chosen ? &*chosen : nullptr, lock_alpha_of(c.doc, b.at, b.id), paint, b.blend);
    if (!empty(damaged)) image_ops::put_layer_pixels(c.doc, b.at, b.id, dst, box);
    c.report = Json{{"changed", !empty(damaged)}, {"box_mm", box_mm(damaged, g)}};
}

// --- Content-Aware Fill ------------------------------------------------------------------------------------------------

// A blank layer of the same kind and grid as `like`'s, above it (its id `id`).
void add_blank_above(core::Document& doc, std::size_t at, const std::string& below, const std::string& id, const Pixels& like, const Grid& g) {
    core::Page& page = doc.edit_page(at);
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) throw OpError("layer " + id + " exists");
    }
    std::size_t index = 0;
    while (index < page.layers.size() && page.layers[index].id != below) ++index;
    core::Layer made;
    made.id = id;
    made.role = core::LayerRole::User;
    made.kind = core::LayerKind::Raster;
    made.title = "コンテンツに応じた塗りつぶし";
    made.exportable = true;
    made.panel_each = true;
    if (index < page.layers.size()) made.parent_id = page.layers[index].parent_id;
    if (like.kind() == Pixels::Kind::Precise) {
        made.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
            static_cast<std::uint32_t>(g.size.width), static_cast<std::uint32_t>(g.size.height), like.floating ? "f32" : "u16",
            [](std::size_t) { return std::array<double, 4>{0.0, 0.0, 0.0, 0.0}; }));
    } else if (like.kind() == Pixels::Kind::Inks) {
        const core::PageColor& color = *page.color;
        made.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(core::blank_ink_pixels(
            color.space, color.depth, static_cast<std::uint32_t>(g.size.width), static_cast<std::uint32_t>(g.size.height), color.planes())));
    }
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(std::min(index + 1, page.layers.size())), std::move(made));
}

void content_aware_fill(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* given = field(op, "layer_id");
    if (given == nullptr || !given->is_string()) throw OpError("layer_id is required");
    const std::string id = given->get<std::string>();
    const Json* area = field(op, "area");
    if (area == nullptr) throw OpError("area (what to fill: the selection) is required");
    const std::string sampling = choice(op, "sampling", {"auto", "rectangular", "custom"});
    const std::string colour = choice(op, "color_adaptation", {"default", "none", "high", "very_high"});
    const std::string rotation = choice(op, "rotation_adaptation", {"none", "low", "medium", "high", "full"});
    const std::string output = choice(op, "output", {"current", "new", "duplicate"});
    const Json* seed_given = field(op, "seed");
    if (seed_given != nullptr && !(seed_given->is_number_integer() && seed_given->get<std::int64_t>() >= 0))
        throw OpError("seed must be a whole number (0 or more)");
    const auto seed = seed_given != nullptr ? static_cast<std::uint64_t>(seed_given->get<std::int64_t>()) : std::uint64_t{1};
    const double margin_mm = number(op, "margin_mm", -1.0);
    if (field(op, "margin_mm") != nullptr && margin_mm < 0.0) throw OpError("margin_mm must be 0 or more");
    const Json* source_given = field(op, "source_area");
    if (sampling == "custom" && source_given == nullptr) throw OpError("custom sampling needs source_area (where to sample from)");
    std::optional<Json> source_area;
    if (source_given != nullptr) {
        source_area = core::area_needs_resolving(*source_given) ? ops_registry().area_resolver()(doc, at, *source_given) : *source_given;
    }
    const Json* new_given = field(op, "new_id");
    if (new_given != nullptr && (!new_given->is_string() || new_given->get<std::string>().empty())) throw OpError("new_id must be a layer id");
    // (out to a new layer or a copy, the layer itself is not changed: its pixels read from a copy of the book — a pen
    // layer's lines and shape parts drawn in there, not into it)
    std::optional<core::Document> apart;
    if (output != "current") apart.emplace(doc);
    core::OpContext read{apart ? *apart : doc, op, c.actor};
    const Ready r = ready(read, at, id);
    const Grid& g = r.grid;
    const bool eight = r.like.kind() == Pixels::Kind::Eight;
    const AreaCover cover(doc.page(at), *area, g, eight);
    const Box hb = cover.box(g.whole());
    if (empty(hb)) throw OpError("content_aware_fill: the area covers nothing of the layer");
    const int ext = std::max(hb.width(), hb.height());
    Box window;
    std::optional<AreaCover> source_cover;
    if (sampling == "auto") {
        window = grown(hb, std::max(ext * 3 / 4, 32));
    } else if (sampling == "rectangular") {
        window = grown(hb, margin_mm >= 0 ? static_cast<int>(std::lround(std::min(g.px(margin_mm), 100000.0))) : std::max(ext, 16));
    } else {
        source_cover.emplace(doc.page(at), *source_area, g, eight);
        const Box sb = source_cover->box(g.whole());
        if (empty(sb)) throw OpError("content_aware_fill: source_area covers nothing of the layer");
        window = grown(Box{std::min(sb.x0, hb.x0), std::min(sb.y0, hb.y0), std::max(sb.x1, hb.x1), std::max(sb.y1, hb.y1)}, 4);
    }
    window = meet(window, g.whole());
    content_aware::FillOptions opts;
    opts.color_adaptation = content_aware::color_level(colour);
    opts.rotations = content_aware::rotation_level(rotation);
    opts.scale = flag(op, "scale", false);
    opts.mirror = flag(op, "mirror", false);
    opts.seed = seed;
    opts.unit_range = !(r.like.kind() == Pixels::Kind::Precise && r.like.floating);
    const Pixels img = layer_window(read, at, id, window, r.like, g);
    const auto w = std::size_t(window.width()), h = std::size_t(window.height());
    const unsigned n = img.all();
    const std::vector<float> covered = cover.over(window);
    std::vector<bool> hole(w * h), source(w * h, true);
    for (std::size_t i = 0; i < w * h; ++i) hole[i] = covered[i] > 0.0f;
    if (source_cover) {
        const std::vector<float> s = source_cover->over(window);
        for (std::size_t i = 0; i < w * h; ++i) source[i] = s[i] > 0.5f;
    }
    const image_ops::Held held(std::uint64_t(w) * h * n * sizeof(float) * 12);
    std::vector<float> filled = content_aware::fill(w, h, n, values_of(img), hole, source, opts);
    clamp_values(img, filled);
    const auto n_hole = static_cast<std::int64_t>(std::count(hole.begin(), hole.end(), true));
    std::string target = id;
    const bool lock = lock_alpha_of(doc, at, id);
    Pixels out = clear_like(img, window);
    if (output == "new") {  // (the fill alone on a new layer above, its alpha the selection's coverage)
        target = new_given != nullptr ? new_given->get<std::string>() : core::new_id();
        add_blank_above(doc, at, id, target, r.like, g);
        for (std::size_t i = 0; i < w * h; ++i) {
            if (!hole[i]) continue;
            float* v = out.at(i);
            std::copy_n(filled.begin() + static_cast<std::ptrdiff_t>(i * n), n, v);
            v[n - 1] *= covered[i];
        }
    } else {
        if (output == "duplicate") {
            target = new_given != nullptr ? new_given->get<std::string>() : core::new_id();
            const Json duplicate{{"op", "duplicate_layer"}, {"page", doc.page(at).index.json()}, {"id", id}, {"new_id", target}};
            core::OpContext inner{doc, duplicate, c.actor};
            (*ops_registry().find("duplicate_layer"))(inner);
            (void)ready(c, at, target);  // (the copy's lines and shape parts drawn into its pixels: it is the paint layer filled)
        }
        for (std::size_t i = 0; i < w * h; ++i) {
            const float* was = img.at(i);
            float* v = out.at(i);
            const float k = covered[i];
            for (unsigned q = 0; q < n; ++q) {
                const bool alpha = q + 1 == n;
                v[q] = alpha && lock ? was[q] : was[q] + (filled[i * n + q] - was[q]) * k;
            }
        }
    }
    image_ops::put_layer_pixels(doc, at, target, out, window);
    c.report = Json{{"layer", target}, {"filled", n_hole}, {"window_mm", box_mm(window, g)}};
}

}  // namespace

image_ops::Pixels page_shown_over(core::OpContext& c, std::size_t at, const std::string& id, const Box& box) {
    const Ready r = ready(c, at, id);
    return shown_window(c.doc, at, id, Sample::All, r.grid, box, r.like);
}

void register_retouch_ops(core::OpRegistry& registry) {
    // the brushes' common keys: page, layer_id, points ([[x_mm, y_mm, pressure?]…]), width_mm, hardness, opacity, flow,
    // spacing (0..100 %, 1..1000 % of the diameter), blend, area?
    registry.add("clone_stamp", clone_stamp);      // (+ source_mm | offset_mm, anchor_mm?, aligned, transform?, sample)
    registry.add("healing_brush", healing_brush);  // (as clone_stamp, blended by Poisson)
    registry.add("spot_healing", spot_healing);    // (+ type: content_aware | create_texture | proximity_match)
    registry.add("history_brush", history_brush);  // (+ from: the layer's pixels of a state, as fade_layer's)
    registry.add("content_aware_fill", content_aware_fill);  // ({page, layer_id, area, sampling, …, output, new_id?, seed})
}

}  // namespace genko::render
