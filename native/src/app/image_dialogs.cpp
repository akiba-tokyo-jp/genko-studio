// PC2c (局所の補正と画像の演算, design 5): the windows and commands of shadows_highlights, hdr_toning, replace_color,
// match_color, apply_image, calculations and trap in レイヤー → 色調補正, and 編集 → フェード (app/adjust_forms.hpp). Each
// works the layer drawn on (inside the selection where the op takes one), shows its result on the page while its numbers
// change (250 ms), and is one change (編集 → 取り消し). フェード is offered right after a filter, a correction or a fill of a
// paint layer's pixels: the pixels before are put in the book's assets and fade_layer names them.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <map>
#include <memory>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"

namespace genko::app::adjust_forms {

using core::Json;

namespace {

const std::vector<std::pair<std::string, QString>>& blend_words() {
    static const std::vector<std::pair<std::string, QString>> words{
        {"normal", QStringLiteral("通常")},           {"multiply", QStringLiteral("乗算")},
        {"screen", QStringLiteral("スクリーン")},     {"add", QStringLiteral("加算（発光）")},
        {"overlay", QStringLiteral("オーバーレイ")},  {"darken", QStringLiteral("比較（暗）")},
        {"lighten", QStringLiteral("比較（明）")},    {"color_burn", QStringLiteral("焼き込みカラー")},
        {"linear_burn", QStringLiteral("焼き込み（リニア）")}, {"color_dodge", QStringLiteral("覆い焼きカラー")},
        {"soft_light", QStringLiteral("ソフトライト")}, {"hard_light", QStringLiteral("ハードライト")},
        {"difference", QStringLiteral("差の絶対値")}, {"exclusion", QStringLiteral("除外")},
        {"subtract", QStringLiteral("減算")},         {"divide", QStringLiteral("除算")},
        {"hue", QStringLiteral("色相")},             {"saturation", QStringLiteral("彩度")},
        {"color", QStringLiteral("カラー")},          {"luminosity", QStringLiteral("輝度")}};
    return words;
}

// The blends of Genko (all, or Apply Image's and Calculations': the separable ones, add and subtract with their scale).
std::vector<std::pair<QString, Json>> blend_options(bool separable) {
    std::vector<std::pair<QString, Json>> out;
    for (const auto& [name, words] : blend_words()) {
        const bool whole = name == "hue" || name == "saturation" || name == "color" || name == "luminosity";
        if (separable && whole) continue;
        QString label = words;
        if (separable && name == "add") label = QStringLiteral("加算（拡大・オフセット）");
        if (separable && name == "subtract") label = QStringLiteral("減算（拡大・オフセット）");
        out.emplace_back(label, Json(name));
    }
    return out;
}

// A correction's window of the group's own fields (as adjust_dialog: 既定に戻す, the result on the page 250 ms after a
// change, かける / やめる). The settings, or none when the person stopped.
std::optional<Json> image_dialog(QWidget* parent, const QString& title, const std::string& kind, const std::function<void(Form&)>& build,
                                 FilterPreview preview) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("adjust_dialog"));
    dialog.setProperty("kind", QString::fromStdString(kind));
    dialog.setWindowTitle(title);
    auto* layout = new QFormLayout(&dialog);
    auto* form = new Form;
    build(*form);
    form->set_values(Json::object());
    layout->addRow(form);
    auto* row = new QDialogButtonBox;
    QPushButton* reset = row->addButton(QStringLiteral("既定に戻す"), QDialogButtonBox::ResetRole);
    reset->setObjectName(QStringLiteral("adjust_reset"));
    QObject::connect(reset, &QPushButton::clicked, form, &Form::reset);
    layout->addRow(row);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    QCheckBox* check = nullptr;
    if (preview) {
        check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
        check->setObjectName(QStringLiteral("filter_preview"));
        check->setChecked(true);
        layout->addRow(QString(), check);
        QObject::connect(form, &Form::changed, &dialog, [timer] { timer->start(); });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [timer] { timer->start(); });
        QObject::connect(timer, &QTimer::timeout, &dialog, [&] {
            try {
                preview(check->isChecked() ? std::optional<Json>(form->values()) : std::nullopt);
            } catch (const std::exception&) {
                preview(std::nullopt);  // (a preview that cannot be made must not stop the window)
            }
        });
        timer->start(0);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("かける"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    const bool ok = ask::exec(&dialog) == QDialog::Accepted;
    timer->stop();
    const Json result = form->values();
    if (preview) preview(std::nullopt);
    if (!ok) return std::nullopt;
    return result;
}

// The values of the window merged into an op: a choice's object (a key "…" holding several settings) laid in, nulls left
// out.
Json with_values(Json op, const Json& values) {
    for (const auto& [key, value] : values.items()) {
        if (value.is_null()) continue;
        if (key.starts_with("_") && value.is_object()) {
            for (const auto& [inner, v] : value.items()) op[inner] = v;
            continue;
        }
        if (value.is_object()) {
            Json cleaned = Json::object();
            for (const auto& [inner, v] : value.items())
                if (!v.is_null()) cleaned[inner] = v;
            op[key] = cleaned;
            continue;
        }
        op[key] = value;
    }
    return op;
}

// The layer drawn on and its page, or none (said in the status line).
struct Drawn {
    const core::Page* page = nullptr;
    const core::Layer* layer = nullptr;
};
Drawn drawn(MainWindow* window, bool paint) {
    Drawn out;
    out.page = window->current_page();
    out.layer = window->target_layer();
    if (out.page == nullptr || out.layer == nullptr) {
        window->flash(QStringLiteral("描く先のレイヤーを選びます"), 3000);
        return {};
    }
    if (paint && out.layer->kind != core::LayerKind::Raster && out.layer->kind != core::LayerKind::Strokes) {
        window->flash(QStringLiteral("描画のレイヤー（ペン・ペイント）を選びます"), 3000);
        return {};
    }
    return out;
}

QString layer_words(const core::Layer& layer) {
    return wording::layer_label(layer);
}

// A source of the page's layers (and the page merged; other pages of its size): {"layer": …} or {"page": n}.
std::vector<std::pair<QString, Json>> source_options(const core::Document& book, const core::Page& page) {
    std::vector<std::pair<QString, Json>> out{{QStringLiteral("このページ（統合）"), Json{{"layer", "merged"}}}};
    for (const core::Layer& layer : page.layers) out.emplace_back(QStringLiteral("レイヤー: %1").arg(layer_words(layer)), Json{{"layer", layer.id}});
    for (const auto& other : book.pages) {
        if (other.get() == &page || other->spec.width_mm.value() != page.spec.width_mm.value() ||
            other->spec.height_mm.value() != page.spec.height_mm.value())
            continue;
        out.emplace_back(QStringLiteral("p.%1（統合）").arg(QString::fromStdString(other->index.repr())), Json{{"page", other->index.json()}, {"layer", "merged"}});
    }
    return out;
}

// The channels a source may name on the page: the composite, each colour plate, the alpha and spot channels, the
// transparency and the mask.
std::vector<std::pair<QString, Json>> channel_options(const core::Page& page, bool layer_planes) {
    std::vector<std::pair<QString, Json>> out{{QStringLiteral("合成"), Json("composite")}};
    const std::vector<std::string> names = core::plate_names(page);
    const std::vector<std::string> labels = core::plate_labels(page);
    for (std::size_t i = 0; i < names.size() && names.size() > 1; ++i) out.emplace_back(QString::fromStdString(labels[i]), Json(names[i]));
    for (const core::Channel& channel : page.channels) out.emplace_back(QString::fromStdString(channel.name), Json(channel.id));
    if (layer_planes) {
        out.emplace_back(QStringLiteral("透明部分"), Json("transparency"));
        out.emplace_back(QStringLiteral("マスク"), Json("mask"));
    }
    return out;
}

// The source's fields under `key` (its layer or page, its channel, inverted).
void source_fields(Form& form, const std::string& key, const QString& label, const core::Document& book, const core::Page& page) {
    form.choice(key, label, source_options(book, page), Json{{"layer", "merged"}});
    form.choice(key + ".channel", QStringLiteral("チャンネル"), channel_options(page, true), Json("composite"));
    form.check(key + ".invert", QStringLiteral("階調を反転"), false);
}

// The op on the snapshot of the book (the window's preview).
void preview_op(MainWindow* window, const Json& op, const std::optional<Json>& values) {
    if (!values) {
        window->preview_ops(std::nullopt);
        return;
    }
    window->preview_ops(Json::array({with_values(op, *values)}));
}

// --- filter_raster's local corrections ---------------------------------------------------------------------------------

void local_filter(MainWindow* window, const std::string& kind, const QString& title, const std::function<void(Form&, const core::Page&)>& build) {
    const auto asked = window->asking();
    const Drawn d = drawn(window, false);
    if (d.page == nullptr) return;
    // (PC3 A2: シャドウ・ハイライト on a smart object is a smart filter, as Photoshop makes it; the selection its filter mask)
    const bool smart = d.layer->kind == core::LayerKind::Smart && kind == "shadows_highlights";
    Json op{{"op", smart ? "smart_filter_add" : "filter_raster"}, {"page", d.page->index.json()}, {"id", d.layer->id}, {"kind", kind}};
    if (const auto area = window->selection_area()) op["area"] = *area;
    const bool lines = d.layer->stroke_count() > 0;
    const core::Page page = *d.page;
    const auto values = image_dialog(window, title, kind,
                                     [&](Form& form) {
                                         build(form, page);
                                         if (lines) form.note(QStringLiteral("ペンの線は画像になり、あとから線として消せなくなります。"));
                                     },
                                     [window, op](const std::optional<Json>& v) { preview_op(window, op, v); });
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({with_values(op, *values)}));
}

