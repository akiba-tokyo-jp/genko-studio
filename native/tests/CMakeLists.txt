# Another process that holds a book's project lock (the lock tests and the e2e tests start it).
add_executable(genko_lock_helper support/lock_helper.cpp)
target_link_libraries(genko_lock_helper PRIVATE genko_storage Qt6::Core)
genko_target_flags(genko_lock_helper)
# A stand-in for a person's upscaler program: writes the arguments it got (test_mcp_api_e2e).
add_executable(genko_argv_helper support/argv_helper.cpp)
genko_target_flags(genko_argv_helper)

# Shared test helpers (strict JSON comparison, the Python reference, running the CLI).
add_library(genko_test_support STATIC support/testsupport.cpp)
target_include_directories(genko_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(genko_test_support PUBLIC genko_storage Qt6::Core)
target_compile_definitions(genko_test_support PRIVATE
  GENKO_REPO_ROOT="${GENKO_REPO_ROOT}"
  GENKO_TEST_DATA="${GENKO_SOURCE_ROOT}/tests/data"
  GENKO_CLI="$<TARGET_FILE:genko>"
  GENKO_LOCK_HELPER="$<TARGET_FILE:genko_lock_helper>"
  GENKO_ARGV_HELPER="$<TARGET_FILE:genko_argv_helper>")
genko_target_flags(genko_test_support)
add_dependencies(genko_test_support genko genko_lock_helper genko_argv_helper)

genko_test(test_buildinfo SOURCES unit/test_buildinfo.cpp DEPS genko_api LABELS unit)
# The HTTP server of `genko serve` and `genko mcp --transport streamable-http` (M5③): errors, the head before the body.
genko_test(test_http_server SOURCES unit/test_http_server.cpp DEPS genko_api Qt6::Network LABELS unit)
genko_test(test_mcp_connection SOURCES unit/test_mcp_connection.cpp DEPS genko_api LABELS unit)
genko_test(test_json SOURCES unit/test_json.cpp DEPS genko_test_support LABELS unit)
genko_test(test_pynum SOURCES unit/test_pynum.cpp DEPS genko_test_support LABELS unit)
genko_test(test_model SOURCES unit/test_model.cpp DEPS genko_test_support LABELS unit)
genko_test(test_strokes SOURCES unit/test_strokes.cpp DEPS genko_test_support LABELS unit)
genko_test(test_storage SOURCES unit/test_storage.cpp DEPS genko_test_support LABELS unit)
genko_test(test_lock SOURCES unit/test_lock.cpp DEPS genko_test_support LABELS unit)
genko_test(test_journal SOURCES unit/test_journal.cpp DEPS genko_test_support LABELS unit)
genko_test(test_command_bus SOURCES unit/test_command_bus.cpp DEPS genko_test_support genko_render_ops LABELS unit)
# The studio (M5): its files where this build goes past Python, and its ops' refusals and changes this build alone makes.
genko_test(test_studio_files SOURCES unit/test_studio_files.cpp DEPS genko_test_support genko_studio LABELS unit)
genko_test(test_studio_ops SOURCES unit/test_studio_ops.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_placed SOURCES unit/test_placed.cpp DEPS genko_test_support genko_formats genko_render_ops LABELS unit)
genko_test(test_ops SOURCES unit/test_ops.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_transaction SOURCES unit/test_transaction.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)

# Drawing (M2-R1): Python's random and stroke geometry, Pillow's Image layer and PNG reading, against tables made by
# tools/migration/render_harness.py unit-tables (data/pyref/render_unit_tables.json).
genko_test(test_pyrandom SOURCES unit/test_pyrandom.cpp DEPS genko_test_support LABELS unit)
genko_test(test_stroke_geom SOURCES unit/test_stroke_geom.cpp DEPS genko_test_support LABELS unit)
genko_test(test_image SOURCES unit/test_image.cpp DEPS genko_test_support genko_render LABELS unit)
# Include current Geometry.c with a test-only allocation interceptor; no production fault hook.
genko_test(test_image_oom SOURCES unit/test_image_oom.cpp unit/geometry_failure.c DEPS genko_render pillow_imaging LABELS unit)
# C succeeds first; only then inject C++ vector/metadata allocation failures.
genko_test(test_image_split_oom SOURCES unit/test_image_split_oom.cpp unit/split_failure.c DEPS genko_render pillow_imaging LABELS unit)
if(MSVC)
  set_source_files_properties(unit/geometry_failure.c unit/split_failure.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(unit/geometry_failure.c unit/split_failure.c PROPERTIES COMPILE_OPTIONS "-w;-fwrapv;-ffp-contract=off")
endif()
genko_test(test_png SOURCES unit/test_png.cpp DEPS genko_test_support genko_render genko_formats LABELS unit)
genko_test(test_color_raster SOURCES unit/test_color_raster.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_color_filters SOURCES unit/test_color_filters.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit TIMEOUT 600)
genko_test(test_color_raster_edit SOURCES unit/test_color_raster_edit.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
# (91 merge/flatten/convert cases at u16 and f32: about 460 s under ASan, alone)
genko_test(test_precision_merge_controls SOURCES unit/test_precision_merge_controls.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit TIMEOUT 1200)
genko_test(test_precision_flatten SOURCES unit/test_precision_flatten.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_precision_merge_safety SOURCES unit/test_precision_merge_safety.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_render_page SOURCES unit/test_render_page.cpp DEPS genko_render genko_storage LABELS unit)
target_compile_definitions(test_render_page PRIVATE GENKO_SOURCE_DIR="${GENKO_SOURCE_ROOT}")
# Tones, effect lines and rulers (M3-B).
genko_test(test_rulers SOURCES unit/test_rulers.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_tones_effects SOURCES unit/test_tones_effects.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
# 3D (M3-C): the ops, their refusals, Undo and saving, the pose library, the guides drawn (no Python needed).
genko_test(test_ops_3d SOURCES unit/test_ops_3d.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
# The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) without Python.
genko_test(test_raster_ops SOURCES unit/test_raster_ops.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
genko_test(test_material_stamp SOURCES unit/test_material_stamp.cpp DEPS genko_core genko_storage genko_render_ops LABELS unit)
# 紙質 (BRUSH-01, this build only): the arithmetic against the independent reference (data/paper/samples.json), the
# pictures taken in, define_brush's paper, the full-size reference pages, saved and opened elsewhere, Undo and Redo,
# the print's PDF drawn back by poppler's pdftocairo, the command line (no Python: test_contract_paper has that part).
genko_test(test_paper SOURCES unit/test_paper.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 900)

# color-model@1 (SPEC.md COLOR-03): the built-in ICC profiles, the colour model's JSON, GKIR version 2, the conversions
# between modes and depths, assign_profile and convert_profile, their refusals, without Python.
genko_test(test_color_model SOURCES unit/test_color_model.cpp DEPS genko_test_support genko_render_ops genko_formats ${GENKO_LCMS2}
           LABELS unit TIMEOUT 600)

# インデックスカラー・2 階調 (SPEC.md COLOR-03, PC1d): render/quantize against PhotoCraft's own tests, and its palettes,
# dithers and bitmaps the same bits as PhotoCraft's quantize.rs run on the same pictures.
genko_test(test_quantize SOURCES unit/test_quantize.cpp DEPS genko_test_support genko_render LABELS unit)

# 色の校正・色域外警告・カラー設定 (SPEC.md COLOR-03, PC1b): render/proof against PhotoCraft's own tests, a picture's own
# profile, set_text_gamma and the letters drawn in it, without Python.
genko_test(test_proof SOURCES unit/test_proof.cpp DEPS genko_test_support genko_render_ops genko_storage genko_formats ZLIB::ZLIB LABELS unit TIMEOUT 600)

# color-mode@1 (SPEC.md COLOR-02): CMYK and Lab pages without Python — GKIR, the colour mode, the ink rules, the ink
# canvas's blends, a page drawn in parts, set_color_mode and back, set_inks, the book on disk, Undo, the refusals.
genko_test(test_ink SOURCES unit/test_ink.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 600)
# color-mode@1, step two: the pixel ops of CMYK and Lab pages through their pictures, the eyedropper's inks, a colour
# for chosen inks and the gamut mark.
genko_test(test_ink_edit SOURCES unit/test_ink_edit.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 1200)
# color-mode@1, step three: the filters and the corrections of CMYK and Lab pages in their inks.
genko_test(test_ink_filters SOURCES unit/test_ink_filters.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# channels@1 (SPEC.md COLOR-03): a page's alpha and spot channels without Python — their JSON, the channel ops and their
# refusals, the selection from a channel, the pixel ops on a channel or one colour plate, split and merge back byte for
# byte, Merge Spot Channel against PhotoCraft's formula, the spots printed and the panel's view, the book on disk.
genko_test(test_channels SOURCES unit/test_channels.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 600)
# PC3-0 (SPEC.md COLOR-06, COLOR-07): the selection's algorithms (PhotoCraft's algo/selection.rs), the area kinds and
# modifiers this build adds, masks@1 (settings, the Quick Mask, density and feather on the three ways, a mask lent to a
# pixel op); a path's coverage (vector/src), the vector data's JSON and the book's frames for vector@1, pen-shapes@1,
# transform@1 and page-spec@1; pixels drawn again through a map (algo/src/transform.rs, warp.rs, resample.rs); everything
# on a page moved by one map and the warps (geom/src/warp.rs)
genko_test(test_selection_ext SOURCES unit/test_selection_ext.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 B1 (SPEC.md COLOR-06): choosing by colour (the wand's colour on a layer or the page, a precise page by its values,
# Color Range's samples, families, tones and out of gamut, Grow and Similar), Select › Modify by distance and the op
# quick_mask; the op magic_erase (PhotoCraft's eraser_cmds/tests.rs and algo/src/erase.rs) on 8-bit, 16-bit and CMYK layers.
genko_test(test_select_color SOURCES unit/test_select_color.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
genko_test(test_magic_erase SOURCES unit/test_magic_erase.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 B2 (SPEC.md COLOR-06): a layer's mask applied and made from the transparency, a linked mask moved with its layer,
# outline_area's bands, fill_area's preserve_transparency, paste's center_mm, the masks of a precise or ink page at its
# finest grid — on 8-bit, precise (16, 32 bits) and CMYK pages, without Python.
genko_test(test_masks SOURCES unit/test_masks.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 B4 (SPEC.md COLOR-06「修復」): the fills (PhotoCraft's poisson.rs, inpaint.rs and content_aware.rs tests) and the
# retouching ops (retouch_cmds/tests.rs: Clone Stamp, Healing Brush, Spot Healing, History Brush, Content-Aware Fill) on
# 8-bit, 16-bit, 32-bit, CMYK, Lab and grey layers.
genko_test(test_inpaint SOURCES unit/test_inpaint.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
genko_test(test_retouch SOURCES unit/test_retouch.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
# PC3 B3 (SPEC.md COLOR-06「賢い選択」「選択とマスク」「マッティング」): PhotoCraft's algo/segment tests (the max-flow against a
# reference, GrabCut, Quick Selection, Select Subject, Focus Area, Select Sky; the same bytes again, a stop asked for) and
# algo/matting.rs's (the guided filter, feather, contrast, shift, the tiles, decontaminate, defringe), the op matting on
# 8-bit, 16-bit and CMYK layers, and the smart area kinds on a page (the same area on an 8-bit, a 16-bit and a CMYK page).
genko_test(test_segment SOURCES unit/test_segment.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
genko_test(test_matting SOURCES unit/test_matting.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
genko_test(test_path_raster SOURCES unit/test_path_raster.cpp DEPS genko_test_support genko_render_ops LABELS unit)
# PC3 C-b (pen-shapes@1): the live shapes, the path operations' coverage, a stroke's alignments and dashes, the gradient
# forms, a part baked, cut, erased, bent and transformed; the shape presets' language and settings file.
genko_test(test_shapes SOURCES unit/test_shapes.cpp DEPS genko_test_support genko_render_ops LABELS unit)
# PC4 E-0 (SPEC.md COLOR-08, brushes@1 and patterns@1): the dab brush's settings and a line's inputs, the book's patterns,
# written and read back (a book without them as before; what cannot be opens read-only; gc keeps their pictures); the
# presets' library in the settings folder; a layer's sparse tiles and the sequential-dab stroke (render/tiled_pixels,
# retouch_dabs' apply_dab_stroke) on 8-bit, 16-bit, 32-bit, CMYK, Lab and grey layers.
genko_test(test_brushes_model SOURCES unit/test_brushes_model.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_patterns_model SOURCES unit/test_patterns_model.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_resample SOURCES unit/test_resample.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_page_geometry SOURCES unit/test_page_geometry.cpp DEPS genko_test_support LABELS unit)
# PC3 C-e (SPEC.md COLOR-07「判型」): the page size ops on a book in memory — each thing on a page where the map takes it,
# turns and flips coming back, the finished size and basic frame kept with what the page holds, a page's own paper.
genko_test(test_page_size SOURCES unit/test_page_size.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 C-c (SPEC.md COLOR-07): the transforms' algorithms (PhotoCraft's puppet.rs, perspective.rs, liquify.rs, seam.rs and
# geom/src/warp.rs tests: the identity, rigid moves, the energy, the shared edges, each Liquify tool's sense, the seams),
# the warps' forms and splits, render/resample's triangles and inverse map.
genko_test(test_transform SOURCES unit/test_transform.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 C-d (SPEC.md COLOR-07「移動・整列」): align and distribute by whole working pixels (PhotoCraft's layer_multi_cmds.rs
# tests), the stacking order among siblings with folders, links, snapping's targets (snap.rs' tests).
genko_test(test_align SOURCES unit/test_align.cpp DEPS genko_test_support genko_render_ops LABELS unit)
# PC2-0 (SPEC.md COLOR-04, adjustments@1): the corrections' common ground without Python — Python's form kept, a
# correction's native read and stored, native.method, the kinds' table and what is not in this build yet, one
# AdjustFunction to the three ways, native.blend on 8 and 16 bits and on CMYK, the PSD's frame; and PC2a's tone kinds
# (the upstream formulas, the channel records in their space, auto_adjust, equalize, set_adjustment, color_lookup's
# asset, the PSD's tone codecs, the .cube export).
genko_test(test_adjust SOURCES unit/test_adjust.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 600)
# PC2c (COLOR-04): the local corrections (PhotoCraft's algo/tone.rs tests; filter_raster's shadows_highlights,
# replace_color and match_color on 8-bit, precise and CMYK layers) and hdr_toning's flatten, without Python.
genko_test(test_tone_ops SOURCES unit/test_tone_ops.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC2c (COLOR-04): Apply Image and Calculations (Blending, the sources, a layer, a channel, inks; the three results),
# trapping (algo/trap.rs's tests and the op) and the fade (its mix, the op reading the pixels before from the assets).
genko_test(test_image_calc SOURCES unit/test_image_calc.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC2b: the colour corrections' formulas (PhotoCraft's own tests), their settings read and stored, the three ways, the
# PSD's colour adjustment layers (round trips, hand-built blocks, what is not read).
genko_test(test_adjust_color SOURCES unit/test_adjust_color.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 600)
# PC2a (SPEC.md COLOR-04): color_lookup's 3D tables without Python — the LUT files read as PhotoCraft reads them, the
# generated looks, the lookup and its dither, the table as a book holds it.
genko_test(test_lut3d SOURCES unit/test_lut3d.cpp DEPS genko_core LABELS unit)
# PC3 A0 (SPEC.md「スマートオブジェクト」, smart-objects@1): the model's rules — the matrix, the book written and read back,
# validate_smart's refusals, an op sent into contents, the ops of A0 and the later steps refused by name.
genko_test(test_smart SOURCES unit/test_smart.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 A0: a smart object's file read with every bit (PNG 16, TIFF 16 and 32), a CMYK file's inks kept, what is refused
# (too large, a decompression bomb, broken), its colours by the use kept (keep, embedded, assign).
genko_test(test_smart_decode SOURCES unit/test_smart_decode.cpp DEPS genko_test_support genko_render PNG::PNG TIFF::TIFF JPEG::JPEG ZLIB::ZLIB
           LABELS unit TIMEOUT 600)
# PC3 A0: a smart object drawn — one layer converted draws as it did (8, 16, 32 bits, CMYK, Lab, grey), a placed picture
# converted and a file placed as place_asset places it draw as it did (the monochrome finish too), smaller and back the same,
# a part the same as the whole.
genko_test(test_smart_render SOURCES unit/test_smart_render.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 900)
# PC3 A1 (SPEC.md COLOR-05「中身とリンク」): smart_replace, smart_relink, smart_update, smart_embed, smart_link and
# smart_to_page (the same pixels), a link's record, every page showing the contents asked its lock, nested contents drawn again.
genko_test(test_smart_contents SOURCES unit/test_smart_contents.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# PC3 A2 (SPEC.md COLOR-05「スマートフィルター」): a smart object's smart filters — set again from the contents, their order,
# blend modes, opacities, all off and on, cleared, the selection their mask, following the transforms; at 200 dpi as
# rasterize_layer and filter_raster (8, 16 bits, CMYK, Lab); a part the whole cut there; refused kinds; read back.
genko_test(test_smart_filters SOURCES unit/test_smart_filters.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 2400)
# (ASan with tests run beside it: about 15 minutes, past the 900 s it had; 113 s in Release)
set_property(TEST test_smart_filters APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=1700000")
# PC3 A3 (SPEC.md COLOR-05「PSD のスマートオブジェクト」): a PSD's placed layer blocks and linked files (upstream's
# linked.rs and blocks.rs tests, this build's SoLd and PlLd read back), a PSD or PSB as a smart object's file (16 and 32 bits
# kept, CMYK inks, Lab, a blank merged picture), import_psd's smart objects drawn and written back byte for byte.
genko_test(test_psd_smart SOURCES unit/test_psd_smart.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 600)
# PC3 A4 (SPEC.md COLOR-05「スタックモード」): a smart object's stack mode — PhotoCraft's statistics, the shots combined in
# the three ways (the median taking the passer-by out), a folder, hidden layers, a part the whole cut there, a file one
# layer, rasterized; rasterize_lines drawing the lines as the page drew them; a tone rasterized on precise and ink pages.
genko_test(test_smart_stack SOURCES unit/test_smart_stack.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 1800)
# PC3 A5 (SPEC.md COLOR-05「スマートオブジェクトの上の変形」): a smart object's warp kept in its contents' mm and undone
# without loss (PhotoCraft's smart_object_warp_is_lossless), a new warp in place of the old, a quad its corners, perspective
# and puppet warps and Liquify its smart filters (as the ops bend a paint layer; set again), its filter mask moved with it,
# the three ways and a part the whole cut there, its warp to a PSD and back, what a reader refuses.
genko_test(test_smart_warp SOURCES unit/test_smart_warp.cpp DEPS genko_test_support genko_render_ops genko_formats LABELS unit TIMEOUT 1800)

# core/pyre's own bounds (Python 3.12's re, ported; SPEC COMP-01a): a search that backtracks without end stopped when its
# steps are spent, groups more than 200 deep refused, replace_text refused without a change; without Python.
genko_test(test_pyre SOURCES unit/test_pyre.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 600)
# TEXT-01 (ruby-layout@1): a line's ruby held by place — the table of edits (data/ruby_cases.json), the ops' refusals,
# the feature, the book read back, the official export stopped while a ruby needs review; and drawn — by place as
# Python's ruby_runs when nothing new is set, the ruby font / size / alignment / distance / spacing, PNG and PDF, the F4
# book saved, read back and written out.
genko_test(test_ruby SOURCES unit/test_ruby.cpp DEPS genko_test_support genko_formats genko_render_ops LABELS unit TIMEOUT 600)
genko_test(test_ruby_render SOURCES unit/test_ruby_render.cpp DEPS genko_test_support genko_formats genko_render_ops ZLIB::ZLIB
           LABELS unit TIMEOUT 900)
set_tests_properties(test_ruby_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# PSD-01 (AC-PSD's structure checks with this build's reader, ACCEPTANCE.md §6): the editable PSD — the text structures
# written and read back, the PSD book (data/psd_text_book.json) as groups of balloon, text and ruby layers with their
# text, runs, fonts, sizes, colours, spacing, alignment, transforms and strokes, the strict refusal and the lines drawn
# as pictures when asked, the image way unchanged, `genko export --psd-text`, the support table in the docs.
genko_test(test_psd_text SOURCES unit/test_psd_text.cpp DEPS genko_test_support genko_formats genko_render_ops
           LABELS unit TIMEOUT 900)
set_tests_properties(test_psd_text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Against the Python baseline (skipped when there is no reference Python: $GENKO_PYREF or /opt/pyref/bin/python).
genko_test(test_contract_storage SOURCES contract/test_contract_storage.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)
genko_test(test_contract_cli SOURCES contract/test_contract_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 600)
genko_test(test_contract_save SOURCES contract/test_contract_save.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 1200)
genko_test(test_contract_draw SOURCES contract/test_contract_draw.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
genko_test(test_contract_brushes SOURCES contract/test_contract_brushes.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
genko_test(test_contract_render SOURCES contract/test_contract_render.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 7200)
# (hundreds of pages in one test function: QtTest's 5-minute limit per function is raised; the whole test takes about
# 12 minutes in Debug and 40 under ASan)
set_tests_properties(test_contract_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_render_cli SOURCES contract/test_contract_render_cli.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 1800)
# Tones, effect lines and rulers against Python (M3-B): the ops (fixed cases, random sequences, the command line), the
# drawing, the snapping, and the pen lines and erasers that snap to the rulers (moved from M2-O1's cases).
genko_test(test_contract_tone_ops SOURCES contract/test_contract_tone_ops.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_tone_ops PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_tone_render SOURCES contract/test_contract_tone_render.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 7200)
set_tests_properties(test_contract_tone_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=7000000")
genko_test(test_contract_rulers SOURCES contract/test_contract_rulers.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_render_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
# The ops of M2-O1 against Python's apply_ops (fixed cases, random sequences, saved and read back) and the command line.
genko_test(test_contract_ops SOURCES contract/test_contract_ops.cpp contract/opsupport.cpp DEPS genko_test_support
           genko_render_ops LABELS contract TIMEOUT 3600)
genko_test(test_contract_ops_cli SOURCES contract/test_contract_ops_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 3600)
# The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) against Python's apply_ops: fixed cases, random sequences,
# saved and read back.
genko_test(test_contract_raster_ops SOURCES contract/test_contract_raster_ops.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 7200)
# … the pages they make, drawn (whole and in parts), and the command line.
genko_test(test_contract_raster_render SOURCES contract/test_contract_raster_render.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_raster_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_raster_cli SOURCES contract/test_contract_raster_cli.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 5400)
# core/filters, the correction layers' adjustments for the ops and the renderer, against Python's filters.apply_filter.
genko_test(test_contract_filters SOURCES contract/test_contract_filters.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# render/colour (CMYK, ICC, the screen proof) against Python's genko/colour.py.
genko_test(test_contract_colour SOURCES contract/test_contract_colour.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# 紙質 (BRUSH-01): the inherited brushes' reference page (data/paper/expected/inherited-brushes.png) is Python's own
# drawing of the same lines, and this build's.
genko_test(test_contract_paper SOURCES contract/test_contract_paper.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# render/abr (Photoshop brush files) and the brush library against Python's genko/abr.py and brushes.py.
genko_test(test_contract_abr SOURCES contract/test_contract_abr.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# render/materials (the person's material library, packs) against Python's genko/materials/__init__.py.
genko_test(test_contract_materials SOURCES contract/test_contract_materials.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# The two together: a book made by the ops (C++ and Python), drawn by `genko render` and `python -m genko render`.
genko_test(test_contract_drawn_by_ops SOURCES contract/test_contract_drawn_by_ops.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# Animation (M3③): the ops against Python's apply_ops (anim_cases.json), an animation page drawn as its first frame.
genko_test(test_contract_anim SOURCES contract/test_contract_anim.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# The vector line edits (M3④): vector_edit and trace_edit against Python's apply_ops (vector_cases.json).
genko_test(test_contract_vector SOURCES contract/test_contract_vector.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# core/pyre against Python 3.12's re (tools/migration/pyre_harness.py): every code point's properties and every name
# re reads, the curated cases of contract/pyre_cases.json and 30000 random ones: compile errors, groups, spans, subn,
# findall.
genko_test(test_contract_pyre SOURCES contract/test_contract_pyre.cpp DEPS genko_test_support LABELS contract TIMEOUT 1800)
# The line ops of M4: add_line, edit_line, move_line, delete_line, reorder_lines, cut_balloon, set_balloon_path,
# replace_text (with re's patterns too) and stamp_material's lines (lettering materials, a picture material made a
# balloon) against Python's apply_ops (lines_cases.json).
genko_test(test_contract_lines SOURCES contract/test_contract_lines.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# The book and page ops of M4: set_page_spec, set_spread, add_cover, for_pages, set_assignee and import_pages against
# Python's apply_ops (book_cases.json), with what covers.py makes of the book; `genko apply` with import_pages copying
# no asset file, as Python's.
genko_test(test_contract_book SOURCES contract/test_contract_book.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# The studio ops of M5 (studio_ops.py: the bible, briefs, regions, requests, candidates, adoption, placement, approvals,
# tickets, proposals; set_bible, add_ticket, set_ticket) against Python's apply_ops (studio_cases.json on
# studio_harness.py's studio book), and the same saved batch by batch with Undo and Redo by actor, the approval audit
# compared with Python's.
genko_test(test_contract_studio SOURCES contract/test_contract_studio.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops genko_studio genko_formats ZLIB::ZLIB LABELS contract TIMEOUT 2400)
# The studio service (M5②: service.py's StudioService and HumanService, adopt-drafts, stats, the audit and `genko studio`)
# against Python's: service_cases.json (make_service_cases.py) run here and by service_harness.py, step by step: each reply,
# the books and the other files after it.
genko_test(test_contract_service SOURCES contract/test_contract_service.cpp
           DEPS genko_test_support genko_service genko_api ZLIB::ZLIB LABELS contract TIMEOUT 2400)
# `genko mcp` (stdio, Streamable HTTP) and `genko serve` against Python's, through the MCP SDK's own client and raw
# JSON-RPC / HTTP (tools/migration/mcp_harness.py on contract/mcp_cases.json; skipped without $GENKO_MCP_PYTHON).
genko_test(test_contract_mcp SOURCES contract/test_contract_mcp.cpp DEPS genko_test_support genko_render LABELS contract TIMEOUT 3600)
# F3 (ACCEPTANCE.md §2, M5's exit): tests/test_m4_pipeline.py's 8-page offline making on this build — Python's FakeAgent
# through the MCP SDK's client on `genko mcp`, the person's `genko studio` — against the same run on Python's
# (tools/migration/pipeline_harness.py): every call and reply, the pictures, the book, its audit and approvals; what was
# written out (drawn from random ids: against Python's drawing of this build's book). Skipped without $GENKO_MCP_PYTHON,
# as test_contract_mcp.
genko_test(test_contract_pipeline SOURCES contract/test_contract_pipeline.cpp DEPS genko_test_support genko_render ZLIB::ZLIB
           LABELS contract TIMEOUT 7200)
set_tests_properties(test_contract_pipeline PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=7100000")
# The studio service where this build goes past Python (SPEC COMP-01a): background jobs (and a thread that cannot
# start), a palette picture kept, the official export handed to the studio (holding the book, taking back its files,
# printing the dots asked for), for_pages and put_raster's reads, preview names, pages gone meanwhile, v1–v3 books
# refused (and `genko studio`'s exit 3), one save for several sheets, chat approvals checked under the lock, another
# process's lock, confine kept from callers, the command line's own refusals, PDF numbers whatever the locale.
genko_test(test_service SOURCES unit/test_service.cpp DEPS genko_test_support genko_service LABELS unit)
# M7 (the ledger counted against the C++ tree): the checks of Python's tests no C++ test made before, ported a Python test
# to a slot (tools/migration/build_ledger.py TEST_CPP names them)
genko_test(test_ledger_service_a SOURCES unit/test_ledger_service_a.cpp DEPS genko_test_support genko_service LABELS unit TIMEOUT 900)
genko_test(test_ledger_service_c SOURCES unit/test_ledger_service_c.cpp DEPS genko_test_support genko_service LABELS unit TIMEOUT 900)
genko_test(test_ledger_service_b SOURCES unit/test_ledger_service_b.cpp DEPS genko_test_support genko_service genko_api LABELS unit TIMEOUT 2400)
set_tests_properties(test_ledger_service_b PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=2300000")
# import_psd (M4②b): psd.read_psd and the op against Python's on the PSD and PSB files of psd_cases.json (every file,
# a few cut after every byte, all of them changed at a few bytes; each step's reply, snapshot and payload, the layers'
# pixels, the book read back), and hostile files refused without the memory Python would take.
genko_test(test_contract_psd SOURCES contract/test_contract_psd.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# PSD-01 against psd-tools (ACCEPTANCE.md §6): the editable PSD book read by psd-tools (tools/migration/psd_text_check.py)
# and by render::psd::File, the same layers, groups, texts, runs, fonts, transforms and strokes.
# color-mode@1 (SPEC.md COLOR-02) against the reference Python (tools/migration/ink_check.py): the conversions as Pillow's
# ImageCms makes them, the files of CMYK and Lab pages as Pillow, psd-tools and pdfimages read them, a Lab PSD.
genko_test(test_contract_ink SOURCES contract/test_contract_ink.cpp DEPS genko_test_support genko_formats genko_render_ops
           LABELS contract TIMEOUT 900)
# color-model@1 (COLOR-03) through the command line: apply, save, undo and redo, render, inspect --pixel, export.
genko_test(test_contract_color_model SOURCES contract/test_contract_color_model.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# PC2b (COLOR-04) through the command line: the colour corrections applied, saved, undone and redone, drawn, written as
# a PSD's adjustment layers (psd-tools reads them) and read back.
genko_test(test_contract_adjust_color SOURCES contract/test_contract_adjust_color.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# PC2a (COLOR-04, adjustments@1) through the command line: every kind of page corrected (apply), asked, drawn, saved, undone
# and done again; a 3D LUT's asset kept after Undo and gc; the .cube export; the PSD's adjustment layers read back by
# psd-tools (tools/migration/psd_adjust_check.py) and by import_psd.
genko_test(test_contract_adjust SOURCES contract/test_contract_adjust.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 1800)
# (its command line over every kind of page under ASan takes longer than QtTest's 5 minutes for one function)
set_property(TEST test_contract_adjust APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=1700000")
# PC2d (COLOR-04) through the command line: put_raster and import_psd with embedded_profile, inspect --profile.
genko_test(test_contract_profile SOURCES contract/test_contract_profile.cpp DEPS genko_test_support genko_render ZLIB::ZLIB
           LABELS contract TIMEOUT 900)
# channels@1 (COLOR-03) through the command line: apply, save, gc, undo and redo, a selection and a fill on a channel,
# render, inspect, export (the PSD's extra channels read back, the PDF's separation, the notes).
genko_test(test_contract_channels SOURCES contract/test_contract_channels.cpp DEPS genko_test_support genko_render ZLIB::ZLIB
           LABELS contract TIMEOUT 900)
# PC2c (COLOR-04) through the command line: a local correction saved, undone and done again; fade_layer from the
# pixels before (an asset gc keeps); apply_image and calculations; hdr_toning's flatten; trap on a CMYK page only.
# PC3 A0 (smart-objects@1) through the command line: converted, saved, opened again, undone and redone, gc keeping the
# contents' files, the journal; a 16-bit file drawn with its bits; import_pages bringing contents; panels and checks.
genko_test(test_contract_smart SOURCES contract/test_contract_smart.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 A3 through the command line: Genko's smart objects written as placed layers that psd-tools (GENKO_PYREF) and this build
# read (ids, embedded files, boxes; a page's contents a PSD), a PSD's placed layer imported as a smart object, drawn,
# undone and redone, drawn from its file once changed, written back with the block and item it was read with.
genko_test(test_contract_psd_smart SOURCES contract/test_contract_psd_smart.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 A1 through the command line: a linked file placed, `genko smart --links` (ok, modified, missing), updated, relinked,
# embedded, drawn from its copy when the file is gone; contents written out (a page's PSD read by psd-tools); `genko package`.
genko_test(test_contract_smart_links SOURCES contract/test_contract_smart_links.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 B1 through the command line: inspect --area, the colour kinds given to ops, quick_mask in and out (saved, undone,
# done again), the Quick Mask in a PSD (resource 1022, read back by psd-tools and this build's reader), magic_erase.
genko_test(test_contract_select SOURCES contract/test_contract_select.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# PC3 C-a (vector@1): a page's paths and a layer's vector mask — set, saved, the path kinds of an area, fill (its blend),
# stroke, work paths from an area and from letters, the vector mask drawn on every kind of page, rasterized, merged;
# through the command line saved, undone, inspect --paths, export --format ai; a PSD's paths and vmsk (psd-tools, import_psd).
genko_test(test_contract_paths SOURCES contract/test_contract_paths.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 B2 (masks@1) through the command line: a mask's link, density and feather saved, drawn, undone and redone;
# apply_layer_mask, mask_from_transparency, outline_area, a copy pasted at a point; a PSD's masks written with Photoshop's
# flags and parameters that psd-tools (GENKO_PYREF) and this build read, imported back with them.
genko_test(test_contract_masks SOURCES contract/test_contract_masks.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 B4 through the command line: the retouching ops saved, undone and done again, history_brush's `from` (an asset the
# collection keeps while the journal names it), content_aware_fill on the layer, a new layer and a copy.
genko_test(test_contract_retouch SOURCES contract/test_contract_retouch.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# PC3 C-b (pen-shapes@1): a pen layer's shape parts — added, edited, combined, moved, made patches and lines; drawn in their
# place on every kind of page; the selection, the eraser, ゆがみ, merges and the bucket with them; a fill layer's new
# gradients; through the command line saved and undone (a pen layer without parts as before), `genko shapes`; the PSD's
# shape layers and groups (psd-tools, import_psd).
genko_test(test_contract_shapes SOURCES contract/test_contract_shapes.cpp DEPS genko_test_support genko_render_ops genko_formats
           LABELS contract TIMEOUT 900)
# PC3 C-c: transform_layers on each kind of layer and pixels (exact moves and turns on 8, 16, 32 bits, CMYK, Lab; lines,
# patches, quads, warps, perspective, puppet; folders, links, masks, areas, parts; smart objects, placed pictures, locks,
# lines of dialogue), content_aware_scale, liquify's strokes, Warp Text; through the command line saved and undone.
genko_test(test_contract_transform SOURCES contract/test_contract_transform.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 900)
# PC3 C-d: align_layers and distribute_layers by each kind of layer's box (pen lines and shape parts, paint, placed,
# tone, folders) and the lines of dialogue, the Genko bases (trim, basic frame, panel), the locks refused; arrange_layers
# in a folder; link_layers and the moves of linked layers; set_layer's lock_position; through the command line saved and
# undone, `genko inspect --at`.
genko_test(test_contract_align SOURCES contract/test_contract_align.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 900)
genko_test(test_contract_image_ops SOURCES contract/test_contract_image_ops.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# PC3 C-e through the command line: every kind of page turned (inspect --pixel), undone and redone; a page of its own paper
# (page-spec@1) written, read again, exported as PDF and PSD at its size; Image Size, Canvas Size's colour; set_page_spec
# moving a precise layer's pixels.
genko_test(test_contract_page_size SOURCES contract/test_contract_page_size.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 1800)

genko_test(test_contract_psd_text SOURCES contract/test_contract_psd_text.cpp DEPS genko_test_support genko_formats genko_render_ops
           LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_psd_text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# TIFF and WebP read as the Python baseline's Pillow reads them (M6): every case made by the reference Pillow, the
# same mode, size and bytes; a TIFF in planes said not yet read.
genko_test(test_contract_tiff_webp SOURCES contract/test_contract_tiff_webp.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 900)
# The text of a line (M4): tategaki and the text side of balloons (cells, kinsoku, glyphs, ruby, 傍点, the letters set
# across and down, their effects, text on a path) against Python's (render_harness.py text-cases), byte for byte (a
# warped line's pictures within ARCHITECTURE.md §9: Python's homography comes from numpy's SVD).
genko_test(test_contract_text SOURCES contract/test_contract_text.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
# Balloons (M4): the shapes, tails and joined balloons of balloons.py against Python's (render_harness.py
# balloon-cases): the geometry bit for bit, draw_lines's pictures byte for byte, whole and drawn in parts.
genko_test(test_contract_balloons SOURCES contract/test_contract_balloons.cpp
           DEPS genko_test_support genko_render LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_balloons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
# The paint ops of M3④: add_shape, smudge and liquify against Python's apply_ops (paint_cases.json).
genko_test(test_contract_paint SOURCES contract/test_contract_paint.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
# The exports (M4③a, formats/): every format of export.py, profiles.py, pack.py and psd.py's writer, and
# app/exporting.run, against Python's on the same books (render_harness.py make-export-books, export-cases): what each
# call returns or raises, and every file it writes, byte for byte (export_files.hpp leaves out only the times Python's
# files carry: the sRGB profile's date, an EPUB's zip times and dcterms:modified); the encoders alone against Pillow's
# (save-cases).
genko_test(test_contract_export SOURCES contract/test_contract_export.cpp DEPS genko_test_support genko_formats ZLIB::ZLIB
           LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_export PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
# `genko export` against `python -m genko export`: every format and option, the output, the files, the refusals and
# exit codes, a moving picture (animation, timelapse) as Pillow plays it.
genko_test(test_contract_export_cli SOURCES contract/test_contract_export_cli.cpp DEPS genko_test_support genko_render ZLIB::ZLIB
           LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_export_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
# 入稿前の点検 (M4③b, formats/checks): checks.book (and the studio preflight it folds in) against Python's on chosen
# books that reach every check and random drawing books (render_harness.py check-cases), each report byte for byte.
genko_test(test_contract_checks SOURCES contract/test_contract_checks.cpp DEPS genko_test_support genko_formats
           LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_checks PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
# Filter plugins (M3③, COMP-04): the runner against Python's plugins.run, the choice, the bounds.
genko_test(test_contract_plugins SOURCES contract/test_contract_plugins.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 1800)
# Native filter plugins (COMP-04's native extension, this build only: docs/PLUGIN_NATIVE.md) with a small program of the
# test tree as the plugin: listed without running, chosen with consent, a picture filtered, and the refusals (plugin.json
# that cannot be used, a missing program, a crash, too much written back, too slow); without Python.
add_executable(genko_test_native_plugin support/native_test_plugin.cpp)
genko_target_flags(genko_test_native_plugin)
genko_test(test_native_plugins SOURCES unit/test_native_plugins.cpp DEPS genko_render_ops LABELS unit TIMEOUT 900)
target_compile_definitions(test_native_plugins PRIVATE GENKO_TEST_NATIVE_PLUGIN="$<TARGET_FILE:genko_test_native_plugin>")
add_dependencies(test_native_plugins genko_test_native_plugin)

# 3D (M3-C): the geometry of figures, guides and models bit for bit against Python, the ops, the drawing.
genko_test(test_contract_3d_geometry SOURCES contract/test_contract_3d_geometry.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_3d_geometry PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_3d_ops SOURCES contract/test_contract_3d_ops.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_3d_ops PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_3d_random SOURCES contract/test_contract_3d_random.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_3d_random PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_3d_render SOURCES contract/test_contract_3d_render.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 5400)
set_tests_properties(test_contract_3d_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=5000000")
genko_test(test_contract_3d_cli SOURCES contract/test_contract_3d_cli.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 1800)
set_tests_properties(test_contract_3d_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")

# The milestones together (M3-I): random books carrying tones, effect lines, rulers and 3D, and random op sequences
# mixing their ops with M2's (add_stroke, add_layer, split_frame, …), given to `genko apply` and `python -m genko apply`:
# each step's reply, the book at the end (`inspect --full`) and every page drawn by `genko render`.
genko_test(test_contract_m3_mixed SOURCES contract/test_contract_m3_mixed.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 5400)
set_tests_properties(test_contract_m3_mixed PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=5000000")

genko_test(test_cli_e2e SOURCES e2e/test_cli_e2e.cpp DEPS genko_test_support LABELS e2e)
genko_test(test_save_e2e SOURCES e2e/test_save_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
genko_test(test_migrate_e2e SOURCES e2e/test_migrate_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
genko_test(test_ops_e2e SOURCES e2e/test_ops_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
# `genko mcp` and `genko serve` where an agent must not get through and where this build draws its own lines (M5③).
genko_test(test_mcp_api_e2e SOURCES e2e/test_mcp_api_e2e.cpp DEPS genko_test_support genko_render Qt6::Network LABELS e2e TIMEOUT 1800)

# The desktop app (M2-G1): the editing session alone, then the windows on the offscreen platform (label gui).
if(GENKO_BUILD_APP)
  genko_test(test_gui_materials SOURCES gui/test_gui_materials.cpp DEPS genko_app GUI TIMEOUT 300)
  # the disposable material preview cache alone (keys, restarts, corrupt files, budgets, originals kept)
  genko_test(test_material_preview_cache SOURCES unit/test_material_preview_cache.cpp DEPS genko_app LABELS unit)
  genko_test(test_migration SOURCES unit/test_migration.cpp DEPS genko_app LABELS unit)
  # (about 60 s in Release; under ASan about 270 s, close to the 300 it had: a guard against a hang, as the GUI tests near it)
  genko_test(test_gui_color SOURCES gui/test_gui_color.cpp DEPS genko_app ZLIB::ZLIB GUI TIMEOUT 1800)
  # the layer panel: its list, buttons, settings, menus (several at once, masks, effects), fill and correction layers,
  # a filter with its result on the page while its numbers change
  genko_test(test_gui_layers SOURCES gui/test_gui_layers.cpp DEPS genko_app GUI TIMEOUT 600)
  # PC2b: レイヤー → 色調補正 の色の補正 (their windows with the result shown, the keys), the forms, the correction layers
  genko_test(test_gui_adjust_color SOURCES gui/test_gui_adjust_color.cpp DEPS genko_app GUI TIMEOUT 600)
  # 範囲選択: the marquee's ways with Shift / Alt, the selection's commands, the chosen part moved, copied, pasted
  genko_test(test_gui_select SOURCES gui/test_gui_select.cpp DEPS genko_app GUI TIMEOUT 600)
  # PC3 B2: 編集's clipboard with the system's, 結合部分をコピー, 特殊ペースト, 選択範囲をコピー／カットしたレイヤー,
  # 塗りつぶし… and 境界線を描く…, a precise page's copy as precise pixels
  genko_test(test_gui_clipboard SOURCES gui/test_gui_clipboard.cpp DEPS genko_app GUI TIMEOUT 600)
  # PC3 C-c: 自由変形 (the box previewed, Enter one op, Esc, 再実行), the turns, the warp's splits, puppet pins, ゆがみ…,
  # コンテンツに応じて拡大・縮小…, ワープテキスト… and the commands' names
  genko_test(test_gui_transform SOURCES gui/test_gui_transform.cpp DEPS genko_app GUI TIMEOUT 600)
  # PC3 B3: クイック選択ツール・オブジェクト選択ツール, 被写体を選択・空を選択・フォーカス領域…, 選択とマスク… (its preview, its 4
  # outputs, each one Undo), すべてのオブジェクトをマスク, マッティング, the worker thread stopped
  genko_test(test_gui_select_mask SOURCES gui/test_gui_select_mask.cpp DEPS genko_app GUI TIMEOUT 900)
  # PC3 C-d: レイヤー → 重ね順・整列・分布・リンク・ロック…, 表示 → スナップ・スナップ先 (the words, keys and places), the move
  # tool's drag (by whole working pixels, the linked layers along), 自動選択 (Ctrl+click), snapping (a guide, Ctrl off)
  genko_test(test_gui_align SOURCES gui/test_gui_align.cpp DEPS genko_app GUI TIMEOUT 600)
  # チャンネル (channels@1, COLOR-03): the panel, the target keys, a fill on a channel, the view, options, split.
  genko_test(test_gui_channels SOURCES gui/test_gui_channels.cpp DEPS genko_app GUI TIMEOUT 600)
  # PC2c (COLOR-04): レイヤー → 色調補正's local corrections and image calculations, 編集 → フェード (only right after).
  genko_test(test_gui_image_ops SOURCES gui/test_gui_image_ops.cpp DEPS genko_app GUI TIMEOUT 600)
  # 色調補正 (PC2a, COLOR-04): the tone commands and their keys, their windows (channels by the page's mode, the formula,
  # 自動, a LUT from a file) with the result on the page, the 色調補正 and 属性 panels (one set_adjustment, one Undo), the
  # .cube written from ファイル.
  genko_test(test_gui_adjust SOURCES gui/test_gui_adjust.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # PC2d (COLOR-04): ヒストグラム and 情報 panels, プロファイルの情報.
  genko_test(test_gui_info SOURCES gui/test_gui_info.cpp DEPS genko_app GUI TIMEOUT 300)
  # the brush panel and ツールの設定: brushes kept per kind, one's own made, opened, given and read back (.abr too)
  genko_test(test_gui_brushes SOURCES gui/test_gui_brushes.cpp DEPS genko_app GUI TIMEOUT 600)
  # 紙質 (BRUSH-01): the dialog's paper, one's own brush with a paper (the library beside Python's file, read by
  # Python's reader), a book drawn with it saved and opened on another computer, the zoom, Undo, a .genkobrush
  genko_test(test_gui_paper SOURCES gui/test_gui_paper.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_gui_anim SOURCES gui/test_gui_anim.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_gui_paint SOURCES gui/test_gui_paint.cpp DEPS genko_app GUI TIMEOUT 600)
  genko_test(test_gui_guides SOURCES gui/test_gui_guides.cpp DEPS genko_app GUI TIMEOUT 900)
  genko_test(test_gui_colours SOURCES gui/test_gui_colours.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  genko_test(test_gui_workspace SOURCES gui/test_gui_workspace.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  genko_test(test_gui_look SOURCES gui/test_gui_look.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  genko_test(test_gui_carried SOURCES gui/test_gui_carried.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # PC3 A0 (smart-objects@1): レイヤー → スマートオブジェクト・ラスタライズ, the layer panel, 埋め込みを配置…, a large file on a
  # 16-bit page, the 属性 panel's part (its size, its colours).
  genko_test(test_gui_smart SOURCES gui/test_gui_smart.cpp DEPS genko_app genko_test_support ZLIB::ZLIB GUI TIMEOUT 600)
  # PC3 A1: コンテンツを編集 in a tab of the book (the parent page follows, undone, what the tab cannot use greyed), a
  # picture made a page first, the linked files (placed, marked, updated after asking where an agent wrote it), replaced,
  # written out, embedded and linked, packed; a candidate adopted over a picture's smart object said.
  genko_test(test_gui_smart_contents SOURCES gui/test_gui_smart_contents.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # PC3 C-a (vector@1): ペンツール・曲線ペンツール・パス選択ツール on the canvas, the パス panel's buttons, レイヤー →
  # ベクトルマスク and ラスタライズ, 選択範囲 → 作業用パスを作成…, the target path shown, パスを Illustrator へ….
  genko_test(test_gui_paths SOURCES gui/test_gui_paths.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # PC3 B4: スポット修復ブラシツール・修復ブラシツール・コピースタンプツール・ヒストリーブラシツール (the tools, their settings, Alt+click
  # and strokes), コピーソース panel, 履歴's ◆ row, コンテンツに応じた塗りつぶし…'s window.
  genko_test(test_gui_retouch SOURCES gui/test_gui_retouch.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # PC3 C-b (pen-shapes@1): the shape tools in their モード (シェイプ・パス・ピクセル, Alt from the centre, a new layer), the
  # pen tools' モード, パス選択ツール on a part, the シェイプ panel, カスタムシェイプを定義…, シェイプを結合 and the other commands.
  genko_test(test_gui_shapes SOURCES gui/test_gui_shapes.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  # M7 (the ledger counted against the C++ tree): the checks of Python's window tests no C++ test made before
  genko_test(test_gui_ledger_a SOURCES gui/test_gui_ledger_a.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_gui_ledger_c SOURCES gui/test_gui_ledger_c.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_gui_ledger_b SOURCES gui/test_gui_ledger_b.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  # the distribution (M6, AC-PACKAGE): this build installed into a folder of its own, run from it with a bare
  # environment, its libraries, notes, licences and SBOM (against docs/cpp-migration/THIRD_PARTY.md), install.sh and
  # uninstall.sh leaving books and settings as they were
  genko_test(test_package SOURCES package/test_package.cpp DEPS genko_test_support LABELS package TIMEOUT 900)
  target_compile_definitions(test_package PRIVATE GENKO_CMAKE_COMMAND="${CMAKE_COMMAND}" GENKO_BUILD_DIR="${CMAKE_BINARY_DIR}"
                                                  GENKO_VERSION="${PROJECT_VERSION}")
  if(GENKO_LIBRARY_KIND STREQUAL "SHARED")
    # (a sanitizer build: Genko's own libraries are shared and go beside the program — GenkoFlags.cmake)
    target_compile_definitions(test_package PRIVATE GENKO_SHARED_LIBRARIES=1)
  endif()
  add_dependencies(test_package genko)
  genko_test(test_gui_effects SOURCES gui/test_gui_effects.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  # 台詞 (M4①c): the text tool, the balloon pen, a balloon's handles and menu, the 台詞 panel, the story editor and
  # 台詞の検索・置換, each gesture's ops against Python's window, the page drawn after them
  genko_test(test_gui_lines SOURCES gui/test_gui_lines.cpp DEPS genko_app GUI TIMEOUT 900)
  # TEXT-01 in the window: the ruby typed with the input method (the table's "gui" cases), the ruby settings beside the
  # select tool, 要確認 marked, reassigned and cleared, Undo, 台詞の検索・置換, 縦書き・横書き, saved and read back
  genko_test(test_gui_ruby SOURCES gui/test_gui_ruby.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  # the book and its pages (M4②c): spreads, the nombre, the paper, covers, who draws a page, another book's pages taken
  # in (its assets copied first), a PSD as layers, the page list's menu; each command's ops against Python's window
  genko_test(test_gui_book SOURCES gui/test_gui_book.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  # PC3 C-e: ページ's paper commands and the crop tool, their ops, the finished size fitted when asked, the book preview
  genko_test(test_gui_page_size SOURCES gui/test_gui_page_size.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  # the book going out (M4③b): 書き出し… (the dialog's exports as `genko export` writes them), 印刷… (to a PDF: the
  # pages as render_page draws them at the printer's resolution), 履歴 (undo and redo by a click), 入稿前の点検 and its
  # panel (the issues listed, each shown on its page), the phone screens and the scales (a guide pulled out)
  genko_test(test_gui_output SOURCES gui/test_gui_output.cpp DEPS genko_app genko_test_support ZLIB::ZLIB GUI TIMEOUT 1200)
  target_compile_definitions(test_gui_output PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")
  add_dependencies(test_gui_output genko)
  genko_test(test_session SOURCES unit/test_session.cpp DEPS genko_app LABELS unit TIMEOUT 300)
  # the pen: Python's packing of mouse and tablet input, and the live line's pixels against the renderer's per brush
  genko_test(test_gui_pen SOURCES gui/test_gui_pen.cpp DEPS genko_app GUI TIMEOUT 1200)
  # the 52 commands of M2 and their groups: Python's words, keys and tips, the menus, and what each does
  genko_test(test_gui_actions SOURCES gui/test_gui_actions.cpp DEPS genko_app GUI TIMEOUT 900)
  # the canvas's gestures: the panel tool (cuts, gutters, corners, drawn panels) and the view
  genko_test(test_gui_canvas SOURCES gui/test_gui_canvas.cpp DEPS genko_app GUI TIMEOUT 1200)
  # saving and closing (AC-SAVE 2, 3, 4, 6, 8, 10): failures shown, the question before closing, recovery, retries
  genko_test(test_gui_save SOURCES gui/test_gui_save.cpp DEPS genko_app GUI TIMEOUT 900)
  target_compile_definitions(test_gui_save PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")
  add_dependencies(test_gui_save genko)
  # the windows on 1024×640 … 1920×1080 at 100–200 %, the long path, the preview, typed Japanese, the picture cache
  genko_test(test_gui_layout SOURCES gui/test_gui_layout.cpp DEPS genko_app genko_test_support GUI TIMEOUT 1200)
  # `genko app` in processes of its own: draw, undo, save, quit, open again; the autosave and the recovery copy across
  # processes (AC-SAVE 1, 8); `genko` alone
  genko_test(test_app_e2e SOURCES e2e/test_app_e2e.cpp DEPS genko_test_support LABELS e2e GUI TIMEOUT 1200)
  target_compile_definitions(test_app_e2e PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")
  # Studio and AI connection in the window (M5④). A stand-in for `genko mcp` the tests start as a client's settings
  # start Genko's server (tools/call runs this build's StudioService): never a network, never the person's settings.
  add_executable(genko_fake_mcp support/fake_mcp.cpp)
  target_link_libraries(genko_fake_mcp PRIVATE genko_service genko_studio Qt6::Core)
  genko_target_flags(genko_fake_mcp)
  # the approval box (each kind of request, answered as the person), the process bar, the panel view, the library, the
  # viewer, チャットでの承認, requests arriving; replies in several lines, asked before they go, kept as drafts (I13) — in
  # the book's studio/reply_drafts/ (v4 only); 絵柄を選ぶ (a catalog folder); genko:// links (a later start hands its
  # link over, a style from a catalog folder, `genko register-links`)
  genko_test(test_gui_studio SOURCES gui/test_gui_studio.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  add_dependencies(test_gui_studio genko)
  # AI と作る (AI-01, I07): the dialog as Python's, the connection's six states, the clients' settings written after the
  # change is seen and confirmed (copied first, put back), the test connection (nothing called, the book unchanged),
  # the book and conversation, and the first setup run through to the AI reading the person's decisions back (with the
  # stand-in server, and with this build's `genko mcp`)
  genko_test(test_gui_ai SOURCES gui/test_gui_ai.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  target_compile_definitions(test_gui_ai PRIVATE GENKO_FAKE_MCP="$<TARGET_FILE:genko_fake_mcp>")
  add_dependencies(test_gui_ai genko_fake_mcp genko)
  # the approval box's model and the AI dialog's Qt-free part against Python's (review_model.py, ai_link.py, links.py);
  # what `genko mcp` keeps in a book read by Python's readers; the reply drafts left alone by the Python version
  genko_test(test_contract_studio_gui SOURCES contract/test_contract_studio_gui.cpp DEPS genko_app genko_test_support LABELS contract TIMEOUT 1800)
endif()

# `genko` alone starts the desktop app when it is built (test_app_e2e); without it, it is a usage error
if(GENKO_BUILD_APP)
  target_compile_definitions(test_cli_e2e PRIVATE GENKO_WITH_APP=1)
else()
  target_compile_definitions(test_cli_e2e PRIVATE GENKO_WITH_APP=0)
endif()

# PERF-F (ACCEPTANCE.md §4), shown and judged against its pass values (not run by the usual CI: ctest -L perf; run it in
# Release): F3's making on `genko mcp` and `genko studio` (every call), 100 calls of each kind, the 8-page proof 10 times
# (tools/migration/pipeline_harness.py).
genko_test(test_perf_pipeline SOURCES perf/test_perf_pipeline.cpp DEPS genko_test_support LABELS perf TIMEOUT 3600)
set_tests_properties(test_perf_pipeline PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
# Timings, shown and not judged (not run by the usual CI: ctest -L perf).
genko_test(test_perf_render SOURCES perf/test_perf_render.cpp DEPS genko_render LABELS perf TIMEOUT 1800)
set_tests_properties(test_perf_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
if(GENKO_BUILD_APP)
  # the app's internal timings: five minutes of G pen input on the F1 book in `genko app` on Xvfb (ACCEPTANCE.md §6)
  genko_test(test_perf_app SOURCES perf/test_perf_app.cpp DEPS genko_app LABELS perf TIMEOUT 3600)
  set_tests_properties(test_perf_app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
  # PERF-G and M7's two hours of continuous work (perf: run on its own, on the quiet machine)
  genko_test(test_soak_app SOURCES perf/test_soak_app.cpp DEPS genko_app LABELS perf TIMEOUT 10800)
  set_tests_properties(test_soak_app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=10000000")
endif()

# (VALIDATION's V3 runs ASan three tests at a time: the slowest functions of these tests come near QtTest's own limit
# for one function, five minutes — test_smart_stack's tone on the precise and ink pages passed it, its test taking 753 s
# in all the time before. A function of these may take as long as its test's own limit, which still catches a hang,
# less 100 s.)
foreach(slow test_contract_raster_ops test_smart_stack test_contract_ops_cli test_contract_ops test_contract_raster_cli test_gui_color
        test_smart_warp test_precision_merge_controls test_ink_edit test_gui_canvas test_contract_studio test_contract_page_size
        test_gui_book test_gui_layout test_contract_service test_adjust test_contract_pyre test_contract_psd test_color_model
        test_ink_filters test_gui_look test_pyre test_masks test_contract_book test_gui_pen test_gui_lines)
  if(TEST ${slow})
    get_test_property(${slow} TIMEOUT limit)
    math(EXPR function_ms "(${limit} - 100) * 1000")
    set_property(TEST ${slow} APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=${function_ms}")
  endif()
endforeach()
