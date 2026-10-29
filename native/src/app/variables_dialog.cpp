#include "app/variables_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/look.hpp"
#include "app/off_ui.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/type_text.hpp"
#include "formats/copy.hpp"
#include "render/page.hpp"
#include "render/ops_registry.hpp"
#include "render/smart_decode.hpp"

namespace genko::app {

using core::Json;

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

// The page's number of a page id (none: gone).
std::optional<Json> page_number(const core::Document& doc, const std::optional<std::string>& id) {
    if (!id) return std::nullopt;
    for (const auto& page : doc.pages) {
        if (page->id == *id) return page->index.json();
    }
    return std::nullopt;
}

QString short_text(const std::string& text) {
    QString t = qs(text).simplified();
    return t.size() > 24 ? t.left(24) + QStringLiteral("…") : t;
}

QDialogButtonBox* ok_cancel(QDialog* dialog, const QString& ok) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(ok);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

}  // namespace

QString variable_type_name(const std::string& type) {
    if (type == "visibility") return QStringLiteral("表示");
    if (type == "text") return QStringLiteral("テキストの置き換え");
    return QStringLiteral("ピクセルの置き換え");
}

QString variable_method_name(const std::string& method) {
    if (method == "fill") return QStringLiteral("塗りつぶす");
    if (method == "as_is") return QStringLiteral("そのまま");
    if (method == "conform") return QStringLiteral("サイズに合わせる");
    return QStringLiteral("合わせる");
}

QString variable_align_name(const std::string& align) {
    static const std::pair<std::string_view, const char*> kNames[] = {
        {"top_left", "左上"}, {"top_center", "上"}, {"top_right", "右上"}, {"center_left", "左"}, {"center", "中央"},
        {"center_right", "右"}, {"bottom_left", "左下"}, {"bottom_center", "下"}, {"bottom_right", "右下"}};
    for (const auto& [key, name] : kNames) {
        if (key == align) return QString::fromUtf8(name);
    }
    return QStringLiteral("中央");
}

// --- 変数の定義… ---------------------------------------------------------------------------------------------------------

