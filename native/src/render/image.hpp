#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pillow's Image, ImageChops and ImageOps (the Python layer of Pillow 12.3.0) over its C library libImaging, which
// is built in native/third_party/pillow. Each operation does what the same call does in the Python baseline, with the
// same defaults, conversions and checks, so the pixels are the same (docs/cpp-migration/ARCHITECTURE.md §4a).
//
// Errors are core::Error: "value" for what Pillow raises as ValueError/TypeError (with Pillow's message), "memory"
// when an image cannot be allocated, "image_too_large" for what Pillow refuses as a decompression bomb.

struct ImagingMemoryInstance;

namespace genko::render {

struct Size {
    int width = 0;
    int height = 0;

    friend bool operator==(const Size&, const Size&) = default;
};

struct Point {
    int x = 0;
    int y = 0;

    friend bool operator==(const Point&, const Point&) = default;
};

// A box of pixels: left, upper, right, lower (right and lower outside), as Pillow's 4-tuples.
struct Box {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    friend bool operator==(const Box&, const Box&) = default;
};

struct BoxF {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

// A colour as Python hands one to Pillow: an int, or a tuple of ints ((r, g, b), (r, g, b, a), (l, a), …). How it
// becomes a pixel depends on the image (Pillow's getink): a tuple of three on an RGBA image is opaque, an int on an
// RGB(A) image is the packed 0xAABBGGRR, values are clipped to 0..255.
class Ink {
public:
    Ink(int value) : values_{value}, tuple_(false) {}  // NOLINT(google-explicit-constructor): Pillow's ints
    Ink(std::initializer_list<std::int64_t> values) : values_(values), tuple_(true) {}
    static Ink tuple(std::vector<std::int64_t> values);
    // (*rgb, alpha): Python's tuple(rgb) + (alpha,)
    static Ink with_alpha(std::span<const std::int64_t> rgb, std::int64_t alpha);

    bool is_tuple() const { return tuple_; }
    const std::vector<std::int64_t>& values() const { return values_; }
    friend bool operator==(const Ink&, const Ink&) = default;

private:
    Ink() = default;
    std::vector<std::int64_t> values_;
    bool tuple_ = true;
};

enum class Resample { Nearest = 0, Lanczos = 1, Bilinear = 2, Bicubic = 3, Box = 4, Hamming = 5 };
enum class Transpose { FlipLeftRight = 0, FlipTopBottom = 1, Rotate90 = 2, Rotate180 = 3, Rotate270 = 4, Transpose = 5, Transverse = 6 };
enum class TransformMethod { Affine = 0, Extent = 1, Perspective = 2, Quad = 3 };
enum class Dither { None = 0, FloydSteinberg = 3 };

// The transparent colour PNG files carry (Pillow's im.info["transparency"]): a palette index or grey level (Index),
// an RGB colour (Rgb) or one alpha per palette entry (Bytes).
struct Transparency {
    enum class Kind { None, Index, Rgb, Bytes };
    Kind kind = Kind::None;
    int value = 0;
    std::array<int, 3> rgb{};
    std::string bytes;

    bool present() const { return kind != Kind::None; }
    friend bool operator==(const Transparency&, const Transparency&) = default;
};

// ImageFilter's filters.
struct Filter {
    enum class Kind { Kernel, Rank, Mode, GaussianBlur, BoxBlur, UnsharpMask };
    Kind kind = Kind::GaussianBlur;
    // Kernel: its size (3×3 or 5×5), scale (the sum of the weights by default), offset and weights.
    int kernel_width = 0;
    int kernel_height = 0;
    double scale = 1.0;
    double offset = 0.0;
    std::vector<double> weights;
    // Rank / Mode: the window size and (rank) which value is picked.
    int size = 3;
    int rank = 0;
    // Gaussian / box blur radii; unsharp mask: radius, percent, threshold.
    double radius_x = 2.0;
    double radius_y = 2.0;
    int percent = 150;
    int threshold = 3;

