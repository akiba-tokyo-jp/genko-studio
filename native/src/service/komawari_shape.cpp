#include "service/komawari_shape.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/frames.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/book.hpp"
#include "service/layout.hpp"
#include "service/lint.hpp"
#include "service/pyfmt.hpp"
#include "service/result.hpp"

namespace genko::service::komawari {

namespace {

// The default gaps apply_layout uses (layout.hpp).
constexpr double kTierGap = layout::kTierGutterMm;
constexpr double kColGap = layout::kColGutterMm;

std::optional<double> number_at(const Json& entry, const char* key) {
    const Json value = core::py_get(entry, key);
    if (value.is_null() || !value.is_number()) return std::nullopt;
    return core::to_float(value);
}

double plan_gap(const Json& plan, const char* key, double fallback) {
    const auto value = number_at(plan, key);
    return value ? *value : fallback;
}

// The slot of a tier or column that is one panel (layout.cpp's leaf: whose border carries that panel's slant).
std::optional<std::string> leaf_slot(const Json& part) {
    const Json cols = core::py_get(part, "cols");
    if (!cols.is_null()) return core::length(cols) == 1 ? leaf_slot(core::subscript(cols, 0)) : std::nullopt;
    if (core::truthy_at(part, "rows")) return std::nullopt;
    const Json slot = core::py_get(part, "slot");
    if (slot.is_null()) return std::nullopt;
    return core::py_str(slot);
}

std::vector<double> ratios(const std::vector<Json>& items, const char* key) {
    std::vector<double> out;
    for (const Json& item : items) {
        const auto value = number_at(item, key);
        out.push_back(value ? std::max(0.0, *value) : 0.0);
    }
    return out;
}

// The spans of n parts over `extent` less their gaps, as apply_layout lays them.
std::vector<double> spans(const std::vector<double>& weights, const std::vector<double>& gaps, double extent) {
    double total = 0;
    for (const double w : weights) total += w;
    if (total <= 0) total = 1;
    double usable = extent;
    for (const double g : gaps) usable -= g;
    std::vector<double> out;
    for (const double w : weights) out.push_back(usable * w / total);
    return out;
}

// core::reading_order on boxes (rows from the top, those whose middles are within the smaller one's half height share
// a row, right to left in a row): the order of `items` (indices into `boxes`).
std::vector<std::size_t> read_drawn(const std::vector<PanelBox>& boxes, std::vector<std::size_t> items) {
    std::stable_sort(items.begin(), items.end(), [&boxes](std::size_t a, std::size_t b) {
        return boxes[a].y + boxes[a].h / 2 < boxes[b].y + boxes[b].h / 2;
    });
    std::vector<std::size_t> out;
    while (!items.empty()) {
        const PanelBox& first = boxes[items.front()];
        const double fcy = first.y + first.h / 2;
        std::vector<std::size_t> row, left;
        for (const std::size_t i : items) {
            const PanelBox& c = boxes[i];
            if (std::fabs(c.y + c.h / 2 - fcy) < std::min(c.h, first.h) / 2) {
                row.push_back(i);
            } else {
                left.push_back(i);
            }
        }
        if (row.empty()) {
            row.push_back(items.front());
            left.erase(left.begin());
        }
        std::stable_sort(row.begin(), row.end(), [&boxes](std::size_t a, std::size_t b) {
            return boxes[a].x + boxes[a].w / 2 > boxes[b].x + boxes[b].w / 2;
        });
        out.insert(out.end(), row.begin(), row.end());
        items = std::move(left);
    }
    return out;
}

void mark_border(std::vector<PanelBox>& panels, std::size_t a, std::size_t b, double tilt, bool across_height) {
    if (std::fabs(tilt) <= 1e-6) return;
    for (const std::size_t i : {a, b}) {
        (across_height ? panels[i].squeeze_h : panels[i].squeeze_w) += std::fabs(tilt) / 2;
        panels[i].slanted = true;
    }
}

}  // namespace

double PanelBox::min_side() const { return std::min(w - squeeze_w, h - squeeze_h); }

std::pair<double, double> frame_size(const core::PageSpec& spec) { return spec.frame_size(); }

Shape shape_of_plan(const Json& plan, double frame_w, double frame_h) {
    const Json tiers = layout::resolve_tiers(plan);
    Json template_shape = Json::object();
    if (!core::truthy_at(plan, "tiers")) {
        const Json name = core::py_get(plan, "template");
        if (name.is_string() && layout::templates().contains(name.get<std::string>())) template_shape = layout::templates()[name.get<std::string>()];
    }
    // the slants and bleeds as apply_layout takes them: the template's, the plan's panels winning
    std::vector<std::pair<std::string, double>> slants;
    const auto set_slant = [&slants](const std::string& key, double value) {
        for (auto& [k, v] : slants) {
            if (k == key) {
                v = value;
                return;
            }
        }
        slants.emplace_back(key, value);
    };
    const auto slant_of = [&slants](const std::optional<std::string>& key) {
        if (!key) return 0.0;
        for (const auto& [k, v] : slants) {
            if (k == *key) return v;
        }
        return 0.0;
    };
    std::set<std::string> bleeds, frameless, overflow;
    if (const Json given = core::py_get(template_shape, "slant"); given.is_object()) {
        for (const auto& [key, value] : given.items()) {
            if (value.is_number()) set_slant(key, core::to_float(value));
        }
    }
    for (const Json& slot : core::iterate(core::py_or(core::py_get(template_shape, "bleed"), Json::array()))) bleeds.insert(core::py_str(slot));
    for (const Json& panel : core::iterate(core::py_or(core::py_get(plan, "panels"), Json::array()))) {
        if (!panel.is_object()) continue;
        const std::string slot = core::py_str(core::py_get(panel, "slot"));
        const Json bleed = core::py_get(panel, "bleed");
        if (!bleed.is_null()) {
            if (core::py_truthy(bleed)) {
                bleeds.insert(slot);
            } else {
                bleeds.erase(slot);
            }
        }
        if (const auto slant = number_at(panel, "slant")) set_slant(slot, *slant);
        if (const auto border = number_at(panel, "border_mm"); border && *border <= 0) frameless.insert(slot);
        if (core::py_truthy(core::py_get(panel, "overflow"))) overflow.insert(slot);
    }
    const double tier_gap = plan_gap(plan, "tier_gap_mm", kTierGap);
    const double col_gap = plan_gap(plan, "col_gap_mm", kColGap);

    Shape out;
    out.frame_w = frame_w;
    out.frame_h = frame_h;
    const std::vector<Json> tier_list = core::iterate(tiers);
    std::vector<double> tier_gaps, tier_tilts;
    for (std::size_t t = 0; t + 1 < tier_list.size(); ++t) {
        const auto own = number_at(tier_list[t], "gap_mm");
        tier_gaps.push_back(own ? *own : tier_gap);
        const auto tilt = number_at(tier_list[t], "tilt_mm");
        tier_tilts.push_back(tilt ? *tilt : slant_of(leaf_slot(tier_list[t])));
    }
    const std::vector<double> heights = spans(ratios(tier_list, "h"), tier_gaps, frame_h);
    std::vector<std::vector<std::size_t>> tier_panels;  // each tier's panels (indices into out.panels)
    double y = 0;
    for (std::size_t t = 0; t < tier_list.size(); ++t) {
        const std::vector<Json> cols = core::iterate(core::py_or(core::py_get(tier_list[t], "cols"), Json::array()));
        std::vector<double> col_gaps, col_tilts;
        for (std::size_t c = 0; c + 1 < cols.size(); ++c) {
            const auto own = number_at(cols[c], "gap_mm");
            col_gaps.push_back(own ? *own : col_gap);
            const auto tilt = number_at(cols[c], "tilt_mm");
            col_tilts.push_back(tilt ? *tilt : slant_of(leaf_slot(cols[c])));
        }
        const std::vector<double> widths = spans(ratios(cols, "w"), col_gaps, frame_w);
        std::vector<std::size_t> here;
        std::vector<std::vector<std::size_t>> col_panels;
        bool stacked = false;
        double right = frame_w;
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const double x = right - widths[c];
            std::vector<std::size_t> in_col;
            const std::vector<Json> rows = core::iterate(core::py_or(core::py_get(cols[c], "rows"), Json::array()));
            if (!rows.empty()) {
                stacked = true;
                std::vector<double> row_gaps, row_tilts;
                for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
                    const auto own = number_at(rows[r], "gap_mm");
                    row_gaps.push_back(own ? *own : tier_gap);
                    const auto tilt = number_at(rows[r], "tilt_mm");
                    const Json slot = core::py_get(rows[r], "slot");
                    row_tilts.push_back(tilt ? *tilt : slant_of(core::py_truthy(slot) ? std::optional<std::string>(core::py_str(slot)) : std::nullopt));
                }
                const std::vector<double> row_h = spans(ratios(rows, "h"), row_gaps, t < heights.size() ? heights[t] : 0);
                double ry = y;
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    PanelBox box;
                    box.slot = core::py_str(core::py_get(rows[r], "slot"));
                    box.x = x;
                    box.y = ry;
                    box.w = widths[c];
                    box.h = row_h[r];
                    box.tier = static_cast<int>(t);
                    box.col = static_cast<int>(c);
                    in_col.push_back(out.panels.size());
                    out.panels.push_back(box);
                    ry += row_h[r] + (r < row_gaps.size() ? row_gaps[r] : 0);
                }
                for (std::size_t r = 0; r + 1 < rows.size(); ++r) mark_border(out.panels, in_col[r], in_col[r + 1], row_tilts[r], true);
                out.gaps.insert(out.gaps.end(), row_gaps.begin(), row_gaps.end());
                for (std::size_t r = 0; r < row_tilts.size(); ++r) {
                    out.tilts += std::fabs(row_tilts[r]) > 1e-6 ? 1 : 0;
                    out.steepest = std::max(out.steepest, std::fabs(row_tilts[r]));
                }
            } else {
                PanelBox box;
                box.slot = core::py_str(core::py_get(cols[c], "slot"));
                box.x = x;
                box.y = y;
                box.w = widths[c];
                box.h = t < heights.size() ? heights[t] : 0;
                box.tier = static_cast<int>(t);
                box.col = static_cast<int>(c);
                in_col.push_back(out.panels.size());
                out.panels.push_back(box);
            }
            here.insert(here.end(), in_col.begin(), in_col.end());
            col_panels.push_back(in_col);
            right = x - (c < col_gaps.size() ? col_gaps[c] : 0);
        }
        for (std::size_t c = 0; c + 1 < col_panels.size(); ++c) {
            for (const std::size_t a : col_panels[c]) {
                for (const std::size_t b : col_panels[c + 1]) mark_border(out.panels, a, b, col_tilts[c], false);
            }
            out.tilts += std::fabs(col_tilts[c]) > 1e-6 ? 1 : 0;
            out.steepest = std::max(out.steepest, std::fabs(col_tilts[c]));
        }
        out.gaps.insert(out.gaps.end(), col_gaps.begin(), col_gaps.end());
        out.pattern += (out.pattern.empty() ? "" : "-") + std::to_string(here.size()) + (stacked ? "s" : "");
        tier_panels.push_back(here);
        y += (t < heights.size() ? heights[t] : 0) + (t < tier_gaps.size() ? tier_gaps[t] : 0);
    }
    for (std::size_t t = 0; t + 1 < tier_panels.size(); ++t) {
        for (const std::size_t a : tier_panels[t]) {
            for (const std::size_t b : tier_panels[t + 1]) mark_border(out.panels, a, b, tier_tilts[t], true);
        }
        out.tilts += std::fabs(tier_tilts[t]) > 1e-6 ? 1 : 0;
        out.steepest = std::max(out.steepest, std::fabs(tier_tilts[t]));
    }
    out.gaps.insert(out.gaps.end(), tier_gaps.begin(), tier_gaps.end());
    // the insets, each read beside the panel it goes into (by where they are, as drawn panels are)
    std::vector<PanelBox> boxes = out.panels;
    std::vector<std::vector<std::size_t>> with(boxes.size());
    for (const Json& inset : core::iterate(core::py_or(core::py_get(plan, "insets"), Json::array()))) {
        if (!inset.is_object()) continue;
        const std::string host = core::py_str(core::py_get(inset, "on"));
        const auto at = std::find_if(boxes.begin(), boxes.begin() + static_cast<std::ptrdiff_t>(out.panels.size()),
                                     [&host](const PanelBox& b) { return b.slot == host; });
        const Json rect = core::py_get(inset, "rect");
        if (at == boxes.begin() + static_cast<std::ptrdiff_t>(out.panels.size()) || !rect.is_array() || rect.size() != 4 ||
            !std::all_of(rect.begin(), rect.end(), [](const Json& v) { return v.is_number(); })) {
            continue;
        }
        PanelBox box;
        box.slot = core::py_str(core::py_get(inset, "slot"));
        box.x = core::to_float(rect[0]) * frame_w;
        box.y = core::to_float(rect[1]) * frame_h;
        box.w = core::to_float(rect[2]) * frame_w;
        box.h = core::to_float(rect[3]) * frame_h;
        box.tier = at->tier;
        box.col = at->col;
        box.over = true;
        box.host = host;
        with[static_cast<std::size_t>(at - boxes.begin())].push_back(boxes.size());
        boxes.push_back(box);
    }
    std::vector<PanelBox> ordered;
    for (std::size_t i = 0; i < out.panels.size(); ++i) {
        std::vector<std::size_t> group{i};
        group.insert(group.end(), with[i].begin(), with[i].end());
        for (const std::size_t k : group.size() > 1 ? read_drawn(boxes, group) : group) ordered.push_back(boxes[k]);
    }
    for (PanelBox& box : ordered) {
        box.bleed = bleeds.count(box.slot) > 0;
        box.frameless = frameless.count(box.slot) > 0;
        box.overflow = overflow.count(box.slot) > 0;
    }
    out.panels = std::move(ordered);
    return out;
}

