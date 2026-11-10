#include "render/fill.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/draw.hpp"
#include "render/imaging.hpp"
#include "render/op_limits.hpp"
#include "render/png.hpp"

namespace genko::render {

bool BoolGrid::any() const { return std::any_of(cells.begin(), cells.end(), [](unsigned char c) { return c != 0; }); }

Image grid_image(const BoolGrid& grid) {
    std::string data(grid.cells.size(), '\0');
    for (std::size_t i = 0; i < grid.cells.size(); ++i) data[i] = grid.cells[i] != 0 ? static_cast<char>(255) : '\0';
    return Image::frombytes("L", Size{grid.width, grid.height}, data);
}

BoolGrid compare(const Image& picture, int threshold, bool above) {
    const std::string values = picture.tobytes();
    BoolGrid out(picture.width(), picture.height());
    for (std::size_t i = 0; i < values.size() && i < out.cells.size(); ++i) {
        const int v = static_cast<unsigned char>(values[i]);
        out.cells[i] = (above ? v > threshold : v < threshold) ? 1 : 0;
    }
    return out;
}

namespace limits {

void check_picture(std::int64_t width, std::int64_t height, std::string_view what) {
    if (width > kSide || height > kSide || (width > 0 && height > 0 && width * height > kPixels)) {
        throw core::OpError(std::string(what) + " is too large (" + std::to_string(width) + "×" + std::to_string(height) +
                            " pixels; at most " + std::to_string(kPixels) + ")");
    }
}

void check_coordinate(double x, std::string_view what) {
    if (!(x <= kCoordinate && x >= -kCoordinate)) throw core::OpError(std::string(what) + " is too far off the page");
}

void check_count(double n, double limit, std::string_view what) {
    if (!(n <= limit)) {
        throw core::OpError(std::string(what) + " is too large (" + core::py_float_repr(n) + "; at most " +
                            core::py_float_repr(limit) + ")");
    }
}

void check_sides(double width, double height, std::string_view what) {
    const auto shown = [](double v) { return std::fabs(v) < 9e18 ? std::to_string(static_cast<std::int64_t>(v)) : core::py_float_repr(v); };
    if (!(std::fabs(width) <= static_cast<double>(kSide)) || !(std::fabs(height) <= static_cast<double>(kSide))) {
        throw core::OpError(std::string(what) + " is too large (" + shown(width) + "×" + shown(height) + " pixels; at most " +
                            std::to_string(kPixels) + ")");
    }
    check_picture(static_cast<std::int64_t>(width), static_cast<std::int64_t>(height), what);
}

}  // namespace limits

namespace fills {

double px(double mm, int dpi) { return core::py_round_whole(mm / 25.4 * dpi); }

BoolGrid region(const BoolGrid& free, double sx_in, double sy_in) {
    const int w = free.width;
    const int h = free.height;
    BoolGrid filled(w, h);
    if (!(0 <= sx_in && sx_in < w && 0 <= sy_in && sy_in < h)) return filled;
    const int sx = static_cast<int>(sx_in);
    const int sy = static_cast<int>(sy_in);
    if (free.at(sx, sy) == 0) return filled;
    // the spans of free pixels reached from the seed (the same pixels as Python's span fill, in any order)
    std::vector<std::pair<int, int>> stack{{sy, sx}};
    while (!stack.empty()) {
        const auto [y, x] = stack.back();
        stack.pop_back();
        if (filled.at(x, y) != 0 || free.at(x, y) == 0) continue;
        int left = x;
        while (left > 0 && free.at(left - 1, y) != 0) --left;
        int right = x;
        while (right + 1 < w && free.at(right + 1, y) != 0) ++right;
        for (int i = left; i <= right; ++i) filled.at(i, y) = 1;
        for (const int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h) continue;
            bool before = false;
            for (int i = left; i <= right; ++i) {
                const bool open = free.at(i, ny) != 0 && filled.at(i, ny) == 0;
                if (open && !before) stack.emplace_back(ny, i);
                before = open;
            }
        }
    }
    return filled;
}

BoolGrid dilate(const BoolGrid& grid, double r_in) {
    if (!(r_in > 0)) return grid;
    // (beyond the grid's size a step changes nothing: Python's slices are empty there)
    const int r = static_cast<int>(std::min(r_in, static_cast<double>(std::max(grid.width, grid.height))));
    const int w = grid.width;
    const int h = grid.height;
    BoolGrid out = grid;
    // down the columns, then along the rows: each pixel true when one within r of it (in that direction) is
    std::vector<int> sums;
    BoolGrid grown(w, h);
    sums.assign(static_cast<std::size_t>(h) + 1, 0);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) sums[static_cast<std::size_t>(y) + 1] = sums[static_cast<std::size_t>(y)] + out.at(x, y);
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0, y - r);
            const int hi = std::min(h - 1, y + r);
            grown.at(x, y) = sums[static_cast<std::size_t>(hi) + 1] - sums[static_cast<std::size_t>(lo)] > 0 ? 1 : 0;
        }
    }
    out = std::move(grown);
    grown = BoolGrid(w, h);
    sums.assign(static_cast<std::size_t>(w) + 1, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) sums[static_cast<std::size_t>(x) + 1] = sums[static_cast<std::size_t>(x)] + out.at(x, y);
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - r);
            const int hi = std::min(w - 1, x + r);
            grown.at(x, y) = sums[static_cast<std::size_t>(hi) + 1] - sums[static_cast<std::size_t>(lo)] > 0 ? 1 : 0;
        }
    }
    return grown;
}