void shadows_highlights(MainWindow* window) {
    local_filter(window, "shadows_highlights", QStringLiteral("シャドウ・ハイライト"), [](Form& form, const core::Page&) {
        form.from_table(*core::adjust_kind("shadows_highlights"),
                        {{"shadow_amount", QStringLiteral("シャドウ: 量（%）")},
                         {"shadow_tone", QStringLiteral("シャドウ: 階調（%）")},
                         {"shadow_radius", QStringLiteral("シャドウ: 半径（px）")},
                         {"highlight_amount", QStringLiteral("ハイライト: 量（%）")},
                         {"highlight_tone", QStringLiteral("ハイライト: 階調（%）")},
                         {"highlight_radius", QStringLiteral("ハイライト: 半径（px）")},
                         {"color", QStringLiteral("カラーの補正")},
                         {"midtone", QStringLiteral("中間調のコントラスト")},
                         {"black_clip", QStringLiteral("シャドウのクリップ（%）")},
                         {"white_clip", QStringLiteral("ハイライトのクリップ（%）")}});
        form.note(QStringLiteral("半径は 200 dpi の画素で数えます。暗い所を持ち上げ、明るい所を抑えます。"));
    });
}

void replace_color(MainWindow* window) {
    std::array<int, 3> pen{20, 20, 20};
    const auto& rgb = window->pen().rgb;
    for (std::size_t c = 0; c < 3 && c < rgb.size(); ++c) pen[c] = static_cast<int>(rgb[c]);
    local_filter(window, "replace_color", QStringLiteral("色の置き換え"), [pen](Form& form, const core::Page&) {
        form.colour("color", QStringLiteral("置き換える色"), pen);
        form.from_table(*core::adjust_kind("replace_color"), {{"fuzziness", QStringLiteral("許容量")},
                                                               {"hue", QStringLiteral("色相")},
                                                               {"saturation", QStringLiteral("彩度")},
                                                               {"lightness", QStringLiteral("明度")}});
    });
}

