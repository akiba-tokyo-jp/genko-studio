// The exports people start from the app (Python's genko/app/exporting.py, without Qt).

#include "formats/exporting.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "formats/export.hpp"
#include "formats/pdf.hpp"
#include "formats/psd_adjust.hpp"
#include "core/adjust.hpp"
#include "core/exposure.hpp"
#include "core/lut3d.hpp"
#include "render/color_canvas.hpp"
#include "formats/psd_text.hpp"
#include "render/not_yet_ported.hpp"
#include "render/psd_annotations.hpp"
#include "render/psd_shapes.hpp"

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

// A page number as Python's int() reads it: its value, held at the largest (or smallest) an int64 holds when it is
// beyond them (no book has such a page), and its digits as Python prints the number.
struct PageNumber {
    std::int64_t value = 0;
    std::string text;
};

// int(text) for a page number: Python's whitespace around, a sign, decimal digits of any script with single "_"
// between them; nothing when it is not one.
std::optional<PageNumber> py_int_text(std::string_view original) {
    const std::string text = core::py_strip(original);
    std::size_t at = 0;
    bool negative = false;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) negative = text[at++] == '-';
    std::int64_t value = 0;
    std::string digits;
    bool beyond = false;
    bool digit_before = false;
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
        beyond = beyond || value > (std::numeric_limits<std::int64_t>::max() - d) / 10;
        if (!beyond) value = value * 10 + d;
        if (!digits.empty() || d != 0) digits += static_cast<char>('0' + d);
        digit_before = true;
        at += n;
    }
    if (!digit_before) return std::nullopt;
    if (digits.empty()) digits = "0";
    if (beyond) value = negative ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
    else if (negative) value = -value;
    return PageNumber{value, (negative && digits != "0" ? "-" : "") + digits};
}

