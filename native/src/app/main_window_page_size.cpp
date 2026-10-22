// 判型 in the window (PC3 C-e, SPEC.md COLOR-07「判型」; Photoshop's Image menu, which is Genko's ページ): 画像解像度…,
// カンバスサイズ… (Ctrl+Alt+C), 画像の回転 ▸ (180°, 90°（時計回り）, 90°（反時計回り）, 角度入力…, カンバスを左右反転,
// カンバスを上下反転), 切り抜き (to the selection's box), トリミング…, すべての領域を表示; ツール → 切り抜きツール (Shift+C)
// and its settings. Each sends one op (resize_pages, set_canvas_size, rotate_pages, crop_page, trim_page, reveal_all),
// undone at once. When the finished size would not fit the new paper, the person is asked whether to fit it to the paper.
// A change of the whole book that left covers as they were (their paper set by their cover settings) says which.
// (Photoshop's Ctrl+Alt+I is 選択範囲を反転 here: 画像解像度… has no key.)

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <vector>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/page_size_dialogs.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/covers.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

// The refusals a fitted finished size answers (render/ops_pagesize's, in the person's words: service/wording).
bool trim_refused(const QString& error) {
    return error.contains(QStringLiteral("仕上がりと裁ち落としが新しい用紙に入りません")) || error.contains(QStringLiteral("基本枠が仕上がりに入りません"));
}

// Each page's paper (its number, whether it is a cover, its width and height): to tell which covers a change of the whole
// book left as they were (Image Size and Canvas Size leave out a cover their size would change — its paper is set by its
// cover settings —: render/ops_pagesize).
struct Paper {
    QString number;
    bool cover = false;
    double width = 0, height = 0;
};

std::vector<Paper> papers_of(const core::Document& doc) {
    std::vector<Paper> out;
    for (const auto& page : doc.pages) {
        out.push_back(Paper{QString::fromStdString(page->index.repr()), core::is_cover(*page), page->spec.width_mm.value(), page->spec.height_mm.value()});
    }
    return out;
}

// The covers whose paper stayed while a page that is not one changed its (their numbers, "・" between); "" when none.
QString covers_left(const std::vector<Paper>& before, const std::vector<Paper>& after) {
    if (before.size() != after.size()) return {};
    const auto same = [](const Paper& a, const Paper& b) { return std::abs(a.width - b.width) < 1e-6 && std::abs(a.height - b.height) < 1e-6; };
    bool changed = false;
    QStringList kept;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (!before[i].cover && !same(before[i], after[i])) changed = true;
        if (before[i].cover && same(before[i], after[i])) kept << before[i].number;
    }
    return changed ? kept.join(QStringLiteral("・")) : QString();
}

}  // namespace

void MainWindow::build_page_size_actions() {
    make("act_image_size", QStringLiteral("画像解像度…"), [this] { image_size_dialog(); }, {},
         QStringLiteral("用紙の大きさ（mm・pixel）と解像度を変え、中身を一緒に拡大・縮小します（全ページか、このページ）"));
    make("act_canvas_size", QStringLiteral("カンバスサイズ…"), [this] { canvas_size_dialog(); }, {QKeySequence(QStringLiteral("Ctrl+Alt+C"))},
         QStringLiteral("中身の大きさはそのままで、用紙を基準位置から広げる・狭めます"));
    make("act_rotate_180", QStringLiteral("180°"), [this] { rotate_pages_by(QStringLiteral("180")); }, {},
         QStringLiteral("このページを中身ごと 180° 回します（台詞の文字は立てたまま）"));
    make("act_rotate_90cw", QStringLiteral("90°（時計回り）"), [this] { rotate_pages_by(QStringLiteral("90cw")); }, {},
         QStringLiteral("このページを中身ごと右に 90° 回します（用紙が横長に。台詞の文字は立てたまま）"));
    make("act_rotate_90ccw", QStringLiteral("90°（反時計回り）"), [this] { rotate_pages_by(QStringLiteral("90ccw")); }, {},
         QStringLiteral("このページを中身ごと左に 90° 回します（用紙が横長に。台詞の文字は立てたまま）"));
    make("act_rotate_angle", QStringLiteral("角度入力…"), [this] { rotate_angle_dialog(); }, {},
         QStringLiteral("このページを中身ごと好きな角度で回します（用紙は回したページが入る大きさに）"));
    make("act_flip_canvas_h", QStringLiteral("カンバスを左右反転"), [this] { rotate_pages_by(QStringLiteral("flip_h")); }, {},
         QStringLiteral("このページを中身ごと左右に反転します（台詞の文字は鏡にしません。表示だけの左右反転は 表示 → 左右反転）"));
    make("act_flip_canvas_v", QStringLiteral("カンバスを上下反転"), [this] { rotate_pages_by(QStringLiteral("flip_v")); }, {},
         QStringLiteral("このページを中身ごと上下に反転します（台詞の文字は鏡にしません）"));
    make("act_crop", QStringLiteral("切り抜き"), [this] { crop_to_selection(); }, {},
         QStringLiteral("選択範囲を囲む長方形でこのページを切り抜きます（切り抜いた画素は残しません）"));
    make("act_trim", QStringLiteral("トリミング…"), [this] { trim_dialog(); }, {},
         QStringLiteral("透明な所（または隅の色の所）を、このページの周りから切り取ります"));
    make("act_reveal_all", QStringLiteral("すべての領域を表示"), [this] { reveal_all_pages(); }, {},
         QStringLiteral("用紙の外にはみ出した線・絵・台詞・コマが入るまで、このページの用紙を広げます"));
    QAction* crop = make("act_crop_tool", QStringLiteral("切り抜きツール"), [this] { choose_tool(QStringLiteral("crop")); },
                         {QKeySequence(QStringLiteral("Shift+C"))},
                         QStringLiteral("ドラッグで枠を描き、中をドラッグで動かす・角で大きさ・外をドラッグで傾ける（Shift で 15° 刻み）。Enter で切り抜く・Esc でやめる"),
                         true);
    crop->setAutoRepeat(false);
    tools_->addAction(crop);
    tool_actions_[QStringLiteral("crop")] = crop;
    connect(canvas_, &PageCanvas::cropRequested, this, [this](const QRectF& box, double angle) { crop_drawn(box, angle); });
}

