// ImageDraw (Pillow 12.3.0 PIL/ImageDraw.py; the draw_* methods of _imaging.c) over libImaging's Draw.c.

#include "render/draw.hpp"

#include <cmath>
#include <cstring>
#include <limits>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/imaging.hpp"

namespace genko::render {

namespace {

constexpr double kRadToDeg = 180.0 / core::kPi;  // mathmodule.c radToDeg
constexpr double kDegToRad = core::kPi / 180.0;  // mathmodule.c degToRad

// _imaging.c's (int)xy[i]: truncation toward zero; what x86 gives for values an int cannot hold.
int c_int(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<int>::min();
    return static_cast<int>(v);
}

// The page-sized image Draw.c draws on for a band of rows: rows [y0, y0 + band->ysize) are the band's, the others
// share one scratch row (written to and never read back).
Imaging make_view(Imaging band, int y0, int page_height, std::vector<unsigned char>& scratch) {
    Imaging view = detail::check(ImagingNewPrologue(band->mode, band->xsize, page_height));
    scratch.assign(static_cast<std::size_t>(band->linesize > 0 ? band->linesize : 1), 0);
    for (int y = 0; y < page_height; ++y) {
        const int row = y - y0;
        view->image[y] = (row >= 0 && row < band->ysize) ? band->image[row] : reinterpret_cast<char*>(scratch.data());
    }
    return view;
}

}  // namespace

struct Draw::Target {
    Imaging im = nullptr;
    Imaging owned_view = nullptr;
    std::vector<unsigned char> scratch;
    // a view: where its own rows are (for masks of the same shape)
    bool is_view = false;
    int window_y0 = 0;
    int window_rows = 0;
    int blend = 0;
    INT32 default_ink = 0;

    ~Target() {
        if (owned_view != nullptr) ImagingDelete(owned_view);
    }

    // _getink(ink): the ink to draw with, or the default one when there is none
    INT32 ink_of(const std::optional<Ink>& colour) const {
        return colour ? detail::ink_for(*colour, im) : default_ink;
    }
};

Draw::Draw(Image& image, std::string_view mode) : target_(std::make_unique<Target>()) {
    if (image.empty()) throw core::Error("value", "no image");
    Target& t = *target_;
    t.im = image.raw();
    const std::string_view own = image.mode();
    if (mode.empty()) mode = own;
    if (mode != own) {
        if (mode == "RGBA" && own == "RGB") {
            t.blend = 1;
        } else {
            throw core::Error("value", "mode mismatch");
        }
    }
    t.default_ink = detail::ink_for(Ink(mode == "I" || mode == "F" ? 1 : -1), t.im);
}

Draw::Draw(std::unique_ptr<Target> target) : target_(std::move(target)) {}

Draw::~Draw() = default;

void Draw::draw_lines(std::span<const PointD> xy, int ink, int width) {
    Imaging im = target_->im;
    const int blend = target_->blend;
    const std::size_t n = xy.size();
    if (width == 1) {
        const PointD* last = nullptr;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            detail::check_status(ImagingDrawLine(im, c_int(xy[i].x), c_int(xy[i].y), c_int(xy[i + 1].x),
                                                 c_int(xy[i + 1].y), &ink, blend));
            last = &xy[i + 1];
        }
        if (last != nullptr) (void)ImagingDrawPoint(im, c_int(last->x), c_int(last->y), &ink, blend);
    } else {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            detail::check_status(ImagingDrawWideLine(im, c_int(xy[i].x), c_int(xy[i].y), c_int(xy[i + 1].x),
                                                     c_int(xy[i + 1].y), &ink, width, blend, nullptr));
        }
    }
}

