#include "app/tool_settings.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

#include "app/lettering.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

// TITLES: each tool's name and what it does
const std::map<QString, std::pair<QString, QString>>& titles() {
    static const std::map<QString, std::pair<QString, QString>> all{
        {QStringLiteral("select"), {QStringLiteral("選択（V）"), QStringLiteral("コマをクリックで選ぶ・フキダシをドラッグで動かす・ダブルクリックで打ち直す・何もない所のドラッグで表示を動かす")}},
        {QStringLiteral("pen"), {QStringLiteral("ペン（B）"), QStringLiteral("描く先はレイヤー パネルで選んだレイヤー。Shift で直線、[ ] で太さ")}},
        {QStringLiteral("eraser"), {QStringLiteral("消しゴム（E）"), QStringLiteral("ペンの線は触れた所で切れる。トーンの上では削る")}},
        {QStringLiteral("text"), {QStringLiteral("テキスト（T）"), QStringLiteral("台詞を入れたい所をクリックして打つ（Ctrl+Enter で決定）")}},
        {QStringLiteral("frame"), {QStringLiteral("コマ割り（F）"), QStringLiteral("コマの中をドラッグで割る（ほぼ水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで動かす・角をドラッグで形を変える。"
                                                                            "「作り方」で長方形・折れ線・フリーハンドにすると、新しいコマを描ける")}},
        {QStringLiteral("picker"), {QStringLiteral("スポイト（I）"), QStringLiteral("クリックした所の色をペンの色にする")}},
        {QStringLiteral("zoom"), {QStringLiteral("虫めがね（Z）"), QStringLiteral("クリックで拡大、Alt＋クリックで縮小、ドラッグで囲んだ所を画面いっぱいに")}},
        {QStringLiteral("fill"), {QStringLiteral("塗りつぶし（G）"), QStringLiteral("線で囲まれた所をクリックで塗る。隙間は「隙間を閉じる」の幅まで閉じる")}},
        {QStringLiteral("lassofill"), {QStringLiteral("囲って塗る（Shift+G）"), QStringLiteral("ドラッグで囲んだ所を塗る")}},
        {QStringLiteral("marquee"), {QStringLiteral("範囲選択（M・L・W）"), QStringLiteral("ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み）")}},
        {QStringLiteral("vector"), {QStringLiteral("線の編集（Shift+Y）"), QStringLiteral("ペンの線の制御点を動かす・足す・消す。線をつなぐ・切る・色を変える")}},
        {QStringLiteral("blend"), {QStringLiteral("色混ぜ（Shift+B）"), QStringLiteral("なぞった所の色をぼかす・のばす・なじませる")}},
        {QStringLiteral("liquify"), {QStringLiteral("ゆがみ（Shift+L）"), QStringLiteral("なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く")}},
        {QStringLiteral("shape"), {QStringLiteral("図形（O）"), QStringLiteral("ドラッグで直線・長方形・楕円・多角形。折れ線と曲線はクリックで点を置く")}},
        {QStringLiteral("reshape"), {QStringLiteral("線の修正（Y）"), QStringLiteral("線をつまんでドラッグすると、その辺りが滑らかに曲がる")}},
        {QStringLiteral("ruler"), {QStringLiteral("定規（R）"), QStringLiteral("下で定規の種類を選び、ドラッグやクリックで置く。□をドラッグで動かす、Delete で消す")}},
        {QStringLiteral("3d"), {QStringLiteral("3D 操作（J）"), QStringLiteral("デッサン人形の関節（○）や箱をドラッグ。箱の上の○で回す")}},
        {QStringLiteral("effect"), {QStringLiteral("効果線（K）"), QStringLiteral("下で種類を選び、コマの中をクリックで入れる。中心の＋をドラッグで動かす")}},
        {QStringLiteral("stamp"), {QStringLiteral("素材を置く"), QStringLiteral("素材パネルで選んだ素材を、クリックした所に置く")}},
        {QStringLiteral("move"), {QStringLiteral("レイヤー移動（Q）"), QStringLiteral("描く先のレイヤーの線・塗り・絵を、ドラッグで丸ごと動かす。Shift で縦・横・45° に")}},
        {QStringLiteral("gradient"), {QStringLiteral("グラデーション（U）"), QStringLiteral("ドラッグの向きに塗る。始めの点がはじめの色、終わりの点が終わりの色。選択範囲があればその中だけ")}},
    };
    return all;
}

