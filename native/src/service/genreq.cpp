#include "service/genreq.hpp"

#include <algorithm>
#include <limits>
#include <cmath>

#include "core/frames.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/studio.hpp"
#include "formats/pillow_save.hpp"
#include "render/text/fonts.hpp"
#include "service/blocking.hpp"
#include "service/briefs.hpp"
#include "service/data.hpp"
#include "service/fxwords.hpp"
#include "service/guide.hpp"
#include "service/book.hpp"
#include "service/pyfmt.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "studio/files.hpp"

namespace genko::service::genreq {

namespace fs = std::filesystem;
using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

constexpr const char* kPurposes[] = {"panel_art", "draft", "character_sheet", "location"};
constexpr const char* kModes[] = {"new", "edit", "inpaint", "upscale"};

// sep.join(items) of texts (TypeError "sequence item i: expected str instance, … found" for another value).
std::string join(const std::vector<Json>& items, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].is_string()) {
            throw core::PyTypeError("sequence item " + std::to_string(i) + ": expected str instance, " + core::py_type_name(items[i]) + " found");
        }
        out += (i == 0 ? "" : sep) + items[i].get<std::string>();
    }
    return out;
}

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) out += (i == 0 ? "" : sep) + items[i];
    return out;
}

// [p for p in values if p]
std::vector<Json> truthy(std::initializer_list<Json> values) {
    std::vector<Json> out;
    for (const Json& v : values) {
        if (py_truthy(v)) out.push_back(v);
    }
    return out;
}

// text.rstrip(chars) for one character (UTF-8)
std::string rstrip(std::string text, const std::string& ch) {
    while (text.size() >= ch.size() && text.compare(text.size() - ch.size(), ch.size(), ch) == 0) text.erase(text.size() - ch.size());
    return text;
}

// text.rstrip(): Python's whitespace taken off the end.
std::string py_rstrip(const std::string& text) {
    std::u32string chars = render::text::u32(text);
    while (!chars.empty() && render::text::py_isspace(chars.back())) chars.pop_back();
    return render::text::utf8(chars);
}

