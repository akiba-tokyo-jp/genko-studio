#include "service/worklist.hpp"

#include <QCryptographicHash>

#include <algorithm>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/paths.hpp"
#include "service/blocking.hpp"
#include "service/finish.hpp"
#include "service/preflight.hpp"
#include "service/pyfmt.hpp"
#include "storage/asset_store.hpp"
#include "studio/files.hpp"
#include "studio/state.hpp"

namespace genko::service::worklist {

using core::Num;

std::string sha256_hex(const std::string& text) {
    return QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex().toStdString();
}

std::string plan_hash(const Json& plan) { return studio::content_hash(plan); }

Json page_side(const Json& page) {
    if (core::py_equals(page, Json(1))) return Json::object({{"side", "first"}, {"turn", "1ページ目（表紙の次）"}});
    if (core::py_mod(core::to_num(page), Num(2)) == Num(0)) return Json::object({{"side", "after_turn"}, {"turn", "めくってすぐ見えるページ（reveal を置く）"}});
    return Json::object({{"side", "before_turn"}, {"turn", "めくる前のページ（最後のコマに hook を置く）"}});
}

namespace {

Json tools(std::initializer_list<const char*> names) {
    Json out = Json::array();
    for (const char* name : names) out.push_back(name);
    return out;
}

// worklist.item(kind, why, tools, page, blocked_by, **extra): frame_id and character_id go to the target; input_hash
// only into the id.
Json item(const std::string& kind, const std::string& why, Json tool_list, const std::optional<Json>& page, Json blocked_by = Json::array(),
          Json extra = Json::object()) {
    Json target = Json::object();
    if (page) target["page"] = *page;
    for (const char* key : {"frame_id", "character_id"}) {
        if (extra.contains(key) && !extra[key].is_null()) {
            target[key] = extra[key];
            extra.erase(key);
        }
    }
    const Json which = core::py_or(target.contains("frame_id") ? target["frame_id"] : Json(),
                                   target.contains("character_id") ? target["character_id"] : Json(""));
    const std::string which_text = core::py_truthy(which) ? core::py_str(which) : std::string();
    const std::string input = extra.contains("input_hash") ? core::py_str(extra["input_hash"]) : std::string();
    Json out = Json::object();
    out["id"] = "wi_" + sha256_hex(kind + ":" + (page ? core::py_str(*page) : std::string("None")) + ":" + which_text + ":" + input).substr(0, 10);
    out["kind"] = kind;
    out["target"] = std::move(target);
    out["why"] = why;
    out["tools"] = std::move(tool_list);
    out["blocked_by"] = blocked_by.is_null() ? Json::array() : std::move(blocked_by);
    for (const auto& [key, value] : extra.items()) {
        if (key != "input_hash") out[key] = value;
    }
    return out;
}

Json frame_target(const std::string& frame_id) { return Json::object({{"frame_id", frame_id}}); }

Json with(Json extra, const Json& more) {
    for (const auto& [key, value] : more.items()) extra[key] = value;
    return extra;
}

Json texts_of(const std::vector<Json>& tickets, const char* key, const Json& fallback) {
    Json out = Json::array();
    for (const Json& t : tickets) out.push_back(fallback.is_null() ? core::subscript(t, key) : core::py_get(t, key, fallback));
    return out;
}

bool contains_pair(const std::vector<std::pair<Json, Json>>& requested, const char* gate, const Json& page) {
    return std::any_of(requested.begin(), requested.end(),
                       [&](const auto& item) { return core::py_equals(item.first, Json(gate)) && core::py_equals(item.second, page); });
}

Json limits_of(const core::Document& doc) {
    Json out = Json::object({{"images_per_panel", 8}, {"fix_rounds", 2}});
    const Json policy = core::py_or(core::py_get(doc.studio, "policy"), Json::object());
    const Json limits = core::py_or(core::py_get(policy, "limits"), Json::object());
    if (!limits.is_object()) throw core::PyTypeError("'" + core::py_type_name(limits) + "' object is not a mapping");
    for (const auto& [key, value] : limits.items()) out[key] = value;
    return out;
}

const Json* open_request(const core::Document& doc, const std::string& frame_id) {
    const Json* requests = core::get(doc.studio, "requests");
    if (requests == nullptr || !requests->is_object()) return nullptr;
    for (const auto& [id, req] : requests->items()) {
        if (core::py_get(req, "status") == Json("open") &&
            core::py_equals(core::py_get(core::py_or(core::py_get(req, "target"), Json::object()), "frame_id"), Json(frame_id))) {
            return &req;
        }
    }
    return nullptr;
}

std::size_t inbox_files(const std::optional<std::filesystem::path>& project, const Json& request_id) {
    if (!project) return 0;
    const std::filesystem::path folder = *project / "studio" / "inbox" / core::path_from_utf8(core::py_str(request_id));
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && !core::path_to_utf8(entry.path().filename()).starts_with(".")) ++n;
    }
    return n;
}

