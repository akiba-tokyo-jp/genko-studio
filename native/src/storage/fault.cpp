#include "storage/fault.hpp"

#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/error.hpp"
#include "storage/fsutil.hpp"

#ifndef GENKO_FAULT_INJECTION
#define GENKO_FAULT_INJECTION 0
#endif

namespace genko::storage::fault {

namespace {

thread_local Stage current_stage = Stage::none;

#if GENKO_FAULT_INJECTION

enum class Action { fail, late, crash, torn };

using Spec = std::map<Stage, Action>;

Spec parse(std::string_view text) {
    Spec spec;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        if (item.empty()) continue;
        const auto colon = item.find(':');
        if (colon == std::string_view::npos) throw core::Error("value", "GENKO_FAULT: expected <stage>:<action>, got " + std::string(item));
        const std::string_view stage_name = item.substr(0, colon);
        const std::string_view action_name = item.substr(colon + 1);
        Stage stage = Stage::none;
        if (stage_name == "assets") stage = Stage::assets;
        else if (stage_name == "prepare") stage = Stage::prepare;
        else if (stage_name == "project") stage = Stage::project;
        else if (stage_name == "audit") stage = Stage::audit;
        else if (stage_name == "commit") stage = Stage::commit;
        else throw core::Error("value", "GENKO_FAULT: unknown stage " + std::string(stage_name));
        Action action = Action::fail;
        if (action_name == "fail") action = Action::fail;
        else if (action_name == "late") action = Action::late;
        else if (action_name == "crash") action = Action::crash;
        else if (action_name == "torn") action = Action::torn;
        else throw core::Error("value", "GENKO_FAULT: unknown action " + std::string(action_name));
        spec[stage] = action;
    }
    return spec;
}

std::mutex spec_mutex;
std::optional<Spec> spec_override;
std::optional<Spec> spec_from_env;

std::optional<Action> action_for(Stage stage) {
    if (stage == Stage::none) return std::nullopt;
    std::lock_guard<std::mutex> guard(spec_mutex);
    const Spec* spec = nullptr;
    if (spec_override) {
        spec = &*spec_override;
    } else {
        if (!spec_from_env) {
            const char* value = std::getenv("GENKO_FAULT");
            spec_from_env = parse(value != nullptr ? std::string_view(value) : std::string_view());
        }
        spec = &*spec_from_env;
    }
    const auto it = spec->find(stage);
    if (it == spec->end()) return std::nullopt;
    return it->second;
}

[[noreturn]] void no_space(const std::filesystem::path& target) {
    throw core::Error("io", os_error_text(ENOSPC, target), path_to_utf8(target));
}

#endif

}  // namespace

bool compiled_in() { return GENKO_FAULT_INJECTION != 0; }

StageScope::StageScope(Stage stage) : previous_(current_stage) { current_stage = stage; }

StageScope::~StageScope() { current_stage = previous_; }

#if GENKO_FAULT_INJECTION

void before_write(const std::filesystem::path& target) {
    if (action_for(current_stage) == Action::fail) no_space(target);
}

std::size_t bytes_to_write(std::size_t size) {
    return action_for(current_stage) == Action::torn ? size / 2 : size;
}

void after_write(const std::filesystem::path& target) {
    const auto action = action_for(current_stage);
    if (action == Action::crash || action == Action::torn) std::_Exit(77);
    if (action == Action::late) no_space(target);
}

void set_for_testing(std::string_view spec) {
    Spec parsed = parse(spec);
    std::lock_guard<std::mutex> guard(spec_mutex);
    spec_override = std::move(parsed);
}

#else

void before_write(const std::filesystem::path&) {}
std::size_t bytes_to_write(std::size_t size) { return size; }
void after_write(const std::filesystem::path&) {}
void set_for_testing(std::string_view) {}

#endif

}  // namespace genko::storage::fault
