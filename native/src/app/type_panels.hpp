#pragma once

#include <QComboBox>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

class QAbstractListModel;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListView;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QToolButton;

// PC5 G-d (type.panels.*, window.panel.*; SPEC.md TEXT-02「文字・段落・スタイル・字形のパネル」, design section G 5.3, 8-5 (a)):
// Photoshop's 文字, 段落, 文字スタイル, 段落スタイル and 字形 panels, for the text chosen (MainWindow::type_target: the target
// type layer, else the chosen line) — the whole of it, or the range chosen in the editor typing over it (graphemes:
// MainWindow::type_range). The lettering fields the 台詞 panel had (書体・大きさ・字間・行間・揃え・色・太さ・斜体・長体・縦中横・
// 約物の詰め) are here now, once: the 台詞 panel keeps what only a line has (its list, words, speaker, balloon, tails, rubies,
// 傍点, its effects and order). Every change is an op through the window: a line set as before (no native_type) given
// what its style says — its face, size, tracking, colour, weight, slant, width, 縦中横, alignment, 約物の詰め — on its whole
// keeps its style (edit_line style, drawn as before); anything else is set_type (the OpenType typesetting, as Photoshop
// sets it), the styles type_style (MainWindow::type_style), an inserted glyph edit_type. What a panel shows comes of the
// record's resolved attributes (core::type_text) and the book's styles (core::type_styles::list, current): no op.

namespace genko::app {

class MainWindow;

// A font list that says when its popup is about to open (the computer's fonts are read then, once).
class FontBox : public QComboBox {
    Q_OBJECT
public:
    using QComboBox::QComboBox;
    void showPopup() override;
signals:
    void aboutToShowPopup();
};

// 文字 panel (Photoshop's order): フォントファミリー, フォントスタイル, サイズ, 行送り, カーニング, トラッキング, 垂直比率, 水平比率,
// ベースラインシフト, カラー, 疑似ボールド, 疑似斜体, オールキャップス, スモールキャップス, 上付き, 下付き, 下線, 取り消し線, OpenType's
// buttons, 言語, アンチエイリアス; Genko's 縦中横 and 行間 (a line's style: the room added to its rows), and — 東アジア言語の機能 —
// プロポーショナルメトリクス. Lengths in 環境設定 → 単位・定規's 文字の単位 (the book keeps mm).
class CharacterPanel : public QWidget {
    Q_OBJECT
public:
    explicit CharacterPanel(MainWindow* window);
    void refresh();
    // The font list with the computer's fonts in it (read once: service::briefs::type_fonts), the fonts used last first.
    void fill_fonts();

    FontBox* family = nullptr;      // (data: a bundled key or a font file's path; its face's index in Qt::UserRole + 1)
    QComboBox* style = nullptr;      // (the family's styles: data the font file's path; UserRole + 1 its face's index)
    QDoubleSpinBox* size = nullptr;  // (0: 自動 — a line fitted to its balloon)
    QDoubleSpinBox* leading = nullptr;  // (行送り; 0: 自動)
    QComboBox* kerning = nullptr;
    QSpinBox* tracking = nullptr;    // (1/1000 em)
    QSpinBox* scale_y = nullptr;     // (%)
    QSpinBox* scale_x = nullptr;     // (%)
    QDoubleSpinBox* baseline = nullptr;
    QPushButton* color = nullptr;
    QComboBox* weight = nullptr;     // (疑似ボールド: なし・太く・もっと太く — Genko's weights)
    QToolButton* italic = nullptr;
    QToolButton* all_caps = nullptr;
    QToolButton* small_caps = nullptr;
    QToolButton* superscript = nullptr;
    QToolButton* subscript = nullptr;
    QToolButton* underline = nullptr;
    QToolButton* strikethrough = nullptr;
    std::vector<QToolButton*> features;  // (OpenType: each holds its tag in its objectName "type_ot_<tag>")
    QComboBox* language = nullptr;
    QComboBox* antialias = nullptr;
    QCheckBox* tcy = nullptr;
    QCheckBox* proportional = nullptr;
    QDoubleSpinBox* line_leading = nullptr;  // (行間: a line's style.leading, the room added to its rows)
    QLabel* title = nullptr;

private:
    void set(const core::Json& attrs, const core::Json& style = core::Json());
    void pick_color();
    void family_chosen();
    void style_chosen();
    void script(bool super, bool on);

