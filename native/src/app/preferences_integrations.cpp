// 環境設定 → 連携 (PC5 J-g, edit.preferences.integrations; SPEC.md PROD-01「制御チャンネル」): the control channel turned on
// or off (integrations.controlChannel, off when the file has none: a row of core/prefs_table, made and written as H-e's
// panes are — pane_row, save_pane_fields; この面を初期設定に戻す puts it back off), what it is and what it never reaches, and
// whether it listens now (or why it cannot). After 決める, preferences::apply_all follows it (control::Server::follow_settings).

#include <QLabel>

#include "app/control_channel.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"

namespace genko::app::preferences {

QWidget* integrations_page(QWidget* parent) {
    (void)parent;
    auto* about = new QLabel(QStringLiteral(
        "このパソコンのプログラム（AI のエージェントなど）が、トークンで名乗って、動いている Genko の画面を見て、前の原稿に操作を当てます。"
        "この利用者だけがつなげるローカルソケットで待ちます（ネットワークのポートは開きません）。"
        "トークンは genko token add --actor ai:<名前> で作り、そのトークンの名前で原稿の記録に残ります。"
        "当てられる操作と断りは MCP の apply_ops と同じです（ページの鍵・承認後の決まり・人だけの操作は断ります）。"
        "承認・プラグインの同意・AI 接続・環境設定・トークンには届きません。ファイルを開く・保存することもありません。"
        "ペンで描いている間は、この画面の操作を止めて状態の欄に出します（Esc で止められます）。"
        "入れる・切るのは、この面か genko prefs set integrations.controlChannel（人のコマンドライン）だけです。"));
    about->setWordWrap(true);
    theme::role(about, "hint");
    const control::Server& server = control::Server::instance();
    auto* state = new QLabel(server.listening()             ? QStringLiteral("いまは待っています（%1）").arg(server.words())
                             : !server.problem().isEmpty() ? QStringLiteral("入れてありますが、待てません: %1").arg(server.problem())
                                                           : QStringLiteral("いまは切っています"));
    state->setObjectName(QStringLiteral("prefs_control_state"));
    state->setWordWrap(true);
    return form_page({{QStringLiteral("制御チャンネル"), {pane_row("integrations.controlChannel"), {QString(), about}, {QStringLiteral("状態"), state}}}});
}

}  // namespace genko::app::preferences
