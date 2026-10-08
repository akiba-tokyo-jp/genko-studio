#include "app/lettering.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>

#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/tategaki.hpp"

namespace genko::app::lettering {

using core::Json;
using core::Num;

namespace {

constexpr double kLeading = 0.4;   // studio.letter.LEADING
constexpr double kSfxEmMm = 12.0;  // studio.letter.SFX_EM_MM
constexpr double kFitPad = 0.15;   // balloons.FIT_PAD

std::string utf8(const QString& text) { return text.toStdString(); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

// Python's max(a, b) and min(a, b) of numbers: the first unless the second is larger (smaller)
Num num_max(const Num& a, const Num& b) { return b > a ? b : a; }
Num num_min(const Num& a, const Num& b) { return b < a ? b : a; }

bool one_of(std::string_view value, std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), value) != names.end();
}

// numpy.linspace(-span, span, 9)
std::array<double, 9> linspace9(double span) {
    const double start = -span;
    const double stop = span;
    const double delta = stop - start;
    const double step = delta / 8;
    std::array<double, 9> y{};
    for (int k = 0; k < 9; ++k) y[static_cast<std::size_t>(k)] = (step == 0 ? (static_cast<double>(k) / 8) * delta : static_cast<double>(k) * step) + start;
    y[8] = stop;
    return y;
}

// numpy.geomspace(0.25, 4.0, 48): 10 ** linspace(log10(start), log10(stop)), its ends set to start and stop
const std::array<double, 48>& ratios() {
    static const std::array<double, 48> out = [] {
        std::array<double, 48> r{};
        const double start = std::log10(0.25);
        const double stop = std::log10(4.0);
        const double step = (stop - start) / 47;
        for (int i = 0; i < 48; ++i) r[static_cast<std::size_t>(i)] = core::py_pow(10.0, static_cast<double>(i) * step + start);
        r[0] = 0.25;
        r[47] = 4.0;
        return r;
    }();
    return out;
}

// balloons.block_points(rects, pad) for rectangles of floats (x0, y0, x1, y1)
std::vector<std::array<double, 2>> block_points(const std::vector<std::array<double, 4>>& rects, double pad) {
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        if (i == 0 || r[0] < min_x) min_x = r[0];
        if (i == 0 || r[2] > max_x) max_x = r[2];
        if (i == 0 || r[1] < min_y) min_y = r[1];
        if (i == 0 || r[3] > max_y) max_y = r[3];
    }
    const double cx = (min_x + max_x) / 2;
    const double cy = (min_y + max_y) / 2;
    std::vector<std::array<double, 2>> out;
    const auto corner = [&](int xi, int yi, double xs, double ys) {
        for (const auto& r : rects) out.push_back({r[static_cast<std::size_t>(xi)] + xs - cx, r[static_cast<std::size_t>(yi)] + ys - cy});
    };
    corner(0, 1, -pad, -pad);
    corner(2, 1, pad, -pad);
    corner(0, 3, -pad, pad);
    corner(2, 3, pad, pad);
    return out;
}

