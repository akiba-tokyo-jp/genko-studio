#include "service/layout_ideas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/lint.hpp"

namespace genko::service::layout_ideas {

using komawari::PanelBox;
using komawari::Shape;

namespace {

// --- random numbers: splitmix64 seeds xoshiro256**; every draw is made from its 64 bits here (no std distribution) ---

std::uint64_t splitmix(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

class Rng {
public:
    explicit Rng(std::uint64_t seed) {
        for (std::uint64_t& word : s_) word = splitmix(seed);
    }
    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }  // [0, 1)
    double range(double lo, double hi) { return lo + (hi - lo) * unit(); }
    int below(int n) { return n <= 1 ? 0 : static_cast<int>((next() >> 11) % static_cast<std::uint64_t>(n)); }
    int between(int lo, int hi) { return hi <= lo ? lo : lo + below(hi - lo + 1); }  // [lo, hi]
    bool chance(double p) { return unit() < p; }
    int sign() { return (next() >> 63) != 0 ? 1 : -1; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::array<std::uint64_t, 4> s_{};
};

std::uint64_t mix(std::uint64_t seed, std::uint64_t n) {
    std::uint64_t x = seed ^ (n * 0xd1b54a32d192ed03ULL);
    return splitmix(x);
}

// --- presets ------------------------------------------------------------------------------------------------------

struct Params {
    int min_n, max_n, min_t, max_t;
    double center_t;  // the tiers it tends to
    double slant, bleed, split, big, overlap, overflow, frameless;
    double gap_vary;  // how often a border's gap is its own (wide or tight)
};

Params params_of(Preset preset) {
    switch (preset) {
        case Preset::Talk: return {5, 7, 3, 4, 3.5, 0.05, 0.05, 0.3, 0.15, 0.1, 0.05, 0.2, 0.15};
        case Preset::Standard: return {4, 6, 2, 4, 3.0, 0.2, 0.2, 0.3, 0.4, 0.2, 0.15, 0.1, 0.2};
        case Preset::Action: return {3, 6, 2, 4, 3.0, 0.65, 0.4, 0.2, 0.55, 0.3, 0.4, 0.05, 0.3};
        case Preset::Showcase: return {1, 4, 1, 3, 2.0, 0.2, 0.7, 0.1, 0.9, 0.35, 0.45, 0.1, 0.2};
    }
    return {4, 6, 2, 4, 3.0, 0.2, 0.2, 0.3, 0.4, 0.2, 0.15, 0.1, 0.2};
}

struct Settings {
    int min_n, max_n, min_t, max_t;
    double center_t;
    double slant, bleed, split, big, overlap, overflow, frameless, gap_vary;
};

Settings settings_of(const Request& r) {
    const Params p = params_of(r.preset);
    Settings s{};
    s.min_n = r.panels > 0 ? r.panels : (r.min_panels > 0 ? r.min_panels : p.min_n);
    s.max_n = r.panels > 0 ? r.panels : (r.max_panels > 0 ? r.max_panels : std::max(p.max_n, s.min_n));
    if (r.panels == 0 && r.min_panels > 0 && r.max_panels == 0) s.max_n = std::max(s.min_n, p.max_n);
    if (r.panels == 0 && r.max_panels > 0 && r.min_panels == 0) s.min_n = std::min(r.max_panels, p.min_n);
    s.min_t = r.min_tiers > 0 ? r.min_tiers : p.min_t;
    s.max_t = r.max_tiers > 0 ? r.max_tiers : std::max(p.max_t, s.min_t);
    if (r.max_tiers > 0 && r.min_tiers == 0) s.min_t = std::min(r.max_tiers, p.min_t);
    s.center_t = p.center_t;
    const auto knob = [](const std::optional<double>& given, double fallback) { return given ? *given : fallback; };
    s.slant = knob(r.knobs.slant, p.slant);
    s.bleed = knob(r.knobs.bleed, p.bleed);
    s.split = knob(r.knobs.split, p.split);
    s.big = knob(r.knobs.big, p.big);
    s.overlap = knob(r.knobs.overlap, p.overlap);
    s.overflow = knob(r.knobs.overflow, p.overflow);
    s.frameless = knob(r.knobs.frameless, p.frameless);
    s.gap_vary = p.gap_vary;
    return s;
}

// --- a draft: tiers of columns of rows, in thousandths (ratios) and tenths of a mm (gaps, slants) ---------------------

struct Leaf {
    bool bleed = false, frameless = false, overflow = false;
};

struct Row {
    int h = 1000;
    int gap10 = -1;  // the gap after it (−1: the plan's)
    int tilt10 = 0;
    Leaf leaf;
};

struct Col {
    int w = 1000;
    std::vector<Row> rows;  // empty: one panel
    int gap10 = -1;
    int tilt10 = 0;
    Leaf leaf;
};

struct Tier {
    int h = 1000;
    std::vector<Col> cols;
    int gap10 = -1;
    int tilt10 = 0;
};

struct Inset {
    int host = 0;  // the tier panel it goes into (0-based, tiers' order)
    int x = 0, y = 0, w = 0, h = 0;  // thousandths of the basic frame
    Leaf leaf;
};

struct Draft {
    std::vector<Tier> tiers;
    std::vector<Inset> insets;
};

int leaves_of(const Draft& d) {
    int n = 0;
    for (const Tier& t : d.tiers) {
        for (const Col& c : t.cols) n += c.rows.empty() ? 1 : static_cast<int>(c.rows.size());
    }
    return n;
}

// Weights to thousandths that add up to 1000 (the remainder to the largest; each at least `least`).
std::vector<int> thousandths(const std::vector<double>& weights, int least = 40) {
    std::vector<int> out;
    double total = 0;
    for (const double w : weights) total += std::max(w, 1e-6);
    int sum = 0;
    for (const double w : weights) {
        out.push_back(std::max(least, static_cast<int>(std::floor(1000.0 * std::max(w, 1e-6) / total + 0.5))));
        sum += out.back();
    }
    if (!out.empty()) {
        std::size_t big = 0;
        for (std::size_t i = 1; i < out.size(); ++i) {
            if (out[i] > out[big]) big = i;
        }
        out[big] += 1000 - sum;
    }
    return out;
}

Json leaf_keys(Json panel, const Leaf& leaf) {
    if (leaf.bleed) panel["bleed"] = true;
    if (leaf.frameless) panel["border_mm"] = 0;
    if (leaf.overflow) panel["overflow"] = true;
    return panel;
}

void border_keys(Json& entry, int gap10, int tilt10) {
    if (gap10 >= 0) entry["gap_mm"] = gap10 / 10.0;
    if (tilt10 != 0) entry["tilt_mm"] = tilt10 / 10.0;
}

// The skeleton with temporary slots (t0…: the tier panels in the tiers' order; i0…: the insets), and those slots' leaves.
Json raw_skeleton(const Draft& d, std::vector<std::pair<std::string, Leaf>>& leaves) {
    Json tiers = Json::array();
    int n = 0;
    for (std::size_t ti = 0; ti < d.tiers.size(); ++ti) {
        const Tier& t = d.tiers[ti];
        Json cols = Json::array();
        for (std::size_t ci = 0; ci < t.cols.size(); ++ci) {
            const Col& c = t.cols[ci];
            Json col = Json::object({{"slot", ""}, {"w", c.w / 1000.0}});
            if (c.rows.empty()) {
                col["slot"] = "t" + std::to_string(n++);
                leaves.emplace_back(col["slot"].get<std::string>(), c.leaf);
            } else {
                col["slot"] = "c" + std::to_string(ti + 1) + "_" + std::to_string(ci + 1);
                Json rows = Json::array();
                for (std::size_t ri = 0; ri < c.rows.size(); ++ri) {
                    const Row& r = c.rows[ri];
                    Json row = Json::object({{"slot", "t" + std::to_string(n++)}, {"h", r.h / 1000.0}});
                    leaves.emplace_back(row["slot"].get<std::string>(), r.leaf);
                    if (ri + 1 < c.rows.size()) border_keys(row, r.gap10, r.tilt10);
                    rows.push_back(std::move(row));
                }
                col["rows"] = std::move(rows);
            }
            if (ci + 1 < t.cols.size()) border_keys(col, c.gap10, c.tilt10);
            cols.push_back(std::move(col));
        }
        Json tier = Json::object({{"h", t.h / 1000.0}, {"cols", std::move(cols)}});
        if (ti + 1 < d.tiers.size()) border_keys(tier, t.gap10, t.tilt10);
        tiers.push_back(std::move(tier));
    }
    Json out = Json::object({{"tiers", std::move(tiers)}});
    if (!d.insets.empty()) {
        Json insets = Json::array();
        for (std::size_t i = 0; i < d.insets.size(); ++i) {
            const Inset& in = d.insets[i];
            const std::string slot = "i" + std::to_string(i);
            insets.push_back(Json::object({{"slot", slot}, {"on", "t" + std::to_string(in.host)},
                                           {"rect", Json::array({in.x / 1000.0, in.y / 1000.0, in.w / 1000.0, in.h / 1000.0})}}));
            leaves.emplace_back(slot, in.leaf);
        }
        out["insets"] = std::move(insets);
    }
    Json panels = Json::array();
    for (const auto& [slot, leaf] : leaves) panels.push_back(leaf_keys(Json::object({{"slot", slot}}), leaf));
    out["panels"] = std::move(panels);
    return out;
}

void rename_slots(Json& node, const std::vector<std::pair<std::string, std::string>>& names) {
    const auto renamed = [&names](const Json& slot) {
        for (const auto& [from, to] : names) {
            if (slot == Json(from)) return Json(to);
        }
        return slot;
    };
    if (node.is_object()) {
        for (auto& [key, value] : node.items()) {
            if ((key == "slot" || key == "on") && value.is_string()) {
                value = renamed(value);
            } else {
                rename_slots(value, names);
            }
        }
    } else if (node.is_array()) {
        for (Json& item : node) rename_slots(item, names);
    }
}

// The skeleton of a draft, its slots p1… in reading order (its panels listed so), and its shape.
std::pair<Json, Shape> finish(const Draft& d, double fw, double fh) {
    std::vector<std::pair<std::string, Leaf>> leaves;
    Json skeleton = raw_skeleton(d, leaves);
    Shape raw = komawari::shape_of_plan(skeleton, fw, fh);
    std::vector<std::pair<std::string, std::string>> names;
    for (std::size_t i = 0; i < raw.panels.size(); ++i) names.emplace_back(raw.panels[i].slot, "p" + std::to_string(i + 1));
    rename_slots(skeleton["tiers"], names);
    if (skeleton.contains("insets")) rename_slots(skeleton["insets"], names);
    Json panels = Json::array();
    for (const auto& [from, to] : names) {
        for (const auto& [slot, leaf] : leaves) {
            if (slot == from) panels.push_back(leaf_keys(Json::object({{"slot", to}}), leaf));
        }
    }
    skeleton["panels"] = std::move(panels);
    for (PanelBox& box : raw.panels) {
        for (const auto& [from, to] : names) {
            if (box.slot == from) {
                box.slot = to;
                break;
            }
        }
        for (const auto& [from, to] : names) {
            if (box.host == from) {
                box.host = to;
                break;
            }
        }
    }
    return {std::move(skeleton), std::move(raw)};
}

// Where a tier panel (tiers' order) is: its tier, column and row (−1: a single panel).
struct Place {
    int tier, col, row;
};

std::vector<Place> places_of(const Draft& d) {
    std::vector<Place> out;
    for (std::size_t t = 0; t < d.tiers.size(); ++t) {
        for (std::size_t c = 0; c < d.tiers[t].cols.size(); ++c) {
            const Col& col = d.tiers[t].cols[c];
            if (col.rows.empty()) {
                out.push_back({static_cast<int>(t), static_cast<int>(c), -1});
            } else {
                for (std::size_t r = 0; r < col.rows.size(); ++r) out.push_back({static_cast<int>(t), static_cast<int>(c), static_cast<int>(r)});
            }
        }
    }
    return out;
}

Leaf& leaf_at(Draft& d, const Place& p) {
    Col& col = d.tiers[static_cast<std::size_t>(p.tier)].cols[static_cast<std::size_t>(p.col)];
    return p.row < 0 ? col.leaf : col.rows[static_cast<std::size_t>(p.row)].leaf;
}

bool on_edge(const Draft& d, const Place& p) {
    const Tier& tier = d.tiers[static_cast<std::size_t>(p.tier)];
    const Col& col = tier.cols[static_cast<std::size_t>(p.col)];
    const bool top = p.tier == 0 && (p.row <= 0);
    const bool bottom = p.tier + 1 == static_cast<int>(d.tiers.size()) && (p.row < 0 || p.row + 1 == static_cast<int>(col.rows.size()));
    return top || bottom || p.col == 0 || p.col + 1 == static_cast<int>(tier.cols.size());
}

// --- drawing a fresh draft ------------------------------------------------------------------------------------------------

int pick_tiers(Rng& rng, int lo, int hi, double center) {
    if (hi <= lo) return lo;
    std::vector<double> weights;
    double total = 0;
    for (int t = lo; t <= hi; ++t) {
        weights.push_back(1.0 / (1.0 + 1.5 * std::fabs(t - center)));
        total += weights.back();
    }
    double u = rng.unit() * total;
    for (int t = lo; t <= hi; ++t) {
        u -= weights[static_cast<std::size_t>(t - lo)];
        if (u < 0) return t;
    }
    return hi;
}

// A tier of k panels: its columns, each one panel or a stack of rows (at most 3 columns, 3 rows a column).
std::vector<int> columns_for(Rng& rng, int k, double split) {
    std::vector<int> cols(static_cast<std::size_t>(k), 1);
    bool stacked = false;
    while (cols.size() > 3 || (cols.size() >= 3 && !stacked && rng.chance(split * 0.5))) {
        const int at = rng.below(static_cast<int>(cols.size()));
        int other = at + 1 < static_cast<int>(cols.size()) ? at + 1 : at - 1;
        if (cols[static_cast<std::size_t>(at)] + cols[static_cast<std::size_t>(other)] > 3) {
            // (find any two that fit in one column)
            bool found = false;
            for (std::size_t i = 0; i + 1 < cols.size() && !found; ++i) {
                if (cols[i] + cols[i + 1] <= 3) {
                    cols[i] += cols[i + 1];
                    cols.erase(cols.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                    found = true;
                }
            }
            if (!found) break;
        } else {
            cols[static_cast<std::size_t>(at)] += cols[static_cast<std::size_t>(other)];
            cols.erase(cols.begin() + other);
        }
        stacked = true;
    }
    return cols;
}

int tilt_of(Rng& rng, double lo_mm, double hi_mm) { return rng.sign() * static_cast<int>(std::floor(rng.range(lo_mm, hi_mm) * 10 + 0.5)); }

int border_gap(Rng& rng, double vary, bool tier) {
    if (!rng.chance(vary)) return -1;
    if (tier) return rng.chance(0.5) ? rng.between(110, 150) : rng.between(30, 45);
    return rng.chance(0.5) ? rng.between(50, 60) : 20;
}

struct Built {
    Draft draft;
    int focus = -1;  // the tier panel made the 大ゴマ (tiers' order), −1: none
};

Built draw_fresh(Rng& rng, const Settings& s, int n, int wanted_focus) {
    Built out;
    int insets = 0;
    if (n >= 3 && rng.chance(s.overlap * 0.8)) insets = 1;
    if (n >= 6 && insets == 1 && rng.chance(s.overlap * 0.4)) insets = 2;
    if (wanted_focus > 0 && insets > 0 && wanted_focus > n - insets) insets = 0;  // (the 見せ場 stays a tier panel)
    const int m = n - insets;
    const int lo = std::max(s.min_t, (m + 5) / 6);
    const int hi = std::max(1, std::min(s.max_t, m));
    const int tiers = pick_tiers(rng, std::min(lo, hi), hi, s.center_t);
    // how many panels each tier holds
    std::vector<int> counts(static_cast<std::size_t>(tiers), 1);
    for (int extra = m - tiers; extra > 0; --extra) {
        int at = rng.below(tiers);
        for (int tries = 0; counts[static_cast<std::size_t>(at)] >= 6 && tries < tiers; ++tries) at = (at + 1) % tiers;
        ++counts[static_cast<std::size_t>(at)];
    }
    Draft& d = out.draft;
    for (int t = 0; t < tiers; ++t) {
        Tier tier;
        for (const int rows : columns_for(rng, counts[static_cast<std::size_t>(t)], s.split)) {
            Col col;
            if (rows > 1) col.rows.resize(static_cast<std::size_t>(rows));
            tier.cols.push_back(col);
        }
        d.tiers.push_back(tier);
    }
    // the 大ゴマ: the panel asked for, or one by chance
    const std::vector<Place> places = places_of(d);
    const double big = wanted_focus > 0 ? std::max(s.big, 0.5) : s.big;
    if (wanted_focus > 0 && wanted_focus <= static_cast<int>(places.size())) {
        out.focus = wanted_focus - 1;
    } else if (wanted_focus <= 0 && rng.chance(0.25 + 0.75 * s.big)) {
        // (a tier with few panels makes the better 大ゴマ)
        std::vector<int> fewest;
        int least = 99;
        for (std::size_t i = 0; i < places.size(); ++i) {
            const int k = counts[static_cast<std::size_t>(places[i].tier)];
            if (k < least) {
                least = k;
                fewest.clear();
            }
            if (k == least) fewest.push_back(static_cast<int>(i));
        }
        out.focus = rng.chance(0.7) ? fewest[static_cast<std::size_t>(rng.below(static_cast<int>(fewest.size())))]
                                    : rng.below(static_cast<int>(places.size()));
    }
    const Place* focus = out.focus >= 0 ? &places[static_cast<std::size_t>(out.focus)] : nullptr;
    // sizes
    std::vector<double> tier_w;
    for (int t = 0; t < tiers; ++t) {
        double w = rng.range(0.8, 1.2) * (1 + 0.12 * (counts[static_cast<std::size_t>(t)] - 1));
        if (focus != nullptr && focus->tier == t) w *= 1 + 2.2 * big;
        tier_w.push_back(w);
    }
    const std::vector<int> tier_h = thousandths(tier_w, 60);
    for (int t = 0; t < tiers; ++t) {
        Tier& tier = d.tiers[static_cast<std::size_t>(t)];
        tier.h = tier_h[static_cast<std::size_t>(t)];
        std::vector<double> col_w;
        for (std::size_t c = 0; c < tier.cols.size(); ++c) {
            double w = rng.range(0.7, 1.3) * (tier.cols[c].rows.empty() ? 1.0 : 1.15);
            if (focus != nullptr && focus->tier == t && focus->col == static_cast<int>(c)) w *= 1 + 1.6 * big;
            col_w.push_back(w);
        }
        const std::vector<int> widths = thousandths(col_w, 80);
        for (std::size_t c = 0; c < tier.cols.size(); ++c) {
            Col& col = tier.cols[c];
            col.w = widths[c];
            if (!col.rows.empty()) {
                std::vector<double> row_w;
                for (std::size_t r = 0; r < col.rows.size(); ++r) {
                    double w = rng.range(0.75, 1.25);
                    if (focus != nullptr && focus->tier == t && focus->col == static_cast<int>(c) && focus->row == static_cast<int>(r)) w *= 1 + 2.0 * big;
                    row_w.push_back(w);
                }
                const std::vector<int> heights = thousandths(row_w, 120);
                for (std::size_t r = 0; r < col.rows.size(); ++r) {
                    col.rows[r].h = heights[r];
                    if (r + 1 < col.rows.size() && rng.chance(s.slant * 0.3)) col.rows[r].tilt10 = tilt_of(rng, 3, 3 + 10 * s.slant);
                }
            }
            if (c + 1 < tier.cols.size()) {
                col.gap10 = border_gap(rng, s.gap_vary * 0.5, false);
                if (rng.chance(s.slant * 0.45)) col.tilt10 = tilt_of(rng, 4, 4 + 14 * s.slant);
            }
        }
        if (t + 1 < tiers) {
            tier.gap10 = border_gap(rng, s.gap_vary, true);
            if (rng.chance(s.slant * 0.7)) tier.tilt10 = tilt_of(rng, 6, 6 + 24 * s.slant);
        }
    }
    // the panels' own keys
    int bleeds = 0, frameless = 0, overflows = 0;
    const int max_frameless = std::max(1, n / 3);
    for (std::size_t i = 0; i < places.size(); ++i) {
        const bool is_focus = out.focus == static_cast<int>(i);
        Leaf& leaf = leaf_at(d, places[i]);
        if (on_edge(d, places[i]) && bleeds < 2 && rng.chance(is_focus ? s.bleed : s.bleed * 0.2)) {
            leaf.bleed = true;
            ++bleeds;
        }
        if (!is_focus && frameless < max_frameless && rng.chance(s.frameless * 0.35)) {
            leaf.frameless = true;
            ++frameless;
        }
        if (overflows < 2 && rng.chance(is_focus ? s.overflow * 0.8 : s.overflow * 0.12)) {
            leaf.overflow = true;
            ++overflows;
        }
    }
    d.insets.resize(static_cast<std::size_t>(insets));
    return out;
}

// Each inset set in its host — a corner of a panel (差し込み) or across its border with the next (重ね) — where the
// reading order of the two is plain (their middles well apart, or well in one row). False when it does not fit.
bool place_insets(Rng& rng, Draft& d, int focus, double fw, double fh) {
    if (d.insets.empty()) return true;
    Draft bare = d;
    bare.insets.clear();
    std::vector<std::pair<std::string, Leaf>> leaves;
    const Json skeleton = raw_skeleton(bare, leaves);
    const Shape shape = komawari::shape_of_plan(skeleton, fw, fh);
    // the tier panels' boxes by their tiers' order (the shape lists them in reading order: t<k>)
    std::vector<const PanelBox*> boxes(shape.panels.size(), nullptr);
    for (const PanelBox& box : shape.panels) {
        const std::size_t k = static_cast<std::size_t>(std::stoi(box.slot.substr(1)));
        if (k < boxes.size()) boxes[k] = &box;
    }
    const std::vector<Place> places = places_of(d);
    std::vector<int> used;
    for (Inset& inset : d.insets) {
        // the host: the 大ゴマ, else the largest panel not used yet
        int host = -1;
        if (focus >= 0 && std::find(used.begin(), used.end(), focus) == used.end()) host = focus;
        if (host < 0 || rng.chance(0.3)) {
            double best = -1;
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                if (boxes[i] == nullptr || std::find(used.begin(), used.end(), static_cast<int>(i)) != used.end()) continue;
                if (focus >= 0 && static_cast<int>(i) < focus) continue;  // (an inset before the 見せ場 would move its number)
                const double a = boxes[i]->area() * rng.range(0.8, 1.2);
                if (a > best) {
                    best = a;
                    host = static_cast<int>(i);
                }
            }
        }
        if (host < 0 || boxes[static_cast<std::size_t>(host)] == nullptr) return false;
        const PanelBox& h = *boxes[static_cast<std::size_t>(host)];
        const double side = lint::kMinPanelSideMm + 2;
        constexpr double margin = 3.0;
        double x = 0, y = 0, w = 0, ht = 0;
        bool across = rng.chance(0.45);
        // (across: the panel read next in the same tier, on the left, or the one below for the leftmost)
        const PanelBox* next = nullptr;
        if (across) {
            for (const PanelBox& b : shape.panels) {
                if (b.tier == h.tier && b.col == h.col + 1 && b.y < h.y + h.h && h.y < b.y + b.h) next = &b;
            }
            if (next == nullptr) across = false;
        }
        if (across) {
            w = std::max(side, std::min(h.w, next->w) * rng.range(0.4, 0.55));
            ht = std::max(side, std::min(h.h, next->h) * rng.range(0.32, 0.42));
            const double border = (h.x + next->x + next->w) / 2;
            x = border - w / 2;
            y = h.y + h.h - ht - margin;  // (low: read after the host, before the next)
        } else {
            w = std::max(side, h.w * rng.range(0.34, 0.46));
            ht = std::max(side, h.h * rng.range(0.3, 0.42));
            if (w > h.w * 0.6 || ht > h.h * 0.6) return false;
            const bool before = focus != host && rng.chance(0.5);  // top right: read before the host; bottom left: after
            x = before ? h.x + h.w - w - margin : h.x + margin;
            y = before ? h.y + margin : h.y + h.h - ht - margin;
        }
        if (x < 0 || y < 0 || x + w > fw || y + ht > fh) return false;
        // plain to read: the two middles well apart in height, or well in one row
        const double dy = std::fabs((y + ht / 2) - (h.y + h.h / 2));
        const double half = std::min(ht, h.h) / 2;
        if (!(dy >= half * 1.25 || dy <= half * 0.6)) return false;
        inset.host = host;
        inset.x = static_cast<int>(std::floor(x / fw * 1000 + 0.5));
        inset.y = static_cast<int>(std::floor(y / fh * 1000 + 0.5));
        inset.w = static_cast<int>(std::floor(w / fw * 1000 + 0.5));
        inset.h = static_cast<int>(std::floor(ht / fh * 1000 + 0.5));
        if (inset.x + inset.w > 1000) inset.w = 1000 - inset.x;
        if (inset.y + inset.h > 1000) inset.h = 1000 - inset.y;
        used.push_back(host);
    }
    (void)places;
    return true;
}

// --- a draft from a skeleton (変異の元) ---------------------------------------------------------------------------------

int to_thousandths(double v) { return static_cast<int>(std::floor(v * 1000 + 0.5)); }
int to_tenths(const Json& v) { return v.is_number() ? static_cast<int>(std::floor(core::to_float(v) * 10 + 0.5)) : 0; }

std::vector<int> ratios_of(const std::vector<Json>& items, const char* key) {
    std::vector<double> w;
    for (const Json& item : items) w.push_back(std::max(0.0, core::py_get(item, key).is_number() ? core::to_float(core::py_get(item, key)) : 0.0));
    return thousandths(w, 1);
}

Draft draft_of(const Json& skeleton) {
    const Json tiers = layout::resolve_tiers(skeleton);
    std::vector<std::pair<std::string, Leaf>> flags;
    for (const Json& panel : core::iterate(core::py_or(core::py_get(skeleton, "panels"), Json::array()))) {
        Leaf leaf;
        leaf.bleed = core::py_truthy(core::py_get(panel, "bleed"));
        const Json border = core::py_get(panel, "border_mm");
        leaf.frameless = border.is_number() && core::to_float(border) <= 0;
        leaf.overflow = core::py_truthy(core::py_get(panel, "overflow"));
        flags.emplace_back(core::py_str(core::py_get(panel, "slot")), leaf);
    }
    const auto flag_of = [&flags](const Json& slot) {
        for (const auto& [k, v] : flags) {
            if (slot == Json(k)) return v;
        }
        return Leaf{};
    };
    const auto gap_of = [](const Json& entry) {
        const Json g = core::py_get(entry, "gap_mm");
        return g.is_number() ? static_cast<int>(std::floor(core::to_float(g) * 10 + 0.5)) : -1;
    };
    Draft d;
    std::vector<std::string> slots;  // the tier panels' slots in the tiers' order
    const std::vector<Json> tier_list = core::iterate(tiers);
    const std::vector<int> th = ratios_of(tier_list, "h");
    for (std::size_t t = 0; t < tier_list.size(); ++t) {
        Tier tier;
        tier.h = th[t];
        tier.gap10 = gap_of(tier_list[t]);
        tier.tilt10 = to_tenths(core::py_get(tier_list[t], "tilt_mm"));
        const std::vector<Json> cols = core::iterate(core::py_or(core::py_get(tier_list[t], "cols"), Json::array()));
        const std::vector<int> cw = ratios_of(cols, "w");
        for (std::size_t c = 0; c < cols.size(); ++c) {
            Col col;
            col.w = cw[c];
            col.gap10 = gap_of(cols[c]);
            col.tilt10 = to_tenths(core::py_get(cols[c], "tilt_mm"));
            const std::vector<Json> rows = core::iterate(core::py_or(core::py_get(cols[c], "rows"), Json::array()));
            if (rows.empty()) {
                col.leaf = flag_of(core::py_get(cols[c], "slot"));
                slots.push_back(core::py_str(core::py_get(cols[c], "slot")));
            } else {
                const std::vector<int> rh = ratios_of(rows, "h");
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    Row row;
                    row.h = rh[r];
                    row.gap10 = gap_of(rows[r]);
                    row.tilt10 = to_tenths(core::py_get(rows[r], "tilt_mm"));
                    row.leaf = flag_of(core::py_get(rows[r], "slot"));
                    slots.push_back(core::py_str(core::py_get(rows[r], "slot")));
                    col.rows.push_back(row);
                }
            }
            tier.cols.push_back(col);
        }
        d.tiers.push_back(tier);
    }
    for (const Json& inset : core::iterate(core::py_or(core::py_get(skeleton, "insets"), Json::array()))) {
        const std::string on = core::py_str(core::py_get(inset, "on"));
        const auto at = std::find(slots.begin(), slots.end(), on);
        const Json rect = core::py_get(inset, "rect");
        if (at == slots.end() || !rect.is_array() || rect.size() != 4) continue;
        Inset in;
        in.host = static_cast<int>(at - slots.begin());
        in.x = to_thousandths(core::to_float(rect[0]));
        in.y = to_thousandths(core::to_float(rect[1]));
        in.w = to_thousandths(core::to_float(rect[2]));
        in.h = to_thousandths(core::to_float(rect[3]));
        in.leaf = flag_of(core::py_get(inset, "slot"));
        d.insets.push_back(in);
    }
    return d;
}

// --- variations -------------------------------------------------------------------------------------------------------

std::vector<int> jittered(Rng& rng, const std::vector<int>& values, double v, int least) {
    std::vector<double> w;
    for (const int x : values) w.push_back(std::max(1, x) * (1 + (rng.unit() * 2 - 1) * 0.6 * v));
    return thousandths(w, least);
}

void toggle_tilt(Rng& rng, int& tilt10, double lo, double hi) { tilt10 = tilt10 != 0 ? 0 : tilt_of(rng, lo, hi); }

Draft vary(Rng& rng, const Draft& base, double v, const Settings& s) {
    Draft d = base;
    // a small change: the sizes, slants, gaps and keys move, the skeleton stays
    {
        std::vector<int> th;
        for (const Tier& t : d.tiers) th.push_back(t.h);
        th = jittered(rng, th, v, 60);
        for (std::size_t t = 0; t < d.tiers.size(); ++t) d.tiers[t].h = th[t];
    }
    for (Tier& tier : d.tiers) {
        std::vector<int> cw;
        for (const Col& c : tier.cols) cw.push_back(c.w);
        cw = jittered(rng, cw, v, 80);
        for (std::size_t c = 0; c < tier.cols.size(); ++c) {
            Col& col = tier.cols[c];
            col.w = cw[c];
            if (!col.rows.empty()) {
                std::vector<int> rh;
                for (const Row& r : col.rows) rh.push_back(r.h);
                rh = jittered(rng, rh, v, 120);
                for (std::size_t r = 0; r < col.rows.size(); ++r) col.rows[r].h = rh[r];
            }
            if (col.tilt10 != 0) col.tilt10 += static_cast<int>(std::floor((rng.unit() * 2 - 1) * 80 * v + 0.5));
            if (col.gap10 >= 0 && col.gap10 < static_cast<int>(komawari::kMinGapMm * 10)) col.gap10 = static_cast<int>(komawari::kMinGapMm * 10);
        }
        if (tier.tilt10 != 0) tier.tilt10 += static_cast<int>(std::floor((rng.unit() * 2 - 1) * 120 * v + 0.5));
        if (tier.gap10 >= 0 && tier.gap10 < static_cast<int>(komawari::kMinGapMm * 10)) tier.gap10 = static_cast<int>(komawari::kMinGapMm * 10);
    }
    if (d.tiers.size() > 1 && rng.chance(0.4 * v + 0.1)) {
        Tier& tier = d.tiers[static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size()) - 1))];
        toggle_tilt(rng, tier.tilt10, 6, 6 + 24 * std::max(s.slant, 0.3));
    }
    if (d.tiers.size() > 1 && rng.chance(0.25 * v)) {
        Tier& tier = d.tiers[static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size()) - 1))];
        tier.gap10 = tier.gap10 >= 0 ? -1 : (rng.chance(0.5) ? rng.between(110, 150) : rng.between(30, 45));
    }
    const std::vector<Place> places = places_of(d);
    if (!places.empty()) {
        if (rng.chance(0.3 * v + 0.05)) {
            const Place& p = places[static_cast<std::size_t>(rng.below(static_cast<int>(places.size())))];
            if (on_edge(d, p)) leaf_at(d, p).bleed = !leaf_at(d, p).bleed;
        }
        if (rng.chance(0.2 * v)) {
            Leaf& leaf = leaf_at(d, places[static_cast<std::size_t>(rng.below(static_cast<int>(places.size())))]);
            leaf.frameless = !leaf.frameless;
        }
        if (rng.chance(0.2 * v)) {
            Leaf& leaf = leaf_at(d, places[static_cast<std::size_t>(rng.below(static_cast<int>(places.size())))]);
            leaf.overflow = !leaf.overflow;
        }
    }
    for (Inset& in : d.insets) {
        in.x = std::clamp(in.x + static_cast<int>(std::floor((rng.unit() * 2 - 1) * 40 * v + 0.5)), 0, 1000 - in.w);
        in.y = std::clamp(in.y + static_cast<int>(std::floor((rng.unit() * 2 - 1) * 40 * v + 0.5)), 0, 1000 - in.h);
    }
    // a larger one: a panel moves to the next tier, a column is stacked or unstacked, the 大ゴマ moves
    if (v >= 0.34 && rng.chance(v)) {
        const int kind = rng.below(3);
        if (kind == 0 && d.tiers.size() > 1) {
            const std::size_t from = static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size())));
            const std::size_t to = from + 1 < d.tiers.size() && (from == 0 || rng.chance(0.5)) ? from + 1 : from - 1;
            Tier& a = d.tiers[from];
            Tier& b = d.tiers[to];
            if (a.cols.size() >= 2 && b.cols.size() < 3) {
                Col moved = a.cols.back();
                a.cols.pop_back();
                if (!moved.rows.empty()) {  // (one panel of it moves; the rest stay a column)
                    Col one;
                    one.leaf = moved.rows.back().leaf;
                    moved.rows.pop_back();
                    if (moved.rows.size() == 1) {
                        const Leaf keep = moved.rows.front().leaf;
                        moved.rows.clear();
                        moved.leaf = keep;
                    }
                    a.cols.push_back(moved);
                    moved = one;
                }
                moved.gap10 = -1;
                moved.tilt10 = 0;
                a.cols.back().gap10 = -1;
                a.cols.back().tilt10 = 0;
                b.cols.push_back(moved);
                std::vector<double> w(b.cols.size(), 1.0);
                const std::vector<int> bw = thousandths(w, 80);
                for (std::size_t c = 0; c < b.cols.size(); ++c) b.cols[c].w = bw[c];
                std::vector<double> aw(a.cols.size(), 1.0);
                const std::vector<int> aws = thousandths(aw, 80);
                for (std::size_t c = 0; c < a.cols.size(); ++c) a.cols[c].w = aws[c];
            }
        } else if (kind == 1) {
            Tier& tier = d.tiers[static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size())))];
            std::size_t c = static_cast<std::size_t>(rng.below(static_cast<int>(tier.cols.size())));
            if (!tier.cols[c].rows.empty() && tier.cols.size() < 3) {  // unstack: its rows side by side
                Col stack = tier.cols[c];
                std::vector<Col> parts;
                for (const Row& r : stack.rows) {
                    Col one;
                    one.leaf = r.leaf;
                    one.w = stack.w / static_cast<int>(stack.rows.size());
                    parts.push_back(one);
                }
                if (tier.cols.size() - 1 + parts.size() <= 3) {
                    tier.cols.erase(tier.cols.begin() + static_cast<std::ptrdiff_t>(c));
                    tier.cols.insert(tier.cols.begin() + static_cast<std::ptrdiff_t>(c), parts.begin(), parts.end());
                }
            } else if (tier.cols.size() >= 3) {  // stack two neighbours (a tier of one stacked column would be tiers)
                if (c + 1 >= tier.cols.size()) c = tier.cols.size() - 2;
                Col& a = tier.cols[c];
                const Col b = tier.cols[c + 1];
                const std::size_t total = (a.rows.empty() ? 1 : a.rows.size()) + (b.rows.empty() ? 1 : b.rows.size());
                if (total <= 3) {
                    Col stack;
                    stack.w = a.w + b.w;
                    for (const Col* part : {static_cast<const Col*>(&a), &b}) {
                        if (part->rows.empty()) {
                            Row r;
                            r.leaf = part->leaf;
                            stack.rows.push_back(r);
                        } else {
                            stack.rows.insert(stack.rows.end(), part->rows.begin(), part->rows.end());
                        }
                    }
                    std::vector<double> w(stack.rows.size(), 1.0);
                    const std::vector<int> rh = thousandths(w, 120);
                    for (std::size_t r = 0; r < stack.rows.size(); ++r) {
                        stack.rows[r].h = rh[r];
                        stack.rows[r].gap10 = -1;
                        stack.rows[r].tilt10 = 0;
                    }
                    stack.gap10 = b.gap10;
                    stack.tilt10 = b.tilt10;
                    tier.cols[c] = stack;
                    tier.cols.erase(tier.cols.begin() + static_cast<std::ptrdiff_t>(c) + 1);
                }
            }
        } else if (d.tiers.size() > 1) {  // the 大ゴマ to another tier: two tiers' heights change places
            const std::size_t a = static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size())));
            std::size_t b = static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size()) - 1));
            if (b >= a) ++b;
            std::swap(d.tiers[a].h, d.tiers[b].h);
        }
    }
    // a large one: two tiers become one, or one becomes two
    if (v >= 0.67 && rng.chance(v - 0.5)) {
        if (d.tiers.size() >= 2 && rng.chance(0.5)) {
            const std::size_t t = static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size()) - 1));
            if (d.tiers[t].cols.size() + d.tiers[t + 1].cols.size() <= 3) {
                Tier merged;
                merged.h = d.tiers[t].h + d.tiers[t + 1].h;
                merged.gap10 = d.tiers[t + 1].gap10;
                merged.tilt10 = d.tiers[t + 1].tilt10;
                for (const std::size_t k : {t, t + 1}) merged.cols.insert(merged.cols.end(), d.tiers[k].cols.begin(), d.tiers[k].cols.end());
                std::vector<double> w(merged.cols.size(), 1.0);
                const std::vector<int> cw = thousandths(w, 80);
                for (std::size_t c = 0; c < merged.cols.size(); ++c) {
                    merged.cols[c].w = cw[c];
                    merged.cols[c].gap10 = -1;
                    merged.cols[c].tilt10 = 0;
                }
                d.tiers[t] = merged;
                d.tiers.erase(d.tiers.begin() + static_cast<std::ptrdiff_t>(t) + 1);
            }
        } else if (static_cast<int>(d.tiers.size()) < kMaxTiers) {
            const std::size_t t = static_cast<std::size_t>(rng.below(static_cast<int>(d.tiers.size())));
            if (d.tiers[t].cols.size() >= 2) {
                Tier upper = d.tiers[t];
                Tier lower = d.tiers[t];
                const std::size_t half = upper.cols.size() / 2;
                upper.cols.erase(upper.cols.begin() + static_cast<std::ptrdiff_t>(half), upper.cols.end());
                lower.cols.erase(lower.cols.begin(), lower.cols.begin() + static_cast<std::ptrdiff_t>(half));
                upper.h = d.tiers[t].h / 2;
                lower.h = d.tiers[t].h - upper.h;
                upper.gap10 = -1;
                upper.tilt10 = 0;
                for (Tier* part : {&upper, &lower}) {
                    std::vector<double> w(part->cols.size(), 1.0);
                    const std::vector<int> cw = thousandths(w, 80);
                    for (std::size_t c = 0; c < part->cols.size(); ++c) part->cols[c].w = cw[c];
                    part->cols.back().gap10 = -1;
                    part->cols.back().tilt10 = 0;
                }
                d.tiers[t] = upper;
                d.tiers.insert(d.tiers.begin() + static_cast<std::ptrdiff_t>(t) + 1, lower);
            }
        }
    }
    // (the last entries carry no border; the heights add up to 1000)
    std::vector<double> th;
    for (const Tier& t : d.tiers) th.push_back(t.h);
    const std::vector<int> fixed_h = thousandths(th, 60);
    for (std::size_t t = 0; t < d.tiers.size(); ++t) d.tiers[t].h = fixed_h[t];
    d.tiers.back().gap10 = -1;
    d.tiers.back().tilt10 = 0;
    for (Tier& tier : d.tiers) {
        tier.cols.back().gap10 = -1;
        tier.cols.back().tilt10 = 0;
        for (Col& col : tier.cols) {
            if (!col.rows.empty()) {
                col.rows.back().gap10 = -1;
                col.rows.back().tilt10 = 0;
            }
            col.tilt10 = std::clamp(col.tilt10, -static_cast<int>(layout::kMaxTiltMm * 10), static_cast<int>(layout::kMaxTiltMm * 10));
        }
        tier.tilt10 = std::clamp(tier.tilt10, -static_cast<int>(layout::kMaxTiltMm * 10), static_cast<int>(layout::kMaxTiltMm * 10));
    }
    const int tier_panels = leaves_of(d);
    for (Inset& in : d.insets) in.host = std::min(in.host, tier_panels - 1);
    return d;
}

