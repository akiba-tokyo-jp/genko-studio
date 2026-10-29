#include "service/wording.hpp"

#include "core/pyconv.hpp"

#include <QHash>
#include <QRegularExpression>

#include <functional>
#include <map>
#include <vector>

namespace genko::wording {

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

const QHash<QString, QString>& fields() {
    static const QHash<QString, QString> table = {
        {"frames", "フレーム数"}, {"frame", "フレーム"}, {"at", "出すフレーム"}, {"fps", "1 秒のフレーム数"},
        {"kind", "種類"}, {"balloon", "フキダシの種類"}, {"align", "揃え"}, {"position", "ノンブルの位置"}, {"font", "書体"},
        {"pattern", "トーンの模様"}, {"preset", "見本"}, {"handle", "動かす所"}, {"count", "数"}, {"lpi", "線数"},
        {"density", "濃さ"}, {"size_mm", "大きさ"}, {"hidden_size_mm", "隠しノンブルの大きさ"}, {"start", "始まりの番号"},
        {"copies", "写しの数"}, {"ratio", "縦横の比"}, {"jitter", "ばらつき"}, {"depth", "トゲの長さ"}, {"length", "長さ"},
        {"fill", "フキダシの塗り"}, {"wrap", "縦書き・横書き"}, {"axis", "割る向き"}, {"to", "送り先"},
        {"space", "座標の取り方"}, {"start_side", "1 ページ目の側"}, {"style", "文字の設定"}, {"id", "対象"},
        {"index", "番号"}, {"op", "操作"}, {"text", "文字"}, {"frame_id", "コマ"}, {"page", "ページ"}, {"order", "並び順"},
        {"matrix", "変形"}, {"area", "範囲"}, {"points", "点"}, {"gradient start", "グラデーションの始まり"},
        {"gradient end", "グラデーションの終わり"}, {"gradient shape", "グラデーションの形"},
    };
    return table;
}

QString field(const QString& name) {
    const QString key = name.trimmed();
    return fields().value(key, key);
}

const QHash<QString, QString>& things() {
    static const QHash<QString, QString> table = {
        {"page", "ページ"}, {"panel", "コマ"}, {"layer", "レイヤー"}, {"line", "台詞"}, {"ruler", "定規"},
        {"effect", "効果線"}, {"tone", "トーン"}, {"ticket", "依頼"}, {"material", "素材"},
        {"mannequin", "デッサン人形"}, {"3D figure or box", "3D"}, {"stroke", "線"}, {"frame", "コマ"},
    };
    return table;
}

QString inner(const QString& text);

using Make = std::function<QString(const QRegularExpressionMatch&)>;

struct Rule {
    QRegularExpression pattern;
    QString replacement;  // (a group reference \1 is filled in)
    Make make;            // or this, when the words are built from the groups
};

Rule rule(const char* pattern, const char* replacement) {
    return Rule{QRegularExpression(QRegularExpression::anchoredPattern(QString::fromUtf8(pattern)),
                                   QRegularExpression::UseUnicodePropertiesOption),
                QString::fromUtf8(replacement), {}};
}

Rule rule(const char* pattern, Make make) {
    return Rule{QRegularExpression(QRegularExpression::anchoredPattern(QString::fromUtf8(pattern)),
                                   QRegularExpression::UseUnicodePropertiesOption),
                {}, std::move(make)};
}

// match.expand: \1 … \9 are the groups.
QString expand(const QString& replacement, const QRegularExpressionMatch& m) {
    QString out;
    for (qsizetype i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement[i];
        if (c == QLatin1Char('\\') && i + 1 < replacement.size() && replacement[i + 1].isDigit()) {
            out += m.captured(replacement[i + 1].digitValue());
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

const std::vector<Rule>& rules() {
    static const std::vector<Rule> table = [] {
        std::vector<Rule> r;
        r.push_back(rule(R"(brush (\S+) must be between (\S+) and (\S+))", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names = {{"width_mm", "太さ"}, {"min_pressure", "弱い筆圧での太さ"},
                                                          {"gamma", "筆圧の効き方"}, {"opacity", "不透明度"},
                                                          {"stabilize", "手ぶれ補正"}};
            return QStringLiteral("ブラシの%1は %2〜%3 の間で決めます").arg(names.value(m.captured(1), m.captured(1)), m.captured(2), m.captured(3));
        }));
        r.push_back(rule("a brush needs a name", "ブラシに名前を付けます"));
        // 紙質 (BRUSH-01: define_brush's paper, render/paper.hpp)
        r.push_back(rule(R"(the paper's picture cannot be read.*)", "紙質の画像を読めません（PNG・JPEG・BMP・GIF の画像を選んでください。壊れた画像は読めません）"));
        r.push_back(rule(R"(this build does not read that kind of picture.*)",
                         "この版の Genko では読めない形式の画像です（紙質は PNG・JPEG・BMP・GIF で読み込みます）"));
        r.push_back(rule(R"(the paper's picture is too large \(at most (\d+) pixels a side\))", "紙質の画像が大きすぎます（縦横 \\1 画素まで）"));
        r.push_back(rule(R"(the paper's picture file is too large.*)", "紙質の画像のファイルが大きすぎます（64 MB まで）"));
        r.push_back(rule("the paper's picture has no pixels", "紙質の画像に画素がありません"));
        r.push_back(rule(R"(the book has no paper picture .*)", "この原稿には、その紙質の画像がありません（画像を読み込み直してください）"));
        r.push_back(rule(R"(a paper needs its picture.*)", "紙質には画像が要ります（png か asset で指定します）"));
        r.push_back(rule(R"(a paper takes its picture once.*)", "紙質の画像は png か asset のどちらか一つで指定します"));
        r.push_back(rule(R"(paper (density|scale|rotation) must be between (\S+) and (\S+))", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names = {{"density", "濃さ"}, {"scale", "倍率"}, {"rotation", "回転"}};
            return QStringLiteral("紙質の%1は %2〜%3 の間で決めます").arg(names.value(m.captured(1)), m.captured(2), m.captured(3));
        }));
        r.push_back(rule("paper blend must be multiply or subtract", "紙質の合成は乗算（multiply）か減算（subtract）です"));
        r.push_back(rule("paper coords must be paper or stroke", "紙目の位置は紙面に固定（paper）か線ごと（stroke）です"));
        r.push_back(rule("paper seam must be repeat or mirror", "紙質の継ぎ目は繰り返し（repeat）か折り返し（mirror）です"));
        r.push_back(rule("paper (flip_x|flip_y|invert) must be true or false", "紙質の反転・濃淡の反転は true か false で決めます"));
        r.push_back(rule(R"(paper seed must be a whole number between 0 and (\d+))", "紙質の乱数の種は 0〜\\1 の整数で決めます"));
        r.push_back(rule(R"(paper has an unknown setting: (.*))", "紙質に知らない設定があります（\\1）"));
        r.push_back(rule(R"(paper asset must be an asset ref.*)", "紙質の画像（asset）は sha256: と 64 桁の 16 進数で指定します"));
        r.push_back(rule("paper must be an object or null", "紙質は設定のまとまり（オブジェクト）か null で指定します"));
        r.push_back(rule(R"(the paper picture \S+ is not known here.*)",
                         "紙質の画像が見つかりません（原稿の素材が欠けているなら、原稿の assets フォルダーに画像を戻すまで、そのブラシでは描けません。"
                         "自分のブラシなら、ブラシの詳細で紙質の画像を読み込み直せます）"));
        r.push_back(rule(R"(the brush paper library cannot be read, so it is left as it is: (.*))",
                         "自分のブラシの紙質の一覧（brush_papers.json）を読めないため、そのままにしました（紙質のあるブラシは保存できません）: \\1"));
        r.push_back(rule("a brush of one's own has a key starting with my_", "自作のブラシの名前（key）は my_ で始めます"));
        r.push_back(rule("wobble must be between 0 and 1", "線の揺れは 0〜1 の間で決めます"));
        r.push_back(rule("spikes must be between 6 and 80", "トゲの数は 6〜80 の間で決めます"));
        r.push_back(rule("spike_depth must be between 0.05 and 0.6", "トゲの長さは 0.05〜0.6 の間で決めます"));
        r.push_back(rule("style_runs is .*", "文字の一部の書式は、言葉と「大きさ・太字・色」の組で指定します"));
        r.push_back(rule("scale must be between 0.3 and 3", "文字の大きさの倍率は 0.3〜3 の間で決めます"));
        r.push_back(rule(R"(unknown style_runs key (\S+).*)", "文字の一部の書式に知らない項目があります（大きさ・太字・色だけ）"));
        // the lines of dialogue (core/ops_lines.cpp: the refusals the C++ build words more closely than the families below)
        r.push_back(rule("text is required", "台詞の文字が要ります（空の台詞は置けません）"));
        r.push_back(rule(R"(a tail's to must be two numbers \[x, y\])", "しっぽの先は [x, y] の 2 つの数で指定します"));
        r.push_back(rule(R"((x_mm|y_mm|w_mm|h_mm|path|tail) must be a finite number)", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names = {{"x_mm", "台詞の横の位置"}, {"y_mm", "台詞の縦の位置"}, {"w_mm", "フキダシの幅"},
                                                          {"h_mm", "フキダシの高さ"}, {"path", "フキダシの形"}, {"tail", "しっぽの先"}};
            return QStringLiteral("%1は、ふつうの数で指定します（無限大や NaN は使えません）").arg(names.value(m.captured(1)));
        }));
        r.push_back(rule(R"(points needs \[x_mm, y_mm\])", "点は [x, y]（mm）の 2 つの数で指定します"));
        // M5 (core/ops_studio.cpp, render/placed.cpp, render/screentone.cpp): the studio's refusals in the C++ build only
        // (SPEC COMP-01a)
        r.push_back(rule(R"((placement_mm|face_box01|pad_mm|rect_mm|poly|border_mm|curves|corner_mm)(?:\.\w+)? must be a finite number)",
                         [](const QRegularExpressionMatch& m) {
                             static const QHash<QString, QString> names = {
                                 {"placement_mm", "画像の置き場所"}, {"face_box01", "顔の位置"}, {"pad_mm", "画像の余白"},
                                 {"rect_mm", "コマや領域の位置と大きさ"}, {"poly", "コマの角の位置"}, {"border_mm", "枠線の太さ"},
                                 {"curves", "コマの辺のふくらみ"}, {"corner_mm", "コマの角の丸み"}};
                             return QStringLiteral("%1は、ふつうの数で指定します（無限大や NaN は使えません）").arg(names.value(m.captured(1)));
                         }));
        r.push_back(rule(R"(each of the bible's characters must be an object, not .*)",
                         "設定資料の登場人物は、一人ずつ項目のまとまり（オブジェクト）で書きます"));
        r.push_back(rule(R"(the asset's picture cannot be read \((.*)\))", "素材の画像を読み込めません（壊れているか、読めない形式の画像です）"));
        r.push_back(rule(R"(the screen is too coarse for this resolution.*)",
                         "網点が粗すぎて、この解像度では作れません（線数を上げるか、解像度を下げてください）"));
        r.push_back(rule(R"(a count past .* is too large for this build.*)", "数が大きすぎて、この版の Genko では数えられません"));
        // M5③ (api/mcp_tools.cpp): an MCP tool's arguments with a number the C++ build cannot carry (SPEC COMP-01a)
        r.push_back(rule(R"(the arguments hold a number this build does not take \(NaN, Infinity or an integer past 64 bits\) at (.*))",
                         "引数に、この版の Genko では扱えない数（NaN・Infinity・64 ビットを超える整数）があります（\\1。原稿は変わりません）"));
        r.push_back(rule(R"(the finish's (\w+) is not a number)", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names = {
                {"black", "ベタにする濃さ"}, {"white", "紙の白にする明るさ"}, {"line_threshold", "線とみなす濃さ"},
                {"line_contrast", "線とみなす周りとの差"}, {"lpi", "網点の線数"}, {"angle", "網点の角度"},
                {"face_light", "顔を明るくする度合い"}, {"smooth", "絵のざらつきのならし"}};
            return QStringLiteral("仕上げの%1は数で指定します（この絵は描けません）").arg(names.value(m.captured(1), m.captured(1)));
        }));
        // TEXT-01 (core/ruby.cpp): the rubies by place and their settings, refused in the C++ build only (SPEC COMP-01a)
        r.push_back(rule(R"(ruby_spans is a list of \{start, end, ruby\}.*)", "ルビは {start, end, ruby}（本文の書記素の位置と読み）の並びで指定します"));
        r.push_back(rule("a ruby span has start, end, ruby, base, review, font, scale, align, gap_mm and tracking, not .*",
                         "ルビに知らない項目があります（start・end・ruby・base・review と、組み方の font・scale・align・gap_mm・tracking だけ）"));
        r.push_back(rule("a ruby span's review must be edited, ambiguous or unplaced", "ルビの要確認の理由は edited・ambiguous・unplaced のどれかです"));
        r.push_back(rule(R"(a ruby span must be \[start, end\) of whole numbers.*)",
                         "ルビの位置が本文の外にあるか、整数ではありません（start は end より前、end は本文の字数まで）"));
        r.push_back(rule("a ruby span needs its reading.*", "ルビの読みが要ります"));
        r.push_back(rule("a ruby span's reading must be one line", "ルビの読みは 1 行で書きます"));
        r.push_back(rule("a ruby span's base must be a text", "ルビの親文字は文字で指定します"));
        r.push_back(rule("a ruby span's base .* is not the text at .*", "ルビの親文字が、その位置の本文と違います"));
        r.push_back(rule("a ruby span that needs review keeps its base.*", "要確認のルビには、元の親文字が要ります"));
        r.push_back(rule("rubies overlap: .*", "ルビの親文字が重なっています（ひとつの字に振れるルビはひとつです）"));
        r.push_back(rule("give the rubies as ruby_spans or as ruby_runs, not both", "ルビは ruby_spans か ruby_runs のどちらかで指定します"));
        r.push_back(rule("a ruby's font must be .*", "ルビの書体は、書体の名前（gothic・mincho など）か書体のファイルで指定します"));
        r.push_back(rule("a ruby's scale must be between 0.25 and 0.8", "ルビの大きさは、本文の 0.25〜0.8 字の間の数で決めます"));
        r.push_back(rule("a ruby's align must be head, center or tail", "ルビの揃えは、先頭（head）・中央（center）・末尾（tail）から選びます"));
        r.push_back(rule("a ruby's gap_mm must be between -1 and 10", "ルビと本文の距離は -1〜10 mm の間の数で決めます"));
        r.push_back(rule("a ruby's tracking must be between -0.5 and 2", "ルビの字間は -0.5〜2 字の間の数で決めます"));
        r.push_back(rule(R"(the official export stops: (\d+) ruby readings? needs? review.*)",
                         "要確認のルビが \\1 つあるため、正式な書き出しはできません。台詞の欄で振り直すか外してください"));
        // core/pyre's own bounds (SPEC COMP-01a: Python searches on, or stops with RecursionError)
        r.push_back(rule("the pattern takes too long to search.*",
                         "この正規表現は探すのに時間がかかりすぎるため、置き換えませんでした（原稿は変わりません）。書き方を見直してください"));
        r.push_back(rule(R"(the pattern nests too deeply \(groups and look-arounds at most (\d+) levels.*)",
                         "正規表現のかっこの入れ子が深すぎます（\\1 段まで）"));
        r.push_back(rule("gradient_fill needs from and to.*", "グラデーションは、始めと終わりの点で指定します"));
        // (groups@1, PC5 I-b: what this build does not do yet with groups — before the general words below)
        r.push_back(rule("not yet ported: group_nesting",
                         "グループの入れ子が深く、一度に描けません（不透明度・描画モード・マスクのあるグループの入れ子を減らすか、解像度を下げてください）"));
        r.push_back(rule("not yet ported: merging what shows through groups on a page in precise colour.*",
                         "高精度のページでは、グループを通して見えるレイヤーの結合・画像の統合はまだできません"
                         "（そのグループの「グループとして描く」を外すと結合できます。見え方は変わります）"));
        // what this build does not do yet (render::NotYetPorted, the studio's official export)
        r.push_back(rule("not yet ported: (.*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("この版の Genko では、まだ扱えないもの（%1）があるため、できませんでした（原稿は変わりません）").arg(unported_element(m.captured(1)));
        }));
        // PSD-01 (formats/psd_text.hpp): the strict editable PSD refused (C++ only, SPEC COMP-01a)
        r.push_back(rule(R"(the editable PSD stops: (\d+) lines? uses? what PSD text cannot keep.*)",
                         "PSD の文字に表せない装飾のある台詞が \\1 つあるため、編集可能な PSD は書き出しませんでした（その台詞だけ画像にする「見た目優先」を選ぶと書き出せます）"));
        r.push_back(rule(R"(official export is not in the C\+\+ build yet)",
                         "この版の Genko では、まだ正式な書き出し（点検と書き出しの承認の記録）はできません。正式な書き出しを外すと書き出せます"));
        // the exports' bounds (formats::check_dpi, render_page's and the strip's size)
        r.push_back(rule("the resolution must be between 1 and 100000 dpi", "解像度は 1〜100000 dpi の間で決めます"));
        r.push_back(rule(R"(.*?; the file that was there is kept as '(.*)')", [](const QRegularExpressionMatch& m) {
            // (one clause for each file that could not be put back)
            const QStringList kept = m.captured(1).split(QStringLiteral("'; the file that was there is kept as '"));
            return QStringLiteral("書き出したファイルを置けず、前からあったファイルも元の場所に戻せませんでした。前のファイルは「%1」にあります")
                .arg(kept.join(QStringLiteral("」「")));
        }));
        r.push_back(rule("the (?:page|picture|strip) is too large at this resolution", "この解像度では絵が大きすぎて扱えません（解像度を下げてください）"));
        r.push_back(rule("the gradient needs a longer drag", "もう少し長くドラッグします"));
        r.push_back(rule("the book's pages are still being read: try again in a moment",
                         "原稿の残りのページを読み込み中です。読み込みが終わってから、もう一度操作してください"));
        r.push_back(rule("the gradient has nothing to show there", "そこにはグラデーションを塗れる所がありません"));
        r.push_back(rule("kind must be box, cylinder, stairs or floor.*", "3D の形は、箱・円柱・階段・床から選びます"));
        r.push_back(rule("scene kind must be one of .*", "背景の 3D は、部屋・教室・廊下・街並みから選びます"));
        r.push_back(rule("shape must be one of .*", "図形は、直線・折れ線・曲線・長方形・楕円・多角形から選びます"));
        r.push_back(rule(R"(box \[x, y, w, h\] is required)", "図形の大きさ（box）が要ります"));
        r.push_back(rule(R"(points needs at least two \[x, y\])", "線を引くには 2 点以上が要ります"));
        r.push_back(rule("no saved area .*", "その名前の選択範囲は残っていません"));
        r.push_back(rule("name is required", "名前が要ります"));
        r.push_back(rule("brush texture must be .*", "ブラシの質感は、なし・鉛筆・エアブラシ・かすれ・水彩から選びます"));
        r.push_back(rule("an image tip needs its picture.*", "画像の先端には画像が要ります（画像から先端を作る）"));
        r.push_back(rule("brush aa must be .*", "アンチエイリアスは、なし・弱・中・強から選びます"));
        r.push_back(rule("brush pattern must be .*", "模様は、点・破線・レース・草・ハート・星・葉から選びます"));
        r.push_back(rule("brush tip must be .*", "先端の形は、丸・平たい・画像から選びます"));
        r.push_back(rule("mode must be blur, smudge or blend", "色混ぜは、ぼかし・指先・なじませから選びます"));
        r.push_back(rule("mode must be cut, to_crossing or whole", "消し方は、触れた所・交点まで・線全体から選びます"));
        r.push_back(rule("the brush is off the page", "ページの外です"));
        r.push_back(rule("action must be one of .*", "線の編集は、点の移動・追加・削除、つなぐ・切る・色を変える・消すから選びます"));
        r.push_back(rule(R"(ids \(or stroke_id\) is required)", "線を選んでください"));
        r.push_back(rule("connect takes two line ids", "つなぐ線を 2 本選びます"));
        r.push_back(rule("no stroke .*", "その線はありません"));
        r.push_back(rule("index is not a point of the line", "その点は線にありません"));
        r.push_back(rule("a line keeps at least two points.*", "線には点が 2 つ要ります（線ごと消すときは「線を消す」）"));
        r.push_back(rule("that is the end of the line", "線の端では切れません"));
        r.push_back(rule("the layer has no colour yet.*", "このレイヤーにはまだ色がありません（先に塗ってから塗り残しを塗ります）"));
        r.push_back(rule("there is nothing on this layer to blend there", "そこにはこのレイヤーの色がないので、混ぜられません"));
        r.push_back(rule("a guide's axis is h or v", "ガイド線の向きは横（h）か縦（v）です"));
        r.push_back(rule("a folder cannot hold itself", "フォルダを自分の中には入れられません"));
        r.push_back(rule("a multi_curve ruler needs points2.*", "多重曲線定規には 2 本目の曲線が要ります"));
        r.push_back(rule("a path needs two points or more", "文字を沿わせるパスには 2 点以上が要ります"));
        r.push_back(rule("a radial_curve ruler needs its center", "放射曲線定規には中心が要ります"));
        r.push_back(rule("an edge cannot bow more than half its length", "辺はその長さの半分より大きくは曲げられません"));
        r.push_back(rule("an image tone needs its picture.*", "画像の柄トーンには、柄にする画像が要ります"));
        r.push_back(rule("bumps must be between 5 and 60", "雲のふくらみの数は 5〜60 です"));
        r.push_back(rule("curves needs one number per edge.*", "辺の曲がりは、辺の数だけ数を並べます"));
        r.push_back(rule(R"(edge must be 0\.\..*)", "その辺はありません"));
        r.push_back(rule("gradient is .*", "文字のグラデーションは、上の色と下の色で決めます"));
        r.push_back(rule("line kind must be one of .*", "枠線の種類は、実線・二重線・破線・点線・手描き風から選びます"));
        r.push_back(rule(R"(scale_mm is 0\.3 to 50)", "柄の大きさは 0.3〜50 mm です"));
        r.push_back(rule(R"(scale_x must be between 0\.3 and 3)", "長体・平体は 0.3〜3 です"));
        r.push_back(rule("screen is .*", "トーン化の設定は、線数・角度・模様です"));
        r.push_back(rule("screen pattern must be .*", "トーン化の模様は、網点・線・交差・砂目から選びます"));
        r.push_back(rule("(?:screen shape|dot_shape) must be .*", "網の形は、丸・四角・ひし形・楕円から選びます"));
        r.push_back(rule(R"(offset_mm is \[x, y\] in mm)", "網のずれは、右と下へ何ミリかの 2 つの数で決めます"));
        r.push_back(rule("screen lpi must be between 10 and 150", "網点の線数は 10〜150 です"));
        r.push_back(rule("a tone curve needs two points or more", "トーンカーブには点が 2 つ以上いります"));
        r.push_back(rule("a .* ruler needs a box with some size.*", "長方形・楕円の定規は、大きさのある四角（対角の 2 点）で置きます"));
        r.push_back(rule(R"(grid is 0 \(none\) to 60 lines)", "パースのグリッドの線の数は 0〜60 です（0 で消す）"));
        r.push_back(rule("no 3D on this page to match.*", "このページに合わせる 3D がありません"));
        r.push_back(rule(R"(no 3D \S+$)", "その 3D はこのページにありません"));
        r.push_back(rule("the 3D is seen straight on.*", "3D を真正面から見ているので、線が平行のままで消失点がありません"));
        r.push_back(rule("the camera follows a perspective ruler", "カメラを合わせられるのはパース定規です"));
        r.push_back(rule("the perspective ruler has no vanishing points", "そのパース定規には消失点がありません"));
        r.push_back(rule("taper is in, out, both or false", "入り抜きは、入り・抜き・両方・なしから選びます"));
        r.push_back(rule("bundle is 1 to 50 lines", "まとまりは 1 束 1〜50 本です"));
        r.push_back(rule("a hand is a pose .*", "手は、決まった形か、指ごとの曲がり（親指・人差し指・中指・薬指・小指、0 まっすぐ〜1 曲げきる）で決めます"));
        r.push_back(rule("body sex is male or female", "体型は男性か女性です"));
        r.push_back(rule("ik moves .*", "引いて腕・脚ごと動かせる（IK）のは、手首・手・足首・つま先です"));
        r.push_back(rule("prop must be one of .*", "小物は、椅子・机・テーブル・ベッド・ドア・窓・棚・車から選びます"));
        r.push_back(rule("a pose needs a name", "ポーズには名前がいります"));
        r.push_back(rule("despeckle what must be .*", "ゴミ取りで取るものは、黒い点・白い穴・両方から選びます"));
        r.push_back(rule("spike_jitter must be between 0 and 1", "トゲの乱れは 0〜1 です"));
        r.push_back(rule("tail kind must be one of .*", "しっぽの形は、くさび・ギザギザ・消える・泡から選びます"));
        r.push_back(rule("the ruler is fixed.*", "この定規は固定されています（先に固定を外します）"));
        r.push_back(rule("this ruler has no line to draw.*", "この定規には描ける線がありません（向きだけの定規です）"));
        r.push_back(rule("unknown feature .*", "その字形の指定（フォントの機能）は使えません"));
        r.push_back(rule("warp is four corners.*", "文字の変形は、4 隅の位置を並べて決めます"));
        r.push_back(rule("3D .* exists", "その名前の 3D はもうあります"));
        r.push_back(rule(".* cannot be repeated page by page", "その操作はページごとには繰り返せません（ページそのものや本全体を変える操作です）"));
        r.push_back(rule("find is the words to look for", "探す言葉が要ります"));
        r.push_back(rule(R"(flap_mm is 0\.\.200 mm)", "袖の幅は 0〜200 mm です"));
        r.push_back(rule("nothing matched", "見つかりませんでした"));
        r.push_back(rule("ops is the list of ops to run on each page", "各ページで行う操作の一覧が要ります"));
        r.push_back(rule("pages is the list of pages", "ページを選んでください"));
        r.push_back(rule("spine_mm is the spine's width.*", "背幅（0〜100 mm）が要ります"));
        r.push_back(rule("height_mm is the band's height.*", "帯の高さは 15〜200 mm です"));
        r.push_back(rule("only a person can let approvals come through a chat", "チャットでの承認を許せるのは人だけです"));
        r.push_back(rule("kind must be front, back, jacket or obi", "表紙・裏表紙・カバー・帯から選びます"));
        r.push_back(rule(R"((\w+) is a frame number)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1はフレームの番号で指定します").arg(field(m.captured(1)));
        }));
        r.push_back(rule(R"(cels is a list of \[frame, cel id or null\])", "セルの指定は［フレーム, セル］の並びです"));
        r.push_back(rule("folder is an animation folder's id.*", "アニメーションフォルダーを指定します（先にアニメーションフォルダーを作る）"));
        r.push_back(rule(R"(page (\d+) is not an animation.*)", "\\1 ページはアニメーションではありません（先にタイムラインでアニメーションにする）"));
        r.push_back(rule("the page is not an animation.*", "このページはアニメーションではありません（先にタイムラインでアニメーションにする）"));
        r.push_back(rule(R"(rect is \[x, y, width, height\] in mm)", "カメラの範囲は［左, 上, 幅, 高さ］（mm）で指定します"));
        r.push_back(rule("cel must be a layer in the animation folder", "セルはそのアニメーションフォルダーの中のレイヤーです"));
        r.push_back(rule("no plugin (.*)", "プラグイン「\\1」が見つかりません（プラグインのフォルダーに入れます）"));
        r.push_back(rule("plugin (.*?) cannot be loaded.*", "プラグイン「\\1」を読み込めません"));
        r.push_back(rule(R"(plugin (.*?) has no run\(image\))", "プラグイン「\\1」に処理（run）がありません"));
        r.push_back(rule("plugin (.*?) failed.*", "プラグイン「\\1」の処理が失敗しました"));
        r.push_back(rule("plugin (.*?) did not return a picture", "プラグイン「\\1」が絵を返しませんでした"));
        r.push_back(rule("plugins need Python on this computer.*", "プラグインを使うには、このパソコンに Python が要ります（レイヤー → プラグインの設定で選べます）"));
        r.push_back(rule("plugins need Pillow in the Python that runs them.*", "プラグインを動かす Python に Pillow が要ります（pip install Pillow）"));
        r.push_back(rule("plugin (.*?) is not chosen to run.*", "プラグイン「\\1」は使う設定になっていません（レイヤー → プラグインの設定で選びます）"));
        r.push_back(rule("plugin (.*?) could not be started.*", "プラグイン「\\1」を動かす Python を起動できませんでした（プラグインの設定で Python を確かめてください）"));
        r.push_back(rule("plugin (.*?) stopped before it answered.*", "プラグイン「\\1」が途中で止まりました（落ちたか、メモリが足りなかった可能性があります）"));
        r.push_back(rule(R"(plugin (.*?) did not finish in (\d+) seconds)", "プラグイン「\\1」が \\2 秒で終わらなかったため止めました"));
        r.push_back(rule("plugin (.*?) wrote back more than a picture", "プラグイン「\\1」が絵より多くのデータを返したため止めました"));
        r.push_back(rule("plugin (.*?) changed since the list was shown.*", "プラグイン「\\1」は一覧を出した後に変わっています。確かめてから選び直してください"));
        // native plugins (render/plugins.hpp, docs/PLUGIN_NATIVE.md): their plugin.json, their program
        r.push_back(rule(R"(the plugin\.json of plugin (.*?) cannot be used \((.*)\))", [](const QRegularExpressionMatch& m) {
            static const std::vector<std::pair<QRegularExpression, QString>> reasons = [] {
                std::vector<std::pair<QRegularExpression, QString>> out;
                const auto add = [&out](const char* pattern, const char* words) {
                    out.emplace_back(QRegularExpression(QRegularExpression::anchoredPattern(QString::fromUtf8(pattern))), QString::fromUtf8(words));
                };
                add(R"(a Python plugin (.*) has the same name)", "同じ名前の Python のプラグイン（\\1）があります");
                add("it is not a file in the plugin's folder", "plugin.json がプラグインのフォルダーの中のファイルではありません");
                add("it cannot be read", "plugin.json を読めません");
                add("it is larger than 64 KB", "plugin.json が 64 KB を超えています");
                add("it is not JSON", "plugin.json が JSON ではありません");
                add("it is not a JSON object", "plugin.json が JSON のオブジェクトではありません");
                add("its protocol must be 1", "protocol は 1 にします");
                add("its name must be a text", "name（名前）を文字で書きます");
                add("its args must be a list of texts", "args は文字のリストにします");
                add("its params must be an object", "params はオブジェクトにします");
                add("its executable must be a path inside the plugin's folder", "executable はプラグインのフォルダーの中のファイルにします");
                add(R"(its program (.*) is not in its folder)", "プログラム「\\1」がプラグインのフォルダーにありません");
                add(R"(its program (.*) is a link)", "プログラム「\\1」がリンクです（プラグインのフォルダーの中のファイルにします）");
                add(R"(its program (.*) cannot be read)", "プログラム「\\1」を読めません（読める権限が要ります）");
                add("its folder is a link", "プラグインのフォルダーがリンクです（設定のフォルダーの plugins の中に置きます）");
                add("its program is larger than 256 MB", "プログラムが 256 MB を超えています");
                return out;
            }();
            QString why = m.captured(2);
            for (const auto& [pattern, words] : reasons) {
                if (const auto found = pattern.match(why); found.hasMatch()) {
                    why = words.contains(QLatin1Char('\\')) ? expand(words, found) : words;
                    break;
                }
            }
            return QStringLiteral("プラグイン「%1」の plugin.json が使えません（%2）").arg(m.captured(1), why);
        }));
        r.push_back(rule("the program of plugin (.*?) could not be started.*",
                         "プラグイン「\\1」のプログラムを起動できませんでした（このコンピューターで動くプログラムか確かめてください）"));
        r.push_back(rule("the program of plugin (.*?) could not be copied to run it.*",
                         "プラグイン「\\1」のプログラムを、動かすために設定のフォルダーへ写せませんでした（空きや権限を確かめてください）"));
        r.push_back(rule(R"(programs cannot run from Genko's settings folder on this computer \((.*?): .*\), so plugin (.*) cannot run)",
                         "このコンピューターでは Genko の設定のフォルダー（\\1）の中でプログラムを動かせないため、プラグイン「\\2」を動かせませんでした"));
        r.push_back(rule("CMYK is written as TIFF or PDF", "CMYK は TIFF か PDF で書き出します"));
        // (color-model@1: set_color_mode, assign_profile, convert_profile — the C++ build's own)
        r.push_back(rule("mode is rgb, gray, cmyk, lab or multichannel", "色モードは RGB・グレースケール・CMYK・Lab・マルチチャンネルのどれかです"));
        // (color-model@1, PC1d: to_indexed, set_color_table, to_bitmap, set_duotone, set_color_mode's multichannel and a
        // colour plate's delete_channel — the C++ build's own)
        r.push_back(rule(R"(page (\d+) is a (bitmap|multichannel) page: Indexed Color takes .*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1 ページは%2のページなので、インデックスカラーにできません（RGB・グレー・ダブルトーン・CMYK・Lab のページからします）")
                .arg(m.captured(1), m.captured(2) == QStringLiteral("bitmap") ? QStringLiteral("2 階調") : QStringLiteral("マルチチャンネル"));
        }));
        r.push_back(rule(R"(the book's default mode is (bitmap|multichannel): Indexed Color takes another)",
                         "原稿の新しいページの色モードが 2 階調かマルチチャンネルなので、インデックスカラーにできません"));
        r.push_back(rule("palette is selective, .*", "パレットは、ローカル（特定）・ローカル（知覚的）・ローカル（割り付け）・正確（256 色まで）・システム（Mac OS）・システム（Windows）・Web・均等のどれかです"));
        r.push_back(rule("forced is blackWhite, none, primaries or web", "強制する色は、白黒・なし・原色・Web のどれかです"));
        r.push_back(rule("dither is diffusion, none, pattern or noise", "ディザは、拡散・なし・パターン・ノイズのどれかです"));
        r.push_back(rule("colors is a whole number from 2 to 256", "色数は 2〜256 の整数です"));
        r.push_back(rule("amount is a number from 0 to 100", "ディザの量は 0〜100 です"));
        r.push_back(rule(R"(page \d+: the image has (\d+) colours; Exact needs 256 or fewer)",
                         "画像に \\1 色あるので、「正確」のパレットにできません（256 色までです。ほかのパレットを選んでください）"));
        r.push_back(rule(R"(page (\d+) is not an indexed page \(to_indexed first\))", "\\1 ページはインデックスカラーのページではありません（先にインデックスカラーにします）"));
        r.push_back(rule(R"(no page is an indexed page \(to_indexed first\))", "インデックスカラーのページがありません（先にインデックスカラーにします）"));
        r.push_back(rule("table is custom, .*", "カラーテーブルは、カスタム・黒体・グレースケール・スペクトル・システム（Mac OS）・システム（Windows）・Web のどれかです"));
        r.push_back(rule(R"(colors is a list of 1\.\.256 colours.*)", "カラーテーブルの色は 1〜256 色です（#rrggbb か [r, g, b]）"));
        r.push_back(rule(R"(entry (.*) is not an index of the table \((\d+) colours\))", "\\1 はカラーテーブルの番号ではありません（\\2 色あります）"));
        r.push_back(rule(R"(entry (\d+): a colour is .*)", "\\1 番の色が読めません（#rrggbb か [r, g, b]）"));
        r.push_back(rule("transparent is an index of the table or null", "透明にする色は、カラーテーブルの番号か「なし」です"));
        r.push_back(rule("method is diffusion, threshold, pattern or halftone", "2 階調の方法は、誤差拡散法・50% を基準に 2 階調・パターンディザ・ハーフトーンスクリーンのどれかです"));
        r.push_back(rule("shape is round, ellipse, line, square, diamond or cross", "網点の形は、円・楕円・線・正方形・ひし形・十字のどれかです"));
        r.push_back(rule("frequency is a number from 1 to 999", "線数は 1〜999 です"));
        r.push_back(rule("angle is a number from -180 to 180", "角度は -180〜180 です"));
        r.push_back(rule(R"(page (\d+) is not a grey page: Bitmap takes a grey page.*)",
                         "\\1 ページはグレースケールのページではないので、2 階調にできません（先にグレースケールにします）"));
        r.push_back(rule("the book's default mode is not grey: Bitmap takes a grey page", "原稿の新しいページの色モードがグレースケールではないので、2 階調にできません"));
        r.push_back(rule("type is monotone, duotone, tritone or quadtone", "種類は、モノトーン・ダブルトーン・トライトーン・クワッドトーンのどれかです"));
        r.push_back(rule(R"(an ink's color is .*)", "インキの色が読めません（#rrggbb か [r, g, b]）"));
        r.push_back(rule(R"(an ink's curve is .*)", "インキのカーブが読めません（0〜100 の [入力, 出力] の並び）"));
        r.push_back(rule(R"(an ink's name is text|inks is \[.*)", "インキの指定が読めません（名前・色・カーブ）"));
        r.push_back(rule(R"(the page's colour mode cannot be read: a duotone ink's name is .*)", "インキの名前は 1〜64 字で、前後に空白を置きません"));
        r.push_back(rule(R"(the page's colour mode cannot be read: a curve has 2\.\.13 points)", "カーブの点は 2〜13 個です"));
        r.push_back(rule(R"(the page's colour mode cannot be read: a curve's (?:points are 0\.\.100|inputs go up.*))",
                         "カーブの点は 0〜100 で、入力は順に大きくします"));
        r.push_back(rule(R"(page (\d+) is not a grey page: Duotone takes a grey or duotone page.*)",
                         "\\1 ページはグレースケールのページではないので、ダブルトーンにできません（先にグレースケールにします）"));
        r.push_back(rule("the book's default mode is not grey: Duotone takes a grey or duotone page",
                         "原稿の新しいページの色モードがグレースケールではないので、ダブルトーンにできません"));
        r.push_back(rule(R"(page (\d+) is an? (indexed|bitmap) page: convert it to rgb or gray first.*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1 ページは%2のページなので、マルチチャンネルにできません（先に RGB かグレースケールにします）")
                .arg(m.captured(1), m.captured(2) == QStringLiteral("bitmap") ? QStringLiteral("2 階調") : QStringLiteral("インデックスカラー"));
        }));
        r.push_back(rule(R"(page (\d+) is an RGB page of a profile: convert it to sRGB first.*)",
                         "\\1 ページはプロファイル付きの RGB のページなので、マルチチャンネルにできません（先に sRGB に変換します）"));
        r.push_back(rule(R"(the book's default mode is an? (?:indexed|bitmap) page: .*|the book's default mode is an RGB page of a profile: .*)",
                         "原稿の新しいページの色モードからはマルチチャンネルにできません"));
        r.push_back(rule(R"(page (\d+): the layer (.*)'s blend '(.*)' is not one a multichannel page composites)",
                         "\\1 ページのレイヤー \\2 の描画モード（\\3）はマルチチャンネルのページでは合成できません（色相・彩度・カラー・輝度は使えません）"));
        r.push_back(rule(R"(page (\d+) is an indexed page: it holds 8-bit pixels only \(the layer (.*) holds precise colour\))",
                         "\\1 ページはインデックスカラーのページなので、高精度（16・32 bit）の画素を置けません（8 bit の画素だけです。レイヤー \\2）"));
        r.push_back(rule(R"(page (\d+) is an indexed page: its colours are sRGB's)", "\\1 ページはインデックスカラーのページなので、プロファイルを指定できません（sRGB の色です）"));
        r.push_back(rule(R"(page (\d+) is a multichannel page: its plates have no profile)", "\\1 ページはマルチチャンネルのページなので、プロファイルを指定できません"));
        r.push_back(rule(R"(page (\d+) is an? (indexed|bitmap|duotone|multichannel) page: convert_profile takes .*)", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names{{"indexed", "インデックスカラー"}, {"bitmap", "2 階調"}, {"duotone", "ダブルトーン"},
                                                       {"multichannel", "マルチチャンネル"}};
            return QStringLiteral("%1 ページは%2のページなので、プロファイルに変換できません（先に色モードを変えます）").arg(m.captured(1), names.value(m.captured(2)));
        }));
        r.push_back(rule(R"(page (\d+) has only one colour plate to keep)", "\\1 ページの色の版は 1 つしかないので、消せません"));
        r.push_back(rule("a multichannel page keeps one plate at least", "マルチチャンネルのページには版が 1 つ以上要ります"));
        r.push_back(rule(R"(a colour plate is deleted only with to_multichannel: true.*)",
                         "色の版を消すとページがマルチチャンネルになります（to_multichannel: true で消します）"));
        r.push_back(rule("depth is 8, 16 or 32", "ビット数は 8・16・32 のどれかです"));
        r.push_back(rule(R"(page \d+: the layer (.*) holds HDR values past 0\.\.1, which cannot be converted as they are \(clip_hdr: true cuts them and converts\))",
                         "レイヤー \\1 に HDR の値（0〜1 の外）があるので、このままでは変換できません（clip_hdr: true で切って変換します）"));
        r.push_back(rule(R"(the profile is for another colour space \((\w+)\).*)", "プロファイルの色空間（\\1）がページの色モードと合いません"));
        r.push_back(rule("a CMYK page needs its ICC profile.*", "CMYK のページにはカラープロファイルが要ります（内蔵の coated-cmyk か、ファイルを選びます）"));
        r.push_back(rule("a Lab page has no ICC profile", "Lab のページにはカラープロファイルを指定しません"));
        r.push_back(rule(R"(page (\d+) is a Lab page: it has no profile)", "\\1 ページは Lab のページなので、プロファイルを指定できません"));
        r.push_back(rule(R"(page (\d+) is a CMYK page: none is for an RGB or grey page)", "\\1 ページは CMYK のページなので、プロファイルを外せません"));
        r.push_back(rule("no page is of the profile's colour space", "プロファイルの色空間に合うページがありません"));
        r.push_back(rule(R"(no built-in profile .*)", "その内蔵プロファイルはありません"));
        r.push_back(rule(R"(the book has no ICC profile .*)", "原稿にそのカラープロファイルがありません（ファイルから選んでください）"));
        r.push_back(rule(R"(the ICC profile cannot be used: .*)", "そのカラープロファイルは使えません"));
        r.push_back(rule(R"(the precise pixels cannot be kept: .*)", "高精度の画素が原稿の上限（1 GiB）を超えるため、変換できません"));
        r.push_back(rule(R"(page (\d+): the layer (.*?) (?:is|holds|keeps|has) .*: a page in precise colour \(16 or 32 bits, or of a profile\) cannot draw it yet)",
                         "\\1 ページのレイヤー \\2 は、16・32 ビットやプロファイル付きのページではまだ描けません（描画モードとレイヤーの種類を確かめてください）"));
        // (PC2d: 取り込む画像のプロファイル, render/import_profile)
        r.push_back(rule("embedded_profile is keep, convert or assign", "画像のプロファイルの扱いは keep・convert・assign のどれかです"));
        r.push_back(rule(R"(embedded_profile assign needs the profile \(profile or icc\))", "割り当てるプロファイル（profile か icc）を指定してください"));
        r.push_back(rule(R"(the profile is not one for the picture's colours.*)", "そのプロファイルは画像の色の空間のものではありません"));
        r.push_back(rule(R"(embedded_profile assign takes an RGB or grey PSD.*)", "プロファイルを割り当てられるのは RGB かグレーの PSD だけです（CMYK・Lab の PSD は自分のプロファイルで取り込みます）"));
        r.push_back(rule("Lab PSD files are not supported: save it as RGB or CMYK", "Lab カラーの PSD は読めません。RGB か CMYK で保存し直してください"));
        r.push_back(rule("MP4 needs ffmpeg on this computer; WebP, GIF and PNG need nothing",
                         "MP4 にはこのパソコンに ffmpeg が要ります（WebP・GIF・PNG なら何も要りません）"));
        r.push_back(rule(R"(PSD depth (\d+) is not supported)", "\\1 ビットの PSD は読めません"));
        r.push_back(rule("broken PSD layer record", "PSD のレイヤーの情報が壊れています"));
        r.push_back(rule("color must be (?:auto, )?rgb, cmyk(?:, gray)? or (?:gray|bitonal)", "色は自動・RGB・CMYK・グレー・2 階調のどれかです"));
        r.push_back(rule(R"(ffmpeg failed: it did not finish in (\d+) minutes)", "動画にできませんでした（ffmpeg が \\1 分で終わらなかったため止めました）"));
        r.push_back(rule("ffmpeg failed: it could not be started", "動画にできませんでした（ffmpeg を起動できませんでした）"));
        r.push_back(rule("ffmpeg failed: (.*)", "動画にできませんでした（\\1）"));
        r.push_back(rule("format must be one of (.*)", "形式は \\1 のどれかです"));
        r.push_back(rule(R"(import_psd needs path or psd \(base64\))", "読み込む PSD のファイルを指定します"));
        r.push_back(rule("not a PSD file", "PSD のファイルではありません"));
        r.push_back(rule("nothing has been recorded yet.*", "まだ記録がありません（ファイル → タイムラプスを記録する をオンにして描くと記録されます）"));
        r.push_back(rule("psd is the file's bytes in base64", "PSD のデータが読めません"));
        r.push_back(rule(R"(the PSD cannot be read \((.*)\))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("PSD を読み込めません（%1）").arg(inner(m.captured(1)));
        }));
        r.push_back(rule("the PSD file is cut short", "PSD のファイルが途中で切れています"));
        r.push_back(rule("fit must be paper, bleed or trim", "PSD を合わせる範囲は、用紙・裁ち落とし・仕上がりから選びます"));
        // (what Python's reader says of a file it cannot read, in the PSD's refusal: numpy's, struct's and zlib's
        // words; zlib's error itself goes through import_psd as "error: …")
        r.push_back(rule("not enough image data", "絵のデータが足りません"));
        r.push_back(rule("buffer size must be a multiple of element size", "絵のデータの長さが合いません"));
        r.push_back(rule(R"(cannot reshape array of size \d+ into shape \(.*\))", "絵のデータの量が、絵の大きさと合いません"));
        r.push_back(rule(R"(unpack requires a buffer of \d+ bytes)", "ファイルが途中で切れています"));
        r.push_back(rule(R"(Error -?\d+ while decompressing data.*)", "圧縮された絵のデータが壊れています"));
        r.push_back(rule(R"(error: Error -?\d+ while decompressing data.*)", "PSD を読み込めません（圧縮された絵のデータが壊れています）"));
        // (this build's own refusals of a PSD too large to read: render/psd.hpp, render/import_psd.cpp)
        r.push_back(rule(R"(the PSD has too many layers for their size \(.*\))",
                         "PSD のレイヤーが、絵の大きさに対して多すぎて読み込めません（レイヤーを結合するか、解像度を下げて保存し直してください）"));
        r.push_back(rule("a layer is too large to read", "レイヤーが大きすぎて読めません"));
        r.push_back(rule("the layers are too large to read", "レイヤーの絵が多すぎて読めません"));
        r.push_back(rule("the picture is too large to read", "絵が大きすぎて読めません"));
        r.push_back(rule(R"(the page layer is too large \(.*\))", "用紙の大きさに対して PSD の絵が大きすぎて、レイヤーにできません"));
        // (PC4 F-f: a .pcraft read or written — render/pcraft_read, formats/pcraft_write; the op smart_place's "the .pcraft
        // cannot be read (…)" around them)
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft is broken \(.*\)\)?)", ".pcraft のファイルが壊れているか途中で切れていて、読めません"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft was written by a newer PhotoCraft \(format (\d+); this build reads format (\d+)\)\)?)",
                         "新しい PhotoCraft で書かれた .pcraft なので読めません（形式 \\1。この版の Genko が読めるのは形式 \\2 です）"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft unpacks to more than this build reads at once \(.*\)\)?)",
                         ".pcraft の中身が大きすぎて、一度に読めません（展開して 8 GiB まで）"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft holds a (?:blob|file) too large to read \(.*\)\)?)",
                         ".pcraft の中に、大きすぎて読めないファイルがあります"));
        // (PC4 F-f review: the read's budgets — its layers, the pixels it makes, what the book keeps, the documents inside)
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft holds more layers than this build reads \(at most (\d+)\)\)?)",
                         ".pcraft のレイヤーが多すぎて読めません（\\1 枚まで）"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft holds more pixels than this build reads at once \(.*\)\)?)",
                         ".pcraft のレイヤーとマスクの画素が多すぎて、一度に読めません"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft makes a book larger than this build holds.*)",
                         ".pcraft の画素とファイルが大きすぎて、本に入りません（作業に使える記憶の上限を超えます）"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft holds more documents inside it than this build reads \(at most (\d+)\)\)?)",
                         ".pcraft の中の .pcraft が多すぎて読めません（\\1 個まで）"));
        r.push_back(rule(R"((?:the \.pcraft cannot be read \()?the \.pcraft is too large a page of inks for this build \((\d+)×(\d+) pixels\)\)?)",
                         ".pcraft の画像が大きすぎて、インキのページにできません（\\1×\\2 px）"));
        r.push_back(rule(R"(the \.pcraft would be larger than 4 GiB or hold more than 65535 files.*)",
                         ".pcraft が 4 GiB を超えるか、中のファイルが 65535 を超えるので書けません"));
        r.push_back(rule(R"(a multichannel page is not written as a \.pcraft by this build.*)",
                         "マルチチャンネルのページは .pcraft に書き出せません（先に RGB・CMYK・グレースケールにします）"));
        r.push_back(rule(R"(the asset's \.pcraft cannot be read \(.*\))", "素材の .pcraft を読めません（中身が名前と違います）"));
        r.push_back(rule(R"(the \.pcraft cannot be read \(.*\))", "読み込めない .pcraft です"));
        // (PC4 F-e: a camera RAW file — render/raw: LibRaw's refusals, the limits —, its development and set_camera_raw)
        r.push_back(rule(R"((?:the asset's picture cannot be read \()?the camera RAW file is not one this build reads \(.*)",
                         "この版の Genko では現像できないカメラ RAW です（LibRaw が知らないカメラか圧縮の形式です）"));
        r.push_back(rule(R"((?:the asset's picture cannot be read \()?the camera RAW file is too large to read at once \(.*)",
                         "カメラ RAW が大きすぎて、一度に読めません"));
        r.push_back(rule(R"((?:the asset's picture cannot be read \()?the camera RAW file cannot be read \(.*)",
                         "カメラ RAW のファイルが壊れているか途中で切れていて、読めません"));
        r.push_back(rule(R"(the camera RAW file is empty)", "カメラ RAW のファイルが空です"));
        r.push_back(rule(R"(the camera RAW development: (.*))", "現像の設定が正しくありません（\\1）"));
        r.push_back(rule(R"(the smart object contents \S+ are not a camera RAW file.*)", "このスマートオブジェクトの中身はカメラ RAW ではありません"));
        r.push_back(rule(R"(the smart object's contents are a camera RAW file: their colours are.*)",
                         "中身はカメラ RAW のため、色は現像の色空間で決まります（コンテンツを編集の Camera Raw で変えます）"));
        // (PC4 F-a: the codecs' refusals — render/codecs: a picture past the limits, a broken file, what a format holds —
        // before the PSD's, whose words are of a PSD's layer)
        r.push_back(rule(R"(the picture is too large \((\d+)×(\d+) pixels; at most (\d+) pixels\))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("画像が大きすぎて読めません（%1×%2 px。%3 画素まで）").arg(m.captured(1), m.captured(2), m.captured(3));
        }));
        r.push_back(rule(R"(the picture is too large \((\d+)×(\d+) pixels; at most (\d+)×(\d+)\))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("画像が大きすぎて読めません（%1×%2 px。幅 %3・高さ %4 px まで）").arg(m.captured(1), m.captured(2), m.captured(3), m.captured(4));
        }));
        r.push_back(rule(R"(the picture is too large to read at once \(\d+ bytes; at most \d+\))",
                         "画像が大きすぎて、一度に読めません（作業に使える記憶の上限 2 GiB を超えます）"));
        r.push_back(rule("the picture is too large to count in this computer's memory", "画像が大きすぎて読めません"));
        r.push_back(rule(R"(the (.+) (?:file|picture) is too large to read)", "\\1 の画像が大きすぎて読めません"));
        r.push_back(rule(R"(the OpenEXR file has too many (?:channels|tiles) \(.*\))", "OpenEXR のファイルのチャンネルかタイルが多すぎて読めません"));
        r.push_back(rule(R"(the OpenEXR file's tiles are too large \(.*\))", "OpenEXR のファイルのタイルが大きすぎて読めません"));
        r.push_back(rule(R"(the OpenEXR file has no R, G, B or Y channels)", "OpenEXR のファイルに R・G・B か Y のチャンネルがありません"));
        r.push_back(rule(R"(the (.+) file cannot be read \(.*\))", "\\1 のファイルが壊れているか途中で切れていて、読めません"));
        r.push_back(rule(R"(the file is not an? (.+) file)", "\\1 のファイルではありません"));
        r.push_back(rule(R"(the bytes are not a picture of a format this build knows)", "この版の Genko が知っている形式の画像ではありません"));
        r.push_back(rule(R"((.+) holds at most (\d+)×(\d+) pixels)", "\\1 には \\2×\\3 px までしか書けません"));
        r.push_back(rule(R"(cannot write ([A-Za-z]+)(?:: .*| \(.*\)))", "\\1 のファイルを書けませんでした"));
        r.push_back(rule(R"(the picture is too large \(\d+×\d+ pixels.*\))", "PSD の絵が大きすぎて、レイヤーにできません"));
        r.push_back(rule(R"(the layer mask is too large \(.*\))", "用紙が大きすぎて、PSD のレイヤーマスクを作れません"));
        r.push_back(rule("the PSD has no pictures to read", "PSD に読み込める絵がありません"));
        // (PC4 F-d: a page past what a PSD holds — 30000 px a side, 2 GiB —, written as a PSB instead)
        r.push_back(rule(R"(the page is too large for a PSD \((\d+)×(\d+) pixels\): write it as a PSB)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("ページが大きすぎて PSD に書けません（%1×%2 px。PSD は 1 辺 30000 px まで）。PSB で書き出してください").arg(m.captured(1), m.captured(2));
        }));
        r.push_back(rule(R"(the page is too large for a PSD \(past (\d+) GiB\): write it as a PSB)",
                         "ファイルが PSD に書ける大きさ（2 GB）を超えます。PSB で書き出してください"));
        // (PC4 F-c: 書き出し形式…, クイック書き出し, Web 用に保存, レイヤーからファイル…, `genko copy`)
        r.push_back(rule(R"(the picture to write is too large \((\d+)×(\d+) pixels; .*\))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("書き出す画像が大きすぎます（%1×%2 px。1 億 2 千万画素・1 辺 10 万 px・作業の記憶 2 GiB まで）。倍率か大きさを下げてください")
                .arg(m.captured(1), m.captured(2));
        }));
        r.push_back(rule("the scale is 1 to 1000 percent", "拡大・縮小は 1〜1000 % です"));
        r.push_back(rule("a side is 1 to 100000 pixels", "幅と高さは 1〜100000 px です"));
        r.push_back(rule("the quality is 0 to 12", "画質は 0〜12 です"));
        r.push_back(rule("the quality is 1 to 100", "画質は 1〜100 です"));
        r.push_back(rule("a folder is written by its layers: choose one of them", "フォルダーは書き出せません。中のレイヤーを選んでください"));
        r.push_back(rule("the layer has nothing to write", "このレイヤーには書き出すものがありません（空のレイヤーです）"));
        r.push_back(rule(R"(no such layer: .*)", "そのレイヤーはこのページにありません"));
        r.push_back(rule(R"(the image has (\d+) colours; Exact needs 256 or fewer)", "この画像には \\1 色あります。減色の「すべての色」は 256 色までです"));
        r.push_back(rule(R"(unknown (preset|format|palette|dither|metadata|resample) `(.*)`.*)", "Web 用に保存の設定が正しくありません（\\1: \\2）"));
        r.push_back(rule("the quick export's format is png, jpg, gif or webp", "クイック書き出しの形式は PNG・JPG・GIF・WebP です"));
        r.push_back(rule("the layers' format is png, jpeg, tiff, psd or webp", "レイヤーからファイルの形式は PNG・JPEG・TIFF・PSD・WebP です"));
        // (PC4 F-c, the coordinator's P3-8: the places no way out writes to)
        r.push_back(rule(R"(cannot write over the book: (.*))", "原稿そのものには書き出しません。別の名前か場所を選んでください: \\1"));
        r.push_back(rule(R"(cannot write inside the book's folder: (.*))", "原稿のフォルダーの中には書き出しません。別の場所を選んでください: \\1"));
        r.push_back(rule(R"(cannot write over a file the book links: (.*))", "配置・リンクした元のファイルには書き出しません。別の名前か場所を選んでください: \\1"));
        r.push_back(rule(R"(too many files of that name: (.*))", "同じ名前のファイルが多すぎて、クイック書き出しの名前を決められません: \\1"));
        r.push_back(rule(R"(cannot write over the picture a book was opened from: (.*))", "開いた元の画像には書き出しません。別の名前か場所を選んでください: \\1"));
        // (PC5 I-0, SPEC.md AUTO-01: the steps of a script, a batch, a script event — service/automate, script_events)
        r.push_back(rule(R"(step ([0-9.]+) \(([a-z_]+)\): (.*))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("手順 %1（%2）: %3").arg(m.captured(1), m.captured(2), error(m.captured(3)));
        }));
        r.push_back(rule(R"(step ([0-9.]+): (\S+) cannot be a step .*)", "手順 \\1: \\2 は手順に入れられません（承認とページのロックは、人が画面でします）"));
        r.push_back(rule(R"(step ([0-9.]+): undo is not a step.*)", "手順 \\1: 元に戻すは手順に入れられません（手順の変更はまとめて元に戻せます）"));
        r.push_back(rule(R"(step ([0-9.]+): (\S+) is not an op an agent may run.*)", "手順 \\1: \\2 は AI が流せない操作です（MCP の apply_ops が受ける操作だけ）"));
        r.push_back(rule(R"(step ([0-9.]+): (\S+) is a pseudo op and cannot be inside for_pages)",
                         "手順 \\1: \\2 は手順の中だけの操作なので、for_pages の中には置けません（手順の上の段に置いてください）"));
        r.push_back(rule(R"(the action was changed after it was bound: (.*))",
                         "オートアクション「\\1」は結び付けた後で変わりました。スクリプトイベントマネージャーで選び直してください"));
        r.push_back(rule(R"(the script's name is longer than (\d+) characters)", "スクリプトの名前が長すぎます（\\1 文字まで）"));
        r.push_back(rule(R"(more than (\d+) files and folders to look through: choose fewer)", "見るファイルとフォルダーが多すぎます（\\1 個まで）"));
        r.push_back(rule(R"(step ([0-9.]+): not an op.*)","手順 \\1 が操作の形ではありません（{\"op\": 操作の名前, …引数} で書きます）"));
        r.push_back(rule(R"(steps, line (\d+): (.*))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("手順の %1 行目が読めません（%2）").arg(m.captured(1), reason(m.captured(2)));
        }));
        r.push_back(rule("the steps are larger than 1 MiB", "手順のファイルが大きすぎます（1 MiB まで）"));
        r.push_back(rule(R"(more than (\d+) steps)", "手順が多すぎます（\\1 まで）"));
        r.push_back(rule(R"(the steps are run page by page: (.*))", "手順をページごとに流せません（\\1）"));
        r.push_back(rule(R"(the script was changed after it was bound: (.*))",
                         "スクリプト「\\1」は結び付けた後で変わりました。スクリプトイベントマネージャーで選び直してください"));
        r.push_back(rule("a picture's own file is never written: write the batch into a folder", "元の画像には書きません。書き出すフォルダーを選んでください"));
        r.push_back(rule(R"(more than (\d+) files to take: choose fewer)", "ファイルが多すぎます（\\1 個まで）"));
        r.push_back(rule(R"(no action (.*))", "オートアクション \\1 がありません"));
        r.push_back(rule(R"(the colour profile cannot be read \((.*)\))", "カラープロファイルを読み込めません"));
        r.push_back(rule(R"(the file cannot be read \(it is larger than 2 GiB\))", "ファイルが大きすぎて読み込めません（2 GiB まで）"));
        // (this build's own: put_raster reads only a regular file — not a device or a pipe, whose reading may not end)
        r.push_back(rule(R"(the file cannot be read \(it is not a regular file\))", "ふつうのファイルではないので読み込めません（デバイスやパイプは読みません）"));
        // (a book of v1–v3, which this build does not write: storage::NeedsMigration)
        r.push_back(rule(R"(this book is in the version (\d+) format, which this build does not write; .*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("この原稿は古い形式（版 %1）なので、書き込めません。genko migrate で新しいフォルダーに v4 として写してから使ってください（元の原稿はそのまま残ります）")
                .arg(m.captured(1));
        }));
        r.push_back(rule(R"(the file cannot be read \((.*)\))", "ファイルを読み込めません"));
        r.push_back(rule("the profile is not a CMYK printing profile", "CMYK の印刷用のカラープロファイルではありません"));
        r.push_back(rule("the recorded pictures are missing", "記録した絵が見つかりません"));
        r.push_back(rule(R"(unknown PSD compression (\d+))", "この PSD の圧縮の形式は読めません"));
        r.push_back(rule("unknown PSD version", "この PSD の版は読めません"));
        r.push_back(rule("the book already has a .* cover", "その表紙はもうあります"));
        r.push_back(rule("the pattern cannot be read.*", "正規表現が読めません"));
        r.push_back(rule("body .* must be between .*", "体型の数値が大きすぎるか小さすぎます"));
        r.push_back(rule("body is .*", "体型は、等身・肩幅・腰幅・体格・脚の長さで決めます"));
        r.push_back(rule("dir must point somewhere", "光の向きを決めてください"));
        r.push_back(rule("each joint is .* in radians", "関節の曲げ方は、x・y・z の角度で決めます"));
        r.push_back(rule("focal_mm must be between 20 and 5000", "画角（焦点距離）は 20〜5000 mm です"));
        r.push_back(rule("hand poses are .*", "手の形は、開く・力を抜く・握る・指さす・ピース・つかむから選びます"));
        r.push_back(rule("hands is .*", "手の形は、左手・右手ごとに決めます"));
        r.push_back(rule("height_mm must be between 10 and 400", "身長は 10〜400 mm です"));
        r.push_back(rule("joints is .*", "関節の設定が正しくありません"));
        r.push_back(rule("obj is the model's OBJ text", "3D モデルの中身（OBJ ファイルの文字）が要ります"));
        r.push_back(rule("side is l or r", "手は左（l）か右（r）です"));
        r.push_back(rule("the 3D has no surfaces to shade here", "ここには陰を付けられる 3D の面がありません"));
        r.push_back(rule("unknown body key .*", "その体型の項目はありません（等身・肩幅・腰幅・体格・脚の長さ）"));
        r.push_back(rule("unknown joint .*", "その関節はありません"));
        r.push_back(rule("the OBJ file has no faces", "3D モデルに面がありません"));
        r.push_back(rule("the model file is cut short", "3D モデルのファイルが途中で切れています"));
        r.push_back(rule("the model file has no scene", "3D モデルのファイルに形がありません"));
        r.push_back(rule("the model file is not glTF", "読める 3D モデルのファイルではありません"));
        r.push_back(rule(R"(a \.gltf must carry its data inside.*)", "この形式のファイルは、データが別ファイルになっているので読めません（1 つにまとめた形式で書き出します）"));
        r.push_back(rule("the model cannot be read.*", "3D モデルを読めませんでした"));
        r.push_back(rule("the model has too many faces.*", "3D モデルの面が多すぎます（3 万まで）"));
        r.push_back(rule("the OBJ file points at corners it does not have", "3D モデルのファイルが壊れています（無い頂点を指しています）"));
        r.push_back(rule("the pack has a file outside itself", "素材パックの中に、パックの外を指すファイルがあります（読み込みません）"));
        r.push_back(rule(R"(a material pack is a folder or a \.zip)", "素材パックは、フォルダか zip ファイルです"));
        r.push_back(rule("the pack has no materials.*", "素材パックに素材がありません（素材の一覧か画像が要ります）"));
        r.push_back(rule(R"(the pack cannot be read as a zip file \(it is not a zip file\))", "zip ファイルとして読めません"));
        r.push_back(rule(R"(the pack cannot be read as a zip file \((zip64 is not read|a file inside is encrypted|a file inside is packed in a way that is not read|it is split over several files)\))",
                         "この zip ファイルは読めません（暗号化・分割・特殊な圧縮・大きすぎる形式には対応していません）"));
        r.push_back(rule(R"(the pack cannot be read as a zip file \((it is too large|it holds too many files|a file inside is too large|its files are too large)\))",
                         "素材パックが大きすぎます（zip 1 GB・中身の合計 512 MB・1 ファイル 64 MB・1 万ファイルまで）"));
        r.push_back(rule(R"(the pack cannot be read as a zip file \(a name inside is not UTF-8\))", "zip ファイルの中のファイル名が読めません（UTF-8 ではありません）"));
        r.push_back(rule(R"(the pack cannot be read as a zip file \(.*\))", "zip ファイルが壊れていて読めません"));
        r.push_back(rule("the pack holds too many files", "素材パックのファイルが多すぎます（1 万まで）"));
        r.push_back(rule("the material library cannot be read, so it is left as it is: (.*)",
                         "素材ライブラリ（\\1）が読めないので、書き換えずにそのままにしました。ファイルを直すか別の場所へ移してからやり直します"));
        r.push_back(rule("the material library folder is a link or not a folder, so it is left as it is: (.*)",
                         "素材ライブラリのフォルダ（\\1）がリンクかフォルダではないので、書き換えずにそのままにしました"));
        r.push_back(rule("the material (library|folders) cannot be written: (.*?) \\(.*\\)", "素材ライブラリ（\\2）に書き込めませんでした"));
        r.push_back(rule(R"(the picture cannot be written into the material library.*)", "画像を素材ライブラリに書き込めませんでした"));
        r.push_back(rule("the picture is too large \\(at most 64 MB\\).*", "画像ファイルが大きすぎます（64 MB まで）"));
        r.push_back(rule("the picture is a link or not a file.*", "画像がリンクかファイルではないので読みません"));
        r.push_back(rule("the pack\\.json is too large \\(at most 16 MB\\).*", "素材パックの pack.json が大きすぎます（16 MB まで）"));
        r.push_back(rule("the pack\\.json is a link or not a file.*", "素材パックの pack.json がリンクかファイルではないので読みません"));
        r.push_back(rule(R"(the material library would be too large for the materials panel \(at most (\d+) MB\))",
                         "素材ライブラリの一覧が大きくなりすぎるので、書き換えませんでした（\\1 MB まで）"));
        r.push_back(rule(R"(the material folders would be too large for the materials panel \(at most (\d+) MB\))",
                         "素材のフォルダーの一覧が大きくなりすぎるので、フォルダーを足しませんでした（\\1 MB まで）"));
        r.push_back(rule("kind must be one of tone, effect, image, lines, lettering, brush, prim", "素材の種類が違います"));
        r.push_back(rule("adjust is set on a correction layer", "補正の設定は、色調補正のレイヤーにだけできます"));
        r.push_back(rule("adjust kind must be one of .*", "色調補正は、レベル補正・トーンカーブ・色相・反転・階調化・2 値化・グラデーションマップ・白黒から選びます"));
        // (PC4 節 D, filters@1 — the C++ build's own: フィルター's kinds' settings and files, core/filter_kinds, render/fx)
        // (D3: フィルターギャラリー's stack of effects, core/gallery_kinds)
        r.push_back(rule(R"(adjust filter_gallery\.effects (?:is required|must be a list of).*)", "フィルターギャラリーには、かけるエフェクトレイヤーが 1〜32 個要ります"));
        r.push_back(rule(R"(adjust filter_gallery\.effects\[\d+\]\.filter: the gallery has no effect '(.*)')", "フィルターギャラリーに「\\1」という効果はありません"));
        r.push_back(rule(R"(adjust filter_gallery\.effects\[\d+\]\.filter must be .*)", "フィルターギャラリーの効果の名前が要ります（genko inspect --filters の gallery）"));
        r.push_back(rule(R"(adjust filter_gallery\.effects\[\d+\] has no setting '(.*)')", "フィルターギャラリーの効果に「\\1」という設定はありません"));
        r.push_back(rule(R"(adjust filter_gallery\.effects\[\d+\]\.(\S+) (?:must|needs|is) .*)", "フィルターギャラリーの効果の「\\1」の値が使えません"));
        r.push_back(rule(R"(adjust filter_gallery\.effects\[\d+\] must .*)", "フィルターギャラリーのエフェクトレイヤーの書き方が違います（{filter, params, visible}）"));
        r.push_back(rule(R"(filter_gallery: every effect is hidden.*)",
                         "フィルターギャラリーのエフェクトレイヤーが全部隠れているので、何も変わりません（スマートフィルターなら隠したまま持てます）"));
        // (D2: 置き換え's map and シアー's curve, core/filter_kinds, render/ops_filters)
        r.push_back(rule(R"(adjust distort_displace\.map is required.*)", "置き換えには置き換えマップが要ります（この原稿のレイヤーか、PNG・PSD のファイル）"));
        r.push_back(rule(R"(adjust distort_displace\.map_data is larger than 32 MiB)", "置き換えマップのファイルが大きすぎます（32 MiB まで）"));
        r.push_back(rule(R"(adjust distort_displace\.map_data must be .*)", "置き換えマップのファイルは PNG か PSD です"));
        r.push_back(rule(R"(adjust distort_displace\.map has no setting '(.*)' .*)", "置き換えマップの指定に「\\1」という設定はありません（page・layer）"));
        r.push_back(rule(R"(adjust distort_displace\.map(?:\.page|\.layer|_suffix)? must .*)",
                         "置き換えマップの指定が正しくありません（{\"layer\": レイヤーの id か \"merged\"}、または map_data）"));
        r.push_back(rule(R"(the displacement map cannot be read \(.*\))", "置き換えマップが読めません（PNG か PSD のファイルを確かめます）"));
        r.push_back(rule(R"(the displacement map's layer (\S+) is not on the page)", "置き換えマップのレイヤー（\\1）がそのページにありません"));
        r.push_back(rule(R"(the displacement map's page (\S+) is not in the book)", "置き換えマップのページ（\\1）がこの原稿にありません"));
        r.push_back(rule(R"(the displacement map's page (\S+) is still being read: try again in a moment)",
                         "置き換えマップのページ（\\1）をまだ読み込んでいます。少し待ってからもう一度試します"));
        r.push_back(rule(R"(the displacement map's layer (\S+) has no picture of its own .*)",
                         "置き換えマップのレイヤー（\\1）はフォルダーか色調補正レイヤーで、絵がありません"));
        r.push_back(rule(R"(the displacement map \S+ is not in the book)", "置き換えマップのファイルがこの原稿にありません"));
        r.push_back(rule(R"(adjust distort_shear\.points must be .*)", "シアーの曲線は、2〜64 個の点 [上からの位置 0〜1, ずらす量 −1〜1] で指定します"));
        // (D6: レンズ補正・広角補正's settings, core/filter_kinds; 消点's planes and op, core/vanishing, render/vanishing)
        r.push_back(rule(R"(adjust lens_correction\.straighten must be .*)", "レンズ補正の角度補正の線は、離れた 2 点 [[x, y], [x, y]]（mm）で指定します"));
        r.push_back(rule(R"(lens_correction: the profile auto needs the focal length.*)",
                         "レンズ補正のプロファイル「自動」には焦点距離が要ります（画像の EXIF に無いときは、焦点距離を入れます）"));
        r.push_back(rule(R"(adjust adaptive_wide_angle\.constraints must be .*)",
                         "広角補正の拘束線は 64 本まで、{a: [x, y], b: [x, y]（mm）, orientation: free・horizontal・vertical} で指定します"));
        r.push_back(rule(R"(vanishing\.planes must be .*)", "消点の面は 1〜32 面です（最初の面は 4 隅 corners_mm、後の面は 4 隅か from・edge・angle・depth）"));
        r.push_back(rule(R"(vanishing\.planes\[0\] needs its four corners.*)", "消点の最初の面には 4 隅（corners_mm）が要ります"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\]\.corners_mm must be .*)", "消点の面の 4 隅は、凸の四角形の 4 つの点 [x, y]（mm）です"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\]\.from must be .*)", "消点の面の from は、それより前の面の番号です"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\]\.edge must be .*)", "消点の面の edge は top・right・bottom・left のどれかです"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\]\.(?:angle|depth) must be .*)", "消点の面の角度と奥行きは数で指定します"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\] has its corners or is torn off.*)", "消点の面は、4 隅か、前の面からの継ぎ足し（from）のどちらかで指定します"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\] has no setting '(.*)'.*)", "消点の面に「\\1」という設定はありません"));
        r.push_back(rule(R"(vanishing\.planes\[\d+\] must be .*)", "消点の面の書き方が違います"));
        r.push_back(rule(R"(vanishing\.focal_mm must be .*)", "消点の焦点距離は、0 より大きい長さ（mm）です"));
        r.push_back(rule(R"(vanishing has no setting '(.*)'.*)", "消点の面の指定に「\\1」という設定はありません（planes・focal_mm）"));
        r.push_back(rule(R"(vanishing must be .*)", "消点の面の書き方が違います（{planes, focal_mm}）"));
        r.push_back(rule(R"(vanishing_point: the plane (\d+) would lie behind the camera)", "消点の面 \\1 がカメラの後ろに回ってしまいます（角度・奥行きを変えます）"));
        r.push_back(rule(R"(vanishing_point: the plane (\d+) is degenerate)", "消点の面 \\1 がつぶれています"));
        r.push_back(rule(R"(vanishing_point: the plane (\d+) reaches too far.*)", "消点の面 \\1 が遠くへ伸びすぎます（角度・奥行きを変えます）"));
        r.push_back(rule(R"(vanishing_point: the page has no planes yet.*)", "このページには消点の面がまだありません（面を作ってから使います）"));
        r.push_back(rule(R"(vanishing_point: no planes to paste or stamp on.*)", "消点の面をすべて消すときは、貼り付けもスタンプもできません"));
        r.push_back(rule(R"(vanishing_point: (\d+) dabs reached toward the horizon.*)",
                         "消点のスタンプの跡のうち \\1 個は、地平線に近く大きくなりすぎるため描きませんでした"));
        // (PC5 J-a, layer-comps@1 — the C++ build's own: the layer comps, render/ops_layer_comps)
        r.push_back(rule("the page has no layer comps", "このページにはレイヤーカンプがありません"));
        r.push_back(rule(R"(no layer comp with id (\S+))", "ID \\1 のレイヤーカンプはありません"));
        r.push_back(rule("no layer comp named (.*)", "「\\1」という名前のレイヤーカンプはありません"));
        r.push_back(rule("comp is a layer comp's id or name", "レイヤーカンプは ID か名前で指定します"));
        r.push_back(rule("what is all, visibility, position or appearance", "更新するものは all・visibility・position・appearance から選びます"));
        r.push_back(rule("give name, comment, visibility, position or appearance", "変える項目（名前・コメント・適用先）を指定します"));
        r.push_back(rule("name is 1 to 255 characters|name is the layer comp's name", "レイヤーカンプの名前は 1〜255 文字です"));
        r.push_back(rule("comment is text|comment is at most 65536 bytes", "レイヤーカンプのコメントは 65536 バイトまでの文字です"));
        r.push_back(rule("a page holds at most 256 layer comps", "レイヤーカンプは 1 ページに 256 個までです"));
        r.push_back(rule("no layer comp has been applied over the page's own state", "最後のドキュメントの状態はまだありません（レイヤーカンプを適用すると残ります）"));
        r.push_back(rule("no layer comp of the page names a layer it does not have", "警告のあるレイヤーカンプはありません"));
        r.push_back(rule("(visibility|position|appearance) is true or false", "レイヤーカンプの適用先（\\1）は true か false です"));
        r.push_back(rule(R"(the comp's layer style of (\S+) is not one: .*)", "レイヤーカンプに記録したレイヤー \\1 のレイヤースタイルが読めません"));
        // (PC5 J-c, annotations@1 — the C++ build's own: the notes, render/ops_annotations and formats/notes_import)
        r.push_back(rule(R"(at_mm is \[x, y\] \(mm\))", "注釈の位置（at_mm）は [x, y]（mm）で指定します"));
        r.push_back(rule(R"(popup_mm is \[x, y, w, h\] \(mm\) or null)", "注釈の本文の枠（popup_mm）は [x, y, 幅, 高さ]（mm）か null で指定します"));
        r.push_back(rule("text is the note's text|a note's text is at most 65536 bytes", "注釈の本文は 65536 バイトまでの文字です"));
        r.push_back(rule("author is the note's author's name|author is at most 1024 bytes", "注釈の作成者は 1024 バイトまでの文字です"));
        r.push_back(rule("open is true or false", "注釈を開くか（open）は true か false です"));
        r.push_back(rule(R"(color is \[r, g, b\] \(0\.\.255\) or #rrggbb)", "注釈の色は [r, g, b]（0〜255）か #rrggbb で指定します"));
        r.push_back(rule(R"(modified is YYYY-MM-DDThh:mm:ssZ or "")", "注釈の更新日時は YYYY-MM-DDThh:mm:ssZ か空の文字です"));
        r.push_back(rule("id is the note's id", "注釈は ID で指定します"));
        r.push_back(rule(R"(no note (\S+) on the page)", "このページに注釈 \\1 はありません"));
        r.push_back(rule("a page holds at most 1000 notes", "注釈は 1 ページに 1000 個までです"));
        r.push_back(rule("give text, author, color, open, at_mm or popup_mm", "変える項目（本文・作成者・色・開閉・位置・枠）を指定します"));
        r.push_back(rule(R"(give ids \(the notes' ids\) or all: true)", "消す注釈を ID の一覧（ids）か all: true で指定します"));
        r.push_back(rule("the page has no notes", "このページには注釈がありません"));
        r.push_back(rule(R"(a person wrote the note (\S+); only a person can change it)", "注釈 \\1 は人が書いたものです。変えられるのは人だけです"));
        r.push_back(rule(R"(a person wrote the note (\S+); only a person can delete it)", "注釈 \\1 は人が書いたものです。削除できるのは人だけです"));
        r.push_back(rule("the file has no notes", "このファイルには注釈がありません"));
        r.push_back(rule(R"(notes are read from a PSD, a PSB or a \.pcraft)", "注釈は PSD・PSB・.pcraft のファイルから読み込みます"));
        r.push_back(rule("the PSD's notes cannot be read|notes: .*", "ファイルの注釈が読めない形です"));
        // (PC5-0, Z0's review P1-1: a new layer of a 16- or 32-bit page painted at 8 bits for the book's precise budget)
        r.push_back(rule(R"(the layer is painted at 8 bits: the book's precise pixels would pass their budget.*)",
                         "本の高精度の画素の上限（1 GiB）を越えるか、本をまだ読み込み中のため、このレイヤーは 8 bit で塗りました"));
        r.push_back(rule(R"(vanishing_point needs the paint layer it lays on.*)", "消点で描くペイントのレイヤーを選ぶか、新しいレイヤーにします"));
        r.push_back(rule(R"(vanishing_point has no setting '(.*)'.*)", "消点に「\\1」という設定はありません"));
        r.push_back(rule(R"(vanishing_point has no kind.*)", "消点の種類の指定が違います"));
        r.push_back(rule(R"(paste must be a list of at most (\d+) .*)", "消点で貼り付ける絵は \\1 個までです"));
        r.push_back(rule(R"(paste\[\d+\]\.data .*)", "消点で貼り付ける絵のデータが読めません（PNG のファイル、32 MiB まで）"));
        r.push_back(rule(R"(paste\[\d+\]\.layer: no layer (\S+) on the page)", "消点で貼り付けるレイヤー（\\1）がそのページにありません"));
        r.push_back(rule(R"(paste\[\d+\]\.plane must be .*)", "消点で貼り付ける面の番号が違います"));
        r.push_back(rule(R"(paste\[\d+\]\.at must be .*)", "消点で貼り付ける位置（at）は、面の上の [u, v]（数 2 つ）です"));
        r.push_back(rule(R"(paste\[\d+\]\.width must be .*)", "消点で貼り付ける幅（width）は、面の幅に対する割合の数（0.01〜10）です"));
        r.push_back(rule(R"(paste\[\d+\] .*)", "消点で貼り付ける絵の指定が違います（{plane, layer か data, at: [u, v], width}）"));
        r.push_back(rule(R"(clone must be a list of at most (\d+) .*)", "消点のスタンプは \\1 本までです"));
        r.push_back(rule(R"(clone\[\d+\]\.(?:source|points) must lie near the page.*)",
                         "消点のスタンプの点とコピー元は、ページとその周り（ページの幅と高さの分）に置きます"));
        r.push_back(rule(R"(clone\[\d+\].*)", "消点のスタンプの指定が違います（{source: [x, y], points: [[x, y]…]（mm）, size_px, hardness, opacity}）"));
        r.push_back(rule(R"(a stroke has more than (\d+) dabs.*)", "線が長すぎます（ブラシの間隔で \\1 個を超える跡になります。点を近づけるか、ブラシを大きくします）"));
        r.push_back(rule(R"(a page's vanishing point planes: .*)", "ページの消点の面が正しくありません（原稿は読み取り専用で開きます）"));
        // (D5: Camera Raw フィルター's point curves, core/filter_kinds)
        r.push_back(rule(R"(adjust camera_raw\.point_curve(?:_red|_green|_blue)? must be .*)",
                         "Camera Raw フィルターのポイントカーブは、[]（直線）か、2〜19 個の点 [入力, 出力]（0〜255）で指定します"));
        r.push_back(rule(R"(the smart object \S+'s filter \S+_data: .*)", "スマートフィルターのファイルは原稿の素材として持ちます（ファイルのデータのままでは持てません）"));
        // (PC4 D4: 描画・ぼかしギャラリー's lists, 照明効果's channel, render_picture — 炎・額縁・木)
        r.push_back(rule(R"(adjust render_lighting_effects\.lights must be a list of at most (\d+) .*)",
                         "照明効果の光は \\1 個までです（種類・位置・高さ・向き・色・強さ・円錐・ホットスポット・半径）"));
        r.push_back(rule(R"(adjust render_lighting_effects\.lights' each light is .*)",
                         "照明効果の光の設定が正しくありません（種類はスポットライト・点光源・無限光、カラーは [r, g, b]、ほかは数です）"));
        r.push_back(rule(R"(adjust render_lighting_effects\.texture must be .*)",
                         "照明効果のテクスチャは、なし・レッド・グリーン・ブルー・透明部分・輝度か、ページのアルファチャンネルです"));
        r.push_back(rule(R"(render_lighting_effects: the page has no channel (.*) for the texture)", "照明効果のテクスチャのチャンネル「\\1」がページにありません"));
        r.push_back(rule(R"(render_lighting_effects: the texture's channel (.*) is not an alpha channel)",
                         "照明効果のテクスチャには、アルファチャンネルを使います（「\\1」はアルファチャンネルではありません）"));
        r.push_back(rule(R"(render_lighting_effects: a smart filter's texture cannot be a channel of the page.*)",
                         "スマートフィルターの照明効果では、テクスチャにページのチャンネルを使えません（なし・レッド・グリーン・ブルー・透明部分・輝度から選びます）"));
        r.push_back(rule(R"(adjust blur_(?:field|iris|spin)\.pins must be a list of 1 to (\d+) .*)", "ぼかしギャラリーのピンは 1〜\\1 個です"));
        r.push_back(rule(R"(adjust blur_(?:field|iris|spin)\.pins' each pin is .*)", "ぼかしギャラリーのピンの設定が正しくありません（場所・ぼかしなどは数です）"));
        r.push_back(rule(R"(adjust blur_path\.paths' each path is .*)",
                         "パスぼかしのパスの設定が正しくありません（点は 2 個以上の [x, y]（ページの mm）、速度・先細りは数です）"));
        r.push_back(rule(R"(adjust blur_path\.paths must be a list of at most (\d+) .*)",
                         "パスぼかしのパスは \\1 本までで、点は各パス 2 個以上・全部で 4096 個まで（ページの mm）です"));
        r.push_back(rule(R"(render_picture's kind must be .*)", "描画の種類は、炎（flame）・額縁（picture_frame）・木（tree）から選びます"));
        r.push_back(rule(R"(render_picture's new_layer must be true or false)", "新しいレイヤーに描くかどうかは true か false で決めます"));
        r.push_back(rule(R"(render_picture's id must be a layer's id)", "描くレイヤーの指定が違います"));
        r.push_back(rule(R"(render_picture would draw more than (\d+) shapes.*)",
                         "描く形が多すぎます（\\1 個を超えます。炎の間隔を広げるか、パスを短くするか、選択範囲を小さくします）"));
        r.push_back(rule(R"(render_picture flame: the page has no path named '(.*)')", "そのページに「\\1」という名前のパスがありません"));
        r.push_back(rule(R"(render_picture flame: the path is too long or too finely curved.*)", "炎の道筋のパスが長すぎるか細かすぎて、たどれません"));
        r.push_back(rule(R"(render_picture (flame|picture_frame|tree) has no setting '(.*)')", [](const QRegularExpressionMatch& m) {
            const QString kind = m.captured(1) == QStringLiteral("flame") ? QStringLiteral("炎") : m.captured(1) == QStringLiteral("tree") ? QStringLiteral("木") : QStringLiteral("額縁");
            return QStringLiteral("%1 に「%2」という設定はありません").arg(kind, m.captured(2));
        }));
        r.push_back(rule(R"(render_picture (flame|picture_frame|tree)\.(\S+) (?:must|needs|is) .*)", [](const QRegularExpressionMatch& m) {
            const QString kind = m.captured(1) == QStringLiteral("flame") ? QStringLiteral("炎") : m.captured(1) == QStringLiteral("tree") ? QStringLiteral("木") : QStringLiteral("額縁");
            return QStringLiteral("%1 の「%2」の値が使えません").arg(kind, m.captured(2));
        }));
        r.push_back(rule(R"(adjust ((?:blur|sharpen|noise|other|video|distort|pixelate|stylize|render|filter|camera|lens|adaptive)_\S+?) has no setting '(.*)')",
                         "フィルター（\\1）に「\\2」という設定はありません"));
        r.push_back(rule(R"(adjust ((?:blur|sharpen|noise|other|video|distort|pixelate|stylize|render|filter|camera|lens|adaptive)_\S+?)\.(\S+) (?:must|needs|is) .*)",
                         "フィルター（\\1）の「\\2」の値が使えません"));
        r.push_back(rule(R"(the smart object \S+'s filter \S+ is not one of the book's files.*)", "スマートフィルターのファイルの指定が正しくありません"));
        r.push_back(rule(R"(the smart object \S+'s filter \S+_suffix is not .*)", "スマートフィルターのファイルの形式は PNG か PSD です"));
        r.push_back(rule(R"(the smart object \S+'s filter \S+: the book holds no file .*)",
                         "スマートフィルターが使うファイルがこの原稿にありません（原稿は読み取り専用で開きます）"));
        r.push_back(rule("the result exceeds the finite working range", "結果の値が大きすぎて、扱える範囲を超えました（原稿は変わりません）"));
        // (PC4 E5: the brushes that change pixels — render/ops_paint_tools)
        r.push_back(rule("range must be one of midtones, shadows, highlights", "範囲は、シャドウ・中間調・ハイライトから選びます"));
        r.push_back(rule("mode must be one of desaturate, saturate", "スポンジのモードは、彩度を下げる・彩度を上げるから選びます"));
        r.push_back(rule("mode must be one of color, hue, saturation, luminosity", "色の置き換えのモードは、色相・彩度・カラー・輝度から選びます"));
        r.push_back(rule("sampling must be one of continuous, once, swatch", "サンプルは、連続・一度・背景のスウォッチから選びます"));
        r.push_back(rule("limits must be one of contiguous, discontiguous, find_edges", "制限は、隣接されていない・隣接・輪郭検出から選びます"));
        r.push_back(rule(R"(the stroke is too much work to lay at once.*)",
                         "筆の仕事が多すぎます（跡の面積が上限を超えます。直径か間隔を変えるか、なぞる長さを短くします）"));
        r.push_back(rule("points must lie within 100000 mm of the page", "筆の点は、ページから 100 m の内に置きます"));
        r.push_back(rule(R"(the layer's transparency is locked: the Background Eraser .*)",
                         "透明部分を保護したレイヤーには、背景消しゴムツールは使えません（保護を外してから使います）"));
        r.push_back(rule(R"(reservoir.*)", "混合ブラシの絵の具の指定が違います（前のストロークの結果をそのまま渡します）"));
        r.push_back(rule("seed must be a string or a whole number", "ブラシの揺らぎの種は、文字か整数で指定します"));
        r.push_back(rule(R"(brush must be a dab brush of the book.*)", "そのブラシは使えません（先端の跡を並べて描くブラシを、原稿のブラシから選びます）"));
        r.push_back(rule(R"((rgb|foreground_rgb|swatch_rgb) is \[r, g, b\] \(0\.\.255\), "foreground" or "background")",
                         "色は 0〜255 の 3 つの数か、描画色（foreground）・背景色（background）で指定します"));
        r.push_back(rule(R"(the smudge is too wide for the layer.*)", "色混ぜの太さが大きすぎます（レイヤーの辺の 4 倍まで）"));
        r.push_back(rule(R"(the Background Eraser takes away the alpha: it cannot work one colour plate)",
                         "背景消しゴムツールは透明にする道具なので、色の版 1 つには使えません（描く先をレイヤーに戻します）"));
        // (PC4 E2: the brush library, tool presets and the presets' files — render/brush_presets, tool_presets, preset_files)
        r.push_back(rule(R"(the built-in brush (.+) cannot be changed.*)", "組み込みのブラシ（\\1）は変えられません（自分のブラシにしてから変えます）"));
        r.push_back(rule(R"(no brush of one's own named (.+))", "「\\1」という自分のブラシはありません"));
        r.push_back(rule(R"(a brush named (.+) is there already)", "「\\1」という名前のブラシがもうあります"));
        r.push_back(rule(R"(the group (.+) is the built-in brushes'.*)", "「\\1」は組み込みのブラシのグループです（自分のブラシには別のグループ名を付けます）"));
        r.push_back(rule("a group's name is 1 to 200 characters", "グループ名は 1〜200 文字にします"));
        r.push_back(rule(R"(a group named (.+) is there already)", "「\\1」というグループがもうあります"));
        r.push_back(rule(R"(no group named (.+))", "「\\1」というグループはありません"));
        r.push_back(rule("a brush's name is 1 to 40 characters", "ブラシ名は 1〜40 文字にします"));
        r.push_back(rule("rename names one brush", "名前を変えるブラシを 1 つ選びます"));
        r.push_back(rule("preset names (?:no brush|a brush by its key or name.*)", "ブラシを選びます"));
        r.push_back(rule("a brush of one's own has a key starting with my_.*", "自分のブラシの鍵は my_ で始めます（40 文字まで）"));
        r.push_back(rule(R"(the library holds (\d+) brushes of one's own at most)", "自分のブラシは \\1 本までです"));
        r.push_back(rule("nothing there holds paint.*", "そこには描いたもの（白でも透明でもない所）がありません"));
        r.push_back(rule("the area covers none of the page", "選択範囲がページの外にあります"));
        r.push_back(rule(R"(the area is too large for a brush tip.*)", "ブラシの先端にするには大きすぎます（2500 画素四方まで。選択範囲を小さくします）"));
        r.push_back(rule(R"(no tool preset named (.+) \(see tool-presets list\))", "「\\1」というツールプリセットはありません"));
        r.push_back(rule(R"((?:a tool preset(?:'s)? |options\.|the field ).*)", "ツールプリセットの中身が正しくありません"));
        r.push_back(rule("the kinds of presets are .*", "プリセットの種類は、ブラシ・グラデーション・パターン・シェイプ・ツールプリセットから選びます"));
        r.push_back(rule("not a Genko presets file.*", "Genko のプリセットのファイルではありません"));
        r.push_back(rule(R"((?:the presets are larger than|a presets file is at most) .*)", "プリセットのファイルが大きすぎます（256 MB まで）"));
        r.push_back(rule("the settings folder is this one: .*", "移行元が今の設定のフォルダーです（別の設定のフォルダーか、書き出したプリセットのファイルを選びます）"));
        r.push_back(rule(R"(the brush file's tips are too large to read.*)", "ブラシのファイルの先端が大きすぎて読めません（ほどくと 512 MB を越えます）"));
        r.push_back(rule(R"(the brush file is larger than (\d+) MB)", "ブラシのファイルが大きすぎます（\\1 MB まで）"));
        r.push_back(rule(R"(brush files of (?:sub)?version (\d+) are not read)", "この版（\\1）のブラシのファイルは読めません（版 1・2・6〜10 を読みます）"));
        r.push_back(rule(R"(not a brush file that can be read: (.*))", "ブラシのファイルとして読めません（\\1）"));
        r.push_back(rule(R"(the brush file holds no brush that can be read.*)", "読めるブラシがファイルに入っていません"));
        // (PC2, adjustments@1 — the C++ build's own: a correction's native settings and kinds, core/adjust)
        r.push_back(rule(R"(adjust kind '(.*)' is not a correction layer's kind)", "「\\1」は色調補正のレイヤーの種類ではありません"));
        // (PC2a — the tone corrections' own: set_adjustment, auto_adjust and a colour lookup's table, render/ops_adjust, core/lut3d)
        r.push_back(rule("set_adjustment needs the id of a correction layer", "設定を変える色調補正のレイヤーを選びます"));
        r.push_back(rule(R"(layer \S+ is not a correction layer)", "そのレイヤーは色調補正のレイヤーではありません"));
        r.push_back(rule(R"(set_adjustment keeps the correction's kind.*)", "色調補正の種類は変えられません（補正のレイヤーを作り直します）"));
        r.push_back(rule("auto_adjust mode must be one of tone, contrast, color", "自動補正は、自動トーン補正・自動コントラスト・自動カラー補正から選びます"));
        r.push_back(rule("auto_adjust needs the id of a layer", "自動補正をかけるレイヤーを選びます"));
        r.push_back(rule(R"(adjust color_lookup\.data must be the text of .*)", "カラールックアップの表は、.cube・.3dl・.look のファイルから読みます"));
        r.push_back(rule(R"(adjust color_lookup\.data is larger than 64 MiB)", "カラールックアップの表のファイルが大きすぎます（64 MiB まで）"));
        r.push_back(rule(R"(adjust color_lookup\.data: .*)", "カラールックアップの表のファイルが読めません（.cube・.3dl・.look の中身を確かめます）"));
        r.push_back(rule(R"((?:the correction layer \S+: )?adjust color_lookup\.lut: the book holds no lookup table .*)",
                         "この原稿にはそのカラールックアップの表がありません（表のファイルを読み込み直します）"));
        r.push_back(rule(R"(the lookup table \S+ is not held: the book is not written)",
                         "カラールックアップの表が見つからないので、原稿を保存しませんでした"));
        r.push_back(rule(R"(adjust (\S+?)(?: native)?(?:\.\S+)? has no setting '(.*)')", "色調補正（\\1）に「\\2」という設定はありません"));
        r.push_back(rule(R"(adjust (\S+?)(?: native)?\.(\S+) (?:must|needs|has no channel) .*)", "色調補正（\\1）の「\\2」の値が使えません"));
        r.push_back(rule(R"(adjust (\S+?) native: a (Lab|multichannel) page cannot composite the blend '(.*)')", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1 のページでは、描画モード「%2」の色調補正を合成できません")
                .arg(m.captured(2) == QStringLiteral("Lab") ? QStringLiteral("Lab") : QStringLiteral("マルチチャンネル"), m.captured(3));
        }));
        r.push_back(rule(R"(filter \S+: native\.blend is a correction layer's.*)",
                         "描画モードは色調補正のレイヤーのものです（フィルターの結果を描画モードで重ねるには、フェードを使います）"));
        // (PC2c — the C++ build's own: the local corrections, HDR toning, Apply Image, Calculations, trapping, the fade)
        r.push_back(rule(R"(adjust replace_color\.color is required)", "置き換える色を選びます"));
        r.push_back(rule(R"(adjust match_color: give source or source_stats, not both)", "色の一致の元は、原稿の中の画像か、色の統計のどちらか 1 つです"));
        r.push_back(rule(R"(adjust match_color\.source has no opaque pixels)", "色の一致の元に、色のある画素がありません"));
        r.push_back(rule(R"(adjust match_color\.source(?:_stats)?(?:\.\S+)?[: ].*)", "色の一致の元の指定が使えません"));
        r.push_back(rule(R"(trap works on a CMYK page.*)", "トラッピングは CMYK のページでだけ使えます"));
        r.push_back(rule(R"(the layer \S+ holds no inks to trap)", "このレイヤーには、トラッピングするインキがありません"));
        r.push_back(rule(R"(give width_px or width_mm, not both)", "トラッピングの幅は、px か mm のどちらかで指定します"));
        r.push_back(rule(R"(the layer \S+ is not a paint layer \(pen or paint\))", "描画のレイヤー（ペン・ペイント）を選びます"));
        r.push_back(rule(R"(id \(the (?:paint layer to trap|layer to fade)\) is required)", "対象のレイヤーが要ります"));
        r.push_back(rule(R"(from \(the layer's pixels before.*\) is required)", "フェードする前の画素（from）が要ります"));
        r.push_back(rule(R"(fade_layer reads the pixels before from the book's folder.*)", "フェードの前に、原稿を保存します（ファイル → 別名で保存…）"));
        r.push_back(rule(R"(the book has no asset \S+ \((?:a tile of )?the pixels before\))", "フェードする前の画素が、原稿の素材にありません"));
        r.push_back(rule(R"(from as tiles is a precise layer's.*)", "タイルの形の前の画素（from）は、高精度のレイヤーのものです（ほかのレイヤーは素材の sha256 を渡します）"));
        r.push_back(rule(R"(the pixels before are too large to read whole.*)",
                         "フェードする前の画素が大きすぎて、丸ごとは読めません（素材 1 つ 256 MiB まで。高精度のレイヤーは project.json のタイルの形で渡せます）"));
        // (independent review B (C): a local correction, a fade or an image calculation past the imaging budget)
        r.push_back(rule(R"(the pixels are too large to work on at once.*)", "絵が大きすぎて、一度に処理できません（作業に使える記憶の上限を超えます）"));
        r.push_back(rule(R"(from is not the pixels of this layer before.*)", "フェードする前の画素が、このレイヤーのものと合いません"));
        r.push_back(rule(R"(the layer \S+ holds no pixels to fade)", "このレイヤーには、フェードする画素がありません"));
        r.push_back(rule(R"(the layer \S+ holds lines or fills besides its pixels)", "線や塗りの残るレイヤーはフェードできません"));
        r.push_back(rule(R"(give the layer \(id\) or the alpha or spot channel \(channel\) to apply the image to)",
                         "画像操作の先は、レイヤーか、アルファ・特色チャンネルのどちらか 1 つです"));
        r.push_back(rule(R"(no alpha or spot channel '(.*)' on page (\S+) .*)", "ページ \\2 にアルファ・特色チャンネル「\\1」はありません"));
        r.push_back(rule(R"((?:source|source1|source2|mask): page (\S+) is not the same size as page (\S+))", "元のページ \\1 は、ページ \\2 と大きさが違います"));
        r.push_back(rule(R"((?:source|source1|source2|mask): no channel '(.*)' on page (\S+))", "ページ \\2 にチャンネル「\\1」はありません"));
        r.push_back(rule(R"((?:source|source1|source2|mask): no layer \S+ on page (\S+))", "ページ \\1 に元のレイヤーはありません"));
        r.push_back(rule(R"((?:source|source1|source2|mask): the transparency is a layer's.*)", "透明部分はレイヤーのものです（元のレイヤーを選びます）"));
        r.push_back(rule(R"((?:source|source1|source2|mask): the layer has no mask)", "元のレイヤーにマスクがありません"));
        r.push_back(rule(R"(blending '(.*)' is not separable.*)", "描画モード「\\1」は、画像操作・演算では使えません"));
        r.push_back(rule(R"(blending must be .*)", "描画モードの指定が違います"));
        r.push_back(rule(R"(result must be channel, page or selection)", "演算の結果は、チャンネル・ページ・選択範囲のどれかです"));
        r.push_back(rule(R"(curve must be a list of 2 to 19 points.*)", "トーンカーブの点は 2〜19 個です（入力・出力 0〜255）"));
        r.push_back(rule(R"(no layer is showing)", "表示しているレイヤーがありません"));
        r.push_back(rule("choose two or more layers to merge", "結合するレイヤーを 2 枚以上選びます"));
        r.push_back(rule("effect is .* and/or .*", "境界効果は、フチか水彩境界です"));
        r.push_back(rule("fill is .* or .*", "塗りの設定は、色かグラデーションです"));
        r.push_back(rule("fill is set on a fill layer", "塗りの設定は、塗りつぶしのレイヤーにだけできます"));
        r.push_back(rule(R"(from and to are \[x_mm, y_mm\])", "グラデーションのはじめと終わりの位置が要ります"));
        r.push_back(rule("ids is the list of layer ids", "レイヤーを選んでください"));
        r.push_back(rule("interp must be .*", "補間は、なめらか・よりなめらか・ハードから選びます"));
        r.push_back(rule("mode must be one of push.*", "ゆがみは、押し流す・縮める・ふくらませる・渦から選びます"));
        r.push_back(rule("mode must be one of .*", "その動かし方は選べません"));
        r.push_back(rule("only a paint layer can become a pen layer", "ペンのレイヤーに変換できるのは、ペイントのレイヤーだけです"));
        r.push_back(rule("page not found", "そのページはありません"));
        r.push_back(rule("parent must be a folder", "入れる先はフォルダを選びます"));
        r.push_back(rule(R"(points is \[\[x, y\], \.\.\.\] in mm)", "なぞった点が要ります"));
        r.push_back(rule(R"(rgb is \[r, g, b\], each 0\.\.255)", "色は 0〜255 の 3 つの数です"));
        r.push_back(rule("set_layers needs something to set.*", "まとめて変える設定がありません"));
        r.push_back(rule("shape must be linear or radial", "グラデーションの形は、直線か円です"));
        r.push_back(rule("shape must be linear, radial or ellipse", "グラデーションの形は、直線・円・楕円から選びます"));
        r.push_back(rule("shape must be linear, radial, ellipse, angle, reflected or diamond", "グラデーションの形は、直線・円・楕円・角度・反射・菱形から選びます"));
        r.push_back(rule(R"(stops are \[\[position.*)", "グラデーションの色は 2〜16 色で、それぞれ位置（0〜100 %）・色・濃さ（0〜100 %）を指定します"));
        r.push_back(rule("repeat is none, repeat or mirror", "繰り返しは、なし・繰り返す・折り返すから選びます"));
        r.push_back(rule(R"(the other book cannot be read \(this book (?:needs repairs|uses features).*\))",
                         "取り込む原稿に、見つからない絵や読めないデータがあるため取り込めません（その原稿を開いて確かめてください）"));
        r.push_back(rule(R"(the other book cannot be read \(project\.json (?:version \S+ is newer than this build supports|needs Genko that reads version \S+).*\))",
                         "取り込む原稿は、この Genko より新しい版で保存されているため取り込めません（Genko を新しくしてください）"));
        r.push_back(rule("the other book cannot be read.*", "取り込む原稿を開けませんでした（原稿のフォルダーを選んでください）"));
        r.push_back(rule(R"(project\.json (?:version \S+ is newer than this build supports|needs Genko that reads version \S+).*)",
                         "この原稿は、この Genko より新しい版で保存されているため開けません（Genko を新しくしてください）"));
        r.push_back(rule(R"(pages (\S+) and (\S+) are not next to each other)", "\\1 ページと \\2 ページは隣り合っていないので、見開きになりません"));
        r.push_back(rule(R"(pages (\S+) and (\S+) are two sides of one leaf, not a spread)",
                         "\\1 ページと \\2 ページは 1 枚の紙の表と裏なので、見開きになりません"));
        r.push_back(rule("preset must be one of b4, b5, a5, a4, webtoon", "原稿用紙は B4・B5・A5・A4・縦読み（Webtoon）から選びます"));
        r.push_back(rule(R"(the other book has no page (\d+))", "取り込む原稿に \\1 ページはありません"));
        r.push_back(rule("the adjustment cannot be used: .*", "その補正の数値は使えません"));
        r.push_back(rule("the layer has no marks to trace", "このレイヤーには線にできる絵がありません"));
        r.push_back(rule("this layer cannot become a paint layer", "このレイヤーはペイントのレイヤーに変換できません"));
        r.push_back(rule("unknown blend mode .*", "その合成モードはありません"));
        r.push_back(rule("unknown effect .*", "その境界効果はありません（フチ・水彩境界）"));
        r.push_back(rule("an area needs at least three points", "範囲には 3 点以上が要ります"));
        r.push_back(rule("no layer .*", "そのレイヤーはありません"));
        r.push_back(rule("the colour point is off the page", "色を取る点がページの外です"));
        r.push_back(rule("(union|intersect|subtract) needs areas", "範囲の組み合わせには範囲が要ります"));
        r.push_back(rule("an area is poly, mask, rect.*", "範囲の指定が読めません"));
        r.push_back(rule("reference must be page, layer or reference", "塗りの見る範囲は「見えている全部」「このレイヤーだけ」「参照レイヤー」です"));
        r.push_back(rule("no layer is set as the reference.*", "参照レイヤーがありません。レイヤー パネルで線のレイヤーを「参照にする」にします"));
        r.push_back(rule("the area has no size", "選んだ範囲に大きさがありません"));
        r.push_back(rule("perspective takes four corners.*", "遠近の変形は 4 隅（左上・右上・右下・左下）で指定します"));
        r.push_back(rule("the four corners must enclose an area", "4 隅が一直線に並んでいて、形になりません"));
        r.push_back(rule("mesh takes .*", "メッシュの変形の点は、格子に並べて行ごとに指定します（ふつうは 3×3 の 9 点、格子の数は 2〜9 点）"));
        r.push_back(rule("a warp is perspective.*", "自由変形は遠近（4 隅）かメッシュ（9 点）で指定します"));
        r.push_back(rule("the transform stretches the area too far", "引き伸ばしすぎです。点を近づけます"));
        r.push_back(rule("a folder cannot be duplicated", "フォルダは複製できません（中のレイヤーを選んで複製します）"));
        r.push_back(rule("there is no layer below to merge into", "下にレイヤーがないので結合できません"));
        r.push_back(rule("placed images and tones cannot be merged", "配置した画像とトーンのレイヤーは結合できません"));
        r.push_back(rule("a folder cannot take a mask.*", "フォルダにはマスクを付けられません（中のレイヤーに付けます）"));
        r.push_back(rule("fill must be show or hide", "マスクは「全部見せる」か「全部隠す」で作ります"));
        // (PC3 B2: render/ops_masks — レイヤーマスクの適用・透明部分から、境界線を描く)
        r.push_back(rule("the layer \\S+ has no layer mask", "このレイヤーにはマスクがありません"));
        r.push_back(rule("(apply_layer_mask|mask_from_transparency): the layer \\S+ is not a pen or paint layer.*",
                         "ペン・ペイントのレイヤーではないため、できません（先にラスタライズします）"));
        r.push_back(rule("width_mm must be more than 0", "幅は 0 より大きくします"));
        r.push_back(rule("location must be inside, center or outside", "境界線の位置は、内側・中央・外側から選びます"));
        r.push_back(rule("the layer has nothing drawn to stroke", "このレイヤーには、まだ何も描いてありません（境界線を描く縁がありません）"));
        r.push_back(rule("the stroke has nothing to draw", "境界線を描く所がありません"));
        r.push_back(rule("the area is off the page", "範囲がページの外です"));
        r.push_back(rule(R"(points needs at least one \[x_mm, y_mm\] pair)", "点が 1 つもありません"));
        // undo, pages, layers
        r.push_back(rule("nothing to undo", "これ以上戻せません"));
        r.push_back(rule("nothing to redo", "やり直せる操作はありません"));
        r.push_back(rule("cannot delete the last page", "最後の 1 ページは削除できません"));
        r.push_back(rule("cannot merge the root frame", "ページ全体のコマは結合できません（隣のコマと結合するには、割ったコマを選びます）"));
        r.push_back(rule("cannot delete core layer", "基本のレイヤーは削除できません"));
        r.push_back(rule("page has no frames", "このページにはコマがありません"));
        r.push_back(rule("can only split a leaf frame", "割れるのは、まだ割っていないコマだけです"));
        r.push_back(rule("can only resize a leaf frame", "大きさを変えられるのは、割っていないコマだけです"));
        r.push_back(rule(R"(only a panel \(not a split\) takes a shape)", "形を変えられるのはコマだけです（割った線の親は変えられません）"));
        r.push_back(rule("frame_id must be a split.*", "コマの間（割った所）を選びます"));
        r.push_back(rule("the cut is too short", "割る線が短すぎます。コマを横切るように引きます"));
        r.push_back(rule("the cut does not cross the panel", "割る線がコマを横切っていません"));
        r.push_back(rule("the gutter would leave a panel too small", "それ以上動かすとコマが小さくなりすぎます"));
        r.push_back(rule("a balloon outline needs at least three points", "フキダシの形は 3 点以上で囲みます"));
        r.push_back(rule("the balloon outline is too small", "描いたフキダシが小さすぎます。もう少し大きく囲みます"));
        r.push_back(rule(R"(path must be \[\[x_mm, y_mm\], \.\.\.\])", "フキダシの形は [[x, y], …]（mm）で指定します"));
        r.push_back(rule("emphasis_runs must be a list of the words that carry dots", "傍点は、付ける言葉のリストで指定します"));
        r.push_back(rule("latin must be rotate or upright", "欧文の組み方は「寝かせる」か「立てる」です"));
        r.push_back(rule("emphasis_mark must be sesame or dot", "傍点の形は「ゴマ」か「黒丸」です"));
        r.push_back(rule("weight must be normal, bold or heavy", "文字の太さは「標準」「太」「極太」のどれかです"));
        r.push_back(rule("skew_deg must be between -60 and 60", "傾きは -60°〜60° の間で指定します"));
        r.push_back(rule("arc must be between -1 and 1", "弓なりは -1〜1 の間で指定します"));
        r.push_back(rule("no gutter there", "そこにはコマの間がありません"));
        r.push_back(rule(R"(p0 and p1 are \[x, y\] in mm)", "割る線の両端を指定します"));
        r.push_back(rule("a shape needs at least three corners.*|an area needs at least three corners", "範囲には 3 つ以上の角が要ります"));
        r.push_back(rule("area is .*", "範囲の指定が正しくありません"));
        r.push_back(rule("the area is empty", "範囲が空です"));
        r.push_back(rule("the layer is locked", "このレイヤーはロックされています（レイヤー パネルでロックを外します）"));
        r.push_back(rule("this layer cannot (be painted on|take pen lines).*", "このレイヤーには描けません。ペン・ペイント・トーンのレイヤーを選びます"));
        r.push_back(rule("layer not found|layer id or role required", "レイヤーが見つかりません"));
        r.push_back(rule("a panel is at least 4 mm across", "コマは 4 mm 以上の大きさにします"));
        r.push_back(rule("no two line ends near the trace are close enough to join.*", "なぞった所に、つなげるほど近い線の端がありません（つなぐ距離を広げる）"));
        r.push_back(rule("the trace must start and end on the same line.*", "描き直すときは、同じ線の上から描き始めて、その線の上で終えます"));
        r.push_back(rule("no line of this layer is near the trace", "なぞった所の近くに、このレイヤーの線がありません"));
        r.push_back(rule("points needs the trace: two points or more", "なぞった線が短すぎます"));
        r.push_back(rule("a cut needs its points", "削る所の点が要ります"));
        r.push_back(rule("points: where the eraser went over the balloon", "フキダシを削る所（なぞった点）が要ります"));
        r.push_back(rule("texture must be hard, soft or rough", "消しゴムの質は硬め・軟らかめ・粗めから選びます"));
        r.push_back(rule("ignore takes draft and text", "見ないものは下描きと台詞から選びます"));
        r.push_back(rule(R"(poly needs three points or more \(the lasso\))", "囲む形は 3 点以上にします"));
        r.push_back(rule("the lasso is too small", "囲んだ所が小さすぎます"));
        r.push_back(rule("nothing closed inside the lasso.*", "囲んだ中に、線で閉じた所がありません（はみ出す所やすき間から漏れる所は塗りません）"));
        r.push_back(rule("a panel needs rect .* or points around at least 25 mm²", "コマは四角か、25 mm² 以上を囲む点で描きます"));
        r.push_back(rule(R"(delete_frame takes one panel \(not a split\))", "消せるのはコマ 1 つです（割った親ではなく）"));
        r.push_back(rule("drawn panels are not merged.*", "描いたコマは結合できません。1 つ消すか、角を動かして形を変えます"));
        r.push_back(rule("drawn panels have no gutter to move.*", "描いたコマには動かす間の白がありません。コマそのものを動かすか形を変えます"));
        r.push_back(rule(R"(frame (\S+) exists)", "同じ名前のコマ（\\1）がもうあります"));
        r.push_back(rule(R"(only a panel \(not a split\) takes round corners)", "角の丸みはコマ 1 つに付けます（割った親ではなく）"));
        r.push_back(rule("the page's last panel cannot be deleted.*", "ページの最後のコマは消せません（枠線を消すなら、枠線の太さを 0 に）"));
        r.push_back(rule("that layer is not a tone", "トーンのレイヤーを選びます"));
        r.push_back(rule("layer (.+) (already )?exists", "同じ名前のレイヤーがすでにあります"));
        r.push_back(rule("unknown layer (.+)", "そのレイヤーはありません"));
        r.push_back(rule("kind must be pen, paint or folder", "レイヤーの種類は ペン・ペイント・フォルダ のどれかです"));
        // drawing, filling, selections
        r.push_back(rule("nothing to fill there.*", "線の上なので塗れません。線で囲まれた内側をクリックします"));
        r.push_back(rule("flood_fill missed the page", "ページの外は塗れません"));
        r.push_back(rule("no line there", "その範囲に線がありません"));
        r.push_back(rule("nothing to paste", "貼り付けるものがありません（先にコピーします）"));
        r.push_back(rule("the transform squashes the selection flat", "つぶれてしまう変形はできません"));
        r.push_back(rule("matrix is .*", "変形の指定が正しくありません"));
        r.push_back(rule("points needs.*", "線には 2 つ以上の点が要ります"));
        r.push_back(rule("stroke index out of range", "その線はありません"));
        r.push_back(rule("a stroke cannot cross the gutter.*", "見開きの綴じ目をまたぐ線は引けません"));
        r.push_back(rule("space spread needs a page with spread_with", "見開きにしたページでだけ使えます"));
        r.push_back(rule("put_raster needs path or png_base64|lt_convert needs a raster.*", "画像がありません"));
        r.push_back(rule("image too large.*", "画像が大きすぎます"));
        r.push_back(rule("not a readable image.*", "読み込めない画像です"));
        r.push_back(rule("the picture file of this material is missing", "この素材の画像ファイルが見つかりません"));
        r.push_back(rule("unknown material kind .*", "この素材は使えません"));
        r.push_back(rule("built-in materials cannot be deleted", "最初から入っている素材は消せません"));
        r.push_back(rule("a folder needs a name|a material needs a name", "名前を入れます"));
        // approvals and people (studio books)
        r.push_back(rule(R"(page \d+: the name is approved.*)", "このページのネームは承認済みなので、コマ割りを変えられません。変えるには承認を取り消します"));
        r.push_back(rule("set_layer exportable on a drawn layer needs name_ok.*",
                         "描いてあるレイヤーを印刷に出すのは、ネームの承認の後にできます（AI と進める原稿）"));
        r.push_back(rule(".*needs name_ok.*|ink strokes require name_ok|.*requires name_ok",
                         "AI と進める原稿では、ネームの承認の後でペン入れできます（承認の前は、ネーム・下描きと、印刷に出さない"
                         "設定にしたレイヤーに描けます。一人の原稿には、この制限はありません）"));
        r.push_back(rule(".*finish needs the art approved.*", "作画が承認されてから仕上げに進めます"));
        r.push_back(rule(".*approve the name first.*", "先にネームの承認が要ります"));
        r.push_back(rule("add_line with frame_id needs explicit x_mm/y_mm.*", "コマを指定して台詞を置くときは、x_mm と y_mm も指定します"));
        r.push_back(rule(".*needs a person.*cannot approve.*", "承認は人だけが行えます"));
        r.push_back(rule("revoke needs a person", "承認の取り消しは人だけが行えます"));
        r.push_back(rule("reject_sheet needs a person", "設定画を却下できるのは人だけです"));
        r.push_back(rule("only a person can change the policy", "進め方の設定（policy）を変えられるのは人だけです"));
        r.push_back(rule("only a person can set prompt/size overrides", "プロンプトや寸法の上書きは人だけが指定できます"));
        r.push_back(rule("only a person can mark a panel skip", "コマを「絵なし」にできるのは人だけです"));
        r.push_back(rule("only a person can unpin", "固定を外せるのは人だけです"));
        // (M5: what only a person holds, refused to an agent in the C++ build only: SPEC COMP-01a)
        r.push_back(rule(R"(only a person can set a panel's regions with set_panel.*)",
                         "コマの範囲（regions）をまとめて書き換えられるのは人だけです（AI は範囲の追加・編集・差し替えの操作を使います）"));
        r.push_back(rule(R"(only a person can change a character's approval.*)",
                         "人物の承認（固定と、承認済みの人物の設定画・顔）を変えられるのは人だけです"));
        r.push_back(rule(R"(only a person can delete an approved character \((.*)\))", "承認済みの人物 \\1 を消せるのは人だけです"));
        r.push_back(rule(R"(only a person can change a request for approval.*)", "承認の依頼を変えられるのは人だけです"));
        r.push_back(rule(R"(only a person can change the style fixed by the pilot page.*)",
                         "試し描きのページで決めた絵柄（style.locked）を変えられるのは人だけです"));
        r.push_back(rule(R"((\w+) is pinned by a person)", "\\1 は人が固定しています"));
        r.push_back(rule("a person drew this region; only a person can change it", "人が描いた範囲なので、変えられるのは人だけです"));
        r.push_back(rule("the character sheet is approved; only a person can change it", "設定画は承認済みなので、変えられるのは人だけです"));
        r.push_back(rule("only a person accepts or rejects a proposal", "提案を確定・却下できるのは人だけです"));
        r.push_back(rule(R"(character (\S+) is locked \(a person can unlock it\))", "人物 \\1 は固定されています（外せるのは人だけです）"));
        r.push_back(rule("only a person can reopen a ticket", "チケットを開き直せるのは人だけです"));
        r.push_back(rule("an agent resolves only a person's fix instruction.*", "AI が閉じられるのは、人からの直しの指示のチケットだけです（承認の依頼や質問は人が閉じます）"));
        r.push_back(rule(R"(ticket (\S+) is not open)", "チケット \\1 は開いていません"));
        r.push_back(rule(R"(no ticket (\S+))", "チケット \\1 はありません"));
        r.push_back(rule(".* is handled with the actor in apply_ops", "この操作はここではできません"));
        r.push_back(rule("cannot lock page .*", "このページは作業中にできません"));
        r.push_back(rule(R"(page (\d+) locked by (.+?)(;.*)?$)", "\\1 ページは \\2 が作業中です"));
        r.push_back(rule("project locked: .*", "この原稿は、ほかの Genko か AI が作業中です。少し待ってからやり直します"));
        r.push_back(rule("frame .* has placed art.*|panels under .* have placed art.*", "このコマには絵が置かれています。絵をどかしてから操作します"));
        r.push_back(rule("the latest change is by (.+?);.*", "直前の変更は \\1 のものなので戻せません"));
        r.push_back(rule("this (undo|redo) changes approvals.*", "承認が変わる操作は、承認・取り消しの画面から行います"));
        r.push_back(rule("project.json changed outside the journal.*", "原稿が外で書き換えられたため、戻せません"));
        // (the session's own check before a journal undo or redo of its change: app/session.cpp)
        r.push_back(rule("the latest saved change is not the one this session made", "原稿の最後の変更が、この画面でした変更ではないため、元に戻せません"));
        r.push_back(rule("the latest saved change is not the one this session undid",
                         "原稿で最後に取り消された変更が、この画面で取り消した変更ではないため、やり直せません"));
        // (a book read again while the adopted recovery point waited to be written: app/session.cpp)
        r.push_back(rule("the recovery point was not taken: the book changed meanwhile",
                         "その間に原稿が変わったため、復旧用コピーを採用していません（元に戻すか、別名で保存してください）"));
        r.push_back(rule("the project did not exist before this change", "これより前には戻せません"));
        r.push_back(rule("snapshot .* is missing .*", "戻すための記録が見つかりません"));
        // (storage::restore: a state of the history, or an asset it needs, gone from the book — an older Genko's
        // `genko gc` removes them: schema-v4 §8)
        r.push_back(rule(R"(cannot (undo past|redo) this change: its history state \S+ is missing from assets/.*)", [](const QRegularExpressionMatch& m) {
            return m.captured(1) == QLatin1String("redo")
                       ? QStringLiteral("この変更をやり直すための記録（履歴の状態）が原稿から消えているため、やり直せません。古い版の Genko の genko gc で消えることがあります。原稿は変えていません（書いたのは、要るときの履歴の記録の修復だけです）")
                       : QStringLiteral("これより前に戻すための記録（履歴の状態）が原稿から消えているため、ここより前には戻せません。古い版の Genko の genko gc で消えることがあります。原稿は変えていません（書いたのは、要るときの履歴の記録の修復だけです）");
        }));
        r.push_back(rule(R"(cannot (undo past|redo) this change: its history state \S+ needs assets that are missing.*)", [](const QRegularExpressionMatch& m) {
            return m.captured(1) == QLatin1String("redo")
                       ? QStringLiteral("この変更をやり直すのに要る絵や線の素材が原稿から消えているため、やり直せません。古い版の Genko の genko gc で消えることがあります。原稿は変えていません（書いたのは、要るときの履歴の記録の修復だけです）")
                       : QStringLiteral("これより前に戻すのに要る絵や線の素材が原稿から消えているため、ここより前には戻せません。古い版の Genko の genko gc で消えることがあります。原稿は変えていません（書いたのは、要るときの履歴の記録の修復だけです）");
        }));
        r.push_back(rule("snapshot .* does not hold the state its name says", "戻すための記録が壊れているため、戻せません。原稿は変えていません"));
        r.push_back(rule("snapshot .* cannot be restored: .*", "戻すための記録を、この版の Genko では戻せません。原稿は変えていません"));
        r.push_back(rule("revision conflict.*", "原稿がほかで変わっています。開き直してからやり直します"));
        r.push_back(rule("add_line with frame_id needs explicit x_mm/y_mm.*", "台詞の位置を指定します"));
        // paper, rulers, 3D, tones, effects, nombre
        r.push_back(rule("the paper must hold the finished size and its bleed", "用紙が、仕上がりと裁ち落としより小さくなっています"));
        r.push_back(rule("the basic frame must fit inside the finished size", "基本枠が仕上がりに収まりません"));
        r.push_back(rule("sizes must be positive", "寸法は 0 より大きくします"));
        r.push_back(rule("(paper|trim|bleed_mm|margins) must be a finite number", [](const QRegularExpressionMatch& m) {
            static const QHash<QString, QString> names = {{"paper", "用紙の大きさ"}, {"trim", "仕上がりの大きさ"},
                                                          {"bleed_mm", "裁ち落とし"}, {"margins", "基本枠の余白"}};
            return QStringLiteral("%1は、ふつうの数で指定します（無限大や NaN は使えません）").arg(names.value(m.captured(1)));
        }));
        r.push_back(rule("paper is too large: .*", "用紙が大きすぎて、ページの中身を新しい基本枠へ動かせません"));
        r.push_back(rule(R"(the paint layer on the new paper is too large \(.*\))",
                         "用紙が大きすぎて、ペイントレイヤーの絵を新しい用紙に移せません"));
        r.push_back(rule(R"(the layer mask on the new paper is too large \(.*\))", "用紙が大きすぎて、レイヤーマスクを新しい用紙に移せません"));
        // (storage::copy_assets: the other book's pictures copied before its pages are taken in)
        r.push_back(rule(R"(the asset ['"](.*)['"] is too large to copy \(at most (\d+) bytes\))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("取り込む原稿の素材ファイル（%1）が大きすぎて取り込めません（1 ファイル %2 MiB まで）")
                .arg(m.captured(1), QString::number(m.captured(2).toULongLong() / (1024 * 1024)));
        }));
        r.push_back(rule("the basic frame must be a finite size", "基本枠の大きさは、ふつうの数で指定します（無限大や NaN は使えません）"));
        r.push_back(rule(R"(a (\w+) ruler needs (\d+) point\(s\))", "この定規には点が \\2 つ要ります"));
        r.push_back(rule("a perspective ruler has 1 to 3 vanishing points", "パース定規の消失点は 1〜3 つです"));
        r.push_back(rule("ratio must be above 0", "縦横の比は 0 より大きくします"));
        r.push_back(rule("no 3D figure or box to trace", "このページに 3D がありません"));
        r.push_back(rule("kind must be box.*", "置ける 3D は箱とデッサン人形です"));
        r.push_back(rule("too many lines.*", "線が多すぎます（2000 本まで）"));
        r.push_back(rule("within is a shape.*", "効果線を描く範囲は 3 点以上の形で指定します"));
        r.push_back(rule("avoid is a list.*", "効果線の避ける範囲は、楕円 [中心 x, 中心 y, 横の半径, 縦の半径] か、3 点以上の形で指定します"));
        r.push_back(rule("lpi is 5 to 300", "線数は 5〜300 です"));
        r.push_back(rule("density is 0 to 1.*", "濃さは 0〜100% です"));
        r.push_back(rule("count is 1 to 200", "一度に足せるのは 200 ページまでです"));
        r.push_back(rule("style must be an object", "文字の設定の指定が正しくありません"));
        r.push_back(rule("unknown style key .*", "文字の設定に知らない項目があります"));
        r.push_back(rule(R"(style (\w+): .*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("文字の設定（%1）が正しくありません").arg(field(m.captured(1)));
        }));
        r.push_back(rule("unknown op: .*", "この版の Genko にない操作です"));
        r.push_back(rule(R"(ops must be a JSON array|ops\[\d+\] must be an object)", "操作の指定が正しくありません"));
        r.push_back(rule("order must .*", "並び順の指定が正しくありません"));
        r.push_back(rule(R"(no page (\d+))", "\\1 ページはありません"));
        // the approval box (app/review_model.cpp: C++ only, where Python's box stops with a KeyError or StopIteration)
        r.push_back(rule("the proposal is gone", "この提案はもうありません（ほかで片付けられました）"));
        r.push_back(rule("the proposal's page is gone", "この提案のページはもうありません"));
        r.push_back(rule("a tail needs to: .*", "しっぽの先の位置を指定します"));
        r.push_back(rule(R"(gradient \S+ is a density from 0 to 1)", "グラデーションの濃さは 0〜100% です"));
        r.push_back(rule("ops file must be a JSON array", "操作のファイルの中身が正しくありません"));
        r.push_back(rule("this .* changes approvals.*", "承認が変わる操作は、承認・取り消しの画面から行います"));
        // families
        r.push_back(rule("no (page|panel|layer|line|ruler|effect|tone|ticket|material|mannequin|3D figure or box|stroke|frame) .*",
                         [](const QRegularExpressionMatch& m) {
                             return QStringLiteral("%1が見つかりません（消されたか、別のページのものです）").arg(things().value(m.captured(1)));
                         }));
        r.push_back(rule("(?:line|ruler) .* already exists", "同じ ID がすでにあります"));
        r.push_back(rule(R"((gradient \w+|[a-z_]+) must be one of .*|(gradient \w+|[a-z_]+) must be .*)", [](const QRegularExpressionMatch& m) {
            const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            return QStringLiteral("%1の指定が正しくありません").arg(field(name));
        }));
        r.push_back(rule(R"((gradient \w+|[a-z_]+) is (?:a density from )?(\d+) to (\d+).*)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1は %2〜%3 で指定します").arg(field(m.captured(1)), m.captured(2), m.captured(3));
        }));
        r.push_back(rule(R"(([a-z_]+) is (\d+) or more)", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("%1は %2 以上にします").arg(field(m.captured(1)), m.captured(2));
        }));
        r.push_back(rule(R"(([a-z_]+(?: \(int\))?) is required)", [](const QRegularExpressionMatch& m) {
            QString name = m.captured(1);
            name.replace(QStringLiteral(" (int)"), QString());
            return QStringLiteral("%1の指定が要ります").arg(field(name));
        }));
        // (smart-objects@1, PC3 — the C++ build's own: smart objects, render/ops_smart, core/smart)
        r.push_back(rule("(?:layer \\S+ is a smart object: .*|a smart object cannot receive strokes.*)",
                         "スマートオブジェクトには直接描けません（レイヤー → ラスタライズ で画素にしてから描きます）"));
        r.push_back(rule("a smart object cannot be merged.*", "スマートオブジェクトは結合できません（先にラスタライズします）"));
        r.push_back(rule("this op cannot be used inside smart object contents", "この操作はスマートオブジェクトの中では使えません"));
        r.push_back(rule("no smart object contents .*", "スマートオブジェクトの中身が見つかりません"));
        r.push_back(rule("the smart object contents \\S+ are a file, not a page", "このスマートオブジェクトの中身はファイルのため、この操作はできません"));
        r.push_back(rule("the smart object contents \\S+ show themselves.*", "スマートオブジェクトの中に、それ自身は置けません"));
        r.push_back(rule("smart objects inside smart objects go deeper than .*", "スマートオブジェクトの入れ子は 8 段までです"));
        r.push_back(rule("layer \\S+ is not a smart object", "スマートオブジェクトを選んでください"));
        // (type@1, PC5 G-0 — the C++ build's own: type layers and the type record, render/ops_type, core/type_text)
        r.push_back(rule("a type layer holds no pixels or lines of its own.*", "テキストレイヤーには直接描けません（先にテキストレイヤーをラスタライズします）"));
        r.push_back(rule("layer \\S+ is not a type layer", "テキストレイヤーを選んでください"));
        r.push_back(rule("the type layer \\S+ holds no text", "このテキストレイヤーには文字がありません"));
        r.push_back(rule("text must not be empty", "文字が空です"));
        r.push_back(rule("(\\S+) is a question: ask it alone.*", "問い合わせ（\\1）は、原稿を変える操作と一緒には使えません"));
        r.push_back(rule("a \\S+ layer's type is not written back", "この層の type の鍵は保存されません"));
        r.push_back(rule(R"(box \[x, y, w, h\] \(mm\) is required)", "文字の枠（box）が要ります"));
        r.push_back(rule("the box's width and height must be above 0", "文字の枠の幅と高さは 0 より大きくします"));
        r.push_back(rule("line or layer_id names the text.*|line \\(a line's id\\) or page and layer_id.*",
                         "台詞（line）かテキストレイヤー（page と layer_id）を一つ指定します"));
        r.push_back(rule("a type layer moves whole: transform it without a selection", "テキストレイヤーは選択範囲なしで変形します"));
        // (PC5 G-a: point and box text, the edits in graphemes, a line and a type layer each other's)
        r.push_back(rule(R"(point \[x, y\] or box \[x, y, w, h\] \(mm\) is required)", "文字を置く点（point）か枠（box）が要ります"));
        r.push_back(rule("point or box shapes the text, not both", "ポイントテキスト（point）と段落テキスト（box）は、どちらか一つを指定します"));
        r.push_back(rule(R"(\S+ \[x, y\] \(mm\) places a point text)", "点は [x, y]（mm）で指定します"));
        r.push_back(rule(R"(\S+ \[x, y, w, h\] \(mm\) places a box text)", "枠は [x, y, w, h]（mm）で指定します"));
        r.push_back(rule("a text in a balloon is set in it.*", "フキダシに入った文字は、ポイントテキスト・段落テキストにできません（先にフキダシを「なし」にします）"));
        r.push_back(rule("text, replace or insert edits the text, one of them", "文字の直し方（text・replace・insert）は一つだけ指定します"));
        r.push_back(rule(R"(.* must be within the text \(0\.\.(\d+) graphemes\))", "文字の位置が本文の外です（0〜\\1 文字）"));
        r.push_back(rule(R"(.* must be a whole number \(graphemes\))", "文字の位置は整数（文字の数）で指定します"));
        r.push_back(rule("replace end must not be before its start", "置き換える範囲の終わりが始まりより前です"));
        r.push_back(rule(R"((?:replace is \{start, end, text\}|insert is \{at, text\}|replace text must be a string|insert text must be a string, not empty))",
                         "文字の置き換え・挿入の指定が正しくありません"));
        r.push_back(rule("anchor moves a point text's anchor.*", "基準点を動かせるのはポイントテキストだけです"));
        r.push_back(rule("antialias must be one of .*", "アンチエイリアスは なし・シャープ・鮮明・強く・滑らかに・Windows LCD・Windows のどれかです"));
        r.push_back(rule("(?:a line stays upright.*|the line \\S+ stays upright.*)", "台詞は回転・拡大・ゆがみができません（テキストレイヤーにしてから変形します）"));
        r.push_back(rule("a type layer turned, scaled or slanted stays a type layer.*", "回転・拡大・ゆがみをかけたテキストレイヤーは台詞にできません（先に変形を外します）"));
        r.push_back(rule("a type layer is bent by .*", "テキストレイヤーはワープテキストで曲げます（ほかの曲げ方はスマートオブジェクトにしてから）"));
        r.push_back(rule("size_pt or style.size_mm sizes the text, not both", "文字の大きさは pt か mm のどちらかで指定します"));
        r.push_back(rule("size_pt must be above 0 and at most 1296", "文字の大きさは 0 より大きく 1296 pt までです"));
        r.push_back(rule("parent must be a folder of the page", "入れる先は、このページのフォルダーを指定します"));
        r.push_back(rule("edit_type changes .*", "直す中身（文字・形・向き・アンチエイリアス・変形・名前）を指定します"));
        r.push_back(rule("title names a type layer: a line has none", "名前はテキストレイヤーにだけ付けられます"));
        r.push_back(rule("the type layer is too large to turn at this resolution", "テキストレイヤーが大きすぎて、この解像度では回して描けません"));
        r.push_back(rule("page and layer_id name the type layer|line \\(a line's id\\) names the line", "台詞（line）かテキストレイヤー（page と layer_id）を指定します"));
        // (PC5 G-a review)
        r.push_back(rule(".* must lie within a metre of the page.*", "文字の位置と枠は、ページから 1 m の内（枠の幅と高さは 1 m まで）で指定します"));
        r.push_back(rule("the text is too large to set.*", "文字が大きすぎて組めません（ポイントテキストの行が長すぎるか、段落テキストが枠から大きくはみ出しています）"));
        r.push_back(rule("no panel \\S+ on the page", "そのページに無いコマです"));
        r.push_back(rule("after names a line of the page", "読む順の位置（after）は、同じページの台詞を指定します"));
        r.push_back(rule("a type layer turned, scaled or bent is not in this build yet",
                         "テキストレイヤーの回転・拡大・縮小・ゆがみは、この版の Genko ではまだできません（移動はできます）"));
        // (PC5 G-b: set_type — the character and paragraph attributes on the whole text or a range)
        r.push_back(rule("unknown attribute \\S+ \\(a character or paragraph attribute of TEXT-02\\)", "文字・段落の属性の名前が正しくありません"));
        r.push_back(rule("set_type sets a character or paragraph attribute.*", "変える文字・段落の属性を指定します"));
        r.push_back(rule(R"(the text is too long to set \((\d+) letters at most\))", "文字が長すぎて組めません（\\1 文字まで）"));
        r.push_back(rule(R"(range is \[start, end\] \(graphemes\))", "範囲は [始まり, 終わり]（文字の数）で指定します"));
        r.push_back(rule(R"(range must lie in the text.*\(0\.\.(\d+) graphemes\))", "範囲が本文の外か、終わりが始まりより前です（0〜\\1 文字）"));
        r.push_back(rule("an empty range takes paragraph attributes only", "空の範囲には段落の属性だけを指定できます"));
        r.push_back(rule("size_pt or size_mm sizes the text, not both", "文字の大きさは pt か mm のどちらかで指定します"));
        r.push_back(rule(".* takes the style \\S+, which the book does not keep", "原稿に無い文字スタイル・段落スタイルを指定しています"));
        r.push_back(rule("(?:unknown )?(?:native_type|char|para|run char|paragraph para|type_styles)\\b.*",
                         "文字の設定（文字・段落の属性、スタイル）の指定が正しくありません"));
        r.push_back(rule("(?:a run(?:'s| must) |the runs must |a paragraph must |the paragraphs must |a style's (?:id|name) |two (?:character|paragraph) styles ).*",
                         "文字の範囲・段落・スタイルの指定が正しくありません"));
        // (variables@1, PC5 I-d — the C++ build's own: the variables and data sets, render/ops_variables, formats/datasets; a
        // variable's name may hold spaces — the review's P3-3 — so a name is taken as (.+?), not (\S+))
        r.push_back(rule(R"(variable (.+?): (.*))", [](const QRegularExpressionMatch& m) {
            return QStringLiteral("変数「%1」: %2").arg(m.captured(1), error(m.captured(2)));
        }));
        r.push_back(rule("no data set at index (\\S+)", "\\1 番目のデータセットはありません"));
        r.push_back(rule("no data set (.*)", "「\\1」という名前のデータセットはありません"));
        r.push_back(rule("name or index names the data set", "データセットは名前か番号で指定します"));
        r.push_back(rule("the book has no variables: define them first.*", "原稿に変数がありません（先に イメージ → 変数 → 定義… で定義します）"));
        r.push_back(rule("the book has no data sets", "原稿にデータセットがありません"));
        r.push_back(rule("no data sets chosen", "書き出すデータセットが選ばれていません"));
        r.push_back(rule("no page to write: the variables name no page of the book", "変数のあるページがないため、書き出すページがありません"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?no variable (.*))", "「\\1」という変数はありません"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?(.+?) \(visibility\) takes true or false)", "変数「\\1」（表示）の値は true か false です"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?(.+?) \(text\) takes text)", "変数「\\1」（テキストの置き換え）の値は文字です"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?(.+?) \(pixels\) takes a picture.*)", "変数「\\1」（ピクセルの置き換え）の値は画像です"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?the picture cannot be read.*)", "データセットの画像が読めません"));
        r.push_back(rule(R"((?:data_sets\[\d+\]\.values\[\d+\]: )?the picture is larger than 256 MiB)", "データセットの画像が大きすぎます（256 MiB まで）"));
        r.push_back(rule("the data sets' pictures are larger than 4 GiB together", "データセットの画像が、あわせて 4 GiB を越えます"));
        r.push_back(rule(R"(defs\[\d+\]: layer (\S+) is not a type layer)", "テキストの置き換えにできるのは文字レイヤーか台詞だけです"));
        r.push_back(rule(R"(defs\[\d+\]: layer (\S+) is not a paint layer or a smart object)", "ピクセルの置き換えにできるのはペイントのレイヤーかスマートオブジェクトだけです"));
        r.push_back(rule(R"(defs\[\d+\]: no line (.*))", "その台詞は原稿にありません"));
        r.push_back(rule(R"(defs\[\d+\]: a line takes a text variable only)", "台詞に使えるのはテキストの置き換えの変数だけです"));
        r.push_back(rule("two variables are named (.*)", "「\\1」という名前の変数が 2 つあります（変数の名前は 1 つずつ）"));
        r.push_back(rule("two data sets are named (.*)", "「\\1」という名前のデータセットが 2 つあります（データセットの名前は 1 つずつ）"));
        r.push_back(rule(R"(def \d+'s name must be 1 to 255 characters|data set \d+'s name must be 1 to 255 characters)", "変数・データセットの名前は 1〜255 文字です"));
        r.push_back(rule(R"(data set \d+ gives (.*) two values)", "1 つのデータセットに変数「\\1」の値が 2 つあります"));
        r.push_back(rule(R"(at most (\d+) (?:variables|defs))", "変数は \\1 個までです"));
        r.push_back(rule(R"(at most (\d+) data sets)", "データセットは \\1 個までです"));
        r.push_back(rule(R"(a printed layer's pixels change only after name_ok on page (\S+) \(strict_gates\))",
                         "\\1 ページのネームが承認されるまで、印刷するレイヤーの画素は変えられません"));
        r.push_back(rule("the text file is not UTF-8.*", "テキストファイルが UTF-8 ではありません（CSV UTF-8 で保存します）"));
        r.push_back(rule("the text file is larger than 64 MiB", "テキストファイルが大きすぎます（64 MiB まで）"));
        r.push_back(rule("the text file holds no line of names", "テキストファイルに変数の名前の行がありません"));
        r.push_back(rule(R"(the text file holds more than (\d+) data sets)", "データセットは \\1 個までです"));
        r.push_back(rule("a quoted field is not closed", "テキストファイルの \" で囲んだ欄が閉じていません"));
        r.push_back(rule(R"(row (\d+): the picture (.*) cannot be read)", "\\1 行目の画像「\\2」が読めません"));
        r.push_back(rule(R"(row (\d+): the picture (.*) is larger than 256 MiB)", "\\1 行目の画像「\\2」が大きすぎます（256 MiB まで）"));
        // (groups@1, PC5 I-b — the C++ build's own: a folder drawn as a group, core/groups, render/ops_groups)
        r.push_back(rule("group makes a folder.*|group is set on a folder|.*the layer \\S+ is marked a group but is not a folder",
                         "グループとして描けるのはフォルダーだけです"));
        r.push_back(rule(".*(?:blend pass_through is a group's|the blend pass_through is a group's only).*", "描画モード「通過」はグループだけのものです"));
        r.push_back(rule("the layers to merge are in different groups.*", "別々のグループのレイヤーは一緒に結合できません（グループの中で結合します）"));
        r.push_back(rule("the layers have nothing to draw", "選んだレイヤーには、描かれたものがありません"));
        r.push_back(rule("layer \\S+ has nothing to rasterize", "このレイヤーには、ラスタライズするものがありません（すでに画素のレイヤーです）"));
        r.push_back(rule("the transform squashes the smart object flat|the picture would be laid too small", "スマートオブジェクトが潰れてしまう変形はできません"));
        r.push_back(rule("the picture is too large to keep its 16 or 32 bits on this page.*", "画像が大きすぎて、このページでは 16・32 bit のまま置けません"));
        r.push_back(rule("(?:the asset's picture cannot be read \\()?the file is larger than 256 MiB\\)?", "ファイルが大きすぎます（256 MiB まで）"));
        r.push_back(rule("(?:the asset's picture cannot be read \\()?the file is not .* its name says\\)?", "ファイルの中身が、名前の形式と違います"));
        r.push_back(rule("(?:the asset's picture cannot be read \\()?the file is not a picture this build reads\\)?", "読み込めない画像です"));
        r.push_back(rule("no asset \\S+ of a picture.*", "画像の素材が見つかりません"));
        r.push_back(rule("the smart object's contents are a page, not a file.*", "スマートオブジェクトの中身はページのため、色の扱いは中のレイヤーのものです"));
        r.push_back(rule("embedded_profile is keep, convert or assign", "プロファイルの扱いは「値のまま」「埋め込みから変換」「作業用とみなす」のどれかです"));
        // (PC3 A1: contents and links, render/ops_smart_contents)
        r.push_back(rule("the smart object has no contents", "スマートオブジェクトの中身が見つかりません"));
        r.push_back(rule("the smart object is not linked to a file", "このスマートオブジェクトはファイルにリンクしていません"));
        r.push_back(rule("the smart object's contents are a page already", "このスマートオブジェクトの中身は、すでに編集できるページです"));
        r.push_back(rule("contents of a page are linked as the file they were written to.*", "ページの中身は、書き出したファイルとしてリンクします"));
        r.push_back(rule("a file's contents are linked as they are.*", "ファイルの中身は、そのままのファイルとしてリンクします"));
        r.push_back(rule("asset \\(sha256:…\\) or data gives the file|the file is an asset ref .*", "読み込むファイルが指定されていません"));
        r.push_back(rule("(?:link is \\{path, relative\\?\\}|link names the file's path|link's bytes is the file's size|its link.*)",
                         "リンクの記録（ファイルの場所）が正しくありません"));
        r.push_back(rule("the smart object's file is not in the book.*", "スマートオブジェクトのファイルが原稿にありません"));
        r.push_back(rule("the package's place holds a book already: .*", "そのフォルダーには、もう同じ名前の原稿があります（別のフォルダーを選びます）"));
        r.push_back(rule("the book is not whole, so it is not packed: .*", "原稿に欠けたものがあるため、パッケージにできません"));
        r.push_back(rule("(?:a file's contents are written as they are|a page's contents are written as psd or png).*",
                         "この中身は、その形式では書き出せません"));
        // (PC3 A2: smart filters, render/ops_smart_filters)
        r.push_back(rule("the smart object has no smart filters", "このスマートオブジェクトには、スマートフィルターがありません"));
        r.push_back(rule("the smart object has no filter mask", "このスマートオブジェクトには、フィルターマスクがありません"));
        r.push_back(rule("a filter plugin cannot be a smart filter.*", "プラグインのフィルターは、スマートフィルターにできません（ラスタライズしてからかけます）"));
        r.push_back(rule("the filter \\S+ cannot be a smart filter", "このフィルターは、スマートフィルターにできません（ラスタライズしてからかけます）"));
        r.push_back(rule("a smart filter works on the whole picture, not on one channel", "スマートフィルターは、1 つのチャンネルだけにはかけられません"));
        r.push_back(rule("a smart filter's colour lookup is one of the looks.*",
                         "スマートフィルターのカラールックアップは、用意された見た目だけを使えます（ファイルの表は色調補正のレイヤーで使います）"));
        r.push_back(rule("a smart object holds at most \\d+ smart filters", "スマートフィルターは 1 つのスマートオブジェクトに 64 個までです"));
        r.push_back(rule("this page does not composite the blend mode \\S+", "このページの色では、その描画モードは使えません"));
        r.push_back(rule("(?:index|to) is the filter's place .*", "スマートフィルターの位置の指定が正しくありません"));
        r.push_back(rule("mask from_area takes the area", "フィルターマスクにする選択範囲がありません"));
        // (PC3 A4: stack modes and lines rasterized, render/ops_smart)
        r.push_back(rule("mode is a stack mode .*", "スタックモードの名前が正しくありません"));
        r.push_back(rule("ids \\(or id\\) names the lines|ids is a list of the lines' ids", "ラスタライズする台詞を選んでください"));
        r.push_back(rule("line \\S+ is not on page \\S+", "その台詞は、このページにありません"));
        r.push_back(rule("line \\S+ has no balloon on the page", "その台詞はフキダシに入っていないため、ラスタライズできません"));
        r.push_back(rule("the lines are drawn at different places among the layers.*",
                         "選んだ台詞はレイヤーの重ね順の中の置き場所が違うため、まとめてラスタライズできません（1 つずつラスタライズします）"));
        // (PC3 B1: choosing by colour, the Quick Mask, the Magic Eraser — render/select_color, ops_quick_mask, erase_magic)
        r.push_back(rule("page \\S+ is in Quick Mask already", "このページは、すでにクイックマスクモードです"));
        r.push_back(rule("page \\S+ is not in Quick Mask", "このページは、クイックマスクモードではありません"));
        r.push_back(rule("the Quick Mask stays an alpha channel", "クイックマスクのチャンネルは、特色チャンネルにできません"));
        r.push_back(rule("(?:grow|similar): the area is empty", "先に選択範囲を作ります"));
        r.push_back(rule("range: Sampled Colors needs colors or points_mm", "指定色域には、色かスポイトで拾った点が要ります"));
        r.push_back(rule("range: localized clusters need points_mm", "ローカライズされたカラークラスタには、スポイトで拾った点が要ります"));
        r.push_back(rule("range(?:\\.\\w+)?[: ].*", "色域指定の設定が正しくありません"));
        r.push_back(rule("a selection is saved into an alpha channel", "選択範囲は、アルファチャンネルに保存します（特色チャンネルには保存できません）"));
        // (PC3 B4: the retouching ops — render/ops_retouch)
        r.push_back(rule("source_mm \\(where to sample from\\) or offset_mm is required", "先にコピー元を決めます（Alt キーを押しながらクリック）"));
        r.push_back(rule("from is not the pixels of this layer .*cannot be painted back",
                         "その状態からは、このレイヤーに塗り戻せません（レイヤーの種類か大きさが違います）"));
        r.push_back(rule("history_brush reads the pixels before from the book's folder.*", "ヒストリーブラシの前に、原稿を保存します"));
        r.push_back(rule("content_aware_fill: the area covers nothing of the layer", "埋める所（選択範囲）が、このレイヤーにありません"));
        r.push_back(rule("content_aware_fill: source_area covers nothing of the layer", "カスタムのサンプリング領域が、このレイヤーにありません"));
        r.push_back(rule("custom sampling needs source_area.*", "カスタムのサンプリング領域を選びます"));
        r.push_back(rule("the layer \\S+ has no pixels to work on", "このレイヤーには、直す画素がありません"));
        // (PC3 B3: the smart selection and the matting — render/select_smart, render/ops_matting)
        r.push_back(rule("kind is defringe, remove_black, remove_white or decontaminate",
                         "マッティングの種類は、カラーの除去・フリンジ削除・黒マット削除・白マット削除から選びます"));
        r.push_back(rule("id \\(the layer\\) is required", "レイヤーを選んでから行います"));
        r.push_back(rule("radius_mm must be more than 0", "半径は 0 より大きくします"));
        r.push_back(rule("(?:subject|sky|object|quick|focus|refine): no layer .*", "そのレイヤーはありません"));
        r.push_back(rule("refine\\.area \\(the selection to refine\\) is required", "先に選択範囲を作ります"));
        r.push_back(rule("(?:subject|sky|object|quick|focus|refine)(?:\\.\\w+)?[: ].*",
                         "賢い選択（被写体・空・オブジェクト選択・クイック選択・フォーカス領域・選択とマスク）の設定が正しくありません"));
        // (vector@1, PC3 C-a: paths and vector masks, render/ops_paths, render/path_areas)
        r.push_back(rule("the page has no work path", "このページには作業用パスがありません（ペンツールで描くか、選択範囲から作ります）"));
        r.push_back(rule("no path named '(.*)'", "パス「\\1」はありません"));
        r.push_back(rule("a path named '(.*)' is already on the page", "パス「\\1」はもうあります。ほかの名前にします"));
        r.push_back(rule(R"(to cannot be the work path.*)", "「work」は作業用パスの名前です。ほかの名前にします"));
        r.push_back(rule(R"((?:name|to): .*(?:empty|longer|characters).*)", "パスの名前は 1〜64 文字にします"));
        r.push_back(rule("the path has nothing to stroke", "パスに、線を引ける所がありません"));
        r.push_back(rule("the layer has no vector mask.*", "このレイヤーにはベクトルマスクがありません"));
        r.push_back(rule("layer \\S+ has no vector mask", "このレイヤーにはベクトルマスクがありません"));
        r.push_back(rule("vector_mask: the layer \\S+ has no vector mask", "そのレイヤーにはベクトルマスクがありません"));
        r.push_back(rule("the layer mask of layer \\S+ is off: enable or delete it before rasterizing the vector mask",
                         "このレイヤーのレイヤーマスクは「使用しない」になっています。ベクトルマスクをラスタライズする前に、レイヤーマスクを使用するか削除します"));
        r.push_back(rule("the vector mask of layer \\S+ is off over a layer mask: enable or delete it before rasterizing it",
                         "このレイヤーのベクトルマスクは「使用しない」になっています（レイヤーマスクもあります）。ラスタライズする前に、ベクトルマスクを使用するか削除します"));
        r.push_back(rule("a folder cannot take a vector mask.*", "フォルダにはベクトルマスクを付けられません（中のレイヤーに付けます）"));
        r.push_back(rule("give one of path, path_name, reveal_all and hide_all", "ベクトルマスクの形は、パス・すべての領域を表示・すべての領域を隠すのどれか 1 つで指定します"));
        r.push_back(rule("the line \\S+ has no letters to trace", "この台詞には、パスにする文字がありません"));
        r.push_back(rule("the line \\S+ is not placed on a page", "この台詞はページに置かれていません"));
        r.push_back(rule("blend paints a paint layer's pixels.*", "描画モードを付けた塗りつぶしは、ペイントレイヤーにだけできます"));
        r.push_back(rule("shape_part: the layer \\S+ has no shape part.*", "そのレイヤーに、その形はありません"));
        // (PC3 C-e: the page size ops, render/ops_pagesize; core/page_geometry)
        r.push_back(rule("the finished size and its bleed would not fit the new paper.*",
                         "仕上がりと裁ち落としが新しい用紙に入りません（仕上がり・裁ち落とし・余白を決め直すか、仕上がりを新しい用紙に合わせます）"));
        r.push_back(rule("the basic frame would not fit the finished size.*", "基本枠が仕上がりに入りません（余白を決め直すか、仕上がりを新しい用紙に合わせます）"));
        r.push_back(rule("the new (?:paper|canvas) must have a size", "新しい用紙の大きさが 0 以下になります"));
        r.push_back(rule(R"(the page on its new paper is too large \(.*\))", "新しい用紙が大きすぎます"));
        r.push_back(rule(R"(the saved area on the new paper is too large \(.*\))", "用紙が大きすぎて、ストックした範囲を新しい用紙に移せません"));
        r.push_back(rule("the precise layer on the new paper is too large", "用紙が大きすぎて、高精度のレイヤーを新しい用紙に移せません"));
        r.push_back(rule("the inks on the new paper are too large", "用紙が大きすぎて、インキのレイヤーやチャンネルを新しい用紙に移せません"));
        r.push_back(rule(R"(page (\S+) is a cover: its paper is set by its cover settings)",
                         "\\1 ページは表紙・カバー・帯です。用紙は表紙の設定で決まるため、大きさは変えられません"));
        r.push_back(rule(R"(page (\S+) is in a spread with page (\S+): change both pages)", "\\1 ページは \\2 ページと見開きです。両方のページに掛けてください"));
        r.push_back(rule(R"(page (\S+) is in a spread with page (\S+): both must keep the same size)", "見開きの \\1・\\2 ページは同じ大きさにします"));
        r.push_back(rule("the resolution is the whole book's: give pages \"all\"", "解像度は本全体で変えます（すべてのページに掛けてください）"));
        r.push_back(rule("resample none keeps the pixels: give dpi alone", "再サンプルしないときは、解像度だけを変えます（大きさは解像度に合わせて変わります）"));
        r.push_back(rule("give the size in mm or in pixels, not both", "大きさは mm か pixel のどちらかで指定します"));
        r.push_back(rule("dpi is a whole number of 1 to 10000", "解像度は 1〜10000 の整数です"));
        r.push_back(rule("resample is bicubic, .*", "再サンプルの方法は バイキュービック・バイリニア・ニアレストネイバー・Lanczos・ディテールを保持・しない のどれかです"));
        r.push_back(rule("anchor is top_left, .*", "基準位置の指定が正しくありません"));
        r.push_back(rule("extension is \"transparent\" or \\[r, g, b\\]", "カンバス拡張カラーは 透明 か [r, g, b] です"));
        r.push_back(rule("the canvas extension colour needs a paint layer at the bottom of the page.*",
                         "カンバス拡張カラーで塗るペイントレイヤーが、ページのいちばん下にありません（透明で広げるか、ペイントレイヤーを作ってから）"));
        r.push_back(rule("delete_pixels must be true.*", "切り抜いた画素は残せません（線・絵・台詞は用紙の外に残り、すべての領域を表示 で戻せます）"));
        r.push_back(rule("give rect \\[x, y, w, h\\] or an area to crop to", "切り抜く枠か範囲を指定します"));
        r.push_back(rule("rect must (?:be finite numbers|have a size)", "切り抜く枠の指定が正しくありません"));
        r.push_back(rule("angle is -359.99 to 359.99 degrees", "角度は -359.99〜359.99° です"));
        r.push_back(rule("interp is bicubic, bilinear or nearest", "補間方法は バイキュービック・バイリニア・ニアレストネイバー のどれかです"));
        r.push_back(rule("based_on is transparent, top_left or bottom_right", "トリミングの基準は 透明ピクセル・左上のピクセルの色・右下のピクセルの色 のどれかです"));
        r.push_back(rule(R"(page (\S+) has nothing to trim to)", "\\1 ページには、トリミングの基準になる描いたものがありません"));
        r.push_back(rule("text is upright: .*", "台詞の文字は立てたまま回します（文字だけを回す・鏡にすることはできません）"));
        r.push_back(rule("turn is 90cw, 90ccw, 180, flip_h or flip_v", "回し方は 90°（時計回り・反時計回り）・180°・左右反転・上下反転 のどれかです"));
        r.push_back(rule("give turn or angle, not both", "回し方か角度の、どちらかを指定します"));
        r.push_back(rule("pages is a list of page numbers or \"all\"", "ページは番号のリストか、すべて（\"all\"）で指定します"));
        r.push_back(rule("trim is \\[width, height\\] or \"fit\"|trim must be positive numbers", "仕上がりは [幅, 高さ] か、用紙に合わせる（\"fit\"）で指定します"));
        r.push_back(rule("bleed_mm must not be negative", "裁ち落としは 0 以上にします"));
        r.push_back(rule("margins (?:is \\[top, bottom, inner, outer\\]|must be finite numbers|takes top, bottom, inner and outer)",
                         "余白は [天, 地, のど, 小口] で指定します"));
        r.push_back(rule("a 3D prop cannot be turned or mirrored with the page.*", "3D は、ページと一緒に回す・反転することはできません（3D を線と面にしてから）"));
        // (pen-shapes@1, PC3 C-b: the shape parts of pen layers, render/ops_shapes)
        r.push_back(rule("a shape part goes on a pen layer.*", "シェイプはペンのレイヤーに置けます（「新しいレイヤー」を入れると、ペンのレイヤーを作って置きます）"));
        r.push_back(rule("the layer \\S+ has no shape part .*", "そのレイヤーに、そのシェイプはありません"));
        r.push_back(rule("the layer \\S+ has a shape part \\S+ already", "そのレイヤーには、同じ id のシェイプがもうあります"));
        r.push_back(rule("choose two or more shape parts to combine", "結合するシェイプを 2 つ以上選びます"));
        r.push_back(rule("the shape (?:is|would be) empty", "シェイプの形が空になります"));
        r.push_back(rule("the area is empty", "先に選択範囲を作ります"));
        r.push_back(rule("a pen layer holds at most (\\d+) shape parts", "1 つのペンのレイヤーに置けるシェイプは \\1 個までです"));
        r.push_back(rule("the shape would have more than (\\d+) knots", "シェイプの点が多すぎます（\\1 個まで）"));
        r.push_back(rule("the matrix squashes the shape flat", "その変形では、シェイプがつぶれてしまいます"));
        r.push_back(rule("\\S+ is a shape part: its anchors and handles are its path.*", "シェイプの点とハンドルは、パス選択ツールで動かします"));
        r.push_back(rule("\\S+ is a shape part, not a line", "シェイプは線ではありません（パス選択ツールで直します）"));
        r.push_back(rule(".*a pattern fill is not in this build yet", "パターンの塗りは、この版の Genko ではまだ使えません"));
        r.push_back(rule("shape's kind is .*|a shape of kind \\S+ needs .*|shape has no key .*", "シェイプの形の指定が正しくありません"));
        // (PC3 C-c: the transforms, Content-Aware Scale, Liquify's strokes, Warp Text — render/layer_transform, distort_ops)
        r.push_back(rule("the layer \\S+ is locked", "ロックされているレイヤーは変形できません（レイヤー パネルでロックを外します）"));
        r.push_back(rule("the position of the layer \\S+ is locked", "位置をロックしたレイヤーは動かせません（レイヤー パネルでロックを外します）"));
        r.push_back(rule("a (?:smart object|placed picture) moves whole: .*", "選択範囲の中だけを変形することはできません（選択を解除してから変形します）"));
        r.push_back(rule("a placed picture is turned and bent as a smart object: .*", "配置画像を回す・曲げるときは、先にスマートオブジェクトに変換します"));
        // (PC3 A5: a smart object's warp and corners, its perspective warp, puppet warp and Liquify kept as smart filters)
        r.push_back(rule("a smart object's corners must make a convex shape", "スマートオブジェクトの 4 隅が、へこみの無い四角形になりません"));
        r.push_back(rule("perspective is the contents' four corners on the page.*", "スマートオブジェクトの 4 隅の指定が正しくありません"));
        r.push_back(rule("a smart object is puppet-warped alone.*", "スマートオブジェクトのパペットワープは、そのスマートオブジェクトだけを選んでかけます"));
        r.push_back(rule("a smart object is bent by a warp, a perspective warp or a puppet warp", "スマートオブジェクトは、ワープ・遠近法ワープ・パペットワープで曲げます"));
        r.push_back(rule("(?:liquify has no key|liquify's settings|mesh_mm must be) .*", "ゆがみの設定が正しくありません"));
        r.push_back(rule("the transform squashes the layers flat", "つぶれてしまう変形はできません"));
        r.push_back(rule("give one of move, matrix, quad, turn, warp, perspective or puppet", "変形のしかたを 1 つだけ指定します"));
        r.push_back(rule("the layers show nothing to (?:transform|pin)", "変形するものがありません"));
        r.push_back(rule("the quad's corners must make a quad|a plane is degenerate: .*", "4 隅が四角形になっていません"));
        r.push_back(rule("ids is \\[layer id, …\\]", "変形するレイヤーを選んでください"));
        r.push_back(rule("(?:the |a )?warp(?:'s)? .*|warp (?:is|has) .*", "ワープの設定が正しくありません"));
        r.push_back(rule("(?:a plane's|perspective|a perspective warp|straighten) .*", "遠近法ワープの設定が正しくありません"));
        r.push_back(rule("(?:puppet|a puppet warp|a pin's) .*", "パペットワープの設定が正しくありません"));
        r.push_back(rule("Content-Aware Scale works on a paint layer's pixels: .*",
                         "コンテンツに応じて拡大・縮小は、ペイントのレイヤーの画素にかけます（ペンのレイヤーやスマートオブジェクトは先にラスタライズします）"));
        r.push_back(rule("nothing to scale: the layer is empty there", "拡大・縮小する絵がありません"));
        r.push_back(rule("protect is an alpha channel's id or name, or none", "保護するチャンネルは、アルファチャンネルを選びます"));
        r.push_back(rule("Liquify works on a pen or paint layer", "ゆがみは、ペンかペイントのレイヤーにかけます"));
        r.push_back(rule("(?:strokes is|liquify takes|a liquify stroke) .*", "ゆがみの設定が正しくありません"));
        r.push_back(rule("style a line's native_warp .*", "ワープテキストの設定が正しくありません"));
        // (PC3 C-d: align, distribute, arrange, link — render/layer_align)
        r.push_back(rule("edge is top, vcenter, bottom, left, hcenter or right", "そろえる辺の指定が正しくありません"));
        r.push_back(rule("to is auto, layers, selection, page, trim, frame or panel", "そろえる基準の指定が正しくありません"));
        r.push_back(rule("the layers show nothing to align", "整列するものがありません（描いてあるレイヤーか台詞を選びます）"));
        r.push_back(rule("there is no selection to align to", "先に選択範囲を作ります"));
        r.push_back(rule("frame_id names the panel to align to", "先にそろえるコマを選びます"));
        r.push_back(rule("by is top, vcenter, bottom, left, hcenter, right, horizontal or vertical", "分布のしかたの指定が正しくありません"));
        r.push_back(rule("distributing needs three or more layers or lines that show something", "分布するには、描いてあるレイヤーか台詞を 3 つ以上選びます"));
        r.push_back(rule("to is front, forward, backward, back or reverse", "重ね順の指定が正しくありません"));
        r.push_back(rule("choose two or more layers to reverse", "逆順にするには、レイヤーを 2 つ以上選びます"));
        r.push_back(rule("the layers cannot move further", "これ以上は動かせません"));
        r.push_back(rule("link is true or false", "リンクの指定が正しくありません"));
        r.push_back(rule("choose two or more layers to link", "リンクするには、レイヤーを 2 つ以上選びます"));
        r.push_back(rule("lines is \\[line id, …\\]", "台詞の指定が正しくありません"));
        // (the C++ build's own: what it does not do yet, and saving)
        r.push_back(rule("official export is not in the C\\+\\+ build yet",
                         "この版の Genko では、まだ正式な書き出し（確認と承認の記録つき）はできません（ふつうの書き出しはできます）"));
        r.push_back(rule(R"((?:ops\[\d+\] )?(\S+) is not in the C\+\+ build yet.*)", "この版の Genko では、まだその操作（\\1）はできません"));
        r.push_back(rule("this book is open read-only: .*", "この原稿は読み取り専用で開いています（変更できません）"));
        r.push_back(rule("the book was closed", "この原稿は閉じられています"));
        return r;
    }();
    return table;
}

