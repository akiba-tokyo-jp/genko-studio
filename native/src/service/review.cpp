#include "service/review.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "formats/pillow_save.hpp"
#include "render/page.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/genreq.hpp"
#include "service/preview.hpp"
#include "service/pyfmt.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "studio/state.hpp"

namespace genko::service::review {

namespace {

using core::Json;
using core::py_get;
using core::py_str;
using core::py_truthy;

constexpr const char* kCss =
    ":root{--line:#dee2e6;--muted:#6c757d;--accent:#1c7ed6;--ok:#2b8a3e;--warn:#c92a2a}"
    "*{box-sizing:border-box}body{font-family:system-ui,-apple-system,'Hiragino Sans','Noto Sans JP',sans-serif;"
    "margin:0 auto;padding:16px;max-width:1100px;color:#212529;line-height:1.6;background:#fff}"
    "h1{font-size:1.5rem;margin:.2em 0}h2{font-size:1.2rem;margin:1.6em 0 .6em;border-bottom:2px solid var(--line)}"
    "h3{font-size:1.05rem;margin:.4em 0}.lead{background:#e7f5ff;border-radius:8px;padding:10px 14px}"
    ".card{border:1px solid var(--line);border-radius:8px;padding:10px 14px;margin:8px 0}"
    ".card .what{font-weight:bold}.muted{color:var(--muted);font-size:.9em}"
    ".page{display:flex;gap:16px;border-top:1px solid var(--line);padding:14px 0;flex-wrap:wrap}"
    ".page>div{flex:1 1 320px;min-width:0}.page img.pg{width:100%;max-width:520px;border:1px solid #adb5bd}"
    ".cands{display:flex;gap:8px;flex-wrap:wrap;margin:4px 0 12px}"
    ".cand{border:2px solid var(--line);border-radius:6px;padding:4px;font-size:.8em;width:176px;max-width:47%}"
    ".cand.adopted{border-color:var(--ok)}.cand.rejected{opacity:.5}.cand img{display:block;width:100%;height:auto}"
    "table{border-collapse:collapse;font-size:.9em;width:100%}td,th{border:1px solid var(--line);padding:3px 6px;vertical-align:top;text-align:left}"
    ".scroll{overflow-x:auto}details.cmd{margin:.3em 0;font-size:.85em}details.cmd summary{color:var(--muted);cursor:pointer}"
    "code{display:block;background:#f1f3f5;padding:6px 8px;border-radius:4px;font-size:.85em;white-space:pre-wrap;word-break:break-all}"
    ".warn{color:var(--warn)}.ok{color:var(--ok)}"
    "@media (max-width:600px){body{padding:10px}.cand{width:47%}}";

// html.escape(text): & < > " ' as entities.
std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#x27;"; break;
            default: out += c;
        }
    }
    return out;
}

// html.escape(value) of a value Python hands it as it is: a str (anything else has no .replace: AttributeError).
std::string escape_value(const Json& value) {
    if (!value.is_string()) core::raise_attribute_error(value, "replace");
    return escape(value.get<std::string>());
}

std::string num(const core::Num& n) { return py_str(n.json()); }

std::string png_b64(const render::Image& image) { return core::b64encode(png(image)); }

// _thumb_b64(store, ref, size)
std::optional<std::string> thumb_b64(const storage::AssetStore& store, const Json& ref, int size) {
    if (!py_truthy(ref)) return std::nullopt;
    const auto data = store.get_bytes(py_str(ref), ".png");
    if (!data) return std::nullopt;
    render::Image image = open_picture(*data).convert("RGB");
    image.thumbnail(render::Size{size, size});
    return png_b64(image);
}

std::string code(std::string_view text) { return "<code>" + escape(text) + "</code>"; }

// _cmd(text, label): a command folded away (people use the app; the command is there for those who want it).
std::string cmd(std::string_view text, std::string_view label = "コマンドで行う") {
    return "<details class='cmd'><summary>" + escape_value(label) + "</summary>" + code(text) + "</details>";
}

