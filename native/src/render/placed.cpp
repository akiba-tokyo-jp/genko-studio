#include "render/placed.hpp"

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/draw.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/screentone.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

using core::Json;

std::optional<std::pair<std::int64_t, std::int64_t>> asset_picture_size(const std::filesystem::path& book, const std::string& ref) {
    if (!storage::AssetStore::is_ref(ref)) return std::nullopt;  // (Python looks for assets/<ab>/<the rest>.png: not there)
    std::optional<std::string> bytes;
    try {
        bytes = storage::AssetStore(book).get_bytes(ref, ".png", storage::kAssetMaxBytes);
    } catch (const core::Error& error) {
        throw core::OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
    if (!bytes) return std::nullopt;
    try {
        const Size size = picture_size(*bytes);
        return std::make_pair(std::int64_t{size.width}, std::int64_t{size.height});
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "unidentified_image") {
            throw core::PyUncaught("UnidentifiedImageError", "cannot identify image file <_io.BytesIO object>");
        }
        if (error.code() == "image_too_large") throw core::PyUncaught("DecompressionBombError", error.what());
        throw core::OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
}

namespace {

std::mutex g_vouched_mutex;
std::list<std::string> g_vouched;  // (the latest first)
constexpr std::size_t kVouched = 64;
std::atomic<std::uint64_t> g_decoded{0};
std::atomic<std::uint64_t> g_let_through{0};
std::atomic<std::uint64_t> g_drawn_decoded{0};

bool vouched(const std::string& ref) {
    std::lock_guard lock(g_vouched_mutex);
    return std::find(g_vouched.begin(), g_vouched.end(), ref) != g_vouched.end();
}

}  // namespace

void vouch_for_picture(const std::filesystem::path& book, const std::string& ref) {
    (void)book;  // (the ref alone: the check reads the file's bytes and their hash wherever it is)
    if (!storage::AssetStore::is_ref(ref)) return;
    std::lock_guard lock(g_vouched_mutex);
    g_vouched.remove(ref);
    g_vouched.push_front(ref);
    while (g_vouched.size() > kVouched) g_vouched.pop_back();
}

PictureChecks picture_checks() { return PictureChecks{g_decoded.load(), g_let_through.load(), g_drawn_decoded.load()}; }

void check_asset_picture(const std::filesystem::path& book, const std::string& ref) {
    if (!storage::AssetStore::is_ref(ref)) return;
    try {
        const auto bytes = storage::AssetStore(book).get_bytes(ref, ".png", storage::kAssetMaxBytes);
        if (!bytes) return;
        // (written just now by this process from pixels it held — vouch_for_picture —, and the file holds those very
        // bytes: nothing to find by decoding it)
        if (vouched(ref) && storage::AssetStore::ref(*bytes) == ref) {
            ++g_let_through;
            return;
        }
        ++g_decoded;
        (void)open_image(*bytes, kPillowOpenLimits);
    } catch (const NotYetPorted&) {
        return;  // (not opened by this build yet: the drawing says so)
    } catch (const core::Error& error) {
        throw core::OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
}

namespace {

// A few pictures remembered by a key (a page is drawn in tiles, each asking for the same picture): the decoded assets
// (a ref is its bytes' hash: never another picture under it) and the finished art (its key holds all it is made from).
class Recent {
public:
    explicit Recent(std::size_t capacity) : capacity_(capacity) {}

    std::shared_ptr<const Image> get(const std::string& key) {
        std::lock_guard lock(mutex_);
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == key) {
                items_.splice(items_.begin(), items_, it);
                return items_.front().second;
            }
        }
        return nullptr;
    }

    // (a key there already: its picture replaced, not kept twice)
    void put(const std::string& key, std::shared_ptr<const Image> image) {
        std::lock_guard lock(mutex_);
        items_.remove_if([&key](const auto& item) { return item.first == key; });
        items_.emplace_front(key, std::move(image));
        while (items_.size() > capacity_) items_.pop_back();
    }

    std::size_t size() {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        items_.clear();
    }

    std::int64_t pixels() {
        std::lock_guard lock(mutex_);
        std::int64_t out = 0;
        for (const auto& [key, image] : items_) out += static_cast<std::int64_t>(image->width()) * image->height();
        return out;
    }

private:
    std::mutex mutex_;
    std::list<std::pair<std::string, std::shared_ptr<const Image>>> items_;
    std::size_t capacity_;
};

Recent& sources() {
    static Recent recent(2);
    return recent;
}

Recent& finished() {
    static Recent recent(2);
    return recent;
}

// The most pixels of finished art kept for the next tiles (a whole page at 600 dpi is about 35 million).
constexpr std::int64_t kKeptFinishPixels = 40'000'000;

Box meet(const Box& a, const Box& b) {
    return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}
bool empty_box(const Box& b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

// `x / 25.4` (in mm_to_px) of a value as Python takes it.
double divided(const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_number()) return v.get<double>();
    throw core::PyTypeError("unsupported operand type(s) for /: '" + core::py_type_name(v) + "' and 'float'");
}
// `x * k` of a value as Python takes it.
double times(const Json& v, double k) {
    if (v.is_boolean() || v.is_number()) return divided(v) * k;
    if (v.is_string() || v.is_array()) throw core::PyTypeError("can't multiply sequence by non-int of type 'float'");
    throw core::PyTypeError("unsupported operand type(s) for *: '" + core::py_type_name(v) + "' and 'float'");
}
// `x - d` of a value as Python takes it.
double minus(const Json& v, double d) {
    if (v.is_boolean() || v.is_number()) return divided(v) - d;
    throw core::PyTypeError("unsupported operand type(s) for -: '" + core::py_type_name(v) + "' and 'float'");
}

// `page._find(layer.frame_id).panel or {}`; nothing when the layer has no panel or the page has no such frame.
std::optional<Json> panel_of_layer(const core::Layer& layer, const core::Page& page) {
    if (!layer.frame_id || layer.frame_id->empty()) return std::nullopt;
    const core::Frame* frame = page.find_frame(*layer.frame_id);
    if (frame == nullptr) return std::nullopt;
    return frame->panel ? core::py_or(*frame->panel, Json::object()) : Json::object();
}

// panel.get("regions", []), each region (an AttributeError for one that is not a dict, when it is looked at).
std::vector<Json> regions_of(const Json& panel) { return core::iterate(core::py_get(panel, "regions", Json::array())); }

// isinstance(r.get("rect_mm"), (list, tuple)) and len(r["rect_mm"]) == 4
bool rect4(const Json& region) {
    const Json rect = core::py_get(region, "rect_mm");
    return rect.is_array() && rect.size() == 4;
}

// r.get("kind") in (…)
bool kind_in(const Json& region, std::initializer_list<const char*> kinds) {
    const Json kind = core::py_get(region, "kind");
    return std::any_of(kinds.begin(), kinds.end(), [&kind](const char* k) { return core::py_equals(kind, Json(k)); });
}

// render.mm_to_px as Python's int: max(1, round(mm / 25.4 * dpi)), unbounded (a whole float; round()'s OverflowError
// for an infinity). A mask's corners are drawn from these as Pillow takes Python's ints (its own (int) cast of each).
double px_whole(double mm, int dpi) { return core::py_max(1.0, core::py_round_whole(mm / 25.4 * dpi)); }

// render._face_mask: white ellipses, softened at the edge, where the panel's reported faces are (in the art's pixels).
std::optional<Image> face_mask(const core::Layer& layer, const core::Page& page, Size size, int dpi, Point origin) {
    const auto panel = panel_of_layer(layer, page);
    if (!panel) return std::nullopt;
    std::vector<Json> faces;
    for (const Json& r : regions_of(*panel)) {
        if (kind_in(r, {"face", "head"}) && rect4(r)) faces.push_back(r["rect_mm"]);
    }
    if (faces.empty()) return std::nullopt;
    Image mask = Image::create("L", size, Ink(0));
    {
        Draw draw(mask);
        for (const Json& rect : faces) {
            const double x0 = px_whole(divided(rect[0]), dpi) - origin.x;
            const double y0 = px_whole(divided(rect[1]), dpi) - origin.y;
            draw.ellipse(BoxF{x0, y0, x0 + px_whole(divided(rect[2]), dpi), y0 + px_whole(divided(rect[3]), dpi)}, Ink(255));
        }
    }
    return mask.filter(Filter::gaussian_blur(core::py_max(1.0, dpi / 100.0)));
}

// render._clothes_masks: (solid black, white): the reported people whose bible says their clothes print that way, less
// their faces (and the hair above them).
std::pair<std::optional<Image>, std::optional<Image>> clothes_masks(const core::Layer& layer, const core::Page& page,
                                                                    const core::Document* episode, Size size, int dpi,
                                                                    Point origin) {
    if (episode == nullptr || !layer.frame_id || layer.frame_id->empty()) return {};
    std::vector<std::pair<Json, Json>> values;  // {c.get("id"): (c.get("look") or {}).get("clothes_value")}
    for (const Json& c : core::iterate(episode->bible.characters)) {
        const Json id = core::py_get(c, "id");
        const Json value = core::py_get(core::py_or(core::py_get(c, "look"), Json::object()), "clothes_value");
        core::require_hashable(id);
        const auto same = std::find_if(values.begin(), values.end(), [&id](const auto& v) { return core::py_equals(v.first, id); });
        if (same != values.end()) {
            same->second = value;
        } else {
            values.emplace_back(id, value);
        }
    }
    const auto prints = [](const Json& v) { return core::py_equals(v, Json("beta")) || core::py_equals(v, Json("white")); };
    if (!std::any_of(values.begin(), values.end(), [&](const auto& v) { return prints(v.second); })) return {};
    const auto panel = panel_of_layer(layer, page);
    if (!panel) return {};
    std::vector<Json> regions;
    for (const Json& r : regions_of(*panel)) {
        if (rect4(r)) regions.push_back(r);
    }
    std::pair<std::optional<Image>, std::optional<Image>> out;
    for (const char* value : {"beta", "white"}) {
        std::vector<Json> bodies;
        for (const Json& r : regions) {
            if (!kind_in(r, {"person", "body"})) continue;
            const Json who = core::py_get(r, "char");
            core::require_hashable(who);
            const auto found = std::find_if(values.begin(), values.end(), [&who](const auto& v) { return core::py_equals(v.first, who); });
            if (found != values.end() && core::py_equals(found->second, Json(value))) bodies.push_back(r["rect_mm"]);
        }
        if (bodies.empty()) continue;
        Image mask = Image::create("L", size, Ink(0));
        {
            Draw draw(mask);
            for (const Json& rect : bodies) {
                const double x0 = px_whole(divided(rect[0]), dpi) - origin.x;
                const double y0 = px_whole(divided(rect[1]), dpi) - origin.y;
                draw.rectangle(BoxF{x0, y0, x0 + px_whole(divided(rect[2]), dpi), y0 + px_whole(divided(rect[3]), dpi)}, Ink(255));
            }
            for (const Json& r : regions) {  // (never the faces, nor the hair above them)
                if (!kind_in(r, {"face", "head"})) continue;
                const Json& rect = r["rect_mm"];
                const double left = minus(rect[0], times(rect[2], 0.15));
                const double x0 = px_whole(left, dpi) - origin.x;
                const double top = minus(rect[1], times(rect[3], 0.3));
                const double y0 = px_whole(top, dpi) - origin.y;
                draw.ellipse(BoxF{x0, y0, x0 + px_whole(times(rect[2], 1.3), dpi), y0 + px_whole(times(rect[3], 1.45), dpi)}, Ink(0));
            }
        }
        Image soft = mask.filter(Filter::gaussian_blur(core::py_max(1.0, dpi / 150.0)));
        if (std::string_view(value) == "beta") {
            out.first = std::move(soft);
        } else {
            out.second = std::move(soft);
        }
    }
    return out;
}

// (layer.source or {}).get("to", "art")
Json art_to(const core::Layer& layer) {
    const Json source = layer.source ? core::py_or(*layer.source, Json::object()) : Json::object();
    return core::py_get(source, "to", Json("art"));
}

// Whether _finish_placed changes the picture (source.to read first, as it reads it).
bool finish_changes(const core::Layer& layer, const core::Page& page, std::string_view mode) {
    (void)art_to(layer);
    return (mode == "print" || mode == "proof") && page.spec.expression != "color" && layer.role != core::LayerRole::Draft &&
           layer.role != core::LayerRole::Name;
}

// {**out, **value}
void spread_into(Json& out, const Json& value) {
    if (!value.is_object()) throw core::PyTypeError("'" + core::py_type_name(value) + "' object is not a mapping");
    for (const auto& [key, item] : value.items()) out[key] = item;
}

}  // namespace

