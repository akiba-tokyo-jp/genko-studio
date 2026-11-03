// レイヤーを自動整列・レイヤーを自動合成 and Photomerge's stitching (render/ops_align.hpp, PC5 I-g): PhotoCraft v0.2.0's
// engine/src/align_cmds.rs and photo_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md), on Genko's layers and pages.

#include "render/ops_align.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/content_aware.hpp"
#include "render/fill.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/layer_masks.hpp"
#include "render/layer_transform.hpp"
#include "render/pyramid.hpp"
#include "render/segment/segment.hpp"

namespace genko::render::align {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;
using panorama::Placement;
using panorama::XY;
using resample::Homography;

void progress(double done) { fx::ProgressScope::put(std::clamp(done, 0.0, 1.0)); }

std::size_t ceil_div(std::size_t a, std::size_t b) { return (a + b - 1) / std::max<std::size_t>(b, 1); }

double r6(double v) { return core::py_round(v, 6); }

// The layers an op names: two or more paint layers of the page, each once, in the page's order (the bottom first).
std::vector<std::string> chosen_layers(const core::Page& page, const Json& op) {
    const Json* given = core::get(op, "ids");
    if (given == nullptr || !given->is_array()) throw OpError("ids is [layer id, …]: two or more paint layers");
    std::set<std::string> wanted;
    for (const Json& v : *given) {
        if (!v.is_string()) throw OpError("ids is [layer id, …]: two or more paint layers");
        if (!wanted.insert(v.get<std::string>()).second) throw OpError("ids names the layer " + v.get<std::string>() + " twice");
    }
    if (wanted.size() < 2) throw OpError("select two or more paint layers (ids)");
    if (wanted.size() > kMaxLayers) throw OpError("more than " + std::to_string(kMaxLayers) + " layers: choose fewer");
    for (const std::string& id : wanted) (void)core::layer_by_id(page, id);  // (OpError: no such layer)
    std::vector<std::string> out;
    for (const core::Layer& layer : page.layers) {
        if (wanted.count(layer.id) == 0) continue;
        if (layer.kind != core::LayerKind::Raster) throw OpError("the layer " + layer.id + " is not a paint layer (auto-align and auto-blend take paint layers)");
        out.push_back(layer.id);
    }
    return out;
}

// Each row of the pixels as straight RGBA (an ink layer's: what its inks mean in RGB, worked in bands).
template <class F>
void rgba_rows(const Pixels& p, F&& row) {
    const std::size_t w = p.width(), h = p.height();
    std::vector<float> line(w * 4);
    if (p.kind() != Pixels::Kind::Inks) {
        const unsigned c = p.colours();
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const float* v = p.at(y * w + x);
                float* o = line.data() + x * 4;
                if (c >= 3) {
                    o[0] = v[0];
                    o[1] = v[1];
                    o[2] = v[2];
                } else {
                    o[0] = o[1] = o[2] = v[0];
                }
                o[3] = v[c];
            }
            row(y, line);
        }
        return;
    }
    const std::size_t band = 64;
    for (std::size_t y0 = 0; y0 < h; y0 += band) {
        const std::size_t rows = std::min(band, h - y0);
        Pixels part(Pixels::Kind::Inks, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(rows), p.colours());
        part.ink = p.ink;
        part.floating = p.floating;
        std::copy_n(p.at(y0 * w), rows * w * p.all(), part.at(0));
        const std::vector<image_ops::Rgba> rgba = image_ops::rgba_of(part);
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t x = 0; x < w; ++x) std::copy_n(rgba[r * w + x].data(), 4, line.data() + x * 4);
            row(y0 + r, line);
        }
    }
}

float luminance(const float* rgba) { return std::clamp(0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2], 0.0f, 1.0f); }

// The pixels' luminance and coverage on a sw × sh grid laid over them: each of their pixels into the cell it falls in (box) —
// on a grid finer than theirs (a mask grid of 200 dpi over a picture's), each cell the cell under its middle.
Shrunk shrink_to(const Pixels& p, std::size_t sw, std::size_t sh) {
    const std::size_t w = p.width(), h = p.height();
    const std::size_t tw = std::min(sw, std::max<std::size_t>(w, 1)), th = std::min(sh, std::max<std::size_t>(h, 1));
    Shrunk t;
    t.w = tw;
    t.h = th;
    t.luma.assign(tw * th, 0.0f);
    t.alpha.assign(tw * th, 0.0f);
    std::vector<float> count(tw * th, 0.0f);
    rgba_rows(p, [&](std::size_t y, const std::vector<float>& line) {
        const std::size_t ty = std::min(y * th / std::max<std::size_t>(h, 1), th - 1);
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = ty * tw + std::min(x * tw / std::max<std::size_t>(w, 1), tw - 1);
            const float* v = line.data() + x * 4;
            const float a = std::clamp(v[3], 0.0f, 1.0f);
            t.luma[i] += luminance(v) * a;
            t.alpha[i] += a;
            count[i] += 1.0f;
        }
    });
    for (std::size_t i = 0; i < tw * th; ++i) {
        t.luma[i] = t.alpha[i] > 0.0f ? t.luma[i] / t.alpha[i] : 0.0f;
        t.alpha[i] = count[i] > 0.0f ? t.alpha[i] / count[i] : 0.0f;
    }
    if (tw == sw && th == sh) return t;
    Shrunk s;
    s.w = sw;
    s.h = sh;
    s.luma.resize(sw * sh);
    s.alpha.resize(sw * sh);
    for (std::size_t y = 0; y < sh; ++y) {
        const std::size_t ty = std::min(static_cast<std::size_t>((static_cast<double>(y) + 0.5) * static_cast<double>(th) / static_cast<double>(sh)), th - 1);
        for (std::size_t x = 0; x < sw; ++x) {
            const std::size_t tx = std::min(static_cast<std::size_t>((static_cast<double>(x) + 0.5) * static_cast<double>(tw) / static_cast<double>(sw)), tw - 1);
            s.luma[y * sw + x] = t.luma[ty * tw + tx];
            s.alpha[y * sw + x] = t.alpha[ty * tw + tx];
        }
    }
    return s;
}

