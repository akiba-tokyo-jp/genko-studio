#include "core/variables.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& message) { throw Error("value", "variables: " + message); }

bool is_ref(const std::string& ref) {
    if (ref.size() != 71 || ref.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(ref.begin() + 7, ref.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Characters of UTF-8 text (its bytes that do not continue one).
std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

void only_keys(const Json& data, std::initializer_list<std::string_view> keys, const std::string& what) {
    if (!data.is_object()) bad(what + " must be an object");
    for (const auto& [key, value] : data.items()) {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) bad(what + " has an unknown key: " + key);
    }
}

const std::string& text_of(const Json& data, const char* key, const std::string& what) {
    const Json* v = get(data, key);
    if (v == nullptr || !v->is_string()) bad(what + "'s " + key + " must be text");
    return v->get_ref<const std::string&>();
}

std::optional<std::string> maybe_text(const Json& data, const char* key, const std::string& what) {
    const Json* v = get(data, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_string() || v->get_ref<const std::string&>().empty()) bad(what + "'s " + key + " must be an id");
    return v->get<std::string>();
}

std::string name_of(const Json& data, const std::string& what) {
    std::string name = text_of(data, "name", what);
    const std::size_t n = characters(name);
    if (n == 0 || n > kVariableNameMax) bad(what + "'s name must be 1 to " + std::to_string(kVariableNameMax) + " characters");
    return name;
}

// (4 decimals, a whole number written as one: the writer's numbers)
Json round4(double v) {
    const double r = std::round(v * 10000.0) / 10000.0;
    if (r == std::floor(r) && std::abs(r) < 1e15) return Json(static_cast<std::int64_t>(r));
    return Json(r);
}

template <class List>
bool listed(const List& list, std::string_view value) {
    return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

VariableDef def_from_json(const Json& data, std::size_t i) {
    const std::string what = "def " + std::to_string(i);
    only_keys(data, {"name", "type", "page", "layer", "line", "method", "align", "clip", "box_mm"}, what);
    VariableDef def;
    def.name = name_of(data, what);
    def.type = text_of(data, "type", what);
    if (def.type != "visibility" && def.type != "text" && def.type != "pixels") bad(what + "'s type must be visibility, text or pixels");
    def.page = maybe_text(data, "page", what);
    def.layer = maybe_text(data, "layer", what);
    def.line = maybe_text(data, "line", what);
    if (def.line) {
        if (def.type != "text") bad(what + " names a line: only a text variable does");
        if (def.page || def.layer) bad(what + " names a line or a page's layer, not both");
    } else if (!def.page || !def.layer) {
        bad(what + " names a page and its layer (or, for text, a line)");
    }
    const bool pixels = def.type == "pixels";
    for (const char* key : {"method", "align", "clip", "box_mm"}) {
        if (pixels != data.contains(key)) bad(what + (pixels ? " (pixels) must say its " : " takes no ") + key);
    }
    if (pixels) {
        def.method = text_of(data, "method", what);
        if (!listed(kVariableMethods, def.method)) bad(what + "'s method must be fit, fill, as_is or conform");
        def.align = text_of(data, "align", what);
        if (!listed(kVariableAligns, def.align)) bad(what + "'s align must be one of the nine places (top_left … bottom_right)");
        const Json& clip = data["clip"];
        if (!clip.is_boolean()) bad(what + "'s clip must be true or false");
        def.clip = clip.get<bool>();
        const Json& box = data["box_mm"];
        if (!box.is_array() || box.size() != 4) bad(what + "'s box_mm must be [x0, y0, x1, y1]");
        for (std::size_t k = 0; k < 4; ++k) {
            if (!box[k].is_number() || !std::isfinite(box[k].get<double>()) || std::abs(box[k].get<double>()) > kVariableBoxMaxMm)
                bad(what + "'s box_mm must be numbers within " + std::to_string(static_cast<int>(kVariableBoxMaxMm)) + " mm");
            def.box_mm[k] = box[k].get<double>();
        }
        if (!(def.box_mm[2] > def.box_mm[0]) || !(def.box_mm[3] > def.box_mm[1])) bad(what + "'s box_mm must be a box (x0 < x1, y0 < y1)");
    }
    return def;
}

DataValue value_from_json(const Json& data, const std::string& what) {
    only_keys(data, {"variable", "value"}, what);
    DataValue value;
    value.variable = text_of(data, "variable", what);
    if (value.variable.empty()) bad(what + " names no variable");
    const Json* v = get(data, "value");
    if (v == nullptr) bad(what + " has no value");
    if (v->is_boolean()) {
        value.value = v->get<bool>();
    } else if (v->is_string()) {
        if (v->get_ref<const std::string&>().size() > kVariableTextMax) bad(what + "'s text is longer than " + std::to_string(kVariableTextMax) + " bytes");
        value.value = v->get<std::string>();
    } else if (v->is_object()) {
        only_keys(*v, {"asset", "suffix", "name"}, what + "'s picture");
        PictureValue picture;
        picture.asset = text_of(*v, "asset", what + "'s picture");
        if (!is_ref(picture.asset)) bad(what + "'s picture must be an asset (sha256: and 64 hex digits)");
        picture.suffix = text_of(*v, "suffix", what + "'s picture");
        if (!smart::format_of_suffix(picture.suffix)) bad(what + "'s picture's suffix " + picture.suffix + " is not a picture's");
        if (v->contains("name")) {
            picture.name = text_of(*v, "name", what + "'s picture");
            if (picture.name.empty() || characters(picture.name) > kVariableNameMax) bad(what + "'s picture's name must be 1 to 255 characters");
        }
        value.value = std::move(picture);
    } else {
        bad(what + "'s value must be true or false, text or a picture");
    }
    return value;
}

}  // namespace

Variables variables_from_json(const Json& data) {
    only_keys(data, {"defs", "data_sets", "active"}, "the variables");
    Variables out;
    const Json* defs = get(data, "defs");
    const Json* sets = get(data, "data_sets");
    if (defs == nullptr || !defs->is_array()) bad("defs must be a list");
    if (sets == nullptr || !sets->is_array()) bad("data_sets must be a list");
    if (defs->size() > kVariableDefsMax) bad("at most " + std::to_string(kVariableDefsMax) + " defs");
    if (sets->size() > kDataSetsMax) bad("at most " + std::to_string(kDataSetsMax) + " data sets");
    std::set<std::string> names;
    for (std::size_t i = 0; i < defs->size(); ++i) {
        VariableDef def = def_from_json((*defs)[i], i);
        if (!names.insert(def.name).second) bad("two variables are named " + def.name);
        out.defs.push_back(std::move(def));
    }
    std::set<std::string> set_names;
    for (std::size_t i = 0; i < sets->size(); ++i) {
        const Json& item = (*sets)[i];
        const std::string what = "data set " + std::to_string(i);
        only_keys(item, {"name", "values"}, what);
        DataSet set;
        set.name = name_of(item, what);
        if (!set_names.insert(set.name).second) bad("two data sets are named " + set.name);
        const Json* values = get(item, "values");
        if (values == nullptr || !values->is_array()) bad(what + "'s values must be a list");
        if (values->size() > kVariableDefsMax) bad(what + " has more values than a book has variables");
        std::set<std::string> seen;
        for (std::size_t j = 0; j < values->size(); ++j) {
            DataValue value = value_from_json((*values)[j], what + " value " + std::to_string(j));
            if (!seen.insert(value.variable).second) bad(what + " gives " + value.variable + " two values");
            set.values.push_back(std::move(value));
        }
        out.data_sets.push_back(std::move(set));
    }
    if (const Json* active = get(data, "active"); active != nullptr && !active->is_null()) {
        if (!active->is_number_integer() || active->get<std::int64_t>() < 0 ||
            static_cast<std::uint64_t>(active->get<std::int64_t>()) >= out.data_sets.size()) {
            bad("active must be the index of one of its data sets");
        }
        out.active = active->get<std::int64_t>();
    }
    return out;
}

Json variables_to_json(const Variables& variables) {
    Json defs = Json::array();
    for (const VariableDef& def : variables.defs) {
        Json item = Json::object();
        item["name"] = def.name;
        item["type"] = def.type;
        if (def.line) {
            item["line"] = *def.line;
        } else {
            item["page"] = def.page.value_or(std::string());
            item["layer"] = def.layer.value_or(std::string());
        }
        if (def.type == "pixels") {
            item["method"] = def.method;
            item["align"] = def.align;
            item["clip"] = def.clip;
            Json box = Json::array();
            for (const double v : def.box_mm) box.push_back(round4(v));
            item["box_mm"] = std::move(box);
        }
        defs.push_back(std::move(item));
    }
    Json sets = Json::array();
    for (const DataSet& set : variables.data_sets) {
        Json values = Json::array();
        for (const DataValue& value : set.values) {
            Json item = Json::object();
            item["variable"] = value.variable;
            if (const bool* shown = std::get_if<bool>(&value.value)) {
                item["value"] = *shown;
            } else if (const std::string* text = std::get_if<std::string>(&value.value)) {
                item["value"] = *text;
            } else {
                const PictureValue& picture = std::get<PictureValue>(value.value);
                Json p = Json::object({{"asset", picture.asset}, {"suffix", picture.suffix}});
                if (!picture.name.empty()) p["name"] = picture.name;
                item["value"] = std::move(p);
            }
            values.push_back(std::move(item));
        }
        sets.push_back(Json::object({{"name", set.name}, {"values", std::move(values)}}));
    }
    Json out = Json::object({{"defs", std::move(defs)}, {"data_sets", std::move(sets)}});
    if (variables.active) out["active"] = *variables.active;
    return out;
}

Variables variables_of(const Document& doc) {
    if (!doc.variables) return {};
    return variables_from_json(*doc.variables);
}

void set_variables(Document& doc, Variables variables) {
    if (variables.active && (*variables.active < 0 || static_cast<std::size_t>(*variables.active) >= variables.data_sets.size())) variables.active.reset();
    if (variables.empty()) {
        doc.variables.reset();
        return;
    }
    doc.variables = variables_to_json(variables);
}

bool value_fits(const std::string& type, const DataValue& value) {
    if (type == "visibility") return std::holds_alternative<bool>(value.value);
    if (type == "text") return std::holds_alternative<std::string>(value.value);
    return std::holds_alternative<PictureValue>(value.value);
}

const VariableDef* find_def(const Variables& variables, std::string_view name) {
    for (const VariableDef& def : variables.defs) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

std::optional<std::size_t> find_data_set(const Variables& variables, std::string_view name) {
    for (std::size_t i = 0; i < variables.data_sets.size(); ++i) {
        if (variables.data_sets[i].name == name) return i;
    }
    return std::nullopt;
}

bool uses_variables(const Document& doc) { return doc.variables.has_value() && !doc.variables->is_null(); }

void validate_variables(const Document& doc) {
    if (!uses_variables(doc)) return;
    const Variables variables = variables_from_json(*doc.variables);
    if (variables.empty()) bad("a book without defs or data sets has no variables");
    if (variables_to_json(variables) != *doc.variables) bad("the variables are not in their normal form");
}

std::vector<std::pair<std::string, std::string>> variable_pictures(const Document& doc) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!uses_variables(doc)) return out;
    std::set<std::string> seen;
    for (const DataSet& set : variables_from_json(*doc.variables).data_sets) {
        for (const DataValue& value : set.values) {
            if (const PictureValue* picture = std::get_if<PictureValue>(&value.value); picture != nullptr && seen.insert(picture->asset).second) {
                out.emplace_back(picture->asset, picture->suffix);
            }
        }
    }
    return out;
}

}  // namespace genko::core
