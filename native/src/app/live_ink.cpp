#include "app/live_ink.hpp"

#include <QPainter>

#include <algorithm>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/stroke_geom.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"

namespace genko::app {

namespace {

using core::Json;

const std::vector<std::int64_t> kNameColor{58, 110, 165};
const std::vector<std::int64_t> kInkColor{20, 20, 20};

bool guide_role(core::LayerRole role) { return role == core::LayerRole::Name || role == core::LayerRole::Draft; }

// brushes._stamped
bool stamped(const core::Brush& b) {
    return !b.pattern.empty() || b.tip == "flat" || b.tip == "image" || b.scatter > 0 || b.spacing > 0;
}

const Json* field(const Json& fields, const char* key) {
    const auto it = fields.find(key);
    return it == fields.end() ? nullptr : &*it;
}

bool truthy(const Json& fields, const char* key) {
    const Json* v = field(fields, key);
    return v != nullptr && core::py_truthy(*v);
}

render::Box to_box(const QRect& r) { return render::Box{r.x(), r.y(), r.x() + r.width(), r.y() + r.height()}; }

QRect to_rect(render::Point origin, const render::Image& image) { return QRect(origin.x, origin.y, image.width(), image.height()); }

// The opacity's cut of a coverage (render's cover.point(lambda v: int(v * opacity))).
render::Image faded(const render::Image& cover, double opacity) {
    if (opacity >= 1) return cover;
    return cover.point([opacity](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); });
}

}  // namespace

LiveInk::LiveInk(std::shared_ptr<const core::Document> doc_ptr, const core::Page& page, const core::Layer& layer, int dpi,
                 const Json& fields, std::string seed)
    : doc_(std::move(doc_ptr)), page_(&page), layer_(&layer), dpi_(dpi), seed_(std::move(seed)) {
    const core::Document& doc = *doc_;
    size_ = render::Size{render::mm_to_px(page.spec.width_mm.value(), dpi), render::mm_to_px(page.spec.height_mm.value(), dpi)};
    try {
        kind_ = core::brush_kind(truthy(fields, "kind") ? fields["kind"] : Json("gpen"), doc);
    } catch (const std::exception&) {
        kind_ = std::string(core::kDefaultBrush);
    }
    const core::Brush b = render::brushes::brush(kind_);
    const Json* width = field(fields, "width_mm");
    width_mm_ = width != nullptr && !width->is_null() ? core::py_float(*width) : doc.brush_width_mm;
    if (width_mm_ == 0.0) width_mm_ = 0.35;  // (render: float(stroke.width_mm or 0.35))
    if (guide_role(layer.role)) {
        rgb_ = kNameColor;
    } else if (truthy(fields, "rgb")) {
        rgb_ = core::int_tuple(fields["rgb"]);
    } else if (doc.brush_rgb != kInkColor && !doc.brush_rgb.empty()) {
        rgb_ = doc.brush_rgb;
    } else {
        rgb_ = b.rgb && !b.rgb->empty() ? *b.rgb : kInkColor;
    }
    if (rgb_.size() > 3) rgb_.resize(3);
    const Json* opacity = field(fields, "opacity");
    const double own = opacity != nullptr && !opacity->is_null() ? core::py_clamp(core::py_float(*opacity), 0.0, 1.0) : 1.0;
    opacity_ = own * b.opacity;
    gamma_ = truthy(fields, "pressure_gamma") ? core::py_clamp(core::py_float(fields["pressure_gamma"]), 0.2, 5.0) : 1.0;
    curve_ = truthy(fields, "curve") ? core::py_str(fields["curve"]) : doc.brush_curve;
    if (curve_.empty()) curve_ = "linear";
    const Json* stabilize = field(fields, "stabilize");
    stabilize_ = stabilize != nullptr ? (core::py_truthy(*stabilize) ? core::py_int(*stabilize) : 0) : doc.brush_stabilize;
    const Json* smooth = field(fields, "post_smooth");
    post_smooth_ = smooth != nullptr ? (core::py_truthy(*smooth) ? core::py_int(*smooth) : 0) : b.post_smooth;
    if (truthy(fields, "pressure_opacity")) {
        pressure_opacity_ = core::py_round(core::py_clamp(core::py_float(fields["pressure_opacity"]), 0.0, 1.0), 3);
    }
    const bool by_speed = truthy(fields, "stabilize_speed") && stabilize_ != 0;
    piecewise_ = !stamped(b) && b.texture.empty() && b.aa != "strong" && pressure_opacity_ <= 0 && !by_speed && !b.paper;
    clip_ = layer.panel_clip &&
            render::detail::clip_mask(page, size_, dpi_, render::Box{0, 0, 1, 1}).has_value();  // (a panel cuts the layers)
}