void keep_placed_picture(const std::filesystem::path& book, const std::string& ref, Image pixels) {
    if (!storage::AssetStore::is_ref(ref) || pixels.empty()) return;
    // (the file under the ref holds the bytes it is named after: the PNG of these pixels, as this process wrote it —
    // AssetStore::put_bytes leaves a file that is there already as it is)
    try {
        const auto bytes = storage::AssetStore(book).get_bytes(ref, ".png", storage::kAssetMaxBytes);
        if (!bytes || storage::AssetStore::ref(*bytes) != ref) return;
    } catch (const std::exception&) {
        return;
    }
    // (what placed_raster makes of the file: open_image(…).convert("RGBA"). A PNG of RGBA pixels has no tRNS; one of
    // other pixels has the transparency they carry, and convert gives the same from either)
    if (pixels.mode() == "RGBA") {
        pixels.set_transparency(Transparency{});
    } else {
        pixels = pixels.convert("RGBA");
    }
    sources().put(core::path_to_utf8(book) + "\n" + ref, std::make_shared<const Image>(std::move(pixels)));
}

void clear_placed_caches() {
    sources().clear();
    finished().clear();
}

std::int64_t placed_cache_pixels() { return sources().pixels() + finished().pixels(); }

Image finish_placed(const Image& fitted, const core::Layer& layer, const core::Page& page, const core::Document* episode, int dpi,
                    std::string_view mode, bool dots, Point origin) {
    const Json to = art_to(layer);
    if ((mode != "print" && mode != "proof") || page.spec.expression == "color" || layer.role == core::LayerRole::Draft ||
        layer.role == core::LayerRole::Name) {
        return fitted;
    }
    const Image rgba = fitted.mode() == "RGBA" ? fitted : fitted.convert("RGBA");
    const Image alpha = rgba.getchannel(3);
    const Image white = Image::create("RGBA", rgba.size(), Ink{255, 255, 255, 255});
    const Image grey = alpha_composite(white, rgba).convert("L");
    if (to == Json("ink")) {  // extracted line art: pure black lines, transparent elsewhere
        const Image ink = grey.point([](int v) { return v < 128 ? 255 : 0; });
        Image out = Image::create("RGBA", rgba.size(), Ink{0, 0, 0, 0});
        out.putalpha(chops::multiply(ink, alpha));
        return out;
    }
    Json style = Json::object();
    if (episode != nullptr) {
        const Json given = core::py_get(core::py_or(core::py_get(episode->studio, "style"), Json::object()), "finish");
        style = core::py_or(given, Json::object());
    }
    Json merged = Json::object();
    spread_into(merged, style);
    spread_into(merged, layer.finish ? core::py_or(*layer.finish, Json::object()) : Json::object());
    const screentone::Finish finish = screentone::Finish::from_dict(merged);
    const auto [dark, light] = clothes_masks(layer, page, episode, rgba.size(), dpi, origin);
    const auto faces = face_mask(layer, page, rgba.size(), dpi, origin);
    const Image done = screentone::finish_gray(grey, finish, dpi, mode == "print" && dots, origin, faces ? &*faces : nullptr,
                                               dark ? &*dark : nullptr, light ? &*light : nullptr);
    Image out = done.convert("RGBA");
    out.putalpha(mode == "proof" ? alpha : alpha.point([](int v) { return v >= 128 ? 255 : 0; }));
    return out;
}

