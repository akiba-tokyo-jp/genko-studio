#include "app/main_window.hpp"
#include "app/layer_panel.hpp"

#include <QApplication>
#include <QPointer>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

#include "app/ask.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/ime.hpp"
#include "app/look.hpp"
#include "app/navigator.hpp"
#include "app/pages_panel.hpp"
#include "app/perf.hpp"
#include "app/save_status.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/brushes.hpp"
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
    return options;
}

std::vector<MainWindow*>& open_windows() {
    static std::vector<MainWindow*> list;
    return list;
}

QString omitted_words(const QString& element) {
    static const std::map<QString, QString> words = {
        {"balloons", "台詞・フキダシ"}, {"tones", "トーン"}, {"effects", "効果線"}, {"prims", "3D"}, {"placed", "配置した画像"},
        {"nombre", "ノンブル"}, {"covers", "表紙の折り目"}, {"anim", "アニメーション"}, {"screen", "トーン化"},
        {"finish", "仕上げの白黒化"}, {"brush_library", "自作ブラシの読み込み"}};
    if (const auto it = words.find(element); it != words.end()) return it->second;
    if (element.startsWith(QStringLiteral("adjust:"))) return QStringLiteral("色調補正（%1）").arg(element.mid(7));
    return element;
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
    documents::add(this);
    pen_ = PenSettings::load();
    // the book's own brushes are known to the drawing (as Python's reader registers them)
    render::brushes::register_brushes(book().brush_custom);

    canvas_ = new PageCanvas;
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        // (Python's 300 × 280 at least for the page; less on a small screen or a large display scaling — 1024 × 640 at
        // 200 % is 512 × 320 — so the window with its bars and panels stays within the screen: SPEC UX-02)
        const QRect room = screen->availableGeometry();
        canvas_->setMinimumSize(std::clamp(room.width() - 420, 160, 300), std::clamp(room.height() - 260, 110, 280));
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
    });
    // the selection (main_window_select.cpp)
    connect(canvas_, &PageCanvas::selectionDrawn, this, &MainWindow::selection_drawn);
    connect(canvas_, &PageCanvas::selectionPainted, this, &MainWindow::selection_painted);
    connect(canvas_, &PageCanvas::wandRequested, this, &MainWindow::wand);
    connect(canvas_, &PageCanvas::colourAreaRequested, this, &MainWindow::select_colour);
    connect(canvas_, &PageCanvas::selectionTransformed, this, &MainWindow::transform_selection);
    connect(canvas_, &PageCanvas::selectionWarped, this, &MainWindow::warp_selection);

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
    ime_ = new ImeEntry(canvas_);  // (the entrance for typed words: the text tool uses it in M4)
    ime_->hide();

    status_ = new QLabel;
    status_->setMinimumWidth(10);
    statusBar()->addWidget(status_, 1);
    save_label_ = new SaveStatusLabel;
    statusBar()->addPermanentWidget(save_label_);
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
    connect_session();
    watch();
    reload_pages();
    pages_->setCurrentRow(0);
    theme::name_buttons(this);
    perf::watch_loop(this);
}

MainWindow::~MainWindow() {
    disconnect_session();
    documents::remove(this);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    canvas_->setFocus();
}

