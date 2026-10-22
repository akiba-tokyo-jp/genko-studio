#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "core/brush_dabs.hpp"

// PhotoCraft v0.2.0 crates/paint/src/dynamics.rs and lib.rs (MIT: native/third_party/photocraft/NOTICE.md): from a
// line's points to its dabs — the brush pose, the walk along the line (spacing, the airbrush's build-up in time) and
// each dab's dynamics (shape, scatter, transfer, colour, the texture's depth). In millimetres: upstream's pixels are
// pixels of the working 200 dpi (kPx mm), so a dab's place, size and angle are fixed before any resolution and the
// screen and the print draw the same dabs. The smoothing (smoothing, pulled string, catch-up) is the window's, on the
// pen's input (a line keeps the points it makes): Smoother.
//
// Everything is incremental (points in any chunks give the same dabs) and deterministic: a jitter is a hash of the
// line's seed and the dab's place along it (render/dabs/rng.hpp).

namespace genko::render::dabs {

// One of upstream's pixels: a pixel of the working 200 dpi, in mm.
inline constexpr double kPx = 25.4 / 200.0;

// One input sample (a mouse: full pressure, no tilt or turn, the wheel full).
struct InputPoint {
    double x = 0.0, y = 0.0;  // mm
    double pressure = 1.0;    // 0..1
    double tilt_x = 0.0, tilt_y = 0.0;  // degrees -90..90
    double rotation = 0.0;    // degrees 0..360 (the barrel's turn)
    double wheel = 1.0;       // 0..1 (the airbrush wheel)
    double time = 0.0;        // ms
    friend bool operator==(const InputPoint&, const InputPoint&) = default;
};

// A dab laid along the line (after the dynamics).
struct Dab {
    double x = 0.0, y = 0.0;  // its centre, mm
    double radius = 0.0;      // mm
    float alpha = 1.0f;       // its flow: how much it builds up
    double angle = 0.0;       // degrees, counter-clockwise
    double roundness = 1.0;
    bool flip_x = false, flip_y = false;
    float opacity = 1.0f;     // the ceiling it builds up to (Transfer › Opacity)
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};  // straight RGBA (colour dynamics)
    float depth = 1.0f;       // the texture's depth for a texture on each tip
    std::uint64_t index = 0;  // its place along the line (0 first)
    friend bool operator==(const Dab&, const Dab&) = default;
};

// What a line draws with: the brush's dab settings and what the line itself gives (its width, the flow the window drew
// it with, its colours and seed).
struct LineBrush {
    core::DabSettings settings;
    double size_mm = 2.54;   // the line's width: the dabs' diameter at full pressure (upstream's size)
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};       // straight, 0..1 (the 描画色)
    std::array<float, 4> background{1.0f, 1.0f, 1.0f, 1.0f};  // the 背景色 (colour dynamics' foreground/background)
    std::uint64_t seed = 0;  // the brush's seed xor the line's id (FNV-1a 64)
};

// --- smoothing (the window's) ------------------------------------------------------------------------------------------

struct Smoothing {
    double amount = 0.0;  // 0..1
    bool pulled_string = false;   // paint only when the pointer pulls a string of amount × the longest string taut
    bool catch_up = true;         // keep catching up while the pointer pauses (by the points' times)
    bool catch_up_on_end = true;  // finish the line at the last pointer position
};

class Smoother {
public:
    // `string_mm`: the string's length at 100 % (upstream's 100 pixels of the screen; the window gives it in mm at its
    // zoom when 「ズームに合わせる」, else 100 pixels of the working 200 dpi).
    Smoother(const Smoothing& smoothing, double string_mm);
    void push(const InputPoint& p, std::vector<InputPoint>& out);
    void finish(std::vector<InputPoint>& out);

private:
    Smoothing cfg_;
    double string_mm_;
    std::optional<InputPoint> pos_, last_input_;
};

std::vector<InputPoint> smooth_points(const Smoothing& smoothing, double string_mm, const std::vector<InputPoint>& points);

// --- the walk along the line ---------------------------------------------------------------------------------------------

// One spacing (or airbrush time) step along the line.
struct StepInput {
    InputPoint point;
    double direction = 0.0;  // degrees, counter-clockwise with y up: the way the line goes
    double initial_direction = 0.0;
    std::uint64_t step = 0;
};

class PathWalker {
public:
    explicit PathWalker(std::optional<double> build_up_interval_ms);
    void push(const InputPoint& p, const std::function<double(const InputPoint&)>& step_len, const std::function<void(const StepInput&)>& out);
    void finish(const std::function<void(const StepInput&)>& out);

private:
    void step_out(const InputPoint& p, const std::function<void(const StepInput&)>& out);
    std::optional<InputPoint> last_, pending_first_;
    double next_at_ = 0.0;
    std::optional<double> initial_dir_;
    double dir_ = 0.0;
    std::uint64_t step_ = 0;
    std::optional<double> interval_;
    double time_acc_ = 0.0;
};

// --- the dynamics ----------------------------------------------------------------------------------------------------------

// A control's value 0..1, and the angle it gives (degrees).
float control_value(core::DabControl control, std::int64_t fade_steps, const StepInput& s);
double control_angle(core::DabControl control, std::int64_t fade_steps, const StepInput& s);
// The colour dynamics of one dab (or once for the line, index 0).
std::array<float, 4> dynamic_color(const LineBrush& b, const StepInput& s, std::uint64_t seed, std::uint64_t index);
// The diameter before the jitter at a step (the size control, or pressure): what the spacing follows. mm.
double base_diameter(const LineBrush& b, const StepInput& s);

// The brush pose on an input point.
InputPoint apply_pose(const core::DabSettings& settings, InputPoint p);

// All the dabs of a line, fed in chunks (the same dabs whatever the chunks).
class DabGenerator {
public:
    explicit DabGenerator(const LineBrush& brush);
    void push(const std::vector<InputPoint>& points, std::vector<Dab>& dabs, std::vector<Dab>& dual);
    void finish(std::vector<Dab>& dabs, std::vector<Dab>& dual);

private:
    void walk(const std::vector<InputPoint>& points, std::vector<Dab>& dabs, std::vector<Dab>& dual);
    void build(const StepInput& s, std::vector<Dab>& out);
    void build_dual(const StepInput& s, std::vector<Dab>& out);
    double step_len(const InputPoint& p) const;
    double dual_step_len() const;

    LineBrush brush_;
    PathWalker walker_;
    std::optional<PathWalker> dual_walker_;
    std::optional<std::array<float, 4>> stroke_color_;
    std::uint64_t next_index_ = 0, next_dual_ = 0;
};

// The primary dabs of a whole line (lib.rs dabs).
std::vector<Dab> all_dabs(const LineBrush& brush, const std::vector<InputPoint>& points);

}  // namespace genko::render::dabs
