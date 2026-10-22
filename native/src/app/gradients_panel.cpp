#include "app/gradients_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/theme.hpp"
#include "core/config_dir.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "render/gradient_presets.hpp"
#include "render/grd.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace store = render::preset_store;
namespace presets = render::gradient_presets;

namespace {

constexpr int kRole = Qt::UserRole + 1;  // (an item's group index and preset index)

QString text_of(const std::string& s) { return QString::fromStdString(s); }

// 描画色から背景色へ: the first built-in preset
Json first_builtin() {
    const auto groups = presets::kind().builtin();
    return groups.front().items.front();
}

}  // namespace

// --- the library --------------------------------------------------------------------------------------------------------

GradientLibrary::GradientLibrary(QObject* parent) : QObject(parent) {
    reload();
    current_ = first_builtin();
    const QString kept = settings()->value(QStringLiteral("gradient/current")).toString();
    if (!kept.isEmpty()) {
        try {
            const Json item = core::parse_python_json(kept.toStdString());
            current_ = presets::item_of(item, item.value("name", std::string("カスタム")));
        } catch (const std::exception&) {  // (a setting that cannot be read: the first preset)
        }
    }
}

void GradientLibrary::reload() {
    store::Library library = store::load(core::config_dir(), presets::kind());
    groups_ = std::move(library.groups);
    problem_ = library.problem;
    emit changed();
}

bool GradientLibrary::change(const std::vector<store::Group>& groups, QString* error) {
    try {
        store::save(core::config_dir(), presets::kind(), groups);
    } catch (const std::exception& e) {
        if (error != nullptr) *error = wording::error(QString::fromUtf8(e.what()));
        return false;
    }
    groups_ = groups;
    problem_.clear();
    emit changed();
    return true;
}

bool GradientLibrary::update(const std::function<void(std::vector<store::Group>&)>& work, QString* error) {
    try {
        // (review P3-4: the library as the settings folder holds it now — what another window or `genko gradients` wrote
        // since kept —, then changed and written; a file there that cannot be read is not written over: save refuses)
        store::Library now = store::load(core::config_dir(), presets::kind());
        std::vector<store::Group> groups = std::move(now.groups);
        work(groups);
        store::save(core::config_dir(), presets::kind(), groups);
        groups_ = std::move(groups);
    } catch (const std::exception& e) {
        if (error != nullptr) *error = wording::error(QString::fromUtf8(e.what()));
        return false;
    }
    problem_.clear();
    emit changed();
    return true;
}

const Json* GradientLibrary::find(const std::string& name) const { return store::find(groups_, name); }

void GradientLibrary::set_current(const Json& item) {
    current_ = item;
    settings()->setValue(QStringLiteral("gradient/current"), QString::fromStdString(core::dump_canonical(item)));
    emit current_changed();
}

// --- a strip --------------------------------------------------------------------------------------------------------------

QPixmap gradient_strip(const Json& item, QSize size, const std::array<int, 3>& foreground, const std::array<int, 3>& background) {
    QPixmap out(std::max(1, size.width()), std::max(1, size.height()));
    QPainter p(&out);
    for (int y = 0; y < out.height(); y += 5)
        for (int x = 0; x < out.width(); x += 5) p.fillRect(x, y, 5, 5, (x / 5 + y / 5) % 2 ? QColor(205, 205, 205) : QColor(245, 245, 245));
    const auto columns = presets::samples(item, out.width(), foreground, background);  // (its stops read once: review P3-5)
    for (int x = 0; x < out.width(); ++x) {
        const auto& c = columns[static_cast<std::size_t>(x)];
        p.fillRect(x, 0, 1, out.height(), QColor(c[0], c[1], c[2], c[3]));
    }
    p.setPen(QColor(128, 128, 128));
    p.drawRect(0, 0, out.width() - 1, out.height() - 1);
    return out;
}

// --- the editor -----------------------------------------------------------------------------------------------------------

