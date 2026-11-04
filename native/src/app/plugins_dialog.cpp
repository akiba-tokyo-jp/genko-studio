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

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>

#include "app/ask.hpp"
#include "app/theme.hpp"
#include "service/wording.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/plugins.hpp"

namespace genko::app {

namespace {

QString qpath(const std::filesystem::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

// What a native plugin runs, as its plugin.json says: the program and its arguments (an argument with a space, a quote
// or nothing in it quoted)
QString what_runs(const core::Json& manifest) {
    QString out = QStringLiteral("動かすプログラム: %1").arg(QString::fromStdString(core::py_str(core::py_get(manifest, "executable", ""))));
    const core::Json args = core::py_get(manifest, "args", core::Json::array());
    if (args.is_array() && !args.empty()) {
        QStringList words;
        for (const core::Json& arg : args) {
            QString word = QString::fromStdString(arg.is_string() ? arg.get<std::string>() : core::py_str(arg));
            if (word.isEmpty() || word.contains(QLatin1Char(' ')) || word.contains(QLatin1Char('"'))) {
                word = QLatin1Char('"') + word.replace(QLatin1Char('"'), QStringLiteral("\\\"")) + QLatin1Char('"');
            }
            words << word;
        }
        out += QStringLiteral("\n引数: ") + words.join(QLatin1Char(' '));
    }
    return out;
}

}  // namespace

PluginsDialog::PluginsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("plugins_dialog"));
    setWindowTitle(QStringLiteral("プラグイン"));  // (PC5 J-f: 編集 → 環境設定 ▸ プラグイン…)
    const render::plugins::Settings now = render::plugins::settings();
    enabled = new QCheckBox(QStringLiteral("プラグインを使う"));
    enabled->setChecked(now.enabled);
    // (PC5 J-f: Photoshop's 追加のプラグインフォルダー — its *.wasm listed after the plugins' folder's)
    use_extra = new QCheckBox(QStringLiteral("追加のプラグインフォルダー"));
    use_extra->setObjectName(QStringLiteral("plugins_use_extra"));
    use_extra->setChecked(now.use_extra_folder);
    extra = new QLineEdit(QString::fromStdString(now.extra_folder));
    extra->setObjectName(QStringLiteral("plugins_extra_folder"));
    extra->setPlaceholderText(QStringLiteral("WebAssembly のプラグイン（.wasm）を置いたフォルダー"));
    auto* pick_extra = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick_extra, &QPushButton::clicked, this, [this] {
        const QString dir = ask::existing_dir(this, QStringLiteral("追加のプラグインフォルダー"), extra->text());
        if (!dir.isEmpty()) {
            extra->setText(dir);
            use_extra->setChecked(true);
        }
    });
    auto* extra_row = new QHBoxLayout;
    extra_row->addWidget(extra, 1);
    extra_row->addWidget(pick_extra);
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
        "WebAssembly のプラグイン（.wasm）は、Genko の外の別のプロセスの中の隔離された場所で動き、ファイルもネットも使えません"
        "（命令の数・時間・メモリに上限があります）。"
        "Python のプラグイン（.py）と、フォルダーに plugin.json とプログラムを置いたもの（ネイティブのプラグイン）は、別のプロセスで動くので落ちても原稿は壊れませんが、"
        "安全な隔離（サンドボックス）ではありません。そのコードにできることを何でもできるので、信頼できるものだけを選んでください。"
        "どのプラグインにも、渡すのはフィルターをかける絵と設定だけです（原稿のファイルやパスワードは渡しません）。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    list = new QListWidget;
    list->setObjectName(QStringLiteral("plugins_list"));
    fill_list(false);
    auto* folder = new QPushButton(QStringLiteral("フォルダーを開く"));
    connect(folder, &QPushButton::clicked, this, [] {
        std::error_code ec;
        std::filesystem::create_directories(render::plugins::folder(), ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(render::plugins::folder())));
    });
    // (PC5 J-f: plugin.install, plugin.remove, plugin.reload)
    auto* install = new QPushButton(QStringLiteral("インストール…"));
    install->setObjectName(QStringLiteral("plugins_install"));
    connect(install, &QPushButton::clicked, this, [this] {
        const QString path = ask::open_path(this, QStringLiteral("プラグインをインストール"), QStringLiteral("プラグイン (*.wasm *.py)"));
        if (!path.isEmpty()) (void)install_file(path);
    });
    auto* remove = new QPushButton(QStringLiteral("取り除く"));
    remove->setObjectName(QStringLiteral("plugins_remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (list->currentRow() >= 0) (void)remove_row(list->currentRow());
    });
    auto* again = new QPushButton(QStringLiteral("読み直す"));
    again->setObjectName(QStringLiteral("plugins_reload"));
    again->setToolTip(QStringLiteral("フォルダーのプラグインを読み直します（選んだ WebAssembly のプラグインは、使えるかも確かめます）"));
    connect(again, &QPushButton::clicked, this, [this] { reload(); });
    auto* list_buttons = new QHBoxLayout;
    list_buttons->addWidget(install);
    list_buttons->addWidget(remove);
    list_buttons->addWidget(again);
    list_buttons->addStretch(1);
    auto* form = new QFormLayout;
    form->addRow(enabled);
    form->addRow(use_extra, extra_row);
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
    layout->addLayout(list_buttons);
    layout->addWidget(buttons);
    show_found();
}

