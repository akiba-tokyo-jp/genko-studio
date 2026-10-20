#include "render/distort_ops.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/areas.hpp"
#include "render/fill_patches.hpp"
#include "render/image_ops.hpp"
#include "render/layer_transform.hpp"
#include "render/liquify_field.hpp"
#include "render/own_pixels.hpp"
#include "render/seam.hpp"
#include "render/selection.hpp"

// Edit › Content-Aware Scale: PhotoCraft v0.2.0 crates/engine/src/edit_menu_cmds.rs (429-491); Filter › Liquify:
// distort_cmds.rs (40-60, 120-240). MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render {

namespace {

using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpError;
using core::Page;
using own_pixels::OwnPixels;
using own_pixels::Window;

constexpr double kUnit = 25.4 / 200.0;  // (mm: a working pixel)

double number_in(const Json& op, const char* key, double fallback, double lo, double hi) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_number() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a number");
    const double x = v->get<double>();
    if (x < lo || x > hi) throw OpError(std::string(key) + " must be between " + core::py_repr(Json(lo)) + " and " + core::py_repr(Json(hi)));
    return x;
}

std::size_t layer_at(const Page& page, const std::string& id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i)
        if (page.layers[i].id == id) return i;
    throw core::OpKeyError(core::py_repr_str(id));
}

}  // namespace

// --- Content-Aware Scale ------------------------------------------------------------------------------------------------

