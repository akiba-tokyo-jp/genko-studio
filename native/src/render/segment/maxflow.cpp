// PC3 B3: Boykov–Kolmogorov max-flow / min-cut — PhotoCraft v0.2.0's algo/src/segment/maxflow.rs (MIT,
// native/third_party/photocraft/NOTICE.md), from Y. Boykov and V. Kolmogorov, "An Experimental Comparison of
// Min-Cut/Max-Flow Algorithms for Energy Minimization in Vision", IEEE PAMI 26(9), 2004: two search trees grow towards
// each other, each contact an augmenting path; saturated tree edges make orphans, adopted again (or freed) before the
// trees grow on. The trees are kept between augmentations.

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/segment/segment.hpp"

namespace genko::render::segment {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kTerminal = kNone - 1;
constexpr std::uint32_t kOrphan = kNone - 2;
constexpr std::uint32_t kInfDist = kNone;
// (residuals at or below this count as saturated: float round-off leaves no long chains of tiny augmentations)
constexpr float kEps = 1e-5f;

}  // namespace

Graph::Graph(std::size_t nodes, std::size_t edges)
    : first_(nodes, kNone), tr_cap_(nodes, 0.0f), parent_(nodes, kNone), is_sink_(nodes, 0), ts_(nodes, 0), dist_(nodes, 0), active_(nodes, 0) {
    head_.reserve(edges * 2);
    next_.reserve(edges * 2);
    r_cap_.reserve(edges * 2);
}

void Graph::add_tweights(std::size_t i, float cap_source, float cap_sink) {
    const float delta = tr_cap_[i];
    float s = cap_source, t = cap_sink;
    if (delta > 0.0f) {
        s += delta;
    } else {
        t -= delta;
    }
    flow_ += static_cast<double>(std::min(s, t));
    tr_cap_[i] = s - t;
}

void Graph::add_edge(std::size_t i, std::size_t j, float cap, float rev_cap) {
    const auto a = static_cast<std::uint32_t>(head_.size());
    head_.push_back(static_cast<std::uint32_t>(j));
    next_.push_back(first_[i]);
    r_cap_.push_back(cap);
    first_[i] = a;
    head_.push_back(static_cast<std::uint32_t>(i));
    next_.push_back(first_[j]);
    r_cap_.push_back(rev_cap);
    first_[j] = a + 1;
}

bool Graph::in_source(std::size_t i) const { return parent_[i] != kNone && is_sink_[i] == 0; }

double Graph::maxflow() {
    const std::size_t n = node_count();
    std::deque<std::uint32_t> queue, orphans;
    for (std::size_t i = 0; i < n; ++i) {
        active_[i] = 0;
        ts_[i] = 0;
        if (std::abs(tr_cap_[i]) > kEps) {
            is_sink_[i] = tr_cap_[i] < 0.0f ? 1 : 0;
            parent_[i] = kTerminal;
            dist_[i] = 1;
            active_[i] = 1;
            queue.push_back(static_cast<std::uint32_t>(i));
        } else {
            parent_[i] = kNone;
        }
    }
    std::uint32_t time = 0;
    std::size_t steps = 0;
    while (!queue.empty()) {
        if ((++steps & 0xFFFF) == 0) check_stop();
        const std::size_t i = queue.front();
        queue.pop_front();
        active_[i] = 0;
        if (parent_[i] == kNone) continue;
        // the growth
        std::uint32_t contact = kNone;
        const bool sink = is_sink_[i] != 0;
        for (std::uint32_t a = first_[i]; a != kNone; a = next_[a]) {
            const std::size_t j = head_[a];
            const float cap = sink ? r_cap_[a ^ 1u] : r_cap_[a];
            if (cap <= kEps) continue;
            if (parent_[j] == kNone) {
                is_sink_[j] = sink ? 1 : 0;
                parent_[j] = a ^ 1u;
                ts_[j] = ts_[i];
                dist_[j] = dist_[i] + 1;
                if (active_[j] == 0) {
                    active_[j] = 1;
                    queue.push_back(static_cast<std::uint32_t>(j));
                }
            } else if ((is_sink_[j] != 0) != sink) {
                contact = sink ? (a ^ 1u) : a;  // (an arc from the S side to the T side)
                break;
            } else if (ts_[j] <= ts_[i] && dist_[j] > dist_[i]) {
                // (shorter paths to the terminal)
                parent_[j] = a ^ 1u;
                ts_[j] = ts_[i];
                dist_[j] = dist_[i] + 1;
            }
        }
        if (contact == kNone) continue;
        // (this node scanned again after the augmentation)
        if (active_[i] == 0) {
            active_[i] = 1;
            queue.push_front(static_cast<std::uint32_t>(i));
        }
        ++time;
        augment(contact, orphans);
        // the adoption
        while (!orphans.empty()) {
            const std::size_t o = orphans.front();
            orphans.pop_front();
            adopt(o, time, queue, orphans);
        }
    }
    return flow_;
}

