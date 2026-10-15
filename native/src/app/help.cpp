// ヘルプ (Python's genko/app/help.py).

#include "app/help.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <functional>
#include <optional>

#include "core/version.hpp"

namespace genko::app::help {

const char* const kGuide = R"HTML(
<h2>はじめての 1 冊</h2>
<p>Genko は、ネームから入稿まで 1 つの窓で描けるマンガの原稿道具です。左端が道具、その右が持っている道具の設定
（選んだ台詞の文字やフキダシの設定もここ）と全体図、右側がレイヤー・台詞・ページ・カラーのタブです。テキストの道具を持つと台詞、
描く道具を持つとレイヤーのタブが前に出ます。履歴・素材・点検などは{top:ウィンドウ}から開くと、同じタブの列に並びます。
描いた線は「ペン入れ」レイヤーに入り、そのまま印刷されます。
道具がアイコンだけで分かりにくいときは{menu:act_tool_names}で名前も並べられます。</p>
<h3>1. 原稿を作る</h3>
<p>{menu:act_new}で作品名・ページ数・原稿用紙（B4 投稿・同人誌 B5／A5 など）を選びます。
用紙は後から{menu:act_paper}で変えられます（コマや台詞も合わせて動きます）。</p>
<h3>2. コマを割る</h3>
<p>コマ割りの道具（{key:act_frame}）で、コマの中を横切るようにドラッグすると割れます。間の白をドラッグで幅、角をドラッグで形を変えます。
{menu:act_template}で定番の割り方も選べます。</p>
<h3>3. ネーム・下描き</h3>
<p>レイヤー パネルで「ネーム」を選び、ペン（{key:act_pen}）で描きます。レイヤーを足して「下描き（書き出さない）」にすると、
印刷には出ない下描きになります。表示色を青にすると見分けやすくなります。</p>
<h3>4. ペン入れ</h3>
<p>「ペン入れ」レイヤーを選び、ブラシ（G ペン・丸ペン・筆…）と太さを選んで描きます。[ と ] で太さ、Shift で直線、
消しゴム（{key:act_eraser}）は線を触れた所で切ります。ペンの後ろ側でなぞっても消えます。回して描きたいときは{menu:act_turn_right}、
形の狂いは{key:act_mirror}（{menu:act_mirror}）で確かめます。細かい所は Ctrl＋ホイールで拡大します。拡大すると一瞬あとに細かい描画に替わり、
線がくっきりします。左の「全体図」をクリック・ドラッグすると、その場所へ飛べます。</p>
<p>レイヤーごと動かす・大きさを変えるのは「レイヤー移動」（{key:act_move}）。広い所に色の濃淡を付けるのは「グラデーション」（{key:act_gradient}）で、
ドラッグした向きに変わります。</p>
<h3>5. 写植（台詞）</h3>
<p>テキストの道具（{key:act_text}）で置きたい所をクリックして打ちます。ルビは ｜約束《やくそく》、傍点は 《《強調》》、
一部だけ大きく・太くするのは {大|ドン}・{太|本当に}（ほかに {特大|…}・{小|…}・{赤|…}）。
フキダシの形・しっぽ・回転は、選択ツール（{key:act_select}）でフキダシをクリックし、左の「ツールの設定」に出る欄とハンドルで変えます。
形は、普通・角丸・雲・心の声・叫び（トゲ）・フラッシュ・電子音・ささやきなど。線を揺らす・二重線にするのは設定の欄で。フキダシペンで好きな形に描くこともできます。</p>
<h3>6. トーン・効果線・仕上げ</h3>
<p>範囲を選んで{menu:act_tone_here}、効果線の道具（{key:act_effect}）でコマをクリックすると集中線などが入ります。
投げ縄などで範囲を選んでからクリックすると、その中にだけ描きます。顔や人物を投げ縄で囲んで効果線を選び、
道具の設定の「選択範囲を避ける」を押すと、線がその手前で細くなって止まります。
素材パネルからトーン・効果線・自分の素材を貼れます。</p>
<h3>7. 点検して書き出す</h3>
<p>{menu:act_checks}（{key:act_checks}）で、はみ出し・文字の小ささなどを探します。
{menu:act_export}で PDF・TIFF・CMYK・PSD・レイヤーごとの PNG・Kindle・縦読み・SNS 用などに書き出します。PSD は{menu:act_import_psd}で読み込めます。制作過程は{menu:act_timelapse}で残せます。書き出す前にも点検が行われます。
{menu:act_timeline}で、ページを短いアニメーションにできます（セル・タイムライン・前後のセルを透かす・カメラワーク・GIF や MP4 に書き出し）。
{menu:act_open}で開いた原稿はタブに並び、{key:act_next_doc}で切り替えます。{menu:act_new_window}で、拡大して描く窓と全体を見る窓を並べられます。
紙で確かめたいときは{menu:act_print}（{key:act_print}）。</p>
<h3>8. AI と一緒に描く（使う人だけ）</h3>
<p>Claude Code などの AI は、MCP でこの原稿を開き、人と同じ道具（線・塗り・トーン・台詞・点検・書き出し）を使えます。
つなぎ方は{menu:act_ai}（状態の行の「AI と作る」でも開きます）。使う AI を選ぶと、その AI の設定に貼る文が
このパソコンの場所入りで出るので、「設定をコピー」して貼り、AI を立ち上げ直します。
AI が作った本では、ネームや絵の承認は人が行います（承認を待つものがあると、右上に「承認待ち」が出ます。下の「承認箱」の数を押しても出ます）。
AI が承認の依頼を出すと、原稿の下に小さな知らせが出て承認箱が前に出ます（環境設定で、前に出さないこともできます）。
AI に 1 つのコマを伝えるときは、コマを右クリックして「AI 用の参照をコピー」を貼ります。</p>
<h3>9. 長い時間の作業に</h3>
<p>原稿の上で {key:act_canvas_only} を押すと、パネルが全部隠れて原稿だけになります（{menu:act_canvas_only}）。左右の端にカーソルを寄せると、その側のパネルが出ます。
{submenu:作業の段階}で、ネーム・作画・仕上げ・写植・承認のそれぞれに合うパネルの並びにできます。
ペンなどで右クリックすると、道具を持ち替える円形のメニューが出ます（Shift＋右クリックでふだんのメニュー）。
縦読みの原稿では、スマホ 1 画面に入る範囲が出ます（{menu:act_phone}）。
休憩の案内・動きを減らす・画面の文字の大きさ・画面の色と明るさは、{menu:act_prefs}で選べます。</p>
)HTML";

