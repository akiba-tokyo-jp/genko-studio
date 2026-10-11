#include "service/lint.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/fxwords.hpp"
#include "service/pyfmt.hpp"
#include "service/result.hpp"
#include "service/text.hpp"

namespace genko::service::lint {

using studio::Issue;

const std::vector<std::pair<std::string, std::string>>& bundled_fonts() {
    static const std::vector<std::pair<std::string, std::string>> fonts = {
        {"antique", "アンチック（かなは明朝・漢字はゴシック）"}, {"gothic", "ゴシック"}, {"mincho", "明朝"}, {"maru", "丸ゴシック"},
        {"hand", "手書き風"}, {"sfx", "極太（効果音）"}, {"sfx_pop", "勢い（効果音）"},
    };
    return fonts;
}

namespace {

const char* const kTextBeats[] = {"dialogue", "monologue", "narration"};
const char* const kEstablishing[] = {"ELS", "LS", "FS"};

bool text_beat(const Json& kind) {
    return kind.is_string() && std::any_of(std::begin(kTextBeats), std::end(kTextBeats), [&kind](const char* k) { return kind == k; });
}

// `value in values` for a set or a dict's keys (Python's hash, then ==)
bool contains(const std::vector<Json>& values, const Json& value) {
    core::require_hashable(value);
    return std::any_of(values.begin(), values.end(), [&value](const Json& v) { return core::py_equals(v, value); });
}

void unique(std::vector<Issue>& issues, const Json& items, const std::string& path, const std::string& label) {
    std::vector<Json> seen;
    const std::vector<Json> list = core::iterate(items);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json key = core::py_get(list[i], "id", Json(""));
        if (contains(seen, key)) issues.push_back(error("duplicate_id", path + "/" + std::to_string(i) + "/id", label + "の id " + core::py_str(key) + " が重複している"));
        seen.push_back(key);
    }
}

// Path(str(face)).expanduser().is_file()
bool font_file(const std::string& face) {
    std::string expanded = face;
    if (face == "~" || face.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home != nullptr) expanded = std::string(home) + face.substr(1);
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(core::path_from_utf8(expanded), ec);
}

std::string ints(std::int64_t n) { return std::to_string(n); }

// lint._shares: each slot's share of the page (from the tier, column and row ratios); empty without tiers.
std::vector<std::pair<std::string, double>> shares_of(const Json& plan) {
    Json tiers;
    try {
        tiers = layout::resolve_tiers(plan);
    } catch (const layout::LayoutError&) {
        return {};
    }
    std::vector<std::pair<std::string, double>> out;
    const auto put = [&out](const std::string& slot, double value) {
        for (auto& [k, v] : out) {
            if (k == slot) {
                v = value;
                return;
            }
        }
        out.emplace_back(slot, value);
    };
    const auto ratio = [](const Json& item, const char* key) { return core::to_float(core::py_or(core::py_get(item, key), Json(0))); };
    const auto total_of = [&ratio](const std::vector<Json>& items, const char* key) {
        std::vector<double> values;
        for (const Json& item : items) values.push_back(ratio(item, key));
        const double total = core::py_float_sum(values);
        return total != 0 ? total : 1.0;
    };
    const std::vector<Json> tier_list = core::iterate(tiers);
    const double total_h = total_of(tier_list, "h");
    for (const Json& tier : tier_list) {
        const std::vector<Json> cols = core::iterate(core::py_or(core::py_get(tier, "cols"), Json::array()));
        const double total_w = total_of(cols, "w");
        for (const Json& col : cols) {
            const double part = ratio(tier, "h") / total_h * ratio(col, "w") / total_w;
            const std::vector<Json> rows = core::iterate(core::py_or(core::py_get(col, "rows"), Json::array()));
            if (!rows.empty()) {
                const double total_r = total_of(rows, "h");
                for (const Json& row : rows) put(core::py_str(core::subscript(row, "slot")), part * ratio(row, "h") / total_r);
            } else {
                put(core::py_str(core::subscript(col, "slot")), part);
            }
        }
    }
    return out;
}

double share(const std::vector<std::pair<std::string, double>>& shares, const std::string& slot) {
    for (const auto& [k, v] : shares) {
        if (k == slot) return v;
    }
    return 1;
}

std::vector<Issue> size_hints(const Json& plan, std::int64_t page, std::int64_t page_count, bool tall) {
    const auto shares = shares_of(plan);
    if (shares.empty()) return {};
    std::vector<std::string> order;
    try {
        order = layout::slots_in_order(layout::resolve_tiers(plan));
    } catch (const layout::LayoutError&) {
        return {};
    }
    std::vector<std::pair<Json, std::pair<std::size_t, Json>>> panels;  // slot → (i, panel), a later one replacing
    const std::vector<Json> panel_list = core::iterate(core::py_or(core::py_get(plan, "panels"), Json::array()));
    for (std::size_t i = 0; i < panel_list.size(); ++i) {
        const Json slot = core::py_get(panel_list[i], "slot");
        core::require_hashable(slot);
        const auto at = std::find_if(panels.begin(), panels.end(), [&slot](const auto& item) { return core::py_equals(item.first, slot); });
        if (at != panels.end()) {
            at->second = {i, panel_list[i]};
        } else {
            panels.push_back({slot, {i, panel_list[i]}});
        }
    }
    std::vector<Issue> out;
    if (core::py_get(plan, "turn_role") == Json("reveal") && !order.empty() && share(shares, order[0]) < 0.3) {
        out.push_back(warning("reveal_small", "/tiers", "めくってすぐの見せ場なのに、最初のコマがページの3割より小さい",
                              "template の reveal_top・reveal_bleed・splash を使うか、最初の段を大きくして bleed（断ち切り）にする"));
    }
    for (const auto& [slot, item] : panels) {
        double weight = 0;
        try {
            weight = core::to_float(core::py_or(core::py_get(item.second, "emphasis"), Json(0)));
        } catch (const core::PyValueError&) {
            continue;
        } catch (const core::PyTypeError&) {
            continue;
        }
        if (weight >= 0.8 && share(shares, core::py_str(slot)) < 0.3) {
            out.push_back(warning("emphasis_small", "/panels/" + std::to_string(item.first) + "/emphasis", "強調の高いコマ " + core::py_str(slot) + " がページの3割より小さい",
                                  "大きい段に置くか、bleed（断ち切り）や slant（斜め）で目立たせる"));
        }
    }
    if (page == page_count && !order.empty() && !tall) {  // (a tall strip ends by scrolling, not with a page's last panel)
        const std::string& last = order.back();
        Json panel = Json::object();
        for (const auto& [slot, item] : panels) {
            if (slot.is_string() && slot == last) panel = item.second;
        }
        bool template_bleed = false;
        if (!core::truthy_at(plan, "tiers")) {
            const Json name = core::py_get(plan, "template");
            const Json& known = layout::templates();
            const std::string key = name.is_string() ? name.get<std::string>() : std::string();
            if (known.contains(key)) {
                for (const Json& slot : core::iterate(core::py_or(core::py_get(known[key], "bleed"), Json::array()))) template_bleed = template_bleed || slot == Json(last);
            }
        }
        if (share(shares, last) < 0.35 && !(core::truthy_at(panel, "bleed") || template_bleed)) {
            out.push_back(warning("finale_small", "/tiers", "最後のページの最後のコマが小さく、断ち切りでもない（余韻が弱い）",
                                  "template の finale_bleed を使うか、最後の段を大きくして bleed にする"));
        }
    }
    return out;
}

// lint._collect_sides: where each character stands in a scene's panels (left to right), and whether two swapped.
void collect_sides(std::vector<std::pair<Json, Json>>& sides, const Json& panel, const Json& index) {
    std::vector<Json> scene_ids;
    for (const Json& bid : core::iterate(core::py_get(panel, "beat_ids", Json::array()))) {
        if (bid.is_string() && index.contains(bid.get<std::string>())) {
            const Json id = index[bid.get<std::string>()]["scene_id"];
            if (!contains(scene_ids, id)) scene_ids.push_back(id);
        }
    }
    const std::vector<Json> chars = core::iterate(core::py_get(panel, "characters", Json::array()));
    if (scene_ids.size() != 1 || chars.size() < 2) return;
    auto at = std::find_if(sides.begin(), sides.end(), [&scene_ids](const auto& item) { return core::py_equals(item.first, scene_ids[0]); });
    if (at == sides.end()) {
        sides.push_back({scene_ids[0], Json::object()});
        at = sides.end() - 1;
    }
    Json& scene = at->second;  // (character id → position; "_crossed")
    const auto order = [](const Json& pos) {
        static const std::pair<const char*, int> table[] = {{"left", 0}, {"left_third", 1}, {"center", 2}, {"right_third", 3}, {"right", 4}};
        core::require_hashable(pos);
        for (const auto& [k, v] : table) {
            if (pos == k) return v;
        }
        return 2;
    };
    const auto key_of = [](const Json& id) { return core::py_str(id); };
    for (std::size_t ci = 0; ci < chars.size(); ++ci) {
        const Json& c = chars[ci];
        const int pos = order(core::py_get(c, "pos"));
        const std::string cid = key_of(core::py_get(c, "id"));
        if (scene.contains(cid) && !core::truthy_at(panel, "cross")) {
            const int prev = scene[cid].is_boolean() ? (scene[cid].get<bool>() ? 1 : 0) : scene[cid].get<int>();
            for (std::size_t oi = 0; oi < chars.size(); ++oi) {
                if (oi == ci) continue;
                const Json& other = chars[oi];
                const std::string oid = key_of(core::py_get(other, "id"));
                if (!scene.contains(oid)) continue;
                const int theirs = scene[oid].is_boolean() ? (scene[oid].get<bool>() ? 1 : 0) : scene[oid].get<int>();
                const int before = prev - theirs;
                const int now = pos - order(core::py_get(other, "pos"));
                if (before * now < 0) scene["_crossed"] = true;
            }
        }
    }
    for (const Json& c : chars) scene[key_of(core::py_get(c, "id"))] = order(core::py_get(c, "pos"));
}

}  // namespace

