// 制御チャンネルの決まり (PC5 J-g, SPEC.md PROD-01; PhotoCraft v0.2.0 crates/automation/src/security.rs's limits and first
// line, MIT): the table of methods, the first line, the ops it takes, the view's commands and panels.

#include "service/control.hpp"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <functional>

#include "core/pyconv.hpp"
#include "service/studio_service.hpp"

namespace genko::service::control {

namespace {

constexpr std::array<Method, 9> kMethods{{
    {"auth", "どのトークンでも（つないで最初の 1 行だけ）", false, "トークンで名乗る。最初の 1 行がこれでないと何も答えずに切る"},
    {"methods", "human・ai", false, "この表（method・使える人・原稿を変えるか）"},
    {"ui.inspect", "human・ai", false,
     "画面を読む: 今の道具・ページ・フレーム・ズーム・パネル・開いている原稿の題名・メニューの木と使えるか・計測"},
    {"ui.screenshot", "human・ai（窓全体は human: だけ）", false, "キャンバスの絵（PNG、base64）。窓全体は human: のトークンだけ"},
    {"ui.set", "human・ai", false, "表示だけを変える: ページ・フレーム・ズーム・中心・合わせ方・道具・パネルの表示"},
    {"ui.menu.invoke", "human・ai", false, "表示の命令だけを選ぶ（ズーム・画面に合わせる・パネル・フレームの移動・エクストラ）"},
    {"ui.pointer", "human・ai", true,
     "キャンバスへのペン（ペン・鉛筆・消しゴム）。線はトークンの actor の op になる。描いている間は人の入力を止める"},
    {"engine.commands", "human・ai", false, "engine.execute が受ける op の一覧と引数（genko schema の MCP が受ける分）"},
    {"engine.execute", "human・ai", true, "op を前の原稿に当てる（MCP の apply_ops と同じ op・同じ断り、トークンの actor）"},
}};

// (the view's commands, by attribute: app/main_window_view.cpp, main_window_anim.cpp, main_window_guide_view.cpp …)
const std::vector<std::string> kViewCommands{
    // the zoom and the fit
    "act_zoom_in", "act_zoom_out", "act_fit", "act_fill_screen", "act_actual", "act_actual_pixels", "act_200_percent", "act_fit_artboard",
    // the timeline's frames (the window's, never the book's: J-e1's act_timeline, ビデオタイムラインを作成, changes it)
    "act_next_frame", "act_previous_frame", "act_first_frame", "act_last_frame", "act_work_start", "act_work_end",
    // the extras' switches (表示 → エクストラ, 表示・非表示 ▸, 定規, ガイド, グリッド)
    "act_extras", "act_show_all", "act_show_none", "act_scale", "act_guides", "act_show_guides", "act_grid", "act_pixel_grid",
    "act_show_rulers", "act_show_slices", "act_show_notes", "act_show_count", "act_show_layer_edges", "act_show_smart_guides",
    "act_show_target_path", "act_sel_edges", "act_show_mesh", "act_show_edit_pins", "act_show_canvas_guides", "act_show_artboard_guides",
    "act_show_artboard_names",
    // the panels' switches made as commands (ウィンドウ ▸)
    "act_panel_actions", "act_panel_layer_comps", "act_panel_measurement_log", "act_panel_notes"};

const std::vector<std::string> kViewPanels{"レイヤー", "ページ", "全体図", "タイムライン", "チャンネル", "パス", "シェイプ", "レイヤーカンプ",
                                           "注釈", "計測ログ", "アクション", "ヒストグラム", "情報", "カラー", "グラデーション", "ツールの設定",
                                           "素材", "台詞", "定規・3D", "コピーソース", "サブビュー"};

bool in(const std::vector<std::string>& list, std::string_view name) { return std::find(list.begin(), list.end(), name) != list.end(); }

// The ops that read a file the channel has no folder for (MCP confines them to --root; the command line reads the person's)
bool reads_a_file(std::string_view op) { return op == "import_psd" || op == "import_pages"; }

}  // namespace

QString server_name() {
    QString user;
    for (const char* key : {"LOGNAME", "USER", "LNAME", "USERNAME"}) {  // (as links::server_name: getpass.getuser())
        user = qEnvironmentVariable(key);
        if (!user.isEmpty()) break;
    }
    QString kept;
    for (const QChar c : user) {
        if (c.isLetterOrNumber()) kept += c;
    }
    const QString name = QStringLiteral("genko-studio-control-%1").arg(kept.isEmpty() ? QStringLiteral("user") : kept);
#ifdef _WIN32
    return name;  // (a named pipe; QLocalServer::UserAccessOption gives it the person's own access)
#else
    // (QStandardPaths makes the runtime folder the person's own, 0700: XDG_RUNTIME_DIR, else /tmp/runtime-<user>)
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return folder.isEmpty() ? name : QDir(folder).filePath(name);
#endif
}

std::span<const Method> methods() { return kMethods; }

const Method* method(std::string_view name) {
    for (const Method& m : kMethods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

Json methods_json() {
    Json out = Json::array();
    for (const Method& m : kMethods) {
        out.push_back(Json{{"name", std::string(m.name)}, {"who", std::string(m.who)}, {"changes_book", m.changes_book}, {"about", std::string(m.about)}});
    }
    return out;
}

std::optional<std::string> authenticate(std::string_view line, const std::map<std::string, std::string>& tokens) {
    Json request;
    try {
        request = Json::parse(line);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!request.is_object() || request.value("method", Json()) != Json("auth")) return std::nullopt;
    const Json params = request.value("params", Json());
    if (!params.is_object() || !params.contains("token") || !params["token"].is_string()) return std::nullopt;
    const std::string& given = params["token"].get_ref<const std::string&>();
    std::optional<std::string> found;
    for (const auto& [token, actor] : tokens) {  // (every token compared to the end: the time says nothing of which is near)
        unsigned diff = token.size() == given.size() ? 0U : 1U;
        for (std::size_t i = 0; i < token.size(); ++i) diff |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(i < given.size() ? given[i] : 0);
        if (diff == 0 && !token.empty()) found = actor;
    }
    if (found && !(found->starts_with("human:") || found->starts_with("ai:"))) return std::nullopt;
    return found;
}

core::Actor actor_of(const std::string& token_actor) { return core::Actor::with_agent_rights(token_actor); }

bool human_token(const std::string& token_actor) { return token_actor.starts_with("human:"); }

std::optional<Refusal> refused_ops(const Json& ops) {
    if (!ops.is_array() || ops.size() > kMaxOps) return Refusal{"ops_invalid", "ops は 500 個までの配列", "/ops"};
    std::function<std::optional<Refusal>(const Json&, const std::string&)> look = [&look](const Json& list, const std::string& at) -> std::optional<Refusal> {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& op = list[i];
            const std::string where = at + "/" + std::to_string(i);
            if (!op.is_object()) return Refusal{"ops_invalid", "op はオブジェクト", where};
            const Json name = op.value("op", Json());
            if (!name.is_string() || !service::agent_op(name.get<std::string>())) {
                return Refusal{"op_not_allowed", "op " + core::py_str(name) + " はこの道具では使えない", where + "/op"};
            }
            if (reads_a_file(name.get<std::string>())) {
                return Refusal{"op_not_allowed",
                               "op " + name.get<std::string>() + " はファイルを読むので制御チャンネルでは使えない（MCP の apply_ops か genko apply で）",
                               where + "/op"};
            }
            if (name == Json("for_pages") && op.contains("ops") && op["ops"].is_array()) {
                if (auto refused = look(op["ops"], where + "/ops")) return refused;
            }
        }
        return std::nullopt;
    };
    return look(ops, "/ops");
}

const std::vector<std::string>& view_commands() { return kViewCommands; }
bool view_command(std::string_view attribute) { return in(kViewCommands, attribute); }
const std::vector<std::string>& view_panels() { return kViewPanels; }
bool view_panel(std::string_view name) { return in(kViewPanels, name); }

bool pen_tool(std::string_view tool) { return tool == "pen" || tool == "pencil" || tool == "eraser"; }

Json answer(const Json& id, Json result) { return Json{{"id", id}, {"ok", true}, {"result", std::move(result)}}; }

Json refusal(const Json& id, const std::string& code, const std::string& message) {
    return Json{{"id", id}, {"ok", false}, {"error", Json{{"code", code}, {"message", message}}}};
}

}  // namespace genko::service::control
