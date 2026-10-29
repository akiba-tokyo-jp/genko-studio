#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "core/annotations.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): Photoshop's 注釈 on a page — PhotoCraft v0.2.0's crates/engine/src/
// notes_cmds.rs (notes.add, notes.set, notes.delete; MIT: native/third_party/photocraft/NOTICE.md) — a note placed at a
// point of the page (mm), with its text, its author, its colour, whether its box is open, when it was changed last and who
// wrote it (the op's actor).
//
// The ops (the C++ build's own: not in `genko schema` or the MCP tool list, applied through `apply` — the window,
// `genko apply`, HTTP; not the MCP apply_ops: service::agent_op does not list them, as PC3's and PC4's own ops and J-a's):
//   add_annotation {page, at_mm: [x, y], text?, author?, color?: [r, g, b] | "#rrggbb", open? (true), popup_mm?: [x, y, w, h],
//                   modified?: "YYYY-MM-DDThh:mm:ssZ" | ""} → {annotation: id}                          注釈を追加
//   set_annotation {page, id, text?, author?, color?, open?, at_mm?, popup_mm?: [x, y, w, h] | null} → {annotation, changed}
//                   (moving it moves its box along; a change of its text, author or colour is its new `modified`, and the
//                   note becomes the writer's; a person's change of any kind makes it the person's)  注釈を編集・移動
//   delete_annotations {page, ids: [id…]} | {page, all: true} → {deleted}                               注釈を削除・すべてを消去
// An agent (an actor that cannot approve: ai:…) never changes or deletes a note a person wrote (human:…, the unnamed
// "genko"): the op is refused, naming the note — as a person's regions are a person's (core/ops_studio). A person changes
// any. `modified` given to add_annotation is the import's (ファイル → 読み込み → 注釈…: a file's notes keep their dates);
// otherwise each change takes the time now (UTC).

namespace genko::render::annotations {

// The actor a note keeps of the op's: human:… and ai:… as they are, a person's other names ("human", the unnamed caller)
// "genko", any other name an agent's ("ai:" before it).
std::string writer_of(const core::Actor& actor);
// "#rrggbb" or [r, g, b] (0..255). OpError otherwise.
std::array<int, 3> color_given(const core::Json& value);
// The time now as `modified` holds it (UTC).
std::string now();
// The page's notes as `genko inspect --annotations` and the panel list them: {"annotations": [each note's JSON, with
// "by_person"]}.
core::Json listing(const core::Page& page);
// The default box of a note's text beside its mark (mm): a note without its own (`popup_mm` null).
std::array<double, 4> popup_of(const core::Annotation& note);
// (the review's P1-2) A note read from a file made one the book holds: its mark pulled inside the paper (`w` × `h` mm; its
// box along), its box dropped (the default) when it lies past what a page's places hold, its author cut to 1024 bytes.
void pull_inside(core::Annotation& note, double w_mm, double h_mm);
// UTF-8 cut to at most `most` bytes, by whole characters.
std::string cut_utf8(const std::string& text, std::size_t most);
// (the review's P2-3) Who writes the notes a file brings while one lives on this thread: an op reading a PSD or a .pcraft
// onto a page (import_psd, smart_to_page, smart_place) its actor's writer — an agent's import its own notes —; none: the
// person who opens a file ("genko").
class ReadingActor {
public:
    explicit ReadingActor(const core::Actor& actor);
    ~ReadingActor();
    ReadingActor(const ReadingActor&) = delete;
    ReadingActor& operator=(const ReadingActor&) = delete;
    static std::string now();

private:
    std::string previous_;
};

}  // namespace genko::render::annotations

namespace genko::render {

void register_annotation_ops(core::OpRegistry& registry);

}  // namespace genko::render