std::optional<Image> region_mask(const Image& reference, double at_x, double at_y, double gap_px, int threshold,
                                 double expand_px, const std::optional<std::array<double, 4>>& window) {
    const Image grey = reference.mode() == "L" ? reference : reference.convert("L");
    std::array<double, 4> box{0.0, 0.0, static_cast<double>(grey.width()), static_cast<double>(grey.height())};
    if (window) box = *window;
    // grey.crop(window): its corners within the page (the op keeps them there), unless they cross
    if (box[2] < box[0]) throw core::PyValueError("Coordinate 'right' is less than 'left'");
    if (box[3] < box[1]) throw core::PyValueError("Coordinate 'lower' is less than 'upper'");
    const Box crop_box{static_cast<int>(box[0]), static_cast<int>(box[1]), static_cast<int>(box[2]), static_cast<int>(box[3])};
    BoolGrid walls = compare(grey.crop(crop_box), threshold);
    // the window's edge is a wall too (a fill never leaves its panel's box)
    if (walls.height == 0) throw core::PyUncaught("IndexError", "index 0 is out of bounds for axis 0 with size 0");
    for (int x = 0; x < walls.width; ++x) walls.at(x, 0) = walls.at(x, walls.height - 1) = 1;
    if (walls.width == 0) throw core::PyUncaught("IndexError", "index 0 is out of bounds for axis 1 with size 0");
    for (int y = 0; y < walls.height; ++y) walls.at(0, y) = walls.at(walls.width - 1, y) = 1;
    BoolGrid free = dilate(walls, gap_px);
    for (unsigned char& c : free.cells) c = c != 0 ? 0 : 1;
    BoolGrid filled = region(free, at_x - box[0], at_y - box[1]);
    if (!filled.any()) return std::nullopt;
    filled = dilate(filled, expand_px + gap_px);  // under the lines (and into the closed gaps)
    Image page = Image::create("L", grey.size(), Ink(0));
    page.paste(grid_image(filled), Point{crop_box.x0, crop_box.y0});
    return page;
}

namespace {

std::uint32_t crc32_of(std::string_view data, std::uint32_t crc = 0) {
    crc = ~crc;
    for (const char ch : data) {
        crc ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void put_be32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xffU));
}

std::string o16(int v) { return std::string{static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)}; }

