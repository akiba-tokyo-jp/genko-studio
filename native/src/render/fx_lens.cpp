// PC4 節 D6 (SPEC.md COLOR-08「フィルター」): フィルター → レンズ補正… and 広角補正… as filter_raster's kinds on the three ways
// (render/fx.hpp) — PhotoCraft v0.2.0's algo/src/lens.rs remap and correct (the inverse map per output pixel, bicubic,
// premultiplied; the edge modes; red and blue sampled at their own places) and wideangle.rs apply (the solved mesh drawn
// by its triangles: warp.rs warp_triangles), MIT (native/third_party/photocraft/NOTICE.md). The geometry is core/lens; the
// triangles' places are render/resample's (PC3's mesh warp).
//
// Both work the frame of the picture — the box of what shows on the layer (Kernel::shown_frame): a photo placed on the page,
// a smart object's picture — as upstream works its canvas: the lens's centre is the frame's, its radius half the frame's
// diagonal, a selection only keeping the result to it. Lengths are the frame's own (a correction is the same at every
// resolution); 広角補正's constraints are places on the page (mm), its mesh solved at the working 200 dpi and laid on the
// layer's grid (every term of it is linear: the same solution scaled), so a 600 dpi page solves no more than a 200 dpi one.
// On a page of inks every plate moves as it is; 周辺光量 lightens or darkens the colour as upstream does (CMYK's and a
// multichannel page's inks 1 − (1 − v)·gain, a Lab page's L alone, grey's lightness).

#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "core/lens.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/resample.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::fx {

using core::Json;

