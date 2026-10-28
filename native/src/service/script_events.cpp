// スクリプトイベントマネージャーの表 (PC5 I-0; PhotoCraft v0.2.0 engine/src/automate_cmds.rs 141-260, MIT).

#include "service/script_events.hpp"

#include <QString>

#include <array>
#include <system_error>

#include "core/config_dir.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "service/actions.hpp"
#include "service/automate.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::script_events {

using core::Json;

namespace {

struct Named {
    Event event;
    std::string_view id;
    const char* label;
};
// (Photoshop's スクリプトイベントマネージャー's events, PhotoCraft's EVENTS; the Japanese names not checked against
// Photoshop's own: design 節 I 9)
constexpr std::array<Named, 8> kEvents{{{Event::StartApplication, "startApplication", "アプリケーションを起動"},
                                        {Event::NewDocument, "newDocument", "新規ドキュメント"},
                                        {Event::OpenDocument, "openDocument", "ドキュメントを開く"},
                                        {Event::SaveDocument, "saveDocument", "ドキュメントを保存"},
                                        {Event::CloseDocument, "closeDocument", "ドキュメントを閉じる"},
                                        {Event::Print, "print", "ドキュメントをプリント"},
                                        {Event::Export, "export", "ドキュメントを書き出し"},
                                        {Event::Everything, "everything", "すべて"}}};

int g_firing = 0;  // (the app's thread raises events)

std::optional<std::string> text_at(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) throw core::Error("value", std::string("a binding's ") + key + " is a text");
    return it->get<std::string>();
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

}  // namespace

std::string_view id_of(Event event) {
    for (const Named& n : kEvents) {
        if (n.event == event) return n.id;
    }
    return "everything";
}

std::optional<Event> event_of(std::string_view id) {
    for (const Named& n : kEvents) {
        if (n.id == id) return n.event;
    }
    return std::nullopt;
}

const std::vector<Event>& events() {
    static const std::vector<Event> out = [] {
        std::vector<Event> list;
        for (const Named& n : kEvents) list.push_back(n.event);
        return list;
    }();
    return out;
}

std::string label_of(Event event) {
    for (const Named& n : kEvents) {
        if (n.event == event) return n.label;
    }
    return {};
}

fs::path path() { return core::config_dir() / "script_events.json"; }

fs::path seal_path() { return core::config_dir() / "script_events.sha256"; }

Json to_json(const Table& table) {
    Json out = Json::object();
    out["enabled"] = table.enabled;
    Json list = Json::array();
    for (const Binding& b : table.bindings) {
        Json item = Json::object();
        item["event"] = b.event;
        item["name"] = b.name;
        if (b.script) item["script"] = *b.script;
        if (b.sha256) item["sha256"] = *b.sha256;
        if (b.action) item["action"] = *b.action;
        list.push_back(std::move(item));
    }
    out["bindings"] = std::move(list);
    return out;
}

Table from_json(const Json& json) {
    if (!json.is_object()) throw core::Error("value", "the script events are an object {\"enabled\", \"bindings\"}");
    Table table;
    if (const auto it = json.find("enabled"); it != json.end()) {
        if (!it->is_boolean()) throw core::Error("value", "enabled is true or false");
        table.enabled = it->get<bool>();
    }
    const auto list = json.find("bindings");
    if (list == json.end()) return table;
    if (!list->is_array()) throw core::Error("value", "bindings is a list");
    if (list->size() > kMaxBindings) throw core::Error("value", "more than " + std::to_string(kMaxBindings) + " bindings");
    for (const Json& item : *list) {
        if (!item.is_object()) throw core::Error("value", "a binding is an object {\"event\", \"name\", \"script\" | \"action\"}");
        Binding b;
        const auto event = text_at(item, "event");
        if (!event || !event_of(*event)) throw core::Error("value", "a binding's event is one of the events (startApplication … everything)");
        b.event = *event;
        b.name = text_at(item, "name").value_or(std::string());
        b.script = text_at(item, "script");
        b.sha256 = text_at(item, "sha256");
        b.action = text_at(item, "action");
        if (b.script.has_value() == b.action.has_value()) throw core::Error("value", "a binding runs a script or an action");
        if (b.script && !b.sha256) throw core::Error("value", "a binding's script is bound with its sha256");
        table.bindings.push_back(std::move(b));
    }
    return table;
}

Loaded load() {
    Loaded out;
    std::error_code ec;
    if (!fs::exists(path(), ec)) return out;
    std::string bytes;
    try {
        const auto size = fs::file_size(path(), ec);
        if (!ec && size > kMaxBytes) throw core::Error("value", "the file is larger than 1 MiB");
        bytes = storage::read_file_bounded(path(), kMaxBytes);
        core::ParseOptions options;
        options.max_depth = 16;
        out.table = from_json(core::parse_python_json(bytes, nullptr, options));
    } catch (const std::exception& error) {
        out.table = Table{};
        out.problem = QStringLiteral("スクリプトイベントの設定を読めないので、無効にしました（%1）").arg(wording::error(std::string(error.what()))).toStdString();
        return out;
    }
    std::string seal;
    try {
        seal = trimmed(storage::read_file_bounded(seal_path(), 256));
    } catch (const std::exception&) {
    }
    if (seal != storage::sha256_hex(bytes)) {
        out.table.enabled = false;
        out.problem = "スクリプトイベントの設定が Genko の外で変わりました。中身を確かめてから、スクリプトイベントマネージャーで有効にしてください";
    }
    return out;
}

std::optional<std::string> save(const Table& table) {
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    const std::string bytes = core::dump(to_json(table), options);
    try {
        storage::write_atomic(path(), bytes);
        storage::write_atomic(seal_path(), storage::sha256_hex(bytes) + "\n");
    } catch (const std::exception& error) {
        return QStringLiteral("スクリプトイベントの設定を書けませんでした（%1）").arg(wording::error(std::string(error.what()))).toStdString();
    }
    return std::nullopt;
}

std::vector<Binding> bound_to(const Table& table, Event event) {
    std::vector<Binding> out;
    if (!table.enabled) return out;
    for (const Binding& b : table.bindings) {
        const auto e = event_of(b.event);
        if (e && (*e == event || *e == Event::Everything)) out.push_back(b);
    }
    return out;
}

Steps steps_of(const Binding& binding, Event event) {
    Steps out;
    out.label = (binding.name.empty() ? binding.script.value_or(binding.action.value_or(std::string())) : binding.name) + "（" + label_of(event) + "）";
    if (binding.script) {
        const fs::path file = core::path_from_utf8(*binding.script);
        const std::string bytes = storage::read_file_bounded(file, automate::kMaxStepsBytes);
        if (!binding.sha256 || storage::sha256_hex(bytes) != *binding.sha256) {
            throw core::Error("script_changed", "the script was changed after it was bound: " + *binding.script);
        }
        out.steps = automate::read_steps(bytes);
        return out;
    }
    const Json all = actions::load();
    const auto found = all.find(binding.action.value_or(std::string()));
    if (found == all.end()) throw core::Error("not_found", "no action " + binding.action.value_or(std::string()));
    out.steps = (*found)["ops"];
    out.recorded = true;
    return out;
}

Firing::Firing() : first_(g_firing == 0) { ++g_firing; }

Firing::~Firing() { --g_firing; }

bool Firing::active() { return g_firing > 0; }

}  // namespace genko::service::script_events