VariablesDialog::VariablesDialog(QWidget* parent, const core::Document& doc, const core::Page& page) : QDialog(parent), doc_(doc), page_(page) {
    setObjectName(QStringLiteral("variablesDialog"));
    setWindowTitle(QStringLiteral("変数"));
    for (const core::VariableDef& def : core::variables_of(doc).defs) {
        Json item{{"name", def.name}, {"type", def.type}};
        if (def.line) {
            item["line"] = *def.line;
        } else {
            item["page"] = page_number(doc, def.page).value_or(Json());
            item["layer"] = *def.layer;
        }
        if (def.type == "pixels") {
            item["method"] = def.method;
            item["align"] = def.align;
            item["clip"] = def.clip;
            item["box_mm"] = Json::array({def.box_mm[0], def.box_mm[1], def.box_mm[2], def.box_mm[3]});
        }
        defs_.push_back(std::move(item));
    }
    table = new QTableWidget(0, 6);
    table->setObjectName(QStringLiteral("variables_table"));
    table->setHorizontalHeaderLabels({QStringLiteral("名前"), QStringLiteral("種類"), QStringLiteral("対象"), QStringLiteral("方法"),
                                      QStringLiteral("配置"), QStringLiteral("クリップ")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    target = new QComboBox;
    target->setObjectName(QStringLiteral("variable_target"));
    for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {  // (the top first, as the layers panel)
        const core::Layer& layer = *it;
        if (layer.kind == core::LayerKind::Folder && !layer.group) continue;
        target->addItem(QStringLiteral("レイヤー: %1").arg(qs(layer.title.empty() ? layer.id : layer.title)),
                        QString::fromStdString(Json{{"layer", layer.id}}.dump()));
    }
    for (const core::StoryLine* line : doc.story_for_page(page.index)) {
        target->addItem(QStringLiteral("台詞: %1").arg(short_text(line->text)), QString::fromStdString(Json{{"line", line->id}}.dump()));
    }
    type = new QComboBox;
    type->setObjectName(QStringLiteral("variable_type"));
    for (const char* t : {"visibility", "text", "pixels"}) type->addItem(variable_type_name(t), QString::fromLatin1(t));
    name = new QLineEdit;
    name->setObjectName(QStringLiteral("variable_name"));
    name->setPlaceholderText(QStringLiteral("（空: 自動で名前を付けます）"));
    method = new QComboBox;
    method->setObjectName(QStringLiteral("variable_method"));
    for (const std::string_view m : core::kVariableMethods) method->addItem(variable_method_name(std::string(m)), QString::fromLatin1(m.data(), static_cast<qsizetype>(m.size())));
    align = new QComboBox;
    align->setObjectName(QStringLiteral("variable_align"));
    for (const std::string_view a : core::kVariableAligns) align->addItem(variable_align_name(std::string(a)), QString::fromLatin1(a.data(), static_cast<qsizetype>(a.size())));
    align->setCurrentIndex(std::max(0, align->findData(QStringLiteral("center"))));
    clip = new QCheckBox(QStringLiteral("境界ボックスでクリップ"));
    clip->setObjectName(QStringLiteral("variable_clip"));
    const auto pixels_shown = [this] {
        const bool pixels = type->currentData().toString() == QLatin1String("pixels");
        method->setEnabled(pixels);
        align->setEnabled(pixels);
        clip->setEnabled(pixels);
    };
    connect(type, &QComboBox::currentIndexChanged, this, pixels_shown);
    pixels_shown();
    add = new QPushButton(QStringLiteral("追加"));
    add->setObjectName(QStringLiteral("variable_add"));
    remove = new QPushButton(QStringLiteral("削除"));
    remove->setObjectName(QStringLiteral("variable_remove"));
    connect(add, &QPushButton::clicked, this, [this] { add_chosen(); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = table->currentRow();
        if (row < 0 || row >= static_cast<int>(defs_.size())) return;
        defs_.erase(defs_.begin() + row);
        fill_table();
    });
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("対象"), target);
    rows->addRow(QStringLiteral("種類"), type);
    rows->addRow(QStringLiteral("名前"), name);
    rows->addRow(QStringLiteral("方法"), method);
    rows->addRow(QStringLiteral("配置"), align);
    rows->addRow(QString(), clip);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(add);
    row->addWidget(remove);
    buttons = ok_cancel(this, QStringLiteral("OK"));
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(QStringLiteral("このページのレイヤー・台詞に変数を定義します。表示はどのレイヤーにも、テキストの置き換えは文字レイヤーと台詞に、"
                                           "ピクセルの置き換えはペイントのレイヤーとスマートオブジェクトに使えます。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addWidget(table, 1);
    layout->addLayout(rows);
    layout->addLayout(row);
    layout->addWidget(buttons);
    resize(620, 520);
    fill_table();
}

void VariablesDialog::fill_table() {
    table->setRowCount(static_cast<int>(defs_.size()));
    for (int r = 0; r < static_cast<int>(defs_.size()); ++r) {
        const Json& def = defs_[std::size_t(r)];
        const std::string t = def["type"].get<std::string>();
        QString what;
        bool gone = false;
        if (def.contains("line")) {
            const auto line = std::find_if(doc_.story.begin(), doc_.story.end(), [&](const core::StoryLine& l) { return l.id == def["line"].get<std::string>(); });
            gone = line == doc_.story.end();
            what = gone ? QString() : QStringLiteral("台詞: %1").arg(short_text(line->text));
        } else {
            const core::Layer* layer = nullptr;
            if (!def["page"].is_null()) {
                for (const auto& page : doc_.pages) {
                    if (page->index.json() != def["page"]) continue;
                    for (const core::Layer& l : page->layers) {
                        if (l.id == def["layer"].get<std::string>()) layer = &l;
                    }
                }
            }
            gone = layer == nullptr;
            what = gone ? QString() : QStringLiteral("%1 ページ: %2").arg(qs(core::py_str(def["page"])), qs(layer->title.empty() ? layer->id : layer->title));
        }
        if (gone) what = QStringLiteral("（見つかりません: OK で外します）");
        const QStringList cells{qs(def["name"].get<std::string>()), variable_type_name(t), what,
                                t == "pixels" ? variable_method_name(def["method"].get<std::string>()) : QString(),
                                t == "pixels" ? variable_align_name(def["align"].get<std::string>()) : QString(),
                                t == "pixels" ? (def["clip"].get<bool>() ? QStringLiteral("する") : QStringLiteral("しない")) : QString()};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (gone) item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
            table->setItem(r, c, item);
        }
    }
}

bool VariablesDialog::add_chosen() {
    if (target->currentIndex() < 0) return false;
    const Json where = Json::parse(target->currentData().toString().toStdString());
    const std::string t = type->currentData().toString().toStdString();
    if (where.contains("line") && t != "text") {
        ask::warning(this, QStringLiteral("変数"), QStringLiteral("台詞に使えるのはテキストの置き換えだけです"));
        return false;
    }
    if (where.contains("layer")) {
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : page_.layers) {  // (a layer of the window's page: ids are a page's own)
            if (l.id == where["layer"].get<std::string>()) layer = &l;
        }
        const bool fits = layer != nullptr && (t == "visibility" || (t == "text" && core::type_text::is_type_layer(*layer)) ||
                                               (t == "pixels" && (layer->kind == core::LayerKind::Raster || layer->kind == core::LayerKind::Smart)));
        if (!fits) {
            ask::warning(this, QStringLiteral("変数"),
                         t == "text" ? QStringLiteral("テキストの置き換えにできるのは文字レイヤーか台詞だけです")
                                     : QStringLiteral("ピクセルの置き換えにできるのはペイントのレイヤーかスマートオブジェクトだけです"));
            return false;
        }
    }
    std::string chosen = name->text().trimmed().toStdString();
    if (chosen.empty()) {
        for (int n = 1;; ++n) {
            const std::string candidate = "変数 " + std::to_string(n);
            if (std::none_of(defs_.begin(), defs_.end(), [&](const Json& d) { return d["name"] == candidate; })) {
                chosen = candidate;
                break;
            }
        }
    }
    Json def{{"name", chosen}, {"type", t}};
    if (where.contains("line")) {
        def["line"] = where["line"];
    } else {
        def["page"] = page_.index.json();
        def["layer"] = where["layer"];
    }
    if (t == "pixels") {
        def["method"] = method->currentData().toString().toStdString();
        def["align"] = align->currentData().toString().toStdString();
        def["clip"] = clip->isChecked();
    }
    const auto same = std::find_if(defs_.begin(), defs_.end(), [&](const Json& d) { return d["name"] == chosen; });
    if (same != defs_.end()) {
        if (t == "pixels" && same->contains("box_mm") && same->value("page", Json()) == def.value("page", Json()) &&
            same->value("layer", Json()) == def.value("layer", Json()))
            def["box_mm"] = (*same)["box_mm"];
        *same = def;
    } else {
        defs_.push_back(def);
    }
    name->clear();
    fill_table();
    return true;
}

Json VariablesDialog::defs() const {
    Json out = Json::array();
    for (int r = 0; r < static_cast<int>(defs_.size()); ++r) {
        const QTableWidgetItem* where = table->item(r, 2);
        if (where != nullptr && where->text().startsWith(QStringLiteral("（見つかりません"))) continue;  // (its layer or line gone)
        out.push_back(defs_[std::size_t(r)]);
    }
    return out;
}

// --- データセット… -------------------------------------------------------------------------------------------------------

DataSetsDialog::DataSetsDialog(QWidget* parent, const core::Document& doc) : QDialog(parent), variables_(core::variables_of(doc)) {
    setObjectName(QStringLiteral("dataSetsDialog"));
    setWindowTitle(QStringLiteral("データセット"));
    for (const core::DataSet& set : variables_.data_sets) {
        Set s;
        s.name = set.name;
        for (const core::DataValue& value : set.values) {
            if (const bool* shown = std::get_if<bool>(&value.value)) {
                s.values.emplace_back(value.variable, Json(*shown));
            } else if (const std::string* words = std::get_if<std::string>(&value.value)) {
                s.values.emplace_back(value.variable, Json(*words));
            } else {
                const core::PictureValue& picture = std::get<core::PictureValue>(value.value);
                Json p{{"asset", picture.asset}};
                if (!picture.name.empty()) p["name"] = picture.name;
                s.values.emplace_back(value.variable, std::move(p));
            }
        }
        sets_.push_back(std::move(s));
    }
    sets = new QComboBox;
    sets->setObjectName(QStringLiteral("data_sets_list"));
    name = new QLineEdit;
    name->setObjectName(QStringLiteral("data_set_name"));
    previous = new QPushButton(QStringLiteral("◀"));
    previous->setToolTip(QStringLiteral("前のデータセット"));
    next = new QPushButton(QStringLiteral("▶"));
    next->setToolTip(QStringLiteral("次のデータセット"));
    add = new QPushButton(QStringLiteral("新規"));
    add->setObjectName(QStringLiteral("data_set_add"));
    remove = new QPushButton(QStringLiteral("削除"));
    remove->setObjectName(QStringLiteral("data_set_remove"));
    values = new QTableWidget(static_cast<int>(variables_.defs.size()), 3);
    values->setObjectName(QStringLiteral("data_set_values"));
    values->setHorizontalHeaderLabels({QStringLiteral("変数"), QStringLiteral("種類"), QStringLiteral("値")});
    values->horizontalHeader()->setStretchLastSection(true);
    values->verticalHeader()->hide();
    values->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int r = 0; r < static_cast<int>(variables_.defs.size()); ++r) {
        const core::VariableDef& def = variables_.defs[std::size_t(r)];
        values->setItem(r, 0, new QTableWidgetItem(qs(def.name)));
        values->setItem(r, 1, new QTableWidgetItem(variable_type_name(def.type)));
        const QString variable = qs(def.name);
        if (def.type == "visibility") {
            auto* box = new QComboBox;
            box->addItem(QStringLiteral("（変えない）"));
            box->addItem(QStringLiteral("表示"), true);
            box->addItem(QStringLiteral("非表示"), false);
            values->setCellWidget(r, 2, box);
        } else if (def.type == "text") {
            auto* edit = new QLineEdit;
            edit->setPlaceholderText(QStringLiteral("（空: 変えない）"));
            values->setCellWidget(r, 2, edit);
        } else {
            auto* pick = new QPushButton(QStringLiteral("（変えない）"));
            pick->setToolTip(QStringLiteral("置き換える画像のファイルを選びます（原稿の素材として取り込みます）"));
            connect(pick, &QPushButton::clicked, this, [this, variable] {
                const QString file = ask::open_path(this, QStringLiteral("置き換える画像"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.tif *.tiff *.psd *.psb *.webp *.bmp *.gif);;すべて (*)"));
                if (!file.isEmpty()) set_picture(variable, file);
            });
            values->setCellWidget(r, 2, pick);
        }
    }
    connect(sets, &QComboBox::currentIndexChanged, this, [this](int index) {
        keep_shown();
        show_set(index);
    });
    connect(name, &QLineEdit::editingFinished, this, [this] {
        if (shown_ < 0) return;
        const QString text = name->text().trimmed();
        if (text.isEmpty()) return;
        sets_[std::size_t(shown_)].name = text.toStdString();
        sets->setItemText(shown_, text);
    });
    connect(previous, &QPushButton::clicked, this, [this] {
        if (sets->currentIndex() > 0) sets->setCurrentIndex(sets->currentIndex() - 1);
    });
    connect(next, &QPushButton::clicked, this, [this] {
        if (sets->currentIndex() + 1 < sets->count()) sets->setCurrentIndex(sets->currentIndex() + 1);
    });
    connect(add, &QPushButton::clicked, this, [this] { add_set(); });
    connect(remove, &QPushButton::clicked, this, [this] { remove_set(); });
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("データセット")));
    top->addWidget(sets, 1);
    top->addWidget(previous);
    top->addWidget(next);
    top->addWidget(add);
    top->addWidget(remove);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("名前"), name);
    buttons = ok_cancel(this, QStringLiteral("OK"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { keep_shown(); });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addLayout(rows);
    layout->addWidget(values, 1);
    layout->addWidget(buttons);
    resize(560, 460);
    {
        const QSignalBlocker quiet(sets);
        for (const Set& s : sets_) sets->addItem(qs(s.name));
    }
    show_set(sets_.empty() ? -1 : 0);
}

