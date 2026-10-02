#include "core/model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <utility>

#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"

namespace genko::core {

namespace {

constexpr std::array<std::string_view, 2> kBindings{"right", "left"};
constexpr std::array<std::string_view, 10> kRoles{"name", "draft", "ink", "bg", "finish",
                                                  "tone", "effect", "frames", "text", "user"};
constexpr std::array<std::string_view, 7> kKinds{"raster", "strokes", "fill", "tone", "folder", "placed", "adjust"};

template <class Enum, std::size_t N>
std::optional<Enum> enum_from(const std::array<std::string_view, N>& names, std::string_view text) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == text) return static_cast<Enum>(i);
    }
    return std::nullopt;
}

PageSpec make_spec(Num w, Num h, Num dpi, Num bleed, Num inner, std::string expression, std::string preset) {
    PageSpec spec;
    spec.width_mm = w;
    spec.height_mm = h;
    spec.dpi = dpi;
    spec.bleed_mm = bleed;
    spec.inner_margin_mm = inner;
    spec.expression = std::move(expression);
    spec.preset = std::move(preset);
    return spec;
}

PageSpec b4() { return PageSpec::b4_comic(); }
PageSpec b5() { return PageSpec::b5_doujin(); }
PageSpec a5() { return PageSpec::a5_doujin(); }
PageSpec a4() { return PageSpec::a4_mono(); }
PageSpec webtoon_spec() { return PageSpec::webtoon(); }

constexpr std::array<PaperPreset, 5> kPaperPresets{{
    {"b4", "B4 商業誌・投稿（仕上がり 220×310・基本枠 180×270・600 dpi）", &b4},
    {"b5", "B5 同人誌（原寸・仕上がり 182×257・基本枠 150×220）", &b5},
    {"a5", "A5 同人誌（原寸・仕上がり 148×210・基本枠 120×180）", &a5},
    {"a4", "A4 練習用（紙全体がページ）", &a4},
    {"webtoon", "縦読み・カラー（Webtoon、幅 80 mm）", &webtoon_spec},
}};

const Frame* find_in(const Frame& node, std::string_view frame_id) {
    if (node.id == frame_id) return &node;
    for (const Frame& child : node.children) {
        if (const Frame* found = find_in(child, frame_id)) return found;
    }
    return nullptr;
}

const Frame* parent_in(const Frame& node, std::string_view frame_id) {
    for (const Frame& child : node.children) {
        if (child.id == frame_id) return &node;
        if (const Frame* found = parent_in(child, frame_id)) return found;
    }
    return nullptr;
}

void walk_leaves(const Frame& frame, bool right_bound, std::vector<const Frame*>& out) {
    if (frame.children.empty()) {
        out.push_back(&frame);
        return;
    }
    std::vector<const Frame*> children;
    children.reserve(frame.children.size());
    for (const Frame& child : frame.children) children.push_back(&child);
    // by the middle of each child (slanted panels' boxes overlap): right to left, top to bottom
    if (frame.split_axis && *frame.split_axis == "free") {
        children = reading_order(std::move(children), right_bound);
    } else {
        const bool vertical = frame.split_axis && *frame.split_axis == "vertical";
        std::vector<std::pair<double, const Frame*>> keyed;
        keyed.reserve(children.size());
        for (const Frame* child : children) {
            const auto pts = shape(*child);
            const auto [cx, cy] = centroid(pts);
            keyed.emplace_back(vertical ? -cx : cy, child);
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < keyed.size(); ++i) children[i] = keyed[i].second;
    }
    for (const Frame* child : children) walk_leaves(*child, right_bound, out);
}