// Two shapes that are the same but for small moves (each panel within 3% of the frame).
bool near_same(const Shape& a, const Shape& b) {
    if (a.count() != b.count() || komawari::difference(a, b) != 0) return false;
    for (std::size_t i = 0; i < a.panels.size(); ++i) {
        const PanelBox& p = a.panels[i];
        const PanelBox& q = b.panels[i];
        const double dx = std::max(std::fabs(p.x - q.x), std::fabs(p.w - q.w)) / std::max(1.0, a.frame_w);
        const double dy = std::max(std::fabs(p.y - q.y), std::fabs(p.h - q.h)) / std::max(1.0, a.frame_h);
        if (dx > 0.03 || dy > 0.03) return false;
    }
    return true;
}

void tally(std::vector<std::pair<std::string, int>>& dropped, const std::string& why) {
    for (auto& [k, n] : dropped) {
        if (k == why) {
            ++n;
            return;
        }
    }
    dropped.emplace_back(why, 1);
}

// Why an idea is sieved out (empty: kept), before the batch's likeness.
std::string sieve(const Shape& shape, const Request& r) {
    if (shape.count() == 0) return "unfit";
    if (komawari::smallest_side(shape) < lint::kMinPanelSideMm) return "small";
    if (komawari::narrowest_gap(shape) < komawari::kMinGapMm) return "gap";
    if (komawari::reading_break(shape) != 0) return "order";
    if (komawari::plain_grid(shape)) return "plain";
    if (r.focus > 0 && komawari::largest(shape) != static_cast<std::size_t>(r.focus)) return "focus";
    return {};
}

}  // namespace

