# The desktop app (Qt Widgets): the editing session, the windows and the canvas (docs/cpp-migration/ARCHITECTURE.md §1).
# Only this module depends on Qt Widgets.

# The icons and the panel templates (taken from the Python baseline's files as they are), and the screen's letters
# (a big resource: compiled straight to an object file).
qt_add_resources(GENKO_APP_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_app.qrc")
qt_add_big_resources(GENKO_APP_FONTS "${GENKO_SOURCE_ROOT}/resources/genko_fonts.qrc")

genko_library(genko_app
  SOURCES
    config.cpp
    session.cpp
    wording.cpp
    theme.cpp
    icons.cpp
    ask.cpp
    path_label.cpp
    fit_preview.cpp
    look.cpp
    pen.cpp
    live_ink.cpp
    tiles.cpp
    frame_tools.cpp
    perf.cpp
    canvas.cpp
    canvas_paint.cpp
    canvas_input.cpp
    canvas_select.cpp
    thumbs.cpp
    material_panel.cpp
    material_preview_cache.cpp
    pages_panel.cpp
    navigator.cpp
    templates.cpp
    dialogs.cpp
    save_status.cpp
    ime.cpp
    main_window.cpp
    main_window_actions.cpp
    main_window_select.cpp
    main_window_brushes.cpp
    main_window_anim.cpp
    main_window_paint.cpp
    main_window_vector.cpp
    main_window_guides.cpp
    main_window_materials.cpp
    canvas_tools.cpp
    canvas_vector.cpp
    canvas_guides.cpp
    canvas_effects.cpp
    guide_panel.cpp
    material_tabs.cpp
    scanner.cpp
    timeline.cpp
    plugins_dialog.cpp
    brush_panel.cpp
    colours.cpp
    fields.cpp
    tool_settings.cpp
    layer_panel.cpp
    layer_dialogs.cpp
    inject.cpp
    test_script.cpp
    app_main.cpp
    ${GENKO_APP_RESOURCES}
    ${GENKO_APP_FONTS}
  PUBLIC_DEPS genko_core genko_storage genko_render genko_render_ops Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Concurrent)
