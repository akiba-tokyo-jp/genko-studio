#pragma once

#include <QWidget>

#include <optional>

class QLabel;
class QListWidget;
class QToolButton;

// ウィンドウ → アクション (PC5 J-g, window.panel.actions; Photoshop's アクション panel, PhotoCraft v0.2.0 ui-egui/src/actions.rs,
// MIT): the actions kept (service/actions: actions.json) and the steps of the one chosen, with 再生 (on the page in front, as
// the person), 記録 (新規アクション: its name first), 停止 and 削除, and 管理… (the steps reordered, left out, renamed: the
// actions' dialog). The recording and the playing are MainWindow's (app/main_window_actions.cpp): nothing new is recorded
// or played here.

namespace genko::app {

class MainWindow;

class ActionsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ActionsPanel(MainWindow* window);

    QListWidget* names = nullptr;
    QListWidget* steps = nullptr;
    QToolButton* play = nullptr;
    QToolButton* record = nullptr;
    QToolButton* stop = nullptr;
    QToolButton* remove = nullptr;
    QToolButton* manage = nullptr;
    QLabel* state = nullptr;

    // The list read again from actions.json (the one chosen kept), the buttons as the recording is.
    void refresh();
    std::optional<QString> current() const;
    // 記録: a name asked (or given), then the window records; 停止: what was recorded kept under that name.
    void start(std::optional<QString> name = std::nullopt);
    void finish();

private:
    void fill_steps();

    MainWindow* window_;
    std::optional<QString> recording_name_;
};

}  // namespace genko::app
