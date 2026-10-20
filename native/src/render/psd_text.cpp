// PSD-01: Photoshop's text layer structures (render/psd_text.hpp): ActionDescriptors (render/psd_descriptor, the PSD's
// one reader and writer of them) and the text engine's EngineData, written and read; the TySh and lfx2 blocks made of
// them.

#include "render/psd_text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#include "core/error.hpp"

namespace genko::render::psd {

namespace {

[[noreturn]] void broken(const std::string& what) { throw core::Error("format", "the text layer cannot be read (" + what + ")"); }

// ---- ActionDescriptor (render/psd_descriptor: the PSD's one reader and writer) ----------------------------------

using descriptor::Bytes;
using descriptor::put_f64;
using descriptor::put_i32;
using descriptor::put_u16;
using descriptor::put_u32;
using Descriptor = descriptor::Writer;
using DObject = descriptor::Object;
using DValue = descriptor::Value;

// A reading of a block made of descriptors: what the descriptor reader cannot read is the text layer's format error.
template <typename Read>
auto reading(Read&& read) {
    try {
        return read();
    } catch (const descriptor::Broken& error) {
        broken(error.what());
    }
}

// ---- EngineData -------------------------------------------------------------------------------------------------

// A value of the text engine's data: a dictionary (its keys in order), a list, a text, a number (an integer is written
// without a point), a truth value.
struct Engine {
    enum class Kind { dict, list, text, integer, number, boolean, name, null } kind = Kind::null;
    std::vector<std::pair<std::string, Engine>> dict;
    std::vector<Engine> list;
    std::u16string text;
    double number = 0.0;
    bool boolean = false;
    std::string name;

    static Engine d(std::vector<std::pair<std::string, Engine>> entries) {
        Engine e;
        e.kind = Kind::dict;
        e.dict = std::move(entries);
        return e;
    }
    static Engine l(std::vector<Engine> items) {
        Engine e;
        e.kind = Kind::list;
        e.list = std::move(items);
        return e;
    }
    static Engine t(std::u16string_view value) {
        Engine e;
        e.kind = Kind::text;
        e.text = value;
        return e;
    }
    static Engine i(std::int64_t value) {
        Engine e;
        e.kind = Kind::integer;
        e.number = static_cast<double>(value);
        return e;
    }
    static Engine f(double value) {
        Engine e;
        e.kind = Kind::number;
        e.number = value;
        return e;
    }
    static Engine b(bool value) {
        Engine e;
        e.kind = Kind::boolean;
        e.boolean = value;
        return e;
    }
    static Engine fs(std::initializer_list<double> values) {
        std::vector<Engine> items;
        for (const double v : values) items.push_back(f(v));
        return l(std::move(items));
    }
    const Engine* get(std::string_view key) const {
        if (kind != Kind::dict) return nullptr;
        for (const auto& [k, v] : dict) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

// A number as the text engine writes it: integers without a point, others in plain decimals (never an exponent, which
// readers of EngineData do not take), always with a digit after the point.
std::string engine_number(double v, bool integer) {
    if (!std::isfinite(v)) v = 0.0;
    if (integer) return std::to_string(static_cast<std::int64_t>(std::llround(v)));
    char buffer[400];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, v, std::chars_format::fixed);
    std::string out(buffer, result.ptr);
    if (out.find('.') == std::string::npos) out += ".0";
    return out;
}

void engine_text(std::string& out, std::u16string_view text) {
    out += "(\xfe\xff";
    for (const char16_t c : text) {
        for (const unsigned byte : {static_cast<unsigned>(c >> 8), static_cast<unsigned>(c & 0xff)}) {
            if (byte == '(' || byte == ')' || byte == '\\') out += '\\';
            out += static_cast<char>(byte);
        }
    }
    out += ')';
}

void write_engine(std::string& out, const Engine& value, int indent);

void write_entries(std::string& out, const Engine& dict, int indent) {
    for (const auto& [key, value] : dict.dict) {
        out.append(static_cast<std::size_t>(indent), '\t');
        out += "/" + key;
        if (value.kind == Engine::Kind::dict || (value.kind == Engine::Kind::list && !value.list.empty() &&
                                                 value.list.front().kind != Engine::Kind::integer &&
                                                 value.list.front().kind != Engine::Kind::number)) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent), '\t');
        } else {
            out += ' ';
        }
        write_engine(out, value, indent);
        out += '\n';
    }
}

