// BRUSH-01 紙質 (render/paper.hpp): the paper picture taken in, the pictures this process knows, and the arithmetic from
// a pixel of the page to the ink left on it (checked against the independent reference tools/migration/paper_reference.py,
// tests/data/paper/samples.json).

#include "render/paper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

#include "core/error.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"

namespace genko::render::paper {

namespace {

constexpr double kDegToRad = 0.017453292519943295;  // pi / 180 as a double
// The decoded grains kept (each at most kMaxSide² bytes): the pictures' PNGs are all kept, they are small.
constexpr std::size_t kGrainsKept = 8;
// Past this, U and V are not whole numbers a double holds exactly: such a pixel is not on any page this build draws.
constexpr double kLargestPlace = 4503599627370496.0;  // 2^52

std::uint64_t splitmix(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ULL;
    }
    return h;
}

// sin and cos of x degrees, 0 <= x <= 45: their Taylor series to t^15 and t^16 (the next terms are below 1e-16).
std::pair<double, double> polynomial(double x) {
    const double t = x * kDegToRad;
    const double t2 = t * t;
    const double s = t * (1.0 + t2 * (-1.0 / 6.0 + t2 * (1.0 / 120.0 + t2 * (-1.0 / 5040.0 + t2 * (1.0 / 362880.0 + t2 * (
        -1.0 / 39916800.0 + t2 * (1.0 / 6227020800.0 + t2 * (-1.0 / 1307674368000.0))))))));
    const double c = 1.0 + t2 * (-1.0 / 2.0 + t2 * (1.0 / 24.0 + t2 * (-1.0 / 720.0 + t2 * (1.0 / 40320.0 + t2 * (
        -1.0 / 3628800.0 + t2 * (1.0 / 479001600.0 + t2 * (-1.0 / 87178291200.0 + t2 * (1.0 / 20922789888000.0))))))));
    return {s, c};
}

// A whole pixel of the picture: the picture repeated, or mirrored at each edge (its period twice its size).
std::int64_t wrap(std::int64_t i, std::int64_t n, bool mirror) {
    if (mirror) {
        const std::int64_t period = 2 * n;
        std::int64_t m = i % period;
        if (m < 0) m += period;
        return m >= n ? period - 1 - m : m;
    }
    std::int64_t m = i % n;
    if (m < 0) m += n;
    return m;
}

// What does not change from pixel to pixel of one line.
struct Frame {
    const Grain* grain = nullptr;
    double sin = 0, cos = 1, texel = 1;
    bool stroke = false, flip_x = false, flip_y = false, invert = false, mirror = false;
    double anchor_x = 0, anchor_y = 0;
    std::pair<std::int64_t, std::int64_t> offset;
    int dpi = 0;
};

Frame frame_of(const Grain& grain, const core::Paper& paper, int dpi, double anchor_x_mm, double anchor_y_mm,
               std::pair<std::int64_t, std::int64_t> offset) {
    if (dpi <= 0) throw core::Error("value", "the resolution must be positive");
    if (grain.width <= 0 || grain.height <= 0 || grain.values.size() != static_cast<std::size_t>(grain.width) * grain.height) {
        throw core::Error("value", "the paper picture holds no pixels");
    }
    Frame f;
    f.grain = &grain;
    std::tie(f.sin, f.cos) = sin_cos_degrees(paper.rotation);
    f.texel = paper.scale * 25.4 / kBaseDpi;
    f.stroke = paper.coords == "stroke";
    f.flip_x = paper.flip_x;
    f.flip_y = paper.flip_y;
    f.invert = paper.invert;
    f.mirror = paper.seam == "mirror";
    f.anchor_x = anchor_x_mm;
    f.anchor_y = anchor_y_mm;
    f.offset = offset;
    f.dpi = dpi;
    return f;
}

// render/paper.hpp step 2 for one pixel (every value kept when `out` is given).
int sample_at(const Frame& f, std::int64_t px, std::int64_t py, Sample* out) {
    const double x_mm = (static_cast<double>(px) + 0.5) * 25.4 / f.dpi;
    const double y_mm = (static_cast<double>(py) + 0.5) * 25.4 / f.dpi;
    double dx = x_mm;
    double dy = y_mm;
    if (f.stroke) {
        dx = x_mm - f.anchor_x;
        dy = y_mm - f.anchor_y;
    }
    double u = (dx * f.cos + dy * f.sin) / f.texel;
    double v = (dy * f.cos - dx * f.sin) / f.texel;
    if (f.flip_x) u = -u;
    if (f.flip_y) v = -v;
    const double fu = std::floor((u - 0.5) * 256.0);
    const double fv = std::floor((v - 0.5) * 256.0);
    if (!(std::fabs(fu) < kLargestPlace && std::fabs(fv) < kLargestPlace)) {
        throw core::Error("value", "a pixel lies too far from the paper's start");
    }
    const std::int64_t U = static_cast<std::int64_t>(fu) + f.offset.first * 256;
    const std::int64_t V = static_cast<std::int64_t>(fv) + f.offset.second * 256;
    const std::int64_t ix = U >> 8;  // (whole steps and 1/256 parts of two's complement: floor for the negative too)
    const std::int64_t iy = V >> 8;
    const int wx = static_cast<int>(U & 255);
    const int wy = static_cast<int>(V & 255);
    const Grain& g = *f.grain;
    const std::int64_t x0 = wrap(ix, g.width, f.mirror);
    const std::int64_t x1 = wrap(ix + 1, g.width, f.mirror);
    const std::int64_t y0 = wrap(iy, g.height, f.mirror);
    const std::int64_t y1 = wrap(iy + 1, g.height, f.mirror);
    int value = (g.at(x0, y0) * (256 - wx) * (256 - wy) + g.at(x1, y0) * wx * (256 - wy) + g.at(x0, y1) * (256 - wx) * wy +
                 g.at(x1, y1) * wx * wy + 32768) >>
                16;
    if (f.invert) value = 255 - value;
    if (out != nullptr) *out = Sample{x_mm, y_mm, u, v, U, V, value};
    return value;
}

// A 16-bit grey picture (I;16, little-endian: what the PNG reader gives) by the upper 8 bits of each value, its
// transparent value (tRNS) kept transparent. (Pillow's convert("L") cuts the values at 255: a scan of paper would be
// almost white. This build's own choice: Python has no paper.)
Image upper_bytes(const Image& image) {
    const std::string raw = image.tobytes();
    std::string grey(raw.size() / 2, '\0');
    std::string alpha(grey.size(), '\xff');
    const Transparency& t = image.transparency();
    for (std::size_t i = 0; i < grey.size(); ++i) {
        const int v = static_cast<unsigned char>(raw[2 * i]) | (static_cast<unsigned char>(raw[2 * i + 1]) << 8);
        grey[i] = static_cast<char>(v >> 8);
        if (t.kind == Transparency::Kind::Index && v == t.value) alpha[i] = '\0';
    }
    return Image::merge("LA", {Image::frombytes("L", image.size(), grey), Image::frombytes("L", image.size(), alpha)});
}

// The picture laid over white and made grey (Pillow: Image.alpha_composite(white, image.convert("RGBA")).convert("L")).
Image grey_of(const Image& image) {
    const std::string_view mode = image.mode();
    Image rgba;
    if (mode == "I;16") {
        rgba = upper_bytes(image).convert("RGBA");
    } else if (mode == "I;16B" || mode == "I;16L" || mode == "I" || mode == "F") {
        rgba = image.convert("L").convert("RGBA");  // (not given by the readers paper takes)
    } else {
        rgba = image.convert("RGBA");
    }
    Image white = Image::create("RGBA", rgba.size(), Ink::tuple({255, 255, 255, 255}));
    white.alpha_composite(rgba);
    return white.convert("L");
}

// The ink a pixel of paper of this value takes away (0..255): ((255 - value) * D + 127) / 255, D = floor(density * 255 + 0.5).
int ink_taken(int value, const core::Paper& paper) {
    const int density = static_cast<int>(std::floor(paper.density * 255.0 + 0.5));
    return ((255 - value) * density + 127) / 255;
}

[[noreturn]] void unreadable() {
    throw core::Error("paper_unreadable", "the paper's picture cannot be read (it is not a PNG, JPEG, BMP or GIF picture, or it is broken)");
}

[[noreturn]] void too_large() {
    throw core::Error("paper_too_large", "the paper's picture is too large (at most " + std::to_string(kMaxSide) + " pixels a side)");
}

void check_size(const Image& image) {
    if (image.width() <= 0 || image.height() <= 0) throw core::Error("paper_empty", "the paper's picture has no pixels");
    if (image.width() > kMaxSide || image.height() > kMaxSide) too_large();
}

constexpr PngLimits kPaperLimits{static_cast<std::int64_t>(kMaxSide) * kMaxSide, kMaxSide};

struct Registry {
    std::shared_mutex mutex;
    std::unordered_map<std::string, core::Bytes> pictures;
    std::unordered_map<std::string, std::shared_ptr<const Grain>> grains;
    std::deque<std::string> decoded;  // the grains' refs, oldest first
};

Registry& registry() {
    static Registry r;
    return r;
}

// The reader's check of a book's paper pictures (storage/reader.hpp set_paper_check): decoded as they are drawn, so a
// book whose picture cannot be drawn opens read-only instead of failing when its lines are drawn. (Set when this file is
// linked: every program that draws lines draws them through it.)
const bool kReaderCheck = [] {
    storage::set_paper_check([](std::string_view png) -> std::optional<std::string> {
        try {
            (void)grain_of(png);
        } catch (const core::Error& error) {
            return std::string(error.what());
        }
        return std::nullopt;
    });
    return true;
}();

}  // namespace

