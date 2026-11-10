// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 描画 → 炎…・額縁…・木…'s windows (app/render_dialogs.hpp).
// Their settings: core::picture_kinds (render_picture's kinds); their shapes: render/render_picture (PhotoCraft v0.2.0's
// render2.rs). The words are Photoshop's (日本語版); the trees' and frames' names are PhotoCraft's own designs in Japanese.

#include "app/render_dialogs.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/colours.hpp"
#include "app/main_window.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyops.hpp"
#include "render/render_picture.hpp"
#include "service/wording.hpp"

namespace genko::app::render_dialogs {

namespace {

using core::Json;

QString words(const char* text) { return QString::fromUtf8(text); }

// Each setting's word (Photoshop's), by kind.
const std::map<std::string, QString>& labels() {
    static const std::map<std::string, QString> all{
        // 炎
        {"flame_type", words("炎の種類")}, {"length", words("長さ（px）")}, {"randomize_length", words("長さのランダム化")}, {"width", words("幅（px）")},
        {"angle", words("角度（°）")}, {"interval", words("間隔（px）")}, {"adjust_interval_for_loops", words("ループの間隔を調整")},
        {"use_custom_color", words("カスタムカラーを炎に使用")}, {"color", words("炎のカラー")}, {"turbulent", words("乱れ")}, {"jag", words("ギザギザ")},
        {"opacity", words("不透明度（%）")}, {"flame_lines", words("炎のライン（複雑さ）")}, {"flame_bottom_alignment", words("炎の下端の配置")},
        {"flame_style", words("炎のスタイル")}, {"flame_shape", words("炎の形状")}, {"randomize_shapes", words("炎の形状をランダム化")},
        {"seed", words("ランダムシード")}, {"quality", words("品質")}, {"path", words("パス")},
        // 額縁
        {"margin", words("余白")}, {"size", words("サイズ")}, {"arrangement", words("配置")}, {"lines", words("線の数")}, {"thickness", words("太さ")},
        {"fade", words("フェード")}, {"vine_color", words("つるのカラー")}, {"flower_color", words("花のカラー")}, {"leaf_color", words("葉のカラー")},
        // 木
        {"light_direction", words("光の方向")}, {"leaves_amount", words("葉の量")}, {"leaves_size", words("葉のサイズ")}, {"branches_height", words("枝の高さ")},
        {"branches_thickness", words("枝の太さ")}, {"default_leaves", words("デフォルトの葉を使用")}, {"leaves_color", words("葉のカラー")},
        {"custom_branch_color", words("カスタムカラーで枝を表示")}, {"branches_color", words("枝のカラー")}, {"flat_shading", words("フラットシェーディング")},
        {"x", words("位置（横）")}, {"y", words("位置（縦）")}};
    return all;
}

std::vector<std::pair<QString, Json>> choices_of(const std::string& key) {
    if (key == "flame_type")
        return {{words("パスに沿って 1 つの炎"), Json("one_flame_along_path")}, {words("パスに沿って複数の炎"), Json("multiple_flames_along_path")},
                {words("パスの方向に複数の炎"), Json("multiple_flames_path_directions")}, {words("複数の炎（様々な長さ）"), Json("multiple_flames_various_length")},
                {words("キャンドルライト"), Json("candle_light")}, {words("1 方向に複数の炎"), Json("multiple_flames_one_direction")}};
    if (key == "flame_style") return {{words("標準"), Json("normal")}, {words("激しい"), Json("violent")}, {words("平坦"), Json("flat")}};
    if (key == "flame_shape")
        return {{words("平行"), Json("parallel")}, {words("中央へ"), Json("to_center")}, {words("広がる"), Json("spread")}, {words("楕円"), Json("oval")},
                {words("尖る"), Json("pointed")}};
    if (key == "quality")
        return {{words("ドラフト"), Json("draft")}, {words("低"), Json("low")}, {words("中"), Json("medium")}, {words("高"), Json("high")}, {words("精細"), Json("fine")}};
    return {};
}

// The shapes drawn small with QPainter (a picture of a type or design: not the op's own laying).
QImage small_picture(const std::vector<render::picture::Prim>& prims, const render::picture::Canvas& c, int size) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(248, 248, 248));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double k = size / std::max(c.width, c.height);
    p.scale(k, k);
    p.translate(-c.x0, -c.y0);
    for (const render::picture::Prim& prim : prims) {
        QColor colour = QColor::fromRgbF(std::clamp(prim.c0[0], 0.0f, 1.0f), std::clamp(prim.c0[1], 0.0f, 1.0f), std::clamp(prim.c0[2], 0.0f, 1.0f),
                                         std::clamp(prim.c0[3], 0.0f, 1.0f));
        p.setCompositionMode(prim.blend == render::picture::Prim::Blend::Add       ? QPainter::CompositionMode_Plus
                             : prim.blend == render::picture::Prim::Blend::Lighten ? QPainter::CompositionMode_Lighten
                                                                                   : QPainter::CompositionMode_SourceOver);
        if (prim.shape == render::picture::Prim::Shape::Capsule) {
            p.setPen(QPen(colour, std::max(0.6, static_cast<double>(prim.r0 + prim.r1)), Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(prim.x0, prim.y0), QPointF(prim.x1, prim.y1));
        } else {
            p.save();
            p.translate(prim.x0, prim.y0);
            p.rotate(prim.angle * 180.0 / 3.14159265358979323846);
            p.setPen(Qt::NoPen);
            p.setBrush(colour);
            p.drawEllipse(QPointF(0, 0), std::max(0.3f, prim.r0), std::max(0.3f, prim.r1));
            p.restore();
        }
    }
    return image;
}

class RenderDialog : public QDialog {
public:
    RenderDialog(QWidget* parent, const FilterWindowCall& call, std::string kind) : QDialog(parent), call_(call), kind_(std::move(kind)) {
        setObjectName(QStringLiteral("filter_dialog"));
        setProperty("kind", QString::fromStdString(kind_));
        const core::PictureKind* table = core::picture_kind(kind_);
        setWindowTitle(QString::fromUtf8(table->name.data(), static_cast<qsizetype>(table->name.size())));
        auto* outer = new QHBoxLayout(this);
        if (kind_ != "flame") {  // (the types or designs, each its small picture)
            list_ = new QListWidget(this);
            list_->setObjectName(kind_ == "tree" ? QStringLiteral("render_tree_types") : QStringLiteral("render_frames"));
            list_->setViewMode(QListView::IconMode);
            list_->setIconSize(QSize(56, 56));
            list_->setGridSize(QSize(92, 92));
            list_->setResizeMode(QListView::Adjust);
            list_->setMovement(QListView::Static);
            list_->setWordWrap(true);
            list_->setMinimumWidth(320);
            const int count = kind_ == "tree" ? static_cast<int>(core::tree_type_names().size()) : static_cast<int>(core::frame_style_names().size());
            for (int i = 1; i <= count; ++i) list_->addItem(kind_ == "tree" ? tree_words(i) : frame_words(i));
            outer->addWidget(list_, 1);
            make_pictures();
        }
        auto* right = new QVBoxLayout;
        outer->addLayout(right);
        form_ = new adjust_forms::Form(this);
        right->addWidget(form_);
        build(*table);
        form_->set_values(Json::object());
        new_layer_ = new QCheckBox(QStringLiteral("新しいレイヤーに描く"), this);
        new_layer_->setObjectName(QStringLiteral("render_new_layer"));
        right->addWidget(new_layer_);
        auto* note = new QLabel(kind_ == "flame" ? QStringLiteral("長さ・幅・間隔は 200 dpi の画素で数えます。パスが無いときは、仕上がり枠の下の方に弧を描きます。")
                                                 : QStringLiteral("選択範囲があればその枠に、無ければ仕上がり枠に描きます。"),
                                this);
        note->setWordWrap(true);
        note->setProperty("role", "hint");
        right->addWidget(note);
        preview_ = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"), this);
        preview_->setObjectName(QStringLiteral("filter_preview"));
        preview_->setChecked(true);
        right->addWidget(preview_);
        auto* busy = new QLabel(QStringLiteral("プレビューを作っています…"), this);
        busy->setObjectName(QStringLiteral("filter_preview_busy"));
        busy->setProperty("role", "hint");
        busy->setVisible(false);
        right->addWidget(busy);
        auto* poll = new QTimer(this);
        poll->setInterval(100);
        connect(poll, &QTimer::timeout, this, [this, busy] { busy->setVisible(call_.previewing && call_.previewing()); });
        poll->start();
        auto* row = new QDialogButtonBox(this);
        QPushButton* reset = row->addButton(QStringLiteral("既定に戻す"), QDialogButtonBox::ResetRole);
        reset->setObjectName(QStringLiteral("filter_reset"));
        connect(reset, &QPushButton::clicked, form_, &adjust_forms::Form::reset);
        right->addWidget(row);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        right->addStretch(1);
        right->addWidget(buttons);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(250);
        connect(timer_, &QTimer::timeout, this, [this] { show_result(); });
        connect(form_, &adjust_forms::Form::changed, this, [this] { timer_->start(); });
        connect(preview_, &QCheckBox::toggled, this, [this] { timer_->start(); });
        connect(new_layer_, &QCheckBox::toggled, this, [this] { timer_->start(); });
        timer_->start(0);
    }
    ~RenderDialog() override {
        if (call_.preview) call_.preview(std::nullopt);
    }