void write_engine(std::string& out, const Engine& value, int indent) {
    switch (value.kind) {
    case Engine::Kind::dict:
        out += "<<\n";
        write_entries(out, value, indent + 1);
        out.append(static_cast<std::size_t>(indent), '\t');
        out += ">>";
        break;
    case Engine::Kind::list: {
        const bool numbers = !value.list.empty() &&
                             (value.list.front().kind == Engine::Kind::integer || value.list.front().kind == Engine::Kind::number);
        if (value.list.empty() || numbers) {
            out += "[";
            for (const Engine& item : value.list) {
                out += ' ';
                write_engine(out, item, indent);
            }
            out += " ]";
            break;
        }
        out += "[\n";
        for (const Engine& item : value.list) {
            out.append(static_cast<std::size_t>(indent + 1), '\t');
            write_engine(out, item, indent + 1);
            out += '\n';
        }
        out.append(static_cast<std::size_t>(indent), '\t');
        out += ']';
        break;
    }
    case Engine::Kind::text:
        engine_text(out, value.text);
        break;
    case Engine::Kind::integer:
    case Engine::Kind::number:
        out += engine_number(value.number, value.kind == Engine::Kind::integer);
        break;
    case Engine::Kind::boolean:
        out += value.boolean ? "true" : "false";
        break;
    case Engine::Kind::name:
        out += "/" + value.name;
        break;
    case Engine::Kind::null:
        out += "null";
        break;
    }
}

// The text engine's data read back (as Photoshop, psd-tools and Krita read it: tokens between whitespace, texts in
// brackets as UTF-16 with \-escapes).
class EngineReader {
public:
    explicit EngineReader(std::string_view data) : data_(data) {}

    Engine root() {
        skip();
        Engine out;
        if (data_.substr(at_, 2) == "<<") {
            out = value(0);
        } else {  // (a dictionary without its brackets, as the global Txt2 block writes it)
            out.kind = Engine::Kind::dict;
            entries(out, 0, true);
        }
        return out;
    }

private:
    static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0'; }
    void skip() {
        while (at_ < data_.size() && space(data_[at_])) ++at_;
    }
    bool ends(char c) const { return space(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')'; }

    std::string word() {
        const std::size_t from = at_;
        while (at_ < data_.size() && !ends(data_[at_])) ++at_;
        return std::string(data_.substr(from, at_ - from));
    }

    void entries(Engine& dict, int depth, bool to_end) {
        for (;;) {
            skip();
            if (at_ >= data_.size()) {
                if (to_end) return;
                broken("the text engine's data ends early");
            }
            if (data_.substr(at_, 2) == ">>") {
                if (to_end) broken("a stray >> in the text engine's data");
                at_ += 2;
                return;
            }
            if (data_[at_] != '/') broken("a key expected in the text engine's data");
            ++at_;
            std::string key = word();
            dict.dict.emplace_back(std::move(key), value(depth + 1));
        }
    }

    Engine value(int depth) {
        if (depth > 64) broken("the text engine's data nests too deep");
        skip();
        if (at_ >= data_.size()) broken("the text engine's data ends early");
        Engine out;
        const char c = data_[at_];
        if (data_.substr(at_, 2) == "<<") {
            at_ += 2;
            out.kind = Engine::Kind::dict;
            entries(out, depth, false);
        } else if (c == '[') {
            ++at_;
            out.kind = Engine::Kind::list;
            for (;;) {
                skip();
                if (at_ >= data_.size()) broken("the text engine's data ends early");
                if (data_[at_] == ']') {
                    ++at_;
                    break;
                }
                out.list.push_back(value(depth + 1));
            }
        } else if (c == '(') {
            ++at_;
            out.kind = Engine::Kind::text;
            std::string bytes;
            for (;;) {
                if (at_ >= data_.size()) broken("a text in the text engine's data does not end");
                char b = data_[at_++];
                if (b == ')') break;
                if (b == '\\') {
                    if (at_ >= data_.size()) broken("a text in the text engine's data does not end");
                    b = data_[at_++];
                }
                bytes += b;
            }
            if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe && static_cast<unsigned char>(bytes[1]) == 0xff) {
                for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
                    out.text.push_back(static_cast<char16_t>((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1])));
                }
            } else {
                for (const char b : bytes) out.text.push_back(static_cast<char16_t>(static_cast<unsigned char>(b)));
            }
        } else if (c == '/') {
            ++at_;
            out.kind = Engine::Kind::name;
            out.name = word();
        } else {
            const std::string token = word();
            if (token.empty()) broken("an unknown token in the text engine's data");
            if (token == "true" || token == "false") {
                out.kind = Engine::Kind::boolean;
                out.boolean = token == "true";
            } else if (token == "null") {
                out.kind = Engine::Kind::null;
            } else {
                double v = 0.0;
                const auto result = std::from_chars(token.data(), token.data() + token.size(), v);
                if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
                    // (".5" and "-.5": from_chars wants a digit first)
                    std::string padded = token;
                    const std::size_t dot = padded.find('.');
                    if (dot == 0 || (dot == 1 && padded[0] == '-')) padded.insert(dot, "0");
                    const auto again = std::from_chars(padded.data(), padded.data() + padded.size(), v);
                    if (again.ec != std::errc() || again.ptr != padded.data() + padded.size()) broken("a number '" + token + "'");
                }
                out.kind = token.find('.') == std::string::npos ? Engine::Kind::integer : Engine::Kind::number;
                out.number = v;
            }
        }
        return out;
    }

    std::string_view data_;
    std::size_t at_ = 0;
};

