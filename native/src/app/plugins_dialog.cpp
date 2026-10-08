#include "app/plugins_dialog.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

#include "app/ask.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/plugins.hpp"

namespace genko::app {

namespace {

QString qpath(const std::filesystem::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

}  // namespace

PluginsDialog::PluginsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("plugins_dialog"));
    setWindowTitle(QStringLiteral("プラグインの設定"));
    const render::plugins::Settings now = render::plugins::settings();
    enabled = new QCheckBox(QStringLiteral("プラグインを使う"));
    enabled->setChecked(now.enabled);
    python = new QLineEdit(QString::fromStdString(now.python));
    python->setPlaceholderText(QStringLiteral("空: このコンピューターの python3 / python"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString path = ask::open_path(this, QStringLiteral("プラグインを動かす Python"));
        if (!path.isEmpty()) python->setText(path);
    });
    found = new QLabel;
    theme::role(found, "hint");
    connect(python, &QLineEdit::textChanged, this, [this] { show_found(); });
    auto* where = new QHBoxLayout;
    where->addWidget(python, 1);
    where->addWidget(pick);
    auto* note = new QLabel(QStringLiteral(
        "プラグインは、Genko の外の別のプロセスとして、このコンピューターの Python で動きます。落ちても原稿は壊れませんが、"
        "安全な隔離（サンドボックス）ではありません。プラグインはそのコードにできることを何でもできるので、信頼できるものだけを選んでください。"
        "渡すのは、フィルターをかける絵（そのレイヤーの絵。補正レイヤーでは、その下に描かれたページの絵）と設定だけです"
        "（原稿のファイルやパスワードは渡しません）。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    list = new QListWidget;
    list->setObjectName(QStringLiteral("plugins_list"));
    for (const render::plugins::Listed& item : render::plugins::listed()) {
        QString text = QString::fromStdString(item.key) + QStringLiteral(".py");
        if (item.manifest && item.manifest->contains("name")) text += QStringLiteral(" — ") + QString::fromStdString(core::py_str((*item.manifest)["name"]));
        auto* row = new QListWidgetItem(text, list);
        row->setData(Qt::UserRole, QString::fromStdString(item.key));
        row->setData(Qt::UserRole + 1, QString::fromStdString(item.sha256));  // (the file as shown: only that is chosen)
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(item.chosen ? Qt::Checked : Qt::Unchecked);
        row->setToolTip(QStringLiteral("sha256 %1（ファイルが変わったら選び直します）").arg(QString::fromStdString(item.sha256.substr(0, 16))));
    }
    if (list->count() == 0) {
        auto* row = new QListWidgetItem(QStringLiteral("（プラグインのフォルダーにまだありません）"), list);
        row->setFlags(Qt::NoItemFlags);
    }
    auto* folder = new QPushButton(QStringLiteral("フォルダーを開く"));
    connect(folder, &QPushButton::clicked, this, [] {
        std::error_code ec;
        std::filesystem::create_directories(render::plugins::folder(), ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(render::plugins::folder())));
    });
    auto* form = new QFormLayout;
    form->addRow(enabled);
    form->addRow(QStringLiteral("Python"), where);
    form->addRow(QString(), found);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    buttons->addButton(folder, QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (keep()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(new QLabel(QStringLiteral("動かしてよいプラグイン（チェックしたものだけ）")));
    layout->addWidget(list, 1);
    layout->addWidget(buttons);
    show_found();
}

void PluginsDialog::show_found() {
    render::plugins::Settings probe = render::plugins::settings();
    probe.python = python->text().trimmed().toStdString();
    // (where the runner's Python is: the setting as typed, without keeping it)
    std::optional<std::filesystem::path> where;
    if (!probe.python.empty()) {
        std::error_code ec;
        const std::filesystem::path given = core::path_from_utf8(probe.python);
        if (std::filesystem::is_regular_file(given, ec)) where = given;
    } else {
        where = render::plugins::python();
    }
    found->setText(where ? QStringLiteral("使う Python: %1").arg(qpath(*where))
                         : QStringLiteral("Python が見つかりません（プラグインを使うには Python と Pillow が要ります）"));
}

bool PluginsDialog::keep() {
    render::plugins::Settings s = render::plugins::settings();
    s.enabled = enabled->isChecked();
    s.python = python->text().trimmed().toStdString();
    try {
        render::plugins::save_settings(s);
        const auto now = render::plugins::listed();
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* row = list->item(i);
            const QString key = row->data(Qt::UserRole).toString();
            if (key.isEmpty()) continue;
            const bool on = row->checkState() == Qt::Checked;
            bool was = false;
            for (const auto& item : now) {
                if (item.key == key.toStdString()) was = item.chosen;
            }
            if (on == was) continue;
            if (on && !ask::question(this, QStringLiteral("プラグインを動かす"),
                                     QStringLiteral("「%1.py」を動かしてよいですか。\n\nプラグインは別のプロセスで動きますが、安全な隔離ではありません。"
                                                    "そのコードはこのコンピューターでできることを何でもできます。信頼できるものだけを選んでください。")
                                         .arg(key))) {
                row->setCheckState(Qt::Unchecked);
                continue;
            }
            render::plugins::choose(key.toStdString(), on, row->data(Qt::UserRole + 1).toString().toStdString());
        }
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    return true;
}

}  // namespace genko::app
