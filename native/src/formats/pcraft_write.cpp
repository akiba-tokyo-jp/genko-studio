// A Genko page written as a PhotoCraft document (formats/pcraft_write.hpp).

#include "formats/pcraft_write.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <thread>
#include <utility>

#include "core/adjust.hpp"
#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "core/patterns.hpp"
#include "core/psd_keep.hpp"
#include "core/smart.hpp"
#include "core/vector.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/output.hpp"
#include "render/blake3.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/page.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/png.hpp"
#include "render/zip.hpp"
#include "render/zstd.hpp"
#include "storage/asset_store.hpp"

namespace genko::formats {

namespace {

namespace pc = render::pcraft;
using core::Json;
using pc::Mode;
using pc::PixelFormat;
using pc::Sample;

constexpr int kTile = pc::kTileSize;

double r6(double v) { return std::round(v * 1e6) / 1e6; }

// --- the bundle's objects ----------------------------------------------------------------------------------------------

// The tiles and blobs of the file, each once by its BLAKE3 hash, held compressed until the manifest (written first) is made.
class Objects {
public:
    std::string blob(std::string_view bytes) {
        const std::string hash = render::blake3::hex(bytes);
        const std::string name = "blobs/" + hash + ".zst";
        if (!files_.contains(name)) add(name, render::zstd::compress(bytes, 1));
        return hash;
    }
    // Raw tiles (each its little-endian samples) compressed on as many threads as the machine has (at most 8).
    std::vector<std::string> tiles(std::vector<std::string> raw) {
        std::vector<std::string> hashes(raw.size()), packed(raw.size());
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        std::vector<std::exception_ptr> failed(threads);  // (a thread's error, thrown here once they have all ended)
        const auto work = [&](std::size_t from) {
            try {
                for (std::size_t i = from; i < raw.size(); i += threads) {
                    hashes[i] = render::blake3::hex(raw[i]);
                    if (!files_.contains("tiles/" + hashes[i] + ".zst")) packed[i] = render::zstd::compress(raw[i], 1);
                }
            } catch (...) {
                failed[from] = std::current_exception();
            }
        };
        if (raw.size() < 4 || threads < 2) {
            for (std::size_t i = 0; i < raw.size(); ++i) {
                hashes[i] = render::blake3::hex(raw[i]);
                if (!files_.contains("tiles/" + hashes[i] + ".zst")) packed[i] = render::zstd::compress(raw[i], 1);
            }
        } else {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work, t);
            for (std::thread& t : pool) t.join();
            for (const std::exception_ptr& e : failed) {
                if (e) std::rethrow_exception(e);
            }
        }
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const std::string name = "tiles/" + hashes[i] + ".zst";
            if (!files_.contains(name)) add(name, std::move(packed[i]));
        }
        return hashes;
    }
    const std::map<std::string, std::string>& files() const { return files_; }

private:
    void add(const std::string& name, std::string packed) {
        held_ += packed.size();
        if (held_ > 0xffffffffull) {
            throw core::Error("image_too_large", "the .pcraft would be larger than 4 GiB or hold more than 65535 files (a zip without zip64)");
        }
        files_.emplace(name, std::move(packed));
        if (files_.size() >= 0xfff0) {
            throw core::Error("image_too_large", "the .pcraft would be larger than 4 GiB or hold more than 65535 files (a zip without zip64)");
        }
    }
    std::map<std::string, std::string> files_;
    std::uint64_t held_ = 0;
};

// A surface of the canvas (w × h) in `format`: each tile made by `row` (row y from x0, n pixels: the format's channels,
// normalized, into `out`), the tiles of nothing but `background` left out.
using RowFn = std::function<void(int y, int x0, int n, double* out)>;

Json surface(Objects& objects, int w, int h, const PixelFormat& format, const std::vector<double>& background, const RowFn& row) {
    const unsigned channels = format.channels();
    const std::size_t pb = format.pixel_bytes();
    const unsigned sb = pc::sample_bytes(format.sample);
    std::string bg(pb, '\0');
    for (unsigned c = 0; c < channels; ++c) pc::write_sample(reinterpret_cast<unsigned char*>(bg.data()) + c * sb, format.sample, background[c]);
    pc::Surface s;
    s.format = format;
    s.background = background;
    std::vector<std::string> raw;
    std::vector<std::pair<int, int>> where;
    std::vector<double> values(std::size_t(kTile) * channels);
    for (int ty = 0; ty * kTile < h; ++ty) {
        for (int tx = 0; tx * kTile < w; ++tx) {
            std::string tile;
            tile.reserve(format.tile_bytes());
            for (int i = 0; i < kTile * kTile; ++i) tile.append(bg);
            bool any = false;
            const int x0 = tx * kTile, n = std::min(kTile, w - x0);
            for (int ly = 0; ly < kTile && ty * kTile + ly < h; ++ly) {
                row(ty * kTile + ly, x0, n, values.data());
                unsigned char* out = reinterpret_cast<unsigned char*>(tile.data()) + std::size_t(ly) * kTile * pb;
                for (int i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < channels; ++c) pc::write_sample(out + std::size_t(i) * pb + c * sb, format.sample, values[std::size_t(i) * channels + c]);
                    if (!any && std::string_view(reinterpret_cast<const char*>(out + std::size_t(i) * pb), pb) != bg) any = true;
                }
            }
            if (!any) continue;
            raw.push_back(std::move(tile));
            where.emplace_back(tx, ty);
        }
    }
    const std::vector<std::string> hashes = objects.tiles(std::move(raw));
    for (std::size_t i = 0; i < hashes.size(); ++i) s.tiles.push_back(pc::TileRef{where[i].first, where[i].second, hashes[i]});
    return pc::to_json(s);
}

// --- the page as a document --------------------------------------------------------------------------------------------

enum class Way { Rgb8, Precise, Inks };

struct Ctx {
    Ctx(const core::Document& d, const core::Page& p, Objects& o, std::vector<std::string>& n) : doc(d), page(p), objects(o), notes(n) {}

    const core::Document& doc;
    const core::Page& page;
    Objects& objects;
    std::vector<std::string>& notes;
    int dpi = 72;
    int w = 1, h = 1;
    double px = 1;  // pixels a mm
    Mode mode = Mode::Rgb;
    Sample sample = Sample::U8;
    Way way = Way::Rgb8;
    std::uint64_t next_id = 1;
    std::set<std::string> said;
    int depth = 0;

    void note(const std::string& text) {
        if (said.insert(text).second) notes.push_back(text);
    }
    PixelFormat layer_format() const {
        const Mode m = mode == Mode::Indexed || mode == Mode::Multichannel ? Mode::Rgb : mode == Mode::Bitmap || mode == Mode::Duotone ? Mode::Grayscale : mode;
        return PixelFormat{m, sample, true};
    }
};

// A book's file by its ref: held in memory (the files ops gave it, opened files' metadata, patterns, lookup tables), else its
// folder's.
std::optional<std::string> file_bytes(const core::Document& doc, const std::string& ref, std::string_view suffix) {
    for (const auto* held : {&doc.smart_files, &doc.native_files, &doc.pattern_files, &doc.luts, &doc.filter_files}) {
        if (const auto it = held->find(ref); it != held->end() && it->second) return *it->second;
    }
    if (doc.asset_dir) return storage::AssetStore(*doc.asset_dir).get_bytes(ref, suffix);
    return std::nullopt;
}

// An 8-bit RGBA picture of the canvas as a layer surface at the page's depth (an RGB page's values: sRGB's, or the page's
// own when `page_values`; a page of inks: its colours by the page's rules — `inks`, a paint layer's own, when given).
Json picture_surface(Ctx& ctx, const render::Image& image, const std::shared_ptr<const std::vector<double>>& inks) {
    const render::Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    const PixelFormat f = ctx.layer_format();
    const unsigned colours = pc::colour_channels(f.mode);
    std::vector<double> background(f.channels(), 0.0);
    if (ctx.way == Way::Inks) {
        std::shared_ptr<const std::vector<double>> values = inks;
        if (!values) values = std::make_shared<const std::vector<double>>(render::ink_picture(rgba, *ctx.page.color).values);
        const std::vector<double>& v = *values;
        return surface(ctx.objects, ctx.w, ctx.h, f, background, [&](int y, int x0, int n, double* out) {
            for (int i = 0; i < n; ++i) {
                const std::size_t at = (std::size_t(y) * ctx.w + x0 + i) * (colours + 1);
                for (unsigned c = 0; c <= colours; ++c) out[std::size_t(i) * (colours + 1) + c] = at + c < v.size() ? v[at + c] : 0.0;
            }
        });
    }
    const std::string bytes = rgba.tobytes();
    return surface(ctx.objects, ctx.w, ctx.h, f, background, [&](int y, int x0, int n, double* out) {
        for (int i = 0; i < n; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + (std::size_t(y) * ctx.w + x0 + i) * 4;
            for (unsigned c = 0; c < 4; ++c) out[std::size_t(i) * 4 + c] = p[c] / 255.0;
        }
    });
}

