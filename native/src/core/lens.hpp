#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」): the geometry of フィルター → レンズ補正… and 広角補正… without pixels —
// PhotoCraft v0.2.0's algo/src/lens.rs (LensProfile, generic_profile, LensMap, auto_scale, straighten_angle) and
// algo/src/wideangle.rs (Camera, arcs, solve: the content-preserving mesh), MIT (native/third_party/photocraft/NOTICE.md).
// The pixels: render/fx_lens.cpp (filter_raster's lens_correction and adaptive_wide_angle on the three ways).
//
// レンズ補正: Brown–Conrady's radial and tangential distortion (D. C. Brown 1966; A. Conrady 1919) about the frame's centre,
// its radius normalised by half the frame's diagonal; lateral chromatic aberration as red and blue scaled about the centre;
// the profile's vignetting divided out and the 周辺光量 sliders' exposure ramp from the midpoint; the 変形's keystone, the
// angle and the scale. Every map runs from an output pixel back to where it samples. A profile is upstream's generic one of
// a 35 mm-equivalent focal length (the coordinator's decision: no maker's data, no lensfun).
// 広角補正: the lens model (equidistant fisheye r = f·θ, rectilinear r = f·tan θ, a 2:1 equirectangular sphere) turns each
// constraint's ends into rays, the scene line is the great circle through them; a mesh over the frame is solved so each
// constrained curve is straight (and level or upright when asked) while each triangle stays as similar as it can to itself
// (T. Igarashi et al. 2005, R. Carroll et al. 2009) — least squares by conjugate gradients (CGLS). Every term is linear in
// the coordinates, so the solution of a frame scaled by k is the scaled solution: render/fx_lens solves it at the working
// 200 dpi and lays it on the layer's grid.

namespace genko::core::lens {

using XY = std::array<double, 2>;

// --- レンズ補正 ------------------------------------------------------------------------------------------------------------

// A lens profile: distortion (k1, k2 radial, p1, p2 tangential), vignetting falloff 1 + v1 r² + v2 r⁴ + v3 r⁶, the red and
// blue planes' radial scale (1 + ca).
struct Profile {
    std::string name;
    double k1 = 0, k2 = 0, p1 = 0, p2 = 0;
    std::array<double, 3> vignette{0, 0, 0};
    double ca_red = 0, ca_blue = 0;
};
// The generic profile of a 35 mm-equivalent focal length (barrel and strong vignetting at the wide end, a mild pincushion
// at the long end; interpolated on the log of the focal length between 12 and 200 mm).
Profile generic_profile(double focal_35mm);

// What fills the frame where the corrected picture does not reach.
enum class Edge { Transparency, Extension, Colour };

// レンズ補正's settings (its normal form's, core/filter_kinds: lens_correction).
struct Correction {
    std::optional<Profile> profile;
    bool correct_distortion = true, correct_vignette = true, correct_ca = true;
    double distortion = 0, red_cyan = 0, blue_yellow = 0;   // −100..100
    double vignette_amount = 0, vignette_midpoint = 50;     // −100..100, 0..100
    double vertical = 0, horizontal = 0;                    // −100..100
    double angle = 0;                                       // degrees (positive turns the picture clockwise)
    double scale = 100;                                     // %
    Edge edge = Edge::Transparency;
    std::array<double, 3> edge_colour{0, 0, 0};             // 0..1 (RGB)
    bool auto_scale = false;
};
// From the normal form of lens_correction's settings ("profile" none | generic with its "focal_length").
Correction correction_of(const Json& params);

// Where an output pixel samples the source: green (and alpha, and every plate but RGB's red and blue) at p, red at pr, blue
// at pb; its colour then times `gain`.
struct Sample {
    XY p, pr, pb;
    float gain = 1;
};

// The inverse map of a correction over a frame [x0, x1) × [y0, y1) of pixels.
class Map {
public:
    Map(const Correction& lc, double x0, double y0, double x1, double y1);
    // Output pixel (x, y) → where it samples (none where the keystone sends it to infinity).
    std::optional<Sample> sample(double x, double y) const;

private:
    XY c_;
    double r_, k1_, k2_, p1_, p2_, ca_r_, ca_b_;
    std::array<double, 3> vig_profile_;
    double vig_amount_, vig_mid_;
    std::array<double, 9> hinv_;
    double sin_, cos_, scale_;
};

// The least scale (%) at which the corrected picture covers the whole frame (自動拡大・縮小: never below the scale given).
double auto_scale(const Correction& lc, double x0, double y0, double x1, double y1);
// The angle (degrees, for Correction::angle) that makes the line a → b level or upright, whichever it is nearer (角度補正).
double straighten_angle(const XY& a, const XY& b);

// --- 広角補正 --------------------------------------------------------------------------------------------------------------

enum class Model { Auto, Fisheye, Perspective, FullSpherical };
enum class Orientation { Free, Horizontal, Vertical };

struct Constraint {
    XY a, b;  // its ends (pixels of the frame's grid)
    Orientation orientation = Orientation::Free;
};

struct WideAngle {
    Model model = Model::Auto;
    double focal_length = 0;  // mm (of a 35 mm frame after the crop factor); 0: the model's default
    double crop_factor = 1;
    double scale = 100;       // %
    std::vector<Constraint> constraints;
};

// The camera over a frame: Auto resolved (a 2:1 frame a full sphere, under 17 mm a fisheye, else perspective), its focal
// length in pixels (the 35 mm-equivalent over a 36 mm long side; none: fisheye 12 mm, perspective 24 mm).
struct Camera {
    Model model = Model::Perspective;
    double f = 1;
    XY c{0, 0};
    XY size{1, 1};
    static Camera of(const WideAngle& p, double x0, double y0, double x1, double y1);
    std::array<double, 3> ray(double x, double y) const;
    std::optional<XY> pixel(const std::array<double, 3>& d) const;
    // n + 1 points along the scene line (the great circle) from a to b.
    std::vector<XY> arc(const XY& a, const XY& b, std::size_t n) const;
};

// The solved warp: an (nx + 1) × (ny + 1) grid over the frame, each vertex's output and source place (row-major), each
// constraint's curve in the source, and how far from straight the constrained curves are after it (pixels).
struct Mesh {
    std::size_t nx = 2, ny = 2;
    std::vector<std::pair<XY, XY>> verts;  // (output, source)
    std::vector<std::vector<XY>> curves;
    double residual = 0;
    std::vector<std::array<std::size_t, 3>> triangles() const;
    // The output place of a source point (bilinear in its cell).
    XY map(double x0, double y0, double x1, double y1, const XY& p) const;
};
Mesh solve(const WideAngle& p, double x0, double y0, double x1, double y1);

std::string_view to_string(Model model);

}  // namespace genko::core::lens
