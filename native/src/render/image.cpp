// Pillow 12.3.0's Image / ImageChops / ImageOps Python layer (PIL/Image.py, PIL/ImageChops.py, PIL/ImageOps.py,
// PIL/ImageFilter.py) and the argument handling of its binding (_imaging.c), over libImaging.

#include "render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/imaging.hpp"

namespace genko::render {

namespace detail {

[[noreturn]] void throw_imaging_error(std::string_view fallback) {
    const int kind = genko_imaging_error_kind();
    std::string message = genko_imaging_error_message();
    genko_imaging_clear_error();
    if (message.empty()) message = std::string(fallback);
    if (kind == 1) throw core::Error("memory", message);
    throw core::Error("value", message);
}

void check_status(int status, std::string_view fallback) {
    if (status < 0) throw_imaging_error(fallback);
}

ModeID mode_id(std::string_view mode) {
    const std::string text(mode);
    return findModeID(text.c_str());
}

RawModeID rawmode_id(std::string_view rawmode) {
    const std::string text(rawmode);
    return findRawModeID(text.c_str());
}

std::string_view mode_name(ModeID mode) { return getModeData(mode)->name; }

namespace {

struct ModeInfo {
    std::string_view mode;
    std::string_view base;
    std::string_view type;
    int bands;
};

constexpr ModeInfo kModes[] = {
    {"1", "L", "L", 1},       {"L", "L", "L", 1},        {"I", "L", "I", 1},     {"F", "L", "F", 1},
    {"P", "P", "L", 1},       {"RGB", "RGB", "L", 3},    {"RGBX", "RGB", "L", 4}, {"RGBA", "RGB", "L", 4},
    {"CMYK", "RGB", "L", 4},  {"YCbCr", "RGB", "L", 3},  {"LAB", "RGB", "L", 3}, {"HSV", "RGB", "L", 3},
    {"RGBa", "RGB", "L", 4},  {"LA", "L", "L", 2},       {"La", "L", "L", 2},    {"PA", "RGB", "L", 2},
};

const ModeInfo& mode_info(std::string_view mode) {
    for (const ModeInfo& info : kModes) {
        if (info.mode == mode) return info;
    }
    if (mode.starts_with("I;16") || mode.starts_with("I;32")) {
        static constexpr ModeInfo kI16{"I;16", "L", "L", 1};
        return kI16;
    }
    throw core::Error("value", "illegal image mode");
}

}  // namespace

std::string_view mode_base(std::string_view mode) { return mode_info(mode).base; }
std::string_view mode_type(std::string_view mode) { return mode_info(mode).type; }
int mode_bands(std::string_view mode) { return mode_info(mode).bands; }

INT32 ink_for(const Ink& colour, Imaging im) {
    // _imaging.c getink(): fills four bytes that can be read as UINT8s or as an INT32
    unsigned char ink[4] = {0, 0, 0, 0};
    const auto& values = colour.values();
    const int tuple_size = colour.is_tuple() ? static_cast<int>(values.size()) : -1;
    const bool scalar = !colour.is_tuple() || tuple_size == 1;  // a tuple of one is its item
    long long r = 0;
    long long g = 0;
    long long b = 0;
    long long a = 0;
    bool r_is_int = false;
    const auto clip8 = [](long long v) { return static_cast<unsigned char>(v <= 0 ? 0 : v < 256 ? v : 255); };
    if (im->type == IMAGING_TYPE_UINT8 || im->type == IMAGING_TYPE_INT32 || im->type == IMAGING_TYPE_SPECIAL) {
        if (scalar && !values.empty()) {
            r = values[0];
            r_is_int = true;
        } else if (im->bands == 1) {
            throw core::Error("value", "color must be int or single-element tuple");
        } else if (tuple_size == -1) {
            throw core::Error("value", "color must be int or tuple");
        }
    }
    switch (im->type) {
        case IMAGING_TYPE_UINT8:
            if (im->bands == 1) {
                ink[0] = clip8(r);
            } else {
                if (r_is_int) {
                    a = static_cast<unsigned char>(r >> 24);
                    b = static_cast<unsigned char>(r >> 16);
                    g = static_cast<unsigned char>(r >> 8);
                    r = static_cast<unsigned char>(r);
                } else {
                    a = 255;
                    if (im->bands == 2) {
                        if (tuple_size != 1 && tuple_size != 2) {
                            throw core::Error("value", "color must be int, or tuple of one or two elements");
                        }
                        r = values[0];
                        if (tuple_size == 2) a = values[1];
                        g = b = r;
                    } else {
                        if (tuple_size != 3 && tuple_size != 4) {
                            throw core::Error("value", "color must be int, or tuple of one, three or four elements");
                        }
                        r = values[0];
                        g = values[1];
                        b = values[2];
                        if (tuple_size == 4) a = values[3];
                    }
                }
                ink[0] = clip8(r);
                ink[1] = clip8(g);
                ink[2] = clip8(b);
                ink[3] = clip8(a);
            }
            break;
        case IMAGING_TYPE_INT32: {
            const auto itmp = static_cast<INT32>(r);
            std::memcpy(ink, &itmp, sizeof(itmp));
            break;
        }
        case IMAGING_TYPE_FLOAT32: {
            if (!scalar || values.empty()) throw core::Error("value", "must be real number, not tuple");
            const auto f = static_cast<FLOAT32>(static_cast<double>(values[0]));
            std::memcpy(ink, &f, sizeof(f));
            break;
        }
        case IMAGING_TYPE_SPECIAL:
            ink[0] = static_cast<unsigned char>(r);
            ink[1] = static_cast<unsigned char>(r >> 8);
            break;
        default:
            throw core::Error("value", "unrecognized image mode");
    }
    INT32 out = 0;
    std::memcpy(&out, ink, sizeof(out));
    return out;
}

}  // namespace detail

using detail::check;
using detail::check_status;

namespace {

// libImaging keeps up to this many freed 16 MB blocks for the next pictures (Pillow's PILLOW_BLOCKS_MAX): a page is
// drawn through many page-sized pictures, and taking fresh memory from the system for each costs more than drawing.
// (A reused block is cleared as a new one is: the pixels are the same.)
const bool g_arena_ready = [] {
    ImagingMemorySetBlocksMax(&ImagingDefaultArena, 32);
    return true;
}();

void check_size(Size size) {
    if (size.width < 0 || size.height < 0) throw core::Error("value", "Width and height must be >= 0");
}

// Image._decompression_bomb_check (the warning is not kept; the error is).
void bomb_check(std::int64_t w, std::int64_t h) {
    const std::int64_t pixels = std::max<std::int64_t>(1, w) * std::max<std::int64_t>(1, h);
    if (pixels > 2 * kMaxImagePixels) {
        throw core::Error("image_too_large", "Image size (" + std::to_string(pixels) + " pixels) exceeds limit of " +
                                                 std::to_string(2 * kMaxImagePixels) +
                                                 " pixels, could be decompression bomb DOS attack.");
    }
}

// Python's round(x) for a float, as an int.
int py_round_int(double x) {
    if (!std::isfinite(x)) throw core::Error("value", "cannot convert float to integer");
    const double r = std::nearbyint(x);
    if (r > static_cast<double>(std::numeric_limits<int>::max()) || r < static_cast<double>(std::numeric_limits<int>::min())) {
        throw core::Error("value", "integer out of range");
    }
    return static_cast<int>(r);
}

int filter_id(Resample r) { return static_cast<int>(r); }

double filter_support(Resample r) {
    switch (r) {
        case Resample::Box: return 0.5;
        case Resample::Bilinear: return 1.0;
        case Resample::Hamming: return 1.0;
        case Resample::Bicubic: return 2.0;
        case Resample::Lanczos: return 3.0;
        case Resample::Nearest: break;
    }
    return 0.0;
}

}  // namespace

// --- Ink ------------------------------------------------------------------------------------------------------------

Ink Ink::tuple(std::vector<std::int64_t> values) {
    Ink out;
    out.values_ = std::move(values);
    out.tuple_ = true;
    return out;
}

Ink Ink::with_alpha(std::span<const std::int64_t> rgb, std::int64_t alpha) {
    std::vector<std::int64_t> values(rgb.begin(), rgb.end());
    values.push_back(alpha);
    return tuple(std::move(values));
}

// --- Filter -------------------------------------------------------------------------------------------------------

Filter Filter::gaussian_blur(double radius) { return gaussian_blur(radius, radius); }

Filter Filter::gaussian_blur(double radius_x, double radius_y) {
    Filter f;
    f.kind = Kind::GaussianBlur;
    f.radius_x = radius_x;
    f.radius_y = radius_y;
    return f;
}

Filter Filter::box_blur(double radius) { return box_blur(radius, radius); }

Filter Filter::box_blur(double radius_x, double radius_y) {
    if (radius_x < 0 || radius_y < 0) throw core::Error("value", "radius must be >= 0");
    Filter f;
    f.kind = Kind::BoxBlur;
    f.radius_x = radius_x;
    f.radius_y = radius_y;
    return f;
}

Filter Filter::rank_filter(int size, int rank) {
    if (size % 2 == 0) throw core::Error("value", "bad filter size");
    if (static_cast<std::int64_t>(size) * size * 4 > 2147483647LL) throw core::Error("value", "filter size too large");
    if (rank < 0 || static_cast<std::int64_t>(rank) >= static_cast<std::int64_t>(size) * size) {
        throw core::Error("value", "bad rank value");
    }
    Filter f;
    f.kind = Kind::Rank;
    f.size = size;
    f.rank = rank;
    return f;
}

Filter Filter::min_filter(int size) { return rank_filter(size, 0); }
Filter Filter::max_filter(int size) { return rank_filter(size, size * size - 1); }
Filter Filter::median_filter(int size) { return rank_filter(size, size * size / 2); }

Filter Filter::mode_filter(int size) {
    Filter f;
    f.kind = Kind::Mode;
    f.size = size;
    return f;
}

Filter Filter::unsharp_mask(double radius, int percent, int threshold) {
    Filter f;
    f.kind = Kind::UnsharpMask;
    f.radius_x = f.radius_y = radius;
    f.percent = percent;
    f.threshold = threshold;
    return f;
}

Filter Filter::kernel(int width, int height, std::vector<double> weights, std::optional<double> scale, double offset) {
    Filter f;
    f.kind = Kind::Kernel;
    if (!scale) {
        // functools.reduce(lambda a, b: a + b, kernel)
        if (weights.empty()) throw core::Error("value", "reduce() of empty iterable with no initial value");
        double sum = weights[0];
        for (std::size_t i = 1; i < weights.size(); ++i) sum = sum + weights[i];
        scale = sum;
    }
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) != weights.size()) {
        throw core::Error("value", "not enough coefficients in kernel");
    }
    f.kernel_width = width;
    f.kernel_height = height;
    f.scale = *scale;
    f.offset = offset;
    f.weights = std::move(weights);
    return f;
}

