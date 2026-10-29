#pragma once

#include <QDialog>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QSpinBox;
class QToolButton;

// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」): Photoshop's レイヤーカンプ パネル for the page shown — its first
// row 最後のドキュメントの状態, then each comp (✓ the one applied last, ⚠ when it records a layer the page no longer has; its
// comment and options in its tip) — after PhotoCraft v0.2.0's ui-egui/src/comps_ui.rs (MIT). A double click on a comp
// applies it (on 最後のドキュメントの状態: restores it); the buttons, as Photoshop's: 前のレイヤーカンプを適用・次のレイヤーカンプを
// 適用・レイヤーカンプを更新・新規レイヤーカンプを作成・レイヤーカンプを削除; the panel's menu the rest (複製, オプション…, 警告を
// クリア, 最後のドキュメントの状態を復元, レイヤーカンプからファイル…). Each change an op through the window (apply_ops).

namespace genko::app {

class MainWindow;

class LayerCompsPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayerCompsPanel(MainWindow* window);
    // The page's rows again (after a change, another page).
    void refresh();
    // The comp the buttons work on: the row chosen, else the comp applied last (none: no comp).
    std::optional<std::int64_t> chosen() const;
    // Tests: the list (row 0 最後のドキュメントの状態; each comp's row holds its id), a button by its name (comp_previous,
    // comp_next, comp_update, comp_new, comp_delete, comp_menu), the panel's menu.
    QListWidget* list() const { return list_; }
    QToolButton* button(const QString& name) const;
    QMenu* menu() const { return menu_; }
    // The buttons' and the menu's work (tests call them as they do).
    void apply_chosen();
    void step(bool next);
    void update_chosen();
    void add_new();
    void remove_chosen();
    void duplicate_chosen();
    void options_of_chosen();
    void clear_warnings();
    void restore_last();

private:
    void double_clicked(QListWidgetItem* item);

    MainWindow* window_;
    QListWidget* list_ = nullptr;
    QMenu* menu_ = nullptr;
    bool loading_ = false;
};

// レイヤーカンプオプション (and 新規レイヤーカンプ): its name, what it puts back (表示/非表示・位置・アピアランス（レイヤー
// スタイル）) and its comment.
class LayerCompOptionsDialog : public QDialog {
    Q_OBJECT
public:
    LayerCompOptionsDialog(QWidget* parent, const QString& title, const QString& name, bool visibility, bool position, bool appearance,
                           const QString& comment);
    QLineEdit* name = nullptr;
    QCheckBox* visibility = nullptr;
    QCheckBox* position = nullptr;
    QCheckBox* appearance = nullptr;
    QPlainTextEdit* comment = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

// レイヤーカンプからファイル…: where, the prefix, the chosen comp alone, the format and a JPEG's quality.
class LayerCompsToFilesDialog : public QDialog {
    Q_OBJECT
public:
    LayerCompsToFilesDialog(QWidget* parent, const QString& folder, const QString& prefix, bool one_chosen);
    QLineEdit* folder = nullptr;
    QLineEdit* prefix = nullptr;
    QCheckBox* chosen_only = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

}  // namespace genko::app
