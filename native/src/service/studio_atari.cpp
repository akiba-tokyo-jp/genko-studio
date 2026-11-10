// StudioService's tools for hand-drawn names and the review page (Python's genko/studio/service.py: import_name,
// analyze_name, propose_lines, review_page): scans placed on the DRAFT layer and analysed into proposals a person
// accepts, lines read from the handwriting proposed too, and review.html written for the person who approves.

#include <algorithm>
#include <tuple>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/importer.hpp"
#include "service/preview.hpp"
#include "service/review.hpp"
#include "service/service_util.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"
#include "studio/state.hpp"

namespace genko::service {

using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

// start_page + i as Python adds them (an int stays an int; a str or a list cannot take an int).
Json page_number(const Json& start_page, std::size_t i) {
    if (start_page.is_number() || start_page.is_boolean()) {
        return (core::to_num(start_page) + core::Num(static_cast<std::int64_t>(i))).json();
    }
    if (start_page.is_string() || start_page.is_array()) {
        const std::string type = start_page.is_string() ? "str" : "list";
        throw core::PyTypeError("can only concatenate " + type + " (not \"int\") to " + type);
    }
    throw core::PyTypeError("unsupported operand type(s) for +: '" + core::py_type_name(start_page) + "' and 'int'");
}

// Path(name).resolve() against the root, and whether it lies under it (`root in path.parents`).
std::pair<fs::path, bool> resolve_under(const fs::path& root, const Json& name) {
    if (!name.is_string()) throw core::PyTypeError("expected str, bytes or os.PathLike object, not " + core::py_type_name(name));
    fs::path source = core::path_from_utf8(name.get<std::string>());
    source = source.is_absolute() ? source : root / source;
    const fs::path resolved = core::resolve_path(source);
    auto p = resolved.begin();
    auto r = root.begin();
    for (; r != root.end() && p != resolved.end() && *p == *r; ++r, ++p) {
    }
    return {resolved, r == root.end() && p != resolved.end()};
}

}  // namespace

ToolResult StudioService::import_name(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& align = a["align"];
    if (!core::is_one_of(align, {"auto", "page", "live"})) return fail("align は auto, page, live", "bad_align", "/align");
    const Json& files = a["files"];
    if (!files.is_array() || files.empty()) return fail("files は 1 つ以上", "no_files", "/files");
    storage::AssetStore store(path);
    const core::Document episode = load_episode(path);
    struct Prepared {
        Json index;
        std::string ref;
        Json size;
        Json placement;
        std::string used;
        std::string filename;
    };
    std::vector<Prepared> prepared;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const Json& name = files[i];
        const std::string where = "/files/" + std::to_string(i);
        const auto [source, under] = resolve_under(root_, name);
        if (confine_ && !under) return fail("--root の外: " + name.get<std::string>(), "outside_root", where);
        std::error_code ec;
        if (!fs::is_regular_file(source, ec)) return fail("ファイルがない: " + name.get<std::string>(), "no_file", where);
        std::string blob;
        std::pair<int, int> size;
        render::Image image;
        try {
            std::tie(blob, size, image) = importer::normalize(storage::read_file(source), i);
        } catch (const importer::ImportError& exc) {
            return fail(exc.what(), "bad_image", where);
        }
        const Json index = page_number(a["start_page"], i);
        const auto at = page_at(episode, index);
        if (!at) return fail(py_str(index) + " ページが無い（ページを先に増やす）", "no_page", where);
        auto [placement, used] = atari::placement_for(image.convert("L"), episode.page(*at), align.get<std::string>());
        prepared.push_back(Prepared{index, store.put_bytes(blob, ".png"), Json::array({size.first, size.second}), std::move(placement), std::move(used),
                                    core::path_to_utf8(source.filename())});
    }
    Json imported = Json::array();
    Json proposals = Json::array();
    {
        Session session(path, actor_);
        Json batch = Json::array();
        for (const Prepared& p : prepared) {
            const Json meta = Json::object({{"kind", "atari"}, {"origin", "self"}, {"file", p.filename}, {"by", actor_}});
            batch.push_back(Json::object({{"op", "register_assets"}, {"assets", Json::object({{p.ref, meta}})}}));
            batch.push_back(Json::object({{"op", "place_asset"}, {"page", p.index}, {"asset", p.ref}, {"to", "draft"}, {"placement_mm", p.placement}, {"title", "アタリ"}}));
            batch.push_back(Json::object({{"op", "set_page_plan"},
                                          {"page", p.index},
                                          {"plan", Json::object({{"atari", Json::object({{"asset", p.ref},
                                                                                         {"px", p.size},
                                                                                         {"placement_mm", p.placement},
                                                                                         {"align", p.used},
                                                                                         {"file", p.filename},
                                                                                         {"by", actor_}})}})}}));
            imported.push_back(Json::object({{"page", p.index}, {"asset", p.ref}, {"align", p.used}, {"placement_mm", p.placement}}));
        }
        session.apply(batch, actor_);
        for (const Json& item : imported) {
            const core::Document& doc = session.doc();
            const auto found = page_at(doc, item["page"]);  // (made just above, in this session)
            if (!found) return fail(py_str(item["page"]) + " ページはない", "no_page", "/start_page");
            const core::Page& page = doc.page(*found);
            const Json proposal = atari::analyze(path, doc, page);
            session.apply(Json::array({Json::object({{"op", "propose"}, {"proposal", proposal}})}), actor_);
            proposals.push_back(Json::object({{"page", item["page"]}, {"proposal", proposal["id"]},
                                              {"panels", static_cast<std::int64_t>(core::length(proposal["panels"]))}, {"confidence", proposal["confidence"]}}));
        }
        session.save(actor_);
    }
    return ToolResult(true, Json::object({{"imported", std::move(imported)}, {"proposals", std::move(proposals)}}));
}