// a < b as numbers (two held at the same end: by their digits)
bool smaller(const PageNumber& a, const PageNumber& b) {
    if (a.value != b.value) return a.value < b.value;
    if (a.text == b.text) return false;
    if (a.text.size() != b.text.size()) return (a.text.size() < b.text.size()) != (a.value < 0);
    return (a.text < b.text) != (a.value < 0);
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
        {"psd", "PSD（レイヤー付き）", "ページごとの PSD。CLIP STUDIO PAINT・Photoshop で仕上げを続けるとき。", {"dpi", "psd_text"}, false},
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
            PageNumber first, last;
            const std::size_t dash = part.find('-');
            if (dash != std::string::npos) {
                const auto a = py_int_text(std::string_view(part).substr(0, dash));
                const auto b = py_int_text(std::string_view(part).substr(dash + 1));
                if (!a || !b) throw core::PyValueError("ページの指定が読めません: " + part);
                first = smaller(*b, *a) ? *b : *a;
                last = smaller(*b, *a) ? *a : *b;
            } else {
                const auto n = py_int_text(part);
                if (!n) throw core::PyValueError("ページの指定が読めません: " + part);
                first = last = *n;
            }
            // (a number past 64 bits is past the book: said with its digits, as Python says it)
            if (!(1 <= first.value && first.value <= count)) throw core::PyValueError(first.text + " ページはありません（1〜" + std::to_string(count) + "）");
            for (std::int64_t n = first.value; n <= last.value; ++n) {
                if (!(1 <= n && n <= count)) {
                    throw core::PyValueError(std::to_string(n) + " ページはありません（1〜" + std::to_string(count) + "）");
                }
                if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
                if (n == last.value) break;  // (no overflow past the last)
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

namespace {

// color-model@1: can a 32-bit page hold values past 0..1 (HDR) — its precise rasters' colours, a grey page's lightness,
// or an exposure correction that may make them?
bool may_hold_hdr(const core::Page& page) {
    const auto outside = [](double v) { return v < 0.0 || v > 1.0; };
    for (const core::Layer& layer : page.layers) {
        if (layer.kind == core::LayerKind::Adjust && layer.visible && layer.adjust && layer.adjust->is_object() &&
            layer.adjust->value("kind", core::Json()) == "exposure") {
            return true;
        }
        try {
            if (layer.color_raster) {
                const core::ColorRasterView view(*layer.color_raster);
                for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
                    const auto p = view.pixel(i);
                    if (outside(p[0]) || outside(p[1]) || outside(p[2])) return true;
                }
            } else if (layer.ink_raster && page.color && page.color->space == core::InkSpace::Gray) {
                const core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
                for (std::size_t i = 0; pixels.is_float() && i < pixels.pixel_count(); ++i) {
                    if (outside(pixels.value(i, 0))) return true;
                }
            }
        } catch (const core::Error&) {  // (pixels this build cannot read: none of theirs written)
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> export_notes(const core::Document& episode, std::string_view key, std::string_view color) {
    std::int64_t converted = 0, halved = 0, cut = 0;
    std::int64_t duotone_grey = 0, duotone_look = 0, multi = 0, flat = 0;  // (color-model@1, PC1d: the modes of their own)
    bool flat_channels = false;
    bool inks_only = true;  // (every page converted a CMYK or Lab one)
    for (const auto& page : episode.pages) {
        if (!page->color && !page->rgb) continue;
        const bool inks = page->color && (key == "psd" || detail::as_inks(*page, key, color));
        const bool values = !page->color && (key == "psd" || detail::as_values(*page, key, color));
        // (a duotone page's inks and a multichannel page's plates: their own notes, never written as such but by a PSD's
        // plates; a bitmap or indexed page in its own mode where the format has one)
        const bool is_duotone = page->color && !page->color->duotone.empty();
        const bool is_multi = page->color && page->color->space == core::InkSpace::Multi;
        const bool is_flat = (page->color && page->color->bitmap) || (!page->color && page->rgb->table);
        // (a duotone page written as its grey where the format holds grey inks — PSD, a TIFF in auto or grey, the pack's
        // TIFFs —, else as it prints)
        if (is_duotone) ++(inks || (key == "pack" && detail::as_inks(*page, "tiff", "auto")) ? duotone_grey : duotone_look);
        if (is_multi) ++multi;
        if (key == "psd" && is_flat) {
            ++flat;
            flat_channels = flat_channels || !page->channels.empty();
        }
        if (is_duotone || is_multi || !detail::native_mode(*page, key, color).empty() || (key == "psd" && is_flat)) {
            const bool deep = page->color && page->color->depth == 32;
            if (deep && is_multi && key == "psd") ++halved;  // (Photoshop's Multichannel holds 8 or 16 bits)
            continue;
        }
        if (!inks && !values) {
            ++converted;
            if (!page->color || page->color->space == core::InkSpace::Gray) inks_only = false;
        }
        // (32 bits at 16: a PDF's; a PSD's or a TIFF's of CMYK or Lab inks — the pack's TIFFs too)
        const bool deep = (page->color ? page->color->depth : page->rgb->depth) == 32;
        const bool cmyk_or_lab = page->color && page->color->space != core::InkSpace::Gray;
        if (deep && ((key == "pdf" && (inks || values)) || (cmyk_or_lab && (inks || (key == "pack" && detail::as_inks(*page, "tiff", "auto")))))) {
            ++halved;
            // (a PDF holds values in 0..1: an RGB or grey page's HDR is cut)
            if (key == "pdf" && !cmyk_or_lab && may_hold_hdr(*page)) ++cut;
        }
    }
    std::vector<std::string> notes;
    const std::string n = std::to_string(converted);
    if (converted > 0 && key == "pack") {  // (its TIFFs keep the inks and values — auto —, its PNGs cannot)
        notes.push_back(inks_only ? "CMYK・Lab のページ " + n + " ページの PNG は、画面の色（sRGB）から変換して書き出しました（TIFF はインキの値のまま）"
                                  : "色モード・プロファイルを持つページ " + n + " ページの PNG は、画面の色（sRGB）から変換して書き出しました（TIFF はページの値のまま）");
    } else if (converted > 0) {
        notes.push_back(inks_only ? "CMYK・Lab のページ " + n + " ページは、この形式・色ではインキの値のまま書けないため、画面の色（sRGB）から変換して書き出しました"
                                  : "色モード・プロファイルを持つページ " + n + " ページは、この形式・色ではその値のまま書けないため、画面の色（sRGB）から変換して書き出しました");
    }
    if (duotone_grey > 0) {
        notes.push_back("ダブルトーンのページ " + std::to_string(duotone_grey) +
                        " ページは、この形式ではダブルトーンのインキのまま書けないため、グレーの値で書き出しました（インキの色と曲線は書いていません）");
    }
    if (duotone_look > 0) {
        notes.push_back("ダブルトーンのページ " + std::to_string(duotone_look) +
                        " ページは、この形式・色ではダブルトーンのインキのまま書けないため、刷った見え方（画面の色、sRGB）で書き出しました");
    }
    if (multi > 0) {
        notes.push_back("マルチチャンネルのページ " + std::to_string(multi) +
                        (key == "psd" ? " ページは、PSD ではレイヤーを持てないため、版を統合した画像だけを書き出しました"
                                      : " ページは、この形式では版のまま書けないため、刷った見え方（画面の色、sRGB）で書き出しました（PSD なら版のまま書けます）"));
    }
    if (flat > 0) {
        notes.push_back("2 階調・インデックスカラーのページ " + std::to_string(flat) + " ページは、PSD ではレイヤーを持てないため、統合した画像だけを書き出しました" +
                        (flat_channels ? std::string("（この色モードではアルファ・特色チャンネルも書けません）") : std::string()));
    }
    if (halved > 0) notes.push_back("32 bit のページ " + std::to_string(halved) + " ページは、この形式では 16 bit にして書き出しました");
    if (cut > 0) {
        notes.push_back("HDR の値（0〜1 の外）を持つ 32 bit のページ " + std::to_string(cut) +
                        " ページは、PDF では 0〜1 に切って書き出しました（HDR のまま残すには TIFF か PSD で書き出します）");
    }
    // channels@1: PSD keeps a page's alpha and spot channels as channels of their own, PDF its spot channels as
    // separations; elsewhere the spots are printed into the picture as the screen shows them (or, where the page is
    // written as its inks or values, left out), and the alpha channels are left out
    if (key != "psd") {
        std::int64_t printed = 0, left = 0, alphas = 0;
        for (const auto& page : episode.pages) {
            const bool spot = std::any_of(page->channels.begin(), page->channels.end(),
                                          [](const core::Channel& c) { return c.kind == core::ChannelKind::Spot && c.raster; });
            const bool alpha = std::any_of(page->channels.begin(), page->channels.end(), [](const core::Channel& c) { return c.kind == core::ChannelKind::Alpha; });
            if (alpha) ++alphas;
            if (!spot || key == "pdf") continue;
            const bool kept = detail::as_inks(*page, key == "pack" ? "tiff" : key, key == "pack" ? "auto" : color) ||
                              detail::as_values(*page, key == "pack" ? "tiff" : key, key == "pack" ? "auto" : color);
            if (kept) ++left;
            else ++printed;
        }
        if (printed > 0) {
            notes.push_back("特色チャンネルのあるページ " + std::to_string(printed) +
                            " ページは、この形式では特色を版として書けないため、画面の見え方で刷り込んで書き出しました（PSD・PDF なら版として書けます）");
        }
        if (left > 0) {
            notes.push_back("特色チャンネルのあるページ " + std::to_string(left) +
                            " ページは、インキ・値のまま書いたため、特色を書き出していません（PSD・PDF なら版として書けます）");
        }
        if (alphas > 0) {
            notes.push_back("アルファチャンネルのあるページ " + std::to_string(alphas) +
                            " ページは、この形式ではアルファチャンネルを書けないため、書き出していません（PSD なら書けます）");
        }
    }
    if (key == "pdf") {  // (a spot named as one of PDF's own colorants: written under another name)
        std::vector<std::pair<std::string, std::string>> renamed;
        for (const auto& page : episode.pages) {
            std::vector<std::string> names;
            for (const core::Channel& c : page->channels) {
                if (c.kind == core::ChannelKind::Spot && c.raster) names.push_back(c.name);
            }
            const std::vector<std::string> written = pdf_spot_names(names);
            for (std::size_t k = 0; k < names.size(); ++k) {
                const std::pair<std::string, std::string> change{names[k], written[k]};
                if (written[k] != names[k] && std::find(renamed.begin(), renamed.end(), change) == renamed.end()) renamed.push_back(change);
            }
        }
        if (!renamed.empty()) {
            std::string from, to;
            for (const auto& [name, made] : renamed) {
                from += (from.empty() ? "" : "・") + name;
                to += (to.empty() ? "" : "・") + made;
            }
            notes.push_back("特色チャンネルの名前 " + from + " は PDF では決まった版（プロセスカラー・All・None）の名前のため、" + to + " として書き出しました");
        }
    }
    if (key == "psd") {  // (PC2, the user's decision 5: what the PSD's adjustment layers hold of the correction layers)
        for (std::string& note : psd_adjust_notes(episode)) notes.push_back(std::move(note));
        for (std::string& note : psd_smart_notes(episode)) notes.push_back(std::move(note));  // (PC3 A3: the placed layers)
        for (std::string& note : render::psd_shapes::notes(episode)) notes.push_back(std::move(note));  // (PC3 C-b: the shape layers)
        for (std::string& note : render::psd_annotations::export_notes(episode)) notes.push_back(std::move(note));  // (PC5 J-d1: the count)
    }
    return notes;
}

std::optional<std::string> ink_note(const core::Document& episode, std::string_view key, std::string_view color) {
    std::vector<std::string> notes = export_notes(episode, key, color);
    if (notes.empty()) return std::nullopt;
    return std::move(notes.front());
}

std::optional<std::string> ink_note(const core::Page& page) {
    if (page.color && page.color->space != core::InkSpace::Gray) {
        return "CMYK・Lab のページは、動画にはインキの値のまま書けないため、画面の色（sRGB）から変換して書き出しました";
    }
    if (!page.color && !page.rgb) return std::nullopt;
    return "色モード・プロファイルを持つページは、動画にはその値のまま書けないため、画面の色（sRGB）から変換して書き出しました";
}

std::string bake_cube(const core::Document& doc, const core::Page& page, int size_in, const std::string& title, std::vector<std::string>* skipped) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::clamp(size_in, 2, 256));
    const double step = 1.0 / (n - 1);
    const render::RgbSpace space = render::RgbSpace::of(page);
    // the correction layers, each drawn as the page draws it (made once: one this build does not draw is left out)
    struct Step {
        const core::Layer* layer;
        std::optional<core::Exposure> exposure;
        core::AdjustPtr function;
        std::string blend;
    };
    std::vector<Step> steps;
    const auto skip = [skipped](const core::Layer& layer) {
        if (skipped != nullptr && std::find(skipped->begin(), skipped->end(), layer.id) == skipped->end()) skipped->push_back(layer.id);
    };
    for (const core::Layer& layer : page.layers) {
        if (layer.kind != core::LayerKind::Adjust || !layer.visible || !layer.adjust || !layer.adjust->is_object()) continue;
        Step one{&layer, std::nullopt, nullptr, core::native_of(*layer.adjust).blend};
        try {
            if (layer.adjust->value("kind", core::Json()) == core::Json("exposure")) {
                one.exposure = core::Exposure::parse(*layer.adjust);
            } else {
                one.function = render::correction_of(layer, page, &doc);
                if (!one.function) continue;
            }
        } catch (const render::NotYetPorted&) {
            skip(layer);
            continue;
        } catch (const core::Error& error) {
            if (error.code() != "not_yet_ported" && error.code() != "value") throw;
            skip(layer);
            continue;
        }
        steps.push_back(std::move(one));
    }
    std::string text = "TITLE \"" + replace_all(title, "\"", "'") + "\"\n# Created by Genko\nLUT_3D_SIZE " + std::to_string(n) +
                       "\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
    text.reserve(text.size() + std::size_t(n) * n * n * 27);
    // The lattice (x = b·n + r, y = g) in slabs of blue planes, each its own colour canvas (one holds 512 MiB at most:
    // 256³ is past it); each slab's place on the lattice given to the corrections (a dither's).
    const std::uint32_t planes = std::max<std::uint32_t>(1, std::min<std::uint32_t>(n, (std::uint32_t{1} << 22) / (n * n)));
    for (std::uint32_t b0 = 0; b0 < n; b0 += planes) {
        const std::uint32_t count = std::min(planes, n - b0);
        const std::uint32_t w = count * n;
        const std::string lattice = core::encode_color_pixels(w, n, "f32", [n, w, b0, step](std::size_t i) {
            const std::size_t x = i % w, y = i / w;
            return std::array<double, 4>{static_cast<double>(x % n) * step, static_cast<double>(y) * step, static_cast<double>(b0 + x / n) * step, 1.0};
        });
        const render::Size size{static_cast<int>(w), static_cast<int>(n)};
        render::ColorCanvas canvas(render::Image::create("RGBA", size, render::Ink{0, 0, 0, 255}), space);
        canvas.blend(core::ColorRasterView(lattice), size, render::Box{0, 0, size.width, size.height}, 1.0, false);
        for (const Step& one : steps) {
            if (skipped != nullptr && std::find(skipped->begin(), skipped->end(), one.layer->id) != skipped->end()) continue;
            try {
                if (one.exposure) {
                    canvas.expose(*one.exposure, one.layer->opacity, false, nullptr, one.blend);
                } else {
                    canvas.adjust(*one.function, one.layer->opacity, false, nullptr, one.blend, render::Point{static_cast<int>(b0 * n), 0});
                }
            } catch (const core::Error& error) {
                if (error.code() != "not_yet_ported" && error.code() != "value") throw;
                skip(*one.layer);
            }
        }
        const std::string baked = canvas.color_raster("f32");  // (held: the view reads these bytes)
        const core::ColorRasterView out(baked);
        for (std::uint32_t b = 0; b < count; ++b)  // (.cube: red fastest, then green, then blue)
            for (std::uint32_t g = 0; g < n; ++g)
                for (std::uint32_t r = 0; r < n; ++r) {
                    const auto p = out.pixel(std::size_t(g) * w + b * n + r);
                    for (std::size_t c = 0; c < 3; ++c) {
                        if (c > 0) text += ' ';
                        core::lut3d::append_fixed6(text, std::clamp(p[c], 0.0, 1.0));
                    }
                    text += '\n';
                }
    }
    return text;
}

core::Json run(const core::Document& episode_in, const std::optional<std::filesystem::path>& project, std::string_view key,
               const std::filesystem::path& out, const RunOptions& o) {
    using core::Json;
    // (a book whose pages are still being read: the ones not read yet would come out empty)
    if (!episode_in.deferred.empty()) throw core::Error("page_not_loaded", "the book's pages are still being read: try again in a moment");
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
        // (TEXT-01: a ruby that needs review stops the official export, here before the rest, which is not ported yet)
        if (const std::int64_t n = core::ruby::review_count(*episode); n > 0) {
            Json refused = refuse("the official export stops: " + std::to_string(n) + " ruby reading" + (n == 1 ? " needs" : "s need") +
                                  " review (reassign or clear " + (n == 1 ? "it" : "them") + " first)");
            refused["code"] = "ruby_review";
            return refused;
        }
        // (HumanService.export: preflight and the export approval, the studio's)
        if (o.official_export) {
            Json reply = o.official_export(*project);
            if (const auto notes = export_notes(*episode, key, o.color); !notes.empty() && reply.value("ok", false)) reply["notes"] = notes;
            return reply;
        }
        Json refused = refuse("official export is not in the C++ build yet");
        refused["code"] = "not_yet_ported";
        return refused;
    }
    const std::int64_t dpi = o.dpi && *o.dpi != 0 ? *o.dpi : default_dpi(*episode, key);
    if (key == "psd" && o.psd_text != "image" && o.psd_text != "editable") return refuse("PSD の文字の扱いが分かりません: " + o.psd_text);
    std::vector<std::filesystem::path> files;
    std::optional<Json> psd_report;  // (PSD-01: what the editable PSD did)
    try {
        if (std::find(fmt->options.begin(), fmt->options.end(), "dpi") != fmt->options.end()) check_dpi(dpi);  // (before anything is made)
        if (key == "pdf" || key == "tiff" || key == "png" || key == "cmyk") {
            files = export_print(*episode, out, std::string(key), dpi, 180, true, o.area, key == "cmyk" ? std::string("cmyk") : o.color, o.icc, o.screen);
        } else if (key == "layers") {
            files = export_layers(*episode, out, static_cast<int>(dpi), o.area);
        } else if (key == "kindle") {
            files = {export_kindle(*episode, detail::join(out, stem(*episode) + "_kindle.epub"), o.long_edge, std::nullopt, o.dots)};
        } else if (key == "psd" && o.psd_text == "editable") {
            PsdTextReport report;
            files = export_psd_pages(*episode, out, dpi, PsdText{true, o.psd_rasterize}, &report);
            psd_report = report.to_json();
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
    } catch (const PsdTextRefused& e) {  // (PSD-01, strict: nothing written; the lines and what they use)
        Json error = refuse(e.what());
        error["code"] = e.code();
        error["items"] = issues_json(e.issues());
        return error;
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
    Json reply{{"ok", true}, {"files", written}};
    if (psd_report) reply["psd_text"] = *psd_report;
    if (const auto notes = export_notes(*episode, key, key == "cmyk" ? std::string("cmyk") : o.color); !notes.empty()) reply["notes"] = notes;
    return reply;
}

}  // namespace genko::formats
