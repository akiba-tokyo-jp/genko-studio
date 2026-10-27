// PC4 E6 (window.panel.styles, style.presets.*; SPEC.md COLOR-09「スタイル」): the Styles panel (app/styles_panel.hpp).
// Library edits are the machine's settings (never the book's history); a style applied is set_layer_style through the
// window, so one Undo takes it back. PhotoCraft v0.2.0's ui-egui/src/preset_panels.rs is the shape it follows (no code
// taken).

#include "app/styles_panel.hpp"

#include <QApplication>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <filesystem>
#include <map>
#include <mutex>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "render/asl.hpp"
#include "render/grd.hpp"
#include "render/pattern_library.hpp"
#include "render/preset_store.hpp"
#include "render/style_presets.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace store = render::preset_store;
namespace styles = render::style_presets;

namespace {

constexpr int kName = Qt::UserRole;     // (a preset's name; empty for a group)
constexpr int kKey = Qt::UserRole + 1;  // (its swatch's key)
constexpr int kSide = 48;

// The swatches made, by the preset's settings (its name aside) and side: shared by the windows, at most 512.
std::mutex& swatch_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, QImage>& swatches() {
    static std::map<std::string, QImage> made;
    return made;
}

std::string key_of(const Json& preset) {
    Json settings = preset;
    settings.erase("name");
    return settings.dump() + "|" + std::to_string(kSide);
}

QImage qimage_of(const render::Image& image) {
    const std::string bytes = image.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 4, QImage::Format_RGBA8888).copy();
}

QString about(const Json& preset) {
    QStringList kinds;
    for (const Json& e : preset.value("effects", Json::array())) kinds << QString::fromStdString(e.value("kind", std::string()));
    return kinds.isEmpty() ? QStringLiteral("効果なし") : kinds.join(QStringLiteral("・"));
}

}  // namespace

StylesPanel::StylesPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("styles_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("styles_tree"));
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(kSide, kSide));
    tree_->setToolTip(QStringLiteral("クリックで選んでいるレイヤーに当てます（Shift を押しながら: 今の効果に加えます）"));
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (loading_ || item == nullptr) return;
        const std::string name = item->data(0, kName).toString().toStdString();
        if (!name.empty()) apply(name, (QApplication::keyboardModifiers() & Qt::ShiftModifier) != 0);
    });
    auto* row = new QHBoxLayout;
    const auto add = [&](const char* name, const QString& text, const QString& tip, const std::function<void()>& work) {
        auto* b = new QPushButton(text);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, work);
        row->addWidget(b);
        return b;
    };
    add("style_new", QStringLiteral("新規…"), QStringLiteral("選んでいるレイヤーの効果と混合オプションを、新しいスタイルにします（新規スタイル…）"),
        [this] { window_->new_style_preset(); });
    add("style_delete", QStringLiteral("削除"), QStringLiteral("選んだスタイルを消します"), [this] { remove(); });
    auto* more = new QToolButton;
    more->setObjectName(QStringLiteral("style_more"));
    more->setText(QStringLiteral("…"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->setObjectName(QStringLiteral("style_menu"));
    menu->addAction(QStringLiteral("名前を変更…"), this, [this] { rename(); });
    menu->addAction(QStringLiteral("新規スタイルグループ…"), this, [this] { new_group(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("スタイルを読み込み…"), this, [this] {
        const QString path = ask::open_path(this, QStringLiteral("スタイルを読み込み"), QStringLiteral("スタイル (*.asl)"));
        if (!path.isEmpty()) import_asl(path);
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("スタイルを初期設定に戻す"), this, [this] { reset(false); });
    menu->addAction(QStringLiteral("初期設定のスタイルを追加"), this, [this] { reset(true); });
    more->setMenu(menu);
    row->addWidget(more);
    layout->addLayout(row);
}

QPushButton* StylesPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

std::string StylesPanel::chosen() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    return item != nullptr ? item->data(0, kName).toString().toStdString() : std::string();
}

void StylesPanel::refresh() {
    loading_ = true;
    const std::string current = chosen();
    tree_->clear();
    const store::Library lib = store::load(config_dir(), styles::kind());
    QTreeWidgetItem* again = nullptr;
    for (const store::Group& g : lib.groups) {
        auto* group = new QTreeWidgetItem(tree_, {QString::fromStdString(g.name)});
        group->setFlags(Qt::ItemIsEnabled);
        for (const Json& preset : g.items) {
            const std::string name = preset.value("name", std::string());
            auto* item = new QTreeWidgetItem(group, {QString::fromStdString(name)});
            item->setData(0, kName, QString::fromStdString(name));
            item->setData(0, kKey, QString::fromStdString(key_of(preset)));
            item->setToolTip(0, about(preset));
            ask_swatch(item, preset);
            if (name == current) again = item;
        }
        group->setExpanded(true);
    }
    if (!lib.problem.empty()) {
        auto* note = new QTreeWidgetItem(tree_, {QStringLiteral("（スタイルのファイルを読めないので、初期設定を見せています）")});
        note->setFlags(Qt::ItemIsEnabled);
    }
    if (again != nullptr) tree_->setCurrentItem(again);
    loading_ = false;
}

void StylesPanel::ask_swatch(QTreeWidgetItem* item, const Json& preset) {
    const std::string key = key_of(preset);
    {
        const std::lock_guard lock(swatch_mutex());
        if (const auto it = swatches().find(key); it != swatches().end()) {
            item->setIcon(0, QIcon(QPixmap::fromImage(it->second)));
            return;
        }
    }
    // (made on a worker: the style drawn small, its patterns the library's — their pictures read there, each once)
    ++pending_;
    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        watcher->deleteLater();
        --pending_;
        put_swatches();
        if (pending_ == 0) emit swatches_done();
    });
    const std::filesystem::path dir = config_dir();
    watcher->setFuture(QtConcurrent::run([preset, key, dir] {
        QImage made;
        try {
            const store::Library patterns = render::pattern_library::load(dir);
            made = qimage_of(styles::thumbnail(preset, kSide, dir, patterns.groups));
        } catch (const std::exception&) {
            made = QImage(kSide, kSide, QImage::Format_RGBA8888);
            made.fill(Qt::transparent);
        }
        const std::lock_guard lock(swatch_mutex());
        if (swatches().size() > 512) swatches().clear();
        swatches()[key] = made;
    }));
}

void StylesPanel::put_swatches() {
    const std::lock_guard lock(swatch_mutex());
    for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = tree_->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* item = group->child(i);
            if (!item->icon(0).isNull()) continue;
            const auto it = swatches().find(item->data(0, kKey).toString().toStdString());
            if (it != swatches().end()) item->setIcon(0, QIcon(QPixmap::fromImage(it->second)));
        }
    }
}

