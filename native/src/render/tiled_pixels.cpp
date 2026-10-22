#include "render/tiled_pixels.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "core/error.hpp"

namespace genko::render::tiled {

using image_ops::Pixels;

namespace {

Pixels clear_like(const Pixels& like, const Box& box) {
    Pixels out(like.kind(), static_cast<std::uint32_t>(box.width()), static_cast<std::uint32_t>(box.height()), like.colours());
    out.floating = like.floating;
    out.ink = like.ink;
    return out;
}

}  // namespace

std::vector<Box> tiles_reached(Size grid, const std::vector<Box>& boxes, int margin) {
    std::set<std::pair<int, int>> keys;  // (row, column)
    for (const Box& b : boxes) {
        const Box r{std::max(b.x0 - margin, 0), std::max(b.y0 - margin, 0), std::min(b.x1 + margin, grid.width), std::min(b.y1 + margin, grid.height)};
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        for (int ty = r.y0 / kTile; ty <= (r.y1 - 1) / kTile; ++ty) {
            for (int tx = r.x0 / kTile; tx <= (r.x1 - 1) / kTile; ++tx) keys.emplace(ty, tx);
        }
    }
    std::vector<Box> out;
    out.reserve(keys.size());
    for (const auto& [ty, tx] : keys) {
        out.push_back(Box{tx * kTile, ty * kTile, std::min((tx + 1) * kTile, grid.width), std::min((ty + 1) * kTile, grid.height)});
    }
    return out;
}

TiledPixels::TiledPixels(core::OpContext& c, std::size_t at, std::string id) : context_(&c), at_(at), id_(std::move(id)) {
    image_ops::Window window;
    like_ = std::make_unique<Pixels>(image_ops::layer_pixels(c, at_, id_, Box{0, 0, 1, 1}, &window));
    grid_ = window.grid;
    columns_ = (grid_.width + kTile - 1) / kTile;
}

TiledPixels::TiledPixels(Size grid, const Pixels& like)
    : grid_(grid), columns_((grid.width + kTile - 1) / kTile), like_(std::make_unique<Pixels>(clear_like(like, Box{0, 0, 1, 1}))) {}

TiledPixels::TiledPixels(const TiledPixels& other)
    : context_(other.context_), at_(other.at_), id_(other.id_), grid_(other.grid_), columns_(other.columns_),
      like_(std::make_unique<Pixels>(*other.like_)), tiles_(other.tiles_) {}

TiledPixels::TiledPixels(TiledPixels&&) noexcept = default;
TiledPixels::~TiledPixels() = default;

Pixels::Kind TiledPixels::kind() const { return like_->kind(); }
unsigned TiledPixels::colours() const { return like_->colours(); }

std::size_t TiledPixels::reach(const std::vector<Box>& boxes) {
    std::vector<Box> missing;
    for (const Box& box : tiles_reached(grid_, boxes)) {
        if (!tiles_.contains(key(box.x0 / kTile, box.y0 / kTile))) missing.push_back(box);
    }
    if (missing.empty()) return 0;
    if (context_ != nullptr) {
        std::vector<Pixels> read = image_ops::layer_pixels(*context_, at_, id_, missing);
        for (std::size_t i = 0; i < missing.size(); ++i) {
            tiles_.emplace(key(missing[i].x0 / kTile, missing[i].y0 / kTile), Tile{missing[i], std::move(read[i]), false});
        }
    } else {
        for (const Box& box : missing) tiles_.emplace(key(box.x0 / kTile, box.y0 / kTile), Tile{box, clear_like(*like_, box), false});
    }
    return missing.size();
}

const TiledPixels::Tile* TiledPixels::tile_at(int x, int y) const {
    if (x < 0 || y < 0 || x >= grid_.width || y >= grid_.height) return nullptr;
    const auto it = tiles_.find(key(x / kTile, y / kTile));
    return it == tiles_.end() ? nullptr : &it->second;
}

bool TiledPixels::holds(int x, int y) const { return tile_at(x, y) != nullptr; }

const float* TiledPixels::at(int x, int y) const {
    const Tile* t = tile_at(x, y);
    if (t == nullptr) throw core::Error("value", "the pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is not in a tile held");
    return t->pixels.at(std::size_t(y - t->box.y0) * std::size_t(t->box.width()) + std::size_t(x - t->box.x0));
}

float* TiledPixels::edit(int x, int y) {
    Tile* t = const_cast<Tile*>(tile_at(x, y));
    if (t == nullptr) throw core::Error("value", "the pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is not in a tile held");
    t->changed = true;
    return t->pixels.at(std::size_t(y - t->box.y0) * std::size_t(t->box.width()) + std::size_t(x - t->box.x0));
}

std::vector<Box> TiledPixels::held() const {
    std::vector<std::pair<std::int64_t, Box>> all;
    for (const auto& [k, t] : tiles_) all.emplace_back(k, t.box);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Box> out;
    for (const auto& [k, box] : all) out.push_back(box);
    return out;
}

std::vector<Box> TiledPixels::changed() const {
    std::vector<std::pair<std::int64_t, Box>> all;
    for (const auto& [k, t] : tiles_) {
        if (t.changed) all.emplace_back(k, t.box);
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Box> out;
    for (const auto& [k, box] : all) out.push_back(box);
    return out;
}

Box TiledPixels::put(core::Document& doc) const {
    if (context_ == nullptr) throw core::Error("value", "these tiles are not a layer's");
    std::vector<std::pair<Box, const Pixels*>> parts;
    Box covered;
    for (const Box& box : changed()) {
        const Tile& t = tiles_.at(key(box.x0 / kTile, box.y0 / kTile));
        parts.emplace_back(t.box, &t.pixels);
        covered = covered.width() <= 0 ? t.box
                                       : Box{std::min(covered.x0, t.box.x0), std::min(covered.y0, t.box.y0), std::max(covered.x1, t.box.x1),
                                             std::max(covered.y1, t.box.y1)};
    }
    image_ops::put_layer_pixels(doc, at_, id_, parts);
    return covered;
}

}  // namespace genko::render::tiled