GradientEditor::GradientEditor(const Json& item, const std::array<int, 3>& foreground, const std::array<int, 3>& background, QWidget* parent)
    : QDialog(parent), fg_(foreground), bg_(background) {
    setObjectName(QStringLiteral("gradient_editor"));
    setWindowTitle(QStringLiteral("グラデーションエディター"));
    for (const Json& s : item.value("stops", Json::array())) {
        ColourStop stop;
        stop.at = s[0].get<double>();
        if (s[1].is_string()) {
            stop.kind = text_of(s[1].get<std::string>());
        } else {
            for (std::size_t k = 0; k < 3; ++k) stop.rgb[k] = s[1][k].get<int>();
        }
        colours_.push_back(stop);
    }
    for (const Json& s : item.value("transparency", Json::array())) opacity_.push_back({s[0].get<double>(), s[1].get<double>()});
    if (colours_.size() < 2) colours_ = {{0.0, QStringLiteral("foreground"), {}}, {1.0, QStringLiteral("background"), {}}};
    if (opacity_.size() < 2) opacity_ = {{0.0, 1.0}, {1.0, 1.0}};
    name = new QLineEdit(text_of(item.value("name", std::string("カスタム"))));
    name->setObjectName(QStringLiteral("gradient_name"));
    strip_ = new QLabel;
    strip_->setMinimumHeight(28);
    colour_rows_ = new QGridLayout;
    opacity_rows_ = new QGridLayout;
    add_colour = new QPushButton(QStringLiteral("＋ カラー分岐点"));
    add_colour->setObjectName(QStringLiteral("gradient_add_colour"));
    connect(add_colour, &QPushButton::clicked, this, [this] {
        if (colours_.size() >= 16) return;
        std::sort(colours_.begin(), colours_.end(), [](const ColourStop& a, const ColourStop& b) { return a.at < b.at; });
        const ColourStop a = colours_[colours_.size() - 2], b = colours_.back();
        ColourStop mid{(a.at + b.at) / 2, QStringLiteral("rgb"), {}};
        for (std::size_t c = 0; c < 3; ++c) mid.rgb[c] = (a.rgb[c] + b.rgb[c]) / 2;
        colours_.insert(colours_.end() - 1, mid);
        fill();
    });
    add_opacity = new QPushButton(QStringLiteral("＋ 不透明度の分岐点"));
    add_opacity->setObjectName(QStringLiteral("gradient_add_opacity"));
    connect(add_opacity, &QPushButton::clicked, this, [this] {
        if (opacity_.size() >= 16) return;
        std::sort(opacity_.begin(), opacity_.end(), [](const OpacityStop& a, const OpacityStop& b) { return a.at < b.at; });
        const OpacityStop a = opacity_[opacity_.size() - 2], b = opacity_.back();
        opacity_.insert(opacity_.end() - 1, OpacityStop{(a.at + b.at) / 2, (a.opacity + b.opacity) / 2});
        fill();
    });
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("名前"), name);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(strip_);
    auto* colour_title = new QLabel(QStringLiteral("カラー分岐点（位置・種類・色）"));
    theme::role(colour_title, "section");
    layout->addWidget(colour_title);
    layout->addLayout(colour_rows_);
    layout->addWidget(add_colour);
    auto* opacity_title = new QLabel(QStringLiteral("不透明度の分岐点（位置・不透明度）"));
    theme::role(opacity_title, "section");
    layout->addWidget(opacity_title);
    layout->addLayout(opacity_rows_);
    layout->addWidget(add_opacity);
    auto* note = new QLabel(QStringLiteral("中間点はいつも 50% です。描画色・背景色の分岐点は、使うときの色になります"));
    note->setWordWrap(true);
    theme::hint(note);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    fill();
}

Json GradientEditor::item() const {
    Json stops = Json::array();
    for (const ColourStop& s : colours_) {
        const Json colour = s.kind == QLatin1String("rgb") ? Json::array({s.rgb[0], s.rgb[1], s.rgb[2]}) : Json(s.kind.toStdString());
        stops.push_back(Json::array({s.at, colour}));
    }
    Json params{{"stops", std::move(stops)}};
    const bool opaque = std::all_of(opacity_.begin(), opacity_.end(), [](const OpacityStop& o) { return o.opacity >= 1.0; });
    if (!opaque) {
        Json t = Json::array();
        for (const OpacityStop& o : opacity_) t.push_back(Json::array({o.at, o.opacity}));
        params["transparency"] = std::move(t);
    }
    const QString typed = name->text().trimmed();
    return presets::item_of(params, (typed.isEmpty() ? QStringLiteral("カスタム") : typed).toStdString());
}

