// A page drawn from its data: Python's genko/render.py (render_page and the functions it calls).
//
// Every picture of the page is held for a box `area` of it (the whole page, or a render region): per-pixel steps are
// done for that box only, shapes are drawn in page coordinates (PageCanvas), resizes compute that part only, and the
// steps that look at neighbouring pixels (layer effects) work on a box widened by their reach. The result is the
// same pixels as drawing the whole page and cutting the box out (docs/cpp-migration/ARCHITECTURE.md §4a).

#include "render/page.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <QFileInfo>

#include "core/adjust.hpp"
#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/frames.hpp"
#include "core/pyconv.hpp"
#include "core/placement.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/channels.hpp"
#include "render/color_canvas.hpp"
#include "render/color_filters.hpp"
#include "render/covers.hpp"
#include "render/effects.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ink_filters.hpp"
#include "render/op_limits.hpp"
#include "render/page_internal.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/png.hpp"
#include "render/prims.hpp"
#include "render/raster.hpp"
#include "render/text/balloons.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/tones.hpp"

namespace genko::render {

namespace detail {

void check_cancel(const Ctx& ctx) {
    if (ctx.stop.stop_requested()) throw Cancelled();
}

bool skip_unported(const Ctx& ctx, const std::string& element) {
    if (!ctx.skip_unported) throw NotYetPorted(element);
    if (ctx.omitted != nullptr && std::find(ctx.omitted->begin(), ctx.omitted->end(), element) == ctx.omitted->end()) {
        ctx.omitted->push_back(element);
    }
    return true;
}

}  // namespace detail

namespace {

using namespace detail;
using core::get;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::LayerRole;
using core::Num;
using core::Page;

const std::vector<std::int64_t> kNameColor{58, 110, 165};
const std::vector<std::int64_t> kInkColor{20, 20, 20};
constexpr int kQuickDpi = 32;  // at or below this lines are drawn as plain polylines
constexpr std::size_t kRoughFrom = 300;
constexpr std::size_t kStrokeCacheSize = 12;
constexpr std::int64_t kStrokeCachePixels = 80'000'000;

bool guide_role(LayerRole role) { return role == LayerRole::Name || role == LayerRole::Draft; }

bool intersects(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

Box intersection(const Box& a, const Box& b) {
    return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

bool contains(const Box& outer, const Box& inner) {
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && inner.x1 <= outer.x1 && inner.y1 <= outer.y1;
}

Box shifted(const Box& b, int dx, int dy) { return Box{b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy}; }

Size size_of(const Box& b) { return Size{b.width(), b.height()}; }

Image transparent(const Box& area) { return Image::create("RGBA", size_of(area), Ink{0, 0, 0, 0}); }

std::vector<std::int64_t> first3(std::vector<std::int64_t> v) {
    if (v.size() > 3) v.resize(3);
    return v;
}

// tuple(stroke.rgb or brush.rgb or INK_COLOR)
std::vector<std::int64_t> stroke_rgb(const core::Stroke& stroke, const brushes::Brush& b) {
    if (stroke.rgb && !stroke.rgb->empty()) return *stroke.rgb;
    if (b.rgb && !b.rgb->empty()) return *b.rgb;
    return kInkColor;
}

// float(stroke.width_mm or 0.35)
double stroke_width(const core::Stroke& s) { return s.width_mm != 0.0 ? s.width_mm : 0.35; }

// --- decoded pictures --------------------------------------------------------------------------------------------
// (Decoding a large PNG costs more than drawing the page: each picture is decoded once while its bytes live.)

struct Decoded {
    std::weak_ptr<const std::string> bytes;
    const std::string* address = nullptr;
    std::string purpose;
    Image image;
};

std::mutex g_decoded_mutex;
std::vector<std::shared_ptr<const Decoded>> g_decoded;
constexpr std::size_t kDecodedCount = 24;
constexpr std::int64_t kDecodedPixels = 160'000'000;

// The picture of `bytes` as `purpose`: "open" (as opened), "RGBA", "RGBa" (from RGBA, premultiplied) or "L".
Image decoded(const core::Bytes& bytes, const std::string& purpose) {
    {
        std::lock_guard lock(g_decoded_mutex);
        for (const auto& d : g_decoded) {
            if (d->address == bytes.get() && d->purpose == purpose && !d->bytes.expired()) return d->image;
        }
    }
    Image image;
    if (purpose == "open") {
        image = open_image(*bytes, kPillowOpenLimits);
    } else if (purpose == "RGBa") {
        image = decoded(bytes, "RGBA").convert("RGBa");
    } else {
        image = decoded(bytes, "open").convert(purpose);
    }
    auto entry = std::make_shared<Decoded>();
    entry->bytes = bytes;
    entry->address = bytes.get();
    entry->purpose = purpose;
    entry->image = image;
    std::lock_guard lock(g_decoded_mutex);
    std::erase_if(g_decoded, [](const auto& d) { return d->bytes.expired(); });
    g_decoded.push_back(std::move(entry));
    std::int64_t pixels = 0;
    for (const auto& d : g_decoded) pixels += static_cast<std::int64_t>(d->image.width()) * d->image.height();
    while (g_decoded.size() > 1 && (g_decoded.size() > kDecodedCount || pixels > kDecodedPixels)) {
        pixels -= static_cast<std::int64_t>(g_decoded.front()->image.width()) * g_decoded.front()->image.height();
        g_decoded.erase(g_decoded.begin());
    }
    return image;
}

// raster.resize(size) for an RGBA picture (BICUBIC, premultiplied as Pillow does), the part `area` only.
Image raster_part(const core::Bytes& bytes, Size size, const Box& area) {
    const Image rgba = decoded(bytes, "RGBA");
    if (rgba.size() == size) return rgba.crop(area);
    return decoded(bytes, "RGBa").resize_region(size, area, Resample::Bicubic).convert("RGBA");
}

// --- the stroke cache (render._STROKE_CACHE) ----------------------------------------------------------------------

struct StrokeSig {
    std::string id;
    std::string kind;
    double width_mm = 0;
    std::optional<std::vector<std::int64_t>> rgb;
    double opacity = 0;
    std::size_t points = 0;
    std::optional<core::PointF> first;
    std::optional<core::PointF> last;
    std::size_t pressure = 0;
    brushes::Brush brush;
    double pressure_opacity = 0;

    bool operator==(const StrokeSig& o) const {
        const auto same_point = [](const std::optional<core::PointF>& a, const std::optional<core::PointF>& b) {
            if (a.has_value() != b.has_value()) return false;
            return !a || (a->x == b->x && a->y == b->y);
        };
        return id == o.id && kind == o.kind && width_mm == o.width_mm && rgb == o.rgb && opacity == o.opacity &&
               points == o.points && same_point(first, o.first) && same_point(last, o.last) && pressure == o.pressure &&
               brush == o.brush && pressure_opacity == o.pressure_opacity;
    }
};

StrokeSig sig_of(const core::Stroke& s) {
    StrokeSig sig;
    sig.id = s.id;
    sig.kind = s.kind;
    sig.width_mm = s.width_mm;
    sig.rgb = s.rgb;
    sig.opacity = s.opacity;
    sig.points = s.points.size();
    if (!s.points.empty()) {
        sig.first = s.points.front();
        sig.last = s.points.back();
    }
    sig.pressure = s.pressure.size();
    sig.brush = brushes::brush(s.kind);
    sig.pressure_opacity = s.pressure_opacity;
    return sig;
}

struct PatchSig {
    Json id;
    Json box;
    std::size_t png = 0;
    std::string rgb;
    Json opacity;
    Json mode;

    bool operator==(const PatchSig& o) const {
        return id == o.id && box == o.box && png == o.png && rgb == o.rgb && opacity == o.opacity && mode == o.mode;
    }
};

PatchSig patch_sig_of(const core::Patch& p) {
    PatchSig sig;
    const auto value = [&](std::string_view key) {
        const Json* v = get(p.attrs, key);
        return v != nullptr ? *v : Json();
    };
    sig.id = value("id");
    const Json box = value("box");
    sig.box = core::py_truthy(box) ? core::py_list(box) : Json::array();
    sig.png = p.png ? p.png->size() : 0;
    sig.rgb = core::py_str(value("rgb"));
    sig.opacity = value("opacity");
    sig.mode = value("mode");
    return sig;
}

struct CacheEntry {
    std::string id;
    int dpi = 0;
    Size size;
    bool guide = false;
    Box area;
    std::vector<PatchSig> patches;
    std::vector<StrokeSig> sigs;
    Image image;
};

std::mutex g_cache_mutex;
std::vector<std::shared_ptr<const CacheEntry>> g_stroke_cache;  // oldest first

std::shared_ptr<const CacheEntry> cached_strokes(const std::string& id, int dpi, Size size, bool guide, const Box& area) {
    std::lock_guard lock(g_cache_mutex);
    std::shared_ptr<const CacheEntry> found;
    for (const auto& e : g_stroke_cache) {
        if (e->id != id || e->dpi != dpi || e->size != size || e->guide != guide) continue;
        if (e->area == area) return e;
        // a picture of more of the layer serves too (the part is cut from it)
        if (contains(e->area, area) && (!found || e->area.width() * e->area.height() > found->area.width() * found->area.height())) {
            found = e;
        }
    }
    return found;
}

void keep_strokes(std::shared_ptr<const CacheEntry> entry) {
    std::lock_guard lock(g_cache_mutex);
    std::erase_if(g_stroke_cache, [&](const auto& e) {
        return e->id == entry->id && e->dpi == entry->dpi && e->size == entry->size && e->guide == entry->guide &&
               e->area == entry->area;
    });
    g_stroke_cache.push_back(std::move(entry));
    const auto pixels = [] {
        std::int64_t total = 0;
        for (const auto& e : g_stroke_cache) total += static_cast<std::int64_t>(e->image.width()) * e->image.height();
        return total;
    };
    while (g_stroke_cache.size() > 1 && (g_stroke_cache.size() > kStrokeCacheSize || pixels() > kStrokeCachePixels)) {
        g_stroke_cache.erase(g_stroke_cache.begin());
    }
}

// --- frame masks (render._FRAME_MASKS) ------------------------------------------------------------------------------

struct MaskEntry {
    std::string key;
    Image mask;
};

std::mutex g_mask_mutex;
std::vector<std::shared_ptr<const MaskEntry>> g_masks;

std::string mask_key(const Page& page, const core::Frame& frame, Size size, int dpi, const Box& area) {
    // everything the mask depends on: the panel, the page's frame of reference and the area
    Json key = Json::array();
    const auto pts = [](const std::vector<PointMM>& v) {
        Json out = Json::array();
        for (const PointMM& p : v) out.push_back(Json::array({p.x, p.y}));
        return out;
    };
    const auto shape = core::bleed_poly(page, &frame);
    key.push_back(shape ? pts(*shape) : pts(outline_mm(frame)));
    const auto outline = bleed_outline_mm(page, &frame, std::nullopt);
    key.push_back(outline ? pts(*outline) : Json());
    const Box clip = rect_px(core::clip_box(page, &frame, "bleed"), dpi);
    key.push_back(Json::array({frame.bleed, frame.poly.has_value(), core::rounded(frame), clip.x0, clip.y0, clip.x1, clip.y1}));
    const Box r = rect_px(frame.rect, dpi);
    key.push_back(Json::array({r.x0, r.y0, r.x1, r.y1, size.width, size.height, dpi, area.x0, area.y0, area.x1, area.y1}));
    return core::dump_canonical(key);
}

Image cached_frame_mask(const Page& page, const core::Frame& frame, Size size, int dpi, const Box& area) {
    const std::string key = mask_key(page, frame, size, dpi, area);
    {
        std::lock_guard lock(g_mask_mutex);
        for (const auto& m : g_masks) {
            if (m->key == key) return m->mask;
        }
    }
    Image mask = frame_mask(page, frame, size, dpi, area);
    std::lock_guard lock(g_mask_mutex);
    if (g_masks.size() > 48) g_masks.clear();
    g_masks.push_back(std::make_shared<MaskEntry>(MaskEntry{key, mask}));
    return mask;
}

// --- patches (render._paint_patch) and plain lines (render._quick_strokes) -----------------------------------------

void paint_patch(Image& out, const Box& area, const core::Patch& patch, int dpi, const std::vector<std::int64_t>* colour) {
    if (!patch.png || patch.png->empty()) return;
    const Json* box_json = get(patch.attrs, "box");
    if (box_json == nullptr) throw core::Error("key", "'box'");
    const Json box = core::py_list(*box_json);
    if (box.size() != 4) {
        throw core::Error("value", box.size() > 4 ? "too many values to unpack (expected 4)"
                                                   : "not enough values to unpack (expected 4, got " + std::to_string(box.size()) + ")");
    }
    const double x = core::py_float(box[0]);
    const double y = core::py_float(box[1]);
    const double w = core::py_float(box[2]);
    const double h = core::py_float(box[3]);
    const auto round_px = [&](double mm) { return static_cast<int>(core::py_round_int(mm / 25.4 * dpi)); };
    const int x0 = round_px(x);
    const int y0 = round_px(y);
    const int pw = std::max(1, round_px(w));
    const int ph = std::max(1, round_px(h));
    const Box where{x0, y0, x0 + pw, y0 + ph};
    if (!intersects(where, area)) return;  // (nothing of it in this part of the page)
    const Box part = intersection(where, area);
    const Box in_piece = shifted(part, -x0, -y0);
    const Json* opacity_json = get(patch.attrs, "opacity");
    const double opacity = core::py_clamp(opacity_json != nullptr ? core::py_float(*opacity_json) : 1.0, 0.0, 1.0);
    const Json* mode = get(patch.attrs, "mode");
    const bool mask_mode = mode == nullptr || (mode->is_string() && mode->get<std::string>() == "mask");
    Image piece;
    if (mask_mode) {
        const Image source = decoded(patch.png, "L");
        Image cover = source.resize_region(Size{pw, ph}, in_piece, Resample::Lanczos);
        if (pw > source.width() * 1.5) cover = cover.point([](int v) { return v >= 128 ? 255 : 0; });  // upscaled fills
        if (opacity < 1) cover = cover.point([&](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); });
        std::vector<std::int64_t> rgb;
        if (colour != nullptr) {
            rgb = *colour;
        } else {
            const Json* own = get(patch.attrs, "rgb");
            rgb = (own != nullptr && core::py_truthy(*own)) ? core::int_tuple(*own) : kInkColor;
        }
        piece = Image::create("RGBA", size_of(in_piece), Ink::with_alpha(rgb, 0));
        piece.putalpha(cover);
    } else {
        // image.convert("RGBA").resize((pw, ph), LANCZOS): unchanged when it is that size already
        const Image rgba = decoded(patch.png, "RGBA");
        if (rgba.size() == Size{pw, ph}) {
            piece = rgba.crop(in_piece);
        } else {
            piece = decoded(patch.png, "RGBa").resize_region(Size{pw, ph}, in_piece, Resample::Lanczos).convert("RGBA");
        }
        if (opacity < 1) {
            piece.putalpha(piece.getchannel(3).point([&](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); }));
        }
    }
    const Box local = shifted(part, -area.x0, -area.y0);
    const Image region = out.crop(local);
    out.paste(alpha_composite(region, piece), Point{local.x0, local.y0});
}

Image quick_strokes(const Ctx& ctx, std::span<const core::StrokePtr> strokes, std::span<const core::Patch> patches,
                    const Box& area, bool guide) {
    Image out = transparent(area);
    for (const core::Patch& patch : patches) paint_patch(out, area, patch, ctx.dpi, guide ? &kNameColor : nullptr);
    PageCanvas canvas(out, area, ctx.size);
    Draw& draw = canvas.draw();
    const double scale = ctx.dpi / 25.4;
    for (const core::StrokePtr& stroke : strokes) {
        if (stroke->points.empty()) continue;
        const brushes::Brush b = brushes::brush(stroke->kind);
        const std::vector<std::int64_t> rgb = guide ? kNameColor : stroke_rgb(*stroke, b);
        const auto shade = core::py_trunc_int(255 * core::py_clamp(stroke->opacity, 0.0, 1.0) * b.opacity);
        std::vector<PointD> pts;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (const core::PointF& p : stroke->points) {
            const PointD q{p.x * scale, p.y * scale};
            if (pts.empty()) {
                x0 = x1 = q.x;
                y0 = y1 = q.y;
            }
            x0 = std::min(x0, q.x);
            x1 = std::max(x1, q.x);
            y0 = std::min(y0, q.y);
            y1 = std::max(y1, q.y);
            pts.push_back(q);
        }
        const int width = std::max(1, static_cast<int>(core::py_round_int(stroke_width(*stroke) * scale)));
        // (a line far from this part of the page changes none of its pixels)
        const double reach = width + 4;
        if (x1 + reach < area.x0 || x0 - reach > area.x1 || y1 + reach < area.y0 || y0 - reach > area.y1) continue;
        const Ink ink = Ink::with_alpha(rgb, shade);
        if (pts.size() == 1) {
            draw.point(pts, ink);
        } else {
            draw.line(pts, ink, width, width > 2 ? Joint::Curve : Joint::None);
        }
    }
    canvas.commit();
    return out;
}

// The alpha band of an RGBA picture (Python's im.split()[3]: the same pixels, without making the other bands).
Image alpha_of(const Image& rgba) { return rgba.getchannel(3); }

// render._clipped
Image clipped(const Layer& layer, Image out, const Image* panel_mask, const Image* raster) {
    if (panel_mask != nullptr && layer.panel_clip) out.putalpha(chops::multiply(alpha_of(out), *panel_mask));
    if (layer.lock_alpha && raster != nullptr) {
        const Image raster_alpha = raster->mode() == "RGBA" ? alpha_of(*raster) : alpha_of(raster->convert("RGBA"));
        out.putalpha(chops::multiply(alpha_of(out), raster_alpha));
    }
    return out;
}

// --- a layer's lines (render._layer_strokes, _each_panel) ------------------------------------------------------------

struct StrokeSet {
    std::string id;  // "" for none (no cache)
    std::vector<core::StrokePtr> strokes;
    std::vector<core::Patch> patches;
};

std::optional<Image> draw_strokes(const Ctx& ctx, const Layer& layer, const StrokeSet& set, const Box& area,
                                  const Image* panel_mask, const Image* raster, bool rough) {
    const bool guide = guide_role(layer.role);
    if (ctx.episode != nullptr) paper::keep_all(ctx.episode->papers);  // (BRUSH-01: its brushes' papers)
    if (ctx.dpi <= kQuickDpi || rough) {
        return clipped(layer, quick_strokes(ctx, set.strokes, set.patches, area, guide), panel_mask, raster);
    }
    // the lines drawn so far are remembered per layer and resolution: a new line is drawn on top of them
    std::vector<PatchSig> patch_sigs;
    for (const core::Patch& p : set.patches) patch_sigs.push_back(patch_sig_of(p));
    std::vector<StrokeSig> sigs;
    sigs.reserve(set.strokes.size());
    for (const core::StrokePtr& s : set.strokes) sigs.push_back(sig_of(*s));
    std::shared_ptr<const CacheEntry> cached;
    if (!set.id.empty()) cached = cached_strokes(set.id, ctx.dpi, ctx.size, guide, area);
    Image out;
    std::size_t start = 0;
    if (cached && cached->patches == patch_sigs && cached->sigs.size() <= sigs.size() &&
        std::equal(cached->sigs.begin(), cached->sigs.end(), sigs.begin())) {
        out = cached->area == area ? cached->image.copy() : cached->image.crop(shifted(area, -cached->area.x0, -cached->area.y0));
        start = cached->sigs.size();
    } else {
        out = transparent(area);
        for (const core::Patch& patch : set.patches) paint_patch(out, area, patch, ctx.dpi, guide ? &kNameColor : nullptr);
    }
    // lines of one colour gather in one coverage mask (screen: a + b − ab) and go onto the layer once per colour
    // (the mask is made when a line of the colour first covers part of this area: an empty one lays nothing)
    std::optional<Image> ink;
    std::optional<std::vector<std::int64_t>> ink_rgb;
    const auto lay = [&] {
        if (!ink) return;
        const auto box = ink->getbbox();
        if (box) {
            Image patch = Image::create("RGBA", Size{box->width(), box->height()}, Ink::with_alpha(*ink_rgb, 0));
            patch.putalpha(ink->crop(*box));
            out.alpha_composite(patch, Point{box->x0, box->y0});
        }
        ink.reset();
    };
    for (std::size_t n = start; n < set.strokes.size(); ++n) {
        if ((n - start) % 64 == 63) check_cancel(ctx);
        const core::Stroke& stroke = *set.strokes[n];
        const brushes::Brush b = brushes::brush(stroke.kind);
        const core::PenPoints points = core::stroke_points(stroke);
        const double width = stroke_width(stroke);
        const auto reach = brushes::extent(ctx.size, points, ctx.dpi, width, stroke.kind);
        if (!reach) continue;  // (drawn is None)
        const std::vector<std::int64_t> rgb = guide ? kNameColor : stroke_rgb(stroke, b);
        if (!ink_rgb || rgb != *ink_rgb) {
            lay();
            ink_rgb = rgb;
        }
        if (!intersects(*reach, area)) continue;  // (it covers nothing of this part of the page)
        auto drawn = brushes::draw(ctx.size, points, ctx.dpi, width, stroke.kind, stroke.id, stroke.rotation,
                                   stroke.pressure_opacity);
        if (!drawn) continue;
        Image cover = std::move(drawn->mask);
        const double opacity = core::py_clamp(stroke.opacity, 0.0, 1.0) * b.opacity;
        if (opacity < 1) cover = cover.point([&](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); });
        const Box box{drawn->origin.x, drawn->origin.y, drawn->origin.x + cover.width(), drawn->origin.y + cover.height()};
        const Box part = intersection(box, area);
        if (part.x1 <= part.x0 || part.y1 <= part.y0) continue;
        const Box in_ink = shifted(part, -area.x0, -area.y0);
        const Image piece = (part == box) ? cover : cover.crop(shifted(part, -box.x0, -box.y0));
        if (!ink) ink = Image::create("L", size_of(area), Ink(0));
        ink->paste(chops::screen(ink->crop(in_ink), piece), Point{in_ink.x0, in_ink.y0});
    }
    lay();
    if (!set.id.empty()) {
        auto entry = std::make_shared<CacheEntry>();
        entry->id = set.id;
        entry->dpi = ctx.dpi;
        entry->size = ctx.size;
        entry->guide = guide;
        entry->area = area;
        entry->patches = std::move(patch_sigs);
        entry->sigs = std::move(sigs);
        entry->image = out.copy();
        keep_strokes(std::move(entry));
    }
    return clipped(layer, std::move(out), panel_mask, raster);
}

std::optional<Image> layer_strokes(const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel_mask,
                                   const Image* raster, bool rough, bool with_page) {
    const auto& strokes = layer.strokes ? layer.strokes->items : core::empty_strokes()->items;
    if (strokes.empty() && layer.patches.empty()) return std::nullopt;
    if (with_page && panel_mask != nullptr && !strokes.empty() && layer.panel_each && layer.panel_clip) {
        // each line stays in the panel it was begun in
        std::vector<std::pair<std::string, std::vector<core::StrokePtr>>> groups;  // (key, lines), first seen first
        std::vector<std::pair<std::string, const core::Frame*>> frames;
        const std::string none = "\x01none";
        for (const core::StrokePtr& s : strokes) {
            const core::Frame* frame = s->points.empty() ? nullptr : panel_of(*ctx.page, s->points[0].x, s->points[0].y);
            const std::string key = frame != nullptr ? frame->id : none;
            if (frame != nullptr && std::none_of(frames.begin(), frames.end(), [&](const auto& f) { return f.first == key; })) {
                frames.emplace_back(key, frame);
            }
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == key; });
            if (it == groups.end()) {
                groups.emplace_back(key, std::vector<core::StrokePtr>{});
                it = groups.end() - 1;
            }
            it->second.push_back(s);
        }
        if (!(groups.size() == 1 && groups[0].first == none)) {
            std::optional<Image> out;
            std::vector<std::string> order{none};
            for (const auto& g : groups) {
                if (g.first != none) order.push_back(g.first);
            }
            for (const std::string& key : order) {
                StrokeSet part;
                const auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& item) { return item.first == key; });
                if (g != groups.end()) part.strokes = g->second;
                if (key == none) part.patches = layer.patches;
                if (part.strokes.empty() && part.patches.empty()) continue;
                part.id = layer.id.empty() ? std::string() : layer.id + "@" + (key == none ? std::string("None") : key);
                Image frame_area_mask;
                const Image* mask = panel_mask;
                if (key != none) {
                    const auto f = std::find_if(frames.begin(), frames.end(), [&](const auto& item) { return item.first == key; });
                    frame_area_mask = cached_frame_mask(*ctx.page, *f->second, ctx.size, ctx.dpi, area);
                    mask = &frame_area_mask;
                }
                auto drawn = draw_strokes(ctx, layer, part, area, mask, raster, rough);
                if (drawn) out = out ? alpha_composite(*out, *drawn) : std::move(*drawn);
            }
            return out ? std::move(*out) : transparent(area);
        }
    }
    StrokeSet all;
    all.id = layer.id;
    all.strokes = strokes;
    all.patches = layer.patches;
    return draw_strokes(ctx, layer, all, area, panel_mask, raster, rough);
}