const char* const kFaq = R"HTML(
<h2>困ったとき</h2>
<h3>線が描けない</h3>
<p>下の欄に「描けません」と出ていないか見ます。レイヤー パネルで選んでいるレイヤーがロックされているか、
ペンかペイントのレイヤーでないと描けません。マスクの編集中だと、ペンはマスクを描きます（{menu:マスクを編集する（ペンで見せる・消しゴムで隠す）}）。</p>
<h3>描いた線が印刷に出ない</h3>
<p>そのレイヤーが「下描き（書き出さない）」になっていないか、ネームのレイヤーでないかを確かめます。
コマの外に描いた線は、レイヤーの「コマの外にもはみ出す」を入れないとコマで切れます。</p>
<h3>間違えた</h3>
<p>{key:act_undo} で戻り、{key:act_redo} でやり直します。{menu:act_history}（{key:act_history}）なら、前の時点をクリックして一度に戻れます。</p>
<h3>画面が回った・反転したまま</h3>
<p>{menu:act_turn_reset}（{key:act_turn_reset}）。</p>
<h3>パネルを閉じてしまった</h3>
<p>{top:ウィンドウ}から出し直せます。{submenu:ワークスペース}の「はじめの配置に戻す」で、最初の並びに戻ります。</p>
<h3>文字が枠からはみ出す・小さすぎる</h3>
<p>{menu:act_checks}（{key:act_checks}）で場所が分かります。台詞パネルで文字の大きさを「自動」にすると、フキダシに合わせて縮みます。</p>
<h3>ほかのパソコンで開いたら線の見た目が違う</h3>
<p>自作のブラシは原稿の中にも写るので、同じに描かれます。書体は、パソコンの書体を使ったときだけ、そのパソコンにも同じ書体が要ります。</p>
<h3>拡大すると一瞬ぼやける・初めて開いたページの線が細い</h3>
<p>厚い原稿でも待たずに描けるよう、まず軽く描いてから、すぐ後に細かく描き直します。少し待つとくっきりします。</p>
<h3>道具がどれか分からない</h3>
<p>{menu:act_tool_names}で、アイコンの横に名前が出ます。道具にマウスを重ねると、キーと使い方も出ます。{menu:act_find_command}（{key:act_find_command}）で、名前の一部からコマンドを探せます。</p>
<h3>台詞の文字の設定が見当たらない</h3>
<p>選択ツール（{key:act_select}）で台詞をクリックすると、左の「ツールの設定」に文字とフキダシの設定が出ます。</p>
<h3>キーを変えたい・筆圧が合わない</h3>
<p>{menu:act_prefs}でショートカットを変え、ペンタブレットの試し書きで筆圧を合わせます。</p>
)HTML";

