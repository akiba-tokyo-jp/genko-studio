// PSD-01: the editable PSD (formats/psd_text.hpp): what PSD text cannot keep, the fonts named and warned of, the
// refusal and the report. (The layers themselves: formats/psd_export.cpp.)

#include "formats/psd_text.hpp"

#include <QFile>
#include <QString>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>

#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "formats/psd_text_detail.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"

namespace genko::formats {

using core::Json;

namespace {

const Json& at(const Json& st, const char* key) {
    static const Json none;
    const auto it = st.find(key);
    return it == st.end() ? none : *it;
}

std::uint32_t be16(std::string_view d, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]);
}
std::uint32_t be32(std::string_view d, std::size_t at) { return (be16(d, at) << 16) | be16(d, at + 2); }

// nameID 6 (the PostScript name) of an OpenType / TrueType font (the first of a collection): Windows' UTF-16 record,
// else the Macintosh one. Nothing when the file does not hold one.
std::optional<std::string> postscript_name(std::string_view d) {
    std::size_t base = 0;
    if (d.size() >= 16 && d.substr(0, 4) == "ttcf") base = be32(d, 12);
    if (base > d.size() || d.size() - base < 12) return std::nullopt;
    const std::uint32_t tables = be16(d, base + 4);
    for (std::uint32_t i = 0; i < tables; ++i) {
        const std::size_t rec = base + 12 + static_cast<std::size_t>(i) * 16;
        if (rec + 16 > d.size()) return std::nullopt;
        if (d.substr(rec, 4) != "name") continue;
        const std::size_t name = be32(d, rec + 8);
        if (name > d.size() || d.size() - name < 6) return std::nullopt;
        const std::uint32_t count = be16(d, name + 2);
        const std::size_t strings = name + be16(d, name + 4);
        std::optional<std::string> mac;
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::size_t r = name + 6 + static_cast<std::size_t>(k) * 12;
            if (r + 12 > d.size()) return std::nullopt;
            if (be16(d, r + 6) != 6) continue;
            const std::uint32_t platform = be16(d, r);
            const std::size_t length = be16(d, r + 8);
            const std::size_t offset = strings + be16(d, r + 10);
            if (offset > d.size() || d.size() - offset < length) continue;
            std::string text;
            if (platform == 3 || platform == 0) {
                for (std::size_t c = 0; c + 1 < length; c += 2) {
                    const std::uint32_t unit = be16(d, offset + c);
                    if (unit >= 0x20 && unit < 0x7f) text += static_cast<char>(unit);
                }
                if (!text.empty()) return text;
            } else if (platform == 1 && !mac) {
                for (std::size_t c = 0; c < length; ++c) {
                    const auto b = static_cast<unsigned char>(d[offset + c]);
                    if (b >= 0x20 && b < 0x7f) text += static_cast<char>(b);
                }
                if (!text.empty()) mac = text;
            }
        }
        return mac;
    }
    return std::nullopt;
}

bool bundled_font(const std::string& name) {
    return name == "gothic" || name == "mincho" || name == "maru" || name == "hand" || name == "sfx" || name == "sfx_pop";
}

}  // namespace

namespace detail {

bool placed_line(const core::StoryLine& line) { return line.x_mm.truthy() || line.y_mm.truthy() || !line.balloon.empty(); }

std::string words_of(const std::string& text, std::size_t count) {
    std::string words = text;
    std::erase(words, '\n');
    std::size_t end = 0;
    for (std::size_t n = 0; n < count && end < words.size(); ++n) {
        const auto lead = static_cast<unsigned char>(words[end]);
        end += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    }
    return words.substr(0, std::min(end, words.size()));
}

core::StoryLine by_place(const core::StoryLine& line) {
    core::StoryLine twin = line;
    if (!twin.ruby_spans) {
        std::vector<core::RubySpan> spans = core::ruby::from_runs(line.text, line.ruby_runs, false);
        if (!spans.empty()) twin.ruby_spans = std::move(spans);
    }
    return twin;
}

std::string font_token(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-') out += c;
    }
    return out;
}