std::vector<std::string> reading_slots(const Json& plan, double frame_w_mm, double frame_h_mm) {
    Shape shape;
    try {
        shape = shape_of_plan(plan, frame_w_mm, frame_h_mm);
    } catch (const layout::LayoutError&) {
        throw;
    } catch (const std::exception&) {  // (a plan its checks refuse: its tiers' order, as before insets)
        return layout::slots_in_order(layout::resolve_tiers(plan));
    }
    std::vector<std::string> out;
    for (const PanelBox& box : shape.panels) out.push_back(box.slot);
    return out;
}

// --- the measures -------------------------------------------------------------------------------------------------------

double size_ratio(const Shape& shape) {
    if (shape.panels.size() < 2) return 1;
    double big = 0, small = -1;
    for (const PanelBox& box : shape.panels) {
        const double a = std::max(box.area(), 1e-9);
        big = std::max(big, a);
        small = small < 0 ? a : std::min(small, a);
    }
    return big / small;
}

bool plain_grid(const Shape& shape) {
    if (shape.panels.size() < 2 || shape.tilts > 0) return false;
    for (const PanelBox& box : shape.panels) {
        if (box.slanted || box.bleed || box.over || box.frameless || box.overflow) return false;
    }
    return size_ratio(shape) < kPlainSizeRatio;
}

