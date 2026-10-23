// PC4 節 D (SPEC.md COLOR-08「フィルター」): フィルター's kinds as filter_raster's local filters — each kind of the table
// (core/filter_kinds.hpp) whose kernel is in this build (render/fx.hpp) on all three ways: an 8-bit layer (its RGBA; the
// op keeps the result inside the selection, filters.within), a precise layer (its new GKCR bytes) and an ink layer (its
// new GKIR bytes), the selection mixing the result by its coverage — and PC4's new ops (render_picture:
// D4: render/render_picture, vanishing_point: D6: render/vanishing).

#include "render/ops_filters.hpp"

#include <algorithm>
#include <functional>
#include <mutex>

#include "core/adjust.hpp"
#include "core/base64.hpp"
#include "core/ink.hpp"
#include "core/command_bus.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/layer_masks.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/render_picture.hpp"
#include "render/selection.hpp"
#include "render/channels.hpp"
#include "render/smart.hpp"
#include "render/smart_decode.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/metadata.hpp"
#include "render/vanishing.hpp"
#include "core/file_info.hpp"
#include "core/lens.hpp"
#include "core/smart.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

namespace {

using core::Json;

constexpr std::uint64_t kBudget = 2ULL * 1024 * 1024 * 1024;  // (the imaging budget of a local filter, as PC2c's)

thread_local const core::Layer* g_layer = nullptr;  // (FilterLayer)

// The plane a kind reads beside the layer over a grid of w × h (ぼかし（レンズ）'s "depth_map": "layer_mask": the layer's
// mask as it shows over the page — its density and feather laid on —, each 0..1); none when it reads none or the layer
// filtered has no mask (then no depth: as "none").
// (the plane held in the imaging budget, by `held`, while the caller keeps it)
std::optional<std::vector<float>> plane_of(const core::Page& page, const Json& params, std::uint32_t w, std::uint32_t h,
                                           std::optional<image_ops::Held>& held) {
    const Json* depth = core::get(params, "depth_map");
    if (depth == nullptr || *depth != Json("layer_mask") || g_layer == nullptr || !g_layer->mask || !g_layer->mask->png || g_layer->mask->png->empty())
        return std::nullopt;
    held.emplace(std::uint64_t(w) * h * (sizeof(float) + 1));
    const Image decoded = read_png(*g_layer->mask->png).convert("L");
    const Box whole{0, 0, static_cast<int>(w), static_cast<int>(h)};
    const std::string bytes =
        layer_masks::shown(page, *g_layer->mask, decoded, Size{static_cast<int>(w), static_cast<int>(h)}, whole, Resample::Bilinear).tobytes();
    std::vector<float> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

// --- 置き換え's map (D2, design 2.6) ---------------------------------------------------------------------------------------

// The working 200 dpi a layer map is drawn at (its pixel a working pixel: 置き換え's tiling).
constexpr int kMapDpi = 200;

[[noreturn]] void unreadable_map(const std::string& why) { throw core::OpError("the displacement map cannot be read (" + why + ")"); }

// A file's bytes as a displacement map's picture: a PNG's or a PSD's (its merged picture), "RGBA".
Image map_picture(std::string_view bytes) {
    try {
        return smart::eight_bit_picture(bytes).convert("RGBA");
    } catch (const core::OpError&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "memory") throw;
        unreadable_map(error.what());
    }
}

// The page a layer map names (its "page", else the filter's own).
const core::Page& map_page(const core::Document& doc, const core::Page& page, const Json& map) {
    const Json* index = core::get(map, "page");
    if (index == nullptr || index->is_null()) return page;
    std::size_t at = 0;
    try {
        at = core::require_page(doc, Json{{"page", *index}});
    } catch (const core::OpError&) {
        throw core::OpError("the displacement map's page " + index->dump() + " is not in the book");
    }
    // (a page whose strokes and pictures are not read yet: drawn when they are — D2's review P3-6)
    if (doc.is_deferred(at)) throw core::OpError("the displacement map's page " + index->dump() + " is still being read: try again in a moment");
    return doc.page(at);
}

// A layer map drawn as a picture: the page's layer alone over a clear page, or the page as it shows ("merged"), at the
// working 200 dpi.
Image layer_map(const core::Document& doc, const core::Page& page, const Json& map) {
    const core::Page& on = map_page(doc, page, map);
    const std::string id = core::py_str(map["layer"]);
    if (id == "merged") return render_page(on, kMapDpi, proof_options(), &doc).image.convert("RGBA");
    for (const core::Layer& layer : on.layers) {
        if (layer.id != id) continue;
        if (layer.kind == core::LayerKind::Folder || layer.kind == core::LayerKind::Adjust)  // (D2's review P3-5)
            throw core::OpError("the displacement map's layer " + id + " has no picture of its own (a folder or a correction layer)");
        return layer_image(on, layer, kMapDpi, &doc).convert("RGBA");
    }
    throw core::OpError("the displacement map's layer " + id + " is not on the page");
}

// Does the book hold the map `ref` as a file of `suffix` (made by an op this session — its bytes of that kind —, or in its
// folder's assets)?
bool holds_map(const core::Document& doc, const std::string& ref, const std::string& suffix) {
    if (const auto held = doc.filter_files.find(ref); held != doc.filter_files.end() && held->second) {
        const std::string& bytes = *held->second;
        return suffix == ".psd" ? bytes.starts_with("8BPS") : bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8));
    }
    return doc.asset_dir && storage::AssetStore::is_ref(ref) && storage::AssetStore(*doc.asset_dir).has(ref, suffix);
}

