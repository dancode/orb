/*==============================================================================================

    runtime_service/imgui/imgui_input.c -- App input -> imgui IO snapshot.

    input_update() is called once per frame before any widget calls.  It:
        1. Samples mouse position and button state via the app() query API.
        2. Samples per-key down/pressed state for all APP_KEY_COUNT keys.
        3. Consumes text + scroll that the host fed in since the last frame via
           imgui_add_input_char() / imgui_add_mouse_wheel().
    The app event ring is single-consumer; the host drains it (for resize etc.) and
    forwards CHAR / MOUSE_WHEEL events to the feeders above, so imgui does not drain
    the ring itself.  The result is stored in s_io and read by the widget code.

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

/* Pending text + scroll fed by the host between frames via imgui_add_input_char /
   imgui_add_mouse_wheel.  The app event ring is single-consumer, and the host must
   drain it itself (for resize etc.), so imgui can no longer drain it here -- the host
   forwards the relevant events to us.  input_update() moves this pending state into
   s_io and clears it at the top of each frame. */
static char s_pending_text[ sizeof( ( (imgui_io_t*)0 )->text ) ];
static u32  s_pending_text_len;
static f32  s_pending_wheel;

/*----------------------------------------------------------------------------------------------
    Host input feeders -- called by the host while it drains the app event ring,
    before imgui_new_frame() for the same frame.
----------------------------------------------------------------------------------------------*/

void
imgui_add_input_char( u32 codepoint )
{
    /* ASCII only: codepoints >127 collapse to '?'. */
    if ( s_pending_text_len + 1u < sizeof( s_pending_text ) )
    {
        s_pending_text[ s_pending_text_len++ ] = ( codepoint < 128u ) ? (char)codepoint : '?';
        s_pending_text[ s_pending_text_len   ] = '\0';
    }
}

void
imgui_add_mouse_wheel( f32 delta )
{
    s_pending_wheel += delta;
}

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

    /* Text + scroll arrive via the host-fed pending state (the host owns the event
       ring drain).  Move it into the frame snapshot, then clear it for next frame. */
    memcpy( s_io.text, s_pending_text, (size_t)s_pending_text_len + 1u );
    s_io.mouse_wheel = s_pending_wheel;

    s_pending_text[ 0 ] = '\0';
    s_pending_text_len  = 0;
    s_pending_wheel     = 0.0f;

    s_io.display_w = win_w;
    s_io.display_h = win_h;
    s_io.dt        = dt;
}

// clang-format on
/*============================================================================================*/