void Graph::augment(std::size_t m, std::deque<std::uint32_t>& orphans) {
    const std::size_t u = head_[m ^ 1u];
    const std::size_t v = head_[m];
    // the bottleneck
    float b = r_cap_[m];
    std::size_t x = u;
    for (;;) {
        const std::uint32_t a = parent_[x];
        if (a == kTerminal) break;
        b = std::min(b, r_cap_[a ^ 1u]);
        x = head_[a];
    }
    b = std::min(b, tr_cap_[x]);
    x = v;
    for (;;) {
        const std::uint32_t a = parent_[x];
        if (a == kTerminal) break;
        b = std::min(b, r_cap_[a]);
        x = head_[a];
    }
    b = std::min(b, -tr_cap_[x]);
    // the push
    r_cap_[m ^ 1u] += b;
    r_cap_[m] -= b;
    x = u;
    for (;;) {
        const std::uint32_t a = parent_[x];
        if (a == kTerminal) break;
        r_cap_[a] += b;
        r_cap_[a ^ 1u] -= b;
        const std::size_t up = head_[a];
        if (r_cap_[a ^ 1u] <= kEps) {
            parent_[x] = kOrphan;
            orphans.push_back(static_cast<std::uint32_t>(x));
        }
        x = up;
    }
    tr_cap_[x] -= b;
    if (tr_cap_[x] <= kEps) {
        parent_[x] = kOrphan;
        orphans.push_back(static_cast<std::uint32_t>(x));
    }
    x = v;
    for (;;) {
        const std::uint32_t a = parent_[x];
        if (a == kTerminal) break;
        r_cap_[a ^ 1u] += b;
        r_cap_[a] -= b;
        const std::size_t up = head_[a];
        if (r_cap_[a] <= kEps) {
            parent_[x] = kOrphan;
            orphans.push_back(static_cast<std::uint32_t>(x));
        }
        x = up;
    }
    tr_cap_[x] += b;
    if (tr_cap_[x] >= -kEps) {
        parent_[x] = kOrphan;
        orphans.push_back(static_cast<std::uint32_t>(x));
    }
    flow_ += static_cast<double>(b);
}

void Graph::adopt(std::size_t i, std::uint32_t time, std::deque<std::uint32_t>& queue, std::deque<std::uint32_t>& orphans) {
    const bool sink = is_sink_[i] != 0;
    std::uint32_t best = kNone;
    std::uint32_t d_min = kInfDist;
    for (std::uint32_t a = first_[i]; a != kNone; a = next_[a]) {
        const float cap = sink ? r_cap_[a] : r_cap_[a ^ 1u];
        const std::size_t j = head_[a];
        if (!(cap > kEps && (is_sink_[j] != 0) == sink && parent_[j] != kNone)) continue;
        // does j's path lead to a terminal?
        std::uint32_t d = 0;
        std::size_t k = j;
        for (;;) {
            if (ts_[k] == time) {
                d = dist_[k] > kInfDist - d ? kInfDist : d + dist_[k];  // (saturating)
                break;
            }
            const std::uint32_t pa = parent_[k];
            d += 1;
            if (pa == kTerminal) {
                ts_[k] = time;
                dist_[k] = 1;
                break;
            }
            if (pa == kOrphan) {
                d = kInfDist;
                break;
            }
            k = head_[pa];
        }
        if (d < kInfDist) {
            if (d < d_min) {
                best = a;
                d_min = d;
            }
            // (the distances cached along the path)
            k = j;
            std::uint32_t dd = d;
            while (ts_[k] != time) {
                ts_[k] = time;
                dist_[k] = dd;
                dd -= 1;
                k = head_[parent_[k]];
            }
        }
    }
    if (best != kNone) {
        parent_[i] = best;
        ts_[i] = time;
        dist_[i] = d_min + 1;
        return;
    }
    // no parent: i becomes free
    parent_[i] = kNone;
    for (std::uint32_t a = first_[i]; a != kNone; a = next_[a]) {
        const std::size_t j = head_[a];
        const std::uint32_t pj = parent_[j];
        if ((is_sink_[j] != 0) != sink || pj == kNone) continue;
        const float cap = sink ? r_cap_[a] : r_cap_[a ^ 1u];
        if (cap > kEps && active_[j] == 0) {
            active_[j] = 1;
            queue.push_back(static_cast<std::uint32_t>(j));
        }
        if (pj != kTerminal && pj != kOrphan && head_[pj] == i) {
            parent_[j] = kOrphan;
            orphans.push_back(static_cast<std::uint32_t>(j));
        }
    }
}

float Graph::residual_out_of_source_side() const {
    float s = 0;
    for (std::size_t i = 0; i < node_count(); ++i) {
        if (!in_source(i)) continue;
        for (std::uint32_t a = first_[i]; a != kNone; a = next_[a]) {
            if (!in_source(head_[a])) s += r_cap_[a];
        }
    }
    return s;
}

}  // namespace genko::render::segment