bool flat(const Shape& shape) { return shape.panels.size() >= 3 && size_ratio(shape) < kFlatSizeRatio; }

bool insets_on_hosts(const Shape& shape) {
    for (const PanelBox& inset : shape.panels) {
        if (inset.host.empty()) continue;
        const auto host = std::find_if(shape.panels.begin(), shape.panels.end(), [&inset](const PanelBox& b) { return b.slot == inset.host; });
        if (host == shape.panels.end()) return false;
        const double w = std::min(inset.x + inset.w, host->x + host->w) - std::max(inset.x, host->x);
        const double h = std::min(inset.y + inset.h, host->y + host->h) - std::max(inset.y, host->y);
        if (w <= 0 || h <= 0 || w * h < inset.area() / 4) return false;
    }
    return true;
}

std::size_t reading_break(const Shape& shape) {
    constexpr double kSlack = 1.0;  // mm
    for (std::size_t i = 1; i < shape.panels.size(); ++i) {
        const PanelBox& a = shape.panels[i - 1];
        const PanelBox& b = shape.panels[i];
        const bool above = b.y + b.h <= a.y + kSlack;  // the next one wholly above: the eye goes back up
        const double overlap = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
        const bool same_row = overlap > std::min(a.h, b.h) / 2;
        const bool right = b.x >= a.x + a.w - kSlack;  // in the same row, wholly to the right: read back against the flow
        if (above || (same_row && right)) return i + 1;
    }
    return 0;
}