    // The settings chosen (render_picture's, without the op's own keys but new_layer).
    Json settings() const {
        Json out = adjust_forms::without_nulls(form_->values());
        if (new_layer_->isChecked()) out["new_layer"] = true;
        return out;
    }

private:
    void build(const core::PictureKind& table) {
        using Type = core::AdjustParam::Type;
        for (const core::AdjustParam& p : table.params) {
            const std::string key(p.key);
            const QString label = labels().count(key) != 0 ? labels().at(key) : QString::fromStdString(key);
            if (key == "base_tree_type" || key == "frame") {
                list_->setCurrentRow(0);
                connect(list_, &QListWidget::currentRowChanged, form_, [this] { emit form_->changed(); });
                form_->custom(key, QString(), new QWidget, [this] { return Json(list_->currentRow() + 1); },
                              [this](const Json& v) { list_->setCurrentRow(v.is_number() ? std::max(0, v.get<int>() - 1) : 0); }, Json(1));
                continue;
            }
            if (key == "use_custom_color" || key == "default_leaves" || key == "custom_branch_color") {
                form_->check(key, label, key == "default_leaves");
                continue;
            }
            if (key == "quality") {
                form_->choice(key, label, choices_of(key), Json("medium"));
                continue;
            }
            if (key == "path") {  // (the page's paths: the work path, each saved one; none: upstream's arc)
                std::vector<std::pair<QString, Json>> options;
                if (call_.page->work_path && !call_.page->work_path->empty()) options.emplace_back(QStringLiteral("作業用パス"), Json("work"));
                for (const core::vector::NamedPath& named : call_.page->paths) options.emplace_back(QString::fromStdString(named.name), Json(named.name));
                if (options.empty()) options.emplace_back(QStringLiteral("パスなし（仕上がり枠の下の方の弧）"), Json(nullptr));
                form_->choice(key, label, options, options.front().second);
                continue;
            }
            switch (p.type) {
                case Type::Number: form_->number(key, label, p.lo, p.hi, p.fallback, p.hi - p.lo <= 2.5 ? 2 : 0); break;
                case Type::Whole: form_->number(key, label, p.lo, p.hi, p.fallback, 0); break;
                case Type::Bool: form_->check(key, label, p.fallback != 0); break;
                case Type::Choice: form_->choice(key, label, choices_of(key), Json(std::string(p.text))); break;
                case Type::Rgb: {
                    std::array<int, 3> fallback{static_cast<int>(p.numbers[0]), static_cast<int>(p.numbers[1]), static_cast<int>(p.numbers[2])};
                    form_->colour(key, label, fallback);
                    break;
                }
                default: break;
            }
        }
        // (a new seed each window, as Photoshop's ランダム化)
        Json seeded = Json::object();
        seeded["seed"] = static_cast<std::int64_t>(QRandomGenerator::global()->bounded(100000));
        initial_ = seeded;
    }

