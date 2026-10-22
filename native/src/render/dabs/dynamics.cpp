#include "render/dabs/dynamics.hpp"

#include <algorithm>
#include <cmath>

#include "render/dabs/maths.hpp"
#include "render/dabs/rng.hpp"

// PhotoCraft v0.2.0 crates/paint/src/dynamics.rs and lib.rs, MIT: native/third_party/photocraft/NOTICE.md. Upstream's
// pixels are kPx mm here (its thresholds and floors too); its f32 weights are kept, its angles are degrees.

namespace genko::render::dabs {

namespace {

using core::DabControl;
using core::DabDynamic;

// (upstream's 1e-9 pixel: a segment too short to have a direction)
constexpr double kTiny = 1e-9 * kPx;

InputPoint lerp_pt(const InputPoint& a, const InputPoint& b, double f) {
    return InputPoint{a.x + (b.x - a.x) * f,
                      a.y + (b.y - a.y) * f,
                      a.pressure + (b.pressure - a.pressure) * f,
                      a.tilt_x + (b.tilt_x - a.tilt_x) * f,
                      a.tilt_y + (b.tilt_y - a.tilt_y) * f,
                      a.rotation + (b.rotation - a.rotation) * f,
                      a.wheel + (b.wheel - a.wheel) * f,
                      a.time + (b.time - a.time) * f};
}

bool active(const DabDynamic& d) { return d.jitter > 0.0 || d.control != DabControl::Off; }

float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// A 0..1 multiplier from a dynamic: the control lerps from the minimum to 1, the jitter takes some of it away at random,
// never below the minimum.
float scalar(const DabDynamic& d, const StepInput& s, float r) {
    if (!active(d)) return 1.0f;
    const float min = clamp01(d.minimum);
    const float cv = control_value(d.control, d.fade_steps, s);
    const float v = (min + (1.0f - min) * cv) * (1.0f - clamp01(d.jitter) * r);
    return std::max(v, min);
}

float rem1(float v) {
    const float r = std::fmod(v, 1.0f);
    return r < 0.0f ? r + 1.0f : r;
}

std::array<float, 3> rgb_to_hsv(const std::array<float, 3>& c) {
    const float mx = std::max({c[0], c[1], c[2]});
    const float mn = std::min({c[0], c[1], c[2]});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 0.0f) {
        if (mx == c[0]) {
            h = std::fmod((c[1] - c[2]) / d, 6.0f);
            if (h < 0.0f) h += 6.0f;
        } else if (mx == c[1]) {
            h = (c[2] - c[0]) / d + 2.0f;
        } else {
            h = (c[0] - c[1]) / d + 4.0f;
        }
        h /= 6.0f;
    }
    return {h, mx > 0.0f ? d / mx : 0.0f, mx};
}

std::array<float, 3> hsv_to_rgb(float h, float s, float v) {
    const float h6 = rem1(h) * 6.0f;
    const float i = std::floor(h6);
    const float f = h6 - i;
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (static_cast<int>(i)) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

// The offset perpendicular to the way the line goes (y down on the page), or both ways.
void scatter_by(double& x, double& y, double ox, double oy, bool both_axes, double direction) {
    if (both_axes) {
        x += ox;
        y += oy;
        return;
    }
    const auto [sn, cs] = sin_cos(direction);
    const double dx = cs, dy = -sn;
    x += -dy * ox;
    y += dx * ox;
}

}  // namespace

// --- smoothing ---------------------------------------------------------------------------------------------------------

Smoother::Smoother(const Smoothing& smoothing, double string_mm) : cfg_(smoothing), string_mm_(string_mm) {}

void Smoother::push(const InputPoint& p, std::vector<InputPoint>& out) {
    const std::optional<InputPoint> prev_input = last_input_;
    last_input_ = p;
    if (!(cfg_.amount > 0.0)) {
        out.push_back(p);
        return;
    }
    if (!pos_) {
        pos_ = p;
        out.push_back(p);
        return;
    }
    const double amount = std::clamp(cfg_.amount, 0.0, 1.0);
    if (cfg_.pulled_string) {
        const double len = amount * string_mm_;
        const double dx = p.x - pos_->x, dy = p.y - pos_->y;
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d > len) {
            const double k = (d - len) / d;
            InputPoint np = p;
            np.x = pos_->x + dx * k;
            np.y = pos_->y + dy * k;
            pos_ = np;
            out.push_back(np);
        }
        return;
    }
    // exponential: each step moves `a` of the way to the pointer
    const double a = 1.0 - std::min(amount, 0.95);
    std::size_t iters = 1;
    if (prev_input && cfg_.catch_up && p.time > prev_input->time) {
        iters = static_cast<std::size_t>(std::clamp(std::round((p.time - prev_input->time) / 16.0), 1.0, 64.0));
    }
    InputPoint cur = *pos_;
    for (std::size_t i = 0; i < iters; ++i) {
        InputPoint next = p;
        next.x = cur.x + (p.x - cur.x) * a;
        next.y = cur.y + (p.y - cur.y) * a;
        cur = next;
        out.push_back(cur);
    }
    pos_ = cur;
}

