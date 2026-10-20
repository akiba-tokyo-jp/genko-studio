// A smart object drawn on its page (render/smart.hpp, PC3 A0). Upstream PhotoCraft v0.2.0's smart objects
// (engine/src/smart_cmds.rs:214-235: whole pixels moved as they are, anything else resampled in one pass — a warp before
// the transform, place_warped: PC3 A5 —; 167-207: a stack mode's layers each drawn alone, normal and not clipped, then
// combined — PC3 A4; MIT, see third_party/photocraft/NOTICE.md), drawn in Genko's three ways: the 8-bit page, a page in
// precise colour, a page of inks.

#include "render/smart.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/color_canvas.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/layer_transform.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page_internal.hpp"
#include "render/placed.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/smart_decode.hpp"
#include "render/smart_filters.hpp"
#include "render/stack.hpp"
#include "storage/asset_store.hpp"

namespace genko::render::smart {

using core::Json;
using core::smart::Matrix;
using detail::Ctx;

namespace {

// --- what is remembered between tiles ---------------------------------------------------------------------------------

// A few things remembered by a key, the most recent first, within a count and a budget of bytes.
template <class T>
class Recent {
public:
    Recent(std::size_t capacity, std::size_t budget) : capacity_(capacity), budget_(budget) {}

    std::shared_ptr<const T> get(const std::string& key) {
        std::lock_guard lock(mutex_);
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->key == key) {
                items_.splice(items_.begin(), items_, it);
                return items_.front().value;
            }
        }
        return nullptr;
    }

    void put(const std::string& key, std::shared_ptr<const T> value, std::size_t bytes) {
        if (bytes > budget_) return;  // (too big to keep: made again when asked)
        std::lock_guard lock(mutex_);
        items_.remove_if([&key](const Item& item) { return item.key == key; });
        items_.push_front(Item{key, std::move(value), bytes});
        std::size_t total = 0;
        for (const Item& item : items_) total += item.bytes;
        while (items_.size() > capacity_ || (items_.size() > 1 && total > budget_)) {
            total -= items_.back().bytes;
            items_.pop_back();
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        items_.clear();
    }

    std::size_t bytes() {
        std::lock_guard lock(mutex_);
        std::size_t total = 0;
        for (const Item& item : items_) total += item.bytes;
        return total;
    }

private:
    struct Item {
        std::string key;
        std::shared_ptr<const T> value;
        std::size_t bytes = 0;
    };
    std::mutex mutex_;
    std::list<Item> items_;
    std::size_t capacity_;
    std::size_t budget_;
};

constexpr std::size_t kBudget = 512ull * 1024 * 1024;  // (each kind: 16 of them at most, 512 MiB)

Recent<Image>& pictures() {  // (the files' 8-bit pictures, the contents drawn whole, a picture laid on the page)
    static Recent<Image> recent(16, kBudget);
    return recent;
}
Recent<std::vector<float>>& deep_pictures() {  // (the files' pictures with every bit, as sRGB floats)
    static Recent<std::vector<float>> recent(16, kBudget);
    return recent;
}
Recent<core::InkPixels>& ink_pictures() {  // (the files and contents as a page's inks)
    static Recent<core::InkPixels> recent(16, kBudget);
    return recent;
}
// (PC3 A2: a smart object's filtered picture of the whole page in precise colour; A4: contents stacked in it)
Recent<ColorPicture>& precise_filtered() {
    static Recent<ColorPicture> recent(16, kBudget);
    return recent;
}
Recent<InkPicture>& inks_filtered() {  // (and in a page's inks)
    static Recent<InkPicture> recent(16, kBudget);
    return recent;
}

std::atomic<std::uint64_t> g_contents_drawn{0};
std::atomic<std::uint64_t> g_files_decoded{0};

std::size_t image_bytes(const Image& image) { return static_cast<std::size_t>(image.width()) * image.height() * 4; }

// A name for the contents as they are now: the object the book holds (contents are never changed once made: the same
// object, the same contents), numbered while it lives — another object made later where it was is another number.
std::string identity(const core::Document& doc, const core::SmartContents& contents) {
    static std::mutex mutex;
    static std::unordered_map<const void*, std::pair<std::weak_ptr<const core::SmartContents>, std::uint64_t>> serials;
    static std::uint64_t next = 0;
    std::shared_ptr<const core::SmartContents> owner;
    for (const auto& [id, held] : doc.smart_contents) {
        if (held.get() == &contents) owner = held;
    }
    std::lock_guard lock(mutex);
    if (owner == nullptr) return "unheld " + std::to_string(++next);  // (not remembered)
    auto& entry = serials[&contents];
    if (entry.first.lock() == owner) return std::to_string(entry.second);
    entry = {owner, ++next};
    if (serials.size() > 4096) std::erase_if(serials, [](const auto& item) { return item.second.first.expired(); });
    return std::to_string(entry.second);
}

// --- the smart object's settings ---------------------------------------------------------------------------------------

// A bent smart object's placing (PC3 A5): its warp (in its contents' mm, render/layer_transform's form) and then its
// corners' homography or its matrix — its contents' mm onto the page's —, and what it is made of (its settings that
// place it, for the pictures remembered).
struct Placing {
    std::shared_ptr<const core::page_geometry::Warp> warp;
    resample::Homography place;
    std::string key;
    std::pair<double, double> apply(double x, double y) const {
        if (warp) {
            const auto p = warp->map(x, y);
            return place.apply(p.x, p.y);
        }
        return place.apply(x, y);
    }
};

// Its placing when it is bent (a warp, or corners of its own); none when its matrix alone places it. Settings this build
// cannot read (a book changed by hand): the smart object left out and said.
std::optional<Placing> placing_of(const core::Layer& layer, const core::SmartContents& contents) {
    if (!core::smart::bent(layer)) return std::nullopt;
    Placing out;
    const auto [w, h] = core::smart::size_mm(contents);
    if (const Json* warp = core::get(*layer.smart, "warp"); warp != nullptr && !warp->is_null()) {
        try {
            const core::page_geometry::Warp read = layer_transform::warp_from_json(*warp, {0, 0, w, h});
            if (!read.is_identity()) out.warp = std::make_shared<const core::page_geometry::Warp>(read);
        } catch (const std::exception&) {
            throw NotYetPorted("smart:warp");
        }
    }
    const Json* quad = core::get(*layer.smart, "perspective");
    if (quad != nullptr && !quad->is_null() && !core::smart::convex(core::smart::corners_on_page(layer, contents))) throw NotYetPorted("smart:perspective");
    out.place = layer_transform::smart_placing(layer, contents);
    Json made = Json::array({core::smart::to_json(core::smart::matrix_of(layer)), quad != nullptr ? *quad : Json(), out.warp ? *core::get(*layer.smart, "warp") : Json()});
    out.key = core::dump_canonical(made);
    return out;
}

// How much it enlarges at most over its contents' box (w × h mm): its placing's Jacobian's larger singular value at a
// grid of places.
double largest_scale(const Placing& p, double w, double h) {
    double most = 0;
    constexpr int kSamples = 8;
    const double dx = std::max(w, 1e-6) * 1e-3, dy = std::max(h, 1e-6) * 1e-3;
    for (int j = 0; j <= kSamples; ++j) {
        for (int i = 0; i <= kSamples; ++i) {
            const double x = w * i / kSamples, y = h * j / kSamples;
            const auto [ax, ay] = p.apply(x - dx, y);
            const auto [bx, by] = p.apply(x + dx, y);
            const auto [cx, cy] = p.apply(x, y - dy);
            const auto [ex, ey] = p.apply(x, y + dy);
            const Matrix jac{(bx - ax) / (2 * dx), (by - ay) / (2 * dx), (ex - cx) / (2 * dy), (ey - cy) / (2 * dy), 0, 0};
            const double s = core::smart::largest_scale(jac);
            if (std::isfinite(s)) most = std::max(most, s);
        }
    }
    return most > 0 ? most : 1.0;
}

resample::Interp bend_interp(Resample how) {
    if (how == Resample::Nearest) return resample::Interp::Nearest;
    if (how == Resample::Bilinear) return resample::Interp::Bilinear;
    return resample::Interp::Bicubic;  // (bicubic, and lanczos: the resampler's finest)
}

// --- a PSD's picture of it (PC3 A3) -------------------------------------------------------------------------------------

// The asset of the picture Photoshop drew of a smart object read from a PSD (its psd.shown: "asset", a PNG over the page
// as a paint layer's, or "ink_raster", a page of inks' GKIR), none when it has none.
std::optional<std::string> shown_ref(const core::Layer& layer, std::string_view key) {
    if (!layer.smart || !layer.smart->is_object()) return std::nullopt;
    const Json* psd = core::get(*layer.smart, "psd");
    const Json* shown = psd != nullptr && psd->is_object() ? core::get(*psd, "shown") : nullptr;
    const Json* ref = shown != nullptr && shown->is_object() ? core::get(*shown, key) : nullptr;
    if (ref == nullptr || !ref->is_string()) return std::nullopt;
    return ref->get<std::string>();
}

// Its bytes: from the files ops kept, else the book's assets; null when the book does not hold them (the reader says so).
core::Bytes shown_bytes(const core::Document& doc, const std::string& ref, std::string_view suffix) {
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) return held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(ref)) return nullptr;
    auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(ref, suffix, storage::kAssetMaxBytes);
    if (!bytes) return nullptr;
    return std::make_shared<const std::string>(std::move(*bytes));
}

// Its PNG laid over the page as a placed picture of the whole page is (what the PSD's layer was, stretched over the page
// as a paint layer's picture; the monochrome finish in print as the smart object's file gets it). None when it has none.
std::optional<std::optional<Image>> shown_8bit(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    const auto ref = shown_ref(layer, "asset");
    if (!ref) return std::nullopt;
    const core::Document& doc = *ctx.episode;
    const std::string key = "shown " + (doc.asset_dir ? core::path_to_utf8(*doc.asset_dir) : std::string()) + "\n" + *ref;
    const auto picture = [&]() -> std::shared_ptr<const Image> {
        if (auto kept = pictures().get(key)) return kept;
        const core::Bytes bytes = shown_bytes(doc, *ref, ".png");
        if (!bytes) return nullptr;
        auto made = std::make_shared<const Image>(open_image(*bytes, kPillowOpenLimits).convert("RGBA"));
        pictures().put(key, made, image_bytes(*made));
        return made;
    };
    const core::Page& page = *ctx.page;
    const core::Rect whole{core::Num(0.0), core::Num(0.0), page.spec.width_mm, page.spec.height_mm};
    return placed_picture(layer, whole, picture, key, Resample::Bicubic, page, ctx.episode, ctx.size, ctx.dpi, ctx.mode, ctx.finish, ctx.dots, area);
}

