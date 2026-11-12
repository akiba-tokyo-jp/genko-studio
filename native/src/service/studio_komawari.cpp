// StudioService's tool for the batch of layout ideas (コマ割り K3, docs/cpp-migration/SPEC.md LAYOUT-01): layout_ideas
// reads the book and changes nothing. It draws a batch of skeletons for one page (service/layout_ideas: an exact number
// of panels or a range, tiers, a preset, knobs, a seed, how many, a skeleton or the page as it is to vary), says which
// neighbouring pages each idea is alike to, and, given the agent's draft plan, merges each idea into it
// (layout_ideas::merge_into_plan) so the agent submits the one it picks through submit_name — where the gates are, a
// page a person approved among them.

#include <algorithm>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/book.hpp"
#include "service/komawari_shape.hpp"
#include "service/layout.hpp"
#include "service/layout_ideas.hpp"
#include "service/studio_service.hpp"

namespace genko::service {

using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

// The request's own keys, as the tool takes them (layout_ideas::request_from_json reads these).
constexpr const char* kRequestKeys[] = {"panels", "min_panels", "max_panels", "min_tiers", "max_tiers", "preset",
                                        "knobs",  "seed",       "count",      "base",      "variation", "focus"};

// The panels a draft plan holds (0 when it holds none).
int panels_of(const Json& plan) {
    if (!plan.is_object()) return 0;
    const Json panels = py_get(plan, "panels");
    return panels.is_array() ? static_cast<int>(panels.size()) : 0;
}

}  // namespace

ToolResult StudioService::layout_ideas(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const Json& page_index = a["page"];
    const auto at = page_at(doc, page_index);
    if (!at) return fail(py_str(page_index) + " ページはない（1〜" + std::to_string(doc.pages.size()) + "）", "no_page", "/page");
    const core::Page& page = doc.page(*at);
    const Json& plan = a["plan"];
    if (!plan.is_null() && !plan.is_object()) return fail("plan はネーム計画（name_plan@1）のオブジェクト", "plan_invalid", "/plan");
    Json args = Json::object();
    for (const char* key : kRequestKeys) {
        if (!a[key].is_null()) args[key] = a[key];
    }
    if (py_truthy(a["base_page"])) {
        if (!a["base"].is_null()) return fail("変異の元は base か base_page のどちらか 1 つ", "base_twice", "/base_page");
        const auto skeleton = komawari::skeleton_of_page(doc, page);
        if (!skeleton || layout::is_blank(doc, page)) {
            return fail(py_str(page_index) + " ページには、元にできる段組のコマが無い（白紙か、描いたコマ）。base に案の skeleton を渡す", "base_unusable",
                        "/base_page");
        }
        args["base"] = *skeleton;
    }
    // (a draft plan and no count of panels: exactly its panels, so each idea has a slot for each of them)
    const bool counted = !a["panels"].is_null() || !a["min_panels"].is_null() || !a["max_panels"].is_null();
    if (!counted && !args.contains("base") && panels_of(plan) > 0 && panels_of(plan) <= static_cast<int>(layout::kMaxPanels)) {
        args["panels"] = panels_of(plan);
    }
    layout_ideas::Request request;
    layout_ideas::Batch batch;
    try {
        request = layout_ideas::request_from_json(args, page.spec);
        batch = layout_ideas::generate(request);
    } catch (const layout::LayoutError& exc) {
        return fail(exc.message(), "layout_ideas_invalid", exc.where());
    }
    Json data = layout_ideas::batch_to_json(batch);
    // the neighbouring pages as they are, and which of them each idea is alike to (layout_repeats looks at runs of these)
    const std::int64_t number = core::to_int(page_index);
    std::vector<std::pair<std::int64_t, komawari::Shape>> around;
    Json around_json = Json::array();
    for (const std::int64_t n : {number - 1, number + 1}) {
        const auto there = page_at(doc, Json(n));
        if (!there) continue;
        const auto shape = komawari::shape_of_page(doc, doc.page(*there));
        if (!shape) continue;
        around.emplace_back(n, *shape);
        around_json.push_back(Json::object({{"page", n}, {"summary", komawari::describe(*shape)}}));
    }
    Json& ideas = data["ideas"];
    for (std::size_t i = 0; i < batch.ideas.size(); ++i) {
        Json like = Json::array();
        for (const auto& [n, shape] : around) {
            if (komawari::difference(batch.ideas[i].shape, shape) <= komawari::kRepeatAtMost) like.push_back(n);
        }
        ideas[i]["like_pages"] = std::move(like);
        if (plan.is_object()) {
            Json merged = layout_ideas::merge_into_plan(batch.ideas[i].skeleton, plan);
            merged["page"] = page_index;
            ideas[i]["plan"] = std::move(merged);
        }
    }
    Json out = Json::object({{"page", page_index}, {"request", layout_ideas::request_to_json(request)}});
    for (const auto& [key, value] : data.items()) out[key] = value;
    out["around"] = std::move(around_json);
    out["name_approved"] = page.name_ok;
    std::vector<Issue> issues;
    if (page.name_ok) {
        issues.push_back(warning("name_approved", "/page",
                                 py_str(page_index) + " ページのネームは承認済み。AI はこのページのコマ割りを変えない（submit_name は断る）",
                                 "案は別のページの参考にする。このページを変えるには人間に承認の取り消しを頼む"));
    }
    if (static_cast<int>(batch.ideas.size()) < request.count) {
        issues.push_back(warning("layout_ideas_few", "/count",
                                 "案が " + std::to_string(batch.ideas.size()) + " 個しか出なかった（" + std::to_string(request.count) +
                                     " 個の指定。dropped に振るいで除いた理由）",
                                 "コマ数・段数の範囲を広げるか、つまみを変える"));
    }
    return ToolResult(true, std::move(out), std::move(issues));
}

}  // namespace genko::service
