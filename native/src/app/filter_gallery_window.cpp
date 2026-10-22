// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → フィルターギャラリー…'s window
// (app/filter_gallery_window.hpp). The effects and their settings: core/gallery_kinds (PhotoCraft v0.2.0's table); their
// formulas: render/fx_gallery.cpp. The words are Photoshop's (日本語版).

#include "app/filter_gallery_window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPromise>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/colours.hpp"
#include "app/main_window.hpp"
#include "core/adjust.hpp"
#include "core/gallery_kinds.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/page.hpp"
#include "render/channels.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
#include "render/segment/segment.hpp"

namespace genko::app::filter_gallery {

using core::Json;

namespace {

QString words(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// --- Photoshop's words ---------------------------------------------------------------------------------------------------

// A setting's word: the effect's own (its key "<effect>.<setting>"), else the setting's.
QString setting_word(std::string_view filter, std::string_view key) {
    static const std::map<std::string, QString, std::less<>> own{
        {"grain.intensity", QStringLiteral("密度")},
        {"stained_glass.light_intensity", QStringLiteral("光の強さ")},
        {"film_grain.grain", QStringLiteral("粒子")},
        {"glass.distortion", QStringLiteral("ゆがみ")},
        {"note_paper.graininess", QStringLiteral("粒子")},
        {"water_paper.brightness", QStringLiteral("明るさ")},
    };
    static const std::map<std::string, QString, std::less<>> common{
        {"pencil_width", QStringLiteral("鉛筆の幅")},
        {"stroke_pressure", QStringLiteral("ストロークの圧力")},
        {"paper_brightness", QStringLiteral("紙の明るさ")},
        {"number_of_levels", QStringLiteral("レベル数")},
        {"edge_simplicity", QStringLiteral("エッジの単純さ")},
        {"edge_fidelity", QStringLiteral("エッジの正確さ")},
        {"brush_size", QStringLiteral("ブラシサイズ")},
        {"brush_detail", QStringLiteral("ブラシの細かさ")},
        {"texture", QStringLiteral("テクスチャ")},
        {"grain", QStringLiteral("粒子")},
        {"highlight_area", QStringLiteral("ハイライト範囲")},
        {"intensity", QStringLiteral("強さ")},
        {"glow_size", QStringLiteral("光彩のサイズ")},
        {"glow_brightness", QStringLiteral("光彩の明るさ")},
        {"glow_color", QStringLiteral("光彩カラー")},
        {"sharpness", QStringLiteral("シャープ")},
        {"brush_type", QStringLiteral("ブラシの種類")},
        {"stroke_size", QStringLiteral("ストロークの大きさ")},
        {"stroke_detail", QStringLiteral("ストロークの細かさ")},
        {"softness", QStringLiteral("柔らかさ")},
        {"highlight_strength", QStringLiteral("ハイライトの強さ")},
        {"detail", QStringLiteral("詳細")},
        {"smoothness", QStringLiteral("滑らかさ")},
        {"edge_thickness", QStringLiteral("エッジの太さ")},
        {"edge_intensity", QStringLiteral("エッジの強さ")},
        {"posterization", QStringLiteral("ポスタリゼーション")},
        {"stroke_length", QStringLiteral("ストロークの長さ")},
        {"scaling", QStringLiteral("拡大・縮小（%）")},
        {"relief", QStringLiteral("レリーフ")},
        {"light", QStringLiteral("照射方向")},
        {"invert", QStringLiteral("反転")},
        {"definition", QStringLiteral("明瞭度")},
        {"texture_coverage", QStringLiteral("テクスチャの適用範囲")},
        {"shadow_intensity", QStringLiteral("シャドウの強さ")},
        {"edge_width", QStringLiteral("エッジの幅")},
        {"edge_brightness", QStringLiteral("エッジの明るさ")},
        {"direction_balance", QStringLiteral("方向のバランス")},
        {"strength", QStringLiteral("強さ")},
        {"balance", QStringLiteral("バランス")},
        {"black_intensity", QStringLiteral("黒の強さ")},
        {"white_intensity", QStringLiteral("白の強さ")},
        {"dark_intensity", QStringLiteral("暗部の強さ")},
        {"light_intensity", QStringLiteral("明部の強さ")},
        {"spray_radius", QStringLiteral("スプレー半径")},
        {"stroke_direction", QStringLiteral("ストロークの方向")},
        {"stroke_width", QStringLiteral("ストロークの幅")},
        {"contrast", QStringLiteral("コントラスト")},
        {"graininess", QStringLiteral("粒子の粗さ")},
        {"glow_amount", QStringLiteral("光彩の量")},
        {"clear_amount", QStringLiteral("光彩の消去量")},
        {"ripple_size", QStringLiteral("波紋のサイズ")},
        {"ripple_magnitude", QStringLiteral("波紋の強さ")},
        {"charcoal_area", QStringLiteral("木炭の適用範囲")},
        {"chalk_area", QStringLiteral("チョークの適用範囲")},
        {"charcoal_thickness", QStringLiteral("木炭の太さ")},
        {"light_dark_balance", QStringLiteral("明るさ・暗さのバランス")},
        {"foreground_level", QStringLiteral("描画色のレベル")},
        {"background_level", QStringLiteral("背景色のレベル")},
        {"size", QStringLiteral("サイズ")},
        {"pattern_type", QStringLiteral("パターンタイプ")},
        {"image_balance", QStringLiteral("画像のバランス")},
        {"darkness", QStringLiteral("暗さ")},
        {"density", QStringLiteral("密度")},
        {"fiber_length", QStringLiteral("繊維の長さ")},
        {"brightness", QStringLiteral("明るさ")},
        {"crack_spacing", QStringLiteral("溝の間隔")},
        {"crack_depth", QStringLiteral("溝の深さ")},
        {"crack_brightness", QStringLiteral("溝の明るさ")},
        {"grain_type", QStringLiteral("粒子の種類")},
        {"tile_size", QStringLiteral("タイルのサイズ")},
        {"grout_width", QStringLiteral("溝の幅")},
        {"lighten_grout", QStringLiteral("溝の明るさ")},
        {"square_size", QStringLiteral("正方形のサイズ")},
        {"cell_size", QStringLiteral("セルの大きさ")},
        {"border_thickness", QStringLiteral("境界線の太さ")},
    };
    const std::string both = std::string(filter) + "." + std::string(key);
    if (const auto found = own.find(both); found != own.end()) return found->second;
    if (const auto found = common.find(key); found != common.end()) return found->second;
    return words(key);
}

// A choice's value's word.
QString choice_word(std::string_view value) {
    static const std::map<std::string, QString, std::less<>> all{
        {"canvas", QStringLiteral("カンバス")},       {"brick", QStringLiteral("レンガ")},        {"burlap", QStringLiteral("麻布")},
        {"sandstone", QStringLiteral("砂岩")},        {"frosted", QStringLiteral("霜付きガラス")},  {"blocks", QStringLiteral("ブロック")},
        {"tinyLens", QStringLiteral("小さいレンズ")}, {"bottom", QStringLiteral("下")},           {"bottomLeft", QStringLiteral("左下")},
        {"left", QStringLiteral("左")},               {"topLeft", QStringLiteral("左上")},          {"top", QStringLiteral("上")},
        {"topRight", QStringLiteral("右上")},         {"right", QStringLiteral("右")},             {"bottomRight", QStringLiteral("右下")},
        {"simple", QStringLiteral("シンプル")},       {"lightRough", QStringLiteral("明るく粗く")}, {"darkRough", QStringLiteral("暗く粗く")},
        {"wideSharp", QStringLiteral("広くシャープに")}, {"wideBlurry", QStringLiteral("広くぼかす")}, {"sparkle", QStringLiteral("スパークル")},
        {"rightDiagonal", QStringLiteral("右上がり")}, {"horizontal", QStringLiteral("水平")},      {"leftDiagonal", QStringLiteral("右下がり")},
        {"vertical", QStringLiteral("垂直")},         {"dot", QStringLiteral("点")},              {"circle", QStringLiteral("円")},
        {"line", QStringLiteral("線")},               {"regular", QStringLiteral("標準")},         {"soft", QStringLiteral("ソフト")},
        {"sprinkles", QStringLiteral("霧状")},        {"clumped", QStringLiteral("凝集")},         {"contrasty", QStringLiteral("コントラスト")},
        {"enlarged", QStringLiteral("拡大")},         {"stippled", QStringLiteral("点描")},         {"speckle", QStringLiteral("斑点")},
    };
    const auto found = all.find(value);
    return found != all.end() ? found->second : words(value);
}

// --- the pictures --------------------------------------------------------------------------------------------------------

// The effects of `settings` over `picture` — the source's pixels at `area` of the page's pixels at `dpi` —: their marks
// where the op on the whole grid puts them (the origin), the base box `base` on the page's pixels, mixed by `cover` (a
// byte a pixel of the picture; none: everywhere). "RGBA", the alpha as the picture's. Stopped (render::Cancelled), refused
// or past the budget: it throws.
render::Image effects_over(const render::Image& picture, int dpi, const render::Box& area, const render::Box& base, const std::string* cover,
                           const Json& settings) {
    const render::ImageAllocationBudget budget{1ULL << 30};
    const render::Image rgba = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    const render::fx::Kernel* kernel = render::fx::kernel("filter_gallery");
    if (kernel == nullptr || rgba.width() <= 0 || rgba.height() <= 0) return rgba;
    const auto surface = render::fx::eight_surface(rgba);
    const render::fx::PlaceScope place(area.x0, area.y0, render::fx::Rect{base.x0, base.y0, base.x1, base.y1});
    render::fx::apply(*surface, *kernel, settings, core::FilterInks::Meaning, render::fx::Rect{0, 0, rgba.width(), rgba.height()}, cover, dpi / 200.0);
    return render::fx::take_image(*surface);
}

// A picture over the paper (white), to show.
QImage on_paper(const render::Image& picture) {
    const render::Image rgba = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    const std::string bytes = rgba.tobytes();
    const QImage layer(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888);
    QImage out(rgba.width(), rgba.height(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, layer);
    painter.end();
    return out;
}

// The page's pixels at `dpi` (as the page is drawn there).
render::Box page_box(const Source& source, int dpi) { return render::Box{0, 0, render::mm_to_px(source.width_mm, dpi), render::mm_to_px(source.height_mm, dpi)}; }

Json rgb_json(const std::array<int, 3>& c) { return Json::array({c[0], c[1], c[2]}); }

// The preview: dragged to look at another part of the layer.
class PreviewLabel : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void(QPoint moved)> dragged;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        from_ = event->position().toPoint();
        held_ = true;
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!held_) return;
        held_ = false;
        const QPoint moved = event->position().toPoint() - from_;
        if (dragged && (moved.manhattanLength() > 2)) dragged(moved);
    }

private:
    QPoint from_;
    bool held_ = false;
};

constexpr int kPreviewSide = 440;
constexpr QSize kThumb{88, 66};

// The stack the window starts from when nothing is given: the one used last (Photoshop's), else 色鉛筆 alone.
Json& last_stack() {
    static Json last = Json::array({Json{{"filter", "colored_pencil"}, {"params", Json::object()}, {"visible", true}}});
    return last;
}

class GalleryWindow : public QDialog {
public:
    GalleryWindow(QWidget* parent, const Source& source, const Json& now, std::array<int, 3> foreground, std::array<int, 3> background)
        : QDialog(parent), source_(source), foreground_(rgb_json(foreground)), background_(rgb_json(background)) {
        setObjectName(QStringLiteral("filter_gallery"));
        setProperty("kind", QStringLiteral("filter_gallery"));
        setWindowTitle(QStringLiteral("フィルターギャラリー"));
        // the stack it starts from: the settings given (a smart filter set again, 前回のフィルター's), else the last one
        Json given = now.is_object() && now.contains("effects") ? now : Json{{"effects", last_stack()}};
        if (now.is_object()) {
            if (now.contains("foreground")) foreground_ = now["foreground"];
            if (now.contains("background")) background_ = now["background"];
        }
        try {
            given = core::normalize_filter("filter_gallery", given, nullptr);
        } catch (const std::exception&) {
            given = core::normalize_filter("filter_gallery", Json{{"effects", Json::array({Json{{"filter", "colored_pencil"}}})}}, nullptr);
        }
        for (const Json& effect : given["effects"]) entries_.push_back(Entry{effect, serial_++});
        current_ = static_cast<int>(entries_.size()) - 1;
        box_ = {0, 0, source_.width_mm, source_.height_mm};
        view_x_ = source_.width_mm / 2;
        view_y_ = source_.height_mm / 2;
        build();
        rebuild_list();
        show_effect();
        say_too_large();
        find_shown();
    }

