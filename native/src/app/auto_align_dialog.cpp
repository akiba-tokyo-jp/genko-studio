// レイヤーを自動整列… and レイヤーを自動合成… (PC5 I-g, SPEC.md AUTO-01): app/auto_align_dialog.hpp.

#include "app/auto_align_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace genko::app {

using core::Json;

namespace {

QDialogButtonBox* ok_cancel(QDialog* dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

Json ids_json(const std::vector<std::string>& ids) {
    Json out = Json::array();
    for (const std::string& id : ids) out.push_back(id);
    return out;
}

}  // namespace

AutoAlignDialog::AutoAlignDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("レイヤーを自動整列"));
    setObjectName(QStringLiteral("auto_align"));
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("投影法"), this);
    auto* grid = new QGridLayout(group);
    int k = 0;
    for (const auto& [key, label] : std::vector<std::pair<const char*, const char*>>{{"auto", "自動設定"}, {"perspective", "遠近法"},
                                                                                     {"collage", "コラージュ"}, {"cylindrical", "円筒法"},
                                                                                     {"spherical", "球面法"}, {"reposition", "位置の変更"}}) {
        auto* button = new QRadioButton(QString::fromUtf8(label), group);
        button->setObjectName(QStringLiteral("projection_%1").arg(QString::fromLatin1(key)));
        grid->addWidget(button, k / 3, k % 3);
        projections.emplace_back(key, button);
        ++k;
    }
    projections.front().second->setChecked(true);
    layout->addWidget(group);
    auto* lens = new QGroupBox(QStringLiteral("レンズ補正"), this);
    auto* lens_layout = new QVBoxLayout(lens);
    geometric = new QCheckBox(QStringLiteral("幾何学的歪曲"), lens);
    geometric->setObjectName(QStringLiteral("align_geometric"));
    lens_layout->addWidget(geometric);
    layout->addWidget(lens);
    layout->addWidget(ok_cancel(this));
}

Json AutoAlignDialog::op(const Json& page, const std::vector<std::string>& ids) const {
    std::string projection = "auto";
    for (const auto& [key, button] : projections) {
        if (button->isChecked()) projection = key;
    }
    Json op{{"op", "auto_align_layers"}, {"page", page}, {"ids", ids_json(ids)}, {"projection", projection}};
    if (geometric->isChecked()) op["geometric"] = true;
    return op;
}

AutoBlendDialog::AutoBlendDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("レイヤーを自動合成"));
    setObjectName(QStringLiteral("auto_blend"));
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("合成方法"), this);
    auto* inner = new QVBoxLayout(group);
    panorama = new QRadioButton(QStringLiteral("パノラマ"), group);
    panorama->setObjectName(QStringLiteral("blend_panorama"));
    stack = new QRadioButton(QStringLiteral("画像をスタック"), group);
    stack->setObjectName(QStringLiteral("blend_stack"));
    panorama->setChecked(true);
    inner->addWidget(panorama);
    inner->addWidget(stack);
    seamless = new QCheckBox(QStringLiteral("シームレスなトーンとカラー"), group);
    seamless->setObjectName(QStringLiteral("blend_seamless"));
    seamless->setChecked(true);
    inner->addWidget(seamless);
    layout->addWidget(group);
    layout->addWidget(ok_cancel(this));
}

Json AutoBlendDialog::op(const Json& page, const std::vector<std::string>& ids) const {
    return Json{{"op", "auto_blend_layers"}, {"page", page}, {"ids", ids_json(ids)}, {"method", stack->isChecked() ? "stack" : "panorama"},
                {"seamless", seamless->isChecked()}};
}

}  // namespace genko::app
