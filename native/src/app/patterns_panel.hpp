#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

#include <optional>
#include <utility>

#include "core/json.hpp"
#include "core/model.hpp"

class QComboBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The Patterns panel (PC4 E3, window.panel.patterns, SPEC.md COLOR-09「パターン」; Photoshop's パターン パネル): the
// machine's library of patterns by its groups (render/pattern_library: the settings folder's patterns.json) and this
// book's own patterns (patterns@1: the copies its fills use), each with a small picture. A click makes it the chosen
// pattern (Photoshop's 今のパターン: what 塗りつぶし… and a new pattern fill layer take first — the window's state,
// not the book's); a double click makes a pattern fill layer of it. The buttons: パターンを定義… (the selection, or the
// page, as the page shows it — into the library), 新規パターン塗りつぶしレイヤー, 名前を変更…, 削除 (a library pattern
// from the library; the book's from the book, refused while a fill uses it), and the panel's menu: パターンを読み込み…
// and パターンを書き出し… (.pat), 新規グループ…, ライブラリに追加 (a book's pattern into the library), 初期設定に戻す.
// A choice is "library:<name>" or "book:<id>".

namespace genko::app {

class MainWindow;

class PatternsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PatternsPanel(MainWindow* window);
    // The library and the book's patterns again (after a change).
    void refresh();
    QTreeWidget* tree() const { return tree_; }
    QPushButton* button(const QString& name) const;
    // The buttons' and the menu's work (tests call them as they do).
    void rename(std::optional<QString> name = std::nullopt);
    void remove();
    bool import_pat(const QString& path);
    bool export_pat(const QString& path);
    void to_library();
    void reset_library();

private:
    void chosen(QTreeWidgetItem* item);

    MainWindow* window_;
    QTreeWidget* tree_ = nullptr;
    bool loading_ = false;
};

// A pattern of the library or the book, as a choice names it: the pattern (its id: the book's, or empty for the
// library's) and its picture; none when it is not there.
std::optional<std::pair<core::Pattern, core::Bytes>> pattern_chosen(const core::Document& book, const QString& choice);
// A small picture of a pattern for a list.
QIcon pattern_icon(const core::Pattern& pattern, const core::Bytes& bytes, int side = 32);
// A box to choose a pattern in (the book's, then the library's by group), `current` chosen; its items' data the choices.
QComboBox* pattern_combo(const core::Document& book, const QString& current);

// The パターン塗り dialog (a new pattern fill layer, and one changed): the pattern, its 比率 (scale, 1..1000 %), 角度
// (−360..360°) and レイヤーにリンク; none when it is cancelled.
struct PatternFillSettings {
    QString choice;
    double scale = 100.0;
    double angle = 0.0;
    bool link = true;
};
std::optional<PatternFillSettings> pattern_fill_dialog(QWidget* parent, const core::Document& book, const PatternFillSettings& start,
                                                       const QString& title);

}  // namespace genko::app