// The bytes of a map the book holds (made by an op this session, or in its folder's assets).
std::string held_map(const core::Document& doc, const std::string& ref, const std::string& suffix) {
    if (const auto held = doc.filter_files.find(ref); held != doc.filter_files.end() && held->second) return *held->second;
    if (doc.asset_dir && storage::AssetStore::is_ref(ref)) {
        if (auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(ref, suffix, storage::kAssetMaxBytes)) return std::move(*bytes);
    }
    throw core::OpError("the displacement map " + ref + " is not in the book");
}

// The map of the settings (their normal form) as the kernel reads it — its clear pixels laid over mid-grey (no move
// there) —, none when they name none; held in the imaging budget while `held` lives.
std::optional<fx::Picture> displacement_map(const core::Document& doc, const core::Page& page, const Json& params, std::optional<image_ops::Held>& held) {
    Image picture;
    const Json* data = core::get(params, "map_data");
    const Json* map = core::get(params, "map");
    if (data != nullptr && data->is_string()) {
        picture = map_picture(core::a2b_base64(data->get_ref<const std::string&>()));
    } else if (map != nullptr && map->is_object()) {
        picture = layer_map(doc, page, *map);
    } else if (map != nullptr && map->is_string()) {
        picture = map_picture(held_map(doc, map->get<std::string>(), core::py_str(core::py_get(params, "map_suffix", Json(".png")))));
    } else {
        return std::nullopt;
    }
    fx::Picture out;
    out.width = picture.width();
    out.height = picture.height();
    held.emplace(std::uint64_t(out.width) * std::uint64_t(out.height) * 4);
    out.rgba = picture.tobytes();
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        const int a = static_cast<unsigned char>(out.rgba[i + 3]);
        if (a == 255) continue;
        for (std::size_t c = 0; c < 3; ++c) {
            const int v = static_cast<unsigned char>(out.rgba[i + c]);
            out.rgba[i + c] = static_cast<char>((v * a + 128 * (255 - a) + 127) / 255);
        }
        out.rgba[i + 3] = static_cast<char>(255);
    }
    return out;
}

// --- 照明効果's channel (D4, design 2.6) ---------------------------------------------------------------------------------

