// 環境設定's panes (PC5 H-e, edit.preferences.general / interface / tools / historyLog / performance / scratchDisks /
// enhancedControls and workspace's two; PhotoCraft v0.2.0's prefs.rs and prefs_ui.rs, MIT; SPEC VIEW-01): the pages in
// Photoshop's order, a field made from a row of core/prefs_table, この面を初期設定に戻す and すべての環境設定をリセット…, and what
// the preferences H-e added do in the window.

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/channel_panel.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/history_log.hpp"
#include "app/history_panel.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/memory.hpp"
#include "app/preferences.hpp"
#include "app/session_file.hpp"
#include "app/theme.hpp"
#include "app/tiles.hpp"
#include "core/paths.hpp"
#include "core/prefs_table.hpp"
#include "render/plugins.hpp"

namespace genko::app {

namespace preferences {

namespace prefs = core::prefs;
using core::Json;

namespace {

// The words of a choice's values (Photoshop's), where they are not the values themselves.
const std::map<std::string, std::map<std::string, QString>>& choice_words() {
    static const std::map<std::string, std::map<std::string, QString>> all{
        {"general.colorPicker", {{"system", QStringLiteral("パソコンの窓")}, {"genko", QStringLiteral("Genko の窓")}}},
        {"general.imageInterpolation",
         {{"nearest", QStringLiteral("ニアレストネイバー法（ハードな輪郭）")}, {"bilinear", QStringLiteral("バイリニア法")},
          {"bicubic", QStringLiteral("バイキュービック法（滑らかなグラデーション）")}}},
        {"interface.uiScale", {{"auto", QStringLiteral("自動")}, {"100", QStringLiteral("100 %")}, {"200", QStringLiteral("200 %")}}},
        {"interface.canvasBorder", {{"dropShadow", QStringLiteral("ドロップシャドウ")}, {"line", QStringLiteral("ライン")}, {"none", QStringLiteral("なし")}}},
        {"historyLog.destination", {{"metadata", QStringLiteral("メタデータ")}, {"textFile", QStringLiteral("テキストファイル")}, {"both", QStringLiteral("両方")}}},
        {"historyLog.detail", {{"sessionsOnly", QStringLiteral("セッションのみ")}, {"concise", QStringLiteral("簡潔")}, {"detailed", QStringLiteral("詳細")}}},
    };
    return all;
}

// What a field does (its tooltip).
const std::map<std::string, QString>& tips() {
    static const std::map<std::string, QString> all{
        {"general.colorPicker", QStringLiteral("色を選ぶ窓: パソコンの窓（Windows・Linux のもの）か、どのパソコンでも同じ Genko の窓")},
        {"general.imageInterpolation", QStringLiteral("選択範囲を変形するときの補間方法の初めの値（次に開いた窓から。変形中は オプション で変えられます）")},
        {"general.autoShowHomeScreen", QStringLiteral("原稿を渡さずに起動したとき、はじめの画面を出します。切ると、新しい原稿の窓がすぐに開きます（次に起動したときから）")},
        {"general.beepWhenDone", QStringLiteral("5 秒以上かかったフィルター・書き出しが終わったら、パソコンの音で知らせます")},
        {"general.exportClipboard", QStringLiteral("Genko を終えたとき、Genko でコピーした絵をほかのアプリに貼り付けられるように残します。切ると、終えるときに消します")},
        {"general.resizeImageDuringPlace", QStringLiteral("埋め込みを配置… で、ページより大きな画像をページに収まる大きさで置きます。切ると、画像の解像度のままの大きさで置きます")},
        {"general.zoomWithScrollWheel", QStringLiteral("マウスのホイールだけで拡大・縮小します（Shift とホイールで横に動かす）。切ると、Ctrl とホイールで拡大・縮小します")},
        {"general.animatedZoom", QStringLiteral("拡大・縮小をなめらかに動かします（動きを減らす のときは動かしません）")},
        {"interface.uiScale", QStringLiteral("画面の文字・ボタンの大きさ。自動はパソコンの表示の設定に合わせます（次に起動したときから）")},
        {"interface.canvasBorder", QStringLiteral("原稿の用紙のまわりの縁: 影・細い線・なし")},
        {"interface.showTooltips", QStringLiteral("ボタンやメニューにカーソルを置いたときの説明を出します")},
        {"interface.showMenuColors", QStringLiteral("編集 → メニュー… で付けた色をメニューに出します（切っても色は残ります）")},
        {"interface.showChannelsInColor", QStringLiteral("チャンネル パネルの小さな絵を、チャンネルの色で出します（グレーの代わりに）")},
        {"tools.showTooltips", QStringLiteral("ツール パネルの道具にカーソルを置いたときの説明を出します")},
        {"tools.zoomClickedPointToCenter", QStringLiteral("ズームツールのクリックで、クリックした所を窓の真ん中に寄せて拡大・縮小します")},
        {"tools.enableFlickPanning", QStringLiteral("手のひらツールで動かしながら離すと、原稿が少し滑って止まります")},
        {"tools.showTransformationValues", QStringLiteral("自由変形の枠を動かしている間、動かした量・回転・幅と高さをカーソルのそばに出します")},
        {"tools.overscroll", QStringLiteral("原稿の端を越えてスクロールできます。切ると、原稿が窓より大きいときは端で止まり、小さいときは真ん中に置きます")},
        {"historyLog.enabled", QStringLiteral("原稿への編集を記録します（このパソコンのアプリで。原稿には残しません）")},
        {"historyLog.destination", QStringLiteral("メタデータ: 書き出すファイルに編集の記録を入れます。テキストファイル: 下のファイルに 1 行ずつ足します")},
        {"historyLog.filePath", QStringLiteral("記録を足していくテキストファイル（原稿のフォルダーの中・原稿が使うファイルには書きません）")},
        {"historyLog.detail", QStringLiteral("セッションのみ: 原稿を開いた・閉じたとき。簡潔: 編集ごとに 1 行。詳細: 編集の操作（op）の名前も")},
        {"performance.memoryUsageMb", QStringLiteral("拡大したときの細かい表示の絵を、窓ごとにこの量まで覚えておきます（超えると見えていない所から外します）。"
                                                     "描く処理の上限は変わりません")},
        {"performance.historyStates", QStringLiteral("ヒストリー パネルに並べる最近の編集の数（古いものも原稿には残り、取り消しで戻れます）")},
        {"scratchDisks.folder", QStringLiteral("縮小図・表紙の覚えを置くフォルダー（空: 設定のフォルダーの中）。Genko は画素をディスクに逃がさないので、"
                                               "ここは覚えの置き場所だけです。決めるとすぐ効きます（ページ パネルの縮小図は次に開く窓から）。"
                                               "原稿のフォルダーの中・書き込めない所は選べません")},
        {"enhancedControls.touchGestures", QStringLiteral("タッチ画面の 2 本の指で、拡大・回転・移動をします")},
        {"enhancedControls.zoomWithTrackpadPinch", QStringLiteral("トラックパッドのピンチで拡大・縮小します")},
        {"enhancedControls.rotateViewWithTrackpad", QStringLiteral("トラックパッドで 2 本の指を回して、表示を回転します")},
        {"workspace.openDocumentsAsTabs", QStringLiteral("開いた原稿を今の窓のタブにします。切ると、原稿ごとに窓を開きます")},
        {"workspace.enableFloatingDocumentWindowDocking",
         QStringLiteral("分離した原稿の窓（原稿が 1 つの窓）を、ほかの窓のタブの所までドラッグして離すと、その窓のタブになります")},
    };
    return all;
}

const char* const kPref = "pref";

QWidget* scrolled(QWidget* inside) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(inside);
    return scroll;
}

// (the panes' tooltips: off, none shown — 環境設定 → インターフェイス; the tools' alone off — 環境設定 → ツール)
class TipFilter : public QObject {
public:
    using QObject::QObject;
    bool all = true;
    bool tools = true;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::ToolTip) return false;
        if (!all) return true;
        if (tools) return false;
        auto* button = qobject_cast<QAbstractButton*>(watched);
        const auto* tool = qobject_cast<QToolButton*>(watched);
        if (button == nullptr || tool == nullptr || tool->defaultAction() == nullptr) return false;
        for (MainWindow* window : documents::windows()) {
            for (const auto& [name, act] : window->tool_actions()) {
                if (act == tool->defaultAction()) return true;
            }
        }
        return false;
    }
};