// balloons.ellipse_around(points, power): the smallest upright oval holding every point, (width, height, dx, dy). The
// numpy arithmetic in its order: |(x - ox) / ratio| ** n + |y - oy| ** n (numpy squares for n = 2, else pow), the
// largest of each middle's, the first of the smallest; b = need ** (1 / n).
std::array<double, 4> ellipse_around(const std::vector<std::array<double, 2>>& pts, double power) {
    const double n = power;
    const auto pw = [n](double v) { return n == 2.0 ? v * v : core::py_pow(v, n); };
    double lo_x = pts.front()[0], hi_x = lo_x, lo_y = pts.front()[1], hi_y = lo_y;
    for (const auto& p : pts) {
        lo_x = std::min(lo_x, p[0]);
        hi_x = std::max(hi_x, p[0]);
        lo_y = std::min(lo_y, p[1]);
        hi_y = std::max(hi_y, p[1]);
    }
    const double span_x = hi_x - lo_x != 0.0 ? hi_x - lo_x : 1.0;
    const double span_y = hi_y - lo_y != 0.0 ? hi_y - lo_y : 1.0;
    bool have = false;
    double best_area = 0, best_ratio = 0, best_b = 0;
    std::array<double, 2> best_off{0, 0};
    std::array<double, 2> centre{0, 0};
    for (const double step : {0.18, 0.05}) {
        const auto lx = linspace9(span_x * step);
        const auto ly = linspace9(span_y * step);
        std::array<std::array<double, 2>, 81> offs{};
        for (int k = 0; k < 81; ++k) offs[static_cast<std::size_t>(k)] = {centre[0] + lx[static_cast<std::size_t>(k % 9)], centre[1] + ly[static_cast<std::size_t>(k / 9)]};
        for (const double ratio : ratios()) {
            double k_need = 0;
            int k_at = 0;
            for (int k = 0; k < 81; ++k) {
                const auto& off = offs[static_cast<std::size_t>(k)];
                double need = -std::numeric_limits<double>::infinity();
                for (const auto& p : pts) need = std::max(need, pw(std::fabs((p[0] - off[0]) / ratio)) + pw(std::fabs(p[1] - off[1])));
                if (k == 0 || need < k_need) {
                    k_need = need;
                    k_at = k;
                }
            }
            const double b = core::py_pow(k_need, 1 / n);
            const double area = ratio * b * b;
            if (!have || area < best_area) {
                best_area = area;
                best_ratio = ratio;
                best_b = b;
                best_off = offs[static_cast<std::size_t>(k_at)];
                have = true;
            }
        }
        centre = best_off;
    }
    return {2 * best_ratio * best_b, 2 * best_b, best_off[0], best_off[1]};
}

QRegularExpression pattern(const QString& text) { return QRegularExpression(text, QRegularExpression::UseUnicodePropertiesOption); }

// re.sub(pattern, take, text): each match (left to right, none overlapping) replaced by what take gives
QString substitute(const QRegularExpression& re, const QString& text, const std::function<QString(const QRegularExpressionMatch&)>& take) {
    QString out;
    qsizetype pos = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(pos, m.capturedStart() - pos);
        out += take(m);
        pos = m.capturedEnd();
    }
    out += text.mid(pos);
    return out;
}

const QRegularExpression& ruby_pattern() {
    // _RUBY: ｜base《ruby》, or kanji right before 《ruby》 (no ｜ needed)
    static const QRegularExpression re = pattern(QStringLiteral("[｜|]([^｜|《》\\n]+)《([^《》\\n]+)》|([㐀-鿿豈-﫿々〆ヶ]+)《([^《》\\n]+)》"));
    return re;
}

const QRegularExpression& emphasis_pattern() {
    static const QRegularExpression re = pattern(QStringLiteral("《《([^《》\\n]+)》》"));
    return re;
}

const QRegularExpression& styled_pattern() {
    static const QRegularExpression re = pattern(QStringLiteral("[{｛]([^{}｛｝|｜\\n]+)[|｜]([^{}｛｝\\n]+)[}｝]"));
    return re;
}

// tag_style(tag): a named mark, a colour (#rrggbb) or a size (×1.5: this many times the line's); none: not a mark
std::optional<Json> tag_style(const QString& tag) {
    for (const auto& [name, style] : style_tags()) {
        if (name == tag) return style;
    }
    static const QRegularExpression colour(QRegularExpression::anchoredPattern(QStringLiteral("[#＃]([0-9a-fA-F]{6})")));
    if (const auto m = colour.match(tag); m.hasMatch()) {
        const QString h = m.captured(1);
        return Json{{"rgb", Json::array({h.mid(0, 2).toInt(nullptr, 16), h.mid(2, 2).toInt(nullptr, 16), h.mid(4, 2).toInt(nullptr, 16)})}};
    }
    static const QRegularExpression size(QRegularExpression::anchoredPattern(QStringLiteral("[x×Ｘｘ]([0-9.]+)")));
    if (const auto m = size.match(tag); m.hasMatch()) {
        const std::string digits = m.captured(1).toStdString();
        double value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        // (float(): "1." and ".5" are numbers, "." and "1.2.3" are not)
        if (error != std::errc() || end != digits.data() + digits.size()) return std::nullopt;
        if (0.3 <= value && value <= 3) return Json{{"scale", value}};
        return std::nullopt;
    }
    return std::nullopt;
}