std::string ascii_lower_strip(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

std::string_view to_string(Binding binding) { return kBindings[static_cast<std::size_t>(binding)]; }
std::string_view to_string(LayerRole role) { return kRoles[static_cast<std::size_t>(role)]; }
std::string_view to_string(LayerKind kind) { return kKinds[static_cast<std::size_t>(kind)]; }
std::optional<Binding> binding_from(std::string_view text) { return enum_from<Binding>(kBindings, text); }
std::optional<LayerRole> layer_role_from(std::string_view text) { return enum_from<LayerRole>(kRoles, text); }
std::optional<LayerKind> layer_kind_from(std::string_view text) { return enum_from<LayerKind>(kKinds, text); }

// --- PageSpec -----------------------------------------------------------------------------------------------------

std::pair<Num, Num> PageSpec::trim_size() const {
    if (trim_w_mm && trim_w_mm->truthy() && trim_h_mm && trim_h_mm->truthy()) {
        return {Num(trim_w_mm->value()), Num(trim_h_mm->value())};
    }
    return {width_mm - Num(2) * bleed_mm, height_mm - Num(2) * bleed_mm};
}

std::pair<double, double> PageSpec::trim_origin() const {
    const auto [w, h] = trim_size();
    return {((width_mm - w) / Num(2)).value(), ((height_mm - h) / Num(2)).value()};
}

PageSpec::Margins PageSpec::margins() const {
    Margins m;
    if (margins_mm && !margins_mm->empty()) {
        if (margins_mm->size() != 4) {
            throw Error("format", margins_mm->size() > 4 ? "too many values to unpack (expected 4)"
                                                         : "not enough values to unpack (expected 4, got " +
                                                               std::to_string(margins_mm->size()) + ")");
        }
        m.top = (*margins_mm)[0].value();
        m.bottom = (*margins_mm)[1].value();
        m.inner = (*margins_mm)[2].value();
        m.outer = (*margins_mm)[3].value();
    } else {
        m.top = m.bottom = m.inner = m.outer = inner_margin_mm.value();
    }
    return m;
}

std::pair<double, double> PageSpec::frame_size() const {
    const auto [w, h] = trim_size();
    const Margins m = margins();
    return {(w - Num(m.inner) - Num(m.outer)).value(), (h - Num(m.top) - Num(m.bottom)).value()};
}

std::string PageSpec::describe() const {
    const auto [w, h] = trim_size();
    const auto [fw, fh] = frame_size();
    return "用紙 " + py_format_g(width_mm.value()) + "×" + py_format_g(height_mm.value()) + " mm ・ 仕上がり " +
           py_format_g(w.value()) + "×" + py_format_g(h.value()) + " mm ・ 裁ち落とし " +
           py_format_g(bleed_mm.value()) + " mm ・ 基本枠 " + py_format_g(fw) + "×" + py_format_g(fh) + " mm ・ " +
           dpi.repr() + " dpi";
}

PageSpec PageSpec::b4_comic() {
    PageSpec spec = make_spec(257, 364, 600, 5, 20, "mono", "commercial-b4");
    spec.trim_w_mm = Num(220);
    spec.trim_h_mm = Num(310);
    spec.margins_mm = NumList{20, 20, 20, 20};
    return spec;
}

PageSpec PageSpec::b5_doujin() {
    PageSpec spec = make_spec(208, 283, 600, 3, 16, "mono", "doujin-b5");
    spec.trim_w_mm = Num(182);
    spec.trim_h_mm = Num(257);
    spec.margins_mm = NumList{18.5, 18.5, 16, 16};
    return spec;
}

PageSpec PageSpec::a5_doujin() {
    PageSpec spec = make_spec(174, 236, 600, 3, 14, "mono", "doujin-a5");
    spec.trim_w_mm = Num(148);
    spec.trim_h_mm = Num(210);
    spec.margins_mm = NumList{15, 15, 14, 14};
    return spec;
}

PageSpec PageSpec::a4_mono() { return make_spec(210, 297, 600, 3, 10, "mono", "a4-mono"); }

PageSpec PageSpec::webtoon() { return make_spec(80, 400, 300, 0, 4, "color", "webtoon"); }

PageSpec PageSpec::custom(double paper_w, double paper_h, double trim_w, double trim_h, double bleed, double top,
                          double bottom, double inner, double outer, std::int64_t dpi, std::string expression) {
    if (trim_w + 2 * bleed > paper_w + 1e-6 || trim_h + 2 * bleed > paper_h + 1e-6) {
        throw Error("value", "the paper must hold the finished size and its bleed");
    }
    if (inner + outer >= trim_w || top + bottom >= trim_h) {
        throw Error("value", "the basic frame must fit inside the finished size");
    }
    if (std::min({trim_w, trim_h, paper_w, paper_h}) <= 0 || std::min({bleed, top, bottom, inner, outer}) < 0) {
        throw Error("value", "sizes must be positive");
    }
    PageSpec spec = make_spec(paper_w, paper_h, dpi, bleed, std::min({top, bottom, inner, outer}),
                              std::move(expression), "custom");
    spec.trim_w_mm = Num(trim_w);
    spec.trim_h_mm = Num(trim_h);
    spec.margins_mm = NumList{top, bottom, inner, outer};
    return spec;
}

PageSpec PageSpec::publisher(std::string_view name) {
    const std::string key = ascii_lower_strip(name);
    PageSpec spec = b4_comic();
    const bool known = key == "shueisha" || key == "kodansha" || key == "kadokawa" || key == "shogakukan";
    spec.preset = known ? key : "none";
    return spec;
}

std::span<const PaperPreset> paper_presets() { return kPaperPresets; }

const PaperPreset* find_paper_preset(std::string_view key) {
    for (const auto& preset : kPaperPresets) {
        if (preset.key == key) return &preset;
    }
    return nullptr;
}

// --- strokes and pictures -------------------------------------------------------------------------------------

const StrokeListPtr& empty_strokes() {
    static const StrokeListPtr empty = std::make_shared<const StrokeList>();
    return empty;
}

StrokeListPtr make_strokes(std::vector<StrokePtr> items, std::string blob_ref) {
    if (items.empty() && blob_ref.empty()) return empty_strokes();
    return std::make_shared<const StrokeList>(StrokeList{std::move(items), std::move(blob_ref)});
}

const std::string* BlobMemo::ref_for(const Bytes& bytes) const {
    if (ref.empty() || !bytes) return nullptr;
    // (owner comparison: a freed and reused address cannot match while `of` still refers to the old block)
    if (of.owner_before(bytes) || bytes.owner_before(of)) return nullptr;
    return &ref;
}

void BlobMemo::remember(const Bytes& bytes, std::string ref_value) {
    ref = std::move(ref_value);
    of = bytes;
}

// --- Page -----------------------------------------------------------------------------------------------------------

Rect Page::paper_rect_mm() const { return Rect{Num(0.0), Num(0.0), Num(spec.width_mm.value()), Num(spec.height_mm.value())}; }

Rect Page::trim_rect_mm() const {
    const auto [x, y] = spec.trim_origin();
    const auto [w, h] = spec.trim_size();
    return Rect{Num(x), Num(y), w, h};
}

Rect Page::bleed_rect_mm() const {
    const Rect t = trim_rect_mm();
    const Num b(spec.bleed_mm.value());
    return Rect{t.x - b, t.y - b, t.width + Num(2) * b, t.height + Num(2) * b};
}

Num Page::spread_step_mm() const { return trim_rect_mm().width; }

std::string Page::side(std::string_view start_side) const {
    const std::string first =
        !start_side.empty() ? std::string(start_side) : std::string(binding == Binding::Right ? "left" : "right");
    if (py_mod(index, Num(2)) == Num(1)) return first;
    return first == "left" ? "right" : "left";
}

std::string Page::binding_edge(std::string_view start_side) const {
    return side(start_side) == "left" ? "right" : "left";
}

Rect Page::inner_rect_mm(std::string_view start_side) const {
    const Rect t = trim_rect_mm();
    const PageSpec::Margins m = spec.margins();
    const bool left_bound = binding_edge(start_side) == "left";
    const double left = left_bound ? m.inner : m.outer;
    const double right = left_bound ? m.outer : m.inner;
    return Rect{t.x + Num(left), t.y + Num(m.top), t.width - Num(left) - Num(right),
                t.height - Num(m.top) - Num(m.bottom)};
}

bool Page::is_recto() const { return py_mod(index, Num(2)) == Num(1); }

std::vector<const Frame*> Page::leaf_frames() const {
    std::vector<const Frame*> out;
    if (frames.empty()) return out;
    walk_leaves(frames.front(), binding == Binding::Right, out);
    return out;
}

const Frame* Page::find_frame(std::string_view frame_id) const {
    if (frames.empty()) return nullptr;
    return find_in(frames.front(), frame_id);
}

Frame* Page::find_frame(std::string_view frame_id) {
    return const_cast<Frame*>(std::as_const(*this).find_frame(frame_id));
}

const Frame* Page::frame_at(const Num& x_mm, const Num& y_mm) const {
    for (const Frame* frame : leaf_frames()) {
        if (contains(*frame, x_mm, y_mm)) return frame;
    }
    return nullptr;
}

const Frame* Page::parent_of(std::string_view frame_id) const {
    if (frames.empty()) return nullptr;
    return parent_in(frames.front(), frame_id);
}

Frame* Page::parent_of(std::string_view frame_id) {
    return const_cast<Frame*>(std::as_const(*this).parent_of(frame_id));
}

std::pair<Frame*, Frame*> Page::split_frame(std::string_view frame_id, std::string_view axis, const Num& ratio,
                                            const Num& gutter_mm) {
    Frame* target = find_frame(frame_id);
    if (target == nullptr) throw Error("key", "no frame " + std::string(frame_id));
    if (!target->children.empty()) throw Error("value", "can only split a leaf frame");
    const Rect rect = target->rect;
    Frame a, b;
    if (axis == "horizontal") {
        const Num available = rect.height - gutter_mm;
        const Num first_span = available * ratio;
        const Num second_span = available - first_span;
        a.id = new_id();
        a.rect = Rect{rect.x, rect.y, rect.width, first_span};
        b.id = new_id();
        b.rect = Rect{rect.x, rect.y + first_span + gutter_mm, rect.width, second_span};
    } else if (axis == "vertical") {
        const Num available = rect.width - gutter_mm;
        const Num first_span = available * ratio;
        const Num second_span = available - first_span;
        a.id = new_id();
        a.rect = Rect{rect.x, rect.y, first_span, rect.height};
        b.id = new_id();
        b.rect = Rect{rect.x + first_span + gutter_mm, rect.y, second_span, rect.height};
    } else {
        throw Error("value", std::string(axis));
    }
    target->children = {std::move(a), std::move(b)};
    target->split_axis = std::string(axis);
    return {&target->children[0], &target->children[1]};
}

Frame& Page::merge_frame(std::string_view frame_id) {
    Frame* parent = parent_of(frame_id);
    if (parent == nullptr) throw Error("value", "cannot merge the root frame");
    parent->children.clear();
    parent->split_axis.reset();
    return *parent;
}

Frame& Page::resize_frame(std::string_view frame_id, const Rect& rect) {
    Frame* target = find_frame(frame_id);
    if (target == nullptr) throw Error("key", "no frame " + std::string(frame_id));
    if (!target->children.empty()) throw Error("value", "can only resize a leaf frame");
    target->rect = rect;
    return *target;
}

void Page::paint(LayerRole role, NumList rgb) {
    auto it = std::find_if(fills.begin(), fills.end(), [role](const auto& item) { return item.first == role; });
    if (it != fills.end()) {
        it->second = rgb;
    } else {
        fills.emplace_back(role, rgb);
    }
    if (role == LayerRole::Name || role == LayerRole::Draft) {
        Layer& layer = layer_for(role);
        layer.kind = LayerKind::Fill;
        layer.fill_rgb = std::move(rgb);
        layer.exportable = false;
    } else if (role == LayerRole::Ink || role == LayerRole::Bg || role == LayerRole::Finish) {
        Layer& layer = layer_for(role);
        layer.kind = LayerKind::Fill;
        layer.fill_rgb = std::move(rgb);
    }
}

const Layer* Page::first_layer(LayerRole role) const {
    for (const Layer& layer : layers) {
        if (layer.role == role) return &layer;
    }
    return nullptr;
}

Layer& Page::layer_for(LayerRole role) {
    for (Layer& layer : layers) {
        if (layer.role == role) return layer;
    }
    Layer layer;
    layer.id = new_id();
    layer.role = role;
    layer.kind = LayerKind::Strokes;
    layer.exportable = role != LayerRole::Name && role != LayerRole::Draft;
    layers.push_back(std::move(layer));
    return layers.back();
}

const NumList* Page::fill_of(LayerRole role) const {
    for (const auto& [r, rgb] : fills) {
        if (r == role) return &rgb;
    }
    return nullptr;
}

// --- Document -------------------------------------------------------------------------------------------------------

Page& Document::edit_page(std::size_t i) {
    PagePtr& page = pages.at(i);
    if (page.use_count() > 1) page = std::make_shared<Page>(*page);
    return *page;
}

std::vector<const StoryLine*> Document::story_for_page(const Num& page_index) const {
    std::vector<const StoryLine*> out;
    for (const StoryLine& line : story) {
        if (line.page_index == page_index) out.push_back(&line);
    }
    return out;
}

StoryLine& Document::add_line(const Num& page_index, std::string text, std::string speaker,
                              std::optional<std::string> frame_id, std::string ruby, Num x_mm, Num y_mm, Num w_mm,
                              Num h_mm, std::string balloon, std::optional<Point> tail) {
    StoryLine line;
    line.id = new_id();
    line.page_index = page_index;
    line.text = std::move(text);
    line.speaker = std::move(speaker);
    line.frame_id = std::move(frame_id);
    line.ruby = std::move(ruby);
    line.x_mm = x_mm;
    line.y_mm = y_mm;
    line.w_mm = w_mm;
    line.h_mm = h_mm;
    line.balloon = std::move(balloon);
    line.tail = tail;
    story.push_back(std::move(line));
    return story.back();
}

std::uint64_t new_layer_identity() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// (as json.dumps writes them: ints and floats apart, floats exact)
bool same_json(const Json& a, const Json& b) { return dump_python(a) == dump_python(b); }

template <class T, class Same>
bool same_optional(const std::optional<T>& a, const std::optional<T>& b, Same same) {
    return a.has_value() == b.has_value() && (!a || same(*a, *b));
}

template <class T, class Same>
bool same_items(const std::vector<T>& a, const std::vector<T>& b, Same same) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a[i], b[i])) return false;
    }
    return true;
}