bool regions_done(const Json& panel) {
    bool cast = false;
    for (const Json& c : core::iterate(core::py_get(panel, "characters", Json::array()))) cast = cast || c.is_object();
    if (!cast) return true;
    if (finish::regions_stale(panel)) return false;  // (reported on the art it had before: the faces are elsewhere)
    for (const Json& r : core::iterate(core::py_get(panel, "regions", Json::array()))) {
        if (core::is_one_of(core::py_get(r, "kind"), {"face", "head", "person", "body"}) && core::is_one_of(core::py_get(r, "source"), {"agent", "user"})) {
            return true;
        }
    }
    const Json review = core::py_get(core::py_or(core::py_get(panel, "reviews"), Json::object()), "regions");
    return core::py_truthy(review) &&
           core::py_equals(core::py_get(review, "input_hash"), core::py_get(core::py_or(core::py_get(panel, "adopted"), Json::object()), "art"));
}

NumBox rect_of(const core::Frame& frame) { return {frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height}; }

Json upscale_items(const core::Document& doc, const core::Page& page, const std::optional<std::filesystem::path>& project,
                   const std::optional<std::string>& frame_id = std::nullopt) {
    Json out = Json::array();
    if (!project) return out;
    const storage::AssetStore store(*project);
    for (const core::Layer* layer : preflight::art_layers(page)) {
        if (frame_id && layer->frame_id != frame_id) continue;
        const auto dpi = preflight::layer_dpi(page, *layer, store);
        if (!dpi || *dpi >= preflight::kMinDpi || !layer->frame_id || layer->frame_id->empty()) continue;
        const core::Frame* frame = page.find_frame(*layer->frame_id);
        if (frame == nullptr) continue;
        const Json panel = frame->panel ? core::py_or(*frame->panel, Json::object()) : Json::object();
        const Json adopted = core::py_get(core::py_or(core::py_get(panel, "adopted"), Json::object()), "art");
        const Json review = core::py_get(core::py_or(core::py_get(panel, "reviews"), Json::object()), "upscale");
        if (core::py_truthy(review) && core::py_equals(core::py_get(review, "input_hash"), adopted)) continue;
        (void)doc;
        out.push_back(item("upscale_panel",
                           "採用した絵の実効解像度が " + fixed(*dpi, 0) + " dpi（" + std::to_string(preflight::kMinDpi) +
                               " 未満）。adopt（candidate_id に adopted）を呼ぶと拡大して採用し直す",
                           tools({"adopt", "generation_request", "take_panel_art", "record_review"}), page.index.json(), Json::array(),
                           Json::object({{"frame_id", *layer->frame_id}, {"dpi", core::py_round_int(*dpi)}, {"adopted", adopted},
                                         {"input_hash", core::py_str(adopted)}})));
    }
    return out;
}

Json finish_items(const core::Document& doc, const core::Page& page, const std::optional<std::filesystem::path>& project) {
    Json out = upscale_items(doc, page, project);
    if (page.stage != "finish") {
        out.push_back(item("finish_page", "作画は承認済み。仕上げ（台詞の顔よけ、効果、finish へ）", tools({"finish_page", "render"}), page.index.json()));
    }
    return out;
}