void Smoother::finish(std::vector<InputPoint>& out) {
    if (!(cfg_.amount > 0.0) || !cfg_.catch_up_on_end) return;
    if (pos_ && last_input_ && (pos_->x != last_input_->x || pos_->y != last_input_->y)) {
        pos_ = last_input_;
        out.push_back(*last_input_);
    }
}

std::vector<InputPoint> smooth_points(const Smoothing& smoothing, double string_mm, const std::vector<InputPoint>& points) {
    Smoother s(smoothing, string_mm);
    std::vector<InputPoint> out;
    out.reserve(points.size());
    for (const InputPoint& p : points) s.push(p, out);
    s.finish(out);
    return out;
}

// --- the walk ----------------------------------------------------------------------------------------------------------

PathWalker::PathWalker(std::optional<double> build_up_interval_ms) : interval_(build_up_interval_ms) {}

void PathWalker::step_out(const InputPoint& p, const std::function<void(const StepInput&)>& out) {
    out(StepInput{p, dir_, initial_dir_.value_or(dir_), step_});
    ++step_;
}

void PathWalker::push(const InputPoint& p, const std::function<double(const InputPoint&)>& step_len,
                      const std::function<void(const StepInput&)>& out) {
    if (!last_) {
        last_ = p;
        pending_first_ = p;
        next_at_ = step_len(p);
        return;
    }
    const InputPoint a = *last_;
    const double dx = p.x - a.x, dy = p.y - a.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len > kTiny) {
        dir_ = atan2_degrees(-dy, dx);
        if (!initial_dir_) initial_dir_ = dir_;
    }
    if (pending_first_) {
        const InputPoint f = *pending_first_;
        pending_first_.reset();
        step_out(f, out);
    }
    if (len > kTiny) {
        while (next_at_ <= len + kTiny) {
            const InputPoint q = lerp_pt(a, p, std::min(next_at_ / len, 1.0));
            step_out(q, out);
            next_at_ += std::max(step_len(q), 0.25 * kPx);
        }
        next_at_ -= len;
    }
    if (interval_) {
        const double dt = p.time - a.time;
        if (dt > 0.0) {
            time_acc_ += dt;
            while (time_acc_ >= *interval_) {
                time_acc_ -= *interval_;
                step_out(lerp_pt(a, p, std::clamp((dt - time_acc_) / dt, 0.0, 1.0)), out);
            }
        }
    }
    last_ = p;
}

void PathWalker::finish(const std::function<void(const StepInput&)>& out) {
    if (pending_first_) {
        const InputPoint f = *pending_first_;
        pending_first_.reset();
        step_out(f, out);
    }
}

// --- the dynamics ------------------------------------------------------------------------------------------------------

