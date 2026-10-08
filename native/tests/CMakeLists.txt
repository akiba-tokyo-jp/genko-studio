# Another process that holds a book's project lock (the lock tests and the e2e tests start it).
add_executable(genko_lock_helper support/lock_helper.cpp)
target_link_libraries(genko_lock_helper PRIVATE genko_storage Qt6::Core)
genko_target_flags(genko_lock_helper)

# Shared test helpers (strict JSON comparison, the Python reference, running the CLI).
add_library(genko_test_support STATIC support/testsupport.cpp)
target_include_directories(genko_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(genko_test_support PUBLIC genko_storage Qt6::Core)
target_compile_definitions(genko_test_support PRIVATE
  GENKO_REPO_ROOT="${GENKO_REPO_ROOT}"
  GENKO_TEST_DATA="${GENKO_SOURCE_ROOT}/tests/data"
  GENKO_CLI="$<TARGET_FILE:genko>"
  GENKO_LOCK_HELPER="$<TARGET_FILE:genko_lock_helper>")
genko_target_flags(genko_test_support)
add_dependencies(genko_test_support genko genko_lock_helper)

genko_test(test_buildinfo SOURCES unit/test_buildinfo.cpp DEPS genko_api LABELS unit)
genko_test(test_json SOURCES unit/test_json.cpp DEPS genko_test_support LABELS unit)
genko_test(test_pynum SOURCES unit/test_pynum.cpp DEPS genko_test_support LABELS unit)
genko_test(test_model SOURCES unit/test_model.cpp DEPS genko_test_support LABELS unit)
genko_test(test_strokes SOURCES unit/test_strokes.cpp DEPS genko_test_support LABELS unit)
genko_test(test_storage SOURCES unit/test_storage.cpp DEPS genko_test_support LABELS unit)
genko_test(test_lock SOURCES unit/test_lock.cpp DEPS genko_test_support LABELS unit)
genko_test(test_journal SOURCES unit/test_journal.cpp DEPS genko_test_support LABELS unit)
genko_test(test_command_bus SOURCES unit/test_command_bus.cpp DEPS genko_test_support genko_render_ops LABELS unit)
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
genko_test(test_png SOURCES unit/test_png.cpp DEPS genko_test_support genko_render LABELS unit)
genko_test(test_color_raster SOURCES unit/test_color_raster.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_color_filters SOURCES unit/test_color_filters.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_color_raster_edit SOURCES unit/test_color_raster_edit.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
# (91 merge/flatten/convert cases at u16 and f32: about 460 s under ASan, alone)
genko_test(test_precision_merge_controls SOURCES unit/test_precision_merge_controls.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit TIMEOUT 1200)
genko_test(test_precision_flatten SOURCES unit/test_precision_flatten.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_precision_merge_safety SOURCES unit/test_precision_merge_safety.cpp DEPS genko_core genko_storage genko_render genko_render_ops LABELS unit)
genko_test(test_render_page SOURCES unit/test_render_page.cpp DEPS genko_render LABELS unit)
target_compile_definitions(test_render_page PRIVATE GENKO_SOURCE_DIR="${GENKO_SOURCE_ROOT}")
# Tones, effect lines and rulers (M3-B).
genko_test(test_rulers SOURCES unit/test_rulers.cpp DEPS genko_test_support genko_render_ops LABELS unit)
genko_test(test_tones_effects SOURCES unit/test_tones_effects.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
# 3D (M3-C): the ops, their refusals, Undo and saving, the pose library, the guides drawn (no Python needed).
genko_test(test_ops_3d SOURCES unit/test_ops_3d.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
# The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) without Python.
genko_test(test_raster_ops SOURCES unit/test_raster_ops.cpp DEPS genko_test_support genko_render_ops LABELS unit TIMEOUT 900)
genko_test(test_material_stamp SOURCES unit/test_material_stamp.cpp DEPS genko_core genko_storage genko_render_ops LABELS unit)

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
# The line ops of M4: add_line, edit_line, move_line, delete_line, reorder_lines, cut_balloon, set_balloon_path and
# replace_text against Python's apply_ops (lines_cases.json).
genko_test(test_contract_lines SOURCES contract/test_contract_lines.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render_ops LABELS contract TIMEOUT 1800)
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
# Filter plugins (M3③, COMP-04): the runner against Python's plugins.run, the choice, the bounds.
genko_test(test_contract_plugins SOURCES contract/test_contract_plugins.cpp DEPS genko_test_support genko_render_ops
           LABELS contract TIMEOUT 1800)

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

# The desktop app (M2-G1): the editing session alone, then the windows on the offscreen platform (label gui).
if(GENKO_BUILD_APP)
  genko_test(test_gui_materials SOURCES gui/test_gui_materials.cpp DEPS genko_app GUI TIMEOUT 300)
  # the disposable material preview cache alone (keys, restarts, corrupt files, budgets, originals kept)
  genko_test(test_material_preview_cache SOURCES unit/test_material_preview_cache.cpp DEPS genko_app LABELS unit)
  genko_test(test_gui_color SOURCES gui/test_gui_color.cpp DEPS genko_app GUI TIMEOUT 300)
  # the layer panel: its list, buttons, settings, menus (several at once, masks, effects), fill and correction layers,
  # a filter with its result on the page while its numbers change
  genko_test(test_gui_layers SOURCES gui/test_gui_layers.cpp DEPS genko_app GUI TIMEOUT 600)
  # 範囲選択: the marquee's ways with Shift / Alt, the selection's commands, the chosen part moved, copied, pasted
  genko_test(test_gui_select SOURCES gui/test_gui_select.cpp DEPS genko_app GUI TIMEOUT 600)
  # the brush panel and ツールの設定: brushes kept per kind, one's own made, opened, given and read back (.abr too)
  genko_test(test_gui_brushes SOURCES gui/test_gui_brushes.cpp DEPS genko_app GUI TIMEOUT 600)
  genko_test(test_gui_anim SOURCES gui/test_gui_anim.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_gui_paint SOURCES gui/test_gui_paint.cpp DEPS genko_app GUI TIMEOUT 600)
  genko_test(test_gui_guides SOURCES gui/test_gui_guides.cpp DEPS genko_app GUI TIMEOUT 900)
  genko_test(test_gui_colours SOURCES gui/test_gui_colours.cpp DEPS genko_app genko_test_support GUI TIMEOUT 600)
  genko_test(test_gui_effects SOURCES gui/test_gui_effects.cpp DEPS genko_app genko_test_support GUI TIMEOUT 900)
  genko_test(test_session SOURCES unit/test_session.cpp DEPS genko_app LABELS unit TIMEOUT 300)
  # the pen: Python's packing of mouse and tablet input, and the live line's pixels against the renderer's per brush
  genko_test(test_gui_pen SOURCES gui/test_gui_pen.cpp DEPS genko_app GUI TIMEOUT 1200)
  # the 52 commands of M2 and their groups: Python's words, keys and tips, the menus, and what each does
  genko_test(test_gui_actions SOURCES gui/test_gui_actions.cpp DEPS genko_app GUI TIMEOUT 900)
  # the canvas's gestures: the panel tool (cuts, gutters, corners, drawn panels) and the view
  genko_test(test_gui_canvas SOURCES gui/test_gui_canvas.cpp DEPS genko_app GUI TIMEOUT 600)
  # saving and closing (AC-SAVE 2, 3, 4, 6, 8, 10): failures shown, the question before closing, recovery, retries
  genko_test(test_gui_save SOURCES gui/test_gui_save.cpp DEPS genko_app GUI TIMEOUT 900)
  target_compile_definitions(test_gui_save PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")
  add_dependencies(test_gui_save genko)
  # the windows on 1024×640 … 1920×1080 at 100–200 %, the long path, the preview, typed Japanese, the picture cache
  genko_test(test_gui_layout SOURCES gui/test_gui_layout.cpp DEPS genko_app GUI TIMEOUT 1200)
  # `genko app` in processes of its own: draw, undo, save, quit, open again; the autosave and the recovery copy across
  # processes (AC-SAVE 1, 8); `genko` alone
  genko_test(test_app_e2e SOURCES e2e/test_app_e2e.cpp DEPS genko_test_support LABELS e2e GUI TIMEOUT 1200)
  target_compile_definitions(test_app_e2e PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")
endif()

# `genko` alone starts the desktop app when it is built (test_app_e2e); without it, it is a usage error
if(GENKO_BUILD_APP)
  target_compile_definitions(test_cli_e2e PRIVATE GENKO_WITH_APP=1)
else()
  target_compile_definitions(test_cli_e2e PRIVATE GENKO_WITH_APP=0)
endif()

# Timings, shown and not judged (not run by the usual CI: ctest -L perf).
genko_test(test_perf_render SOURCES perf/test_perf_render.cpp DEPS genko_render LABELS perf TIMEOUT 1800)
set_tests_properties(test_perf_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
if(GENKO_BUILD_APP)
  # the app's internal timings: five minutes of G pen input on the F1 book in `genko app` on Xvfb (ACCEPTANCE.md §6)
  genko_test(test_perf_app SOURCES perf/test_perf_app.cpp DEPS genko_app LABELS perf TIMEOUT 3600)
  set_tests_properties(test_perf_app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
endif()
