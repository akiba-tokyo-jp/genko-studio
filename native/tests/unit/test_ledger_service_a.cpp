// Checks of the Python tests that no other C++ test makes (M7 ledger, service group A), each slot named after the
// Python test it ports: tests/test_c_extras.py (an aside, a character's tag, frames measured wider), test_b_lettering.py
// (one speaker's lines run together), test_effect_clearing.py (the finish keeps an effect put before clear of the
// reported faces), test_ai_feedback.py (references most needed first, take_panel_art's whole chain),
// test_battle_report.py (flash words, no white over an adopted picture, one sheet of references), test_chat_approval.py
// (the person who last approved, a chat name with a space), test_l1_style.py (links and ids, the requests that carry a
// kept style, a black and white style on a colour book, a shared page URL, the style's path from the old catalog),
// test_m11.py (an unknown face key), test_m1_report.py (a jacket and a back cover in the e-book, the assignee in status,
// the page role on one panel, import_psd from inside the book, Kindle's own long edge, how much fits, the name preview's
// bands) and test_m1_security.py (releasing a lock not held, every handled op in the catalogue).
// Not here: test_m1_security.py test_apply_cost_does_not_grow_with_undo_depth (a timing bound: the perf tests').
//
// DEPS genko_test_support genko_service
// (Several slots run the studio's whole flow: four names, Genko's enlargement, a Kindle export. TIMEOUT 900 is suggested.)

#include <QtTest>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "core/stylecat.hpp"
#include "formats/export.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/text/fonts.hpp"
#include "render/zip.hpp"
#include "service/blocking.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/finish.hpp"
#include "service/fxwords.hpp"
#include "service/genreq.hpp"
#include "service/human_service.hpp"
#include "service/layout.hpp"
#include "service/letter.hpp"
#include "service/preview.hpp"
#include "service/result.hpp"
#include "service/studio_service.hpp"
#include "service/stylecat.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
using genko::core::PageSpec;
using genko::service::StudioService;
using genko::service::ToolResult;
namespace fs = std::filesystem;
namespace letter = genko::service::letter;
namespace genreq = genko::service::genreq;
namespace stylecat = genko::service::stylecat;

namespace {

// What a tool answered, for a failure's message.
std::string shown(const ToolResult& result) { return result.to_dict().dump(); }

std::set<std::string> codes(const ToolResult& result) {
    std::set<std::string> out;
    for (const auto& issue : result.issues) out.insert(issue.code);
    return out;
}

bool has(const Json& text, const std::string& words) { return text.is_string() && text.get<std::string>().find(words) != std::string::npos; }

bool starts(const Json& text, const std::string& words) { return text.is_string() && text.get<std::string>().starts_with(words); }

// tests/fixtures/studio/demo4/<name>
Json fixture(const std::string& name) {
    return genko::test::read_json(genko::test::repo_root() + QStringLiteral("/tests/fixtures/studio/demo4/") + QString::fromStdString(name));
}

// While it lives, the environment variable `name` is `value` (and then as it was).
class EnvVar {
public:
    EnvVar(const char* name, const QByteArray& value) : name_(name), had_(qEnvironmentVariableIsSet(name)), before_(qgetenv(name)) {
        qputenv(name_, value);
    }
    ~EnvVar() {
        if (had_) {
            qputenv(name_, before_);
        } else {
            qunsetenv(name_);
        }
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;

private:
    const char* name_;
    bool had_;
    QByteArray before_;
};

// demo4's book as Python's tests make it: <root>/demo.genko (4 pages, `spec`) with its bible and script and the names of
// its first `named` pages saved by `agent` (test_chat_approval._book, test_l1_style._named); "" when every step went
// through, else what stopped it.
std::string demo_book(StudioService& agent, int named, const std::string& spec = "commercial-b4") {
    const auto step = [](const std::string& what, const ToolResult& r) { return r.ok ? std::string() : what + ": " + shown(r); };
    std::string failed = step("create_project", agent.call("create_project", Json::array({"demo.genko", "demo", 4, spec}), Json::object()));
    if (failed.empty()) failed = step("set_bible", agent.call("set_bible", Json::array({"demo.genko", fixture("bible.json")}), Json::object({{"commit", true}})));
    if (failed.empty()) failed = step("set_script", agent.call("set_script", Json::array({"demo.genko", fixture("script.json")}), Json::object({{"commit", true}})));
    for (int n = 1; n <= named && failed.empty(); ++n) {
        const std::string name = "p00" + std::to_string(n) + ".json";
        failed = step("submit_name " + name, agent.call("submit_name", Json::array({"demo.genko", fixture(name)}), Json::object({{"commit", true}})));
    }
    return failed;
}

// demo_book with the names of its first `pages` pages approved by human:leaf (test_ai_feedback._approved: 1 page;
// test_m4_units._approved: 4).
std::string approved_demo(StudioService& agent, int pages) {
    const std::string made = demo_book(agent, pages);
    if (!made.empty()) return made;
    Json numbers = Json::array();
    for (int n = 1; n <= pages; ++n) numbers.push_back(n);
    const Json approved = genko::service::HumanService(agent.root() / "demo.genko", "human:leaf").approve_name(numbers);
    return approved.value("ok", false) ? std::string() : "approve_name: " + approved.dump();
}

// StudioService._ops(project, ops) as `actor`: the ops applied and saved under the lock; "" when they went through, else
// the refusal.
std::string ops_as(const fs::path& book, const Json& ops, const std::string& actor) {
    try {
        genko::service::Session session(book, actor);
        session.apply(ops, actor);
        session.save(actor);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::string first_leaf(const fs::path& book) {
    const Document doc = genko::service::load_episode(book);
    return doc.page(0).leaf_frames().at(0)->id;
}

// The panel brief of page 1's panel `frame` as the book holds it now ({} without one).
Json panel_of(const fs::path& book, const std::string& frame) {
    const Document doc = genko::service::load_episode(book);
    const genko::core::Frame* found = doc.page(0).find_frame(frame);
    return found != nullptr && found->panel ? *found->panel : Json::object();
}

// tests/agents/images.panel(px, figures): a grey ground in bands, a grey body and a white head (outlined) where the
// request's pose guide puts each figure, as an RGB PNG of the request's size.
std::string panel_png(const Json& px, const Json& figures) {
    const int w = static_cast<int>(px.at(std::size_t{0}).get<std::int64_t>());
    const int h = static_cast<int>(px.at(std::size_t{1}).get<std::int64_t>());
    genko::render::Image image = genko::render::Image::create("L", genko::render::Size{w, h}, genko::render::Ink(230));
    {
        genko::render::Draw draw(image);
        for (int y = 0; y < h; y += 8) {
            draw.rectangle(genko::render::BoxF{0.0, double(y), double(w), double(y + 8)}, genko::render::Ink(150 + 80 * y / h));
        }
        for (const Json& fig : figures) {
            const auto at = [&fig](const char* key, int i) { return fig.at(key).at(static_cast<std::size_t>(i)).get<double>(); };
            draw.rectangle(genko::render::BoxF{at("body01", 0) * w, at("body01", 1) * h, (at("body01", 0) + at("body01", 2)) * w,
                                               (at("body01", 1) + at("body01", 3)) * h},
                           genko::render::Ink(90));
            draw.ellipse(genko::render::BoxF{at("head01", 0) * w, at("head01", 1) * h, (at("head01", 0) + at("head01", 2)) * w,
                                             (at("head01", 1) + at("head01", 3)) * h},
                         genko::render::Ink(250), genko::render::Ink(0), std::max(2, w / 200));
        }
    }
    return genko::formats::png_bytes(image.convert("RGB"));
}

// [{"kind": "face", "char", "box01": head01} for each figure of the request]
Json face_regions(const Json& request) {
    Json regions = Json::array();
    for (const Json& f : request.value("figures", Json::array())) {
        regions.push_back(Json{{"kind", "face"}, {"char", f.value("char", Json())}, {"box01", f.value("head01", Json())}});
    }
    return regions;
}

const Json kOrigin = Json{{"kind", "agent"}, {"tool_id", "xai:grok-imagine"}, {"model", "grok-imagine"}};

struct PanelArt {
    std::string error;  // what stopped it before take_panel_art ("" when nothing did)
    std::string frame;
    Json request;
    Json regions = Json::array();
    ToolResult done;
};

// test_battle_report._panel_with_art (with `regions`: test_ai_feedback's call, the faces of the request's figures):
// page 1's first panel asked for, a picture of the request's size written into its inbox and taken with take_panel_art.
PanelArt panel_with_art(StudioService& agent, bool regions = false) {
    PanelArt art;
    art.error = approved_demo(agent, 1);
    if (!art.error.empty()) return art;
    const fs::path book = agent.root() / "demo.genko";
    art.frame = first_leaf(book);
    ToolResult asked = agent.call("generation_request", Json::array({"demo.genko"}), Json{{"page", 1}, {"frame_id", art.frame}});
    if (!asked.ok) {
        art.error = "generation_request: " + shown(asked);
        return art;
    }
    art.request = asked.data["request"];
    const std::string id = art.request.at("id").get<std::string>();
    genko::storage::write_atomic(book / "studio" / "inbox" / id / "a.png",
                                 panel_png(art.request.at("size").at("suggested_px"), art.request.value("figures", Json::array())));
    Json kwargs = Json::object();
    if (regions) {
        art.regions = face_regions(art.request);
        kwargs["regions"] = art.regions;
    }
    art.done = agent.call("take_panel_art", Json::array({"demo.genko", id, Json{{"file", "studio/inbox/" + id + "/a.png"}, {"origin", kOrigin}}}),
                          kwargs);
    return art;
}

// test_c_extras._plan / _placed: one panel with a speaker tagged 司会 サワっち and three lines (a fancy box, an aside, a
// rounded balloon), its layout applied to a new B5 book and its balloons placed.
struct Placed {
    genko::service::Work work;
    genko::service::layout::CompiledLayout compiled;
    std::vector<letter::BalloonPlacement> placements;
    std::vector<genko::studio::Issue> issues;
};

Placed placed_c_extras() {
    const Json plan = Json::parse(R"({"page": 1, "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": null}]}],
        "panels": [{"slot": "p1", "shot": "MS", "characters": [{"id": "a", "pos": "right", "tag": "司会 サワっち"}],
                    "lines": [{"beat_id": "b1", "balloon": "fancy_box", "breaks": ["今月の", "テーマ"]},
                              {"beat_id": "b2", "balloon": "aside", "breaks": ["あぁあ…", "やってもた"]},
                              {"beat_id": "b3", "balloon": "rounded", "breaks": ["つまり", "こういうこと"]}]}]})");
    Placed out{genko::service::Work{genko::core::new_episode("t", Num(1), 1, PageSpec::b5_doujin()), Json::array()}, {}, {}, {}};
    out.compiled = genko::service::layout::apply_layout(out.work, Json(1), plan, "ai:test");
    auto [placements, issues] = letter::place_page(plan, out.compiled, Json::parse(R"({"characters": [{"id": "a", "name": "A"}]})"),
                                                   Json{{"b1", "a"}, {"b2", "a"}, {"b3", "a"}});
    out.placements = std::move(placements);
    out.issues = std::move(issues);
    return out;
}

// test_m1_report._book(pages): a new B5 book whose names are approved.
Document named_book(int pages) {
    Document doc = genko::core::new_episode("t", Num(1), pages, PageSpec::b5_doujin());
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).name_ok = true;
    return doc;
}

// A picture of one colour as PNG.
std::string colour_png(int r, int g, int b, int width, int height) {
    return genko::formats::png_bytes(
        genko::render::Image::create("RGB", genko::render::Size{width, height}, genko::render::Ink{r, g, b}));
}

// A book in memory (its pictures in the AssetStore of the folder `project`) whose page 1 is cut across into two panels
// at the park: the first with its picture adopted, the second asking for ひな (her face and her sheet), a cup (a prop with
// a picture) in the book's catalog style (its sample). `only_face`: the book has ひな's face and nothing else. The face is
// red (100×160), the sheet green (300×200) and the style's sample blue (100×160), as Python's _reference_sheet test has
// them.
struct RefsBook {
    Document doc;
    std::string frame;
    std::string prop;
    std::string location;
};

RefsBook refs_book(const fs::path& project, bool only_face) {
    fs::create_directories(project);
    genko::storage::AssetStore store(project);
    RefsBook out;
    const std::string face = store.put_bytes(colour_png(200, 0, 0, 100, 160), ".png");
    const std::string sheet = store.put_bytes(colour_png(0, 200, 0, 300, 200), ".png");
    const std::string style = store.put_bytes(colour_png(0, 0, 200, 100, 160), ".png");
    const std::string previous = store.put_bytes(colour_png(90, 90, 90, 120, 80), ".png");
    out.prop = store.put_bytes(colour_png(200, 200, 0, 60, 60), ".png");
    out.location = store.put_bytes(colour_png(0, 200, 200, 160, 100), ".png");
    Document doc = genko::core::new_episode("t", Num(1), 1, PageSpec::b5_doujin());
    doc = genko::service::apply_ops(doc,
                                    Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", doc.page(0).frames.at(0).id},
                                                      {"axis", "horizontal"}, {"ratio", 0.5}}}),
                                    "genko")
              .doc;
    const auto leaves = doc.page(0).leaf_frames();
    const std::string before = leaves.at(0)->id;
    out.frame = leaves.at(1)->id;
    genko::core::Page& page = doc.edit_page(0);
    Json first = Json{{"slot", "p1"}, {"location_id", "park"}};
    if (!only_face) {
        first["adopted"] = Json::object({{"art", "cd_prev"}});
        first["candidates"] = Json::array({Json{{"id", "cd_prev"}, {"asset", previous}}});
    }
    page.find_frame(before)->panel = first;
    page.find_frame(out.frame)->panel = Json{{"slot", "p2"},
                                             {"shot", "MS"},
                                             {"location_id", "park"},
                                             {"characters", Json::array({Json{{"id", "hina"}, {"pos", "center"}}})},
                                             {"props", Json::array({"cup"})}};
    Json refs = Json::array({Json{{"kind", "face"}, {"asset", face}}});
    if (!only_face) refs.push_back(Json{{"kind", "sheet"}, {"asset", sheet}});
    doc.bible.characters = Json::array({Json{{"id", "hina"}, {"name", "ひな"}, {"refs", refs}}});
    if (!only_face) {
        doc.studio["locations"] = Json::array({Json{{"id", "park"}, {"name", "公園"}, {"refs", Json::array({Json::object({{"asset", out.location}})})}}});
        doc.studio["props"] = Json::array({Json{{"id", "cup"}, {"name", "マグ"}, {"refs", Json::array({Json::object({{"asset", out.prop}})})}}});
        doc.studio["style"] = Json::object(
            {{"catalog", Json{{"id", "shonen"},
                              {"title", "少年漫画"},
                              {"path", Json::array({"少年漫画"})},
                              {"expression", "mono"},
                              {"prompt", Json{{"ja", "白黒のマンガのコマ。"}, {"en", "A black-and-white manga panel."}, {"tags", "manga"}}},
                              {"avoid", Json::array()},
                              {"sample", style},
                              {"version", 3}}}});
    }
    out.doc = std::move(doc);
    return out;
}

