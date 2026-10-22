#include "app/tool_settings.hpp"
#include "app/main_window.hpp"
#include "app/timeline.hpp"
#include "app/guide_panel.hpp"
#include "app/material_tabs.hpp"
#include "app/layer_panel.hpp"

#include <QCheckBox>
#include <QSignalBlocker>
#include <QApplication>
#include <QPointer>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QLayout>
#include <QPushButton>
#include <QMenu>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>

#include "app/actions.hpp"
#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/backup.hpp"
#include "app/comfort.hpp"
#include "app/config.hpp"
#include "app/preferences.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/ime.hpp"
#include "app/inputs.hpp"
#include "app/look.hpp"
#include "app/memory.hpp"
#include "app/navigator.hpp"
#include "app/pages_panel.hpp"
#include "app/paths_panel.hpp"
#include "app/perf.hpp"
#include "app/reply_box.hpp"
#include "app/save_status.hpp"
#include "app/studio_widgets.hpp"
#include "app/icons.hpp"
#include "app/theme.hpp"
#include "app/workspace.hpp"
#include "app/colours.hpp"
#include "service/wording.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/brushes.hpp"
#include "render/placed.hpp"
#include "render/timelapse.hpp"
#include "render/ops_registry.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

// A book opened in a window: its first page read and shown first, the other pages read meanwhile (SPEC PERF-01).
Session::Options opening_options() {
    Session::Options options;
    options.defer_pages = true;
    options.idle = std::min(options.longest, std::chrono::milliseconds(preferences::save_after_ms()));  // (環境設定)
    return options;
}

std::vector<MainWindow*>& open_windows() {
    static std::vector<MainWindow*> list;
    return list;
}

}  // namespace

// --- documents (several books, one book in several windows) ------------------------------------------------------

namespace documents {

void add(MainWindow* window) {
    auto& list = open_windows();
    if (std::find(list.begin(), list.end(), window) == list.end()) list.push_back(window);
}

void remove(MainWindow* window) { std::erase(open_windows(), window); }

std::vector<MainWindow*> windows() {
    std::vector<MainWindow*> out;
    for (MainWindow* w : open_windows()) {
        if (!w->closed()) out.push_back(w);
    }
    return out;
}

std::pair<MainWindow*, int> find(const fs::path& path) {
    std::error_code ec;
    const fs::path wanted = fs::weakly_canonical(fs::absolute(path, ec), ec);
    for (MainWindow* window : windows()) {
        for (int i = 0; i < static_cast<int>(window->documents().size()); ++i) {
            const auto& at = window->documents()[static_cast<std::size_t>(i)].session->path();
            if (at && fs::weakly_canonical(*at, ec) == wanted) return {window, i};
        }
    }
    return {nullptr, -1};
}

bool open_elsewhere(const Session* session, const MainWindow* but) {
    for (MainWindow* window : windows()) {
        if (window == but) continue;
        for (const Document& doc : window->documents()) {
            if (doc.session.get() == session) return true;
        }
    }
    return false;
}

}  // namespace documents

QString Document::title() const {
    if (contents) return QStringLiteral("%1の中身").arg(contents_title.isEmpty() ? QStringLiteral("スマートオブジェクト") : contents_title);  // (PC3 A1)
    const core::Document& book = session->document();
    return QStringLiteral("%1 第%2話").arg(book.title.empty() ? QStringLiteral("無題") : QString::fromStdString(book.title),
                                         QString::fromStdString(book.episode.repr()));
}

// --- the window ------------------------------------------------------------------------------------------------------

