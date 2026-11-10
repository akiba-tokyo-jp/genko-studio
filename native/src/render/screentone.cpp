#include "render/screentone.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/tones.hpp"

namespace genko::render::screentone {

using core::Json;

namespace {

const char* const kKnown[] = {"black", "white", "line_threshold", "line_contrast", "steps", "lpi", "angle", "screen",
                              "face_light", "smooth"};

double default_of(std::string_view name) {
    if (name == "black") return 40;
    if (name == "white") return 225;
    if (name == "line_threshold") return 110;
    if (name == "line_contrast") return 45;
    if (name == "lpi") return 60.0;
    if (name == "angle") return 45.0;
    if (name == "face_light") return 0.6;
    if (name == "smooth") return 1.0;
    throw core::Error("value", "no finish setting " + std::string(name));
}

// One "L" picture's bytes through a function of each value (Image.point(lambda v: …) on "L").
Image mapped(const Image& grey, const std::function<int(int)>& fn) { return grey.point(fn); }

// screentone._max_filter: the brightest value in a size×size square around each pixel, the edges repeated outwards
// (a row pass, then a column pass).
Image max_filter(const Image& grey, int size) {
    const std::string pixels = grey.tobytes();
    const int width = grey.width();
    const int height = grey.height();
    const int r = size / 2;
    std::string rows(pixels.size(), '\0');
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(pixels.data()) + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            unsigned char best = 0;
            for (int k = x - r; k <= x + r; ++k) best = std::max(best, row[std::clamp(k, 0, width - 1)]);
            rows[static_cast<std::size_t>(y) * width + x] = static_cast<char>(best);
        }
    }
    std::string out(pixels.size(), '\0');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char best = 0;
            for (int k = y - r; k <= y + r; ++k) {
                const std::size_t at = static_cast<std::size_t>(std::clamp(k, 0, height - 1)) * width + x;
                best = std::max(best, static_cast<unsigned char>(rows[at]));
            }
            out[static_cast<std::size_t>(y) * width + x] = static_cast<char>(best);
        }
    }
    return Image::frombytes("L", grey.size(), out);
}

}  // namespace

Finish Finish::from_dict(const Json& data) {
    Finish out;
    if (!data.is_object() || data.empty()) return out;  // (the caller has made it a dict: {**style, **layer.finish})
    for (const auto& [key, value] : data.items()) {
        if (value.is_null() || std::find(std::begin(kKnown), std::end(kKnown), key) == std::end(kKnown)) continue;
        out.values_[key] = value;
    }
    if (out.values_.contains("steps")) {
        std::vector<double> steps;
        for (const Json& s : core::iterate(out.values_["steps"])) {
            if (const double v = core::to_float(s); 0 < v && v < 1) steps.push_back(core::to_float(s));
        }
        std::sort(steps.begin(), steps.end());
        if (!steps.empty()) out.steps_ = std::move(steps);
    }
    return out;
}

double Finish::number(const char* name) const {
    const auto it = values_.find(name);
    if (it == values_.end()) return default_of(name);
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    if (it->is_number()) return it->get<double>();
    throw core::Error("value", std::string("the finish's ") + name + " is not a number");
}

bool Finish::fm() const {
    const auto it = values_.find("screen");
    return it != values_.end() && *it == Json("fm");
}

Image line_mask(const Image& grey, const Finish& finish, int dpi) {
    const int size = static_cast<int>(std::max<std::int64_t>(3, core::py_round_int(dpi / 100.0) * 2 + 1));
    const Image local_max = max_filter(grey, size);
    const Image contrast = chops::subtract(local_max, grey);
    const double threshold = finish.number("line_threshold");
    const Image dark = mapped(grey, [threshold](int v) { return v < threshold ? 255 : 0; });
    const double strong_at = finish.number("line_contrast");
    const Image strong = mapped(contrast, [strong_at](int v) { return v >= strong_at ? 255 : 0; });
    return chops::multiply(dark, strong);
}

Image dots(double coverage, Size size, int dpi, double lpi, double angle, Point origin) {
    const std::int64_t level = std::max<std::int64_t>(0, std::min<std::int64_t>(256, core::py_round_int(coverage * 256)));
    const auto [m, n] = tones::screen_vector(dpi, lpi, angle);
    const auto tile = tones::threshold_tile(m, n, "round");
    const int t = tile->width();
    const std::string cells = tile->tobytes();
    const int ox = ((origin.x % t) + t) % t;
    const int oy = ((origin.y % t) + t) % t;
    std::string out(static_cast<std::size_t>(size.width) * size.height, '\0');
    for (int y = 0; y < size.height; ++y) {
        const std::size_t row = static_cast<std::size_t>((y + oy) % t) * t;
        for (int x = 0; x < size.width; ++x) {
            const auto value = static_cast<unsigned char>(cells[row + static_cast<std::size_t>((x + ox) % t)]);
            out[static_cast<std::size_t>(y) * size.width + x] = static_cast<char>(value < level ? 0 : 255);
        }
    }
    return Image::frombytes("L", size, out);
}

