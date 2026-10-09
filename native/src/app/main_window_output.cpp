// The book going out and what it went through (M4③b, Python's main.py): 書き出し… (_export with dialogs.ExportDialog),
// 印刷… (_print with printing.PrintDialog), 履歴… (the 履歴 panel, history.HistoryPanel), 入稿前の点検 (_run_checks with
// check_panel.CheckPanel, and show_issue: a problem shown on its page), スマホの画面の範囲を表示 (_toggle_phone,
// comfort.phone_default) and 目盛りを表示 (_toggle_scale; the scales and the guide lines pulled from them: the canvas's
// canvas_scale.cpp, the add_ruler of _place_ruler).

#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QSignalBlocker>

#include <algorithm>

#include "app/ask.hpp"
#include "app/check_panel.hpp"
#include "app/config.hpp"
#include "app/export_dialog.hpp"
#include "app/history_panel.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/print_dialog.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

using core::Json;

namespace {

// comfort.phone_default: on by choice; with no choice made, on for tall strips (a webtoon's pages).
bool phone_default(const core::Document& book) {
    const QVariant chosen = settings()->value(QStringLiteral("ui/phone_view"));
    if (chosen.isValid()) {
        const QString text = chosen.toString().toLower();
        return text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("yes");
    }
    if (book.pages.empty()) return false;
    const auto [w, h] = book.pages.front()->spec.trim_size();
    return h.value() >= 2.5 * w.value();
}

QDockWidget* occasional_dock(QMainWindow* window, const QString& title, QWidget* panel) {
    auto* dock = new QDockWidget(title, window);
    dock->setObjectName(title);
    auto* scroll = new QScrollArea;  // (a tall panel scrolls on a small screen instead of making the window taller)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(panel);
    dock->setWidget(scroll);
    dock->setMinimumWidth(200);
    // (for occasional work, as Python's OCCASIONAL_PANELS: its tab has a close button and it joins the row when opened)
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    return dock;
}

}  // namespace

void MainWindow::build_output_actions() {
    make("act_export", QStringLiteral("書き出し…"), [this] { export_book(); }, {QKeySequence(QStringLiteral("Ctrl+E"))},
         QStringLiteral("PDF・TIFF・PSD・縦読み・SNS 用などに書き出します"));
    make("act_print", QStringLiteral("印刷…"), [this] { print_book(); }, {QKeySequence(QStringLiteral("Ctrl+P"))},
         QStringLiteral("プリンターで紙に印刷します（仕上がりで切る・用紙全体）"));
    make("act_history", QStringLiteral("履歴…"), [this] { show_dock(QStringLiteral("履歴")); }, {QKeySequence(QStringLiteral("Ctrl+H"))},
         QStringLiteral("変更の一覧。クリックでその時点まで戻る・進む"));
    QAction* phone = make("act_phone", QStringLiteral("スマホの画面の範囲を表示"), [this] { toggle_phone(); }, {},
                          QStringLiteral("縦読みの原稿で、スマホ 1 画面に入る範囲と画面の切れ目"), true);
    QAction* scale = make("act_scale", QStringLiteral("目盛りを表示"), [] {}, {QKeySequence(QStringLiteral("Ctrl+R"))},
                          QStringLiteral("上と左に mm の目盛り。目盛りからドラッグするとガイド線を引けます"), true);
    connect(scale, &QAction::triggered, this, [this](bool on) {
        canvas_->show_scale = on;
        canvas_->update();
    });
    make("act_checks", QStringLiteral("入稿前の点検"), [this] { run_checks(); }, {QKeySequence(QStringLiteral("F9"))},
         QStringLiteral("はみ出し・文字の重なりや小ささ・解像度などを探します"));
    // (the phone screens: as chosen before, else on for a tall strip — Python decides it when the window is made)
    const QSignalBlocker quiet(phone);
    phone->setChecked(phone_default(book()));
    canvas_->phone_view = phone->isChecked();
}