Json art_items(const core::Document& doc, const core::Page& page, const std::vector<std::pair<Json, Json>>& requested,
               const std::vector<Json>& requested_sheets, const std::optional<std::filesystem::path>& project) {
    if (page.art_ok) return finish_items(doc, page, project);
    std::vector<std::pair<Json, Json>> chars;  // id → character (a later one replacing)
    for (const Json& c : core::iterate(doc.bible.characters)) {
        const Json id = core::py_get(c, "id");
        core::require_hashable(id);
        const auto at = std::find_if(chars.begin(), chars.end(), [&id](const auto& item) { return core::py_equals(item.first, id); });
        if (at != chars.end()) {
            at->second = c;
        } else {
            chars.emplace_back(id, c);
        }
    }
    const Json limits = limits_of(doc);
    Json out = Json::array();
    int waiting = 0;
    for (const core::Frame* frame : page.leaf_frames()) {
        const Json panel = frame->panel ? core::py_or(*frame->panel, Json::object()) : Json::object();
        const Json status = core::py_get(panel, "status", Json("empty"));
        const Json target = frame_target(frame->id);
        if (status == Json("skip")) continue;
        if (status == Json("adopted")) {
            if (!regions_done(panel)) {
                ++waiting;
                Json figures_json = Json::array();
                for (const Figure& f : figures(panel, rect_of(*frame))) {
                    figures_json.push_back(Json::object({{"char", f.char_id}, {"head_mm", box_json(f.head, 1)}, {"body_mm", box_json(f.body, 1)}}));
                }
                out.push_back(item("report_regions",
                                   "採用した絵の顔と人物の位置をまだ報告していない（写植の顔よけに使う）。"
                                   "figures はネームでの目安（絵で実際の位置を確かめて rect_mm か box01 で報告する）",
                                   tools({"render", "report_regions"}), page.index.json(), Json::array(),
                                   with(Json::object({{"adopted", core::py_get(core::py_or(core::py_get(panel, "adopted"), Json::object()), "art")},
                                                      {"figures", figures_json}}),
                                        target)));
                continue;
            }
            const Json small = upscale_items(doc, page, project, frame->id);  // (before the art is shown for approval)
            if (!small.empty()) {
                ++waiting;
                for (const Json& entry : small) out.push_back(entry);
            }
            continue;
        }
        ++waiting;
        std::vector<Json> unlocked;
        for (const Json& c : core::iterate(core::py_get(panel, "characters", Json::array()))) {
            if (!c.is_object()) continue;
            const Json cid = core::py_get(c, "id");
            core::require_hashable(cid);
            const auto at = std::find_if(chars.begin(), chars.end(), [&cid](const auto& item) { return core::py_equals(item.first, cid); });
            if (at != chars.end() && !core::py_truthy(core::py_get(at->second, "locked"))) unlocked.push_back(cid);
        }
        if (!unlocked.empty()) {
            for (const Json& cid : unlocked) {
                const bool asked = std::any_of(requested_sheets.begin(), requested_sheets.end(), [&cid](const Json& s) { return core::py_equals(s, cid); });
                bool offered = false;
                const Json bucket = core::py_or(core::py_get(doc.studio, "character_candidates"), Json::object());
                for (const Json& cand : core::iterate(core::py_get(bucket, core::py_str(cid), Json::array()))) {
                    offered = offered || !core::is_one_of(core::py_get(cand, "status"), {"rejected", "withdrawn"});
                }
                if (asked) {
                    out.push_back(item("await_human", "キャラクター設定画の承認待ち", tools({"request_approval"}), std::nullopt, Json::array({"requested"}),
                                       Json::object({{"gate", "sheet"}, {"character_id", cid}})));
                } else if (offered) {
                    out.push_back(item("await_human", "設定画の候補がある。人間に選んで承認してもらう", tools({"candidates", "request_approval"}), std::nullopt,
                                       Json::array({"await_human:sheet"}), Json::object({{"gate", "sheet"}, {"character_id", cid}})));
                } else {
                    Json notes = Json::array();
                    for (const Json& t : core::iterate(doc.tickets)) {
                        if (core::py_get(t, "kind") == Json("fix") && core::py_get(t, "status") == Json("open") && core::py_equals(core::py_get(t, "character_id"), cid)) {
                            notes.push_back(core::py_get(t, "text", Json("")));
                        }
                    }
                    Json extra = Json::object({{"character_id", cid}});
                    if (!notes.empty()) extra["comments"] = notes;
                    out.push_back(item("make_sheet", "作画の前にキャラクター設定画を作り、承認してもらう",
                                       tools({"generation_request", "import_images", "candidates", "request_approval"}), std::nullopt, Json::array(), extra));
                }
            }
            continue;
        }
        const Json attempts = core::py_or(core::py_get(panel, "attempts"), Json::object());
        const bool over = !core::py_less(core::py_get(attempts, "images", Json(0)), limits["images_per_panel"]) ||
                          core::py_less(limits["fix_rounds"], core::py_get(attempts, "fix_rounds", Json(0)));
        const Json* request = open_request(doc, frame->id);
        if (request != nullptr) {
            const Json request_id = core::subscript(*request, "id");
            const std::size_t files = inbox_files(project, request_id);
            const std::string why = files ? "inbox に取り込んでいない画像が " + std::to_string(files) + " 枚ある" : "依頼パックの画像を生成して inbox に置き、取り込む";
            out.push_back(item("import_pending", why, tools({"take_panel_art", "import_images"}), page.index.json(), Json::array(),
                               with(Json::object({{"request_id", request_id}, {"input_hash", request_id}}), target)));
        } else if (status == Json("candidates")) {
            out.push_back(item("review_candidates", "候補を比べて評価し、採用するか直す",
                               tools({"candidates", "render", "review_candidates", "adopt", "generation_request"}), page.index.json(), Json::array(), target));
        } else if (over) {
            out.push_back(item("gen_panel", "このコマの生成回数が上限に達した。人間の判断を待つ", tools({"ask_human"}), page.index.json(), Json::array({"limit"}),
                               with(Json::object({{"attempts", attempts}}), target)));
        } else if (status == Json("fix_requested")) {
            std::vector<Json> open_fixes;
            for (const Json& t : core::iterate(doc.tickets)) {
                if (core::py_get(t, "status") == Json("open") && core::py_get(t, "kind") == Json("fix") && core::py_equals(core::py_get(t, "frame_id"), Json(frame->id))) {
                    open_fixes.push_back(t);
                }
            }
            out.push_back(item("fix_panel", "人間からコマの修正指示がある（絵を採用し直すと閉じる。絵以外の直しは resolve_ticket で閉じる）",
                               tools({"inspect", "generation_request", "take_panel_art", "apply_ops", "resolve_ticket"}), page.index.json(), Json::array(),
                               with(Json::object({{"comments", texts_of(open_fixes, "text", Json(""))}, {"tickets", texts_of(open_fixes, "id", Json())}}),
                                    target)));
        } else {
            out.push_back(item("gen_panel", "このコマの絵がまだない（依頼パックを作り、外部で生成して取り込む）",
                               tools({"inspect", "generation_request", "take_panel_art", "candidates"}), page.index.json(), Json::array(), target));
        }
    }
    if (waiting == 0) {
        const char* blocker = contains_pair(requested, "art", page.index.json()) ? "requested" : "await_human:art";
        out.push_back(item("await_human", "人間の作画承認待ち", tools({"render", "request_approval"}), page.index.json(), Json::array({blocker}),
                           Json::object({{"gate", "art"}})));
    }
    return out;
}

