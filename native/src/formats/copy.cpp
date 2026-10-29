// コピーを保存… (formats/copy.hpp).

#include "formats/copy.hpp"

#include <algorithm>
#include <random>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/open_file.hpp"
#include "formats/output.hpp"
#include "formats/page_picture.hpp"
#include "formats/pcraft_write.hpp"
#include "formats/pillow_save.hpp"
#include "formats/smart_files.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/color_model.hpp"
#include "render/codecs/codecs.hpp"
#include "render/not_yet_ported.hpp"
#include "render/smart_decode.hpp"
#include "storage/asset_store.hpp"
#include "core/file_info.hpp"
#include "core/covers.hpp"
#include "core/paths.hpp"
#include "render/codecs/metadata.hpp"
#include "render/codecs/resize.hpp"
#include "render/op_limits.hpp"
#include "render/ops_layer_comps.hpp"

namespace genko::formats {

namespace {

namespace fs = std::filesystem;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

// (PC4 F-a: and the codecs' formats, the default extension of each first; PC4 F-d: and a PSB; PC4 F-f: PhotoCraft's .pcraft)
constexpr std::array<std::string_view, 25> kExtensions{"psd", "tif",  "png", "jpg", "jpeg", "webp", "gif", "bmp", "tga", "ico", "pnm", "pbm", "pgm",
                                                       "ppm", "pam", "pfm", "qoi", "exr", "hdr", "avif", "jpe", "jfif", "dib", "psb", "pcraft"};

std::string lower_suffix(const fs::path& dest) {
    std::string suffix = dest.extension().string();
    if (!suffix.empty() && suffix[0] == '.') suffix.erase(0, 1);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix == "tiff" ? std::string("tif") : suffix;
}

// The page's TIFF at its depth (the exports' rule: an RGB page's values, a grey or CMYK page's inks, an 8-bit picture
// otherwise), its profile and the book's XMP.
std::string tiff_of(const CodecImage& image, double dpi, const std::optional<std::string>& xmp) {
    TiffSave save{"tiff_lzw", std::array<double, 2>{dpi, dpi}, image.icc.value_or(std::string())};
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

// --- PC4 F-c: the picture at its size, its layer alone, its metadata ---

constexpr double kScaleMin = 1.0;
constexpr double kScaleMax = 1000.0;

int page_dpi(const core::Page& page) { return std::max(1, static_cast<int>(std::lround(page.spec.dpi.value()))); }

void check_options(const CopyOptions& options) {
    if (!std::isfinite(options.scale) || options.scale < kScaleMin || options.scale > kScaleMax) {
        throw core::Error("value", "the scale is 1 to 1000 percent");
    }
    for (const auto& side : {options.width, options.height}) {
        if (side && (*side == 0 || *side > static_cast<std::uint32_t>(render::limits::kSide))) throw core::Error("value", "a side is 1 to 100000 pixels");
    }
    if (options.quality && (*options.quality < 0 || *options.quality > 12)) throw core::Error("value", "the quality is 0 to 12");
    if (options.quality_percent && (*options.quality_percent < 1 || *options.quality_percent > 100)) throw core::Error("value", "the quality is 1 to 100");
    if (std::find(kAreas.begin(), kAreas.end(), options.area) == kAreas.end()) throw core::Error("value", "the area is paper, bleed or trim");
}

bool sized(const CopyOptions& options) { return options.scale != 100.0 || options.width || options.height; }

// The bytes a pixel of the page's picture takes at its depth (four samples: a layer's alpha too), and the writer's work
// beside it: what is checked against the budget before it is drawn.
std::uint64_t pixel_bytes(const core::Page& page) {
    int depth = 8;
    if (page.color) depth = page.color->depth;
    else if (page.rgb) depth = page.rgb->depth;
    const std::uint64_t sample = depth == 32 ? 4 : depth == 16 ? 2 : 1;
    return 4 * sample * 2;
}

void check_size(const core::Page& page, std::uint64_t w, std::uint64_t h) {
    // (the codecs' limits — a picture written can be read back —, said as what is written: before anything is drawn)
    const std::uint64_t pixels = w * h;
    if (w > std::uint64_t(render::limits::kSide) || h > std::uint64_t(render::limits::kSide) || pixels > std::uint64_t(render::limits::kPixels) ||
        pixels > render::codecs::kAllocBudget / pixel_bytes(page)) {
        throw core::Error("image_too_large", "the picture to write is too large (" + std::to_string(w) + "×" + std::to_string(h) +
                                                 " pixels; at most 120000000 pixels, 100000 a side and 2 GiB of work)");
    }
}

const core::Layer& layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) {
            if (layer.kind == core::LayerKind::Folder) throw core::Error("value", "a folder is written by its layers: choose one of them");
            return layer;
        }
    }
    throw core::Error("value", "no such layer: " + id);
}

