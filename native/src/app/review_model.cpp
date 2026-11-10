#include "app/review_model.hpp"

#include <algorithm>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/atari.hpp"
#include "service/pyfmt.hpp"
#include "service/sheets.hpp"
#include "studio/state.hpp"

namespace genko::app::review_model {

namespace {

// dict.get on a JSON object (null for anything else or a missing key).
Json at(const Json& object, std::string_view key) {
    if (!object.is_object()) return Json();
    const auto it = object.find(std::string(key));
    return it == object.end() ? Json() : *it;
}

// {"layout": "コマ割り", …}.get(key, key)
std::string label_of(const std::vector<std::pair<std::string, std::string>>& table, const std::string& key) {
    for (const auto& [k, v] : table) {
        if (k == key) return v;
    }
    return key;
}

std::string joined_pages(const std::vector<Json>& pages) {
    std::string out;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (i > 0) out += ", ";
        out += core::py_str(pages[i]);
    }
    return out;
}

const core::Page* page_with_id(const core::Document& doc, const Json& page_id) {
    if (!page_id.is_string()) return nullptr;
    for (const auto& page : doc.pages) {
        if (page->id == page_id.get<std::string>()) return page.get();
    }
    return nullptr;
}

const core::Page* page_with_index(const core::Document& doc, const Json& index) {
    for (const auto& page : doc.pages) {
        if (core::py_equals(page->index.json(), index)) return page.get();
    }
    return nullptr;
}

bool panel_truthy(const core::Frame& frame) { return frame.panel && core::py_truthy(*frame.panel); }

Json panel_status(const core::Frame& frame) { return frame.panel ? at(*frame.panel, "status") : Json(); }

}  // namespace

std::string InboxItem::gate_text() const { return gate.is_string() ? gate.get<std::string>() : (core::py_truthy(gate) ? core::py_str(gate) : std::string()); }

std::string InboxItem::title() const {
    if (kind == "help") {
        const std::string where = pages.empty() ? std::string("全体") : core::py_str(pages.front()) + " ページ";
        return "相談（" + where + "）";
    }
    if (kind == "proposal") {
        const std::string label = label_of({{"layout", "コマ割り"}, {"lines", "台詞"}}, gate_text());
        // (C++: a proposal whose page is gone shows "?" — Python's title raises IndexError and the box stops)
        const std::string page = pages.empty() ? std::string("?") : core::py_str(pages.front());
        return "アタリからの提案: " + page + " ページの" + label;
    }
    const std::string label = label_of({{"name", "ネーム"}, {"art", "作画"}, {"sheet", "設定画"}, {"export", "書き出し"}}, gate_text());
    if (gate_text() == "sheet") return label + "の承認: " + (name.empty() ? core::py_str(character_id) : name);
    if (!pages.empty()) return label + "の承認: " + joined_pages(pages) + " ページ";
    return label + "の承認";
}

