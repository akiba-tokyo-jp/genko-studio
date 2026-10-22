#include "render/raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "core/strokes.hpp"
#include "core/brush_dabs.hpp"
#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "render/color_canvas.hpp"
#include "render/draw.hpp"
#include "render/fill.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"
#include "render/stroke.hpp"

namespace genko::render::raster {

namespace {

// f"{page.index:03d}"
std::string index3(const core::Num& index) {
    if (!index.is_int()) throw core::PyValueError("Unknown format code 'd' for object of type 'float'");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%03lld", static_cast<long long>(index.int_value()));
    return buf;
}

// What stamping the eraser along the points costs (stroke.stamp_polyline: a round dab every pixel along each
// segment): refused past the limits, where Python draws for hours.
void check_stamps(const core::PenPoints& points, int dpi, double width_mm) {
    if (points.size() < 2) return;
    double steps_total = 0;
    double work = 0;
    const auto px = [dpi](double mm) {
        const double v = core::py_round_whole(mm / 25.4 * dpi);
        limits::check_coordinate(v, "points");
        return std::max(1.0, v);
    };
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const core::PenPoint& a = points[i];
        const core::PenPoint& b = points[i + 1];
        const double pressure = a.p.value_or(1.0);
        const double radius = std::max(1.0, core::py_round_whole(px(width_mm * core::py_max(0.15, pressure)) / 2.0));
        limits::check_count(radius, limits::kReach, "width_mm");
        const double dx = px(b.x) - px(a.x);
        const double dy = px(b.y) - px(a.y);
        const double steps = std::max(1.0, std::trunc(std::sqrt(dx * dx + dy * dy)));
        steps_total += steps + 1;
        work += (steps + 1) * (2 * radius + 1) * (2 * radius + 1);
    }
    limits::check_count(steps_total, static_cast<double>(limits::kSteps), "the eraser's path");
    limits::check_count(work, limits::kDabWork, "the eraser's path (its length × its width)");
}

// raster._clip: the pixels cut by the panels (every leaf that cuts the layers)
void clip_to_panels(const core::Page& page, Image& image, int dpi) {
    std::vector<const core::Frame*> leaves;
    for (const core::Frame* frame : page.leaf_frames()) {
        if (frame->clip) leaves.push_back(frame);
    }
    if (leaves.empty()) return;
    Image mask = Image::create("L", image.size(), Ink(0));
    {
        Draw draw(mask);
        for (const core::Frame* frame : leaves) detail::fill_frame(draw, *frame, dpi);
    }
    image.putalpha(chops::multiply(image.getchannel(3), mask));
}

// raster.bake_stroke (its defaults: the ink colour, a G pen, 0.35 mm)
void bake_stroke(const core::Page& page, core::Layer& layer, const core::PenPoints& points, int dpi) {
    Image image = ensure_raster(page, layer, dpi);
    const Image old_alpha = image.getchannel(3);
    Image overlay = Image::create("RGBA", image.size(), Ink{0, 0, 0, 0});
    check_stamps(points, dpi, 0.35);
    {
        Draw draw(overlay);
        stamp_polyline(draw, points, dpi, 0.35, Ink{20, 20, 20, 255}, true);
    }
    image = alpha_composite(image, overlay);
    if (layer.lock_alpha) image.putalpha(chops::multiply(image.getchannel(3), old_alpha));
    clip_to_panels(page, image, dpi);
    save_raster(page, layer, image);
    if (layer.kind == core::LayerKind::Strokes) layer.kind = core::LayerKind::Raster;
}

// _bake_vectors' picture: the layer's pixels with its fills and lines drawn in (`page_values`: color-model@1, those of
// an RGB page of a profile, the fills and lines brought into them)
Image baked(const core::Page& page, const core::Layer& layer, bool page_values = false) {
    Image base = ensure_raster(page, layer);
    const Size size = base.size();
    int dpi = kWorkingDpi;
    if (layer.raster_png && !layer.raster_png->empty()) {
        // (the pixels' own resolution)
        const double width_in = page.spec.width_mm.value() / 25.4;
        if (width_in == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
        const double d = core::py_round_whole(size.width / width_in);
        limits::check_count(d, limits::kResolution, "the layer's resolution");
        dpi = static_cast<int>(std::max(1.0, d));
    }
    detail::Ctx ctx;
    std::vector<std::string> omitted;
    ctx.page = &page;
    ctx.dpi = dpi;
    ctx.size = size;
    ctx.mode = "proof";
    ctx.omitted = &omitted;
    std::optional<Image> mask;
    if (layer.panel_clip) mask = detail::clip_mask(page, size, dpi, Box{0, 0, size.width, size.height});
    auto drawn = detail::layer_lines(ctx, layer, mask ? &*mask : nullptr, &base, true);
    if (drawn && page_values && page.rgb) drawn = srgb_into(*drawn, RgbSpace(*page.rgb));
    if (drawn) base = alpha_composite(base, *drawn);
    return base;
}

}  // namespace

Image ensure_raster(const core::Page& page, const core::Layer& layer, int dpi) {
    if (layer.raster_png && !layer.raster_png->empty()) return selection::open_picture(*layer.raster_png).convert("RGBA");
    const int w = mm_to_px(page.spec.width_mm.value(), dpi);
    const int h = mm_to_px(page.spec.height_mm.value(), dpi);
    limits::check_picture(w, h, "the page");
    return Image::create("RGBA", Size{w, h}, Ink{0, 0, 0, 0});
}

std::string page_folder(const core::Page& page) { return "pages/" + index3(page.index); }

void save_raster(const core::Page& page, core::Layer& layer, const Image& image) {
    const std::string folder = page_folder(page);
    layer.raster_png = fills::png_bytes(image);
    layer.raster_relpath = folder + "/" + std::string(core::to_string(layer.role)) + ".png";
}

void bake_vectors(const core::Page& page, core::Layer& layer, bool page_values) {
    const Image base = baked(page, layer, page_values);
    layer.strokes = core::empty_strokes();
    layer.patches.clear();
    layer.shapes = core::vector::empty_shapes();  // (pen-shapes@1, PC3 C-b: its shape parts drawn in too)
    save_raster(page, layer, base);
    layer.kind = core::LayerKind::Raster;
}

Image layer_pixels(const core::Page& page, const core::Layer& layer) {
    // (the same pixels as _bake_vectors on a copy and ensure_raster of it: PNG keeps them)
    if (layer.stroke_count() > 0 || !layer.patches.empty() || shape_parts::any(layer)) {
        core::Layer copy = layer;
        (void)index3(page.index);  // (save_raster's page number)
        return baked(page, copy, core::holds_page_values(page, layer));
    }
    return ensure_raster(page, layer);
}

std::vector<core::Stroke> mixed_pieces(const core::Page& page, const core::Layer& layer, const core::Stroke& stroke, double mix,
                                       double stretch) {
    constexpr double kStepMm = 1.5;
    // The colour under the line, on the layer's grid: its colours (0..255) and alpha (0..1). An 8-bit layer's picture
    // (color-model@1: on an RGB page of a profile its values picked up as the sRGB they show); PC4 E1 (the user's leftover
    // of PC2, design 0.4): a precise layer's own values and an ink layer's colours, in float, the same way.
    int w = 0, h = 0;
    std::string px;
    std::vector<std::array<float, 4>> fine;  // (straight sRGB 0..255 and the alpha 0..1)
    if (layer.color_raster && !page.color) {
        const core::ColorRasterView view(*layer.color_raster);
        w = static_cast<int>(view.width());
        h = static_cast<int>(view.height());
        const RgbSpace space = RgbSpace::of(page);
        fine.resize(std::size_t(w) * std::size_t(h));
        for (std::size_t i = 0; i < fine.size(); ++i) {
            const auto v = view.pixel(i);
            double rgb[3] = {v[0], v[1], v[2]};
            if (!space.is_srgb()) space.to_srgb(rgb, 1);
            fine[i] = {static_cast<float>(rgb[0] * 255.0), static_cast<float>(rgb[1] * 255.0), static_cast<float>(rgb[2] * 255.0), static_cast<float>(v[3])};
        }
    } else if (page.color && layer.ink_raster) {
        const image_ops::Pixels inks = image_ops::from_inks(core::decode_ink_raster(*layer.ink_raster), *page.color);
        w = static_cast<int>(inks.width());
        h = static_cast<int>(inks.height());
        const std::vector<image_ops::Rgba> meant = image_ops::rgba_of(inks);
        fine.resize(meant.size());
        for (std::size_t i = 0; i < meant.size(); ++i) fine[i] = {meant[i][0] * 255.0f, meant[i][1] * 255.0f, meant[i][2] * 255.0f, meant[i][3]};
    } else {
        const Image under = core::holds_page_values(page, layer) ? srgb_from(layer_pixels(page, layer).convert("RGBA"), RgbSpace(*page.rgb))
                                                                  : layer_pixels(page, layer).convert("RGBA");
        px = under.tobytes();
        w = under.width();
        h = under.height();
    }
    const double scale = static_cast<double>(w) / page.spec.width_mm.value();
    std::array<float, 3> base{20.0f, 20.0f, 20.0f};
    if (stroke.rgb && !stroke.rgb->empty()) {
        for (std::size_t i = 0; i < 3 && i < stroke.rgb->size(); ++i) base[i] = static_cast<float>((*stroke.rgb)[i]);
    }
    // (PC4 E1: a precise line's own colour, its color_rgb, mixed in float; the pieces keep theirs so)
    const bool precise = stroke.color_rgb && stroke.color_rgb->is_object() && stroke.color_rgb->contains("values");
    if (precise) {
        for (std::size_t i = 0; i < 3; ++i) base[i] = static_cast<float>((*stroke.color_rgb)["values"][i].get<double>() * 255.0);
    }
    std::array<float, 3> carried = base;
    // (long straight parts get points in between, so the colour can change; brushes@1: the pen's inputs with them)
    std::vector<core::PointF> points{stroke.points.front()};
    std::vector<double> pressure, rotation, tilt, time_ms, wheel;
    if (!stroke.pressure.empty()) pressure.push_back(stroke.pressure.front());
    if (!stroke.rotation.empty()) rotation.push_back(stroke.rotation.front());
    const std::size_t n = stroke.points.size();
    const bool tilted = core::inputs_fit(stroke.tilt, n, 2), timed = core::inputs_fit(stroke.time_ms, n, 1), wheeled = core::inputs_fit(stroke.wheel, n, 1);
    if (tilted) tilt = {stroke.tilt[0], stroke.tilt[1]};
    if (timed) time_ms.push_back(stroke.time_ms.front());
    if (wheeled) wheel.push_back(stroke.wheel.front());
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        const core::PointF a = stroke.points[i - 1], b = stroke.points[i];
        const auto steps = std::max<std::int64_t>(1, static_cast<std::int64_t>(core::py_ceil(core::py_dist(a.x, a.y, b.x, b.y) / (kStepMm / 3))));
        for (std::int64_t k = 1; k <= steps; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(steps);
            points.push_back({core::py_round(a.x + (b.x - a.x) * t, 3), core::py_round(a.y + (b.y - a.y) * t, 3)});
            if (!stroke.pressure.empty() && i < stroke.pressure.size()) {
                pressure.push_back(core::py_round(stroke.pressure[i - 1] + (stroke.pressure[i] - stroke.pressure[i - 1]) * t, 3));
            }
            if (!stroke.rotation.empty() && i < stroke.rotation.size()) {
                rotation.push_back(stroke.rotation[i - 1] + (stroke.rotation[i] - stroke.rotation[i - 1]) * t);
            }
            const auto between = [t](double p, double q) { return p + (q - p) * t; };
            if (tilted) {
                tilt.push_back(core::py_round(between(stroke.tilt[2 * (i - 1)], stroke.tilt[2 * i]), 1));
                tilt.push_back(core::py_round(between(stroke.tilt[2 * (i - 1) + 1], stroke.tilt[2 * i + 1]), 1));
            }
            if (timed) time_ms.push_back(core::py_round(between(stroke.time_ms[i - 1], stroke.time_ms[i]), 1));
            if (wheeled) wheel.push_back(core::py_round(between(stroke.wheel[i - 1], stroke.wheel[i]), 3));
        }
    }
    // numpy's float32 with Python floats as weak scalars (NEP 50): each Python float cast to float32 first
    const float mix_f = static_cast<float>(mix), unmix_f = static_cast<float>(1 - mix);
    std::vector<std::array<float, 3>> colours;
    colours.reserve(points.size());
    core::PointF last = points.front();
    for (const core::PointF& p : points) {
        std::array<float, 3> target = base;
        const auto x = static_cast<std::int64_t>(p.x * scale);  // (int(): toward zero)
        const auto y = static_cast<std::int64_t>(p.y * scale);
        if (0 <= x && x < w && 0 <= y && y < h && !fine.empty()) {
            const auto& at = fine[static_cast<std::size_t>(y) * std::size_t(w) + static_cast<std::size_t>(x)];
            for (int c = 0; c < 3; ++c) target[c] = base[c] + (at[static_cast<std::size_t>(c)] - base[c]) * at[3];
        } else if (0 <= x && x < w && 0 <= y && y < h) {
            const auto* at = reinterpret_cast<const unsigned char*>(px.data()) + (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
            const float alpha = static_cast<float>(at[3]) / 255.0f;  // (see-through places give nothing to pick up)
            for (int c = 0; c < 3; ++c) {
                const float diff = static_cast<float>(at[c]) - base[c];
                const float scaled = diff * alpha;
                target[c] = base[c] + scaled;
            }
        }
        // the colour on the brush moves toward what is under it (or back to its own); stretch keeps it longer
        const double keep = core::py_pow(stretch, std::max(core::py_dist(last.x, last.y, p.x, p.y), 0.05) / 2.0);
        const float keep_f = static_cast<float>(keep), let_f = static_cast<float>(1 - keep);
        std::array<float, 3> colour{};
        for (int c = 0; c < 3; ++c) {
            const float held = carried[c] * keep_f;
            const float taken = target[c] * let_f;
            carried[c] = held + taken;
            const float own = base[c] * unmix_f;
            const float picked = carried[c] * mix_f;
            colour[c] = own + picked;
        }
        last = p;
        colours.push_back(colour);
    }
    std::vector<core::Stroke> pieces;
    std::size_t start = 0;
    double walked = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        walked += core::py_dist(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y);
        if (walked >= kStepMm || i == points.size() - 1) {
            core::Stroke piece = stroke;
            piece.id = pieces.empty() ? stroke.id : core::new_id();
            piece.points.assign(points.begin() + static_cast<std::ptrdiff_t>(start), points.begin() + static_cast<std::ptrdiff_t>(i + 1));
            const auto slice = [&](const std::vector<double>& v) {
                const std::size_t from = std::min(start, v.size()), to = std::min(i + 1, v.size());
                return std::vector<double>(v.begin() + static_cast<std::ptrdiff_t>(from), v.begin() + static_cast<std::ptrdiff_t>(to));
            };
            piece.pressure = slice(pressure);
            piece.rotation = slice(rotation);
            piece.time_ms = slice(time_ms);
            piece.wheel = slice(wheel);
            piece.tilt = tilted ? std::vector<double>(tilt.begin() + static_cast<std::ptrdiff_t>(2 * start), tilt.begin() + static_cast<std::ptrdiff_t>(2 * (i + 1)))
                                : std::vector<double>{};
            // np.mean(colours[start:i + 1], axis=0): each column added in order in float32 (a reduction across rows),
            // then divided by the count as an intp — in float64 — into the float32 result
            std::vector<std::int64_t> rgb;
            std::array<double, 3> exact{};
            const auto count = static_cast<double>(i + 1 - start);
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (std::size_t k = start; k <= i; ++k) sum = sum + colours[k][c];
                const auto mean = static_cast<float>(static_cast<double>(sum) / count);
                exact[static_cast<std::size_t>(c)] = mean;
                rgb.push_back(std::clamp<std::int64_t>(static_cast<std::int64_t>(std::nearbyint(mean)), 0, 255));  // (round(): half to even)
            }
            piece.rgb = rgb;
            if (precise) {  // (PC4 E1: the piece's own precise colour, of the line's precision)
                const std::string precision = (*stroke.color_rgb)["precision"].get<std::string>();
                core::Json values = core::Json::array();
                for (const double v : exact) values.push_back(precision == "u16" ? std::clamp(v / 255.0, 0.0, 1.0) : v / 255.0);
                piece.color_rgb = core::Json{{"precision", precision}, {"values", values}};
            }
            pieces.push_back(std::move(piece));
            start = i;
            walked = 0.0;
        }
    }
    return pieces;
}

