// The exports people start from the app (Python's genko/app/exporting.py, without Qt).

#include "formats/exporting.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/export.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::formats {

namespace {

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

// The first code point of the UTF-8 text at `at` (and how many bytes it takes).
std::uint32_t code_point(std::string_view s, std::size_t at, std::size_t& n) {
    const auto lead = static_cast<unsigned char>(s[at]);
    std::uint32_t cp = lead;
    n = 1;
    if (lead >= 0xf0) {
        cp = lead & 0x07u;
        n = 4;
    } else if (lead >= 0xe0) {
        cp = lead & 0x0fu;
        n = 3;
    } else if (lead >= 0xc0) {
        cp = lead & 0x1fu;
        n = 2;
    }
    for (std::size_t k = 1; k < n && at + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3fu);
    return cp;
}

// The value of a decimal digit of any script (unicodedata.decimal; Unicode 15.0, as the reference's Python): -1 for
// anything else. Each script's digits run 0 to 9 from these code points.
int decimal(std::uint32_t cp) {
    static constexpr std::array<std::uint32_t, 68> kZeros{
        0x30,    0x660,   0x6F0,   0x7C0,   0x966,   0x9E6,   0xA66,   0xAE6,   0xB66,   0xBE6,   0xC66,   0xCE6,   0xD66,   0xDE6,
        0xE50,   0xED0,   0xF20,   0x1040,  0x1090,  0x17E0,  0x1810,  0x1946,  0x19D0,  0x1A80,  0x1A90,  0x1B50,  0x1BB0,  0x1C40,
        0x1C50,  0xA620,  0xA8D0,  0xA900,  0xA9D0,  0xA9F0,  0xAA50,  0xABF0,  0xFF10,  0x104A0, 0x10D30, 0x11066, 0x110F0, 0x11136,
        0x111D0, 0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0, 0x11950, 0x11C50, 0x11D50, 0x11DA0, 0x11F50, 0x16A60,
        0x16AC0, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0, 0x1E950, 0x1FBF0};
    for (const std::uint32_t zero : kZeros) {
        if (cp >= zero && cp < zero + 10) return static_cast<int>(cp - zero);
    }
    return -1;
}

// int(text) for a page number: Python's whitespace around, a sign, decimal digits of any script with single "_"
// between them; nothing when it is not one.
std::optional<std::int64_t> py_int_text(std::string_view original) {
    const std::string text = core::py_strip(original);
    std::size_t at = 0;
    bool negative = false;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) negative = text[at++] == '-';
    std::int64_t value = 0;
    bool digit_before = false;
    bool any = false;
    while (at < text.size()) {
        if (text[at] == '_') {
            if (!digit_before) return std::nullopt;
            digit_before = false;
            ++at;
            continue;
        }
        std::size_t n = 1;
        const int d = decimal(code_point(text, at, n));
        if (d < 0) return std::nullopt;
        if (value > (std::numeric_limits<std::int64_t>::max() - d) / 10) throw core::Error("format", "int(" + core::py_repr_str(original) + ") is too large for this build");
        value = value * 10 + d;
        digit_before = true;
        any = true;
        at += n;
    }
    if (!any || !digit_before) return std::nullopt;
    return negative ? -value : value;
}

}  // namespace