    ~GalleryWindow() override {
        if (preview_stop_) preview_stop_->request_stop();
        if (thumbs_) thumbs_->cancel();
    }

    // filter_gallery's settings as the window holds them (their normal form).
    Json settings() const {
        Json effects = Json::array();
        for (const Entry& e : entries_) effects.push_back(e.effect);
        return core::normalize_filter("filter_gallery", Json{{"effects", effects}, {"foreground", foreground_}, {"background", background_}}, nullptr);
    }

private:
    struct Entry {
        Json effect;
        int serial = 0;
    };

    void build() {
        auto* column = new QVBoxLayout(this);
        auto* row = new QHBoxLayout;
        column->addLayout(row, 1);
        // the preview, its zoom
        auto* left = new QVBoxLayout;
        preview_ = new PreviewLabel;
        preview_->setObjectName(QStringLiteral("gallery_preview"));
        preview_->setFixedSize(kPreviewSide, kPreviewSide);
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setFrameShape(QFrame::StyledPanel);
        preview_->setToolTip(QStringLiteral("ドラッグしてほかの所を見ます"));
        preview_->dragged = [this](QPoint moved) { pan(moved); };
        left->addWidget(preview_);
        auto* under = new QHBoxLayout;
        zoom_ = new QComboBox;
        zoom_->setObjectName(QStringLiteral("gallery_zoom"));
        zoom_->addItem(QStringLiteral("全体"), 0.0);
        zoom_->addItem(QStringLiteral("50%"), 0.5);
        zoom_->addItem(QStringLiteral("100%"), 1.0);
        zoom_->addItem(QStringLiteral("200%"), 2.0);
        zoom_->setCurrentIndex(2);
        connect(zoom_, &QComboBox::currentIndexChanged, this, [this] {
            place();
            run_preview();
        });
        under->addWidget(zoom_);
        busy_ = new QLabel(QStringLiteral("プレビューを作っています…"));
        busy_->setObjectName(QStringLiteral("gallery_preview_busy"));
        busy_->setProperty("role", "hint");
        busy_->setVisible(false);
        under->addWidget(busy_, 1);
        left->addLayout(under);
        row->addLayout(left);
        // the folders and their effects
        folders_ = new QTreeWidget;
        folders_->setObjectName(QStringLiteral("gallery_effects"));
        folders_->setHeaderHidden(true);
        folders_->setIconSize(kThumb);
        folders_->setMinimumWidth(240);
        for (const std::string_view category : core::gallery_categories()) {
            auto* folder = new QTreeWidgetItem(folders_, QStringList{words(category)});
            folder->setFlags(Qt::ItemIsEnabled);
            QFont bold = folder->font(0);
            bold.setBold(true);
            folder->setFont(0, bold);
            for (const core::GalleryEffectKind& e : core::gallery_effects()) {
                if (e.category != category) continue;
                auto* item = new QTreeWidgetItem(folder, QStringList{words(e.name)});
                item->setData(0, Qt::UserRole, words(e.filter));
                thumb_items_.push_back(item);
            }
        }
        folders_->topLevelItem(0)->setExpanded(true);
        connect(folders_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
            const QString key = item->data(0, Qt::UserRole).toString();
            if (!key.isEmpty()) set_filter(key.toStdString());
        });
        row->addWidget(folders_, 1);
        // the chosen effect's settings, the stack
        auto* right = new QVBoxLayout;
        effect_ = new QComboBox;
        effect_->setObjectName(QStringLiteral("gallery_effect"));
        for (const core::GalleryEffectKind& e : core::gallery_effects()) effect_->addItem(words(e.name), words(e.filter));
        connect(effect_, &QComboBox::activated, this, [this](int at) { set_filter(effect_->itemData(at).toString().toStdString()); });
        right->addWidget(effect_);
        form_box_ = new QVBoxLayout;
        right->addLayout(form_box_);
        reset_ = new QPushButton(QStringLiteral("既定に戻す"));
        reset_->setObjectName(QStringLiteral("gallery_reset"));
        connect(reset_, &QPushButton::clicked, this, [this] {
            if (form_ != nullptr) form_->reset();
        });
        right->addWidget(reset_, 0, Qt::AlignRight);
        stack_ = new QListWidget;
        stack_->setObjectName(QStringLiteral("gallery_stack"));
        stack_->setDragDropMode(QAbstractItemView::InternalMove);
        stack_->setToolTip(QStringLiteral("下のエフェクトレイヤーから順にかけます（ドラッグで並べ替え、チェックで表示）"));
        connect(stack_, &QListWidget::currentRowChanged, this, [this](int row_at) {
            if (quiet_ || row_at < 0) return;
            current_ = static_cast<int>(entries_.size()) - 1 - row_at;
            show_effect();
        });
        connect(stack_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
            if (quiet_) return;
            for (Entry& e : entries_) {
                if (e.serial == item->data(Qt::UserRole).toInt()) e.effect["visible"] = item->checkState() == Qt::Checked;
            }
            schedule();
        });
        connect(stack_->model(), &QAbstractItemModel::rowsMoved, this, [this] { reordered(); });
        right->addWidget(stack_, 1);
        auto* buttons_row = new QHBoxLayout;
        add_ = new QPushButton(QStringLiteral("新しいエフェクトレイヤー"));
        add_->setObjectName(QStringLiteral("gallery_add"));
        connect(add_, &QPushButton::clicked, this, [this] { add_effect(); });
        remove_ = new QPushButton(QStringLiteral("エフェクトレイヤーを削除"));
        remove_->setObjectName(QStringLiteral("gallery_remove"));
        connect(remove_, &QPushButton::clicked, this, [this] { remove_effect(); });
        buttons_row->addWidget(add_);
        buttons_row->addWidget(remove_);
        right->addLayout(buttons_row);
        auto* holder = new QWidget;
        holder->setLayout(right);
        holder->setMinimumWidth(280);
        row->addWidget(holder);
        too_large_ = new QLabel;
        too_large_->setObjectName(QStringLiteral("gallery_too_large"));
        too_large_->setProperty("role", "warning");
        too_large_->setWordWrap(true);
        too_large_->setVisible(false);
        column->addWidget(too_large_);
        auto* note = new QLabel(QStringLiteral("見え方は Photoshop のフィルターギャラリーに近づけたもので、同じではありません。長さは 200 dpi の画素で数えます。"));
        note->setProperty("role", "hint");
        note->setWordWrap(true);
        column->addWidget(note);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("かける"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        column->addWidget(buttons);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(250);
        connect(timer_, &QTimer::timeout, this, [this] { run_preview(); });
    }

