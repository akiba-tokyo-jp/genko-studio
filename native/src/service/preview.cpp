#include "service/preview.hpp"

#include <cmath>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"
#include "service/blocking.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/layout.hpp"
#include "service/text.hpp"

namespace genko::service {

namespace {

using core::Num;

// The fonts of the badges: render._font(None, size), the first of _CJK_FONTS, the bundled Dela Gothic.
const render::TrueTypeFont& badge_font(render::TrueTypeFonts& fonts, int size) { return fonts.truetype("sfx", std::max(8, size)); }

render::Ink rgb(int r, int g, int b) { return render::Ink{r, g, b}; }

// int(max(64, max_px) / (length_mm / 25.4)): the dpi a preview of max_px pixels along that length is drawn at,
// then held between low and high.
int preview_dpi(const Json& max_px, const Num& length_mm, int low, int high) {
    const Json at_least = core::py_less(max_px, Json(64), ">") ? max_px : Json(64);
    const double inches = (length_mm / Num(25.4)).value();
    if (inches == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
    const double dpi = core::to_num(at_least).value() / inches;
    if (std::isnan(dpi)) throw core::PyValueError("cannot convert float NaN to integer");
    if (std::isinf(dpi)) throw core::PyUncaught("OverflowError", "cannot convert float infinity to integer");
    const double whole = std::trunc(dpi);
    return static_cast<int>(std::max<double>(low, std::min<double>(high, whole)));
}

}  // namespace

std::string png(const render::Image& image) { return formats::png_bytes(image); }

render::Image annotate(const render::Image& image, const core::Page& page, int dpi, const Json& plan) {
    render::Image out = image.convert("RGB");
    render::Draw draw(out);
    PyDict panels;
    std::vector<std::string> slots_by_order;
    if (core::py_truthy(plan)) {
        for (const Json& p : core::iterate(core::py_get(plan, "panels", Json::array()))) panels.set(core::py_get(p, "slot"), p);
        if (core::truthy_at(plan, "tiers")) {
            slots_by_order = layout::slots_in_order(plan["tiers"]);
        } else if (core::truthy_at(plan, "template")) {
            slots_by_order = layout::slots_in_order(layout::resolve_tiers(plan));
        }
    }
    const int badge = std::max(10, render::mm_to_px(6, dpi));
    render::TrueTypeFonts fonts;
    const render::TrueTypeFont& font = badge_font(fonts, std::max(10, badge * 2 / 3));
    const render::TrueTypeFont& small = badge_font(fonts, std::max(9, badge / 2));
    std::size_t order = 0;
    for (const core::Frame* frame : page.leaf_frames()) {
        ++order;
        const core::Rect& r = frame->rect;
        const int x2 = render::mm_to_px((r.x + r.width).value(), dpi);
        const int y = render::mm_to_px(r.y.value(), dpi);
        const int x = render::mm_to_px(r.x.value(), dpi);
        draw.rectangle(render::BoxF{double(x), double(y), double(x + badge), double(y + badge)}, rgb(40, 90, 200));
        draw.text(render::PointD{double(x + badge / 5), double(y + badge / 8)}, render::text::u32(std::to_string(order)), font,
                  rgb(255, 255, 255));
        const std::string slot = order - 1 < slots_by_order.size() ? slots_by_order[order - 1] : std::string();
        const Json* panel = panels.find(Json(slot));
        if (panel == nullptr || !core::py_truthy(*panel)) continue;
        // where each person stands and how big (pos, scale): the name is approved with the blocking in view
        const NumBox rect{r.x, r.y, r.width, r.height};
        for (const Figure& fig : figures(*panel, rect)) {
            for (const auto& [box, width] : {std::pair<const NumBox*, int>{&fig.body, 1}, std::pair<const NumBox*, int>{&fig.head, 2}}) {
                const int bx = render::mm_to_px((*box)[0].value(), dpi);
                const int by = render::mm_to_px((*box)[1].value(), dpi);
                const int bw = render::mm_to_px((*box)[2].value(), dpi);
                const int bh = render::mm_to_px((*box)[3].value(), dpi);
                draw.rectangle(render::BoxF{double(bx), double(by), double(bx + bw), double(by + bh)}, std::nullopt, rgb(200, 80, 60), width);
            }
            const int hx = render::mm_to_px(fig.head[0].value(), dpi);
            const int hy = render::mm_to_px(fig.head[1].value(), dpi);
            draw.text(render::PointD{double(hx + 3), double(hy + 2)}, render::text::u32(core::py_str(fig.char_id)), small, rgb(200, 80, 60));
        }
        std::u32string label = render::text::u32(text::head(slot + " " + core::py_str(core::py_get(*panel, "shot")) + "/" +
                                                                core::py_str(core::py_get(*panel, "angle")) + " " +
                                                                core::py_str(core::py_get(*panel, "action", Json(""))),
                                                            40));
        const int ly = render::mm_to_px((r.y + r.height).value(), dpi) - badge;
        const int room = x2 - x - 6;
        if (small.getlength(label) > room) {  // (the band stays inside its own panel: the words are cut to fit)
            while (!label.empty() && small.getlength(label + U"…") > room) label.pop_back();
            label += U"…";
        }
        draw.rectangle(render::BoxF{double(x), double(ly), double(x2), double(ly + badge)}, rgb(235, 240, 255));
        draw.text(render::PointD{double(x + 3), double(ly + 2)}, label, small, rgb(40, 60, 140));
    }
    return out;
}

std::string preview(const core::Document& doc, const Json& page_index, const std::string& mode, const Json& max_px,
                    const Json& plan, const Json& frame_id) {
    const auto at = page_at(doc, page_index);
    if (!at) throw core::ApplyError(core::py_str(page_index) + " ページはない");
    const core::Page& page = doc.page(*at);
    render::Image image;
    int dpi = 0;
    if (core::py_truthy(frame_id)) {
        const core::Frame* frame = nullptr;
        try {
            frame = &briefs::find_frame(page, frame_id);
        } catch (const core::PyUncaught& e) {
            if (e.type() != "KeyError" && e.type() != "IndexError") throw;
            std::string ids;
            for (const core::Frame* f : page.leaf_frames()) ids += (ids.empty() ? "" : ", ") + f->id;
            throw core::ApplyError(core::py_str(page_index) + " ページにコマ " + core::py_str(frame_id) + " はない（このページのコマ: " + ids + "）");
        }
        const Num longest = frame->rect.height > frame->rect.width ? frame->rect.height : frame->rect.width;
        dpi = preview_dpi(max_px, longest, 36, 600);
        render::RenderOptions options;
        options.mode = mode == "name" ? "proof" : mode;
        image = render::render_frame(page, frame->id, dpi, options, &doc);
    } else {
        dpi = preview_dpi(max_px, page.spec.height_mm, 36, 300);
        render::RenderOptions options;
        options.mode = mode;
        image = render::render_page(page, dpi, options, &doc).image;
    }
    if (mode == "name" && !core::py_truthy(frame_id)) image = annotate(image, page, dpi, plan);
    return png(image);
}

}  // namespace genko::service
