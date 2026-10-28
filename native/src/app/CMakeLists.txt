# The desktop app (Qt Widgets): the editing session, the windows and the canvas (docs/cpp-migration/ARCHITECTURE.md §1).
# Only this module depends on Qt Widgets.

# The icons and the panel templates (taken from the Python baseline's files as they are), and the screen's letters
# (a big resource: compiled straight to an object file).
qt_add_resources(GENKO_APP_RESOURCES "${GENKO_SOURCE_ROOT}/resources/genko_app.qrc")
qt_add_big_resources(GENKO_APP_FONTS "${GENKO_SOURCE_ROOT}/resources/genko_fonts.qrc")

genko_library(genko_app
  SOURCES
    config.cpp
    migration.cpp
    session.cpp
    theme.cpp
    icons.cpp
    ask.cpp
    path_label.cpp
    fit_preview.cpp
    workspace.cpp
    preferences.cpp
    color_settings_dialog.cpp
    comfort.cpp
    memory.cpp
    inputs.cpp
    glass.cpp
    backup.cpp
    actions.cpp
    bookview.cpp
    help.cpp
    look.cpp
    pen.cpp
    live_ink.cpp
    tiles.cpp
    display_color.cpp
    frame_tools.cpp
    perf.cpp
    canvas.cpp
    canvas_paint.cpp
    canvas_gpu.cpp
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
    select_dialogs.cpp
    main_window_channels.cpp
    main_window_brushes.cpp
    main_window_anim.cpp
    main_window_paint.cpp
    main_window_vector.cpp
    main_window_guides.cpp
    main_window_materials.cpp
    main_window_lines.cpp
    main_window_book.cpp
    main_window_color.cpp
    book_dialogs.cpp
    mode_dialogs.cpp
    main_window_output.cpp
    export_dialog.cpp
    print_dialog.cpp
    history_panel.cpp
    check_panel.cpp
    canvas_scale.cpp
    canvas_tools.cpp
    canvas_vector.cpp
    canvas_guides.cpp
    canvas_effects.cpp
    canvas_lines.cpp
    lettering.cpp
    text_style.cpp
    story_panel.cpp
    story_editor.cpp
    guide_panel.cpp
    material_tabs.cpp
    scanner.cpp
    subview.cpp
    timeline.cpp
    plugins_dialog.cpp
    brush_panel.cpp
    colours.cpp
    fields.cpp
    tool_settings.cpp
    layer_panel.cpp
    channel_panel.cpp
    info_panels.cpp
    layer_dialogs.cpp
    adjust_forms.cpp
    adjust_forms_tone.cpp
    adjust_panels.cpp
    adjust_forms_color.cpp
    image_dialogs.cpp
    filter_forms.cpp
    filter_forms_basic.cpp
    filter_forms_distort.cpp
    filter_menu.cpp
    filter_gallery_window.cpp
    lens_window.cpp
    vanishing_window.cpp
    camera_raw_window.cpp
    blur_gallery_tool.cpp
    lighting_window.cpp
    render_dialogs.cpp
    main_window_adjust.cpp
    main_window_smart.cpp
    main_window_page_size.cpp
    page_size_dialogs.cpp
    canvas_crop.cpp
    layer_panel_smart_filters.cpp
    inject.cpp
    review_model.cpp
    ai_link.cpp
    client_config.cpp
    mcp_probe.cpp
    reply_box.cpp
    style_picker.cpp
    viewer.cpp
    studio_widgets.cpp
    ai_dialog.cpp
    links.cpp
    main_window_studio.cpp
    test_script.cpp
    app_main.cpp
    # PC3-0: 編集's clipboard items (split from main_window_select.cpp)
    main_window_clipboard.cpp
    # PC3 B3: 賢い選択 (クイック選択・オブジェクト選択・被写体・空・フォーカス領域), 選択とマスク…, マッティング
    main_window_smart_select.cpp
    select_mask_window.cpp
    # PC3 C-a: the path tools, the Paths panel, the paths' and vector masks' commands
    canvas_pen.cpp
    paths_panel.cpp
    main_window_paths.cpp
    # PC3 B2: 塗りつぶし…・境界線を描く…, レイヤーマスク and the layers chosen, すべてのマスクを統合, 属性's mask
    fill_dialogs.cpp
    main_window_masks.cpp
    layer_panel_masks.cpp
    mask_panel.cpp
    # PC3 B4: the retouching tools, コピーソース, コンテンツに応じた塗りつぶし…
    retouch_tools.cpp
    clone_source_panel.cpp
    content_aware_window.cpp
    # PC3 C-b: the shape tools, the Shapes panel, the shape parts' commands
    shapes_panel.cpp
    main_window_shapes.cpp
    # PC3 C-c: 自由変形 and the warps (the box on the canvas; the window's commands, options bar and dialogs)
    canvas_transform.cpp
    main_window_transform.cpp
    # PC3 C-d: 表示 → スナップ・スナップ先 (the canvas's snapping), the move tool's options, レイヤー → 整列・分布・重ね順・リンク
    canvas_snap.cpp
    main_window_align.cpp
    # PC4 F-0: ファイル in Photoshop's order and names, 節 F's new commands as frames
    main_window_file.cpp
    # PC4 F-b: 開く…・指定形式で開く… (Genko's own open window), ファイル情報…
    open_dialog.cpp
    file_info_dialog.cpp
    # PC4 F-c: ファイル → 書き出し ▸ (書き出し形式…・書き出しの環境設定…・Web 用に保存（従来）…・クイック書き出し・レイヤーからファイル…) and レイヤー's
    main_window_export.cpp
    export_as_dialog.cpp
    save_for_web_dialog.cpp
    # PC4 E-0: the drawing tools' groups (the ツール menu, the palette's long press) and the frames of their panels
    paint_panels.cpp
    # PC4 E3: the Patterns panel, パターンを定義…, the pattern fill layer, パターンプレビュー
    patterns_panel.cpp
    main_window_patterns.cpp
    # PC4 E4: グラデーション (the panel, the library and the current gradient, the グラデーションエディター)
    gradients_panel.cpp
    # PC4 E5: the brushes that change pixels (ぼかし・シャープ・指先・覆い焼き・焼き込み・スポンジ・混合ブラシ・背景消しゴム・色の置き換え)
    paint_tools.cpp
    # PC4 E2: ブラシ・ブラシ設定 (F5)・ツールプリセット panels, ブラシを定義…, プリセット ▸ (プリセットマネージャー…・書き出し／読み込み…・移行)
    brushes_panel.cpp
    brush_settings_panel.cpp
    tool_presets_panel.cpp
    preset_manager.cpp
    main_window_brush_presets.cpp
    # PC4 F-e: Camera Raw's window for a camera RAW file (opening one, a RAW smart object's コンテンツを編集)
    camera_raw_open.cpp
    main_window_raw.cpp
    # PC4 E6: レイヤー → レイヤースタイル (the window), the スタイル panel
    layer_style_dialog.cpp
    styles_panel.cpp
    main_window_styles.cpp
    # PC5 G-0 (書式 メニュー)
    main_window_type.cpp
    ${GENKO_APP_RESOURCES}
    ${GENKO_APP_FONTS}
  PUBLIC_DEPS genko_core genko_storage genko_render genko_render_ops genko_formats genko_service Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Concurrent
              Qt6::PrintSupport Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
  PRIVATE_DEPS ${GENKO_LCMS2})
# (the monitor's profile on Windows: GetICMProfileW, display_color.cpp)
if(WIN32)
  target_link_libraries(genko_app PRIVATE gdi32)
endif()
