// コマ割りの案を出す… (コマ割り K2, SPEC LAYOUT-01): app/komawari_dialog.hpp.

#include "app/komawari_dialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "app/ask.hpp"
#include "app/look.hpp"
#include "app/theme.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "service/book.hpp"
#include "service/komawari_shape.hpp"
#include "service/layout.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace ideas = service::layout_ideas;
namespace layout = service::layout;

namespace {

constexpr QSize kPicture(120, 170);
constexpr int kVariationDefault = 30;  // 変異度 0.3 (layout_ideas::Request::variation)

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

// The panels of a page as a tree with their ids (what the change must make).
Json panels_of(const core::Page& page) {
    const auto node = [](const auto& self, const core::Frame& frame) -> Json {
        Json out = Json::object({{"id", frame.id}, {"rect", Json::array({frame.rect.x.json(), frame.rect.y.json(), frame.rect.width.json(), frame.rect.height.json()})},
                                 {"bleed", frame.bleed}, {"clip", frame.clip}, {"border", frame.border_mm}, {"over", frame.over}});
        Json kids = Json::array();
        for (const core::Frame& child : frame.children) kids.push_back(self(self, child));
        out["children"] = std::move(kids);
        return out;
    };
    Json out = Json::array();
    for (const core::Frame& root : page.frames) out.push_back(node(node, root));
    return out;
}

// The page laid with the skeleton, on a copy of the book (the window's pictures: random ids, apart from any a test counts).
core::Document laid(const core::Document& doc, std::size_t index, const Json& skeleton, const std::string& actor) {
    core::ScopedIdsApart apart;
    service::Work work{doc, Json::array()};
    const Json number = work.doc.page(index).index.json();
    (void)ideas::apply_idea(work, number, skeleton, actor, true);
    return std::move(work.doc);
}

// A page whose root is a node of drawn panels: its panels go when an idea is laid, even when it is blank (one drawn panel).
bool drawn_root(const core::Page& page) { return !page.frames.empty() && core::is_free(page.frames.front()); }

// The picture of an idea: the page's panels alone (no layers: the layout is what is chosen), drawn at 20 dpi, with what a
// border cannot show marked — the paper's edge and the basic frame faintly, a panel without a border as a grey dashed
// outline, a bleed coloured out to the paper's edge, a panel whose art runs past its border with a small arrow.
QImage idea_picture(const core::Document& done, std::size_t index) {
    core::Page page = done.page(index);
    page.layers.clear();
    render::RenderOptions options;
    options.skip_unported = true;
    QImage picture = to_qimage(render::render_page(page, 20, options, &done).image).convertToFormat(QImage::Format_RGB32);
    const double paper_w = page.spec.width_mm.value(), paper_h = page.spec.height_mm.value();
    if (picture.isNull() || paper_w <= 0 || paper_h <= 0) return picture;
    const double kx = picture.width() / paper_w, ky = picture.height() / paper_h;
    const auto at = [&](double x, double y) { return QPointF(x * kx, y * ky); };
    const auto box = [&](const core::Rect& r) {
        return QRectF(at(r.x.value(), r.y.value()), at((r.x + r.width).value(), (r.y + r.height).value()));
    };
    const core::Rect trim = page.trim_rect_mm();
    const core::Rect inner = page.inner_rect_mm(done.start_side ? std::string_view(*done.start_side) : std::string_view());
    QPainter painter(&picture);
    painter.setRenderHint(QPainter::Antialiasing);
    // the bleeds: from the panel out to the paper's edge on each side it meets the basic frame
    for (const core::Frame* frame : page.leaf_frames()) {
        if (!frame->bleed) continue;
        QRectF r = box(frame->rect);
        const QRectF in = box(inner);
        if (std::abs(r.left() - in.left()) < 2) r.setLeft(0);
        if (std::abs(r.right() - in.right()) < 2) r.setRight(picture.width());
        if (std::abs(r.top() - in.top()) < 2) r.setTop(0);
        if (std::abs(r.bottom() - in.bottom()) < 2) r.setBottom(picture.height());
        painter.fillRect(r, QColor(120, 160, 230, 70));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QStringLiteral("#c8c8c8")), 1));
    painter.drawRect(box(trim).adjusted(0, 0, -1, -1));
    painter.setPen(QPen(QColor(QStringLiteral("#dcdcdc")), 1, Qt::DotLine));
    painter.drawRect(box(inner).adjusted(-2, -2, 2, 2));  // (just outside it: a border on the basic frame stays a solid line)
    for (const core::Frame* frame : page.leaf_frames()) {
        QPolygonF outline;
        if (frame->poly && !frame->poly->empty()) {
            for (const core::Point& p : *frame->poly) outline << at(p.x.value(), p.y.value());
        } else {
            outline = QPolygonF(box(frame->rect));
        }
        if (frame->border_mm <= 0) {
            painter.setPen(QPen(QColor(QStringLiteral("#909090")), 1, Qt::DashLine));
            painter.drawPolygon(outline);
        }
        if (!frame->clip) {
            // (はみ出し: an arrow out of the top right corner)
            const QPointF c = box(frame->rect).topRight();
            painter.setPen(QPen(QColor(QStringLiteral("#d0602a")), 1.4));
            painter.drawLine(c + QPointF(-7, 7), c + QPointF(2, -2));
            painter.drawLine(c + QPointF(2, -2), c + QPointF(-3, -2));
            painter.drawLine(c + QPointF(2, -2), c + QPointF(2, 3));
        }
    }
    return picture;
}

