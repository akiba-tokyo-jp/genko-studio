#include "app/commands_file.hpp"

#include <QAction>
#include <QKeySequence>

#include <algorithm>
#include <set>
#include <vector>

#include "app/key_sets.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/workspace.hpp"

namespace genko::app {

using core::Json;

Json commands_catalogue(MainWindow* window) {
    const auto names = command_names(window);
    const auto places = menu_places(window);
    std::set<const QAction*> tools;
    for (const auto& [tool, act] : window->tool_actions()) tools.insert(act);
    const auto keys = [](const QList<QKeySequence>& list) {
        Json out = Json::array();
        for (const QKeySequence& key : list) out.push_back(key.toString(QKeySequence::PortableText).toStdString());
        return out;
    };
    std::vector<Json> commands;
    for (QAction* action : preferences::commands(window)) {
        const QString name = command_name(names, action);
        const QString attribute = action->property("genko_attribute").toString();
        Json menu = Json::array();
        if (const auto place = places.find(action); place != places.end()) {
            for (const QString& part : place->second.split(QStringLiteral(" → "), Qt::SkipEmptyParts)) menu.push_back(part.toStdString());
        }
        Json old = Json::array();
        for (const QString& one : preferences::old_names(name)) old.push_back(one.toStdString());
        commands.push_back(Json{{"id", (attribute.isEmpty() ? name : attribute).toStdString()},
                                {"name", name.toStdString()},
                                {"old_names", std::move(old)},
                                {"menu", std::move(menu)},
                                {"keys", Json{{"photoshop", keys(key_sets::keys_of(action, QStringLiteral("photoshop")))},
                                              {"genko", keys(key_sets::keys_of(action, QStringLiteral("genko")))}}},
                                {"checkable", action->isCheckable()},
                                {"tool", tools.contains(action)}});
    }
    std::stable_sort(commands.begin(), commands.end(), [](const Json& a, const Json& b) { return a["name"].get<std::string>() < b["name"].get<std::string>(); });
    Json listed = Json::array();
    for (Json& one : commands) listed.push_back(std::move(one));
    return Json{{"v", 1}, {"commands", std::move(listed)}};
}

}  // namespace genko::app