// A placement of shrunk indices as a map of a full grid whose pixel is sx × sy of them (continuous coordinates).
Homography scaled_matrix(const Homography& m, double sx, double sy) {
    const Homography s{{sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0}}, si{{1.0 / sx, 0.0, 0.0, 0.0, 1.0 / sy, 0.0, 0.0, 0.0, 1.0}};
    const Homography half{{1.0, 0.0, 0.5, 0.0, 1.0, 0.5, 0.0, 0.0, 1.0}}, back{{1.0, 0.0, -0.5, 0.0, 1.0, -0.5, 0.0, 0.0, 1.0}};
    Homography h = s.mul(half).mul(m).mul(back).mul(si);
    for (double& v : h.m) v /= h.m[8];
    return h;
}

XY scaled_forward(const Placement& p, double sx, double sy, double x, double y) {
    const XY q = p.forward(x / sx - 0.5, y / sy - 0.5);
    return XY{sx * (q[0] + 0.5), sy * (q[1] + 0.5)};
}

std::string model_of(panorama::Layout layout) {
    switch (layout) {
        case panorama::Layout::Reposition: return "reposition";
        case panorama::Layout::Collage: return "collage";
        case panorama::Layout::Cylindrical: return "cylindrical";
        case panorama::Layout::Spherical: return "spherical";
        default: return "perspective";
    }
}

bool planar(panorama::Layout layout) {
    return layout == panorama::Layout::Perspective || layout == panorama::Layout::Collage || layout == panorama::Layout::Reposition;
}

const char* kNoMatch = "found no matching detail between the layers (対応が見つかりません)";

// --- auto_align_layers -------------------------------------------------------------------------------------------------

void auto_align_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::vector<std::string> ids = chosen_layers(doc.page(at), c.op);
    const std::string projection = core::truthy_at(c.op, "projection") ? core::py_str(c.op["projection"]) : std::string("auto");
    const auto layout = panorama::layout_from(projection);
    if (!layout) throw OpError("projection is auto, perspective, cylindrical, spherical, collage or reposition");
    std::size_t ref = 0;
    if (const Json* given = core::get(c.op, "reference"); given != nullptr && !given->is_null()) {
        const auto found = given->is_string() ? std::find(ids.begin(), ids.end(), given->get<std::string>()) : ids.end();
        if (found == ids.end()) throw OpError("reference is one of the layers given (ids)");
        ref = static_cast<std::size_t>(found - ids.begin());
    }
    const bool geometric = core::truthy_at(c.op, "geometric");
    const std::string interp_name = core::truthy_at(c.op, "interpolation") ? core::py_str(c.op["interpolation"]) : std::string("bicubic");
    const auto interp = resample::interp_from(interp_name);
    if (!interp) throw OpError("interpolation is bicubic, bilinear or nearest");
    std::optional<double> focal35;
    if (const Json* f = core::get(c.op, "focal_length"); f != nullptr && !f->is_null()) {
        const double v = core::finite_float(*f, "focal_length");
        if (!(v > 0.0 && v <= 2000.0)) throw OpError("focal_length is the mm of 35 mm film (more than 0, at most 2000)");
        focal35 = v;
    }
    // (the locks first: a locked layer, or one whose position is locked, is refused before anything is read)
    for (const std::string& id : ids) (void)layer_transform::layers_moved(doc.page(at), {id});
    const ImageAllocationBudget budget{kBudget};
    // (each layer's luminance on one registration grid over the page, ≤ 1200 px: one layer's pixels held at a time)
    // (the grid: the first layer's shrunk by a whole factor; a layer of another grid laid over it, both stretched over the page)
    std::vector<Shrunk> shrunk;
    std::size_t reg_w = 0, reg_h = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        segment::check_stop();
        const Pixels p = image_ops::layer_pixels(c, at, ids[i]);
        if (i == 0) {
            const std::size_t k = std::max<std::size_t>(ceil_div(std::max<std::size_t>(p.width(), p.height()), kRegisterSide), 1);
            reg_w = std::max<std::size_t>(ceil_div(p.width(), k), 1);
            reg_h = std::max<std::size_t>(ceil_div(p.height(), k), 1);
        }
        shrunk.push_back(shrink_to(p, reg_w, reg_h));
        progress(0.3 * static_cast<double>(i + 1) / static_cast<double>(ids.size()));
    }
    std::optional<double> focal_px;
    if (focal35) focal_px = *focal35 / 36.0 * static_cast<double>(std::max(reg_w, reg_h));
    const auto al = register_shrunk(shrunk, *layout, ref, geometric, focal_px);
    if (!al) throw OpError(std::string("auto-align ") + kNoMatch);
    progress(0.6);
    const core::Rect paper = doc.page(at).paper_rect_mm();
    const double pw = paper.width.value(), ph = paper.height.value();
    const double sx = pw / static_cast<double>(reg_w), sy = ph / static_cast<double>(reg_h);  // (mm a registration pixel)
    std::vector<std::size_t> inliers(ids.size(), 0);
    for (const auto& [a, b, n] : al->pairs) {
        inliers[a] += n;
        inliers[b] += n;
    }
    // (in the plane layouts all but the reference move; in the others every layer is drawn again on the surface)
    std::vector<std::size_t> moves;
    Json failed = Json::array();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!al->placements[i]) {
            failed.push_back(ids[i]);
            continue;
        }
        if (i != al->reference || !planar(al->layout)) moves.push_back(i);
    }
    if (moves.empty()) throw OpError(std::string("auto-align ") + kNoMatch);
    Json aligned = Json::array();
    for (std::size_t n = 0; n < moves.size(); ++n) {
        segment::check_stop();
        const std::size_t i = moves[n];
        const Placement& pl = *al->placements[i];
        layer_transform::Map m;
        m.box = {0.0, 0.0, pw, ph};
        Json matrix = nullptr;
        if (const auto plane = panorama::plane_matrix(pl)) {
            const Homography h = scaled_matrix(*plane, sx, sy);
            matrix = Json::array();
            for (const double v : h.m) matrix.push_back(r6(v));
            if (std::fabs(h.m[6]) < 1e-12 && std::fabs(h.m[7]) < 1e-12) {
                m.kind = layer_transform::Map::Kind::Affine;
                m.affine = {h.m[0], h.m[3], h.m[1], h.m[4], h.m[2], h.m[5]};
                m.projective = resample::Homography::affine(m.affine);
            } else {
                m.kind = layer_transform::Map::Kind::Projective;
                m.projective = h;
            }
        } else {
            m.kind = layer_transform::Map::Kind::Mesh;
            m.forward = [pl, sx, sy](double x, double y) {
                const XY q = scaled_forward(pl, sx, sy, x, y);
                return layer_transform::XY{q[0], q[1]};
            };
        }
        layer_transform::move_layer(doc, at, ids[i], m, std::nullopt, *interp);
        aligned.push_back(Json{{"id", ids[i]}, {"model", model_of(al->layout)}, {"matches", inliers[i]}, {"matrix", matrix}});
        progress(0.6 + 0.4 * static_cast<double>(n + 1) / static_cast<double>(moves.size()));
    }
    const double focal_mm35 = al->focal / static_cast<double>(std::max(reg_w, reg_h)) * 36.0;
    c.report = Json{{"reference", ids[al->reference]},
                    {"layout", std::string(panorama::layout_name(al->layout))},
                    {"focal", r6(focal_mm35)},
                    {"rms", r6(al->rms * std::max(sx, sy))},
                    {"aligned", aligned},
                    {"failed", failed}};
}