const core::Page* MainWindow::current_page() const {
    const core::Document& b = book();
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
    if (const core::Layer* layer = target_layer()) flash(QStringLiteral("描く先: %1").arg(wording::layer_label(*layer)), 2500);
    if (layer_panel_ != nullptr) layer_panel_->show_target();  // (not built again: a Ctrl / Shift+click keeps the others)
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
        auto result = bus.apply(*session_->snapshot(), *ops, core::Actor(session_->actor()));
        canvas_->set_page(std::make_shared<const core::Document>(std::move(result.doc)), static_cast<std::size_t>(page_index_));
    } catch (const core::Error&) {
        show_page();  // (a preview that cannot be made: the page as it is)
    }
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
    session_links_.push_back(connect(s, &Session::changed, this, &MainWindow::on_book_changed));
    session_links_.push_back(connect(s, &Session::statusChanged, this, &MainWindow::refresh_status));
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
    QElapsedTimer clock;
    clock.start();
    try {
        session_->apply(ops, ids);
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);  // (the window and the book stay as they were)
        return false;
    }
    perf::event("applied", {{"ms", clock.nsecsElapsed() / 1e6}, {"op", ops.size() > 0 && ops[0].contains("op") ? ops[0]["op"] : Json()}});
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
        show_page();
    }
    if (doc_ < doc_tabs_->count()) doc_tabs_->setTabText(doc_, documents_[static_cast<std::size_t>(doc_)].title());
    render::brushes::register_brushes(b.brush_custom);
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
        return;
    }
    if (target_layer_id_ && std::none_of(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == *target_layer_id_; })) {
        target_layer_id_.reset();  // (another page: back to its default layer)
    }
    canvas_->binding = book().binding;
    // (a person alone sees the page as it will print, name lines in blue; the name view is for the agent's name stage)
    canvas_->set_render_mode(!page->name_ok && agent_book() ? "name" : "proof");
    canvas_->set_page(session_->snapshot(), static_cast<std::size_t>(page_index_));
    if (book().is_deferred(static_cast<std::size_t>(page_index_))) {
        flash(QStringLiteral("このページを読み込んでいます。読み込みが終わると表示します。"), 3000);
    }
    pen_changed();
    refresh_status();
    if (navigator_ != nullptr) navigator_->update();
    if (layer_panel_ != nullptr) layer_panel_->refresh();
}

void MainWindow::select_page(int row) {
    if (row < 0) return;
    if (row != page_index_) {
        perf::event("page_switch", {{"row", row}});
        session_->save_now();  // a page switch writes what was done on the last page (on the worker)
        canvas_->set_selection(std::nullopt);
    }
    page_index_ = row;
    show_page();
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
    if (flash_timer_.isActive()) return;  // (a notice is showing: it goes back to the page's status after its time)
    const QString saved = state_words(status);
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
    if (auto* exposure = action("act_exposure")) exposure->setEnabled(current_page() && session_->read_only_reason().empty());
    if (QAction* name_ok = action(QStringLiteral("act_name_ok"))) name_ok->setVisible(agent_book());  // (stages are for books made with agents)
}

void MainWindow::show_omitted(const QStringList& elements) {
    if (elements.isEmpty()) {
        unported_->hide();
        return;
    }
    QStringList words;
    for (const QString& element : elements) words << omitted_words(element);
    unported_->setText(QStringLiteral("未移植の要素があります（%1）。このページの編集はできますが、これらはこの版ではまだ表示されません（原稿からは消えません）。")
                           .arg(words.join(QStringLiteral("・"))));
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

void MainWindow::on_disk_change() {
    watch();  // (project.json is replaced atomically, so the watch has to be set again)
    session_->check_outside();
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
    connect_session();
    canvas_->set_selection(std::nullopt);
    watch();
    render::brushes::register_brushes(book().brush_custom);
    reload_pages();
    if (doc.view) canvas_->set_view_state(*doc.view);
    doc_tabs_->blockSignals(true);
    doc_tabs_->setCurrentIndex(index);
    doc_tabs_->setTabText(index, doc.title());
    doc_tabs_->blockSignals(false);
    refresh_status();
}

void MainWindow::switch_document(int index) {
    if (index < 0 || index >= static_cast<int>(documents_.size()) || index == doc_) return;
    if (session_->unsaved()) {
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
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("原稿を開けませんでした:\n%1").arg(wording::error(read.second)));
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
            add_document(std::move(opened));
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("Genko"),
                         QStringLiteral("原稿・復旧用コピーを開けませんでした:\n%1").arg(wording::error(QString::fromUtf8(error.what()))));
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
    if (!documents::open_elsewhere(documents_[static_cast<std::size_t>(index)].session.get(), this) && !settle(index)) return false;
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
    // every book here with changes not saved: saved, or the person's choice (キャンセル keeps the window)
    std::vector<std::pair<std::shared_ptr<Session>, std::uint64_t>> decisions;
    std::vector<std::shared_ptr<Session>> discards;
    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        if (documents::open_elsewhere(documents_[static_cast<std::size_t>(i)].session.get(), this)) continue;
        const auto chosen = documents_[static_cast<std::size_t>(i)].session;
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

}  // namespace genko::app
