#include "service/layout.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/data.hpp"
#include "service/pyfmt.hpp"
#include "service/result.hpp"
#include "service/wording.hpp"

namespace genko::service::layout {

using core::Num;

LayoutError::LayoutError(std::string path, std::string message)
    : core::ApplyError(path + ": " + message), path_(std::move(path)), message_(std::move(message)) {}

const std::string* CompiledLayout::frame_of(const std::string& slot) const {
    for (const auto& [key, frame] : slot_to_frame) {
        if (key == slot) return &frame;
    }
    return nullptr;
}

const RectMm* CompiledLayout::rect_of(const std::string& slot) const {
    for (const auto& [key, rect] : leaf_rects_mm) {
        if (key == slot) return &rect;
    }
    return nullptr;
}

const Json& templates() { return data::layouts(); }

Json resolve_tiers(const Json& plan) {
    if (core::truthy_at(plan, "tiers")) return plan["tiers"];
    const Json name = core::py_get(plan, "template");
    if (!core::py_truthy(name)) throw LayoutError("/tiers", "tiers か template のどちらかが要る");
    const Json& known = templates();
    core::require_hashable(name);
    if (!name.is_string() || !known.contains(name.get<std::string>())) {
        std::vector<std::string> names;
        for (const auto& [key, value] : known.items()) names.push_back(key);
        std::sort(names.begin(), names.end());
        std::string listed;
        for (const std::string& key : names) listed += (listed.empty() ? "" : ", ") + key;
        throw LayoutError("/template", "定型 " + core::py_str(name) + " はない（" + listed + "）");
    }
    return known[name.get<std::string>()]["tiers"];
}

std::vector<std::string> slots_in_order(const Json& tiers) {
    std::vector<std::string> out;
    for (const Json& tier : core::iterate(tiers)) {
        for (const Json& col : core::iterate(core::subscript(tier, "cols"))) {
            if (core::truthy_at(col, "rows")) {
                for (const Json& row : core::iterate(col["rows"])) out.push_back(core::py_str(core::subscript(row, "slot")));
            } else {
                out.push_back(core::py_str(core::subscript(col, "slot")));
            }
        }
    }
    return out;
}

namespace {

void check_ratios(std::vector<studio::Issue>& issues, const std::vector<Json>& values, const std::string& path, const std::string& key,
                  const std::string& label) {
    std::vector<Num> nums;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const Json& value = values[index];
        const Num n = core::to_num(value);
        if (!(Num(0) < n && n <= Num(1))) {
            issues.push_back(error("layout_ratio_range", path + "/" + std::to_string(index) + "/" + key,
                                   label + "の " + key + " は 0 より大きく 1 以下にする（今 " + core::py_str(value) + "）"));
        }
        nums.push_back(n);
    }
    const Num total = core::py_sum(nums);
    if (std::abs(total.value() - 1.0) > kSumTolerance) {
        issues.push_back(error("layout_ratio_sum", path, label + "の " + key + " の合計が " + fixed(total, 2), "合計が 1.0 になるように直す"));
    }
}

using Seen = std::vector<std::pair<std::string, std::string>>;  // slot → where (in order)

bool seen_has(const Seen& seen, const std::string& slot) {
    return std::any_of(seen.begin(), seen.end(), [&slot](const auto& item) { return item.first == slot; });
}

void check_slot(std::vector<studio::Issue>& issues, Seen& seen, const Json& slot_value, const std::string& path) {
    const std::string slot = core::py_str(slot_value);
    if (!core::py_truthy(slot_value)) {
        issues.push_back(error("layout_slot_empty", path, "slot が空"));
    } else if (seen_has(seen, slot)) {
        issues.push_back(error("layout_slot_duplicate", path, "slot " + slot + " が重複している"));
    } else {
        seen.emplace_back(slot, path);
    }
}

std::vector<Json> column(const Json& items, const char* key) {
    std::vector<Json> out;
    for (const Json& item : core::iterate(items)) out.push_back(core::subscript(item, key));
    return out;
}

std::size_t find_page(const core::Document& doc, const Json& index) {
    if (const auto at = page_at(doc, index)) return *at;
    throw LayoutError("/page", core::py_str(index) + " ページはない（1〜" + std::to_string(doc.pages.size()) + "）");
}

const core::Frame& find_frame(const core::Document& doc, const Json& index, const std::string& frame_id) {
    const core::Page& page = doc.page(find_page(doc, index));
    const core::Frame* frame = page.find_frame(frame_id);
    if (frame == nullptr) throw core::PyUncaught("KeyError", core::py_repr_str(frame_id));
    return *frame;
}

double gap(const Json& plan, const std::string& key, double fallback) {
    const Json value = core::py_get(plan, key);
    if (value.is_null()) return fallback;
    const double v = core::to_float(value);
    if (!(0 <= v && v <= 100)) throw LayoutError("/" + key, key + " は 0〜100 mm");
    return v;
}

// The slot of a tier or column that is one panel (its border to the next is that panel's), or nothing.
std::optional<std::string> leaf(const Json& part) {
    const Json cols = core::py_get(part, "cols");
    if (!cols.is_null()) {
        return core::length(cols) == 1 ? leaf(core::subscript(cols, 0)) : std::nullopt;
    }
    if (core::truthy_at(part, "rows")) return std::nullopt;
    const Json slot = core::py_get(part, "slot");
    if (slot.is_null()) return std::nullopt;
    return core::py_str(slot);
}

// komawari@1: what one entry (a tier, column or row) of the plan says about the border after it — its gap (gap_mm) and
// its slant (tilt_mm) — when it says it (none: the plan's gap, and the panel's slant).
struct Border {
    std::optional<double> gap;
    std::optional<double> tilt;
};

Border border_after(const Json& entry) {
    Border out;
    const Json gap_value = core::py_get(entry, "gap_mm");
    if (!gap_value.is_null()) out.gap = core::to_float(gap_value);
    const Json tilt_value = core::py_get(entry, "tilt_mm");
    if (!tilt_value.is_null()) out.tilt = core::to_float(tilt_value);
    return out;
}

std::vector<Border> borders_of(const std::vector<Json>& entries) {
    std::vector<Border> out;
    for (const Json& entry : entries) out.push_back(border_after(entry));
    return out;
}

// komawari@1: an entry's own border after it (gap_mm, tilt_mm), as a page can hold it; on the last entry there is none.
void check_border(std::vector<studio::Issue>& issues, const Json& entry, const std::string& path, bool last, const std::string& label) {
    const Json gap_value = core::py_get(entry, "gap_mm");
    if (!gap_value.is_null()) {
        if (!gap_value.is_number() || !(core::to_float(gap_value) >= 0 && core::to_float(gap_value) <= 100)) {
            issues.push_back(error("layout_gap_range", path + "/gap_mm", "gap_mm は 0〜100 mm（今 " + core::py_str(gap_value) + "）"));
        } else if (last) {
            issues.push_back(warning("layout_border_unused", path + "/gap_mm", "最後の" + label + "の gap_mm は使われない（次との間を決める鍵）",
                                     "間を変えたい境目の前の" + label + "に書く"));
        }
    }
    const Json tilt_value = core::py_get(entry, "tilt_mm");
    if (!tilt_value.is_null()) {
        if (!tilt_value.is_number() || !(std::abs(core::to_float(tilt_value)) <= kMaxTiltMm)) {
            issues.push_back(error("layout_tilt_range", path + "/tilt_mm",
                                   "tilt_mm は -" + fixed(kMaxTiltMm, 0) + "〜" + fixed(kMaxTiltMm, 0) + " mm（今 " + core::py_str(tilt_value) + "）"));
        } else if (last) {
            issues.push_back(warning("layout_border_unused", path + "/tilt_mm", "最後の" + label + "の tilt_mm は使われない（次との境目を斜めにする鍵）",
                                     "斜めにしたい境目の前の" + label + "に書く"));
        }
    }
}

// komawari@1: the panels set in another's place (差し込み・重ね): their slots, the panel each goes into, and where.
void check_insets(std::vector<studio::Issue>& issues, Seen& seen, const Json& plan) {
    const Json insets = core::py_get(plan, "insets");
    if (insets.is_null()) return;
    if (!insets.is_array()) {
        issues.push_back(error("layout_insets", "/insets", "insets は配列（[{slot, on, rect}]）"));
        return;
    }
    const Seen hosts = seen;  // (the panels of the tiers: an inset goes into one of them)
    for (std::size_t i = 0; i < insets.size(); ++i) {
        const Json& inset = insets[i];
        const std::string path = "/insets/" + std::to_string(i);
        if (!inset.is_object()) {
            issues.push_back(error("layout_insets", path, "差し込みは {slot, on, rect}"));
            continue;
        }
        check_slot(issues, seen, core::py_get(inset, "slot"), path + "/slot");
        const Json on = core::py_get(inset, "on");
        if (!on.is_string() || !seen_has(hosts, on.get<std::string>())) {
            issues.push_back(error("layout_inset_host", path + "/on", "差し込む先（on）の " + core::py_str(on) + " が段組のコマにない"));
        }
        const Json rect = core::py_get(inset, "rect");
        bool fits = rect.is_array() && rect.size() == 4 && std::all_of(rect.begin(), rect.end(), [](const Json& v) { return v.is_number(); });
        if (fits) {
            const double x = core::to_float(rect[0]), y = core::to_float(rect[1]), w = core::to_float(rect[2]), h = core::to_float(rect[3]);
            fits = x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= 1 + 1e-9 && y + h <= 1 + 1e-9;
        }
        if (!fits) {
            issues.push_back(error("layout_inset_rect", path + "/rect", "rect は基本枠の中の [x, y, w, h]（基本枠の幅・高さを 1 とした割合）"));
        }
    }
}

std::vector<std::string> split_sequence(Work& work, const Json& page_index, const std::string& frame_id, const std::vector<Json>& ratios,
                                        const std::string& axis, double gutter, const std::string& agent, CompiledLayout& out,
                                        const std::string& path, const std::vector<double>& tilts, const std::vector<Border>& borders = {}) {
    if (ratios.size() == 1) return {frame_id};
    const bool horizontal = axis == "horizontal";
    std::vector<Num> nums;
    for (const Json& r : ratios) nums.push_back(core::to_num(r));
    const core::Frame& frame = find_frame(work.doc, page_index, frame_id);
    const Num total = core::py_sum(nums);
    const double extent = (horizontal ? frame.rect.height : frame.rect.width).value();
    // each border's gap: the entry's own (komawari@1), else the plan's
    std::vector<double> gaps;
    bool own_gaps = false;
    for (std::size_t index = 0; index + 1 < ratios.size(); ++index) {
        const bool own = index < borders.size() && borders[index].gap.has_value();
        own_gaps = own_gaps || own;
        gaps.push_back(own ? *borders[index].gap : gutter);
    }
    double usable = extent - gutter * static_cast<double>(ratios.size() - 1);  // (a plan without its own gaps: as before)
    if (own_gaps) {
        usable = extent;
        for (const double g : gaps) usable -= g;
    }
    std::vector<double> spans;
    for (const Num& r : nums) spans.push_back(usable * r.value() / total.value());
    std::vector<std::string> parts;
    std::string remaining = frame_id;
    for (std::size_t index = 0; index + 1 < spans.size(); ++index) {
        const core::Rect rect = find_frame(work.doc, page_index, remaining).rect;
        const double rest_extent = (horizontal ? rect.height : rect.width).value() - gaps[index];
        const double ratio = horizontal ? spans[index] / rest_extent : (rest_extent - spans[index]) / rest_extent;
        const bool own_tilt = index < borders.size() && borders[index].tilt.has_value();
        const double tilt = own_tilt ? *borders[index].tilt : (!tilts.empty() ? tilts[index] : 0.0);
        Json op = Json::object();
        op["op"] = "split_frame";
        op["page"] = page_index;
        op["frame_id"] = remaining;
        op["axis"] = axis;
        op["ratio"] = ratio;
        op["gutter_mm"] = gaps[index];
        if (std::abs(tilt) > 1e-6) op["tilt_mm"] = core::py_round(tilt, 2);
        out.source_map.emplace_back(out.ops.size(), path + "/" + std::to_string(index));
        out.ops.push_back(op);
        if (own_tilt || own_gaps) {
            // (komawari@1: a border the plan shapes itself — what does not fit is the plan's, in its words)
            if (!(ratio > 0 && ratio < 1)) throw LayoutError(path + "/" + std::to_string(index), "間（gap_mm）が広すぎて、コマが入らない");
            try {
                work.apply(Json::array({op}), agent);
            } catch (const core::ApplyError& e) {
                throw LayoutError(path + "/" + std::to_string(index) + (own_tilt ? "/tilt_mm" : "/gap_mm"),
                                  wording::error(std::string(e.what())).toStdString());
            }
        } else {
            work.apply(Json::array({op}), agent);
        }
        const core::Frame& split = find_frame(work.doc, page_index, remaining);
        if (split.children.size() != 2) throw core::PyValueError("not enough values to unpack (expected 2, got " + std::to_string(split.children.size()) + ")");
        if (horizontal) {
            parts.push_back(split.children[0].id);
            remaining = split.children[1].id;
        } else {
            parts.push_back(split.children[1].id);
            remaining = split.children[0].id;
        }
    }
    parts.push_back(remaining);
    return parts;
}

}  // namespace