void content_aware_scale(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* id_value = core::get(op, "id");
    if (id_value == nullptr || id_value->is_null()) id_value = core::get(op, "layer_id");
    if (id_value == nullptr || !id_value->is_string()) throw OpError("id is the paint layer's id");
    const std::string id = id_value->get<std::string>();
    {
        const Layer& seen = doc.page(at).layers[layer_at(doc.page(at), id)];
        if (seen.kind == LayerKind::Strokes) throw OpError("Content-Aware Scale works on a paint layer's pixels: rasterize the pen layer first");
        if (seen.kind == LayerKind::Smart) throw OpError("Content-Aware Scale works on a paint layer's pixels: rasterize the smart object first");
    }
    const double scale_x = number_in(op, "scale_x", 100.0, 1.0, 400.0);
    const double scale_y = number_in(op, "scale_y", 100.0, 1.0, 400.0);
    const double amount = number_in(op, "amount", 100.0, 0.0, 100.0) / 100.0;
    bool skin = false;
    if (const Json* s = core::get(op, "protect_skin"); s != nullptr && !s->is_null()) {
        if (!s->is_boolean()) throw OpError("protect_skin is true or false");
        skin = s->get<bool>();
    }
    std::optional<Json> area;
    if (const Json* a = core::get(op, "area"); a != nullptr && !a->is_null()) area = core::op_area(op);
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id);
    const Page& page = doc.page(at);
    const int gw = static_cast<int>(p.width()), gh = static_cast<int>(p.height());
    const unsigned ch = p.all();
    // (the region: the selection's box, or what the layer shows)
    Window region{gw, gh, 0, 0};
    std::vector<float> cov;
    if (area) cov = image_ops::coverage(page, *area, p.width(), p.height(), p.kind() == image_ops::Pixels::Kind::Eight);
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(gw) + std::size_t(x);
            const bool in = area ? cov[i] > 0.0f : p.at(i)[ch - 1] > 0.0f;
            if (!in) continue;
            region.x0 = std::min(region.x0, x);
            region.y0 = std::min(region.y0, y);
            region.x1 = std::max(region.x1, x + 1);
            region.y1 = std::max(region.y1, y + 1);
        }
    }
    if (region.empty()) throw OpError("nothing to scale: the layer is empty there");
    const std::size_t w = std::size_t(region.width()), h = std::size_t(region.height());
    const std::size_t nw = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(w) * scale_x / 100.0)));
    const std::size_t nh = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(h) * scale_y / 100.0)));
    const double mm_x = page.spec.width_mm.value() / gw, mm_y = page.spec.height_mm.value() / gh;
    Json report{{"from", Json::array({w, h})}, {"to", Json::array({nw, nh})},
                {"box_mm", Json::array({core::py_round(region.x0 * mm_x, 3), core::py_round(region.y0 * mm_y, 3), core::py_round(nw * mm_x, 3),
                                        core::py_round(nh * mm_y, 3)})}};
    if (nw == w && nh == h) {
        c.report = std::move(report);
        return;
    }
    const image_ops::Held held((std::uint64_t(w) * h + std::uint64_t(nw) * nh) * ch * 4 * 4);
    std::vector<float> img(w * h * ch);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            std::copy_n(p.at((std::size_t(region.y0) + y) * std::size_t(gw) + std::size_t(region.x0) + x), ch, img.data() + (y * w + x) * ch);
    // (what is protected: an alpha channel, the skin tones)
    std::optional<std::vector<float>> protect;
    if (const Json* pr = core::get(op, "protect"); pr != nullptr && !pr->is_null() && *pr != Json("none")) {
        if (!pr->is_string()) throw OpError("protect is an alpha channel's id or name, or none");
        const Json resolved = selection::resolve(Json{{"channel", *pr}}, page, &doc);
        const std::vector<float> keep = image_ops::coverage(page, resolved, p.width(), p.height(), p.kind() == image_ops::Pixels::Kind::Eight);
        protect.emplace(w * h);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) (*protect)[y * w + x] = keep[(std::size_t(region.y0) + y) * std::size_t(gw) + std::size_t(region.x0) + x];
    }
    if (skin) {
        std::vector<float> rgb(w * h * 3);
        if (p.kind() == image_ops::Pixels::Kind::Inks) {
            image_ops::Pixels part(p.kind(), static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), p.colours());
            part.ink = p.ink;
            for (std::size_t i = 0; i < w * h; ++i) std::copy_n(img.data() + i * ch, ch, part.at(i));
            const std::vector<image_ops::Rgba> shown = image_ops::rgba_of(part);
            for (std::size_t i = 0; i < w * h; ++i)
                for (std::size_t k = 0; k < 3; ++k) rgb[i * 3 + k] = std::clamp(shown[i][k], 0.0f, 1.0f);
        } else {
            for (std::size_t i = 0; i < w * h; ++i)
                for (std::size_t k = 0; k < 3; ++k) rgb[i * 3 + k] = std::clamp(img[i * ch + k], 0.0f, 1.0f);
        }
        const std::vector<float> s = seam::skin_mask(w, h, 3, rgb);
        if (!protect) {
            protect = s;
        } else {
            for (std::size_t i = 0; i < s.size(); ++i) (*protect)[i] = std::max((*protect)[i], s[i]);
        }
    }
    const std::vector<float> carved = amount > 0.0 ? seam::carve(w, h, ch, img, protect ? &*protect : nullptr, nw, nh) : std::vector<float>{};
    const std::vector<float> plain = amount < 1.0 ? seam::resize_bilinear(w, h, ch, img, nw, nh) : std::vector<float>{};
    std::vector<float> out(nw * nh * ch);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (plain.empty()) out[i] = carved[i];
        else if (carved.empty()) out[i] = plain[i];
        else out[i] = plain[i] + (carved[i] - plain[i]) * static_cast<float>(amount);
    }
    // (the region cleared, the scaled pixels laid at its top left)
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float* v = p.at((std::size_t(region.y0) + y) * std::size_t(gw) + std::size_t(region.x0) + x);
            std::fill_n(v, ch, 0.0f);
        }
    }
    for (std::size_t y = 0; y < nh; ++y) {
        const std::size_t gy = std::size_t(region.y0) + y;
        if (gy >= std::size_t(gh)) break;
        for (std::size_t x = 0; x < nw; ++x) {
            const std::size_t gx = std::size_t(region.x0) + x;
            if (gx >= std::size_t(gw)) break;
            const float* v = out.data() + (y * nw + x) * ch;
            float* d = p.at(gy * std::size_t(gw) + gx);
            for (unsigned k = 0; k < ch; ++k) d[k] = k + 1 == ch ? std::clamp(v[k], 0.0f, 1.0f) : v[k];
        }
    }
    image_ops::put_layer_pixels(doc, at, id, p);
    c.report = std::move(report);
}

// --- Liquify's strokes -------------------------------------------------------------------------------------------------