void Draw::line(std::span<const PointD> xy, const std::optional<Ink>& fill, int width, Joint joint) {
    const INT32 ink = target_->ink_of(fill);
    if (width == 0) return;
    draw_lines(xy, ink, width);
    if (joint != Joint::Curve || width <= 4) return;
    for (std::size_t i = 1; i + 1 < xy.size(); ++i) {
        const PointD point = xy[i];
        double angles[2];
        const PointD ends[2][2] = {{xy[i - 1], point}, {point, xy[i + 1]}};
        for (int k = 0; k < 2; ++k) {
            const PointD& start = ends[k][0];
            const PointD& end = ends[k][1];
            angles[k] = core::py_fmod(core::py_atan2(end.x - start.x, start.y - end.y) * kRadToDeg, 360.0);
        }
        if (angles[0] == angles[1]) continue;  // a straight line: no joint is needed

        const double distance = width / 2.0 - 1;
        const auto coord_at_angle = [&](const PointD& coord, double angle) {
            angle -= 90;
            const double dx = distance * core::py_cos(angle * kDegToRad);
            const double dy = distance * core::py_sin(angle * kDegToRad);
            return PointD{coord.x + (dx > 0 ? std::floor(dx) : std::ceil(dx)), coord.y + (dy > 0 ? std::floor(dy) : std::ceil(dy))};
        };
        const bool flipped = (angles[1] > angles[0] && angles[1] - 180 > angles[0]) ||
                             (angles[1] < angles[0] && angles[1] + 180 > angles[0]);
        const BoxF coords{point.x - width / 2.0 + 1, point.y - width / 2.0 + 1, point.x + width / 2.0 - 1,
                          point.y + width / 2.0 - 1};
        double start = 0.0;
        double end = 0.0;
        if (flipped) {
            start = angles[1] + 90;
            end = angles[0] + 90;
        } else {
            start = angles[0] - 90;
            end = angles[1] - 90;
        }
        pieslice(coords, start - 90, end - 90, fill);
        if (width > 8) {
            // cover potential gaps between the line and the joint
            std::vector<PointD> gap;
            if (flipped) {
                gap = {coord_at_angle(point, angles[0] + 90), point, coord_at_angle(point, angles[1] + 90)};
            } else {
                gap = {coord_at_angle(point, angles[0] - 90), point, coord_at_angle(point, angles[1] - 90)};
            }
            line(gap, fill, 3);
        }
    }
}

void Draw::polygon(std::span<const PointD> xy, const std::optional<Ink>& fill, const std::optional<Ink>& outline,
                   int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (xy.size() < 2) throw core::Error("value", "coordinate list must contain at least 2 coordinates");
    std::vector<int> ixy;
    ixy.reserve(xy.size() * 2);
    for (const PointD& p : xy) {
        ixy.push_back(c_int(p.x));
        ixy.push_back(c_int(p.y));
    }
    const int count = static_cast<int>(xy.size());
    if (fill_ink) {
        detail::check_status(ImagingDrawPolygon(t.im, count, ixy.data(), &*fill_ink, 1, 0, t.blend, nullptr));
    }
    if (ink && ink != fill_ink && width != 0) {
        if (width == 1) {
            detail::check_status(ImagingDrawPolygon(t.im, count, ixy.data(), &*ink, 0, 1, t.blend, nullptr));
        } else {
            // to avoid expanding the polygon outwards, the fill is the mask of the outline (a "1" image of the
            // picture's size, drawn with the ink of 1)
            const INT32 mask_ink = detail::ink_for(Ink(1), t.im);
            Image band;
            Imaging mask = nullptr;
            Imaging mask_view = nullptr;
            std::vector<unsigned char> scratch;
            if (t.is_view) {
                band = Image::create("1", Size{t.im->xsize, t.window_rows});
                mask_view = make_view(band.raw(), t.window_y0, t.im->ysize, scratch);
                mask = mask_view;
            } else {
                band = Image::create("1", Size{t.im->xsize, t.im->ysize});
                mask = band.raw();
            }
            const int drawn = ImagingDrawPolygon(mask, count, ixy.data(), &mask_ink, 1, 0, 0, nullptr);
            const int outlined = drawn < 0 ? drawn
                                           : ImagingDrawPolygon(t.im, count, ixy.data(), &*ink, 0, width * 2 - 1,
                                                                t.blend, mask);
            if (mask_view != nullptr) ImagingDelete(mask_view);
            detail::check_status(outlined);
        }
    }
}

