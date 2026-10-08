#include "app/wording.hpp"

#include <QHash>
#include <QRegularExpression>

#include <functional>
#include <vector>

namespace genko::app::wording {

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
        r.push_back(rule("a brush of one's own has a key starting with my_", "自作のブラシの名前（key）は my_ で始めます"));
        r.push_back(rule("wobble must be between 0 and 1", "線の揺れは 0〜1 の間で決めます"));
        r.push_back(rule("spikes must be between 6 and 80", "トゲの数は 6〜80 の間で決めます"));
        r.push_back(rule("spike_depth must be between 0.05 and 0.6", "トゲの長さは 0.05〜0.6 の間で決めます"));
        r.push_back(rule("style_runs is .*", "文字の一部の書式は、言葉と「大きさ・太字・色」の組で指定します"));
        r.push_back(rule("scale must be between 0.3 and 3", "文字の大きさの倍率は 0.3〜3 の間で決めます"));
        r.push_back(rule(R"(unknown style_runs key (\S+).*)", "文字の一部の書式に知らない項目があります（大きさ・太字・色だけ）"));
        r.push_back(rule("gradient_fill needs from and to.*", "グラデーションは、始めと終わりの点で指定します"));
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
        r.push_back(rule("CMYK is written as TIFF or PDF", "CMYK は TIFF か PDF で書き出します"));
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
        r.push_back(rule("the PSD has no pictures to read", "PSD に読み込める絵がありません"));
        r.push_back(rule(R"(the colour profile cannot be read \((.*)\))", "カラープロファイルを読み込めません"));
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
        r.push_back(rule(R"(the material library would be too large for the materials panel \(at most 1 MB\))",
                         "素材ライブラリの一覧が大きくなりすぎるので、書き換えませんでした（1 MB まで）"));
        r.push_back(rule("kind must be one of tone, effect, image, lines, lettering, brush, prim", "素材の種類が違います"));
        r.push_back(rule("adjust is set on a correction layer", "補正の設定は、色調補正のレイヤーにだけできます"));
        r.push_back(rule("adjust kind must be one of .*", "色調補正は、レベル補正・トーンカーブ・色相・反転・階調化・2 値化・グラデーションマップ・白黒から選びます"));
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
        r.push_back(rule(R"(stops are \[\[position.*)", "グラデーションの色は 2〜16 色で、それぞれ位置（0〜100 %）・色・濃さ（0〜100 %）を指定します"));
        r.push_back(rule("repeat is none, repeat or mirror", "繰り返しは、なし・繰り返す・折り返すから選びます"));
        r.push_back(rule("the other book cannot be read.*", "取り込む原稿を開けませんでした（原稿のフォルダーを選んでください）"));
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
        r.push_back(rule("the project did not exist before this change", "これより前には戻せません"));
        r.push_back(rule("snapshot .* is missing .*", "戻すための記録が見つかりません"));
        r.push_back(rule("revision conflict.*", "原稿がほかで変わっています。開き直してからやり直します"));
        r.push_back(rule("add_line with frame_id needs explicit x_mm/y_mm.*", "台詞の位置を指定します"));
        // paper, rulers, 3D, tones, effects, nombre
        r.push_back(rule("the paper must hold the finished size and its bleed", "用紙が、仕上がりと裁ち落としより小さくなっています"));
        r.push_back(rule("the basic frame must fit inside the finished size", "基本枠が仕上がりに収まりません"));
        r.push_back(rule("sizes must be positive", "寸法は 0 より大きくします"));
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
        // (the C++ build's own: what it does not do yet, and saving)
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

QString layer_label(const core::Layer& layer) {
    const QString title = qs(layer.title).trimmed();
    const QString role = qs(core::to_string(layer.role));
    QString base = layer_role(core::to_string(layer.role));
    if (layer.kind == core::LayerKind::Placed) base = QStringLiteral("絵（配置）");
    // titles made of ids (placed art) are not names people gave
    static const QRegularExpression made(
        QRegularExpression::anchoredPattern(QStringLiteral(R"((art|ink|placed)?\s*[0-9a-f]{8,}|placed (art|bg|draft|name)|layer)")));
    if (!title.isEmpty() && !made.match(title).hasMatch() && title != role) return title;
    return base;
}

QString error(const std::string& message) { return error(QString::fromStdString(message)); }

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

}  // namespace genko::app::wording
