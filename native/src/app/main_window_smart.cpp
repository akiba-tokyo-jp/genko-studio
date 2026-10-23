// smart-objects@1 (PC3 A0, design section A 5; Photoshop's names, the user's decision 8 of PC2): the window's smart
// objects — ファイル → 画像を読み込む… (main_window_materials.cpp: the file placed as a smart object, decision 2) and
// 埋め込みを配置…; レイヤー → スマートオブジェクト, スマートフィルター, ラスタライズ, レイヤーの内容オプション…. Every change
// is an op (render/ops_smart.cpp); the contents and links (PC3 A1), the smart filters (A2), the stack modes and a line
// rasterized (A4) too. ワープ・遠近法ワープ・パペットワープ (A5) open 節 C's transform box on the smart object
// (main_window_transform.cpp).

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTabBar>
#include <QUrl>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "app/ask.hpp"
#include "app/color_settings_dialog.hpp"
#include "app/layer_panel.hpp"
#include "app/off_ui.hpp"
#include "app/main_window.hpp"
#include "app/smart_panel.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "formats/smart_files.hpp"
#include "render/pcraft_read.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/smart.hpp"
#include "render/smart_decode.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

// Photoshop's stack modes (レイヤー → スマートオブジェクト → スタックモード), the op's names.
const std::vector<std::pair<const char*, const char*>>& stack_modes() {
    static const std::vector<std::pair<const char*, const char*>> modes = {
        {"entropy", "エントロピー"}, {"kurtosis", "尖度"},     {"maximum", "最大"},   {"mean", "平均"},
        {"median", "中央値"},        {"minimum", "最小"},     {"range", "範囲"},     {"skewness", "歪度"},
        {"standard_deviation", "標準偏差"}, {"summation", "合計"}, {"variance", "分散"}, {"none", "なし"}};
    return modes;
}

// --- PC3 A1: linked files ------------------------------------------------------------------------------------------------

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& file) { return core::path_from_utf8(file.toStdString()); }

QString canonical(const QString& file) {
    const QFileInfo info(file);
    const QString real = info.canonicalFilePath();
    return real.isEmpty() ? info.absoluteFilePath() : real;
}

// The linked files the person chose here or said yes to (in this run of Genko): a place the book recorded — an agent's,
// perhaps — is shown and asked about before Genko reads it, opens it or takes it in (the design's safety rule).
std::set<QString>& trusted_links() {
    static std::set<QString> paths;
    return paths;
}
void trust_link(const QString& file) { trusted_links().insert(canonical(file)); }
bool trusted_link(const QString& file) { return trusted_links().contains(canonical(file)); }

// A linked file's record for an op: its absolute place, its place from the book's folder ("/" between names; none when
// it is on another drive), its sha256 and size.
Json link_record(const QString& file, std::string_view bytes, const std::optional<fs::path>& book) {
    Json link{{"path", QFileInfo(file).absoluteFilePath().toStdString()}};
    if (book) {
        const QString relative = QDir(qpath(*book)).relativeFilePath(QFileInfo(file).absoluteFilePath());
        if (!relative.isEmpty() && !QDir::isAbsolutePath(relative)) link["relative"] = QDir::fromNativeSeparators(relative).toStdString();
    }
    link["sha256"] = storage::sha256_hex(bytes);
    link["bytes"] = static_cast<std::int64_t>(bytes.size());
    return link;
}

// The sha256 of a linked file, remembered by its place, size and time (looked at again only when it changed).
std::string linked_sha(const QString& file) {
    struct Seen {
        qint64 size = -1;
        QDateTime time;
        std::string sha;
    };
    static std::map<QString, Seen> seen;
    const QFileInfo info(file);
    Seen& was = seen[file];
    if (was.size == info.size() && was.time == info.lastModified() && !was.sha.empty()) return was.sha;
    try {
        was.sha = storage::sha256_file(fpath(file));
    } catch (const std::exception&) {
        was.sha.clear();
    }
    was.size = info.size();
    was.time = info.lastModified();
    return was.sha;
}

// The file's bytes when it is a picture of at most 256 MiB (core::Error otherwise, said by the caller).
std::string read_picture(const QString& file, render::smart::FileInfo* info) {
    std::ifstream in(fpath(file), std::ios::binary | std::ios::ate);
    if (!in) throw core::Error("io", "cannot read " + file.toStdString());
    if (static_cast<std::uint64_t>(in.tellg()) > render::smart::kFileMaxBytes) throw core::Error("too_large", "the file is larger than 256 MiB");
    in.seekg(0);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    *info = render::smart::look_at(bytes, {});
    return bytes;
}

constexpr const char* kPictures = "画像 (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.gif *.psd *.psb *.tga *.ico *.pnm *.pbm *.pgm *.ppm *.pam *.pfm *.qoi *.exr *.hdr *.avif)";