// 色の一致: the source among the page's layers, the page merged, the book's other pages, and the other books open (their
// page shown: its statistics worked out here and given to the op — the op reads only its own book, the user's decision 7).
void match_color(MainWindow* window) {
    std::vector<std::pair<QString, Json>> sources{{QStringLiteral("なし（このレイヤーの色で）"), Json()}};
    const core::Page* here = window->current_page();
    if (here != nullptr) {
        for (const auto& [label, value] : source_options(window->book(), *here)) sources.emplace_back(label, Json{{"source", value}});
    }
    const auto& tabs = window->documents();
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (i == window->current_document() || !tabs[static_cast<std::size_t>(i)].session) continue;
        const auto& tab = tabs[static_cast<std::size_t>(i)];
        const core::Document& other = tab.session->document();
        const std::size_t at = static_cast<std::size_t>(std::clamp(tab.page_index, 0, std::max(0, static_cast<int>(other.pages.size()) - 1)));
        if (other.pages.empty() || other.is_deferred(at)) continue;
        try {
            const auto stats = render::image_ops::lab_stats(render::image_ops::rgba_of(render::image_ops::page_pixels(other, other.page(at))));
            if (!stats) continue;
            sources.emplace_back(QStringLiteral("「%1」の p.%2").arg(tab.title(), QString::fromStdString(other.page(at).index.repr())),
                                 Json{{"source_stats", Json{{"mean", Json::array({stats->mean[0], stats->mean[1], stats->mean[2]})},
                                                            {"std", Json::array({stats->std[0], stats->std[1], stats->std[2]})}}}});
        } catch (const std::exception&) {
            // (a page that cannot be drawn offers no statistics)
        }
    }
    local_filter(window, "match_color", QStringLiteral("色の一致"), [sources](Form& form, const core::Page&) {
        form.choice("_match", QStringLiteral("元の画像"), sources, Json());
        form.from_table(*core::adjust_kind("match_color"), {{"luminance", QStringLiteral("輝度")},
                                                             {"intensity", QStringLiteral("カラーの強さ")},
                                                             {"fade", QStringLiteral("フェード")},
                                                             {"neutralize", QStringLiteral("中和")},
                                                             {"stats_in_area", QStringLiteral("選択範囲で色を計算")}});
    });
}