TipFilter* tip_filter() {
    static QPointer<TipFilter> filter;
    if (filter == nullptr && QCoreApplication::instance() != nullptr) {
        filter = new TipFilter(QCoreApplication::instance());
        QCoreApplication::instance()->installEventFilter(filter);
    }
    return filter;
}

// (a window moved by its title bar and let go over another's tabs: looked at once it stops moving — the window manager
// moves it, the app sees no release —; フローティングドキュメントウィンドウのドッキングを有効にする)
class DockWatch : public QObject {
public:
    explicit DockWatch(MainWindow* window) : QObject(window), window_(window) {
        still_.setSingleShot(true);
        still_.setInterval(350);
        connect(&still_, &QTimer::timeout, this, [this] {
            // (only a window the person moved by its title bar — the pointer still on it, above the window's inside —; a
            // window the app placed, or moved by the keys, never)
            const QPoint at = QCursor::pos();
            if (window_ != nullptr && window_->frameGeometry().contains(at) && at.y() < window_->geometry().top()) (void)window_->dock_over(at);
        });
        window->installEventFilter(this);
    }

    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Move && window_ != nullptr && window_->isVisible() && window_->documents().size() == 1) still_.start();
        return false;
    }

private:
    QPointer<MainWindow> window_;
    QTimer still_;
};

}  // namespace

