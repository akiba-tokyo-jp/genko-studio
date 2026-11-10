#include "service/guide.hpp"

#include <algorithm>
#include <cmath>

#include "core/mannequin.hpp"
#include "core/pynum.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/text/fonts.hpp"
#include "service/blocking.hpp"
#include "service/briefs.hpp"

namespace genko::service::guide {

namespace {

using render::BoxF;
using render::Draw;
using render::Ink;
using render::PointD;

BoxF as_boxf(const std::array<double, 4>& b) { return BoxF{b[0], b[1], b[2], b[3]}; }

Box mm_box(const NumBox& b) { return Box{b[0].value(), b[1].value(), b[2].value(), b[3].value()}; }

std::vector<Figure> figures_for(const core::Frame& frame) {
    const NumBox rect{frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height};
    return figures(frame.panel ? *frame.panel : Json::object(), rect);
}

const core::Layer* name_layer(const core::Page& page) {
    for (const core::Layer& layer : page.layers) {
        if (layer.role == core::LayerRole::Name) return &layer;
    }
    return nullptr;
}

std::array<double, 4> frame_rect(const core::Frame& frame) {
    return {frame.rect.x.value(), frame.rect.y.value(), frame.rect.width.value(), frame.rect.height.value()};
}

}  // namespace

GenBox GenBox::for_frame(const core::Frame& frame, double pad_mm, std::array<int, 2> px) {
    const core::Rect& r = frame.rect;
    return GenBox{r.x.value() - pad_mm, r.y.value() - pad_mm, r.width.value() + 2 * pad_mm, r.height.value() + 2 * pad_mm, px};
}

std::array<double, 2> GenBox::to_px(double x_mm, double y_mm) const {
    return {(x_mm - x) * px[0] / width, (y_mm - y) * px[1] / height};
}

std::array<double, 4> GenBox::box_px(const Box& box) const {
    const auto a = to_px(box[0], box[1]);
    const auto b = to_px(box[0] + box[2], box[1] + box[3]);
    return {a[0], a[1], b[0], b[1]};
}

Json GenBox::box01(const Box& box) const {
    const double x0 = (box[0] - x) / width;
    const double y0 = (box[1] - y) / height;
    const double w = box[2] / width;
    const double h = box[3] / height;
    return Json::array({core::py_round(core::py_max(0.0, x0), 3), core::py_round(core::py_max(0.0, y0), 3),
                        core::py_round(core::py_min(1.0, w), 3), core::py_round(core::py_min(1.0, h), 3)});
}

int line_width(const GenBox& box, double mm) { return std::max(1, static_cast<int>(core::py_round_int(mm * box.px[0] / box.width))); }

std::vector<std::pair<Box, std::string>> keepout_boxes(const core::Document& doc, const core::Page& page, const core::Frame& frame,
                                                       double grow_mm) {
    std::vector<std::pair<Box, std::string>> out;
    std::size_t i = 0;
    for (const core::StoryLine* line : doc.story_for_page(page.index)) {
        if (!line->frame_id || *line->frame_id != frame.id) continue;
        const Box box{line->x_mm.value() - grow_mm, line->y_mm.value() - grow_mm, line->w_mm.value() + 2 * grow_mm,
                      line->h_mm.value() + 2 * grow_mm};
        out.emplace_back(box, "台詞（読み順 " + std::to_string(++i) + "）");
    }
    return out;
}

bool has_mannequin(const core::Page& page, const core::Frame& frame) {
    for (const Json& p : core::iterate(page.prims)) {
        if (core::py_equals(core::py_get(p, "kind"), Json("mannequin")) && core::mannequin::in_rect(p, frame_rect(frame))) return true;
    }
    return false;
}

render::Image composition(const core::Page& page, const core::Frame& frame, const GenBox& box) {
    render::Image image = render::Image::create("L", render::Size{box.px[0], box.px[1]}, Ink(255));
    Draw draw(image);
    const int lw = line_width(box, 0.5);
    draw.rectangle(as_boxf(box.box_px(frame_rect(frame))), std::nullopt, Ink(0), lw);
    if (const core::Layer* layer = name_layer(page); layer != nullptr && layer->strokes) {
        for (const core::StrokePtr& stroke : layer->strokes->items) {
            std::vector<PointD> points;
            for (const core::PointF& p : stroke->points) {
                const auto at = box.to_px(p.x, p.y);
                points.push_back(PointD{at[0], at[1]});
            }
            if (points.size() >= 2) draw.line(points, Ink(0), line_width(box, core::py_max(0.3, stroke->width_mm)));
        }
    }
    for (const Figure& fig : figures_for(frame)) {
        const auto h = box.box_px(mm_box(fig.head));
        draw.ellipse(BoxF{h[0], h[1], h[2], h[3]}, std::nullopt, Ink(0), lw);
        const auto b = box.box_px(mm_box(fig.body));
        const double neck = h[3];
        const std::array<PointD, 2> spine{PointD{(h[0] + h[2]) / 2, neck}, PointD{(h[0] + h[2]) / 2, b[3]}};
        draw.line(spine, Ink(0), lw);
        const double shoulders = neck + (b[3] - neck) * 0.15;
        const std::array<PointD, 2> across{PointD{b[0], shoulders}, PointD{b[2], shoulders}};
        draw.line(across, Ink(0), lw);
    }
    return image;
}

render::Image pose(const core::Page& page, const core::Frame& frame, const GenBox& box) {
    render::Image image = render::Image::create("L", render::Size{box.px[0], box.px[1]}, Ink(255));
    Draw draw(image);
    const int lw = line_width(box, 0.6);
    PyDict chars;
    for (const Json& c : core::iterate(core::py_get(frame.panel ? *frame.panel : Json::object(), "characters", Json::array()))) {
        if (c.is_object()) chars.set(core::py_get(c, "id"), c);
    }
    render::TrueTypeFonts fonts;
    const render::TrueTypeFont& font = fonts.load_default();
    for (const Figure& fig : figures_for(frame)) {
        draw.rectangle(as_boxf(box.box_px(mm_box(fig.body))), std::nullopt, Ink(128), lw);
        const auto h = box.box_px(mm_box(fig.head));
        draw.ellipse(BoxF{h[0], h[1], h[2], h[3]}, Ink(200), Ink(0), lw);
        const double cx = (h[0] + h[2]) / 2;
        const double cy = (h[1] + h[3]) / 2;
        const Json* char_entry = chars.find(fig.char_id);
        const Json facing = char_entry != nullptr ? core::py_get(*char_entry, "facing", Json("front")) : Json("front");
        const double length = (h[2] - h[0]) * 0.9;
        if (core::is_one_of(facing, {"left", "right"})) {
            const int sign = facing == "left" ? -1 : 1;
            const PointD tip{cx + sign * length, cy};
            const std::array<PointD, 2> shaft{PointD{cx, cy}, tip};
            draw.line(shaft, Ink(0), lw);
            const std::array<PointD, 3> head{tip, PointD{tip.x - sign * lw * 4, cy - lw * 3}, PointD{tip.x - sign * lw * 4, cy + lw * 3}};
            draw.polygon(head, Ink(0));
        } else if (core::py_equals(facing, Json("front"))) {
            draw.ellipse(BoxF{cx - lw * 2, cy - lw * 2, cx + lw * 2, cy + lw * 2}, Ink(0));
        } else {
            const std::array<PointD, 2> one{PointD{cx - lw * 3, cy - lw * 3}, PointD{cx + lw * 3, cy + lw * 3}};
            const std::array<PointD, 2> two{PointD{cx - lw * 3, cy + lw * 3}, PointD{cx + lw * 3, cy - lw * 3}};
            draw.line(one, Ink(0), lw);
            draw.line(two, Ink(0), lw);
        }
        draw.text(PointD{h[0], core::py_max(0.0, h[1] - 12)}, render::text::u32(core::py_str(fig.char_id)), font, Ink(0));
    }
    // posed mannequins placed in this panel are the pose itself: drawn over the boxes
    for (const Json& prim : core::iterate(page.prims)) {
        if (!core::py_equals(core::py_get(prim, "kind"), Json("mannequin")) || !core::mannequin::in_rect(prim, frame_rect(frame))) continue;
        const core::mannequin::Bone bone = core::mannequin::skeleton(prim);
        for (const core::mannequin::Segment& s : bone.segments) {
            const auto a = box.to_px(s.a[0], s.a[1]);
            const auto b = box.to_px(s.b[0], s.b[1]);
            const std::array<PointD, 2> line{PointD{a[0], a[1]}, PointD{b[0], b[1]}};
            draw.line(line, Ink(s.part == "body" ? 0 : 60), lw * 2);
        }
        const double hx = bone.head_c[0];
        const double hy = bone.head_c[1];
        const double hr = bone.head_r;
        draw.ellipse(as_boxf(box.box_px(Box{hx - hr, hy - hr, hr * 2, hr * 2})), std::nullopt, Ink(0), lw * 2);
    }
    return image;
}

render::Image keepout(const core::Document& doc, const core::Page& page, const core::Frame& frame, const GenBox& box) {
    render::Image image = render::Image::create("L", render::Size{box.px[0], box.px[1]}, Ink(255));
    Draw draw(image);
    for (const auto& [rect, why] : keepout_boxes(doc, page, frame)) {
        (void)why;
        draw.rectangle(as_boxf(box.box_px(rect)), Ink(160));
    }
    return image;
}

render::Image mask(const GenBox& box, const std::vector<Box>& regions_mm) {
    render::Image image = render::Image::create("L", render::Size{box.px[0], box.px[1]}, Ink(0));
    Draw draw(image);
    for (const Box& rect : regions_mm) draw.rectangle(as_boxf(box.box_px(rect)), Ink(255));
    return image;
}

render::Image fit_cover(const render::Image& image, std::array<int, 2> size) {
    const double ratio = core::py_max(static_cast<double>(size[0]) / image.width(), static_cast<double>(size[1]) / image.height());
    const int w = std::max(1, static_cast<int>(core::py_round_int(image.width() * ratio)));
    const int h = std::max(1, static_cast<int>(core::py_round_int(image.height() * ratio)));
    const render::Image scaled = image.resize(render::Size{w, h}, render::Resample::Lanczos);
    const int left = static_cast<int>(core::py_floor((scaled.width() - size[0]) / 2.0));
    const int top = static_cast<int>(core::py_floor((scaled.height() - size[1]) / 2.0));
    return scaled.crop(render::Box{left, top, left + size[0], top + size[1]});
}

render::Image compare(const render::Image& candidate, const core::Page& page, const core::Frame& frame, const GenBox& box) {
    render::Image base = fit_cover(candidate.convert("RGB"), box.px);
    const render::Image lines = composition(page, frame, box);
    const render::Image red = render::Image::create("RGB", render::Size{box.px[0], box.px[1]}, Ink{230, 20, 20});
    const render::Image alpha = lines.point([](int v) { return v < 128 ? 128 : 0; });
    base.paste(red, render::Point{0, 0}, &alpha);
    return base;
}

std::string to_png(const render::Image& image) { return formats::png_bytes(image); }

double ink_ratio(const render::Image& image) {
    const render::Image gray = image.convert("L");
    const std::vector<long> hist = gray.histogram();
    long dark = 0;
    for (std::size_t i = 0; i < 128 && i < hist.size(); ++i) dark += hist[i];
    return core::py_round(static_cast<double>(dark) / std::max(1, gray.width() * gray.height()), 4);
}

}  // namespace genko::service::guide