void PluginsDialog::fill_list(bool again) {
    // (the checks the person made and has not kept yet: kept across the list shown again)
    std::map<std::pair<QString, int>, Qt::CheckState> marked;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem* row = list->item(i);
        if (row->flags() & Qt::ItemIsUserCheckable) marked[{row->data(Qt::UserRole).toString(), row->data(Qt::UserRole + 4).toInt()}] = row->checkState();
    }
    list->clear();
    const std::vector<render::plugins::Listed> items = again ? render::plugins::reload() : render::plugins::listed();
    const render::plugins::Settings now = render::plugins::settings();
    for (const render::plugins::Listed& item : items) {
        // (a native plugin: its folder and plugin.json's name, read without running anything; a module: its file and size,
        // and once chosen what its manifest says)
        const int kind = item.wasm ? 2 : item.native ? 1 : 0;
        QString text = QString::fromStdString(item.key) + (item.wasm ? QStringLiteral(".wasm") : item.native ? QStringLiteral("（プログラム）") : QStringLiteral(".py"));
        std::optional<core::Json> manifest = item.manifest;
        std::optional<std::string> problem = item.problem;
        if (item.wasm && item.chosen && now.enabled && !manifest && !problem) {  // (chosen: asked its manifest, in its sandbox)
            try {
                manifest = render::plugins::describe(item.key).manifest;
            } catch (const std::exception& error) {
                problem = error.what();
            }
        }
        if (manifest && manifest->contains("name")) text += QStringLiteral(" — ") + QString::fromStdString(core::py_str((*manifest)["name"]));
        if (item.wasm && manifest) {
            const std::string version = core::py_str(core::py_get(*manifest, "version", ""));
            const std::string author = core::py_str(core::py_get(*manifest, "author", ""));
            if (!version.empty()) text += QStringLiteral(" ") + QString::fromStdString(version);
            if (!author.empty()) text += QStringLiteral("（%1）").arg(QString::fromStdString(author));
        }
        if (item.wasm) text += QStringLiteral("  %1 KB").arg(QString::number(std::max<double>(1.0, std::round(double(item.bytes) / 1024.0)), 'f', 0));
        auto* row = new QListWidgetItem(text, list);
        row->setData(Qt::UserRole, QString::fromStdString(item.key));
        row->setData(Qt::UserRole + 1, QString::fromStdString(item.sha256));  // (the file as shown: only that is chosen)
        row->setData(Qt::UserRole + 2, item.native);
        if (item.native && item.manifest) row->setData(Qt::UserRole + 3, what_runs(*item.manifest));
        row->setData(Qt::UserRole + 4, kind);
        if (problem) {  // (cannot be chosen: said why)
            row->setText(text + QStringLiteral("（使えません）"));
            row->setFlags(row->flags() & ~(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled));
            row->setToolTip(wording::error(QString::fromStdString(*problem)));
            continue;
        }
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        const auto mark = marked.find({QString::fromStdString(item.key), kind});
        row->setCheckState(mark != marked.end() ? mark->second : item.chosen ? Qt::Checked : Qt::Unchecked);
        QString tip = QStringLiteral("sha256 %1（ファイルが変わったら選び直します）").arg(QString::fromStdString(item.sha256.substr(0, 16)));
        if (item.wasm) {
            tip = QString::fromStdString(item.path) + QStringLiteral("\n") + tip;
            if (manifest && core::py_get(*manifest, "description", "").is_string() && !core::py_str((*manifest)["description"]).empty())
                tip = QString::fromStdString(core::py_str((*manifest)["description"])) + QStringLiteral("\n") + tip;
        }
        row->setToolTip(tip);
    }
    if (list->count() == 0) {
        auto* row = new QListWidgetItem(QStringLiteral("（プラグインのフォルダーにまだありません）"), list);
        row->setFlags(Qt::NoItemFlags);
    }
}