// --- the ops of their own ------------------------------------------------------------------------------------------

void hdr_toning(MainWindow* window) {
    const auto asked = window->asking();
    const core::Page* page = window->current_page();
    if (page == nullptr) return;
    const render::FlattenCount count = render::flatten_count(*page);
    if (count.merged >= 2 || count.discarded > 0) {
        const QString layers = count.merged >= 2 ? QStringLiteral("このページの画素レイヤー（%1 枚）を 1 枚に統合します").arg(count.merged)
                                                 : QStringLiteral("このページの画素レイヤーを 1 枚に統合します");
        if (!ask::question(window, QStringLiteral("画像を統合"),
                           QStringLiteral("HDR トーンにすると、%1。統合しますか？\n%2台詞・フキダシは要素のまま残ります。編集 → 取り消し で元に戻せます。")
                               .arg(layers, MainWindow::flatten_discards(count.discarded))))
            return;
        if (!window->still(asked)) return;
    }
    const Json op{{"op", "hdr_toning"}, {"page", page->index.json()}};
    const auto values = image_dialog(window, QStringLiteral("HDR トーン"), "hdr_toning",
                                     [](Form& form) {
                                         form.number("radius", QStringLiteral("エッジ: 半径（px）"), 1, 500, 30, 0);
                                         form.number("strength", QStringLiteral("エッジ: 強さ"), 0.1, 4, 0.5, 2);
                                         form.number("gamma", QStringLiteral("ガンマ"), 0.1, 2, 1, 2);
                                         form.number("exposure", QStringLiteral("露光量"), -5, 5, 0, 2);
                                         form.number("detail", QStringLiteral("ディテール（%）"), -100, 300, 30, 0);
                                         form.number("shadow", QStringLiteral("シャドウ"), -100, 100, 0, 0);
                                         form.number("highlight", QStringLiteral("ハイライト"), -100, 100, 0, 0);
                                         form.number("vibrance", QStringLiteral("自然な彩度"), -100, 100, 0, 0);
                                         form.number("saturation", QStringLiteral("彩度"), -100, 100, 20, 0);
                                         form.note(QStringLiteral("ページの画素レイヤーを 1 枚に統合してからかけます。"));
                                     },
                                     [window, op](const std::optional<Json>& v) { preview_op(window, op, v); });
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({with_values(op, *values)}));
}

