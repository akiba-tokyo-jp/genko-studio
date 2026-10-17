#include "render/channels.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "render/ink.hpp"
#include "render/page.hpp"
#include "render/op_limits.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

namespace genko::render::channels {

namespace {

using core::Json;

Size page_size(const core::Page& page, int dpi) {
    return Size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
}

inline unsigned char byte_of(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// The lightness an 8-bit RGB colour shows (Pillow's "L": a grey keeps its value).
inline double lightness(unsigned r, unsigned g, unsigned b) { return (r * 299.0 + g * 587.0 + b * 114.0) / 1000.0 / 255.0; }

// The key a pixel op reads the id of the layer it works on from ("" for flood_fill, which takes a role only).
std::string_view layer_key(std::string_view op) {
    if (op == "filter_raster" || op == "put_raster") return "id";
    if (op == "flood_fill") return {};
    return "layer_id";
}

core::LayerRole role_of(const Json& op) {
    const Json* layer = core::get(op, "layer");
    return core::role_from(Json(layer != nullptr && core::py_truthy(*layer) ? core::py_str(*layer) : "ink"));
}

// The layer the op works on, found as it finds it (null when the page has none of that role yet).
const core::Layer* op_layer(const core::Page& page, const Json& op, std::string_view name) {
    const std::string_view key = layer_key(name);
    if (!key.empty() && core::truthy_at(op, std::string(key).c_str())) {
        const std::string id = core::py_str(op[std::string(key)]);
        for (const core::Layer& layer : page.layers) {
            if (layer.id == id) return &layer;
        }
        return nullptr;
    }
    if (key == "id" && !core::truthy_at(op, "layer")) return nullptr;
    const core::LayerRole role = role_of(op);
    for (const core::Layer& layer : page.layers) {
        if (layer.role == role) return &layer;
    }
    return nullptr;
}

std::optional<std::size_t> page_by_id(const core::Document& doc, const std::string& id) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i) && doc.page(i).id == id) return i;
    }
    return std::nullopt;
}

// A channel's raster as the drawing holds it: its density at its own depth (8 bits a byte, 16 bits two, 32 a float —
// not InkPixels' two bytes a sample, nor a float each: a B4 channel at 600 dpi, 8 bits, is 52 MB). Empty (no pixels)
// for a raster that cannot be read: the book is read-only then, its problem reported where it was read.
struct Held {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int depth = 8;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint16_t> words;
    std::vector<float> floats;

    double value(std::size_t i) const { return depth == 8 ? bytes[i] / 255.0 : depth == 16 ? words[i] / 65535.0 : double(floats[i]); }
    std::size_t held_bytes() const { return bytes.size() + words.size() * 2 + floats.size() * 4; }
};

Held hold(const std::string& raster) {
    const core::InkRasterHeader header = core::ink_raster_header(raster);
    if (header.space != core::InkSpace::Mask) throw core::Error("format", "a channel's raster is a mask");
    Held out;
    out.width = header.width;
    out.height = header.height;
    out.depth = header.depth;
    const std::size_t n = std::size_t(header.width) * header.height;
    if (out.depth == 8) out.bytes.resize(n);
    else if (out.depth == 16) out.words.resize(n);
    else out.floats.resize(n);
    core::read_ink_raster_rows(raster, [&](std::uint32_t y, const float* values) {
        const std::size_t at = std::size_t(y) * out.width;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            if (out.depth == 8) out.bytes[at + x] = static_cast<std::uint8_t>(std::lround(values[x] * 255.0));
            else if (out.depth == 16) out.words[at + x] = static_cast<std::uint16_t>(std::lround(values[x] * 65535.0));
            else out.floats[at + x] = values[x];
        }
    });
    return out;
}

std::shared_ptr<const Held> decoded(const core::Bytes& raster);

// One axis resampled: for each of `to` cells the cells of `from` it reads and their weights (summing to 1) — the cells
// it covers, by their share, when it is the larger (a mean), else bilinear by the centres, the edges carried on. The
// same size reads each cell alone.
using Taps = std::vector<std::vector<std::pair<std::uint32_t, double>>>;
Taps taps(std::uint32_t from, std::uint32_t to) {
    Taps out(to);
    const double scale = double(from) / double(to);
    for (std::uint32_t i = 0; i < to; ++i) {
        if (scale > 1.0) {
            const double a = i * scale, b = (i + 1) * scale;
            const auto last = static_cast<std::uint32_t>(std::min<double>(from, std::ceil(b)));
            for (auto j = static_cast<std::uint32_t>(std::floor(a)); j < last; ++j) {
                const double w = std::min(b, j + 1.0) - std::max(a, double(j));
                if (w > 0) out[i].emplace_back(j, w / scale);
            }
        } else {
            const double x = (i + 0.5) * scale - 0.5, f = std::floor(x), t = x - f;
            const auto at = [&](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, double(from - 1))); };
            const std::uint32_t j0 = at(f), j1 = at(f + 1);
            if (t > 0 && j1 != j0) {
                out[i].emplace_back(j0, 1.0 - t);
                out[i].emplace_back(j1, t);
            } else {
                out[i].emplace_back(t > 0 ? j1 : j0, 1.0);
            }
        }
    }
    return out;
}

