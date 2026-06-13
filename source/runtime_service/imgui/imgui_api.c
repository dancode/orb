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
    .begin_window  = imgui_begin_window,
    .end_window    = imgui_end_window,
    .begin_child   = imgui_begin_child,
    .end_child     = imgui_end_child,
    .layout        = imgui_layout,
    .layout_default = imgui_layout_default,
    .row           = imgui_row,
    .row_cols      = imgui_row_cols,
    .row2          = imgui_row2,
    .row3          = imgui_row3,
    .row4          = imgui_row4,
    .row_track     = imgui_row_track,
    .field_split   = imgui_field_split,
    .field_label_left  = imgui_field_label_left,
    .field_label_right = imgui_field_label_right,
    .pad           = imgui_pad,
    .grid          = imgui_grid,
    .grid_cells    = imgui_grid_cells,
    .align         = imgui_align,
    .skip          = imgui_skip,
    .spacing       = imgui_spacing,
    .separator     = imgui_separator,
    .line_h        = imgui_line_h,
    .text_w        = imgui_text_w,
    .h_min         = imgui_h_min,
    .w_min         = imgui_w_min,
    .calc_row      = imgui_calc_row,
    .calc_col      = imgui_calc_col,
    .push_id       = imgui_push_id,
    .push_id_int   = imgui_push_id_int,
    .pop_id        = imgui_pop_id,
    .set_window_drag = imgui_set_window_drag,
    .text          = imgui_text,
    .textf         = imgui_textf,
    .button        = imgui_button,
    .checkbox      = imgui_checkbox,
    .slider_float  = imgui_slider_float,
    .input_text    = imgui_input_text,
    .selectable    = imgui_selectable,
    .set_font      = imgui_set_font,
    .set_bmp_scale = imgui_set_bmp_scale,
    .draw_rect     = imgui_draw_rect,
    .draw_text     = imgui_draw_text,
    .push_clip     = imgui_push_clip,
    .pop_clip      = imgui_pop_clip,
    .debug_set_layers = imgui_debug_set_layers,
    .debug_get_layers = imgui_debug_get_layers,
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