QWidget* form_page(const FormSections& sections) {
    auto* page = new QWidget;
    QFormLayout* rows = look::form();
    page->setLayout(rows);
    for (const auto& [head, fields] : sections) {
        if (!head.isEmpty()) rows->addRow(look::section(head));
        for (const auto& [label, widget] : fields) rows->addRow(label, widget);
    }
    look::quiet_labels(rows);
    return scrolled(page);
}

std::pair<QString, QWidget*> pane_row(const char* path) {
    const prefs::Pref* pref = prefs::by_path(path);
    if (pref == nullptr) return {QString(), new QLabel(QString::fromUtf8(path))};  // (never: a path of the table)
    const auto store = settings();
    const Json value = prefs::value(*store, path);
    const QString label = QString::fromStdString(pref->label);
    const QString name = QStringLiteral("prefs_") + QString::fromUtf8(path);
    const auto tip = tips().find(path);
    QWidget* field = nullptr;
    QWidget* shown = nullptr;  // (what the row holds: the field, or the field with its button)
    switch (pref->kind) {
        case prefs::Kind::Boolean: {
            auto* box = new QCheckBox(label);
            box->setChecked(value.is_boolean() && value.get<bool>());
            field = box;
            break;
        }
        case prefs::Kind::Choice: {
            auto* box = new QComboBox;
            const auto words = choice_words().find(path);
            for (const std::string& choice : pref->choices) {
                QString shown_words = QString::fromStdString(choice);
                if (words != choice_words().end()) {
                    if (const auto w = words->second.find(choice); w != words->second.end()) shown_words = w->second;
                }
                box->addItem(shown_words, QString::fromStdString(choice));
            }
            box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(value.is_string() ? value.get<std::string>() : pref->fallback))));
            field = box;
            break;
        }
        case prefs::Kind::Integer: {
            auto* spin = new QSpinBox;
            spin->setRange(static_cast<int>(pref->min), static_cast<int>(pref->max));
            spin->setValue(value.is_number() ? value.get<int>() : 0);
            if (std::string_view(path).ends_with("Mb")) spin->setSuffix(QStringLiteral(" MB"));
            if (std::string_view(path) == "performance.historyStates") spin->setSuffix(QStringLiteral(" 件"));
            field = spin;
            break;
        }
        case prefs::Kind::Number: {
            auto* spin = new QDoubleSpinBox;
            spin->setRange(pref->min, pref->max);
            spin->setValue(value.is_number() ? value.get<double>() : 0.0);
            field = spin;
            break;
        }
        default: {  // (Path, Text: a line; a path with 選ぶ…)
            auto* line = new QLineEdit(QString::fromStdString(value.is_string() ? value.get<std::string>() : std::string()));
            field = line;
            if (pref->kind == prefs::Kind::Path) {
                const bool folder = std::string_view(path) == "scratchDisks.folder";
                line->setPlaceholderText(folder ? QStringLiteral("設定のフォルダーの中") : QStringLiteral("なし"));
                auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
                pick->setObjectName(name + QStringLiteral("_pick"));
                QObject::connect(pick, &QPushButton::clicked, line, [line, folder, label] {
                    const QString chosen = folder ? ask::existing_dir(line, label, line->text())
                                                  : ask::save_path(line, label, line->text().isEmpty() ? QStringLiteral("Genko ヒストリーログ.txt") : line->text(),
                                                                   QStringLiteral("テキスト (*.txt *.log)"));
                    if (!chosen.isEmpty()) line->setText(chosen);
                });
                shown = new QWidget;
                auto* row = new QHBoxLayout(shown);
                row->setContentsMargins(0, 0, 0, 0);
                row->addWidget(line, 1);
                row->addWidget(pick);
            }
            break;
        }
    }
    field->setObjectName(name);
    field->setProperty(kPref, QString::fromUtf8(path));
    if (tip != tips().end()) field->setToolTip(tip->second);
    if (shown == nullptr) shown = field;
    return {pref->kind == prefs::Kind::Boolean ? QString() : label, shown};
}