std::vector<InboxItem> inbox(const core::Document& doc) {
    std::vector<InboxItem> items;
    // names = {c.get("id"): c.get("name") for c in characters}: the last character with an id wins
    const auto name_of = [&](const Json& character_id) -> Json {
        Json found;
        bool any = false;
        if (doc.bible.characters.is_array()) {
            for (const Json& c : doc.bible.characters) {
                if (!c.is_object()) continue;
                if (core::py_equals(at(c, "id"), character_id)) {
                    found = at(c, "name");
                    any = true;
                }
            }
        }
        return any ? found : Json();
    };
    for (const Json& ticket : studio::open_tickets(doc)) {
        const Json kind = at(ticket, "kind");
        if (!(kind == Json("gate") || kind == Json("help"))) continue;
        std::vector<Json> pages;
        if (const Json listed = at(ticket, "pages"); core::py_truthy(listed)) {
            pages = core::iterate(listed);
        } else if (const Json one = at(ticket, "page_index"); core::py_truthy(one)) {
            pages.push_back(one);
        }
        InboxItem item;
        item.ticket_id = core::py_str(at(ticket, "id"));
        item.kind = kind.get<std::string>();
        item.gate = at(ticket, "gate");
        item.pages = std::move(pages);
        item.character_id = at(ticket, "character_id");
        item.frame_id = at(ticket, "frame_id");
        const Json text = at(ticket, "text");
        item.text = core::py_truthy(text) ? core::py_str(text) : std::string();
        const Json by = at(ticket, "created_by");
        item.by = core::py_truthy(by) ? core::py_str(by) : std::string();
        const Json name = name_of(item.character_id);
        item.name = core::py_truthy(name) ? core::py_str(name) : std::string();
        items.push_back(std::move(item));
    }
    const Json proposals = at(doc.studio, "proposals");
    if (proposals.is_object()) {
        for (const auto& [key, proposal] : proposals.items()) {
            (void)key;
            if (!proposal.is_object() || !(at(proposal, "status") == Json("open"))) continue;
            const core::Page* page = page_with_id(doc, at(proposal, "page_id"));
            Json listed = at(proposal, "panels");
            if (!core::py_truthy(listed)) listed = at(proposal, "lines");
            const std::size_t count = core::py_truthy(listed) ? core::length(listed) : 0;
            const Json kind = at(proposal, "kind");
            std::string what;
            if (kind == Json("layout")) {
                const Json confidence = proposal.contains("confidence") ? proposal["confidence"] : Json(0);
                const double value = confidence.is_number() ? core::py_float(confidence) : 0.0;
                what = std::to_string(count) + " コマ（信頼度 " + service::fixed(value, 2) + "）";
            } else {
                what = std::to_string(count) + " 本の台詞";
            }
            InboxItem item;
            item.ticket_id = core::py_str(at(proposal, "id"));
            item.kind = "proposal";
            item.gate = kind;
            if (page != nullptr) item.pages.push_back(page->index.json());
            item.text = what;
            Json by = at(proposal, "source");
            if (!core::py_truthy(by)) by = at(proposal, "by");
            item.by = core::py_truthy(by) ? core::py_str(by) : std::string();
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::vector<std::pair<std::string, Json>> numbered_sheets(const core::Document& doc, const InboxItem& item) {
    const auto whole = [](const Json& value) -> std::int64_t {
        if (!core::py_truthy(value)) return 0;
        try {
            return core::py_int(value);
        } catch (const core::Error&) {
            return 0;
        }
    };
    const std::int64_t changed = whole(at(at(doc.studio, "style"), "changed_rev"));
    const Json all = at(doc.studio, "character_candidates");
    const Json every = item.character_id.is_string() ? at(all, item.character_id.get<std::string>()) : Json();
    std::vector<std::pair<std::string, Json>> out;
    std::size_t n = 0;
    for (const Json& cand : core::py_truthy(every) ? core::iterate(every) : std::vector<Json>{}) {
        ++n;
        const Json status = at(cand, "status");
        if (status == Json("rejected") || status == Json("withdrawn")) continue;
        const bool old = changed != 0 && whole(at(cand, "rev")) < changed;
        out.emplace_back("候補 " + std::to_string(n) + (old ? "（前の絵柄）" : ""), cand);
    }
    std::vector<std::pair<std::string, Json>> fresh, stale;
    for (auto it = out.rbegin(); it != out.rend(); ++it) (it->first.find("前の絵柄") != std::string::npos ? stale : fresh).push_back(*it);
    fresh.insert(fresh.end(), stale.begin(), stale.end());
    return fresh;
}

std::vector<std::pair<std::string, std::int64_t>> progress(const core::Document& doc) {
    std::vector<std::pair<std::string, std::int64_t>> counts = {{"ネーム未提出", 0}, {"ネーム承認待ち", 0}, {"作画中", 0},
                                                                {"作画承認待ち", 0}, {"仕上げ待ち", 0},     {"仕上げ済み", 0}};
    const auto add = [&](const char* key) {
        for (auto& [k, v] : counts) {
            if (k == key) ++v;
        }
    };
    for (const auto& page : doc.pages) {
        if (!page->name_ok) {
            add(studio::name(doc, page->index) ? "ネーム承認待ち" : "ネーム未提出");
        } else if (!page->art_ok) {
            const auto leaves = page->leaf_frames();
            bool done = true;
            bool any = false;
            for (const core::Frame* f : leaves) {
                if (!panel_truthy(*f)) continue;
                any = true;
                const Json status = panel_status(*f);
                if (!(status == Json("adopted") || status == Json("skip") || status.is_null())) done = false;
            }
            add(done && any ? "作画承認待ち" : "作画中");
        } else if (page->stage != "finish") {
            add("仕上げ待ち");
        } else {
            add("仕上げ済み");
        }
    }
    std::vector<std::pair<std::string, std::int64_t>> out;
    const Json& characters = doc.bible.characters;
    if (core::py_truthy(characters)) {
        std::int64_t sheets = 0;
        for (const Json& c : core::iterate(characters)) {
            if (!core::py_truthy(at(c, "locked"))) ++sheets;
        }
        out.emplace_back("設定画未承認", sheets);
    }
    out.insert(out.end(), counts.begin(), counts.end());
    return out;
}

Json approve_ops(const core::Document& doc, const InboxItem& item, const std::optional<std::filesystem::path>& project, const Json& candidate_id,
                 const Json& face_box01) {
    const auto ticket_done = [&] { return Json::array({Json{{"op", "set_ticket"}, {"id", item.ticket_id}, {"status", "done"}}}); };
    if (item.kind == "help") return ticket_done();
    if (item.kind == "proposal") {
        const Json proposals = at(doc.studio, "proposals");
        if (!proposals.is_object() || !proposals.contains(item.ticket_id)) throw core::Error("key", "the proposal is gone");
        const Json& proposal = proposals[item.ticket_id];
        const core::Page* page = page_with_id(doc, at(proposal, "page_id"));
        if (page == nullptr) throw core::Error("key", "the proposal's page is gone");
        return service::atari::assign_frames(service::atari::accept_ops(doc, proposal, false), *page);
    }
    const std::string gate = item.gate_text();
    if (gate == "name" || gate == "art") {
        Json ops = Json::array();
        for (const Json& p : item.pages) ops.push_back(Json{{"op", "approve"}, {"gate", gate}, {"page", p}});
        return ops;
    }
    if (gate == "sheet") {
        if (!core::py_truthy(candidate_id)) throw core::PyValueError("設定画の候補を 1 つ選んでから承認します");
        const Json character = core::py_truthy(item.character_id) ? item.character_id : Json("");
        return Json::array({service::sheet_approval_op(project.value_or(std::filesystem::path()), doc, character, candidate_id, face_box01)});
    }
    if (gate == "export") return Json::array();
    return ticket_done();
}

Json send_back_ops(const core::Document& doc, const InboxItem& item, const std::string& raw, const std::optional<std::vector<std::string>>& frame_ids) {
    const std::string text = core::py_strip(raw);
    if (text.empty()) throw core::PyValueError("理由（返事）を書いてから送ります");
    if (item.kind == "proposal") {
        return Json::array({Json{{"op", "resolve_proposal"}, {"id", item.ticket_id}, {"status", "rejected"}, {"note", text}}});
    }
    Json ops = Json::array({Json{{"op", "set_ticket"}, {"id", item.ticket_id}, {"status", "returned"}}});
    if (item.kind == "help") {
        ops = Json::array({Json{{"op", "set_ticket"}, {"id", item.ticket_id}, {"status", "done"}}});
        if (!item.pages.empty()) {
            ops.push_back(Json{{"op", "request_fix"},
                               {"page", item.pages.front()},
                               {"frame_id", item.frame_id},
                               {"instruction", text},
                               {"scope", core::py_truthy(item.frame_id) ? "frame" : "page"}});
        }
        return ops;
    }
    const std::string gate = item.gate_text();
    if (gate == "name") {
        for (const Json& p : item.pages) ops.push_back(Json{{"op", "request_fix"}, {"page", p}, {"instruction", text}, {"scope", "page"}});
    } else if (gate == "art") {
        for (const Json& p : item.pages) {
            const core::Page* page = page_with_index(doc, p);
            if (page == nullptr) throw core::Error("key", "no page " + core::py_str(p));
            std::vector<std::string> targets;
            if (frame_ids && !frame_ids->empty()) {
                targets = *frame_ids;
            } else {
                for (const core::Frame* f : page->leaf_frames()) {
                    if (panel_status(*f) == Json("adopted")) targets.push_back(f->id);
                }
            }
            for (const std::string& fid : targets) {
                ops.push_back(Json{{"op", "request_fix"}, {"page", p}, {"frame_id", fid}, {"instruction", text}, {"scope", "frame"}});
            }
        }
    } else if (gate == "sheet") {
        ops = Json::array({Json{{"op", "reject_sheet"}, {"character_id", item.character_id}, {"note", text}}});
    }
    return ops;
}

}  // namespace genko::app::review_model