std::string take_in(std::string_view file_bytes) {
    if (file_bytes.size() > kMaxFileBytes) {
        throw core::Error("paper_file_too_large", "the paper's picture file is too large (at most 64 MiB)");
    }
    Image image;
    try {
        image = open_image(file_bytes, kPaperLimits);
    } catch (const NotYetPorted&) {
        throw core::Error("paper_format", "this build does not read that kind of picture (TIFF, WebP, PSD): give the paper as PNG, JPEG, BMP or GIF");
    } catch (const core::Error& error) {
        if (error.code() == "image_too_large") too_large();
        unreadable();
    }
    check_size(image);
    Image grey;
    try {
        grey = grey_of(image);
    } catch (const core::Error&) {
        unreadable();
    }
    return write_png(grey);
}

Grain grain_of(std::string_view png) {
    Image image;
    try {
        image = read_png(png, kPaperLimits);
    } catch (const core::Error& error) {
        if (error.code() == "image_too_large") too_large();
        unreadable();
    }
    check_size(image);
    const Image grey = image.mode() == "L" && !image.transparency().present() ? image : grey_of(image);
    const std::string values = grey.tobytes();
    Grain out;
    out.width = grey.width();
    out.height = grey.height();
    out.values.assign(values.begin(), values.end());
    return out;
}

