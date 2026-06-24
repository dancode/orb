/*==============================================================================================

    runtime_service/imgui/imgui_api.c -- Module vtable and lifecycle descriptor.

    Included last by imgui.c so all functions from the other constituent
    files are visible here.  The vtable (g_imgui_api) is the live struct that
    the module system hands out through the imgui() accessor.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Vtable struct  (extern const -- referenced by MOD_GATEWAY_STATIC and func_api pointer)
==============================================================================================*/

const imgui_api_t g_imgui_api_struct = 
{
    .init                               = imgui_init,
    .shutdown                           = imgui_shutdown,
    .mem_stats                          = imgui_mem_stats,
    .print_mem_stats                    = imgui_print_mem_stats,
    .render_stats                       = imgui_render_stats,
    .perf_overlay                       = imgui_perf_overlay,
    .load_font                          = imgui_load_font,
    .render                             = imgui_render,
    .viewport_open                      = imgui_viewport_open,
    .viewport_close                     = imgui_viewport_close,
    .viewport_resize                    = imgui_viewport_resize,
    .viewport_spawn                     = imgui_viewport_spawn,
    .update_platform_windows            = imgui_update_platform_windows,
    .render_floaters                    = imgui_render_floaters,
    .ctx_create                         = imgui_ctx_create,
    .ctx_destroy                        = imgui_ctx_destroy,
    .ctx_bind                           = imgui_ctx_bind,
    .ctx_set_listening                  = imgui_ctx_set_listening,
    .ctx_begin                          = imgui_ctx_begin,
    .ctx_end                            = imgui_ctx_end,
    .frame_begin                        = imgui_frame_begin,
    .frame_end                          = imgui_frame_end,
    .event                              = imgui_event,
    .window_set_next_pos                = imgui_window_set_next_pos,
    .window_set_next_viewport           = imgui_window_set_next_viewport,
    .window_set_next_size               = imgui_window_set_next_size,
    .window_set_next_size_constraints   = imgui_window_set_next_size_constraints,
    .window_begin                       = imgui_window_begin,
    .window_end                         = imgui_window_end,
    .window_set_open                    = imgui_window_set_open,
    .window_is_open                     = imgui_window_is_open,
    .dockspace_over_viewport            = imgui_dockspace_over_viewport,
    .dock_split                         = imgui_dock_split,
    .dock_split_root                    = imgui_dock_split_root,
    .dock_window                        = imgui_dock_window,
    .dock_undock                        = imgui_dock_undock,
    .window_is_docked                   = imgui_window_is_docked,
    .dock_save                          = imgui_dock_save,
    .dock_load                          = imgui_dock_load,
    .popup_open                         = imgui_popup_open,
    .popup_begin                        = imgui_popup_begin,
    .popup_modal_begin                  = imgui_popup_modal_begin,
    .popup_end                          = imgui_popup_end,
    .popup_close_current                = imgui_popup_close_current,
    .popup_is_open                      = imgui_popup_is_open,
    .popup_context_item_begin           = imgui_popup_context_item_begin,
    .popup_context_window_begin         = imgui_popup_context_window_begin,
    .set_item_tooltip                   = imgui_set_item_tooltip,
    .tooltip_begin                      = imgui_tooltip_begin,
    .tooltip_end                        = imgui_tooltip_end,
    .help_marker                        = imgui_help_marker,
    .main_menu_bar_begin                = imgui_main_menu_bar_begin,
    .main_menu_bar_end                  = imgui_main_menu_bar_end,
    .menu_bar_begin                     = imgui_menu_bar_begin,
    .menu_bar_end                       = imgui_menu_bar_end,
    .menu_begin                         = imgui_menu_begin,
    .menu_end                           = imgui_menu_end,
    .menu_item                          = imgui_menu_item,
    .child_begin                        = imgui_child_begin,
    .child_end                          = imgui_child_end,
    .push_layout                        = imgui_push_layout,
    .pop_layout                         = imgui_pop_layout,
    .layout                             = imgui_layout,
    .layout_default                     = imgui_layout_default,
    .stack                              = imgui_stack,
    .row                                = imgui_row,
    .columns                            = imgui_columns,
    .cols_n                             = imgui_cols_n,
    .row_cols                           = imgui_row_cols,
    .row2                               = imgui_row2,
    .row3                               = imgui_row3,
    .row4                               = imgui_row4,
    .row_track                          = imgui_row_track,
    .form                               = imgui_form,
    .form_split                         = imgui_form_split,
    .field_split                        = imgui_field_split,
    .field_label_left                   = imgui_field_label_left,
    .field_label_right                  = imgui_field_label_right,
    .pad                                = imgui_pad,
    .grid                               = imgui_grid,
    .grid_cells                         = imgui_grid_cells,
    .pack                               = imgui_pack,
    .bar                                = imgui_bar,
    .strip                              = imgui_strip,
    .pack_size                          = imgui_pack_size,
    .pack_nextline                      = imgui_pack_nextline,
    .align                              = imgui_align,
    .same_line                          = imgui_same_line,
    .stack_sameline                     = imgui_stack_sameline,
    .skip                               = imgui_skip,
    .spacing                            = imgui_spacing,
    .separator                          = imgui_separator,
    .canvas                             = imgui_canvas,
    .line_h                             = imgui_line_h,
    .text_w                             = imgui_text_w,
    .h_min                              = imgui_h_min,
    .w_min                              = imgui_w_min,
    .calc_row                           = imgui_calc_row,
    .calc_col                           = imgui_calc_col,
    .content_avail                      = imgui_content_avail,
    .cursor_screen_pos                  = imgui_cursor_screen_pos,
    .dummy                              = imgui_dummy,
    .push_id                            = imgui_push_id,
    .push_id_int                        = imgui_push_id_int,
    .pop_id                             = imgui_pop_id,
    .push_item_flag                     = imgui_push_item_flag,
    .pop_item_flag                      = imgui_pop_item_flag,
    .next_item_flag                     = imgui_next_item_flag,
    .disabled_begin                     = imgui_disabled_begin,
    .disabled_end                       = imgui_disabled_end,
    .push_style_color                   = imgui_push_style_color,
    .pop_style_color                    = imgui_pop_style_color,
    .next_style_color                   = imgui_next_style_color,
    .push_style_var                     = imgui_push_style_var,
    .pop_style_var                      = imgui_pop_style_var,
    .next_style_var                     = imgui_next_style_var,
    .window_set_drag                    = imgui_window_set_drag,
    .window_set_nav                     = imgui_window_set_nav,
    .text                               = imgui_text,
    .textf                              = imgui_textf,
    .bullet_text                        = imgui_bullet_text,
    .text_colored                       = imgui_text_colored,
    .text_disabled                      = imgui_text_disabled,
    .text_wrapped                       = imgui_text_wrapped,
    .bullet                             = imgui_bullet,
    .new_line                           = imgui_new_line,
    .label_text                         = imgui_label_text,
    .button                             = imgui_button,
    .small_button                       = imgui_small_button,
    .progress_bar                       = imgui_progress_bar,
    .arrow_button                       = imgui_arrow_button,
    .invisible_button                   = imgui_invisible_button,
    .checkbox                           = imgui_checkbox,
    .radio_button                       = imgui_radio_button,
    .slider_float                       = imgui_slider_float,
    .slider_float_step                  = imgui_slider_float_step,
    .slider_int                         = imgui_slider_int,
    .drag_int                           = imgui_drag_int,
    .drag_float                         = imgui_drag_float,
    .drag_float2                        = imgui_drag_float2,
    .drag_float3                        = imgui_drag_float3,
    .drag_float4                        = imgui_drag_float4,
    .input_text                         = imgui_input_text,
    .input_text_ex                      = imgui_input_text_ex,
    .input_text_with_hint               = imgui_input_text_with_hint,
    .input_int                          = imgui_input_int,
    .input_float                        = imgui_input_float,
    .input_double                       = imgui_input_double,
    .input_float2                       = imgui_input_float2,
    .input_float3                       = imgui_input_float3,
    .input_float4                       = imgui_input_float4,
    .selectable                         = imgui_selectable,
    .combo_begin                        = imgui_combo_begin,
    .combo_end                          = imgui_combo_end,
    .combo                              = imgui_combo,
    .listbox_begin                      = imgui_listbox_begin,
    .listbox_end                        = imgui_listbox_end,
    .listbox                            = imgui_listbox,
    .collapsing_header                  = imgui_collapsing_header,
    .separator_text                     = imgui_separator_text,
    .tree_node                          = imgui_tree_node,
    .tree_pop                           = imgui_tree_pop,
    .indent                             = imgui_indent,
    .unindent                           = imgui_unindent,
    .set_font                           = imgui_set_font,
    .set_bmp_scale                      = imgui_set_bmp_scale,
    .set_font_file                      = imgui_set_font_file,
    .use_font                           = imgui_use_font,
    .push_font                          = imgui_push_font,
    .pop_font                           = imgui_pop_font,
    .draw_rect                          = imgui_draw_rect,
    .draw_text                          = imgui_draw_text,
    .text_size                          = imgui_text_size,
    .draw_text_in                       = imgui_draw_text_in,
    .draw_text_clipped                  = imgui_draw_text_clipped,
    .register_icon                      = imgui_register_icon,
    .find_icon                          = imgui_find_icon,
    .icon_size                          = imgui_icon_size,
    .image                              = imgui_image,
    .draw_icon_in                       = imgui_draw_icon_in,
    .render_check_mark                  = imgui_render_check_mark,
    .render_arrow                       = imgui_render_arrow,
    .render_bullet                      = imgui_render_bullet,
    .render_close                       = imgui_render_close,
    .render_arrow_pointing_at           = imgui_render_arrow_pointing_at,
    .render_chevron                     = imgui_render_chevron,
    .render_plus_minus                  = imgui_render_plus_minus,
    .render_frame                       = imgui_render_frame,
    .render_round_rect                  = imgui_render_round_rect,
    .render_ngon                        = imgui_render_ngon,
    .render_circle                      = imgui_render_circle,
    .render_arc                         = imgui_render_arc,
    .render_pie                         = imgui_render_pie,
    .render_bezier_quad                 = imgui_render_bezier_quad,
    .render_bezier_cubic                = imgui_render_bezier_cubic,
    .render_dashed_line                 = imgui_render_dashed_line,
    .render_checker                     = imgui_render_checker,
    .render_hatch                       = imgui_render_hatch,
    .render_gradient                    = imgui_render_gradient,
    .render_shadow                      = imgui_render_shadow,
    .render_text_outline                = imgui_render_text_outline,
    .render_text_shadow                 = imgui_render_text_shadow,
    .render_grip                        = imgui_render_grip,
    .render_spinner                     = imgui_render_spinner,
    .render_progress_arc                = imgui_render_progress_arc,
    .set_check_style                    = imgui_set_check_style,
    .set_bullet_style                   = imgui_set_bullet_style,
    .set_arrow_style                    = imgui_set_arrow_style,
    .draw_line                          = imgui_draw_line,
    .draw_polyline                      = imgui_draw_polyline,
    .path_clear                         = imgui_path_clear,
    .path_line_to                       = imgui_path_line_to,
    .path_stroke                        = imgui_path_stroke,
    .push_clip                          = imgui_push_clip,
    .pop_clip                           = imgui_pop_clip,
    .debug_set_layers                   = imgui_debug_set_layers,
    .debug_get_layers                   = imgui_debug_get_layers,
    .debug_set_render_mode              = imgui_render_set_mode,
    .debug_get_render_mode              = imgui_render_get_mode,
    .want_capture_mouse                 = imgui_want_capture_mouse,
    .want_capture_keyboard              = imgui_want_capture_keyboard,
    .is_mouse_hovering_rect             = imgui_is_mouse_hovering_rect,
    .is_item_hovered                    = imgui_is_item_hovered,
    .is_item_active                     = imgui_is_item_active,
    .is_item_clicked                    = imgui_is_item_clicked,
    .is_item_focused                    = imgui_is_item_focused,
    .is_item_activated                  = imgui_is_item_activated,
    .is_item_deactivated                = imgui_is_item_deactivated,
    .is_item_visible                    = imgui_is_item_visible,
    .get_item_rect                      = imgui_get_item_rect,
    .is_key_down                        = imgui_is_key_down,
    .is_key_pressed                     = imgui_is_key_pressed,
    .is_key_pressed_repeat              = imgui_is_key_pressed_repeat,
    .is_key_released                    = imgui_is_key_released,
    .is_mouse_down                      = imgui_is_mouse_down,
    .is_mouse_clicked                   = imgui_is_mouse_clicked,
    .is_mouse_released                  = imgui_is_mouse_released,
    .is_mouse_double_clicked            = imgui_is_mouse_double_clicked,
    .get_mouse_pos                      = imgui_get_mouse_pos,
    .get_mouse_wheel                    = imgui_get_mouse_wheel,
    .get_delta_time                     = imgui_get_delta_time,
    .get_time                           = imgui_get_time,
    .set_mouse_cursor                   = imgui_set_mouse_cursor,
    .get_mouse_cursor                   = imgui_get_mouse_cursor,
    .wants_redraw                       = imgui_wants_redraw,
    .table_begin                        = imgui_table_begin,
    .table_end                          = imgui_table_end,
    .table_setup_column                 = imgui_table_setup_column,
    .table_headers_row                  = imgui_table_headers_row,
    .table_next_row                     = imgui_table_next_row,
    .table_next_column                  = imgui_table_next_column,
    .table_set_column_index             = imgui_table_set_column_index,
    .table_get_column_count             = imgui_table_get_column_count,
    .table_get_column_index             = imgui_table_get_column_index,
    .table_get_row_index                = imgui_table_get_row_index,
    .table_get_sort_specs               = imgui_table_get_sort_specs,
    .table_sort_order                   = imgui_table_sort_order,
    .table_set_bg_color                 = imgui_table_set_bg_color,
};

/*==============================================================================================
    Module lifecycle callbacks
==============================================================================================*/

static bool
imgui_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );  
    UNUSED( get_api );

    /* Cache sibling API pointers.  GPU resources are NOT created here; the host
       must call imgui()->init() explicitly after rhi()->init(). */

    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static bool
imgui_mod_reload( void* state, get_api_fn get_api )
{
    UNUSED( state );
    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static void
imgui_mod_exit( void* state )
{
    UNUSED( state );
    imgui_shutdown();
}

/*==============================================================================================
    Module descriptor + DLL exports
==============================================================================================*/

static mod_desc_t s_imgui_mod_desc = {
    .version       = 1,
    .state_size    = 0,
    .func_api_size = sizeof( imgui_api_t ),
    .func_api      = &g_imgui_api_struct,
    .dep_count     = 2,
    .deps          = { "rhi", "app" },
    .init          = imgui_mod_init,
    .reload        = imgui_mod_reload,
    .exit          = imgui_mod_exit,
};

mod_desc_t*
imgui_get_mod_desc( void )
{
    return &s_imgui_mod_desc;
}

// clang-format on
/*============================================================================================*/