// --- masks and colours of a layer --------------------------------------------------------------------------------------

// render._masked
Image masked(const Ctx& ctx, const Layer& layer, Image image, const Box& area) {
    if (!layer.mask || !layer.mask->png || layer.mask->png->empty() || !layer.mask->enabled) return image;
    const Image shown = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
    if (image.mode() != "RGBA") image = image.convert("RGBA");
    image.putalpha(chops::multiply(alpha_of(image), shown));
    return image;
}

// render._tinted
Image tinted(const Image& image, const std::vector<std::int64_t>& rgb) {
    Image out = Image::create("RGBA", image.size(), Ink::with_alpha(first3(rgb), 0));
    out.putalpha(image.mode() == "RGBA" ? alpha_of(image) : alpha_of(image.convert("RGBA")));
    return out;
}

bool is_tone(const Layer& layer) { return layer.role == LayerRole::Tone || layer.kind == LayerKind::Tone; }

// What tones::draw_layer needs from this drawing: the page and render._paint_patch.
tones::Page tone_page(const Ctx& ctx) {
    const int dpi = ctx.dpi;
    return tones::Page{ctx.page, dpi, ctx.size,
                       [dpi](Image& out, const Box& box, const core::Patch& patch) { paint_patch(out, box, patch, dpi, nullptr); }};
}

// render._screen_dots: the tones as dots on screen too (render.SCREEN_DOTS, at a size where a dot is a pixel or two).
bool screen_dots(const Ctx& ctx) { return ctx.screen_dots && ctx.mode != "print" && ctx.dpi >= 96 && ctx.dots; }

// Is the layer screened as this drawing draws it (トーン化: its greys as dots)?
bool screened_here(const Ctx& ctx, const Layer& layer) {
    return layer.screen && core::py_truthy(*layer.screen) && ((ctx.mode == "print" && ctx.dots) || screen_dots(ctx));
}

// color-model@1: is the layer's picture as this drawing makes it the page's values (an RGB page of a profile's paint
// pixels, with the fills and lines drawn on them brought into them)? Not where an effect or a screen is laid on it:
// those are drawn in sRGB, the pixels brought to sRGB first.
bool drawn_in_values(const Ctx& ctx, const Layer& layer) {
    return core::holds_page_values(*ctx.page, layer) && !(layer.effect && core::py_truthy(*layer.effect)) && !screened_here(ctx, layer);
}

// tones.screened for the layer's picture over `area` (トーン化). A noise screen runs from the top of the page: the rows
// above the area are drawn too.
Image screened_layer(const Ctx& ctx, const Layer& layer, const Image& raster, const Box& area, const Image* panel_mask);

// render._draw_effects: every visible effect of a known kind over the RGB picture of `area`.
Image draw_effects(const Ctx& ctx, const Image& image, const Box& area) {
    Image rgba = image.convert("RGBA");
    bool drawn = false;
    for (const Json& effect : core::iterate(ctx.page->effects)) {
        if (!effects::drawn(effect)) continue;
        rgba = effects::draw(std::move(rgba), effect, *ctx.page, ctx.dpi, ctx.size, area);
        drawn = true;
    }
    return drawn ? rgba.convert("RGB") : image;
}

// The layer's own picture and lines over `area` (render_page: _open_raster, raster.resize, _layer_strokes), or nothing.
std::optional<Image> layer_pixels(const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel_mask, bool rough) {
    std::optional<Image> raster;
    if (layer.kind == LayerKind::Placed) {
        try {
            raster = placed_raster(layer, *ctx.page, ctx.episode, ctx.size, ctx.dpi, ctx.mode, ctx.finish, ctx.dots, area);
        } catch (const NotYetPorted& e) {  // (a picture this build cannot open yet: left out as if it had none, and said)
            skip_unported(ctx, e.element());
        }
    } else if (layer.color_raster) {
        if (is_tone(layer)) throw NotYetPorted("high_precision_tone");
        raster = color_raster_preview(core::ColorRasterView(*layer.color_raster), ctx.size, area);
    } else if (layer.raster_png && !layer.raster_png->empty()) {
        raster = raster_part(layer.raster_png, ctx.size, area);
    } else if (layer.ink_raster && ctx.page->color) {  // (color-mode@1: its inks as the screen shows them)
        raster = ink_picture_rgba(sample_ink_raster(*decoded_ink(layer.ink_raster), ctx.size, area), *ctx.page->color);
    }
    // (color-model@1: on an RGB page of a profile a paint layer's pixels are the page's values; its lines and fills,
    // sRGB colours, go over them as the page's values too — or, where an effect or a screen is drawn on the layer in
    // sRGB, its pixels come to sRGB first)
    if (raster && core::holds_page_values(*ctx.page, layer) && !drawn_in_values(ctx, layer)) raster = srgb_from(*raster, RgbSpace(*ctx.page->rgb));
    auto lines = layer_strokes(ctx, layer, area, panel_mask, raster ? &*raster : nullptr, rough, true);
    if (lines && raster && drawn_in_values(ctx, layer)) lines = srgb_into(*lines, RgbSpace(*ctx.page->rgb));
    if (lines) raster = raster ? alpha_composite(*raster, *lines) : std::move(*lines);  // (raster is RGBA)
    return raster;
}

// The layer's picture over `area` after its effects (computed over a box widened by their reach).
std::optional<Image> layer_with_effects(const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel_mask,
                                        bool rough) {
    const int margin = effect_margin(layer, ctx.dpi);
    if (margin == 0) {
        auto raster = layer_pixels(ctx, layer, area, panel_mask, rough);
        if (raster) raster = layer_effects(layer, std::move(*raster), ctx.dpi);
        return raster;
    }
    const Box wide = intersection(Box{area.x0 - margin, area.y0 - margin, area.x1 + margin, area.y1 + margin},
                                  Box{0, 0, ctx.size.width, ctx.size.height});
    std::optional<Image> wide_mask;
    if (panel_mask != nullptr) wide_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, wide);
    auto raster = layer_pixels(ctx, layer, wide, wide_mask ? &*wide_mask : nullptr, rough);
    if (!raster) return std::nullopt;
    const Image done = layer_effects(layer, std::move(*raster), ctx.dpi);
    return done.crop(shifted(area, -wide.x0, -wide.y0));
}