void DataSetsDialog::show_set(int index) {
    shown_ = index;
    const bool any = index >= 0 && index < static_cast<int>(sets_.size());
    name->setEnabled(any);
    values->setEnabled(any);
    name->setText(any ? qs(sets_[std::size_t(index)].name) : QString());
    for (int r = 0; r < values->rowCount(); ++r) {
        const core::VariableDef& def = variables_.defs[std::size_t(r)];
        const Json* value = nullptr;
        if (any) {
            for (const auto& [variable, v] : sets_[std::size_t(index)].values) {
                if (variable == def.name) value = &v;
            }
        }
        QWidget* cell = values->cellWidget(r, 2);
        if (auto* box = qobject_cast<QComboBox*>(cell)) {
            box->setCurrentIndex(value == nullptr || !value->is_boolean() ? 0 : value->get<bool>() ? 1 : 2);
        } else if (auto* edit = qobject_cast<QLineEdit*>(cell)) {
            edit->setText(value != nullptr && value->is_string() ? qs(value->get<std::string>()) : QString());
        } else if (auto* pick = qobject_cast<QPushButton*>(cell)) {
            QString text = QStringLiteral("（変えない）");
            if (value != nullptr && value->is_object()) text = qs(value->value("name", std::string("画像")));
            pick->setText(text);
        }
    }
}