std::vector<Issue> lint_bible(const Json& bible) {
    std::vector<Issue> issues;
    unique(issues, core::py_get(bible, "characters", Json::array()), "/characters", "キャラ");
    unique(issues, core::py_get(bible, "locations", Json::array()), "/locations", "場所");
    unique(issues, core::py_or(core::py_get(bible, "props"), Json::array()), "/props", "小物");
    const Json lettering = core::py_or(core::py_get(bible, "lettering"), Json::object());
    if (!lettering.is_object()) core::raise_attribute_error(lettering, "items");
    for (const auto& [kind, look] : lettering.items()) {
        const Json face = look.is_object() ? core::py_get(core::py_or(look, Json::object()), "font") : Json();
        if (!core::py_truthy(face)) continue;
        const std::string name = core::py_str(face);
        const auto& bundled = bundled_fonts();
        const bool is_bundled = face.is_string() &&
                                std::any_of(bundled.begin(), bundled.end(), [&name](const auto& item) { return item.first == name; });
        if (!is_bundled && !font_file(name)) {
            issues.push_back(warning("unknown_font", "/lettering/" + kind + "/font", "書体 " + name + " が見つからない（台詞の書体になる）",
                                     "inspect target=fonts の bundled の key か、パソコンの書体のパス"));
        }
    }
    const std::vector<Json> chars = core::iterate(core::py_get(bible, "characters", Json::array()));
    for (std::size_t i = 0; i < chars.size(); ++i) {
        for (std::size_t j = i + 1; j < chars.size(); ++j) {
            const Json la = core::py_get(chars[i], "look", Json::object());
            const Json lb = core::py_get(chars[j], "look", Json::object());
            const Json silhouette = core::py_get(la, "silhouette");
            if (core::py_equals(core::py_get(la, "hair_value"), core::py_get(lb, "hair_value")) && core::py_truthy(silhouette) &&
                core::py_equals(silhouette, core::py_get(lb, "silhouette"))) {
                issues.push_back(warning("characters_look_alike", "/characters/" + std::to_string(j) + "/look",
                                         core::py_str(core::py_get(chars[i], "name")) + " と " + core::py_str(core::py_get(chars[j], "name")) +
                                             " はモノクロで見分けにくい（髪の値とシルエットが同じ）",
                                         "hair_value か silhouette を変える"));
            }
        }
    }
    return issues;
}

