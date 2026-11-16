// StudioService's tool for the figure's pose library (人形のポーズ集 G4, docs/cpp-migration/SPEC.md FIGURE-02):
// figure_poses reads the library (core/pose_library, data/figure_poses.json) and changes nothing — not the book, not
// anything beside it. It lists the categories with their counts and the poses that match (a category, tags, words in
// Japanese or English, contacts, what the body rests on, whether the hands hold something), a page of them or a random
// pick (splitmix64 from a seed "fp-<16 hex>": the same seed, the same pick; random is only here and in the gallery,
// never in an op), and, asked, a contact sheet of them as a picture. A pose is put on a figure through apply_ops:
// pose_figure {page, id, pose_id, mirror, keep: "ground"} — the ops' own checks and the book's approvals as ever.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/pose_library.hpp"
#include "core/pyconv.hpp"
#include "render/figure_thumbs.hpp"
#include "service/studio_service.hpp"

namespace genko::service {

namespace {

namespace library = core::pose_library;
namespace thumbs = render::figure_thumbs;

constexpr std::size_t kMaxLimit = 500;
constexpr std::size_t kMaxSheet = 24;  // (the poses a contact sheet shows at most)

const char* const kContacts[] = {"l_foot", "r_foot", "l_knee", "r_knee", "l_hand", "r_hand", "seat", "pelvis", "back", "front", "side", "partner"};
const std::pair<const char*, double> kViews[] = {{"front", thumbs::kFront}, {"three_quarter", thumbs::kThreeQuarter}, {"side", thumbs::kSide}};

// The words of a list argument (a string or a list of strings), or why not.
std::optional<std::vector<std::string>> strings_of(const Json& v) {
    std::vector<std::string> out;
    if (v.is_null()) return out;
    if (v.is_string()) {
        out.push_back(v.get<std::string>());
        return out;
    }
    if (!v.is_array()) return std::nullopt;
    for (const Json& item : v) {
        if (!item.is_string()) return std::nullopt;
        out.push_back(item.get<std::string>());
    }
    return out;
}

ToolResult refused(const std::string& message, const std::string& path) { return fail(message, "figure_poses_invalid", path); }

// A whole number argument (null: the default), or nothing when it is not one
std::optional<std::int64_t> whole(const Json& v, std::int64_t fallback) {
    if (v.is_null()) return fallback;
    if (v.is_number_integer()) return v.get<std::int64_t>();
    return std::nullopt;
}

}  // namespace

ToolResult StudioService::figure_poses(const Json& a) {
    if (!a["project"].is_null()) (void)project_path(a["project"]);  // (only that it is one: the library is the build's, not the book's)
    library::Filter filter;
    std::string all_categories;
    const std::vector<library::Category> categories = library::categories();
    for (const auto& c : categories) all_categories += (all_categories.empty() ? "" : ", ") + c.id;
    if (!a["category"].is_null()) {
        if (!a["category"].is_string()) return refused("category は分類の id（" + all_categories + "）", "/category");
        filter.category = a["category"].get<std::string>();
        if (std::none_of(categories.begin(), categories.end(), [&](const auto& c) { return c.id == filter.category; })) {
            return refused("分類 " + filter.category + " は無い（" + all_categories + "）", "/category");
        }
    }
    const auto tags = strings_of(a["tags"]);
    if (!tags) return refused("tags は言葉の並び", "/tags");
    filter.tags = *tags;
    if (!a["query"].is_null()) {
        if (!a["query"].is_string()) return refused("query は言葉（日本語か英語、空白で区切ると全部に合うもの）", "/query");
        filter.query = a["query"].get<std::string>();
    }
    const auto contact = strings_of(a["contact"]);
    if (!contact) return refused("contact は接地の並び", "/contact");
    for (const std::string& c : *contact) {
        if (std::find_if(std::begin(kContacts), std::end(kContacts), [&](const char* k) { return c == k; }) == std::end(kContacts)) {
            return refused("接地 " + c + " は無い（l_foot, r_foot, l_knee, r_knee, l_hand, r_hand, seat, pelvis, back, front, side, partner）", "/contact");
        }
    }
    filter.contact = *contact;
    if (!a["support"].is_null()) {
        const Json& s = a["support"];
        if (!s.is_string() || (s != "floor" && s != "chair" && s != "none")) return refused("support は floor・chair・none のどれか", "/support");
        filter.support = s.get<std::string>();
    }
    if (!a["props"].is_null()) filter.props = core::py_truthy(a["props"]);
    const std::optional<std::int64_t> limit_given = whole(a["limit"], 50);
    const std::optional<std::int64_t> offset_given = whole(a["offset"], 0);
    if (!limit_given) return refused("limit は整数", "/limit");
    if (!offset_given) return refused("offset は整数", "/offset");
    const std::int64_t limit = *limit_given;
    const std::int64_t offset = *offset_given;
    if (limit < 1 || limit > static_cast<std::int64_t>(kMaxLimit)) return refused("limit は 1〜" + std::to_string(kMaxLimit), "/limit");
    if (offset < 0) return refused("offset は 0 以上", "/offset");
    std::vector<std::pair<std::string, double>> views;
    if (a["views"].is_null()) {
        for (const auto& [name, turn] : kViews) views.emplace_back(name, turn);
    } else {
        const auto names = strings_of(a["views"]);
        if (!names || names->empty()) return refused("views は front・three_quarter・side の並び", "/views");
        for (const std::string& n : *names) {
            const auto at = std::find_if(std::begin(kViews), std::end(kViews), [&](const auto& v) { return n == v.first; });
            if (at == std::end(kViews)) return refused("向き " + n + " は無い（front・three_quarter・side）", "/views");
            views.emplace_back(at->first, at->second);
        }
    }
    if (!a["body"].is_null() && !a["body"].is_object()) return refused("body は add_figure の body と同じ形（{sex, heads, shoulders, hips, build, legs}）", "/body");

    const std::vector<const Json*> found = library::list(filter);
    std::vector<const Json*> shown;
    Json data = Json::object();
    if (!a["random"].is_null()) {
        const std::int64_t n = whole(a["random"], 0).value_or(0);
        if (n < 1 || n > static_cast<std::int64_t>(kMaxLimit)) return refused("random は 1〜" + std::to_string(kMaxLimit) + "（何ポーズ選ぶか）", "/random");
        std::uint64_t seed = 0;
        if (!a["seed"].is_null()) {
            const std::optional<std::uint64_t> given = a["seed"].is_string() ? library::seed_of(a["seed"].get<std::string>()) : std::nullopt;
            if (!given) return refused("seed は fp- と 16 桁の 16 進数（例 fp-0123456789abcdef）。返された seed をそのまま渡す", "/seed");
            seed = *given;
        } else {
            std::random_device device;  // (a new seed: returned, so the same pick can be asked again)
            seed = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
        }
        shown = library::sample(found, static_cast<std::size_t>(n), seed);
        data["seed"] = library::seed_text(seed);
    } else {
        if (!a["seed"].is_null()) return refused("seed は random と一緒に（何ポーズ選ぶか）", "/seed");
        const auto from = std::min(found.size(), static_cast<std::size_t>(offset));
        const auto to = std::min(found.size(), from + static_cast<std::size_t>(limit));
        shown.assign(found.begin() + static_cast<std::ptrdiff_t>(from), found.begin() + static_cast<std::ptrdiff_t>(to));
        data["offset"] = offset;
        data["limit"] = limit;
    }
    Json listed = Json::array();
    for (const auto& c : categories) listed.push_back(Json::object({{"id", c.id}, {"ja", c.ja}, {"en", c.en}, {"count", c.count}}));
    Json poses = Json::array();
    for (const Json* p : shown) poses.push_back(library::summary(*p));
    data["categories"] = std::move(listed);
    data["total"] = found.size();
    data["poses"] = std::move(poses);
    data["apply"] = "apply_ops: [{\"op\": \"pose_figure\", \"page\": N, \"id\": \"<人形の id>\", \"pose_id\": \"<id>\", \"mirror\": false, \"keep\": \"ground\"}]"
                    "（新しい人形なら add_figure に pose_id。mirror: true で左右反転。pair: true の二人のポーズは、その人形が a になり、"
                    "partner_id の人形が b（無い id なら b を足す。add_figure では省くと新しい id で足す）。片方だけなら pose_id に <id>#a か <id>#b）";
    std::vector<std::string> images;
    if (core::py_truthy(a["sheet"])) {
        std::vector<Json> picked;
        for (const Json* p : shown) {
            if (picked.size() >= kMaxSheet) break;
            picked.push_back(*p);
        }
        std::vector<double> turns;
        Json view_names = Json::array();
        for (const auto& [name, turn] : views) {
            turns.push_back(turn);
            view_names.push_back(name);
        }
        Json order = Json::array();
        for (const Json& p : picked) order.push_back(p.value("id", Json()));
        if (!picked.empty()) images.push_back(thumbs::sheet_png(picked, turns, a["body"].is_object() ? a["body"] : Json::object()));
        data["sheet"] = Json::object({{"poses", std::move(order)},
                                      {"views", std::move(view_names)},
                                      {"note", "絵は左上から右へ、ポーズごとに views の向きの順。床は茶色の線。" + std::to_string(kMaxSheet) +
                                                   " ポーズまで（多いときは offset で先へ）"}});
    }
    return ToolResult(true, std::move(data), {}, std::move(images));
}

}  // namespace genko::service
