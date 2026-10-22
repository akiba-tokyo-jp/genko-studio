// コピーを保存… (formats/copy.hpp).

#include "formats/copy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/open_file.hpp"
#include "formats/output.hpp"
#include "formats/page_picture.hpp"
#include "formats/pillow_save.hpp"
#include "render/codecs/codecs.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::formats {

namespace {

namespace fs = std::filesystem;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

constexpr std::array<std::string_view, 5> kExtensions{"psd", "tif", "png", "jpg", "jpeg"};

std::string lower_suffix(const fs::path& dest) {
    std::string suffix = dest.extension().string();
    if (!suffix.empty() && suffix[0] == '.') suffix.erase(0, 1);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix == "tiff" ? std::string("tif") : suffix;
}

// The page's TIFF at its depth (the exports' rule: an RGB page's values, a grey or CMYK page's inks, an 8-bit picture
// otherwise), its profile and the book's XMP.
std::string tiff_of(const PagePicture& picture, int dpi, const std::optional<std::string>& xmp) {
    const CodecImage& image = picture.image;
    TiffSave save{"tiff_lzw", std::array<double, 2>{double(dpi), double(dpi)}, image.icc.value_or(std::string())};
    if (xmp) save.xmp = *xmp;
    const std::uint32_t w = image.width(), h = image.height();
    const unsigned all = render::codecs::channels(image.layout());
    if (render::codecs::is_rgb(image.layout()) && image.sample() != Sample::U8) {
        const std::string values = core::encode_color_pixels(w, h, image.sample() == Sample::U16 ? "u16" : "f32", [&](std::size_t i) {
            return std::array<double, 4>{image.value(i * all), image.value(i * all + 1), image.value(i * all + 2), 1.0};
        });
        return values_tiff_bytes(core::ColorRasterView(values), image.sample() == Sample::U16 ? 16 : 32, save);
    }
    if (render::codecs::is_gray(image.layout()) || render::codecs::is_cmyk(image.layout())) {
        const bool cmyk = render::codecs::is_cmyk(image.layout());
        const int depth = render::codecs::depth_of(image.sample());
        core::InkPixels inks = core::blank_ink_pixels(cmyk ? core::InkSpace::Cmyk : core::InkSpace::Gray, depth, w, h);
        const unsigned colours = render::codecs::colour_channels(image.layout());
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            for (unsigned c = 0; c < colours; ++c) inks.set(i, c, image.value(i * all + c));
            inks.set(i, colours, 1.0);
        }
        return ink_tiff_bytes(inks, save);
    }
    const CodecImage rgb = image.convert(Layout::Rgb, Sample::U8);
    return tiff_bytes(render::Image::frombytes("RGB", render::Size{static_cast<int>(w), static_cast<int>(h)}, rgb.data()), save);
}

}  // namespace

std::span<const std::string_view> copy_extensions() { return kExtensions; }

int jpeg_quality_of(int photoshop) { return static_cast<int>(std::lround(std::clamp(photoshop, 0, 12) / 12.0 * 99.0 + 1.0)); }

PlannedCopy plan_copy(const core::Document& doc, std::size_t at, const fs::path& dest, const CopyOptions& options) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const std::string suffix = lower_suffix(dest);
    if (std::find(kExtensions.begin(), kExtensions.end(), suffix) == kExtensions.end()) throw render::NotYetPorted("image_write");
    const core::Page& page = doc.page(at);
    const int dpi = std::max(1, static_cast<int>(std::lround(page.spec.dpi.value())));
    check_dpi(dpi);
    PlannedCopy plan;
    plan.file = dest;
    plan.dpi = dpi;
    if (suffix == "psd") {
        if (options.layers) {
            plan.psd_book = doc;  // (the book's XMP in it: put_page_psd)
            plan.psd_page = at;
            return plan;
        }
        // (its merged picture alone: a book of it, the File Info carried over)
        Opened merged = duplicate_page(doc, at, doc.title, true);
        merged.doc.file_info = doc.file_info;
        merged.doc.native_files = doc.native_files;
        merged.doc.asset_dir = doc.asset_dir;
        merged.doc.title = doc.title;
        plan.notes = std::move(merged.notes);
        plan.psd_book = std::move(merged.doc);
        plan.psd_page = 0;
        return plan;
    }
    const std::optional<std::string> xmp = book_xmp(doc);
    PagePicture picture = page_picture(page, doc, dpi);
    plan.notes = picture.notes;
    if (suffix == "tif") {
        plan.bytes = tiff_of(picture, dpi, xmp);
    } else {
        const render::codecs::Format format = suffix == "png" ? render::codecs::Format::Png : render::codecs::Format::Jpeg;
        render::codecs::EncodeOptions encode;
        encode.jpeg_quality = jpeg_quality_of(options.quality.value_or(10));
        for (const render::codecs::Loss& loss : render::codecs::losses(picture.image, format, encode)) {
            if (!loss.fatal()) plan.notes.push_back(loss.text());
        }
        plan.bytes = render::codecs::encode(picture.image, format, encode);
        if (xmp) plan.bytes = format == render::codecs::Format::Png ? png_with_xmp(std::move(plan.bytes), *xmp) : jpeg_with_xmp(std::move(plan.bytes), *xmp);
    }
    return plan;
}

CopyResult write_planned(const PlannedCopy& plan) {
    CopyResult result;
    result.file = plan.file;
    result.notes = plan.notes;
    if (plan.file.has_parent_path()) detail::make_dirs(plan.file.parent_path());
    if (plan.psd_book) {
        export_page_psd(*plan.psd_book, plan.psd_book->page(plan.psd_page), plan.file, plan.dpi);
        return result;
    }
    detail::Output output;
    output.put(plan.file, plan.bytes);
    output.commit();
    return result;
}

CopyResult write_copy(const core::Document& doc, std::size_t at, const fs::path& dest, const CopyOptions& options) {
    return write_planned(plan_copy(doc, at, dest, options));
}

}  // namespace genko::formats