// 照明効果's bumps read from an alpha channel of the page ("texture": {"channel": its id or name}) as the kernel reads them:
// the channel over the layer's grid of w × h (a pixel of it a pixel of the grid; its density in each colour), held in the
// imaging budget while `held` lives; none for any other texture. A channel the page does not have is refused.
std::optional<fx::Picture> lighting_texture(const core::Page& page, const Json& params, std::uint32_t w, std::uint32_t h, std::optional<image_ops::Held>& held) {
    const Json* texture = core::get(params, "texture");
    if (texture == nullptr || !texture->is_object()) return std::nullopt;
    const std::string name = core::py_str(core::py_get(*texture, "channel", Json("")));
    for (const core::Channel& channel : page.channels) {
        if (channel.id != name && channel.name != name) continue;
        if (channel.kind != core::ChannelKind::Alpha) throw core::OpError("render_lighting_effects: the texture's channel " + name + " is not an alpha channel");
        held.emplace(std::uint64_t(w) * h * 5);
        const Image mask = channels::mask_of(channel, Size{static_cast<int>(w), static_cast<int>(h)});
        const std::string bytes = mask.tobytes();
        fx::Picture out;
        out.width = static_cast<int>(w);
        out.height = static_cast<int>(h);
        out.rgba.resize(bytes.size() * 4);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            out.rgba[i * 4] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = bytes[i];
            out.rgba[i * 4 + 3] = static_cast<char>(255);
        }
        return out;
    }
    throw core::OpError("render_lighting_effects: the page has no channel " + name + " for the texture");
}

// The coverage of an area (a resolved {poly} or {mask}) on a grid of w × h over the page, a byte a pixel: an 8-bit layer's
// as filter_raster draws its area (the working 200 dpi), any other's by render/ink_edit.
std::string cover_of(const core::Page& page, const Json& area, std::uint32_t w, std::uint32_t h, bool eight) {
    const image_ops::Held held(std::uint64_t(w) * h);
    if (!eight) return ink_edit::area_coverage(page, w, h, area);
    // (PC4 F-b: on a page of grid "finest", the area at the layer's own resolution)
    const selection::AreaMask inside = selection::area_mask(area, raster::dpi_across(page, static_cast<int>(w)));
    Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
    mask.paste(inside.mask, Point{static_cast<int>(inside.x0), static_cast<int>(inside.y0)});
    return mask.tobytes();
}

// The box a coverage covers at all (the base box of a filter run inside a selection: upstream's selection bounds).
fx::Rect covered(const std::string& cover, std::uint32_t w, std::uint32_t h) {
    int x0 = int(w), y0 = int(h), x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            if (cover[std::size_t(y) * w + x] == 0) continue;
            x0 = std::min(x0, int(x));
            x1 = std::max(x1, int(x) + 1);
            y0 = std::min(y0, int(y));
            y1 = std::max(y1, int(y) + 1);
        }
    return fx::Rect{x0, y0, x1, y1}.intersect(fx::Rect{0, 0, int(w), int(h)});
}

// The kernel on a surface: the base box the selection's box (else the grid), the coverage mixing it (`mix`: the caller
// keeps it to the area itself — an 8-bit layer — and only the area's box is worked).
void run(fx::Surface& surface, const fx::Kernel& kernel, core::FilterInks inks, const Json& params, const core::Page& page, const core::Document& doc,
         const Json* area, bool eight, bool mix) {
    const std::uint32_t w = surface.width(), h = surface.height();
    const fx::Rect grid{0, 0, int(w), int(h)};
    const double s = image_ops::grid_scale(page, w, h);
    std::optional<image_ops::Held> plane_held;
    const std::optional<std::vector<float>> plane = plane_of(page, params, w, h, plane_held);
    fx::Reads reads;
    reads.plane = plane ? &*plane : nullptr;
    std::optional<image_ops::Held> map_held;  // (D2: 置き換え's map)
    const std::optional<fx::Picture> map = kernel.kind == "distort_displace"          ? displacement_map(doc, page, params, map_held)
                                           : kernel.kind == "render_lighting_effects" ? lighting_texture(page, params, w, h, map_held)  // (D4)
                                                                                      : std::nullopt;
    reads.map = map ? &*map : nullptr;
    if (area == nullptr) {
        fx::apply(surface, kernel, params, inks, grid, nullptr, s, nullptr, {}, reads);
        return;
    }
    const std::string cover = cover_of(page, *area, w, h, eight);
    const fx::Rect box = covered(cover, w, h);
    if (box.empty()) return;  // (nothing inside: as it was)
    if (mix) {
        fx::apply(surface, kernel, params, inks, box, &cover, s, nullptr, {}, reads);
    } else {  // (the caller keeps the result to the area; a kind that reads the coverage itself has it: 平均)
        reads.within = &cover;
        fx::apply(surface, kernel, params, inks, box, nullptr, s, &box, {}, reads);
    }
}