// parse_styles(typed): the text with the styled parts' marks taken off, and [[words, style], …]
std::pair<QString, std::vector<std::pair<QString, Json>>> parse_styles(const QString& typed) {
    std::vector<std::pair<QString, Json>> styles;
    static const QRegularExpression split(pattern(QStringLiteral("[、,，・ ]+")));
    const QString text = substitute(styled_pattern(), typed, [&](const QRegularExpressionMatch& m) {
        QStringList tags;
        for (const QString& t : m.captured(1).split(split)) {
            if (!strip(t).isEmpty()) tags << strip(t);
        }
        std::vector<Json> found;
        for (const QString& t : tags) {
            const auto style = tag_style(t);
            if (!style) return m.captured(0);
            found.push_back(*style);
        }
        if (tags.isEmpty()) return m.captured(0);
        Json style = Json::object();
        for (const Json& part : found) {
            for (const auto& [key, value] : part.items()) style[key] = value;
        }
        styles.emplace_back(m.captured(2), style);
        return m.captured(2);
    });
    return {text, styles};
}

// f"{v:02x}" (the width counts the sign)
QString py_hex2(std::int64_t v) {
    if (v < 0) return QStringLiteral("-") + QString::number(-v, 16);
    const QString digits = QString::number(v, 16);
    return digits.size() < 2 ? QStringLiteral("0") + digits : digits;
}

// f"{x:g}"
QString general(double x) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), x, std::chars_format::general, 6);
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

}  // namespace

const std::vector<std::pair<QString, QString>>& kinds() {
    static const std::vector<std::pair<QString, QString>> list = {
        {"speech", "普通（楕円）"},
        {"rounded", "角丸"},
        {"box", "四角"},
        {"cloud", "雲（もくもく）"},
        {"thought", "心の声（泡つき）"},
        {"shout", "叫び（トゲ）"},
        {"electric", "電子音（電話・テレビ）"},
        {"flash", "フラッシュ（放射線）"},
        {"whisper", "ささやき（点線）"},
        {"narration", "ナレーション（四角・しっぽなし）"},
        {"sfx", "効果音（描き文字）"},
        {"none", "文字だけ"},
        {"dotted_box", "点線の囲み（見出し・メモ）"},
        {"tone_box", "トーンの囲み（見出し・解説）"},
        {"fancy_box", "飾り枠（テーマ・題目）"},
    };
    return list;
}

QString kind_label(const std::string& key) {
    const QString k = qs(key);
    for (const auto& [name, label] : kinds()) {
        if (name == k) return label;
    }
    return k;
}

const std::vector<std::pair<QString, QString>>& bundled_fonts() {
    static const std::vector<std::pair<QString, QString>> list = {
        {"antique", "アンチック（かなは明朝・漢字はゴシック）"},
        {"gothic", "ゴシック"},
        {"mincho", "明朝"},
        {"maru", "丸ゴシック"},
        {"hand", "手書き風"},
        {"sfx", "極太（効果音）"},
        {"sfx_pop", "勢い（効果音）"},
    };
    return list;
}

const std::vector<std::pair<QString, Json>>& style_tags() {
    static const std::vector<std::pair<QString, Json>> list = {
        {"大", Json{{"scale", 1.4}}},
        {"特大", Json{{"scale", 1.8}}},
        {"小", Json{{"scale", 0.7}}},
        {"太", Json{{"bold", true}}},
        {"極太", Json{{"bold", 2}}},
        {"赤", Json{{"rgb", Json::array({210, 30, 30})}}},
        {"青", Json{{"rgb", Json::array({30, 80, 200})}}},
        {"白", Json{{"rgb", Json::array({255, 255, 255})}}},
        {"縦中横", Json{{"tcy", true}}},
    };
    return list;
}