// --- auto_blend_layers -------------------------------------------------------------------------------------------------

float sample(const std::vector<float>& v, std::size_t w, std::size_t h, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(w - 1));
    y = std::clamp(y, 0.0, static_cast<double>(h - 1));
    const std::size_t x0 = static_cast<std::size_t>(x), y0 = static_cast<std::size_t>(y);
    const std::size_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = static_cast<float>(x - static_cast<double>(x0)), ty = static_cast<float>(y - static_cast<double>(y0));
    const float top = v[y0 * w + x0] * (1 - tx) + v[y0 * w + x1] * tx;
    const float bot = v[y1 * w + x0] * (1 - tx) + v[y1 * w + x1] * tx;
    return top * (1 - ty) + bot * ty;
}

// The seams' weights of the panorama method (upstream's seam_blend without gains): on the work grid, one-hot.
std::vector<std::vector<float>> seam_weights(std::size_t w, std::size_t h, const std::vector<Shrunk>& s) {
    const std::size_t n = s.size();
    // (the order: the bottom layer first, then by how far their middles are from it)
    std::vector<std::array<double, 2>> middle(n, {0.0, 0.0});
    for (std::size_t i = 0; i < n; ++i) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (std::size_t p = 0; p < w * h; ++p) {
            if (s[i].alpha[p] <= 0.0f) continue;
            x0 = std::min(x0, static_cast<double>(p % w));
            y0 = std::min(y0, static_cast<double>(p / w));
            x1 = std::max(x1, static_cast<double>(p % w + 1));
            y1 = std::max(y1, static_cast<double>(p / w + 1));
        }
        middle[i] = x1 > x0 ? std::array<double, 2>{(x0 + x1) / 2.0, (y0 + y1) / 2.0} : std::array<double, 2>{0.0, 0.0};
    }
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if ((a != 0) != (b != 0)) return a == 0;
        return std::hypot(middle[a][0] - middle[0][0], middle[a][1] - middle[0][1]) < std::hypot(middle[b][0] - middle[0][0], middle[b][1] - middle[0][1]);
    });
    std::vector<std::size_t> rank(n, 0);
    for (std::size_t k = 0; k < n; ++k) rank[order[k]] = k;
    const std::size_t step = std::max<std::size_t>(ceil_div(std::max(w, h), kSeamSide), 1);
    const std::size_t gw = ceil_div(w, step), gh = ceil_div(h, step);
    std::vector<std::vector<float>> luma(n, std::vector<float>(gw * gh, 0.0f));
    std::vector<std::vector<bool>> cover(n, std::vector<bool>(gw * gh, false));
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<float> l(gw * gh, 0.0f), a(gw * gh, 0.0f), cnt(gw * gh, 0.0f);
        for (std::size_t p = 0; p < w * h; ++p) {
            const std::size_t g = std::min((p / w) / step, gh - 1) * gw + std::min((p % w) / step, gw - 1);
            l[g] += s[i].luma[p];
            a[g] += s[i].alpha[p];
            cnt[g] += 1.0f;
        }
        for (std::size_t g = 0; g < gw * gh; ++g) {
            if (cnt[g] <= 0.0f) continue;
            luma[i][g] = l[g] / cnt[g];
            cover[i][g] = a[g] / static_cast<float>(step * step) > 0.98f;
        }
    }
    const std::vector<int> labels = panorama::seam_labels(gw, gh, luma, cover, order);
    std::vector<std::vector<float>> out(n, std::vector<float>(w * h, 0.0f));
    for (std::size_t p = 0; p < w * h; ++p) {
        const int lab = labels[std::min((p / w) / step, gh - 1) * gw + std::min((p % w) / step, gw - 1)];
        std::optional<std::size_t> first;
        for (std::size_t j = 0; j < n; ++j) {
            if (s[j].alpha[p] >= 0.5f && (!first || rank[j] < rank[*first])) first = j;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (s[i].alpha[p] < 0.5f) continue;
            if (lab == static_cast<int>(i)) {
                out[i][p] = 1.0f;
            } else if (lab >= 0 && s[static_cast<std::size_t>(lab)].alpha[p] >= 0.5f) {
                out[i][p] = 0.0f;
            } else {
                out[i][p] = first == i ? 1.0f : 0.0f;
            }
        }
    }
    return out;
}