core::PenPoints LiveInk::as_committed(const core::PenPoints& raw, std::size_t& settled) const {
    core::PenPoints pts = raw;
    settled = pts.size();
    if (stabilize_ != 0) {
        pts = core::stabilize_points(pts, stabilize_, false);
        if (stabilize_ >= 3 && raw.size() >= 3) {
            settled = static_cast<std::size_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(pts.size()) - stabilize_ / 2));
        }
    }
    if (post_smooth_ > 0) {
        pts = core::smoothed(pts, post_smooth_);
        settled = static_cast<std::size_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(settled) - post_smooth_));
    }
    if (gamma_ != 1.0) {
        for (core::PenPoint& p : pts) {
            if (p.p) p.p = core::py_pow(core::py_clamp(*p.p, 0.0, 1.0), gamma_);
        }
    }
    if (curve_ != "linear") pts = core::apply_pressure_curve(pts, curve_);
    for (core::PenPoint& p : pts) {
        p.x = core::py_round(p.x, 3);
        p.y = core::py_round(p.y, 3);
        if (p.p) p.p = core::py_round(*p.p, 3);
    }
    return pts;
}

void LiveInk::follow(const core::PenPoints& points, std::span<const double> rotation) {
    if (points.empty()) return;
    core::PenPoints raw = points;
    if (raw.size() == 1) raw.push_back(core::PenPoint{raw[0].x + 0.01, raw[0].y + 0.01, raw[0].p});  // (a tap: a dot)
    std::size_t settled = 0;
    const core::PenPoints pts = as_committed(raw, settled);
    if (!panel_ && !pts.empty() && layer_->panel_each) {
        if (const core::Frame* frame = render::detail::panel_of(*page_, pts.front().x, pts.front().y)) panel_ = frame->id;
    }
    // the pen's turns as add_stroke keeps them (a tenth of a degree, one per point)
    std::vector<double> turns;
    if (!rotation.empty() && rotation.size() == points.size() &&
        std::any_of(rotation.begin(), rotation.end(), [](double v) { return std::abs(v) > 0.5; })) {
        std::vector<double> values;
        for (const double v : rotation) values.push_back(core::py_round(v, 1));
        if (values.size() == 1) values.push_back(values.back());
        turns = core::resampled(values, static_cast<std::int64_t>(pts.size()));
    }
    if (!piecewise_) {
        const auto drawn = render::brushes::draw(size_, pts, dpi_, width_mm_, kind_, seed_, turns, pressure_opacity_);
        if (drawn) {
            add_coverage(drawn->mask, drawn->origin, true);
        }
        drawn_ = pts.size();
        set_tail(std::nullopt);
        return;
    }
    if (settled > drawn_) {
        const std::size_t start = drawn_ >= 2 ? drawn_ - 2 : 0;  // (one point of overlap for the join)
        core::PenPoints part(pts.begin() + static_cast<std::ptrdiff_t>(start), pts.begin() + static_cast<std::ptrdiff_t>(settled));
        if (part.size() == 1) part.push_back(core::PenPoint{part[0].x + 0.01, part[0].y + 0.01, part[0].p});
        if (const auto drawn = render::brushes::draw(size_, part, dpi_, width_mm_, kind_, seed_, {}, 0.0)) {
            add_coverage(drawn->mask, drawn->origin, false);
        }
        drawn_ = settled;
    }
    std::optional<render::brushes::Coverage> tail;
    if (settled < pts.size()) {
        const std::size_t from = settled >= 2 ? settled - 2 : 0;
        const core::PenPoints rest(pts.begin() + static_cast<std::ptrdiff_t>(from), pts.end());
        if (rest.size() > 1) tail = render::brushes::draw(size_, rest, dpi_, width_mm_, kind_, seed_, {}, 0.0);
    }
    set_tail(tail);
}

