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
    // (PC5 J-g, SPEC.md PROD-01「制御チャンネル」) An actor that keeps its name — the journal says who it is — with an
    // agent's rights: it never approves, and the person's rules (strict_gates, the person's own ops and fields) refuse it
    // as they refuse ai:…, whatever the name (a human: token on the control channel works as MCP's apply_ops).
    static Actor with_agent_rights(std::string name) {
        Actor actor(std::move(name));
        actor.agent_rights_ = true;
        return actor;
    }

    const std::string& name() const { return name_; }
    bool can_approve() const { return !agent_rights_ && core::can_approve(name_); }
    bool agent_rights() const { return agent_rights_; }

private:
    std::string name_;
    bool agent_rights_ = false;
};

}  // namespace genko::core