// The part of a picture with an alpha that shows (alpha above 0), or none.
std::optional<std::array<std::uint32_t, 4>> shown_box(const CodecImage& image) {
    const unsigned all = render::codecs::channels(image.layout());
    std::uint32_t x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            if (image.value((std::size_t(y) * image.width() + x) * all + all - 1) <= 0.0f) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return std::array<std::uint32_t, 4>{x0, y0, x1 - x0, y1 - y0};
}

// A picture with an alpha laid over white (no ink for CMYK), its alpha gone — row by row.
CodecImage over_white(const CodecImage& image) {
    const Layout to = render::codecs::without_alpha(image.layout());
    const unsigned all = render::codecs::channels(image.layout()), colours = render::codecs::colour_channels(image.layout());
    const bool cmyk = render::codecs::is_cmyk(image.layout());
    CodecImage out = CodecImage::blank(image.width(), image.height(), to, image.sample());
    out.icc = image.icc;
    out.meta = image.meta;
    const std::size_t row_bytes = std::size_t(image.width()) * colours * render::codecs::sample_bytes(image.sample());
    std::vector<float> row(std::size_t(image.width()) * colours);
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            const std::size_t at = (std::size_t(y) * image.width() + x) * all;
            const float a = std::clamp(image.value(at + all - 1), 0.0f, 1.0f);
            for (unsigned c = 0; c < colours; ++c) row[std::size_t(x) * colours + c] = image.value(at + c) * a + (cmyk ? 0.0f : 1.0f - a);
        }
        const std::string stored = render::codecs::quantize(row, image.sample());
        std::memcpy(out.data().data() + std::size_t(y) * row_bytes, stored.data(), row_bytes);
    }
    return out;
}

void put_metadata(CodecImage& image, const core::Document& doc, const core::Page& page, const std::optional<Metadata>& metadata);

}  // namespace

std::optional<std::string> copy_exif(const core::Document& doc, const core::Page& page, std::uint32_t width, std::uint32_t height) {
    if (!page.native_source || page.native_source->exif.empty()) return std::nullopt;
    const std::string& ref = page.native_source->exif;
    std::optional<std::string> bytes;
    if (const auto it = doc.native_files.find(ref); it != doc.native_files.end() && it->second) {
        bytes = *it->second;
    } else if (doc.asset_dir) {
        try {
            bytes = storage::AssetStore(*doc.asset_dir).get_bytes(ref, core::kExifSuffix);
        } catch (const std::exception&) {
            bytes.reset();  // (not to be read: none written)
        }
    }
    if (!bytes) return std::nullopt;
    // (the pixels are upright: never turned twice; PC4 F-c's review P2-3: the camera's thumbnail — the picture before it
    // was edited — gone, the sizes it says the written picture's)
    return render::codecs::exif_for_copy(render::codecs::exif_upright(std::move(*bytes)), width, height);
}

namespace {

void put_metadata(CodecImage& image, const core::Document& doc, const core::Page& page, const std::optional<Metadata>& metadata) {
    image.meta.xmp.reset();
    image.meta.exif.reset();
    if (!metadata || *metadata == Metadata::All) {
        if (auto xmp = book_xmp(doc)) image.meta.xmp = std::move(*xmp);
        if (metadata) image.meta.exif = copy_exif(doc, page, image.width(), image.height());
        return;
    }
    if (*metadata == Metadata::Copyright && doc.file_info) {
        core::FileInfo kept;
        kept.author = doc.file_info->author;
        kept.author_title = doc.file_info->author_title;
        kept.copyright = doc.file_info->copyright;
        kept.copyright_status = doc.file_info->copyright_status;
        kept.copyright_url = doc.file_info->copyright_url;
        if (!kept.empty()) image.meta.xmp = core::write_xmp(std::nullopt, kept, std::nullopt);
    }
}

// The image resources of an opened PSD that say who made it or of the shot (IPTC, the copyright flag and URL, thumbnails,
// EXIF, XMP, the caption digest): the ones a PSD's or a .pcraft's copy without all its metadata leaves out.
bool metadata_resource(int id) { return id == 1028 || id == 1033 || id == 1034 || id == 1035 || id == 1036 || (id >= 1058 && id <= 1061); }

bool trims(const std::optional<Metadata>& metadata) { return metadata && *metadata != Metadata::All; }

// (PC4 F-c's review P2-2) A PSD's, a PSB's or a .pcraft's book with only the metadata chosen: none — no File Info, no
// packet it was opened with, no EXIF, none of an opened PSD's resources of the kind —; the copyright and contact — those
// five items of File Info alone, in a packet of their own —; all (or none chosen): as it is.
void trim_metadata(core::Document& book, std::size_t at, const std::optional<Metadata>& metadata) {
    if (!trims(metadata)) return;
    if (book.file_info) {
        core::FileInfo kept;
        if (*metadata == Metadata::Copyright) {
            kept.author = book.file_info->author;
            kept.author_title = book.file_info->author_title;
            kept.copyright = book.file_info->copyright;
            kept.copyright_status = book.file_info->copyright_status;
            kept.copyright_url = book.file_info->copyright_url;
        }
        if (kept.empty()) book.file_info.reset();
        else book.file_info = std::move(kept);
    }
    if (at >= book.pages.size() || book.is_deferred(at)) return;
    const core::Page& seen = book.page(at);
    const bool exif = seen.native_source && !seen.native_source->exif.empty();
    const bool resources = seen.native_psd && std::any_of(seen.native_psd->resources.begin(), seen.native_psd->resources.end(),
                                                          [](const core::PsdResource& r) { return metadata_resource(r.id); });
    if (!exif && !resources) return;
    core::Page& page = book.edit_page(at);
    if (exif) page.native_source->exif.clear();
    if (resources) std::erase_if(page.native_psd->resources, [](const core::PsdResource& r) { return metadata_resource(r.id); });
}

}  // namespace