// GATE.get(gate, gate)
std::string gate_word(const Json& gate) {
    if (gate.is_string()) {
        const std::string& g = gate.get_ref<const std::string&>();
        if (g == "name") return "ネーム";
        if (g == "art") return "作画";
        if (g == "sheet") return "設定画";
        if (g == "export") return "書き出し";
    }
    core::require_hashable(gate);
    return py_str(gate);
}

// PANEL.get(status, status)
std::string panel_word(const Json& status) {
    if (status.is_null()) return "未着手";
    if (status.is_string()) {
        static const std::pair<const char*, const char*> words[] = {
            {"empty", "未着手"},      {"briefed", "指示あり"}, {"requested", "依頼済み"},     {"candidates", "候補あり"},
            {"adopted", "採用済み"}, {"fix_requested", "直し待ち"}, {"skip", "絵なし"}};
        for (const auto& [key, word] : words) {
            if (status.get_ref<const std::string&>() == key) return word;
        }
    }
    core::require_hashable(status);
    return py_str(status);
}

// _who(actor)
std::string who(const Json& actor) {
    const std::string text = py_truthy(actor) ? py_str(actor) : std::string();
    if (text.rfind("ai:", 0) == 0) return "AI（" + text.substr(3) + "）";
    if (text.rfind("human:", 0) == 0) return text.substr(6);
    return text;
}

// _words(panel): the shot and angle in Japanese.
std::string words(const Json& panel, const Json& vocab) {
    std::vector<std::string> out;
    for (const char* key : {"shot", "angle"}) {
        const Json given = py_get(panel, key);
        core::require_hashable(given);
        const Json& table = vocab[key];
        const Json entry = given.is_string() && table.contains(given.get<std::string>()) ? table[given.get<std::string>()] : Json();
        const Json ja = py_get(core::py_or(entry, Json::object()), "ja");
        out.push_back(py_str(core::py_or(core::py_or(ja, given), Json(""))));
    }
    std::string joined;
    for (const std::string& w : out) {
        if (w.empty()) continue;
        joined += (joined.empty() ? "" : " / ") + w;
    }
    return joined;
}

// `x in adopted` for a set of the panel's adopted ids.
bool among(const std::vector<Json>& adopted, const Json& id) {
    core::require_hashable(id);
    return std::any_of(adopted.begin(), adopted.end(), [&id](const Json& a) { return core::py_equals(a, id); });
}

// _candidate_tiles(store, cands, adopted, command)
void candidate_tiles(std::string& parts, const storage::AssetStore& store, const std::vector<Json>& cands, const std::vector<Json>& adopted,
                     const std::function<std::string(const Json&)>& command = {}) {
    parts += "<div class='cands'>";
    std::size_t n = 0;
    for (const Json& cand : cands) {
        ++n;
        const auto b64 = thumb_b64(store, py_get(cand, "asset", Json("")), 320);
        const Json review = core::py_or(py_get(cand, "review"), Json::object());
        const Json id = util::item(cand, "id");
        const bool is_adopted = among(adopted, id);
        const std::string cls = std::string("cand") + (is_adopted ? " adopted" : "") +
                                (core::py_equals(py_get(cand, "status"), Json("rejected")) ? " rejected" : "");
        const Json origin = core::py_or(py_get(cand, "origin"), Json::object());
        const Json label = core::py_or(py_get(cand, "label"), Json("候補 " + std::to_string(n)));
        std::vector<std::string> lines = {"<b>" + escape_value(label) + "</b>" + (is_adopted ? " ✔採用" : "")};
        if (py_truthy(review)) {
            lines.push_back("評価 " + py_str(py_get(review, "score")) + ": " + escape(py_str(core::py_or(py_get(review, "note"), Json("")))));
        }
        if (core::truthy_at(cand, "stale")) lines.push_back("<span class='warn'>指示が変わる前の候補</span>");
        std::string tool;
        for (const char* key : {"tool_id", "model"}) {
            if (py_truthy(py_get(origin, key))) tool += (tool.empty() ? "" : " / ") + py_str(origin[key]);
        }
        if (!tool.empty()) lines.push_back("<span class='muted'>" + escape(tool) + "</span>");
        if (core::py_equals(py_get(origin, "kind"), Json("fixture"))) lines.push_back("<span class='warn'>試験用の画像</span>");
        if (!core::truthy_at(cand, "label")) lines.push_back("<span class='muted'>" + escape_value(id) + "</span>");
        if (command) lines.push_back(cmd(command(cand), "この候補で承認（コマンド）"));
        const std::string img = b64 ? "<img alt='" + escape_value(label) + "' src='data:image/png;base64," + *b64 + "'>" : std::string("(画像なし)");
        std::string body;
        for (const std::string& line : lines) body += (body.empty() ? "" : "<br>") + line;
        parts += "<div class='" + cls + "'>" + img + body + "</div>";
    }
    parts += "</div>";
}