adjust_ops::LocalFilter local_of(const fx::Kernel& kernel, core::FilterInks inks) {
    adjust_ops::LocalFilter out;
    out.eight = [&kernel, inks](const Image& rgba, const Json& params, const core::Page& page, const core::Document& doc) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::eight_surface(rgba);
        run(*surface, kernel, inks, params, page, doc, nullptr, true, false);
        return fx::take_image(*surface);
    };
    out.eight_in = [&kernel, inks](const Image& rgba, const Json& params, const core::Page& page, const core::Document& doc, const Json& area) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::eight_surface(rgba);
        run(*surface, kernel, inks, params, page, doc, &area, true, false);  // (the caller keeps it to the area)
        return fx::take_image(*surface);
    };
    out.precise = [&kernel, inks](const core::Page& page, const core::Layer& layer, const Json& params, const Json* area, const core::Document& doc) {
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::precise_surface(*layer.color_raster);
        run(*surface, kernel, inks, params, page, doc, area, false, true);
        return fx::take_bytes(*surface);
    };
    out.inks = [&kernel, inks](const core::Page& page, const core::InkPixels& raster, const Json& params, const Json* area, const core::Document& doc) {
        if (!page.color) throw core::OpError("an RGB page holds no ink pixels");
        const ImageAllocationBudget budget{kBudget};
        const auto surface = fx::ink_surface(raster, *page.color);
        run(*surface, kernel, inks, params, page, doc, area, false, true);
        return fx::take_bytes(*surface);
    };
    return out;
}

}  // namespace

FilterLayer::FilterLayer(const core::Layer& layer) : before_(g_layer) { g_layer = &layer; }
FilterLayer::~FilterLayer() { g_layer = before_; }

namespace adjust_ops {

LocalFilters filter_local_filters() {
    LocalFilters out;
    for (const core::FilterInfo& info : core::filter_infos()) {
        const fx::Kernel* kernel = fx::kernel(info.kind);
        if (kernel == nullptr) continue;  // (a kind its group has not drawn yet: not_yet_ported naming it)
        out.emplace_back(info.kind, local_of(*kernel, info.inks));
    }
    return out;
}

}  // namespace adjust_ops

namespace {

// A filter_raster op's settings (what is not the op's own keys).
Json settings_of_op(const Json& op) {
    Json params = Json::object();
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "page" && key != "layer" && key != "id" && key != "kind" && key != "area" && key != "channel") params[key] = value;
    }
    return params;
}

}  // namespace

void refuse_filter(const core::Document& doc, const Json& op) {
    const Json* kind = core::get(op, "kind");
    if (kind == nullptr || !kind->is_string() || kind->get_ref<const std::string&>() != "filter_gallery") return;
    const std::size_t at = core::require_page(doc, op);
    const Json normal = core::normalize_filter("filter_gallery", settings_of_op(op), &doc.page(at));
    for (const Json& effect : normal["effects"]) {
        if (effect.value("visible", true)) return;
    }
    throw core::OpError("filter_gallery: every effect is hidden, so it would change nothing (a smart filter may keep them hidden)");
}

namespace {

// What the book's files said of their cameras, by the file's ref (an asset's sha256 never changes): the last few kept.
struct KnownCameras {
    std::mutex lock;
    std::vector<std::pair<std::string, std::optional<core::exif::Camera>>> kept;  // (the latest last)
};

KnownCameras& known_cameras() {
    static KnownCameras all;
    return all;
}

constexpr std::size_t kCamerasKept = 32;

// What `read` says of the file `ref`, read once while it is kept (a ref not of an asset: read each time).
std::optional<core::exif::Camera> camera_of(const std::string& ref, const std::function<std::optional<core::exif::Camera>()>& read) {
    if (!storage::AssetStore::is_ref(ref)) return read();
    {
        KnownCameras& known = known_cameras();
        const std::lock_guard<std::mutex> hold(known.lock);
        for (const auto& [k, camera] : known.kept)
            if (k == ref) return camera;
    }
    std::optional<core::exif::Camera> camera = read();
    KnownCameras& known = known_cameras();
    const std::lock_guard<std::mutex> hold(known.lock);
    if (known.kept.size() >= kCamerasKept) known.kept.erase(known.kept.begin());
    known.kept.emplace_back(ref, camera);
    return camera;
}

}  // namespace