std::optional<Metadata> metadata_of(std::string_view id) {
    if (id == "none") return Metadata::None;
    if (id == "copyright") return Metadata::Copyright;
    if (id == "all") return Metadata::All;
    return std::nullopt;
}

std::string_view to_string(Metadata metadata) {
    switch (metadata) {
    case Metadata::None: return "none";
    case Metadata::Copyright: return "copyright";
    case Metadata::All: return "all";
    }
    return "copyright";
}

std::span<const std::string_view> copy_extensions() { return kExtensions; }

int jpeg_quality_of(int photoshop) { return static_cast<int>(std::lround(std::clamp(photoshop, 0, 12) / 12.0 * 99.0 + 1.0)); }

CodecImage srgb_picture(const CodecImage& picture, bool linear, bool grey) {
    grey = grey && render::codecs::is_gray(picture.layout());
    const bool alpha = render::codecs::has_alpha(picture.layout());
    const unsigned all = render::codecs::channels(picture.layout());
    const unsigned colours = grey ? 1 : 3;
    const unsigned keep = colours + (alpha ? 1 : 0);
    const Layout layout = grey ? (alpha ? Layout::GrayA : Layout::Gray) : (alpha ? Layout::Rgba : Layout::Rgb);
    const Sample sample = linear ? Sample::F32 : picture.sample();
    CodecImage out = CodecImage::blank(picture.width(), picture.height(), layout, sample);
    out.meta = picture.meta;
    if (linear && !grey) {
        if (const core::BuiltinProfile* p = core::builtin_profile("linear-srgb"); p != nullptr && p->bytes) out.icc = *p->bytes;
    }
    const auto linear_of = [](float v) {
        if (std::isnan(v)) return 0.0f;
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    // (a band of rows at a time — about a million pixels —: Little CMS once a band, the picture never held again whole
    // as floats; the same values as the whole picture at once)
    const std::uint32_t w = picture.width();
    const std::uint32_t band = std::max<std::uint32_t>(1, static_cast<std::uint32_t>((std::size_t(1) << 20) / std::max<std::uint32_t>(1, w)));
    const std::size_t row_bytes = std::size_t(w) * keep * render::codecs::sample_bytes(sample);
    render::smart::DeepPicture deep;
    deep.width = w;
    deep.channels = all;
    deep.alpha = alpha;
    deep.depth = render::codecs::depth_of(picture.sample());
    deep.mode = std::string(render::codecs::to_string(picture.layout()));
    std::vector<float> values;
    for (std::uint32_t y0 = 0; y0 < picture.height(); y0 += band) {
        const std::uint32_t rows = std::min(band, picture.height() - y0);
        deep.height = rows;
        deep.samples.resize(std::size_t(w) * rows * all);
        const std::size_t first = std::size_t(y0) * w * all;
        for (std::size_t i = 0; i < deep.samples.size(); ++i) deep.samples[i] = picture.value(first + i);
        const std::vector<float> rgba = render::smart::srgb_floats(deep, picture.icc, "relative", true);
        values.clear();
        values.reserve(std::size_t(w) * rows * keep);
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            std::array<float, 3> c{rgba[i], rgba[i + 1], rgba[i + 2]};
            if (linear) {
                for (float& v : c) v = linear_of(v);
            }
            if (grey) values.push_back(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
            else values.insert(values.end(), c.begin(), c.end());
            if (alpha) values.push_back(rgba[i + 3]);
        }
        const std::string stored = render::codecs::quantize(values, sample);
        std::memcpy(out.data().data() + std::size_t(y0) * row_bytes, stored.data(), std::size_t(rows) * row_bytes);
    }
    return out;
}

std::string encode_for(CodecImage image, render::codecs::Format format, const render::codecs::EncodeOptions& encode, bool srgb,
                       std::vector<std::string>& notes) {
    // (PC4 F-a: the book's XMP and the page's profile and resolution in the file where its format holds them — what it
    // does not, said in the notes, of the page's picture as it is)
    using render::codecs::Loss;
    std::vector<Loss> said = render::codecs::losses(image, format, encode);
    const auto has = [&](Loss::Kind kind) { return std::any_of(said.begin(), said.end(), [kind](const Loss& l) { return l.kind == kind; }); };
    // (its colours through its profile when the file cannot hold them — or, PC4 F-c, when sRGB's are asked —: OpenEXR's
    // and Radiance HDR's linear light — the linear-srgb of an opened one's page as it is —; a format that holds neither the
    // profile nor CMYK, sRGB's colours — not the profile dropped, nor CMYK's plain formula —; the same way for every format)
    const bool grey = render::codecs::is_gray(render::codecs::plan(image, format, encode).layout);
    const bool profiled = image.icc && !image.icc->empty();
    bool converted = false;
    if (render::codecs::linear_light(format)) {
        if (!linear_srgb_already(image)) {
            image = srgb_picture(image, true, grey);
            converted = true;
        }
    } else if ((srgb && (profiled || render::codecs::is_cmyk(image.layout()))) || (profiled && (has(Loss::Kind::IccDropped) || has(Loss::Kind::CmykConverted)))) {
        image = srgb_picture(image, false, grey);
        converted = true;
    }
    if (converted) said.erase(std::remove_if(said.begin(), said.end(), [](const Loss& l) { return l.kind == Loss::Kind::IccDropped; }), said.end());
    for (const Loss& loss : said) {
        if (!loss.fatal()) notes.push_back(loss.text());
    }
    return render::codecs::encode(image, format, encode);
}

render::codecs::EncodeOptions encode_options(const CopyOptions& options, std::string_view extension) {
    render::codecs::EncodeOptions encode;
    encode.jpeg_quality = options.quality_percent ? std::clamp(*options.quality_percent, 1, 100) : jpeg_quality_of(options.quality.value_or(10));
    encode.pnm_kind = pnm_kind_of(extension);
    if (extension == "webp" && (options.quality_percent || options.quality)) encode.webp_lossless = false;  // (PC4 F-c: a quality asked)
    return encode;
}

CopySize copy_size(const core::Document& doc, std::size_t at, const CopyOptions& options) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    check_options(options);
    const core::Page& page = doc.page(at);
    const int dpi = page_dpi(page);
    const auto [w100, h100] = page_picture_size(page, dpi, options.area);
    double s = options.scale / 100.0;
    std::uint32_t w = 0, h = 0;
    const auto round_side = [](double v) { return static_cast<std::uint32_t>(std::clamp<long long>(std::llround(v), 1LL, render::limits::kSide)); };
    if (options.width && options.height) {
        w = *options.width;
        h = *options.height;
        s = std::max(double(w) / w100, double(h) / h100);
    } else if (options.width) {
        w = *options.width;
        s = double(w) / w100;
        h = round_side(h100 * s);
    } else if (options.height) {
        h = *options.height;
        s = double(h) / h100;
        w = round_side(w100 * s);
    } else {
        w = round_side(w100 * s);
        h = round_side(h100 * s);
    }
    CopySize out;
    out.width = w;
    out.height = h;
    out.dpi = dpi * double(w) / double(w100);
    // (drawn at a resolution that makes that size, when one does — 100 %: the page's own —; else at the next one up, made
    // that size by the resample)
    const double want = dpi * s;
    const auto fits = [&](double d) {
        if (d < 1.0 || d > double(kMaxDpi)) return false;
        return page_picture_size(page, static_cast<int>(d), options.area) == std::make_pair(w, h);
    };
    out.drawn_dpi = 0;
    for (const double d : {std::round(want), std::floor(want), std::ceil(want)}) {
        if (fits(d)) {
            out.drawn_dpi = static_cast<int>(d);
            break;
        }
    }
    if (out.drawn_dpi == 0) out.drawn_dpi = static_cast<int>(std::clamp(std::ceil(want), 1.0, double(kMaxDpi)));
    return out;
}

