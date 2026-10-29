// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): Photoshop's 注釈 — the 注釈ツール (in the スポイト group; its options:
// 作成者・カラー・すべてを消去・注釈パネルの表示), ウィンドウ → 注釈 (the dock and its panel, app/notes_panel), 表示 →
// 表示・非表示 → 注釈 (the extra "notes": app/canvas_notes.cpp) and ファイル → 読み込み → 注釈… (formats/notes_import, off
// the window's thread) — each change of the book an op through apply_ops (render/ops_annotations), as the person.

#include <QAction>
#include <QColorDialog>
#include <QDockWidget>
#include <QFileInfo>
#include <QColor>
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/notes_panel.hpp"
#include "app/off_ui.hpp"
#include "app/tool_settings.hpp"
#include "core/paths.hpp"
#include "formats/notes_import.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;

namespace {

QString default_author() {
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty()) name = qEnvironmentVariable("USERNAME");
    return name;
}

QColor note_colour() {
    const QColor kept(settings()->value(QStringLiteral("notes/color"), QStringLiteral("#ffffb5")).toString());
    return kept.isValid() ? kept : QColor(255, 255, 181);
}

void paint_swatch(QToolButton* button, const QColor& colour) {
    QPixmap swatch(28, 16);
    swatch.fill(colour);
    button->setIcon(QIcon(swatch));
    button->setIconSize(swatch.size());
}

}  // namespace

void MainWindow::build_note_actions() {
    // the 注釈ツール (Photoshop's, in the スポイト group: app/paint_panels.cpp); no key of its own (I is the group's)
    QAction* tool = make(QStringLiteral("act_note"), QStringLiteral("注釈ツール"), [this] { choose_tool(QStringLiteral("note")); }, {},
                         QStringLiteral("クリックした所に注釈を置きます（注釈をドラッグで移動、ダブルクリックで開く・閉じる）"), true);
    tools_->addAction(tool);
    tool->setAutoRepeat(false);
    tool_actions_[QStringLiteral("note")] = tool;
    // 表示 → 表示・非表示 → 注釈 (view.show.notes): the extra "notes" on the canvas
    QAction* shown = make(QStringLiteral("act_show_notes"), QStringLiteral("注釈"), [] {}, {}, QStringLiteral("ページに置いた注釈を表示します"), true);
    shown->setChecked(settings()->value(QStringLiteral("view/show_notes"), true).toBool());
    connect(shown, &QAction::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("view/show_notes"), on);
        canvas_->show_extra(QStringLiteral("notes"), on);
        canvas_->update();
    });
    // ファイル → 読み込み → 注釈… (file.import.notes)
    make(QStringLiteral("act_import_notes"), QStringLiteral("注釈…"), [this] { import_notes(); }, {},
         QStringLiteral("PSD・PSB・.pcraft の注釈を、このページに読み込みます"));
    // ウィンドウ → 注釈 (window.panel.notes)
    make(QStringLiteral("act_panel_notes"), QStringLiteral("注釈"),
         [this] {
             if (notes_dock_ == nullptr) return;
             const bool show = !notes_dock_->isVisible();
             notes_dock_->setVisible(show);
             if (show) notes_dock_->raise();
         },
         {}, QStringLiteral("ページに置いた注釈の一覧と本文"), true);
}

void MainWindow::build_notes_menus(QMenu* file, QMenu* view) {
    // ファイル → 読み込み ▸ (Photoshop's; after the other ways in, PSD をレイヤーのまま読み込む…)
    import_menu_ = new QMenu(QStringLiteral("読み込み"), file);
    import_menu_->setObjectName(QStringLiteral("menu_import"));
    import_menu_->addAction(action(QStringLiteral("act_import_notes")));
    const QList<QAction*> items = file->actions();
    const auto psd = std::find(items.begin(), items.end(), action(QStringLiteral("act_import_psd")));
    if (psd != items.end() && std::next(psd) != items.end()) {
        file->insertMenu(*std::next(psd), import_menu_);
    } else {
        file->addMenu(import_menu_);
    }
    // 表示 → 表示・非表示 ▸ (節 H's place for the extras: made here when it is not yet — "menu_show", laid out by view_rows)
    QMenu* shows = show_menu(view);
    if (!view->actions().contains(shows->menuAction())) view->addMenu(shows);
    shows->addAction(action(QStringLiteral("act_show_notes")));
}

