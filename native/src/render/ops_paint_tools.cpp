// PC4 節 E (SPEC.md COLOR-09「画素を変える筆」): the ops of the brushes that change pixels — registered by E-0, written by E5:
// dodge and burn (覆い焼きツール・焼き込みツール), sponge (スポンジツール), blur_brush and sharpen_brush (ぼかしツール・シャープ
// ツール), smudge_brush (指先ツール), mixer_brush (混合ブラシツール), background_eraser (背景消しゴムツール) and
// color_replacement (色の置き換えツール). PhotoCraft v0.2.0's crates/engine/src/retouch_cmds.rs (lines 434-575),
// brush_cmds.rs (lines 300-372) and eraser_cmds.rs (lines 125-170), MIT (native/third_party/photocraft/NOTICE.md), with
// Genko's mm, layers and areas: each a stroke of `points` (mm) on a paint layer's own grid with B4's brushwork (diameter,
// hardness, opacity, flow, spacing; render/brushwork) or the book's dab brush `brush` (its tip and dynamics, render/dabs),
// laid dab by dab on the layer's tiles (render/tiled_pixels, retouch::apply_dab_stroke) in the layer's own values on 8-bit,
// precise and ink layers (render/paint_tools). Only the tiles the stroke reaches are read and the pixels it changes
// written back; a stroke of more work than kMaxWork, or past the imaging budget, is refused before it is laid.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/brushes.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/brushwork.hpp"
#include "render/color_canvas.hpp"
#include "render/dabs/lines.hpp"
#include "render/image.hpp"
#include "render/ops_paint_tools.hpp"
#include "render/paint_fill.hpp"
#include "render/paint_tools/effects.hpp"
#include "render/paint_tools/tone.hpp"
#include "render/retouch_dabs.hpp"
#include "render/tiled_pixels.hpp"

namespace genko::render {

core::OpFunction not_yet_pc4(const char* op, const char* group) {
    const std::string message = std::string(op) + " is not in the C++ build yet (PC4 " + group + ")";
    return [message](core::OpContext&) { core::not_yet_ported(message); };
}

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using namespace brushing;
namespace pt = paint_tools;

// The most work a stroke is: its dabs' boxes (each grown by what its dab reads around it, within the layer's grid), all
// told, in pixels of the layer's grid. Two thousand million: a 20 mm brush at 600 dpi across a B4 page at 25 % spacing is
// some 2×10^7, a 50 mm airbrush at 1 % some 10^9; a stroke whose every dab covers the page is refused before it is laid.
constexpr double kMaxWork = 2.0e9;
// How far from the page a point may lie (mm): past it the op says so (the grid's pixels are ints).
constexpr double kFarthest = 100000.0;
// The most dabs a stroke lays (section E 3.6: two hundred thousand a line, a round brush's and a dab brush's alike).
constexpr std::size_t kMostDabs = 200000;

// --- the stroke --------------------------------------------------------------------------------------------------------

// A stroke ready to be laid: the brushwork, the page and the book's features as they were (put back when the stroke
// changes nothing: a layer's lines not made pixels for it), the layer's tiles (none held yet; none at all for a stroke
// off the layer) and a pixel of its kind, its grid, the dabs and the brush's diameter (pixels) and spacing (of the
// diameter), the selection's coverage, the layer's transparency lock.
struct PixelStroke {
    Brushwork b;
    core::PagePtr before;
    std::vector<std::string> features;
    std::unique_ptr<tiled::TiledPixels> target;
    std::unique_ptr<Pixels> like;
    Grid g;
    retouch::DabSequence dabs;
    float diameter = 1.0f;
    float spacing = 0.25f;
    std::unique_ptr<AreaCover> area;
    retouch::SelectionAt selection;
    bool lock = false;
    bool reaches = false;  // (whether a dab reaches the grid at all)
};

// The line seed of a dab brush's jitter (its "seed": a string or a whole number; none: "").
std::string seed_of(const Json& op) {
    const Json* v = field(op, "seed");
    if (v == nullptr) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<std::int64_t>());
    throw OpError("seed must be a string or a whole number");
}

