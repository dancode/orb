/*==============================================================================================

    runtime_service/imgui/imgui_api.c -- Module vtable and lifecycle descriptor.

    Included last by imgui.c so all functions from the other constituent
    files are visible here.  The vtable (g_imgui_api) is the live struct that
    the module system hands out through the imgui() accessor.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Frame-lifecycle / style implementations (orchestrate across constituent files)
----------------------------------------------------------------------------------------------*/

bool imgui_init( void )
{
    if ( !imgui_render_init() )
        return false;

#ifdef IMGUI_DEBUG_OVERLAY
    /* Debug overlay GPU buffers.  Non-fatal: a failure just leaves the overlay dark. */
    if ( !imgui_debug_init() )
        printf( "[imgui] WARNING: debug overlay buffers failed; overlay disabled\n" );
#endif
    return true;
}

bool imgui_load_font( const char* path )
{
    /* load the font */
    if ( !tt_font_load( path ) )
        return false;

    /* update global font size */
    s_font_size = (u32)s_font->char_h;

    /* update supporting layout dimensions (driven by the font's line height) */
    layout_compute( (u32)s_font->line_h );
    return true;
}

void imgui_shutdown( void )
{
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_shutdown();
#endif
    imgui_render_shutdown();
}

imgui_mem_stats_t imgui_mem_stats( void )
{
    return imgui_render_memory();
}

void imgui_print_mem_stats( void )
{
    imgui_render_print_memory();
}

void imgui_new_frame( i32 win_w, i32 win_h, f32 dt )
{
    input_update( win_w, win_h, dt );
    draw_reset( win_w, win_h );
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_reset();         /* clear the overlay's per-frame geometry */
#endif
    ctx_new_frame();             /* promotes last frame's hover_win */
    popup_close_check();         /* stale-close + click-outside, BEFORE any user open_popup */
    popup_apply_modal();         /* fence interaction behind an open modal (steals hover_win) */
    window_raise_on_press();     /* a press raises the hover window (takes effect this frame) */
}

void imgui_render( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    imgui_render_flush( cmd, win_w, win_h );
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_flush( cmd, win_w, win_h );   /* bolt-on overlay, painted last (on top) */
#endif
}

void imgui_set_font( imgui_font_t font )
{
    tt_font_unload();
    bitmap_font_select( font );
    s_font_size = (u32)s_font->char_h;
    layout_compute( (u32)s_font->line_h );

    const bitmap_font_def_t* def = s_bitmap_active->def;

    printf( "[imgui] set font '%s : %s' (char_h=%.1f line_h=%.1f)\n",
            s_font->proportional ? "TrueType" : "Bitmap", def->debug_name,s_font->char_h, s_font->line_h );
}

void imgui_set_bmp_scale( u32 scale )
{
    bitmap_scale_set( scale );
    if ( !s_tt_font.active )
    {
        s_font_size = (u32)s_font->char_h;
        layout_compute( (u32)s_font->line_h );
    }
}

void imgui_push_clip( f32 x, f32 y, f32 w, f32 h )
{
    draw_push_clip_rect( x, y, w, h );
}

void imgui_pop_clip( void )
{
    draw_pop_clip_rect();
}

/*----------------------------------------------------------------------------------------------
    Vtable struct  (extern const -- referenced by MOD_GATEWAY_STATIC and func_api pointer)
----------------------------------------------------------------------------------------------*/

