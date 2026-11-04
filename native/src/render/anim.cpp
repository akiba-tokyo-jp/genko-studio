#include "render/anim.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/video.hpp"
#include "render/movie.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

namespace genko::render::anim {

namespace {

using core::Json;

// anim._tinted: the picture's alpha, weakened by `strength`, in one colour.
Image tinted(const Image& image, const std::array<int, 3>& rgb, double strength) {
    const Image alpha = image.getchannel("A").point([strength](int v) { return static_cast<int>(v * strength); });
    Image out = Image::create("RGBA", image.size(), Ink{rgb[0], rgb[1], rgb[2], 0});
    out.putalpha(alpha);
    return out;
}

// export.crop_to: a page picture cut to the paper, the bleed or the finished size.
Image crop_to(const Image& image, const core::Page& page, std::string_view area, int dpi) {
    if (area == "paper") return image;
    const core::Rect r = area == "bleed" ? page.bleed_rect_mm() : page.trim_rect_mm();
    const int x0 = edge_px(r.x.value(), dpi), y0 = edge_px(r.y.value(), dpi);
    return image.crop(Box{x0, y0, x0 + mm_to_px(r.width.value(), dpi), y0 + mm_to_px(r.height.value(), dpi)});
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

std::optional<Image> onion(const core::Page& page, std::int64_t frame, int dpi, std::int64_t before, std::int64_t after,
                           double strength, const core::Document* episode) {
    const Json* data = core::anim::spec(page);
    if (data == nullptr) return std::nullopt;
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    Image out = Image::create("RGBA", size, Ink{0, 0, 0, 0});
    const bool loop = core::anim::loops(page);
    const Json tracks = core::py_get(*data, "tracks", Json::array());
    std::vector<Json> shown_now;
    for (const Json& t : core::iterate(tracks)) shown_now.push_back(core::anim::cel_at(page, core::py_get(t, "folder"), frame));
    std::map<std::string, const core::Layer*> layers;
    for (const core::Layer& layer : page.layers) layers[layer.id] = &layer;
    const auto layer_of = [&](const Json& cel) -> const core::Layer* {
        if (!cel.is_string()) return nullptr;
        const auto found = layers.find(cel.get<std::string>());
        return found != layers.end() ? found->second : nullptr;
    };
    bool used = false;
    for (const Json& entry : core::iterate(tracks)) {
        const Json folder = core::py_get(entry, "folder");
        for (const auto& [direction, rgb, how_many] : {std::tuple{-1, kBeforeRgb, before}, std::tuple{1, kAfterRgb, after}}) {
            if (how_many <= 0) continue;
            int step = 0;
            for (const Json& cel : core::anim::neighbours(page, folder, frame, direction, how_many, loop)) {
                ++step;
                if (const core::Layer* layer = layer_of(cel)) {
                    out = alpha_composite(out, tinted(cel_image(page, *layer, dpi, episode), rgb, strength / step));
                    used = true;
                }
            }
        }
    }
    for (const Json& cel : core::iterate(core::py_or(core::py_get(*data, "light_table"), Json::array()))) {
        const core::Layer* layer = layer_of(cel);
        if (layer == nullptr) continue;
        if (std::any_of(shown_now.begin(), shown_now.end(), [&](const Json& now) { return core::py_equals(now, cel); })) continue;
        out = alpha_composite(out, tinted(cel_image(page, *layer, dpi, episode), kLightRgb, strength));
        used = true;
    }
    if (!used) return std::nullopt;
    return out;
}

Image with_onion(const Image& image, const core::Page& page, std::int64_t frame, int dpi, const core::Document* episode,
                 std::int64_t before, std::int64_t after, double strength) {
    std::optional<Image> ghost = onion(page, frame, dpi, before, after, strength, episode);
    if (!ghost) return image;
    const Image base = image.convert("RGBA");
    if (ghost->size() != base.size()) ghost = ghost->resize(base.size());
    const std::string_view mode = image.mode();
    return alpha_composite(base, *ghost).convert(mode == "RGB" || mode == "RGBA" ? mode : std::string_view("RGB"));
}

Image render_frame(const core::Page& page, std::int64_t frame, int dpi, const core::Document* episode, bool camera,
                   std::string_view area, std::optional<Size> size, bool skip_unported) {
    RenderOptions options;
    options.mode = "print";
    options.finish = false;
    options.at_frame = true;  // (the page as it is at that frame: anim.at_frame's copy)
    options.skip_unported = skip_unported;
    Image image = render_page(core::anim::at_frame(page, frame), dpi, options, episode).image.convert("RGB");
    const auto rect = camera ? core::anim::camera_at(page, frame) : std::nullopt;
    if (rect) {
        if (rect->size() < 4) throw core::PyValueError("not enough values to unpack (expected 4, got " + std::to_string(rect->size()) + ")");
        if (rect->size() > 4) throw core::PyValueError("too many values to unpack (expected 4)");
        const double x = (*rect)[0], y = (*rect)[1], w = (*rect)[2], h = (*rect)[3];
        image = image.crop(Box{mm_to_px(x, dpi), mm_to_px(y, dpi), mm_to_px(x + w, dpi), mm_to_px(y + h, dpi)});
    } else {
        image = crop_to(image, page, area, dpi);
    }
    if (size && image.size() != *size) image = image.resize(*size, Resample::Lanczos);
    return image;
}

std::vector<std::filesystem::path> export_animation(const core::Page& page, const std::filesystem::path& dest,
                                                    const core::Document* episode, int dpi, std::optional<std::string> fmt,
                                                    bool camera, std::string_view area, std::optional<int> width,
                                                    std::optional<std::array<std::int64_t, 2>> range, std::optional<double> fps,
                                                    const std::function<bool(std::int64_t, std::int64_t)>& progress) {
    if (!core::anim::is_animation(page)) throw core::PyValueError("the page is not an animation (set_animation first)");
    std::string format = fmt.value_or("");
    if (format.empty()) {
        const std::string suffix = core::path_to_utf8(dest.extension());
        format = suffix.size() > 1 ? suffix.substr(1) : std::string("gif");
    }
    format = lower(format);
    if (std::find(kFormats.begin(), kFormats.end(), format) == kFormats.end()) {
        throw core::PyValueError("format must be one of gif, webp, png, mp4, frames");
    }
    const std::int64_t count = core::anim::frames_of(page);
    // (PC5 J-e1: the frames asked for and the speed, else all of them at the page's — as before)
    const std::int64_t from = range ? (*range)[0] : 1, to = range ? (*range)[1] : count;
    if (from < 1 || to > count || from > to) {
        throw core::PyValueError("range is [first, last] of the frames 1 to " + std::to_string(count));
    }
    if (fps && !(core::kTimelineFpsMin <= *fps && *fps <= core::kTimelineFpsMax)) throw core::PyValueError("fps is 0.5 to 120");
    // (PC5 J-e1: each frame written told, and given up when asked — nothing left written)
    const std::int64_t total = to - from + 1;
    const auto step = [&progress, total](std::int64_t done) {
        if (progress && !progress(done, total)) throw core::Error("cancelled", "the video was not rendered (cancelled)");
    };
    Image first = render_frame(page, from, dpi, episode, camera, area);
    Size size = first.size();
    if (width && *width != 0) {
        // (PC5 J-e1: a frame no larger than an op's picture — a narrow camera made wide would take the machine's memory)
        const double tall = static_cast<double>(first.height()) * *width / first.width();
        try {
            limits::check_sides(static_cast<double>(*width), std::max(1.0, tall), "the video's frame");
        } catch (const core::OpError& error) {  // (a writing's refusal, as a picture too large to write is refused)
            throw core::Error("image_too_large", error.what());
        }
        size = Size{*width, static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(tall)))};
    }
    if (first.size() != size) first = first.resize(size, Resample::Lanczos);
    if (format == "frames") {
        // (each file written beside its name and renamed only when all are drawn: a failed drawing leaves the folder as
        // it was; a failure while renaming — rare, in one folder — may leave some new frames beside old ones)
        std::error_code error;
        std::filesystem::create_directories(dest, error);
        if (error) throw core::Error("io", "cannot make the folder: " + core::path_to_utf8(dest));
        std::vector<std::filesystem::path> parts;
        std::vector<std::filesystem::path> files;
        try {
            for (std::int64_t f = from; f <= to; ++f) {
                const Image picture = f == from ? first : render_frame(page, f, dpi, episode, camera, area, size);
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%04lld.png", static_cast<long long>(f - from + 1));
                files.push_back(dest / name);
                parts.push_back(dest / (std::string(".") + name + ".part"));
                save_png(picture, parts.back());
                step(f - from + 1);
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
                std::filesystem::rename(parts[i], files[i], error);
                if (error) throw core::Error("io", "cannot write: " + core::path_to_utf8(files[i]));
            }
        } catch (...) {
            for (const auto& part : parts) std::filesystem::remove(part, error);
            throw;
        }
        return files;
    }
    std::filesystem::path out = dest;
    out.replace_extension("." + format);
    movie::Writer writer(out, fps.value_or(core::anim::fps_of(page)), format, 0.0, core::anim::loops(page));
    writer.add(first);
    step(1);
    for (std::int64_t f = from + 1; f <= to; ++f) {
        writer.add(render_frame(page, f, dpi, episode, camera, area, size));
        step(f - from + 1);
    }
    return {writer.finish()};
}

}  // namespace genko::render::anim