namespace {

thread_local Json* g_notes = nullptr;  // (NoteScope)

constexpr double kMmPerInch = 25.4;
constexpr double kWorkingDpi = 200.0;

double number(const Json& params, const char* key, double fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_number() && !v->is_boolean() && std::isfinite(v->get<double>()) ? v->get<double>() : fallback;
}

std::string text(const Json& params, const char* key, const char* fallback) {
    const Json* v = core::get(params, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

// Catmull–Rom's weights (Keys a = −0.5) of the four taps around a place t (0..1) past the second.
std::array<float, 4> cubic(float t) {
    const float t2 = t * t, t3 = t2 * t;
    return {-0.5f * t3 + t2 - 0.5f * t, 1.5f * t3 - 2.5f * t2 + 1.0f, -1.5f * t3 + 2.0f * t2 + 0.5f * t, 0.5f * t3 - 0.5f * t2};
}

// A colour "paper" a clear pixel holds in the samples' space (so a layer of inks without an alpha gets its paper where the
// picture does not reach: CMYK and a multichannel page's no ink, a Lab page's white, grey's white; RGB's 0).
void paper(const Ctx& ctx, float* px) {
    for (unsigned c = 0; c < ctx.colours; ++c) {
        switch (ctx.space) {
            case Space::Lab: px[c] = c == 0 ? 1.0f : 128.0f / 255.0f; break;
            case Space::Gray: px[c] = 1.0f; break;
            case Space::Rgb: px[c] = 0.0f; break;
            default: px[c] = 0.0f; break;
        }
    }
}

// An RGB colour (0..1) as the samples' space's values (the edge's colour: black, white or its own).
std::vector<float> colour_in(const Ctx& ctx, const std::array<double, 3>& rgb) {
    std::vector<float> out(ctx.colours, 0.0f);
    const double grey = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
    switch (ctx.space) {
        case Space::Rgb:
            for (unsigned c = 0; c < ctx.colours && c < 3; ++c) out[c] = static_cast<float>(rgb[c]);
            break;
        case Space::Gray: out[0] = static_cast<float>(grey); break;
        case Space::Cmyk: {
            const double k = 1.0 - std::max({rgb[0], rgb[1], rgb[2]});
            for (unsigned c = 0; c < 3 && c < ctx.colours; ++c) out[c] = k >= 1.0 ? 0.0f : static_cast<float>((1.0 - rgb[c] - k) / (1.0 - k));
            if (ctx.colours > 3) out[3] = static_cast<float>(k);
            break;
        }
        case Space::Lab: {
            const segment::Rgb lab = segment::rgb_to_lab({static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2])});
            out[0] = std::clamp(lab[0] / 100.0f, 0.0f, 1.0f);
            if (ctx.colours > 1) out[1] = std::clamp((lab[1] + 128.0f) / 255.0f, 0.0f, 1.0f);
            if (ctx.colours > 2) out[2] = std::clamp((lab[2] + 128.0f) / 255.0f, 0.0f, 1.0f);
            break;
        }
        case Space::Multi:
            for (unsigned c = 0; c < ctx.colours; ++c) out[c] = static_cast<float>(1.0 - grey);
            break;
    }
    return out;
}

// --- レンズ補正 (lens.rs remap) ---------------------------------------------------------------------------------------------

// The bicubic of the window's channel c at p (its pixel centres at + 0.5), the place and its taps clamped to the frame (the
// window holds it): premultiplied by the alpha (`pre`), or the alpha itself.
float bicubic(const Window& src, const Rect& frame, std::size_t c, const core::lens::XY& p, bool pre) {
    const double x = std::clamp(p[0] - 0.5, static_cast<double>(frame.x0), static_cast<double>(frame.x1) - 1.0);
    const double y = std::clamp(p[1] - 0.5, static_cast<double>(frame.y0), static_cast<double>(frame.y1) - 1.0);
    const double xi = std::floor(x), yi = std::floor(y);
    const auto wx = cubic(static_cast<float>(x - xi)), wy = cubic(static_cast<float>(y - yi));
    const std::size_t a = src.ch - 1;
    float acc = 0.0f;
    for (int j = 0; j < 4; ++j) {
        const int yy = std::clamp(static_cast<int>(yi) + j - 1, src.rect.y0, src.rect.y1 - 1);
        for (int i = 0; i < 4; ++i) {
            const int xx = std::clamp(static_cast<int>(xi) + i - 1, src.rect.x0, src.rect.x1 - 1);
            const float* q = src.px(xx, yy);
            acc += wx[static_cast<std::size_t>(i)] * wy[static_cast<std::size_t>(j)] * (pre ? q[c] * q[a] : q[c]);
        }
    }
    return acc;
}

Kernel lens_correction() {
    Kernel k;
    k.kind = "lens_correction";
    k.shown_frame = true;
    k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
    k.cost = [](const Json&, const Ctx&) { return 160.0; };  // (16 taps of each colour and the alpha, three places)
    // (自動拡大・縮小: the least scale that covers the frame, found once — it is the frame's — and noted for the report)
    k.once = [](const Json& params, const Window&, const Ctx& ctx) {
        Json given = params;
        const core::lens::Correction lc = core::lens::correction_of(params);
        const Rect& f = ctx.bounds;
        double scale = lc.scale;
        if (lc.auto_scale) {
            scale = core::lens::auto_scale(lc, f.x0, f.y0, f.x1, f.y1);
            given["scale"] = scale;
            given["auto_scale"] = false;
        }
        NoteScope::put("scale", scale);
        return given;
    };
    k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
        core::lens::Correction lc = core::lens::correction_of(params);
        const Rect& frame = ctx.bounds;
        if (lc.auto_scale) lc.scale = core::lens::auto_scale(lc, frame.x0, frame.y0, frame.x1, frame.y1);  // (a run without once)
        const core::lens::Map map(lc, frame.x0, frame.y0, frame.x1, frame.y1);
        const std::size_t n = src.ch, cc = ctx.colours;
        const bool rgb = ctx.space == Space::Rgb && cc == 3;
        const bool floats = ctx.levels == 0;
        const double fx0 = frame.x0, fy0 = frame.y0, fx1 = frame.x1, fy1 = frame.y1;
        std::vector<float> edge(n, 0.0f), clear(n, 0.0f);
        paper(ctx, clear.data());
        if (lc.edge == core::lens::Edge::Colour) {
            const std::vector<float> made = colour_in(ctx, lc.edge_colour);
            std::copy(made.begin(), made.end(), edge.begin());
            edge[n - 1] = 1.0f;
        } else {
            edge = clear;
        }
        const bool usable = !src.rect.empty() && !frame.empty();
        std::vector<float> res(out.area() * n, 0.0f);
        std::vector<float> col(cc);
        std::size_t i = 0;
        for (int y = out.y0; y < out.y1; ++y)
            for (int x = out.x0; x < out.x1; ++x, i += n) {
                float* o = res.data() + i;
                const auto s = usable ? map.sample(x + 0.5, y + 0.5) : std::nullopt;
                if (!s) {
                    std::copy(edge.begin(), edge.end(), o);
                    continue;
                }
                // (a soft edge of a pixel inside the frame, but where the edge is carried on)
                float inside = 1.0f;
                if (lc.edge != core::lens::Edge::Extension) {
                    const double ix = std::clamp(std::min(s->p[0] - fx0, fx1 - s->p[0]) + 0.5, 0.0, 1.0);
                    const double iy = std::clamp(std::min(s->p[1] - fy0, fy1 - s->p[1]) + 0.5, 0.0, 1.0);
                    inside = static_cast<float>(ix * iy);
                }
                const float alpha = std::clamp(bicubic(src, frame, n - 1, s->p, false), 0.0f, 1.0f);
                for (std::size_t c = 0; c < cc; ++c) {
                    const core::lens::XY& at = rgb && c == 0 ? s->pr : rgb && c == 2 ? s->pb : s->p;
                    const float pm = bicubic(src, frame, c, at, true);
                    const float straight = alpha > 1e-6f ? pm / alpha : 0.0f;
                    const float g = s->gain;
                    float v = straight * g;
                    if (ctx.space == Space::Cmyk || ctx.space == Space::Multi) v = 1.0f - (1.0f - straight) * g;  // (ink: less of it)
                    else if (ctx.space == Space::Lab) v = c == 0 ? straight * g : straight;                      // (L alone)
                    if (!floats) v = std::clamp(v, 0.0f, 1.0f);
                    col[c] = v;
                }
                if (lc.edge == core::lens::Edge::Colour && inside < 1.0f) {
                    // (towards the edge's colour by the share outside)
                    const float oa = alpha * inside + 1.0f * (1.0f - inside);
                    for (std::size_t c = 0; c < cc; ++c) {
                        const float mixed = col[c] * alpha * inside + edge[c] * (1.0f - inside);
                        o[c] = oa > 1e-6f ? mixed / oa : 0.0f;
                    }
                    o[n - 1] = oa;
                    continue;
                }
                const float a_out = alpha * inside;
                if (a_out > 0.0f) std::copy(col.begin(), col.end(), o);
                else std::copy(clear.begin(), clear.end() - 1, o);
                o[n - 1] = a_out;
            }
        return res;
    };
    return k;
}

