#include "render/tiled_pixels.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "core/error.hpp"
#include "core/model.hpp"
#include "render/ink_edit.hpp"

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

TiledPixels::TiledPixels(core::OpContext& c, std::size_t at, std::string id, Size grid, const Pixels& like)
    : context_(&c), at_(at), id_(std::move(id)), grid_(grid), columns_((grid.width + kTile - 1) / kTile),
      like_(std::make_unique<Pixels>(clear_like(like, Box{0, 0, 1, 1}))) {}

TiledPixels::TiledPixels(Size grid, const Pixels& like)
    : grid_(grid), columns_((grid.width + kTile - 1) / kTile), like_(std::make_unique<Pixels>(clear_like(like, Box{0, 0, 1, 1}))) {}

TiledPixels::TiledPixels(const TiledPixels& other)
    : context_(other.context_), at_(other.at_), id_(other.id_), grid_(other.grid_), columns_(other.columns_),
      like_(std::make_unique<Pixels>(*other.like_)), tiles_(other.tiles_), inks_(other.inks_), inks_held_(other.inks_held_) {}

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
    if (context_ != nullptr && like_->kind() == Pixels::Kind::Inks) {
        // (an ink layer: its inks with its marks drawn, once — ink_edit::with_marks, as layer_pixels reads them —, each
        // tile from them)
        if (!inks_) {
            const core::Page& page = context_->doc.page(at_);
            const core::Layer* layer = nullptr;
            for (const core::Layer& l : page.layers) {
                if (l.id == id_) layer = &l;
            }
            if (layer == nullptr) throw core::OpError("no layer " + id_);
            if (layer->locked) throw core::OpError("the layer is locked");
            if (!page.color || layer->raster_png) throw core::Error("value", "the layer " + id_ + " holds no inks");
            const std::uint64_t sample = like_->ink && like_->ink->depth == 32 ? sizeof(float) : sizeof(std::uint16_t);
            const auto bytes = std::uint64_t(grid_.width) * std::uint64_t(grid_.height) * (like_->colours() + 1) * sample;
            inks_held_ = std::make_shared<image_ops::Held>(bytes);
            inks_ = std::make_shared<core::InkPixels>(ink_edit::with_marks(page, *layer, &context_->doc));
            if (static_cast<int>(inks_->width) != grid_.width || static_cast<int>(inks_->height) != grid_.height) {
                throw core::Error("value", "the layer " + id_ + "'s grid is not the one it was worked at");
            }
        }
        std::vector<Pixels> read;  // (all made before any is held: past the budget, none)
        read.reserve(missing.size());
        for (const Box& box : missing) read.push_back(image_ops::inks_part(*inks_, *like_->ink, box));
        for (std::size_t i = 0; i < missing.size(); ++i) {
            tiles_.emplace(key(missing[i].x0 / kTile, missing[i].y0 / kTile), Tile{missing[i], std::move(read[i]), Box{}, {}});
        }
    } else if (context_ != nullptr) {
        Size read_grid;
        std::vector<Pixels> read = image_ops::layer_pixels(*context_, at_, id_, missing, nullptr, &read_grid);
        if (read_grid.width != grid_.width || read_grid.height != grid_.height) {
            throw core::Error("value", "the layer " + id_ + "'s grid is not the one it was worked at");
        }
        for (std::size_t i = 0; i < missing.size(); ++i) {
            tiles_.emplace(key(missing[i].x0 / kTile, missing[i].y0 / kTile), Tile{missing[i], std::move(read[i]), Box{}, {}});
        }
    } else {
        std::vector<Pixels> made;  // (all made before any is held: past the budget, none)
        made.reserve(missing.size());
        for (const Box& box : missing) made.push_back(clear_like(*like_, box));
        for (std::size_t i = 0; i < missing.size(); ++i) {
            tiles_.emplace(key(missing[i].x0 / kTile, missing[i].y0 / kTile), Tile{missing[i], std::move(made[i]), Box{}, {}});
        }
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
    t->edited = t->edited.width() <= 0 ? Box{x, y, x + 1, y + 1}
                                       : Box{std::min(t->edited.x0, x), std::min(t->edited.y0, y), std::max(t->edited.x1, x + 1),
                                             std::max(t->edited.y1, y + 1)};
    const std::size_t i = std::size_t(y - t->box.y0) * std::size_t(t->box.width()) + std::size_t(x - t->box.x0);
    if (t->marks.empty()) t->marks.assign(std::size_t(t->box.width()) * std::size_t(t->box.height()), 0);
    t->marks[i] = 1;
    return t->pixels.at(i);
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
        if (t.edited.width() > 0) all.emplace_back(k, t.box);
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Box> out;
    for (const auto& [k, box] : all) out.push_back(box);
    return out;
}

Box TiledPixels::put(core::Document& doc) const {
    if (context_ == nullptr) throw core::Error("value", "these tiles are not a layer's");
    // (each changed tile's box of the pixels edited, copied out with their marks: a pixel never edited is never written —
    // the ink encoder puts a clear pixel's inks to nothing)
    std::vector<Pixels> edited;
    std::vector<std::vector<std::uint8_t>> marks;
    std::vector<Box> boxes;
    Box covered;
    for (const Box& box : changed()) {
        const Tile& t = tiles_.at(key(box.x0 / kTile, box.y0 / kTile));
        const Box& e = t.edited;
        Pixels part = clear_like(*like_, e);
        std::vector<std::uint8_t> only(part.size(), 0);
        const unsigned n = part.all();
        for (int y = e.y0; y < e.y1; ++y) {
            for (int x = e.x0; x < e.x1; ++x) {
                const std::size_t from_at = std::size_t(y - t.box.y0) * std::size_t(t.box.width()) + std::size_t(x - t.box.x0);
                const std::size_t to_at = std::size_t(y - e.y0) * std::size_t(e.width()) + std::size_t(x - e.x0);
                const float* from = t.pixels.at(from_at);
                std::copy(from, from + n, part.at(to_at));
                only[to_at] = t.marks[from_at];
            }
        }
        edited.push_back(std::move(part));
        marks.push_back(std::move(only));
        boxes.push_back(e);
        covered = covered.width() <= 0 ? e : Box{std::min(covered.x0, e.x0), std::min(covered.y0, e.y0), std::max(covered.x1, e.x1), std::max(covered.y1, e.y1)};
    }
    if (edited.empty()) return covered;
    if (inks_) {
        // (an ink layer's inks held from the read: the pixels edited written into them, encoded once — as
        // put_layer_pixels writes an ink layer's: its marks in its inks after)
        if (inks_.use_count() > 1) inks_ = std::make_shared<core::InkPixels>(*inks_);  // (a copy still shares them)
        for (std::size_t i = 0; i < edited.size(); ++i) image_ops::put_inks_part(*inks_, edited[i], boxes[i], &marks[i]);
        core::Page& page = doc.edit_page(at_);
        for (core::Layer& layer : page.layers) {
            if (layer.id != id_) continue;
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*inks_));
            layer.ink_memo = {};
            layer.strokes = core::empty_strokes();
            layer.shapes = core::vector::empty_shapes();
            layer.patches.clear();
            layer.kind = core::LayerKind::Raster;
        }
        return covered;
    }
    std::vector<image_ops::PixelsPart> parts;
    for (std::size_t i = 0; i < edited.size(); ++i) parts.push_back(image_ops::PixelsPart{boxes[i], &edited[i], &marks[i]});
    image_ops::put_layer_pixels(doc, at_, id_, parts);
    return covered;
}

}  // namespace genko::render::tiled