Filter Filter::blur() { return kernel(5, 5, {1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1}, 16, 0); }
Filter Filter::contour() { return kernel(3, 3, {-1, -1, -1, -1, 8, -1, -1, -1, -1}, 1, 255); }
Filter Filter::detail() { return kernel(3, 3, {0, -1, 0, -1, 10, -1, 0, -1, 0}, 6, 0); }
Filter Filter::edge_enhance() { return kernel(3, 3, {-1, -1, -1, -1, 10, -1, -1, -1, -1}, 2, 0); }
Filter Filter::edge_enhance_more() { return kernel(3, 3, {-1, -1, -1, -1, 9, -1, -1, -1, -1}, 1, 0); }
Filter Filter::emboss() { return kernel(3, 3, {-1, 0, 0, 0, 1, 0, 0, 0, 0}, 1, 128); }
Filter Filter::find_edges() { return kernel(3, 3, {-1, -1, -1, -1, 8, -1, -1, -1, -1}, 1, 0); }
Filter Filter::sharpen() { return kernel(3, 3, {-2, -2, -2, -2, 32, -2, -2, -2, -2}, 16, 0); }
Filter Filter::smooth() { return kernel(3, 3, {1, 1, 1, 1, 5, 1, 1, 1, 1}, 13, 0); }
Filter Filter::smooth_more() {
    return kernel(5, 5, {1, 1, 1, 1, 1, 1, 5, 5, 5, 1, 1, 5, 44, 5, 1, 1, 5, 5, 5, 1, 1, 1, 1, 1, 1}, 100, 0);
}

// --- Image: lifetime ----------------------------------------------------------------------------------------------

Image::~Image() {
    if (im_ != nullptr) ImagingDelete(im_);
}

Image::Image(const Image& other) : transparency_(other.transparency_) {
    if (other.im_ != nullptr) im_ = check(ImagingCopy(other.im_));
}

