# The studio (docs/cpp-migration/ARCHITECTURE.md §1: studio/): the studio's own files in a book's folder and the person's
# config folder (presence, the tool log, claims, generation requests, the image tools), its input schemas and their
# checker, and the agent's state read from a book. The studio's ops are core's (core/studio.hpp), applied through the
# CommandBus like every other op.
genko_library(genko_studio
  SOURCES
    files.cpp
    schemas.cpp
    state.cpp
  PUBLIC_DEPS genko_core genko_storage Qt6::Core)