PagePicture copy_picture(const core::Document& doc, std::size_t at, const CopyOptions& options) {
    const CopySize size = copy_size(doc, at, options);
    const core::Page& page = doc.page(at);
    PagePicture out;
    if (options.layer) {
        const core::Layer& layer = layer_of(page, *options.layer);
        // (drawn alone over nothing at the resolution its scale asks — a size asked exactly: at the page's own, then made
        // that size —, cut to what it holds; PhotoCraft's layer_document)
        const bool exact = options.width || options.height;
        const int dpi = exact ? page_dpi(page) : size.drawn_dpi;
        const auto [pw, ph] = page_picture_size(page, dpi, "paper");
        check_size(page, pw, ph);
        out = layer_picture(page, layer, doc, dpi);
        const auto box = shown_box(out.image);
        if (!box) throw core::Error("value", "the layer has nothing to write");
        out.image = out.image.crop((*box)[0], (*box)[1], (*box)[2], (*box)[3]);
        double file_dpi = dpi;
        if (exact) {
            const std::uint32_t bw = out.image.width(), bh = out.image.height();
            // (counted in 64 bits — a thin layer made wide can ask past 2^32 —, checked, then made that size)
            std::uint64_t w64 = options.width.value_or(0), h64 = options.height.value_or(0);
            if (!options.height) h64 = static_cast<std::uint64_t>(std::max(1LL, std::llround(double(bh) * double(w64) / bw)));
            if (!options.width) w64 = static_cast<std::uint64_t>(std::max(1LL, std::llround(double(bw) * double(h64) / bh)));
            check_size(page, w64, h64);
            const auto w = static_cast<std::uint32_t>(w64), h = static_cast<std::uint32_t>(h64);
            out.image = render::codecs::resized(out.image, w, h, options.resample);
            file_dpi = dpi * double(w) / double(bw);
        }
        if (!options.transparency) out.image = over_white(out.image);
        out.image.meta.dpi = std::make_pair(file_dpi, file_dpi);
    } else {
        const auto [dw, dh] = page_picture_size(page, size.drawn_dpi, options.area);
        check_size(page, std::max(dw, size.width), std::max(dh, size.height));  // (before it is drawn)
        out = page_picture(page, doc, size.drawn_dpi, options.area);
        if (out.image.width() != size.width || out.image.height() != size.height) {
            out.image = render::codecs::resized(out.image, size.width, size.height, options.resample);
        }
        out.image.meta.dpi = std::make_pair(size.dpi, size.dpi);
    }
    put_metadata(out.image, doc, page, options.metadata);
    return out;
}