double smallest_side(const Shape& shape) {
    double out = -1;
    for (const PanelBox& box : shape.panels) out = out < 0 ? box.min_side() : std::min(out, box.min_side());
    return out < 0 ? 0 : out;
}

double narrowest_gap(const Shape& shape) {
    double out = 1e9;
    for (const double g : shape.gaps) out = std::min(out, g);
    return out;
}

std::size_t largest(const Shape& shape) {
    std::size_t out = 0;
    double best = -1;
    for (std::size_t i = 0; i < shape.panels.size(); ++i) {
        if (shape.panels[i].area() > best + 1e-9) {
            best = shape.panels[i].area();
            out = i + 1;
        }
    }
    return out;
}

double share(const Shape& shape, std::size_t index) {
    if (index == 0 || index > shape.panels.size() || shape.frame_w <= 0 || shape.frame_h <= 0) return 0;
    return shape.panels[index - 1].area() / (shape.frame_w * shape.frame_h);
}

namespace {

// Where the 大ゴマ is: its tier and column (from the right), or none when no panel holds kBigShare of the frame.
std::pair<int, int> big_place(const Shape& shape) {
    const std::size_t at = largest(shape);
    if (at == 0 || share(shape, at) < kBigShare) return {-1, -1};
    const PanelBox& box = shape.panels[at - 1];
    if (shape.drawn) return {static_cast<int>(std::floor(3 * (box.y + box.h / 2) / std::max(1.0, shape.frame_h))), 0};
    return {box.tier, box.over ? -2 : box.col};
}

bool any_of(const Shape& shape, bool PanelBox::*flag) {
    return std::any_of(shape.panels.begin(), shape.panels.end(), [flag](const PanelBox& b) { return b.*flag; });
}

}  // namespace

int difference(const Shape& a, const Shape& b) {
    int d = 0;
    if (a.pattern != b.pattern || a.count() != b.count()) d += 3;
    if (big_place(a) != big_place(b)) d += 2;
    if ((a.tilts > 0) != (b.tilts > 0)) {
        d += 2;
    } else if (a.tilts != b.tilts) {
        d += 1;
    }
    if (any_of(a, &PanelBox::bleed) != any_of(b, &PanelBox::bleed)) d += 1;
    if (any_of(a, &PanelBox::over) != any_of(b, &PanelBox::over)) d += 2;
    if (any_of(a, &PanelBox::frameless) != any_of(b, &PanelBox::frameless) || any_of(a, &PanelBox::overflow) != any_of(b, &PanelBox::overflow)) d += 1;
    return d;
}

std::string describe(const Shape& shape) {
    std::string out = std::to_string(shape.count()) + "コマ・段 " + shape.pattern;
    const auto [tier, col] = big_place(shape);
    if (tier >= 0) out += "・大ゴマ " + shape.panels[largest(shape) - 1].slot;
    if (shape.tilts > 0) out += "・斜め " + std::to_string(shape.tilts);
    int bleeds = 0, overs = 0, frameless = 0, overflows = 0;
    for (const PanelBox& b : shape.panels) {
        bleeds += b.bleed ? 1 : 0;
        overs += b.over ? 1 : 0;
        frameless += b.frameless ? 1 : 0;
        overflows += b.overflow ? 1 : 0;
    }
    if (bleeds > 0) out += "・断ち切り " + std::to_string(bleeds);
    if (overs > 0) out += "・重ね " + std::to_string(overs);
    if (overflows > 0) out += "・はみ出し " + std::to_string(overflows);
    if (frameless > 0) out += "・枠なし " + std::to_string(frameless);
    return out;
}

namespace {

// Each tier's panels and whether a column of it is stacked, from the pattern ("2s-1" → {2, true}, {1, false}).
std::vector<std::pair<int, bool>> tiers_of(const Shape& shape) {
    std::vector<std::pair<int, bool>> out;
    if (shape.drawn) return out;
    std::size_t at = 0;
    while (at < shape.pattern.size()) {
        std::size_t end = shape.pattern.find('-', at);
        if (end == std::string::npos) end = shape.pattern.size();
        const std::string part = shape.pattern.substr(at, end - at);
        const bool stacked = !part.empty() && part.back() == 's';
        try {
            out.emplace_back(std::stoi(stacked ? part.substr(0, part.size() - 1) : part), stacked);
        } catch (const std::exception&) {
            return {};
        }
        at = end + 1;
    }
    return out;
}

// A 大ゴマ that stands out: kAccentBigShare of the frame, and kAccentBigOver times the next largest panel.
bool standing_big(const Shape& shape) {
    const std::size_t at = largest(shape);
    if (at == 0 || shape.count() < 2 || share(shape, at) < kAccentBigShare) return false;  // (one panel alone is a blank page)
    double next = 0;
    for (std::size_t i = 0; i < shape.panels.size(); ++i) {
        if (i + 1 != at) next = std::max(next, shape.panels[i].area());
    }
    return next <= 0 || shape.panels[at - 1].area() >= kAccentBigOver * next;
}

// Where a panel is on the page, in words: 右上, 上, 中ほど, 左下 …
std::string place_words(const Shape& shape, const PanelBox& box) {
    const double fw = std::max(1.0, shape.frame_w), fh = std::max(1.0, shape.frame_h);
    const double cy = (box.y + box.h / 2) / fh, cx = (box.x + box.w / 2) / fw;
    const std::string v = cy < 1.0 / 3 ? "上" : cy > 2.0 / 3 ? "下" : "";
    const std::string h = box.w > fw * 0.66 ? "" : cx > 2.0 / 3 ? "右" : cx < 1.0 / 3 ? "左" : "";
    if (v.empty() && h.empty()) return "中ほど";
    return h + v;
}

}  // namespace