QString strip(const QString& text) { return qs(core::py_strip(utf8(text))); }

std::vector<std::u32string> columns(std::u32string_view text, std::int64_t per_column) {
    std::vector<std::u32string> out;
    const std::u32string set = render::text::without_periods(text.empty() ? std::u32string_view(U" ") : text);
    std::size_t start = 0;
    while (true) {
        const std::size_t end = set.find(U'\n', start);
        const std::u32string part = set.substr(start, end == std::u32string::npos ? std::u32string::npos : end - start);
        if (static_cast<std::int64_t>(part.size()) > per_column) {
            for (std::u32string& col : render::text::phrase_columns(part, per_column)) out.push_back(std::move(col));
        } else {
            out.push_back(part.empty() ? std::u32string(U" ") : part);
        }
        if (end == std::u32string::npos) break;
        start = end + 1;
    }
    return out;
}

std::pair<double, double> measure(const std::vector<std::u32string>& breaks, std::string_view balloon, double em) {
    std::vector<std::u32string> cols;
    for (const auto& b : breaks) {
        if (!b.empty()) cols.push_back(b);
    }
    if (cols.empty()) cols.push_back(U" ");
    std::size_t longest = 0;
    for (const auto& c : cols) longest = std::max(longest, c.size());
    const auto count = static_cast<double>(cols.size());
    if (balloon == "sfx") return {kSfxEmMm * count, kSfxEmMm * static_cast<double>(longest)};
    // columns are LEADING em apart (the renderer's default), characters sit edge to edge
    const double text_w = em * count + em * kLeading * (count - 1);
    const double text_h = em * static_cast<double>(longest);
    if (one_of(balloon, {"speech", "thought", "shout", "whisper"})) {
        // the ellipse around the letters' own shape (columns from the top, uneven at their feet), a third of a letter to
        // spare; a spiked edge's valleys cut in, so the outline is that much bigger
        std::vector<std::array<double, 4>> rects;
        for (std::size_t i = 0; i < cols.size(); ++i) {  // (the first column on the right)
            const double x1 = text_w - static_cast<double>(i) * em * (1 + kLeading);
            rects.push_back({x1 - em, 0.0, x1, em * static_cast<double>(std::max<std::size_t>(1, cols[i].size()))});
        }
        const Json st = balloon == "shout" ? Json{{"spike_depth", 0.26}} : Json::object();  // (studio.letter.SPIKED)
        const auto box = ellipse_around(block_points(rects, em * kFitPad), render::text::hug_power(balloon, st));
        const double keep = render::text::hug_keep(balloon, st);
        return {box[0] / keep, box[1] / keep};
    }
    double pad = one_of(balloon, {"none", "aside"}) ? 0.0 : em / 4;
    if (one_of(balloon, {"dotted_box", "tone_box", "fancy_box"})) pad *= 3;  // (as balloons._inner: the frame keeps the words further in)
    if (balloon == "electric") return {(text_w + 2 * pad) / 0.76, (text_h + 2 * pad) / 0.76};  // (its zigzag leaves about three quarters)
    return {text_w + 2 * pad, text_h + 2 * pad};
}

std::pair<double, double> box_size(const std::string& text, std::string_view balloon, bool vertical, double max_h_mm, double max_w_mm) {
    const double em = kEmMm;
    const std::u32string words = render::text::u32(text);
    double w = 0, h = 0;
    if (vertical) {
        const std::int64_t fits = core::py_trunc_int((max_h_mm * 0.75) / (em * (one_of(balloon, {"speech", "thought", "shout", "whisper"}) ? 1.5 : 1.1)));
        const std::int64_t per = std::max<std::int64_t>(2, std::min<std::int64_t>(fits, balloon == "sfx" ? 4 : 7));  // (a manga column: about 7)
        std::tie(w, h) = measure(columns(words, per), balloon);
    } else {
        const std::int64_t per = std::max<std::int64_t>(4, core::py_trunc_int((max_w_mm * 0.8) / em));
        std::tie(h, w) = measure(columns(words, per), balloon);  // (the same box turned: rows across, characters along)
    }
    return {core::py_round(core::py_min(w, max_w_mm), 2), core::py_round(core::py_min(h, max_h_mm), 2)};
}