float control_value(DabControl control, std::int64_t fade_steps, const StepInput& s) {
    const InputPoint& p = s.point;
    switch (control) {
        case DabControl::Off:
        case DabControl::InitialDirection:
        case DabControl::Direction: return 1.0f;
        case DabControl::Fade:
            return 1.0f - std::min(static_cast<float>(s.step) / static_cast<float>(std::max<std::int64_t>(fade_steps, 1)), 1.0f);
        case DabControl::Pressure: return clamp01(p.pressure);
        // (upright 1, flat 0: a mouse gives no tilt, so the full value)
        case DabControl::Tilt: return 1.0f - clamp01(std::sqrt(p.tilt_x * p.tilt_x + p.tilt_y * p.tilt_y) / 90.0);
        case DabControl::Wheel: return clamp01(p.wheel);
        case DabControl::Rotation: return static_cast<float>(rem_euclid(p.rotation, 360.0) / 360.0);
    }
    return 1.0f;
}

double control_angle(DabControl control, std::int64_t fade_steps, const StepInput& s) {
    const InputPoint& p = s.point;
    switch (control) {
        case DabControl::Off: return 0.0;
        case DabControl::Fade:
            return 360.0 * std::min(static_cast<double>(s.step) / static_cast<double>(std::max<std::int64_t>(fade_steps, 1)), 1.0);
        case DabControl::Pressure: return 360.0 * std::clamp(p.pressure, 0.0, 1.0);
        case DabControl::Tilt: return p.tilt_x == 0.0 && p.tilt_y == 0.0 ? 0.0 : atan2_degrees(-p.tilt_y, p.tilt_x);
        case DabControl::Wheel: return 360.0 * std::clamp(p.wheel, 0.0, 1.0);
        case DabControl::Rotation: return p.rotation;
        case DabControl::InitialDirection: return s.initial_direction;
        case DabControl::Direction: return s.direction;
    }
    return 0.0;
}

std::array<float, 4> dynamic_color(const LineBrush& b, const StepInput& s, std::uint64_t seed, std::uint64_t index) {
    const core::DabColor& cd = b.settings.color;
    const auto& fg = b.color;
    const auto& bg = b.background;
    const float base = cd.fg_bg.control != DabControl::Off ? 1.0f - control_value(cd.fg_bg.control, cd.fg_bg.fade_steps, s) : 0.0f;
    const float t = std::clamp(base + (1.0f - base) * clamp01(cd.fg_bg.jitter) * rand01(seed, index, stream::kFgBg), 0.0f, 1.0f);
    const std::array<float, 3> rgb{fg[0] + (bg[0] - fg[0]) * t, fg[1] + (bg[1] - fg[1]) * t, fg[2] + (bg[2] - fg[2]) * t};
    auto [h, sat, v] = rgb_to_hsv(rgb);
    h += rand_signed(seed, index, stream::kHue) * clamp01(cd.hue_jitter) * 0.5f;
    sat = std::clamp(sat + rand_signed(seed, index, stream::kSat) * clamp01(cd.saturation_jitter), 0.0f, 1.0f);
    const auto pu = static_cast<float>(std::clamp(cd.purity, -1.0, 1.0));
    sat = pu >= 0.0f ? sat + (1.0f - sat) * pu : sat * (1.0f + pu);
    v = std::clamp(v + rand_signed(seed, index, stream::kBright) * clamp01(cd.brightness_jitter), 0.0f, 1.0f);
    const auto o = hsv_to_rgb(h, sat, v);
    return {o[0], o[1], o[2], fg[3] + (bg[3] - fg[3]) * t};
}

double base_diameter(const LineBrush& b, const StepInput& s) {
    const core::DabShape& sd = b.settings.shape;
    double f = 1.0;
    if (sd.enabled && sd.size.control != DabControl::Off) {
        const double min = std::clamp(sd.size.minimum, 0.0, 1.0);
        f = min + (1.0 - min) * static_cast<double>(control_value(sd.size.control, sd.size.fade_steps, s));
    } else if (b.settings.pressure_size) {
        f = std::clamp(s.point.pressure, 0.0, 1.0);
    }
    return b.size_mm * f;
}