std::string describe_for_people(const Shape& shape) {
    std::string out = std::to_string(shape.count()) + " コマ";
    const auto tiers = tiers_of(shape);
    if (shape.drawn) {
        out += "（描いたコマ）";
    } else if (!tiers.empty()) {
        out += "・" + std::to_string(tiers.size()) + " 段";
        if (tiers.size() > 1) {
            out += "（上から ";
            for (std::size_t i = 0; i < tiers.size(); ++i) out += (i > 0 ? "・" : "") + std::to_string(tiers[i].first);
            out += "）";
        }
        if (std::any_of(tiers.begin(), tiers.end(), [](const auto& t) { return t.second; })) out += "・縦積み";
    }
    const std::size_t big = largest(shape);
    if (big > 0 && shape.count() > 1 && share(shape, big) >= kBigShare) out += "・大ゴマは" + place_words(shape, shape.panels[big - 1]);
    if (shape.tilts > 0) out += shape.steepest >= kAccentTiltMm ? "・深い斜め" : "・斜め";
    if (any_of(shape, &PanelBox::bleed)) out += "・断ち切り";
    if (any_of(shape, &PanelBox::over)) out += "・差し込み";
    if (any_of(shape, &PanelBox::frameless)) out += "・枠なし";
    if (any_of(shape, &PanelBox::overflow)) out += "・はみ出しあり";
    return out;
}

int accents(const Shape& shape) {
    int n = 0;
    n += shape.steepest >= kAccentTiltMm - 1e-9 ? 1 : 0;
    n += any_of(shape, &PanelBox::bleed) ? 1 : 0;
    n += any_of(shape, &PanelBox::over) ? 1 : 0;
    n += standing_big(shape) ? 1 : 0;
    const auto tiers = tiers_of(shape);
    n += std::any_of(tiers.begin(), tiers.end(), [](const auto& t) { return t.second; }) ? 1 : 0;
    double widest = 0;
    for (const double g : shape.gaps) widest = std::max(widest, g);
    n += widest >= kAccentGapMm - 1e-9 ? 1 : 0;
    n += any_of(shape, &PanelBox::frameless) ? 1 : 0;
    return n;
}

bool strip_stack(const Shape& shape) {
    if (shape.count() < 4 || shape.tilts > 0 || shape.drawn) return false;
    const auto tiers = tiers_of(shape);
    if (tiers.empty()) return false;
    // (the tiers alone are strips: an inset over one does not make it another layout)
    return std::all_of(tiers.begin(), tiers.end(), [](const auto& t) { return t.first == 1 && !t.second; });
}

bool look_alike(const Shape& a, const Shape& b, bool exact) {
    if (a.count() != b.count() || a.drawn != b.drawn) return false;
    if (exact) {
        if (a.pattern != b.pattern) return false;
    } else {
        auto ta = tiers_of(a), tb = tiers_of(b);
        std::sort(ta.begin(), ta.end());
        std::sort(tb.begin(), tb.end());
        if (ta != tb) return false;
    }
    const auto deep = [](const Shape& s) { return s.tilts == 0 ? 0 : s.steepest >= kAccentTiltMm ? 2 : 1; };
    return big_place(a) == big_place(b) && deep(a) == deep(b) && any_of(a, &PanelBox::bleed) == any_of(b, &PanelBox::bleed) &&
           any_of(a, &PanelBox::over) == any_of(b, &PanelBox::over) && any_of(a, &PanelBox::frameless) == any_of(b, &PanelBox::frameless);
}

// --- the page's own panels ----------------------------------------------------------------------------------------------

namespace {

struct Border {
    double gap = 0;
    double tilt = 0;
    std::optional<double> mid;  // where the cut's middle is along the axis (mm), when the node keeps its cut
};

// A node's parts along `axis` (a chain of splits the same way), first to last by position, with the borders between.
bool flatten(const core::Frame& node, const std::string& axis, std::vector<const core::Frame*>& parts, std::vector<Border>& borders) {
    const bool along = node.split_axis && *node.split_axis == axis && !node.children.empty();
    if (!along) {
        parts.push_back(&node);
        return true;
    }
    const bool horizontal = axis == "horizontal";
    std::vector<const core::Frame*> kids;
    for (const core::Frame& child : node.children) kids.push_back(&child);
    std::stable_sort(kids.begin(), kids.end(), [horizontal](const core::Frame* a, const core::Frame* b) {
        const auto ca = core::centroid(core::shape(*a));
        const auto cb = core::centroid(core::shape(*b));
        return horizontal ? ca.second < cb.second : ca.first < cb.first;
    });
    const auto line = core::cut_line(node);
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (i > 0) {
            Border border;
            if (line && kids.size() == 2) {
                core::Point p0 = line->p0, p1 = line->p1;
                if (horizontal ? p1.x < p0.x : p1.y < p0.y) std::swap(p0, p1);
                border.gap = line->gutter;
                border.tilt = horizontal ? (p1.y - p0.y).value() : (p1.x - p0.x).value();
                border.mid = horizontal ? (p0.y.value() + p1.y.value()) / 2 : (p0.x.value() + p1.x.value()) / 2;
            } else {
                const core::Rect& a = kids[i - 1]->rect;
                const core::Rect& b = kids[i]->rect;
                border.gap = horizontal ? (b.y - (a.y + a.height)).value() : (b.x - (a.x + a.width)).value();
            }
            borders.push_back(border);
        }
        if (!flatten(*kids[i], axis, parts, borders)) return false;
    }
    return true;
}