// Its inks (a page of inks' smart object read from a PSD onto it), over `area`; none when it has none of the page's inks.
std::optional<InkPicture> shown_inks(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    const auto ref = shown_ref(layer, "ink_raster");
    if (!ref) return std::nullopt;
    const core::Document& doc = *ctx.episode;
    const std::string key = "shown inks " + (doc.asset_dir ? core::path_to_utf8(*doc.asset_dir) : std::string()) + "\n" + *ref;
    std::shared_ptr<const core::InkPixels> inks = ink_pictures().get(key);
    if (inks == nullptr) {
        const core::Bytes bytes = shown_bytes(doc, *ref, core::kInkRasterSuffix);
        if (!bytes) return std::nullopt;
        inks = decoded_ink(bytes);
        if (inks == nullptr) return std::nullopt;
        ink_pictures().put(key, inks, inks->samples.size() * 2 + inks->floats.size() * 4);
    }
    if (inks->space != ctx.page->color->space) return std::nullopt;
    return sample_ink_raster(*inks, ctx.size, area);
}

// The statistic the smart object's stack mode combines its contents' layers by (PC3 A4); none when it has no stack
// mode. A name this build does not know (a book changed by hand): the smart object left out and said.
std::optional<stack::Stat> stack_of(const core::Layer& layer) {
    const Json* v = layer.smart ? core::get(*layer.smart, "stack") : nullptr;
    if (v == nullptr || v->is_null()) return std::nullopt;
    const std::optional<stack::Stat> stat = v->is_string() ? stack::stat_of(v->get<std::string>()) : std::nullopt;
    if (!stat) throw NotYetPorted("smart:stack");
    return stat;
}
std::string stack_name(const core::Layer& layer) { return core::py_str((*layer.smart)["stack"]); }

std::string interp_of(const core::Layer& layer, const core::SmartContents& contents) {
    const Json* v = layer.smart ? core::get(*layer.smart, "interp") : nullptr;
    if (v != nullptr && v->is_string()) return v->get<std::string>();
    return contents.kind == "file" ? "lanczos" : "bicubic";
}

Resample resample_of(std::string_view interp) {
    if (interp == "nearest") return Resample::Nearest;
    if (interp == "bilinear") return Resample::Bilinear;
    if (interp == "lanczos") return Resample::Lanczos;
    return Resample::Bicubic;
}

// The matrix from a picture's pixels (`px_per_mm` of them to the contents' mm) to the page's pixels at `dpi`.
Matrix pixel_matrix(const Matrix& m, double px_per_mm, int dpi) {
    const double k = dpi / 25.4;
    const double s = k / px_per_mm;
    return Matrix{m.a * s, m.b * s, m.c * s, m.d * s, m.e * k, m.f * k};
}

// The whole pixels a matrix moves a picture by, when that is all it does.
std::optional<std::pair<int, int>> whole_shift(const Matrix& pm) {
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    if (!near(pm.a, 1) || !near(pm.b, 0) || !near(pm.c, 0) || !near(pm.d, 1)) return std::nullopt;
    const double x = std::round(pm.e), y = std::round(pm.f);
    if (!near(pm.e, x) || !near(pm.f, y) || std::abs(x) > 1e8 || std::abs(y) > 1e8) return std::nullopt;
    return std::make_pair(static_cast<int>(x), static_cast<int>(y));
}

// The resolution contents are drawn at to be laid on the page at `dpi` through `m`: as fine as the page's pixels where
// they land (its largest scale), whole dpi, 8 to 2400.
int contents_dpi(const Matrix& m, int dpi) {
    const double scale = core::smart::largest_scale(m);
    if (std::abs(scale - 1) < 1e-9) return dpi;
    return static_cast<int>(std::clamp(std::ceil(dpi * scale - 1e-6), 8.0, 2400.0));
}

int to_px(double mm, int dpi) { return mm_to_px(mm, dpi); }

// --- a picture laid on the page through a matrix (anything but whole pixels moved) --------------------------------------

double kernel(Resample how, double x) {
    x = std::abs(x);
    switch (how) {
    case Resample::Bilinear: return x < 1 ? 1 - x : 0;
    case Resample::Lanczos: {
        if (x >= 3) return 0;
        if (x < 1e-12) return 1;
        const double pi = 3.14159265358979323846;
        return 3 * std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x);
    }
    default: {  // bicubic (a = -0.5, as Pillow's)
        const double a = -0.5;
        if (x < 1) return ((a + 2) * x - (a + 3)) * x * x + 1;
        if (x < 2) return (((x - 5) * x + 8) * x - 4) * a;
        return 0;
    }
    }
}

double support(Resample how) {
    switch (how) {
    case Resample::Bilinear: return 1;
    case Resample::Lanczos: return 3;
    default: return 2;
    }
}

// The picture (w × h pixels of n values, the last its straight alpha) laid on the page through `pm` (its pixels → the
// page's), over the box `box` of the page: each pixel the picture's values at the point it comes from, weighed by the
// interpolation's kernel (widened where the picture is made smaller) over the picture's own pixels, premultiplied by the
// alpha; times how much of the page's pixel the picture covers (its edges soft by a pixel of the page, what lies off it
// clear). The same pixel for the same place whatever box it is asked in.
template <class T>
std::vector<double> lay(const std::vector<T>& src, int w, int h, unsigned n, const Matrix& pm, const Box& box, Resample how, const Ctx* ctx) {
    std::vector<double> out(std::size_t(std::max(0, box.width())) * std::max(0, box.height()) * n, 0.0);
    const auto inv = core::smart::inverse(pm);
    if (!inv || out.empty()) return out;
    const double fx = std::min(1.0, std::hypot(pm.a, pm.b)), fy = std::min(1.0, std::hypot(pm.c, pm.d));
    const double rx = how == Resample::Nearest ? 0 : support(how) / fx, ry = how == Resample::Nearest ? 0 : support(how) / fy;
    // (the page's pixels to the picture's edges: one picture pixel along u or v is this many of the page's)
    const double gu = std::hypot(inv->a, inv->c), gv = std::hypot(inv->b, inv->d);
    std::vector<double> wx, wy, acc(n);
    for (int y = box.y0; y < box.y1; ++y) {
        if (ctx != nullptr && (y - box.y0) % 64 == 0) detail::check_cancel(*ctx);
        for (int x = box.x0; x < box.x1; ++x) {
            const auto [u, v] = inv->apply(x + 0.5, y + 0.5);
            double* to = out.data() + (std::size_t(y - box.y0) * box.width() + (x - box.x0)) * n;
            if (how == Resample::Nearest) {
                const auto i = static_cast<long long>(std::floor(u)), j = static_cast<long long>(std::floor(v));
                if (i < 0 || j < 0 || i >= w || j >= h) continue;
                const T* from = src.data() + (std::size_t(j) * w + std::size_t(i)) * n;
                std::copy(from, from + n, to);
                continue;
            }
            const double cover = std::clamp(0.5 + std::min(u, w - u) / (gu > 0 ? gu : 1), 0.0, 1.0) *
                                 std::clamp(0.5 + std::min(v, h - v) / (gv > 0 ? gv : 1), 0.0, 1.0);
            if (cover <= 0) continue;
            // (the kernel over the picture's pixels only, weighed again to one, as a picture made larger or smaller is)
            const long long i0 = std::max(0LL, static_cast<long long>(std::floor(u - rx))), i1 = std::min<long long>(w - 1, static_cast<long long>(std::ceil(u + rx)));
            const long long j0 = std::max(0LL, static_cast<long long>(std::floor(v - ry))), j1 = std::min<long long>(h - 1, static_cast<long long>(std::ceil(v + ry)));
            if (i1 < i0 || j1 < j0) continue;
            wx.assign(std::size_t(i1 - i0 + 1), 0.0);
            wy.assign(std::size_t(j1 - j0 + 1), 0.0);
            double sx = 0, sy = 0;
            for (long long i = i0; i <= i1; ++i) sx += wx[std::size_t(i - i0)] = kernel(how, (i + 0.5 - u) * fx);
            for (long long j = j0; j <= j1; ++j) sy += wy[std::size_t(j - j0)] = kernel(how, (j + 0.5 - v) * fy);
            if (sx == 0 || sy == 0) {
                // (a point between the picture's pixels where the kernel gives none: the nearest one)
                const long long i = std::clamp<long long>(static_cast<long long>(std::floor(u)), 0, w - 1);
                const long long j = std::clamp<long long>(static_cast<long long>(std::floor(v)), 0, h - 1);
                const T* from = src.data() + (std::size_t(j) * w + std::size_t(i)) * n;
                std::copy(from, from + n, to);
                to[n - 1] = from[n - 1] * cover;
                continue;
            }
            std::fill(acc.begin(), acc.end(), 0.0);
            for (long long j = j0; j <= j1; ++j) {
                const double wj = wy[std::size_t(j - j0)];
                if (wj == 0) continue;
                for (long long i = i0; i <= i1; ++i) {
                    const double wij = wj * wx[std::size_t(i - i0)];
                    if (wij == 0) continue;
                    const T* from = src.data() + (std::size_t(j) * w + std::size_t(i)) * n;
                    const double alpha = from[n - 1];
                    for (unsigned c = 0; c + 1 < n; ++c) acc[c] += wij * from[c] * alpha;
                    acc[n - 1] += wij * alpha;
                }
            }
            const double norm = sx * sy;
            const double alpha = std::clamp(acc[n - 1] / norm, 0.0, 1.0);
            to[n - 1] = alpha * cover;
            if (alpha <= 0) continue;
            for (unsigned c = 0; c + 1 < n; ++c) to[c] = acc[c] / norm / alpha;
        }
    }
    return out;
}

std::vector<double> rgba_values(const Image& rgba) {
    const Image in = rgba.mode() == "RGBA" ? rgba : rgba.convert("RGBA");
    std::vector<double> out(std::size_t(in.width()) * in.height() * 4);
    for (int y = 0; y < in.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(in.raw()->image[y]);
        for (int x = 0; x < in.width() * 4; ++x) out[std::size_t(y) * in.width() * 4 + x] = row[x] / 255.0;
    }
    return out;
}

Image rgba_image(const std::vector<double>& values, Size size) {
    std::string bytes(values.size(), '\0');
    for (std::size_t i = 0; i < values.size(); ++i) {
        bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(values[i], 0.0, 1.0) * 255.0)));
    }
    return Image::frombytes("RGBA", size, bytes);
}