// レイヤー → スマートフィルター's commands on the smart object drawn on (PC3 A2, render/ops_smart_filters.cpp): "off"
// (スマートフィルターを無効にする: off, or on again), "mask_off" (フィルターマスクを無効にする: the same), "mask_delete",
// "clear" (every filter and the mask).
void smart_filters_command(MainWindow* window, const std::string& what) {
    const core::Page* page = window->current_page();
    const core::Layer* layer = window->target_layer();
    if (page == nullptr || layer == nullptr) return;
    if (layer->kind != core::LayerKind::Smart || !layer->smart || !layer->smart->is_object()) {
        window->flash(QStringLiteral("スマートオブジェクトを選んでください"), 4000);
        return;
    }
    const Json& s = *layer->smart;
    const bool filters = s.contains("filters") && s.at("filters").is_array() && !s.at("filters").empty();
    const Json* mask = s.contains("filter_mask") && s.at("filter_mask").is_object() ? &s.at("filter_mask") : nullptr;
    if (what == "off" ? !filters : what == "clear" ? !filters && mask == nullptr : mask == nullptr) {
        window->flash(what == "off" || what == "clear" ? QStringLiteral("このスマートオブジェクトには、スマートフィルターがありません")
                                                       : QStringLiteral("このスマートオブジェクトには、フィルターマスクがありません"),
                      4000);
        return;
    }
    Json op{{"op", "smart_filter_state"}, {"page", page->index.json()}, {"id", layer->id}};
    QString done;
    if (what == "off") {
        const bool on = !(s.contains("filters_on") && s.at("filters_on") == Json(false));
        op["filters_on"] = !on;
        done = on ? QStringLiteral("スマートフィルターを無効にしました（もう一度で有効に戻ります）") : QStringLiteral("スマートフィルターを有効にしました");
    } else if (what == "mask_off") {
        const bool on = !(mask->contains("enabled") && mask->at("enabled") == Json(false));
        op["mask_on"] = !on;
        done = on ? QStringLiteral("フィルターマスクを無効にしました（もう一度で有効に戻ります）") : QStringLiteral("フィルターマスクを有効にしました");
    } else if (what == "mask_delete") {
        op["mask"] = "delete";
        done = QStringLiteral("フィルターマスクを削除しました");
    } else {
        op = Json{{"op", "smart_filter_delete"}, {"page", page->index.json()}, {"id", layer->id}, {"all", true}};
        done = QStringLiteral("スマートフィルターをクリアしました");
    }
    if (window->apply_ops(Json::array({op}))) window->flash(done, 4000);
}

}  // namespace