bool same_bytes(const Bytes& a, const Bytes& b) { return a == b || (a && b && *a == *b); }
bool same_double(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool same_num(const Num& a, const Num& b) { return a.same(b); }
bool same_point(const Point& a, const Point& b) { return a.x.same(b.x) && a.y.same(b.y); }
bool same_rect(const Rect& a, const Rect& b) {
    return a.x.same(b.x) && a.y.same(b.y) && a.width.same(b.width) && a.height.same(b.height);
}
bool same_patch(const Patch& a, const Patch& b) {
    return same_json(a.attrs, b.attrs) && same_bytes(a.png, b.png) && a.asset == b.asset && same_json(a.after_asset, b.after_asset);
}
bool same_mask(const Mask& a, const Mask& b) { return same_bytes(a.png, b.png) && a.enabled == b.enabled; }

}  // namespace

bool same_layer(const Layer& a, const Layer& b) {
    const auto same_points = [](const std::vector<Point>& x, const std::vector<Point>& y) { return same_items(x, y, same_point); };
    const auto same_nums = [](const NumList& x, const NumList& y) { return same_items(x, y, same_num); };
    return a.id == b.id && a.role == b.role && a.kind == b.kind && a.visible == b.visible && a.exportable == b.exportable &&
           a.strokes == b.strokes && a.raster_relpath == b.raster_relpath && same_optional(a.fill_rgb, b.fill_rgb, same_nums) &&
           same_bytes(a.raster_png, b.raster_png) && same_optional(a.lpi, b.lpi, same_num) &&
           same_optional(a.density, b.density, same_num) && same_optional(a.region, b.region, same_points) &&
           same_double(a.opacity, b.opacity) && a.material_id == b.material_id && same_double(a.angle, b.angle) &&
           a.title == b.title && a.blend == b.blend && a.clip == b.clip && a.lock_alpha == b.lock_alpha &&
           a.locked == b.locked && a.panel_clip == b.panel_clip && a.panel_each == b.panel_each &&
           same_items(a.patches, b.patches, same_patch) && same_optional(a.tone, b.tone, same_json) &&
           same_json(a.parent_id, b.parent_id) && a.asset == b.asset && a.frame_id == b.frame_id &&
           same_optional(a.placement_mm, b.placement_mm, same_rect) && a.fit == b.fit && a.clip_to == b.clip_to &&
           same_optional(a.source, b.source, same_json) && same_optional(a.finish, b.finish, same_json) &&
           same_optional(a.mask, b.mask, same_mask) && a.color == b.color && a.reference == b.reference &&
           same_optional(a.fill, b.fill, same_json) && same_optional(a.adjust, b.adjust, same_json) &&
           same_optional(a.effect, b.effect, same_json) && a.color_prints == b.color_prints &&
           same_optional(a.screen, b.screen, same_json);
}