void LiveInk::finish(const core::Stroke& stroke) {
    const core::Brush b = render::brushes::brush(stroke.kind);
    kind_ = stroke.kind;
    seed_ = stroke.id;
    width_mm_ = stroke.width_mm != 0.0 ? stroke.width_mm : 0.35;
    rgb_ = guide_role(layer_->role) ? kNameColor : (stroke.rgb && !stroke.rgb->empty() ? *stroke.rgb : (b.rgb && !b.rgb->empty() ? *b.rgb : kInkColor));
    if (rgb_.size() > 3) rgb_.resize(3);
    opacity_ = core::py_clamp(stroke.opacity, 0.0, 1.0) * b.opacity;
    pressure_opacity_ = stroke.pressure_opacity;
    const core::PenPoints pts = core::stroke_points(stroke);
    panel_.reset();
    if (layer_->panel_each && !pts.empty()) {
        if (const core::Frame* frame = render::detail::panel_of(*page_, pts.front().x, pts.front().y)) panel_ = frame->id;
    }
    tail_.reset();
    const auto drawn = render::brushes::draw(size_, pts, dpi_, width_mm_, kind_, stroke.id, stroke.rotation, stroke.pressure_opacity);
    if (drawn) {
        add_coverage(drawn->mask, drawn->origin, true);
    } else {
        box_ = QRect();
        coverage_ = render::Image();
        image_ = QImage();
    }
    drawn_ = pts.size();
}

QRectF LiveInk::box_mm() const {
    if (box_.isEmpty()) return {};
    const double mm = 25.4 / dpi_;
    return QRectF(box_.x() * mm, box_.y() * mm, box_.width() * mm, box_.height() * mm);
}

void LiveInk::ensure_box(const QRect& wanted) {
    if (wanted.isEmpty() || box_.contains(wanted)) return;
    const QRect page(0, 0, size_.width, size_.height);
    QRect grown = box_.isEmpty() ? wanted : box_.united(wanted);
    grown = grown.adjusted(-128, -128, 128, 128).intersected(page);  // (room to grow without copying every time)
    render::Image cover = render::Image::create("L", render::Size{grown.width(), grown.height()}, render::Ink(0));
    QImage shown(grown.size(), QImage::Format_ARGB32_Premultiplied);
    shown.fill(Qt::transparent);
    if (!box_.isEmpty() && !coverage_.empty()) {
        cover.paste(coverage_, render::Point{box_.x() - grown.x(), box_.y() - grown.y()});
        QPainter painter(&shown);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(box_.topLeft() - grown.topLeft(), image_);
    }
    box_ = grown;
    coverage_ = std::move(cover);
    image_ = std::move(shown);
}

void LiveInk::add_coverage(const render::Image& mask, render::Point origin, bool replace) {
    const QRect piece = to_rect(origin, mask);
    if (replace) {
        const QRect old = box_;
        changed_ = changed_.united(old);  // (what was shown there goes)
        box_ = QRect();
        coverage_ = render::Image();
        image_ = QImage();
        ensure_box(piece.isEmpty() ? old : piece);
        if (!piece.isEmpty()) coverage_.paste(mask, render::Point{piece.x() - box_.x(), piece.y() - box_.y()});
        refresh(box_);
        return;
    }
    ensure_box(piece);
    const render::Box local = to_box(piece.translated(-box_.topLeft()));
    // (overlaps never darken twice: the line is the union of its parts)
    coverage_.paste(render::chops::lighter(coverage_.crop(local), mask), render::Point{local.x0, local.y0});
    refresh(piece);
}