Image screened_layer(const Ctx& ctx, const Layer& layer, const Image& raster, const Box& area, const Image* panel_mask) {
    const Json& spec = *layer.screen;
    const Json* pattern = get(spec, "pattern");
    const Box band{0, 0, ctx.size.width, area.y1};
    if (pattern != nullptr && core::py_truthy(*pattern) && core::py_str(*pattern) == "noise" && area != band) {
        std::optional<Image> band_mask;
        if (panel_mask != nullptr) band_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, band);
        const auto full = layer_with_effects(ctx, layer, band, band_mask ? &*band_mask : nullptr, ctx.rough);
        return tones::screened(*full, spec, ctx.dpi, ctx.size, band).crop(area);
    }
    return tones::screened(raster, spec, ctx.dpi, ctx.size, area);
}

// --- what else a page can carry ---------------------------------------------------------------------------------------

bool truthy_json(const std::optional<Json>& v) { return v && core::py_truthy(*v); }

// nombre.placements(episode, page) is not empty
Json nombre_settings(const core::Document* episode) {
    Json cfg = Json::object({{"show", true}, {"hidden", false}, {"position", "bottom_center"},
                             {"font", "gothic"}, {"size_mm", 3.0}, {"hidden_size_mm", 2.0}, {"start", 1}});
    if (episode != nullptr && episode->nombre.is_object()) for (const auto& [k, v] : episode->nombre.items()) cfg[k] = v;
    return cfg;
}

std::string nombre_text(const Page& page, const core::Document* episode) {
    std::int64_t before = 0;
    if (episode != nullptr) for (const auto& p : episode->pages) if (p->index < page.index && core::is_cover(*p)) ++before;
    return (Num(core::py_int(nombre_settings(episode)["start"])) - Num(1) + page.index - Num(before)).repr();
}

std::vector<std::array<double, 3>> nombre_items(const Page& page, const core::Document* episode) {
    if (!page.numero) return {};
    const Json cfg = nombre_settings(episode);
    if (!core::py_truthy(cfg["show"]) && !core::py_truthy(cfg["hidden"])) return {};
    const std::string start_side = episode != nullptr ? episode->start_side.value_or("") : "";
    const core::Rect trim = page.trim_rect_mm(), inner = page.inner_rect_mm(start_side);
    const double x0 = trim.x.value(), y0 = trim.y.value(), x1 = x0 + trim.width.value(), y1 = y0 + trim.height.value();
    const bool outer_right = page.binding_edge(start_side) == "left";
    std::vector<std::array<double, 3>> out;
    const auto item = [&](double x, double y, double size) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(size) || size < 0 || size > 20)
            throw core::Error("value", "invalid nombre geometry");
        out.push_back({core::py_round(x, 3), core::py_round(y, 3), size});
    };
    if (core::py_truthy(cfg["show"])) {
        const double size = core::py_float(cfg["size_mm"]);
        const std::string position = core::py_str(cfg["position"]);
        const double below = (inner.y.value() + inner.height.value() + y1) / 2;
        const double near = outer_right ? inner.x.value() + inner.width.value() - size : inner.x.value() + size;
        if (position == "bottom_outside") item(near, below, size);
        else if (position == "top_outside") item(near, (y0 + inner.y.value()) / 2, size);
        else if (position == "side_outside") item(outer_right ? (inner.x.value() + inner.width.value() + x1) / 2 : (x0 + inner.x.value()) / 2,
                                                    inner.y.value() + inner.height.value() / 2, size);
        else item((x0 + x1) / 2, below, size);
    }
    if (core::py_truthy(cfg["hidden"])) {
        const double small = core::py_float(cfg["hidden_size_mm"]);
        item(outer_right ? x0 + small : x1 - small, y1 - 15, small);
    }
    return out;
}

std::string nombre_font(const core::Document* episode) {
    const Json cfg = nombre_settings(episode);
    return core::py_truthy(cfg["font"]) ? core::py_str(cfg["font"]) : "gothic";
}

std::pair<PointD, Box> nombre_geometry(const std::string& text, const std::string& font, int size,
                                     const std::array<double, 3>& item, const Ctx& ctx) {
    auto measured = text_mask(text, font, size, {}, ctx.stop);
    const auto& box = measured.second;
    const double x = mm_to_px(item[0], ctx.dpi) - (box[2] - box[0]) / 2.0 - box[0];
    const double y = mm_to_px(item[1], ctx.dpi) - (box[3] - box[1]) / 2.0 - box[1];
    const int pad = std::max(2, size / 5);
    return {{x, y}, {static_cast<int>(x + box[0]) - pad, static_cast<int>(y + box[1]) - pad,
                     static_cast<int>(x + box[2]) + pad, static_cast<int>(y + box[3]) + pad}};
}

Box nombre_required_area(const Box& area, const Ctx& ctx) {
    const auto items = nombre_items(*ctx.page, ctx.episode);
    if (items.empty()) return area;
    const auto text = nombre_text(*ctx.page, ctx.episode), font = nombre_font(ctx.episode);
    Box out = area;
    for (const auto& item : items) {
        const int size = std::max(6, mm_to_px(item[2], ctx.dpi));
        const Box sample = nombre_geometry(text, font, size, item, ctx).second;
        if (!intersects(sample, area)) continue;
        out.x0 = std::min(out.x0, std::max(0, sample.x0)); out.y0 = std::min(out.y0, std::max(0, sample.y0));
        out.x1 = std::max(out.x1, std::min(ctx.size.width, sample.x1)); out.y1 = std::max(out.y1, std::min(ctx.size.height, sample.y1));
    }
    return out;
}

// (`under`: the picture whose colours decide the halo, when it is not `image` itself: a CMYK or Lab page draws its
// letters on black and on white, and decides by the page as shown)
void draw_nombre(Image& image, const Box& area, const Ctx& ctx, const Image* under = nullptr) {
    const auto items = nombre_items(*ctx.page, ctx.episode);
    if (items.empty()) return;
    const auto text = nombre_text(*ctx.page, ctx.episode), font = nombre_font(ctx.episode);
    for (const auto& item : items) {
        check_cancel(ctx);
        const int size = std::max(6, mm_to_px(item[2], ctx.dpi));
        const auto [origin, sample] = nombre_geometry(text, font, size, item, ctx);
        if (!intersects(sample, area)) continue;
        const Image background = (under != nullptr ? *under : image).crop(shifted(sample, -area.x0, -area.y0)).convert("L");
        const bool busy = background.width() > 0 && background.height() > 0 && background.getextrema().front().first < 235;
        // Pillow ImageDraw keeps the fractional part separately, not as a subpixel FT transform.
        const PointD fraction{origin.x - std::floor(origin.x), origin.y - std::floor(origin.y)};
        const auto paint = [&](int stroke, const Ink& color) {
            auto glyph = text_mask(text, font, size, fraction, ctx.stop, stroke);
            const int left = static_cast<int>(std::floor(origin.x)) + glyph.second[0] - area.x0;
            const int top = static_cast<int>(std::floor(origin.y)) + glyph.second[1] - area.y0;
            image.paste(color, Box{left, top, left + glyph.first.width(), top + glyph.first.height()}, &glyph.first);
        };
        if (busy) paint(std::max(1, static_cast<int>(core::py_round_int(size * 0.14))), Ink{255, 255, 255});
        paint(0, Ink{20, 20, 20});
    }
}

// The story lines of this page (Episode.story_for_page; a page alone has none here: C++ keeps lines in the book).
std::vector<const core::StoryLine*> lines_of(const Page& page, const core::Document* episode) {
    if (episode == nullptr) return {};
    return episode->story_for_page(page.index);
}

// `line.x_mm or line.y_mm or line.balloon`: a line placed on the page (drawn in its balloon, not as a label).
bool placed_line(const core::StoryLine& line) { return line.x_mm.truthy() || line.y_mm.truthy() || !line.balloon.empty(); }

// {f.id: (f.rect.x, f.rect.y, f.rect.width, f.rect.height) for f in page.leaf_frames()}
text::Panels panels_of(const Page& page) {
    text::Panels out;
    for (const core::Frame* f : page.leaf_frames()) {
        out.emplace_back(f->id, std::array<double, 4>{f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value()});
    }
    return out;
}

// render._font(font_path) at its size 14: the book's font file when it opens, else the first of _CJK_FONTS, the
// bundled Dela Gothic (which always opens).
const TrueTypeFont& label_font(TrueTypeFonts& fonts, const std::optional<std::string>& font_path) {
    if (font_path && !font_path->empty() && QFileInfo(QString::fromStdString(*font_path)).isFile()) {
        try {
            return fonts.truetype(*font_path, 14);
        } catch (const NotYetPorted& e) {  // (Python's OSError: not a font it reads)
            if (e.element() != "default_font") throw;
        }
    }
    return fonts.truetype("sfx", 14);
}

// The lines of a page drawn over everything (render_page after the panels' borders): the placed ones in balloons
// (balloons.draw_lines, speaker names shown but in print), the others as labels at the inner frame's corner.
void draw_story(Image& image, const Box& area, const Ctx& ctx, const std::vector<const core::StoryLine*>& lines,
                const std::vector<const core::StoryLine*>& drawn_below, const text::Panels& panels,
                const std::optional<std::string>& font_path, const text::Unported& unported) {
    const auto below = [&](const core::StoryLine* line) {
        return std::find(drawn_below.begin(), drawn_below.end(), line) != drawn_below.end();
    };
    std::vector<const core::StoryLine*> placed;
    for (const core::StoryLine* line : lines) {
        if (placed_line(*line) && !below(line)) placed.push_back(line);
    }
    // Speaker names are a working aid: shown in name/proof, never printed.
    text::draw_lines(text::PagePart{&image, Point{area.x0, area.y0}, ctx.size}, placed, ctx.dpi, font_path, ctx.mode != "print",
                     &panels, ctx.stop, &unported);
    std::optional<TrueTypeFonts> fonts;
    for (const core::StoryLine* line : lines) {
        if (placed_line(*line) || below(line)) continue;
        check_cancel(ctx);
        if (!fonts) fonts.emplace(ctx.stop);
        const TrueTypeFont* found = nullptr;
        try {
            found = &label_font(*fonts, font_path);
        } catch (const NotYetPorted& e) {  // (the book's font past what this build reads: this label left out, said)
            skip_unported(ctx, e.element());
            continue;
        }
        const TrueTypeFont& font = *found;
        const core::Rect inner = ctx.page->inner_rect_mm();
        const int x = mm_to_px(inner.x.value() + 4, ctx.dpi);
        const int y = mm_to_px(inner.y.value() + 4, ctx.dpi);
        const std::string label = line->speaker.empty() ? line->text : line->speaker + ": " + line->text;
        Draw draw(image);  // (whole pixels: the part's corner moves them exactly)
        draw.text(PointD{static_cast<double>(x - area.x0), static_cast<double>(y - area.y0)}, text::u32(label), font, Ink{10, 10, 10});
    }
}

void draw_ruler(Image& image, const Box& area, const Ctx& ctx) {
    const Json& ruler = *ctx.page->ruler;
    if (!ruler.is_object()) throw core::Error("value", "'" + core::py_type_name(ruler) + "' object has no attribute 'get'");
    const Json* points = get(ruler, "points");
    if (points == nullptr || !core::py_truthy(*points)) return;
    PageCanvas canvas(image, area, ctx.size);
    Draw& draw = canvas.draw();
    for (const Json& pt : core::py_list(*points)) {
        const Json p = core::py_list(pt);
        const auto [px, py] = xy(core::py_float(p.at(0)), core::py_float(p.at(1)), ctx.dpi);
        draw.ellipse(BoxF{px - 3.0, py - 3.0, px + 3.0, py + 3.0}, std::nullopt, Ink{180, 80, 80}, 2);
        const std::vector<PointD> v{{static_cast<double>(px), 0.0}, {static_cast<double>(px), static_cast<double>(ctx.size.height)}};
        draw.line(v, Ink{220, 180, 180}, 1);
        const std::vector<PointD> hz{{0.0, static_cast<double>(py)}, {static_cast<double>(ctx.size.width), static_cast<double>(py)}};
        draw.line(hz, Ink{220, 180, 180}, 1);
    }
    canvas.commit();
}

Box area_of(const RenderOptions& options, Size size) {
    if (!options.region) return Box{0, 0, size.width, size.height};
    const RenderRegion& r = *options.region;
    if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x > size.width - r.w || r.y > size.height - r.h) {
        throw core::Error("value", "the region is not inside the page");
    }
    return Box{r.x, r.y, r.x + r.w, r.y + r.h};
}

void precise_strokes(ColorCanvas& target, const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel) {
    if (layer.mask || layer.effect || layer.screen || layer.color || layer.panel_each || !layer.patches.empty() || layer.raster_png)
        throw NotYetPorted("precise stroke layer style");
    ColorCanvas own(transparent(area), target.space());
    if (ctx.episode != nullptr) paper::keep_all(ctx.episode->papers);  // (BRUSH-01: its brushes' papers)
    for (const auto& stroke : layer.strokes->items) {
        check_cancel(ctx);
        const auto brush=brushes::brush(stroke->kind);
        const auto reach=brushes::extent(ctx.size,core::stroke_points(*stroke),ctx.dpi,stroke_width(*stroke),stroke->kind);
        if (!reach || !intersects(*reach,area)) continue;
        auto drawn=brushes::draw(ctx.size,core::stroke_points(*stroke),ctx.dpi,stroke_width(*stroke),stroke->kind,stroke->id,stroke->rotation,stroke->pressure_opacity);
        if (!drawn) continue;
        const Box box{drawn->origin.x,drawn->origin.y,drawn->origin.x+drawn->mask.width(),drawn->origin.y+drawn->mask.height()};
        const auto part=intersection(box,area);
        if (part.width()<=0 || part.height()<=0) continue;
        Image mask=Image::create("L",size_of(area),Ink(0));
        mask.paste(drawn->mask.crop(shifted(part,-box.x0,-box.y0)),Point{part.x0-area.x0,part.y0-area.y0});
        if (panel && layer.panel_clip) mask=chops::multiply(mask,*panel);
        Json color;
        if (stroke->color_rgb && !guide_role(layer.role)) color=*stroke->color_rgb;
        else {
            const auto rgb=guide_role(layer.role)?kNameColor:stroke_rgb(*stroke,brush);
            color=Json{{"precision","f32"},{"values",Json::array({rgb[0]/255.,rgb[1]/255.,rgb[2]/255.})}};
        }
        own.blend_stroke(mask,color,core::py_clamp(stroke->opacity,0.,1.)*brush.opacity);
    }
    target.blend(own,layer.opacity,layer.clip,layer.blend);
}

// --- PC2d (document:color-depth-model, design 4.3): what a page in precise colour draws as the 8-bit page does --------

// A part of a layer's precise picture (`box` inside it).
ColorPicture crop_color_picture(const ColorPicture& p, const Box& box) {
    ColorPicture out{Size{box.width(), box.height()}, {}};
    out.pixels.reserve(std::size_t(box.width()) * box.height());
    for (int y = box.y0; y < box.y1; ++y) {
        const auto row = p.pixels.begin() + std::ptrdiff_t(std::size_t(y) * p.size.width + box.x0);
        out.pixels.insert(out.pixels.end(), row, row + box.width());
    }
    return out;
}

// Does the layer with precise colour of its own draw as it did before PC2d — its precise pixels alone (their mask,
// opacity and blend), or its precise lines alone (precise_strokes)? Else it is drawn as one picture (precise_layer).
bool plain_precise(const Layer& layer) {
    if (core::has_color_strokes(layer)) {
        return layer.kind == LayerKind::Strokes && !layer.color_raster && !layer.mask && !layer.effect && !layer.screen && !layer.color &&
               !layer.panel_each && layer.patches.empty() && !layer.raster_png;
    }
    return !layer.effect && !layer.screen && !layer.color && layer.patches.empty() && layer.stroke_count() == 0;
}