// A paint layer's own pixels, when they are the page's at this resolution and nothing is drawn on them (a layer an opened
// file made, or one painted at the page's grid): its values at the page's depth, exactly.
std::optional<Json> own_pixels(Ctx& ctx, const core::Layer& layer) {
    if (layer.kind != core::LayerKind::Raster || layer.stroke_count() > 0 || (layer.shapes && !layer.shapes->items.empty()) || !layer.patches.empty() ||
        (layer.effect && !layer.effect->is_null()) || (layer.screen && !layer.screen->is_null()) || (layer.color && !layer.color->empty()) ||
        (layer.tone && !layer.tone->is_null()) || (layer.panel_clip && !ctx.page.frames.empty()))  // (the panels cut what it shows)
        return std::nullopt;
    const PixelFormat f = ctx.layer_format();
    const std::vector<double> background(f.channels(), 0.0);
    if (ctx.way == Way::Precise && layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        if (static_cast<int>(view.width()) != ctx.w || static_cast<int>(view.height()) != ctx.h) return std::nullopt;
        return surface(ctx.objects, ctx.w, ctx.h, f, background, [&](int y, int x0, int n, double* out) {
            for (int i = 0; i < n; ++i) {
                const std::array<double, 4> p = view.pixel(std::size_t(y) * ctx.w + x0 + i);
                for (unsigned c = 0; c < 4; ++c) out[std::size_t(i) * 4 + c] = p[c];
            }
        });
    }
    if (ctx.way == Way::Inks && layer.ink_raster) {
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        if (static_cast<int>(inks.width) != ctx.w || static_cast<int>(inks.height) != ctx.h || inks.space != ctx.page.color->space) return std::nullopt;
        const unsigned all = inks.channels();
        return surface(ctx.objects, ctx.w, ctx.h, f, background, [&](int y, int x0, int n, double* out) {
            for (int i = 0; i < n; ++i) {
                for (unsigned c = 0; c < all; ++c) out[std::size_t(i) * all + c] = inks.value(std::size_t(y) * ctx.w + x0 + i, c);
            }
        });
    }
    if (ctx.way == Way::Rgb8 && layer.raster_png && !layer.raster_png->empty()) {
        render::Image image = render::open_image(*layer.raster_png);
        if (image.width() != ctx.w || image.height() != ctx.h) return std::nullopt;
        return picture_surface(ctx, image, nullptr);
    }
    return std::nullopt;
}

// A grey picture of any size stretched over the canvas as a mask's surface (white: shown).
Json mask_surface(Ctx& ctx, const core::Bytes& png, double untouched) {
    render::Image image = render::open_image(*png).convert("L");
    if (image.width() != ctx.w || image.height() != ctx.h) image = image.resize(render::Size{ctx.w, ctx.h}, render::Resample::Bilinear);
    const std::string bytes = image.tobytes();
    return surface(ctx.objects, ctx.w, ctx.h, PixelFormat{Mode::Grayscale, Sample::U8, false}, {untouched}, [&](int y, int x0, int n, double* out) {
        for (int i = 0; i < n; ++i) out[i] = static_cast<unsigned char>(bytes[std::size_t(y) * ctx.w + x0 + i]) / 255.0;
    });
}

Json mask_json(Ctx& ctx, const core::Mask& mask) {
    return Json{{"surface", mask_surface(ctx, mask.png, 1.0)},
                {"enabled", mask.enabled},
                {"linked", mask.linked},
                {"density", mask.density},
                {"feather", r6(mask.feather_mm * ctx.px)}};
}

Json color_of_rgb(const std::array<int, 3>& rgb, double alpha = 1.0) {
    return pc::to_json(pc::rgb_color(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, alpha));
}

std::array<int, 3> rgb_from(const Json& value) {
    std::array<int, 3> out{0, 0, 0};
    if (value.is_array() && value.size() >= 3) {
        for (std::size_t i = 0; i < 3; ++i) out[i] = value[i].is_number() ? static_cast<int>(std::lround(value[i].get<double>())) : 0;
    }
    return out;
}

double num(const Json& object, const char* key, double fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : fallback;
}

// --- fills -------------------------------------------------------------------------------------------------------------

// A gradient fill's from → to (mm) as upstream's angle and scale over the canvas (the inverse of render/pcraft_read's
// layout): the centre where it lands is the canvas's (a gradient off it: said).
Json gradient_of(Ctx& ctx, const Json& g) {
    const auto point = [&](const char* key, std::array<double, 2> fallback) {
        const auto it = g.find(key);
        if (it == g.end() || !it->is_array() || it->size() < 2) return fallback;
        return std::array<double, 2>{(*it)[0].get<double>() * ctx.px, (*it)[1].get<double>() * ctx.px};
    };
    const auto from = point("from", {0, 0}), to = point("to", {0, 100 * ctx.px});
    const std::string shape = g.value("shape", std::string("linear"));
    const double dx = to[0] - from[0], dy = to[1] - from[1];
    const double span = std::hypot(dx, dy);
    const double angle = span > 0 ? std::atan2(-dy, dx) * 180.0 / 3.14159265358979323846 : 90.0;
    const double a = angle * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double w = ctx.w, h = ctx.h;
    const double chord = std::max(1.0, std::min(w / std::max(std::fabs(c), 1e-6), h / std::max(std::fabs(s), 1e-6)));
    const double len = std::max(1.0, std::hypot(c * w, s * h));
    std::string style = "Linear";
    double scale = 1.0;
    std::array<double, 2> centre{};
    if (shape == "linear") {
        scale = span / chord;
        centre = {(from[0] + to[0]) / 2, (from[1] + to[1]) / 2};
    } else {
        style = shape == "reflected" ? "Reflected" : shape == "angle" ? "Angle" : shape == "diamond" ? "Diamond" : "Radial";
        scale = style == "Reflected" ? 2 * span / chord : 2 * span / len;
        centre = from;
        if (shape == "ellipse") ctx.note("楕円のグラデーションは円形（Radial）として書きました");
    }
    if (std::hypot(centre[0] - w / 2, centre[1] - h / 2) > 0.5) ctx.note("グラデーションの塗りの中心は、PhotoCraft では画像の中心になります");
    Json stops = Json::array();
    if (const auto list = g.find("stops"); list != g.end() && list->is_array() && list->size() >= 2) {
        for (const Json& st : *list) {
            const double opacity = st.size() > 2 && st[2].is_number() ? st[2].get<double>() : 1.0;
            stops.push_back(Json::array({st[0].get<double>(), color_of_rgb(rgb_from(st[1]), opacity)}));
        }
    } else {
        stops.push_back(Json::array({0.0, color_of_rgb(rgb_from(g.value("rgb_from", Json::array({20, 20, 20}))), num(g, "opacity_from", 1.0))}));
        stops.push_back(Json::array({1.0, color_of_rgb(rgb_from(g.value("rgb_to", Json::array({255, 255, 255}))), num(g, "opacity_to", 1.0))}));
    }
    if (g.contains("transparency") || g.contains("repeat") || g.contains("ratio")) ctx.note("グラデーションの不透明度の分岐点・繰り返し・縦横比は PhotoCraft の塗りに無いため外しました");
    return Json{{"Gradient", Json{{"stops", std::move(stops)}, {"angle", r6(angle)}, {"scale", r6(std::max(scale, 1e-3))}, {"style", style}, {"reverse", g.value("reverse", false)}}}};
}

