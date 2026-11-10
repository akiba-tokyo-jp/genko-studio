#pragma once

#include <QColor>
#include <QDialog>
#include <QPushButton>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "app/templates.hpp"
#include "core/model.hpp"
#include "formats/open_file.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QThreadPool;

// The dialogs of Python's genko/app/dialogs.py that M2 needs: the start screen, a new book, the paper settings and
// the panel templates. They are laid out so that their buttons stay in sight on a 1024 × 640 screen and at 125–200 %
// display scaling (SPEC UX-02): the body scrolls, the heading and the buttons do not.

namespace genko::app {

namespace look {
class PaperDiagram;
}
class PathLabel;

// The recent books (Python's main.recent_projects / remember_project: <config>/recent.json).
std::vector<std::filesystem::path> recent_projects();
void remember_project(const std::filesystem::path& path);
// PC4 F-b (最近使用したファイルを開く): the same list with the pictures opened as books in it too — each entry still there (a
// book's folder with its project.json, a file), the latest first, at most 環境設定 → ファイル管理's number of them;
// remember_file puts one first (a book or a file), forget_recent takes one out (a file gone when it was chosen).
std::vector<std::filesystem::path> recent_files();
void remember_file(const std::filesystem::path& path);
void forget_recent(const std::filesystem::path& path);

// 用紙の設定: a preset, or every number — paper, finished size, bleed and the basic frame's margins.
class PaperDialog : public QDialog {
    Q_OBJECT

public:
    PaperDialog(QWidget* parent, const core::PageSpec& spec, bool changing = false);
    // The paper chosen (throws core::Error when the numbers do not fit).
    core::PageSpec spec() const;
    // The set_page_spec op for this choice (Python's PaperDialog.op): the preset by its key, or every number; and
    // whether what is on the pages moves with the basic frame.
    core::Json op() const;
    QPushButton* ok_button() const { return ok_; }
    QPushButton* cancel_button() const { return cancel_; }
    // The picture of the paper (it follows the numbers).
    look::PaperDiagram* diagram() const { return diagram_; }

    QComboBox* preset = nullptr;
    QDoubleSpinBox *paper_w = nullptr, *paper_h = nullptr, *trim_w = nullptr, *trim_h = nullptr, *bleed = nullptr;
    QDoubleSpinBox *top = nullptr, *bottom = nullptr, *inner = nullptr, *outer = nullptr;
    QSpinBox* dpi = nullptr;
    QCheckBox* move = nullptr;
    QLabel* summary = nullptr;

private:
    void from_preset();
    void changed(bool keep_preset = false);

    QString preset_key_;
    look::PaperDiagram* diagram_ = nullptr;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

// 新しい原稿: the title, the episode and pages, the binding, the paper and where it is kept. 作る writes the book
// (on a worker thread) and `created` is its folder.
class NewProjectDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget* parent = nullptr);
    std::filesystem::path target() const;
    core::PageSpec chosen_spec() const;
    std::optional<std::filesystem::path> created;
    // Write the book (true when it was made).
    bool create();
    QPushButton* ok_button() const { return ok_; }
    QPushButton* cancel_button() const { return cancel_; }

    QLineEdit* title = nullptr;
    QSpinBox* episode = nullptr;
    QSpinBox* pages = nullptr;
    QComboBox* paper = nullptr;
    QComboBox* binding = nullptr;
    QLineEdit* folder = nullptr;

    // PC4 F-b (新規…, file.new; 節 F 5.3): the left of the window chooses 漫画の原稿 (the book above, written to its folder
    // at once) or 画像 (formats::new_image: a book of one page in memory, its .genko asked for when it is saved — the
    // user's decision 4). The image's name, preset, size and unit, resolution, colour mode, bits, canvas colour (白・黒・
    // 背景色 — the window's, set_background — ・透明・カスタム…) and profile (the working space's, or a built-in one).
    QListWidget* kind = nullptr;
    QLineEdit* image_name = nullptr;
    QComboBox* image_preset = nullptr;
    QDoubleSpinBox* image_width = nullptr;
    QDoubleSpinBox* image_height = nullptr;
    QComboBox* image_unit = nullptr;
    QDoubleSpinBox* image_dpi = nullptr;
    QComboBox* image_mode = nullptr;
    QComboBox* image_depth = nullptr;
    QComboBox* image_background = nullptr;
    QComboBox* image_profile = nullptr;
    bool making_image() const;
    // The image as asked (its pixels from the size and unit at its resolution).
    formats::NewImage chosen_image() const;
    void set_background(const QColor& colour);
    // 画像: made (true) into `image`, the dialog accepted; or what stops it said.
    bool create_image();
    std::optional<formats::Opened> image;

private:
    void note();
    void paper_changed();
    void image_changed();
    QStackedWidget* kinds_ = nullptr;
    QColor background_ = QColor(255, 255, 255);
    QColor custom_ = QColor(255, 255, 255);

    std::optional<core::PageSpec> custom_spec_;
    QLabel* paper_note_ = nullptr;
    PathLabel* where_note_ = nullptr;
    look::PaperDiagram* diagram_ = nullptr;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

// The first screen: three ways to begin as cards and the recent books (their first pages from the lasting cache).
class StartDialog : public QDialog {
    Q_OBJECT

public:
    explicit StartDialog(QWidget* parent = nullptr);
    std::optional<std::filesystem::path> chosen;
    void show_notice(const QString& words);
    std::vector<QPushButton*> cards() const { return cards_; }
    QPushButton* open_button() const { return open_; }
    QPushButton* close_button() const { return close_; }
    QListWidget* recent() const { return list_; }

private:
    void pick(const std::filesystem::path& path);

    std::vector<QPushButton*> cards_;
    QListWidget* list_ = nullptr;
    QLabel* notice_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* close_ = nullptr;
    std::vector<std::filesystem::path> paths_;
};

// テンプレートでコマを割る: the layouts as small pictures (a person's own first); `plan` is the change chosen.
class TemplateDialog : public QDialog {
    Q_OBJECT

public:
    TemplateDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page_index, std::string actor);
    ~TemplateDialog() override;
    std::optional<templates::Plan> plan;
    bool needs_clearing = false;
    QListWidget* list() const { return list_; }
    void choose();
    bool wait_pictures(int ms);

private:
    std::shared_ptr<const core::Document> doc_;
    std::size_t page_index_ = 0;
    std::string actor_;
    std::vector<templates::Template> items_;
    QListWidget* list_ = nullptr;
    std::unique_ptr<QThreadPool> pool_;
    std::shared_ptr<TemplateDialog*> self_;
    int pending_ = 0;
};

}  // namespace genko::app
