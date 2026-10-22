#include "render/dab_ops.hpp"

#include <algorithm>
#include <cmath>

#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/stroke_geom.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/color_canvas.hpp"
#include "render/dabs/lines.hpp"
#include "render/image_ops.hpp"
#include "render/raster.hpp"
#include "render/tiled_pixels.hpp"

namespace genko::render::dab_ops {

namespace {

using core::Json;
using core::OpError;

const std::vector<std::int64_t> kInkColor{20, 20, 20};

std::vector<std::int64_t> line_rgb(const core::Stroke& stroke, const brushes::Brush& b) {
    if (stroke.rgb && !stroke.rgb->empty()) return *stroke.rgb;
    if (b.rgb && !b.rgb->empty()) return *b.rgb;
    return kInkColor;
}

double line_width(const core::Stroke& s) { return s.width_mm != 0.0 ? s.width_mm : 0.35; }

// The dabs of a dab brush's line counted (and its width bounded) before it is kept.
void check_line(const core::Document& doc, const core::Stroke& stroke) {
    brushes::follow_book(doc);
    const brushes::Brush b = brushes::brush(stroke.kind);
    if (!b.dabs) return;
    if (line_width(stroke) > 500.0) throw OpError("a dab brush's line is at most 500 mm wide (width_mm)");
    try {
        const dabs::LineInputs inputs = brushes::line_inputs(stroke, line_rgb(stroke, b));
        (void)dabs::line_dabs(dabs::line_brush(*b.dabs, line_width(stroke), stroke.id, inputs),
                              dabs::input_points(core::stroke_points(stroke), stroke.rotation, inputs));
    } catch (const core::Error& error) {
        if (error.code() == "too_large") throw OpError(std::string(error.what()) + ": too_large");
        throw;
    }
}

// The square tip of the block eraser (its larger side the dab's diameter: a full square, aliased).
std::shared_ptr<const dabs::Mips> block_tip() {
    static const auto tip = std::make_shared<const dabs::Mips>(dabs::GrayTile::from_fn(16, 16, [](std::uint32_t, std::uint32_t) { return 1.0f; }));
    return tip;
}

}  // namespace

void blend_line(core::OpContext& c, std::size_t at, const std::string& id, const core::Stroke& stroke, const std::string& blend) {
    if (!image_ops::is_blend(blend)) throw OpError("blend is not one of the blends (normal, multiply, screen, …)");
    brushes::follow_book(c.doc);
    const brushes::Brush b = brushes::brush(stroke.kind);
    const Size grid = tiled::TiledPixels(c, at, id).grid();  // (refused for a locked layer or one that is not a paint layer)
    const core::Page& page = c.doc.page(at);
    const double width_mm = page.spec.width_mm.value();
    if (!(width_mm > 0)) throw OpError("the page has no size");
    const int dpi = std::max(1, static_cast<int>(std::lround(grid.width * 25.4 / width_mm)));
    const std::vector<std::int64_t> rgb = line_rgb(stroke, b);
    const dabs::LineInputs inputs = brushes::line_inputs(stroke, rgb);
    const core::PenPoints points = core::stroke_points(stroke);
    const auto drawn = b.dabs ? brushes::draw(grid, points, dpi, line_width(stroke), stroke.kind, stroke.id, stroke.rotation, stroke.pressure_opacity, &inputs)
                              : brushes::draw(grid, points, dpi, line_width(stroke), stroke.kind, stroke.id, stroke.rotation, stroke.pressure_opacity);
    if (!drawn) return;
    const Box box{std::max(0, drawn->origin.x), std::max(0, drawn->origin.y), std::min(grid.width, drawn->origin.x + drawn->mask.width()),
                  std::min(grid.height, drawn->origin.y + drawn->mask.height())};
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return;
    const std::string mask = drawn->mask.tobytes();
    const int mw = drawn->mask.width();
    const auto cover_at = [&](int x, int y) {
        if (drawn->dabs && drawn->fine) return drawn->dabs->at(x, y);
        return static_cast<unsigned char>(mask[std::size_t(y - drawn->origin.y) * std::size_t(mw) + std::size_t(x - drawn->origin.x)]) / 255.0f;
    };
    // (only the layer's tiles the line covers are read)
    std::vector<Box> wanted;
    for (const Box& t : tiled::tiles_reached(grid, {box})) {
        bool any = false;
        for (int y = std::max(t.y0, box.y0); y < std::min(t.y1, box.y1) && !any; ++y)
            for (int x = std::max(t.x0, box.x0); x < std::min(t.x1, box.x1) && !any; ++x) any = cover_at(x, y) > 0.0f;
        if (any) wanted.push_back(t);
    }
    if (wanted.empty()) return;
    std::vector<Box> boxes;
    std::vector<image_ops::Pixels> parts = image_ops::layer_pixels(c, at, id, wanted, &boxes);
    const core::Layer& layer = c.doc.page(at).layers[core::layer_by_id(c.doc.page(at), id)];
    const bool keep_alpha = layer.lock_alpha;
    const bool inks = !parts.empty() && parts.front().kind() == image_ops::Pixels::Kind::Inks;
    const RgbSpace space = RgbSpace::of(c.doc.page(at));
    const auto values = [&](std::array<float, 3> srgb) {
        double v[3] = {srgb[0], srgb[1], srgb[2]};
        if (!inks) space.from_srgb(v, 1);  // (the page's values)
        return std::array<float, 3>{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
    };
    const std::array<float, 3> colour = values(inputs.color);
    const bool coloured = drawn->dabs && drawn->dabs->coloured();
    const auto opacity = static_cast<float>(core::py_clamp(stroke.opacity, 0.0, 1.0) * b.opacity);
    std::vector<std::vector<std::uint8_t>> marks(parts.size());
    for (std::size_t n = 0; n < parts.size(); ++n) {
        image_ops::Pixels& p = parts[n];
        const Box& pb = boxes[n];
        std::vector<std::uint8_t>& only = marks[n];
        only.assign(std::size_t(pb.width()) * std::size_t(pb.height()), 0);
        std::vector<image_ops::Rgba> px;
        if (inks) px = image_ops::rgba_of(p);
        for (int y = std::max(pb.y0, box.y0); y < std::min(pb.y1, box.y1); ++y) {
            for (int x = std::max(pb.x0, box.x0); x < std::min(pb.x1, box.x1); ++x) {
                const float m = cover_at(x, y);
                if (m <= 0.0f) continue;
                const std::size_t k = std::size_t(y - pb.y0) * std::size_t(pb.width()) + std::size_t(x - pb.x0);
                const std::array<float, 3> src = coloured ? values(drawn->dabs->colour(x, y)) : colour;
                float* v = inks ? px[k].data() : p.at(k);
                const image_ops::Rgba backdrop{v[0], v[1], v[2], v[3]};
                image_ops::Rgba out = image_ops::composite(blend, backdrop, image_ops::Rgba{src[0], src[1], src[2], m}, opacity);
                if (keep_alpha) out[3] = backdrop[3];
                for (std::size_t ch = 0; ch < 4; ++ch) v[ch] = out[ch];
                only[k] = 1;
            }
        }
        if (inks) image_ops::from_rgba(p, px);
    }
    std::vector<image_ops::PixelsPart> written;
    for (std::size_t n = 0; n < parts.size(); ++n) written.push_back(image_ops::PixelsPart{boxes[n], &parts[n], &marks[n]});
    image_ops::put_layer_pixels(c.doc, at, id, written);
}

Image eraser_mask(Size size, const core::PenPoints& points, double width_mm, int dpi, const Eraser& eraser, const std::string& seed) {
    Image out = Image::create("L", size, Ink(0));
    if (points.empty()) return out;
    core::DabSettings settings;
    std::shared_ptr<const dabs::BrushContext> ctx;
    if (eraser.block) {
        settings.aliased = true;
        settings.hardness = 1.0;
        settings.pressure_size = false;
        settings.spacing = 0.1;
        ctx = std::make_shared<const dabs::BrushContext>(settings, block_tip(), nullptr, nullptr, dpi);
    } else {
        const brushes::Brush b = brushes::brush(eraser.brush);
        if (!b.dabs) throw OpError("brush must be a dab brush of the book (its engine dabs)");
        settings = *b.dabs;
        ctx = dabs::context(settings, dpi);
    }
    dabs::LineInputs inputs;
    inputs.flow = eraser.flow;
    const auto set = dabs::line_dabs(dabs::line_brush(settings, width_mm, seed, inputs), dabs::input_points(points, {}, inputs));
    const auto laid = dabs::lay(ctx, set->dabs, set->dual, Box{0, 0, size.width, size.height}, false);
    if (laid->box.x1 <= laid->box.x0 || laid->box.y1 <= laid->box.y0) return out;
    std::vector<float> values;
    laid->fill(laid->box, values);
    const auto k = static_cast<float>(std::clamp(eraser.opacity, 0.0, 1.0));
    std::string bytes(values.size(), '\0');
    for (std::size_t i = 0; i < values.size(); ++i) bytes[i] = static_cast<char>(static_cast<int>(values[i] * k * 255.0f + 0.5f));
    out.paste(Image::frombytes("L", Size{laid->box.width(), laid->box.height()}, bytes), Point{laid->box.x0, laid->box.y0});
    return out;
}

std::optional<Eraser> eraser_of(const Json& op, const core::Document& doc, std::vector<std::string>& notes) {
    const Json* brush = core::get(op, "brush");
    const Json* block = core::get(op, "block");
    const bool has_brush = brush != nullptr && !brush->is_null();
    const bool has_block = block != nullptr && !block->is_null() && core::py_truthy(*block);
    if (!has_brush && !has_block) {
        for (const char* key : {"opacity", "flow"}) {
            if (const Json* v = core::get(op, key); v != nullptr && !v->is_null()) {
                notes.push_back(std::string(key) + " is the eraser's brush's (brush, a dab brush of the book, or block): not used");
            }
        }
        return std::nullopt;
    }
    if (has_brush && has_block) throw OpError("the eraser is a brush or a block, not both");
    Eraser out;
    out.block = has_block;
    if (has_brush) {
        out.brush = core::brush_kind(*brush, doc);
        if (!core::brush_for(Json(out.brush), doc).dabs) throw OpError("brush must be a dab brush of the book (its engine dabs)");
    }
    const auto unit = [&](const char* key) -> std::optional<double> {
        const Json* v = core::get(op, key);
        if (v == nullptr || v->is_null()) return std::nullopt;
        if (!v->is_number() || v->is_boolean() || !(v->get<double>() >= 0.0 && v->get<double>() <= 1.0)) {
            throw OpError(std::string(key) + " is a number between 0 and 1");
        }
        return v->get<double>();
    };
    out.opacity = unit("opacity").value_or(1.0);
    out.flow = unit("flow");
    return out;
}

core::StrokeHooks stroke_hooks() {
    return core::StrokeHooks{raster::mixed_pieces, check_line, blend_line};
}

core::DabTakeIn take_in() {
    core::DabTakeIn hooks;
    hooks.picture = [](std::string_view file, bool ink, std::uint32_t edge) {
        auto made = dabs::take_in_picture(file, ink, edge);
        dabs::keep_picture(made.first, made.second);
        return made;
    };
    hooks.photocraft = [](const Json& upstream, std::map<std::string, core::Bytes>& pictures, std::vector<std::string>& ignored) {
        Json out = dabs::dabs_from_photocraft(upstream, pictures, ignored);
        for (const auto& [ref, png] : pictures) dabs::keep_picture(ref, png);
        return out;
    };
    return hooks;
}

}  // namespace genko::render::dab_ops
