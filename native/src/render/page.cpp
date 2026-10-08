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
#include <utility>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/pyconv.hpp"
#include "core/placement.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/color_canvas.hpp"
#include "render/effects.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/prims.hpp"
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
constexpr std::int64_t kMaxAreaPixels = 1'500'000'000;

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
        skip_unported(ctx, "placed");  // (left out: as if it had no picture)
    } else if (layer.color_raster) {
        if (is_tone(layer)) throw NotYetPorted("high_precision_tone");
        raster = color_raster_preview(core::ColorRasterView(*layer.color_raster), ctx.size, area);
    } else if (layer.raster_png && !layer.raster_png->empty()) {
        raster = raster_part(layer.raster_png, ctx.size, area);
    }
    auto lines = layer_strokes(ctx, layer, area, panel_mask, raster ? &*raster : nullptr, rough, true);
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

void draw_nombre(Image& image, const Box& area, const Ctx& ctx) {
    const auto items = nombre_items(*ctx.page, ctx.episode);
    if (items.empty()) return;
    const auto text = nombre_text(*ctx.page, ctx.episode), font = nombre_font(ctx.episode);
    for (const auto& item : items) {
        check_cancel(ctx);
        const int size = std::max(6, mm_to_px(item[2], ctx.dpi));
        const auto [origin, sample] = nombre_geometry(text, font, size, item, ctx);
        if (!intersects(sample, area)) continue;
        const Image background = image.crop(shifted(sample, -area.x0, -area.y0)).convert("L");
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

// covers.folds(page) is not empty
bool folds_draw(const Page& page) {
    const Json* cover = core::cover_of(page);
    if (cover == nullptr) return false;
    const std::string k = (*cover)["kind"].get<std::string>();
    if (k != "jacket" && k != "obi") return false;  // (covers.WRAPS)
    const core::Rect t = page.trim_rect_mm();
    const auto mm = [&](std::string_view key) {
        const Json* v = get(*cover, key);
        return (v != nullptr && core::py_truthy(*v)) ? core::py_float(*v) : 0.0;
    };
    const double spine = mm("spine_mm");
    const double flap = mm("flap_mm");
    const double face = (t.width.value() - spine - 2 * flap) / 2;
    return flap > 0 || face > 0 || spine > 0;
}

// The story lines of this page (Episode.story_for_page; a page alone has none here: C++ keeps lines in the book).
std::vector<const core::StoryLine*> lines_of(const Page& page, const core::Document* episode) {
    if (episode == nullptr) return {};
    return episode->story_for_page(page.index);
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
    ColorCanvas own(transparent(area));
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
RenderResult render(const Page& page_in, int dpi, const RenderOptions& options, const core::Document* episode,
                    std::vector<std::string>& omitted) {
    if (options.region && needs_whole_page(page_in)) {
        const Box box = area_of(options, Size{mm_to_px(page_in.spec.width_mm.value(), dpi), mm_to_px(page_in.spec.height_mm.value(), dpi)});
        RenderOptions whole = options;
        whole.region.reset();
        RenderResult out = render(page_in, dpi, whole, episode, omitted);
        out.image = out.image.crop(box);
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
    const Json* anim = get(page.extra, "anim");
    if (anim != nullptr && anim->is_object()) skip_unported(ctx, "anim");  // (left out: the page as it is)
    ctx.finish = options.finish.value_or(page.spec.expression != "color");
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
        precision.emplace(rgba);
    std::optional<Image> prev_alpha;
    const std::optional<Image> panel_mask = clip_mask(page, ctx.size, dpi, area);
    const Image* panel = panel_mask ? &*panel_mask : nullptr;
    // lines set under a layer are drawn just before it
    const std::vector<const core::StoryLine*> lines = lines_of(page, episode);
    std::vector<std::string> below;
    for (const core::StoryLine* line : lines) {
        const Json* under = get(line->style, "below_layer");
        if (under == nullptr || !under->is_string()) continue;
        const std::string id = under->get<std::string>();
        const bool placed = line->x_mm.truthy() || line->y_mm.truthy() || !line->balloon.empty();
        const bool known = std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (!id.empty() && known && placed) below.push_back(id);
    }
    for (const Layer& layer : page.layers) {
        check_cancel(ctx);
        if (std::find(below.begin(), below.end(), layer.id) != below.end()) skip_unported(ctx, "balloons");
        if (layer.kind == LayerKind::Folder) continue;
        if (!layer.visible) continue;
        if (print && !layer.exportable) continue;
        if (guide_role(layer.role) && print) continue;
        if (precision && core::has_color_strokes(layer)) {
            precise_strokes(*precision,ctx,layer,area,panel);continue;
        }
        if (precision && layer.color_raster) {
            if (is_tone(layer)) throw NotYetPorted("high_precision_tone");
            if (!ColorCanvas::supports_blend(layer.blend) || layer.effect || layer.screen || layer.color ||
                !layer.patches.empty() || layer.stroke_count() || layer.panel_clip)
                throw NotYetPorted("high_precision_layer_style");
            std::optional<Image> shown;  // (its mask, as masked() takes it)
            if (layer.mask && layer.mask->png && !layer.mask->png->empty() && layer.mask->enabled)
                shown = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
            precision->blend(core::ColorRasterView(*layer.color_raster), ctx.size, area, layer.opacity, layer.clip, layer.blend,
                             shown ? &*shown : nullptr);
            continue;
        }
        if (precision && !ColorCanvas::supports_blend(layer.blend)) throw NotYetPorted("high_precision_blend:"+layer.blend);
        if (is_tone(layer)) {  // tones sit in the layer order: a layer above can cover them
            if (precision) throw NotYetPorted("high_precision_tone");
            const bool dots = (print && ctx.finish && ctx.dots) || screen_dots(ctx);
            rgba = tones::draw_layer(std::move(rgba), layer, tone_page(ctx), area, panel, dots);
            prev_alpha.reset();
            continue;
        }
        if (layer.kind == LayerKind::Adjust) {  // a correction layer changes what is under it
            if (precision) {
                if (!layer.blend.empty() && layer.blend!="normal")throw NotYetPorted("high_precision_adjustment_blend");
                if (!layer.adjust || !layer.adjust->contains("kind") || (*layer.adjust)["kind"] != "exposure")
                    throw NotYetPorted("high_precision_adjustment");
                std::optional<Image> mask;
                if (layer.mask && layer.mask->enabled && layer.mask->png)
                    mask = decoded(layer.mask->png, "L").resize_region(ctx.size, area, Resample::Bilinear);
                precision->expose(core::Exposure::parse(*layer.adjust), layer.opacity, layer.clip, mask ? &*mask : nullptr);
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
        if (ctx.finish && (print || mode == "proof") && source_kind != nullptr && *source_kind == "psd" &&
            !truthy_json(layer.screen)) {
            // (a picture from a painting app on a monochrome page is finished like placed art)
            const bool whole = area == Box{0, 0, ctx.size.width, ctx.size.height};
            const bool colour = whole ? has_colour(*raster)
                                      : [&] {
                                            const Box all{0, 0, ctx.size.width, ctx.size.height};
                                            const auto full_mask = clip_mask(page, ctx.size, dpi, all);
                                            const auto full = layer_with_effects(ctx, layer, all, full_mask ? &*full_mask : nullptr,
                                                                                 ctx.rough);
                                            return full && has_colour(*full);
                                        }();
            if (colour) skip_unported(ctx, "finish");
        }
        if (truthy_json(layer.screen) && ((print && ctx.dots) || screen_dots(ctx))) {
            raster = screened_layer(ctx, layer, *raster, area, panel);  // トーン化: its greys as dots in print
        }
        Image picture = masked(ctx, layer, std::move(*raster), area);
        if (layer.color && !layer.color->empty() && (!print || layer.color_prints)) picture = tinted(picture, *layer.color);
        const Image* clip = layer.clip && prev_alpha ? &*prev_alpha : nullptr;
        if (precision) precision->blend(picture, layer.opacity, layer.clip, layer.blend);
        else rgba = blend_over(rgba, picture, layer.blend.empty() ? "normal" : layer.blend, layer.opacity, clip);
        prev_alpha = alpha_of(picture);
    }
    Image image = (precision ? precision->image() : rgba).convert("RGB");

    if (core::py_truthy(page.effects)) image = draw_effects(ctx, image, area);  // effect lines (M3-B)
    if (!print && core::py_truthy(page.prims)) draw_prims(image, area, ctx);  // 3D guides, never printed (M3-C)
    if (truthy_json(page.ruler) && name_or_proof) draw_ruler(image, area, ctx);
    draw_frames(image, area, page, ctx.size, dpi);
    if (name_or_proof && get(page.extra, "cover") != nullptr && core::py_truthy(*get(page.extra, "cover")) && folds_draw(page)) {
        skip_unported(ctx, "covers");
    }
    // lines: placed ones in balloons, the others as labels (both drawn with text, in M4)
    if (!lines.empty()) skip_unported(ctx, "balloons");
    if (print || mode == "proof") draw_nombre(image, area, ctx);
    if (options.crop_marks && print) {
        PageCanvas canvas(image, area, ctx.size);
        draw_crop_marks(canvas.draw(), page, dpi);
        canvas.commit();
    }
    if (options.onion && episode != nullptr && name_or_proof && page.onion_from && page.onion_from->truthy()) {
        const Page* prev = nullptr;
        for (const auto& p : episode->pages) {
            if (p->index == *page.onion_from) {
                prev = p.get();
                break;
            }
        }
        if (prev != nullptr) {
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
        }
    }
    return RenderResult{std::move(image), {}};
}

}  // namespace

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

RenderResult render_page(const core::Page& page, int dpi, const RenderOptions& options, const core::Document* episode) {
    std::vector<std::string> omitted;
    // Keep every layer in high precision within a region; quantize only the final output pixels.
    // Reuse the established global-coordinate region renderer, not an RGBA8 layer intermediate.
    if(uses_color_precision(page,options.mode=="print") && !needs_whole_page(page)){
        const Size size{mm_to_px(page.spec.width_mm.value(),dpi),mm_to_px(page.spec.height_mm.value(),dpi)};
        if(size.width>0x7fffffff/4 || size.height>0x7fffffff/4)throw core::Error("image_too_large","the page is too large at this resolution");
        const Box area=area_of(options,size);
        const auto pixels=static_cast<std::int64_t>(area.width())*area.height();
        if(pixels>kMaxAreaPixels)throw core::Error("image_too_large","the picture is too large at this resolution");
        if(pixels>1024LL*1024){
            if(options.stop.stop_requested())throw Cancelled();
            RenderResult out{Image::create_blank("RGB",size_of(area)),{}};
            constexpr int tile=512;
            for(int y=area.y0;y<area.y1;y+=tile){
                for(int x=area.x0;x<area.x1;x+=tile){
                    if(options.stop.stop_requested())throw Cancelled();
                    RenderOptions part=options;part.region=RenderRegion{x,y,std::min(tile,area.x1-x),std::min(tile,area.y1-y)};
                    const auto piece=render(page,dpi,part,episode,omitted);
                    out.image.paste(piece.image,Point{x-area.x0,y-area.y0});
                }
            }
            out.omitted=std::move(omitted);return out;
        }
    }
    RenderResult out = render(page, dpi, options, episode, omitted);
    out.omitted = std::move(omitted);
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
    precise_strokes(canvas,ctx,layer,area,panels?&*panels:nullptr);
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
