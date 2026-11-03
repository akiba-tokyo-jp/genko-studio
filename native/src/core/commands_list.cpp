#include "core/commands_list.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

#include "core/prefs_table.hpp"

namespace genko::core::detail {
extern const unsigned char kCommandsJson[];
extern const std::size_t kCommandsJsonSize;
}  // namespace genko::core::detail

namespace genko::core::commands {

namespace {

using Strings = std::vector<std::string>;

QString q(const std::string& text) { return QString::fromStdString(text); }

Strings keys_in(const QString& text) {
    Strings out;
    for (const QString& one : text.split(QLatin1Char('|'), Qt::SkipEmptyParts)) out.push_back(one.toStdString());
    return out;
}

Strings strings(const Json& list) {
    Strings out;
    if (!list.is_array()) return out;
    for (const Json& one : list) {
        if (one.is_string()) out.push_back(one.get<std::string>());
    }
    return out;
}

bool reused(const std::string& name) {
    const auto& all = prefs::reused_names();
    return std::find(all.begin(), all.end(), name) != all.end();
}

// (app/preferences' names_current: a key kept under a reused name by a build that knows the new command — marked so)
bool current(QSettings& settings, const QString& key) {
    const QString mark = QStringLiteral("names_current/") + key;
    return settings.contains(mark) && settings.value(mark).toString() == settings.value(key).toString();
}

QString lower(const QString& text) { return text.toCaseFolded(); }

}  // namespace

std::string_view catalogue_text() { return {reinterpret_cast<const char*>(detail::kCommandsJson), detail::kCommandsJsonSize}; }

const Json& catalogue() {
    static const Json all = Json::parse(catalogue_text());
    return all;
}

Json list(QSettings& settings, std::string_view menu, std::string_view words) {
    // the key set chosen (keyboardShortcuts.keySet), and the person's own set over it (keyboardShortcuts.ownSet: its base
    // and its keys by command name — app/key_sets)
    std::string set = prefs::value(settings, "keyboardShortcuts.keySet").get<std::string>();
    std::string chosen = set;
    std::map<std::string, Strings> own;
    const QString own_name = settings.value(QStringLiteral("ui/key_set_own")).toString();
    if (!own_name.isEmpty() && settings.contains(QStringLiteral("key_sets/%1/base").arg(own_name))) {
        const std::string base = settings.value(QStringLiteral("key_sets/%1/base").arg(own_name)).toString().toStdString();
        set = base == "genko" ? "genko" : "photoshop";
        chosen = "own:" + own_name.toStdString();
        const QJsonObject kept = QJsonDocument::fromJson(settings.value(QStringLiteral("key_sets/%1/keys").arg(own_name)).toString().toUtf8()).object();
        for (auto it = kept.begin(); it != kept.end(); ++it) own[it.key().toStdString()] = keys_in(it.value().toString());
    }
    struct Row {
        const Json* command;
        Strings keys;
        bool person = false;  // (keys the person gave it in 環境設定)
    };
    std::vector<Row> rows;
    std::map<std::string, std::string> taken;  // (a key the person gave a command: the command's name)
    for (const Json& c : catalogue()["commands"]) {
        const std::string name = c["name"].get<std::string>();
        const Strings old = strings(c.value("old_names", Json::array()));
        Row row{&c, strings(c["keys"].value(set, Json::array())), false};
        if (!own.empty()) {
            auto found = own.find(name);
            for (std::size_t i = 0; found == own.end() && i < old.size(); ++i) found = own.find(old[i]);
            if (found != own.end()) row.keys = found->second;
        }
        // (the person's keys: under its name — a reused name's only when marked as this build's —, or an old name)
        QString key = QStringLiteral("shortcuts/") + q(name);
        if (reused(name) && !current(settings, key)) key = QStringLiteral("shortcuts_unread/") + q(name);
        for (std::size_t i = 0; !settings.contains(key) && i < old.size(); ++i) {
            if (settings.contains(QStringLiteral("shortcuts/") + q(old[i]))) key = QStringLiteral("shortcuts/") + q(old[i]);
        }
        if (settings.contains(key)) {
            row.keys = keys_in(settings.value(key).toString());
            row.person = true;
            for (const std::string& k : row.keys) taken.emplace(k, name);
        }
        rows.push_back(std::move(row));
    }
    // (a set's key the person gave another command is taken off this one, as the app does)
    for (Row& row : rows) {
        if (row.person) continue;
        const std::string name = (*row.command)["name"].get<std::string>();
        std::erase_if(row.keys, [&](const std::string& k) {
            const auto other = taken.find(k);
            return other != taken.end() && other->second != name;
        });
    }
    const QString wanted_menu = q(std::string(menu)).trimmed();
    const QStringList wanted_words = lower(q(std::string(words))).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    Json out = Json::array();
    for (const Row& row : rows) {
        const Json& c = *row.command;
        const Strings place = strings(c["menu"]);
        QStringList path;
        for (const std::string& p : place) path << q(p);
        if (!wanted_menu.isEmpty()) {
            const QString joined = path.join(QStringLiteral(" → "));
            if (joined != wanted_menu && !joined.startsWith(wanted_menu + QStringLiteral(" → "))) continue;
        }
        if (!wanted_words.isEmpty()) {
            QString hay = q(c["name"].get<std::string>()) + QLatin1Char(' ') + path.join(QLatin1Char(' '));
            for (const std::string& o : strings(c.value("old_names", Json::array()))) hay += QLatin1Char(' ') + q(o);
            hay = lower(hay);
            if (!std::all_of(wanted_words.begin(), wanted_words.end(), [&hay](const QString& w) { return hay.contains(w); })) continue;
        }
        Json one{{"id", c["id"]}, {"name", c["name"]}, {"menu", c["menu"]}, {"keys", row.keys}, {"checkable", c.value("checkable", false)}};
        if (c.value("tool", false)) one["tool"] = true;
        out.push_back(std::move(one));
    }
    return Json{{"ok", true}, {"key_set", chosen}, {"commands", std::move(out)}};
}

}  // namespace genko::core::commands