    // --- the stack ---

    void rebuild_list() {
        quiet_ = true;
        stack_->clear();
        for (int i = static_cast<int>(entries_.size()) - 1; i >= 0; --i) {
            const Entry& e = entries_[static_cast<std::size_t>(i)];
            const core::GalleryEffectKind* kind = core::gallery_effect(e.effect.value("filter", std::string()));
            auto* item = new QListWidgetItem(kind != nullptr ? words(kind->name) : QString(), stack_);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
            item->setCheckState(e.effect.value("visible", true) ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, e.serial);
        }
        stack_->setCurrentRow(static_cast<int>(entries_.size()) - 1 - current_);
        remove_->setEnabled(entries_.size() > 1);
        add_->setEnabled(entries_.size() < core::kGalleryMostEffects);
        quiet_ = false;
    }

    // The list dragged into another order: the stack in its order (the list's items as they are — a drop may still use
    // them —, the one chosen kept).
    void reordered() {
        if (quiet_) return;
        const int keep = entries_[static_cast<std::size_t>(current_)].serial;
        std::vector<Entry> order;
        for (int r = stack_->count() - 1; r >= 0; --r) {
            const int serial = stack_->item(r)->data(Qt::UserRole).toInt();
            for (const Entry& e : entries_)
                if (e.serial == serial) order.push_back(e);
        }
        if (order.size() != entries_.size()) return;
        entries_ = std::move(order);
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].serial == keep) current_ = static_cast<int>(i);
        schedule();
    }

    void add_effect() {
        if (entries_.size() >= core::kGalleryMostEffects) return;
        Entry copy{entries_[static_cast<std::size_t>(current_)].effect, serial_++};
        copy.effect["visible"] = true;
        entries_.insert(entries_.begin() + current_ + 1, std::move(copy));
        ++current_;
        rebuild_list();
        show_effect();
        schedule();
    }

    void remove_effect() {
        if (entries_.size() <= 1) return;
        entries_.erase(entries_.begin() + current_);
        current_ = std::min(current_, static_cast<int>(entries_.size()) - 1);
        rebuild_list();
        show_effect();
        schedule();
    }

    void set_filter(const std::string& key) {
        Json& effect = entries_[static_cast<std::size_t>(current_)].effect;
        if (effect.value("filter", std::string()) == key || core::gallery_effect(key) == nullptr) return;
        const bool visible = effect.value("visible", true);
        effect = core::normalize_filter("filter_gallery", Json{{"effects", Json::array({Json{{"filter", key}, {"visible", visible}}})}}, nullptr)["effects"][0];
        rebuild_list();
        show_effect();
        schedule();
    }

    // The current effect's settings as fields (its table: numbers, choices, checks, ネオン光彩's colour).
    void show_effect() {
        const Json& effect = entries_[static_cast<std::size_t>(current_)].effect;
        const std::string key = effect.value("filter", std::string());
        const core::GalleryEffectKind* kind = core::gallery_effect(key);
        if (kind == nullptr) return;
        effect_->setCurrentIndex(effect_->findData(words(kind->filter)));
        if (form_ != nullptr) {  // (never from the form's own signal: a list, a folder or the effects' list chose another)
            form_box_->removeWidget(form_);
            delete form_;
            form_ = nullptr;
        }
        form_ = new adjust_forms::Form;
        form_->setObjectName(QStringLiteral("gallery_form"));
        for (const core::AdjustParam& p : kind->params) {
            const std::string k(p.key);
            const QString label = setting_word(kind->filter, p.key);
            switch (p.type) {
                case core::AdjustParam::Type::Choice: {
                    std::vector<std::pair<QString, Json>> options;
                    for (const std::string_view c : p.choices) options.emplace_back(choice_word(c), Json(std::string(c)));
                    form_->choice(k, label, std::move(options), Json(std::string(p.text)));
                    break;
                }
                case core::AdjustParam::Type::Bool: form_->check(k, label, p.fallback != 0); break;
                case core::AdjustParam::Type::Rgb:
                    form_->colour(k, label, {static_cast<int>(p.numbers[0]), static_cast<int>(p.numbers[1]), static_cast<int>(p.numbers[2])});
                    break;
                default: form_->number(k, label, p.lo, p.hi, p.fallback, 0); break;
            }
        }
        form_->set_values(effect.contains("params") ? effect["params"] : Json::object());
        connect(form_, &adjust_forms::Form::changed, this, [this] {
            Json& now = entries_[static_cast<std::size_t>(current_)].effect;
            Json params = now.contains("params") ? now["params"] : Json::object();
            const Json values = form_->values();
            for (const auto& [k, v] : values.items()) params[k] = v;
            now["params"] = std::move(params);
            schedule();
        });
        form_box_->addWidget(form_);
    }

    // --- the preview ---

    double zoom() const { return zoom_->currentData().toDouble(); }

    // What the layer shows on (a small picture of it, on a thread), then the view there, the preview and the pictures.
    void find_shown() {
        busy_->setVisible(true);
        setProperty("previewing", true);
        auto* watcher = new QFutureWatcher<std::array<double, 4>>(this);
        connect(watcher, &QFutureWatcher<std::array<double, 4>>::finished, this, [this, watcher] {
            box_ = watcher->result();
            watcher->deleteLater();
            if (!panned_) {
                view_x_ = (box_[0] + box_[2]) / 2;
                view_y_ = (box_[1] + box_[3]) / 2;
            }
            found_ = true;
            place();
            run_preview();
            start_thumbnails();
        });
        watcher->setFuture(QtConcurrent::run([source = source_]() { return shown_box(source); }));
    }

    // The part looked at for the zoom (the page's pixels at the resolution it is drawn at) and how much it is enlarged.
    void place() {
        const double z = zoom();
        const double page_w = std::max(source_.width_mm, 1.0), page_h = std::max(source_.height_mm, 1.0);
        double dpi = source_.dpi;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // (mm)
        shown_factor_ = 1;
        if (z <= 0) {  // (全体: what the layer shows on, fitted)
            x0 = box_[0];
            y0 = box_[1];
            x1 = box_[2];
            y1 = box_[3];
            if (x1 <= x0 || y1 <= y0) {
                x0 = y0 = 0;
                x1 = page_w;
                y1 = page_h;
            }
            dpi = std::min(source_.dpi, std::floor(std::min(kPreviewSide / ((x1 - x0) / 25.4), kPreviewSide / ((y1 - y0) / 25.4))));
        } else {
            dpi = source_.dpi * std::min(z, 1.0);
            shown_factor_ = std::max(z, 1.0);
            const double side_mm = kPreviewSide / shown_factor_ / dpi * 25.4;
            x0 = std::clamp(view_x_ - side_mm / 2, 0.0, std::max(0.0, page_w - side_mm));
            y0 = std::clamp(view_y_ - side_mm / 2, 0.0, std::max(0.0, page_h - side_mm));
            x1 = std::min(page_w, x0 + side_mm);
            y1 = std::min(page_h, y0 + side_mm);
        }
        shown_dpi_ = std::max(1, static_cast<int>(std::lround(dpi)));
        shown_area_ = render::Box{static_cast<int>(std::floor(x0 / 25.4 * shown_dpi_)), static_cast<int>(std::floor(y0 / 25.4 * shown_dpi_)),
                                  static_cast<int>(std::ceil(x1 / 25.4 * shown_dpi_)), static_cast<int>(std::ceil(y1 / 25.4 * shown_dpi_))};
    }

    void pan(QPoint moved) {
        if (zoom() <= 0 || !found_) return;
        const double per_mm = shown_dpi_ / 25.4 * shown_factor_;
        view_x_ -= moved.x() / per_mm;
        view_y_ -= moved.y() / per_mm;
        panned_ = true;
        place();
        run_preview();
    }

    void show(const QImage& image) {
        if (image.isNull()) return;
        const QImage scaled = shown_factor_ > 1 ? image.scaled(image.size() * static_cast<int>(shown_factor_), Qt::KeepAspectRatio, Qt::FastTransformation) : image;
        preview_->setPixmap(QPixmap::fromImage(scaled));
    }

    void schedule() {
        say_too_large();
        timer_->start();
    }

    void idle() {
        busy_->setVisible(false);
        setProperty("previewing", false);
    }

    // The effects over the part looked at — the pixels drawn and laid on there on a thread (the last asked for wins).
    void run_preview() {
        timer_->stop();
        if (preview_stop_) preview_stop_->request_stop();
        if (!found_) return;  // (what shows is being found: the preview after it)
        Json params;
        try {
            params = settings();
        } catch (const std::exception&) {
            idle();
            return;
        }
        auto stop = std::make_shared<std::stop_source>();
        preview_stop_ = stop;
        busy_->setVisible(true);
        setProperty("previewing", true);
        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, stop] {
            const QImage image = watcher->result();
            watcher->deleteLater();
            if (preview_stop_ != stop || stop->stop_requested()) return;  // (not wanted any more: a later one says when it is done)
            idle();
            ++previews_;
            setProperty("previews", previews_);
            show(image);
        });
        watcher->setFuture(
            QtConcurrent::run([source = source_, params, dpi = shown_dpi_, area = shown_area_, token = stop->get_token()]() -> QImage {
                const render::segment::StopScope scope(token);
                try {
                    return on_paper(laid_on(source, dpi, area, params));
                } catch (...) {  // (stopped, refused, past the budget: none)
                    return QImage();
                }
            }));
    }

    // Whether the stack can be worked at once at the resolution the layer is drawn at for print (the imaging budget holds
    // one window of the smallest tiles: render/fx), else said under the window.
    void say_too_large() {
        if (too_large_ == nullptr) return;
        bool fits = true;
        try {
            const render::fx::Kernel* kernel = render::fx::kernel("filter_gallery");
            render::fx::Ctx ctx;
            ctx.s = source_.print_s;
            const int halo = kernel != nullptr && kernel->halo ? kernel->halo(settings(), ctx).radius : 0;
            const double scale = source_.print_s / std::max(source_.dpi / 200.0, 1e-9);
            const double w = std::max(1.0, source_.grid_width * scale), h = std::max(1.0, source_.grid_height * scale);
            const double side = 64.0 + 2.0 * halo;
            const double bytes_a_pixel = 4 * 16 + 4 * 4 + (kernel != nullptr ? static_cast<double>(kernel->work_bytes) : 0.0) + (source_.inks ? 9 * 8 : 0);
            fits = std::min(side, w) * std::min(side, h) * bytes_a_pixel < 2.0 * 1024 * 1024 * 1024;
        } catch (const std::exception&) {
        }
        too_large_->setText(fits ? QString()
                                 : QStringLiteral("効果が多すぎるか大きすぎて、このレイヤーの解像度（%1 dpi）では一度に掛けられません（作業に使える記憶の上限を超えます）。"
                                                  "エフェクトレイヤーを減らすか、設定を小さくします。")
                                       .arg(std::lround(200.0 * source_.print_s)));
        too_large_->setVisible(!fits);
        setProperty("too_large", !fits);
    }

    // --- the thumbnails: each effect alone with its settings by default over a small copy of the layer, in turn ---

    void start_thumbnails() {
        const int dpi = static_cast<int>(std::lround(std::min(source_.dpi, 100.0)));
        const double cx = (box_[0] + box_[2]) / 2, cy = (box_[1] + box_[3]) / 2;
        const int px = static_cast<int>(std::lround(cx / 25.4 * dpi)), py = static_cast<int>(std::lround(cy / 25.4 * dpi));
        const render::Box area{px - kThumb.width() / 2, py - kThumb.height() / 2, px + kThumb.width() - kThumb.width() / 2, py + kThumb.height() - kThumb.height() / 2};
        std::vector<std::string> keys;
        for (const QTreeWidgetItem* item : thumb_items_) keys.push_back(item->data(0, Qt::UserRole).toString().toStdString());
        thumbs_ = new QFutureWatcher<std::pair<int, QImage>>(this);
        connect(thumbs_, &QFutureWatcher<std::pair<int, QImage>>::resultReadyAt, this, [this](int at) {
            const auto [index, image] = thumbs_->resultAt(at);
            if (index >= 0 && index < static_cast<int>(thumb_items_.size()) && !image.isNull()) {
                thumb_items_[static_cast<std::size_t>(index)]->setIcon(0, QIcon(QPixmap::fromImage(image)));
                setProperty("thumbnails", property("thumbnails").toInt() + 1);
            }
        });
        const Json fg = foreground_, bg = background_;
        thumbs_->setFuture(QtConcurrent::run([source = source_, keys, fg, bg, dpi, area](QPromise<std::pair<int, QImage>>& promise) {
            render::Image sample;
            try {
                sample = source.picture ? source.picture(dpi, area) : render::Image();
            } catch (const std::exception&) {
                return;
            }
            if (sample.width() <= 0 || sample.height() <= 0) return;
            const render::Box at{std::max(0, area.x0), std::max(0, area.y0), std::max(0, area.x0) + sample.width(), std::max(0, area.y0) + sample.height()};
            const render::Box page = page_box(source, dpi);
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (promise.isCanceled()) return;
                const Json settings{{"effects", Json::array({Json{{"filter", keys[i]}}})}, {"foreground", fg}, {"background", bg}};
                QImage made;
                try {
                    made = on_paper(effects_over(sample, dpi, at, page, nullptr, settings));
                } catch (...) {
                }
                promise.addResult(std::make_pair(static_cast<int>(i), made));
            }
        }));
    }

    Source source_;
    Json foreground_, background_;
    std::vector<Entry> entries_;
    int current_ = 0;
    int serial_ = 0;
    bool quiet_ = false;
    std::array<double, 4> box_{};  // (what the layer shows on, mm)
    bool found_ = false, panned_ = false;
    double view_x_ = 0, view_y_ = 0;
    int shown_dpi_ = 200;
    render::Box shown_area_;
    double shown_factor_ = 1;
    int previews_ = 0;
    std::shared_ptr<std::stop_source> preview_stop_;
    QFutureWatcher<std::pair<int, QImage>>* thumbs_ = nullptr;
    std::vector<QTreeWidgetItem*> thumb_items_;
    PreviewLabel* preview_ = nullptr;
    QComboBox* zoom_ = nullptr;
    QLabel* busy_ = nullptr;
    QTreeWidget* folders_ = nullptr;
    QComboBox* effect_ = nullptr;
    QVBoxLayout* form_box_ = nullptr;
    adjust_forms::Form* form_ = nullptr;
    QPushButton* reset_ = nullptr;
    QListWidget* stack_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLabel* too_large_ = nullptr;
    QTimer* timer_ = nullptr;
};

}  // namespace