MainWindow::MainWindow(std::shared_ptr<Session> session) {
    setWindowTitle(QStringLiteral("Genko Studio"));
    look::fit_to_screen(this, QSize(1280, 800), false);  // (a small screen, or a large display scaling: within the screen)
    session_ = session ? std::move(session)
                       : std::make_shared<Session>(core::new_episode("無題", core::Num(1), 8, core::PageSpec::a4_mono()), std::nullopt);
    documents_.push_back(Document{session_});
    connect(session_.get(), &Session::statusChanged, this, &MainWindow::maybe_backup);  // (each book's saves: バックアップ)
    documents::add(this);
    inputs::install();  // (number fields select their value; the wheel leaves unused fields alone)
    memory::watch_idle();  // (the heap's free room back to the system after 10 s without input: PERF-G)
    pen_ = PenSettings::load();  // (until the brush panel is made: it drives the pen from then on)
    // the book's own brushes are known to the drawing (as Python's reader registers them)
    render::brushes::register_book(book());  // (and the pictures of their papers)

    canvas_ = new PageCanvas;
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        // (Python's 300 × 280 at least for the page; less on a small screen or a large display scaling — 1024 × 640 at
        // 200 % is 512 × 320 — so the window with its bars and panels (the tool palette, the tabs on both sides) stays
        // within the screen: SPEC UX-02)
        const QRect room = screen->availableGeometry();
        canvas_->setMinimumSize(std::clamp(room.width() - 420, 100, 300), std::clamp(room.height() - 260, 110, 280));
    }
    pages_ = new PageList;
    pages_->setMinimumWidth(120);
    connect(pages_, &QListWidget::currentRowChanged, this, &MainWindow::select_page);
    connect(pages_, &PageList::reorderRequested, this, [this](const std::vector<int>& order, int moving) { reorder_pages(order, moving); });
    connect(pages_, &PageList::addAfterRequested, this, [this](int index) {
        if (apply_ops(Json::array({Json::object({{"op", "add_page"}, {"count", 1}, {"after", index}})}))) {
            page_index_ = index;  // (the new page)
            reload_pages();
        }
    });
    connect(pages_, &PageList::duplicateRequested, this, &MainWindow::duplicate_page);
    connect(pages_, &PageList::deleteRequested, this, [this](int index) {
        apply_ops(Json::array({Json::object({{"op", "delete_page"}, {"page", index}})}));
    });
    // (the menu's choices wait for the menu and its question: they go to the page by its id in the book in front then,
    // when it is still the one the menu was opened on — main_window_actions' del_page asks the same of its question)
    pages_->set_book_check([this] {
        const Asked asked = asking();
        return std::function<DocPtr()>([this, asked]() -> DocPtr { return session_ == asked.session ? session_->snapshot() : DocPtr(); });
    });
    connect(pages_, &PageList::targetLost, this, [this] {
        flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
    });

    connect(canvas_, &PageCanvas::changed, this, [this] { refresh_status(); });
    connect(canvas_, &PageCanvas::zoomChanged, this, [this](double) { refresh_zoom(); });
    connect(canvas_, &PageCanvas::strokeCommitted, this, &MainWindow::on_stroke);
    connect(canvas_, &PageCanvas::frameSelected, this, &MainWindow::on_frame_selected);
    connect(canvas_, &PageCanvas::contextMenuAt, this, &MainWindow::context_menu);
    connect(canvas_, &PageCanvas::cutRequested, this, &MainWindow::cut_frame);
    connect(canvas_, &PageCanvas::gutterMoved, this, [this](const QString& node, int index, double delta) {
        if (const core::Page* page = current_page()) {
            apply_ops(Json::array({Json::object({{"op", "move_gutter"}, {"page", page->index.json()}, {"frame_id", node.toStdString()},
                                                 {"index", index}, {"delta_mm", delta}})}));
        }
    });
    connect(canvas_, &PageCanvas::frameShaped, this, [this](const QString& frame_id, const QVector<QPointF>& poly) {
        const core::Page* page = current_page();
        if (page == nullptr) return;
        Json points = Json::array();
        for (const QPointF& p : poly) points.push_back(Json::array({p.x(), p.y()}));
        apply_ops(Json::array({Json::object({{"op", "set_frame"}, {"page", page->index.json()}, {"frame_id", frame_id.toStdString()}, {"poly", points}})}));
    });
    connect(canvas_, &PageCanvas::frameBowed, this, [this](const QString& frame_id, int edge, double mm) {
        const core::Page* page = current_page();
        if (page == nullptr) return;
        apply_ops(Json::array({Json::object({{"op", "set_frame"},
                                             {"page", page->index.json()},
                                             {"frame_id", frame_id.toStdString()},
                                             {"bow", Json::object({{"edge", edge}, {"mm", mm}})}})}));
    });
    connect(canvas_, &PageCanvas::frameDrawn, this, &MainWindow::frame_drawn);
    connect(canvas_, &PageCanvas::layerMoveStarted, this, &MainWindow::layer_move_started);
    connect(canvas_, &PageCanvas::layerMoved, this, &MainWindow::layer_moved);
    connect(canvas_, &PageCanvas::omittedChanged, this, &MainWindow::show_omitted);
    connect(canvas_, &PageCanvas::renderFailed, this, [this](const QString& message) {
        flash(QStringLiteral("ページの一部を描けませんでした: %1").arg(wording::error(message)), 6000, true);
    });
    connect(canvas_, &PageCanvas::toolHeld, this, [this](const QString& tool) {
        const QString name = tool == QLatin1String("marquee") ? marquee_tool_of(canvas_->marquee) : tool;
        if (const auto it = tool_actions_.find(name); it != tool_actions_.end()) it->second->setChecked(true);
        if (tool_settings_ != nullptr) {  // (its settings show the tool in use, for the moment the key is held)
            tool_settings_->show_tool(tool);
            if (canvas_->tool_held()) tool_settings_->title()->setText(tool_settings_->title()->text() + QStringLiteral("（キーを押している間）"));
        }
    });
    // the selection (main_window_select.cpp)
    connect(canvas_, &PageCanvas::selectionDrawn, this, &MainWindow::selection_drawn);
    connect(canvas_, &PageCanvas::selectionPainted, this, &MainWindow::selection_painted);
    connect(canvas_, &PageCanvas::wandRequested, this, &MainWindow::wand);
    connect(canvas_, &PageCanvas::magicEraseRequested, this, &MainWindow::magic_erase);  // (PC3 B1: マジック消しゴムツール)
    connect(canvas_, &PageCanvas::selectionTransformed, this, &MainWindow::transform_selection);
    connect(canvas_, &PageCanvas::selectionWarped, this, &MainWindow::warp_selection);
    // the drawing tools (main_window_paint.cpp)
    canvas_->layer_colour_at = [this](double x_mm, double y_mm) { return layer_colour_at(x_mm, y_mm); };
    connect(canvas_, &PageCanvas::colourPicked, this, &MainWindow::colour_picked);
    connect(canvas_, &PageCanvas::fillRequested, this, &MainWindow::fill_at);
    connect(canvas_, &PageCanvas::areaFilled, this, &MainWindow::lasso_filled);
    connect(canvas_, &PageCanvas::shapeDrawn, this, &MainWindow::shape_drawn);
    connect(canvas_, &PageCanvas::gradientRequested, this, &MainWindow::gradient);
    // editing lines (main_window_vector.cpp): the lines of the layer drawn on
    canvas_->drawing_layer = [this] { return target_layer(); };
    connect(canvas_, &PageCanvas::vectorEdited, this, &MainWindow::vector_edit);
    connect(canvas_, &PageCanvas::vectorTraced, this, &MainWindow::vector_traced);
    connect(canvas_, &PageCanvas::strokeReshaped, this, &MainWindow::reshaped);
    // rulers and 3D (main_window_guides.cpp)
    connect(canvas_, &PageCanvas::rulerPlaced, this, &MainWindow::place_ruler);
    connect(canvas_, &PageCanvas::rulerEdited, this, [this](const QString& id, const Json& change) {
        if (const core::Page* page = current_page()) {
            Json op{{"op", "edit_ruler"}, {"page", page->index.json()}, {"id", id.toStdString()}};
            for (const auto& [key, value] : change.items()) op[key] = value;
            apply_ops(Json::array({op}));
        }
    });
    connect(canvas_, &PageCanvas::primSelected, this, [this](const QString& id) {
        if (guides_ != nullptr) guides_->select_prim(id.toStdString());
    });
    connect(canvas_, &PageCanvas::primEdited, this, [this](const QString& id, const Json& change) {
        if (const core::Page* page = current_page()) {
            Json op{{"op", "edit_prim"}, {"page", page->index.json()}, {"id", id.toStdString()}};
            for (const auto& [key, value] : change.items()) op[key] = value;
            apply_ops(Json::array({op}));
        }
    });
    connect(canvas_, &PageCanvas::primPosed, this, &MainWindow::prim_posed);
    // effect lines and materials (main_window_materials.cpp)
    connect(canvas_, &PageCanvas::effectRequested, this, &MainWindow::effect_at);
    connect(canvas_, &PageCanvas::effectSelected, this, [this](const QString& id) {
        if (materials_ != nullptr) materials_->select_effect(id.toStdString());
    });
    connect(canvas_, &PageCanvas::effectMoved, this, [this](const QString& id, const QPointF& centre) {
        if (const core::Page* page = current_page()) {
            apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", page->index.json()}, {"id", id.toStdString()},
                                        {"params", Json{{"center", Json::array({centre.x(), centre.y()})}}}}}));
        }
    });
    connect(canvas_, &PageCanvas::stampRequested, this, &MainWindow::stamp_at);

    // the page gets the room; above it, a tab for each open book (in the command bar)
    doc_tabs_ = new QTabBar;
    doc_tabs_->setTabsClosable(true);
    doc_tabs_->setMovable(true);
    doc_tabs_->setDocumentMode(true);
    doc_tabs_->setExpanding(false);
    doc_tabs_->setElideMode(Qt::ElideRight);
    doc_tabs_->addTab(documents_[0].title());
    connect(doc_tabs_, &QTabBar::currentChanged, this, &MainWindow::switch_document);
    connect(doc_tabs_, &QTabBar::tabCloseRequested, this, [this](int index) { close_document(index); });
    connect(doc_tabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        Document moved = documents_[static_cast<std::size_t>(from)];
        documents_.erase(documents_.begin() + from);
        documents_.insert(documents_.begin() + to, moved);
        doc_ = doc_tabs_->currentIndex();
    });

    auto* central = new QWidget;
    auto* column = new QVBoxLayout(central);
    column_ = column;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    unported_ = new QLabel;
    unported_->setObjectName(QStringLiteral("unportedBand"));
    unported_->setWordWrap(true);
    unported_->hide();
    read_only_ = new QLabel;
    read_only_->setObjectName(QStringLiteral("unportedBand"));
    read_only_->setWordWrap(true);
    read_only_->hide();
    failure_bar_ = new SaveFailureBar;
    connect(failure_bar_, &SaveFailureBar::retry, this, [this] {
        session_->save_now();
        flash(QStringLiteral("もう一度保存しています…"), 2000);
    });
    connect(failure_bar_, &SaveFailureBar::saveAs, this, &MainWindow::save_as);
    connect(failure_bar_, &SaveFailureBar::recoveryCopy, this, [this] {
        session_->write_recovery_copy();
        flash(QStringLiteral("復旧用のコピーを書いています（設定フォルダーの recovery）"), 3000);
    });
    column->addWidget(unported_);
    column->addWidget(read_only_);
    column->addWidget(failure_bar_);
    column->addWidget(canvas_, 1);
    setCentralWidget(central);
    ime_ = new ImeEntry(canvas_);  // (the entrance for typed words; the text tool types in its InlineEditor, ime.hpp)
    ime_->hide();

    status_ = new QLabel;
    status_->setMinimumWidth(10);
    statusBar()->addWidget(status_, 1);
    save_label_ = new SaveStatusLabel;
    statusBar()->addPermanentWidget(save_label_);
    build_studio_bar();  // (AI と作る and the process bar, before the zoom: main_window_studio.cpp)
    zoom_label_ = new QLabel;
    statusBar()->addPermanentWidget(zoom_label_);
    zoom_box_ = new QSpinBox;  // (表示倍率の直接入力)
    zoom_box_->setRange(5, 6400);
    zoom_box_->setSuffix(QStringLiteral(" %"));
    zoom_box_->setToolTip(QStringLiteral("表示倍率。数を打ち込んで Enter"));
    zoom_box_->setKeyboardTracking(false);
    connect(zoom_box_, &QSpinBox::valueChanged, this, [this](int value) {
        if (value != canvas_->zoom_percent()) canvas_->set_zoom_percent(value);
    });
    statusBar()->addPermanentWidget(zoom_box_);

    build_actions();
    build_menus();
    build_toolbars();
    build_docks();
    flash_timer_.setSingleShot(true);
    connect(&flash_timer_, &QTimer::timeout, this, &MainWindow::refresh_status);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) { on_disk_change(); });
    connect(&link_watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) { check_links(); });  // (smart-objects@1)
    connect_session();
    watch();
    reload_pages();
    pages_->setCurrentRow(0);
    known_requests_ = request_ids();
    studio_follow(true);
    refresh_ai();
    // comfort (comfort.cpp): 原稿だけを表示 — Tab on the page only (elsewhere Tab moves between fields) — at the top of
    // the window menu, and the rest reminder
    canvas_only_ = new CanvasOnly(this);
    QAction* only = make("act_canvas_only", QStringLiteral("原稿だけを表示"), [this] { canvas_only_->toggle(); }, {QKeySequence(Qt::Key_Tab)},
                         QStringLiteral("パネルとバーを隠して原稿だけにします（Tab で戻る）。左右の端にカーソルを寄せるとその側のパネルが出ます"));
    removeAction(only);
    only->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    canvas_->addAction(only);
    view_menu_->insertAction(view_menu_->actions().isEmpty() ? nullptr : view_menu_->actions().front(), only);
    rest_ = new RestReminder(this);
    // the person's own keys and ways (環境設定): every command named by its words, its keys as chosen
    preferences::name_commands(this);
    preferences::apply_all(this);
    if (gpu_available()) canvas_->use_gpu(true);  // (the page drawn by the graphics card, when chosen and real)
    theme::name_buttons(this);
    // (tool_settings.fit_narrow, as Python's window does to the side panels and the tool settings: each shrinks to the
    // side's width instead of being cut off at its edge)
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        if (dock == tool_settings_dock_ || dockWidgetArea(dock) == Qt::RightDockWidgetArea) fit_narrow(dock->widget());
    }
    // (within the screen again: the panels laid out while they were made, their title bars not yet taken away, may
    // have made it larger than the room given at the start — a small screen, a large display scaling: SPEC UX-02)
    layout()->activate();
    look::fit_to_screen(this, QSize(1280, 800), false);
    perf::watch_loop(this);
}