// The lines and pictures laid on a layer with precise lines, over `box`, in the canvas's space: as draw_strokes makes an
// 8-bit layer's — the pictures, then the lines, each panel's in its turn when they keep to the panel they begin in —,
// each line in its own colour as precise_strokes lays it; inside the panels (はみ出し off) and, locked, inside
// `kept`'s alpha (its pixels).
ColorPicture precise_lines(const Ctx& ctx, const Layer& layer, const Box& box, const Image* panel, const Image* kept, const RgbSpace& space) {
    ColorCanvas own(transparent(box), space);
    const bool guide = guide_role(layer.role);
    if (ctx.episode != nullptr) paper::keep_all(ctx.episode->papers);  // (BRUSH-01: its brushes' papers)
    if (!layer.patches.empty()) {
        Image pictures = transparent(box);
        for (const core::Patch& patch : layer.patches) paint_patch(pictures, box, patch, ctx.dpi, guide ? &kNameColor : nullptr);
        own.blend(clipped(layer, std::move(pictures), panel, kept), 1, false);
    }
    // (each line's panel: the panel it begins in when they keep to it, render._each_panel; the others the page's)
    const bool each = panel != nullptr && layer.panel_each && layer.panel_clip;
    std::vector<std::pair<std::string, const core::Frame*>> order{{std::string(), nullptr}};
    std::vector<std::size_t> group(layer.strokes->items.size(), 0);
    for (std::size_t k = 0; k < layer.strokes->items.size(); ++k) {
        const core::Stroke& s = *layer.strokes->items[k];
        const core::Frame* frame = each && !s.points.empty() ? panel_of(*ctx.page, s.points[0].x, s.points[0].y) : nullptr;
        if (frame == nullptr) continue;
        auto it = std::find_if(order.begin(), order.end(), [&](const auto& o) { return o.second == frame; });
        if (it == order.end()) it = order.insert(order.end(), std::pair<std::string, const core::Frame*>{frame->id, frame});
        group[k] = static_cast<std::size_t>(it - order.begin());
    }
    const std::optional<Image> lock = layer.lock_alpha && kept != nullptr ? std::optional<Image>(alpha_of(*kept)) : std::nullopt;
    for (std::size_t g = 0; g < order.size(); ++g) {
        std::optional<Image> frame_mask;
        if (order[g].second != nullptr) frame_mask = cached_frame_mask(*ctx.page, *order[g].second, ctx.size, ctx.dpi, box);
        const Image* inside = frame_mask ? &*frame_mask : layer.panel_clip ? panel : nullptr;
        for (std::size_t k = 0; k < layer.strokes->items.size(); ++k) {
            if (group[k] != g) continue;
            check_cancel(ctx);
            const core::Stroke& stroke = *layer.strokes->items[k];
            const auto brush = brushes::brush(stroke.kind);
            const auto reach = brushes::extent(ctx.size, core::stroke_points(stroke), ctx.dpi, stroke_width(stroke), stroke.kind);
            if (!reach || !intersects(*reach, box)) continue;
            auto drawn = brushes::draw(ctx.size, core::stroke_points(stroke), ctx.dpi, stroke_width(stroke), stroke.kind, stroke.id, stroke.rotation,
                                       stroke.pressure_opacity);
            if (!drawn) continue;
            const Box at{drawn->origin.x, drawn->origin.y, drawn->origin.x + drawn->mask.width(), drawn->origin.y + drawn->mask.height()};
            const Box part = intersection(at, box);
            if (part.width() <= 0 || part.height() <= 0) continue;
            Image mask = Image::create("L", size_of(box), Ink(0));
            mask.paste(drawn->mask.crop(shifted(part, -at.x0, -at.y0)), Point{part.x0 - box.x0, part.y0 - box.y0});
            if (inside != nullptr) mask = chops::multiply(mask, *inside);
            if (lock) mask = chops::multiply(mask, *lock);
            Json color;
            if (stroke.color_rgb && !guide) {
                color = *stroke.color_rgb;
            } else {
                const auto rgb = guide ? kNameColor : stroke_rgb(stroke, brush);
                color = Json{{"precision", "f32"}, {"values", Json::array({rgb[0] / 255., rgb[1] / 255., rgb[2] / 255.})}};
            }
            own.blend_stroke(mask, color, core::py_clamp(stroke.opacity, 0., 1.) * brush.opacity);
        }
    }
    return own.picture();
}

// A layer's own picture over `box` before its effects, in the canvas's space (layer_pixels for a layer with precise
// colour): its pixels — precise, or a v2 line layer's 8-bit raster —, the pictures and lines laid on them.
ColorPicture precise_pixels(const Ctx& ctx, const Layer& layer, const Box& box, const Image* panel, const RgbSpace& space) {
    ColorPicture picture{size_of(box), std::vector<std::array<double, 4>>(std::size_t(box.width()) * box.height(), {0, 0, 0, 0})};
    bool pixels = true;
    if (layer.color_raster) {
        picture = color_picture(core::ColorRasterView(*layer.color_raster), ctx.size, box, space);
    } else if (layer.raster_png && !layer.raster_png->empty() && layer.kind != LayerKind::Placed) {
        picture = color_picture(raster_part(layer.raster_png, ctx.size, box), space, core::holds_page_values(*ctx.page, layer));
    } else {
        pixels = false;
    }
    if (layer.stroke_count() == 0 && layer.patches.empty()) return picture;
    // (what a locked layer's lines are kept inside: its pixels' alpha, when it has pixels)
    Image kept = Image::create("RGBA", size_of(box), Ink{0, 0, 0, 0});
    if (pixels) kept.putalpha(picture_alpha(picture));
    const Image* inside = pixels ? &kept : nullptr;
    if (core::has_color_strokes(layer)) {
        picture_over(picture, precise_lines(ctx, layer, box, panel, inside, space));
    } else if (auto lines = layer_strokes(ctx, layer, box, panel, inside, ctx.rough, true)) {
        picture_over(picture, color_picture(*lines, space));  // (their sRGB colours brought into the page's space)
    }
    return picture;
}

// ... and after its effects (computed over a box widened by their reach, as layer_with_effects does).
ColorPicture precise_with_effects(const Ctx& ctx, const Layer& layer, const Box& box, const Image* panel, const RgbSpace& space) {
    const int margin = effect_margin(layer, ctx.dpi);
    if (margin == 0) {
        ColorPicture picture = precise_pixels(ctx, layer, box, panel, space);
        color_layer_effects(layer, picture, ctx.dpi, space);
        return picture;
    }
    const Box wide = intersection(Box{box.x0 - margin, box.y0 - margin, box.x1 + margin, box.y1 + margin}, Box{0, 0, ctx.size.width, ctx.size.height});
    std::optional<Image> wide_mask;
    if (panel != nullptr) wide_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, wide);
    ColorPicture picture = precise_pixels(ctx, layer, wide, wide_mask ? &*wide_mask : nullptr, space);
    color_layer_effects(layer, picture, ctx.dpi, space);
    return crop_color_picture(picture, shifted(box, -wide.x0, -wide.y0));
}

// A tone layer on a page in precise colour: drawn in 8 bits over the picture so far as on the 8-bit page (as a CMYK or
// Lab page draws it: its dots are 8-bit), and a layer above clipped to it clips to nothing, as there.
void precise_tone(ColorCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel) {
    const bool dots = (ctx.mode == "print" && ctx.finish && ctx.dots) || screen_dots(ctx);
    canvas.draw_8bit([&](Image& picture) { picture = tones::draw_layer(std::move(picture), layer, tone_page(ctx), area, panel, dots); });
    canvas.forget_clip();
}

// A layer with precise colour of its own (precise pixels, precise lines) on a page in precise colour, with what the
// 8-bit page draws on a layer (render_page): its lines and the pictures laid on it over its pixels, its effects (the
// user's decision 6 of PC2: color_layer_effects), its screen (トーン化: dots, in 8 bits as they print), its colour (each
// pixel in the page's precision), its mask. A tone layer as the 8-bit page draws one.
void precise_layer(ColorCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel) {
    if (is_tone(layer)) {
        precise_tone(canvas, ctx, layer, area, panel);
        return;
    }
    if (plain_precise(layer)) {
        if (core::has_color_strokes(layer)) {
            precise_strokes(canvas, ctx, layer, area, panel);
            return;
        }
        std::optional<Image> shown;  // (its mask, as masked() takes it)
        if (layer.mask && layer.mask->png && !layer.mask->png->empty() && layer.mask->enabled)
            shown = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
        canvas.blend(core::ColorRasterView(*layer.color_raster), ctx.size, area, layer.opacity, layer.clip, layer.blend, shown ? &*shown : nullptr);
        return;
    }
    const RgbSpace& space = canvas.space();
    const bool print = ctx.mode == "print";
    const bool tint = layer.color && !layer.color->empty() && (!print || layer.color_prints);
    if (screened_here(ctx, layer)) {
        // トーン化: the greys of the picture as the screen shows it, as dots in 8 bits as on any page; a noise screen runs
        // from the top of the page (screened_layer): the band above the area made the same way
        const Json& spec = *layer.screen;
        const Json* pattern = get(spec, "pattern");
        const Box band{0, 0, ctx.size.width, area.y1};
        Image raster;
        if (pattern != nullptr && core::py_truthy(*pattern) && core::py_str(*pattern) == "noise" && area != band) {
            std::optional<Image> band_mask;
            if (panel != nullptr) band_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, band);
            raster = tones::screened(picture_rgba(precise_with_effects(ctx, layer, band, band_mask ? &*band_mask : nullptr, space), space), spec, ctx.dpi,
                                     ctx.size, band)
                         .crop(area);
        } else {
            raster = tones::screened(picture_rgba(precise_with_effects(ctx, layer, area, panel, space), space), spec, ctx.dpi, ctx.size, area);
        }
        Image dots = masked(ctx, layer, std::move(raster), area);
        if (tint) dots = tinted(dots, *layer.color);
        canvas.blend(dots, layer.opacity, layer.clip, layer.blend);
        return;
    }
    ColorPicture picture = precise_with_effects(ctx, layer, area, panel, space);
    if (tint) {  // (shown in its colour, each pixel's alpha kept)
        const auto rgb = first3(*layer.color);
        const auto at = [&](std::size_t k) { return static_cast<int>(std::clamp<std::int64_t>(rgb.size() > k ? rgb[k] : 0, 0, 255)); };
        const auto colour = linear_colour(space, {at(0), at(1), at(2)});
        for (auto& p : picture.pixels) p = {colour[0], colour[1], colour[2], p[3]};
    }
    std::optional<Image> shown;
    if (layer.mask && layer.mask->png && !layer.mask->png->empty() && layer.mask->enabled)
        shown = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
    canvas.blend(picture, layer.opacity, layer.clip, layer.blend, shown ? &*shown : nullptr);
}

// A correction layer whose filter moves shapes too (core::kShapeFilters: an old book's may hold one).
bool shape_correction(const Layer& layer) {
    if (layer.kind != LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object()) return false;
    const auto kind = layer.adjust->find("kind");
    return kind != layer.adjust->end() && kind->is_string() &&
           std::find(core::kShapeFilters.begin(), core::kShapeFilters.end(), kind->get<std::string>()) != core::kShapeFilters.end();
}

// A page of a drawing's size at the working 200 dpi: render/color_filters and ink_filters scale their sizes in pixels
// from 200 dpi to a raster's resolution on its page; on this one they stay as given — the drawing's own pixels, as the
// 8-bit page's correction layer takes them (filters.apply_filter on its picture).
Page working_grid(const Page& page, Size size) {
    Page grid;
    grid.spec = page.spec;
    grid.spec.width_mm = Num(size.width * 25.4 / raster::kWorkingDpi);
    grid.spec.height_mm = Num(size.height * 25.4 / raster::kWorkingDpi);
    grid.color = page.color;
    grid.rgb = page.rgb;
    return grid;
}

// What the 8-bit page gives filters.apply_filter of a correction layer: its settings without its kind and native.
Json shape_params(const Layer& layer) {
    Json params = *layer.adjust;
    params.erase("kind");
    params.erase("native");
    return params;
}

// A correction that moves shapes on a page in precise colour (the whole page: needs_whole_page): the page's values so
// far through render/color_filters (in their precision; the sizes in the drawing's pixels), laid over them as the 8-bit
// page lays its result — by opacity × mask × the clipped-to layer's alpha, its native.blend first. Settings the 8-bit
// page refuses (a ValueError there): the layer does nothing.
void precise_shape_correction(ColorCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area) {
    const std::string kind = layer.adjust->value("kind", Json()).get<std::string>();
    const std::string blend = core::native_of(*layer.adjust).blend;
    std::string changed;
    try {
        Layer drawn;
        drawn.kind = LayerKind::Raster;
        drawn.color_raster = std::make_shared<const std::string>(canvas.color_raster("f32"));
        changed = color_filters::apply(working_grid(*ctx.page, canvas.size()), drawn, kind, shape_params(layer), nullptr);
    } catch (const core::OpError&) {
        return;
    } catch (const core::PyValueError&) {
        return;
    }
    std::optional<Image> mask;  // (its mask as render._adjusted resizes it)
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
        mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bicubic);
    canvas.correct(core::ColorRasterView(changed), layer.opacity, layer.clip, mask ? &*mask : nullptr, blend);
}

// ... and on a CMYK, Lab or grey page: its inks so far (at the page's depth, with their alpha) through
// render/ink_filters, laid back over them the same way. A native.blend the page does not composite: left out as a
// layer's is.
void ink_shape_correction(InkCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area) {
    const std::string kind = layer.adjust->value("kind", Json()).get<std::string>();
    const std::string blend = core::native_of(*layer.adjust).blend;
    if (!canvas.supports_blend(blend)) {
        skip_unported(ctx, "ink_blend:" + blend);
        return;
    }
    const core::PageColor& color = *ctx.page->color;
    std::string changed;
    try {
        changed = ink_filters::apply(working_grid(*ctx.page, canvas.size()), canvas.ink_pixels(color.depth, true), kind, shape_params(layer), nullptr);
    } catch (const core::OpError&) {
        return;
    } catch (const core::PyValueError&) {
        return;
    }
    std::optional<Image> mask;
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
        mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bicubic);
    const InkPicture picture = sample_ink_raster(core::decode_ink_raster(changed), canvas.size(), Box{0, 0, canvas.size().width, canvas.size().height});
    canvas.correct(picture, layer.opacity, layer.clip, mask ? &*mask : nullptr, blend);
}

// A part of an ink picture (`box` inside it).
InkPicture crop_picture(const InkPicture& p, const Box& box) {
    const unsigned all = p.channels + 1;
    InkPicture out{Size{box.width(), box.height()}, p.channels, std::vector<double>(std::size_t(box.width()) * box.height() * all, 0.0)};
    for (int y = 0; y < box.height(); ++y) {
        const double* from = p.values.data() + (std::size_t(box.y0 + y) * p.size.width + box.x0) * all;
        std::copy_n(from, std::size_t(box.width()) * all, out.values.data() + std::size_t(y) * box.width() * all);
    }
    return out;
}

// An ink picture as the 8-bit RGBA of the colours its inks mean (render/ink meaning_of: a line on K alone grey).
Image meant_rgba(const InkPicture& p, const core::PageColor& color) {
    const unsigned n = p.channels, all = n + 1;
    const std::size_t count = static_cast<std::size_t>(p.size.width) * p.size.height;
    std::vector<double> inks(count * n), rgb(count * 3);
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned c = 0; c < n; ++c) inks[i * n + c] = from_canvas(color.space, c, p.values[i * all + c]);
    ink::meaning_of(color, inks.data(), rgb.data(), count);
    std::string bytes(count * 4, '\0');
    for (std::size_t i = 0; i < count; ++i) {
        for (unsigned c = 0; c < 3; ++c) bytes[i * 4 + c] = static_cast<char>(std::lround(std::clamp(rgb[i * 3 + c], 0.0, 1.0) * 255.0));
        bytes[i * 4 + 3] = static_cast<char>(std::lround(std::clamp(p.values[i * all + n], 0.0, 1.0) * 255.0));
    }
    return Image::frombytes("RGBA", p.size, bytes);
}