// The dabs of the book's dab brush `kind` along the stroke at the grid's resolution: its tip, dynamics, noise, texture on
// each tip and wet edges (render/dabs), sized by width_mm; the op's hardness, spacing and flow, when given, over the
// brush's.
retouch::DabSequence dab_brush_dabs(const core::Document& doc, const Json& op, const Brushwork& b, const std::string& kind, const Grid& g,
                                    bool own_flow, float& diameter, float& spacing) {
    brushes::follow_book(doc);
    const brushes::Brush brush = brushes::brush(kind);
    if (!brush.dabs) throw OpError("brush must be a dab brush of the book (its engine dabs)");
    core::DabSettings settings = *brush.dabs;
    if (field(op, "hardness") != nullptr) settings.hardness = b.hardness;
    if (field(op, "spacing") != nullptr) settings.spacing = b.spacing;
    const int dpi = static_cast<int>(std::clamp(std::lround(g.sx * 25.4), 1L, 4800L));
    const double px_mm = dpi / 25.4;
    dabs::LineInputs inputs;
    if (own_flow && field(op, "flow") != nullptr) inputs.flow = b.flow;
    inputs.remember = false;
    std::shared_ptr<const dabs::LineDabs> laid;
    std::shared_ptr<const dabs::BrushContext> ctx;
    try {
        laid = dabs::line_dabs(dabs::line_brush(settings, b.width_mm, seed_of(op), inputs), dabs::input_points(b.points, {}, inputs), false);
        ctx = dabs::context(settings, dpi);
    } catch (const core::Error& error) {
        if (error.code() == "too_large") throw OpError(std::string(error.what()) + ": too_large");
        throw;
    }
    retouch::DabSequence out;
    out.rects.reserve(laid->dabs.size());
    for (const dabs::Dab& d : laid->dabs) out.rects.push_back(ctx->dab_rect(d, false));
    out.footprint = [laid, ctx, px_mm](std::size_t i) {
        const dabs::Dab& d = laid->dabs[i];
        retouch::Footprint fp;
        fp.dab.x = d.x * px_mm;
        fp.dab.y = d.y * px_mm;
        fp.dab.radius = static_cast<float>(std::max(d.radius * px_mm, 0.5));
        fp.dab.alpha = d.alpha;
        fp.dab.opacity = d.opacity;
        fp.dab.index = static_cast<std::size_t>(d.index);
        fp.rect = ctx->dab_rect(d, false);
        ctx->rasterize(d, false, fp.rect, fp.cov);  // (its tip × noise × texture × wet edges × its flow)
        return fp;
    };
    diameter = static_cast<float>(std::max(b.width_mm * px_mm, 1.0));
    spacing = static_cast<float>(settings.spacing);
    return out;
}

// The dabs of the stroke on a grid of `size` and their work — counted with `halo_of` (the pixels a dab reads around it,
// by the brush's diameter), refused past kMostDabs and kMaxWork — and whether a dab reaches the grid.
template <class Halo>
void plan(core::OpContext& c, PixelStroke& s, Size size, const std::optional<std::string>& kind, Halo& halo_of, bool own_flow) {
    s.g = grid_of(c.doc.page(s.b.at), size, s.b.id);
    if (kind) {
        s.dabs = dab_brush_dabs(c.doc, c.op, s.b, *kind, s.g, own_flow, s.diameter, s.spacing);
    } else {
        const retouch::Brush round = brush_of(s.b, s.g);
        s.dabs = retouch::round_dabs(round, on_grid(s.b.points, s.g));
        s.diameter = round.size;
        s.spacing = round.spacing;
    }
    if (s.dabs.rects.size() > kMostDabs) {
        throw OpError("a stroke has more than " + std::to_string(kMostDabs) + " dabs (its points lie too far apart for its brush's spacing)");
    }
    const int halo = halo_of(s.diameter);
    double work = 0.0;
    s.reaches = false;
    for (const Box& r : s.dabs.rects) {
        const Box reached = meet(grown(r, halo), s.g.whole());
        if (empty(reached)) continue;
        s.reaches = s.reaches || !empty(meet(r, s.g.whole()));
        work += double(reached.width()) * double(reached.height());
        if (work > kMaxWork) throw OpError("the stroke is too much work to lay at once (its dabs cover more than 2000000000 pixels): too_large");
    }
}

bool precise_first(core::OpContext& c);

