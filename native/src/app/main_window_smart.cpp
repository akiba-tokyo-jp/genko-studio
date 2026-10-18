// smart-objects@1 (PC3 A0, design section A 5; Photoshop's names, the user's decision 8 of PC2): the window's smart
// objects — ファイル → 画像を読み込む… (main_window_materials.cpp: the file placed as a smart object, decision 2) and
// 埋め込みを配置…; レイヤー → スマートオブジェクト, スマートフィルター, ラスタライズ, レイヤーの内容オプション…. Every change
// is an op (render/ops_smart.cpp). The commands of PC3's later steps (contents and links, smart filters, stack modes,
// warps) are in their places and not usable yet: those steps write them here.

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <fstream>

#include "app/ask.hpp"
#include "app/color_settings_dialog.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/smart_panel.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/smart.hpp"
#include "render/smart_decode.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr const char* kLater = "この版ではまだ使えません";

// Photoshop's stack modes (レイヤー → スマートオブジェクト → スタックモード), the op's names.
const std::vector<std::pair<const char*, const char*>>& stack_modes() {
    static const std::vector<std::pair<const char*, const char*>> modes = {
        {"entropy", "エントロピー"}, {"kurtosis", "尖度"},     {"maximum", "最大"},   {"mean", "平均"},
        {"median", "中央値"},        {"minimum", "最小"},     {"range", "範囲"},     {"skewness", "歪度"},
        {"standard_deviation", "標準偏差"}, {"summation", "合計"}, {"variance", "分散"}, {"none", "なし"}};
    return modes;
}

}  // namespace

void MainWindow::build_smart_actions() {
    const auto later = [this](const char* attribute, const QString& title) {
        QAction* act = make(QString::fromLatin1(attribute), title, [] {}, {}, QString::fromUtf8(kLater));
        act->setEnabled(false);
        return act;
    };
    // ファイル
    make("act_place_embedded", QStringLiteral("埋め込みを配置…"), [this] { place_embedded(); }, {},
         QStringLiteral("画像をスマートオブジェクトとしてページの中央に置きます（元のファイルのまま。大きければページに収めます）"));
    later("act_place_linked", QStringLiteral("リンクを配置…"));
    later("act_package", QStringLiteral("パッケージ…"));
    // レイヤー → スマートオブジェクト
    make("act_smart_convert", QStringLiteral("スマートオブジェクトに変換"), [this] { smart_convert(); }, {},
         QStringLiteral("選んだレイヤーをスマートオブジェクトにまとめます（中身はそのまま。拡大・縮小しても劣化しません）"));
    make("act_smart_copy", QStringLiteral("新規スマートオブジェクト（コピー）"), [this] { smart_copy(); }, {},
         QStringLiteral("選んだスマートオブジェクトを、中身ごと別のものとして複製します"));
    later("act_smart_edit", QStringLiteral("コンテンツを編集"));
    later("act_smart_export", QStringLiteral("コンテンツを書き出し…"));
    later("act_smart_replace", QStringLiteral("コンテンツを置換…"));
    later("act_smart_relink", QStringLiteral("ファイルに再リンク…"));
    later("act_smart_embed", QStringLiteral("埋め込みに変換"));
    later("act_smart_link", QStringLiteral("リンクに変換…"));
    later("act_smart_update", QStringLiteral("変更されたコンテンツを更新"));
    later("act_smart_update_all", QStringLiteral("変更されたすべてのコンテンツを更新"));
#ifdef Q_OS_WIN
    later("act_smart_reveal", QStringLiteral("エクスプローラーで表示"));
#else
    later("act_smart_reveal", QStringLiteral("ファイルマネージャーで表示"));
#endif
    for (const auto& [mode, label] : stack_modes()) later(("act_smart_stack_" + std::string(mode)).c_str(), QString::fromUtf8(label));
    later("act_smart_warp", QStringLiteral("ワープ"));
    later("act_smart_perspective_warp", QStringLiteral("遠近法ワープ"));
    later("act_smart_puppet_warp", QStringLiteral("パペットワープ"));
    make("act_smart_rasterize", QStringLiteral("ラスタライズ"), [this] { rasterize("smart"); }, {},
         QStringLiteral("選んだスマートオブジェクトを、見えているとおりの画素のレイヤーにします"));
    // レイヤー → スマートフィルター
    make("act_smart_filters_convert", QStringLiteral("スマートフィルター用に変換"), [this] { smart_convert(); }, {},
         QStringLiteral("選んだレイヤーをスマートオブジェクトにします（フィルターをあとから直せる形）"));
    later("act_smart_filters_off", QStringLiteral("スマートフィルターを無効にする"));
    later("act_smart_filter_mask_delete", QStringLiteral("フィルターマスクを削除"));
    later("act_smart_filter_mask_off", QStringLiteral("フィルターマスクを無効にする"));
    later("act_smart_filters_clear", QStringLiteral("スマートフィルターをクリア"));
    // レイヤー → ラスタライズ
    later("act_rasterize_type", QStringLiteral("テキスト"));
    make("act_rasterize_fill", QStringLiteral("塗りつぶし内容"), [this] { rasterize("fill"); }, {},
         QStringLiteral("選んだ塗りつぶし・グラデーションのレイヤーを、画素のレイヤーにします"));
    make("act_rasterize_smart", QStringLiteral("スマートオブジェクト"), [this] { rasterize("smart"); }, {},
         QStringLiteral("選んだスマートオブジェクトを、見えているとおりの画素のレイヤーにします"));
    make("act_rasterize_layer", QStringLiteral("レイヤー"), [this] { rasterize("layer"); }, {},
         QStringLiteral("選んだレイヤー（スマートオブジェクト・塗りつぶし・配置した画像）を画素のレイヤーにします。マスクはそのまま残ります"));
    make("act_rasterize_all", QStringLiteral("すべてのレイヤー"), [this] { rasterize("all"); }, {},
         QStringLiteral("このページのスマートオブジェクト・塗りつぶし・配置した画像を、まとめて画素のレイヤーにします（ペンの線はそのまま）"));
    make("act_layer_content_options", QStringLiteral("レイヤーの内容オプション…"), [this] { content_options(); }, {},
         QStringLiteral("選んだレイヤーの中身を直します（塗りつぶし・色調補正は設定、スマートオブジェクトはコンテンツの編集）"));
}