Image& Image::operator=(const Image& other) {
    if (this != &other) {
        Image copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Image::Image(Image&& other) noexcept : im_(other.im_), transparency_(std::move(other.transparency_)) {
    other.im_ = nullptr;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        if (im_ != nullptr) ImagingDelete(im_);
        im_ = other.im_;
        other.im_ = nullptr;
        transparency_ = std::move(other.transparency_);
    }
    return *this;
}

Image Image::adopt(ImagingMemoryInstance* im) { return Image(check(im)); }

Image Image::derived(ImagingMemoryInstance* im) const {
    Image out(check(im));
    out.transparency_ = transparency_;
    return out;
}

void Image::require() const {
    if (im_ == nullptr) throw core::Error("value", "no image");
}

// --- Image: making ------------------------------------------------------------------------------------------------

Image Image::create(std::string_view mode, Size size, const Ink& colour) {
    check_size(size);
    Image out(check(ImagingNewDirty(detail::mode_id(mode), size.width, size.height)));
    const INT32 ink = detail::ink_for(colour, out.im_);
    ImagingFill(out.im_, &ink);
    return out;
}

Image Image::create_blank(std::string_view mode, Size size) {
    check_size(size);
    return Image(check(ImagingNew(detail::mode_id(mode), size.width, size.height)));
}

Image Image::frombytes(std::string_view mode, Size size, std::string_view data, std::string_view rawmode) {
    check_size(size);
    Image out = create(mode, size);
    if (size.width == 0 || size.height == 0) return out;
    int bits = 0;
    const ImagingShuffler unpack =
        ImagingFindUnpacker(detail::mode_id(mode), detail::rawmode_id(rawmode.empty() ? mode : rawmode), &bits);
    if (unpack == nullptr) throw core::Error("value", "unknown raw mode for given image mode");
    const std::size_t stride = (static_cast<std::size_t>(bits) * static_cast<std::size_t>(size.width) + 7) / 8;
    if (data.size() < stride * static_cast<std::size_t>(size.height)) throw core::Error("value", "not enough image data");
    for (int y = 0; y < size.height; ++y) {
        unpack(reinterpret_cast<UINT8*>(out.im_->image[y]),
               reinterpret_cast<const UINT8*>(data.data()) + stride * static_cast<std::size_t>(y), size.width);
    }
    return out;
}

std::string_view Image::mode() const {
    require();
    return detail::mode_name(im_->mode);
}

Size Image::size() const {
    require();
    return Size{im_->xsize, im_->ysize};
}

int Image::width() const { return size().width; }
int Image::height() const { return size().height; }

int Image::bands() const {
    require();
    return im_->bands;
}

std::vector<std::string> Image::getbands() const {
    require();
    std::vector<std::string> out;
    const std::string_view m = mode();
    if (m == "RGBA" || m == "RGBa" || m == "RGBX" || m == "CMYK") {
        const char* names = m == "CMYK" ? "CMYK" : m == "RGBA" ? "RGBA" : m == "RGBa" ? "RGBa" : "RGBX";
        for (int i = 0; i < 4; ++i) out.emplace_back(1, names[i]);
    } else if (m == "RGB" || m == "HSV") {
        for (char c : m) out.emplace_back(1, c);
    } else if (m == "YCbCr") {
        out = {"Y", "Cb", "Cr"};
    } else if (m == "LAB") {
        out = {"L", "A", "B"};
    } else if (m == "LA") {
        out = {"L", "A"};
    } else if (m == "La") {
        out = {"L", "a"};
    } else if (m == "PA") {
        out = {"P", "A"};
    } else if (m.starts_with("I;")) {
        out = {"I"};
    } else {
        out = {std::string(m)};
    }
    return out;
}

std::string Image::tobytes() const {
    require();
    if (im_->xsize == 0 || im_->ysize == 0) return {};
    int bits = 0;
    const ImagingShuffler pack = ImagingFindPacker(im_->mode, detail::rawmode_id(mode()), &bits);
    if (pack == nullptr) throw core::Error("value", "unknown raw mode for given image mode");
    const std::size_t stride = (static_cast<std::size_t>(bits) * static_cast<std::size_t>(im_->xsize) + 7) / 8;
    std::string out(stride * static_cast<std::size_t>(im_->ysize), '\0');
    for (int y = 0; y < im_->ysize; ++y) {
        pack(reinterpret_cast<UINT8*>(out.data()) + stride * static_cast<std::size_t>(y),
             reinterpret_cast<const UINT8*>(im_->image[y]), im_->xsize);
    }
    return out;
}

std::vector<double> Image::getpixel(int x, int y) const {
    require();
    if (x < 0) x = im_->xsize + x;
    if (y < 0) y = im_->ysize + y;
    if (x < 0 || x >= im_->xsize || y < 0 || y >= im_->ysize) throw core::Error("value", "image index out of range");
    const ImagingAccess access = ImagingAccessNew(im_);
    if (access == nullptr) throw core::Error("value", "no pixel access for this mode");
    union {
        UINT8 b[4];
        UINT16 h;
        INT32 i;
        FLOAT32 f;
    } pixel{};
    access->get_pixel(im_, x, y, &pixel);
    std::vector<double> out;
    switch (im_->type) {
        case IMAGING_TYPE_UINT8:
            for (int i = 0; i < im_->bands; ++i) out.push_back(pixel.b[i]);
            break;
        case IMAGING_TYPE_INT32: out.push_back(pixel.i); break;
        case IMAGING_TYPE_FLOAT32: out.push_back(pixel.f); break;
        default: out.push_back(pixel.h); break;
    }
    return out;
}

// --- Image: copy, crop, paste ---------------------------------------------------------------------------------------

Image Image::copy() const {
    require();
    return derived(ImagingCopy(im_));
}

Image Image::crop(const Box& box) const {
    require();
    if (box.x1 < box.x0) throw core::Error("value", "Coordinate 'right' is less than 'left'");
    if (box.y1 < box.y0) throw core::Error("value", "Coordinate 'lower' is less than 'upper'");
    bomb_check(std::abs(static_cast<std::int64_t>(box.x1) - box.x0), std::abs(static_cast<std::int64_t>(box.y1) - box.y0));
    return derived(ImagingCrop(im_, box.x0, box.y0, box.x1, box.y1));
}

Image Image::crop(const BoxF& box) const {
    if (box.x1 < box.x0) throw core::Error("value", "Coordinate 'right' is less than 'left'");
    if (box.y1 < box.y0) throw core::Error("value", "Coordinate 'lower' is less than 'upper'");
    return crop(Box{py_round_int(box.x0), py_round_int(box.y0), py_round_int(box.x1), py_round_int(box.y1)});
}

void Image::paste(const Image& im, Point at, const Image* mask) {
    paste(im, Box{at.x, at.y, at.x + im.width(), at.y + im.height()}, mask);
}

void Image::paste(const Image& im, const Box& box, const Image* mask) {
    require();
    im.require();
    const ImagingMemoryInstance* source = im.im_;
    Image converted;
    const std::string_view mine = mode();
    const std::string_view theirs = im.mode();
    if (mine != theirs) {
        if (mine != "RGB" || (theirs != "LA" && theirs != "RGBA" && theirs != "RGBa")) {
            converted = im.convert(mine);
            source = converted.im_;
        }
    }
    check_status(ImagingPaste(im_, const_cast<Imaging>(source), mask != nullptr ? mask->im_ : nullptr, box.x0, box.y0,
                              box.x1, box.y1));
}

void Image::paste(const Ink& colour, const Box& box, const Image* mask) {
    require();
    const INT32 ink = detail::ink_for(colour, im_);
    check_status(ImagingFill2(im_, &ink, mask != nullptr ? mask->im_ : nullptr, box.x0, box.y0, box.x1, box.y1));
}

void Image::paste(const Ink& colour, const Image& mask) { paste(colour, Box{0, 0, mask.width(), mask.height()}, &mask); }

void Image::alpha_composite(const Image& im, Point dest, std::optional<Box> source) {
    require();
    const Box overlay_box = source.value_or(Box{0, 0, im.width(), im.height()});
    if (overlay_box.x0 < 0 || overlay_box.y0 < 0 || overlay_box.x1 < 0 || overlay_box.y1 < 0) {
        throw core::Error("value", "Source must be non-negative");
    }
    const bool whole = overlay_box == Box{0, 0, im.width(), im.height()};
    const Image cropped = whole ? Image() : im.crop(overlay_box);
    const Image& overlay = whole ? im : cropped;
    const Box box{dest.x, dest.y, dest.x + overlay.width(), dest.y + overlay.height()};
    if (box == Box{0, 0, width(), height()}) {
        Image result = render::alpha_composite(*this, overlay);
        paste(result, box);
    } else {
        const Image background = crop(box);
        const Image result = render::alpha_composite(background, overlay);
        paste(result, box);
    }
}

// --- Image: modes ---------------------------------------------------------------------------------------------------

Image Image::convert(std::string_view mode, Dither dither) const {
    require();
    const std::string_view from = this->mode();
    const bool has_transparency = transparency_.present();
    if (mode.empty() || mode == from) return copy();
    if (from == "RGBA" && (mode == "P" || mode == "PA")) throw core::Error("value", "conversion to P is not ported");
    if (mode == "LAB" || from == "LAB") throw core::Error("value", "conversion to or from LAB is not ported");
    if (mode == "P" || mode == "PA") throw core::Error("value", "conversion to P is not ported");

    bool delete_trns = false;
    std::optional<Transparency> trns;
    Image palette_source;  // P with transparency: a copy whose palette carries the alpha (Python changes self.im)
    const ImagingMemoryInstance* src = im_;
    if (has_transparency) {
        const bool promote = ((from == "1" || from == "L" || from == "I" || from == "I;16") && (mode == "LA" || mode == "RGBA")) ||
                             (from == "RGB" && (mode == "La" || mode == "LA" || mode == "RGBa" || mode == "RGBA"));
        if (promote) {
            // convert_transparent: the transparent colour becomes alpha 0
            int r = 0;
            int g = 0;
            int b = 0;
            if (transparency_.kind == Transparency::Kind::Rgb) {
                r = transparency_.rgb[0];
                g = transparency_.rgb[1];
                b = transparency_.rgb[2];
            } else if (transparency_.kind == Transparency::Kind::Index) {
                r = transparency_.value;
            } else {
                throw core::Error("value", "an integer is required");
            }
            Image out(check(ImagingConvertTransparent(im_, detail::mode_id(mode), r, g, b)));
            return out;  // (its info has no transparency any more)
        }
        if ((from == "L" || from == "RGB" || from == "P") && (mode == "L" || mode == "RGB" || mode == "P")) {
            if (transparency_.kind == Transparency::Kind::Bytes) {
                delete_trns = true;
            } else {
                // the transparent colour in the new mode (one pixel converted the same way)
                Image one = create(from, Size{1, 1});
                if (from == "P" && im_->palette != nullptr) {
                    ImagingPaletteDelete(one.im_->palette);
                    one.im_->palette = ImagingPaletteDuplicate(im_->palette);
                }
                Ink colour = transparency_.kind == Transparency::Kind::Rgb
                                 ? Ink{transparency_.rgb[0], transparency_.rgb[1], transparency_.rgb[2]}
                                 : Ink(transparency_.value);
                const INT32 ink = detail::ink_for(colour, one.im_);
                const ImagingAccess access = ImagingAccessNew(one.im_);
                if (access != nullptr) access->put_pixel(one.im_, 0, 0, &ink);
                const Image moved = one.convert(mode == "P" ? std::string_view("RGB") : mode);
                const std::vector<double> px = moved.getpixel(0, 0);
                Transparency t;
                if (px.size() == 1) {
                    t.kind = Transparency::Kind::Index;
                    t.value = static_cast<int>(px[0]);
                } else {
                    t.kind = Transparency::Kind::Rgb;
                    t.rgb = {static_cast<int>(px[0]), static_cast<int>(px[1]), static_cast<int>(px[2])};
                }
                trns = t;
            }
        } else if (from == "P" && (mode == "LA" || mode == "PA" || mode == "RGBA")) {
            delete_trns = true;
            palette_source = copy();
            ImagingPalette palette = palette_source.im_->palette;
            if (palette == nullptr) throw core::Error("value", "image has no palette");
            palette->mode = IMAGING_MODE_RGBA;
            if (transparency_.kind == Transparency::Kind::Bytes) {
                if (transparency_.bytes.size() > 256) throw core::Error("value", "palette index out of range");
                for (std::size_t i = 0; i < transparency_.bytes.size(); ++i) {
                    palette->palette[i * 4 + 3] = static_cast<UINT8>(transparency_.bytes[i]);
                }
            } else if (transparency_.kind == Transparency::Kind::Index) {
                if (transparency_.value < 0 || transparency_.value >= 256) {
                    throw core::Error("value", "palette index out of range");
                }
                palette->palette[transparency_.value * 4 + 3] = 0;
            } else {
                throw core::Error("value", "Transparency for P mode should be bytes or int");
            }
            src = palette_source.im_;
        }
    }

    ImagingMemoryInstance* converted =
        ImagingConvert(const_cast<Imaging>(src), detail::mode_id(mode), nullptr, static_cast<int>(dither));
    if (converted == nullptr) {
        // normalize the source image and try again (its base mode first, without dithering)
        const std::string message = genko_imaging_error_message();
        genko_imaging_clear_error();
        const std::string_view base = detail::mode_base(from);
        if (base == from) throw core::Error("value", message.empty() ? "illegal conversion" : message);
        Image normal(check(ImagingConvert(const_cast<Imaging>(src), detail::mode_id(base), nullptr, 0)));
        converted = check(ImagingConvert(normal.im_, detail::mode_id(mode), nullptr, static_cast<int>(dither)));
    }
    Image out = derived(converted);
    if (delete_trns) out.transparency_ = Transparency{};
    if (trns) out.transparency_ = *trns;
    return out;
}

// --- Image: pixel tables, bands ---------------------------------------------------------------------------------------

Image Image::point(std::span<const int> lut, std::string_view mode) const {
    require();
    if (this->mode() == "F") throw core::Error("value", "point operation not supported for this mode");
    const ModeID target = mode.empty() ? IMAGING_MODE_UNKNOWN : detail::mode_id(mode);
    if (target == IMAGING_MODE_F) throw core::Error("value", "point to F is not ported");
    if (im_->mode == IMAGING_MODE_I && target == IMAGING_MODE_L) throw core::Error("value", "point from I is not ported");
    const int bands = target != IMAGING_MODE_UNKNOWN ? detail::mode_bands(mode) : im_->bands;
    const std::size_t n = static_cast<std::size_t>(256 * bands);
    if (lut.size() != n) throw core::Error("value", "wrong number of lut entries");
    const auto clip8 = [](int v) { return static_cast<UINT8>(v <= 0 ? 0 : v < 256 ? v : 255); };
    ImagingMemoryInstance* out = nullptr;
    if (target == IMAGING_MODE_I) {
        std::vector<INT32> data(lut.begin(), lut.end());
        out = ImagingPoint(im_, target, data.data());
    } else if (target != IMAGING_MODE_UNKNOWN && bands > 1) {
        UINT8 table[1024] = {};
        for (std::size_t i = 0; i < 256; ++i) {
            table[i * 4] = clip8(lut[i]);
            table[i * 4 + 1] = clip8(lut[i + 256]);
            table[i * 4 + 2] = clip8(lut[i + 512]);
            if (n > 768) table[i * 4 + 3] = clip8(lut[i + 768]);
        }
        out = ImagingPoint(im_, target, table);
    } else {
        UINT8 table[1024] = {};
        for (std::size_t i = 0; i < n; ++i) table[i] = clip8(lut[i]);
        out = ImagingPoint(im_, target, table);
    }
    return derived(out);
}

Image Image::point(const std::function<int(int)>& fn, std::string_view mode) const {
    require();
    std::vector<int> one(256);
    for (int i = 0; i < 256; ++i) one[static_cast<std::size_t>(i)] = fn(i);
    std::vector<int> lut;
    lut.reserve(static_cast<std::size_t>(256 * im_->bands));
    for (int b = 0; b < im_->bands; ++b) lut.insert(lut.end(), one.begin(), one.end());
    return point(lut, mode);
}

void Image::putalpha(const Image& alpha) {
    require();
    alpha.require();
    std::string_view m = mode();
    if (m != "LA" && m != "PA" && m != "RGBA") {
        const std::string target = std::string(detail::mode_base(m)) + "A";
        const ModeID target_id = detail::mode_id(target);
        const bool rgb_like = im_->mode == IMAGING_MODE_RGB || im_->mode == IMAGING_MODE_RGBA || im_->mode == IMAGING_MODE_RGBX;
        const bool target_rgb_like = target_id == IMAGING_MODE_RGB || target_id == IMAGING_MODE_RGBA || target_id == IMAGING_MODE_RGBX;
        if (rgb_like && target_rgb_like) {
            // im_setmode: colour to colour in place
            im_->mode = target_id;
            im_->bands = static_cast<int>(target.size());
            if (target_id == IMAGING_MODE_RGBA) (void)ImagingFillBand(im_, 3, 255);
        } else {
            Image converted(check(ImagingConvert(im_, target_id, nullptr, 0)));
            const std::string_view cm = converted.mode();
            if (cm != "LA" && cm != "PA" && cm != "RGBA") throw core::Error("value", "alpha channel could not be added");
            ImagingDelete(im_);
            im_ = converted.im_;
            converted.im_ = nullptr;
        }
        m = mode();
    }
    const int band = (m == "LA" || m == "PA") ? 1 : 3;
    const std::string_view am = alpha.mode();
    if (am != "1" && am != "L") throw core::Error("value", "illegal image mode");
    if (am == "1") {
        const Image l = alpha.convert("L");
        check(ImagingPutBand(im_, l.im_, band));
    } else {
        check(ImagingPutBand(im_, alpha.im_, band));
    }
}

void Image::putalpha(int alpha) {
    require();
    std::string_view m = mode();
    if (m != "LA" && m != "PA" && m != "RGBA") {
        putalpha(create("L", size(), Ink(alpha)));
        return;
    }
    const int band = (m == "LA" || m == "PA") ? 1 : 3;
    if (ImagingFillBand(im_, band, alpha) == nullptr) {
        genko_imaging_clear_error();
        putalpha(create("L", size(), Ink(alpha)));
    }
}

std::vector<Image> Image::split() const {
    require();
    if (im_->bands == 1) return {copy()};
    Imaging bands[4] = {nullptr, nullptr, nullptr, nullptr};
    if (!ImagingSplit(im_, bands)) detail::throw_imaging_error();
    std::vector<Image> out;
    for (int i = 0; i < im_->bands; ++i) out.push_back(derived(bands[i]));
    return out;
}

Image Image::getchannel(int band) const {
    require();
    return derived(ImagingGetBand(im_, band));
}

Image Image::getchannel(std::string_view name) const {
    const std::vector<std::string> names = getbands();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return getchannel(static_cast<int>(i));
    }
    throw core::Error("value", "The image has no channel \"" + std::string(name) + "\"");
}