// text.split(): the words between runs of Python's whitespace.
std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> out;
    std::u32string current;
    for (char32_t c : render::text::u32(text)) {
        if (render::text::py_isspace(c)) {
            if (!current.empty()) out.push_back(render::text::utf8(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(render::text::utf8(current));
    return out;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

// d.get(key, {}) of a dict keyed by texts (the vocab's sections; any hashable key Python would look up)
Json section(const Json& dict, const Json& key) {
    core::require_hashable(key);
    if (key.is_string() && dict.is_object()) {
        const auto it = dict.find(key.get<std::string>());
        if (it != dict.end()) return *it;
    }
    return Json::object();
}

Json artwork(const Json& style) {
    std::string ja = style["ja"].get<std::string>();
    ja = replace_all(replace_all(replace_all(ja, "漫画のコマ", "漫画の絵"), "マンガのコマ", "マンガの絵"), "ウェブトゥーンのコマ", "ウェブトゥーンの絵");
    std::string en = style["en"].get<std::string>();
    en = replace_all(replace_all(en, "manga panel", "manga artwork"), "webtoon panel", "webtoon artwork");
    return Json::object({{"ja", ja}, {"en", en}, {"tags", style["tags"]}});
}

std::optional<std::string> asset_bytes(const storage::AssetStore& store, const Json& ref) {
    if (!ref.is_string() || ref.get<std::string>().rfind("sha256:", 0) != 0) return std::nullopt;
    return store.get_bytes(ref.get<std::string>(), ".png");
}

void catalog_sample(const core::Document& doc, const storage::AssetStore& store, const std::string& expression,
                    std::map<std::string, std::string>& files, std::vector<std::string>& refs, std::vector<std::string>& notes) {
    const Json kept = briefs::catalog(doc);
    if (kept.is_null()) return;
    if (!briefs::catalog_fits(kept, expression)) {
        notes.push_back("原稿の絵柄（" + py_str(py_get(kept, "title")) + "）は白黒用なので、カラーのページには使っていない。カラーの絵柄は人に確かめる");
        return;
    }
    const auto data = asset_bytes(store, core::py_or(py_get(kept, "sample"), Json("")));
    if (!data) return;
    files["refs/style_catalog.png"] = *data;
    refs.push_back("refs/style_catalog.png");
    const Json path = core::py_or(py_get(kept, "path"), Json::array({py_get(kept, "title", Json(""))}));
    notes.push_back("refs/style_catalog.png は原稿の絵柄「" + join(core::iterate(path), "・") +
                    "」の見本。線・ベタとトーンの量・目や髪の描き方・頭身を合わせる。人物・服・構図は真似しない");
}

std::string aspect_text(double aspect) {
    return aspect >= 1 ? fixed(aspect, 2) + ":1" : "1:" + fixed(1 / aspect, 2);
}

Json tool_sizes(double aspect, const Json& sizes) {
    struct Ranked {
        double closeness;
        double area;
        Json entry;
    };
    std::vector<Ranked> ranked;
    for (const Json& wh : core::iterate(sizes)) {
        const auto pair = core::unpack_values(wh, 2);
        const double w = core::to_float(pair[0]);
        const double h = core::to_float(pair[1]);
        const double tool_aspect = w / h;
        std::string crop = "そのまま";
        if (tool_aspect > aspect * 1.02) {
            crop = "左右を中央で切る";
        } else if (tool_aspect < aspect / 1.02) {
            crop = "上下を中央で切る";
        }
        ranked.push_back(Ranked{core::py_round(std::abs(std::log(tool_aspect / aspect)), 6), -w * h,
                                Json::object({{"px", Json::array({pair[0], pair[1]})}, {"crop", crop}})});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.closeness != b.closeness ? a.closeness < b.closeness : a.area < b.area;
    });
    Json out = Json::array();
    for (std::size_t i = 0; i < ranked.size() && i < 3; ++i) out.push_back(ranked[i].entry);
    return out;
}

Json size_block(const Num& width_mm, const Num& height_mm, double pad_mm, const Num& dpi, const Json& tool, const Json& mode) {
    const double box_w = width_mm.value() + 2 * pad_mm;
    const double box_h = height_mm.value() + 2 * pad_mm;
    const double aspect = box_w / box_h;
    const Json print_px = Json::array({core::py_round_int(box_w / 25.4 * dpi.value()), core::py_round_int(box_h / 25.4 * dpi.value())});
    Json block = Json::object({{"frame_mm", Json::array({core::py_round(width_mm, 2).json(), core::py_round(height_mm, 2).json()})},
                               {"pad_mm", pad_mm},
                               {"aspect", aspect_text(aspect)},
                               {"suggested_px", suggested_px(aspect)},
                               {"tool_sizes", tool_sizes(aspect, util::item(tool, "sizes_px"))},
                               {"print_px", Json::object({{"dpi", dpi.json()}, {"px", print_px}})}});
    if (core::py_equals(mode, Json("upscale"))) block["suggested_px"] = print_px;
    return block;
}

std::string character_ja(const Json& ch, const Json& outfit_id, const Json& v) {
    const Json look = util::or_dict(py_get(ch, "look"));
    std::vector<Json> parts{py_get(look, "build", Json("")), py_get(look, "hair", Json(""))};
    const Json hair_value = py_get(look, "hair_value");
    const Json values = v["hair_value"];
    core::require_hashable(hair_value);
    if (hair_value.is_string() && values.contains(hair_value.get<std::string>())) parts.push_back(values[hair_value.get<std::string>()]["ja"]);
    if (core::truthy_at(look, "eyes")) parts.push_back("目は" + py_str(look["eyes"]));
    PyDict outfits;
    for (const Json& o : core::iterate(py_get(look, "outfits", Json::array()))) outfits.set(py_get(o, "id"), py_get(o, "desc", Json("")));
    const Json* wanted = outfits.find(core::py_or(outfit_id, Json("default")));
    Json outfit = wanted != nullptr ? *wanted : Json();
    if (!py_truthy(outfit)) outfit = outfits.items().empty() ? Json("") : outfits.items().front().second;
    if (py_truthy(outfit)) parts.push_back(outfit);
    for (const Json& m : core::iterate(py_get(look, "marks", Json::array()))) parts.push_back(m);
    std::vector<Json> kept;
    for (const Json& p : parts) {
        if (py_truthy(p)) kept.push_back(p);
    }
    const std::string desc = join(kept, "、");
    const Json name = py_get(ch, "name", py_get(ch, "id"));
    return desc.empty() ? py_str(name) : py_str(name) + "（" + desc + "）";
}

std::string character_en(const Json& ch, const Json& v) {
    if (core::truthy_at(ch, "tokens_en")) return py_str(ch["tokens_en"]);
    const Json look = util::or_dict(py_get(ch, "look"));
    const Json value = py_get(section(v["hair_value"], py_get(look, "hair_value")), "en", Json(""));
    return "the character '" + py_str(py_get(ch, "id")) + "' as in the reference sheet" + (py_truthy(value) ? ", " + py_str(value) : "");
}

std::pair<std::string, std::string> keepout_sentence(const std::vector<Json>& boxes01) {
    if (boxes01.empty()) return {"", ""};
    const auto at = [](const Json& b, int i) { return b[i].get<double>(); };
    double x0 = at(boxes01[0], 0), y0 = at(boxes01[0], 1);
    double x1 = at(boxes01[0], 0) + at(boxes01[0], 2), y1 = at(boxes01[0], 1) + at(boxes01[0], 3);
    for (const Json& b : boxes01) {
        x0 = core::py_min(x0, at(b, 0));
        y0 = core::py_min(y0, at(b, 1));
        x1 = core::py_max(x1, at(b, 0) + at(b, 2));
        y1 = core::py_max(y1, at(b, 1) + at(b, 3));
    }
    const double cx = (x0 + x1) / 2;
    const double cy = (y0 + y1) / 2;
    std::string horiz_ja, horiz_en, vert_ja, vert_en;
    if (cx < 0.4) {
        horiz_ja = "左", horiz_en = "left";
    } else if (cx > 0.6) {
        horiz_ja = "右", horiz_en = "right";
    }
    if (cy < 0.4) {
        vert_ja = "上", vert_en = "upper";
    } else if (cy > 0.6) {
        vert_ja = "下", vert_en = "lower";
    }
    std::string where_ja = horiz_ja + vert_ja;
    if (where_ja.empty()) where_ja = "中央";
    std::string where_en = vert_en.empty() ? horiz_en : horiz_en.empty() ? vert_en : vert_en + "-" + horiz_en;
    if (where_en.empty()) where_en = "central";
    const double share = core::py_min(0.9, core::py_max(0.1, (x1 - x0) * (y1 - y0)));
    const std::int64_t tenths = std::max<std::int64_t>(1, core::py_round_int(share * 10));
    return {where_ja + "の約" + std::to_string(tenths) + "割は台詞用に静かに空けておく。",
            "Keep the " + where_en + " area (about " + std::to_string(tenths * 10) + "% of the image) calm and empty for speech balloons."};
}

Json location_of(const core::Document& doc, const Json& location_id) {
    if (!py_truthy(location_id)) return Json();
    const Json bible_doc = util::or_dict(py_get(doc.studio, "bible_doc"));
    for (const Json& item : core::iterate(py_get(doc.studio, "locations", Json::array()))) {
        if (!core::py_equals(py_get(item, "id"), location_id)) continue;
        Json base = Json::object();
        for (const Json& loc : core::iterate(py_get(bible_doc, "locations", Json::array()))) {
            if (core::py_equals(py_get(loc, "id"), location_id)) {
                base = loc;
                break;
            }
        }
        return util::merged(base, item);
    }
    for (const Json& item : core::iterate(py_get(bible_doc, "locations", Json::array()))) {
        if (core::py_equals(py_get(item, "id"), location_id)) return item;
    }
    return Json();
}

std::pair<Json, std::vector<std::string>> panel_prompt(const core::Document& doc, const core::Page& page, const core::Frame& frame,
                                                       const std::vector<Json>& keep01, const Json& instruction) {
    const Json v = book_vocab(doc, page.spec.expression);
    const Json panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
    const Json& studio = doc.studio;
    const Json bible_doc = util::or_dict(py_get(studio, "bible_doc"));
    PyDict chars;
    for (const Json& c : core::iterate(doc.bible.characters)) chars.set(py_get(c, "id"), c);
    std::vector<std::string> notes;
    std::vector<std::string> ja{v["style"]["ja"].get<std::string>()};
    std::vector<std::string> en{v["style"]["en"].get<std::string>()};
    std::vector<Json> tags{v["style"]["tags"]};
    Json style_notes = py_get(util::or_dict(py_get(studio, "style")), "notes");
    if (!py_truthy(style_notes)) style_notes = py_get(util::or_dict(py_get(bible_doc, "style")), "notes");
    for (const Json& n : core::iterate(core::py_or(style_notes, Json::array()))) ja.push_back(rstrip(py_str(n), "。") + "。");
    std::vector<Json> cast;
    for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
        if (c.is_object()) cast.push_back(c);
    }
    for (const Json& c : cast) {
        const Json* found = chars.find(py_get(c, "id"));
        const Json ch = found != nullptr ? *found : Json::object({{"id", py_get(c, "id")}});
        ja.push_back(character_ja(ch, py_get(c, "outfit"), v) + "。");
        en.push_back(character_en(ch, v) + ".");
        const Json value = py_get(section(v["hair_value"], py_get(util::or_dict(py_get(ch, "look")), "hair_value")), "tags");
        if (py_truthy(value)) tags.push_back(value);
    }
    const Json shot = section(v["shot"], py_get(panel, "shot", Json("")));
    const Json angle = section(v["angle"], py_get(panel, "angle", Json("")));
    if (py_truthy(shot) || py_truthy(angle)) {
        ja.push_back(join(truthy({py_get(angle, "ja"), py_get(shot, "ja")}), "、") + "の構図。");
        en.push_back(join(truthy({py_get(angle, "en"), py_get(shot, "en")}), ", ") + ".");
        for (const Json& t : truthy({py_get(shot, "tags"), py_get(angle, "tags")})) tags.push_back(t);
    }
    PyDict known_props;
    for (const Json& p : core::iterate(py_get(studio, "props", Json::array()))) known_props.set(py_get(p, "id"), p);
    for (const Json& pid : core::iterate(core::py_or(py_get(panel, "props"), Json::array()))) {
        const Json* prop = known_props.find(pid);
        if (prop == nullptr || prop->is_null()) continue;
        ja.push_back("小物「" + py_str(py_get(*prop, "name", pid)) + "」: " + py_str(py_get(*prop, "desc", Json(""))) +
                     "（参照画像があれば、形と柄をそのまま）。");
        const Json tokens = py_get(*prop, "tokens_en");
        en.push_back("Prop " + py_str(pid) + ": " +
                     (py_truthy(tokens) ? py_str(tokens) : py_str(py_get(*prop, "desc", Json(""))) + " (in Japanese)") +
                     ", exactly as in its reference.");
    }
    const Json location = location_of(doc, py_get(panel, "location_id"));
    const Json time = section(v["time"], core::py_or(py_get(panel, "time"), Json("")));
    if (py_truthy(location)) {
        const Json name = py_get(location, "name", py_get(location, "id"));
        ja.push_back("場所は" + py_str(name) + "（" + py_str(py_get(location, "desc", Json(""))) + "）" +
                     (py_truthy(time) ? "、" + py_str(time["ja"]) : std::string()) + "。");
        const Json name_en = py_get(location, "name_en");
        const std::string where_en = py_truthy(name_en) ? py_str(name_en)
                                                         : py_str(name) + " (in Japanese: " + py_str(py_get(location, "desc", Json(""))) + ")";
        en.push_back("Setting: " + where_en + (py_truthy(time) ? ", " + py_str(time["en"]) : std::string()) + ".");
        if (py_truthy(time)) tags.push_back(time["tags"]);
    }
    for (const Json& c : cast) {
        const Json* found = chars.find(py_get(c, "id"));
        const Json ch = found != nullptr ? *found : Json::object();
        const Json name = py_get(ch, "name", py_get(c, "id"));
        const Json pos = section(v["pos"], py_get(c, "pos", Json("")));
        const Json facing = section(v["facing"], py_get(c, "facing", Json("")));
        const std::vector<Json> bits = truthy({py_get(pos, "ja"), py_get(facing, "ja"), py_get(c, "pose"), py_get(c, "expression")});
        if (!bits.empty()) ja.push_back(py_str(name) + ": " + join(bits, "、") + "。");
        std::vector<Json> en_bits = truthy({py_get(pos, "en"), py_get(facing, "en")});
        const std::vector<Json> jp_bits = truthy({py_get(c, "pose"), py_get(c, "expression")});
        if (!jp_bits.empty()) en_bits.push_back("pose/expression (in Japanese): " + join(jp_bits, "、"));
        if (!en_bits.empty()) en.push_back(py_str(py_get(c, "id")) + ": " + join(en_bits, ", ") + ".");
        if (core::truthy_at(facing, "tags")) tags.push_back(facing["tags"]);
    }
    if (core::truthy_at(panel, "action")) {
        ja.push_back(py_str(panel["action"]) + "。");
        en.push_back("Action (in Japanese): " + py_str(panel["action"]) + ".");
    }
    if (core::truthy_at(panel, "emotion")) {
        ja.push_back("感情: " + py_str(panel["emotion"]) + "。");
        en.push_back("Mood (in Japanese): " + py_str(panel["emotion"]) + ".");
    }
    const auto [fx_ja, fx_en] = fxwords::art_words(core::py_or(py_get(panel, "fx"), Json::array()));
    if (!fx_ja.empty()) {
        ja.push_back("効果: " + join(fx_ja, "、") + "。");
        en.push_back("Effects: " + join(fx_en, "; ") + ".");
    }
    if (const auto weight = fxwords::emphasis_words(py_get(panel, "emphasis"))) {
        ja.push_back(weight->first);
        en.push_back(weight->second);
    }
    const auto [keep_ja, keep_en] = keepout_sentence(keep01);
    if (!keep_ja.empty()) {
        ja.push_back(keep_ja);
        en.push_back(keep_en);
    }
    for (const Json& c : core::iterate(doc.bible.constraints)) ja.push_back(rstrip(py_str(c), "。") + "。");
    const Json plan = page.plan && py_truthy(*page.plan) ? *page.plan : Json::object();
    const auto leaves = page.leaf_frames();
    if (core::py_equals(py_get(plan, "turn_role"), Json("reveal")) && !leaves.empty() && frame.id == leaves.front()->id) {
        ja.push_back("めくってすぐの見せ場のコマ。");
        en.push_back("The big reveal right after the page turn.");
    } else if (core::py_equals(py_get(plan, "turn_role"), Json("hook")) && !leaves.empty() && frame.id == leaves.back()->id) {
        ja.push_back("次のページへ引く最後のコマ。");
        en.push_back("The last panel of the page, a hook into the next page.");
    }
    if (core::truthy_at(panel, "memo")) ja.push_back(py_str(panel["memo"]));
    const Json human = py_get(panel, "instruction");
    if (human.is_object() && core::truthy_at(human, "text")) {
        ja.push_back("指示: " + py_str(human["text"]));
    } else if (human.is_string() && !human.get<std::string>().empty()) {
        ja.push_back("指示: " + human.get<std::string>());
    }
    if (py_truthy(instruction)) ja.push_back("今回の指示: " + py_str(instruction));
    if (panel.contains("action") || panel.contains("emotion") || py_truthy(instruction) || py_truthy(human)) {
        notes.push_back("英語のプロンプトの (in Japanese) の部分は日本語のまま入っている。英語で受ける画像ツールには訳して渡す");
    }
    bool all_tokens = true;
    for (const Json& c : cast) {
        const Json* found = chars.find(py_get(c, "id"));
        all_tokens = all_tokens && found != nullptr && core::truthy_at(*found, "tokens_en");
    }
    if (!all_tokens) notes.push_back("英語の見た目の記述（tokens_en）が無い人物がいる。英語のプロンプトは参照画像と日本語から補う");
    std::vector<Json> unique_tags;
    for (const Json& t : tags) {
        if (!py_truthy(t)) continue;
        core::require_hashable(t);
        if (std::none_of(unique_tags.begin(), unique_tags.end(), [&t](const Json& u) { return core::py_equals(u, t); })) unique_tags.push_back(t);
    }
    Json prompt = Json::object({{"ja", join(ja, "")}, {"en", join(en, " ")}, {"tags", join(unique_tags, ", ")}});
    const Json override_ = py_get(util::or_dict(py_get(panel, "gen")), "prompt_override");
    if (py_truthy(override_)) {
        prompt = override_.is_object() ? override_
                                       : Json::object({{"ja", py_str(override_)}, {"en", py_str(override_)}, {"tags", ""}});
        notes.push_back("プロンプトは人間の指定なので変えない");
    }
    return {std::move(prompt), std::move(notes)};
}

Json avoid_of(const core::Document& doc, const std::vector<Json>& char_ids, const Json& panel, const std::string& expression) {
    const Json v = book_vocab(doc, expression);
    const Json override_ = py_get(util::or_dict(py_get(util::or_dict(panel), "gen")), "avoid_override");
    if (py_truthy(override_)) {
        Json out = Json::array();
        for (const Json& x : core::iterate(override_)) out.push_back(py_str(x));
        return out;
    }
    Json out = v["avoid"];
    PyDict chars;
    for (const Json& c : core::iterate(doc.bible.characters)) chars.set(py_get(c, "id"), c);
    for (const Json& cid : char_ids) {
        const Json* ch = chars.find(cid);
        for (const Json& item : core::iterate(core::py_or(py_get(ch != nullptr ? *ch : Json::object(), "never", Json::array()), Json::array()))) {
            out.push_back(py_str(item) + "（" + py_str(cid) + "）");
        }
    }
    return out;
}

std::pair<const core::Page*, const core::Frame*> page_frame(const core::Document& doc, const Json& page, const Json& frame_id) {
    const core::Page* found = nullptr;
    if (!page.is_null()) {
        const Json wanted = Json(core::to_int(page));
        if (const auto at = page_at(doc, wanted)) found = &doc.page(*at);
    }
    if (found == nullptr) throw RequestError("ページ " + py_str(core::py_or(page, Json())) + " はない", "/page");
    const core::Frame* frame = nullptr;
    try {
        frame = &briefs::find_frame(*found, Json(py_str(core::py_or(frame_id, Json("")))));
    } catch (const core::PyUncaught& e) {
        if (e.type() != "KeyError" && e.type() != "IndexError") throw;
        throw RequestError(found->index.repr() + " ページにコマ " + py_str(frame_id) + " はない", "/frame_id");
    }
    if (!frame->children.empty()) throw RequestError("コマ（葉）を指定する", "/frame_id");
    return {found, frame};
}

Json candidate_of(const core::Document& doc, const core::Frame* frame, const Json& candidate_id) {
    if (!py_truthy(candidate_id)) return Json();
    std::vector<Json> pools;
    if (frame != nullptr) pools.push_back(py_get(frame->panel ? *frame->panel : Json::object(), "candidates", Json::array()));
    for (const char* bucket : {"character_candidates", "location_candidates"}) {
        const Json all = py_get(doc.studio, bucket, Json::object());
        for (const auto& [key, pool] : all.items()) {
            (void)key;
            pools.push_back(pool);
        }
    }
    for (const Json& pool : pools) {
        for (const Json& cand : core::iterate(pool)) {
            if (core::py_equals(py_get(cand, "id"), candidate_id)) return cand;
        }
    }
    return Json();
}

std::vector<std::string> panel_refs(const core::Document& doc, const storage::AssetStore& store, const Json& panel,
                                    const std::vector<Json>& cast, std::map<std::string, std::string>& files) {
    std::vector<std::string> names;
    PyDict chars;
    for (const Json& c : core::iterate(doc.bible.characters)) chars.set(py_get(c, "id"), c);
    const auto put = [&](const std::string& name, const std::string& data) {
        files[name] = data;
        names.push_back(name);
    };
    for (const Json& cid : cast) {
        const Json* ch = chars.find(cid);
        for (const Json& ref : core::iterate(py_get(ch != nullptr ? *ch : Json::object(), "refs", Json::array()))) {
            if (!core::is_one_of(py_get(ref, "kind"), {"face", "sheet"})) continue;
            if (const auto data = asset_bytes(store, py_get(ref, "asset", Json("")))) put("refs/" + py_str(cid) + "_" + py_str(ref["kind"]) + ".png", *data);
        }
    }
    const Json location = location_of(doc, py_get(panel, "location_id"));
    for (const Json& ref : core::iterate(py_get(util::or_dict(location), "refs", Json::array()))) {
        if (const auto data = asset_bytes(store, py_get(ref, "asset", Json("")))) {
            put("refs/" + py_str(location["id"]) + "_" + ref["asset"].get<std::string>().substr(7, 8) + ".png", *data);
        }
    }
    PyDict props;
    for (const Json& p : core::iterate(py_get(doc.studio, "props", Json::array()))) props.set(py_get(p, "id"), p);
    for (const Json& pid : core::iterate(core::py_or(py_get(panel, "props"), Json::array()))) {
        const Json* prop = props.find(pid);
        for (const Json& ref : core::iterate(py_get(util::or_dict(prop != nullptr ? *prop : Json()), "refs", Json::array()))) {
            if (const auto data = asset_bytes(store, py_get(ref, "asset", Json("")))) {
                put("refs/prop_" + py_str(pid) + "_" + ref["asset"].get<std::string>().substr(7, 8) + ".png", *data);
            }
        }
    }
    std::vector<Json> refs = core::iterate(py_get(panel, "refs", Json::array()));
    std::stable_sort(refs.begin(), refs.end(), [](const Json& a, const Json& b) {
        return core::py_less(py_get(a, "order", Json(0)), py_get(b, "order", Json(0)));
    });
    for (const Json& ref : refs) {
        const Json source = util::or_dict(py_get(ref, "source"));
        if (!core::py_equals(py_get(source, "kind"), Json("asset"))) continue;
        if (const auto data = asset_bytes(store, py_get(source, "asset", Json("")))) {
            put("refs/" + py_str(py_get(ref, "id")) + "_" + py_str(py_get(ref, "role", Json("ref"))) + ".png", *data);
        }
    }
    return names;
}

std::optional<std::pair<std::string, std::string>> previous_art(const core::Document& doc, const storage::AssetStore& store,
                                                                const core::Frame& frame) {
    const Json panel = frame.panel ? *frame.panel : Json::object();
    std::vector<const core::Page*> pages;
    for (const auto& p : doc.pages) pages.push_back(p.get());
    std::stable_sort(pages.begin(), pages.end(), [](const core::Page* a, const core::Page* b) { return a->index < b->index; });
    std::vector<std::pair<const core::Page*, const core::Frame*>> order;
    for (const core::Page* p : pages) {
        for (const core::Frame* f : p->leaf_frames()) order.emplace_back(p, f);
    }
    std::size_t at = order.size();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i].second->id == frame.id) {
            at = i;
            break;
        }
    }
    if (at == order.size() || at == 0) return std::nullopt;
    const auto [before_page, before] = order[at - 1];
    const Json other = before->panel ? *before->panel : Json::object();
    if (!core::truthy_at(panel, "location_id") || !core::py_equals(py_get(other, "location_id"), py_get(panel, "location_id"))) return std::nullopt;
    const Json adopted = py_get(util::or_dict(py_get(other, "adopted")), "art");
    for (const Json& c : core::iterate(py_get(other, "candidates", Json::array()))) {
        if (!core::py_equals(py_get(c, "id"), adopted)) continue;
        if (const auto data = asset_bytes(store, py_get(c, "asset", Json("")))) {
            return std::pair{*data, before_page->index.repr() + " ページ " + py_str(core::py_or(py_get(other, "slot"), Json(before->id)))};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<guide::Box> mask_rects(const core::Frame& frame, const Json& regions) {
    const Json all = py_get(frame.panel ? *frame.panel : Json::object(), "regions", Json::array());
    PyDict by_id;
    for (const Json& r : core::iterate(all)) by_id.set(py_get(r, "id"), r);
    const auto rect = [](const Json& value) {
        const std::vector<double> v = core::unpack_floats(value, 4);
        return guide::Box{v[0], v[1], v[2], v[3]};
    };
    std::vector<guide::Box> out;
    for (const Json& item : core::iterate(core::py_or(regions, Json::array()))) {
        const Json* named = item.is_string() ? by_id.find(item) : nullptr;
        if (item.is_string() && item.get<std::string>().find(':') != std::string::npos && named == nullptr) {
            const std::string text = item.get<std::string>();
            const std::string kind = text.substr(0, text.find(':'));
            const std::string ch = text.substr(text.find(':') + 1);
            const bool face = kind == "face";
            for (const Json& r : core::iterate(all)) {
                const Json k = py_get(r, "kind");
                const bool fits = face ? core::is_one_of(k, {"face", "head"}) : core::is_one_of(k, {"person", "body"});
                if (fits && core::py_equals(py_get(r, "char"), Json(ch)) && core::truthy_at(r, "rect_mm")) out.push_back(rect(r["rect_mm"]));
            }
        } else if (named != nullptr && core::truthy_at(*named, "rect_mm")) {
            out.push_back(rect((*named)["rect_mm"]));
        } else if (item.is_array() && item.size() == 4) {
            out.push_back(rect(item));
        }
    }
    return out;
}

std::vector<std::string> by_priority(std::vector<std::string> names) {
    const auto rank = [](const std::string& name) {
        if (name.ends_with("_face.png")) return 0;
        if (name.ends_with("_sheet.png")) return 1;
        if (name == "refs/style_pilot.png" || name == "refs/style_catalog.png") return 2;
        if (name == "refs/previous_panel.png") return 3;
        if (name.rfind("refs/prop_", 0) == 0) return 4;
        return 5;
    };
    std::stable_sort(names.begin(), names.end(), [&rank](const std::string& a, const std::string& b) {
        const int ra = rank(a);
        const int rb = rank(b);
        return ra != rb ? ra < rb : a < b;
    });
    return names;
}

std::optional<std::string> reference_sheet(const std::map<std::string, std::string>& files, const std::vector<std::string>& names,
                                           const Json& size_px) {
    std::vector<std::string> picked;
    for (const std::string& n : names) {
        if (files.count(n) != 0 && picked.size() < 3) picked.push_back(n);
    }
    if (picked.size() < 2) return std::nullopt;
    const auto wh = core::int_tuple(size_px);
    // (the tool's size as Python's ints, in doubles: a size past an int's range keeps its shape, not its low bits)
    const double asked_w = static_cast<double>(wh.at(0));
    const double asked_h = static_cast<double>(wh.at(1));
    const double scale = 1024.0 / std::max(asked_w, asked_h);
    const int width = std::max(256, static_cast<int>(core::py_round_int(asked_w * scale)));
    const int height = std::max(256, static_cast<int>(core::py_round_int(asked_h * scale)));
    render::Image sheet = render::Image::create("RGB", render::Size{width, height}, render::Ink{255, 255, 255});
    const int gap = std::max(8, width / 64);
    const bool wide = width >= height;
    const int n = static_cast<int>(picked.size());
    const int slot_w = wide ? static_cast<int>(core::py_floor((width - gap * (n + 1)) / static_cast<double>(n))) : width - 2 * gap;
    const int slot_h = wide ? height - 2 * gap : static_cast<int>(core::py_floor((height - gap * (n + 1)) / static_cast<double>(n)));
    for (int i = 0; i < n; ++i) {
        render::Image ref = open_picture(files.at(picked[static_cast<std::size_t>(i)])).convert("RGB");
        ref.thumbnail(render::Size{slot_w, slot_h});
        const int x0 = wide ? gap + i * (slot_w + gap) : gap;
        const int y0 = wide ? gap : gap + i * (slot_h + gap);
        const int px = x0 + static_cast<int>(core::py_floor((slot_w - ref.width()) / 2.0));
        const int py = y0 + static_cast<int>(core::py_floor((slot_h - ref.height()) / 2.0));
        sheet.paste(ref, render::Point{px, py});
    }
    formats::PngSave save;
    save.optimize = true;
    return formats::png_bytes(sheet, save);
}

}  // namespace

Json vocab(const std::string& expression) {
    Json v = data::vocab();
    if (expression == "color") {
        v["style"] = v["style_color"];
        Json kept = Json::array();
        for (const Json& a : v["avoid"]) {
            if (!(a == "色")) kept.push_back(a);
        }
        v["avoid"] = std::move(kept);
    }
    return v;
}

Json book_vocab(const core::Document& doc, const std::string& expression) {
    Json v = vocab(expression);
    const Json kept = briefs::catalog(doc);
    if (kept.is_null() || !briefs::catalog_fits(kept, expression)) return v;
    const Json words = kept["prompt"];
    const std::string ja = py_rstrip(py_str(words["ja"]));
    const Json en = core::py_or(py_get(words, "en"), words["ja"]);
    v["style"] = Json::object({{"ja", ja + (ja.ends_with("。") ? "" : "。")},
                               {"en", py_rstrip(py_str(en))},
                               {"tags", py_str(core::py_or(py_get(words, "tags"), Json("")))}});
    const auto key = [](const std::string& word) { return core::py_strip(replace_all(replace_all(word, "の", ""), "線", "")); };
    std::vector<std::string> known;
    for (const Json& item : v["avoid"]) {
        for (const std::string& a : split_words(replace_all(item.get<std::string>(), "・", " "))) known.push_back(key(a));
    }
    for (const Json& a : v["avoid"]) known.push_back(key(a.get<std::string>()));
    for (const Json& a : core::iterate(core::py_or(py_get(kept, "avoid"), Json::array()))) {
        const std::string word = py_str(a);
        if (std::find(known.begin(), known.end(), key(word)) != known.end()) continue;
        if (expression == "color" && core::py_equals(a, Json("色"))) continue;
        v["avoid"].push_back(a);
    }
    return v;
}

Json suggested_px(double aspect, std::int64_t pixels) {
    const double width = std::sqrt(static_cast<double>(pixels) * aspect);
    const double height = width / aspect;
    return Json::array({std::max<std::int64_t>(256, core::py_round_int(width / 64) * 64), std::max<std::int64_t>(256, core::py_round_int(height / 64) * 64)});
}

Pack build(const core::Document& doc, const fs::path& project, Options o) {
    if (!core::is_one_of(o.purpose, {"panel_art", "draft", "character_sheet", "location"})) {
        throw RequestError("purpose は " + join(std::vector<std::string>(std::begin(kPurposes), std::end(kPurposes)), ", ") + " のどれか", "/purpose");
    }
    if (!core::is_one_of(o.mode, {"new", "edit", "inpaint", "upscale"})) {
        throw RequestError("mode は " + join(std::vector<std::string>(std::begin(kModes), std::end(kModes)), ", ") + " のどれか", "/mode");
    }
    const std::string purpose = o.purpose.get<std::string>();
    const std::string mode = o.mode.get<std::string>();
    Json locked = core::py_or(py_get(util::or_dict(py_get(doc.studio, "style")), "locked"), Json());
    const bool from_lock = o.tool.is_null() && py_truthy(locked) && py_truthy(py_get(locked, "tool"));
    if (from_lock) o.tool = locked["tool"];
    const auto [found_id, tool_spec] = studio::get_tool(core::poses::config_dir(), o.tool.is_string() ? std::optional<std::string>(o.tool.get<std::string>()) : std::nullopt);
    Json tool_id = found_id ? Json(*found_id) : Json();
    if (from_lock && tool_id.is_null()) tool_id = o.tool;  // fixed by the pilot page even when tools.json does not describe it
    const storage::AssetStore store(project);
    std::map<std::string, std::string> files;
    std::vector<std::string> notes;
    Json steps;
    const Num dpi = doc.spec.dpi.truthy() ? doc.spec.dpi : Num(600);
    std::string expression = doc.spec.expression;
    Json box_size, prompt, target, keep01 = Json::array(), figures_json = Json::array(), b_hash, avoid, characters = Json::array();
    std::vector<Json> cast;
    std::vector<std::string> refs;
    if (purpose == "panel_art" || purpose == "draft") {
        const auto [pg, frame] = page_frame(doc, o.page, o.frame_id);
        expression = pg->spec.expression;
        const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
        box_size = size_block(frame->rect.width, frame->rect.height, kPadMm, dpi, tool_spec, o.mode);
        // guides are drawn at the ~1 MP size, also for upscale requests (whose suggested size is the print size)
        const Json px = suggested_px((frame->rect.width.value() + 2 * kPadMm) / (frame->rect.height.value() + 2 * kPadMm));
        const guide::GenBox box = guide::GenBox::for_frame(*frame, kPadMm, {static_cast<int>(px[0].get<std::int64_t>()), static_cast<int>(px[1].get<std::int64_t>())});
        const auto keep = guide::keepout_boxes(doc, *pg, *frame);
        std::vector<Json> boxes01;
        for (const auto& [rect, why] : keep) {
            keep01.push_back(Json::object({{"box01", box.box01(rect)}, {"why", why}}));
            boxes01.push_back(box.box01(rect));
        }
        auto [made, more] = panel_prompt(doc, *pg, *frame, boxes01, o.instruction);
        prompt = std::move(made);
        notes.insert(notes.end(), more.begin(), more.end());
        for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
            if (c.is_object()) cast.push_back(py_get(c, "id"));
        }
        const Json slot = py_get(panel, "slot");
        target = Json::object({{"page_id", pg->id}, {"page", pg->index.json()}, {"frame_id", frame->id},
                               {"label", pg->index.repr() + "-" + py_str(py_truthy(slot) ? slot : Json(frame->id))}});
        files["guides/composition.png"] = guide::to_png(guide::composition(*pg, *frame, box));
        const bool mannequin = guide::has_mannequin(*pg, *frame);
        if (!cast.empty() || mannequin) {
            files["guides/pose.png"] = guide::to_png(guide::pose(*pg, *frame, box));
            if (mannequin) notes.push_back("guides/pose.png のマネキンがポーズの指定。体の向きと手足の角度を合わせる");
        }
        if (!keep.empty()) files["guides/keepout.png"] = guide::to_png(guide::keepout(doc, *pg, *frame, box));
        const NumBox rect{frame->rect.x, frame->rect.y, frame->rect.width, frame->rect.height};
        for (const Figure& f : service::figures(panel, rect)) {
            const guide::Box head{f.head[0].value(), f.head[1].value(), f.head[2].value(), f.head[3].value()};
            const guide::Box body{f.body[0].value(), f.body[1].value(), f.body[2].value(), f.body[3].value()};
            figures_json.push_back(Json::object({{"char", f.char_id}, {"head01", box.box01(head)}, {"body01", box.box01(body)},
                                                 {"head_mm", box_json(f.head, 2)}, {"body_mm", box_json(f.body, 2)}}));
        }
        if (py_truthy(o.focus_character)) {
            if (std::none_of(cast.begin(), cast.end(), [&](const Json& c) { return core::py_equals(c, o.focus_character); })) {
                throw RequestError(py_str(o.focus_character) + " はこのコマにいない", "/focus_character");
            }
            if (mode == "new") throw RequestError("focus_character は直し（mode edit / inpaint）で使う", "/focus_character");
            Json name = o.focus_character;
            for (const Json& c : core::iterate(doc.bible.characters)) {
                if (core::py_equals(py_get(c, "id"), o.focus_character)) {
                    name = py_get(c, "name");
                    break;
                }
            }
            const std::string who = py_str(name);
            const std::string fc = py_str(o.focus_character);
            prompt["ja"] = "直すのは" + who + "だけ。ほかの人物と背景は元の画像のまま変えない。" + who + "は参照画像（設定画・顔）に合わせる。" +
                           prompt["ja"].get<std::string>();
            prompt["en"] = "Only change " + fc + "; keep everything else exactly as in the source image. Match " + fc +
                           " to the reference sheet and face. " + prompt["en"].get<std::string>();
            refs = panel_refs(doc, store, panel, {o.focus_character}, files);
            if (mode == "inpaint" && !py_truthy(o.regions)) o.regions = Json::array({"person:" + fc, "face:" + fc});
        } else {
            refs = panel_refs(doc, store, panel, cast, files);
        }
        if (cast.size() >= 2 && mode == "new") {
            steps = Json::array({"1. このパックで構図全体（全員）を生成して取り込む", "2. 採用候補を決めたら report_regions で人物ごとの領域を報告する",
                                 "3. 似ていない人物ごとに generation_request（mode inpaint、parent、focus_character）で直す"});
            notes.push_back("複数人物のコマ: まず全体を作り、似ていない人物だけを一人ずつ inpaint で直す（steps を参照）");
        }
        if (mode == "new") {
            if (const auto before = previous_art(doc, store, *frame)) {  // (the same scene's panel before: people, clothes and props carry on)
                files["refs/previous_panel.png"] = before->first;
                refs.push_back("refs/previous_panel.png");
                notes.push_back("refs/previous_panel.png は同じ場面の直前のコマ（" + before->second + "）。人物の服装・持ち物・背景をそろえる。構図は真似しない");
            }
        }
        if (py_truthy(locked) && core::truthy_at(locked, "reference") && !core::py_equals(py_get(locked, "page"), pg->index.json()) &&
            purpose == "panel_art") {
            if (const auto data = asset_bytes(store, locked["reference"])) {
                files["refs/style_pilot.png"] = *data;
                refs.push_back("refs/style_pilot.png");
                notes.push_back("絵柄は " + py_str(py_get(locked, "page")) + " ページ目（試しのページ）で固定済み。refs/style_pilot.png の絵柄・線の太さ・トーンに合わせる" +
                                (core::truthy_at(locked, "tool") ? "。画像ツールは " + py_str(locked["tool"]) + " を使う" : std::string()));
            }
        }
        b_hash = core::brief_hash(panel);
        avoid = avoid_of(doc, cast, panel, pg->spec.expression);
        for (const Json& cid : cast) {
            Json tokens;
            for (const Json& c : core::iterate(doc.bible.characters)) {
                if (core::py_equals(py_get(c, "id"), cid)) {
                    tokens = py_get(c, "tokens_en");
                    break;
                }
            }
            Json own = Json::array();
            for (const std::string& r : refs) {
                if (r.rfind("refs/" + py_str(cid) + "_", 0) == 0) own.push_back(r);
            }
            characters.push_back(Json::object({{"id", cid}, {"tokens_en", tokens}, {"files", std::move(own)}}));
        }
    } else if (purpose == "character_sheet") {
        Json ch;
        for (const Json& c : core::iterate(doc.bible.characters)) {
            if (core::py_equals(py_get(c, "id"), o.character_id)) {
                ch = c;
                break;
            }
        }
        if (ch.is_null()) throw RequestError("登場人物 " + py_str(o.character_id) + " はない", "/character_id");
        Json v = book_vocab(doc, doc.spec.expression);
        v["style"] = artwork(v["style"]);
        box_size = size_block(Num(160.0), Num(240.0), 0.0, dpi, tool_spec, o.mode);
        box_size["frame_mm"] = Json();
        prompt = Json::object({{"ja", v["style"]["ja"].get<std::string>() + "キャラクター設定画。" + character_ja(ch, Json(), v) +
                                          "。同じ人物の正面・横・後ろの全身と、顔のアップ。無地の背景、ポーズは自然に立つ。"},
                               {"en", v["style"]["en"].get<std::string>() + " Character model sheet: " + character_en(ch, v) +
                                          ". Front, side and back full-body views of the same person, plus a face close-up. Plain background."},
                               {"tags", v["style"]["tags"].get<std::string>() + ", character sheet, multiple views, full body, simple background"}});
        const Json id = util::item(ch, "id");
        target = Json::object({{"character_id", id}, {"label", "sheet:" + py_str(id)}});
        cast = {id};
        avoid = avoid_of(doc, cast, Json(), doc.spec.expression);
        Json own = Json::array();
        for (const Json& ref : core::iterate(py_get(ch, "refs", Json::array()))) {
            const auto data = asset_bytes(store, py_get(ref, "asset", Json("")));
            if (data && !core::is_one_of(py_get(ref, "kind"), {"sheet", "face"})) {
                const std::string name = "refs/" + py_str(id) + "_" + py_str(py_get(ref, "kind", Json("ref"))) + "_" +
                                         ref["asset"].get<std::string>().substr(7, 8) + ".png";
                files[name] = *data;
                refs.push_back(name);
                own.push_back(name);
            }
        }
        characters.push_back(Json::object({{"id", id}, {"tokens_en", py_get(ch, "tokens_en")}, {"files", std::move(own)}}));
        notes.push_back("顔が正面を向いたアップを必ず入れる。取り込むとき face_box01 でその範囲を付ける（承認時に顔の参照として切り出す）");
    } else {  // location
        const Json location = location_of(doc, o.location_id);
        if (location.is_null()) throw RequestError("場所 " + py_str(o.location_id) + " はない", "/location_id");
        Json v = book_vocab(doc, doc.spec.expression);
        v["style"] = artwork(v["style"]);
        box_size = size_block(Num(240.0), Num(160.0), 0.0, dpi, tool_spec, o.mode);
        box_size["frame_mm"] = Json();
        const Json id = util::item(location, "id");
        const Json name = py_get(location, "name", id);
        const Json name_en = core::py_or(py_get(location, "name_en"), name);
        prompt = Json::object({{"ja", v["style"]["ja"].get<std::string>() + "背景美術: " + py_str(name) + "（" + py_str(py_get(location, "desc", Json(""))) +
                                          "）。人物は描かない。"},
                               {"en", v["style"]["en"].get<std::string>() + " Background art of " + py_str(name_en) + ", no people."},
                               {"tags", v["style"]["tags"].get<std::string>() + ", scenery, background, no humans"}});
        target = Json::object({{"location_id", id}, {"label", "location:" + py_str(id)}});
        avoid = avoid_of(doc, {}, Json(), doc.spec.expression);
        for (const Json& ref : core::iterate(py_get(location, "refs", Json::array()))) {
            if (const auto data = asset_bytes(store, py_get(ref, "asset", Json("")))) {
                const std::string file = "refs/" + py_str(id) + "_" + ref["asset"].get<std::string>().substr(7, 8) + ".png";
                files[file] = *data;
                refs.push_back(file);
            }
        }
    }
    if (mode == "new" && !(purpose == "panel_art" && files.count("refs/style_pilot.png") != 0)) {  // (the pilot page is the book's own sample)
        catalog_sample(doc, store, expression, files, refs, notes);
    }
    Json source, mask;
    if (mode == "edit" || mode == "inpaint" || mode == "upscale") {
        const core::Frame* frame = (purpose == "panel_art" || purpose == "draft") ? page_frame(doc, o.page, o.frame_id).second : nullptr;
        if (!py_truthy(o.parent) && frame != nullptr && mode == "upscale") {
            o.parent = py_get(util::or_dict(py_get(frame->panel ? *frame->panel : Json::object(), "adopted")), "art");
        }
        const Json cand = candidate_of(doc, frame, o.parent);
        if (cand.is_null()) throw RequestError("mode " + mode + " には元の候補（parent）が要る", "/parent");
        const auto data = asset_bytes(store, util::item(cand, "asset"));
        if (!data) throw RequestError("候補 " + py_str(o.parent) + " の画像が assets/ にない", "/parent");
        files["source.png"] = *data;
        source = "source.png";
        if (mode == "inpaint") {
            if (frame == nullptr) throw RequestError("inpaint はコマの候補だけ", "/mode");
            const std::vector<guide::Box> rects = mask_rects(*frame, o.regions);
            if (rects.empty()) {
                throw RequestError("inpaint には描き直す領域（regions: 領域 id、\"face:<人物>\"、rect_mm）が要る。人物の領域は先に report_regions で報告する",
                                   "/regions");
            }
            const Json px = core::py_or(py_get(cand, "px"), box_size["suggested_px"]);
            const auto wh = core::int_tuple(px);
            // (C++: a size an int cannot hold is refused — Python makes the mask until its memory runs out; not cut to its
            // low bits)
            constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
            if (wh.at(0) > kIntMax || wh.at(1) > kIntMax || wh.at(0) < -kIntMax || wh.at(1) < -kIntMax) {
                throw RequestError("候補の大きさ（px）が大きすぎて、描き直す領域のマスクを作れない", "/candidate_id");
            }
            const Json pad = py_get(util::or_dict(py_get(cand, "mapping")), "pad_mm", Json(kPadMm));
            const guide::GenBox mask_box = guide::GenBox::for_frame(*frame, core::to_float(pad), {static_cast<int>(wh.at(0)), static_cast<int>(wh.at(1))});
            files["mask.png"] = guide::to_png(guide::mask(mask_box, rects));
            mask = "mask.png";
        }
        if (mode == "upscale") notes.push_back("画像ツールの高解像度化で source.png を拡大する。無ければ record_review kind=upscale で理由を残す");
    }
    const Json supports = py_get(tool_spec, "supports", Json::object());
    if (!py_truthy(py_get(supports, "references", Json(true)))) {
        notes.push_back("このツールは参照画像を渡せない。prompt だけで作り、取り込み時に refs_used を空にする");
    }
    if (!mask.is_null() && !py_truthy(py_get(supports, "mask", Json(true)))) notes.push_back("このツールはマスクを使えない。mode edit で指示だけ渡す");
    notes.push_back("composition と pose は構図の参考。線をなぞらせる必要はない");
    notes.push_back("画像に文字・フキダシ・効果音を描かせない。台詞は Genko が描く");
    std::vector<std::string> ref_names;
    for (const auto& [name, data] : files) {
        (void)data;
        if (name.rfind("refs/", 0) == 0) ref_names.push_back(name);
    }
    const std::vector<std::string> ordered_refs = by_priority(ref_names);
    std::optional<std::string> sheet;
    if (purpose == "panel_art" || purpose == "draft") {
        sheet = reference_sheet(files, ordered_refs, core::py_or(py_get(box_size, "suggested_px"), Json::array({1024, 1024})));
    }
    if (sheet) files["guides/references_sheet.png"] = *sheet;
    const auto file_or_null = [&files](const char* name) { return files.count(name) != 0 ? Json(name) : Json(); };
    Json ordered = Json::array();
    for (const std::string& r : ordered_refs) ordered.push_back(r);
    Json notes_json = Json::array();
    for (const std::string& n : notes) notes_json.push_back(n);
    Json request = Json::object({{"type", kType},
                                 {"purpose", purpose},
                                 {"mode", mode},
                                 {"target", target},
                                 {"parent", o.parent},
                                 {"tool", tool_id},
                                 {"brief_hash", b_hash},
                                 {"size", box_size},
                                 {"prompt", prompt},
                                 {"color", expression == "color"},
                                 {"avoid", avoid},
                                 {"keepout", keep01},
                                 {"figures", figures_json},
                                 {"characters", characters},
                                 {"focus_character", o.focus_character},
                                 {"steps", steps},
                                 {"files", Json::object({{"composition", file_or_null("guides/composition.png")},
                                                         {"pose", file_or_null("guides/pose.png")},
                                                         {"keepout", file_or_null("guides/keepout.png")},
                                                         {"references", ordered},
                                                         {"references_sheet", sheet ? Json("guides/references_sheet.png") : Json()},
                                                         {"source", source},
                                                         {"mask", mask}})},
                                 {"order_of_instructions", Json::array({"共通制約", "ページ", "コマのメモ", "今回の指示"})},
                                 {"references_note", "references は大事な順。画像ツールが受けられる枚数が少ないときは、前から順に渡す"
                                                     "（人物の顔・設定画 → 絵柄の見本 → 直前のコマ → 小物 → 場所）。1〜2 枚しか渡せないなら "
                                                     "references_sheet（大事な参照を並べた 1 枚。コマと同じ縦横比なので、出てくる絵の形もそろう）を渡す"},
                                 {"notes_for_agent", notes_json},
                                 {"instruction", o.instruction}});
    const Json kept = briefs::catalog(doc);
    if (!kept.is_null() && briefs::catalog_fits(kept, expression)) {  // (which style, which version: the request says so)
        request["style"] = Json::object({{"catalog_id", util::item(kept, "id")}, {"version", py_get(kept, "version")}, {"title", py_get(kept, "title")}});
    }
    Json hashes = Json::object();
    for (const auto& [name, data] : files) hashes[name] = worklist::sha256_hex(data);
    const std::string digest = worklist::sha256_hex(studio::canonical_json(Json::object({{"request", request}, {"files", hashes}})));
    const std::string request_id = "rq_" + digest.substr(0, 10);
    Json out = Json::object({{"type", kType}, {"id", request_id}});
    for (const auto& [key, value] : request.items()) {
        if (key != "type") out[key] = value;
    }
    const std::string inbox = "studio/inbox/" + request_id + "/";
    out["import"] = Json::object({{"tool", "import_images"}, {"request_id", request_id}, {"inbox", inbox}});
    const std::string where = "画像は " + inbox + " に置く（別マシンなら POST /v1/assets）。";
    Json final_notes = notes_json;
    if (purpose == "panel_art" || purpose == "draft") {
        final_notes.push_back(where + "コマの絵は take_panel_art 1 回で取り込み・採用・拡大・顔の位置まで済む（白い余白が出たら crop01 で切り抜く）");
        out["import"] = Json::object({{"tool", "take_panel_art"}, {"request_id", request_id}, {"inbox", inbox}});
    } else {
        final_notes.push_back(where + "そのあと import_images を呼ぶ");
    }
    out["notes_for_agent"] = std::move(final_notes);
    return Pack{std::move(out), std::move(files)};
}

fs::path write(const Pack& pack, const fs::path& project) { return studio::write_request(project, pack.request, pack.files); }

Json record(const Pack& pack) {
    const Json& req = pack.request;
    const Json target = req["target"];
    Json kept = Json::object();
    for (const char* key : {"page", "page_id", "frame_id", "character_id", "location_id"}) {
        if (target.contains(key)) kept[key] = target[key];
    }
    return Json::object({{"id", req["id"]},
                         {"purpose", req["purpose"]},
                         {"mode", req["mode"]},
                         {"target", std::move(kept)},
                         {"brief_hash", req["brief_hash"]},
                         {"parent", req["parent"]},
                         {"tool", req["tool"]},
                         {"pad_mm", req["size"]["pad_mm"]}});
}

}  // namespace genko::service::genreq