Source source_of(const MainWindow& window, const core::Page& page, const core::Layer& layer, int filter) {
    auto book = window.session().snapshot();
    auto shown = std::make_shared<core::Page>(page);
    const std::string id = layer.id;
    // (the layer alone as the op works it: no layer style, mask, vector mask, opacity, blend, clipping or tint — a paint
    // layer's pixels not cut to the panels —; a smart object's smart filter set again: the filters below it alone)
    for (core::Layer& l : shown->layers) {
        if (l.id != id) continue;
        l.visible = true;
        l.effect.reset();
        l.mask.reset();
        l.vector_mask.reset();
        l.opacity = 1.0;
        l.blend = "normal";
        l.clip = false;
        l.color.reset();
        if (l.kind == core::LayerKind::Raster && l.stroke_count() == 0 && l.patches.empty()) l.panel_clip = false;
        if (filter >= 0 && l.smart && l.smart->is_object() && l.smart->contains("filters")) {
            Json smart = *l.smart;
            Json& filters = smart["filters"];
            if (filters.is_array() && filter < static_cast<int>(filters.size())) filters.erase(filters.begin() + filter, filters.end());
            l.smart = std::move(smart);
        }
    }
    Source source;
    source.width_mm = page.spec.width_mm.value();
    source.height_mm = page.spec.height_mm.value();
    const render::fx::Grid grid = render::fx::layer_grid(page, layer);
    source.dpi = 200.0 * grid.s;
    source.grid_width = grid.width;
    source.grid_height = grid.height;
    source.print_s = layer.kind == core::LayerKind::Smart ? std::max(grid.s, page.spec.dpi.value() / 200.0) : grid.s;
    source.inks = page.color.has_value();
    source.selection = window.selection_area();
    // (the channel drawn on — PC1c: lent as an 8-bit grey at the working 200 dpi —: its density)
    const std::string target = window.channel_view().target;
    if (!target.empty()) {
        for (const core::Channel& channel : page.channels) {
            if (channel.name != target && channel.id != target) continue;
            const auto lent = std::make_shared<core::Channel>(channel);
            source.dpi = 200.0;
            source.grid_width = static_cast<std::uint32_t>(render::mm_to_px(source.width_mm, 200));
            source.grid_height = static_cast<std::uint32_t>(render::mm_to_px(source.height_mm, 200));
            source.print_s = 1.0;
            source.inks = false;
            const double width_mm = source.width_mm, height_mm = source.height_mm;
            source.picture = [lent, width_mm, height_mm](int dpi, const render::Box& asked) {
                const render::channels::Density density(*lent);
                const int w = render::mm_to_px(width_mm, dpi), h = render::mm_to_px(height_mm, dpi);
                const render::Box area{std::max(0, asked.x0), std::max(0, asked.y0), std::min(w, asked.x1), std::min(h, asked.y1)};
                std::string bytes;
                for (int y = area.y0; y < area.y1; ++y)
                    for (int x = area.x0; x < area.x1; ++x) {
                        const auto v = static_cast<char>(std::lround(std::clamp(density.at((x + 0.5) / w, (y + 0.5) / h), 0.0f, 1.0f) * 255.0f));
                        bytes.append({v, v, v, static_cast<char>(255)});
                    }
                return render::Image::frombytes("RGBA", render::Size{std::max(0, area.x1 - area.x0), std::max(0, area.y1 - area.y0)}, bytes);
            };
            return source;
        }
    }
    source.picture = [book, shown, id](int dpi, const render::Box& area) {
        for (const core::Layer& l : shown->layers) {
            if (l.id == id) return render::layer_image_over(*shown, l, dpi, area, book.get(), true);
        }
        return render::Image();
    };
    return source;
}

