#include "core/pose_library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

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
    std::string role;
    if (base.size() > 2 && base[base.size() - 2] == '#' && (base.back() == 'a' || base.back() == 'b')) {
        role = std::string(1, base.back());
        base.remove_suffix(2);
    }
    if (base.size() > kMirror.size() && base.substr(base.size() - kMirror.size()) == kMirror) {
        base.remove_suffix(kMirror.size());
        mirror = !mirror;
    }
    const Json* pose = find(base);
    if (pose == nullptr || (!role.empty() && !is_pair(*pose))) return std::nullopt;
    Json out = mirror ? mirrored(*pose) : *pose;
    out["id"] = std::string(base) + (mirror ? std::string(kMirror) : std::string());
    if (!role.empty()) return member(out, role);
    return out;
}

bool is_pair(const Json& pose) { return pose.is_object() && pose.contains("figures"); }

Json member(const Json& pair, std::string_view role) {
    const Json& figures = pair.at("figures");
    const Json& f = figures.at(role == "b" ? 1 : 0);
    Json out = Json::object();
    out["id"] = pair.value("id", std::string()) + "#" + std::string(role);
    for (const char* key : {"category", "ja", "en", "tags"}) out[key] = pair.value(key, Json());
    for (const char* key : {"joints", "hands", "hold", "support", "seat_h", "contact"}) {
        if (f.contains(key)) out[key] = f[key];
    }
    out["symmetric"] = false;
    return out;
}

std::string mirror_id(std::string_view id) {
    std::string name(id);
    std::string role;
    if (name.size() > 2 && name[name.size() - 2] == '#') {
        role = name.substr(name.size() - 2);
        name.resize(name.size() - 2);
    }
    if (name.size() > kMirror.size() && name.ends_with(kMirror)) {
        name.resize(name.size() - kMirror.size());
    } else {
        name += kMirror;
    }
    return name + role;
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
    if (is_pair(pose) && pose.at("figures").is_array()) {  // (each person mirrored; b's place and turn the other way; the touches' sides)
        Json out = pose;
        Json figures = Json::array();
        for (const Json& f : pose.at("figures")) {
            Json m = mirrored(f);
            if (const auto it = f.find("offset"); it != f.end() && it->is_array() && !it->empty()) {
                Json offset = *it;
                offset[0] = negated(offset[0]);
                m["offset"] = std::move(offset);
            }
            if (const auto it = f.find("turn"); it != f.end()) m["turn"] = negated(*it);
            figures.push_back(std::move(m));
        }
        out["figures"] = std::move(figures);
        if (const auto it = pose.find("touch"); it != pose.end() && it->is_array()) {
            Json touch = Json::array();
            for (const Json& t : *it) {
                Json both = t;
                if (t.is_object()) {
                    for (const auto& [k, v] : t.items()) both[k] = v.is_string() ? Json(other_side(v.get<std::string>())) : v;
                }
                touch.push_back(std::move(both));
            }
            out["touch"] = std::move(touch);
        }
        return out;
    }
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

namespace {

// The query's words (split by ASCII and 全角 spaces), lower-cased
std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) out.push_back(lower(word));
        word.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ' ' || text[i] == '\t') {
            flush();
        } else if (text.compare(i, 3, "\xe3\x80\x80") == 0) {  // (U+3000, the 全角 space)
            flush();
            i += 2;
        } else {
            word += text[i];
        }
    }
    flush();
    return out;
}

}  // namespace

std::vector<const Json*> list(const Filter& filter) {
    std::vector<const Json*> out;
    const std::vector<std::string> query = words(filter.query);
    for (const Json& p : library().at("poses")) {
        if (!filter.category.empty() && p.at("category").get_ref<const std::string&>() != filter.category) continue;
        // (a pair: either of its people)
        std::vector<const Json*> people;
        if (is_pair(p)) {
            for (const Json& f : p.at("figures")) people.push_back(&f);
        } else {
            people.push_back(&p);
        }
        const auto any = [&](const auto& test) { return std::any_of(people.begin(), people.end(), [&](const Json* f) { return test(*f); }); };
        if (!filter.support.empty() && !any([&](const Json& f) { return f.at("support").get_ref<const std::string&>() == filter.support; })) continue;
        const Json& tags = p.at("tags");
        if (!std::all_of(filter.tags.begin(), filter.tags.end(), [&](const std::string& t) { return has_string(tags, t); })) continue;
        if (!any([&](const Json& f) {
                return std::all_of(filter.contact.begin(), filter.contact.end(), [&](const std::string& c) { return has_string(f.at("contact"), c); });
            })) {
            continue;
        }
        if (filter.props && any([](const Json& f) { return f.contains("hold") && !f.at("hold").empty(); }) != *filter.props) continue;
        const bool all = std::all_of(query.begin(), query.end(), [&](const std::string& word) {
            bool hit = false;
            for (const char* key : {"id", "ja", "en"}) hit = hit || lower(p.at(key).get_ref<const std::string&>()).find(word) != std::string::npos;
            for (const Json& t : tags) hit = hit || lower(t.get_ref<const std::string&>()).find(word) != std::string::npos;
            return hit;
        });
        if (!all) continue;
        out.push_back(&p);
    }
    return out;
}