// The words of test_l1_style's catalog (its prompt in Japanese).
const std::string kWords =
    "モノクロの日本のマンガのコマ。灰色はすべて網点で表す。 バトル漫画の絵柄。荒々しく太い線。 仕上げの絵柄（上と違う所はこちらに従う）: 2〜3頭身のデフォルメ。";

// test_l1_style's catalog (少年漫画 › バトル › 低頭身のデフォルメ, each version 3) in a folder, read through
// GENKO_STYLE_CATALOG=file://localhost/… while it lives (Python fakes stylecat._get); its samples are grey PNGs there
// (Python's are JPEGs). Its pages' URLs are https://localhost/n/<id> (the catalog's own host).
struct StyleCatalog {
    fs::path root;
    std::string base;
    EnvVar env;

    static std::string file_url(const fs::path& folder) {
        std::string path = genko::core::path_to_utf8(folder);
        std::replace(path.begin(), path.end(), '\\', '/');
        return "file://localhost" + (path.starts_with("/") ? path : "/" + path);
    }

    explicit StyleCatalog(const fs::path& folder)
        : root(folder / "catalog"), base(file_url(folder / "catalog")), env("GENKO_STYLE_CATALOG", QByteArray::fromStdString(base)) {
        const Json tree = Json{{"version", 1},
                               {"nodes", Json::array({Json{{"id", "shonen"}, {"parent", nullptr}, {"level", 1}, {"title", "少年漫画"}, {"summary", "熱い"},
                                                           {"thumbnail_url", base + "/t/shonen.png"}},
                                                      Json{{"id", "shonen-battle"}, {"parent", "shonen"}, {"level", 2}, {"title", "バトル"},
                                                           {"summary", "戦い"}, {"thumbnail_url", nullptr}},
                                                      Json{{"id", "shonen-battle-chibi"}, {"parent", "shonen-battle"}, {"level", 3},
                                                           {"title", "低頭身のデフォルメ"}, {"summary", "2〜3頭身"}, {"thumbnail_url", nullptr}}})}};
        genko::storage::write_atomic(root / "v1" / "tree", tree.dump());
        for (const char* id : {"shonen", "shonen-battle", "shonen-battle-chibi"}) {
            write_style(id, 3);
            genko::storage::write_atomic(root / "files" / id / "upper_body-v1.png",
                                         genko::formats::png_bytes(genko::render::Image::create("L", genko::render::Size{30, 40}, genko::render::Ink(128))));
        }
    }