// A fill of Genko's (a colour, a gradient, a pattern of the book's) as upstream's Fill; none for one it has no form of.
std::optional<Json> fill_json(Ctx& ctx, const Json& fill, std::set<std::string>& patterns) {
    if (!fill.is_object()) return std::nullopt;
    if (const Json* pattern = core::pattern_of_fill(fill)) {
        const std::string id = pattern->value("id", std::string());
        const core::Pattern* p = core::find_pattern(ctx.doc, id);
        if (p == nullptr) return std::nullopt;
        patterns.insert(id);
        const auto phase = pattern->value("phase_mm", Json::array({0.0, 0.0}));
        return Json{{"Pattern", Json{{"name", p->name},
                                     {"scale", r6(pattern->value("scale", 100.0) / 100.0 * ctx.dpi / std::max(p->dpi, 1.0))},
                                     {"id", p->id},
                                     {"angle", pattern->value("angle", 0.0)},
                                     {"link", pattern->value("link", true)},
                                     {"phase", Json::array({r6(phase[0].get<double>() * ctx.px), r6(phase[1].get<double>() * ctx.px)})}}}};
    }
    if (const auto g = fill.find("gradient"); g != fill.end() && g->is_object()) return gradient_of(ctx, *g);
    if (const auto rgb = fill.find("rgb"); rgb != fill.end()) {
        if (rgb->is_null()) return std::nullopt;  // (a part's: the layer's ink)
        return Json{{"Solid", color_of_rgb(rgb_from(*rgb))}};
    }
    return std::nullopt;
}

// --- adjustments (a correction's adjust → upstream's doc::Adjustment) --------------------------------------------------

const Json* native_of(const Json& adjust) {
    const auto it = adjust.find("native");
    return it != adjust.end() && it->is_object() ? &*it : nullptr;
}

std::string native_text(const Json& adjust, const char* key) {
    const Json* native = native_of(adjust);
    if (native == nullptr) return {};
    const auto it = native->find(key);
    return it != native->end() && it->is_string() ? it->get<std::string>() : std::string();
}

Json levels_channel(const Json& r) {
    return Json{{"in_black", r6(num(r, "black", 0) / 255.0)}, {"in_white", r6(num(r, "white", 255) / 255.0)}, {"gamma", num(r, "gamma", 1.0)},
                {"out_black", r6(num(r, "out_black", 0) / 255.0)}, {"out_white", r6(num(r, "out_white", 255) / 255.0)}};
}

Json curve_list(const Json& points) {
    Json out = Json::array();
    if (points.is_array()) {
        for (const Json& p : points) {
            if (p.is_array() && p.size() >= 2) out.push_back(Json{{"input", r6(p[0].get<double>() / 255.0)}, {"output", r6(p[1].get<double>() / 255.0)}});
        }
    }
    return out;
}

std::string upstream_space(const std::string& space) { return space == "cmyk" ? "Cmyk" : space == "lab" ? "Lab" : "Rgb"; }

Json array3(const Json& v, double k = 1.0) {
    Json out = Json::array();
    for (std::size_t i = 0; i < 3; ++i) out.push_back(v.is_array() && v.size() > i && v[i].is_number() ? r6(v[i].get<double>() * k) : 0.0);
    return out;
}

Json array4(const Json& v, double k = 1.0) {
    Json out = Json::array();
    for (std::size_t i = 0; i < 4; ++i) out.push_back(v.is_array() && v.size() > i && v[i].is_number() ? r6(v[i].get<double>() * k) : 0.0);
    return out;
}