QString inner(const QString& text) {
    const QString shown = error(text);
    return shown.startsWith(QStringLiteral("この操作はできませんでした")) ? text : shown;
}

}  // namespace

QString stage(std::string_view key) {
    static const QHash<QString, QString> table = {{"name", "ネーム"}, {"ink", "作画"}, {"finish", "仕上げ"}};
    const QString k = qs(key);
    return table.value(k, k);
}

QString layer_role(std::string_view role) {
    static const QHash<QString, QString> table = {
        {"name", "ネーム"}, {"draft", "下描き・アタリ"}, {"ink", "ペン入れ"}, {"bg", "背景"}, {"finish", "仕上げ"},
        {"tone", "トーン"}, {"effect", "効果"}, {"frames", "コマ枠"}, {"text", "文字"}, {"user", "レイヤー"},
    };
    const QString k = qs(role);
    return table.value(k, k);
}

QString actor(std::string_view name) {
    const QString n = qs(name);
    if (n.startsWith(QStringLiteral("ai:"))) return QStringLiteral("AI（%1）").arg(n.mid(3));
    if (n.startsWith(QStringLiteral("human:"))) return n.mid(6);
    return n.isEmpty() ? QStringLiteral("不明") : n;
}

QString panel_status(const core::Json& status) {
    static const QHash<QString, QString> table = {{"empty", "未着手"},      {"briefed", "指示あり"}, {"requested", "依頼済み"}, {"candidates", "候補あり"},
                                                  {"adopted", "採用済み"},  {"fix_requested", "直し待ち"}, {"skip", "絵なし"}};
    if (status.is_null()) return QStringLiteral("未着手");
    if (status.is_string()) {
        const QString k = QString::fromStdString(status.get<std::string>());
        return table.value(k, k);
    }
    return QString::fromStdString(core::py_str(status));
}