const Engine* path(const Engine& root, std::initializer_list<std::string_view> keys) {
    const Engine* at = &root;
    for (const std::string_view key : keys) {
        at = at->get(key);
        if (at == nullptr) return nullptr;
    }
    return at;
}

double number_or(const Engine* e, double fallback) {
    return e != nullptr && (e->kind == Engine::Kind::number || e->kind == Engine::Kind::integer) ? e->number : fallback;
}
bool bool_or(const Engine* e, bool fallback) { return e != nullptr && e->kind == Engine::Kind::boolean ? e->boolean : fallback; }

// ---- what Photoshop's text engine has around a text --------------------------------------------------------------

Engine colour(const std::array<int, 3>& rgb) {
    return Engine::d({{"Type", Engine::i(1)},
                      {"Values", Engine::fs({1.0, rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0})}});
}

// (color-mode@1) a CMYK document's colour: Type 2, alpha then C, M, Y, K (0..1)
Engine colour_cmyk(const std::array<double, 4>& cmyk) {
    return Engine::d({{"Type", Engine::i(2)}, {"Values", Engine::fs({1.0, cmyk[0], cmyk[1], cmyk[2], cmyk[3]})}});
}

// The RGB a CMYK colour stands for, without a profile: K alone its grey (the page's rules backwards), else each ink
// taking its share of the light.
std::array<int, 3> rgb_of_cmyk(const std::array<double, 4>& c) {
    if (c[0] == 0 && c[1] == 0 && c[2] == 0) {
        const int v = static_cast<int>(std::lround(std::clamp(1.0 - c[3], 0.0, 1.0) * 255.0));
        return {v, v, v};
    }
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(std::lround(std::clamp((1.0 - c[i]) * (1.0 - c[3]), 0.0, 1.0) * 255.0));
    return out;
}

Engine paragraph_properties(int justification) {
    return Engine::d({{"Justification", Engine::i(justification)},
                      {"FirstLineIndent", Engine::f(0.0)},
                      {"StartIndent", Engine::f(0.0)},
                      {"EndIndent", Engine::f(0.0)},
                      {"SpaceBefore", Engine::f(0.0)},
                      {"SpaceAfter", Engine::f(0.0)},
                      {"AutoHyphenate", Engine::b(false)},
                      {"HyphenatedWordSize", Engine::i(6)},
                      {"PreHyphen", Engine::i(2)},
                      {"PostHyphen", Engine::i(2)},
                      {"ConsecutiveHyphens", Engine::i(8)},
                      {"Zone", Engine::f(36.0)},
                      {"WordSpacing", Engine::fs({0.8, 1.0, 1.33})},
                      {"LetterSpacing", Engine::fs({0.0, 0.0, 0.0})},
                      {"GlyphSpacing", Engine::fs({1.0, 1.0, 1.0})},
                      {"AutoLeading", Engine::f(1.2)},
                      {"LeadingType", Engine::i(0)},
                      {"Hanging", Engine::b(false)},
                      {"Burasagari", Engine::b(false)},
                      {"KinsokuOrder", Engine::i(0)},
                      {"EveryLineComposer", Engine::b(false)}});
}

