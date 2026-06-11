/*==============================================================================================

    runtime_service/imgui/imgui_ctx.c -- Immediate-mode context state.

    Tracks hot/active/focused widget IDs and the layout cursor.
    All widget code in imgui_widget.c reads and writes s_ctx directly.

    Included by imgui.c after imgui_input.c so s_io is in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static struct
{
    imgui_id_t  hot_id;       /* widget the cursor is hovering over this frame         */
    imgui_id_t  active_id;    /* widget with the mouse button held (drag / hold)       */
    imgui_id_t  focused_id;   /* widget that owns keyboard input                       */

    f32  cursor_x;            /* layout pen, top-left of the next widget               */
    f32  cursor_y;
    f32  win_x, win_y;        /* current window top-left (outer frame)                 */
    f32  win_w, win_h;        /* current window dimensions                             */
    f32  content_x;           /* inner content area left edge (win_x + padding)        */
    f32  content_w;           /* inner content area width                              */

} s_ctx;

/*----------------------------------------------------------------------------------------------
    id_hash -- FNV-1a 32-bit hash of a NUL-terminated string
----------------------------------------------------------------------------------------------*/

static imgui_id_t
id_hash( const char* str )
{
    u32 h = 0x811C9DC5u;
    for ( ; *str; ++str )
        h = ( h ^ (u8)*str ) * 0x01000193u;
    return h ? h : 1u;    /* never return IMGUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
----------------------------------------------------------------------------------------------*/

static bool
rect_hit( imgui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/*----------------------------------------------------------------------------------------------
    ctx_new_frame -- reset per-frame hot ID; call at the start of each frame
----------------------------------------------------------------------------------------------*/

static void
ctx_new_frame( void )
{
    /* hot_id is re-established every frame by widget calls; clear it now. */
    s_ctx.hot_id = IMGUI_ID_NONE;

    /* Release active_id once the primary button is up.  Keep it alive on the
       release-edge frame (mouse_released) so widgets can still observe the
       press+release pair this frame; it clears on the following frame when the
       button is neither down nor just released. */
    if ( !s_io.mouse_down[ 0 ] && !s_io.mouse_released[ 0 ] )
        s_ctx.active_id = IMGUI_ID_NONE;

    /* Drop keyboard focus on any press; the widget under the cursor re-claims
       it the same frame (input_text sets focused_id from hot_id + press).
       A press on a button or empty space thus leaves focus cleared. */
    if ( s_io.mouse_pressed[ 0 ] )
        s_ctx.focused_id = IMGUI_ID_NONE;
}

// clang-format on
/*============================================================================================*/
