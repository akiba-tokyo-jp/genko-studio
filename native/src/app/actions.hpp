#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

class QListWidget;

// オートアクション (Python's genko/app/actions.py): record what was done (the ops) under a name, and do it again on another
// page. The recording keeps the ops as they were applied. Played back, they go to the page on screen and the layer
// being drawn on: "page" becomes the current page, "layer_id" the current layer, a panel the chosen panel (or none),
// and things the recording made (a layer, a tone, a line…) get new ids each time. Actions are kept in the settings
// folder (actions.json), so every book can use them.

namespace genko::app {

class MainWindow;

namespace actions {

std::filesystem::path path();
// The actions kept ({name: {"ops": […], "made": "YYYY-MM-DD HH:MM"}}, in their order); none readable: empty.
core::Json load();
// Kept in actions.json; why it could not be written (a folder that cannot be written, a full disk) when it was not —
// said by the caller, never ending the app (Python's raised OSError is only logged by Qt).
std::optional<QString> save(const core::Json& all);
std::optional<QString> store(const QString& name, const core::Json& ops);
std::optional<QString> remove(const QString& name);
// The ops worth recording (not undo, the book's pages, the gates …), copied.
core::Json recordable(const core::Json& ops);
// For each step: does it change one particular thing that was on the recorded page (a line, a stroke, an effect…)?
// Such steps cannot be done on another page and are left out when played.
std::vector<bool> page_bound(const core::Json& ops);
core::Json playable(const core::Json& ops);
// The recorded ops aimed at this page, layer and panel, with fresh ids for what they make (`new_id`: tests).
core::Json replay(const core::Json& ops, std::optional<core::Json> page, const std::optional<std::string>& layer_id,
                  const std::optional<std::string>& frame_id, const std::function<std::string()>& new_id = {});
QString describe(const core::Json& ops);
QString step_label(const core::Json& op);

// オートアクションの管理: each action's steps, to look at, reorder, leave out, rename, play or delete.
class ActionsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ActionsDialog(MainWindow* window);

    QListWidget* names = nullptr;
    QListWidget* steps = nullptr;
    void fill_names(const QString& select = {});
    void move_step(int delta);
    void drop_step();
    void rename();
    void remove_current();
    void play();

private:
    std::optional<QString> current() const;
    void fill_steps();
    void keep();

    MainWindow* window_;
    core::Json data_;
};

}  // namespace actions
}  // namespace genko::app
