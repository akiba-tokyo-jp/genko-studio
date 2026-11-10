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
// PC5 J-f (edit.preferences.plugIns, plugin.install, plugin.remove, plugin.reload): 編集 → 環境設定 ▸ プラグイン… — the
// additional plugins folder (追加のプラグインフォルダー), the WebAssembly modules among the plugins (their size; once chosen
// their manifest's name, version and author; one that cannot be used with its reason: nothing runs before it is chosen),
// the consent for a module (a sandbox: no files, no network), and インストール…, 取り除く and 読み直す.

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
    // (PC5 J-f)
    QCheckBox* use_extra = nullptr;
    QLineEdit* extra = nullptr;
    // Install a plugin file (.wasm, .py) into the plugins' folder, remove the plugin of a row, read the plugins again
    // (each chosen module described again): the list shown again; false (said) when refused.
    bool install_file(const QString& path);
    bool remove_row(int row);
    void reload();

private:
    void show_found();
    // The rows of the plugins as they are (`again`: everything remembered of them forgotten first, render::plugins::reload).
    void fill_list(bool again);
};

}  // namespace genko::app