namespace {

std::vector<liquify::Stroke> strokes_of(const Json& value) {
    if (!value.is_array()) throw OpError("strokes is [{tool, size_mm, density, pressure, rate, points, amount?}…]");
    if (value.size() > 10000) throw OpError("liquify takes at most 10000 strokes");
    std::vector<liquify::Stroke> out;
    std::size_t points = 0;
    for (const Json& s : value) {
        if (!s.is_object()) throw OpError("strokes is [{tool, size_mm, density, pressure, rate, points, amount?}…]");
        for (const auto& [key, v] : s.items()) {
            if (key != "tool" && key != "size_mm" && key != "density" && key != "pressure" && key != "rate" && key != "points" && key != "amount")
                throw OpError("a liquify stroke has no key " + key);
        }
        liquify::Stroke st;
        const Json* tool = core::get(s, "tool");
        const auto found = tool != nullptr && tool->is_string() ? liquify::tool_from(tool->get<std::string>()) : std::optional<liquify::Tool>(liquify::Tool::ForwardWarp);
        if (!found || (tool != nullptr && !tool->is_null() && !tool->is_string()))
            throw OpError("a liquify stroke's tool is forward_warp, reconstruct, smooth, twirl_cw, twirl_ccw, pucker, bloat, push_left, freeze, thaw, "
                          "reconstruct_all, freeze_all, thaw_all or invert_freeze");
        st.tool = *found;
        st.size = number_in(s, "size_mm", 100.0 * kUnit, kUnit, 15000.0 * kUnit);
        st.density = number_in(s, "density", 50.0, 0.0, 100.0);
        st.pressure = number_in(s, "pressure", 100.0, 0.0, 100.0);
        st.rate = number_in(s, "rate", 80.0, 0.0, 100.0);
        if (const Json* a = core::get(s, "amount"); a != nullptr && !a->is_null()) st.amount = number_in(s, "amount", 100.0, 0.0, 100.0);
        if (const Json* pts = core::get(s, "points"); pts != nullptr && !pts->is_null()) {
            if (!pts->is_array()) throw OpError("a liquify stroke's points are [[x, y, pressure?]…] in mm");
            for (const Json& p : *pts) {
                if (!p.is_array() || p.size() < 2 || p.size() > 3) throw OpError("a liquify stroke's points are [[x, y, pressure?]…] in mm");
                std::array<double, 3> q{0, 0, 1.0};
                for (std::size_t k = 0; k < p.size(); ++k) {
                    if (!p[k].is_number() || !std::isfinite(p[k].get<double>())) throw OpError("a liquify stroke's points are finite numbers");
                    q[k] = p[k].get<double>();
                }
                st.points.push_back(q);
            }
            points += pts->size();
        }
        out.push_back(std::move(st));
    }
    if (points > 1'000'000) throw OpError("liquify takes at most a million points");
    return out;
}

// A line given more points where the field moves (pressure following), `step` apart.
void densify(core::Stroke& stroke, const std::array<double, 4>& box, double step) {
    const std::vector<core::PointF> pts = stroke.points;
    if (pts.size() < 2) return;
    const std::vector<double> pressure = stroke.pressure.size() == pts.size() ? stroke.pressure : std::vector<double>(pts.size(), 1.0);
    const bool rotated = stroke.rotation.size() == pts.size();
    std::vector<core::PointF> out{pts[0]};
    std::vector<double> out_p{pressure[0]};
    std::vector<double> out_r;
    if (rotated) out_r.push_back(stroke.rotation[0]);
    for (std::size_t k = 1; k < pts.size(); ++k) {
        const double ax = pts[k - 1].x, ay = pts[k - 1].y, bx = pts[k].x, by = pts[k].y;
        const bool touches = !(std::max(ax, bx) < box[0] || std::min(ax, bx) > box[2] || std::max(ay, by) < box[1] || std::min(ay, by) > box[3]);
        const std::size_t n = touches ? std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::hypot(bx - ax, by - ay) / step))) : 1;
        for (std::size_t i = 1; i <= n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(n);
            out.push_back(core::PointF{ax + (bx - ax) * t, ay + (by - ay) * t});
            out_p.push_back(pressure[k - 1] + (pressure[k] - pressure[k - 1]) * t);
            if (rotated) out_r.push_back(stroke.rotation[k - 1] + (stroke.rotation[k] - stroke.rotation[k - 1]) * t);
        }
    }
    if (out.size() == pts.size()) return;
    stroke.points = std::move(out);
    stroke.pressure = std::move(out_p);
    if (rotated) stroke.rotation = std::move(out_r);
}

// The plane drawn through the field: each place shows what was at it + d (the source plane's pixel (0, 0) at the grid's
// (sx0, sy0), the grid sx × sy pixels a mm).
resample::Plane through_field(const resample::Plane& src, int sx0, int sy0, const Window& dst, double sx, double sy, const liquify::Field& field) {
    const auto g = [&](double X, double Y) {
        const double x = X / sx, y = Y / sy;
        const auto d = field.sample_cubic(x, y);
        return std::pair<double, double>{(x + d[0]) * sx - sx0, (y + d[1]) * sy - sy0};
    };
    return resample::remap(src, g, dst.width(), dst.height(), resample::Interp::Bicubic, dst.x0, dst.y0);
}

}  // namespace