    static Filter gaussian_blur(double radius = 2.0);
    static Filter gaussian_blur(double radius_x, double radius_y);
    static Filter box_blur(double radius);
    static Filter box_blur(double radius_x, double radius_y);
    static Filter rank_filter(int size, int rank);
    static Filter min_filter(int size = 3);
    static Filter max_filter(int size = 3);
    static Filter median_filter(int size = 3);
    static Filter mode_filter(int size = 3);
    static Filter unsharp_mask(double radius = 2.0, int percent = 150, int threshold = 3);
    static Filter kernel(int width, int height, std::vector<double> weights, std::optional<double> scale = std::nullopt,
                         double offset = 0.0);
    // The built-in kernels: BLUR, CONTOUR, DETAIL, EDGE_ENHANCE, EDGE_ENHANCE_MORE, EMBOSS, FIND_EDGES, SHARPEN,
    // SMOOTH, SMOOTH_MORE.
    static Filter blur();
    static Filter contour();
    static Filter detail();
    static Filter edge_enhance();
    static Filter edge_enhance_more();
    static Filter emboss();
    static Filter find_edges();
    static Filter sharpen();
    static Filter smooth();
    static Filter smooth_more();
};

// A picture: mode (Pillow's: "1", "L", "LA", "La", "P", "PA", "I", "I;16", "F", "RGB", "RGBA", "RGBa", "RGBX",
// "HSV", "CMYK", "YCbCr"), size and pixels. A value: copying copies the pixels, moving is cheap. An Image made with
// the default constructor holds nothing (empty()).
class Image {
public:
    Image() noexcept = default;
    ~Image();
    Image(const Image& other);
    Image& operator=(const Image& other);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    // Image.new(mode, size, colour).
    static Image create(std::string_view mode, Size size, const Ink& colour = Ink(0));
    // Image.new(mode, size, None): not filled with a colour (all zero here).
    static Image create_blank(std::string_view mode, Size size);
    // Image.frombytes(mode, size, data[, "raw", rawmode]).
    static Image frombytes(std::string_view mode, Size size, std::string_view data, std::string_view rawmode = {});

    bool empty() const { return im_ == nullptr; }
    std::string_view mode() const;
    Size size() const;
    int width() const;
    int height() const;
    int bands() const;
    // The band names of the mode ("R", "G", "B", "A" for RGBA).
    std::vector<std::string> getbands() const;

    // The pixels packed as the mode's own raw mode (Image.tobytes()).
    std::string tobytes() const;
    // One pixel: each band's value (getpixel), x and y counted from the end when negative.
    std::vector<double> getpixel(int x, int y) const;

    Image copy() const;
    // crop(box): parts outside the image are zero. BoxF rounds as Pillow does (round half to even).
    Image crop(const Box& box) const;
    Image crop(const BoxF& box) const;

    // paste(im, (x, y) | box, mask): im is converted to this image's mode first (except RGBA/LA onto RGB).
    void paste(const Image& im, Point at = {}, const Image* mask = nullptr);
    void paste(const Image& im, const Box& box, const Image* mask = nullptr);
    // paste(colour, box, mask)
    void paste(const Ink& colour, const Box& box, const Image* mask = nullptr);
    // paste(colour, mask): the mask's size from the corner.
    void paste(const Ink& colour, const Image& mask);

    // Image.alpha_composite(self, im, dest, source): im over this image, in place.
    void alpha_composite(const Image& im, Point dest = {}, std::optional<Box> source = std::nullopt);

    Image convert(std::string_view mode, Dither dither = Dither::FloydSteinberg) const;

    // point(lut[, mode]): lut holds 256 values per band (clipped to 0..255), or 256 when mode is "1" or "L" from "L"
    // or "P".
    Image point(std::span<const int> lut, std::string_view mode = {}) const;
    // point(function[, mode]): the function's value for 0..255, for every band.
    Image point(const std::function<int(int)>& fn, std::string_view mode = {}) const;

    // putalpha(alpha): an "L" or "1" image, or a constant.
    void putalpha(const Image& alpha);
    void putalpha(int alpha);

    std::vector<Image> split() const;
    Image getchannel(int band) const;
    Image getchannel(std::string_view name) const;
    static Image merge(std::string_view mode, const std::vector<Image>& bands);