void MainWindow::set_save_after(int ms) {
    save_after_ms_ = ms;
    for (const Document& document : documents_) {
        if (document.session) document.session->set_idle(std::chrono::milliseconds(ms));
    }
}

void MainWindow::refresh_icons() {
    theme::refresh_icons();
    if (quick_access_ != nullptr) quick_access_->refresh();
    if (layer_panel_ != nullptr) layer_panel_->refresh_look();
}

RadialMenu* MainWindow::radial() const { return radial_.data(); }

void MainWindow::maybe_backup() {
    // バックアップ (Python's _backup): after a save, the book zipped into the folder chosen in 環境設定, not more often
    // than set — each book of the window, its tab in front or not
    for (const Document& document : documents_) {
        if (document.session) back_up(document.session);
    }
}

void MainWindow::back_up(const std::shared_ptr<Session>& session) {
    const SaveStatus status = session->status();
    if (!session->path() || status.kind != SaveKind::Saved || !status.last_saved.isValid()) return;
    const std::string key = core::path_to_utf8(*session->path());
    if (const auto seen = backup_seen_.find(key); (seen != backup_seen_.end() && seen->second == status.last_revision) || backups_running_.contains(key)) {
        return;
    }
    const auto store = settings();
    const QString folder = store->value(QStringLiteral("backup/folder"), QString()).toString();
    if (folder.isEmpty()) return;
    backup_seen_[key] = status.last_revision;
    const double minutes = std::max(0.0, store->value(QStringLiteral("backup/minutes"), 30).toDouble());
    const int keep = store->value(QStringLiteral("backup/keep"), 10).toInt();
    const fs::path project = *session->path();
    const fs::path into = core::path_from_utf8(folder.toStdString());
    backups_running_.insert(key);
    QPointer<MainWindow> self(this);
    // (on the book's own worker, between its saves: never zipped halfway through one; the page never waits for it)
    session->beside_saves(
        [project, into, minutes, keep]() -> QString {
            try {
                if (backup::due(into, QString::fromStdString(core::path_to_utf8(project.stem())), minutes)) backup::make(project, into, keep);
                return {};
            } catch (const std::exception& error) {
                return QString::fromUtf8(error.what());
            }
        },
        [self, key](const QString& problem) {
            if (!self) return;
            self->backups_running_.erase(key);
            if (!problem.isEmpty()) self->flash(QStringLiteral("バックアップを残せませんでした: %1").arg(wording::error(problem)), 6000, true);
        });
}

MainWindow::~MainWindow() {
    cancel_smart_select();  // (PC3 B3: a smart selection still worked out on its worker stops — not waited for to its end)
    stop_filter_work();     // (PC4 節 D: a heavy filter's preview on its worker stops, as the smart selection)
    disconnect_session();
    documents::remove(this);
    render::clear_placed_caches();  // (a window closed: its books' placed pictures are let go)
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    inputs::watch(this);  // (its number fields and lists take input the app's way)
    canvas_->setFocus();
    if (!settled_) {
        settled_ = true;
        settle_docks();
        // (a tab brought to the front fades in — only once the window is up, not while it is laid out)
        for (QDockWidget* dock : findChildren<QDockWidget*>()) {
            connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool shown) {
                if (shown && isVisible() && !dock->isFloating()) fade_in(dock->widget(), 140);
            });
        }
        default_state_ = saveState();  // (ウィンドウ → ワークスペース → はじめの配置に戻す)
        quick_access_->refresh();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, &MainWindow::fit_tools);  // (after the palette has its new height)
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && !closed_) check_links();  // (smart-objects@1, PC3 A1)
}

void MainWindow::settle_docks() {
    fit_box();
    if (tool_settings_dock_ != nullptr && navigator_dock_ != nullptr) {
        resizeDocks({tool_settings_dock_, navigator_dock_}, {std::max(300, height() - 330), 170}, Qt::Vertical);
    }
    // the row of tabs whole, without arrows, from 1366 px up; under that the page keeps the room
    constexpr int kSide = 230;
    const int wide = width() >= 1360 ? std::max(kSide, std::min(320, width() / 5)) : kSide;
    if (layers_dock_ != nullptr && !layers_dock_->isFloating()) resizeDocks({layers_dock_}, {wide}, Qt::Horizontal);
    if (layers_dock_ != nullptr && !fronted_) {
        fronted_ = true;
        layers_dock_->raise();
    }
}

void MainWindow::fit_tools() {
    if (palette_ == nullptr || palette_->toolButtonStyle() != Qt::ToolButtonIconOnly) return;
    QList<QAction*> tools;
    for (QAction* act : palette_->actions()) {
        if (!act->isSeparator() && act->isVisible()) tools << act;
    }
    if (tools.isEmpty()) return;
    for (int size = 24; size >= 16; --size) {  // (the largest pictures that let the last tool show)
        palette_->setIconSize(QSize(size, size));
        palette_->layout()->activate();
        QWidget* last = palette_->widgetForAction(tools.back());
        if (last != nullptr && last->isVisibleTo(palette_)) return;
    }
}

const core::Page* MainWindow::current_page() const {
    const core::Document& b = book();
    if (contents_) {  // (smart-objects@1, PC3 A1: コンテンツを編集's tab — the contents' page)
        const auto found = b.smart_contents.find(*contents_);
        return found != b.smart_contents.end() && found->second && found->second->page ? found->second->page.get() : nullptr;
    }
    if (b.pages.empty()) return nullptr;
    return b.pages[static_cast<std::size_t>(std::min<int>(page_index_, static_cast<int>(b.pages.size()) - 1))].get();
}

bool MainWindow::agent_book() const { return book().strict_gates || core::py_truthy(book().studio); }

bool MainWindow::drawable(const core::Layer& layer) {
    return (layer.kind == core::LayerKind::Strokes || layer.kind == core::LayerKind::Raster || layer.kind == core::LayerKind::Tone) &&
           !layer.locked;
}

const core::Layer* MainWindow::target_layer() const {
    const core::Page* page = current_page();
    if (page == nullptr) return nullptr;
    if (target_layer_id_) {
        for (const core::Layer& layer : page->layers) {
            if (layer.id == *target_layer_id_) return &layer;
        }
    }
    // a person drawing alone starts on the ink (it prints); a book made with agents starts with the name
    const core::LayerRole role = page->stage == "name" && agent_book() ? core::LayerRole::Name : core::LayerRole::Ink;
    for (const core::Layer& layer : page->layers) {
        if (layer.role == role) return &layer;
    }
    return nullptr;
}

void MainWindow::set_target_layer(const std::string& layer_id) {
    target_layer_id_ = layer_id;
    pen_changed();
    if (const core::Layer* layer = target_layer()) {
        const bool tone = layer->kind == core::LayerKind::Tone;
        flash(QStringLiteral("描く先: %1").arg(wording::layer_label(*layer)) + (tone ? QStringLiteral("（ペンでトーンを足す・消しゴムで削る）") : QString()), 2500);
    }
    if (layer_panel_ != nullptr) layer_panel_->show_target();  // (not built again: a Ctrl / Shift+click keeps the others)
    if (materials_ != nullptr) materials_->refresh();
}

std::optional<core::Json> MainWindow::selection_area() const {
    const auto& selection = canvas_->selection();
    if (!selection) return std::nullopt;
    return selection->area;
}