void DataSetsDialog::keep_shown() {
    if (shown_ < 0 || shown_ >= static_cast<int>(sets_.size())) return;
    Set& set = sets_[std::size_t(shown_)];
    const QString text = name->text().trimmed();
    if (!text.isEmpty()) set.name = text.toStdString();
    std::vector<std::pair<std::string, Json>> kept;
    for (int r = 0; r < values->rowCount(); ++r) {
        const core::VariableDef& def = variables_.defs[std::size_t(r)];
        QWidget* cell = values->cellWidget(r, 2);
        if (auto* box = qobject_cast<QComboBox*>(cell)) {
            if (box->currentIndex() > 0) kept.emplace_back(def.name, Json(box->currentIndex() == 1));
        } else if (auto* edit = qobject_cast<QLineEdit*>(cell)) {
            if (!edit->text().isEmpty()) kept.emplace_back(def.name, Json(edit->text().toStdString()));
        } else {
            for (const auto& [variable, v] : set.values) {
                if (variable == def.name && v.is_object()) kept.emplace_back(def.name, v);
            }
        }
    }
    set.values = std::move(kept);
}

bool DataSetsDialog::set_picture(const QString& variable, const QString& file) {
    if (shown_ < 0) return false;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly) || in.size() <= 0 || static_cast<std::uint64_t>(in.size()) > render::smart::kFileMaxBytes) {
        ask::warning(this, QStringLiteral("データセット"), QStringLiteral("画像を読めません（256 MiB までの画像のファイルを選びます）"));
        return false;
    }
    const QByteArray bytes = in.readAll();
    Set& set = sets_[std::size_t(shown_)];
    std::erase_if(set.values, [&](const auto& v) { return v.first == variable.toStdString(); });
    set.values.emplace_back(variable.toStdString(), Json{{"data", core::b64encode(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))},
                                                         {"name", QFileInfo(file).fileName().toStdString()}});
    keep_shown();
    show_set(shown_);
    return true;
}