// The extents of parts along an axis: from the node's start to the first border's middle less half its gap, and so on.
std::vector<double> extents(const std::vector<const core::Frame*>& parts, const std::vector<Border>& borders, const core::Rect& box,
                            bool horizontal) {
    std::vector<double> mids;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const core::Rect& a = parts[i]->rect;
        const core::Rect& b = parts[i + 1]->rect;
        // the border's middle: halfway between the parts' boxes as they meet (their middles where slanted)
        const double end_a = horizontal ? (a.y + a.height).value() : (a.x + a.width).value();
        const double start_b = horizontal ? b.y.value() : b.x.value();
        mids.push_back(borders[i].mid ? *borders[i].mid : (end_a + start_b) / 2);
    }
    std::vector<double> out;
    const double begin = horizontal ? box.y.value() : box.x.value();
    const double end = horizontal ? (box.y + box.height).value() : (box.x + box.width).value();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const double s = i == 0 ? begin : mids[i - 1] + borders[i - 1].gap / 2;
        const double e = i + 1 == parts.size() ? end : mids[i] - borders[i].gap / 2;
        out.push_back(std::max(0.0, e - s));
    }
    return out;
}

Json ratio_json(double part, double total) { return core::py_round(total > 0 ? part / total : 0, 3); }

void put_border(Json& entry, const Border& border, double fallback_gap) {
    if (std::fabs(border.gap - fallback_gap) > 0.05) entry["gap_mm"] = core::py_round(border.gap, 1);
    if (std::fabs(border.tilt) > 0.05) entry["tilt_mm"] = core::py_round(border.tilt, 1);
}

}  // namespace

std::optional<Json> skeleton_of_page(const core::Document& doc, const core::Page& page) {
    (void)doc;
    if (page.frames.empty()) return std::nullopt;
    const core::Frame& root = page.frames.front();
    // (a root of drawn panels is one only when it is a panel with its insets: a page of one tier, one panel)
    // (drawn panels, and one drawn panel alone, are not tiers; nor a node not over the basic frame)
    if (core::is_free(root)) {
        const auto under = std::count_if(root.children.begin(), root.children.end(), [](const core::Frame& f) { return !f.over && f.children.empty(); });
        const auto over = std::count_if(root.children.begin(), root.children.end(), [](const core::Frame& f) { return f.over && f.children.empty(); });
        const core::Rect inner = page.inner_rect_mm(doc.start_side ? std::string_view(*doc.start_side) : std::string_view());
        const bool on_frame = std::fabs((root.rect.x - inner.x).value()) < 0.01 && std::fabs((root.rect.y - inner.y).value()) < 0.01 &&
                              std::fabs((root.rect.width - inner.width).value()) < 0.01 && std::fabs((root.rect.height - inner.height).value()) < 0.01;
        if (under != 1 || over < 1 || under + over != static_cast<std::ptrdiff_t>(root.children.size()) || !on_frame) return std::nullopt;
    }
    const core::Rect frame = root.rect;
    if (!(frame.width.value() > 0 && frame.height.value() > 0)) return std::nullopt;
    // the slots: p1… in the page's reading order
    std::vector<std::pair<std::string, std::string>> slot_of;  // frame id → slot
    for (const core::Frame* leaf : page.leaf_frames()) slot_of.emplace_back(leaf->id, "p" + std::to_string(slot_of.size() + 1));
    const auto slot = [&slot_of](const core::Frame& f) {
        for (const auto& [id, s] : slot_of) {
            if (id == f.id) return s;
        }
        return std::string();
    };
    Json insets = Json::array();
    Json panels = Json::array();
    // a panel, or a node of drawn panels holding one panel and the insets over it: the panel's slot
    const auto host_slot = [&](const core::Frame& node) -> std::optional<std::string> {
        if (node.children.empty()) return slot(node);
        if (!core::is_free(node)) return std::nullopt;
        const core::Frame* host = nullptr;
        for (const core::Frame& child : node.children) {
            if (!child.children.empty()) return std::nullopt;
            if (!child.over) {
                if (host != nullptr) return std::nullopt;
                host = &child;
            }
        }
        if (host == nullptr) return std::nullopt;
        for (const core::Frame& child : node.children) {
            if (!child.over) continue;
            const core::Rect& r = child.rect;
            insets.push_back(Json::object({{"slot", slot(child)},
                                           {"on", slot(*host)},
                                           {"rect", Json::array({core::py_round((r.x - frame.x).value() / frame.width.value(), 3),
                                                                 core::py_round((r.y - frame.y).value() / frame.height.value(), 3),
                                                                 core::py_round(r.width.value() / frame.width.value(), 3),
                                                                 core::py_round(r.height.value() / frame.height.value(), 3)})}}));
        }
        return slot(*host);
    };
    std::vector<const core::Frame*> tier_parts;
    std::vector<Border> tier_borders;
    flatten(root, "horizontal", tier_parts, tier_borders);
    const std::vector<double> tier_ext = extents(tier_parts, tier_borders, frame, true);
    double tier_total = 0;
    for (const double e : tier_ext) tier_total += e;
    Json tiers = Json::array();
    for (std::size_t t = 0; t < tier_parts.size(); ++t) {
        std::vector<const core::Frame*> col_parts;
        std::vector<Border> col_borders;
        flatten(*tier_parts[t], "vertical", col_parts, col_borders);
        const std::vector<double> col_ext = extents(col_parts, col_borders, tier_parts[t]->rect, false);
        double col_total = 0;
        for (const double e : col_ext) col_total += e;
        Json cols = Json::array();
        // right to left: the columns as they are read
        for (std::size_t k = col_parts.size(); k-- > 0;) {
            const core::Frame& col = *col_parts[k];
            Json entry = Json::object({{"slot", ""}, {"w", ratio_json(col_ext[k], col_total)}});
            if (const auto s = host_slot(col)) {
                entry["slot"] = *s;
            } else {
                std::vector<const core::Frame*> row_parts;
                std::vector<Border> row_borders;
                flatten(col, "horizontal", row_parts, row_borders);
                if (row_parts.size() < 2) return std::nullopt;
                const std::vector<double> row_ext = extents(row_parts, row_borders, col.rect, true);
                double row_total = 0;
                for (const double e : row_ext) row_total += e;
                Json rows = Json::array();
                for (std::size_t r = 0; r < row_parts.size(); ++r) {
                    const auto row_slot = host_slot(*row_parts[r]);
                    if (!row_slot) return std::nullopt;
                    Json row = Json::object({{"slot", *row_slot}, {"h", ratio_json(row_ext[r], row_total)}});
                    if (r + 1 < row_parts.size()) put_border(row, row_borders[r], kTierGap);
                    rows.push_back(std::move(row));
                }
                entry["slot"] = "c" + std::to_string(t + 1) + "_" + std::to_string(cols.size() + 1);
                entry["rows"] = std::move(rows);
            }
            // (the border after this column, read right to left, is the one on its left)
            if (k > 0) put_border(entry, col_borders[k - 1], kColGap);
            cols.push_back(std::move(entry));
        }
        Json tier = Json::object({{"h", ratio_json(tier_ext[t], tier_total)}, {"cols", std::move(cols)}});
        if (t + 1 < tier_parts.size()) put_border(tier, tier_borders[t], kTierGap);
        tiers.push_back(std::move(tier));
    }
    for (const core::Frame* leaf : page.leaf_frames()) {
        Json panel = Json::object({{"slot", slot(*leaf)}});
        if (leaf->bleed) panel["bleed"] = true;
        if (!leaf->clip) panel["overflow"] = true;
        if (std::fabs(leaf->border_mm - 0.8) > 1e-6) panel["border_mm"] = leaf->border_mm;
        panels.push_back(std::move(panel));
    }
    Json out = Json::object({{"tiers", std::move(tiers)}});
    if (!insets.empty()) out["insets"] = std::move(insets);
    out["panels"] = std::move(panels);
    return out;
}