constexpr std::array<const char*, 6> kHueRanges{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
constexpr std::array<const char*, 9> kSelective{"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};

std::optional<Json> adjustment_of(Ctx& ctx, const Json& adjust, const std::string& name) {
    const std::string kind = adjust.value("kind", std::string());
    const std::string method = native_text(adjust, "method");
    const auto python_form = [&](const char* what) {
        ctx.note(std::string(what) + "の Genko の式は、PhotoCraft の式で書きました（少し違うことがあります）（「" + name + "」）");
    };
    if (kind == "invert") return Json("Invert");
    if (kind == "brightness_contrast") {
        double b = num(adjust, "brightness", 0), c = num(adjust, "contrast", 0);
        std::string m = method;
        if (m.empty()) {
            b = std::clamp(b, -100.0, 100.0) * 1.275;
            m = "legacy";
            python_form("明るさ・コントラスト");
        }
        const bool legacy = m == "legacy";
        return Json{{"BrightnessContrast", Json{{"brightness", std::clamp(b, -150.0, 150.0)}, {"contrast", std::clamp(c, legacy ? -100.0 : -50.0, 100.0)}, {"legacy", legacy}}}};
    }
    if (kind == "levels" || kind == "curve") {
        if (method != "upstream") python_form(kind == "levels" ? "レベル補正" : "トーンカーブ");
        const std::string space = native_text(adjust, "space").empty() ? std::string("rgb") : native_text(adjust, "space");
        const Json* native = native_of(adjust);
        const Json channels = native != nullptr && native->contains("channels") && (*native)["channels"].is_object() ? (*native)["channels"] : Json::object();
        const std::vector<std::string_view> names = core::tone_channels(space == "gray" ? "rgb" : space);
        if (kind == "levels") {
            const Json identity = Json{{"black", 0}, {"white", 255}, {"gamma", 1.0}, {"out_black", 0}, {"out_white", 255}};
            Json per = Json::array();
            for (std::size_t i = 0; i < 3; ++i) {
                const std::string key = i < names.size() ? std::string(names[i]) : std::string();
                per.push_back(levels_channel(channels.contains(key) ? channels[key] : identity));
            }
            Json out{{"master", levels_channel(adjust)}, {"per_channel", std::move(per)}, {"space", upstream_space(space)},
                     {"black", levels_channel(space == "cmyk" && channels.contains("black") ? channels["black"] : identity)}};
            return Json{{"Levels", std::move(out)}};
        }
        Json per = Json::array();
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string key = i < names.size() ? std::string(names[i]) : std::string();
            per.push_back(channels.contains(key) ? curve_list(channels[key]) : Json::array());
        }
        Json master = curve_list(adjust.value("points", Json::array({Json::array({0, 0}), Json::array({255, 255})})));
        Json out{{"master", std::move(master)}, {"per_channel", std::move(per)}, {"space", upstream_space(space)},
                 {"black", space == "cmyk" && channels.contains("black") ? curve_list(channels["black"]) : Json::array()}};
        return Json{{"Curves", std::move(out)}};
    }
    if (kind == "exposure") {
        return Json{{"Exposure", Json{{"exposure", num(adjust, "exposure", 0)}, {"offset", num(adjust, "offset", 0)}, {"gamma", num(adjust, "gamma", 1)}}}};
    }
    if (kind == "posterize") {
        if (method != "floor") python_form("ポスタリゼーション");
        return Json{{"Posterize", Json{{"levels", std::clamp<std::int64_t>(std::llround(num(adjust, "levels", 4)), 2, 255)}}}};
    }
    if (kind == "threshold" || kind == "bitonal") {
        if (kind == "bitonal") ctx.note("白黒にする（bitonal）の補正は、PhotoCraft の 2 階調化として書きました（「" + name + "」）");
        const double t = kind == "bitonal" ? 180.0 : num(adjust, "threshold", 128);
        return Json{{"Threshold", Json{{"level", r6(std::clamp(t, 1.0, 255.0) / 255.0)}}}};
    }
    if (kind == "gradient_map") {
        Json stops = Json::array();
        if (const auto list = adjust.find("stops"); list != adjust.end() && list->is_array()) {
            for (const Json& s : *list) {
                if (!s.is_array() || s.size() < 2) continue;
                const auto rgb = rgb_from(s[1]);
                stops.push_back(Json::array({s[0].get<double>(), Json::array({rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0})}));
            }
        }
        if (stops.size() < 2) stops = Json::array({Json::array({0.0, Json::array({0.0, 0.0, 0.0})}), Json::array({1.0, Json::array({1.0, 1.0, 1.0})})});
        const Json* native = native_of(adjust);
        return Json{{"GradientMap", Json{{"stops", std::move(stops)},
                                         {"reverse", native != nullptr && native->value("reverse", false)},
                                         {"dither", native != nullptr && native->value("dither", false)}}}};
    }
    if (kind == "hue") {
        ctx.note("色相（Genko の hue）の補正は、PhotoCraft の色相・彩度として書きました（「" + name + "」）");
        return Json{{"HueSaturation", Json{{"hue", num(adjust, "hue", 0) * (num(adjust, "hue", 0) > 1 || num(adjust, "hue", 0) < -1 ? 1.0 : 180.0)},
                                           {"saturation", 0.0},
                                           {"lightness", 0.0},
                                           {"colorize", false}}}};
    }
    if (kind == "hue_saturation") {
        Json out{{"hue", num(adjust, "hue", 0)}, {"saturation", num(adjust, "saturation", 0)}, {"lightness", num(adjust, "lightness", 0)}, {"colorize", adjust.value("colorize", false)}};
        Json ranges = Json::array();
        for (int i = 0; i < 6; ++i) {
            const Json r = adjust.value(kHueRanges[static_cast<std::size_t>(i)], Json::object());
            const double c = 60.0 * i;
            ranges.push_back(Json{{"hue", num(r, "hue", 0)}, {"saturation", num(r, "saturation", 0)}, {"lightness", num(r, "lightness", 0)},
                                  {"bounds", r.contains("range") ? array4(r["range"]) : Json::array({c - 45, c - 15, c + 15, c + 45})}});
        }
        out["ranges"] = std::move(ranges);
        return Json{{"HueSaturation", std::move(out)}};
    }
    if (kind == "vibrance") return Json{{"Vibrance", Json{{"vibrance", num(adjust, "vibrance", 0)}, {"saturation", num(adjust, "saturation", 0)}}}};
    if (kind == "color_balance") {
        return Json{{"ColorBalance", Json{{"shadows", array3(adjust.value("shadows", Json()))},
                                          {"midtones", array3(adjust.value("midtones", Json()))},
                                          {"highlights", array3(adjust.value("highlights", Json()))},
                                          {"preserve_luminosity", adjust.value("preserve_luminosity", true)}}}};
    }
    if (kind == "black_white") {
        Json weights = Json::array();
        constexpr std::array<double, 6> kDefaults{40, 60, 40, 60, 20, 80};
        for (std::size_t i = 0; i < 6; ++i) weights.push_back(num(adjust, kHueRanges[i], kDefaults[i]));
        Json tint = nullptr;
        if (adjust.value("tint", false)) {
            const auto rgb = rgb_from(adjust.value("tint_color", Json::array({225, 211, 179})));
            tint = Json::array({rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0});
        }
        return Json{{"BlackWhite", Json{{"weights", std::move(weights)}, {"tint", std::move(tint)}}}};
    }
    if (kind == "photo_filter") {
        const auto rgb = rgb_from(adjust.value("color", Json::array({236, 138, 0})));
        return Json{{"PhotoFilter", Json{{"color", Json::array({rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0})},
                                         {"density", r6(num(adjust, "density", 25) / 100.0)},
                                         {"preserve_luminosity", adjust.value("preserve_luminosity", true)}}}};
    }
    if (kind == "channel_mixer") {
        const bool mono = adjust.value("monochrome", false);
        Json m = Json::array();
        m.push_back(array4(adjust.value(mono ? "gray" : "red", Json::array({100, 0, 0, 0})), 0.01));
        m.push_back(array4(adjust.value("green", Json::array({0, 100, 0, 0})), 0.01));
        m.push_back(array4(adjust.value("blue", Json::array({0, 0, 100, 0})), 0.01));
        return Json{{"ChannelMixer", Json{{"matrix", std::move(m)}, {"monochrome", mono}}}};
    }
    if (kind == "selective_color") {
        Json list = Json::array();
        for (const char* key : kSelective) list.push_back(array4(adjust.value(key, Json::array({0, 0, 0, 0}))));
        return Json{{"SelectiveColor", Json{{"relative", adjust.value("method", std::string("relative")) != "absolute"}, {"adjustments", std::move(list)}}}};
    }
    if (kind == "color_lookup") {
        std::shared_ptr<const core::lut3d::Table> table;
        if (const auto ref = adjust.find("lut"); ref != adjust.end() && ref->is_string()) {
            if (const auto bytes = file_bytes(ctx.doc, ref->get<std::string>(), core::lut3d::kSuffix)) {
                try {
                    table = std::make_shared<const core::lut3d::Table>(core::lut3d::decode(*bytes));
                } catch (const core::Error&) {
                }
            }
        } else if (const auto look = adjust.find("look"); look != adjust.end() && look->is_string()) {
            if (const core::lut3d::Table* builtin = core::lut3d::builtin(look->get<std::string>())) table = std::make_shared<const core::lut3d::Table>(*builtin);
        }
        if (!table) return std::nullopt;
        Json lut = Json::array();
        for (const float v : table->data) lut.push_back(static_cast<double>(v));
        return Json{{"ColorLookup", Json{{"name", adjust.value("name", table->title)},
                                         {"lut", std::move(lut)},
                                         {"size", table->size},
                                         {"tetrahedral", adjust.value("interpolation", std::string("trilinear")) == "tetrahedral"},
                                         {"dither", adjust.value("dither", false)}}}};
    }
    return std::nullopt;
}

// --- layers --------------------------------------------------------------------------------------------------------------

// An upstream layer, and the Genko folder it is in ("" none).
struct Out {
    Json layer;
    std::string folder;
};

Json no_effects() { return Json{{"enabled", true}, {"items", Json::array()}, {"psd_raw", nullptr}, {"reference", nullptr}}; }

Json locks_of(const core::Layer* layer) {
    return Json{{"transparency", layer != nullptr && layer->lock_alpha}, {"pixels", layer != nullptr && layer->locked},
                {"position", layer != nullptr && layer->lock_position}, {"artboard", false}, {"all", false}};
}

// A layer's common settings (its content given): from the Genko layer when there is one, else plain.
Json layer_json(Ctx& ctx, const std::string& name, const core::Layer* layer, Json content, const render::PageLayer* shown) {
    Json out{{"id", ctx.next_id++},
             {"name", name},
             {"visible", layer == nullptr || layer->visible},
             {"locks", locks_of(layer)},
             {"blend", pc::upstream_blend(layer != nullptr ? layer->blend : shown != nullptr && shown->settings ? shown->blend : std::string("normal"))},
             {"opacity", layer != nullptr ? layer->opacity : shown != nullptr && shown->settings ? shown->opacity : 1.0},
             {"fill_opacity", 1.0},
             {"clipped", layer != nullptr ? layer->clip : shown != nullptr && shown->settings && shown->clip},
             {"mask", nullptr},
             {"vector_mask", nullptr},
             {"effects", no_effects()},
             {"label", "None"},
             {"content", std::move(content)},
             {"psd_blocks", Json::array()},
             {"psd_id", nullptr},
             {"fill_cache", nullptr},
             {"link_group", nullptr}};
    if (layer != nullptr) {
        if (layer->mask && layer->mask->png && !layer->mask->png->empty()) out["mask"] = mask_json(ctx, *layer->mask);
        if (layer->vector_mask) out["vector_mask"] = pc::vector_mask_json(*layer->vector_mask, ctx.px);
        if (layer->link) {
            // (a link group's number: the same link the same number)
            std::uint64_t n = 1469598103934665603ull;
            for (const char c : *layer->link) n = (n ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            out["link_group"] = n >> 12;
        }
        if (layer->native_psd) {
            Json blocks = Json::array();
            for (const auto& [key, ref] : layer->native_psd->blocks) {
                if (const auto bytes = file_bytes(ctx.doc, ref, core::kPsdBlockSuffix)) blocks.push_back(Json::array({pc::hex_of(key), ctx.objects.blob(*bytes)}));
            }
            out["psd_blocks"] = std::move(blocks);
            if (layer->native_psd->id && *layer->native_psd->id >= 0 && *layer->native_psd->id <= 0xffffffff) out["psd_id"] = *layer->native_psd->id;
        }
    } else if (shown != nullptr) {
        if (shown->mask) {
            const std::string png = render::write_png(*shown->mask);
            out["mask"] = Json{{"surface", mask_surface(ctx, std::make_shared<const std::string>(png), 1.0)}, {"enabled", true}, {"linked", false}, {"density", 1.0}, {"feather", 0.0}};
        }
        if (shown->vector_mask) out["vector_mask"] = pc::vector_mask_json(*shown->vector_mask, ctx.px);
    }
    return out;
}

Json raster_content(Json surface_json) { return Json{{"kind", "raster"}, {"surface", std::move(surface_json)}}; }

// A smart object of Genko's as upstream's (its contents' file embedded; contents of a page a .pcraft of its own), its picture
// its cache; none when its contents cannot be written.
std::optional<Json> smart_content(Ctx& ctx, const core::Layer& layer, const render::PageLayer* shown, const std::string& name);

const core::Layer* find_layer(const core::Page& page, const std::string& id) {
    for (const core::Layer& l : page.layers) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

std::size_t index_of(const core::Page& page, const std::string& id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == id) return i;
    }
    return page.layers.size();
}

std::string title_of(const core::Layer& layer) { return !layer.title.empty() ? layer.title : std::string(core::to_string(layer.role)) + " " + layer.id.substr(0, 6); }

// A layer of the page (the picture page_layers drew of it, when it did) as an upstream layer.
std::optional<Json> layer_of(Ctx& ctx, const core::Layer& layer, const render::PageLayer* shown, std::set<std::string>& patterns) {
    const std::string name = title_of(layer);
    if (layer.kind == core::LayerKind::Adjust) {
        if (!layer.adjust) return std::nullopt;
        std::optional<Json> adjustment = adjustment_of(ctx, *layer.adjust, name);
        if (!adjustment) {
            ctx.note("補正レイヤー「" + name + "」は PhotoCraft に同じ補正が無いため書きませんでした");
            return std::nullopt;
        }
        Json out = layer_json(ctx, name, &layer, Json{{"kind", "adjustment"}, {"adjustment", std::move(*adjustment)}}, nullptr);
        const std::string blend = native_text(*layer.adjust, "blend");
        out["blend"] = pc::upstream_blend(blend.empty() ? std::string("normal") : blend);
        return out;
    }
    if (layer.kind == core::LayerKind::Fill && layer.fill) {
        if (auto fill = fill_json(ctx, *layer.fill, patterns)) return layer_json(ctx, name, &layer, Json{{"kind", "fill"}, {"fill", std::move(*fill)}}, nullptr);
    }
    if (layer.kind == core::LayerKind::Smart) {
        if (auto content = smart_content(ctx, layer, shown, name)) return layer_json(ctx, name, &layer, std::move(*content), nullptr);
    }
    if (shown != nullptr && shown->shape != nullptr) {
        const core::vector::ShapePart& part = *shown->shape;
        Json content{{"kind", "shape"}, {"fill", nullptr}, {"cache", picture_surface(ctx, shown->image, nullptr)}, {"psd_raw", nullptr},
                     {"path", pc::path_json(part.path, ctx.px)}, {"stroke", nullptr}, {"live", nullptr}};
        if (!part.fill.is_null()) {
            if (auto fill = fill_json(ctx, part.fill, patterns)) content["fill"] = std::move(*fill);
            else if (part.fill.is_object() && part.fill.contains("rgb")) content["fill"] = Json{{"Solid", color_of_rgb({20, 20, 20})}};
        }
        if (part.stroke) {
            const core::vector::ShapeStroke& s = *part.stroke;
            std::optional<Json> paint = s.fill.is_null() ? std::nullopt : fill_json(ctx, s.fill, patterns);
            content["stroke"] = Json{{"width", r6(s.width_mm * ctx.px)},
                                     {"paint", paint ? *paint : Json{{"Solid", color_of_rgb({20, 20, 20})}}},
                                     {"opacity", s.opacity},
                                     {"align", s.align == core::vector::StrokeAlign::Inside ? "Inside" : s.align == core::vector::StrokeAlign::Outside ? "Outside" : "Center"},
                                     {"cap", s.cap == core::vector::LineCap::Round ? "Round" : s.cap == core::vector::LineCap::Square ? "Square" : "Butt"},
                                     {"join", s.join == core::vector::LineJoin::Round ? "Round" : s.join == core::vector::LineJoin::Bevel ? "Bevel" : "Miter"},
                                     {"miter_limit", s.miter_limit},
                                     {"dashes", s.dashes},
                                     {"dash_offset", s.dash_offset}};
        }
        if (part.live) {
            const core::vector::LiveShape& l = *part.live;
            const auto px4 = [&](const std::array<double, 4>& v) { return Json::array({r6(v[0] * ctx.px), r6(v[1] * ctx.px), r6(v[2] * ctx.px), r6(v[3] * ctx.px)}); };
            // (upstream's LiveShape is tagged inside: {"kind": "rect" | "ellipse" | "polygon" | "line", …its fields})
            switch (l.kind) {
            case core::vector::LiveShape::Kind::Rect:
                content["live"] = Json{{"kind", "rect"}, {"rect", px4(l.rect)}, {"radii", px4(l.radii)}};
                break;
            case core::vector::LiveShape::Kind::Ellipse:
                content["live"] = Json{{"kind", "ellipse"}, {"rect", px4(l.rect)}};
                break;
            case core::vector::LiveShape::Kind::Polygon:
                content["live"] = Json{{"kind", "polygon"}, {"rect", px4(l.rect)}, {"sides", l.sides}, {"star_ratio", l.star_ratio}};
                if (l.angle != 0.0) ctx.note("回した多角形の生きた形は、PhotoCraft では回さない形になります");
                break;
            case core::vector::LiveShape::Kind::Line:
                content["live"] = Json{{"kind", "line"},
                                       {"from", Json::array({r6(l.from.x * ctx.px), r6(l.from.y * ctx.px)})},
                                       {"to", Json::array({r6(l.to.x * ctx.px), r6(l.to.y * ctx.px)})},
                                       {"weight", r6(l.weight_mm * ctx.px)}};
                break;
            }
        }
        Json out = layer_json(ctx, part.name ? *part.name : name, shown->id.empty() ? nullptr : &layer, std::move(content), shown);
        if (part.opacity < 1.0) out["opacity"] = out["opacity"].get<double>() * part.opacity;
        return out;
    }
    // its pixels: its own at the page's depth, else its picture
    if (auto own = own_pixels(ctx, layer)) return layer_json(ctx, name, &layer, raster_content(std::move(*own)), nullptr);
    if (shown != nullptr) return layer_json(ctx, name, &layer, raster_content(picture_surface(ctx, shown->image, shown->inks)), nullptr);
    const render::Image picture = render::layer_image(ctx.page, layer, ctx.dpi, &ctx.doc);
    return layer_json(ctx, name, &layer, raster_content(picture_surface(ctx, picture, nullptr)), nullptr);
}

// A contents page as a .pcraft's bytes (written as a book of that page alone, at its own resolution).
std::string contents_bytes(Ctx& ctx, const core::SmartContents& contents);

std::optional<Json> smart_content(Ctx& ctx, const core::Layer& layer, const render::PageLayer* shown, const std::string& name) {
    const core::SmartContents* contents = core::smart::contents_of(ctx.doc, layer);
    if (contents == nullptr) return std::nullopt;
    std::string file_name = contents->name.empty() ? std::string("contents") : contents->name;
    std::string bytes;
    double cdpi = 72;
    if (contents->kind == "page" && contents->page) {
        if (ctx.depth + 1 >= core::smart::kDepthMax) return std::nullopt;
        bytes = contents_bytes(ctx, *contents);
        cdpi = contents->page->spec.dpi.value();
        if (!file_name.ends_with(".pcraft")) file_name += ".pcraft";
    } else {
        const auto read = file_bytes(ctx.doc, contents->asset, contents->suffix);
        if (!read) return std::nullopt;
        bytes = *read;
        cdpi = contents->dpi > 0 ? contents->dpi : ctx.dpi;
        if (!contents->suffix.empty() && !file_name.ends_with(contents->suffix)) file_name += contents->suffix;
    }
    const core::smart::Matrix m = core::smart::matrix_of(layer);
    const double k = ctx.dpi / cdpi;
    const pc::Affine t{r6(m.a * k), r6(m.b * k), r6(m.c * k), r6(m.d * k), r6(m.e * ctx.px), r6(m.f * ctx.px)};
    if (layer.smart->contains("perspective") || layer.smart->contains("warp")) {
        ctx.note("スマートオブジェクトの遠近法・ワープは PhotoCraft に書けないため、見えだけを画素（キャッシュ）にしました（「" + name + "」）");
    }
    Json filters = Json::array();
    if (const auto list = layer.smart->find("filters"); list != layer.smart->end() && list->is_array()) {
        for (const Json& f : *list) {
            const std::string kind = f.value("kind", std::string());
            const auto mapped = core::to_upstream(kind, f.value("params", Json::object()), ctx.dpi);
            if (!mapped) {
                ctx.note("スマートフィルター「" + kind + "」は PhotoCraft に同じものが無いため外しました（「" + name + "」）");
                continue;
            }
            filters.push_back(Json{{"command", mapped->first}, {"params", mapped->second}, {"blend", pc::upstream_blend(f.value("blend", std::string("normal")))},
                                   {"opacity", f.value("opacity", 1.0)}, {"visible", f.value("visible", true)}});
        }
    }
    Json filter_mask = nullptr;
    if (const auto fm = layer.smart->find("filter_mask"); fm != layer.smart->end() && fm->is_object() && fm->contains("asset")) {
        if (const auto png = file_bytes(ctx.doc, (*fm)["asset"].get<std::string>(), ".png")) {
            filter_mask = Json{{"surface", mask_surface(ctx, std::make_shared<const std::string>(*png), 1.0)},
                               {"enabled", fm->value("enabled", true)},
                               {"linked", false},
                               {"density", 1.0},
                               {"feather", 0.0}};
        }
    }
    Json stack = nullptr;
    if (const auto s = layer.smart->find("stack"); s != layer.smart->end() && s->is_string()) {
        std::string word = s->get<std::string>(), out;
        bool up = true;
        for (const char c : word) {
            if (c == '_') {
                up = true;
                continue;
            }
            out.push_back(up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
            up = false;
        }
        stack = out;
    }
    const auto on = layer.smart->find("filters_on");
    return Json{{"kind", "smart"},
                {"source", Json{{"kind", "embedded"}, {"file_name", file_name}, {"blob", ctx.objects.blob(bytes)}}},
                {"transform", pc::to_json(t)},
                {"smart_filters", std::move(filters)},
                {"cache", shown != nullptr ? picture_surface(ctx, shown->image, shown->inks) : Json(nullptr)},
                {"psd_raw", nullptr},
                {"filters_enabled", !(on != layer.smart->end() && on->is_boolean() && !on->get<bool>())},
                {"filter_mask", std::move(filter_mask)},
                {"warp", nullptr},
                {"stack_mode", std::move(stack)}};
}

// Folders: the layers of one in a row (in the page's order) made a group of it, nested as the folders are.
std::vector<Json> grouped(Ctx& ctx, std::vector<Out> flat) {
    // (each layer's folders, outermost first)
    const auto chain = [&](const std::string& folder) {
        std::vector<std::string> out;
        std::set<std::string> seen;
        for (std::string at = folder; !at.empty() && seen.insert(at).second;) {
            out.insert(out.begin(), at);
            const core::Layer* f = find_layer(ctx.page, at);
            at = f != nullptr && f->parent_id.is_string() ? f->parent_id.get<std::string>() : std::string();
        }
        return out;
    };
    std::function<std::vector<Json>(std::vector<std::pair<std::vector<std::string>, Json>>&, std::size_t)> build;
    build = [&](std::vector<std::pair<std::vector<std::string>, Json>>& items, std::size_t depth) {
        std::vector<Json> out;
        std::size_t i = 0;
        while (i < items.size()) {
            if (items[i].first.size() <= depth) {
                out.push_back(std::move(items[i].second));
                ++i;
                continue;
            }
            const std::string folder = items[i].first[depth];
            std::vector<std::pair<std::vector<std::string>, Json>> run;
            while (i < items.size() && items[i].first.size() > depth && items[i].first[depth] == folder) run.push_back(std::move(items[i++]));
            std::vector<Json> children = build(run, depth + 1);
            const core::Layer* f = find_layer(ctx.page, folder);
            Json content{{"kind", "group"}, {"children", std::move(children)}, {"expanded", true}, {"artboard", nullptr}};
            Json group = layer_json(ctx, f != nullptr ? title_of(*f) : std::string("フォルダー"), f, std::move(content), nullptr);
            group["blend"] = "PassThrough";
            group["opacity"] = 1.0;
            group["clipped"] = false;
            group["mask"] = nullptr;
            group["vector_mask"] = nullptr;
            out.push_back(std::move(group));
        }
        return out;
    };
    std::vector<std::pair<std::vector<std::string>, Json>> items;
    items.reserve(flat.size());
    for (Out& o : flat) items.emplace_back(chain(o.folder), std::move(o.layer));
    return build(items, 0);
}

std::string folder_of(const core::Layer& layer) { return layer.parent_id.is_string() ? layer.parent_id.get<std::string>() : std::string(); }

// --- the document ------------------------------------------------------------------------------------------------------

Json document_json(Ctx& ctx) {
    const core::Page& page = ctx.page;
    std::set<std::string> patterns;
    std::vector<Out> flat;
    std::size_t next = 0;  // (the page's layers written so far, page_layers' or hidden)
    const auto hidden_until = [&](std::size_t index) {
        for (; next < index && next < page.layers.size(); ++next) {
            const core::Layer& l = page.layers[next];
            // (the layers page_layers does not give: the hidden ones, and a fill layer of a colour or a gradient, which it
            // draws into the page's picture only)
            const bool unseen = !l.visible || (l.kind == core::LayerKind::Fill && l.fill && core::pattern_of_fill(*l.fill) == nullptr);
            if (!unseen || l.kind == core::LayerKind::Folder || !l.exportable || l.role == core::LayerRole::Name || l.role == core::LayerRole::Draft) continue;
            if (auto made = layer_of(ctx, l, nullptr, patterns)) flat.push_back(Out{std::move(*made), folder_of(l)});
        }
    };
    const bool picture_page = page.native_source.has_value() || (page.spec.grid && *page.spec.grid == "finest");
    std::vector<std::vector<Out>> groups;  // (a pen layer's group being made: page_layers' "end" … "group")
    bool drawn = false;  // (Genko's own elements drawn as pixels)
    const std::function<void(render::PageLayer&&)> each = [&](render::PageLayer&& shown) {
        if (shown.section == "end") {
            groups.emplace_back();
            return;
        }
        if (shown.section == "group") {
            std::vector<Out> children = groups.empty() ? std::vector<Out>{} : std::move(groups.back());
            if (!groups.empty()) groups.pop_back();
            const core::Layer* layer = shown.id.empty() ? nullptr : find_layer(page, shown.id);
            if (layer != nullptr) hidden_until(index_of(page, layer->id));
            Json kids = Json::array();
            for (Out& o : children) kids.push_back(std::move(o.layer));
            Json group = layer_json(ctx, shown.name, layer, Json{{"kind", "group"}, {"children", std::move(kids)}, {"expanded", true}, {"artboard", nullptr}}, &shown);
            Out out{std::move(group), layer != nullptr ? folder_of(*layer) : std::string()};
            if (!groups.empty()) groups.back().push_back(std::move(out));
            else {
                flat.push_back(std::move(out));
                next = std::max(next, layer != nullptr ? index_of(page, layer->id) + 1 : next);
            }
            return;
        }
        const core::Layer* layer = shown.id.empty() ? nullptr : find_layer(page, shown.id);
        if (layer == nullptr) {
            if (!shown.image.getbbox() && !(groups.size() > 0 && shown.settings)) return;  // (nothing drawn)
            if (shown.name == "紙" && picture_page) return;  // (an opened picture's page: its own layers, no paper)
            if (groups.empty()) drawn = true;
            Json made = shown.shape != nullptr ? *layer_of(ctx, core::Layer{}, &shown, patterns)
                                               : layer_json(ctx, shown.name, nullptr, raster_content(picture_surface(ctx, shown.image, shown.inks)), &shown);
            if (!groups.empty()) groups.back().push_back(Out{std::move(made), {}});
            else flat.push_back(Out{std::move(made), {}});
            return;
        }
        const std::size_t at = index_of(page, layer->id);
        hidden_until(at);
        std::optional<Json> made = layer_of(ctx, *layer, &shown, patterns);
        if (made) {
            if (!groups.empty()) groups.back().push_back(Out{std::move(*made), {}});
            else flat.push_back(Out{std::move(*made), folder_of(*layer)});
        }
        next = std::max(next, at + 1);
    };
    const std::function<void(const core::Layer&)> adjust_layers = [&](const core::Layer& layer) {
        const std::size_t at = index_of(page, layer.id);
        hidden_until(at);
        if (auto made = layer_of(ctx, layer, nullptr, patterns)) flat.push_back(Out{std::move(*made), folder_of(layer)});
        next = std::max(next, at + 1);
    };
    render::page_layers(page, ctx.doc, ctx.dpi, each, nullptr, &adjust_layers, true);
    hidden_until(page.layers.size());
    if (drawn && !picture_page) ctx.note("ペンの線・コマ枠・台詞・トーン・効果線・紙など PhotoCraft に無いものは、画素のレイヤーにしました");

    Json doc{{"id", 1},
             {"name", ctx.doc.title.empty() ? std::string("無題") : ctx.doc.title},
             {"size", Json{{"width", ctx.w}, {"height", ctx.h}}},
             {"resolution_dpi", static_cast<double>(ctx.dpi)},
             {"mode", pc::to_string(ctx.mode)},
             {"depth", pc::to_string(ctx.sample)},
             {"icc_profile", nullptr}};
    // its profile
    if (page.color && page.color->space != core::InkSpace::Lab && page.color->icc && !page.color->icc->empty()) {
        doc["icc_profile"] = ctx.objects.blob(*page.color->icc);
    } else if (!page.color && page.rgb && !page.rgb->profile.empty()) {
        const std::string icc(detail::rgb_icc(*page.rgb));
        if (!icc.empty()) doc["icc_profile"] = ctx.objects.blob(icc);
    }
    Json layers = Json::array();
    for (Json& l : grouped(ctx, std::move(flat))) layers.push_back(std::move(l));
    doc["layers"] = std::move(layers);

    // channels and the Quick Mask
    Json channels = Json::array();
    Json quick = nullptr;
    const PixelFormat channel_format{Mode::Grayscale, ctx.sample, false};
    for (const core::Channel& ch : page.channels) {
        Json surf;
        if (ch.raster) {
            const core::InkPixels mask = core::decode_ink_raster(*ch.raster);
            const bool same = static_cast<int>(mask.width) == ctx.w && static_cast<int>(mask.height) == ctx.h;
            surf = surface(ctx.objects, ctx.w, ctx.h, channel_format, {0.0}, [&](int y, int x0, int n, double* out) {
                for (int i = 0; i < n; ++i) {
                    const int x = x0 + i;
                    const std::uint32_t sx = same ? static_cast<std::uint32_t>(x) : static_cast<std::uint32_t>(std::min<std::int64_t>(mask.width - 1, std::int64_t(x) * mask.width / ctx.w));
                    const std::uint32_t sy = same ? static_cast<std::uint32_t>(y) : static_cast<std::uint32_t>(std::min<std::int64_t>(mask.height - 1, std::int64_t(y) * mask.height / ctx.h));
                    out[i] = mask.value(std::size_t(sy) * mask.width + sx, 0);
                }
            });
        } else {
            surf = pc::to_json(pc::Surface{channel_format, {0.0}, {}});
        }
        Json c{{"name", ch.name},
               {"surface", std::move(surf)},
               {"spot", ch.kind == core::ChannelKind::Spot ? Json::array({color_of_rgb(ch.color), ch.solidity}) : Json(nullptr)},
               {"color", color_of_rgb(ch.color)},
               {"opacity", ch.opacity},
               {"indicates", ch.indicates == "selected" ? "selectedAreas" : "maskedAreas"}};
        if (ch.quick_mask) quick = std::move(c);
        else channels.push_back(std::move(c));
    }
    doc["channels"] = std::move(channels);
    // guides (the page's rulers of kind guide)
    Json horizontal = Json::array(), vertical = Json::array();
    for (const Json& r : page.rulers) {
        if (!r.is_object() || r.value("kind", std::string()) != "guide" || !r.contains("at") || !r["at"].is_number()) continue;
        (r.value("axis", std::string()) == "h" ? horizontal : vertical).push_back(r6(r["at"].get<double>() * ctx.px));
    }
    doc["guides"] = Json{{"horizontal", std::move(horizontal)}, {"vertical", std::move(vertical)}};
    doc["selection"] = nullptr;
    // metadata: the book's File Info as XMP, the EXIF it was opened with, the PSD's kept resources and blocks
    Json metadata{{"xmp", nullptr}, {"exif", nullptr}, {"psd_resources", Json::array()}, {"psd_global_blocks", Json::array()}};
    if (const auto xmp = book_xmp(ctx.doc)) metadata["xmp"] = *xmp;
    if (page.native_source && !page.native_source->exif.empty()) {
        if (const auto exif = file_bytes(ctx.doc, page.native_source->exif, core::kExifSuffix)) metadata["exif"] = ctx.objects.blob(*exif);
    }
    if (page.native_psd) {
        for (const core::PsdResource& r : page.native_psd->resources) {
            if (const auto bytes = file_bytes(ctx.doc, r.ref, core::kPsdBlockSuffix)) metadata["psd_resources"].push_back(Json::array({r.id, r.name, ctx.objects.blob(*bytes)}));
        }
        for (const core::PsdGlobal& g : page.native_psd->globals) {
            if (const auto bytes = file_bytes(ctx.doc, g.ref, core::kPsdBlockSuffix))
                metadata["psd_global_blocks"].push_back(Json::array({pc::hex_of(g.signature), pc::hex_of(g.key), ctx.objects.blob(*bytes)}));
        }
    }
    doc["metadata"] = std::move(metadata);
    doc["global_light"] = Json{{"angle", 120.0}, {"altitude", 30.0}};
    // paths
    Json paths = Json::array();
    for (const core::vector::NamedPath& p : page.paths) paths.push_back(Json{{"name", p.name}, {"path", pc::path_json(p.path, ctx.px)}, {"psd_raw", nullptr}});
    doc["paths"] = std::move(paths);
    doc["work_path"] = page.work_path ? pc::path_json(*page.work_path, ctx.px) : Json(nullptr);
    doc["clipping_path"] = page.clipping_path ? Json{{"name", page.clipping_path->name}, {"flatness", page.clipping_path->flatness}} : Json(nullptr);
    doc["quick_mask"] = std::move(quick);
    // the patterns its fills use
    Json list = Json::array();
    for (const std::string& id : patterns) {
        const core::Pattern* p = core::find_pattern(ctx.doc, id);
        if (p == nullptr) continue;
        const auto bytes = file_bytes(ctx.doc, p->asset, core::pattern_suffix(*p));
        if (!bytes) continue;
        const int w = static_cast<int>(p->width), h = static_cast<int>(p->height);
        Json surf;
        if (p->mode == "rgb" && p->depth == 8) {
            const render::Image image = render::open_image(*bytes).convert("RGBA");
            const std::string raw = image.tobytes();
            surf = surface(ctx.objects, w, h, PixelFormat{Mode::Rgb, Sample::U8, true}, {0, 0, 0, 0}, [&](int y, int x0, int n, double* out) {
                for (int i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < 4; ++c) out[std::size_t(i) * 4 + c] = static_cast<unsigned char>(raw[(std::size_t(y) * w + x0 + i) * 4 + c]) / 255.0;
                }
            });
        } else if (p->mode == "rgb") {
            const core::ColorRasterView view(*bytes);
            surf = surface(ctx.objects, w, h, PixelFormat{Mode::Rgb, p->depth == 32 ? Sample::F32 : Sample::U16, true}, {0, 0, 0, 0}, [&](int y, int x0, int n, double* out) {
                for (int i = 0; i < n; ++i) {
                    const auto px = view.pixel(std::size_t(y) * w + x0 + i);
                    for (unsigned c = 0; c < 4; ++c) out[std::size_t(i) * 4 + c] = px[c];
                }
            });
        } else if (p->mode == "gray" && p->depth == 8) {
            const render::Image image = render::open_image(*bytes).convert("LA");
            const std::string raw = image.tobytes();
            surf = surface(ctx.objects, w, h, PixelFormat{Mode::Grayscale, Sample::U8, true}, {0, 0}, [&](int y, int x0, int n, double* out) {
                for (int i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < 2; ++c) out[std::size_t(i) * 2 + c] = static_cast<unsigned char>(raw[(std::size_t(y) * w + x0 + i) * 2 + c]) / 255.0;
                }
            });
        } else {
            const core::InkPixels inks = core::decode_ink_raster(*bytes);
            const Mode m = inks.space == core::InkSpace::Cmyk ? Mode::Cmyk : inks.space == core::InkSpace::Lab ? Mode::Lab : Mode::Grayscale;
            const PixelFormat f{m, inks.depth == 32 ? Sample::F32 : inks.depth == 16 ? Sample::U16 : Sample::U8, true};
            const unsigned all = inks.channels();
            surf = surface(ctx.objects, w, h, f, std::vector<double>(f.channels(), 0.0), [&](int y, int x0, int n, double* out) {
                for (int i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < all; ++c) out[std::size_t(i) * all + c] = inks.value(std::size_t(y) * w + x0 + i, c);
                }
            });
        }
        list.push_back(Json{{"id", p->id}, {"name", p->name}, {"width", p->width}, {"height", p->height}, {"surface", std::move(surf)}});
    }
    doc["patterns"] = std::move(list);
    // the indexed page's table, a duotone page's inks
    doc["color_table"] = nullptr;
    if (!page.color && page.rgb && page.rgb->table) {
        Json colors = Json::array();
        for (const auto& c : page.rgb->table->colors) colors.push_back(Json::array({c[0], c[1], c[2]}));
        doc["color_table"] = Json{{"colors", std::move(colors)}, {"transparent", page.rgb->table->transparent ? Json(*page.rgb->table->transparent) : Json(nullptr)}};
    }
    doc["duotone"] = nullptr;
    if (page.color && !page.color->duotone.empty()) {
        Json inks = Json::array();
        for (const core::DuotoneInk& ink : page.color->duotone) {
            Json curve = Json::array();
            for (const auto& p : ink.curve) curve.push_back(Json{{"input", r6(p[0] / 100.0)}, {"output", r6(p[1] / 100.0)}});
            inks.push_back(Json{{"name", ink.name}, {"color", Json::array({ink.color[0] / 255.0, ink.color[1] / 255.0, ink.color[2] / 255.0})}, {"curve", std::move(curve)}});
        }
        doc["duotone"] = Json{{"inks", std::move(inks)}, {"psd_raw", nullptr}};
    }
    return doc;
}