    void make_pictures() {
        auto* watcher = new QFutureWatcher<std::vector<QImage>>(this);
        connect(watcher, &QFutureWatcher<std::vector<QImage>>::finished, this, [this, watcher] {
            const std::vector<QImage> pictures = watcher->result();
            watcher->deleteLater();
            for (std::size_t i = 0; i < pictures.size() && static_cast<int>(i) < list_->count(); ++i)
                list_->item(static_cast<int>(i))->setIcon(QIcon(QPixmap::fromImage(pictures[i])));
            setProperty("pictures", static_cast<int>(pictures.size()));
        });
        watcher->setFuture(QtConcurrent::run([kind = kind_]() {
            std::vector<QImage> out;
            const render::picture::Canvas canvas{0, 0, 120, 120};
            const int count = kind == "tree" ? static_cast<int>(core::tree_type_names().size()) : static_cast<int>(core::frame_style_names().size());
            for (int i = 1; i <= count; ++i) {
                try {
                    const auto prims = kind == "tree" ? render::picture::tree(core::normalize_picture("tree", Json{{"base_tree_type", i}, {"size", 0.9}}), canvas)
                                                      : render::picture::picture_frame(core::normalize_picture("picture_frame", Json{{"frame", i}, {"margin", 2}}), canvas);
                    out.push_back(small_picture(prims, canvas, 56));
                } catch (const std::exception&) {
                    out.emplace_back();
                }
            }
            return out;
        }));
    }