    // resize(size, resample=BICUBIC, box=None, reducing_gap=None)
    Image resize(Size size, Resample resample = Resample::Bicubic, std::optional<BoxF> box = std::nullopt,
                 std::optional<double> reducing_gap = std::nullopt) const;
    // The part `region` (a box inside size) of resize(size, resample, box): the same pixels as cropping the whole
    // resized picture, without computing the rest.
    Image resize_region(Size size, const Box& region, Resample resample = Resample::Bicubic,
                        std::optional<BoxF> box = std::nullopt) const;
    Image reduce(int factor_x, int factor_y, std::optional<Box> box = std::nullopt) const;
    // rotate(angle, resample=NEAREST, expand=False, center=None, translate=None, fillcolor=None)
    Image rotate(double angle, Resample resample = Resample::Nearest, bool expand = false,
                 std::optional<std::pair<double, double>> center = std::nullopt,
                 std::optional<std::pair<int, int>> translate = std::nullopt,
                 std::optional<Ink> fillcolor = std::nullopt) const;
    // transform(size, method, data, resample=NEAREST, fillcolor=None)
    Image transform(Size size, TransformMethod method, std::span<const double> data, Resample resample = Resample::Nearest,
                    std::optional<Ink> fillcolor = std::nullopt) const;
    Image transpose(Transpose method) const;
    // thumbnail(size, resample=BICUBIC, reducing_gap=2.0), in place.
    void thumbnail(Size size, Resample resample = Resample::Bicubic, std::optional<double> reducing_gap = 2.0);

    Image filter(const Filter& filter) const;

    // getbbox(alpha_only=True): the box of the non-zero pixels (of the alpha band only, when there is one).
    std::optional<Box> getbbox(bool alpha_only = true) const;
    // getextrema(): (min, max) of each band.
    std::vector<std::pair<double, double>> getextrema() const;
    // histogram(): 256 counts per band.
    std::vector<long> histogram() const;

    const Transparency& transparency() const { return transparency_; }
    void set_transparency(Transparency t) { transparency_ = std::move(t); }

    // The libImaging image (for render's own code; null when empty()).
    ImagingMemoryInstance* raw() const { return im_; }
    // Takes ownership of a libImaging image; throws the pending libImaging error when it is null.
    static Image adopt(ImagingMemoryInstance* im);

private:
    explicit Image(ImagingMemoryInstance* im) noexcept : im_(im) {}
    // A new image with this one's info (Pillow's Image._new).
    Image derived(ImagingMemoryInstance* im) const;
    void require() const;

    ImagingMemoryInstance* im_ = nullptr;
    Transparency transparency_;
};

// Image.alpha_composite(a, b): b over a (both RGBA, or both LA, of one size).
Image alpha_composite(const Image& a, const Image& b);
// Image.blend(a, b, alpha): a * (1 - alpha) + b * alpha.
Image blend(const Image& a, const Image& b, double alpha);
// Image.composite(a, b, mask): a where the mask is white, b where it is black.
Image composite(const Image& a, const Image& b, const Image& mask);

// ImageChops.
namespace chops {
Image invert(const Image& image);
Image lighter(const Image& a, const Image& b);
Image darker(const Image& a, const Image& b);
Image difference(const Image& a, const Image& b);
Image multiply(const Image& a, const Image& b);
Image screen(const Image& a, const Image& b);
Image soft_light(const Image& a, const Image& b);
Image hard_light(const Image& a, const Image& b);
Image overlay(const Image& a, const Image& b);
Image add(const Image& a, const Image& b, double scale = 1.0, int offset = 0);
Image subtract(const Image& a, const Image& b, double scale = 1.0, int offset = 0);
Image add_modulo(const Image& a, const Image& b);
Image subtract_modulo(const Image& a, const Image& b);
Image logical_and(const Image& a, const Image& b);
Image logical_or(const Image& a, const Image& b);
Image logical_xor(const Image& a, const Image& b);
Image offset(const Image& image, int x, std::optional<int> y = std::nullopt);
}  // namespace chops

// ImageOps.
namespace ops {
Image grayscale(const Image& image);
Image invert(const Image& image);
}  // namespace ops

// Pillow's MAX_IMAGE_PIXELS: crops beyond twice this are refused, and so are the pictures the drawing opens
// (png.hpp: kPillowOpenLimits).
inline constexpr std::int64_t kMaxImagePixels = 89478485;

}  // namespace genko::render