std::vector<Layer> default_layers() {
    std::vector<Layer> out(4);
    out[0].id = new_id();
    out[0].role = LayerRole::Bg;
    out[0].kind = LayerKind::Fill;
    out[0].exportable = true;
    out[1].id = new_id();
    out[1].role = LayerRole::Name;
    out[1].kind = LayerKind::Strokes;
    out[1].exportable = false;
    out[2].id = new_id();
    out[2].role = LayerRole::Ink;
    out[2].kind = LayerKind::Strokes;
    out[2].exportable = true;
    out[2].panel_each = true;
    out[3].id = new_id();
    out[3].role = LayerRole::Finish;
    out[3].kind = LayerKind::Strokes;
    out[3].exportable = true;
    return out;
}

Page make_page(const Num& index, const PageSpec& spec, Binding binding) {
    Page page;
    page.index = index;
    page.spec = spec;
    page.binding = binding;
    page.id = new_page_id();         // (the id default comes first, then __post_init__'s layers)
    page.layers = default_layers();
    return page;
}

Document new_episode(std::string title, const Num& episode, int page_count, const PageSpec& spec, Binding binding) {
    Document doc;
    doc.title = std::move(title);
    doc.episode = episode;
    doc.spec = spec;
    doc.binding = binding;
    for (int index = 1; index <= page_count; ++index) {
        Page page = make_page(Num(index), spec, binding);
        Frame root;
        root.id = new_id();
        root.rect = page.inner_rect_mm();
        page.frames.push_back(std::move(root));
        doc.pages.push_back(std::make_shared<Page>(std::move(page)));
    }
    doc.book_id = new_book_id();
    return doc;
}

}  // namespace genko::core