const std::vector<Format>& formats() {
    static const std::vector<Format> table{
        {"pdf", "PDF（印刷）", "1 冊の PDF。印刷所・校正用。色は自動（モノクロはグレー）・RGB・CMYK・グレー・2 階調から。仕上がりの位置（TrimBox）入り。",
         {"dpi", "area", "color", "icc", "screen"}, true},
        {"tiff", "TIFF（入稿）", "ページごとの 2 値 TIFF。モノクロの入稿用。", {"dpi", "area", "screen"}, true},
        {"png", "PNG", "ページごとの PNG。色は自動（モノクロはグレー）・RGB・グレー・2 階調から。", {"dpi", "area", "color", "screen"}, true},
        {"cmyk", "CMYK（カラー入稿）",
         "ページごとの CMYK の TIFF。印刷所のカラープロファイル（ICC）を選ぶとそれで変換して埋め込む。"
         "選ばなければ、黒い線は K 版だけ・総インキ量は 320% 以内にして変換する。",
         {"dpi", "area", "icc"}, false},
        {"layers", "レイヤーごとの PNG", "ページごとのフォルダーに、レイヤーを 1 枚ずつ透明な PNG で（下のレイヤーから番号順）。", {"dpi", "area"}, false},
        {"psd", "PSD（レイヤー付き）", "ページごとの PSD。CLIP STUDIO PAINT・Photoshop で仕上げを続けるとき。", {"dpi"}, false},
        {"pack", "入稿セット", "TIFF・PNG・ページ一覧（CSV）・説明書きをまとめたフォルダ。", {"dpi"}, false},
        {"webtoon", "縦読み（Webtoon）", "全ページを縦につなげ、決まった高さで切った画像。網点にしない。", {"width", "max_height", "jpeg"}, true},
        {"sns", "SNS 用画像", "1 ページ 1 枚の JPEG。見開きも 1 枚にできる。網点にしない。", {"long_edge", "jpeg", "spreads"}, true},
        {"epub", "EPUB（電子書籍）", "固定レイアウトの EPUB 3。", {"dpi"}, false},
        {"kindle", "Kindle（固定レイアウト）",
         "Kindle 用の固定レイアウトの電子書籍（KDP にそのまま出せる EPUB）。全ページ同じ大きさの JPEG、"
         "右綴じは右から左へ。モノクロの原稿はグレーで。",
         {"long_edge"}, false},
        {"strip", "つなげた 1 枚", "全ページを縦に並べた 1 枚の PNG（確認用）。", {"dpi"}, false},
    };
    return table;
}