std::vector<Issue> lint_script(const Json& script, const Json& bible, std::int64_t page_count) {
    std::vector<Issue> issues;
    std::vector<Json> known;
    for (const Json& c : core::iterate(core::py_get(bible, "characters", Json::array()))) known.push_back(core::py_get(c, "id"));
    std::vector<Json> locations;
    for (const Json& loc : core::iterate(core::py_get(bible, "locations", Json::array()))) locations.push_back(core::py_get(loc, "id"));
    std::vector<Json> seen;
    std::vector<std::int64_t> pages_used;
    const std::vector<Json> scenes = core::iterate(core::py_get(script, "scenes", Json::array()));
    for (std::size_t si = 0; si < scenes.size(); ++si) {
        const Json& scene = scenes[si];
        const std::string spath = "/scenes/" + std::to_string(si);
        if (core::truthy_at(scene, "location_id") && !contains(locations, scene["location_id"])) {
            issues.push_back(warning("unknown_location", spath + "/location_id", "場所 " + core::py_str(scene["location_id"]) + " が企画書にない"));
        }
        const std::vector<Json> beats = core::iterate(core::py_get(scene, "beats", Json::array()));
        for (std::size_t bi = 0; bi < beats.size(); ++bi) {
            const Json& beat = beats[bi];
            const std::string bpath = spath + "/beats/" + std::to_string(bi);
            const Json beat_id = core::py_get(beat, "id", Json(""));
            if (contains(seen, beat_id)) issues.push_back(error("duplicate_beat_id", bpath + "/id", "beat id " + core::py_str(beat_id) + " が重複している"));
            seen.push_back(beat_id);
            const Json page = core::py_get(beat, "page", Json(0));
            const std::int64_t page_n = core::to_int(page);
            if (!(1 <= page_n && page_n <= page_count)) {
                issues.push_back(error("page_out_of_range", bpath + "/page", "page " + core::py_str(page) + " は範囲外（1〜" + ints(page_count) + "）"));
            } else if (std::find(pages_used.begin(), pages_used.end(), page_n) == pages_used.end()) {
                pages_used.push_back(page_n);
            }
            const Json kind = core::py_get(beat, "kind");
            const Json speaker = core::py_get(beat, "speaker_id");
            if (kind == Json("dialogue") || kind == Json("monologue")) {
                if (!core::py_truthy(speaker)) {
                    issues.push_back(error("speaker_missing", bpath + "/speaker_id", "台詞に話者がない"));
                } else if (!contains(known, speaker)) {
                    issues.push_back(error("unknown_speaker", bpath + "/speaker_id", "話者 " + core::py_str(speaker) + " が企画書のキャラにない"));
                }
            }
            if (text_beat(kind)) {
                const std::size_t length = core::length(core::py_get(beat, "text", Json("")));
                if (length > kMaxBalloonChars * kMaxBalloonsPerBeat) {
                    issues.push_back(error("beat_too_long", bpath + "/text",
                                           "台詞が " + std::to_string(length) + " 字ある（フキダシ " + std::to_string(kMaxBalloonsPerBeat) + " つ分の " +
                                               std::to_string(kMaxBalloonChars * kMaxBalloonsPerBeat) + " 字まで）",
                                           "beat を分けるか短くする"));
                }
            }
            if (core::truthy_at(beat, "reveal") && core::py_mod(core::to_num(page), core::Num(2)) == core::Num(1)) {
                issues.push_back(warning("reveal_on_odd_page", bpath + "/reveal", "reveal が奇数ページ（" + core::py_str(page) + "）にある",
                                         "めくってすぐ見えるのは偶数ページ。偶数ページの先頭に置く"));
            }
        }
    }
    for (std::int64_t page = 1; page <= page_count; ++page) {
        if (std::find(pages_used.begin(), pages_used.end(), page) == pages_used.end()) {
            issues.push_back(warning("page_without_beats", "/scenes", ints(page) + " ページに beat がない"));
        }
    }
    return issues;
}