bool keep(const std::string& ref, core::Bytes png) {
    if (!png || storage::AssetStore::ref(*png) != ref) return false;  // (never another picture under its name)
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    r.pictures.emplace(ref, std::move(png));
    return true;
}

void keep_all(const std::map<std::string, core::Bytes>& papers) {
    if (papers.empty()) return;
    Registry& r = registry();
    {
        // (every tile of a page asks: those it knows already cost a shared look only)
        std::shared_lock lock(r.mutex);
        if (std::all_of(papers.begin(), papers.end(), [&](const auto& p) { return !p.second || r.pictures.contains(p.first); })) return;
    }
    for (const auto& [ref, png] : papers) {
        bool known = false;
        {
            std::shared_lock lock(r.mutex);
            known = r.pictures.contains(ref);
        }
        if (!known) keep(ref, png);  // (hashed outside the lock: each new picture once)
    }
}

core::Bytes bytes(const std::string& ref) {
    Registry& r = registry();
    std::shared_lock lock(r.mutex);
    const auto it = r.pictures.find(ref);
    return it == r.pictures.end() ? nullptr : it->second;
}

std::shared_ptr<const Grain> grain(const std::string& ref) {
    Registry& r = registry();
    core::Bytes png;
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.grains.find(ref); it != r.grains.end()) return it->second;
        if (const auto it = r.pictures.find(ref); it != r.pictures.end()) png = it->second;
    }
    if (!png) throw core::Error("missing_asset", "the paper picture " + ref + " is not known here (its book's brushes are not read)");
    auto made = std::make_shared<const Grain>(grain_of(*png));
    std::unique_lock lock(r.mutex);
    if (const auto it = r.grains.find(ref); it != r.grains.end()) return it->second;  // (another thread was quicker)
    r.grains.emplace(ref, made);
    r.decoded.push_back(ref);
    while (r.decoded.size() > kGrainsKept) {
        r.grains.erase(r.decoded.front());
        r.decoded.pop_front();
    }
    return made;
}