Image Image::merge(std::string_view mode, const std::vector<Image>& bands) {
    if (bands.empty() || detail::mode_bands(mode) != static_cast<int>(bands.size()) ||
        mode.find('*') != std::string_view::npos) {
        throw core::Error("value", "wrong number of bands");
    }
    for (std::size_t i = 1; i < bands.size(); ++i) {
        if (bands[i].mode() != detail::mode_type(mode)) throw core::Error("value", "mode mismatch");
        if (bands[i].size() != bands[0].size()) throw core::Error("value", "size mismatch");
    }
    Imaging raw[4] = {nullptr, nullptr, nullptr, nullptr};
    for (std::size_t i = 0; i < bands.size(); ++i) raw[i] = bands[i].im_;
    Image out(check(ImagingMerge(detail::mode_id(mode), raw)));
    ImagingCopyPalette(out.im_, bands[0].im_);
    out.transparency_ = bands[0].transparency_;
    return out;
}

// --- Image: geometry ------------------------------------------------------------------------------------------------

Image Image::resize(Size size, Resample resample, std::optional<BoxF> box_in, std::optional<double> reducing_gap) const {
    require();
    if (reducing_gap && *reducing_gap < 1.0) throw core::Error("value", "reducing_gap must be 1.0 or greater");
    BoxF box = box_in.value_or(BoxF{0, 0, static_cast<double>(width()), static_cast<double>(height())});
    const bool full_box = box.x0 == 0 && box.y0 == 0 && box.x1 == width() && box.y1 == height();
    if (this->size() == size && full_box) return copy();
    const std::string_view m = mode();
    if (m == "1" || m == "P") resample = Resample::Nearest;
    if ((m == "LA" || m == "RGBA") && resample != Resample::Nearest) {
        // (Pillow passes the box on but not reducing_gap)
        const Image pre = convert(m == "LA" ? "La" : "RGBa");
        const Image sized = pre.resize(size, resample, box);
        return sized.convert(m);
    }

    const Image* self = this;
    Image reduced;
    if (reducing_gap && resample != Resample::Nearest) {
        // Python's int(...) or 1
        const auto factor_of = [](double v) {
            const auto f = static_cast<int>(std::trunc(v));
            return f == 0 ? 1 : f;
        };
        const int factor_x = factor_of((box.x1 - box.x0) / size.width / *reducing_gap);
        const int factor_y = factor_of((box.y1 - box.y0) / size.height / *reducing_gap);
        if (factor_x > 1 || factor_y > 1) {
            // _get_safe_box
            const double support = filter_support(resample) - 0.5;
            const double scale_x = (box.x1 - box.x0) / size.width;
            const double scale_y = (box.y1 - box.y0) / size.height;
            const double support_x = support * scale_x;
            const double support_y = support * scale_y;
            const Box reduce_box{std::max(0, static_cast<int>(std::trunc(box.x0 - support_x))),
                                 std::max(0, static_cast<int>(std::trunc(box.y0 - support_y))),
                                 std::min(width(), static_cast<int>(std::ceil(box.x1 + support_x))),
                                 std::min(height(), static_cast<int>(std::ceil(box.y1 + support_y)))};
            reduced = reduce(factor_x, factor_y, reduce_box);
            self = &reduced;
            box = BoxF{(box.x0 - reduce_box.x0) / factor_x, (box.y0 - reduce_box.y0) / factor_y,
                       (box.x1 - reduce_box.x0) / factor_x, (box.y1 - reduce_box.y0) / factor_y};
        }
    }

    // _imaging.c _resize: the box in single precision
    const auto core_resize = [&](const Image& src, Size to, BoxF b) {
        float fbox[4] = {static_cast<float>(b.x0), static_cast<float>(b.y0), static_cast<float>(b.x1), static_cast<float>(b.y1)};
        Imaging in = src.im_;
        if (to.width < 1 || to.height < 1) throw core::Error("value", "height and width must be > 0");
        if (fbox[0] < 0 || fbox[1] < 0) throw core::Error("value", "box offset can't be negative");
        if (fbox[2] > static_cast<float>(in->xsize) || fbox[3] > static_cast<float>(in->ysize)) {
            throw core::Error("value", "box can't exceed original image size");
        }
        if (fbox[2] - fbox[0] < 0 || fbox[3] - fbox[1] < 0) throw core::Error("value", "box can't be empty");
        Imaging out = nullptr;
        if (fbox[0] - static_cast<float>(static_cast<int>(fbox[0])) == 0 && fbox[2] - fbox[0] == static_cast<float>(to.width) &&
            fbox[1] - static_cast<float>(static_cast<int>(fbox[1])) == 0 && fbox[3] - fbox[1] == static_cast<float>(to.height)) {
            out = ImagingCrop(in, static_cast<int>(fbox[0]), static_cast<int>(fbox[1]), static_cast<int>(fbox[2]),
                              static_cast<int>(fbox[3]));
        } else if (resample == Resample::Nearest) {
            double a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            a[0] = static_cast<double>(fbox[2] - fbox[0]) / to.width;
            a[4] = static_cast<double>(fbox[3] - fbox[1]) / to.height;
            a[2] = fbox[0];
            a[5] = fbox[1];
            out = ImagingNewDirty(in->mode, to.width, to.height);
            if (out != nullptr) {
                Imaging done = ImagingTransform(out, in, IMAGING_TRANSFORM_AFFINE, 0, 0, to.width, to.height, a,
                                                filter_id(resample), 1);
                if (done == nullptr) {
                    ImagingDelete(out);
                    out = nullptr;
                }
            }
        } else {
            out = ImagingResample(in, to.width, to.height, filter_id(resample), fbox);
        }
        return src.derived(out);
    };

    if (self->height() > self->width() * 100 && size.height < self->height()) {
        const Image tall = core_resize(*self, Size{self->width(), size.height}, BoxF{0, box.y0, static_cast<double>(self->width()), box.y1});
        return core_resize(tall, size, BoxF{box.x0, 0, box.x1, static_cast<double>(size.height)});
    }
    return core_resize(*self, size, box);
}