Engine style_sheet(const TextStyle& s, std::int64_t font) {
    return Engine::d({{"Font", Engine::i(font)},
                      {"FontSize", Engine::f(s.size)},
                      {"FauxBold", Engine::b(s.faux_bold)},
                      {"FauxItalic", Engine::b(s.faux_italic)},
                      {"AutoLeading", Engine::b(false)},
                      {"Leading", Engine::f(s.leading)},
                      {"HorizontalScale", Engine::f(s.horizontal_scale)},
                      {"VerticalScale", Engine::f(s.vertical_scale)},
                      {"Tracking", Engine::f(s.tracking)},
                      {"AutoKerning", Engine::b(true)},
                      {"Kerning", Engine::i(0)},
                      {"BaselineShift", Engine::f(0.0)},
                      {"FontCaps", Engine::i(0)},
                      {"FontBaseline", Engine::i(0)},
                      {"Underline", Engine::b(false)},
                      {"Strikethrough", Engine::b(false)},
                      {"Ligatures", Engine::b(true)},
                      {"DLigatures", Engine::b(false)},
                      {"BaselineDirection", Engine::i(s.baseline_direction)},
                      {"Tsume", Engine::f(0.0)},
                      {"StyleRunAlignment", Engine::i(s.run_alignment)},
                      {"Language", Engine::i(0)},
                      {"NoBreak", Engine::b(false)},
                      {"FillColor", s.fill_cmyk ? colour_cmyk(*s.fill_cmyk) : colour(s.fill)},
                      {"StrokeColor", s.fill_cmyk ? colour_cmyk({0.0, 0.0, 0.0, 1.0}) : colour({0, 0, 0})},
                      {"FillFlag", Engine::b(true)},
                      {"StrokeFlag", Engine::b(false)},
                      {"FillFirst", Engine::b(true)},
                      {"YUnderline", Engine::i(1)},
                      {"OutlineWidth", Engine::f(1.0)},
                      {"CharacterDirection", Engine::i(0)},
                      {"HindiNumbers", Engine::b(false)},
                      {"Kashida", Engine::i(1)},
                      {"DiacriticPos", Engine::i(2)}});
}

// Photoshop's own kinsoku and mojikumi sets (the resources every text names; this writer sets nothing by them).
Engine resources(const std::vector<std::string>& fonts, const TextStyle& normal) {
    const auto kinsoku = [](std::u16string_view name, std::u16string_view no_start, std::u16string_view no_end) {
        return Engine::d({{"Name", Engine::t(name)},
                          {"NoStart", Engine::t(no_start)},
                          {"NoEnd", Engine::t(no_end)},
                          {"Keep", Engine::t(u"―‥")},
                          {"Hanging", Engine::t(u"、。.,")}});
    };
    std::vector<Engine> font_set;
    for (const std::string& font : fonts) {
        font_set.push_back(Engine::d({{"Name", Engine::t(utf16(font))},
                                      {"Script", Engine::i(0)},
                                      {"FontType", Engine::i(0)},
                                      {"Synthetic", Engine::i(0)}}));
    }
    std::vector<Engine> mojikumi;
    for (int i = 1; i <= 4; ++i) {
        mojikumi.push_back(Engine::d({{"InternalName", Engine::t(u"Photoshop6MojiKumiSet" + std::u16string(1, static_cast<char16_t>(u'0' + i)))}}));
    }
    return Engine::d({{"KinsokuSet",
                       Engine::l({kinsoku(u"PhotoshopKinsokuHard",
                                          u"、。，．・：；？！ー―’”）〕］｝"
                                          u"〉》」』】ヽヾゝゞ々ぁぃぅぇぉっゃ"
                                          u"ゅょゎァィゥェォッャュョヮヵヶ゛゜"
                                          u"?!)]},.:;℃℉¢％‰",
                                          u"‘“（〔［｛〈《「『【([{￥＄£＠§〒＃"),
                                  kinsoku(u"PhotoshopKinsokuSoft",
                                          u"、。，．・：；？！’”）〕］｝〉》"
                                          u"」』】ヽヾゝゞ々",
                                          u"‘“（〔［｛〈《「『【")})},
                      {"MojiKumiSet", Engine::l(std::move(mojikumi))},
                      {"TheNormalStyleSheet", Engine::i(0)},
                      {"TheNormalParagraphSheet", Engine::i(0)},
                      {"ParagraphSheetSet",
                       Engine::l({Engine::d({{"Name", Engine::t(u"Normal RGB")},
                                             {"DefaultStyleSheet", Engine::i(0)},
                                             {"Properties", paragraph_properties(0)}})})},
                      {"StyleSheetSet", Engine::l({Engine::d({{"Name", Engine::t(u"Normal RGB")},
                                                         {"StyleSheetData", style_sheet(normal, fonts.size() > 1 ? 1 : 0)}})})},
                      {"FontSet", Engine::l(std::move(font_set))},
                      {"SuperscriptSize", Engine::f(0.583)},
                      {"SuperscriptPosition", Engine::f(0.333)},
                      {"SubscriptSize", Engine::f(0.583)},
                      {"SubscriptPosition", Engine::f(0.333)},
                      {"SmallCapSize", Engine::f(0.7)}});
}

