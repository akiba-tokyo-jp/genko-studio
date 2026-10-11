#include "studio/schemas.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::studio {

using core::Json;

namespace {

// --- schemas.py's builders --------------------------------------------------------------------------------------------

using Props = std::vector<std::pair<std::string, Json>>;

// obj(**props)
Json obj(const Props& props) {
    Json properties = Json::object();
    Json required = Json::array();
    for (const auto& [key, sub] : props) {
        properties[key] = sub;
        required.push_back(key);
    }
    return Json::object({{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}});
}

// more(schema, **later): properties added later, required and nullable like the rest
Json more(const Json& schema, const Props& later) {
    Json out = schema;
    for (const auto& [key, sub] : later) {
        out["properties"][key] = sub;
        out["required"].push_back(key);
    }
    return out;
}

Json arr(const Json& items) { return Json::object({{"type", "array"}, {"items", items}}); }
Json nullable(const Json& schema) { return Json::object({{"anyOf", Json::array({schema, Json::object({{"type", "null"}})})}}); }
Json enumeration(std::initializer_list<const char*> values) {
    Json list = Json::array();
    for (const char* value : values) list.push_back(value);
    return Json::object({{"type", "string"}, {"enum", list}});
}

const Json kStr = Json::object({{"type", "string"}});
const Json kNum = Json::object({{"type", "number"}});
const Json kInt = Json::object({{"type", "integer"}});
const Json kBool = Json::object({{"type", "boolean"}});

Json shots() { return enumeration({"ELS", "LS", "FS", "MS", "MCU", "CU", "ECU", "INSERT"}); }
Json angles() { return enumeration({"eye", "high", "low", "bird", "worm", "dutch"}); }
Json positions() { return enumeration({"left", "left_third", "center", "right_third", "right"}); }
Json facings() { return enumeration({"left", "right", "front", "back"}); }
Json balloons() {
    return enumeration({"speech", "rounded", "thought", "shout", "whisper", "electric", "narration", "sfx", "aside", "dotted_box",
                        "tone_box", "fancy_box"});
}
Json beat_kinds() { return enumeration({"action", "dialogue", "monologue", "narration", "sfx"}); }

// LOOK: the look of one kind of text
Json look() { return obj({{"font", nullable(kStr)}, {"scale", nullable(kNum)}, {"weight", nullable(enumeration({"normal", "bold", "heavy"}))}}); }

Json bible_v1() {
    const Json character = obj({
        {"id", kStr},
        {"name", kStr},
        {"role", kStr},
        {"age", nullable(kInt)},
        {"look", more(obj({{"hair", kStr},
                           {"hair_value", enumeration({"beta", "tone", "white"})},
                           {"eyes", kStr},
                           {"build", kStr},
                           {"height_cm", nullable(kNum)},
                           {"outfits", arr(obj({{"id", kStr}, {"desc", kStr}}))},
                           {"marks", arr(kStr)},
                           {"silhouette", kStr}}),
                      {{"clothes_value", nullable(enumeration({"beta", "tone", "white"}))}})},
        {"speech", obj({{"first_person", kStr}, {"tone", kStr}})},
        {"tokens_en", nullable(kStr)},
    });
    Props lettering;
    for (const char* kind : {"speech", "thought", "shout", "whisper", "narration", "sfx", "title", "aside"}) lettering.emplace_back(kind, nullable(look()));
    return more(obj({{"title", kStr},
                     {"logline", kStr},
                     {"plot", kStr},
                     {"themes", arr(kStr)},
                     {"characters", arr(character)},
                     {"locations", arr(obj({{"id", kStr}, {"name", kStr}, {"desc", kStr}, {"times", arr(kStr)}}))},
                     {"style", obj({{"notes", arr(kStr)}})},
                     {"constraints", arr(kStr)}}),
                {{"author", nullable(kStr)},
                 {"lettering", nullable(obj(lettering))},
                 {"props", nullable(arr(obj({{"id", kStr}, {"name", kStr}, {"desc", kStr}, {"tokens_en", nullable(kStr)}})))}});
}

Json script_v1() {
    return obj({{"scenes", arr(obj({{"id", kStr},
                                    {"summary", kStr},
                                    {"location_id", nullable(kStr)},
                                    {"time", nullable(kStr)},
                                    {"beats", arr(obj({{"id", kStr},
                                                       {"kind", beat_kinds()},
                                                       {"speaker_id", nullable(kStr)},
                                                       {"text", kStr},
                                                       {"emotion", kStr},
                                                       {"page", kInt},
                                                       {"reveal", kBool}}))}}))}});
}

Json name_plan_v1() {
    const Json panel = more(obj({{"slot", kStr},
                                 {"shot", shots()},
                                 {"angle", angles()},
                                 {"location_id", nullable(kStr)},
                                 {"time", nullable(kStr)},
                                 {"characters", arr(obj({{"id", kStr},
                                                         {"pose", kStr},
                                                         {"expression", kStr},
                                                         {"facing", facings()},
                                                         {"pos", positions()},
                                                         {"scale", kNum},
                                                         {"tag", nullable(kStr)}}))},
                                 {"action", kStr},
                                 {"emotion", kStr},
                                 {"fx", arr(kStr)},
                                 {"emphasis", kNum},
                                 {"cross", kBool},
                                 {"beat_ids", arr(kStr)},
                                 {"lines", arr(obj({{"beat_id", kStr}, {"balloon", balloons()}, {"breaks", arr(kStr)}}))}}),
                            {{"props", nullable(arr(kStr))},
                             {"bleed", nullable(kBool)},
                             {"slant", nullable(kNum)},
                             {"sfx_at", nullable(arr(kNum))}});
    const Json tiers = nullable(arr(obj({{"h", kNum},
                                         {"cols", arr(obj({{"slot", kStr}, {"w", kNum}, {"rows", nullable(arr(obj({{"slot", kStr}, {"h", kNum}})))}}))}})));
    return more(obj({{"page", kInt},
                     {"turn_role", enumeration({"normal", "reveal", "hook"})},
                     {"template", nullable(kStr)},
                     {"tiers", tiers},
                     {"panels", arr(panel)}}),
                {{"spread", nullable(kBool)}, {"title", nullable(kBool)}, {"tier_gap_mm", nullable(kNum)}, {"col_gap_mm", nullable(kNum)}});
}

// --- jsonschema_lite --------------------------------------------------------------------------------------------------

Issue error_issue(std::string code, std::string path, std::string message, std::string hint = {}) {
    return Issue{std::move(code), "error", std::move(path), std::move(message), std::move(hint)};
}

std::string escape(const std::string& key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

bool type_fits(const std::string& expected, const Json& value) {
    if (expected == "object") return value.is_object();
    if (expected == "array") return value.is_array();
    if (expected == "string") return value.is_string();
    if (expected == "boolean") return value.is_boolean();
    if (expected == "null") return value.is_null();
    if (expected == "integer") return value.is_number_integer();
    if (expected == "number") return value.is_number();
    throw core::Error("key", core::py_repr_str(expected));  // (_TYPES[expected]: Python's KeyError)
}

std::string describe(const Json& schema) {
    std::string out = "使える型: ";
    const Json* options = core::get(schema, "anyOf");
    bool first = true;
    if (options != nullptr && options->is_array()) {
        for (const Json& option : *options) {
            const Json* type = core::get(option, "type");
            const Json* constant = core::get(option, "const");
            out += (first ? "" : " / ") + core::py_str(type != nullptr ? *type : (constant != nullptr ? *constant : Json("?")));
            first = false;
        }
    }
    return out;
}

void check(const Json& value, const Json& schema, const std::string& path, std::vector<Issue>& issues) {
    const std::string where = path.empty() ? "/" : path;
    if (const Json* options = core::get(schema, "anyOf")) {
        for (const Json& option : core::iterate(*options)) {
            if (validate(value, option, path).empty()) return;
        }
        issues.push_back(error_issue("schema_any_of", where, "どの型にも合わない値です", describe(schema)));
        return;
    }
    if (const Json* constant = core::get(schema, "const"); constant != nullptr && !core::py_equals(value, *constant)) {
        issues.push_back(error_issue("schema_const", where, "値は " + core::py_repr(*constant) + " でなければならない"));
        return;
    }
    if (const Json* values = core::get(schema, "enum")) {
        const std::vector<Json> allowed = core::iterate(*values);
        if (std::none_of(allowed.begin(), allowed.end(), [&value](const Json& v) { return core::py_equals(value, v); })) {
            std::string list;
            for (std::size_t i = 0; i < allowed.size(); ++i) list += (i > 0 ? ", " : "") + core::py_str(allowed[i]);
            issues.push_back(error_issue("schema_enum", where, core::py_repr(value) + " は使えない値です", "使える値: " + list));
            return;
        }
    }
    const Json* type = core::get(schema, "type");
    const std::string expected = type != nullptr && core::py_truthy(*type) ? core::py_str(*type) : std::string();
    if (!expected.empty() && !type_fits(expected, value)) {
        issues.push_back(error_issue("schema_type", where, "型が違う（" + expected + " が必要）"));
        return;
    }
    if (expected == "object") {
        const Json props = core::get_or(schema, "properties", Json::object());
        if (const Json* required = core::get(schema, "required")) {
            for (const Json& key : core::iterate(*required)) {
                const std::string name = core::py_str(key);
                if (!value.contains(name)) issues.push_back(error_issue("schema_required", path + "/" + escape(name), name + " がない"));
            }
        }
        if (const Json* extra = core::get(schema, "additionalProperties"); extra != nullptr && *extra == Json(false)) {
            std::string allowed = "この場所に項目は置けない";
            if (!props.empty()) {
                allowed = "使える項目: ";
                bool first = true;
                for (const auto& [key, sub] : props.items()) {
                    allowed += (first ? "" : ", ") + key;
                    first = false;
                }
            }
            for (const auto& [key, item] : value.items()) {
                if (!props.contains(key)) {
                    issues.push_back(error_issue("schema_unknown_key", path + "/" + escape(key), key + " は定義されていない項目です", allowed));
                }
            }
        }
        for (const auto& [key, sub] : props.items()) {
            if (value.contains(key)) check(value[key], sub, path + "/" + escape(key), issues);
        }
    } else if (expected == "array" && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) check(value[index], schema["items"], path + "/" + std::to_string(index), issues);
    }
}

bool allowed_key(const std::string& key) {
    for (const char* name : {"type", "properties", "required", "additionalProperties", "items", "enum", "const", "anyOf", "description",
                             "title", "$schema", "$id"}) {
        if (key == name) return true;
    }
    return false;
}

}  // namespace