Image Image::resize_region(Size size, const Box& region, Resample resample, std::optional<BoxF> box_in) const {
    require();
    if (region.x0 < 0 || region.y0 < 0 || region.x1 > size.width || region.y1 > size.height || region.x1 <= region.x0 ||
        region.y1 <= region.y0) {
        throw core::Error("value", "the region is not inside the size");
    }
    if (region == Box{0, 0, size.width, size.height}) return resize(size, resample, box_in);
    const BoxF box = box_in.value_or(BoxF{0, 0, static_cast<double>(width()), static_cast<double>(height())});
    const bool full_box = box.x0 == 0 && box.y0 == 0 && box.x1 == width() && box.y1 == height();
    if (this->size() == size && full_box) return crop(region);
    const std::string_view m = mode();
    if (m == "1" || m == "P") resample = Resample::Nearest;
    if ((m == "LA" || m == "RGBA") && resample != Resample::Nearest) {
        const Image pre = convert(m == "LA" ? "La" : "RGBa");
        return pre.resize_region(size, region, resample, box).convert(m);
    }
    // (the rare cases Pillow does in other ways: the whole picture, then the part)
    if (resample == Resample::Nearest || (height() > width() * 100 && size.height < height())) {
        return resize(size, resample, box).crop(region);
    }
    float fbox[4] = {static_cast<float>(box.x0), static_cast<float>(box.y0), static_cast<float>(box.x1), static_cast<float>(box.y1)};
    if (size.width < 1 || size.height < 1) throw core::Error("value", "height and width must be > 0");
    if (fbox[0] < 0 || fbox[1] < 0) throw core::Error("value", "box offset can't be negative");
    if (fbox[2] > static_cast<float>(im_->xsize) || fbox[3] > static_cast<float>(im_->ysize)) {
        throw core::Error("value", "box can't exceed original image size");
    }
    if (fbox[2] - fbox[0] < 0 || fbox[3] - fbox[1] < 0) throw core::Error("value", "box can't be empty");
    if (fbox[0] - static_cast<float>(static_cast<int>(fbox[0])) == 0 && fbox[2] - fbox[0] == static_cast<float>(size.width) &&
        fbox[1] - static_cast<float>(static_cast<int>(fbox[1])) == 0 && fbox[3] - fbox[1] == static_cast<float>(size.height)) {
        const int bx = static_cast<int>(fbox[0]);
        const int by = static_cast<int>(fbox[1]);
        return derived(ImagingCrop(im_, bx + region.x0, by + region.y0, bx + region.x1, by + region.y1));
    }
    return derived(genko_ImagingResampleRegion(im_, size.width, size.height, filter_id(resample), fbox, region.x0, region.y0,
                                               region.x1, region.y1));
}