QMenu* MainWindow::show_menu(QMenu* view) {
    for (QAction* item : view->actions()) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_show")) return item->menu();
    }
    auto* menu = new QMenu(QStringLiteral("表示・非表示"), view);
    menu->setObjectName(QStringLiteral("menu_show"));
    return menu;
}

void MainWindow::build_notes_dock() {
    notes_panel_ = new NotesPanel(this);
    notes_dock_ = new QDockWidget(QStringLiteral("注釈"), this);
    notes_dock_->setObjectName(QStringLiteral("注釈"));
    notes_dock_->setWidget(notes_panel_);
    notes_dock_->setMinimumWidth(200);
    notes_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, notes_dock_);
    if (layers_dock_ != nullptr) tabifyDockWidget(layers_dock_, notes_dock_);
    notes_dock_->hide();  // (ウィンドウ → 注釈 brings it, and a new note)
    QAction* toggle = action(QStringLiteral("act_panel_notes"));
    view_menu_->addAction(toggle);
    connect(notes_dock_, &QDockWidget::visibilityChanged, this, [this, toggle](bool) {
        const QSignalBlocker quiet(toggle);
        toggle->setChecked(notes_dock_->isVisible());
        if (notes_dock_->isVisible() && notes_panel_ != nullptr) notes_panel_->refresh();
    });
    // the canvas: the notes drawn over the page, the 注釈ツール's clicks and drags
    canvas_->add_extra({QStringLiteral("notes"), [this](QPainter& painter, const core::Page& page, const PageCanvas::ExtraView& view) {
                            canvas_->paint_notes(painter, page, view);
                        }});
    canvas_->show_extra(QStringLiteral("notes"), action(QStringLiteral("act_show_notes"))->isChecked());
    connect(canvas_, &PageCanvas::noteRequested, this, [this](double x, double y) { add_note_at(x, y); });
    connect(canvas_, &PageCanvas::noteChosen, this, [this](const QString& id) { choose_note(id); });
    connect(canvas_, &PageCanvas::noteMoved, this, [this](const QString& id, double x, double y) {
        note_op(Json{{"op", "set_annotation"}, {"id", id.toStdString()}, {"at_mm", Json::array({x, y})}});
    });
    connect(canvas_, &PageCanvas::noteOpened, this, [this](const QString& id) {
        const core::Page* page = current_page();
        if (page == nullptr) return;
        std::optional<bool> open;
        for (const core::Annotation& a : page->annotations)
            if (QString::fromStdString(a.id) == id) open = a.open;
        if (!open) return;
        note_op(Json{{"op", "set_annotation"}, {"id", id.toStdString()}, {"open", !*open}});
        choose_note(id);
    });
    // the 注釈ツール's options: 作成者・カラー・すべてを消去・注釈パネルの表示切り替え (Photoshop's)
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("note_settings"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(QStringLiteral("作成者")));
    note_author_ = new QLineEdit(settings()->value(QStringLiteral("notes/author"), default_author()).toString());
    note_author_->setObjectName(QStringLiteral("note_author"));
    note_author_->setToolTip(QStringLiteral("新しい注釈の作成者の名前"));
    connect(note_author_, &QLineEdit::editingFinished, this, [this] { settings()->setValue(QStringLiteral("notes/author"), note_author_->text()); });
    layout->addWidget(note_author_);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("カラー")));
    note_colour_ = new QToolButton;
    note_colour_->setObjectName(QStringLiteral("note_color"));
    note_colour_->setToolTip(QStringLiteral("新しい注釈の色"));
    paint_swatch(note_colour_, note_colour());
    connect(note_colour_, &QToolButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(note_colour(), this, QStringLiteral("注釈のカラー"));
        if (!chosen.isValid()) return;
        settings()->setValue(QStringLiteral("notes/color"), chosen.name());
        paint_swatch(note_colour_, chosen);
    });
    row->addWidget(note_colour_);
    row->addStretch(1);
    layout->addLayout(row);
    auto* clear = new QPushButton(QStringLiteral("すべてを消去"));
    clear->setObjectName(QStringLiteral("note_clear"));
    clear->setToolTip(QStringLiteral("このページの注釈をすべて削除します（取り消せます）"));
    connect(clear, &QPushButton::clicked, this, [this] { clear_notes(); });
    layout->addWidget(clear);
    auto* panel = new QPushButton(QStringLiteral("注釈パネルの表示切り替え"));
    panel->setObjectName(QStringLiteral("note_panel"));
    connect(panel, &QPushButton::clicked, this, [this] { action(QStringLiteral("act_panel_notes"))->trigger(); });
    layout->addWidget(panel);
    layout->addStretch(1);
    if (tool_settings_ != nullptr) tool_settings_->add({QStringLiteral("note")}, page);
}