// The stroke an op asks for (its brushwork and brush): the layer's grid read from its headers (brushing::unread), the
// dabs and their work counted and refused before the layer is read or its lines drawn into it; a stroke off the layer
// reads nothing. `own_flow` false: the op's "flow" is the tool's own (the Mixer Brush's), the dabs at the brush's.
// `precise`: an empty layer of a page of 16 or 32 bits made precise first (precise_first) when the stroke reaches it.
template <class Halo>
PixelStroke stroke_of(core::OpContext& c, Halo&& halo_of, bool own_flow = true, bool precise = false) {
    PixelStroke s;
    s.b = brushwork(c.doc, c.op);
    s.before = c.doc.pages[s.b.at];
    s.features = c.doc.features;
    if (!own_flow) s.b.flow = 1.0f;
    for (const core::PenPoint& p : s.b.points) {
        if (std::fabs(p.x) > kFarthest || std::fabs(p.y) > kFarthest) throw OpError("points must lie within 100000 mm of the page");
    }
    std::optional<std::string> kind;
    if (const Json* given = field(c.op, "brush")) kind = core::brush_kind(*given, c.doc);
    std::optional<Unread> known = unread(c.doc, s.b.at, s.b.id);  // (refused for a locked layer or one that is not a paint layer)
    if (!known) s.target = std::make_unique<tiled::TiledPixels>(c, s.b.at, s.b.id);  // (only reading tells its grid)
    plan(c, s, known ? known->grid : s.target->grid(), kind, halo_of, own_flow);
    if (s.reaches && precise && precise_first(c)) {  // (made precise: its grid and kind as they are now)
        s.target.reset();
        known = unread(c.doc, s.b.at, s.b.id);
    }
    if (!s.target && known && s.reaches) {
        s.target = std::make_unique<tiled::TiledPixels>(c, s.b.at, s.b.id, known->grid, known->like);
    } else if (!s.target && s.reaches) {
        s.target = std::make_unique<tiled::TiledPixels>(c, s.b.at, s.b.id);
    }
    s.like = std::make_unique<Pixels>(s.target ? s.target->like() : known->like);
    if (!s.reaches) return s;  // (off the layer: nothing read)
    if (s.target->grid().width != s.g.size.width || s.target->grid().height != s.g.size.height) {
        plan(c, s, s.target->grid(), kind, halo_of, own_flow);
    }
    const core::Page& page = c.doc.page(s.b.at);
    if (s.b.area != nullptr) {
        s.area = std::make_unique<AreaCover>(page, *s.b.area, s.g, s.target->kind() == Pixels::Kind::Eight);
        const AreaCover* cover = s.area.get();
        s.selection = [cover](int x, int y) { return cover->at(x, y); };
    }
    s.lock = layer_of(page, s.b.id).lock_alpha;
    return s;
}

int no_halo(float) { return 0; }

// An empty paint layer of a page of 16 or 32 bits made precise before paint is laid on it (as a fill makes it:
// paint_fill::becomes_precise), so the paint stays finer than 8 bits.
bool precise_first(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* id = field(c.op, "layer_id");
    if (id == nullptr || !id->is_string()) return false;
    const std::string wanted = id->get<std::string>();
    bool make = false;
    for (const core::Layer& layer : c.doc.page(at).layers) {
        if (layer.id == wanted) make = !layer.locked && paint_fill::becomes_precise(c.doc.page(at), layer);
    }
    if (make) paint_fill::make_precise(c.doc, at, wanted);
    return make;
}

// The stroke laid with `effect`, mixed back at `opacity`, written back, reported ({"changed", "box_mm"}). A stroke that
// changes nothing leaves the page as it was (review P3-2: a layer's lines are not made pixels, an empty layer not made
// precise, for nothing).
Json lay(core::OpContext& c, PixelStroke& s, float opacity, bool lock, int halo, const retouch::DabEffect& effect) {
    Box changed;
    if (s.reaches) changed = retouch::apply_dab_stroke(*s.target, s.dabs, opacity, s.selection, lock, halo, effect);
    if (!empty(changed)) {
        s.target->put(c.doc);
    } else {
        c.doc.pages[s.b.at] = s.before;
        c.doc.features = s.features;
    }
    return Json{{"changed", !empty(changed)}, {"box_mm", box_mm(changed, s.g)}};
}