std::string_view preset_key(Preset preset) {
    switch (preset) {
        case Preset::Talk: return "talk";
        case Preset::Standard: return "standard";
        case Preset::Action: return "action";
        case Preset::Showcase: return "showcase";
    }
    return "standard";
}

PresetDefaults preset_defaults(Preset preset) {
    const Params p = params_of(preset);
    PresetDefaults out;
    out.min_panels = p.min_n;
    out.max_panels = p.max_n;
    out.min_tiers = p.min_t;
    out.max_tiers = p.max_t;
    out.knobs = Knobs{p.slant, p.bleed, p.split, p.big, p.overlap, p.overflow, p.frameless};
    return out;
}

std::string_view preset_label(Preset preset) {
    switch (preset) {
        case Preset::Talk: return "会話";
        case Preset::Standard: return "標準";
        case Preset::Action: return "アクション";
        case Preset::Showcase: return "見せゴマ／扉";
    }
    return "標準";
}

std::optional<Preset> preset_from(std::string_view text) {
    for (const Preset p : {Preset::Talk, Preset::Standard, Preset::Action, Preset::Showcase}) {
        if (text == preset_key(p) || text == preset_label(p)) return p;
    }
    if (text == "見せゴマ" || text == "扉") return Preset::Showcase;
    return std::nullopt;
}