// --- 広角補正 (wideangle.rs apply: warp.rs warp_triangles) ---------------------------------------------------------------

// The settings on the surface's grid: the constraints' ends (the page's mm) as its pixels.
core::lens::WideAngle wide_of(const Json& params, const Ctx& ctx) {
    core::lens::WideAngle wa;
    const std::string model = text(params, "model", "auto");
    wa.model = model == "fisheye"          ? core::lens::Model::Fisheye
               : model == "perspective"    ? core::lens::Model::Perspective
               : model == "full_spherical" ? core::lens::Model::FullSpherical
                                           : core::lens::Model::Auto;
    wa.focal_length = std::clamp(number(params, "focal_length", 0), 0.0, 200.0);
    wa.crop_factor = std::clamp(number(params, "crop_factor", 1), 0.1, 10.0);
    wa.scale = std::clamp(number(params, "scale", 100), 50.0, 150.0);
    const double px = kWorkingDpi / kMmPerInch * ctx.s;
    if (const Json* list = core::get(params, "constraints"); list != nullptr && list->is_array()) {
        for (const Json& c : *list) {
            if (!c.is_object()) continue;
            const Json* a = core::get(c, "a");
            const Json* b = core::get(c, "b");
            if (a == nullptr || b == nullptr || !a->is_array() || !b->is_array() || a->size() != 2 || b->size() != 2) continue;
            core::lens::Constraint made;
            made.a = {(*a)[0].get<double>() * px - ctx.x0, (*a)[1].get<double>() * px - ctx.y0};
            made.b = {(*b)[0].get<double>() * px - ctx.x0, (*b)[1].get<double>() * px - ctx.y0};
            const std::string o = text(c, "orientation", "free");
            made.orientation = o == "horizontal" ? core::lens::Orientation::Horizontal
                               : o == "vertical" ? core::lens::Orientation::Vertical
                                                 : core::lens::Orientation::Free;
            wa.constraints.push_back(made);
        }
    }
    return wa;
}