Image Image::reduce(int factor_x, int factor_y, std::optional<Box> box_in) const {
    require();
    const Box box = box_in.value_or(Box{0, 0, width(), height()});
    if (factor_x == 1 && factor_y == 1 && box == Box{0, 0, width(), height()}) return copy();
    const std::string_view m = mode();
    if (m == "LA" || m == "RGBA") {
        const Image pre = convert(m == "LA" ? "La" : "RGBa");
        return pre.reduce(factor_x, factor_y, box).convert(m);
    }
    if (factor_x < 1 || factor_y < 1) throw core::Error("value", "scale must be > 0");
    if (box.x0 < 0 || box.y0 < 0) throw core::Error("value", "box offset can't be negative");
    if (box.x1 > width() || box.y1 > height()) throw core::Error("value", "box can't exceed original image size");
    if (box.x1 <= box.x0 || box.y1 <= box.y0) throw core::Error("value", "box can't be empty");
    if (factor_x == 1 && factor_y == 1) return derived(ImagingCrop(im_, box.x0, box.y0, box.x1, box.y1));
    int b[4] = {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
    return derived(ImagingReduce(im_, factor_x, factor_y, b));
}

Image Image::transpose(Transpose method) const {
    require();
    const int op = static_cast<int>(method);
    Imaging out = nullptr;
    if (op == 0 || op == 1 || op == 3) {
        out = check(ImagingNewDirty(im_->mode, im_->xsize, im_->ysize));
    } else {
        out = check(ImagingNewDirty(im_->mode, im_->ysize, im_->xsize));
    }
    switch (method) {
        case Transpose::FlipLeftRight: (void)ImagingFlipLeftRight(out, im_); break;
        case Transpose::FlipTopBottom: (void)ImagingFlipTopBottom(out, im_); break;
        case Transpose::Rotate90: (void)ImagingRotate90(out, im_); break;
        case Transpose::Rotate180: (void)ImagingRotate180(out, im_); break;
        case Transpose::Rotate270: (void)ImagingRotate270(out, im_); break;
        case Transpose::Transpose: (void)ImagingTranspose(out, im_); break;
        case Transpose::Transverse: (void)ImagingTransverse(out, im_); break;
    }
    return derived(out);
}

Image Image::rotate(double angle, Resample resample, bool expand, std::optional<std::pair<double, double>> center,
                    std::optional<std::pair<int, int>> translate, std::optional<Ink> fillcolor) const {
    require();
    angle = core::py_fmod(angle, 360.0);
    if (!center && !translate) {
        if (angle == 0) return copy();
        if (angle == 180) return transpose(Transpose::Rotate180);
        if ((angle == 90 || angle == 270) && (expand || width() == height())) {
            return transpose(angle == 90 ? Transpose::Rotate90 : Transpose::Rotate270);
        }
    }
    int w = width();
    int h = height();
    const std::pair<int, int> post = translate.value_or(std::pair<int, int>{0, 0});
    const std::pair<double, double> c = center.value_or(std::pair<double, double>{w / 2.0, h / 2.0});
    const double rad = -(angle * (core::kPi / 180.0));  // -math.radians(angle)
    std::vector<double> matrix{core::py_round(core::py_cos(rad), 15), core::py_round(core::py_sin(rad), 15), 0.0,
                               core::py_round(-core::py_sin(rad), 15), core::py_round(core::py_cos(rad), 15), 0.0};
    const auto apply = [](double x, double y, const std::vector<double>& m) {
        return std::pair<double, double>{m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]};
    };
    const auto [tx, ty] = apply(-c.first - post.first, -c.second - post.second, matrix);
    matrix[2] = tx;
    matrix[5] = ty;
    matrix[2] += c.first;
    matrix[5] += c.second;
    if (expand) {
        std::vector<double> xx;
        std::vector<double> yy;
        for (const auto& [x, y] : {std::pair<double, double>{0, 0}, {static_cast<double>(w), 0}, {static_cast<double>(w), static_cast<double>(h)}, {0, static_cast<double>(h)}}) {
            const auto [px, py] = apply(x, y, matrix);
            xx.push_back(px);
            yy.push_back(py);
        }
        // max()/min() of Python floats (the first of equal values)
        const auto pymax = [](const std::vector<double>& v) {
            double m = v[0];
            for (double e : v) {
                if (e > m) m = e;
            }
            return m;
        };
        const auto pymin = [](const std::vector<double>& v) {
            double m = v[0];
            for (double e : v) {
                if (e < m) m = e;
            }
            return m;
        };
        const int nw = static_cast<int>(std::ceil(pymax(xx)) - std::floor(pymin(xx)));
        const int nh = static_cast<int>(std::ceil(pymax(yy)) - std::floor(pymin(yy)));
        const auto [ex, ey] = apply(-(nw - w) / 2.0, -(nh - h) / 2.0, matrix);
        matrix[2] = ex;
        matrix[5] = ey;
        w = nw;
        h = nh;
    }
    return transform(Size{w, h}, TransformMethod::Affine, matrix, resample, fillcolor);
}