void erase_raster(const core::Page& page, core::Layer& layer, const core::PenPoints& points, double width_mm, int dpi,
                  std::string_view texture, const std::string& seed) {
    if (!layer.raster_png || layer.raster_png->empty()) {
        const std::vector<core::StrokePtr> strokes = layer.strokes->items;
        for (const core::StrokePtr& stroke : strokes) bake_stroke(page, layer, core::stroke_points(*stroke), dpi);
    }
    Image image = ensure_raster(page, layer, dpi);
    if (texture != "soft" && texture != "rough") {
        check_stamps(points, dpi, width_mm);
        {
            Draw draw(image);
            stamp_polyline(draw, points, dpi, width_mm, Ink{0, 0, 0, 0}, true);
        }
        save_raster(page, layer, image);
        return;
    }
    const Image mask = eraser_mask(image.size(), points, width_mm, dpi, texture, seed);
    const Image alpha = image.getchannel(3);
    image.putalpha(chops::subtract(alpha, mask));
    save_raster(page, layer, image);
}

void erase_raster_masked(const core::Page& page, core::Layer& layer, int dpi, const Image& mask) {
    if (!layer.raster_png || layer.raster_png->empty()) {
        const std::vector<core::StrokePtr> strokes = layer.strokes->items;
        for (const core::StrokePtr& stroke : strokes) bake_stroke(page, layer, core::stroke_points(*stroke), dpi);
    }
    Image image = ensure_raster(page, layer, dpi);
    const Image alpha = image.getchannel(3);
    image.putalpha(chops::subtract(alpha, mask));
    save_raster(page, layer, image);
}

