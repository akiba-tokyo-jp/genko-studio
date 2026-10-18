#include "render/path_info.hpp"

#include <cmath>
#include <cstdio>

#include "core/error.hpp"
#include "core/vector.hpp"

// PhotoCraft v0.2.0 crates/engine/src/print_cmds.rs (461-521: paths_to_ai) and vector_cmds.rs (635-651: paths_list),
// MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::path_info {

using core::Json;
namespace vec = core::vector;

namespace {

Json counted(const vec::Path& path) {
    return Json{{"path", vec::to_json(path)},
                {"subpaths", static_cast<std::int64_t>(path.subpaths.size())},
                {"knots", static_cast<std::int64_t>(path.knot_count())}};
}

std::string fixed4(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", std::fabs(v) < 5e-5 ? 0.0 : v);
    return buf;
}

}  // namespace

Json page_paths(const core::Page& page) {
    Json paths = Json::array();
    for (const vec::NamedPath& named : page.paths) {
        Json one = counted(named.path);
        one["name"] = named.name;
        paths.push_back(std::move(one));
    }
    Json layers = Json::array();
    for (const core::Layer& layer : page.layers) {
        if (!layer.vector_mask && layer.shapes->items.empty()) continue;
        Json item{{"id", layer.id}, {"title", layer.title}};
        if (layer.vector_mask) {
            Json mask = vec::to_json(*layer.vector_mask);
            mask["subpaths"] = static_cast<std::int64_t>(layer.vector_mask->path.subpaths.size());
            mask["knots"] = static_cast<std::int64_t>(layer.vector_mask->path.knot_count());
            item["vector_mask"] = std::move(mask);
        }
        if (!layer.shapes->items.empty()) {
            Json shapes = Json::array();
            for (const vec::ShapePart& part : layer.shapes->items) {
                Json one = counted(part.path);
                one["id"] = part.id;
                if (part.name) one["name"] = *part.name;
                if (const auto box = part.path.control_bounds()) one["bounds"] = Json::array({(*box)[0], (*box)[1], (*box)[2], (*box)[3]});
                shapes.push_back(std::move(one));
            }
            item["shapes"] = std::move(shapes);
        }
        layers.push_back(std::move(item));
    }
    Json clipping = nullptr;
    if (page.clipping_path) clipping = Json{{"name", page.clipping_path->name}, {"flatness", page.clipping_path->flatness}};
    return Json{{"page", page.index.json()},
                {"paths", std::move(paths)},
                {"work_path", page.work_path ? counted(*page.work_path) : Json(nullptr)},
                {"clipping_path", std::move(clipping)},
                {"layers", std::move(layers)}};
}

Json book_paths(const core::Document& doc, std::optional<std::int64_t> page) {
    if (page) {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.pages[i]->index == core::Num(*page) && !doc.is_deferred(i)) return page_paths(doc.page(i));
        }
        throw core::Error("key", "no page " + std::to_string(*page));
    }
    Json pages = Json::array();
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const Json one = page_paths(doc.page(i));
        if (one["paths"].empty() && one["work_path"].is_null() && one["clipping_path"].is_null() && one["layers"].empty()) continue;
        pages.push_back(one);
    }
    return Json{{"pages", std::move(pages)}};
}

std::string illustrator(const core::Document& doc, const core::Page& page, std::string_view which, int* count) {
    const double k = 72.0 / 25.4;  // (mm to points)
    const double w = page.spec.width_mm.value() * k, h = page.spec.height_mm.value() * k;
    std::vector<std::pair<std::string, const vec::Path*>> chosen;
    if (which == "all") {
        for (const vec::NamedPath& named : page.paths) chosen.emplace_back(named.name, &named.path);
        if (page.work_path) chosen.emplace_back("作業用パス", &*page.work_path);
    } else if (which == "work") {
        if (page.work_path) chosen.emplace_back("作業用パス", &*page.work_path);
    } else {
        for (const vec::NamedPath& named : page.paths)
            if (named.name == which) chosen.emplace_back(named.name, &named.path);
    }
    std::string title = doc.title.empty() ? std::string("Genko") : doc.title;
    for (char& c : title) {
        if (c == '(' || c == ')') c = '_';
    }
    std::string s = "%!PS-Adobe-2.0 EPSF-1.2\n%%Creator: Genko\n%%Title: (" + title + ")\n";
    s += "%%BoundingBox: 0 0 " + std::to_string(static_cast<std::int64_t>(std::ceil(w))) + " " + std::to_string(static_cast<std::int64_t>(std::ceil(h))) + "\n";
    s += "%%HiResBoundingBox: 0 0 " + fixed4(w) + " " + fixed4(h) + "\n%AI3_Cropmarks: 0 0 " + fixed4(w) + " " + fixed4(h) + "\n";
    s += "%%DocumentProcessColors: Black\n%%EndComments\n%%EndProlog\n%%BeginSetup\n%%EndSetup\n";
    const auto pt = [&](const vec::XY& p) { return fixed4(p.x * k) + " " + fixed4(h - p.y * k); };
    int n = 0;
    for (const auto& [name, path] : chosen) {
        std::vector<const vec::Subpath*> subs;
        for (const vec::Subpath& sub : path->subpaths)
            if (!sub.knots.empty()) subs.push_back(&sub);
        if (subs.empty()) continue;
        ++n;
        s += "%%Note: " + name + "\n";
        const bool compound = subs.size() > 1;
        if (compound) s += "*u\n";
        for (const vec::Subpath* sub : subs) {
            s += pt(sub->knots.front().anchor) + " m\n";
            for (const auto& [a, c1, c2, b] : sub->segments()) {
                if (c1 == a && c2 == b) {
                    s += pt(b) + " L\n";
                } else {
                    s += pt(c1) + " " + pt(c2) + " " + pt(b) + " C\n";
                }
            }
            s += sub->closed ? "n\n" : "N\n";  // (unpainted: n closes it, N leaves it open)
        }
        if (compound) s += "*U\n";
    }
    s += "%%PageTrailer\n%%Trailer\n%%EOF\n";
    if (count != nullptr) *count = n;
    return s;
}

}  // namespace genko::render::path_info