// The page's picture made small (its longer side at most `side`) as a PNG: the composite preview and the thumbnail.
std::pair<std::string, std::string> previews(const core::Document& doc, const core::Page& page, int dpi, int w, int h) {
    const double longest = std::max(w, h);
    const int preview_dpi = std::max(1, static_cast<int>(std::floor(dpi * std::min(1.0, 1024.0 / longest))));
    render::RenderOptions options;
    options.mode = "print";
    render::Image image = render::render_page(page, preview_dpi, options, &doc).image;
    const auto fit = [&](const render::Image& from, int side) {
        const double k = std::min(1.0, side / static_cast<double>(std::max(from.width(), from.height())));
        const render::Size size{std::max(1, static_cast<int>(std::lround(from.width() * k))), std::max(1, static_cast<int>(std::lround(from.height() * k)))};
        return size == from.size() ? from : from.resize(size, render::Resample::Lanczos);
    };
    const render::Image composite = fit(image, 1024).convert("RGBA");
    const render::Image thumb = fit(composite, 256);
    return {render::write_png(thumb), render::write_png(composite)};
}

std::vector<std::string> write_page(const core::Document& doc, const core::Page& page, int dpi, int depth, const std::function<void(std::string_view)>& sink) {
    check_dpi(dpi);
    std::vector<std::string> notes;
    Objects objects;
    Ctx ctx(doc, page, objects, notes);
    ctx.dpi = dpi;
    ctx.depth = depth;
    ctx.w = render::mm_to_px(page.spec.width_mm.value(), dpi);
    ctx.h = render::mm_to_px(page.spec.height_mm.value(), dpi);
    ctx.px = dpi / 25.4;
    if (ctx.w < 1 || ctx.h < 1) throw core::Error("value", "the page has no pixels at this resolution");
    if (std::int64_t(ctx.w) * ctx.h > 120'000'000 || ctx.w > 100'000 || ctx.h > 100'000) {
        throw core::Error("image_too_large", "the picture is too large at this resolution");
    }
    if (page.color) {
        ctx.way = Way::Inks;
        switch (page.color->space) {
        case core::InkSpace::Gray:
            ctx.mode = page.color->bitmap ? Mode::Bitmap : !page.color->duotone.empty() ? Mode::Duotone : Mode::Grayscale;
            break;
        case core::InkSpace::Cmyk:
            ctx.mode = Mode::Cmyk;
            break;
        case core::InkSpace::Lab:
            ctx.mode = Mode::Lab;
            break;
        default:
            throw core::Error("value", "a multichannel page is not written as a .pcraft by this build (convert it to RGB, CMYK or grey first)");
        }
        ctx.sample = page.color->depth == 32 ? Sample::F32 : page.color->depth == 16 ? Sample::U16 : Sample::U8;
    } else {
        const int bits = page.rgb ? page.rgb->depth : 8;
        ctx.way = bits == 8 ? Way::Rgb8 : Way::Precise;
        ctx.mode = page.rgb && page.rgb->table ? Mode::Indexed : Mode::Rgb;
        ctx.sample = bits == 32 ? Sample::F32 : bits == 16 ? Sample::U16 : Sample::U8;
    }
    Json document = document_json(ctx);
    const auto [thumb, composite] = previews(doc, page, dpi, ctx.w, ctx.h);
    Json manifest{{"format_version", pc::kFormatVersion}, {"generator", std::string(pc::kGenerator)}, {"document", std::move(document)},
                  {"thumbnail", std::string(pc::kThumb)}, {"composite", std::string(pc::kComposite)}};
    core::DumpOptions options;
    options.indent = 2;
    options.item_separator = ",";
    const std::string text = core::dump(manifest, options);
    render::zip::StoreWriter zip(sink);
    zip.add(pc::kManifest, text);
    zip.add(pc::kThumb, thumb);
    zip.add(pc::kComposite, composite);
    for (const auto& [name, packed] : objects.files()) zip.add(name, packed);
    zip.finish();
    return notes;
}