// The box of the page a picture of w × h pixels covers through `pm`, within `limit`.
Box covered(int w, int h, const Matrix& pm, const Box& limit) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [u, v] : {std::pair<double, double>{0, 0}, {double(w), 0}, {0, double(h)}, {double(w), double(h)}}) {
        const auto [x, y] = pm.apply(u, v);
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    const double pad = 3;  // (the kernel's reach past the picture's edge)
    const Box out{static_cast<int>(std::max<double>(limit.x0, std::floor(x0 - pad))), static_cast<int>(std::max<double>(limit.y0, std::floor(y0 - pad))),
                  static_cast<int>(std::min<double>(limit.x1, std::ceil(x1 + pad))), static_cast<int>(std::min<double>(limit.y1, std::ceil(y1 + pad)))};
    return out;
}

// The box of the page the smart object is cut to (its panel's, clip_to) within the page, in pixels.
Box clip_px(const Ctx& ctx, const core::Layer& layer) {
    const core::Frame* frame = layer.frame_id && !layer.frame_id->empty() ? ctx.page->find_frame(*layer.frame_id) : nullptr;
    // (no panel: the whole page, its first row and column too — rect_px's sides are at least a pixel from 0)
    if (frame == nullptr || layer.clip_to == "none") return Box{0, 0, ctx.size.width, ctx.size.height};
    const Box clip = rect_px(core::clip_box(*ctx.page, frame, layer.clip_to), ctx.dpi);
    return Box{std::max(0, clip.x0), std::max(0, clip.y0), std::min(ctx.size.width, clip.x1), std::min(ctx.size.height, clip.y1)};
}

// A picture (RGBA) laid on the page through a matrix that is more than a box (turned, sheared, mirrored): made over the
// part of the page it may reach (its clip box) once, then laid as a placed picture of that box (placed_picture: its
// panel's shape, a picture's monochrome finish).
std::optional<Image> lay_turned(const Ctx& ctx, const core::Layer& layer, const std::shared_ptr<const Image>& source, const std::string& key,
                                const Matrix& pm, Resample how, bool finish, const Box& area) {
    const Box limit = clip_px(ctx, layer);
    const Box reach = covered(source->width(), source->height(), pm, limit);
    if (reach.width() <= 0 || reach.height() <= 0) return std::nullopt;
    const std::string laid_key = key + "\nlaid " + core::dump_python(Json::array({pm.a, pm.b, pm.c, pm.d, pm.e, pm.f, reach.x0, reach.y0, reach.x1,
                                                                                  reach.y1, static_cast<int>(how)}));
    std::shared_ptr<const Image> laid = pictures().get(laid_key);
    if (laid == nullptr) {
        const std::vector<double> values = lay(rgba_values(*source), source->width(), source->height(), 4, pm, reach, how, &ctx);
        laid = std::make_shared<const Image>(rgba_image(values, Size{reach.width(), reach.height()}));
        pictures().put(laid_key, laid, image_bytes(*laid));
    }
    const double mm = 25.4 / ctx.dpi;
    const core::Rect placement{core::Num(reach.x0 * mm), core::Num(reach.y0 * mm), core::Num(reach.width() * mm), core::Num(reach.height() * mm)};
    return placed_picture(layer, placement, [&] { return laid; }, laid_key, Resample::Lanczos, *ctx.page, ctx.episode, ctx.size, ctx.dpi, ctx.mode,
                          finish && ctx.finish, ctx.dots, area);
}

// --- a picture laid through a bent smart object's placing (PC3 A5) -----------------------------------------------------

// Where a picture's pixels land on the page's (its pixels — `px_per_mm` of them to the contents' mm — through the
// placing onto the page's at `dpi`).
resample::Forward page_pixels(const Placing& p, double px_per_mm, int dpi) {
    const double k = dpi / 25.4;
    return [&p, px_per_mm, k](double u, double v) {
        const auto [x, y] = p.apply(u / px_per_mm, v / px_per_mm);
        return std::pair<double, double>{x * k, y * k};
    };
}

// The box of the page a picture of w × h pixels reaches through a forward map: its edges and a grid inside it sampled,
// padded by the kernel's reach and a grid cell, within `limit`.
Box reached(const resample::Forward& f, int w, int h, const Box& limit) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    constexpr int kSamples = 32;
    for (int j = 0; j <= kSamples; ++j) {
        for (int i = 0; i <= kSamples; ++i) {
            const auto [x, y] = f(static_cast<double>(w) * i / kSamples, static_cast<double>(h) * j / kSamples);
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x0 > x1) return Box{limit.x0, limit.y0, limit.x0, limit.y0};
    const double pad = 3 + std::ceil(std::max(x1 - x0, y1 - y0) / kSamples);
    return Box{static_cast<int>(std::max<double>(limit.x0, std::floor(x0 - pad))), static_cast<int>(std::max<double>(limit.y0, std::floor(y0 - pad))),
               static_cast<int>(std::min<double>(limit.x1, std::ceil(x1 + pad))), static_cast<int>(std::min<double>(limit.y1, std::ceil(y1 + pad)))};
}

// A plane laid through the placing over `reach` of the page (render/resample: its corners alone a homography drawn exactly
// so; a warp through its cells' triangles).
resample::Plane bent_plane(const resample::Plane& src, const Placing& p, double px_per_mm, int dpi, const Box& reach, Resample how) {
    const resample::Interp interp = bend_interp(how);
    if (!p.warp) {
        const double k = dpi / 25.4;
        const resample::Homography to_page{{k, 0, 0, 0, k, 0, 0, 0, 1}};
        const resample::Homography from_px{{1.0 / px_per_mm, 0, 0, 0, 1.0 / px_per_mm, 0, 0, 0, 1}};
        return resample::project(src, to_page.mul(p.place).mul(from_px), reach.width(), reach.height(), interp, reach.x0, reach.y0);
    }
    return resample::mesh(src, page_pixels(p, px_per_mm, dpi), reach.width(), reach.height(), interp, reach.x0, reach.y0);
}

// A picture (RGBA) laid on the page through a bent smart object's placing: made over the part of the page it reaches
// (within its clip box) once and remembered, then laid as a placed picture of that box (its panel's shape, a picture's
// monochrome finish), as lay_turned lays one.
std::optional<Image> lay_bent(const Ctx& ctx, const core::Layer& layer, const std::shared_ptr<const Image>& source, const std::string& key, const Placing& p,
                              double px_per_mm, Resample how, bool finish, const Box& area) {
    const Box reach = reached(page_pixels(p, px_per_mm, ctx.dpi), source->width(), source->height(), clip_px(ctx, layer));
    if (reach.width() <= 0 || reach.height() <= 0) return std::nullopt;
    const std::string laid_key = key + "\nbent " + p.key + " " + core::dump_python(Json::array({px_per_mm, ctx.dpi, reach.x0, reach.y0, reach.x1, reach.y1,
                                                                                               static_cast<int>(how)}));
    std::shared_ptr<const Image> laid = pictures().get(laid_key);
    if (laid == nullptr) {
        detail::check_cancel(ctx);
        laid = std::make_shared<const Image>(resample::to_rgba8(bent_plane(resample::from_rgba8(*source), p, px_per_mm, ctx.dpi, reach, how)));
        pictures().put(laid_key, laid, image_bytes(*laid));
    }
    const double mm = 25.4 / ctx.dpi;
    const core::Rect placement{core::Num(reach.x0 * mm), core::Num(reach.y0 * mm), core::Num(reach.width() * mm), core::Num(reach.height() * mm)};
    return placed_picture(layer, placement, [&] { return laid; }, laid_key, Resample::Lanczos, *ctx.page, ctx.episode, ctx.size, ctx.dpi, ctx.mode,
                          finish && ctx.finish, ctx.dots, area);
}

// The resolution contents are drawn at to be laid through a bent placing: as fine as the page's pixels where they land
// most enlarged.
int bent_dpi(const Placing& p, const core::SmartContents& contents, int dpi) {
    const auto [w, h] = core::smart::size_mm(contents);
    const double scale = largest_scale(p, w, h);
    if (std::abs(scale - 1) < 1e-9) return dpi;
    return static_cast<int>(std::clamp(std::ceil(dpi * scale - 1e-6), 8.0, 2400.0));
}

// A picture of w × h pixels (`px_per_mm` of them to the contents' mm) laid on the page through the smart object's matrix
// `m`: as a placed picture of the box it lands on when the matrix only scales and moves it (the same pixels as
// place_asset's), else turned (lay_turned); through its placing when it is bent (PC3 A5, lay_bent).
std::optional<Image> lay_picture(const Ctx& ctx, const core::Layer& layer, const std::function<std::shared_ptr<const Image>()>& picture,
                                 std::int64_t w, std::int64_t h, double px_per_mm, const std::string& key, const Matrix& m, Resample how,
                                 bool finish, const Box& area, const std::optional<Placing>& bend = std::nullopt) {
    if (bend) {
        const std::shared_ptr<const Image> source = picture();
        if (source == nullptr) return std::nullopt;
        return lay_bent(ctx, layer, source, key, *bend, px_per_mm, how, finish, area);
    }
    if (core::smart::axis_aligned(m)) {
        const auto snap = [](double v) { return std::round(v * 1e9) / 1e9; };
        const core::Rect placement{core::Num(m.e), core::Num(m.f), core::Num(snap(m.a * static_cast<double>(w) / px_per_mm)),
                                   core::Num(snap(m.d * static_cast<double>(h) / px_per_mm))};
        return placed_picture(layer, placement, picture, key, how, *ctx.page, ctx.episode, ctx.size, ctx.dpi, ctx.mode, finish && ctx.finish, ctx.dots,
                              area);
    }
    const std::shared_ptr<const Image> source = picture();
    if (source == nullptr) return std::nullopt;
    return lay_turned(ctx, layer, source, key, pixel_matrix(m, px_per_mm, ctx.dpi), how, finish, area);
}

// --- the contents drawn ---------------------------------------------------------------------------------------------

RenderOptions contents_options(const Ctx& ctx) {
    RenderOptions o;
    o.mode = ctx.mode;
    o.finish = ctx.finish;
    o.rough = ctx.rough;
    o.dots = ctx.dots;
    o.screen_dots = ctx.screen_dots;
    o.skip_unported = ctx.skip_unported;
    o.stop = ctx.stop;
    return o;
}

void left_out(const Ctx& ctx, const std::vector<std::string>& omitted) {
    for (const std::string& element : omitted) detail::skip_unported(ctx, element);
}

