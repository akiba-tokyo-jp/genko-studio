#include "api/mcp_tools.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ops_schema.hpp"
#include "core/pyconv.hpp"
#include "render/not_yet_ported.hpp"
#include "service/data.hpp"
#include "service/result.hpp"
#include "service/studio_service.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "studio/files.hpp"

namespace genko::api::mcp {

namespace {

// genko/mcp/server.py INSTRUCTIONS
constexpr std::string_view kInstructions = R"(Genko は漫画原稿のシステム。文章も絵も作らない。あなた（エージェント）が企画書・脚本・ネーム計画を書き、
Genko が検査・コマ割り・縦書き写植・プレビュー描画をする。絵はあなたが別の道具で生成し、import_image で取り込む。
進め方: status → next で次の作業を取る → 書く道具を commit=true で呼ぶ（エラーがあれば何も書かずに返るので issues の path を直して送り直す）。書く道具の返事の next が次の作業。
ネームの規則は resource genko://guide/manga-rules（inspect target=rules でも読める）。
作画: generation_request で依頼パックを受け取る → 自分の画像ツールで生成し、返された inbox フォルダに保存 →
take_panel_art（取り込み・採用・足りなければ拡大・顔と人物の位置の報告を 1 回で）。2 枚以上を比べるときだけ import_images → candidates → review_candidates → adopt。
作画の承認後に finish_page。最後に check（人と同じ点検）・preflight と export（既定は png）。
描く・直す: apply_ops でページ・コマ・レイヤー（複製・結合・マスク）・線・塗り・グラデーション・台詞（傍点・部分書式・回転）・3D（背景は add_scene）・トーン・効果線まで、人が画面でできることは全部できる（op 一覧は resource genko://ops）。間違えたら undo（自分の変更だけ）。
使える素材・書体・ブラシは inspect target=materials / fonts / brushes、レイヤーと台詞の今の設定は inspect target=snapshot。
render は layer_id でそのレイヤーだけ、mode=print で印刷と同じ見え方。
3 回直しても通らないときは ask_human で人間に相談して、その作業を置いておく。
承認と本番の書き出しは人間だけが行う。承認が要るところでは request_approval を出して待つ（人がチャットではっきり承認したら record_chat_approval で人の名前で記録する）。
同じ Genko を複数の会話（Telegram のスレッドなど）で使うときは、どの道具にも session に会話の名前（例 "9204"）を渡す。
変更はその名前で記録され、undo はその会話の変更だけを戻す。別の会話が使っている原稿に書くと、一度だけ book_in_use で止まる。)";

// The tools as Python registers them (@tool on each function of build_server): name, docstring and parameters, the
// keyword-only session last (str | None = None).
const std::vector<ToolSpec>& table() {
    static const std::vector<ToolSpec> tools = {
        {"projects",
         R"(--root 配下のプロジェクト一覧。)",
         {{"session", Kind::Str, true, Json()}}},
        {"create_project",
         R"(新しいプロジェクトを作る。name は --root からの相対名（例 summer.genko）。spec_preset: commercial-b4 / a4-mono / webtoon。)",
         {{"name", Kind::Str, false, std::nullopt}, {"title", Kind::Str, false, std::nullopt}, {"pages", Kind::Int, false, std::nullopt}, {"spec_preset", Kind::Str, false, Json("commercial-b4")}, {"session", Kind::Str, true, Json()}}},
        {"status",
         R"(工程の様子（企画書・脚本の有無、ページごとのネームと承認、承認待ち）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"next",
         R"(次にやる作業（kind、対象ページ・コマ、使う道具の目安）。waiting_for は人間の承認待ち。
        並行して動くときは claim=true で返した作業を 10 分予約する（他のエージェントには出ない）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"limit", Kind::Int, false, Json(5)}, {"claim", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"inspect",
         R"(読む。target: ops（apply_ops の op の名前の一覧。op に名前〔カンマ区切り〕を渡すとその引数の形） / drawing（op と書き出しの
        詳しい使い方） / bible / script / page（そのページの beat、前後ページ、めくりの位置、定型、コマ一覧） /
        panel（コマのブリーフ・寸法 mm・候補の要約〔full=true で全部〕・登場人物の設定画、frame_id 省略でページ全部） / studio / schemas / rules /
        snapshot（page を渡すとそのページだけ。ページ・レイヤー〔名前・種類・不透明度・合成・マスク・表示色・フォルダ・参照〕・台詞〔書式・フキダシ〕・3D） /
        materials（stamp_material で貼れる素材: id・名前・種類〔トーン・効果線・画像・パーツ・描き文字・ブラシ・3D〕・フォルダ・タグ） / fonts（style.font に使える書体） /
        brushes（add_stroke の kind に使えるブラシ: 入っているもの・この原稿の自作・自分の自作） / upscalers（upscale の method） / plugins（人が入れた
        フィルターのプラグイン: filter_raster の kind に "plugin:<key>"、params は PARAMS のとおり。置き場所は folder:
        Linux は ~/.config/genko/plugins、Windows は %APPDATA%\genko\plugins）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"target", Kind::Str, false, std::nullopt}, {"page", Kind::Int, true, Json()}, {"frame_id", Kind::Str, true, Json()}, {"op", Kind::Str, true, Json()}, {"full", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"render",
         R"(ページのプレビュー画像（mode: name / proof / print。print は印刷と同じ見え方）。コマ番号は読み順。
        frame_id を渡すとそのコマだけ。layer_id を渡すとそのレイヤーだけを白の上に。
        kind: compare（候補にネームを赤で重ねる。candidate_id 省略で採用中の絵）/ guide:composition / guide:pose / guide:keepout /
        atari（アタリと提案の重ね表示）。
        画像はファイルにも保存する。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"mode", Kind::Str, false, Json("name")}, {"max_px", Kind::Int, false, Json(1024)}, {"frame_id", Kind::Str, true, Json()}, {"kind", Kind::Str, true, Json()}, {"candidate_id", Kind::Str, true, Json()}, {"layer_id", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"import_image",
         R"(生成した画像を取り込み、asset（sha256:…）と画素数 px を返す。path は --root の中のファイル。
        取り込んだだけではどこにも使われない。apply_ops の import_candidates でコマの候補にする。)",
         {{"project", Kind::Str, false, std::nullopt}, {"path", Kind::Str, true, Json()}, {"png_base64", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"generation_request",
         R"(絵の依頼パックを作る（Genko は生成しない）。コマは page+frame_id、設定画は character_id、背景の参照は location_id。
        purpose: panel_art / draft / character_sheet / location。mode: new / edit / inpaint（regions に領域 id、
        "face:<人物 id>"、"person:<人物 id>" か rect_mm）/ upscale。複数人物のコマで一人だけ直すときは focus_character。
        修正は parent に元の候補 id。tool は tools.json のツール id（例 openai:gpt-image-1）。
        返す: request（サイズの候補、prompt の ja/en/tags、avoid、keepout、figures）、files（ガイドと参照画像の場所）、inbox。
        同じ内容なら同じ id。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, true, Json()}, {"frame_id", Kind::Str, true, Json()}, {"character_id", Kind::Str, true, Json()}, {"location_id", Kind::Str, true, Json()}, {"purpose", Kind::Str, true, Json()}, {"mode", Kind::Str, false, Json("new")}, {"parent", Kind::Str, true, Json()}, {"tool", Kind::Str, true, Json()}, {"instruction", Kind::Str, true, Json()}, {"regions", Kind::List, true, Json()}, {"focus_character", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"import_images",
         R"(生成した画像を依頼の候補として取り込む。images: [{file: "studio/inbox/<request_id>/a.png" か asset: "sha256:…",
        origin: {kind: "agent", tool_id, model, prompt（実際に使ったもの）, params, refs_used, note}}]。
        file は studio/inbox/ の中だけ。同じ画像は 2 回取り込まれない。設定画では face_box01: [x, y, 幅, 高さ]（画像の中の
        0〜1）で顔のアップの範囲を付ける（承認のとき、ここを顔の参照として切り出す）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"request_id", Kind::Str, false, std::nullopt}, {"images", Kind::ListDict, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"candidates",
         R"(候補の一覧（状態、点数、stale、来歴の要約、取り込み時の目安 metrics。rank が小さいほど良い）と並べた縮小画像。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, true, Json()}, {"frame_id", Kind::Str, true, Json()}, {"character_id", Kind::Str, true, Json()}, {"location_id", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"review_candidates",
         R"(候補の評価を残す。page と frame_id（どのコマの候補か）は必須。reviews: [{candidate_id, score (0..1), note, fix?, checks}]。
        人物のいるコマでは checks が必須: {likeness: 設定画に似ているか 0..1, hands: ok / broken / none, text: 絵の中の文字 none / some,
        cut: 顔・手・頭が枠で切れる none / some}。Genko は絵を判断できないので、あなたが render と candidates を見て書く。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"frame_id", Kind::Str, false, std::nullopt}, {"reviews", Kind::ListDict, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"adopt",
         R"(候補を採用してコマに置く（to: art / bg / draft。fit: cover / contain / stretch）。page・frame_id は省くと候補から引く。
        art は本の解像度に足りなければ拡大して採用し直し（upscale: false で止める）、regions（顔・人物の位置）を渡せば報告まで
        1 回で済む。場所の参照画像は location_id。作画の確定は人間の art 承認で行う。)",
         {{"project", Kind::Str, false, std::nullopt}, {"candidate_id", Kind::Str, false, std::nullopt}, {"page", Kind::Int, true, Json()}, {"frame_id", Kind::Str, true, Json()}, {"to", Kind::Str, false, Json("art")}, {"fit", Kind::Str, true, Json()}, {"offset_mm", Kind::ListFloat, true, Json()}, {"scale", Kind::Float, true, Json()}, {"location_id", Kind::Str, true, Json()}, {"regions", Kind::ListDict, true, Json()}, {"upscale", Kind::Bool, false, Json(true)}, {"session", Kind::Str, true, Json()}}},
        {"request_fix",
         R"(修正のチケットを残す（自分で直す予定のメモ、または人間への相談）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"instruction", Kind::Str, false, std::nullopt}, {"frame_id", Kind::Str, true, Json()}, {"candidate_id", Kind::Str, true, Json()}, {"scope", Kind::Str, false, Json("frame")}, {"session", Kind::Str, true, Json()}}},
        {"report_regions",
         R"(採用した絵の顔と人物の位置を報告する（写植の顔よけに使う）。regions: [{kind: face|person, char?,
        box01: [x, y, w, h]（採用した画像の中の 0..1）か rect_mm}]。人物のいない絵なら record_review kind=regions。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"frame_id", Kind::Str, false, std::nullopt}, {"regions", Kind::ListDict, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"finish_page",
         R"(作画承認済みのページを仕上げる（顔にかかる台詞の移動、効果、finish へ）。commit=false で提案とプレビューだけ。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"preflight",
         R"(書き出しを止めている理由の一覧（承認、未採用のコマ、実効解像度、試験用の画像、来歴）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"export_proof",
         R"(校正用の書き出し（150 dpi、全ページに「校正」の透かし）。本番の書き出しは人間が行う。40 秒で終わらないときは job を返す。)",
         {{"project", Kind::Str, false, std::nullopt}, {"format", Kind::Str, false, Json("png")}, {"session", Kind::Str, true, Json()}}},
        {"check",
         R"(人が使う「入稿前の点検」と同じ点検（はみ出し・文字の小ささや重なり・印刷に出ない絵など）。見つかった一つずつは checks に入る。
        pages を渡すとそのページの指摘だけ（本全体の指摘＝ページの無いものは入る）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"pages", Kind::ListInt, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"undo",
         R"(自分（このエージェント）の最後の保存済みの変更を取り消す。次のときは断る: 最後の変更が人（や別のエージェント）のもの、
        承認が変わる変更、project.json が Genko の外で書き換えられた（記録と中身が合わない）、取り消すものが無い。)",
         {{"project", Kind::Str, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"export",
         R"(書き出し（承認は要らない。正式な書き出しは人だけ）: dpi の既定は 300（見せる・確かめる用。印刷用の本番は人が書き出す）。format は png（既定）/ pdf / tiff / cmyk / layers / psd / pack / epub /
        kindle / strip / webtoon / sns / timelapse / animation。pages でページを選ぶ（例 [3, 4, 5]）。area は paper / bleed / trim。
        pdf・png・tiff の color は auto（既定: モノクロの原稿はグレー、カラーは RGB）/ rgb / cmyk / gray / bitonal（白黒 2 階調）、cmyk と pdf の icc は印刷所の CMYK プロファイル（.icc のパス）。long_edge の既定は kindle 2560・sns 2048。timelapse は記録した制作過程（set_timelapse で記録）を movie（webp / gif / png / mp4）で、fps と
        seconds（全体の長さ）、pages は省略で全ページ（描いた順）か、1 ページだけを [n] で。animation はアニメーションのページ（pages に 1 つ）を movie（gif / webp /
        png / mp4 / frames〔連番 PNG〕）で、width で幅を。書いた先は <原稿>/exports/。
        epub と kindle は仕上がりで切り、トーンを網点にせずグレーで描く（網点を縮めるとモアレが出る）。dots=true で印刷と同じ網点。
        screen（{lpi, angle, shape}）は color bitonal のときグレーを網点にする。
        40 秒で終わらないときは job を返す（書き出しは続いている）。export_status（wait_s で最大 60 秒待つ）で結果を取る。)",
         {{"project", Kind::Str, false, std::nullopt}, {"format", Kind::Str, false, Json("png")}, {"pages", Kind::ListInt, true, Json()}, {"dpi", Kind::Int, true, Json()}, {"area", Kind::Str, false, Json("bleed")}, {"width", Kind::Int, false, Json(800)}, {"max_height", Kind::Int, false, Json(1280)}, {"long_edge", Kind::Int, true, Json()}, {"jpeg", Kind::Bool, false, Json(false)}, {"spreads", Kind::Bool, false, Json(false)}, {"color", Kind::Str, false, Json("auto")}, {"icc", Kind::Str, true, Json()}, {"fps", Kind::Float, false, Json(12)}, {"seconds", Kind::Float, true, Json()}, {"movie", Kind::Str, false, Json("webp")}, {"dots", Kind::Bool, false, Json(false)}, {"screen", Kind::Dict, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"record_chat_approval",
         R"(人がチャット（Telegram など）で承認したときに、その承認を人の名前で記録する。許可は要らない（人が原稿ごとに
        止めていれば chat_approval_off）。gate は name / art / sheet。message に人が送った言葉をそのまま、person に
        チャットでのその人の名前を入れる。人がはっきり承認していないのに呼ばない。)",
         {{"project", Kind::Str, false, std::nullopt}, {"gate", Kind::Str, false, std::nullopt}, {"message", Kind::Str, false, std::nullopt}, {"pages", Kind::ListInt, true, Json()}, {"character_id", Kind::Str, true, Json()}, {"candidate_id", Kind::Str, true, Json()}, {"face_box01", Kind::ListFloat, true, Json()}, {"person", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"export_status",
         R"(export / export_proof / upscale が job を返したとき（40 秒で終わらなかった処理）の様子。status: running / done / failed /
        lost（Genko が途中で止まった）。done なら result に書き出しの返事（files など）が入る。wait_s 秒（最大 60）まで終わるのを待ってから返す。)",
         {{"project", Kind::Str, false, std::nullopt}, {"job", Kind::Str, false, std::nullopt}, {"wait_s", Kind::Float, false, Json(60)}, {"session", Kind::Str, true, Json()}}},
        {"upscale",
         R"(採用した絵（か candidate_id の候補）を拡大して、新しい候補にする（印刷の解像度に届かないとき）。method: "genko"
        （Genko の拡大: Lanczos と線の輪郭の整え）か、人が登録した高解像度化の道具（inspect target=upscalers）。scale の既定は
        原稿の dpi に届く倍率（4 倍まで）。描き込みは増えない。候補には拡大した印が付き、preflight に出る。できた候補は adopt で置く。
        40 秒で終わらないときは job を返す（export_status で結果を取る）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"frame_id", Kind::Str, false, std::nullopt}, {"candidate_id", Kind::Str, true, Json()}, {"scale", Kind::Float, true, Json()}, {"method", Kind::Str, false, Json("genko")}, {"session", Kind::Str, true, Json()}}},
        {"derive",
         R"(Genko の決定的な処理で候補を作る。kind lineart: 候補（省略で採用中の絵）の線を抜き出した黒線の層。
        adopt の to: "ink" で置くと、トーンにした絵の上にくっきりした線が乗る。params: {radius, threshold, min_px}。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"frame_id", Kind::Str, false, std::nullopt}, {"kind", Kind::Str, false, Json("lineart")}, {"candidate_id", Kind::Str, true, Json()}, {"params", Kind::Dict, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"import_name",
         R"(人間が描いたアタリ（スキャン画像）を取り込む。files は --root の中の画像（1 ページ 1 枚、start_page から）。
        原本は資産になり下描き層に置かれる（印刷されない）。コマ割りを検出して提案にする。確定は人間。)",
         {{"project", Kind::Str, false, std::nullopt}, {"files", Kind::ListStr, false, std::nullopt}, {"start_page", Kind::Int, false, Json(1)}, {"align", Kind::Str, false, Json("auto")}, {"session", Kind::Str, true, Json()}}},
        {"analyze_name",
         R"(アタリのコマ割りを検出し直す（params: min_gutter_mm, min_panel_mm, speck_mm など）。新しい提案と重ね表示を返す。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"params", Kind::Dict, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"propose_lines",
         R"(アタリの手書き台詞を読んだ結果を提案する。lines: [{text（列は 
 で区切る）, balloon?, speaker?,
        box01: [x, y, w, h]（アタリ画像の中の 0..1）か x_mm, y_mm（w_mm, h_mm は省略可）}]。確定は人間。重ね表示を返す。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"lines", Kind::ListDict, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"proposals",
         R"(コマ割りと台詞の提案の一覧（status: open / all）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"status", Kind::Str, false, Json("open")}, {"session", Kind::Str, true, Json()}}},
        {"review_page",
         R"(人間の確認用ページ（studio/review.html）を作り、場所を返す。承認を頼んだら、この場所をメッセージで人間に知らせる。)",
         {{"project", Kind::Str, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
        {"ask_human",
         R"(人間に相談する（3 回直しても通らないときなど）。そのページ・コマの作業は人間が閉じるまで next に出ない。)",
         {{"project", Kind::Str, false, std::nullopt}, {"text", Kind::Str, false, std::nullopt}, {"page", Kind::Int, true, Json()}, {"frame_id", Kind::Str, true, Json()}, {"item", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"set_bible",
         R"(企画書（bible@1）を検査し、commit=true で保存する。形式は inspect target=schemas。)",
         {{"project", Kind::Str, false, std::nullopt}, {"bible", Kind::Dict, false, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"set_script",
         R"(脚本（script@1: scene → beat、beat ごとに page）を検査し、commit=true で保存する。)",
         {{"project", Kind::Str, false, std::nullopt}, {"script", Kind::Dict, false, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"submit_name",
         R"(1ページのネーム計画（name_plan@1）を検査・コマ割り・写植し、プレビュー画像を返す。
        commit=true で保存。すでにネームがあるページを作り直すときは replace=true。)",
         {{"project", Kind::Str, false, std::nullopt}, {"plan", Kind::Dict, false, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"replace", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"take_panel_art",
         R"(コマ 1 つの絵を 1 回で入れる: 依頼（request_id）の画像を取り込み、採用し、原稿の解像度に足りなければ拡大して
        採用し直し、regions があれば顔と人物の位置を報告する（report_regions と同じ形）。image は import_images の 1 件と
        同じ {file（studio/inbox/ の中）か asset, origin}。止まったら stopped_at にどの段かが入る。候補の点検
        （review_candidates）と人の承認は別に行う。upscale=false で拡大しない（dpi_before・dpi_wanted は返る）。
        crop01 [x, y, 幅, 高さ]（絵の中の 0..1）で、取り込む前に切り抜く（画像ツールが残した白い余白など。regions の
        box01 は切り抜いた後の絵の中）。拡大しても本の解像度に届かないときは警告 dpi_short。)",
         {{"project", Kind::Str, false, std::nullopt}, {"request_id", Kind::Str, false, std::nullopt}, {"image", Kind::Dict, false, std::nullopt}, {"regions", Kind::ListDict, true, Json()}, {"upscale", Kind::Bool, false, Json(true)}, {"method", Kind::Str, false, Json("genko")}, {"crop01", Kind::ListFloat, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"style_catalog",
         R"(マンガの絵柄カタログ（https://manga.akiba.tokyo.jp）を読む。引数なし: 1段目のジャンルの一覧。
        style_id: その絵柄（言葉 prompt_ja・白黒かカラーか・1 つ下の段の children）。id のほか、サイトのページの URL（人が共有したもの）でもよい。children から選んで下の段へたどる。
        どの段で止めてもよい。同じ名前の絵柄が別の分類にあれば same_title_elsewhere に分類（path）と id が入る。
        title: 名前にその言葉を含む絵柄を全部（分類 path つき）。名前だけで指定されたら、これで重なりを確かめる。
        project だけ: 原稿の絵柄と、カタログに新しい版が出ているか（newer）。)",
         {{"project", Kind::Str, true, Json()}, {"style_id", Kind::Str, true, Json()}, {"title", Kind::Str, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"use_style",
         R"(カタログの絵柄を原稿の絵柄にする（言葉・描かない物・見本の絵・版を原稿に写す。サイトが変わっても原稿の絵柄は
        変わらない）。以後の generation_request の prompt・avoid・参照画像（refs/style_catalog.png）に入る。
        style_id null で Genko の言葉に戻す。試しのページで絵柄が固定されたあとは人しか変えられない。
        絵柄は本の印象を決めるので、選ぶ前に ask_human で人に確かめるのがよい。)",
         {{"project", Kind::Str, false, std::nullopt}, {"style_id", Kind::Str, true, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"apply_ops",
         R"(細かい修正と作画の状態（台詞の移動・編集、コマの分割・結合、set_panel、import_candidates、adopt_candidate、
        set_placement など。一覧は inspect target=schemas ではなく genko://ops）。承認・ロック・ファイル読み込みの op は使えない
        （import_psd だけは読める: path は原稿のフォルダからの相対パスか、--root の中の絶対パス）。返事の results に、
        一部の op が見つけたもの・作ったもの（replace_text の件数と場所、import_psd のレイヤー）が入る。)",
         {{"project", Kind::Str, false, std::nullopt}, {"ops", Kind::ListDict, false, std::nullopt}, {"commit", Kind::Bool, false, Json(false)}, {"session", Kind::Str, true, Json()}}},
        {"record_review",
         R"(プレビューを見た自己点検の結果を残す（同じネームで点検を繰り返さないため）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"page", Kind::Int, false, std::nullopt}, {"notes", Kind::Str, false, std::nullopt}, {"score", Kind::Float, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"request_approval",
         R"(人間に承認を依頼する。gate: name（ネーム、pages は複数まとめてよい）/ art（絵、pages）/ sheet（キャラクター設定画、
        character_id、複数なら character_ids で 1 回に）/ export（全ページの仕上げ後、本番の書き出し）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"pages", Kind::ListInt, true, Json()}, {"note", Kind::Str, false, Json("")}, {"gate", Kind::Str, false, Json("name")}, {"character_id", Kind::Str, true, Json()}, {"character_ids", Kind::ListStr, true, Json()}, {"session", Kind::Str, true, Json()}}},
        {"tickets",
         R"(承認依頼と、人間からの修正指示の一覧（status: open / all）。)",
         {{"project", Kind::Str, false, std::nullopt}, {"status", Kind::Str, false, Json("open")}, {"session", Kind::Str, true, Json()}}},
        {"resolve_ticket",
         R"(人間からの直しの指示（kind fix、担当 agent のチケット）を直し終えたと返して閉じる。note に何をしたかを書く。
        承認の依頼や ask_human の質問は閉じられない（人が閉じる）。人は genko studio reopen-ticket で開き直せる。)",
         {{"project", Kind::Str, false, std::nullopt}, {"ticket_id", Kind::Str, false, std::nullopt}, {"note", Kind::Str, false, std::nullopt}, {"session", Kind::Str, true, Json()}}},
    };
    return tools;
}

}  // namespace

std::string_view instructions() { return kInstructions; }

const std::vector<ToolSpec>& tool_specs() { return table(); }

const ToolSpec* find_tool(std::string_view name) {
    for (const ToolSpec& tool : table()) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

// --- the input schema ------------------------------------------------------------------------------------------------

namespace {

// pydantic's schema of one annotation after slim_schema (no titles, `anyOf [X, null]` as X, no `default: null`, no
// `additionalProperties: true`).
Json kind_schema(Kind kind) {
    const auto array_of = [](Json items) { return Json::object({{"items", std::move(items)}, {"type", "array"}}); };
    switch (kind) {
        case Kind::Str: return Json::object({{"type", "string"}});
        case Kind::Int: return Json::object({{"type", "integer"}});
        case Kind::Float: return Json::object({{"type", "number"}});
        case Kind::Bool: return Json::object({{"type", "boolean"}});
        case Kind::Dict: return Json::object({{"type", "object"}});
        case Kind::List: return array_of(Json::object());
        case Kind::ListInt: return array_of(Json::object({{"type", "integer"}}));
        case Kind::ListFloat: return array_of(Json::object({{"type", "number"}}));
        case Kind::ListStr: return array_of(Json::object({{"type", "string"}}));
        case Kind::ListDict: return array_of(Json::object({{"type", "object"}}));
    }
    return Json::object();
}

}  // namespace

Json input_schema(const ToolSpec& tool) {
    Json properties = Json::object();
    Json required = Json::array();
    for (const ToolParam& p : tool.params) {
        Json schema = kind_schema(p.kind);
        if (p.fallback && !p.fallback->is_null()) {
            // ({"default": …, "title": …, "type": …}: pydantic's keys in order, the title dropped)
            Json with = Json::object({{"default", *p.fallback}});
            for (const auto& [key, value] : schema.items()) with[key] = value;
            schema = std::move(with);
        }
        properties[std::string(p.name)] = std::move(schema);
        if (!p.fallback) required.push_back(std::string(p.name));
    }
    Json out = Json::object({{"properties", std::move(properties)}});
    if (!required.empty()) out["required"] = std::move(required);
    out["type"] = "object";
    return out;
}

// --- the arguments, as pydantic checks them ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kPydanticDocs = "https://errors.pydantic.dev/2.13/v/";

struct Problem {
    std::string loc;
    std::string type;
    std::string message;
    Json input;
};

// Rust's str::trim: the Unicode White_Space characters at both ends.
bool rust_space(char32_t c) {
    return c == 0x20 || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// The code point at text[i] (UTF-8) and its length; (0xFFFD, 1) for a byte that does not start one.
std::pair<char32_t, std::size_t> code_point(std::string_view text, std::size_t i) {
    const auto b = static_cast<unsigned char>(text[i]);
    if (b < 0x80) return {b, 1};
    const std::size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if (n == 1 || i + n > text.size()) return {0xFFFD, 1};
    char32_t c = b & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (std::size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    return {c, n};
}

std::string_view trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto [c, n] = code_point(text, start);
        if (!rust_space(c)) break;
        start += n;
    }
    std::size_t end = text.size();
    while (end > start) {
        std::size_t back = end - 1;
        while (back > start && (static_cast<unsigned char>(text[back]) & 0xC0) == 0x80) --back;
        if (!rust_space(code_point(text, back).first)) break;
        end = back;
    }
    return text.substr(start, end - start);
}

bool digit(char c) { return c >= '0' && c <= '9'; }

// Python's digit groups: digits with single underscores between them. The digits without the underscores, or nothing.
std::optional<std::string> digit_groups(std::string_view text) {
    if (text.empty() || !digit(text.front()) || !digit(text.back())) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '_') {
            if (!digit(text[i - 1]) || !digit(text[i + 1])) return std::nullopt;
            continue;
        }
        if (!digit(text[i])) return std::nullopt;
        out += text[i];
    }
    return out;
}

enum class Parsed { Ok, Bad, TooBig };

// pydantic's str → int: trimmed, a sign, digits (Python's underscores), and a fraction of zeros only.
Parsed str_to_int(std::string_view raw, std::int64_t& out) {
    std::string_view text = trim(raw);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (const std::size_t dot = text.find('.'); dot != std::string_view::npos) {
        const std::string_view fraction = text.substr(dot + 1);
        if (fraction.empty() || fraction.find_first_not_of('0') != std::string_view::npos) return Parsed::Bad;
        text = text.substr(0, dot);
    }
    const auto digits = digit_groups(text);
    if (!digits) return Parsed::Bad;
    const std::string signed_digits = (negative ? "-" : "") + *digits;
    const auto [end, ec] = std::from_chars(signed_digits.data(), signed_digits.data() + signed_digits.size(), out);
    if (ec == std::errc::result_out_of_range) return Parsed::TooBig;  // (Python's int has no bound: this build stops)
    return ec == std::errc() && end == signed_digits.data() + signed_digits.size() ? Parsed::Ok : Parsed::Bad;
}

// pydantic's str → float: trimmed, Python's float syntax in ASCII (underscores between digits, inf, infinity, nan).
bool str_to_float(std::string_view raw, double& out) {
    std::string_view text = trim(raw);
    bool negative = false;
    std::string_view body = text;
    if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }
    std::string lower;
    for (char c : body) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (lower == "inf" || lower == "infinity") {
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (lower == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    // mantissa [e|E [sign] digits]
    std::string_view mantissa = body;
    std::string_view exponent;
    bool has_exponent = false;
    if (const std::size_t e = lower.find('e'); e != std::string::npos) {
        mantissa = body.substr(0, e);
        exponent = body.substr(e + 1);
        has_exponent = true;
    }
    std::string clean = negative ? "-" : "";
    const std::size_t dot = mantissa.find('.');
    const std::string_view whole = dot == std::string_view::npos ? mantissa : mantissa.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view() : mantissa.substr(dot + 1);
    if (whole.empty() && fraction.empty()) return false;
    if (!whole.empty()) {
        const auto d = digit_groups(whole);
        if (!d) return false;
        clean += *d;
    } else {
        clean += '0';
    }
    if (dot != std::string_view::npos) {
        clean += '.';
        if (!fraction.empty()) {
            const auto d = digit_groups(fraction);
            if (!d) return false;
            clean += *d;
        }
    }
    if (has_exponent) {
        clean += 'e';
        if (!exponent.empty() && (exponent.front() == '+' || exponent.front() == '-')) {
            clean += exponent.front();
            exponent.remove_prefix(1);
        }
        const auto d = digit_groups(exponent);
        if (!d) return false;
        clean += *d;
    }
    const auto [end, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), out);
    if (ec == std::errc::result_out_of_range) {
        // (a value past the range: an infinity, or a zero, as Python's float() makes it)
        bool big = has_exponent && exponent.find_first_not_of('0') != std::string_view::npos &&
                   clean.find("e-") == std::string::npos;
        if (!has_exponent) big = true;
        out = big ? (negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity()) : (negative ? -0.0 : 0.0);
        return true;
    }
    return ec == std::errc() && end == clean.data() + clean.size();
}

constexpr double kTwo63 = 9223372036854775808.0;

bool in_i64(double value) { return value >= -kTwo63 && value < kTwo63; }

// An integer the JSON holds from 2^63 to 2^64 - 1 (the parser keeps those as unsigned): past this build's 64 bits as
// any integer past them is, whatever the parameter.
bool past_i64(const Json& value) {
    return value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

// The value for an int parameter, or the problem.
std::optional<Problem> as_int(const Json& value, Json& out) {
    if (value.is_boolean()) {
        out = value.get<bool>() ? 1 : 0;
        return std::nullopt;
    }
    if (past_i64(value)) return Problem{{}, "too_big", {}, value};
    if (value.is_number_integer()) {
        out = value;
        return std::nullopt;
    }
    if (value.is_number_float()) {
        const double f = value.get<double>();
        if (!std::isfinite(f)) return Problem{{}, "finite_number", "Input should be a finite number", value};
        if (f != std::floor(f)) return Problem{{}, "int_from_float", "Input should be a valid integer, got a number with a fractional part", value};
        if (!in_i64(f)) return Problem{{}, "int_parsing_size", "Unable to parse input string as an integer, exceeded maximum size", value};
        out = static_cast<std::int64_t>(f);
        return std::nullopt;
    }
    if (value.is_string()) {
        std::int64_t parsed = 0;
        switch (str_to_int(value.get_ref<const std::string&>(), parsed)) {
            case Parsed::Ok:
                out = parsed;
                return std::nullopt;
            case Parsed::TooBig:
                return Problem{{}, "too_big", {}, value};
            case Parsed::Bad:
                break;
        }
        return Problem{{}, "int_parsing", "Input should be a valid integer, unable to parse string as an integer", value};
    }
    return Problem{{}, "int_type", "Input should be a valid integer", value};
}

std::optional<Problem> as_float(const Json& value, Json& out) {
    if (value.is_boolean()) {
        out = value.get<bool>() ? 1.0 : 0.0;
        return std::nullopt;
    }
    if (past_i64(value)) return Problem{{}, "too_big", {}, value};  // (an integer past 64 bits: refused as a number or a text)
    if (value.is_number()) {
        out = value.get<double>();
        return std::nullopt;
    }
    if (value.is_string()) {
        std::int64_t whole = 0;
        if (str_to_int(value.get_ref<const std::string&>(), whole) == Parsed::TooBig) return Problem{{}, "too_big", {}, value};
        double parsed = 0;
        if (str_to_float(value.get_ref<const std::string&>(), parsed)) {
            out = parsed;
            return std::nullopt;
        }
        return Problem{{}, "float_parsing", "Input should be a valid number, unable to parse string as a number", value};
    }
    return Problem{{}, "float_type", "Input should be a valid number", value};
}

std::optional<Problem> as_bool(const Json& value, Json& out) {
    const Problem parsing{{}, "bool_parsing", "Input should be a valid boolean, unable to interpret input", value};
    const Problem type{{}, "bool_type", "Input should be a valid boolean", value};
    if (value.is_boolean()) {
        out = value;
        return std::nullopt;
    }
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return type;
    if (value.is_number_integer()) {
        const std::int64_t n = value.get<std::int64_t>();
        if (n == 0 || n == 1) {
            out = n == 1;
            return std::nullopt;
        }
        return parsing;
    }
    if (value.is_number_float()) {
        const double f = value.get<double>();
        if (f == 0.0 || f == 1.0) {
            out = f == 1.0;
            return std::nullopt;
        }
        if (!std::isfinite(f) || f != std::floor(f) || !in_i64(f)) return type;
        return parsing;
    }
    if (value.is_string()) {
        std::string lower;
        for (char c : value.get_ref<const std::string&>()) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        static constexpr std::array<std::string_view, 6> kFalse{"0", "off", "f", "false", "n", "no"};
        static constexpr std::array<std::string_view, 6> kTrue{"1", "on", "t", "true", "y", "yes"};
        if (std::find(kFalse.begin(), kFalse.end(), lower) != kFalse.end()) {
            out = false;
            return std::nullopt;
        }
        if (std::find(kTrue.begin(), kTrue.end(), lower) != kTrue.end()) {
            out = true;
            return std::nullopt;
        }
        return parsing;
    }
    return type;
}

// One value checked as pydantic checks `kind` (lax, Python input): the value it makes, or problems at `loc`. A text of an
// integer past 64 bits, which Python takes, sets `unsupported` (this build refuses the call when nothing else is wrong).
void check_value(Kind kind, const Json& value, const std::string& loc, Json& out, std::vector<Problem>& problems,
                 std::optional<std::string>& unsupported) {
    const auto add = [&](std::optional<Problem> problem, const std::string& where) {
        if (!problem) return;
        if (problem->type == "too_big") {
            if (!unsupported) {
                std::string pointer;
                std::size_t start = 0;
                for (;;) {  // (loc "pages.0" is the pointer "/pages/0")
                    const std::size_t dot = where.find('.', start);
                    pointer += "/" + core::json_pointer_token(where.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
                    if (dot == std::string::npos) break;
                    start = dot + 1;
                }
                unsupported = pointer;
            }
            out = nullptr;
            return;
        }
        problem->loc = where;
        problems.push_back(std::move(*problem));
    };
    const auto list_of = [&](Kind item) {
        if (!value.is_array()) {
            add(Problem{{}, "list_type", "Input should be a valid list", value}, loc);
            return;
        }
        out = Json::array();
        for (std::size_t i = 0; i < value.size(); ++i) {
            Json one;
            check_value(item, value[i], loc + "." + std::to_string(i), one, problems, unsupported);
            out.push_back(std::move(one));
        }
    };
    switch (kind) {
        case Kind::Str:
            if (value.is_string()) {
                out = value;
            } else {
                add(Problem{{}, "string_type", "Input should be a valid string", value}, loc);
            }
            return;
        case Kind::Int: add(as_int(value, out), loc); return;
        case Kind::Float: add(as_float(value, out), loc); return;
        case Kind::Bool: add(as_bool(value, out), loc); return;
        case Kind::Dict:
            if (value.is_object()) {
                out = value;
            } else {
                add(Problem{{}, "dict_type", "Input should be a valid dictionary", value}, loc);
            }
            return;
        case Kind::List:
            if (value.is_array()) {
                out = value;
            } else {
                add(Problem{{}, "list_type", "Input should be a valid list", value}, loc);
            }
            return;
        case Kind::ListInt: list_of(Kind::Int); return;
        case Kind::ListFloat: list_of(Kind::Float); return;
        case Kind::ListStr: list_of(Kind::Str); return;
        case Kind::ListDict: list_of(Kind::Dict); return;
    }
}

// pydantic's input_value: repr(value), past 50 bytes its first 25 and last 24 (cut at whole characters) around "...".
std::string input_value(const Json& value) {
    const std::string text = core::py_repr(value);
    if (text.size() <= 50) return text;
    const auto boundary = [&text](std::size_t i) { return i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80; };
    std::size_t head = 25;
    while (!boundary(head)) --head;
    std::size_t tail = text.size() - 24;
    while (!boundary(tail)) ++tail;
    return text.substr(0, head) + "..." + text.substr(tail);
}

// The pre-parse: a string for a parameter that is not a plain str is read as JSON, kept when it is a list, an object
// or null (json.loads of it; a number, a bool or a string leaves the text). A text of NaN, an infinity or an integer
// past 64 bits inside a list or an object: `unsupported` gets where.
Json pre_parse(const ToolParam& param, const Json& value, std::optional<std::string>& unsupported) {
    if (!value.is_string() || (param.kind == Kind::Str && !param.nullable)) return value;
    core::ParseRepairs repairs;
    Json parsed;
    try {
        parsed = core::parse_python_json(value.get_ref<const std::string&>(), &repairs);
    } catch (const core::Error&) {
        return value;  // (not JSON: the text is checked as it is)
    }
    if (!(parsed.is_array() || parsed.is_object() || (parsed.is_null() && repairs.nonfinite.empty() && repairs.inexact.empty()))) {
        return value;  // (a number, a bool, a string, or a lone NaN / infinity / big integer, which Python reads as a number)
    }
    if (!repairs.nonfinite.empty() || !repairs.inexact.empty()) {
        unsupported = "/" + std::string(param.name) + (!repairs.nonfinite.empty() ? repairs.nonfinite.front() : repairs.inexact.front());
    } else if (const auto big = unsupported_number(parsed)) {  // (2^63 to 2^64 - 1 in the text)
        unsupported = "/" + std::string(param.name) + *big;
    }
    return parsed;
}

}  // namespace

Checked check_arguments(const ToolSpec& tool, const Json& arguments) {
    Checked out;
    Json given = Json::object();
    std::optional<std::string> unsupported;
    for (const auto& [key, value] : arguments.items()) {
        const ToolParam* param = nullptr;
        for (const ToolParam& p : tool.params) {
            if (p.name == key) param = &p;
        }
        given[key] = param != nullptr ? pre_parse(*param, value, unsupported) : value;
    }
    if (unsupported) {
        out.unsupported = unsupported;
        return out;
    }
    std::vector<Problem> problems;
    Json values = Json::object();
    for (const ToolParam& p : tool.params) {
        const std::string name(p.name);
        const auto found = given.find(name);
        if (found == given.end()) {
            if (p.fallback) {
                values[name] = *p.fallback;  // (a default is not checked)
            } else {
                problems.push_back(Problem{name, "missing", "Field required", given});
            }
            continue;
        }
        if (found->is_null() && p.nullable) {
            values[name] = nullptr;
            continue;
        }
        Json value;
        check_value(p.kind, *found, name, value, problems, unsupported);
        values[name] = std::move(value);
    }
    if (!problems.empty()) {
        std::string text = std::to_string(problems.size()) + (problems.size() == 1 ? " validation error for " : " validation errors for ") +
                           std::string(tool.name) + "Arguments";
        for (const Problem& problem : problems) {
            text += "\n" + problem.loc + "\n  " + problem.message + " [type=" + problem.type + ", input_value=" + input_value(problem.input) +
                    ", input_type=" + core::py_type_name(problem.input) + "]\n    For further information visit " + std::string(kPydanticDocs) +
                    problem.type;
        }
        out.error = std::move(text);
        return out;
    }
    if (!unsupported) unsupported = unsupported_number(values);  // (a float parameter's "inf" or "nan", or a text past the range)
    if (unsupported) {
        out.unsupported = unsupported;
        return out;
    }
    out.values = std::move(values);
    return out;
}

std::optional<std::string> unsupported_number(const Json& value) {
    if (value.is_number_float() && !std::isfinite(value.get<double>())) return std::string();
    if (past_i64(value)) return std::string();
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (auto inner = unsupported_number(value[i])) return "/" + std::to_string(i) + *inner;
        }
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (auto inner = unsupported_number(item)) return "/" + core::json_pointer_token(key) + *inner;
        }
    }
    return std::nullopt;
}

// --- the call ----------------------------------------------------------------------------------------------------------

namespace {

// genko/mcp/server.py READ_ONLY: tools that never write (no book-in-use warning, no presence).
bool read_only(std::string_view tool) {
    static constexpr std::array<std::string_view, 15> kNames{"projects", "status", "next", "inspect", "render", "candidates", "preflight", "check",
                                                             "tickets", "proposals", "review_page", "export", "export_proof", "export_status",
                                                             "style_catalog"};
    return std::find(kNames.begin(), kNames.end(), tool) != kNames.end();
}

// NEXT_AFTER: writes whose reply carries the next work item.
bool next_after(std::string_view tool) {
    static constexpr std::array<std::string_view, 18> kNames{"create_project", "record_review", "set_bible", "set_script", "submit_name", "apply_ops",
                                                             "import_images", "take_panel_art", "adopt", "review_candidates", "report_regions",
                                                             "finish_page", "request_approval", "resolve_ticket", "record_chat_approval", "upscale",
                                                             "import_name", "use_style"};
    return std::find(kNames.begin(), kNames.end(), tool) != kNames.end();
}

double epoch_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// TextContent / ImageContent as the SDK writes them (their fields in the schema's order).
Json text_block(const std::string& text) { return Json::object({{"text", text}, {"type", "text"}}); }
Json image_block(const std::string& png) { return Json::object({{"data", core::b64encode(png)}, {"mimeType", "image/png"}, {"type", "image"}}); }

CallOutcome text_reply(const Json& reply) {
    CallOutcome out;
    out.content.push_back(text_block(core::dump_python(reply)));
    return out;
}

CallOutcome crashed(std::string_view name) {
    CallOutcome out;
    out.content.push_back(text_block("Error executing tool " + std::string(name)));
    out.is_error = true;
    return out;
}

// A refusal this build alone makes, as a tool's failure: {"ok": false, "error" (the words people read), "detail" (the
// English, when the words differ), "code"}.
Json refusal(const std::string& message, const std::string& code) {
    Json reply = Json::object({{"ok", false}, {"error", service::wording_error(message)}});
    if (reply["error"] != message) reply["detail"] = message;
    reply["code"] = code;
    return reply;
}

CallOutcome refused_number(const std::string& where) {
    return text_reply(refusal("the arguments hold a number this build does not take (NaN, Infinity or an integer past 64 bits) at " +
                                  (where.empty() ? std::string("/") : where),
                              "unsupported_number"));
}

// The method a tool calls and how (each tool function's body in build_server: call(service.<method>, …)).
struct Call {
    std::string_view method;
    Json positional = Json::array();
    Json kwargs = Json::object();
};

Call mapping(std::string_view tool, const Json& v) {
    Call c{tool};
    const auto args = [&](std::initializer_list<const char*> names) {
        for (const char* name : names) c.positional.push_back(v.at(name));
    };
    if (tool == "projects") {
    } else if (tool == "create_project") {
        args({"name", "title", "pages", "spec_preset"});
    } else if (tool == "status" || tool == "preflight" || tool == "undo" || tool == "review_page") {
        args({"project"});
    } else if (tool == "next") {
        args({"project", "limit", "claim"});
    } else if (tool == "inspect") {
        args({"project", "target", "page", "frame_id", "op", "full"});
    } else if (tool == "render") {
        args({"project", "page", "mode", "max_px", "frame_id", "kind", "candidate_id", "layer_id"});
    } else if (tool == "import_image") {
        args({"project", "path", "png_base64"});
    } else if (tool == "generation_request") {
        args({"project", "page", "frame_id", "character_id", "location_id", "purpose", "mode", "parent", "tool", "instruction", "regions",
              "focus_character"});
    } else if (tool == "import_images") {
        args({"project", "request_id", "images"});
    } else if (tool == "candidates") {
        args({"project", "page", "frame_id", "character_id", "location_id"});
    } else if (tool == "review_candidates") {
        args({"project", "page", "frame_id", "reviews"});
    } else if (tool == "adopt") {
        args({"project", "candidate_id", "page", "frame_id", "to", "fit", "offset_mm", "scale", "location_id", "regions", "upscale"});
    } else if (tool == "request_fix") {
        args({"project", "page", "instruction", "frame_id", "candidate_id", "scope"});
    } else if (tool == "report_regions") {
        args({"project", "page", "frame_id", "regions"});
    } else if (tool == "finish_page") {
        args({"project", "page", "commit"});
    } else if (tool == "export_proof") {
        args({"project", "format"});
        c.kwargs["background"] = true;
    } else if (tool == "check") {
        args({"project", "pages"});
    } else if (tool == "export") {
        Json dpi = v.at("dpi");
        static constexpr std::array<std::string_view, 8> kPrint{"png", "pdf", "tiff", "cmyk", "layers", "psd", "pack", "strip"};
        const Json& format = v.at("format");
        if (dpi.is_null() && format.is_string() &&
            std::find(kPrint.begin(), kPrint.end(), format.get_ref<const std::string&>()) != kPrint.end()) {
            dpi = 300;
        }
        c.positional = Json::array({v.at("project"), format, v.at("pages"), dpi, v.at("area"), v.at("width"), v.at("max_height"), v.at("long_edge"),
                                    v.at("jpeg"), v.at("spreads"), v.at("color"), v.at("icc"), v.at("fps"), v.at("seconds"), v.at("movie")});
        c.kwargs = Json::object({{"background", true}, {"dots", v.at("dots")}, {"screen", v.at("screen")}});
    } else if (tool == "record_chat_approval") {
        args({"project", "gate", "message", "pages", "character_id", "candidate_id", "face_box01", "person"});
    } else if (tool == "export_status") {
        args({"project", "job", "wait_s"});
    } else if (tool == "upscale") {
        args({"project", "page", "frame_id", "candidate_id", "scale", "method"});
        c.kwargs["background"] = true;
    } else if (tool == "derive") {
        args({"project", "page", "frame_id", "kind", "candidate_id", "params"});
    } else if (tool == "import_name") {
        args({"project", "files", "start_page", "align"});
    } else if (tool == "analyze_name") {
        args({"project", "page", "params"});
    } else if (tool == "propose_lines") {
        args({"project", "page", "lines"});
    } else if (tool == "proposals" || tool == "tickets") {
        args({"project", "status"});
    } else if (tool == "ask_human") {
        args({"project", "text", "page", "frame_id", "item"});
    } else if (tool == "set_bible") {
        args({"project", "bible", "commit"});
    } else if (tool == "set_script") {
        args({"project", "script", "commit"});
    } else if (tool == "submit_name") {
        args({"project", "plan", "commit", "replace"});
    } else if (tool == "take_panel_art") {
        args({"project", "request_id", "image", "regions", "upscale", "method", "crop01"});
    } else if (tool == "style_catalog") {
        args({"project", "style_id", "title"});
    } else if (tool == "use_style") {
        args({"project", "style_id", "commit"});
    } else if (tool == "apply_ops") {
        args({"project", "ops", "commit"});
    } else if (tool == "record_review") {
        args({"project", "page", "score", "notes"});  // (the method takes score before notes)
    } else if (tool == "request_approval") {
        const Json& pages = v.at("pages");
        c.positional = Json::array({v.at("project"), v.at("gate"), core::py_truthy(pages) ? pages : Json::array(), v.at("note"),
                                    v.at("character_id"), v.at("character_ids")});
    } else if (tool == "resolve_ticket") {
        args({"project", "ticket_id", "note"});
    }
    return c;
}

// inspect.signature(method).bind(*args, **kwargs).arguments: what the call names, in the method's order ({} when the
// call does not bind).
Json bound_arguments(std::string_view method, const Json& positional, const Json& kwargs) {
    const auto params = service::StudioService::parameters(method);
    try {
        (void)service::bind_arguments("StudioService." + std::string(method), params, positional, kwargs);
    } catch (const core::PyTypeError&) {
        return Json::object();
    }
    Json out = Json::object();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i < positional.size() && !params[i].keyword_only) {
            out[params[i].name] = positional[i];
        } else if (kwargs.contains(params[i].name)) {
            out[params[i].name] = kwargs.at(params[i].name);
        }
    }
    return out;
}

// The exceptions this build throws where Python's tool raises something call() does not catch (ValueError, TypeError,
// FileNotFoundError, JSONDecodeError, any other): MCPServer answers those with "Error executing tool <name>" alone.
bool python_crash(const core::Error& error) {
    const std::string& code = error.code();
    return code == "value" || code == "type" || code == "python_error" || code == "not_found" || code == "json";
}

// A v1–v3 book (its project.json says so): this build reads it and writes nothing into it, the tool log and presence
// included (genko migrate makes a v4 copy; SPEC COMP-01a). A folder without a readable project.json is not one.
bool legacy_book(const fs::path& dir) {
    try {
        std::error_code ec;
        if (!fs::is_regular_file(dir / "project.json", ec)) return false;
        core::ParseOptions options;
        options.universal_newlines = true;
        return storage::project_version(core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, options)) < 4;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

Tools::Tools(fs::path root, std::string actor) : root_(std::move(root)), actor_(std::move(actor)) {
    if (!actor_.starts_with("ai:")) {
        throw core::ApplyError("MCP サーバーの actor は ai:<名前> にする（人間の権限はエージェントに渡さない）");
    }
}

CallOutcome Tools::call(std::string_view name, const Json& arguments, const std::optional<std::string>& unsupported) const {
    const ToolSpec* tool = find_tool(name);
    if (tool == nullptr) {
        CallOutcome out;
        out.content.push_back(text_block("Unknown tool: " + std::string(name)));
        out.is_error = true;
        return out;
    }
    if (unsupported) return refused_number(*unsupported);
    const Checked checked = check_arguments(*tool, arguments);
    if (!checked.error.empty()) {
        CallOutcome out;
        out.content.push_back(text_block("Error executing tool " + std::string(name) + ": " + checked.error));
        out.is_error = true;
        return out;
    }
    if (checked.unsupported) return refused_number(*checked.unsupported);
    const Json& values = *checked.values;
    try {
        const auto started = std::chrono::steady_clock::now();
        const Call c = mapping(name, values);
        const Json& session = values.at("session");
        std::string who;
        try {
            who = studio::actor_for(actor_, session.is_string() ? std::optional<std::string>(session.get<std::string>()) : std::nullopt);
        } catch (const core::Error& exc) {  // (ValueError: the session's name)
            return text_reply(Json::object({{"ok", false}, {"error", exc.what()}}));
        }
        const Json bound = bound_arguments(c.method, c.positional, c.kwargs);
        Json project;
        if (bound.contains("project") && core::py_truthy(bound["project"])) {
            project = bound["project"];
        } else if (c.method == "create_project" && bound.contains("name")) {
            project = bound["name"];
        }
        std::optional<fs::path> path;
        if (core::py_truthy(project)) {
            try {
                path = service::StudioService(root_, actor_).project_path(core::py_str(project));
            } catch (const core::ApplyError&) {
                path.reset();
            }
        }
        if (path && legacy_book(*path)) path.reset();  // (nothing written into a v1–v3 book: no presence, no tool log)
        const bool writes = path && !read_only(c.method) && !(bound.contains("commit") && bound["commit"].is_boolean() && !bound["commit"].get<bool>());
        std::error_code ec;
        if (writes && fs::is_directory(*path, ec)) {
            const std::vector<studio::Other> busy = studio::check(*path, who, epoch_now());
            if (!busy.empty()) {  // (held back once: the same call again goes through)
                std::string names;
                Json others = Json::array();
                for (const studio::Other& o : busy) {
                    const Json minutes = o.minutes_ago;
                    names += (names.empty() ? "" : "、") + o.actor + "（" + core::py_str(minutes) + " 分前）";
                    others.push_back(Json::object({{"actor", o.actor}, {"minutes_ago", minutes}}));
                }
                return text_reply(Json::object({{"ok", false},
                                                {"error", "この原稿は別の会話も使っている: " + names +
                                                              "。書き込まなかった。同じ原稿で続けるなら、もう一度同じ呼び出しをする（人に確かめてから）。"
                                                              "別々に進めるなら、原稿を複製して使う"},
                                                {"code", "book_in_use"},
                                                {"others", others},
                                                {"you", who}}));
            }
        }
        std::optional<service::ToolResult> result;
        std::optional<std::string> error;
        Json extra = Json::object();  // (this build's own refusals: their code, and what is not ported yet)
        try {
            result = service::StudioService(root_, who).call(c.method, c.positional, c.kwargs);
        } catch (const core::ApplyError& exc) {
            error = exc.what();
        } catch (const render::NotYetPorted& exc) {
            error = exc.what();
            extra["code"] = exc.code();
            extra["element"] = exc.element();
        } catch (const core::Error& exc) {
            if (python_crash(exc)) throw;
            error = exc.what();
            extra["code"] = exc.code();
        }
        if (path) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            studio::record_tool(*path, who, std::string(c.method), bound, result ? std::optional<Json>(result->to_dict()) : std::nullopt, error,
                                core::Num(ms), epoch_now());
            if (writes && result && result->ok) {
                studio::touch(*path, who, epoch_now());
                if (next_after(c.method) && result->data.is_object() && !result->data.contains("next")) {
                    // (what to do next rides on the reply: no separate next call after each write)
                    try {
                        const Json upcoming = service::StudioService(root_, who)
                                                  .call("next", Json::array({core::py_str(project)}), Json::object({{"limit", 1}}))
                                                  .data;
                        const Json& items = upcoming.at("items");
                        const Json first = core::py_truthy(items) ? items[0] : Json();
                        result->data["next"] = core::py_truthy(first) ? first : Json::object({{"waiting_for", upcoming.at("waiting_for")}});
                    } catch (const std::exception&) {  // (the write went through; the hint is only a help)
                    }
                }
            }
        }
        if (!result) {
            const std::string shown = service::wording_error(*error);
            const std::string head = error->substr(0, error->find(" ‖ "));
            Json reply = Json::object({{"ok", false}, {"error", shown}});
            if (service::wording_error(head) != head) reply["detail"] = head;
            for (const auto& [key, value] : extra.items()) reply[key] = value;
            return text_reply(reply);
        }
        CallOutcome out = text_reply(result->to_dict());
        for (const std::string& png : result->images) out.content.push_back(image_block(png));
        return out;
    } catch (const std::exception&) {
        return crashed(name);  // (a crash: its words stay on the server, as MCPServer keeps them)
    }
}

// --- resources ---------------------------------------------------------------------------------------------------------

const std::vector<ResourceSpec>& resource_specs() {
    static const std::vector<ResourceSpec> specs = {
        {"genko://ops", "ops_catalog", "apply_ops で使える op と引数。", "application/json"},
        {"genko://guide/skill", "skill", "Hermes 用のスキル（作業の手順、止まるところ、してはいけないこと）の正本。", "text/markdown"},
        {"genko://guide/drawing", "drawing_guide", "人と同じ道具で描く・直す: op と書き出しの詳しい使い方（inspect target=drawing と同じ）。",
         "text/markdown"},
        {"genko://guide/manga-rules", "manga_rules", "ネームの規則（段組 DSL、読み順、めくり、台詞の長さ）。", "text/markdown"},
    };
    return specs;
}

std::optional<std::string> read_resource(std::string_view uri) {
    if (uri == "genko://ops") {
        Json usable = Json::array();
        for (const Json& op : core::ops_schema()) {
            if (op.contains("op") && op["op"].is_string() && service::agent_op(op["op"].get<std::string>())) usable.push_back(op);
        }
        return core::dump_python(usable);
    }
    if (uri == "genko://guide/skill") return service::data::skill();
    if (uri == "genko://guide/drawing") return service::data::drawing();
    if (uri == "genko://guide/manga-rules") return service::data::rules();
    return std::nullopt;
}

}  // namespace genko::api::mcp