void GradientEditor::fill() {
    for (QGridLayout* rows : {colour_rows_, opacity_rows_}) {
        while (QLayoutItem* each = rows->takeAt(0)) {
            if (QWidget* w = each->widget()) w->deleteLater();
            delete each;
        }
    }
    const auto refresh = [this] {
        try {
            strip_->setPixmap(gradient_strip(item(), QSize(std::max(200, strip_->width()), 24), fg_, bg_));
        } catch (const std::exception&) {
        }
    };
    for (std::size_t i = 0; i < colours_.size(); ++i) {
        const int row = static_cast<int>(i);
        auto* where = new QDoubleSpinBox;
        where->setRange(0, 100);
        where->setDecimals(1);
        where->setSuffix(QStringLiteral(" %"));
        where->setValue(std::round(colours_[i].at * 1000) / 10);
        where->setObjectName(QStringLiteral("gradient_colour_at"));
        connect(where, &QDoubleSpinBox::valueChanged, this, [this, i, refresh](double v) {
            colours_[i].at = v / 100;
            refresh();
        });
        auto* kind = new QComboBox;
        kind->setObjectName(QStringLiteral("gradient_colour_kind"));
        kind->addItem(QStringLiteral("色"), QStringLiteral("rgb"));
        kind->addItem(QStringLiteral("描画色"), QStringLiteral("foreground"));
        kind->addItem(QStringLiteral("背景色"), QStringLiteral("background"));
        kind->setCurrentIndex(std::max(0, kind->findData(colours_[i].kind)));
        auto* colour = new QPushButton;
        colour->setFixedWidth(46);
        const auto rgb = colours_[i].kind == QLatin1String("foreground")   ? fg_
                         : colours_[i].kind == QLatin1String("background") ? bg_
                                                                          : colours_[i].rgb;
        colour->setStyleSheet(QStringLiteral("background: rgb(%1,%2,%3)").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
        colour->setEnabled(colours_[i].kind == QLatin1String("rgb"));
        colour->setToolTip(QStringLiteral("色を選ぶ"));
        connect(kind, &QComboBox::activated, this, [this, i, kind](int) {
            colours_[i].kind = kind->currentData().toString();
            fill();
        });
        connect(colour, &QPushButton::clicked, this, [this, i] {
            const auto& now = colours_[i].rgb;
            const auto picked = ask::colour(this, QColor(now[0], now[1], now[2]), QStringLiteral("カラー分岐点の色"));
            if (!picked) return;
            colours_[i].rgb = {picked->red(), picked->green(), picked->blue()};
            fill();
        });
        auto* remove = new QPushButton(QStringLiteral("×"));
        remove->setFixedWidth(28);
        remove->setEnabled(colours_.size() > 2);
        connect(remove, &QPushButton::clicked, this, [this, i] {
            if (colours_.size() <= 2) return;
            colours_.erase(colours_.begin() + static_cast<std::ptrdiff_t>(i));
            fill();
        });
        colour_rows_->addWidget(where, row, 0);
        colour_rows_->addWidget(kind, row, 1);
        colour_rows_->addWidget(colour, row, 2);
        colour_rows_->addWidget(remove, row, 3);
    }
    for (std::size_t i = 0; i < opacity_.size(); ++i) {
        const int row = static_cast<int>(i);
        auto* where = new QDoubleSpinBox;
        where->setRange(0, 100);
        where->setDecimals(1);
        where->setSuffix(QStringLiteral(" %"));
        where->setValue(std::round(opacity_[i].at * 1000) / 10);
        where->setObjectName(QStringLiteral("gradient_opacity_at"));
        connect(where, &QDoubleSpinBox::valueChanged, this, [this, i, refresh](double v) {
            opacity_[i].at = v / 100;
            refresh();
        });
        auto* strong = new QDoubleSpinBox;
        strong->setRange(0, 100);
        strong->setDecimals(1);
        strong->setSuffix(QStringLiteral(" %"));
        strong->setValue(std::round(opacity_[i].opacity * 1000) / 10);
        strong->setObjectName(QStringLiteral("gradient_opacity_value"));
        connect(strong, &QDoubleSpinBox::valueChanged, this, [this, i, refresh](double v) {
            opacity_[i].opacity = v / 100;
            refresh();
        });
        auto* remove = new QPushButton(QStringLiteral("×"));
        remove->setFixedWidth(28);
        remove->setEnabled(opacity_.size() > 2);
        connect(remove, &QPushButton::clicked, this, [this, i] {
            if (opacity_.size() <= 2) return;
            opacity_.erase(opacity_.begin() + static_cast<std::ptrdiff_t>(i));
            fill();
        });
        opacity_rows_->addWidget(where, row, 0);
        opacity_rows_->addWidget(strong, row, 1);
        opacity_rows_->addWidget(remove, row, 3);
    }
    add_colour->setEnabled(colours_.size() < 16);
    add_opacity->setEnabled(opacity_.size() < 16);
    refresh();
}

// --- the panel ------------------------------------------------------------------------------------------------------------

GradientsPanel::GradientsPanel(GradientLibrary* library, QWidget* parent) : QWidget(parent), library_(library) {
    setObjectName(QStringLiteral("gradients_panel"));
    tree = new QTreeWidget;
    tree->setObjectName(QStringLiteral("gradients_tree"));
    tree->setHeaderHidden(true);
    tree->setIconSize(QSize(96, 16));
    tree->setRootIsDecorated(true);
    connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem*, int) {
        if (const Json* item = chosen()) emit preset_chosen(*item);
    });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { edit_chosen(); });
    apply = new QPushButton(QStringLiteral("新規グラデーション塗りつぶしレイヤー"));
    apply->setObjectName(QStringLiteral("gradients_apply"));
    apply->setToolTip(QStringLiteral("選んだグラデーション（無ければ今のグラデーション）で、新しいグラデーションの塗りつぶしレイヤーを作ります"));
    connect(apply, &QPushButton::clicked, this, [this] {
        const Json* item = chosen();
        emit apply_requested(item != nullptr ? *item : library_->current());
    });
    more = new QPushButton(QStringLiteral("グラデーション ▾"));
    more->setObjectName(QStringLiteral("gradients_menu"));
    auto* menu = new QMenu(more);
    menu->addAction(QStringLiteral("新規グラデーション…"), this, [this] { new_preset(); });
    menu->addAction(QStringLiteral("グラデーションを編集…"), this, [this] { edit_chosen(); });
    menu->addAction(QStringLiteral("グラデーション名を変更…"), this, [this] { rename_chosen(); });
    menu->addAction(QStringLiteral("グラデーションを削除"), this, [this] { delete_chosen(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("新規グラデーショングループ…"), this, [this] { new_group(); });
    menu->addAction(QStringLiteral("グラデーションを読み込み…"), this, [this] { import_grd(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("初期設定のグラデーションに戻す"), this, [this] { reset(false); });
    menu->addAction(QStringLiteral("初期設定のグラデーションを追加"), this, [this] { reset(true); });
    more->setMenu(menu);
    auto* note = new QLabel(QStringLiteral("クリックでグラデーションツールのグラデーションに（グラデーションの塗りつぶしレイヤーを選んでいれば、その色も）。"
                                           "ダブルクリックで編集"));
    note->setWordWrap(true);
    theme::hint(note);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tree, 1);
    layout->addWidget(apply);
    layout->addWidget(more);
    layout->addWidget(note);
    connect(library_, &GradientLibrary::changed, this, [this] { fill(); });
    fill();
}

void GradientsPanel::set_colours(const std::array<int, 3>& foreground, const std::array<int, 3>& background) {
    if (foreground == fg_ && background == bg_) return;
    fg_ = foreground;
    bg_ = background;
    // (review P3-5: only the strips of the presets that use the 描画色 or the 背景色 drawn again — the list as it is)
    const auto& groups = library_->groups();
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* one = group->child(i);
            const QPoint at = one->data(0, kRole).toPoint();
            if (at.x() < 0 || static_cast<std::size_t>(at.x()) >= groups.size() || at.y() < 0 ||
                static_cast<std::size_t>(at.y()) >= groups[static_cast<std::size_t>(at.x())].items.size())
                continue;
            const Json& item = groups[static_cast<std::size_t>(at.x())].items[static_cast<std::size_t>(at.y())];
            if (presets::uses_brush_colours(item)) one->setIcon(0, QIcon(gradient_strip(item, QSize(96, 16), fg_, bg_)));
        }
    }
}

