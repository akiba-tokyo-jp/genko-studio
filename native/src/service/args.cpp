#include "service/args.hpp"

#include <vector>

#include "core/error.hpp"

namespace genko::service {

namespace {

std::string names_listed(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += names.size() > 2 ? ", " : " ";
        if (i > 0 && i + 1 == names.size()) out += "and ";
        out += "'" + names[i] + "'";
    }
    return out;
}

}  // namespace

Json bind_arguments(std::string_view qualname, std::span<const Param> params, const Json& positional, const Json& kwargs) {
    const std::string who = std::string(qualname) + "()";
    std::vector<const Param*> ordinary;
    for (const Param& p : params) {
        if (!p.keyword_only) ordinary.push_back(&p);
    }
    const std::size_t given = positional.is_array() ? positional.size() : 0;
    // (as CPython's initialize_locals: the positional arguments bound, then the keyword ones, then too many positional
    // ones, then what is missing)
    Json bound = Json::object();
    std::vector<bool> set(params.size(), false);
    std::size_t at = 0;
    for (std::size_t i = 0; i < params.size() && at < given; ++i) {
        if (params[i].keyword_only) continue;
        bound[params[i].name] = positional[at++];
        set[i] = true;
    }
    if (kwargs.is_object()) {
        for (const auto& [key, value] : kwargs.items()) {
            std::size_t found = params.size();
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (key == params[i].name) found = i;
            }
            if (found == params.size()) throw core::PyTypeError(who + " got an unexpected keyword argument '" + key + "'");
            if (set[found]) throw core::PyTypeError(who + " got multiple values for argument '" + key + "'");
            bound[params[found].name] = value;
            set[found] = true;
        }
    }
    if (given > ordinary.size()) {
        std::size_t required = 0;
        for (const Param* p : ordinary) required += p->fallback ? 0 : 1;
        const std::string takes = required == ordinary.size()
                                      ? std::to_string(ordinary.size() + 1)
                                      : "from " + std::to_string(required + 1) + " to " + std::to_string(ordinary.size() + 1);
        throw core::PyTypeError(who + " takes " + takes + " positional argument" + (ordinary.size() + 1 == 1 && required == ordinary.size() ? "" : "s") +
                                " but " + std::to_string(given + 1) + " were given");
    }
    std::vector<std::string> missing;
    std::vector<std::string> missing_keywords;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (set[i] || params[i].fallback) continue;
        (params[i].keyword_only ? missing_keywords : missing).push_back(params[i].name);
    }
    if (!missing.empty()) {
        throw core::PyTypeError(who + " missing " + std::to_string(missing.size()) + " required positional argument" + (missing.size() == 1 ? "" : "s") +
                                ": " + names_listed(missing));
    }
    if (!missing_keywords.empty()) {
        throw core::PyTypeError(who + " missing " + std::to_string(missing_keywords.size()) + " required keyword-only argument" +
                                (missing_keywords.size() == 1 ? "" : "s") + ": " + names_listed(missing_keywords));
    }
    Json out = Json::object();
    for (std::size_t i = 0; i < params.size(); ++i) out[params[i].name] = set[i] ? bound[params[i].name] : *params[i].fallback;
    return out;
}

}  // namespace genko::service