Json place_new(const core::Document& doc, const core::Page& page, const core::Frame& frame, const std::string& text, const std::string& balloon,
               bool vertical) {
    const core::Rect& r = frame.rect;
    const auto [w, h] = box_size(text, balloon, vertical, r.height.value() - 2 * kMarginMm, r.width.value() - 2 * kMarginMm);
    double right = (r.x + r.width).value() - kMarginMm;
    for (const core::StoryLine* line : doc.story_for_page(page.index)) {
        if (line->frame_id && *line->frame_id == frame.id) right = core::py_min(right, line->x_mm.value() - 1.5);
    }
    const double x = core::py_max(r.x.value() + kMarginMm, right - w);
    return Json{{"x_mm", core::py_round(x, 2)}, {"y_mm", core::py_round(r.y.value() + kMarginMm, 2)}, {"w_mm", w}, {"h_mm", h},
                {"wrap", vertical ? "vertical" : "horizontal"}};
}

Json refit(const core::StoryLine& line, const core::Frame* frame, const std::string& text, const std::string& balloon, bool vertical) {
    const double max_h = frame != nullptr ? frame->rect.height.value() - 2 * kMarginMm : 120.0;
    const double max_w = frame != nullptr ? frame->rect.width.value() - 2 * kMarginMm : 120.0;
    const auto [w, h] = box_size(text, balloon, vertical, max_h, max_w);
    const Num right = line.x_mm + line.w_mm;
    return Json{{"x_mm", core::py_round((right - Num(w)).value(), 2)}, {"y_mm", line.y_mm.json()}, {"w_mm", w}, {"h_mm", h}};
}

Json place_at(double x_mm, double y_mm, const std::string& text, const std::string& balloon, bool vertical, const core::Frame* frame) {
    const double max_h = frame != nullptr ? frame->rect.height.value() - 2 * kMarginMm : 120.0;
    const double max_w = frame != nullptr ? frame->rect.width.value() - 2 * kMarginMm : 160.0;
    const auto [w, h] = box_size(text, balloon, vertical, max_h, max_w);
    Num x(x_mm - w / 2);
    Num y(y_mm - h / 2);
    if (frame != nullptr) {
        const core::Rect& r = frame->rect;
        x = num_max(r.x + Num(1), num_min(r.x + r.width - Num(w) - Num(1), x));
        y = num_max(r.y + Num(1), num_min(r.y + r.height - Num(h) - Num(1), y));
    }
    return Json{{"x_mm", core::py_round(x, 2).json()}, {"y_mm", core::py_round(y, 2).json()}, {"w_mm", w}, {"h_mm", h},
                {"wrap", vertical ? "vertical" : "horizontal"}};
}

std::pair<QString, Json> parse_ruby(const QString& text) {
    Json runs = Json::array();
    const QString out = substitute(ruby_pattern(), text, [&](const QRegularExpressionMatch& m) {
        const QString base = !m.captured(1).isEmpty() ? m.captured(1) : m.captured(3);
        const QString ruby = !m.captured(1).isEmpty() ? m.captured(2) : m.captured(4);
        runs.push_back(Json::array({utf8(base), utf8(ruby)}));
        return base;
    });
    return {out, runs};
}