void check_copy_size(const core::Document& doc, std::size_t at, std::string_view extension, const CopyOptions& options) {
    const CopySize size = copy_size(doc, at, options);
    const core::Page& page = doc.page(at);
    if (options.layer) {
        // (what is drawn before a layer's size is known: its page at the resolution it is drawn at; a size asked exactly)
        const bool exact = options.width || options.height;
        const auto [pw, ph] = page_picture_size(page, exact ? page_dpi(page) : size.drawn_dpi, "paper");
        check_size(page, pw, ph);
        if (options.width && options.height) check_size(page, *options.width, *options.height);
        return;
    }
    // (what the format holds first — in its own words —, then the codecs' limits)
    std::string ext(extension);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::optional<render::codecs::Format> format = ext == "tif" ? std::optional(render::codecs::Format::Tiff) : render::codecs::from_extension(ext);
    if (format && ext != "psd" && ext != "psb" && ext != "pcraft") {
        if (const auto most = render::codecs::max_dimensions(*format)) {
            if (size.width > most->first || size.height > most->second) {
                throw core::Error("image_too_large", std::string(render::codecs::name(*format)) + " holds at most " + std::to_string(most->first) + "×" +
                                                         std::to_string(most->second) + " pixels");
            }
        }
    }
    const auto [dw, dh] = page_picture_size(page, size.drawn_dpi, options.area);
    check_size(page, std::max(dw, size.width), std::max(dh, size.height));
}

