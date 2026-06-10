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
    return imgui_render_init();
}

bool imgui_load_font( const char* path )
{
    /* load the font */
    if ( !tt_font_load( path ) )
        return false;

    /* update global font size */
    s_font_size = (u32)s_font->char_h;

    /* update supporting layout dimensions */
    // layout_compute( s_font_size );
    layout_compute( (u32)s_font->line_h );
    return true;
}

void imgui_shutdown( void )
{
    imgui_render_shutdown();
}

void imgui_new_frame( i32 win_w, i32 win_h, f32 dt )
{
    input_update( win_w, win_h, dt );
    draw_reset( win_w, win_h );
    ctx_new_frame();
}

void imgui_render( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    imgui_render_flush( cmd, win_w, win_h );
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
    .load_font     = imgui_load_font,
    .new_frame     = imgui_new_frame,
    .render        = imgui_render,
    .begin_window  = imgui_begin_window,
    .end_window    = imgui_end_window,
    .text          = imgui_text,
    .button        = imgui_button,
    .checkbox      = imgui_checkbox,
    .slider_float  = imgui_slider_float,
    .input_text    = imgui_input_text,
    .set_font      = imgui_set_font,
    .set_bmp_scale = imgui_set_bmp_scale,
    .draw_rect     = imgui_draw_rect,
    .draw_text     = imgui_draw_text,
    .push_clip     = imgui_push_clip,
    .pop_clip      = imgui_pop_clip,
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