std::string contents_key(const Ctx& ctx, const core::SmartContents& contents, int dpi) {
    return "contents " + identity(*ctx.episode, contents) + " " + std::to_string(dpi) + " " + ctx.mode + (ctx.finish ? " f" : "") + (ctx.rough ? " r" : "") +
           (ctx.dots ? " d" : "") + (ctx.screen_dots ? " s" : "");
}

// The contents' page drawn whole at `dpi` ("RGBA", over nothing), remembered.
std::shared_ptr<const Image> contents_whole(const Ctx& ctx, const core::SmartContents& contents, int dpi) {
    const std::string key = contents_key(ctx, contents, dpi);
    if (auto kept = pictures().get(key)) return kept;
    ++g_contents_drawn;
    std::shared_ptr<const Image> out;
    if (contents.page->color) {  // (a page of inks: as the screen shows them)
        const core::InkPixels inks = composite_ink_layers(*contents.page, contents.page->layers, dpi, ctx.episode, false);
        out = std::make_shared<const Image>(ink::to_rgba8(inks, *contents.page->color));
    } else {
        RenderResult drawn = render_contents(*contents.page, dpi, contents_options(ctx), ctx.episode);
        left_out(ctx, drawn.omitted);
        out = std::make_shared<const Image>(std::move(drawn.image));
    }
    pictures().put(key, out, image_bytes(*out));
    return out;
}

// The contents' page drawn over a box of its own pixels at `dpi` ("RGBA"), what lies off it clear.
Image contents_part(const Ctx& ctx, const core::SmartContents& contents, int dpi, const Box& box) {
    const Size size{to_px(contents.page->spec.width_mm.value(), dpi), to_px(contents.page->spec.height_mm.value(), dpi)};
    Image out = Image::create("RGBA", Size{box.width(), box.height()}, Ink{0, 0, 0, 0});
    const Box inside{std::max(0, box.x0), std::max(0, box.y0), std::min(size.width, box.x1), std::min(size.height, box.y1)};
    if (inside.width() <= 0 || inside.height() <= 0) return out;
    if (contents.page->color) {
        out.paste(contents_whole(ctx, contents, dpi)->crop(inside), Point{inside.x0 - box.x0, inside.y0 - box.y0});
        return out;
    }
    RenderOptions o = contents_options(ctx);
    o.region = RenderRegion{inside.x0, inside.y0, inside.width(), inside.height()};
    RenderResult drawn = render_contents(*contents.page, dpi, o, ctx.episode);
    left_out(ctx, drawn.omitted);
    out.paste(drawn.image, Point{inside.x0 - box.x0, inside.y0 - box.y0});
    return out;
}

std::optional<Image> page_8bit(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m, const Box& area) {
    const std::optional<Placing> bend = placing_of(layer, contents);  // (PC3 A5)
    const int dpi = bend ? bent_dpi(*bend, contents, ctx.dpi) : contents_dpi(m, ctx.dpi);
    const Matrix pm = pixel_matrix(m, dpi / 25.4, ctx.dpi);
    if (const auto shift = bend ? std::nullopt : whole_shift(pm)) {  // (whole pixels moved: its own pixels, part by part)
        const Box limit = clip_px(ctx, layer);
        const Box part{std::max(area.x0, limit.x0), std::max(area.y0, limit.y0), std::min(area.x1, limit.x1), std::min(area.y1, limit.y1)};
        if (part.width() <= 0 || part.height() <= 0) return std::nullopt;
        const Image drawn = contents_part(ctx, contents, dpi, Box{part.x0 - shift->first, part.y0 - shift->second, part.x1 - shift->first,
                                                                  part.y1 - shift->second});
        Image out = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
        out.paste(drawn, Point{part.x0 - area.x0, part.y0 - area.y0});
        return out;
    }
    const auto whole = [&] { return contents_whole(ctx, contents, dpi); };
    const std::int64_t w = to_px(contents.page->spec.width_mm.value(), dpi), h = to_px(contents.page->spec.height_mm.value(), dpi);
    return lay_picture(ctx, layer, whole, w, h, dpi / 25.4, contents_key(ctx, contents, dpi), m, resample_of(interp_of(layer, contents)), false,
                       area, bend);
}

// --- a file's picture ---------------------------------------------------------------------------------------------------

std::string file_key(const core::Document& doc, const core::SmartContents& contents) {
    return "file " + (doc.asset_dir ? core::path_to_utf8(*doc.asset_dir) : std::string()) + "\n" + contents.asset + "\n" +
           core::dump_canonical(contents.profile);
}

// The ICC profile the contents' colour use takes its colours from (none: keep, or a file that carries none).
std::optional<std::string> use_profile(const core::Document& doc, const core::SmartContents& contents, std::string_view bytes) {
    const Json* use = core::get(contents.profile, "use");
    const std::string way = use != nullptr && use->is_string() ? use->get<std::string>() : "keep";
    if (way == "embedded") return proof::embedded_profile(bytes);
    if (way != "assign") return std::nullopt;
    const Json* icc = core::get(contents.profile, "icc");
    if (icc == nullptr || !icc->is_string()) return std::nullopt;
    const std::string ref = icc->get<std::string>();
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) return *held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(ref)) return std::nullopt;
    return storage::AssetStore(*doc.asset_dir).get_bytes(ref, core::kProfileSuffix, std::size_t(16) << 20);
}

std::string intent_of(const core::SmartContents& contents) {
    const Json* intent = core::get(contents.profile, "intent");
    return intent != nullptr && intent->is_string() ? intent->get<std::string>() : std::string("relative");
}
bool bpc_of(const core::SmartContents& contents) {
    const Json* bpc = core::get(contents.profile, "bpc");
    return bpc == nullptr || !bpc->is_boolean() || bpc->get<bool>();
}

// `one` (PC3 A4): what a stack mode makes of the file's one sample (stack::of_one, `mode` its name) — its colours that
// value where it shows, its alpha kept.
std::optional<Image> file_8bit(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m, const Box& area,
                               std::optional<double> one = std::nullopt, const std::string& mode = {}) {
    const core::Document& doc = *ctx.episode;
    const std::string key = file_key(doc, contents) + (one ? "\nstack " + mode : std::string());
    const auto picture = [&]() -> std::shared_ptr<const Image> {
        if (!one) return file_picture(doc, contents);
        if (auto kept = pictures().get(key)) return kept;
        const std::shared_ptr<const Image> shown = file_picture(doc, contents);
        if (shown == nullptr) return nullptr;
        const int v = static_cast<int>(std::lround(std::clamp(*one, 0.0, 1.0) * 255.0));
        Image flat = Image::create("RGBA", shown->size(), Ink{v, v, v, 255});
        flat.putalpha(shown->getchannel(3));
        auto out = std::make_shared<const Image>(std::move(flat));
        pictures().put(key, out, image_bytes(*out));
        return out;
    };
    const double dpi = contents.dpi > 0 ? contents.dpi : 72.0;
    return lay_picture(ctx, layer, picture, contents.width_px, contents.height_px, dpi / 25.4, key, m, resample_of(interp_of(layer, contents)), true,
                       area, placing_of(layer, contents));
}

// --- a file with every bit, on a page in precise colour ------------------------------------------------------------------

bool deep_file(const core::SmartContents& contents) {
    return contents.kind == "file" && contents.depth > 8 && !contents.mode.starts_with("cmyk") && !contents.mode.starts_with("lab");
}

// The file's picture with every bit as sRGB floats (straight RGBA), through its colour use, remembered.
std::shared_ptr<const std::vector<float>> deep_srgb(const core::Document& doc, const core::SmartContents& contents) {
    const std::string key = file_key(doc, contents) + "\ndeep";
    if (auto kept = deep_pictures().get(key)) return kept;
    const core::Bytes bytes = file_bytes(doc, contents);
    if (!bytes) return nullptr;
    ++g_files_decoded;
    const DeepPicture deep = deep_picture(*bytes);
    if (deep.width != contents.width_px || deep.height != contents.height_px) throw core::Error("format", "the smart object's file is not the size it was");
    auto out = std::make_shared<const std::vector<float>>(srgb_floats(deep, use_profile(doc, contents, *bytes), intent_of(contents), bpc_of(contents)));
    deep_pictures().put(key, out, out->size() * sizeof(float));
    return out;
}

// The shape a smart object's panel cuts it to over `area` ("L"), when its panel is not a plain box (as placed_picture cuts).
std::optional<Image> panel_shape(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    const core::Frame* frame = layer.frame_id && !layer.frame_id->empty() ? ctx.page->find_frame(*layer.frame_id) : nullptr;
    const bool shaped = frame != nullptr && ((frame->poly && !frame->poly->empty()) || frame->corner_mm > 0 || (frame->curves && !frame->curves->empty()));
    if (!shaped || !(layer.clip_to == "frame" || layer.clip_to == "bleed")) return std::nullopt;
    // (placed_picture draws the same shape: a clear picture laid through it gives its mask)
    Image white = Image::create("RGBA", Size{1, 1}, Ink{255, 255, 255, 255});
    const auto shown = std::make_shared<const Image>(std::move(white));
    const double mm = 25.4 / ctx.dpi;
    const core::Rect all{core::Num(area.x0 * mm), core::Num(area.y0 * mm), core::Num(area.width() * mm), core::Num(area.height() * mm)};
    core::Layer cut = layer;
    auto drawn = placed_picture(cut, all, [&] { return shown; }, "shape", Resample::Nearest, *ctx.page, ctx.episode, ctx.size, ctx.dpi, "name", false,
                                ctx.dots, area);
    if (!drawn) return Image::create("L", Size{area.width(), area.height()}, Ink(0));
    return drawn->getchannel(3);
}