QString gate(std::string_view key) {
    static const QHash<QString, QString> table = {{"name", "ネーム"}, {"art", "作画"}, {"sheet", "設定画"}, {"export", "書き出し"}};
    const QString k = qs(key);
    return table.value(k, k);
}

const std::vector<std::pair<QString, QString>>& regions() {
    static const std::vector<std::pair<QString, QString>> list = {
        {QStringLiteral("face"), QStringLiteral("顔")}, {QStringLiteral("person"), QStringLiteral("人物")}, {QStringLiteral("keep"), QStringLiteral("空けておく")}};
    return list;
}

QString region_label(const core::Json& kind) {
    if (kind.is_string()) {
        const QString k = QString::fromStdString(kind.get<std::string>());
        for (const auto& [key, label] : regions()) {
            if (key == k) return label;
        }
        return k;
    }
    return QString::fromStdString(core::py_str(kind));
}

QString layer_label(const core::Layer& layer) {
    const QString title = qs(layer.title).trimmed();
    const QString role = qs(core::to_string(layer.role));
    QString base = layer_role(core::to_string(layer.role));
    if (layer.kind == core::LayerKind::Placed) base = QStringLiteral("絵（配置）");
    if (layer.kind == core::LayerKind::Smart) base = QStringLiteral("スマートオブジェクト");  // (smart-objects@1)
    // titles made of ids (placed art) are not names people gave
    static const QRegularExpression made(
        QRegularExpression::anchoredPattern(QStringLiteral(R"((art|ink|placed)?\s*[0-9a-f]{8,}|placed (art|bg|draft|name)|layer)")));
    if (!title.isEmpty() && !made.match(title).hasMatch() && title != role) return title;
    return base;
}

