// PC4 E3 (window.panel.patterns, pattern.*, pattern.presets.*; SPEC.md COLOR-09「パターン」): the Patterns panel — the
// machine's library (render/pattern_library) by its groups and this book's own patterns, as Photoshop's パターン パネル.
// Library edits are the machine's settings (never the book's history); the book's are ops (rename_pattern,
// delete_pattern) through the window, so one Undo takes each back. PhotoCraft v0.2.0's ui-egui/src/preset_panels.rs is
// the shape it follows (no code taken).

#include "app/patterns_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <map>
#include <mutex>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/theme.hpp"
#include "core/patterns.hpp"
#include "render/pat.hpp"
#include "render/pattern_library.hpp"
#include "render/patterns.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace library = render::pattern_library;
namespace store = render::preset_store;

namespace {

constexpr int kRole = Qt::UserRole;  // (an item's choice: "library:<name>" or "book:<id>")

QImage qimage_of(const render::Image& image) {
    const render::Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    const std::string bytes = rgba.tobytes();
    QImage out(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888);
    return out.copy();
}

QString about(const core::Pattern& p) {
    const QString mode = p.mode == "gray" ? QStringLiteral("グレー") : p.mode == "cmyk" ? QStringLiteral("CMYK") : p.mode == "lab" ? QStringLiteral("Lab") : QStringLiteral("RGB");
    const double mm = 25.4 / p.dpi;
    return QStringLiteral("%1 × %2 画素・%3 %4 bit・%5 dpi（100 % で %6 × %7 mm ごとに繰り返します）")
        .arg(p.width)
        .arg(p.height)
        .arg(mode)
        .arg(p.depth)
        .arg(p.dpi, 0, 'f', 0)
        .arg(p.width * mm, 0, 'f', 1)
        .arg(p.height * mm, 0, 'f', 1);
}

}  // namespace

QIcon pattern_icon(const core::Pattern& pattern, const std::function<core::Bytes()>& load, int side) {
    static std::mutex mutex;
    static std::map<std::string, QIcon> made;
    const std::string key = pattern.asset + "|" + std::to_string(side);
    {
        const std::lock_guard lock(mutex);
        if (const auto it = made.find(key); it != made.end()) return it->second;
    }
    QIcon icon;
    if (const core::Bytes bytes = load()) {
        try {
            QImage image(side, side, QImage::Format_RGBA8888);
            image.fill(Qt::white);  // (a pattern with clear pixels over white, as on the paper)
            const QImage tile = qimage_of(render::patterns::swatch(pattern, *bytes, side));
            QPainter painter(&image);
            painter.drawImage(0, 0, tile);
            painter.end();
            icon = QIcon(QPixmap::fromImage(image));
        } catch (const std::exception&) {
        }
    }
    const std::lock_guard lock(mutex);
    if (made.size() > 512) made.clear();
    made[key] = icon;
    return icon;
}