Image Image::transform(Size size, TransformMethod method, std::span<const double> data_in, Resample resample,
                       std::optional<Ink> fillcolor) const {
    require();
    const std::string_view m = mode();
    if ((m == "LA" || m == "RGBA") && resample != Resample::Nearest) {
        return convert(m == "LA" ? "La" : "RGBa").transform(size, method, data_in, resample, fillcolor).convert(m);
    }
    Image out = fillcolor ? create(m, size, *fillcolor) : create_blank(m, size);
    if (m == "P" && im_->palette != nullptr) ImagingCopyPalette(out.im_, im_);
    out.transparency_ = transparency_;
    const int w = size.width;
    const int h = size.height;
    std::vector<double> data(data_in.begin(), data_in.end());
    int core_method = IMAGING_TRANSFORM_AFFINE;
    std::size_t needed = 6;
    switch (method) {
        case TransformMethod::Affine:
            if (data.size() < 6) throw core::Error("value", "wrong number of matrix entries");
            data.resize(6);
            break;
        case TransformMethod::Extent: {
            if (data.size() != 4) throw core::Error("value", "wrong number of matrix entries");
            const double xs = (data[2] - data[0]) / w;
            const double ys = (data[3] - data[1]) / h;
            data = {xs, 0, data[0], 0, ys, data[1]};
            break;
        }
        case TransformMethod::Perspective:
            if (data.size() < 8) throw core::Error("value", "wrong number of matrix entries");
            data.resize(8);
            core_method = IMAGING_TRANSFORM_PERSPECTIVE;
            needed = 8;
            break;
        case TransformMethod::Quad: {
            if (data.size() < 8) throw core::Error("value", "wrong number of matrix entries");
            const double x0 = data[0];
            const double y0 = data[1];
            const double as = 1.0 / w;
            const double at = 1.0 / h;
            const double nw0 = data[0], sw0 = data[2], se0 = data[4], ne0 = data[6];
            const double nw1 = data[1], sw1 = data[3], se1 = data[5], ne1 = data[7];
            (void)nw0;
            (void)nw1;
            data = {x0, (ne0 - x0) * as, (sw0 - x0) * at, (se0 - sw0 - ne0 + x0) * as * at,
                    y0, (ne1 - y0) * as, (sw1 - y0) * at, (se1 - sw1 - ne1 + y0) * as * at};
            core_method = IMAGING_TRANSFORM_QUAD;
            needed = 8;
            break;
        }
    }
    if (data.size() != needed) throw core::Error("value", "wrong number of matrix entries");
    if (m == "1" || m == "P") resample = Resample::Nearest;
    if (resample != Resample::Nearest && resample != Resample::Bilinear && resample != Resample::Bicubic) {
        throw core::Error("value", "the resampling filter cannot be used with transform");
    }
    double a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < data.size(); ++i) a[i] = data[i];
    check(ImagingTransform(out.im_, im_, core_method, 0, 0, w, h, a, filter_id(resample), fillcolor ? 0 : 1));
    return out;
}

void Image::thumbnail(Size size, Resample resample, std::optional<double> reducing_gap) {
    require();
    int x = size.width;
    int y = size.height;
    if (x >= width() && y >= height()) return;
    const double aspect = static_cast<double>(width()) / height();
    // round_aspect: max(min(floor, ceil, key=key), 1)
    const auto round_aspect = [](double number, const std::function<double(int)>& key) {
        const auto lo = static_cast<int>(std::floor(number));
        const auto hi = static_cast<int>(std::ceil(number));
        const int best = key(hi) < key(lo) ? hi : lo;
        return std::max(best, 1);
    };
    if (static_cast<double>(x) / y >= aspect) {
        const int yy = y;
        x = round_aspect(yy * aspect, [&](int n) { return std::fabs(aspect - static_cast<double>(n) / yy); });
    } else {
        const int xx = x;
        y = round_aspect(xx / aspect, [&](int n) { return n == 0 ? 0.0 : std::fabs(aspect - static_cast<double>(xx) / n); });
    }
    const Size final_size{x, y};
    if (this->size() != final_size) {
        Image im = resize(final_size, resample, std::nullopt, reducing_gap);
        *this = std::move(im);
    }
}

// --- Image: filters and statistics ------------------------------------------------------------------------------------