Json atari_items(const core::Document& doc, const core::Page& page, const std::vector<std::pair<Json, Json>>& requested) {
    const Json n = page.index.json();
    std::vector<Json> proposals;
    const Json all = core::py_or(core::py_get(doc.studio, "proposals"), Json::object());
    if (!all.is_object()) core::raise_attribute_error(all, "values");
    for (const auto& [id, p] : all.items()) {
        if (core::py_equals(core::py_get(p, "page_id"), Json(page.id))) proposals.push_back(p);
    }
    std::vector<std::string> open_kinds;
    bool accepted_lines = false;
    for (const Json& p : proposals) {
        const std::string kind = core::py_str(core::subscript(p, "kind"));
        if (core::py_get(p, "status") == Json("open") && std::find(open_kinds.begin(), open_kinds.end(), kind) == open_kinds.end()) open_kinds.push_back(kind);
        accepted_lines = accepted_lines || (kind == "lines" && core::py_get(p, "status") == Json("accepted"));
    }
    std::sort(open_kinds.begin(), open_kinds.end());
    const auto open = [&open_kinds](const char* kind) { return std::find(open_kinds.begin(), open_kinds.end(), kind) != open_kinds.end(); };
    const bool blank = page.leaf_frames().size() == 1;
    const bool lines = !doc.story_for_page(page.index).empty();
    const Json plan = page.plan ? core::py_or(*page.plan, Json::object()) : Json::object();
    const bool no_lines_noted = core::py_truthy(core::py_get(core::py_or(core::py_get(plan, "reviews"), Json::object()), "atari_lines"));
    Json out = Json::array();
    if (!open_kinds.empty()) {
        std::string joined;
        for (const std::string& k : open_kinds) joined += (joined.empty() ? "" : ",") + k;
        out.push_back(item("await_human", "アタリからの提案（コマ割り・台詞）を人間が確かめて確定する", tools({"render", "review_page"}), n,
                           Json::array({"await_human:proposal"}), Json::object({{"gate", "proposal"}, {"input_hash", joined}})));
    }
    if (blank && !open("layout")) {
        out.push_back(item("atari_layout", "アタリのコマ割りの提案が無い（見つからなかったか却下された）", tools({"render", "analyze_name", "ask_human"}), n));
    }
    if (!lines && !accepted_lines && !open("lines") && !no_lines_noted) {
        out.push_back(item("read_atari", "アタリの手書きの台詞を読み、位置と一緒に提案する（台詞が無ければ record_review kind=atari_lines）",
                           tools({"render", "propose_lines", "record_review"}), n));
    }
    if (!blank && !open("layout")) {
        Json missing = Json::array();
        std::string joined;
        for (const core::Frame* f : page.leaf_frames()) {
            const Json panel = f->panel ? core::py_or(*f->panel, Json::object()) : Json::object();
            if (!core::truthy_at(panel, "shot")) {
                missing.push_back(f->id);
                joined += (joined.empty() ? "" : ",") + f->id;
            }
        }
        if (!missing.empty()) {
            out.push_back(item("brief_panels", "コマ割りは確定した。アタリを見て各コマの指示（shot、人物、動き）を書く", tools({"inspect", "render", "apply_ops"}), n,
                               Json::array(), Json::object({{"frames", missing}, {"input_hash", joined}})));
            return out;
        }
        if (open_kinds.empty() && (lines || accepted_lines || no_lines_noted)) {
            const char* blocker = contains_pair(requested, "name", n) ? "requested" : "await_human:name";
            out.push_back(item("await_human", "人間のネーム承認待ち", tools({"request_approval"}), n, Json::array({blocker}), Json::object({{"gate", "name"}})));
        }
    }
    return out;
}