Json script_index(const Json& script) {
    Json out = Json::object();
    for (const Json& scene : core::iterate(core::py_get(script, "scenes", Json::array()))) {
        const std::vector<Json> beats = core::iterate(core::py_get(scene, "beats", Json::array()));
        for (std::size_t bi = 0; bi < beats.size(); ++bi) {
            const Json id = core::py_get(beats[bi], "id", Json(""));
            out[core::py_str(id)] = Json::object({{"beat", beats[bi]}, {"scene_id", core::py_get(scene, "id")}, {"scene_first", bi == 0}});
        }
    }
    return out;
}

std::vector<Issue> lint_name_plan(const Json& plan, const Json& script, const Json& bible, std::int64_t page_count, bool tall) {
    std::vector<Issue> issues;
    const Json page_value = core::py_get(plan, "page", Json(0));
    const std::int64_t page = core::to_int(page_value);
    if (!(1 <= page && page <= page_count)) {
        return {error("page_out_of_range", "/page", "page " + core::py_str(page_value) + " は範囲外（1〜" + ints(page_count) + "）")};
    }
    for (Issue& issue : layout::check_tiers(plan)) issues.push_back(std::move(issue));
    const Json index = script_index(script);
    std::vector<Json> known;
    for (const Json& c : core::iterate(core::py_get(bible, "characters", Json::array()))) known.push_back(core::py_get(c, "id"));
    std::vector<std::string> page_text_beats;
    for (const auto& [bid, info] : index.items()) {
        if (core::py_equals(core::py_get(info["beat"], "page"), page_value) && text_beat(core::py_get(info["beat"], "kind"))) page_text_beats.push_back(bid);
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> placed;
    std::vector<Json> shots;
    std::vector<std::pair<Json, Json>> sides;
    std::vector<Json> known_props;
    for (const Json& p : core::iterate(core::py_or(core::py_get(bible, "props"), Json::array()))) {
        if (p.is_object()) known_props.push_back(core::py_get(p, "id"));
    }
    const std::vector<Json> panels = core::iterate(core::py_get(plan, "panels", Json::array()));
    for (std::size_t pi = 0; pi < panels.size(); ++pi) {
        const Json& panel = panels[pi];
        const std::string ppath = "/panels/" + std::to_string(pi);
        shots.push_back(core::py_get(panel, "shot", Json("")));
        const std::vector<Json> beat_ids = core::iterate(core::py_get(panel, "beat_ids", Json::array()));
        for (std::size_t bi = 0; bi < beat_ids.size(); ++bi) {
            const Json& bid = beat_ids[bi];
            core::require_hashable(bid);
            const bool found = bid.is_string() && index.contains(bid.get<std::string>());
            if (!found) {
                issues.push_back(error("unknown_beat", ppath + "/beat_ids/" + std::to_string(bi), "beat " + core::py_str(bid) + " が脚本にない"));
            } else {
                const Json there = core::py_get(index[bid.get<std::string>()]["beat"], "page");
                if (!core::py_equals(there, page_value)) {
                    issues.push_back(warning("beat_on_other_page", ppath + "/beat_ids/" + std::to_string(bi),
                                             "beat " + core::py_str(bid) + " は脚本では " + core::py_str(there) + " ページ"));
                }
            }
        }
        const std::vector<Json> chars = core::iterate(core::py_get(panel, "characters", Json::array()));
        for (std::size_t ci = 0; ci < chars.size(); ++ci) {
            const Json id = core::py_get(chars[ci], "id");
            if (!contains(known, id)) {
                issues.push_back(error("unknown_character", ppath + "/characters/" + std::to_string(ci) + "/id", "キャラ " + core::py_str(id) + " が企画書にない"));
            }
        }
        const std::vector<Json> lines = core::iterate(core::py_get(panel, "lines", Json::array()));
        for (std::size_t li = 0; li < lines.size(); ++li) {
            const Json& line = lines[li];
            const std::string lpath = ppath + "/lines/" + std::to_string(li);
            const Json bid = core::py_get(line, "beat_id", Json(""));
            core::require_hashable(bid);
            if (!(bid.is_string() && index.contains(bid.get<std::string>()))) {
                issues.push_back(error("unknown_beat", lpath + "/beat_id", "beat " + core::py_str(bid) + " が脚本にない"));
                continue;
            }
            const Json& info = index[bid.get<std::string>()];
            const std::string key = bid.get<std::string>();
            auto at = std::find_if(placed.begin(), placed.end(), [&key](const auto& item) { return item.first == key; });
            if (at == placed.end()) {
                placed.push_back({key, {}});
                at = placed.end() - 1;
            }
            at->second.push_back(lpath);
            std::string text;
            std::vector<std::string> breaks;
            for (const Json& part : core::iterate(core::py_get(line, "breaks", Json::array()))) {
                breaks.push_back(core::py_str(part));
                text += breaks.back();
            }
            if (core::is_blank(text)) issues.push_back(error("empty_line", lpath + "/breaks", "台詞が空"));
            if (text::length(text) > kMaxBalloonChars) {
                issues.push_back(error("balloon_too_long", lpath + "/breaks",
                                       "フキダシ1つに " + std::to_string(text::length(text)) + " 字（" + std::to_string(kMaxBalloonChars) + " 字まで）", "フキダシを分ける"));
            }
            for (std::size_t ki = 0; ki < breaks.size(); ++ki) {
                if (text::length(breaks[ki]) > kMaxColumnChars) {
                    issues.push_back(error("column_too_long", lpath + "/breaks/" + std::to_string(ki),
                                           "1行が " + std::to_string(text::length(breaks[ki])) + " 字（" + std::to_string(kMaxColumnChars) + " 字まで）",
                                           "breaks で行を分ける"));
                }
            }
            const Json speaker = core::py_get(info["beat"], "speaker_id");
            if (core::py_get(info["beat"], "kind") == Json("dialogue") && core::py_truthy(speaker) && core::truthy_at(panel, "characters")) {
                std::vector<Json> here;
                for (const Json& c : core::iterate(panel["characters"])) here.push_back(core::py_get(c, "id"));
                if (!contains(here, speaker)) {
                    issues.push_back(warning("speaker_off_panel", lpath, "話者 " + core::py_str(speaker) + " がこのコマにいない（フキダシの尾が付かない）"));
                }
            }
        }
        const std::vector<Json> props = core::iterate(core::py_or(core::py_get(panel, "props"), Json::array()));
        for (std::size_t pi2 = 0; pi2 < props.size(); ++pi2) {
            if (!contains(known_props, props[pi2])) {
                issues.push_back(warning("unknown_prop", ppath + "/props/" + std::to_string(pi2), "小物 " + core::py_str(props[pi2]) + " が企画書（props）にない",
                                         "企画書の props に足すと、登録した参照画像が絵の依頼に付く"));
            }
        }
        const std::vector<Json> fx = core::iterate(core::py_or(core::py_get(panel, "fx"), Json::array()));
        for (std::size_t fi = 0; fi < fx.size(); ++fi) {
            if (fxwords::resolve(Json(core::py_str(fx[fi])))) continue;
            const bool listed = std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return i.code == "fx_unknown"; });
            std::string words;
            for (const std::string& word : fxwords::known()) words += (words.empty() ? "" : "・") + word;
            issues.push_back(warning("fx_unknown", ppath + "/fx/" + std::to_string(fi),
                                     "効果の言葉「" + core::py_str(fx[fi]) + "」を Genko は知らない（絵の依頼文には入る。仕上げで Genko は描かない）",
                                     listed ? "知っている言葉: " + words : "知っている言葉は最初の fx_unknown に"));
        }
        collect_sides(sides, panel, index);
    }
    for (const std::string& bid : page_text_beats) {
        if (std::none_of(placed.begin(), placed.end(), [&bid](const auto& item) { return item.first == bid; })) {
            issues.push_back(error("beat_not_placed", "/panels", "台詞の beat " + bid + " がどのコマにも置かれていない"));
        }
    }
    for (const auto& [bid, paths] : placed) {
        if (paths.size() > 1) issues.push_back(error("beat_placed_twice", paths[1], "beat " + bid + " が2回置かれている（最初は " + paths[0] + "）"));
    }
    int run = 1;
    for (std::size_t i = 1; i < shots.size(); ++i) {
        run = core::py_equals(shots[i], shots[i - 1]) ? run + 1 : 1;
        if (run > kMaxSameShotRun) {
            issues.push_back(warning("same_shot_run", "/panels/" + std::to_string(i) + "/shot", "同じ shot（" + core::py_str(shots[i]) + "）が " + std::to_string(run) + " コマ続く",
                                     "寄りと引きを混ぜる"));
        }
    }
    for (std::size_t pi = 0; pi < panels.size(); ++pi) {
        bool first = false;
        for (const Json& bid : core::iterate(core::py_get(panels[pi], "beat_ids", Json::array()))) {
            if (bid.is_string() && index.contains(bid.get<std::string>()) && core::truthy_at(index[bid.get<std::string>()], "scene_first")) first = true;
        }
        const Json shot = core::py_get(panels[pi], "shot");
        const bool establishing = shot.is_string() && std::any_of(std::begin(kEstablishing), std::end(kEstablishing), [&shot](const char* s) { return shot == s; });
        if (first && !establishing) {
            issues.push_back(warning("scene_without_establishing", "/panels/" + std::to_string(pi) + "/shot", "場面の最初のコマが状況説明のショット（ELS / LS / FS）でない"));
        }
    }
    for (const auto& [scene_id, positions] : sides) {
        if (core::truthy_at(positions, "_crossed")) {
            issues.push_back(warning("axis_crossed", "/panels", "場面 " + core::py_str(scene_id) + " で人物の左右が入れ替わる（180度ルール）", "意図的なら cross:true を付ける"));
        }
    }
    for (Issue& issue : size_hints(plan, page, page_count, tall)) issues.push_back(std::move(issue));
    if (core::truthy_at(plan, "title")) {
        if (core::py_strip(core::py_str(core::py_or(core::py_get(bible, "title"), Json("")))).empty()) {
            issues.push_back(warning("title_missing", "/title", "扉にするページだが、企画書に題名（title）が無い"));
        }
        if (core::py_strip(core::py_str(core::py_or(core::py_get(bible, "author"), Json("")))).empty()) {
            issues.push_back(warning("author_missing", "/title", "扉にするページだが、企画書に作者名（author）が無い", "set_bible で author を足す"));
        }
    }
    const Json role = core::py_get(plan, "turn_role");
    if (role == Json("reveal") && page % 2 == 1) issues.push_back(warning("reveal_on_odd_page", "/turn_role", "reveal は偶数ページ（めくってすぐ見えるページ）に置く"));
    if (role == Json("hook") && page % 2 == 0) issues.push_back(warning("hook_on_even_page", "/turn_role", "hook（引き）は奇数ページの最後に置く"));
    return issues;
}