std::vector<std::string> line_features(const core::StoryLine& line, std::optional<std::int64_t> dpi, const std::optional<std::string>& font_path) {
    const Json st = render::text::style_of(line);
    std::vector<std::string> out;
    if (core::py_truthy(at(st, "gradient"))) out.emplace_back("gradient");
    if (core::py_truthy(at(st, "fill_png"))) out.emplace_back("picture_fill");
    if (core::py_truthy(at(st, "text_path"))) out.emplace_back("text_path");
    if (core::py_truthy(at(st, "arc"))) out.emplace_back("arc");
    if (core::py_truthy(at(st, "warp"))) out.emplace_back("warp");
    for (const std::string& run : line.emphasis_runs) {
        if (!run.empty() && line.text.find(run) != std::string::npos) {
            out.emplace_back("emphasis");
            break;
        }
    }
    if (dpi && !line.ruby_spans && !line.ruby_runs.empty() && !core::py_truthy(at(st, "text_path"))) {
        const core::StoryLine twin = by_place(line);
        if (twin.ruby_spans) {  // (set by place as the editable PSD sets it, against the line's own lettering)
            const int at_dpi = static_cast<int>(*dpi);
            const std::shared_ptr<const render::text::Layout> own = render::text::remembered_layout(line, at_dpi, font_path);
            const render::text::Layout placed = render::text::text_layout(twin, at_dpi, font_path);
            if (own->image.size() != placed.image.size() || own->corner_x != placed.corner_x || own->corner_y != placed.corner_y ||
                own->image.tobytes() != placed.image.tobytes()) {
                out.emplace_back("ruby_runs");
            }
        }
    }
    return out;
}

std::string font_name(render::text::Fonts& fonts, const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, std::string> known;  // (font file → its name: the files never change while Genko runs)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = known.find(path); it != known.end()) return it->second;
    }
    std::string bytes;
    try {
        (void)fonts.truetype(path, 12);  // (a bundled font's resources are there once it is opened)
        QFile file(bundled_font(path) ? QStringLiteral(":/genko/text/") + QString::fromStdString(path) : QString::fromStdString(path));
        if (file.open(QIODevice::ReadOnly) && file.size() <= 64LL * 1024 * 1024) bytes = file.readAll().toStdString();
    } catch (const std::exception&) {
        // (a font that is not read: named by its file)
    }
    const std::optional<std::string> found = postscript_name(bytes);
    std::string name = font_token(found ? *found : core::path_to_utf8(core::path_from_utf8(path).stem()));
    if (name.empty()) name = "Font";  // (a name of none of those characters)
    const std::lock_guard<std::mutex> lock(mutex);
    known.emplace(path, name);
    return name;
}

void font_warnings(const core::StoryLine& line, std::int64_t page, const std::optional<std::string>& font_path, render::text::Fonts& fonts,
                   std::vector<std::string>* used, std::vector<Json>& warnings) {
    const auto [face, missing] = render::text::line_face(line, font_path);
    const Json st = render::text::style_of(line);
    if (missing) warnings.push_back(Json{{"page", page}, {"line", line.id}, {"kind", "font_missing"}, {"detail", core::py_str(at(st, "font"))}});
    const auto note = [&](const std::string& path) {
        if (used == nullptr) return;
        const std::string name = font_name(fonts, path);
        if (std::find(used->begin(), used->end(), name) == used->end()) used->push_back(name);
    };
    std::u32string elsewhere;
    for (const char32_t c : fonts.normalize(face, render::text::u32(line.text))) {
        if (c == U'\n' || render::text::py_isspace(c) || render::text::is_vs(c)) continue;
        const std::string path = fonts.path_for(face, std::u32string(1, c));
        note(path);
        if (path != face.kana && path != face.other) elsewhere.push_back(c);
    }
    if (!elsewhere.empty()) {
        warnings.push_back(Json{{"page", page}, {"line", line.id}, {"kind", "font_fallback"}, {"detail", render::text::utf8(elsewhere)}});
    }
    if (!line.ruby_spans) return;
    for (const core::RubySpan& span : *line.ruby_spans) {
        if (!span.review.empty()) continue;
        const bool found = span.font && render::text::face_exists(*span.font);
        if (span.font && !found) warnings.push_back(Json{{"page", page}, {"line", line.id}, {"kind", "font_missing"}, {"detail", *span.font}});
        const render::text::Face own = found ? render::text::face_of(Json(*span.font)) : face;
        for (const char32_t c : render::text::u32(span.ruby)) note(fonts.path_for(own, std::u32string(1, c)));
    }
}

}  // namespace detail