Json Issue::to_json() const {
    return Json::object({{"code", code}, {"severity", severity}, {"path", path}, {"message", message}, {"hint", hint}});
}

bool has_errors(const std::vector<Issue>& issues) {
    return std::any_of(issues.begin(), issues.end(), [](const Issue& issue) { return issue.severity == "error"; });
}

const Json& schema(std::string_view name) {
    static const Json bible = bible_v1();
    static const Json script = script_v1();
    static const Json name_plan = name_plan_v1();
    if (name == "bible@1") return bible;
    if (name == "script@1") return script;
    if (name == "name_plan@1") return name_plan;
    throw core::Error("key", core::py_repr_str(name));
}

Json fill_nulls(const Json& data, const Json& schema) {
    const Json* type = core::get(schema, "type");
    if (data.is_object() && type != nullptr && *type == Json("object")) {
        Json out = data;
        if (const Json* props = core::get(schema, "properties"); props != nullptr && props->is_object()) {
            for (const auto& [key, sub] : props->items()) {
                if (!out.contains(key)) {
                    bool nullable_here = false;
                    if (const Json* options = core::get(sub, "anyOf"); options != nullptr && options->is_array()) {
                        for (const Json& option : *options) {
                            const Json* t = core::get(option, "type");
                            if (t != nullptr && *t == Json("null")) nullable_here = true;
                        }
                    }
                    if (nullable_here) out[key] = nullptr;
                } else {
                    out[key] = fill_nulls(out[key], sub);
                }
            }
        }
        return out;
    }
    if (data.is_array() && type != nullptr && *type == Json("array")) {
        Json out = Json::array();
        const Json items = core::get_or(schema, "items", Json::object());
        for (const Json& item : data) out.push_back(fill_nulls(item, items));
        return out;
    }
    if (const Json* options = core::get(schema, "anyOf"); options != nullptr && !data.is_null()) {
        for (const Json& option : core::iterate(*options)) {
            const Json* t = core::get(option, "type");
            if (t == nullptr || !(*t == Json("null"))) return fill_nulls(data, option);
        }
    }
    return data;
}