namespace {

Json pane_values(QWidget* root) {
    Json values = Json::object();
    for (QWidget* field : root->findChildren<QWidget*>()) {
        const QString path = field->property(kPref).toString();
        if (path.isEmpty()) continue;
        Json value;
        if (auto* box = qobject_cast<QCheckBox*>(field)) value = box->isChecked();
        else if (auto* choice = qobject_cast<QComboBox*>(field)) value = choice->currentData().toString().toStdString();
        else if (auto* spin = qobject_cast<QSpinBox*>(field)) value = spin->value();
        else if (auto* number = qobject_cast<QDoubleSpinBox*>(field)) value = number->value();
        else if (auto* line = qobject_cast<QLineEdit*>(field)) value = line->text().trimmed().toStdString();
        else continue;
        values[path.toStdString()] = std::move(value);
    }
    return values;
}

}  // namespace

bool pane_fields_fit(QWidget* root) {
    const Json values = pane_values(root);  // (held: items() of a temporary would dangle)
    for (const auto& [path, value] : values.items()) {
        const prefs::Pref* pref = prefs::by_path(path);
        if (pref == nullptr) continue;
        if (const auto wrong = prefs::check(*pref, value)) {
            ask::warning(root, QStringLiteral("環境設定"), QString::fromStdString(*wrong));
            return false;
        }
    }
    return true;
}

bool save_pane_fields(QWidget* root) {
    const Json values = pane_values(root);
    if (values.empty()) return true;
    const auto store = settings();
    const prefs::Outcome out = prefs::set(*store, values);
    if (!out.ok) ask::warning(root, QStringLiteral("環境設定"), QString::fromStdString(out.message));
    return out.ok;
}