std::optional<Shape> shape_of_page(const core::Document& doc, const core::Page& page) {
    const auto leaves = page.leaf_frames();
    const bool planned = page.plan && core::py_truthy(*page.plan);
    if (leaves.empty() || (leaves.size() == 1 && !planned && leaves.front()->border_mm > 0 && !leaves.front()->bleed)) return std::nullopt;
    const auto [fw, fh] = frame_size(page.spec);
    if (const auto skeleton = skeleton_of_page(doc, page)) {
        try {
            Shape shape = shape_of_plan(*skeleton, fw, fh);
            // (the real frames' slants: a hand-cut panel's own shape counts)
            for (std::size_t i = 0; i < shape.panels.size() && i < leaves.size(); ++i) {
                if (leaves[i]->poly && !leaves[i]->poly->empty() && !shape.panels[i].slanted) {
                    shape.panels[i].slanted = true;
                    ++shape.tilts;
                }
            }
            return shape;
        } catch (const std::exception&) {
        }
    }
    Shape shape;
    shape.drawn = true;
    shape.pattern = "free";
    shape.frame_w = fw;
    shape.frame_h = fh;
    const core::Rect inner = page.inner_rect_mm(doc.start_side ? std::string_view(*doc.start_side) : std::string_view());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const core::Frame& f = *leaves[i];
        PanelBox box;
        box.slot = "p" + std::to_string(i + 1);
        box.x = (f.rect.x - inner.x).value();
        box.y = (f.rect.y - inner.y).value();
        box.w = f.rect.width.value();
        box.h = f.rect.height.value();
        box.slanted = f.poly && !f.poly->empty();
        box.bleed = f.bleed;
        box.over = f.over;
        box.frameless = f.border_mm <= 0;
        box.overflow = !f.clip;
        shape.tilts += box.slanted ? 1 : 0;
        shape.panels.push_back(box);
    }
    return shape;
}

// --- 単調さの警告（C） -----------------------------------------------------------------------------------------------