    void show_result() {
        if (!call_.preview) return;
        if (!preview_->isChecked()) {
            call_.preview(std::nullopt);
            return;
        }
        call_.preview(settings());
    }

    FilterWindowCall call_;
    std::string kind_;
    adjust_forms::Form* form_ = nullptr;
    QListWidget* list_ = nullptr;
    QCheckBox* new_layer_ = nullptr;
    QCheckBox* preview_ = nullptr;
    QTimer* timer_ = nullptr;
    Json initial_;

public:
    const Json& initial() const { return initial_; }
    adjust_forms::Form* form() const { return form_; }
};

}  // namespace

QString tree_words(int type) {
    static const char* const kWords[] = {"オーク",       "カエデ",       "マツ",         "ヤシ",     "シラカバ",   "シダレヤナギ", "サクラ",     "タケ",
                                         "イトスギ",     "バオバブ",     "イチョウ",     "トウヒ",   "冬の裸木",   "リンゴ",       "アカシア",   "ポプラ",
                                         "ニレ",         "モクレン",     "バナナ",       "盆栽",     "ジャカランダ", "紅葉したカエデ", "黄葉の木", "オリーブ",
                                         "モミ",         "スギ",         "レモン",       "フジ",     "若木",       "枯れ木",       "ココヤシ",   "低木",
                                         "ウメ",         "雲のような松"};
    return type >= 1 && type <= 34 ? QString::fromUtf8(kWords[type - 1]) : QString::number(type);
}

QString frame_words(int design) {
    static const char* const kWords[] = {"つると花", "つると葉", "ツタ",   "バラ",   "ヒナギク", "木の実",   "竹",   "波",
                                         "ジグザグ", "水玉",     "ロープ", "波形の縁", "二重線", "雪の結晶", "星",   "ハート"};
    return design >= 1 && design <= 16 ? QString::fromUtf8(kWords[design - 1]) : QString::number(design);
}

void open(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.item == nullptr || core::picture_kind(call.item->kind) == nullptr) return;
    RenderDialog dialog(call.window, call, call.item->kind);
    dialog.form()->set_values(dialog.initial());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const Json params = dialog.settings();
    try {
        (void)core::normalize_picture(call.item->kind, [&params] {
            Json given = params;
            given.erase("new_layer");
            return given;
        }());
    } catch (const std::exception& error) {
        call.window->flash(wording::error(QString::fromUtf8(error.what())), 5000, true);
        return;
    }
    if (call.send) call.send(params);
}

}  // namespace genko::app::render_dialogs