// color-mode@1: a correction layer on a CMYK or Lab page (SPEC.md COLOR-02, 群C): the colours the inks mean through its
// function and back by the page's rules, or (PC2) its records laid on the plates directly. Its own blend is not looked
// at, as Python does not; its native.blend is laid by the page's blend formula (the user's decision 2 of PC2) — one the
// page does not composite left out as a layer's is.
void ink_correction(InkCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area) {
    const Json kind = layer.adjust && layer.adjust->is_object() ? layer.adjust->value("kind", Json()) : Json();
    if (kind.is_string()) {
        const std::string name = kind.get<std::string>();
        if (name.starts_with("plugin:")) {  // (a person's filter plugin runs in the external runner)
            skip_unported(ctx, "adjust:plugin");
            return;
        }
        if (std::find(core::kShapeFilters.begin(), core::kShapeFilters.end(), name) != core::kShapeFilters.end()) {
            skip_unported(ctx, "adjust:" + name);  // (one that moves shapes too: an old book's, as on precise pixels)
            return;
        }
    }
    const std::string blend = layer.adjust ? core::native_of(*layer.adjust).blend : std::string("normal");
    if (!canvas.supports_blend(blend)) {
        skip_unported(ctx, "ink_blend:" + blend);
        return;
    }
    const bool exposure = kind == Json("exposure");
    std::optional<Image> mask;  // (its mask as the precise page resizes it)
    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
        mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, exposure ? Resample::Bilinear : Resample::Bicubic);
    if (exposure) (void)core::Exposure::parse(*layer.adjust);  // (settings it refuses: refused, as they were)
    core::AdjustPtr correction;
    try {
        correction = correction_of(layer, *ctx.page, ctx.episode);
    } catch (const NotYetPorted& error) {
        skip_unported(ctx, error.element());
        return;
    }
    if (!correction) return;
    if (const auto records = correction->ink_records(); records && records->space == ctx.page->color->space) {
        canvas.adjust_planes(*records, layer.opacity, layer.clip, mask ? &*mask : nullptr, blend);
        return;
    }
    canvas.recolour(*correction, layer.opacity, layer.clip, mask ? &*mask : nullptr, blend, Point{area.x0, area.y0});
}

// color-mode@1: one layer of a CMYK or Lab page into the page's canvas (SPEC.md COLOR-02). Its own inks (an ink raster,
// resampled as a paint layer is) are composited as they are; everything drawn in 8 bits (lines, fills, placed
// pictures, a raster in RGB) as the page's inks of its colours; tones as drawn over the page.
void ink_layer(InkCanvas& canvas, const Ctx& ctx, const Layer& layer, const Box& area, const Image* panel) {
    const bool print = ctx.mode == "print";
    if (is_tone(layer)) {  // (tones sit in the layer order as drawn over the page; a layer above clips to nothing)
        const bool dots = (print && ctx.finish && ctx.dots) || screen_dots(ctx);
        canvas.draw_8bit([&](Image& picture) {
            picture = tones::draw_layer(picture.convert("RGBA"), layer, tone_page(ctx), area, panel, dots).convert("RGB");
        });
        canvas.forget_clip();
        return;
    }
    if (layer.kind == LayerKind::Adjust) {  // a correction of the colours the inks mean (SPEC.md COLOR-02, 群C)
        ink_correction(canvas, ctx, layer, area);
        return;
    }
    if (!canvas.supports_blend(layer.blend)) {
        skip_unported(ctx, "ink_blend:" + layer.blend);
        return;
    }
    if (layer.ink_raster) {
        const core::PageColor& color = *ctx.page->color;
        // its picture over a box (its pixels, its lines over them inside the box's panels), as layer_with_effects takes
        // an 8-bit one: wider by what its effects reach, then cut to the box
        const auto part = [&](const Box& box, const Image* box_panel) {
            const int margin = effect_margin(layer, ctx.dpi);
            const Box wide = margin == 0 ? box : intersection(Box{box.x0 - margin, box.y0 - margin, box.x1 + margin, box.y1 + margin},
                                                              Box{0, 0, ctx.size.width, ctx.size.height});
            std::optional<Image> wide_mask;
            if (margin != 0 && box_panel != nullptr) wide_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, wide);
            InkPicture picture = sample_ink_raster(*decoded_ink(layer.ink_raster), ctx.size, wide);
            if (layer.stroke_count() > 0 || !layer.patches.empty()) {
                const Image kept = ink_alpha_rgba(picture);  // (透明保護: lines inside its pixels)
                if (auto lines = layer_strokes(ctx, layer, wide, margin == 0 ? box_panel : (wide_mask ? &*wide_mask : nullptr), &kept, ctx.rough, true)) {
                    ink_over(picture, ink_picture(*lines, color));
                }
            }
            ink_layer_effects(layer, picture, ctx.dpi, color);
            if (margin != 0) picture = crop_picture(picture, shifted(box, -wide.x0, -wide.y0));
            return picture;
        };
        InkPicture picture = part(area, panel);
        const bool tinted_here = layer.color && !layer.color->empty() && (!print || layer.color_prints);
        if (truthy_json(layer.screen) && ((print && ctx.dots) || screen_dots(ctx))) {
            // トーン化: the greys its inks mean as dots, in 8 bits as on any page (K by the rules); a noise screen runs
            // from the top of the page (screened_layer): the band above the area made the same way
            const Json& spec = *layer.screen;
            const Json* pattern = get(spec, "pattern");
            const Box band{0, 0, ctx.size.width, area.y1};
            Image raster;
            if (pattern != nullptr && core::py_truthy(*pattern) && core::py_str(*pattern) == "noise" && area != band) {
                std::optional<Image> band_mask;
                if (panel != nullptr) band_mask = clip_mask(*ctx.page, ctx.size, ctx.dpi, band);
                raster = tones::screened(meant_rgba(part(band, band_mask ? &*band_mask : nullptr), color), spec, ctx.dpi, ctx.size, band)
                             .crop(area);
            } else {
                raster = tones::screened(meant_rgba(picture, color), spec, ctx.dpi, ctx.size, area);
            }
            Image dots = masked(ctx, layer, std::move(raster), area);
            if (tinted_here) dots = tinted(dots, *layer.color);
            canvas.blend_rgba(dots.mode() == "RGBA" ? dots : dots.convert("RGBA"), layer.opacity, layer.clip, layer.blend);
            return;
        }
        if (tinted_here) {  // (shown in its colour: the colour's inks by the rules, its alpha kept)
            const std::vector<std::int64_t> rgb = *layer.color;
            const unsigned char c[3] = {static_cast<unsigned char>(std::clamp<std::int64_t>(rgb.size() > 0 ? rgb[0] : 0, 0, 255)),
                                        static_cast<unsigned char>(std::clamp<std::int64_t>(rgb.size() > 1 ? rgb[1] : 0, 0, 255)),
                                        static_cast<unsigned char>(std::clamp<std::int64_t>(rgb.size() > 2 ? rgb[2] : 0, 0, 255))};
            std::array<double, core::kPlatesMax> ink{};
            ink::inks_of(color, c, nullptr, ink.data(), 1);
            const unsigned n = picture.channels;
            for (std::size_t i = 0; i < picture.values.size() / (n + 1); ++i)
                for (unsigned k = 0; k < n; ++k) picture.values[i * (n + 1) + k] = to_canvas(color.space, k, ink[k]);
        }
        std::optional<Image> shown;
        if (layer.mask && layer.mask->png && !layer.mask->png->empty() && layer.mask->enabled) {
            shown = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
        }
        canvas.blend(picture, layer.opacity, layer.clip, layer.blend, shown ? &*shown : nullptr);
        return;
    }
    if (layer.kind == LayerKind::Fill && truthy_json(layer.fill)) {
        Image raster = fill_layer_image(layer, ctx.size, ctx.dpi, false, area);
        if (panel != nullptr && layer.panel_clip) raster.putalpha(chops::multiply(alpha_of(raster), *panel));
        raster = masked(ctx, layer, std::move(raster), area);
        canvas.blend_rgba(raster, layer.opacity, layer.clip, layer.blend);
        return;
    }
    std::optional<Image> raster = layer_with_effects(ctx, layer, area, panel, ctx.rough);
    if (!raster) return;
    if (truthy_json(layer.screen) && ((print && ctx.dots) || screen_dots(ctx))) {
        raster = screened_layer(ctx, layer, *raster, area, panel);  // トーン化: its greys as dots in print (K)
    }
    Image picture = masked(ctx, layer, std::move(*raster), area);
    if (layer.color && !layer.color->empty() && (!print || layer.color_prints)) picture = tinted(picture, *layer.color);
    canvas.blend_rgba(picture.mode() == "RGBA" ? picture : picture.convert("RGBA"), layer.opacity, layer.clip, layer.blend);
}

// color-model@1: the page's values (GKCR f32 of its layers) with what is drawn over them in 8 bits — the panels' borders,
// the lines of dialogue, the effect lines, the nombres, the crop marks: where the picture differs from its layers as
// they are shown, the colour drawn (sRGB) in the page's space, opaque.
std::string with_drawn_over(const std::string& values, const Image& layers_shown, const Image& image, const RgbSpace& space) {
    const core::ColorRasterView view(values);
    const auto width = static_cast<std::size_t>(view.width());
    if (layers_shown.size() != image.size() || static_cast<std::size_t>(image.width()) != width ||
        static_cast<std::uint32_t>(image.height()) != view.height()) {
        return values;
    }
    return core::encode_color_pixels(view.width(), view.height(), "f32", [&](std::size_t i) {
        const std::size_t y = i / width, x = i % width;
        const auto* was = reinterpret_cast<const unsigned char*>(layers_shown.raw()->image[y]) + x * 4;
        const auto* now = reinterpret_cast<const unsigned char*>(image.raw()->image[y]) + x * 4;
        if (was[0] == now[0] && was[1] == now[1] && was[2] == now[2]) return view.pixel(i);
        double rgb[3] = {now[0] / 255.0, now[1] / 255.0, now[2] / 255.0};
        space.from_srgb(rgb, 1);
        return std::array<double, 4>{rgb[0], rgb[1], rgb[2], 1.0};
    });
}

// color-model@1: the values (GKCR f32) of a box of them, as a picture's crop cuts it.
std::string crop_values(const std::string& values, const Box& box) {
    const core::ColorRasterView view(values);
    const auto width = static_cast<std::size_t>(box.width());
    return core::encode_color_pixels(static_cast<std::uint32_t>(box.width()), static_cast<std::uint32_t>(box.height()), "f32", [&](std::size_t i) {
        return view.pixel((std::size_t(box.y0) + i / width) * view.width() + std::size_t(box.x0) + i % width);
    });
}