// The density a pixel of a lent channel gives back after the op: its lightness, over black where the op made it clear
// (a grey page's rules: Genko's blacks, the greys 0, 10, 15 and 20, are none; white is full; another grey v is v/255).
double density_of(unsigned r, unsigned g, unsigned b, unsigned a) {
    if (r == g && g == b && (r == 0 || r == 10 || r == 15 || r == 20)) return 0.0;
    return lightness(r, g, b) * (a / 255.0);
}

}  // namespace

// --- planes ------------------------------------------------------------------------------------------------------------

Plane blank(const core::Page& page, int dpi) {
    const Size size = page_size(page, dpi);
    Plane out;
    out.width = static_cast<std::uint32_t>(size.width);
    out.height = static_cast<std::uint32_t>(size.height);
    if (std::uint64_t(out.width) * out.height > core::kInkRasterMaxPixels) throw core::OpError("the channel is too large at this resolution");
    out.values.assign(std::size_t(out.width) * out.height, 0.0f);
    return out;
}

Plane plane_of(const core::Page& page, const core::Channel& channel) {
    if (!channel.raster) return blank(page, core::channel_dpi(page, channel.kind));
    const std::shared_ptr<const Held> held = decoded(channel.raster);
    if (held->width == 0) throw core::Error("format", "the channel " + channel.id + "'s raster cannot be read");
    Plane out;
    out.width = held->width;
    out.height = held->height;
    out.values.resize(std::size_t(out.width) * out.height);
    for (std::size_t i = 0; i < out.values.size(); ++i) out.values[i] = static_cast<float>(held->value(i));
    return out;
}

namespace {

// Bilinear by the pixels' centres, the edges carried on: value(i) of a width × height raster where the page's point
// (fx, fy) is.
template <class Value>
double bilinear(std::uint32_t width, std::uint32_t height, double fx, double fy, const Value& value) {
    if (width == 0 || height == 0) return 0.0;
    const double x = fx * width - 0.5, y = fy * height - 0.5;
    const double xf = std::floor(x), yf = std::floor(y);
    const double tx = x - xf, ty = y - yf;
    const auto clampx = [&](double v) { return static_cast<std::size_t>(std::clamp(v, 0.0, double(width - 1))); };
    const auto clampy = [&](double v) { return static_cast<std::size_t>(std::clamp(v, 0.0, double(height - 1))); };
    const std::size_t x0 = clampx(xf), x1 = clampx(xf + 1), y0 = clampy(yf), y1 = clampy(yf + 1);
    const auto at = [&](std::size_t px, std::size_t py) { return value(py * width + px); };
    const double top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
    const double bottom = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
    return top * (1 - ty) + bottom * ty;
}

// The rasters decoded for the drawing (the screen's tiles each read the same channel), by their bytes: the last few, in
// at most kKeptBytes (the newest always), each at its own depth (Held).
constexpr std::size_t kKept = 4;
constexpr std::size_t kKeptBytes = 192u << 20;

std::shared_ptr<const Held> decoded(const core::Bytes& raster) {
    static std::mutex lock;
    static std::vector<std::pair<std::weak_ptr<const std::string>, std::shared_ptr<const Held>>> kept;
    {
        const std::lock_guard<std::mutex> held(lock);
        for (const auto& [bytes, pixels] : kept) {
            if (bytes.lock() == raster) return pixels;
        }
    }
    std::shared_ptr<const Held> pixels;
    try {
        pixels = std::make_shared<const Held>(hold(*raster));
    } catch (const core::Error&) {
        pixels = std::make_shared<const Held>();  // (a raster that cannot be read: none, remembered as such)
    }
    const std::lock_guard<std::mutex> held(lock);
    std::erase_if(kept, [](const auto& entry) { return entry.first.expired(); });
    kept.emplace_back(raster, pixels);
    const auto total = [&] {
        std::size_t n = 0;
        for (const auto& entry : kept) n += entry.second->held_bytes();
        return n;
    };
    while (kept.size() > 1 && (kept.size() > kKept || total() > kKeptBytes)) kept.erase(kept.begin());
    return pixels;
}

}  // namespace

float sample(const Plane& plane, double fx, double fy) {
    return static_cast<float>(bilinear(plane.width, plane.height, fx, fy, [&](std::size_t i) { return double(plane.values[i]); }));
}

Density::Density(const core::Channel& channel) {
    if (channel.raster) pixels_ = decoded(channel.raster);
    if (pixels_ && std::static_pointer_cast<const Held>(pixels_)->width == 0) pixels_.reset();  // (one that cannot be read: none)
}