void MainWindow::preview_ops(const std::optional<core::Json>& ops) {
    if (!ops) {
        show_page();
        return;
    }
    try {
        const core::CommandBus bus(render::ops_registry());
        // (channels@1: what is previewed is what is applied: the pixel ops on the channel targeted)
        auto result = bus.apply(*session_->snapshot(), with_contents_target(with_channel_target(*ops)), core::Actor(session_->actor()));
        show_preview(std::move(result.doc));
    } catch (const core::Error&) {
        show_page();  // (a preview that cannot be made: the page as it is)
    }
}

void MainWindow::show_preview(std::optional<core::Document> doc) {
    if (!doc) {
        show_page();
        return;
    }
    if (contents_) {  // (smart-objects@1: the contents' page as it would be)
        const auto found = doc->smart_contents.find(*contents_);
        if (found == doc->smart_contents.end() || !found->second || !found->second->page) {
            show_page();
            return;
        }
        const core::PagePtr page = found->second->page;
        doc->pages = {page};
        doc->deferred.clear();
        doc->story.clear();
        canvas_->set_page(std::make_shared<const core::Document>(std::move(*doc)), 0);
        return;
    }
    canvas_->set_page(std::make_shared<const core::Document>(std::move(*doc)), static_cast<std::size_t>(page_index_));
}

void MainWindow::pen_changed() {
    const core::Layer* layer = target_layer();
    canvas_->set_live_pen(pen_.stroke_fields(), layer != nullptr && drawable(*layer) ? std::optional<std::string>(layer->id) : std::nullopt);
    canvas_->brush_width_mm = pen_.width_mm;
    canvas_->eraser_mm = eraser_mm_;
}

// --- the session ---------------------------------------------------------------------------------------------------

void MainWindow::connect_session() {
    disconnect_session();
    Session* s = session_.get();
    drafts_session_ = session_;  // (the key its reply drafts are kept under now: on_book_changed follows a new one)
    drafts_key_ = draft_book_key(*this);
    session_links_.push_back(connect(s, &Session::changed, this, &MainWindow::on_book_changed));
    session_links_.push_back(connect(s, &Session::statusChanged, this, &MainWindow::refresh_status));
    // (a change saved: its time in 履歴)
    session_links_.push_back(connect(s, &Session::statusChanged, this, [this] { history_follows(); }));
    session_links_.push_back(connect(s, &Session::conflicts, this, [this](const QStringList& messages) {
        QStringList lines;
        for (int i = 0; i < messages.size() && i < 8; ++i) lines << QStringLiteral("・%1").arg(wording::error(messages[i]));
        flash(QStringLiteral("AI の変更と重なったため、次の操作は入りませんでした:\n") + lines.join(QLatin1Char('\n')), 6000);
    }));
    session_links_.push_back(connect(s, &Session::notice, this, [this](const QString& message, bool error) {
        flash(wording::error(message), 6000, error);
    }));
}

void MainWindow::disconnect_session() {
    for (const auto& link : session_links_) disconnect(link);
    session_links_.clear();
}

bool MainWindow::apply_ops(const Json& ops, const std::vector<std::string>& ids) {
    // (a change that takes a while says so, with the busy cursor, while it is made: Python's SLOW_OPS)
    static const std::map<std::string, QString> kSlow = {
        {"trace_prims", QStringLiteral("3D を線にしています…")},          {"add_cover", QStringLiteral("表紙を作っています…")},
        {"add_scene", QStringLiteral("3D の背景を置いています…")},        {"add_prim3d", QStringLiteral("3D を置いています…")},
        {"pose_figure", QStringLiteral("3D のポーズを変えています…")},    {"pose_mannequin", QStringLiteral("3D のポーズを変えています…")},
        {"import_pages", QStringLiteral("ページを取り込んでいます…")}};
    struct Busy {
        bool on = false;
        ~Busy() {
            if (on) QGuiApplication::restoreOverrideCursor();
        }
    } busy;
    for (std::size_t i = 0; ops.is_array() && i < ops.size(); ++i) {
        const Json& op = ops[i];
        const auto name = op.is_object() ? op.find("op") : op.end();
        const auto slow = name != op.end() && name->is_string() ? kSlow.find(name->get<std::string>()) : kSlow.end();
        if (slow == kSlow.end()) continue;
        status_->setText(QStringLiteral("<b>%1</b>").arg(slow->second));
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        busy.on = true;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);  // (shown before the work; no input taken meanwhile)
        break;
    }
    QElapsedTimer clock;
    clock.start();
    try {
        // (channels@1: the pixel ops on the channel targeted; smart-objects@1: in a contents' tab, on its contents)
        session_->apply(with_contents_target(with_channel_target(ops)), ids);
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);  // (the window and the book stay as they were)
        return false;
    }
    perf::event("applied", {{"ms", clock.nsecsElapsed() / 1e6}, {"op", ops.size() > 0 && ops[0].contains("op") ? ops[0]["op"] : Json()}});
    if (recording_) {  // (オートアクション: what was done goes into the recording)
        for (Json& op : actions::recordable(ops)) recording_->push_back(std::move(op));
    }
    return true;
}

bool MainWindow::apply_worked(const Json& ops, const Json& targeted, const DocPtr& base, core::ApplyResult result,
                              const std::vector<std::string>& taken) {
    QElapsedTimer clock;
    clock.start();
    try {
        session_->apply_worked(targeted, base, std::move(result), taken);
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);  // (the window and the book stay as they were)
        return false;
    }
    perf::event("applied", {{"ms", clock.nsecsElapsed() / 1e6}, {"op", ops.size() > 0 && ops[0].contains("op") ? ops[0]["op"] : Json()}});
    if (recording_) {  // (オートアクション: what was done goes into the recording)
        for (Json& op : actions::recordable(ops)) recording_->push_back(std::move(op));
    }
    return true;
}

void MainWindow::on_book_changed(const BookChange& change) {
    if (closed_) return;
    const core::Document& b = book();
    if (page_index_ >= static_cast<int>(b.pages.size())) page_index_ = std::max(0, static_cast<int>(b.pages.size()) - 1);
    if (change.why == BookChange::Why::Undo || change.why == BookChange::Why::Redo) perf::event("undo_shown_model");
    if (pages_->count() != static_cast<int>(b.pages.size())) {
        reload_pages();
    } else {
        pages_->fill(session_->snapshot(), page_index_);
        lines_later_ = change.why == BookChange::Why::Edit;  // (an edit on this page: the lines panel follows a moment later)
        show_page();
        lines_later_ = false;
    }
    if (doc_ < doc_tabs_->count()) doc_tabs_->setTabText(doc_, documents_[static_cast<std::size_t>(doc_)].title());
    render::brushes::follow_book(b);  // (a define_brush: the person's own brushes and their edits stay)
    output_panels_follow();  // (履歴 and 点検: main_window_output.cpp)
    adjust_forms::follow_change(*this, change);  // (PC2c: 編集 → フェード, app/image_dialogs.cpp)
    // (the studio: someone else's changes may bring requests for the person — a note, and the box in front)
    const bool outside = change.why == BookChange::Why::Rebase || change.why == BookChange::Why::Reload;
    const std::set<std::string> before = known_requests_;
    // (the book saved in another place — Save As, a new book's first save —: its reply drafts go on there, before the
    // boxes show the book under its new key)
    const QString drafts_key = draft_book_key(*this);
    if (drafts_session_.lock() == session_ && !drafts_key_.isEmpty() && drafts_key_ != drafts_key) Drafts::instance().carry(drafts_key_, drafts_key);
    drafts_session_ = session_;
    drafts_key_ = drafts_key;
    studio_follow(change.why != BookChange::Why::Edit);
    if (outside) {
        notice_requests(before);
        refresh_ai();
    }
    check_links();  // (smart-objects@1: a link made, changed or gone)
}

void MainWindow::reload_pages() {
    const core::Document& b = book();
    page_index_ = std::min(page_index_, std::max(0, static_cast<int>(b.pages.size()) - 1));
    pages_->fill(session_->snapshot(), page_index_);
    show_page();
}

