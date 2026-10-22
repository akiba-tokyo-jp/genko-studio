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
#include "core/color_model.hpp"
#include "render/codecs/codecs.hpp"
#include "render/not_yet_ported.hpp"
#include "render/smart_decode.hpp"

namespace genko::formats {

namespace {

namespace fs = std::filesystem;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

// (PC4 F-a: and the codecs' formats, the default extension of each first)
constexpr std::array<std::string_view, 23> kExtensions{"psd", "tif",  "png", "jpg", "jpeg", "webp", "gif", "bmp", "tga", "ico", "pnm", "pbm",
                                                       "pgm", "ppm", "pam", "pfm", "qoi", "exr", "hdr", "avif", "jpe", "jfif", "dib"};

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

// (PC4 F-a) The page's picture in sRGB's colours — through its profile, Little CMS in floats, values past 1 kept
// (render/smart_decode's srgb_floats) —, for a format that does not hold its profile or its colours: RGB(A), or `grey`
// a grey picture's grey(A) (a grey's sRGB is grey); `linear` (OpenEXR, Radiance HDR: their light is linear) with sRGB's
// curve undone, as floats with the built-in linear-srgb (RGB; a grey none) — else at the picture's own depth, with no
// profile (sRGB's, as a file without one is taken).
CodecImage srgb_picture(const CodecImage& picture, bool linear, bool grey) {
    render::smart::DeepPicture deep;
    deep.width = picture.width();
    deep.height = picture.height();
    deep.channels = render::codecs::channels(picture.layout());
    deep.alpha = render::codecs::has_alpha(picture.layout());
    deep.depth = render::codecs::depth_of(picture.sample());
    deep.mode = std::string(render::codecs::to_string(picture.layout()));
    deep.samples = picture.normalized();
    const std::vector<float> rgba = render::smart::srgb_floats(deep, picture.icc, "relative", true);
    const auto linear_of = [](float v) {
        if (std::isnan(v)) return 0.0f;
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    grey = grey && render::codecs::is_gray(picture.layout());
    const unsigned colours = grey ? 1 : 3;
    std::vector<float> values;
    values.reserve(rgba.size() / 4 * (colours + (deep.alpha ? 1 : 0)));
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        std::array<float, 3> c{rgba[i], rgba[i + 1], rgba[i + 2]};
        if (linear) {
            for (float& v : c) v = linear_of(v);
        }
        if (grey) values.push_back(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
        else values.insert(values.end(), c.begin(), c.end());
        if (deep.alpha) values.push_back(rgba[i + 3]);
    }
    const Layout layout = grey ? (deep.alpha ? Layout::GrayA : Layout::Gray) : (deep.alpha ? Layout::Rgba : Layout::Rgb);
    CodecImage out = CodecImage::from_normalized(picture.width(), picture.height(), layout, linear ? Sample::F32 : picture.sample(), values);
    out.meta = picture.meta;
    if (linear && !grey) {
        if (const core::BuiltinProfile* p = core::builtin_profile("linear-srgb"); p != nullptr && p->bytes) out.icc = *p->bytes;
    }
    return out;
}

// The page's picture already in the linear-srgb an opened OpenEXR or Radiance HDR has (its RGB): written as it is.
bool linear_srgb_already(const CodecImage& picture) {
    const core::BuiltinProfile* p = core::builtin_profile("linear-srgb");
    return render::codecs::is_rgb(picture.layout()) && picture.icc && p != nullptr && p->bytes && *picture.icc == *p->bytes;
}

// Netpbm's kind as the extension names it (render/codecs/options.hpp).
render::codecs::PnmKind pnm_kind_of(std::string_view suffix) {
    using render::codecs::PnmKind;
    if (suffix == "pbm") return PnmKind::Bitmap;
    if (suffix == "pgm") return PnmKind::Graymap;
    if (suffix == "ppm") return PnmKind::Pixmap;
    if (suffix == "pam") return PnmKind::Arbitrary;
    if (suffix == "pfm") return PnmKind::Float;
    return PnmKind::Any;
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
        // (PC4 F-a: every other picture through the codecs, the book's XMP and the page's profile and resolution in the
        // file where its format holds them — what it does not, said in the notes, of the page's picture as it is)
        using render::codecs::Loss;
        const render::codecs::Format format = *render::codecs::from_extension(suffix);
        render::codecs::EncodeOptions encode;
        encode.jpeg_quality = jpeg_quality_of(options.quality.value_or(10));
        encode.pnm_kind = pnm_kind_of(suffix);
        CodecImage image = std::move(picture.image);
        if (xmp) image.meta.xmp = *xmp;
        std::vector<Loss> said = render::codecs::losses(image, format, encode);
        const auto has = [&](Loss::Kind kind) { return std::any_of(said.begin(), said.end(), [kind](const Loss& l) { return l.kind == kind; }); };
        // (its colours through its profile when the file cannot hold them: OpenEXR's and Radiance HDR's linear light —
        // the linear-srgb of an opened one's page as it is —; a format that holds neither the profile nor CMYK, sRGB's
        // colours — not the profile dropped, nor CMYK's plain formula —)
        const bool grey = render::codecs::is_gray(render::codecs::plan(image, format, encode).layout);
        bool converted = false;
        if (render::codecs::linear_light(format)) {
            if (!linear_srgb_already(image)) {
                image = srgb_picture(image, true, grey);
                converted = true;
            }
        } else if (image.icc && !image.icc->empty() && (has(Loss::Kind::IccDropped) || has(Loss::Kind::CmykConverted))) {
            image = srgb_picture(image, false, grey);
            converted = true;
        }
        if (converted) said.erase(std::remove_if(said.begin(), said.end(), [](const Loss& l) { return l.kind == Loss::Kind::IccDropped; }), said.end());
        for (const Loss& loss : said) {
            if (!loss.fatal()) plan.notes.push_back(loss.text());
        }
        plan.bytes = render::codecs::encode(image, format, encode);
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
