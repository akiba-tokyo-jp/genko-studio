#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// PC4 E4 (gradient.presets.importGrd, SPEC.md COLOR-09「グラデーションのプリセット」): Photoshop's gradient files (.grd,
// version 5: Photoshop 6 and later) — "8BGR", the version (5), then a versioned descriptor whose GrdL list holds Grdn
// objects, each a Grad: "Nm  " its name, "GrdF" its form (CstS stops, ClNs noise), "Clrs" its colour stops ("Clr " a
// colour, "Type" a colour of its own / the foreground / the background, "Lctn" 0..4096, "Mdpn" the midpoint %) and
// "Trns" its transparency stops ("Opct" %, "Lctn", "Mdpn"). PhotoCraft v0.2.0's crates/psd/src/grd.rs (from public
// descriptions of the format), MIT: native/third_party/photocraft/NOTICE.md; read with Genko's descriptor reader
// (render/psd_descriptor). The colours are read in the model they are kept in; to_presets() makes them sRGB.

namespace genko::render::grd {

inline constexpr std::size_t kMaxGradients = 10'000;  // (read from one file)
inline constexpr std::size_t kMaxStops = 1'000;       // (each gradient's)
inline constexpr std::size_t kMaxBytes = 512u << 20;  // (a file read)

struct Colour {
    enum class Kind { Rgb, Hsb, Cmyk, Lab, Gray, Foreground, Background };
    Kind kind = Kind::Rgb;
    // Rgb: 0..1 each; Hsb: the hue in degrees, saturation and brightness 0..1; Cmyk: inks 0..1; Lab: L 0..100, a, b;
    // Gray: the ink 0..1 (0 white).
    std::array<double, 4> values{};
    bool operator==(const Colour&) const = default;
};

struct Stop {
    double location = 0;   // 0..1
    double midpoint = 0.5;  // 0..1 (0.5: centred)
    Colour colour;
    bool operator==(const Stop&) const = default;
};

struct Opacity {
    double location = 0, opacity = 1, midpoint = 0.5;  // (each 0..1)
    bool operator==(const Opacity&) const = default;
};

struct Gradient {
    std::string name;
    bool noise = false;  // (ClNs: no stops)
    std::vector<Stop> stops;
    std::vector<Opacity> opacity;
    bool operator==(const Gradient&) const = default;
};

// The gradients of a .grd file. core::Error("format") for what is not one (another signature, a version other than 5,
// a descriptor that cannot be read, no gradients in it).
std::vector<Gradient> read(std::string_view data);
// A version 5 .grd file of these gradients (their stops' own colours as RGB — another model's written as black —, the
// foreground and the background): what the tests make their files with, as upstream's.
std::string write(const std::vector<Gradient>& gradients);

// The gradients as Genko's gradient presets (render/gradient_presets: sRGB colours, "foreground", "background", the
// transparency 0..1; each name made unique among them), and what was not kept as it was in `warnings`: a noise gradient
// (left out), midpoints other than 50 % (drawn at 50 %), more than 16 stops (the first 16), a colour of another model
// (made sRGB: HSB as it is, CMYK by the simple sums, Lab from D50, grey as its ink).
core::Json to_presets(const std::vector<Gradient>& gradients, std::vector<std::string>& warnings);

}  // namespace genko::render::grd