std::string engine_data(const TextLayer& layer) {
    std::vector<std::string> fonts{"AdobeInvisFont"};
    const auto font_index = [&](const std::string& name) {
        const auto it = std::find(fonts.begin(), fonts.end(), name);
        if (it != fonts.end()) return static_cast<std::int64_t>(it - fonts.begin());
        fonts.push_back(name);
        return static_cast<std::int64_t>(fonts.size() - 1);
    };
    std::vector<Engine> style_runs;
    std::vector<Engine> style_lengths;
    for (const StyleRun& run : layer.runs) {
        style_runs.push_back(Engine::d({{"StyleSheet", Engine::d({{"StyleSheetData", style_sheet(run.style, font_index(run.style.font))}})}}));
        style_lengths.push_back(Engine::i(static_cast<std::int64_t>(run.length)));
    }
    std::vector<Engine> paragraph_runs;
    std::vector<Engine> paragraph_lengths;
    const auto adjustments = [] { return Engine::d({{"Axis", Engine::fs({1.0, 0.0, 1.0})}, {"XY", Engine::fs({0.0, 0.0})}}); };
    for (const ParagraphRun& run : layer.paragraphs) {
        paragraph_runs.push_back(Engine::d({{"ParagraphSheet", Engine::d({{"DefaultStyleSheet", Engine::i(0)}, {"Properties", paragraph_properties(run.justification)}})},
                                            {"Adjustments", adjustments()}}));
        paragraph_lengths.push_back(Engine::i(static_cast<std::int64_t>(run.length)));
    }
    const std::int64_t direction = layer.vertical ? 2 : 0;
    const std::int64_t shape = layer.box ? 1 : 0;
    std::vector<std::pair<std::string, Engine>> photoshop{{"ShapeType", Engine::i(shape)}};
    if (layer.box) {
        photoshop.emplace_back("BoxBounds", Engine::fs({(*layer.box)[0], (*layer.box)[1], (*layer.box)[2], (*layer.box)[3]}));
    } else {
        photoshop.emplace_back("PointBase", Engine::fs({0.0, 0.0}));
    }
    photoshop.emplace_back("Base", Engine::d({{"ShapeType", Engine::i(shape)},
                                              {"TransformPoint0", Engine::fs({1.0, 0.0})},
                                              {"TransformPoint1", Engine::fs({0.0, 1.0})},
                                              {"TransformPoint2", Engine::fs({0.0, 0.0})}}));
    const TextStyle normal = layer.runs.empty() ? TextStyle{} : layer.runs.front().style;
    Engine engine_dict = Engine::d(
        {{"Editor", Engine::d({{"Text", Engine::t(layer.text + u"\r")}})},
         {"ParagraphRun", Engine::d({{"DefaultRunData", Engine::d({{"ParagraphSheet", Engine::d({{"DefaultStyleSheet", Engine::i(0)},
                                                                                                {"Properties", Engine::d({})}})},
                                                                   {"Adjustments", adjustments()}})},
                                     {"RunArray", Engine::l(std::move(paragraph_runs))},
                                     {"RunLengthArray", Engine::l(std::move(paragraph_lengths))},
                                     {"IsJoinable", Engine::i(1)}})},
         {"StyleRun", Engine::d({{"DefaultRunData", Engine::d({{"StyleSheet", Engine::d({{"StyleSheetData", Engine::d({})}})}})},
                                 {"RunArray", Engine::l(std::move(style_runs))},
                                 {"RunLengthArray", Engine::l(std::move(style_lengths))},
                                 {"IsJoinable", Engine::i(2)}})},
         {"GridInfo", Engine::d({{"GridIsOn", Engine::b(false)},
                                 {"ShowGrid", Engine::b(false)},
                                 {"GridSize", Engine::f(18.0)},
                                 {"GridLeading", Engine::f(22.0)},
                                 {"GridColor", colour({0, 0, 255})},
                                 {"GridLeadingFillColor", colour({0, 0, 255})},
                                 {"AlignLineHeightToGridFlags", Engine::b(false)}})},
         {"AntiAlias", Engine::i(4)},
         {"UseFractionalGlyphWidths", Engine::b(true)},
         {"Rendered", Engine::d({{"Version", Engine::i(1)},
                                 {"Shapes", Engine::d({{"WritingDirection", Engine::i(direction)},
                                                       {"Children", Engine::l({Engine::d({{"ShapeType", Engine::i(shape)},
                                                                                          {"Procession", Engine::i(layer.vertical ? 1 : 0)},
                                                                                          {"Lines", Engine::d({{"WritingDirection", Engine::i(direction)},
                                                                                                               {"Children", Engine::l({})}})},
                                                                                          {"Cookie", Engine::d({{"Photoshop", Engine::d(std::move(photoshop))}})}})})}})}})}});
    const Engine res = resources(fonts, normal);
    const Engine root = Engine::d({{"EngineDict", std::move(engine_dict)}, {"ResourceDict", res}, {"DocumentResources", res}});
    std::string out = "\n\n";
    write_engine(out, root, 0);
    return out;
}

