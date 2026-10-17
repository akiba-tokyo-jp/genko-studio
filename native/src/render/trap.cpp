// PC2c (トラッピング, SPEC.md COLOR-04): the op trap on a CMYK page's paint layer (design 3.4, PhotoCraft v0.2.0's
// engine/trap_cmds.rs:19-43 and algo/trap.rs:1-65, MIT: third_party/photocraft/NOTICE.md; render/ops_adjust.hpp). Each
// ink spreads by the trap's width under the pixels that hold any ink, so where two colours meet they overlap and a
// misregistered press leaves no paper showing between them; the edge of a shape against the paper is left as it is.

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/ops_adjust.hpp"

namespace genko::render {

namespace image_ops {

namespace {

// The largest value within r of each of n values `stride` apart (the window cut at the ends), in time independent of r
// (van Herk / Gil-Werman: each block of 2r + 1 values' running maxima from its start and from its end).
void running_max(const float* in, float* out, std::size_t n, std::size_t stride, std::size_t r, std::vector<float>& g, std::vector<float>& h) {
    const std::size_t k = 2 * r + 1;
    const std::size_t padded = ((n + 2 * r + k - 1) / k) * k;
    constexpr float kNone = -std::numeric_limits<float>::infinity();
    const auto value = [&](std::size_t j) { return j < r || j - r >= n ? kNone : in[(j - r) * stride]; };
    g.resize(padded);
    h.resize(padded);
    for (std::size_t j = 0; j < padded; ++j) g[j] = j % k == 0 ? value(j) : std::max(g[j - 1], value(j));
    for (std::size_t j = padded; j-- > 0;) h[j] = j % k == k - 1 || j + 1 == padded ? value(j) : std::max(h[j + 1], value(j));
    // (window [x − r, x + r] of the values is [x, x + 2r] of the padded ones)
    for (std::size_t x = 0; x < n; ++x) out[x * stride] = std::max(h[x], g[x + 2 * r]);
}

// One ink's dilation (a max filter of radius r, rows then columns) of an interleaved buffer.
std::vector<float> dilate(const std::vector<float>& px, std::size_t w, std::size_t h, std::size_t ch, std::size_t c, std::size_t r) {
    std::vector<float> plane(w * h), tmp(w * h);
    for (std::size_t i = 0; i < w * h; ++i) plane[i] = px[i * ch + c];
    std::vector<float> g, hh;
    for (std::size_t y = 0; y < h; ++y) running_max(plane.data() + y * w, tmp.data() + y * w, w, 1, r, g, hh);
    for (std::size_t x = 0; x < w; ++x) running_max(tmp.data() + x, plane.data() + x, h, w, r, g, hh);
    return plane;
}

}  // namespace

void trap(std::vector<float>& px, std::size_t w, std::size_t h, std::size_t ch, std::size_t width) {
    if (width == 0 || w == 0 || h == 0 || ch < 4) return;
    const std::size_t inks = std::min<std::size_t>(4, ch);
    std::vector<std::vector<float>> spread;
    for (std::size_t c = 0; c < inks; ++c) spread.push_back(dilate(px, w, h, ch, c, width));
    for (std::size_t i = 0; i < w * h; ++i) {
        float* pixel = px.data() + i * ch;
        bool inked = false;  // (printed where any ink is)
        for (std::size_t c = 0; c < inks; ++c) inked = inked || pixel[c] > 1e-4f;
        if (!inked) continue;
        for (std::size_t c = 0; c < inks; ++c) pixel[c] = std::max(pixel[c], spread[c][i]);
    }
}

}  // namespace image_ops

namespace adjust_ops {

namespace {

using core::Json;
using core::OpError;

constexpr double kMostTrapPx = 100;  // (a trap is a hair's width: more is no trap)

double width_number(const Json& v, const char* key) {
    if (!v.is_number() || v.is_boolean() || !std::isfinite(v.get<double>())) throw OpError(std::string(key) + " must be a number");
    return v.get<double>();
}

}  // namespace

void trap(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const core::Page& page = doc.page(at);
    if (!page.color || page.color->space != core::InkSpace::Cmyk)
        throw OpError("trap works on a CMYK page (page " + page.index.repr() + " is not one)");
    const Json* id = core::get(op, "id");
    if (id == nullptr || !id->is_string()) throw OpError("id (the paint layer to trap) is required");
    const Json* px = core::get(op, "width_px");
    const Json* mm = core::get(op, "width_mm");
    if (px != nullptr && px->is_null()) px = nullptr;
    if (mm != nullptr && mm->is_null()) mm = nullptr;
    if (px != nullptr && mm != nullptr) throw OpError("give width_px or width_mm, not both");
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id->get<std::string>());
    if (p.kind() != image_ops::Pixels::Kind::Inks || p.colours() != 4) throw OpError("the layer " + id->get<std::string>() + " holds no inks to trap");
    // (the width in the layer's own pixels: width_mm at its resolution over the page)
    double width = 1;
    if (px != nullptr) width = width_number(*px, "width_px");
    if (mm != nullptr) width = width_number(*mm, "width_mm") * p.width() / doc.page(at).spec.width_mm.value();
    const auto pixels = static_cast<std::size_t>(std::clamp(std::round(width), 0.0, kMostTrapPx));
    std::vector<float> values(p.at(0), p.at(0) + p.size() * p.all());
    image_ops::trap(values, p.width(), p.height(), p.all(), pixels);
    std::copy(values.begin(), values.end(), p.at(0));
    image_ops::put_layer_pixels(doc, at, id->get<std::string>(), p);
    c.report = Json{{"trapped", true}, {"width_px", pixels}};
}

}  // namespace adjust_ops

}  // namespace genko::render