float Density::at(double fx, double fy) const {
    if (!pixels_) return 0.0f;
    const Held& p = *std::static_pointer_cast<const Held>(pixels_);
    return static_cast<float>(bilinear(p.width, p.height, fx, fy, [&](std::size_t i) { return p.value(i); }));
}

Image thumbnail(const core::Channel& channel, Size size) {
    std::vector<double> sums(std::size_t(size.width) * size.height, 0.0), counts(sums.size(), 0.0);
    if (channel.raster) {
        try {
            const core::InkRasterHeader header = core::ink_raster_header(*channel.raster);
            if (header.space != core::InkSpace::Mask) throw core::Error("format", "a channel's raster is a mask");
            if (header.width < std::uint32_t(size.width) || header.height < std::uint32_t(size.height)) return mask_of(channel, size);
            std::vector<std::uint32_t> column(header.width);
            for (std::uint32_t x = 0; x < header.width; ++x) column[x] = static_cast<std::uint32_t>(std::uint64_t(x) * size.width / header.width);
            core::read_ink_raster_rows(*channel.raster, [&](std::uint32_t y, const float* values) {
                const std::size_t row = std::size_t(std::uint64_t(y) * size.height / header.height) * size.width;
                for (std::uint32_t x = 0; x < header.width; ++x) {
                    sums[row + column[x]] += values[x];
                    counts[row + column[x]] += 1.0;
                }
            });
        } catch (const core::Error&) {  // (a raster that cannot be read: none)
            std::fill(sums.begin(), sums.end(), 0.0);
        }
    }
    std::string bytes(sums.size(), '\0');
    for (std::size_t i = 0; i < sums.size(); ++i) bytes[i] = static_cast<char>(byte_of(counts[i] > 0 ? sums[i] / counts[i] : 0.0));
    return Image::frombytes("L", size, bytes);
}

Image mask_of(const core::Channel& channel, Size size) {
    const Density density(channel);
    std::string bytes(std::size_t(size.width) * size.height, '\0');
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            bytes[std::size_t(y) * size.width + x] = static_cast<char>(byte_of(density.at((x + 0.5) / size.width, (y + 0.5) / size.height)));
        }
    }
    return Image::frombytes("L", size, bytes);
}

Plane resized(const Plane& plane, std::uint32_t width, std::uint32_t height) {
    if (plane.width == width && plane.height == height) return plane;
    Plane out;
    out.width = width;
    out.height = height;
    out.values.resize(std::size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            out.values[std::size_t(y) * width + x] = sample(plane, (x + 0.5) / width, (y + 0.5) / height);
        }
    }
    return out;
}

core::Bytes encode(const Plane& plane, int depth) {
    if (std::all_of(plane.values.begin(), plane.values.end(), [](float v) { return !(v > 0.0f); })) return nullptr;
    core::InkPixels pixels = core::blank_ink_pixels(core::InkSpace::Mask, depth, plane.width, plane.height);
    for (std::size_t i = 0; i < plane.values.size(); ++i) pixels.set(i, 0, plane.values[i]);
    return std::make_shared<const std::string>(core::encode_ink_raster(pixels));
}

Image mask_image(const Plane& plane, Size size) {
    std::string bytes(std::size_t(size.width) * size.height, '\0');
    if (plane.width == static_cast<std::uint32_t>(size.width) && plane.height == static_cast<std::uint32_t>(size.height)) {
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(byte_of(plane.values[i]));
    } else {
        for (int y = 0; y < size.height; ++y) {
            for (int x = 0; x < size.width; ++x) {
                bytes[std::size_t(y) * size.width + x] = static_cast<char>(byte_of(sample(plane, (x + 0.5) / size.width, (y + 0.5) / size.height)));
            }
        }
    }
    return Image::frombytes("L", size, bytes);
}

