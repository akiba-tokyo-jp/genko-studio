// PC3 B3: Gaussian mixture colour models with full 3×3 covariances — PhotoCraft v0.2.0's algo/src/segment/gmm.rs (MIT,
// native/third_party/photocraft/NOTICE.md): k-means++ seeding (Arthur & Vassilvitskii, SODA 2007), Lloyd iterations,
// then a few EM steps; GrabCut's update (each sample to its likeliest component, estimated again) is refit. The
// covariances get a small ridge so that flat colours stay well conditioned.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

using M3 = std::array<std::array<double, 3>, 3>;

std::optional<Component> make_component(double weight, const std::array<double, 3>& mean, M3 c, double reg) {
    for (std::size_t i = 0; i < 3; ++i) c[i][i] += reg;
    const double det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                       c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
    if (det <= 1e-30 || weight <= 0.0) return std::nullopt;
    Component out;
    out.inv = M3{{{(c[1][1] * c[2][2] - c[1][2] * c[2][1]) / det, (c[0][2] * c[2][1] - c[0][1] * c[2][2]) / det, (c[0][1] * c[1][2] - c[0][2] * c[1][1]) / det},
                  {(c[1][2] * c[2][0] - c[1][0] * c[2][2]) / det, (c[0][0] * c[2][2] - c[0][2] * c[2][0]) / det, (c[0][2] * c[1][0] - c[0][0] * c[1][2]) / det},
                  {(c[1][0] * c[2][1] - c[1][1] * c[2][0]) / det, (c[0][1] * c[2][0] - c[0][0] * c[2][1]) / det, (c[0][0] * c[1][1] - c[0][1] * c[1][0]) / det}}};
    out.log_norm = std::log(weight) - 0.5 * std::log(det) - 1.5 * std::log(2.0 * std::numbers::pi);
    out.weight = static_cast<float>(weight);
    for (std::size_t i = 0; i < 3; ++i) {
        out.mean[i] = static_cast<float>(mean[i]);
        for (std::size_t j = 0; j < 3; ++j) out.cov[i][j] = static_cast<float>(c[i][j]);
    }
    return out;
}

struct Acc {
    double n = 0;
    std::array<double, 3> s{};
    M3 ss{};

    void add(const Rgb& zf, double w) {
        const std::array<double, 3> z{zf[0], zf[1], zf[2]};
        n += w;
        for (std::size_t i = 0; i < 3; ++i) {
            s[i] += w * z[i];
            for (std::size_t j = 0; j < 3; ++j) ss[i][j] += w * z[i] * z[j];
        }
    }
    std::optional<Component> component(double total, float reg) const {
        if (n <= 0.0) return std::nullopt;
        std::array<double, 3> m{};
        for (std::size_t i = 0; i < 3; ++i) m[i] = s[i] / n;
        M3 c{};
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j) c[i][j] = ss[i][j] / n - m[i] * m[j];
        return make_component(n / total, m, c, static_cast<double>(reg));
    }
};

std::size_t nearest(const std::vector<Rgb>& centers, const Rgb& z) {
    std::size_t best = 0;
    float bd = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < centers.size(); ++i) {
        const float d = d2(centers[i], z);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

std::optional<Gmm> gmm_of(const std::vector<Acc>& acc, double total, float reg) {
    Gmm g;
    g.reg = reg;
    for (const Acc& a : acc)
        if (auto c = a.component(total, reg)) g.comps.push_back(*c);
    if (g.comps.empty()) return std::nullopt;
    return g;
}

}  // namespace

double Component::log_weighted(const Rgb& z) const {
    const double d[3] = {static_cast<double>(z[0] - mean[0]), static_cast<double>(z[1] - mean[1]), static_cast<double>(z[2] - mean[2])};
    const M3& m = inv;
    const double q = d[0] * (m[0][0] * d[0] + m[0][1] * d[1] + m[0][2] * d[2]) + d[1] * (m[1][0] * d[0] + m[1][1] * d[1] + m[1][2] * d[2]) +
                     d[2] * (m[2][0] * d[0] + m[2][1] * d[1] + m[2][2] * d[2]);
    return log_norm - 0.5 * q;
}

std::optional<Gmm> Gmm::fit(const std::vector<Rgb>& samples, std::size_t k, float reg, std::size_t em_iters) {
    if (samples.empty()) return std::nullopt;
    k = std::clamp<std::size_t>(k, 1, samples.size());
    const std::vector<Rgb> centers = kmeans(samples, k, 10);
    std::vector<std::size_t> labels(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) labels[i] = nearest(centers, samples[i]);
    std::optional<Gmm> g = from_labels(samples, labels, centers.size(), reg);
    if (!g) return std::nullopt;
    for (std::size_t i = 0; i < em_iters; ++i) {
        if (auto next = g->em_step(samples)) g = std::move(next);
    }
    return g;
}

