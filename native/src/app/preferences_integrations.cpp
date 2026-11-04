// 環境設定 → 連携 (PC5 J-g, edit.preferences.integrations; SPEC.md PROD-01「制御チャンネル」): the control channel turned on
// or off (integrations.controlChannel, off when the file has none), what it is and what it never reaches, and whether it
// listens now.

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "app/control_channel.hpp"
#include "app/look.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "core/prefs_table.hpp"

namespace genko::app::preferences {

QWidget* integrations_page(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("prefs_integrations"));
    auto* rows = new QFormLayout(page);
    rows->addRow(look::section(QStringLiteral("制御チャンネル")));
    auto* on = new QCheckBox(QStringLiteral("制御チャンネルを使う"));
    on->setObjectName(QStringLiteral("prefs_control_channel"));
    bool now = false;
    try {
        now = core::prefs::value("integrations.controlChannel") == core::Json(true);
    } catch (const std::exception&) {
        now = false;
    }
    on->setChecked(now);
    on->setToolTip(QStringLiteral("このパソコンのプログラム（AI のエージェントなど）が、トークンで名乗って、動いている Genko の画面を見て、前の原稿に操作を当てます"));
    rows->addRow(QString(), on);
    auto* about = new QLabel(QStringLiteral(
        "このパソコンの、この利用者だけがつなげるローカルソケットで待ちます（ネットワークのポートは開きません）。"
        "トークンは genko token add --actor ai:<名前> で作り、そのトークンの名前で原稿の記録に残ります。"
        "当てられる操作と断りは MCP の apply_ops と同じです（ページの鍵・承認後の決まり・人だけの操作は断ります）。"
        "承認・プラグインの同意・AI 接続・環境設定・トークンには届きません。ファイルを開く・保存することもありません。"
        "ペンで描いている間は、この画面の操作を止めて状態の欄に出します（Esc で止められます）。"));
    about->setWordWrap(true);
    theme::role(about, "hint");
    rows->addRow(QString(), about);
    const control::Server& server = control::Server::instance();
    auto* state = new QLabel(server.listening()             ? QStringLiteral("いまは待っています（%1）").arg(server.words())
                             : !server.problem().isEmpty() ? QStringLiteral("入れてありますが、待てません: %1").arg(server.problem())
                                                           : QStringLiteral("いまは切っています"));
    state->setObjectName(QStringLiteral("prefs_control_state"));
    state->setWordWrap(true);
    rows->addRow(QStringLiteral("状態"), state);
    look::quiet_labels(rows);
    return page;
}

void save_integrations_page(QWidget* page) {
    if (page == nullptr) return;
    auto* on = page->findChild<QCheckBox*>(QStringLiteral("prefs_control_channel"));
    if (on == nullptr) return;
    const auto settings = core::prefs::store();
    (void)core::prefs::set(*settings, core::Json{{"integrations.controlChannel", on->isChecked()}});
    settings->sync();
    control::Server::instance().follow_settings();
}

}  // namespace genko::app::preferences