std::optional<ColorPicture> file_precise(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m,
                                         const Box& area, const RgbSpace& space) {
    const core::Document& doc = *ctx.episode;
    const auto srgb = deep_srgb(doc, contents);
    if (srgb == nullptr) return std::nullopt;
    const int w = static_cast<int>(contents.width_px), h = static_cast<int>(contents.height_px);
    const double dpi = contents.dpi > 0 ? contents.dpi : 72.0;
    const Matrix pm = pixel_matrix(m, dpi / 25.4, ctx.dpi);
    const Box limit = clip_px(ctx, layer);
    const Box part{std::max(area.x0, limit.x0), std::max(area.y0, limit.y0), std::min(area.x1, limit.x1), std::min(area.y1, limit.y1)};
    ColorPicture out{Size{area.width(), area.height()}, std::vector<std::array<double, 4>>(std::size_t(area.width()) * area.height(), {0, 0, 0, 0})};
    if (part.width() <= 0 || part.height() <= 0) return out;
    // (its sRGB values into the space's, then laid on the page in linear light as the canvas mixes; remembered)
    const std::string key = file_key(doc, contents) + "\nlinear " + (ctx.page->rgb ? core::dump_canonical(core::to_json(*ctx.page->rgb)) : "srgb");
    std::shared_ptr<const std::vector<float>> linear = deep_pictures().get(key);
    if (linear == nullptr) {
        std::vector<double> rgb(std::size_t(w) * h * 3);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i)
            for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = (*srgb)[i * 4 + c];
        space.from_srgb(rgb.data(), std::size_t(w) * h);
        std::vector<float> values(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
            for (unsigned c = 0; c < 3; ++c) values[i * 4 + c] = static_cast<float>(space.decode(c, rgb[i * 3 + c]));
            values[i * 4 + 3] = (*srgb)[i * 4 + 3];
        }
        linear = std::make_shared<const std::vector<float>>(std::move(values));
        deep_pictures().put(key, linear, linear->size() * sizeof(float));
    }
    const Resample how = resample_of(interp_of(layer, contents));
    if (const std::optional<Placing> bend = placing_of(layer, contents)) {  // (PC3 A5: through its placing, its reach once, remembered)
        const Box reach = reached(page_pixels(*bend, dpi / 25.4, ctx.dpi), w, h, limit);
        const std::string laid_key = key + "\nbent " + bend->key + " " + core::dump_python(Json::array({ctx.dpi, reach.x0, reach.y0, reach.x1, reach.y1, static_cast<int>(how)}));
        std::shared_ptr<const std::vector<float>> laid = deep_pictures().get(laid_key);
        if (laid == nullptr) {
            detail::check_cancel(ctx);
            resample::Plane src(w, h, 4, true);
            src.hdr = true;
            src.v = *linear;
            laid = std::make_shared<const std::vector<float>>(bent_plane(src, *bend, dpi / 25.4, ctx.dpi, reach, how).v);
            deep_pictures().put(laid_key, laid, laid->size() * sizeof(float));
        }
        for (int y = std::max(part.y0, reach.y0); y < std::min(part.y1, reach.y1); ++y) {
            for (int x = std::max(part.x0, reach.x0); x < std::min(part.x1, reach.x1); ++x) {
                const float* v = laid->data() + (std::size_t(y - reach.y0) * reach.width() + (x - reach.x0)) * 4;
                out.pixels[std::size_t(y - area.y0) * area.width() + (x - area.x0)] = {v[0], v[1], v[2], v[3]};
            }
        }
    } else {
        const std::vector<double> laid = lay(*linear, w, h, 4, pm, part, how, &ctx);
        for (int y = part.y0; y < part.y1; ++y) {
            for (int x = part.x0; x < part.x1; ++x) {
                const double* v = laid.data() + (std::size_t(y - part.y0) * part.width() + (x - part.x0)) * 4;
                out.pixels[std::size_t(y - area.y0) * area.width() + (x - area.x0)] = {v[0], v[1], v[2], v[3]};
            }
        }
    }
    if (const auto shape = panel_shape(ctx, layer, area)) {
        for (int y = 0; y < area.height(); ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(shape->raw()->image[y]);
            for (int x = 0; x < area.width(); ++x) out.pixels[std::size_t(y) * area.width() + x][3] *= row[x] / 255.0;
        }
    }
    return out;
}

// --- a file or contents of inks, on a page of inks ------------------------------------------------------------------------

bool ink_file(const core::SmartContents& contents) {
    return contents.kind == "file" && (contents.mode.starts_with("cmyk") || contents.mode.starts_with("gray") || contents.mode.starts_with("lab"));
}

// Channel c of pixel `at` of ink pixels as the canvas holds it (c = colours(): its alpha).
double canvas_value(const core::InkPixels& inks, std::size_t at, unsigned c) {
    if (c == inks.colours()) return inks.has_alpha() ? inks.value(at, c) : 1.0;
    const unsigned in_all = inks.channels();
    const double ink = inks.is_float() ? inks.floats[at * in_all + c] : ink::decode_sample(inks.space, c, inks.samples[at * in_all + c], inks.depth);
    return to_canvas(inks.space, c, ink);
}

// Ink pixels laid into a picture of the canvas's values over `area`, at (x, y) on the page; what falls outside left.
void into_picture(InkPicture& out, const Box& area, const core::InkPixels& inks, int x, int y, const Box& limit) {
    const unsigned n = inks.colours(), all = n + 1;
    for (std::uint32_t j = 0; j < inks.height; ++j) {
        const int py = y + static_cast<int>(j);
        if (py < area.y0 || py >= area.y1 || py < limit.y0 || py >= limit.y1) continue;
        for (std::uint32_t i = 0; i < inks.width; ++i) {
            const int px = x + static_cast<int>(i);
            if (px < area.x0 || px >= area.x1 || px < limit.x0 || px >= limit.x1) continue;
            const std::size_t at = std::size_t(j) * inks.width + i;
            double* to = out.values.data() + (std::size_t(py - area.y0) * area.width() + (px - area.x0)) * all;
            for (unsigned c = 0; c <= n; ++c) to[c] = canvas_value(inks, at, c);
        }
    }
}

std::optional<InkPicture> file_inks_on_page(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m,
                                            const Box& area) {
    // (turned or bent: by the page's rules, as an 8-bit picture)
    if (!core::smart::axis_aligned(m) || core::smart::bent(layer)) return std::nullopt;
    const core::Document& doc = *ctx.episode;
    const core::PageColor& color = *ctx.page->color;
    const double dpi = contents.dpi > 0 ? contents.dpi : 72.0;
    // (its box on the page as a placed picture's)
    const auto snap = [](double v) { return std::round(v * 1e9) / 1e9; };
    const double x0 = core::py_round_whole(m.e / 25.4 * ctx.dpi), y0 = core::py_round_whole(m.f / 25.4 * ctx.dpi);
    const double x1 = core::py_round_whole((m.e + snap(m.a * contents.width_px * 25.4 / dpi)) / 25.4 * ctx.dpi);
    const double y1 = core::py_round_whole((m.f + snap(m.d * contents.height_px * 25.4 / dpi)) / 25.4 * ctx.dpi);
    if (x1 <= x0 || y1 <= y0 || (x1 - x0) * (y1 - y0) > 4.0 * ctx.size.width * ctx.size.height) return std::nullopt;
    const Box limit = clip_px(ctx, layer);
    InkPicture out{Size{area.width(), area.height()}, color.planes(),
                   std::vector<double>(std::size_t(area.width()) * area.height() * (color.planes() + 1), 0.0)};
    const Box part{std::max(area.x0, limit.x0), std::max(area.y0, limit.y0), std::min(area.x1, limit.x1), std::min(area.y1, limit.y1)};
    if (part.width() <= 0 || part.height() <= 0 || x1 <= part.x0 || y1 <= part.y0 || x0 >= part.x1 || y0 >= part.y1) return out;
    const Json* use = core::get(contents.profile, "use");
    const std::string way = use != nullptr && use->is_string() ? use->get<std::string>() : "keep";
    const std::string key = file_key(doc, contents) + "\ninks " + core::dump_canonical(core::to_json(color)) + " " +
                            core::dump_python(Json::array({x0, y0, x1, y1}));
    std::shared_ptr<const core::InkPixels> fitted = ink_pictures().get(key);
    if (fitted == nullptr) {
        const core::Bytes bytes = file_bytes(doc, contents);
        if (!bytes) return std::nullopt;
        ++g_files_decoded;
        const core::InkPixels inks = file_inks(*bytes, color, way);
        fitted = std::make_shared<const core::InkPixels>(
            resize_inks(inks, Size{static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)}, resample_of(interp_of(layer, contents)) == Resample::Bicubic
                                                                                               ? Resample::Bicubic
                                                                                               : Resample::Lanczos));
        ink_pictures().put(key, fitted, fitted->samples.size() * 2 + fitted->floats.size() * 4);
    }
    into_picture(out, area, *fitted, static_cast<int>(x0), static_cast<int>(y0), part);
    if (const auto shape = panel_shape(ctx, layer, area)) {
        const unsigned n = out.channels;
        for (int y = 0; y < area.height(); ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(shape->raw()->image[y]);
            for (int x = 0; x < area.width(); ++x) out.values[(std::size_t(y) * area.width() + x) * (n + 1) + n] *= row[x] / 255.0;
        }
    }
    return out;
}

// The part of `area` inside the smart object's panel's box (clip_px).
Box shown_part(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    const Box limit = clip_px(ctx, layer);
    return Box{std::max(area.x0, limit.x0), std::max(area.y0, limit.y0), std::min(area.x1, limit.x1), std::min(area.y1, limit.y1)};
}

// --- stack modes (PC3 A4, render/stack) --------------------------------------------------------------------------------

// The layers a stack mode combines, each as a page of the contents holding it alone: the contents' layers at the top that
// show — or, when the top is one folder, the layers in it —, each with the layers inside it, normal and not clipped
// (PhotoCraft's stack_image: a layer drawn alone).
std::vector<core::Page> stack_pages(const core::Page& contents) {
    std::set<std::string> ids;
    for (const core::Layer& l : contents.layers) ids.insert(l.id);
    const auto parent_of = [&ids](const core::Layer& l) {
        return l.parent_id.is_string() && ids.contains(l.parent_id.get<std::string>()) ? l.parent_id.get<std::string>() : std::string();
    };
    std::vector<const core::Layer*> tops;
    for (const core::Layer& l : contents.layers) {
        if (parent_of(l).empty()) tops.push_back(&l);
    }
    if (tops.size() == 1 && tops.front()->kind == core::LayerKind::Folder) {
        const std::string folder = tops.front()->id;
        tops.clear();
        for (const core::Layer& l : contents.layers) {
            if (parent_of(l) == folder) tops.push_back(&l);
        }
    }
    core::Page shell = contents;
    shell.layers.clear();
    std::vector<core::Page> out;
    for (const core::Layer* top : tops) {
        if (!top->visible) continue;
        std::set<std::string> with{top->id};
        for (bool grew = true; grew;) {
            grew = false;
            for (const core::Layer& l : contents.layers) {
                if (with.contains(l.id) || !with.contains(parent_of(l))) continue;
                with.insert(l.id);
                grew = true;
            }
        }
        core::Page page = shell;
        for (const core::Layer& l : contents.layers) {
            if (!with.contains(l.id)) continue;
            page.layers.push_back(l);
            if (&l != top) continue;
            core::Layer& alone = page.layers.back();
            alone.blend = "normal";
            alone.clip = false;
            alone.parent_id = nullptr;
        }
        out.push_back(std::move(page));
    }
    return out;
}