Descriptor bounds_of(const std::array<double, 4>& b) {
    Descriptor d;
    d.unit("Left", "#Pnt", b[0]);
    d.unit("Top ", "#Pnt", b[1]);
    d.unit("Rght", "#Pnt", b[2]);
    d.unit("Btom", "#Pnt", b[3]);
    return d;
}

TextStyle style_from(const Engine& data, const std::vector<std::string>& fonts) {
    TextStyle s;
    const auto index = static_cast<std::int64_t>(number_or(data.get("Font"), 0));
    s.font = index >= 0 && static_cast<std::size_t>(index) < fonts.size() ? fonts[static_cast<std::size_t>(index)] : std::string();
    s.size = number_or(data.get("FontSize"), s.size);
    s.faux_bold = bool_or(data.get("FauxBold"), false);
    s.faux_italic = bool_or(data.get("FauxItalic"), false);
    if (const Engine* values = path(data, {"FillColor", "Values"}); values != nullptr && values->kind == Engine::Kind::list && values->list.size() >= 4) {
        if (number_or(path(data, {"FillColor", "Type"}), 1) == 2 && values->list.size() >= 5) {  // (a CMYK document's)
            s.fill_cmyk = std::array<double, 4>{values->list[1].number, values->list[2].number, values->list[3].number, values->list[4].number};
            s.fill = rgb_of_cmyk(*s.fill_cmyk);
        } else {
            for (std::size_t i = 0; i < 3; ++i) s.fill[i] = static_cast<int>(std::lround(values->list[i + 1].number * 255.0));
        }
    }
    s.tracking = number_or(data.get("Tracking"), 0.0);
    s.leading = number_or(data.get("Leading"), 0.0);
    s.horizontal_scale = number_or(data.get("HorizontalScale"), 1.0);
    s.vertical_scale = number_or(data.get("VerticalScale"), 1.0);
    s.baseline_direction = static_cast<int>(number_or(data.get("BaselineDirection"), 2));
    s.run_alignment = static_cast<int>(number_or(data.get("StyleRunAlignment"), 2));
    return s;
}

std::size_t length_of(const Engine& e) {
    const double v = number_or(&e, -1);
    if (!(v >= 0) || v > 1e9) broken("a run's length");
    return static_cast<std::size_t>(v);
}

}  // namespace

const TextStyle* TextLayer::style_at(std::size_t at) const {
    std::size_t start = 0;
    for (const StyleRun& run : runs) {
        if (at < start + run.length) return &run.style;
        start += run.length;
    }
    return nullptr;
}

std::string tysh(const TextLayer& layer) {
    Descriptor text;
    text.text("Txt ", layer.text);
    text.enumerated("textGridding", "textGridding", "None");
    text.enumerated("Ornt", "Ornt", layer.vertical ? "Vrtc" : "Hrzn");
    text.enumerated("AntA", "Annt", "antiAliasSharp");
    text.object("bounds", bounds_of(layer.bounds), "bounds");
    text.object("boundingBox", bounds_of(layer.bounds), "boundingBox");
    text.integer("TextIndex", 0);
    text.raw("EngineData", engine_data(layer));
    Descriptor warp;
    warp.enumerated("warpStyle", "warpStyle", layer.warp.empty() ? std::string("warpNone") : layer.warp);
    warp.number("warpValue", layer.warp_value);
    warp.number("warpPerspective", layer.warp_perspective);
    warp.number("warpPerspectiveOther", layer.warp_perspective_other);
    warp.enumerated("warpRotate", "Ornt", layer.warp_vertical ? "Vrtc" : "Hrzn");
    std::string out;
    put_u16(out, 1);
    for (const double v : layer.transform) put_f64(out, v);
    put_u16(out, 50);
    put_u32(out, 16);
    out += text.structure("TxLr");
    put_u16(out, 1);
    put_u32(out, 16);
    out += warp.structure("warp");
    for (int i = 0; i < 4; ++i) put_i32(out, 0);  // (left, top, right, bottom: Photoshop's own; 0 is read the same as either kind)
    return out;
}