void MainWindow::build_smart_actions() {
    // ファイル
    make("act_place_embedded", QStringLiteral("埋め込みを配置…"), [this] { place_embedded(); }, {},
         QStringLiteral("画像をスマートオブジェクトとしてページの中央に置きます（元のファイルのまま。大きければページに収めます）"));
    make("act_place_linked", QStringLiteral("リンクを配置…"), [this] { place_linked(); }, {},
         QStringLiteral("画像をファイルにリンクしたスマートオブジェクトとして置きます（ファイルを直すと「変更されたコンテンツを更新」で取り込めます）"));
    make("act_package", QStringLiteral("パッケージ…"), [this] { package_book(); }, {},
         QStringLiteral("原稿を、リンクしたファイルの写し（Links フォルダー）と一緒に別のフォルダーへまとめます"));
    // レイヤー → スマートオブジェクト
    make("act_smart_convert", QStringLiteral("スマートオブジェクトに変換"), [this] { smart_convert(); }, {},
         QStringLiteral("選んだレイヤーをスマートオブジェクトにまとめます（中身はそのまま。拡大・縮小しても劣化しません）"));
    make("act_smart_copy", QStringLiteral("新規スマートオブジェクト（コピー）"), [this] { smart_copy(); }, {},
         QStringLiteral("選んだスマートオブジェクトを、中身ごと別のものとして複製します"));
    // (PC3 A1: contents and links)
    make("act_smart_edit", QStringLiteral("コンテンツを編集"), [this] { edit_contents(); }, {},
         QStringLiteral("スマートオブジェクトの中身を別のタブで開きます（変更はすぐ原稿に出ます。リンクしたファイルは既定のアプリで開きます）"));
    make("act_smart_export", QStringLiteral("コンテンツを書き出し…"), [this] { export_contents(); }, {},
         QStringLiteral("スマートオブジェクトの中身をファイルに書き出します（画像は元のファイルのまま、ページは PSD か PNG）"));
    make("act_smart_replace", QStringLiteral("コンテンツを置換…"), [this] { replace_contents(); }, {},
         QStringLiteral("スマートオブジェクトの中身を別の画像に入れ替えます（変形はそのまま。同じ中身のものはすべて変わります）"));
    make("act_smart_relink", QStringLiteral("ファイルに再リンク…"), [this] { relink_contents(); }, {},
         QStringLiteral("スマートオブジェクトのリンク先を別のファイルにします"));
    make("act_smart_embed", QStringLiteral("埋め込みに変換"), [this] { embed_contents(); }, {},
         QStringLiteral("リンクしたスマートオブジェクトを、原稿の中の写しだけを持つものにします"));
    make("act_smart_link", QStringLiteral("リンクに変換…"), [this] { link_contents(); }, {},
         QStringLiteral("スマートオブジェクトの中身をファイルに書き出し、そのファイルにリンクします"));
    make("act_smart_update", QStringLiteral("変更されたコンテンツを更新"), [this] { update_contents(false); }, {},
         QStringLiteral("リンクしたファイルが変わっていれば、選んだスマートオブジェクトの中身を新しいファイルにします"));
    make("act_smart_update_all", QStringLiteral("変更されたすべてのコンテンツを更新"), [this] { update_contents(true); }, {},
         QStringLiteral("この原稿のリンクしたファイルのうち、変わったものをすべて取り込みます"));
#ifdef Q_OS_WIN
    make("act_smart_reveal", QStringLiteral("エクスプローラーで表示"), [this] { reveal_contents(); }, {},
         QStringLiteral("リンクしたファイルのあるフォルダーを開きます"));
#else
    make("act_smart_reveal", QStringLiteral("ファイルマネージャーで表示"), [this] { reveal_contents(); }, {},
         QStringLiteral("リンクしたファイルのあるフォルダーを開きます"));
#endif
    // (PC3 A4: the contents' layers combined pixel by pixel; なし: composited as they are)
    for (const auto& [mode, label] : stack_modes()) {
        const std::string name = mode;
        make(QString::fromLatin1("act_smart_stack_%1").arg(QString::fromLatin1(mode)), QString::fromUtf8(label), [this, name] { smart_stack(name); }, {},
             name == "none" ? QStringLiteral("スマートオブジェクトの中のレイヤーを、ふつうに重ねて見せます（スタックモードを外します）")
                            : QStringLiteral("スマートオブジェクトの中のレイヤーを、画素ごとの%1で 1 枚にします（中身はそのまま）").arg(QString::fromUtf8(label)));
    }
    // (PC3 A5: its contents as they are — its warp kept with it, the other two as its smart filters)
    make("act_smart_warp", QStringLiteral("ワープ"), [this] { smart_bend(QStringLiteral("warp")); }, {},
         QStringLiteral("選んだスマートオブジェクトをワープで曲げます（中身はそのまま。もう一度で今のワープから曲げ直せます）"));
    make("act_smart_perspective_warp", QStringLiteral("遠近法ワープ"), [this] { smart_bend(QStringLiteral("perspective_warp")); }, {},
         QStringLiteral("選んだスマートオブジェクトに遠近法ワープをスマートフィルターとしてかけます（あとから直せます）"));
    make("act_smart_puppet_warp", QStringLiteral("パペットワープ"), [this] { smart_bend(QStringLiteral("puppet")); }, {},
         QStringLiteral("選んだスマートオブジェクトにパペットワープをスマートフィルターとしてかけます（あとから直せます）"));
    make("act_smart_rasterize", QStringLiteral("ラスタライズ"), [this] { rasterize("smart"); }, {},
         QStringLiteral("選んだスマートオブジェクトを、見えているとおりの画素のレイヤーにします"));
    // レイヤー → スマートフィルター
    make("act_smart_filters_convert", QStringLiteral("スマートフィルター用に変換"), [this] { smart_convert(); }, {},
         QStringLiteral("選んだレイヤーをスマートオブジェクトにします（フィルターをあとから直せる形）"));
    // (PC3 A2: on the smart object drawn on; each again to undo it — 無効にする turns them back on)
    make("act_smart_filters_off", QStringLiteral("スマートフィルターを無効にする"), [this] { smart_filters_command(this, "off"); }, {},
         QStringLiteral("選んだスマートオブジェクトのスマートフィルターを、すべて一時的に外して見ます（もう一度で戻ります）"));
    make("act_smart_filter_mask_delete", QStringLiteral("フィルターマスクを削除"), [this] { smart_filters_command(this, "mask_delete"); }, {},
         QStringLiteral("スマートフィルターのフィルターマスクを消します（フィルターは全体にかかります）"));
    make("act_smart_filter_mask_off", QStringLiteral("フィルターマスクを無効にする"), [this] { smart_filters_command(this, "mask_off"); }, {},
         QStringLiteral("フィルターマスクを一時的に使わずに見ます（もう一度で戻ります）"));
    make("act_smart_filters_clear", QStringLiteral("スマートフィルターをクリア"), [this] { smart_filters_command(this, "clear"); }, {},
         QStringLiteral("選んだスマートオブジェクトのスマートフィルターとフィルターマスクを、すべて消します"));
    // レイヤー → ラスタライズ
    make("act_rasterize_type", QStringLiteral("テキスト"), [this] { rasterize_type(); }, {},
         QStringLiteral("選んだ台詞（フキダシと文字）を、見えているとおりの画素のレイヤーにします（台詞としては直せなくなります）"));
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
    // (ファイル: 埋め込みを配置…・リンクを配置…・パッケージ… are put in Photoshop's place by build_file_menu, PC4 F-0)
    (void)file;
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
        flash(QStringLiteral("画像を置く前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const QString file =
        chosen ? *chosen : ask::open_path(this, QStringLiteral("埋め込みを配置"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.gif *.psd *.psb *.tga *.ico *.pnm *.pbm *.pgm *.ppm *.pam *.pfm *.qoi *.exr *.hdr *.avif);;PhotoCraft (*.pcraft)"));
    if (file.isEmpty() || !still(asked)) return false;
    return place_smart(file, false);
}

bool MainWindow::place_smart(const QString& file, bool as_placed, bool linked) {
    if (!as_placed && !linked && file.endsWith(QStringLiteral(".pcraft"), Qt::CaseInsensitive)) return place_pcraft(file);  // (PC4 F-f)
    const Asked asked = asking();
    std::string bytes;
    render::smart::FileInfo info;
    Json use;
    std::optional<Json> link;  // (PC3 A1: the file as it is — its sha256 and size —, whatever copy the book keeps)
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
        if (linked) link = link_record(file, bytes, session_->path());
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
    if (link) place["link"] = *link;
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
    } else if (link) {
        trust_link(file);  // (the person chose it)
        flash(QStringLiteral("「%1」をリンクしたスマートオブジェクトとして置きました").arg(named.fileName()), 4000);
    } else {
        flash(QStringLiteral("「%1」をスマートオブジェクトとして置きました").arg(named.fileName()), 4000);
    }
    return true;
}

// PC4 F-f (design 2.8): 埋め込みを配置… of a .pcraft — its page the contents of a smart object (the op smart_place of the
// book's asset of it). The file is read on a worker thread first (render/pcraft_read: the heavy part, kept for the op),
// so the window keeps drawing; what it could not keep is said.
bool MainWindow::place_pcraft(const QString& file) {
    const Asked asked = asking();
    std::string bytes;
    std::string ref;
    std::vector<std::string> notes;
    std::optional<render::pcraft::Read> read;
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
        const std::string name = QFileInfo(file).completeBaseName().toStdString();
        read = off_ui([&bytes, &name] { return render::pcraft::read(bytes, name); });
        notes = read->notes;
        if (!still(asked)) return false;
    } catch (const std::exception& error) {
        flash(QStringLiteral("読み込めない .pcraft です:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    session_->save_now();
    try {
        ref = storage::AssetStore(*session_->path()).put_bytes(bytes, ".pcraft");
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return false;
    }
    page = current_page();
    if (page == nullptr) return false;
    const QFileInfo named(file);
    const Json place{{"op", "smart_place"}, {"page", page->index.json()}, {"asset", ref}, {"name", named.fileName().toStdString()}, {"to", "art"},
                     {"title", named.completeBaseName().toStdString()}};
    // (what was read on the worker the op's, once — never kept past it for another placing)
    render::pcraft::remember_read(ref, std::move(*read));
    const bool placed = apply_ops(Json::array({place}));
    render::pcraft::forget_read();
    if (!placed) return false;
    QString said = QStringLiteral("「%1」をスマートオブジェクトとして置きました").arg(named.fileName());
    for (const std::string& note : notes) said += QStringLiteral("。") + QString::fromStdString(note);
    flash(said, notes.empty() ? 4000 : 8000);
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
    if (layer->kind == core::LayerKind::Smart) {  // (コンテンツを編集, PC3 A1)
        edit_contents();
        return;
    }
    layer_panel_->edit_special();
}

void MainWindow::smart_stack(const std::string& mode) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    if (layer->kind != core::LayerKind::Smart) {
        flash(QStringLiteral("スマートオブジェクトを選んでください"), 4000);
        return;
    }
    if (!apply_ops(Json::array({Json{{"op", "smart_stack"}, {"page", page->index.json()}, {"id", layer->id}, {"mode", mode}}}))) return;
    for (const auto& [name, label] : stack_modes()) {
        if (name != mode) continue;
        flash(mode == "none" ? QStringLiteral("スタックモードを外しました") : QStringLiteral("スタックモードを「%1」にしました").arg(QString::fromUtf8(label)), 4000);
    }
}

void MainWindow::rasterize_type() {
    const core::StoryLine* line = selected_line_or_say();
    if (line == nullptr) return;
    const std::string id = core::new_id();
    const std::string chosen = line->id;
    if (!apply_ops(Json::array({Json{{"op", "rasterize_lines"}, {"page", line->page_index.json()}, {"ids", Json::array({chosen})}, {"new_id", id}}})))
        return;
    canvas_->selected_line_id.reset();
    set_target_layer(id);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
    flash(QStringLiteral("台詞を画素のレイヤーにしました（元に戻すで台詞に戻ります）"), 5000);
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
    if (contents != nullptr && contents->link) {  // (PC3 A1: where it is linked, and how the file is now)
        const auto id = core::smart::contents_id(layer);
        const std::string status = id ? window->link_status(*id) : std::string();
        const QString state = status == "missing" ? QStringLiteral("（リンク切れ: 原稿の中の写しで描いています）")
                              : status == "modified" ? QStringLiteral("（ファイルが変更されています）")
                                                     : QString();
        about->setText(about->text() + QStringLiteral("\nリンク: %1%2")
                                           .arg(QString::fromStdString(core::py_str(core::py_get(*contents->link, "path", Json("")))), state));
    }
    column->addWidget(about);
    auto* edit = new QPushButton(QStringLiteral("コンテンツを編集"));
    edit->setObjectName(QStringLiteral("properties_smart_edit"));
    {
        const std::string chosen = layer.id;
        QObject::connect(edit, &QPushButton::clicked, box, [window, chosen] {
            window->set_target_layer(chosen);
            window->edit_contents();
        });
    }
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
    // (its matrix as a turn of a width, a height and a shear: the shear and its corners kept through 変形を適用)
    const double sx = std::hypot(m.a, m.b), sy = m.det() / (sx > 0 ? sx : 1.0);
    const double turned = std::atan2(m.b, m.a), shear = m.c * std::cos(turned) + m.d * std::sin(turned);
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
    // (PC3 A4: its stack mode, なし first)
    auto* stack = new QComboBox;
    stack->setObjectName(QStringLiteral("properties_smart_stack"));
    stack->addItem(QStringLiteral("なし"), QStringLiteral("none"));
    for (const auto& [mode, label] : stack_modes()) {
        if (std::string_view(mode) != "none") stack->addItem(QString::fromUtf8(label), QString::fromLatin1(mode));
    }
    {
        const Json* now = layer.smart ? core::get(*layer.smart, "stack") : nullptr;
        const int at = now != nullptr && now->is_string() ? stack->findData(QString::fromStdString(now->get<std::string>())) : 0;
        stack->setCurrentIndex(std::max(0, at));
    }
    stack->setToolTip(QStringLiteral("中のレイヤーを画素ごとの統計で 1 枚にします（中央値で写り込んだ人を消す・平均でノイズを減らすなど）"));
    QObject::connect(stack, &QComboBox::currentIndexChanged, box, [window, stack, at = page.index.json(), chosen = layer.id] {
        window->apply_ops(Json::array({Json{{"op", "smart_stack"}, {"page", at}, {"id", chosen}, {"mode", stack->currentData().toString().toStdString()}}}));
    });
    form->addRow(QStringLiteral("スタックモード"), stack);
    column->addWidget(rows);
    auto* apply = new QPushButton(QStringLiteral("変形を適用"));
    apply->setObjectName(QStringLiteral("properties_smart_apply"));
    const Json index = page.index.json();
    const std::string id = layer.id;
    // (a box left as it was shown keeps its exact number: the shown one is rounded)
    const std::array<double, 5> exact{sx * 100, sy * 100, turned * 180 / 3.14159265358979323846, m.e, m.f};
    const std::array<double, 5> shown{width->value(), height->value(), angle->value(), x->value(), y->value()};
    // (the box it takes on the page, for transform_layers to report: its corners')
    Json at_box = Json();
    if (contents != nullptr) {
        const core::smart::Corners c = core::smart::corners_on_page(layer, *contents);
        double x0 = c[0].first, y0 = c[0].second, x1 = x0, y1 = y0;
        for (const auto& [cx, cy] : c) {
            x0 = std::min(x0, cx);
            y0 = std::min(y0, cy);
            x1 = std::max(x1, cx);
            y1 = std::max(y1, cy);
        }
        if (x1 > x0 && y1 > y0) at_box = Json::array({x0, y0, x1 - x0, y1 - y0});
    }
    // (moved as the Move tool moves it — transform_layers by the change from its matrix now: its corners, warp and masks
    // with it, the layers linked to it along, nothing where its position is locked; independent review A P2-2)
    QObject::connect(apply, &QPushButton::clicked, box, [window, index, id, width, height, angle, x, y, exact, shown, m, sy, shear, at_box] {
        const auto value = [&](std::size_t i, const QDoubleSpinBox* b) { return b->value() == shown[i] ? exact[i] : b->value(); };
        const double w = value(0, width) / 100, h = value(1, height) / 100, turn = value(2, angle) * 3.14159265358979323846 / 180;
        const double c = std::cos(turn), s = std::sin(turn);
        const double k = std::abs(sy) > 1e-12 ? shear / sy * h : 0.0;  // (the shear's slant kept)
        const core::smart::Matrix wanted{w * c, w * s, k * c - h * s, k * s + h * c, value(3, x), value(4, y)};
        const auto back = core::smart::inverse(m);
        if (!back) return;
        const core::smart::Matrix by = core::smart::multiply(wanted, *back);
        if (core::smart::is_identity(core::smart::snapped(by))) return;  // (nothing changed)
        Json op{{"op", "transform_layers"}, {"page", index}, {"ids", Json::array({id})}, {"matrix", core::smart::to_json(by)}};
        if (!at_box.is_null()) op["box"] = at_box;
        window->apply_ops(Json::array({op}));
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

// --- PC3 A1: contents and links -------------------------------------------------------------------------------------------

namespace {

// The smart object chosen (the layer drawn on), its contents' id and its contents; nothing (said) when it is not one.
struct Chosen {
    const core::Page* page = nullptr;
    const core::Layer* layer = nullptr;
    std::string id;
    const core::SmartContents* contents = nullptr;
};

std::optional<Chosen> chosen_smart(MainWindow& window) {
    Chosen c;
    c.page = window.current_page();
    c.layer = window.target_layer();
    if (c.page == nullptr || c.layer == nullptr || c.layer->kind != core::LayerKind::Smart) {
        window.flash(QStringLiteral("スマートオブジェクトを選んでください"), 4000);
        return std::nullopt;
    }
    const auto id = core::smart::contents_id(*c.layer);
    c.contents = core::smart::contents_of(window.book(), *c.layer);
    if (!id || c.contents == nullptr) {
        window.flash(QStringLiteral("スマートオブジェクトの中身が見つかりません"), 5000, true);
        return std::nullopt;
    }
    c.id = *id;
    return c;
}

QString layer_title(const core::Layer& layer) {
    return layer.title.empty() ? QStringLiteral("スマートオブジェクト") : QString::fromStdString(layer.title);
}

// The commands a contents' tab cannot use (their ops are the book's, its pages', panels' or lines', or the studio's):
// greyed while such a tab is in front. The bus refuses their ops there anyway (core/command_bus.cpp apply_in_contents).
constexpr const char* kNotInContents[] = {
    // pages
    "act_overview", "act_prev", "act_next", "act_add_page", "act_del_page", "act_dup_page", "act_page_up", "act_page_down", "act_spread",
    "act_nombre", "act_paper", "act_page_nombre", "act_add_cover", "act_assignee", "act_merge_book", "act_name_ok", "act_color_mode",
    "act_assign_profile", "act_convert_profile", "act_color_table", "act_mode_bitmap", "act_mode_indexed", "act_mode_duotone",
    "act_mode_multichannel",
    // (PC3 C-e: the page's paper)
    "act_image_size", "act_canvas_size", "act_rotate_180", "act_rotate_90cw", "act_rotate_90ccw", "act_rotate_angle", "act_flip_canvas_h",
    "act_flip_canvas_v", "act_crop", "act_trim", "act_reveal_all", "act_crop_tool",
    // panels
    "act_frame", "act_split_h", "act_split_v", "act_merge", "act_delete_frame", "act_frame_selection", "act_gutters", "act_border", "act_corner",
    "act_no_border", "act_bleed", "act_border_colour", "act_border_detail", "act_template", "act_save_template", "act_reset_shape",
    "act_ruler_frame",
    // lines
    "act_text", "act_story_editor", "act_replace", "act_line_type", "act_balloon_pen", "act_line_edit", "act_line_delete", "act_line_wrap",
    "act_rasterize_type",
    // the studio
    "act_ai", "act_style", "act_chat_approval",
    // the book's own files
    "act_package"};

}  // namespace

std::shared_ptr<const core::Document> MainWindow::shown_book() const {
    const std::shared_ptr<const core::Document> snapshot = session_->snapshot();
    if (!contents_) return snapshot;
    if (contents_view_ && contents_view_of_ == snapshot) return contents_view_;
    const auto found = snapshot->smart_contents.find(*contents_);
    if (found == snapshot->smart_contents.end() || !found->second || !found->second->page) return nullptr;
    auto view = std::make_shared<core::Document>(*snapshot);
    view->pages = {found->second->page};
    view->deferred.clear();
    view->story.clear();
    contents_view_ = std::move(view);
    contents_view_of_ = snapshot;
    return contents_view_;
}

Json MainWindow::with_contents_target(const Json& ops) const {
    if (!contents_ || !ops.is_array()) return ops;
    Json out = ops;
    for (Json& op : out) {
        if (op.is_object() && !op.contains("contents")) op["contents"] = *contents_;
    }
    return out;
}

void MainWindow::contents_tab_follow() {
    for (const char* name : kNotInContents) {
        QAction* act = action(QString::fromLatin1(name));
        if (act == nullptr) continue;
        if (contents_) {
            if (act->isEnabled()) {
                act->setEnabled(false);
                greyed_in_contents_.insert(act);
            }
        } else if (greyed_in_contents_.erase(act) > 0) {
            act->setEnabled(true);
        }
    }
}

bool MainWindow::open_contents_tab(const std::string& contents_id, const QString& title) {
    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        const Document& tab = documents_[static_cast<std::size_t>(i)];
        if (tab.session == session_ && tab.contents == contents_id) {  // (open already: in front)
            switch_document(i);
            return true;
        }
    }
    session_->save_now();
    store_document();
    Document tab{session_};
    tab.contents = contents_id;
    tab.contents_title = title;
    tab.page_index = page_index_;
    documents_.push_back(std::move(tab));
    doc_tabs_->blockSignals(true);
    doc_tabs_->addTab(documents_.back().title());
    doc_tabs_->blockSignals(false);
    show_document(static_cast<int>(documents_.size()) - 1);
    canvas_->fit_page();
    flash(QStringLiteral("「%1」の中身をこのタブで開きました（ここでの変更はすぐ原稿に出ます。保存は原稿と一緒です）").arg(title), 6000);
    return true;
}

bool MainWindow::edit_contents() {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    const QString title = layer_title(*chosen->layer);
    if (chosen->contents->kind == "page") return open_contents_tab(chosen->id, title);
    if (chosen->contents->link) {  // (a linked file: opened in its own app, watched; 変更されたコンテンツを更新 takes it)
        const auto found = formats::smart_files::linked_file(*chosen->contents->link, session_->path());
        if (!found) {
            flash(QStringLiteral("リンクしたファイルが見つかりません（ファイルに再リンク… で場所を直せます）"), 6000, true);
            return false;
        }
        const QString file = qpath(*found);
        if (!core::smart::suffix_of_name(file.toStdString())) {
            flash(QStringLiteral("リンク先が画像のファイルではないため、開きません"), 6000, true);
            return false;
        }
        if (!trusted_link(file) &&
            !ask::question(this, QStringLiteral("コンテンツを編集"), QStringLiteral("原稿に記録されたリンク先のファイルを、既定のアプリで開きます:\n%1\n\n開いてよいですか").arg(file)))
            return false;
        trust_link(file);
        if (!ask::open_url(QUrl::fromLocalFile(file))) {
            flash(QStringLiteral("ファイルを開けませんでした: %1").arg(file), 6000, true);
            return false;
        }
        if (!link_watcher_.files().contains(file)) link_watcher_.addPath(file);
        flash(QStringLiteral("「%1」を既定のアプリで開きました。保存したら「変更されたコンテンツを更新」で取り込めます").arg(QFileInfo(file).fileName()), 6000);
        return true;
    }
    // (a picture placed in the book: made a page of its own to edit first — asked, as it changes what every smart object
    // showing it is made of)
    const Asked asked = asking();
    if (!ask::question(this, QStringLiteral("コンテンツを編集"),
                       QStringLiteral("画像のスマートオブジェクトを、編集できるページ（ペイントのレイヤー 1 枚）にしてから開きます。\n"
                                      "同じ中身のスマートオブジェクトはすべて同じに変わり、白黒の仕上げは掛からなくなります。よろしいですか")))
        return false;
    if (!still(asked)) return false;
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    if (!apply_ops(Json::array({Json{{"op", "smart_to_page"}, {"page", page->index.json()}, {"id", chosen->layer->id}}}))) return false;
    return open_contents_tab(chosen->id, title);
}

bool MainWindow::export_contents(std::optional<QString> path) {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    const core::SmartContents& contents = *chosen->contents;
    QString file;
    if (path) {
        file = *path;
    } else {
        const QString suggested = contents.kind == "file" ? QString::fromStdString(contents.name) : layer_title(*chosen->layer) + QStringLiteral(".psd");
        const QString filter = contents.kind == "file" ? QStringLiteral("元のファイル (*%1)").arg(QString::fromStdString(contents.suffix))
                                                       : QStringLiteral("PSD (*.psd);;PNG (*.png)");
        file = ask::save_path(this, QStringLiteral("コンテンツを書き出し"), suggested, filter);
    }
    if (file.isEmpty()) return false;
    std::string format;
    if (contents.kind == "page") format = file.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ? "png" : "psd";
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);  // (a page's PSD at its dpi takes a moment)
    try {
        (void)formats::smart_files::export_contents(book(), chosen->id, fpath(file), format);
    } catch (const std::exception& error) {
        QGuiApplication::restoreOverrideCursor();
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    QGuiApplication::restoreOverrideCursor();
    flash(QStringLiteral("コンテンツを書き出しました: %1").arg(QDir::toNativeSeparators(file)), 5000);
    return true;
}

namespace {

// A picture the person chose, read and put in the book's assets (the book saved first): its ref, its bytes' record for a
// link and its colours' use (カラー設定's policy, as 画像を読み込む… asks); nothing when it cannot be (said).
struct Taken {
    std::string ref;
    Json link;
    Json use;
    QString name;
};

std::optional<Taken> take_picture(MainWindow& window, Session& session, const QString& file, bool ask_colours) {
    render::smart::FileInfo info;
    std::string bytes;
    try {
        bytes = read_picture(file, &info);
    } catch (const std::exception& error) {
        window.flash(QStringLiteral("読み込めない画像です:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return std::nullopt;
    }
    Taken out;
    out.name = QFileInfo(file).fileName();
    out.link = link_record(file, bytes, session.path());
    if (ask_colours) {
        try {
            const render::Image picture = render::open_image(bytes, render::kPillowOpenLimits);
            out.use = window.picture_use(bytes, picture.mode());
        } catch (const std::exception&) {
            out.use = Json::object();  // (read as 16 or 32 bits only: its numbers)
        }
    }
    session.save_now();
    try {
        out.ref = storage::AssetStore(*session.path()).put_bytes(bytes, info.suffix);
    } catch (const std::exception& error) {
        window.flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return std::nullopt;
    }
    return out;
}

}  // namespace

bool MainWindow::replace_contents(std::optional<QString> path) {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    if (!session_->path()) {
        flash(QStringLiteral("画像を置く前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const Asked asked = asking();
    const QString file = path ? *path : ask::open_path(this, QStringLiteral("コンテンツを置換"), QString::fromLatin1(kPictures));
    if (file.isEmpty() || !still(asked)) return false;
    const std::string layer = chosen->layer->id;
    const std::optional<Taken> taken = take_picture(*this, *session_, file, true);
    if (!taken) return false;
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    Json op{{"op", "smart_replace"}, {"page", page->index.json()}, {"id", layer}, {"asset", taken->ref}, {"name", taken->name.toStdString()}};
    op.update(taken->use);
    if (!apply_ops(Json::array({op}))) return false;
    flash(QStringLiteral("スマートオブジェクトの中身を「%1」にしました").arg(taken->name), 4000);
    return true;
}

bool MainWindow::relink_contents(std::optional<QString> path) {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    if (!session_->path()) {
        flash(QStringLiteral("画像を置く前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const Asked asked = asking();
    const QString file = path ? *path : ask::open_path(this, QStringLiteral("ファイルに再リンク"), QString::fromLatin1(kPictures));
    if (file.isEmpty() || !still(asked)) return false;
    const std::string layer = chosen->layer->id;
    const std::optional<Taken> taken = take_picture(*this, *session_, file, true);
    if (!taken) return false;
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    Json op{{"op", "smart_relink"}, {"page", page->index.json()}, {"id", layer}, {"asset", taken->ref}, {"name", taken->name.toStdString()}, {"link", taken->link}};
    op.update(taken->use);
    if (!apply_ops(Json::array({op}))) return false;
    trust_link(file);
    check_links();
    flash(QStringLiteral("リンク先を「%1」にしました").arg(QDir::toNativeSeparators(file)), 4000);
    return true;
}

bool MainWindow::embed_contents() {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    if (!chosen->contents->link) {
        flash(QStringLiteral("このスマートオブジェクトは、もう原稿に埋め込まれています"), 4000);
        return false;
    }
    if (!apply_ops(Json::array({Json{{"op", "smart_embed"}, {"page", chosen->page->index.json()}, {"id", chosen->layer->id}}}))) return false;
    flash(QStringLiteral("埋め込みに変換しました（原稿の中の写しが中身です）"), 4000);
    return true;
}

bool MainWindow::link_contents(std::optional<QString> path) {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    if (chosen->contents->link) {
        flash(QStringLiteral("このスマートオブジェクトは、もうファイルにリンクしています"), 4000);
        return false;
    }
    if (!session_->path()) {
        flash(QStringLiteral("リンクにする前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const bool page_contents = chosen->contents->kind == "page";
    const Asked asked = asking();
    QString file;
    if (path) {
        file = *path;
    } else {
        const QString suggested = page_contents ? layer_title(*chosen->layer) + QStringLiteral(".psd") : QString::fromStdString(chosen->contents->name);
        const QString filter = page_contents ? QStringLiteral("PSD (*.psd)") : QStringLiteral("元のファイル (*%1)").arg(QString::fromStdString(chosen->contents->suffix));
        file = ask::save_path(this, QStringLiteral("リンクに変換"), suggested, filter);
    }
    if (file.isEmpty() || !still(asked)) return false;
    const std::string layer = chosen->layer->id;
    const std::string id = chosen->id;
    // the contents written to the file first (a page's as a PSD), then linked to it
    try {
        formats::smart_files::export_contents(book(), id, fpath(file), page_contents ? "psd" : "");
    } catch (const std::exception& error) {
        flash(QStringLiteral("ファイルに書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    trust_link(file);
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    Json op{{"op", "smart_link"}, {"page", page->index.json()}, {"id", layer}};
    if (page_contents) {  // (the PSD written is the contents now: put in the book)
        const std::optional<Taken> taken = take_picture(*this, *session_, file, false);
        if (!taken) return false;
        page = current_page();
        if (page == nullptr) return false;
        op["page"] = page->index.json();
        op["asset"] = taken->ref;
        op["link"] = taken->link;
    } else {
        const auto held = book().smart_contents.find(id);
        const core::Bytes bytes = held != book().smart_contents.end() && held->second ? render::smart::file_bytes(book(), *held->second) : nullptr;
        if (!bytes) return false;
        op["link"] = link_record(file, *bytes, session_->path());
    }
    if (!apply_ops(Json::array({op}))) return false;
    check_links();
    flash(QStringLiteral("「%1」に書き出して、リンクに変換しました").arg(QDir::toNativeSeparators(file)), 5000);
    return true;
}

std::optional<std::pair<std::string, std::string>> MainWindow::read_linked(const QString& file, bool chosen_here) {
    if (!chosen_here && !trusted_link(file)) {
        if (!ask::question(this, QStringLiteral("リンクしたファイルを読み込む"),
                           QStringLiteral("原稿に記録されたリンク先のファイルを読み込みます:\n%1\n\nこの場所のファイルを原稿に取り込んでよいですか").arg(file)))
            return std::nullopt;
    }
    const std::optional<Taken> taken = take_picture(*this, *session_, file, false);
    if (!taken) return std::nullopt;
    trust_link(file);
    return std::make_pair(taken->ref, taken->name.toStdString());
}

bool MainWindow::update_contents(bool all) {
    if (!session_->path()) {
        flash(QStringLiteral("原稿を保存してから更新します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    check_links();
    // (each linked contents changed: one op on a smart object showing them — on a page, or inside other contents)
    struct Wanted {
        std::string contents;
        QString file;
        Json target;  // {"page", "id", "contents"?}
    };
    std::vector<Wanted> wanted;
    const core::Document& b = book();
    if (all) {
        for (const auto& [id, status] : link_status_) {
            if (status != "modified") continue;
            const core::SmartContents* contents = b.smart_contents.contains(id) ? b.smart_contents.at(id).get() : nullptr;
            const auto found = contents != nullptr && contents->link ? formats::smart_files::linked_file(*contents->link, session_->path()) : std::nullopt;
            const auto shown = core::smart::showing(b, id);
            if (!found || !shown) continue;
            Json target{{"page", shown->page.json()}, {"id", shown->layer}};
            if (shown->inside) target["contents"] = *shown->inside;
            wanted.push_back(Wanted{id, qpath(*found), target});
        }
        if (wanted.empty()) {
            flash(QStringLiteral("変更されたリンクのファイルはありません"), 4000);
            return false;
        }
    } else {
        const auto chosen = chosen_smart(*this);
        if (!chosen) return false;
        if (!chosen->contents->link) {
            flash(QStringLiteral("このスマートオブジェクトはファイルにリンクしていません"), 4000);
            return false;
        }
        const std::string status = link_status(chosen->id);
        if (status == "missing") {
            flash(QStringLiteral("リンクしたファイルが見つかりません（ファイルに再リンク… で場所を直せます）"), 6000, true);
            return false;
        }
        if (status != "modified") {
            flash(QStringLiteral("リンクしたファイルは変わっていません"), 4000);
            return false;
        }
        const auto found = formats::smart_files::linked_file(*chosen->contents->link, session_->path());
        if (!found) return false;
        Json target{{"page", chosen->page->index.json()}, {"id", chosen->layer->id}};
        if (contents_) target["contents"] = *contents_;
        wanted.push_back(Wanted{chosen->id, qpath(*found), target});
    }
    Json ops = Json::array();
    for (const Wanted& w : wanted) {
        const auto read = read_linked(w.file, false);
        if (!read) continue;
        Json op{{"op", "smart_update"}, {"asset", read->first}};
        op.update(w.target);
        ops.push_back(op);
    }
    if (ops.empty()) return false;
    // (the ops name where each smart object is: sent as they are, not into the contents of the tab in front)
    const std::optional<std::string> tab = std::exchange(contents_, std::nullopt);
    const bool ok = apply_ops(ops);
    contents_ = tab;
    show_page();
    if (!ok) return false;
    check_links();
    flash(ops.size() == 1 ? QStringLiteral("リンクしたファイルの変更を取り込みました") : QStringLiteral("リンクしたファイル %1 件の変更を取り込みました").arg(ops.size()), 5000);
    return true;
}

bool MainWindow::reveal_contents() {
    const auto chosen = chosen_smart(*this);
    if (!chosen) return false;
    if (!chosen->contents->link) {
        flash(QStringLiteral("埋め込みのスマートオブジェクトには、表示するファイルがありません"), 4000);
        return false;
    }
    const auto found = formats::smart_files::linked_file(*chosen->contents->link, session_->path());
    if (!found) {
        flash(QStringLiteral("リンクしたファイルが見つかりません（ファイルに再リンク… で場所を直せます）"), 6000, true);
        return false;
    }
    if (!ask::open_url(QUrl::fromLocalFile(QFileInfo(qpath(*found)).absolutePath()))) {
        flash(QStringLiteral("フォルダーを開けませんでした"), 5000, true);
        return false;
    }
    return true;
}

bool MainWindow::place_linked(std::optional<QString> chosen) {
    if (current_page() == nullptr) return false;
    const Asked asked = asking();
    if (!session_->path()) {
        flash(QStringLiteral("画像を置く前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const QString file = chosen ? *chosen : ask::open_path(this, QStringLiteral("リンクを配置"), QString::fromLatin1(kPictures));
    if (file.isEmpty() || !still(asked)) return false;
    if (!place_smart(file, false, true)) return false;
    check_links();
    return true;
}

bool MainWindow::package_book(std::optional<QString> folder) {
    if (!session_->path()) {
        flash(QStringLiteral("パッケージの前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return false;
    }
    const QString out = folder ? *folder : ask::existing_dir(this, QStringLiteral("パッケージを作るフォルダー"));
    if (out.isEmpty()) return false;
    session_->save_now();
    if (!session_->wait_saved(std::chrono::milliseconds(30000)) || session_->unsaved()) {
        flash(QStringLiteral("保存が終わらないため、パッケージを作れませんでした"), 6000, true);
        return false;
    }
    Json made;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        made = formats::smart_files::package(*session_->path(), fpath(out), session_->actor());
    } catch (const std::exception& error) {
        QGuiApplication::restoreOverrideCursor();
        flash(QStringLiteral("パッケージを作れませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    QGuiApplication::restoreOverrideCursor();
    flash(QStringLiteral("パッケージを作りました: %1（リンクしたファイル %2 件を Links に）")
              .arg(QDir::toNativeSeparators(QString::fromStdString(made["book"].get<std::string>())))
              .arg(made["relinked"].get<int>()),
          6000);
    return true;
}

std::string MainWindow::link_status(const std::string& contents_id) const {
    const auto found = link_status_.find(contents_id);
    return found == link_status_.end() ? std::string() : found->second;
}

void MainWindow::check_links() {
    if (closed_ || !session_) return;
    const core::Document& b = book();
    std::map<std::string, std::string> now;
    QStringList watched;
    for (const std::string& id : core::smart::used_contents(b)) {
        const auto found = b.smart_contents.find(id);
        if (found == b.smart_contents.end() || !found->second || !found->second->link) continue;
        const Json& link = *found->second->link;
        const auto file = formats::smart_files::linked_file(link, session_->path());
        if (!file) {
            now[id] = "missing";
            continue;
        }
        const QString at = qpath(*file);
        watched << at;
        now[id] = linked_sha(at) == core::py_str(core::py_get(link, "sha256", Json(""))) ? "ok" : "modified";
    }
    bool newly_modified = false;
    for (const auto& [id, status] : now) {
        if (status == "modified" && link_status(id) != "modified") newly_modified = true;
    }
    const bool differs = now != link_status_;
    link_status_ = std::move(now);
    // (a file replaced as it is saved leaves the watcher: each one looked for again)
    const QStringList had = link_watcher_.files();
    for (const QString& file : had) {
        if (!watched.contains(file)) link_watcher_.removePath(file);
    }
    for (const QString& file : watched) {
        if (!had.contains(file)) link_watcher_.addPath(file);
    }
    if (differs && layer_panel_ != nullptr) layer_panel_->refresh();
    if (newly_modified) {
        flash(QStringLiteral("リンクしたファイルが変更されました（レイヤー → スマートオブジェクト → 変更されたコンテンツを更新 で取り込めます）"), 6000);
    }
}

}  // namespace genko::app