void MainWindow::build_page_size_menu(QMenu* pages) {
    pages->addAction(action("act_image_size"));
    pages->addAction(action("act_canvas_size"));
    QMenu* rotation = pages->addMenu(QStringLiteral("画像の回転"));
    for (const char* name : {"act_rotate_180", "act_rotate_90cw", "act_rotate_90ccw", "act_rotate_angle"}) rotation->addAction(action(name));
    rotation->addSeparator();
    for (const char* name : {"act_flip_canvas_h", "act_flip_canvas_v"}) rotation->addAction(action(name));
    for (const char* name : {"act_crop", "act_trim", "act_reveal_all"}) pages->addAction(action(name));
}

// 切り抜きツール's settings: the box's shape and turn; 切り抜く (Enter) and やめる (Esc).
void MainWindow::build_crop_page(ToolSettings* ts) {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    auto* ratio = new QComboBox;
    ratio->setObjectName(QStringLiteral("crop_ratio"));
    ratio->addItem(QStringLiteral("比率なし"), 0.0);
    ratio->addItem(QStringLiteral("元の比率（このページ）"), -1.0);
    for (const auto& [label, value] : {std::pair{"1:1（正方形）", 1.0}, {"4:5", 0.8}, {"2:3", 2.0 / 3.0}, {"3:4", 0.75}, {"16:9", 16.0 / 9.0}}) {
        ratio->addItem(QString::fromUtf8(label), value);
    }
    connect(ratio, &QComboBox::currentIndexChanged, this, [this, ratio] {
        const double value = ratio->currentData().toDouble();
        const core::Page* now = current_page();
        canvas_->crop_ratio = value >= 0 ? value : (now != nullptr && now->spec.height_mm.value() > 0 ? now->spec.width_mm.value() / now->spec.height_mm.value() : 0.0);
    });
    auto* angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("crop_angle"));
    angle->setRange(-180, 180);
    angle->setDecimals(1);
    angle->setSuffix(QStringLiteral("°"));
    angle->setToolTip(QStringLiteral("枠の傾き（時計回り）。枠の外をドラッグしても傾けられます"));
    connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canvas_->crop() || std::abs(canvas_->crop()->angle - value) < 1e-9) return;
        PageCanvas::CropBox box = *canvas_->crop();
        box.angle = value;
        canvas_->set_crop(box);
    });
    connect(canvas_, &PageCanvas::cropChanged, angle, [this, angle] {
        const QSignalBlocker quiet(angle);
        angle->setValue(canvas_->crop() ? canvas_->crop()->angle : 0.0);
    });
    auto* apply = new QPushButton(QStringLiteral("切り抜く（Enter）"));
    apply->setObjectName(QStringLiteral("crop_apply"));
    connect(apply, &QPushButton::clicked, this, [this] {
        if (const auto box = canvas_->crop()) {
            const PageCanvas::CropBox chosen = *box;
            canvas_->set_crop(std::nullopt);
            crop_drawn(chosen.rect(), chosen.angle);
        }
    });
    auto* cancel = new QPushButton(QStringLiteral("やめる（Esc）"));
    connect(cancel, &QPushButton::clicked, this, [this] { canvas_->set_crop(std::nullopt); });
    auto* note = new QLabel(QStringLiteral("用紙の外まで枠を広げると、用紙もそこまで広がります。切り抜いた画素は残しません（線・絵・台詞は用紙の外に残り、"
                                           "すべての領域を表示 で戻せます）"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    form->addRow(QStringLiteral("縦横比"), ratio);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(apply);
    form->addRow(cancel);
    form->addRow(note);
    ts->add({QStringLiteral("crop")}, page);
}

bool MainWindow::page_size_op(Json op, const QString& done) {
    const Asked asked = asking();
    const std::vector<Paper> before = papers_of(book());
    bool applied = apply_ops(Json::array({op}));
    if (!applied && !op.contains("trim") && trim_refused(last_error()) && still(asked)) {
        if (!ask::question(this, QStringLiteral("仕上がり"),
                           QStringLiteral("仕上がりが新しい用紙に入りません。仕上がりを新しい用紙に合わせますか？\n"
                                          "（用紙いっぱいに、裁ち落としの分を残して置き、余白は入るように縮めます）")) ||
            !still(asked)) {
            return false;
        }
        op["trim"] = "fit";
        applied = apply_ops(Json::array({op}));
    }
    if (!applied) return false;
    // (the selection and a crop box lie on the page as it was: let go)
    canvas_->set_selection(std::nullopt);
    canvas_->set_crop(std::nullopt);
    reload_pages();
    canvas_->fit_page();
    if (const core::Page* page = current_page()) {
        QString said = QStringLiteral("%1: %2").arg(done, QString::fromStdString(page->spec.describe()));
        if (const QString covers = covers_left(before, papers_of(book())); !covers.isEmpty()) {
            said += QStringLiteral("。表紙・カバー・帯（%1 ページ）は、用紙が表紙の設定で決まるため、そのままです").arg(covers);
        }
        flash(said, 8000);
    }
    return true;
}

void MainWindow::image_size_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    ImageSizeDialog dialog(this, *page);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    page_size_op(dialog.op(), QStringLiteral("画像解像度を変えました"));
}