std::vector<studio::Issue> check_tiers(const Json& plan) {
    std::vector<studio::Issue> issues;
    Json tiers;
    try {
        tiers = resolve_tiers(plan);
    } catch (const LayoutError& e) {
        return {error("layout_missing", e.where(), e.message())};
    }
    const std::string base = core::truthy_at(plan, "tiers") ? "/tiers" : "/template";
    if (!core::py_truthy(tiers)) return {error("layout_empty", base, "段が1つもない")};
    check_ratios(issues, column(tiers, "h"), base, "h", "段");
    Seen seen;
    const std::vector<Json> tier_list = core::iterate(tiers);
    for (std::size_t ti = 0; ti < tier_list.size(); ++ti) {
        const Json& tier = tier_list[ti];
        const std::string tpath = base + "/" + std::to_string(ti);
        const Json cols = core::subscript(tier, "cols");
        if (!core::py_truthy(cols)) {
            issues.push_back(error("layout_empty", tpath + "/cols", "列が1つもない"));
            continue;
        }
        check_ratios(issues, column(cols, "w"), tpath + "/cols", "w", "列");
        const std::vector<Json> col_list = core::iterate(cols);
        for (std::size_t ci = 0; ci < col_list.size(); ++ci) {
            const Json& col = col_list[ci];
            const std::string cpath = tpath + "/cols/" + std::to_string(ci);
            const Json rows = core::py_get(col, "rows");
            if (core::py_truthy(rows)) {
                check_ratios(issues, column(rows, "h"), cpath + "/rows", "h", "行");
                const std::vector<Json> row_list = core::iterate(rows);
                for (std::size_t ri = 0; ri < row_list.size(); ++ri) {
                    check_slot(issues, seen, core::subscript(row_list[ri], "slot"), cpath + "/rows/" + std::to_string(ri) + "/slot");
                    check_border(issues, row_list[ri], cpath + "/rows/" + std::to_string(ri), ri + 1 == row_list.size(), "行");
                }
            } else {
                check_slot(issues, seen, core::subscript(col, "slot"), cpath + "/slot");
            }
            check_border(issues, col, cpath, ci + 1 == col_list.size(), "列");
        }
        check_border(issues, tier, tpath, ti + 1 == tier_list.size(), "段");
    }
    check_insets(issues, seen, plan);
    if (seen.size() > kMaxPanels) {
        issues.push_back(error("layout_too_many", base, "コマが " + std::to_string(seen.size()) + " 個ある（1ページ " + std::to_string(kMaxPanels) + " 個まで）"));
    }
    std::vector<std::string> panel_slots;
    for (const Json& panel : core::iterate(core::py_get(plan, "panels", Json::array()))) panel_slots.push_back(core::py_str(core::subscript(panel, "slot")));
    for (std::size_t pi = 0; pi < panel_slots.size(); ++pi) {
        const std::string& slot = panel_slots[pi];
        if (!seen_has(seen, slot)) {
            issues.push_back(error("panel_unknown_slot", "/panels/" + std::to_string(pi) + "/slot", "段組に " + slot + " というコマがない"));
        } else if (std::count(panel_slots.begin(), panel_slots.end(), slot) > 1) {
            issues.push_back(error("panel_duplicate_slot", "/panels/" + std::to_string(pi) + "/slot", slot + " のコマ指示が2つある"));
        }
    }
    for (const auto& [slot, where] : seen) {
        if (std::find(panel_slots.begin(), panel_slots.end(), slot) == panel_slots.end()) {
            issues.push_back(error("panel_missing", where, "コマ " + slot + " の指示（panels）がない"));
        }
    }
    // komawari@1: a panel's border (border_mm: 0 は枠なし)
    const std::vector<Json> panel_list = core::iterate(core::py_get(plan, "panels", Json::array()));
    for (std::size_t pi = 0; pi < panel_list.size(); ++pi) {
        const Json border = core::py_get(panel_list[pi], "border_mm");
        if (border.is_null()) continue;
        if (!border.is_number() || !(core::to_float(border) >= 0 && core::to_float(border) <= kMaxBorderMm)) {
            issues.push_back(error("panel_border_range", "/panels/" + std::to_string(pi) + "/border_mm",
                                   "border_mm は 0〜" + fixed(kMaxBorderMm, 0) + " mm（0 で枠なし。今 " + core::py_str(border) + "）"));
        }
    }
    return issues;
}

