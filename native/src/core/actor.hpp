#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace genko::core {

// Who makes a change (Python's agent strings): "human:<name>", "ai:<name>", "legacy:unknown", … The unnamed caller
// of the command line is "genko" (Python's LEGACY_ACTOR) and is treated as a person.
inline constexpr std::string_view kLegacyActor = "genko";

// Gates and unlocking other people's pages need a person: "human:<name>", "human" or the legacy unnamed caller
// (Python's ops.can_approve).
bool can_approve(std::string_view actor);

class Actor {
public:
    Actor() : name_(kLegacyActor) {}
    explicit Actor(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    bool can_approve() const { return core::can_approve(name_); }

private:
    std::string name_;
};

}  // namespace genko::core