// A percentage of the op (lo..hi, clamped) as 0..1.
float percent(const Json& op, const char* key, double fallback, double lo = 1.0, double hi = 100.0) {
    return static_cast<float>(std::clamp(number(op, key, fallback), lo, hi) / 100.0);
}

// --- the colours an op gives ---------------------------------------------------------------------------------------------

// A colour of the op (`key`: [r, g, b] 0..255, "foreground" or "background" — the book's brush colours —, none: `fallback`)
// and, on an RGB page of 16 or 32 bits, its precise values (`precise_key`: {"precision", "values"}, straight sRGB 0..1)
// over it — as straight RGB as the colour formulas see the page (an RGB page's values: its profile's; an ink page's sRGB).
std::array<float, 3> colour_of(core::OpContext& c, const core::Page& page, const char* key, const char* precise_key, bool background) {
    const core::Document& doc = c.doc;
    const auto bytes = [](const std::vector<std::int64_t>& rgb) {
        std::array<double, 3> out{};
        for (std::size_t k = 0; k < 3; ++k) out[k] = static_cast<double>(k < rgb.size() ? std::clamp<std::int64_t>(rgb[k], 0, 255) : 0) / 255.0;
        return out;
    };
    const std::vector<std::int64_t> foreground = doc.brush_rgb.empty() ? std::vector<std::int64_t>{20, 20, 20} : doc.brush_rgb;
    const std::vector<std::int64_t> back = doc.brush_background_rgb.value_or(std::vector<std::int64_t>{255, 255, 255});
    std::array<double, 3> srgb = bytes(background ? back : foreground);
    if (const Json* v = field(c.op, key)) {
        if (*v == Json("foreground")) {
            srgb = bytes(foreground);
        } else if (*v == Json("background")) {
            srgb = bytes(back);
        } else {
            const std::string message = std::string(key) + " is [r, g, b] (0..255), \"foreground\" or \"background\"";
            if (!v->is_array() || v->size() != 3) throw OpError(message);
            for (std::size_t k = 0; k < 3; ++k) {
                if (!(*v)[k].is_number() || (*v)[k].is_boolean() || !((*v)[k].get<double>() >= 0.0 && (*v)[k].get<double>() <= 255.0)) throw OpError(message);
                srgb[k] = (*v)[k].get<double>() / 255.0;
            }
        }
    }
    if (const Json* precise = precise_key != nullptr ? field(c.op, precise_key) : nullptr) {
        const bool deep = !page.color && page.rgb && page.rgb->depth != 8;
        if (deep) {
            try {
                core::validate_stroke_color(*precise);
            } catch (const core::Error& error) {
                throw OpError(std::string(precise_key) + ": " + error.what());
            }
            for (std::size_t k = 0; k < 3; ++k) srgb[k] = (*precise)["values"][k].get<double>();
        } else {
            c.notes.push_back(std::string(precise_key) + " is for an RGB page of 16 or 32 bits: " + key + " is used");
        }
    }
    if (!page.color) RgbSpace::of(page).from_srgb(srgb.data(), 1);  // (the page's values)
    return {static_cast<float>(srgb[0]), static_cast<float>(srgb[1]), static_cast<float>(srgb[2])};
}

// --- Dodge, Burn, Sponge ---------------------------------------------------------------------------------------------------

void dodge_or_burn(core::OpContext& c, bool burn) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const std::string range = choice(c.op, "range", {"midtones", "shadows", "highlights"});
    const pt::ToneRange tones = range == "shadows" ? pt::ToneRange::Shadows : range == "highlights" ? pt::ToneRange::Highlights : pt::ToneRange::Midtones;
    const float exposure = percent(c.op, "exposure", 50.0);
    const bool protect = flag(c.op, "protect_tones", true);
    PixelStroke s = stroke_of(c, no_halo);
    const pt::Colours colours(*s.like);
    const float spacing = s.spacing;
    c.report = lay(c, s, s.b.opacity, s.lock, 0, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) {
        pt::colour_dab(work, fp, colours, exposure, spacing, true,
                       [&](const std::array<float, 3>& rgb, float k) { return pt::dodge_burn(rgb, k, tones, burn, protect); });
    });
}

