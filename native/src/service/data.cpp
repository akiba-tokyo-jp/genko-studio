#include "service/data.hpp"

#include <cstddef>
#include <string_view>

#include "core/json.hpp"

namespace genko::service::detail {
extern const unsigned char kLayoutsJson[];
extern const std::size_t kLayoutsJsonSize;
extern const unsigned char kVocabJson[];
extern const std::size_t kVocabJsonSize;
extern const unsigned char kRulesMd[];
extern const std::size_t kRulesMdSize;
extern const unsigned char kDrawingMd[];
extern const std::size_t kDrawingMdSize;
extern const unsigned char kSkillMd[];
extern const std::size_t kSkillMdSize;
}  // namespace genko::service::detail

namespace genko::service::data {

namespace {

std::string_view text(const unsigned char* bytes, std::size_t size) { return {reinterpret_cast<const char*>(bytes), size}; }

}  // namespace

const core::Json& layouts() {
    static const core::Json parsed = core::parse_python_json(std::string(text(detail::kLayoutsJson, detail::kLayoutsJsonSize)));
    return parsed;
}

const core::Json& vocab() {
    static const core::Json parsed = core::parse_python_json(std::string(text(detail::kVocabJson, detail::kVocabJsonSize)));
    return parsed;
}

const std::string& rules() {
    static const std::string kept(text(detail::kRulesMd, detail::kRulesMdSize));
    return kept;
}

const std::string& drawing() {
    static const std::string kept(text(detail::kDrawingMd, detail::kDrawingMdSize));
    return kept;
}

const std::string& skill() {
    static const std::string kept(text(detail::kSkillMd, detail::kSkillMdSize));
    return kept;
}

}  // namespace genko::service::data