void DataSetsDialog::add_set() {
    keep_shown();
    Set s;
    for (int n = static_cast<int>(sets_.size()) + 1;; ++n) {
        const std::string candidate = "データセット " + std::to_string(n);
        if (std::none_of(sets_.begin(), sets_.end(), [&](const Set& o) { return o.name == candidate; })) {
            s.name = candidate;
            break;
        }
    }
    sets_.push_back(std::move(s));
    {
        const QSignalBlocker quiet(sets);
        sets->addItem(qs(sets_.back().name));
        sets->setCurrentIndex(sets->count() - 1);
    }
    show_set(sets->count() - 1);
}

void DataSetsDialog::remove_set() {
    if (shown_ < 0 || shown_ >= static_cast<int>(sets_.size())) return;
    sets_.erase(sets_.begin() + shown_);
    const int at = std::min(shown_, static_cast<int>(sets_.size()) - 1);
    {
        const QSignalBlocker quiet(sets);
        sets->removeItem(shown_);
        sets->setCurrentIndex(at);
    }
    show_set(at);
}

Json DataSetsDialog::data_sets() const {
    Json out = Json::array();
    for (const Set& s : sets_) {
        Json values = Json::array();
        for (const auto& [variable, value] : s.values) values.push_back(Json{{"variable", variable}, {"value", value}});
        out.push_back(Json{{"name", s.name}, {"values", std::move(values)}});
    }
    return out;
}