// SHORT: a command's words on a narrow page
const std::map<QString, QString>& shorts() {
    static const std::map<QString, QString> all{
        {QStringLiteral("コマを横に割る（上下に分ける）"), QStringLiteral("横に割る（上下に）")},
        {QStringLiteral("コマを縦に割る（左右に分ける）"), QStringLiteral("縦に割る（左右に）")},
        {QStringLiteral("コマを結合（割る前に戻す）"), QStringLiteral("結合（割る前に戻す）")},
        {QStringLiteral("テンプレートでコマを割る…"), QStringLiteral("テンプレートで割る…")},
        {QStringLiteral("選んだコマの枠線の太さ…"), QStringLiteral("枠線の太さ…")},
        {QStringLiteral("選んだコマの枠線をなくす"), QStringLiteral("枠線をなくす")},
        {QStringLiteral("選んだコマを断ち切りにする（紙の端まで）"), QStringLiteral("断ち切りにする")},
        {QStringLiteral("選んだコマの形を元に戻す"), QStringLiteral("形を元に戻す")},
        {QStringLiteral("選択範囲・選んだコマにトーンを貼る"), QStringLiteral("トーンを貼る")},
        {QStringLiteral("3D を線にする（描く先のレイヤーへ）"), QStringLiteral("3D を線にする")},
        {QStringLiteral("このページの定規をすべて消す"), QStringLiteral("定規をすべて消す")},
        {QStringLiteral("放射線定規（集中線）"), QStringLiteral("放射線定規")},
        {QStringLiteral("選択範囲の線の太さ…"), QStringLiteral("線の太さ…")},
        {QStringLiteral("このコマを消す（ほかのコマはそのまま）"), QStringLiteral("このコマを消す")},
        {QStringLiteral("このコマを選択範囲にする"), QStringLiteral("コマを選択範囲に")},
        {QStringLiteral("今のコマ割りをテンプレートに残す…"), QStringLiteral("テンプレートに残す…")},
        {QStringLiteral("選んだコマの角の丸み…"), QStringLiteral("角の丸み…")},
        {QStringLiteral("選んだ定規でコマを割る・作る"), QStringLiteral("定規でコマを割る・作る")},
    };
    return all;
}

// rows without a picture keep its room, so every row's words start at the same place
QIcon or_blank(const QIcon& icon) {
    if (!icon.isNull()) return icon;
    QPixmap blank(16, 16);
    blank.fill(Qt::transparent);
    return QIcon(blank);
}

QLabel* section(const QString& title) {
    auto* label = new QLabel(title);
    theme::role(label, "section");
    return label;
}

}  // namespace

QWidget* action_button(QAction* action) {
    if (const auto found = shorts().find(action->text()); found != shorts().end()) action->setIconText(found->second);
    if (action->isCheckable()) {
        auto* box = new QCheckBox(action->iconText());
        box->setToolTip(action->toolTip() != action->text() ? action->toolTip() : action->statusTip());
        box->setChecked(action->isChecked());
        QObject::connect(box, &QCheckBox::toggled, action, [action](bool on) {
            if (action->isChecked() != on) action->trigger();
        });
        QObject::connect(action, &QAction::toggled, box, [box](bool on) { box->setChecked(on); });
        return box;
    }
    auto* button = new QPushButton(action->iconText());
    button->setProperty("row", true);  // (drawn as a row: theme)
    button->setIcon(or_blank(action->icon()));
    button->setToolTip(!action->statusTip().isEmpty() ? action->statusTip() : action->toolTip());
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
    const auto follow = [button, action] {
        button->setEnabled(action->isEnabled());
        button->setVisible(action->isVisible());
        button->setIcon(or_blank(action->icon()));
    };
    QObject::connect(action, &QAction::changed, button, follow);
    follow();
    return button;
}

QPushButton* menu_button(const QString& label, const std::vector<std::vector<QAction*>>& groups) {
    auto* button = new QPushButton(label + QStringLiteral(" ▾"));
    button->setProperty("row", true);
    button->setIcon(or_blank(QIcon()));
    auto* menu = new QMenu(button);
    for (std::size_t n = 0; n < groups.size(); ++n) {
        if (n > 0) menu->addSeparator();
        for (QAction* action : groups[n]) menu->addAction(action);
    }
    button->setMenu(menu);
    return button;
}

QWidget* action_page(const std::vector<PageItem>& items) {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("toolPage"));  // (its switches line up with the rows: theme)
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    for (const PageItem& item : items) {
        if (std::holds_alternative<std::nullptr_t>(item)) {
            layout->addSpacing(8);
        } else if (const auto* title = std::get_if<QString>(&item)) {
            if (layout->count() > 0) layout->addSpacing(8);
            layout->addWidget(section(*title));
        } else if (const auto* widget = std::get_if<QWidget*>(&item)) {
            layout->addWidget(*widget);
        } else if (const auto* action = std::get_if<QAction*>(&item)) {
            layout->addWidget(action_button(*action));
        }
    }
    layout->addStretch(1);
    return page;
}