void MainWindow::show_page() {
    const core::Page* page = current_page();
    if (page == nullptr) {
        canvas_->clear_page();
        refresh_status();
        if (contents_) flash(QStringLiteral("このタブのスマートオブジェクトの中身は、原稿にもうありません（取り消しで戻せます）"), 6000);
        if (layer_panel_ != nullptr) layer_panel_->refresh();
        return;
    }
    if (target_layer_id_ && std::none_of(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == *target_layer_id_; })) {
        target_layer_id_.reset();  // (another page: back to its default layer)
    }
    canvas_->binding = book().binding;
    // (a person alone sees the page as it will print, name lines in blue; the name view is for the agent's name stage)
    canvas_->set_render_mode(!page->name_ok && agent_book() ? "name" : "proof");
    // (an animation page: the frame shown, with the frames around it faint when the timeline asks for it)
    canvas_->renderer().set_anim(current_frame(page), timeline_ == nullptr || timeline_->onion->isChecked());
    if (contents_) {  // (smart-objects@1: the contents' page, alone in the book)
        canvas_->set_page(shown_book(), 0);
    } else {
        canvas_->set_page(session_->snapshot(), static_cast<std::size_t>(page_index_));
    }
    if (!contents_ && book().is_deferred(static_cast<std::size_t>(page_index_))) {
        flash(QStringLiteral("このページを読み込んでいます。読み込みが終わると表示します。"), 3000);
    }
    pen_changed();
    refresh_status();
    if (navigator_ != nullptr) navigator_->update();
    if (layer_panel_ != nullptr) layer_panel_->refresh();
    channel_view_changed();  // (channels@1: this page's view on screen, its panel)
    quick_mask_follow();     // (masks@1: the page's Quick Mask, its menu item and the tools painting it)
    if (paths_panel_ != nullptr && paths_dock_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();  // (vector@1, PC3 C-a)
    if (lines_later_) {
        lines_timer_.start();
    } else {
        refresh_lines();
    }
    if (guides_ != nullptr && guides_->isVisible()) guides_->refresh();
    if (materials_ != nullptr && materials_->isVisible()) materials_->refresh();
    if (timeline_ != nullptr) {
        timeline_->frame = current_frame(page);
        if (timeline_->isVisible()) timeline_->refresh();
    }
    if (QAction* lapse = action("act_timelapse")) {
        const QSignalBlocker quiet(lapse);
        lapse->setChecked(render::timelapse::is_on(book()));
    }
}

void MainWindow::select_page(int row) {
    if (row < 0) return;
    if (contents_) {  // (smart-objects@1: a contents' tab shows its contents; the book's page kept for its own tab)
        page_index_ = row;
        return;
    }
    if (row != page_index_) {
        perf::event("page_switch", {{"row", row}});
        session_->save_now();  // a page switch writes what was done on the last page (on the worker)
        canvas_->set_selection(std::nullopt);
        canvas_->highlight_box.reset();  // (a problem shown by the checks stays on its page)
        if (panel_view_ != nullptr) panel_view_->frame_id.reset();
    }
    page_index_ = row;
    show_page();
    studio_follow(true);
}

void MainWindow::go_to_page(int index) {
    const core::Document& b = book();
    for (int row = 0; row < static_cast<int>(b.pages.size()); ++row) {
        if (b.pages[static_cast<std::size_t>(row)]->index == core::Num(static_cast<std::int64_t>(index))) {
            if (row != page_index_) pages_->setCurrentRow(row);
            return;
        }
    }
}

void MainWindow::jump(int delta) {
    const int next = page_index_ + delta;
    if (next >= 0 && next < static_cast<int>(book().pages.size())) pages_->setCurrentRow(next);
}

void MainWindow::flash(const QString& message, int ms, bool error) {
    QString text = message.toHtmlEscaped();
    text.replace(QLatin1Char('\n'), QStringLiteral(" ・ "));
    if (error) {
        status_->setText(QStringLiteral("<span style='color:%1'><b>⚠ %2</b></span>").arg(theme::tokens().danger, text));
        ms = std::max(ms, 6000);
        last_error_ = message;
    } else {
        status_->setText(QStringLiteral("<b>%1</b>").arg(text));
    }
    last_notice_ = message;
    flash_timer_.start(ms);
}

void MainWindow::refresh_status() {
    if (closed_) return;
    const SaveStatus status = session_->status();
    const core::Document& b = book();
    setWindowTitle(QStringLiteral("%1 第%2話 — Genko Studio").arg(QString::fromStdString(b.title), QString::fromStdString(b.episode.repr())));
    if (doc_ < doc_tabs_->count()) {
        doc_tabs_->setTabText(doc_, documents_[static_cast<std::size_t>(doc_)].title());
        doc_tabs_->setTabToolTip(doc_, session_->path() ? QDir::toNativeSeparators(qpath(*session_->path())) : QStringLiteral("未保存"));
    }
    save_label_->show_status(status);
    failure_bar_->show_status(status);
    if (!session_->read_only_reason().empty()) {
        read_only_->setText(QStringLiteral("この原稿は読み取り専用で開いています（変更はできません）: %1")
                                .arg(QString::fromStdString(session_->read_only_reason())));
        read_only_->show();
    } else {
        read_only_->hide();
    }
    if (colours_ != nullptr) colours_->set_page_color(current_page() != nullptr ? current_page()->color : std::nullopt);  // (color-mode@1)
    if (colours_ != nullptr) {  // (color-model@1, PC1d: an indexed page's table)
        const core::Page* page = current_page();
        colours_->set_color_table(page != nullptr && !page->color && page->rgb ? page->rgb->table : std::nullopt);
    }
    refresh_first_steps();
    maybe_backup();  // (after a save: バックアップ, when chosen in 環境設定)
    if (flash_timer_.isActive()) {
        // (a notice is showing: the status line goes back to the page's status after its time; the view's label and the
        // commands follow the book and the view meanwhile, as Python's _refresh_zoom)
        refresh_zoom();
        refresh_actions();
        return;
    }
    const QString saved = state_words(status) + (recording_ ? QStringLiteral(" ・ ● オートアクションを記録中") : QString());
    const core::Page* page = current_page();
    if (page == nullptr) {
        status_->setText(saved);
        refresh_zoom();
        return;
    }
    QString selected;
    if (page->selected_frame_id.is_string()) {
        const auto leaves = page->leaf_frames();
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            if (leaves[i]->id == page->selected_frame_id.get<std::string>()) selected = QStringLiteral(" ・ 選択中: %1 コマ目").arg(i + 1);
        }
    }
    const auto frames = page->leaf_frames().size();
    if (page->color || page->rgb) {  // (color-mode@1, color-model@1: the page's mode beside its panels, and its profile)
        const bool lab = page->color && (page->color->space == core::InkSpace::Lab || (page->color->space == core::InkSpace::Multi && page->color->profile.empty()));
        const std::string_view mode = page->color ? core::mode_name(*page->color) : page->rgb->table ? "indexed" : "rgb";
        const QString name = mode == "rgb"            ? QStringLiteral("RGB")
                             : mode == "indexed"      ? QStringLiteral("インデックスカラー（%1 色）").arg(page->rgb->table->colors.size())
                             : mode == "cmyk"         ? QStringLiteral("CMYK")
                             : mode == "gray"         ? QStringLiteral("グレー")
                             : mode == "bitmap"       ? QStringLiteral("2 階調")
                             : mode == "duotone"      ? QStringLiteral("ダブルトーン")
                             : mode == "multichannel" ? QStringLiteral("マルチチャンネル（%1 版）").arg(page->color->plates.size())
                                                      : QStringLiteral("Lab");
        selected += QStringLiteral(" ・ %1 %2 bit").arg(name).arg(page->color ? page->color->depth : page->rgb->depth);
        if (!lab) {
            const std::string said = page->color ? core::profile_description(page->color->profile, page->color->icc)
                                                 : core::profile_description(page->rgb->profile, page->rgb->icc);
            if (!said.empty()) selected += QStringLiteral("・") + QString::fromStdString(said);
        }
    }
    if (agent_book()) {
        status_->setText(QStringLiteral("%1 ページ（%2） ・ コマ %3%4 ・ %5")
                             .arg(QString::fromStdString(page->index.repr()), wording::stage(page->stage))
                             .arg(frames)
                             .arg(selected, saved));
    } else {
        status_->setText(QStringLiteral("%1 / %2 ページ ・ コマ %3 個%4 ・ %5")
                             .arg(QString::fromStdString(page->index.repr()))
                             .arg(b.pages.size())
                             .arg(frames)
                             .arg(selected, saved));
    }
    refresh_zoom();
    refresh_actions();
}