    // test_l1_style._branch
    Json branch(const std::string& sid, const std::string& title, int level, const Json& parent_path, const Json& children, int version) const {
        Json path = parent_path;
        path.push_back(Json{{"id", sid}, {"title", title}});
        return Json{{"id", sid},
                    {"title", title},
                    {"summary", title + "の絵柄。"},
                    {"level", level},
                    {"expression", "mono"},
                    {"path", path},
                    {"prompt", Json{{"ja", kWords}, {"en", "Black-and-white manga panel. Finished style: chibi."}, {"tags", "manga, chibi"}}},
                    {"avoid", Json::array({"文字", "フキダシ", "効果音の描き文字", "コマ枠", "署名", "透かし", "色"})},
                    {"samples", Json::array({Json{{"subject", "upper_body"}, {"url", base + "/files/" + sid + "/upper_body-v1.png"}}})},
                    {"reference", 0},
                    {"license", Json{{"generated", true}, {"artist_names", false}, {"terms", "見本の絵はすべて生成 AI"}}},
                    {"version", version},
                    {"updated_at", "2026-09-27T11:18:12Z"},
                    {"children", children}};
    }

    // The catalog's reply for the style `id` (CATALOG[id] with this version).
    void write_style(const std::string& id, int version) const {
        const Json shonen = Json::array({Json{{"id", "shonen"}, {"title", "少年漫画"}}});
        const Json battle = Json::array({Json{{"id", "shonen"}, {"title", "少年漫画"}}, Json{{"id", "shonen-battle"}, {"title", "バトル"}}});
        Json data;
        if (id == "shonen") {
            data = branch(id, "少年漫画", 1, Json::array(), Json::array({Json{{"id", "shonen-battle"}, {"title", "バトル"}, {"summary", "戦い"}}}), version);
        } else if (id == "shonen-battle") {
            data = branch(id, "バトル", 2, shonen,
                          Json::array({Json{{"id", "shonen-battle-chibi"}, {"title", "低頭身のデフォルメ"}, {"summary", "2〜3頭身"}}}), version);
        } else {
            data = branch(id, "低頭身のデフォルメ", 3, battle, Json::array(), version);
        }
        genko::storage::write_atomic(root / "v1" / "styles" / id, data.dump());
    }
};

// An 8-bit RGB PSD of width × height with these layers (bottom first), each a box of one colour with its own
// transparency channel, its name in Unicode (luni) with an ASCII one beside it; no merged picture (test_gui_book's
// writer: genko.psd.read_psd reads it).
struct PsdLayer {
    QString name;
    std::string ascii;
    std::uint32_t top, left, width, height;
    std::array<unsigned char, 4> rgba;
};

std::string psd_bytes(std::uint32_t width, std::uint32_t height, const std::vector<PsdLayer>& layers) {
    const auto u16 = [](std::string& out, unsigned v) { out += {static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)}; };
    const auto u32 = [](std::string& out, std::uint32_t v) {
        out += {static_cast<char>(v >> 24 & 0xFF), static_cast<char>(v >> 16 & 0xFF), static_cast<char>(v >> 8 & 0xFF),
                static_cast<char>(v & 0xFF)};
    };
    std::string records, data;
    for (const PsdLayer& l : layers) {
        u32(records, l.top);
        u32(records, l.left);
        u32(records, l.top + l.height);
        u32(records, l.left + l.width);
        u16(records, 4);
        for (const int channel : {-1, 0, 1, 2}) {
            u16(records, static_cast<unsigned>(channel) & 0xFFFF);
            u32(records, 2 + l.width * l.height);
        }
        records += "8BIMnorm";
        records += {static_cast<char>(255), '\0', '\0', '\0'};
        std::string pascal(1, static_cast<char>(l.ascii.size()));
        pascal += l.ascii;
        while (pascal.size() % 4 != 0) pascal += '\0';
        std::string luni;
        u32(luni, static_cast<std::uint32_t>(l.name.size()));
        for (const QChar c : l.name) u16(luni, c.unicode());
        while (luni.size() % 4 != 0) luni += '\0';
        std::string extra;
        u32(extra, 0);  // no mask
        u32(extra, 0);  // no blending ranges
        extra += pascal;
        extra += "8BIMluni";
        u32(extra, static_cast<std::uint32_t>(luni.size()));
        extra += luni;
        u32(records, static_cast<std::uint32_t>(extra.size()));
        records += extra;
        for (const unsigned char v : {l.rgba[3], l.rgba[0], l.rgba[1], l.rgba[2]}) {
            u16(data, 0);
            data.append(static_cast<std::size_t>(l.width) * l.height, static_cast<char>(v));
        }
    }
    std::string info;
    u16(info, static_cast<unsigned>(layers.size()));
    info += records;
    info += data;
    if (info.size() % 2 != 0) info += '\0';
    std::string out = "8BPS";
    u16(out, 1);
    out.append(6, '\0');
    u16(out, 3);
    u32(out, height);
    u32(out, width);
    u16(out, 8);
    u16(out, 3);
    u32(out, 0);
    u32(out, 0);
    u32(out, static_cast<std::uint32_t>(4 + info.size() + 4));
    u32(out, static_cast<std::uint32_t>(info.size()));
    out += info;
    u32(out, 0);
    return out;
}

}  // namespace

