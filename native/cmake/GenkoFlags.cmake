# Compiler flags shared by every Genko target (call genko_target_flags(<target>)).
#
# Determinism (docs/cpp-migration/ARCHITECTURE.md §6): the page pixels must be the same on every OS and
# compiler, so floating point is never contracted into FMA and fast-math is never used.

function(genko_target_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /fp:precise /Zc:__cplusplus /EHsc /bigobj)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    if(GENKO_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow=local -Wno-unused-parameter
                                              -ffp-contract=off -fno-fast-math -fno-strict-aliasing)
    if(GENKO_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(GENKO_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=${GENKO_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${target} PRIVATE -fsanitize=${GENKO_SANITIZE})
  endif()
endfunction()

# A Genko library: static, its include root is native/src, flags as above.
function(genko_library name)
  cmake_parse_arguments(GL "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(${name} STATIC ${GL_SOURCES})
  target_include_directories(${name} PUBLIC "${GENKO_SOURCE_ROOT}/src")
  target_link_libraries(${name} PUBLIC ${GL_PUBLIC_DEPS} PRIVATE ${GL_PRIVATE_DEPS})
  genko_target_flags(${name})
endfunction()

# A Qt Test executable registered with CTest. GUI-less unless GUI is given.
function(genko_test name)
  cmake_parse_arguments(GT "GUI" "TIMEOUT" "SOURCES;DEPS;LABELS" ${ARGN})
  add_executable(${name} ${GT_SOURCES})
  target_link_libraries(${name} PRIVATE Qt6::Test ${GT_DEPS})
  target_compile_definitions(${name} PRIVATE GENKO_REPO_ROOT="${GENKO_REPO_ROOT}"
                                             GENKO_TEST_DATA="${GENKO_SOURCE_ROOT}/tests/data")
  genko_target_flags(${name})
  add_test(NAME ${name} COMMAND ${name})
  set(_labels ${GT_LABELS})
  if(GT_GUI)
    list(APPEND _labels gui)
  endif()
  if(_labels)
    set_tests_properties(${name} PROPERTIES LABELS "${_labels}")
  endif()
  if(GT_TIMEOUT)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${GT_TIMEOUT})
  else()
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()