// The rows of a band drawn at once: the layers' pictures of it (`bytes` a pixel) within 256 MiB, at least 16 rows.
int band_rows(std::size_t layers, int width, std::size_t bytes, int height) {
    const std::size_t row = std::max<std::size_t>(1, layers * static_cast<std::size_t>(std::max(1, width)) * bytes);
    return std::clamp(static_cast<int>(std::min<std::size_t>((std::size_t(256) << 20) / row, 1u << 30)), std::min(16, std::max(1, height)),
                      std::max(1, height));
}

// The contents' layers stacked at `dpi` ("RGBA", the contents page's size, sRGB), remembered: drawn a band of rows at a
// time, so the layers' pictures held at once stay small (a page of inks: each whole, as the screen shows it).
std::shared_ptr<const Image> stacked_8bit(const Ctx& ctx, const core::SmartContents& contents, int dpi, stack::Stat stat, const std::string& key) {
    if (auto kept = pictures().get(key)) return kept;
    ++g_contents_drawn;
    const core::Page& inner = *contents.page;
    const Size size{to_px(inner.spec.width_mm.value(), dpi), to_px(inner.spec.height_mm.value(), dpi)};
    std::string bytes(std::size_t(size.width) * size.height * 4, '\0');
    const std::vector<core::Page> layers = stack_pages(inner);
    const std::size_t n = layers.size();
    if (n > 0) {
        const int rows = inner.color ? size.height : band_rows(n, size.width, 4, size.height);
        std::vector<double> values(n * 4), pixel(4), scratch;
        for (int y0 = 0; y0 < size.height; y0 += rows) {
            detail::check_cancel(ctx);
            const Box band{0, y0, size.width, std::min(size.height, y0 + rows)};
            std::vector<Image> parts;
            for (const core::Page& page : layers) {
                Image part;
                if (inner.color) {  // (a page of inks: as the screen shows them)
                    part = ink::to_rgba8(composite_ink_layers(page, page.layers, dpi, ctx.episode, false), *inner.color);
                } else {
                    RenderOptions o = contents_options(ctx);
                    o.region = RenderRegion{band.x0, band.y0, band.width(), band.height()};
                    RenderResult drawn = render_contents(page, dpi, o, ctx.episode);
                    left_out(ctx, drawn.omitted);
                    part = std::move(drawn.image);
                }
                parts.push_back(part.mode() == "RGBA" ? std::move(part) : part.convert("RGBA"));
            }
            for (int y = band.y0; y < band.y1; ++y) {
                std::vector<const unsigned char*> from;
                for (const Image& part : parts) from.push_back(reinterpret_cast<const unsigned char*>(part.raw()->image[y - band.y0]));
                for (int x = 0; x < size.width; ++x) {
                    for (std::size_t k = 0; k < n; ++k)
                        for (unsigned c = 0; c < 4; ++c) values[k * 4 + c] = from[k][x * 4 + c] / 255.0;
                    stack::combine_pixel(values.data(), n, 3, stat, pixel.data(), scratch);
                    char* to = bytes.data() + (std::size_t(y) * size.width + x) * 4;
                    for (unsigned c = 0; c < 4; ++c) to[c] = static_cast<char>(static_cast<unsigned char>(std::lround(pixel[c] * 255.0)));
                }
            }
        }
    }
    auto out = std::make_shared<const Image>(Image::frombytes("RGBA", size, bytes));
    pictures().put(key, out, image_bytes(*out));
    return out;
}

// A stacked contents page laid on the 8-bit page, as page_8bit lays one.
std::optional<Image> stacked_page_8bit(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m, stack::Stat stat,
                                       const Box& area) {
    const std::optional<Placing> bend = placing_of(layer, contents);  // (PC3 A5)
    const int dpi = bend ? bent_dpi(*bend, contents, ctx.dpi) : contents_dpi(m, ctx.dpi);
    const std::string key = contents_key(ctx, contents, dpi) + "\nstack " + stack_name(layer);
    const auto whole = [&] { return stacked_8bit(ctx, contents, dpi, stat, key); };
    if (const auto shift = bend ? std::nullopt : whole_shift(pixel_matrix(m, dpi / 25.4, ctx.dpi))) {  // (whole pixels moved: its own pixels)
        const Box part = shown_part(ctx, layer, area);
        if (part.width() <= 0 || part.height() <= 0) return std::nullopt;
        const std::shared_ptr<const Image> picture = whole();
        const Box from{std::max(0, part.x0 - shift->first), std::max(0, part.y0 - shift->second), std::min(picture->width(), part.x1 - shift->first),
                       std::min(picture->height(), part.y1 - shift->second)};
        Image out = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
        if (from.width() > 0 && from.height() > 0) out.paste(picture->crop(from), Point{from.x0 + shift->first - area.x0, from.y0 + shift->second - area.y0});
        return out;
    }
    const std::int64_t w = to_px(contents.page->spec.width_mm.value(), dpi), h = to_px(contents.page->spec.height_mm.value(), dpi);
    return lay_picture(ctx, layer, whole, w, h, dpi / 25.4, key, m, resample_of(interp_of(layer, contents)), false, area, bend);
}

// A stacked contents page on a page in precise colour: contents of the page's RGB that composite in precise colour,
// moved by whole pixels — each layer's values (one of 8 bits: its sRGB colours in the space), combined on their encoded
// samples as the 8-bit page combines its own —; nothing otherwise (the 8-bit stack is drawn).
std::optional<ColorPicture> stacked_precise(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m, stack::Stat stat,
                                            const Box& area, const RgbSpace& space) {
    const core::Page& inner = *contents.page;
    const bool print = ctx.mode == "print";
    if (inner.color || !(inner.rgb == ctx.page->rgb) || !(uses_color_precision(inner, print) || needs_precision(inner, ctx.episode, print)))
        return std::nullopt;
    if (core::smart::bent(layer)) return std::nullopt;  // (PC3 A5: bent, its 8-bit stack laid)
    const auto shift = whole_shift(pixel_matrix(m, ctx.dpi / 25.4, ctx.dpi));
    if (!shift) return std::nullopt;
    const std::string key = "stack " + stack_name(layer) + "\n" + contents_key(ctx, contents, ctx.dpi) + "\n" +
                            (ctx.page->rgb ? core::dump_canonical(core::to_json(*ctx.page->rgb)) : std::string("srgb"));
    std::shared_ptr<const ColorPicture> whole = precise_filtered().get(key);
    if (whole == nullptr) {
        ++g_contents_drawn;
        const Size size{to_px(inner.spec.width_mm.value(), ctx.dpi), to_px(inner.spec.height_mm.value(), ctx.dpi)};
        ColorPicture made{size, std::vector<std::array<double, 4>>(std::size_t(size.width) * size.height, {0, 0, 0, 0})};
        const std::vector<core::Page> layers = stack_pages(inner);
        const std::size_t n = layers.size();
        if (n > 0) {
            const int rows = band_rows(n, size.width, sizeof(std::array<double, 4>), size.height);
            std::vector<double> values(n * 4), pixel(4), scratch;
            for (int y0 = 0; y0 < size.height; y0 += rows) {
                detail::check_cancel(ctx);
                const Box band{0, y0, size.width, std::min(size.height, y0 + rows)};
                std::vector<ColorPicture> parts;
                for (const core::Page& page : layers) {
                    RenderOptions o = contents_options(ctx);
                    o.precise = true;
                    o.region = RenderRegion{band.x0, band.y0, band.width(), band.height()};
                    RenderResult drawn = render_contents(page, ctx.dpi, o, ctx.episode);
                    left_out(ctx, drawn.omitted);
                    if (drawn.precise) {
                        const core::ColorRasterView view(*drawn.precise);
                        parts.push_back(color_picture(view, Size{band.width(), band.height()}, Box{0, 0, band.width(), band.height()}, space));
                    } else {
                        parts.push_back(color_picture(drawn.image.mode() == "RGBA" ? drawn.image : drawn.image.convert("RGBA"), space));
                    }
                }
                for (int y = band.y0; y < band.y1; ++y) {
                    for (int x = 0; x < size.width; ++x) {
                        const std::size_t at = std::size_t(y - band.y0) * band.width() + x;
                        for (std::size_t k = 0; k < n; ++k) {
                            const std::array<double, 4>& p = parts[k].pixels[at];
                            for (unsigned c = 0; c < 3; ++c) values[k * 4 + c] = space.encode(c, p[c]);
                            values[k * 4 + 3] = p[3];
                        }
                        stack::combine_pixel(values.data(), n, 3, stat, pixel.data(), scratch);
                        made.pixels[std::size_t(y) * size.width + x] = {space.decode(0, pixel[0]), space.decode(1, pixel[1]), space.decode(2, pixel[2]),
                                                                        pixel[3]};
                    }
                }
            }
        }
        whole = std::make_shared<const ColorPicture>(std::move(made));
        precise_filtered().put(key, whole, whole->pixels.size() * sizeof(whole->pixels[0]));
    }
    ColorPicture out{Size{area.width(), area.height()}, std::vector<std::array<double, 4>>(std::size_t(area.width()) * area.height(), {0, 0, 0, 0})};
    const Box part = shown_part(ctx, layer, area);
    for (int y = part.y0; y < part.y1; ++y) {
        const int sy = y - shift->second;
        if (sy < 0 || sy >= whole->size.height) continue;
        for (int x = part.x0; x < part.x1; ++x) {
            const int sx = x - shift->first;
            if (sx < 0 || sx >= whole->size.width) continue;
            out.pixels[std::size_t(y - area.y0) * area.width() + (x - area.x0)] = whole->pixels[std::size_t(sy) * whole->size.width + sx];
        }
    }
    return out;
}

