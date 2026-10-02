// Python's render._draw_prims, _draw_surfaces and _draw_mannequin over the box `area` of the page: shapes are drawn in
// page coordinates (PageCanvas), a guide's shaded surfaces are made for the part of them inside the area only (each
// of their pixels depends on nothing else), so a part of the page has the same pixels as the whole page cut.

#include "render/prims.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "core/error.hpp"
#include "core/mannequin.hpp"
#include "core/mesh3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyvalue.hpp"
#include "render/draw.hpp"

namespace genko::render::detail {

namespace {

using core::Json;
namespace mesh3d = core::mesh3d;
namespace prim3d = core::prim3d;
namespace pyv = core::pyv;

PointD at(const mesh3d::Point2& p, int dpi) { return xy_point(p[0], p[1], dpi); }

Box intersect(const Box& a, const Box& b) {
    return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

// render._draw_surfaces: the guide's surfaces, lightly shaded by the page's light
void draw_surfaces(Image& target, const Box& area, const Ctx& ctx, const Json& prim, const Json* camera) {
    const int dpi = ctx.dpi;
    const auto [x, y, w, h] = prim3d::prim_bbox(prim, camera);
    if (w <= 0 || h <= 0) return;
    static const Json kNone = Json::object();
    const Json* light_value = pyv::find(ctx.page->extra, "light");
    const Json& light = light_value != nullptr && core::py_truthy(*light_value) ? *light_value : kNone;
    const Json* dir = pyv::get(light, "dir");
    const Json* ambient_value = pyv::get(light, "ambient");
    const double ambient = ambient_value != nullptr ? pyv::to_float(*ambient_value) : 0.35;
    const int sw = std::max(1, mm_to_px(w, dpi) + 2);
    const int sh = std::max(1, mm_to_px(h, dpi) + 2);
    const int ox = mm_to_px(x, dpi);
    const int oy = mm_to_px(y, dpi);
    // (the patch's pixels outside the page are cut away by the paste: only the ones in this part of it are made)
    const Box shown = intersect(Box{ox, oy, ox + sw, oy + sh}, area);
    if (shown.x1 <= shown.x0 || shown.y1 <= shown.y0) return;
    Json own = Json::object();
    for (const auto& [k, v] : prim.items()) {
        if (k != "camera") own[k] = v;
    }
    const mesh3d::Window window{shown.x0 - ox, shown.y0 - oy, shown.x1 - ox, shown.y1 - oy};
    const mesh3d::Raster r = mesh3d::raster(std::span<const Json>(&own, 1), sw, sh, dpi, camera, dir, ambient, x, y, &window);
    if (!r.any()) return;
    // grey = (200 + 55 * np.clip(shade, 0, 1)).astype("uint8"); (grey - 20, grey - 20, grey, alpha * 150)
    std::string bytes(static_cast<std::size_t>(r.width) * static_cast<std::size_t>(r.height) * 4, '\0');
    for (std::size_t i = 0; i < r.zbuf.size(); ++i) {
        const float s = r.shade[i];
        const float clipped = std::isnan(s) ? s : std::min(std::max(s, 0.0f), 1.0f);
        const float g = 200.0f + 55.0f * clipped;
        const auto grey = static_cast<unsigned char>(std::isfinite(g) && g >= 0.0f && g < 256.0f ? static_cast<int>(g) : 0);
        bytes[i * 4] = static_cast<char>(static_cast<unsigned char>(grey - 20));
        bytes[i * 4 + 1] = static_cast<char>(static_cast<unsigned char>(grey - 20));
        bytes[i * 4 + 2] = static_cast<char>(grey);
        bytes[i * 4 + 3] = static_cast<char>(r.alpha(i) ? 150 : 0);
    }
    const Image piece = Image::frombytes("RGBA", Size{r.width, r.height}, bytes);
    const Box local{shown.x0 - area.x0, shown.y0 - area.y0, shown.x1 - area.x0, shown.y1 - area.y0};
    Image region = target.crop(local);
    if (region.mode() != "RGBA") region = region.convert("RGBA");
    region.alpha_composite(piece);
    target.paste(region.mode() == target.mode() ? region : region.convert(target.mode()), Point{local.x0, local.y0});
}

// render._draw_mannequin: body, elbows, knees and neck, turned and leaned by rot
void draw_mannequin(Draw& draw, const Json& prim, int dpi) {
    const core::mannequin::Bone bone = core::mannequin::skeleton(prim);
    const int width = std::max(2, mm_to_px(0.5, dpi));
    const Ink color{90, 90, 140};
    const auto shade = [&](const std::string& part) {
        if (part == "left") return Ink{60, 120, 170};
        if (part == "right") return Ink{150, 90, 120};
        return color;
    };
    for (const auto& s : bone.segments) {
        const std::vector<PointD> seg{at(s.a, dpi), at(s.b, dpi)};
        draw.line(seg, shade(s.part), width);
    }
    for (const auto& s : bone.segments) {
        if (s.part == "body") continue;
        const PointD p = at(s.a, dpi);
        const int r = std::max(1, width);
        draw.ellipse(BoxF{p.x - r, p.y - r, p.x + r, p.y + r}, shade(s.part));
    }
    const double hx = bone.head_c[0];
    const double hy = bone.head_c[1];
    const double hr = bone.head_r;
    const PointD a = xy_point(hx - hr, hy - hr, dpi);
    const PointD b = xy_point(hx + hr, hy + hr, dpi);
    draw.ellipse(BoxF{a.x, a.y, b.x, b.y}, std::nullopt, color, width);
    const std::vector<PointD> nose{xy_point(hx, hy, dpi), xy_point(hx + hr * 0.8 * bone.facing, hy, dpi)};
    draw.line(nose, color, std::max(1, width / 2));
}

}  // namespace

void draw_prims(Image& part, const Box& area, const Ctx& ctx) {
    const core::Page& page = *ctx.page;
    if (ctx.mode == "print" || !core::py_truthy(page.prims)) return;
    const int dpi = ctx.dpi;
    const int width = std::max(1, mm_to_px(0.3, dpi));
    std::map<std::string, const core::Frame*> frames;  // (a dict: the last panel of an id wins)
    for (const core::Frame* frame : page.leaf_frames()) frames[frame->id] = frame;
    const Json* page_camera = pyv::find(page.extra, "camera");
    for (const Json& prim : core::py_list(page.prims)) {
        check_cancel(ctx);
        const Json* camera = mesh3d::camera_of(prim, page_camera);
        // a guide set in a panel stays in it (a room seen from inside runs far past the panel's edges)
        const Json* frame_id = pyv::get(prim, "frame_id");
        const core::Frame* frame = nullptr;
        if (frame_id != nullptr && core::py_truthy(*frame_id)) {
            if (frame_id->is_array() || frame_id->is_object()) {
                throw core::Error("type", "unhashable type: '" + core::py_type_name(*frame_id) + "'");
            }
            if (frame_id->is_string()) {
                const auto it = frames.find(frame_id->get<std::string>());
                if (it != frames.end()) frame = it->second;
            }
        }
        Image sheet;
        if (frame != nullptr) sheet = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
        Image& target = frame != nullptr ? sheet : part;
        const Json* kind = pyv::get(prim, "kind");
        if (kind != nullptr && pyv::eq(*kind, Json("mannequin"))) {
            PageCanvas canvas(target, area, ctx.size);
            draw_mannequin(canvas.draw(), prim, dpi);
            canvas.commit();
        } else if (kind != nullptr && mesh3d::is_mesh_kind(*kind)) {
            draw_surfaces(target, area, ctx, prim, camera);
            const std::vector<prim3d::Line2> lines = prim3d::trace(prim, camera);
            PageCanvas canvas(target, area, ctx.size);
            for (const auto& line : lines) {
                std::vector<PointD> pts;
                pts.reserve(line.size());
                for (const auto& p : line) pts.push_back(at(p, dpi));
                canvas.draw().line(pts, Ink{70, 70, 120}, width);
            }
            canvas.commit();
        } else {
            const std::vector<prim3d::Edge> edges = prim3d::edges(prim, camera);
            PageCanvas canvas(target, area, ctx.size);
            for (const auto& e : edges) {
                const std::vector<PointD> seg{at(e.a, dpi), at(e.b, dpi)};
                canvas.draw().line(seg, e.seen ? Ink{90, 90, 140} : Ink{190, 190, 215}, width);
            }
            canvas.commit();
        }
        if (frame != nullptr) {
            Image inside = Image::create("L", Size{area.width(), area.height()}, Ink(0));
            {
                PageCanvas canvas(inside, area, ctx.size);
                fill_frame(canvas.draw(), *frame, dpi);
                canvas.commit();
            }
            const Image mask = chops::multiply(sheet.getchannel(3), inside);
            part.paste(sheet, Point{0, 0}, &mask);
        }
    }
}

}  // namespace genko::render::detail