Plane from_mask(const Image& mask) {
    const Image l = mask.mode() == "L" ? mask : mask.convert("L");
    const std::string bytes = l.tobytes();
    Plane out;
    out.width = static_cast<std::uint32_t>(l.width());
    out.height = static_cast<std::uint32_t>(l.height());
    out.values.resize(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out.values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

core::Bytes relayout(const core::Bytes& raster, const core::PageSpec& old_paper, const core::PageSpec& paper, const core::Rect& old_frame,
                     const core::Rect& new_frame) {
    if (!raster) return raster;
    const core::InkPixels pixels = core::decode_ink_raster(*raster);
    const double old_w = old_paper.width_mm.value(), old_h = old_paper.height_mm.value();
    const double new_w = paper.width_mm.value(), new_h = paper.height_mm.value();
    if (!(old_w > 0) || !(old_h > 0) || !(new_w > 0) || !(new_h > 0)) throw core::OpError("the paper must be a finite size");
    // (the resolution it had, across and down)
    const double width = std::max(1.0, std::round(new_w * pixels.width / old_w));
    const double height = std::max(1.0, std::round(new_h * pixels.height / old_h));
    if (width > core::kInkRasterMaxSide || height > core::kInkRasterMaxSide || width * height > double(core::kInkRasterMaxPixels)) {
        throw core::OpError("the channel on the new paper is too large");
    }
    const double sx = old_frame.width.truthy() ? (new_frame.width / old_frame.width).value() : 1.0;
    const double sy = old_frame.height.truthy() ? (new_frame.height / old_frame.height).value() : 1.0;
    if (!std::isfinite(sx) || !std::isfinite(sy) || sx == 0 || sy == 0) throw core::OpError("the basic frame must be a finite size");
    Plane old;
    old.width = pixels.width;
    old.height = pixels.height;
    old.values.resize(pixels.pixel_count());
    for (std::size_t i = 0; i < old.values.size(); ++i) old.values[i] = static_cast<float>(pixels.value(i, 0));
    Plane out;
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.values.resize(std::size_t(out.width) * out.height);
    for (std::uint32_t v = 0; v < out.height; ++v) {
        const double mm_y = (v + 0.5) * new_h / out.height;
        const double fy = (old_frame.y.value() + (mm_y - new_frame.y.value()) / sy) / old_h;
        for (std::uint32_t u = 0; u < out.width; ++u) {
            const double mm_x = (u + 0.5) * new_w / out.width;
            const double fx = (old_frame.x.value() + (mm_x - new_frame.x.value()) / sx) / old_w;
            if (!std::isfinite(fx) || !std::isfinite(fy)) throw core::OpError("the basic frame must be a finite size");
            out.values[std::size_t(v) * out.width + u] = sample(old, fx, fy);
        }
    }
    return encode(out, pixels.depth);
}

void follow_depth(core::Document& doc) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p) || doc.page(p).channels.empty()) continue;
        const int depth = core::page_depth(doc.page(p));
        const auto off = [&](const core::Channel& c) { return c.raster && core::ink_raster_header(*c.raster).depth != depth; };
        if (std::none_of(doc.page(p).channels.begin(), doc.page(p).channels.end(), off)) continue;
        core::Page& page = doc.edit_page(p);
        for (core::Channel& channel : page.channels) {
            if (!off(channel)) continue;
            channel.raster = encode(plane_of(page, channel), depth);
            channel.memo = {};
        }
    }
}

Plane page_plane(const core::Page& page, const core::Document* episode, std::optional<std::size_t> plate) {
    constexpr int kDpi = 200;
    RenderOptions options;
    options.mode = "print";
    options.finish = false;
    options.onion = false;
    options.spots = std::vector<std::string>{};
    // (an RGB page of more than 8 bits or of a profile: its values in its space, not the sRGB the screen shows)
    const bool deep = !page.color && page.rgb && (page.rgb->depth != 8 || !page.rgb->profile.empty());
    options.inks = page.color.has_value();
    options.precise = deep;
    core::Page shown = page;  // (without its nombre: a page made of the plane draws its own)
    shown.numero = false;
    const RenderResult made = render_page(shown, kDpi, options, episode);
    Plane out;
    out.width = static_cast<std::uint32_t>(made.image.width());
    out.height = static_cast<std::uint32_t>(made.image.height());
    out.values.resize(std::size_t(out.width) * out.height);
    if (page.color && made.inks && (plate || page.color->space == core::InkSpace::Gray)) {
        const core::InkPixels& inks = *made.inks;
        const unsigned c = plate ? static_cast<unsigned>(*plate) : 0;
        for (std::size_t i = 0; i < out.values.size(); ++i) {
            const double v = inks.value(i, c);
            out.values[i] = static_cast<float>(page.color->space == core::InkSpace::Cmyk || page.color->space == core::InkSpace::Multi ? 1.0 - v : v);
        }
        return out;
    }
    if (deep && made.precise && plate) {
        const core::ColorRasterView values(*made.precise);
        for (std::size_t i = 0; i < out.values.size(); ++i) out.values[i] = static_cast<float>(std::clamp(values.pixel(i)[*plate], 0.0, 1.0));
        return out;
    }
    const Image rgb = made.image.mode() == "RGB" ? made.image : made.image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    for (std::size_t i = 0; i < out.values.size(); ++i) {
        const auto r = static_cast<unsigned char>(bytes[3 * i]), g = static_cast<unsigned char>(bytes[3 * i + 1]),
                   b = static_cast<unsigned char>(bytes[3 * i + 2]);
        out.values[i] = static_cast<float>(plate ? (plate == 0u ? r : plate == 1u ? g : b) / 255.0 : 0.299 * r / 255.0 + 0.587 * g / 255.0 + 0.114 * b / 255.0);
    }
    return out;
}