namespace {

std::set<std::string> reveal_beats(const Json& script) {
    std::set<std::string> out;
    for (const Json& scene : core::iterate(core::py_or(core::py_get(script, "scenes"), Json::array()))) {
        for (const Json& beat : core::iterate(core::py_or(core::py_get(scene, "beats"), Json::array()))) {
            if (core::truthy_at(beat, "reveal") && core::py_get(beat, "id").is_string()) out.insert(core::py_get(beat, "id").get<std::string>());
        }
    }
    return out;
}


void flat_check(std::vector<studio::Issue>& out, const Shape& shape, const std::string& path) {
    if (flat(shape)) {
        const double ratio = size_ratio(shape);
        out.push_back(warning("layout_flat", path, "コマの大きさの差が小さい（いちばん大きいコマが、いちばん小さいコマの " + fixed(ratio, 1) + " 倍）",
                              "見せたいコマを大きくし、ほかを小さくして緩急を付ける（大ゴマ・断ち切り・コマの数の変化）"));
    }
}

void reveal_check(std::vector<studio::Issue>& out, const Shape& shape, const Json& plan, const std::set<std::string>& reveals) {
    if (reveals.empty()) return;
    const std::size_t big = largest(shape);
    const std::vector<Json> panels = core::iterate(core::py_or(core::py_get(plan, "panels"), Json::array()));
    for (std::size_t pi = 0; pi < panels.size(); ++pi) {
        std::string beat;
        for (const Json& bid : core::iterate(core::py_or(core::py_get(panels[pi], "beat_ids"), Json::array()))) {
            if (bid.is_string() && reveals.count(bid.get<std::string>()) > 0) beat = bid.get<std::string>();
        }
        if (beat.empty()) continue;
        const std::string slot = core::py_str(core::py_get(panels[pi], "slot"));
        std::size_t at = 0;
        for (std::size_t i = 0; i < shape.panels.size(); ++i) {
            if (shape.panels[i].slot == slot) at = i + 1;
        }
        if (at == 0) continue;
        const double s = share(shape, at);
        if (s < kRevealSmallShare && (at != big || s < 0.2)) {
            out.push_back(warning("reveal_beat_small", "/panels/" + std::to_string(pi) + "/beat_ids",
                                  "見せ場（reveal）の beat " + beat + " のコマ " + slot + " が小さい（基本枠の " + percent(s, 0) + "" +
                                      (at != big ? "、いちばん大きいコマでない" : "") + "）",
                                  "そのコマを大ゴマにするか、断ち切り（bleed）・はみ出し（overflow）で目立たせる"));
        }
    }
}

// The book's pages in their order: each one's number and shape (none: blank, or one that cannot be measured).
struct PageShape {
    core::Num index;
    std::optional<Shape> shape;
};
using Shapes = std::vector<PageShape>;

std::optional<Shape> safe_shape(const core::Document& doc, const core::Page& page) {
    try {
        return shape_of_page(doc, page);
    } catch (const std::exception&) {  // (a warning never stops: a page that cannot be measured is not counted)
        return std::nullopt;
    }
}

// Whether the three pages from position `start` have the same or alike layouts.
bool run_at(const Shapes& shapes, std::size_t start) {
    if (start + 2 >= shapes.size()) return false;
    const auto& a = shapes[start].shape;
    const auto& b = shapes[start + 1].shape;
    const auto& c = shapes[start + 2].shape;
    if (!a || !b || !c) return false;
    return difference(*a, *b) <= kRepeatAtMost && difference(*b, *c) <= kRepeatAtMost;
}

std::optional<studio::Issue> accent_check(const Shapes& shapes) {
    int counted = 0;
    for (const PageShape& p : shapes) {
        if (!p.shape) continue;
        ++counted;
        if (p.shape->tilts > 0 || any_of(*p.shape, &PanelBox::bleed)) return std::nullopt;
    }
    if (counted < kAccentPages) return std::nullopt;
    return warning("book_without_accent", "/", "コマのある " + std::to_string(counted) + " ページに、斜めの境目も断ち切りのコマも 1 つも無い",
                   "見せ場や動きのあるページで、斜め（tilt_mm・slant）か断ち切り（bleed）を使う");
}

studio::Issue repeat_issue(const Shapes& shapes, std::size_t start, const std::string& path) {
    return warning("layout_repeats", path,
                   shapes[start].index.repr() + "〜" + shapes[start + 2].index.repr() + " ページの段組が、続けて同じか似ている（" +
                       describe(*shapes[start].shape) + "）",
                   "大ゴマ・斜め・断ち切り・コマの数で、ページごとに緩急を付ける（コマ割りの案の束: layout_ideas）");
}

}  // namespace

std::vector<studio::Issue> monotony(const core::Document& doc, const Json& page_index, const Json& plan, const Json& script) {
    std::vector<studio::Issue> out;
    try {
        const auto at = page_at(doc, page_index);
        if (!at) return out;
        const core::Page& target = doc.page(*at);
        const auto [fw, fh] = frame_size(target.spec);
        const Shape now = shape_of_plan(plan, fw, fh);
        const std::string base = core::truthy_at(plan, "tiers") ? "/tiers" : "/template";
        flat_check(out, now, base);
        reveal_check(out, now, plan, reveal_beats(script));
        Shapes shapes;
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            const core::Page& page = *doc.pages[i];
            shapes.push_back({page.index, i == *at ? std::optional<Shape>(now) : safe_shape(doc, page)});
        }
        for (std::size_t start = *at >= 2 ? *at - 2 : 0; start <= *at; ++start) {
            if (run_at(shapes, start)) {
                out.push_back(repeat_issue(shapes, start, base));
                break;
            }
        }
        if (auto accent = accent_check(shapes)) out.push_back(std::move(*accent));
    } catch (const std::exception&) {  // (warnings never stop a name: a plan that cannot be measured gets none)
        return {};
    }
    return out;
}

std::vector<std::pair<Json, studio::Issue>> book_monotony(const core::Document& doc, const Json& script) {
    std::vector<std::pair<Json, studio::Issue>> out;
    Shapes shapes;
    for (const auto& held : doc.pages) shapes.push_back({held->index, safe_shape(doc, *held)});
    const std::set<std::string> reveals = reveal_beats(script);
    std::size_t next_run = 0;  // (one warning a run of three)
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (!shapes[i].shape) continue;
        std::vector<studio::Issue> here;
        flat_check(here, *shapes[i].shape, "/tiers");
        const core::Page& page = *doc.pages[i];
        const Json name = page.plan ? core::py_get(*page.plan, "name") : Json();
        if (name.is_object()) {
            try {
                const auto [fw, fh] = frame_size(page.spec);
                reveal_check(here, shape_of_plan(name, fw, fh), name, reveals);
            } catch (const std::exception&) {
            }
        }
        if (i >= next_run && run_at(shapes, i)) {
            here.push_back(repeat_issue(shapes, i, "/tiers"));
            next_run = i + 3;
        }
        for (studio::Issue& issue : here) out.emplace_back(page.index.json(), std::move(issue));
    }
    if (auto accent = accent_check(shapes)) out.emplace_back(Json(), std::move(*accent));
    return out;
}

}  // namespace genko::service::komawari