void dodge(core::OpContext& c) { dodge_or_burn(c, false); }
void burn(core::OpContext& c) { dodge_or_burn(c, true); }

void sponge(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const bool saturate = choice(c.op, "mode", {"desaturate", "saturate"}) == "saturate";
    const bool vibrance = flag(c.op, "vibrance", true);
    PixelStroke s = stroke_of(c, no_halo);
    const pt::Colours colours(*s.like);
    const float spacing = s.spacing;
    // (the flow is in each dab's coverage: a pass at full flow moves the colours half-way, upstream's)
    c.report = lay(c, s, s.b.opacity, s.lock, 0, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) {
        pt::colour_dab(work, fp, colours, 0.5f, spacing, true,
                       [&](const std::array<float, 3>& rgb, float k) { return pt::sponge(rgb, k, saturate, vibrance); });
    });
}

// --- Blur, Sharpen, Smudge -------------------------------------------------------------------------------------------------

void focus(core::OpContext& c, bool sharpen) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const float strength = percent(c.op, "strength", 50.0);
    const bool protect = flag(c.op, "protect_detail", true);
    PixelStroke s = stroke_of(c, [](float diameter) { return pt::focus_halo(pt::focus_sigma(diameter)); });
    const float sigma = pt::focus_sigma(s.diameter);
    const int halo = pt::focus_halo(sigma);
    const float spacing = s.spacing;
    c.report = lay(c, s, s.b.opacity, s.lock, halo, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) {
        pt::focus_dab(work, fp, sharpen, sigma, strength, spacing, protect);
    });
}

void blur_brush(core::OpContext& c) { focus(c, false); }
void sharpen_brush(core::OpContext& c) { focus(c, true); }

void smudge_brush(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const float strength = percent(c.op, "strength", 50.0);
    const bool finger = flag(c.op, "finger_painting", false);
    // (finger painting on an empty layer of a page of 16 or 32 bits: made precise first, finer than 8 bits)
    PixelStroke s = stroke_of(c, [](float diameter) { return static_cast<int>(std::ceil(diameter / 2.0f)) + 2; }, true, finger);
    std::optional<std::vector<float>> start;
    if (finger) {
        const pt::Colours colours(*s.like);
        std::vector<float> v = colours.values_of(colour_of(c, c.doc.page(s.b.at), "rgb", "color_rgb", false));
        v.push_back(1.0f);
        start = std::move(v);
    }
    retouch::Smudge smudge(strength, std::move(start), s.diameter);
    const Box whole = s.g.whole();
    c.report = lay(c, s, s.b.opacity, s.lock, smudge.halo(), [&](tiled::TiledPixels& work, const retouch::Footprint& fp) {
        if (!empty(meet(fp.rect, whole))) smudge.dab(work, fp);  // (a dab off the layer picks nothing up: the first on it starts the paint)
    });
}

// --- the Mixer Brush -------------------------------------------------------------------------------------------------------

std::optional<std::array<float, 4>> rgba_of_json(const Json& v, const char* what) {
    if (v.is_null()) return std::nullopt;
    const std::string message = std::string("reservoir.") + what + " is [r, g, b, a] (finite numbers) or null";
    if (!v.is_array() || v.size() != 4) throw OpError(message);
    std::array<float, 4> out{};
    for (std::size_t k = 0; k < 4; ++k) {
        if (!v[k].is_number() || v[k].is_boolean() || !std::isfinite(v[k].get<double>()) || std::fabs(v[k].get<double>()) > 1e6) throw OpError(message);
        out[k] = static_cast<float>(v[k].get<double>());
    }
    return out;
}

Json rgba_json(const std::optional<std::array<float, 4>>& c) {
    if (!c) return nullptr;
    Json out = Json::array();
    for (const float v : *c) out.push_back(std::round(double(v) * 1e6) / 1e6);
    return out;
}