void MainWindow::canvas_size_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    CanvasSizeDialog dialog(this, *page);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    page_size_op(dialog.op(), QStringLiteral("カンバスサイズを変えました"));
}

void MainWindow::rotate_pages_by(const QString& turn) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    page_size_op(Json{{"op", "rotate_pages"}, {"page", page->index.json()}, {"turn", turn.toStdString()}, {"text", "upright"}},
                 turn.startsWith(QStringLiteral("flip")) ? QStringLiteral("カンバスを反転しました") : QStringLiteral("カンバスを回転しました"));
}

void MainWindow::rotate_angle_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    RotateAngleDialog dialog(this, *page);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    page_size_op(dialog.op(), QStringLiteral("カンバスを回転しました"));
}

void MainWindow::crop_to_selection() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const std::optional<Json> area = selection_area();
    if (!area) {
        flash(QStringLiteral("切り抜く範囲を選んでから（または 切り抜きツール で枠を描いて Enter）"), 4000);
        return;
    }
    page_size_op(Json{{"op", "crop_page"}, {"page", page->index.json()}, {"area", *area}}, QStringLiteral("切り抜きました"));
}

void MainWindow::crop_drawn(const QRectF& box, double angle) {
    const core::Page* page = current_page();
    if (page == nullptr || box.width() <= 0 || box.height() <= 0) return;
    const auto r3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    Json op{{"op", "crop_page"}, {"page", page->index.json()}, {"rect", Json::array({r3(box.x()), r3(box.y()), r3(box.width()), r3(box.height())})}};
    if (std::abs(angle) > 1e-9) op["angle"] = std::round(angle * 100.0) / 100.0;
    page_size_op(op, QStringLiteral("切り抜きました"));
}

void MainWindow::trim_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    TrimDialog dialog(this, *page);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    page_size_op(dialog.op(), QStringLiteral("トリミングしました"));
}

void MainWindow::reveal_all_pages() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    page_size_op(Json{{"op", "reveal_all"}, {"page", page->index.json()}}, QStringLiteral("すべての領域を表示しました"));
}

}  // namespace genko::app
