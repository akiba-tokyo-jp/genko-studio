// masks@1 (PC3 B-0): a layer's mask lent to a pixel op as a layer to paint ("layer_mask": true): render/layer_masks.hpp.

#include "render/layer_masks.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "core/channels.hpp"
#include "core/masks.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

namespace genko::render::layer_masks {

using core::Json;

namespace {

constexpr int kMaskDpi = 150;  // (a mask's resolution: ops.MASK_DPI)

unsigned char byte_of(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// The key a pixel op reads the id of the layer it works on from ("" for flood_fill, which takes a role only): as a channel
// is lent (render/channels).
std::string_view layer_key(std::string_view op) {
    if (op == "filter_raster" || op == "put_raster") return "id";
    if (op == "flood_fill") return {};
    return "layer_id";
}

std::size_t owner_of(const core::Page& page, const Json& op, std::string_view name) {
    const std::string_view key = layer_key(name);
    if (!key.empty() && core::truthy_at(op, std::string(key).c_str())) {
        const std::string id = core::py_str(op[std::string(key)]);
        for (std::size_t i = 0; i < page.layers.size(); ++i)
            if (page.layers[i].id == id) return i;
        throw core::OpError("layer_mask: no layer " + id);
    }
    const Json* layer = core::get(op, "layer");
    const core::LayerRole role = core::role_from(Json(layer != nullptr && core::py_truthy(*layer) ? core::py_str(*layer) : "ink"));
    for (std::size_t i = 0; i < page.layers.size(); ++i)
        if (page.layers[i].role == role) return i;
    throw core::OpError("layer_mask: the page has no layer to take the mask of");
}

// One axis resampled: for each of `to` cells the cells of `from` it reads and their weights (a mean of the cells it
// covers when it is the larger, else bilinear by the centres, the edges carried on): render/channels' taps.
using Taps = std::vector<std::vector<std::pair<int, double>>>;
Taps taps(int from, int to) {
    Taps out(static_cast<std::size_t>(to));
    const double scale = double(from) / double(to);
    for (int i = 0; i < to; ++i) {
        auto& list = out[static_cast<std::size_t>(i)];
        if (scale > 1.0) {
            const double a = i * scale, b = (i + 1) * scale;
            const int last = static_cast<int>(std::min<double>(from, std::ceil(b)));
            for (int j = static_cast<int>(std::floor(a)); j < last; ++j) {
                const double w = std::min(b, j + 1.0) - std::max(a, double(j));
                if (w > 0) list.emplace_back(j, w / scale);
            }
        } else {
            const double x = (i + 0.5) * scale - 0.5, f = std::floor(x), t = x - f;
            const auto at = [&](double v) { return static_cast<int>(std::clamp(v, 0.0, double(from - 1))); };
            const int j0 = at(f), j1 = at(f + 1);
            if (t > 0 && j1 != j0) {
                list.emplace_back(j0, 1.0 - t);
                list.emplace_back(j1, t);
            } else {
                list.emplace_back(t > 0 ? j1 : j0, 1.0);
            }
        }
    }
    return out;
}

// What a pixel of the lent mask gives back: its lightness over black where the op made it clear (Genko's blacks — the
// greys 0, 10, 15, 20 — hide all; white shows all).
double value_of(unsigned r, unsigned g, unsigned b, unsigned a) {
    if (r == g && g == b && (r == 0 || r == 10 || r == 15 || r == 20)) return 0.0;
    return (r * 299.0 + g * 587.0 + b * 114.0) / 1000.0 / 255.0 * (a / 255.0);
}

Size page_size(const core::Page& page, int dpi) {
    return Size{std::max(1, mm_to_px(page.spec.width_mm.value(), dpi)), std::max(1, mm_to_px(page.spec.height_mm.value(), dpi))};
}

}  // namespace

MaskLending::MaskLending(core::Document& doc, std::size_t at, Json& op, std::string_view name) {
    const core::Page& seen = doc.page(at);
    page_id_ = seen.id;
    const std::size_t index = owner_of(seen, op, name);
    const core::Layer& owner = seen.layers[index];
    if (owner.kind == core::LayerKind::Folder) throw core::OpError("layer_mask: a folder cannot take a mask (mask the layers in it)");
    if (owner.locked) throw core::OpError("the layer is locked");
    owner_ = owner.id;
    lent_size_ = page_size(seen, raster::kWorkingDpi);
    limits::check_picture(lent_size_.width, lent_size_.height, "the page");
    Image mask;
    if (owner.mask && owner.mask->png && !owner.mask->png->empty()) {
        mask = open_image(*owner.mask->png, kPillowOpenLimits).convert("L");
    } else {  // (a layer without a mask: one that reveals all, as Photoshop makes it)
        mask = Image::create("L", page_size(seen, kMaskDpi), Ink(255));
        made_ = true;
    }
    own_size_ = mask.size();
    own_ = mask.tobytes();
    // (each pixel lent: the mean of the mask's pixels it covers, or bilinear where the mask is coarser)
    const Taps across = taps(own_size_.width, lent_size_.width), down = taps(own_size_.height, lent_size_.height);
    std::string rgba(static_cast<std::size_t>(lent_size_.width) * static_cast<std::size_t>(lent_size_.height) * 4, '\xff');
    for (int y = 0; y < lent_size_.height; ++y) {
        for (int x = 0; x < lent_size_.width; ++x) {
            double v = 0.0;
            for (const auto& [row, wy] : down[static_cast<std::size_t>(y)])
                for (const auto& [col, wx] : across[static_cast<std::size_t>(x)])
                    v += wy * wx * static_cast<unsigned char>(own_[static_cast<std::size_t>(row) * static_cast<std::size_t>(own_size_.width) + static_cast<std::size_t>(col)]) / 255.0;
            std::fill_n(rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * static_cast<std::size_t>(lent_size_.width) + static_cast<std::size_t>(x)) * 4), 3,
                        static_cast<char>(byte_of(v)));
        }
    }
    core::Layer layer;
    layer_id_ = "lent_mask_" + owner.id;
    layer.id = layer_id_;
    layer.kind = core::LayerKind::Raster;
    layer.role = owner.role;
    layer.title = owner.title;
    layer.visible = false;  // (never drawn with the page)
    layer.exportable = false;
    layer.panel_clip = false;
    layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", lent_size_, rgba), 1));
    lent_ = std::move(rgba);
    core::Page& page = doc.edit_page(at);
    page.layers.insert(page.layers.begin(), std::move(layer));  // (the first of its role, for an op that takes a role)
    if (const std::string_view key = layer_key(name); !key.empty()) op[std::string(key)] = layer_id_;
}

