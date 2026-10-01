# Shared test helpers (strict JSON comparison, the Python reference, running the CLI).
add_library(genko_test_support STATIC support/testsupport.cpp)
target_include_directories(genko_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(genko_test_support PUBLIC genko_storage Qt6::Core)
target_compile_definitions(genko_test_support PRIVATE
  GENKO_REPO_ROOT="${GENKO_REPO_ROOT}"
  GENKO_TEST_DATA="${GENKO_SOURCE_ROOT}/tests/data"
  GENKO_CLI="$<TARGET_FILE:genko>")
genko_target_flags(genko_test_support)
add_dependencies(genko_test_support genko)

genko_test(test_buildinfo SOURCES unit/test_buildinfo.cpp DEPS genko_api LABELS unit)
genko_test(test_json SOURCES unit/test_json.cpp DEPS genko_test_support LABELS unit)
genko_test(test_pynum SOURCES unit/test_pynum.cpp DEPS genko_test_support LABELS unit)
genko_test(test_model SOURCES unit/test_model.cpp DEPS genko_test_support LABELS unit)
genko_test(test_strokes SOURCES unit/test_strokes.cpp DEPS genko_test_support LABELS unit)
genko_test(test_storage SOURCES unit/test_storage.cpp DEPS genko_test_support LABELS unit)

# Drawing (M2-R1): Python's random and stroke geometry, Pillow's Image layer and PNG reading, against tables made by
# tools/migration/render_harness.py unit-tables (data/pyref/render_unit_tables.json).
genko_test(test_pyrandom SOURCES unit/test_pyrandom.cpp DEPS genko_test_support LABELS unit)
genko_test(test_stroke_geom SOURCES unit/test_stroke_geom.cpp DEPS genko_test_support LABELS unit)
genko_test(test_image SOURCES unit/test_image.cpp DEPS genko_test_support genko_render LABELS unit)
genko_test(test_png SOURCES unit/test_png.cpp DEPS genko_test_support genko_render LABELS unit)
genko_test(test_render_page SOURCES unit/test_render_page.cpp DEPS genko_render LABELS unit)

# Against the Python baseline (skipped when there is no reference Python: $GENKO_PYREF or /opt/pyref/bin/python).
genko_test(test_contract_storage SOURCES contract/test_contract_storage.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)
genko_test(test_contract_cli SOURCES contract/test_contract_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 600)
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

genko_test(test_cli_e2e SOURCES e2e/test_cli_e2e.cpp DEPS genko_test_support LABELS e2e)

# Timings, shown and not judged (not run by the usual CI: ctest -L perf).
genko_test(test_perf_render SOURCES perf/test_perf_render.cpp DEPS genko_render LABELS perf TIMEOUT 1800)
set_tests_properties(test_perf_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=1700000")
