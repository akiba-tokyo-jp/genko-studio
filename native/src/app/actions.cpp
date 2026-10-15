// オートアクション (Python's genko/app/actions.py).

#include "app/actions.hpp"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <map>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/history_panel.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::actions {

using core::Json;

namespace {

// ops that make something with an "id" of its own
const std::vector<std::string> kMakers{"add_layer", "add_tone", "add_line", "add_effect", "add_prim3d", "add_scene", "add_mannequin", "add_ruler",
                                       "stamp_material", "duplicate_layer"};
// ops whose "id" names the layer they change: played back, the layer being drawn on
const std::vector<std::string> kLayerOps{"set_layer", "set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer", "filter_raster"};
// ops that pick an existing line by its place in a layer: only right on the page they were recorded on
const std::vector<std::string> kBoundOps{"delete_stroke", "reshape_stroke", "edit_stroke", "set_stroke_width", "simplify_stroke", "edit_line", "move_line",
                                         "delete_line", "set_balloon_path", "reorder_lines"};
const std::vector<std::string> kNotRecorded{"undo", "lock_page", "unlock_page", "name_ok", "advance", "set_meta", "set_bible", "add_page",
                                            "delete_page", "duplicate_page", "reorder", "set_page_spec"};

bool in(const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); }

std::string op_name(const Json& op) {
    const auto it = op.find("op");
    return it != op.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// op.get(key) as a truthy string: its text (Python's str(op["id"]) when op.get("id") is truthy)
std::optional<std::string> text_of(const Json& op, const char* key) {
    const auto it = op.find(key);
    if (it == op.end() || !core::py_truthy(*it)) return std::nullopt;
    return it->is_string() ? it->get<std::string>() : core::py_str(*it);
}

std::vector<std::string> made_ids(const Json& ops) {
    std::vector<std::string> out;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        if (in(kMakers, name) && name != "duplicate_layer") {
            if (const auto id = text_of(op, "id")) out.push_back(*id);
        }
        if (name == "duplicate_layer") {
            if (const auto id = text_of(op, "new_id")) out.push_back(*id);
        }
    }
    return out;
}

}  // namespace

std::filesystem::path path() { return config_dir() / "actions.json"; }

Json load() {
    Json data;
    try {
        std::ifstream in(path(), std::ios::binary);
        if (!in) return Json::object();
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        data = Json::parse(text);
    } catch (const std::exception&) {
        return Json::object();
    }
    Json out = Json::object();
    if (!data.is_object()) return out;
    for (const auto& [name, value] : data.items()) {
        if (value.is_object() && value.contains("ops") && value["ops"].is_array()) out[name] = value;
    }
    return out;
}

void save(const Json& all) {
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    storage::write_atomic(path(), core::dump(all, options));
}

void store(const QString& name, const Json& ops) {
    Json all = load();
    all[name.toStdString()] = Json{{"ops", ops}, {"made", QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")).toStdString()}};
    save(all);
}

void remove(const QString& name) {
    Json all = load();
    all.erase(name.toStdString());
    save(all);
}

Json recordable(const Json& ops) {
    Json out = Json::array();
    for (const Json& op : ops) {
        if (op.is_object() && !in(kNotRecorded, op_name(op))) out.push_back(op);
    }
    return out;
}

std::vector<bool> page_bound(const Json& ops) {
    const std::vector<std::string> made = made_ids(ops);
    std::vector<bool> out;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        bool bound = false;
        if (const auto id = text_of(op, "id"); id && !in(kMakers, name) && !in(kLayerOps, name) && !in(made, *id)) bound = true;
        for (const char* key : {"ids", "stroke_id", "stroke_ids", "line_id"}) {
            if (text_of(op, key)) bound = true;
        }
        if (in(kBoundOps, name)) bound = true;
        out.push_back(bound);
    }
    return out;
}

Json playable(const Json& ops) {
    const std::vector<bool> bound = page_bound(ops);
    Json out = Json::array();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (!bound[i]) out.push_back(ops[i]);
    }
    return out;
}

Json replay(const Json& recorded, std::optional<Json> page, const std::optional<std::string>& layer_id, const std::optional<std::string>& frame_id,
            const std::function<std::string()>& new_id) {
    const Json ops = playable(recorded);
    std::map<std::string, std::string> made;
    for (const Json& op : ops) {
        const std::string name = op_name(op);
        std::optional<std::string> key;
        if (in(kMakers, name) && name != "duplicate_layer") key = text_of(op, "id");
        if (name == "duplicate_layer") key = text_of(op, "new_id");
        if (key && !made.contains(*key)) made[*key] = new_id ? new_id() : core::new_id();
    }
    Json out = Json::array();
    for (Json op : ops) {
        if (op.contains("page") && page) op["page"] = *page;
        for (const char* key : {"id", "new_id", "after", "parent", "parent_id", "clip_to"}) {
            const auto it = op.find(key);
            if (it != op.end() && it->is_string() && made.contains(it->get<std::string>())) {
                *it = made[it->get<std::string>()];
            } else if (std::string_view(key) == "id" && in(kLayerOps, op_name(op)) && text_of(op, "id") && layer_id) {
                op["id"] = *layer_id;  // (the layer it changed then: the one drawn on now)
            }
        }
        if (op.contains("layer_id")) {
            const std::string recorded_layer = op["layer_id"].is_string() ? op["layer_id"].get<std::string>() : core::py_str(op["layer_id"]);
            op["layer_id"] = made.contains(recorded_layer) ? made[recorded_layer] : layer_id ? *layer_id : recorded_layer;
        }
        if (op.contains("frame_id")) {
            if (frame_id) {
                op["frame_id"] = *frame_id;
            } else {
                op.erase("frame_id");
            }
        }
        out.push_back(std::move(op));
    }
    return out;
}