Image noise(double coverage, Size size) {
    return Image::create("L", size, Ink(static_cast<int>(core::py_round_int(255 * (1 - coverage))))).convert("1").convert("L");
}

namespace {

// screentone.quantize: (the proof grey, [(coverage, region mask)] for each tone step).
std::pair<Image, std::vector<std::pair<double, Image>>> quantize(const Image& grey, const Finish& finish) {
    const std::vector<double>& steps = finish.steps();
    const double lo = finish.number("black");
    const double hi = finish.number("white");
    struct Band {
        double cov;
        std::int64_t a;
        std::int64_t b;
    };
    std::vector<Band> bands;
    const double span = (hi - lo) / static_cast<double>(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const double cov = steps[steps.size() - 1 - i];
        bands.push_back(Band{cov, core::py_round_int(lo + static_cast<double>(i) * span),
                             core::py_round_int(lo + static_cast<double>(i + 1) * span)});
    }
    std::vector<int> table;
    for (int v = 0; v < 256; ++v) {
        if (v <= lo) {
            table.push_back(0);
        } else if (v >= hi) {
            table.push_back(255);
        } else {
            double cov = steps.front();
            for (const Band& band : bands) {
                if (band.a < v && v <= band.b) {
                    cov = band.cov;
                    break;
                }
            }
            table.push_back(static_cast<int>(core::py_round_int(255 * (1 - cov))));
        }
    }
    Image proof = grey.point(table);
    std::vector<std::pair<double, Image>> regions;
    for (const Band& band : bands) {
        regions.emplace_back(band.cov, mapped(grey, [&band](int v) { return band.a < v && v <= band.b ? 255 : 0; }));
    }
    return {std::move(proof), std::move(regions)};
}

}  // namespace

Image finish_gray(const Image& grey_in, const Finish& finish, int dpi, bool screen, Point origin, const Image* faces,
                  const Image* dark, const Image* light) {
    const Image grey = grey_in.convert("L");
    const Image lines = line_mask(grey, finish, dpi);
    Image toned = grey;
    if (const double smooth = finish.number("smooth"); smooth > 0) {
        // (the picture's own dots and grain would beat against the screen: even them out first)
        toned = grey.filter(Filter::gaussian_blur(core::py_max(0.3, smooth * dpi / 600)));
    }
    if (dark != nullptr) {  // (clothes printed solid black: their darker half goes to black, highlights stay)
        const Image solid = mapped(toned, [](int v) { return v < 160 ? 0 : v; });
        toned = composite(solid, toned, dark->convert("L").resize(toned.size()));
    }
    if (light != nullptr) {  // (clothes printed white: only their lines and deepest shadows stay)
        const double lo = finish.number("black");
        const Image paper = mapped(toned, [lo](int v) { return v <= lo ? v : 255; });
        toned = composite(paper, toned, light->convert("L").resize(toned.size()));
    }
    if (faces != nullptr && finish.number("face_light") > 0) {
        const double keep = core::py_max(0.0, core::py_min(1.0, finish.number("face_light")));
        const double lo = finish.number("black");
        const Image lifted = mapped(toned, [lo, keep](int v) {
            return v <= lo ? v : static_cast<int>(core::py_round_int(255 - (255 - v) * (1 - keep)));
        });
        toned = composite(lifted, toned, faces->convert("L").resize(toned.size()));
    }
    auto [proof, regions] = quantize(toned, finish);
    if (!screen) return chops::darker(proof, chops::invert(lines));
    Image out = mapped(proof, [](int v) { return v == 0 ? 0 : 255; });  // solid black and paper
    for (const auto& [cov, region] : regions) {
        const Image tone = finish.fm() ? noise(cov, grey.size())
                                       : dots(cov, grey.size(), dpi, finish.number("lpi"), finish.number("angle"), origin);
        out = composite(tone, out, region);  // (where the region is on, the tone)
    }
    return chops::darker(out, chops::invert(lines));
}

}  // namespace genko::render::screentone