std::optional<Image> placed_raster(const core::Layer& layer, const core::Page& page, const core::Document* episode, Size size,
                                   int dpi, std::string_view mode, bool finish, bool dots, const Box& area) {
    if (episode == nullptr || !episode->asset_dir || !layer.asset || layer.asset->empty() || !layer.placement_mm) return std::nullopt;
    const std::string& ref = *layer.asset;
    if (!storage::AssetStore::is_ref(ref)) return std::nullopt;  // (no such file: Python would look outside assets/ for some)
    const storage::AssetStore store(*episode->asset_dir);
    if (!store.has(ref, ".png")) return std::nullopt;
    const core::Rect& r = *layer.placement_mm;  // may start left of / above the paper, so no clamping here
    // (Python's ints, unbounded, held as whole floats: a placement far off the page — any finite size — is nothing
    // drawn, as in Python; round()'s OverflowError only for an infinity, as Python's)
    const double x0 = core::py_round_whole(r.x.value() / 25.4 * dpi);
    const double y0 = core::py_round_whole(r.y.value() / 25.4 * dpi);
    const double x1 = core::py_round_whole((r.x + r.width).value() / 25.4 * dpi);
    const double y1 = core::py_round_whole((r.y + r.height).value() / 25.4 * dpi);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    const core::Frame* frame = layer.frame_id && !layer.frame_id->empty() ? page.find_frame(*layer.frame_id) : nullptr;
    const Box clip = rect_px(core::clip_box(page, frame, layer.clip_to), dpi);
    // only the visible part is resampled, straight from the source pixels (its corners on the page, so ints)
    const double vx0 = std::max({x0, static_cast<double>(clip.x0), 0.0});
    const double vy0 = std::max({y0, static_cast<double>(clip.y0), 0.0});
    const double vx1 = std::min({x1, static_cast<double>(clip.x1), static_cast<double>(size.width)});
    const double vy1 = std::min({y1, static_cast<double>(clip.y1), static_cast<double>(size.height)});
    if (vx1 <= vx0 || vy1 <= vy0) return std::nullopt;
    const Box visible{static_cast<int>(vx0), static_cast<int>(vy0), static_cast<int>(vx1), static_cast<int>(vy1)};

    const std::string source_key = core::path_to_utf8(*episode->asset_dir) + "\n" + ref;
    std::shared_ptr<const Image> source = sources().get(source_key);
    if (source == nullptr) {
        const auto bytes = store.get_bytes(ref, ".png", storage::kAssetMaxBytes);
        if (!bytes) return std::nullopt;
        ++g_drawn_decoded;
        source = std::make_shared<const Image>(open_image(*bytes, kPillowOpenLimits).convert("RGBA"));
        sources().put(source_key, source);
    }
    // (Python's int differences, exact, as floats: the same while they are below 2**53 pixels; past that, a difference
    // is rounded before the division here, where Python divides the exact ints)
    const double sx = static_cast<double>(source->width()) / (x1 - x0);
    const double sy = static_cast<double>(source->height()) / (y1 - y0);
    const BoxF box{(vx0 - x0) * sx, (vy0 - y0) * sy, (vx1 - x0) * sx, (vy1 - y0) * sy};
    const Size fitted_size{visible.width(), visible.height()};

    Image out = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    const Box part = meet(visible, area);
    if (!empty_box(part)) {
        const Box in_fitted{part.x0 - visible.x0, part.y0 - visible.y0, part.x1 - visible.x0, part.y1 - visible.y0};
        Image piece;
        if (finish && finish_changes(layer, page, mode)) {
            // (the finish looks at each pixel's neighbourhood and lays its dots from the art's corner: the whole art)
            const Json bits = Json::array({x0, y0, x1, y1, visible.x0, visible.y0, visible.x1, visible.y1, dpi, std::string(mode), dots,
                                           page.spec.expression, std::string(core::to_string(layer.role)),
                                           layer.source ? *layer.source : Json(), layer.finish ? *layer.finish : Json(),
                                           core::py_get(episode->studio, "style"),
                                           frame != nullptr && frame->panel ? *frame->panel : Json(), episode->bible.characters});
            const std::string key = source_key + "\n" + core::dump_python(bits);
            std::shared_ptr<const Image> art = finished().get(key);
            if (art == nullptr) {
                const Image fitted = source->resize_in_bands(fitted_size, Resample::Lanczos, box);  // (in bands: the same pixels)
                art = std::make_shared<const Image>(finish_placed(fitted, layer, page, episode, dpi, mode, dots, Point{visible.x0, visible.y0}));
                if (static_cast<std::int64_t>(fitted_size.width) * fitted_size.height <= kKeptFinishPixels) finished().put(key, art);
            }
            piece = art->crop(in_fitted);
        } else {
            piece = source->resize_region_in_bands(fitted_size, in_fitted, Resample::Lanczos, box);  // (in bands: the same pixels)
        }
        out.paste(piece, Point{part.x0 - area.x0, part.y0 - area.y0});
    }
    const bool shaped = frame != nullptr && ((frame->poly && !frame->poly->empty()) || frame->corner_mm > 0 ||
                                             (frame->curves && !frame->curves->empty()));
    if (shaped && (layer.clip_to == "frame" || layer.clip_to == "bleed")) {
        Image shape_mask = Image::create("L", Size{area.width(), area.height()}, Ink(0));
        {
            PageCanvas canvas(shape_mask, area, size);
            Draw& draw = canvas.draw();
            const auto shape = layer.clip_to == "bleed" ? core::bleed_poly(page, frame) : std::nullopt;
            if (layer.clip_to == "bleed" && detail::bleed_shape_mask(draw, page, *frame, dpi)) {
                // (cut along the panel's own outline, out to the bleed)
            } else if (shape) {  // (a slanted bleed panel: cut along its slanted sides, out to the bleed elsewhere)
                std::vector<PointD> points;
                for (const auto& p : *shape) {
                    const auto [px, py] = detail::xy(p.x, p.y, dpi);
                    points.push_back(PointD{static_cast<double>(px), static_cast<double>(py)});
                }
                draw.polygon(points, Ink(255));
            } else {
                detail::fill_frame(draw, *frame, dpi);
            }
            canvas.commit();
        }
        out.putalpha(chops::multiply(out.getchannel(3), shape_mask));
    }
    return out;
}

}  // namespace genko::render