PlannedCopy plan_copy(const core::Document& doc, std::size_t at, const fs::path& dest, const CopyOptions& options) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const std::string suffix = lower_suffix(dest);
    if (std::find(kExtensions.begin(), kExtensions.end(), suffix) == kExtensions.end()) throw render::NotYetPorted("image_write");
    check_options(options);
    const core::Page& page = doc.page(at);
    const int dpi = page_dpi(page);
    check_dpi(dpi);
    PlannedCopy plan;
    plan.file = dest;
    plan.dpi = dpi;
    plan.editable_text = options.editable_text;
    // (PC4 F-c: a PSD, a PSB or a .pcraft of the page at another size is the page drawn at that resolution; of a layer,
    // a book of its picture)
    if (sized(options) && !options.layer) plan.dpi = copy_size(doc, at, options).drawn_dpi;
    std::optional<Opened> alone;
    if (options.layer && (suffix == "psd" || suffix == "psb" || suffix == "pcraft")) {
        PagePicture picture = copy_picture(doc, at, options);
        plan.width = picture.image.width();
        plan.height = picture.image.height();
        const double file_dpi = picture.image.meta.dpi ? picture.image.meta.dpi->first : dpi;
        std::vector<std::string> notes = std::move(picture.notes);
        alone = picture_book(std::move(picture.image), layer_of(page, *options.layer).title);
        alone->doc.file_info = doc.file_info;
        alone->doc.native_files = doc.native_files;
        alone->doc.asset_dir = doc.asset_dir;
        trim_metadata(alone->doc, 0, options.metadata);
        alone->notes.insert(alone->notes.begin(), notes.begin(), notes.end());
        plan.dpi = std::max(1, static_cast<int>(std::lround(file_dpi)));
    }
    if (suffix == "psd" || suffix == "psb") {
        plan.psb = suffix == "psb";  // (PC4 F-d)
        if (alone) {
            plan.notes = std::move(alone->notes);
            plan.psd_book = std::move(alone->doc);
            plan.psd_page = 0;
            return plan;
        }
        if (options.layers) {
            plan.psd_book = doc;  // (the book's XMP in it: put_page_psd)
            trim_metadata(*plan.psd_book, at, options.metadata);
            plan.psd_page = at;
            return plan;
        }
        // (its merged picture alone: a book of it, the File Info carried over)
        Opened merged = duplicate_page(doc, at, doc.title, true);
        merged.doc.file_info = doc.file_info;
        merged.doc.native_files = doc.native_files;
        merged.doc.asset_dir = doc.asset_dir;
        merged.doc.title = doc.title;
        trim_metadata(merged.doc, 0, options.metadata);
        plan.notes = std::move(merged.notes);
        plan.psd_book = std::move(merged.doc);
        plan.psd_page = 0;
        return plan;
    }
    if (suffix == "pcraft") {  // (PC4 F-f: its layers as PhotoCraft's, or its merged picture alone)
        // (written as it is made into a file beside `dest` — never held whole —, moved over it by write_planned; the file
        // removed if the copy is not made)
        if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
        auto part = std::make_shared<detail::PartFile>(dest);
        const auto sink = [&part](std::string_view bytes) { part->write(bytes); };
        PcraftOptions pcraft;
        if (sized(options) && !alone) pcraft.dpi = plan.dpi;
        if (alone) {
            std::vector<std::string> notes = write_pcraft(alone->doc, 0, sink, pcraft);
            plan.notes = std::move(alone->notes);
            for (std::string& note : notes) plan.notes.push_back(std::move(note));
        } else if (options.layers && trims(options.metadata)) {
            core::Document book = doc;  // (its pages shared until one is changed)
            trim_metadata(book, at, options.metadata);
            plan.notes = write_pcraft(book, at, sink, pcraft);
        } else if (options.layers) {
            plan.notes = write_pcraft(doc, at, sink, pcraft);
        } else {
            Opened merged = duplicate_page(doc, at, doc.title, true);
            merged.doc.file_info = doc.file_info;
            merged.doc.native_files = doc.native_files;
            merged.doc.asset_dir = doc.asset_dir;
            merged.doc.title = doc.title;
            trim_metadata(merged.doc, 0, options.metadata);
            std::vector<std::string> notes = write_pcraft(merged.doc, 0, sink, pcraft);
            plan.notes = std::move(merged.notes);
            for (std::string& note : notes) plan.notes.push_back(std::move(note));
        }
        part->close();
        plan.part = std::move(part);
        return plan;
    }
    // (a page larger than its format holds — GIF's, WebP's, an icon's —: refused before it is drawn, in the codecs' words)
    const render::codecs::Format format = suffix == "tif" ? render::codecs::Format::Tiff : *render::codecs::from_extension(suffix);
    check_copy_size(doc, at, suffix, options);
    PagePicture picture = copy_picture(doc, at, options);
    plan.notes = picture.notes;
    plan.width = picture.image.width();
    plan.height = picture.image.height();
    const double file_dpi = picture.image.meta.dpi ? picture.image.meta.dpi->first : dpi;
    const render::codecs::EncodeOptions encode = encode_options(options, suffix);
    // (PC4 F-c's review P2-1: a layer's picture in a format without alpha — JPEG's and the others' — over white, its
    // edges mixed with it, as クイック書き出し and レイヤーからファイル… write it; never its alpha merely dropped)
    if (render::codecs::has_alpha(picture.image.layout()) && !render::codecs::has_alpha(render::codecs::plan(picture.image, format, encode).layout)) {
        picture.image = over_white(picture.image);
    }
    if (suffix == "tif" && !render::codecs::has_alpha(picture.image.layout())) {
        // (a page's TIFF as the exports write it; PC4 F-c: its colours in sRGB when asked)
        CodecImage image = std::move(picture.image);
        if (options.srgb && ((image.icc && !image.icc->empty()) || render::codecs::is_cmyk(image.layout()))) {
            image = srgb_picture(image, false, render::codecs::is_gray(image.layout()));
            image.icc.reset();
        }
        plan.bytes = tiff_of(image, file_dpi, image.meta.xmp);
    } else {
        // (every other picture — a layer's TIFF with its alpha too — through the codecs, one way for every format)
        plan.bytes = encode_for(std::move(picture.image), format, encode, options.srgb, plan.notes);
    }
    return plan;
}