void clear() {
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    r.pictures.clear();
    r.grains.clear();
    r.decoded.clear();
}

std::pair<std::int64_t, std::int64_t> offsets(const core::Paper& paper, std::string_view line_seed, int width, int height) {
    if (width <= 0 || height <= 0) throw core::Error("value", "the paper picture holds no pixels");
    std::uint64_t state = static_cast<std::uint64_t>(paper.seed);
    if (paper.coords == "stroke") state ^= fnv1a(line_seed);
    const std::uint64_t a = splitmix(state);
    const std::uint64_t b = splitmix(state);
    return {static_cast<std::int64_t>(a % static_cast<std::uint64_t>(width)), static_cast<std::int64_t>(b % static_cast<std::uint64_t>(height))};
}

std::pair<double, double> sin_cos_degrees(double degrees) {
    double r = std::fmod(degrees, 360.0);
    if (r < 0.0) r += 360.0;
    if (r >= 360.0) r = 0.0;
    const int quarter = r < 90.0 ? 0 : r < 180.0 ? 1 : r < 270.0 ? 2 : 3;
    double x = r - 90.0 * quarter;  // (exact: Sterbenz)
    const bool swap = x > 45.0;
    if (swap) x = 90.0 - x;
    auto [s, c] = polynomial(x);
    if (swap) std::swap(s, c);
    switch (quarter) {
        case 1: return {c, -s};
        case 2: return {-s, -c};
        case 3: return {-c, s};
        default: return {s, c};
    }
}

Sample sample(const Grain& grain, const core::Paper& paper, int dpi, std::int64_t px, std::int64_t py, double anchor_x_mm,
              double anchor_y_mm, std::pair<std::int64_t, std::int64_t> offset) {
    const Frame f = frame_of(grain, paper, dpi, anchor_x_mm, anchor_y_mm, offset);
    Sample out;
    sample_at(f, px, py, &out);
    return out;
}

int blend(int cover, int value, const core::Paper& paper) {
    const int taken = ink_taken(value, paper);
    return paper.blend == "subtract" ? std::max(0, cover - taken) : (cover * (255 - taken) + 127) / 255;
}

void apply(Image& mask, Point origin, int dpi, const core::Paper& paper, std::string_view line_seed, double anchor_x_mm,
           double anchor_y_mm) {
    if (mask.mode() != "L") throw core::Error("value", "a line's coverage is an L picture");
    const std::shared_ptr<const Grain> g = grain(paper.asset);
    const Frame f = frame_of(*g, paper, dpi, anchor_x_mm, anchor_y_mm, offsets(paper, line_seed, g->width, g->height));
    // the ink each value of the paper takes away (blend's)
    std::array<int, 256> taken{};
    for (int value = 0; value < 256; ++value) taken[static_cast<std::size_t>(value)] = ink_taken(value, paper);
    const bool subtract = paper.blend == "subtract";
    std::string pixels = mask.tobytes();
    const int w = mask.width();
    for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < w; ++x) {
            char& p = pixels[static_cast<std::size_t>(y) * w + x];
            const auto cover = static_cast<unsigned char>(p);
            if (cover == 0) continue;  // (no ink: nothing to take)
            const int value = sample_at(f, static_cast<std::int64_t>(origin.x) + x, static_cast<std::int64_t>(origin.y) + y, nullptr);
            const int off = taken[static_cast<std::size_t>(value)];
            p = static_cast<char>(subtract ? std::max(0, cover - off) : (cover * (255 - off) + 127) / 255);
        }
    }
    mask = Image::frombytes("L", mask.size(), pixels);
}

}  // namespace genko::render::paper
