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
genko_test(test_transaction SOURCES unit/test_transaction.cpp DEPS genko_test_support LABELS unit TIMEOUT 600)

# Against the Python baseline (skipped when there is no reference Python: $GENKO_PYREF or /opt/pyref/bin/python).
genko_test(test_contract_storage SOURCES contract/test_contract_storage.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)
genko_test(test_contract_cli SOURCES contract/test_contract_cli.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 600)
genko_test(test_contract_save SOURCES contract/test_contract_save.cpp DEPS genko_test_support LABELS contract
           TIMEOUT 1200)

genko_test(test_cli_e2e SOURCES e2e/test_cli_e2e.cpp DEPS genko_test_support LABELS e2e)
genko_test(test_save_e2e SOURCES e2e/test_save_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
genko_test(test_migrate_e2e SOURCES e2e/test_migrate_e2e.cpp DEPS genko_test_support LABELS e2e TIMEOUT 1200)