CopyResult write_planned(const PlannedCopy& plan) {
    CopyResult result;
    result.file = plan.file;
    result.notes = plan.notes;
    result.width = plan.width;
    result.height = plan.height;
    if (plan.file.has_parent_path()) detail::make_dirs(plan.file.parent_path());
    if (plan.psd_book) {
        const PsdText text{plan.editable_text, plan.editable_text};  // (a line it cannot keep as text drawn as a picture)
        export_page_psd(*plan.psd_book, plan.psd_book->page(plan.psd_page), plan.file, plan.dpi, text, nullptr, plan.psb);
        std::error_code ec;
        result.bytes = fs::file_size(plan.file, ec);
        return result;
    }
    if (plan.part) {  // (PC4 F-f: the .pcraft written beside it, moved into place)
        result.bytes = plan.part->size();
        plan.part->commit();
        return result;
    }
    detail::Output output;
    output.put(plan.file, plan.bytes);
    output.commit();
    result.bytes = plan.bytes.size();
    return result;
}

namespace {

// A path as the file system names it (its folders' links followed, `..` gone), whether or not it is there yet.
fs::path resolved(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec) out = fs::absolute(path, ec).lexically_normal();
    while (!out.empty() && out.filename().empty() && out.has_relative_path()) out = out.parent_path();  // (no trailing slash)
    return out;
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec)) return true;  // (a link, another case of the name)
    return resolved(a) == resolved(b);
}

}  // namespace

WriteGuard write_guard(const core::Document& doc, const std::optional<fs::path>& book, std::vector<fs::path> sources) {
    WriteGuard guard;
    if (book && !book->empty()) guard.books.push_back(*book);
    for (const auto& [id, contents] : doc.smart_contents) {
        if (!contents || !contents->link) continue;
        const std::string path = core::py_str(core::py_get(*contents->link, "path", core::Json("")));
        if (!path.empty()) guard.linked.push_back(core::path_from_utf8(path));
        if (const auto found = smart_files::linked_file(*contents->link, book)) guard.linked.push_back(*found);
    }
    guard.sources = std::move(sources);
    return guard;
}

void check_target(const WriteGuard& guard, const fs::path& dest) {
    const std::string shown = core::path_to_utf8(dest);
    const fs::path here = resolved(dest);
    for (const fs::path& book : guard.books) {
        if (same_file(book, dest)) throw core::Error("refused_target", "cannot write over the book: " + shown);
        const fs::path inside = here.lexically_relative(resolved(book));
        if (!inside.empty() && *inside.begin() != ".." && inside != ".") {
            throw core::Error("refused_target", "cannot write inside the book's folder: " + shown);
        }
    }
    for (const fs::path& linked : guard.linked) {
        if (same_file(linked, dest)) throw core::Error("refused_target", "cannot write over a file the book links: " + shown);
    }
    for (const fs::path& source : guard.sources) {
        if (same_file(source, dest)) throw core::Error("refused_target", "cannot write over the picture a book was opened from: " + shown);
    }
}

CopyResult write_copy(const core::Document& doc, std::size_t at, const fs::path& dest, const CopyOptions& options) {
    return write_planned(plan_copy(doc, at, dest, options));
}

std::vector<CopyResult> copy_pages(const core::Document& doc, const std::vector<std::size_t>& pages, const fs::path& folder, std::string_view extension,
                                   const CopyOptions& options) {
    std::vector<CopyResult> out;
    for (const std::size_t at : pages) {
        if (at >= doc.pages.size()) throw core::Error("value", "no such page");
        if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
        const fs::path dest = folder / core::path_from_utf8(core::file_stem(doc.page(at)) + "." + std::string(extension));
        out.push_back(write_copy(doc, at, dest, options));  // (one page's picture and bytes at a time)
    }
    return out;
}

