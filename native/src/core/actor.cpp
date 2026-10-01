#include "core/actor.hpp"

namespace genko::core {

bool can_approve(std::string_view actor) {
    return actor == kLegacyActor || actor == "human" || actor.starts_with("human:");
}

}  // namespace genko::core
