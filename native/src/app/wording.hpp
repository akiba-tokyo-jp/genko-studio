#pragma once

#include <QString>

#include <string>
#include <string_view>

#include "core/model.hpp"

// The words people see (Python's genko/app/wording.py): stages, layers, actors, the save states and error messages.
// Internal ids and op names stay in the data; the screens show these instead.

namespace genko::app::wording {

// STAGE: name → ネーム, ink → 作画, finish → 仕上げ (the key itself when unknown).
QString stage(std::string_view key);

// LAYER: a layer's role in words.
QString layer_role(std::string_view role);

// ai:hermes → AI（hermes）, human:leaf → leaf, "" → 不明.
QString actor(std::string_view name);

// A layer as people call it: its title when a person gave one, else its role (絵（配置） for placed art).
QString layer_label(const core::Layer& layer);

// An ApplyError message in plain Japanese (unknown ones are kept, after a short lead).
QString error(const QString& message);
QString error(const std::string& message);

}  // namespace genko::app::wording
