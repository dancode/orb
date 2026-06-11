/*==============================================================================================

    runtime_service/imgui/imgui_ctx.c -- Immediate-mode context state.

    Tracks hover/active/focused widget IDs and the layout cursor.
    All widget code in imgui_widget.c reads and writes s_ctx directly.

    Included by imgui.c after imgui_input.c so s_io is in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static struct
{
    imgui_id_t  hover_id;       /* widget under the cursor this frame (rebuilt each frame) */
    imgui_id_t  active_id;    /* widget with the mouse button held (drag / hold)       */
    imgui_id_t  focused_id;   /* widget that owns keyboard input                       */

    /* Window occlusion is resolved one frame deferred: the single window the cursor is
       over (front-most by z) is only known after every window has been submitted.  
       Each begin_window nominates itself into next_hover_win; ctx_new_frame promotes it to
       hover_win.  Next frame a window compares its id against hover_win at entry -- if it
       isn't the hover window it cannot be hit, so it (and all its widgets) skip hit-testing
       entirely.  Only the hover window does widget hit-testing, and within one window
       widgets don't overlap, so widget hover can be resolved immediately (no deferral). */

    imgui_id_t  hover_win;      /* the window the cursor is over (resolved last frame)   */
    imgui_id_t  next_hover_win; /* front-most window nominee gathered this frame         */
    u32         next_hover_win_z;

    imgui_id_t  win_id;       /* id of the window currently between begin/end_window   */

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
    window_nominate_hover -- begin_window calls this with its rect + z.  Keeps the front-most
    (highest z) window the cursor is over; promoted to hover_win next frame.
----------------------------------------------------------------------------------------------*/

static void
window_nominate_hover( imgui_id_t id, imgui_rect_t r, u32 z )
{
    /* Cheap z test gates the rect_hit; window z is unique so > / >= are equivalent. */
    if ( z >= s_ctx.next_hover_win_z && rect_hit( r ) )
    {
        s_ctx.next_hover_win   = id;
        s_ctx.next_hover_win_z = z;
    }
}

/*----------------------------------------------------------------------------------------------
    ctx_new_frame -- reset per-frame hover state; call at the start of each frame
----------------------------------------------------------------------------------------------*/

static void
ctx_new_frame( void )
{
    /* Widget hover is rebuilt every frame by the hover window's widget calls; clear it now. */
    s_ctx.hover_id = IMGUI_ID_NONE;

    /* Promote the window the cursor was over last frame, then start a fresh nomination.
       hover_win lags the cursor by one frame -- the only deferral, and it is what lets the
       front-most window be known before any widget hit-tests this frame. */
    s_ctx.hover_win        = s_ctx.next_hover_win;
    s_ctx.next_hover_win   = IMGUI_ID_NONE;
    s_ctx.next_hover_win_z = 0;

    /* Release active_id once the primary button is up.  Keep it alive on the
       release-edge frame (mouse_released) so widgets can still observe the
       press+release pair this frame; it clears on the following frame when the
       button is neither down nor just released. */
    if ( !s_io.mouse_down[ 0 ] && !s_io.mouse_released[ 0 ] )
        s_ctx.active_id = IMGUI_ID_NONE;

    /* Drop keyboard focus on any press; the widget under the cursor re-claims
       it the same frame (input_text sets focused_id from hover_id + press).
       A press on a button or empty space thus leaves focus cleared. */
    if ( s_io.mouse_pressed[ 0 ] )
        s_ctx.focused_id = IMGUI_ID_NONE;
}

// clang-format on
/*============================================================================================*/
