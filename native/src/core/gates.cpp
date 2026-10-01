#include "core/gates.hpp"

#include <cstdint>
#include <map>
#include <string>

#include "core/pyconv.hpp"

namespace genko::core {

namespace {

const Json* get(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

bool truthy_at(const Json& object, const char* key) {
    const Json* v = get(object, key);
    return v != nullptr && py_truthy(*v);
}

// A gate's value as Python compares it: a bool or an int (False == 0).
std::int64_t as_int(const Json& value) {
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number_integer()) return value.get<std::int64_t>();
    return 0;
}

}  // namespace

Json gates_of(const Json* payload) {
    Json out = Json::object();
    if (payload == nullptr || !payload->is_object() || payload->empty()) return out;  // (Python: `if not payload`)
    std::map<std::string, Json> gates;
    if (const Json* pages = get(*payload, "pages"); pages != nullptr && pages->is_array()) {
        for (const Json& page : *pages) {
            if (!page.is_object()) continue;
            std::string key;
            if (const Json* id = get(page, "id"); id != nullptr && py_truthy(*id)) {
                key = py_str(*id);
            } else {
                const Json* index = get(page, "index");
                key = "index:" + (index != nullptr ? py_str(*index) : std::string("None"));
            }
            gates["page:" + key + ":name_ok"] = truthy_at(page, "name_ok");
            gates["page:" + key + ":art_ok"] = truthy_at(page, "art_ok");
        }
    }
    if (const Json* bible = get(*payload, "bible"); bible != nullptr && py_truthy(*bible)) {
        if (const Json* characters = get(*bible, "characters"); characters != nullptr && characters->is_array()) {
            for (const Json& character : *characters) {
                if (!character.is_object()) continue;
                const Json* id = get(character, "id");
                gates["character:" + (id != nullptr ? py_str(*id) : std::string("None")) + ":locked"] =
                    truthy_at(character, "locked");
            }
        }
    }
    std::int64_t approvals = 0;
    if (const Json* studio = get(*payload, "studio"); studio != nullptr && py_truthy(*studio)) {
        if (const Json* list = get(*studio, "approvals"); list != nullptr && list->is_array()) {
            for (const Json& approval : *list) {
                const Json* gate = get(approval, "gate");
                if (gate != nullptr && gate->is_string() && gate->get_ref<const std::string&>() == "export" &&
                    !truthy_at(approval, "revoked")) {
                    ++approvals;
                }
            }
        }
    }
    gates["export_approvals"] = approvals;
    for (auto& [key, value] : gates) out[key] = std::move(value);
    return out;
}

Json gate_changes(const Json* before, const Json& after) {
    const Json old_gates = gates_of(before);
    const Json new_gates = gates_of(&after);
    Json out = Json::array();
    for (const auto& [key, value] : new_gates.items()) {
        const auto old = old_gates.find(key);
        const std::int64_t was = old == old_gates.end() ? 0 : as_int(*old);  // (a missing key is False, or 0)
        if (was == as_int(value)) continue;
        Json change = Json::object();
        change["what"] = key;
        change["from"] = old == old_gates.end() ? Json(nullptr) : *old;
        change["to"] = value;
        out.push_back(std::move(change));
    }
    return out;
}

}  // namespace genko::core