void apply_image(MainWindow* window) {
    const auto asked = window->asking();
    const Drawn d = drawn(window, true);
    if (d.page == nullptr) return;
    Json op{{"op", "apply_image"}, {"page", d.page->index.json()}, {"id", d.layer->id}};
    if (const auto area = window->selection_area()) op["area"] = *area;
    const core::Page page = *d.page;
    const core::Document& book = window->book();
    std::vector<std::pair<QString, Json>> masks{{QStringLiteral("なし"), Json()}};
    for (const core::Channel& channel : page.channels) masks.emplace_back(QString::fromStdString(channel.name), Json{{"channel", channel.id}});
    const auto values = image_dialog(window, QStringLiteral("画像操作"), "apply_image",
                                     [&](Form& form) {
                                         source_fields(form, "source", QStringLiteral("元の画像"), book, page);
                                         form.choice("blending", QStringLiteral("描画モード"), blend_options(true), Json("multiply"));
                                         form.number("opacity", QStringLiteral("不透明度（%）"), 0, 100, 100, 0);
                                         form.number("scale", QStringLiteral("拡大・縮小（加算・減算）"), 1, 2, 1, 3);
                                         form.number("offset", QStringLiteral("オフセット（加算・減算）"), -255, 255, 0, 0);
                                         form.check("preserve_transparency", QStringLiteral("透明部分を保護"), false);
                                         form.choice("mask", QStringLiteral("マスク"), masks, Json());
                                     },
                                     [window, op](const std::optional<Json>& v) { preview_op(window, op, v); });
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({with_values(op, *values)}));
}

void calculations(MainWindow* window) {
    const auto asked = window->asking();
    const core::Page* here = window->current_page();
    if (here == nullptr) return;
    const core::Page page = *here;
    const Json op{{"op", "calculations"}, {"page", page.index.json()}};
    const core::Document& book = window->book();
    auto* name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("（空: Alpha 1, 2, …）"));
    const auto values = image_dialog(window, QStringLiteral("演算"), "calculations",
                                     [&](Form& form) {
                                         source_fields(form, "source1", QStringLiteral("ソース 1"), book, page);
                                         source_fields(form, "source2", QStringLiteral("ソース 2"), book, page);
                                         form.choice("blending", QStringLiteral("描画モード"), blend_options(true), Json("multiply"));
                                         form.number("opacity", QStringLiteral("不透明度（%）"), 0, 100, 100, 0);
                                         form.number("scale", QStringLiteral("拡大・縮小（加算・減算）"), 1, 2, 1, 3);
                                         form.number("offset", QStringLiteral("オフセット（加算・減算）"), -255, 255, 0, 0);
                                         form.choice("result", QStringLiteral("結果"),
                                                     {{QStringLiteral("新しいアルファチャンネル"), Json("channel")},
                                                      {QStringLiteral("新しいページ（グレー）"), Json("page")},
                                                      {QStringLiteral("選択範囲"), Json("selection")}},
                                                     Json("channel"));
                                         form.custom("name", QStringLiteral("名前"), name,
                                                     [name] { return name->text().trimmed().isEmpty() ? Json() : Json(name->text().trimmed().toStdString()); },
                                                     [name](const Json& v) { name->setText(v.is_string() ? QString::fromStdString(v.get<std::string>()) : QString()); },
                                                     Json());
                                         form.note(QStringLiteral("ソース 1 をソース 2 の上に重ねます（ページの 200 dpi で）。"));
                                     },
                                     {});
    if (!values || !window->still(asked)) return;
    const Json made = with_values(op, *values);
    if (made.value("result", "channel") != "selection") {
        window->apply_ops(Json::array({made}));
        return;
    }
    // (a selection: the book as it is — the op on its snapshot, its words taken as the selection)
    try {
        const auto result = core::CommandBus(render::ops_registry()).apply(*window->session().snapshot(), Json::array({made}),
                                                                           core::Actor(window->session().actor()), true);
        const Json area = result.results.empty() ? Json() : result.results[0].value("area", Json());
        if (area.is_null()) {
            window->flash(QStringLiteral("演算の結果に、選択する所がありません"), 3000);
            return;
        }
        if (window->canvas()->tool() != QLatin1String("marquee")) window->choose_tool(QStringLiteral("rect"));
        window->canvas()->set_selection(area);
    } catch (const core::Error& error) {
        window->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
    }
}