void arrange_pages(PreferencesDialog* dialog) {
    QTabWidget* tabs = dialog->tabs;
    std::vector<std::pair<QWidget*, QString>> pages;
    for (int i = 0; i < tabs->count(); ++i) pages.emplace_back(tabs->widget(i), tabs->tabText(i));
    while (tabs->count() > 0) tabs->removeTab(0);
    std::vector<std::pair<QWidget*, QString>> ordered;
    std::map<QString, QString> pane_of;  // (a page's title: its pane's id)
    for (const Pane& pane : panes()) {
        if (pane.page.isEmpty() || pane_of.contains(pane.page)) continue;
        pane_of[pane.page] = pane.id;
        for (const auto& page : pages) {
            if (page.second == pane.page) ordered.push_back(page);
        }
    }
    for (const auto& page : pages) {  // (a page no pane names: after them, as it came)
        if (!pane_of.contains(page.second)) ordered.push_back(page);
    }
    for (const auto& [widget, title] : ordered) {
        const auto pane = pane_of.find(title);
        // (プラグイン: what the plugins may do is put back in their own window, never by this button — J-f's review P3-6)
        if (pane == pane_of.end() || pane->second == QLatin1String("plugIns")) {
            tabs->addTab(widget, title);
            continue;
        }
        auto* holder = new QWidget;
        auto* column = new QVBoxLayout(holder);
        column->setContentsMargins(0, 0, 0, 0);
        column->addWidget(widget, 1);
        auto* reset = new QPushButton(QStringLiteral("この面を初期設定に戻す"));
        reset->setObjectName(QStringLiteral("prefs_reset_") + pane->second);
        reset->setToolTip(QStringLiteral("この面の環境設定を初めの値に戻します（ほかの面で変えたものは、決めてから戻します）"));
        QObject::connect(reset, &QPushButton::clicked, dialog, [dialog, id = pane->second] { dialog->reset_pane(id); });
        auto* line = new QHBoxLayout;
        line->addStretch(1);
        line->addWidget(reset);
        column->addLayout(line);
        tabs->addTab(holder, title);
    }
    tabs->setCurrentIndex(0);
}

std::vector<std::string> pane_paths(const QString& pane) {
    const std::string id = pane.toStdString();
    if (id == "fileHandling") return {"fileHandling", "book"};
    // (a section put back passes over the person's own lists — the quick access, the command bar, the type's dictionary and
    // table: Pref::kept_on_reset, review P2-3 —; the workspace in use and the stage are not this pane's either: kept)
    if (id == "workspace") {
        std::vector<std::string> out;
        for (const prefs::Pref& p : prefs::table()) {
            if (p.path.rfind("workspace.", 0) == 0 && !p.kept_on_reset && p.path != "workspace.current" && p.path != "workspace.stage" &&
                p.path.find('*') == std::string::npos) {
                out.push_back(p.path);
            }
        }
        return out;
    }
    return {id};
}

QWidget* plugins_page(MainWindow* window) {
    auto* status = new QLabel;
    status->setObjectName(QStringLiteral("prefs_plugins_status"));
    status->setWordWrap(true);
    const auto show = [status] {
        const render::plugins::Settings now = render::plugins::settings();
        const QString folder = now.use_extra_folder && !now.extra_folder.empty() ? QString::fromStdString(now.extra_folder) : QStringLiteral("使わない");
        status->setText(QStringLiteral("プラグイン: %1　動かしてよいもの: %2 個\n追加のプラグインフォルダー: %3")
                            .arg(now.enabled ? QStringLiteral("使う") : QStringLiteral("使わない"))
                            .arg(now.chosen.is_object() ? static_cast<int>(now.chosen.size()) : 0)
                            .arg(folder));
    };
    show();
    auto* open = new QPushButton(QStringLiteral("プラグイン…"));
    open->setObjectName(QStringLiteral("prefs_plugins_window"));
    open->setToolTip(QStringLiteral("プラグインを使うか、追加のプラグインフォルダー、どれを動かしてよいかを決める窓を開きます"));
    QObject::connect(open, &QPushButton::clicked, status, [window, show] {
        if (QAction* plugins = window != nullptr ? window->action(QStringLiteral("act_plugin_settings")) : nullptr) plugins->trigger();
        show();
    });
    auto* hint = new QLabel(QStringLiteral("プラグインを使うか・どれを動かしてよいか（同意）は、プラグイン… の窓でだけ決めます。"
                                           "genko prefs や AI（MCP）からは読めず、変えられません。"));
    hint->setWordWrap(true);
    theme::role(hint, "hint");
    return form_page({{QStringLiteral("プラグイン"), {{QString(), status}, {QString(), open}, {QString(), hint}}}});
}