std::optional<core::exif::Camera> picture_camera(const core::Document& doc, const core::Page& page, const core::Layer* layer) {
    // (a smart object's file: its EXIF as the file holds it)
    if (layer != nullptr && layer->kind == core::LayerKind::Smart) {
        const core::SmartContents* contents = core::smart::contents_of(doc, *layer);
        if (contents == nullptr || contents->kind != "file") return std::nullopt;
        return camera_of(contents->asset, [&]() -> std::optional<core::exif::Camera> {
            try {
                const core::Bytes bytes = smart::file_bytes(doc, *contents);
                if (bytes) {
                    if (const auto format = codecs::detect(*bytes)) {
                        const codecs::FileMetadata meta = codecs::read_metadata(*format, *bytes);
                        if (meta.exif) return core::exif::read(*meta.exif);
                    }
                }
            } catch (const core::Error&) {
            }
            return std::nullopt;
        });
    }
    // (the file the page was opened from: its EXIF kept with the book; a focal length it lacks from what its camera said
    // — a RAW file's)
    if (!page.native_source) return std::nullopt;
    const core::NativeSource& source = *page.native_source;
    std::optional<core::exif::Camera> camera;
    if (!source.exif.empty()) {
        camera = camera_of(source.exif, [&]() -> std::optional<core::exif::Camera> {
            std::optional<std::string> bytes;
            if (const auto held = doc.native_files.find(source.exif); held != doc.native_files.end() && held->second) {
                bytes = *held->second;
            } else if (doc.asset_dir && storage::AssetStore::is_ref(source.exif)) {
                try {
                    bytes = storage::AssetStore(*doc.asset_dir).get_bytes(source.exif, core::kExifSuffix, storage::kAssetMaxBytes);
                } catch (const std::exception&) {
                    bytes.reset();
                }
            }
            if (bytes) return core::exif::read(*bytes);
            return std::nullopt;
        });
    }
    if (source.camera && source.camera->focal && *source.camera->focal > 0 && (!camera || (!camera->focal_length && !camera->focal_length_35mm))) {
        if (!camera) camera.emplace();
        camera->focal_length = source.camera->focal;
    }
    return camera;
}

Json resolve_filter(const core::Document& doc, const core::Page& page, const core::Layer* layer, std::string_view kind, Json params,
                    const std::optional<core::exif::Camera>* known) {
    if ((kind != "lens_correction" && kind != "adaptive_wide_angle") || !params.is_object()) return params;
    const auto focal_given = [&params] {
        const Json* f = core::get(params, "focal_length");
        return f != nullptr && f->is_number() && !f->is_boolean() && f->get<double>() > 0;
    };
    if (kind == "lens_correction") {
        if (params.value("profile", Json("none")) != Json("auto") || focal_given()) return params;
        const auto camera = known != nullptr ? *known : picture_camera(doc, page, layer);
        if (!camera) return params;
        if (const auto f = camera->focal_length_35mm ? camera->focal_length_35mm : camera->focal_length; f && *f > 0) params["focal_length"] = *f;
        return params;
    }
    if (focal_given()) return params;
    const auto camera = known != nullptr ? *known : picture_camera(doc, page, layer);
    if (!camera) return params;
    if (camera->focal_length && *camera->focal_length > 0) {
        params["focal_length"] = *camera->focal_length;
        if (camera->focal_length_35mm) params["crop_factor"] = std::clamp(*camera->focal_length_35mm / *camera->focal_length, 0.1, 10.0);
    } else if (camera->focal_length_35mm) {
        params["focal_length"] = *camera->focal_length_35mm;
        params["crop_factor"] = 1.0;
    }
    return params;
}