std::uint64_t seed_value(std::string_view text) {
    if (text.size() == 19 && text.substr(0, 3) == "km-") {
        std::uint64_t v = 0;
        bool hex = true;
        for (const char c : text.substr(3)) {
            const int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
            if (d < 0) {
                hex = false;
                break;
            }
            v = (v << 4) | static_cast<std::uint64_t>(d);
        }
        if (hex) return v;
    }
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

namespace {

int int_arg(const Json& args, const char* key, int lo, int hi, const std::string& message) {
    const Json value = core::py_get(args, key);
    if (value.is_null()) return 0;
    if (!value.is_number_integer() || core::to_int(value) < lo || core::to_int(value) > hi) throw layout::LayoutError(std::string("/") + key, message);
    return static_cast<int>(core::to_int(value));
}

double unit_arg(const Json& value, const std::string& path, const std::string& label) {
    if (!value.is_number() || !(core::to_float(value) >= 0 && core::to_float(value) <= 1)) {
        throw layout::LayoutError(path, label + "は 0〜1（今 " + core::py_str(value) + "）");
    }
    return core::to_float(value);
}

}  // namespace

Request request_from_json(const Json& args, const core::PageSpec& spec) {
    if (!args.is_object() && !args.is_null()) throw layout::LayoutError("/", "コマ割りの案の指定は {panels, preset, knobs, seed, count, …}");
    static const char* const kKeys[] = {"panels", "min_panels", "max_panels", "min_tiers", "max_tiers", "preset", "knobs", "seed",
                                        "count", "base", "variation", "focus"};
    if (args.is_object()) {
        for (const auto& [key, value] : args.items()) {
            if (std::none_of(std::begin(kKeys), std::end(kKeys), [&key](const char* k) { return key == k; })) {
                throw layout::LayoutError("/" + key, key + " は使えない項目（panels, min_panels, max_panels, min_tiers, max_tiers, preset, knobs, seed, count, base, variation, focus）");
            }
        }
    }
    const Json a = args.is_object() ? args : Json::object();
    Request r;
    const auto [fw, fh] = komawari::frame_size(spec);
    r.frame_w_mm = fw;
    r.frame_h_mm = fh;
    const int most = static_cast<int>(layout::kMaxPanels);
    const std::string panels_message = "コマ数は 1〜" + std::to_string(most) + "（1ページのコマは " + std::to_string(most) + " 個まで）";
    r.panels = int_arg(a, "panels", 1, most, panels_message);
    r.min_panels = int_arg(a, "min_panels", 1, most, panels_message);
    r.max_panels = int_arg(a, "max_panels", 1, most, panels_message);
    if (r.min_panels > 0 && r.max_panels > 0 && r.min_panels > r.max_panels) throw layout::LayoutError("/min_panels", "min_panels が max_panels より大きい");
    const std::string tiers_message = "段数は 1〜" + std::to_string(kMaxTiers);
    r.min_tiers = int_arg(a, "min_tiers", 1, kMaxTiers, tiers_message);
    r.max_tiers = int_arg(a, "max_tiers", 1, kMaxTiers, tiers_message);
    if (r.min_tiers > 0 && r.max_tiers > 0 && r.min_tiers > r.max_tiers) throw layout::LayoutError("/min_tiers", "min_tiers が max_tiers より大きい");
    if (const Json preset = core::py_get(a, "preset"); !preset.is_null()) {
        const auto p = preset.is_string() ? preset_from(preset.get<std::string>()) : std::nullopt;
        if (!p) throw layout::LayoutError("/preset", "プリセットは talk（会話）・standard（標準）・action（アクション）・showcase（見せゴマ／扉）のどれか");
        r.preset = *p;
    }
    if (const Json knobs = core::py_get(a, "knobs"); !knobs.is_null()) {
        if (!knobs.is_object()) throw layout::LayoutError("/knobs", "つまみは {slant, bleed, split, big, overlap, overflow, frameless}（各 0〜1）");
        static const std::pair<const char*, const char*> kLabels[] = {{"slant", "斜め"}, {"bleed", "断ち切り"}, {"split", "コマ内分割"},
                                                                       {"big", "大ゴマ率"}, {"overlap", "重ね"}, {"overflow", "はみ出し"},
                                                                       {"frameless", "枠なし"}};
        for (const auto& [key, value] : knobs.items()) {
            const auto at = std::find_if(std::begin(kLabels), std::end(kLabels), [&key](const auto& kv) { return key == kv.first; });
            if (at == std::end(kLabels)) throw layout::LayoutError("/knobs/" + key, key + " というつまみはない（slant, bleed, split, big, overlap, overflow, frameless）");
            if (value.is_null()) continue;
            const double v = unit_arg(value, "/knobs/" + key, at->second);
            std::optional<double>* slot = key == "slant" ? &r.knobs.slant : key == "bleed" ? &r.knobs.bleed : key == "split" ? &r.knobs.split
                                        : key == "big" ? &r.knobs.big : key == "overlap" ? &r.knobs.overlap : key == "overflow" ? &r.knobs.overflow
                                                                                                                      : &r.knobs.frameless;
            *slot = v;
        }
    }
    if (const Json seed = core::py_get(a, "seed"); !seed.is_null()) {
        if (!seed.is_string() && !seed.is_number_integer()) throw layout::LayoutError("/seed", "seed は文字列か整数");
        r.seed = core::py_str(seed);
    }
    const int count = int_arg(a, "count", 1, kMaxCount, "束の数（count）は 1〜" + std::to_string(kMaxCount));
    r.count = count > 0 ? count : kDefaultCount;
    if (const Json base = core::py_get(a, "base"); !base.is_null()) {
        if (!base.is_object()) throw layout::LayoutError("/base", "変異の元（base）は骨組み {tiers, insets?, panels}");
        r.base = base;
    }
    if (const Json variation = core::py_get(a, "variation"); !variation.is_null()) r.variation = unit_arg(variation, "/variation", "変異度（variation）");
    r.focus = int_arg(a, "focus", 1, most, "見せ場のコマ（focus）は 1〜" + std::to_string(most) + " 番");
    if (r.panels > 0 && r.focus > r.panels) throw layout::LayoutError("/focus", "見せ場のコマ（focus）がコマ数より大きい");
    return r;
}

Json request_to_json(const Request& r) {
    Json knobs = Json::object();
    const auto put = [&knobs](const char* key, const std::optional<double>& v) {
        if (v) knobs[key] = *v;
    };
    put("slant", r.knobs.slant);
    put("bleed", r.knobs.bleed);
    put("split", r.knobs.split);
    put("big", r.knobs.big);
    put("overlap", r.knobs.overlap);
    put("overflow", r.knobs.overflow);
    put("frameless", r.knobs.frameless);
    Json out = Json::object({{"preset", std::string(preset_key(r.preset))}, {"knobs", std::move(knobs)}, {"count", r.count}, {"seed", r.seed}});
    const auto put_int = [&out](const char* key, int v) {
        if (v > 0) out[key] = v;
    };
    put_int("panels", r.panels);
    put_int("min_panels", r.min_panels);
    put_int("max_panels", r.max_panels);
    put_int("min_tiers", r.min_tiers);
    put_int("max_tiers", r.max_tiers);
    put_int("focus", r.focus);
    if (r.base) {
        out["base"] = *r.base;
        out["variation"] = r.variation;
    }
    return out;
}

Batch generate(const Request& r) {
    Batch out;
    out.seed = !r.seed.empty() ? r.seed : "km-" + (core::new_id() + core::new_id()).substr(0, 16);
    const std::uint64_t seed = seed_value(out.seed);
    const Settings s = settings_of(r);
    const int count = std::clamp(r.count, 1, kMaxCount);
    const double fw = r.frame_w_mm, fh = r.frame_h_mm;
    std::optional<Draft> base;
    std::optional<Shape> base_shape;
    if (r.base) {
        Json checked = *r.base;
        for (const studio::Issue& issue : layout::check_tiers(checked)) {
            if (issue.severity == "error") throw layout::LayoutError("/base" + issue.path, issue.message);
        }
        base = draft_of(checked);
        base_shape = komawari::shape_of_plan(checked, fw, fh);
    } else if (s.min_n > s.max_n) {
        throw layout::LayoutError("/min_panels", "min_panels が max_panels より大きい");
    }
    const int budget = count * 60;
    int similar_below = komawari::kSimilarBelow;
    for (int attempt = 0; attempt < budget && static_cast<int>(out.ideas.size()) < count; ++attempt) {
        // (half the budget spent and the batch not full: alike ideas are let in, still not the same ones)
        if (!base && attempt == budget / 2 && similar_below > 1) similar_below = 1;
        Rng rng(mix(seed, static_cast<std::uint64_t>(attempt)));
        ++out.tried;
        Draft draft;
        if (base) {
            draft = vary(rng, *base, r.variation, s);
        } else {
            const int n = rng.between(s.min_n, s.max_n);
            Built built = draw_fresh(rng, s, n, r.focus);
            if (!place_insets(rng, built.draft, built.focus, fw, fh)) {
                tally(out.dropped, "unfit");
                continue;
            }
            draft = std::move(built.draft);
        }
        std::pair<Json, Shape> made;
        bool fits = true;
        try {
            made = finish(draft, fw, fh);
            for (const studio::Issue& issue : layout::check_tiers(made.first)) fits = fits && issue.severity != "error";
        } catch (const core::ApplyError&) {
            fits = false;
        }
        if (!fits) {
            tally(out.dropped, "unfit");
            continue;
        }
        const std::string why = sieve(made.second, r);
        if (!why.empty()) {
            tally(out.dropped, why);
            continue;
        }
        if (base_shape && near_same(made.second, *base_shape)) {
            tally(out.dropped, "same_as_base");
            continue;
        }
        bool alike = false;
        for (const Idea& idea : out.ideas) {
            alike = alike || (base ? near_same(made.second, idea.shape) : komawari::difference(made.second, idea.shape) < similar_below ||
                                                                               near_same(made.second, idea.shape));
        }
        if (alike) {
            tally(out.dropped, "similar");
            continue;
        }
        Idea idea;
        idea.skeleton = std::move(made.first);
        idea.shape = std::move(made.second);
        idea.summary = komawari::describe(idea.shape);
        out.ideas.push_back(std::move(idea));
    }
    return out;
}

Json batch_to_json(const Batch& batch) {
    Json ideas = Json::array();
    for (const Idea& idea : batch.ideas) {
        Json order = Json::array();
        for (const PanelBox& box : idea.shape.panels) order.push_back(box.slot);
        const std::size_t big = komawari::largest(idea.shape);
        ideas.push_back(Json::object({{"skeleton", idea.skeleton},
                                      {"summary", idea.summary},
                                      {"panels", idea.shape.count()},
                                      {"pattern", idea.shape.pattern},
                                      {"largest", big > 0 ? Json(idea.shape.panels[big - 1].slot) : Json()},
                                      {"reading_order", std::move(order)}}));
    }
    Json dropped = Json::object();
    for (const auto& [why, n] : batch.dropped) dropped[why] = n;
    return Json::object({{"seed", batch.seed}, {"ideas", std::move(ideas)}, {"tried", batch.tried}, {"dropped", std::move(dropped)}});
}

Json merge_into_plan(const Json& skeleton, const Json& plan) {
    Json out = plan.is_object() ? plan : Json::object();
    out["template"] = nullptr;
    out["tiers"] = core::py_get(skeleton, "tiers");
    for (const char* key : {"insets", "tier_gap_mm", "col_gap_mm"}) {
        const Json value = core::py_get(skeleton, key);
        if (!value.is_null()) {
            out[key] = value;
        } else if (out.contains(key)) {
            out[key] = nullptr;
        }
    }
    Json panels = out.contains("panels") && out["panels"].is_array() ? out["panels"] : Json::array();
    for (const Json& given : core::iterate(core::py_or(core::py_get(skeleton, "panels"), Json::array()))) {
        const Json slot = core::py_get(given, "slot");
        Json* panel = nullptr;
        for (Json& p : panels) {
            if (p.is_object() && core::py_get(p, "slot") == slot) panel = &p;
        }
        if (panel == nullptr) {
            panels.push_back(Json::object({{"slot", slot}}));
            panel = &panels.back();
        }
        for (const char* key : {"bleed", "border_mm", "overflow"}) {
            const Json value = core::py_get(given, key);
            if (!value.is_null()) {
                (*panel)[key] = value;
            } else if (panel->contains(key)) {
                (*panel)[key] = nullptr;
            }
        }
    }
    out["panels"] = std::move(panels);
    return out;
}

layout::CompiledLayout apply_idea(Work& work, const Json& page_index, const Json& skeleton, const std::string& agent, bool replace) {
    const auto at = page_at(work.doc, page_index);
    if (!at) throw layout::LayoutError("/page", core::py_str(page_index) + " ページはない（1〜" + std::to_string(work.doc.pages.size()) + "）");
    if (agent.rfind("ai:", 0) == 0 && work.doc.page(*at).name_ok) {
        throw layout::LayoutError("/page", core::py_str(page_index) + " ページのネームは承認済み。AI はコマ割りを変えない（変えるには人間が承認を取り消す）");
    }
    if (!layout::is_blank(work.doc, work.doc.page(*at))) {
        if (!replace) throw layout::LayoutError("/page", core::py_str(page_index) + " ページにはすでにコマか台詞がある（replace で作り直す）");
        layout::clear_page(work, *at, agent);
    }
    Json plan = Json::object({{"page", page_index}, {"tiers", core::py_get(skeleton, "tiers")}});
    for (const char* key : {"insets", "tier_gap_mm", "col_gap_mm"}) {
        if (!core::py_get(skeleton, key).is_null()) plan[key] = skeleton[key];
    }
    plan["panels"] = core::py_or(core::py_get(skeleton, "panels"), Json::array());
    return layout::apply_layout(work, page_index, plan, agent);
}

}  // namespace genko::service::layout_ideas