void MainWindow::refresh_zoom() {
    QString turned = canvas_->rotation() != 0.0 ? QStringLiteral(" ・ 回転 %1%2°").arg(canvas_->rotation() > 0 ? QStringLiteral("+") : QString()).arg(std::nearbyint(canvas_->rotation()))
                                                : QString();
    QString mirrored = canvas_->flipped() ? QStringLiteral(" ・ 左右反転") : QString();
    if (canvas_->flipped_vertical()) mirrored += QStringLiteral(" ・ 上下反転");
    zoom_label_->setText(QStringLiteral("表示 %1%%2%3").arg(canvas_->zoom_percent()).arg(turned, mirrored));
    if (QAction* mirror = action(QStringLiteral("act_mirror")); mirror != nullptr && mirror->isChecked() != canvas_->flipped()) {
        mirror->setChecked(canvas_->flipped());
    }
    if (!zoom_box_->hasFocus()) {
        zoom_box_->blockSignals(true);
        zoom_box_->setValue(canvas_->zoom_percent());
        zoom_box_->blockSignals(false);
    }
}

void MainWindow::refresh_actions() {
    show_color_mode();
    if (auto* exposure = action("act_exposure")) exposure->setEnabled(current_page() && session_->read_only_reason().empty());
    if (QAction* name_ok = action(QStringLiteral("act_name_ok"))) name_ok->setVisible(agent_book());  // (stages are for books made with agents)
    contents_tab_follow();  // (smart-objects@1: what a contents' tab cannot use, greyed)
}

void MainWindow::show_omitted(const QStringList& elements) {
    if (elements.isEmpty()) {
        unported_->hide();
        return;
    }
    // (what this build cannot draw yet, and the filter plugins left out for their own reasons: said apart)
    QStringList words, off, failed;
    for (const QString& element : elements) {
        if (element.startsWith(QStringLiteral("plugin_off:"))) {
            off << element.mid(11);
        } else if (element.startsWith(QStringLiteral("plugin_failed:"))) {
            failed << element.mid(14);
        } else {
            words << wording::unported_element(element);
        }
    }
    QStringList text;
    if (!words.isEmpty()) {
        text << QStringLiteral("未移植の要素があります（%1）。このページの編集はできますが、これらはこの版ではまだ表示されません（原稿からは消えません）。")
                    .arg(words.join(QStringLiteral("・")));
    }
    if (!off.isEmpty()) {
        text << QStringLiteral("プラグイン（%1）は使う設定になっていないため、その補正を表示していません（レイヤー → プラグインの設定）。")
                    .arg(off.join(QStringLiteral("・")));
    }
    if (!failed.isEmpty()) {
        text << QStringLiteral("プラグイン（%1）を動かせなかったため、その補正を表示していません（Python と Pillow、プラグインの設定を確かめてください）。")
                    .arg(failed.join(QStringLiteral("・")));
    }
    unported_->setText(text.join(QLatin1Char(' ')));
    unported_->show();
}

// --- the disk -------------------------------------------------------------------------------------------------------

void MainWindow::watch() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (session_->path()) {
        const QString file = qpath(*session_->path() / "project.json");
        if (QFileInfo::exists(file)) watcher_.addPath(file);
    }
}

QString MainWindow::key_hint(const char* attribute) const {
    const QAction* act = action(QString::fromLatin1(attribute));
    const QString key = act != nullptr ? act->shortcut().toString(QKeySequence::NativeText) : QString();
    return key.isEmpty() ? QString() : QStringLiteral("（%1）").arg(key);
}

QString MainWindow::links_kept() const {
    // (links that came while a book was being read are kept for the next book opened, and said)
    if (pending_links_.isEmpty()) return {};
    return QStringLiteral("\n\n届いていたリンク（%1）は、次に原稿を開いたときに使います。").arg(pending_links_.join(QStringLiteral("、")));
}

void MainWindow::on_disk_change() {
    watch();  // (project.json is replaced atomically, so the watch has to be set again)
    session_->check_outside();
    refresh_ai();
}

// --- several books -----------------------------------------------------------------------------------------------

void MainWindow::store_document() {
    if (documents_.empty() || doc_ >= static_cast<int>(documents_.size())) return;
    Document& doc = documents_[static_cast<std::size_t>(doc_)];
    doc.session = session_;
    doc.page_index = page_index_;
    doc.view = canvas_->view_state();
    doc.target_layer = target_layer_id_;
}

void MainWindow::show_document(int index) {
    const Document& doc = documents_[static_cast<std::size_t>(index)];
    doc_ = index;
    session_ = doc.session;
    page_index_ = doc.page_index;
    target_layer_id_ = doc.target_layer;
    contents_ = doc.contents;  // (smart-objects@1, PC3 A1: コンテンツを編集's tab)
    contents_title_ = doc.contents_title;
    contents_view_.reset();
    contents_view_of_.reset();
    if (pages_ != nullptr) pages_->setEnabled(!contents_);
    connect_session();
    if (panel_view_ != nullptr) panel_view_->frame_id.reset();
    canvas_->set_selection(std::nullopt);
    watch();
    // (another book in front: the placed pictures remembered for the one before — decoded and finished, up to
    // hundreds of MB at a high zoom — are let go; the book in front decodes its own again as its tiles ask)
    render::clear_placed_caches();
    render::brushes::follow_book(book());
    reload_pages();
    if (doc.view) canvas_->set_view_state(*doc.view);
    doc_tabs_->blockSignals(true);
    doc_tabs_->setCurrentIndex(index);
    doc_tabs_->setTabText(index, doc.title());
    doc_tabs_->blockSignals(false);
    refresh_status();
    apply_proof_view();  // (its own 色の校正 and 色域外警告)
    output_panels_follow(true);  // (another book: its history at once; the checks of the last one are out of date)
    known_requests_ = request_ids();  // (another book's requests are not new ones)
    studio_follow(true);
    refresh_ai();
    if (session_->path() && !pending_links_.isEmpty()) {  // (links that came while it was being read)
        const QStringList links = std::exchange(pending_links_, {});
        for (const QString& link : links) QTimer::singleShot(0, this, [this, link] { open_link(link); });
    }
    contents_tab_follow();
    check_links();  // (smart-objects@1: its linked files looked at)
}

void MainWindow::switch_document(int index) {
    if (index < 0 || index >= static_cast<int>(documents_.size()) || index == doc_) return;
    if (documents_[static_cast<std::size_t>(index)].session == session_) {
        session_->save_now();  // (smart-objects@1: another tab of the same book — its contents —: nothing to settle)
    } else if (session_->unsaved()) {
        // Every entrance observes the same unsaved/saving/failed boundary.
        if (!settle(doc_, QStringLiteral("切り替える"))) {
            doc_tabs_->blockSignals(true);
            doc_tabs_->setCurrentIndex(doc_);
            doc_tabs_->blockSignals(false);
            return;
        }
    } else {
        session_->save_now();  // (what was done in this book is written now, on the worker)
    }
    store_document();
    show_document(index);
}

void MainWindow::next_document(int step) {
    const int n = static_cast<int>(documents_.size());
    if (n > 1) switch_document(((doc_ + step) % n + n) % n);
}

void MainWindow::add_document(std::shared_ptr<Session> session) {
    if (session) session->set_idle(std::chrono::milliseconds(save_after_ms_));  // (環境設定's 変更を保存するまで)
    if (session) connect(session.get(), &Session::statusChanged, this, &MainWindow::maybe_backup, Qt::UniqueConnection);
    const bool untouched = documents_.size() == 1 && !session_->path() && session_->generation() == 0 &&
                           session_->waiting() == 0 && session_->status().kind != SaveKind::Saving;
    if (!untouched && session_->unsaved() && !settle(doc_, QStringLiteral("切り替える"))) return;
    store_document();
    // an untouched untitled book in the only tab gives its place
    if (untouched) {
        documents_[0] = Document{std::move(session)};
        doc_ = -1;
        show_document(0);
        canvas_->fit_page();
        return;
    }
    documents_.push_back(Document{std::move(session)});
    doc_tabs_->blockSignals(true);
    doc_tabs_->addTab(documents_.back().title());
    doc_tabs_->blockSignals(false);
    show_document(static_cast<int>(documents_.size()) - 1);
    canvas_->fit_page();
}