bool PluginsDialog::install_file(const QString& path) {
    try {
        const std::string key = render::plugins::install(core::path_from_utf8(path.toStdString()));
        fill_list(false);
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == QString::fromStdString(key)) list->setCurrentRow(i);
        }
        return true;
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("プラグインをインストール"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
}

bool PluginsDialog::remove_row(int at) {
    const QListWidgetItem* row = list->item(at);
    if (row == nullptr || row->data(Qt::UserRole).toString().isEmpty()) return false;
    const QString key = row->data(Qt::UserRole).toString();
    if (!ask::question(this, QStringLiteral("プラグインを取り除く"),
                       QStringLiteral("プラグイン「%1」をプラグインのフォルダーから取り除きますか。\n（ファイルは消え、元に戻すでは戻りません）").arg(key)))
        return false;
    try {
        render::plugins::remove(key.toStdString());
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("プラグインを取り除く"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    fill_list(false);
    return true;
}

void PluginsDialog::reload() {
    // (the additional folder as typed: looked in before it is kept)
    render::plugins::Settings s = render::plugins::settings();
    const bool use = use_extra->isChecked();
    const std::string folder = extra->text().trimmed().toStdString();
    if (s.use_extra_folder != use || s.extra_folder != folder) {
        s.use_extra_folder = use;
        s.extra_folder = folder;
        try {
            render::plugins::save_settings(s);
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
            return;
        }
    }
    fill_list(true);
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
                         : QStringLiteral("Python が見つかりません（Python のプラグインには Python と Pillow が要ります。ネイティブのプラグインには要りません）"));
}

bool PluginsDialog::keep() {
    render::plugins::Settings s = render::plugins::settings();
    s.enabled = enabled->isChecked();
    s.python = python->text().trimmed().toStdString();
    s.use_extra_folder = use_extra->isChecked();  // (PC5 J-f)
    s.extra_folder = extra->text().trimmed().toStdString();
    try {
        render::plugins::save_settings(s);
        const auto now = render::plugins::listed();
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* row = list->item(i);
            const QString key = row->data(Qt::UserRole).toString();
            if (key.isEmpty() || !(row->flags() & Qt::ItemIsUserCheckable)) continue;
            const bool on = row->checkState() == Qt::Checked;
            const bool native = row->data(Qt::UserRole + 2).toBool();
            const bool wasm = row->data(Qt::UserRole + 4).toInt() == 2;  // (PC5 J-f)
            bool was = false;
            for (const auto& item : now) {  // (a Python file, a folder and a module may share a key: the row's own kind)
                if (item.key == key.toStdString() && item.native == native && item.wasm == wasm) was = item.chosen;
            }
            if (on == was) continue;
            if (on && !ask::question(this, QStringLiteral("プラグインを動かす"),
                                     wasm ? QStringLiteral("「%1.wasm」（WebAssembly）を動かしてよいですか。\n\nWebAssembly のプラグインは、別のプロセスの中の隔離された場所で動き、"
                                                           "ファイルもネットも使えません（命令の数・時間・メモリに上限があります）。渡すのは、フィルターをかける絵と設定だけです。"
                                                           "このファイルが変わったら、もう一度選び直します。")
                                                .arg(key)
                                     : native ? QStringLiteral("「%1」（プログラム）を動かしてよいですか。\n\n%2\n\nプラグインは別のプロセスで動きますが、安全な隔離ではありません。"
                                                             "そのプログラムはこのコンピューターでできることを何でもできます。信頼できるものだけを選んでください。")
                                                  .arg(key, row->data(Qt::UserRole + 3).toString())
                                            : QStringLiteral("「%1.py」を動かしてよいですか。\n\nプラグインは別のプロセスで動きますが、安全な隔離ではありません。"
                                                             "そのコードはこのコンピューターでできることを何でもできます。信頼できるものだけを選んでください。")
                                                  .arg(key))) {
                row->setCheckState(Qt::Unchecked);
                continue;
            }
            // (a module listed while plugins were off was not read through: chosen as its bytes are now)
            const std::string shown = row->data(Qt::UserRole + 1).toString().toStdString();
            render::plugins::choose(key.toStdString(), on, shown.empty() ? std::nullopt : std::optional<std::string>(shown));
        }
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    return true;
}

}  // namespace genko::app