void auto_blend_layers(core::OpContext& c) {
    core::Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::vector<std::string> ids = chosen_layers(doc.page(at), c.op);
    const std::string method = core::truthy_at(c.op, "method") ? core::py_str(c.op["method"]) : std::string("panorama");
    if (method != "panorama" && method != "stack") throw OpError("method is panorama or stack");
    const bool seamless = core::has(c.op, "seamless") ? core::py_truthy(c.op["seamless"]) : true;
    for (const std::string& id : ids) {
        if (doc.page(at).layers[core::layer_by_id(doc.page(at), id)].locked) throw OpError("the layer is locked");
    }
    std::string new_id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : std::string();
    if (!new_id.empty()) {
        for (const core::Layer& l : doc.page(at).layers) {
            if (l.id == new_id) throw OpError("layer " + new_id + " exists");
        }
    }
    const ImageAllocationBudget budget{kBudget};
    // (the layers' pixels — one kind and one grid —, each held: the budget refuses too many or too large)
    std::vector<Pixels> px;
    px.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        segment::check_stop();
        px.push_back(image_ops::layer_pixels(c, at, ids[i]));
        const Pixels& p = px.back();
        if (p.kind() != px.front().kind() || p.width() != px.front().width() || p.height() != px.front().height() || p.colours() != px.front().colours()) {
            throw OpError("the layers to blend hold different kinds of pixels (8 bits, high precision, inks): convert them to one first");
        }
        progress(0.2 * static_cast<double>(i + 1) / static_cast<double>(ids.size()));
    }
    const std::size_t gw = px.front().width(), gh = px.front().height(), ch = px.front().colours(), n = ids.size();
    bool any = false;
    for (const Pixels& p : px) {
        for (std::size_t i = 0; i < p.size() && !any; ++i) any = p.at(i)[ch] > 0.0f;
    }
    if (!any) throw OpError("the layers have nothing to draw");
    // (the weights on the masks' grid shrunk to 2048 px at most)
    const core::Page& page = doc.page(at);
    const Size mgrid = layer_masks::grid_size(page);
    const std::size_t mw = static_cast<std::size_t>(std::max(mgrid.width, 1)), mh = static_cast<std::size_t>(std::max(mgrid.height, 1));
    const std::size_t kk = std::max<std::size_t>(ceil_div(std::max(mw, mh), kWorkSide), 1);
    const std::size_t ww = std::max<std::size_t>(ceil_div(mw, kk), 1), wh = std::max<std::size_t>(ceil_div(mh, kk), 1);
    const image_ops::Held held(std::uint64_t(n) * ww * wh * sizeof(float) * 4);
    std::vector<Shrunk> s;
    for (const Pixels& p : px) s.push_back(shrink_to(p, ww, wh));
    progress(0.35);
    std::vector<std::vector<float>> weights;
    if (method == "stack") {
        std::vector<std::vector<float>> luma, alpha;
        for (const Shrunk& each : s) {
            luma.push_back(each.luma);
            alpha.push_back(each.alpha);
        }
        weights = pyramid::stack_weights(ww, wh, luma, alpha);
    } else {
        weights = seam_weights(ww, wh, s);
    }
    progress(0.5);
    // (each layer's mask: its weights at the masks' grid)
    core::Page& edited = doc.edit_page(at);
    for (std::size_t i = 0; i < n; ++i) {
        std::string bytes(mw * mh, '\0');
        for (std::size_t y = 0; y < mh; ++y) {
            const double sy = (static_cast<double>(y) + 0.5) * static_cast<double>(wh) / static_cast<double>(mh) - 0.5;
            for (std::size_t x = 0; x < mw; ++x) {
                const double sx = (static_cast<double>(x) + 0.5) * static_cast<double>(ww) / static_cast<double>(mw) - 0.5;
                bytes[y * mw + x] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(sample(weights[i], ww, wh, sx, sy), 0.0f, 1.0f) * 255.0f)));
            }
        }
        core::Layer& layer = edited.layers[core::layer_by_id(edited, ids[i])];
        const std::optional<core::Mask> before = layer.mask;
        core::Mask mask;
        mask.png = fills::png_bytes(Image::frombytes("L", Size{static_cast<int>(mw), static_cast<int>(mh)}, bytes));
        mask.enabled = true;
        if (before) {
            mask.linked = before->linked;
            mask.density = before->density;
            mask.feather_mm = before->feather_mm;
        }
        layer.mask = std::move(mask);
    }
    progress(0.6);
    Json blended = nullptr;
    if (seamless) {
        // (the merged layer: Laplacian pyramids in 1024 px tiles, its colours where the layers show, its alpha their union)
        const std::size_t levels = std::min(method == "stack" ? pyramid::auto_levels(gw, gh) : panorama::blend_levels(gw, gh), pyramid::kMaxTiledLevels);
        Pixels out = px.front();
        for (std::size_t i = 0; i < out.size(); ++i) std::fill_n(out.at(i), ch + 1, 0.0f);
        const bool clamp = !(out.kind() == Pixels::Kind::Precise && out.floating);
        const auto colours = [&](std::size_t i, std::size_t x0, std::size_t y0, std::size_t w, std::size_t h, std::vector<float>& to) {
            std::vector<float> cover(w * h, 0.0f);
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    const float* v = px[i].at((y0 + y) * gw + x0 + x);
                    std::copy_n(v, ch, to.data() + (y * w + x) * ch);
                    cover[y * w + x] = v[ch] > 0.5f ? 1.0f : 0.0f;
                }
            }
            panorama::fill_holes(w, h, ch, to, cover);
        };
        const auto weight = [&](std::size_t i, std::size_t x0, std::size_t y0, std::size_t w, std::size_t h, std::vector<float>& to) {
            for (std::size_t y = 0; y < h; ++y) {
                const double sy = (static_cast<double>(y0 + y) + 0.5) * static_cast<double>(wh) / static_cast<double>(gh) - 0.5;
                for (std::size_t x = 0; x < w; ++x) {
                    const double sx = (static_cast<double>(x0 + x) + 0.5) * static_cast<double>(ww) / static_cast<double>(gw) - 0.5;
                    to[y * w + x] = sample(weights[i], ww, wh, sx, sy) * std::clamp(px[i].at((y0 + y) * gw + x0 + x)[ch], 0.0f, 1.0f);
                }
            }
        };
        std::size_t tiles = 0;
        const std::size_t all_tiles = ceil_div(gw, pyramid::kTile) * ceil_div(gh, pyramid::kTile);
        pyramid::blend_tiled(gw, gh, ch, n, colours, weight, levels, clamp,
                             [&](std::size_t x0, std::size_t y0, std::size_t w, std::size_t h, const std::vector<float>& core_px) {
                                 for (std::size_t y = 0; y < h; ++y) {
                                     for (std::size_t x = 0; x < w; ++x) {
                                         const std::size_t i = (y0 + y) * gw + x0 + x;
                                         float a = 0.0f;
                                         for (const Pixels& p : px) a = std::max(a, std::clamp(p.at(i)[ch], 0.0f, 1.0f));
                                         float* o = out.at(i);
                                         if (a > 0.0f) std::copy_n(core_px.data() + (y * w + x) * ch, ch, o);
                                         o[ch] = a;
                                     }
                                 }
                                 progress(0.6 + 0.4 * static_cast<double>(++tiles) / static_cast<double>(std::max<std::size_t>(all_tiles, 1)));
                             });
        // (above the top layer, in its folder)
        std::size_t top = 0;
        for (std::size_t i = 0; i < edited.layers.size(); ++i) {
            if (edited.layers[i].id == ids.back()) top = i;
        }
        core::Layer layer;
        layer.id = new_id.empty() ? core::new_id() : new_id;
        layer.role = core::LayerRole::User;
        layer.kind = core::LayerKind::Raster;
        layer.title = core::truthy_at(c.op, "name") ? core::py_str(c.op["name"]) : std::string("自動合成");
        layer.parent_id = edited.layers[top].parent_id;
        layer.panel_clip = edited.layers[top].panel_clip;
        layer.panel_each = edited.layers[top].panel_each;
        const std::string made = layer.id;
        edited.layers.insert(edited.layers.begin() + static_cast<std::ptrdiff_t>(top + 1), std::move(layer));
        image_ops::put_layer_pixels(doc, at, made, out);
        blended = made;
    }
    progress(1.0);
    Json listed = Json::array();
    for (const std::string& id : ids) listed.push_back(id);
    c.report = Json{{"method", method}, {"ids", listed}, {"blended", blended}};
}

}  // namespace

