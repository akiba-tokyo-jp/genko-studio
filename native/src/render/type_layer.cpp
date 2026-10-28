// PC5 G-0 (type@1, render/type_layer.hpp): a type layer's picture.

#include "render/type_layer.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "core/type_text.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/page_internal.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"

namespace genko::render::type_layer {

std::optional<Image> picture_8bit(const detail::Ctx& ctx, const core::Layer& layer, const Box& area) {
    if (!layer.type || ctx.page == nullptr || area.width() <= 0 || area.height() <= 0) return std::nullopt;
    const core::StoryLine line = core::type_text::line_of(layer, ctx.page->index);
    const std::vector<const core::StoryLine*> lines{&line};
    text::Panels panels;
    for (const core::Frame* f : ctx.page->leaf_frames())
        panels.emplace_back(f->id, std::array<double, 4>{f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value()});
    const std::optional<std::string> font_path = ctx.episode != nullptr ? std::optional<std::string>(ctx.episode->font_path) : std::nullopt;
    const text::Unported unported = [&ctx](const NotYetPorted& e) { detail::skip_unported(ctx, e.element()); };
    const text::TextGamma plain(std::nullopt);  // (the layer's own picture: no page under it to mix with)
    const auto draw = [&](Image& on) {
        Image rgba = on.mode() == "RGBA" ? std::move(on) : on.convert("RGBA");
        text::draw_lines(text::PagePart{&rgba, Point{area.x0, area.y0}, ctx.size}, lines, ctx.dpi, font_path, false, &panels, ctx.stop, &unported);
        on = std::move(rgba);
    };
    Image picture = smart_ops::drawn_alone(Size{area.width(), area.height()}, draw);
    if (!picture.getbbox()) return std::nullopt;
    return picture;
}

}  // namespace genko::render::type_layer