// max(36, int(max_px / (height_mm / 25.4)))
int page_dpi(int max_px, const core::Page& page) {
    const double inches = (page.spec.height_mm / core::Num(25.4)).value();
    if (inches == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
    return std::max(36, static_cast<int>(core::loop_count(max_px / inches)));
}

std::string joined_pages(const Json& pages, const char* between) {
    std::string out;
    for (const Json& p : core::iterate(pages)) out += (out.empty() ? "" : between) + py_str(p);
    return out;
}

}  // namespace

std::string review_html(const std::filesystem::path& project, int max_px) {
    const core::Document episode = load_episode(project);
    const storage::AssetStore store(project);
    const std::string proj = util::path_text(project);
    const std::vector<Json> requests = studio::open_tickets(episode);
    const Json report = formats::checks::studio_check(episode, project, formats::checks::StudioCheck{});
    PyDict names;
    for (const Json& c : core::iterate(episode.bible.characters)) {
        const Json id = py_get(c, "id");
        names.set(id, core::py_or(py_get(c, "name"), id));
    }
    const auto name_of = [&names](const Json& id, const Json& fallback) {
        const Json* found = names.find(id);
        return found != nullptr ? *found : fallback;
    };
    std::string parts = "<!doctype html><html lang='ja'><head><meta charset='utf-8'>";
    parts += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    parts += "<title>" + escape(episode.title) + " の確認</title><style>" + kCss + "</style></head><body>";
    parts += "<h1>" + escape(episode.title) + "</h1>";
    parts += "<p class='lead'>このページは見るためのものです。承認・差し戻し・相談への返事は Genko アプリの「承認箱」で行います（開き方: " + escape("genko app") +
             " のあと、この原稿を選ぶ）。AI は承認できません。<br><span class='muted'>版 r" + std::to_string(episode.revision) + "</span></p>";
    if (!requests.empty()) {
        parts += "<h2>あなたを待っているもの（" + std::to_string(requests.size()) + " 件）</h2>";
        for (const Json& req : requests) {
            const Json kind = py_get(req, "kind");
            Json pages = py_get(req, "pages");
            if (!py_truthy(pages)) pages = core::truthy_at(req, "page_index") ? Json::array({req["page_index"]}) : Json::array();
            const std::string where = py_truthy(pages) ? joined_pages(pages, "・") + " ページ" : std::string();
            std::string action;
            std::string what;
            if (core::py_equals(kind, Json("gate"))) {
                const Json gate = py_get(req, "gate");
                const Json target = core::py_equals(gate, Json("sheet")) ? name_of(py_get(req, "character_id"), Json("")) : Json(where);
                what = py_truthy(target) ? gate_word(gate) + "の承認（" + py_str(target) + "）" : gate_word(gate) + "の承認";
                if (core::is_one_of(gate, {"name", "art"})) {
                    action = "genko studio approve " + proj + " " + py_str(gate) + " --pages " +
                             joined_pages(core::py_or(py_get(req, "pages"), Json::array()), ",");
                } else if (core::py_equals(gate, Json("sheet"))) {
                    action = "genko studio approve " + proj + " sheet --character " + py_str(py_get(req, "character_id")) + " --candidate 候補id";
                } else if (core::py_equals(gate, Json("export"))) {
                    action = "genko studio export " + proj + " --format pdf --out 出力先";
                }
            } else if (core::py_equals(kind, Json("help"))) {
                what = "相談（" + (where.empty() ? std::string("全体") : where) + "）";
                action = "genko studio close-ticket " + proj + " " + py_str(util::item(req, "id")) + " --reply 返事";
            } else {
                what = where + "への指示";
            }
            parts += "<div class='card'><div class='what'>" + escape(what) + "</div>";
            if (core::truthy_at(req, "text")) parts += "<div>" + escape_value(req["text"]) + "</div>";
            parts += "<div class='muted'>依頼: " + escape(who(py_get(req, "created_by"))) + "</div>";
            if (!action.empty()) parts += cmd(action);
            parts += "</div>";
        }
    }
    std::vector<Json> proposals;
    {
        const Json all = core::py_or(py_get(episode.studio, "proposals"), Json::object());
        if (!all.is_object()) core::raise_attribute_error(all, "values");
        for (const auto& [key, p] : all.items()) {
            (void)key;
            if (core::py_equals(py_get(p, "status"), Json("open"))) proposals.push_back(p);
        }
    }
    if (!proposals.empty()) {
        parts += "<h2>アタリからの提案（確定するまで原稿は変わりません）</h2>";
        for (const Json& proposal : proposals) {
            const core::Page* page = nullptr;
            for (const auto& p : episode.pages) {
                if (core::py_equals(Json(p->id), py_get(proposal, "page_id"))) {
                    page = p.get();
                    break;
                }
            }
            if (page == nullptr) continue;
            const Json items = core::py_or(core::py_or(py_get(proposal, "panels"), py_get(proposal, "lines")), Json::array());
            const std::string count = std::to_string(core::length(items));
            const bool layout = core::py_equals(util::item(proposal, "kind"), Json("layout"));
            std::string label;
            if (layout) {
                const Json confidence = py_get(proposal, "confidence", Json(0));
                if (!(confidence.is_number() || confidence.is_boolean())) {
                    throw core::PyValueError("Unknown format code '%' for object of type '" + core::py_type_name(confidence) + "'");
                }
                label = count + " コマ（確からしさ " + percent(core::to_float(confidence), 0) + "）";
            } else {
                label = count + " 本の台詞";
            }
            const render::Image image = atari::overlay_image(episode, *page, page_dpi(max_px, *page));
            parts += "<div class='page'><div><h3>" + num(page->index) + " ページ: " + escape_value(label) + "</h3><img class='pg' alt='提案' src='data:image/png;base64," +
                     png_b64(image) + "'></div><div>";
            if (core::py_equals(proposal["kind"], Json("lines"))) {
                parts += "<ul>";
                for (const Json& line : core::iterate(py_get(proposal, "lines", Json::array()))) parts += "<li>" + escape_value(util::item(line, "text")) + "</li>";
                parts += "</ul>";
            }
            const std::string pid = py_str(util::item(proposal, "id"));
            parts += cmd("genko studio accept " + proj + " " + pid, "確定（コマンド）") + cmd("genko studio reject " + proj + " " + pid + " --note 理由", "却下（コマンド）") +
                     "</div></div>";
        }
    }
    // character sheets
    PyDict sheets;
    {
        const Json all = core::py_or(py_get(episode.studio, "character_candidates"), Json::object());
        if (!all.is_object()) core::raise_attribute_error(all, "items");
        for (const auto& [cid, cands] : all.items()) {
            Json kept = Json::array();
            for (const Json& c : core::iterate(cands)) {
                if (!core::py_equals(py_get(c, "status"), Json("withdrawn"))) kept.push_back(c);
            }
            sheets.set(Json(cid), kept);
        }
    }
    const std::vector<Json> characters = core::iterate(episode.bible.characters);
    if (!characters.empty()) {
        parts += "<h2>キャラクターの設定画</h2>";
        for (const Json& character : characters) {
            const Json cid = py_get(character, "id");
            PyDict refs;
            for (const Json& r : core::iterate(py_get(character, "refs", Json::array()))) refs.set(py_get(r, "kind"), py_get(r, "asset"));
            const auto ref = [&refs](const char* kind) {
                const Json* found = refs.find(Json(kind));
                return found != nullptr ? *found : Json();
            };
            const std::string status = core::truthy_at(character, "locked") ? "<span class='ok'>承認済み</span>" : "<span class='warn'>未承認</span>";
            parts += "<h3>" + escape(py_str(py_get(character, "name", cid))) + " " + status + "</h3>";
            const std::string look = studio::written_look(character);
            if (!look.empty()) parts += "<p class='muted'>企画書の見た目: " + escape(look) + "</p>";  // (the words beside the pictures: do they agree?)
            const Json* own = sheets.find(cid);
            if (py_truthy(ref("sheet"))) {
                std::vector<Json> shown = {Json::object({{"id", "sheet"}, {"label", "承認した設定画"}, {"asset", ref("sheet")}})};
                if (py_truthy(ref("face"))) shown.push_back(Json::object({{"id", "face"}, {"label", "顔"}, {"asset", ref("face")}}));
                candidate_tiles(parts, store, shown, {});
            } else if (own != nullptr && py_truthy(*own)) {
                candidate_tiles(parts, store, core::iterate(*own), {}, [&proj, &cid](const Json& c) {
                    return "genko studio approve " + proj + " sheet --character " + py_str(cid) + " --candidate " + py_str(util::item(c, "id"));
                });
            } else {
                parts += "<p class='muted'>候補はまだありません。</p>";
            }
        }
    }
    const Json locked = py_get(core::py_or(py_get(episode.studio, "style"), Json::object()), "locked");
    if (py_truthy(locked)) {
        parts += "<p>絵柄は " + py_str(py_get(locked, "page")) + " ページで決まりました（画像ツール: " +
                 escape(py_str(core::py_or(py_get(locked, "tool"), Json("記録なし")))) + "）。</p>";
    }
    parts += "<h2>ページ</h2>";
    const Json vocab = genreq::vocab();
    for (const auto& page_ptr : episode.pages) {
        const core::Page& page = *page_ptr;
        const std::optional<Json> record = studio::name(episode, page.index);
        const std::optional<Json> draft = record ? std::optional<Json>(util::item(*record, "name")) : std::nullopt;
        const int dpi = page_dpi(max_px, page);
        const bool name_mode = !page.name_ok;
        render::RenderOptions options;
        options.mode = name_mode ? "name" : "proof";
        render::Image image = render::render_page(page, dpi, options, &episode).image;
        if (name_mode) image = annotate(image, page, dpi, draft ? *draft : Json());
        std::string status = page.name_ok ? "ネーム承認済み" : (draft ? "ネーム確認待ち" : "ネームなし");
        if (page.name_ok) {
            status += page.art_ok ? "・作画承認済み" : "・作画中";
            if (page.stage == "finish") status += "・仕上げ済み";
        }
        parts += "<div class='page'><div><h3>" + num(page.index) + " ページ <span class='muted'>" + status + "・" +
                 escape_value(util::item(worklist::page_side(page.index.json()), "turn")) + "</span></h3>";
        parts += "<img class='pg' alt='" + num(page.index) + " ページ' src='data:image/png;base64," + png_b64(image) + "'></div><div>";
        if (draft && !page.name_ok) {
            parts += "<div class='scroll'><table><tr><th>コマ</th><th>構図</th><th>人物</th><th>内容</th><th>台詞</th></tr>";
            std::size_t order = 0;
            for (const Json& panel : core::iterate(py_get(*draft, "panels", Json::array()))) {
                ++order;
                std::string cast;
                for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
                    const Json id = py_get(c, "id");
                    cast += (cast.empty() ? "" : "、") + py_str(name_of(id, id));
                }
                std::string lines;
                bool first = true;
                for (const Json& line : core::iterate(py_get(panel, "lines", Json::array()))) {
                    std::string text;
                    for (const Json& piece : core::iterate(py_get(line, "breaks", Json::array()))) {
                        if (!piece.is_string()) throw core::PyTypeError("sequence item 0: expected str instance, " + core::py_type_name(piece) + " found");
                        text += piece.get<std::string>();
                    }
                    lines += (first ? "" : " / ") + text;
                    first = false;
                }
                parts += "<tr><td>" + std::to_string(order) + "</td><td>" + escape(words(panel, vocab)) + "</td><td>" + escape(cast) + "</td><td>" +
                         escape_value(py_get(panel, "action", Json(""))) + "（" + escape_value(py_get(panel, "emotion", Json(""))) + "）</td><td>" + escape(lines) +
                         "</td></tr>";
            }
            parts += "</table></div>";
            if (const auto review = studio::name_review(episode, page.index); review && py_truthy(*review)) {
                parts += "<p class='muted'>AI の自己点検: " + py_str(py_get(*review, "score")) + " " +
                         escape_value(core::py_or(py_get(*review, "notes"), Json(""))) + "</p>";
            }
            parts += cmd("genko studio approve " + proj + " name --pages " + num(page.index), "ネームを承認（コマンド）");
        }
        if (page.name_ok) {
            std::size_t order = 0;
            for (const core::Frame* frame : page.leaf_frames()) {
                ++order;
                const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
                const std::vector<Json> cands = core::iterate(py_get(panel, "candidates", Json::array()));
                const Json status_value = py_get(panel, "status");
                if (cands.empty() && (status_value.is_null() || core::is_one_of(status_value, {"empty", "skip"}))) continue;
                std::vector<Json> adopted;
                const Json adopted_map = core::py_or(py_get(panel, "adopted"), Json::object());
                if (!adopted_map.is_object()) core::raise_attribute_error(adopted_map, "values");
                for (const auto& [key, value] : adopted_map.items()) {
                    (void)key;
                    if (!py_truthy(value)) continue;
                    core::require_hashable(value);
                    if (!among(adopted, value)) adopted.push_back(value);
                }
                const Json images = py_get(core::py_or(py_get(panel, "attempts"), Json::object()), "images", Json(0));
                parts += "<b>" + std::to_string(order) + " コマ目</b> <span class='muted'>" + panel_word(status_value) + "・取り込んだ絵 " + py_str(images) + " 枚</span>";
                candidate_tiles(parts, store, cands, adopted);
            }
            if (!page.art_ok) parts += cmd("genko studio approve " + proj + " art --pages " + num(page.index), "作画を承認（コマンド）");
        }
        if (!page.art_ok) {
            parts += cmd("genko studio comment " + proj + " --page " + num(page.index) + " 指示", "直してほしいことを送る（コマンド。コマだけなら --frame コマid）");
        }
        parts += "</div></div>";
    }
    parts += "<h2>書き出し</h2>";
    if (py_truthy(report["ok"])) {
        parts += "<p class='ok'>点検は通っています。Genko アプリの「ファイル → 書き出し → 原稿を書き出す…」から書き出せます。</p>" +
                 cmd("genko studio export " + proj + " --format pdf --out 出力先", "正式に書き出す（コマンド）");
    } else {
        const Json& errors = report["errors"];
        parts += "<p class='warn'>書き出しを止めている理由:</p><ul>";
        for (std::size_t i = 0; i < errors.size() && i < 12; ++i) parts += "<li>" + escape_value(errors[i]["message"]) + "</li>";
        if (errors.size() > 12) parts += "<li>ほか " + std::to_string(errors.size() - 12) + " 件</li>";
        parts += "</ul>";
    }
    parts += "</body></html>";
    return parts;
}

}  // namespace genko::service::review
