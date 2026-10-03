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
genko_test(test_command_bus SOURCES unit/test_command_bus.cpp DEPS genko_test_support LABELS unit)
genko_test(test_ops SOURCES unit/test_ops.cpp DEPS genko_test_support LABELS unit)
genko_test(test_transaction SOURCES unit/test_transaction.cpp DEPS genko_test_support LABELS unit TIMEOUT 600)

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
genko_test(test_render_page SOURCES unit/test_render_page.cpp DEPS genko_render LABELS unit)
# The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) without Python.
genko_test(test_raster_ops SOURCES unit/test_raster_ops.cpp DEPS genko_test_support genko_render LABELS unit TIMEOUT 900)

# Against the Python baseline (skipped when there is no reference Python: $GENKO_PYREF or /opt/pyref/bin/python).
genko_test(test_contract_storage SOURCES contract/test_contract_storage.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)
genko_test(test_contract_cli SOURCES contract/test_contract_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 600)
genko_test(test_contract_save SOURCES contract/test_contract_save.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)
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
set_tests_properties(test_contract_render_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
# The ops of M2-O1 against Python's apply_ops (fixed cases, random sequences, saved and read back) and the command line.
genko_test(test_contract_ops SOURCES contract/test_contract_ops.cpp contract/opsupport.cpp DEPS genko_test_support
           genko_render LABELS contract TIMEOUT 3600)
genko_test(test_contract_ops_cli SOURCES contract/test_contract_ops_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 3600)
# The ops of M3-A1 (layers, masks, pixels, fills, areas, paper) against Python's apply_ops: fixed cases, random sequences,
# saved and read back.
genko_test(test_contract_raster_ops SOURCES contract/test_contract_raster_ops.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render LABELS contract TIMEOUT 7200)
# … the pages they make, drawn (whole and in parts), and the command line.
genko_test(test_contract_raster_render SOURCES contract/test_contract_raster_render.cpp
           DEPS genko_test_support genko_render LABELS contract TIMEOUT 3600)
set_tests_properties(test_contract_raster_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=3500000")
genko_test(test_contract_raster_cli SOURCES contract/test_contract_raster_cli.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render LABELS contract TIMEOUT 5400)
# core/filters, the correction layers' adjustments for the ops and the renderer, against Python's filters.apply_filter.
genko_test(test_contract_filters SOURCES contract/test_contract_filters.cpp DEPS genko_test_support genko_render
           LABELS contract TIMEOUT 600)
# The two together: a book made by the ops (C++ and Python), drawn by `genko render` and `python -m genko render`.
genko_test(test_contract_drawn_by_ops SOURCES contract/test_contract_drawn_by_ops.cpp contract/opsupport.cpp
           DEPS genko_test_support genko_render LABELS contract TIMEOUT 1800)

genko_test(test_cli_e2e SOURCES e2e/test_cli_e2e.cpp DEPS genko_test_support LABELS e2e)
genko_test(test_save_e2e SOURCES e2e/test_save_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
genko_test(test_migrate_e2e SOURCES e2e/test_migrate_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
genko_test(test_ops_e2e SOURCES e2e/test_ops_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)

# Timings, shown and not judged (not run by the usual CI: ctest -L perf).
genko_test(test_perf_render SOURCES perf/test_perf_render.cpp DEPS genko_render LABELS perf TIMEOUT 1800)
set_tests_properties(test_perf_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