namespace {

// The two-corner shapes of _imaging.c: the corners in order, then truncated.
struct Corners {
    int x0, y0, x1, y1;
};

Corners corners_of(const BoxF& box) {
    if (box.x1 < box.x0) throw core::Error("value", "x1 must be greater than or equal to x0");
    if (box.y1 < box.y0) throw core::Error("value", "y1 must be greater than or equal to y0");
    return Corners{c_int(box.x0), c_int(box.y0), c_int(box.x1), c_int(box.y1)};
}

}  // namespace

void Draw::ellipse(const BoxF& box, const std::optional<Ink>& fill, const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawEllipse(t.im, c.x0, c.y0, c.x1, c.y1, &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawEllipse(t.im, c.x0, c.y0, c.x1, c.y1, &*ink, 0, width, t.blend));
    }
}

void Draw::rectangle(const BoxF& box, const std::optional<Ink>& fill, const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawRectangle(t.im, c.x0, c.y0, c.x1, c.y1, &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawRectangle(t.im, c.x0, c.y0, c.x1, c.y1, &*ink, 0, width, t.blend));
    }
}

void Draw::point(std::span<const PointD> xy, const std::optional<Ink>& fill) {
    Target& t = *target_;
    const INT32 ink = t.ink_of(fill);
    for (const PointD& p : xy) detail::check_status(ImagingDrawPoint(t.im, c_int(p.x), c_int(p.y), &ink, t.blend));
}

void Draw::arc(const BoxF& box, double start, double end, const std::optional<Ink>& fill, int width) {
    Target& t = *target_;
    const INT32 ink = t.ink_of(fill);
    if (width == 0) return;
    const Corners c = corners_of(box);
    detail::check_status(ImagingDrawArc(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                        static_cast<float>(end), &ink, width, t.blend));
}

void Draw::pieslice(const BoxF& box, double start, double end, const std::optional<Ink>& fill,
                    const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawPieslice(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                                 static_cast<float>(end), &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawPieslice(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                                 static_cast<float>(end), &*ink, 0, width, t.blend));
    }
}

void Draw::chord(const BoxF& box, double start, double end, const std::optional<Ink>& fill,
                 const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawChord(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                              static_cast<float>(end), &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawChord(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                              static_cast<float>(end), &*ink, 0, width, t.blend));
    }
}

// --- PageCanvas -----------------------------------------------------------------------------------------------------

PageCanvas::PageCanvas(Image& part, const Box& area, Size page, std::string_view mode)
    : part_(part), area_(area), page_(page) {
    if (part.empty()) throw core::Error("value", "no image");
    if (part.size() != Size{area.width(), area.height()}) throw core::Error("value", "the part is not the area's size");
    if (area == Box{0, 0, page.width, page.height}) {
        draw_ = std::make_unique<Draw>(part, mode);
        return;
    }
    // the rows of the area, as wide as the page
    Imaging band = nullptr;
    if (area.x0 == 0 && area.x1 == page.width) {
        band = part.raw();
    } else {
        band_ = Image::create_blank(part.mode(), Size{page.width, area.height()});
        band_.paste(part, Point{area.x0, 0});
        band = band_.raw();
    }
    auto target = std::make_unique<Draw::Target>();
    target->owned_view = make_view(band, area.y0, page.height, target->scratch);
    target->im = target->owned_view;
    target->is_view = true;
    target->window_y0 = area.y0;
    target->window_rows = area.height();
    const std::string_view own = part.mode();
    if (mode.empty()) mode = own;
    if (mode != own) {
        if (mode == "RGBA" && own == "RGB") {
            target->blend = 1;
        } else {
            throw core::Error("value", "mode mismatch");
        }
    }
    target->default_ink = detail::ink_for(Ink(mode == "I" || mode == "F" ? 1 : -1), target->im);
    draw_.reset(new Draw(std::move(target)));
}

PageCanvas::~PageCanvas() = default;

void PageCanvas::commit() {
    if (committed_) return;
    committed_ = true;
    draw_.reset();
    if (!band_.empty()) {
        const Image back = band_.crop(Box{area_.x0, 0, area_.x1, area_.height()});
        part_.paste(back, Point{0, 0});
    }
}

}  // namespace genko::render