Image Image::filter(const Filter& f) const {
    require();
    const auto one = [&](const Image& src) -> Image {
        Imaging in = src.im_;
        switch (f.kind) {
            case Filter::Kind::Kernel: {
                if (in->mode == IMAGING_MODE_P) throw core::Error("value", "cannot filter palette images");
                const auto divisor = static_cast<float>(f.scale);
                std::vector<FLOAT32> kernel;
                for (const double w : f.weights) kernel.push_back(static_cast<FLOAT32>(w) / divisor);
                return src.derived(ImagingFilter(in, f.kernel_width, f.kernel_height, kernel.data(), static_cast<FLOAT32>(f.offset)));
            }
            case Filter::Kind::Rank: {
                if (in->mode == IMAGING_MODE_P) throw core::Error("value", "cannot filter palette images");
                Image expanded = src.derived(ImagingExpand(in, f.size / 2));
                return expanded.derived(ImagingRankFilter(expanded.im_, f.size, f.rank));
            }
            case Filter::Kind::Mode: return src.derived(ImagingModeFilter(in, f.size));
            case Filter::Kind::GaussianBlur: {
                if (f.radius_x == 0 && f.radius_y == 0) return src.copy();
                Image out = src.derived(ImagingNewDirty(in->mode, in->xsize, in->ysize));
                check(ImagingGaussianBlur(out.im_, in, static_cast<float>(f.radius_x), static_cast<float>(f.radius_y), 3));
                return out;
            }
            case Filter::Kind::BoxBlur: {
                if (f.radius_x == 0 && f.radius_y == 0) return src.copy();
                Image out = src.derived(ImagingNewDirty(in->mode, in->xsize, in->ysize));
                check(ImagingBoxBlur(out.im_, in, static_cast<float>(f.radius_x), static_cast<float>(f.radius_y), 1));
                return out;
            }
            case Filter::Kind::UnsharpMask: {
                Image out = src.derived(ImagingNewDirty(in->mode, in->xsize, in->ysize));
                check(ImagingUnsharpMask(out.im_, in, static_cast<float>(f.radius_x), f.percent, f.threshold));
                return out;
            }
        }
        throw core::Error("value", "unknown filter");
    };
    const bool multiband = f.kind != Filter::Kind::Rank && f.kind != Filter::Kind::Mode;
    if (im_->bands == 1 || multiband) return one(*this);
    std::vector<Image> ims;
    for (int c = 0; c < im_->bands; ++c) ims.push_back(one(getchannel(c)));
    return merge(mode(), ims);
}

std::optional<Box> Image::getbbox(bool alpha_only) const {
    require();
    int bbox[4] = {0, 0, 0, 0};
    if (!ImagingGetBBox(im_, bbox, alpha_only ? 1 : 0)) return std::nullopt;
    return Box{bbox[0], bbox[1], bbox[2], bbox[3]};
}

std::vector<std::pair<double, double>> Image::getextrema() const {
    require();
    const auto band_extrema = [](Imaging im) -> std::pair<double, double> {
        union {
            UINT8 u[2];
            INT32 i[2];
            FLOAT32 f[2];
            UINT16 s[2];
        } extrema{};
        const int status = ImagingGetExtrema(im, &extrema);
        if (status < 0) detail::throw_imaging_error();
        if (status == 0) return {0.0, 0.0};
        switch (im->type) {
            case IMAGING_TYPE_UINT8: return {extrema.u[0], extrema.u[1]};
            case IMAGING_TYPE_INT32: return {extrema.i[0], extrema.i[1]};
            case IMAGING_TYPE_FLOAT32: return {extrema.f[0], extrema.f[1]};
            default: return {extrema.s[0], extrema.s[1]};
        }
    };
    std::vector<std::pair<double, double>> out;
    if (im_->bands > 1) {
        for (int i = 0; i < im_->bands; ++i) {
            const Image band = getchannel(i);
            out.push_back(band_extrema(band.im_));
        }
    } else {
        out.push_back(band_extrema(im_));
    }
    return out;
}

std::vector<long> Image::histogram() const {
    require();
    const ImagingHistogram h = ImagingGetHistogram(im_, nullptr, nullptr);
    if (h == nullptr) detail::throw_imaging_error();
    std::vector<long> out(h->histogram, h->histogram + static_cast<std::ptrdiff_t>(h->bands) * 256);
    ImagingHistogramDelete(h);
    return out;
}

// --- module functions ---------------------------------------------------------------------------------------------------

Image alpha_composite(const Image& a, const Image& b) {
    if (a.empty() || b.empty()) throw core::Error("value", "no image");
    Image out = Image::adopt(ImagingAlphaComposite(a.raw(), b.raw()));
    out.set_transparency(a.transparency());
    return out;
}

Image blend(const Image& a, const Image& b, double alpha) {
    if (a.empty() || b.empty()) throw core::Error("value", "no image");
    Image out = Image::adopt(ImagingBlend(a.raw(), b.raw(), static_cast<float>(alpha)));
    out.set_transparency(a.transparency());
    return out;
}

Image composite(const Image& a, const Image& b, const Image& mask) {
    Image image = b.copy();
    image.paste(a, Point{0, 0}, &mask);
    return image;
}

namespace chops {

namespace {

Image of(const Image& first, Imaging made) {
    Image out = Image::adopt(made);
    out.set_transparency(first.transparency());
    return out;
}

void both(const Image& a, const Image& b) {
    if (a.empty() || b.empty()) throw core::Error("value", "no image");
}

}  // namespace

Image invert(const Image& image) {
    if (image.empty()) throw core::Error("value", "no image");
    return of(image, ImagingNegative(image.raw()));
}
Image lighter(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopLighter(a.raw(), b.raw())); }
Image darker(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopDarker(a.raw(), b.raw())); }
Image difference(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopDifference(a.raw(), b.raw())); }
Image multiply(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopMultiply(a.raw(), b.raw())); }
Image screen(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopScreen(a.raw(), b.raw())); }
Image soft_light(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopSoftLight(a.raw(), b.raw())); }
Image hard_light(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopHardLight(a.raw(), b.raw())); }
Image overlay(const Image& a, const Image& b) { both(a, b); return of(a, ImagingOverlay(a.raw(), b.raw())); }
Image add(const Image& a, const Image& b, double scale, int offset) {
    both(a, b);
    return of(a, ImagingChopAdd(a.raw(), b.raw(), static_cast<float>(scale), offset));
}
Image subtract(const Image& a, const Image& b, double scale, int offset) {
    both(a, b);
    return of(a, ImagingChopSubtract(a.raw(), b.raw(), static_cast<float>(scale), offset));
}
Image add_modulo(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopAddModulo(a.raw(), b.raw())); }
Image subtract_modulo(const Image& a, const Image& b) {
    both(a, b);
    return of(a, ImagingChopSubtractModulo(a.raw(), b.raw()));
}
Image logical_and(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopAnd(a.raw(), b.raw())); }
Image logical_or(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopOr(a.raw(), b.raw())); }
Image logical_xor(const Image& a, const Image& b) { both(a, b); return of(a, ImagingChopXor(a.raw(), b.raw())); }
Image offset(const Image& image, int x, std::optional<int> y) {
    if (image.empty()) throw core::Error("value", "no image");
    return of(image, ImagingOffset(image.raw(), x, y.value_or(x)));
}

}  // namespace chops

namespace ops {

Image grayscale(const Image& image) { return image.convert("L"); }

Image invert(const Image& image) {
    std::vector<int> lut(256);
    for (int i = 0; i < 256; ++i) lut[static_cast<std::size_t>(i)] = 255 - i;
    const std::string_view m = image.mode();
    if (m == "1") return image.point(lut);
    if (m == "P") throw core::Error("value", "mode P support coming soon");
    if (m == "L") return image.point(lut);
    if (m == "RGB") {
        std::vector<int> three;
        for (int k = 0; k < 3; ++k) three.insert(three.end(), lut.begin(), lut.end());
        return image.point(three);
    }
    throw core::Error("value", "not supported for mode " + std::string(m));
}

}  // namespace ops

}  // namespace genko::render