const std::pair<const char*, const char*> kKnobs[] = {{"slant", "斜め"},       {"bleed", "断ち切り"}, {"split", "コマ内分割"}, {"big", "大ゴマ率"},
                                                     {"overlap", "重ね"},     {"overflow", "はみ出し"}, {"frameless", "枠なし"}};
const char* const kKnobTips[] = {"境目を斜めにする多さと傾き", "紙の端まで広げるコマの多さ", "列を上下の行に分ける多さ",
                                 "大ゴマをどれだけ大きくするか", "コマの上に重ねるコマ（差し込み・重ね）の多さ",
                                 "絵が枠の外へはみ出すコマの多さ", "枠線のないコマの多さ"};

std::optional<double> knob_of(const ideas::Knobs& k, const std::string& key) {
    if (key == "slant") return k.slant;
    if (key == "bleed") return k.bleed;
    if (key == "split") return k.split;
    if (key == "big") return k.big;
    if (key == "overlap") return k.overlap;
    if (key == "overflow") return k.overflow;
    return k.frameless;
}

// An error in the window's words: the service's (no API keys in brackets, such as （min_tiers）).
QString shown_error(const std::exception& error) {
    QString words;
    if (const auto* given = dynamic_cast<const layout::LayoutError*>(&error)) {
        words = wording::error(given->message());
    } else {
        words = wording::error(QString::fromUtf8(error.what()));
    }
    static const QRegularExpression key(QStringLiteral("（[A-Za-z_]+）"));
    return words.remove(key);
}

// Why a batch came out short, from the reason the most ideas were sieved out for.
QString dropped_words(const ideas::Batch& batch) {
    std::string most;
    int n = 0;
    for (const auto& [why, k] : batch.dropped) {
        if (k > n) {
            most = why;
            n = k;
        }
    }
    if (most == "focus") return QStringLiteral("見せ場のコマが、いちばん大きくならない案を外しました");
    if (most == "small") return QStringLiteral("小さすぎるコマのある案を外しました");
    if (most == "order") return QStringLiteral("読む順が乱れる案を外しました");
    if (most == "gap") return QStringLiteral("コマの間が狭すぎる案を外しました");
    if (most == "same_as_base") return QStringLiteral("元とほとんど同じ案を外しました");
    if (most == "unfit") return QStringLiteral("この大きさに収まらない案を外しました");
    if (most == "tiers") return QStringLiteral("段数に合わない案を外しました");
    if (most == "accents" || most == "strips") return QStringLiteral("目立つ所の少ない案を外しました（つまみを上げると増えます）");
    return QStringLiteral("似ている案や無難すぎる案を外しました");
}

}  // namespace