std::optional<Gmm> Gmm::from_labels(const std::vector<Rgb>& samples, const std::vector<std::size_t>& labels, std::size_t k, float reg) {
    std::vector<Acc> acc(k);
    for (std::size_t i = 0; i < samples.size(); ++i) acc[labels[i]].add(samples[i], 1.0);
    return gmm_of(acc, static_cast<double>(samples.size()), reg);
}

std::optional<Gmm> Gmm::em_step(const std::vector<Rgb>& samples) const {
    const std::size_t k = comps.size();
    std::vector<Acc> acc(k);
    std::vector<double> lw(k, 0.0);
    for (const Rgb& z : samples) {
        double mx = std::numeric_limits<double>::lowest();
        for (std::size_t c = 0; c < k; ++c) {
            lw[c] = comps[c].log_weighted(z);
            mx = std::max(mx, lw[c]);
        }
        double sum = 0;
        for (double& l : lw) {
            l = std::exp(l - mx);
            sum += l;
        }
        for (std::size_t c = 0; c < k; ++c) {
            const double r = lw[c] / sum;
            if (r > 1e-6) acc[c].add(z, r);
        }
    }
    return gmm_of(acc, static_cast<double>(samples.size()), reg);
}

std::optional<Gmm> Gmm::refit(const std::vector<Rgb>& samples) const {
    std::vector<std::size_t> labels(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) labels[i] = component_of(samples[i]);
    return from_labels(samples, labels, comps.size(), reg);
}

std::size_t Gmm::component_of(const Rgb& z) const {
    std::size_t best = 0;
    double bv = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        const double v = comps[i].log_weighted(z);
        if (v > bv) {
            bv = v;
            best = i;
        }
    }
    return best;
}

double Gmm::log_prob(const Rgb& z) const {
    double mx = std::numeric_limits<double>::lowest();
    std::array<double, 16> vals{};
    const std::size_t n = std::min<std::size_t>(comps.size(), 16);
    for (std::size_t i = 0; i < n; ++i) {
        vals[i] = comps[i].log_weighted(z);
        mx = std::max(mx, vals[i]);
    }
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) s += std::exp(vals[i] - mx);
    return mx + std::log(s);
}

float Gmm::neg_log(const Rgb& z) const { return static_cast<float>(std::clamp(-log_prob(z), -50.0, 200.0)); }

std::vector<Rgb> kmeans(const std::vector<Rgb>& samples, std::size_t k, std::size_t iters) {
    Rng rng(0x5EED + static_cast<std::uint64_t>(samples.size()));
    std::vector<Rgb> centers{samples[static_cast<std::size_t>(rng.next_u64() % samples.size())]};
    std::vector<float> dist(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) dist[i] = d2(samples[i], centers[0]);
    while (centers.size() < k) {
        double total = 0;
        for (const float d : dist) total += static_cast<double>(d);
        if (total <= 1e-12) break;
        double t = static_cast<double>(rng.f32()) * total;
        std::size_t pick = samples.size() - 1;
        for (std::size_t i = 0; i < dist.size(); ++i) {
            t -= static_cast<double>(dist[i]);
            if (t <= 0.0) {
                pick = i;
                break;
            }
        }
        const Rgb c = samples[pick];
        centers.push_back(c);
        for (std::size_t i = 0; i < samples.size(); ++i) dist[i] = std::min(dist[i], d2(samples[i], c));
    }
    for (std::size_t it = 0; it < iters; ++it) {
        std::vector<std::pair<std::array<double, 3>, std::size_t>> sums(centers.size(), {std::array<double, 3>{0, 0, 0}, 0});
        for (const Rgb& z : samples) {
            const std::size_t i = nearest(centers, z);
            for (std::size_t c = 0; c < 3; ++c) sums[i].first[c] += static_cast<double>(z[c]);
            sums[i].second += 1;
        }
        std::vector<Rgb> fresh;
        for (const auto& [s, n] : sums) {
            if (n == 0) continue;
            fresh.push_back(Rgb{static_cast<float>(s[0] / static_cast<double>(n)), static_cast<float>(s[1] / static_cast<double>(n)),
                                static_cast<float>(s[2] / static_cast<double>(n))});
        }
        const bool done = fresh == centers;
        centers = std::move(fresh);
        if (done) break;
    }
    return centers;
}

}  // namespace genko::render::segment