// --- データセットを適用… --------------------------------------------------------------------------------------------------

ApplyDataSetDialog::ApplyDataSetDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page, const std::string& actor)
    : QDialog(parent), doc_(std::move(doc)), page_(page), actor_(actor) {
    setObjectName(QStringLiteral("applyDataSetDialog"));
    setWindowTitle(QStringLiteral("データセットを適用"));
    list = new QListWidget;
    list->setObjectName(QStringLiteral("apply_data_set_list"));
    const core::Variables variables = core::variables_of(*doc_);
    for (const core::DataSet& set : variables.data_sets) list->addItem(qs(set.name));
    preview = new QLabel;
    preview->setObjectName(QStringLiteral("apply_data_set_preview"));
    preview->setMinimumSize(240, 300);
    preview->setAlignment(Qt::AlignCenter);
    buttons = ok_cancel(this, QStringLiteral("適用"));
    auto* body = new QHBoxLayout;
    body->addWidget(list, 1);
    body->addWidget(preview, 1);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);
    connect(list, &QListWidget::currentRowChanged, this, [this] { draw(); });
    list->setCurrentRow(variables.active ? static_cast<int>(*variables.active) : (list->count() > 0 ? 0 : -1));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(list->count() > 0);
}

int ApplyDataSetDialog::chosen() const { return list->currentRow(); }

