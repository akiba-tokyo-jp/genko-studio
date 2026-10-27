#pragma once

#include <QDialog>

#include <optional>
#include <string>

#include "core/json.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

// 編集 → プリセット → プリセットマネージャー… (PC4 E2, edit.presets.presetManager; SPEC.md COLOR-09「プリセットマネージャー」;
// Photoshop's プリセットマネージャー): the presets of each kind — ブラシ, グラデーション, パターン, シェイプ, ツールプリセット —
// by their groups (render/preset_files manager_list), and what can be done to them: 名前を変更…, 削除, 上へ・下へ, グループに
// 移動…, 新規グループ…, グループ名を変更…, グループを削除. Each is the kind's library's edit, the file read again first and
// written whole (render/preset_files manager_edit); the built-in brushes cannot be changed (said).

namespace genko::app {

class MainWindow;

class PresetManager : public QDialog {
    Q_OBJECT
public:
    explicit PresetManager(MainWindow* window);
    // The kind shown ("brushes", "gradients", "patterns", "shapes", "tool_presets").
    void show_kind(const std::string& kind);
    std::string kind() const;
    // The list again.
    void refresh();
    // One edit of the kind shown ({"action", …}), as the buttons send it: true when done (else said).
    bool apply(const core::Json& params);
    QTreeWidget* tree() const { return tree_; }
    QComboBox* kinds = nullptr;
    QLabel* problem = nullptr;
    QPushButton* button(const QString& name) const;

private:
    // The preset chosen (its name, or a brush's key) and its group; none for a group row.
    std::optional<std::pair<std::string, std::string>> chosen() const;
    std::optional<std::string> chosen_group() const;
    void rename();
    void remove();
    void move(int by);
    void to_group();
    void new_group();
    void rename_group();
    void remove_group();

    MainWindow* window_;
    QTreeWidget* tree_ = nullptr;
};

}  // namespace genko::app