InputPoint apply_pose(const core::DabSettings& settings, InputPoint p) {
    const core::DabPose& pose = settings.pose;
    if (pose.enabled) {
        if (pose.override_tilt) {
            p.tilt_x = pose.tilt_x;
            p.tilt_y = pose.tilt_y;
        }
        if (pose.override_rotation) p.rotation = pose.rotation;
        if (pose.override_pressure) p.pressure = pose.pressure;
    }
    return p;
}

DabGenerator::DabGenerator(const LineBrush& brush)
    : brush_(brush),
      walker_(brush.settings.build_up ? std::optional<double>(1000.0 / std::clamp(brush.settings.build_up_rate, 0.1, 1000.0)) : std::nullopt) {
    if (brush.settings.dual.enabled) dual_walker_.emplace(std::nullopt);
}

double DabGenerator::step_len(const InputPoint& p) const {
    const StepInput s{p, 0.0, 0.0, 0};
    const double d = std::max(base_diameter(brush_, s), kPx);
    return std::max(d * std::max(brush_.settings.spacing, 0.01), 0.5 * kPx);
}

double DabGenerator::dual_step_len() const {
    const core::DabDual& d = brush_.settings.dual;
    return std::max(std::max(d.size_mm, kPx) * std::max(d.spacing, 0.01), 0.5 * kPx);
}

void DabGenerator::build(const StepInput& s, std::vector<Dab>& out) {
    const core::DabSettings& b = brush_.settings;
    const std::uint64_t seed = brush_.seed;
    const core::DabShape& sd = b.shape;
    const core::DabScatter& sc = b.scatter;
    std::int64_t count = 1;
    if (sc.enabled) {
        const float n = static_cast<float>(std::clamp<std::int64_t>(sc.count, 1, 16));
        const float k = scalar(sc.count_jitter, s, rand01(seed, s.step, stream::kCount));
        count = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::round(n * k)), 1, 16);
    }
    const double base_d = base_diameter(brush_, s);
    for (std::int64_t n = 0; n < count; ++n) {
        const std::uint64_t i = next_index_++;
        const auto r = [&](std::uint64_t st) { return rand01(seed, i, st); };
        double diameter = base_d;
        double angle = b.angle;
        double roundness = std::clamp(b.roundness, 0.01, 1.0);
        bool fx = b.flip_x, fy = b.flip_y;
        if (sd.enabled) {
            if (sd.size.jitter > 0.0) {
                const float min = clamp01(sd.size.minimum);
                diameter *= static_cast<double>(std::max(1.0f - clamp01(sd.size.jitter) * r(stream::kSize), min));
            }
            angle += control_angle(sd.angle.control, sd.angle.fade_steps, s) +
                     static_cast<double>(rand_signed(seed, i, stream::kAngle) * clamp01(sd.angle.jitter)) * 180.0;
            roundness = std::clamp(roundness * static_cast<double>(scalar(sd.roundness, s, r(stream::kRoundness))), 0.01, 1.0);
            if (sd.flip_x_jitter && r(stream::kFlipX) < 0.5f) fx = !fx;
            if (sd.flip_y_jitter && r(stream::kFlipY) < 0.5f) fy = !fy;
        }
        const double radius = std::max(diameter / 2.0, 0.5 * kPx);
        double cx = s.point.x, cy = s.point.y;
        if (sc.enabled && sc.scatter.jitter > 0.0) {
            const double cv = control_value(sc.scatter.control, sc.scatter.fade_steps, s);
            const double amt = sc.scatter.jitter * cv * radius;
            scatter_by(cx, cy, static_cast<double>(rand_signed(seed, i, stream::kScatterX)) * amt,
                       static_cast<double>(rand_signed(seed, i, stream::kScatterY)) * amt, sc.both_axes, s.direction);
        }
        const core::DabTransfer& tr = b.transfer;
        float opacity = 1.0f, flow = static_cast<float>(b.flow);
        if (tr.enabled) {
            opacity = scalar(tr.opacity, s, r(stream::kOpacity));
            flow *= scalar(tr.flow, s, r(stream::kFlow));
        }
        if (b.pressure_opacity && !(tr.enabled && tr.opacity.control != DabControl::Off)) opacity *= clamp01(s.point.pressure);
        std::array<float, 4> color = brush_.color;
        if (b.color.enabled) {
            if (b.color.per_tip) {
                color = dynamic_color(brush_, s, seed, i);
            } else {
                if (!stroke_color_) stroke_color_ = dynamic_color(brush_, s, seed, 0);
                color = *stroke_color_;
            }
        }
        const core::DabTexture& tx = b.texture;
        float depth = clamp01(tx.depth);
        if (tx.enabled && tx.each_tip) depth *= scalar(tx.depth_jitter, s, r(stream::kDepth));
        out.push_back(Dab{cx, cy, radius, std::clamp(flow, 0.0f, 1.0f), angle, roundness, fx, fy, std::clamp(opacity, 0.0f, 1.0f), color, depth, i});
    }
}

