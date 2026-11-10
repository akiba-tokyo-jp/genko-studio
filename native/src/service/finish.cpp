#include "service/finish.hpp"

#include <algorithm>
#include <cmath>

#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "service/fxwords.hpp"
#include "service/letter.hpp"
#include "service/text.hpp"

namespace genko::service::finish {

namespace {

using core::Json;
using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

constexpr const char* kFxLayer = "効果（仕上げ）";

std::string label_of(const std::string& kind) {
    if (kind == "focus") return "集中線";
    if (kind == "speed") return "流線";
    if (kind == "uni_flash") return "ウニフラッシュ";
    if (kind == "beta_flash") return "ベタフラッシュ";
    if (kind == "white") return "白で塗る";
    return kind;
}

bool kept_clear(const Json& kind) { return core::is_one_of(kind, {"focus", "speed", "uni_flash", "beta_flash"}); }
bool aimed(const std::string& kind) { return kind == "focus" || kind == "uni_flash" || kind == "white"; }

Box box_of(const NumBox& b) { return as_box(b); }

NumBox floats(const Json& rect) {
    const std::vector<double> v = core::unpack_floats(rect, 4);
    return NumBox{Num(v[0]), Num(v[1]), Num(v[2]), Num(v[3])};
}

// finish.face_core(head): the eyes, nose and mouth of a reported head.
Box face_core(const Box& head) {
    const auto [x, y, w, h] = head;
    return Box{x + w * 0.15, y + h * 0.3, w * 0.7, h * 0.62};
}

Json rounded(double v, int places) { return core::py_round(v, places); }
Json rounded(const Num& v, int places) { return core::py_round(v, places).json(); }

// max(a, b) / min(a, b) as Python picks them
Num most(const Num& a, const Num& b) { return b > a ? b : a; }
Num least(const Num& a, const Num& b) { return b < a ? b : a; }

std::pair<Json, std::string> fx_layer(const core::Page& page, const Json& ops, const core::Frame* frame, const std::string& word) {
    const std::string layer_id = frame != nullptr ? text::head("fx-" + page.id + "-" + frame->id + "-" + word, 60) : text::head("fx-" + page.id, 40);
    bool exists = std::any_of(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == layer_id; });
    for (const Json& op : ops) exists = exists || core::py_equals(py_get(op, "id"), Json(layer_id));
    if (exists) return {Json::array(), layer_id};
    std::string name = kFxLayer;
    if (frame != nullptr) {
        const Json panel = frame->panel ? *frame->panel : Json::object();
        const Json slot = py_get(panel, "slot");
        name = word + "（コマ " + py_str(py_truthy(slot) ? slot : Json(frame->id)) + "・仕上げ）";
    }
    return {Json::array({Json::object({{"op", "add_layer"}, {"page", page.index.json()}, {"name", name}, {"kind", "pen"}, {"id", layer_id}})}),
            layer_id};
}

std::tuple<Json, Json, std::string> mark_spot(const core::Frame& frame, const std::vector<Figure>& figs) {
    const core::Rect& r = frame.rect;
    if (!figs.empty()) {
        const Num hx = figs[0].head[0], hy = figs[0].head[1], hw = figs[0].head[2];
        const Num x = hx + hw + Num(10) <= r.x + r.width ? hx + hw + Num(5) : hx - Num(5);
        const Num y = most(r.y + Num(6), hy + Num(2));
        return {rounded(least(most(x, r.x + Num(6)), r.x + r.width - Num(6)), 2), rounded(least(y, r.y + r.height - Num(6)), 2), ""};
    }
    return {rounded(r.x + r.width * Num(0.25), 2), rounded(r.y + r.height * Num(0.25), 2),
            "顔の位置が報告されていないので、コマの左上に置いた。report_regions の後に仕上げると顔の横に置く"};
}

Json rain(const core::Page& page, const core::Frame& frame, const std::string& layer_id) {
    const core::Rect& r = frame.rect;
    core::PyRandom rng = core::PyRandom::from_str(frame.id);
    const double area = (r.width * r.height / Num(90)).value();
    const std::int64_t count = std::max<std::int64_t>(12, std::min<std::int64_t>(220, core::py_trunc_held(area)));
    Json ops = Json::array();
    for (std::int64_t i = 0; i < count; ++i) {
        const double length = rng.uniform(5.0, 13.0);
        const double x = rng.uniform((r.x - Num(4)).value(), (r.x + r.width).value());
        const double y = rng.uniform((r.y - Num(6)).value(), (r.y + r.height).value());
        const double dx = length * 0.26;
        const double dy = length;
        const double width = rng.uniform(0.12, 0.22);
        ops.push_back(Json::object({{"op", "add_stroke"},
                                    {"page", page.index.json()},
                                    {"layer_id", layer_id},
                                    {"points", Json::array({Json::array({rounded(x, 2), rounded(y, 2)}), Json::array({rounded(x + dx, 2), rounded(y + dy, 2)})})},
                                    {"width_mm", rounded(width, 3)},
                                    {"taper", true}}));
    }
    return ops;
}

bool tailed(const core::StoryLine& line) {
    const std::string balloon = line.balloon.empty() ? std::string("speech") : line.balloon;
    static const char* const kNone[] = {"narration", "sfx", "none", "box", "dotted_box", "tone_box", "fancy_box"};
    return std::none_of(std::begin(kNone), std::end(kNone), [&](const char* b) { return balloon == b; }) && line.tails.empty();
}

Json face_clearing(const std::vector<Figure>& figs) {
    Json out = Json::array();
    for (const Figure& f : figs) {
        const auto [x, y, w, h] = box_of(f.head);
        out.push_back(Json::object({{"ellipse", Json::array({rounded(x + w / 2, 2), rounded(y + h / 2, 2), rounded(w / 2 * 1.3 + 1.5, 2),
                                                             rounded(h / 2 * 1.3 + 1.5, 2)})}}));
    }
    return out;
}

Json aim(const std::string& kind, const std::vector<Figure>& figs) {
    if (!aimed(kind) || figs.empty()) return Json::object();
    Box first = box_of(figs[0].head);
    double x0 = first[0], y0 = first[1], x1 = first[0] + first[2], y1 = first[1] + first[3];
    for (const Figure& f : figs) {
        const Box h = box_of(f.head);
        x0 = core::py_min(x0, h[0]);
        y0 = core::py_min(y0, h[1]);
        x1 = core::py_max(x1, h[0] + h[2]);
        y1 = core::py_max(y1, h[1] + h[3]);
    }
    return Json::object({{"center", Json::array({rounded((x0 + x1) / 2, 2), rounded((y0 + y1) / 2, 2)})},
                         {"inner", Json::array({rounded((x1 - x0) / 2 * 1.5 + 2, 2), rounded((y1 - y0) / 2 * 1.5 + 2, 2)})}});
}

}  // namespace