std::optional<std::pair<core::Pattern, core::Bytes>> pattern_chosen(const core::Document& book, const QString& choice) {
    if (choice.startsWith(QLatin1String("book:"))) {
        const core::Pattern* p = core::find_pattern(book, choice.mid(5).toStdString());
        if (p == nullptr) return std::nullopt;
        const auto file = book.pattern_files.find(p->asset);
        if (file == book.pattern_files.end() || !file->second) return std::nullopt;
        return std::pair<core::Pattern, core::Bytes>{*p, file->second};
    }
    if (choice.startsWith(QLatin1String("library:"))) {
        const store::Library lib = library::load(config_dir());
        const Json* item = store::find(lib.groups, choice.mid(8).toStdString());
        if (item == nullptr) return std::nullopt;
        try {
            core::Pattern p = library::pattern_of(*item);
            p.name = choice.mid(8).toStdString();
            core::Bytes bytes = library::picture(config_dir(), p);
            if (!bytes) return std::nullopt;
            return std::pair<core::Pattern, core::Bytes>{p, bytes};
        } catch (const core::Error&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

QComboBox* pattern_combo(const core::Document& book, const QString& current) {
    auto* box = new QComboBox;
    box->setObjectName(QStringLiteral("pattern_choice"));
    box->setIconSize(QSize(24, 24));
    for (const core::Pattern& p : book.patterns) {
        const auto file = book.pattern_files.find(p.asset);
        const core::Bytes held = file != book.pattern_files.end() ? file->second : nullptr;
        box->addItem(pattern_icon(p, [held] { return held; }, 24),
                     QString::fromStdString(p.name) + QStringLiteral("（この原稿）"), QStringLiteral("book:") + QString::fromStdString(p.id));
    }
    const store::Library lib = library::load(config_dir());
    for (const store::Group& g : lib.groups) {
        for (const Json& item : g.items) {
            const QString name = QString::fromStdString(item.value("name", std::string()));
            try {
                core::Pattern p = library::pattern_of(item);
                box->addItem(pattern_icon(p, [p] { return library::picture(config_dir(), p); }, 24), name, QStringLiteral("library:") + name);
            } catch (const core::Error&) {
            }
        }
    }
    const int at = box->findData(current);
    box->setCurrentIndex(at >= 0 ? at : 0);
    return box;
}

std::optional<PatternFillSettings> pattern_fill_dialog(QWidget* parent, const core::Document& book, const PatternFillSettings& start,
                                                       const QString& title) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pattern_fill_dialog"));
    dialog.setWindowTitle(title);
    auto* form = new QFormLayout(&dialog);
    QComboBox* choice = pattern_combo(book, start.choice);
    auto* scale = new QDoubleSpinBox;
    scale->setObjectName(QStringLiteral("pattern_scale"));
    scale->setRange(core::kPatternScaleMin, core::kPatternScaleMax);
    scale->setDecimals(0);
    scale->setSuffix(QStringLiteral(" %"));
    scale->setValue(start.scale);
    auto* angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("pattern_angle"));
    angle->setRange(-360, 360);
    angle->setDecimals(1);
    angle->setSuffix(QStringLiteral("°"));
    angle->setValue(start.angle);
    auto* link = new QCheckBox(QStringLiteral("レイヤーにリンク"));
    link->setObjectName(QStringLiteral("pattern_link"));
    link->setChecked(start.link);
    form->addRow(QStringLiteral("パターン"), choice);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QStringLiteral("比率"), scale);
    form->addRow(link);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (choice->count() == 0) return std::nullopt;
    if (ask::exec(&dialog) != QDialog::Accepted) return std::nullopt;
    return PatternFillSettings{choice->currentData().toString(), scale->value(), angle->value(), link->isChecked()};
}