void ApplyDataSetDialog::draw() {
    const int index = chosen();
    if (index < 0) {
        preview->clear();
        return;
    }
    const auto doc = doc_;
    const std::size_t at = page_;
    const std::string who = actor_;
    QImage picture;
    try {
        picture = off_ui([doc, at, who, index] {
            core::Document free = *doc;  // (a picture of it only: nothing kept)
            free.page_locks = Json::object();
            free.strict_gates = false;
            const core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(free, Json::array({Json{{"op", "apply_data_set"}, {"index", index}}}), core::Actor(who));
            render::RenderOptions options;
            options.skip_unported = true;
            return to_qimage(render::render_page(result.doc.page(at), 30, options, &result.doc).image);
        });
    } catch (const std::exception& error) {
        preview->setText(QStringLiteral("下見を描けません"));
        preview->setToolTip(QString::fromUtf8(error.what()));
        return;
    }
    preview->setToolTip(QString());
    preview->setPixmap(QPixmap::fromImage(picture.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

// --- データセットからファイル… --------------------------------------------------------------------------------------------

DataSetsToFilesDialog::DataSetsToFilesDialog(QWidget* parent, const QString& where, const core::Document& doc) : QDialog(parent) {
    setObjectName(QStringLiteral("dataSetsToFilesDialog"));
    setWindowTitle(QStringLiteral("データセットからファイル"));
    folder = new QLineEdit(QDir::toNativeSeparators(where));
    folder->setObjectName(QStringLiteral("data_sets_folder"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString got = ask::existing_dir(this, QStringLiteral("書き出し先"), folder->text());
        if (!got.isEmpty()) folder->setText(QDir::toNativeSeparators(got));
    });
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(folder, 1);
    line->addWidget(pick);
    auto* where_row = new QWidget;
    where_row->setLayout(line);
    format = new QComboBox;
    format->setObjectName(QStringLiteral("data_sets_format"));
    for (const std::string_view ext : formats::copy_extensions()) {
        const QString e = QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size()));
        format->addItem(e.toUpper(), e);
    }
    format->setCurrentIndex(std::max(0, format->findData(QStringLiteral("png"))));
    quality = new QSpinBox;
    quality->setObjectName(QStringLiteral("data_sets_quality"));
    quality->setRange(0, 12);
    quality->setValue(10);
    quality->setToolTip(QStringLiteral("JPEG の画質（0〜12。12 が最高）"));
    const auto jpeg = [this] {
        const QString f = format->currentData().toString();
        return f == QLatin1String("jpg") || f == QLatin1String("jpeg");
    };
    connect(format, &QComboBox::currentIndexChanged, this, [this, jpeg] { quality->setEnabled(jpeg()); });
    quality->setEnabled(jpeg());
    naming = new QLineEdit(QStringLiteral("{name}"));
    naming->setObjectName(QStringLiteral("data_sets_naming"));
    naming->setToolTip(QStringLiteral("{name}: データセットの名前、{index}: 番号（001 から）、{document}: 原稿の名前"));
    which = new QComboBox;
    which->setObjectName(QStringLiteral("data_sets_which"));
    which->addItem(QStringLiteral("すべてのデータセット"));
    for (const core::DataSet& set : core::variables_of(doc).data_sets) which->addItem(qs(set.name), qs(set.name));
    buttons = ok_cancel(this, QStringLiteral("書き出す"));
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("書き出し先"), where_row);
    rows->addRow(QStringLiteral("データセット"), which);
    rows->addRow(QStringLiteral("ファイル名"), naming);
    rows->addRow(QStringLiteral("ファイル形式"), format);
    rows->addRow(QStringLiteral("画質"), quality);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(rows);
    layout->addWidget(buttons);
}

// --- 変数データセットを読み込み ---------------------------------------------------------------------------------------------

ImportDataSetsDialog::ImportDataSetsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("importDataSetsDialog"));
    setWindowTitle(QStringLiteral("変数データセットを読み込み"));
    file = new QLineEdit;
    file->setObjectName(QStringLiteral("import_data_sets_file"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString got = ask::open_path(this, QStringLiteral("データセットのテキストファイル"), QStringLiteral("テキスト (*.csv *.txt);;すべて (*)"));
        if (!got.isEmpty()) file->setText(QDir::toNativeSeparators(got));
    });
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(file, 1);
    line->addWidget(pick);
    auto* file_row = new QWidget;
    file_row->setLayout(line);
    delimiter = new QComboBox;
    delimiter->setObjectName(QStringLiteral("import_data_sets_delimiter"));
    delimiter->addItem(QStringLiteral("自動"), 0);
    delimiter->addItem(QStringLiteral("コンマ"), static_cast<int>(','));
    delimiter->addItem(QStringLiteral("タブ"), static_cast<int>('\t'));
    replace = new QCheckBox(QStringLiteral("既存のデータセットを置き換える"));
    replace->setObjectName(QStringLiteral("import_data_sets_replace"));
    replace->setChecked(true);
    buttons = ok_cancel(this, QStringLiteral("読み込む"));
    auto* note = new QLabel(QStringLiteral("1 行目に変数の名前、2 行目からの各行に 1 つのデータセット（UTF-8 のテキスト）。最初の列の見出しが空か"
                                           "「データセット」なら、その列をデータセットの名前にします。画像はファイルの場所（テキストファイルからの相対パスでも）。"));
    note->setWordWrap(true);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("ファイル"), file_row);
    rows->addRow(QStringLiteral("区切り"), delimiter);
    rows->addRow(QString(), replace);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addLayout(rows);
    layout->addWidget(buttons);
}

}  // namespace genko::app