// --- the view ----------------------------------------------------------------------------------------------------------

void print_spots(Image& image, const core::Page& page, const Box& area, Size size, const std::optional<std::vector<std::string>>& shown) {
    std::vector<const core::Channel*> spots;
    for (const core::Channel& channel : page.channels) {
        if (channel.kind != core::ChannelKind::Spot || !channel.raster) continue;
        if (shown && std::find(shown->begin(), shown->end(), channel.id) == shown->end()) continue;
        spots.push_back(&channel);
    }
    if (spots.empty()) return;
    const std::string mode(image.mode());
    const unsigned bands = static_cast<unsigned>(image.bands());
    if (mode != "RGB" && mode != "RGBA") return;
    std::string bytes = image.tobytes();
    for (const core::Channel* spot : spots) {
        const Density plane(*spot);
        const double s = std::clamp(spot->solidity, 0.0, 1.0);
        const double c[3] = {spot->color[0] / 255.0, spot->color[1] / 255.0, spot->color[2] / 255.0};
        for (int y = 0; y < area.height(); ++y) {
            const double fy = (area.y0 + y + 0.5) / size.height;
            for (int x = 0; x < area.width(); ++x) {
                const double v = std::clamp(double(plane.at((area.x0 + x + 0.5) / size.width, fy)), 0.0, 1.0);
                if (!(v > 0)) continue;
                const std::size_t at = (std::size_t(y) * area.width() + x) * bands;
                for (unsigned j = 0; j < 3; ++j) {
                    const double p = static_cast<unsigned char>(bytes[at + j]) / 255.0;
                    bytes[at + j] = static_cast<char>(byte_of(p + (s * c[j] + (1.0 - s) * p * c[j] - p) * v));
                }
            }
        }
    }
    image = Image::frombytes(mode, image.size(), bytes);
}

void finish(RenderResult& result, const core::Page& page, int dpi, const RenderOptions& options) {
    const bool spots = std::any_of(page.channels.begin(), page.channels.end(), [](const core::Channel& c) { return c.kind == core::ChannelKind::Spot; });
    const std::vector<std::string> plates = core::plate_names(page);
    const bool hiding = options.planes && std::find(options.planes->begin(), options.planes->end(), false) != options.planes->end();
    if (!spots && !hiding && options.overlays.empty()) return;
    const Size size = page_size(page, dpi);
    const Box area = options.region ? Box{options.region->x, options.region->y, options.region->x + options.region->w, options.region->y + options.region->h}
                                    : Box{0, 0, size.width, size.height};
    if (area.width() != result.image.width() || area.height() != result.image.height()) return;
    std::vector<std::string> overlays = options.overlays;
    if (hiding) {
        std::vector<std::size_t> visible;
        for (std::size_t k = 0; k < plates.size(); ++k) {
            if (k < options.planes->size() ? (*options.planes)[k] : true) visible.push_back(k);
        }
        Image& image = result.image;
        const std::size_t n = std::size_t(area.width()) * area.height();
        std::string out(n * 3, '\xff');
        if (visible.empty()) {
            // (nothing of the page shown: the first alpha channel laid over, in grey — PhotoCraft's view)
            const auto first = std::find_if(overlays.begin(), overlays.end(), [&](const std::string& id) {
                const core::Channel* c = core::find_channel(page, id);
                return c != nullptr && c->kind == core::ChannelKind::Alpha;
            });
            if (first != overlays.end()) {
                const Density plane(*core::find_channel(page, *first));
                for (int y = 0; y < area.height(); ++y) {
                    for (int x = 0; x < area.width(); ++x) {
                        const unsigned char g = byte_of(plane.at((area.x0 + x + 0.5) / size.width, (area.y0 + y + 0.5) / size.height));
                        std::fill_n(out.begin() + std::ptrdiff_t((std::size_t(y) * area.width() + x) * 3), 3, static_cast<char>(g));
                    }
                }
                overlays.erase(first);
            }
        } else if (page.color && result.inks) {
            const core::InkPixels& inks = *result.inks;
            const unsigned colours = inks.colours();
            if (visible.size() == 1) {
                for (std::size_t i = 0; i < n; ++i) {
                    const double v = inks.value(i, static_cast<unsigned>(visible[0]));
                    std::fill_n(out.begin() + std::ptrdiff_t(i * 3), 3, static_cast<char>(byte_of(page.color->space == core::InkSpace::Cmyk || page.color->space == core::InkSpace::Multi ? 1.0 - v : v)));
                }
            } else {
                std::vector<double> values(n * colours);
                for (std::size_t i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < colours; ++c) {
                        const bool shown_plate = std::find(visible.begin(), visible.end(), c) != visible.end();
                        double v = inks.value(i, c);
                        if (page.color->space == core::InkSpace::Lab) {  // (an ink's value as render/ink takes it: L 0..100, a and b ±128)
                            v = c == 0 ? (shown_plate ? v * 100.0 : 0.0) : (shown_plate ? v * 255.0 - 128.0 : 0.0);
                        } else if (!shown_plate) {
                            v = 0.0;
                        }
                        values[i * colours + c] = v;
                    }
                }
                std::vector<float> srgb(n * 3);
                ink::to_srgb(*page.color, values.data(), srgb.data(), n);
                for (std::size_t i = 0; i < n * 3; ++i) out[i] = static_cast<char>(byte_of(srgb[i]));
            }
        } else {
            const Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
            const std::string bytes = rgb.tobytes();
            for (std::size_t i = 0; i < n; ++i) {
                for (unsigned c = 0; c < 3; ++c) {
                    const bool shown_plate = std::find(visible.begin(), visible.end(), c) != visible.end();
                    out[i * 3 + c] = visible.size() == 1 ? bytes[i * 3 + visible[0]] : shown_plate ? bytes[i * 3 + c] : '\0';
                }
            }
        }
        image = Image::frombytes("RGB", Size{area.width(), area.height()}, out);
    }
    if (spots) print_spots(result.image, page, area, size, options.spots);
    std::vector<const core::Channel*> laid;
    for (const std::string& id : overlays) {
        const core::Channel* channel = core::find_channel(page, id);
        if (channel != nullptr && channel->kind == core::ChannelKind::Alpha) laid.push_back(channel);
    }
    if (laid.empty()) return;
    Image& image = result.image;
    const std::string mode(image.mode());
    const unsigned bands = static_cast<unsigned>(image.bands());
    std::string bytes = image.tobytes();
    for (const core::Channel* channel : laid) {
        const Density plane(*channel);
        const bool selected = channel->indicates == "selected";
        const double opacity = std::clamp(channel->opacity, 0.0, 1.0);
        for (int y = 0; y < area.height(); ++y) {
            const double fy = (area.y0 + y + 0.5) / size.height;
            for (int x = 0; x < area.width(); ++x) {
                const double v = std::clamp(double(plane.at((area.x0 + x + 0.5) / size.width, fy)), 0.0, 1.0);
                const double a = (selected ? v : 1.0 - v) * opacity;
                if (!(a > 0)) continue;
                const std::size_t at = (std::size_t(y) * area.width() + x) * bands;
                for (unsigned j = 0; j < 3; ++j) {
                    const double p = static_cast<unsigned char>(bytes[at + j]) / 255.0;
                    bytes[at + j] = static_cast<char>(byte_of(channel->color[j] / 255.0 * a + p * (1.0 - a)));
                }
            }
        }
    }
    image = Image::frombytes(mode, image.size(), bytes);
}