QString unported_element(const QString& element) {
    static const std::map<QString, QString> words = {
        {"default_font", "読めないフォントの文字"}, {"large_font", "大きすぎるフォント（256 MB 超）の文字"},
        {"text_features", "横書きの文字の字形の指定（OpenType 機能）"}, {"text_warp", "文字のゆがみ"}, {"tones", "トーン"}, {"effects", "効果線"},
        {"prims", "3D"}, {"placed", "配置した画像"}, {"nombre", "ノンブル"}, {"covers", "表紙の折り目"}, {"anim", "アニメーション"},
        {"screen", "トーン化"}, {"finish", "仕上げの白黒化"}, {"brush_library", "自作ブラシの読み込み"},
        {"image_format", "この版では読めない形式の画像（PPM・ICO・JPEG 2000 など）"},
        // PC4 F-0 (render/codecs): a format this build does not write yet
        {"image_write", "この版では書けない形式の画像"},
        // PC4 F-b (formats/open_file): the files 開く… opens in later groups
        {"psd_file", "PSD・PSB を 1 冊として開くこと（ファイル → PSD をレイヤーのまま読み込む… で読み込めます）"},
        {"raw_file", "カメラ RAW のファイル"},
        {"pcraft_file", "PhotoCraft の原稿（.pcraft）"}};
    // (PC4 F-a: PPM and ICO are read since; the formats this build still does not read)
    if (element == QLatin1String("image_format")) return QStringLiteral("この版では読めない形式の画像（JPEG 2000・ICNS・DDS など）");
    if (const auto it = words.find(element); it != words.end()) return it->second;
    if (element.startsWith(QStringLiteral("adjust:"))) return QStringLiteral("色調補正（%1）").arg(element.mid(7));
    if (element.startsWith(QStringLiteral("smart:"))) {  // (smart-objects@1: what PC3's later steps draw)
        static const std::map<QString, QString> later = {{"perspective", "遠近の変形"}, {"warp", "ワープ"}, {"stack", "スタックモード"},
                                                         {"filters", "スマートフィルター"}, {"filter_mask", "フィルターマスク"},
                                                         {"psd", "PSD のスマートオブジェクトの絵"}, {"psb", "PSB の画像"},
                                                         {"too_large", "大きすぎる絵（作業に使える記憶の上限 2 GiB を超えるため描いていません）"}};
        const auto it = later.find(element.mid(6));
        return QStringLiteral("スマートオブジェクトの%1").arg(it != later.end() ? it->second : element.mid(6));
    }
    return element;
}