std::vector<Issue> validate(const Json& data, const Json& schema, const std::string& path) {
    std::vector<Issue> issues;
    check(data, schema, path, issues);
    return issues;
}

std::vector<std::string> conservative_problems(const Json& schema, const std::string& path) {
    std::vector<std::string> problems;
    if (!schema.is_object()) return problems;
    const std::string where = path.empty() ? "/" : path;
    for (const auto& [key, value] : schema.items()) {
        if (!allowed_key(key)) problems.push_back(where + ": " + key + " は使わない");
    }
    if (const Json* type = core::get(schema, "type"); type != nullptr && *type == Json("object")) {
        const Json props = core::get_or(schema, "properties", Json::object());
        const Json* extra = core::get(schema, "additionalProperties");
        if (extra == nullptr || !(*extra == Json(false))) problems.push_back(where + ": additionalProperties は false にする");
        std::vector<std::string> required;
        if (const Json* given = core::get(schema, "required"); given != nullptr && given->is_array()) {
            for (const Json& key : *given) required.push_back(core::py_str(key));
        }
        std::vector<std::string> keys;
        for (const auto& [key, sub] : props.items()) keys.push_back(key);
        std::sort(required.begin(), required.end());
        std::sort(keys.begin(), keys.end());
        if (required != keys) problems.push_back(where + ": required は全項目にする（任意項目は null を許す）");
        for (const auto& [key, sub] : props.items()) {
            for (std::string& problem : conservative_problems(sub, path + "/properties/" + key)) problems.push_back(std::move(problem));
        }
    }
    if (schema.contains("items")) {
        for (std::string& problem : conservative_problems(schema["items"], path + "/items")) problems.push_back(std::move(problem));
    }
    if (const Json* options = core::get(schema, "anyOf"); options != nullptr && options->is_array()) {
        for (std::size_t index = 0; index < options->size(); ++index) {
            for (std::string& problem : conservative_problems((*options)[index], path + "/anyOf/" + std::to_string(index))) {
                problems.push_back(std::move(problem));
            }
        }
    }
    return problems;
}

}  // namespace genko::studio