Json summary(const Json& pose) {
    if (is_pair(pose)) {
        Json people = Json::array();
        Json contact = Json::array();
        Json props = Json::array();
        const char* const roles[] = {"a", "b"};
        std::size_t i = 0;
        for (const Json& f : pose.at("figures")) {
            Json one = summary(f);
            Json mine = Json::object({{"role", roles[std::min<std::size_t>(i++, 1)]}, {"hands", one["hands"]}, {"contact", one["contact"]},
                                      {"support", one["support"]}, {"props", one["props"]}});
            for (const Json& c : one["contact"]) {
                if (std::find(contact.begin(), contact.end(), c) == contact.end()) contact.push_back(c);
            }
            for (const Json& c : one["props"]) props.push_back(c);
            people.push_back(std::move(mine));
        }
        Json out = Json::object({{"id", pose.value("id", Json())},
                                 {"ja", pose.value("ja", Json())},
                                 {"en", pose.value("en", Json())},
                                 {"category", pose.value("category", Json())},
                                 {"tags", pose.value("tags", Json::array())},
                                 {"hands", people.empty() ? Json::object() : people[0]["hands"]},
                                 {"contact", std::move(contact)},
                                 {"support", people.empty() ? Json() : people[0]["support"]},
                                 {"props", std::move(props)},
                                 {"symmetric", pose.value("symmetric", Json(false))},
                                 {"pair", true},
                                 {"figures", std::move(people)}});
        return out;
    }
    Json hands = Json::object();
    if (const auto it = pose.find("hands"); it != pose.end() && it->is_object()) {
        for (const char* side : {"l", "r"}) {
            if (!it->contains(side)) continue;
            const Json& hand = (*it)[side];
            hands[side] = hand.is_object() ? hand.value("pose", Json("relaxed")) : hand;
        }
    }
    Json props = Json::array();
    if (const auto it = pose.find("hold"); it != pose.end() && it->is_object()) {
        for (const char* side : {"l", "r"}) {
            if (it->contains(side)) props.push_back((*it)[side]);
        }
    }
    return Json::object({{"id", pose.value("id", Json())},
                         {"ja", pose.value("ja", Json())},
                         {"en", pose.value("en", Json())},
                         {"category", pose.value("category", Json())},
                         {"tags", pose.value("tags", Json::array())},
                         {"hands", std::move(hands)},
                         {"contact", pose.value("contact", Json::array())},
                         {"support", pose.value("support", Json())},
                         {"props", std::move(props)},
                         {"symmetric", pose.value("symmetric", Json(false))}});
}

namespace {

std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

}  // namespace

std::vector<const Json*> sample(const std::vector<const Json*>& from, std::size_t n, std::uint64_t seed) {
    std::vector<const Json*> pool = from;
    const std::size_t take = std::min(n, pool.size());
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < take; ++i) {  // (a partial Fisher–Yates shuffle)
        const std::size_t j = i + static_cast<std::size_t>(splitmix64(state) % static_cast<std::uint64_t>(pool.size() - i));
        std::swap(pool[i], pool[j]);
    }
    pool.resize(take);
    return pool;
}

std::optional<std::uint64_t> seed_of(std::string_view text) {
    if (text.size() != 19 || text.substr(0, 3) != "fp-") return std::nullopt;
    std::uint64_t v = 0;
    for (const char c : text.substr(3)) {
        const int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
        if (d < 0) return std::nullopt;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    return v;
}

std::string seed_text(std::uint64_t seed) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "fp-%016llx", static_cast<unsigned long long>(seed));
    return buf;
}

bool is_prop(std::string_view name) { return std::find(std::begin(kProps), std::end(kProps), name) != std::end(kProps); }

Held held_of(std::string_view value) {
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos) return Held{value, {}, false};
    return Held{value.substr(0, colon), value.substr(colon + 1), true};
}

std::vector<std::string_view> ways_of(std::string_view prop) {
    if (prop == "sword" || prop == "bat" || prop == "umbrella") return {std::begin(kLongWays), std::end(kLongWays)};
    if (prop == "bag") return {std::begin(kBagWays), std::end(kBagWays)};
    return {};
}

std::string hold_problem(std::string_view value) {
    const Held held = held_of(value);
    if (!is_prop(held.prop)) {
        std::string all;
        for (const std::string_view p : kProps) all += (all.empty() ? "" : ", ") + std::string(p);
        return "a hand holds one of " + all;
    }
    if (!held.has_way) return {};
    const std::vector<std::string_view> ways = ways_of(held.prop);
    const std::string prop(held.prop);
    const std::string article = prop == "umbrella" ? "an " : "a ";
    if (ways.empty()) return article + prop + " has no way (sword, bat, umbrella: forward, down; bag: shoulder, elbow)";
    if (std::find(ways.begin(), ways.end(), held.way) != ways.end()) return {};
    return article + prop + "'s way is " + std::string(ways[0]) + " or " + std::string(ways[1]) + " (" + prop + ":" + std::string(ways[0]) + "), not \"" +
           std::string(held.way) + "\"";
}

bool uses_figures_feature(const Json& prim) {
    if (!prim.is_object()) return false;
    const auto kind = prim.find("kind");
    if (kind == prim.end() || !kind->is_string()) return false;
    if (*kind == "figure") {
        if (prim.contains("pose_id") || prim.contains("hold") || prim.contains("props") || prim.contains("pair")) return true;
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

bool uses_figures2_feature(const Json& prim) {
    if (!prim.is_object()) return false;
    const auto kind = prim.find("kind");
    if (kind == prim.end() || !kind->is_string() || *kind != "figure") return false;
    const auto hold = prim.find("hold");
    if (hold == prim.end() || !hold->is_object()) return false;
    for (const auto& [side, what] : hold->items()) {
        if (what.is_string() && held_of(what.get_ref<const std::string&>()).has_way) return true;
    }
    return false;
}

bool uses_figures2_feature(const Document& doc) {
    for (const auto& page : doc.pages) {
        if (!page || !page->prims.is_array()) continue;
        for (const Json& prim : page->prims) {
            if (uses_figures2_feature(prim)) return true;
        }
    }
    return false;
}

}  // namespace genko::core::pose_library