QString error(const std::string& message) { return error(QString::fromStdString(message)); }

QString reason(const QString& text) { return inner(text); }

QString error(const QString& message) {
    QString text = message.trimmed();
    static const QRegularExpression prefix(QStringLiteral(R"(^ops\[\d+\] [a-z_]+: )"));
    text.replace(prefix, QString());
    static const QRegularExpression usage(QStringLiteral(R"(^(.*?)(?: ‖ unknown keys: (.*?))? ‖ ([a-z_0-9]+) takes (\{.*\})$)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    if (const auto m = usage.match(text); m.hasMatch()) {
        const QString strange = m.captured(2).isEmpty() ? QString() : QStringLiteral("知らない鍵: %1。").arg(m.captured(2));
        return QStringLiteral("%1（%2%3 の書き方: %4）").arg(error(m.captured(1)), strange, m.captured(3), m.captured(4));
    }
    static const QRegularExpression wrong(QRegularExpression::anchoredPattern(QStringLiteral(R"(a value of the wrong type \((.*)\))")),
                                          QRegularExpression::DotMatchesEverythingOption);
    if (const auto m = wrong.match(text); m.hasMatch()) return QStringLiteral("値の型が違います（%1）").arg(m.captured(1));
    static const QRegularExpression missing(QRegularExpression::anchoredPattern(
        QStringLiteral(R"(not found: '?([^']*)'? \(a key the op needs, or an id the book does not have\))")));
    if (const auto m = missing.match(text); m.hasMatch()) {
        return QStringLiteral("%1 が見つかりません（op に要る鍵が無いか、原稿に無い id です）").arg(m.captured(1));
    }
    for (const Rule& r : rules()) {
        const auto m = r.pattern.match(text);
        if (!m.hasMatch()) continue;
        if (r.make) return r.make(m);
        return r.replacement.contains(QLatin1Char('\\')) ? expand(r.replacement, m) : r.replacement;
    }
    static const QRegularExpression japanese(QStringLiteral("[ぁ-んァ-ヶ一-龥]"));
    if (japanese.match(text).hasMatch()) return text;
    return QStringLiteral("この操作はできませんでした（%1）").arg(text);
}

}  // namespace genko::wording