const imgui_api_t g_imgui_api_struct = {
    .init          = imgui_init,
    .shutdown      = imgui_shutdown,
    .mem_stats       = imgui_mem_stats,
    .print_mem_stats = imgui_print_mem_stats,
    .load_font     = imgui_load_font,
    .new_frame      = imgui_new_frame,
    .render         = imgui_render,
    .event          = imgui_event,
    .set_next_window_pos  = imgui_set_next_window_pos,
    .set_next_window_size = imgui_set_next_window_size,
    .begin_window  = imgui_begin_window,
    .end_window    = imgui_end_window,
    .open_popup          = imgui_open_popup,
    .begin_popup         = imgui_begin_popup,
    .begin_popup_modal   = imgui_begin_popup_modal,
    .end_popup           = imgui_end_popup,
    .close_current_popup = imgui_close_current_popup,
    .is_popup_open        = imgui_is_popup_open,
    .begin_popup_context_item   = imgui_begin_popup_context_item,
    .begin_popup_context_window = imgui_begin_popup_context_window,
    .set_item_tooltip = imgui_set_item_tooltip,
    .begin_tooltip    = imgui_begin_tooltip,
    .end_tooltip      = imgui_end_tooltip,
    .help_marker      = imgui_help_marker,
    .begin_child   = imgui_begin_child,
    .end_child     = imgui_end_child,
    .push_layout   = imgui_push_layout,
    .pop_layout    = imgui_pop_layout,
    .layout        = imgui_layout,
    .layout_default = imgui_layout_default,
    .stack         = imgui_stack,
    .row           = imgui_row,
    .columns       = imgui_columns,
    .cols_n        = imgui_cols_n,
    .row_cols      = imgui_row_cols,
    .row2          = imgui_row2,
    .row3          = imgui_row3,
    .row4          = imgui_row4,
    .row_track     = imgui_row_track,
    .form          = imgui_form,
    .form_split    = imgui_form_split,
    .field_split   = imgui_field_split,
    .field_label_left  = imgui_field_label_left,
    .field_label_right = imgui_field_label_right,
    .pad           = imgui_pad,
    .grid          = imgui_grid,
    .grid_cells    = imgui_grid_cells,
    .pack          = imgui_pack,
    .bar           = imgui_bar,
    .strip         = imgui_strip,
    .pack_size     = imgui_pack_size,
    .pack_nextline = imgui_pack_nextline,
    .align         = imgui_align,
    .same_line     = imgui_same_line,
    .stack_sameline = imgui_stack_sameline,
    .skip          = imgui_skip,
    .spacing       = imgui_spacing,
    .separator     = imgui_separator,
    .line_h        = imgui_line_h,
    .text_w        = imgui_text_w,
    .h_min         = imgui_h_min,
    .w_min         = imgui_w_min,
    .calc_row      = imgui_calc_row,
    .calc_col      = imgui_calc_col,
    .content_avail = imgui_content_avail,
    .push_id       = imgui_push_id,
    .push_id_int   = imgui_push_id_int,
    .pop_id        = imgui_pop_id,
    .push_item_flag = imgui_push_item_flag,
    .pop_item_flag  = imgui_pop_item_flag,
    .next_item_flag = imgui_next_item_flag,
    .push_style_color = imgui_push_style_color,
    .pop_style_color  = imgui_pop_style_color,
    .next_style_color = imgui_next_style_color,
    .push_style_var   = imgui_push_style_var,
    .pop_style_var    = imgui_pop_style_var,
    .next_style_var   = imgui_next_style_var,
    .set_window_drag = imgui_set_window_drag,
    .text          = imgui_text,
    .textf         = imgui_textf,
    .bullet_text   = imgui_bullet_text,
    .label_text    = imgui_label_text,
    .button        = imgui_button,
    .arrow_button  = imgui_arrow_button,
    .checkbox      = imgui_checkbox,
    .radio_button  = imgui_radio_button,
    .slider_float  = imgui_slider_float,
    .input_text    = imgui_input_text,
    .selectable    = imgui_selectable,
    .collapsing_header = imgui_collapsing_header,
    .separator_text    = imgui_separator_text,
    .tree_node     = imgui_tree_node,
    .tree_pop      = imgui_tree_pop,
    .indent        = imgui_indent,
    .unindent      = imgui_unindent,
    .set_font      = imgui_set_font,
    .set_bmp_scale = imgui_set_bmp_scale,
    .draw_rect     = imgui_draw_rect,
    .draw_text     = imgui_draw_text,
    .push_clip     = imgui_push_clip,
    .pop_clip      = imgui_pop_clip,
    .debug_set_layers = imgui_debug_set_layers,
    .debug_get_layers = imgui_debug_get_layers,
    .want_capture_mouse      = imgui_want_capture_mouse,
    .want_capture_keyboard   = imgui_want_capture_keyboard,
    .is_key_down             = imgui_is_key_down,
    .is_key_pressed          = imgui_is_key_pressed,
    .is_key_released         = imgui_is_key_released,
    .is_mouse_down           = imgui_is_mouse_down,
    .is_mouse_clicked        = imgui_is_mouse_clicked,
    .is_mouse_released       = imgui_is_mouse_released,
    .is_mouse_double_clicked = imgui_is_mouse_double_clicked,
    .get_mouse_pos           = imgui_get_mouse_pos,
    .get_mouse_wheel         = imgui_get_mouse_wheel,
    .get_delta_time          = imgui_get_delta_time,
    .get_time                = imgui_get_time,
};

/*----------------------------------------------------------------------------------------------
    Module lifecycle callbacks
----------------------------------------------------------------------------------------------*/

static bool
imgui_mod_init( void* state, get_api_fn get_api )
{
    (void)state;
    /* Cache sibling API pointers.  GPU resources are NOT created here; the host
       must call imgui()->init() explicitly after rhi()->init(). */

    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static bool
imgui_mod_reload( void* state, get_api_fn get_api )
{
    (void)state;
    if ( !MOD_FETCH_RHI ) return false;
    if ( !MOD_FETCH_APP ) return false;
    return true;
}

static void
imgui_mod_exit( void* state )
{
    (void)state;
    imgui_shutdown();
}

/*----------------------------------------------------------------------------------------------
    Module descriptor + DLL exports
----------------------------------------------------------------------------------------------*/

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