templates::Plan idea_plan(const core::Document& doc, std::size_t page_index, const Json& skeleton, const std::string& actor) {
    const core::Page& page = doc.page(page_index);
    if (page.frames.empty()) throw core::Error("value", "このページにはコマがありません");
    const Json number = page.index.json();
    // 1. the page laid on a copy (each op is a batch of its own there, and the service draws a job id after each)
    Json clearing = Json::array();
    layout::CompiledLayout compiled;
    std::vector<std::string> taken;
    Json made;
    {
        core::ScopedIdScript script;
        service::Work work{doc, Json::array()};
        if (!layout::is_blank(work.doc, work.doc.page(page_index)) || drawn_root(work.doc.page(page_index))) {
            clearing = layout::clear_page(work, page_index, actor);
        }
        compiled = ideas::apply_idea(work, number, skeleton, actor, false);
        taken = script.taken();
        made = panels_of(work.doc.page(page_index));
    }
    // 2. the same ops again, batch by batch, with the same ids: the ids the ops gave, without the job ids between them
    templates::Plan out;
    {
        const core::CommandBus bus(render::ops_registry());
        core::ScopedIdScript script(taken);
        core::Document copy = doc;
        std::vector<std::size_t> jobs;
        const auto run = [&](const Json& batch) {
            copy = bus.apply(copy, batch, core::Actor(actor)).doc;
            for (const Json& op : batch) out.ops.push_back(op);
            (void)core::new_id();
            jobs.push_back(script.taken().size() - 1);
        };
        if (!clearing.empty()) run(clearing);
        for (const Json& op : compiled.ops) run(Json::array({op}));
        for (std::size_t i = 0; i < script.taken().size(); ++i) {
            if (std::find(jobs.begin(), jobs.end(), i) == jobs.end()) out.ids.push_back(script.taken()[i]);
        }
    }
    // 3. as one change, they make the same panels
    {
        core::ScopedIdScript script(out.ids);
        const core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(doc, out.ops, core::Actor(actor));
        if (panels_of(result.doc.page(page_index)) != made) throw core::Error("value", "コマ割りの案を 1 つの変更にまとめられません");
    }
    return out;
}