bool is_blank(const core::Document& doc, const core::Page& page) {
    return page.leaf_frames().size() == 1 && doc.story_for_page(page.index).empty();
}

Json clear_page(Work& work, std::size_t at, const std::string& agent) {
    const core::Page& page = work.doc.page(at);
    Json ops = Json::array();
    for (const core::StoryLine* line : work.doc.story_for_page(page.index)) ops.push_back(Json::object({{"op", "delete_line"}, {"id", line->id}}));
    const core::Frame& root = page.frames.at(0);
    if (root.split_axis && *root.split_axis == "free") {  // (drawn panels are not merged: the page goes back to its basic frame)
        const core::Rect inner = page.inner_rect_mm(work.doc.start_side ? std::string_view(*work.doc.start_side) : std::string_view());
        ops.push_back(Json::object({{"op", "set_layout"}, {"page", page.index.json()}, {"force", true},
                                    {"tree", Json::object({{"rect_mm", Json::array({inner.x.json(), inner.y.json(), inner.width.json(), inner.height.json()})}})}}));
    } else if (!root.children.empty()) {
        ops.push_back(Json::object({{"op", "merge_frame"}, {"page", page.index.json()}, {"frame_id", root.children[0].id}}));
    }
    // merge_frame only collapses one level: the root keeps no children afterwards.
    if (!ops.empty()) work.apply(ops, agent);
    return ops;
}

