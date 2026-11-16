#include "render/figure_thumbs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

#include "core/mesh3d.hpp"
#include "core/pose_checks.hpp"
#include "core/pose_library.hpp"
#include "core/pyconv.hpp"
#include "render/image.hpp"
#include "render/png.hpp"

namespace genko::render::figure_thumbs {

namespace mesh3d = core::mesh3d;

namespace {

constexpr double kTall = 13.0;     // (u: the frame's height at the usual scale)
constexpr double kFloorAt = 0.9;   // (u: the floor above the picture's foot)
constexpr double kFill = 0.92;     // (the most of the picture a body drawn smaller fills)

Json seen_at(const Json& prim, double turn) {
    Json seen = prim;
    seen["pos"] = Json::array({0.0, 0.0, 0.0});
    seen["rot"] = Json::array({0.0, turn, 0.0});
    return seen;
}

}  // namespace

Json figure_for(const Json& pose, const Json& body, double height_mm) {
    Json prim = core::pose_checks::figure_of(pose, height_mm, body);
    if (pose.contains("hold")) prim["hold"] = pose["hold"];  // (what the hands hold, drawn: SPEC FIGURE-02)
    return prim;
}

Frame frame_of(const Json& prim, double turn, int width, int height) {
    const Json seen = seen_at(prim, turn);
    Frame f;
    f.unit = mesh3d::figure_skeleton(seen).unit;
    const auto model = mesh3d::model_of(seen);
    double x0 = std::numeric_limits<double>::infinity();
    double x1 = -x0;
    double y0 = x0;
    double y1 = -x0;
    if (model && !model->mesh.v.empty()) {
        const mesh3d::OnPage at = mesh3d::to_page(seen, model->mesh.v, nullptr);
        for (const auto& p : at.pts) {
            x0 = std::min(x0, p[0]);
            x1 = std::max(x1, p[0]);
            y0 = std::min(y0, p[1]);
            y1 = std::max(y1, p[1]);
        }
    }
    if (!std::isfinite(x0)) x0 = x1 = y0 = y1 = 0.0;
    const double u = f.unit > 0 ? f.unit : 1.0;
    double k = height / (kTall * u);  // (px per mm, the usual scale)
    const double margin = kFloorAt / kTall;  // (of the height, under the floor)
    const double room_w = width * kFill;
    const double room_h = height * (1.0 - margin) * kFill;
    if ((x1 - x0) * k > room_w) k = room_w / (x1 - x0);
    if ((y1 - y0) * k > room_h) k = room_h / (y1 - y0);
    f.px_per_mm = k;
    f.floor = y1;
    f.left = (x0 + x1) / 2 - width / 2.0 / k;
    f.top = y1 + margin * height / k - height / k;
    return f;
}

std::string grey(const Json& prim, double turn, int width, int height, int supersample, Frame* frame) {
    const int ss = std::max(1, supersample);
    const Frame f = frame_of(prim, turn, width, height);
    if (frame != nullptr) *frame = f;
    const Json seen = seen_at(prim, turn);
    const std::vector<Json> prims{seen};
    const int w = width * ss;
    const int h = height * ss;
    const mesh3d::Raster r = mesh3d::raster(prims, w, h, f.px_per_mm * ss * 25.4, nullptr, nullptr, 0.35, f.left, f.top);
    std::string out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), static_cast<char>(255));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sum = 0;
            for (int dy = 0; dy < ss; ++dy) {
                for (int dx = 0; dx < ss; ++dx) {
                    const std::size_t i = static_cast<std::size_t>(y * ss + dy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x * ss + dx);
                    int g = 255;
                    if (r.alpha(i)) {
                        const float s = std::max(0.0f, std::min(1.0f, r.shade[i]));
                        g = static_cast<int>(s * 254.0f + 0.5f);
                    }
                    sum += g;
                }
            }
            const int n = ss * ss;
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = static_cast<char>((sum + n / 2) / n);
        }
    }
    return out;
}

std::string body_digest(const Json& body) { return body.is_object() ? core::dump_python(body) : std::string("{}"); }

Cache::Cache(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

std::string Cache::get(std::string_view id, double turn, const Json& body, int width, int height, int supersample) {
    char size[96];
    std::snprintf(size, sizeof size, "|%.4f|%dx%d@%d|", turn, width, height, supersample);
    const std::string key = std::string(id) + size + body_digest(body);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& e : entries_) {
            if (e.key == key) return e.grey;
        }
    }
    const std::optional<Json> pose = core::pose_library::resolve(id);
    if (!pose) return {};
    std::string made = grey(figure_for(*pose, body), turn, width, height, supersample);
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const Entry& e : entries_) {
        if (e.key == key) return e.grey;  // (another thread drew it meanwhile: the same bytes)
    }
    if (entries_.size() >= capacity_) entries_.erase(entries_.begin());
    entries_.push_back({key, made});
    return made;
}

std::size_t Cache::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void Cache::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::string sheet_png(const std::vector<Json>& poses, const std::vector<double>& turns, const Json& body, int cell_w, int cell_h, int columns) {
    const int views = std::max<int>(1, static_cast<int>(turns.size()));
    const int per_row = columns > 0 ? columns : std::max(1, 6 / views);
    const int gap = 6;
    const int n = static_cast<int>(poses.size());
    const int rows = std::max(1, (n + per_row - 1) / per_row);
    const int width = per_row * (views * cell_w + gap) + gap;
    const int height = rows * (cell_h + gap) + gap;
    std::string rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, static_cast<char>(246));
    const auto put = [&](int x, int y, int r, int g, int b) {
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3;
        rgb[i] = static_cast<char>(r);
        rgb[i + 1] = static_cast<char>(g);
        rgb[i + 2] = static_cast<char>(b);
    };
    for (int k = 0; k < n; ++k) {
        const int ox0 = gap + (k % per_row) * (views * cell_w + gap);
        const int oy = gap + (k / per_row) * (cell_h + gap);
        const Json prim = figure_for(poses[static_cast<std::size_t>(k)], body);
        for (int v = 0; v < views; ++v) {
            const double turn = turns.empty() ? kThreeQuarter : turns[static_cast<std::size_t>(v)];
            Frame f;
            const std::string g = grey(prim, turn, cell_w, cell_h, 2, &f);
            const int ox = ox0 + v * cell_w;
            const int fy = static_cast<int>(std::lround((f.floor - f.top) * f.px_per_mm));
            for (int y = 0; y < cell_h; ++y) {
                for (int x = 0; x < cell_w; ++x) {
                    const int c = static_cast<unsigned char>(g[static_cast<std::size_t>(y) * static_cast<std::size_t>(cell_w) + static_cast<std::size_t>(x)]);
                    const bool edge = x == 0 || y == 0 || x == cell_w - 1 || y == cell_h - 1;
                    if (edge) {
                        put(ox + x, oy + y, 205, 205, 215);
                    } else if (c == 255) {
                        put(ox + x, oy + y, y == fy ? 170 : 255, y == fy ? 140 : 255, y == fy ? 110 : 255);  // (the floor's line)
                    } else {
                        put(ox + x, oy + y, c, c, std::min(255, c + 18));
                    }
                }
            }
        }
    }
    const Image image = Image::frombytes("RGB", Size{width, height}, rgb);
    return write_png(image);
}

}  // namespace genko::render::figure_thumbs