class TestLedgerServiceA : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = genko::core::path_from_utf8((dir_.path() + QLatin1Char('/') + name).toStdString());
        fs::create_directories(out);
        return out;
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // (Python's fixtures: the config in the test's own folder, the person leaf)
        qputenv("GENKO_CONFIG_DIR", (dir_.path() + QStringLiteral("/config")).toUtf8());
        qputenv("GENKO_USER", "leaf");
    }

    // --- test_c_extras.py ---------------------------------------------------------------------------------------------

    // An aside (呟き) is placed as bare hand lettering: no balloon, no tail, the hand face, leaning 6 degrees.
    void test_an_aside_is_bare_hand_lettering_that_leans() {
        Placed placed = placed_c_extras();
        for (const auto& issue : placed.issues) QVERIFY2(issue.severity != "error", issue.to_json().dump().c_str());
        const auto aside = std::find_if(placed.placements.begin(), placed.placements.end(),
                                        [](const letter::BalloonPlacement& p) { return p.text.starts_with("あぁあ"); });
        QVERIFY(aside != placed.placements.end());
        QCOMPARE(aside->balloon, std::string("none"));
        QVERIFY(!aside->tail);
        Json style = aside->style.value_or(Json::object());
        QCOMPARE(style.value("font", Json()), Json("hand"));
        QVERIFY(style.value("rotate_deg", Json()).is_number());
        QCOMPARE(std::abs(style["rotate_deg"].get<double>()), 6.0);
    }

    // A character's tag (名札) is set across in a small rounded box by its character; a decorated frame has no tail; the
    // tag's line is horizontal in the book once the placements are applied.
    void test_a_tag_is_set_across_by_its_character_and_frames_have_no_tail() {
        Placed placed = placed_c_extras();
        const auto tag = std::find_if(placed.placements.begin(), placed.placements.end(),
                                      [](const letter::BalloonPlacement& p) { return p.text == "司会 サワっち"; });
        QVERIFY(tag != placed.placements.end());
        QCOMPARE(tag->balloon, std::string("rounded"));
        QCOMPARE(tag->wrap, std::string("horizontal"));
        QVERIFY(tag->w_mm > tag->h_mm);
        const auto frame = std::find_if(placed.placements.begin(), placed.placements.end(),
                                        [](const letter::BalloonPlacement& p) { return p.balloon == "fancy_box"; });
        QVERIFY(frame != placed.placements.end());
        QVERIFY(!frame->tail);
        placed.work.apply(letter::placements_to_ops(Json(1), placed.placements, placed.compiled), "genko");
        QVERIFY(std::any_of(placed.work.doc.story.begin(), placed.work.doc.story.end(),
                            [](const genko::core::StoryLine& line) { return line.wrap == "horizontal"; }));
    }

    // A fancy box keeps its words further in than a plain box (letter.measure); box, dotted box, tone box and fancy box
    // each draw differently.
    void test_frames_draw_differently_and_keep_their_words_further_in() {
        QVERIFY(letter::measure({"テーマ"}, "fancy_box")[0] > letter::measure({"テーマ"}, "box")[0]);
        std::set<std::string> looks;
        for (const char* kind : {"box", "dotted_box", "tone_box", "fancy_box"}) {
            Document doc = genko::core::new_episode("t", Num(1), 1, PageSpec::b5_doujin());
            doc = genko::service::apply_ops(doc,
                                            Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "見出し"}, {"balloon", kind}, {"wrap", "vertical"},
                                                              {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 20}, {"h_mm", 30}}}),
                                            "genko")
                      .doc;
            genko::render::RenderOptions options;
            options.mode = "print";
            const genko::render::Image page = genko::render::render_page(doc.page(0), 100, options, &doc).image;
            looks.insert(page.convert("L").crop(genko::render::Box{200, 200, 360, 380}).tobytes());
        }
        QCOMPARE(looks.size(), std::size_t{4});
    }

    // --- test_b_lettering.py ------------------------------------------------------------------------------------------

    // One speaker's two lines in a row run together: one style group, no tail on the second, the second to the left of
    // the first and overlapping it.
    void test_one_speakers_lines_in_a_row_run_together() {
        genko::service::Work work{genko::core::new_episode("t", Num(1), 1, PageSpec::b5_doujin()), Json::array()};
        const Json plan = Json::parse(R"({"page": 1, "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": null}]}],
            "panels": [{"slot": "p1", "shot": "MS", "characters": [{"id": "a", "pos": "left"}, {"id": "b", "pos": "right"}],
                        "lines": [{"beat_id": "b1", "balloon": "speech", "breaks": ["ねえ", "聞いてる？"]},
                                  {"beat_id": "b2", "balloon": "speech", "breaks": ["ちょっと", "待ってよ"]}]}]})");
        const auto compiled = genko::service::layout::apply_layout(work, Json(1), plan, "ai:test");
        [[maybe_unused]] auto [placements, issues] =
            letter::place_page(plan, compiled, Json::parse(R"({"characters": [{"id": "a", "name": "A"}, {"id": "b", "name": "B"}]})"),
                               Json{{"b1", "a"}, {"b2", "a"}});
        QCOMPARE(placements.size(), std::size_t{2});
        const letter::BalloonPlacement& first = placements[0];
        const letter::BalloonPlacement& second = placements[1];
        QVERIFY(first.style && !first.style->empty() && second.style && !second.style->empty());
        QVERIFY(first.style->contains("group"));
        QCOMPARE(second.style->value("group", Json()), first.style->value("group", Json()));
        QVERIFY(!second.tail);
        QVERIFY(second.x_mm < first.x_mm);
        QVERIFY(second.x_mm + second.w_mm > first.x_mm);  // (to its left, the two overlapping)
    }

    // --- test_effect_clearing.py --------------------------------------------------------------------------------------

    // The finish keeps every effect clear of the faces reported in the adopted art: an effect put earlier is given
    // them (edit_effect params.avoid), a new focus effect from the panel's fx too, and the notes say so.
    void test_the_finish_keeps_every_effect_clear_of_the_reported_faces() {
        StudioService agent(folder(QStringLiteral("clear-faces")), "ai:test");
        PanelArt art = panel_with_art(agent);
        QVERIFY2(art.error.empty(), art.error.c_str());
        QVERIFY2(art.done.ok, shown(art.done).c_str());
        const fs::path book = agent.root() / "demo.genko";
        const ToolResult reported = agent.call("report_regions", Json::array({"demo.genko", 1, art.frame, face_regions(art.request)}), Json::object());
        QVERIFY2(reported.ok, shown(reported).c_str());
        // an effect put before: it is given the faces to keep clear of
        std::string refused = ops_as(book,
                                     Json::array({Json{{"op", "add_effect"}, {"page", 1}, {"kind", "speed"}, {"frame_id", art.frame}, {"id", "old"},
                                                       {"params", Json::object()}}}),
                                     agent.actor());
        QVERIFY2(refused.empty(), refused.c_str());
        refused = ops_as(book,
                         Json::array({Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", art.frame},
                                           {"set", Json::object({{"fx", Json::array({"集中線"})}})}}}),
                         agent.actor());
        QVERIFY2(refused.empty(), refused.c_str());
        const Document episode = genko::service::load_episode(book);
        auto [ops, notes] = genko::service::finish::plan(episode, episode.page(0));
        Json edit, focus;
        for (const Json& op : ops) {
            if (edit.is_null() && op.value("op", Json()) == Json("edit_effect") && op.value("id", Json()) == Json("old")) edit = op;
            if (focus.is_null() && op.value("op", Json()) == Json("add_effect") && op.value("kind", Json()) == Json("focus")) focus = op;
        }
        QVERIFY2(!edit.is_null(), ops.dump().c_str());
        QVERIFY2(!focus.is_null(), ops.dump().c_str());
        const Json edit_avoid = edit.value("params", Json::object()).value("avoid", Json());
        const Json focus_avoid = focus.value("params", Json::object()).value("avoid", Json());
        QVERIFY2(edit_avoid.is_array() && !edit_avoid.empty(), edit.dump().c_str());
        QVERIFY2(focus_avoid.is_array() && !focus_avoid.empty(), focus.dump().c_str());
        bool cleared = false;
        bool said = false;
        for (const Json& n : notes) {
            cleared = cleared || n.value("kind", Json()) == Json("clear_faces");
            said = said || has(n.value("why", Json()), "手前で線を止めた");
        }
        QVERIFY2(cleared && said, notes.dump().c_str());
    }

    // --- test_ai_feedback.py ------------------------------------------------------------------------------------------

    // References come most needed first: a character's face, her sheet, the book's style sample, the panel before,
    // then a prop, then the place. (Python checks genreq._by_priority on names; this build's by_priority is internal to
    // service/genreq.cpp, so the order is checked in the request it orders: request.files.references.)
    void test_references_come_most_needed_first() {
        const fs::path project = folder(QStringLiteral("refs-order")) / "r.genko";
        const RefsBook book = refs_book(project, false);
        genreq::Options options;
        options.page = 1;
        options.frame_id = book.frame;
        const genreq::Pack pack = genreq::build(book.doc, project, options);
        Json request = pack.request;
        const Json want = Json::array({"refs/hina_face.png", "refs/hina_sheet.png", "refs/style_catalog.png", "refs/previous_panel.png",
                                       "refs/prop_cup_" + book.prop.substr(7, 8) + ".png", "refs/park_" + book.location.substr(7, 8) + ".png"});
        QCOMPARE(request["files"]["references"], want);
    }

    // One call takes a panel's picture from the request to adopted: import, adopt, Genko's enlargement (a ~1 MP picture
    // is short of 600 dpi for a whole panel) adopted in its place, then the reported faces; a wrong file stops at the
    // import; a character sheet's request is not for this tool.
    void test_one_call_takes_a_panels_picture_from_request_to_adopted() {
        StudioService agent(folder(QStringLiteral("take-art")), "ai:test");
        PanelArt art = panel_with_art(agent, true);
        QVERIFY2(art.error.empty(), art.error.c_str());
        ToolResult& done = art.done;
        QVERIFY2(done.ok, shown(done).c_str());
        std::vector<std::string> steps;
        for (const Json& s : done.data["steps"]) steps.push_back(s.value("step", std::string()));
        QVERIFY2(steps.size() >= 3 && steps[0] == "import" && steps[1] == "adopt" && steps[2] == "upscale", done.data["steps"].dump().c_str());
        QVERIFY2(art.regions.empty() || steps.back() == "report_regions", done.data["steps"].dump().c_str());
        const fs::path book = agent.root() / "demo.genko";
        Json panel = panel_of(book, art.frame);
        QCOMPARE(panel["adopted"]["art"], done.data["adopted"]);
        QCOMPARE(done.data["upscaled"], Json(true));  // (a ~1 MP picture is below 600 dpi for a whole panel: enlarged and adopted again)
        QVERIFY(std::find(steps.begin(), steps.end(), "adopt_upscaled") != steps.end());
        QVERIFY(done.data["adopted"] != done.data["candidate_id"]);
        // a wrong file stops at the import and says so
        const std::string id = art.request.at("id").get<std::string>();
        ToolResult bad = agent.call("take_panel_art", Json::array({"demo.genko", id, Json{{"file", "studio/inbox/none.png"}, {"origin", kOrigin}}}),
                                    Json::object());
        QVERIFY(!bad.ok);
        QCOMPARE(bad.data["stopped_at"], Json("import"));
        // a sheet request is not for this tool
        ToolResult asked = agent.call("generation_request", Json::array({"demo.genko"}),
                                      Json::object({{"character_id", fixture("bible.json").at("characters").at(std::size_t{0}).at("id")}}));
        QVERIFY2(asked.ok, shown(asked).c_str());
        const std::string sheet = asked.data["request"]["id"].get<std::string>();
        const ToolResult refused =
            agent.call("take_panel_art", Json::array({"demo.genko", sheet, Json{{"file", "x"}, {"origin", kOrigin}}}), Json::object());
        QVERIFY2(!refused.issues.empty(), shown(refused).c_str());
        QCOMPARE(refused.issues[0].code, std::string("not_a_panel"));
    }

    // --- test_battle_report.py ----------------------------------------------------------------------------------------

    // フラッシュ and flash are the rays (uni_flash); white is only what 白で塗る asks for.
    void test_a_flash_is_rays_over_the_picture_and_white_is_only_asked_for() {
        const auto flash = genko::service::fxwords::resolve(Json("フラッシュ"));
        const auto english = genko::service::fxwords::resolve(Json("flash"));
        const auto white = genko::service::fxwords::resolve(Json("白で塗る"));
        QVERIFY(flash && english && white);
        QCOMPARE(flash->value("key", Json()), Json("uni_flash"));
        QCOMPARE(english->value("key", Json()), Json("uni_flash"));
        QCOMPARE(white->value("key", Json()), Json("white"));
    }

    // White is never painted over an adopted picture: the finish leaves 白で塗る out (an effect_skipped note saying why)
    // and puts the flash (ウニフラッシュ) in.
    void test_white_is_never_painted_over_an_adopted_picture() {
        StudioService agent(folder(QStringLiteral("no-white")), "ai:test");
        PanelArt art = panel_with_art(agent);
        QVERIFY2(art.error.empty(), art.error.c_str());
        QVERIFY2(art.done.ok, shown(art.done).c_str());
        const fs::path book = agent.root() / "demo.genko";
        const std::string refused = ops_as(book,
                                           Json::array({Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", art.frame},
                                                             {"set", Json::object({{"fx", Json::array({"白で塗る", "フラッシュ"})}})}}}),
                                           agent.actor());
        QVERIFY2(refused.empty(), refused.c_str());
        const Document episode = genko::service::load_episode(book);
        auto [ops, notes] = genko::service::finish::plan(episode, episode.page(0));
        std::vector<Json> kinds;
        for (const Json& op : ops) {
            if (op.value("op", Json()) == Json("add_effect") && op.value("frame_id", Json()) == Json(art.frame)) kinds.push_back(op.value("kind", Json()));
        }
        QVERIFY2(std::find(kinds.begin(), kinds.end(), Json("white")) == kinds.end(), ops.dump().c_str());
        QVERIFY2(std::find(kinds.begin(), kinds.end(), Json("uni_flash")) != kinds.end(), ops.dump().c_str());
        Json skipped;
        bool labelled = false;
        for (const Json& n : notes) {
            if (skipped.is_null() && n.value("kind", Json()) == Json("effect_skipped")) skipped = n;
            if (n.value("kind", Json()) == Json("add_effect") && n.value("label", Json()) == Json("ウニフラッシュ")) labelled = true;
        }
        QVERIFY2(!skipped.is_null() && has(skipped.value("why", Json()), "白"), notes.dump().c_str());
        QVERIFY2(labelled, notes.dump().c_str());
    }

    // For a tool that takes two references, one sheet of the three most needed (face, sheet, style) side by side in the
    // request's shape; none when there is only one reference. (Python checks genreq._reference_sheet itself; this
    // build's is internal to service/genreq.cpp, so the sheet is checked in the request: guides/references_sheet.png.)
    void test_one_sheet_for_a_tool_that_takes_two_references() {
        const fs::path project = folder(QStringLiteral("refs-sheet")) / "s.genko";
        const RefsBook book = refs_book(project, false);
        genreq::Options options;
        options.page = 1;
        options.frame_id = book.frame;
        const genreq::Pack pack = genreq::build(book.doc, project, options);
        Json request = pack.request;
        QCOMPARE(request["files"]["references_sheet"], Json("guides/references_sheet.png"));
        const auto file = pack.files.find("guides/references_sheet.png");
        QVERIFY(file != pack.files.end());
        const genko::render::Image image = genko::render::open_image(file->second);
        const Json size = request["size"]["suggested_px"];
        const double asked = size.at(std::size_t{0}).get<double>() / size.at(std::size_t{1}).get<double>();
        QVERIFY2(std::abs(double(image.width()) / image.height() - asked) < 0.02, size.dump().c_str());
        std::vector<std::vector<double>> colours;
        for (int x = 0; x < image.width(); x += 8) colours.push_back(image.getpixel(x, image.height() / 2));
        for (const std::vector<double>& colour : {std::vector<double>{200, 0, 0}, std::vector<double>{0, 200, 0}, std::vector<double>{0, 0, 200}}) {
            QVERIFY(std::find(colours.begin(), colours.end(), colour) != colours.end());
        }
        // one reference only: no sheet
        const fs::path alone_project = folder(QStringLiteral("refs-sheet-one")) / "o.genko";
        const RefsBook alone = refs_book(alone_project, true);
        options.frame_id = alone.frame;
        const genreq::Pack one = genreq::build(alone.doc, alone_project, options);
        Json one_request = one.request;
        QCOMPARE(one_request["files"]["references"], Json::array({"refs/hina_face.png"}));
        QVERIFY(one_request["files"]["references_sheet"].is_null());
        QCOMPARE(one.files.count("guides/references_sheet.png"), std::size_t{0});
    }

    // --- test_chat_approval.py ----------------------------------------------------------------------------------------

    // A chat approval is recorded in the person's name with no setting first (a name with a space cleaned: human:mi_mi);
    // with no name given, the person who last approved in this book; the person's words are needed and the final export
    // stays with the person; a person can stop it for this book and the agent cannot turn it back on; opened again, the
    // person who opened it.
    void test_a_chat_approval_is_recorded_without_a_setting_and_stops_where_the_person_stopped_it() {
        StudioService agent(folder(QStringLiteral("chat")), "ai:hermes");
        const std::string made = demo_book(agent, 1);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path path = agent.root() / "demo.genko";
        const Json page1 = Json::object({{"pages", Json::array({1})}});
        ToolResult done = agent.call("record_chat_approval", Json::array({"demo.genko", "name", "承認します 続けてください"}),
                                     Json{{"pages", Json::array({1})}, {"person", "mi mi"}});
        QVERIFY2(done.ok, shown(done).c_str());
        QCOMPARE(done.data["by"], Json("human:mi_mi"));
        QVERIFY(starts(done.data["via"], "ai:hermes"));
        Document episode = genko::service::load_episode(path);
        QVERIFY(episode.page(0).name_ok);
        QVERIFY(episode.studio["approvals"].is_array() && !episode.studio["approvals"].empty());
        Json record = episode.studio["approvals"].back();
        QCOMPARE(record["by"], Json("human:mi_mi"));
        QVERIFY(starts(record["via"], "ai:hermes"));
        QCOMPARE(record["message"], Json("承認します 続けてください"));
        // no name given: the person who last approved in this book
        ToolResult again = agent.call("record_chat_approval", Json::array({"demo.genko", "name", "OK"}), page1);
        QVERIFY2(again.ok, shown(again).c_str());
        QCOMPARE(again.data["by"], Json("human:mi_mi"));
        // the person's words are needed, and the final export stays with the person in Genko
        const ToolResult blank = agent.call("record_chat_approval", Json::array({"demo.genko", "name", "  "}), page1);
        QVERIFY(!blank.issues.empty());
        QCOMPARE(blank.issues[0].code, std::string("message_required"));
        const ToolResult exported = agent.call("record_chat_approval", Json::array({"demo.genko", "export", "OK"}), Json::object());
        QVERIFY(!exported.issues.empty());
        QCOMPARE(exported.issues[0].code, std::string("gate_not_available"));
        // a person can stop it for this book; the AI cannot turn it back on
        genko::service::HumanService(path, "human:mimi").chat_approval(Json(false));
        const ToolResult off = agent.call("record_chat_approval", Json::array({"demo.genko", "name", "OK"}), page1);
        QVERIFY(!off.issues.empty());
        QCOMPARE(off.issues[0].code, std::string("chat_approval_off"));
        QVERIFY(!agent.call("apply_ops", Json::array({"demo.genko", Json::array({Json{{"op", "allow_chat_approval"}, {"on", true}}})}),
                            Json::object({{"commit", true}}))
                     .ok);
        genko::service::HumanService(path, "human:mimi").chat_approval(Json(true));
        ToolResult back = agent.call("record_chat_approval", Json::array({"demo.genko", "name", "OK"}), page1);
        QVERIFY2(back.ok, shown(back).c_str());
        QCOMPARE(back.data["by"], Json("human:mimi"));  // (open again: no name given, the person who opened it)
    }

    // --- test_l1_style.py ---------------------------------------------------------------------------------------------

    // genko://use-style?id=… gives the id, another genko:// link nothing, an id that climbs out (../../etc) is refused;
    // a style's page on the site is <catalog>/n/<id>?lang=ja.
    void test_links_and_ids() {
        EnvVar site("GENKO_STYLE_CATALOG", "https://catalog.test");
        QCOMPARE(stylecat::style_from_link(Json("genko://use-style?id=shonen-battle")).value_or("<none>"), std::string("shonen-battle"));
        QVERIFY(!stylecat::style_from_link(Json("genko://other?id=x")).has_value());
        QVERIFY_THROWS_EXCEPTION(genko::core::stylecat::CatalogError, stylecat::style_from_link(Json("genko://use-style?id=../../etc")));
        QCOMPARE(stylecat::page_url("shonen"), std::string("https://catalog.test/n/shonen?lang=ja"));
    }

    // The book keeps the style it was given (a look first writes nothing); the art request then carries it: its words
    // first, the avoid lists merged without a word twice, its sample among the references, the request's style field;
    // a character sheet's request opens with the sheet's own words. The catalog's newer version is seen, the book's
    // stays; and the style can be let go.
    void test_the_book_keeps_the_style_and_the_art_requests_carry_it() {
        const fs::path root = folder(QStringLiteral("style-kept"));
        StyleCatalog catalog(root);
        StudioService agent(root, "ai:test");
        const std::string made = demo_book(agent, 1);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = agent.root() / "demo.genko";
        ToolResult look = agent.call("use_style", Json::array({"demo.genko", "shonen-battle-chibi"}), Json::object());  // (a look first: nothing is written)
        QVERIFY2(look.ok, shown(look).c_str());
        QCOMPARE(look.data["committed"], Json(false));
        QCOMPARE(look.data["style"]["path"], Json::array({"少年漫画", "バトル", "低頭身のデフォルメ"}));
        QVERIFY(genko::service::briefs::catalog(genko::service::load_episode(book)).is_null());
        ToolResult done = agent.call("use_style", Json::array({"demo.genko", "shonen-battle-chibi"}), Json::object({{"commit", true}}));
        QVERIFY2(done.ok, shown(done).c_str());
        QCOMPARE(done.data["style"]["id"], Json("shonen-battle-chibi"));
        QCOMPARE(done.data["style"]["has_sample"], Json(true));
        const Document episode = genko::service::load_episode(book);
        Json kept = genko::service::briefs::catalog(episode);
        QCOMPARE(kept["version"], Json(3));
        QCOMPARE(kept["prompt"]["ja"], Json(kWords));
        QVERIFY(starts(kept["sample"], "sha256:"));
        ToolResult status = agent.call("status", Json::array({"demo.genko"}), Json::object());
        QCOMPARE(status.data["style"]["title"], Json("低頭身のデフォルメ"));

        genreq::Options options;
        options.page = 1;
        options.frame_id = episode.page(0).leaf_frames().at(0)->id;
        const genreq::Pack pack = genreq::build(episode, book, options);
        Json request = pack.request;
        QVERIFY2(starts(request["prompt"]["ja"], kWords), request["prompt"].dump().c_str());
        QVERIFY(has(request["prompt"]["en"], "Finished style: chibi"));
        QVERIFY(has(request["prompt"]["tags"], "chibi"));
        const Json avoid = request["avoid"];
        QCOMPARE(std::count_if(avoid.begin(), avoid.end(), [](const Json& a) { return has(a, "透かし"); }), std::ptrdiff_t{1});  // (no word twice)
        QVERIFY(std::find(avoid.begin(), avoid.end(), Json("色")) != avoid.end());
        QVERIFY(std::find(avoid.begin(), avoid.end(), Json("コマ枠")) == avoid.end());  // (the same thing, once)
        QVERIFY(std::find(avoid.begin(), avoid.end(), Json("コマの枠線")) != avoid.end());
        const Json references = request["files"]["references"];
        QVERIFY(std::find(references.begin(), references.end(), Json("refs/style_catalog.png")) != references.end());
        QCOMPARE(pack.files.count("refs/style_catalog.png"), std::size_t{1});
        std::string where;
        QVERIFY2(genko::test::same_content(request["style"], Json{{"catalog_id", "shonen-battle-chibi"}, {"version", 3}, {"title", "低頭身のデフォルメ"}}, &where),
                 where.c_str());
        genreq::Options sheet_options;
        sheet_options.purpose = "character_sheet";
        sheet_options.character_id = episode.bible.characters.at(std::size_t{0}).at("id");
        Json sheet = genreq::build(episode, book, sheet_options).request;
        QVERIFY2(starts(sheet["prompt"]["ja"], "モノクロの日本のマンガの絵。"), sheet["prompt"].dump().c_str());  // (a sheet is a picture, not a panel)

        // the site changes: the book does not; the agent can see a newer version
        catalog.write_style("shonen-battle-chibi", 4);
        ToolResult seen = agent.call("style_catalog", Json::array({"demo.genko"}), Json::object());
        catalog.write_style("shonen-battle-chibi", 3);
        QVERIFY2(seen.ok, shown(seen).c_str());
        QCOMPARE(seen.data["style"]["version"], Json(3));
        QCOMPARE(seen.data["newer"]["version"], Json(4));
        const ToolResult back = agent.call("use_style", Json::array({"demo.genko", nullptr}), Json::object({{"commit", true}}));
        QVERIFY2(back.ok, shown(back).c_str());
        QVERIFY(genko::service::briefs::catalog(genko::service::load_episode(book)).is_null());
    }

    // A black and white style on a colour book (webtoon) is told (style_mono_on_color) and the art request does not use
    // it: not its words, no style field, a note saying it is for black and white.
    void test_a_black_and_white_style_on_a_colour_book_is_told_and_not_used() {
        const fs::path root = folder(QStringLiteral("style-mono"));
        StyleCatalog catalog(root);
        StudioService agent(root, "ai:test");
        const std::string made = demo_book(agent, 1, "webtoon");
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = agent.root() / "demo.genko";
        const ToolResult done = agent.call("use_style", Json::array({"demo.genko", "shonen"}), Json::object({{"commit", true}}));
        QVERIFY2(done.ok, shown(done).c_str());
        QVERIFY(codes(done).count("style_mono_on_color") == 1);
        const Document episode = genko::service::load_episode(book);
        genreq::Options options;
        options.page = 1;
        options.frame_id = episode.page(0).leaf_frames().at(0)->id;
        Json request = genreq::build(episode, book, options).request;
        QVERIFY(!starts(request["prompt"]["ja"], kWords));
        QVERIFY(!request.contains("style"));
        const Json notes = request["notes_for_agent"];
        QVERIFY2(std::any_of(notes.begin(), notes.end(), [](const Json& n) { return has(n, "白黒用"); }), notes.dump().c_str());
    }

    // A style's page on the catalog's own site names its style (with spaces around and more in its query); another
    // site's page does not; the studio's style_catalog takes the page's URL as an id.
    void test_a_shared_page_url_names_its_style() {
        {
            EnvVar site("GENKO_STYLE_CATALOG", "https://catalog.test");
            QCOMPARE(stylecat::style_from_link(Json("https://catalog.test/n/shonen-battle?lang=ja")).value_or("<none>"), std::string("shonen-battle"));
            QVERIFY(!stylecat::style_from_link(Json("https://elsewhere.test/n/shonen-battle")).has_value());  // (only the catalog's own pages)
            QCOMPARE(stylecat::style_id_from(Json("  https://catalog.test/n/shonen-battle?lang=ja&x=1 ")), std::string("shonen-battle"));
            QCOMPARE(stylecat::style_id_from(Json("shonen-battle")), std::string("shonen-battle"));
            QVERIFY_THROWS_EXCEPTION(genko::core::stylecat::CatalogError, stylecat::style_id_from(Json("https://elsewhere.test/n/shonen-battle")));
        }
        // (the catalog here is a folder, file://localhost/…: its pages are https://localhost/n/<id>)
        const fs::path root = folder(QStringLiteral("style-url"));
        StyleCatalog catalog(root);
        StudioService agent(root, "ai:test");
        ToolResult seen = agent.call("style_catalog", Json::array(), Json::object({{"style_id", "https://localhost/n/shonen-battle?lang=ja"}}));
        QVERIFY2(seen.ok, shown(seen).c_str());
        QCOMPARE(seen.data["style"]["id"], Json("shonen-battle"));
    }

    // A style's path reads the same from the new catalog (its parents only) and the old one (itself last too), for a
    // first-level style, and for a book saved before (the whole path as titles).
    void test_the_style_path_reads_the_same_from_the_new_and_the_old_catalog() {
        const Json fresh = Json{{"id", "gag-surreal-doodle-casual"},
                                {"title", "ゆるい手描きの落書き風"},
                                {"path", Json::array({Json{{"id", "gag"}, {"title", "ギャグ漫画"}}, Json{{"id", "gag-surreal"}, {"title", "シュール"}}})}};
        Json old = fresh;
        old["path"].push_back(Json{{"id", "gag-surreal-doodle-casual"}, {"title", "ゆるい手描きの落書き風"}});
        const Json want = Json::array({"ギャグ漫画", "シュール", "ゆるい手描きの落書き風"});
        QCOMPARE(stylecat::full_path(fresh), want);
        QCOMPARE(stylecat::full_path(old), want);
        QCOMPARE(stylecat::full_path(Json{{"id", "gag"}, {"title", "ギャグ漫画"}, {"path", Json::array()}}), Json::array({"ギャグ漫画"}));  // (the first level)
        QCOMPARE(stylecat::full_path(Json{{"id", "x"}, {"title", "ゆるい手描きの落書き風"}, {"path", want}}), want);  // (a book saved before)
    }

    // --- test_m11.py --------------------------------------------------------------------------------------------------

    // An unknown face key falls back to the default face (アンチック).
    void test_antique_sets_kana_in_mincho_and_kanji_in_gothic() {
        QCOMPARE(genko::render::text::face_of(Json("nope")).key, std::string("antique"));
    }

    // --- test_m1_report.py --------------------------------------------------------------------------------------------

    // A jacket and a separate back cover both reach the e-book: the front first, the pages, the back last, both covers
    // alone in the middle of the screen; a jacket alone gives its own back, cut out of it.
    void test_a_jacket_and_a_back_cover_both_reach_the_ebook() {
        const fs::path out = folder(QStringLiteral("covers"));
        Document ep = named_book(2);
        ep = genko::service::apply_ops(ep,
                                       Json::array({Json{{"op", "add_cover"}, {"kind", "jacket"}, {"spine_mm", 8}, {"flap_mm", 60}},
                                                    Json::object({{"op", "add_cover"}, {"kind", "back"}})}),
                                       "genko")
                 .doc;
        auto files = genko::render::zip::read(genko::formats::export_epub(ep, out / "b.epub", 20));
        std::string opf = files["OEBPS/content.opf"];
        const std::size_t front = opf.find("cover_front");
        const std::size_t p1 = opf.find("p001");
        const std::size_t p2 = opf.find("p002");
        const std::size_t back = opf.find("cover_back");
        QVERIFY2(front != std::string::npos && p1 != std::string::npos && p2 != std::string::npos && back != std::string::npos, opf.c_str());
        QVERIFY2(front < p1 && p1 < p2 && p2 < back, opf.c_str());
        const std::string spine = opf.substr(opf.find("<spine"));
        QVERIFY2(spine.find("idref=\"page_cover_front\" properties=\"rendition:page-spread-center\"") != std::string::npos, spine.c_str());
        QVERIFY2(spine.find("idref=\"page_cover_back\" properties=\"rendition:page-spread-center\"") != std::string::npos, spine.c_str());
        Document only_jacket = named_book(1);
        only_jacket = genko::service::apply_ops(only_jacket,
                                                Json::array({Json{{"op", "add_cover"}, {"kind", "jacket"}, {"spine_mm", 8}, {"flap_mm", 0}}}), "genko")
                          .doc;
        auto jacket_files = genko::render::zip::read(genko::formats::export_epub(only_jacket, out / "j.epub", 20));
        opf = jacket_files["OEBPS/content.opf"];
        QVERIFY2(opf.find("cover_front") != std::string::npos && opf.find("cover_back") != std::string::npos, opf.c_str());  // (the jacket's own back)
    }

    // replace_text says how many it replaced and where; the page's assignee shows in status and in the snapshot.
    void test_replace_text_says_how_many_and_where() {
        StudioService agent(folder(QStringLiteral("replace")), "ai:hermes");
        QVERIFY(agent.call("create_project", Json::array({"demo.genko", "demo", 2}), Json::object()).ok);
        const fs::path path = agent.project_path(Json("demo.genko"));
        {
            genko::service::Session session(path, "human:leaf");
            session.apply(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "かさを持って"}, {"id", "a"}},
                                       Json{{"op", "set_assignee"}, {"pages", Json::array({2})}, {"who", "さくら"}}}),
                          "genko");
            session.save("human:leaf");
        }
        const ToolResult replaced = agent.call("apply_ops", Json::array({"demo.genko", Json::array({Json{{"op", "replace_text"}, {"find", "かさ"}, {"replace", "傘"}}})}),
                                               Json::object({{"commit", true}}));
        QVERIFY2(replaced.ok, shown(replaced).c_str());
        Json result = replaced.to_dict();
        Json report = result["results"][0];
        QCOMPARE(report["replaced"], Json(1));
        QCOMPARE(report["where"][0]["line"], Json("a"));
        QCOMPARE(report["where"][0]["page"], Json(1));
        Json status = agent.call("status", Json::array({"demo.genko"}), Json::object()).to_dict();
        QCOMPARE(status["pages"][1]["assignee"], Json("さくら"));
        Json snap = agent.call("inspect", Json::array({"demo.genko", "snapshot"}), Json::object()).to_dict()["snapshot"];
        QCOMPARE(snap["pages"][1]["assignee"], Json("さくら"));
    }

    // The page role (hook) goes to the page's last panel only, and the English prompt carries each panel's action; a
    // character sheet's request is a picture (漫画の絵), not a panel (漫画のコマ).
    void test_the_page_role_goes_to_one_panel_and_english_carries_the_action() {
        StudioService agent(folder(QStringLiteral("page-role")), "ai:test");
        const std::string made = approved_demo(agent, 4);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path project = agent.root() / "demo.genko";
        {
            genko::service::Session session(project, "human:leaf");
            genko::core::Page& page = session.doc().edit_page(0);
            Json plan = page.plan && page.plan->is_object() ? *page.plan : Json::object();
            plan["turn_role"] = "hook";
            page.plan = plan;
            session.save("human:leaf");
        }
        const Document episode = genko::service::load_episode(project);
        const auto frames = episode.page(0).leaf_frames();
        QVERIFY(!frames.empty());
        Json hooks = Json::array();
        for (const genko::core::Frame* frame : frames) {
            ToolResult asked = agent.call("generation_request", Json::array({"demo.genko"}), Json{{"page", 1}, {"frame_id", frame->id}});
            QVERIFY2(asked.ok, shown(asked).c_str());
            Json request = asked.data["request"];
            hooks.push_back(has(request["prompt"]["ja"], "次のページへ引く最後のコマ"));
            const Json panel = frame->panel ? *frame->panel : Json::object();
            const Json action = panel.value("action", Json());
            if (action.is_string() && !action.get<std::string>().empty()) {
                QVERIFY2(has(request["prompt"]["en"], "Action (in Japanese): " + action.get<std::string>()), request["prompt"]["en"].dump().c_str());
            }
        }
        Json want = Json::array();
        for (std::size_t i = 0; i + 1 < frames.size(); ++i) want.push_back(false);
        want.push_back(true);
        QCOMPARE(hooks, want);
        ToolResult sheet = agent.call("generation_request", Json::array({"demo.genko"}),
                                      Json::object({{"character_id", episode.bible.characters.at(std::size_t{0}).at("id")}}));
        QVERIFY2(sheet.ok, shown(sheet).c_str());
        const Json ja = sheet.data["request"]["prompt"]["ja"];
        QVERIFY2(!has(ja, "漫画のコマ") && has(ja, "漫画の絵"), ja.dump().c_str());
    }

    // import_psd reads a path inside the book (studio/layers.psd) through the studio; a PSD outside the root is refused
    // (path_outside_root). (A small PSD of two layers; what the reader makes of a painting app's file: test_contract_psd.)
    void test_import_psd_reads_from_the_book_and_stays_under_root() {
        const fs::path root = folder(QStringLiteral("psd"));
        StudioService agent(root, "ai:hermes");
        QVERIFY(agent.call("create_project", Json::array({"demo.genko", "demo", 2}), Json::object()).ok);
        const fs::path path = agent.project_path(Json("demo.genko"));
        const std::string psd = psd_bytes(182, 257, {{QStringLiteral("緑"), "green", 0, 0, 182, 257, {30, 200, 30, 255}},
                                                     {QStringLiteral("赤"), "red", 10, 10, 50, 50, {220, 30, 30, 255}}});
        genko::storage::write_atomic(path / "studio" / "layers.psd", psd);
        {
            genko::service::Session session(path, "human:leaf");
            for (std::size_t i = 0; i < session.doc().pages.size(); ++i) session.doc().edit_page(i).name_ok = true;
            session.save("human:leaf");
        }
        ToolResult ok = agent.call("apply_ops", Json::array({"demo.genko", Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", "studio/layers.psd"}, {"id", "p"}}})}),
                                   Json::object({{"commit", true}}));
        QVERIFY2(ok.ok, shown(ok).c_str());
        const Json layers = ok.data["results"][0]["layers"];
        QVERIFY2(layers.is_array() && !layers.empty(), shown(ok).c_str());
        const fs::path outside = root.parent_path() / "outside.psd";
        genko::storage::write_atomic(outside, psd);
        const ToolResult refused = agent.call(
            "apply_ops", Json::array({"demo.genko", Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", genko::core::path_to_utf8(outside)}}})}),
            Json::object());
        QVERIFY(!refused.ok);
        QVERIFY2(!refused.issues.empty(), shown(refused).c_str());
        QCOMPARE(refused.issues[0].code, std::string("path_outside_root"));
    }

    // An export to Kindle without a long edge takes Kindle's own (2560 px), not the 2048 of the other formats.
    void test_kindle_defaults_to_its_own_long_edge() {
        StudioService agent(folder(QStringLiteral("kindle")), "ai:hermes");
        QVERIFY(agent.call("create_project", Json::array({"demo.genko", "demo", 2}), Json::object()).ok);
        const ToolResult exported = agent.call("export", Json::array({"demo.genko"}), Json::object({{"format", "kindle"}}));
        QVERIFY2(exported.ok, shown(exported).c_str());
        Json reply = exported.to_dict();
        QVERIFY2(reply["files"].is_array() && !reply["files"].empty(), reply.dump().c_str());
        const auto files = genko::render::zip::read(genko::core::path_from_utf8(reply["files"][0].get<std::string>()));
        const auto image = std::find_if(files.begin(), files.end(), [](const auto& entry) { return entry.first.starts_with("OEBPS/images/"); });
        QVERIFY(image != files.end());
        const genko::render::Size size = genko::render::picture_size(image->second);
        QCOMPARE(std::max(size.width, size.height), 2560);
    }

    // A balloon that does not fit says how much the panel takes ("1 列 … 字・… 列まで").
    void test_an_overflowing_balloon_says_how_much_fits() {
        const std::string words = letter::fits(genko::service::Box{0, 0, 180, 76.8}, "thought");
        QVERIFY2(words.find("1 列") != std::string::npos && words.find("列まで") != std::string::npos, words.c_str());
    }

    // The name preview keeps a panel's long action band inside that panel (nothing spills over the gutter into the next
    // one) and shows the blocking (the person's head box in the panel's colour).
    void test_the_name_preview_keeps_its_bands_in_their_panels_and_shows_the_blocking() {
        Document ep = named_book(1);
        ep = genko::service::apply_ops(ep,
                                       Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", ep.page(0).frames.at(0).id},
                                                         {"axis", "vertical"}, {"ratio", 0.5}}}),
                                       "genko")
                 .doc;
        std::vector<std::string> ids;
        for (const genko::core::Frame* f : ep.page(0).leaf_frames()) ids.push_back(f->id);
        QVERIFY(ids.size() >= 2);
        {
            genko::core::Page& page = ep.edit_page(0);
            page.find_frame(ids[0])->panel = Json::object({{"slot", "p1"}});
            page.find_frame(ids[1])->panel = Json::object({{"slot", "p2"}});
        }
        std::string action;
        for (int i = 0; i < 20; ++i) action += "とても長い説明";
        const Json plan = Json{{"panels", Json::array({Json{{"slot", "p2"}, {"shot", "MS"}, {"angle", "eye"}, {"action", action},
                                                            {"characters", Json::array({Json{{"id", "hina"}, {"pos", "center"}, {"scale", 0.8}}})}}})},
                               {"tiers", Json::array({Json::object({{"cols", Json::array({Json::object({{"slot", "p1"}}), Json::object({{"slot", "p2"}})})}})})}};
        constexpr int dpi = 40;
        const genko::render::Image base = genko::render::Image::create("RGB", genko::render::Size{400, 560}, genko::render::Ink{255, 255, 255});
        const genko::render::Image out = genko::service::annotate(base, ep.page(0), dpi, plan);
        QVERIFY(out.getbbox().has_value());
        const auto px = [](double mm) { return genko::render::mm_to_px(mm, dpi); };
        const auto leaves = ep.page(0).leaf_frames();
        const genko::core::Rect& first = leaves.at(1)->rect;  // (p2, the left panel: its long band would run right, into p1)
        const genko::core::Rect& other = leaves.at(0)->rect;
        const int band_y = px(first.y.value() + first.height.value()) - 2;
        // (nothing of the first panel's band spills over the gutter into the next panel)
        const std::vector<double> white{255, 255, 255};
        for (int x = px(other.x.value()) + 3; x < px(other.x.value() + other.width.value()) - 3; ++x) {
            QVERIFY2(out.getpixel(x, band_y) == white, ("x " + std::to_string(x) + " y " + std::to_string(band_y)).c_str());
        }
        // (the blocking: the person's head box is drawn in the first panel)
        const std::vector<double> head_colour{200, 80, 60};
        bool head = false;
        for (int x = px(first.x.value()); x < px(first.x.value() + first.width.value()) && !head; ++x) {
            for (int y = px(first.y.value()); y < px(first.y.value() + first.height.value()) - 12 && !head; ++y) head = out.getpixel(x, y) == head_colour;
        }
        QVERIFY(head);
    }

    // --- test_m1_security.py ------------------------------------------------------------------------------------------

    // Releasing a lock never acquired (another holds it) does not touch the holder's lock: a third still cannot take it
    // until the holder gives it back.
    void test_release_does_not_remove_a_lock_it_does_not_hold() {
        const fs::path project = folder(QStringLiteral("lock")) / "p.genko";
        genko::storage::ProjectLock first(project, "ai:a");
        first.try_acquire();
        genko::storage::ProjectLock second(project, "ai:b");
        QVERIFY_THROWS_EXCEPTION(genko::storage::LockedError, second.try_acquire());
        second.release();  // never acquired: must not touch the holder's lock
        {
            genko::storage::ProjectLock third(project, "ai:c");
            QVERIFY_THROWS_EXCEPTION(genko::storage::LockedError, third.try_acquire());
        }
#ifndef Q_OS_WIN  // (on Windows no one else can read the locked byte while it is held)
        Json content = genko::storage::ProjectLock::read_content(project / "project.lock");
        QCOMPARE(content.value("agent", Json()), Json("ai:a"));
        QVERIFY(!content.value("released", false));
#endif
        first.release();
        genko::storage::ProjectLock fourth(project, "ai:c");
        fourth.try_acquire();
        QVERIFY(fourth.held());
        fourth.release();
    }

    // Every op this build handles (the registry the studio and the app apply ops through) is in the public catalogue
    // (`genko schema`), and docs/ops.schema.json is that catalogue — but the C++ build's own ops of the CMYK and Lab pages
    // (SPEC COLOR-02: set_color_mode, set_inks, put_color_raster; COLOR-03: assign_profile, convert_profile, PC1b's
    // set_text_gamma and the channel ops of channels@1, PC1d's to_indexed, set_color_table, to_bitmap and set_duotone; COLOR-04:
    // PC2's set_adjustment, auto_adjust, hdr_toning, apply_image, calculations, trap and fade_layer; the public `genko schema`
    // stays Python's).
    void test_every_handled_op_is_in_the_catalog_and_docs() {
        const Json docs = genko::test::read_json(genko::test::repo_root() + QStringLiteral("/docs/ops.schema.json"));
        std::string where;
        QVERIFY2(genko::test::same_content(docs.at("ops"), genko::core::ops_schema(), &where), where.c_str());
        std::set<std::string> listed;
        for (const Json& item : genko::core::ops_schema()) listed.insert(item.at("op").get<std::string>());
        std::string missing;
        const std::set<std::string> own = {"set_color_mode", "set_inks", "put_color_raster", "assign_profile", "convert_profile", "set_text_gamma",
                                           "add_channel", "duplicate_channel", "delete_channel", "rename_channel", "move_channel",
                                           "set_channel", "merge_spot_channel", "split_channels", "merge_channels",
                                           "to_indexed", "set_color_table", "to_bitmap", "set_duotone",
                                           "set_adjustment", "auto_adjust", "hdr_toning", "apply_image", "calculations", "trap", "fade_layer",
                                           // (PC3 A0, smart-objects@1: this build's own, and the later steps' registered)
                                           "smart_convert", "smart_place", "smart_profile", "smart_copy", "smart_transform", "rasterize_layer",
                                           "smart_replace", "smart_relink", "smart_update", "smart_embed", "smart_link", "smart_to_page",
                                           "smart_filter_add", "smart_filter_set", "smart_filter_move", "smart_filter_delete", "smart_filter_state",
                                           "smart_stack", "rasterize_lines",
                                           // (PC3 B1, SPEC COLOR-06: this build's own)
                                           "quick_mask", "magic_erase",
                                           // (PC3-0: the later groups' ops registered, SPEC COLOR-06 and COLOR-07)
                                           "outline_area", "apply_layer_mask", "mask_from_transparency", "matting", "clone_stamp",
                                           "healing_brush", "spot_healing", "history_brush", "content_aware_fill", "set_path", "delete_path",
                                           "rename_path", "make_work_path", "text_to_path", "stroke_path", "set_vector_mask", "add_shape_part",
                                           "edit_shape_part", "combine_shape_parts", "move_shape_parts", "transform_layers",
                                           "content_aware_scale", "align_layers", "distribute_layers", "arrange_layers", "link_layers",
                                           "resize_pages", "set_canvas_size", "crop_page", "trim_page", "reveal_all", "rotate_pages",
                                           // (PC4 F-0: 節 F's ops registered for F-b and F-e, SPEC FILE-01)
                                           "set_file_info", "set_camera_raw",
                                           // (PC4 E-0: the later groups' ops registered, SPEC COLOR-09)
                                           "dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush",
                                           "background_eraser", "color_replacement", "add_pattern", "rename_pattern", "delete_pattern",
                                           // (PC4 D0: 節 D's ops registered for D4 and D6, SPEC COLOR-08)
                                           "render_picture", "vanishing_point"};
        for (const std::string& name : genko::render::ops_registry().names()) {
            if (listed.count(name) == 0 && own.count(name) == 0) missing += (missing.empty() ? "" : ", ") + name;
        }
        for (const std::string& name : own) QVERIFY2(listed.count(name) == 0, name.c_str());
        QVERIFY2(missing.empty(), ("handled but not in the catalogue: " + missing).c_str());
    }
};

QTEST_GUILESS_MAIN(TestLedgerServiceA)
#include "test_ledger_service_a.moc"