// --- the pixel ops' "channel" ----------------------------------------------------------------------------------------

std::optional<Target> target_of(const core::Document& doc, const Json& op) {
    const std::optional<core::ChannelTarget> named = core::channel_target(doc, op);
    if (!named || named->kind == core::ChannelTarget::Kind::Composite) return std::nullopt;
    if (named->kind == core::ChannelTarget::Kind::Channel) return Target{named->index, std::nullopt};
    return Target{std::nullopt, named->index};
}

ChannelLending::ChannelLending(core::Document& doc, std::size_t at, std::size_t index, Json& op, std::string_view name) {
    const core::Page& seen = doc.page(at);
    const core::Channel& channel = seen.channels.at(index);
    page_id_ = seen.id;
    channel_id_ = channel.id;
    depth_ = core::page_depth(seen);
    lent_size_ = page_size(seen, raster::kWorkingDpi);
    limits::check_picture(lent_size_.width, lent_size_.height, "the page");
    std::string rgba(std::size_t(lent_size_.width) * lent_size_.height * 4, '\xff');
    if (channel.raster) {
        const std::shared_ptr<const Held> held = decoded(channel.raster);
        if (held->width == 0) throw core::OpError("the channel " + core::py_repr_str(channel.name) + " cannot be read");
        own_size_ = Size{int(held->width), int(held->height)};
        // (each pixel lent: the mean of the channel's pixels it covers, or bilinear where the channel is coarser)
        const Taps across = taps(held->width, static_cast<std::uint32_t>(lent_size_.width));
        const Taps down = taps(held->height, static_cast<std::uint32_t>(lent_size_.height));
        for (int y = 0; y < lent_size_.height; ++y) {
            for (int x = 0; x < lent_size_.width; ++x) {
                double v = 0.0;
                for (const auto& [row, wy] : down[std::size_t(y)]) {
                    for (const auto& [col, wx] : across[std::size_t(x)]) v += wy * wx * held->value(std::size_t(row) * held->width + col);
                }
                std::fill_n(rgba.begin() + std::ptrdiff_t((std::size_t(y) * lent_size_.width + x) * 4), 3, static_cast<char>(byte_of(v)));
            }
        }
    } else {
        own_size_ = page_size(seen, core::channel_dpi(seen, channel.kind));
        for (std::size_t i = 0; i < rgba.size(); i += 4) std::fill_n(rgba.begin() + std::ptrdiff_t(i), 3, '\0');
    }
    core::Layer layer;
    layer_id_ = "lent_" + channel.id;
    layer.id = layer_id_;
    layer.kind = core::LayerKind::Raster;
    layer.role = layer_key(name).empty() ? role_of(op) : core::LayerRole::User;
    layer.title = channel.name;
    layer.visible = false;  // (never drawn with the page: what the op looks at around it is the page as it is)
    layer.exportable = false;
    layer.panel_clip = false;
    layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", lent_size_, rgba), 1));
    lent_ = std::move(rgba);
    core::Page& page = doc.edit_page(at);
    page.layers.insert(page.layers.begin(), std::move(layer));  // (the first of its role, for an op that takes a role)
    if (const std::string_view key = layer_key(name); !key.empty()) op[std::string(key)] = layer_id_;
}