bool MaskLending::take_back(core::Document& doc) const {
    std::optional<std::size_t> at;
    for (std::size_t i = 0; i < doc.pages.size(); ++i)
        if (!doc.is_deferred(i) && doc.page(i).id == page_id_) at = i;
    if (!at) return false;
    core::Page& page = doc.edit_page(*at);
    const auto lent = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == layer_id_; });
    if (lent == page.layers.end()) throw core::OpError("the mask lent to the op was taken away");
    core::Layer layer = *lent;
    page.layers.erase(lent);
    const auto owner = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == owner_; });
    if (owner == page.layers.end()) return false;
    if (layer.stroke_count() > 0 || !layer.patches.empty()) raster::bake_vectors(page, layer);
    const std::size_t n = static_cast<std::size_t>(lent_size_.width) * static_cast<std::size_t>(lent_size_.height);
    std::vector<std::uint8_t> changed(n, 0);
    std::vector<float> made(n, 0.0f);
    int x0 = lent_size_.width, y0 = lent_size_.height, x1 = -1, y1 = -1;
    const auto mark = [&](std::size_t i, double v) {
        changed[i] = 1;
        made[i] = static_cast<float>(v);
        const int x = static_cast<int>(i % static_cast<std::size_t>(lent_size_.width)), y = static_cast<int>(i / static_cast<std::size_t>(lent_size_.width));
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    if (layer.raster_png && !layer.raster_png->empty()) {
        Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
        const bool same = picture.size() == lent_size_;
        if (!same) picture = picture.resize(lent_size_, Resample::Bilinear);
        const std::string now = picture.tobytes();
        for (std::size_t i = 0; i < n; ++i) {
            const char* px = now.data() + i * 4;
            if (same && std::equal(px, px + 4, lent_.data() + i * 4)) continue;
            mark(i, value_of(static_cast<unsigned char>(px[0]), static_cast<unsigned char>(px[1]), static_cast<unsigned char>(px[2]),
                             static_cast<unsigned char>(px[3])));
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) mark(i, 0.0);  // (the op left no pixels: all hidden)
    }
    if (x1 < 0) return false;  // (nothing changed: the mask as it was, its bytes too)
    // (each pixel of the mask by the changed pixels it reads, by their share: v(1 − share) + Σ share × value)
    std::string own = own_;
    const Taps across = taps(lent_size_.width, own_size_.width), down = taps(lent_size_.height, own_size_.height);
    for (int y = 0; y < own_size_.height; ++y) {
        bool row_reached = false;
        for (const auto& [row, wy] : down[static_cast<std::size_t>(y)]) row_reached = row_reached || (row >= y0 && row <= y1 && wy > 0);
        if (!row_reached) continue;
        for (int x = 0; x < own_size_.width; ++x) {
            double share = 0.0, value = 0.0;
            for (const auto& [row, wy] : down[static_cast<std::size_t>(y)]) {
                for (const auto& [col, wx] : across[static_cast<std::size_t>(x)]) {
                    const std::size_t i = static_cast<std::size_t>(row) * static_cast<std::size_t>(lent_size_.width) + static_cast<std::size_t>(col);
                    if (!changed[i]) continue;
                    share += wy * wx;
                    value += wy * wx * made[i];
                }
            }
            if (!(share > 0)) continue;
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(own_size_.width) + static_cast<std::size_t>(x);
            const double before = static_cast<unsigned char>(own[i]) / 255.0;
            own[i] = static_cast<char>(byte_of(share >= 1.0 ? value : before * (1.0 - share) + value));
        }
    }
    core::Mask mask = owner->mask ? *owner->mask : core::Mask{};
    mask.png = fills::png_bytes(Image::frombytes("L", own_size_, own));
    mask.memo = {};
    if (made_) {
        mask.enabled = true;
        mask.linked = true;  // (a new mask moves with its layer, as Photoshop's: masks@1)
    }
    owner->mask = std::move(mask);
    return true;
}

}  // namespace genko::render::layer_masks