void liquify_strokes(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::vector<liquify::Stroke> strokes = strokes_of(op["strokes"]);
    Page& page = doc.edit_page(at);
    Layer& layer = core::paint_target(page, op);
    if (layer.kind == LayerKind::Smart) core::not_yet_ported("liquifying a smart object is not in the C++ build yet (PC3 A5)");
    if (layer.kind != LayerKind::Raster && layer.kind != LayerKind::Strokes) throw OpError("Liquify works on a pen or paint layer");
    if (layer.locked) throw OpError("the layer is locked");
    const double pw = page.spec.width_mm.value(), ph = page.spec.height_mm.value();
    double cell = liquify::auto_cell(pw, ph, kUnit);
    if (const Json* m = core::get(op, "mesh_mm"); m != nullptr && !m->is_null()) cell = number_in(op, "mesh_mm", cell, kUnit, 64.0 * kUnit);
    std::optional<Json> area;
    if (const Json* a = core::get(op, "area"); a != nullptr && !a->is_null()) area = core::op_area(op);
    liquify::Field field(0.0, 0.0, pw, ph, cell, kUnit);
    for (const liquify::Stroke& s : strokes) field.apply(s);
    const double most = field.max_displacement();
    Json report{{"changed", false}, {"max_displacement_mm", core::py_round(most, 4)}};
    const auto moved = field.moved_box();
    if (field.is_identity() || !moved) {
        c.report = std::move(report);
        return;
    }
    report["changed"] = true;
    std::optional<selection::AreaTest> inside;
    if (area) inside.emplace(*area);
    // its pen lines' points and shape parts moved by the field
    double smallest = 1e9;
    for (const liquify::Stroke& s : strokes)
        if (!liquify::is_global(s.tool)) smallest = std::min(smallest, s.size / 2.0);
    const double step = std::clamp(smallest / 4.0, kUnit, 1.5);
    const std::array<double, 4> reach{(*moved)[0] - most, (*moved)[1] - most, (*moved)[2] + most, (*moved)[3] + most};
    if (layer.stroke_count() > 0) {
        std::vector<core::StrokePtr> items = layer.strokes->items;
        for (core::StrokePtr& s : items) {
            auto stroke = std::make_shared<core::Stroke>(*s);
            densify(*stroke, reach, step);
            bool changed = false;
            for (core::PointF& p : stroke->points) {
                if (p.x < reach[0] || p.x > reach[2] || p.y < reach[1] || p.y > reach[3]) continue;
                if (inside && !inside->contains(p.x, p.y)) continue;
                const auto q = field.forward(p.x, p.y);
                p = core::PointF{core::py_round(q[0], 4), core::py_round(q[1], 4)};
                changed = true;
            }
            if (changed) s = std::move(stroke);
        }
        layer.strokes = core::make_strokes(std::move(items));
    }
    if (!layer.shapes->items.empty()) {
        layer_transform::Map m;
        m.kind = layer_transform::Map::Kind::Mesh;
        m.forward = [&field, &inside](double x, double y) {
            if (inside && !inside->contains(x, y)) return core::vector::XY{x, y};
            const auto q = field.forward(x, y);
            return core::vector::XY{q[0], q[1]};
        };
        std::vector<core::vector::ShapePart> items = layer.shapes->items;
        for (core::vector::ShapePart& part : items) {
            const auto b = part.path.control_bounds();
            if (!b || (*b)[2] < reach[0] || (*b)[0] > reach[2] || (*b)[3] < reach[1] || (*b)[1] > reach[3]) continue;
            part = layer_transform::part_through(part, m);
        }
        layer.shapes = core::vector::make_shapes(std::move(items));
    }
    // its patches and own pixels drawn through it once
    std::map<std::pair<int, int>, std::vector<float>> covers;  // (the area's coverage on each grid it is asked on)
    const auto mix = [&](resample::Plane& now, const resample::Plane& was, int x0, int y0, double sx, double sy, bool eight) {
        if (!area) return;
        const int gw = std::max(1, static_cast<int>(std::lround(pw * sx)));
        const int gh = std::max(1, static_cast<int>(std::lround(ph * sy)));
        auto& cov = covers[{gw, gh}];
        if (cov.empty()) cov = image_ops::coverage(page, *area, static_cast<std::uint32_t>(gw), static_cast<std::uint32_t>(gh), eight);
        const int a = now.channels - 1;
        for (int y = 0; y < now.height; ++y) {
            for (int x = 0; x < now.width; ++x) {
                const int gx = x0 + x, gy = y0 + y;
                const float k = gx >= 0 && gy >= 0 && gx < gw && gy < gh ? cov[std::size_t(gy) * std::size_t(gw) + std::size_t(gx)] : 0.0f;
                float* n = now.at(x, y);
                const float* o = was.at(x, y);
                const float oa = o[a], na = n[a];
                const float alpha = oa + (na - oa) * k;
                for (int ch = 0; ch < a; ++ch) {
                    const float pre = o[ch] * oa + (n[ch] * na - o[ch] * oa) * k;
                    n[ch] = alpha > 0.0f ? pre / alpha : 0.0f;
                }
                n[a] = alpha;
            }
        }
    };
    if (!layer.patches.empty()) {
        const int dpi = fills::kFillDpi;
        const double s = dpi / 25.4;
        std::vector<core::Patch> kept;
        for (const core::Patch& patch : layer.patches) {
            const Json* box = core::get(patch.attrs, "box");
            if (!patch.png || box == nullptr || !box->is_array() || box->size() != 4) {
                kept.push_back(patch);
                continue;
            }
            const double bx = (*box)[0].get<double>(), by = (*box)[1].get<double>(), bw = (*box)[2].get<double>(), bh = (*box)[3].get<double>();
            if (bx + bw < reach[0] || bx > reach[2] || by + bh < reach[1] || by > reach[3]) {
                kept.push_back(patch);
                continue;
            }
            const selection::PatchPicture pic = selection::patch_px(patch, dpi);
            bool grey = false;
            const resample::Plane plane = own_pixels::plane_of_picture(pic.image, grey);
            const Window dst = own_pixels::window_of(static_cast<double>(pic.x0) - most * s, static_cast<double>(pic.y0) - most * s,
                                                     static_cast<double>(pic.x0 + pic.image.width()) + most * s,
                                                     static_cast<double>(pic.y0 + pic.image.height()) + most * s, 2);
            resample::Plane out = through_field(plane, static_cast<int>(pic.x0), static_cast<int>(pic.y0), dst, s, s, field);
            if (area) {
                resample::Plane was(out.width, out.height, out.channels, true);
                for (int y = 0; y < out.height; ++y) {
                    for (int x = 0; x < out.width; ++x) {
                        const int px = dst.x0 + x - static_cast<int>(pic.x0), py = dst.y0 + y - static_cast<int>(pic.y0);
                        if (px >= 0 && py >= 0 && px < plane.width && py < plane.height) std::copy_n(plane.at(px, py), plane.channels, was.at(x, y));
                    }
                }
                mix(out, was, dst.x0, dst.y0, s, s, false);
            }
            if (auto made = selection::to_patch(own_pixels::picture_of_plane(out, grey), dst.x0, dst.y0, patch, dpi)) {
                if (const Json* id = core::get(patch.attrs, "id"); id != nullptr) made->attrs["id"] = *id;
                kept.push_back(std::move(*made));
            }
        }
        layer.patches = std::move(kept);
    }
    if (std::optional<OwnPixels> own = OwnPixels::of(page, layer)) {
        const double sx = own->width() / pw, sy = own->height() / ph;
        const Window dst = own_pixels::clamp_window(own_pixels::window_of((*moved)[0] * sx, (*moved)[1] * sy, (*moved)[2] * sx, (*moved)[3] * sy, 2),
                                                    own->width(), own->height());
        if (!dst.empty()) {
            const int pad = static_cast<int>(std::ceil(most * std::max(sx, sy))) + 3;
            const Window src = own_pixels::clamp_window(Window{dst.x0 - pad, dst.y0 - pad, dst.x1 + pad, dst.y1 + pad}, own->width(), own->height());
            const image_ops::Held held(own_pixels::work_bytes(std::uint64_t(src.width()) * std::uint64_t(src.height()) +
                                                                  std::uint64_t(dst.width()) * std::uint64_t(dst.height()),
                                                              own->channels()));
            const resample::Plane plane = own->read(src);
            resample::Plane out = through_field(plane, src.x0, src.y0, dst, sx, sy, field);
            if (area) mix(out, own->read(dst), dst.x0, dst.y0, sx, sy, own->kind() == OwnPixels::Kind::Eight);
            own->write(out, dst.x0, dst.y0);
            own->store(page, layer);
        }
    }
    c.report = std::move(report);
}

}  // namespace genko::render
