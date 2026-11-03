#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <filesystem>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QToolButton;

// The timeline (タイムライン, Python's genko/app/timeline.py) of an animation page: a row per animation folder, a
// column per frame; each cell shows which cel is exposed there. Click a frame to see it (and draw on its cel),
// right-click to set the cel, play it, show the frames around it faint (onion skin), move the camera, write it out.

namespace genko::app {

class MainWindow;

class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    static constexpr int kCellWidth = 26;

    explicit TimelinePanel(MainWindow* window);

    // The frame shown (Python's panel.frame).
    std::int64_t frame = 1;

    // The animation folders of the page shown (their order on the timeline), and the one chosen (its row).
    std::vector<std::string> folder_ids() const;
    std::optional<std::string> current_folder() const;
    void refresh();
    // Show this frame; the drawing goes on the cel it shows in the chosen folder (`follow`).
    void set_frame(std::int64_t frame, bool follow = true);
    void play(bool on);
    // One frame on while playing.
    void tick();

    // The commands (Python's _start, _add_folder, _add_cel, _camera_here, _export; the cell menu). export_animation is
    // ビデオをレンダリング… (PC5 J-e1: its window — the frames, the size, the speed — then where).
    void start_animation();
    void add_folder();
    void add_cel();
    void camera_here();
    void export_animation();
    QMenu* cell_menu(int row, int column);

    // PC5 J-e1 (timeline.nextFrame・previousFrame・setFrame): the frame shown moved — "next", "previous", "first", "last",
    // "work_start", "work_end" (the frames played: the work area, else all) —, or asked for (フレームへ移動…). Playing stops;
    // a page that is not an animation is said so. The frame is the window's (the book is not changed).
    void go(const QString& where_to);
    void ask_frame();
    // The work area (timeline@1): [first, last] of the frames, none: all of them (set_animation's work).
    void set_work(std::optional<std::array<std::int64_t, 2>> work);
    void delete_timeline();  // (timeline.delete: set_animation off — the cels stay as layers)

    QCheckBox* onion = nullptr;
    QPushButton* start = nullptr;
    QPushButton* play_button = nullptr;
    QDoubleSpinBox* fps = nullptr;  // (PC5 J-e1: 0.5..120, timeline@1 past 1..60)
    QSpinBox* frames = nullptr;
    QCheckBox* loop = nullptr;
    QLabel* where = nullptr;
    QTableWidget* table = nullptr;
    QPushButton* add_folder_button = nullptr;
    QPushButton* add_cel_button = nullptr;
    QPushButton* camera = nullptr;
    QPushButton* export_button = nullptr;
    QTimer* timer = nullptr;
    // PC5 J-e1: the frame before and after (the commands' buttons), the work area (作業範囲: on, its first and last frame)
    // and the panel's menu (ビデオタイムラインを作成, タイムラインを削除, 移動 ▸, 作業範囲 ▸, ビデオをレンダリング…; J-e2 adds
    // the video layers' items to it).
    QToolButton* previous_button = nullptr;
    QToolButton* next_button = nullptr;
    QCheckBox* work = nullptr;
    QSpinBox* work_first = nullptr;
    QSpinBox* work_last = nullptr;
    QToolButton* menu_button = nullptr;
    QMenu* panel_menu = nullptr;
    // Its menu's commands and buttons made from the window's commands once they are made (MainWindow::build_anim_dock).
    void build_menu();

private:
    const core::Page* page() const;
    bool ops(const core::Json& ops);
    void set(const core::Json& change);
    // A frame drawn for playing (once each while it plays: quick enough to keep up after the first time round).
    QImage frame_picture(const core::Page& page, std::int64_t frame);

    MainWindow* window_;
    std::map<std::int64_t, QImage> played_;
};

// ビデオをレンダリング (PC5 J-e1, file.export.renderVideo): which frames — すべてのフレーム, 作業範囲 (the page's, when it
// has one), or from one frame to another —, the size (the width in pixels; none: the page's at 100 dpi) and the speed
// (0.5..120 fps, the page's at first). Where and in which format is asked after it (the panel's export_animation).
class RenderVideoDialog : public QDialog {
    Q_OBJECT

public:
    RenderVideoDialog(QWidget* parent, const core::Page& page);
    // [first, last] of the page's frames.
    std::array<std::int64_t, 2> frames() const;
    // The width asked for (none: as the page draws).
    std::optional<int> width_px() const;
    // The speed asked for (none: the page's own).
    std::optional<double> speed() const;

    QComboBox* range = nullptr;  // (data: "all" | "work" | "custom")
    QSpinBox* first = nullptr;
    QSpinBox* last = nullptr;
    QSpinBox* size = nullptr;  // (0: as the page draws)
    QDoubleSpinBox* fps = nullptr;

private:
    std::int64_t count_ = 1;
    std::array<std::int64_t, 2> work_{1, 1};
    double page_fps_ = 12;
};

// タイムラプスを書き出す (Python's TimelapseDialog): the recorded making-of as a moving picture, every page in the order
// it was drawn or one page; its format (MP4 only with ffmpeg), speed and length.
class TimelapseDialog : public QDialog {
    Q_OBJECT

public:
    TimelapseDialog(QWidget* parent, std::filesystem::path project, const core::Json& current_page);
    // The page asked for (none: all of them).
    std::optional<core::Json> page() const;
    // The pictures recorded for it, shown under the form.
    int count();
    std::filesystem::path write(const std::filesystem::path& dest);
    // 書き出す…: where, then written (a word when it cannot be).
    void run();

    QComboBox* which = nullptr;
    QComboBox* movie = nullptr;
    QDoubleSpinBox* fps = nullptr;
    QDoubleSpinBox* seconds = nullptr;
    QLabel* counted = nullptr;
    std::optional<std::filesystem::path> written;

private:
    std::filesystem::path project_;
    core::Json current_page_;
};

}  // namespace genko::app