CompiledLayout apply_layout(Work& work, const Json& page_index, const Json& plan, const std::string& agent) {
    {
        const core::Page& page = work.doc.page(find_page(work.doc, page_index));
        if (!is_blank(work.doc, page)) throw LayoutError("/", core::py_str(page_index) + " ページはすでにコマか台詞がある（replace:true で作り直す）");
    }
    for (const studio::Issue& issue : check_tiers(plan)) {
        if (issue.severity == "error") throw LayoutError(issue.path, issue.message);
    }
    const Json tiers = resolve_tiers(plan);
    const std::string base = core::truthy_at(plan, "tiers") ? "/tiers" : "/template";
    Json shape = Json::object();
    if (!core::truthy_at(plan, "tiers")) {
        const Json name = core::py_get(plan, "template");
        if (name.is_string() && templates().contains(name.get<std::string>())) shape = templates()[name.get<std::string>()];
    }
    std::set<std::string> bleeds;
    for (const Json& slot : core::iterate(core::py_or(core::py_get(shape, "bleed"), Json::array()))) bleeds.insert(core::py_str(slot));
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
        for (const auto& [k, v] : slants) {
            if (k == key.value_or("")) return v;
        }
        return 0.0;
    };
    const Json given_slants = core::py_or(core::py_get(shape, "slant"), Json::object());
    if (!given_slants.is_object()) core::raise_attribute_error(given_slants, "items");
    for (const auto& [key, value] : given_slants.items()) set_slant(key, core::to_float(value));
    for (const Json& panel : core::iterate(core::py_or(core::py_get(plan, "panels"), Json::array()))) {  // (the plan's own choices win)
        const Json bleed = core::py_get(panel, "bleed");
        if (!bleed.is_null()) {
            const std::string slot = core::py_str(core::py_get(panel, "slot"));
            if (core::py_truthy(bleed)) {
                bleeds.insert(slot);
            } else {
                bleeds.erase(slot);
            }
        }
        const Json slant = core::py_get(panel, "slant");
        if (!slant.is_null()) set_slant(core::py_str(core::py_get(panel, "slot")), core::to_float(slant));
    }

    const double tier_gap = gap(plan, "tier_gap_mm", kTierGutterMm);
    const double col_gap = gap(plan, "col_gap_mm", kColGutterMm);
    CompiledLayout out;
    const std::vector<Json> tier_list = core::iterate(tiers);
    std::vector<double> tier_tilts;
    for (const Json& tier : tier_list) tier_tilts.push_back(slant_of(leaf(tier)));
    const std::string root_id = work.doc.page(find_page(work.doc, page_index)).frames.at(0).id;
    const core::Rect basic_frame = work.doc.page(find_page(work.doc, page_index)).frames.at(0).rect;  // (insets are placed in it)
    const std::vector<std::string> tier_ids =
        split_sequence(work, page_index, root_id, column(tiers, "h"), "horizontal", tier_gap, agent, out, base, tier_tilts, borders_of(tier_list));
    const auto put_slot = [&out](const std::string& slot, const std::string& frame_id) {
        for (auto& [key, value] : out.slot_to_frame) {
            if (key == slot) {
                value = frame_id;
                return;
            }
        }
        out.slot_to_frame.emplace_back(slot, frame_id);
    };
    for (std::size_t ti = 0; ti < tier_list.size() && ti < tier_ids.size(); ++ti) {
        const Json cols = core::subscript(tier_list[ti], "cols");
        const std::vector<Json> col_list = core::iterate(cols);
        std::vector<double> col_tilts;
        for (const Json& col : col_list) col_tilts.push_back(slant_of(leaf(col)));
        const std::vector<std::string> col_ids = split_sequence(work, page_index, tier_ids[ti], column(cols, "w"), "vertical", col_gap, agent, out,
                                                                base + "/" + std::to_string(ti) + "/cols", col_tilts, borders_of(col_list));
        for (std::size_t ci = 0; ci < col_list.size() && ci < col_ids.size(); ++ci) {
            const Json rows = core::py_get(col_list[ci], "rows");
            if (core::py_truthy(rows)) {
                const std::vector<Json> row_list = core::iterate(rows);
                std::vector<double> row_tilts;
                for (const Json& row : row_list) {
                    const Json slot = core::py_get(row, "slot");
                    row_tilts.push_back(slant_of(core::py_truthy(slot) ? std::optional<std::string>(core::py_str(slot)) : std::nullopt));
                }
                const std::vector<std::string> row_ids =
                    split_sequence(work, page_index, col_ids[ci], column(rows, "h"), "horizontal", tier_gap, agent, out,
                                   base + "/" + std::to_string(ti) + "/cols/" + std::to_string(ci) + "/rows", row_tilts, borders_of(row_list));
                for (std::size_t ri = 0; ri < row_list.size() && ri < row_ids.size(); ++ri) put_slot(core::py_str(core::subscript(row_list[ri], "slot")), row_ids[ri]);
            } else {
                put_slot(core::py_str(core::subscript(col_list[ci], "slot")), col_ids[ci]);
            }
        }
    }
    // komawari@1: the panels set in another's place (差し込み・重ね), over the others, read with the panel they go into
    const Json insets = core::py_get(plan, "insets");
    if (insets.is_array()) {
        const Json selected = work.doc.page(find_page(work.doc, page_index)).selected_frame_id;
        const core::Rect frame = basic_frame;
        for (std::size_t i = 0; i < insets.size(); ++i) {
            const Json& inset = insets[i];
            const std::string path = "/insets/" + std::to_string(i);
            const std::string* host = out.frame_of(core::py_str(core::py_get(inset, "on")));
            if (host == nullptr) throw LayoutError(path + "/on", "差し込む先のコマがない");
            const Json rect = core::py_get(inset, "rect");  // (check_tiers has made it [x, y, w, h] in 0..1)
            if (!rect.is_array() || rect.size() != 4) throw LayoutError(path + "/rect", "rect は基本枠の中の [x, y, w, h]");
            const double fw = frame.width.value(), fh = frame.height.value();
            Json op = Json::object({{"op", "add_frame"},
                                    {"page", page_index},
                                    {"rect", Json::array({core::py_round(frame.x.value() + core::to_float(rect[0]) * fw, 2),
                                                          core::py_round(frame.y.value() + core::to_float(rect[1]) * fh, 2),
                                                          core::py_round(core::to_float(rect[2]) * fw, 2), core::py_round(core::to_float(rect[3]) * fh, 2)})},
                                    {"into", *host},
                                    {"over", true}});
            out.source_map.emplace_back(out.ops.size(), path);
            out.ops.push_back(op);
            try {
                work.apply(Json::array({op}), agent);
            } catch (const core::ApplyError& e) {
                throw LayoutError(path, wording::error(std::string(e.what())).toStdString());
            }
            const Json chosen = work.doc.page(find_page(work.doc, page_index)).selected_frame_id;  // (add_frame chose the new panel)
            put_slot(core::py_str(core::py_get(inset, "slot")), core::py_str(chosen));
        }
        work.doc.edit_page(find_page(work.doc, page_index)).selected_frame_id = selected;  // (the selection as it was)
    }
    Json extra = Json::array();
    for (const std::string& slot : bleeds) {  // (sorted: a set of slots)
        if (const std::string* frame_id = out.frame_of(slot)) {
            extra.push_back(Json::object({{"op", "set_frame"}, {"page", page_index}, {"frame_id", *frame_id}, {"bleed", true}}));
        }
    }
    // komawari@1: each panel's border (border_mm; 0 は枠なし) and whether its art runs past it (overflow: はみ出し)
    for (const Json& panel : core::iterate(core::py_or(core::py_get(plan, "panels"), Json::array()))) {
        const Json border = core::py_get(panel, "border_mm");
        const Json overflow = core::py_get(panel, "overflow");
        if (border.is_null() && overflow.is_null()) continue;
        const std::string* frame_id = out.frame_of(core::py_str(core::py_get(panel, "slot")));
        if (frame_id == nullptr) continue;
        Json op = Json::object({{"op", "set_frame"}, {"page", page_index}, {"frame_id", *frame_id}});
        if (!border.is_null()) op["border_mm"] = core::to_float(border);
        if (!overflow.is_null()) op["clip"] = !core::py_truthy(overflow);
        extra.push_back(std::move(op));
    }
    if (core::truthy_at(plan, "spread")) {  // (見開き: this page and the next one are one picture)
        extra.push_back(Json::object({{"op", "set_spread"}, {"page", page_index}, {"with", (core::to_num(page_index) + Num(1)).json()}}));
    }
    for (const Json& op : extra) {
        const std::string where = op["op"] == Json("set_spread") ? "/spread" : "/panels";
        out.source_map.emplace_back(out.ops.size(), where);
        out.ops.push_back(op);
        try {
            work.apply(Json::array({op}), agent);
        } catch (const core::ApplyError& e) {
            throw LayoutError(where, e.what());
        }
    }
    const core::Page& page = work.doc.page(find_page(work.doc, page_index));
    for (const core::Frame* frame : page.leaf_frames()) {
        const auto slot = std::find_if(out.slot_to_frame.begin(), out.slot_to_frame.end(), [&frame](const auto& item) { return item.second == frame->id; });
        if (slot == out.slot_to_frame.end()) throw core::PyUncaught("KeyError", core::py_repr_str(frame->id));
        out.reading_order.push_back(slot->first);
        const core::Rect& r = frame->rect;
        out.leaf_rects_mm.emplace_back(slot->first, RectMm{core::py_round(r.x, 3), core::py_round(r.y, 3), core::py_round(r.width, 3), core::py_round(r.height, 3)});
    }
    return out;
}

}  // namespace genko::service::layout
