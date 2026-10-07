#pragma once

#include <QAction>
#include <QActionGroup>
#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/canvas.hpp"
#include "app/pen.hpp"
#include "app/session.hpp"

class QLabel;
class QSpinBox;
class QTabBar;
class QToolBar;
class QMenu;
class QDockWidget;

// The main window (Python's genko/app/main.py MainWindow): the page in the middle, the tools on the left, the
// commands and the open books' tabs above, the navigator and the page list beside it, and in the status bar the page,
// the save state with where the book is kept, and the zoom. The menus hold the commands of M2 with Python's words and
// keys (docs/cpp-migration/ledger.json: the 52 gui actions of M2 and their groups).
//
// Every change goes through the book's Session (and so the CommandBus and the Saver): nothing here writes the book or
// its files. A book with changes not saved (or a save that failed, or one running) is never closed without asking.

namespace genko::app {

class Navigator;
class PageList;
class SaveFailureBar;
class SaveStatusLabel;
class ImeEntry;

// One open book in a window: its session and how the window last showed it.
struct Document {
    explicit Document(std::shared_ptr<Session> s = nullptr) : session(std::move(s)) {}

    std::shared_ptr<Session> session;
    int page_index = 0;
    std::optional<ViewState> view;
    std::optional<std::string> target_layer;
    QString title() const;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // A window on a book (shared with any other window on it); none: a new untitled book in memory (8 pages, A4).
    explicit MainWindow(std::shared_ptr<Session> session = nullptr);
    ~MainWindow() override;

    // --- the book in front ------------------------------------------------------------------------------------
    Session& session() const { return *session_; }
    const std::shared_ptr<Session>& session_ptr() const { return session_; }
    const core::Document& book() const { return session_->document(); }
    const core::Page* current_page() const;
    int page_index() const { return page_index_; }
    // The layer the pen draws on (the one chosen, else the ink — or the name, for a book made with agents at its name
    // stage).
    const core::Layer* target_layer() const;
    void set_target_layer(const std::string& layer_id);
    PenSettings& pen() { return pen_; }
    void pen_changed();

    PageCanvas* canvas() const { return canvas_; }
    PageList* pages() const { return pages_; }
    Navigator* navigator() const { return navigator_; }
    SaveStatusLabel* save_label() const { return save_label_; }
    SaveFailureBar* failure_bar() const { return failure_bar_; }
    QLabel* status_label() const { return status_; }
    QLabel* zoom_label() const { return zoom_label_; }
    QLabel* unported_band() const { return unported_; }
    QLabel* read_only_band() const { return read_only_; }
    QTabBar* doc_tabs() const { return doc_tabs_; }
    ImeEntry* ime() const { return ime_; }
    const std::vector<Document>& documents() const { return documents_; }
    int current_document() const { return doc_; }

    // --- the commands (the actions by Python's attribute names: "act_new" …) ---------------------------------
    QAction* action(const QString& attribute) const;
    const std::map<QString, QAction*>& actions_by_name() const { return actions_; }
    const std::map<QString, QAction*>& tool_actions() const { return tool_actions_; }
    const std::vector<QAction*>& border_kind_actions() const { return border_kind_actions_; }
    const std::map<QString, QAction*>& stage_actions() const { return stage_actions_; }
    QActionGroup* tools_group() const { return tools_; }

