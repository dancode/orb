/*==============================================================================================

    runtime_service/imgui/imgui_window.c -- Persistent per-window state.

    begin_window()'s x/y/w/h arguments are the window's *initial* geometry, used only
    the first frame a given title is seen.  From then on this registry owns the window's
    position so it survives across frames -- the foundation for dragging (now) and for
    collapse / scroll / saved-layout state (later).  Windows are keyed by id_hash(title).

    Drag interaction itself lives in imgui_widget.c alongside begin_window / end_window,
    where the layout dimensions (title-bar height, padding) are in scope.  This file owns
    only the table, the active drag mode, and the in-flight drag offset.

    Included by imgui.c after imgui_ctx.c (no dependency on s_ctx / s_io).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

#define IMGUI_MAX_WINDOWS 64

/* One persisted window.  Geometry is owned here after the first appearance. */
typedef struct
{
    imgui_id_t id;          /* id_hash(title); 0 = free slot                 */
    f32        x, y;        /* persisted top-left (updated by dragging)       */
    f32        w, h;        /* persisted dimensions                           */
    u32        z;           /* paint order: higher = more recently raised = in front */

} imgui_window_t;

static imgui_window_t s_windows[ IMGUI_MAX_WINDOWS ];
static u32            s_window_count;
static imgui_window_t s_window_scratch;   /* fallback when the table is full (not persisted) */

/* Monotonic z dispenser.  Each new or raised window takes the next value, so the
   most recently raised window always has the highest z and sorts to the front. */
static u32 s_z_counter;

/* Front-most window pressed this frame (highest z among those hit).  Resolved across
   the frame and applied by window_apply_raise() so a click brings that window forward. */
static imgui_id_t s_raise_id;
static u32        s_raise_z;

/* Drag configuration + in-flight drag offset (mouse - window pos at grab time).
   The window currently being dragged is tracked via s_ctx.active_id == window id. */
static imgui_win_drag_t s_win_drag_mode = IMGUI_WIN_DRAG_TITLEBAR;
static f32              s_drag_off_x, s_drag_off_y;

/*----------------------------------------------------------------------------------------------
    window_get -- find the window for this id, or create it from the initial geometry.
    Never returns NULL; an overflowing table falls back to a transient scratch entry.
----------------------------------------------------------------------------------------------*/

static imgui_window_t*
window_get( imgui_id_t id, f32 x, f32 y, f32 w, f32 h )
{
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == id )
            return &s_windows[ i ];

    /* First time seen: seed from the caller's initial geometry, place on top. */
    imgui_window_t* win = ( s_window_count < IMGUI_MAX_WINDOWS )
                        ? &s_windows[ s_window_count++ ]
                        : &s_window_scratch;   /* table full: transient, not persisted */
    win->id = id;
    win->x  = x;
    win->y  = y;
    win->w  = w;
    win->h  = h;
    win->z  = ++s_z_counter;
    return win;
}

/*----------------------------------------------------------------------------------------------
    Raise-to-front -- a press brings the front-most window under the cursor to the top.
----------------------------------------------------------------------------------------------*/

/* Reset the per-frame raise candidate; called from imgui_new_frame(). */
static void
window_new_frame( void )
{
    s_raise_id = IMGUI_ID_NONE;
    s_raise_z  = 0;
}

/* Record a press on win this frame, keeping the front-most (highest z) candidate.
   Called from begin_window once the window's resolved rect is known. */
static void
window_note_press( const imgui_window_t* win )
{
    if ( !s_io.mouse_pressed[ 0 ] )
        return;

    imgui_rect_t r = { win->x, win->y, win->w, win->h };
    if ( rect_hit( r ) && ( s_raise_id == IMGUI_ID_NONE || win->z > s_raise_z ) )
    {
        s_raise_id = win->id;
        s_raise_z  = win->z;
    }
}

/* Bring the pressed front-most window to the top.  Called from imgui_render() after
   all windows are recorded; the new z is consumed next frame (one-frame latency),
   since this frame's commands were already stamped with the old z. */
static void
window_apply_raise( void )
{
    if ( s_raise_id == IMGUI_ID_NONE )
        return;

    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == s_raise_id )
        {
            /* Already on top?  Don't burn a z value every frame the button is held. */
            if ( s_windows[ i ].z != s_z_counter )
                s_windows[ i ].z = ++s_z_counter;
            break;
        }
}

/*----------------------------------------------------------------------------------------------
    set_window_drag -- select the global drag mode; call between frames.
----------------------------------------------------------------------------------------------*/

void
imgui_set_window_drag( imgui_win_drag_t mode )
{
    s_win_drag_mode = mode;
}

// clang-format on
/*============================================================================================*/