const std::vector<PsdTextFeature>& psd_text_unsupported() {
    static const std::vector<PsdTextFeature> table{
        {"gradient", "グラデーションの文字塗り", "PSD の文字の色は単色だけのため、文字にできない"},
        {"picture_fill", "画像の文字塗り", "PSD の文字の色は単色だけのため、文字にできない"},
        {"text_path", "パスに沿った文字", "パス上文字は初版では書かない"},
        {"arc", "弓なりの曲げ", "非線形の変形は文字レイヤーの行列に表せない"},
        {"warp", "四隅を引くゆがみ", "非線形の変形は文字レイヤーの行列に表せない"},
        {"emphasis", "傍点", "PSD の文字に傍点がないため、文字にできない"},
        {"ruby_runs", "組み方の変わる旧形式のルビ",
         "旧形式のルビ（ruby_runs）は位置で持つルビにして書くが、読みが本文より長いときなど画面と違う組み方になるため、文字にできない（ルビを振り直すと文字にできる）"},
    };
    return table;
}

std::vector<PsdTextIssue> psd_text_issues(const core::Document& episode, std::int64_t dpi) {
    std::vector<PsdTextIssue> out;
    const std::optional<std::string> font_path(episode.font_path);
    for (const auto& page : episode.pages) {
        for (const core::StoryLine* line : episode.story_for_page(page->index)) {
            if (!detail::placed_line(*line)) continue;
            std::vector<std::string> features = detail::line_features(*line, dpi, font_path);
            if (features.empty()) continue;
            out.push_back(PsdTextIssue{core::py_int(page->index.json()), line->id, detail::words_of(line->text, 24), std::move(features)});
        }
    }
    return out;
}

Json issues_json(const std::vector<PsdTextIssue>& issues) {
    Json out = Json::array();
    for (const PsdTextIssue& issue : issues) {
        out.push_back(Json{{"page", issue.page}, {"line", issue.line}, {"text", issue.text}, {"features", Json(issue.features)}});
    }
    return out;
}

PsdTextFonts psd_text_fonts(const core::Document& episode) {
    PsdTextFonts out;
    render::text::Fonts fonts;
    const std::optional<std::string> font_path(episode.font_path);
    for (const auto& page : episode.pages) {
        for (const core::StoryLine* line : episode.story_for_page(page->index)) {
            if (!detail::placed_line(*line) || !detail::line_features(*line).empty()) continue;
            detail::font_warnings(*line, core::py_int(page->index.json()), font_path, fonts, &out.fonts, out.warnings);
        }
    }
    return out;
}

namespace {

std::string refusal(const std::vector<PsdTextIssue>& issues) {
    const std::size_t n = issues.size();
    std::string listed;
    for (std::size_t i = 0; i < n && i < 10; ++i) {
        std::string features;
        for (const auto& f : issues[i].features) features += (features.empty() ? "" : ", ") + f;
        listed += (listed.empty() ? "" : "; ") + std::string("page ") + std::to_string(issues[i].page) + " 「" + issues[i].text + "」: " + features;
    }
    if (n > 10) listed += "; …";
    return "the editable PSD stops: " + std::to_string(n) + (n == 1 ? " line uses" : " lines use") + " what PSD text cannot keep (" + listed +
           "): draw " + (n == 1 ? "it" : "them") + " as pictures (rasterize) or change " + (n == 1 ? "it" : "them");
}

}  // namespace

PsdTextRefused::PsdTextRefused(std::vector<PsdTextIssue> issues) : core::Error("psd_text_unsupported", refusal(issues)), issues_(std::move(issues)) {}

Json PsdTextReport::to_json() const {
    return Json{{"mode", "editable"},
                {"text_layers", text_layers},
                {"ruby_layers", ruby_layers},
                {"rasterized", issues_json(rasterized)},
                {"fonts", Json(fonts)},
                {"warnings", Json(warnings)}};
}

}  // namespace genko::formats
