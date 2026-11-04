// The windows of Photomerge…, ファイルをスタックとして読み込み… and 統計… (PC5 I-g, SPEC.md AUTO-01): app/photomerge_dialog.hpp.

#include "app/photomerge_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

#include "app/ask.hpp"
#include "core/paths.hpp"

namespace genko::app {

namespace fs = std::filesystem;
namespace automate = service::automate;

namespace {

fs::path fpath(const QString& path) { return core::path_from_utf8(QDir::fromNativeSeparators(path.trimmed()).toStdString()); }

// ソースファイル: 使用 ファイル | フォルダー, the list and its buttons.
QGroupBox* source_box(QWidget* parent, SourceFiles& source, const QStringList& open_books) {
    auto* group = new QGroupBox(QStringLiteral("ソースファイル"), parent);
    auto* layout = new QVBoxLayout(group);
    source.use = new QComboBox(group);
    source.use->setObjectName(QStringLiteral("source_use"));
    source.use->addItem(QStringLiteral("ファイル"), QStringLiteral("files"));
    source.use->addItem(QStringLiteral("フォルダー"), QStringLiteral("folder"));
    auto* use_row = new QFormLayout;
    use_row->addRow(QStringLiteral("使用:"), source.use);
    layout->addLayout(use_row);
    auto* row = new QHBoxLayout;
    source.list = new QListWidget(group);
    source.list->setObjectName(QStringLiteral("source_list"));
    source.open_books = open_books;
    auto* buttons = new QVBoxLayout;
    auto* browse = new QPushButton(QStringLiteral("参照…"), group);
    auto* remove = new QPushButton(QStringLiteral("削除"), group);
    auto* open = new QPushButton(QStringLiteral("開いているファイルを追加"), group);
    open->setObjectName(QStringLiteral("source_add_open"));
    open->setEnabled(!open_books.isEmpty());
    for (QPushButton* b : {browse, remove, open}) buttons->addWidget(b);
    buttons->addStretch(1);
    row->addWidget(source.list, 1);
    row->addLayout(buttons);
    layout->addLayout(row);
    QListWidget* list = source.list;
    QComboBox* use = source.use;
    QObject::connect(browse, &QPushButton::clicked, group, [group, list, use] {
        if (use->currentData().toString() == QLatin1String("folder")) {
            const QString chosen = ask::existing_dir(group, QStringLiteral("ソースのフォルダー"), QString());
            if (chosen.isEmpty()) return;
            list->clear();
            list->addItem(QDir::toNativeSeparators(chosen));
            return;
        }
        const QString chosen = ask::open_path(group, QStringLiteral("ファイルを追加"));
        if (!chosen.isEmpty()) list->addItem(QDir::toNativeSeparators(chosen));
    });
    QObject::connect(remove, &QPushButton::clicked, group, [list] { delete list->currentItem(); });
    QObject::connect(open, &QPushButton::clicked, group, [list, open_books] {
        for (int i = 0; i < open_books.size(); ++i) {
            auto* item = new QListWidgetItem(QStringLiteral("（開いている原稿）%1").arg(open_books[i]), list);
            item->setData(Qt::UserRole, i);
        }
    });
    QObject::connect(use, &QComboBox::currentIndexChanged, group, [list] { list->clear(); });
    return group;
}

QDialogButtonBox* ok_cancel(QDialog* dialog, const std::function<std::optional<QString>()>& problem) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, problem] {
        if (const auto why = problem()) {
            ask::warning(dialog, dialog->windowTitle(), *why);
            return;
        }
        dialog->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

const std::vector<std::pair<const char*, const char*>>& modes() {
    static const std::vector<std::pair<const char*, const char*>> m = {
        {"entropy", "エントロピー"}, {"kurtosis", "尖度"}, {"maximum", "最大"}, {"mean", "平均"}, {"median", "中央値"}, {"minimum", "最小"},
        {"range", "範囲"}, {"skewness", "歪度"}, {"standard_deviation", "標準偏差"}, {"summation", "合計"}, {"variance", "分散"}};
    return m;
}

}  // namespace

// --- ソースファイル --------------------------------------------------------------------------------------------------------

std::vector<fs::path> SourceFiles::files() const {
    std::vector<fs::path> out;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem* item = list->item(i);
        if (item->data(Qt::UserRole).isValid()) continue;  // (an open book)
        out.push_back(fpath(item->text()));
    }
    return out;
}

std::vector<int> SourceFiles::open_chosen() const {
    std::vector<int> out;
    for (int i = 0; i < list->count(); ++i) {
        const QVariant v = list->item(i)->data(Qt::UserRole);
        if (v.isValid()) out.push_back(v.toInt());
    }
    return out;
}

int SourceFiles::count() const { return list->count(); }

// --- Photomerge… ---------------------------------------------------------------------------------------------------------