void MainWindow::open_project(const fs::path& path) {
    remember_project(path);
    const auto [window, index] = documents::find(path);
    if (window == this) {
        switch_document(index);
        return;
    }
    if (window != nullptr) {  // (another window has it: the two share it)
        add_document(window->documents()[static_cast<std::size_t>(index)].session);
        return;
    }
    // read on a worker thread (a thick book takes a moment): the window keeps answering
    flash(QStringLiteral("開いています: %1").arg(QDir::toNativeSeparators(qpath(path))), 30000);
    using Read = std::pair<std::optional<Session::Opened>, QString>;
    auto* watcher = new QFutureWatcher<Read>(this);
    const fs::path where = path;
    connect(watcher, &QFutureWatcher<Read>::finished, this, [this, watcher, where] {
        Read read = watcher->result();
        watcher->deleteLater();
        flash_timer_.stop();
        if (!read.first) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("原稿を開けませんでした:\n%1").arg(wording::error(read.second)) + links_kept());
            refresh_status();
            return;
        }
        // Exceptions after the worker read (including a copy changed during
        // the modal question) must not escape a Qt callback or replace the desk.
        try {
            // (the session, its thread and its timers are made here, on the window's thread)
            std::shared_ptr<Session> opened = Session::from(std::move(*read.first), where, opening_options());
            if (const auto& offer = opened->recovery_offer()) {
                const QString when = offer->written.toString(QStringLiteral("M月d日 HH:mm"));
                if (ask::question(this, QStringLiteral("復旧用のコピー"),
                                  QStringLiteral("この原稿には、保存できなかった変更の復旧用コピーがあります（%1、%2 ページ）。\n"
                                                 "復旧用コピーの内容を採用しますか？（採用すると新しい世代として保存されます。"
                                                 "採用しないときは、原稿に保存されている内容のまま開きます）")
                                      .arg(when)
                                      .arg(offer->pages))) {
                    opened->adopt_recovery();
                } else {
                    opened->decline_recovery();
                }
            }
            const std::shared_ptr<Session> added = opened;
            add_document(std::move(opened));
            if (session_ == added) color_policy_on_open(added);  // (カラー設定's policies: a page of another profile kept, converted or asked)
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("Genko"),
                         QStringLiteral("原稿・復旧用コピーを開けませんでした:\n%1").arg(wording::error(QString::fromUtf8(error.what()))) + links_kept());
            refresh_status();
        }
    });
    watcher->setFuture(QtConcurrent::run([where]() -> Read {
        try {
            return {Session::read(where, opening_options()), QString()};
        } catch (const std::exception& error) {
            return {std::nullopt, QString::fromUtf8(error.what())};
        }
    }));
}

bool MainWindow::settle(int index, const QString& verb, bool* defer_discard) {
    if (index < 0 || index >= static_cast<int>(documents_.size())) return true;
    const std::shared_ptr<Session> s = documents_[static_cast<std::size_t>(index)].session;
    try {
        if (!s->unsaved()) return true;
        // a save that is simply due: written now (the window keeps answering), then nothing is left to ask about
        const SaveKind kind = s->status().kind;
        if ((kind == SaveKind::Dirty || kind == SaveKind::Saving) && s->path()) {
            s->save_now();
            s->wait_saved(std::chrono::milliseconds(2500));
            if (!s->unsaved()) return true;
        }
        const bool switching = verb != QStringLiteral("閉じる");
        CloseGuard guard(this, s->status(), documents_[static_cast<std::size_t>(index)].title(), verb);
        if (switching) guard.discard_button()->setText(QStringLiteral("そのまま切り替える"));
        ask::exec(&guard);
        switch (guard.choice()) {
        case CloseGuard::Choice::Cancel:
            return false;
        case CloseGuard::Choice::Save:
            s->save_now();
            if (s->wait_saved(std::chrono::milliseconds(30000)) && !s->unsaved()) return true;
            flash(QStringLiteral("保存できなかったので、閉じずに残しました: %1").arg(failure_reason(s->status())), 6000, true);
            return false;
        case CloseGuard::Choice::SaveAs: {
            const int was = doc_;
            if (index != doc_) {
                store_document();
                show_document(index);
            }
            const bool ok = save_as_and_wait(verb, s);
            if (!ok && was != doc_ && was < static_cast<int>(documents_.size())) {
                store_document();
                show_document(was);
            }
            return ok;
        }
        case CloseGuard::Choice::Discard:
            if (switching) return true;  // (nothing is thrown away: the book stays in its tab with its changes)
            if (defer_discard != nullptr) *defer_discard = true;
            else s->discard();
            return true;
        }
    } catch (const std::exception& error) {
        // (whatever went wrong, the window and the book in memory stay)
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    }
    return false;
}

bool MainWindow::close_document(int index) {
    if (index < 0) index = doc_;
    if (index < 0 || index >= static_cast<int>(documents_.size())) return false;
    if (documents_.size() == 1) return close();
    // (smart-objects@1: a book also in another tab here — its contents' tab, or the book's beside it — stays open there)
    const auto& closing = documents_[static_cast<std::size_t>(index)].session;
    const bool shared_here = std::count_if(documents_.begin(), documents_.end(), [&](const Document& d) { return d.session == closing; }) > 1;
    if (!shared_here && !documents::open_elsewhere(closing.get(), this) && !settle(index)) return false;
    if (index != doc_) store_document();
    documents_.erase(documents_.begin() + index);
    doc_tabs_->blockSignals(true);
    doc_tabs_->removeTab(index);
    doc_tabs_->blockSignals(false);
    if (index == doc_) {
        doc_ = -1;
        show_document(std::min(index, static_cast<int>(documents_.size()) - 1));
    } else {
        doc_ = doc_tabs_->currentIndex();
    }
    return true;
}

MainWindow* MainWindow::new_window() {
    session_->save_now();
    auto* window = new MainWindow(session_);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    if (const core::Page* page = current_page()) window->go_to_page(static_cast<int>(core::py_int(page->index.json())));
    window->show();
    window->move(pos() + QPoint(40, 40));
    return window;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (closed_) {
        event->accept();
        return;
    }
    // (PC4 節 D: a heavy filter worked here — its window waits for the thread —: asked to stop, the window closed once it
    // is back, app/filter_menu.cpp)
    if (filter_working_) {
        filter_working_->request_stop();
        close_after_filter_ = true;
        event->ignore();
        return;
    }
    // every book here with changes not saved: saved, or the person's choice (キャンセル keeps the window)
    std::vector<std::pair<std::shared_ptr<Session>, std::uint64_t>> decisions;
    std::vector<std::shared_ptr<Session>> discards;
    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        if (documents::open_elsewhere(documents_[static_cast<std::size_t>(i)].session.get(), this)) continue;
        const auto chosen = documents_[static_cast<std::size_t>(i)].session;
        // (smart-objects@1: a book in two tabs here — its contents' tab — is settled once)
        if (std::any_of(documents_.begin(), documents_.begin() + i, [&](const Document& d) { return d.session == chosen; })) continue;
        if (i != doc_) {
            store_document();
            show_document(i);
        }
        bool discard = false;
        if (!settle(i, QStringLiteral("閉じる"), &discard)) {
            event->ignore();
            return;
        }
        decisions.emplace_back(chosen, chosen->generation());
        if (discard) discards.push_back(chosen);
    }
    // A later Cancel leaves every earlier book live. Re-entrant edits during
    // the questions also invalidate the decision instead of discarding new work.
    for (const auto& [chosen, generation] : decisions) {
        if (chosen->generation() != generation) {
            flash(QStringLiteral("確認中に原稿が変更されたため、閉じずに残しました。"), 6000, true);
            event->ignore();
            return;
        }
    }
    for (const auto& chosen : discards) chosen->discard();
    Drafts::instance().flush();  // (the reply drafts not written yet go into their books now)
    closed_ = true;
    documents::remove(this);
    disconnect_session();
    perf::flush();
    event->accept();
}

// --- files ---------------------------------------------------------------------------------------------------------

void MainWindow::new_book() {
    NewProjectDialog dialog(this);
    if (ask::exec(&dialog) == QDialog::Accepted && dialog.created) open_project(*dialog.created);
}

void MainWindow::open_book() {
    const QString path = ask::existing_dir(this, QStringLiteral("原稿（.genko のフォルダ）を開く"));
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path + QStringLiteral("/project.json"))) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("Genko の原稿ではありません（.genko のフォルダを選びます）"));
        return;
    }
    open_project(core::path_from_utf8(path.toStdString()));
}

void MainWindow::save() {
    if (!session_->path()) {
        save_as();
        return;
    }
    session_->save_now();
    if (session_->wait_saved(std::chrono::milliseconds(30000)) && !session_->unsaved()) {
        flash(QStringLiteral("保存しました: %1").arg(QDir::toNativeSeparators(qpath(*session_->path()))), 3000);
    } else {
        flash(QStringLiteral("保存できませんでした: %1").arg(failure_reason(session_->status())), 6000, true);
    }
}

void MainWindow::save_as() { save_as_and_wait(QString()); }