void GradientsPanel::fill() {
    const QString was = tree->currentItem() != nullptr ? tree->currentItem()->text(0) : QString();
    tree->clear();
    const auto& groups = library_->groups();
    for (std::size_t g = 0; g < groups.size(); ++g) {
        auto* group = new QTreeWidgetItem(tree, QStringList{text_of(groups[g].name)});
        group->setData(0, kRole, QPoint(static_cast<int>(g), -1));
        for (std::size_t i = 0; i < groups[g].items.size(); ++i) {
            const Json& item = groups[g].items[i];
            auto* one = new QTreeWidgetItem(group, QStringList{text_of(item.value("name", std::string()))});
            one->setData(0, kRole, QPoint(static_cast<int>(g), static_cast<int>(i)));
            one->setIcon(0, QIcon(gradient_strip(item, QSize(96, 16), fg_, bg_)));
            if (one->text(0) == was) tree->setCurrentItem(one);
        }
        group->setExpanded(g == 0);
    }
}

const Json* GradientsPanel::chosen() const {
    const QTreeWidgetItem* item = tree->currentItem();
    if (item == nullptr) return nullptr;
    const QPoint at = item->data(0, kRole).toPoint();
    const auto& groups = library_->groups();
    if (at.y() < 0 || at.x() < 0 || static_cast<std::size_t>(at.x()) >= groups.size()) return nullptr;
    const auto& items = groups[static_cast<std::size_t>(at.x())].items;
    return static_cast<std::size_t>(at.y()) < items.size() ? &items[static_cast<std::size_t>(at.y())] : nullptr;
}