const Format* format(std::string_view key) {
    for (const Format& f : formats()) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

std::vector<std::int64_t> parse_pages(std::string_view text, std::int64_t count) {
    std::string all = replace_all(replace_all(replace_all(replace_all(std::string(text), "、", ","), "，", ","), "〜", "-"), "～", "-");
    std::vector<std::int64_t> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = all.find(',', start);
        const std::string part = core::py_strip(std::string_view(all).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        start = comma == std::string::npos ? all.size() + 1 : comma + 1;
        if (!part.empty()) {
            std::int64_t first = 0, last = 0;
            const std::size_t dash = part.find('-');
            if (dash != std::string::npos) {
                const auto a = py_int_text(std::string_view(part).substr(0, dash));
                const auto b = py_int_text(std::string_view(part).substr(dash + 1));
                if (!a || !b) throw core::PyValueError("ページの指定が読めません: " + part);
                first = std::min(*a, *b);
                last = std::max(*a, *b);
            } else {
                const auto n = py_int_text(part);
                if (!n) throw core::PyValueError("ページの指定が読めません: " + part);
                first = last = *n;
            }
            for (std::int64_t n = first; n <= last; ++n) {
                if (!(1 <= n && n <= count)) {
                    throw core::PyValueError(std::to_string(n) + " ページはありません（1〜" + std::to_string(count) + "）");
                }
                if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
                if (n == last) break;  // (no overflow past the last)
            }
        }
        if (comma == std::string::npos) break;
    }
    if (out.empty()) throw core::PyValueError("書き出すページがありません");
    std::sort(out.begin(), out.end());
    return out;
}

core::Document subset(const core::Document& episode, const std::vector<std::int64_t>& pages) {
    core::Document part = episode;
    const auto kept = [&](const core::Num& n) {
        return std::any_of(pages.begin(), pages.end(), [&](std::int64_t p) { return core::Num(p) == n; });
    };
    part.pages.clear();
    for (const auto& page : episode.pages) {
        if (kept(page->index)) part.pages.push_back(page);
    }
    for (std::size_t i = 0; i < part.pages.size(); ++i) {
        const auto& with = part.pages[i]->spread_with;
        if (with && with->truthy() && !kept(*with)) part.edit_page(i).spread_with.reset();
    }
    return part;
}

std::int64_t default_dpi(const core::Document& episode, std::string_view key) {
    if (key == "epub" || key == "strip") return 150;
    return episode.spec.dpi.truthy() ? core::py_int(episode.spec.dpi) : 600;
}

core::Json run(const core::Document& episode_in, const std::optional<std::filesystem::path>& project, std::string_view key,
               const std::filesystem::path& out, const RunOptions& o) {
    using core::Json;
    const auto refuse = [](const std::string& error) { return Json{{"ok", false}, {"error", error}}; };
    const Format* fmt = format(key);
    if (fmt == nullptr) return refuse("知らない形式: " + std::string(key));
    const core::Document* episode = &episode_in;
    std::optional<core::Document> part;
    if (o.pages) {
        std::vector<std::int64_t> sorted = *o.pages;
        std::sort(sorted.begin(), sorted.end());
        bool all = sorted.size() == episode_in.pages.size();
        for (std::size_t i = 0; all && i < sorted.size(); ++i) all = core::Num(sorted[i]) == episode_in.pages[i]->index;
        if (!all) {
            if (o.official) return refuse("正式な書き出しは全ページで行います");
            part = subset(episode_in, *o.pages);
            episode = &*part;
            if (episode->pages.empty()) return refuse("書き出すページがありません");
        }
    }
    if (o.official) {
        if (!fmt->official) return refuse(std::string(fmt->label) + " は正式な書き出しに使えない");
        if (!project) return refuse("正式な書き出しの前に、原稿を保存する");
        // (HumanService.export: preflight and the export approval, the studio's; not in this build yet)
        Json refused = refuse("official export is not in the C++ build yet");
        refused["code"] = "not_yet_ported";
        return refused;
    }
    const std::int64_t dpi = o.dpi && *o.dpi != 0 ? *o.dpi : default_dpi(*episode, key);
    std::vector<std::filesystem::path> files;
    try {
        if (key == "pdf" || key == "tiff" || key == "png" || key == "cmyk") {
            files = export_print(*episode, out, std::string(key), dpi, 180, true, o.area, key == "cmyk" ? std::string("cmyk") : o.color, o.icc, o.screen);
        } else if (key == "layers") {
            files = export_layers(*episode, out, static_cast<int>(dpi), o.area);
        } else if (key == "kindle") {
            files = {export_kindle(*episode, detail::join(out, stem(*episode) + "_kindle.epub"), o.long_edge, std::nullopt, o.dots)};
        } else if (key == "psd") {
            files = export_psd_pages(*episode, out, dpi);
        } else if (key == "pack") {
            files = export_pack(*episode, out, "shueisha", dpi);
        } else if (key == "epub") {
            files = {export_epub(*episode, detail::join(out, stem(*episode) + ".epub"), static_cast<int>(dpi), false, std::nullopt, false, false, o.dots)};
        } else if (key == "strip") {
            files = {export_strip(*episode, detail::join(out, stem(*episode) + "_strip.png"), static_cast<int>(dpi))};
        } else if (key == "webtoon") {
            files = export_webtoon(*episode, out, o.width, o.max_height, 0, o.jpeg ? "jpeg" : "png");
        } else {
            files = export_sns(*episode, out, o.long_edge, o.jpeg ? "jpeg" : "png", 92, o.spreads);
        }
    } catch (const render::NotYetPorted& e) {  // (what this build does not draw yet: nothing written)
        Json error = refuse(e.what());
        error["code"] = e.code();
        error["element"] = e.element();
        return error;
    } catch (const core::PyValueError& e) {
        return refuse(e.what());
    } catch (const core::Error& e) {
        // (Python's OSError, and its ValueError where this build's conversions raise one)
        if (e.code() != "io" && e.code() != "not_found" && e.code() != "value" && e.code() != "format") throw;
        return refuse(e.what());
    }
    Json written = Json::array();
    for (const auto& f : files) written.push_back(core::path_to_utf8(f));
    return Json{{"ok", true}, {"files", written}};
}

}  // namespace genko::formats