Image eraser_mask(Size size, const core::PenPoints& points, double width_mm, int dpi, std::string_view texture,
                  const std::string& seed) {
    Image mask = Image::create("L", size, Ink(0));
    const double width = width_mm * (texture == "soft" ? 0.7 : 1.0);
    check_stamps(points, dpi, width);
    {
        Draw draw(mask);
        stamp_polyline(draw, points, dpi, width, Ink(255), true);
    }
    if (texture == "soft") {
        const double radius = core::py_max(1.0, width_mm / 25.4 * dpi / 4);
        limits::check_count(radius, limits::kReach, "width_mm");
        mask = mask.filter(Filter::gaussian_blur(radius));
    } else if (texture == "rough") {
        core::PyRandom rng = core::PyRandom::from_str(seed);
        const int grain = static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(dpi / 100.0)));
        const Size small_size{std::max(1, mask.width() / grain), std::max(1, mask.height() / grain)};
        std::string grains(static_cast<std::size_t>(small_size.width) * static_cast<std::size_t>(small_size.height), '\0');
        for (char& g : grains) g = rng.random() < 0.7 ? static_cast<char>(255) : '\0';
        const Image small = Image::frombytes("L", small_size, grains);
        mask = chops::multiply(mask, small.resize(mask.size(), Resample::Nearest));
    }
    return mask;
}

std::string first_point_repr(const core::PenPoints& points) {
    if (points.empty()) return {};
    const core::PenPoint& p = points.front();
    std::string out = "(" + core::py_float_repr(p.x) + ", " + core::py_float_repr(p.y);
    if (p.p) out += ", " + core::py_float_repr(*p.p);
    return out + ")";
}

}  // namespace genko::render::raster