RenderResult render(const Page& page_in, int dpi, const RenderOptions& options, const core::Document* episode,
                    std::vector<std::string>& omitted) {
    if (!options.at_frame) {  // (an animation page prints as its first frame, not every cel at once)
        const Json* anim = get(page_in.extra, "anim");
        if (anim != nullptr && core::py_truthy(*anim)) {
            RenderOptions framed = options;
            framed.at_frame = true;
            return render(core::anim::at_frame(page_in, 1), dpi, framed, episode, omitted);
        }
    }
    if (options.region && needs_whole_page(page_in)) {
        const Box box = area_of(options, Size{mm_to_px(page_in.spec.width_mm.value(), dpi), mm_to_px(page_in.spec.height_mm.value(), dpi)});
        RenderOptions whole = options;
        whole.region.reset();
        RenderResult out = render(page_in, dpi, whole, episode, omitted);
        out.image = out.image.crop(box);
        if (out.inks) out.inks = crop_inks(*out.inks, box);
        if (out.precise) out.precise = crop_values(*out.precise, box);
        return out;
    }
    Ctx ctx;
    ctx.page = &page_in;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = options.mode;
    ctx.rough = options.rough;
    ctx.dots = options.dots;
    ctx.screen_dots = options.screen_dots;
    ctx.skip_unported = options.skip_unported;
    ctx.omitted = &omitted;
    ctx.stop = options.stop;
    const Page& page = page_in;
    // (a CMYK or Lab page is a colour page: never the monochrome finish)
    ctx.finish = page.color ? false : options.finish.value_or(page.spec.expression != "color");
    const int width = mm_to_px(page.spec.width_mm.value(), dpi);
    const int height = mm_to_px(page.spec.height_mm.value(), dpi);
    if (width > 0x7fffffff / 4 || height > 0x7fffffff / 4) throw core::Error("image_too_large", "the page is too large at this resolution");
    ctx.size = Size{width, height};
    const Box area = area_of(options, ctx.size);
    // Background inspection for white text edges must see the same neighbouring pixels in a region render.
    // Widen only intersecting labels' small sampling boxes, never force a whole-page high-precision canvas.
    if (options.region && (ctx.mode == "print" || ctx.mode == "proof")) {
        const Box expanded = nombre_required_area(area, ctx);
        if (expanded != area) {
            RenderOptions widened = options;
            widened.region = RenderRegion{expanded.x0, expanded.y0, expanded.width(), expanded.height()};
            RenderResult out = render(page_in, dpi, widened, episode, omitted);
            out.image = out.image.crop(shifted(area, -expanded.x0, -expanded.y0));
            if (out.inks) out.inks = crop_inks(*out.inks, shifted(area, -expanded.x0, -expanded.y0));
            if (out.precise) out.precise = crop_values(*out.precise, shifted(area, -expanded.x0, -expanded.y0));
            return out;
        }
    }
    if (static_cast<std::int64_t>(area.width()) * area.height() > kMaxAreaPixels) {
        throw core::Error("image_too_large", "the picture is too large at this resolution");
    }
    const std::string& mode = ctx.mode;
    const bool print = mode == "print";
    const bool name_or_proof = mode == "name" || mode == "proof";

    Image rgba = page_background(page,size_of(area),name_or_proof);

    std::optional<ColorCanvas> precision;
    if (uses_color_precision(page, print))
        precision.emplace(rgba, RgbSpace::of(page));  // (color-model@1: an RGB page of a profile, in its space)
    std::optional<Image> prev_alpha;
    const std::optional<Image> panel_mask = clip_mask(page, ctx.size, dpi, area);
    const Image* panel = panel_mask ? &*panel_mask : nullptr;
    // lines set under a layer (テキストの重ね順): drawn just before that layer, not over everything
    const std::vector<const core::StoryLine*> lines = lines_of(page, episode);
    std::vector<std::pair<std::string, std::vector<const core::StoryLine*>>> below;  // (layer id, its lines), as met
    std::vector<const core::StoryLine*> drawn_below;
    for (const core::StoryLine* line : lines) {
        const Json st = text::style_of(*line);
        const Json under = st.contains("below_layer") ? st.at("below_layer") : Json();
        if (!core::py_truthy(under)) continue;
        core::require_hashable(under);  // (`under in layer_ids`)
        if (!under.is_string()) continue;  // (a layer's id is a str)
        const std::string id = under.get<std::string>();
        const bool known = std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (!known || !placed_line(*line)) continue;
        auto it = std::find_if(below.begin(), below.end(), [&](const auto& b) { return b.first == id; });
        if (it == below.end()) {
            below.emplace_back(id, std::vector<const core::StoryLine*>{});
            it = below.end() - 1;
        }
        it->second.push_back(line);
        drawn_below.push_back(line);
    }
    const text::Panels panels = lines.empty() ? text::Panels{} : panels_of(page);
    const std::optional<std::string> font_path = episode != nullptr ? std::optional<std::string>(episode->font_path) : std::nullopt;
    // (what a balloon cannot draw yet — a font file that does not open or is past 32 MB, a warp with no map, OpenType
    // features across — stops the render, or is left out and reported with skip_unported: that line's letters)
    const text::Unported unported = [&ctx](const NotYetPorted& e) { skip_unported(ctx, e.element()); };
    // the previous page faint over this one (name and proof)
    const auto onion = [&](Image& image) {
        if (!(options.onion && episode != nullptr && name_or_proof && page.onion_from && page.onion_from->truthy())) return;
        const Page* prev = nullptr;
        for (const auto& p : episode->pages) {
            if (p->index == *page.onion_from) {
                prev = p.get();
                break;
            }
        }
        if (prev == nullptr) return;
        RenderOptions ghost_options;
        ghost_options.mode = "print";
        ghost_options.onion = false;
        ghost_options.dots = options.dots;
        ghost_options.screen_dots = options.screen_dots;
        ghost_options.skip_unported = options.skip_unported;
        ghost_options.region = options.region;
        ghost_options.stop = options.stop;
        const Size ghost_size{mm_to_px(prev->spec.width_mm.value(), dpi), mm_to_px(prev->spec.height_mm.value(), dpi)};
        if (ghost_size != ctx.size) throw core::Error("value", "images do not match");
        const Image ghost = render(*prev, dpi, ghost_options, episode, omitted).image.convert("RGBA");
        const std::vector<Image> c = ghost.split();
        const auto dim = [](int p) { return static_cast<int>(core::py_trunc_int(p * 0.4)); };  // int(p * 0.4)
        const Image tint = Image::merge("RGBA", {c[0].point(dim), c[1].point(dim), c[2], c[3].point([](int) { return 70; })});
        image = alpha_composite(image.convert("RGBA"), tint).convert("RGB");
    };
    if (page.color) {  // color-mode@1: a CMYK or Lab page, composited in its inks (SPEC.md COLOR-02)
        InkCanvas canvas(*page.color, rgba);
        for (const Layer& layer : page.layers) {
            check_cancel(ctx);
            if (const auto under = std::find_if(below.begin(), below.end(), [&](const auto& b) { return b.first == layer.id; });
                under != below.end()) {
                canvas.draw_8bit([&](Image& picture) {
                    text::draw_lines(text::PagePart{&picture, Point{area.x0, area.y0}, ctx.size}, under->second, dpi, font_path, !print,
                                     &panels, ctx.stop, &unported);
                });
            }
            if (layer.kind == LayerKind::Folder) continue;
            if (!layer.visible) continue;
            if (print && !layer.exportable) continue;
            if (guide_role(layer.role) && print) continue;
            if (shape_correction(layer)) {  // (PC2d: on the page's inks so far)
                ink_shape_correction(canvas, ctx, layer, area);
                continue;
            }
            ink_layer(canvas, ctx, layer, area, panel);
        }
        if (core::py_truthy(page.effects)) canvas.draw_8bit([&](Image& picture) { picture = draw_effects(ctx, picture, area); });
        canvas.draw_8bit([&](Image& picture) { draw_frames(picture, area, page, ctx.size, dpi); });
        if (!lines.empty()) {
            canvas.draw_8bit([&](Image& picture) { draw_story(picture, area, ctx, lines, drawn_below, panels, font_path, unported); });
        }
        if (print || mode == "proof") {
            const Image shown = canvas.display();
            canvas.draw_8bit([&](Image& picture) { draw_nombre(picture, area, ctx, &shown); });
        }
        if (options.crop_marks && print) {
            canvas.draw_8bit([&](Image& picture) {
                PageCanvas marks(picture, area, ctx.size);
                draw_crop_marks(marks.draw(), page, dpi);
                marks.commit();
            });
        }
        RenderResult out{canvas.display(), {}, std::nullopt, std::nullopt};
        if (options.inks) out.inks = canvas.ink_pixels(page.color->depth);
        // (what is never printed, over the page as shown)
        if (!print && core::py_truthy(page.prims)) draw_prims(out.image, area, ctx);
        if (truthy_json(page.ruler) && name_or_proof) draw_ruler(out.image, area, ctx);
        if (name_or_proof && get(page.extra, "cover") != nullptr && core::py_truthy(*get(page.extra, "cover"))) {
            const std::string binding(episode != nullptr ? core::to_string(episode->binding) : std::string_view("right"));
            draw_folds(out.image, area, ctx.size, page, dpi, binding, ctx.stop);
        }
        onion(out.image);
        return out;
    }
    for (const Layer& layer : page.layers) {
        check_cancel(ctx);
        if (const auto under = std::find_if(below.begin(), below.end(), [&](const auto& b) { return b.first == layer.id; });
            under != below.end()) {
            // (drawn on the picture so far. A page in precise colour, which Python does not have, keeps that picture in
            // its own canvas: the balloons are drawn as here on its 8-bit picture, and what they paint goes into it.)
            const auto draw = [&](Image& picture) {
                text::draw_lines(text::PagePart{&picture, Point{area.x0, area.y0}, ctx.size}, under->second, dpi, font_path, !print,
                                 &panels, ctx.stop, &unported);
            };
            if (precision) {
                precision->draw_8bit(draw);
            } else {
                draw(rgba);
            }
        }
        if (layer.kind == LayerKind::Folder) continue;
        if (!layer.visible) continue;
        if (print && !layer.exportable) continue;
        if (guide_role(layer.role) && print) continue;
        if (precision && (core::has_color_strokes(layer) || layer.color_raster)) {
            precise_layer(*precision, ctx, layer, area, panel);  // (PC2d: with its lines, effects, colour; a tone as tones)
            continue;
        }
        // (a correction layer's own blend is not looked at, as Python does not: below)
        if (precision && layer.kind != LayerKind::Adjust && !ColorCanvas::supports_blend(layer.blend))
            throw NotYetPorted("high_precision_blend:" + layer.blend);
        if (precision && shape_correction(layer)) {  // (PC2d: on the page's values so far)
            precise_shape_correction(*precision, ctx, layer, area);
            continue;
        }
        if (is_tone(layer)) {  // tones sit in the layer order: a layer above can cover them
            if (precision) {
                precise_tone(*precision, ctx, layer, area, panel);  // (PC2d: in 8 bits over the page's values)
                continue;
            }
            const bool dots = (print && ctx.finish && ctx.dots) || screen_dots(ctx);
            rgba = tones::draw_layer(std::move(rgba), layer, tone_page(ctx), area, panel, dots);
            prev_alpha.reset();
            continue;
        }
        if (layer.kind == LayerKind::Adjust) {  // a correction layer changes what is under it
            if (precision) {
                // (PC2, the user's decision 2: its native.blend laid by this canvas's blend formula)
                const std::string blend = layer.adjust ? core::native_of(*layer.adjust).blend : std::string("normal");
                if (layer.adjust && layer.adjust->is_object() && layer.adjust->value("kind", Json()) == "exposure") {
                    std::optional<Image> mask;
                    if (layer.mask && layer.mask->enabled && layer.mask->png)
                        mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
                    precision->expose(core::Exposure::parse(*layer.adjust), layer.opacity, layer.clip, mask ? &*mask : nullptr, blend);
                    continue;
                }
                if (layer.adjust && layer.adjust->is_object() && layer.adjust->value("kind", Json()).is_string()) {
                    const std::string kind = layer.adjust->value("kind", Json()).get<std::string>();
                    if (kind.starts_with("plugin:")) {
                        skip_unported(ctx, "adjust:plugin");  // (a person's filter plugin runs in the external runner)
                        continue;
                    }
                    // (one that moves shapes too: not ported for precise pixels; a book that was not checked may hold one)
                    if (std::find(core::kShapeFilters.begin(), core::kShapeFilters.end(), kind) != core::kShapeFilters.end()) {
                        skip_unported(ctx, "adjust:" + kind);
                        continue;
                    }
                }
                core::AdjustPtr correction;
                try {
                    correction = correction_of(layer, page, episode);
                } catch (const NotYetPorted& error) {
                    skip_unported(ctx, error.element());
                    continue;
                }
                if (correction) {  // (its mask as render._adjusted resizes it)
                    std::optional<Image> mask;
                    if (layer.mask && layer.mask->enabled && layer.mask->png && !layer.mask->png->empty())
                        mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bicubic);
                    precision->adjust(*correction, layer.opacity, layer.clip, mask ? &*mask : nullptr, blend, Point{area.x0, area.y0});
                }
                continue;
            }
            rgba = adjusted(ctx, std::move(rgba), layer, layer.clip && prev_alpha ? &*prev_alpha : nullptr, area);
            continue;
        }
        if (layer.kind == LayerKind::Fill && truthy_json(layer.fill)) {
            Image raster = fill_layer_image(layer, ctx.size, dpi, page.spec.expression != "color", area);
            if (panel != nullptr && layer.panel_clip) raster.putalpha(chops::multiply(alpha_of(raster), *panel));
            raster = masked(ctx, layer, std::move(raster), area);
            const Image* clip = layer.clip && prev_alpha ? &*prev_alpha : nullptr;
            if (precision) precision->blend(raster, layer.opacity, layer.clip, layer.blend);
            else rgba = blend_over(rgba, raster, layer.blend.empty() ? "normal" : layer.blend, layer.opacity, clip);
            prev_alpha = alpha_of(raster);
            continue;
        }
        std::optional<Image> raster = layer_with_effects(ctx, layer, area, panel, ctx.rough);
        if (!raster) continue;
        const Json* source_kind = layer.source ? get(*layer.source, "kind") : nullptr;
        const bool finished = ctx.finish && (print || mode == "proof") && source_kind != nullptr && *source_kind == "psd" && !truthy_json(layer.screen);
        if (finished) {
            // (a picture from a painting app on a monochrome page is finished like placed art: grey and tones. The
            // finish looks at each pixel's neighbourhood and lays its dots from the page's corner: the whole picture.)
            const Box all{0, 0, ctx.size.width, ctx.size.height};
            if (area == all) {
                if (has_colour(*raster)) raster = finish_placed(*raster, layer, page, episode, dpi, mode, ctx.dots, Point{0, 0});
            } else {
                const auto full_mask = clip_mask(page, ctx.size, dpi, all);
                const auto full = layer_with_effects(ctx, layer, all, full_mask ? &*full_mask : nullptr, ctx.rough);
                if (full && has_colour(*full)) {
                    raster = finish_placed(*full, layer, page, episode, dpi, mode, ctx.dots, Point{0, 0}).crop(area);
                }
            }
        }
        if (truthy_json(layer.screen) && ((print && ctx.dots) || screen_dots(ctx))) {
            raster = screened_layer(ctx, layer, *raster, area, panel);  // トーン化: its greys as dots in print
        }
        Image picture = masked(ctx, layer, std::move(*raster), area);
        const bool tint = layer.color && !layer.color->empty() && (!print || layer.color_prints);
        if (tint) picture = tinted(picture, *layer.color);
        const Image* clip = layer.clip && prev_alpha ? &*prev_alpha : nullptr;
        if (precision) {
            // (color-model@1: a paint layer's pixels on a page of a profile are its values — as drawn here: not once a
            // tint, an effect, a screen or the monochrome finish makes the picture in sRGB)
            const bool page_values = drawn_in_values(ctx, layer) && !tint && !finished;
            precision->blend(picture, layer.opacity, layer.clip, layer.blend, page_values);
        } else {
            rgba = blend_over(rgba, picture, layer.blend.empty() ? "normal" : layer.blend, layer.opacity, clip);
        }
        prev_alpha = alpha_of(picture);
    }
    Image image = (precision ? precision->image() : rgba).convert("RGB");
    std::optional<std::string> precise;  // (color-model@1: the page's values, asked for)
    std::optional<Image> layers_shown;   // (and how its layers alone are shown: what is drawn over them is told by it)
    if (options.precise && precision) {
        precise = precision->color_raster("f32");
        layers_shown = image.copy();
    }

    if (core::py_truthy(page.effects)) image = draw_effects(ctx, image, area);  // effect lines (M3-B)
    if (!print && core::py_truthy(page.prims)) draw_prims(image, area, ctx);  // 3D guides, never printed (M3-C)
    if (truthy_json(page.ruler) && name_or_proof) draw_ruler(image, area, ctx);
    draw_frames(image, area, page, ctx.size, dpi);
    // where a jacket or a band folds, and what each part is (in name and proof; covers.draw_folds)
    if (name_or_proof && get(page.extra, "cover") != nullptr && core::py_truthy(*get(page.extra, "cover"))) {
        const std::string binding(episode != nullptr ? core::to_string(episode->binding) : std::string_view("right"));
        draw_folds(image, area, ctx.size, page, dpi, binding, ctx.stop);
    }
    // lines: placed ones in balloons, the others as labels
    if (!lines.empty()) draw_story(image, area, ctx, lines, drawn_below, panels, font_path, unported);
    if (print || mode == "proof") draw_nombre(image, area, ctx);
    if (options.crop_marks && print) {
        PageCanvas canvas(image, area, ctx.size);
        draw_crop_marks(canvas.draw(), page, dpi);
        canvas.commit();
    }
    onion(image);
    if (precise) precise = with_drawn_over(*precise, *layers_shown, image, RgbSpace::of(page));
    return RenderResult{std::move(image), {}, std::nullopt, std::move(precise)};
}

// The first `count` characters of UTF-8 text (Python's text[:count]).
std::string first_chars(const std::string& text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t n = 0; n < count && at < text.size(); ++n) {
        const auto lead = static_cast<unsigned char>(text[at]);
        at += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    }
    return text.substr(0, std::min(at, text.size()));
}

}  // namespace