QLabel* scratch_note() {
    const QString folder = QString::fromStdString(core::path_to_utf8(cache_root()));
    const QStorageInfo disk(folder);
    const double free_gb = disk.isValid() ? static_cast<double>(disk.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0) : 0.0;
    auto* note = new QLabel(QStringLiteral("今の置き場所: %1（空き %2 GB）。Genko は画素をディスクに逃がしません: 大きな原稿の画素は原稿のフォルダーとメモリーにあり、"
                                           "処理の上限（2 GiB）と高精度の上限（1 GiB）で守られます。")
                                .arg(QDir::toNativeSeparators(folder), QString::number(free_gb, 'f', 1)));
    note->setObjectName(QStringLiteral("prefs_scratch_note"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    return note;
}

void apply_panes(MainWindow* window) {
    const auto store = settings();
    const auto on = [&store](const char* path) {
        const Json v = prefs::value(*store, path);
        return v.is_boolean() && v.get<bool>();
    };
    const auto text = [&store](const char* path) {
        const Json v = prefs::value(*store, path);
        return v.is_string() ? v.get<std::string>() : std::string();
    };
    PageCanvas* canvas = window->canvas();
    canvas->wheel_zooms = on("general.zoomWithScrollWheel");
    canvas->animated_zoom = on("general.animatedZoom");
    canvas->zoom_to_center = on("tools.zoomClickedPointToCenter");
    canvas->flick_panning = on("tools.enableFlickPanning");
    canvas->transform_values = on("tools.showTransformationValues");
    canvas->overscroll = on("tools.overscroll");
    canvas->touch_gestures = on("enhancedControls.touchGestures");
    canvas->pinch_zoom = on("enhancedControls.zoomWithTrackpadPinch");
    canvas->trackpad_rotate = on("enhancedControls.rotateViewWithTrackpad");
    canvas->page_border = QString::fromStdString(text("interface.canvasBorder"));
    canvas->keep_in_view();
    // (パフォーマンス → 使用するメモリ: the finer tiles the window keeps — the ops' own budgets stay as they are)
    const std::size_t budget = static_cast<std::size_t>(prefs::value(*store, "performance.memoryUsageMb").get<std::int64_t>()) * 1024 * 1024;
    if (window->property("genko_fine_budget").toULongLong() != budget) {
        window->setProperty("genko_fine_budget", QVariant::fromValue<qulonglong>(budget));
        canvas->renderer().set_fine_budget(budget);
        memory::give_back();  // (made smaller: the room let go goes back to the computer — app/memory)
    }
    if (HistoryPanel* history = window->history(); history != nullptr) {
        const int states = static_cast<int>(prefs::value(*store, "performance.historyStates").get<std::int64_t>());
        if (history->states != states) {
            history->states = states;
            history->refresh();
        }
    }
    if (ChannelPanel* channels = window->channel_panel(); channels != nullptr) {
        const bool coloured = on("interface.showChannelsInColor");
        if (channels->in_colour != coloured) {
            channels->in_colour = coloured;
            channels->refresh();
        }
    }
    if (TipFilter* tips = tip_filter(); tips != nullptr) {
        tips->all = on("interface.showTooltips");
        tips->tools = on("tools.showTooltips");
    }
    if (!window->property("genko_panes_applied").toBool()) {  // (once for a window: what it starts with, and what it watches)
        window->setProperty("genko_panes_applied", true);
        window->set_default_interpolation(text("general.imageInterpolation"));
        new DockWatch(window);
    }
    // (ヒストリーログ: the window's books followed — the one it opened with too; a book shown later, as it comes to the front)
    for (const Document& d : window->documents()) history_log::follow(d.session.get(), d.source);
    static bool once = false;
    if (!once && QCoreApplication::instance() != nullptr) {
        once = true;
        // (クリップボードの書き出し off: what Genko put on the computer's clipboard taken away as it quits)
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
            if (prefs::value("general.exportClipboard").get<bool>()) return;
            if (QClipboard* clip = QGuiApplication::clipboard(); clip != nullptr && clip->ownsClipboard()) clip->clear();
        });
        history_log::install();
        session_file::start();
    }
}