Json export_items(const core::Document& doc, const std::vector<std::pair<Json, Json>>& requested) {
    if (doc.pages.empty()) return Json::array();
    for (const auto& page : doc.pages) {
        if (!(page->art_ok && page->stage == "finish")) return Json::array();
    }
    bool done = false;
    for (const Json& record : core::iterate(core::py_get(doc.studio, "approvals", Json::array()))) {
        if (core::py_get(record, "gate") == Json("export")) done = !core::py_truthy(core::py_get(record, "revoked"));
    }
    if (done) return Json::array();
    bool asked = std::any_of(requested.begin(), requested.end(), [](const auto& item) { return item.first == Json("export"); });
    for (const Json& t : core::iterate(doc.tickets)) {
        asked = asked || (core::py_get(t, "gate") == Json("export") && core::py_get(t, "status") == Json("open") && core::py_get(t, "kind") == Json("gate"));
    }
    return Json::array({item("await_human", "全ページの仕上げが済んだ。preflight を確かめ、人間に書き出しを頼む", tools({"preflight", "export_proof", "request_approval"}),
                             std::nullopt, Json::array({asked ? "requested" : "await_human:export"}), Json::object({{"gate", "export"}}))});
}

const core::Page* pilot_page(const core::Document& doc) {
    const Json policy = core::py_or(core::py_get(doc.studio, "policy"), Json::object());
    const Json pilot = core::py_get(policy, "pilot");
    if ((pilot.is_boolean() && !pilot.get<bool>()) || doc.pages.empty()) return nullptr;
    const Json wanted = core::py_get(policy, "pilot_page");
    for (const auto& page : doc.pages) {
        if (core::py_equals(page->index.json(), wanted)) return page.get();
    }
    return doc.pages.front().get();
}

}  // namespace