CopyEstimate estimate_copy(const core::Document& doc, std::size_t at, std::string_view extension, const CopyOptions& options,
                           std::optional<std::uint32_t> proxy) {
    CopyOptions asked = options;
    CopyEstimate out;
    const CopySize full = copy_size(doc, at, options);
    // (PC4 F-c's review P3-2: a size the copy cannot be written at refused as the copy refuses it — not a proxy's estimate)
    if (at < doc.pages.size() && !doc.is_deferred(at)) check_copy_size(doc, at, extension, options);
    if (proxy && *proxy > 0 && std::max(full.width, full.height) > *proxy) {
        // (made at most `proxy` long: the scale and any size asked brought down alike)
        const double k = double(*proxy) / double(std::max(full.width, full.height));
        asked.scale = std::clamp(options.scale * k, kScaleMin, kScaleMax);
        if (options.width) asked.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(*options.width * k)));
        if (options.height) asked.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(*options.height * k)));
        out.exact = false;
    }
    std::string ext(extension);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::uint64_t bytes = 0, fixed = 0;
    std::uint32_t w = 0, h = 0;
    if (ext == "psd" || ext == "psb" || ext == "pcraft") {
        // (a PSD's or a .pcraft's writer writes a file: one of its own in the temporary folder, measured and removed — its
        // name random, never another process's estimate)
        std::random_device random;
        const std::uint64_t tag = (std::uint64_t(random()) << 32) ^ random() ^ reinterpret_cast<std::uintptr_t>(&out);
        const fs::path temp = fs::temp_directory_path() / core::path_from_utf8("genko-estimate-" + std::to_string(tag) + "." + ext);
        CopyResult written = write_copy(doc, at, temp, asked);
        std::error_code ec;
        bytes = written.bytes;
        w = written.width;
        h = written.height;
        out.notes = std::move(written.notes);
        fs::remove(temp, ec);
    } else {
        // (planned only: the bytes made, counted and let go — nothing written)
        PlannedCopy plan = plan_copy(doc, at, fs::path("estimate." + ext), asked);
        bytes = plan.bytes.size();
        w = plan.width;
        h = plan.height;
        out.notes = std::move(plan.notes);
        if (!out.exact) {
            // (what a file has whatever its size — its headers, profile, metadata —, from the same file of the page at 1 %:
            // not scaled with the pixels)
            CopyOptions tiny = asked;
            tiny.scale = kScaleMin;
            tiny.width.reset();
            tiny.height.reset();
            try {
                fixed = std::min<std::uint64_t>(bytes, plan_copy(doc, at, fs::path("estimate." + ext), tiny).bytes.size());
            } catch (const std::exception&) {
                fixed = 0;  // (a small layer gone at 1 %: all of it scaled)
            }
        }
    }
    // (from a proxy: a page's pixels over the proxy's; a layer's — whose size is known only once it is drawn — by the
    // factor the proxy was made smaller by)
    double ratio = 1.0;
    out.width = w;
    out.height = h;
    if (!out.exact && w > 0 && h > 0) {
        if (options.layer) {
            const double k = options.width    ? double(*asked.width) / double(*options.width)
                             : options.height ? double(*asked.height) / double(*options.height)
                                              : asked.scale / options.scale;
            ratio = 1.0 / (k * k);
            out.width = static_cast<std::uint32_t>(std::max(1LL, std::llround(w / k)));
            out.height = static_cast<std::uint32_t>(std::max(1LL, std::llround(h / k)));
        } else {
            ratio = double(full.width) * full.height / (double(w) * h);
            out.width = full.width;
            out.height = full.height;
        }
    }
    out.bytes = fixed + static_cast<std::uint64_t>(std::llround(double(bytes - fixed) * ratio));
    return out;
}

std::vector<CopyResult> export_layer_comps(const core::Document& doc, std::size_t at, const fs::path& folder, const CompFiles& files) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const core::Page& page = doc.page(at);
    if (!page.layer_comps || page.layer_comps->comps.empty()) throw core::Error("value", "the page has no layer comps");
    const std::vector<core::LayerComp>& all = page.layer_comps->comps;
    for (const std::int64_t id : files.comps) {
        if (std::none_of(all.begin(), all.end(), [&](const core::LayerComp& c) { return c.id == id; })) {
            throw core::Error("value", "no layer comp with id " + std::to_string(id));
        }
    }
    std::string prefix = files.prefix.empty() ? (doc.title.empty() ? std::string("無題") : doc.title) : files.prefix;
    prefix = render::layer_comps::file_part(prefix);
    std::vector<CopyResult> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const core::LayerComp& comp = all[i];
        if (!files.comps.empty() && std::find(files.comps.begin(), files.comps.end(), comp.id) == files.comps.end()) continue;
        char number[32];
        std::snprintf(number, sizeof number, "%04zu", i);
        const fs::path dest = folder / core::path_from_utf8(prefix + "_" + number + "_" + render::layer_comps::file_part(comp.name) + "." + files.extension);
        if (files.check) files.check(dest);
        core::Document one = doc;  // (the comp applied to a copy: the book unchanged)
        try {
            render::layer_comps::apply(one, at, comp.layers, comp.apply_visibility, comp.apply_position, comp.apply_appearance);
        } catch (const core::OpError& error) {
            throw core::Error("value", error.what());
        }
        one.edit_page(at).layer_comps->last_applied = comp.id;
        out.push_back(write_copy(one, at, dest, files.options));  // (one comp's picture and bytes at a time)
    }
    if (out.empty()) throw core::Error("value", "no layer comps chosen");
    return out;
}

}  // namespace genko::formats