bool MainWindow::save_as_and_wait(const QString&, std::shared_ptr<Session> origin) {
    if (!origin) origin = session_;
    if (session_ != origin) return false;
    const QPointer<MainWindow> alive(this);
    const QString suggested = QString::fromStdString(origin->document().title.empty() ? std::string("無題") : origin->document().title) + QStringLiteral(".genko");
    const QString path = ask::save_path(this, QStringLiteral("原稿を保存する場所"), suggested);
    if (!alive || path.isEmpty()) return false;
    if (alive->session_ != origin) {
        alive->flash(QStringLiteral("確認中に原稿が切り替わったため、別名保存を中止しました。"), 6000, true);
        return false;
    }
    const QString target = path.endsWith(QStringLiteral(".genko")) ? path : path + QStringLiteral(".genko");
    bool done = false;
    bool ok = false;
    QString message;
    QObject callback_guard;
    const auto link = connect(origin.get(), &Session::savedAs, &callback_guard, [&](bool success, const QString& why) {
        done = true;
        ok = success;
        message = why;
    });
    origin->save_as(core::path_from_utf8(target.toStdString()));
    QElapsedTimer clock;
    clock.start();
    while (!done && alive && clock.elapsed() < 120000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QObject::disconnect(link);
    if (!alive || alive->session_ != origin) return false;
    if (!ok) {
        flash(QStringLiteral("保存できませんでした: %1").arg(wording::error(message)), 6000, true);
        return false;
    }
    const bool saved = origin->wait_saved(std::chrono::milliseconds(30000));
    if (!alive || alive->session_ != origin || !saved || origin->unsaved()) return false;
    remember_project(core::path_from_utf8(target.toStdString()));
    watch();
    refresh_status();
    flash(QStringLiteral("保存しました: %1").arg(QDir::toNativeSeparators(target)), 3000);
    return true;
}

namespace {

bool drawn_on(const core::Layer& layer) {
    return layer.stroke_count() > 0 || layer.raster_png || layer.color_raster || layer.ink_raster || (layer.shapes && !layer.shapes->items.empty());
}

// A row of hints over the page, one line high: on a window narrower than the row (a small screen, a large display
// scaling) it scrolls sideways instead of making the window wider than the screen (SPEC UX-02).
class HintStrip : public QScrollArea {
public:
    HintStrip(const QString& name, QWidget* row) {
        setObjectName(name);
        setWidget(row);
        setWidgetResizable(true);
        row->setAutoFillBackground(false);  // (the strip's own background shows: theme.cpp)
        viewport()->setAutoFillBackground(false);
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return QSize(widget()->sizeHint().width(), height_for(width())); }
    QSize minimumSizeHint() const override { return QSize(0, height_for(width())); }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QScrollArea::resizeEvent(event);
        fit();
    }
    void showEvent(QShowEvent* event) override {
        QScrollArea::showEvent(event);
        fit();
    }

private:
    // (the row's height, and the scroll bar's under it when the row does not fit)
    int height_for(int width) const {
        const bool sideways = widget()->minimumSizeHint().width() > width;
        return std::max(0, widget()->sizeHint().height()) + (sideways ? horizontalScrollBar()->sizeHint().height() : 0);
    }
    void fit() {
        const int wanted = height_for(width());
        if (height() != wanted || minimumHeight() != wanted) setFixedHeight(wanted);
    }
};

}  // namespace

QWidget* MainWindow::first_steps_bar() {
    auto* inside = new QWidget;
    auto* bar = new HintStrip(QStringLiteral("firstSteps"), inside);
    auto* row = new QHBoxLayout(inside);
    row->setContentsMargins(12, 6, 8, 6);
    row->setSpacing(6);
    auto* words = new QLabel(QStringLiteral("まだ何もない原稿です。最初の一歩:"));
    theme::role(words, "hint");
    row->addWidget(words);
    for (const auto& [label, tool, attribute] : {std::tuple{"コマを割る", "frame", "act_frame"}, {"ペンで描く", "pen", "act_pen"},
                                                 {"台詞を入れる", "text", "act_text"}}) {
        auto* button = new QPushButton(QString::fromUtf8(label) + key_hint(attribute));
        button->setProperty("words", QString::fromUtf8(label));
        button->setProperty("attribute", QByteArray(attribute));
        first_buttons_.push_back(button);
        const QString which = QString::fromLatin1(tool);
        connect(button, &QPushButton::clicked, this, [this, which] { choose_tool(which); });
        row->addWidget(button);
    }
    auto* ai = new QPushButton(QStringLiteral("AI に頼む…"));
    connect(ai, &QPushButton::clicked, this, &MainWindow::open_ai_dialog);
    row->addWidget(ai);
    row->addStretch(1);
    auto* close = new QPushButton;
    theme::iconed(close, "close");
    close->setToolTip(QStringLiteral("この案内を閉じる"));
    close->setAccessibleName(QStringLiteral("この案内を閉じる"));
    close->setProperty("iconbtn", true);
    close->setFixedSize(24, 24);
    connect(close, &QPushButton::clicked, this, [this, bar] {
        first_steps_closed_ = true;
        bar->hide();
    });
    row->addWidget(close);
    bar->hide();
    return bar;
}

QWidget* MainWindow::next_steps_bar() {
    auto* inside = new QWidget;
    auto* bar = new HintStrip(QStringLiteral("nextSteps"), inside);
    auto* row = new QHBoxLayout(inside);
    row->setContentsMargins(12, 4, 8, 4);
    row->setSpacing(6);
    auto* words = new QLabel(QStringLiteral("コマができました。次は:"));
    theme::role(words, "hint");
    row->addWidget(words);
    const auto step = [this, row](const QString& key, std::function<void()> go) {
        auto* button = new QPushButton;
        button->setObjectName(QStringLiteral("next_") + key);
        connect(button, &QPushButton::clicked, this, std::move(go));
        row->addWidget(button);
        next_buttons_[key] = button;
    };
    step(QStringLiteral("draw"), [this] { choose_tool(QStringLiteral("pen")); });
    step(QStringLiteral("line"), [this] { choose_tool(QStringLiteral("text")); });
    step(QStringLiteral("save"), [this] { action(QStringLiteral("act_save"))->trigger(); });
    step(QStringLiteral("export"), [this] { action(QStringLiteral("act_export"))->trigger(); });
    row->addStretch(1);
    auto* close = new QPushButton;
    theme::iconed(close, "close");
    close->setToolTip(QStringLiteral("この案内を閉じる（ヘルプ → 次の作業の案内を表示 で、また出せます）"));
    close->setAccessibleName(QStringLiteral("この案内を閉じる"));
    close->setProperty("iconbtn", true);
    close->setFixedSize(22, 22);
    connect(close, &QPushButton::clicked, this, [this] { action(QStringLiteral("act_next_steps"))->setChecked(false); });
    row->addWidget(close);
    bar->hide();
    return bar;
}

void MainWindow::refresh_first_steps() {
    if (column_ == nullptr) return;
    const core::Document& b = book();
    bool drawn = false;
    bool lines = !b.story.empty();
    bool panels = false;
    for (const auto& page : b.pages) {
        if (page->leaf_frames().size() > 1) panels = true;
        for (const core::Layer& layer : page->layers) drawn = drawn || drawn_on(layer);
    }
    const bool empty = !first_steps_closed_ && !lines && !drawn && !panels;
    if (empty && first_steps_ == nullptr) {
        first_steps_ = first_steps_bar();
        column_->insertWidget(column_->indexOf(canvas_), first_steps_);
    }
    if (first_steps_ != nullptr) first_steps_->setVisible(empty);
    for (QPushButton* button : first_buttons_) {  // (the keys as they are now: 環境設定 may have changed them)
        button->setText(button->property("words").toString() + key_hint(button->property("attribute").toByteArray().constData()));
    }
    // (I11) after the first panels: the next steps, marked as they are done, until closed
    QAction* wanted = action(QStringLiteral("act_next_steps"));
    const bool next = panels && wanted != nullptr && wanted->isChecked() && !(first_steps_ != nullptr && first_steps_->isVisible());
    if (next && next_steps_ == nullptr) {
        next_steps_ = next_steps_bar();
        column_->insertWidget(column_->indexOf(canvas_), next_steps_);
    }
    if (next_steps_ == nullptr) return;
    const bool saved = session_->path().has_value() && session_->status().kind == SaveKind::Saved;
    const auto mark = [](bool done, const QString& words) { return (done ? QStringLiteral("✓ ") : QString()) + words; };
    next_buttons_[QStringLiteral("draw")]->setText(mark(drawn, QStringLiteral("ペンで描く") + key_hint("act_pen")));
    next_buttons_[QStringLiteral("line")]->setText(mark(lines, QStringLiteral("台詞を入れる") + key_hint("act_text")));
    next_buttons_[QStringLiteral("save")]->setText(mark(saved, QStringLiteral("保存")));
    next_buttons_[QStringLiteral("export")]->setText(QStringLiteral("書き出し…"));
    next_steps_->setVisible(next);
}

}  // namespace genko::app