bool MainWindow::note_op(Json op) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    op["page"] = page->index.json();
    const bool done = apply_ops(Json::array({std::move(op)}));
    refresh_notes();
    return done;
}

void MainWindow::refresh_notes() {
    const core::Page* page = current_page();
    if (page == nullptr || std::none_of(page->annotations.begin(), page->annotations.end(),
                                        [&](const core::Annotation& a) { return QString::fromStdString(a.id) == canvas_->chosen_note; })) {
        canvas_->chosen_note.clear();
    }
    if (notes_panel_ != nullptr && notes_dock_ != nullptr && notes_dock_->isVisible()) {
        notes_panel_->choose(canvas_->chosen_note);
        notes_panel_->refresh();
    }
    canvas_->update();
}

void MainWindow::choose_note(const QString& id) {
    canvas_->chosen_note = id;
    canvas_->update();
    if (notes_panel_ != nullptr) notes_panel_->choose(id);
}

void MainWindow::add_note_at(double x, double y) {
    const core::Page* before = current_page();
    if (before == nullptr) return;
    const std::size_t had = before->annotations.size();
    const QColor colour = note_colour();
    Json op{{"op", "add_annotation"}, {"at_mm", Json::array({std::round(x * 1000) / 1000, std::round(y * 1000) / 1000})},
            {"color", Json::array({colour.red(), colour.green(), colour.blue()})}};
    const QString author = note_author_ != nullptr ? note_author_->text() : settings()->value(QStringLiteral("notes/author"), default_author()).toString();
    if (!author.isEmpty()) op["author"] = author.toStdString();
    if (!note_op(std::move(op))) return;
    const core::Page* page = current_page();
    if (page == nullptr || page->annotations.size() <= had) return;
    // (the new note shown in the 注釈 panel, its text to type: Photoshop's)
    if (notes_dock_ != nullptr && !notes_dock_->isVisible()) {
        notes_dock_->show();
        notes_dock_->raise();
    }
    choose_note(QString::fromStdString(page->annotations.back().id));
    if (notes_panel_ != nullptr) {
        notes_panel_->refresh();
        notes_panel_->text()->setFocus();
    }
}

void MainWindow::clear_notes() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (page->annotations.empty()) {
        flash(QStringLiteral("このページには注釈がありません"), 3000);
        return;
    }
    note_op(Json{{"op", "delete_annotations"}, {"all", true}});
}

void MainWindow::import_notes(const QString& given) {
    if (current_page() == nullptr) return;
    QString path = given;
    if (path.isEmpty()) {
        const Asked asked = asking();
        path = ask::open_path(this, QStringLiteral("注釈を読み込む"), QStringLiteral("注釈のあるファイル (*.psd *.psb *.pcraft)"));
        if (path.isEmpty() || !still(asked)) return;
    }
    const std::filesystem::path file = core::path_from_utf8(path.toStdString());
    std::vector<core::Annotation> notes;
    try {
        notes = off_ui([file] {
            const std::string bytes = storage::read_file_bounded(file, std::size_t{4} << 30);
            return formats::read_annotations(bytes);
        });
    } catch (const std::exception& error) {
        flash(QStringLiteral("注釈を読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    const core::Page* page = current_page();
    if (page == nullptr) return;
    Json ops;
    try {
        ops = formats::annotation_ops(notes, *page);
    } catch (const core::Error& error) {
        flash(QStringLiteral("注釈を読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    if (!apply_ops(ops)) return;
    refresh_notes();
    flash(QStringLiteral("「%1」の注釈を %2 個読み込みました").arg(QFileInfo(path).fileName()).arg(notes.size()), 5000);
}

}  // namespace genko::app