bool StylesPanel::apply(const std::string& name, bool add) {
    const store::Library lib = store::load(config_dir(), styles::kind());
    const Json* preset = store::find(lib.groups, name);
    if (preset == nullptr) return false;
    return window_->apply_style_preset(*preset, add);
}

void StylesPanel::rename(std::optional<QString> name) {
    const std::string now = chosen();
    if (now.empty()) return;
    if (!name) {
        name = ask::get_text(this, QStringLiteral("スタイル名を変更"), QStringLiteral("スタイル名"), QString::fromStdString(now));
        if (!name) return;
    }
    const QString given = name->trimmed();
    if (given.isEmpty()) return;
    store::Library lib = store::load(config_dir(), styles::kind());
    try {
        store::edit(lib.groups, styles::kind(), Json{{"action", "rename"}, {"preset", now}, {"name", given.toStdString()}});
        store::save(config_dir(), styles::kind(), lib.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("スタイル名を変更"), QString::fromUtf8(error.what()));
    }
    refresh();
}

void StylesPanel::remove() {
    const std::string now = chosen();
    if (now.empty()) return;
    store::Library lib = store::load(config_dir(), styles::kind());
    try {
        store::edit(lib.groups, styles::kind(), Json{{"action", "delete"}, {"preset", now}});
        store::save(config_dir(), styles::kind(), lib.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("スタイルを削除"), QString::fromUtf8(error.what()));
    }
    refresh();
}

void StylesPanel::new_group(std::optional<QString> name) {
    if (!name) {
        name = ask::get_text(this, QStringLiteral("新規スタイルグループ"), QStringLiteral("グループ名"), QStringLiteral("グループ 1"));
        if (!name) return;
    }
    if (name->trimmed().isEmpty()) return;
    store::Library lib = store::load(config_dir(), styles::kind());
    try {
        store::edit(lib.groups, styles::kind(), Json{{"action", "new_group"}, {"group", name->trimmed().toStdString()}});
        store::save(config_dir(), styles::kind(), lib.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("新規スタイルグループ"), QString::fromUtf8(error.what()));
    }
    refresh();
}

bool StylesPanel::import_asl(const QString& path) {
    Json added = Json::array(), notes = Json::array();
    QString group_name;
    try {
        const std::filesystem::path file = storage::path_from_utf8(path.toStdString());
        const std::string group = render::grd::kept_name(storage::path_to_utf8(file.stem()), "スタイル");
        group_name = QString::fromStdString(group);
        const std::filesystem::path dir = config_dir();
        store::Library lib = store::load(dir, styles::kind());
        if (!lib.problem.empty()) throw core::Error("io", "the styles' file cannot be read: " + lib.problem);
        store::Library patterns = render::pattern_library::load(dir);
        if (!patterns.problem.empty()) throw core::Error("io", "the pattern library cannot be read: " + patterns.problem);
        // (the file read, its styles made and its patterns' pictures written off the window's thread)
        struct Read {
            Json read;
            std::vector<store::Group> patterns;
            bool added = false;
        };
        const Read done = off_ui([file, dir, group, groups = patterns.groups]() mutable {
            Read out;
            out.read = styles::import_asl(storage::read_file_bounded(file, render::asl::kMaxFileBytes), 150.0, dir, groups, group, out.added);
            out.patterns = std::move(groups);
            return out;
        });
        const Json& items = done.read["items"];
        added = store::add_many(lib.groups, styles::kind(), std::vector<Json>(items.begin(), items.end()), group, "スタイル")["names"];
        if (done.added) render::pattern_library::save(dir, done.patterns);
        store::save(dir, styles::kind(), lib.groups);
        notes = done.read["notes"];
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("スタイルを読み込み"), QStringLiteral("スタイルを読み込めませんでした: %1").arg(QString::fromUtf8(error.what())));
        refresh();
        return false;
    }
    window_->flash(QStringLiteral("スタイルを %1 個「%2」に読み込みました%3")
                       .arg(added.size())
                       .arg(group_name)
                       .arg(notes.empty() ? QString() : QStringLiteral("（そのままでは読めなかったものが %1 件あります）").arg(notes.size())),
                   5000);
    refresh();
    return true;
}

void StylesPanel::reset(bool append) {
    store::Library lib = store::load(config_dir(), styles::kind());
    try {
        lib.groups = store::reset(lib.groups, styles::kind(), append);
        store::save(config_dir(), styles::kind(), lib.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("スタイルを初期設定に戻す"), QString::fromUtf8(error.what()));
    }
    refresh();
}

}  // namespace genko::app
