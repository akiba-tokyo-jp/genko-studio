#include "core/pose_library.hpp"

#include <algorithm>
#include <cctype>

#include "core/model.hpp"

namespace genko::core::detail {
extern const unsigned char kFigurePosesJson[];
extern const std::size_t kFigurePosesJsonSize;
}  // namespace genko::core::detail

namespace genko::core::pose_library {

namespace {

// l_… ↔ r_… (and "l" ↔ "r"); any other name as it is
std::string other_side(const std::string& name) {
    if (name == "l") return "r";
    if (name == "r") return "l";
    if (name.size() > 2 && name[1] == '_' && (name[0] == 'l' || name[0] == 'r')) return std::string(1, name[0] == 'l' ? 'r' : 'l') + name.substr(1);
    return name;
}

// −v, but 0 stays 0 (not −0.0)
Json negated(const Json& v) {
    if (v.is_number_integer()) return v.get<std::int64_t>() == 0 ? v : Json(-v.get<std::int64_t>());
    if (v.is_number()) {
        const double d = v.get<double>();
        return d == 0.0 ? Json(0.0) : Json(-d);
    }
    return v;
}

// The keys renamed to the other side's, each in its place (so twice is the same object, key for key)
Json sides_swapped(const Json& object) {
    if (!object.is_object()) return object;
    Json out = Json::object();
    for (const auto& [k, v] : object.items()) out[other_side(k)] = v;
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_string(const Json& list, std::string_view want) {
    if (!list.is_array()) return false;
    return std::any_of(list.begin(), list.end(), [&](const Json& v) { return v.is_string() && v.get_ref<const std::string&>() == want; });
}

bool is_hand_with_shape(const Json& hand) { return hand.is_object() && (hand.contains("spread") || hand.contains("thumb")); }

}  // namespace

std::string_view text() { return {reinterpret_cast<const char*>(detail::kFigurePosesJson), detail::kFigurePosesJsonSize}; }

const Json& library() {
    static const Json file = parse_python_json(text());
    return file;
}

std::vector<Category> categories() {
    std::vector<Category> out;
    for (const Json& c : library().at("categories")) out.push_back(Category{c.at("id").get<std::string>(), c.at("ja").get<std::string>(), c.at("en").get<std::string>(), 0});
    for (const Json& p : library().at("poses")) {
        for (Category& c : out) {
            if (c.id == p.at("category").get_ref<const std::string&>()) ++c.count;
        }
    }
    return out;
}

const Json* find(std::string_view id) {
    for (const Json& p : library().at("poses")) {
        if (p.at("id").get_ref<const std::string&>() == id) return &p;
    }
    return nullptr;
}

std::optional<Json> resolve(std::string_view id, bool mirror) {
    std::string_view base = id;
    if (base.size() > kMirror.size() && base.substr(base.size() - kMirror.size()) == kMirror) {
        base.remove_suffix(kMirror.size());
        mirror = !mirror;
    }
    const Json* pose = find(base);
    if (pose == nullptr) return std::nullopt;
    Json out = mirror ? mirrored(*pose) : *pose;
    out["id"] = std::string(base) + (mirror ? std::string(kMirror) : std::string());
    return out;
}

Json mirrored_joints(const Json& joints) {
    if (!joints.is_object()) return joints;
    Json out = Json::object();
    for (const auto& [name, axes] : joints.items()) {
        Json turned = axes;
        if (axes.is_object()) {
            turned = Json::object();
            for (const auto& [axis, v] : axes.items()) turned[axis] = axis == "x" ? v : negated(v);
        }
        out[other_side(name)] = std::move(turned);
    }
    return out;
}

Json mirrored(const Json& pose) {
    Json out = pose;
    if (const auto it = pose.find("joints"); it != pose.end()) out["joints"] = mirrored_joints(*it);
    for (const char* key : {"hands", "hold"}) {
        if (const auto it = pose.find(key); it != pose.end()) out[key] = sides_swapped(*it);
    }
    if (const auto it = pose.find("contact"); it != pose.end() && it->is_array()) {
        Json contact = Json::array();
        for (const Json& c : *it) contact.push_back(c.is_string() ? Json(other_side(c.get<std::string>())) : c);
        out["contact"] = std::move(contact);
    }
    return out;
}

std::vector<const Json*> list(const Filter& filter) {
    std::vector<const Json*> out;
    const std::string query = lower(filter.query);
    for (const Json& p : library().at("poses")) {
        if (!filter.category.empty() && p.at("category").get_ref<const std::string&>() != filter.category) continue;
        if (!filter.support.empty() && p.at("support").get_ref<const std::string&>() != filter.support) continue;
        const Json& tags = p.at("tags");
        if (!std::all_of(filter.tags.begin(), filter.tags.end(), [&](const std::string& t) { return has_string(tags, t); })) continue;
        const Json& contact = p.at("contact");
        if (!std::all_of(filter.contact.begin(), filter.contact.end(), [&](const std::string& c) { return has_string(contact, c); })) continue;
        if (!query.empty()) {
            bool hit = false;
            for (const char* key : {"id", "ja", "en"}) hit = hit || lower(p.at(key).get_ref<const std::string&>()).find(query) != std::string::npos;
            for (const Json& t : tags) hit = hit || lower(t.get_ref<const std::string&>()).find(query) != std::string::npos;
            if (!hit) continue;
        }
        out.push_back(&p);
    }
    return out;
}

bool is_prop(std::string_view name) { return std::find(std::begin(kProps), std::end(kProps), name) != std::end(kProps); }

bool uses_figures_feature(const Json& prim) {
    if (!prim.is_object()) return false;
    const auto kind = prim.find("kind");
    if (kind == prim.end() || !kind->is_string()) return false;
    if (*kind == "figure") {
        if (prim.contains("pose_id") || prim.contains("hold")) return true;
        if (const auto hands = prim.find("hands"); hands != prim.end() && hands->is_object()) {
            for (const auto& [side, hand] : hands->items()) {
                if (is_hand_with_shape(hand)) return true;
            }
        }
        return false;
    }
    if (*kind == "hand") return prim.contains("spread") || prim.contains("thumb");
    return false;
}

bool uses_figures_feature(const Document& doc) {
    for (const auto& page : doc.pages) {
        if (!page || !page->prims.is_array()) continue;
        for (const Json& prim : page->prims) {
            if (uses_figures_feature(prim)) return true;
        }
    }
    return false;
}

}  // namespace genko::core::pose_library