std::optional<panorama::Alignment> register_shrunk(const std::vector<Shrunk>& shrunk, panorama::Layout layout, std::optional<std::size_t> reference,
                                                   bool geometric, std::optional<double> focal_px) {
    std::vector<panorama::ImageFeatures> feats;
    for (const Shrunk& s : shrunk) {
        segment::check_stop();
        bool partial = false;
        for (const float a : s.alpha) partial = partial || a < 0.5f;
        std::vector<bool> valid;
        if (partial) {
            valid.resize(s.alpha.size());
            for (std::size_t i = 0; i < s.alpha.size(); ++i) valid[i] = s.alpha[i] > 0.5f;
        }
        feats.push_back(panorama::detect(s.w, s.h, s.luma, partial ? &valid : nullptr, kFeatures));
    }
    const std::vector<panorama::PairMatch> matches = panorama::match_pairs(feats);
    panorama::AlignOptions o;
    o.layout = layout;
    o.focal = focal_px;
    o.reference = reference;
    o.geometric = geometric;
    return panorama::align(feats, matches, o);
}

std::optional<resample::Homography> full_matrix(const Placement& p, double k) {
    const auto plane = panorama::plane_matrix(p);
    if (!plane) return std::nullopt;
    return scaled_matrix(*plane, k, k);
}

XY full_forward(const Placement& p, double k, double x, double y) { return scaled_forward(p, k, k, x, y); }

std::optional<XY> full_inverse(const Placement& p, double k, double u, double v) {
    const auto s = p.inverse(u / k - 0.5, v / k - 0.5);
    if (!s) return std::nullopt;
    return XY{k * ((*s)[0] + 0.5), k * ((*s)[1] + 0.5)};
}

// --- Photomerge ----------------------------------------------------------------------------------------------------------

std::uint64_t photomerge_bytes(std::uint64_t canvas, std::uint64_t parts, bool blend, bool fill) {
    // (each picture over its box: RGBA floats warped, its colours and coverage and weights; the blend's levels (a third more)
    // of the panorama's colours and weights and its coverage; the fill's panorama of colours twice; a layer of the book made
    // at a time: the panorama's RGBA floats)
    std::uint64_t bytes = parts * (4 + 3 + 1 + 1) * sizeof(float) + canvas * 4 * sizeof(float);
    if (blend) bytes += canvas * (3 + 1) * sizeof(float) * 4 / 3 * 2 + canvas * sizeof(float);
    if (fill) bytes += canvas * 4 * sizeof(float) * 2;
    return bytes;
}