std::vector<Issue> lint_name_geometry(const Json& plan, const layout::CompiledLayout& compiled, const std::vector<letter::BalloonPlacement>& placements) {
    std::vector<Issue> issues;
    std::vector<std::pair<Json, std::string>> panel_path;
    const std::vector<Json> panels = core::iterate(core::py_get(plan, "panels", Json::array()));
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const Json slot = core::py_get(panels[i], "slot");
        const auto at = std::find_if(panel_path.begin(), panel_path.end(), [&slot](const auto& item) { return core::py_equals(item.first, slot); });
        if (at != panel_path.end()) {
            at->second = "/panels/" + std::to_string(i);
        } else {
            panel_path.push_back({slot, "/panels/" + std::to_string(i)});
        }
    }
    for (const auto& [slot, rect] : compiled.leaf_rects_mm) {
        std::string path = "/panels";
        for (const auto& [key, where] : panel_path) {
            if (key == Json(slot)) path = where;
        }
        const double w = rect[2].value();
        const double h = rect[3].value();
        if (core::py_min(w, h) < kMinPanelSideMm) {
            issues.push_back(warning("panel_too_small", path, "コマ " + slot + " が " + fixed(rect[2], 0) + "×" + fixed(rect[3], 0) + " mm（短辺 " + fixed(kMinPanelSideMm, 0) + " mm 未満）",
                                     "段の h か列の w を大きくする"));
        }
        std::vector<double> areas;
        for (const auto& p : placements) {
            if (p.slot == slot) areas.push_back(p.w_mm * p.h_mm);
        }
        const double area = core::py_float_sum(areas);
        if (w * h > 0 && area / (w * h) > kBalloonAreaRatio) {
            issues.push_back(warning("panel_too_wordy", path, "コマ " + slot + " の " + percent(area / (w * h), 0) + " が台詞で埋まる（" + percent(kBalloonAreaRatio, 0) + " まで）",
                                     "台詞を減らすかコマを大きくする"));
        }
    }
    return issues;
}

}  // namespace genko::service::lint