PhotomergeDialog::PhotomergeDialog(QWidget* parent, const QStringList& open_books) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Photomerge"));
    setObjectName(QStringLiteral("photomerge"));
    auto* top = new QHBoxLayout;
    auto* layouts = new QGroupBox(QStringLiteral("レイアウト"), this);
    auto* layouts_layout = new QVBoxLayout(layouts);
    layout = new QComboBox(layouts);
    layout->setObjectName(QStringLiteral("photomerge_layout"));
    for (const auto& [key, label] : std::vector<std::pair<const char*, const char*>>{{"auto", "自動設定"}, {"perspective", "遠近法"},
                                                                                     {"cylindrical", "円筒法"}, {"spherical", "球面法"},
                                                                                     {"collage", "コラージュ"}, {"reposition", "位置の変更"}}) {
        layout->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    }
    layouts_layout->addWidget(layout);
    layouts_layout->addStretch(1);
    top->addWidget(layouts);
    auto* right = new QVBoxLayout;
    right->addWidget(source_box(this, source, open_books));
    blend = new QCheckBox(QStringLiteral("画像を合成"), this);
    blend->setChecked(true);
    vignette = new QCheckBox(QStringLiteral("周辺光量補正"), this);
    geometric = new QCheckBox(QStringLiteral("幾何学的歪曲の補正"), this);
    fill = new QCheckBox(QStringLiteral("コンテンツに応じた塗りつぶしを透明な領域に適用"), this);
    for (QCheckBox* box : {blend, vignette, geometric, fill}) right->addWidget(box);
    connect(blend, &QCheckBox::toggled, fill, &QWidget::setEnabled);
    auto* scale_row = new QFormLayout;
    scale = new QSpinBox(this);
    scale->setObjectName(QStringLiteral("photomerge_scale"));
    scale->setRange(10, 100);
    scale->setValue(100);
    scale->setSuffix(QStringLiteral(" %"));
    scale->setToolTip(QStringLiteral("Genko の上限（1 億 2 千万画素・作業の記憶 2 GiB）を超えるパノラマは作れません。そのときは縮小して読み込みます"));
    scale_row->addRow(QStringLiteral("読み込むときに縮小:"), scale);
    right->addLayout(scale_row);
    top->addLayout(right, 1);
    auto* all = new QVBoxLayout(this);
    all->addLayout(top);
    all->addWidget(ok_cancel(this, [this] { return problem(); }));
}

automate::PhotomergeOptions PhotomergeDialog::options() const {
    automate::PhotomergeOptions o;
    o.layout = layout->currentData().toString().toStdString();
    o.blend = blend->isChecked();
    o.vignette = vignette->isChecked();
    o.geometric = geometric->isChecked();
    o.content_aware_fill = fill->isChecked() && blend->isChecked();
    o.scale = scale->value();
    return o;
}

std::optional<QString> PhotomergeDialog::problem() const {
    if (source.count() == 0) return QStringLiteral("ソースファイルを追加してください（2 枚以上）");
    if (source.use->currentData().toString() == QLatin1String("files") && source.count() < 2) return QStringLiteral("画像を 2 枚以上選んでください");
    return std::nullopt;
}

// --- ファイルをスタックとして読み込み…・統計… -------------------------------------------------------------------------------

StackDialog::StackDialog(QWidget* parent, const QStringList& open_books, bool statistics) : QDialog(parent) {
    setWindowTitle(statistics ? QStringLiteral("画像統計") : QStringLiteral("レイヤーを読み込み"));
    setObjectName(statistics ? QStringLiteral("statistics") : QStringLiteral("load_stack"));
    auto* all = new QVBoxLayout(this);
    if (statistics) {
        auto* row = new QFormLayout;
        mode = new QComboBox(this);
        mode->setObjectName(QStringLiteral("statistics_mode"));
        for (const auto& [key, label] : modes()) mode->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
        mode->setCurrentIndex(mode->findData(QStringLiteral("median")));
        row->addRow(QStringLiteral("スタックモードを選択:"), mode);
        all->addLayout(row);
    }
    all->addWidget(source_box(this, source, open_books));
    align = new QCheckBox(QStringLiteral("ソース画像を自動的に位置合わせ"), this);
    align->setObjectName(QStringLiteral("stack_align"));
    all->addWidget(align);
    if (!statistics) {
        smart = new QCheckBox(QStringLiteral("読み込み後にスマートオブジェクトを作成"), this);
        smart->setObjectName(QStringLiteral("stack_smart"));
        all->addWidget(smart);
    }
    all->addWidget(ok_cancel(this, [this] { return problem(); }));
}

std::optional<QString> StackDialog::problem() const {
    if (source.count() == 0) return QStringLiteral("ソースファイルを追加してください");
    return std::nullopt;
}

}  // namespace genko::app