KomawariDialog::KomawariDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page_index, std::string actor)
    : QDialog(parent),
      doc_(std::move(doc)),
      page_index_(page_index),
      actor_(std::move(actor)),
      pool_(std::make_unique<QThreadPool>()),
      self_(std::make_shared<KomawariDialog*>(this)) {
    setWindowTitle(QStringLiteral("コマ割りの案を出す"));
    setObjectName(QStringLiteral("komawari"));
    pool_->setMaxThreadCount(2);
    const core::Page& page = doc_->page(page_index_);
    spec_ = page.spec;
    needs_clearing = !templates::is_blank(*doc_, page) || drawn_root(page);  // (drawn panels go too)
    page_skeleton_ = service::komawari::skeleton_of_page(*doc_, page);
    if (page.leaf_frames().size() < 2) page_skeleton_.reset();  // (one panel: nothing to vary)

    // --- the settings (left) ---
    auto* settings = new QWidget;
    auto* form = new QFormLayout(settings);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    preset = new QComboBox;
    preset->setObjectName(QStringLiteral("komawari_preset"));
    for (const ideas::Preset p : {ideas::Preset::Talk, ideas::Preset::Standard, ideas::Preset::Action, ideas::Preset::Showcase}) {
        preset->addItem(QString::fromUtf8(ideas::preset_label(p).data(), static_cast<int>(ideas::preset_label(p).size())),
                        QString::fromLatin1(ideas::preset_key(p).data(), static_cast<int>(ideas::preset_key(p).size())));
    }
    preset->setCurrentIndex(1);  // (標準)
    preset->setToolTip(QStringLiteral("コマ数・段数・つまみの初めの値が変わります"));
    form->addRow(QStringLiteral("プリセット"), preset);
    panels = new QComboBox;
    panels->setObjectName(QStringLiteral("komawari_panels"));
    panels->addItem(QStringLiteral("指定なし"), 0);
    for (int n = 1; n <= static_cast<int>(layout::kMaxPanels); ++n) panels->addItem(QStringLiteral("%1 コマ").arg(n), n);
    panels->setToolTip(QStringLiteral("指定すると、どの案もちょうどこのコマ数になります（指定なし: プリセットの範囲）"));
    form->addRow(QStringLiteral("コマ数"), panels);
    auto* tiers = new QWidget;
    auto* tiers_row = new QHBoxLayout(tiers);
    tiers_row->setContentsMargins(0, 0, 0, 0);
    min_tiers = new QSpinBox;
    max_tiers = new QSpinBox;
    for (QSpinBox* box : {min_tiers, max_tiers}) {
        box->setRange(0, ideas::kMaxTiers);
        box->setSpecialValueText(QStringLiteral("指定なし"));
        box->setSuffix(QStringLiteral(" 段"));
    }
    min_tiers->setObjectName(QStringLiteral("komawari_min_tiers"));
    max_tiers->setObjectName(QStringLiteral("komawari_max_tiers"));
    tiers_row->addWidget(min_tiers, 1);
    tiers_row->addWidget(new QLabel(QStringLiteral("〜")));
    tiers_row->addWidget(max_tiers, 1);
    tiers->setToolTip(QStringLiteral("段の数の範囲（指定なし: プリセットの範囲）"));
    form->addRow(QStringLiteral("段数"), tiers);
    focus = new QSpinBox;
    focus->setObjectName(QStringLiteral("komawari_focus"));
    focus->setRange(0, static_cast<int>(layout::kMaxPanels));
    focus->setSpecialValueText(QStringLiteral("なし"));
    focus->setPrefix(QStringLiteral("読み順で "));
    focus->setSuffix(QStringLiteral(" 番目"));
    focus->setToolTip(QStringLiteral("このコマをいちばん大きいコマ（見せ場）にします"));
    form->addRow(QStringLiteral("見せ場のコマ"), focus);
    count = new QComboBox;
    count->setObjectName(QStringLiteral("komawari_count"));
    for (const int n : {5, ideas::kDefaultCount, 24}) count->addItem(QStringLiteral("%1 案").arg(n), n);
    count->setCurrentIndex(1);
    form->addRow(QStringLiteral("束の数"), count);
    auto* knob_title = new QLabel(QStringLiteral("つまみ"));
    theme::role(knob_title, "section");
    form->addRow(knob_title);
    for (std::size_t i = 0; i < std::size(kKnobs); ++i) {
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setObjectName(QStringLiteral("komawari_knob_%1").arg(QString::fromLatin1(kKnobs[i].first)));
        slider->setRange(0, 100);
        slider->setToolTip(QString::fromUtf8(kKnobTips[i]));
        auto* shown = new QLabel(QStringLiteral("0.00"));
        shown->setMinimumWidth(32);
        connect(slider, &QSlider::valueChanged, shown, [shown](int v) { shown->setText(QString::number(v / 100.0, 'f', 2)); });
        auto* row = new QWidget;
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(slider, 1);
        line->addWidget(shown);
        form->addRow(QString::fromUtf8(kKnobs[i].second), row);
        knobs.emplace_back(kKnobs[i].first, slider);
    }
    auto* seed_title = new QLabel(QStringLiteral("シード（seed）"));
    theme::role(seed_title, "section");
    form->addRow(seed_title);
    seed = new QLineEdit;
    seed->setObjectName(QStringLiteral("komawari_seed"));
    seed->setToolTip(QStringLiteral("同じ設定とシードなら、同じ案が出ます。貼り付けて Enter で出し直します"));
    form->addRow(seed);
    auto* seed_buttons = new QWidget;
    auto* seed_row = new QHBoxLayout(seed_buttons);
    seed_row->setContentsMargins(0, 0, 0, 0);
    copy_seed = new QPushButton(QStringLiteral("コピー"));
    copy_seed->setToolTip(QStringLiteral("この束のシードをクリップボードへ"));
    paste_seed = new QPushButton(QStringLiteral("貼り付け"));
    paste_seed->setToolTip(QStringLiteral("クリップボードのシードで束を出し直します"));
    seed_row->addWidget(copy_seed);
    seed_row->addWidget(paste_seed);
    form->addRow(seed_buttons);
    new_batch = new QPushButton(QStringLiteral("新しい束"));
    new_batch->setObjectName(QStringLiteral("komawari_new"));
    new_batch->setToolTip(QStringLiteral("シードを変えて、同じ設定で別の束を出します"));
    form->addRow(new_batch);
    settings->setFixedWidth(std::max(280, settings->sizeHint().width()));  // (its rows whole)

    // --- the batch (right) ---
    state = new QLabel;
    state->setObjectName(QStringLiteral("komawari_state"));
    state->setWordWrap(true);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("komawari_ideas"));
    list_->setViewMode(QListWidget::IconMode);
    list_->setIconSize(kPicture);
    list_->setResizeMode(QListWidget::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSpacing(4);
    list_->setGridSize(QSize(kPicture.width() + 16, kPicture.height() + 32));  // (the pictures in even rows and columns)
    list_->setUniformItemSizes(true);
    list_->setWordWrap(true);
    summary = new QLabel;
    summary->setObjectName(QStringLiteral("komawari_summary"));
    summary->setWordWrap(true);
    auto* vary_box = new QWidget;
    auto* vary_row = new QHBoxLayout(vary_box);
    vary_row->setContentsMargins(0, 0, 0, 0);
    base = new QComboBox;
    base->setObjectName(QStringLiteral("komawari_base"));
    base->addItem(QStringLiteral("選んだ案"));
    base->addItem(QStringLiteral("今のページのコマ割り"));
    if (!page_skeleton_) {
        if (auto* model = qobject_cast<QStandardItemModel*>(base->model())) {
            model->item(1)->setEnabled(false);
            model->item(1)->setToolTip(QStringLiteral("コマが 1 つのページや、描いたコマのページは、元にできません"));
        }
    }
    variation = new QSlider(Qt::Horizontal);
    variation->setObjectName(QStringLiteral("komawari_variation"));
    variation->setRange(0, 100);
    variation->setValue(kVariationDefault);
    variation->setToolTip(QStringLiteral("変異度: 小さいと元に近く、大きいと段の組み方まで変わります"));
    auto* variation_shown = new QLabel(QString::number(kVariationDefault / 100.0, 'f', 2));
    connect(variation, &QSlider::valueChanged, variation_shown, [variation_shown](int v) { variation_shown->setText(QString::number(v / 100.0, 'f', 2)); });
    vary = new QPushButton(QStringLiteral("これを元に変異"));
    vary->setObjectName(QStringLiteral("komawari_vary"));
    vary->setToolTip(QStringLiteral("変異の元（選んだ案か今のページのコマ割り）を、変異度に応じて少しずつ変えた案の束を出します"));
    back = new QPushButton(QStringLiteral("ガチャに戻る"));
    back->setObjectName(QStringLiteral("komawari_back"));
    back->setToolTip(QStringLiteral("変異の前の束に戻ります"));
    // (two rows: the base and the 変異度, then the buttons — the slider keeps its room on a narrow screen)
    auto* vary_rows = new QVBoxLayout;
    vary_rows->setContentsMargins(0, 0, 0, 0);
    vary_row->addWidget(new QLabel(QStringLiteral("変異の元")));
    vary_row->addWidget(base);
    vary_row->addWidget(new QLabel(QStringLiteral("変異度")));
    vary_row->addWidget(variation, 1);
    vary_row->addWidget(variation_shown);
    auto* vary_buttons = new QHBoxLayout;
    vary_buttons->addWidget(vary);
    vary_buttons->addWidget(back);
    vary_buttons->addStretch(1);
    vary_rows->addWidget(vary_box);
    vary_rows->addLayout(vary_buttons);
    auto* right = new QVBoxLayout;
    right->addWidget(state);
    right->addWidget(list_, 1);
    right->addWidget(summary);
    right->addLayout(vary_rows);

    QString words = needs_clearing ? QStringLiteral("コマと台詞は作り直されます（元に戻す で取り消せます）。") : QStringLiteral("空のページを、選んだ案で割ります。");
    if (page.name_ok) words = QStringLiteral("承認済みのネームのページです。") + words;
    auto* note = new QLabel(words);
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    apply_button = buttons->button(QDialogButtonBox::Ok);
    apply_button->setText(QStringLiteral("このページに当てる"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { choose(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // (the body scrolls on a small screen; the note and the buttons stay in sight — SPEC UX-02)
    list_->setMinimumSize(kPicture.width() * 2 + 48, kPicture.height() + 48);
    auto* body = new QWidget;
    auto* row = new QHBoxLayout(body);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(settings, 0, Qt::AlignTop);
    row->addLayout(right, 1);
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("dialogBody"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    auto* outer = new QVBoxLayout(this);
    outer->addWidget(scroll, 1);
    outer->addWidget(note);
    outer->addWidget(buttons);

    // --- what each does ---
    again_ = new QTimer(this);
    again_->setSingleShot(true);
    again_->setInterval(250);
    connect(again_, &QTimer::timeout, this, [this] { make_batch(seed->text().trimmed().toStdString()); });
    connect(preset, &QComboBox::currentIndexChanged, this, [this] { preset_changed(); });
    for (QComboBox* box : {panels, count}) connect(box, &QComboBox::currentIndexChanged, this, [this] { settings_changed(); });
    for (QSpinBox* box : {min_tiers, max_tiers, focus}) connect(box, &QSpinBox::valueChanged, this, [this] { settings_changed(); });
    connect(min_tiers, &QSpinBox::valueChanged, this, [this](int v) {
        if (v > 0 && max_tiers->value() > 0 && max_tiers->value() < v) max_tiers->setValue(v);
    });
    connect(max_tiers, &QSpinBox::valueChanged, this, [this](int v) {
        if (v > 0 && min_tiers->value() > v) min_tiers->setValue(v);
    });
    connect(panels, &QComboBox::currentIndexChanged, this, [this] { bound(); });
    for (const auto& [key, slider] : knobs) connect(slider, &QSlider::valueChanged, this, [this] { settings_changed(); });
    connect(seed, &QLineEdit::returnPressed, this, [this] { from_seed_field(); });
    connect(copy_seed, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(QString::fromStdString(batch_.seed)); });
    connect(paste_seed, &QPushButton::clicked, this, [this] {
        const QString text = QApplication::clipboard()->text().trimmed();
        if (text.isEmpty()) return;
        seed->setText(text);
        from_seed_field();
    });
    connect(new_batch, &QPushButton::clicked, this, [this] {
        if (varying_ && base_skeleton_) {
            vary_from(Json(*base_skeleton_), base_words_, {});  // (new variations of the same base)
        } else {
            make_batch({});
        }
    });
    connect(vary, &QPushButton::clicked, this, [this] { make_variations(); });
    connect(back, &QPushButton::clicked, this, [this] { back_to_gacha(); });
    connect(base, &QComboBox::currentIndexChanged, this, [this] { follow(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { follow(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { choose(); });

    // (Enter in the seed field draws the batch again: it never lays an idea on the page)
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    preset_changed();  // (the knobs at the preset's)
    make_batch({});    // (the first batch at once)
    look::fit_to_screen(this, QSize(1000, 680));
}

KomawariDialog::~KomawariDialog() {
    self_.reset();
    pool_->clear();
    pool_->waitForDone();
}

ideas::Request KomawariDialog::request() const {
    Json args = Json::object();
    if (const int n = panels->currentData().toInt(); n > 0) args["panels"] = n;
    if (min_tiers->value() > 0) args["min_tiers"] = min_tiers->value();
    if (max_tiers->value() > 0) args["max_tiers"] = max_tiers->value();
    if (focus->value() > 0) args["focus"] = focus->value();
    args["count"] = count->currentData().toInt();
    const auto chosen_preset = ideas::preset_from(preset->currentData().toString().toStdString());
    const ideas::Preset p = chosen_preset ? *chosen_preset : ideas::Preset::Standard;
    args["preset"] = std::string(ideas::preset_key(p));
    // (a knob where the preset has it is left to the preset: the same batch as a request without it)
    const ideas::PresetDefaults defaults = ideas::preset_defaults(p);
    Json given = Json::object();
    for (const auto& [key, slider] : knobs) {
        const std::optional<double> preset_value = knob_of(defaults.knobs, key);
        if (preset_value && static_cast<int>(std::lround(*preset_value * 100)) == slider->value()) continue;
        given[key] = slider->value() / 100.0;
    }
    if (!given.empty()) args["knobs"] = given;
    return ideas::request_from_json(args, spec_);
}

void KomawariDialog::preset_changed() {
    const auto p = ideas::preset_from(preset->currentData().toString().toStdString());
    const ideas::PresetDefaults defaults = ideas::preset_defaults(p ? *p : ideas::Preset::Standard);
    for (const auto& [key, slider] : knobs) slider->setValue(static_cast<int>(std::lround(knob_of(defaults.knobs, key).value_or(0.0) * 100)));
    bound();
    settings_changed();
}

void KomawariDialog::bound() {
    // (見せ場 and 段数 within the panels there can be: those given, else the preset's most)
    const auto p = ideas::preset_from(preset->currentData().toString().toStdString());
    const int given = panels->currentData().toInt();
    const int most = given > 0 ? given : ideas::preset_defaults(p ? *p : ideas::Preset::Standard).max_panels;
    focus->setMaximum(most);
    min_tiers->setMaximum(std::min(most, ideas::kMaxTiers));
    max_tiers->setMaximum(std::min(most, ideas::kMaxTiers));
}

void KomawariDialog::keyPressEvent(QKeyEvent* event) {
    // (Enter lays the idea only from the list or the button: in the seed's field or a number's it draws the batch again)
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        QWidget* at = focusWidget();
        if (at != list_ && at != apply_button) {
            event->accept();
            return;
        }
        if (at == list_) {
            choose();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void KomawariDialog::settings_changed() {
    if (varying_) {
        // (the settings are the batch's: a change goes back to the batch they make, from its seed)
        if (gacha_) seed->setText(QString::fromStdString(gacha_->seed));
        varying_ = false;
        gacha_.reset();
        base_skeleton_.reset();
    }
    again_->start();
    follow();  // (nothing laid nor varied from the list shown until the batch the settings make is there)
}

void KomawariDialog::make_batch(const std::string& with_seed) {
    again_->stop();
    varying_ = false;
    gacha_.reset();
    base_skeleton_.reset();
    try {
        ideas::Request r = request();
        r.seed = with_seed;
        batch_ = ideas::generate(r);
    } catch (const std::exception& error) {
        batch_ = {};
        state->setText(shown_error(error));
        show_batch();
        return;
    }
    show_batch();
}

void KomawariDialog::from_seed_field() {
    const std::string typed = seed->text().trimmed().toStdString();
    if (!varying_ || !base_skeleton_) {
        make_batch(typed);
        return;
    }
    vary_from(Json(*base_skeleton_), base_words_, typed);  // (the variations of the same base, from that seed)
}

void KomawariDialog::vary_from(const Json& from, const QString& words, const std::string& with_seed) {
    again_->stop();
    if (!varying_) {
        gacha_ = batch_;
        gacha_row_ = list_->currentRow();
    }
    try {
        ideas::Request r = request();
        r.base = from;
        r.variation = variation->value() / 100.0;
        r.panels = 0;  // (the base's own)
        r.seed = with_seed;
        if (r.focus > 0 && static_cast<int>(service::komawari::reading_slots(from).size()) < r.focus) r.focus = 0;
        batch_ = ideas::generate(r);
    } catch (const std::exception& error) {
        batch_ = {};
        state->setText(shown_error(error));
    }
    varying_ = true;
    base_skeleton_ = from;
    base_words_ = words;
    show_batch();
}

void KomawariDialog::make_variations() {
    if (again_->isActive()) return;
    if (base->currentIndex() == 1) {
        if (page_skeleton_) vary_from(Json(*page_skeleton_), QStringLiteral("今のページのコマ割り"), {});
        return;
    }
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(batch_.ideas.size())) return;
    const Json from = batch_.ideas[static_cast<std::size_t>(row)].skeleton;
    vary_from(from, varying_ ? QStringLiteral("変異の案 %1").arg(row + 1) : QStringLiteral("案 %1").arg(row + 1), {});
}

void KomawariDialog::back_to_gacha() {
    if (!varying_ || !gacha_) return;
    batch_ = *gacha_;
    varying_ = false;
    gacha_.reset();
    base_skeleton_.reset();
    show_batch();
    if (gacha_row_ >= 0 && gacha_row_ < list_->count()) list_->setCurrentRow(gacha_row_);
}

void KomawariDialog::show_batch() {
    ++generation_;
    pool_->clear();  // (the pictures of the batch before, not yet started, are not drawn)
    list_->clear();
    if (!batch_.seed.empty()) seed->setText(QString::fromStdString(batch_.seed));  // (an error keeps the seed it was)
    const int asked = count->currentData().toInt();
    const int n = static_cast<int>(batch_.ideas.size());
    if (!batch_.seed.empty()) {
        QString words = varying_ ? QStringLiteral("変異: %1 を元に、変異度 %2 の %3 案").arg(base_words_, QString::number(variation->value() / 100.0, 'f', 2)).arg(n)
                                 : QStringLiteral("ガチャ: %1 案").arg(n);
        if (n < asked) words += QStringLiteral("（%1 案のうち、%2）").arg(asked).arg(dropped_words(batch_));
        state->setText(words);
    }
    QPixmap blank(kPicture);
    blank.fill(QColor(QStringLiteral("#f4f4f4")));
    pending_ = n;
    for (int i = 0; i < n; ++i) {
        const ideas::Idea& idea = batch_.ideas[static_cast<std::size_t>(i)];
        auto* item = new QListWidgetItem(QIcon(blank), QStringLiteral("案 %1").arg(i + 1));
        item->setToolTip(QString::fromStdString(service::komawari::describe_for_people(idea.shape)));
        item->setData(Qt::UserRole, i);
        list_->addItem(item);
        // its picture: the page cut by it, drawn small on a worker thread
        std::weak_ptr<KomawariDialog*> self = self_;
        const auto book = doc_;
        const std::size_t index = page_index_;
        const std::string who = actor_;
        const Json skeleton = idea.skeleton;
        const int generation = generation_;
        pool_->start([self, book, index, who, skeleton, generation, i]() {
            QImage picture;
            try {
                const core::Document done = laid(*book, index, skeleton, who);
                picture = idea_picture(done, index);
            } catch (const std::exception&) {
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, picture, generation, i]() {
                    const auto alive = self.lock();
                    if (!alive) return;
                    KomawariDialog* dialog = *alive;
                    if (generation != dialog->generation_) return;  // (a batch before this one)
                    --dialog->pending_;
                    if (!picture.isNull() && i < dialog->list_->count()) dialog->list_->item(i)->setIcon(QIcon(QPixmap::fromImage(picture)));
                },
                Qt::QueuedConnection);
        });
    }
    if (n > 0) list_->setCurrentRow(0);
    follow();
}

void KomawariDialog::follow() {
    const int row = list_->currentRow();
    const bool picked = row >= 0 && row < static_cast<int>(batch_.ideas.size());
    summary->setText(picked ? QStringLiteral("案 %1: %2").arg(row + 1).arg(QString::fromStdString(
                                                                  service::komawari::describe_for_people(batch_.ideas[static_cast<std::size_t>(row)].shape)))
                            : QString());
    const bool settled = !again_->isActive();
    apply_button->setEnabled(picked && settled);
    vary->setEnabled(settled && (base->currentIndex() == 1 ? page_skeleton_.has_value() : picked));
    back->setEnabled(settled && varying_ && gacha_.has_value());
}

bool KomawariDialog::wait_pictures(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (pending_ > 0) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

void KomawariDialog::choose() {
    if (again_->isActive()) return;
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(batch_.ideas.size())) return;
    const ideas::Idea& idea = batch_.ideas[static_cast<std::size_t>(row)];
    try {
        plan = idea_plan(*doc_, page_index_, idea.skeleton, actor_);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), shown_error(error));
        return;
    }
    chosen = QString::fromStdString(service::komawari::describe_for_people(idea.shape));
    accept();
}

}  // namespace genko::app
