// PC3 B3 (SPEC.md COLOR-06「マッティング」): the op matting — Layer › Matting on a paint layer: カラーの除去
// (decontaminate: the colours of the soft fringe of the layer's alpha moved toward the opaque colours near them),
// フリンジ削除 (defringe: the pixels within `width_mm` of transparency take the colours further inside), 黒マット削除・
// 白マット削除 (remove_black, remove_white: the half-clear colours composited over black or white unpremultiplied). The
// alpha is kept. Worked on straight RGBA as PhotoCraft does: an 8-bit or precise layer's own values, an ink layer's the
// colours its inks mean (image_ops::rgba_of), its inks found again by the page's rules for the colours changed. With an
// "area" the change is weighed by its coverage (decontaminate: the matte is the alpha × the coverage — 選択とマスク's
// カラーの除去 gives the refined selection). PhotoCraft v0.2.0's engine/src/layer_menu_cmds.rs (lines 227-310) over
// algo/src/matting.rs (render/matting), MIT (native/third_party/photocraft/NOTICE.md).

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/image_ops.hpp"
#include "render/matting.hpp"
#include "render/ops_pc3.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

std::optional<double> number(const Json& op, const char* key) {
    const Json* v = field(op, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a finite number");
    return v->get<double>();
}

}  // namespace

void matting_op(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* given = field(op, "id");
    if (given == nullptr || !given->is_string()) throw OpError("id (the layer) is required");
    const std::string id = given->get<std::string>();
    const Json* kind_given = field(op, "kind");
    static const std::vector<std::string> kKinds{"defringe", "remove_black", "remove_white", "decontaminate"};
    if (kind_given == nullptr || !kind_given->is_string() || std::find(kKinds.begin(), kKinds.end(), kind_given->get<std::string>()) == kKinds.end())
        throw OpError("kind is defringe, remove_black, remove_white or decontaminate");
    const std::string kind = kind_given->get<std::string>();
    const std::optional<double> width_mm = number(op, "width_mm");
    const std::optional<double> radius_mm = number(op, "radius_mm");
    const double amount = std::clamp(number(op, "amount").value_or(100.0), 0.0, 100.0);
    if (width_mm && *width_mm <= 0) throw OpError("width_mm must be more than 0");
    if (radius_mm && *radius_mm <= 0) throw OpError("radius_mm must be more than 0");
    Pixels p = image_ops::layer_pixels(c, at, id);  // (refused when locked or not a paint layer; its lines drawn in)
    const core::Page& page = doc.page(at);
    const std::size_t w = p.width(), h = p.height();
    // (the layer's grid: its pixels a millimetre)
    const double per_mm = static_cast<double>(w) / page.spec.width_mm.value();
    const image_ops::Held held(std::uint64_t(p.size()) * sizeof(image_ops::Rgba) * 2);
    const std::vector<image_ops::Rgba> before = image_ops::rgba_of(p);
    std::vector<image_ops::Rgba> px = before;
    std::optional<std::vector<float>> cover;
    if (const Json* area = field(op, "area")) cover = image_ops::coverage(page, *area, p.width(), p.height(), p.kind() == Pixels::Kind::Eight);
    if (kind == "decontaminate") {
        // (upstream: radius 1..100 pixels, 4 when not given)
        const float radius = static_cast<float>(std::clamp(radius_mm ? *radius_mm * per_mm : 4.0, 1.0, 100.0));
        std::vector<float> matte(px.size());
        for (std::size_t i = 0; i < px.size(); ++i) {
            const float a = std::isfinite(px[i][3]) ? std::clamp(px[i][3], 0.0f, 1.0f) : 0.0f;
            matte[i] = cover ? a * (*cover)[i] : a;
        }
        matting::decontaminate(px, w, h, matte, radius, static_cast<float>(amount));
    } else if (kind == "defringe") {
        const auto width = static_cast<std::size_t>(std::clamp(std::round(width_mm ? *width_mm * per_mm : 1.0), 1.0, 200.0));
        matting::defringe(px, w, h, width);
    } else {
        matting::remove_matte(px, kind == "remove_white");
    }
    if (cover && kind != "decontaminate") {
        for (std::size_t i = 0; i < px.size(); ++i)
            for (std::size_t k = 0; k < 3; ++k) px[i][k] = before[i][k] + (px[i][k] - before[i][k]) * (*cover)[i];
    }
    bool changed = false;
    for (std::size_t i = 0; i < px.size() && !changed; ++i) changed = px[i] != before[i];
    if (!changed) {
        c.report = Json{{"changed", false}};
        return;
    }
    if (p.kind() == Pixels::Kind::Inks) {
        image_ops::from_rgba(p, px);  // (the page's rules: a colour left as it was keeps its inks)
    } else {
        for (std::size_t i = 0; i < px.size(); ++i) {
            float* v = p.at(i);
            for (std::size_t k = 0; k < 3; ++k) v[k] = px[i][k];
        }
    }
    image_ops::put_layer_pixels(doc, at, id, p);
    c.report = Json{{"changed", true}};
}

}  // namespace genko::render