void MainWindow::build_output_docks() {
    history_ = new HistoryPanel(this);
    history_dock_ = occasional_dock(this, QStringLiteral("履歴"), history_);
    addDockWidget(Qt::RightDockWidgetArea, history_dock_);
    tabifyDockWidget(pages_dock_, history_dock_);
    history_dock_->hide();
    connect(history_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) history_->refresh();
    });
    view_menu_->addAction(history_dock_->toggleViewAction());
    history_timer_.setSingleShot(true);
    history_timer_.setInterval(250);  // (as Python's panels follow a change: a moment later)
    connect(&history_timer_, &QTimer::timeout, this, [this] {
        if (history_dock_->isVisible()) history_->refresh();
    });
    checks_ = new CheckPanel(this);
    checks_dock_ = occasional_dock(this, QStringLiteral("点検"), checks_);
    addDockWidget(Qt::RightDockWidgetArea, checks_dock_);
    tabifyDockWidget(pages_dock_, checks_dock_);
    checks_dock_->hide();
    view_menu_->addAction(checks_dock_->toggleViewAction());
}

// After a change to the book (or its save): the history follows (when it is shown), the checks are out of date.
void MainWindow::output_panels_follow() {
    if (checks_ != nullptr) checks_->refresh();
    if (history_dock_ != nullptr && history_dock_->isVisible()) history_timer_.start();
}

void MainWindow::run_checks() {
    show_dock(QStringLiteral("点検"));
    const std::optional<Json> report = checks_->run();
    if (!report) {
        flash(checks_->summary->text(), 6000, true);
        return;
    }
    if ((*report)["issues"].empty()) {
        flash(QStringLiteral("直すところは見つかりませんでした"), 4000);
    } else {
        flash(QStringLiteral("止まる問題 %1 件・確かめた方がよいこと %2 件（点検パネル）")
                  .arg((*report)["errors"].get<std::int64_t>())
                  .arg((*report)["warnings"].get<std::int64_t>()),
              4000);
    }
}

void MainWindow::show_issue(const Json& issue) {
    if (const Json* page = issue.is_object() && issue.contains("page") ? &issue["page"] : nullptr; page != nullptr && core::py_truthy(*page)) {
        go_to_page(static_cast<int>(core::py_int(*page)));
    }
    canvas_->highlight_box.reset();
    if (issue.contains("box") && issue["box"].is_array() && issue["box"].size() == 4) {
        std::array<double, 4> box{};
        for (std::size_t i = 0; i < 4; ++i) box[i] = core::py_float(issue["box"][i]);
        canvas_->highlight_box = box;
    }
    const Json target = issue.contains("target") && issue["target"].is_object() ? issue["target"] : Json::object();
    const std::string kind = target.contains("kind") && target["kind"].is_string() ? target["kind"].get<std::string>() : std::string();
    const bool named = target.contains("id") && core::py_truthy(target["id"]) && target["id"].is_string();
    if (kind == "line" && named) {
        canvas_->selected_line_id = target["id"].get<std::string>();
    } else if (kind == "layer" && named) {
        const std::string id = target["id"].get<std::string>();
        const core::Page* page = current_page();
        if (page != nullptr && std::any_of(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == id; })) {
            target_layer_id_ = id;
            pen_changed();
            if (layer_panel_ != nullptr) layer_panel_->show_target();
        }
    }
    canvas_->update();
    flash(QString::fromStdString(issue.contains("message") ? core::py_str(issue["message"]) : std::string()), 5000);
}

void MainWindow::export_book() {
    session_->save_now();  // (Python's commit_now: what was done is written first)
    const core::Page* page = current_page();
    ExportDialog dialog(this, session_->snapshot(), session_->path(), session_->actor(), false,
                        page != nullptr ? core::py_int(page->index) : 1);
    ask::exec(&dialog);
    if (dialog.fix_requested) run_checks();
    session_->check_outside();  // (an official export may have recorded an approval: Python's sync)
    reload_pages();
}

void MainWindow::print_book() {
    session_->save_now();  // (Python's commit_now)
    PrintDialog dialog(this);
    ask::exec(&dialog);
}

void MainWindow::toggle_phone() {
    canvas_->phone_view = action("act_phone")->isChecked();
    settings()->setValue(QStringLiteral("ui/phone_view"), canvas_->phone_view ? QStringLiteral("true") : QStringLiteral("false"));
    canvas_->update();
}

}  // namespace genko::app