// The mesh on the surface's grid: solved at the working 200 dpi (the frame and the constraints over s), laid back by s.
core::lens::Mesh wide_mesh(const Json& params, const Ctx& ctx) {
    core::lens::WideAngle wa = wide_of(params, ctx);
    const double s = ctx.s > 0 ? ctx.s : 1.0;
    for (core::lens::Constraint& c : wa.constraints) {
        c.a = {c.a[0] / s, c.a[1] / s};
        c.b = {c.b[0] / s, c.b[1] / s};
    }
    const Rect& f = ctx.bounds;
    core::lens::Mesh mesh = core::lens::solve(wa, f.x0 / s, f.y0 / s, f.x1 / s, f.y1 / s);
    for (auto& [o, src] : mesh.verts) {
        o = {o[0] * s, o[1] * s};
        src = {src[0] * s, src[1] * s};
    }
    return mesh;
}

Json mesh_json(const core::lens::Mesh& mesh) {
    std::vector<double> v;
    v.reserve(mesh.verts.size() * 4);
    for (const auto& [o, src] : mesh.verts) v.insert(v.end(), {o[0], o[1], src[0], src[1]});
    return Json{{"nx", mesh.nx}, {"ny", mesh.ny}, {"v", Json(v)}};
}

core::lens::Mesh mesh_of(const Json& made) {
    core::lens::Mesh mesh;
    mesh.nx = made["nx"].get<std::size_t>();
    mesh.ny = made["ny"].get<std::size_t>();
    const Json& v = made["v"];
    for (std::size_t i = 0; i + 3 < v.size(); i += 4)
        mesh.verts.push_back({core::lens::XY{v[i].get<double>(), v[i + 1].get<double>()}, core::lens::XY{v[i + 2].get<double>(), v[i + 3].get<double>()}});
    return mesh;
}

// The premultiplied sample of the window at (u, v) — its pixel centres at whole numbers — by `interp`, clear past the
// window (the frame), into `out` as straight colours and the alpha; false where nothing reaches.
bool sample_clear(const Window& src, double u, double v, resample::Interp interp, bool floats, float* out) {
    const std::size_t n = src.ch, a = n - 1;
    double acc[16] = {0};
    std::vector<double> wide;
    double* sum = acc;
    if (n > 16) {
        wide.assign(n, 0.0);
        sum = wide.data();
    }
    const auto add = [&](int x, int y, double w) {
        if (w == 0.0 || !src.rect.contains(x, y)) return;
        const float* q = src.px(x, y);
        for (std::size_t c = 0; c < a; ++c) sum[c] += static_cast<double>(q[c]) * q[a] * w;
        sum[a] += static_cast<double>(q[a]) * w;
    };
    switch (interp) {
        case resample::Interp::Nearest: add(static_cast<int>(std::floor(u + 0.5)), static_cast<int>(std::floor(v + 0.5)), 1.0); break;
        case resample::Interp::Bilinear: {
            const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
            const double fx = u - ix, fy = v - iy;
            add(ix, iy, (1 - fx) * (1 - fy));
            add(ix + 1, iy, fx * (1 - fy));
            add(ix, iy + 1, (1 - fx) * fy);
            add(ix + 1, iy + 1, fx * fy);
            break;
        }
        case resample::Interp::Bicubic: {
            // (within float noise of the grid: on it, so the identity is exact)
            const auto near = [](double t) { return std::fabs(t - std::round(t)) < 1e-7 ? std::round(t) : t; };
            u = near(u);
            v = near(v);
            const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
            const auto wx = cubic(static_cast<float>(u - ix)), wy = cubic(static_cast<float>(v - iy));
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) add(ix - 1 + i, iy - 1 + j, static_cast<double>(wx[std::size_t(i)]) * wy[std::size_t(j)]);
            break;
        }
    }
    const double al = std::clamp(sum[a], 0.0, 1.0);
    if (al <= 1e-6) return false;
    for (std::size_t c = 0; c < a; ++c) {
        const double value = sum[c] / al;
        out[c] = static_cast<float>(floats ? value : std::clamp(value, 0.0, 1.0));
    }
    out[a] = static_cast<float>(al);
    return true;
}