Marks parse_marks(const QString& typed) {
    Marks marks;
    const auto [styled, styles] = parse_styles(typed);
    const QString dotted = substitute(emphasis_pattern(), styled, [&](const QRegularExpressionMatch& m) {
        marks.emphasis_runs.push_back(utf8(m.captured(1)));
        return m.captured(1);
    });
    auto [text, runs] = parse_ruby(dotted);
    marks.text = utf8(text);
    marks.ruby_runs = std::move(runs);
    // (the styled words, found again in the text without its marks)
    for (const auto& [words, style] : styles) {
        const QString plain = substitute(emphasis_pattern(), words, [](const QRegularExpressionMatch& m) { return m.captured(1); });
        marks.style_runs.push_back(Json::array({utf8(parse_ruby(plain).first), style}));
    }
    return marks;
}

QString with_ruby(const QString& text, const std::vector<Json>& runs) {
    QString out;
    qsizetype pos = 0;
    for (const Json& run : runs) {
        if (!run.is_array() || run.size() < 2) continue;
        const QString base = qs(core::py_str(run[0]));
        const QString ruby = qs(core::py_str(run[1]));
        const qsizetype at = text.indexOf(base, pos);
        if (at < 0) continue;
        out += text.mid(pos, at - pos);
        out += QStringLiteral("｜%1《%2》").arg(base, ruby);
        pos = at + base.size();
    }
    out += text.mid(pos);
    return out;
}

QString with_marks(const core::StoryLine& line) {
    QString text = with_ruby(qs(line.text), line.ruby_runs);
    qsizetype pos = 0;
    for (const std::string& b : line.emphasis_runs) {
        const QString base = qs(b);
        const qsizetype at = text.indexOf(base, pos);
        if (at < 0) continue;
        text = text.left(at) + QStringLiteral("《《%1》》").arg(base) + text.mid(at + base.size());
        pos = at + base.size() + 4;
    }
    pos = 0;
    for (const auto& run : line.style_runs) {
        const QString words = qs(run.first);
        const Json& style = run.second;
        QStringList tags;
        std::vector<std::string> covered;
        const auto get = [&style](const std::string& key) { return style.is_object() && style.contains(key) ? style[key] : Json(); };
        for (const auto& [tag, value] : style_tags()) {
            bool all = true;
            bool overlap = false;
            for (const auto& [key, want] : value.items()) {
                all = all && core::py_equals(get(key), want);
                overlap = overlap || std::find(covered.begin(), covered.end(), key) != covered.end();
            }
            if (all && !overlap) {
                tags << tag;
                for (const auto& [key, want] : value.items()) covered.push_back(key);
            }
        }
        const auto is_covered = [&covered](const char* key) { return std::find(covered.begin(), covered.end(), key) != covered.end(); };
        if (core::py_truthy(get("rgb")) && !is_covered("rgb")) {
            QString hex = QStringLiteral("#");
            const Json rgb = get("rgb");
            for (std::size_t i = 0; i < rgb.size() && i < 3; ++i) hex += py_hex2(core::py_int(rgb[i]));
            tags << hex;
        }
        if (core::py_truthy(get("scale")) && !is_covered("scale")) tags << QStringLiteral("×") + general(core::to_float(get("scale")));
        const qsizetype at = text.indexOf(words, pos);
        if (at < 0 || tags.isEmpty()) continue;
        const QString mark = tags.join(QStringLiteral("、"));
        text = text.left(at) + QStringLiteral("{%1|%2}").arg(mark, words) + text.mid(at + words.size());
        pos = at + words.size() + mark.size() + 3;
    }
    return text;
}

bool same_marks(const Marks& marks, const core::StoryLine& line) {
    if (marks.text != line.text) return false;
    Json ruby = Json::array();
    for (const Json& run : line.ruby_runs) ruby.push_back(run);
    Json dots = Json::array();
    for (const std::string& words : line.emphasis_runs) dots.push_back(words);
    Json styles = Json::array();
    for (const auto& [words, style] : line.style_runs) styles.push_back(Json::array({words, style}));
    return core::py_equals(marks.ruby_runs, ruby) && core::py_equals(marks.emphasis_runs, dots) && core::py_equals(marks.style_runs, styles);
}

}  // namespace genko::app::lettering