std::optional<Json> resolve_filter_op(const core::Document& doc, const Json& op) {
    const Json* kind = core::get(op, "kind");
    if (kind == nullptr || !kind->is_string() || (*kind != Json("lens_correction") && *kind != Json("adaptive_wide_angle"))) return std::nullopt;
    const std::size_t at = core::require_page(doc, op);
    const core::Page& page = doc.page(at);
    const core::Layer* layer = nullptr;
    if (const Json* id = core::get(op, "id"); id != nullptr && id->is_string())
        for (const core::Layer& l : page.layers)
            if (l.id == id->get<std::string>()) layer = &l;
    const Json params = settings_of_op(op);
    const Json filled = resolve_filter(doc, page, layer, kind->get<std::string>(), params);
    if (filled == params) return std::nullopt;
    Json out = op;
    out.update(filled);
    return out;
}

std::optional<Json> filter_report(const core::Document& doc, const Json& op) {
    const Json* kind = core::get(op, "kind");
    if (kind == nullptr || !kind->is_string() || !core::pc4_filter(kind->get_ref<const std::string&>())) return std::nullopt;
    const Json params = settings_of_op(op);
    const std::size_t at = core::require_page(doc, op);
    Json drawn{{"kind", *kind}};
    drawn.update(core::normalize_filter(kind->get_ref<const std::string&>(), params, &doc.page(at)));
    for (const std::string_view key : core::filter_info(kind->get_ref<const std::string&>())->files)
        drawn.erase(std::string(key) + "_data");  // (D2: a file's data said by its ref and suffix, not again)
    // (D6: レンズ補正's profile by its name, as PhotoCraft's result says it)
    if (*kind == Json("lens_correction")) {
        const core::lens::Correction lc = core::lens::correction_of(drawn);
        drawn["profile_name"] = lc.profile ? Json(lc.profile->name) : Json(nullptr);
    }
    return Json{{"filter", std::move(drawn)}};
}

Json keep_filter_files(core::Document& doc, const core::Page& page, std::string_view kind, Json params) {
    // (D4: 照明効果's bumps from a channel of the page — a smart object's picture is drawn on a page of its own, without the
    // page's channels: refused)
    if (kind == "render_lighting_effects" && params.is_object()) {
        if (const Json* texture = core::get(params, "texture"); texture != nullptr && texture->is_object())
            throw core::OpError("render_lighting_effects: a smart filter's texture cannot be a channel of the page (none, red, green, blue, alpha or luminance)");
    }
    const core::FilterInfo* info = core::filter_info(kind);
    if (info == nullptr || info->files.empty() || !params.is_object()) return params;
    for (const std::string_view name : info->files) {
        const std::string key(name), data_key = key + "_data", suffix_key = key + "_suffix";
        std::string bytes, suffix;
        if (const Json* data = core::get(params, data_key); data != nullptr && !data->is_null()) {
            // (its normal form says what it is — a PNG or a PSD of at most 32 MiB —, refused as the op refuses it)
            const Json normal = core::normalize_filter(kind, params, &page);
            bytes = core::a2b_base64(data->get_ref<const std::string&>());
            suffix = normal[suffix_key].get<std::string>();
            (void)map_picture(bytes);  // (a map that cannot be read is not kept)
        } else if (const Json* map = core::get(params, key); map != nullptr && map->is_object()) {
            (void)core::normalize_filter(kind, params, &page);
            bytes = write_png(layer_map(doc, page, *map));
            suffix = ".png";
        } else if (map != nullptr && map->is_string()) {
            // (one of the book's files: held by it, of that kind — as filter_raster reads it —, or the smart object could not
            // be drawn and the book would open read-only: D2's review P2-2)
            const Json normal = core::normalize_filter(kind, params, &page);
            const std::string ref = normal[key].get<std::string>();
            if (!holds_map(doc, ref, normal[suffix_key].get<std::string>())) throw core::OpError("the displacement map " + ref + " is not in the book");
            continue;
        } else {
            continue;
        }
        const std::string ref = core::bytes_ref(bytes);
        doc.filter_files[ref] = std::make_shared<const std::string>(std::move(bytes));
        params[key] = ref;
        params[suffix_key] = suffix;
        params.erase(data_key);
    }
    return params;
}

void register_filter_ops(core::OpRegistry& registry) {
    // フィルター → 描画 → 炎…・額縁…・木… (D4: render/render_picture.cpp) and 消点… (D6: render/vanishing.cpp)
    registry.add("render_picture", picture::render_picture);
    registry.add("vanishing_point", vanishing::vanishing_point);
}

}  // namespace genko::render