void LiveInk::set_tail(const std::optional<render::brushes::Coverage>& tail) {
    const QRect old = tail_ ? to_rect(tail_->origin, tail_->mask) : QRect();
    tail_ = tail;
    const QRect now = tail_ ? to_rect(tail_->origin, tail_->mask) : QRect();
    ensure_box(now);
    if (!old.isEmpty()) refresh(old);
    if (!now.isEmpty()) refresh(now);
}

render::Image LiveInk::clip_for(const render::Box& area) const {
    if (!clip_) return {};
    if (panel_) {
        if (const core::Frame* frame = page_->find_frame(*panel_)) return render::detail::frame_mask(*page_, *frame, size_, dpi_, area);
    }
    if (auto mask = render::detail::clip_mask(*page_, size_, dpi_, area)) return std::move(*mask);
    return {};
}

void LiveInk::refresh(const QRect& part_in) {
    const QRect part = part_in.intersected(box_);
    if (part.isEmpty()) return;
    changed_ = changed_.united(part);
    const render::Box local = to_box(part.translated(-box_.topLeft()));
    render::Image cover = coverage_.crop(local);
    if (tail_) {
        const render::Box tail_local{part.x() - tail_->origin.x, part.y() - tail_->origin.y, part.x() - tail_->origin.x + part.width(),
                                     part.y() - tail_->origin.y + part.height()};
        cover = render::chops::lighter(cover, tail_->mask.crop(tail_local));
    }
    cover = faded(cover, opacity_);
    render::Image rgba = render::Image::create("RGBA", render::Size{part.width(), part.height()}, render::Ink::with_alpha(rgb_, 0));
    rgba.putalpha(cover);
    if (clip_) {
        const render::Image mask = clip_for(to_box(part));
        if (!mask.empty()) rgba.putalpha(render::chops::multiply(rgba.getchannel(3), mask));
    }
    const std::string bytes = rgba.tobytes();
    const QImage piece = QImage(reinterpret_cast<const uchar*>(bytes.data()), part.width(), part.height(), part.width() * 4,
                                QImage::Format_RGBA8888)
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image_);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(part.topLeft() - box_.topLeft(), piece);
}

render::Image LiveInk::patch() const {
    if (box_.isEmpty() || coverage_.empty()) return {};
    render::Image cover = coverage_;
    if (tail_) {
        render::Image tail = render::Image::create("L", coverage_.size(), render::Ink(0));
        tail.paste(tail_->mask, render::Point{tail_->origin.x - box_.x(), tail_->origin.y - box_.y()});
        cover = render::chops::lighter(cover, tail);
    }
    cover = faded(cover, opacity_);
    // the renderer's steps for a layer's lines (render._layer_strokes: the coverage's colour composited over a
    // transparent picture, then the panel clip)
    render::Image out = render::Image::create("RGBA", coverage_.size(), render::Ink{0, 0, 0, 0});
    if (const auto bbox = cover.getbbox()) {
        render::Image piece = render::Image::create("RGBA", render::Size{bbox->width(), bbox->height()}, render::Ink::with_alpha(rgb_, 0));
        piece.putalpha(cover.crop(*bbox));
        out.alpha_composite(piece, render::Point{bbox->x0, bbox->y0});
    }
    if (clip_) {
        const render::Image mask = clip_for(to_box(box_));
        if (!mask.empty()) out.putalpha(render::chops::multiply(out.getchannel(3), mask));
    }
    return out;
}

}  // namespace genko::app