Kernel adaptive_wide_angle() {
    Kernel k;
    k.kind = "adaptive_wide_angle";
    k.shown_frame = true;
    k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
    k.cost = [](const Json&, const Ctx&) { return 120.0; };
    // (the mesh's solve: some 10⁸–10⁹ steps with constraints — on a thread, the window waiting — and none without)
    k.setup = [](const Json& params) {
        const Json* list = core::get(params, "constraints");
        return list != nullptr && list->is_array() && !list->empty() ? kHeavyWork : 0.0;
    };
    // (the mesh solved once for the frame, given to each tile, its model, focal length and straightness noted)
    k.once = [](const Json& params, const Window&, const Ctx& ctx) {
        const core::lens::Mesh mesh = wide_mesh(params, ctx);
        Json given = params;
        given["_mesh"] = mesh_json(mesh);
        const double s = ctx.s > 0 ? ctx.s : 1.0;
        const Rect& f = ctx.bounds;
        const core::lens::Camera cam = core::lens::Camera::of(wide_of(params, ctx), f.x0 / s, f.y0 / s, f.x1 / s, f.y1 / s);
        NoteScope::put("model", std::string(core::lens::to_string(cam.model)));
        NoteScope::put("focal_px", cam.f);  // (pixels of the working 200 dpi)
        NoteScope::put("residual", mesh.residual);
        NoteScope::put("constraints", mesh.curves.size());
        return given;
    };
    k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
        const Json* made = core::get(params, "_mesh");
        const core::lens::Mesh mesh = made != nullptr && made->is_object() ? mesh_of(*made) : wide_mesh(params, ctx);
        std::vector<resample::Corner> corners;
        corners.reserve(mesh.verts.size());
        for (const auto& [o, from] : mesh.verts) corners.push_back(resample::Corner{o[0], o[1], from[0], from[1]});
        const std::vector<std::array<std::size_t, 3>> tris = mesh.triangles();
        const std::vector<std::array<double, 2>> uv = resample::triangle_places(corners, tris, out.x0, out.y0, out.width(), out.height());
        const auto interp = resample::interp_from(text(params, "interpolation", "bicubic")).value_or(resample::Interp::Bicubic);
        const std::size_t n = src.ch;
        std::vector<float> clear(n, 0.0f);
        paper(ctx, clear.data());
        std::vector<float> res(out.area() * n);
        for (std::size_t i = 0; i < out.area(); ++i) {
            float* o = res.data() + i * n;
            const auto& q = uv[i];
            if (std::isnan(q[0]) || !sample_clear(src, q[0] - 0.5, q[1] - 0.5, interp, ctx.levels == 0, o)) std::copy(clear.begin(), clear.end(), o);
        }
        return res;
    };
    return k;
}

}  // namespace

NoteScope::NoteScope(Json& into) : before_(g_notes) { g_notes = &into; }
NoteScope::~NoteScope() { g_notes = before_; }
void NoteScope::put(std::string_view key, Json value) {
    if (g_notes == nullptr) return;
    if (!g_notes->is_object()) *g_notes = Json::object();
    (*g_notes)[std::string(key)] = std::move(value);
}

Kernels lens_kernels() { return {lens_correction(), adaptive_wide_angle()}; }

}  // namespace genko::render::fx