bool GradientsPanel::keep(const std::function<void(std::vector<store::Group>&)>& work) {
    QString error;
    if (library_->update(work, &error)) return true;
    ask::warning(this, QStringLiteral("グラデーション"), QStringLiteral("グラデーションを保存できませんでした。\n%1").arg(error));
    return false;
}

void GradientsPanel::new_preset(const QString& asked) {
    QString name = asked;
    if (name.isEmpty()) {
        const auto typed = ask::get_text(this, QStringLiteral("新規グラデーション"), QStringLiteral("名前"), QString::fromStdString(library_->current().value("name", std::string())));
        if (!typed || typed->trimmed().isEmpty()) return;
        name = typed->trimmed();
    }
    std::optional<std::string> group;
    if (const QTreeWidgetItem* item = tree->currentItem()) {
        const QPoint at = item->data(0, kRole).toPoint();
        const auto& groups = library_->groups();
        if (at.x() >= 0 && static_cast<std::size_t>(at.x()) < groups.size()) group = groups[static_cast<std::size_t>(at.x())].name;
    }
    Json item = library_->current();
    item["name"] = name.toStdString();  // (made unique among the presets by add)
    if (keep([&](std::vector<store::Group>& groups) {
            (void)store::add(groups, presets::kind(), item, group ? std::optional<std::string_view>(*group) : std::nullopt, "カスタム");
        }))
        emit said(QStringLiteral("グラデーション「%1」を足しました").arg(name));
}

void GradientsPanel::rename_chosen(const QString& asked) {
    const Json* item = chosen();
    if (item == nullptr) return;
    const std::string old = item->value("name", std::string());
    QString name = asked;
    if (name.isEmpty()) {
        const auto typed = ask::get_text(this, QStringLiteral("グラデーション名を変更"), QStringLiteral("名前"), text_of(old));
        if (!typed || typed->trimmed().isEmpty()) return;
        name = typed->trimmed();
    }
    keep([&](std::vector<store::Group>& groups) {
        (void)store::edit(groups, presets::kind(), Json{{"action", "rename"}, {"preset", old}, {"name", name.toStdString()}});
    });
}

void GradientsPanel::delete_chosen() {
    const Json* item = chosen();
    if (item == nullptr) return;
    const std::string name = item->value("name", std::string());
    keep([&](std::vector<store::Group>& groups) { (void)store::edit(groups, presets::kind(), Json{{"action", "delete"}, {"preset", name}}); });
}

void GradientsPanel::new_group(const QString& asked) {
    QString name = asked;
    if (name.isEmpty()) {
        const auto typed = ask::get_text(this, QStringLiteral("新規グラデーショングループ"), QStringLiteral("グループの名前"));
        if (!typed || typed->trimmed().isEmpty()) return;
        name = typed->trimmed();
    }
    keep([&](std::vector<store::Group>& groups) { (void)store::edit(groups, presets::kind(), Json{{"action", "new_group"}, {"name", name.toStdString()}}); });
}

namespace {

// A .grd read (in the background): its presets, what was not kept as it was, or why it cannot be read.
struct ReadGrd {
    Json items = Json::array();
    std::vector<std::string> warnings;
    std::string error;
};

ReadGrd read_grd(const QString& path) {
    ReadGrd out;
    try {
        out.items = render::grd::to_presets(
            render::grd::read(storage::read_file_bounded(storage::path_from_utf8(path.toStdString()), render::grd::kMaxBytes)), out.warnings);
    } catch (const std::exception& error) {
        out.error = error.what();
    }
    return out;
}

}  // namespace

