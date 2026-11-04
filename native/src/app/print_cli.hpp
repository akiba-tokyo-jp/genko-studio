#pragma once

namespace genko::app {

// `genko print` (PC5 I-f, SPEC.md AUTO-01「プリント」): a book's pages on a printer or into a PDF as プリント… prints them
// (app/print_dialog.cpp's print_sheets: the same pictures, places and marks), from the command line — a Qt without a screen
// (offscreen) when none is set, so it runs where there is no display. Read-only: the book is never written or locked.
//   genko print <book> [--pages 1-3,5] [--printer NAME | --pdf FILE] [--copies N] [--area trim|bleed|paper]
//               [--scale fit|actual|<N>%] [--position center|<left>,<top> (mm)] [--paper a4|b5|…|<w>x<h>]
//               [--orientation portrait|landscape] [--spreads] [--marks corner,center,registration,description,labels]
//               [--profile <built-in id | ICC file> [--intent perceptual|relative|saturation|absolute] [--no-bpc]]
//               [--dpi 72..600] [--dry-run]
// stdout: {"ok": true, "dry_run", "pdf"? | "printer"?, "copies", "paper": {"name", "width_mm", "height_mm", "orientation"},
// "printable_mm": [x, y, w, h], "color": {"handling": "printer" | "genko", "profile"?, "intent"?, "bpc"?}, "sheets": [each
// sheet's place (formats::print::Sheet::json) with "sha256" of its picture's RGB rows]}; --dry-run makes the pictures and
// places them but sends nothing (a PDF is not written, a printer not opened). Errors as the command line's: 2 bad usage
// (stderr), 1 the command failed ({"ok": false, "error", "code"}).
int print_command(int argc, char** argv);

}  // namespace genko::app