namespace {

TextLayer tysh_of(std::string_view payload) {
    Bytes b(payload);
    TextLayer out;
    if (b.left() < 2 || b.u16() != 1) broken("not a TySh block of version 1");
    for (double& v : out.transform) v = b.f64();
    if (b.u16() != 50) broken("a text version other than 50");
    if (b.u32() != 16) broken("a descriptor version other than 16");
    const DObject text = descriptor::read_structure(b, 0);
    if (b.u16() != 1) broken("a warp version other than 1");
    if (b.u32() != 16) broken("a descriptor version other than 16");
    const DObject warp = descriptor::read_structure(b, 0);
    if (const DValue* t = text.get("Txt "); t != nullptr) out.text = t->text;
    if (const DValue* o = text.get("Ornt"); o != nullptr) out.vertical = o->enum_value == "Vrtc";
    if (const DValue* bounds = text.get("bounds"); bounds != nullptr && bounds->object) {
        const char* const keys[4] = {"Left", "Top ", "Rght", "Btom"};
        for (std::size_t i = 0; i < 4; ++i) {
            if (const DValue* v = bounds->object->get(keys[i])) out.bounds[i] = v->number;
        }
    }
    if (const DValue* w = warp.get("warpStyle"); w != nullptr) out.warp = w->enum_value;
    if (const DValue* v = warp.get("warpValue"); v != nullptr) out.warp_value = v->number;
    if (const DValue* v = warp.get("warpPerspective"); v != nullptr) out.warp_perspective = v->number;
    if (const DValue* v = warp.get("warpPerspectiveOther"); v != nullptr) out.warp_perspective_other = v->number;
    if (const DValue* r = warp.get("warpRotate"); r != nullptr) out.warp_vertical = r->enum_value == "Vrtc";
    const DValue* data = text.get("EngineData");
    if (data == nullptr) broken("no EngineData");
    const Engine root = EngineReader(data->raw).root();
    std::vector<std::string> fonts;
    if (const Engine* set = path(root, {"ResourceDict", "FontSet"}); set != nullptr) {
        for (const Engine& font : set->list) {
            const Engine* name = font.get("Name");
            fonts.push_back(name != nullptr ? utf8_of(name->text) : std::string());
        }
    }
    if (const Engine* editor = path(root, {"EngineDict", "Editor", "Text"}); editor != nullptr) out.engine_text = editor->text;
    const Engine* style_runs = path(root, {"EngineDict", "StyleRun", "RunArray"});
    const Engine* style_lengths = path(root, {"EngineDict", "StyleRun", "RunLengthArray"});
    if (style_runs == nullptr || style_lengths == nullptr || style_runs->list.size() != style_lengths->list.size()) broken("the style runs");
    for (std::size_t i = 0; i < style_runs->list.size(); ++i) {
        const Engine* data_i = path(style_runs->list[i], {"StyleSheet", "StyleSheetData"});
        out.runs.push_back(StyleRun{length_of(style_lengths->list[i]), data_i != nullptr ? style_from(*data_i, fonts) : TextStyle{}});
    }
    const Engine* paragraph_runs = path(root, {"EngineDict", "ParagraphRun", "RunArray"});
    const Engine* paragraph_lengths = path(root, {"EngineDict", "ParagraphRun", "RunLengthArray"});
    if (paragraph_runs != nullptr && paragraph_lengths != nullptr && paragraph_runs->list.size() == paragraph_lengths->list.size()) {
        for (std::size_t i = 0; i < paragraph_runs->list.size(); ++i) {
            const Engine* justification = path(paragraph_runs->list[i], {"ParagraphSheet", "Properties", "Justification"});
            out.paragraphs.push_back(ParagraphRun{length_of(paragraph_lengths->list[i]), static_cast<int>(number_or(justification, 0))});
        }
    }
    if (const Engine* shapes = path(root, {"EngineDict", "Rendered", "Shapes", "Children"}); shapes != nullptr && !shapes->list.empty()) {
        const Engine* photoshop = path(shapes->list.front(), {"Cookie", "Photoshop"});
        if (photoshop != nullptr && number_or(photoshop->get("ShapeType"), 0) == 1) {
            const Engine* box = photoshop->get("BoxBounds");
            if (box == nullptr || box->list.size() != 4) broken("an area text without its box");
            out.box = std::array<double, 4>{box->list[0].number, box->list[1].number, box->list[2].number, box->list[3].number};
        }
    }
    return out;
}

}  // namespace