bool regions_stale(const core::Json& panel) {
    const core::Json art = core::py_get(core::py_or(core::py_get(panel, "adopted"), core::Json::object()), "art");
    return core::truthy_at(panel, "regions") && core::truthy_at(panel, "regions_for") && core::py_truthy(art) &&
           !core::py_equals(panel["regions_for"], art);
}

std::vector<Figure> region_figures(const core::Json& panel) {
    std::vector<Json> regions;
    for (const Json& r : core::iterate(py_get(panel, "regions", Json::array()))) {
        const Json rect = py_get(r, "rect_mm");
        if (rect.is_array() && rect.size() == 4) regions.push_back(r);
    }
    std::vector<Json> bodies;
    for (const Json& r : regions) {
        if (core::is_one_of(py_get(r, "kind"), {"person", "body"})) bodies.push_back(r);
    }
    std::vector<Figure> figures;
    for (const Json& face : regions) {
        if (!core::is_one_of(py_get(face, "kind"), {"face", "head"})) continue;
        const NumBox head = floats(face["rect_mm"]);
        NumBox body = head;
        for (const Json& b : bodies) {
            if (core::truthy_at(b, "char") && core::py_equals(b["char"], py_get(face, "char"))) {
                body = floats(b["rect_mm"]);
                break;
            }
        }
        figures.push_back(Figure{Json(py_str(core::py_or(py_get(face, "char"), Json("")))), head, body});
    }
    for (const Json& body : bodies) {
        const Json ch = py_get(body, "char");
        const bool known = std::any_of(figures.begin(), figures.end(), [&](const Figure& f) {
            return py_truthy(f.char_id) && core::py_equals(f.char_id, ch);
        });
        if (known) continue;
        const NumBox box = floats(body["rect_mm"]);
        const NumBox head{box[0], box[1], box[2], least(box[3], box[2])};
        figures.push_back(Figure{Json(py_str(core::py_or(ch, Json("")))), head, box});
    }
    return figures;
}

