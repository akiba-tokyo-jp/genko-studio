#pragma once

#include <QString>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// The words people see (Python's genko/app/wording.py): stages, layers, actors, the save states and error messages.
// Internal ids and op names stay in the data; the screens show these instead.

namespace genko::wording {

// STAGE: name → ネーム, ink → 作画, finish → 仕上げ (the key itself when unknown).
QString stage(std::string_view key);

// LAYER: a layer's role in words.
QString layer_role(std::string_view role);

// ai:hermes → AI（hermes）, human:leaf → leaf, "" → 不明.
QString actor(std::string_view name);

// PANEL: a panel's state in words (null and "empty": 未着手, …; the state itself when unknown, str() of it).
QString panel_status(const core::Json& status);

// GATE: name → ネーム, art → 作画, sheet → 設定画, export → 書き出し (the key itself when unknown).
QString gate(std::string_view key);

// REGION: the kinds of a panel's region, in Python's order (face → 顔, person → 人物, keep → 空けておく).
const std::vector<std::pair<QString, QString>>& regions();
// REGION_LABEL.get(kind, kind)
QString region_label(const core::Json& kind);

// A layer as people call it: its title when a person gave one, else its role (絵（配置） for placed art).
QString layer_label(const core::Layer& layer);

// What this build does not draw or do yet (render::NotYetPorted's element: "placed", "adjust:<kind>", …) in words.
QString unported_element(const QString& element);

// An ApplyError message in plain Japanese (unknown ones are kept, after a short lead).
QString error(const QString& message);
QString error(const std::string& message);

// wording._inner: a reason inside another message, in Japanese when it is known, else as it is (no lead).
QString reason(const QString& text);

}  // namespace genko::wording