Merged photomerge(const std::vector<Source>& sources, const MergeOptions& options) {
    const std::size_t n = sources.size();
    if (n < 2) throw OpError("Photomerge needs two or more pictures");
    if (n > kMaxLayers) throw OpError("more than " + std::to_string(kMaxLayers) + " pictures: choose fewer");
    std::size_t side = 1;
    for (const Source& s : sources) side = std::max<std::size_t>(side, static_cast<std::size_t>(std::max(s.plane.width, s.plane.height)));
    const std::size_t k = std::max<std::size_t>(ceil_div(side, kRegisterSide), 1);
    std::vector<Shrunk> shrunk;
    for (const Source& s : sources) {
        segment::check_stop();
        const std::size_t w = static_cast<std::size_t>(s.plane.width), h = static_cast<std::size_t>(s.plane.height);
        const std::size_t sw = std::max<std::size_t>(ceil_div(w, k), 1), sh = std::max<std::size_t>(ceil_div(h, k), 1);
        Shrunk out;
        out.w = sw;
        out.h = sh;
        out.luma.assign(sw * sh, 0.0f);
        out.alpha.assign(sw * sh, 0.0f);
        std::vector<float> count(sw * sh, 0.0f);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const float* v = s.plane.at(static_cast<int>(x), static_cast<int>(y));
                const float a = std::clamp(v[3], 0.0f, 1.0f);
                const std::size_t i = (y / k) * sw + x / k;
                out.luma[i] += luminance(v) * a;
                out.alpha[i] += a;
                count[i] += 1.0f;
            }
        }
        for (std::size_t i = 0; i < sw * sh; ++i) {
            out.luma[i] = out.alpha[i] > 0.0f ? out.luma[i] / out.alpha[i] : 0.0f;
            out.alpha[i] = count[i] > 0.0f ? out.alpha[i] / count[i] : 0.0f;
        }
        shrunk.push_back(std::move(out));
    }
    progress(0.1);
    std::optional<double> focal_px;
    if (options.focal35) focal_px = *options.focal35 / 36.0 * static_cast<double>(side) / static_cast<double>(k);
    const auto al = register_shrunk(shrunk, options.layout, std::nullopt, options.geometric, focal_px);
    if (!al) throw OpError("photomerge found no matching detail between the pictures (対応が見つかりません)");
    progress(0.3);
    const double kd = static_cast<double>(k);
    Merged out;
    out.layout = std::string(panorama::layout_name(al->layout));
    out.reference = al->reference;
    for (std::size_t i = 0; i < n; ++i) (al->placements[i] ? out.placed : out.failed).push_back(i);
    // (the canvas: the placed pictures' union)
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    std::vector<std::array<double, 4>> boxes;
    for (const std::size_t i : out.placed) {
        const double w = sources[i].plane.width, h = sources[i].plane.height;
        std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
        for (int t = 0; t <= 64; ++t) {
            const double f = t / 64.0;
            for (const XY& q : {XY{f * w, 0.0}, XY{f * w, h}, XY{0.0, f * h}, XY{w, f * h}}) {
                const XY u = full_forward(*al->placements[i], kd, q[0], q[1]);
                if (!std::isfinite(u[0]) || !std::isfinite(u[1])) continue;
                b = {std::min(b[0], u[0]), std::min(b[1], u[1]), std::max(b[2], u[0]), std::max(b[3], u[1])};
            }
        }
        boxes.push_back(b);
        x0 = std::min(x0, b[0]);
        y0 = std::min(y0, b[1]);
        x1 = std::max(x1, b[2]);
        y1 = std::max(y1, b[3]);
    }
    const double cwf = std::ceil(x1 - x0), chf = std::ceil(y1 - y0);
    const auto too_big = [&](double w, double h) {
        std::ostringstream said;
        said << "the panorama would be " << static_cast<long long>(std::isfinite(w) ? w : -1) << " × " << static_cast<long long>(std::isfinite(h) ? h : -1)
             << " px (about " << static_cast<long long>(std::isfinite(w * h) ? w * h / 10000.0 : -1) << " 万 pixels): use fewer pictures or shrink them when they are read";
        return OpError(said.str());
    };
    if (!(std::isfinite(cwf) && std::isfinite(chf)) || cwf < 1 || chf < 1 || cwf > 30000.0 || chf > 30000.0 ||
        (options.max_pixels > 0 && cwf * chf > static_cast<double>(options.max_pixels))) {
        throw too_big(cwf, chf);
    }
    const int cw = static_cast<int>(cwf), chh = static_cast<int>(chf);
    out.width = cw;
    out.height = chh;
    const double ox = -std::floor(x0), oy = -std::floor(y0);
    // (each picture's box in the canvas)
    std::vector<std::array<int, 4>> roi;
    std::uint64_t parts = 0;
    for (const auto& b : boxes) {
        const int rx0 = std::clamp(static_cast<int>(std::floor(b[0] + ox)), 0, cw), ry0 = std::clamp(static_cast<int>(std::floor(b[1] + oy)), 0, chh);
        const int rx1 = std::clamp(static_cast<int>(std::ceil(b[2] + ox)), 0, cw), ry1 = std::clamp(static_cast<int>(std::ceil(b[3] + oy)), 0, chh);
        roi.push_back({rx0, ry0, rx1, ry1});
        parts += std::uint64_t(std::max(rx1 - rx0, 0)) * std::uint64_t(std::max(ry1 - ry0, 0));
    }
    if (options.budget > 0 && photomerge_bytes(std::uint64_t(cw) * std::uint64_t(chh), parts, options.blend, options.content_aware_fill) > options.budget) {
        throw too_big(cwf, chf);
    }
    // (each picture drawn into its box: a plane's homography, a cylinder's or sphere's inverse map)
    std::vector<resample::Plane> warped;
    for (std::size_t j = 0; j < out.placed.size(); ++j) {
        segment::check_stop();
        const std::size_t i = out.placed[j];
        const Placement pl = al->placements[i]->translated(ox / kd, oy / kd);
        const auto& r = roi[j];
        const int rw = std::max(r[2] - r[0], 1), rh = std::max(r[3] - r[1], 1);
        if (const auto h = full_matrix(pl, kd)) {
            warped.push_back(resample::project(sources[i].plane, *h, rw, rh, resample::Interp::Bicubic, r[0], r[1]));
        } else {
            warped.push_back(resample::remap(sources[i].plane, [&pl, kd](double u, double v) {
                const auto s = full_inverse(pl, kd, u, v);
                return s ? std::pair<double, double>{(*s)[0], (*s)[1]} : std::pair<double, double>{-1e9, -1e9};
            }, rw, rh, resample::Interp::Bicubic, r[0], r[1]));
        }
        progress(0.3 + 0.3 * static_cast<double>(j + 1) / static_cast<double>(out.placed.size()));
    }
    Json pairs = Json::array();
    for (const auto& [a, b, m] : al->pairs) pairs.push_back(Json::array({a, b, m}));
    out.info = Json{{"focal_length35", r6(al->focal / static_cast<double>(side) * kd * 36.0)}, {"geometric_k1", r6(al->k1)}, {"rms", r6(al->rms * kd)}, {"pairs", pairs}};
    if (!options.blend) {
        for (std::size_t j = 0; j < warped.size(); ++j) out.layers.push_back(Part{roi[j][0], roi[j][1], std::move(warped[j])});
        progress(1.0);
        return out;
    }
    // (gains and vignetting, the seams on ≤ 1000 px, the multi-band blend — upstream's seam_blend)
    std::vector<panorama::RoiImage> rois;
    for (std::size_t j = 0; j < warped.size(); ++j) {
        panorama::RoiImage im;
        im.x0 = roi[j][0];
        im.y0 = roi[j][1];
        im.w = static_cast<std::size_t>(warped[j].width);
        im.h = static_cast<std::size_t>(warped[j].height);
        im.ch = 3;
        im.px.resize(im.w * im.h * 3);
        im.alpha.resize(im.w * im.h);
        for (std::size_t p = 0; p < im.w * im.h; ++p) {
            const float* v = warped[j].v.data() + p * 4;
            std::copy_n(v, 3, im.px.data() + p * 3);
            im.alpha[p] = v[3];
        }
        rois.push_back(std::move(im));
    }
    const bool hdr = !sources.empty() && sources.front().plane.hdr;
    std::vector<Placement> shifted;
    std::vector<std::pair<std::size_t, std::size_t>> sizes;
    for (const std::size_t i : out.placed) {
        shifted.push_back(al->placements[i]->translated(ox / kd, oy / kd).scaled(kd));
        sizes.emplace_back(static_cast<std::size_t>(sources[i].plane.width), static_cast<std::size_t>(sources[i].plane.height));
    }
    const panorama::Photometric photo = panorama::photometric(rois, options.vignette ? &shifted : nullptr, options.vignette ? &sizes : nullptr, true, options.vignette);
    const bool vig = photo.vignette[0] != 0.0 || photo.vignette[1] != 0.0;
    for (std::size_t j = 0; j < rois.size(); ++j) {
        panorama::RoiImage& im = rois[j];
        bool same = !vig;
        for (const float g : photo.gains[j]) same = same && std::fabs(g - 1.0f) < 1e-6f;
        if (same) continue;
        for (std::size_t y = 0; y < im.h; ++y) {
            for (std::size_t x = 0; x < im.w; ++x) {
                double r2 = 0.0;
                if (vig) {
                    // (the placement scaled to full pixels works in index coordinates near enough for the falloff)
                    if (const auto s = shifted[j].inverse(static_cast<double>(im.x0 + static_cast<int>(x)) + 0.5, static_cast<double>(im.y0 + static_cast<int>(y)) + 0.5)) {
                        const double w = static_cast<double>(sizes[j].first), h = static_cast<double>(sizes[j].second);
                        const double dx = (*s)[0] - w / 2.0, dy = (*s)[1] - h / 2.0;
                        r2 = (dx * dx + dy * dy) / (w * w / 4.0 + h * h / 4.0);
                    }
                }
                const std::size_t p = y * im.w + x;
                for (std::size_t cc = 0; cc < 3; ++cc) {
                    float& v = im.px[p * 3 + cc];
                    v *= photo.factor(j, cc, r2);
                    if (!hdr) v = std::clamp(v, 0.0f, 1.0f);
                }
            }
        }
    }
    progress(0.65);
    const std::size_t W = static_cast<std::size_t>(cw), H = static_cast<std::size_t>(chh);
    const std::size_t s = std::max<std::size_t>(ceil_div(std::max(W, H), kSeamSide), 1);
    const std::size_t gw = ceil_div(W, s), gh = ceil_div(H, s);
    const std::size_t m = rois.size();
    std::vector<std::vector<float>> luma(m, std::vector<float>(gw * gh, 0.0f));
    std::vector<std::vector<bool>> cover(m, std::vector<bool>(gw * gh, false));
    for (std::size_t j = 0; j < m; ++j) {
        const auto& im = rois[j];
        std::vector<float> l(gw * gh, 0.0f), a(gw * gh, 0.0f), cnt(gw * gh, 0.0f);
        for (std::size_t y = 0; y < im.h; ++y) {
            for (std::size_t x = 0; x < im.w; ++x) {
                const std::size_t u = static_cast<std::size_t>(im.x0) + x, v = static_cast<std::size_t>(im.y0) + y;
                const std::size_t g = std::min(v / s, gh - 1) * gw + std::min(u / s, gw - 1);
                const std::size_t p = y * im.w + x;
                l[g] += (im.px[p * 3] + im.px[p * 3 + 1] + im.px[p * 3 + 2]) / 3.0f;
                a[g] += im.alpha[p];
                cnt[g] += 1.0f;
            }
        }
        for (std::size_t g = 0; g < gw * gh; ++g) {
            if (cnt[g] <= 0.0f) continue;
            luma[j][g] = l[g] / cnt[g];
            cover[j][g] = a[g] / static_cast<float>(s * s) > 0.98f;
        }
    }
    // (the seams' order: the reference first, then by how far each middle is from its)
    const std::size_t ref = static_cast<std::size_t>(std::find(out.placed.begin(), out.placed.end(), al->reference) - out.placed.begin());
    const auto middle = [&](std::size_t j) { return std::array<double, 2>{(roi[j][0] + roi[j][2]) / 2.0, (roi[j][1] + roi[j][3]) / 2.0}; };
    std::vector<std::size_t> order(m);
    for (std::size_t j = 0; j < m; ++j) order[j] = j;
    const auto rc = middle(std::min(ref, m - 1));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if ((a != ref) != (b != ref)) return a == ref;
        const auto ca = middle(a), cb = middle(b);
        return std::hypot(ca[0] - rc[0], ca[1] - rc[1]) < std::hypot(cb[0] - rc[0], cb[1] - rc[1]);
    });
    std::vector<std::size_t> rank(m, 0);
    for (std::size_t q = 0; q < m; ++q) rank[order[q]] = q;
    const std::vector<int> labels = panorama::seam_labels(gw, gh, luma, cover, order);
    const auto alpha_at = [&](std::size_t j, int x, int y) {
        const auto k2 = rois[j].at(x, y);
        return k2 ? rois[j].alpha[*k2] : 0.0f;
    };
    std::vector<std::vector<float>> weights(m);
    for (std::size_t j = 0; j < m; ++j) {
        const auto& im = rois[j];
        weights[j].assign(im.w * im.h, 0.0f);
        for (std::size_t p = 0; p < im.w * im.h; ++p) {
            if (im.alpha[p] < 0.5f) continue;
            const int x = im.x0 + static_cast<int>(p % im.w), y = im.y0 + static_cast<int>(p / im.w);
            const int lab = labels[std::min(static_cast<std::size_t>(y) / s, gh - 1) * gw + std::min(static_cast<std::size_t>(x) / s, gw - 1)];
            if (lab == static_cast<int>(j)) {
                weights[j][p] = 1.0f;
            } else if (lab >= 0 && alpha_at(static_cast<std::size_t>(lab), x, y) >= 0.5f) {
                weights[j][p] = 0.0f;
            } else {
                std::optional<std::size_t> first;
                for (std::size_t q = 0; q < m; ++q) {
                    if (alpha_at(q, x, y) >= 0.5f && (!first || rank[q] < rank[*first])) first = q;
                }
                weights[j][p] = first == j ? 1.0f : 0.0f;
            }
        }
    }
    progress(0.75);
    const std::size_t levels = std::min<std::size_t>(panorama::blend_levels(W, H), 7);
    const auto [blend, cov] = panorama::multiband(W, H, rois, weights, levels);
    progress(0.9);
    for (std::size_t j = 0; j < m; ++j) {
        const auto& im = rois[j];
        resample::Plane layer(static_cast<int>(im.w), static_cast<int>(im.h), 4, true);
        resample::Plane mask(static_cast<int>(im.w), static_cast<int>(im.h), 1, false);
        layer.hdr = hdr;
        for (std::size_t p = 0; p < im.w * im.h; ++p) {
            const std::size_t g = (static_cast<std::size_t>(im.y0) + p / im.w) * W + static_cast<std::size_t>(im.x0) + p % im.w;
            const bool inside = weights[j][p] > 0.5f;
            for (std::size_t cc = 0; cc < 3; ++cc) {
                const float v = inside ? blend[g * 3 + cc] : im.px[p * 3 + cc];
                layer.v[p * 4 + cc] = hdr ? std::max(v, 0.0f) : std::clamp(v, 0.0f, 1.0f);
            }
            layer.v[p * 4 + 3] = im.alpha[p];
            mask.v[p] = weights[j][p];
        }
        out.layers.push_back(Part{im.x0, im.y0, std::move(layer)});
        out.masks.push_back(Part{im.x0, im.y0, std::move(mask)});
    }
    Json gains = Json::array();
    for (const auto& g : photo.gains) {
        Json row = Json::array();
        for (const float v : g) row.push_back(r6(v));
        gains.push_back(row);
    }
    out.info["gains"] = gains;
    out.info["vignette"] = Json::array({r6(photo.vignette[0]), r6(photo.vignette[1])});
    if (options.content_aware_fill) {
        // (the transparent areas inside the panorama's box filled, at a reduced scale for a large one: upstream's)
        const std::size_t kf = std::max<std::size_t>(static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(W * H) / 1.5e6))), 1);
        const std::size_t sw = ceil_div(W, kf), sh = ceil_div(H, kf);
        std::vector<float> small(sw * sh * 3, 0.0f), acc(sw * sh, 0.0f), cnt(sw * sh, 0.0f);
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t x = 0; x < W; ++x) {
                const std::size_t i = (y / kf) * sw + x / kf, g = y * W + x;
                const float a = cov[g];
                for (std::size_t cc = 0; cc < 3; ++cc) small[i * 3 + cc] += blend[g * 3 + cc] * a;
                acc[i] += a;
                cnt[i] += 1.0f;
            }
        }
        std::vector<bool> hole(sw * sh), source(sw * sh);
        for (std::size_t i = 0; i < sw * sh; ++i) {
            for (std::size_t cc = 0; cc < 3; ++cc) small[i * 3 + cc] = acc[i] > 0.0f ? small[i * 3 + cc] / acc[i] : 0.0f;
            hole[i] = acc[i] / std::max(cnt[i], 1.0f) < 0.99f;
            source[i] = !hole[i];
        }
        content_aware::FillOptions fo;
        fo.unit_range = !hdr;
        const std::vector<float> filled = content_aware::fill(sw, sh, 3, small, hole, source, fo);
        resample::Plane f(cw, chh, 4, true);
        f.hdr = hdr;
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t x = 0; x < W; ++x) {
                const std::size_t g = y * W + x;
                const float a = cov[g];
                const double fx = std::clamp((static_cast<double>(x) + 0.5) / static_cast<double>(kf) - 0.5, 0.0, static_cast<double>(sw - 1));
                const double fy = std::clamp((static_cast<double>(y) + 0.5) / static_cast<double>(kf) - 0.5, 0.0, static_cast<double>(sh - 1));
                for (std::size_t cc = 0; cc < 3; ++cc) {
                    const std::size_t xi = static_cast<std::size_t>(fx), yi = static_cast<std::size_t>(fy);
                    const std::size_t x2 = std::min(xi + 1, sw - 1), y2 = std::min(yi + 1, sh - 1);
                    const float tx = static_cast<float>(fx - static_cast<double>(xi)), ty = static_cast<float>(fy - static_cast<double>(yi));
                    const auto at = [&](std::size_t xx, std::size_t yy) { return filled[(yy * sw + xx) * 3 + cc]; };
                    const float fv = (at(xi, yi) * (1 - tx) + at(x2, yi) * tx) * (1 - ty) + (at(xi, y2) * (1 - tx) + at(x2, y2) * tx) * ty;
                    const float v = blend[g * 3 + cc] * a + fv * (1.0f - a);
                    f.v[g * 4 + cc] = hdr ? std::max(v, 0.0f) : std::clamp(v, 0.0f, 1.0f);
                }
                f.v[g * 4 + 3] = 1.0f;
            }
        }
        out.fill = std::move(f);
    }
    progress(1.0);
    return out;
}

void register_align_ops(core::OpRegistry& registry) {
    registry.add("auto_align_layers", auto_align_layers);
    registry.add("auto_blend_layers", auto_blend_layers);
}

}  // namespace genko::render::align