bool ChannelLending::take_back(core::Document& doc) const {
    const auto at = page_by_id(doc, page_id_);
    if (!at) return false;
    core::Page& page = doc.edit_page(*at);
    const auto lent = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == layer_id_; });
    if (lent == page.layers.end()) throw core::OpError("the channel lent to the op was taken away");
    core::Layer layer = *lent;
    page.layers.erase(lent);
    const std::size_t index = core::channel_index(page, channel_id_);
    if (index >= page.channels.size()) return false;
    if (layer.stroke_count() > 0 || !layer.patches.empty()) raster::bake_vectors(page, layer);
    // what the op changed, at 200 dpi: each pixel's density after it, and whether it changed
    const std::size_t n = std::size_t(lent_size_.width) * lent_size_.height;
    std::vector<std::uint8_t> changed(n, 0);
    std::vector<float> made(n, 0.0f);
    int x0 = lent_size_.width, y0 = lent_size_.height, x1 = -1, y1 = -1;  // (the changed pixels' box)
    const auto mark = [&](std::size_t i, double v) {
        changed[i] = 1;
        made[i] = static_cast<float>(v);
        const int x = static_cast<int>(i % std::size_t(lent_size_.width)), y = static_cast<int>(i / std::size_t(lent_size_.width));
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    if (layer.raster_png && !layer.raster_png->empty()) {
        Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
        const bool same = picture.size() == lent_size_;
        if (!same) picture = picture.resize(lent_size_, Resample::Bilinear);  // (a picture of another size: all of it)
        const std::string now = picture.tobytes();
        for (std::size_t i = 0; i < n; ++i) {
            const char* px = now.data() + i * 4;
            if (same && std::equal(px, px + 4, lent_.data() + i * 4)) continue;
            mark(i, density_of(static_cast<unsigned char>(px[0]), static_cast<unsigned char>(px[1]), static_cast<unsigned char>(px[2]),
                               static_cast<unsigned char>(px[3])));
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) mark(i, 0.0);  // (the op left the layer with no pixels: all clear)
    }
    if (x1 < 0) return false;  // (nothing changed: the channel as it was, its bytes too)
    core::Channel& channel = page.channels[index];
    // (its pixels as they were: from the raster held while it was lent — the same samples —, or none)
    const std::shared_ptr<const Held> held = channel.raster ? decoded(channel.raster) : nullptr;
    if (held && held->width == 0) throw core::OpError("the channel " + core::py_repr_str(channel.name) + " cannot be read");
    core::InkPixels own = core::blank_ink_pixels(core::InkSpace::Mask, held ? held->depth : depth_,
                                                 held ? held->width : static_cast<std::uint32_t>(own_size_.width),
                                                 held ? held->height : static_cast<std::uint32_t>(own_size_.height));
    if (held && held->depth == 32) own.floats = held->floats;
    else if (held && held->depth == 16) own.samples.assign(held->words.begin(), held->words.end());
    else if (held) own.samples.assign(held->bytes.begin(), held->bytes.end());
    // each pixel of the channel by the changed pixels it reads (their share): v(1 − w) + Σ share × density
    const Taps across = taps(static_cast<std::uint32_t>(lent_size_.width), own.width);
    const Taps down = taps(static_cast<std::uint32_t>(lent_size_.height), own.height);
    const auto reaching = [](const Taps& list, int from, int to) {  // (the cells that read a changed one's row or column)
        std::vector<char> out(list.size(), 0);
        for (std::size_t k = 0; k < list.size(); ++k) {
            out[k] = std::any_of(list[k].begin(), list[k].end(), [&](const auto& tap) { return int(tap.first) >= from && int(tap.first) <= to && tap.second > 0; });
        }
        return out;
    };
    const std::vector<char> rows = reaching(down, y0, y1), columns = reaching(across, x0, x1);
    for (std::uint32_t y = 0; y < own.height; ++y) {
        if (!rows[y]) continue;
        for (std::uint32_t x = 0; x < own.width; ++x) {
            if (!columns[x]) continue;
            double share = 0.0, density = 0.0;
            for (const auto& [row, wy] : down[y]) {
                for (const auto& [col, wx] : across[x]) {
                    const std::size_t i = std::size_t(row) * lent_size_.width + col;
                    if (!changed[i]) continue;
                    share += wy * wx;
                    density += wy * wx * made[i];
                }
            }
            if (!(share > 0)) continue;
            const std::size_t i = std::size_t(y) * own.width + x;
            own.set(i, 0, share >= 1.0 ? density : own.value(i, 0) * (1.0 - share) + density);
        }
    }
    const bool none = own.depth == 32 ? std::all_of(own.floats.begin(), own.floats.end(), [](float v) { return !(v > 0.0f); })
                                      : std::all_of(own.samples.begin(), own.samples.end(), [](std::uint16_t v) { return v == 0; });
    channel.raster = none ? nullptr : std::make_shared<const std::string>(core::encode_ink_raster(own));
    channel.memo = {};
    return true;
}