Json next_actions(const core::Document& doc, const std::optional<std::filesystem::path>& project) {
    if (!studio::bible(doc)) return Json::array({item("write_bible", "企画書（bible）がまだない", tools({"inspect", "set_bible"}), std::nullopt)});
    std::vector<Json> atari_pages;
    for (const auto& page : doc.pages) {
        const Json plan = page->plan ? core::py_or(*page->plan, Json::object()) : Json::object();
        if (core::truthy_at(plan, "atari")) atari_pages.push_back(page->index.json());
    }
    const auto is_atari = [&atari_pages](const Json& index) {
        return std::any_of(atari_pages.begin(), atari_pages.end(), [&index](const Json& a) { return core::py_equals(a, index); });
    };
    const bool no_script = !studio::script(doc);
    if (no_script && atari_pages.empty()) return Json::array({item("write_script", "脚本がまだない", tools({"inspect", "set_script"}), std::nullopt)});
    Json out = Json::array();
    if (no_script) {  // (a hand-drawn name needs no script for its own pages; the other pages still do)
        if (std::any_of(doc.pages.begin(), doc.pages.end(), [&](const auto& p) { return !is_atari(p->index.json()) && !p->name_ok; })) {
            out.push_back(item("write_script", "アタリの無いページのために脚本が要る", tools({"inspect", "set_script"}), std::nullopt));
        }
    }
    std::vector<std::pair<Json, Json>> requested;
    std::vector<Json> requested_sheets;
    for (const Json& t : studio::open_tickets(doc, std::string("gate"))) {
        for (const Json& p : core::iterate(core::py_or(core::py_get(t, "pages"), Json::array()))) requested.emplace_back(core::py_get(t, "gate"), p);
        if (core::py_get(t, "gate") == Json("sheet")) requested_sheets.push_back(core::py_get(t, "character_id"));
    }
    for (const auto& page_ptr : doc.pages) {
        const core::Page& page = *page_ptr;
        const Json n = page.index.json();
        const auto draft = studio::name(doc, page.index);
        const std::vector<Json> fixes = studio::page_fixes(doc, page.index);
        if (!fixes.empty() && !page.name_ok) {
            out.push_back(item("revise_page", "人間から修正の指示がある", tools({"inspect", "render", "submit_name"}), n, Json::array(),
                               Json::object({{"comments", texts_of(fixes, "text", Json(""))}, {"tickets", texts_of(fixes, "id", Json())}})));
            continue;
        }
        if (page.name_ok) {
            if (!fixes.empty()) {  // (after the name: the instruction is done with apply_ops and closed with resolve_ticket)
                std::string joined;
                for (const Json& t : fixes) joined += (joined.empty() ? "" : ",") + core::py_str(core::subscript(t, "id"));
                out.push_back(item("fix_page", "人間から修正の指示がある（直したら resolve_ticket で閉じる）", tools({"inspect", "render", "apply_ops", "resolve_ticket"}), n,
                                   Json::array(),
                                   Json::object({{"comments", texts_of(fixes, "text", Json(""))}, {"tickets", texts_of(fixes, "id", Json())}, {"input_hash", joined}})));
            }
            for (const Json& entry : art_items(doc, page, requested, requested_sheets, project)) out.push_back(entry);
            continue;
        }
        const Json plan = page.plan ? core::py_or(*page.plan, Json::object()) : Json::object();
        if (core::truthy_at(plan, "atari")) {
            for (const Json& entry : atari_items(doc, page, requested)) out.push_back(entry);
            continue;
        }
        if (!draft) {
            const bool blank = page.leaf_frames().size() == 1 && doc.story_for_page(page.index).empty();
            if (blank && !no_script) out.push_back(item("plan_page", "ネームがまだない", tools({"inspect", "submit_name", "render"}), n, Json::array(), page_side(n)));
            continue;
        }
        const Json review = core::py_get(core::py_or(core::py_get(*draft, "reviews"), Json::object()), "name");
        if (!core::py_truthy(review) || !core::py_equals(core::py_get(review, "input_hash"), core::py_get(*draft, "input_hash"))) {
            out.push_back(item("review_name",
                               "今のネームをまだ自己点検していない（submit_name の返事の画像で見て record_review。見返すときだけ render）",
                               tools({"record_review", "submit_name", "render"}), n, Json::array(),
                               Json::object({{"input_hash", core::py_get(*draft, "input_hash", Json(""))}})));
            continue;
        }
        const char* blocker = contains_pair(requested, "name", n) ? "requested" : "await_human:name";
        out.push_back(item("await_human", "人間のネーム承認待ち", tools({"request_approval"}), n, Json::array({blocker}), Json::object({{"gate", "name"}})));
    }
    for (const Json& entry : export_items(doc, requested)) out.push_back(entry);
    // _dedupe
    Json items = Json::array();
    std::vector<std::string> seen;
    for (const Json& entry : out) {
        const std::string id = entry["id"].get<std::string>();
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        items.push_back(entry);
    }
    // _block_by_pilot: until the pilot page's art is approved (and the style fixed), other pages wait for their art
    if (const core::Page* pilot = pilot_page(doc); pilot != nullptr && !pilot->art_ok) {
        for (Json& entry : items) {
            if (core::is_one_of(entry["kind"], {"gen_panel", "import_pending", "review_candidates", "fix_panel", "report_regions"}) &&
                !core::py_equals(core::py_get(entry["target"], "page"), pilot->index.json())) {
                entry["blocked_by"].push_back("pilot:" + pilot->index.repr());
            }
        }
    }
    // _block_by_help: open help tickets (ask_human) park the matching items until a person closes them
    std::vector<Json> help;
    for (const Json& t : core::iterate(doc.tickets)) {
        if (core::py_get(t, "kind") == Json("help") && core::py_get(t, "status") == Json("open")) help.push_back(t);
    }
    if (!help.empty()) {
        for (Json& entry : items) {
            const Json& target = entry["target"];
            for (const Json& ticket : help) {
                const Json page_index = core::py_get(ticket, "page_index");
                const bool same_page = !page_index.is_null() && core::py_equals(page_index, core::py_get(target, "page"));
                const bool same_frame = !core::truthy_at(ticket, "frame_id") || core::py_equals(ticket["frame_id"], core::py_get(target, "frame_id"));
                const bool same_kind = core::truthy_at(ticket, "item") && core::py_equals(ticket["item"], entry["kind"]) && page_index.is_null();
                if ((same_page && same_frame) || same_kind) {
                    const std::string blocker = "ticket:" + core::py_str(core::subscript(ticket, "id"));
                    if (std::find(entry["blocked_by"].begin(), entry["blocked_by"].end(), Json(blocker)) == entry["blocked_by"].end()) {
                        entry["blocked_by"].push_back(blocker);
                    }
                }
            }
        }
    }
    return items;
}

}  // namespace genko::service::worklist