void page_layers(const core::Page& page, const core::Document& episode, int dpi, const std::function<void(PageLayer&&)>& each,
                 const std::function<void(const core::StoryLine&)>* line_layers, const std::function<void(const core::Layer&)>* adjust_layers) {
    // (color-model@1: the book's text_gamma, for the letters of the lines' layers — as render_page draws them)
    const text::TextGamma gamma(episode.text_gamma);
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = &episode;
    ctx.dpi = dpi;
    ctx.mode = "print";
    ctx.finish = true;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const Size size = ctx.size;
    if (static_cast<std::int64_t>(size.width) * size.height > kMaxAreaPixels) {
        throw core::Error("image_too_large", "the picture is too large at this resolution");
    }
    const Box whole{0, 0, size.width, size.height};
    const auto blank = [&] { return transparent(whole); };
    const auto give = [&](std::string name, Image image) {
        PageLayer out;
        out.name = std::move(name);
        out.image = std::move(image);
        each(std::move(out));
    };
    give("紙", Image::create("RGBA", size, Ink{255, 255, 255, 255}));
    for (const LayerRole role : {LayerRole::Bg, LayerRole::Ink, LayerRole::Finish}) {
        const core::NumList* fill = page.fill_of(role);
        if (fill == nullptr) continue;
        std::vector<std::int64_t> rgba;
        for (const Num& v : *fill) rgba.push_back(core::py_int(v.json()));
        rgba.push_back(255);
        give("塗り " + std::string(core::to_string(role)), Image::create("RGBA", size, Ink::tuple(rgba)));
    }
    for (const Layer& layer : page.layers) {
        check_cancel(ctx);
        if (!layer.visible || !layer.exportable || guide_role(layer.role)) continue;
        if (layer.kind == LayerKind::Folder) continue;
        if (adjust_layers != nullptr && layer.kind == LayerKind::Adjust) (*adjust_layers)(layer);  // (PC2: in its place)
        if (layer.kind == LayerKind::Placed) {  // its art before the monochrome finish (mode "name")
            auto art = placed_raster(layer, page, &episode, size, dpi, "name", true, ctx.dots, whole);
            if (art) {
                const Json to = core::py_get(layer.source ? core::py_or(*layer.source, Json::object()) : Json::object(), "to",
                                             Json("art"));
                core::require_hashable(to);  // ({…}.get(to, to))
                std::string label = core::py_str(to);
                if (to == Json("art")) label = "絵";
                else if (to == Json("bg")) label = "背景";
                else if (to == Json("ink")) label = "線画";
                else if (to == Json("draft")) label = "下描き";
                give(core::py_strip(label + " " + (!layer.title.empty() ? layer.title : layer.id)), std::move(*art));
            }
            continue;
        }
        std::optional<Image> raster;
        std::shared_ptr<const std::vector<double>> own_inks;
        if (layer.color_raster) {  // (a layer in precise colour, which Python does not have: its 8-bit picture)
            raster = color_raster_preview(core::ColorRasterView(*layer.color_raster), size, whole);
        } else if (layer.raster_png && !layer.raster_png->empty()) {
            raster = raster_part(layer.raster_png, size, whole);
        } else if (layer.ink_raster && page.color) {  // (color-mode@1: its inks, and how they are shown)
            InkPicture picture = sample_ink_raster(*decoded_ink(layer.ink_raster), size, whole);
            raster = ink_picture_rgba(picture, *page.color);
            own_inks = std::make_shared<const std::vector<double>>(std::move(picture.values));
        }
        if (!raster) continue;
        PageLayer out;
        out.inks = std::move(own_inks);
        // (color-model@1: on an RGB page of a profile a paint layer's pixels are the page's values)
        out.page_values = (core::profiled_rgb(page) && layer.color_raster) || core::holds_page_values(page, layer);
        out.name = !layer.title.empty() ? layer.title : std::string(core::to_string(layer.role)) + " " + layer.id.substr(0, 6);
        out.image = std::move(*raster);
        out.settings = true;
        out.opacity = layer.opacity;
        out.blend = layer.blend;
        out.clip = layer.clip;
        if (layer.mask && layer.mask->png && !layer.mask->png->empty() && layer.mask->enabled) {
            out.mask = decoded(layer.mask->png, "L").resize(size);
        }
        each(std::move(out));
    }
    // the pen lines of the ink layer (page.ink_strokes: its first), each as render._stroke draws it, 3 px wide
    const Layer* ink_layer = page.first_layer(LayerRole::Ink);
    const bool ink_has_raster = std::any_of(page.layers.begin(), page.layers.end(), [](const Layer& l) {
        return l.role == LayerRole::Ink && ((l.raster_png && !l.raster_png->empty()) || l.color_raster || l.ink_raster);
    });
    if (ink_layer != nullptr && ink_layer->stroke_count() > 0 && !ink_has_raster) {
        Image ink = blank();
        {
            Draw draw(ink);
            const Ink colour{20, 20, 20};
            for (const core::StrokePtr& stroke : ink_layer->strokes->items) {
                check_cancel(ctx);
                const auto& pts = stroke->points;
                if (pts.size() < 2) continue;
                const auto at = [&](std::size_t i) {
                    return PointD{static_cast<double>(mm_to_px(pts[i].x, dpi)), static_cast<double>(mm_to_px(pts[i].y, dpi))};
                };
                if (!stroke->pressure.empty() && stroke->pressure.size() == pts.size()) {
                    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                        const double pressure = stroke->pressure[i];
                        const int w = std::max<int>(1, static_cast<int>(core::py_round_int(3 * std::max(0.15, std::min(1.5, pressure)))));
                        const std::vector<PointD> seg{at(i), at(i + 1)};
                        draw.line(seg, colour, w, Joint::Curve);
                    }
                } else {
                    std::vector<PointD> all;
                    all.reserve(pts.size());
                    for (std::size_t i = 0; i < pts.size(); ++i) all.push_back(at(i));
                    draw.line(all, colour, 3, Joint::Curve);
                }
            }
        }
        if (const auto mask = clip_mask(page, size, dpi, whole)) ink.putalpha(chops::multiply(alpha_of(ink), *mask));
        give("ペン入れ", std::move(ink));
    }
    if (std::any_of(page.layers.begin(), page.layers.end(), [](const Layer& l) { return l.role == LayerRole::Tone && l.visible; })) {
        Image tones_image = blank();
        const std::optional<Image> panel_mask = clip_mask(page, size, dpi, whole);
        for (const Layer& layer : page.layers) {
            if (is_tone(layer) && layer.visible) {
                tones_image = tones::draw_layer(std::move(tones_image), layer, tone_page(ctx), whole, panel_mask ? &*panel_mask : nullptr, true);
            }
        }
        give("トーン", std::move(tones_image));
    }
    if (core::py_truthy(page.effects)) {
        Image lines = blank();
        for (const Json& effect : core::iterate(page.effects)) {
            if (!effects::drawn(effect)) continue;
            lines = effects::draw(std::move(lines), effect, page, dpi, size, whole);
        }
        give("効果", std::move(lines));
    }
    Image frames = blank();
    draw_frames(frames, whole, page, size, dpi);
    give("コマ枠", std::move(frames));
    const std::optional<std::string> font_path(episode.font_path);
    for (const core::StoryLine* line : episode.story_for_page(page.index)) {
        if (!placed_line(*line)) continue;
        check_cancel(ctx);
        if (line_layers != nullptr && *line_layers) {  // (PSD-01: the editable PSD makes the line's layers)
            (*line_layers)(*line);
            continue;
        }
        Image balloon = blank();
        text::draw_lines(text::whole_page(balloon), {line}, dpi, font_path, false, nullptr, ctx.stop, nullptr);
        std::string words = line->text;
        std::erase(words, '\n');
        give("台詞 " + first_chars(words, 24), std::move(balloon));
    }
    if (!nombre_items(page, &episode).empty()) {
        Image numero = blank();
        draw_nombre(numero, whole, ctx);
        give("ノンブル", std::move(numero));
    }
}

std::vector<Box> nombre_areas(const core::Page& page, int dpi, const core::Document* episode) {
    const auto items = nombre_items(page, episode);
    std::vector<Box> out;
    if (items.empty()) return out;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const auto text = nombre_text(page, episode), font = nombre_font(episode);
    for (const auto& item : items) {
        const int size = std::max(6, mm_to_px(item[2], dpi));
        out.push_back(nombre_geometry(text, font, size, item, ctx).second);
    }
    return out;
}

std::optional<Image> detail::layer_lines(const Ctx& ctx, const core::Layer& layer, const Image* panel_mask, const Image* raster,
                                         bool with_page) {
    return layer_strokes(ctx, layer, Box{0, 0, ctx.size.width, ctx.size.height}, panel_mask, raster, false, with_page);
}

// --- public ---------------------------------------------------------------------------------------------------------

RenderOptions proof_options() {
    RenderOptions options;
    options.mode = "proof";
    return options;
}

std::vector<core::LayerRole> export_plan(const core::Page&) {
    return {LayerRole::Ink, LayerRole::Bg, LayerRole::Finish, LayerRole::Tone, LayerRole::Effect, LayerRole::Frames,
            LayerRole::Text};
}

int mm_to_px(double mm, int dpi) {
    const double px = mm / 25.4 * dpi;
    // max(1, round(px)); a size an int cannot hold is held as the largest (the page is refused as too large)
    if (std::isfinite(px) && px > 2147483647.0) return 2147483647;
    if (std::isfinite(px) && px < -2147483648.0) return 1;
    return std::max(1, static_cast<int>(core::py_round_int(px)));
}

Box rect_px(const core::Rect& rect, int dpi) {
    const int x = mm_to_px(rect.x.value(), dpi);
    const int y = mm_to_px(rect.y.value(), dpi);
    const int w = mm_to_px(rect.width.value(), dpi);
    const int h = mm_to_px(rect.height.value(), dpi);
    return Box{x, y, x + w, y + h};
}

bool uses_color_precision(const core::Page& page, bool print) {
    if (page.color) return false;  // (a CMYK, Lab or grey page has its own canvas)
    if (page.rgb && !page.rgb->profile.empty()) return true;  // (color-model@1: an RGB page of a profile, in its space)
    return std::any_of(page.layers.begin(), page.layers.end(), [print](const Layer& l) {
        return (l.color_raster || core::has_color_strokes(l)) && l.kind != LayerKind::Folder && l.visible &&
               (!print || (l.exportable && !guide_role(l.role)));
    });
}

Image page_background(const core::Page& page, Size size, bool name_or_proof) {
    // Keep the established fill order and integer/channel semantics unchanged.
    std::vector<std::int64_t> paper{255, 255, 255};
    const Json* paper_json = get(page.extra, "paper_rgb");
    if (paper_json != nullptr && core::py_truthy(*paper_json)) paper = first3(core::int_tuple(*paper_json));
    Image rgba = Image::create("RGBA", size, Ink::with_alpha(paper, 255));
    std::vector<LayerRole> fill_roles{LayerRole::Bg, LayerRole::Ink, LayerRole::Finish};
    if (name_or_proof) fill_roles = {LayerRole::Bg, LayerRole::Name, LayerRole::Ink, LayerRole::Finish};
    for (const LayerRole role : fill_roles) {
        const core::NumList* fill = page.fill_of(role);
        if (fill == nullptr) continue;
        if (guide_role(role) && !name_or_proof) continue;
        std::vector<std::int64_t> rgb;
        for (std::size_t i = 0; i < fill->size() && i < 3; ++i) rgb.push_back(core::py_int((*fill)[i]));
        rgba.paste(Ink::with_alpha(rgb, 255), Box{0, 0, size.width, size.height});
    }
    return rgba;
}

core::InkPixels composite_ink_layers(const core::Page& page, const std::vector<core::Layer>& layers, int dpi, const core::Document* episode,
                                     bool paper) {
    if (!page.color) throw core::Error("value", "an RGB page holds no inks");
    std::vector<std::string> omitted;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = "proof";  // (as layer_image draws what the 8-bit merges composite)
    ctx.omitted = &omitted;
    ctx.finish = false;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    limits::check_picture(ctx.size.width, ctx.size.height, "the page");
    const Box area{0, 0, ctx.size.width, ctx.size.height};
    InkCanvas canvas = paper ? InkCanvas(*page.color, page_background(page, ctx.size, false)) : InkCanvas(*page.color, ctx.size);
    const std::optional<Image> panel_mask = clip_mask(page, ctx.size, dpi, area);
    for (const Layer& layer : layers) {
        if (layer.kind == LayerKind::Folder) continue;
        ink_layer(canvas, ctx, layer, area, panel_mask ? &*panel_mask : nullptr);
    }
    return canvas.ink_pixels(page.color->depth, true);
}

namespace {

RenderResult drawn_page(const core::Page& page, int dpi, const RenderOptions& options, const core::Document* episode) {
    // (color-model@1: the book's text_gamma, for the letters of its lines drawn on this thread)
    const text::TextGamma gamma(episode != nullptr ? episode->text_gamma : std::nullopt);
    std::vector<std::string> omitted;
    // Keep every layer in high precision within a region; quantize only the final output pixels.
    // Reuse the established global-coordinate region renderer, not an RGBA8 layer intermediate.
    // (a CMYK or Lab page too: its canvas holds floats)
    if((uses_color_precision(page,options.mode=="print") || page.color) && !needs_whole_page(page)){
        const Size size{mm_to_px(page.spec.width_mm.value(),dpi),mm_to_px(page.spec.height_mm.value(),dpi)};
        if(size.width>0x7fffffff/4 || size.height>0x7fffffff/4)throw core::Error("image_too_large","the page is too large at this resolution");
        const Box area=area_of(options,size);
        const auto pixels=static_cast<std::int64_t>(area.width())*area.height();
        if(pixels>kMaxAreaPixels)throw core::Error("image_too_large","the picture is too large at this resolution");
        // (color-model@1: the page's values, held whole in GKCR f32 as a precise raster is: past the most pixels one
        // holds, refused before anything is made)
        const bool values=options.precise && !page.color && uses_color_precision(page,options.mode=="print");
        if(values && static_cast<std::uint64_t>(pixels)>core::kColorRasterMaxPixels)
            throw core::Error("image_too_large","the page's values are too large at this resolution (at most "+
                              std::to_string(core::kColorRasterMaxPixels)+" pixels)");
        if(pixels>1024LL*1024){
            if(options.stop.stop_requested())throw Cancelled();
            RenderResult out{Image::create_blank("RGB",size_of(area)),{},std::nullopt,std::nullopt};
            if(options.inks && page.color){  // (each tile's inks laid in)
                out.inks=core::blank_ink_pixels(page.color->space,page.color->depth,static_cast<std::uint32_t>(area.width()),
                                                static_cast<std::uint32_t>(area.height()),page.color->planes());
            }
            // (color-model@1: each tile's values laid into the page's GKCR f32, row by row)
            if(values){
                std::string gkcr("GKCR\1\4\0\0",8);
                for(const std::uint32_t v:{static_cast<std::uint32_t>(area.width()),static_cast<std::uint32_t>(area.height())})
                    for(int b=0;b<4;++b)gkcr+=static_cast<char>(v>>(8*b)&0xff);
                gkcr.resize(16+static_cast<std::size_t>(pixels)*16,'\0');
                out.precise=std::move(gkcr);
            }
            constexpr int tile=512;
            for(int y=area.y0;y<area.y1;y+=tile){
                for(int x=area.x0;x<area.x1;x+=tile){
                    if(options.stop.stop_requested())throw Cancelled();
                    RenderOptions part=options;part.region=RenderRegion{x,y,std::min(tile,area.x1-x),std::min(tile,area.y1-y)};
                    const auto piece=render(page,dpi,part,episode,omitted);
                    out.image.paste(piece.image,Point{x-area.x0,y-area.y0});
                    if(out.inks && piece.inks)paste_inks(*out.inks,*piece.inks,Point{x-area.x0,y-area.y0});
                    if(values){
                        if(!piece.precise)throw core::Error("value","the page's values were not drawn");
                        const core::ColorRasterView tile_values(*piece.precise);
                        if(static_cast<int>(tile_values.width())!=piece.image.width() || static_cast<int>(tile_values.height())!=piece.image.height())
                            throw core::Error("value","the page's values do not match its picture");
                        const std::size_t row=std::size_t(tile_values.width())*16;
                        for(std::uint32_t r=0;r<tile_values.height();++r){
                            const std::size_t to=16+((std::size_t(y-area.y0)+r)*std::size_t(area.width())+std::size_t(x-area.x0))*16;
                            out.precise->replace(to,row,*piece.precise,16+r*row,row);
                        }
                    }
                }
            }
            out.omitted=std::move(omitted);return out;
        }
    }
    RenderResult out = render(page, dpi, options, episode, omitted);
    out.omitted = std::move(omitted);
    return out;
}

}  // namespace

namespace {

// color-model@1 (PC1d): an indexed page shown in its table — the whole picture, its frames, words and marks too, each
// pixel its nearest colour (no dither: a part of the page drawn the same as the whole; PhotoCraft's ColorTable::nearest).
void in_table(RenderResult& out, const core::Page& page) {
    if (page.color || !page.rgb || !page.rgb->table) return;
    const core::ColorTable& table = *page.rgb->table;
    Image& image = out.image;
    const std::string mode(image.mode());
    if (mode != "RGB" && mode != "RGBA") return;
    const unsigned bands = static_cast<unsigned>(image.bands());
    std::string bytes = image.tobytes();
    std::unordered_map<std::uint32_t, std::size_t> nearest;
    for (std::size_t at = 0; at + bands <= bytes.size(); at += bands) {
        auto* q = reinterpret_cast<unsigned char*>(bytes.data() + at);
        const std::uint32_t key = (std::uint32_t(q[0]) << 16) | (std::uint32_t(q[1]) << 8) | q[2];
        auto it = nearest.find(key);
        if (it == nearest.end()) it = nearest.emplace(key, table.nearest(q[0], q[1], q[2])).first;
        for (unsigned k = 0; k < 3; ++k) q[k] = static_cast<unsigned char>(table.colors[it->second][k]);
    }
    image = Image::frombytes(mode, image.size(), bytes);
}

}  // namespace