pt::Reservoir reservoir_of(const Json& op) {
    pt::Reservoir r;
    const Json* v = field(op, "reservoir");
    if (v == nullptr) return r;
    if (!v->is_object()) throw OpError("reservoir is {paint, amount, pickup} (a mixer_brush's result) or null");
    for (const auto& [key, value] : v->items()) {
        if (key != "paint" && key != "amount" && key != "pickup") throw OpError("reservoir takes paint, amount and pickup, not " + key);
    }
    if (v->contains("paint")) r.paint = rgba_of_json((*v)["paint"], "paint");
    if (v->contains("pickup")) r.pickup = rgba_of_json((*v)["pickup"], "pickup");
    r.amount = static_cast<float>(std::clamp(number(*v, "amount", r.paint ? 1.0 : 0.0), 0.0, 1.0));
    return r;
}

void mixer_brush(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    pt::MixerSettings m;
    m.wet = percent(c.op, "wet", 50.0, 0.0);
    m.load = percent(c.op, "load", 50.0, 0.0);
    m.mix = percent(c.op, "mix", 50.0, 0.0);
    m.flow = percent(c.op, "flow", 100.0, 0.0);
    const bool sample_all = flag(c.op, "sample_all", false);
    const bool clean_after = flag(c.op, "clean_after", true);
    const bool load_after = flag(c.op, "load_after", true);
    pt::Reservoir reservoir = reservoir_of(c.op);
    // (the op's flow is the Mixer Brush's own: how much of the paint lands; an empty layer of a page of 16 or 32 bits made
    // precise first: the paint finer than 8 bits)
    PixelStroke s = stroke_of(c, no_halo, false, true);
    const core::Page& page = c.doc.page(s.b.at);
    const std::array<float, 3> loaded = colour_of(c, page, "rgb", "color_rgb", false);
    if (load_after || !reservoir.paint) {  // (loaded with the 描画色 before the stroke: ストローク後にブラシを補充, or the first)
        reservoir.paint = std::array<float, 4>{loaded[0], loaded[1], loaded[2], 1.0f};
        reservoir.amount = 1.0f;
    }
    const pt::Colours colours(*s.like);
    // (全レイヤーを対象: the page as it shows over the stroke's box, before the stroke, its colours as the formulas see them)
    std::vector<image_ops::Rgba> sample;
    Box sample_box;
    std::optional<image_ops::Held> sample_held;
    if (sample_all && s.reaches) {
        for (const Box& r : s.dabs.rects) {
            const Box in = meet(r, s.g.whole());
            if (empty(in)) continue;
            sample_box = empty(sample_box) ? in : Box{std::min(sample_box.x0, in.x0), std::min(sample_box.y0, in.y0), std::max(sample_box.x1, in.x1),
                                                      std::max(sample_box.y1, in.y1)};
        }
        sample_held.emplace(std::uint64_t(sample_box.width()) * std::uint64_t(sample_box.height()) * sizeof(image_ops::Rgba));
        const Pixels shown = shown_window(c.doc, s.b.at, s.b.id, Sample::All, s.g, sample_box, *s.like);
        sample = image_ops::rgba_of(shown);
    }
    pt::Mixer mixer(colours, m, reservoir, s.lock, std::move(sample), sample_box);
    // (the mixing's own flow and its paint's ceiling: the dabs land at full opacity, upstream's)
    Json report = lay(c, s, 1.0f, s.lock, 0, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) { mixer.dab(work, fp); });
    pt::Reservoir after = mixer.reservoir();
    if (clean_after) after.pickup.reset();  // (ストローク後にブラシをクリーン: the colour picked up is let go)
    report["reservoir"] = Json{{"paint", rgba_json(after.paint)}, {"amount", std::round(double(after.amount) * 1e6) / 1e6}, {"pickup", rgba_json(after.pickup)}};
    c.report = std::move(report);
}

// --- the Background Eraser, Color Replacement --------------------------------------------------------------------------------

pt::Sampling sampling_of(const Json& op) {
    const std::string s = choice(op, "sampling", {"continuous", "once", "swatch"});
    return s == "once" ? pt::Sampling::Once : s == "swatch" ? pt::Sampling::Swatch : pt::Sampling::Continuous;
}

pt::Limits limits_of(const Json& op) {
    const std::string s = choice(op, "limits", {"contiguous", "discontiguous", "find_edges"});
    return s == "discontiguous" ? pt::Limits::Discontiguous : s == "find_edges" ? pt::Limits::FindEdges : pt::Limits::Contiguous;
}

