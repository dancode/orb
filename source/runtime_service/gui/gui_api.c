/*==============================================================================================

    runtime_service/gui/gui_api.c -- Module vtable and lifecycle descriptor.

    Included last by gui.c so all functions from the other constituent
    files are visible here.  The vtable (g_gui_api) is the live struct that
    the module system hands out through the gui() accessor.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Vtable struct  (extern const -- referenced by MOD_GATEWAY_STATIC and func_api pointer)
==============================================================================================*/

const gui_api_t g_gui_api_struct = 
{

    /*===============================================  GUI_FRAME  ===============================================*/

    .init                               = gui_init,
    .shutdown                           = gui_shutdown,
    .font_load                          = gui_font_load,
    .font_load_builtin                  = gui_font_load_builtin,
    .asset_path                         = gui_asset_path,
    .boot                               = gui_boot,
    .mem_stats                          = gui_mem_stats,
    .print_mem_stats                    = gui_print_mem_stats,
    .render_stats                       = gui_render_stats,
    .set_frame_hooks                    = gui_set_frame_hooks,
    .frame_begin                        = gui_frame_begin,
    .frame_end                          = gui_frame_end,
    .render                             = gui_render,
    .frame_pace                         = gui_frame_pace,
    .frame_poll                         = gui_frame_poll,
    .present_begin                      = gui_present_begin,
    .present_end                        = gui_present_end,
    .set_idle_skip                      = gui_set_idle_skip,
    .idle_skip                          = gui_idle_skip,
    .viewport_open                      = gui_viewport_open,
    .viewport_close                     = gui_viewport_close,
    .viewport_resize                    = gui_viewport_resize,
    .viewport_shell                     = gui_viewport_shell,
    .viewport_caption_h                 = gui_viewport_caption_h,
    .viewport_size                      = gui_viewport_size,
    .viewport_content_y                 = gui_viewport_content_y,
    .viewport_spawn                     = gui_viewport_spawn,
    .viewport_update                    = gui_viewport_update,
    .viewport_render_floaters           = gui_viewport_render_floaters,
    .ctx_create                         = gui_ctx_create,
    .ctx_destroy                        = gui_ctx_destroy,
    .ctx_bind                           = gui_ctx_bind,
    .ctx_set_listening                  = gui_ctx_set_listening,
    .ctx_begin                          = gui_ctx_begin,
    .ctx_end                            = gui_ctx_end,
    .event                              = gui_event,

    /*===============================================  GUI_DRAW  ================================================*/

    .font_load_into                     = gui_font_load_into,
    .font_use                           = gui_font_use,
    .push_font                          = gui_push_font,
    .pop_font                           = gui_pop_font,
    .font_active_id                     = gui_font_active_id,

    /* custom draw -- canvas primitives, symbols, paths */

    .draw_rect                          = gui_draw_rect,
    .draw_rects                         = gui_draw_rects,
    .draw_text                          = gui_draw_text,
    .volatile_cb                        = gui_volatile_cb,
    .volatile_begin                     = gui_volatile_begin,
    .volatile_end                       = gui_volatile_end,
    .text_size                          = gui_text_size,
    .draw_text_in                       = gui_draw_text_in,
    .draw_text_clipped                  = gui_draw_text_clipped,
    .register_icon                      = gui_register_icon,
    .load_icon                          = gui_load_icon,
    .find_icon                          = gui_find_icon,
    .icon_size                          = gui_icon_size,
    .image                              = gui_image,
    .draw_icon_in                       = gui_draw_icon_in,
    .image_texture                      = gui_image_texture,
    .draw_texture_in                    = gui_draw_texture_in,
    .font_atlas_idx                     = gui_font_atlas_idx,
    .font_atlas_size                    = gui_font_atlas_size,
    .draw_check_mark                    = gui_draw_check_mark,
    .draw_arrow                         = gui_draw_arrow,
    .draw_bullet                        = gui_draw_bullet,
    .draw_close                         = gui_draw_close,
    .draw_arrow_pointing_at             = gui_draw_arrow_pointing_at,
    .draw_chevron                       = gui_draw_chevron,
    .draw_plus_minus                    = gui_draw_plus_minus,
    .draw_frame                         = gui_draw_frame,
    .draw_round_rect                    = gui_draw_round_rect,
    .draw_ngon                          = gui_draw_ngon,
    .draw_circle                        = gui_draw_circle,
    .draw_arc                           = gui_draw_arc,
    .draw_pie                           = gui_draw_pie,
    .draw_bezier_quad                   = gui_draw_bezier_quad,
    .draw_bezier_cubic                  = gui_draw_bezier_cubic,
    .draw_dashed_line                   = gui_draw_dashed_line,
    .draw_checker                       = gui_draw_checker,
    .draw_hatch                         = gui_draw_hatch,
    .draw_gradient                      = gui_draw_gradient,
    .draw_shadow                        = gui_draw_shadow,
    .draw_text_outline                  = gui_draw_text_outline,
    .draw_text_shadow                   = gui_draw_text_shadow,
    .draw_grip                          = gui_draw_grip,
    .draw_spinner                       = gui_draw_spinner,
    .draw_progress_arc                  = gui_draw_progress_arc,
    .draw_line                          = gui_draw_line,
    .draw_polyline                      = gui_draw_polyline,
    .path_clear                         = gui_path_clear,
    .path_line_to                       = gui_path_line_to,
    .path_stroke                        = gui_path_stroke,
    .push_clip                          = gui_push_clip,
    .pop_clip                           = gui_pop_clip,

    /*===============================================  GUI_CORE  ================================================*/

    /* item() -- behavior over a caller rect */

    .item                               = gui_item,
    .invisible_button                   = gui_invisible_button,

    /* animation service */

    .anim_f32                           = gui_anim_f32,
    .anim_start                         = gui_anim_start,
    .anim_ease                          = gui_anim_ease,
    .anim_color                         = gui_anim_color,
    .anim_vec2                          = gui_anim_vec2,
    .anim_rect                          = gui_anim_rect,

    /* identity + item flags + drag and drop */

    .push_id                            = gui_push_id,
    .push_id_int                        = gui_push_id_int,
    .pop_id                             = gui_pop_id,
    .push_item_flag                     = gui_push_item_flag,
    .pop_item_flag                      = gui_pop_item_flag,
    .next_item_flag                     = gui_next_item_flag,
    .disabled_begin                     = gui_disabled_begin,
    .disabled_end                       = gui_disabled_end,
    .drag_source_begin                  = gui_drag_source_begin,
    .drag_source_end                    = gui_drag_source_end,
    .drag_payload_set                   = gui_drag_payload_set,
    .drag_target_begin                  = gui_drag_target_begin,
    .drag_payload_accept                = gui_drag_payload_accept,
    .drag_target_end                    = gui_drag_target_end,
    .drag_active                        = gui_drag_active,
    .drag_payload_peek                  = gui_drag_payload_peek,

    /* queries -- io snapshot, item state, redraw state */

    .want_capture_mouse                 = gui_want_capture_mouse,
    .want_capture_keyboard              = gui_want_capture_keyboard,
    .is_mouse_hovering_rect             = gui_is_mouse_hovering_rect,
    .is_item_hovered                    = gui_is_item_hovered,
    .is_item_active                     = gui_is_item_active,
    .is_item_clicked                    = gui_is_item_clicked,
    .is_item_focused                    = gui_is_item_focused,
    .is_item_activated                  = gui_is_item_activated,
    .is_item_deactivated                = gui_is_item_deactivated,
    .is_item_deactivated_after_edit     = gui_is_item_deactivated_after_edit,
    .is_item_visible                    = gui_is_item_visible,
    .get_item_rect                      = gui_get_item_rect,
    .is_key_down                        = gui_is_key_down,
    .is_key_pressed                     = gui_is_key_pressed,
    .is_key_pressed_repeat              = gui_is_key_pressed_repeat,
    .is_key_released                    = gui_is_key_released,
    .is_mouse_down                      = gui_is_mouse_down,
    .is_mouse_clicked                   = gui_is_mouse_clicked,
    .is_mouse_released                  = gui_is_mouse_released,
    .is_mouse_double_clicked            = gui_is_mouse_double_clicked,
    .get_mouse_pos                      = gui_get_mouse_pos,
    .get_mouse_wheel                    = gui_get_mouse_wheel,
    .get_delta_time                     = gui_get_delta_time,
    .get_time                           = gui_get_time,
    .cursor_set                         = gui_cursor_set,
    .get_mouse_cursor                   = gui_get_mouse_cursor,
    .set_keyboard_focus                 = gui_set_keyboard_focus,
    .set_edit_cursor_end                = gui_set_edit_cursor_end,
    .set_edit_key_hook                  = gui_set_edit_key_hook,
    .wants_redraw                       = gui_wants_redraw,
    .request_redraw                     = gui_request_redraw,
    .frame_dirty                        = gui_frame_dirty,
    .volatile_live                      = gui_volatile_live,
    .set_force_redraw                   = gui_set_force_redraw,
    .force_redraw                       = gui_force_redraw,

    /*==============================================  GUI_SURFACE  =============================================*/

    /* surfaces -- root regions + scroll */

    .pane_begin                         = gui_pane_begin,
    .pane_end                           = gui_pane_end,
    .region_begin                       = gui_region_begin,
    .region_end                         = gui_region_end,
    .scroll_by                          = gui_scroll_by,
    .feat_move                          = gui_feat_move,
    .feat_resize                        = gui_feat_resize,
    .feat_collapse                      = gui_feat_collapse,
    .feat_maximize                      = gui_feat_maximize,
    .feat_clamp                         = gui_feat_clamp,

    /*===============================================  GUI_RECT  ================================================*/

    .content_rect                       = gui_content_rect,
    .split                              = gui_split,
    .carve                              = gui_carve,
    .anchor                             = gui_anchor,

    /*===============================================  GUI_FLOW  ================================================*/

    .child_begin                        = gui_child_begin,
    .push_layout                        = gui_push_layout,
    .push_layout_overlay                = gui_push_layout_overlay,
    .pop_layout                         = gui_pop_layout,
    .child_end                          = gui_child_end,

    /* layout verbs, sizing, virtualization, seams */

    .layout_default                     = gui_layout_default,
    .stack                              = gui_stack,
    .row                                = gui_row,
    .cols                               = gui_cols,
    .cols_n                             = gui_cols_n,
    .row_cols                           = gui_row_cols,
    .row_cols_n                         = gui_row_cols_n,
    .row2                               = gui_row2,
    .row3                               = gui_row3,
    .row4                               = gui_row4,
    .form                               = gui_form,
    .field_split                        = gui_field_split,
    .field_label_left                   = gui_field_label_left,
    .field_label_right                  = gui_field_label_right,
    .field_set                          = gui_field_set,
    .field_get                          = gui_field_get,
    .skip_label                         = gui_skip_label,
    .field_row                          = field_row,
    .grid                               = gui_grid,
    .grid_cells                         = gui_grid_cells,
    .bar                                = gui_bar,
    .strip                              = gui_strip,
    .pack_size                          = gui_pack_size,
    .pack_nextline                      = gui_pack_nextline,
    .pack_wrap                          = gui_pack_wrap,
    .push_layout_state                  = gui_push_layout_state,
    .pop_layout_state                   = gui_pop_layout_state,
    .align                              = gui_align,
    .next_item_fit                      = gui_next_item_fit,
    .next_item_h                        = gui_next_item_h,
    .next_item_rect                     = gui_next_item_rect,
    .next_item_align                    = gui_next_item_align,
    .same_line                          = gui_same_line,
    .stack_same_line                    = gui_stack_same_line,
    .skip                               = gui_skip,
    .separator                          = gui_separator,
    .canvas                             = gui_canvas,
    .sz_u                               = gui_sz_u,
    .sz_row_gap                         = gui_sz_row_gap,
    .sz_rows_h                          = gui_sz_rows_h,
    .sz_child_rows_h                    = gui_sz_child_rows_h,
    .sz_scale_row                       = gui_sz_scale_row,
    .sz_line_h                          = gui_sz_line_h,
    .sz_chars                           = gui_sz_chars,
    .sz_fit_row                         = gui_sz_fit_row,
    .sz_fit_col                         = gui_sz_fit_col,
    .content_avail                      = gui_content_avail,
    .view_avail                         = gui_view_avail,
    .rows_clip                          = gui_rows_clip,
    .rows_clip_end                      = gui_rows_clip_end,
    .cursor_screen_pos                  = gui_cursor_screen_pos,
    .empty                              = gui_empty,
    .flow_begin                         = gui_flow_begin,
    .flow_cell                          = gui_flow_cell,
    .flow_end                           = gui_flow_end,
    .split_begin                        = gui_split_begin,
    .split_next                         = gui_split_next,
    .split_end                          = gui_split_end,

    /*===============================================  GUI_STYLE  ===============================================*/

    .style_get                          = gui_style_get,
    .style_peek                         = gui_style_peek,
    .style_apply                        = gui_style_apply,
    .style_source_set                   = gui_style_source_set,
    .push_style_color                   = gui_push_style_color,
    .pop_style_color                    = gui_pop_style_color,
    .next_style_color                   = gui_next_style_color,
    .push_style_var                     = gui_push_style_var,
    .pop_style_var                      = gui_pop_style_var,
    .next_style_var                     = gui_next_style_var,
    .style_color                        = gui_style_color,
    .scale_push                         = gui_scale_push,
    .scale_pop                          = gui_scale_pop,
    .set_check_style                    = gui_set_check_style,
    .set_bullet_style                   = gui_set_bullet_style,
    .set_arrow_style                    = gui_set_arrow_style,

    /*==============================================  GUI_ELEMENT  ==============================================*/

    .el_style                           = gui_el_style,
    .el_panel                           = gui_el_panel,
    .el_label                           = gui_el_label,
    .el_button                          = gui_el_button,
    .el_check                           = gui_el_check,
    .el_slider                          = gui_el_slider,
    .el_meter                           = gui_el_meter,
    .el_cycle                           = gui_el_cycle,
    .el_input                           = gui_el_input,
    .el_selectable                      = gui_el_selectable,

    /*==============================================  GUI_CHROME  ===============================================*/

    .window_set_next_pos                = gui_window_set_next_pos,
    .window_set_next_size               = gui_window_set_next_size,
    .window_set_next_viewport           = gui_window_set_next_viewport,
    .window_set_next_size_constraints   = gui_window_set_next_size_constraints,
    .window_begin                       = gui_window_begin,
    .window_end                         = gui_window_end,
    .window_set_open                    = gui_window_set_open,
    .window_is_open                     = gui_window_is_open,
    .window_anim_enable                 = gui_window_anim_enable,
    .window_anim_is_enabled             = gui_window_anim_is_enabled,

    /* dock/ -- dock tree, tab groups, layout persistence */

    .dockspace_over_viewport            = gui_dockspace_over_viewport,
    .dock_split                         = gui_dock_split,
    .dock_split_root                    = gui_dock_split_root,
    .dock_window                        = gui_dock_window,
    .dock_undock                        = gui_dock_undock,
    .window_is_docked                   = gui_window_is_docked,
    .dock_window_maximize               = gui_dock_window_maximize,
    .window_is_dock_maximized           = gui_window_is_dock_maximized,
    .window_tab                         = gui_window_tab,
    .dock_save                          = gui_dock_save,
    .dock_load                          = gui_dock_load,
    .dock_clear                         = gui_dock_clear,
    .dockspace_inset                    = gui_dockspace_inset,

    /* popup/ -- popups, tooltips, menus, combo + listbox */

    .popup_open                         = gui_popup_open,
    .popup_begin                        = gui_popup_begin,
    .popup_modal_begin                  = gui_popup_modal_begin,
    .popup_end                          = gui_popup_end,
    .popup_close_current                = gui_popup_close_current,
    .popup_is_open                      = gui_popup_is_open,
    .popup_context_item_begin           = gui_popup_context_item_begin,
    .popup_context_window_begin         = gui_popup_context_window_begin,
    .set_item_tooltip                   = gui_set_item_tooltip,
    .tooltip_begin                      = gui_tooltip_begin,
    .tooltip_end                        = gui_tooltip_end,
    .help_marker                        = gui_help_marker,
    .main_menu_bar_begin                = gui_main_menu_bar_begin,
    .main_menu_bar_end                  = gui_main_menu_bar_end,
    .main_menu_bar_h                    = gui_main_menu_bar_h,
    .menu_bar_begin                     = gui_menu_bar_begin,
    .menu_bar_end                       = gui_menu_bar_end,
    .menu_begin                         = gui_menu_begin,
    .menu_end                           = gui_menu_end,
    .menu_item                          = gui_menu_item,
    .toolbar_begin                      = gui_toolbar_begin,
    .toolbar_end                        = gui_toolbar_end,
    .toolbar_button                     = gui_toolbar_button,
    .toolbar_toggle                     = gui_toolbar_toggle,
    .toolbar_dropdown_begin             = gui_toolbar_dropdown_begin,
    .toolbar_dropdown_end               = gui_toolbar_dropdown_end,
    .toolbar_separator                  = gui_toolbar_separator,

    /* widgets/ -- the stock widget set */

    .text                               = gui_text,
    .textf                              = gui_textf,
    .bullet_text                        = gui_bullet_text,
    .text_colored                       = gui_text_colored,
    .text_disabled                      = gui_text_disabled,
    .text_wrapped                       = gui_text_wrapped,
    .bullet                             = gui_bullet,
    .new_line                           = gui_new_line,
    .label_text                         = gui_label_text,
    .button                             = gui_button,
    .small_button                       = gui_small_button,
    .progress_bar                       = gui_progress_bar,
    .arrow_button                       = gui_arrow_button,
    .checkbox                           = gui_checkbox,
    .radio_button                       = gui_radio_button,
    .slider_float                       = gui_slider_float,
    .slider_float_step                  = gui_slider_float_step,
    .slider_int                         = gui_slider_int,
    .drag_int                           = gui_drag_int,
    .drag_float                         = gui_drag_float,
    .drag_float2                        = gui_drag_float2,
    .drag_float3                        = gui_drag_float3,
    .drag_float4                        = gui_drag_float4,
    .color_edit3                        = gui_color_edit3,
    .color_edit4                        = gui_color_edit4,
    .input_text                         = gui_input_text,
    .input_text_ex                      = gui_input_text_ex,
    .input_text_with_hint               = gui_input_text_with_hint,
    .input_text_multiline               = gui_input_text_multiline,
    .input_int                          = gui_input_int,
    .input_float                        = gui_input_float,
    .input_double                       = gui_input_double,
    .input_float2                       = gui_input_float2,
    .input_float3                       = gui_input_float3,
    .input_float4                       = gui_input_float4,
    .selectable                         = gui_selectable,
    .combo_begin                        = gui_combo_begin,
    .combo_end                          = gui_combo_end,
    .combo                              = gui_combo,
    .listbox_begin                      = gui_listbox_begin,
    .listbox_end                        = gui_listbox_end,
    .listbox                            = gui_listbox,
    .collapsing_header                  = gui_collapsing_header,
    .separator_text                     = gui_separator_text,
    .tree_node                          = gui_tree_node,
    .tree_pop                           = gui_tree_pop,
    .indent                             = gui_indent,
    .unindent                           = gui_unindent,
    .tab_bar_begin                      = gui_tab_bar_begin,
    .tab_bar_end                        = gui_tab_bar_end,
    .tab_item_begin                     = gui_tab_item_begin,
    .tab_item_end                       = gui_tab_item_end,
    .button_width                       = gui_button_width,
    .button_fill                        = gui_button_fill,

    /* table/ -- multi-column rows over the layout engine */

    .table_begin                        = gui_table_begin,
    .table_end                          = gui_table_end,
    .table_setup_column                 = gui_table_setup_column,
    .table_headers_row                  = gui_table_headers_row,
    .table_next_row                     = gui_table_next_row,
    .table_rows_clip                    = gui_table_rows_clip,
    .table_next_column                  = gui_table_next_column,
    .table_set_column_index             = gui_table_set_column_index,
    .table_get_column_count             = gui_table_get_column_count,
    .table_get_column_index             = gui_table_get_column_index,
    .table_get_row_index                = gui_table_get_row_index,
    .table_get_sort_specs               = gui_table_get_sort_specs,
    .table_sort_order                   = gui_table_sort_order,
    .table_set_bg_color                 = gui_table_set_bg_color,
    .window_set_drag                    = gui_window_set_drag,
    .window_set_nav                     = gui_window_set_nav,

    /* theme -- chrome's named style presets (style kit) */

    .theme_list                         = gui_theme_list,
    .theme_set                          = gui_theme_set,
    .theme_get                          = gui_theme_get,
    .theme_reset                        = gui_theme_reset,

    /*===============================================  GUI_DEBUG  ===============================================*/

    .debug_set_layers                   = gui_debug_set_layers,
    .debug_get_layers                   = gui_debug_get_layers,
    .debug_enable                       = gui_debug_enable,
    .debug_is_enabled                   = gui_debug_is_enabled,
    .debug_hotkeys_armed                = gui_debug_hotkeys_armed,
    .debug_set_render_mode              = gui_render_set_mode,
    .debug_get_render_mode              = gui_render_get_mode,
    .debug_dump_geometry                = gui_build_dump_geometry,
    .set_retained_skip                  = gui_build_set_retained_skip,
    .retained_skip                      = gui_build_retained_skip,

};

/*==============================================================================================
    Module lifecycle callbacks
==============================================================================================*/

static bool
gui_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );  
    UNUSED( get_api );

    /* Cache sibling API pointers.  GPU resources are NOT created here; the host
       must call gui()->init() explicitly after rhi()->init(). */

    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static bool
gui_mod_reload( void* state, get_api_fn get_api )
{
    UNUSED( state );
    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static void
gui_mod_exit( void* state )
{
    UNUSED( state );
    gui_shutdown();
}

/*==============================================================================================
    Module descriptor + DLL exports
==============================================================================================*/

static mod_desc_t s_gui_mod_desc = {
    .version       = 1,
    .state_size    = 0,
    .func_api_size = sizeof( gui_api_t ),
    .func_api      = &g_gui_api_struct,
    .dep_count     = 2,
    .deps          = { "rhi", "app" },
    .init          = gui_mod_init,
    .reload        = gui_mod_reload,
    .exit          = gui_mod_exit,
};

mod_desc_t*
gui_get_mod_desc( void )
{
    return &s_gui_mod_desc;
}

// clang-format on
/*============================================================================================*/