PatternsPanel::PatternsPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("patterns_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("patterns_tree"));
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(32, 32));
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { chosen(item); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString choice = item != nullptr ? item->data(0, kRole).toString() : QString();
        if (!choice.isEmpty()) window_->new_pattern_layer(choice);
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
    add("pattern_define", QStringLiteral("定義…"), QStringLiteral("選択範囲（無ければページ全体）を、見えているとおりにパターンのライブラリに加えます（パターンを定義…）"),
        [this] { window_->define_pattern(); });
    add("pattern_new_layer", QStringLiteral("＋レイヤー"), QStringLiteral("選んだパターンで新しいパターン塗りつぶしレイヤーを作ります"),
        [this] { window_->new_pattern_layer(window_->current_pattern()); });
    add("pattern_rename", QStringLiteral("名前…"), QStringLiteral("選んだパターンの名前を変えます"), [this] { rename(); });
    add("pattern_delete", QStringLiteral("削除"), QStringLiteral("選んだパターンを消します（この原稿のパターンは、塗りに使っている間は消せません）"),
        [this] { remove(); });
    auto* more = new QToolButton;
    more->setObjectName(QStringLiteral("pattern_more"));
    more->setText(QStringLiteral("…"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->setObjectName(QStringLiteral("pattern_menu"));
    menu->addAction(QStringLiteral("パターンを読み込み…"), this, [this] {
        const QString path = ask::open_path(this, QStringLiteral("パターンを読み込み"), QStringLiteral("パターン (*.pat)"));
        if (!path.isEmpty()) import_pat(path);
    });
    menu->addAction(QStringLiteral("パターンを書き出し…"), this, [this] {
        const QString path = ask::save_path(this, QStringLiteral("パターンを書き出し"), QStringLiteral("パターン.pat"), QStringLiteral("パターン (*.pat)"));
        if (!path.isEmpty()) export_pat(path);
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("新規グループ…"), this, [this] {
        const auto name = ask::get_text(this, QStringLiteral("新規グループ"), QStringLiteral("グループ名"), QStringLiteral("グループ 1"));
        if (!name || name->trimmed().isEmpty()) return;
        store::Library lib = library::load(config_dir());
        try {
            store::edit(lib.groups, library::kind(), Json{{"action", "new_group"}, {"group", name->trimmed().toStdString()}});
            library::save(config_dir(), lib.groups);
        } catch (const core::Error& error) {
            ask::warning(this, QStringLiteral("新規グループ"), QString::fromUtf8(error.what()));
        }
        refresh();
    });
    menu->addAction(QStringLiteral("ライブラリに追加"), this, [this] { to_library(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("パターンを初期設定に戻す"), this, [this] { reset_library(); });
    more->setMenu(menu);
    row->addWidget(more);
    layout->addLayout(row);
}

QPushButton* PatternsPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

void PatternsPanel::refresh() {
    loading_ = true;
    const QString current = window_->current_pattern();
    tree_->clear();
    QTreeWidgetItem* chosen_item = nullptr;
    const core::Document& book = window_->book();
    if (!book.patterns.empty()) {
        auto* group = new QTreeWidgetItem(tree_, {QStringLiteral("この原稿のパターン")});
        group->setFlags(Qt::ItemIsEnabled);
        for (const core::Pattern& p : book.patterns) {
            const auto file = book.pattern_files.find(p.asset);
            auto* item = new QTreeWidgetItem(group, {QString::fromStdString(p.name)});
            const core::Bytes held = file != book.pattern_files.end() ? file->second : nullptr;
            item->setIcon(0, pattern_icon(p, [held] { return held; }));
            item->setToolTip(0, about(p));
            item->setData(0, kRole, QStringLiteral("book:") + QString::fromStdString(p.id));
            if (item->data(0, kRole).toString() == current) chosen_item = item;
        }
        group->setExpanded(true);
    }
    const store::Library lib = library::load(config_dir());
    for (const store::Group& g : lib.groups) {
        auto* group = new QTreeWidgetItem(tree_, {QString::fromStdString(g.name)});
        group->setFlags(Qt::ItemIsEnabled);
        for (const Json& it : g.items) {
            const QString name = QString::fromStdString(it.value("name", std::string()));
            auto* item = new QTreeWidgetItem(group, {name});
            try {
                const core::Pattern p = library::pattern_of(it);
                item->setIcon(0, pattern_icon(p, [p] { return library::picture(config_dir(), p); }));
                item->setToolTip(0, about(p));
            } catch (const core::Error&) {
            }
            item->setData(0, kRole, QStringLiteral("library:") + name);
            if (item->data(0, kRole).toString() == current) chosen_item = item;
        }
        group->setExpanded(true);
    }
    if (!lib.problem.empty()) {
        auto* note = new QTreeWidgetItem(tree_, {QStringLiteral("（パターンのライブラリのファイルを読めないので、初期設定を見せています）")});
        note->setFlags(Qt::ItemIsEnabled);
    }
    if (chosen_item != nullptr) tree_->setCurrentItem(chosen_item);
    loading_ = false;
}

void PatternsPanel::chosen(QTreeWidgetItem* item) {
    if (loading_ || item == nullptr) return;
    const QString choice = item->data(0, kRole).toString();
    if (!choice.isEmpty()) window_->set_current_pattern(choice);  // (pattern.presets.select: the window's state)
}

void PatternsPanel::rename(std::optional<QString> name) {
    const QString choice = window_->current_pattern();
    const auto chosen = pattern_chosen(window_->book(), choice);
    if (!chosen) return;
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("名前を変更"), QStringLiteral("パターン名"), QString::fromStdString(chosen->first.name));
        if (!asked) return;
        name = asked;
    }
    const QString given = name->trimmed();
    if (given.isEmpty()) return;
    if (choice.startsWith(QLatin1String("book:"))) {
        window_->apply_ops(Json::array({Json{{"op", "rename_pattern"}, {"id", chosen->first.id}, {"name", given.toStdString()}}}));
        refresh();
        return;
    }
    store::Library lib = library::load(config_dir());
    try {
        const Json out = store::edit(lib.groups, library::kind(), Json{{"action", "rename"}, {"preset", chosen->first.name}, {"name", given.toStdString()}});
        library::save(config_dir(), lib.groups);
        window_->set_current_pattern(QStringLiteral("library:") + QString::fromStdString(out.value("name", given.toStdString())));
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("名前を変更"), QString::fromUtf8(error.what()));
    }
    refresh();
}

void PatternsPanel::remove() {
    const QString choice = window_->current_pattern();
    const auto chosen = pattern_chosen(window_->book(), choice);
    if (!chosen) return;
    if (choice.startsWith(QLatin1String("book:"))) {
        if (core::patterns_used(window_->book()).contains(chosen->first.id)) {
            ask::warning(this, QStringLiteral("パターンを削除"), QStringLiteral("このパターンは塗りに使われているので消せません（その塗りを変えてから消してください）"));
            return;
        }
        if (window_->apply_ops(Json::array({Json{{"op", "delete_pattern"}, {"id", chosen->first.id}}}))) window_->set_current_pattern({});
        refresh();
        return;
    }
    store::Library lib = library::load(config_dir());
    try {
        store::edit(lib.groups, library::kind(), Json{{"action", "delete"}, {"preset", chosen->first.name}});
        library::save(config_dir(), lib.groups);
        library::tidy(config_dir(), lib.groups);
        window_->set_current_pattern({});
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("パターンを削除"), QString::fromUtf8(error.what()));
    }
    refresh();
}

bool PatternsPanel::import_pat(const QString& path) {
    store::Library lib = library::load(config_dir());
    QStringList skipped;
    int added = 0;
    try {
        // (the file read and its patterns made off the window's thread; the library written here)
        struct Read {
            std::vector<render::pat::Taken> taken;
            std::vector<std::string> skipped;
        };
        const std::filesystem::path file = storage::path_from_utf8(path.toStdString());
        const Read read = off_ui([file] {
            Read out;
            const std::string bytes = storage::read_file_bounded(file, static_cast<std::size_t>(render::pat::kMaxFileBytes));
            for (const render::pat::PsdPattern& p : render::pat::parse_file(bytes)) {
                if (auto one = render::pat::from_psd(p, 300.0)) {
                    out.taken.push_back(std::move(*one));
                } else {
                    out.skipped.push_back(render::pat::shown_name(p.name));
                }
            }
            return out;
        });
        for (const std::string& name : read.skipped) skipped << QString::fromStdString(name);
        for (const render::pat::Taken& one : read.taken) {
            library::add(config_dir(), lib.groups, one.pattern, one.bytes, std::nullopt);
            ++added;
        }
        library::save(config_dir(), lib.groups);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("パターンを読み込み"), QStringLiteral("パターンを読み込めませんでした: %1").arg(QString::fromUtf8(error.what())));
        refresh();
        return false;
    }
    window_->flash(skipped.isEmpty() ? QStringLiteral("パターンを %1 個読み込みました").arg(added)
                                     : QStringLiteral("パターンを %1 個読み込みました（読めなかったもの: %2）").arg(added).arg(skipped.join(QStringLiteral("、"))),
                   5000);
    refresh();
    return true;
}

