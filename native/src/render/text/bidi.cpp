// PC5 G-b (render/text/bidi.hpp): SheenBidi's bidirectional levels, a row's visual runs and the letters' scripts.

#include "render/text/bidi.hpp"

#include <SheenBidi/SheenBidi.h>

#include <algorithm>

#include "core/error.hpp"

namespace genko::render::text {

struct BidiParagraph::State {
    std::u32string text;
    SBAlgorithmRef algorithm = nullptr;
    SBParagraphRef paragraph = nullptr;
    ~State() {
        if (paragraph != nullptr) SBParagraphRelease(paragraph);
        if (algorithm != nullptr) SBAlgorithmRelease(algorithm);
    }
};

BidiParagraph::BidiParagraph(std::u32string text, std::string_view direction) : state_(std::make_unique<State>()) {
    State& s = *state_;
    s.text = std::move(text);
    // (a paragraph separator of Unicode's — U+2029, CR, NEL, U+001C..U+001E: class B — would end SheenBidi's paragraph
    // early; the typesetting splits its paragraphs at them, and any left — a CR before a line break — is read as a space,
    // which the rule L1 sets at the paragraph's level as it would the separator: review P2-4)
    for (char32_t& c : s.text) {
        if (c == U'\r' || c == U'\u2029' || c == U'\u0085' || (c >= 0x1C && c <= 0x1E) || c == U'\n') c = U' ';
    }
    levels_.assign(s.text.size(), 0);
    if (s.text.empty()) {
        base_ = direction == "rtl" ? 1 : 0;
        return;
    }
    const SBCodepointSequence sequence{SBStringEncodingUTF32, s.text.data(), static_cast<SBUInteger>(s.text.size())};
    s.algorithm = SBAlgorithmCreate(&sequence);
    if (s.algorithm == nullptr) throw core::Error("memory", "the bidirectional order could not be found");
    const SBLevel asked = direction == "rtl" ? 1 : direction == "ltr" ? 0 : SBLevelDefaultLTR;
    // (one paragraph: a line break inside — none here, the typesetting splits at them — would end SheenBidi's first)
    s.paragraph = SBAlgorithmCreateParagraph(s.algorithm, 0, static_cast<SBUInteger>(s.text.size()), asked);
    if (s.paragraph == nullptr) throw core::Error("memory", "the bidirectional order could not be found");
    base_ = SBParagraphGetBaseLevel(s.paragraph);
    const SBUInteger length = SBParagraphGetLength(s.paragraph);
    const SBLevel* levels = SBParagraphGetLevelsPtr(s.paragraph);
    for (SBUInteger i = 0; i < length && i < s.text.size(); ++i) levels_[i] = levels[i];
    for (std::size_t i = length; i < s.text.size(); ++i) levels_[i] = base_;
}

BidiParagraph::BidiParagraph() : state_(std::make_unique<State>()) {}
BidiParagraph::~BidiParagraph() = default;
BidiParagraph::BidiParagraph(BidiParagraph&&) noexcept = default;
BidiParagraph& BidiParagraph::operator=(BidiParagraph&&) noexcept = default;

bool BidiParagraph::mixed() const {
    return std::any_of(levels_.begin(), levels_.end(), [](std::uint8_t l) { return (l & 1) != 0; });
}

std::vector<BidiParagraph::Run> BidiParagraph::visual_runs(std::size_t start, std::size_t end) const {
    std::vector<Run> out;
    end = std::min(end, levels_.size());
    if (start >= end) return out;
    const State& s = *state_;
    if (s.paragraph == nullptr || (!mixed() && base_ == 0)) {
        out.push_back(Run{start, end, base_});
        return out;
    }
    SBLineRef line = SBParagraphCreateLine(s.paragraph, static_cast<SBUInteger>(start), static_cast<SBUInteger>(end - start));
    if (line == nullptr) {  // (a row SheenBidi does not hold: one run at the paragraph's level, never a failure)
        out.push_back(Run{start, end, base_});
        return out;
    }
    const SBUInteger count = SBLineGetRunCount(line);
    const SBRun* runs = SBLineGetRunsPtr(line);
    for (SBUInteger i = 0; i < count; ++i) {
        out.push_back(Run{static_cast<std::size_t>(runs[i].offset), static_cast<std::size_t>(runs[i].offset + runs[i].length), runs[i].level});
    }
    SBLineRelease(line);
    return out;
}

std::vector<std::string> scripts_of(std::u32string_view text) {
    std::vector<std::string> out(text.size(), "Zyyy");
    if (text.empty()) return out;
    const SBCodepointSequence sequence{SBStringEncodingUTF32, text.data(), static_cast<SBUInteger>(text.size())};
    SBScriptLocatorRef locator = SBScriptLocatorCreate();
    if (locator == nullptr) throw core::Error("memory", "the scripts could not be found");
    SBScriptLocatorLoadCodepoints(locator, &sequence);
    const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator);
    while (SBScriptLocatorMoveNext(locator)) {
        const SBUInt32 tag = SBScriptGetUnicodeTag(agent->script);
        if (tag == 0) continue;
        const std::string name{static_cast<char>((tag >> 24) & 0xFF), static_cast<char>((tag >> 16) & 0xFF), static_cast<char>((tag >> 8) & 0xFF),
                               static_cast<char>(tag & 0xFF)};
        for (SBUInteger i = agent->offset; i < agent->offset + agent->length && i < text.size(); ++i) out[i] = name;
    }
    SBScriptLocatorRelease(locator);
    return out;
}

std::string sheenbidi_version() { return SHEENBIDI_VERSION_STRING; }

}  // namespace genko::render::text
