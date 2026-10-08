#pragma once

#include <QDialog>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;

// プラグインの設定 (docs/cpp-migration/SPEC.md COMP-04): plugins on or off (off at first), the Python that runs them,
// and which plugins may run. The list shows the files and their manifests only (nothing runs to show it); a plugin
// chosen is asked about once more, with what running it means (its own process: a crash cannot take the book, but it is
// not a sandbox). render/plugins.hpp keeps the choice.

namespace genko::app {

class PluginsDialog : public QDialog {
    Q_OBJECT

public:
    explicit PluginsDialog(QWidget* parent);
    // Keep what is set (asking about each plugin newly chosen): false when nothing was kept.
    bool keep();

    QCheckBox* enabled = nullptr;
    QLineEdit* python = nullptr;
    QLabel* found = nullptr;
    QListWidget* list = nullptr;

private:
    void show_found();
};

}  // namespace genko::app