    // --- doing things ------------------------------------------------------------------------------------------
    // Apply in memory as the person, show it at once (saved after a pause). A refusal shows in the status line.
    bool apply_ops(const core::Json& ops, const std::vector<std::string>& ids = {});
    void go_to_page(int index);
    void select_page(int row);
    void choose_tool(const QString& tool);
    void add_document(std::shared_ptr<Session> session);
    void open_project(const std::filesystem::path& path);
    void next_document(int step);
    void switch_document(int index);
    // Close a book's tab (asking first when it has unsaved changes); the last one closes the window. False: kept.
    bool close_document(int index = -1);
    MainWindow* new_window();
    // A notice in the status line that never stops the work (problems in red, a little longer).
    void flash(const QString& message, int ms = 3000, bool error = false);
    QString last_notice() const { return last_notice_; }
    QString last_error() const { return last_error_; }
    // Before a book goes (its tab closes, the window, Genko): saved, or the person's choice. False: keep it open.
    bool settle(int index, const QString& verb = QStringLiteral("閉じる"), bool* defer_discard = nullptr);
    void refresh_status();
    // The window is closing for good (tests).
    bool closed() const { return closed_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    bool modal_target_unchanged(const std::shared_ptr<Session>& origin, const DocPtr& snapshot, int page_index);
    void build_actions();
    void build_menus();
    void build_toolbars();
    void build_docks();
    QAction* make(const QString& attribute, const QString& title, std::function<void()> slot, const QList<QKeySequence>& keys = {},
                  const QString& tip = {}, bool checkable = false);
    void connect_session();
    void disconnect_session();
    void on_book_changed(const BookChange& change);
    void reload_pages();
    void show_page();
    void store_document();
    void show_document(int index);
    void watch();
    void on_disk_change();
    void on_stroke(const StrokeInput& stroke);
    void on_frame_selected(const QString& frame_id);
    void context_menu(const QString& frame_id, const QPoint& global);
    void refresh_zoom();
    void refresh_actions();
    void layer_move_started();
    void undo();
    void redo();
    void save();
    void save_as();
    bool save_as_and_wait(const QString& verb, std::shared_ptr<Session> origin = {});
    void new_book();
    void open_book();
    void split(const QString& axis);
    double gutter_mm(const QString& cut) const;
    void gutter_settings();
    void set_selected_frame(const core::Json& change);
    const core::Frame* selected_frame() const;
    void border_width();
    void corner_radius();
    void border_kind(const QString& kind);
    void border_colour();
    void toggle_bleed();
    void frame_to_selection();
    void templates_dialog();
    void save_template();
    void add_page();
    void del_page();
    void duplicate_page(int index);
    void reorder_pages(const std::vector<int>& order, int follow);
    void page_overview();
    void name_ok();
    void point_width(double factor);
    void pick_colour();
    void exposure_dialog();
    void nombre_dialog();
    void layer_operation(const std::string& operation);
    void ask_zoom();
    void jump(int delta);
    void apply_stage(const QString& key);
    void delete_frame();
    void merge();
    void cut_frame(const QString& frame_id, const QPointF& p0, const QPointF& p1);
    void frame_drawn(const QVector<QPointF>& points);
    void layer_moved(double dx, double dy);
    void show_omitted(const QStringList& elements);
    bool agent_book() const;
    static bool drawable(const core::Layer& layer);
    // The layer drawn on when it can be painted on; otherwise a notice and null.
    const core::Layer* paint_layer();

    std::shared_ptr<Session> session_;
    std::vector<Document> documents_;
    int doc_ = 0;
    int page_index_ = 0;
    std::optional<std::string> target_layer_id_;
    bool closed_ = false;
    PenSettings pen_;
    double eraser_mm_ = 2.0;

    PageCanvas* canvas_ = nullptr;
    PageList* pages_ = nullptr;
    Navigator* navigator_ = nullptr;
    QDockWidget* pages_dock_ = nullptr;
    QDockWidget* navigator_dock_ = nullptr;
    QTabBar* doc_tabs_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* zoom_label_ = nullptr;
    QSpinBox* zoom_box_ = nullptr;
    QLabel* unported_ = nullptr;
    QLabel* read_only_ = nullptr;
    SaveStatusLabel* save_label_ = nullptr;
    SaveFailureBar* failure_bar_ = nullptr;
    ImeEntry* ime_ = nullptr;
    QToolBar* palette_ = nullptr;
    QToolBar* commands_ = nullptr;
    QMenu* recent_menu_ = nullptr;
    QMenu* view_menu_ = nullptr;

    std::map<QString, QAction*> actions_;
    std::map<QString, QAction*> tool_actions_;
    std::vector<QAction*> border_kind_actions_;
    std::map<QString, QAction*> stage_actions_;
    QActionGroup* tools_ = nullptr;

    QFileSystemWatcher watcher_;
    QTimer flash_timer_;
    QString last_notice_;
    QString last_error_;
    std::vector<QMetaObject::Connection> session_links_;
    std::uint64_t move_ticket_ = 0;
};

}  // namespace genko::app