void done_after(qint64 elapsed_ms) {
    if (elapsed_ms < 5000) return;
    if (prefs::value("general.beepWhenDone").get<bool>()) QApplication::beep();
}

void PreferencesDialog::reset_pane(const QString& pane) {
    QString words = pane;
    for (const Pane& p : panes()) {
        if (p.id == pane) words = p.page;
    }
    if (!ask::question(this, QStringLiteral("この面を初期設定に戻す"),
                       QStringLiteral("「%1」の環境設定を初めの値に戻しますか？（ほかの面で変えたものは、先に決めます）").arg(words))) {
        return;
    }
    save();  // (what is chosen on the other panes kept first)
    if (result() != QDialog::Accepted) return;
    {
        const auto store = settings();
        for (const std::string& path : pane_paths(pane)) (void)prefs::reset(*store, path);
    }
    for (MainWindow* open : documents::windows()) apply_all(open);
    reopen = pane;
}

void PreferencesDialog::reset_everything() {
    if (!ask::question(this, QStringLiteral("すべての環境設定をリセット"),
                       QStringLiteral("このパソコンの環境設定をすべて初めの値に戻しますか？（キー・メニュー・ツールバーも。道具の覚え・残したワークスペース・クイックアクセスとコマンドバーの並び・ユーザー辞書と表記の表は残ります）"))) {
        return;
    }
    save();
    if (result() != QDialog::Accepted) return;
    {
        const auto store = settings();
        (void)prefs::reset(*store);
    }
    for (MainWindow* open : documents::windows()) apply_all(open);
    reopen = QStringLiteral("general");
}

}  // namespace preferences

// --- the window (PC5 H-e: 環境設定 → ワークスペース) -----------------------------------------------------------------------

MainWindow* MainWindow::window_to_open_in() {
    if (core::prefs::value("workspace.openDocumentsAsTabs").get<bool>()) return this;
    const bool untouched = documents_.size() == 1 && !documents_.front().picture && !session_->path() && session_->generation() == 0 &&
                           session_->waiting() == 0;
    if (documents_.empty() || untouched) return this;
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    window->show();
    window->move(pos() + QPoint(40, 40));
    return window;
}

bool MainWindow::dock_over(const QPoint& global) {
    if (documents_.size() != 1 || !core::prefs::value("workspace.enableFloatingDocumentWindowDocking").get<bool>()) return false;
    for (MainWindow* other : documents::windows()) {
        if (other == this || !other->isVisible()) continue;
        // (its tabs' strip: the tab bar across the page's top — or, with one tab and no bar shown, the page's top)
        QWidget* page = other->centralWidget();
        if (page == nullptr) continue;
        const QPoint top = page->mapToGlobal(QPoint(0, 0));
        const int high = other->doc_tabs_ != nullptr && other->doc_tabs_->isVisible() ? std::max(other->doc_tabs_->height(), 24) : 32;
        const QRect strip(top, QSize(page->width(), high));
        if (!strip.contains(global)) continue;
        finish_typing();
        store_document();
        const Document going = documents_.front();
        // (the same book shown there already — 新規ウィンドウ's other window —: not twice, as すべてをタブに統合 does)
        const bool there = std::any_of(other->documents_.begin(), other->documents_.end(),
                                       [&going](const Document& d) { return d.session == going.session && d.contents == going.contents; });
        if (!there) other->take_document(going);
        other->mark_tabs();
        other->raise();
        other->activateWindow();
        close();  // (its book is open there: closed without a question)
        return true;
    }
    return false;
}

}  // namespace genko::app