bool PatternsPanel::export_pat(const QString& path) {
    std::size_t count = 0;
    const QString choice = window_->current_pattern();
    try {
        // (the pictures to write gathered here; turned into the file's and written off the window's thread)
        std::vector<std::pair<core::Pattern, core::Bytes>> chosen_ones;
        if (const auto chosen = pattern_chosen(window_->book(), choice)) {
            chosen_ones.push_back(*chosen);
        } else {
            const store::Library lib = library::load(config_dir());
            for (const store::Group& g : lib.groups) {
                for (const Json& item : g.items) {
                    core::Pattern p = library::pattern_of(item);
                    p.name = item.value("name", std::string());
                    if (core::Bytes bytes = library::picture(config_dir(), p)) chosen_ones.emplace_back(p, std::move(bytes));
                }
            }
        }
        const std::filesystem::path file = storage::path_from_utf8(path.toStdString());
        count = off_ui([chosen_ones, file] {
            std::vector<render::pat::PsdPattern> written;
            for (const auto& [p, bytes] : chosen_ones) written.push_back(render::pat::to_psd(p, *bytes, render::pat::file_id(p)));
            storage::write_atomic(file, render::pat::write_file(written));
            return written.size();
        });
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("パターンを書き出し"), QStringLiteral("書き出せませんでした: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
    window_->flash(QStringLiteral("パターンを %1 個書き出しました").arg(count), 4000);
    return true;
}

void PatternsPanel::to_library() {
    const QString choice = window_->current_pattern();
    if (!choice.startsWith(QLatin1String("book:"))) return;
    const auto chosen = pattern_chosen(window_->book(), choice);
    if (!chosen) return;
    store::Library lib = library::load(config_dir());
    try {
        core::Pattern p = chosen->first;
        p.id.clear();
        const Json out = library::add(config_dir(), lib.groups, p, chosen->second, std::nullopt);
        library::save(config_dir(), lib.groups);
        window_->flash(QStringLiteral("「%1」をパターンのライブラリに加えました").arg(QString::fromStdString(out.value("name", p.name))), 4000);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("ライブラリに追加"), QString::fromUtf8(error.what()));
    }
    refresh();
}

void PatternsPanel::reset_library() {
    store::Library lib = library::load(config_dir());
    try {
        lib.groups = store::reset(lib.groups, library::kind(), false);
        library::save(config_dir(), lib.groups);
        library::tidy(config_dir(), lib.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("パターンを初期設定に戻す"), QString::fromUtf8(error.what()));
    }
    refresh();
}

}  // namespace genko::app
