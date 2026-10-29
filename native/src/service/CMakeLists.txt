# The studio service (docs/cpp-migration/ARCHITECTURE.md §1 service/): the tools an agent calls and the approvals a
# person makes (Python's genko/studio/service.py: StudioService and HumanService, and the modules they use), shared
# by `genko studio`, MCP, HTTP and the app; and the words people see in errors (Python's genko/app/wording.py). Above
# formats: the tools draw previews and write the exports.

# The studio's own data, embedded from the Python baseline's files as they are (a change to one re-runs CMake): the
# page layout templates, the words of the art requests, the rules and drawing guide an agent reads, and the skill the
# MCP server hands an agent (genko://guide/skill).
set(_genko_studio_data
  "LAYOUTS|${GENKO_REPO_ROOT}/src/genko/studio/layouts.json"
  "VOCAB|${GENKO_REPO_ROOT}/src/genko/studio/vocab.json"
  "RULES|${GENKO_REPO_ROOT}/src/genko/studio/guide/manga_rules.md"
  "DRAWING|${GENKO_REPO_ROOT}/src/genko/studio/guide/drawing.md"
  "SKILL|${GENKO_REPO_ROOT}/src/genko/studio/guide/SKILL.md")
set(_b "0x[0-9a-f][0-9a-f],")
foreach(_entry IN LISTS _genko_studio_data)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _file)
  file(READ "${_file}" _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n"
         _bytes "${_bytes}")
  set(GENKO_STUDIO_${_name}_BYTES "${_bytes}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
endforeach()
configure_file(studio_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/studio_data.cpp" @ONLY)

genko_library(genko_service
  SOURCES
    wording.cpp
    result.cpp
    book.cpp
    data.cpp
    args.cpp
    pyfmt.cpp
    text.cpp
    blocking.cpp
    fxwords.cpp
    layout.cpp
    letter.cpp
    lint.cpp
    preflight.cpp
    finish.cpp
    worklist.cpp
    urls.cpp
    stylecat.cpp
    upscale.cpp
    service_util.cpp
    briefs.cpp
    preview.cpp
    sheets.cpp
    guide.cpp
    genreq.cpp
    importer.cpp
    atari.cpp
    review.cpp
    jobs.cpp
    evaluate.cpp
    evaluate_tests.cpp
    studio_service.cpp
    service_write.cpp
    studio_art.cpp
    studio_atari.cpp
    studio_output.cpp
    tool_table.cpp
    human_service.cpp
    # PC5 I-0 (自動処理の土台: オートアクションの置き場, the steps and the runner, the script events' table)
    actions.cpp
    automate.cpp
    script_events.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/studio_data.cpp"
  PUBLIC_DEPS genko_formats genko_render_ops genko_render genko_studio genko_storage genko_core Qt6::Core
  PRIVATE_DEPS Qt6::Network)