QString describe(const Json& ops) { return history::describe(ops); }

QString step_label(const Json& op) { return history::describe(Json::array({op})); }

// --- the dialog --------------------------------------------------------------------------------------------------------

ActionsDialog::ActionsDialog(MainWindow* window) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("オートアクションの管理"));
    resize(560, 420);
    names = new QListWidget;
    steps = new QListWidget;
    auto* note = new QLabel(QStringLiteral("灰色の手順は、記録したページの決まった物（台詞・線など）を変えるもので、ほかのページでは飛ばします。"));
    note->setWordWrap(true);
    theme::hint(note);
    data_ = load();
    connect(names, &QListWidget::currentRowChanged, this, [this](int) { fill_steps(); });
    auto* buttons = new QHBoxLayout;
    const std::vector<std::pair<QString, std::function<void()>>> list{{QStringLiteral("↑"), [this] { move_step(-1); }},
                                                                      {QStringLiteral("↓"), [this] { move_step(1); }},
                                                                      {QStringLiteral("手順を外す"), [this] { drop_step(); }},
                                                                      {QStringLiteral("名前を変える"), [this] { rename(); }},
                                                                      {QStringLiteral("消す"), [this] { remove_current(); }},
                                                                      {QStringLiteral("このページで実行"), [this] { play(); }}};
    for (const auto& [label, slot] : list) {
        auto* button = new QPushButton(label);
        connect(button, &QPushButton::clicked, this, slot);
        buttons->addWidget(button);
    }
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* row = new QHBoxLayout;
    row->addWidget(names, 1);
    row->addWidget(steps, 2);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(row, 1);
    layout->addWidget(note);
    layout->addLayout(buttons);
    layout->addWidget(close);
    fill_names();
}

std::optional<QString> ActionsDialog::current() const {
    QListWidgetItem* item = names->currentItem();
    return item != nullptr ? std::optional<QString>(item->text()) : std::nullopt;
}

void ActionsDialog::fill_names(const QString& select) {
    QSignalBlocker quiet(names);
    names->clear();
    for (const auto& [name, value] : data_.items()) names->addItem(QString::fromStdString(name));
    if (names->count() > 0) {
        const auto found = select.isEmpty() ? QList<QListWidgetItem*>() : names->findItems(select, Qt::MatchExactly);
        names->setCurrentItem(found.isEmpty() ? names->item(0) : found.front());
    }
    fill_steps();
}

void ActionsDialog::fill_steps() {
    steps->clear();
    const auto name = current();
    if (!name) return;
    const Json& ops = data_[name->toStdString()]["ops"];
    const std::vector<bool> bound = page_bound(ops);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        auto* item = new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(step_label(ops[i])) +
                                         (bound[i] ? QStringLiteral("（このページだけ）") : QString()));
        if (bound[i]) item->setForeground(Qt::gray);
        steps->addItem(item);
    }
}

void ActionsDialog::keep() {
    save(data_);
    fill_steps();
}

void ActionsDialog::move_step(int delta) {
    const auto name = current();
    const int row = steps->currentRow();
    if (!name) return;
    Json& ops = data_[name->toStdString()]["ops"];
    const int count = static_cast<int>(ops.size());
    if (!(0 <= row && row < count) || !(0 <= row + delta && row + delta < count)) return;
    std::swap(ops[static_cast<std::size_t>(row)], ops[static_cast<std::size_t>(row + delta)]);
    keep();
    steps->setCurrentRow(row + delta);
}

void ActionsDialog::drop_step() {
    const auto name = current();
    const int row = steps->currentRow();
    if (!name) return;
    Json& ops = data_[name->toStdString()]["ops"];
    if (0 <= row && row < static_cast<int>(ops.size())) {
        ops.erase(ops.begin() + row);
        keep();
    }
}

void ActionsDialog::rename() {
    const auto name = current();
    if (!name) return;
    const auto given = ask::get_text(this, QStringLiteral("名前を変える"), QStringLiteral("新しい名前"), *name);
    if (!given || given->trimmed().isEmpty() || given->trimmed() == *name) return;
    const QString renamed = given->trimmed();
    Json again = Json::object();
    for (const auto& [key, value] : data_.items()) again[key == name->toStdString() ? renamed.toStdString() : key] = value;
    data_ = again;
    save(data_);
    fill_names(renamed);
}

void ActionsDialog::remove_current() {
    const auto name = current();
    if (name && ask::question(this, QStringLiteral("オートアクション"), QStringLiteral("「%1」を消しますか？").arg(*name))) {
        data_.erase(name->toStdString());
        save(data_);
        fill_names();
    }
}

void ActionsDialog::play() {
    if (const auto name = current()) window_->play_action(*name);
}

}  // namespace genko::app::actions
