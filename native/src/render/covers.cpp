#include "render/covers.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/covers.hpp"
#include "render/draw.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"

namespace genko::render {

void draw_folds(Image& part, const Box& area, Size page_size, const core::Page& page, int dpi, std::string_view binding,
                std::stop_token stop) {
    const std::vector<core::Fold> parts = core::folds(page, binding);
    if (parts.empty()) return;
    PageCanvas canvas(part, area, page_size);
    Draw& draw = canvas.draw();
    const core::Rect t = page.trim_rect_mm();
    const int y0 = mm_to_px(t.y.value(), dpi);
    const int y1 = mm_to_px(t.y.value() + t.height.value(), dpi);
    const Ink blue{60, 140, 220};
    const int step = std::max(4, mm_to_px(4, dpi));
    const int dash = std::max(2, mm_to_px(2, dpi));
    const int size = std::max(10, mm_to_px(4, dpi));
    text::Fonts fonts(stop);
    const text::Face face = text::face_of(core::Json(nullptr));  // (fonts.face(None): the default dialogue face)
    for (const core::Fold& fold : parts) {
        for (const double x : {fold.x0, fold.x1}) {
            const auto px = static_cast<double>(mm_to_px(x, dpi));
            for (int y = y0; y < y1; y += step) {
                if (stop.stop_requested()) throw Cancelled();
                const std::vector<PointD> dashed{{px, static_cast<double>(y)}, {px, static_cast<double>(std::min(y1, y + dash))}};
                draw.line(dashed, blue, 1);
            }
        }
        // the part's name in the font its first character takes, centred on the part, a letter below the trim's top
        const std::u32string name = text::u32(fold.name);
        const TrueTypeFont& font = fonts.font(face, size, std::u32string_view(name).substr(0, 1));
        const PointD at{(mm_to_px(fold.x0, dpi) + mm_to_px(fold.x1, dpi)) / 2.0, static_cast<double>(y0 + size)};
        draw.text(at, name, font, blue, "mm");
    }
    canvas.commit();
}

Image front_of(const core::Page& page, const Image& image, int dpi, std::string_view binding, std::string_view which) {
    for (const core::Fold& fold : core::folds(page, binding)) {
        if (fold.name != which) continue;
        const core::Rect t = page.trim_rect_mm();
        return image.crop(Box{mm_to_px(fold.x0, dpi), mm_to_px(t.y.value(), dpi), mm_to_px(fold.x1, dpi),
                              mm_to_px(t.y.value() + t.height.value(), dpi)});
    }
    return image;
}

}  // namespace genko::render