RenderResult render_page(const core::Page& page, int dpi, const RenderOptions& options, const core::Document* episode) {
    const bool hiding = options.planes && std::find(options.planes->begin(), options.planes->end(), false) != options.planes->end();
    if (page.channels.empty() && !hiding) {
        RenderResult out = drawn_page(page, dpi, options, episode);
        in_table(out, page);
        return out;
    }
    // (channels@1: the spots printed, the panel's view; a CMYK or Lab page's planes shown from its inks)
    RenderOptions asked = options;
    const bool lent_inks = hiding && page.color && !options.inks;
    if (lent_inks) asked.inks = true;
    RenderResult out = drawn_page(page, dpi, asked, episode);
    in_table(out, page);
    channels::finish(out, page, dpi, options);
    if (lent_inks) out.inks.reset();
    return out;
}

Image render_frame(const core::Page& page, std::string_view frame_id, int dpi, const RenderOptions& options,
                   const core::Document* episode) {
    const core::Frame* frame = page.find_frame(frame_id);
    if (frame == nullptr) throw core::Error("key", std::string(frame_id));
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const Box b = rect_px(core::clip_box(page, frame, frame->bleed ? "bleed" : "frame"), dpi);
    const Box box{std::max(0, b.x0), std::max(0, b.y0), std::min(size.width, b.x1), std::min(size.height, b.y1)};
    if (box.x1 < box.x0) throw core::Error("value", "Coordinate 'right' is less than 'left'");
    if (box.y1 < box.y0) throw core::Error("value", "Coordinate 'lower' is less than 'upper'");
    if (box.x1 == box.x0 || box.y1 == box.y0) return render_page(page, dpi, options, episode).image.crop(box);
    RenderOptions part = options;
    part.region = RenderRegion{box.x0, box.y0, box.width(), box.height()};
    return render_page(page, dpi, part, episode).image;
}

Image render_spread(const core::Document& episode, const core::Num& first, const core::Num& second, int dpi,
                    const RenderOptions& options, bool to_trim) {
    const core::Page* a = nullptr;
    const core::Page* b = nullptr;
    for (const auto& p : episode.pages) {  // ({page.index: page}: the last of an index wins)
        if (p->index == first) a = p.get();
        if (p->index == second) b = p.get();
    }
    if (a == nullptr) throw core::Error("key", first.repr());
    if (b == nullptr) throw core::Error("key", second.repr());
    const std::string start = episode.start_side.value_or("");
    const core::Page* left = a;
    const core::Page* right = b;
    if (a->side(start) == "right" && b->side(start) == "left") {
        right = a;
        left = b;
    } else if (b->side(start) == "right" && a->side(start) == "left") {
        right = b;
        left = a;
    }
    RenderOptions o = options;
    o.region.reset();
    Image left_img = render_page(*left, dpi, o, &episode).image;
    Image right_img = render_page(*right, dpi, o, &episode).image;
    const core::Rect lt = left->trim_rect_mm();
    const core::Rect rt = right->trim_rect_mm();
    const auto top = [&](const core::Rect& t) { return to_trim ? mm_to_px(t.y.value(), dpi) : 0; };
    const auto bottom = [&](const core::Rect& t, const Image& img) {
        return to_trim ? mm_to_px((t.y + t.height).value(), dpi) : img.height();
    };
    left_img = left_img.crop(Box{to_trim ? mm_to_px(lt.x.value(), dpi) : 0, top(lt), mm_to_px((lt.x + lt.width).value(), dpi),
                                 bottom(lt, left_img)});
    right_img = right_img.crop(Box{mm_to_px(rt.x.value(), dpi), top(rt),
                                   to_trim ? mm_to_px((rt.x + rt.width).value(), dpi) : right_img.width(), bottom(rt, right_img)});
    Image image = Image::create("RGB", Size{left_img.width() + right_img.width(), std::max(left_img.height(), right_img.height())},
                                Ink{255, 255, 255});
    image.paste(left_img, Point{0, 0});
    image.paste(right_img, Point{left_img.width(), 0});
    return image;
}

void blend_color_strokes(ColorCanvas& canvas, const core::Page& page, const core::Layer& layer, int dpi, Box area, const core::Document* episode) {
    std::vector<std::string> omitted;
    Ctx ctx;ctx.page=&page;ctx.episode=episode;ctx.dpi=dpi;ctx.mode="proof";ctx.omitted=&omitted;
    ctx.size=Size{mm_to_px(page.spec.width_mm.value(),dpi),mm_to_px(page.spec.height_mm.value(),dpi)};
    const auto panels=clip_mask(page,ctx.size,dpi,area);
    if (layer.panel_each && !layer.mask && !layer.effect && !layer.screen && !layer.color && layer.patches.empty() && !layer.raster_png) {
        // (each line in the panel it begins in, as precise_lines lays them for the page)
        canvas.blend(precise_lines(ctx, layer, area, panels ? &*panels : nullptr, nullptr, canvas.space()), layer.opacity, layer.clip, layer.blend);
        return;
    }
    precise_strokes(canvas,ctx,layer,area,panels?&*panels:nullptr);
}
bool precise_layer_styled(const core::Layer& layer) {
    return (core::has_color_strokes(layer) || layer.color_raster) && (is_tone(layer) || !plain_precise(layer));
}
void blend_precise_layer(ColorCanvas& canvas, const core::Page& page, const core::Layer& layer, int dpi, Box area, const core::Document* episode) {
    if (is_tone(layer)) {  // (a tone's ink as the 8-bit merge takes it: drawable_layer_image)
        if (const auto picture = drawable_layer_image(page, layer, dpi, episode, false, true))
            canvas.blend(picture->crop(area), layer.opacity, layer.clip, layer.blend);
        return;
    }
    std::vector<std::string> omitted;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = "proof";
    ctx.omitted = &omitted;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const auto panels = clip_mask(page, ctx.size, dpi, area);
    precise_layer(canvas, ctx, layer, area, panels ? &*panels : nullptr);
}
Image cel_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode) {
    std::vector<std::string> omitted;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = "proof";
    ctx.skip_unported = true;  // (a faint picture while drawing: what is not drawn yet is left out)
    ctx.omitted = &omitted;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const Box area{0, 0, ctx.size.width, ctx.size.height};
    std::optional<Image> raster;
    if (layer.color_raster && !is_tone(layer)) {
        raster = color_raster_preview(core::ColorRasterView(*layer.color_raster), ctx.size, area);
    } else if (layer.kind != LayerKind::Placed && layer.raster_png && !layer.raster_png->empty()) {
        raster = raster_part(layer.raster_png, ctx.size, area);
    }
    auto lines = layer_strokes(ctx, layer, area, nullptr, raster ? &*raster : nullptr, false, false);
    if (lines) raster = raster ? alpha_composite(raster->convert("RGBA"), *lines) : std::move(*lines);
    return raster ? raster->convert("RGBA") : Image::create("RGBA", ctx.size, Ink{0, 0, 0, 0});
}

Image layer_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode,
                  bool skip_unported_flag, bool bake_color) {
    std::vector<std::string> omitted;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = "proof";
    ctx.skip_unported = skip_unported_flag;
    ctx.omitted = &omitted;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const Box area{0, 0, ctx.size.width, ctx.size.height};
    Image empty = transparent(area);
    const auto colored = [&](Image image) {
        return bake_color && layer.color && !layer.color->empty() ? tinted(image, *layer.color) : image;
    };
    if (layer.kind == LayerKind::Folder) return empty;
    if (is_tone(layer)) {  // the tone's ink as alpha, drawn on white
        const Image white = Image::create("RGBA", ctx.size, Ink{255, 255, 255, 255});
        const std::optional<Image> panels = clip_mask(page, ctx.size, dpi, area);
        const Image drawn = tones::draw_layer(white, layer, tone_page(ctx), area, panels ? &*panels : nullptr, false);
        Image out = Image::create("RGBA", ctx.size, Ink{20, 20, 20, 0});
        out.putalpha(chops::difference(white.convert("L"), drawn.convert("L")));
        return colored(masked(ctx, layer, std::move(out), area));
    }
    if (layer.kind == LayerKind::Adjust) return empty;
    const std::optional<Image> panel_mask = clip_mask(page, ctx.size, dpi, area);
    if (layer.kind == LayerKind::Fill && truthy_json(layer.fill)) {
        Image raster = fill_layer_image(layer, ctx.size, dpi, page.spec.expression != "color", area);
        if (panel_mask && layer.panel_clip) raster.putalpha(chops::multiply(alpha_of(raster), *panel_mask));
        return colored(masked(ctx, layer, std::move(raster), area));
    }
    std::optional<Image> raster = layer_pixels(ctx, layer, area, panel_mask ? &*panel_mask : nullptr, false);
    if (!raster) return empty;
    return colored(masked(ctx, layer, layer_effects(layer, std::move(*raster), dpi), area));
}

Image tone_mask(const core::Page& page, const core::Layer& layer, Size size, int dpi) {
    const tones::Page p{&page, dpi, size, [dpi](Image& out, const Box& box, const core::Patch& patch) { paint_patch(out, box, patch, dpi, nullptr); }};
    return tones::mask(layer, p);
}

bool picture_holds_page_values(const core::Page& page, const core::Layer& layer, bool tinted) {
    return core::holds_page_values(page, layer) && !(layer.effect && core::py_truthy(*layer.effect)) && !tinted;
}

std::optional<Image> drawable_layer_image(const core::Page& page, const core::Layer& layer, int dpi, const core::Document* episode,
                  bool skip_unported_flag, bool bake_color) {
    std::vector<std::string> omitted;
    Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.mode = "proof";
    ctx.skip_unported = skip_unported_flag;
    ctx.omitted = &omitted;
    ctx.size = Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const Box area{0, 0, ctx.size.width, ctx.size.height};
    const auto colored = [&](Image image) {
        return bake_color && layer.color && !layer.color->empty() ? tinted(image, *layer.color) : image;
    };
    if (layer.kind == LayerKind::Folder) return std::nullopt;
    if (is_tone(layer)) {  // the tone's ink as alpha, drawn on white
        const Image white = Image::create("RGBA", ctx.size, Ink{255, 255, 255, 255});
        const std::optional<Image> panels = clip_mask(page, ctx.size, dpi, area);
        const Image drawn = tones::draw_layer(white, layer, tone_page(ctx), area, panels ? &*panels : nullptr, false);
        Image out = Image::create("RGBA", ctx.size, Ink{20, 20, 20, 0});
        out.putalpha(chops::difference(white.convert("L"), drawn.convert("L")));
        return colored(masked(ctx, layer, std::move(out), area));
    }
    if (layer.kind == LayerKind::Adjust) return std::nullopt;
    const std::optional<Image> panel_mask = clip_mask(page, ctx.size, dpi, area);
    if (layer.kind == LayerKind::Fill && truthy_json(layer.fill)) {
        Image raster = fill_layer_image(layer, ctx.size, dpi, page.spec.expression != "color", area);
        if (panel_mask && layer.panel_clip) raster.putalpha(chops::multiply(alpha_of(raster), *panel_mask));
        return masked(ctx, layer, std::move(raster), area);
    }
    std::optional<Image> raster = layer_pixels(ctx, layer, area, panel_mask ? &*panel_mask : nullptr, false);
    if (!raster) return std::nullopt;
    return masked(ctx, layer, colored(layer_effects(layer, std::move(*raster), dpi)), area);
}

Image to_bitonal(const Image& image, int threshold, const core::Json* screen) {
    const Image grey = image.convert("L");
    if (screen == nullptr || !core::py_truthy(*screen)) {
        return grey.point([&](int p) { return p > threshold ? 255 : 0; }, "1");
    }
    const Json* pattern_json = get(*screen, "pattern");
    const std::string pattern = (pattern_json != nullptr && core::py_truthy(*pattern_json)) ? core::py_str(*pattern_json) : "dot";
    const auto number = [&](std::string_view key, double fallback) {
        const Json* v = get(*screen, key);
        return v != nullptr ? core::py_float(*v) : fallback;
    };
    if (pattern != "noise") {
        // the greys as dots (or lines) at that screen; solid black and paper stay as they are
        const Json* dpi_json = get(*screen, "dpi");
        const std::int64_t dpi_value = dpi_json != nullptr && core::py_truthy(*dpi_json) ? core::py_int(*dpi_json) : 600;
        if (dpi_value > 1'000'000 || dpi_value < -1'000'000) throw core::Error("value", "the screen's dpi is out of range");
        const auto dpi = static_cast<int>(dpi_value);
        const auto black_at = number("black", 0.1);
        const auto white_at = number("white", 0.95);
        const float white = static_cast<float>(white_at);
        const float span = static_cast<float>(std::max(0.01, white_at - black_at));
        const Json* shape_json = get(*screen, "shape");
        const std::string shape = shape_json != nullptr && core::py_truthy(*shape_json) ? core::py_str(*shape_json) : "round";
        const std::string kind = pattern == "dot" || pattern == "line" || pattern == "cross" ? pattern : "dot";
        const std::vector<float> limit = tones::screen(kind, grey.size(), dpi, number("lpi", 60), number("angle", 45), {0, 0}, shape,
                                                       Box{0, 0, grey.width(), grey.height()});
        const std::string values = grey.tobytes();
        std::string bw(values.size(), '\0');
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float v = static_cast<float>(static_cast<unsigned char>(values[i])) / 255.0f;
            const float cover = std::min(std::max((white - v) / span, 0.0f), 1.0f);
            bw[i] = limit[i] < cover ? '\0' : static_cast<char>(255);
        }
        return Image::frombytes("L", grey.size(), bw).convert("1", Dither::None);
    }
    // (numpy float32 like the Python code: the Python floats meet the float32 array one by one)
    const double black_at = number("black", 0.1);
    const double white_at_d = number("white", 0.95);
    const auto white_at = static_cast<float>(white_at_d);
    const auto span = static_cast<float>(std::max(0.01, white_at_d - black_at));
    const std::string values = grey.tobytes();
    std::string lifted(values.size(), '\0');
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float v = static_cast<float>(static_cast<unsigned char>(values[i])) / 255.0f;
        const float cover = std::min(std::max((white_at - v) / span, 0.0f), 1.0f);
        const float level = std::min(std::max(255 * (1 - cover), 0.0f), 255.0f);
        lifted[i] = static_cast<char>(static_cast<unsigned char>(static_cast<int>(level)));
    }
    const std::string dots = Image::frombytes("L", grey.size(), lifted).convert("1").convert("L").tobytes();
    std::string bw(dots.size(), '\0');
    for (std::size_t i = 0; i < dots.size(); ++i) bw[i] = static_cast<unsigned char>(dots[i]) < 128 ? '\0' : static_cast<char>(255);
    return Image::frombytes("L", grey.size(), bw).convert("1", Dither::None);
}

bool rough_needed(const core::Page& page, int dpi) {
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    std::lock_guard lock(g_cache_mutex);
    for (const core::Layer& layer : page.layers) {
        const std::size_t n = layer.stroke_count();
        if (n < kRoughFrom || !layer.visible) continue;
        const bool guide = guide_role(layer.role);
        std::size_t best = 0;
        bool found = false;
        for (const auto& e : g_stroke_cache) {
            if (e->id == layer.id && e->dpi == dpi && e->size == size && e->guide == guide) {
                found = true;
                best = std::max(best, e->sigs.size());
            }
        }
        if (!found || n - std::min(n, best) >= kRoughFrom) return true;
    }
    return false;
}

void clear_render_caches() {
    text::clear_balloon_cache();
    clear_placed_caches();
    {
        std::lock_guard lock(g_cache_mutex);
        g_stroke_cache.clear();
    }
    {
        std::lock_guard lock(g_mask_mutex);
        g_masks.clear();
    }
    std::lock_guard lock(g_decoded_mutex);
    g_decoded.clear();
}

}  // namespace genko::render