PlateKeep::PlateKeep(const core::Document& doc, std::size_t at, std::size_t plate, const Json& op, std::string_view name) {
    const core::Page& page = doc.page(at);
    page_id_ = page.id;
    plate_ = plate;
    const core::Layer* layer = op_layer(page, op, name);
    if (layer == nullptr) return;
    if (layer->kind != core::LayerKind::Raster) {
        throw core::OpError("only a paint layer's colour plates can be worked one at a time");
    }
    layer_id_ = layer->id;
    before_ = *layer;
}

void PlateKeep::restore(core::Document& doc) const {
    if (!before_) return;
    const auto at = page_by_id(doc, page_id_);
    if (!at) return;
    core::Page& page = doc.edit_page(*at);
    const auto found = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == layer_id_; });
    if (found == page.layers.end()) return;
    core::Layer& layer = *found;
    const core::Layer& before = *before_;
    if (layer.ink_raster && page.color) {
        core::InkPixels now = core::decode_ink_raster(*layer.ink_raster);
        const core::InkPixels was = before.ink_raster ? core::decode_ink_raster(*before.ink_raster)
                                                      : core::blank_ink_pixels(now.space, now.depth, now.width, now.height, now.planes);
        if (was.width != now.width || was.height != now.height || was.space != now.space || was.depth != now.depth) return;
        const unsigned all = now.channels();
        for (std::size_t i = 0; i < now.pixel_count(); ++i) {
            bool changed = false;
            for (unsigned c = 0; c < all && !changed; ++c) changed = now.value(i, c) != was.value(i, c);
            if (!changed) continue;
            const double mine = now.value(i, static_cast<unsigned>(plate_));
            now.copy_pixel(i, was, i);
            now.set(i, static_cast<unsigned>(plate_), mine);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(now));
        layer.ink_memo = {};
        return;
    }
    if (layer.color_raster) {
        if (!before.color_raster) return;
        const core::ColorRasterView was(*before.color_raster);
        core::ColorRasterEdit now{std::string(*layer.color_raster)};
        if (was.width() != now.width() || was.height() != now.height()) return;
        for (std::size_t i = 0; i < std::size_t(now.width()) * now.height(); ++i) {
            const std::array<double, 4> old = was.pixel(i), made = now.pixel(i);
            if (old == made) continue;
            std::array<double, 4> kept = old;
            kept[plate_] = made[plate_];
            now.set(i, kept);
        }
        layer.color_raster = std::make_shared<const std::string>(std::move(now).take());
        return;
    }
    // (8-bit pixels: the layer's lines and pictures drawn into them first, before and after)
    if (layer.stroke_count() > 0 || !layer.patches.empty()) raster::bake_vectors(page, layer);
    if (!layer.raster_png || layer.raster_png->empty()) return;
    const Image now = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
    Image was = raster::layer_pixels(page, before).convert("RGBA");
    if (was.size() != now.size()) was = was.resize(now.size(), Resample::Bilinear);
    std::string made = now.tobytes();
    const std::string old = was.tobytes();
    for (std::size_t i = 0; i + 3 < made.size(); i += 4) {
        if (std::equal(made.begin() + std::ptrdiff_t(i), made.begin() + std::ptrdiff_t(i + 4), old.begin() + std::ptrdiff_t(i))) continue;
        const char mine = made[i + plate_];
        std::copy_n(old.begin() + std::ptrdiff_t(i), 4, made.begin() + std::ptrdiff_t(i));
        made[i + plate_] = mine;
    }
    layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", now.size(), made)));
    layer.raster_memo = {};
}

}  // namespace genko::render::channels
