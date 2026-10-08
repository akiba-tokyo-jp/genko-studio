#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <filesystem>

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

    // The commands (Python's _start, _add_folder, _add_cel, _camera_here, _export; the cell menu).
    void start_animation();
    void add_folder();
    void add_cel();
    void camera_here();
    void export_animation();
    QMenu* cell_menu(int row, int column);

    QCheckBox* onion = nullptr;
    QPushButton* start = nullptr;
    QPushButton* play_button = nullptr;
    QSpinBox* fps = nullptr;
    QSpinBox* frames = nullptr;
    QCheckBox* loop = nullptr;
    QLabel* where = nullptr;
    QTableWidget* table = nullptr;
    QPushButton* add_folder_button = nullptr;
    QPushButton* add_cel_button = nullptr;
    QPushButton* camera = nullptr;
    QPushButton* export_button = nullptr;
    QTimer* timer = nullptr;

private:
    const core::Page* page() const;
    bool ops(const core::Json& ops);
    void set(const core::Json& change);
    // A frame drawn for playing (once each while it plays: quick enough to keep up after the first time round).
    QImage frame_picture(const core::Page& page, std::int64_t frame);

    MainWindow* window_;
    std::map<std::int64_t, QImage> played_;
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