// PngImagePlugin._save's tRNS chunk for the picture (nothing when it writes none)
std::optional<std::string> trns_of(const Image& image) {
    const std::string_view mode = image.mode();
    const Transparency& t = image.transparency();
    const auto colours = [&] {
        const ImagingMemoryInstance* im = image.raw();
        return im->palette != nullptr && im->palette->size > 0 ? std::min(im->palette->size, 256) : 256;
    };
    if (t.present()) {
        if (mode == "P") {
            const auto n = static_cast<std::size_t>(colours());
            if (t.kind == Transparency::Kind::Bytes) return t.bytes.substr(0, n);
            if (t.kind == Transparency::Kind::Index) {
                const int index = std::max(0, std::min(255, t.value));
                return (std::string(static_cast<std::size_t>(index), '\xff') + std::string(1, '\0')).substr(0, n);
            }
            throw core::PyValueError("transparency for P must be an integer or bytes");
        }
        if (mode == "1" || mode == "L" || mode == "I" || mode == "I;16") {
            if (t.kind != Transparency::Kind::Index) {
                throw core::PyValueError("transparency for " + std::string(mode) + " must be an integer");
            }
            return o16(std::max(0, std::min(65535, t.value)));
        }
        if (mode == "RGB") {
            if (t.kind != Transparency::Kind::Rgb) throw core::PyValueError("transparency for RGB must be list or tuple");
            return o16(t.rgb[0]) + o16(t.rgb[1]) + o16(t.rgb[2]);
        }
        return std::nullopt;  // (stale, from the picture's info: Pillow leaves it out)
    }
    const ImagingMemoryInstance* im = image.raw();
    if (mode == "P" && im->palette != nullptr && im->palette->mode == IMAGING_MODE_RGBA) {
        std::string alpha;
        for (int i = 0; i < colours(); ++i) alpha.push_back(static_cast<char>(im->palette->palette[i * 4 + 3]));
        return alpha;
    }
    return std::nullopt;
}

}  // namespace

std::string png_data(const Image& image) {
    std::string bytes = write_png(image);
    const std::optional<std::string> trns = trns_of(image);
    if (!trns) return bytes;
    // the chunk goes before the first IDAT (after IHDR and PLTE), as Pillow writes it
    std::size_t at = 8;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t length = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
                                     (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
                                     (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
                                     static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
        if (bytes.compare(at + 4, 4, "IDAT") == 0) break;
        at += 12 + static_cast<std::size_t>(length);
    }
    std::string chunk;
    put_be32(chunk, static_cast<std::uint32_t>(trns->size()));
    const std::string body = "tRNS" + *trns;
    chunk += body;
    put_be32(chunk, crc32_of(body));
    bytes.insert(std::min(at, bytes.size()), chunk);
    return bytes;
}

core::Bytes png_bytes(const Image& image) { return std::make_shared<const std::string>(png_data(image)); }

std::optional<core::Patch> mask_patch(const Image& mask, int dpi, const std::vector<std::int64_t>& rgb, double opacity,
                                      std::pair<double, double> offset) {
    const auto box = mask.getbbox();
    if (!box) return std::nullopt;
    const Image crop = mask.crop(*box);
    core::Patch patch;
    patch.png = png_bytes(crop);
    const double x0 = box->x0 + offset.first;
    const double y0 = box->y0 + offset.second;
    const double mm = 25.4 / dpi;
    patch.attrs["id"] = core::new_id();
    patch.attrs["box"] = core::Json::array({core::py_round(x0 * mm, 3), core::py_round(y0 * mm, 3),
                                            core::py_round(crop.width() * mm, 3), core::py_round(crop.height() * mm, 3)});
    patch.attrs["mode"] = "mask";
    patch.attrs["rgb"] = core::ints_json(rgb);
    patch.attrs["opacity"] = opacity;
    return patch;
}

std::optional<core::Patch> polygon_patch(const core::Json& points_mm, const std::vector<std::int64_t>& rgb, double opacity,
                                         int dpi, std::string_view what) {
    const std::vector<core::Json> points = core::iterate(points_mm);
    if (points.size() < 3) return std::nullopt;
    limits::check_count(static_cast<double>(points.size()), static_cast<double>(limits::kPoints), what);
    std::vector<double> xs;
    std::vector<double> ys;
    for (const core::Json& p : points) xs.push_back(core::finite_float(core::subscript(p, 0), what));
    for (const core::Json& p : points) ys.push_back(core::finite_float(core::subscript(p, 1), what));
    const double x0 = px(*std::min_element(xs.begin(), xs.end()), dpi);
    const double y0 = px(*std::min_element(ys.begin(), ys.end()), dpi);
    const double w = px(*std::max_element(xs.begin(), xs.end()), dpi) - x0 + 2;
    const double h = px(*std::max_element(ys.begin(), ys.end()), dpi) - y0 + 2;
    if (w < 2 || h < 2) return std::nullopt;
    limits::check_sides(w, h, what);
    Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
    std::vector<PointD> corners;
    corners.reserve(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) corners.push_back(PointD{px(xs[i], dpi) - x0, px(ys[i], dpi) - y0});
    Draw(mask).polygon(corners, Ink(255));
    return mask_patch(mask, dpi, rgb, opacity, {x0, y0});
}

}  // namespace fills

}  // namespace genko::render