void DabGenerator::build_dual(const StepInput& s, std::vector<Dab>& out) {
    const core::DabDual& d = brush_.settings.dual;
    const std::uint64_t seed = brush_.seed ^ 0xD0A1ULL;
    for (std::int64_t n = 0; n < std::clamp<std::int64_t>(d.count, 1, 16); ++n) {
        const std::uint64_t i = next_dual_++;
        const double radius = std::max(d.size_mm / 2.0, 0.5 * kPx);
        const double amt = d.scatter * radius;
        double cx = s.point.x, cy = s.point.y;
        scatter_by(cx, cy, static_cast<double>(rand_signed(seed, i, stream::kDualX)) * amt, static_cast<double>(rand_signed(seed, i, stream::kDualY)) * amt,
                   d.both_axes, s.direction);
        const bool flip = d.flip && rand01(seed, i, stream::kDualFlip) < 0.5f;
        out.push_back(Dab{cx, cy, radius, 1.0f, d.angle, std::clamp(d.roundness, 0.01, 1.0), flip, false, 1.0f, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, i});
    }
}

void DabGenerator::walk(const std::vector<InputPoint>& points, std::vector<Dab>& dabs, std::vector<Dab>& dual) {
    const auto len = [this](const InputPoint& q) { return step_len(q); };
    const auto dual_len = [this](const InputPoint&) { return dual_step_len(); };
    for (const InputPoint& raw : points) {
        const InputPoint p = apply_pose(brush_.settings, raw);
        std::vector<StepInput> steps;
        walker_.push(p, len, [&](const StepInput& s) { steps.push_back(s); });
        for (const StepInput& s : steps) build(s, dabs);
        if (dual_walker_) {
            std::vector<StepInput> dual_steps;
            dual_walker_->push(p, dual_len, [&](const StepInput& s) { dual_steps.push_back(s); });
            for (const StepInput& s : dual_steps) build_dual(s, dual);
        }
    }
}

void DabGenerator::push(const std::vector<InputPoint>& points, std::vector<Dab>& dabs, std::vector<Dab>& dual) { walk(points, dabs, dual); }

void DabGenerator::finish(std::vector<Dab>& dabs, std::vector<Dab>& dual) {
    std::vector<StepInput> steps;
    walker_.finish([&](const StepInput& s) { steps.push_back(s); });
    for (const StepInput& s : steps) build(s, dabs);
    if (dual_walker_) {
        std::vector<StepInput> dual_steps;
        dual_walker_->finish([&](const StepInput& s) { dual_steps.push_back(s); });
        for (const StepInput& s : dual_steps) build_dual(s, dual);
    }
}

std::vector<Dab> all_dabs(const LineBrush& brush, const std::vector<InputPoint>& points) {
    DabGenerator g(brush);
    std::vector<Dab> out, dual;
    g.push(points, out, dual);
    g.finish(out, dual);
    return out;
}

}  // namespace genko::render::dabs
