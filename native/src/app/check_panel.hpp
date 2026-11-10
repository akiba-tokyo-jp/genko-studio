#pragma once

#include <QWidget>

#include <optional>
#include <vector>

#include "core/json.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// 点検 (Python's genko/app/check_panel.py): 入稿前の点検 on demand (formats/checks: every problem in the book with its
// page), the stopping ones first; clicking one shows it on its page (MainWindow::show_issue). After an edit the list is
// said to be out of date until it is run again (checking draws every line's letters, so it is not run on every edit).

namespace genko::app {

class MainWindow;

class CheckPanel : public QWidget {
    Q_OBJECT

public:
    explicit CheckPanel(MainWindow* window);

    // After edits: the list may be out of date.
    void refresh();
    // Check the book in front now (checks.book with its folder): the report, listed. Nothing when the check could not
    // be made (what this build cannot check yet, or the book's data where Python stops too: said in the summary).
    std::optional<core::Json> run();
    const std::optional<core::Json>& report() const { return report_; }
    // The issue of a row of the list (the order shown).
    const core::Json& issue_at(int row) const { return shown_.at(static_cast<std::size_t>(row)); }

    QLabel* summary = nullptr;
    QPushButton* run_button = nullptr;
    QListWidget* list = nullptr;

private:
    void show(QListWidgetItem* item);

    MainWindow* window_;
    std::optional<core::Json> report_;
    std::vector<core::Json> shown_;
    bool stale_ = false;
};

}  // namespace genko::app