void trap(MainWindow* window) {
    const auto asked = window->asking();
    const Drawn d = drawn(window, true);
    if (d.page == nullptr) return;
    if (!d.page->color || d.page->color->space != core::InkSpace::Cmyk) {
        window->flash(QStringLiteral("トラッピングは CMYK のページでだけ使えます（ページ → 色モード → CMYK）"), 4000);
        return;
    }
    const Json op{{"op", "trap"}, {"page", d.page->index.json()}, {"id", d.layer->id}};
    const auto values = image_dialog(window, QStringLiteral("トラッピング"), "trap",
                                     [](Form& form) {
                                         form.number("width_mm", QStringLiteral("幅（mm）"), 0.01, 1, 0.1, 2);
                                         form.note(QStringLiteral("色と色の境目で、それぞれのインキを幅の分だけ広げて重ねます（紙との境目は変えません）。"));
                                     },
                                     [window, op](const std::optional<Json>& v) { preview_op(window, op, v); });
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({with_values(op, *values)}));
}

// --- フェード ---------------------------------------------------------------------------------------------------------

// What フェード would fade, kept for each window: the book right after the change, the layer and its pixels before.
struct FadeSource {
    DocPtr after;
    std::string page_id;
    std::string layer_id;
    core::Bytes before;
    std::string suffix;
    QString label;
};

std::map<const MainWindow*, FadeSource>& fades() {
    static std::map<const MainWindow*, FadeSource> all;
    return all;
}

// The ops whose result フェード takes back toward what was before (a filter, a correction, a fill: PhotoCraft's
// fadeable commands).
bool fadeable(const Json& ops) {
    static const std::vector<std::string> names{"filter_raster", "auto_adjust", "apply_image", "fill", "fill_area", "fill_enclosed",
                                                "flood_fill", "gradient_fill"};
    if (!ops.is_array() || ops.size() != 1 || !ops[0].is_object()) return false;
    const Json* name = core::get(ops[0], "op");
    return name != nullptr && name->is_string() && std::find(names.begin(), names.end(), name->get<std::string>()) != names.end();
}

// The one paint layer whose pixels the change changed (of the same kind before and after, holding no lines or fills),
// its pixels before.
std::optional<FadeSource> changed_pixels(const core::Document& before, const core::Document& after) {
    std::optional<FadeSource> out;
    for (std::size_t p = 0; p < after.pages.size() && p < before.pages.size(); ++p) {
        if (after.pages[p] == before.pages[p] || after.is_deferred(p) || before.is_deferred(p)) continue;
        if (before.page(p).id != after.page(p).id) return std::nullopt;
        for (const core::Layer& now : after.page(p).layers) {
            const core::Layer* was = nullptr;
            for (const core::Layer& each : before.page(p).layers) {
                if (each.id == now.id) was = &each;
            }
            if (was == nullptr) continue;
            if (was->raster_png == now.raster_png && was->color_raster == now.color_raster && was->ink_raster == now.ink_raster) continue;
            if (out) return std::nullopt;  // (more than one layer changed)
            const bool lines = was->stroke_count() > 0 || !was->patches.empty() || now.stroke_count() > 0 || !now.patches.empty();
            FadeSource f;
            f.page_id = after.page(p).id;
            f.layer_id = now.id;
            if (now.color_raster && was->color_raster) {
                f.before = was->color_raster;
                f.suffix = std::string(core::kColorRasterSuffix);
            } else if (now.ink_raster && was->ink_raster && !now.raster_png) {
                f.before = was->ink_raster;
                f.suffix = std::string(core::kInkRasterSuffix);
            } else if (now.raster_png && was->raster_png && !was->raster_png->empty()) {
                f.before = was->raster_png;
                f.suffix = ".png";
            } else {
                return std::nullopt;
            }
            if (lines) return std::nullopt;
            out = std::move(f);
        }
    }
    return out;
}

const FadeSource* current_fade(const MainWindow& window) {
    const auto it = fades().find(&window);
    if (it == fades().end() || it->second.after != window.session().snapshot()) return nullptr;
    return &it->second;
}

void follow_fade(MainWindow& window) {
    if (QAction* act = window.action(QStringLiteral("act_fade"))) act->setEnabled(current_fade(window) != nullptr);
}