TextLayer read_tysh(std::string_view payload) {
    return reading([payload] { return tysh_of(payload); });
}

std::string lfx2(const Stroke& stroke) {
    Descriptor rgb;  // (its colour in the document's mode: RGBC, CMYC in %, LbCl)
    std::string_view colour_class = "RGBC";
    if (stroke.cmyk) {
        rgb.number("Cyn ", (*stroke.cmyk)[0] * 100.0);
        rgb.number("Mgnt", (*stroke.cmyk)[1] * 100.0);
        rgb.number("Ylw ", (*stroke.cmyk)[2] * 100.0);
        rgb.number("Blck", (*stroke.cmyk)[3] * 100.0);
        colour_class = "CMYC";
    } else if (stroke.lab) {
        rgb.number("Lmnc", (*stroke.lab)[0]);
        rgb.number("A   ", (*stroke.lab)[1]);
        rgb.number("B   ", (*stroke.lab)[2]);
        colour_class = "LbCl";
    } else {
        rgb.number("Rd  ", stroke.rgb[0]);
        rgb.number("Grn ", stroke.rgb[1]);
        rgb.number("Bl  ", stroke.rgb[2]);
    }
    Descriptor frame;
    frame.boolean("enab", stroke.enabled);
    frame.boolean("present", true);
    frame.boolean("showInDialog", true);
    frame.enumerated("Styl", "FStl", stroke.position == "inside" ? "InsF" : stroke.position == "center" ? "CtrF" : "OutF");
    frame.enumerated("PntT", "FrFl", "SClr");
    frame.enumerated("Md  ", "BlnM", "Nrml");
    frame.unit("Opct", "#Prc", stroke.opacity);
    frame.unit("Sz  ", "#Pxl", stroke.size);
    frame.object("Clr ", rgb, colour_class);
    frame.boolean("overprint", false);
    Descriptor effects;
    effects.unit("Scl ", "#Prc", 100.0);
    effects.boolean("masterFXSwitch", true);
    effects.object("FrFX", frame, "FrFX");
    effects.integer("numModifyingFX", stroke.enabled ? 1 : 0);
    std::string out;
    put_u32(out, 0);
    put_u32(out, 16);
    return out + effects.structure("null");
}

std::optional<Stroke> read_lfx2(std::string_view payload) {
    const DObject effects = reading([payload] {
        Bytes b(payload);
        if (b.u32() != 0) broken("an lfx2 block of another version");
        if (b.u32() != 16) broken("a descriptor version other than 16");
        return descriptor::read_structure(b, 0);
    });
    const DValue* frame = effects.get("FrFX");
    if (frame == nullptr || !frame->object) {
        const DValue* many = effects.get("frameFXMulti");
        if (many == nullptr || many->list.empty() || !many->list.front().object) return std::nullopt;
        frame = &many->list.front();
    }
    Stroke out;
    const DObject& f = *frame->object;
    if (const DValue* v = f.get("enab")) out.enabled = v->boolean;
    if (const DValue* v = f.get("Sz  ")) out.size = v->number;
    if (const DValue* v = f.get("Opct")) out.opacity = v->number;
    if (const DValue* v = f.get("Styl")) out.position = v->enum_value == "InsF" ? "inside" : v->enum_value == "CtrF" ? "center" : "outside";
    if (const DValue* c = f.get("Clr "); c != nullptr && c->object) {
        const DObject& colour = *c->object;
        const auto number_at = [&](const char* key) { const DValue* v = colour.get(key); return v != nullptr ? v->number : 0.0; };
        if (colour.class_id == "CMYC") {
            out.cmyk = std::array<double, 4>{number_at("Cyn ") / 100.0, number_at("Mgnt") / 100.0, number_at("Ylw ") / 100.0, number_at("Blck") / 100.0};
            out.rgb = rgb_of_cmyk(*out.cmyk);
        } else if (colour.class_id == "LbCl") {
            out.lab = std::array<double, 3>{number_at("Lmnc"), number_at("A   "), number_at("B   ")};
        } else {
            const char* const keys[3] = {"Rd  ", "Grn ", "Bl  "};
            for (std::size_t i = 0; i < 3; ++i) {
                if (const DValue* v = colour.get(keys[i])) out.rgb[i] = static_cast<int>(std::lround(v->number));
            }
        }
    }
    return out;
}

}  // namespace genko::render::psd
