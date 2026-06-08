/*==============================================================================================

    runtime_service/imgui/imgui_input.c -- App input -> imgui IO snapshot.

    input_update() is called once per frame before any widget calls.  It:
        1. Samples mouse position and button state via the app() query API.
        2. Samples per-key down/pressed state for all APP_KEY_COUNT keys.
        3. Drains the app event ring buffer to collect CHAR (printable text) and
           MOUSE_WHEEL (scroll delta) events.  All other event types are processed
           but their data is not retained here -- the host loop should handle them.
    The result is stored in s_io and is read by the widget code in imgui_ctx.c.

    Included by imgui.c after imgui_draw.c so imgui_io_t is in scope from imgui.h.

==============================================================================================*/
// clang-format off

/* Compile-time check: IMGUI_KEY_COUNT must be large enough to index all app keys. */
ORB_STATIC_ASSERT( APP_KEY_COUNT <= IMGUI_KEY_COUNT,
                   "IMGUI_KEY_COUNT too small for APP_KEY_COUNT" );

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static imgui_io_t s_io;

/*----------------------------------------------------------------------------------------------
    input_update -- populate s_io for the current frame.
----------------------------------------------------------------------------------------------*/

static void
input_update( i32 win_w, i32 win_h, f32 dt )
{
    /* Mouse position. */
    {
        i32 mx = 0, my = 0;
        app()->mouse_position( &mx, &my );
        s_io.mouse_x = (f32)mx;
        s_io.mouse_y = (f32)my;
    }

    /* Mouse button snapshot (left=0, right=1, middle=2). */
    {
        const app_mouse_button_t map[ 3 ] = {
            APP_MOUSE_LEFT, APP_MOUSE_RIGHT, APP_MOUSE_MIDDLE
        };
        for ( u32 i = 0; i < 3; ++i )
        {
            s_io.mouse_down     [ i ] = app()->mouse_button_down     ( map[ i ] );
            s_io.mouse_pressed  [ i ] = app()->mouse_button_pressed  ( map[ i ] );
            s_io.mouse_released [ i ] = app()->mouse_button_released ( map[ i ] );
        }
    }

    /* Key state snapshot. */
    for ( i32 k = 0; k < APP_KEY_COUNT; ++k )
    {
        s_io.keys_down    [ k ] = app()->key_down    ( (app_key_t)k );
        s_io.keys_pressed [ k ] = app()->key_pressed ( (app_key_t)k );
    }

    /* Drain the event ring for CHAR (text input) and MOUSE_WHEEL (scroll). */
    s_io.mouse_wheel = 0.0f;
    s_io.text[ 0 ]   = '\0';
    u32 text_len     = 0;

    app_event_t ev;
    while ( app()->next_event( &ev ) )
    {
        if ( ev.type == APP_EV_CHAR )
        {
            /* Encode the UTF-32 codepoint into the text buffer as ASCII (>127 -> '?'). */
            u32 cp = ev.data.text.codepoint;
            if ( text_len + 1 < sizeof( s_io.text ) )
            {
                s_io.text[ text_len++ ] = ( cp < 128u ) ? (char)cp : '?';
                s_io.text[ text_len   ] = '\0';
            }
        }
        else if ( ev.type == APP_EV_MOUSE_WHEEL )
        {
            /* Accumulate scroll; multiple events per frame are possible. */
            s_io.mouse_wheel += (f32)ev.data.mouse_wheel.delta;
        }
        /* All other event types (resize, focus, etc.) are consumed but not retained here.
           Hosts that need those events should call next_event() before imgui_new_frame(). */
    }

    s_io.display_w = win_w;
    s_io.display_h = win_h;
    s_io.dt        = dt;
}

// clang-format on
/*============================================================================================*/