void MainWindow::build_smart_menus(QMenu* layers, QMenu* file) {
    // (ファイル: after 画像を読み込む…)
    const QList<QAction*> items = file->actions();
    const qsizetype at = items.indexOf(action("act_import"));
    QAction* before = at >= 0 && at + 1 < items.size() ? items.at(at + 1) : nullptr;
    for (const char* name : {"act_place_embedded", "act_place_linked", "act_package"}) file->insertAction(before, action(name));
    layers->addSeparator();
    QMenu* smart = layers->addMenu(QStringLiteral("スマートオブジェクト"));
    smart->setObjectName(QStringLiteral("menu_smart"));
    for (const char* name : {"act_smart_convert", "act_smart_copy"}) smart->addAction(action(name));
    smart->addSeparator();
    for (const char* name : {"act_smart_edit", "act_smart_export", "act_smart_replace"}) smart->addAction(action(name));
    smart->addSeparator();
    for (const char* name : {"act_smart_relink", "act_smart_embed", "act_smart_link", "act_smart_update", "act_smart_update_all", "act_smart_reveal"})
        smart->addAction(action(name));
    smart->addSeparator();
    QMenu* stack = smart->addMenu(QStringLiteral("スタックモード"));
    for (const auto& [mode, label] : stack_modes()) stack->addAction(action(QString::fromLatin1("act_smart_stack_%1").arg(QString::fromLatin1(mode))));
    smart->addSeparator();
    for (const char* name : {"act_smart_warp", "act_smart_perspective_warp", "act_smart_puppet_warp"}) smart->addAction(action(name));
    smart->addSeparator();
    smart->addAction(action("act_smart_rasterize"));
    QMenu* filters = layers->addMenu(QStringLiteral("スマートフィルター"));
    filters->setObjectName(QStringLiteral("menu_smart_filters"));
    filters->addAction(action("act_smart_filters_convert"));
    filters->addSeparator();
    for (const char* name : {"act_smart_filters_off", "act_smart_filter_mask_delete", "act_smart_filter_mask_off", "act_smart_filters_clear"})
        filters->addAction(action(name));
    QMenu* rasterize = layers->addMenu(QStringLiteral("ラスタライズ"));
    rasterize->setObjectName(QStringLiteral("menu_rasterize"));
    for (const char* name : {"act_rasterize_type", "act_rasterize_fill", "act_rasterize_smart", "act_rasterize_layer"}) rasterize->addAction(action(name));
    rasterize->addSeparator();
    rasterize->addAction(action("act_rasterize_all"));
    layers->addAction(action("act_layer_content_options"));
}