// A stacked contents page on a page of inks: contents of the page's colour mode moved by whole pixels — each layer's
// inks, combined plate by plate on the canvas's values —; nothing otherwise (the 8-bit stack is drawn by the page's rules).
std::optional<InkPicture> stacked_inks(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Matrix& m, stack::Stat stat,
                                       const Box& area) {
    const core::Page& inner = *contents.page;
    if (!inner.color || !(*inner.color == *ctx.page->color) || core::smart::bent(layer)) return std::nullopt;
    const auto shift = whole_shift(pixel_matrix(m, ctx.dpi / 25.4, ctx.dpi));
    if (!shift) return std::nullopt;
    const unsigned n = ctx.page->color->planes();
    const std::string key = "stack " + stack_name(layer) + "\ninks " + contents_key(ctx, contents, ctx.dpi);
    std::shared_ptr<const InkPicture> whole = inks_filtered().get(key);
    if (whole == nullptr) {
        ++g_contents_drawn;
        const Size size{to_px(inner.spec.width_mm.value(), ctx.dpi), to_px(inner.spec.height_mm.value(), ctx.dpi)};
        InkPicture made{size, n, std::vector<double>(std::size_t(size.width) * size.height * (n + 1), 0.0)};
        std::vector<core::InkPixels> parts;
        for (const core::Page& page : stack_pages(inner)) {
            detail::check_cancel(ctx);
            parts.push_back(composite_ink_layers(page, page.layers, ctx.dpi, ctx.episode, false));
            if (parts.back().width != static_cast<std::uint32_t>(size.width) || parts.back().height != static_cast<std::uint32_t>(size.height) ||
                parts.back().colours() != n)
                throw core::Error("value", "a stack's layer does not match its contents");
        }
        // (a page of ink plates — CMYK, multichannel — on its inverted plates, as its blend modes: the maximum the lightest,
        // as on an RGB page; Lab and grey on their values, lightness the larger)
        const core::InkSpace space = ctx.page->color->space;
        const bool inverted = space == core::InkSpace::Cmyk || space == core::InkSpace::Multi;
        const std::size_t k_n = parts.size();
        if (k_n > 0) {
            std::vector<double> values(k_n * (n + 1)), scratch;
            for (std::size_t at = 0; at < std::size_t(size.width) * size.height; ++at) {
                for (std::size_t k = 0; k < k_n; ++k) {
                    for (unsigned c = 0; c <= n; ++c) {
                        const double v = canvas_value(parts[k], at, c);
                        values[k * (n + 1) + c] = inverted && c < n ? 1.0 - v : v;
                    }
                }
                double* out = made.values.data() + at * (n + 1);
                stack::combine_pixel(values.data(), k_n, n, stat, out, scratch);
                if (inverted && out[n] > 0) {
                    for (unsigned c = 0; c < n; ++c) out[c] = 1.0 - out[c];
                }
            }
        }
        whole = std::make_shared<const InkPicture>(std::move(made));
        inks_filtered().put(key, whole, whole->values.size() * sizeof(double));
    }
    InkPicture out{Size{area.width(), area.height()}, n, std::vector<double>(std::size_t(area.width()) * area.height() * (n + 1), 0.0)};
    const Box part = shown_part(ctx, layer, area);
    for (int y = part.y0; y < part.y1; ++y) {
        const int sy = y - shift->second;
        if (sy < 0 || sy >= whole->size.height) continue;
        for (int x = part.x0; x < part.x1; ++x) {
            const int sx = x - shift->first;
            if (sx < 0 || sx >= whole->size.width) continue;
            std::copy_n(whole->values.data() + (std::size_t(sy) * whole->size.width + sx) * (n + 1), n + 1,
                        out.values.data() + (std::size_t(y - area.y0) * area.width() + (x - area.x0)) * (n + 1));
        }
    }
    return out;
}

// --- smart filters (PC3 A2, render/smart_filters) ---------------------------------------------------------------------

// The smart object as its filters take it: without them and their mask, and not cut to its panel (they work its picture
// of the whole page; the panel cuts what they make, as it cuts a layer).
core::Layer unfiltered(const core::Layer& layer) {
    core::Layer out = layer;
    for (const char* key : {"filters", "filters_on", "filter_mask"}) out.smart->erase(key);
    out.clip_to = "none";
    return out;
}

// What a smart object's filtered picture is made of: its contents as they are, its settings (its filters and their mask
// among them), what draws a picture's finish (its panel, the studio's style), the page's paper and colours, the
// resolution and the drawing's way.
std::string filtered_key(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, std::string_view way) {
    const core::Frame* frame = layer.frame_id && !layer.frame_id->empty() ? ctx.page->find_frame(*layer.frame_id) : nullptr;
    const core::Page& page = *ctx.page;
    const Json bits = Json::array(
        {std::string(way), identity(*ctx.episode, contents), core::dump_canonical(*layer.smart), ctx.dpi, ctx.size.width, ctx.size.height, ctx.mode,
         ctx.finish, ctx.rough, ctx.dots, ctx.screen_dots, layer.frame_id ? *layer.frame_id : std::string(), std::string(core::to_string(layer.role)),
         layer.source ? *layer.source : Json(), layer.finish ? *layer.finish : Json(), core::py_get(ctx.episode->studio, "style"),
         frame != nullptr && frame->panel ? *frame->panel : Json(), ctx.episode->bible.characters, page.spec.width_mm.json(), page.spec.height_mm.json(),
         page.spec.expression, page.rgb ? core::to_json(*page.rgb) : Json(), page.color ? core::to_json(*page.color) : Json(),
         ctx.episode->asset_dir ? core::path_to_utf8(*ctx.episode->asset_dir) : std::string()});
    return "filtered " + core::dump_canonical(bits);
}

// Settings a filter cannot use (a book changed by hand): the smart object left out and said, as what is not drawn yet.
template <class Work>
auto filtering(const Work& work) {
    try {
        return work();
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::OpError&) {
        throw NotYetPorted("smart:filters");
    } catch (const core::Error& error) {
        if (error.code() == "value" || error.code() == "type" || error.code() == "not_yet_ported") throw NotYetPorted("smart:filters");
        throw;
    }
}

Box whole_page(const Ctx& ctx) { return Box{0, 0, ctx.size.width, ctx.size.height}; }

std::optional<Image> filtered_8bit(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Box& area) {
    const std::string key = filtered_key(ctx, layer, contents, "8");
    std::shared_ptr<const Image> whole = pictures().get(key);
    if (whole == nullptr) {
        std::optional<Image> base = picture_8bit(ctx, unfiltered(layer), whole_page(ctx));
        if (!base) base = Image::create("RGBA", ctx.size, Ink{0, 0, 0, 0});
        whole = std::make_shared<const Image>(filtering([&] { return smart_filters::on_8bit(*ctx.episode, *ctx.page, layer, *base, ctx.dpi); }));
        pictures().put(key, whole, image_bytes(*whole));
    }
    Image out = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    const Box part = shown_part(ctx, layer, area);
    if (part.width() > 0 && part.height() > 0) out.paste(whole->crop(part), Point{part.x0 - area.x0, part.y0 - area.y0});
    if (const auto shape = panel_shape(ctx, layer, area)) out.putalpha(chops::multiply(out.getchannel(3), *shape));
    return out;
}

std::optional<ColorPicture> filtered_precise(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Box& area,
                                             const RgbSpace& space) {
    const std::string key = filtered_key(ctx, layer, contents, "precise");
    std::shared_ptr<const ColorPicture> whole = precise_filtered().get(key);
    if (whole == nullptr) {
        const core::Layer bare = unfiltered(layer);
        std::optional<ColorPicture> base = picture_precise(ctx, bare, whole_page(ctx), space);
        if (!base) {  // (a picture of 8 bits: its sRGB colours in the space)
            const std::optional<Image> eight = picture_8bit(ctx, bare, whole_page(ctx));
            base = color_picture(eight ? *eight : Image::create("RGBA", ctx.size, Ink{0, 0, 0, 0}), space);
        }
        whole = std::make_shared<const ColorPicture>(
            filtering([&] { return smart_filters::on_precise(*ctx.episode, *ctx.page, layer, *base, space, ctx.dpi); }));
        precise_filtered().put(key, whole, whole->pixels.size() * sizeof(whole->pixels[0]));
    }
    ColorPicture out{Size{area.width(), area.height()}, std::vector<std::array<double, 4>>(std::size_t(area.width()) * area.height(), {0, 0, 0, 0})};
    const Box part = shown_part(ctx, layer, area);
    for (int y = part.y0; y < part.y1; ++y) {
        for (int x = part.x0; x < part.x1; ++x)
            out.pixels[std::size_t(y - area.y0) * area.width() + (x - area.x0)] = whole->pixels[std::size_t(y) * ctx.size.width + x];
    }
    if (const auto shape = panel_shape(ctx, layer, area)) {
        for (int y = 0; y < area.height(); ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(shape->raw()->image[y]);
            for (int x = 0; x < area.width(); ++x) out.pixels[std::size_t(y) * area.width() + x][3] *= row[x] / 255.0;
        }
    }
    return out;
}

std::optional<InkPicture> filtered_inks(const Ctx& ctx, const core::Layer& layer, const core::SmartContents& contents, const Box& area) {
    const core::PageColor& color = *ctx.page->color;
    const std::string key = filtered_key(ctx, layer, contents, "inks");
    std::shared_ptr<const InkPicture> whole = inks_filtered().get(key);
    if (whole == nullptr) {
        const core::Layer bare = unfiltered(layer);
        std::optional<InkPicture> base = picture_inks(ctx, bare, whole_page(ctx));
        if (!base) {  // (a picture of 8 bits: its colours as the page's inks, by its rules)
            const std::optional<Image> eight = picture_8bit(ctx, bare, whole_page(ctx));
            base = ink_picture(eight ? *eight : Image::create("RGBA", ctx.size, Ink{0, 0, 0, 0}), color);
        }
        whole = std::make_shared<const InkPicture>(filtering([&] { return smart_filters::on_inks(*ctx.episode, *ctx.page, layer, *base, ctx.dpi); }));
        inks_filtered().put(key, whole, whole->values.size() * sizeof(double));
    }
    const unsigned n = whole->channels;
    InkPicture out{Size{area.width(), area.height()}, n, std::vector<double>(std::size_t(area.width()) * area.height() * (n + 1), 0.0)};
    const Box part = shown_part(ctx, layer, area);
    for (int y = part.y0; y < part.y1; ++y) {
        for (int x = part.x0; x < part.x1; ++x)
            std::copy_n(whole->values.data() + (std::size_t(y) * ctx.size.width + x) * (n + 1), n + 1,
                        out.values.data() + (std::size_t(y - area.y0) * area.width() + (x - area.x0)) * (n + 1));
    }
    if (const auto shape = panel_shape(ctx, layer, area)) {
        for (int y = 0; y < area.height(); ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(shape->raw()->image[y]);
            for (int x = 0; x < area.width(); ++x) out.values[(std::size_t(y) * area.width() + x) * (n + 1) + n] *= row[x] / 255.0;
        }
    }
    return out;
}

}  // namespace