    MainWindow* window_;
    bool loading_ = false;
    std::vector<std::function<void()>> pending_;  // (the number fields' changes waiting to be sent: review P2-5)
    bool fonts_read_ = false;
    bool waiting_fonts_ = false;  // (the computer's fonts being read for the list: review P2-6)
};

// 段落 panel: 揃え (Photoshop's seven), 左／右／1 行目のインデント, 段落前／後のアキ, 禁則処理, 文字組み (Genko's 約物の詰め),
// ハイフネーション, 方向 and Composer.
class ParagraphPanel : public QWidget {
    Q_OBJECT
public:
    explicit ParagraphPanel(MainWindow* window);
    void refresh();

    std::vector<QToolButton*> aligns;  // (each holds its value in its objectName "type_align_<value>")
    QDoubleSpinBox* start_indent = nullptr;
    QDoubleSpinBox* end_indent = nullptr;
    QDoubleSpinBox* first_indent = nullptr;
    QDoubleSpinBox* space_before = nullptr;
    QDoubleSpinBox* space_after = nullptr;
    QComboBox* kinsoku = nullptr;
    QCheckBox* yakumono = nullptr;
    QCheckBox* hyphenate = nullptr;
    QComboBox* direction = nullptr;
    QComboBox* composer = nullptr;
    QLabel* title = nullptr;

private:
    void set(const core::Json& attrs, const core::Json& style = core::Json());

    MainWindow* window_;
    bool loading_ = false;
    std::vector<std::function<void()>> pending_;  // (the number fields' changes waiting to be sent: review P2-5)
};

// 文字スタイル and 段落スタイル panels (one each, as Photoshop's): the book's styles — [なし] / [基本段落] first —, the one the
// chosen text takes marked ▶ and its overrides "+"; a click applies it (Alt: its overrides cleared too), the buttons make a
// new one from the text, redefine it from the text, clear the overrides, delete it; the panel's menu duplicates, renames
// and sets a style's options (スタイルオプション…). All through MainWindow::type_style.
class TypeStylesPanel : public QWidget {
    Q_OBJECT
public:
    TypeStylesPanel(MainWindow* window, bool paragraph);
    void refresh();
    // The style of the row chosen (none: なし; "basic": 基本段落), whether a row is chosen.
    std::optional<std::string> chosen() const;
    // The buttons' and the menu's work (tests call them as they do).
    void apply_row(QListWidgetItem* item, bool clear_overrides);
    void new_style();
    void redefine();
    void clear_override();
    void remove();
    void duplicate();
    void rename(const QString& name);
    void options();

    QListWidget* list = nullptr;
    QToolButton* new_button = nullptr;
    QToolButton* redefine_button = nullptr;
    QToolButton* clear_button = nullptr;
    QToolButton* delete_button = nullptr;
    QToolButton* menu_button = nullptr;

private:
    std::string kind() const { return paragraph_ ? "paragraph" : "character"; }
    core::Json range_args() const;

    MainWindow* window_;
    bool paragraph_;
    bool loading_ = false;
};

// 字形 panel: the characters the font maps (its cmap; upstream glyphs.rs's 表示 categories and the kana, CJK symbols and
// kanji), the ones used last, and — 異体字 — the variation sequences (IVS) the font maps for the character chosen. A double
// click puts the character at the caret of the text being typed, or (none typed) where the caret was last, or at the end
// of the text chosen (edit_type).
class GlyphsPanel : public QWidget {
    Q_OBJECT
public:
    explicit GlyphsPanel(MainWindow* window);
    void refresh();
    // The font shown: the chosen text's at its caret (or the one picked), and the characters of the category.
    void show_font(const QString& font, int face_index);
    void insert(const QString& glyph);

    QComboBox* font = nullptr;
    QComboBox* category = nullptr;
    QListWidget* recent = nullptr;
    QListView* glyphs = nullptr;  // (its model the font's characters of the category, made as they are shown)
    QListWidget* variants = nullptr;

private:
    void chosen(const QString& glyph);
    void fill();

    MainWindow* window_;
    QString font_;
    int face_ = 0;
    QString family_;  // (the Qt family the glyphs are shown in)
    std::shared_ptr<const std::vector<char32_t>> mapped_;  // (the font's characters, kept a while: charmap_of)
    QAbstractListModel* glyph_model_ = nullptr;
};

}  // namespace genko::app