bool MainWindow::place_embedded(std::optional<QString> chosen) {
    if (current_page() == nullptr) return false;
    const Asked asked = asking();
    if (!session_->path()) {
        flash(QStringLiteral("画像を置く前に、原稿を保存します（ファイル → 別の場所に保存）"), 6000);
        return false;
    }
    const QString file =
        chosen ? *chosen : ask::open_path(this, QStringLiteral("埋め込みを配置"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.gif *.psd)"));
    if (file.isEmpty() || !still(asked)) return false;
    return place_smart(file, false);
}

bool MainWindow::place_smart(const QString& file, bool as_placed) {
    const Asked asked = asking();
    std::string bytes;
    render::smart::FileInfo info;
    Json use;
    try {
        std::ifstream in(core::path_from_utf8(file.toStdString()), std::ios::binary | std::ios::ate);
        if (!in) throw core::Error("io", "cannot read " + file.toStdString());
        const auto size = static_cast<std::uint64_t>(in.tellg());
        if (size > render::smart::kFileMaxBytes) {
            flash(QStringLiteral("ファイルが大きすぎます（256 MiB まで）"), 6000, true);
            return false;
        }
        in.seekg(0);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        info = render::smart::look_at(bytes, {});  // (the whole picture read first: a file that cannot be drawn is never placed)
        const render::Image picture = render::open_image(bytes, render::kPillowOpenLimits);
        // (カラー設定's policy for its own profile: the answer kept, its pixels as they are)
        use = picture_use(bytes, picture.mode());
        if (!still(asked)) return false;
    } catch (const std::exception& error) {  // (a file that cannot be read as a picture)
        flash(QStringLiteral("読み込めない画像です:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    // (a page of 16 or 32 bits takes a picture of more than 8 bits whole only up to its precise pictures' size: asked)
    const bool deep_page = !page->color && page->rgb && page->rgb->depth > 8;
    if (deep_page && info.depth > 8 && static_cast<std::uint64_t>(info.width) * info.height > core::kColorRasterMaxPixels) {
        if (!ask::question(this, QStringLiteral("画像を読み込む"), QStringLiteral("16 bit のままでは大きすぎます。8 bit で置きますか"))) return false;
        if (!still(asked)) return false;
        try {
            const render::Image picture = render::open_image(bytes, render::kPillowOpenLimits);
            const std::string_view mode = picture.mode();
            bytes = render::write_png(picture.convert(mode == "RGBA" || mode == "LA" || mode == "P" ? "RGBA" : "RGB"));
            info = render::smart::look_at(bytes, ".png");
        } catch (const std::exception& error) {
            flash(QStringLiteral("読み込めない画像です:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
            return false;
        }
    }
    session_->save_now();  // (Python's commit_now)
    std::string ref;
    try {
        ref = storage::AssetStore(*session_->path()).put_bytes(bytes, info.suffix);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    }
    page = current_page();  // (after the book was saved: never a pointer from before)
    if (page == nullptr) return false;
    const QFileInfo named(file);
    Json place{{"op", "smart_place"}, {"page", page->index.json()}, {"asset", ref}, {"name", named.fileName().toStdString()}, {"to", "art"},
               {"title", named.completeBaseName().toStdString()}};
    place.update(use);
    const core::Frame* frame = as_placed ? selected_frame() : nullptr;
    if (as_placed) {
        if (frame != nullptr) {
            place["frame_id"] = frame->id;
        } else {
            const core::Rect b = page->bleed_rect_mm();  // the whole page, out to the bleed
            place["placement_mm"] = Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()});
        }
    }
    const Json ops = Json::array({Json{{"op", "register_assets"},
                                       {"assets", Json{{ref, Json{{"kind", "image"}, {"origin", Json{{"kind", "self"}, {"file", named.fileName().toStdString()}}}}}}}},
                                  place});
    if (!apply_ops(ops)) return false;
    if (as_placed) {
        flash(QStringLiteral("%1に「%2」を置きました").arg(frame != nullptr ? QStringLiteral("選んだコマ") : QStringLiteral("ページ全体"), named.fileName()), 4000);
    } else {
        flash(QStringLiteral("「%1」をスマートオブジェクトとして置きました").arg(named.fileName()), 4000);
    }
    return true;
}

namespace {

// The layers a command works on: those chosen in the layer panel, else the layer drawn on.
std::vector<std::string> chosen_layers(LayerPanel* panel, const core::Layer* target) {
    std::vector<std::string> ids = panel != nullptr ? panel->selected_ids() : std::vector<std::string>{};
    if (ids.empty() && target != nullptr) ids.push_back(target->id);
    return ids;
}

}  // namespace

void MainWindow::smart_convert() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const std::vector<std::string> ids = chosen_layers(layer_panel_, target_layer());
    if (ids.empty()) {
        flash(QStringLiteral("スマートオブジェクトにするレイヤーを選んでください"), 4000);
        return;
    }
    const std::string id = core::new_id();
    Json ids_json = Json::array();
    for (const std::string& each : ids) ids_json.push_back(each);
    if (!apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", page->index.json()}, {"ids", ids_json}, {"new_id", id}}}))) return;
    set_target_layer(id);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
    flash(ids.size() == 1 ? QStringLiteral("スマートオブジェクトに変換しました")
                          : QStringLiteral("%1 枚のレイヤーをスマートオブジェクトにまとめました（中の描画モードは中だけで効きます）").arg(ids.size()),
          5000);
}

void MainWindow::smart_copy() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    if (layer->kind != core::LayerKind::Smart) {
        flash(QStringLiteral("スマートオブジェクトを選んでください"), 4000);
        return;
    }
    const std::string id = core::new_id();
    if (!apply_ops(Json::array({Json{{"op", "smart_copy"}, {"page", page->index.json()}, {"id", layer->id}, {"new_id", id}}}))) return;
    set_target_layer(id);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
}

void MainWindow::rasterize(const std::string& what) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (what == "all") {
        apply_ops(Json::array({Json{{"op", "rasterize_layer"}, {"page", page->index.json()}, {"all", true}}}));
        return;
    }
    const core::Layer* layer = target_layer();
    if (layer == nullptr) return;
    const bool fill = layer->kind == core::LayerKind::Fill && ((layer->fill && core::py_truthy(*layer->fill)) || (layer->fill_rgb && !layer->fill_rgb->empty()));
    if ((what == "smart" && layer->kind != core::LayerKind::Smart) || (what == "fill" && !fill)) {
        flash(what == "smart" ? QStringLiteral("スマートオブジェクトを選んでください") : QStringLiteral("塗りつぶしのレイヤーを選んでください"), 4000);
        return;
    }
    apply_ops(Json::array({Json{{"op", "rasterize_layer"}, {"page", page->index.json()}, {"id", layer->id}}}));
}

void MainWindow::content_options() {
    const core::Layer* layer = target_layer();
    if (layer == nullptr || layer_panel_ == nullptr) return;
    if (layer->kind == core::LayerKind::Smart) {  // (コンテンツを編集: a later step of PC3)
        flash(QStringLiteral("スマートオブジェクトのコンテンツを編集は、%1").arg(QString::fromUtf8(kLater)), 4000);
        return;
    }
    layer_panel_->edit_special();
}

QWidget* smart_properties(MainWindow* window, const core::Page& page, const core::Layer& layer) {
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("properties_smart"));
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    const core::Document& book = window->book();
    const core::SmartContents* contents = core::smart::contents_of(book, layer);
    const core::smart::Matrix m = core::smart::matrix_of(layer);
    auto* about = new QLabel;
    about->setObjectName(QStringLiteral("properties_smart_contents"));
    about->setWordWrap(true);
    if (contents == nullptr) {
        about->setText(QStringLiteral("スマートオブジェクトの中身が見つかりません"));
    } else if (contents->kind == "file") {
        const auto [w_mm, h_mm] = core::smart::size_mm(*contents);
        QString colours = QString::fromStdString(contents->mode);
        colours.replace(QStringLiteral("rgba"), QStringLiteral("RGB（透明あり）")).replace(QStringLiteral("graya"), QStringLiteral("グレー（透明あり）"));
        colours.replace(QStringLiteral("rgb"), QStringLiteral("RGB")).replace(QStringLiteral("gray"), QStringLiteral("グレー"));
        colours.replace(QStringLiteral("cmyk"), QStringLiteral("CMYK")).replace(QStringLiteral("lab"), QStringLiteral("Lab"));
        const double across = std::hypot(m.a, m.b);
        about->setText(QStringLiteral("中身: ファイル「%1」\n%2 %3 bit %4・%5×%6 px（%7×%8 mm）\n実効解像度 %9 dpi")
                           .arg(QString::fromStdString(contents->name), QString::fromStdString(contents->format).toUpper())
                           .arg(contents->depth)
                           .arg(colours)
                           .arg(contents->width_px)
                           .arg(contents->height_px)
                           .arg(w_mm, 0, 'f', 1)
                           .arg(h_mm, 0, 'f', 1)
                           .arg(across > 0 ? contents->dpi / across : 0.0, 0, 'f', 0));
    } else {
        about->setText(QStringLiteral("中身: ページ（レイヤー %1 枚）").arg(contents->page ? static_cast<int>(contents->page->layers.size()) : 0));
    }
    column->addWidget(about);
    auto* edit = new QPushButton(QStringLiteral("コンテンツを編集"));
    edit->setObjectName(QStringLiteral("properties_smart_edit"));
    edit->setEnabled(false);
    edit->setToolTip(QString::fromUtf8(kLater));
    column->addWidget(edit);
    // its size, turn and place (the contents' top left on the page)
    auto* rows = new QWidget;
    auto* form = new QFormLayout(rows);
    form->setContentsMargins(0, 0, 0, 0);
    const auto spin = [&](const char* name, double lo, double hi, double value, const QString& suffix, int decimals) {
        auto* b = new QDoubleSpinBox;
        b->setObjectName(QString::fromLatin1(name));
        b->setRange(lo, hi);
        b->setDecimals(decimals);
        b->setSuffix(suffix);
        b->setValue(value);
        return b;
    };
    const double sx = std::hypot(m.a, m.b), sy = m.det() / (sx > 0 ? sx : 1.0);
    // (a picture of few pixels laid over a page is many thousands of percent)
    auto* width = spin("properties_smart_w", -1e7, 1e7, sx * 100, QStringLiteral(" %"), 2);
    auto* height = spin("properties_smart_h", -1e7, 1e7, sy * 100, QStringLiteral(" %"), 2);
    auto* angle = spin("properties_smart_angle", -360, 360, std::atan2(m.b, m.a) * 180 / 3.14159265358979323846, QStringLiteral(" °"), 2);
    auto* x = spin("properties_smart_x", -10000, 10000, m.e, QStringLiteral(" mm"), 2);
    auto* y = spin("properties_smart_y", -10000, 10000, m.f, QStringLiteral(" mm"), 2);
    form->addRow(QStringLiteral("W"), width);
    form->addRow(QStringLiteral("H"), height);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QStringLiteral("X"), x);
    form->addRow(QStringLiteral("Y"), y);
    auto* stack = new QComboBox;
    stack->setObjectName(QStringLiteral("properties_smart_stack"));
    stack->addItem(QStringLiteral("なし"));
    stack->setEnabled(false);
    stack->setToolTip(QString::fromUtf8(kLater));
    form->addRow(QStringLiteral("スタックモード"), stack);
    column->addWidget(rows);
    auto* apply = new QPushButton(QStringLiteral("変形を適用"));
    apply->setObjectName(QStringLiteral("properties_smart_apply"));
    const Json index = page.index.json();
    const std::string id = layer.id;
    // (a box left as it was shown keeps its exact number: the shown one is rounded)
    const std::array<double, 5> exact{sx * 100, sy * 100, std::atan2(m.b, m.a) * 180 / 3.14159265358979323846, m.e, m.f};
    const std::array<double, 5> shown{width->value(), height->value(), angle->value(), x->value(), y->value()};
    QObject::connect(apply, &QPushButton::clicked, box, [window, index, id, width, height, angle, x, y, exact, shown] {
        const auto value = [&](std::size_t i, const QDoubleSpinBox* b) { return b->value() == shown[i] ? exact[i] : b->value(); };
        const double w = value(0, width) / 100, h = value(1, height) / 100, turn = value(2, angle) * 3.14159265358979323846 / 180;
        const double c = std::cos(turn), s = std::sin(turn);
        const Json set = Json::array({w * c, w * s, -h * s, h * c, value(3, x), value(4, y)});
        window->apply_ops(Json::array({Json{{"op", "smart_transform"}, {"page", index}, {"id", id}, {"set_matrix", set}}}));
    });
    column->addWidget(apply);
    // a file's colours: as its numbers, from its own profile, or taken as the working space's
    if (contents != nullptr && contents->kind == "file") {
        auto* use = new QComboBox;
        use->setObjectName(QStringLiteral("properties_smart_profile"));
        use->addItem(QStringLiteral("値のまま"), QStringLiteral("keep"));
        use->addItem(QStringLiteral("埋め込みから変換"), QStringLiteral("convert"));
        use->addItem(QStringLiteral("作業用とみなす"), QStringLiteral("assign"));
        const std::string now = core::py_str(core::py_get(contents->profile, "use", Json("keep")));
        use->setCurrentIndex(now == "embedded" ? 1 : now == "assign" ? 2 : 0);
        const std::string mode = contents->mode;
        QObject::connect(use, &QComboBox::currentIndexChanged, box, [window, index, id, use, mode] {
            const std::string way = use->currentData().toString().toStdString();
            Json op{{"op", "smart_profile"}, {"page", index}, {"id", id}, {"embedded_profile", way}};
            const color_settings::Settings settings = color_settings::load();
            op["intent"] = settings.intent.toStdString();
            op["bpc"] = settings.bpc;
            if (way == "assign") {
                const color_settings::Space space = mode.starts_with("cmyk") ? color_settings::Space::Cmyk
                                                    : mode.starts_with("gray") ? color_settings::Space::Gray
                                                                               : color_settings::Space::Rgb;
                const color_settings::Working working = color_settings::working(space, settings);
                if (!working.usable || !working.icc) {
                    window->flash(QStringLiteral("作業用の空間のプロファイルが使えません（カラー設定）"), 5000, true);
                    return;
                }
                op["icc"] = core::b64encode(*working.icc);
            }
            window->apply_ops(Json::array({op}));
        });
        auto* colours = new QFormLayout;
        colours->addRow(QStringLiteral("色の扱い"), use);
        column->addLayout(colours);
    }
    return box;
}

}  // namespace genko::app