TextToolSettings::TextToolSettings(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("text_settings"));
    balloon = new QComboBox;
    balloon->setObjectName(QStringLiteral("text_balloon"));
    for (const auto& [key, label] : lettering::kinds()) balloon->addItem(label, key);
    vertical = new QCheckBox(QStringLiteral("縦書き"));
    vertical->setObjectName(QStringLiteral("text_vertical"));
    vertical->setChecked(true);
    draw_balloon = new QCheckBox(QStringLiteral("フキダシを手で描く"));
    draw_balloon->setObjectName(QStringLiteral("text_draw_balloon"));
    draw_balloon->setToolTip(QStringLiteral("ドラッグで囲んだ形がフキダシになり、そのあと台詞を打ちます。クリックだけなら、いつもどおり台詞を置きます"));
    font = new QComboBox;
    font->setObjectName(QStringLiteral("text_font"));
    font->addItem(QStringLiteral("いつもの書体（アンチック）"), QString());
    for (const auto& [key, label] : lettering::bundled_fonts()) {
        if (key != QLatin1String("antique")) font->addItem(label, key);
    }
    size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("text_size"));
    size->setRange(0, 60);
    size->setSingleStep(0.5);
    size->setSuffix(QStringLiteral(" mm"));
    size->setSpecialValueText(QStringLiteral("自動（フキダシに合わせる）"));
    auto* form = new QFormLayout(this);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(QStringLiteral("フキダシ"), balloon);
    form->addRow(QString(), vertical);
    form->addRow(QString(), draw_balloon);
    form->addRow(QStringLiteral("書体"), font);
    form->addRow(QStringLiteral("文字の大きさ"), size);
    auto* note = new QLabel(QStringLiteral("ルビは ｜約束《やくそく》、傍点は 《《強調》》、一部を大きく {大|…}（特大・小・太・赤・青・白も）と打ちます。"
                                           "入れた後の台詞は、台詞パネルで直せます。"));
    note->setWordWrap(true);
    theme::hint(note);
    form->addRow(note);
}

TextToolSettings::LineFields TextToolSettings::line_fields() const {
    LineFields fields;
    if (!font->currentData().toString().isEmpty()) fields.style["font"] = font->currentData().toString().toStdString();
    if (size->value() > 0) fields.style["size_mm"] = size->value();
    fields.balloon = balloon->currentData().toString().toStdString();
    fields.vertical = vertical->isChecked();
    return fields;
}

ToolSettings::ToolSettings(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tool_settings"));
    title_ = new QLabel;
    theme::role(title_, "heading");
    hint_ = new QLabel;
    hint_->setWordWrap(true);
    theme::hint(hint_);
    // the settings fold to the tool's name
    fold_ = new QPushButton(QStringLiteral("▾"));
    fold_->setProperty("iconbtn", true);
    fold_->setFixedSize(24, 24);
    fold_->setCheckable(true);
    fold_->setToolTip(QStringLiteral("ツールの設定をたたむ・開く"));
    connect(fold_, &QPushButton::toggled, this, [this](bool on) { set_folded(on); });
    auto* head = new QHBoxLayout;
    head->addWidget(title_, 1);
    head->addWidget(fold_);
    stack_widget_ = new QWidget;  // (the pages, one shown at a time, as tall as the one shown)
    stack_ = new QVBoxLayout(stack_widget_);
    stack_->setContentsMargins(0, 0, 0, 0);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addLayout(head);
    layout->addWidget(hint_);
    layout->addWidget(stack_widget_, 1);
    show_tool(tool_);
}

void ToolSettings::add(const std::vector<QString>& tools, QWidget* page) {
    bool known = false;
    for (const auto& [tool, p] : pages_) known = known || p == page;
    if (!known) {
        stack_->addWidget(page);
        page->setVisible(false);
    }
    for (const QString& tool : tools) pages_[tool] = page;
    if (std::find(tools.begin(), tools.end(), tool_) != tools.end()) show_tool(tool_);
}

QWidget* ToolSettings::page_for(const QString& tool) const {
    const auto found = pages_.find(tool);
    return found != pages_.end() ? found->second : nullptr;
}

void ToolSettings::set_folded(bool on) {
    folded_ = on;
    fold_->setText(on ? QStringLiteral("▸") : QStringLiteral("▾"));
    hint_->setVisible(!on && theme::show_hints() && !hint_->text().isEmpty());
    stack_widget_->setVisible(!on && page_for(tool_) != nullptr);
    QWidget* dock = parentWidget();
    while (dock != nullptr && !dock->inherits("QDockWidget")) dock = dock->parentWidget();
    if (dock != nullptr) dock->setMaximumHeight(on ? title_->sizeHint().height() + 18 : QWIDGETSIZE_MAX);
}

void ToolSettings::show_tool(const QString& tool) {
    tool_ = tool;
    const auto found = titles().find(tool);
    title_->setText(found != titles().end() ? found->second.first : tool);
    hint_->setText(found != titles().end() ? found->second.second : QString());
    hint_->setVisible(!folded_ && theme::show_hints() && !hint_->text().isEmpty());
    QWidget* page = page_for(tool);
    if (page != current_) {
        if (current_ != nullptr) current_->hide();
        current_ = page;
        if (current_ != nullptr) current_->show();
    }
    stack_widget_->setVisible(!folded_ && page != nullptr);
}

}  // namespace genko::app
