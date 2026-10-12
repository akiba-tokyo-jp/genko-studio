#pragma once

#include <QDialog>
#include <QPixmap>
#include <QString>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;

// 絵柄を選ぶ (Python's genko/app/style_picker.py): walk the manga style catalog (genre → sub-genre → finished style) and
// choose the book's style. The left side lists the branches of the level you are in (with their sample thumbnails); a
// double click goes down. The card on the right shows the chosen branch: its sample, where it is in the tree, and its
// words. Any level may be chosen (the levels under it are left to the artist). The catalog is read in the background
// (service::stylecat, the same reading as the studio tools and use_style: GENKO_STYLE_CATALOG may point it at a folder);
// nothing here writes to the book — the window calls MainWindow::use_style with the chosen id.

namespace genko::app {

class StylePicker : public QDialog {
    Q_OBJECT

public:
    // kept: the book's catalog style (briefs::catalog: null or {} when it has none); expression: the book's ("mono",
    // "color": a black-and-white style on a colour book is marked).
    StylePicker(QWidget* parent, const core::Json& kept = core::Json(), const std::string& expression = "mono");
    ~StylePicker() override;

    // After Accepted: the branch chosen, or back_to_genko when the person asked for Genko's own words instead.
    std::optional<std::string> chosen;
    bool back_to_genko = false;

    // The branch of the row chosen now.
    std::optional<std::string> chosen_id() const;
    // The branch whose children are listed (none: the genres).
    const std::optional<std::string>& here() const { return here_; }
    // Reads of the catalog still going (tests wait for none).
    int reading() const { return reading_; }
    void go_down(const std::string& node_id);
    void go_up();

    QLabel* where = nullptr;
    QPushButton* up = nullptr;
    QLineEdit* find = nullptr;
    QListWidget* list = nullptr;
    QLabel* note = nullptr;
    QLabel* picture = nullptr;
    QLabel* title = nullptr;
    QLabel* summary = nullptr;
    QLabel* fit = nullptr;
    QLabel* words = nullptr;
    QScrollArea* words_area = nullptr;
    QPushButton* show_words = nullptr;
    QPushButton* site = nullptr;
    QPushButton* ok = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* plain = nullptr;
    QPushButton* links_button = nullptr;

private:
    struct Result {
        core::Json json;
        std::string bytes;
        QString problem;  // (empty: read)
    };
    void run(std::function<Result()> job, std::function<void(const Result&)> then);
    void message(const QString& text, bool error = false);
    void got_tree(const Result& result);
    std::vector<std::string> children(const std::optional<std::string>& parent) const;
    std::optional<std::string> parent_of(const std::string& node_id) const;
    void show_list(const std::optional<std::string>& parent, const std::optional<std::string>& select = std::nullopt);
    void filter(const QString& typed);
    void thumb(const std::string& node_id);
    void show_card(const std::optional<std::string>& node_id);
    void detail(const std::string& node_id, const core::Json& data);
    void show_picture(const QPixmap& pixmap);

    core::Json kept_;
    std::string expression_;
    std::vector<std::string> order_;              // (the branches as the catalog lists them)
    std::map<std::string, core::Json> nodes_;
    std::optional<std::string> here_;
    std::map<std::string, core::Json> details_;
    std::map<std::string, QPixmap> pictures_;
    int reading_ = 0;
};

}  // namespace genko::app
