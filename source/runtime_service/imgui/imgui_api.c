/*==============================================================================================

    runtime_service/imgui/imgui_api.c -- Module vtable and lifecycle descriptor.

    Included last by imgui.c so all static functions from the other constituent
    files are visible here.  The vtable (g_imgui_api) is the live struct that
    the module system hands out through the imgui() accessor.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Public API implementations (thin dispatch to static functions above)
----------------------------------------------------------------------------------------------*/

static bool imgui_init_fn( void )
{
    return render_init();
}

static void imgui_shutdown_fn( void )
{
    render_shutdown();
}

static void imgui_new_frame_fn( i32 win_w, i32 win_h, f32 dt )
{
    input_update( win_w, win_h, dt );
    draw_reset( win_w, win_h );
    ctx_new_frame();
}

static void imgui_render_fn( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    render_flush( cmd, win_w, win_h );
}

static void imgui_begin_window_fn( const char* title, f32 x, f32 y, f32 w, f32 h )
{
    widget_begin_window( title, x, y, w, h );
}

static void imgui_end_window_fn( void )
{
    widget_end_window();
}

static void imgui_text_fn( const char* str )
{
    widget_text( str );
}

static bool imgui_button_fn( const char* label )
{
    return widget_button( label );
}

static bool imgui_checkbox_fn( const char* label, bool* v )
{
    return widget_checkbox( label, v );
}

static bool imgui_slider_float_fn( const char* label, f32* v, f32 lo, f32 hi )
{
    return widget_slider_float( label, v, lo, hi );
}

static bool imgui_input_text_fn( const char* label, char* buf, u32 bufsz )
{
    return widget_input_text( label, buf, bufsz );
}

static void imgui_draw_rect_fn( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    widget_draw_rect( x, y, w, h, abgr );
}

static void imgui_draw_text_fn( f32 x, f32 y, u32 abgr, const char* str )
{
    widget_draw_text( x, y, abgr, str );
}

static void imgui_push_clip_fn( f32 x, f32 y, f32 w, f32 h )
{
    draw_push_clip_rect( x, y, w, h );
}

static void imgui_pop_clip_fn( void )
{
    draw_pop_clip_rect();
}

/*----------------------------------------------------------------------------------------------
    Vtable struct  (extern const -- referenced by MOD_GATEWAY_STATIC and func_api pointer)
----------------------------------------------------------------------------------------------*/

const imgui_api_t g_imgui_api_struct = {
    .init          = imgui_init_fn,
    .shutdown      = imgui_shutdown_fn,
    .new_frame     = imgui_new_frame_fn,
    .render        = imgui_render_fn,
    .begin_window  = imgui_begin_window_fn,
    .end_window    = imgui_end_window_fn,
    .text          = imgui_text_fn,
    .button        = imgui_button_fn,
    .checkbox      = imgui_checkbox_fn,
    .slider_float  = imgui_slider_float_fn,
    .input_text    = imgui_input_text_fn,
    .draw_rect     = imgui_draw_rect_fn,
    .draw_text     = imgui_draw_text_fn,
    .push_clip     = imgui_push_clip_fn,
    .pop_clip      = imgui_pop_clip_fn,
};

/*----------------------------------------------------------------------------------------------
    Direct-call wrappers (declared in imgui_host.h for host / sandbox use)
----------------------------------------------------------------------------------------------*/

bool imgui_init( void )          { return imgui_init_fn(); }
void imgui_shutdown( void )      { imgui_shutdown_fn(); }

void imgui_new_frame( i32 win_w, i32 win_h, f32 dt ) { imgui_new_frame_fn( win_w, win_h, dt ); }
void imgui_render( rhi_cmd_t cmd, i32 win_w, i32 win_h ) { imgui_render_fn( cmd, win_w, win_h ); }

void imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h ) { imgui_begin_window_fn( title, x, y, w, h ); }
void imgui_end_window( void )    { imgui_end_window_fn(); }

void imgui_text( const char* str )           { imgui_text_fn( str ); }
bool imgui_button( const char* label )       { return imgui_button_fn( label ); }
bool imgui_checkbox( const char* label, bool* v )  { return imgui_checkbox_fn( label, v ); }
bool imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi ) { return imgui_slider_float_fn( label, v, lo, hi ); }
bool imgui_input_text( const char* label, char* buf, u32 bufsz )     { return imgui_input_text_fn( label, buf, bufsz ); }

void imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr ) { imgui_draw_rect_fn( x, y, w, h, abgr ); }
void imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str ) { imgui_draw_text_fn( x, y, abgr, str ); }
void imgui_push_clip( f32 x, f32 y, f32 w, f32 h ) { imgui_push_clip_fn( x, y, w, h ); }
void imgui_pop_clip( void )      { imgui_pop_clip_fn(); }

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
    imgui_shutdown_fn();
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

MOD_DEFINE_EXPORTS( imgui )

// clang-format on
/*============================================================================================*/
