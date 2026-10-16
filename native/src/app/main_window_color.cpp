// 色の校正・校正の設定・色域外警告・カラー設定・開いている原稿 (SPEC COLOR-03 PC1b; PhotoCraft v0.2.0's color_cmds.rs,
// proof_sim.rs and display_color.rs, MIT): view.proofColors, view.proofSetup and its kinds, view.gamutWarning,
// edit.colorSettings with its policies for the books opened and the pictures taken in, document.activate. What is shown
// (the proof, the mark, the monitor's colours) is the window's; what changes a book goes through its ops.

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/color_settings_dialog.hpp"
#include "app/config.hpp"
#include "app/display_color.hpp"
#include "app/documents.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/tiles.hpp"
#include "core/color_model.hpp"
#include "core/paths.hpp"
#include "render/colour.hpp"
#include "render/ink.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace cs = color_settings;
using render::proof::Kind;

namespace {

core::Bytes read_profile_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray bytes = file.read(static_cast<qint64>(core::kProfileMaxBytes) + 1);
    if (bytes.isEmpty() || bytes.size() > static_cast<qsizetype>(core::kProfileMaxBytes)) return nullptr;
    return std::make_shared<const std::string>(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// The proof kinds of 校正の設定, in the menu's order: (action, words, id, tip).
struct ProofItem {
    const char* attribute;
    const char* title;
    const char* id;
    const char* tip;
};
const std::vector<ProofItem>& proof_items() {
    static const std::vector<ProofItem> items = {
        {"act_proof_custom", "カスタム…", "custom", "校正に使うプロファイル（CMYK・RGB・グレー）、変換の意図、黒点の補正、紙色のシミュレーションを選びます"},
        {"act_proof_working_cmyk", "作業用 CMYK", "workingCmyk", "カラー設定の作業用 CMYK で刷ったときの色で見ます"},
        {"act_proof_export", "書き出しの CMYK（CMYK で見る）", "export",
         "書き出しで選んだ CMYK の ICC（無ければ Genko の変換: 墨版・総インキ量の制限）で刷ったときの色で見ます。これまでの「CMYK で見る」と同じです"},
        {"act_proof_cyan", "作業用シアン版", "workingCyanPlate", "作業用 CMYK のシアン版だけを、墨の濃さで見ます"},
        {"act_proof_magenta", "作業用マゼンタ版", "workingMagentaPlate", "作業用 CMYK のマゼンタ版だけを、墨の濃さで見ます"},
        {"act_proof_yellow", "作業用イエロー版", "workingYellowPlate", "作業用 CMYK のイエロー版だけを、墨の濃さで見ます"},
        {"act_proof_black", "作業用ブラック版", "workingBlackPlate", "作業用 CMYK のブラック（墨）版だけを見ます"},
        {"act_proof_cmy", "作業用 CMY 版", "workingCmyPlate", "作業用 CMYK の墨版を除いたシアン・マゼンタ・イエローで見ます"},
        {"act_proof_srgb", "インターネット標準の RGB（sRGB）", "internetStandardRgb", "ページの値を sRGB の色として見ます（Web・スマホでの見え方）"},
        {"act_proof_monitor", "モニターの RGB", "monitorRgb", "ページの値をそのままモニターに出したときの色で見ます（色の管理をしない画面での見え方）"},
        {"act_proof_legacy_mac", "旧 Macintosh の RGB（ガンマ 1.8）", "legacyMacintoshRgb", "ページの値をガンマ 1.8 の画面で見たときの色で見ます"},
        {"act_proof_protan", "P 型（1 型）色覚", "colorBlindnessProtanopia", "P 型（1 型）の色覚の人の見え方に近い色で見ます"},
        {"act_proof_deutan", "D 型（2 型）色覚", "colorBlindnessDeuteranopia", "D 型（2 型）の色覚の人の見え方に近い色で見ます"},
    };
    return items;
}

// 環境設定 → 透明部分・色域: the gamut warning's mark.
GamutLook gamut_look() {
    const auto store = settings();
    GamutLook look;
    const QColor colour(store->value(QStringLiteral("view/gamut_color"), QStringLiteral("#808080")).toString());
    if (colour.isValid()) look.color = colour;
    look.opacity = std::clamp(store->value(QStringLiteral("view/gamut_opacity"), 100).toInt(), 1, 100) / 100.0;
    return look;
}

// A built-in profile's bytes for a proof (sRGB: Little CMS's own, written out).
core::Bytes builtin_bytes(const core::BuiltinProfile& b) {
    if (b.bytes) return b.bytes;
    if (b.id == "srgb") return std::make_shared<const std::string>(render::colour::srgb_icc());
    return nullptr;
}

QString policy_of(const cs::Settings& s, cs::Space space) {
    return space == cs::Space::Rgb ? s.policy_rgb : space == cs::Space::Gray ? s.policy_gray : s.policy_cmyk;
}

// A page's or the book's colour model as the policies see it: its working space (none: Lab), its profile (ref and bytes;
// "" for sRGB) and its own way (intent, black point compensation).
struct Model {
    std::optional<cs::Space> space;
    std::string ref;
    core::Bytes icc;
    std::string intent = "relative";
    bool bpc = true;
};
Model model_of(const std::optional<core::PageColor>& color, const std::optional<core::RgbMode>& rgb) {
    Model out;
    if (color) {
        if (color->space == core::InkSpace::Cmyk) out.space = cs::Space::Cmyk;
        else if (color->space == core::InkSpace::Gray) out.space = cs::Space::Gray;
        out.ref = color->profile;
        out.icc = color->icc;
        out.intent = color->intent;
        out.bpc = color->bpc;
        return out;
    }
    out.space = cs::Space::Rgb;
    if (rgb) {
        out.ref = rgb->profile;
        out.icc = rgb->icc;
        out.intent = rgb->intent;
        out.bpc = rgb->bpc;
    }
    return out;
}

}  // namespace

void MainWindow::build_color_actions() {
    make("act_color_settings", QStringLiteral("カラー設定…"), [this] { color_settings_dialog(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+K"))},
         QStringLiteral("作業用の空間（RGB・CMYK・グレー）、プロファイルが違う原稿・画像の扱い、変換の意図、モニターのプロファイル"));
    auto* kinds = new QActionGroup(this);
    kinds->setExclusionPolicy(QActionGroup::ExclusionPolicy::Exclusive);
    for (const ProofItem& item : proof_items()) {
        const QString id = QString::fromLatin1(item.id);
        kinds->addAction(make(QString::fromLatin1(item.attribute), QString::fromUtf8(item.title), [this, id] { choose_proof(id); }, {},
                              QString::fromUtf8(item.tip), true));
    }
    QAction* gamut = make("act_gamut_warning", QStringLiteral("色域外警告"), [] {}, {QKeySequence(QStringLiteral("Ctrl+Shift+Y"))},
                          QStringLiteral("校正のプロファイルで刷れない色（ΔE が 4 を超える色）に印を付け、その数をステータスバーに出します（表示だけ）"), true);
    connect(gamut, &QAction::triggered, this, [this](bool on) {
        if (documents_.empty()) return;
        documents_[static_cast<std::size_t>(doc_)].proof.gamut = on;
        apply_proof_view();
    });
    gamut_label_ = new QLabel;
    gamut_label_->setObjectName(QStringLiteral("gamutCount"));
    gamut_label_->setToolTip(QStringLiteral("色域外警告: 校正のプロファイルで刷れない色の画素の数（表示している解像度で）"));
    gamut_label_->hide();
    statusBar()->addPermanentWidget(gamut_label_);
    connect(&canvas_->renderer(), &PageRenderer::gamutCounted, this, &MainWindow::refresh_gamut_count);
}

const ProofView& MainWindow::proof_view() const { return documents_[static_cast<std::size_t>(doc_)].proof; }

void MainWindow::choose_proof(const QString& id) {
    if (documents_.empty()) return;
    if (id == QLatin1String("custom")) {
        proof_custom_dialog();
        return;
    }
    ProofView& v = documents_[static_cast<std::size_t>(doc_)].proof;
    if (id == QLatin1String("export")) {
        v = ProofView{QStringLiteral("export"), Kind::Profile, 3, QStringLiteral("relative"), true, false, true, v.gamut};
    } else if (const auto kind = render::proof::kind_from_id(id.toStdString())) {
        if (kind->first == Kind::Profile) {  // (作業用 CMYK: its own defaults, as upstream's Proof Setup gives them)
            v = ProofView{QStringLiteral("working"), Kind::Profile, 3, QStringLiteral("relative"), true, false, true, v.gamut};
        } else {
            if (kind->first == Kind::Plate || kind->first == Kind::CmyPlate) v.profile = QStringLiteral("working");  // (plates: the working CMYK's)
            v.kind = kind->first;
            v.plate = kind->second;
        }
    }
    v.on = true;
    apply_proof_view();
}

void MainWindow::apply_proof_view() {
    if (documents_.empty() || canvas_ == nullptr) return;
    const ProofView& v = proof_view();
    render::proof::Setup setup;
    setup.kind = v.kind;
    setup.plate = v.plate;
    setup.intent = v.intent.toStdString();
    setup.bpc = v.bpc;
    setup.simulate_paper = v.simulate_paper;
    QString profile = v.profile;
    PageRenderer& r = canvas_->renderer();
    if (profile == QLatin1String("working")) {
        const cs::Working w = cs::working(cs::Space::Cmyk, cs::load());
        profile = w.spec == QLatin1String("export") ? QStringLiteral("export") : QString();
        if (profile.isEmpty()) setup.icc = w.icc;
    } else if (profile != QLatin1String("export")) {
        const core::BuiltinProfile* b = core::builtin_profile(profile.toStdString());
        setup.icc = b != nullptr ? builtin_bytes(*b) : read_profile_file(profile);
        if (!setup.icc && (v.on || v.gamut)) {
            // (カスタム…'s file gone or unreadable: not silently another proof — both off, said; the tiles as they were)
            ProofView& now = documents_[static_cast<std::size_t>(doc_)].proof;
            now.on = false;
            now.gamut = false;
            r.set_proof(false);
            r.set_gamut(std::nullopt);
            show_proof_marks();
            refresh_gamut_count();
            flash(QStringLiteral("校正のプロファイル「%1」が読めないため、色の校正と色域外警告を切りました（表示 → 校正の設定 → カスタム… で選び直せます）")
                      .arg(QFileInfo(profile).fileName()),
                  6000, true);
            canvas_->update();
            return;
        }
    }
    if (profile == QLatin1String("export")) setup.export_icc = cs::export_icc();
    r.set_proof_setup(setup);
    r.set_proof(v.on);
    r.set_gamut(v.gamut ? std::optional<GamutLook>(gamut_look()) : std::nullopt);
    r.set_monitor(display_color::current());
    show_proof_marks();
    refresh_gamut_count();
    canvas_->update();
}

void MainWindow::show_proof_marks() {
    if (documents_.empty()) return;
    const ProofView& v = proof_view();
    const auto quietly = [](QAction* a, bool on) {
        if (a == nullptr) return;
        const QSignalBlocker quiet(a);
        a->setChecked(on);
    };
    quietly(action(QStringLiteral("act_cmyk_proof")), v.on);
    quietly(action(QStringLiteral("act_gamut_warning")), v.gamut);
    // (the kind shown; a proof through a profile by where it comes from)
    QString id;
    if (v.kind != Kind::Profile) id = QString::fromStdString(render::proof::kind_id(v.kind, v.plate));
    else if (v.profile == QLatin1String("export")) id = QStringLiteral("export");
    else if (v.profile == QLatin1String("working")) id = QStringLiteral("workingCmyk");
    else id = QStringLiteral("custom");
    for (const ProofItem& item : proof_items()) quietly(action(QString::fromLatin1(item.attribute)), id == QLatin1String(item.id));
}

void MainWindow::refresh_gamut_count() {
    if (gamut_label_ == nullptr) return;
    const PageRenderer& r = canvas_->renderer();
    if (!r.gamut()) {
        gamut_label_->hide();
        return;
    }
    const auto [out, all] = r.gamut_count();
    const QLocale here;
    gamut_label_->setText(all == 0 ? QStringLiteral("色域外: 調べています…")
                                   : QStringLiteral("色域外 %1 画素（%2%）")
                                         .arg(here.toString(static_cast<qulonglong>(out)))
                                         .arg(static_cast<double>(out) * 100.0 / static_cast<double>(all), 0, 'f', 1));
    gamut_label_->show();
}

void MainWindow::proof_custom_dialog() {
    if (documents_.empty()) return;
    ProofView& v = documents_[static_cast<std::size_t>(doc_)].proof;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("proof_custom_dialog"));
    dialog.setWindowTitle(QStringLiteral("校正の設定"));
    auto* profile = new QComboBox;
    profile->setObjectName(QStringLiteral("proof_profile"));
    profile->addItem(QStringLiteral("作業用 CMYK（カラー設定）"), QStringLiteral("working"));
    profile->addItem(QStringLiteral("書き出しの CMYK（CMYK で見る）"), QStringLiteral("export"));
    for (const core::BuiltinProfile& b : core::builtin_profiles()) {
        if (b.space == core::ProfileSpace::Lab) continue;
        profile->addItem(QString::fromUtf8(b.description.data(), static_cast<qsizetype>(b.description.size())),
                         QString::fromUtf8(b.id.data(), static_cast<qsizetype>(b.id.size())));
    }
    if (profile->findData(v.profile) < 0) profile->addItem(QFileInfo(v.profile).fileName(), v.profile);
    profile->setCurrentIndex(std::max(0, profile->findData(v.profile)));
    auto* pick = new QPushButton(QStringLiteral("ファイルを選ぶ…"));
    pick->setObjectName(QStringLiteral("proof_pick"));
    connect(pick, &QPushButton::clicked, &dialog, [&dialog, profile] {
        const QString path = ask::open_path(&dialog, QStringLiteral("校正のプロファイル"), QStringLiteral("ICC プロファイル (*.icc *.icm)"));
        if (path.isEmpty()) return;
        const core::Bytes bytes = read_profile_file(path);
        const auto space = bytes ? render::ink::profile_space(*bytes) : std::nullopt;
        if (!space || *space == core::ProfileSpace::Lab) {
            ask::warning(&dialog, QStringLiteral("校正の設定"), QStringLiteral("このファイルは校正に使える ICC プロファイル（CMYK・RGB・グレー）ではありません。"));
            return;
        }
        if (profile->findData(path) < 0) profile->addItem(QFileInfo(path).fileName(), path);
        profile->setCurrentIndex(profile->findData(path));
    });
    auto* intent = new QComboBox;
    intent->setObjectName(QStringLiteral("proof_intent"));
    intent->addItem(QStringLiteral("相対的な色域を維持"), QStringLiteral("relative"));
    intent->addItem(QStringLiteral("知覚的"), QStringLiteral("perceptual"));
    intent->addItem(QStringLiteral("彩度"), QStringLiteral("saturation"));
    intent->addItem(QStringLiteral("絶対的な色域を維持"), QStringLiteral("absolute"));
    intent->setCurrentIndex(std::max(0, intent->findData(v.intent)));
    auto* bpc = new QCheckBox(QStringLiteral("黒点の補正"));
    bpc->setObjectName(QStringLiteral("proof_bpc"));
    bpc->setChecked(v.bpc);
    auto* paper = new QCheckBox(QStringLiteral("紙色をシミュレート"));
    paper->setObjectName(QStringLiteral("proof_paper"));
    paper->setChecked(v.simulate_paper);
    auto* form = new QFormLayout;
    auto* row = new QHBoxLayout;
    row->addWidget(profile, 1);
    row->addWidget(pick);
    form->addRow(QStringLiteral("プロファイル"), row);
    form->addRow(QStringLiteral("変換の意図"), intent);
    form->addRow(QString(), bpc);
    form->addRow(QString(), paper);
    auto* note = new QLabel(QStringLiteral("画面の見え方だけを変えます（原稿・書き出しは変わりません）。書き出しの CMYK では意図・黒点・紙色は Genko の変換のとおりです。"));
    note->setWordWrap(true);
    form->addRow(note);
    look::quiet_labels(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    look::frame(&dialog, look::header(QStringLiteral("校正の設定"), QStringLiteral("色の校正で見るプロファイル")), form, nullptr, look::footer(buttons));
    if (ask::exec(&dialog) != QDialog::Accepted || documents_.empty()) {
        show_proof_marks();  // (the radio as it was)
        return;
    }
    ProofView& now = documents_[static_cast<std::size_t>(doc_)].proof;
    now.profile = profile->currentData().toString();
    now.kind = Kind::Profile;
    now.intent = intent->currentData().toString();
    now.bpc = bpc->isChecked();
    now.simulate_paper = paper->isChecked();
    now.on = true;
    apply_proof_view();
}

void MainWindow::color_settings_dialog() {
    const Asked asked = asking();
    const bool writable = session_->read_only_reason().empty();
    cs::ColorSettingsDialog dialog(this, cs::load(), book().text_gamma, writable);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    cs::save(dialog.settings());
    // (the book's own: the gamma its letters are mixed in, an op undone as any — said to two places, as shown)
    const std::optional<double> was = asked.snapshot->text_gamma;
    const std::optional<double> wanted = dialog.text_gamma();
    const bool same = was.has_value() == wanted.has_value() && (!was || std::abs(*was - *wanted) < 0.005);
    if (writable && !same && still(asked)) {
        apply_ops(Json::array({Json{{"op", "set_text_gamma"}, {"gamma", wanted ? Json(std::round(*wanted * 100.0) / 100.0) : Json()}}}));
    }
    for (MainWindow* window : documents::windows()) window->apply_proof_view();  // (the working CMYK, the monitor: every window's)
    flash(QStringLiteral("カラー設定を変えました"), 3000);
}

// --- the policies ------------------------------------------------------------------------------------------------------

void MainWindow::color_policy_on_open(const std::shared_ptr<Session>& session) {
    if (!session || !session->read_only_reason().empty()) return;  // (a book opened read-only is never converted)
    if (session->loading()) {
        // (its later pages are read yet: asked once they are all there — after the read is done with, not inside the
        // change it tells: the saves it put off and the file watcher's look come first, then the question and its ops)
        auto once = std::make_shared<QMetaObject::Connection>();
        QPointer<MainWindow> self(this);
        std::weak_ptr<Session> weak = session;
        *once = connect(session.get(), &Session::changed, this, [self, weak, once](const BookChange&) {
            const auto s = weak.lock();
            if (!self || !s || s->loading()) return;
            QObject::disconnect(*once);
            QTimer::singleShot(0, self.data(), [self, weak] {
                if (const auto later = weak.lock(); self && later) self->color_policy_on_open(later);
            });
        });
        return;
    }
    const DocPtr doc = session->snapshot();
    const cs::Settings s = cs::load();
    struct Page {
        Json index;
        std::string intent;  // (its own way, kept by the conversion)
        bool bpc = true;
    };
    struct Group {
        cs::Space space;
        std::string ref;
        QString name;
        std::vector<Page> pages;
    };
    std::vector<Group> groups;
    std::map<int, cs::Working> works;
    for (const cs::Space space : {cs::Space::Rgb, cs::Space::Gray, cs::Space::Cmyk}) works.emplace(static_cast<int>(space), cs::working(space, s));
    for (const auto& page : doc->pages) {
        const Model m = model_of(page->color, page->rgb);
        if (!m.space) continue;  // (Lab: never)
        const cs::Working& w = works.at(static_cast<int>(*m.space));
        if (!w.usable || m.ref == cs::ref_of(w.icc)) continue;
        auto at = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.space == *m.space && g.ref == m.ref; });
        if (at == groups.end()) {
            const QString name = QString::fromStdString(core::profile_description(m.ref, m.icc));
            groups.push_back(Group{*m.space, m.ref, name.isEmpty() ? QStringLiteral("（名前のないプロファイル）") : name, {}});
            at = groups.end() - 1;
        }
        at->pages.push_back(Page{page->index.json(), m.intent, m.bpc});
    }
    if (groups.empty()) return;
    // (the book's default — the mode of the pages made after — goes with its pages of the same space and profile)
    const Model book = model_of(doc->color, doc->rgb);
    QString action = policy_of(s, groups.front().space);
    if (s.ask_mismatch) {
        QStringList lines;
        for (const Group& g : groups) {
            const cs::Working& w = works.at(static_cast<int>(g.space));
            lines << QStringLiteral("・%1 ページ: %2（作業用: %3）").arg(g.pages.size()).arg(g.name, w.name);
        }
        const QStringList choices{QStringLiteral("埋め込まれたプロファイルを使う（そのまま）"), QStringLiteral("作業用の空間に変換する"),
                                  QStringLiteral("プロファイルを外して作業用の空間とみなす")};
        const int now = action == QLatin1String("convert") ? 1 : action == QLatin1String("off") ? 2 : 0;
        const auto chosen = ask::get_item(this, QStringLiteral("プロファイルの不一致"),
                                          QStringLiteral("原稿「%1」には、作業用の空間と違うプロファイルのページがあります。\n%2\nどうしますか？（変換は取り消せます）")
                                              .arg(Document(session).title(), lines.join(QLatin1Char('\n'))),
                                          choices, now);
        if (!chosen) return;  // (closed without a choice: as it is)
        action = chosen == choices[1] ? QStringLiteral("convert") : chosen == choices[2] ? QStringLiteral("off") : QStringLiteral("preserve");
    }
    Json ops = Json::array();
    for (const Group& g : groups) {
        const QString what = s.ask_mismatch ? action : policy_of(s, g.space);
        if (what != QLatin1String("convert") && what != QLatin1String("off")) continue;
        const cs::Working& w = works.at(static_cast<int>(g.space));
        bool with_book = book.space == g.space && book.ref == g.ref;
        for (const Page& page : g.pages) {
            Json op{{"op", what == QLatin1String("convert") ? "convert_profile" : "assign_profile"}, {"page", page.index}};
            cs::put_profile(op, w);
            if (what == QLatin1String("convert")) {
                // (converted the settings' way; the page keeps its own: page_intent, page_bpc)
                op["intent"] = s.intent.toStdString();
                if (!s.bpc) op["bpc"] = false;
                if (page.intent != s.intent.toStdString()) op["page_intent"] = page.intent;
                if (page.bpc != s.bpc) op["page_bpc"] = page.bpc;
                if (s.dither) op["dither"] = true;
            }
            if (with_book && page.intent == book.intent && page.bpc == book.bpc) {  // (the book's own way kept as well)
                op["default"] = true;
                with_book = false;
            }
            ops.push_back(std::move(op));
        }
    }
    if (ops.empty()) return;
    try {
        if (session == session_) {
            if (!apply_ops(ops)) return;
            reload_pages();
        } else {
            session->apply(ops);
        }
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    flash(action == QLatin1String("off") || (!s.ask_mismatch && ops.front()["op"] == "assign_profile")
              ? QStringLiteral("作業用の空間のプロファイルにしました（元に戻せます）")
              : QStringLiteral("作業用の空間に変換しました（元に戻せます）"),
          5000);
}

bool MainWindow::picture_policy(const std::string& bytes, render::Image& picture) {
    const cs::Settings s = cs::load();
    const std::string_view mode = picture.mode();
    const cs::Space space = mode == "CMYK" ? cs::Space::Cmyk
                            : (mode == "L" || mode == "LA" || mode == "1" || mode.starts_with("I")) ? cs::Space::Gray
                                                                                                     : cs::Space::Rgb;
    const core::ProfileSpace wanted = space == cs::Space::Rgb ? core::ProfileSpace::Rgb : space == cs::Space::Gray ? core::ProfileSpace::Gray : core::ProfileSpace::Cmyk;
    const QString policy = policy_of(s, space);
    std::optional<std::string> embedded = render::proof::embedded_profile(bytes);
    if (embedded && render::ink::profile_space(*embedded) != wanted) embedded.reset();  // (not one for its colours: as none)
    core::Bytes from;
    if (embedded) {
        // (a profile that shows its numbers as Genko does — sRGB, or a grey whose greys are sRGB's — changes nothing)
        if (render::proof::same_as_srgb(*embedded)) return true;
        bool convert = policy == QLatin1String("convert");
        if (s.ask_paste) {
            const QString name = QString::fromStdString(core::icc_description(*embedded));
            const QStringList choices{QStringLiteral("変換しない（値のまま。いままでどおり）"), QStringLiteral("sRGB に変換する（見た目を保つ）")};
            const auto chosen = ask::get_item(this, QStringLiteral("埋め込まれたプロファイル"),
                                              QStringLiteral("この画像にはプロファイル「%1」が埋め込まれています。原稿には sRGB の色として置きます。")
                                                  .arg(name.isEmpty() ? QStringLiteral("（名前なし）") : name),
                                              choices, convert ? 1 : 0);
            if (chosen) convert = chosen == choices[1];  // (closed without a choice: the policy's)
        }
        if (!convert) return true;
        from = std::make_shared<const std::string>(std::move(*embedded));
    } else {
        // (no profile: taken as the working space's when that is not the default one, as upstream assigns it)
        const cs::Working w = cs::working(space, s);
        if (w.is_default || !w.usable || !w.icc || policy == QLatin1String("off")) return true;
        bool convert = true;
        if (s.ask_missing) {
            const QStringList choices{QStringLiteral("そのまま（sRGB として置く）"), QStringLiteral("作業用の空間（%1）とみなして変換する").arg(w.name)};
            const auto chosen = ask::get_item(this, QStringLiteral("プロファイルのない画像"),
                                              QStringLiteral("この画像にはプロファイルがありません。"), choices, 1);
            if (chosen) convert = chosen == choices[1];
        }
        if (!convert) return true;
        from = w.icc;
    }
    try {
        picture = render::proof::convert_picture(picture, *from, s.intent.toStdString(), s.bpc);
    } catch (const std::exception& error) {
        flash(QStringLiteral("画像のプロファイルで変換できませんでした（値のまま置きます）: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
    return true;
}

// --- ウィンドウ → 開いている原稿 ------------------------------------------------------------------------------------------

void MainWindow::fill_books_menu() {
    if (books_menu_ == nullptr) return;
    books_menu_->clear();
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        const int index = static_cast<int>(i);
        QAction* item = books_menu_->addAction(QStringLiteral("%1  %2").arg(index + 1).arg(documents_[i].title()), this, [this, index] {
            if (index < static_cast<int>(documents_.size()) && index != doc_) switch_document(index);
        });
        item->setCheckable(true);
        item->setChecked(index == doc_);
        const std::optional<std::filesystem::path> path = documents_[i].session ? documents_[i].session->path() : std::nullopt;
        item->setToolTip(path ? QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(*path))) : QStringLiteral("未保存"));
    }
}

}  // namespace genko::app
