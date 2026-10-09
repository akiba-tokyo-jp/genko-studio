// 点検 (Python's genko/app/check_panel.py CheckPanel).

#include "app/check_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/main_window.hpp"
#include "app/wording.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "formats/checks.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::app {

using core::Json;

CheckPanel::CheckPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("check_panel"));
    summary = new QLabel(QStringLiteral("「点検する」で、入稿の前に直すところを探します。"));
    summary->setWordWrap(true);
    run_button = new QPushButton(QStringLiteral("点検する"));
    connect(run_button, &QPushButton::clicked, this, [this] { run(); });
    list = new QListWidget;
    list->setWordWrap(true);
    connect(list, &QListWidget::itemClicked, this, &CheckPanel::show);
    connect(list, &QListWidget::itemActivated, this, &CheckPanel::show);
    auto* top = new QHBoxLayout;
    top->addWidget(run_button);
    top->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(summary);
    layout->addWidget(list, 1);
}

void CheckPanel::refresh() {
    if (report_ && !stale_) {
        stale_ = true;
        summary->setText(summary->text().split(QLatin1Char('\n')).front() + QStringLiteral("\n原稿が変わりました。「点検する」で最新にします。"));
    }
}

std::optional<Json> CheckPanel::run() {
    Json report;
    try {
        report = formats::checks::book(window_->book(), window_->session().path());
    } catch (const core::Error& error) {
        // (what this build cannot check yet, or a book's data where Python's check stops too: said, nothing listed)
        report_.reset();
        stale_ = false;
        list->clear();
        shown_.clear();
        summary->setText(QStringLiteral("点検できませんでした。%1").arg(wording::error(QString::fromUtf8(error.what()))));
        return std::nullopt;
    }
    report_ = report;
    stale_ = false;
    list->clear();
    // sorted(issues, key=lambda i: (i["level"] != "error", i["page"] or 0)): the stopping ones first, then by page
    std::vector<Json> issues(report["issues"].begin(), report["issues"].end());
    const auto page_of = [](const Json& issue) {
        const Json& page = issue["page"];
        return core::py_truthy(page) ? core::py_float(page) : 0.0;
    };
    std::stable_sort(issues.begin(), issues.end(), [&](const Json& a, const Json& b) {
        const bool wa = a["level"] != "error";
        const bool wb = b["level"] != "error";
        if (wa != wb) return !wa;
        return page_of(a) < page_of(b);
    });
    shown_ = issues;
    for (const Json& issue : issues) {
        const QString mark = issue["level"] == "error" ? QStringLiteral("⛔") : QStringLiteral("⚠");
        const QString where = core::py_truthy(issue["page"]) ? QStringLiteral("%1 ページ ・ ").arg(QString::fromStdString(core::py_str(issue["page"])))
                                                             : QString();
        auto* item = new QListWidgetItem(QStringLiteral("%1 %2%3").arg(mark, where, QString::fromStdString(core::py_str(issue["message"]))));
        item->setData(Qt::UserRole, list->count());
        item->setToolTip(QStringLiteral("クリックでそのページのその場所を表示"));
        list->addItem(item);
    }
    if (issues.empty()) {
        summary->setText(QStringLiteral("直すところは見つかりませんでした。書き出せます。"));
    } else {
        summary->setText(QStringLiteral("止まる問題 %1 件、確かめた方がよいこと %2 件。")
                             .arg(report["errors"].get<std::int64_t>())
                             .arg(report["warnings"].get<std::int64_t>()));
    }
    return report;
}

void CheckPanel::show(QListWidgetItem* item) {
    const int row = item->data(Qt::UserRole).toInt();
    if (row < 0 || row >= static_cast<int>(shown_.size())) return;
    window_->show_issue(shown_[static_cast<std::size_t>(row)]);
}

}  // namespace genko::app