ToolResult StudioService::analyze_name(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& page = a["page"];
    Json proposal;
    {
        Session session(path, actor_);
        const core::Document& doc = session.doc();
        const auto at = page_at(doc, page);
        if (!at) return fail(py_str(page) + " ページはない", "no_page", "/page");
        try {
            proposal = atari::analyze(path, doc, doc.page(*at), a["params"]);
        } catch (const core::PyValueError& exc) {
            return fail(exc.what(), "no_atari", "/page");
        } catch (const core::ApplyError& exc) {  // (ApplyError is a ValueError)
            return fail(exc.what(), "no_atari", "/page");
        }
        session.apply(Json::array({Json::object({{"op", "propose"}, {"proposal", proposal}})}), actor_);
        session.save(actor_);
    }
    // (Python reads the book twice here: once for the overlay's book, once for its page — each read makes the pages'
    // default layers, taking ids, so both are read)
    const core::Document after = load_episode(path);
    const core::Document again = load_episode(path);
    const auto found = page_at(again, page);  // (gone since the proposal was saved: Python's StopIteration)
    if (!found) return fail(py_str(page) + " ページはない", "no_page", "/page");
    const std::string bytes = png(atari::overlay_image(after, again.page(*found), 72));
    return ToolResult(true, Json::object({{"proposal", proposal["id"]}, {"panels", proposal["panels"]}, {"tiers", proposal["tiers"]},
                                          {"confidence", proposal["confidence"]}, {"analysis", proposal["analysis"]}}),
                      {}, {bytes});
}

ToolResult StudioService::propose_lines(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const Json& page = a["page"];
    const auto at = page_at(doc, page);
    if (!at) return fail(py_str(page) + " ページはない", "no_page", "/page");
    const core::Page& target = doc.page(*at);
    const Json& lines = a["lines"];
    if (!lines.is_array() || lines.empty()) return fail("lines は 1 つ以上", "no_lines", "/lines");
    Json checked = Json::array();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        try {
            checked.push_back(atari::line_from_input(lines[i].is_object() ? lines[i] : Json::object(), target, atari::atari_of(target)));
        } catch (const core::PyValueError& exc) {
            return fail(exc.what(), "bad_line", "/lines/" + std::to_string(i));
        } catch (const core::PyTypeError& exc) {
            return fail(exc.what(), "bad_line", "/lines/" + std::to_string(i));
        }
    }
    const std::string id = "pr_" + studio::content_hash(Json::object({{"page", target.id}, {"lines", checked}})).substr(7, 10);
    const Json proposal = Json::object({{"id", id}, {"kind", "lines"}, {"page", page}, {"source", actor_}, {"lines", checked}});
    ToolResult result = ops(a["project"], Json::array({Json::object({{"op", "propose"}, {"proposal", proposal}})}));
    if (!result.ok) return result;
    const core::Document after = load_episode(path);
    const auto found = page_at(after, page);  // (gone since the proposal was saved: Python's StopIteration)
    if (!found) return fail(py_str(page) + " ページはない", "no_page", "/page");
    const std::string bytes = png(atari::overlay_image(after, after.page(*found), 72));
    return ToolResult(true, Json::object({{"proposal", id}, {"lines", static_cast<std::int64_t>(checked.size())}}), {}, {bytes});
}

ToolResult StudioService::review_page(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const fs::path out = path / "studio" / "review.html";
    fs::create_directories(out.parent_path());
    storage::write_atomic(out, studio::text_mode(review::review_html(path)));  // (atomic_write_text)
    Json open_items = Json::array();
    for (const Json& t : studio::open_tickets(load_episode(path))) {
        if (!core::is_one_of(py_get(t, "kind"), {"gate", "help"})) continue;
        open_items.push_back(Json::object({{"kind", py_get(t, "kind")}, {"gate", py_get(t, "gate")}, {"pages", py_get(t, "pages")},
                                           {"character_id", py_get(t, "character_id")}}));
    }
    const std::string text = util::path_text(out);
    return ToolResult(true, Json::object({{"path", text}, {"open_requests", std::move(open_items)},
                                          {"message", "確認ページ: " + text + "（ブラウザで開く。承認はページ内のコマンドで）"}}));
}

}  // namespace genko::service