std::pair<Json, Json> plan(const core::Document& doc, const core::Page& page) {
    Json ops = Json::array();
    Json notes = Json::array();
    const std::vector<const core::StoryLine*> lines = doc.story_for_page(page.index);
    for (const core::Frame* frame : page.leaf_frames()) {
        const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
        std::vector<Figure> figs = region_figures(panel);
        if (!figs.empty() && regions_stale(panel)) {
            notes.push_back(Json::object({{"kind", "regions_stale"},
                                          {"frame_id", frame->id},
                                          {"why", "顔の位置は差し替える前の絵のもの。report_regions で今の絵の顔を送り直してから仕上げる（このコマの台詞は動かしていない）"}}));
            figs.clear();
        }
        std::vector<const core::StoryLine*> in_frame;
        for (const core::StoryLine* line : lines) {
            if (line->frame_id && *line->frame_id == frame->id) in_frame.push_back(line);
        }
        if (!figs.empty() && !in_frame.empty()) {
            const Box rect{frame->rect.x.value(), frame->rect.y.value(), frame->rect.width.value(), frame->rect.height.value()};
            std::vector<Box> placed;
            for (const core::StoryLine* line : in_frame) {
                const Box box{line->x_mm.value(), line->y_mm.value(), line->w_mm.value(), line->h_mm.value()};
                const std::string who = py_str(core::py_or(py_get(line->style.is_object() ? line->style : Json::object(), "speaker_id"), Json("")));
                const Figure* speaker = nullptr;
                for (const Figure& f : figs) {
                    if (!who.empty() && core::py_equals(f.char_id, Json(who))) {
                        speaker = &f;
                        break;
                    }
                }
                bool covers = false;
                for (const Figure& f : figs) covers = covers || letter::overlap(box, face_core(box_of(f.head))) > 0;
                bool misread = false;
                if (speaker != nullptr) {
                    for (const Figure& f : figs) {
                        if (&f != speaker) misread = misread || letter::gap(box, box_of(f.head)) + 1.0 < letter::gap(box, box_of(speaker->head));
                    }
                }
                const Json speaker_id = who.empty() ? Json() : Json(who);
                const std::string head_text = text::head(line->text, 20);
                const Json from = Json::array({rounded(line->x_mm, 1), rounded(line->y_mm, 1)});
                if (misread && !covers) {
                    // (nearer another face than the speaker's: suggested, not moved)
                    const auto spot = letter::find_spot(rect, {line->w_mm.value(), line->h_mm.value()}, placed, figs, speaker_id);
                    Json note = Json::object({{"kind", "suggest_move"}, {"frame_id", frame->id}, {"line", head_text}, {"line_id", line->id}, {"from", from},
                                              {"why", "話していない人の顔のほうが近い。絵を見て、読み違えるなら move_line で動かす（自動では動かしていない）"}});
                    if (spot && !spot->second) note["to"] = Json::array({rounded(spot->first[0], 1), rounded(spot->first[1], 1)});
                    notes.push_back(std::move(note));
                }
                if (covers) {
                    const auto spot = letter::find_spot(rect, {line->w_mm.value(), line->h_mm.value()}, placed, figs, speaker_id);
                    if (spot && !spot->second) {
                        const Box moved = spot->first;
                        Json move = Json::object({{"op", "move_line"}, {"id", line->id}, {"x_mm", rounded(moved[0], 2)}, {"y_mm", rounded(moved[1], 2)}});
                        if (speaker != nullptr && tailed(*line)) {
                            const auto tip = letter::tail_to(moved, box_of(speaker->head), line->balloon.empty() ? "speech" : line->balloon);
                            if (tip) move["tail"] = Json::array({rounded((*tip)[0], 2), rounded((*tip)[1], 2)});
                        }
                        ops.push_back(std::move(move));
                        notes.push_back(Json::object({{"kind", "move_line"}, {"frame_id", frame->id}, {"line", head_text}, {"from", from},
                                                      {"to", Json::array({rounded(moved[0], 1), rounded(moved[1], 1)})}, {"why", "顔にかかっていた"}}));
                        placed.push_back(moved);
                        continue;
                    }
                    notes.push_back(Json::object({{"kind", "face_covered"}, {"frame_id", frame->id}, {"line", head_text},
                                                  {"why", "目・鼻・口にかかっていて、顔を避けて置ける場所が無い。台詞を減らすか人の手で動かす"}}));
                } else if (speaker != nullptr && tailed(*line)) {  // (the tail points at where the speaker really is)
                    const auto tip = letter::tail_to(box, box_of(speaker->head), line->balloon.empty() ? "speech" : line->balloon);
                    if (tip && (!line->tail || std::abs((*tip)[0] - line->tail->x.value()) + std::abs((*tip)[1] - line->tail->y.value()) > 1.0)) {
                        ops.push_back(Json::object({{"op", "move_line"}, {"id", line->id}, {"tail", Json::array({rounded((*tip)[0], 2), rounded((*tip)[1], 2)})}}));
                        notes.push_back(Json::object({{"kind", "aim_tail"}, {"frame_id", frame->id}, {"line", head_text}}));
                    }
                }
                placed.push_back(box);
            }
        }
        std::vector<std::pair<Json, Json>> existing;
        for (const Json& e : core::iterate(page.effects)) {
            const Json kind = py_get(e, "kind");
            const Json fid = py_get(e, "frame_id");
            core::require_hashable(kind);
            core::require_hashable(fid);
            existing.emplace_back(kind, fid);
        }
        const auto has_effect = [&existing](const Json& kind, const Json& fid) {
            return std::any_of(existing.begin(), existing.end(), [&](const auto& e) { return core::py_equals(e.first, kind) && core::py_equals(e.second, fid); });
        };
        if (!figs.empty()) {  // (effects put before the faces were reported: they now stop short of them too)
            for (const Json& effect : core::iterate(page.effects)) {
                if (core::py_equals(py_get(effect, "frame_id"), Json(frame->id)) && kept_clear(py_get(effect, "kind")) && core::truthy_at(effect, "id") &&
                    !py_truthy(py_get(core::py_or(py_get(effect, "params"), Json::object()), "avoid"))) {
                    ops.push_back(Json::object({{"op", "edit_effect"}, {"page", page.index.json()}, {"id", effect["id"]},
                                                {"params", Json::object({{"avoid", face_clearing(figs)}})}}));
                    notes.push_back(Json::object({{"kind", "clear_faces"}, {"frame_id", frame->id}, {"effect", effect["kind"]},
                                                  {"label", label_of(py_str(effect["kind"]))},
                                                  {"why", "顔 " + std::to_string(figs.size()) + " つの手前で線を止めた"}}));
                }
            }
        }
        std::vector<Json> done = core::iterate(core::py_or(py_get(panel, "fx_done"), Json::array()));
        std::vector<std::string> drawn;
        for (const Json& raw : core::iterate(core::py_or(py_get(panel, "fx", Json::array()), Json::array()))) {
            const std::string word = core::py_strip(py_str(raw));
            const auto found = fxwords::resolve(Json(word));
            if (!found) {
                notes.push_back(Json::object({{"kind", "fx_unknown"}, {"frame_id", frame->id}, {"word", word},
                                              {"why", "Genko の知らない効果の言葉。絵の依頼文には入れた。描くなら apply_ops で"}}));
                continue;
            }
            const std::string kind = py_str((*found)["kind"]);
            if (kind == "art") {
                notes.push_back(Json::object({{"kind", "fx_in_art"}, {"frame_id", frame->id}, {"word", word}, {"why", "画像ツールが描く効果（依頼文に入れた）"}}));
                continue;
            }
            if (kind == "effect") {
                const std::string effect = py_str((*found)["key"]);
                if (has_effect(Json(effect), Json(frame->id))) continue;
                existing.emplace_back(Json(effect), Json(frame->id));
                if (effect == "white" && py_truthy(py_get(core::py_or(py_get(panel, "adopted"), Json::object()), "art"))) {
                    notes.push_back(Json::object({{"kind", "effect_skipped"}, {"frame_id", frame->id}, {"effect", effect}, {"label", label_of(effect)},
                                                  {"why", "「白で塗る」はコマの絵を全部白で隠すので、絵のあるコマには入れなかった。光らせるならウニフラッシュ"}}));
                    continue;
                }
                Json params = aim(effect, figs);
                const bool clear = kept_clear(Json(effect));
                if (clear && !figs.empty()) params["avoid"] = face_clearing(figs);
                ops.push_back(Json::object({{"op", "add_effect"}, {"page", page.index.json()}, {"kind", effect}, {"frame_id", frame->id}, {"params", params}}));
                Json note = Json::object({{"kind", "add_effect"}, {"frame_id", frame->id}, {"effect", effect}, {"label", label_of(effect)}});
                if (effect == "white") note["why"] = "コマを白で塗った（絵はまだ無い）";
                if (clear && figs.empty()) {
                    note["why"] = "顔の位置が報告されていないので、線は顔を避けていない。report_regions で顔を報告すると、顔の手前で線を止める";
                } else if (clear) {
                    note["why"] = "顔 " + std::to_string(figs.size()) + " つの手前で線を止めた";
                }
                notes.push_back(std::move(note));
                continue;
            }
            if (std::any_of(done.begin(), done.end(), [&word](const Json& d) { return core::py_equals(d, Json(word)); })) continue;
            auto [layer_ops, layer_id] = fx_layer(page, ops, frame, word);
            for (Json& op : layer_ops) ops.push_back(std::move(op));
            const std::string gone = "絵に同じ記号がもう描かれていたら delete_layer（id " + layer_id + "）で消す";
            if (kind == "mark") {
                const auto [x, y, why] = mark_spot(*frame, figs);
                ops.push_back(Json::object({{"op", "stamp_material"}, {"page", page.index.json()}, {"material_id", "mark-" + py_str((*found)["key"])},
                                            {"layer_id", layer_id}, {"x_mm", x}, {"y_mm", y}}));
                std::string because = why.empty() ? gone : why + "。" + gone;
                notes.push_back(Json::object({{"kind", "add_mark"}, {"frame_id", frame->id}, {"mark", (*found)["key"]}, {"layer_id", layer_id}, {"why", because}}));
            } else {  // rain
                for (Json& op : rain(page, *frame, layer_id)) ops.push_back(std::move(op));
                std::string why = gone;
                const std::size_t at = why.find("記号");
                if (at != std::string::npos) why.replace(at, std::string("記号").size(), "雨");
                notes.push_back(Json::object({{"kind", "add_rain"}, {"frame_id", frame->id}, {"layer_id", layer_id}, {"why", why}}));
            }
            drawn.push_back(word);
        }
        if (!drawn.empty()) {
            std::vector<Json> all = done;
            for (const std::string& w : drawn) {
                if (std::none_of(all.begin(), all.end(), [&w](const Json& d) { return core::py_equals(d, Json(w)); })) all.push_back(w);
            }
            std::stable_sort(all.begin(), all.end(), [](const Json& a, const Json& b) { return core::py_less(a, b); });
            Json unique = Json::array();
            for (const Json& a : all) {
                if (std::none_of(unique.begin(), unique.end(), [&a](const Json& u) { return core::py_equals(u, a); })) unique.push_back(a);
            }
            ops.push_back(Json::object({{"op", "set_panel"}, {"page", page.index.json()}, {"frame_id", frame->id}, {"set", Json::object({{"fx_done", unique}})}}));
        }
    }
    if (page.stage != "finish") ops.push_back(Json::object({{"op", "advance"}, {"page", page.index.json()}, {"to", "finish"}}));
    return {std::move(ops), std::move(notes)};
}

}  // namespace genko::service::finish