std::array<double, 4> shown_box(const Source& source) {
    std::array<double, 4> box{0, 0, source.width_mm, source.height_mm};
    try {
        constexpr int low = 36;
        const render::Image small = source.picture(low, page_box(source, low)).convert("RGBA");
        const std::string bytes = small.tobytes();
        int x0 = small.width(), y0 = small.height(), x1 = 0, y1 = 0;
        for (int y = 0; y < small.height(); ++y)
            for (int x = 0; x < small.width(); ++x) {
                if (static_cast<unsigned char>(bytes[(std::size_t(y) * std::size_t(small.width()) + std::size_t(x)) * 4 + 3]) == 0) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
        if (x1 > x0 && y1 > y0)
            box = {x0 * 25.4 / low, y0 * 25.4 / low, std::min(source.width_mm, x1 * 25.4 / low), std::min(source.height_mm, y1 * 25.4 / low)};
    } catch (const std::exception&) {
    }
    return box;
}

render::Image laid_on(const Source& source, int dpi, const render::Box& asked, const Json& settings) {
    const render::Box page = page_box(source, dpi);
    const render::Box area{std::max(page.x0, asked.x0), std::max(page.y0, asked.y0), std::min(page.x1, asked.x1), std::min(page.y1, asked.y1)};
    if (area.width() <= 0 || area.height() <= 0 || !source.picture) return render::Image();
    const render::Image picture = source.picture(dpi, area);
    const render::Box at{area.x0, area.y0, area.x0 + picture.width(), area.y0 + picture.height()};
    if (!source.selection) return effects_over(picture, dpi, at, page, nullptr, settings);
    // (inside the selection: its coverage over the part, the base box its box — as the op's run)
    const render::selection::AreaMask inside = render::selection::area_mask(*source.selection, dpi);
    const render::Image mask = inside.mask.mode() == "L" ? inside.mask : inside.mask.convert("L");
    const std::string mask_bytes = mask.tobytes();
    const int mx = static_cast<int>(inside.x0), my = static_cast<int>(inside.y0);
    std::string cover(std::size_t(std::max(0, picture.width())) * std::size_t(std::max(0, picture.height())), '\0');
    int bx0 = page.x1, by0 = page.y1, bx1 = page.x0, by1 = page.y0;  // (the box it covers at all, on the page)
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            const char v = mask_bytes[std::size_t(y) * std::size_t(mask.width()) + std::size_t(x)];
            if (v == 0) continue;
            const int gx = mx + x, gy = my + y;
            bx0 = std::min(bx0, gx);
            by0 = std::min(by0, gy);
            bx1 = std::max(bx1, gx + 1);
            by1 = std::max(by1, gy + 1);
            if (gx >= at.x0 && gx < at.x1 && gy >= at.y0 && gy < at.y1) cover[std::size_t(gy - at.y0) * std::size_t(picture.width()) + std::size_t(gx - at.x0)] = v;
        }
    const render::Box base = bx1 > bx0 && by1 > by0 ? render::Box{bx0, by0, bx1, by1} : page;
    return effects_over(picture, dpi, at, base, &cover, settings);
}

std::optional<Json> dialog(QWidget* parent, const Source& source, const Json& now, std::array<int, 3> foreground, std::array<int, 3> background) {
    GalleryWindow window(parent, source, now, foreground, background);
    if (ask::exec(&window) != QDialog::Accepted) return std::nullopt;
    try {
        Json chosen = window.settings();
        last_stack() = chosen["effects"];
        return chosen;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void open(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.layer == nullptr) return;
    MainWindow& window = *call.window;
    render::use_brushes_of(window.book());
    const Source source = source_of(window, *call.page, *call.layer, call.filter);  // (its pictures made on threads)
    const auto& rgb = window.pen().rgb;
    const std::array<int, 3> main{static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
    const std::array<int, 3> sub = window.colours() != nullptr ? window.colours()->sub_rgb() : std::array<int, 3>{255, 255, 255};
    const auto chosen = dialog(&window, source, call.now, main, sub);
    if (chosen && call.send) call.send(*chosen);
}

}  // namespace genko::app::filter_gallery