namespace {

QString words(const QAction* action) { return QString(action->text()).remove(QLatin1Char('&')); }

// Every command of the menus (by its window's attribute name "act_x" when it has one, else by its words), with its path.
struct Found {
    QAction* action = nullptr;
    QStringList path;  // the menus above it, from the menu bar
};

void walk(QMenu* menu, QStringList path, const std::function<void(QAction*, const QStringList&)>& each) {
    for (QAction* action : menu->actions()) {
        if (action->isSeparator() || action->text().isEmpty()) continue;
        if (action->menu() != nullptr) {
            each(action, path);
            walk(action->menu(), path + QStringList{words(action)}, each);
        } else {
            each(action, path);
        }
    }
}

std::optional<Found> find(QMainWindow* window, const QString& name, bool submenu) {
    // (an attribute name: the window's own command by it; else the words of a command or a menu)
    QAction* wanted = nullptr;
    if (name.startsWith(QStringLiteral("act_"))) {
        for (QAction* action : window->findChildren<QAction*>()) {
            if (action->property("genko_attribute").toString() == name) wanted = action;
        }
        if (wanted == nullptr) return std::nullopt;
    }
    std::optional<Found> found;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr || found) continue;
        walk(top->menu(), {words(top)}, [&](QAction* action, const QStringList& path) {
            if (found) return;
            const bool is_menu = action->menu() != nullptr;
            if (submenu != is_menu) return;
            if (wanted != nullptr ? action == wanted : words(action) == name) found = Found{action, path};
        });
    }
    return found;
}

QString keys_of(const QAction* action) {
    QStringList all;
    for (const QKeySequence& key : action->shortcuts()) all << key.toString(QKeySequence::NativeText);
    return all.join(QStringLiteral("・"));
}

}  // namespace

Rendered render(QMainWindow* window, const QString& text) {
    static const QRegularExpression reference(QStringLiteral("\\{(menu|key|submenu|top):([^}|]+)\\}"));
    Rendered out;
    QString html;
    qsizetype at = 0;
    for (auto it = reference.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        html += text.mid(at, m.capturedStart() - at);
        at = m.capturedEnd();
        const QString kind = m.captured(1);
        const QString name = m.captured(2);
        QString said;
        if (kind == QLatin1String("top")) {
            for (QAction* top : window->menuBar()->actions()) {
                if (top->menu() != nullptr && words(top) == name) said = QStringLiteral("「%1」メニュー").arg(name);
            }
        } else if (const auto found = find(window, name, kind == QLatin1String("submenu"))) {
            if (kind == QLatin1String("key")) {
                said = keys_of(found->action);
            } else {
                said = QStringLiteral("「%1」").arg((found->path + QStringList{words(found->action)}).join(QStringLiteral(" → ")));
            }
        }
        if (said.isEmpty()) {  // (not in the menus, or no keys: said as missing, never described as if it were)
            out.missing << m.captured(0);
            said = QStringLiteral("（%1）").arg(name);
        }
        html += said;
    }
    html += text.mid(at);
    out.html = html;
    return out;
}

QString guide(QMainWindow* window) { return render(window, QString::fromUtf8(kGuide)).html; }
QString faq(QMainWindow* window) { return render(window, QString::fromUtf8(kFaq)).html; }

QString about() {
    return QStringLiteral("<h2>Genko Studio</h2><p>版 %1</p><p>マンガの原稿を、ネームから入稿まで描く道具。AIと分担して進めることもできます。</p>")
        .arg(QString::fromUtf8(genko::version().data(), static_cast<qsizetype>(genko::version().size())));
}

std::vector<std::tuple<QString, QString, QString>> shortcut_rows(QMainWindow* window) {
    std::vector<std::tuple<QString, QString, QString>> out;
    QSet<QString> seen;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr) continue;
        walk(top->menu(), {top->text()}, [&](QAction* action, const QStringList& path) {
            if (action->menu() != nullptr) return;  // (the menus inside a menu: ツール → 定規, ページ → コマ…)
            QStringList keys;
            for (const QKeySequence& key : action->shortcuts()) keys << key.toString(QKeySequence::NativeText);
            if (!keys.isEmpty() && !seen.contains(action->text())) {
                seen.insert(action->text());
                out.emplace_back(path.join(QStringLiteral(" → ")), action->text(), keys.join(QStringLiteral(", ")));
            }
        });
    }
    return out;
}

QString shortcut_html(QMainWindow* window) {
    QString rows;
    QString last;
    for (const auto& [menu, command, keys] : shortcut_rows(window)) {
        if (menu != last) {
            rows += QStringLiteral("<tr><th colspan=2 align=left style='padding-top:8px'>%1</th></tr>").arg(menu);
            last = menu;
        }
        rows += QStringLiteral("<tr><td>%1</td><td><b>%2</b></td></tr>").arg(command, keys);
    }
    return QStringLiteral("<h2>ショートカット一覧</h2><p>ほかに: スペース＋ドラッグで表示を動かす、Shift＋スペース＋ドラッグで回す、"
                          "Ctrl＋ホイールで拡大縮小。キーは「環境設定」で変えられます。</p><table cellspacing=4>") +
           rows + QStringLiteral("</table>");
}

HelpDialog::HelpDialog(QWidget* parent, const QString& title, const QString& html) : QDialog(parent) {
    setWindowTitle(title);
    resize(620, 560);
    text = new QTextBrowser;
    text->setOpenExternalLinks(true);
    text->setHtml(html);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
}

}  // namespace genko::app::help
