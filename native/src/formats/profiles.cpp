// Screen outputs (Python's genko/profiles.py): webtoon strips and SNS pictures, in colour or grey, never screened.
// The pages are trimmed (the bleed cut off), drawn without the monochrome print finish (no dots: tones are flat grey)
// and tagged sRGB.

#include <algorithm>
#include <map>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "formats/export.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "render/colour.hpp"
#include "render/page.hpp"

namespace genko::formats {

using render::Image;

namespace {

// profiles._save: the picture as RGB, at path with the suffix .jpg (fmt "jpeg": quality, optimized) or .png
// (optimized), with the sRGB profile (made anew for each file, as Python's _srgb is).
fs::path save(detail::Output& output, const Image& image, const fs::path& path, std::string_view fmt, int quality) {
    const std::string icc = render::colour::srgb_icc();
    const Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    if (fmt == "jpeg") {
        const fs::path file = detail::with_suffix(path, ".jpg");
        output.put(file, jpeg_bytes(rgb, JpegSave{quality, true, icc}));
        return file;
    }
    const fs::path file = detail::with_suffix(path, ".png");
    output.put(file, png_bytes(rgb, PngSave{std::nullopt, icc, true}));
    return file;
}

// profiles._dpi_for_width
int dpi_for_width(const core::Page& page, int width_px) {
    const double trim_w = page.spec.trim_size().first.value();
    return static_cast<int>(std::max<std::int64_t>(36, core::py_round_int(width_px / (trim_w / 25.4))));
}

}  // namespace

Image trimmed(const core::Page& page, const core::Document& episode, int dpi) {
    render::RenderOptions options;
    options.mode = "print";
    options.finish = false;
    const Image image = render::render_page(page, dpi, options, &episode).image;
    const core::Rect trim = page.trim_rect_mm();
    const int x0 = render::mm_to_px(trim.x.value(), dpi);
    const int y0 = render::mm_to_px(trim.y.value(), dpi);
    return image.crop(render::Box{x0, y0, x0 + render::mm_to_px(trim.width.value(), dpi), y0 + render::mm_to_px(trim.height.value(), dpi)});
}

std::vector<fs::path> export_webtoon(const core::Document& episode, const fs::path& dest, int width_px, int max_height, int gap_px,
                                     std::string_view fmt, int quality) {
    detail::make_dirs(dest);
    // heights first (each page drawn once, at its own scale), then the strip cut into slices
    std::vector<std::int64_t> heights;
    for (const auto& page : episode.pages) {
        const auto [trim_w, trim_h] = page->spec.trim_size();
        heights.push_back(core::py_round_int((core::Num(width_px) * trim_h / trim_w).value()));
    }
    std::vector<std::int64_t> tops;
    std::int64_t y = 0;
    for (const std::int64_t h : heights) {
        tops.push_back(y);
        y += h + gap_px;
    }
    const std::int64_t total = heights.empty() ? 0 : y - gap_px;
    // (no slice a pixel high: Python's first slice fails as Pillow does — Image.new, or saving a picture of no rows)
    if (max_height < 0 && total > 0) throw core::PyValueError("Width and height must be >= 0");
    if (max_height == 0 && total > 0) throw core::PyValueError("cannot write empty image");
    detail::Output output;
    std::vector<fs::path> written;
    std::map<std::size_t, Image> cache;
    std::int64_t start = 0;
    while (start < total) {
        const std::int64_t end = std::min<std::int64_t>(total, start + max_height);
        Image slice = Image::create("RGB", render::Size{width_px, static_cast<int>(end - start)}, render::Ink{255, 255, 255});
        for (std::size_t i = 0; i < heights.size(); ++i) {
            if (tops[i] >= end || tops[i] + heights[i] <= start) continue;
            if (!cache.contains(i)) {
                const core::Page& page = *episode.pages[i];
                cache[i] = trimmed(page, episode, dpi_for_width(page, width_px))
                               .resize(render::Size{width_px, static_cast<int>(heights[i])}, render::Resample::Lanczos);
            }
            slice.paste(cache[i], render::Point{0, static_cast<int>(tops[i] - start)});
        }
        for (auto it = cache.begin(); it != cache.end();) {  // pages fully above the next slice are no longer needed
            it = tops[it->first] + heights[it->first] <= end ? cache.erase(it) : std::next(it);
        }
        written.push_back(save(output, slice, detail::join(dest, detail::padded(core::Num(static_cast<std::int64_t>(written.size()) + 1), 3)), fmt, quality));
        start = end;
    }
    output.commit();
    return written;
}

std::vector<fs::path> export_sns(const core::Document& episode, const fs::path& dest, int long_edge, std::string_view fmt, int quality,
                                 bool spreads) {
    detail::make_dirs(dest);
    detail::Output output;
    std::vector<fs::path> written;
    for (const auto& page : episode.pages) {
        const auto [trim_w, trim_h] = page->spec.trim_size();
        const double longest = std::max(trim_w.value(), trim_h.value());
        const int dpi = static_cast<int>(std::max<std::int64_t>(36, core::py_round_int(long_edge / (longest / 25.4))));
        Image image = trimmed(*page, episode, dpi);
        image.thumbnail(render::Size{long_edge, long_edge}, render::Resample::Lanczos);
        written.push_back(save(output, image, detail::join(dest, stem(episode) + "_p" + detail::padded(page->index, 3)), fmt, quality));
    }
    if (spreads) {
        std::vector<core::Num> done;
        const auto seen = [&](const core::Num& n) { return std::any_of(done.begin(), done.end(), [&](const core::Num& d) { return d == n; }); };
        for (const auto& page : episode.pages) {
            if (!page->spread_with || !page->spread_with->truthy() || seen(page->index)) continue;
            const core::Num partner = *page->spread_with;
            done.push_back(page->index);
            done.push_back(partner);
            core::Num first = page->index, second = partner;
            if (second.value() < first.value()) std::swap(first, second);  // sorted((page.index, partner))
            const int dpi = static_cast<int>(
                std::max<std::int64_t>(36, core::py_round_int(long_edge / (2 * page->spec.trim_size().first.value() / 25.4))));
            render::RenderOptions options;
            options.mode = "print";
            options.finish = false;
            Image image = render::render_spread(episode, first, second, dpi, options, true);
            image.thumbnail(render::Size{long_edge, long_edge}, render::Resample::Lanczos);
            written.push_back(save(output, image,
                                   detail::join(dest, stem(episode) + "_spread_" + detail::padded(first, 3) + "-" + detail::padded(second, 3)), fmt,
                                   quality));
        }
    }
    output.commit();
    return written;
}

}  // namespace genko::formats