void GradientsPanel::import_grd(const QString& asked) {
    if (importing_) return;
    QString path = asked;
    if (path.isEmpty()) path = ask::open_path(this, QStringLiteral("グラデーションを読み込み"), QStringLiteral("Photoshop のグラデーション (*.grd)"));
    if (path.isEmpty()) return;
    // (review P1-1: read and unpacked in the background — the file 32 MB, its descriptors 512 MB at most —, the panel's
    // menu grey meanwhile)
    importing_ = true;
    more->setEnabled(false);
    emit said(QStringLiteral("「%1」を読み込んでいます…").arg(QFileInfo(path).fileName()));
    auto* watcher = new QFutureWatcher<ReadGrd>(this);
    connect(watcher, &QFutureWatcher<ReadGrd>::finished, this, [this, watcher, path] {
        const ReadGrd read = watcher->result();
        watcher->deleteLater();
        importing_ = false;
        more->setEnabled(true);
        imported(path, read.items, read.warnings, read.error);
        emit import_done();
    });
    watcher->setFuture(QtConcurrent::run([path] { return read_grd(path); }));
}

void GradientsPanel::imported(const QString& path, const Json& items, const std::vector<std::string>& warnings, const std::string& error) {
    if (!error.empty()) {
        ask::warning(this, QStringLiteral("グラデーションを読み込み"), QStringLiteral("読み込めませんでした。\n%1").arg(wording::error(QString::fromUtf8(error.c_str()))));
        return;
    }
    const std::string group = render::grd::kept_name(QFileInfo(path).completeBaseName().toStdString(), "グラデーション");
    // (review P3-3: a group of the file's name there already — the person's own changes in it, perhaps — replaced only when
    // asked; else the presets added to it)
    const bool there = std::any_of(library_->groups().begin(), library_->groups().end(), [&](const store::Group& g) { return g.name == group; });
    const bool replace = there && ask::question(this, QStringLiteral("グラデーションを読み込み"),
                                                QStringLiteral("グループ「%1」はもうあります。置き換えますか？\n（いいえ: そのグループに足します）").arg(text_of(group)));
    const std::vector<Json> list(items.begin(), items.end());
    if (!keep([&](std::vector<store::Group>& groups) {
            if (replace) std::erase_if(groups, [&](const store::Group& g) { return g.name == group; });
            (void)store::add_many(groups, presets::kind(), list, group, "グラデーション");  // (the names taken counted once)
        }))
        return;
    QString words = QStringLiteral("グラデーションを %1 個読み込みました（グループ「%2」）").arg(list.size()).arg(text_of(group));
    for (const std::string& w : warnings) words += QStringLiteral("\n・") + text_of(w);
    emit said(words);
}

void GradientsPanel::reset(bool append) {
    if (!append && !ask::question(this, QStringLiteral("初期設定のグラデーションに戻す"), QStringLiteral("今のグラデーションを初期設定のものに置き換えますか？"))) return;
    keep([&](std::vector<store::Group>& groups) { groups = store::reset(groups, presets::kind(), append); });
}

void GradientsPanel::edit_chosen() {
    const Json* item = chosen();
    const Json now = item != nullptr ? *item : library_->current();
    const std::optional<std::string> old = item != nullptr ? std::optional<std::string>(item->value("name", std::string())) : std::nullopt;
    GradientEditor editor(now, fg_, bg_, this);
    if (ask::exec(&editor) != QDialog::Accepted) return;
    Json edited;
    try {
        edited = editor.item();
    } catch (const std::exception& error) {
        emit said(wording::error(QString::fromUtf8(error.what())));
        return;
    }
    if (!old) {  // (the current gradient, not a preset)
        library_->set_current(edited);
        return;
    }
    bool found = false;
    const bool kept = keep([&](std::vector<store::Group>& groups) {
        for (store::Group& g : groups) {
            for (Json& each : g.items) {
                if (each.value("name", std::string()) != *old) continue;
                const std::string want = edited["name"].get<std::string>();
                if (want != *old && store::find(groups, want) != nullptr) edited["name"] = store::unique_name(groups, want);
                each = edited;
                found = true;
                return;
            }
        }
    });
    if (kept && found) {
        library_->set_current(edited);
        emit preset_chosen(edited);
    }
}

}  // namespace genko::app