void fade(MainWindow* window) {
    const FadeSource* source = current_fade(*window);
    if (source == nullptr) {
        window->flash(QStringLiteral("フェードは、フィルター・補正・塗りのすぐ後にだけ使えます"), 3000);
        return;
    }
    if (!window->session().path()) {
        window->flash(QStringLiteral("フェードの前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return;
    }
    const auto asked = window->asking();
    const FadeSource kept = *source;
    const core::Document& book = window->book();
    std::size_t at = 0;
    while (at < book.pages.size() && book.pages[at]->id != kept.page_id) ++at;
    if (at >= book.pages.size()) return;
    const bool precise = kept.suffix == core::kColorRasterSuffix;
    if (!precise && kept.before->size() > storage::kAssetMaxBytes) {  // (more than the op reads of one asset)
        window->flash(QStringLiteral("このレイヤーは大きすぎてフェードできません（前の画素が 256 MiB を超えます）"), 6000, true);
        return;
    }
    Json from;
    try {  // (the pixels before into the book's assets: the op names them — a precise layer's in tiles, as the book saves
           // them, so a layer of any size fades)
        storage::AssetStore store(*window->session().path());
        if (precise) {
            std::vector<std::string> refs;
            for (const std::string& tile : core::color_tiles(*kept.before)) refs.push_back(store.put_bytes(tile, core::kColorRasterSuffix));
            from = core::tiled_metadata(core::ColorRasterView(*kept.before), refs, core::color_raster_space(book.page(at).rgb));
        } else {
            from = store.put_bytes(*kept.before, kept.suffix);
        }
    } catch (const std::exception& error) {
        window->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    const Json op{{"op", "fade_layer"}, {"page", book.page(at).index.json()}, {"id", kept.layer_id}, {"from", from}};
    const auto values = image_dialog(window, QStringLiteral("フェード: %1").arg(kept.label), "fade_layer",
                                     [](Form& form) {
                                         form.number("opacity", QStringLiteral("不透明度（%）"), 0, 100, 100, 0);
                                         std::vector<std::pair<QString, Json>> blends;
                                         for (const auto& [name, words] : blend_words()) blends.emplace_back(words, Json(name));
                                         form.choice("blend", QStringLiteral("描画モード"), blends, Json("normal"));
                                     },
                                     [window, op](const std::optional<Json>& v) { preview_op(window, op, v); });
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({with_values(op, *values)}));
}

}  // namespace

void follow_change(MainWindow& window, const BookChange& change) {
    std::optional<FadeSource> made;
    if (change.why == BookChange::Why::Edit && change.before && change.after && fadeable(change.ops)) {
        made = changed_pixels(*change.before, *change.after);
        if (made) {
            made->after = change.after;
            const Json* name = core::get(change.ops[0], "op");
            made->label = name != nullptr && *name == Json("filter_raster") ? QStringLiteral("フィルター・補正") : QStringLiteral("塗り");
        }
    }
    if (made) {
        fades()[&window] = std::move(*made);
    } else {
        fades().erase(&window);
    }
    follow_fade(window);
}

void build_image_menu(QMenu* adjust, QMenu* edit, const MenuContext& context) {
    MainWindow* window = context.window;
    adjust->addSeparator();
    adjust->addAction(context.make(QStringLiteral("act_shadows_highlights"), QStringLiteral("シャドウ・ハイライト…"), [window] { shadows_highlights(window); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_hdr_toning"), QStringLiteral("HDR トーン…"), [window] { hdr_toning(window); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_replace_color"), QStringLiteral("色の置き換え…"), [window] { replace_color(window); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_match_color"), QStringLiteral("色の一致…"), [window] { match_color(window); }, {}));
    adjust->addSeparator();
    adjust->addAction(context.make(QStringLiteral("act_apply_image"), QStringLiteral("画像操作…"), [window] { apply_image(window); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_calculations"), QStringLiteral("演算…"), [window] { calculations(window); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_trap"), QStringLiteral("トラッピング…"), [window] { trap(window); }, {}));
    // 編集 → フェード… (only right after a filter, a correction or a fill; no key: Ctrl+Shift+F is コマンドを探す)
    QAction* fade_action = context.make(QStringLiteral("act_fade"), QStringLiteral("フェード…"), [window] { fade(window); }, {});
    fade_action->setEnabled(false);
    edit->addSeparator();
    edit->addAction(fade_action);
    QObject::connect(edit, &QMenu::aboutToShow, window, [window] { follow_fade(*window); });
    QObject::connect(window, &QObject::destroyed, [window] { fades().erase(window); });
}

}  // namespace genko::app::adjust_forms
