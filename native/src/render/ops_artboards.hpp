#pragma once

#include "core/command_bus.hpp"

// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」, core/artboards.hpp; the user's decision 3: アートボード＝ページ): the
// artboard ops — after PhotoCraft v0.2.0's engine/src/artboard_cmds.rs (146-320, MIT: native/third_party/photocraft/NOTICE.md),
// on Genko's pages:
//   add_artboard {after?, width_px?, height_px?, preset?, name?, background?, rgb?, dpi?} → {page, name, size_px}
//       (レイヤー → 新規 → アートボード…): a new page marked an artboard — its own paper the size in pixels at the book's
//       resolution (a preset's, else the book's paper's), its finished size the paper, no bleed, its basic frame the
//       paper, no panels and no nombre —, one empty paint layer (レイヤー 1) on it; after the page `after` names (0: first),
//       else after the last page that is not a cover (add_page's). Its name アートボード N unless given.
//   artboard_from_layers {page, ids, name?, background?, rgb?, remove? (true)} → {page, name, rect_mm, kept}
//       (グループからのアートボード… and レイヤーからのアートボード…): a new artboard page right after `page`, its paper the box
//       of what the layers show (render/layer_bounds; a folder's: its layers'), the layers (a folder with what it holds)
//       copied onto it where they were on the page (the page cut to the box: crop_page's way — the pixels on their grids,
//       never drawn again), then taken off `page` (delete_layers: the core layers and those locked with すべてをロック
//       stay, said in `kept`) unless remove is false. A folder named alone gives its name; the folder stays on the
//       artboard (Photoshop's group is the artboard).
//   set_artboard {page, name?, preset?, background?, rgb?, width_px?, height_px?, anchor? (top_left), move_contents?
//       (true), origin_px?} → {page, name, size_px} (the Properties' artboard): its size changed as Canvas Size changes it
//       (set_canvas_size, from the anchor; move_contents false: the contents stay at the top left and the board's place
//       on a PSD's canvas moves instead), a preset's size, its name, background, place on a PSD's canvas. A size given
//       by hand drops its preset. {page, artboard: null}: the page an artboard no more (its paper as it is).
// A cover or a page of a spread is never made an artboard (core::artboard_refusal). Not the agent's (MCP agent_op): ops
// that change the pages of the book (the coordinator's decision, as add_page).

namespace genko::render {

void register_artboard_ops(core::OpRegistry& registry);

}  // namespace genko::render