core::Bytes file_bytes(const core::Document& doc, const core::SmartContents& contents) {
    if (const auto held = doc.smart_files.find(contents.asset); held != doc.smart_files.end() && held->second) return held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(contents.asset) || !storage::AssetStore::is_suffix(contents.suffix)) return nullptr;
    auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(contents.asset, contents.suffix, storage::kAssetMaxBytes);
    if (!bytes) return nullptr;
    return std::make_shared<const std::string>(std::move(*bytes));
}

std::shared_ptr<const Image> file_picture(const core::Document& doc, const core::SmartContents& contents) {
    const std::string key = file_key(doc, contents);
    if (auto kept = pictures().get(key)) return kept;
    const core::Bytes bytes = file_bytes(doc, contents);
    if (!bytes) return nullptr;
    ++g_files_decoded;
    Image picture = eight_bit_picture(*bytes);
    if (const auto icc = use_profile(doc, contents, *bytes); icc && !icc->empty()) {
        // (taken from that profile to sRGB, as the colour settings take a picture in: a profile of other colours is none)
        const std::string_view mode = picture.mode();
        const core::ProfileSpace wanted = mode == "CMYK" ? core::ProfileSpace::Cmyk
                                          : (mode == "L" || mode == "LA" || mode == "1" || mode.starts_with("I")) ? core::ProfileSpace::Gray
                                                                                                                   : core::ProfileSpace::Rgb;
        if (ink::profile_space(*icc) == wanted) picture = proof::convert_picture(picture, *icc, intent_of(contents), bpc_of(contents));
    }
    // (as the 8-bit picture a placed picture was made of: RGB(A) as the picture held it, then as the drawing reads it)
    const std::string_view mode = picture.mode();
    picture = picture.convert(mode == "RGBA" || mode == "LA" || mode == "P" ? "RGBA" : "RGB").convert("RGBA");
    auto out = std::make_shared<const Image>(std::move(picture));
    pictures().put(key, out, image_bytes(*out));
    return out;
}

std::optional<Image> picture_8bit(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    if (ctx.episode == nullptr) return std::nullopt;
    const core::SmartContents* contents = core::smart::contents_of(*ctx.episode, layer);
    if (contents == nullptr) return std::nullopt;
    if (auto shown = shown_8bit(ctx, layer, area)) return std::move(*shown);  // (PC3 A3: a PSD's picture, until it changes)
    if (core::smart::filters_drawn(layer)) return filtered_8bit(ctx, layer, *contents, area);  // (PC3 A2)
    const Matrix m = core::smart::matrix_of(layer);
    const std::optional<stack::Stat> stat = stack_of(layer);  // (PC3 A4)
    if (contents->kind == "file") {
        // (stacked, a file is one layer: its one sample as it is, or what the statistic makes of any one sample)
        const std::optional<double> one = stat ? stack::of_one(*stat) : std::nullopt;
        return one ? file_8bit(ctx, layer, *contents, m, area, one, stack_name(layer)) : file_8bit(ctx, layer, *contents, m, area);
    }
    if (!contents->page) return std::nullopt;
    if (stat) return stacked_page_8bit(ctx, layer, *contents, m, *stat, area);
    return page_8bit(ctx, layer, *contents, m, area);
}

std::optional<ColorPicture> picture_precise(const Ctx& ctx, const core::Layer& layer, const Box& area, const RgbSpace& space) {
    if (ctx.episode == nullptr) return std::nullopt;
    const core::SmartContents* contents = core::smart::contents_of(*ctx.episode, layer);
    if (contents == nullptr) return std::nullopt;
    if (shown_ref(layer, "asset")) return std::nullopt;  // (PC3 A3: a PSD's picture, of 8 bits)
    if (core::smart::filters_drawn(layer)) return filtered_precise(ctx, layer, *contents, area, space);  // (PC3 A2)
    const Matrix m = core::smart::matrix_of(layer);
    if (const std::optional<stack::Stat> stat = stack_of(layer)) {  // (PC3 A4)
        if (contents->kind == "page" && contents->page) return stacked_precise(ctx, layer, *contents, m, *stat, area, space);
        if (stack::of_one(*stat)) return std::nullopt;  // (a file stacked to one value: its 8-bit picture)
    }
    if (deep_file(*contents)) {
        // (a mono page's finish is laid on an 8-bit picture: as a placed picture's)
        if (ctx.finish && (ctx.mode == "print" || ctx.mode == "proof") && ctx.page->spec.expression != "color" && layer.role != core::LayerRole::Draft &&
            layer.role != core::LayerRole::Name)
            return std::nullopt;
        return file_precise(ctx, layer, *contents, m, area, space);
    }
    if (contents->kind != "page" || !contents->page || contents->page->color || !(contents->page->rgb == ctx.page->rgb)) return std::nullopt;
    if (core::smart::bent(layer)) return std::nullopt;  // (PC3 A5: bent contents, laid in 8 bits)
    const Matrix pm = pixel_matrix(m, ctx.dpi / 25.4, ctx.dpi);
    const auto shift = whole_shift(pm);
    if (!shift) return std::nullopt;
    // (contents in precise colour moved by whole pixels: their values, part by part)
    const Box limit = clip_px(ctx, layer);
    const Box part{std::max(area.x0, limit.x0), std::max(area.y0, limit.y0), std::min(area.x1, limit.x1), std::min(area.y1, limit.y1)};
    ColorPicture out{Size{area.width(), area.height()}, std::vector<std::array<double, 4>>(std::size_t(area.width()) * area.height(), {0, 0, 0, 0})};
    const Size size{to_px(contents->page->spec.width_mm.value(), ctx.dpi), to_px(contents->page->spec.height_mm.value(), ctx.dpi)};
    const Box from{std::max(0, part.x0 - shift->first), std::max(0, part.y0 - shift->second), std::min(size.width, part.x1 - shift->first),
                   std::min(size.height, part.y1 - shift->second)};
    if (from.width() <= 0 || from.height() <= 0) return out;
    RenderOptions o = contents_options(ctx);
    o.precise = true;
    o.region = RenderRegion{from.x0, from.y0, from.width(), from.height()};
    RenderResult drawn = render_contents(*contents->page, ctx.dpi, o, ctx.episode);
    left_out(ctx, drawn.omitted);
    if (!drawn.precise) return std::nullopt;  // (contents that do not composite in precise colour: 8 bits)
    const core::ColorRasterView view(*drawn.precise);
    const ColorPicture values = color_picture(view, Size{from.width(), from.height()}, Box{0, 0, from.width(), from.height()}, space);
    for (int y = 0; y < from.height(); ++y) {
        for (int x = 0; x < from.width(); ++x) {
            const int px = from.x0 + x + shift->first - area.x0, py = from.y0 + y + shift->second - area.y0;
            out.pixels[std::size_t(py) * area.width() + px] = values.pixels[std::size_t(y) * from.width() + x];
        }
    }
    return out;
}

std::optional<InkPicture> picture_inks(const Ctx& ctx, const core::Layer& layer, const Box& area) {
    if (ctx.episode == nullptr || !ctx.page->color) return std::nullopt;
    const core::SmartContents* contents = core::smart::contents_of(*ctx.episode, layer);
    if (contents == nullptr) return std::nullopt;
    if (auto shown = shown_inks(ctx, layer, area)) return shown;  // (PC3 A3: a PSD's picture, until it changes)
    if (shown_ref(layer, "asset")) return std::nullopt;  // (its PNG: as an 8-bit picture, by the page's rules)
    if (core::smart::filters_drawn(layer)) return filtered_inks(ctx, layer, *contents, area);  // (PC3 A2)
    const Matrix m = core::smart::matrix_of(layer);
    if (const std::optional<stack::Stat> stat = stack_of(layer)) {  // (PC3 A4)
        if (contents->kind == "page" && contents->page) return stacked_inks(ctx, layer, *contents, m, *stat, area);
        if (stack::of_one(*stat)) return std::nullopt;  // (a file stacked to one value: its 8-bit picture, by the page's rules)
    }
    if (ink_file(*contents)) return file_inks_on_page(ctx, layer, *contents, m, area);
    if (contents->kind != "page" || !contents->page || !contents->page->color || !(*contents->page->color == *ctx.page->color)) return std::nullopt;
    if (core::smart::bent(layer)) return std::nullopt;  // (PC3 A5: bent contents, laid in 8 bits by the page's rules)
    const auto shift = whole_shift(pixel_matrix(m, ctx.dpi / 25.4, ctx.dpi));
    if (!shift) return std::nullopt;
    // (contents of the page's inks moved by whole pixels: their inks, composited whole once)
    const std::string key = "inks " + contents_key(ctx, *contents, ctx.dpi);
    std::shared_ptr<const core::InkPixels> inks = ink_pictures().get(key);
    if (inks == nullptr) {
        ++g_contents_drawn;
        inks = std::make_shared<const core::InkPixels>(composite_ink_layers(*contents->page, contents->page->layers, ctx.dpi, ctx.episode, false));
        ink_pictures().put(key, inks, inks->samples.size() * 2 + inks->floats.size() * 4);
    }
    const unsigned n = ctx.page->color->planes();
    InkPicture out{Size{area.width(), area.height()}, n, std::vector<double>(std::size_t(area.width()) * area.height() * (n + 1), 0.0)};
    into_picture(out, area, *inks, shift->first, shift->second, clip_px(ctx, layer));
    return out;
}

bool needs_precision(const core::Page& page, const core::Document* doc, bool print) {
    if (doc == nullptr || page.color || !page.rgb || page.rgb->depth <= 8) return false;
    for (const core::Layer& layer : page.layers) {
        if (layer.kind != core::LayerKind::Smart || !layer.visible) continue;
        if (print && (!layer.exportable || layer.role == core::LayerRole::Name || layer.role == core::LayerRole::Draft)) continue;
        const core::SmartContents* contents = core::smart::contents_of(*doc, layer);
        if (contents == nullptr) continue;
        if (deep_file(*contents)) return true;
        if (contents->kind == "page" && contents->page && !contents->page->color && contents->page->rgb == page.rgb &&
            (uses_color_precision(*contents->page, print) || needs_precision(*contents->page, doc, print)))
            return true;
    }
    return false;
}

void clear_caches() {
    pictures().clear();
    deep_pictures().clear();
    ink_pictures().clear();
    precise_filtered().clear();
    inks_filtered().clear();
}

std::int64_t cache_pixels() {
    return static_cast<std::int64_t>(pictures().bytes() / 4 + deep_pictures().bytes() / 16 + ink_pictures().bytes() / 10 + precise_filtered().bytes() / 32 +
                                     inks_filtered().bytes() / 40);
}

Counts counts() { return Counts{g_contents_drawn.load(), g_files_decoded.load()}; }

}  // namespace genko::render::smart