void background_eraser(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const pt::Sampling sampling = sampling_of(c.op);
    const pt::Limits limits = limits_of(c.op);
    const float tolerance = percent(c.op, "tolerance", 50.0, 0.0);
    const bool protect = flag(c.op, "protect_foreground", false);
    {
        const std::size_t at = core::require_page(c.doc, c.op);
        const Json* id = field(c.op, "layer_id");
        if (id != nullptr && id->is_string() && layer_of(c.doc.page(at), id->get<std::string>()).lock_alpha)
            throw OpError("the layer's transparency is locked: the Background Eraser takes away its alpha");
    }
    PixelStroke s = stroke_of(c, no_halo);
    const core::Page& page = c.doc.page(s.b.at);
    const pt::Colours colours(*s.like);
    std::vector<float> fore = colours.values_of(colour_of(c, page, "foreground_rgb", nullptr, false));
    std::vector<float> swatch = colours.values_of(colour_of(c, page, "swatch_rgb", nullptr, true));
    pt::BackgroundEraser eraser(sampling, limits, tolerance, protect, std::move(fore), std::move(swatch));
    // (no opacity: Photoshop's 背景消しゴムツール has none; the selection limits it)
    c.report = lay(c, s, 1.0f, false, 0, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) { eraser.dab(work, fp); });
}

void color_replacement(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const std::string mode = choice(c.op, "mode", {"color", "hue", "saturation", "luminosity"});
    const pt::Sampling sampling = sampling_of(c.op);
    const pt::Limits limits = limits_of(c.op);
    const float tolerance = percent(c.op, "tolerance", 30.0, 0.0);
    const bool antialias = flag(c.op, "antialias", true);
    PixelStroke s = stroke_of(c, no_halo);
    const core::Page& page = c.doc.page(s.b.at);
    const pt::Colours colours(*s.like);
    const std::array<float, 3> colour = colour_of(c, page, "rgb", "color_rgb", false);
    const std::array<float, 3> swatch = colour_of(c, page, "swatch_rgb", nullptr, true);
    if (!s.reaches) {  // (off the layer: nothing to sample, nothing changed)
        c.report = lay(c, s, s.b.opacity, s.lock, 0, [](tiled::TiledPixels&, const retouch::Footprint&) {});
        return;
    }
    pt::ColorReplacement replacing(colours, *s.target, mode, sampling, limits, tolerance, antialias, colour, swatch);
    c.report = lay(c, s, s.b.opacity, s.lock, 0, [&](tiled::TiledPixels& work, const retouch::Footprint& fp) { replacing.dab(work, fp); });
}

}  // namespace

void register_paint_tool_ops(core::OpRegistry& registry) {
    // the brushes' common keys (B4's brushwork): page, layer_id, points ([[x_mm, y_mm, pressure?]…]), width_mm, hardness,
    // opacity, flow, spacing (%), area?; brush? (a dab brush of the book: its tip and dynamics), seed?; then each tool's own
    registry.add("dodge", dodge);                    // (+ range: midtones | shadows | highlights, exposure 1..100, protect_tones)
    registry.add("burn", burn);                      // (the same)
    registry.add("sponge", sponge);                  // (+ mode: desaturate | saturate, vibrance)
    registry.add("blur_brush", blur_brush);          // (+ strength 1..100)
    registry.add("sharpen_brush", sharpen_brush);    // (+ strength, protect_detail)
    registry.add("smudge_brush", smudge_brush);      // (+ strength, finger_painting, rgb?, color_rgb?)
    registry.add("mixer_brush", mixer_brush);        // (+ wet, load, mix, flow 0..100, rgb?, color_rgb?, reservoir?, sample_all, clean_after,
                                                     //  load_after → {"reservoir"})
    registry.add("background_eraser", background_eraser);  // (+ sampling: continuous | once | swatch, limits: contiguous | discontiguous |
                                                           //  find_edges, tolerance 0..100, protect_foreground, foreground_rgb?, swatch_rgb?)
    registry.add("color_replacement", color_replacement);  // (+ mode: color | hue | saturation | luminosity, sampling, limits, tolerance,
                                                           //  antialias, rgb?, color_rgb?, swatch_rgb?)
}

}  // namespace genko::render