std::string contents_bytes(Ctx& ctx, const core::SmartContents& contents) {
    std::string out;
    const int dpi = std::clamp(static_cast<int>(std::lround(contents.page->spec.dpi.value())), 1, 100000);
    std::vector<std::string> notes = write_page(ctx.doc, *contents.page, dpi, ctx.depth + 1, [&out](std::string_view bytes) { out.append(bytes); });
    for (const std::string& note : notes) ctx.note("スマートオブジェクトの中身: " + note);
    return out;
}

const core::Page& page_at(const core::Document& doc, std::size_t page) {
    if (page >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(page)) throw core::Error("page_not_loaded", "the page is not read yet");
    return doc.page(page);
}

int dpi_of(const core::Page& page, const PcraftOptions& options) {
    return options.dpi ? *options.dpi : std::max(1, static_cast<int>(std::lround(page.spec.dpi.value())));
}

}  // namespace

std::vector<std::string> write_pcraft(const core::Document& doc, std::size_t page, const std::function<void(std::string_view)>& sink, const PcraftOptions& options) {
    const core::Page& p = page_at(doc, page);
    return write_page(doc, p, dpi_of(p, options), 0, sink);
}

std::string pcraft_bytes(const core::Document& doc, std::size_t page, const PcraftOptions& options, std::vector<std::string>* notes) {
    std::string out;
    std::vector<std::string> said = write_pcraft(doc, page, [&out](std::string_view bytes) { out.append(bytes); }, options);
    if (notes != nullptr) *notes = std::move(said);
    return out;
}

std::vector<std::string> write_pcraft_file(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const PcraftOptions& options) {
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    detail::PartFile part(dest);
    std::vector<std::string> notes = write_pcraft(doc, page, [&part](std::string_view bytes) { part.write(bytes); }, options);
    part.close();
    part.commit();
    return notes;
}

std::vector<std::filesystem::path> export_pcraft(const core::Document& doc, const std::filesystem::path& out, bool file_given, std::optional<int> dpi,
                                                 std::string_view stem, std::vector<std::string>* notes) {
    if (doc.pages.empty()) throw core::Error("value", "the book has no pages");
    const std::size_t count = file_given ? 1 : doc.pages.size();
    if (!file_given) detail::make_dirs(out);
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> said;
    detail::Output output;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string named = stem.empty() ? formats::stem(doc) : std::string(stem);  // (as the PSD's pages: <title>_ep01_p001)
        const std::filesystem::path dest = file_given ? out : detail::join(out, named + "_p" + detail::padded(doc.page(i).index, 3) + ".pcraft");
        if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
        detail::PartFile part(dest);
        PcraftOptions options;
        options.dpi = dpi;
        for (std::string& note : write_pcraft(doc, i, [&part](std::string_view bytes) { part.write(bytes); }, options)) {
            if (std::find(said.begin(), said.end(), note) == said.end()) said.push_back(std::move(note));
        }
        part.close();
        output.put(std::move(part));
        paths.push_back(dest);
    }
    output.commit();
    if (notes != nullptr) *notes = std::move(said);
    return paths;
}

}  // namespace genko::formats
